/* Include the implementation to check internal densities without adding a test ABI. */
#include "../seedatlas_sax.c"
#include <assert.h>
#include <stdio.h>
#include "nether_vanilla_26_3.h"

int main(void)
{
    double max_base_error = 0.0, max_density_error = 0.0;
    const int layers[] = {64, 100, 127};
    const size_t count = sizeof(nether_vanilla) / sizeof(nether_vanilla[0]);
    for (size_t i = 0; i < count; ++i) {
        sax_context *context = sax_create(nether_vanilla[i].seed, SAX_WORLD_NORMAL);
        int x = nether_vanilla[i].x, z = nether_vanilla[i].z;
        int cx = floor_div(x, 4), cz = floor_div(z, 4);
        double columns[4][17], col[17];
        double tx = (x - cx * 4) / 4.0, tz = (z - cz * 4) / 4.0;
        assert(context);
        for (int corner = 0; corner < 4; ++corner)
            for (int j = 0; j <= 16; ++j)
                columns[corner][j] = nether_density(nether_noise(&context->nether_noise,
                    cx + (corner & 1), j, cz + (corner >> 1)) / 128.0, j * 8);
        for (int j = 0; j <= 16; ++j) {
            double base = sampleSurfaceNoise(&context->nether_noise, cx, j, cz) / 128.0;
            double error = fabs(base - nether_vanilla[i].base[j]);
            if (error > max_base_error) max_base_error = error;
            assert(error < 2e-6); /* Vanilla 26.3 Perlin/FBM evaluation uses floats. */
            double top = columns[0][j] + (columns[1][j] - columns[0][j]) * tx;
            double bottom = columns[2][j] + (columns[3][j] - columns[2][j]) * tx;
            col[j] = top + (bottom - top) * tz;
        }
        for (int j = 0; j <= 16; ++j) {
            int y = j == 16 ? 127 : j * 8;
            double value = col[y >> 3] + (col[(y >> 3) + 1] - col[y >> 3]) * ((y & 7) / 8.0);
            double clamped = fmax(-1.0, fmin(1.0, value));
            double squeezed = clamped / 2.0 - clamped * clamped * clamped / 24.0;
            double error = fabs(squeezed - nether_vanilla[i].density[j]);
            if (error > max_density_error) max_density_error = error;
            assert(error < 2e-6);
        }
        const int expected[] = {nether_vanilla[i].floor64, nether_vanilla[i].floor100, nether_vanilla[i].floor127};
        for (int layer = 0; layer < 3; ++layer) {
            float height;
            assert(sax_nether_floor_area_sampled(context, x, z, layers[layer], 1, 1, 1, &height, 1) == SAX_OK);
            assert(height == expected[layer]);
        }
        sax_destroy(context);
    }
    printf("Vanilla 26.3: %zu base values, %zu final densities, %zu cave floors; max errors %.9g / %.9g\n",
           count * 17, count * 17, count * 3, max_base_error, max_density_error);
    /* The octave-skipping noise must equal the engine's full evaluation bit for bit. */
    {
        sax_context *context = sax_create(INT64_C(2842062689604764280), SAX_WORLD_NORMAL);
        long skipped = 0;
        for (int cx = -300; cx <= 300; cx += 7)
            for (int cz = -300; cz <= 300; cz += 11)
                for (int j = 0; j <= 16; ++j) {
                    double full = sampleSurfaceNoise(&context->nether_noise, cx, j, cz);
                    double fast = nether_noise(&context->nether_noise, cx, j, cz);
                    assert(memcmp(&full, &fast, sizeof(full)) == 0);
                    skipped++;
                }
        sax_destroy(context);
        printf("Octave-skipping Nether noise matches the engine at %ld points\n", skipped);
    }
    /* End heights on the shared island grid must equal the engine's mapEndSurfaceHeight. */
    {
        static float full[66 * 66], fast[66 * 66];
        const int64_t seeds[] = {INT64_C(2842062689604764280), INT64_C(8371904829), -7};
        const int scales[] = {1, 2, 4, 8};
        const int origins[][2] = {{0, 0}, {-130, 96}, {300, -517}, {-2001, -1333}, {9000, 12345}, {-40000, 3}};
        int compared = 0;
        for (int s_i = 0; s_i < 3; ++s_i) {
            sax_context *context = sax_create(seeds[s_i], SAX_WORLD_NORMAL);
            for (int c = 0; c < 4; ++c)
                for (int o = 0; o < 6; ++o) {
                    int scale = scales[c], w = 66 - c * 5, h = 66 - o * 3;
                    int x = origins[o][0] / scale, z = origins[o][1] / scale;
                    assert(mapEndSurfaceHeight(full, &context->biome_generators[2].en,
                        &context->end_height_noise, x, z, w, h, scale, 0) == 0);
                    assert(end_surface_area(fast, &context->biome_generators[2].en,
                        &context->end_height_noise, x, z, w, h, scale) == SAX_OK);
                    assert(memcmp(full, fast, sizeof(float) * w * h) == 0);
                    compared += w * h;
                }
            sax_destroy(context);
        }
        printf("End island grid heights match the engine at %d samples\n", compared);
    }
    sax_context *reported = sax_create(INT64_C(2842062689604764280), SAX_WORLD_NORMAL);
    float raw, carved_height;
    /* Actual vanilla carver opens Y102/103 here; the exposed floor is 101, not 62. */
    uint64_t mask[2] = {0, (UINT64_C(1) << (102 - 64)) | (UINT64_C(1) << (103 - 64))};
    assert(sax_nether_floor_area_sampled(reported, -200, 14, 127, 1, 1, 1, &raw, 1) == SAX_OK);
    assert(raw == 62);
    assert(sax_nether_floor_area_carved_sampled(reported, -200, 14, 127, 1, 1, 1,
        mask, 2, &carved_height, 1) == SAX_OK);
    assert(carved_height == 101);
    assert(sax_nether_floor_area_carved_sampled(reported, -200, 14, 127, 1, 1, 1,
        mask, 1, &carved_height, 1) == SAX_ERR_ARGUMENT);
    assert(sax_nether_floor_area_carved_sampled(reported, -200, 14, 127, 1, 1, 1,
        NULL, 2, &carved_height, 1) == SAX_ERR_ARGUMENT);
    mask[1] = 0;
    assert(sax_nether_floor_area_carved_sampled(reported, -200, 14, 127, 1, 1, 1,
        mask, 2, &carved_height, 1) == SAX_OK);
    assert(carved_height == raw);
    sax_destroy(reported);
    puts("Nether carved-air floor and mask bounds tests passed");
    return 0;
}
