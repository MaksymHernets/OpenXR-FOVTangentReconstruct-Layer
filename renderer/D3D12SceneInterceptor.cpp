#include "D3D12SceneInterceptor.h"
#include <MinHook.h>
#include <map>
#include <mutex>
#include <unordered_map>
#include <set>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <chrono>
namespace fov {
namespace {
thread_local unsigned suppress = 0;
}
D3D12SceneInterceptor::Suppress::Suppress() {
    ++suppress;
}
D3D12SceneInterceptor::Suppress::~Suppress() {
    --suppress;
}
struct D3D12SceneInterceptor::Impl : std::enable_shared_from_this<Impl> {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> mapFence;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
    mutable std::recursive_mutex mutex;
    bool enabled = false, stopping = false;
    std::array<FFRPlan, 2> plans;
    std::vector<std::pair<uint32_t, uint32_t>> targets;
    struct Target {
        uint32_t width{}, height{}, samples{}, slice{}, arrays{};
        bool valid = false;
    };
    std::unordered_map<SIZE_T, Target> descriptors;
    struct Map {
        std::vector<uint8_t> bytes;
        uint32_t cols{}, rows{};
        ComPtr<ID3D12Resource> image, upload;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> initialize;
        uint64_t ready{}, generation{};
        uint32_t targetWidth{}, targetHeight{};
        XrRect2Di viewport{};
        bool packed = false;
    };
    std::vector<std::shared_ptr<Map>> maps;
    // A recorded command list also owns every immutable map it references. This
    // protects closed/replayed lists beyond the OpenXR session object's lifetime.
    struct KeepAlive final : IUnknown {
        std::atomic<ULONG> refs{1};
        std::vector<std::shared_ptr<Map>> maps;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (iid != __uuidof(IUnknown))
                return E_NOINTERFACE;
            *out = this;
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {
            return ++refs;
        }
        ULONG STDMETHODCALLTYPE Release() override {
            auto n = --refs;
            if (!n)
                delete this;
            return n;
        }
    };
    inline static constexpr GUID lifetimeKey{
        0x33bf4a6e, 0x6b39, 0x49d1, {0x9c, 0x27, 0x43, 0x15, 0x3e, 0x9f, 0x1b, 0x04}};

    struct List {
        bool known = false, inRenderPass = false, closed = false;
        Target target;
        UINT viewportCount = 0;
        D3D12_VIEWPORT viewport{};
        D3D12_SHADING_RATE rate = D3D12_SHADING_RATE_1X1;
        D3D12_SHADING_RATE_COMBINER combiners[2]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                 D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
        ComPtr<ID3D12Resource> foreignImage;
        uint64_t draws = 0;
        ComPtr<KeepAlive> keepAlive;
        ComPtr<ID3D12GraphicsCommandList5> commands;
    };
    std::unordered_map<ID3D12GraphicsCommandList *, List> lists;
    struct Queue {
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        uint64_t serial = 0;
    };
    struct Pending {
        ComPtr<ID3D12Fence> fence;
        uint64_t value{}, draws{};
    };
    std::unordered_map<ID3D12CommandQueue *, Queue> queues;
    std::vector<Pending> pending;
    uint64_t mapBuilds = 0;
    std::string shapeSignature;
    uint64_t shapeGeneration = 0;
    uint64_t mapSerial = 0, drawsRecorded = 0, drawsExecuted = 0, foreignSkipped = 0, candidateDraws = 0;
    Json lastForeignState = Json::object();
    std::string lastReason = "Waiting for observed projection targets and command-list Reset";
    inline static std::recursive_mutex routerMutex;
    inline static std::mutex lifecycleMutex, callbackMutex;
    inline static std::condition_variable callbacksFinished;
    inline static std::atomic<unsigned> callbacks{0};
    struct CallbackGuard {
        CallbackGuard() {
            callbacks.fetch_add(1);
        }
        ~CallbackGuard() {
            if (callbacks.fetch_sub(1) == 1)
                callbacksFinished.notify_all();
        }
    };
    inline static std::vector<std::weak_ptr<Impl>> instances;
    inline static std::unordered_map<ID3D12GraphicsCommandList *, std::weak_ptr<Impl>> owners;
    inline static std::vector<void *> hooks;
    using ResetFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *,
                                                 ID3D12PipelineState *);
    using CloseFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *);
    using ClearFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
    using RtvFn = void(STDMETHODCALLTYPE *)(ID3D12Device *, ID3D12Resource *,
                                            const D3D12_RENDER_TARGET_VIEW_DESC *,
                                            D3D12_CPU_DESCRIPTOR_HANDLE);
    using OmFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE *);
    using ViewFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_VIEWPORT *);
    using DrawFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT);
    using IndexedFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT);
    using IndirectFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT,
                                                 ID3D12Resource *, UINT64, ID3D12Resource *, UINT64);
    using ExecuteFn = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
    using RateFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList5 *, D3D12_SHADING_RATE,
                                             const D3D12_SHADING_RATE_COMBINER *);
    using ImageFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList5 *, ID3D12Resource *);
    using BeginPassFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *, UINT,
                                                  const D3D12_RENDER_PASS_RENDER_TARGET_DESC *,
                                                  const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *,
                                                  D3D12_RENDER_PASS_FLAGS);
    using EndPassFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *);
    using CopyFn = void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *,
                                             const UINT *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *,
                                             const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE);
    using CopySimpleFn = void(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
    inline static ResetFn originalReset{};
    inline static CloseFn originalClose{};
    inline static ClearFn originalClear{};
    inline static RtvFn originalRtv{};
    inline static OmFn originalOm{};
    inline static ViewFn originalView{};
    inline static DrawFn originalDraw{};
    inline static IndexedFn originalIndexed{};
    inline static IndirectFn originalIndirect{};
    inline static ExecuteFn originalExecute{};
    inline static RateFn originalRate{};
    inline static ImageFn originalImage{};
    inline static BeginPassFn originalBeginPass{};
    inline static EndPassFn originalEndPass{};
    inline static CopyFn originalCopy{};
    inline static CopySimpleFn originalCopySimple{};
    template <class T> static void attach(IUnknown *object, size_t index, void *replacement, T &original) {
        void *address = (*reinterpret_cast<void ***>(object))[index];
        if (MH_CreateHook(address, replacement, reinterpret_cast<void **>(&original)) != MH_OK)
            throw std::runtime_error("Cannot install D3D12 scene hook " + std::to_string(index));
        hooks.push_back(address);
    }
    static std::shared_ptr<Impl> byDevice(ID3D12Device *d) {
        std::lock_guard lock(routerMutex);
        for (auto &w : instances)
            if (auto p = w.lock(); p && p->device.Get() == d)
                return p;
        return {};
    }
    static std::shared_ptr<Impl> byList(ID3D12GraphicsCommandList *list) {
        if (suppress)
            return {};
        std::lock_guard lock(routerMutex);
        auto it = owners.find(list);
        return it == owners.end() ? nullptr : it->second.lock();
    }
    static void install(ID3D12Device *device, ID3D12CommandQueue *queue) {
        if (!hooks.empty())
            return;
        auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            throw std::runtime_error("MinHook initialization failed");
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)));
        ComPtr<ID3D12GraphicsCommandList5> v5;
        check(list.As(&v5));
        check(list->Close());
        hooks.reserve(16);
        try {
            attach(list.Get(), 9, reinterpret_cast<void *>(close), originalClose);
            attach(list.Get(), 10, reinterpret_cast<void *>(reset), originalReset);
            attach(list.Get(), 11, reinterpret_cast<void *>(clear), originalClear);
            attach(list.Get(), 12, reinterpret_cast<void *>(draw), originalDraw);
            attach(list.Get(), 13, reinterpret_cast<void *>(indexed), originalIndexed);
            attach(list.Get(), 21, reinterpret_cast<void *>(view), originalView);
            attach(list.Get(), 46, reinterpret_cast<void *>(om), originalOm);
            attach(list.Get(), 59, reinterpret_cast<void *>(indirect), originalIndirect);
            attach(v5.Get(), 68, reinterpret_cast<void *>(beginPass), originalBeginPass);
            attach(v5.Get(), 69, reinterpret_cast<void *>(endPass), originalEndPass);
            attach(v5.Get(), 77, reinterpret_cast<void *>(rate), originalRate);
            attach(v5.Get(), 78, reinterpret_cast<void *>(image), originalImage);
            attach(device, 20, reinterpret_cast<void *>(rtv), originalRtv);
            attach(device, 23, reinterpret_cast<void *>(copy), originalCopy);
            attach(device, 24, reinterpret_cast<void *>(copySimple), originalCopySimple);
            attach(queue, 10, reinterpret_cast<void *>(execute), originalExecute);
            for (auto h : hooks)
                if (MH_QueueEnableHook(h) != MH_OK)
                    throw std::runtime_error("D3D12 hook enable failed");
            if (MH_ApplyQueued() != MH_OK)
                throw std::runtime_error("D3D12 hook transaction failed");
        } catch (...) {
            for (auto h : hooks) {
                MH_DisableHook(h);
                MH_RemoveHook(h);
            }
            hooks.clear();
            throw;
        }
    }
    static HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList *list, ID3D12CommandAllocator *a,
                                           ID3D12PipelineState *pso) {
        CallbackGuard callback;
        auto result = originalReset(list, a, pso);
        if (SUCCEEDED(result) && !suppress && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
            try {
                ComPtr<ID3D12Device> d;
                if (SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&d))))
                    if (auto p = byDevice(d.Get())) {
                        {
                            std::lock_guard lock(routerMutex);
                            owners[list] = p;
                        }
                        std::lock_guard lock(p->mutex);
                        if (p->lists.size() < 2048 || p->lists.contains(list)) {
                            p->lists[list] = List{};
                            p->lists[list].known =
                                SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&p->lists[list].commands)));
                            list->SetPrivateDataInterface(lifetimeKey, nullptr);
                        }
                    }
            } catch (...) {
            }
        return result;
    }
    static HRESULT STDMETHODCALLTYPE close(ID3D12GraphicsCommandList *list) {
        CallbackGuard callback;
        auto result = originalClose(list);
        if (SUCCEEDED(result))
            if (auto p = byList(list)) {
                std::lock_guard lock(p->mutex);
                auto &s = p->lists[list];
                s.known = false;
                s.closed = true;
                s.foreignImage.Reset();
                s.commands.Reset();
            }
        return result;
    }
    static void STDMETHODCALLTYPE clear(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso) {
        CallbackGuard callback;
        originalClear(list, pso);
        if (auto p = byList(list)) {
            std::lock_guard lock(p->mutex);
            auto &s = p->lists[list];
            auto count = s.draws;
            auto keep = s.keepAlive;
            auto command = s.commands;
            s = List{};
            s.known = true;
            s.draws = count;
            s.keepAlive = keep;
            s.commands = command;
        }
    }
    static void STDMETHODCALLTYPE rtv(ID3D12Device *d, ID3D12Resource *resource,
                                      const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                      D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        CallbackGuard callback;
        originalRtv(d, resource, desc, handle);
        if (suppress)
            return;
        try {
            if (auto p = byDevice(d)) {
                Target t;
                if (resource) {
                    auto rd = resource->GetDesc();
                    t.width = UINT(rd.Width);
                    t.height = rd.Height;
                    t.samples = rd.SampleDesc.Count;
                    t.arrays = rd.DepthOrArraySize;
                    t.valid = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.MipLevels == 1 &&
                              t.samples == 1 && t.arrays <= 2;
                    if (desc) {
                        if (desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) {
                            t.slice = desc->Texture2DArray.FirstArraySlice;
                            t.valid &= desc->Texture2DArray.ArraySize == 1 && !desc->Texture2DArray.MipSlice;
                        } else
                            t.valid &= desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D &&
                                       !desc->Texture2D.MipSlice;
                    }
                }
                std::lock_guard lock(p->mutex);
                if (p->descriptors.size() < 65536 || p->descriptors.contains(handle.ptr))
                    p->descriptors[handle.ptr] = t;
            }
        } catch (...) {
        }
    }
    static void STDMETHODCALLTYPE om(ID3D12GraphicsCommandList *list, UINT n,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE *handles, BOOL contiguous,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE *depth) {
        CallbackGuard callback;
        originalOm(list, n, handles, contiguous, depth);
        if (auto p = byList(list)) {
            std::lock_guard lock(p->mutex);
            auto &s = p->lists[list];
            s.target = {};
            if (n && handles && n <= 8) {
                UINT step = p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                for (UINT i = 0; i < n; ++i) {
                    SIZE_T h = contiguous ? handles[0].ptr + size_t(i) * step : handles[i].ptr;
                    auto it = p->descriptors.find(h);
                    if (it == p->descriptors.end() || !it->second.valid) {
                        s.target = {};
                        break;
                    }
                    if (i == 0)
                        s.target = it->second;
                    else if (s.target.width != it->second.width || s.target.height != it->second.height ||
                             s.target.samples != it->second.samples) {
                        s.target = {};
                        break;
                    }
                }
            }
        }
    }
    static void STDMETHODCALLTYPE view(ID3D12GraphicsCommandList *list, UINT n, const D3D12_VIEWPORT *views) {
        CallbackGuard callback;
        originalView(list, n, views);
        if (auto p = byList(list)) {
            std::lock_guard lock(p->mutex);
            auto &s = p->lists[list];
            s.viewportCount = n;
            if (n == 1 && views)
                s.viewport = views[0];
        }
    }
    static void STDMETHODCALLTYPE rate(ID3D12GraphicsCommandList5 *list, D3D12_SHADING_RATE r,
                                       const D3D12_SHADING_RATE_COMBINER *c) {
        CallbackGuard callback;
        originalRate(list, r, c);
        if (auto p = byList(list)) {
            std::lock_guard lock(p->mutex);
            auto &s = p->lists[list];
            s.rate = r;
            for (int i = 0; i < 2; ++i)
                s.combiners[i] = c ? c[i] : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
        }
    }
    static void STDMETHODCALLTYPE image(ID3D12GraphicsCommandList5 *list, ID3D12Resource *r) {
        CallbackGuard callback;
        originalImage(list, r);
        if (auto p = byList(list)) {
            std::lock_guard lock(p->mutex);
            p->lists[list].foreignImage = r;
        }
    }
    static void STDMETHODCALLTYPE beginPass(ID3D12GraphicsCommandList4 *list, UINT n,
                                            const D3D12_RENDER_PASS_RENDER_TARGET_DESC *r,
                                            const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *d,
                                            D3D12_RENDER_PASS_FLAGS f) {
        CallbackGuard callback;
        if (auto p = byList(list)) {
            std::lock_guard lock(p->mutex);
            p->lists[list].inRenderPass = true;
        }
        originalBeginPass(list, n, r, d, f);
    }
    static void STDMETHODCALLTYPE endPass(ID3D12GraphicsCommandList4 *list) {
        CallbackGuard callback;
        originalEndPass(list);
        if (auto p = byList(list)) {
            std::lock_guard lock(p->mutex);
            p->lists[list].inRenderPass = false;
        }
    }
    void copyRtv(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE *dst, const UINT *ds, UINT ns,
                 const D3D12_CPU_DESCRIPTOR_HANDLE *src, const UINT *ss) {
        if (!dst || !src)
            return;
        std::lock_guard lock(mutex);
        UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        std::vector<Target> source;
        for (UINT i = 0; i < ns; ++i)
            for (UINT j = 0; j < (ss ? ss[i] : 1); ++j) {
                auto it = descriptors.find(src[i].ptr + size_t(j) * step);
                source.push_back(it == descriptors.end() ? Target{} : it->second);
            }
        size_t index = 0;
        for (UINT i = 0; i < nd; ++i)
            for (UINT j = 0; j < (ds ? ds[i] : 1); ++j) {
                if (index >= source.size())
                    return;
                auto h = dst[i].ptr + size_t(j) * step;
                if (descriptors.size() < 65536 || descriptors.contains(h))
                    descriptors[h] = source[index];
                ++index;
            }
    }
    static void STDMETHODCALLTYPE copy(ID3D12Device *d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE *dst,
                                       const UINT *ds, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE *src,
                                       const UINT *ss, D3D12_DESCRIPTOR_HEAP_TYPE type) {
        CallbackGuard callback;
        originalCopy(d, nd, dst, ds, ns, src, ss, type);
        if (suppress || type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
            return;
        try {
            if (auto p = byDevice(d))
                p->copyRtv(nd, dst, ds, ns, src, ss);
        } catch (...) {
        }
    }
    static void STDMETHODCALLTYPE copySimple(ID3D12Device *d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                             D3D12_CPU_DESCRIPTOR_HANDLE src,
                                             D3D12_DESCRIPTOR_HEAP_TYPE type) {
        CallbackGuard callback;
        originalCopySimple(d, n, dst, src, type);
        if (suppress || type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
            return;
        try {
            if (auto p = byDevice(d))
                p->copyRtv(1, &dst, &n, 1, &src, &n);
        } catch (...) {
        }
    }
    bool candidate(const Target &t, bool &packed) const {
        if (!t.valid || t.width < 128 || t.height < 128)
            return false;
        for (auto [w, h] : targets)
            for (int split : {1, 2}) {
                float sx = float(t.width) / (w * split), sy = float(t.height) / h;
                if (sx >= .32f && sx <= 1.1f && std::abs(sx / sy - 1) < .01f) {
                    packed = split == 2;
                    return true;
                }
            }
        return false;
    }
    std::shared_ptr<Map> getMap(const Target &t, XrRect2Di viewport, bool packed) {
        auto sameRect = [](XrRect2Di a, XrRect2Di b) {
            return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width &&
                   a.extent.height == b.extent.height;
        };
        for (auto &m : maps)
            if (m->generation == shapeGeneration && m->targetWidth == t.width &&
                m->targetHeight == t.height && m->packed == packed && sameRect(m->viewport, viewport))
                return m;
        auto remember = [&](const std::shared_ptr<Map> &m) {
            m->generation = shapeGeneration;
            m->targetWidth = t.width;
            m->targetHeight = t.height;
            m->viewport = viewport;
            m->packed = packed;
        };
        ++mapBuilds;
        uint32_t tile = options.ShadingRateImageTileSize, cols = (t.width + tile - 1) / tile,
                 rows = (t.height + tile - 1) / tile;
        auto bytes =
            ffrRateMap(plans[0], t.width, t.height, tile, viewport, options.AdditionalShadingRatesSupported);
        auto other =
            ffrRateMap(plans[1], t.width, t.height, tile, viewport, options.AdditionalShadingRatesSupported);
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = std::min(bytes[i], other[i]);
        if (packed && viewport.offset.x == 0 && viewport.extent.width == int32_t(t.width)) {
            std::fill(bytes.begin(), bytes.end(), 0);
            for (int eye = 0; eye < 2; ++eye) {
                XrRect2Di half{{int32_t(eye * t.width / 2), viewport.offset.y},
                               {int32_t(t.width / 2), viewport.extent.height}};
                auto a = ffrRateMap(plans[eye], t.width, t.height, tile, half,
                                    options.AdditionalShadingRatesSupported);
                for (size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = std::max(bytes[i], a[i]);
            }
        }
        for (auto &m : maps)
            if (m->cols == cols && m->rows == rows && m->bytes == bytes) {
                remember(m);
                return m;
            }
        if (maps.size() >= 64) {
            lastReason = "Immutable VRS map budget reached; new maps skipped";
            return {};
        }
        bool fullRate = std::all_of(bytes.begin(), bytes.end(), [](uint8_t r) { return r == 0; });
        Suppress guard;
        auto m = std::make_shared<Map>();
        remember(m);
        m->bytes = std::move(bytes);
        m->cols = cols;
        m->rows = rows;
        if (fullRate) {
            maps.push_back(m);
            return m;
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = cols;
        desc.Height = rows;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UINT;
        desc.SampleDesc.Count = 1;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&m->image)));
        uint32_t pitch = (cols + 255) & ~255u;
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = uint64_t(pitch) * rows;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&m->upload)));
        void *data = nullptr;
        D3D12_RANGE empty{0, 0};
        check(m->upload->Map(0, &empty, &data));
        for (UINT y = 0; y < rows; ++y)
            memcpy(static_cast<uint8_t *>(data) + size_t(y) * pitch, m->bytes.data() + size_t(y) * cols,
                   cols);
        m->upload->Unmap(0, nullptr);
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m->allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m->allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&m->initialize)));
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m->image.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = m->upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UINT, cols, rows, 1, pitch};
        m->initialize->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {m->image.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE};
        m->initialize->ResourceBarrier(1, &barrier);
        check(m->initialize->Close());
        // Keep ownership before submission, including a failed Signal/device-loss path.
        maps.push_back(m);
        m->ready = ++mapSerial;
        ID3D12CommandList *commands = m->initialize.Get();
        queue->ExecuteCommandLists(1, &commands);
        check(queue->Signal(mapFence.Get(), m->ready));
        return m;
    }
    ComPtr<ID3D12GraphicsCommandList5> prepare(ID3D12GraphicsCommandList *list,
        std::array<D3D12_SHADING_RATE_COMBINER, 2> &restore) {
        auto it = lists.find(list);
        if (it == lists.end())
            return {};
        auto &s = it->second;
        if (!enabled || stopping || !s.known || s.inRenderPass || s.viewportCount != 1)
            return {};
        // RE Engine explicitly enables per-primitive MAX even when no image is
        // bound. Preserve that primitive contribution and compose our image with
        // MAX. Keep rejecting image ownership, coarse base rates and other modes.
        const bool primitiveMax = s.combiners[0] == D3D12_SHADING_RATE_COMBINER_MAX;
        if (s.foreignImage || s.rate != D3D12_SHADING_RATE_1X1 ||
            (!primitiveMax && s.combiners[0] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH) ||
            s.combiners[1] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH) {
            ++foreignSkipped;
            lastForeignState = {{"rate", unsigned(s.rate)}, {"combiners", {unsigned(s.combiners[0]), unsigned(s.combiners[1])}},
                                {"imageBound", bool(s.foreignImage)}, {"targetWidth", s.target.width}, {"targetHeight", s.target.height}};
            lastReason = "Foreign VRS state observed; automatic override skipped";
            return {};
        }
        bool packed = false;
        if (!candidate(s.target, packed))
            return {};
        ++candidateDraws;
        auto v = s.viewport;
        if (!std::isfinite(v.TopLeftX) || !std::isfinite(v.TopLeftY) || !std::isfinite(v.Width) ||
            !std::isfinite(v.Height) || v.TopLeftX < 0 || v.TopLeftY < 0 || v.Width <= 0 || v.Height <= 0 ||
            v.TopLeftX + v.Width > s.target.width || v.TopLeftY + v.Height > s.target.height)
            return {};
        XrRect2Di rect{{int32_t(v.TopLeftX), int32_t(v.TopLeftY)}, {int32_t(v.Width), int32_t(v.Height)}};
        if (rect.offset.y != 0 || rect.extent.height != int32_t(s.target.height))
            return {};
        if (!packed && (rect.offset.x != 0 || rect.extent.width != int32_t(s.target.width)))
            return {};
        if (packed && !((rect.offset.x == 0 && rect.extent.width == int32_t(s.target.width)) ||
                        (rect.extent.width == int32_t(s.target.width / 2) &&
                         (rect.offset.x == 0 || rect.offset.x == int32_t(s.target.width / 2)))))
            return {};
        if (float(rect.offset.x) != v.TopLeftX || float(rect.offset.y) != v.TopLeftY ||
            float(rect.extent.width) != v.Width || float(rect.extent.height) != v.Height)
            return {};
        auto m = getMap(s.target, rect, packed);
        auto ready = mapFence->GetCompletedValue();
        if (!m)
            return {};
        if (!m->image) {
            lastReason = "Selected policy yields 1x1 throughout this target";
            return {};
        }
        if (ready == UINT64_MAX || ready < m->ready) {
            lastReason = "Waiting for GPU map initialization (no wait inserted)";
            return {};
        }
        auto command = s.commands;
        if (!command)
            return {};
        restore = {s.combiners[0], s.combiners[1]};
        D3D12_SHADING_RATE_COMBINER c[]{s.combiners[0], primitiveMax ? D3D12_SHADING_RATE_COMBINER_MAX
                                                                 : D3D12_SHADING_RATE_COMBINER_OVERRIDE};
        if (!s.keepAlive) {
            s.keepAlive.Attach(new KeepAlive);
            check(list->SetPrivateDataInterface(lifetimeKey, s.keepAlive.Get()));
        }
        if (std::find(s.keepAlive->maps.begin(), s.keepAlive->maps.end(), m) == s.keepAlive->maps.end())
            s.keepAlive->maps.push_back(m);
        lastReason = "Automatic D3D12 scene draws recorded";
        originalRate(command.Get(), D3D12_SHADING_RATE_1X1, c);
        originalImage(command.Get(), m->image.Get());
        ++s.draws;
        ++drawsRecorded;
        return command;
    }
    template <class F> static void withDraw(ID3D12GraphicsCommandList *list, F &&draw) {
        auto p = byList(list);
        if (!p) {
            draw();
            return;
        }
        Suppress nestedCallbacks;
        ComPtr<ID3D12GraphicsCommandList5> command;
        std::array<D3D12_SHADING_RATE_COMBINER, 2> restore{};
        {
            std::lock_guard lock(p->mutex);
            try {
                command = p->prepare(list, restore);
            } catch (const std::exception &e) {
                p->lastReason = e.what();
            } catch (...) {
            }
        }
        // Do not serialize the application's driver draw calls or hold our state
        // mutex across another component's trampoline. Eligible state is known
        // base 1x1/null; restore the observed primitive combiners after the draw.
        draw();
        if (command) {
            originalImage(command.Get(), nullptr);
            originalRate(command.Get(), D3D12_SHADING_RATE_1X1, restore.data());
        }
    }

    static void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList *list, UINT a, UINT b, UINT c, UINT d) {
        CallbackGuard callback;
        withDraw(list, [&] { originalDraw(list, a, b, c, d); });
    }
    static void STDMETHODCALLTYPE indexed(ID3D12GraphicsCommandList *list, UINT a, UINT b, UINT c, INT d,
                                          UINT e) {
        CallbackGuard callback;
        withDraw(list, [&] { originalIndexed(list, a, b, c, d, e); });
    }
    static void STDMETHODCALLTYPE indirect(ID3D12GraphicsCommandList *list, ID3D12CommandSignature *sig,
                                           UINT n, ID3D12Resource *args, UINT64 offset,
                                           ID3D12Resource *counts, UINT64 countOffset) {
        CallbackGuard callback;
        // Indirect may contain dispatch/bundle operations; no verified graphics-signature metadata.
        originalIndirect(list, sig, n, args, offset, counts, countOffset);
    }
    static void STDMETHODCALLTYPE execute(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *commands) {
        CallbackGuard callback;
        originalExecute(q, n, commands);
        if (suppress || !n || !commands)
            return;
        try {
            ComPtr<ID3D12Device> d;
            if (FAILED(q->GetDevice(IID_PPV_ARGS(&d))))
                return;
            auto p = byDevice(d.Get());
            if (!p)
                return;
            std::lock_guard lock(p->mutex);
            uint64_t draws = 0;
            for (UINT i = 0; i < n; ++i) {
                auto it = p->lists.find(static_cast<ID3D12GraphicsCommandList *>(commands[i]));
                if (it != p->lists.end() && it->second.closed)
                    draws += it->second.draws;
            }
            if (!draws)
                return;
            auto &state = p->queues[q];
            if (!state.queue) {
                state.queue = q;
                check(p->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence)));
            }
            if (p->pending.size() >= 4096)
                p->poll();
            if (p->pending.size() >= 4096) {
                p->lastReason = "Execution evidence queue full";
                return;
            }
            uint64_t value = ++state.serial;
            p->pending.push_back({state.fence, value, draws});
            check(q->Signal(state.fence.Get(), value));
        } catch (...) {
        }
    }
    void poll() {
        std::erase_if(pending, [&](const Pending &p) {
            auto done = p.fence->GetCompletedValue();
            if (done == UINT64_MAX)
                return false;
            if (done >= p.value) {
                drawsExecuted += p.draws;
                return true;
            }
            return false;
        });
    }
    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            enabled = false;
        }
        std::lock_guard lifecycle(lifecycleMutex);
        {
            std::lock_guard lock(routerMutex);
            for (auto h : hooks)
                MH_QueueDisableHook(h);
            MH_ApplyQueued();
        }
        // Keep routing live until already-entered ExecuteCommandLists callbacks
        // have recorded their completion fences, including secondary queues.
        {
            std::unique_lock lock(callbackMutex);
            callbacksFinished.wait(lock, [] { return callbacks.load() == 0; });
        }
        {
            std::lock_guard lock(routerMutex);
            std::erase_if(instances, [&](auto &w) {
                auto p = w.lock();
                return !p || p.get() == this;
            });
            std::erase_if(owners, [&](auto &kv) {
                auto p = kv.second.lock();
                return !p || p.get() == this;
            });
            for (auto h : hooks)
                MH_RemoveHook(h);
            hooks.clear();
        }
        // Only teardown waits. All observed queues receive a completion marker after
        // recorded work. On loss/timeout retain maps instead of freeing in-flight memory.
        std::lock_guard lock(mutex);
        bool complete = true;
        Suppress guard;
        UniqueHandle event;
        event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        auto wait = [&](ID3D12CommandQueue *q, ID3D12Fence *f, uint64_t value) {
            if (FAILED(q->Signal(f, value)) || !event.get() ||
                FAILED(f->SetEventOnCompletion(value, event.get())) ||
                WaitForSingleObject(event.get(), 5000) != WAIT_OBJECT_0)
                complete = false;
        };
        wait(queue.Get(), mapFence.Get(), ++mapSerial);
        for (auto &[_, q] : queues)
            if (q.fence)
                wait(q.queue.Get(), q.fence.Get(), ++q.serial);
        if (!complete) {
            for (auto &m : maps) {
                m->image.Detach();
                m->upload.Detach();
                m->allocator.Detach();
                m->initialize.Detach();
            }
            queue.Detach();
            mapFence.Detach();
            for (auto &[_, q] : queues) {
                q.queue.Detach();
                q.fence.Detach();
            }
        }
    }
};
D3D12SceneInterceptor::D3D12SceneInterceptor(ID3D12Device *device, ID3D12CommandQueue *queue)
    : impl(std::make_shared<Impl>()) {
    if (!device || !queue)
        throw std::runtime_error("Missing automatic D3D12 scene binding");
    impl->device = device;
    impl->queue = queue;
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &impl->options, sizeof(impl->options)));
    if (impl->options.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 ||
        !impl->options.ShadingRateImageTileSize || device->GetNodeCount() != 1)
        throw std::runtime_error("Automatic FFR requires single-node D3D12 Tier 2");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->mapFence)));
    // Command-list private COM owners may outlive the session. Keep their vtables
    // loaded until process exit, as required for an in-process graphics interceptor.
    HMODULE resident{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Impl::originalDraw), &resident))
        throw std::runtime_error("Cannot retain interceptor module lifetime");
    std::lock_guard lifecycle(Impl::lifecycleMutex);
    std::lock_guard lock(Impl::routerMutex);
    if (!Impl::instances.empty())
        throw std::runtime_error("One automatic D3D12 scene session per process is supported");
    Impl::instances.reserve(1);
    Impl::install(device, queue);
    Impl::instances.push_back(impl);
}
D3D12SceneInterceptor::~D3D12SceneInterceptor() {
    if (impl)
        impl->shutdown();
}
void D3D12SceneInterceptor::target(uint32_t width, uint32_t height, uint32_t arrays) {
    if (width < 128 || height < 128 || arrays > 2)
        return;
    std::lock_guard lock(impl->mutex);
    auto value = std::pair{width, height};
    if (std::find(impl->targets.begin(), impl->targets.end(), value) == impl->targets.end()) {
        if (impl->targets.size() == 16)
            impl->targets.erase(impl->targets.begin());
        impl->targets.push_back(value);
    }
}
void D3D12SceneInterceptor::update(const std::array<FFRPlan, 2> &plans, bool enabled) {
    std::lock_guard lock(impl->mutex);
    auto signature = ffrPlanJson(plans[0]).dump() + ffrPlanJson(plans[1]).dump() +
                     plans[0].settings.geometry + plans[0].settings.policy +
                     std::to_string(plans[0].settings.transition);
    if (signature != impl->shapeSignature) {
        impl->shapeSignature = std::move(signature);
        ++impl->shapeGeneration;
    }
    impl->plans = plans;
    impl->enabled = enabled && plans[0].valid && plans[1].valid;
}
Json D3D12SceneInterceptor::diagnostics() const {
    std::lock_guard lock(impl->mutex);
    impl->poll();
    return {{"backend", "Automatic D3D12 Tier 2 scene interception"},
            {"installed", true},
            {"enabled", impl->enabled},
            {"drawsRecorded", impl->drawsRecorded},
            {"drawsExecuted", impl->drawsExecuted},
            {"executionConfirmedByFence", impl->drawsExecuted > 0},
            {"candidateDraws", impl->candidateDraws},
            {"foreignVrsSkipped", impl->foreignSkipped},
            {"lastForeignVrsState", impl->lastForeignState},
            {"immutableMaps", impl->maps.size()},
            {"mapBuilds", impl->mapBuilds},
            {"targetDimensions", impl->targets},
            {"trackedCommandLists", impl->lists.size()},
            {"observedRtvDescriptors", impl->descriptors.size()},
            {"reason", impl->lastReason},
            {"eyeMapping",
             "Conservative union of both per-eye full-quality centers; packed targets use per-eye halves"},
            {"outsideSceneSuppression", false}};
}
} // namespace fov
