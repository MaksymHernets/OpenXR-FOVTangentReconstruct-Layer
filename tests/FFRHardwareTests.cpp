// Reuse the existing OpenXR runtime fixture, not its WARP test entry point.
#define wmain originalLayerTestMain
#include "LayerTests.cpp"
#undef wmain
#include "renderer/D3D12FFR.h"
#include "renderer/IEdgeRenderer.h"
#include <fstream>
#include "layer/FFRCooperation.h"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <chrono>
#include <numeric>
struct RasterFixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12GraphicsCommandList> scene, tail;
    ComPtr<ID3D12CommandAllocator> sceneAllocator, tailAllocator;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    ComPtr<ID3D12QueryHeap> timestamps, statistics;
    ComPtr<ID3D12Resource> results;
    ComPtr<ID3D12InfoQueue> errors;
    UniqueHandle event;
    uint64_t serial = 0, frequency = 0;
    std::string adapter;
    bool debug = false;
    RasterFixture() {
        ComPtr<ID3D12Debug> layer;
        debug = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
        if (debug)
            layer->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 d{};
            check(a->GetDesc1(&d));
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            ComPtr<ID3D12Device> candidate;
            if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate))))
                continue;
            D3D12FFR probe(candidate.Get());
            if (!probe.capabilities()["hardwareSupported"].get<bool>())
                continue;
            device = candidate;
            adapter = utf8(d.Description);
            break;
        }
        if (!device)
            throw std::runtime_error("UNAVAILABLE: D3D12 VRS Tier 2 GPU");
        if (debug)
            check(device.As(&errors));
        D3D12_COMMAND_QUEUE_DESC q{};
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        check(queue->GetTimestampFrequency(&frequency));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        require(event.get() != nullptr, "fixture event");
        for (auto pair : {std::pair{std::addressof(sceneAllocator), std::addressof(scene)},
                          std::pair{std::addressof(tailAllocator), std::addressof(tail)}}) {
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(pair.first->ReleaseAndGetAddressOf())));
            check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pair.first->Get(), nullptr,
                                            IID_PPV_ARGS(pair.second->ReleaseAndGetAddressOf())));
            check(pair.second->Get()->Close());
        }
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 0, 1};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{};
        rd.NumParameters = 1;
        rd.pParameters = &parameter;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, error;
        check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
        check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root)));
        const char *shader = R"(
cbuffer Params:register(b0){uint eye;}
float4 vs(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}
float4 ps(float4 p:SV_Position):SV_Target {
 float v=frac(dot(p.xy,float2(.019,.023)));
 [loop] for(int i=0;i<120;++i)v=frac(sin(v*6.1+i*.013)*.71+v*.83);
 float grid=(fmod(p.x,48)<2 || fmod(p.y,48)<2)?1:.15+v*.12;
 return float4(eye==0?grid:.12,.15+v*.15,eye==1?grid:.12,1);
})";
        ComPtr<ID3DBlob> vs, ps;
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error));
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 4;
        check(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&timestamps)));
        qd.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        qd.Count = 2;
        check(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&statistics)));
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 512;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&results)));
    }
    ComPtr<ID3D12Resource> target(uint32_t width, uint32_t height) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width;
        d.Height = height;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r)));
        return r;
    }
    void wait() {
        if (fence->GetCompletedValue() < serial) {
            check(fence->SetEventOnCompletion(serial, event.get()));
            require(WaitForSingleObject(event.get(), 10000) == WAIT_OBJECT_0, "GPU fixture fence timeout");
        }
    }
    void submit(ID3D12GraphicsCommandList *list) {
        check(list->Close());
        ID3D12CommandList *lists[]{list};
        queue->ExecuteCommandLists(1, lists);
    }
    void openScene() {
        wait();
        check(sceneAllocator->Reset());
        check(scene->Reset(sceneAllocator.Get(), pso.Get()));
        scene->SetGraphicsRootSignature(root.Get());
        scene->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    void draw(ID3D12Resource *image, XrRect2Di rect, uint32_t eye) {
        device->CreateRenderTargetView(image, nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());
        auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        scene->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT v{float(rect.offset.x),
                         float(rect.offset.y),
                         float(rect.extent.width),
                         float(rect.extent.height),
                         0,
                         1};
        D3D12_RECT sc{rect.offset.x, rect.offset.y, rect.offset.x + rect.extent.width,
                      rect.offset.y + rect.extent.height};
        scene->RSSetViewports(1, &v);
        scene->RSSetScissorRects(1, &sc);
        scene->SetGraphicsRoot32BitConstant(0, eye, 0);
        scene->DrawInstanced(3, 1, 0, 0);
    }
    void validate() {
        if (!errors)
            return;
        for (UINT64 i = 0; i < errors->GetNumStoredMessages(); ++i) {
            SIZE_T n = 0;
            errors->GetMessage(i, nullptr, &n);
            std::vector<uint8_t> bytes(n);
            auto m = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
            check(errors->GetMessage(i, m, &n));
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(m->pDescription);
        }
    }
    void capture(ID3D12Resource *image, const std::string &name) {
        auto d = image->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};
        UINT rows;
        UINT64 row, size;
        device->GetCopyableFootprints(&d, 0, 1, 0, &foot, &rows, &row, &size);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = size;
        b.Height = b.DepthOrArraySize = b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> read;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&read)));
        check(tailAllocator->Reset());
        check(tail->Reset(tailAllocator.Get(), nullptr));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        tail->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = image;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = read.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = foot;
        tail->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        tail->ResourceBarrier(1, &barrier);
        submit(tail.Get());
        check(queue->Signal(fence.Get(), ++serial));
        wait();
        uint8_t *data;
        D3D12_RANGE range{0, SIZE_T(size)};
        check(read->Map(0, &range, reinterpret_cast<void **>(&data)));
        std::filesystem::create_directories("ffr-evidence");
        std::ofstream out("ffr-evidence/" + name + ".ppm", std::ios::binary);
        out << "P6\n" << d.Width << ' ' << d.Height << "\n255\n";
        for (UINT y = 0; y < d.Height; ++y)
            for (UINT x = 0; x < d.Width; ++x) {
                auto p = data + foot.Offset + size_t(y) * foot.Footprint.RowPitch + x * 4;
                require(p[3] == 255, "Rendered output alpha covers all pixels");
                require(p[0] || p[1] || p[2], "Colored peripheral pattern has no unintended black gaps");
                out.write(reinterpret_cast<char *>(p), 3);
            }
        read->Unmap(0, nullptr);
    }
};
static double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[size_t(std::ceil(p * (v.size() - 1)))];
}
static Json benchmark(RasterFixture &gpu, float h, float v, bool enabled, int quality, int outer,
                      const std::string &policy) {
    uint32_t width = uint32_t(1024 * h) / 2 * 2, height = uint32_t(768 * v);
    auto source = gpu.target(width, height), output = gpu.target(1024, 768);
    D3D12FFR ffr(gpu.device.Get());
    auto reconstruction = makeD3D12(gpu.device.Get(), gpu.queue.Get());
    FFRSettings settings;
    settings.enabled = enabled;
    settings.fullMiddleQuality = settings.reducedMiddleQuality = quality;
    settings.outerQuality = outer;
    settings.policy = policy;
    std::vector<double> sceneMs, totalMs, reconMs, cpuMs, waitMs;
    std::vector<uint64_t> invocations;
    Json recorded;
    unsigned warmAllocations = 0;
    for (int frame = 0; frame < 80; ++frame) {
        auto cpuStart = std::chrono::steady_clock::now();
        gpu.openScene();
        gpu.scene->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        gpu.scene->BeginQuery(gpu.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        uint64_t completion = ++gpu.serial;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            XrRect2Di rect{{int(eye * width / 2), 0}, {int(width / 2), int(height)}};
            auto plan = makeFFRPlan(runtime::original[eye], reduce(runtime::original[eye], h, v), settings);
            if (enabled) {
                recorded =
                    ffr.begin(gpu.scene.Get(), eye, gpu.fence.Get(), completion, plan, width, height, rect);
                if (frame >= 16 && recorded["mapAllocated"].get<bool>())
                    ++warmAllocations;
            }
            gpu.draw(source.Get(), rect, eye);
            if (enabled)
                ffr.end(gpu.scene.Get());
        }
        gpu.scene->EndQuery(gpu.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        gpu.scene->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        gpu.submit(gpu.scene.Get());
        check(gpu.queue->Signal(gpu.fence.Get(), completion));
        if (h < 1 || v < 1) {
            reconstruction->capture(77, source.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            Profile p;
            p.horizontal = h;
            p.vertical = v;
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.width = width;
            ci.height = height;
            ci.arraySize = 1;
            ci.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            for (uint32_t eye = 0; eye < 2; ++eye) {
                XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.fov = reduce(runtime::original[eye], h, v);
                view.subImage.imageRect = {{int(eye * width / 2), 0}, {int(width / 2), int(height)}};
                auto c = constants(view, runtime::original[eye], ci, p);
                reconstruction->render(77, output.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, c,
                                       {{int(eye * 512), 0}, {512, 768}});
            }
        }
        check(gpu.tailAllocator->Reset());
        check(gpu.tail->Reset(gpu.tailAllocator.Get(), nullptr));
        gpu.tail->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        gpu.tail->ResolveQueryData(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, gpu.results.Get(),
                                   0);
        gpu.tail->ResolveQueryData(gpu.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
                                   gpu.results.Get(), 64);
        gpu.submit(gpu.tail.Get());
        check(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial));
        auto cpuEnd = std::chrono::steady_clock::now();
        gpu.wait();
        auto waited = std::chrono::steady_clock::now();
        uint8_t *data;
        D3D12_RANGE range{0, 256};
        check(gpu.results->Map(0, &range, reinterpret_cast<void **>(&data)));
        auto t = reinterpret_cast<uint64_t *>(data);
        auto stats = reinterpret_cast<D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(data + 64);
        if (frame >= 16) {
            sceneMs.push_back(double(t[1] - t[0]) * 1000 / gpu.frequency);
            totalMs.push_back(double(t[2] - t[0]) * 1000 / gpu.frequency);
            reconMs.push_back(double(t[2] - t[1]) * 1000 / gpu.frequency);
            invocations.push_back(stats->PSInvocations);
            cpuMs.push_back(std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count());
            waitMs.push_back(std::chrono::duration<double, std::milli>(waited - cpuEnd).count());
        }
        gpu.results->Unmap(0, nullptr);
    }
    require(warmAllocations == 0, "No steady-state VRS map allocation");
    std::string name =
        (h == 1 && v == 1
             ? "full"
             : ("reduced-" + std::to_string(int(h * 100)) + "-" + std::to_string(int(v * 100)))) +
        (enabled ? "-ffr" : "-baseline") + "-q" + std::to_string(quality);
    gpu.capture(h < 1 || v < 1 ? output.Get() : source.Get(), name);
    gpu.validate();
    auto mean = [](const auto &a) { return std::accumulate(a.begin(), a.end(), 0.0) / a.size(); };
    auto deadlines = [&](double ms) {
        return std::count_if(totalMs.begin(), totalMs.end(), [&](double t) { return t > ms; });
    };
    return {{"name", name},
            {"scale", {h, v}},
            {"ffr", enabled},
            {"middleQuality", quality},
            {"outerQuality", outer},
            {"policy", policy},
            {"mode", ffrModeName(DetermineFFRMode(h, v))},
            {"sourceDimensions", {width, height}},
            {"outputDimensions", {1024, 768}},
            {"measuredFrames", 64},
            {"warmupFrames", 16},
            {"sceneGpuMeanMs", mean(sceneMs)},
            {"frameGpuTimelineMeanMs", mean(totalMs)},
            {"postSceneGpuTimelineMeanMs", mean(reconMs)},
            {"frameGpuP50Ms", percentile(totalMs, .5)},
            {"frameGpuP95Ms", percentile(totalMs, .95)},
            {"frameGpuP99Ms", percentile(totalMs, .99)},
            {"cpuRecordSubmitMeanMs", mean(cpuMs)},
            {"fixtureCpuFenceWaitMeanMs", mean(waitMs)},
            {"meanPixelShaderInvocations", mean(invocations)},
            {"missed90HzTimelineBudgets", deadlines(1000. / 90)},
            {"missed120HzTimelineBudgets", deadlines(1000. / 120)},
            {"headsetMissedDeadlines", nullptr},
            {"steadyStateMapAllocations", warmAllocations},
            {"recordedPlan", recorded}};
}
static void layerIntegration(RasterFixture &, const wchar_t *);
int wmain(int argc, wchar_t **argv) {
    try {
        require(argc >= 2, "Layer DLL required");
        RasterFixture gpu;
        std::cout << "Hardware: " << gpu.adapter << " debug=" << gpu.debug << '\n';
        Json results = Json::array();
        for (auto scale : {std::pair{1.f, 1.f}, std::pair{.85f, .85f}, std::pair{.7f, 1.f}}) {
            auto baseline = benchmark(gpu, scale.first, scale.second, false, 65, 15, "balanced");
            auto coarse = benchmark(gpu, scale.first, scale.second, true, 65, 15, "balanced");
            require(coarse["meanPixelShaderInvocations"].get<double>() <
                        baseline["meanPixelShaderInvocations"].get<double>() * .9,
                    "Hardware PS invocation reduction");
            coarse["invocationReductionPercent"] =
                100 * (1 - coarse["meanPixelShaderInvocations"].get<double>() /
                               baseline["meanPixelShaderInvocations"].get<double>());
            auto loadCapture = [](const std::string &name) {
                std::ifstream in("ffr-evidence/" + name + ".ppm", std::ios::binary);
                std::string magic;
                int width, height, max;
                in >> magic >> width >> height >> max;
                in.get();
                require(magic == "P6" && width == 1024 && height == 768, "Expected capture dimensions");
                std::vector<uint8_t> pixels(size_t(width) * height * 3);
                in.read(reinterpret_cast<char *>(pixels.data()), pixels.size());
                return pixels;
            };
            auto finePixels = loadCapture(baseline["name"]), coarsePixels = loadCapture(coarse["name"]);
            for (int eye = 0; eye < 2; ++eye) {
                auto plan = makeFFRPlan(runtime::original[eye], runtime::original[eye], FFRSettings{});
                int cx = eye * 512 + int(plan.cx * 512), cy = int(plan.cy * 768);
                for (int y = cy - 8; y < cy + 8; ++y)
                    for (int x = cx - 8; x < cx + 8; ++x)
                        for (int channel = 0; channel < 3; ++channel)
                            require(finePixels[(y * 1024 + x) * 3 + channel] ==
                                        coarsePixels[(y * 1024 + x) * 3 + channel],
                                    "FFR preserves full-quality central pixels");
            }
            coarse["centralPixelsMatchBaseline"] = true;
            results.push_back(baseline);
            results.push_back(coarse);
        }
        results.push_back(benchmark(gpu, 1, 1, true, 75, 10, "conservative"));
        results.push_back(benchmark(gpu, .85f, .7f, true, 50, 20, "aggressive"));
        layerIntegration(gpu, argv[1]);
        gpu.validate();
        updateJson(std::filesystem::current_path() / "ffr-evidence/benchmark.json", [&](Json &j) {
            j = {{"adapter", gpu.adapter},
                 {"d3d12DebugLayer", gpu.debug},
                 {"hardwareApplicationConfirmedByPipelineStatistics", true},
                 {"cases", results},
                 {"hubrisTested", false},
                 {"headsetRuntimeTested", false}};
        });
        std::cout << "PASS: actual D3D12 Tier 2 scene VRS, workload statistics, GPU timestamps, "
                     "reconstruction and layer cooperation\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return std::string(e.what()).starts_with("UNAVAILABLE:") ? 77 : 1;
    }
}
static XrSwapchain submittedFFR{};
static XrResult XRAPI_CALL ffrRuntimeEnd(XrSession, const XrFrameEndInfo *info) {
    require(info->layerCount == 1, "FFR fixture projection");
    auto p = reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0]);
    require(p->viewCount == 2, "FFR fixture stereo");
    submittedFFR = p->views[0].subImage.swapchain;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        require(same(p->views[eye].fov, runtime::original[eye]), "FFR retains runtime projection coverage");
        require(!runtime::chains.at(p->views[eye].subImage.swapchain).acquired,
                "FFR output released before submission");
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL ffrRuntimeGipa(XrInstance i, const char *name, PFN_xrVoidFunction *out) {
    if (!strcmp(name, "xrEndFrame")) {
        *out = reinterpret_cast<PFN_xrVoidFunction>(ffrRuntimeEnd);
        return XR_SUCCESS;
    }
    return runtime::gipa(i, name, out);
}
static void layerIntegration(RasterFixture &gpu, const wchar_t *dll) {
    auto dir = std::filesystem::current_path() / "test-data-ffr-layer";
    SetEnvironmentVariableW(L"FOV_DATA_DIR", dir.c_str());
    Profile profile;
    profile.horizontal = profile.vertical = 1;
    profile.ffr.enabled = true;
    updateJson(dir / "config.json", [&](Json &j) { j = {{"defaults", profileJson(profile)}}; });
    runtime::d3d12 = true;
    runtime::device12 = gpu.device;
    runtime::queue12 = gpu.queue;
    HMODULE module = LoadLibraryW(dll);
    require(module != nullptr, "FFR layer DLL");
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
            "FFR layer negotiation");
    XrApiLayerNextInfo next{XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO,
                            XR_API_LAYER_NEXT_INFO_STRUCT_VERSION, sizeof(XrApiLayerNextInfo)};
    strcpy_s(next.layerName, "XR_APILAYER_FOV_extension");
    next.nextGetInstanceProcAddr = ffrRuntimeGipa;
    next.nextCreateApiLayerInstance = runtime::createInstance;
    XrApiLayerCreateInfo layer{XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
                               XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(XrApiLayerCreateInfo)};
    layer.nextInfo = &next;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "FFR controlled scene fixture");
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    XrInstance instance{};
    require(XR_SUCCEEDED(request.createApiLayerInstance(&ci, &layer, &instance)), "FFR XR instance");
    auto g = request.getInstanceProcAddr;
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = gpu.device.Get();
    binding.queue = gpu.queue.Get();
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
    si.next = &binding;
    XrSession session{};
    require(XR_SUCCEEDED(proc<PFN_xrCreateSession>(g, instance, "xrCreateSession")(instance, &si, &session)),
            "FFR XR session");
    auto begin = proc<PFN_xrFovBeginD3D12FFR>(g, instance, "xrFovBeginD3D12FFR");
    auto end = proc<PFN_xrFovEndD3D12FFR>(g, instance, "xrFovEndD3D12FFR");
    Json evidence = Json::array();
    for (int test = 0; test < 5; ++test) {
        if (test == 4)
            for (auto &f : runtime::original)
                f = reduce(f, .7f, .8f);
        profile.horizontal = (test == 0 || test == 4) ? 1 : .8f;
        profile.vertical = test == 2 ? 1 : profile.horizontal;
        profile.edgeEnabled = test != 3;
        updateJson(dir / "config.json", [&](Json &j) { j["defaults"] = profileJson(profile); });
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = 1000000000 + test * 10000000;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count;
        require(XR_SUCCEEDED(proc<PFN_xrLocateViews>(g, instance, "xrLocateViews")(session, &locate, &state,
                                                                                   2, &count, views)),
                "FFR authoritative views");
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc.width = 1024;
        sc.height = 768;
        sc.arraySize = sc.sampleCount = sc.mipCount = sc.faceCount = 1;
        require(XR_SUCCEEDED(proc<PFN_xrCreateSwapchain>(g, instance, "xrCreateSwapchain")(session, &sc,
                                                                                           &runtime::source)),
                "FFR source chain");
        uint32_t index;
        XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        w.timeout = XR_INFINITE_DURATION;
        require(XR_SUCCEEDED(proc<PFN_xrAcquireSwapchainImage>(g, instance, "xrAcquireSwapchainImage")(
                    runtime::source, &a, &index)),
                "FFR acquire");
        require(XR_SUCCEEDED(
                    proc<PFN_xrWaitSwapchainImage>(g, instance, "xrWaitSwapchainImage")(runtime::source, &w)),
                "FFR wait");
        ComPtr<ID3D12Resource> image;
        check(runtime::chains.at(runtime::source).images[index].As(&image));
        gpu.openScene();
        uint64_t completion = ++gpu.serial;
        XrCompositionLayerProjectionView submission[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                       {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            FovD3D12FFRDrawInfo draw;
            draw.displayTime = locate.displayTime;
            draw.eye = eye;
            draw.slot = eye;
            draw.subImage = {runtime::source, {{int(eye * 512), 0}, {512, 768}}, 0};
            draw.commands = gpu.scene.Get();
            draw.completionFence = gpu.fence.Get();
            draw.completionValue = completion;
            if (eye == 0) {
                auto invalid = draw;
                invalid.displayTime -= 1;
                require(XR_FAILED(begin(session, &invalid)), "Reject unlocated frame");
            }
            require(XR_SUCCEEDED(begin(session, &draw)), "FFR scene scope from located projection");
            require(XR_FAILED(begin(session, &draw)), "Reject nested FFR scope");
            require(XR_FAILED(proc<PFN_xrDestroySession>(g, instance, "xrDestroySession")(session)),
                    "Reject active scene destruction");
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            require(XR_FAILED(proc<PFN_xrReleaseSwapchainImage>(g, instance, "xrReleaseSwapchainImage")(
                        runtime::source, &release)),
                    "Prevent active FFR state entering reconstruction");
            gpu.draw(image.Get(), draw.subImage.imageRect, eye);
            require(XR_SUCCEEDED(end(session, gpu.scene.Get())), "Restore 1x1 scene state");
            require(XR_FAILED(begin(session, &draw)), "Reject reuse before promised fence completion");
            submission[eye].fov = views[eye].fov;
            submission[eye].pose = views[eye].pose;
            submission[eye].subImage = draw.subImage;
        }
        // A draw after End must execute at 1x1, proving no coarse-state leakage.
        gpu.scene->BeginQuery(gpu.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
        gpu.draw(image.Get(), {{512, 0}, {64, 64}}, 1);
        gpu.scene->EndQuery(gpu.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
        gpu.scene->ResolveQueryData(gpu.statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1, 1,
                                    gpu.results.Get(), 256);
        gpu.submit(gpu.scene.Get());
        check(gpu.queue->Signal(gpu.fence.Get(), completion));
        gpu.wait();
        uint8_t *data;
        D3D12_RANGE range{256, 512};
        check(gpu.results->Map(0, &range, reinterpret_cast<void **>(&data)));
        auto stats = reinterpret_cast<D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(data + 256);
        require(stats->PSInvocations >= 64 * 64, "End restores full-rate draw");
        gpu.results->Unmap(0, nullptr);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        require(XR_SUCCEEDED(proc<PFN_xrReleaseSwapchainImage>(g, instance, "xrReleaseSwapchainImage")(
                    runtime::source, &release)),
                "FFR release and existing source snapshot");
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.viewCount = 2;
        projection.views = submission;
        auto header = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
        frame.displayTime = locate.displayTime;
        frame.layerCount = 1;
        frame.layers = &header;
        require(XR_SUCCEEDED(proc<PFN_xrEndFrame>(g, instance, "xrEndFrame")(session, &frame)),
                "FFR/reconstruction end frame");
        bool cropped = test == 1 || test == 2;
        require((submittedFFR != runtime::source) == cropped, "FFR alone never invokes reconstruction");
        ComPtr<ID3D12Resource> submittedImage;
        check(runtime::chains.at(submittedFFR).images[0].As(&submittedImage));
        gpu.capture(submittedImage.Get(), "layer-case-" + std::to_string(test));
        auto plan = makeFFRPlan(runtime::original[0], views[0].fov, profile.ffr);
        require((plan.mode == FFRZoneMode::TwoRenderedZonesWithCrop) == cropped,
                "Layer effective mode matches actual FOV");
        evidence.push_back({{"case", test},
                            {"plan", ffrPlanJson(plan)},
                            {"reconstructed", cropped},
                            {"restoredFullRateDraw", true}});
        require(
            XR_SUCCEEDED(proc<PFN_xrDestroySwapchain>(g, instance, "xrDestroySwapchain")(runtime::source)),
            "FFR source teardown");
    }
    require(XR_SUCCEEDED(proc<PFN_xrDestroySession>(g, instance, "xrDestroySession")(session)),
            "FFR completed session teardown");
    require(XR_SUCCEEDED(proc<PFN_xrDestroyInstance>(g, instance, "xrDestroyInstance")(instance)),
            "FFR instance teardown");
    FreeLibrary(module);
    updateJson(std::filesystem::current_path() / "ffr-evidence/layer.json", [&](Json &j) {
        j = {{"fixture", "developer-owned OpenXR runtime and real D3D12 rasterized scenes"},
             {"cases", evidence}};
    });
}
