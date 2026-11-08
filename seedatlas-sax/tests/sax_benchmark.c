#include "seedatlas_sax.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static double now_milliseconds(void)
{
#ifdef _WIN32
    /* MinGW's legacy MSVCRT target does not provide C11 timespec_get. */
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter)) {
        fputs("Could not read the performance counter\n", stderr);
        exit(EXIT_FAILURE);
    }
    return (double) counter.QuadPart * 1000.0 / (double) frequency.QuadPart;
#else
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
#endif
}

static uint64_t checksum(const uint32_t *values, int count)
{
    uint64_t result = 0;
    int i;
    for (i = 0; i < count; ++i)
        result = result * UINT64_C(1099511628211) ^ values[i];
    return result;
}

static int compare_doubles(const void *left, const void *right)
{
    double a = *(const double *) left;
    double b = *(const double *) right;
    return (a > b) - (a < b);
}

int main(void)
{
    static const int lod_steps[] = {1, 4, 16, 64, 256, 512, 1024};
    const int area_size = 512;
    const int area_samples = area_size * area_size;
    sax_context *context = sax_create(INT64_C(8371904829), SAX_WORLD_NORMAL);
    int32_t *ids = (int32_t *) malloc(sizeof(*ids) * area_samples);
    uint32_t *colors = (uint32_t *) malloc(sizeof(*colors) * area_samples);
    int32_t chunk_ids[SAX_CHUNK_SAMPLE_COUNT];
    uint32_t chunk_colors[SAX_CHUNK_SAMPLE_COUNT];
    double started;
    double elapsed;
    uint64_t digest = 0;
    int repeat, chunk_x, chunk_z;

    if (!context || !ids || !colors)
        return 2;

    sax_biome_area_sampled(context, SAX_DIM_OVERWORLD, -4096, -4096,
                           255, 1, area_size, area_size,
                           ids, colors, area_samples);

    started = now_milliseconds();
    for (repeat = 0; repeat < 3; ++repeat) {
        for (chunk_z = -256; chunk_z < -224; ++chunk_z) {
            for (chunk_x = -256; chunk_x < -224; ++chunk_x) {
                if (sax_biome_chunk(context, SAX_DIM_OVERWORLD,
                                    chunk_x, chunk_z, 255,
                                    chunk_ids, chunk_colors) != SAX_OK)
                    return 3;
                digest ^= checksum(chunk_colors, SAX_CHUNK_SAMPLE_COUNT);
            }
        }
    }
    elapsed = now_milliseconds() - started;
    printf("1024 individual 16x16 calls: %.3f ms/run\n", elapsed / 3.0);

    started = now_milliseconds();
    for (repeat = 0; repeat < 3; ++repeat) {
        if (sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
                                   -4096, -4096, 255, 1,
                                   area_size, area_size, ids, colors,
                                   area_samples) != SAX_OK)
            return 4;
        digest ^= checksum(colors, area_samples);
    }
    elapsed = now_milliseconds() - started;
    printf("one compact 512x512 area call: %.3f ms/run\n", elapsed / 3.0);

    for (size_t lod = 0; lod < sizeof(lod_steps) / sizeof(lod_steps[0]); ++lod) {
        int step = lod_steps[lod];
        const int samples = 256 * 256;
        double timings[11];
        for (repeat = 0; repeat < 11; ++repeat) {
            started = now_milliseconds();
            if (sax_biome_area_sampled(context, SAX_DIM_OVERWORLD,
                                       -65536, -65536, 255, step,
                                       256, 256, ids, colors,
                                       samples) != SAX_OK)
                return 5;
            timings[repeat] = now_milliseconds() - started;
            digest ^= checksum(colors, samples);
        }
        qsort(timings, 11, sizeof(timings[0]), compare_doubles);
        printf("256x256 samples at 1:%d: median %.3f ms, %.3f Msample/s\n",
               step, timings[5], samples / timings[5] / 1000.0);
    }

    printf("checksum: %" PRIu64 "\n", digest);
    free(colors);
    free(ids);
    sax_destroy(context);
    return 0;
}
