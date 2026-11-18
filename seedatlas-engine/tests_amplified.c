/* Tests for the world type Amplified (1.18+): terrain splines, the terrain
 * density and its surface, the biomes, and the consumers of heights.
 *
 * Usage: test-amplified [worldgen data dir]
 *        e.g. ./test-amplified ../tools/worldgen-reference/data
 *
 * Without an argument only the built-in checks run. With the directory of the
 * real-server reference data (tools/worldgen-reference, files
 * amplified_<mc>_<seed>.txt and normal_<mc>_<seed>.txt) the heights and biomes
 * of the engine are compared with it as well; the default worlds are the
 * control for what the same method reaches without the Amplified flag.
 *
 * The spline numbers are float32 evaluations of the vanilla JSON splines
 * (density_function/overworld{,_amplified}/{offset,factor,jaggedness}.json of
 * 1.21.11, identical in 1.18 to 26.3), see docs/terrain_splines.py.
 */
#include "generator.h"
#include "finders.h"
#include "eyesearch.h"
#include "util.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; \
    printf("FAILED %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char*) p;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ULL;
    return h;
}
#define FNV_INIT 0xcbf29ce484222325ULL

//==============================================================================
// Splines
//==============================================================================

static void testSplines(void)
{
    // inputs: continentalness, erosion, folded ridges, weirdness
    // values: offset spline, factor, jaggedness; each default, amplified
    static const struct { float in[4], v[6]; } ref[] = {
    {{-1.37152636F, -0.350000024F, -0.926344275F, 0.0245519038F}, {0.0439999998F, 0.0879999995F, 3.95000005F, 3.95000005F, 0, 0}},
    {{-0.129763871F, 0.613107979F, 0.330892414F, -0.443630815F}, {0.00414659502F, 0.0140808821F, 5.9701252F, 4.24173927F, 0, 0}},
    {{1.3089509F, -0.992993057F, -0.989500046F, -0.00349997566F}, {0.348052591F, 0.696168065F, 5.51612902F, 0.652159989F, 0, 0}},
    {{-0.439999968F, -0.185709342F, -0.958473325F, -0.0138422297F}, {-0.119999997F, -0.119999997F, 3.95000005F, 3.95000005F, 0, 0}},
    {{-0.0229858123F, 0.0601425022F, -0.700078905F, 0.0500000007F}, {-0.00995366368F, -0.00680995313F, 5.55426693F, 0.656409502F, 0, 0}},
    {{1.30838954F, -0.942079484F, -1.39108205F, 0.103937484F}, {0.347926259F, 0.695852518F, 4.92397022F, 0.618360043F, 0, 0}},
    {{-0.151998773F, 1.0597254F, 0.424086839F, -0.474695623F}, {0.00133150397F, 0.00133150397F, 6.23334503F, 6.23334503F, 0, 0}},
    {{0.503817677F, -0.400000006F, -0.394020408F, 0.0560072772F}, {0.252102703F, 0.563541889F, 4.51866293F, 0.581078112F, 0, 0}},
    {{0.650000095F, -0.375F, -0.75000006F, 1.12385952F}, {0.14151372F, 0.34123528F, 4.54037046F, 0.592422366F, 0, 0}},
    {{1.02193797F, -0.950526655F, 0.62529707F, -0.54176569F}, {1.28415728F, 2.60213614F, 6.30000019F, 0.696902692F, 0.568250358F, 1.13650072F}},
    {{0.997905374F, -1.14044452F, 0.337897182F, 0.398687422F}, {1.11859918F, 2.19541693F, 4.69000006F, 0.605005205F, 0.173132598F, 0.346265197F}},
    {{-0.101483814F, -0.405877262F, 0.764467001F, -0.588155687F}, {0.0408830345F, 0.0771784037F, 6.30000019F, 0.711413383F, 0.000127583742F, 0.000255167484F}},
    {{0.437030405F, -0.703205824F, 0.741147876F, 0.752950728F}, {0.785897017F, 1.61228347F, 4.69000006F, 0.605005205F, 0.140927285F, 0.28185457F}},
    };
    Generator gd, ga;
    setupGenerator(&gd, MC_1_21_1, 0);
    setupGenerator(&ga, MC_1_21_1, AMPLIFIED);
    CHECK(!gd.bn.amplified && ga.bn.amplified, "BiomeNoise.amplified");
    size_t i;
    for (i = 0; i < sizeof(ref) / sizeof(*ref); i++)
    {
        const float *in = ref[i].in, *v = ref[i].v;
        float od = getSpline(gd.bn.sp, in);
        float oa = getSpline(ga.bn.sp, in);
        // the offset spline is built by code, within two float steps of the JSON
        CHECK(fabsf(od - v[0]) <= 4e-7F * fmaxf(1, fabsf(v[0])), "offset default %zu: %.9g != %.9g", i, od, v[0]);
        CHECK(fabsf(oa - v[1]) <= 4e-7F * fmaxf(1, fabsf(v[1])), "offset amplified %zu: %.9g != %.9g", i, oa, v[1]);
        // the tables are evaluated like the game: exact
        CHECK(getTerrainFactor(0, in) == v[2], "factor default %zu: %.9g != %.9g", i, getTerrainFactor(0, in), v[2]);
        CHECK(getTerrainFactor(1, in) == v[3], "factor amplified %zu: %.9g != %.9g", i, getTerrainFactor(1, in), v[3]);
        CHECK(getTerrainJaggedness(0, in) == v[4], "jaggedness default %zu: %.9g != %.9g", i, getTerrainJaggedness(0, in), v[4]);
        CHECK(getTerrainJaggedness(1, in) == v[5], "jaggedness amplified %zu: %.9g != %.9g", i, getTerrainJaggedness(1, in), v[5]);
    }

    // The transforms of TerrainProvider on random inputs: the amplified
    // jaggedness is exactly twice the default one (all leaves doubled, the
    // spline is linear in its values up to rounding); offset and factor are
    // only transformed in their leaves, so no such identity holds for them.
    uint64_t s = 12345;
    double maxj = 0;
    for (i = 0; i < 20000; i++)
    {
        float in[4];
        int k;
        for (k = 0; k < 4; k++)
            in[k] = nextFloat(&s) * 2.6F - 1.3F;
        float jd = getTerrainJaggedness(0, in), ja = getTerrainJaggedness(1, in);
        double e = fabs(ja - 2.0 * jd);
        if (e > maxj) maxj = e;
        float fa = getTerrainFactor(1, in), fd = getTerrainFactor(0, in);
        CHECK(fa > 0.05F && fa <= 6.31F && fd > 0.5F && fd <= 6.31F, "factor out of range: %g %g", fd, fa);
        CHECK(in[0] >= -0.19F || (fa == 3.95F && fd == 3.95F && jd == 0 && ja == 0),
            "ocean factor / jaggedness: %g %g %g %g", fd, fa, jd, ja);
    }
    CHECK(maxj < 1e-5, "amplified jaggedness is not twice the default: %g", maxj);
    printf("splines: %s\n", g_fail ? "FAILED" : "ok");
}

//==============================================================================
// Terrain density and surface
//==============================================================================

// Highest solid cell corner by evaluating every corner in full.
static double bruteSurface(const BiomeNoise *bn, const TerrainNoise *tn, int x, int z)
{
    TerrainColumn tc;
    sampleTerrainColumn(bn, tn, x, z, 0, &tc);
    double v0 = 0, v1 = 0;
    int k;
    for (k = TERRAIN_K_MAX; k >= TERRAIN_K_MIN; k--)
    {
        v0 = sampleTerrainDensity(&tc, tn, k);
        if (v0 > 0)
            break;
        v1 = v0;
    }
    return k * 8.0 + 8.0 * v0 / (v0 - v1);
}

static void testTerrain(void)
{
    const uint64_t seed = 123456789;
    static TerrainNoise tn;
    Generator gd, ga;
    int fail0 = g_fail;
    setupGenerator(&gd, MC_1_21_1, 0);
    setupGenerator(&ga, MC_1_21_1, AMPLIFIED);
    applySeed(&gd, DIM_OVERWORLD, seed);
    applySeed(&ga, DIM_OVERWORLD, seed);
    initTerrainNoise(&tn, seed);
    const TerrainNoise *tt = getThreadTerrainNoise(seed);
    CHECK(tt == getThreadTerrainNoise(seed), "thread terrain noise is reseeded for the same seed");
    CHECK(sampleTerrainBaseNoise(tt, 3, 9, -7) == sampleTerrainBaseNoise(&tn, 3, 9, -7) &&
        sampleDoublePerlin(&tt->jagged, 6000, 0, -24000) == sampleDoublePerlin(&tn.jagged, 6000, 0, -24000),
        "thread terrain noise");

    // the slides: the bottom corners are solid, the top ones empty
    int i, j, k;
    TerrainColumn tc;
    sampleTerrainColumn(&ga.bn, &tn, 10, 20, 0, &tc);
    CHECK(sampleTerrainDensity(&tc, &tn, TERRAIN_K_MIN) == 0.4, "amplified bottom slide");
    CHECK(sampleTerrainDensity(&tc, &tn, TERRAIN_K_MAX) == -0.078125, "amplified top slide");
    sampleTerrainColumn(&gd.bn, &tn, 10, 20, 0, &tc);
    CHECK(sampleTerrainDensity(&tc, &tn, TERRAIN_K_MIN) == 0.1171875, "default bottom slide");
    for (k = 32; k <= TERRAIN_K_MAX; k++)
        CHECK(sampleTerrainDensity(&tc, &tn, k) == -0.078125, "default top slide at %d", k);

    // the fast surface scan against the full evaluation of every corner
    double maxe = 0;
    for (j = 0; j < 40; j++)
    {
        for (i = 0; i < 40; i++)
        {
            int x = i * 37 - 700, z = j * 41 - 900;
            double e = fabs(sampleTerrainSurface(&gd.bn, &tn, x, z, 0) - bruteSurface(&gd.bn, &tn, x, z));
            if (e > maxe) maxe = e;
            e = fabs(sampleTerrainSurface(&ga.bn, &tn, x, z, 0) - bruteSurface(&ga.bn, &tn, x, z));
            if (e > maxe) maxe = e;
            // on a cell corner column the block height is ceil() of the surface
            double s = sampleTerrainSurface(&ga.bn, &tn, x, z, 0);
            int h = getTerrainHeightAt(&ga.bn, &tn, x * 4, z * 4);
            CHECK(h == (int) ceil(s) || fabs(s - round(s)) < 1e-9, "block height %d, surface %.6f", h, s);
        }
    }
    CHECK(maxe < 1e-9, "surface scan differs from the full evaluation by %g", maxe);

    // between the columns the block height stays within the four corners' range
    for (i = 0; i < 300; i++)
    {
        int bx = i * 53 - 4000, bz = i * 29 + 100;
        int h = getTerrainHeightAt(&ga.bn, &tn, bx, bz);
        CHECK(h > -64 && h <= 320, "block height %d", h);
    }

    // mapApproxHeight() and mapReliefHeight(): the real surface with the flag,
    // and never the depth estimate
    static float ya[64*64], yr[64*64], yd[64*64];
    ReliefNoise rn;
    reliefNoiseInit(&rn);
    CHECK(mapApproxHeight(ya, NULL, &ga, NULL, -120, 300, 64, 64) == 0, "mapApproxHeight amplified");
    CHECK(mapReliefHeight(yr, &ga, NULL, &rn, -120, 300, 64, 64) == 0, "mapReliefHeight amplified");
    CHECK(memcmp(ya, yr, sizeof(ya)) == 0, "amplified relief and approx heights differ");
    CHECK(mapReliefHeight(yr, &ga, NULL, NULL, -120, 300, 64, 64) == 0 && memcmp(ya, yr, sizeof(ya)) == 0,
        "amplified relief heights without ReliefNoise");
    CHECK(rn.tn != NULL && rn.sn == NULL, "ReliefNoise of an Amplified world");
    reliefNoiseFree(&rn);
    CHECK(mapApproxHeight(yd, NULL, &gd, NULL, -120, 300, 64, 64) == 0, "mapApproxHeight default");
    long ndepth = 0, nsame = 0;
    for (i = 0; i < 64*64; i++)
    {
        int x = -120 + i % 64, z = 300 + i / 64;
        CHECK(ya[i] == (float) (sampleTerrainSurface(&ga.bn, &tn, x, z, 0) + 0.5), "amplified height at %d %d", x, z);
        // flag off: still exactly the depth estimate
        CHECK(yd[i] == (float) (sampleBiomeNoiseDepth(&gd.bn, x, 0, z, 0) / 76.0), "default height at %d %d", x, z);
        ndepth += ya[i] == (float) (sampleBiomeNoiseDepth(&ga.bn, x, 0, z, 0) / 76.0);
        nsame += ya[i] == yd[i];
    }
    CHECK(ndepth == 0 && nsame == 0, "amplified heights equal to a depth estimate: %ld %ld", ndepth, nsame);

    // the top slide (304..320): heights stay within the world. A peak column
    // with a large offset would be far above 320 by its depth alone.
    float ymax = -1e9F, ymin = 1e9F, dmax = -1e9F;
    for (j = 0; j < 12; j++)
    {
        applySeed(&ga, DIM_OVERWORLD, 1000 + j * 7919ULL);
        mapApproxHeight(ya, NULL, &ga, NULL, j * 4001 - 20000, 9000 - j * 2503, 64, 64);
        for (i = 0; i < 64*64; i++)
        {
            float d = (float) (sampleBiomeNoiseDepth(&ga.bn, j * 4001 - 20000 + i % 64, 0, 9000 - j * 2503 + i / 64, 0) / 76.0);
            if (ya[i] > ymax) ymax = ya[i];
            if (ya[i] < ymin) ymin = ya[i];
            if (d > dmax) dmax = d;
        }
    }
    CHECK(ymax < 320 && ymin >= -63.5F, "amplified heights outside of the world: %g .. %g", ymin, ymax);
    printf("terrain: amplified heights of 12 areas from %.1f to %.1f (the depth estimate would reach %.1f)\n", ymin, ymax, dmax);
    applySeed(&ga, DIM_OVERWORLD, seed);

    // regression hashes of amplified heights (quarter blocks) for a few seeds
    static const uint64_t seeds[] = { 1, 12345, 6027786629280759191ULL, (uint64_t) -4172144997902289642LL };
    uint64_t h_map = FNV_INIT, h_block = FNV_INIT, h_def = FNV_INIT;
    double sum = 0, sumd = 0; long n = 0, high = 0;
    size_t si;
    for (si = 0; si < sizeof(seeds) / sizeof(*seeds); si++)
    {
        applySeed(&ga, DIM_OVERWORLD, seeds[si]);
        applySeed(&gd, DIM_OVERWORLD, seeds[si]);
        initTerrainNoise(&tn, seeds[si]);
        int x0 = (int) (si * 1500) - 2000, z0 = 700 - (int) (si * 2100);
        mapApproxHeight(ya, NULL, &ga, NULL, x0, z0, 64, 64);
        for (i = 0; i < 64*64; i++)
        {
            int q = (int) lround(ya[i] * 4.0);
            h_map = fnv(h_map, &q, sizeof(q));
            sum += ya[i]; n++; high += ya[i] > 200;
            // the same method without the flag, as a hash of its own
            q = (int) lround(4.0 * sampleTerrainSurface(&gd.bn, &tn, x0 + i % 64, z0 + i / 64, 0));
            h_def = fnv(h_def, &q, sizeof(q));
            sumd += q / 4.0;
        }
        for (i = 0; i < 400; i++)
        {
            int h = getTerrainHeightAt(&ga.bn, &tn, x0 * 4 + i * 7, z0 * 4 + i * 3);
            h_block = fnv(h_block, &h, sizeof(h));
        }
    }
    printf("terrain: amplified mean height %.2f (default terrain %.2f), %.1f%% above y 200; hashes %016llx %016llx %016llx\n",
        sum / n, sumd / n, 100.0 * high / n,
        (unsigned long long) h_map, (unsigned long long) h_block, (unsigned long long) h_def);
    CHECK(h_map == 0x74d313ff8c16bbb9ULL, "amplified height hash %016llx", (unsigned long long) h_map);
    CHECK(h_block == 0x1837aa580092d582ULL, "amplified block height hash %016llx", (unsigned long long) h_block);
    CHECK(h_def == 0x9d7f368f83a6562cULL, "default terrain surface hash %016llx", (unsigned long long) h_def);
    printf("terrain: %s\n", g_fail == fail0 ? "ok" : "FAILED");
}

//==============================================================================
// Biomes and structures
//==============================================================================

static void testBiomes(void)
{
    static const uint64_t seeds[] = { 1, 12345, 6027786629280759191ULL };
    static const int mcs[] = { MC_1_18, MC_1_19_2, MC_1_21_1, MC_26_3 };
    static const int ys[] = { -15, 0, 16, 32, 64, 79 }; // 1:4, blocks -60 .. 316
    // expected hashes, one per version
    static const uint64_t expect[] = {
        0xb5e1aa37a7998600ULL, 0xd45733fe1fb3213fULL, 0xc8422e6df7774f47ULL, 0x58357494c0663c8dULL,
    };
    int fail0 = g_fail;
    size_t m, s, k;
    for (m = 0; m < sizeof(mcs) / sizeof(*mcs); m++)
    {
        Generator gd, ga;
        setupGenerator(&gd, mcs[m], 0);
        setupGenerator(&ga, mcs[m], AMPLIFIED);
        uint64_t h = FNV_INIT;
        long diff[6] = {0}, n = 0;
        for (s = 0; s < sizeof(seeds) / sizeof(*seeds); s++)
        {
            applySeed(&gd, DIM_OVERWORLD, seeds[s]);
            applySeed(&ga, DIM_OVERWORLD, seeds[s]);
            for (k = 0; k < 6; k++)
            {
                Range r = { 4, -300 + 500 * (int) k, 200 - 400 * (int) s, 80, 80, ys[k], 1 };
                int *ca = allocCache(&ga, r), *cd = allocCache(&gd, r);
                CHECK(genBiomes(&ga, ca, r) == 0 && genBiomes(&gd, cd, r) == 0, "genBiomes");
                h = fnv(h, ca, sizeof(int) * 80 * 80);
                int i;
                for (i = 0; i < 80 * 80; i++)
                    diff[k] += ca[i] != cd[i];
                free(ca);
                free(cd);
            }
            n += 80 * 80;
            // the five horizontal climate parameters do not depend on the flag
            int i;
            for (i = 0; i < 200; i++)
            {
                int64_t npa[6], npd[6];
                int x = i * 91 - 5000, z = i * 57 - 3000;
                sampleBiomeNoise(&ga.bn, npa, x, 16, z, NULL, 0);
                sampleBiomeNoise(&gd.bn, npd, x, 16, z, NULL, 0);
                CHECK(npa[0] == npd[0] && npa[1] == npd[1] && npa[2] == npd[2] &&
                    npa[3] == npd[3] && npa[5] == npd[5], "climate differs at %d %d", x, z);
                CHECK(npa[4] >= npd[4], "amplified depth below the default one at %d %d", x, z);
            }
        }
        printf("biomes %s: differ from the default world at y", mc2str(mcs[m]));
        for (k = 0; k < 6; k++)
            printf(" %d: %.1f%%", ys[k] * 4, 100.0 * diff[k] / n);
        printf("; hash %016llx\n", (unsigned long long) h);
        CHECK(h == expect[m], "amplified biome hash of %s: %016llx", mc2str(mcs[m]), (unsigned long long) h);
    }

    // The flag does nothing before 1.18.
    Generator g0, g1;
    setupGenerator(&g0, MC_1_17, 0);
    setupGenerator(&g1, MC_1_17, AMPLIFIED);
    applySeed(&g0, DIM_OVERWORLD, 12345);
    applySeed(&g1, DIM_OVERWORLD, 12345);
    Range r = { 4, -100, -100, 64, 64, 15, 1 };
    int *c0 = allocCache(&g0, r), *c1 = allocCache(&g1, r);
    genBiomes(&g0, c0, r);
    genBiomes(&g1, c1, r);
    CHECK(memcmp(c0, c1, sizeof(int) * 64 * 64) == 0, "the flag changes 1.17 biomes");
    free(c0);
    free(c1);
    printf("biomes: %s\n", g_fail == fail0 ? "ok" : "FAILED");
}

static void testStructures(void)
{
    // Terrain checks and structure positions under the flag: a regression
    // hash over the viability of the structures whose checks read heights.
    static const int sts[] = { Desert_Pyramid, Jungle_Temple, Mansion, Abandoned_Camp, Village };
    static const uint64_t seeds[] = { 12345, 6027786629280759191ULL };
    int fail0 = g_fail;
    Generator ga, gd;
    setupGenerator(&ga, MC_26_3, AMPLIFIED);
    setupGenerator(&gd, MC_26_3, 0);
    uint64_t h = FNV_INIT;
    long n = 0, viable = 0, terrain = 0, dviable = 0, dterrain = 0, tdiff = 0;
    size_t s, t;
    for (s = 0; s < sizeof(seeds) / sizeof(*seeds); s++)
    {
        applySeed(&ga, DIM_OVERWORLD, seeds[s]);
        applySeed(&gd, DIM_OVERWORLD, seeds[s]);
        for (t = 0; t < sizeof(sts) / sizeof(*sts); t++)
        {
            StructureConfig sc;
            if (!getStructureConfig(sts[t], MC_26_3, &sc))
                continue;
            int rx, rz;
            for (rz = -12; rz < 12; rz++)
            {
                for (rx = -12; rx < 12; rx++)
                {
                    Pos p;
                    if (!getStructurePos(sts[t], MC_26_3, seeds[s], rx, rz, &p))
                        continue;
                    int v = isViableStructurePos(sts[t], &ga, p.x, p.z, 0);
                    int tv = v ? isViableStructureTerrain(sts[t], &ga, p.x, p.z) : -1;
                    int dv = isViableStructurePos(sts[t], &gd, p.x, p.z, 0);
                    int dt = dv ? isViableStructureTerrain(sts[t], &gd, p.x, p.z) : -1;
                    h = fnv(h, &v, sizeof(v));
                    h = fnv(h, &tv, sizeof(tv));
                    n++;
                    viable += v != 0; terrain += tv > 0;
                    dviable += dv != 0; dterrain += dt > 0;
                    tdiff += v && dv && tv != dt;
                }
            }
        }
        // the spawn search runs on amplified heights
        Pos sp = getSpawn(&ga);
        h = fnv(h, &sp, sizeof(sp));
    }
    printf("structures: %ld candidates; amplified %ld biome-viable, %ld pass the terrain; default %ld and %ld; terrain verdict differs for %ld; hash %016llx\n",
        n, viable, terrain, dviable, dterrain, tdiff, (unsigned long long) h);
    CHECK(h == 0x70d63f469d623edeULL, "structure hash %016llx", (unsigned long long) h);
    printf("structures: %s\n", g_fail == fail0 ? "ok" : "FAILED");
}

//==============================================================================
// Strongholds and the eye search
//==============================================================================

typedef struct { EyeSearchHit hit[64]; int n; } HitList;

static int collectHit(void *user, const EyeSearchHit *hit)
{
    HitList *hl = (HitList*) user;
    if (hl->n < 64)
        hl->hit[hl->n] = *hit;
    hl->n++;
    return 0;
}

static int cmpHit(const void *a, const void *b)
{
    const EyeSearchHit *x = (const EyeSearchHit*) a, *y = (const EyeSearchHit*) b;
    if (x->seed != y->seed)
        return x->seed < y->seed ? -1 : 1;
    return x->index - y->index;
}

static void testStrongholds(void)
{
    // The ring geometry is that of a default world, but the positions are
    // snapped to biomes sampled with the amplified depth.
    static const int mcs[] = { MC_1_18, MC_1_21_1, MC_26_3 };
    static const uint64_t expect[] = {
        0x5ddccf039cf7e27fULL, 0x4992bfd84e307ef6ULL, 0x9b5e4b4874213857ULL,
    };
    int fail0 = g_fail;
    size_t m;
    for (m = 0; m < sizeof(mcs) / sizeof(*mcs); m++)
    {
        Generator gd, ga;
        setupGenerator(&gd, mcs[m], 0);
        setupGenerator(&ga, mcs[m], AMPLIFIED);
        uint64_t h = FNV_INIT, seed = 0x5eed + m;
        long n = 0, diff = 0;
        int s, i;
        for (s = 0; s < 60; s++)
        {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            applySeed(&gd, DIM_OVERWORLD, seed);
            applySeed(&ga, DIM_OVERWORLD, seed);
            StrongholdIter itd, ita;
            Pos pd = initFirstStronghold(&itd, mcs[m], seed);
            Pos pa = initFirstStronghold(&ita, mcs[m], seed);
            CHECK(pd.x == pa.x && pd.z == pa.z, "approximate first stronghold differs");
            for (i = 0; i < 9; i++)
            {
                int md = nextStronghold(&itd, &gd);
                int ma = nextStronghold(&ita, &ga);
                h = fnv(h, &ita.pos, sizeof(ita.pos));
                n++;
                diff += itd.pos.x != ita.pos.x || itd.pos.z != ita.pos.z;
                // The snap moves a start by at most 112 blocks from its ring
                // position. (Up to 1.19.2 the snap draws from the random
                // that places the following strongholds, so there one
                // different snap moves all the later ones.)
                CHECK(mcs[m] <= MC_1_19_2 ||
                    (abs(itd.pos.x - ita.pos.x) <= 224 && abs(itd.pos.z - ita.pos.z) <= 224),
                    "stronghold %d of seed %lld moved too far", i, (long long) seed);
                if (md <= 0 || ma <= 0)
                    break;
            }
        }
        printf("strongholds %s: %ld of %ld positions (first 9 of 60 seeds) differ from the default world (%.1f%%); hash %016llx\n",
            mc2str(mcs[m]), diff, n, 100.0 * diff / n, (unsigned long long) h);
        CHECK(h == expect[m], "amplified stronghold hash of %s: %016llx", mc2str(mcs[m]), (unsigned long long) h);
        CHECK(diff > 0, "no stronghold position depends on the flag");
    }

    // Eye search: the fast scan against the reference scan with the flag, and
    // every hit against the strongholds of an Amplified generator.
    EyeSearchConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mc = MC_1_21_1;
    cfg.numStrongholds = 3;
    cfg.minEyes = 3;
    cfg.amplified = 1;
    EyeSearch *es = createEyeSearch(&cfg);
    CHECK(es != NULL, "createEyeSearch with the Amplified flag");
    if (es)
    {
        static HitList fast, ref;
        const uint64_t low = 0x00007a3b1c2d3e4fULL;
        fast.n = ref.n = 0;
        int nf = scanEyeFamilyRange(es, low, 30000, 30192, collectHit, &fast, NULL);
        int nr = scanEyeFamilyReferenceRange(es, low, 30000, 30192, collectHit, &ref, NULL);
        CHECK(nf == nr && nf == fast.n && nf > 0 && nf <= 64, "eye search hits: fast %d, reference %d", nf, nr);
        if (nf == nr && nf > 0 && nf <= 64)
        {
            qsort(fast.hit, nf, sizeof(EyeSearchHit), cmpHit);
            qsort(ref.hit, nf, sizeof(EyeSearchHit), cmpHit);
            Generator ga;
            setupGenerator(&ga, cfg.mc, AMPLIFIED);
            int i, k;
            for (i = 0; i < nf; i++)
            {
                const EyeSearchHit *a = &fast.hit[i], *b = &ref.hit[i];
                CHECK(a->seed == b->seed && a->index == b->index &&
                    a->startChunk.x == b->startChunk.x && a->startChunk.z == b->startChunk.z,
                    "eye search hit %d differs from the reference scan", i);
                applySeed(&ga, DIM_OVERWORLD, a->seed);
                StrongholdIter it;
                initFirstStronghold(&it, cfg.mc, a->seed);
                for (k = 0; k <= a->index; k++)
                    nextStronghold(&it, &ga);
                CHECK((it.pos.x >> 4) == a->startChunk.x && (it.pos.z >> 4) == a->startChunk.z,
                    "eye search hit %d is not at the Amplified stronghold", i);
            }
        }
        printf("eye search (Amplified): %d hits in 192 seeds, fast and reference scan agree\n", nf);
        freeEyeSearch(es);
    }
    printf("strongholds: %s\n", g_fail == fail0 ? "ok" : "FAILED");
}

//==============================================================================
// Real-server reference data (tools/worldgen-reference)
//==============================================================================

static int cmpInt(const void *a, const void *b)
{
    return *(const int*) a - *(const int*) b;
}

typedef struct
{
    long n, exact, within2, within8;
    double sumabs, sum;
    int median, p90;
    long nb, okb;
    long nby[10], okby[10];
} RefStats;

// the y levels of the B lines
static const int g_ref_ys[10] = { -60, -28, 0, 32, 64, 96, 128, 192, 256, 316 };

static int checkReferenceFile(const char *path, RefStats *st, int *mcOut, int *ampOut)
{
    const int *ys = g_ref_ys;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    char line[512], mcs[32] = "", lt[32] = "";
    long long seed = 0;
    int ready = 0, mc = -1, amp = 0;
    static Generator g;
    static TerrainNoise tn;
    int *err = NULL;
    long cap = 0;
    memset(st, 0, sizeof(*st));
    while (fgets(line, sizeof(line), fp))
    {
        if (line[0] == '#')
        {
            sscanf(line, "# mc %31s", mcs);
            sscanf(line, "# level_type %31s", lt);
            sscanf(line, "# seed %lld", &seed);
            continue;
        }
        if (!ready)
        {
            mc = str2mc(mcs);
            amp = strcmp(lt, "amplified") == 0;
            if (mc < MC_1_18 || (!amp && strcmp(lt, "normal") != 0))
                break;
            setupGenerator(&g, mc, amp ? AMPLIFIED : 0);
            applySeed(&g, DIM_OVERWORLD, (uint64_t) seed);
            initTerrainNoise(&tn, (uint64_t) seed);
            ready = 1;
        }
        int x, y, z, ws, of;
        char name[128];
        if (sscanf(line, "H %d %d %d %d", &x, &z, &ws, &of) == 4)
        {   // the y of the highest motion-blocking block of a full chunk
            int e = getTerrainHeightAt(&g.bn, &tn, x, z) - 1 - of;
            if (st->n == cap)
            {
                cap = cap ? 2 * cap : 65536;
                err = (int*) realloc(err, cap * sizeof(int));
            }
            err[st->n++] = abs(e);
            st->sumabs += abs(e);
            st->sum += e;
            st->exact += e == 0;
            st->within2 += abs(e) <= 2;
            st->within8 += abs(e) <= 8;
        }
        else if (sscanf(line, "B %d %d %d %127s", &x, &y, &z, name) == 4)
        {
            const char *s = biome2str(mc, getBiomeAt(&g, 4, x >> 2, y >> 2, z >> 2));
            int ok = s && strcmp(s, name) == 0, k;
            st->nb++;
            st->okb += ok;
            for (k = 0; k < 10; k++)
            {
                if (ys[k] == y)
                {
                    st->nby[k]++;
                    st->okby[k] += ok;
                }
            }
        }
    }
    fclose(fp);
    if (st->n)
    {
        qsort(err, st->n, sizeof(int), cmpInt);
        st->median = err[st->n / 2];
        st->p90 = err[st->n * 9 / 10];
    }
    free(err);
    *mcOut = mc;
    *ampOut = amp;
    return ready && st->n > 0;
}

static void testReference(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int files = 0, fail0 = g_fail;
    if (!d)
    {
        printf("reference data directory '%s' not found\n", dir);
        g_fail++;
        return;
    }
    printf("reference data (heights: y of the top block against the vanilla heightmap OCEAN_FLOOR;\n"
           "the engine has no trees, carvers, cave entrances or structures, which the data has)\n");
    while ((e = readdir(d)) != NULL)
    {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if ((strncmp(n, "amplified_", 10) != 0 && strncmp(n, "normal_", 7) != 0) ||
            len < 5 || strcmp(n + len - 4, ".txt") != 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, n);
        RefStats st;
        int mc, amp, k;
        if (!checkReferenceFile(path, &st, &mc, &amp))
        {
            printf("%s: skipped\n", n);
            continue;
        }
        files++;
        printf("%-48s heights n=%ld: exact %.1f%%, within 2: %.1f%%, within 8: %.1f%%, mean abs %.2f, median %d, p90 %d, bias %+.2f;"
            " biomes %ld / %ld (%.3f%%)",
            n, st.n, 100.0 * st.exact / st.n, 100.0 * st.within2 / st.n, 100.0 * st.within8 / st.n,
            st.sumabs / st.n, st.median, st.p90, st.sum / st.n,
            st.okb, st.nb, st.nb ? 100.0 * st.okb / st.nb : 0);
        if (st.okb != st.nb)
        {
            printf(", differing cells at y");
            for (k = 0; k < 10; k++)
                if (st.nby[k] != st.okby[k])
                    printf(" %d: %ld", g_ref_ys[k], st.nby[k] - st.okby[k]);
        }
        printf("\n");
        // Limits with a margin to what the reference worlds reach (exact in
        // 70 to 83% of the columns; most of the rest is trees). A wrong
        // spline, slide or noise drops the exact share to a few percent.
        CHECK(st.median == 0, "%s: median height error %d", n, st.median);
        CHECK(st.exact >= 0.6 * st.n, "%s: only %.1f%% of the heights exact", n, 100.0 * st.exact / st.n);
        CHECK(st.sumabs / st.n < 4.0, "%s: mean height error %.2f", n, st.sumabs / st.n);
        CHECK(fabs(st.sum / st.n) < 1.5, "%s: height bias %.2f", n, st.sum / st.n);
        // biomes: single cells differ in default worlds as well (the stored
        // biomes depend on the lookup order within a chunk, MC-241546)
        CHECK(st.nb == 0 || st.okb >= 0.998 * st.nb, "%s: biomes %ld / %ld", n, st.okb, st.nb);
    }
    closedir(d);
    CHECK(files > 0, "no amplified_*.txt or normal_*.txt files in '%s'", dir);
    printf("reference data: %d files, %s\n", files, g_fail == fail0 ? "ok" : "FAILED");
}

// End biomes at 1:1 against the server's answers per block ("Z" lines of the
// files endzoom_*.txt): the voronoi zoom at the block's own y, read point by
// point and from a small map.
static void testEndZoom(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int files = 0;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL)
    {
        const char *n = e->d_name;
        size_t len = strlen(n);
        if (strncmp(n, "endzoom_", 8) != 0 || len < 5 || strcmp(n + len - 4, ".txt") != 0)
            continue;
        char path[1024], line[512], name[128], mcs[32] = "";
        snprintf(path, sizeof(path), "%s/%s", dir, n);
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;
        uint64_t seed = 0;
        int ready = 0, cnt = 0, bad = 0, badmap = 0, x, y, z;
        Generator g;
        while (fgets(line, sizeof(line), fp))
        {
            if (sscanf(line, "# mc %31s", mcs) == 1 || sscanf(line, "# seed %" SCNu64, &seed) == 1)
                continue;
            if (sscanf(line, "Z %d %d %d minecraft:%127s", &x, &y, &z, name) != 4)
                continue;
            if (!ready)
            {
                int mc = str2mc(mcs);
                CHECK(mc >= 0, "%s: unknown version '%s'", n, mcs);
                if (mc < 0)
                    break;
                setupGenerator(&g, mc, 0);
                applySeed(&g, DIM_END, seed);
                ready = 1;
            }
            cnt++;
            if (strcmp(biome2str(g.mc, getBiomeAt(&g, 1, x, y, z)), name) != 0)
                bad++;
            Range r = {1, x - 2, z - 2, 5, 5, y, 1};
            int *c = allocCache(&g, r);
            genBiomes(&g, c, r);
            if (strcmp(biome2str(g.mc, c[2 * 5 + 2]), name) != 0)
                badmap++;
            free(c);
        }
        fclose(fp);
        files++;
        printf("%-48s End at 1:1: %d blocks, %d differ, %d differ in maps\n", n, cnt, bad, badmap);
        CHECK(cnt > 0 && bad == 0 && badmap == 0, "%s: End biomes at 1:1 differ", n);
    }
    closedir(d);
    CHECK(files > 0, "no endzoom_*.txt files in '%s'", dir);
}

// The jagged noise far out: the game wraps the input of every octave
// (PerlinNoise.wrap); without it the finer octaves' lattice index leaves
// the range of an int beyond about 2.8 million blocks.
static double wrapRef(double d)
{
    return d - floor(d / 33554432.0 + 0.5) * 33554432.0;
}

static double jaggedRef(const DoublePerlinNoise *n, double x, double z)
{
    const double f = 337.0 / 331.0;
    double a = 0, b = 0;
    int i;
    for (i = 0; i < n->octA.octcnt; i++)
    {
        const PerlinNoise *p = n->octA.octaves + i;
        a += p->amplitude * samplePerlin(p, wrapRef(x * p->lacunarity), 0, wrapRef(z * p->lacunarity), 0, 0);
    }
    for (i = 0; i < n->octB.octcnt; i++)
    {
        const PerlinNoise *p = n->octB.octaves + i;
        b += p->amplitude * samplePerlin(p, wrapRef(x * f * p->lacunarity), 0, wrapRef(z * f * p->lacunarity), 0, 0);
    }
    return (a + b) * n->amplitude;
}

static void testFarJagged(void)
{
    const uint64_t seed = 12345;
    static TerrainNoise tn;
    Generator ga;
    setupGenerator(&ga, MC_1_21_1, AMPLIFIED);
    applySeed(&ga, DIM_OVERWORLD, seed);
    initTerrainNoise(&tn, seed);
    // quart positions near 3, 13 and 29.9 million blocks, both signs
    static const int qs[] = { 750000, -750000, 3250000, -3250000, 7475000, -7475000, 1000, -1000 };
    int n = 0, bad = 0, i, j, k;
    for (i = 0; i < 8; i++)
    {
        for (j = 0; j < 8; j++)
        {
            for (k = 0; k < 40; k++)
            {
                const int qx = qs[i] + k * 7, qz = qs[j] - k * 11;
                TerrainColumn tc;
                sampleTerrainColumn(&ga.bn, &tn, qx, qz, 0, &tc);
                if (tc.jaggedness == 0)
                    continue;
                double jv = jaggedRef(&tn.jagged, qx * 4 * 1500.0, qz * 4 * 1500.0);
                double want = tc.jaggedness * (jv > 0 ? jv : jv * 0.5);
                n++;
                if (tc.jagged != want)
                    bad++;
            }
        }
    }
    CHECK(n > 0, "no jagged columns far out");
    CHECK(bad == 0, "jagged noise far out: %d of %d columns differ from the wrapped noise", bad, n);
    printf("jagged noise up to 29.9 million blocks: %d columns, %d differ\n", n, bad);
}

int main(int argc, char **argv)
{
    testSplines();
    testTerrain();
    testFarJagged();
    testBiomes();
    testStructures();
    testStrongholds();
    if (argc > 1)
    {
        testReference(argv[1]);
        testEndZoom(argv[1]);
    }
    else
        printf("reference data: not checked (pass the directory tools/worldgen-reference/data)\n");
    printf(g_fail ? "amplified tests FAILED (%d)\n" : "amplified tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
