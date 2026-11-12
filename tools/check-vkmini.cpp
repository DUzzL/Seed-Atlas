// Compares src/vkmini.h with the official Vulkan headers. Build and run:
//   g++ -std=c++17 -Isrc tools/check-vkmini.cpp -o check-vkmini && ./check-vkmini
// (Debian/Ubuntu: libvulkan-dev). Compilation fails on a mismatch.
#include <vulkan/vulkan.h>
#include "vkmini.h"
#include <cstdio>

#define SAME_SIZE(a, b) static_assert(sizeof(a) == sizeof(b), #a)
#define SAME_OFFSET(a, b, m) static_assert(offsetof(a, m) == offsetof(b, m), #a "." #m)
#define SAME_VALUE(a, b) static_assert((long long)(a) == (long long)(b), #a)

SAME_SIZE(vk::ApplicationInfo, VkApplicationInfo);
SAME_OFFSET(vk::ApplicationInfo, VkApplicationInfo, apiVersion);
SAME_SIZE(vk::InstanceCreateInfo, VkInstanceCreateInfo);
SAME_OFFSET(vk::InstanceCreateInfo, VkInstanceCreateInfo, ppEnabledExtensionNames);
SAME_OFFSET(vk::PhysicalDeviceProperties, VkPhysicalDeviceProperties, deviceType);
SAME_OFFSET(vk::PhysicalDeviceProperties, VkPhysicalDeviceProperties, deviceName);
static_assert(sizeof(vk::PhysicalDeviceProperties) >= sizeof(VkPhysicalDeviceProperties), "properties buffer");
static_assert(vk::LIMITS_MAX_COMPUTE_SHARED_MEMORY_SIZE_OFFSET ==
    offsetof(VkPhysicalDeviceProperties, limits) + offsetof(VkPhysicalDeviceLimits, maxComputeSharedMemorySize), "limit offset");
SAME_SIZE(vk::PhysicalDeviceFeatures, VkPhysicalDeviceFeatures);
static_assert(vk::FEATURE_SHADER_INT64 * 4 == offsetof(VkPhysicalDeviceFeatures, shaderInt64), "shaderInt64");
SAME_SIZE(vk::QueueFamilyProperties, VkQueueFamilyProperties);
SAME_SIZE(vk::PhysicalDeviceMemoryProperties, VkPhysicalDeviceMemoryProperties);
SAME_OFFSET(vk::PhysicalDeviceMemoryProperties, VkPhysicalDeviceMemoryProperties, memoryHeapCount);
SAME_SIZE(vk::DeviceQueueCreateInfo, VkDeviceQueueCreateInfo);
SAME_OFFSET(vk::DeviceQueueCreateInfo, VkDeviceQueueCreateInfo, pQueuePriorities);
SAME_SIZE(vk::DeviceCreateInfo, VkDeviceCreateInfo);
SAME_OFFSET(vk::DeviceCreateInfo, VkDeviceCreateInfo, pEnabledFeatures);
SAME_SIZE(vk::BufferCreateInfo, VkBufferCreateInfo);
SAME_OFFSET(vk::BufferCreateInfo, VkBufferCreateInfo, pQueueFamilyIndices);
SAME_SIZE(vk::MemoryRequirements, VkMemoryRequirements);
SAME_SIZE(vk::MemoryAllocateInfo, VkMemoryAllocateInfo);
SAME_OFFSET(vk::MemoryAllocateInfo, VkMemoryAllocateInfo, memoryTypeIndex);
SAME_SIZE(vk::ShaderModuleCreateInfo, VkShaderModuleCreateInfo);
SAME_OFFSET(vk::ShaderModuleCreateInfo, VkShaderModuleCreateInfo, pCode);
SAME_SIZE(vk::DescriptorSetLayoutBinding, VkDescriptorSetLayoutBinding);
SAME_SIZE(vk::DescriptorSetLayoutCreateInfo, VkDescriptorSetLayoutCreateInfo);
SAME_SIZE(vk::PipelineLayoutCreateInfo, VkPipelineLayoutCreateInfo);
SAME_OFFSET(vk::PipelineLayoutCreateInfo, VkPipelineLayoutCreateInfo, pPushConstantRanges);
SAME_SIZE(vk::SpecializationMapEntry, VkSpecializationMapEntry);
SAME_SIZE(vk::SpecializationInfo, VkSpecializationInfo);
SAME_SIZE(vk::PipelineShaderStageCreateInfo, VkPipelineShaderStageCreateInfo);
SAME_OFFSET(vk::PipelineShaderStageCreateInfo, VkPipelineShaderStageCreateInfo, pSpecializationInfo);
SAME_SIZE(vk::ComputePipelineCreateInfo, VkComputePipelineCreateInfo);
SAME_OFFSET(vk::ComputePipelineCreateInfo, VkComputePipelineCreateInfo, layout);
SAME_OFFSET(vk::ComputePipelineCreateInfo, VkComputePipelineCreateInfo, basePipelineIndex);
SAME_SIZE(vk::DescriptorPoolSize, VkDescriptorPoolSize);
SAME_SIZE(vk::DescriptorPoolCreateInfo, VkDescriptorPoolCreateInfo);
SAME_SIZE(vk::DescriptorSetAllocateInfo, VkDescriptorSetAllocateInfo);
SAME_SIZE(vk::DescriptorBufferInfo, VkDescriptorBufferInfo);
SAME_SIZE(vk::WriteDescriptorSet, VkWriteDescriptorSet);
SAME_OFFSET(vk::WriteDescriptorSet, VkWriteDescriptorSet, pBufferInfo);
SAME_SIZE(vk::CommandPoolCreateInfo, VkCommandPoolCreateInfo);
SAME_SIZE(vk::CommandBufferAllocateInfo, VkCommandBufferAllocateInfo);
SAME_SIZE(vk::CommandBufferBeginInfo, VkCommandBufferBeginInfo);
SAME_SIZE(vk::MemoryBarrierInfo, VkMemoryBarrier);
SAME_SIZE(vk::SubmitInfo, VkSubmitInfo);
SAME_OFFSET(vk::SubmitInfo, VkSubmitInfo, pSignalSemaphores);
SAME_SIZE(vk::FenceCreateInfo, VkFenceCreateInfo);

SAME_VALUE(vk::STRUCTURE_TYPE_APPLICATION_INFO, VK_STRUCTURE_TYPE_APPLICATION_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_INSTANCE_CREATE_INFO, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_DEVICE_CREATE_INFO, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_SUBMIT_INFO, VK_STRUCTURE_TYPE_SUBMIT_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_FENCE_CREATE_INFO, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_BUFFER_CREATE_INFO, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
SAME_VALUE(vk::STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
SAME_VALUE(vk::STRUCTURE_TYPE_MEMORY_BARRIER, VK_STRUCTURE_TYPE_MEMORY_BARRIER);
SAME_VALUE(vk::PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
SAME_VALUE(vk::PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
SAME_VALUE(vk::PHYSICAL_DEVICE_TYPE_CPU, VK_PHYSICAL_DEVICE_TYPE_CPU);
SAME_VALUE(vk::SHARING_MODE_EXCLUSIVE, VK_SHARING_MODE_EXCLUSIVE);
SAME_VALUE(vk::DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
SAME_VALUE(vk::DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
SAME_VALUE(vk::PIPELINE_BIND_POINT_COMPUTE, VK_PIPELINE_BIND_POINT_COMPUTE);
SAME_VALUE(vk::COMMAND_BUFFER_LEVEL_PRIMARY, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
SAME_VALUE(vk::API_VERSION_1_1, VK_API_VERSION_1_1);
SAME_VALUE(vk::QUEUE_COMPUTE_BIT, VK_QUEUE_COMPUTE_BIT);
SAME_VALUE(vk::MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
SAME_VALUE(vk::MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
SAME_VALUE(vk::BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
SAME_VALUE(vk::BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
SAME_VALUE(vk::SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_COMPUTE_BIT);
SAME_VALUE(vk::COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
SAME_VALUE(vk::COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
SAME_VALUE(vk::ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
SAME_VALUE(vk::ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
SAME_VALUE(vk::ACCESS_HOST_READ_BIT, VK_ACCESS_HOST_READ_BIT);
SAME_VALUE(vk::PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
SAME_VALUE(vk::PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_HOST_BIT);

int main()
{
    printf("vkmini.h matches the Vulkan headers (limit offset %zu)\n",
        offsetof(VkPhysicalDeviceProperties, limits) + offsetof(VkPhysicalDeviceLimits, maxComputeSharedMemorySize));
    return 0;
}
