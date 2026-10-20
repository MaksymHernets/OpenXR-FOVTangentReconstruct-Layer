#include "renderer/IEdgeRenderer.h"
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <vector>
#include <numbers>
using namespace fov;
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
using Pixels = std::vector<uint32_t>;
class Gpu {
    bool dx12;
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> c11;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UniqueHandle event;
    UINT64 serial = 0;
    std::vector<ComPtr<IUnknown>> keepAlive;
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        ComPtr<ID3D12Resource> resource;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                           IID_PPV_ARGS(&resource)));
        return resource;
    }
    void execute() {
        check(list->Close());
        ID3D12CommandList *commands = list.Get();
        queue->ExecuteCommandLists(1, &commands);
        check(queue->Signal(fence.Get(), ++serial));
        check(fence->SetEventOnCompletion(serial, event.get()));
        WaitForSingleObject(event.get(), INFINITE);
        check(allocator->Reset());
        check(list->Reset(allocator.Get(), nullptr));
    }
    void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    }

  public:
    std::unique_ptr<IEdgeRenderer> renderer;
    ComPtr<IUnknown> output;
    explicit Gpu(bool use12) : dx12(use12) {
        if (!dx12) {
            UINT flags = D3D11_CREATE_DEVICE_DEBUG;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                           D3D11_SDK_VERSION, &d11, nullptr, &c11);
            if (FAILED(hr))
                check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                        D3D11_SDK_VERSION, &d11, nullptr, &c11));
            renderer = makeD3D11(d11.Get());
        } else {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
            ComPtr<IDXGIFactory4> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
            check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)));
            D3D12_COMMAND_QUEUE_DESC desc{};
            check(d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
            check(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
            check(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list)));
            check(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
            event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            renderer = makeD3D12(d12.Get(), queue.Get());
        }
        renderer->setTiming(true);
        output = texture(128, 2, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr);
    }
    ComPtr<IUnknown> texture(UINT size, UINT slices, DXGI_FORMAT format, const uint32_t *data) {
        bool depth = depthFormat(format);
        ComPtr<IUnknown> result;
        if (!dx12) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = size;
            d.ArraySize = slices;
            d.MipLevels = 1;
            d.Format = format;
            d.SampleDesc.Count = 1;
            d.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
            std::vector<D3D11_SUBRESOURCE_DATA> initial(slices);
            for (UINT i = 0; i < slices; i++)
                initial[i] = {data ? data + i * size * size : nullptr, size * 4, 0};
            ComPtr<ID3D11Texture2D> image;
            check(d11->CreateTexture2D(&d, data ? initial.data() : nullptr, &image));
            result = image;
        } else {
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = d.Height = size;
            d.DepthOrArraySize = (UINT16)slices;
            d.MipLevels = 1;
            d.Format = format;
            d.SampleDesc.Count = 1;
            d.Flags =
                depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> image;
            check(d12->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &d,
                data ? D3D12_RESOURCE_STATE_COPY_DEST
                     : (depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET),
                nullptr, IID_PPV_ARGS(&image)));
            if (data) {
                auto upload = buffer(UINT64(size) * size * slices * 4, D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
                void *ptr = nullptr;
                check(upload->Map(0, nullptr, &ptr));
                memcpy(ptr, data, size_t(size) * size * slices * 4);
                upload->Unmap(0, nullptr);
                for (UINT i = 0; i < slices; i++) {
                    D3D12_TEXTURE_COPY_LOCATION from{};
                    from.pResource = upload.Get();
                    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    from.PlacedFootprint.Offset = UINT64(i) * size * size * 4;
                    from.PlacedFootprint.Footprint = {format, size, size, 1, size * 4};
                    D3D12_TEXTURE_COPY_LOCATION to{};
                    to.pResource = image.Get();
                    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    to.SubresourceIndex = i;
                    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                }
                barrier(image.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
                execute();
            }
            result = image;
        }
        keepAlive.push_back(result);
        return result;
    }
    void captureColor(uint64_t key, uint32_t left, uint32_t right) {
        Pixels pixels(64 * 64 * 2, left);
        std::fill(pixels.begin() + 4096, pixels.end(), right);
        auto image = texture(64, 2, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data());
        renderer->capture(key, image.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    void captureDepth(uint64_t key, float sample) {
        std::vector<float> pixels(64 * 64 * 2, sample);
        auto image = texture(64, 2, DXGI_FORMAT_D32_FLOAT, reinterpret_cast<const uint32_t *>(pixels.data()));
        renderer->capture(key, image.Get(), DXGI_FORMAT_D32_FLOAT);
    }
    void captureDepthFormat(uint64_t key, DXGI_FORMAT format, float sample) {
        auto image = texture(64, 2, format, nullptr);
        if (!dx12) {
            ComPtr<ID3D11Texture2D> texture11;
            check(image.As(&texture11));
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
            desc.Format = format;
            desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.ArraySize = 2;
            ComPtr<ID3D11DepthStencilView> view;
            check(d11->CreateDepthStencilView(texture11.Get(), &desc, &view));
            c11->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, sample, 0);
        } else {
            ComPtr<ID3D12Resource> texture12;
            check(image.As(&texture12));
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = 1;
            ComPtr<ID3D12DescriptorHeap> heap;
            check(d12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)));
            D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
            desc.Format = format;
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.ArraySize = 2;
            auto handle = heap->GetCPUDescriptorHandleForHeapStart();
            d12->CreateDepthStencilView(texture12.Get(), &desc, handle);
            list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, sample, 0, 0, nullptr);
            execute();
        }
        renderer->capture(key, image.Get(), format);
    }
    Pixels read(UINT slice) {
        Pixels pixels(128 * 128);
        if (!dx12) {
            ComPtr<ID3D11Texture2D> image;
            check(output.As(&image));
            D3D11_TEXTURE2D_DESC desc{};
            image->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            check(d11->CreateTexture2D(&desc, nullptr, &staging));
            c11->CopyResource(staging.Get(), image.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(c11->Map(staging.Get(), slice, D3D11_MAP_READ, 0, &mapped));
            for (UINT y = 0; y < 128; y++)
                memcpy(pixels.data() + y * 128, (BYTE *)mapped.pData + y * mapped.RowPitch, 512);
            c11->Unmap(staging.Get(), slice);
        } else {
            ComPtr<ID3D12Resource> image;
            check(output.As(&image));
            auto staging = buffer(128 * 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            barrier(image.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = image.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = slice;
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = staging.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128, 1, 512};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            barrier(image.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            execute();
            void *ptr = nullptr;
            check(staging->Map(0, nullptr, &ptr));
            memcpy(pixels.data(), ptr, pixels.size() * 4);
            staging->Unmap(0, nullptr);
        }
        return pixels;
    }
    void validation() {
        if (!dx12) {
            ComPtr<ID3D11InfoQueue> info;
            if (SUCCEEDED(d11.As(&info)))
                for (UINT64 i = 0; i < info->GetNumStoredMessages(); i++) {
                    SIZE_T size = 0;
                    info->GetMessage(i, nullptr, &size);
                    std::vector<BYTE> data(size);
                    auto message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
                    info->GetMessage(i, message, &size);
                    if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                        throw std::runtime_error(message->pDescription);
                }
        } else {
            ComPtr<ID3D12InfoQueue> info;
            if (SUCCEEDED(d12.As(&info)))
                for (UINT64 i = 0; i < info->GetNumStoredMessages(); i++) {
                    SIZE_T size = 0;
                    info->GetMessage(i, nullptr, &size);
                    std::vector<BYTE> data(size);
                    auto message = reinterpret_cast<D3D12_MESSAGE *>(data.data());
                    info->GetMessage(i, message, &size);
                    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                        throw std::runtime_error(message->pDescription);
                }
        }
    }
};
static TemporalFrame frame(XrTime time) {
    TemporalFrame f;
    f.time = time;
    f.pose.orientation.w = 1;
    f.original = {-std::atan(1.f), std::atan(1.f), std::atan(1.f), -std::atan(1.f)};
    f.reduced = reduce(f.original, .8f, .8f);
    f.width = f.height = 64;
    f.depthWidth = f.depthHeight = 64;
    f.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    f.trackingValid = true;
    return f;
}
static ShaderConstants shader(const TemporalFrame &f, UINT eye) {
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.fov = f.reduced;
    view.subImage.imageRect.extent = {64, 64};
    view.subImage.imageArrayIndex = eye;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.width = ci.height = 64;
    ci.format = f.format;
    Profile profile;
    profile.method = 5;
    return constants(view, f.original, ci, profile);
}
static void metadata() {
    DepthMapping standard{.2f, .9f, .1f, 100};
    require(standard.valid(), "Standard depth mapping");
    float t = (1.f - 10) / (.01f - 10);
    require(std::abs(standard.distance(.2f + t * .7f) - 1) < 1e-5f, "Window depth conversion");
    DepthMapping reversed{.2f, .9f, 100, .1f};
    require(reversed.valid() &&
                std::abs(reversed.distance(.2f + ((1 - .01f) / (10 - .01f)) * .7f) - 1) < 1e-5f,
            "Reversed Z conversion");
    DepthMapping infinite{0, 1, .1f, INFINITY};
    require(infinite.valid() && std::abs(infinite.distance(.9f) - 1) < 1e-5f, "Infinite far plane");
    TemporalHistory history;
    TemporalSettings settings;
    auto old = frame(1000000000);
    history.prepare(old, settings);
    history.writable();
    history.commit(old);
    auto current = frame(1010000000);
    history.prepare(current, settings);
    require(history.size() == 1, "History retained");
    current.reduced = reduce(current.original, .7f, .8f);
    history.prepare(current, settings);
    require(!history.size(), "FOV invalidation");
    history.writable();
    history.commit(current);
    current.time += 10000000;
    current.trackingValid = false;
    history.prepare(current, settings);
    require(!history.size(), "Tracking invalidation");
    settings.historyFrames = 2;
    for (int i = 0; i < 5; i++) {
        auto f = frame(2000000000 + i * 10000000);
        history.prepare(f, settings);
        history.writable();
        history.commit(f);
    }
    require(history.size() == 2, "Bounded history ring");
    current = frame(3000000000);
    history.prepare(current, settings);
    require(!history.size(), "Stale history rejection");
    auto profile = parseProfile(
        {{"edgeFillMethod", "temporalDepth"},
         {"temporalDepth",
          {{"historyFrames", 50}, {"confidenceThreshold", -1}, {"fallback", {{"blurRadius", 100}}}}}});
    require(profile.method == 5 && profile.temporal.historyFrames == 8 &&
                profile.temporal.confidenceThreshold == 0 && profile.temporal.fallbackBlurRadius == 32,
            "Temporal config validation");
    require(parseProfile(profileJson(profile)).temporal == profile.temporal, "Temporal settings round trip");
}
static void scenario(Gpu &gpu, UINT eye, int mode) {
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
    gpu.captureColor(1, 0xff1432c8, 0xffc83214);
    gpu.renderer->storeHistory(1, entry.colorKey, region);
    if (old.hasDepth) {
        float t = (1 / 1.f - 1 / old.depth.nearZ) / (1 / old.depth.farZ - 1 / old.depth.nearZ);
        float sample = mode == 5 ? old.depth.maxDepth : std::lerp(old.depth.minDepth, old.depth.maxDepth, t);
        if (mode >= 7) {
            auto format = mode == 7   ? DXGI_FORMAT_D16_UNORM
                          : mode == 8 ? DXGI_FORMAT_D24_UNORM_S8_UINT
                                      : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            gpu.renderer->forget(2);
            gpu.captureDepthFormat(2, format, sample);
        } else {
            gpu.renderer->forget(2);
            gpu.captureDepth(2, sample);
        }
        gpu.renderer->storeHistory(2, entry.depthKey, region);
    }
    history.commit(old);
    history.prepare(current, settings);
    gpu.captureColor(1, 0xff408040, 0xff408040);
    auto input = history.input(current, settings, 128, 128);
    gpu.renderer->setTemporal(input);
    auto c = shader(current, eye);
    Pixels result;
    for (int repeat = 0; repeat < 10; repeat++) {
        gpu.renderer->render(1, gpu.output.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, c, {{0, 0}, {128, 128}});
        result = gpu.read(eye);
    }
    require(result[64 * 128 + 64] == 0xff408040, "Temporal path preserves valid current pixels");
    for (auto pixel : result)
        require((pixel >> 24) == 255 && (pixel & 0xffffff) != 0, "Opaque nonblack fallback");
    auto edge = result[64 * 128 + 120];
    bool reconstruct = mode == 1 || mode == 3 || mode == 4 || mode >= 7;
    if (reconstruct) {
        int channel = eye ? 16 : 0;
        require(((edge >> channel) & 255) > 80, "Rotation/depth reprojection covers newly exposed periphery");
        auto metrics = gpu.renderer->temporalMetrics();
        require(metrics.available && metrics.reconstructedPercent > 0 && metrics.fallbackPercent > 0,
                "Real delayed GPU coverage metrics");
        require(metrics.depthUsed == (mode == 3 || mode == 4 || mode >= 7), "Depth/rotation diagnostic mode");
    } else
        require(edge == 0xff408040, "Unreliable/no coverage uses current boundary blur");
    gpu.validation();
    for (auto key : history.resourceKeys())
        gpu.renderer->forget(key);
}
static void streaming(Gpu &gpu) {
    TemporalHistory history;
    TemporalSettings settings;
    settings.historyFrames = 2;
    gpu.captureColor(1, 0xff408040, 0xff408040);
    for (int i = 0; i < 24; i++) {
        auto current = frame(2000000000 + i * 11000000);
        float angle = -i * .005f;
        current.pose.orientation = {0, std::sin(angle / 2), 0, std::cos(angle / 2)};
        history.prepare(current, settings);
        auto input = history.input(current, settings, 128, 128);
        gpu.renderer->setTemporal(input);
        gpu.renderer->render(1, gpu.output.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, shader(current, 0),
                             {{0, 0}, {128, 128}});
        auto &entry = history.writable();
        gpu.renderer->storeHistory(1, entry.colorKey, {XR_NULL_HANDLE, {{0, 0}, {64, 64}}, 0});
        history.commit(current);
    }
    require(history.size() == 2, "In-flight ring buffer reuse stays bounded");
    auto result = gpu.read(0);
    require(result[64 * 128 + 64] == 0xff408040, "In-flight reuse preserves current content");
    gpu.validation();
    for (auto key : history.resourceKeys())
        gpu.renderer->forget(key);
}
int main(int argc, char **) {
    try {
        bool dx12 = argc > 1;
        metadata();
        Gpu gpu(dx12);
        for (UINT eye = 0; eye < 2; eye++)
            for (int mode = 0; mode < 10; mode++)
                scenario(gpu, eye, mode);
        require(gpu.renderer->sourceCopyMilliseconds().has_value() &&
                    gpu.renderer->historyCopyMilliseconds().has_value(),
                "Separate GPU copy stage timestamps");
        streaming(gpu);
        std::cout << "PASS: " << (dx12 ? "D3D12" : "D3D11")
                  << " temporal history, rotation, translation/parallax, reversed Z, invalid depth/tracking, "
                     "independent eyes, fallback, GPU counters and validation\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
