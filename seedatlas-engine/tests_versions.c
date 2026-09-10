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

    puts("version, biome, and structure tests passed");
    return 0;
}
