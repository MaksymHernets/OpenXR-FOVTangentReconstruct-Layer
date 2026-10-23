#include "D3D11FFR.h"
#include <nvapi.h>
#include <array>
namespace fov {
namespace {
struct NvApi {
    // Loaded only from System32. Windows owns the module until process exit;
    // never unload global NVAPI state used by the host or another API layer.
    HMODULE module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Query = void *(__cdecl *)(unsigned);
    Query query = module ? reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface")) : nullptr;
    template <class T> T get(unsigned id) {
        return query ? reinterpret_cast<T>(query(id)) : nullptr;
    }
    decltype(&NvAPI_Initialize) initialize = get<decltype(initialize)>(0x0150e828);
    decltype(&NvAPI_D3D1x_GetGraphicsCapabilities) caps = get<decltype(caps)>(0x52b1499a);
    decltype(&NvAPI_D3D11_RSSetViewportsPixelShadingRates) rates = get<decltype(rates)>(0x34f7938f);
    decltype(&NvAPI_D3D11_CreateShadingRateResourceView) create = get<decltype(create)>(0x99ca2dff);
    decltype(&NvAPI_D3D11_RSSetShadingRateResourceView) bind = get<decltype(bind)>(0x1b0c2f83);
    bool ready = initialize && caps && rates && create && bind && initialize() == NVAPI_OK;
};
NvApi &nv() {
    static NvApi api;
    return api;
}
void checkNv(NvAPI_Status status) {
    if (status != NVAPI_OK)
        throw std::runtime_error("NVAPI VRS failed: " + std::to_string(int(status)));
}
} // namespace
struct D3D11FFR::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> map;
    ComPtr<ID3D11NvShadingRateResourceView> view;
    std::vector<uint8_t> cached;
    uint32_t width = 0, height = 0;
    bool supported = false, recording = false;
};
D3D11FFR::D3D11FFR(ID3D11Device *device) : impl(std::make_unique<Impl>()) {
    if (!device)
        throw std::runtime_error("Missing D3D11 device");
    impl->device = device;
    NV_D3D1x_GRAPHICS_CAPS caps{};
    impl->supported = nv().ready && nv().caps(device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps) == NVAPI_OK &&
                      caps.bVariablePixelRateShadingSupported;
}
D3D11FFR::~D3D11FFR() {
    if (impl->recording) {
        try {
            end(impl->context.Get());
        } catch (...) { /* Device loss: no further application work. */
        }
    }
}
bool D3D11FFR::active() const {
    return impl->recording;
}
Json D3D11FFR::capabilities() const {
    return {{"backend", "D3D11 NVIDIA NVAPI VRS"},
            {"hardwareSupported", impl->supported},
            {"sceneIntegrationSupported", impl->supported},
            {"integrationRequired", true},
            {"tileSize", 16},
            {"supportedRates", impl->supported ? Json::array({"1x1", "1x2", "2x1", "2x2"}) : Json::array()},
            {"reason", impl->supported ? "Explicit immediate-context scene cooperation required"
                                       : "NVAPI variable pixel shading unavailable"}};
}
Json D3D11FFR::begin(ID3D11DeviceContext *context, const FFRPlan &plan, uint32_t width, uint32_t height,
                     XrRect2Di viewport) {
    if (!impl->supported)
        throw std::runtime_error("NVAPI VRS unavailable");
    if (impl->recording || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !plan.settings.enabled)
        throw std::runtime_error("Invalid or nested D3D11 scene scope");
    ComPtr<ID3D11Device> owner;
    context->GetDevice(&owner);
    if (owner.Get() != impl->device.Get())
        throw std::runtime_error("Foreign D3D11 context");
    auto map = ffrRateMap(plan, width, height, 16, viewport, false);
    // Indices into our NVAPI table deliberately match D3D12 encodings. No cull rate.
    uint32_t cols = (width + 15) / 16, rows = (height + 15) / 16;
    bool allocated = false;
    if (cols != impl->width || rows != impl->height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = cols;
        desc.Height = rows;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UINT;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> image;
        check(impl->device->CreateTexture2D(&desc, nullptr, &image));
        NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
        vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
        vd.Format = desc.Format;
        vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11NvShadingRateResourceView> view;
        checkNv(nv().create(impl->device.Get(), image.Get(), &vd, &view));
        impl->map = image;
        impl->view = view;
        impl->width = cols;
        impl->height = rows;
        impl->cached.clear();
        allocated = true;
    }
    auto result = ffrPlanJson(plan);
    result["recordedRates"] = {"1x1", ffrRateName(ffrRate(plan.middleQuality, plan.settings.policy, false))};
    result["hardwareApplicationConfirmed"] = false;
    result["mapAllocated"] = allocated;
    result["state"] = "Immediate-context VRS bound; GPU execution not observed by layer";
    if (map != impl->cached) {
        context->UpdateSubresource(impl->map.Get(), 0, nullptr, map.data(), cols, 0);
        impl->cached = map;
    }
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC table{};
    table.enableVariablePixelShadingRate = true;
    for (auto &rate : table.shadingRateTable)
        rate = NV_PIXEL_X1_PER_RASTER_PIXEL;
    table.shadingRateTable[1] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
    table.shadingRateTable[4] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
    table.shadingRateTable[5] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC desc{};
    desc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    desc.numViewports = 1;
    desc.pViewports = &table;
    checkNv(nv().bind(context, impl->view.Get()));
    impl->context = context;
    impl->recording = true;
    auto status = nv().rates(context, &desc);
    if (status != NVAPI_OK) {
        end(context);
        checkNv(status);
    }
    return result;
}
void D3D11FFR::end(ID3D11DeviceContext *context) {
    if (!impl->recording || context != impl->context.Get())
        throw std::runtime_error("D3D11 FFR scope mismatch");
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC disabled{};
    disabled.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    // Attempt both restoration operations even if the device fails one.
    auto rates = nv().rates(context, &disabled);
    auto image = nv().bind(context, nullptr);
    checkNv(rates);
    checkNv(image);
    impl->recording = false;
    impl->context.Reset();
}
} // namespace fov

#include "D3D11SceneInterceptor.h"
#include <MinHook.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
namespace fov {
namespace {
thread_local unsigned suppress11 = 0;
}
D3D11SceneInterceptor::Suppress::Suppress() {
    ++suppress11;
}
D3D11SceneInterceptor::Suppress::~Suppress() {
    --suppress11;
}
struct D3D11SceneInterceptor::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    mutable std::recursive_mutex mutex;
    bool enabled = false, stopped = false, foreignImage = false, foreignRates = false;
    bool nvStateKnown = true, targetValid = false;
    uint32_t width = 0, height = 0;
    UINT viewportCount = 0;
    D3D11_VIEWPORT viewport{};
    std::array<FFRPlan, 2> plans;
    std::vector<std::pair<uint32_t, uint32_t>> targets;
    std::string signature, reason = "Waiting for observed projection targets";
    uint64_t generation = 0, builds = 0, recorded = 0, executed = 0, foreignSkipped = 0;
    uint64_t candidates = 0, deferredSkipped = 0, observedDraws = 0;
    std::string targetKey = "No RTV";
    std::map<std::string, uint64_t> observedTargets;
    struct Map {
        uint64_t generation;
        uint32_t width, height;
        XrRect2Di rect;
        bool packed;
        ComPtr<ID3D11Texture2D> image;
        ComPtr<ID3D11NvShadingRateResourceView> view;
    };
    std::vector<Map> maps;
    struct Marker {
        ComPtr<ID3D11Query> query;
        uint64_t draws = 0;
    };
    std::array<Marker, 16> markers;
    inline static std::mutex lifecycle, router, callbackMutex;
    inline static std::condition_variable drained;
    inline static std::atomic<unsigned> callbacks{0};
    inline static std::weak_ptr<Impl> instance;
    inline static std::vector<void *> hooks;
    struct Callback {
        Callback() {
            ++callbacks;
        }
        ~Callback() {
            std::lock_guard lock(callbackMutex);
            if (--callbacks == 0)
                drained.notify_all();
        }
    };
    using DrawFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
    using IndexedFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
    using InstancedFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
    using IndexedInstancedFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
    using AutoFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *);
    using IndirectFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
    using OmFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                           ID3D11DepthStencilView *);
    using OmUavFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                              ID3D11DepthStencilView *, UINT, UINT,
                                              ID3D11UnorderedAccessView *const *, const UINT *);
    using ViewFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
    using ClearFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *);
    using ExecuteFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
    inline static DrawFn originalDraw;
    inline static IndexedFn originalIndexed;
    inline static InstancedFn originalInstanced;
    inline static IndexedInstancedFn originalIndexedInstanced;
    inline static AutoFn originalAuto;
    inline static IndirectFn originalIndirect, originalIndexedIndirect;
    inline static OmFn originalOm;
    inline static OmUavFn originalOmUav;
    inline static ViewFn originalView;
    inline static ClearFn originalClear;
    inline static ExecuteFn originalExecute;
    inline static decltype(nv().rates) originalRates;
    inline static decltype(nv().bind) originalBind;
    static std::shared_ptr<Impl> owner(IUnknown *ctx) {
        if (suppress11)
            return {};
        std::lock_guard lock(router);
        auto p = instance.lock();
        return p && static_cast<IUnknown *>(p->context.Get()) == ctx ? p : nullptr;
    }
    // Called on the application's immediate-context thread only, never IPC/XR
    // diagnostics threads. No waits, Flush, or forced GPU submission.
    void poll() {
        for (auto &m : markers)
            if (m.draws) {
                BOOL done = FALSE;
                auto hr =
                    context->GetData(m.query.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr == S_OK && done) {
                    executed = std::max(executed, m.draws);
                    m.draws = 0;
                } else if (FAILED(hr)) {
                    enabled = false;
                    reason = "D3D11 execution query failed/device lost";
                }
            }
    }
    void mark() {
        if (recorded > 4 && recorded % 128)
            return;
        for (auto &m : markers)
            if (!m.draws) {
                context->End(m.query.Get());
                m.draws = recorded;
                break;
            }
    }
    void observeTarget() {
        targetValid = false;
        targetKey = "No RTV";
        ID3D11RenderTargetView *raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw, nullptr);
        bool found = false, valid = true;
        for (auto r : raw)
            if (r) {
                ComPtr<ID3D11RenderTargetView> view;
                view.Attach(r);
                ComPtr<ID3D11Resource> resource;
                view->GetResource(&resource);
                ComPtr<ID3D11Texture2D> image;
                if (FAILED(resource.As(&image))) {
                    valid = false;
                    continue;
                }
                D3D11_TEXTURE2D_DESC d{};
                image->GetDesc(&d);
                D3D11_RENDER_TARGET_VIEW_DESC v{};
                view->GetDesc(&v);
                targetKey = std::to_string(d.Width) + "x" + std::to_string(d.Height) +
                            " samples=" + std::to_string(d.SampleDesc.Count) +
                            " mips=" + std::to_string(d.MipLevels) + " array=" + std::to_string(d.ArraySize) +
                            " view=" + std::to_string(v.ViewDimension) + " slices=" +
                            std::to_string(v.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY
                                               ? v.Texture2DArray.ArraySize
                                               : 1);
                bool layout =
                    (v.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && v.Texture2D.MipSlice == 0 &&
                     d.ArraySize == 1) ||
                    (v.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY &&
                     v.Texture2DArray.MipSlice == 0 && v.Texture2DArray.ArraySize >= 1 &&
                     v.Texture2DArray.FirstArraySlice + v.Texture2DArray.ArraySize <= d.ArraySize) ||
                    (v.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS && d.ArraySize == 1) ||
                    (v.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY &&
                     v.Texture2DMSArray.ArraySize >= 1 &&
                     v.Texture2DMSArray.FirstArraySlice + v.Texture2DMSArray.ArraySize <= d.ArraySize);
                const auto samples = d.SampleDesc.Count;
                if (!layout || d.ArraySize > 2 || d.MipLevels != 1 ||
                    (samples != 1 && samples != 2 && samples != 4 && samples != 8))
                    valid = false;
                if (found && (width != d.Width || height != d.Height))
                    valid = false;
                width = d.Width;
                height = d.Height;
                found = true;
            }
        targetValid = found && valid;
    }
    void observeViewport() {
        viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        context->RSGetViewports(&viewportCount, v);
        if (viewportCount == 1)
            viewport = v[0];
    }
    bool candidate(bool &packed) const {
        if (!targetValid || width < 128 || height < 128)
            return false;
        for (auto [w, h] : targets)
            for (int halves : {1, 2}) {
                float sx = float(width) / (w * halves), sy = float(height) / h;
                if (sx >= .32f && sx <= 1.1f && std::abs(sx / sy - 1) < .01f) {
                    packed = halves == 2;
                    return true;
                }
            }
        return false;
    }
    Map *map(XrRect2Di rect, bool packed) {
        for (auto &m : maps)
            if (m.generation == generation && m.width == width && m.height == height && m.packed == packed &&
                m.rect.offset.x == rect.offset.x && m.rect.offset.y == rect.offset.y &&
                m.rect.extent.width == rect.extent.width && m.rect.extent.height == rect.extent.height)
                return &m;
        if (maps.size() >= 64) {
            reason = "Immutable map cache budget reached";
            return nullptr;
        }
        auto bytes = ffrRateMap(plans[0], width, height, 16, rect, false);
        auto other = ffrRateMap(plans[1], width, height, 16, rect, false);
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = std::min(bytes[i], other[i]);
        if (packed && rect.offset.x == 0 && rect.extent.width == int32_t(width)) {
            std::fill(bytes.begin(), bytes.end(), 0);
            for (int eye = 0; eye < 2; ++eye) {
                auto half =
                    ffrRateMap(plans[eye], width, height, 16,
                               {{int32_t(eye * width / 2), 0}, {int32_t(width / 2), int32_t(height)}}, false);
                for (size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = std::max(bytes[i], half[i]);
            }
        }
        ++builds;
        Map m{generation, width, height, rect, packed, {}, {}};
        if (std::any_of(bytes.begin(), bytes.end(), [](auto b) { return b != 0; })) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = (width + 15) / 16;
            d.Height = (height + 15) / 16;
            d.ArraySize = d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R8_UINT;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{bytes.data(), d.Width, 0};
            check(device->CreateTexture2D(&d, &data, &m.image));
            NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
            vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
            vd.Format = d.Format;
            vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
            checkNv(nv().create(device.Get(), m.image.Get(), &vd, &m.view));
        }
        maps.push_back(std::move(m));
        return &maps.back();
    }
    bool prepare(Map &keepAlive) {
        if (!enabled || stopped || !nvStateKnown || viewportCount != 1)
            return false;
        if (foreignImage || foreignRates) {
            ++foreignSkipped;
            reason = "Foreign NVAPI state observed; automatic override skipped";
            return false;
        }
        bool packed = false;
        if (!candidate(packed))
            return false;
        ++candidates;
        auto v = viewport;
        if (!std::isfinite(v.TopLeftX) || !std::isfinite(v.TopLeftY) || !std::isfinite(v.Width) ||
            !std::isfinite(v.Height) || v.TopLeftX < 0 || v.TopLeftY != 0 || v.Width <= 0 ||
            v.Height != float(height) || v.TopLeftX + v.Width > width)
            return false;
        XrRect2Di rect{{int32_t(v.TopLeftX), 0}, {int32_t(v.Width), int32_t(v.Height)}};
        if (float(rect.offset.x) != v.TopLeftX || float(rect.extent.width) != v.Width)
            return false;
        if (!packed && (rect.offset.x != 0 || rect.extent.width != int32_t(width)))
            return false;
        if (packed && !((rect.offset.x == 0 && rect.extent.width == int32_t(width)) ||
                        (rect.extent.width == int32_t(width / 2) &&
                         (rect.offset.x == 0 || rect.offset.x == int32_t(width / 2)))))
            return false;
        auto selected = map(rect, packed);
        if (!selected || !selected->view) {
            reason = "No coarse shading tiles for this target/profile";
            return false;
        }
        keepAlive = *selected; // Retain both resources across concurrent profile changes.
        NV_D3D11_VIEWPORT_SHADING_RATE_DESC table{};
        table.enableVariablePixelShadingRate = true;
        for (auto &r : table.shadingRateTable)
            r = NV_PIXEL_X1_PER_RASTER_PIXEL;
        table.shadingRateTable[1] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
        table.shadingRateTable[4] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
        table.shadingRateTable[5] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
        NV_D3D11_VIEWPORTS_SHADING_RATE_DESC rates{};
        rates.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
        rates.numViewports = 1;
        rates.pViewports = &table;
        checkNv(originalBind(context.Get(), keepAlive.view.Get()));
        auto result = originalRates(context.Get(), &rates);
        if (result != NVAPI_OK) {
            restore();
            checkNv(result);
        }
        return true;
    }
    void restore() {
        NV_D3D11_VIEWPORTS_SHADING_RATE_DESC disabled{};
        disabled.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
        auto a = originalRates(context.Get(), &disabled), b = originalBind(context.Get(), nullptr);
        if (a != NVAPI_OK || b != NVAPI_OK) {
            enabled = false;
            nvStateKnown = false;
            reason = "NVAPI restoration failed; automatic FFR disabled";
        }
    }
    template <class F> static void around(ID3D11DeviceContext *c, F draw) {
        auto p = owner(c);
        if (!p) {
            draw();
            return;
        }
        Suppress guard;
        bool bound = false;
        Map keepAlive{};
        {
            std::lock_guard lock(p->mutex);
            try {
                ++p->observedDraws;
                if (p->observedTargets.size() < 32 || p->observedTargets.contains(p->targetKey))
                    ++p->observedTargets[p->targetKey];
                p->poll();
                bound = p->prepare(keepAlive);
            } catch (const std::exception &e) {
                p->reason = e.what();
            } catch (...) {
            }
        }
        draw();
        if (bound) {
            std::lock_guard lock(p->mutex);
            p->restore();
            ++p->recorded;
            p->mark();
            if (p->nvStateKnown)
                p->reason = "Automatic D3D11 NVAPI scene draws recorded";
        }
    }
    static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT a, UINT b) {
        Callback cb;
        around(c, [&] { originalDraw(c, a, b); });
    }
    static void STDMETHODCALLTYPE indexed(ID3D11DeviceContext *c, UINT a, UINT b, INT d) {
        Callback cb;
        around(c, [&] { originalIndexed(c, a, b, d); });
    }
    static void STDMETHODCALLTYPE instanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, UINT e) {
        Callback cb;
        around(c, [&] { originalInstanced(c, a, b, d, e); });
    }
    static void STDMETHODCALLTYPE indexedInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, INT e,
                                                   UINT f) {
        Callback cb;
        around(c, [&] { originalIndexedInstanced(c, a, b, d, e, f); });
    }
    static void STDMETHODCALLTYPE autoDraw(ID3D11DeviceContext *c) {
        Callback cb;
        around(c, [&] { originalAuto(c); });
    }
    static void STDMETHODCALLTYPE indirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT o) {
        Callback cb;
        around(c, [&] { originalIndirect(c, b, o); });
    }
    static void STDMETHODCALLTYPE indexedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT o) {
        Callback cb;
        around(c, [&] { originalIndexedIndirect(c, b, o); });
    }
    static void STDMETHODCALLTYPE om(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *r,
                                     ID3D11DepthStencilView *d) {
        Callback cb;
        originalOm(c, n, r, d);
        if (auto p = owner(c)) {
            std::lock_guard lock(p->mutex);
            p->observeTarget();
        }
    }
    static void STDMETHODCALLTYPE omUav(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *r,
                                        ID3D11DepthStencilView *d, UINT start, UINT count,
                                        ID3D11UnorderedAccessView *const *u, const UINT *init) {
        Callback cb;
        originalOmUav(c, n, r, d, start, count, u, init);
        if (auto p = owner(c)) {
            std::lock_guard lock(p->mutex);
            p->observeTarget();
        }
    }
    static void STDMETHODCALLTYPE viewports(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v) {
        Callback cb;
        originalView(c, n, v);
        if (auto p = owner(c)) {
            std::lock_guard lock(p->mutex);
            p->viewportCount = n;
            if (n == 1 && v)
                p->viewport = v[0];
        }
    }
    static void STDMETHODCALLTYPE clear(ID3D11DeviceContext *c) {
        Callback cb;
        originalClear(c);
        if (auto p = owner(c)) {
            std::lock_guard lock(p->mutex);
            p->targetValid = false;
            p->viewportCount = 0; /* NVAPI extension state is not assumed reset by ClearState. */
        }
    }
    static void STDMETHODCALLTYPE execute(ID3D11DeviceContext *c, ID3D11CommandList *l, BOOL restore) {
        Callback cb;
        originalExecute(c, l, restore);
        if (auto p = owner(c)) {
            std::lock_guard lock(p->mutex);
            ++p->deferredSkipped;
            p->nvStateKnown = false;
            p->reason = "Deferred command list executed; NVAPI state unknown, automatic path suspended";
            p->observeTarget();
            p->observeViewport();
        }
    }
    static NvAPI_Status __cdecl rates(IUnknown *c, NV_D3D11_VIEWPORTS_SHADING_RATE_DESC *d) {
        Callback cb;
        auto result = originalRates(c, d);
        if (result == NVAPI_OK)
            if (auto p = owner(c)) {
                std::lock_guard lock(p->mutex);
                p->foreignRates =
                    d && d->numViewports > 0; /* A conservative refusal also covers nonstandard tables. */
            }
        return result;
    }
    static NvAPI_Status __cdecl bind(IUnknown *c, ID3D11NvShadingRateResourceView *v) {
        Callback cb;
        auto result = originalBind(c, v);
        if (result == NVAPI_OK)
            if (auto p = owner(c)) {
                std::lock_guard lock(p->mutex);
                p->foreignImage = v != nullptr;
            }
        return result;
    }
    template <class T> static void attach(void *address, void *replacement, T &original) {
        if (MH_CreateHook(address, replacement, reinterpret_cast<void **>(&original)) != MH_OK)
            throw std::runtime_error("Cannot install D3D11/NVAPI scene hook");
        hooks.push_back(address);
    }
    void install() {
        auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            throw std::runtime_error("MinHook initialization failed");
        auto v = *reinterpret_cast<void ***>(context.Get());
        hooks.reserve(14);
        try {
            attach(v[12], reinterpret_cast<void *>(indexed), originalIndexed);
            attach(v[13], reinterpret_cast<void *>(draw), originalDraw);
            attach(v[20], reinterpret_cast<void *>(indexedInstanced), originalIndexedInstanced);
            attach(v[21], reinterpret_cast<void *>(instanced), originalInstanced);
            attach(v[38], reinterpret_cast<void *>(autoDraw), originalAuto);
            attach(v[39], reinterpret_cast<void *>(indexedIndirect), originalIndexedIndirect);
            attach(v[40], reinterpret_cast<void *>(indirect), originalIndirect);
            attach(v[33], reinterpret_cast<void *>(om), originalOm);
            attach(v[34], reinterpret_cast<void *>(omUav), originalOmUav);
            attach(v[44], reinterpret_cast<void *>(viewports), originalView);
            attach(v[110], reinterpret_cast<void *>(clear), originalClear);
            attach(v[58], reinterpret_cast<void *>(execute), originalExecute);
            attach(reinterpret_cast<void *>(nv().rates), reinterpret_cast<void *>(rates), originalRates);
            attach(reinterpret_cast<void *>(nv().bind), reinterpret_cast<void *>(bind), originalBind);
            for (auto h : hooks)
                if (MH_QueueEnableHook(h) != MH_OK)
                    throw std::runtime_error("D3D11 hook enable failed");
            if (MH_ApplyQueued() != MH_OK)
                throw std::runtime_error("D3D11 hook transaction failed");
        } catch (...) {
            for (auto h : hooks) {
                MH_DisableHook(h);
                MH_RemoveHook(h);
            }
            hooks.clear();
            throw;
        }
    }
};
D3D11SceneInterceptor::D3D11SceneInterceptor(ID3D11Device *d) : impl(std::make_shared<Impl>()) {
    std::lock_guard lifecycle(Impl::lifecycle);
    if (!Impl::instance.expired())
        throw std::runtime_error("One automatic D3D11 session per process supported");
    D3D11FFR capability(d);
    if (!capability.capabilities().value("hardwareSupported", false))
        throw std::runtime_error("NVAPI VRS hardware unavailable");
    // There is no general getter for preinstallation NVAPI state. Refuse known
    // competing VRS layers rather than presume ownership of their context.
    if (GetModuleHandleW(L"XR_APILAYER_MBUCCHIA_toolkit.dll"))
        throw std::runtime_error("Disable OpenXR Toolkit before automatic D3D11 NVAPI FFR");
    impl->device = d;
    d->GetImmediateContext(&impl->context);
    impl->observeTarget();
    impl->observeViewport();
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT, 0};
    for (auto &m : impl->markers)
        check(d->CreateQuery(&q, &m.query));
    impl->install();
    std::lock_guard route(Impl::router);
    Impl::instance = impl;
}
D3D11SceneInterceptor::~D3D11SceneInterceptor() {
    std::lock_guard lifecycle(Impl::lifecycle);
    {
        std::lock_guard lock(impl->mutex);
        impl->stopped = true;
    }
    for (auto h : Impl::hooks)
        MH_QueueDisableHook(h);
    MH_ApplyQueued();
    {
        std::unique_lock lock(Impl::callbackMutex);
        Impl::drained.wait(lock, [] { return Impl::callbacks.load() == 0; });
    }
    {
        std::lock_guard route(Impl::router);
        Impl::instance.reset();
    }
    for (auto h : Impl::hooks)
        MH_RemoveHook(h);
    Impl::hooks.clear();
}
void D3D11SceneInterceptor::target(uint32_t w, uint32_t h, uint32_t arrays) {
    if (w < 128 || h < 128 || arrays > 2)
        return;
    std::lock_guard lock(impl->mutex);
    auto v = std::pair{w, h};
    if (std::find(impl->targets.begin(), impl->targets.end(), v) == impl->targets.end()) {
        if (impl->targets.size() == 16)
            impl->targets.erase(impl->targets.begin());
        impl->targets.push_back(v);
    }
}
void D3D11SceneInterceptor::update(const std::array<FFRPlan, 2> &p, bool enabled) {
    std::lock_guard lock(impl->mutex);
    auto key = ffrPlanJson(p[0]).dump() + ffrPlanJson(p[1]).dump() + p[0].settings.geometry +
               p[0].settings.policy + std::to_string(p[0].settings.transition);
    if (key != impl->signature) {
        impl->signature = std::move(key);
        ++impl->generation;
        impl->maps.clear();
    }
    impl->plans = p;
    impl->enabled = enabled && p[0].valid && p[1].valid && impl->nvStateKnown;
}
Json D3D11SceneInterceptor::diagnostics() const {
    std::lock_guard lock(impl->mutex);
    return {{"backend", "Automatic D3D11 NVIDIA NVAPI scene interception"},
            {"installed", true},
            {"enabled", impl->enabled},
            {"drawsRecorded", impl->recorded},
            {"drawsExecuted", impl->executed},
            {"executionConfirmedByFence", impl->executed > 0},
            {"executionEvidence",
             "D3D11 GPU event queries, polled without flush on immediate-context draw thread"},
            {"candidateDraws", impl->candidates},
            {"observedDraws", impl->observedDraws},
            {"observedTargets", impl->observedTargets},
            {"lastViewport",
             {impl->viewport.TopLeftX, impl->viewport.TopLeftY, impl->viewport.Width, impl->viewport.Height}},
            {"viewportCount", impl->viewportCount},
            {"foreignVrsSkipped", impl->foreignSkipped},
            {"deferredExecutionsSkipped", impl->deferredSkipped},
            {"nvapiStateKnown", impl->nvStateKnown},
            {"immutableMaps", impl->maps.size()},
            {"mapBuilds", impl->builds},
            {"targetDimensions", impl->targets},
            {"reason", impl->reason},
            {"eyeMapping", "Conservative union of per-eye centers; packed full targets use per-eye halves"},
            {"outsideSceneSuppression", false}};
}
} // namespace fov
