#include "common/Platform.h"
#include "common/ConfigManager.h"
#include "common/FovMath.h"
#include "renderer/IEdgeRenderer.h"
#include <iostream>
#include <thread>
#include <array>
#include <fstream>
using namespace fov;
static void require(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
static void configuration() {
    auto dir = std::filesystem::current_path() / L"test-data-core";
    SetEnvironmentVariableW(L"FOV_DATA_DIR", dir.c_str());
    auto p = parseProfile({{"horizontalFovMultiplier", -2},
                           {"verticalFovMultiplier", "bad"},
                           {"edgeFill", {{"edgeSamples", 999}, {"blurRadius", -1}}}});
    require(p.horizontal == .5f && p.vertical == .8f && p.edgeSamples == 16 && p.blurRadius == 0,
            "Profile validation");
    auto solid = parseProfile({{"edgeFillMethod", "solid"}, {"edgeFill", {{"color", {-.5, .5, 2.}}}}});
    require(solid.method == 4 && solid.color == std::array<float, 3>{0, .5f, 1}, "Solid color validation");
    require(parseProfile(profileJson(solid)).color == solid.color, "Solid color persistence");
    require(parseProfile({{"edgeFillMethod", "solid"}}).color == std::array<float, 3>{0, 0, 0},
            "Solid default black");
    require(parseProfile({{"edgeFill", {{"color", "invalid"}}}}, solid).color == solid.color,
            "Invalid color inherits defaults");
    auto path = normalizePath("C:/Games/Example/../Example/Game.EXE");
    require(path == normalizePath("c:\\games\\example\\game.exe"), "Path normalization");
    auto file = dir / L"concurrent.json";
    updateJson(file, [](Json &j) { j["count"] = 0; });
    std::vector<std::jthread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back([file]() {
            for (int i = 0; i < 10; i++)
                updateJson(file, [](Json &j) { j["count"] = j.value("count", 0) + 1; });
        });
    threads.clear();
    require(loadJson(file)["count"] == 40, "Concurrent atomic updates");
    updateJson(dir / L"config.json", [&](Json &j) {
        j = {{"defaults", profileJson({})}, {"games", {{path, {{"horizontalFovMultiplier", .65}}}}}};
    });
    require(std::abs(loadProfile(path).horizontal - .65f) < 1e-6f, "Profile override");
    std::ofstream(dir / L"invalid.json") << "{broken";
    require(loadJson(dir / L"invalid.json").empty(), "Invalid JSON fallback");
}
static void math() {
    XrFovf original{-.9f, .65f, .8f, -.7f};
    auto r = reduce(original, .7f, .8f);
    require(std::abs(std::tan(r.angleLeft) / std::tan(original.angleLeft) - .7f) < 1e-6f,
            "Tangent FOV reduction");
    float s[2], b[2];
    uvMapping(original, r, s, b);
    require(std::abs(s[0] - 1 / .7f) < 1e-5f && std::abs(s[1] - 1 / .8f) < 1e-5f, "FOV scale");
    float center =
        (0 - std::tan(original.angleLeft)) / (std::tan(original.angleRight) - std::tan(original.angleLeft));
    float reducedCenter = (0 - std::tan(r.angleLeft)) / (std::tan(r.angleRight) - std::tan(r.angleLeft));
    require(std::abs(center * s[0] + b[0] - reducedCenter) < 1e-5f, "Asymmetric optical center");
}
using Pixels = std::vector<uint32_t>;
static ShaderConstants settings(int method, DXGI_FORMAT format) {
    ShaderConstants c{};
    c.scale[0] = 1.5f;
    c.scale[1] = 1.4f;
    c.bias[0] = -.3f;
    c.bias[1] = -.1f;
    c.rect[2] = c.rect[3] = 1;
    c.texel[0] = c.texel[1] = 1 / 16.f;
    c.slice = 1;
    c.method = std::min(method, 4);
    Profile profile;
    if (method == 5)
        profile.color = {.25f, .5f, .75f};
    setFillColor(c, profile, format);
    c.stretchWidth = .15f;
    c.gradientStrength = .7f;
    c.blurRadius = 3;
    c.blurStrength = .5f;
    c.edgeSamples = 8;
    return c;
}
static Pixels sourcePixels() {
    Pixels p(16 * 16 * 2);
    for (int z = 0; z < 2; z++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                p[z * 256 + y * 16 + x] =
                    0xff000000u | (uint32_t(z ? 190 : 40) << 16) | (uint32_t(y * 15) << 8) | uint32_t(x * 15);
    return p;
}
static void inspect(const Pixels &p, int method) {
    for (size_t i = 0; i < p.size(); i++) {
        auto v = p[i];
        require((v >> 24) == 255, "GPU output alpha");
        float x = ((i % 32) + .5f) / 32 * 1.5f - .3f, y = ((i / 32) + .5f) / 32 * 1.4f - .1f;
        if (method >= 4 && (x < 0 || x > 1 || y < 0 || y > 1)) {
            uint32_t expected = method == 4 ? 0xff000000u : 0xffbf8040u;
            for (int channel = 0; channel < 3; channel++)
                require(std::abs(int((v >> (channel * 8)) & 255) - int((expected >> (channel * 8)) & 255)) <=
                            1,
                        "Exact solid edge color");
            continue;
        }
        require(((v >> 16) & 255) == 190, "Correct eye array slice");
        require((v & 255) <= 225 && ((v >> 8) & 255) <= 225, "Bounded edge colors");
    }
    require((p[16 * 32 + 16] & 255) > 50, "Valid center preserved");
}
static std::array<Pixels, 6> d3d11(DXGI_FORMAT format) {
    ComPtr<ID3D11Device> d;
    ComPtr<ID3D11DeviceContext> ctx;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                            nullptr, &ctx));
    auto renderer = makeD3D11(d.Get());
    renderer->setTiming(true);
    D3D11_VIEWPORT originalViewport{3, 4, 19, 20, 0, 1};
    ctx->RSSetViewports(1, &originalViewport);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    auto p = sourcePixels();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 2;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data[2]{{p.data(), 64, 0}, {p.data() + 256, 64, 0}};
    ComPtr<ID3D11Texture2D> source, out, read;
    check(d->CreateTexture2D(&desc, data, &source));
    desc.Width = desc.Height = 32;
    check(d->CreateTexture2D(&desc, nullptr, &out));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;
    check(d->CreateTexture2D(&desc, nullptr, &read));
    renderer->capture(1, source.Get(), desc.Format);
    std::array<Pixels, 6> results;
    for (int iteration = 0; iteration < 18; iteration++) {
        int m = iteration % 6;
        renderer->capture(1, source.Get(), desc.Format);
        renderer->render(1, out.Get(), desc.Format, settings(m, format), {{0, 0}, {32, 32}});
        D3D11_VIEWPORT restored{};
        UINT viewportCount = 1;
        ctx->RSGetViewports(&viewportCount, &restored);
        D3D11_PRIMITIVE_TOPOLOGY topology;
        ctx->IAGetPrimitiveTopology(&topology);
        require(restored.TopLeftX == 3 && restored.TopLeftY == 4 && restored.Width == 19 &&
                    topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
                "D3D11 application state restored");
        ctx->CopyResource(read.Get(), out.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        check(ctx->Map(read.Get(), 1, D3D11_MAP_READ, 0, &mapped));
        auto &r = results[m];
        r.resize(1024);
        for (int y = 0; y < 32; y++)
            memcpy(r.data() + y * 32, (BYTE *)mapped.pData + y * mapped.RowPitch, 128);
        ctx->Unmap(read.Get(), 1);
        inspect(r, m);
        if (m >= 4)
            for (size_t i = 0; i < r.size(); i++) {
                float x = ((i % 32) + .5f) / 32 * 1.5f - .3f, y = ((i / 32) + .5f) / 32 * 1.4f - .1f;
                if (x >= 0 && x <= 1 && y >= 0 && y <= 1)
                    require(r[i] == results[0][i], "Solid fill preserves original center pixels");
            }
        if (m == 0)
            require((r.front() & 0xffff) == 0, "Clamp corner uses nearest boundary pixel");
    }
    require(renderer->gpuMilliseconds().has_value(), "D3D11 timestamp measurement");
    renderer->forget(1);
    return results;
}
static void d3d12(const std::array<Pixels, 6> &reference, DXGI_FORMAT format) {
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> d;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d)));
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> q;
    check(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    auto renderer = makeD3D12(d.Get(), q.Get());
    renderer->setTiming(true);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                               IID_PPV_ARGS(&list)));
    ComPtr<ID3D12Fence> fence;
    check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 serial = 0;
    auto execute = [&]() {
        check(list->Close());
        ID3D12CommandList *l = list.Get();
        q->ExecuteCommandLists(1, &l);
        check(q->Signal(fence.Get(), ++serial));
        check(fence->SetEventOnCompletion(serial, event));
        WaitForSingleObject(event, INFINITE);
        check(allocator->Reset());
        check(list->Reset(allocator.Get(), nullptr));
    };
    auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    };
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = 16;
    td.DepthOrArraySize = 2;
    td.MipLevels = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> source, out;
    check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&source)));
    td.Width = td.Height = 32;
    check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     nullptr, IID_PPV_ARGS(&out)));
    auto buffer = [&](D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state) {
        ComPtr<ID3D12Resource> r;
        D3D12_HEAP_PROPERTIES h{};
        h.Type = type;
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = bytes;
        b.Height = 1;
        b.DepthOrArraySize = b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &b, state, nullptr, IID_PPV_ARGS(&r)));
        return r;
    };
    auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, 8192, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto read = buffer(D3D12_HEAP_TYPE_READBACK, 8192, D3D12_RESOURCE_STATE_COPY_DEST);
    void *ptr = nullptr;
    check(upload->Map(0, nullptr, &ptr));
    auto pixels = sourcePixels();
    for (int z = 0; z < 2; z++)
        for (int y = 0; y < 16; y++)
            memcpy((BYTE *)ptr + z * 4096 + y * 256, pixels.data() + z * 256 + y * 16, 64);
    upload->Unmap(0, nullptr);
    for (int z = 0; z < 2; z++) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = z * 4096;
        src.PlacedFootprint.Footprint = {td.Format, 16, 16, 1, 256};
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = source.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = z;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    execute();
    renderer->capture(1, source.Get(), td.Format);
    for (int iteration = 0; iteration < 18; iteration++) {
        int m = iteration % 6;
        renderer->capture(1, source.Get(), td.Format);
        renderer->render(1, out.Get(), td.Format, settings(m, format), {{0, 0}, {32, 32}});
        barrier(out.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = out.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 1;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = read.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {td.Format, 32, 32, 1, 256};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        execute();
        D3D12_RANGE range{0, 8192};
        check(read->Map(0, &range, &ptr));
        Pixels result(1024);
        for (int y = 0; y < 32; y++)
            memcpy(result.data() + y * 32, (BYTE *)ptr + y * 256, 128);
        read->Unmap(0, nullptr);
        inspect(result, m);
        for (int i = 0; i < 1024; i++)
            for (int channel = 0; channel < 4; channel++)
                require(std::abs(int((result[i] >> (8 * channel)) & 255) -
                                 int((reference[m][i] >> (8 * channel)) & 255)) <= 1,
                        "D3D11/D3D12 shader parity");
    }
    require(renderer->gpuMilliseconds().has_value(), "D3D12 timestamp measurement");
    renderer->forget(1);
    CloseHandle(event);
}
int main() {
    try {
        configuration();
        math();
        for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
            auto reference = d3d11(format);
            d3d12(reference, format);
        }
        std::cout << "PASS: configuration, concurrency, asymmetric FOV, all five algorithms on D3D11 and "
                     "D3D12 WARP, pixel parity\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
