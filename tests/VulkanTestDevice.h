#pragma once
#include "renderer/VulkanContext.h"
#include <memory>
#include <cstring>
#include <fstream>
#include <iostream>
#include <atomic>
#include <filesystem>
#include <cstdlib>
namespace vkTest {
inline bool validationRequired() {
    char value[4]{};
    return GetEnvironmentVariableA("FOV_REQUIRE_VULKAN_VALIDATION", value, 4) == 1 && value[0] == '1';
}
class Unavailable : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Image {
    VkImage handle{};
    VkDeviceMemory memory{};
    uint32_t width{}, height{}, layers{};
    VkFormat format{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    void *mapped{};
};
class Device {
    PFN_vkDestroyDevice destroyDevice{};
    PFN_vkDestroyInstance destroyInstance{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger{};
    VkDebugUtilsMessengerEXT messenger{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    std::vector<std::unique_ptr<Image>> images;
    inline static std::atomic<unsigned> validationErrors{0};
    static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++validationErrors;
            std::cerr << "VULKAN VALIDATION: " << data->pMessage << '\n';
        }
        return VK_FALSE;
    }

  public:
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    std::unique_ptr<fov::VulkanContext> vk;
    VkFence producerFence{};
    bool validation = false;
    Device() {
        PFN_vkGetInstanceProcAddr g{};
        try {
            g = fov::vulkanLoader();
        } catch (const std::exception &e) {
            throw Unavailable(e.what());
        }
        auto enumerateLayers = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            g(nullptr, "vkEnumerateInstanceLayerProperties"));
        auto create = reinterpret_cast<PFN_vkCreateInstance>(g(nullptr, "vkCreateInstance"));
        uint32_t count = 0;
        fov::vkCheck(enumerateLayers(&count, nullptr), "enumerate Vulkan layers");
        std::vector<VkLayerProperties> layers(count);
        fov::vkCheck(enumerateLayers(&count, layers.data()), "enumerate Vulkan layers");
        for (auto &l : layers)
            if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation"))
                validation = true;
        require(!validationRequired() || validation, "Required Khronos validation layer unavailable");
        auto enumExt = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            g(nullptr, "vkEnumerateInstanceExtensionProperties"));
        fov::vkCheck(enumExt(nullptr, &count, nullptr), "enumerate instance extensions");
        std::vector<VkExtensionProperties> extensions(count);
        fov::vkCheck(enumExt(nullptr, &count, extensions.data()), "enumerate instance extensions");
        bool debug = false;
        for (auto &e : extensions)
            if (!strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                debug = true;
        const char *layer = "VK_LAYER_KHRONOS_validation";
        std::vector<const char *> enabledExtensions;
        if (debug)
            enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (validation) {
            fov::vkCheck(enumExt(layer, &count, nullptr), "enumerate validation extensions");
            extensions.resize(count);
            fov::vkCheck(enumExt(layer, &count, extensions.data()), "enumerate validation extensions");
            bool featuresAvailable = false;
            for (auto &e : extensions)
                if (!strcmp(e.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
                    featuresAvailable = true;
            require(featuresAvailable && debug, "validation requires features and debug-utils extensions");
            enabledExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "FOV controlled Vulkan fixture";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        if (validation) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = &layer;
        }
        ci.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        ci.ppEnabledExtensionNames = enabledExtensions.data();
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        m.pfnUserCallback = callback;
        VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        features.enabledValidationFeatureCount = 1;
        features.pEnabledValidationFeatures = &sync;
        if (validation) {
            features.pNext = &m;
            ci.pNext = &features;
        } else if (debug)
            ci.pNext = &m;
        auto instanceResult = create(&ci, nullptr, &binding.instance);
        if (instanceResult == VK_ERROR_INCOMPATIBLE_DRIVER)
            throw Unavailable("Vulkan driver unavailable");
        fov::vkCheck(instanceResult, "create test Vulkan instance");
        destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(g(binding.instance, "vkDestroyInstance"));
        if (debug) {
            auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                g(binding.instance, "vkCreateDebugUtilsMessengerEXT"));
            destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                g(binding.instance, "vkDestroyDebugUtilsMessengerEXT"));
            fov::vkCheck(createMessenger(binding.instance, &m, nullptr, &messenger),
                         "create test debug messenger");
        }
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            g(binding.instance, "vkEnumeratePhysicalDevices"));
        fov::vkCheck(enumerate(binding.instance, &count, nullptr), "enumerate GPU");
        if (!count)
            throw Unavailable("Vulkan GPU required for execution tests");
        std::vector<VkPhysicalDevice> physicals(count);
        fov::vkCheck(enumerate(binding.instance, &count, physicals.data()), "enumerate GPU");
        auto queueProps = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            g(binding.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        bool found = false;
        for (auto physical : physicals) {
            queueProps(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(count);
            queueProps(physical, &count, queues.data());
            for (uint32_t i = 0; i < count; i++)
                if ((queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                    binding.physicalDevice = physical;
                    binding.queueFamilyIndex = i;
                    found = true;
                    break;
                }
            if (found)
                break;
        }
        require(found, "graphics/compute Vulkan queue required");
        float priority = 1;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = binding.queueFamilyIndex;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &q;
        auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(g(binding.instance, "vkCreateDevice"));
        fov::vkCheck(createDevice(binding.physicalDevice, &dc, nullptr, &binding.device),
                     "create test Vulkan device");
        vk = std::make_unique<fov::VulkanContext>(binding);
        destroyDevice =
            reinterpret_cast<PFN_vkDestroyDevice>(vk->GetDeviceProcAddr(binding.device, "vkDestroyDevice"));
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = binding.queueFamilyIndex;
        fov::vkCheck(vk->CreateCommandPool(binding.device, &cp, nullptr, &pool), "create test command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        fov::vkCheck(vk->AllocateCommandBuffers(binding.device, &ca, &command), "allocate test command");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        fov::vkCheck(vk->CreateFence(binding.device, &fi, nullptr, &producerFence), "create producer fence");
        std::cout << "Vulkan device: " << vk->properties.deviceName
                  << "; validation=" << (validation ? "synchronization validation enabled" : "unavailable")
                  << '\n';
    }
    ~Device() {
        shutdown();
    }
    void shutdown() {
        if (vk && binding.device) {
            if (producerFence)
                vk->WaitForFences(binding.device, 1, &producerFence, VK_TRUE, UINT64_MAX);
            for (auto &im : images)
                release(*im);
            if (producerFence)
                vk->DestroyFence(binding.device, producerFence, nullptr);
            if (pool)
                vk->DestroyCommandPool(binding.device, pool, nullptr);
            if (destroyDevice)
                destroyDevice(binding.device, nullptr);
        }
        binding.device = VK_NULL_HANDLE;
        if (messenger) {
            destroyMessenger(binding.instance, messenger, nullptr);
            messenger = VK_NULL_HANDLE;
        }
        if (destroyInstance && binding.instance)
            destroyInstance(binding.instance, nullptr);
        binding.instance = VK_NULL_HANDLE;
    }
    Image &image(uint32_t width, uint32_t height, uint32_t layers, VkFormat format, VkImageUsageFlags usage) {
        auto im = std::make_unique<Image>();
        im->width = width;
        im->height = height;
        im->layers = layers;
        im->format = format;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        fov::vkCheck(vk->CreateImage(binding.device, &ci, nullptr, &im->handle), "create fixture image");
        VkMemoryRequirements req{};
        vk->GetImageMemoryRequirements(binding.device, im->handle, &req);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex = vk->memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        fov::vkCheck(vk->AllocateMemory(binding.device, &a, nullptr, &im->memory), "allocate fixture image");
        fov::vkCheck(vk->BindImageMemory(binding.device, im->handle, im->memory, 0), "bind fixture image");
        auto &result = *im;
        images.push_back(std::move(im));
        return result;
    }
    void release(Image &im) {
        if (im.handle)
            vk->DestroyImage(binding.device, im.handle, nullptr);
        if (im.memory)
            vk->FreeMemory(binding.device, im.memory, nullptr);
        im.handle = {};
        im.memory = {};
    }
    Buffer buffer(VkDeviceSize size) {
        Buffer b;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        fov::vkCheck(vk->CreateBuffer(binding.device, &ci, nullptr, &b.handle), "create fixture staging");
        VkMemoryRequirements req{};
        vk->GetBufferMemoryRequirements(binding.device, b.handle, &req);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex = vk->memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        fov::vkCheck(vk->AllocateMemory(binding.device, &a, nullptr, &b.memory), "allocate fixture staging");
        fov::vkCheck(vk->BindBufferMemory(binding.device, b.handle, b.memory, 0), "bind fixture staging");
        fov::vkCheck(vk->MapMemory(binding.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped),
                     "map fixture staging");
        return b;
    }
    void release(Buffer &b) {
        vk->UnmapMemory(binding.device, b.memory);
        vk->DestroyBuffer(binding.device, b.handle, nullptr);
        vk->FreeMemory(binding.device, b.memory, nullptr);
        b = {};
    }
    VkCommandBuffer begin() {
        fov::vkCheck(vk->ResetCommandBuffer(command, 0), "reset fixture command");
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        fov::vkCheck(vk->BeginCommandBuffer(command, &b), "begin fixture command");
        return command;
    }
    void execute() {
        fov::vkCheck(vk->EndCommandBuffer(command), "end fixture command");
        fov::vkCheck(vk->ResetFences(binding.device, 1, &producerFence), "reset fixture fence");
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s.commandBufferCount = 1;
        s.pCommandBuffers = &command;
        fov::vkCheck(vk->QueueSubmit(vk->queue, 1, &s, producerFence), "submit fixture scene");
        fov::vkCheck(vk->WaitForFences(binding.device, 1, &producerFence, VK_TRUE, 5000000000),
                     "wait fixture evidence");
    }
    void transition(Image &im, VkImageLayout next) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = im.layout;
        b.newLayout = next;
        b.srcAccessMask = im.layout == VK_IMAGE_LAYOUT_UNDEFINED
                              ? 0
                              : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im.handle;
        b.subresourceRange = {fov::vulkanAspects(im.format), 0, 1, 0, im.layers};
        vk->CmdPipelineBarrier(command,
                               im.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                      : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        im.layout = next;
    }
    void upload(Image &im, const std::vector<uint32_t> &pixels) {
        require(pixels.size() == size_t(im.width) * im.height * im.layers, "fixture pixel extent");
        bool packed16 = im.format == VK_FORMAT_D16_UNORM;
        auto staging = buffer(pixels.size() * (packed16 ? 2 : 4));
        if (packed16)
            for (size_t i = 0; i < pixels.size(); ++i)
                static_cast<uint16_t *>(staging.mapped)[i] = uint16_t(pixels[i]);
        else
            std::memcpy(staging.mapped, pixels.data(), pixels.size() * 4);
        begin();
        transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VkImageAspectFlags(fov::vulkanDepthFormat(im.format)
                                                        ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                        : VK_IMAGE_ASPECT_COLOR_BIT),
                                 0, 0, im.layers};
        copy.imageExtent = {im.width, im.height, 1};
        vk->CmdCopyBufferToImage(command, staging.handle, im.handle, im.layout, 1, &copy);
        transition(im, fov::vulkanDepthFormat(im.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                         : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        execute();
        release(staging);
    }
    std::vector<uint32_t> read(Image &im) {
        auto staging = buffer(size_t(im.width) * im.height * im.layers * 4);
        begin();
        transition(im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, im.layers};
        copy.imageExtent = {im.width, im.height, 1};
        vk->CmdCopyImageToBuffer(command, im.handle, im.layout, staging.handle, 1, &copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = staging.handle;
        host.size = VK_WHOLE_SIZE;
        vk->CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                               nullptr, 1, &host, 0, nullptr);
        transition(im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        execute();
        std::vector<uint32_t> pixels(size_t(im.width) * im.height * im.layers);
        std::memcpy(pixels.data(), staging.mapped, pixels.size() * 4);
        release(staging);
        return pixels;
    }
    void validationCheck() {
        require(validationErrors.load() == 0, "Vulkan validation reported an error");
    }
};
inline void ppm(const std::filesystem::path &path, const std::vector<uint32_t> &pixels, uint32_t width,
                uint32_t height, uint32_t layer = 0) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    for (size_t i = size_t(layer) * width * height; i < size_t(layer + 1) * width * height; i++) {
        uint32_t c = pixels[i];
        char rgb[3] = {char(c), char(c >> 8), char(c >> 16)};
        out.write(rgb, 3);
    }
}
} // namespace vkTest
