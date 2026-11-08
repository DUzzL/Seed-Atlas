#ifndef GPUFILTER_H
#define GPUFILTER_H

#include <cstdint>
#include <vector>

struct ConditionTree;

/* Experimental GPU prefilter for searches with 1.18+ climate parameter
 * conditions (macOS with Metal only, enabled with SEED_ATLAS_GPU=1).
 *
 * The GPU rejects seeds whose climate noise cannot reach a required range of
 * a climate condition that has to hold at the search origin. It builds the
 * noise permutations exactly and samples the noise on a grid in single
 * precision; the grid values are widened by a bound on the noise gradient and
 * a margin for the rounding errors. Only seeds the exact CPU test would reject
 * as well are removed, and the remaining seeds are tested on the CPU as usual,
 * so the results do not change.
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

    // Is the GPU filter enabled and available on this system?
    static bool enabled();

    // The tests that can serve as a prefilter for a condition tree.
    static std::vector<Test> analyze(const ConditionTree& tree, int mc);

    GpuClimateFilter();
    ~GpuClimateFilter();
    GpuClimateFilter(const GpuClimateFilter&) = delete;
    GpuClimateFilter& operator=(const GpuClimateFilter&) = delete;

    bool init(const std::vector<Test>& tests, bool large);

    // Appends the seeds in [start, start + count) that the filter keeps to
    // passed, in ascending order. Returns false if the GPU failed, in which
    // case all seeds have to be tested on the CPU.
    bool filter(uint64_t start, uint32_t count, std::vector<uint64_t>& passed);

private:
    struct Impl;
    Impl *d;
};

#endif // GPUFILTER_H
