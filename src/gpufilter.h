#ifndef GPUFILTER_H
#define GPUFILTER_H

#include <cstdint>
#include <memory>
#include <vector>

class GpuBackend;

/* GPU prefilter for searches with 1.18+ climate parameter and biome sample
 * conditions: Metal on macOS, Vulkan on Windows and Linux. A search uses it
 * where it is faster than the CPU (see enabled() and forced()).
 *
 * A climate test rejects seeds whose climate noise cannot reach a required
 * range of a climate condition that has to hold at the search origin. It
 * builds the noise permutations exactly and samples the noise on a grid in
 * single precision; the grid values are widened by a bound on the noise
 * gradient and a margin for the rounding errors.
 *
 * A sample test rejects seeds that fail a biome sample condition at the
 * origin for sure: it draws the positions of monteCarloBiomes() exactly and
 * lets only those succeed whose climate can be within the bounds of the
 * included biomes, following the exact stopping rule of the CPU.
 *
 * Only seeds the exact CPU test would reject as well are removed, and the
 * remaining seeds are tested on the CPU as usual, so the results do not
 * change.
 */
class GpuClimateFilter
{
public:
    struct Test
    {
        int para;               // climate parameter (NP_*)
        int x, z, w, h;         // area in quart (1:4) coordinates
        double lmin, lmax;      // the range of the noise has to overlap this
    };

    struct SampleTest
    {
        int x, z, w, h;         // sampled area in quart coordinates
        int64_t rmaxsq;         // squared radius in blocks around the origin, or 0
        double coverage, confidence;
        std::vector<int> paras; // climate parameters with bounds
        std::vector<int> lo, hi; // their bounds (as sampleBiomeNoisePara())
    };

    struct Tests
    {
        std::vector<Test> climate;
        std::vector<SampleTest> samples;
        bool empty() const { return climate.empty() && samples.empty(); }
    };

    // May searches use the GPU? Yes, unless the user switched it off or
    // SEED_ATLAS_GPU=0 is set, and only if the system has a usable GPU.
    static bool enabled();
    // Use the GPU even where it is slower than the CPU (SEED_ATLAS_GPU=force,
    // for testing)?
    static bool forced();
    // The user setting (the GPU is used by default).
    static void setAllowed(bool allowed);

    GpuClimateFilter();
    ~GpuClimateFilter();
    GpuClimateFilter(const GpuClimateFilter&) = delete;
    GpuClimateFilter& operator=(const GpuClimateFilter&) = delete;

    // Prepares the filter; false where the GPU cannot take the tests or
    // fails its self test (the first time a filter is set up, the GPU has to
    // keep every seed that passes a few exact CPU tests).
    bool init(const Tests& tests, bool large);

    // Appends the seeds in [start, start + count) that the filter keeps to
    // passed, in ascending order. Returns false if the GPU failed, in which
    // case all seeds have to be tested on the CPU.
    bool filter(uint64_t start, uint32_t count, std::vector<uint64_t>& passed);

    // The exact CPU tests that the GPU tests prefilter: does the seed pass?
    static bool cpuPasses(const Test& t, bool large, uint64_t seed);
    static bool cpuPasses(const SampleTest& t, bool large, uint64_t seed);

private:
    bool initBackend(const Tests& tests, bool large);
    static bool selfTest();

    std::unique_ptr<GpuBackend> backend;
};

#endif // GPUFILTER_H
