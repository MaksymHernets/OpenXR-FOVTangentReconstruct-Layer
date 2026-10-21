#define wmain originalLayerTestMain
#include "LayerTests.cpp"
#undef wmain
#include "layer/FFRCooperation.h"
#include "renderer/D3D11FFR.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
using namespace fov;
static void layerCheck(ID3D11Device *, ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11PixelShader *,
                       const char *);
int main(int argc, char **argv) {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                       nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        bool debug = SUCCEEDED(hr);
        if (FAILED(hr))
            check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &device, nullptr, &context));
        D3D11FFR ffr(device.Get());
        auto caps = ffr.capabilities();
        std::cout << caps.dump() << '\n';
        if (!caps["hardwareSupported"].get<bool>()) {
            std::cout << "UNAVAILABLE: NVAPI VRS hardware\n";
            return 77;
        }
        const char *shader = R"(
        struct V{float4 pos:SV_Position;};
        V VS(uint id:SV_VertexID){V v;v.pos=float4(float2((id<<1)&2,id&2)*float2(2,-2)+float2(-1,1),0,1);return v;}
        float4 PS(V v):SV_Target{float s=sin(v.pos.x*.03)+cos(v.pos.y*.02);[loop]for(int i=0;i<120;i++)s=sin(s*.7+i*.01);return float4(.2+s*.1,.5,.8,1);}
    )";
        ComPtr<ID3DBlob> vs, ps, errors;
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs,
                         &errors));
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps,
                         &errors));
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> pixel;
        check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
        check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 512;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> rtv;
        check(device->CreateTexture2D(&td, nullptr, &target));
        check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        auto raw = rtv.Get();
        context->OMSetRenderTargets(1, &raw, nullptr);
        D3D11_VIEWPORT vp{0, 0, 512, 512, 0, 1};
        context->RSSetViewports(1, &vp);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        ComPtr<ID3D11Query> stats;
        check(device->CreateQuery(&qd, &stats));
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        ComPtr<ID3D11Query> disjoint;
        check(device->CreateQuery(&qd, &disjoint));
        qd.Query = D3D11_QUERY_TIMESTAMP;
        ComPtr<ID3D11Query> start, end;
        check(device->CreateQuery(&qd, &start));
        check(device->CreateQuery(&qd, &end));
        auto ready = [&](ID3D11Query *query, void *data, UINT bytes) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            HRESULT result;
            while ((result = context->GetData(query, data, bytes, 0)) == S_FALSE) {
                require(std::chrono::steady_clock::now() < deadline, "D3D11 query timeout");
                std::this_thread::yield();
            }
            check(result);
        };
        FFRSettings settings;
        settings.enabled = true;
        settings.centerX = settings.centerY = .2f;
        settings.fullMiddleQuality = settings.reducedMiddleQuality = 65;
        XrFovf baseline{-.8f, .8f, .8f, -.8f};
        Json cases = Json::array();
        for (float scale : {1.f, .8f}) {
            auto plan = makeFFRPlan(baseline, reduce(baseline, scale, scale), settings);
            double fullInvocations = 0;
            for (bool enabled : {false, true}) {
                double invocations = 0, gpuMs = 0;
                Json observed;
                for (int frame = 0; frame < 48; ++frame) {
                    context->Begin(disjoint.Get());
                    context->End(start.Get());
                    context->Begin(stats.Get());
                    if (enabled)
                        observed = ffr.begin(context.Get(), plan, 512, 512, {{0, 0}, {512, 512}});
                    context->Draw(3, 0);
                    if (enabled)
                        ffr.end(context.Get());
                    context->End(stats.Get());
                    context->End(end.Get());
                    context->End(disjoint.Get());
                    context->Flush();
                    D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
                    UINT64 a = 0, b = 0;
                    ready(disjoint.Get(), &d, sizeof(d));
                    ready(stats.Get(), &s, sizeof(s));
                    ready(start.Get(), &a, sizeof(a));
                    ready(end.Get(), &b, sizeof(b));
                    require(!d.Disjoint, "Stable D3D11 GPU clock");
                    if (frame >= 16) {
                        invocations += s.PSInvocations;
                        gpuMs += double(b - a) * 1000 / d.Frequency;
                    }
                }
                invocations /= 32;
                gpuMs /= 32;
                if (!enabled)
                    fullInvocations = invocations;
                else
                    require(invocations < fullInvocations * .9,
                            "NVAPI reduces actual pixel shader invocations");
                cases.push_back({{"effectiveScale", scale},
                                 {"ffr", enabled},
                                 {"meanPSInvocations", invocations},
                                 {"meanSceneGpuMs", gpuMs},
                                 {"recordedPlan", observed},
                                 {"outsideSceneSavings", 0}});
            }
            // End must restore the complete viewport to baseline shading.
            context->Begin(stats.Get());
            context->Draw(3, 0);
            context->End(stats.Get());
            context->Flush();
            D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
            ready(stats.Get(), &s, sizeof(s));
            require(s.PSInvocations >= 512 * 512, "NVAPI state restored after End");
        }
        std::filesystem::create_directories("ffr-evidence");
        std::ofstream("ffr-evidence/d3d11-nvapi.json") << Json{
            {"debugLayer", debug},
            {"capabilities", caps},
            {"cases", cases},
            {"gameIntegrationTested", false}}.dump(2);
        require(argc >= 2, "Layer DLL path required");
        layerCheck(device.Get(), context.Get(), vertex.Get(), pixel.Get(), argv[1]);
        if (debug) {
            ComPtr<ID3D11InfoQueue> messages;
            check(device.As(&messages));
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
                SIZE_T size = 0;
                check(messages->GetMessage(i, nullptr, &size));
                std::vector<uint8_t> data(size);
                auto message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
                check(messages->GetMessage(i, message, &size));
                if (message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
                    message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
                    std::cerr << message->pDescription << '\n';
                    require(false, "D3D11 debug layer error");
                }
            }
        }
        std::cout << "PASS: D3D11 NVAPI hardware VRS, measured scene workload and restoration\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

static XrSwapchain observedOutput{};
static XrResult XRAPI_CALL observedEnd(XrSession session, const XrFrameEndInfo *info) {
    auto result = runtime::end(session, info);
    observedOutput =
        reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0])->views[0].subImage.swapchain;
    return result;
}
static XrResult XRAPI_CALL observedGipa(XrInstance instance, const char *name, PFN_xrVoidFunction *out) {
    if (!strcmp(name, "xrEndFrame")) {
        *out = reinterpret_cast<PFN_xrVoidFunction>(observedEnd);
        return XR_SUCCESS;
    }
    return runtime::gipa(instance, name, out);
}
static void layerCheck(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11VertexShader *vs,
                       ID3D11PixelShader *ps, const char *dll) {
    auto dir = std::filesystem::current_path() / "test-data-nvapi-layer";
    SetEnvironmentVariableW(L"FOV_DATA_DIR", dir.c_str());
    Profile profile;
    profile.horizontal = profile.vertical = 1;
    profile.ffr.enabled = true;
    profile.color = {.25f, .5f, .75f};
    updateJson(dir / "config.json", [&](Json &j) { j = {{"defaults", profileJson(profile)}}; });
    runtime::device = device;
    runtime::d3d12 = false;
    runtime::expectOutput = true;
    HMODULE module = LoadLibraryW(wide(dll).c_str());
    require(module, "D3D11 layer load");
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
        GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
    XrNegotiateLoaderInfo loader{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
                                 XR_LOADER_INFO_STRUCT_VERSION,
                                 sizeof(XrNegotiateLoaderInfo),
                                 1,
                                 1,
                                 XR_MAKE_VERSION(1, 0, 0),
                                 XR_CURRENT_API_VERSION};
    XrNegotiateApiLayerRequest request{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST,
                                       XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(XrNegotiateApiLayerRequest)};
    require(negotiate && XR_SUCCEEDED(negotiate(&loader, "XR_APILAYER_FOV_extension", &request)),
            "NVAPI layer negotiate");
    XrApiLayerNextInfo next{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO,
                            XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(XrApiLayerNextInfo)};
    strcpy_s(next.layerName, "XR_APILAYER_FOV_extension");
    next.nextGetInstanceProcAddr = observedGipa;
    next.nextCreateApiLayerInstance = runtime::createInstance;
    XrApiLayerCreateInfo layer{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
                               XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(XrApiLayerCreateInfo)};
    layer.nextInfo = &next;
    const char *extension = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = &extension;
    XrInstance instance{};
    require(XR_SUCCEEDED(request.createApiLayerInstance(&ci, &layer, &instance)), "NVAPI instance");
    auto g = request.getInstanceProcAddr;
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
    si.next = &binding;
    XrSession session{};
    require(XR_SUCCEEDED(proc<PFN_xrCreateSession>(g, instance, "xrCreateSession")(instance, &si, &session)),
            "NVAPI session");
    auto begin = proc<PFN_xrFovBeginD3D11FFR>(g, instance, "xrFovBeginD3D11FFR");
    auto end = proc<PFN_xrFovEndD3D11FFR>(g, instance, "xrFovEndD3D11FFR");
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.width = sc.height = 512;
    sc.arraySize = 2;
    sc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.sampleCount = sc.faceCount = sc.mipCount = 1;
    require(XR_SUCCEEDED(proc<PFN_xrCreateSwapchain>(g, instance, "xrCreateSwapchain")(session, &sc,
                                                                                       &runtime::source)),
            "NVAPI source swapchain");
    Json evidence = Json::array();
    for (int mode = 0; mode < 2; ++mode)
        for (int method = 0; method < 6; ++method) {
            profile.horizontal = profile.vertical = mode ? .8f : 1.f;
            profile.method = method;
            updateJson(dir / "config.json", [&](Json &j) { j["defaults"] = profileJson(profile); });
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = 1000000000 + (mode * 6 + method) * 10000000;
            XrViewState state{XR_TYPE_VIEW_STATE};
            XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t count = 0;
            require(XR_SUCCEEDED(proc<PFN_xrLocateViews>(g, instance, "xrLocateViews")(session, &li, &state,
                                                                                       2, &count, views)),
                    "NVAPI located frame");
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            uint32_t index = 0;
            require(XR_SUCCEEDED(proc<PFN_xrAcquireSwapchainImage>(g, instance, "xrAcquireSwapchainImage")(
                        runtime::source, &ai, &index)),
                    "NVAPI acquire");
            require(XR_SUCCEEDED(proc<PFN_xrWaitSwapchainImage>(g, instance, "xrWaitSwapchainImage")(
                        runtime::source, &wi)),
                    "NVAPI wait");
            ComPtr<ID3D11Texture2D> image;
            check(runtime::chains.at(runtime::source).images[index].As(&image));
            XrCompositionLayerProjectionView submission[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            XrCompositionLayerDepthInfoKHR depth[2]{{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                    {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
            for (uint32_t eye = 0; eye < 2; ++eye) {
                D3D11_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.ArraySize = 1;
                desc.Texture2DArray.FirstArraySlice = eye;
                ComPtr<ID3D11RenderTargetView> rtv;
                check(device->CreateRenderTargetView(image.Get(), &desc, &rtv));
                auto raw = rtv.Get();
                context->OMSetRenderTargets(1, &raw, nullptr);
                D3D11_VIEWPORT viewport{0, 0, 512, 512, 0, 1};
                context->RSSetViewports(1, &viewport);
                context->VSSetShader(vs, nullptr, 0);
                context->PSSetShader(ps, nullptr, 0);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                FovD3D11FFRDrawInfo draw;
                draw.displayTime = li.displayTime;
                draw.eye = eye;
                draw.context = context;
                draw.subImage = {runtime::source, {{0, 0}, {512, 512}}, eye};
                auto invalid = draw;
                --invalid.displayTime;
                require(XR_FAILED(begin(session, &invalid)), "NVAPI stale projection rejected");
                require(XR_SUCCEEDED(begin(session, &draw)), "NVAPI layer begin");
                require(XR_FAILED(begin(session, &draw)), "NVAPI nested scope rejected");
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                require(XR_FAILED(proc<PFN_xrReleaseSwapchainImage>(g, instance, "xrReleaseSwapchainImage")(
                            runtime::source, &ri)),
                        "NVAPI active scope blocks capture");
                require(XR_FAILED(proc<PFN_xrDestroySession>(g, instance, "xrDestroySession")(session)),
                        "NVAPI active scope blocks destruction");
                context->Draw(3, 0);
                require(XR_SUCCEEDED(end(session, context)), "NVAPI layer restoration");
                submission[eye].pose = views[eye].pose;
                submission[eye].fov = views[eye].fov;
                submission[eye].subImage = draw.subImage;
                submission[eye].next = &depth[eye];
            }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            require(XR_SUCCEEDED(proc<PFN_xrReleaseSwapchainImage>(g, instance, "xrReleaseSwapchainImage")(
                        runtime::source, &ri)),
                    "NVAPI release capture");
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.views = submission;
            projection.viewCount = 2;
            auto header = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
            XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
            frame.displayTime = li.displayTime;
            frame.layerCount = 1;
            frame.layers = &header;
            require(XR_SUCCEEDED(proc<PFN_xrEndFrame>(g, instance, "xrEndFrame")(session, &frame)),
                    "NVAPI reconstructed frame");
            ComPtr<ID3D11Texture2D> output;
            check(runtime::chains.at(observedOutput).images[0].As(&output));
            D3D11_TEXTURE2D_DESC desc{};
            output->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.BindFlags = 0;
            ComPtr<ID3D11Texture2D> read;
            check(device->CreateTexture2D(&desc, nullptr, &read));
            context->CopyResource(read.Get(), output.Get());
            for (uint32_t eye = 0; eye < 2; ++eye) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(context->Map(read.Get(), eye, D3D11_MAP_READ, 0, &mapped));
                auto corner = *static_cast<uint32_t *>(mapped.pData);
                require((corner >> 24) == 255 && (corner & 0xffffff) != 0,
                        "All six algorithms fill full/cropped Outside with depth attachment");
                if (method == 4)
                    require(std::abs(int(corner & 255) - 64) <= 1 &&
                                std::abs(int((corner >> 8) & 255) - 128) <= 1 &&
                                std::abs(int((corner >> 16) & 255) - 191) <= 1,
                            "Selected solid color reaches full and cropped Outside");
                context->Unmap(read.Get(), eye);
            }
            evidence.push_back({{"fullFov", mode == 0},
                                {"algorithm", method},
                                {"outputReplaced", true},
                                {"plainDepthAccepted", true}});
        }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    require(XR_SUCCEEDED(proc<PFN_xrDestroySession>(g, instance, "xrDestroySession")(session)),
            "NVAPI session teardown");
    require(XR_SUCCEEDED(proc<PFN_xrDestroyInstance>(g, instance, "xrDestroyInstance")(instance)),
            "NVAPI instance teardown");
    FreeLibrary(module);
    std::ofstream("ffr-evidence/d3d11-layer.json") << evidence.dump(2);
}
