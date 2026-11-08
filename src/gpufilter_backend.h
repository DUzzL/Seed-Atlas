#ifndef GPUFILTER_BACKEND_H
#define GPUFILTER_BACKEND_H

// Interface between GpuClimateFilter and the GPU APIs (Metal, Vulkan). The
// structs are shared with the kernel (gpufilter.comp).

#include <cstddef>
#include <cstdint>
#include <vector>

// The kernel reads at most this many tests of each kind and octaves.
const uint32_t GPU_MAX_TESTS = 8;
const uint32_t GPU_MAX_SAMPLE_TESTS = 2;
const uint32_t GPU_MAX_OCTS = 128;
// A sample test draws at most this many positions and has at most this many
// noises (the shift noise and the five climate parameters).
const uint32_t GPU_MAX_POSITIONS = 16;
const uint32_t GPU_MAX_NOISES = 6;

struct GpuOct {
    uint64_t mlo, mhi;      // md5 salt of the octave
    float amp, lac;         // amplitude and lacunarity
    float rest;             // bound on what the later octave pairs add (x 10000)
    float pad;
};

// A climate test: the noise on a grid over an area.
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

// A noise of a sample test.
struct GpuNoise {
    uint64_t clo, chi;      // md5 salt of the climate parameter
    uint32_t noct, oct0;    // octaves per octave noise, first in the octave buffer
    float dpamp, margin;    // double Perlin amplitude, rounding margin (x 10000)
    float lo, hi;           // bounds of the parameter (x 10000)
    uint32_t pad0, pad1;
};

// A sample test: the positions of monteCarloBiomes(), which succeed at most
// where the climate parameters are within their bounds.
struct GpuSampleTest {
    int32_t x, z, w, h;     // sampled area in quart coordinates
    uint64_t rmaxsq;        // squared radius in blocks (0: none)
    uint32_t npos, nnoise;  // positions drawn, noises (the shift noise first)
    // after m samples (index m-1): the most successes that fail for sure
    // (or -1), and the fewest with which the test may pass
    int32_t fail[GPU_MAX_POSITIONS];
    int32_t pass[GPU_MAX_POSITIONS];
    GpuNoise noise[GPU_MAX_NOISES];
};

// The uniform buffer with all tests.
struct GpuTestBuf {
    uint32_t ntests, nsamples, pad0, pad1;
    GpuTest tests[GPU_MAX_TESTS];
    GpuSampleTest samples[GPU_MAX_SAMPLE_TESTS];
};

struct GpuParams {
    uint64_t seed0;         // first seed
    uint32_t count, pad;    // number of seeds
};

// std140 layouts of the kernel's uniform buffers
static_assert(sizeof(GpuOct) == 32, "GpuOct layout");
static_assert(sizeof(GpuTest) == 96 && offsetof(GpuTest, step) == 16 &&
              offsetof(GpuTest, bound) == 32 && offsetof(GpuTest, noct) == 48, "GpuTest layout");
static_assert(sizeof(GpuNoise) == 48, "GpuNoise layout");
static_assert(offsetof(GpuSampleTest, fail) == 32 && offsetof(GpuSampleTest, pass) == 96 &&
              offsetof(GpuSampleTest, noise) == 160 && sizeof(GpuSampleTest) == 448,
              "GpuSampleTest layout");
static_assert(offsetof(GpuTestBuf, tests) == 16 && offsetof(GpuTestBuf, samples) == 16 + 8 * 96,
              "GpuTestBuf layout");
static_assert(sizeof(GpuParams) == 16, "GpuParams layout");

class GpuBackend
{
public:
    virtual ~GpuBackend() {}
    // maxoct: the most octaves per octave noise among the climate tests
    virtual bool init(const GpuTestBuf& tests, const std::vector<GpuOct>& octs,
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

// The variant for a test set: the one with the fewest octaves that suffice
// (maxoct = 0: sample tests only).
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
// permutations take 260 bytes each of 32 KiB shared memory, 2 * noct per
// thread (or one for noct = 0).
const uint32_t GPU_SHARED_MEMORY = 32768;
inline uint32_t gpuKernelThreads(uint32_t noct)
{
    return GPU_SHARED_MEMORY / ((noct ? 2 * noct : 1) * 260);
}

#endif // GPUFILTER_BACKEND_H
