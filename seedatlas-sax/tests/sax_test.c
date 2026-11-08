#include "seedatlas_sax.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    sax_context *context;
    int32_t biome = -1;
    int32_t ids[SAX_CHUNK_SAMPLE_COUNT];
    uint32_t colors[SAX_CHUNK_SAMPLE_COUNT];
    int32_t sampled_ids[SAX_CHUNK_SAMPLE_COUNT];
    uint32_t sampled_colors[SAX_CHUNK_SAMPLE_COUNT];
    int32_t results[64 * SAX_RESULT_STRIDE];
    int32_t position[3];
    char version[64];
    char name[64];
    int count;
    int i;
    const int32_t dimensions[] = {
        SAX_DIM_OVERWORLD, SAX_DIM_NETHER, SAX_DIM_END};
    const int32_t heights[] = {255, 64, 128};

    assert(sax_abi_version() == SAX_ABI_VERSION);
    assert((sax_capabilities() & SAX_CAP_BIOMES) != 0);
    assert((sax_capabilities() & SAX_CAP_BIOME_SAMPLING) != 0);
    assert((sax_capabilities() & SAX_CAP_BIOME_AREAS) != 0);
    assert((sax_capabilities() & SAX_CAP_HEIGHTS) != 0);
    assert(sax_engine_version(version, sizeof(version)) > 1);
    assert(strstr(version, "26.3") != NULL);

    context = sax_create(INT64_C(8371904829), SAX_WORLD_NORMAL);
    assert(context != NULL);
    assert(sax_biome_at(context, SAX_DIM_OVERWORLD, 0, 255, 0, &biome) == SAX_OK);
    assert(biome >= 0);
    assert(sax_biome_name(biome, name, sizeof(name)) > 1);
    assert(sax_biome_color(biome) != 0);

    assert(sax_biome_chunk(context, SAX_DIM_OVERWORLD, 0, 0, 255,
                           ids, colors) == SAX_OK);
    for (i = 0; i < SAX_CHUNK_SAMPLE_COUNT; ++i) {
        assert(ids[i] >= 0);
        assert((colors[i] & UINT32_C(0xff000000)) == UINT32_C(0xff000000));
    }

    for (int step = 1; step <= 16; step *= 2) {
        int x, z;
        assert(sax_biome_chunk_sampled(context, SAX_DIM_OVERWORLD,
                                       0, 0, 255, step,
                                       sampled_ids, sampled_colors) == SAX_OK);
        for (z = 0; z < 16; ++z) {
            for (x = 0; x < 16; ++x) {
                int sample_index = ((z / step) * step + step / 2) * 16 +
                                   (x / step) * step + step / 2;
                int index = z * 16 + x;
                assert(sampled_ids[index] == ids[sample_index]);
                assert(sampled_colors[index] == colors[sample_index]);
            }
        }
    }
    assert(sax_biome_chunk_sampled(context, SAX_DIM_OVERWORLD,
                                   0, 0, 255, 3,
                                   sampled_ids, sampled_colors) ==
           SAX_ERR_ARGUMENT);

    /* Compact areas preserve exact center sampling at block/detail levels. */
    {
        float h1[40 * 40], h2[40 * 40], hs[10 * 10];
        int x, z;
        float lo = 1e9F, hi = -1e9F;
        /* Heights are deterministic, finite and vary over a wide area. */
        assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                   -4000, -4000, 32, 40, 40, h1, 1600) == SAX_OK);
        assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                   -4000, -4000, 32, 40, 40, h2, 1600) == SAX_OK);
        for (i = 0; i < 1600; ++i) {
            assert(h1[i] == h2[i]);
            assert(h1[i] > -400.0F && h1[i] < 400.0F);
            if (h1[i] < lo) lo = h1[i];
            if (h1[i] > hi) hi = h1[i];
        }
        assert(hi - lo > 20.0F);
        /* Interpolation: a step-2 sample centred on a climate-cell centre
           equals that cell's step-4 value, and a step-1 sample 1/8 cell past
           it is the bilinear blend with weights 7/8 and 1/8. */
        {
            float cells[4], s2[1], s1[1], expect;
            assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                       -64, -64, 4, 2, 2, cells, 4) == SAX_OK);
            assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                       -63, -63, 2, 1, 1, s2, 1) == SAX_OK);
            assert(s2[0] == cells[0]);
            assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                       -62, -62, 1, 1, 1, s1, 1) == SAX_OK);
            expect = cells[0] * (49.0F / 64.0F) + cells[1] * (7.0F / 64.0F) +
                     cells[2] * (7.0F / 64.0F) + cells[3] * (1.0F / 64.0F);
            assert(fabsf(s1[0] - expect) < 1e-3F);
        }
        /* Steps below 4 are continuous across the origin, including negatives
           and tile borders: two adjacent tiles equal one whole tile. */
        {
            float whole[16 * 8], left[8 * 8], right[8 * 8];
            assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                       -13, -21, 2, 16, 8, whole, 128) == SAX_OK);
            assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                       -13, -21, 2, 8, 8, left, 64) == SAX_OK);
            assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                       -13 + 16, -21, 2, 8, 8, right, 64) == SAX_OK);
            for (z = 0; z < 8; ++z) {
                for (x = 0; x < 8; ++x) {
                    assert(whole[z * 16 + x] == left[z * 8 + x]);
                    assert(whole[z * 16 + x + 8] == right[z * 8 + x]);
                }
            }
            /* neighbouring blocks differ by far less than the full range */
            for (x = 1; x < 16; ++x) {
                float d = whole[x] - whole[x - 1];
                assert(d > -30.0F && d < 30.0F);
            }
        }
        /* Errors: Nether/End unsupported, bad step, small buffer, no output. */
        {
            float end_a[32 * 32], end_b[32 * 32], end_l[16 * 32], end_r[16 * 32];
            float end_far[16 * 16];
            float peak = 0.0F, far_peak = 0.0F;
            assert(sax_height_area_sampled(context, SAX_DIM_END,
                       -64, -64, 4, 32, 32, end_a, 1024) == SAX_OK);
            assert(sax_height_area_sampled(context, SAX_DIM_END,
                       -64, -64, 4, 32, 32, end_b, 1024) == SAX_OK);
            for (i = 0; i < 1024; ++i) {
                assert(end_a[i] == end_b[i]);
                if (end_a[i] > peak) peak = end_a[i];
            }
            assert(peak > 30.0F); /* the central island */
            assert(sax_height_area_sampled(context, SAX_DIM_END,
                       400, 400, 8, 16, 16, end_far, 256) == SAX_OK);
            for (i = 0; i < 256; ++i)
                if (end_far[i] > far_peak) far_peak = end_far[i];
            assert(far_peak == 0.0F); /* the void between the islands */
            /* Two adjacent aligned tiles equal one whole tile. */
            assert(sax_height_area_sampled(context, SAX_DIM_END,
                       -64, -64, 4, 16, 32, end_l, 512) == SAX_OK);
            assert(sax_height_area_sampled(context, SAX_DIM_END,
                       0, -64, 4, 16, 32, end_r, 512) == SAX_OK);
            for (z = 0; z < 32; ++z) {
                for (x = 0; x < 16; ++x) {
                    assert(end_a[z * 32 + x] == end_l[z * 16 + x]);
                    assert(end_a[z * 32 + x + 16] == end_r[z * 16 + x]);
                }
            }
            assert(sax_height_area_sampled(context, SAX_DIM_END,
                       -62, -64, 4, 4, 4, hs, 100) == SAX_ERR_ARGUMENT);
            assert(sax_height_area_sampled(context, SAX_DIM_END,
                       0, 0, 16, 4, 4, hs, 100) == SAX_ERR_UNSUPPORTED);
        }
        assert(sax_height_area_sampled(context, SAX_DIM_NETHER,
                   0, 0, 4, 2, 2, hs, 4) == SAX_ERR_UNSUPPORTED);
        assert((sax_capabilities() & SAX_CAP_NETHER_FLOOR) != 0);
        {
            /* Nether floor: deterministic, in range, tile-partition exact. */
            float na[24 * 24], nb[24 * 24], nl[12 * 24], nr[12 * 24];
            int lava = 0, rock = 0;
            assert(sax_nether_floor_area_sampled(context, -48, 16, 64, 1, 24, 24, na, 576) == SAX_OK);
            assert(sax_nether_floor_area_sampled(context, -48, 16, 64, 1, 24, 24, nb, 576) == SAX_OK);
            for (i = 0; i < 576; ++i) {
                assert(na[i] == nb[i]);
                assert(na[i] >= 0.0F && na[i] <= 64.0F);
                if (na[i] < 31.0F) ++lava; else ++rock;
            }
            assert(lava > 0 && rock > 0); /* both lava sea and raised ground */
            assert(sax_nether_floor_area_sampled(context, -48, 16, 64, 1, 12, 24, nl, 288) == SAX_OK);
            assert(sax_nether_floor_area_sampled(context, -36, 16, 64, 1, 12, 24, nr, 288) == SAX_OK);
            for (z = 0; z < 24; ++z)
                for (x = 0; x < 12; ++x) {
                    assert(na[z * 24 + x] == nl[z * 12 + x]);
                    assert(na[z * 24 + x + 12] == nr[z * 12 + x]);
                }
            assert(sax_nether_floor_area_sampled(context, 0, 0, 64, 32, 2, 2, hs, 4) == SAX_ERR_UNSUPPORTED);
            assert(sax_nether_floor_area_sampled(context, 0, 0, 64, 3, 2, 2, hs, 4) == SAX_ERR_ARGUMENT);
            assert(sax_nether_floor_area_sampled(context, 0, 0, 64, 4, 2, 2, hs, 3) == SAX_ERR_ARGUMENT);
            assert(sax_nether_floor_area_sampled(NULL, 0, 0, 64, 4, 2, 2, hs, 4) == SAX_ERR_ARGUMENT);
            assert(sax_nether_floor_area_sampled(context, 0, 0, 64, 4, 2, 2, NULL, 4) == SAX_ERR_ARGUMENT);
            assert(sax_nether_floor_area_sampled(context, 0, 0, 64, 4, 8, 8, hs, 100) == SAX_OK);
        }
        assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                   0, 0, 3, 2, 2, hs, 4) == SAX_ERR_ARGUMENT);
        assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                   0, 0, 4, 2, 2, hs, 3) == SAX_ERR_ARGUMENT);
        assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                   0, 0, 4, 2, 2, NULL, 4) == SAX_ERR_ARGUMENT);
        assert(sax_height_area_sampled(NULL, SAX_DIM_OVERWORLD,
                   0, 0, 4, 2, 2, hs, 4) == SAX_ERR_ARGUMENT);
        assert(sax_height_area_sampled(context, SAX_DIM_OVERWORLD,
                   INT32_MAX - 3, 0, 4, 2, 1, hs, 4) ==
               SAX_ERR_RANGE_TOO_LARGE);
    }

    /* Compact areas preserve exact center sampling at block/detail levels. */
    {
        int32_t area_ids[32 * 24];
        uint32_t area_colors[32 * 24];
        int x, z;
        assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
                   -117, 43, 255, 1, 32, 24, area_ids, area_colors,
                   32 * 24) == SAX_OK);
        for (z = 0; z < 24; ++z) {
            for (x = 0; x < 32; ++x) {
                int index = z * 32 + x;
                assert(sax_biome_at(context, SAX_DIM_OVERWORLD,
                           -117 + x, 255, 43 + z, &biome) == SAX_OK);
                assert(area_ids[index] == biome);
                assert(area_colors[index] == sax_biome_color(biome));
            }
        }

        assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
                   -117, 43, 255, 2, 16, 12, area_ids, area_colors,
                   32 * 24) == SAX_OK);
        for (z = 0; z < 12; ++z) {
            for (x = 0; x < 16; ++x) {
                int index = z * 16 + x;
                assert(sax_biome_at(context, SAX_DIM_OVERWORLD,
                           -117 + x * 2 + 1, 255,
                           43 + z * 2 + 1, &biome) == SAX_OK);
                assert(area_ids[index] == biome);
                assert(area_colors[index] == sax_biome_color(biome));
            }
        }
    }

    /* Seed Atlas-style scaled levels stay compact across all dimensions. */
    for (i = 0; i < 3; ++i) {
        int step;
        for (step = 4; step <= 1024; step *= 2) {
            int j;
            assert(sax_biome_area_sampled(context, dimensions[i],
                       -2048, 1024, heights[i], step, 16, 8,
                       sampled_ids, sampled_colors,
                       SAX_CHUNK_SAMPLE_COUNT) == SAX_OK);
            for (j = 0; j < 16 * 8; ++j) {
                assert(sampled_ids[j] >= 0);
                assert((sampled_colors[j] & UINT32_C(0xff000000)) ==
                       UINT32_C(0xff000000));
            }
        }
    }
    assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
               1, 0, 255, 4, 16, 16, sampled_ids, sampled_colors,
               SAX_CHUNK_SAMPLE_COUNT) == SAX_ERR_ARGUMENT);
    assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
               0, 0, 255, 3, 16, 16, sampled_ids, sampled_colors,
               SAX_CHUNK_SAMPLE_COUNT) == SAX_ERR_ARGUMENT);
    assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
               0, 0, 255, 4, 16, 16, sampled_ids, sampled_colors,
               SAX_CHUNK_SAMPLE_COUNT - 1) == SAX_ERR_ARGUMENT);
    assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
               INT32_MAX - 3, 0, 255, 4, 2, 1,
               sampled_ids, sampled_colors,
               SAX_CHUNK_SAMPLE_COUNT) == SAX_ERR_RANGE_TOO_LARGE);
    assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
               INT32_MIN, 0, 255, 1, 1, 1,
               sampled_ids, sampled_colors,
               SAX_CHUNK_SAMPLE_COUNT) == SAX_ERR_RANGE_TOO_LARGE);
    assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
               0, 0, 255, 1, 4096, 1025,
               sampled_ids, sampled_colors,
               INT32_MAX) == SAX_ERR_RANGE_TOO_LARGE);

    /* Exercise negative coordinates and all supported dimensions. */
    for (i = 0; i < 3; ++i) {
        int step, x, z;
        assert(sax_biome_chunk(context, dimensions[i], -7, 3, heights[i],
                               ids, colors) == SAX_OK);
        for (step = 2; step <= 16; step *= 2) {
            assert(sax_biome_chunk_sampled(context, dimensions[i],
                                           -7, 3, heights[i], step,
                                           sampled_ids, sampled_colors) == SAX_OK);
            for (z = 0; z < 16; ++z) {
                for (x = 0; x < 16; ++x) {
                    int sample_index = ((z / step) * step + step / 2) * 16 +
                                       (x / step) * step + step / 2;
                    int index = z * 16 + x;
                    assert(sampled_ids[index] == ids[sample_index]);
                    assert(sampled_colors[index] == colors[sample_index]);
                    if (x % step == 0 && z % step == 0) {
                        assert(sax_biome_at(context, dimensions[i],
                                           -7 * 16 + x + step / 2,
                                           heights[i],
                                           3 * 16 + z + step / 2,
                                           &biome) == SAX_OK);
                        assert(sampled_ids[index] == biome);
                    }
                }
            }
        }
    }

    /* 26.3 fixtures from the pinned Seed Atlas engine regression suite. */
    assert(sax_biome_at(context, SAX_DIM_OVERWORLD,
                        -1184, 100, 1248, &biome) == SAX_OK);
    assert(biome == 188); /* Dappled Forest */
    assert(sax_biome_name(biome, name, sizeof(name)) > 1);
    assert(strcmp(name, "dappled_forest") == 0);
    assert(sax_biome_color(biome) == UINT32_C(0xffdf6827));
    assert(sax_biome_chunk(context, SAX_DIM_OVERWORLD,
                           -74, 78, 100, ids, colors) == SAX_OK);
    assert(ids[0] == 188 && colors[0] == UINT32_C(0xffdf6827));
    assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
               -1184, 1248, 100, 1, 1, 1, ids, colors, 1) == SAX_OK);
    assert(ids[0] == 188 && colors[0] == UINT32_C(0xffdf6827));

    count = sax_scan_structures(context, SAX_ABANDONED_CAMP,
                                288, 32, 288, 32, 0, results, 64);
    assert(count == 1 && results[5] == -1); /* unchecked: variant unknown */
    count = sax_scan_structures(context, SAX_ABANDONED_CAMP,
                                -512, 416, -512, 416, 1, results, 64);
    assert(count == 1 && results[0] == SAX_ABANDONED_CAMP);
    assert(results[1] == -512 && results[3] == 416);
    assert(results[5] == 4); /* forest, regular loot */
    assert((results[4] & SAX_RESULT_APPROXIMATE) != 0);
    assert((results[4] & SAX_RESULT_BIOME_VIABLE) != 0);
    count = sax_scan_structures(context, SAX_ABANDONED_CAMP,
                                10704, -23600, 10704, -23600, 0, results, 64);
    assert(count == 1);
    count = sax_scan_structures(context, SAX_ABANDONED_CAMP,
                                10704, -23600, 10704, -23600, 1, results, 64);
    assert(count == 0); /* biome boundary: reject wrong chunk-centre sample */
    assert(sax_scan_structures(context, SAX_ABANDONED_CAMP,
                -512, 416, -512, 416, 1, NULL, 0) == 1); /* truncated */
    {
        sax_context *camp = sax_create(14, SAX_WORLD_NORMAL);
        assert(camp != NULL);
        count = sax_scan_structures(camp, SAX_ABANDONED_CAMP,
                                    -832, -2128, -832, -2128, 1, results, 64);
        assert(count == 1);
        assert((results[5] & SAX_CAMP_BIOME_MASK) == 30); /* snowy taiga */
        assert((results[5] & SAX_CAMP_SPECIAL_LOOT) != 0);
        sax_destroy(camp);
    }
    {
        sax_context *large = sax_create(8371904829LL, SAX_WORLD_LARGE_BIOMES);
        assert(large != NULL);
        assert(sax_biome_area_sampled(large, SAX_DIM_OVERWORLD,
                   -4096, 2048, 100, 16, 16, 16, ids, colors, 256) == SAX_OK);
        count = sax_scan_structures(large, SAX_ABANDONED_CAMP,
                                    -4096, -4096, 4096, 4096, 1, results, 64);
        assert(count > 0);
        for (i = 0; i < count && i < 64; ++i) {
            assert(results[i * SAX_RESULT_STRIDE] == SAX_ABANDONED_CAMP);
            assert(results[i * SAX_RESULT_STRIDE + 5] >= 0);
        }
        sax_destroy(large);
    }

    /* Known 26.3 generation attempt from seedatlas-engine tests_versions.c. */
    count = sax_scan_structures(context, SAX_RUINED_PORTAL,
                                272, 48, 272, 48, 0, results, 64);
    assert(count == 1);
    assert(results[1] == 272 && results[3] == 48);

    /* These starts have Vanilla terrain/block gates that cubiomes cannot
       reproduce exactly. Keep them rather than applying the known
       false-negative-prone depth heuristic, and label them approximate. */
    count = sax_scan_structures(context, SAX_DESERT_PYRAMID,
                                -3456, 80, -3456, 80, 1, results, 64);
    assert(count == 1);
    assert((results[4] & SAX_RESULT_APPROXIMATE) != 0);
    count = sax_scan_structures(context, SAX_JUNGLE_TEMPLE,
                                -1712, 352, -1712, 352, 1, results, 64);
    assert(count == 1);
    assert((results[4] & SAX_RESULT_APPROXIMATE) != 0);
    count = sax_scan_structures(context, SAX_MANSION,
                                688, 1808, 688, 1808, 1, results, 64);
    assert(count == 1);
    assert((results[4] & SAX_RESULT_APPROXIMATE) != 0);
    count = sax_scan_structures(context, SAX_AMETHYST_GEODE,
                                -256, -256, 255, 255, 0, results, 64);
    assert(count > 0 && count <= 64);
    for (i = 0; i < count; ++i)
        assert((results[i * SAX_RESULT_STRIDE + 4] &
                SAX_RESULT_APPROXIMATE) != 0);
    count = sax_scan_structures(context, SAX_DESERT_WELL,
                                -1024, -1024, 1023, 1023,
                                0, results, 64);
    assert(count > 0 && count <= 64);
    for (i = 0; i < count; ++i)
        assert((results[i * SAX_RESULT_STRIDE + 4] &
                SAX_RESULT_APPROXIMATE) != 0);

    /* Vanilla rejects End City attempts whose lowest rotated 5x5 terrain
       corner is below Y=60. Keep raw scans available, but require checked
       city and ship markers to apply that dedicated terrain rule. */
    count = sax_scan_structures(context, SAX_END_CITY,
                                1072, 64, 1072, 64, 0, results, 64);
    assert(count == 1);
    count = sax_scan_structures(context, SAX_END_CITY,
                                1072, 64, 1072, 64, 1, results, 64);
    assert(count == 0);
    count = sax_scan_structures(context, SAX_END_CITY,
                                -592, -896, -592, -896, 1, results, 64);
    assert(count == 1);
    assert(results[0] == SAX_END_CITY);
    assert(results[1] == -592 && results[3] == -896);
    assert((results[4] & (SAX_RESULT_BIOME_CHECKED |
                          SAX_RESULT_BIOME_VIABLE)) ==
           (SAX_RESULT_BIOME_CHECKED | SAX_RESULT_BIOME_VIABLE));

    count = sax_scan_structures(context, SAX_END_SHIP,
                                1072, 64, 1072, 64, 0, results, 64);
    assert(count == 1);
    count = sax_scan_structures(context, SAX_END_SHIP,
                                1072, 64, 1072, 64, 1, results, 64);
    assert(count == 0);
    count = sax_scan_structures(context, SAX_END_SHIP,
                                400, -1248, 400, -1248, 1, results, 64);
    assert(count == 1);
    assert(results[0] == SAX_END_SHIP);
    assert(results[1] == 400 && results[3] == -1248);

    /* Small End islands use their dedicated placed-feature RNG: one attempt
       can create one or two islands, each with its own in-chunk X/Y/Z. */
    count = sax_scan_structures(context, SAX_END_ISLAND,
                                -2624, -3193, -2624, -3193,
                                0, results, 64);
    assert(count == 0); /* coordinate from the old generic decorator path */
    count = sax_scan_structures(context, SAX_END_ISLAND,
                                -2617, -3200, -2617, -3200,
                                1, results, 64);
    assert(count == 1);
    assert(results[0] == SAX_END_ISLAND);
    assert(results[1] == -2617 && results[2] == 66 && results[3] == -3200);
    assert((results[4] & (SAX_RESULT_BIOME_CHECKED |
                          SAX_RESULT_BIOME_VIABLE)) ==
           (SAX_RESULT_BIOME_CHECKED | SAX_RESULT_BIOME_VIABLE));
    assert((results[4] & SAX_RESULT_APPROXIMATE) == 0);
    assert(results[5] >= 4 && results[5] <= 6); /* generated radius */

    count = sax_scan_structures(context, SAX_END_ISLAND,
                                -3440, -4800, -3425, -4785,
                                1, results, 64);
    assert(count == 2);
    assert(results[1] == -3439 && results[2] == 60 && results[3] == -4800);
    assert(results[SAX_RESULT_STRIDE + 1] == -3428);
    assert(results[SAX_RESULT_STRIDE + 2] == 66);
    assert(results[SAX_RESULT_STRIDE + 3] == -4788);

    /* A raw decoration attempt outside small_end_islands must disappear
       when the caller requests Vanilla biome validation. */
    count = sax_scan_structures(context, SAX_END_ISLAND,
                                -1509, -1588, -1509, -1588,
                                0, results, 64);
    assert(count == 1);
    assert(results[2] == 64);
    count = sax_scan_structures(context, SAX_END_ISLAND,
                                -1509, -1588, -1509, -1588,
                                1, results, 64);
    assert(count == 0);

    /* Java Random.nextInt rejection is observable only for very rare seeds,
       but it applies to every LINEAR random-spread structure. Verify the ABI
       no longer exposes the old Village coordinate. */
    {
        sax_context *placement_context =
            sax_create(INT64_C(329087727717716), SAX_WORLD_NORMAL);
        assert(placement_context != NULL);
        count = sax_scan_structures(placement_context, SAX_VILLAGE,
                                    0, 384, 0, 384, 0, results, 64);
        assert(count == 0);
        count = sax_scan_structures(placement_context, SAX_VILLAGE,
                                    384, 224, 384, 224, 0, results, 64);
        assert(count == 1);
        assert(results[1] == 384 && results[3] == 224);
        count = sax_scan_structures(placement_context, SAX_VILLAGE,
                                    384, 224, 384, 224, 1, results, 64);
        assert(count == 0); /* corrected candidate has no viable biome */
        sax_destroy(placement_context);
    }

    /* Version selection for servers that run an older Minecraft release. */
    {
        int32_t newest[SAX_CHUNK_SAMPLE_COUNT];
        sax_context *older;
        assert((sax_capabilities() & SAX_CAP_MC_VERSIONS) != 0);
        assert(sax_mc_version_supported("26.2") == 1);
        assert(sax_mc_version_supported("1.18") == 1);
        assert(sax_mc_version_supported("1.17.1") == 1);
        assert(sax_mc_version_supported("1.1") == 1);
        assert(sax_mc_version_supported("1.0") == 0);
        assert(sax_mc_version_supported("not a version") == 0);
        assert(sax_mc_version_supported(NULL) == 0);
        assert(sax_create_versioned(1, SAX_WORLD_NORMAL, "Beta 1.7") == NULL);

        older = sax_create_versioned(INT64_C(8371904829), SAX_WORLD_NORMAL, "26.3");
        assert(older != NULL);
        assert(sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
                   -4096, 2048, 64, 16, 16, 16, newest, NULL, 256) == SAX_OK);
        assert(sax_biome_area_sampled(older, SAX_DIM_OVERWORLD,
                   -4096, 2048, 64, 16, 16, 16, ids, NULL, 256) == SAX_OK);
        assert(memcmp(newest, ids, sizeof(newest)) == 0); /* NULL means newest */
        sax_destroy(older);

        older = sax_create_versioned(INT64_C(8371904829), SAX_WORLD_NORMAL, "1.20");
        assert(older != NULL);
        assert(sax_biome_at(older, SAX_DIM_OVERWORLD, 0, 64, 0, &biome) == SAX_OK);
        /* Trial Chambers arrived in 1.21: an empty scan, not an error. */
        assert(sax_scan_structures(older, SAX_TRIAL_CHAMBERS,
                   -4096, -4096, 4096, 4096, 0, results, 64) == 0);
        assert(sax_scan_structures(context, SAX_TRIAL_CHAMBERS,
                   -4096, -4096, 4096, 4096, 0, results, 64) > 0);
        assert(sax_scan_structures(older, SAX_VILLAGE,
                   -4096, -4096, 4096, 4096, 0, results, 64) > 0);
        sax_destroy(older);
    }

    /* Layered generation before 1.18: every map step works, terrain is unavailable. */
    {
        const char *layered[] = {"1.17.1", "1.12.2", "1.7.10", "1.1"};
        const int32_t steps[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
        static int32_t area_ids[64 * 64];
        static uint32_t area_argb[64 * 64];
        float heights[4];
        for (int v = 0; v < 4; ++v) {
            sax_context *old = sax_create_versioned(INT64_C(8371904829), SAX_WORLD_NORMAL, layered[v]);
            assert(old != NULL);
            assert(sax_context_has_terrain(old) == 0);
            for (int s = 0; s < 11; ++s) {
                int32_t step = steps[s];
                assert(sax_biome_area_sampled(old, SAX_DIM_OVERWORLD, -32 * step, 64 * step, 64,
                    step, 64, 64, area_ids, area_argb, 64 * 64) == SAX_OK);
                for (i = 0; i < 64 * 64; ++i)
                    assert(area_ids[i] >= 0 && area_argb[i] != 0);
            }
            if (v == 0) {
                /* A coarse step keeps the centre samples of the finer layer. */
                static int32_t fine[128 * 128];
                assert(sax_biome_area_sampled(old, SAX_DIM_OVERWORLD, 0, 0, 64, 4, 128, 128,
                    fine, NULL, 128 * 128) == SAX_OK);
                assert(sax_biome_area_sampled(old, SAX_DIM_OVERWORLD, 0, 0, 64, 8, 64, 64,
                    area_ids, NULL, 64 * 64) == SAX_OK);
                for (i = 0; i < 64 * 64; ++i)
                    assert(area_ids[i] == fine[(i / 64 * 2 + 1) * 128 + i % 64 * 2 + 1]);
            }
            assert(sax_height_area_sampled(old, SAX_DIM_OVERWORLD, 0, 0, 4, 2, 2, heights, 4) == SAX_ERR_UNSUPPORTED);
            assert(sax_height_area_sampled(old, SAX_DIM_END, 0, 0, 4, 2, 2, heights, 4) == SAX_ERR_UNSUPPORTED);
            assert(sax_nether_floor_area_sampled(old, 0, 0, 64, 4, 2, 2, heights, 4) == SAX_ERR_UNSUPPORTED);
            assert(sax_scan_structures(old, SAX_ORE_VEIN, -512, -512, 512, 512, 0, results, 64) == 0);
            assert(sax_scan_structures(old, SAX_VILLAGE, -4096, -4096, 4096, 4096, 0, results, 64) > 0);
            assert(sax_scan_strongholds(old, -30000, -30000, 30000, 30000, 0, results, 64) > 0);
            assert(sax_spawn(old, 0, position) == SAX_OK);
            sax_destroy(old);
        }
        {
            sax_context *old = sax_create_versioned(INT64_C(8371904829), SAX_WORLD_NORMAL, "1.17.1");
            /* id 3 is "mountains" before 1.18 and "windswept_hills" since. */
            assert(sax_context_biome_name(old, 3, name, sizeof(name)) > 1 && strcmp(name, "mountains") == 0);
            assert(sax_context_biome_name(context, 3, name, sizeof(name)) > 1 && strcmp(name, "windswept_hills") == 0);
            assert(sax_context_has_terrain(context) == 1);
            sax_destroy(old);
        }
    }

    assert(sax_spawn(context, 0, position) == SAX_OK);
    count = sax_scan_strongholds(context, -30000, -30000, 30000, 30000,
                                 0, results, 64);
    assert(count > 0);
    count = sax_scan_slime_chunks(context, -256, -256, 255, 255,
                                  results, 64);
    assert(count > 0);

    sax_destroy(context);
    puts("seedatlas-sax ABI tests passed");
    return 0;
}
