/* Checks the GPU prefilter against the exact CPU tests: every seed that passes
 * a climate or sample test on the CPU must also pass on the GPU. Also prints
 * the speed.
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

// The CPU side of a sample test: the stopping rule of monteCarloBiomes(), with
// the positions whose climate is within the bounds as the successes (as
// biomeMayBeIncluded() in search.cpp; the real biomes succeed at most there).
struct SampleEval { const GpuClimateFilter::SampleTest *t; BiomeNoise *bn; };

static int evalSample(Generator *, int scale, int x, int, int z, void *data)
{
    const SampleEval *e = (const SampleEval*) data;
    if (e->t->rmaxsq)
    {
        int64_t dx = x * scale, dz = z * scale;
        if (dx*dx + dz*dz >= e->t->rmaxsq)
            return -1;
    }
    double px, pz;
    getBiomeNoisePos(e->bn, x, z, 0, &px, &pz);
    for (size_t i = 0; i < e->t->paras.size(); i++)
    {
        int64_t v = sampleBiomeNoisePara(e->bn, e->t->paras[i], px, pz);
        if (v < e->t->lo[i] || v > e->t->hi[i])
            return 0;
    }
    return 1;
}

static int checkSamples(uint32_t ncpu, uint32_t ngpu)
{
    struct Case { const char *name; int mc; int biome; int r; double cov; int large; };
    const Case cases[] = {
        { "mushroom 50%, r100",        MC_1_21_5, mushroom_fields, 100, 0.5, 0 },
        { "mushroom 90%, r1000",       MC_1_21_5, mushroom_fields, 1000, 0.9, 0 },
        { "jungle 30%, r200",          MC_1_21_5, jungle, 200, 0.3, 0 },
        { "desert 40%, r100",          MC_1_21_5, desert, 100, 0.4, 0 },
        { "ice spikes 40%, r300",      MC_1_21_5, ice_spikes, 300, 0.4, 0 },
        { "mushroom 50%, r400, large", MC_1_21_5, mushroom_fields, 400, 0.5, 1 },
    };
    int failures = 0;
    for (const Case& c : cases)
    {
        uint64_t set[4] = { 0, 0, 0, 0 };
        set[c.biome >= 128 ? 2 : 0] |= 1ULL << (c.biome & 63);
        BiomeClimateBounds cb;
        if (!getBiomeClimateBounds(&cb, c.mc, set))
        {
            printf("%-34s no climate bounds\n", c.name);
            failures++;
            continue;
        }
        GpuClimateFilter::SampleTest t;
        int r = c.r - 1;
        t.x = t.z = -r >> 2;
        t.w = t.h = (r >> 2) - t.x + 1;
        t.rmaxsq = (int64_t) r * r + 1;
        t.coverage = c.cov;
        t.confidence = 0.95f;
        for (int p = 0; p < NP_MAX; p++)
        {
            if (p == NP_SHIFT || (cb.lo[p] <= -6000 && cb.hi[p] >= 6000))
                continue;
            t.paras.push_back(p);
            t.lo.push_back(cb.lo[p]);
            t.hi.push_back(cb.hi[p]);
        }
        GpuClimateFilter filter;
        GpuClimateFilter::Tests tests;
        tests.samples.push_back(t);
        if (t.paras.empty() || !filter.init(tests, c.large))
        {
            printf("%-34s init failed\n", c.name);
            failures++;
            continue;
        }
        const uint64_t seed0 = 123456789;
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

        BiomeNoise bn;
        SampleEval e = { &t, &bn };
        Range rg = {4, t.x, t.z, t.w, t.h, 0, 1};
        uint32_t cpupass = 0, missed = 0;
        t0 = now();
        for (uint32_t i = 0; i < ncpu && i < ngpu; i++)
        {
            uint64_t s = seed0 + i;
            setBiomeSeed(&bn, s, c.large);
            uint64_t rng;
            setSeed(&rng, s);
            bool ok = monteCarloBiomes(NULL, rg, &rng, t.coverage, t.confidence, evalSample, &e) == 1;
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
    return failures;
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
        GpuClimateFilter::Tests tests;
        tests.climate.push_back(c.t);
        if (!filter.init(tests, c.large))
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
    failures += checkSamples(ncpu, ngpu);
    printf(failures ? "FAILED\n" : "OK: no seed that passes on the CPU was removed\n");
    return failures ? 1 : 0;
}
