#include "common/Platform.h"
#include "common/ConfigManager.h"
#include "common/FovMath.h"
#include "common/Temporal.h"
#include <iostream>
#include <map>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>
using namespace fov;
static void require(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
namespace runtime {
uintptr_t next = 100;
ComPtr<ID3D11Device> device;
bool d3d12 = false;
bool temporal = false;
bool trackingValid = true, spaceValid = true;
XrTime referenceChange = 0;
XrSession lastSession{};
int spacesCreated = 0, spacesDestroyed = 0;
ComPtr<ID3D12Device> device12;
ComPtr<ID3D12CommandQueue> queue12;
struct Chain {
    XrSwapchainCreateInfo info;
    XrSession session{};
    std::vector<ComPtr<IUnknown>> images;
    bool acquired = false, waited = false;
};
std::map<XrSwapchain, Chain> chains;
bool timeout = false;
bool expectOutput = true;
XrSwapchain source{};
XrSwapchain depthSource{};
int calls = 0;
XrFovf original[2] = {{-.9f, .65f, .8f, -.7f}, {-.65f, .9f, .8f, -.7f}};
XrResult XRAPI_CALL destroyInstance(XrInstance) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createSession(XrInstance, const XrSessionCreateInfo *, XrSession *out) {
    *out = reinterpret_cast<XrSession>(++next);
    lastSession = *out;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroySession(XrSession session) {
    for (auto it = chains.begin(); it != chains.end();)
        if (it->second.session == session)
            it = chains.erase(it);
        else
            ++it;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerateViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t capacity,
                                   uint32_t *count, XrViewConfigurationView *views) {
    *count = 2;
    if (capacity) {
        for (int i = 0; i < 2; i++) {
            views[i].recommendedImageRectWidth = 1000;
            views[i].recommendedImageRectHeight = 800;
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locate(XrSession, const XrViewLocateInfo *, XrViewState *state, uint32_t capacity,
                           uint32_t *count, XrView *views) {
    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (!trackingValid)
        state->viewStateFlags = 0;
    if (capacity)
        for (int i = 0; i < 2; i++) {
            views[i].fov = original[i];
            views[i].pose.orientation = {0, 0, 0, 1};
            views[i].pose.position = {(i ? 1.f : -1.f) * .032f, 0, 0};
        }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createSwapchain(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *out) {
    *out = reinterpret_cast<XrSwapchain>(++next);
    Chain c;
    c.info = *info;
    c.session = session;
    if (d3d12) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = info->width;
        td.Height = info->height;
        td.DepthOrArraySize = static_cast<UINT16>(info->arraySize);
        td.MipLevels = 1;
        td.Format = (DXGI_FORMAT)info->format;
        td.SampleDesc.Count = 1;
        td.Flags = depthFormat(info->format) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                             : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (int i = 0; i < 2; i++) {
            ComPtr<ID3D12Resource> image;
            check(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                                                    depthFormat(info->format)
                                                        ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                        : D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                    nullptr, IID_PPV_ARGS(&image)));
            c.images.push_back(image);
        }
        chains[*out] = std::move(c);
        return XR_SUCCESS;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = info->width;
    td.Height = info->height;
    td.ArraySize = info->arraySize;
    td.MipLevels = 1;
    td.Format = (DXGI_FORMAT)info->format;
    td.SampleDesc.Count = 1;
    td.BindFlags = depthFormat(info->format) ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; i++) {
        ComPtr<ID3D11Texture2D> t;
        check(device->CreateTexture2D(&td, nullptr, &t));
        c.images.push_back(t);
    }
    chains[*out] = std::move(c);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroySwapchain(XrSwapchain h) {
    chains.erase(h);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerateImages(XrSwapchain h, uint32_t capacity, uint32_t *count,
                                    XrSwapchainImageBaseHeader *images) {
    *count = 2;
    if (capacity) {
        for (int i = 0; i < 2; i++) {
            if (d3d12) {
                ComPtr<ID3D12Resource> t;
                check(chains.at(h).images[i].As(&t));
                reinterpret_cast<XrSwapchainImageD3D12KHR *>(images)[i].texture = t.Get();
            } else {
                ComPtr<ID3D11Texture2D> t;
                check(chains.at(h).images[i].As(&t));
                reinterpret_cast<XrSwapchainImageD3D11KHR *>(images)[i].texture = t.Get();
            }
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL acquire(XrSwapchain h, const XrSwapchainImageAcquireInfo *, uint32_t *index) {
    auto &c = chains.at(h);
    require(!c.acquired, "Double acquire in test runtime");
    c.acquired = true;
    c.waited = false;
    *index = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL wait(XrSwapchain h, const XrSwapchainImageWaitInfo *) {
    auto &c = chains.at(h);
    require(c.acquired, "Wait without acquire");
    if (timeout && h != source && h != depthSource) {
        timeout = false;
        return XR_TIMEOUT_EXPIRED;
    }
    c.waited = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL release(XrSwapchain h, const XrSwapchainImageReleaseInfo *) {
    auto &c = chains.at(h);
    require(c.acquired && c.waited, "Release without successful wait");
    c.acquired = c.waited = false;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL end(XrSession, const XrFrameEndInfo *info) {
    calls++;
    auto p = reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0]);
    for (uint32_t i = 0; i < p->viewCount; i++) {
        auto v = p->views[i];
        require(!chains.at(v.subImage.swapchain).acquired, "Submitted acquired image");
        require((v.subImage.swapchain != source) == expectOutput, "Output swapchain replacement");
        if (temporal && expectOutput)
            require(!v.next, "Narrow input depth is not attached to widened output");
        auto wanted = expectOutput ? original[i] : reduce(original[i], .8f, .8f);
        require(same(v.fov, wanted), "Consistent submitted FOV");
        require(std::abs(v.pose.position.x - (i ? 1.f : -1.f) * .032f) < 1e-6f,
                "Original eye pose preserved");
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL extra(XrInstance, XrInstanceProperties *) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL beginSession(XrSession, const XrSessionBeginInfo *) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL endSession(XrSession) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL pollEvent(XrInstance, XrEventDataBuffer *buffer) {
    if (!referenceChange)
        return XR_EVENT_UNAVAILABLE;
    XrEventDataReferenceSpaceChangePending change{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
    change.session = lastSession;
    change.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    change.changeTime = referenceChange;
    memcpy(buffer, &change, sizeof(change));
    referenceChange = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL createReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo *, XrSpace *space) {
    *space = reinterpret_cast<XrSpace>(++next);
    spacesCreated++;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation *location) {
    location->locationFlags =
        spaceValid ? XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT : 0;
    location->pose = {{0, 0, 0, 1}, {0, 0, 0}};
    return XR_SUCCESS;
}
XrResult XRAPI_CALL destroySpace(XrSpace) {
    spacesDestroyed++;
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
    MAP(CreateReferenceSpace, createReferenceSpace)
    MAP(LocateSpace, locateSpace)
    MAP(DestroySpace, destroySpace)
    MAP(BeginSession, beginSession)
    MAP(EndSession, endSession)
    MAP(PollEvent, pollEvent) MAP(LocateViews, locate) MAP(EnumerateViewConfigurationViews, enumerateViews)
        MAP(CreateSwapchain, createSwapchain) MAP(DestroySwapchain, destroySwapchain)
            MAP(EnumerateSwapchainImages, enumerateImages) MAP(AcquireSwapchainImage, acquire)
                MAP(WaitSwapchainImage, wait) MAP(ReleaseSwapchainImage, release) MAP(EndFrame, end)
                    MAP(GetInstanceProperties, extra)
#undef MAP
                        *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult XRAPI_CALL createInstance(const XrInstanceCreateInfo *, const XrApiLayerCreateInfo *info,
                                   XrInstance *out) {
    require(!info->nextInfo, "Layer chain advanced");
    *out = reinterpret_cast<XrInstance>(++next);
    return XR_SUCCESS;
}
} // namespace runtime
template <class T> T proc(PFN_xrGetInstanceProcAddr g, XrInstance i, const char *name) {
    T p{};
    require(XR_SUCCEEDED(g(i, name, reinterpret_cast<PFN_xrVoidFunction *>(&p))),
            "Get instance proc address");
    return p;
}
int wmain(int argc, wchar_t **argv) {
    try {
        require(argc >= 2, "DLL path required");
        for (int argument = 2; argument < argc; argument++) {
            runtime::d3d12 |= !wcscmp(argv[argument], L"--d3d12");
            runtime::temporal |= !wcscmp(argv[argument], L"--temporal");
        }
        auto dir = std::filesystem::current_path() /
                   (runtime::temporal
                        ? (runtime::d3d12 ? L"test-data-temporal-layer12" : L"test-data-temporal-layer11")
                        : (runtime::d3d12 ? L"test-data-layer12" : L"test-data-layer"));
        SetEnvironmentVariableW(L"FOV_DATA_DIR", dir.c_str());
        updateJson(dir / L"config.json", [](Json &j) {
            Profile profile;
            profile.ffr.enabled = true; // Unsupported hardware/integration must preserve reconstruction.
            if (runtime::temporal)
                profile.method = 5;
            j = {{"defaults", profileJson(profile)}};
        });
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &runtime::device, nullptr, nullptr));
        if (runtime::d3d12) {
            ComPtr<IDXGIFactory4> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
            check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&runtime::device12)));
            D3D12_COMMAND_QUEUE_DESC desc{};
            check(runtime::device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&runtime::queue12)));
        }
        HMODULE module = LoadLibraryW(argv[1]);
        require(module != nullptr, "Load layer DLL");
        auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
            GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
        require(negotiate != nullptr, "Negotiation export");
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
        require(negotiate(&loader, "wrong", &request) == XR_ERROR_INITIALIZATION_FAILED,
                "Reject wrong layer name");
        require(XR_SUCCEEDED(negotiate(&loader, "XR_APILAYER_FOV_extension", &request)), "Negotiate");
        auto g = request.getInstanceProcAddr;
        XrApiLayerNextInfo next{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO,
                                XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(XrApiLayerNextInfo)};
        strcpy_s(next.layerName, "XR_APILAYER_FOV_extension");
        next.nextGetInstanceProcAddr = runtime::gipa;
        next.nextCreateApiLayerInstance = runtime::createInstance;
        XrApiLayerCreateInfo layer{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
                                   XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(XrApiLayerCreateInfo)};
        layer.nextInfo = &next;
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ci.applicationInfo.applicationName, "FOV integration test");
        ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        const char *depthExtension = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
        if (runtime::temporal) {
            ci.enabledExtensionCount = 1;
            ci.enabledExtensionNames = &depthExtension;
        }
        XrInstance instance{}, second{};
        require(XR_SUCCEEDED(request.createApiLayerInstance(&ci, &layer, &instance)), "Create instance");
        require(XR_SUCCEEDED(request.createApiLayerInstance(&ci, &layer, &second)), "Multiple instances");
        require(proc<PFN_xrGetInstanceProperties>(g, instance, "xrGetInstanceProperties") == runtime::extra,
                "Forward unknown hooks");
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = runtime::device.Get();
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding12.device = runtime::device12.Get();
        binding12.queue = runtime::queue12.Get();
        sessionInfo.next = runtime::d3d12 ? static_cast<void *>(&binding12) : static_cast<void *>(&binding);
        XrSession session;
        require(XR_SUCCEEDED(proc<PFN_xrCreateSession>(g, instance, "xrCreateSession")(instance, &sessionInfo,
                                                                                       &session)),
                "Create D3D11 session");
        XrViewConfigurationView dimensions[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                 {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t count = 0;
        proc<PFN_xrEnumerateViewConfigurationViews>(g, instance, "xrEnumerateViewConfigurationViews")(
            instance, 1, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, dimensions);
        require(dimensions[0].recommendedImageRectWidth == 800 &&
                    dimensions[0].recommendedImageRectHeight == 640,
                "Reduced render dimensions");
        XrSwapchainCreateInfo si{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        si.width = si.height = 32;
        si.arraySize = 2;
        si.faceCount = si.mipCount = si.sampleCount = 1;
        si.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        si.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        require(XR_SUCCEEDED(proc<PFN_xrCreateSwapchain>(g, instance, "xrCreateSwapchain")(session, &si,
                                                                                           &runtime::source)),
                "Create swapchain");
        if (runtime::temporal) {
            auto depthInfo = si;
            depthInfo.width = depthInfo.height = 64;
            depthInfo.format = DXGI_FORMAT_D32_FLOAT;
            depthInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            require(XR_SUCCEEDED(proc<PFN_xrCreateSwapchain>(g, instance, "xrCreateSwapchain")(
                        session, &depthInfo, &runtime::depthSource)),
                    "Create depth swapchain");
        }
        auto renderFrame = [&](int frame, bool expectOutput, bool extension) {
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = frame * 11000000;
            XrViewState state{XR_TYPE_VIEW_STATE};
            require(XR_SUCCEEDED(proc<PFN_xrLocateViews>(g, instance, "xrLocateViews")(session, &li, &state,
                                                                                       2, &count, views)),
                    "Locate views");
            require(same(views[0].fov, reduce(runtime::original[0], .8f, .8f)), "Returned reduced FOV");
            uint32_t index;
            XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            w.timeout = XR_INFINITE_DURATION;
            XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto acq = proc<PFN_xrAcquireSwapchainImage>(g, instance, "xrAcquireSwapchainImage");
            auto wait = proc<PFN_xrWaitSwapchainImage>(g, instance, "xrWaitSwapchainImage");
            auto release = proc<PFN_xrReleaseSwapchainImage>(g, instance, "xrReleaseSwapchainImage");
            require(XR_SUCCEEDED(acq(runtime::source, &a, &index)), "Acquire");
            require(XR_SUCCEEDED(wait(runtime::source, &w)), "Wait");
            require(XR_SUCCEEDED(release(runtime::source, &r)), "Release");
            if (runtime::temporal) {
                require(XR_SUCCEEDED(acq(runtime::depthSource, &a, &index)), "Acquire depth");
                require(XR_SUCCEEDED(wait(runtime::depthSource, &w)), "Wait depth");
                require(XR_SUCCEEDED(release(runtime::depthSource, &r)), "Release depth");
            }
            XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            XrCompositionLayerDepthInfoKHR depth[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                       {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
            for (int i = 0; i < 2; i++) {
                pv[i].fov = views[i].fov;
                pv[i].pose = views[i].pose;
                pv[i].subImage = {runtime::source, {{0, 0}, {32, 32}}, (uint32_t)i};
                if (extension)
                    pv[i].next = &depth[i];
                if (runtime::temporal) {
                    depth[i].subImage = {runtime::depthSource, {{0, 0}, {64, 64}}, (uint32_t)i};
                    depth[i].minDepth = 0;
                    depth[i].maxDepth = 1;
                    depth[i].nearZ = .1f;
                    depth[i].farZ = 100;
                }
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.viewCount = 2;
            projection.views = pv;
            auto base = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = li.displayTime;
            end.layerCount = 1;
            end.layers = &base;
            runtime::expectOutput = expectOutput;
            require(XR_SUCCEEDED(proc<PFN_xrEndFrame>(g, instance, "xrEndFrame")(session, &end)),
                    "End frame");
            require(same(pv[0].fov, views[0].fov) && pv[0].subImage.swapchain == runtime::source,
                    "Application frame structures unchanged");
        };
        runtime::timeout = true;
        renderFrame(1, false, runtime::temporal);
        renderFrame(2, true, runtime::temporal);
        renderFrame(3, runtime::temporal, true);
        renderFrame(4, true, runtime::temporal);
        if (runtime::temporal)
            for (int frame = 5; frame <= 12; frame++)
                renderFrame(frame, true, true);
        if (runtime::temporal) {
            runtime::spaceValid = false;
            renderFrame(13, true, true);
            runtime::spaceValid = true;
            renderFrame(14, true, true);
            runtime::referenceChange = 15 * 11000000;
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            require(proc<PFN_xrPollEvent>(g, instance, "xrPollEvent")(instance, &event) == XR_SUCCESS,
                    "Reference-space event forwarded");
            renderFrame(15, true, true);
            renderFrame(16, true, true);
            runtime::trackingValid = false;
            renderFrame(17, true, true);
            runtime::trackingValid = true;
            renderFrame(18, true, true);
            renderFrame(19, true, true);
            require(proc<PFN_xrEndSession>(g, instance, "xrEndSession")(session) == XR_SUCCESS,
                    "Session end invalidates history");
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            require(proc<PFN_xrBeginSession>(g, instance, "xrBeginSession")(session, &begin) == XR_SUCCESS,
                    "Session begin invalidates history");
            renderFrame(20, true, true);
            renderFrame(21, true, true);
        }
        // Connect to the real local diagnostic service. No GUI is needed for history/config.
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        WIN32_FIND_DATAW pipeData{};
        auto pattern = L"\\\\.\\pipe\\OpenXRFovExtension-" + std::to_wstring(GetCurrentProcessId()) + L"-*";
        HANDLE find = FindFirstFileW(pattern.c_str(), &pipeData);
        require(find != INVALID_HANDLE_VALUE, "Diagnostic pipe discovery");
        FindClose(find);
        auto pipePath = std::wstring(L"\\\\.\\pipe\\") + pipeData.cFileName;
        HANDLE pipe = CreateFileW(pipePath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        require(pipe != INVALID_HANDLE_VALUE, "Diagnostic pipe connection");
        char payload[16384];
        DWORD read = 0;
        require(ReadFile(pipe, payload, sizeof(payload), &read, nullptr) && read > 0, "Receive diagnostics");
        CloseHandle(pipe);
        auto diagnostics = Json::parse(std::string(payload, read));
        if (runtime::temporal) {
            auto status = diagnostics.at("temporalDepth");
            require(status.is_array() && status.size() == 2 && status[0].value("depthAvailable", false) &&
                        status[0].value("validHistoryFrames", 0) >= 1,
                    "Depth/history hooks report actual acquisition");
        }
        require(diagnostics.value("graphicsApi", "") == (runtime::d3d12 ? "D3D12" : "D3D11"),
                "Diagnostic graphics binding");
        auto ffrStatus = diagnostics.at("ffr");
        require(!ffrStatus.value("hardwareApplicationConfirmed", true) && ffrStatus["eyes"].size() == 2,
                "FFR configured maps never imply hardware application");
        require(ffrStatus["eyes"][0]["mode"] == "TwoRenderedZonesWithCrop" &&
                    !ffrStatus["eyes"][0]["cropIsRenderedZone"].get<bool>(),
                "FFR mode follows actual layer projection");
        if (!runtime::d3d12)
            require(!ffrStatus["capabilities"]["sceneIntegrationSupported"].get<bool>(),
                    "D3D11 FFR unsupported explicitly");
        auto path = executablePath();
        auto history = loadJson(dir / L"history.json");
        require(history.contains(path) &&
                    history[path].value("graphicsApi", "") == (runtime::d3d12 ? "D3D12" : "D3D11"),
                "History without GUI");
        updateJson(dir / L"config.json", [](Json &j) { j["defaults"]["enabled"] = false; });
        XrView unmodified[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewState unmodifiedState{XR_TYPE_VIEW_STATE};
        XrViewLocateInfo unmodifiedLocate{XR_TYPE_VIEW_LOCATE_INFO};
        unmodifiedLocate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        unmodifiedLocate.displayTime = 10;
        bool reloaded = false;
        for (int retry = 0; retry < 30; retry++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto result = proc<PFN_xrLocateViews>(g, instance, "xrLocateViews")(
                session, &unmodifiedLocate, &unmodifiedState, 2, &count, unmodified);
            if (XR_SUCCEEDED(result) && same(unmodified[0].fov, runtime::original[0])) {
                reloaded = true;
                break;
            }
        }
        if (!reloaded)
            std::cerr << "Loaded profile enabled=" << loadProfile(path).enabled
                      << " actual left=" << unmodified[0].fov.angleLeft
                      << " expected=" << runtime::original[0].angleLeft << "\\n";
        require(reloaded, "Live configuration reload");
        require(runtime::calls == (runtime::temporal ? 21 : 4), "Frame forwarding count");
        require(
            XR_SUCCEEDED(proc<PFN_xrDestroySwapchain>(g, instance, "xrDestroySwapchain")(runtime::source)),
            "Destroy swapchain");
        if (runtime::temporal)
            require(XR_SUCCEEDED(proc<PFN_xrDestroySwapchain>(g, instance,
                                                              "xrDestroySwapchain")(runtime::depthSource)),
                    "Destroy depth swapchain");
        require(runtime::chains.empty(), "Output swapchain destroyed");
        require(XR_SUCCEEDED(proc<PFN_xrDestroySession>(g, instance, "xrDestroySession")(session)),
                "Destroy session");
        if (runtime::temporal)
            require(runtime::spacesCreated == 1 && runtime::spacesDestroyed == 1,
                    "Owned reference-space lifetime");
        auto destroy = proc<PFN_xrDestroyInstance>(g, instance, "xrDestroyInstance");
        require(XR_SUCCEEDED(destroy(instance)) && XR_SUCCEEDED(destroy(second)), "Destroy instances");
        FreeLibrary(module);
        std::cout << "PASS: negotiation, chain forwarding, multiple instances, reconstruction, timeout "
                     "recovery, extension passthrough, FOV/lifetime integrity, live IPC/config/history\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
