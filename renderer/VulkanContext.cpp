#include "VulkanContext.h"
#include <algorithm>
namespace fov {
PFN_vkGetInstanceProcAddr vulkanLoader() {
    static HMODULE module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        throw std::runtime_error("Vulkan loader unavailable: diagnostic passthrough");
    auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
    if (!proc)
        throw std::runtime_error("Vulkan loader has no vkGetInstanceProcAddr");
    return proc;
}
void vkCheck(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " VkResult " + std::to_string(result));
}
bool vulkanColorFormat(int64_t f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM ||
           f == VK_FORMAT_B8G8R8A8_SRGB;
}
bool vulkanDepthFormat(int64_t f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
bool vulkanSrgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB;
}
VkImageAspectFlags vulkanAspects(VkFormat f) {
    if (!vulkanDepthFormat(f))
        return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT |
           ((f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT)
                ? VK_IMAGE_ASPECT_STENCIL_BIT
                : 0);
}
VulkanContext::VulkanContext(const XrGraphicsBindingVulkanKHR &b)
    : instance(b.instance), physical(b.physicalDevice), device(b.device), family(b.queueFamilyIndex),
      queueIndex(b.queueIndex) {
    if (!instance || !physical || !device)
        throw std::runtime_error("Vulkan binding contains null handles");
    auto gipa = vulkanLoader();
#define FOV_LOAD_INSTANCE(name)                                                                              \
    name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name));                                       \
    if (!name)                                                                                               \
        throw std::runtime_error("Missing Vulkan instance command vk" #name);
    FOV_VK_INSTANCE_FUNCTIONS(FOV_LOAD_INSTANCE)
#undef FOV_LOAD_INSTANCE
    uint32_t count = 0;
    vkCheck(EnumeratePhysicalDevices(instance, &count, nullptr), "enumerate physical devices");
    std::vector<VkPhysicalDevice> devices(count);
    vkCheck(EnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate physical devices");
    if (std::find(devices.begin(), devices.end(), physical) == devices.end())
        throw std::runtime_error("Vulkan physical device does not belong to binding instance");
    GetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1)
        throw std::runtime_error("Vulkan 1.1 or newer required");
    GetPhysicalDeviceMemoryProperties(physical, &memory);
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    if (family >= count || queueIndex >= families[family].queueCount)
        throw std::runtime_error("Vulkan binding queue index out of range");
    if (!(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
        !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        throw std::runtime_error("Vulkan bound queue requires graphics and compute capability");
    timestampBits = families[family].timestampValidBits;
#define FOV_LOAD_DEVICE(name)                                                                                \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name));                            \
    if (!name)                                                                                               \
        throw std::runtime_error("Missing Vulkan device command vk" #name);
    FOV_VK_DEVICE_FUNCTIONS(FOV_LOAD_DEVICE)
#undef FOV_LOAD_DEVICE
    GetDeviceQueue(device, family, queueIndex, &queue);
    if (!queue)
        throw std::runtime_error("Vulkan binding references an unavailable device queue");
}
uint32_t VulkanContext::memoryType(uint32_t mask, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("Vulkan compatible memory type unavailable");
}
bool VulkanContext::formatSupports(VkFormat format, VkFormatFeatureFlags flags) const {
    VkFormatProperties p{};
    GetPhysicalDeviceFormatProperties(physical, format, &p);
    return (p.optimalTilingFeatures & flags) == flags;
}
} // namespace fov
