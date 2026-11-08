#include "gpufilter.h"

#include "seedatlas-engine/biomenoise.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

static const char *g_kernel_source =
#include "gpufilter.metal.inc"
;

// Bound on the directional derivative of a Perlin octave per unit of its
// input. The largest value observed in 4e7 random samples is 3.16; the
// engine's own range search assumes 3.75.
static const double PERLIN_GRADIENT_BOUND = 4.0;
// Margin for the single precision noise (in units of 1/10000). The observed
// error is below 1 near the origin. Further out, the rounding of the scaled
// coordinates (relative error ~6e-8) adds a positional error, which the
// gradient bound converts into a value error; it gets a safety factor of 10.
static const double FLOAT_MARGIN = 50.0;
static const double FLOAT_POS_ERROR = 10 * 1.2e-7;
// Samples of the coarsest grid level of a test, the most samples of the
// finest level and the largest gradient part of its bound. Seeds that are
// still undecided at the finest level are left to the CPU.
static const double COARSE_GRID_SAMPLES = 64.0;
static const double MAX_GRID_SAMPLES = 16384.0;
static const double MAX_GRID_BOUND = 400.0;

namespace {

struct GpuOct { uint64_t mlo, mhi; float amp, lac; };

struct GpuTest {
    uint64_t clo, chi;
    uint32_t noct, oct0;
    float dpamp, margin, lmin, lmax;
    int32_t x, z, w, h;
    uint32_t nlevels;
    int32_t step[4];
    float bound[4];
    int32_t pad;
};

struct GpuParams {
    uint64_t seed0;
    uint32_t count, ntests;
};

// md5 of "octave_-12" ... "octave_0" (as in xOctaveInit())
const uint64_t md5_octave_n[][2] = {
    {0xb198de63a8012672, 0x7b84cad43ef7b5a8}, {0x0fd787bfbc403ec3, 0x74a4a31ca21b48b8},
    {0x36d326eed40efeb2, 0x5be9ce18223c636a}, {0x082fe255f8be6631, 0x4e96119e22dedc81},
    {0x0ef68ec68504005e, 0x48b6bf93a2789640}, {0xf11268128982754f, 0x257a1d670430b0aa},
    {0xe51c98ce7d1de664, 0x5f9478a733040c45}, {0x6d7b49e7e429850a, 0x2e3063c622a24777},
    {0xbd90d5377ba1b762, 0xc07317d419a7548d}, {0x53d39c6752dac858, 0xbcd1c5a80ab65b3e},
    {0xb4a24d7a84e7677b, 0x023ff9668e89b5c4}, {0xdffa22b534c5f608, 0xb9b67517d3665ca9},
    {0xd50708086cef4d7c, 0x6e1651ecc7f43309},
};

struct Shared
{
    std::mutex mutex;
    id<MTLDevice> device = nil;
    bool tried = false;
    // compiled pipelines by (octaves, threadgroup size)
    std::map<std::pair<uint32_t, NSUInteger>, id<MTLComputePipelineState>> pipelines;
};

Shared& shared()
{
    static Shared s;
    return s;
}

id<MTLDevice> getDevice()
{
    Shared& s = shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.tried)
    {
        s.tried = true;
        s.device = MTLCreateSystemDefaultDevice();
    }
    return s.device;
}

id<MTLComputePipelineState> getPipeline(id<MTLDevice> device, uint32_t maxoct, NSUInteger tg)
{
    Shared& s = shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto key = std::make_pair(maxoct, tg);
    auto it = s.pipelines.find(key);
    if (it != s.pipelines.end())
        return it->second;
    NSError *err = nil;
    MTLCompileOptions *opt = [MTLCompileOptions new];
    opt.mathMode = MTLMathModeSafe;
    opt.preprocessorMacros = @{ @"NOCT": @(maxoct), @"TG": @(tg) };
    NSString *src = [NSString stringWithUTF8String:g_kernel_source];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:opt error:&err];
    id<MTLComputePipelineState> pso = nil;
    if (lib)
    {
        id<MTLFunction> fn = [lib newFunctionWithName:@"climateFilter"];
        pso = [device newComputePipelineStateWithFunction:fn error:&err];
    }
    s.pipelines[key] = pso;
    return pso;
}

} // namespace

struct GpuClimateFilter::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pso = nil;
    id<MTLBuffer> tests = nil, octs = nil, params = nil, npass = nil, passed = nil;
    uint32_t ntests = 0;
    uint32_t capacity = 0;
    NSUInteger tg = 1;
};

bool GpuClimateFilter::enabled()
{
    const char *env = getenv("SEED_ATLAS_GPU");
    if (!env || strcmp(env, "1") != 0)
        return false;
    return getDevice() != nil;
}

GpuClimateFilter::GpuClimateFilter() : d(new Impl) {}

GpuClimateFilter::~GpuClimateFilter()
{
    delete d;
}

bool GpuClimateFilter::init(const std::vector<Test>& tests, bool large)
{
    @autoreleasepool
    {
        if (tests.empty() || !(d->device = getDevice()))
            return false;

        std::vector<GpuTest> gt;
        std::vector<GpuOct> go;
        uint32_t maxoct = 1;
        for (const Test& t : tests)
        {
            const ClimateNoiseSpec *spec = getClimateNoiseSpec(t.para);
            if (!spec)
                return false;
            // seed independent octave amplitudes and frequencies
            BiomeNoise bn;
            setClimateParaSeed(&bn, 0, large, t.para, -1);
            const DoublePerlinNoise *dp = &bn.climate[t.para];
            if (dp->octA.octcnt != dp->octB.octcnt)
                return false;

            GpuTest g = {};
            int li = large ? 1 : 0;
            g.clo = spec->md5[li][0];
            g.chi = spec->md5[li][1];
            g.noct = dp->octA.octcnt;
            g.oct0 = go.size();
            g.dpamp = dp->amplitude;
            double gradsum = 0;
            int n = 0;
            for (int i = 0; i < spec->len; i++)
            {
                if (spec->amp[i] == 0)
                    continue;
                const PerlinNoise *p = &dp->octA.octaves[n++];
                GpuOct o;
                o.mlo = md5_octave_n[12 + spec->omin[li] + i][0];
                o.mhi = md5_octave_n[12 + spec->omin[li] + i][1];
                o.amp = (float) p->amplitude;
                o.lac = (float) p->lacunarity;
                go.push_back(o);
                gradsum += p->amplitude * p->lacunarity * (1.0 + 337.0 / 331.0);
            }
            if (n != (int) g.noct)
                return false;
            maxoct = std::max(maxoct, g.noct);

            // Grid levels from coarse to fine. The CPU samples integer
            // positions only, so at a step of one the bound is just the
            // rounding margin; larger steps add the gradient bound times the
            // distance to the next sample.
            double grad = 10000.0 * dp->amplitude * PERLIN_GRADIENT_BOUND * gradsum;
            // the single precision error grows with the coordinates
            double far = std::max(std::max(std::abs((double) t.x), std::abs((double) t.x + t.w)),
                                  std::max(std::abs((double) t.z), std::abs((double) t.z + t.h)));
            if (far > (1 << 22))
                return false; // too far out for single precision
            double margin = FLOAT_MARGIN + grad * far * FLOAT_POS_ERROR;
            double area = (double) t.w * t.h;
            // the finest level: the widest grid with a small gradient bound,
            // but at least a step of 4 (finer grids cost more than they save)
            int fine = 4;
            while (fine < 64 && grad * std::sqrt(2.0) * fine <= MAX_GRID_BOUND)
                fine *= 2;
            while (area / ((double) fine * fine) > MAX_GRID_SAMPLES)
                fine *= 2;
            int coarse = fine;
            while (area / ((double) coarse * coarse) > COARSE_GRID_SAMPLES)
                coarse *= 2;
            int nl = 0;
            for (int st = coarse; nl < 4; st = std::max(fine, st / 4))
            {
                g.step[nl] = st;
                g.bound[nl] = (float) (grad * std::sqrt(2.0) * (st / 2) + margin);
                nl++;
                if (st == fine)
                    break;
            }
            if (g.step[nl-1] != fine)
            {   // keep the finest level
                g.step[nl-1] = fine;
                g.bound[nl-1] = (float) (grad * std::sqrt(2.0) * (fine / 2) + margin);
            }
            g.nlevels = nl;
            g.margin = (float) margin;
            if (getenv("SEED_ATLAS_GPU_DEBUG"))
                fprintf(stderr, "gpu test: para=%d area=(%d,%d %dx%d) range=[%g,%g] noct=%u levels=%u fine=%d grad=%g margin=%g\n",
                    t.para, t.x, t.z, t.w, t.h, t.lmin, t.lmax, g.noct, g.nlevels, g.step[g.nlevels-1], grad, margin);
            g.lmin = (float) t.lmin;
            g.lmax = (float) t.lmax;
            g.x = t.x;
            g.z = t.z;
            g.w = t.w;
            g.h = t.h;
            gt.push_back(g);
        }

        NSUInteger tg = std::max<NSUInteger>(1, 32768 / (2 * maxoct * 260));
        if (tg > 32)
            tg = 32;
        d->pso = getPipeline(d->device, maxoct, tg);
        if (!d->pso)
            return false;
        d->tg = std::min<NSUInteger>(tg, d->pso.maxTotalThreadsPerThreadgroup);
        d->queue = [d->device newCommandQueue];
        d->tests = [d->device newBufferWithBytes:gt.data() length:gt.size() * sizeof(GpuTest) options:0];
        d->octs = [d->device newBufferWithBytes:go.data() length:go.size() * sizeof(GpuOct) options:0];
        d->params = [d->device newBufferWithLength:sizeof(GpuParams) options:MTLResourceStorageModeShared];
        d->npass = [d->device newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
        d->ntests = gt.size();
        return d->queue && d->tests && d->octs && d->params && d->npass;
    }
}

bool GpuClimateFilter::filter(uint64_t start, uint32_t count, std::vector<uint64_t>& passed)
{
    if (!d->pso || count == 0)
        return d->pso != nil;
    @autoreleasepool
    {
        if (count > d->capacity)
        {
            d->passed = [d->device newBufferWithLength:(NSUInteger) count * sizeof(uint64_t)
                                               options:MTLResourceStorageModeShared];
            if (!d->passed)
                return false;
            d->capacity = count;
        }
        GpuParams *p = (GpuParams*) d->params.contents;
        p->seed0 = start;
        p->count = count;
        p->ntests = d->ntests;
        *(uint32_t*) d->npass.contents = 0;

        id<MTLCommandBuffer> cb = [d->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:d->pso];
        [enc setBuffer:d->params offset:0 atIndex:0];
        [enc setBuffer:d->tests offset:0 atIndex:1];
        [enc setBuffer:d->octs offset:0 atIndex:2];
        [enc setBuffer:d->npass offset:0 atIndex:3];
        [enc setBuffer:d->passed offset:0 atIndex:4];
        [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(d->tg, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted)
            return false;

        uint32_t n = *(const uint32_t*) d->npass.contents;
        if (getenv("SEED_ATLAS_GPU_DEBUG"))
            fprintf(stderr, "gpu filter: start=%llu count=%u passed=%u gpu=%.3f ms\n",
                (unsigned long long) start, count, n, 1e3 * (cb.GPUEndTime - cb.GPUStartTime));
        if (n > count)
            return false;
        const uint64_t *s = (const uint64_t*) d->passed.contents;
        size_t old = passed.size();
        passed.insert(passed.end(), s, s + n);
        std::sort(passed.begin() + old, passed.end());
        return true;
    }
}
