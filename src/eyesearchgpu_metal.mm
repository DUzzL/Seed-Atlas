// Metal backend of the GPU eye search (macOS).

#include "eyesearchgpu_backend.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "eyesearchgpu_msl.inc"

namespace {

// The device and the compiled pipelines are shared by all workers.
struct MetalEyeShared
{
    std::mutex mutex;
    bool tried = false;
    id<MTLDevice> device = nil;
    id<MTLComputePipelineState> pso[2] = { nil, nil };  // pass 1, 2
};

id<MTLComputePipelineState> compile(id<MTLDevice> device, const char *source, NSString *name)
{
    NSError *err = nil;
    // integer arithmetic only, the math mode does not matter
    MTLCompileOptions *opt = [MTLCompileOptions new];
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:source]
                                              options:opt error:&err];
    id<MTLComputePipelineState> pso = nil;
    if (lib)
        pso = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&err];
    if (pso && pso.maxTotalThreadsPerThreadgroup < GPU_EYE_THREADS)
        pso = nil;
    return pso;
}

MetalEyeShared& shared()
{
    static MetalEyeShared s;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.tried)
    {
        s.tried = true;
        @autoreleasepool
        {
            s.device = MTLCreateSystemDefaultDevice();
            if (s.device)
            {
                s.pso[0] = compile(s.device, g_eye_msl_1, @"eyeSweep1");
                s.pso[1] = compile(s.device, g_eye_msl_2, @"eyeSweep2");
                if (!s.pso[0] || !s.pso[1])
                    s.pso[0] = s.pso[1] = nil;
            }
        }
    }
    return s;
}

class MetalEyeBackend : public GpuEyeBackend
{
public:
    bool init() override
    {
        @autoreleasepool
        {
            MetalEyeShared& s = shared();
            if (!s.pso[0])
                return false;
            device = s.device;
            pso1 = s.pso[0];
            pso2 = s.pso[1];
            queue = [device newCommandQueue];
            params = newBuffer(sizeof(GpuEyeParams));
            decobuf = newBuffer(2 * 65536 * sizeof(uint64_t));
            return queue && params && decobuf;
        }
    }

    uint64_t *deco() override
    {
        return (uint64_t*) decobuf.contents;
    }

    GpuEyePlan *plans(uint32_t n) override
    {
        if (!fit(planbuf, planCap, n, sizeof(GpuEyePlan), false))
            return nullptr;
        return (GpuEyePlan*) planbuf.contents;
    }

    uint32_t *tables(uint32_t words) override
    {
        if (!fit(tablebuf, tableCap, words, sizeof(uint32_t), true))
            return nullptr;
        return (uint32_t*) tablebuf.contents;
    }

    GpuEyeRunResult run(const GpuEyeParams& p, uint32_t listGroups, std::vector<uint32_t>& kept,
        uint32_t need[2]) override
    {
        @autoreleasepool
        {
            if (!planbuf || !tablebuf || p.nplans > planCap || p.nplans == 0 || listGroups == 0 ||
                p.firstRange > p.nplans || p.nunits > p.firstRange)
                return GPU_EYE_RUN_FAILED;
            if (!fit(outbuf, outCap, p.maxOut + 1, sizeof(uint32_t), false) ||
                !fit(listbuf, listCap, p.maxList + 1, sizeof(uint32_t), false))
                return GPU_EYE_RUN_FAILED;
            memcpy(params.contents, &p, sizeof(p));
            *(uint32_t*) outbuf.contents = 0;
            *(uint32_t*) listbuf.contents = 0;

            // the two passes run one after the other (serial dispatch)
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setBuffer:params offset:0 atIndex:0];
            [enc setBuffer:decobuf offset:0 atIndex:1];
            [enc setBuffer:planbuf offset:0 atIndex:2];
            [enc setBuffer:tablebuf offset:0 atIndex:3];
            [enc setBuffer:outbuf offset:0 atIndex:4];
            [enc setBuffer:listbuf offset:0 atIndex:5];
            [enc setComputePipelineState:pso1];
            [enc dispatchThreadgroups:MTLSizeMake((NSUInteger) (p.nunits + p.nplans - p.firstRange) *
                                                  p.groupsPerPlan, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(GPU_EYE_THREADS, 1, 1)];
            [enc setComputePipelineState:pso2];
            [enc dispatchThreadgroups:MTLSizeMake(listGroups, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(GPU_EYE_THREADS, 1, 1)];
            [enc endEncoding];
            [cb commit];
            // (Metal ends command buffers that run too long with an error
            // itself, so this wait is bounded.)
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted)
                return GPU_EYE_RUN_FAILED;

            // the counters count on past the capacity: the sizes needed
            const uint32_t *o = (const uint32_t*) outbuf.contents;
            need[0] = o[0];
            need[1] = *(const uint32_t*) listbuf.contents;
            if (need[0] > p.maxOut || need[1] > p.maxList)
                return GPU_EYE_RUN_OVERFLOW;
            kept.insert(kept.end(), o + 1, o + 1 + o[0]);
            return GPU_EYE_RUN_OK;
        }
    }

private:
    id<MTLBuffer> newBuffer(size_t size)
    {
        return [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    }

    // A buffer for n elements of the given size: a larger one if needed, a
    // smaller one if it is more than 4x too large (so a run with huge lists
    // does not keep its memory). With 'keep', the content is copied over.
    bool fit(__strong id<MTLBuffer>& buf, uint32_t& cap, uint32_t n, size_t elem, bool keep)
    {
        if (buf && n <= cap && (cap <= 65536 || n >= cap / 4))
            return true;
        uint32_t want = n + n / 4 > 1024 ? n + n / 4 : 1024;
        id<MTLBuffer> nb = newBuffer((size_t) want * elem);
        if (!nb)
            return false;
        if (keep && buf)
            memcpy(nb.contents, buf.contents, (size_t) std::min(cap, want) * elem);
        buf = nb;
        cap = want;
        return true;
    }

    id<MTLDevice> device = nil;
    id<MTLComputePipelineState> pso1 = nil, pso2 = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLBuffer> params = nil, decobuf = nil, planbuf = nil, tablebuf = nil, outbuf = nil,
        listbuf = nil;
    uint32_t planCap = 0, tableCap = 0, outCap = 0, listCap = 0;
};

} // namespace

bool gpuEyeBackendAvailable()
{
    return shared().pso[0] != nil;
}

GpuEyeBackend *gpuEyeBackendCreate()
{
    return new MetalEyeBackend;
}
