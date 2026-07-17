#include "biomenoise.h"
#include "biomes.h"
#include "finders.h"
#include "quadbase.h"
#include "util.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    static const char *stable[] = {
        "1.21.1", "1.21.2", "1.21.3", "1.21.4", "1.21.5",
        "1.21.6", "1.21.7", "1.21.8", "1.21.9", "1.21.10",
        "1.21.11", "26.1", "26.1.1", "26.1.2", "26.2",
    };

    for (int mc = MC_UNDEF + 1; mc <= MC_NEWEST; mc++)
        assert(strcmp(mc2str(mc), "?") != 0);

    for (size_t i = 0; i < sizeof(stable) / sizeof(stable[0]); i++)
    {
        int mc = str2mc(stable[i]);
        assert(mc != MC_UNDEF);
        assert(strcmp(mc2str(mc), stable[i]) == 0);
    }

    assert(str2mc("1.21 WD") == MC_1_21_4);
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

    puts("stable version, biome, and structure tests passed");
    return 0;
}
