#include "VulkanTestDevice.h"
#include "renderer/IEdgeRenderer.h"
#include <bit>
#include <chrono>
using namespace fov;
using vkTest::require;
static XrSwapchainCreateInfo info(vkTest::Image &im, bool storage = false) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.width = im.width;
    ci.height = im.height;
    ci.arraySize = im.layers;
    ci.format = im.format;
    ci.sampleCount = ci.faceCount = ci.mipCount = 1;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                    (vulkanDepthFormat(im.format) ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                  : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    if (storage)
        ci.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
    return ci;
}
static ShaderConstants shader(vkTest::Image &source, Profile p, uint32_t eye) {
    XrFovf original{-std::atan(1.f), std::atan(1.f), std::atan(1.f), -std::atan(1.f)};
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.fov = reduce(original, .8f, .8f);
    view.subImage = {XR_NULL_HANDLE, {{0, 0}, {int(source.width), int(source.height)}}, eye};
    auto c = constants(view, original, info(source), p, vulkanSrgb(source.format));
    return c;
}
static TemporalFrame frame(XrTime time) {
    TemporalFrame f;
    f.time = time;
    f.pose.orientation.w = 1;
    f.original = {-std::atan(1.f), std::atan(1.f), std::atan(1.f), -std::atan(1.f)};
    f.reduced = reduce(f.original, .8f, .8f);
    f.width = f.height = f.depthWidth = f.depthHeight = 64;
    f.trackingValid = true;
    return f;
}
static void algorithms(vkTest::Device &device, IEdgeRenderer &renderer, VkFormat format, bool storage) {
    auto &source = device.image(64, 64, 2, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    auto &output =
        device.image(128, 128, 2, format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0));
    std::vector<uint32_t> pixels(64 * 64 * 2);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = i < 64 * 64 ? 0xff204080 : 0xff804020;
    device.upload(source, pixels);
    renderer.captureVulkan(1, {source.handle, info(source), source.layout});
    TemporalHistory history;
    TemporalSettings settings;
    auto current = frame(1000000000);
    history.prepare(current, settings);
    renderer.setTemporal(history.input(current, settings, 128, 128));
    for (int method = 0; method < 6; ++method) {
        Profile p;
        p.method = method;
        p.color = {.5f, .25f, .75f};
        for (uint32_t eye : {1u, 0u})
            renderer.renderVulkan(1, {output.handle, info(output, storage), output.layout},
                                  shader(source, p, eye), {{0, 0}, {128, 128}});
        output.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        auto result = device.read(output);
        require(result[64 * 128 + 64] == pixels[32 * 64 + 32] &&
                    result[128 * 128 + 64 * 128 + 64] == pixels[64 * 64 + 32 * 64 + 32],
                "Vulkan central content and nonzero eye slice preserved");
        if (method == 4) {
            auto corner = result[0];
            require(std::abs(int(corner & 255) -
                             (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB
                                  ? 191
                                  : 128)) <= 1 &&
                        std::abs(int((corner >> 8) & 255) - 64) <= 1 &&
                        std::abs(int((corner >> 16) & 255) -
                                 (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB
                                      ? 128
                                      : 191)) <= 1,
                    "Vulkan solid color and sRGB display encoding");
        } else
            require(result[0] == pixels[0] && result[128 * 128] == pixels[64 * 64],
                    "Vulkan edges preserve independent eye colors");
    }
    renderer.forget(1);
    renderer.forgetVulkanOutput(output.handle);
    device.release(source);
    device.release(output);
}
static void temporal(vkTest::Device &device, IEdgeRenderer &renderer, uint32_t eye, int mode) {
    auto &source = device.image(64, 64, 2, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    auto &output = device.image(128, 128, 2, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    // Each case renders one eye, but readback examines both array layers.
    // Initialize the untouched layer instead of claiming it has been rendered.
    device.upload(output, std::vector<uint32_t>(128 * 128 * 2, 0xff000000));
    auto depthFormat = mode == 7   ? VK_FORMAT_D16_UNORM
                       : mode == 8 ? VK_FORMAT_D24_UNORM_S8_UINT
                       : mode == 9 ? VK_FORMAT_D32_SFLOAT_S8_UINT
                                   : VK_FORMAT_D32_SFLOAT;
    auto &depth = device.image(64, 64, 2, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    TemporalHistory history;
    TemporalSettings settings;
    auto old = frame(1000000000), current = frame(1005000000);
    if (mode == 1)
        old.pose.orientation = {0, std::sin(-.2f / 2), 0, std::cos(-.2f / 2)};
    if ((mode >= 2 && mode <= 5) || mode >= 7) {
        old.pose.position.x = .2f;
        old.hasDepth = mode >= 3;
        current.hasDepth = old.hasDepth;
        if (mode == 4)
            old.depth = {.2f, .9f, 100, .1f};
        current.depth = old.depth;
    }
    if (mode == 6)
        current.trackingValid = false;
    history.prepare(old, settings);
    auto &entry = history.writable();
    XrSwapchainSubImage region{XR_NULL_HANDLE, {{0, 0}, {64, 64}}, eye};
    std::vector<uint32_t> colors(64 * 64 * 2, 0xff1432c8);
    std::fill(colors.begin() + 64 * 64, colors.end(), 0xffc83214);
    device.upload(source, colors);
    renderer.captureVulkan(1, {source.handle, info(source), source.layout});
    renderer.storeHistory(1, entry.colorKey, region);
    if (old.hasDepth) {
        float t = (1.f - 1 / old.depth.nearZ) / (1 / old.depth.farZ - 1 / old.depth.nearZ);
        float sample = mode == 5 ? old.depth.maxDepth : std::lerp(old.depth.minDepth, old.depth.maxDepth, t);
        uint32_t encoded = depthFormat == VK_FORMAT_D16_UNORM ? uint32_t(std::round(sample * 65535))
                           : depthFormat == VK_FORMAT_D24_UNORM_S8_UINT
                               ? uint32_t(std::round(sample * 16777215))
                               : std::bit_cast<uint32_t>(sample);
        std::vector<uint32_t> values(64 * 64 * 2, encoded);
        device.upload(depth, values);
        renderer.captureVulkan(2, {depth.handle, info(depth), depth.layout});
        renderer.storeHistory(2, entry.depthKey, region);
    }
    history.commit(old);
    history.prepare(current, settings);
    std::fill(colors.begin(), colors.end(), 0xff408040);
    device.upload(source, colors);
    renderer.captureVulkan(1, {source.handle, info(source), source.layout});
    renderer.setTemporal(history.input(current, settings, 128, 128));
    Profile p;
    p.method = 5;
    std::vector<uint32_t> result;
    for (int repeat = 0; repeat < 36; repeat++) {
        renderer.renderVulkan(1, {output.handle, info(output), output.layout}, shader(source, p, eye),
                              {{0, 0}, {128, 128}});
        output.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        result = device.read(output);
    }
    size_t offset = size_t(eye) * 128 * 128;
    require(result[offset + 64 * 128 + 64] == 0xff408040, "Vulkan temporal central pixels preserved");
    auto edge = result[offset + 64 * 128 + 120];
    bool reconstruct = mode == 1 || mode == 3 || mode == 4 || mode >= 7;
    if (reconstruct) {
        int channel = eye ? 16 : 0;
        require(((edge >> channel) & 255) > 80, "Vulkan rotation/depth covers newly exposed periphery");
        auto metrics = renderer.temporalMetrics();
        require(metrics.available && metrics.reconstructedPercent > 0 && metrics.fallbackPercent > 0,
                "Vulkan real delayed coverage counters");
        require(metrics.depthUsed == (mode >= 3), "Vulkan depth/rotation diagnostic mode");
    } else
        require(edge == 0xff408040, "Vulkan invalid or missing depth/tracking uses fallback");
    auto path = std::filesystem::current_path() / "vulkan-evidence";
    vkTest::ppm(path / ("temporal-eye" + std::to_string(eye) + "-mode" + std::to_string(mode) + ".ppm"),
                result, 128, 128, eye);
    for (auto key : history.resourceKeys())
        renderer.forget(key);
    renderer.forget(1);
    renderer.forget(2);
    renderer.forgetVulkanOutput(output.handle);
    device.release(source);
    device.release(output);
    device.release(depth);
}
static void gridAndBenchmark(vkTest::Device &device, IEdgeRenderer &renderer) {
    auto &source = device.image(128, 128, 2, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    auto &output = device.image(160, 160, 2, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    std::vector<uint32_t> background(128 * 128 * 2);
    for (uint32_t eye = 0; eye < 2; eye++)
        for (uint32_t y = 0; y < 128; y++)
            for (uint32_t x = 0; x < 128; x++)
                background[eye * 128 * 128 + y * 128 + x] =
                    (x % 16 < 2 || y % 16 < 2) ? 0xffeeeeee : (eye ? 0xffc03010 : 0xff1030c0);
    uint64_t warmAllocations = 0;
    Json modes = Json::array();
    const char *names[] = {"clamp", "gradient", "blur", "mirror", "solid", "temporal-no-history-fallback"};
    for (int mode = 0; mode < 6; mode++) {
        std::vector<double> wallTimes;
        for (int pass = 0; pass < 80; pass++) {
            auto pixels = background;
            uint32_t shift = uint32_t(pass % 16);
            for (uint32_t eye = 0; eye < 2; eye++)
                for (uint32_t y = 50; y < 70; y++)
                    for (uint32_t x = 40 + shift; x < 60 + shift; x++)
                        pixels[eye * 128 * 128 + y * 128 + x] = 0xff10e0e0;
            device.upload(source, pixels);
            auto start = std::chrono::steady_clock::now();
            renderer.captureVulkan(1, {source.handle, info(source), source.layout});
            Profile p;
            p.method = mode;
            if (mode == 5) {
                TemporalHistory h;
                auto current = frame(1000000000);
                current.width = current.height = 128;
                h.prepare(current, p.temporal);
                renderer.setTemporal(h.input(current, p.temporal, 160, 160));
            }
            for (uint32_t eye : {1u, 0u})
                renderer.renderVulkan(1, {output.handle, info(output), output.layout}, shader(source, p, eye),
                                      {{0, 0}, {160, 160}});
            output.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            auto result = device.read(output);
            if (pass == 0)
                for (uint32_t eye = 0; eye < 2; eye++)
                    vkTest::ppm(
                        std::filesystem::current_path() / "vulkan-evidence" /
                            ("grid-mode" + std::to_string(mode) + "-eye" + std::to_string(eye) + ".ppm"),
                        result, 160, 160, eye);
            if (pass >= 16)
                wallTimes.push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                        .count());
            if (mode == 0 && pass == 16)
                warmAllocations = renderer.vulkanDiagnostics()["allocationCount"].get<uint64_t>();
        }
        auto measured = renderer.vulkanDiagnostics();
        require(measured["allocationCount"] == warmAllocations,
                "Vulkan resources reused without steady-state allocations");
        double wall = 0;
        for (auto value : wallTimes)
            wall += value;
        wall /= wallTimes.size();
        modes.push_back({{"mode", names[mode]},
                         {"gpuPassMs", measured["gpuPassMs"]},
                         {"sourceCopyGpuMs", measured["sourceCopyGpuMs"]},
                         {"cpuSubmissionMs", measured["cpuSubmissionMs"]},
                         {"perViewProcessing", measured["perViewProcessing"]},
                         {"fixtureFrameWallMsIncludingReadback", wall},
                         {"measuredFrames", wallTimes.size()},
                         {"allocatedBytes", measured["allocatedBytes"]},
                         {"steadyStateAllocations", 0}});
    }
    auto diagnostics = renderer.vulkanDiagnostics();
    require(renderer.gpuMilliseconds().has_value() && renderer.sourceCopyMilliseconds().has_value(),
            "Vulkan timestamp queries complete");
    require(diagnostics["busyFallbacks"] == 0, "Controlled workload does not exhaust command ring");
    diagnostics["fixtureOutputDimensions"] = {160, 160};
    diagnostics["fixtureSourceDimensions"] = {128, 128};
    diagnostics["modes"] = modes;
    diagnostics["validationEnabled"] = device.validation;
    diagnostics["thirdPartyRuntimeTested"] = false;
    diagnostics["timingSemantics"] =
        "last completed per-operation timestamps; fixture wall time includes validation readback";
    updateJson(std::filesystem::current_path() / "vulkan-evidence/benchmark.json",
               [&](Json &j) { j = diagnostics; });
    std::cout << diagnostics.dump() << '\n';
    renderer.forget(1);
    renderer.forgetVulkanOutput(output.handle);
    device.release(source);
    device.release(output);
}
int main() {
    try {
        auto p = parseProfile({{"vulkan", {{"enabled", false}, {"processing", "passthrough"}}}});
        require(!p.vulkanEnabled && p.vulkanProcessing == "passthrough", "Vulkan profile policy");
        require(parseProfile(profileJson(p)).vulkanProcessing == p.vulkanProcessing,
                "Vulkan profile persistence");
        require(
            parseProfile({{"vulkan", {{"enabled", "invalid"}, {"processing", "unsafe"}}}}).vulkanProcessing ==
                "auto",
            "Malformed Vulkan policy defaults");
        vkTest::Device device;
        bool rejected = false;
        try {
            auto b = device.binding;
            b.device = VK_NULL_HANDLE;
            VulkanContext invalid(b);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Reject null device");
        rejected = false;
        try {
            auto b = device.binding;
            b.queueFamilyIndex = UINT32_MAX;
            VulkanContext invalid(b);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Reject invalid queue family");
        auto renderer = makeVulkan(device.binding);
        renderer->setTiming(true);
        for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
                            VK_FORMAT_B8G8R8A8_SRGB})
            algorithms(device, *renderer, format, false);
        algorithms(device, *renderer, VK_FORMAT_R8G8B8A8_UNORM, true);
        for (uint32_t eye = 0; eye < 2; eye++)
            for (int mode = 0; mode < 10; mode++)
                temporal(device, *renderer, eye, mode);
        gridAndBenchmark(device, *renderer);
        renderer.reset();
        device.shutdown();
        device.validationCheck();
        std::cout << "PASS: Vulkan GPU modes, stereo arrays, direct/intermediate, sRGB, "
                     "rotation/depth/fallback, timestamps and cleanup\n";
        return 0;
    } catch (const vkTest::Unavailable &e) {
        std::cout << (vkTest::validationRequired() ? "FAIL: " : "SKIP: ") << e.what() << '\n';
        return vkTest::validationRequired() ? 1 : 77;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
