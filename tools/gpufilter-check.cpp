/* Checks the GPU prefilter against the exact CPU test: every seed that passes
 * a climate test on the CPU must also pass on the GPU. Also prints the speed.
 * Build the engine first: make -C seedatlas-engine release CFLAGS=-DSTRUCT_CONFIG_OVERRIDE=1
 *
 * macOS (Metal):
 *   c++ -std=gnu++17 -O2 -fobjc-arc -fwrapv -ffp-contract=off -DSTRUCT_CONFIG_OVERRIDE=1
 *     -I. -Isrc -Iseedatlas-engine tools/gpufilter-check.cpp src/gpufilter.cpp
 *     -x objective-c++ src/gpufilter_metal.mm -x none seedatlas-engine/libseedatlas-engine.a
 *     -framework Metal -framework Foundation -o gpufilter-check
 *
 * Windows, Linux (Vulkan; SEED_ATLAS_GPU_ALLOW_CPU=1 also accepts lavapipe):
 *   c++ -std=gnu++17 -O2 -fwrapv -ffp-contract=off -DSTRUCT_CONFIG_OVERRIDE=1
 *     -I. -Isrc -Iseedatlas-engine tools/gpufilter-check.cpp src/gpufilter.cpp
 *     src/gpufilter_vulkan.cpp seedatlas-engine/libseedatlas-engine.a -ldl -o gpufilter-check
 *
 * Usage: gpufilter-check [seeds tested on the CPU] [seeds for the GPU speed]
 */

#include "gpufilter.h"
#include "seedatlas-engine/finders.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>

extern "C" int getStructureConfig_override(int stype, int mc, StructureConfig *sconf)
{
    return getStructureConfig(stype, mc, sconf);
}

static double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
    uint32_t ncpu = argc > 1 ? atoi(argv[1]) : 5000;   // seeds checked on the CPU
    uint32_t ngpu = argc > 2 ? atoi(argv[2]) : 1 << 18; // seeds for the speed
#if defined(_WIN32)
    _putenv_s("SEED_ATLAS_GPU", "1");
#else
    setenv("SEED_ATLAS_GPU", "1", 1);
#endif
    if (!GpuClimateFilter::enabled())
    {
        printf("no GPU backend available\n");
        return 1;
    }
    struct Case { const char *name; GpuClimateFilter::Test t; int large; };
    const double lo = -2147483648.0, hi = 2147483647.0;
    const Case cases[] = {
        { "temperature hot and cold, 88^2",   { NP_TEMPERATURE, -44, -44, 88, 88, 5500, -4501 }, 0 },
        { "temperature >= 2000, 64^2",        { NP_TEMPERATURE, -32, -32, 64, 64, 2000, hi }, 0 },
        { "humidity <= -3500, 64^2",          { NP_HUMIDITY, -32, -32, 64, 64, lo, -3500 }, 0 },
        { "continentalness >= 3000, 32^2",    { NP_CONTINENTALNESS, -16, -16, 32, 32, 3000, hi }, 0 },
        { "erosion >= 4500, 40^2",            { NP_EROSION, -20, -20, 40, 40, 4500, hi }, 0 },
        { "weirdness spread, 100^2",          { NP_WEIRDNESS, -50, -50, 100, 100, 6000, -6000 }, 0 },
        { "temperature >= 2000, far out",     { NP_TEMPERATURE, 250000, -250000, 64, 64, 2000, hi }, 0 },
        { "continentalness >= 3000, far out", { NP_CONTINENTALNESS, -1000000, 1000000, 32, 32, 3000, hi }, 0 },
        { "temperature spread, large biomes", { NP_TEMPERATURE, -44, -44, 88, 88, 3000, -3000 }, 1 },
        { "temperature spread, 351^2",        { NP_TEMPERATURE, -175, -175, 351, 351, 5500, -4501 }, 0 },
    };
    int failures = 0;
    for (const Case& c : cases)
    {
        GpuClimateFilter filter;
        if (!filter.init({ c.t }, c.large))
        {
            printf("%-34s init failed\n", c.name);
            failures++;
            continue;
        }
        const uint64_t seed0 = 77777;
        std::vector<uint64_t> passed;
        double t0 = now();
        if (!filter.filter(seed0, ngpu, passed))
        {
            printf("%-34s filter failed\n", c.name);
            failures++;
            continue;
        }
        double tgpu = now() - t0;
        std::set<uint64_t> kept(passed.begin(), passed.end());

        // the exact test of the search (see F_CLIMATE_NOISE in search.cpp)
        BiomeNoise bn;
        uint32_t cpupass = 0, missed = 0;
        t0 = now();
        for (uint32_t i = 0; i < ncpu && i < ngpu; i++)
        {
            uint64_t s = seed0 + i;
            setClimateParaSeed(&bn, s, c.large, c.t.para, -1);
            double pmin, pmax;
            getParaRange(&bn.climate[c.t.para], &pmin, &pmax, c.t.x, c.t.z, c.t.w, c.t.h, NULL, NULL);
            bool ok = pmin <= c.t.lmax && pmax >= c.t.lmin;
            cpupass += ok;
            if (ok && !kept.count(s))
                missed++;
        }
        double tcpu = now() - t0;
        failures += missed != 0;
        printf("%-34s GPU %6.2f M/s, passes %5.1f%% | CPU %6.3f M/s per thread, passes %5.1f%% | missed %u\n",
            c.name, ngpu / tgpu / 1e6, 100.0 * passed.size() / ngpu,
            ncpu / tcpu / 1e6, 100.0 * cpupass / ncpu, missed);
    }
    printf(failures ? "FAILED\n" : "OK: no seed that passes on the CPU was removed\n");
    return failures ? 1 : 0;
}
