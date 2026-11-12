#include "biomenoise.h"
#include "biomes.h"
#include "finders.h"
#include "quadbase.h"
#include "util.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if STRUCT_CONFIG_OVERRIDE
/* The application always compiles the engine with STRUCT_CONFIG_OVERRIDE=1 and
 * provides getStructureConfig_override() itself (see src/main.cpp, where the
 * user's salt overrides are applied). The regression tests exercise the stock
 * configurations, so route the override back to the regular lookup. */
int getStructureConfig_override(int stype, int mc, StructureConfig *sconf)
{
    return getStructureConfig(stype, mc, sconf);
}
#endif

static void assertStructureConfig(int stype, int mc, int spacing,
                                  int separation, int salt)
{
    StructureConfig config;
    assert(getStructureConfig(stype, mc, &config));
    assert(config.regionSize == spacing);
    assert(config.chunkRange == spacing - separation);
    assert(config.salt == salt);
}

static void assertStructurePos(int stype, int mc, uint64_t seed,
                               int regX, int regZ, int x, int z)
{
    Pos pos;
    assert(getStructurePos(stype, mc, seed, regX, regZ, &pos));
    assert(pos.x == x && pos.z == z);
}

/* getBiomeClimateBounds(): wherever a climate parameter is outside the bounds
 * of a biome, climateToBiome() must not return it. Random climate points, and
 * points just beyond each bound. */
static void testBiomeClimateBounds(void)
{
    static const int mcs[] = { MC_1_18, MC_1_21_5, MC_26_3 };
    uint64_t rng = 88172645463325252ULL;
    for (size_t v = 0; v < sizeof(mcs) / sizeof(mcs[0]); v++)
    {
        for (int id = 0; id < 256; id++)
        {
            uint64_t set[4] = { 0 };
            set[id >> 6] |= 1ULL << (id & 63);
            BiomeClimateBounds b;
            if (!getBiomeClimateBounds(&b, mcs[v], set))
                continue;
            for (int it = 0; it < 4000; it++)
            {
                int64_t np[6];
                for (int k = 0; k < 6; k++)
                {
                    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                    np[k] = (int64_t)(rng % 50001) - 25000;
                }
                int k = it % 6;
                if (k != NP_DEPTH && (it & 1))
                {
                    int64_t d = 1 + (int64_t)(rng >> 40) % 40;
                    if ((it & 2) && b.hi[k] != INT32_MAX)
                        np[k] = (int64_t) b.hi[k] + d;
                    else if (b.lo[k] != INT32_MIN)
                        np[k] = (int64_t) b.lo[k] - d;
                }
                if (climateToBiome(mcs[v], (const uint64_t*) np, NULL) != id)
                    continue;
                for (k = 0; k < 6; k++)
                    assert(k == NP_DEPTH || (np[k] >= b.lo[k] && np[k] <= b.hi[k]));
            }
        }
    }
    // the example from the parameter tree: Mushroom Fields only below -1.05
    uint64_t mushroom[4] = { 1ULL << mushroom_fields, 0, 0, 0 };
    BiomeClimateBounds b;
    assert(getBiomeClimateBounds(&b, MC_26_3, mushroom));
    assert(b.hi[NP_CONTINENTALNESS] == -10500);
    assert(!getBiomeClimateBounds(&b, MC_1_17, mushroom));
}

// Every structure position that isViableStructurePos() accepts passes one of
// the checks of getStructureBiomeChecks() with the climate bounds of its set.
static void testStructureBiomeChecks(void)
{
    static const int mcs[] = { MC_1_18, MC_1_21_5, MC_26_3 };
    int tested = 0;
    static const int sts[] = {
        Desert_Pyramid, Jungle_Pyramid, Swamp_Hut, Igloo, Village, Ocean_Ruin,
        Shipwreck, Monument, Mansion, Outpost, Treasure, Desert_Well,
        Trail_Ruins, Ancient_City, Trial_Chambers, Abandoned_Camp,
    };
    for (size_t v = 0; v < sizeof(mcs) / sizeof(mcs[0]); v++)
    {
        Generator g;
        setupGenerator(&g, mcs[v], 0);
        for (uint64_t seed = 1; seed <= 6; seed++)
        {
            applySeed(&g, DIM_OVERWORLD, seed * 0x9E3779B97F4A7C15ULL);
            for (size_t t = 0; t < sizeof(sts) / sizeof(sts[0]); t++)
            {
                StructureConfig sc;
                if (!getStructureConfig(sts[t], mcs[v], &sc))
                    continue;
                for (int r = -6; r < 6; r++)
                {
                    Pos p;
                    if (!getStructurePos(sts[t], mcs[v], g.seed, r, r * 3 % 7, &p))
                        continue;
                    if (!isViableStructurePos(sts[t], &g, p.x, p.z, 0))
                        continue;
                    StructureBiomeCheck ck[5];
                    int n = getStructureBiomeChecks(sts[t], mcs[v], g.seed, p.x, p.z, ck);
                    int ok = n == 0;
                    for (int i = 0; i < n && !ok; i++)
                    {
                        BiomeClimateBounds b;
                        if (!getBiomeClimateBounds(&b, mcs[v], ck[i].biomes))
                        {
                            ok = 1;
                            break;
                        }
                        double px, pz;
                        getBiomeNoisePos(&g.bn, ck[i].x, ck[i].z, 0, &px, &pz);
                        ok = 1;
                        for (int k = 0; k < NP_MAX; k++)
                        {
                            if (k == NP_SHIFT)
                                continue;
                            int64_t c = sampleBiomeNoisePara(&g.bn, k, px, pz);
                            if (c < b.lo[k] || c > b.hi[k])
                                ok = 0;
                        }
                    }
                    assert(ok);
                    tested += n > 0;
                }
            }
        }
    }
    assert(tested > 100); // 447 when written
}

// checkForBiomes() gives the same results with the climate bounds of the
// filter, which only let it skip positions whose biome does not matter.
static void testBiomeFilterClimateBounds(void)
{
    static const int mcs[] = { MC_1_18, MC_1_21_5, MC_26_3 };
    static const int req[][2] = {
        { river, -1 }, { lukewarm_ocean, -1 }, { mushroom_fields, -1 },
        { jungle, desert }, { ice_spikes, -1 }, { dark_forest, -1 },
    };
    uint64_t rng = 0x2545F4914F6CDD1DULL;
    int differ = 0, skipping = 0;
    for (size_t v = 0; v < sizeof(mcs) / sizeof(mcs[0]); v++)
    {
        Generator g;
        setupGenerator(&g, mcs[v], 0);
        for (size_t f = 0; f < sizeof(req) / sizeof(req[0]); f++)
        {
            // the biomes as required ones, then as excluded ones
            int n = req[f][1] < 0 ? 1 : 2;
            BiomeFilter plain, bounded, exclP, exclB;
            setupBiomeFilter(&plain, mcs[v], 0, req[f], n, 0, 0, 0, 0);
            bounded = plain;
            setBiomeFilterClimateBounds(&bounded, mcs[v]);
            setupBiomeFilter(&exclP, mcs[v], 0, 0, 0, req[f], n, 0, 0);
            exclB = exclP;
            setBiomeFilterClimateBounds(&exclB, mcs[v]);
            skipping += bounded.ncbpara > 0;
            for (int it = 0; it < 24; it++)
            {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                uint64_t seed = rng;
                int scale = (it & 3) == 3 ? 16 : 4;
                int w = 1 + (int)(rng >> 58) % 12, h = 1 + (int)(rng >> 52) % 12;
                Range r = { scale, (int)(rng >> 40) % 2000 - 1000, (int)(rng >> 30) % 2000 - 1000,
                            w, h, 16, 1 };
                differ += checkForBiomes(&g, NULL, r, DIM_OVERWORLD, seed, &plain, NULL) !=
                          checkForBiomes(&g, NULL, r, DIM_OVERWORLD, seed, &bounded, NULL);
                differ += checkForBiomes(&g, NULL, r, DIM_OVERWORLD, seed, &exclP, NULL) !=
                          checkForBiomes(&g, NULL, r, DIM_OVERWORLD, seed, &exclB, NULL);
            }
        }
    }
    assert(differ == 0);
    assert(skipping > 0);
}

/* 1.15 - 1.17 overworld at 1:1: vanilla's column zoomer, i.e. the voronoi
 * access of the 1:4 biomes at block y = 0. Reference from a vanilla 1.16.5
 * dedicated server (server jar sha1 1b557e7b033b583cd9f66746b7a9ab1ec1673ced):
 * the biome at each block was tested with "execute positioned x y z if
 * predicate" and location_check predicates, at y = 10, 100 or 190 (the result
 * does not depend on y). 209 of the 354 points are where the previous, doubly
 * shifted voronoi access gave another biome. All points lie within 112 blocks
 * of the origin. */
static const struct { int64_t seed; int x, z, id; } g_vanilla_1_16_5[] = {
    {12345LL, 65, -107, taiga_mountains}, {12345LL, -45, -106, taiga}, {12345LL, -35, -106, taiga_mountains},
    {12345LL, 45, -106, taiga_mountains}, {12345LL, 48, -106, taiga_mountains}, {12345LL, -61, -100, taiga},
    {12345LL, 74, -100, taiga}, {12345LL, -40, -99, taiga_mountains}, {12345LL, -99, -95, taiga_mountains},
    {12345LL, -94, -93, taiga_mountains}, {12345LL, -92, -88, taiga}, {12345LL, -43, -88, taiga_mountains},
    {12345LL, -42, -88, taiga_mountains}, {12345LL, 36, -88, taiga_mountains}, {12345LL, 66, -88, taiga_mountains},
    {12345LL, 85, -88, taiga_mountains}, {12345LL, -108, -87, taiga_mountains}, {12345LL, 78, -86, taiga_mountains},
    {12345LL, -77, -85, taiga_mountains}, {12345LL, 56, -85, taiga_mountains}, {12345LL, -84, -83, taiga_mountains},
    {12345LL, -42, -80, taiga_mountains}, {12345LL, -45, -79, taiga_mountains}, {12345LL, -46, -77, taiga_mountains},
    {12345LL, -50, -76, taiga_mountains}, {12345LL, -54, -74, taiga_mountains}, {12345LL, -56, -73, taiga_mountains},
    {12345LL, -58, -71, taiga_mountains}, {12345LL, -63, -68, taiga_mountains}, {12345LL, -59, -68, taiga_mountains},
    {12345LL, 26, -68, taiga_mountains}, {12345LL, 110, -66, taiga_mountains}, {12345LL, 16, -64, taiga_mountains},
    {12345LL, 39, -59, taiga_mountains}, {12345LL, -1, -55, taiga_mountains}, {12345LL, 111, -55, taiga},
    {12345LL, 109, -51, taiga}, {12345LL, 21, -50, taiga_mountains}, {12345LL, 95, -49, taiga_mountains},
    {12345LL, 108, -47, taiga}, {12345LL, -104, -46, taiga_mountains}, {12345LL, 85, -40, taiga_mountains},
    {12345LL, -71, -38, taiga_mountains}, {12345LL, 105, -37, taiga}, {12345LL, 47, -34, taiga_mountains},
    {12345LL, -80, -30, taiga_mountains}, {12345LL, -4, -30, taiga_mountains}, {12345LL, 34, -28, taiga_mountains},
    {12345LL, 45, -28, taiga_mountains}, {12345LL, -1, -24, taiga_mountains}, {12345LL, 14, -24, taiga_mountains},
    {12345LL, 40, -23, taiga_mountains}, {12345LL, -67, -12, taiga_mountains}, {12345LL, 27, -12, taiga_mountains},
    {12345LL, -35, -10, taiga_mountains}, {12345LL, -25, -10, taiga_mountains}, {12345LL, -38, -5, taiga_mountains},
    {12345LL, 94, 3, taiga_mountains}, {12345LL, -55, 7, taiga_mountains}, {12345LL, -24, 15, mountains},
    {12345LL, 0, 16, taiga_mountains}, {12345LL, 8, 16, mountains}, {12345LL, -25, 18, mountains},
    {12345LL, -51, 19, taiga_mountains}, {12345LL, -56, 20, taiga_mountains}, {12345LL, -29, 20, mountains},
    {12345LL, -27, 21, mountains}, {12345LL, 10, 21, taiga_mountains}, {12345LL, -32, 25, mountains},
    {12345LL, -31, 26, mountains}, {12345LL, -30, 26, mountains}, {12345LL, 11, 31, taiga_mountains},
    {12345LL, -33, 32, mountains}, {12345LL, -32, 34, mountains}, {12345LL, 10, 36, mountains},
    {12345LL, -97, 37, taiga_mountains}, {12345LL, -82, 39, taiga_mountains}, {12345LL, -47, 39, mountains},
    {12345LL, -40, 39, mountains}, {12345LL, 34, 39, taiga_mountains}, {12345LL, -51, 41, mountains},
    {12345LL, -81, 43, mountains}, {12345LL, -54, 44, mountains}, {12345LL, -52, 44, mountains},
    {12345LL, -81, 45, taiga_mountains}, {12345LL, -62, 46, mountains}, {12345LL, -88, 47, mountains},
    {12345LL, 62, 47, taiga_mountains}, {12345LL, 19, 48, taiga_mountains}, {12345LL, -88, 49, taiga_mountains},
    {12345LL, -28, 49, mountains}, {12345LL, 99, 49, taiga_mountains}, {12345LL, 12, 51, taiga_mountains},
    {12345LL, 13, 51, taiga_mountains}, {12345LL, -106, 52, taiga_mountains}, {12345LL, -90, 52, mountains},
    {12345LL, 11, 52, taiga_mountains}, {12345LL, 45, 53, taiga_mountains}, {12345LL, 44, 54, taiga_mountains},
    {12345LL, 105, 54, taiga_mountains}, {12345LL, 14, 56, mountains}, {12345LL, 18, 56, taiga_mountains},
    {12345LL, -85, 57, mountains}, {12345LL, 100, 57, taiga_mountains}, {12345LL, 18, 58, taiga_mountains},
    {12345LL, 40, 79, taiga_mountains}, {12345LL, 79, 81, mountains}, {12345LL, 89, 81, mountains},
    {12345LL, 92, 81, mountains}, {12345LL, 79, 83, mountains}, {12345LL, 45, 85, mountains},
    {12345LL, 49, 85, taiga_mountains}, {12345LL, 73, 85, mountains}, {12345LL, 75, 85, mountains},
    {12345LL, 104, 85, taiga_mountains}, {12345LL, 43, 86, mountains}, {12345LL, 52, 89, mountains},
    {12345LL, 59, 93, mountains}, {12345LL, 59, 94, mountains}, {-4172144997902289642LL, -43, -108, frozen_river},
    {-4172144997902289642LL, -91, -106, snowy_tundra}, {-4172144997902289642LL, -21, -104, plains}, {-4172144997902289642LL, -34, -101, snowy_tundra},
    {-4172144997902289642LL, 42, -101, plains}, {-4172144997902289642LL, -97, -99, snowy_tundra}, {-4172144997902289642LL, -49, -98, snowy_tundra},
    {-4172144997902289642LL, 111, -98, snowy_tundra}, {-4172144997902289642LL, -22, -96, river}, {-4172144997902289642LL, -19, -96, plains},
    {-4172144997902289642LL, 92, -96, snowy_tundra}, {-4172144997902289642LL, -106, -95, snowy_tundra}, {-4172144997902289642LL, -104, -95, snowy_tundra},
    {-4172144997902289642LL, 89, -95, snowy_tundra}, {-4172144997902289642LL, -24, -93, river}, {-4172144997902289642LL, 76, -92, snowy_tundra},
    {-4172144997902289642LL, -17, -91, snowy_mountains}, {-4172144997902289642LL, 69, -90, plains}, {-4172144997902289642LL, -13, -88, plains},
    {-4172144997902289642LL, -31, -87, frozen_river}, {-4172144997902289642LL, -1, -87, plains}, {-4172144997902289642LL, 65, -87, snowy_tundra},
    {-4172144997902289642LL, 30, -86, plains}, {-4172144997902289642LL, -40, -85, frozen_river}, {-4172144997902289642LL, -36, -85, river},
    {-4172144997902289642LL, -27, -85, river}, {-4172144997902289642LL, -34, -81, snowy_mountains}, {-4172144997902289642LL, -29, -81, snowy_mountains},
    {-4172144997902289642LL, -36, -79, snowy_mountains}, {-4172144997902289642LL, 10, -79, plains}, {-4172144997902289642LL, 55, -79, snowy_tundra},
    {-4172144997902289642LL, 44, -75, plains}, {-4172144997902289642LL, 25, -74, snowy_mountains}, {-4172144997902289642LL, -49, -70, frozen_river},
    {-4172144997902289642LL, 27, -70, snowy_mountains}, {-4172144997902289642LL, -38, -69, snowy_mountains}, {-4172144997902289642LL, 14, -69, snowy_mountains},
    {-4172144997902289642LL, 13, -68, snowy_mountains}, {-4172144997902289642LL, 7, -67, snowy_mountains}, {-4172144997902289642LL, -41, -66, snowy_mountains},
    {-4172144997902289642LL, 22, -66, snowy_mountains}, {-4172144997902289642LL, -47, -64, snowy_tundra}, {-4172144997902289642LL, -46, -63, snowy_mountains},
    {-4172144997902289642LL, -44, -62, snowy_mountains}, {-4172144997902289642LL, -94, -61, snowy_tundra}, {-4172144997902289642LL, -55, -61, frozen_river},
    {-4172144997902289642LL, -49, -60, river}, {-4172144997902289642LL, -40, -58, snowy_mountains}, {-4172144997902289642LL, -56, -56, frozen_river},
    {-4172144997902289642LL, -34, -54, snowy_mountains}, {-4172144997902289642LL, 43, -53, snowy_mountains}, {-4172144997902289642LL, -40, -52, frozen_river},
    {-4172144997902289642LL, -38, -52, frozen_river}, {-4172144997902289642LL, -44, -51, frozen_river}, {-4172144997902289642LL, -40, -51, frozen_river},
    {-4172144997902289642LL, -33, -51, frozen_river}, {-4172144997902289642LL, -26, -51, river}, {-4172144997902289642LL, -54, -50, snowy_tundra},
    {-4172144997902289642LL, -100, -48, snowy_tundra}, {-4172144997902289642LL, -40, -48, snowy_tundra}, {-4172144997902289642LL, -44, -47, snowy_tundra},
    {-4172144997902289642LL, -27, -47, frozen_river}, {-4172144997902289642LL, -23, -45, river}, {-4172144997902289642LL, -29, -44, frozen_river},
    {-4172144997902289642LL, -24, -44, river}, {-4172144997902289642LL, 48, -44, snowy_tundra}, {-4172144997902289642LL, 49, -44, snowy_tundra},
    {-4172144997902289642LL, 61, -43, snowy_tundra}, {-4172144997902289642LL, -27, -42, snowy_tundra}, {-4172144997902289642LL, -13, -40, frozen_river},
    {-4172144997902289642LL, 62, -39, snowy_tundra}, {-4172144997902289642LL, -63, -38, snowy_taiga}, {-4172144997902289642LL, -62, -38, snowy_taiga},
    {-4172144997902289642LL, -16, -35, snowy_tundra}, {-4172144997902289642LL, -14, -35, snowy_tundra}, {-4172144997902289642LL, -26, -33, snowy_tundra},
    {-4172144997902289642LL, 23, -32, snowy_tundra}, {-4172144997902289642LL, -1, -31, frozen_river}, {-4172144997902289642LL, -64, -30, snowy_taiga},
    {-4172144997902289642LL, -3, -28, frozen_river}, {-4172144997902289642LL, 21, -25, snowy_tundra}, {-4172144997902289642LL, 5, -21, snowy_tundra},
    {-4172144997902289642LL, 15, -21, snowy_tundra}, {-4172144997902289642LL, -3, -20, frozen_river}, {-4172144997902289642LL, -106, -18, snowy_taiga_hills},
    {-4172144997902289642LL, -83, -18, snowy_taiga}, {-4172144997902289642LL, -87, -16, snowy_taiga}, {-4172144997902289642LL, -86, -15, snowy_taiga},
    {-4172144997902289642LL, -1, -15, snowy_tundra}, {-4172144997902289642LL, 111, -15, snowy_tundra}, {-4172144997902289642LL, -87, -9, snowy_taiga_hills},
    {-4172144997902289642LL, -46, -3, snowy_taiga}, {-4172144997902289642LL, 104, -3, snowy_tundra}, {-4172144997902289642LL, -45, -1, snowy_taiga},
    {-4172144997902289642LL, 55, 0, snowy_tundra}, {-4172144997902289642LL, 84, 3, snowy_tundra}, {-4172144997902289642LL, 27, 4, snowy_tundra},
    {-4172144997902289642LL, 71, 8, snowy_tundra}, {-4172144997902289642LL, 35, 15, snowy_tundra}, {-4172144997902289642LL, -108, 19, snowy_taiga_hills},
    {-4172144997902289642LL, 78, 29, snowy_tundra}, {-4172144997902289642LL, -82, 31, snowy_taiga}, {-4172144997902289642LL, -54, 38, snowy_tundra},
    {-4172144997902289642LL, 81, 39, snowy_tundra}, {-4172144997902289642LL, -20, 40, snowy_tundra}, {-4172144997902289642LL, 45, 40, snowy_tundra},
    {-4172144997902289642LL, -23, 44, snowy_tundra}, {-4172144997902289642LL, -5, 46, snowy_tundra}, {-4172144997902289642LL, -55, 47, snowy_tundra},
    {-4172144997902289642LL, -47, 48, snowy_tundra}, {-4172144997902289642LL, 98, 52, snowy_tundra}, {-4172144997902289642LL, -78, 64, snowy_tundra},
    {-4172144997902289642LL, -10, 66, frozen_river}, {-4172144997902289642LL, 17, 68, snowy_tundra}, {-4172144997902289642LL, 42, 72, frozen_river},
    {-4172144997902289642LL, 40, 84, snowy_tundra}, {-4172144997902289642LL, 26, 96, snowy_tundra}, {-4172144997902289642LL, 64, 98, snowy_tundra},
    {-4172144997902289642LL, 30, 101, snowy_tundra}, {-4172144997902289642LL, 29, 104, snowy_tundra}, {3141592653589793LL, -74, -108, forest},
    {3141592653589793LL, -98, -107, plains}, {3141592653589793LL, -86, -107, river}, {3141592653589793LL, -61, -106, plains},
    {3141592653589793LL, 44, -106, plains}, {3141592653589793LL, -27, -105, forest}, {3141592653589793LL, 45, -105, plains},
    {3141592653589793LL, -85, -103, river}, {3141592653589793LL, 43, -102, plains}, {3141592653589793LL, -84, -101, river},
    {3141592653589793LL, -99, -100, river}, {3141592653589793LL, -53, -100, plains}, {3141592653589793LL, 38, -100, river},
    {3141592653589793LL, -89, -99, river}, {3141592653589793LL, -106, -98, lukewarm_ocean}, {3141592653589793LL, -102, -98, river},
    {3141592653589793LL, 44, -98, plains}, {3141592653589793LL, -101, -97, river}, {3141592653589793LL, -92, -96, beach},
    {3141592653589793LL, -11, -95, forest}, {3141592653589793LL, -52, -94, forest}, {3141592653589793LL, -90, -93, beach},
    {3141592653589793LL, -85, -93, plains}, {3141592653589793LL, 82, -92, river}, {3141592653589793LL, 66, -91, river},
    {3141592653589793LL, 99, -91, river}, {3141592653589793LL, -101, -90, river}, {3141592653589793LL, -94, -89, beach},
    {3141592653589793LL, -92, -89, beach}, {3141592653589793LL, -49, -89, forest}, {3141592653589793LL, 105, -89, plains},
    {3141592653589793LL, -48, -88, forest}, {3141592653589793LL, 49, -88, river}, {3141592653589793LL, 108, -88, river},
    {3141592653589793LL, 62, -86, river}, {3141592653589793LL, 89, -86, river}, {3141592653589793LL, -98, -84, beach},
    {3141592653589793LL, -45, -84, plains}, {3141592653589793LL, -26, -84, forest}, {3141592653589793LL, 82, -83, plains},
    {3141592653589793LL, -47, -82, plains}, {3141592653589793LL, 101, -82, river}, {3141592653589793LL, -102, -81, plains},
    {3141592653589793LL, -73, -81, plains}, {3141592653589793LL, -101, -80, plains}, {3141592653589793LL, 93, -80, plains},
    {3141592653589793LL, -96, -79, plains}, {3141592653589793LL, 87, -79, plains}, {3141592653589793LL, -103, -77, plains},
    {3141592653589793LL, 64, -77, plains}, {3141592653589793LL, -46, -75, plains}, {3141592653589793LL, 59, -75, plains},
    {3141592653589793LL, -108, -72, river}, {3141592653589793LL, 21, -72, forest}, {3141592653589793LL, 55, -72, plains},
    {3141592653589793LL, -34, -70, forest}, {3141592653589793LL, 57, -70, plains}, {3141592653589793LL, 58, -70, plains},
    {3141592653589793LL, 109, -70, plains}, {3141592653589793LL, -47, -69, plains}, {3141592653589793LL, 51, -69, forest},
    {3141592653589793LL, -32, -68, forest}, {3141592653589793LL, 53, -68, plains}, {3141592653589793LL, -33, -67, forest},
    {3141592653589793LL, 98, -65, plains}, {3141592653589793LL, -102, -64, river}, {3141592653589793LL, -106, -62, river},
    {3141592653589793LL, 86, -61, plains}, {3141592653589793LL, -35, -56, forest}, {3141592653589793LL, 38, -56, plains},
    {3141592653589793LL, -104, -55, plains}, {3141592653589793LL, -33, -55, plains}, {3141592653589793LL, -97, -53, plains},
    {3141592653589793LL, -96, -53, plains}, {3141592653589793LL, -86, -53, plains}, {3141592653589793LL, 2, -52, forest},
    {3141592653589793LL, -4, -50, forest}, {3141592653589793LL, 23, -50, plains}, {3141592653589793LL, -104, -48, river},
    {3141592653589793LL, -23, -48, plains}, {3141592653589793LL, -19, -43, forest}, {3141592653589793LL, -20, -41, forest},
    {3141592653589793LL, -22, -40, forest}, {3141592653589793LL, -101, -38, river}, {3141592653589793LL, 42, -38, plains},
    {3141592653589793LL, -97, -37, river}, {3141592653589793LL, 11, -37, plains}, {3141592653589793LL, 14, -36, plains},
    {3141592653589793LL, -85, -34, plains}, {3141592653589793LL, -83, -33, plains}, {3141592653589793LL, -92, -32, beach},
    {3141592653589793LL, -80, -31, river}, {3141592653589793LL, 55, -30, plains}, {3141592653589793LL, 66, -29, plains},
    {3141592653589793LL, -95, -15, river}, {3141592653589793LL, -21, -11, plains}, {3141592653589793LL, -9, -8, plains},
    {3141592653589793LL, -4, 0, plains}, {3141592653589793LL, 96, 4, plains}, {3141592653589793LL, 64, 9, plains},
    {3141592653589793LL, -86, 14, plains}, {3141592653589793LL, -89, 21, plains}, {3141592653589793LL, -26, 23, plains},
    {3141592653589793LL, -77, 25, plains}, {3141592653589793LL, -55, 32, plains}, {3141592653589793LL, -35, 35, plains},
    {3141592653589793LL, -11, 35, plains}, {3141592653589793LL, -35, 53, wooded_hills}, {3141592653589793LL, 32, 59, plains},
    {3141592653589793LL, 8, 70, wooded_hills}, {3141592653589793LL, -94, 72, lukewarm_ocean}, {3141592653589793LL, 62, 73, forest},
    {3141592653589793LL, -92, 85, lukewarm_ocean}, {3141592653589793LL, 44, 102, forest}, {3141592653589793LL, 36, 108, river},
};

static void testVoronoi1x1Vanilla(void)
{
    enum { X0 = -112, W = 224 };
    int64_t seed = 0;
    int *area = NULL;
    Generator g;
    setupGenerator(&g, MC_1_16_5, 0);
    for (size_t i = 0; i < sizeof(g_vanilla_1_16_5) / sizeof(g_vanilla_1_16_5[0]); i++)
    {
        int x = g_vanilla_1_16_5[i].x, z = g_vanilla_1_16_5[i].z;
        if (!area || seed != g_vanilla_1_16_5[i].seed)
        {
            seed = g_vanilla_1_16_5[i].seed;
            applySeed(&g, DIM_OVERWORLD, (uint64_t) seed);
            Range r = { 1, X0, X0, W, W, 0, 1 };
            free(area);
            area = allocCache(&g, r);
            assert(area && genBiomes(&g, area, r) == 0);
        }
        assert(area[(z - X0) * W + (x - X0)] == g_vanilla_1_16_5[i].id);
        assert(getBiomeAt(&g, 1, x, 64, z) == g_vanilla_1_16_5[i].id);
    }
    free(area);
}

/* getBiomeCentersEx(): the climate prefilter of 1.18+ only skips cells that
 * cannot be the biome, so the result equals the search without it (the
 * reference, getBiomeCentersUnfiltered()). */
int getBiomeCentersUnfiltered(Pos *pos, int *siz, int nmax, Generator *g, Range r,
    int match, int minsiz, int tol, volatile char *stop, int *status);

static void testBiomeCentersPrefilter(void)
{
    static const int mcs[] = { MC_1_18, MC_NEWEST };
    static const int biomes[] = { ocean, forest, river, cherry_grove };
    static const int scales[] = { 4, 16, 1 };
    enum { NMAX = 4096 };
    static Pos p0[NMAX], p1[NMAX];
    static int s0[NMAX], s1[NMAX];
    for (size_t v = 0; v < sizeof(mcs) / sizeof(mcs[0]); v++)
    {
        Generator g;
        setupGenerator(&g, mcs[v], 0);
        applySeed(&g, DIM_OVERWORLD, 1234567 + v);
        for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); s++)
        {
            int sc = scales[s];
            int w = sc == 1 ? 64 : (sc == 4 ? 100 : 50);
            Range r = { sc, -w / 2 - 13, -w / 2 + 7, w, w, sc == 1 ? 64 : 16, 1 };
            for (size_t b = 0; b < sizeof(biomes) / sizeof(biomes[0]); b++)
            {
                int tol = 1 + (int) (b % 3);
                int st0, st1;
                int n0 = getBiomeCentersUnfiltered(p0, s0, NMAX, &g, r, biomes[b],
                    1, tol, NULL, &st0);
                int n1 = getBiomeCentersEx(p1, s1, NMAX, &g, r, biomes[b],
                    1, tol, NULL, &st1);
                assert(st0 == 0 && st1 == 0);
                assert(n0 == n1);
                for (int i = 0; i < n0; i++)
                {   // the same regions (listed in the order of their first cell)
                    int k;
                    for (k = 0; k < n1; k++)
                        if (p1[k].x == p0[i].x && p1[k].z == p0[i].z && s1[k] == s0[i])
                            break;
                    assert(k < n1);
                }
            }
        }
    }
}

int main(void)
{
    static const char *versions[] = {
        "1.21.1", "1.21.2", "1.21.3", "1.21.4", "1.21.5",
        "1.21.6", "1.21.7", "1.21.8", "1.21.9", "1.21.10",
        "1.21.11", "26.1", "26.1.1", "26.1.2", "26.2",
        "26.3",
    };

    for (int mc = MC_UNDEF + 1; mc <= MC_NEWEST; mc++)
        assert(strcmp(mc2str(mc), "?") != 0);

    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
    {
        int mc = str2mc(versions[i]);
        assert(mc != MC_UNDEF);
        assert(strcmp(mc2str(mc), versions[i]) == 0);
    }

    assert(str2mc("1.21 WD") == MC_1_21_4);
    assert(str2mc("26.3") == MC_26_3);
    assert(str2mc("26.3-pre-2") == MC_26_3);
    assert(biomeExists(MC_1_21_3, pale_garden) == 0);
    assert(biomeExists(MC_1_21_4, pale_garden) == 1);

    {
        StructureConfig config;
        Pos full[8], tile[1];
        volatile char abort = 1;
        assert(getStructureConfig(Swamp_Hut, MC_1_21_5, &config));
        assert(scanForQuadsCancelable(config, 128, 0, low20QuadHutBarely,
            20, config.salt, -58593, -58593, 117186, 117186,
            full, 8, &abort) == 0);
        int count = scanForQuads(config, 128, 0, low20QuadHutBarely,
            20, config.salt, -58593, -58593, 117186, 117186, full, 8);
        assert(count == 8);
        assert(scanForQuads(config, 128, 0, low20QuadHutBarely,
            20, config.salt, full[0].x, full[0].z, 0, 0, tile, 1) == 1);
        assert(tile[0].x == full[0].x && tile[0].z == full[0].z);
    }
    assert(biomeExists(MC_26_1_2, sulfur_caves) == 0);
    assert(biomeExists(MC_26_2, sulfur_caves) == 1);
    assert(biomeExists(MC_26_2, dappled_forest) == 0);
    assert(biomeExists(MC_26_3, dappled_forest) == 1);
    assert(getCategory(MC_26_3, dappled_forest) == forest);
    assert(isViableFeatureBiome(MC_1_21_4, Mansion, pale_garden) == 0);
    assert(isViableFeatureBiome(MC_1_21_5, Mansion, pale_garden) == 1);

    static const int32_t expanded_pale_ranges[10][6] = {
        {3000, 10000, -7799, -3750, -10000, -9333},
        {3000, 10000, -3750, -2225, -10000, -9333},
        { 300, 10000, -3750, -2225,  -9333, -7666},
        {3000, 10000, -2225,   500,  -9333, -7666},
        { 300, 10000, -3750, -2225,  -7666, -5666},
        {3000, 10000, -2225,   500,  -7666, -5666},
        { 300, 10000, -3750, -2225,  -5666, -4000},
        {3000, 10000, -2225,   500,  -5666, -4000},
        {3000, 10000, -7799, -3750,  -4000, -2666},
        {3000, 10000, -3750, -2225,  -4000, -2666},
    };
    for (int i = 0; i < 10; i++)
    {
        for (int depth = 0; depth <= 10000; depth += 10000)
        {
            const int32_t *r = expanded_pale_ranges[i];
            uint64_t expanded_pale[6] = {
                250, 6500, (uint64_t)(int64_t)((r[0] + r[1]) / 2),
                (uint64_t)(int64_t)((r[2] + r[3]) / 2), depth,
                (uint64_t)(int64_t)((r[4] + r[5]) / 2),
            };
            assert(climateToBiome(MC_1_21_4, expanded_pale, NULL) == dark_forest);
            assert(climateToBiome(MC_1_21_5, expanded_pale, NULL) == pale_garden);
        }
    }

    // A point inside the stable 26.2 Sulfur Caves parameter box. Before 26.2
    // the same climate point must resolve to one of the pre-existing biomes.
    const uint64_t np[6] = {
        0, 0, 0, 5000, 5000, (uint64_t)(int64_t)-9000
    };
    assert(climateToBiome(MC_26_2, np, NULL) == sulfur_caves);
    assert(climateToBiome(MC_26_1_2, np, NULL) != sulfur_caves);

    const int *lim = getBiomeParaLimits(MC_26_2, sulfur_caves);
    assert(lim != NULL);
    assert(lim[4] == -1900 && lim[5] == 5500);
    assert(lim[6] == 4500 && lim[7] == INT_MAX);
    assert(lim[8] == 2000 && lim[9] == 9000);
    assert(lim[10] == -11000 && lim[11] == -8500);

    // 26.3 Pre-Release 2 relabels the coldest/driest Plains points on the
    // non-negative weirdness side as Dappled Forest. Climate noise itself is
    // unchanged, so the same representative point is Plains in 26.2.
    const uint64_t dappled_np[6] = {
        (uint64_t)(int64_t)-3000, (uint64_t)(int64_t)-6750,
        5150, (uint64_t)(int64_t)-6875, 0, 0
    };
    assert(climateToBiome(MC_26_2, dappled_np, NULL) == plains);
    assert(climateToBiome(MC_26_3, dappled_np, NULL) == dappled_forest);

    lim = getBiomeParaLimits(MC_26_3, dappled_forest);
    assert(lim != NULL);
    assert(lim[0] == -4500 && lim[1] == -1500);
    assert(lim[2] == INT_MIN && lim[3] == -3500);
    assert(lim[4] == -1900 && lim[5] == INT_MAX);
    assert(lim[10] == -500 && lim[11] == INT_MAX);
    {
        // Coordinate returned by Vanilla's /locate biome command for this
        // seed in 26.3 Pre-Release 2.
        Generator dappled;
        setupGenerator(&dappled, MC_26_3, 0);
        applySeed(&dappled, DIM_OVERWORLD, 8371904829ULL);
        assert(getBiomeAt(&dappled, 1, -1184, 100, 1248) == dappled_forest);
    }

    // Official 26.3 Pre-Release 2 abandoned-camp structure set and biome tags.
    assertStructureConfig(Abandoned_Camp, MC_26_3, 37, 8, 91231127);
    {
        StructureConfig config;
        assert(!getStructureConfig(Abandoned_Camp, MC_26_2, &config));
    }
    static const int camp_biomes[] = {
        bamboo_jungle, birch_forest, cherry_grove, dappled_forest,
        flower_forest, forest, meadow, old_growth_birch_forest,
        old_growth_pine_taiga, old_growth_spruce_taiga, pale_garden,
        savanna, snowy_taiga, sparse_jungle, swamp, taiga,
        windswept_forest, wooded_badlands,
    };
    for (size_t i = 0; i < sizeof(camp_biomes) / sizeof(camp_biomes[0]); i++)
        assert(isViableFeatureBiome(MC_26_3, Abandoned_Camp, camp_biomes[i]));
    assert(!isViableFeatureBiome(MC_26_2, Abandoned_Camp, forest));
    assert(!isViableFeatureBiome(MC_26_3, Abandoned_Camp, plains));
    assertStructurePos(Abandoned_Camp, MC_26_3, 8371904829ULL,
                       0, 0, 288, 32);
    // Nearest camp reported by a fully generated Vanilla 26.3 Pre-Release 2
    // server for this seed. It is the forest variant in region (-1, 0).
    assertStructurePos(Abandoned_Camp, MC_26_3, 8371904829ULL,
                       -1, 0, -512, 416);
    {
        Generator camp;
        setupGenerator(&camp, MC_26_3, 0);
        applySeed(&camp, DIM_OVERWORLD, 8371904829ULL);
        assert(isViableStructurePos(Abandoned_Camp, &camp,
                                    -512, 416, 0) == forest);
        // The generic chunk-centre check classified this boundary candidate
        // as Bamboo Jungle even though the camp start itself is not viable.
        assert(isViableStructurePos(Abandoned_Camp, &camp,
                                    10704, -23600, 0) == 0);
    }
    {
        // Vanilla places this seed-14 camp in chunk (-52,-133). Its selected
        // 8x8 start tent has centre (-828,63,-2124): Java's absolute-coordinate
        // division rounds the negative X/Z values toward zero, and the biome
        // must be sampled at the projected surface Y instead of Y=319.
        assertStructurePos(Abandoned_Camp, MC_26_3, 14,
                           -2, -4, -832, -2128);
        Generator camp;
        setupGenerator(&camp, MC_26_3, 0);
        applySeed(&camp, DIM_OVERWORLD, 14);
        float surfaceY;
        int nptype = camp.bn.nptype;
        camp.bn.nptype = NP_DEPTH;
        assert(mapApproxHeight(&surfaceY, NULL, &camp, NULL,
                               -207, -531, 1, 1) == 0);
        camp.bn.nptype = nptype;
        assert((int) floorf(surfaceY) + 1 == 63);
        assert(getBiomeAt(&camp, 0, -207, 63 >> 2, -531) == snowy_taiga);
        // Computing the centre as a relative offset first used the adjacent
        // quart (-208, -532), which is Frozen River and rejected the camp.
        assert(getBiomeAt(&camp, 0, -208, 63 >> 2, -532) == frozen_river);
        assert(isViableStructurePos(Abandoned_Camp, &camp,
                                    -832, -2128, 0) == snowy_taiga);

        StructureVariant variant;
        assert(getVariant(&variant, Abandoned_Camp, MC_26_3, 14,
                          -832, -2128, snowy_taiga));
        // Vanilla selects campsite_default_special_4 here.
        assert(variant.special);
    }
    {
        // This second known camp selects a regular (non-special) pool element.
        StructureVariant variant;
        assert(getVariant(&variant, Abandoned_Camp, MC_26_3, 8371904829ULL,
                          -512, 416, forest));
        assert(!variant.special);
    }
    {
        StructureVariant variant;

        // Template names alone do not determine the special-loot variant.
        // special_11 has no oxidized copper chest and must remain unmarked.
        assert(getVariant(&variant, Abandoned_Camp, MC_26_3, 14,
                          -6416, -2688, forest));
        assert(!variant.special);

        // Conversely, barrel_12 does contain the secret oxidized copper chest.
        assert(getVariant(&variant, Abandoned_Camp, MC_26_3, 14,
                          -16720, 10416, snowy_taiga));
        assert(variant.special);

        // Biome-specific pool entries differ even at the same pool index:
        // forest_3 has special loot, while snowy_taiga_3 does not.
        assert(getVariant(&variant, Abandoned_Camp, MC_26_3, 14,
                          -16256, -8128, forest));
        assert(variant.special);
        assert(getVariant(&variant, Abandoned_Camp, MC_26_3, 14,
                          -12016, -5184, snowy_taiga));
        assert(!variant.special);
    }

    // Stable Java structure-set parameters from the official 26.2 data pack.
    // The values are semantically unchanged from 1.21.4 through 26.2.
    assertStructureConfig(Desert_Pyramid, MC_26_2, 32, 8, 14357617);
    assertStructureConfig(Igloo,          MC_26_2, 32, 8, 14357618);
    assertStructureConfig(Jungle_Pyramid, MC_26_2, 32, 8, 14357619);
    assertStructureConfig(Swamp_Hut,      MC_26_2, 32, 8, 14357620);
    assertStructureConfig(Village,        MC_26_2, 34, 8, 10387312);
    assertStructureConfig(Ocean_Ruin,     MC_26_2, 20, 8, 14357621);
    assertStructureConfig(Shipwreck,      MC_26_2, 24, 4, 165745295);
    assertStructureConfig(Monument,       MC_26_2, 32, 5, 10387313);
    assertStructureConfig(Mansion,        MC_26_2, 80, 20, 10387319);
    assertStructureConfig(Outpost,        MC_26_2, 32, 8, 165745296);
    assertStructureConfig(Ruined_Portal,  MC_26_2, 40, 15, 34222645);
    assertStructureConfig(Ruined_Portal_N, MC_26_2, 40, 15, 34222645);
    assertStructureConfig(Ancient_City,   MC_26_2, 24, 8, 20083232);
    assertStructureConfig(Trail_Ruins,    MC_26_2, 34, 8, 83469867);
    assertStructureConfig(Trial_Chambers, MC_26_2, 34, 12, 94251327);
    assertStructureConfig(Fortress,       MC_26_2, 27, 4, 30084232);
    assertStructureConfig(Bastion,        MC_26_2, 27, 4, 30084232);
    assertStructureConfig(End_City,       MC_26_2, 20, 11, 10387313);

    /* End City random-spread placement has stayed at spacing 20,
       separation 11, triangular spreading, and salt 10387313 since 1.9.
       Exercise every supported release so a future version branch cannot
       silently change its candidate coordinates or skip the terrain rule. */
    for (int mc = MC_1_9; mc <= MC_NEWEST; mc++)
    {
        Generator end;
        SurfaceNoise surface;
        const uint64_t seed = UINT64_C(8371904829);

        assertStructureConfig(End_City, mc, 20, 11, 10387313);
        assertStructurePos(End_City, mc, seed, 4, -4, 1360, -1248);

        setupGenerator(&end, mc, 0);
        applySeed(&end, DIM_END, seed);
        initSurfaceNoise(&surface, DIM_END, seed);

        /* Both starts pass the End biome rule, but Vanilla's rotated 5x5
           minimum-height check rejects the first and accepts the second. */
        assert(isViableStructurePos(End_City, &end, 1072, 64, 0));
        assert(isViableEndCityTerrain(&end, &surface, 1072, 64) == 0);
        assert(isViableStructurePos(End_City, &end, -592, -896, 0));
        assert(isViableEndCityTerrain(&end, &surface, -592, -896) >= 60);
    }

    {
        /* This mixed seed makes the first next(31) value fall in Java's
           nextInt(9) rejection tail. A simple `% 9` implementation instead
           shifts all later triangular-spread draws to block (48, 64). */
        StructureConfig config;
        assert(getStructureConfig(End_City, MC_1_9, &config));
        Pos chunk = getLargeStructureChunkInRegion(config,
            UINT64_C(266262701690195), 0, 0);
        assert(chunk.x == 5 && chunk.z == 5);

        /* Preserve the same mixed RNG seed in a region outside the central
           End exclusion radius and verify the public block-coordinate API. */
        assertStructurePos(End_City, MC_1_9, UINT64_C(264895209175347),
            4, 0, 1360, 80);
        assertStructurePos(End_City, MC_NEWEST, UINT64_C(264895209175347),
            4, 0, 1360, 80);

        /* The helper is shared by every built-in triangular placement. */
        assertStructurePos(Monument, MC_NEWEST, UINT64_C(138460028882259),
            0, 0, 112, 304);
        assertStructurePos(Mansion, MC_NEWEST, UINT64_C(181071668596045),
            0, 0, 336, 576);
    }

    {
        /* LINEAR random-spread placements use the same Java rejection rule.
           These deliberately rare seeds put the first draw in the rejection
           tail and exercise both Overworld and Nether public API paths. */
        Pos pos;
        assertStructurePos(Village, MC_NEWEST, UINT64_C(329087727717716),
            0, 0, 384, 224);
        assertStructurePos(Ruined_Portal, MC_NEWEST,
            UINT64_C(186643030383247), 0, 0, 144, 208);

        /* At this corrected Nether candidate the shared placement selects a
           fortress. The old modulo shortcut instead reported a viable
           bastion at block (0, 256). */
        assert(!getStructurePos(Bastion, MC_NEWEST,
            UINT64_C(21796851793980), 0, 0, &pos));
        assert(pos.x == 256 && pos.z == 16);
        assert(getStructurePos(Fortress, MC_NEWEST,
            UINT64_C(21796851793980), 0, 0, &pos));
        assert(pos.x == 256 && pos.z == 16);
    }

    {
        /* The same candidate exercises all three Vanilla rotation sources:
           1.9-1.10 MapGen RNG after its dummy draw, 1.11-1.18's position-only
           seed, and 1.19+'s chunk-generation RNG without the dummy draw. */
        static const struct { int mc, height; } cases[] = {
            {MC_1_9,     0},
            {MC_1_10,    0},
            {MC_1_11,   61},
            {MC_1_18,   61},
            {MC_1_19_2, 60},
            {MC_NEWEST, 60},
        };
        const uint64_t seed = 0;
        SurfaceNoise surface;
        Pos pos;
        assert(getStructurePos(End_City, MC_1_9, seed, -35, -50, &pos));
        assert(pos.x == -11120 && pos.z == -15904);
        initSurfaceNoise(&surface, DIM_END, seed);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
            Generator end;
            setupGenerator(&end, cases[i].mc, 0);
            applySeed(&end, DIM_END, seed);
            assert(isViableEndCityTerrain(&end, &surface, pos.x, pos.z)
                == cases[i].height);
        }
    }

    // Buried treasure and mineshafts use Mojang's legacy frequency reducers.
    // The treasure salt is supplied by legacy_type_2 rather than the JSON salt.
    assertStructureConfig(Treasure, MC_26_2, 1, 0, 10387320);
    assertStructureConfig(Mineshaft, MC_26_2, 1, 0, 0);

    // Starts observed in fully generated, unmodified Vanilla server worlds.
    // seed 8371904829, Java 26.2
    assertStructurePos(Ruined_Portal,  MC_26_2, 8371904829ULL,  0,   0,  272,   48);
    assertStructurePos(Treasure,       MC_26_2, 8371904829ULL,  0, -14,    9, -215);
    assertStructurePos(Mineshaft,      MC_26_2, 8371904829ULL, -14, 15, -224,  240);
    assertStructurePos(Trial_Chambers, MC_26_2, 8371904829ULL,  0,   0,  272,   96);

    // seed 3515201313347228787, Java 1.21.5. The mansion coordinate is the
    // exact result returned by Vanilla's /locate command.
    assertStructurePos(Mansion,        MC_1_21_5, 3515201313347228787ULL,
                        3, -1, 4320, -768);
    assertStructurePos(Ruined_Portal,  MC_1_21_5, 3515201313347228787ULL,
                        0, 0, 48, 176);
    assertStructurePos(Trial_Chambers, MC_1_21_5, 3515201313347228787ULL,
                        0, 0, 48, 240);

    Generator g;
    setupGenerator(&g, MC_1_21_5, 0);
    applySeed(&g, DIM_OVERWORLD, 3515201313347228787ULL);
    assert(isViableStructurePos(Mansion, &g, 4320, -768, 0));

    // 1.21.2 replaced the spawn fitness function. This seed exercises both
    // sides of the exact version boundary found in Mojang's Climate class.
    Generator spawnOld, spawnNew;
    setupGenerator(&spawnOld, MC_1_21_1, 0);
    setupGenerator(&spawnNew, MC_1_21_2, 0);
    applySeed(&spawnOld, DIM_OVERWORLD, 7636401805092394055ULL);
    applySeed(&spawnNew, DIM_OVERWORLD, 7636401805092394055ULL);
    Pos oldSpawn = estimateSpawn(&spawnOld, NULL);
    Pos newSpawn = estimateSpawn(&spawnNew, NULL);
    assert(oldSpawn.x == -136 && oldSpawn.z == 584);
    assert(newSpawn.x == -760 && newSpawn.z == -920);

    testBiomeClimateBounds();
    testStructureBiomeChecks();
    testBiomeFilterClimateBounds();
    testVoronoi1x1Vanilla();
    testBiomeCentersPrefilter();

    puts("version, biome, and structure tests passed");
    return 0;
}
