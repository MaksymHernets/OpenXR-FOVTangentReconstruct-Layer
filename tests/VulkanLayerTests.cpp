#include "VulkanTestDevice.h"
#include "layer/VulkanCooperation.h"
#include "common/ConfigManager.h"
#include "common/FovMath.h"
#include <map>
#include <thread>
#include <chrono>
#include <bit>
using namespace fov;
using vkTest::require;
namespace runtime {
struct Chain {
    XrSwapchainCreateInfo info{};
    std::vector<vkTest::Image *> images;
    bool acquired = false, waited = false;
    uint32_t index = 0;
};
std::map<XrSwapchain, Chain> chains;
std::unique_ptr<vkTest::Device> device;
uintptr_t next = 100;
bool timeout = false, expectOutput = false, temporal = false, checkPixels = true;
XrCompositionLayerProjectionView expected[2]{};
uint32_t wantedColor[2]{};
int calls = 0, spacesCreated = 0, spacesDestroyed = 0;
std::string evidenceName;
Json journal = Json::array();
XrFovf original[2] = {{-.9f, .65f, .8f, -.7f}, {-.65f, .9f, .8f, -.7f}};
XrResult XRAPI_CALL createInstance(const XrInstanceCreateInfo *, const XrApiLayerCreateInfo *layer,
                                   XrInstance *out) {
    require(!layer->nextInfo, "advance layer chain");
    *out = reinterpret_cast<XrInstance>(++next);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroyInstance(XrInstance) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createSession(XrInstance, const XrSessionCreateInfo *, XrSession *out) {
    *out = reinterpret_cast<XrSession>(++next);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroySession(XrSession) {
    // OpenXR session teardown implicitly destroys remaining application chains.
    for (auto &[_, chain] : chains)
        for (auto image : chain.images)
            device->release(*image);
    chains.clear();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL beginSession(XrSession, const XrSessionBeginInfo *) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL endSession(XrSession) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL pollEvent(XrInstance, XrEventDataBuffer *) {
    return XR_EVENT_UNAVAILABLE;
}
XrResult XRAPI_CALL createSpace(XrSession, const XrReferenceSpaceCreateInfo *, XrSpace *space) {
    *space = reinterpret_cast<XrSpace>(++next);
    ++spacesCreated;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroySpace(XrSpace) {
    ++spacesDestroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation *location) {
    location->pose = {{0, 0, 0, 1}, {0, 0, 0}};
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerateViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity,
                                   uint32_t *count, XrViewConfigurationView *views) {
    *count = 2;
    if (capacity)
        for (int eye = 0; eye < 2; eye++) {
            views[eye].recommendedImageRectWidth = 1000;
            views[eye].recommendedImageRectHeight = 800;
        }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locate(XrSession, const XrViewLocateInfo *, XrViewState *state, uint32_t capacity,
                           uint32_t *count, XrView *views) {
    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (capacity)
        for (int eye = 0; eye < 2; eye++) {
            views[eye].fov = original[eye];
            views[eye].pose = {{0, 0, 0, 1}, {eye ? .032f : -.032f, 0, 0}};
        }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createSwapchain(XrSession, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
    // Exercise rejection of optional storage and fallback to a transfer-only output.
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
        return XR_ERROR_FEATURE_UNSUPPORTED;
    Chain chain;
    chain.info = *ci;
    bool depth = vulkanDepthFormat(ci->format);
    VkImageUsageFlags usage =
        depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    for (int i = 0; i < 3; i++) {
        auto &image = device->image(ci->width, ci->height, ci->arraySize, VkFormat(ci->format), usage);
        device->begin();
        device->transition(image, depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        device->execute();
        chain.images.push_back(&image);
    }
    *out = reinterpret_cast<XrSwapchain>(++next);
    chains.emplace(*out, std::move(chain));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroySwapchain(XrSwapchain handle) {
    auto &chain = chains.at(handle);
    for (auto im : chain.images)
        device->release(*im);
    chains.erase(handle);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerateImages(XrSwapchain handle, uint32_t capacity, uint32_t *count,
                                    XrSwapchainImageBaseHeader *images) {
    auto &chain = chains.at(handle);
    *count = uint32_t(chain.images.size());
    if (capacity < *count)
        return capacity ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
    auto result = reinterpret_cast<XrSwapchainImageVulkanKHR *>(images);
    for (uint32_t i = 0; i < *count; i++) {
        require(result[i].type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, "Vulkan image tag including alias");
        result[i].image = chain.images[i]->handle;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL acquire(XrSwapchain handle, const XrSwapchainImageAcquireInfo *, uint32_t *index) {
    auto &chain = chains.at(handle);
    require(!chain.acquired, "no double acquire");
    chain.acquired = true;
    chain.waited = false;
    chain.index = (chain.index + 1) % 3;
    *index = chain.index;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL wait(XrSwapchain handle, const XrSwapchainImageWaitInfo *info) {
    auto &chain = chains.at(handle);
    require(chain.acquired, "wait after acquire");
    if (timeout && info->timeout == 0) {
        timeout = false;
        return XR_TIMEOUT_EXPIRED;
    }
    chain.waited = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL release(XrSwapchain handle, const XrSwapchainImageReleaseInfo *) {
    auto &chain = chains.at(handle);
    require(chain.acquired && chain.waited, "release after successful wait");
    chain.acquired = chain.waited = false;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL end(XrSession, const XrFrameEndInfo *info) {
    ++calls;
    require(info->layerCount == 1, "projection forwarded");
    auto layer = reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0]);
    Json record = {{"frame", calls}, {"reconstructed", expectOutput}, {"views", Json::array()}};
    for (uint32_t eye = 0; eye < 2; eye++) {
        const auto &v = layer->views[eye];
        auto &chain = chains.at(v.subImage.swapchain);
        require(!chain.acquired, "runtime only consumes released output");
        require((v.subImage.swapchain != expected[eye].subImage.swapchain) == expectOutput,
                "cooperative replacement or unchanged passthrough");
        require(same(v.fov, expectOutput ? original[eye] : expected[eye].fov),
                "Vulkan submitted FOV matches the actual image");
        require(v.pose.position.x == expected[eye].pose.position.x &&
                    v.subImage.imageArrayIndex == expected[eye].subImage.imageArrayIndex,
                "pose and actual array slice preserved");
        if (temporal && expectOutput)
            require(!v.next, "narrow depth removed from widened output");
        record["views"].push_back(
            {{"eye", eye},
             {"arrayLayer", v.subImage.imageArrayIndex},
             {"rect",
              {v.subImage.imageRect.offset.x, v.subImage.imageRect.offset.y,
               v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height}},
             {"fov", {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown}}});
        if (expectOutput && checkPixels) {
            auto &im = *chain.images[chain.index];
            im.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            auto pixels = device->read(im);
            auto r = v.subImage.imageRect;
            size_t center = size_t(v.subImage.imageArrayIndex) * im.width * im.height +
                            size_t(r.offset.y + r.extent.height / 2) * im.width + r.offset.x +
                            r.extent.width / 2;
            require(pixels[center] == wantedColor[eye], "real runtime output has correct eye center content");
            if (!temporal) {
                size_t corner = size_t(v.subImage.imageArrayIndex) * im.width * im.height +
                                size_t(r.offset.y) * im.width + r.offset.x;
                require(pixels[corner] == 0xff000000, "solid peripheral output is black");
            }
            vkTest::ppm(std::filesystem::current_path() / "vulkan-evidence" /
                            ("openxr-" + evidenceName + "-" + std::to_string(calls) + "-eye" +
                             std::to_string(eye) + ".ppm"),
                        pixels, im.width, im.height, v.subImage.imageArrayIndex);
        }
    }
    journal.push_back(std::move(record));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL gipa(XrInstance, const char *name, PFN_xrVoidFunction *out) {
#define MAP(n, f)                                                                                            \
    if (!strcmp(name, "xr" #n)) {                                                                            \
        *out = reinterpret_cast<PFN_xrVoidFunction>(f);                                                      \
        return XR_SUCCESS;                                                                                   \
    }
    MAP(DestroyInstance, destroyInstance)
    MAP(CreateSession, createSession)
    MAP(DestroySession, destroySession)
    MAP(EnumerateViewConfigurationViews, enumerateViews)
    MAP(LocateViews, locate)
    MAP(CreateSwapchain, createSwapchain)
    MAP(DestroySwapchain, destroySwapchain) MAP(EnumerateSwapchainImages, enumerateImages)
        MAP(AcquireSwapchainImage, acquire) MAP(WaitSwapchainImage, wait) MAP(ReleaseSwapchainImage, release)
            MAP(EndFrame, end) MAP(BeginSession, beginSession) MAP(EndSession, endSession)
                MAP(PollEvent, pollEvent) MAP(CreateReferenceSpace, createSpace) MAP(LocateSpace, locateSpace)
                    MAP(DestroySpace, destroySpace)
#undef MAP
                        *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
} // namespace runtime
template <class T> T proc(PFN_xrGetInstanceProcAddr g, XrInstance instance, const char *name) {
    T result{};
    require(XR_SUCCEEDED(g(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(&result))), name);
    return result;
}
int wmain(int argc, wchar_t **argv) {
    try {
        require(argc >= 2, "layer DLL path required");
        bool separate = false, packed = false, vulkan2 = false, mixed = false;
        for (int i = 2; i < argc; i++) {
            separate |= !wcscmp(argv[i], L"--separate");
            packed |= !wcscmp(argv[i], L"--packed");
            vulkan2 |= !wcscmp(argv[i], L"--vulkan2");
            runtime::temporal |= !wcscmp(argv[i], L"--temporal");
            mixed |= !wcscmp(argv[i], L"--mixed");
        }
        runtime::evidenceName = (runtime::temporal ? "depth-" : "") + std::string(separate  ? "separate"
                                                                                  : packed  ? "packed"
                                                                                  : mixed   ? "mixed"
                                                                                  : vulkan2 ? "enable2"
                                                                                            : "array");
        static_assert(XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
        static_assert(sizeof(XrSwapchainImageVulkanKHR) == sizeof(XrSwapchainImageVulkan2KHR));
        auto dir = std::filesystem::current_path() / ("test-data-vulkan-" + runtime::evidenceName);
        SetEnvironmentVariableW(L"FOV_DATA_DIR", dir.c_str());
        Profile profile;
        profile.method = runtime::temporal ? 5 : 4;
        updateJson(dir / "config.json", [&](Json &j) { j = {{"defaults", profileJson(profile)}}; });
        runtime::device = std::make_unique<vkTest::Device>();
        HMODULE module = LoadLibraryW(argv[1]);
        require(module != nullptr, "load OpenXR layer");
        auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
            GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
        require(negotiate != nullptr, "negotiation export");
        XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
                                     XR_LOADER_INFO_STRUCT_VERSION,
                                     sizeof(XrNegotiateLoaderInfo),
                                     1,
                                     1,
                                     XR_MAKE_VERSION(1, 0, 0),
                                     XR_CURRENT_API_VERSION};
        XrNegotiateApiLayerRequest request{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST,
                                           XR_API_LAYER_INFO_STRUCT_VERSION,
                                           sizeof(XrNegotiateApiLayerRequest)};
        require(XR_SUCCEEDED(negotiate(&loader, "XR_APILAYER_FOV_extension", &request)),
                "negotiate Vulkan layer");
        XrApiLayerNextInfo next{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO,
                                XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(XrApiLayerNextInfo)};
        strcpy_s(next.layerName, "XR_APILAYER_FOV_extension");
        next.nextGetInstanceProcAddr = runtime::gipa;
        next.nextCreateApiLayerInstance = runtime::createInstance;
        XrApiLayerCreateInfo layer{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
                                   XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(XrApiLayerCreateInfo)};
        layer.nextInfo = &next;
        std::vector<const char *> extensions{vulkan2 ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
                                                     : XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
        if (runtime::temporal)
            extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
        if (mixed)
            extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        strcpy_s(ci.applicationInfo.applicationName, "Controlled Vulkan OpenXR fixture");
        ci.enabledExtensionCount = uint32_t(extensions.size());
        ci.enabledExtensionNames = extensions.data();
        XrInstance instance{};
        require(XR_SUCCEEDED(request.createApiLayerInstance(&ci, &layer, &instance)),
                "create Vulkan XR instance");
        auto g = request.getInstanceProcAddr;
        auto createSession = proc<PFN_xrCreateSession>(g, instance, "xrCreateSession");
        auto destroySession = proc<PFN_xrDestroySession>(g, instance, "xrDestroySession");
        auto createSwapchain = proc<PFN_xrCreateSwapchain>(g, instance, "xrCreateSwapchain");
        auto destroySwapchain = proc<PFN_xrDestroySwapchain>(g, instance, "xrDestroySwapchain");
        auto enumerate = proc<PFN_xrEnumerateSwapchainImages>(g, instance, "xrEnumerateSwapchainImages");
        auto acquire = proc<PFN_xrAcquireSwapchainImage>(g, instance, "xrAcquireSwapchainImage");
        auto wait = proc<PFN_xrWaitSwapchainImage>(g, instance, "xrWaitSwapchainImage");
        auto release = proc<PFN_xrReleaseSwapchainImage>(g, instance, "xrReleaseSwapchainImage");
        auto locate = proc<PFN_xrLocateViews>(g, instance, "xrLocateViews");
        auto end = proc<PFN_xrEndFrame>(g, instance, "xrEndFrame");
        auto enable = proc<PFN_xrFovEnableVulkanProcessing>(g, instance, "xrFovEnableVulkanProcessing");
        auto declare = proc<PFN_xrFovDeclareVulkanImageState>(g, instance, "xrFovDeclareVulkanImageState");
        for (int cycle = 0; cycle < 2; cycle++) {
            XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
            si.next = &runtime::device->binding;
            XrSession session{};
            require(XR_SUCCEEDED(createSession(instance, &si, &session)), "detect Vulkan session");
            uint32_t count = 0;
            XrViewConfigurationView dimensions[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                     {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
            auto sizes =
                proc<PFN_xrEnumerateViewConfigurationViews>(g, instance, "xrEnumerateViewConfigurationViews");
            sizes(instance, 1, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, dimensions);
            require(dimensions[0].recommendedImageRectWidth == 1000,
                    "unverified Vulkan keeps native dimensions");
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = 1000000000;
            XrViewState state{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            require(XR_SUCCEEDED(locate(session, &li, &state, 2, &count, views)) &&
                        same(views[0].fov, runtime::original[0]),
                    "unverified Vulkan keeps native FOV");
            // A third-party-style session with no cooperation must keep native FOV,
            // leave creation usage unchanged, and forward real image content intact.
            XrSwapchainCreateInfo nativeInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            nativeInfo.width = nativeInfo.height = 32;
            nativeInfo.arraySize = 2;
            nativeInfo.sampleCount = nativeInfo.faceCount = nativeInfo.mipCount = 1;
            nativeInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            nativeInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            XrSwapchain native{};
            require(XR_SUCCEEDED(createSwapchain(session, &nativeInfo, &native)),
                    "create diagnostic-only Vulkan chain");
            require(runtime::chains.at(native).info.usageFlags == nativeInfo.usageFlags,
                    "diagnostic-only swapchain usage unchanged");
            uint32_t nativeIndex = 0;
            XrSwapchainImageAcquireInfo nativeAcquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo nativeWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            nativeWait.timeout = XR_INFINITE_DURATION;
            XrSwapchainImageReleaseInfo nativeRelease{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            require(XR_SUCCEEDED(acquire(native, &nativeAcquire, &nativeIndex)) &&
                        XR_SUCCEEDED(wait(native, &nativeWait)),
                    "diagnostic-only acquisition");
            auto &nativeImage = *runtime::chains.at(native).images[nativeIndex];
            std::vector<uint32_t> nativePixels(32 * 32 * 2, 0xff123456);
            runtime::device->upload(nativeImage, nativePixels);
            require(XR_SUCCEEDED(release(native, &nativeRelease)), "diagnostic-only release");
            XrCompositionLayerProjectionView nativeViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                               {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            for (uint32_t eye = 0; eye < 2; eye++) {
                nativeViews[eye].pose = views[eye].pose;
                nativeViews[eye].fov = views[eye].fov;
                nativeViews[eye].subImage = {native, {{0, 0}, {32, 32}}, eye};
                runtime::expected[eye] = nativeViews[eye];
            }
            XrCompositionLayerProjection nativeProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            nativeProjection.viewCount = 2;
            nativeProjection.views = nativeViews;
            auto nativeBase = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&nativeProjection);
            XrFrameEndInfo nativeEnd{XR_TYPE_FRAME_END_INFO};
            nativeEnd.displayTime = li.displayTime;
            nativeEnd.layerCount = 1;
            nativeEnd.layers = &nativeBase;
            runtime::expectOutput = false;
            require(XR_SUCCEEDED(end(session, &nativeEnd)), "unmodified Vulkan passthrough frame");
            require(runtime::device->read(nativeImage) == nativePixels,
                    "diagnostic-only native image remains untouched");
            require(XR_SUCCEEDED(destroySwapchain(native)), "destroy diagnostic-only chain");
            require(enable(session, 999) == XR_ERROR_API_VERSION_UNSUPPORTED,
                    "cooperation API version validation");
            require(XR_SUCCEEDED(enable(session, FOV_VULKAN_COOPERATION_VERSION)),
                    "initialize cooperative Vulkan processing");
            sizes(instance, 1, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, dimensions);
            require(dimensions[0].recommendedImageRectWidth == 800, "cooperative Vulkan recommendations");
            XrSwapchain color[2]{}, depth[2]{};
            XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            sc.width = packed ? 64 : 32;
            sc.height = 32;
            sc.arraySize = separate || packed ? 1 : 2;
            sc.sampleCount = sc.faceCount = sc.mipCount = 1;
            sc.format = VK_FORMAT_R8G8B8A8_UNORM;
            sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            int chainCount = separate ? 2 : 1;
            for (int i = 0; i < chainCount; i++) {
                require(XR_SUCCEEDED(createSwapchain(session, &sc, &color[i])), "create Vulkan source chain");
                uint32_t imageCount = 0;
                require(XR_SUCCEEDED(enumerate(color[i], 0, &imageCount, nullptr)) && imageCount == 3,
                        "count runtime Vulkan images");
                std::vector<XrSwapchainImageVulkan2KHR> images(imageCount,
                                                               {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
                require(
                    XR_SUCCEEDED(enumerate(color[i], imageCount, &imageCount,
                                           reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()))),
                    "enumerate Vulkan2 alias images");
                if (runtime::temporal) {
                    auto ds = sc;
                    ds.format = VK_FORMAT_D32_SFLOAT;
                    ds.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                    require(XR_SUCCEEDED(createSwapchain(session, &ds, &depth[i])),
                            "create Vulkan depth chain");
                }
            }
            auto frame = [&](int frameIndex, int evidence, bool output) {
                li.displayTime = 1000000000 + frameIndex * 11000000;
                require(XR_SUCCEEDED(locate(session, &li, &state, 2, &count, views)),
                        "locate cooperative Vulkan views");
                require(same(views[0].fov, reduce(runtime::original[0], .8f, .8f)),
                        "cooperative Vulkan reduced FOV");
                for (int chainIndex = 0; chainIndex < chainCount; chainIndex++)
                    for (int type = 0; type < (runtime::temporal ? 2 : 1); type++) {
                        XrSwapchain handle = type ? depth[chainIndex] : color[chainIndex];
                        uint32_t index = 0;
                        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wi.timeout = XR_INFINITE_DURATION;
                        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        require(XR_SUCCEEDED(acquire(handle, &ai, &index)), "Vulkan acquire");
                        require(declare(handle, index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        runtime::device->vk->queue,
                                        runtime::device->producerFence) == XR_ERROR_VALIDATION_FAILURE,
                                "reject declaration before wait");
                        require(XR_SUCCEEDED(wait(handle, &wi)), "Vulkan wait");
                        auto &im = *runtime::chains.at(handle).images[index];
                        std::vector<uint32_t> pixels(size_t(im.width) * im.height * im.layers);
                        for (uint32_t array = 0; array < im.layers; array++)
                            for (uint32_t y = 0; y < im.height; y++)
                                for (uint32_t x = 0; x < im.width; x++) {
                                    bool blue = separate ? chainIndex == 1 : packed ? x >= 32 : array == 1;
                                    pixels[size_t(array) * im.width * im.height + y * im.width + x] =
                                        type ? std::bit_cast<uint32_t>(.5f)
                                             : (blue ? 0xffc83214 : 0xff1432c8);
                                }
                        runtime::device->upload(im, pixels);
                        auto layout = type ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                           : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        require(declare(handle, index, layout, VK_NULL_HANDLE,
                                        runtime::device->producerFence) == XR_ERROR_VALIDATION_FAILURE,
                                "reject wrong producer queue");
                        require(declare(handle, index + 10, layout, runtime::device->vk->queue,
                                        runtime::device->producerFence) == XR_ERROR_VALIDATION_FAILURE,
                                "reject wrong acquired index");
                        require(declare(handle, index, VK_IMAGE_LAYOUT_GENERAL, runtime::device->vk->queue,
                                        runtime::device->producerFence) == XR_ERROR_VALIDATION_FAILURE,
                                "reject unverified layout");
                        VkFence pending{};
                        if (evidence == 2) {
                            VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                            vkCheck(runtime::device->vk->CreateFence(runtime::device->binding.device, &f,
                                                                     nullptr, &pending),
                                    "create incomplete evidence fence");
                        }
                        if (evidence)
                            require(XR_SUCCEEDED(declare(handle, index, layout, runtime::device->vk->queue,
                                                         pending ? pending : runtime::device->producerFence)),
                                    "declare exact Vulkan image state");
                        require(XR_SUCCEEDED(release(handle, &ri)), "Vulkan image release");
                        require(declare(handle, index, layout, runtime::device->vk->queue,
                                        runtime::device->producerFence) == XR_ERROR_VALIDATION_FAILURE,
                                "reject stale declaration after release");
                        if (pending)
                            runtime::device->vk->DestroyFence(runtime::device->binding.device, pending,
                                                              nullptr);
                    }
                XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                          {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
                XrCompositionLayerDepthInfoKHR d[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                       {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
                for (uint32_t eye = 0; eye < 2; eye++) {
                    uint32_t array = separate || packed ? 0 : 1 - eye;
                    XrRect2Di rect{{packed ? int32_t(eye * 32) : 0, 0}, {32, 32}};
                    pv[eye].pose = views[eye].pose;
                    pv[eye].fov = views[eye].fov;
                    pv[eye].subImage = {color[separate ? eye : 0], rect, array};
                    if (runtime::temporal) {
                        d[eye].subImage = {depth[separate ? eye : 0], rect, array};
                        d[eye].maxDepth = 1;
                        d[eye].nearZ = .1f;
                        d[eye].farZ = 100;
                        pv[eye].next = &d[eye];
                    }
                    runtime::expected[eye] = pv[eye];
                    bool blue = separate || packed ? eye == 1 : array == 1;
                    runtime::wantedColor[eye] = blue ? 0xffc83214 : 0xff1432c8;
                }
                XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                projection.viewCount = 2;
                projection.views = pv;
                auto base = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
                XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
                endInfo.displayTime = li.displayTime;
                endInfo.layerCount = 1;
                endInfo.layers = &base;
                runtime::expectOutput = output;
                require(XR_SUCCEEDED(end(session, &endInfo)), "Vulkan end frame");
                require(pv[0].subImage.swapchain == runtime::expected[0].subImage.swapchain &&
                            same(pv[0].fov, views[0].fov),
                        "immutable application view structs");
            };
            runtime::timeout = true;
            frame(1, 1, false);
            frame(2, 1, true);
            frame(3, 0, false);
            frame(4, 2, false);
            frame(5, 1, true);
            for (int i = 6; i < 15; i++)
                frame(i, 1, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(550));
            // Disable only Vulkan through the existing live profile worker.
            updateJson(dir / "config.json",
                       [](Json &j) { j["defaults"]["vulkan"]["processing"] = "passthrough"; });
            bool stopped = false;
            for (int i = 0; i < 20; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                li.displayTime += 11000000;
                locate(session, &li, &state, 2, &count, views);
                if (same(views[0].fov, runtime::original[0])) {
                    stopped = true;
                    break;
                }
            }
            require(stopped, "Vulkan live policy disables FOV modification");
            for (int i = 0; cycle == 0 && i < chainCount; i++) {
                require(XR_SUCCEEDED(destroySwapchain(color[i])), "destroy Vulkan color/output resources");
                if (depth[i])
                    require(XR_SUCCEEDED(destroySwapchain(depth[i])), "destroy Vulkan depth resources");
            }
            if (cycle == 0)
                require(runtime::chains.empty(), "Vulkan outputs destroyed with input chains");
            require(XR_SUCCEEDED(destroySession(session)), "destroy Vulkan session");
            require(runtime::chains.empty(), "implicit session teardown destroys input and output chains");
            runtime::device->validationCheck();
            updateJson(dir / "config.json", [&](Json &j) { j = {{"defaults", profileJson(profile)}}; });
            std::this_thread::sleep_for(std::chrono::milliseconds(550));
        }
        auto history = loadJson(dir / "history.json");
        require(history[executablePath()].value("graphicsApi", "") == "Vulkan", "Vulkan process history");
        require(runtime::spacesCreated == runtime::spacesDestroyed, "Vulkan reference spaces released");
        require(XR_SUCCEEDED(proc<PFN_xrDestroyInstance>(g, instance, "xrDestroyInstance")(instance)),
                "destroy Vulkan XR instance");
        FreeLibrary(module);
        updateJson(std::filesystem::current_path() / "vulkan-evidence" /
                       ("openxr-" + runtime::evidenceName + ".json"),
                   [&](Json &j) {
                       j = {{"fixture", "developer-owned OpenXR runtime with actual Vulkan images"},
                            {"thirdPartyRuntime", false},
                            {"frames", runtime::journal},
                            {"sessionsCreatedAndDestroyed", 2},
                            {"validationEnabled", runtime::device->validation}};
                   });
        runtime::device->shutdown();
        runtime::device->validationCheck();
        runtime::device.reset();
        std::cout
            << "PASS: Vulkan OpenXR fixture, binding aliases, FIFO state, arrays/packed/separate eyes, exact "
               "layout/fence refusal and recovery, GPU output, depth, live policy and repeated teardown\n";
        return 0;
    } catch (const vkTest::Unavailable &e) {
        std::cout << (vkTest::validationRequired() ? "FAIL: " : "SKIP: ") << e.what() << '\n';
        return vkTest::validationRequired() ? 1 : 77;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
