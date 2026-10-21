#pragma once
#include "common/Platform.h"
#include <vector>
#include <string>

namespace fov {
// clang-format off
#define FOV_VK_INSTANCE_FUNCTIONS(X) \
    X(EnumeratePhysicalDevices) \
    X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceFormatProperties) \
    X(GetDeviceProcAddr)
#define FOV_VK_DEVICE_FUNCTIONS(X) \
    X(GetDeviceQueue) \
    X(CreateImage) \
    X(DestroyImage) \
    X(GetImageMemoryRequirements) \
    X(AllocateMemory) \
    X(FreeMemory) \
    X(BindImageMemory) \
    X(CreateImageView) \
    X(DestroyImageView) \
    X(CreateBuffer) \
    X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) \
    X(BindBufferMemory) \
    X(MapMemory) \
    X(UnmapMemory) \
    X(CreateCommandPool) \
    X(DestroyCommandPool) \
    X(AllocateCommandBuffers) \
    X(ResetCommandBuffer) \
    X(BeginCommandBuffer) \
    X(EndCommandBuffer) \
    X(CmdPipelineBarrier) \
    X(CmdCopyImage) \
    X(CmdCopyBufferToImage) \
    X(CmdCopyImageToBuffer) \
    X(CmdClearColorImage) \
    X(CmdClearDepthStencilImage) \
    X(CreateFence) \
    X(DestroyFence) \
    X(GetFenceStatus) \
    X(ResetFences) \
    X(WaitForFences) \
    X(QueueSubmit) \
    X(CreateSampler) \
    X(DestroySampler) \
    X(CreateShaderModule) \
    X(DestroyShaderModule) \
    X(CreateDescriptorSetLayout) \
    X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) \
    X(DestroyPipelineLayout) \
    X(CreateComputePipelines) \
    X(DestroyPipeline) \
    X(CreateDescriptorPool) \
    X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) \
    X(UpdateDescriptorSets) \
    X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) \
    X(CmdDispatch) \
    X(CreateQueryPool) \
    X(DestroyQueryPool) \
    X(CmdResetQueryPool) \
    X(CmdWriteTimestamp) \
    X(GetQueryPoolResults)
// clang-format on

PFN_vkGetInstanceProcAddr vulkanLoader();
void vkCheck(VkResult result, const char *operation);
bool vulkanColorFormat(int64_t format);
bool vulkanDepthFormat(int64_t format);
bool vulkanSrgb(VkFormat format);
VkImageAspectFlags vulkanAspects(VkFormat format);

struct VulkanContext {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{}, queueIndex{}, timestampBits{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
#define FOV_DECLARE_VK(name) PFN_vk##name name{};
    FOV_VK_INSTANCE_FUNCTIONS(FOV_DECLARE_VK)
    FOV_VK_DEVICE_FUNCTIONS(FOV_DECLARE_VK)
#undef FOV_DECLARE_VK
    explicit VulkanContext(const XrGraphicsBindingVulkanKHR &binding);
    uint32_t memoryType(uint32_t mask, VkMemoryPropertyFlags flags) const;
    bool formatSupports(VkFormat format, VkFormatFeatureFlags flags) const;
};
} // namespace fov
