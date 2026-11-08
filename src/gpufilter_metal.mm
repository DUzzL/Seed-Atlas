// Metal backend of the GPU prefilter (macOS).

#include "gpufilter_backend.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstring>
#include <map>
#include <mutex>

#include "gpufilter_msl.inc"

namespace {

// The device and the compiled pipelines are shared by all filters.
struct MetalShared
{
    std::mutex mutex;
    bool tried = false;
    id<MTLDevice> device = nil;
    std::map<uint32_t, id<MTLComputePipelineState>> pipelines; // by kernel variant
};

MetalShared& shared()
{
    static MetalShared s;
    return s;
}

id<MTLDevice> getDevice()
{
    MetalShared& s = shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.tried)
    {
        s.tried = true;
        s.device = MTLCreateSystemDefaultDevice();
    }
    return s.device;
}

id<MTLComputePipelineState> getPipeline(id<MTLDevice> device, const GpuMslKernel *kernel)
{
    MetalShared& s = shared();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.pipelines.find(kernel->noct);
    if (it != s.pipelines.end())
        return it->second;
    NSError *err = nil;
    MTLCompileOptions *opt = [MTLCompileOptions new];
    opt.mathMode = MTLMathModeSafe;
    NSString *src = [NSString stringWithUTF8String:kernel->source];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:opt error:&err];
    id<MTLComputePipelineState> pso = nil;
    if (lib)
        pso = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"climateFilter"]
                                                    error:&err];
    s.pipelines[kernel->noct] = pso;
    return pso;
}

class MetalBackend : public GpuBackend
{
public:
    bool init(const GpuTestBuf& tests, const std::vector<GpuOct>& octs,
              uint32_t maxoct) override
    {
        @autoreleasepool
        {
            const GpuMslKernel *kernel = gpuSelectKernel(g_msl_kernels, maxoct);
            device = getDevice();
            if (!kernel || !device || !(pso = getPipeline(device, kernel)))
                return false;
            // the kernel sizes its threadgroup memory for this many threads
            tg = gpuKernelThreads(kernel->noct);
            if (tg > pso.maxTotalThreadsPerThreadgroup)
                return false;
            queue = [device newCommandQueue];
            // the kernel declares a buffer of GPU_MAX_OCTS octaves
            testbuf = [device newBufferWithLength:sizeof(GpuTestBuf) options:MTLResourceStorageModeShared];
            octbuf = [device newBufferWithLength:GPU_MAX_OCTS * sizeof(GpuOct) options:MTLResourceStorageModeShared];
            if (!testbuf || !octbuf || octs.size() > GPU_MAX_OCTS)
                return false;
            memcpy(testbuf.contents, &tests, sizeof(GpuTestBuf));
            memcpy(octbuf.contents, octs.data(), octs.size() * sizeof(GpuOct));
            params = [device newBufferWithLength:sizeof(GpuParams) options:MTLResourceStorageModeShared];
            npass = [device newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
            return queue && testbuf && octbuf && params && npass;
        }
    }

    bool run(uint64_t start, uint32_t count, std::vector<uint64_t>& passed) override
    {
        @autoreleasepool
        {
            if (count > capacity)
            {
                out = [device newBufferWithLength:(NSUInteger) count * sizeof(uint64_t)
                                          options:MTLResourceStorageModeShared];
                if (!out)
                    return false;
                capacity = count;
            }
            GpuParams *p = (GpuParams*) params.contents;
            p->seed0 = start;
            p->count = count;
            *(uint32_t*) npass.contents = 0;

            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:params offset:0 atIndex:0];
            [enc setBuffer:testbuf offset:0 atIndex:1];
            [enc setBuffer:octbuf offset:0 atIndex:2];
            [enc setBuffer:npass offset:0 atIndex:3];
            [enc setBuffer:out offset:0 atIndex:4];
            [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted)
                return false;

            uint32_t n = *(const uint32_t*) npass.contents;
            if (n > count)
                return false;
            const uint64_t *s = (const uint64_t*) out.contents;
            passed.insert(passed.end(), s, s + n);
            return true;
        }
    }

private:
    id<MTLDevice> device = nil;
    id<MTLComputePipelineState> pso = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLBuffer> testbuf = nil, octbuf = nil, params = nil, npass = nil, out = nil;
    NSUInteger tg = 1;
    uint32_t capacity = 0;
};

} // namespace

bool gpuBackendAvailable()
{
    return getDevice() != nil;
}

GpuBackend *gpuBackendCreate()
{
    return new MetalBackend;
}
