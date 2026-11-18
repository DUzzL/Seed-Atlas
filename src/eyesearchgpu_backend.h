#ifndef EYESEARCHGPU_BACKEND_H
#define EYESEARCHGPU_BACKEND_H

// Interface between the GPU eye search (eyesearchgpu.cpp) and the GPU APIs
// (Metal, Vulkan). The structs are shared with the kernel (eyesearchgpu.comp).

#include <cstddef>
#include <cstdint>
#include <vector>

// The kernel's constants (eyesearchgpu.comp).
const uint32_t GPU_EYE_MAX_TESTS = 12;
const uint32_t GPU_EYE_STD_TABLES = 12;     // the shared skips 760 .. 771
const uint32_t GPU_EYE_BYTE_WORDS = 4096;   // byte table: 16 bytes x 256 values
const uint32_t GPU_EYE_NIBBLE_WORDS = 512;  // nibble table: 32 nibbles x 16 values
const uint32_t GPU_EYE_THREADS = 256;       // TG, threads per workgroup

struct GpuEyeTest {
    int32_t mx, mz;         // 16 * chunk x, 16 * chunk z of the frame chunk
    uint32_t table;         // table of the skip (< GpuEyeParams.nbyte: a byte table)
    uint32_t pad;
};

struct GpuEyePlan {
    uint32_t ntests, k;     // tests, allowed misses
    uint32_t unit;          // of unit 'index' (not of this plan): start | count << 16
    uint32_t pad;
    GpuEyeTest test[GPU_EYE_MAX_TESTS];
};

struct GpuEyeParams {
    uint64_t low48, salt;
    uint32_t h0, h1;        // seed highs h0 .. h1-1
    uint32_t nplans;
    uint32_t maxOut;        // capacity of the output (pairs)
    uint32_t groupsPerPlan; // pass 1: workgroups per plan
    uint32_t seedsPerGroup; // pass 1: seeds per workgroup
    uint32_t failMax;       // coarse sum + 2 <= failMax: certainly no eye
    uint32_t maxList;       // capacity of the list for pass 2 (pairs)
    uint32_t nunits;        // pass 1: units of plans with a shared first test
    uint32_t firstRange;    // pass 1: the first plan that is not in a unit
    uint32_t nbyte;         // byte tables (first in the table buffer), then nibble tables
                            // (see eyesearchgpu.comp for the layout)
    uint32_t pad;
};

// std430 / std140 layouts of the kernel's buffers
static_assert(sizeof(GpuEyeTest) == 16, "GpuEyeTest layout");
static_assert(sizeof(GpuEyePlan) == 16 + 12 * 16, "GpuEyePlan layout");
static_assert(sizeof(GpuEyeParams) == 64 && offsetof(GpuEyeParams, h0) == 16 &&
              offsetof(GpuEyeParams, nunits) == 48, "GpuEyeParams layout");

// Result of GpuEyeBackend::run().
enum GpuEyeRunResult
{
    GPU_EYE_RUN_OK,         // the kept pairs were appended
    GPU_EYE_RUN_OVERFLOW,   // the output or the list was too small: need[] has the sizes
    GPU_EYE_RUN_FAILED,     // the GPU failed (error, timeout, lost device)
};

class GpuEyeBackend
{
public:
    virtual ~GpuEyeBackend() {}
    virtual bool init() = 0;
    // Host visible input buffers, valid until the next call of the same
    // function (which may reallocate); nullptr if out of memory. A
    // reallocated table buffer keeps its first min(old, new) words, so
    // constant tables at its start need to be written only once.
    virtual uint64_t *deco() = 0;                       // 2 * 65536 values
    virtual GpuEyePlan *plans(uint32_t n) = 0;
    virtual uint32_t *tables(uint32_t words) = 0;
    // Runs both passes (pass 1 with (p.nunits + p.nplans - p.firstRange) *
    // p.groupsPerPlan workgroups, pass 2 with 'listGroups') and appends the
    // kept pairs (plan << 16 | high). On GPU_EYE_RUN_OVERFLOW, need[0] and
    // need[1] are the numbers of kept pairs and list entries the run
    // produced (more than p.maxOut or p.maxList); nothing is appended.
    // Buffers are sized for the parameters of each run and shrink again when
    // much smaller runs follow.
    virtual GpuEyeRunResult run(const GpuEyeParams& p, uint32_t listGroups,
        std::vector<uint32_t>& kept, uint32_t need[2]) = 0;
};

// Implemented by the backend of the platform (or the stub).
bool gpuEyeBackendAvailable();
GpuEyeBackend *gpuEyeBackendCreate();

#endif // EYESEARCHGPU_BACKEND_H
