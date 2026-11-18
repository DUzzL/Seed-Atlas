// Vulkan backend of the GPU eye search (Windows, Linux). As the prefilter's
// backend (gpufilter_vulkan.cpp), it loads the Vulkan library at run time;
// without it (or without a suitable GPU) the eye search runs on the CPU.

#include "eyesearchgpu_backend.h"
#include "vkmini.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "eyesearchgpu_spirv.inc"

namespace {

#define VK_CHECK(call) do { if ((call) != vk::SUCCESS) return false; } while (0)

// A result code that vkmini.h does not name (Vulkan specification value).
const vk::Result RESULT_ERROR_DEVICE_LOST = -4;

// The longest wait for one dispatch. Dispatches are sized to take well below
// a second (eyesearchgpu.cpp); one that takes this long has hung.
const uint64_t FENCE_TIMEOUT_NS = 10ull * 1000 * 1000 * 1000;

void *loadLibrary()
{
#if defined(_WIN32)
    return (void*) LoadLibraryA("vulkan-1.dll");
#elif defined(__APPLE__)
    return dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    return dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}

vk::VoidFunction loadSymbol(void *lib, const char *name)
{
#if defined(_WIN32)
    return (vk::VoidFunction) GetProcAddress((HMODULE) lib, name);
#else
    return (vk::VoidFunction) dlsym(lib, name);
#endif
}

// The kernel's shared memory: one byte table.
const uint32_t SHARED_MEMORY = GPU_EYE_BYTE_WORDS * 4;

struct HostBuffer
{
    vk::Buffer buffer = 0;
    vk::DeviceMemory memory = 0;
    void *data = nullptr;
    vk::DeviceSize size = 0;
};

// Binding 0 is the uniform parameter block, 1 .. 5 are storage buffers.
const int NBIND = 6;

vk::Flags bufferUsage(int binding)
{
    return binding == 0 ? vk::BUFFER_USAGE_UNIFORM_BUFFER_BIT : vk::BUFFER_USAGE_STORAGE_BUFFER_BIT;
}

int32_t descriptorType(int binding)
{
    return binding == 0 ? vk::DESCRIPTOR_TYPE_UNIFORM_BUFFER : vk::DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

// The instance, the device and the pipeline are shared by all workers.
struct VulkanEyeShared
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
    vk::Pipeline pipeline[2] = { 0, 0 };   // pass 1, 2
    std::atomic<bool> lost{false}; // the device was lost: no new workers use it

    bool init()
    {
        return loadFunctions() && pickDevice() && createDevice() && createPipeline();
    }
    bool loadFunctions();
    bool pickDevice();
    bool createDevice();
    bool createPipeline();
    bool createPass(const uint32_t *code, size_t size, vk::Pipeline *p);
    bool createBuffer(HostBuffer& b, vk::DeviceSize size, vk::Flags usage);
    void destroyBuffer(HostBuffer& b);
};

VulkanEyeShared& shared()
{
    static VulkanEyeShared s;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.tried)
    {
        s.tried = true;
        s.ok = s.init();
        if (getenv("SEED_ATLAS_GPU_DEBUG"))
            fprintf(stderr, "gpu eye search: vulkan %s\n", s.ok ? "ready" : "not available");
    }
    return s;
}

bool VulkanEyeShared::loadFunctions()
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

#define VKMINI_LOAD(ret, name, ...) \
    f.name = (decltype(f.name)) f.GetInstanceProcAddr(instance, "vk" #name); \
    if (!f.name) return false;
    VKMINI_FUNCTIONS(VKMINI_LOAD)
#undef VKMINI_LOAD
    return true;
}

// As the prefilter: a GPU with 64-bit integers, enough shared memory and a
// compute queue, discrete before integrated; CPU implementations (such as
// lavapipe) only with SEED_ATLAS_GPU_ALLOW_CPU=1.
bool VulkanEyeShared::pickDevice()
{
    uint32_t n = 0;
    VK_CHECK(f.EnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<vk::PhysicalDevice> devices(n);
    VK_CHECK(f.EnumeratePhysicalDevices(instance, &n, devices.data()));
    const bool allowCpu = getenv("SEED_ATLAS_GPU_ALLOW_CPU") != nullptr;

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
        if (!feat.features[vk::FEATURE_SHADER_INT64] || shmem < SHARED_MEMORY)
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
                fprintf(stderr, "gpu eye search: vulkan device %s\n", props.deviceName);
        }
    }
    return gpu != nullptr;
}

bool VulkanEyeShared::createDevice()
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

bool VulkanEyeShared::createPipeline()
{
    vk::DescriptorSetLayoutBinding bindings[NBIND] = {};
    for (int i = 0; i < NBIND; i++)
    {
        bindings[i].binding = uint32_t(i);
        bindings[i].descriptorType = descriptorType(i);
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = vk::SHADER_STAGE_COMPUTE_BIT;
    }
    vk::DescriptorSetLayoutCreateInfo li = {};
    li.sType = vk::STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = NBIND;
    li.pBindings = bindings;
    VK_CHECK(f.CreateDescriptorSetLayout(device, &li, nullptr, &setLayout));

    vk::PipelineLayoutCreateInfo pi = {};
    pi.sType = vk::STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &setLayout;
    VK_CHECK(f.CreatePipelineLayout(device, &pi, nullptr, &pipelineLayout));

    return createPass(g_eye_spirv_1, sizeof(g_eye_spirv_1), &pipeline[0]) &&
        createPass(g_eye_spirv_2, sizeof(g_eye_spirv_2), &pipeline[1]);
}

bool VulkanEyeShared::createPass(const uint32_t *code, size_t size, vk::Pipeline *p)
{
    vk::ShaderModule shader = 0;
    vk::ShaderModuleCreateInfo si = {};
    si.sType = vk::STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = size;
    si.pCode = code;
    VK_CHECK(f.CreateShaderModule(device, &si, nullptr, &shader));
    vk::ComputePipelineCreateInfo ci = {};
    ci.sType = vk::STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = vk::STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = vk::SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = shader;
    ci.stage.pName = "main";
    ci.layout = pipelineLayout;
    ci.basePipelineIndex = -1;
    VK_CHECK(f.CreateComputePipelines(device, 0, 1, &ci, nullptr, p));
    return true; // the shader module stays alive with the pipeline
}

bool VulkanEyeShared::createBuffer(HostBuffer& b, vk::DeviceSize size, vk::Flags usage)
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

void VulkanEyeShared::destroyBuffer(HostBuffer& b)
{
    if (b.buffer)
        f.DestroyBuffer(device, b.buffer, nullptr);
    if (b.memory)
        f.FreeMemory(device, b.memory, nullptr); // also unmaps
    b = HostBuffer();
}

class VulkanEyeBackend : public GpuEyeBackend
{
public:
    ~VulkanEyeBackend() override
    {
        VulkanEyeShared& s = shared();
        if (!s.ok)
            return;
        // A dispatch that timed out may still use the buffers: wait for it
        // once more, and leak them rather than free memory the GPU uses.
        if (pending && s.f.WaitForFences(s.device, 1, &fence, 1, FENCE_TIMEOUT_NS) != vk::SUCCESS)
            return;
        for (HostBuffer& b : bufs)
            s.destroyBuffer(b);
        if (pool)
            s.f.DestroyDescriptorPool(s.device, pool, nullptr);
        if (cmdpool)
            s.f.DestroyCommandPool(s.device, cmdpool, nullptr);
        if (fence)
            s.f.DestroyFence(s.device, fence, nullptr);
    }

    bool init() override
    {
        VulkanEyeShared& s = shared();
        if (!s.ok || s.lost)
            return false;
        // initial sizes; plans, tables and the output grow on demand
        const vk::DeviceSize sizes[NBIND] = {
            sizeof(GpuEyeParams), 2 * 65536 * sizeof(uint64_t), 512 * sizeof(GpuEyePlan),
            GPU_EYE_STD_TABLES * GPU_EYE_BYTE_WORDS * sizeof(uint32_t), 4096 * sizeof(uint32_t),
            65536 * sizeof(uint32_t),
        };
        for (int i = 0; i < NBIND; i++)
            if (!s.createBuffer(bufs[i], sizes[i], bufferUsage(i)))
                return false;

        vk::DescriptorPoolSize ps[2] = {
            { vk::DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { vk::DESCRIPTOR_TYPE_STORAGE_BUFFER, NBIND - 1 },
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
        ready = true;
        return true;
    }

    uint64_t *deco() override
    {
        return ready ? (uint64_t*) bufs[1].data : nullptr;
    }

    GpuEyePlan *plans(uint32_t n) override
    {
        return fit(2, vk::DeviceSize(n) * sizeof(GpuEyePlan), false) ? (GpuEyePlan*) bufs[2].data : nullptr;
    }

    uint32_t *tables(uint32_t words) override
    {
        return fit(3, vk::DeviceSize(words) * sizeof(uint32_t), true) ?
            (uint32_t*) bufs[3].data : nullptr;
    }

    GpuEyeRunResult run(const GpuEyeParams& p, uint32_t listGroups, std::vector<uint32_t>& kept,
        uint32_t need[2]) override
    {
        return submit(p, listGroups, kept, need);
    }

private:
    GpuEyeRunResult submit(const GpuEyeParams& p, uint32_t listGroups, std::vector<uint32_t>& kept,
        uint32_t need[2])
    {
        VulkanEyeShared& s = shared();
        if (!ready || s.lost || p.nplans == 0 || listGroups == 0 || p.firstRange > p.nplans ||
            p.nunits > p.firstRange ||
            vk::DeviceSize(p.nplans) * sizeof(GpuEyePlan) > bufs[2].size)
            return GPU_EYE_RUN_FAILED;
        if (!fit(4, (vk::DeviceSize(p.maxOut) + 1) * sizeof(uint32_t), false) ||
            !fit(5, (vk::DeviceSize(p.maxList) + 1) * sizeof(uint32_t), false))
            return GPU_EYE_RUN_FAILED;
        memcpy(bufs[0].data, &p, sizeof(p));
        *(uint32_t*) bufs[4].data = 0;
        *(uint32_t*) bufs[5].data = 0;

        vk::CommandBufferBeginInfo bi = {};
        bi.sType = vk::STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = vk::COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (s.f.BeginCommandBuffer(cmd, &bi) != vk::SUCCESS)
            return GPU_EYE_RUN_FAILED;
        s.f.CmdBindDescriptorSets(cmd, vk::PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayout,
            0, 1, &set, 0, nullptr);
        s.f.CmdBindPipeline(cmd, vk::PIPELINE_BIND_POINT_COMPUTE, s.pipeline[0]);
        s.f.CmdDispatch(cmd, (p.nunits + p.nplans - p.firstRange) * p.groupsPerPlan, 1, 1);
        // pass 2 reads the list and adds to the output of pass 1
        vk::MemoryBarrierInfo lb = {};
        lb.sType = vk::STRUCTURE_TYPE_MEMORY_BARRIER;
        lb.srcAccessMask = vk::ACCESS_SHADER_WRITE_BIT;
        lb.dstAccessMask = vk::ACCESS_SHADER_READ_BIT | vk::ACCESS_SHADER_WRITE_BIT;
        s.f.CmdPipelineBarrier(cmd, vk::PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            vk::PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &lb, 0, nullptr, 0, nullptr);
        s.f.CmdBindPipeline(cmd, vk::PIPELINE_BIND_POINT_COMPUTE, s.pipeline[1]);
        s.f.CmdDispatch(cmd, listGroups, 1, 1);
        vk::MemoryBarrierInfo mb = {};
        mb.sType = vk::STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = vk::ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = vk::ACCESS_HOST_READ_BIT;
        s.f.CmdPipelineBarrier(cmd, vk::PIPELINE_STAGE_COMPUTE_SHADER_BIT, vk::PIPELINE_STAGE_HOST_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
        if (s.f.EndCommandBuffer(cmd) != vk::SUCCESS)
            return GPU_EYE_RUN_FAILED;

        vk::SubmitInfo si = {};
        si.sType = vk::STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        {
            std::lock_guard<std::mutex> lock(s.queueMutex);
            const vk::Result r = s.f.QueueSubmit(s.queue, 1, &si, fence);
            if (r != vk::SUCCESS)
            {
                if (r == RESULT_ERROR_DEVICE_LOST)
                    s.lost = true;
                return GPU_EYE_RUN_FAILED;
            }
        }
        pending = true;
        const vk::Result w = s.f.WaitForFences(s.device, 1, &fence, 1, FENCE_TIMEOUT_NS);
        if (w != vk::SUCCESS)
        {   // hung or lost: this backend is not used again (the destructor
            // waits for the dispatch once more before it frees anything)
            if (w == RESULT_ERROR_DEVICE_LOST)
                s.lost = true;
            ready = false;
            return GPU_EYE_RUN_FAILED;
        }
        pending = false;
        if (s.f.ResetFences(s.device, 1, &fence) != vk::SUCCESS)
        {
            ready = false;
            return GPU_EYE_RUN_FAILED;
        }

        // the counters count on past the capacity: the sizes needed
        const uint32_t *o = (const uint32_t*) bufs[4].data;
        need[0] = o[0];
        need[1] = *(const uint32_t*) bufs[5].data;
        if (need[0] > p.maxOut || need[1] > p.maxList)
            return GPU_EYE_RUN_OVERFLOW;
        kept.insert(kept.end(), o + 1, o + 1 + o[0]);
        return GPU_EYE_RUN_OK;
    }

    // Binding i sized for 'size' bytes: a larger buffer if needed, a smaller
    // one if it is more than 4x too large. With 'keep', the content is
    // copied over.
    bool fit(int i, vk::DeviceSize size, bool keep)
    {
        if (!ready)
            return false;
        if (size <= bufs[i].size && (bufs[i].size <= (vk::DeviceSize(1) << 18) || size >= bufs[i].size / 4))
            return true;
        VulkanEyeShared& s = shared();
        vk::DeviceSize want = size + size / 4 > 4096 ? size + size / 4 : 4096;
        HostBuffer nb;
        if (!s.createBuffer(nb, want, bufferUsage(i)))
        {
            s.destroyBuffer(nb);
            ready = false;
            return false;
        }
        if (keep)
            memcpy(nb.data, bufs[i].data, size_t(std::min(bufs[i].size, want)));
        s.destroyBuffer(bufs[i]);
        bufs[i] = nb;
        updateDescriptors();
        return true;
    }

    void updateDescriptors()
    {
        VulkanEyeShared& s = shared();
        vk::DescriptorBufferInfo info[NBIND];
        vk::WriteDescriptorSet writes[NBIND] = {};
        for (int i = 0; i < NBIND; i++)
        {
            info[i] = vk::DescriptorBufferInfo{ bufs[i].buffer, 0, bufs[i].size };
            writes[i].sType = vk::STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = uint32_t(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = descriptorType(i);
            writes[i].pBufferInfo = &info[i];
        }
        s.f.UpdateDescriptorSets(s.device, NBIND, writes, 0, nullptr);
    }

    // params, deco, plans, tables, output, list
    HostBuffer bufs[NBIND];
    vk::DescriptorPool pool = 0;
    vk::DescriptorSet set = 0;
    vk::CommandPool cmdpool = 0;
    vk::CommandBuffer cmd = nullptr;
    vk::Fence fence = 0;
    bool ready = false;
    bool pending = false;   // a submitted dispatch has not been seen to finish
};

} // namespace

bool gpuEyeBackendAvailable()
{
    VulkanEyeShared& s = shared();
    return s.ok && !s.lost;
}

GpuEyeBackend *gpuEyeBackendCreate()
{
    return new VulkanEyeBackend;
}
