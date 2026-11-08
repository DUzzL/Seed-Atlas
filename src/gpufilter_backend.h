#ifndef GPUFILTER_BACKEND_H
#define GPUFILTER_BACKEND_H

// Interface between GpuClimateFilter and the GPU APIs (Metal, Vulkan). The
// structs are shared with the kernel (gpufilter.comp).

#include <cstddef>
#include <cstdint>
#include <vector>

// The kernel reads at most this many tests and octaves.
const uint32_t GPU_MAX_TESTS = 8;
const uint32_t GPU_MAX_OCTS = GPU_MAX_TESTS * 9;

struct GpuOct {
    uint64_t mlo, mhi;      // md5 salt of the octave
    float amp, lac;         // amplitude and lacunarity
    float pad0, pad1;
};

struct GpuTest {
    uint64_t clo, chi;      // md5 salt of the climate parameter
    int32_t step[4];        // grid spacing of each level
    float bound[4];         // gradient bound * grid radius + margin of each level
    uint32_t noct, oct0;    // octaves per octave noise, first in the octave buffer
    float dpamp, margin;    // double Perlin amplitude, rounding margin
    float lmin, lmax;       // the noise range has to overlap this range
    int32_t x, z, w, h;     // area in quart coordinates
    uint32_t nlevels;       // grid levels, coarse to fine
    uint32_t pad;
};

struct GpuParams {
    uint64_t seed0;         // first seed
    uint32_t count, ntests; // number of seeds and tests
};

// std140 layouts of the kernel's uniform buffers
static_assert(sizeof(GpuOct) == 32, "GpuOct layout");
static_assert(sizeof(GpuTest) == 96 && offsetof(GpuTest, step) == 16 &&
              offsetof(GpuTest, bound) == 32 && offsetof(GpuTest, noct) == 48, "GpuTest layout");
static_assert(sizeof(GpuParams) == 16, "GpuParams layout");

class GpuBackend
{
public:
    virtual ~GpuBackend() {}
    // maxoct: the most octaves per octave noise among the tests
    virtual bool init(const std::vector<GpuTest>& tests, const std::vector<GpuOct>& octs,
                      uint32_t maxoct) = 0;
    // Appends the seeds of [start, start + count) that pass, in any order.
    virtual bool run(uint64_t start, uint32_t count, std::vector<uint64_t>& passed) = 0;
};

// Implemented by the backend of the platform (or the stub).
bool gpuBackendAvailable();
GpuBackend *gpuBackendCreate();

// The generated kernel variants (gpufilter_spirv.inc, gpufilter_msl.inc):
// noct is the most octaves per octave noise that a variant supports.
struct GpuSpirvKernel { uint32_t noct; const uint32_t *code; size_t size; };
struct GpuMslKernel { uint32_t noct; const char *source; };

// The variant for a test set: the one with the fewest octaves that suffice.
template <class Kernel, size_t N>
const Kernel *gpuSelectKernel(const Kernel (&kernels)[N], uint32_t maxoct)
{
    const Kernel *best = nullptr;
    for (const Kernel& k : kernels)
        if (k.noct >= maxoct && (!best || k.noct < best->noct))
            best = &k;
    return best;
}

// Threads per workgroup of a variant, as TG in gpufilter.comp: the
// permutations take 2 * noct * 260 bytes per thread of 32 KiB shared memory.
const uint32_t GPU_SHARED_MEMORY = 32768;
inline uint32_t gpuKernelThreads(uint32_t noct)
{
    return GPU_SHARED_MEMORY / (2 * noct * 260);
}

#endif // GPUFILTER_BACKEND_H
