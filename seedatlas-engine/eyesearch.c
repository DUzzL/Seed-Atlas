/* Family scan for End portals with many eyes. See eyesearch.h for the
 * structure (stages a, b, c) and the data that stage (b) consumes, and
 * docs/portal-eyes.md for the underlying rules. */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L // clock_gettime() with -std=c11
#endif
#include "eyesearch.h"
#include "generator.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define ES_PI 3.14159265358979323846 // the same constant as finders.c

enum
{
    NSEEDS      = 0x10000,  // world seeds per family
    JUMP_POW    = 20,       // power-of-two jump tables M^(2^k), k < JUMP_POW
    SLOTS       = 8,        // cache of eye tables for other skips
    SLOW_MAX    = 192,      // fewer survivors: test them one by one instead
                            // of building an eye table
    BATCH       = 1024,     // candidate plans per batch
};

#define XO_XL 0x9e3779b97f4a7c15ULL
#define XO_XH 0x6a09e667f3bcc909ULL
#define EYE_T ((uint64_t) PORTAL_EYE_THRESHOLD)

// Rough costs for choosing between filtering and direct confirmation; they
// only affect speed, never results.
#define COST_TEST   4e-9    // one eye test of one seed
#define COST_LAYOUT 20e-6   // one candidate layout + plan
#define COST_BIOME  2.2e-3  // one stronghold biome search (nextStronghold)

typedef struct
{
    uint64_t v[32][16][2];  // nibble table of a 128x128 GF(2) matrix
} NibTable;

typedef struct
{
    uint32_t n;             // skip of the cached table, 0 = empty
    uint32_t age;
    uint64_t col[128][2];   // columns of M^n
    EyeTable t;
} EyeSlot;

typedef struct
{
    int64_t mx, mz;         // 16 * chunkX, 16 * chunkZ of the frame chunk
    uint32_t n;             // RNG outputs before the tested draw
} EyeTest;

typedef struct
{
    int group;
    int ntests;             // counted frames (tests), >= minEyes
    int k;                  // allowed misses: ntests - minEyes
    EyeTest test[12];
} KPlan;

enum { G_WINDOW, G_STRIP, G_ANNULUS };

typedef struct
{
    int type;
    int i0, i1;             // stronghold indices covered (inclusive)
    int x0, z0, x1, z1;     // bounding box of the candidate start chunks
    double ux, uz;          // strip: direction
    double dmin, dmax;      // strip: distance range; annulus: radii
    int direct;             // confirm every seed instead of filtering
    uint64_t mask[2];       // bits i0..i1
} Group;

struct EyeSearch
{
    EyeSearchConfig cfg;
    int newIter;            // 1.19.3+: approximate positions are biome independent
    uint64_t salt;          // structIndex + 10000 * step
    int mode;               // EYESEARCH_AUTO / _FILTER / _DIRECT
    Generator g;
    Stronghold *sh;         // work buffer

    NibTable *pow;          // [JUMP_POW]: M^(2^k)
    EyeTable *exactT;       // [12]: M^(760 + d)
    EyeSlot *slot;          // [SLOTS]
    uint32_t clock;

    uint64_t low48;
    int h0, h1;
    uint64_t *decoA, *decoB;    // [NSEEDS] decoration multipliers per seed
    uint32_t *bufA, *bufB;      // [NSEEDS] survivor lists: high | misses << 16
    uint32_t *bufS;             // [NSEEDS] result of a shared first test
    uint64_t *mark;             // [NSEEDS][2]: stronghold indices to confirm
    KPlan *plans;               // [BATCH]
    EyeTest *keys;              // [BATCH * 12]
    void *order;                // [BATCH] PlanOrder
    Group groups[128];
    int ngroups;
    int sgi, sx, sz;            // split scan: position of the next plans

    EyeSearchStats st;
};


//==============================================================================
// GF(2) jumps of Xoroshiro128++
//==============================================================================

static void nibFromCols(NibTable *t, const uint64_t col[128][2])
{
    for (int k = 0; k < 32; k++)
    {
        t->v[k][0][0] = t->v[k][0][1] = 0;
        for (int v = 1; v < 16; v++)
        {
            int b = __builtin_ctz(v);
            t->v[k][v][0] = t->v[k][v & (v - 1)][0] ^ col[4 * k + b][0];
            t->v[k][v][1] = t->v[k][v & (v - 1)][1] ^ col[4 * k + b][1];
        }
    }
}

static inline void nibApply(const NibTable *t, uint64_t *plo, uint64_t *phi)
{
    uint64_t lo = 0, hi = 0, s = *plo;
    for (int k = 0; k < 16; k++, s >>= 4)
    {
        lo ^= t->v[k][s & 15][0];
        hi ^= t->v[k][s & 15][1];
    }
    s = *phi;
    for (int k = 16; k < 32; k++, s >>= 4)
    {
        lo ^= t->v[k][s & 15][0];
        hi ^= t->v[k][s & 15][1];
    }
    *plo = lo;
    *phi = hi;
}

/* Advance a Xoroshiro128++ state by n outputs. */
static void jumpN(const EyeSearch *es, uint64_t *lo, uint64_t *hi, uint64_t n)
{
    for (int k = 0; n; k++, n >>= 1)
    {
        if (k == JUMP_POW - 1)
        {   // 2^(JUMP_POW-1) * n, far beyond any real skip
            for (; n; n--)
                nibApply(&es->pow[k], lo, hi);
            return;
        }
        if (n & 1)
            nibApply(&es->pow[k], lo, hi);
    }
}

/* Packs the state bits that the eye test needs (see eyeBase) into 63 bits:
 * lo[63:24] at bits 62..23, hi[46:24] at bits 22..0. Linear over GF(2). */
static inline uint64_t eyePack(uint64_t lo, uint64_t hi)
{
    return ((lo >> 24) << 23) | ((hi >> 24) & 0x7fffff);
}

static void eyeTableFromCols(EyeTable *t, const uint64_t col[128][2])
{
    for (int k = 0; k < 16; k++)
    {
        uint64_t pc[8];
        for (int b = 0; b < 8; b++)
            pc[b] = eyePack(col[8 * k + b][0], col[8 * k + b][1]);
        t->v[k][0] = 0;
        for (int v = 1; v < 256; v++)
            t->v[k][v] = t->v[k][v & (v - 1)] ^ pc[__builtin_ctz(v)];
    }
}

static void colsOfJump(const EyeSearch *es, uint64_t col[128][2], uint32_t n)
{
    for (int j = 0; j < 128; j++)
    {
        uint64_t lo = j < 64 ? 1ULL << j : 0;
        uint64_t hi = j < 64 ? 0 : 1ULL << (j - 64);
        jumpN(es, &lo, &hi, n);
        col[j][0] = lo;
        col[j][1] = hi;
    }
}

/* One linear Xoroshiro step (the state update without the output). */
static inline void xStepLinear(uint64_t *lo, uint64_t *hi)
{
    Xoroshiro xr = { *lo, *hi };
    xNextLong(&xr);
    *lo = xr.lo;
    *hi = xr.hi;
}

static void initJumps(EyeSearch *es)
{
    uint64_t col[128][2];
    for (int j = 0; j < 128; j++)
    {
        col[j][0] = j < 64 ? 1ULL << j : 0;
        col[j][1] = j < 64 ? 0 : 1ULL << (j - 64);
        xStepLinear(&col[j][0], &col[j][1]);
    }
    nibFromCols(&es->pow[0], (const uint64_t (*)[2]) col);
    for (int k = 1; k < JUMP_POW; k++)
    {
        for (int j = 0; j < 128; j++)
            nibApply(&es->pow[k - 1], &col[j][0], &col[j][1]);
        nibFromCols(&es->pow[k], (const uint64_t (*)[2]) col);
    }
    colsOfJump(es, col, PORTAL_ROOM_CALLS_BEFORE_EYES);
    for (int d = 0; d < 12; d++)
    {
        if (d)
        {
            for (int j = 0; j < 128; j++)
                xStepLinear(&col[j][0], &col[j][1]);
        }
        eyeTableFromCols(&es->exactT[d], (const uint64_t (*)[2]) col);
    }
}

/* The eye table for a skip of n outputs, or NULL if building one is not
 * worth it for 'count' seeds. */
static const EyeTable *eyeTableFor(EyeSearch *es, uint32_t n, int count)
{
    if (n >= PORTAL_ROOM_CALLS_BEFORE_EYES && n < PORTAL_ROOM_CALLS_BEFORE_EYES + 12)
        return &es->exactT[n - PORTAL_ROOM_CALLS_BEFORE_EYES];
    es->clock++;
    int lru = 0, nearest = -1;
    for (int i = 0; i < SLOTS; i++)
    {
        EyeSlot *s = &es->slot[i];
        if (s->n == n)
        {
            s->age = es->clock;
            return &s->t;
        }
        if (s->n && s->n < n && n - s->n <= 12 &&
            (nearest < 0 || s->n > es->slot[nearest].n))
            nearest = i;
        if (s->age < es->slot[lru].age)
            lru = i;
    }
    if (count < SLOW_MAX)
        return NULL;

    EyeSlot *s = &es->slot[lru];
    if (nearest >= 0)
    {   // a few steps on from a cached table (the next draw of a chunk)
        if (nearest != lru)
            memcpy(s->col, es->slot[nearest].col, sizeof(s->col));
        for (uint32_t d = es->slot[nearest].n; d < n; d++)
            for (int j = 0; j < 128; j++)
                xStepLinear(&s->col[j][0], &s->col[j][1]);
    }
    else
    {
        colsOfJump(es, s->col, n);
    }
    eyeTableFromCols(&s->t, (const uint64_t (*)[2]) s->col);
    s->n = n;
    s->age = es->clock;
    es->st.tables++;
    return &s->t;
}


//==============================================================================
// Stage (b): bulk eye tests
//==============================================================================

static inline uint64_t mix64s(uint64_t v)
{
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}

#define EB(q, k) (((q) >> (8 * (k))) & 255)

/* For the Xoroshiro seed 'v' (setFeatureSeed value), returns an estimate
 * 'base' of the top 24 bits of the output at the table's position. The true
 * value is (base + delta) mod 2^24 with 0 <= delta <= 4, where delta stands
 * for the three carries that the packed bits cannot see:
 *   out = rotl(lo + hi, 17) + lo,  out[63:40] = s[46:23] + lo[63:40] + c40,
 *   s[46:23] = 2 * (lo[46:24] + hi[46:24] + c24) + s[23]   (mod 2^24),
 * so delta = 2*c24 + s[23] + c40. */
static inline uint64_t eyeBaseLH(const EyeTable *t, uint64_t l, uint64_t h)
{
    uint64_t a0 = t->v[0][EB(l,0)] ^ t->v[1][EB(l,1)];
    uint64_t a1 = t->v[2][EB(l,2)] ^ t->v[3][EB(l,3)];
    uint64_t a2 = t->v[4][EB(l,4)] ^ t->v[5][EB(l,5)];
    uint64_t a3 = t->v[6][EB(l,6)] ^ t->v[7][l >> 56];
    uint64_t b0 = t->v[8][EB(h,0)] ^ t->v[9][EB(h,1)];
    uint64_t b1 = t->v[10][EB(h,2)] ^ t->v[11][EB(h,3)];
    uint64_t b2 = t->v[12][EB(h,4)] ^ t->v[13][EB(h,5)];
    uint64_t b3 = t->v[14][EB(h,6)] ^ t->v[15][h >> 56];
    uint64_t p = ((a0 ^ a1) ^ (a2 ^ a3)) ^ ((b0 ^ b1) ^ (b2 ^ b3));
    uint64_t lo40 = p >> 23;
    uint64_t x = (lo40 + (p & 0x7fffff)) & 0x7fffff;
    return (2 * x + (lo40 >> 16)) & 0xffffff;
}

static inline uint64_t eyeBase(const EyeTable *t, uint64_t v)
{
    uint64_t l = v ^ XO_XH;
    return eyeBaseLH(t, mix64s(l), mix64s(l + XO_XL));
}

// base <= EYE_T - 4: no eye for any delta.
#define EYE_FAIL(base)  ((base) < EYE_T - 3)
// EYE_T < base and base + 4 < 2^24: an eye for any delta.
#define EYE_PASS(base)  ((base) - (EYE_T + 1) <= 0xffffffULL - 4 - (EYE_T + 1))

/* Exact test of one draw: the slow path for ambiguous estimates and for
 * small survivor lists. */
__attribute__((noinline)) static int eyeExact(const EyeSearch *es, uint64_t v, uint32_t n)
{
    Xoroshiro xr;
    xSetSeed(&xr, v);
    jumpN(es, &xr.lo, &xr.hi, n);
    return (xNextLong(&xr) >> 40) > EYE_T;
}

static inline uint64_t testSeed(const EyeSearch *es, const EyeTest *t, uint32_t h)
{
    uint64_t seed = ((uint64_t) h << 48) | es->low48;
    return (((uint64_t) t->mx * es->decoA[h] + (uint64_t) t->mz * es->decoB[h])
        ^ seed) + es->salt;
}

enum { SWEEP_BLOCK = 128 };

/* One test over a list of entries (high | misses << 16), keeping those with
 * at most k misses. ranged: the entries are all seeds h0 .. h0+nin-1 with no
 * misses yet (in is unused). This is the hot loop. It runs in blocks of two
 * passes, xSetSeed for the block, then the table lookups, which keeps the
 * register pressure of each pass low (about 20% faster than one pass). */
__attribute__((always_inline)) static inline int sweepCore(const EyeSearch *es, const EyeTable *tab,
    const EyeTest *t, int k, const uint32_t *in, int nin, uint32_t *out,
    const int ranged)
{
    const uint64_t *A = es->decoA, *B = es->decoB;
    const uint64_t mx = (uint64_t) t->mx, mz = (uint64_t) t->mz;
    const uint64_t low = es->low48, salt = es->salt;
    const uint32_t h0 = (uint32_t) es->h0, tn = t->n;
    uint64_t L[SWEEP_BLOCK], H[SWEEP_BLOCK];
    int n = 0;
    for (int b0 = 0; b0 < nin; b0 += SWEEP_BLOCK)
    {
        int cnt = nin - b0 < SWEEP_BLOCK ? nin - b0 : SWEEP_BLOCK;
        for (int i = 0; i < cnt; i++)
        {
            uint32_t h = ranged ? h0 + (uint32_t)(b0 + i) : (in[b0 + i] & 0xffff);
            uint64_t v = ((mx * A[h] + mz * B[h]) ^ (((uint64_t) h << 48) | low)) + salt;
            uint64_t l = v ^ XO_XH;
            L[i] = mix64s(l);
            H[i] = mix64s(l + XO_XL);
        }
        for (int i = 0; i < cnt; i++)
        {
            uint64_t base = eyeBaseLH(tab, L[i], H[i]);
            uint32_t e = ranged ? h0 + (uint32_t)(b0 + i) : in[b0 + i];
            uint32_t miss = EYE_FAIL(base);
            if (__builtin_expect(!miss && !EYE_PASS(base), 0))
            {
                uint32_t h = e & 0xffff;
                uint64_t v = ((mx * A[h] + mz * B[h]) ^ (((uint64_t) h << 48) | low)) + salt;
                miss = !eyeExact(es, v, tn);
            }
            if (ranged)
            {   // no earlier misses
                out[n] = e | miss << 16;
                n += (int) miss <= k;
            }
            else
            {
                e += miss << 16;
                out[n] = e;
                n += (int)(e >> 16) <= k;
            }
        }
    }
    return n;
}

__attribute__((noinline)) static int sweepRange(const EyeSearch *es, const EyeTable *tab,
    const EyeTest *t, int k, int nin, uint32_t *out)
{
    return sweepCore(es, tab, t, k, NULL, nin, out, 1);
}

__attribute__((noinline)) static int sweepList(const EyeSearch *es, const EyeTable *tab,
    const EyeTest *t, int k, const uint32_t *in, int nin, uint32_t *out)
{
    return sweepCore(es, tab, t, k, in, nin, out, 0);
}

/* Exact one-by-one variant for short lists (no table). */
static int sweepSlow(const EyeSearch *es, const EyeTest *t, int k,
    const uint32_t *in, int nin, uint32_t *out)
{
    int n = 0;
    for (int i = 0; i < nin; i++)
    {
        uint32_t e = in ? in[i] : (uint32_t)(es->h0 + i);
        uint32_t miss = !eyeExact(es, testSeed(es, t, e & 0xffff), t->n);
        e += miss << 16;
        out[n] = e;
        n += (int)(e >> 16) <= k;
    }
    return n;
}

/* Runs the tests of a plan over the seed range and marks the survivors
 * (seeds for which the plan has enough eyes) for confirmation. If 'first' is
 * given, it holds the result of the plan's first test (shared with other
 * plans) and the cascade continues from there. */
static void sweepPlan(EyeSearch *es, const KPlan *p, const uint32_t *first,
    int nfirst)
{
    const uint32_t *in = NULL;
    uint32_t *out = es->bufA;
    int nin = es->h1 - es->h0, i0 = 0;
    if (first)
    {
        in = first;
        nin = nfirst;
        i0 = 1;
    }
    for (int i = i0; i < p->ntests && nin > 0; i++)
    {
        const EyeTest *t = &p->test[i];
        const EyeTable *tab = eyeTableFor(es, t->n, nin);
        int nout;
        es->st.tests += nin;
        if (!tab)
            nout = sweepSlow(es, t, p->k, in, nin, out);
        else if (!in)
            nout = sweepRange(es, tab, t, p->k, nin, out);
        else
            nout = sweepList(es, tab, t, p->k, in, nin, out);
        in = out;
        out = out == es->bufA ? es->bufB : es->bufA;
        nin = nout;
    }
    if (nin <= 0)
        return;
    const Group *g = &es->groups[p->group];
    es->st.survivors += nin;
    for (int i = 0; i < nin; i++)
    {
        uint32_t h = in ? in[i] & 0xffff : (uint32_t)(es->h0 + i);
        es->mark[2 * h + 0] |= g->mask[0];
        es->mark[2 * h + 1] |= g->mask[1];
    }
}

static inline int cmpTest(const EyeTest *a, const EyeTest *b)
{
    if (a->mx != b->mx) return a->mx < b->mx ? -1 : 1;
    if (a->mz != b->mz) return a->mz < b->mz ? -1 : 1;
    return a->n < b->n ? -1 : a->n > b->n;
}

static int cmpTestQ(const void *a, const void *b)
{
    return cmpTest((const EyeTest *) a, (const EyeTest *) b);
}

static int countTest(const EyeTest *keys, int n, const EyeTest *t)
{
    int lo = 0, hi = n;     // first index with keys[i] >= t
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (cmpTest(&keys[mid], t) < 0) lo = mid + 1; else hi = mid;
    }
    int c = 0;
    while (lo + c < n && cmpTest(&keys[lo + c], t) == 0)
        c++;
    return c;
}

typedef struct
{
    EyeTest t;      // first test, or a unique key for plans that allow misses
    int k;
    int idx;        // plan index
} PlanOrder;

static int cmpOrder(const void *a, const void *b)
{
    const PlanOrder *p = (const PlanOrder *) a, *q = (const PlanOrder *) b;
    if (p->k != q->k) return p->k - q->k;
    int c = cmpTest(&p->t, &q->t);
    return c ? c : p->idx - q->idx;
}

/* Stage (b) for a batch of plans. Plans without allowed misses may test
 * their frames in any order; each starts with the test that most plans of
 * the batch share (same frame chunk and draw), and the first sweep is then
 * done once per distinct test (10-25% fewer sweeps). */
static int sweepBatch(EyeSearch *es, int np, const volatile int *stop)
{
    KPlan *pl = es->plans;
    EyeTest *keys = es->keys;
    int nk = 0;
    for (int i = 0; i < np; i++)
        if (pl[i].k == 0)
            for (int j = 0; j < pl[i].ntests; j++)
                keys[nk++] = pl[i].test[j];
    qsort(keys, nk, sizeof(*keys), cmpTestQ);
    for (int i = 0; i < np; i++)
    {
        KPlan *p = &pl[i];
        if (p->k != 0)
            continue;
        int best = 0, bc = -1;
        for (int j = 0; j < p->ntests; j++)
        {
            int c = countTest(keys, nk, &p->test[j]);
            if (c > bc)
            {
                bc = c;
                best = j;
            }
        }
        EyeTest t = p->test[best];
        memmove(&p->test[1], &p->test[0], best * sizeof(EyeTest));
        p->test[0] = t;
    }
    PlanOrder *ord = (PlanOrder *) es->order;
    for (int i = 0; i < np; i++)
    {
        ord[i].t = pl[i].test[0];
        ord[i].k = pl[i].k;
        ord[i].idx = i;
    }
    qsort(ord, np, sizeof(*ord), cmpOrder);

    for (int i = 0; i < np; )
    {
        if (stop && *stop)
            return -1;
        const KPlan *p = &pl[ord[i].idx];
        int j = i + 1;
        if (p->k == 0)
            while (j < np && ord[j].k == 0 && cmpTest(&ord[j].t, &p->test[0]) == 0)
                j++;
        if (j == i + 1)
        {
            sweepPlan(es, p, NULL, 0);
        }
        else
        {   // shared first test
            const EyeTest *t = &p->test[0];
            int nin = es->h1 - es->h0;
            const EyeTable *tab = eyeTableFor(es, t->n, nin);
            es->st.tests += nin;
            int ns = tab ? sweepRange(es, tab, t, 0, nin, es->bufS) :
                sweepSlow(es, t, 0, NULL, nin, es->bufS);
            for (int q = i; q < j; q++)
                sweepPlan(es, &pl[ord[q].idx], es->bufS, ns);
        }
        i = j;
    }
    return 0;
}


//==============================================================================
// Stage (a): candidate start chunks and plans
//==============================================================================

static int popcount16(unsigned v)
{
    int n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

/* P(Binomial(n, 0.1) >= m) */
static double binomTail(int n, int m)
{
    double p = 0;
    for (int i = m; i <= n; i++)
    {
        double c = 1;
        for (int j = 0; j < i; j++)
            c = c * (n - j) / (j + 1);
        p += c * pow(0.1, i) * pow(0.9, n - i);
    }
    return p;
}

/* Builds the tests of a plan. Returns 0 if the plan cannot reach minEyes. */
static int makeKPlan(const EyeSearch *es, KPlan *kp, const PortalPlan *pp, int group)
{
    unsigned counted = es->cfg.exactOnly ? pp->exact : 0xfff;
    int nc = popcount16(counted);
    if (nc < es->cfg.minEyes)
        return 0;
    kp->group = group;
    kp->ntests = 0;
    kp->k = nc - es->cfg.minEyes;
    // shared tables first (chunks with the plain 760 skip), then the rest
    for (int pass = 0; pass < 2; pass++)
    {
        for (int j = 0; j < pp->nchunks; j++)
        {
            const PortalChunk *pc = &pp->chunks[j];
            int shared = pc->skip == PORTAL_ROOM_CALLS_BEFORE_EYES;
            if (shared != (pass == 0))
                continue;
            for (int d = 0; d < 12; d++)
            {
                if (!(pc->frames & counted & (1u << d)))
                    continue;
                EyeTest *t = &kp->test[kp->ntests++];
                t->mx = (int64_t) pc->chunkX * 16;
                t->mz = (int64_t) pc->chunkZ * 16;
                t->n = (uint32_t)(pc->skip + d);
            }
        }
    }
    return 1;
}

static int inGroup(const Group *g, int cx, int cz)
{
    if (cx < g->x0 || cx > g->x1 || cz < g->z0 || cz > g->z1)
        return 0;
    if (g->type == G_WINDOW)
        return 1;
    if (g->type == G_STRIP)
    {   // does the square of half size 7.5 (+ margin) around the chunk meet
        // the segment {d * u : dmin <= d <= dmax}?
        const double r = 7.5 + 0.05;
        double lo = g->dmin, hi = g->dmax;
        double c[2] = { cx, cz }, u[2] = { g->ux, g->uz };
        for (int a = 0; a < 2; a++)
        {
            if (fabs(u[a]) < 1e-12)
            {
                if (fabs(c[a]) > r)
                    return 0;
                continue;
            }
            double t0 = (c[a] - r) / u[a], t1 = (c[a] + r) / u[a];
            if (t0 > t1) { double t = t0; t0 = t1; t1 = t; }
            if (t0 > lo) lo = t0;
            if (t1 < hi) hi = t1;
        }
        return lo <= hi;
    }
    // annulus: does the square of half size 7 meet the annulus?
    double ax = fabs((double) cx), az = fabs((double) cz);
    double nx = ax > 7 ? ax - 7 : 0, nz = az > 7 ? az - 7 : 0;
    double fx = ax + 7, fz = az + 7;
    return nx * nx + nz * nz <= g->dmax * g->dmax &&
           fx * fx + fz * fz >= g->dmin * g->dmin;
}

static void setGroupMask(Group *g)
{
    g->mask[0] = g->mask[1] = 0;
    for (int i = g->i0; i <= g->i1; i++)
        g->mask[i >> 6] |= 1ULL << (i & 63);
}

static void addWindow(EyeSearch *es, int index, Pos approx)
{
    Group *g = &es->groups[es->ngroups++];
    memset(g, 0, sizeof(*g));
    g->type = G_WINDOW;
    g->i0 = g->i1 = index;
    // locateBiome() searches +/-112 blocks (in 4-block steps) around the
    // approximate position; the start chunk is the chunk of the result
    g->x0 = (approx.x - 112) >> 4;
    g->x1 = (approx.x + 112) >> 4;
    g->z0 = (approx.z - 112) >> 4;
    g->z1 = (approx.z + 112) >> 4;
    setGroupMask(g);
}

/* Ring number of every stronghold index, as in nextStronghold(). */
static int ringOf(int index)
{
    int ringnum = 0, ringmax = 3, ringidx = 0;
    for (int i = 0; i < index; i++)
    {
        if (++ringidx == ringmax)
        {
            ringnum++;
            ringidx = 0;
            ringmax = ringmax + 2 * ringmax / (ringnum + 1);
            if (ringmax > 128 - i)
                ringmax = 128 - i;
        }
    }
    return ringnum;
}

/* Candidate groups of a family: for each stronghold index a superset of the
 * start chunks that nextStronghold() can return. */
static void makeGroups(EyeSearch *es)
{
    const int mc = es->cfg.mc, N = es->cfg.numStrongholds;
    StrongholdIter it;
    initFirstStronghold(&it, mc, es->low48);
    es->ngroups = 0;

    if (es->newIter)
    {   // the approximate positions follow from the lower 48 bits alone
        for (int i = 0; i < N; i++)
        {
            addWindow(es, i, it.nextapprox);
            if (nextStronghold(&it, NULL) <= 0)
                break;
        }
        return;
    }

    // 1.18 .. 1.19.2: the ring iteration shares its RNG with the biome
    // search, so only the first position is known exactly.
    addWindow(es, 0, it.nextapprox);
    double angle = it.angle;
    for (int i = 1; i < N && i < 3; i++)
    {   // same ring: the angle advances by 2pi/3, the distance is random
        angle += 2 * ES_PI / 3;
        Group *g = &es->groups[es->ngroups++];
        memset(g, 0, sizeof(*g));
        g->type = G_STRIP;
        g->i0 = g->i1 = i;
        g->ux = cos(angle);
        g->uz = sin(angle);
        g->dmin = 128.0 - 40;
        g->dmax = 128.0 + 40;
        double ex[2] = { g->ux * g->dmin, g->ux * g->dmax };
        double ez[2] = { g->uz * g->dmin, g->uz * g->dmax };
        g->x0 = (int) floor(fmin(ex[0], ex[1]) - 8);
        g->x1 = (int) ceil(fmax(ex[0], ex[1]) + 8);
        g->z0 = (int) floor(fmin(ez[0], ez[1]) - 8);
        g->z1 = (int) ceil(fmax(ez[0], ez[1]) + 8);
        setGroupMask(g);
    }
    for (int i = 3; i < N; )
    {   // later rings start at a random angle: the whole annulus
        int r = ringOf(i), j = i;
        while (j + 1 < N && ringOf(j + 1) == r)
            j++;
        Group *g = &es->groups[es->ngroups++];
        memset(g, 0, sizeof(*g));
        g->type = G_ANNULUS;
        g->i0 = i;
        g->i1 = j;
        // dist = 128 + 192 r + [-40, 40) chunks; rounding of the
        // approximate chunk adds at most sqrt(0.5)
        g->dmin = 128.0 + 192.0 * r - 40 - 0.75;
        g->dmax = 128.0 + 192.0 * r + 40 + 0.75;
        g->x0 = g->z0 = -(int) ceil(g->dmax) - 8;
        g->x1 = g->z1 = (int) ceil(g->dmax) + 8;
        setGroupMask(g);
        i = j + 1;
    }
}

static double expectedTests(int ntests, int k)
{
    double e = (k + 1) / 0.9;
    return e < ntests ? e : ntests;
}

/* A priori choice for groups whose plans are not built yet: assume that
 * every candidate counts all 12 frames (this overestimates the survivors). */
static void decideGroupsApriori(EyeSearch *es)
{
    const int m = es->cfg.minEyes;
    double nseeds = es->h1 - es->h0;
    double p = binomTail(12, m);
    int lastDirect = -1;
    for (int i = 0; i < es->ngroups; i++)
    {
        Group *g = &es->groups[i];
        g->direct = 0;
        if (es->mode == EYESEARCH_DIRECT)
            g->direct = 1;
        if (es->mode != EYESEARCH_AUTO || g->type == G_WINDOW)
            continue; // windows decide after building their plans
        // annulus area (a window is never decided here, a strip is small)
        double ncand = g->type == G_STRIP ? 1500.0 :
            ES_PI * ((g->dmax + 10) * (g->dmax + 10) -
            (g->dmin > 10 ? (g->dmin - 10) * (g->dmin - 10) : 0));
        double f = 1 - pow(1 - p, ncand);
        double cost = ncand * (COST_LAYOUT + nseeds * COST_TEST * expectedTests(12, 12 - m));
        double gain = (1 - f) * nseeds * COST_BIOME * (g->i1 - g->i0 + 1);
        if (cost >= gain)
        {
            g->direct = 1;
            lastDirect = g->i1;
        }
    }
    // old versions: a seed that is confirmed up to some index pays for all
    // biome searches before it anyway
    if (!es->newIter)
        for (int i = 0; i < es->ngroups; i++)
            if (es->groups[i].i1 <= lastDirect)
                es->groups[i].direct = 1;
}

static int stopped(const volatile int *stop)
{
    return stop && *stop;
}

static void markGroup(EyeSearch *es, const Group *g)
{
    for (int h = es->h0; h < es->h1; h++)
    {
        es->mark[2 * h + 0] |= g->mask[0];
        es->mark[2 * h + 1] |= g->mask[1];
    }
}

/* Monotonic wall clock in seconds for the stage timings: per call, so the
 * statistics of one context stay meaningful when several threads scan. */
static double now(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart / (double) f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

/* Stage (a) for group gi from candidate (*px, *pz) on: up to 'max' plans into
 * es->plans + 'at'. Returns the number of plans (the group is complete once
 * *pz > g->z1), or -1 if stopped. */
static int makePlans(EyeSearch *es, int gi, int *px, int *pz, int at, int max,
    const volatile int *stop)
{
    const Group *g = &es->groups[gi];
    int x = *px, z = *pz, np = 0;
    double t0 = now();
    for (; z <= g->z1 && np < max; )
    {
        if (stopped(stop))
            return -1;
        if (inGroup(g, x, z))
        {
            PortalPlan pp;
            es->st.candidates++;
            if (getStrongholdLayoutToPortal(es->sh, es->cfg.mc, es->low48, x, z) &&
                getPortalPlan(&pp, es->sh) &&
                makeKPlan(es, &es->plans[at + np], &pp, gi))
                np++;
        }
        if (++x > g->x1) { x = g->x0; z++; }
    }
    *px = x;
    *pz = z;
    es->st.timePrep += now() - t0;
    return np;
}

/* All plans of a window are known (AUTO mode): is confirming every seed
 * directly cheaper than filtering? Then the group is marked as direct. */
static int decideWindow(EyeSearch *es, Group *g, const KPlan *plans, int np)
{
    if (g->type != G_WINDOW || es->mode != EYESEARCH_AUTO)
        return 0;
    double nseeds = es->h1 - es->h0, keep = 1, cost = 0;
    for (int i = 0; i < np; i++)
    {
        const KPlan *p = &plans[i];
        keep *= 1 - binomTail(p->ntests, es->cfg.minEyes);
        cost += nseeds * COST_TEST * expectedTests(p->ntests, p->k);
    }
    if (cost >= keep * nseeds * COST_BIOME)
    {
        g->direct = 1;
        markGroup(es, g);
        return 1;
    }
    return 0;
}

/* Stage (a) + (b) for one group. Returns -1 if stopped. */
static int scanGroup(EyeSearch *es, int gi, const volatile int *stop)
{
    Group *g = &es->groups[gi];
    if (g->direct)
    {
        markGroup(es, g);
        return 0;
    }
    int x = g->x0, z = g->z0;
    for (;;)
    {
        // (a) the next batch of candidate plans
        int np = makePlans(es, gi, &x, &z, 0, BATCH, stop);
        if (np < 0)
            return -1;
        if (decideWindow(es, g, es->plans, np))
            return 0;

        // (b) eye tests
        double t1 = now();
        es->st.plans += np;
        if (sweepBatch(es, np, stop) < 0)
            return -1;
        es->st.timeSweep += now() - t1;
        if (z > g->z1)
            return 0;
    }
}


//==============================================================================
// Stage (c): exact confirmation
//==============================================================================

static int confirmSeeds(EyeSearch *es, EyeSearchCallback cb, void *user,
    const volatile int *stop)
{
    const EyeSearchConfig *cfg = &es->cfg;
    int hits = 0;
    for (int h = es->h0; h < es->h1; h++)
    {
        uint64_t m0 = es->mark[2 * h], m1 = es->mark[2 * h + 1];
        if (!(m0 | m1))
            continue;
        int last = m1 ? 64 + 63 - __builtin_clzll(m1) : 63 - __builtin_clzll(m0);
        uint64_t seed = ((uint64_t) h << 48) | es->low48;
        applySeed(&es->g, DIM_OVERWORLD, seed);
        es->st.confirmSeeds++;

        StrongholdIter it;
        initFirstStronghold(&it, cfg->mc, seed);
        for (int i = 0; i <= last; i++)
        {
            if (stopped(stop))
                return -1;
            int want = (int)((i < 64 ? m0 >> i : m1 >> (i - 64)) & 1);
            // 1.19.3+: positions of skipped indices need no biome search
            const Generator *g = (want || !es->newIter) ? &es->g : NULL;
            int more = nextStronghold(&it, g);
            if (g)
                es->st.biomeSearches++;
            if (want)
            {
                EyeSearchHit hit;
                hit.seed = seed;
                hit.index = i;
                hit.startChunk.x = it.pos.x >> 4;
                hit.startChunk.z = it.pos.z >> 4;
                if (getEndPortal(&hit.portal, es->sh, cfg->mc, seed,
                        hit.startChunk.x, hit.startChunk.z))
                {
                    int eyes = cfg->exactOnly ?
                        hit.portal.exactEyeCount : hit.portal.eyeCount;
                    if (eyes >= cfg->minEyes)
                    {
                        hits++;
                        es->st.hits++;
                        if (cb && cb(user, &hit))
                            return -1;
                    }
                }
            }
            if (more <= 0)
                break;
        }
    }
    return hits;
}


//==============================================================================
// API
//==============================================================================

EyeSearch *createEyeSearch(const EyeSearchConfig *cfg)
{
    if (cfg->mc < MC_1_18 || cfg->mc > MC_NEWEST)
        return NULL;
    if (cfg->numStrongholds < 1 || cfg->numStrongholds > 128)
        return NULL;
    if (cfg->minEyes < 1 || cfg->minEyes > 12)
        return NULL;

    EyeSearch *es = (EyeSearch*) calloc(1, sizeof(*es));
    if (!es)
        return NULL;
    es->cfg = *cfg;
    es->newIter = cfg->mc > MC_1_19_2;
    int idx = 0, step = 0;
    getStrongholdFeatureParams(cfg->mc, &idx, &step);
    es->salt = (uint64_t)(idx + 10000 * step);
    es->mode = EYESEARCH_AUTO;
    setupGenerator(&es->g, cfg->mc,
        (cfg->large ? LARGE_BIOMES : 0) | (cfg->amplified ? AMPLIFIED : 0));

    es->sh = (Stronghold*) malloc(sizeof(Stronghold));
    es->pow = (NibTable*) malloc(JUMP_POW * sizeof(NibTable));
    es->exactT = (EyeTable*) malloc(12 * sizeof(EyeTable));
    es->slot = (EyeSlot*) calloc(SLOTS, sizeof(EyeSlot));
    es->decoA = (uint64_t*) malloc(NSEEDS * sizeof(uint64_t));
    es->decoB = (uint64_t*) malloc(NSEEDS * sizeof(uint64_t));
    es->bufA = (uint32_t*) malloc(NSEEDS * sizeof(uint32_t));
    es->bufB = (uint32_t*) malloc(NSEEDS * sizeof(uint32_t));
    es->bufS = (uint32_t*) malloc(NSEEDS * sizeof(uint32_t));
    es->keys = (EyeTest*) malloc(BATCH * 12 * sizeof(EyeTest));
    es->order = malloc(BATCH * sizeof(PlanOrder));
    es->mark = (uint64_t*) malloc(2 * NSEEDS * sizeof(uint64_t));
    es->plans = (KPlan*) malloc(BATCH * sizeof(KPlan));
    if (!es->sh || !es->pow || !es->exactT || !es->slot || !es->decoA ||
        !es->decoB || !es->bufA || !es->bufB || !es->bufS || !es->keys || !es->order || !es->mark || !es->plans)
    {
        freeEyeSearch(es);
        return NULL;
    }
    initJumps(es);
    return es;
}

void freeEyeSearch(EyeSearch *es)
{
    if (!es)
        return;
    free(es->sh);
    free(es->pow);
    free(es->exactT);
    free(es->slot);
    free(es->decoA);
    free(es->decoB);
    free(es->bufA);
    free(es->bufB);
    free(es->bufS);
    free(es->keys);
    free(es->order);
    free(es->mark);
    free(es->plans);
    free(es);
}

void setEyeSearchMode(EyeSearch *es, int mode)
{
    es->mode = mode;
}

void getEyeSearchStats(const EyeSearch *es, EyeSearchStats *st)
{
    *st = es->st;
}

void resetEyeSearchStats(EyeSearch *es)
{
    memset(&es->st, 0, sizeof(es->st));
}

int eyeSearchIsCandidate(EyeSearch *es, uint64_t low48, int index, int cx, int cz)
{
    es->low48 = low48 & MASK48;
    makeGroups(es);
    for (int i = 0; i < es->ngroups; i++)
    {
        const Group *g = &es->groups[i];
        if (index >= g->i0 && index <= g->i1 && inGroup(g, cx, cz))
            return 1;
    }
    return 0;
}

long eyeSearchFilterCheck(EyeSearch *es, uint64_t low48, int h0, int h1,
    long *checked)
{
    long missing = 0, n = 0;
    int mode = es->mode;
    es->mode = EYESEARCH_FILTER;
    es->low48 = low48 & MASK48;
    es->h0 = h0;
    es->h1 = h1;
    for (int h = h0; h < h1; h++)
    {
        uint64_t seed = ((uint64_t) h << 48) | es->low48;
        Xoroshiro xr;
        xSetSeed(&xr, seed);
        es->decoA[h] = xNextLongJ(&xr) | 1;
        es->decoB[h] = xNextLongJ(&xr) | 1;
        es->mark[2 * h] = es->mark[2 * h + 1] = 0;
    }
    makeGroups(es);
    decideGroupsApriori(es);
    for (int gi = 0; gi < es->ngroups; gi++)
    {
        Group *g = &es->groups[gi];
        if (g->type == G_ANNULUS)
            continue;
        scanGroup(es, gi, NULL);
        for (int z = g->z0; z <= g->z1; z++)
        {
            for (int x = g->x0; x <= g->x1; x++)
            {
                PortalPlan pp;
                if (!inGroup(g, x, z) ||
                    !getStrongholdLayout(es->sh, es->cfg.mc, es->low48, x, z) ||
                    !getPortalPlan(&pp, es->sh))
                    continue;
                for (int h = h0; h < h1; h++)
                {
                    uint64_t seed = ((uint64_t) h << 48) | es->low48;
                    uint16_t eyes = evalPortalEyes(&pp, seed, es->cfg.exactOnly);
                    if (es->cfg.exactOnly)
                        eyes &= pp.exact;
                    n++;
                    if (popcount16(eyes) >= es->cfg.minEyes &&
                        !((es->mark[2 * h + (g->i0 >> 6)] >> (g->i0 & 63)) & 1))
                        missing++;
                }
            }
        }
    }
    es->mode = mode;
    if (checked)
        *checked = n;
    return missing;
}

long eyeSearchSelfTest(EyeSearch *es, uint64_t rng, long count, long *ambiguous)
{
    long errors = 0, amb = 0;
    for (long i = 0; i < count; i++)
    {
        // a skip: mostly the shared ones, sometimes a random longer one
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t n = (rng >> 33) % 16 < 12 ? 760 + (uint32_t)((rng >> 40) % 12) :
            760 + (uint32_t)((rng >> 20) % 3000);
        const EyeTable *t = eyeTableFor(es, n, SLOW_MAX);
        for (int j = 0; j < 64; j++)
        {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            uint64_t v = rng ^ (rng >> 29) * 0x9e3779b97f4a7c15ULL;
            Xoroshiro xr;
            xSetSeed(&xr, v);
            xSkipN(&xr, (int) n);
            uint64_t top = xNextLong(&xr) >> 40;
            uint64_t base = eyeBase(t, v);
            uint64_t delta = (top - base) & 0xffffff;
            int eye = top > EYE_T;
            if (delta > 4)
                errors++;
            if (EYE_FAIL(base) && eye)
                errors++;
            if (EYE_PASS(base) && !eye)
                errors++;
            if (!EYE_FAIL(base) && !EYE_PASS(base))
            {
                amb++;
                if (eyeExact(es, v, n) != eye)
                    errors++;
            }
            if (j == 0 && eyeExact(es, v, n) != eye)
                errors++;
        }
    }
    // boundary cases: many cheap estimates, the exact check only near the
    // threshold or the wrap-around, where the carries decide
    const EyeTable *t = &es->exactT[0];
    for (long i = 0; i < count * 1000; i++)
    {
        uint64_t v = rng + 0x9e3779b97f4a7c15ULL * (uint64_t) i;
        uint64_t base = eyeBase(t, v);
        if ((base >= EYE_T - 8 && base <= EYE_T + 8) || base >= 0xffffff - 8)
        {
            Xoroshiro xr;
            xSetSeed(&xr, v);
            xSkipN(&xr, PORTAL_ROOM_CALLS_BEFORE_EYES);
            uint64_t top = xNextLong(&xr) >> 40;
            int eye = top > EYE_T;
            if (((top - base) & 0xffffff) > 4)
                errors++;
            if ((EYE_FAIL(base) && eye) || (EYE_PASS(base) && !eye))
                errors++;
            if (!EYE_FAIL(base) && !EYE_PASS(base))
            {
                amb++;
                if (eyeExact(es, v, PORTAL_ROOM_CALLS_BEFORE_EYES) != eye)
                    errors++;
            }
        }
    }
    if (ambiguous)
        *ambiguous = amb;
    return errors;
}

/* Common start of a scan: decoration multipliers, cleared marks, groups. */
static void beginScan(EyeSearch *es, uint64_t low48, int highBegin, int highEnd)
{
    es->low48 = low48 & MASK48;
    es->h0 = highBegin;
    es->h1 = highEnd;
    es->st.families++;
    es->st.seeds += highEnd - highBegin;

    double t0 = now();
    for (int h = es->h0; h < es->h1; h++)
    {
        uint64_t seed = ((uint64_t) h << 48) | es->low48;
        Xoroshiro xr;
        xSetSeed(&xr, seed);
        es->decoA[h] = xNextLongJ(&xr) | 1;
        es->decoB[h] = xNextLongJ(&xr) | 1;
        es->mark[2 * h] = es->mark[2 * h + 1] = 0;
    }
    makeGroups(es);
    decideGroupsApriori(es);
    es->st.timePrep += now() - t0;
}

int scanEyeFamilyRange(EyeSearch *es, uint64_t low48, int highBegin, int highEnd,
    EyeSearchCallback cb, void *user, const volatile int *stop)
{
    if (highBegin < 0) highBegin = 0;
    if (highEnd > NSEEDS) highEnd = NSEEDS;
    if (highBegin >= highEnd)
        return 0;
    beginScan(es, low48, highBegin, highEnd);

    for (int i = 0; i < es->ngroups; i++)
        if (scanGroup(es, i, stop) < 0)
            return -1;

    double t0 = now();
    int hits = confirmSeeds(es, cb, user, stop);
    es->st.timeConfirm += now() - t0;
    return hits;
}

int scanEyeFamily(EyeSearch *es, uint64_t low48, EyeSearchCallback cb,
    void *user, const volatile int *stop)
{
    return scanEyeFamilyRange(es, low48, 0, NSEEDS, cb, user, stop);
}


//==============================================================================
// Split scan (stage (b) outside the engine)
//==============================================================================

void eyeSearchBegin(EyeSearch *es, uint64_t low48)
{
    beginScan(es, low48, 0, NSEEDS);
    es->sgi = 0;
    es->sx = es->ngroups ? es->groups[0].x0 : 0;
    es->sz = es->ngroups ? es->groups[0].z0 : 0;
}

void eyeSearchGetDecoration(const EyeSearch *es, uint64_t *ab)
{
    for (int h = 0; h < NSEEDS; h++)
    {
        ab[2 * h + 0] = es->decoA[h];
        ab[2 * h + 1] = es->decoB[h];
    }
}

uint64_t eyeSearchSalt(const EyeSearch *es)
{
    return es->salt;
}

uint64_t eyeSearchFamily(const EyeSearch *es)
{
    return es->low48;
}

static void toPublicPlan(EyePlan *out, const KPlan *p)
{
    memset(out, 0, sizeof(*out));
    out->group = p->group;
    out->ntests = p->ntests;
    out->k = p->k;
    for (int i = 0; i < p->ntests; i++)
    {
        out->test[i].mx = (int32_t) p->test[i].mx;
        out->test[i].mz = (int32_t) p->test[i].mz;
        out->test[i].n = p->test[i].n;
    }
}

static void fromPublicPlan(KPlan *out, const EyePlan *p)
{
    out->group = p->group;
    out->ntests = p->ntests;
    out->k = p->k;
    for (int i = 0; i < p->ntests; i++)
    {
        out->test[i].mx = p->test[i].mx;
        out->test[i].mz = p->test[i].mz;
        out->test[i].n = p->test[i].n;
    }
}

// A window (15x15 candidates, see addWindow) always fits into one call.
typedef char eyesearch_window_fits[(15 * 15 <= EYESEARCH_MIN_PLANS) ? 1 : -1];

int eyeSearchPlansDone(const EyeSearch *es)
{
    return es->sgi >= es->ngroups;
}

int eyeSearchNextPlans(EyeSearch *es, EyePlan *plans, int max,
    const volatile int *stop)
{
    if (max < EYESEARCH_MIN_PLANS)
        return -1;
    int np = 0;
    while (es->sgi < es->ngroups)
    {
        Group *g = &es->groups[es->sgi];
        int done = g->direct;
        if (g->direct)
        {
            markGroup(es, g);
        }
        else
        {
            // a window is generated at once (decideWindow needs all its plans)
            int room = max - np < BATCH ? max - np : BATCH;
            int area = (g->x1 - g->x0 + 1) * (g->z1 - g->z0 + 1);
            if (g->type == G_WINDOW && area > room)
            {
                if (np > 0)
                    break;  // in the next call
                // It cannot fit at all (windows have 15x15 chunks, fewer
                // than EYESEARCH_MIN_PLANS). Returning 0 here would end the
                // family early, so confirm its seeds directly (exact).
                g->direct = 1;
                markGroup(es, g);
                es->sx = g->x0;
                es->sz = g->z1 + 1;
                done = 1;
            }
        }
        if (!done)
        {
            int room = max - np < BATCH ? max - np : BATCH;
            int n = makePlans(es, es->sgi, &es->sx, &es->sz, 0, room, stop);
            if (n < 0)
                return -1;
            done = es->sz > g->z1;
            if (!decideWindow(es, g, es->plans, n))
            {
                es->st.plans += n;
                for (int i = 0; i < n; i++)
                    toPublicPlan(&plans[np + i], &es->plans[i]);
                np += n;
            }
        }
        if (done)
        {
            es->sgi++;
            if (es->sgi < es->ngroups)
            {
                es->sx = es->groups[es->sgi].x0;
                es->sz = es->groups[es->sgi].z0;
            }
        }
        if (np >= max)
            break;
    }
    return np;
}

int eyeSearchKeep(EyeSearch *es, const EyePlan *plan, int high)
{
    if (high < 0 || high >= NSEEDS || plan->group < 0 ||
        plan->group >= es->ngroups || plan->ntests > 12)
        return -1;
    double t0 = now();
    KPlan p;
    fromPublicPlan(&p, plan);
    int miss = 0;
    for (int i = 0; i < p.ntests && miss <= p.k; i++)
    {
        es->st.tests++;
        miss += !eyeExact(es, testSeed(es, &p.test[i], (uint32_t) high), p.test[i].n);
    }
    int keep = miss <= p.k;
    if (keep)
    {
        const Group *g = &es->groups[p.group];
        es->st.survivors++;
        es->mark[2 * high + 0] |= g->mask[0];
        es->mark[2 * high + 1] |= g->mask[1];
    }
    es->st.timeSweep += now() - t0;
    return keep;
}

int eyeSearchSweep(EyeSearch *es, const EyePlan *plans, int n,
    const volatile int *stop)
{
    for (int i0 = 0; i0 < n; i0 += BATCH)
    {
        int m = n - i0 < BATCH ? n - i0 : BATCH;
        for (int i = 0; i < m; i++)
            fromPublicPlan(&es->plans[i], &plans[i0 + i]);
        double t0 = now();
        if (sweepBatch(es, m, stop) < 0)
            return -1;
        es->st.timeSweep += now() - t0;
    }
    return 0;
}

int eyeSearchFinish(EyeSearch *es, EyeSearchCallback cb, void *user,
    const volatile int *stop)
{
    double t0 = now();
    int hits = confirmSeeds(es, cb, user, stop);
    es->st.timeConfirm += now() - t0;
    return hits;
}

void eyeSearchJumpColumns(const EyeSearch *es, uint32_t n, uint64_t col[128][2])
{
    colsOfJump(es, col, n);
}

int scanEyeFamilyReferenceRange(EyeSearch *es, uint64_t low48, int highBegin,
    int highEnd, EyeSearchCallback cb, void *user, const volatile int *stop)
{
    const EyeSearchConfig *cfg = &es->cfg;
    int hits = 0;

    if (highBegin < 0) highBegin = 0;
    if (highEnd > NSEEDS) highEnd = NSEEDS;
    for (uint64_t high = (uint64_t) highBegin; high < (uint64_t) highEnd; high++)
    {
        if (stopped(stop))
            return -1;

        uint64_t seed = (high << 48) | (low48 & MASK48);
        applySeed(&es->g, DIM_OVERWORLD, seed);

        StrongholdIter it;
        initFirstStronghold(&it, cfg->mc, seed);

        for (int i = 0; i < cfg->numStrongholds; i++)
        {
            int more = nextStronghold(&it, &es->g);
            EyeSearchHit hit;
            hit.seed = seed;
            hit.index = i;
            hit.startChunk.x = it.pos.x >> 4;
            hit.startChunk.z = it.pos.z >> 4;
            if (getEndPortal(&hit.portal, es->sh, cfg->mc, seed,
                    hit.startChunk.x, hit.startChunk.z))
            {
                int eyes = cfg->exactOnly ?
                    hit.portal.exactEyeCount : hit.portal.eyeCount;
                if (eyes >= cfg->minEyes)
                {
                    hits++;
                    if (cb && cb(user, &hit))
                        return -1;
                }
            }
            if (more <= 0)
                break;
        }
    }
    return hits;
}

int scanEyeFamilyReference(EyeSearch *es, uint64_t low48, EyeSearchCallback cb,
    void *user, const volatile int *stop)
{
    return scanEyeFamilyReferenceRange(es, low48, 0, NSEEDS, cb, user, stop);
}
