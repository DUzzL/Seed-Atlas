// Vulkan backend of the GPU prefilter (Windows, Linux). The Vulkan library
// is loaded at run time: without it (or without a suitable GPU) the filter
// is simply not available and the search runs on the CPU.

#include "gpufilter_backend.h"
#include "vkmini.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "gpufilter_spirv.inc"

namespace {

#define VK_CHECK(call) do { if ((call) != vk::SUCCESS) return false; } while (0)

static void *loadLibrary()
{
#if defined(_WIN32)
    return (void*) LoadLibraryA("vulkan-1.dll");
#elif defined(__APPLE__)
    return dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    return dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}

static vk::VoidFunction loadSymbol(void *lib, const char *name)
{
#if defined(_WIN32)
    return (vk::VoidFunction) GetProcAddress((HMODULE) lib, name);
#else
    return (vk::VoidFunction) dlsym(lib, name);
#endif
}

// A host visible buffer, mapped for its whole lifetime.
struct HostBuffer
{
    vk::Buffer buffer = 0;
    vk::DeviceMemory memory = 0;
    void *data = nullptr;
    vk::DeviceSize size = 0;
};

// The instance, the device and the pipelines are shared by all filters.
struct VulkanShared
{
    std::mutex mutex;
    bool tried = false, ok = false;
    vk::Functions f;
    vk::Instance instance = nullptr;
    vk::PhysicalDevice gpu = nullptr;
    vk::Device device = nullptr;
    vk::Queue queue = nullptr;
    std::mutex queueMutex; // vkQueueSubmit needs external synchronization
    uint32_t family = 0;
    vk::PhysicalDeviceMemoryProperties memprops = {};
    vk::DescriptorSetLayout setLayout = 0;
    vk::PipelineLayout pipelineLayout = 0;
    std::map<uint32_t, vk::Pipeline> pipelines; // by kernel variant

    bool init();
    bool loadFunctions();
    bool pickDevice();
    bool createDevice();
    bool createLayouts();
    vk::Pipeline getPipeline(const GpuSpirvKernel *kernel);
    bool createBuffer(HostBuffer& b, vk::DeviceSize size, vk::Flags usage);
    void destroyBuffer(HostBuffer& b);
};

VulkanShared& shared()
{
    static VulkanShared s;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.tried)
    {
        s.tried = true;
        s.ok = s.init();
        if (getenv("SEED_ATLAS_GPU_DEBUG"))
            fprintf(stderr, "gpu: vulkan %s\n", s.ok ? "ready" : "not available");
    }
    return s;
}

bool VulkanShared::loadFunctions()
{
    void *lib = loadLibrary();
    if (!lib)
        return false;
    f.GetInstanceProcAddr = (decltype(f.GetInstanceProcAddr)) loadSymbol(lib, "vkGetInstanceProcAddr");
    if (!f.GetInstanceProcAddr)
        return false;
    f.CreateInstance = (decltype(f.CreateInstance)) f.GetInstanceProcAddr(nullptr, "vkCreateInstance");
    if (!f.CreateInstance)
        return false;

    vk::ApplicationInfo app = {};
    app.sType = vk::STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Seed Atlas";
    app.apiVersion = vk::API_VERSION_1_1;
    vk::InstanceCreateInfo ci = {};
    ci.sType = vk::STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VK_CHECK(f.CreateInstance(&ci, nullptr, &instance));

    // all other functions through the instance
#define VKMINI_LOAD(ret, name, ...) \
    f.name = (decltype(f.name)) f.GetInstanceProcAddr(instance, "vk" #name); \
    if (!f.name) return false;
    VKMINI_FUNCTIONS(VKMINI_LOAD)
#undef VKMINI_LOAD
    return true;
}

// A GPU with 64-bit integers, enough shared memory and a compute queue;
// discrete before integrated.
// CPU implementations (such as lavapipe) only with SEED_ATLAS_GPU_ALLOW_CPU=1.
bool VulkanShared::pickDevice()
{
    uint32_t n = 0;
    VK_CHECK(f.EnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<vk::PhysicalDevice> devices(n);
    VK_CHECK(f.EnumeratePhysicalDevices(instance, &n, devices.data()));
    bool allowCpu = getenv("SEED_ATLAS_GPU_ALLOW_CPU") != nullptr;

    int best = -1;
    for (vk::PhysicalDevice d : devices)
    {
        vk::PhysicalDeviceProperties props;
        f.GetPhysicalDeviceProperties(d, &props);
        vk::PhysicalDeviceFeatures feat;
        f.GetPhysicalDeviceFeatures(d, &feat);
        uint32_t shmem;
        memcpy(&shmem, (const char*) &props + vk::LIMITS_MAX_COMPUTE_SHARED_MEMORY_SIZE_OFFSET,
            sizeof(shmem));
        if (!feat.features[vk::FEATURE_SHADER_INT64] || shmem < GPU_SHARED_MEMORY)
            continue;
        int score;
        switch (props.deviceType)
        {
        case vk::PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 3; break;
        case vk::PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 2; break;
        case vk::PHYSICAL_DEVICE_TYPE_CPU:            score = allowCpu ? 0 : -1; break;
        default:                                      score = 1; break;
        }
        uint32_t nf = 0;
        f.GetPhysicalDeviceQueueFamilyProperties(d, &nf, nullptr);
        std::vector<vk::QueueFamilyProperties> fams(nf);
        f.GetPhysicalDeviceQueueFamilyProperties(d, &nf, fams.data());
        uint32_t fam = nf;
        for (uint32_t i = 0; i < nf && fam == nf; i++)
            if (fams[i].queueFlags & vk::QUEUE_COMPUTE_BIT)
                fam = i;
        if (score > best && fam < nf)
        {
            best = score;
            gpu = d;
            family = fam;
            if (getenv("SEED_ATLAS_GPU_DEBUG"))
                fprintf(stderr, "gpu: vulkan device %s\n", props.deviceName);
        }
    }
    return gpu != nullptr;
}

bool VulkanShared::createDevice()
{
    float priority = 1.0f;
    vk::DeviceQueueCreateInfo qi = {};
    qi.sType = vk::STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    vk::PhysicalDeviceFeatures feat = {};
    feat.features[vk::FEATURE_SHADER_INT64] = 1;
    vk::DeviceCreateInfo di = {};
    di.sType = vk::STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &feat;
    VK_CHECK(f.CreateDevice(gpu, &di, nullptr, &device));
    f.GetDeviceQueue(device, family, 0, &queue);
    f.GetPhysicalDeviceMemoryProperties(gpu, &memprops);
    return queue != nullptr;
}

// The buffers of gpufilter.comp: three uniform and two storage buffers.
static const int32_t g_binding_types[5] = {
    vk::DESCRIPTOR_TYPE_UNIFORM_BUFFER, vk::DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    vk::DESCRIPTOR_TYPE_UNIFORM_BUFFER, vk::DESCRIPTOR_TYPE_STORAGE_BUFFER,
    vk::DESCRIPTOR_TYPE_STORAGE_BUFFER,
};

static vk::Flags bufferUsage(int binding)
{
    return g_binding_types[binding] == vk::DESCRIPTOR_TYPE_UNIFORM_BUFFER ?
        vk::BUFFER_USAGE_UNIFORM_BUFFER_BIT : vk::BUFFER_USAGE_STORAGE_BUFFER_BIT;
}

// The descriptor set layout and the pipeline layout.
bool VulkanShared::createLayouts()
{
    vk::DescriptorSetLayoutBinding bindings[5] = {};
    for (uint32_t i = 0; i < 5; i++)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = g_binding_types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = vk::SHADER_STAGE_COMPUTE_BIT;
    }
    vk::DescriptorSetLayoutCreateInfo li = {};
    li.sType = vk::STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 5;
    li.pBindings = bindings;
    VK_CHECK(f.CreateDescriptorSetLayout(device, &li, nullptr, &setLayout));

    vk::PipelineLayoutCreateInfo pi = {};
    pi.sType = vk::STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &setLayout;
    VK_CHECK(f.CreatePipelineLayout(device, &pi, nullptr, &pipelineLayout));
    return true;
}

bool VulkanShared::init()
{
    return loadFunctions() && pickDevice() && createDevice() && createLayouts();
}

// The pipeline of a kernel variant.
vk::Pipeline VulkanShared::getPipeline(const GpuSpirvKernel *kernel)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = pipelines.find(kernel->noct);
    if (it != pipelines.end())
        return it->second;

    vk::Pipeline p = 0;
    vk::ShaderModule shader = 0;
    vk::ShaderModuleCreateInfo si = {};
    si.sType = vk::STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = kernel->size;
    si.pCode = kernel->code;
    if (f.CreateShaderModule(device, &si, nullptr, &shader) == vk::SUCCESS)
    {
        vk::ComputePipelineCreateInfo ci = {};
        ci.sType = vk::STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = vk::STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = vk::SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = shader;
        ci.stage.pName = "main";
        ci.layout = pipelineLayout;
        ci.basePipelineIndex = -1;
        if (f.CreateComputePipelines(device, 0, 1, &ci, nullptr, &p) != vk::SUCCESS)
            p = 0;
    }
    pipelines[kernel->noct] = p; // the shader module stays alive with it
    return p;
}

bool VulkanShared::createBuffer(HostBuffer& b, vk::DeviceSize size, vk::Flags usage)
{
    vk::BufferCreateInfo bi = {};
    bi.sType = vk::STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = vk::SHARING_MODE_EXCLUSIVE;
    VK_CHECK(f.CreateBuffer(device, &bi, nullptr, &b.buffer));
    vk::MemoryRequirements req;
    f.GetBufferMemoryRequirements(device, b.buffer, &req);

    const uint32_t want = vk::MEMORY_PROPERTY_HOST_VISIBLE_BIT | vk::MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = memprops.memoryTypeCount;
    for (uint32_t i = 0; i < memprops.memoryTypeCount && type == memprops.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    if (type == memprops.memoryTypeCount)
        return false;
    vk::MemoryAllocateInfo ai = {};
    ai.sType = vk::STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VK_CHECK(f.AllocateMemory(device, &ai, nullptr, &b.memory));
    VK_CHECK(f.BindBufferMemory(device, b.buffer, b.memory, 0));
    VK_CHECK(f.MapMemory(device, b.memory, 0, size, 0, &b.data));
    b.size = size;
    return true;
}

void VulkanShared::destroyBuffer(HostBuffer& b)
{
    if (b.buffer)
        f.DestroyBuffer(device, b.buffer, nullptr);
    if (b.memory)
        f.FreeMemory(device, b.memory, nullptr); // also unmaps
    b = HostBuffer();
}

class VulkanBackend : public GpuBackend
{
public:
    ~VulkanBackend() override
    {
        VulkanShared& s = shared();
        if (!s.ok)
            return;
        for (HostBuffer *b : { &params, &testbuf, &octbuf, &npass, &out })
            s.destroyBuffer(*b);
        if (pool)
            s.f.DestroyDescriptorPool(s.device, pool, nullptr);
        if (cmdpool)
            s.f.DestroyCommandPool(s.device, cmdpool, nullptr);
        if (fence)
            s.f.DestroyFence(s.device, fence, nullptr);
    }

    bool init(const std::vector<GpuTest>& tests, const std::vector<GpuOct>& octs,
              uint32_t maxoct) override
    {
        VulkanShared& s = shared();
        const GpuSpirvKernel *kernel = gpuSelectKernel(g_spirv_kernels, maxoct);
        if (!kernel || !s.ok || !(pipeline = s.getPipeline(kernel)))
            return false;
        tg = gpuKernelThreads(kernel->noct); // the workgroup size of the kernel
        ntests = tests.size();
        // the kernel declares buffers of GPU_MAX_TESTS tests and GPU_MAX_OCTS octaves
        if (tests.size() > GPU_MAX_TESTS || octs.size() > GPU_MAX_OCTS)
            return false;
        if (!s.createBuffer(params, sizeof(GpuParams), bufferUsage(0)) ||
            !s.createBuffer(testbuf, GPU_MAX_TESTS * sizeof(GpuTest), bufferUsage(1)) ||
            !s.createBuffer(octbuf, GPU_MAX_OCTS * sizeof(GpuOct), bufferUsage(2)) ||
            !s.createBuffer(npass, sizeof(uint32_t), bufferUsage(3)) ||
            !s.createBuffer(out, 0x10000 * sizeof(uint64_t), bufferUsage(4)))
            return false;
        memcpy(testbuf.data, tests.data(), tests.size() * sizeof(GpuTest));
        memcpy(octbuf.data, octs.data(), octs.size() * sizeof(GpuOct));

        vk::DescriptorPoolSize ps[2] = {
            { vk::DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 },
            { vk::DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
        };
        vk::DescriptorPoolCreateInfo pi = {};
        pi.sType = vk::STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = ps;
        VK_CHECK(s.f.CreateDescriptorPool(s.device, &pi, nullptr, &pool));
        vk::DescriptorSetAllocateInfo ai = {};
        ai.sType = vk::STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &s.setLayout;
        VK_CHECK(s.f.AllocateDescriptorSets(s.device, &ai, &set));
        updateDescriptors();

        vk::CommandPoolCreateInfo ci = {};
        ci.sType = vk::STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = vk::COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = s.family;
        VK_CHECK(s.f.CreateCommandPool(s.device, &ci, nullptr, &cmdpool));
        vk::CommandBufferAllocateInfo bi = {};
        bi.sType = vk::STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        bi.commandPool = cmdpool;
        bi.level = vk::COMMAND_BUFFER_LEVEL_PRIMARY;
        bi.commandBufferCount = 1;
        VK_CHECK(s.f.AllocateCommandBuffers(s.device, &bi, &cmd));
        vk::FenceCreateInfo fi = {};
        fi.sType = vk::STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(s.f.CreateFence(s.device, &fi, nullptr, &fence));
        return true;
    }

    bool run(uint64_t start, uint32_t count, std::vector<uint64_t>& passed) override
    {
        VulkanShared& s = shared();
        if (count * sizeof(uint64_t) > out.size)
        {   // a larger output buffer
            s.destroyBuffer(out);
            if (!s.createBuffer(out, count * sizeof(uint64_t), bufferUsage(4)))
                return false;
            updateDescriptors();
        }
        GpuParams *p = (GpuParams*) params.data;
        p->seed0 = start;
        p->count = count;
        p->ntests = ntests;
        *(uint32_t*) npass.data = 0;

        vk::CommandBufferBeginInfo bi = {};
        bi.sType = vk::STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = vk::COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(s.f.BeginCommandBuffer(cmd, &bi));
        s.f.CmdBindPipeline(cmd, vk::PIPELINE_BIND_POINT_COMPUTE, pipeline);
        s.f.CmdBindDescriptorSets(cmd, vk::PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayout,
            0, 1, &set, 0, nullptr);
        s.f.CmdDispatch(cmd, (count + tg - 1) / tg, 1, 1);
        // make the results visible to the host
        vk::MemoryBarrierInfo mb = {};
        mb.sType = vk::STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = vk::ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = vk::ACCESS_HOST_READ_BIT;
        s.f.CmdPipelineBarrier(cmd, vk::PIPELINE_STAGE_COMPUTE_SHADER_BIT, vk::PIPELINE_STAGE_HOST_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
        VK_CHECK(s.f.EndCommandBuffer(cmd));

        vk::SubmitInfo si = {};
        si.sType = vk::STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        {
            std::lock_guard<std::mutex> lock(s.queueMutex);
            VK_CHECK(s.f.QueueSubmit(s.queue, 1, &si, fence));
        }
        VK_CHECK(s.f.WaitForFences(s.device, 1, &fence, 1, ~(uint64_t)0));
        VK_CHECK(s.f.ResetFences(s.device, 1, &fence));

        uint32_t n = *(const uint32_t*) npass.data;
        if (n > count)
            return false;
        const uint64_t *seeds = (const uint64_t*) out.data;
        passed.insert(passed.end(), seeds, seeds + n);
        return true;
    }

private:
    void updateDescriptors()
    {
        VulkanShared& s = shared();
        HostBuffer *bufs[5] = { &params, &testbuf, &octbuf, &npass, &out };
        vk::DescriptorBufferInfo info[5];
        vk::WriteDescriptorSet writes[5] = {};
        for (uint32_t i = 0; i < 5; i++)
        {
            info[i] = vk::DescriptorBufferInfo{ bufs[i]->buffer, 0, bufs[i]->size };
            writes[i].sType = vk::STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = g_binding_types[i];
            writes[i].pBufferInfo = &info[i];
        }
        s.f.UpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
    }

    vk::Pipeline pipeline = 0;
    HostBuffer params, testbuf, octbuf, npass, out;
    vk::DescriptorPool pool = 0;
    vk::DescriptorSet set = 0;
    vk::CommandPool cmdpool = 0;
    vk::CommandBuffer cmd = nullptr;
    vk::Fence fence = 0;
    uint32_t tg = 1;
    uint32_t ntests = 0;
};

} // namespace

bool gpuBackendAvailable()
{
    return shared().ok;
}

GpuBackend *gpuBackendCreate()
{
    return new VulkanBackend;
}
