#ifndef VKMINI_H
#define VKMINI_H

// The small part of the Vulkan 1.1 API that the GPU prefilter uses, so that
// builds do not need the Vulkan SDK. The library is loaded at run time.
// Types, values and layouts are those of the official vulkan_core.h; the
// test tools/check-vkmini.cpp compares them with it.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32) && !defined(_WIN64)
#define VKMINI_CALL __stdcall
#else
#define VKMINI_CALL
#endif

namespace vk {

typedef uint32_t Bool32;
typedef uint32_t Flags;
typedef uint64_t DeviceSize;
typedef int32_t Result;

// dispatchable handles are pointers, the others are 64-bit values
typedef struct Instance_T *Instance;
typedef struct PhysicalDevice_T *PhysicalDevice;
typedef struct Device_T *Device;
typedef struct Queue_T *Queue;
typedef struct CommandBuffer_T *CommandBuffer;
typedef uint64_t Buffer, DeviceMemory, ShaderModule, DescriptorSetLayout, PipelineLayout,
    Pipeline, PipelineCache, DescriptorPool, DescriptorSet, CommandPool, Fence, Semaphore;

enum : int32_t {
    SUCCESS = 0,
    // VkStructureType
    STRUCTURE_TYPE_APPLICATION_INFO = 0,
    STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    STRUCTURE_TYPE_SUBMIT_INFO = 4,
    STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
    STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
    STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
    STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29,
    STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
    STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
    STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
    STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
    STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
    STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
    STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
    STRUCTURE_TYPE_MEMORY_BARRIER = 46,
    // VkPhysicalDeviceType
    PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
    PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
    PHYSICAL_DEVICE_TYPE_CPU = 4,
    // other enums
    SHARING_MODE_EXCLUSIVE = 0,
    DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6,
    DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
    PIPELINE_BIND_POINT_COMPUTE = 1,
    COMMAND_BUFFER_LEVEL_PRIMARY = 0,
};

enum : uint32_t {
    API_VERSION_1_1 = (1u << 22) | (1u << 12),
    QUEUE_COMPUTE_BIT = 0x2,
    MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x2,
    MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x4,
    BUFFER_USAGE_UNIFORM_BUFFER_BIT = 0x10,
    BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x20,
    SHADER_STAGE_COMPUTE_BIT = 0x20,
    COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2,
    COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1,
    ACCESS_SHADER_READ_BIT = 0x20,
    ACCESS_SHADER_WRITE_BIT = 0x40,
    ACCESS_HOST_READ_BIT = 0x2000,
    PIPELINE_STAGE_COMPUTE_SHADER_BIT = 0x800,
    PIPELINE_STAGE_HOST_BIT = 0x4000,
};

struct ApplicationInfo {
    int32_t sType; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion;
    const char *pEngineName; uint32_t engineVersion;
    uint32_t apiVersion;
};
struct InstanceCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    const ApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
};
// VkPhysicalDeviceProperties: only the leading members are read.
struct PhysicalDeviceProperties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    int32_t deviceType;
    char deviceName[256];
    uint8_t rest[1024];
};
enum { LIMITS_MAX_COMPUTE_SHARED_MEMORY_SIZE_OFFSET = 512 }; // in PhysicalDeviceProperties
struct PhysicalDeviceFeatures { Bool32 features[55]; };
enum { FEATURE_SHADER_INT64 = 40 };
struct Extent3D { uint32_t width, height, depth; };
struct QueueFamilyProperties {
    Flags queueFlags; uint32_t queueCount; uint32_t timestampValidBits;
    Extent3D minImageTransferGranularity;
};
struct MemoryType { Flags propertyFlags; uint32_t heapIndex; };
struct MemoryHeap { DeviceSize size; Flags flags; };
struct PhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount; MemoryType memoryTypes[32];
    uint32_t memoryHeapCount; MemoryHeap memoryHeaps[16];
};
struct DeviceQueueCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    uint32_t queueFamilyIndex; uint32_t queueCount; const float *pQueuePriorities;
};
struct DeviceCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    uint32_t queueCreateInfoCount; const DeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const PhysicalDeviceFeatures *pEnabledFeatures;
};
struct BufferCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    DeviceSize size; Flags usage; int32_t sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices;
};
struct MemoryRequirements { DeviceSize size, alignment; uint32_t memoryTypeBits; };
struct MemoryAllocateInfo {
    int32_t sType; const void *pNext; DeviceSize allocationSize; uint32_t memoryTypeIndex;
};
struct ShaderModuleCreateInfo {
    int32_t sType; const void *pNext; Flags flags; size_t codeSize; const uint32_t *pCode;
};
struct DescriptorSetLayoutBinding {
    uint32_t binding; int32_t descriptorType; uint32_t descriptorCount;
    Flags stageFlags; const void *pImmutableSamplers;
};
struct DescriptorSetLayoutCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    uint32_t bindingCount; const DescriptorSetLayoutBinding *pBindings;
};
struct PipelineLayoutCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    uint32_t setLayoutCount; const DescriptorSetLayout *pSetLayouts;
    uint32_t pushConstantRangeCount; const void *pPushConstantRanges;
};
struct SpecializationMapEntry { uint32_t constantID; uint32_t offset; size_t size; };
struct SpecializationInfo {
    uint32_t mapEntryCount; const SpecializationMapEntry *pMapEntries;
    size_t dataSize; const void *pData;
};
struct PipelineShaderStageCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    Flags stage; ShaderModule module; const char *pName;
    const SpecializationInfo *pSpecializationInfo;
};
struct ComputePipelineCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    PipelineShaderStageCreateInfo stage; PipelineLayout layout;
    Pipeline basePipelineHandle; int32_t basePipelineIndex;
};
struct DescriptorPoolSize { int32_t type; uint32_t descriptorCount; };
struct DescriptorPoolCreateInfo {
    int32_t sType; const void *pNext; Flags flags;
    uint32_t maxSets; uint32_t poolSizeCount; const DescriptorPoolSize *pPoolSizes;
};
struct DescriptorSetAllocateInfo {
    int32_t sType; const void *pNext; DescriptorPool descriptorPool;
    uint32_t descriptorSetCount; const DescriptorSetLayout *pSetLayouts;
};
struct DescriptorBufferInfo { Buffer buffer; DeviceSize offset; DeviceSize range; };
struct WriteDescriptorSet {
    int32_t sType; const void *pNext; DescriptorSet dstSet;
    uint32_t dstBinding; uint32_t dstArrayElement; uint32_t descriptorCount;
    int32_t descriptorType; const void *pImageInfo;
    const DescriptorBufferInfo *pBufferInfo; const void *pTexelBufferView;
};
struct CommandPoolCreateInfo {
    int32_t sType; const void *pNext; Flags flags; uint32_t queueFamilyIndex;
};
struct CommandBufferAllocateInfo {
    int32_t sType; const void *pNext; CommandPool commandPool;
    int32_t level; uint32_t commandBufferCount;
};
struct CommandBufferBeginInfo {
    int32_t sType; const void *pNext; Flags flags; const void *pInheritanceInfo;
};
struct MemoryBarrierInfo { // VkMemoryBarrier (MemoryBarrier is a macro in windows.h)
    int32_t sType; const void *pNext; Flags srcAccessMask; Flags dstAccessMask;
};
struct SubmitInfo {
    int32_t sType; const void *pNext;
    uint32_t waitSemaphoreCount; const Semaphore *pWaitSemaphores; const Flags *pWaitDstStageMask;
    uint32_t commandBufferCount; const CommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount; const Semaphore *pSignalSemaphores;
};
struct FenceCreateInfo { int32_t sType; const void *pNext; Flags flags; };

typedef void (VKMINI_CALL *VoidFunction)(void);

// The functions, as members of a table that is filled at run time.
#define VKMINI_FUNCTIONS(X) \
    X(Result, CreateInstance, const InstanceCreateInfo*, const void*, Instance*) \
    X(Result, EnumeratePhysicalDevices, Instance, uint32_t*, PhysicalDevice*) \
    X(void, GetPhysicalDeviceProperties, PhysicalDevice, PhysicalDeviceProperties*) \
    X(void, GetPhysicalDeviceFeatures, PhysicalDevice, PhysicalDeviceFeatures*) \
    X(void, GetPhysicalDeviceQueueFamilyProperties, PhysicalDevice, uint32_t*, QueueFamilyProperties*) \
    X(void, GetPhysicalDeviceMemoryProperties, PhysicalDevice, PhysicalDeviceMemoryProperties*) \
    X(Result, CreateDevice, PhysicalDevice, const DeviceCreateInfo*, const void*, Device*) \
    X(void, GetDeviceQueue, Device, uint32_t, uint32_t, Queue*) \
    X(Result, CreateBuffer, Device, const BufferCreateInfo*, const void*, Buffer*) \
    X(void, DestroyBuffer, Device, Buffer, const void*) \
    X(void, GetBufferMemoryRequirements, Device, Buffer, MemoryRequirements*) \
    X(Result, AllocateMemory, Device, const MemoryAllocateInfo*, const void*, DeviceMemory*) \
    X(void, FreeMemory, Device, DeviceMemory, const void*) \
    X(Result, BindBufferMemory, Device, Buffer, DeviceMemory, DeviceSize) \
    X(Result, MapMemory, Device, DeviceMemory, DeviceSize, DeviceSize, Flags, void**) \
    X(Result, CreateShaderModule, Device, const ShaderModuleCreateInfo*, const void*, ShaderModule*) \
    X(Result, CreateDescriptorSetLayout, Device, const DescriptorSetLayoutCreateInfo*, const void*, DescriptorSetLayout*) \
    X(Result, CreatePipelineLayout, Device, const PipelineLayoutCreateInfo*, const void*, PipelineLayout*) \
    X(Result, CreateComputePipelines, Device, PipelineCache, uint32_t, const ComputePipelineCreateInfo*, const void*, Pipeline*) \
    X(Result, CreateDescriptorPool, Device, const DescriptorPoolCreateInfo*, const void*, DescriptorPool*) \
    X(void, DestroyDescriptorPool, Device, DescriptorPool, const void*) \
    X(Result, AllocateDescriptorSets, Device, const DescriptorSetAllocateInfo*, DescriptorSet*) \
    X(void, UpdateDescriptorSets, Device, uint32_t, const WriteDescriptorSet*, uint32_t, const void*) \
    X(Result, CreateCommandPool, Device, const CommandPoolCreateInfo*, const void*, CommandPool*) \
    X(void, DestroyCommandPool, Device, CommandPool, const void*) \
    X(Result, AllocateCommandBuffers, Device, const CommandBufferAllocateInfo*, CommandBuffer*) \
    X(Result, BeginCommandBuffer, CommandBuffer, const CommandBufferBeginInfo*) \
    X(Result, EndCommandBuffer, CommandBuffer) \
    X(void, CmdBindPipeline, CommandBuffer, int32_t, Pipeline) \
    X(void, CmdBindDescriptorSets, CommandBuffer, int32_t, PipelineLayout, uint32_t, uint32_t, const DescriptorSet*, uint32_t, const uint32_t*) \
    X(void, CmdDispatch, CommandBuffer, uint32_t, uint32_t, uint32_t) \
    X(void, CmdPipelineBarrier, CommandBuffer, Flags, Flags, Flags, uint32_t, const MemoryBarrierInfo*, uint32_t, const void*, uint32_t, const void*) \
    X(Result, CreateFence, Device, const FenceCreateInfo*, const void*, Fence*) \
    X(void, DestroyFence, Device, Fence, const void*) \
    X(Result, ResetFences, Device, uint32_t, const Fence*) \
    X(Result, WaitForFences, Device, uint32_t, const Fence*, Bool32, uint64_t) \
    X(Result, QueueSubmit, Queue, uint32_t, const SubmitInfo*, Fence)

struct Functions {
    VoidFunction (VKMINI_CALL *GetInstanceProcAddr)(Instance, const char*) = nullptr;
#define VKMINI_MEMBER(ret, name, ...) ret (VKMINI_CALL *name)(__VA_ARGS__) = nullptr;
    VKMINI_FUNCTIONS(VKMINI_MEMBER)
#undef VKMINI_MEMBER
};

} // namespace vk

#endif // VKMINI_H
