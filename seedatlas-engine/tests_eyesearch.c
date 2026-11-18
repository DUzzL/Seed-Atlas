/* Tests for eyesearch.c: scanEyeFamily() must report exactly the hits of
 * scanEyeFamilyReference().
 *
 * Usage:
 *   test-eyesearch                 the standard suite (a few minutes)
 *   test-eyesearch -quick          a smaller suite
 *   test-eyesearch -real DIR       also scan the families of the real 12-eye
 *                                  portals in DIR/portals_1.21.1.txt
 *   test-eyesearch -cmp MC LARGE N MINEYES EXACT LOW48 BEGIN END
 *                                  compare one configuration on the seed
 *                                  highs BEGIN..END-1 (MC as in "1.21.1")
 */
#include "eyesearch.h"
#include "util.h"

// The configurations here are short positional initializers; fields they do
// not name (the world type) are zero, which is the default world.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
static uint64_t lastTables; // EyeSearchStats.tables of the last compareRange()

static int popcount16(unsigned v)
{
    int n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* Processor time on POSIX, wall time on Windows; both fine for one thread. */
static double now(void)
{
    return (double) clock() / CLOCKS_PER_SEC;
}

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

typedef struct
{
    EyeSearchHit *v;
    int n, cap;
} HitList;

static int collect(void *user, const EyeSearchHit *hit)
{
    HitList *l = (HitList *) user;
    if (l->n == l->cap)
    {
        l->cap = l->cap ? 2 * l->cap : 256;
        l->v = (EyeSearchHit *) realloc(l->v, l->cap * sizeof(*l->v));
    }
    l->v[l->n++] = *hit;
    return 0;
}

static int cmpHit(const void *a, const void *b)
{
    const EyeSearchHit *x = (const EyeSearchHit *) a, *y = (const EyeSearchHit *) b;
    if (x->seed != y->seed)
        return x->seed < y->seed ? -1 : 1;
    return x->index - y->index;
}

static int samePortal(const EndPortal *a, const EndPortal *b)
{
    for (int i = 0; i < 12; i++)
        if (a->frame[i].x != b->frame[i].x || a->frame[i].y != b->frame[i].y ||
            a->frame[i].z != b->frame[i].z)
            return 0;
    return a->center.x == b->center.x && a->center.y == b->center.y &&
        a->center.z == b->center.z && a->facing == b->facing &&
        a->eyes == b->eyes && a->exact == b->exact &&
        a->eyeCount == b->eyeCount && a->exactEyeCount == b->exactEyeCount &&
        a->portalBlocks == b->portalBlocks && a->lit == b->lit;
}

static const char *mcName(int mc)
{
    return mc2str(mc);
}

/* Compares fast and reference scan on one seed range. Returns the number of
 * hits (or -1 on mismatch). */
static int compareRange(const EyeSearchConfig *cfg, int mode, uint64_t low48,
    int h0, int h1, double *tFast, double *tRef)
{
    EyeSearch *es = createEyeSearch(cfg);
    if (!es)
    {
        CHECK(0, "createEyeSearch failed");
        return -1;
    }
    HitList a = {0}, b = {0};
    setEyeSearchMode(es, mode);
    double t0 = now();
    int na = scanEyeFamilyRange(es, low48, h0, h1, collect, &a, NULL);
    double t1 = now();
    int nb = scanEyeFamilyReferenceRange(es, low48, h0, h1, collect, &b, NULL);
    double t2 = now();
    if (tFast) *tFast += t1 - t0;
    if (tRef) *tRef += t2 - t1;

    int ok = na == a.n && nb == b.n && a.n == b.n;
    qsort(a.v, a.n, sizeof(*a.v), cmpHit);
    qsort(b.v, b.n, sizeof(*b.v), cmpHit);
    for (int i = 0; ok && i < a.n; i++)
    {
        ok = a.v[i].seed == b.v[i].seed && a.v[i].index == b.v[i].index &&
            a.v[i].startChunk.x == b.v[i].startChunk.x &&
            a.v[i].startChunk.z == b.v[i].startChunk.z &&
            samePortal(&a.v[i].portal, &b.v[i].portal);
        if (!ok)
            printf("  first difference: fast %lld #%d, reference %lld #%d\n",
                (long long) a.v[i].seed, a.v[i].index,
                (long long) b.v[i].seed, b.v[i].index);
    }
    CHECK(ok, "%s large=%d n=%d min=%d exact=%d mode=%d family %#llx [%d,%d): "
        "fast %d (%d), reference %d (%d)", mcName(cfg->mc), cfg->large,
        cfg->numStrongholds, cfg->minEyes, cfg->exactOnly, mode,
        (unsigned long long) low48, h0, h1, na, a.n, nb, b.n);
    int hits = a.n;
    EyeSearchStats st;
    getEyeSearchStats(es, &st);
    lastTables = st.tables;
    free(a.v);
    free(b.v);
    freeEyeSearch(es);
    return ok ? hits : -1;
}

/* Ranges of at least SLOW_MAX (192) seeds, exactOnly 0, always filtering:
 * only then are eye tables built for the skips of non-exact chunks (and
 * stepped on from a cached table for the next draw of the same chunk), and
 * the table sweeps run over them. The short ranges above test those skips
 * one seed at a time only. */
static void testLargeRanges(int quick)
{
    const struct { int mc, large, nsh, minEyes; uint64_t low; int h0; } cases[] = {
        { MC_26_3,   0, 2, 4, 0x0000123456789abcULL, 1000 },
        { MC_1_21_1, 1, 2, 3, 0x00007a3b1c2d3e4fULL, 30000 },
        { MC_1_18,   0, 2, 4, 0x0000fedcba987654ULL, 0 },
        { MC_1_19_2, 0, 1, 5, 0x0000314159265358ULL, 60000 },
    };
    int n = quick ? 2 : 4, len = 256;
    double tf = 0, tr = 0;
    long hits = 0;
    uint64_t tables = 0;
    for (int c = 0; c < n; c++)
    {
        EyeSearchConfig cfg = { cases[c].mc, cases[c].large, cases[c].nsh,
            cases[c].minEyes, 0 };
        int h = compareRange(&cfg, EYESEARCH_FILTER, cases[c].low, cases[c].h0,
            cases[c].h0 + len, &tf, &tr);
        CHECK(lastTables > 0, "%s: no eye table built for other skips",
            mcName(cases[c].mc));
        if (h > 0)
            hits += h;
        tables += lastTables;
    }
    printf("large ranges: %d comparisons of %d seeds, %ld hits matched, "
        "%llu eye tables built (fast %.1f s, reference %.1f s)\n", n, len, hits,
        (unsigned long long) tables, tf, tr);
}

/* The candidate sets must contain every start chunk that nextStronghold()
 * returns. This is where the scan depends on the behaviour of finders.c
 * (ring iteration, +/-112 block biome search); a change there fails here. */
static void testCandidates(int quick)
{
    const int mcs[] = { MC_1_18, MC_1_19_2, MC_1_19_4, MC_1_21_1, MC_26_3 };
    uint64_t rng = 99;
    long checked = 0;
    for (int v = 0; v < 5; v++)
    {
        for (int large = 0; large <= 1; large++)
        {
            int mc = mcs[v];
            int nsh = mc > MC_1_19_2 ? 128 : 12;
            int nseeds = quick ? 4 : (mc > MC_1_19_2 ? 6 : 12);
            EyeSearchConfig cfg = { mc, large, nsh, 12, 0 };
            EyeSearch *es = createEyeSearch(&cfg);
            Generator g;
            setupGenerator(&g, mc, large ? LARGE_BIOMES : 0);
            for (int s = 0; s < nseeds; s++)
            {
                uint64_t seed = splitmix64(&rng);
                applySeed(&g, DIM_OVERWORLD, seed);
                StrongholdIter it;
                initFirstStronghold(&it, mc, seed);
                for (int i = 0; i < nsh; i++)
                {
                    int more = nextStronghold(&it, &g);
                    int cx = it.pos.x >> 4, cz = it.pos.z >> 4;
                    CHECK(eyeSearchIsCandidate(es, seed, i, cx, cz),
                        "%s seed %lld #%d at chunk %d,%d is not a candidate",
                        mcName(mc), (long long) seed, i, cx, cz);
                    checked++;
                    if (more <= 0)
                        break;
                }
            }
            freeEyeSearch(es);
        }
    }
    printf("candidate sets: %ld stronghold positions checked\n", checked);
}

/* getStrongholdLayoutToPortal() must give the same plan as the full layout. */
static void testEarlyLayout(int quick)
{
    static Stronghold a, b;
    uint64_t rng = 7;
    int n = quick ? 500 : 3000, cmp = 0;
    for (int i = 0; i < n; i++)
    {
        uint64_t seed = splitmix64(&rng);
        int cx = (int)(splitmix64(&rng) % 4001) - 2000;
        int cz = (int)(splitmix64(&rng) % 4001) - 2000;
        int mc = (i & 1) ? MC_26_3 : MC_1_18;
        int na = getStrongholdLayout(&a, mc, seed, cx, cz);
        int nb = getStrongholdLayoutToPortal(&b, mc, seed, cx, cz);
        if (!na)
            continue;
        CHECK(nb && b.portal == b.n - 1 && b.portal == a.portal,
            "early layout: piece count/portal index");
        PortalPlan pa, pb;
        if (!nb || !getPortalPlan(&pa, &a) || !getPortalPlan(&pb, &b))
            continue;
        int ok = pa.nchunks == pb.nchunks && pa.exact == pb.exact &&
            pa.facing == pb.facing;
        for (int j = 0; ok && j < pa.nchunks; j++)
        {
            const PortalChunk *x = &pa.chunks[j], *y = &pb.chunks[j];
            ok = x->chunkX == y->chunkX && x->chunkZ == y->chunkZ &&
                x->frames == y->frames && x->opening == y->opening &&
                x->exact == y->exact && x->npre == y->npre &&
                x->skip == y->skip && x->wallCells == y->wallCells;
        }
        for (int j = 0; ok && j < 12; j++)
            ok = pa.frame[j].x == pb.frame[j].x && pa.frame[j].z == pb.frame[j].z;
        for (int j = 0; ok && j < a.portal; j++)
            ok = a.pieces[j].x0 == b.pieces[j].x0 && a.pieces[j].z1 == b.pieces[j].z1 &&
                a.pieces[j].y1 - a.pieces[j].y0 == b.pieces[j].y1 - b.pieces[j].y0 &&
                a.pieces[j].type == b.pieces[j].type;
        CHECK(ok, "early layout plan differs: seed %lld chunk %d,%d",
            (long long) seed, cx, cz);
        cmp++;
    }
    printf("early layouts: %d plans compared\n", cmp);
}

static void testKernel(int quick)
{
    EyeSearchConfig cfg = { MC_26_3, 0, 1, 12, 0 };
    EyeSearch *es = createEyeSearch(&cfg);
    long amb = 0;
    long count = quick ? 20000 : 200000;
    long err = eyeSearchSelfTest(es, 12345, count, &amb);
    CHECK(err == 0, "eye kernel self test: %ld errors", err);
    printf("eye kernel: %ld + %ld draws checked, %ld ambiguous estimates resolved\n",
        count * 64, count * 1000, amb);
    freeEyeSearch(es);
}

/* Stage (b) against plain evaluation of every candidate, at thresholds where
 * plans pass often (this covers the cascade without needing biome searches). */
static void testFilter(int quick)
{
    const int mcs[] = { MC_26_3, MC_1_19_2, MC_1_18, MC_1_21_1 };
    uint64_t rng = 5;
    long total = 0;
    for (int c = 0; c < (quick ? 6 : 16); c++)
    {
        int mc = mcs[c % 4];
        int minEyes = 2 + (c * 5) % 6;      // 2 .. 7
        int exactOnly = (c / 2) & 1;
        int nsh = mc > MC_1_19_2 ? 2 : 3;
        EyeSearchConfig cfg = { mc, c & 1, nsh, minEyes, exactOnly };
        EyeSearch *es = createEyeSearch(&cfg);
        uint64_t low = splitmix64(&rng) & MASK48;
        long checked = 0;
        int h0 = (int)(splitmix64(&rng) % 60000);
        long miss = eyeSearchFilterCheck(es, low, h0, h0 + (quick ? 40 : 120), &checked);
        CHECK(miss == 0, "%s min=%d exact=%d family %#llx: %ld passing "
            "(seed, candidate) pairs not marked", mcName(mc), minEyes, exactOnly,
            (unsigned long long) low, miss);
        total += checked;
        freeEyeSearch(es);
    }
    printf("filter: %ld (seed, candidate) pairs checked\n", total);
}

typedef struct
{
    int mc, large, nsh, minEyes, exactOnly;
    int h0, h1;     // seed highs
} Case;

static void testEquivalence(int quick)
{
    // Families: arbitrary values plus the family of a real 12-eye portal.
    const uint64_t fams[] = {
        0x0000123456789abcULL, 0x0000fedcba987654ULL, 0x00007a3b1c2d3e4fULL,
        (uint64_t) -6695163795316312869LL & MASK48,
    };
    const Case cases[] = {
        // frequent hits: exercise the filter cascade with many misses allowed
        { MC_26_3,   0, 3, 3, 0,   0, 48 },
        { MC_26_3,   0, 3, 4, 1,   0, 48 },
        { MC_26_3,   1, 3, 3, 1, 100, 140 },
        { MC_26_3,   0, 8, 4, 0, 500, 524 },
        { MC_1_21_1, 0, 3, 3, 0,   0, 48 },
        { MC_1_21_1, 1, 1, 2, 1,   0, 64 },
        { MC_1_21_1, 0, 8, 3, 1,   0, 24 },
        { MC_1_19_4, 0, 3, 3, 0,   0, 48 },
        { MC_1_19_4, 1, 8, 4, 1,   0, 24 },
        { MC_1_19_2, 0, 1, 2, 0,   0, 64 },
        { MC_1_19_2, 0, 3, 3, 1,   0, 32 },
        { MC_1_19_2, 1, 3, 3, 0,   0, 32 },
        { MC_1_18,   0, 3, 3, 0,   0, 32 },
        { MC_1_18,   1, 1, 3, 1,   0, 64 },
        { MC_1_18,   0, 8, 4, 0,   0, 6 },
        { MC_1_19_2, 0, 8, 3, 1,   0, 6 },
        // plans whose counted frames just reach minEyes (no miss allowed)
        { MC_26_3,   0, 3, 5, 1,   0, 64 },
        { MC_1_21_1, 0, 3, 6, 1,   0, 64 },
        // high thresholds (no or rare hits; covers the cheap path)
        { MC_26_3,   0, 3, 12, 1,  0, 64 },
        { MC_26_3,   0, 3, 7, 0,   0, 64 },
        { MC_1_18,   0, 3, 8, 0,   0, 32 },
    };
    const int ncases = (int)(sizeof(cases) / sizeof(cases[0]));
    double tf = 0, tr = 0;
    long total = 0;
    int runs = 0;
    for (int c = 0; c < ncases; c++)
    {
        const Case *k = &cases[c];
        if (quick && (c % 3))
            continue;
        EyeSearchConfig cfg = { k->mc, k->large, k->nsh, k->minEyes, k->exactOnly };
        for (int f = 0; f < 4; f++)
        {
            if (quick && f >= 2)
                break;
            if (k->nsh >= 8 && f >= 2)
                break;  // the reference is slow there
            int modes[2] = { EYESEARCH_FILTER, EYESEARCH_AUTO };
            for (int m = 0; m < 2; m++)
            {
                // the old-version annulus (index >= 3) is huge; filter it only
                // in AUTO (where it is direct for these tiny ranges) and once
                // explicitly
                if (modes[m] == EYESEARCH_FILTER && k->nsh > 3 &&
                    k->mc <= MC_1_19_2 && f > 0)
                    continue;
                int h = compareRange(&cfg, modes[m], fams[f], k->h0, k->h1, &tf, &tr);
                if (h >= 0)
                    total += h;
                runs++;
            }
        }
    }
    printf("equivalence: %d comparisons, %ld hits matched (fast %.1f s, "
        "reference %.1f s)\n", runs, total, tf, tr);
}

/* The split scan (eyesearch.h) with stage (b) by eyeSearchSweep(), in
 * batches of the smallest allowed size and of all plans at once, must give
 * the candidates, plans, stage (b) survivors and hits of scanEyeFamily().
 * (eyeSearchKeep() is checked against the exact cascade by the GPU tools,
 * which call it for every pair the GPU keeps; here only its basic
 * behaviour.) */
static void testSplit(int quick)
{
    const EyeSearchConfig cfgs[] = {
        { MC_26_3, 0, 3, 12, 1 }, { MC_26_3, 0, 8, 8, 0 }, { MC_1_21_1, 0, 3, 7, 1 },
        { MC_1_19_2, 0, 3, 11, 0 }, { MC_1_18, 1, 1, 7, 1 }, { MC_26_3, 1, 1, 9, 0 },
    };
    static EyePlan plans[4096];
    uint64_t rng = 31;
    int runs = 0;
    long hits = 0, keeps = 0;
    for (int c = 0; c < 6; c++)
    {
        if (quick && (c & 1))
            continue;
        uint64_t low = splitmix64(&rng) & MASK48;
        HitList ref = {0};
        EyeSearch *a = createEyeSearch(&cfgs[c]);
        int nref = scanEyeFamily(a, low, collect, &ref, NULL);
        EyeSearchStats sa;
        getEyeSearchStats(a, &sa);
        qsort(ref.v, ref.n, sizeof(*ref.v), cmpHit);
        for (int m = 0; m < 2; m++)
        {
            int max = m ? 4096 : EYESEARCH_MIN_PLANS;
            EyeSearch *b = createEyeSearch(&cfgs[c]);
            HitList l = {0};
            eyeSearchBegin(b, low);
            int n, ok = 1;
            while ((n = eyeSearchNextPlans(b, plans, max, NULL)) > 0)
            {
                ok &= n <= max;
                eyeSearchSweep(b, plans, n, NULL);
                // eyeSearchKeep() on a few pairs: valid and repeatable
                for (int i = 0; i < n && i < 4; i++)
                {
                    int h = (int)(splitmix64(&rng) & 0xffff);
                    EyeSearch *x = createEyeSearch(&cfgs[c]);
                    eyeSearchBegin(x, low);
                    int k1 = eyeSearchKeep(x, &plans[i], h);
                    int k2 = eyeSearchKeep(x, &plans[i], h);
                    ok &= k1 == k2 && k1 >= 0;
                    keeps++;
                    freeEyeSearch(x);
                }
            }
            // 0 plans only once every group is handled (else the caller
            // would end the family early)
            ok &= n == 0 && eyeSearchPlansDone(b);
            int nb = eyeSearchFinish(b, collect, &l, NULL);
            EyeSearchStats sb;
            getEyeSearchStats(b, &sb);
            qsort(l.v, l.n, sizeof(*l.v), cmpHit);
            ok &= nb == nref && l.n == ref.n && sb.survivors == sa.survivors &&
                sb.plans == sa.plans && sb.candidates == sa.candidates;
            for (int i = 0; ok && i < l.n; i++)
                ok = l.v[i].seed == ref.v[i].seed && l.v[i].index == ref.v[i].index &&
                    samePortal(&l.v[i].portal, &ref.v[i].portal);
            CHECK(ok, "split scan %s n=%d min=%d exact=%d family %#llx max=%d: %d / %d hits, "
                "survivors %llu / %llu", mcName(cfgs[c].mc), cfgs[c].numStrongholds,
                cfgs[c].minEyes, cfgs[c].exactOnly, (unsigned long long) low, max, nb, nref,
                (unsigned long long) sb.survivors, (unsigned long long) sa.survivors);
            runs++;
            hits += l.n;
            free(l.v);
            freeEyeSearch(b);
        }
        free(ref.v);
        freeEyeSearch(a);
    }
    printf("split scan: %d full families compared, %ld hits matched, %ld keep calls\n",
        runs, hits, keeps);
}

/* Start chunks seen in real vanilla worlds (files with stronghold indices)
 * must be candidates too, independently of finders.c. */
static void testVanillaCandidates(const char *dir)
{
    const char *files[] = { "portals_26.3.txt", "portals_1.21.1.txt", "portals_1.18.2.txt" };
    long checked = 0;
    for (int f = 0; f < 3; f++)
    {
        char path[1024], line[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, files[f]);
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;
        while (fgets(line, sizeof(line), fp))
        {
            char ver[32];
            long long seed;
            int idx, cx, cz;
            if (line[0] == '#' ||
                sscanf(line, "%31s %lld %d %d %d", ver, &seed, &idx, &cx, &cz) != 5 ||
                idx < 0)
                continue;
            int mc = str2mc(ver);
            EyeSearchConfig cfg = { mc, 0, idx + 1, 12, 0 };
            EyeSearch *es = createEyeSearch(&cfg);
            if (!es)
                continue;
            CHECK(eyeSearchIsCandidate(es, (uint64_t) seed, idx, cx, cz),
                "vanilla start %s %lld #%d (%d,%d) is not a candidate", ver,
                seed, idx, cx, cz);
            checked++;
            freeEyeSearch(es);
        }
        fclose(fp);
    }
    printf("vanilla start chunks: %ld checked against the candidate sets\n", checked);
}

/* Full families of the real 12-eye portals of tools/portal-reference. */
static void testReal(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/portals_1.21.1.txt", dir);
    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        CHECK(0, "cannot open %s", path);
        return;
    }
    char line[4096];
    int nreal = 0;
    while (fgets(line, sizeof(line), fp))
    {
        if (line[0] == '#')
            continue;
        char ver[32];
        long long seed;
        int idx, cx, cz, off;
        if (sscanf(line, "%31s %lld %d %d %d%n", ver, &seed, &idx, &cx, &cz, &off) != 5)
            continue;
        int eyes = 0;
        const char *p = line + off;
        for (int i = 0; i < 12; i++)
        {
            int x, y, z, e, o;
            if (sscanf(p, "%d %d %d %d%n", &x, &y, &z, &e, &o) != 4)
                break;
            eyes += e;
            p += o;
        }
        if (eyes != 12)
            continue;
        nreal++;
        // stronghold index of this start in the engine
        Generator g;
        setupGenerator(&g, MC_1_21_1, 0);
        applySeed(&g, DIM_OVERWORLD, (uint64_t) seed);
        StrongholdIter it;
        initFirstStronghold(&it, MC_1_21_1, (uint64_t) seed);
        int index = -1;
        for (int i = 0; i < 128 && index < 0; i++)
        {
            nextStronghold(&it, &g);
            if ((it.pos.x >> 4) == cx && (it.pos.z >> 4) == cz)
                index = i;
        }
        for (int exactOnly = 0; exactOnly <= 1; exactOnly++)
        {
            EndPortal ep;
            int ok = getEndPortal(&ep, NULL, MC_1_21_1, (uint64_t) seed, cx, cz);
            int engineEyes = !ok ? -1 : exactOnly ? ep.exactEyeCount : ep.eyeCount;
            EyeSearchConfig cfg = { MC_1_21_1, 0, 3, 12, exactOnly };
            EyeSearch *es = createEyeSearch(&cfg);
            HitList l = {0};
            double t0 = now();
            int n = scanEyeFamily(es, (uint64_t) seed, collect, &l, NULL);
            double t1 = now();
            int found = 0;
            for (int i = 0; i < l.n; i++)
                if (l.v[i].seed == (uint64_t) seed && l.v[i].startChunk.x == cx &&
                    l.v[i].startChunk.z == cz)
                    found = 1;
            printf("real 12-eye portal %lld (chunk %d,%d, engine index %d, "
                "exactOnly %d): engine counts %d eyes, %d exact frames; "
                "family scan %.2f s, %d hits, %s\n", seed, cx, cz, index,
                exactOnly, engineEyes, ok ? popcount16(ep.exact) : 0, t1 - t0,
                n, found ? "found" : "not found");
            if (index >= 0 && index < 3 && engineEyes == 12)
                CHECK(found, "real portal %lld not found", seed);
            if (!(index >= 0 && index < 3 && engineEyes == 12))
                CHECK(!found, "unexpected hit for %lld", seed);
            free(l.v);
            freeEyeSearch(es);
        }
    }
    fclose(fp);
    CHECK(nreal == 4, "expected 4 real 12-eye portals, found %d", nreal);
}

int main(int argc, char **argv)
{
    int quick = 0;
    const char *real = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-quick"))
            quick = 1;
        else if (!strcmp(argv[i], "-real") && i + 1 < argc)
            real = argv[++i];
        else if (!strcmp(argv[i], "-cmp") && i + 8 < argc)
        {
            EyeSearchConfig cfg;
            cfg.mc = str2mc(argv[i + 1]);
            cfg.large = atoi(argv[i + 2]);
            cfg.numStrongholds = atoi(argv[i + 3]);
            cfg.minEyes = atoi(argv[i + 4]);
            cfg.exactOnly = atoi(argv[i + 5]);
            uint64_t low = strtoull(argv[i + 6], NULL, 0);
            int h0 = atoi(argv[i + 7]), h1 = atoi(argv[i + 8]);
            double tf = 0, tr = 0;
            int n = compareRange(&cfg, EYESEARCH_AUTO, low, h0, h1, &tf, &tr);
            printf("%d hits, fast %.2f s, reference %.2f s\n", n, tf, tr);
            return failures != 0;
        }
        else
        {
            printf("usage: %s [-quick] [-real DIR] | -cmp MC LARGE N MINEYES "
                "EXACT LOW48 BEGIN END\n", argv[0]);
            return 2;
        }
    }
    testKernel(quick);
    testEarlyLayout(quick);
    testCandidates(quick);
    testFilter(quick);
    testEquivalence(quick);
    testLargeRanges(quick);
    testSplit(quick);
    if (real)
    {
        testVanillaCandidates(real);
        testReal(real);
    }
    if (failures)
        printf("%d FAILURES\n", failures);
    else
        printf("eyesearch tests passed\n");
    return failures != 0;
}
