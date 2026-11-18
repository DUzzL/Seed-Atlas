/* Regression tests for strongholds.c (stronghold layouts, End portal eyes).
 *
 * The expected values were produced by running the vanilla 26.3 stronghold
 * classes (StrongholdPieces, StructurePiecesBuilder, WorldgenRandom) on the
 * same inputs; see docs/portal-eyes.md, "Verification".
 *
 * Usage: test-strongholds [-bench]
 */
#include "strongholds.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t getPopulationSeed(int mc, uint64_t ws, int x, int z);

/* FNV-1a of the vanilla 26.3 harness output for hashLayouts(MC_26_3, 1000,
 * 2024) (153876 lines: layouts and eyes of 1000 random starts). */
#define LAYOUT_HASH_26_3 0x79c47731d5af1558ULL

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint64_t fnv1a(uint64_t h, const char *s)
{
    for (; *s; s++)
        h = (h ^ (uint8_t) *s) * 0x100000001b3ULL;
    return h;
}

typedef struct { Pos3 p; int e; } FrameOut;

static int cmpFrame(const void *a, const void *b)
{
    const FrameOut *x = (const FrameOut *) a, *y = (const FrameOut *) b;
    if (x->p.x != y->p.x)
        return x->p.x < y->p.x ? -1 : 1;
    return x->p.z < y->p.z ? -1 : (x->p.z > y->p.z);
}

/* Eyes of a plan with explicit setFeatureSeed() parameters, assuming either
 * no air (solid = 1) or only air (solid = 0) in the walls of earlier pieces. */
static void planEyes(const PortalPlan *pp, uint64_t seed, int idx, int step,
    int solid, FrameOut out[12])
{
    for (int j = 0; j < pp->nchunks; j++)
    {
        const PortalChunk *pc = &pp->chunks[j];
        Xoroshiro xr;
        xSetSeed(&xr, getPopulationSeed(pp->mc, seed, pc->chunkX * 16,
            pc->chunkZ * 16) + idx + 10000 * step);
        xSkipN(&xr, pc->skip - (solid ? 0 : pc->wallCells));
        uint16_t m = drawPortalEyes(&xr);
        for (int i = 0; i < 12; i++)
        {
            if (pc->frames >> i & 1)
            {
                out[i].p = pp->frame[i];
                out[i].e = m >> i & 1;
            }
        }
    }
    qsort(out, 12, sizeof(FrameOut), cmpFrame);
}

/* Hash of the text that the vanilla harness prints for the same starts:
 * "S seed cx cz n tries", one line per piece (vanilla id, box, facing,
 * depth), then the frames sorted by x, z with eyes for both terrain
 * assumptions, using structure index 37 in step 4. */
static uint64_t hashLayouts(int mc, int count, uint64_t rngseed)
{
    static const char *ids[] = { "shstart", "shs", "shph", "shlt", "shrt",
        "shrc", "shssd", "shsd", "sh5c", "shcc", "shli", "shpr", "shfc" };
    static const char *dirs[] = { "north", "east", "south", "west" };
    static Stronghold sh;
    uint64_t h = 0xcbf29ce484222325ULL;
    char buf[256];

    for (int k = 0; k < count; k++)
    {
        uint64_t seed = splitmix64(&rngseed);
        int cx = (int)(splitmix64(&rngseed) % 801) - 400;
        int cz = (int)(splitmix64(&rngseed) % 801) - 400;
        int n = getStrongholdLayout(&sh, mc, seed, cx, cz);
        assert(n > 0);
        snprintf(buf, sizeof(buf), "S %lld %d %d %d %d\n", (long long) seed, cx, cz, n, sh.tries);
        h = fnv1a(h, buf);
        for (int i = 0; i < n; i++)
        {
            const StrongholdPiece *p = &sh.pieces[i];
            snprintf(buf, sizeof(buf), "%s %d %d %d %d %d %d %s %d\n", ids[p->type],
                p->x0, p->y0, p->z0, p->x1, p->y1, p->z1, dirs[p->facing], p->depth);
            h = fnv1a(h, buf);
        }
        PortalPlan pp;
        assert(getPortalPlan(&pp, &sh));
        for (int solid = 1; solid >= 0; solid--)
        {
            FrameOut f[12];
            planEyes(&pp, seed, 37, 4, solid, f);
            h = fnv1a(h, solid ? "E solid" : "E air");
            for (int i = 0; i < 12; i++)
            {
                snprintf(buf, sizeof(buf), "  %d %d %d %d", f[i].p.x, f[i].p.y, f[i].p.z, f[i].e);
                h = fnv1a(h, buf);
            }
            h = fnv1a(h, "\n");
        }
    }
    return h;
}

static void testLayoutVector(void)
{
    static Stronghold sh;
    // vanilla 26.3: seed 12345, start chunk (10, -20)
    assert(getStrongholdLayout(&sh, MC_26_3, 12345, 10, -20) == 102);
    assert(sh.tries == 0);
    const StrongholdPiece *s = &sh.pieces[0];
    assert(s->type == SH_START && s->facing == SH_NORTH);
    assert(s->x0 == 162 && s->y0 == -31 && s->z0 == -318);
    assert(s->x1 == 166 && s->y1 == -21 && s->z1 == -314);
    assert(sh.portal == 20);
    const StrongholdPiece *pr = &sh.pieces[sh.portal];
    assert(pr->type == SH_PORTAL_ROOM && pr->facing == SH_NORTH && pr->depth == 6);
    assert(pr->x0 == 148 && pr->y0 == -33 && pr->z0 == -361);
    assert(pr->x1 == 158 && pr->y1 == -26 && pr->z1 == -346);

    PortalPlan pp;
    assert(getPortalPlan(&pp, &sh));
    assert(pp.center.x == 153 && pp.center.y == -30 && pp.center.z == -356);
    // frames in draw order: north row (z = maxZ - 8), south row, east, west
    assert(pp.frame[0].x == 152 && pp.frame[0].z == -354);
    assert(pp.frame[3].x == 152 && pp.frame[3].z == -358);
    assert(pp.frame[6].x == 151 && pp.frame[6].z == -355);
    assert(pp.frame[9].x == 155 && pp.frame[9].z == -355);
    for (int i = 0; i < 12; i++)
        assert(pp.frame[i].y == -30);
}

static void testEyeHelpers(void)
{
    static PortalJumpTable jt;
    uint64_t r = 99;
    initPortalJumpTable(&jt);
    for (int k = 0; k < 2000; k++)
    {
        Xoroshiro a, b, t;
        xSetSeed(&a, splitmix64(&r));
        b = t = a;
        xSkipPortalRoomT(&jt, &t);
        // the jump matrix equals 760 single steps
        xSkipPortalRoom(&a);
        xSkipN(&b, PORTAL_ROOM_CALLS_BEFORE_EYES);
        assert(a.lo == b.lo && a.hi == b.hi);
        assert(t.lo == b.lo && t.hi == b.hi);
        // the integer eye test equals vanilla's nextFloat() > 0.9F
        Xoroshiro c = a;
        uint16_t m = drawPortalEyes(&a);
        for (int i = 0; i < 12; i++)
            assert((m >> i & 1) == (xNextFloat(&c) > 0.9F));
    }
    // threshold boundary: 15099494 * 2^-24 == 0.9F
    assert((float)(PORTAL_EYE_THRESHOLD * 5.9604645E-8F) == 0.9F);
    assert((float)((PORTAL_EYE_THRESHOLD + 1) * 5.9604645E-8F) > 0.9F);
}

static void testExactness(void)
{
    // An exact chunk has no RNG calls before the portal room's own.
    static Stronghold sh;
    uint64_t r = 7;
    int exactChunks = 0, chunks = 0;
    for (int k = 0; k < 2000; k++)
    {
        uint64_t seed = splitmix64(&r);
        assert(getStrongholdLayout(&sh, MC_26_3, seed, (int)(seed >> 50) % 300, (int)(seed >> 20) % 300));
        PortalPlan pp;
        assert(getPortalPlan(&pp, &sh));
        uint16_t all = 0;
        for (int j = 0; j < pp.nchunks; j++)
        {
            const PortalChunk *pc = &pp.chunks[j];
            assert(!(all & pc->frames));
            all |= pc->frames;
            chunks++;
            if (pc->exact)
            {
                exactChunks++;
                assert(pc->wallCells == 0 && pc->skip == PORTAL_ROOM_CALLS_BEFORE_EYES);
                assert(pp.exact & pc->frames);
            }
            else
                assert(pc->wallCells > 0 && !(pp.exact & pc->frames));
        }
        assert(all == 0xfff);
        uint16_t op = 0;
        for (int j = 0; j < pp.nchunks; j++)
        {
            assert(!(op & pp.chunks[j].opening));
            op |= pp.chunks[j].opening;
        }
        assert(op == 0x1ff);
        // every frame chunk is within 8 chunks of the start, so vanilla
        // creates a structure reference there (createReferences range)
        for (int i = 0; i < 12; i++)
        {
            assert(abs((pp.frame[i].x >> 4) - sh.chunkX) <= 8);
            assert(abs((pp.frame[i].z >> 4) - sh.chunkZ) <= 8);
        }
        assert(pp.nchunks >= 1 && pp.nchunks <= 4); // 3 if a corner cell is cut off
        // evalPortalEyes agrees with the plan, exactOnly drops non-exact chunks
        uint16_t e = evalPortalEyes(&pp, seed, 0);
        assert((evalPortalEyes(&pp, seed, 1) & ~pp.exact) == 0);
        assert(evalPortalEyes(&pp, seed, 1) == (e & pp.exact));
        EndPortal ep;
        assert(getEndPortal(&ep, NULL, MC_26_3, seed, sh.chunkX, sh.chunkZ));
        assert(ep.eyes == e && ep.exact == pp.exact);
        assert(ep.lit == (ep.portalBlocks == 0x1ff));
    }
    assert(exactChunks > 0 && exactChunks < chunks);
}

static void testVersions(void)
{
    static Stronghold sh;
    int idx, step;
    assert(getStrongholdLayout(&sh, MC_1_17_1, 1, 0, 0) == 0);
    assert(!getStrongholdFeatureParams(MC_1_17_1, &idx, &step));
    for (int mc = MC_1_18; mc <= MC_26_3; mc++)
    {
        assert(getStrongholdFeatureParams(mc, &idx, &step));
        // the layout does not depend on the version within 1.18 .. 26.3
        assert(getStrongholdLayout(&sh, mc, 12345, 10, -20) == 102);
    }
    // step and index per version, established by running each version's
    // registry loading (see docs/portal-eyes.md)
    assert(getStrongholdFeatureParams(MC_1_18_2, &idx, &step) && step == 5 && idx == 0);
    assert(getStrongholdFeatureParams(MC_1_19_2, &idx, &step) && step == 4 && idx == 8);
    assert(getStrongholdFeatureParams(MC_1_19_4, &idx, &step) && step == 4 && idx == 19);
    assert(getStrongholdFeatureParams(MC_1_21_1, &idx, &step) && step == 4 && idx == 19);
    assert(getStrongholdFeatureParams(MC_26_2, &idx, &step) && step == 4 && idx == 19);
    assert(getStrongholdFeatureParams(MC_26_3, &idx, &step) && step == 4 && idx == 37);
    assert(getStrongholdDecoSeed(MC_26_3, 12345, 9, -23) ==
        getPopulationSeed(MC_26_3, 12345, 144, -368) + 37 + 40000);
}

static void testIterator(void)
{
    static Generator g;
    setupGenerator(&g, MC_26_3, 0);
    applySeed(&g, DIM_OVERWORLD, 12345);
    StrongholdIter it;
    initFirstStronghold(&it, MC_26_3, 12345);
    for (int i = 0; i < 3; i++)
    {
        EndPortal ep;
        Pos sc;
        nextStronghold(&it, &g);
        assert(getEndPortalOfStronghold(&ep, &g, i, &sc));
        assert(sc.x == it.pos.x >> 4 && sc.z == it.pos.z >> 4);
        assert(abs((ep.center.x >> 4) - sc.x) <= 8 && abs((ep.center.z >> 4) - sc.z) <= 8);
        assert(ep.eyeCount >= ep.exactEyeCount && ep.eyeCount <= 12);
    }
    // a world has strongholds 0 .. 127 only
    {
        EndPortal ep;
        static Generator g1;
        setupGenerator(&g1, MC_1_21_1, 0);
        applySeed(&g1, DIM_OVERWORLD, 12345);
        assert(getEndPortalOfStronghold(&ep, &g1, 127, NULL));
        assert(!getEndPortalOfStronghold(&ep, &g1, 128, NULL));
        assert(!getEndPortalOfStronghold(&ep, &g1, -1, NULL));
    }
}

static void bench(void)
{
    static Stronghold sh;
    static PortalPlan plans[256];
    uint64_t r = 1;
    int N = 100000, nplans = 0, exactPlan = -1;
    long pieces = 0;
    clock_t t0 = clock();
    for (int k = 0; k < N; k++)
    {
        uint64_t seed = splitmix64(&r);
        pieces += getStrongholdLayout(&sh, MC_26_3, seed, (int)(seed >> 50) % 300, (int)(seed >> 20) % 300);
        if (nplans < 256)
        {
            getPortalPlan(&plans[nplans], &sh);
            if (exactPlan < 0 && plans[nplans].exact == 0xfff && plans[nplans].nchunks == 1)
                exactPlan = nplans;
            nplans++;
        }
    }
    double tl = (double)(clock() - t0) / CLOCKS_PER_SEC / N;

    int M = 2000000;
    unsigned acc = 0;
    long nch = 0;
    t0 = clock();
    for (int k = 0; k < M; k++)
    {
        const PortalPlan *pp = &plans[k & 255];
        nch += pp->nchunks;
        acc += evalPortalEyes(pp, splitmix64(&r), 0);
    }
    double te = (double)(clock() - t0) / CLOCKS_PER_SEC / M;

    double tx = 0;
    if (exactPlan >= 0)
    {
        t0 = clock();
        for (int k = 0; k < M; k++)
            acc += evalPortalEyes(&plans[exactPlan], splitmix64(&r), 1);
        tx = (double)(clock() - t0) / CLOCKS_PER_SEC / M;
    }

    Xoroshiro xr = {1, 2};
    t0 = clock();
    for (int k = 0; k < M; k++)
    {
        xr.lo += k;
        xSkipPortalRoom(&xr);
        acc += drawPortalEyes(&xr);
    }
    double tj = (double)(clock() - t0) / CLOCKS_PER_SEC / M;

    static PortalJumpTable jt;
    initPortalJumpTable(&jt);
    t0 = clock();
    for (int k = 0; k < M; k++)
    {
        xr.lo += k;
        xSkipPortalRoomT(&jt, &xr);
        acc += drawPortalEyes(&xr);
    }
    double tt = (double)(clock() - t0) / CLOCKS_PER_SEC / M;

    printf("layout generation: %.2f us/start (%.1f pieces avg)\n", tl * 1e6, (double) pieces / N);
    printf("evalPortalEyes, 256 random plans (%.2f chunks avg, incl. best-effort skips): %.1f ns/seed\n",
        (double) nch / M, te * 1e9);
    if (exactPlan >= 0)
        printf("evalPortalEyes, fully exact single-chunk plan: %.1f ns/seed\n", tx * 1e9);
    printf("xSkipPortalRoom + drawPortalEyes: %.1f ns; with nibble table: %.1f ns [%u]\n",
        tj * 1e9, tt * 1e9, acc & 1);
}

int main(int argc, char **argv)
{
    testLayoutVector();
    testEyeHelpers();
    testExactness();
    testVersions();
    testIterator();
    // 1000 random starts, compared against the vanilla 26.3 harness output
    assert(hashLayouts(MC_26_3, 1000, 2024) == LAYOUT_HASH_26_3);
    if (argc > 1 && strcmp(argv[1], "-bench") == 0)
        bench();
    printf("stronghold tests passed\n");
    return 0;
}
