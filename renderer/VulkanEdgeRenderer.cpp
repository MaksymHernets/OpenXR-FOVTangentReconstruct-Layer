#include "IEdgeRenderer.h"
#include "VulkanContext.h"
#include "VulkanShader.h"
#include <array>
#include <map>
#include <chrono>
#include <cstring>

namespace fov {
namespace {
struct alignas(16) VulkanParameters {
    ShaderConstants image;
    TemporalGpu temporal;
    int32_t destination[4];
    uint32_t outputFlags[4];
};
static_assert(sizeof(VulkanParameters) == 896);
static_assert(offsetof(VulkanParameters, temporal) == 144);
static_assert(offsetof(VulkanParameters, destination) == 864);
} // namespace
class VulkanEdgeRenderer final : public IEdgeRenderer {
    VulkanContext vk;
    struct Image {
        VkImage handle{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkFormat format{};
        uint32_t width{}, height{}, layers{};
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDeviceSize bytes{};
        bool owned = true;
    };
    struct Buffer {
        VkBuffer handle{};
        VkDeviceMemory memory{};
        void *mapped{};
        VkDeviceSize bytes{};
    };
    struct Slot {
        VkCommandBuffer command{};
        VkFence fence{};
        VkDescriptorSet descriptors{};
        Buffer parameters, counters;
        bool pending = false, measured = false, metric = false;
        uint64_t token{};
        uint32_t index{}, stage{};
        std::string view;
    };
    static constexpr uint32_t slotCount = 32;
    std::array<Slot, slotCount> slots{};
    VkCommandPool commandPool{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSetLayout descriptorLayout{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    VkSampler colorSampler{}, depthSampler{};
    VkQueryPool queries{};
    std::map<uint64_t, Image> images, scratch;
    std::map<std::pair<uint64_t, uint32_t>, Image> directViews;
    std::map<uint64_t, TemporalMetrics> metrics;
    std::map<std::string, Json> viewTimings;
    TemporalInput temporal;
    size_t cursor = 0;
    bool timingEnabled = false, failed = false;
    std::optional<double> elapsed, copyElapsed, historyElapsed;
    double cpuSubmitMs = 0;
    VkDeviceSize allocatedBytes = 0, peakBytes = 0;
    uint64_t allocations = 0, submissions = 0, directPasses = 0, intermediatePasses = 0, busyFallbacks = 0;
    void check(VkResult result, const char *operation) {
        if (result != VK_SUCCESS)
            failed = true;
        vkCheck(result, operation);
    }
    void account(VkDeviceSize bytes) {
        allocatedBytes += bytes;
        peakBytes = std::max(peakBytes, allocatedBytes);
        ++allocations;
    }
    void destroy(Image &im) {
        if (im.view)
            vk.DestroyImageView(vk.device, im.view, nullptr);
        if (im.owned && im.handle)
            vk.DestroyImage(vk.device, im.handle, nullptr);
        if (im.memory) {
            vk.FreeMemory(vk.device, im.memory, nullptr);
            allocatedBytes -= im.bytes;
        }
        im = {};
    }
    void destroy(Buffer &b) {
        if (b.mapped)
            vk.UnmapMemory(vk.device, b.memory);
        if (b.handle)
            vk.DestroyBuffer(vk.device, b.handle, nullptr);
        if (b.memory) {
            vk.FreeMemory(vk.device, b.memory, nullptr);
            allocatedBytes -= b.bytes;
        }
        b = {};
    }
    Buffer makeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage) {
        Buffer b;
        try {
            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            ci.size = bytes;
            ci.usage = usage;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vk.CreateBuffer(vk.device, &ci, nullptr, &b.handle), "create Vulkan buffer");
            VkMemoryRequirements req{};
            vk.GetBufferMemoryRequirements(vk.device, b.handle, &req);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = req.size;
            allocation.memoryTypeIndex =
                vk.memoryType(req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vk.AllocateMemory(vk.device, &allocation, nullptr, &b.memory),
                  "allocate Vulkan buffer memory");
            b.bytes = req.size;
            account(b.bytes);
            check(vk.BindBufferMemory(vk.device, b.handle, b.memory, 0), "bind Vulkan buffer");
            check(vk.MapMemory(vk.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped),
                  "map Vulkan parameter memory");
            std::memset(b.mapped, 0, size_t(bytes));
            return b;
        } catch (...) {
            destroy(b);
            throw;
        }
    }
    Image makeImage(uint32_t width, uint32_t height, uint32_t layers, VkFormat format,
                    VkImageUsageFlags usage) {
        Image im;
        im.width = width;
        im.height = height;
        im.layers = layers;
        im.format = format;
        try {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = format;
            ci.extent = {width, height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = layers;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = usage;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vk.CreateImage(vk.device, &ci, nullptr, &im.handle), "create private Vulkan image");
            VkMemoryRequirements req{};
            vk.GetImageMemoryRequirements(vk.device, im.handle, &req);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = req.size;
            allocation.memoryTypeIndex =
                vk.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vk.AllocateMemory(vk.device, &allocation, nullptr, &im.memory),
                  "allocate private Vulkan image");
            im.bytes = req.size;
            account(im.bytes);
            check(vk.BindImageMemory(vk.device, im.handle, im.memory, 0), "bind private Vulkan image");
            createView(im, vulkanDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
            return im;
        } catch (...) {
            destroy(im);
            throw;
        }
    }
    void createView(Image &im, VkImageAspectFlags aspect, uint32_t firstLayer = 0) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = im.handle;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        ci.format = im.format;
        ci.subresourceRange = {aspect, 0, 1, firstLayer, im.layers};
        check(vk.CreateImageView(vk.device, &ci, nullptr, &im.view), "create Vulkan image view");
    }
    Image &privateImage(std::map<uint64_t, Image> &map, uint64_t key, uint32_t width, uint32_t height,
                        uint32_t layers, VkFormat format, VkImageUsageFlags usage) {
        auto &im = map[key];
        if (im.handle &&
            (im.width != width || im.height != height || im.layers != layers || im.format != format)) {
            drain();
            destroy(im);
        }
        if (!im.handle)
            im = makeImage(width, height, layers, format, usage);
        return im;
    }
    static void barrier(VulkanContext &vk, VkCommandBuffer command, VkImage image, VkFormat format,
                        VkImageLayout before, VkImageLayout after, uint32_t firstLayer, uint32_t layers,
                        VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = before;
        b.newLayout = after;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {vulkanAspects(format), 0, 1, firstLayer, layers};
        vk.CmdPipelineBarrier(
            command, before == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : srcStage,
            dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    void transition(Slot &s, Image &im, VkImageLayout after, VkAccessFlags destination) {
        barrier(vk, s.command, im.handle, im.format, im.layout, after, 0, im.layers,
                VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, destination);
        im.layout = after;
    }
    void collect(Slot &s) {
        if (s.measured && queries) {
            uint64_t t[2]{};
            auto r = vk.GetQueryPoolResults(vk.device, queries, s.index * 2, 2, sizeof(t), t,
                                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (r == VK_SUCCESS) {
                uint64_t mask = vk.timestampBits == 64 ? UINT64_MAX : ((uint64_t(1) << vk.timestampBits) - 1);
                double duration = double((t[1] - t[0]) & mask) * vk.properties.limits.timestampPeriod / 1e6;
                (s.stage == 0 ? elapsed : s.stage == 1 ? copyElapsed : historyElapsed) = duration;
                if (s.stage == 0 && !s.view.empty())
                    viewTimings[s.view]["gpuPassMs"] = duration;
            } else if (r != VK_NOT_READY) {
                failed = true;
                check(r, "read Vulkan timestamps");
            }
        }
        if (s.metric) {
            auto values = static_cast<const uint32_t *>(s.counters.mapped);
            TemporalMetrics m;
            m.available = values[0] > 0;
            m.samples = values[0];
            m.reconstructedPercent = values[0] ? 100.0 * values[1] / values[0] : 0;
            m.fallbackPercent = 100 - m.reconstructedPercent;
            m.depthUsed = values[2] > 0;
            metrics[s.token] = m;
        }
        s.pending = s.measured = s.metric = false;
    }
    Slot &begin(uint32_t stage) {
        if (failed)
            throw std::runtime_error("Vulkan backend stopped after submission/device failure");
        for (size_t i = 0; i < slots.size(); ++i) {
            auto &s = slots[cursor++ % slots.size()];
            if (s.pending) {
                auto result = vk.GetFenceStatus(vk.device, s.fence);
                if (result == VK_NOT_READY)
                    continue;
                if (result != VK_SUCCESS)
                    failed = true;
                check(result, "query Vulkan submission fence");
                collect(s);
            }
            check(vk.ResetCommandBuffer(s.command, 0), "reset Vulkan command buffer");
            VkCommandBufferBeginInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            ci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vk.BeginCommandBuffer(s.command, &ci), "begin Vulkan command buffer");
            s.stage = stage;
            s.metric = false;
            s.measured = timingEnabled && queries;
            if (s.measured) {
                vk.CmdResetQueryPool(s.command, queries, s.index * 2, 2);
                vk.CmdWriteTimestamp(s.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, s.index * 2);
            }
            return s;
        }
        ++busyFallbacks;
        throw std::runtime_error("Vulkan command ring busy: nonblocking passthrough");
    }
    void submit(Slot &s) {
        if (s.measured)
            vk.CmdWriteTimestamp(s.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, s.index * 2 + 1);
        check(vk.EndCommandBuffer(s.command), "end Vulkan command buffer");
        check(vk.ResetFences(vk.device, 1, &s.fence), "reset Vulkan submission fence");
        VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &s.command;
        auto start = std::chrono::steady_clock::now();
        auto result = vk.QueueSubmit(vk.queue, 1, &submission, s.fence);
        cpuSubmitMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (result != VK_SUCCESS) {
            failed = true;
            check(result, "Vulkan reconstruction queue submit");
        }
        s.pending = true;
        ++submissions;
    }
    void drain() {
        for (auto &s : slots)
            if (s.pending) {
                auto result = vk.WaitForFences(vk.device, 1, &s.fence, VK_TRUE, UINT64_MAX);
                if (result == VK_ERROR_DEVICE_LOST) {
                    failed = true;
                    s.pending = false;
                    continue;
                }
                check(result, "drain Vulkan optimizer fence");
                collect(s);
            }
    }
    void cleanup() noexcept {
        for (auto &[_, im] : images)
            destroy(im);
        for (auto &[_, im] : scratch)
            destroy(im);
        for (auto &[_, im] : directViews)
            destroy(im);
        for (auto &s : slots) {
            destroy(s.parameters);
            destroy(s.counters);
            if (s.fence)
                vk.DestroyFence(vk.device, s.fence, nullptr);
        }
        if (queries)
            vk.DestroyQueryPool(vk.device, queries, nullptr);
        if (pipeline)
            vk.DestroyPipeline(vk.device, pipeline, nullptr);
        if (pipelineLayout)
            vk.DestroyPipelineLayout(vk.device, pipelineLayout, nullptr);
        if (descriptorPool)
            vk.DestroyDescriptorPool(vk.device, descriptorPool, nullptr);
        if (descriptorLayout)
            vk.DestroyDescriptorSetLayout(vk.device, descriptorLayout, nullptr);
        if (colorSampler)
            vk.DestroySampler(vk.device, colorSampler, nullptr);
        if (depthSampler)
            vk.DestroySampler(vk.device, depthSampler, nullptr);
        if (commandPool)
            vk.DestroyCommandPool(vk.device, commandPool, nullptr);
    }
    void initialize() {
        const auto &limits = vk.properties.limits;
        if (limits.maxPerStageDescriptorSamplers < 17 || limits.maxPerStageDescriptorSampledImages < 17 ||
            limits.maxDescriptorSetSamplers < 17)
            throw std::runtime_error("Vulkan device cannot bind the bounded temporal descriptor set");
        if (!vk.formatSupports(VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
            throw std::runtime_error("Vulkan RGBA8 storage/transfer intermediate unsupported");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = vk.family;
        check(vk.CreateCommandPool(vk.device, &pool, nullptr, &commandPool), "create Vulkan command pool");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0;
        check(vk.CreateSampler(vk.device, &sampler, nullptr, &colorSampler), "create Vulkan color sampler");
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        check(vk.CreateSampler(vk.device, &sampler, nullptr, &depthSampler), "create Vulkan depth sampler");
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{
            {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
             {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
             {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
             {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
             {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
             {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = uint32_t(bindings.size());
        layout.pBindings = bindings.data();
        check(vk.CreateDescriptorSetLayout(vk.device, &layout, nullptr, &descriptorLayout),
              "create Vulkan descriptor layout");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &descriptorLayout;
        check(vk.CreatePipelineLayout(vk.device, &pl, nullptr, &pipelineLayout),
              "create Vulkan pipeline layout");
        VkShaderModule module{};
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = sizeof(vulkanShader);
        shader.pCode = vulkanShader;
        check(vk.CreateShaderModule(vk.device, &shader, nullptr, &module), "load embedded Vulkan SPIR-V");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = module;
        compute.stage.pName = "main";
        compute.layout = pipelineLayout;
        auto result = vk.CreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline);
        vk.DestroyShaderModule(vk.device, module, nullptr);
        check(result, "create Vulkan reconstruction pipeline");
        std::array<VkDescriptorPoolSize, 4> sizes{
            {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 17 * slotCount},
             {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, slotCount},
             {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, slotCount},
             {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slotCount}}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = slotCount;
        dp.poolSizeCount = uint32_t(sizes.size());
        dp.pPoolSizes = sizes.data();
        check(vk.CreateDescriptorPool(vk.device, &dp, nullptr, &descriptorPool),
              "create Vulkan descriptor pool");
        if (vk.timestampBits) {
            VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            q.queryType = VK_QUERY_TYPE_TIMESTAMP;
            q.queryCount = slotCount * 2;
            check(vk.CreateQueryPool(vk.device, &q, nullptr, &queries), "create Vulkan timestamp pool");
        }
        for (uint32_t i = 0; i < slotCount; ++i) {
            auto &s = slots[i];
            s.index = i;
            VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ca.commandPool = commandPool;
            ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ca.commandBufferCount = 1;
            check(vk.AllocateCommandBuffers(vk.device, &ca, &s.command), "allocate Vulkan command buffer");
            VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            f.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vk.CreateFence(vk.device, &f, nullptr, &s.fence), "create Vulkan submission fence");
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            da.descriptorPool = descriptorPool;
            da.descriptorSetCount = 1;
            da.pSetLayouts = &descriptorLayout;
            check(vk.AllocateDescriptorSets(vk.device, &da, &s.descriptors),
                  "allocate Vulkan descriptor set");
            s.parameters = makeBuffer(sizeof(VulkanParameters), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            s.counters = makeBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
    }

  public:
    explicit VulkanEdgeRenderer(const XrGraphicsBindingVulkanKHR &b) : vk(b) {
        try {
            initialize();
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~VulkanEdgeRenderer() override {
        try {
            drain();
        } catch (...) {
            // With no completion proof, retain resources for device teardown
            // rather than destroying objects that the GPU may still reference.
            return;
        }
        cleanup();
    }
    bool supportsVulkan(const XrSwapchainCreateInfo &ci, std::string &reason) const override {
        if (!vulkanColorFormat(ci.format) && !vulkanDepthFormat(ci.format))
            reason = "Vulkan swapchain format unsupported";
        else if (ci.sampleCount != 1)
            reason = "Vulkan multisampled images require unsupported resolve path";
        else if (ci.mipCount != 1 || ci.faceCount != 1 || ci.createFlags || ci.next)
            reason = "Vulkan swapchain flags, mip/face count or extension chain unsupported";
        else if (ci.width == 0 || ci.height == 0 || ci.arraySize == 0 ||
                 ci.width > vk.properties.limits.maxImageDimension2D ||
                 ci.height > vk.properties.limits.maxImageDimension2D ||
                 ci.arraySize > vk.properties.limits.maxImageArrayLayers)
            reason = "Vulkan image extent outside device limits";
        else if (!vk.formatSupports(VkFormat(ci.format),
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                        (vulkanColorFormat(ci.format)
                                             ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                                             : 0)))
            reason = "Vulkan format lacks sampled/transfer/filter capabilities";
        else {
            reason.clear();
            return true;
        }
        return false;
    }
    void captureVulkan(uint64_t key, const VulkanImage &source) override {
        std::string reason;
        if (!source.image || !supportsVulkan(source.info, reason))
            throw std::runtime_error(reason.empty() ? "Null Vulkan source image" : reason);
        if (!(source.info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT))
            throw std::runtime_error("Vulkan source lacks transfer-source usage");
        auto format = VkFormat(source.info.format);
        auto required = vulkanDepthFormat(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (source.layout != required)
            throw std::runtime_error("Vulkan source layout is not explicitly verified attachment layout");
        auto &im = privateImage(
            images, key, source.info.width, source.info.height, source.info.arraySize, format,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto &s = begin(1);
        barrier(vk, s.command, source.image, format, source.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                source.info.arraySize, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        transition(s, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = {VkImageAspectFlags(vulkanDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                            : VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 0, source.info.arraySize};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {source.info.width, source.info.height, 1};
        vk.CmdCopyImage(s.command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.handle,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(vk, s.command, source.image, format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout, 0,
                source.info.arraySize, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        transition(s, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
        submit(s);
    }
    void renderVulkan(uint64_t key, const VulkanImage &output, const ShaderConstants &c,
                      const XrRect2Di &dest) override {
        auto cpuStart = std::chrono::steady_clock::now();
        auto &source = images.at(key);
        if (!output.image || !vulkanColorFormat(output.info.format) || dest.offset.x < 0 ||
            dest.offset.y < 0 || dest.extent.width <= 0 || dest.extent.height <= 0 ||
            uint64_t(dest.offset.x) + dest.extent.width > output.info.width ||
            uint64_t(dest.offset.y) + dest.extent.height > output.info.height ||
            c.slice >= output.info.arraySize)
            throw std::runtime_error("Vulkan output subimage invalid");
        auto format = VkFormat(output.info.format);
        bool direct = format == VK_FORMAT_R8G8B8A8_UNORM &&
                      (output.info.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) &&
                      vk.formatSupports(format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        if (!direct && (!(output.info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) ||
                        !vk.formatSupports(format, VK_FORMAT_FEATURE_TRANSFER_DST_BIT)))
            throw std::runtime_error("Vulkan output lacks legal storage or transfer-destination path");
        auto handleKey = reinterpret_cast<uint64_t>(output.image);
        Image *target;
        if (direct) {
            auto &im = directViews[{handleKey, c.slice}];
            if (!im.view) {
                im.owned = false;
                im.handle = output.image;
                im.format = format;
                im.layers = 1;
                createView(im, VK_IMAGE_ASPECT_COLOR_BIT, c.slice);
            }
            target = &im;
        } else
            target = &privateImage(scratch, handleKey, output.info.width, output.info.height, 1,
                                   VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto &s = begin(0);
        s.view = std::to_string(key) + ":" + std::to_string(c.slice) + ":" + std::to_string(dest.offset.x) +
                 ":" + std::to_string(dest.offset.y);
        auto &viewTiming = viewTimings[s.view];
        if (!viewTiming.contains("gpuPassMs"))
            viewTiming["gpuPassMs"] = nullptr;
        viewTiming["arrayLayer"] = c.slice;
        viewTiming["outputRect"] = {dest.offset.x, dest.offset.y, dest.extent.width, dest.extent.height};
        if (direct)
            barrier(vk, s.command, output.image, format, output.layout, VK_IMAGE_LAYOUT_GENERAL, c.slice, 1,
                    VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        else
            transition(s, *target, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT);
        VulkanParameters parameters{};
        parameters.image = c;
        parameters.temporal = temporal.gpu;
        parameters.destination[0] = dest.offset.x;
        parameters.destination[1] = dest.offset.y;
        parameters.destination[2] = dest.extent.width;
        parameters.destination[3] = dest.extent.height;
        parameters.outputFlags[0] = 0;
        parameters.outputFlags[1] = vulkanSrgb(format);
        parameters.outputFlags[2] = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
        std::memcpy(s.parameters.mapped, &parameters, sizeof(parameters));
        std::memset(s.counters.mapped, 0, 16);
        std::array<VkDescriptorImageInfo, 18> info{};
        info[0] = {colorSampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        info[1] = {VK_NULL_HANDLE, target->view, VK_IMAGE_LAYOUT_GENERAL};
        for (size_t i = 0; i < 16; ++i) {
            uint64_t historyKey = i < 8 ? temporal.colors[i] : temporal.depths[i - 8];
            auto found = c.method == 5 ? images.find(historyKey) : images.end();
            info[i + 2] = {i < 8 ? colorSampler : depthSampler,
                           found == images.end() ? source.view : found->second.view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        VkDescriptorBufferInfo uniform{s.parameters.handle, 0, sizeof(parameters)},
            counter{s.counters.handle, 0, 16};
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (uint32_t i = 0; i < 6; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = s.descriptors;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &info[0];
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &info[1];
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &uniform;
        writes[3].descriptorType = writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].descriptorCount = writes[4].descriptorCount = 8;
        writes[3].pImageInfo = &info[2];
        writes[4].pImageInfo = &info[10];
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5].pBufferInfo = &counter;
        vk.UpdateDescriptorSets(vk.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = s.counters.handle;
        host.size = 16;
        vk.CmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                              0, nullptr, 1, &host, 0, nullptr);
        vk.CmdBindPipeline(s.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vk.CmdBindDescriptorSets(s.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                 &s.descriptors, 0, nullptr);
        vk.CmdDispatch(s.command, (dest.extent.width + 7) / 8, (dest.extent.height + 7) / 8, 1);
        host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vk.CmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                              0, nullptr, 1, &host, 0, nullptr);
        if (!direct) {
            transition(s, *target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(vk, s.command, output.image, format, output.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    c.slice, 1, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c.slice, 1};
            copy.srcOffset = copy.dstOffset = {dest.offset.x, dest.offset.y, 0};
            copy.extent = {uint32_t(dest.extent.width), uint32_t(dest.extent.height), 1};
            vk.CmdCopyImage(s.command, target->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            ++intermediatePasses;
        } else
            ++directPasses;
        barrier(vk, s.command, output.image, format,
                direct ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, c.slice, 1,
                direct ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        s.metric = c.method == 5;
        s.token = temporal.token;
        submit(s);
        viewTiming["cpuRecordAndSubmitMs"] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
    }
    void storeHistory(uint64_t source, uint64_t destination, const XrSwapchainSubImage &region) override {
        auto &from = images.at(source);
        auto r = region.imageRect;
        if (region.imageArrayIndex >= from.layers || r.offset.x < 0 || r.offset.y < 0 ||
            r.extent.width <= 0 || r.extent.height <= 0 ||
            uint64_t(r.offset.x) + r.extent.width > from.width ||
            uint64_t(r.offset.y) + r.extent.height > from.height)
            throw std::runtime_error("Vulkan history subimage invalid");
        auto &to = privateImage(images, destination, r.extent.width, r.extent.height, 1, from.format,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto &s = begin(2);
        transition(s, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
        transition(s, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copy{};
        VkImageAspectFlags aspect =
            vulkanDepthFormat(from.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource = {aspect, 0, region.imageArrayIndex, 1};
        copy.dstSubresource = {aspect, 0, 0, 1};
        copy.srcOffset = {r.offset.x, r.offset.y, 0};
        copy.extent = {uint32_t(r.extent.width), uint32_t(r.extent.height), 1};
        vk.CmdCopyImage(s.command, from.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.handle,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(s, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
        transition(s, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
        submit(s);
    }
    void capture(uint64_t, IUnknown *, DXGI_FORMAT) override {
        throw std::runtime_error("Vulkan backend requires native Vulkan image metadata");
    }
    void render(uint64_t, IUnknown *, DXGI_FORMAT, const ShaderConstants &, const XrRect2Di &) override {
        throw std::runtime_error("Vulkan backend requires native Vulkan output metadata");
    }
    void forget(uint64_t key) override {
        drain();
        auto found = images.find(key);
        if (found != images.end()) {
            destroy(found->second);
            images.erase(found);
        }
        metrics.clear();
        viewTimings.clear();
    }
    void forgetVulkanOutput(VkImage output) override {
        drain();
        auto key = reinterpret_cast<uint64_t>(output);
        auto found = scratch.find(key);
        if (found != scratch.end()) {
            destroy(found->second);
            scratch.erase(found);
        }
        for (auto it = directViews.begin(); it != directViews.end();) {
            if (it->first.first == key) {
                destroy(it->second);
                it = directViews.erase(it);
            } else
                ++it;
        }
    }
    void setTemporal(const TemporalInput &value) override {
        temporal = value;
    }
    TemporalMetrics temporalMetrics() const override {
        auto found = metrics.find(temporal.token);
        return found == metrics.end() ? TemporalMetrics{} : found->second;
    }
    void setTiming(bool enabled) override {
        timingEnabled = enabled;
        if (!enabled) {
            elapsed.reset();
            copyElapsed.reset();
            historyElapsed.reset();
        }
    }
    std::optional<double> gpuMilliseconds() const override {
        return elapsed;
    }
    std::optional<double> sourceCopyMilliseconds() const override {
        return copyElapsed;
    }
    std::optional<double> historyCopyMilliseconds() const override {
        return historyElapsed;
    }
    bool hasFailed() const override {
        return failed;
    }
    Json vulkanDiagnostics() const override {
        Json views = Json::array();
        for (auto &[key, value] : viewTimings) {
            auto item = value;
            item["viewKey"] = key;
            views.push_back(std::move(item));
        }
        return {{"device", vk.properties.deviceName},
                {"apiVersion", vk.properties.apiVersion},
                {"queueFamily", vk.family},
                {"queueIndex", vk.queueIndex},
                {"synchronization", "explicit cooperative image declarations; bound queue barriers"},
                {"synchronization2Required", false},
                {"timelineSemaphoresRequired", false},
                {"descriptorIndexingRequired", false},
                {"multiviewShaderRequired", false},
                {"timestampValidBits", vk.timestampBits},
                {"allocatedBytes", allocatedBytes},
                {"peakAllocatedBytes", peakBytes},
                {"allocationCount", allocations},
                {"submissionCount", submissions},
                {"directPasses", directPasses},
                {"intermediatePasses", intermediatePasses},
                {"busyFallbacks", busyFallbacks},
                {"cpuSubmissionMs", cpuSubmitMs},
                {"perViewProcessing", views},
                {"gpuPassMs", elapsed ? Json(*elapsed) : Json(nullptr)},
                {"gpuFrameMs", nullptr},
                {"sourceCopyGpuMs", copyElapsed ? Json(*copyElapsed) : Json(nullptr)},
                {"historyCopyGpuMs", historyElapsed ? Json(*historyElapsed) : Json(nullptr)},
                {"failed", failed}};
    }
};
std::unique_ptr<IEdgeRenderer> makeVulkan(const XrGraphicsBindingVulkanKHR &binding) {
    return std::make_unique<VulkanEdgeRenderer>(binding);
}
} // namespace fov
