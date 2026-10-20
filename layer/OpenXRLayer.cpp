#include "common/Platform.h"
#include "common/FovMath.h"
#include "renderer/IEdgeRenderer.h"
#include "IPCServer.h"
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <tuple>
#include <set>
namespace fov {
#define DISPATCH_FUNCTIONS(X)                                                                                \
    X(DestroyInstance)                                                                                       \
    X(CreateSession)                                                                                         \
    X(DestroySession)                                                                                        \
    X(LocateViews)                                                                                           \
    X(EnumerateViewConfigurationViews)                                                                       \
    X(CreateSwapchain) X(DestroySwapchain) X(EnumerateSwapchainImages) X(AcquireSwapchainImage)              \
        X(WaitSwapchainImage) X(ReleaseSwapchainImage) X(EndFrame) X(BeginSession) X(EndSession)             \
            X(PollEvent)
struct Instance {
    XrInstance handle{};
    PFN_xrGetInstanceProcAddr gipa{};
#define MEMBER(name) PFN_xr##name name{};
    DISPATCH_FUNCTIONS(MEMBER)
#undef MEMBER
    std::shared_ptr<IPCServer> ipc;
    Profile startup;
    bool depthExtension = false;
    PFN_xrCreateReferenceSpace CreateReferenceSpace{};
    PFN_xrLocateSpace LocateSpace{};
    PFN_xrDestroySpace DestroySpace{};
};
struct Located {
    XrTime time{};
    XrViewConfigurationType type{};
    std::vector<XrFovf> original, reduced;
    Profile profile;
    bool trackingValid = false;
};
struct Session {
    XrSession handle{};
    std::shared_ptr<Instance> instance;
    std::unique_ptr<IEdgeRenderer> renderer;
    std::string api = "Unsupported";
    std::deque<Located> located;
    using HistoryKey = std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, int, int, int, int>;
    std::map<HistoryKey, TemporalHistory> histories;
    uint64_t historyInvalidations = 0, frameEpoch = 0;
    XrTime resetTime = 0;
    XrSpace historySpace = XR_NULL_HANDLE;
};
struct Acquisition {
    uint32_t index;
    bool waited = false;
};
struct Swapchain {
    XrSwapchain handle{}, output{};
    std::shared_ptr<Session> session;
    XrSwapchainCreateInfo info{}, outInfo{};
    bool capable = false, captured = false, outputAcquired = false, outputWaited = false;
    std::vector<ComPtr<IUnknown>> images, outImages;
    std::deque<Acquisition> acquired;
    uint32_t outputIndex = 0;
    uint64_t captureEpoch = 0;
};
std::recursive_mutex objectsMutex;
std::unordered_map<XrInstance, std::shared_ptr<Instance>> instances;
std::unordered_map<XrSession, std::shared_ptr<Session>> sessions;
std::unordered_map<XrSwapchain, std::shared_ptr<Swapchain>> swapchains;
template <auto Function> struct Boundary;
template <class... Args, XrResult(XRAPI_PTR *Function)(Args...)> struct Boundary<Function> {
    static XrResult XRAPI_CALL invoke(Args... args) noexcept {
        try {
            return Function(args...);
        } catch (const std::bad_alloc &) {
            return XR_ERROR_OUT_OF_MEMORY;
        } catch (...) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }
};
template <class Map, class Key> auto lookup(Map &map, Key key) {
    auto i = map.find(key);
    return i == map.end() ? typename Map::mapped_type{} : i->second;
}
static uint64_t id(XrSwapchain s) {
    return reinterpret_cast<uint64_t>(s);
}
static void resetHistory(Session &session, bool freeResources = false) {
    for (auto &[_, history] : session.histories) {
        if (freeResources)
            session.historyInvalidations += history.invalidations() + (history.size() ? 1 : 0);
        if (freeResources && session.renderer)
            for (auto key : history.resourceKeys())
                session.renderer->forget(key);
        history.invalidate();
    }
    if (freeResources)
        session.histories.clear();
}
static bool validSubImage(const Swapchain &sc, const XrSwapchainSubImage &sub) {
    auto r = sub.imageRect;
    return sub.imageArrayIndex < sc.info.arraySize && r.offset.x >= 0 && r.offset.y >= 0 &&
           r.extent.width > 0 && r.extent.height > 0 &&
           (uint64_t)r.offset.x + r.extent.width <= sc.info.width &&
           (uint64_t)r.offset.y + r.extent.height <= sc.info.height;
}
static void error(Session &s, const std::string &message) {
    s.instance->ipc->report({{"lastError", message}, {"status", "Passthrough"}});
}
static bool allowed(const Profile &p) {
    return p.enabled && p.edgeEnabled && (p.horizontal < 1 || p.vertical < 1);
}
static void getImages(Swapchain &sc, bool output) {
    auto &dispatch = *sc.session->instance;
    auto handle = output ? sc.output : sc.handle;
    auto &images = output ? sc.outImages : sc.images;
    uint32_t n = 0;
    if (XR_FAILED(dispatch.EnumerateSwapchainImages(handle, 0, &n, nullptr)))
        throw std::runtime_error("Cannot enumerate swapchain images");
    if (sc.session->api == "D3D11") {
        std::vector<XrSwapchainImageD3D11KHR> v(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(dispatch.EnumerateSwapchainImages(
                handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(v.data()))))
            throw std::runtime_error("Cannot enumerate D3D11 images");
        for (auto &x : v)
            images.emplace_back(x.texture);
    } else {
        std::vector<XrSwapchainImageD3D12KHR> v(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        if (XR_FAILED(dispatch.EnumerateSwapchainImages(
                handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(v.data()))))
            throw std::runtime_error("Cannot enumerate D3D12 images");
        for (auto &x : v)
            images.emplace_back(x.texture);
    }
}
static bool outputImage(Swapchain &sc) {
    auto &d = *sc.session->instance;
    if (!sc.output) {
        auto ci = sc.info;
        ci.next = nullptr;
        ci.createFlags = 0;
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.mipCount = 1;
        ci.width = static_cast<uint32_t>(std::ceil(ci.width / d.startup.horizontal));
        ci.height = static_cast<uint32_t>(std::ceil(ci.height / d.startup.vertical));
        auto r = d.CreateSwapchain(sc.session->handle, &ci, &sc.output);
        if (XR_FAILED(r)) {
            error(*sc.session, "Output swapchain creation failed: " + std::to_string(r));
            return false;
        }
        sc.outInfo = ci;
        getImages(sc, true);
    }
    if (sc.outImages.empty())
        getImages(sc, true);
    if (!sc.outputAcquired) {
        XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        auto r = d.AcquireSwapchainImage(sc.output, &a, &sc.outputIndex);
        if (XR_FAILED(r))
            return false;
        sc.outputAcquired = true;
    }
    if (!sc.outputWaited) {
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        w.timeout = 0;
        auto r = d.WaitSwapchainImage(sc.output, &w);
        if (r != XR_SUCCESS && r != XR_SESSION_LOSS_PENDING)
            return false;
        sc.outputWaited = true;
    }
    return sc.outputIndex < sc.outImages.size();
}
static bool releaseOutput(Swapchain &sc) {
    if (sc.outputAcquired && sc.outputWaited) {
        XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        auto result = sc.session->instance->ReleaseSwapchainImage(sc.output, &r);
        if (XR_FAILED(result))
            return false;
        sc.outputAcquired = sc.outputWaited = false;
    }
    return !sc.outputAcquired;
}
static void disposeSwapchain(Swapchain &sc) {
    resetHistory(*sc.session, true);
    if (sc.session->renderer)
        sc.session->renderer->forget(id(sc.handle));
    sc.images.clear();
    sc.captured = false;
    sc.outImages.clear();
    if (sc.output) {
        auto result = sc.session->instance->DestroySwapchain(sc.output);
        if (XR_FAILED(result))
            throw std::runtime_error("Output swapchain destruction failed: " + std::to_string(result));
        sc.output = XR_NULL_HANDLE;
    }
    sc.outputAcquired = sc.outputWaited = false;
}
static Json fovJson(XrFovf v) {
    return Json::array({v.angleLeft, v.angleRight, v.angleUp, v.angleDown});
}
XRAPI_ATTR XrResult XRAPI_CALL hookBeginSession(XrSession handle, const XrSessionBeginInfo *info) {
    std::lock_guard lock(objectsMutex);
    auto s = lookup(sessions, handle);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    auto result = s->instance->BeginSession(handle, info);
    if (XR_SUCCEEDED(result)) {
        resetHistory(*s);
        s->located.clear();
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL hookEndSession(XrSession handle) {
    std::lock_guard lock(objectsMutex);
    auto s = lookup(sessions, handle);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    auto result = s->instance->EndSession(handle);
    if (XR_SUCCEEDED(result)) {
        resetHistory(*s);
        s->located.clear();
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL hookPollEvent(XrInstance handle, XrEventDataBuffer *event) {
    std::lock_guard lock(objectsMutex);
    auto inst = lookup(instances, handle);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    auto result = inst->PollEvent(handle, event);
    if (result == XR_SUCCESS && event->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        auto changed = reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(event);
        auto s = lookup(sessions, changed->session);
        if (s)
            s->resetTime = changed->changeTime;
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL hookDestroySwapchain(XrSwapchain handle) {
    std::lock_guard lock(objectsMutex);
    auto sc = lookup(swapchains, handle);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    try {
        disposeSwapchain(*sc);
    } catch (const std::exception &e) {
        error(*sc->session, e.what());
        return XR_ERROR_RUNTIME_FAILURE;
    }
    auto r = sc->session->instance->DestroySwapchain(handle);
    if (XR_SUCCEEDED(r))
        swapchains.erase(handle);
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL hookDestroySession(XrSession handle) {
    std::lock_guard lock(objectsMutex);
    auto s = lookup(sessions, handle);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    try {
        resetHistory(*s, true);
        if (s->historySpace) {
            auto result = s->instance->DestroySpace(s->historySpace);
            if (XR_FAILED(result))
                return result;
            s->historySpace = XR_NULL_HANDLE;
        }
        for (auto &[_, sc] : swapchains)
            if (sc->session == s) {
                disposeSwapchain(*sc);
                sc->captured = false;
            }
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    auto r = s->instance->DestroySession(handle);
    if (XR_SUCCEEDED(r)) {
        for (auto it = swapchains.begin(); it != swapchains.end();)
            if (it->second->session == s)
                it = swapchains.erase(it);
            else
                ++it;
        sessions.erase(handle);
        s->instance->ipc->report({{"status", "Session destroyed"}});
    }
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL hookDestroyInstance(XrInstance handle) {
    std::lock_guard lock(objectsMutex);
    auto inst = lookup(instances, handle);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    std::vector<XrSession> children;
    for (auto &[h, s] : sessions)
        if (s->instance == inst)
            children.push_back(h);
    for (auto h : children) {
        auto r = hookDestroySession(h);
        if (XR_FAILED(r))
            return r;
    }
    auto r = inst->DestroyInstance(handle);
    if (XR_SUCCEEDED(r))
        instances.erase(handle);
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL hookCreateSession(XrInstance handle, const XrSessionCreateInfo *info,
                                                 XrSession *result) {
    std::lock_guard lock(objectsMutex);
    auto inst = lookup(instances, handle);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    auto r = inst->CreateSession(handle, info, result);
    if (XR_FAILED(r))
        return r;
    try {
        auto s = std::make_shared<Session>();
        s->handle = *result;
        s->instance = inst;
        try {
            for (auto p = static_cast<const XrBaseInStructure *>(info->next); p; p = p->next) {
                if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                    auto b = reinterpret_cast<const XrGraphicsBindingD3D11KHR *>(p);
                    s->api = "D3D11";
                    s->renderer = makeD3D11(b->device);
                    break;
                }
                if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                    auto b = reinterpret_cast<const XrGraphicsBindingD3D12KHR *>(p);
                    s->api = "D3D12";
                    s->renderer = makeD3D12(b->device, b->queue);
                    break;
                }
            }
        } catch (const std::exception &e) {
            error(*s, e.what());
        }
        sessions[*result] = s;
        inst->ipc->report({{"graphicsApi", s->api},
                           {"status", s->renderer ? "Ready" : "Unsupported graphics binding: passthrough"}});
        return r;
    } catch (...) {
        inst->DestroySession(*result);
        *result = XR_NULL_HANDLE;
        return XR_ERROR_OUT_OF_MEMORY;
    }
}
XRAPI_ATTR XrResult XRAPI_CALL hookEnumerateViewConfigurationViews(XrInstance handle, XrSystemId system,
                                                                   XrViewConfigurationType type,
                                                                   uint32_t capacity, uint32_t *count,
                                                                   XrViewConfigurationView *views) {
    std::lock_guard lock(objectsMutex);
    auto i = lookup(instances, handle);
    if (!i)
        return XR_ERROR_HANDLE_INVALID;
    auto r = i->EnumerateViewConfigurationViews(handle, system, type, capacity, count, views);
    if (XR_SUCCEEDED(r) && capacity && views && type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
        allowed(i->startup)) {
        for (uint32_t n = 0; n < *count; n++) {
            views[n].recommendedImageRectWidth =
                std::max(1u, (uint32_t)std::ceil(views[n].recommendedImageRectWidth * i->startup.horizontal));
            views[n].recommendedImageRectHeight =
                std::max(1u, (uint32_t)std::ceil(views[n].recommendedImageRectHeight * i->startup.vertical));
        }
    }
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL hookLocateViews(XrSession handle, const XrViewLocateInfo *info,
                                               XrViewState *state, uint32_t capacity, uint32_t *count,
                                               XrView *views) {
    std::lock_guard lock(objectsMutex);
    auto s = lookup(sessions, handle);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    auto r = s->instance->LocateViews(handle, info, state, capacity, count, views);
    if (XR_FAILED(r) || !capacity || !views)
        return r;
    try {
        auto p = s->instance->ipc->profile();
        if (p.method != 5 && !s->histories.empty())
            resetHistory(*s, true);
        const auto validTracking = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        bool tracking = (state->viewStateFlags & validTracking) == validTracking;
        if (!tracking || (s->resetTime && info->displayTime >= s->resetTime)) {
            resetHistory(*s);
            s->resetTime = 0;
        }
        if (!s->renderer || !allowed(p) ||
            info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO || *count != 2) {
            s->instance->ipc->report(
                {{"status",
                  !allowed(p) ? "Processing disabled" : "Passthrough: unsupported views or graphics binding"},
                 {"fovMatched", false}});
            resetHistory(*s);
            return r;
        }
        Located l;
        l.time = info->displayTime;
        l.type = info->viewConfigurationType;
        l.profile = p;
        l.trackingValid = tracking;
        for (uint32_t n = 0; n < *count; n++) {
            l.original.push_back(views[n].fov);
            l.reduced.push_back(reduce(views[n].fov, p.horizontal, p.vertical));
        }
        // Allocate and retain provenance before modifying the application's output.
        s->located.push_back(l);
        while (s->located.size() > 16)
            s->located.pop_front();
        for (uint32_t n = 0; n < *count; n++)
            views[n].fov = l.reduced[n];
    } catch (const std::exception &e) {
        error(*s, e.what());
    }
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL hookCreateSwapchain(XrSession handle, const XrSwapchainCreateInfo *info,
                                                   XrSwapchain *result) {
    std::lock_guard lock(objectsMutex);
    auto s = lookup(sessions, handle);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    bool color =
        supportedFormat(info->format) && (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    bool depth = s->instance->depthExtension && depthFormat(info->format) &&
                 (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    bool capable = s->renderer && (color || depth) && info->sampleCount == 1 && info->faceCount == 1 &&
                   info->mipCount == 1 && info->createFlags == 0 && !info->next &&
                   (info->usageFlags & (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                        XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    auto ci = *info;
    if (capable)
        ci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    auto r = s->instance->CreateSwapchain(handle, &ci, result);
    if (XR_FAILED(r) && capable) {
        capable = false;
        r = s->instance->CreateSwapchain(handle, info, result);
    }
    if (XR_FAILED(r))
        return r;
    try {
        auto sc = std::make_shared<Swapchain>();
        sc->handle = *result;
        sc->session = s;
        sc->info = *info;
        sc->capable = capable;
        swapchains[*result] = sc;
        return r;
    } catch (...) {
        s->instance->DestroySwapchain(*result);
        *result = XR_NULL_HANDLE;
        return XR_ERROR_OUT_OF_MEMORY;
    }
}
XRAPI_ATTR XrResult XRAPI_CALL hookEnumerateSwapchainImages(XrSwapchain handle, uint32_t capacity,
                                                            uint32_t *count,
                                                            XrSwapchainImageBaseHeader *images) {
    std::lock_guard lock(objectsMutex);
    auto sc = lookup(swapchains, handle);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    return sc->session->instance->EnumerateSwapchainImages(handle, capacity, count, images);
}
XRAPI_ATTR XrResult XRAPI_CALL hookAcquireSwapchainImage(XrSwapchain handle,
                                                         const XrSwapchainImageAcquireInfo *info,
                                                         uint32_t *index) {
    std::lock_guard lock(objectsMutex);
    auto sc = lookup(swapchains, handle);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    auto r = sc->session->instance->AcquireSwapchainImage(handle, info, index);
    if (XR_SUCCEEDED(r))
        sc->acquired.push_back({*index, false});
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL hookWaitSwapchainImage(XrSwapchain handle,
                                                      const XrSwapchainImageWaitInfo *info) {
    std::lock_guard lock(objectsMutex);
    auto sc = lookup(swapchains, handle);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    auto r = sc->session->instance->WaitSwapchainImage(handle, info);
    if (r == XR_SUCCESS || r == XR_SESSION_LOSS_PENDING)
        for (auto &a : sc->acquired)
            if (!a.waited) {
                a.waited = true;
                break;
            }
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL hookReleaseSwapchainImage(XrSwapchain handle,
                                                         const XrSwapchainImageReleaseInfo *info) {
    std::lock_guard lock(objectsMutex);
    auto sc = lookup(swapchains, handle);
    if (!sc)
        return XR_ERROR_HANDLE_INVALID;
    sc->captured = false;
    bool copied = false;
    auto profile = sc->session->instance->ipc->profile();
    if (sc->capable && !info->next && !sc->acquired.empty() && sc->acquired.front().waited &&
        allowed(profile) &&
        (!depthFormat(sc->info.format) || (profile.method == 5 && profile.temporal.useDepth)))
        try {
            if (sc->images.empty())
                getImages(*sc, false);
            auto n = sc->acquired.front().index;
            if (n >= sc->images.size())
                throw std::runtime_error("Invalid acquired image index");
            sc->session->renderer->capture(id(handle), sc->images[n].Get(), (DXGI_FORMAT)sc->info.format);
            copied = true;
        } catch (const std::exception &e) {
            error(*sc->session, e.what());
            sc->capable = false;
        }
    auto r = sc->session->instance->ReleaseSwapchainImage(handle, info);
    if (XR_SUCCEEDED(r)) {
        if (!sc->acquired.empty())
            sc->acquired.pop_front();
        sc->captured = copied;
        if (copied)
            sc->captureEpoch = sc->session->frameEpoch;
    }
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL hookEndFrame(XrSession handle, const XrFrameEndInfo *info) {
    std::lock_guard lock(objectsMutex);
    auto s = lookup(sessions, handle);
    if (!s)
        return XR_ERROR_HANDLE_INVALID;
    const auto captureEpoch = s->frameEpoch++;
    auto passthrough = [&]() { return s->instance->EndFrame(handle, info); };
    if (!s->renderer || !info || !info->layerCount)
        return passthrough();
    try {
        auto rec = s->located.rend();
        for (auto it = s->located.rbegin(); it != s->located.rend(); ++it)
            if (it->time == info->displayTime) {
                rec = it;
                break;
            }
        if (rec == s->located.rend())
            return passthrough();
        // Temporal reconstruction consumes an optional plain KHR depth attachment.
        // Other extension chains retain their original image/FOV pairing.
        struct Projection {
            uint32_t index;
            XrCompositionLayerProjection layer;
            std::vector<XrCompositionLayerProjectionView> views;
            XrPosef worldFromSpace{};
            bool worldValid = false;
        };
        std::vector<Projection> projections;
        std::vector<std::shared_ptr<Swapchain>> used;
        for (uint32_t n = 0; n < info->layerCount; n++) {
            auto base = info->layers[n];
            if (!base || base->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                continue;
            auto p = reinterpret_cast<const XrCompositionLayerProjection *>(base);
            if (p->next || p->viewCount != 2)
                continue;
            s->instance->ipc->report(
                {{"requestedFov", Json::array({fovJson(rec->reduced[0]), fovJson(rec->reduced[1])})},
                 {"submittedFov", Json::array({fovJson(p->views[0].fov), fovJson(p->views[1].fov)})},
                 {"fovMatched", false}});
            bool valid = true;
            for (uint32_t v = 0; v < 2; v++) {
                auto &view = p->views[v];
                auto sc = lookup(swapchains, view.subImage.swapchain);
                auto r = view.subImage.imageRect;
                bool supportedChain = !view.next;
                if (view.next && rec->profile.method == 5 && s->instance->depthExtension) {
                    auto node = static_cast<const XrBaseInStructure *>(view.next);
                    supportedChain = node->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR && !node->next;
                }
                if (!supportedChain || !same(view.fov, rec->reduced[v]) || !sc || sc->session != s ||
                    !supportedFormat(sc->info.format) || !sc->capable || !sc->captured ||
                    view.subImage.imageArrayIndex >= sc->info.arraySize || r.offset.x < 0 || r.offset.y < 0 ||
                    r.extent.width <= 0 || r.extent.height <= 0 ||
                    (uint64_t)r.offset.x + r.extent.width > sc->info.width ||
                    (uint64_t)r.offset.y + r.extent.height > sc->info.height) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                s->instance->ipc->report(
                    {{"status", "Passthrough: submitted FOV or projection configuration differs"}});
                continue;
            }
            projections.push_back({n, *p, {p->views, p->views + 2}});
        }
        if (projections.empty())
            return passthrough();
        s->renderer->setTiming(rec->profile.gpuTiming ||
                               (rec->profile.method == 5 && rec->profile.temporal.gpuBudgetMs > 0));
        if (rec->profile.method == 5 && !s->historySpace && s->instance->CreateReferenceSpace &&
            s->instance->LocateSpace && s->instance->DestroySpace) {
            XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            space.poseInReferenceSpace.orientation.w = 1;
            auto result = s->instance->CreateReferenceSpace(handle, &space, &s->historySpace);
            if (XR_FAILED(result))
                s->historySpace = XR_NULL_HANDLE;
        }
        // Reject repeated overlapping references: a single output image cannot represent
        // different reconstructions of the same sub-image in one submitted frame.
        struct Region {
            XrSwapchain swapchain;
            uint32_t slice;
            XrRect2Di rect;
        };
        std::vector<Region> regions;
        for (auto &p : projections)
            for (auto &v : p.views) {
                auto r = v.subImage.imageRect;
                for (auto &prior : regions)
                    if (prior.swapchain == v.subImage.swapchain &&
                        prior.slice == v.subImage.imageArrayIndex &&
                        r.offset.x < prior.rect.offset.x + prior.rect.extent.width &&
                        prior.rect.offset.x < r.offset.x + r.extent.width &&
                        r.offset.y < prior.rect.offset.y + prior.rect.extent.height &&
                        prior.rect.offset.y < r.offset.y + r.extent.height)
                        return passthrough();
                regions.push_back({v.subImage.swapchain, v.subImage.imageArrayIndex, r});
                auto sc = swapchains.at(v.subImage.swapchain);
                if (std::find(used.begin(), used.end(), sc) == used.end())
                    used.push_back(sc);
            }
        bool ready = true;
        for (auto &sc : used)
            if (!outputImage(*sc))
                ready = false;
        if (!ready) {
            for (auto &sc : used)
                releaseOutput(*sc);
            error(*s, "Output image unavailable; keeping submitted image and FOV");
            return passthrough();
        }
        Json temporalStatus = Json::array();
        std::set<Session::HistoryKey> activeHistories;
        try {
            for (auto &p : projections)
                for (uint32_t v = 0; v < 2; v++) {
                    if (v == 0 && rec->profile.method == 5 && s->historySpace) {
                        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                        auto result = s->instance->LocateSpace(p.layer.space, s->historySpace,
                                                               info->displayTime, &location);
                        auto required =
                            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                        p.worldValid = XR_SUCCEEDED(result) &&
                                       (location.locationFlags & required) == required &&
                                       validPose(location.pose);
                        if (p.worldValid)
                            p.worldFromSpace = location.pose;
                    }
                    auto &view = p.views[v];
                    auto sc = swapchains.at(view.subImage.swapchain);
                    auto c = constants(view, rec->original[v], sc->info, rec->profile);
                    auto src = view.subImage.imageRect;
                    auto rect = src;
                    // Scale both endpoints to avoid gaps between packed eye rectangles.
                    auto x = [&](int a) { return (int)((int64_t)a * sc->outInfo.width / sc->info.width); };
                    auto y = [&](int a) { return (int)((int64_t)a * sc->outInfo.height / sc->info.height); };
                    rect.offset = {x(src.offset.x), y(src.offset.y)};
                    rect.extent = {x(src.offset.x + src.extent.width) - rect.offset.x,
                                   y(src.offset.y + src.extent.height) - rect.offset.y};
                    TemporalHistory *history = nullptr;
                    TemporalFrame temporalFrame;
                    XrSwapchainSubImage depthRegion{};
                    std::shared_ptr<Swapchain> depthChain;
                    if (rec->profile.method == 5) {
                        if (s->histories.size() > 32)
                            resetHistory(*s, true);
                        auto identity = Session::HistoryKey{id(sc->handle),
                                                            reinterpret_cast<uint64_t>(p.layer.space),
                                                            p.index,
                                                            v,
                                                            view.subImage.imageArrayIndex,
                                                            src.offset.x,
                                                            src.offset.y,
                                                            src.extent.width,
                                                            src.extent.height};
                        activeHistories.insert(identity);
                        history = &s->histories[identity];
                        temporalFrame.pose = p.worldValid ? compose(p.worldFromSpace, view.pose) : view.pose;
                        temporalFrame.original = rec->original[v];
                        temporalFrame.reduced = view.fov;
                        temporalFrame.time = info->displayTime;
                        temporalFrame.width = src.extent.width;
                        temporalFrame.height = src.extent.height;
                        temporalFrame.format = (DXGI_FORMAT)sc->info.format;
                        temporalFrame.trackingValid =
                            rec->trackingValid && p.worldValid && validPose(temporalFrame.pose);
                        if (view.next && rec->profile.temporal.useDepth) {
                            auto depth = static_cast<const XrCompositionLayerDepthInfoKHR *>(view.next);
                            depthChain = lookup(swapchains, depth->subImage.swapchain);
                            DepthMapping mapping{depth->minDepth, depth->maxDepth, depth->nearZ, depth->farZ};
                            if (depthChain && depthChain->session == s && depthChain->capable &&
                                depthChain->captured && depthChain->captureEpoch == captureEpoch &&
                                depthFormat(depthChain->info.format) &&
                                validSubImage(*depthChain, depth->subImage) && mapping.valid()) {
                                depthRegion = depth->subImage;
                                temporalFrame.hasDepth = true;
                                temporalFrame.depth = mapping;
                                temporalFrame.depthWidth = depthRegion.imageRect.extent.width;
                                temporalFrame.depthHeight = depthRegion.imageRect.extent.height;
                            }
                        }
                        history->prepare(temporalFrame, rec->profile.temporal);
                        auto input = history->input(temporalFrame, rec->profile.temporal, rect.extent.width,
                                                    rect.extent.height);
                        s->renderer->setTemporal(input);
                        bool budgetFallback =
                            rec->profile.temporal.gpuBudgetMs > 0 &&
                            s->renderer->gpuMilliseconds().value_or(0) > rec->profile.temporal.gpuBudgetMs;
                        if (budgetFallback) {
                            c.method = 2;
                            c.blurRadius = rec->profile.temporal.fallbackBlurRadius;
                            c.blurStrength = rec->profile.temporal.fallbackBlurStrength;
                            c.stretchWidth = rec->profile.temporal.fallbackStretchWidth;
                        }
                        auto metrics = s->renderer->temporalMetrics();
                        temporalStatus.push_back(
                            {{"eye", v},
                             {"depthAvailable", temporalFrame.hasDepth},
                             {"trackingValid", temporalFrame.trackingValid},
                             {"referenceSpaceAvailable", p.worldValid},
                             {"mode", budgetFallback || input.gpu.settings[0] == 0 ? "Blur Fallback"
                                      : metrics.available
                                          ? (metrics.reconstructedPercent > 0
                                                 ? (metrics.depthUsed ? "Depth" : "Rotation Only")
                                                 : "Blur Fallback")
                                          : "Awaiting GPU coverage"},
                             {"validHistoryFrames", (int)input.gpu.settings[0]},
                             {"reconstructedPercent",
                              metrics.available ? Json(metrics.reconstructedPercent) : Json(nullptr)},
                             {"fallbackPercent",
                              metrics.available ? Json(metrics.fallbackPercent) : Json(nullptr)},
                             {"coverageSampleCount", metrics.samples},
                             {"coverageIsSampled", true},
                             {"historyInvalidations", s->historyInvalidations + history->invalidations()},
                             {"gpuBudgetFallback", budgetFallback}});
                    }
                    s->renderer->render(id(sc->handle), sc->outImages[sc->outputIndex].Get(),
                                        (DXGI_FORMAT)sc->info.format, c, rect);
                    if (history && temporalFrame.trackingValid && rec->profile.temporal.enabled) {
                        auto &entry = history->writable();
                        s->renderer->storeHistory(id(sc->handle), entry.colorKey, view.subImage);
                        if (temporalFrame.hasDepth)
                            s->renderer->storeHistory(id(depthChain->handle), entry.depthKey, depthRegion);
                        history->commit(temporalFrame);
                    }
                    if (history)
                        view.next = nullptr;
                    view.fov = rec->original[v];
                    view.subImage.swapchain = sc->output;
                    view.subImage.imageRect = rect;
                }
            if (rec->profile.method == 5)
                for (auto entry = s->histories.begin(); entry != s->histories.end();) {
                    if (!activeHistories.contains(entry->first)) {
                        s->historyInvalidations +=
                            entry->second.invalidations() + (entry->second.size() ? 1 : 0);
                        for (auto key : entry->second.resourceKeys())
                            s->renderer->forget(key);
                        entry = s->histories.erase(entry);
                    } else
                        ++entry;
                }
        } catch (...) {
            for (auto &sc : used)
                releaseOutput(*sc);
            throw;
        }
        bool released = true;
        for (auto &sc : used)
            if (!releaseOutput(*sc))
                released = false;
        if (!released) {
            error(*s, "Output release failed; keeping submitted image and FOV");
            return passthrough();
        }
        std::vector<const XrCompositionLayerBaseHeader *> layers(info->layers,
                                                                 info->layers + info->layerCount);
        for (auto &p : projections) {
            p.layer.views = p.views.data();
            layers[p.index] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&p.layer);
        }
        auto frame = *info;
        frame.layers = layers.data();
        auto gpu = s->renderer->gpuMilliseconds();
        auto sourceGpu = s->renderer->sourceCopyMilliseconds(),
             historyGpu = s->renderer->historyCopyMilliseconds();
        s->instance->ipc->report(
            {{"status", "Reconstructing"},
             {"requestedFov", Json::array({fovJson(rec->reduced[0]), fovJson(rec->reduced[1])})},
             {"submittedFov", Json::array({fovJson(rec->original[0]), fovJson(rec->original[1])})},
             {"fovMatched", true},
             {"temporalDepth", temporalStatus},
             {"gpuStageMilliseconds",
              {{"sourceSnapshot", rec->profile.gpuTiming && sourceGpu ? Json(*sourceGpu) : Json(nullptr)},
               {"historyCopy", rec->profile.gpuTiming && historyGpu ? Json(*historyGpu) : Json(nullptr)},
               {"fusedReprojectionConfidenceFallback",
                rec->profile.method == 5 && gpu ? Json(*gpu) : Json(nullptr)}}},
             {"gpuMillisecondsPerEyePass", gpu ? Json(*gpu) : Json(nullptr)}});
        return s->instance->EndFrame(handle, &frame);
    } catch (const std::exception &e) {
        for (auto &[_, sc] : swapchains)
            if (sc->session == s)
                releaseOutput(*sc);
        resetHistory(*s);
        error(*s, e.what());
        return passthrough();
    }
}
XRAPI_ATTR XrResult XRAPI_CALL hookGetInstanceProcAddr(XrInstance handle, const char *name,
                                                       PFN_xrVoidFunction *function) {
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    *function = nullptr;
    if (!std::strcmp(name, "xrGetInstanceProcAddr")) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Boundary<hookGetInstanceProcAddr>::invoke);
        return XR_SUCCESS;
    }
    std::lock_guard lock(objectsMutex);
    auto inst = lookup(instances, handle);
    if (!inst)
        return XR_ERROR_HANDLE_INVALID;
    auto r = inst->gipa(handle, name, function);
    if (XR_FAILED(r))
        return r;
#define HOOK(n)                                                                                              \
    if (!std::strcmp(name, "xr" #n)) {                                                                       \
        *function = reinterpret_cast<PFN_xrVoidFunction>(Boundary<hook##n>::invoke);                         \
        return XR_SUCCESS;                                                                                   \
    }
    DISPATCH_FUNCTIONS(HOOK)
#undef HOOK
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL createLayerInstance(const XrInstanceCreateInfo *info,
                                                   const XrApiLayerCreateInfo *layerInfo,
                                                   XrInstance *result) {
    if (!info || !layerInfo || !result ||
        layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layerInfo->structSize != sizeof(XrApiLayerCreateInfo) || !layerInfo->nextInfo)
        return XR_ERROR_INITIALIZATION_FAILED;
    auto next = layerInfo->nextInfo;
    if (next->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        next->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        next->structSize != sizeof(XrApiLayerNextInfo) || !next->nextGetInstanceProcAddr ||
        !next->nextCreateApiLayerInstance || std::strcmp(next->layerName, "XR_APILAYER_FOV_extension"))
        return XR_ERROR_INITIALIZATION_FAILED;
    auto copy = *layerInfo;
    copy.nextInfo = next->next;
    auto r = next->nextCreateApiLayerInstance(info, &copy, result);
    if (XR_FAILED(r))
        return r;
    try {
        auto inst = std::make_shared<Instance>();
        inst->handle = *result;
        inst->gipa = next->nextGetInstanceProcAddr;
        inst->gipa(*result, "xrCreateReferenceSpace",
                   reinterpret_cast<PFN_xrVoidFunction *>(&inst->CreateReferenceSpace));
        inst->gipa(*result, "xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction *>(&inst->LocateSpace));
        inst->gipa(*result, "xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction *>(&inst->DestroySpace));
        for (uint32_t extension = 0; extension < info->enabledExtensionCount; extension++)
            if (!std::strcmp(info->enabledExtensionNames[extension],
                             XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME))
                inst->depthExtension = true;
#define LOAD(n)                                                                                              \
    if (XR_FAILED(inst->gipa(*result, "xr" #n, reinterpret_cast<PFN_xrVoidFunction *>(&inst->n))) ||         \
        !inst->n)                                                                                            \
        throw std::runtime_error("Missing downstream xr" #n);
        DISPATCH_FUNCTIONS(LOAD)
#undef LOAD
        auto path = executablePath();
        inst->startup = loadProfile(path);
        {
            std::lock_guard serviceLock(objectsMutex);
            static std::weak_ptr<IPCServer> service;
            inst->ipc = service.lock();
            if (!inst->ipc) {
                inst->ipc = std::make_shared<IPCServer>(
                    path, std::string(
                              info->applicationInfo.applicationName,
                              strnlen(info->applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE)));
                service = inst->ipc;
            }
        }
        std::lock_guard lock(objectsMutex);
        instances[*result] = inst;
        return r;
    } catch (...) {
        PFN_xrDestroyInstance destroy = nullptr;
        next->nextGetInstanceProcAddr(*result, "xrDestroyInstance",
                                      reinterpret_cast<PFN_xrVoidFunction *>(&destroy));
        if (destroy)
            destroy(*result);
        *result = XR_NULL_HANDLE;
        return XR_ERROR_INITIALIZATION_FAILED;
    }
}
} // namespace fov
extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo *loader, const char *name, XrNegotiateApiLayerRequest *request) {
    if (!loader || !request || !name || std::strcmp(name, "XR_APILAYER_FOV_extension") ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loader->structSize != sizeof(XrNegotiateLoaderInfo) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        request->structSize != sizeof(XrNegotiateApiLayerRequest) ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0) || loader->minApiVersion > XR_CURRENT_API_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = std::min(loader->maxApiVersion, XR_CURRENT_API_VERSION);
    request->getInstanceProcAddr = fov::Boundary<fov::hookGetInstanceProcAddr>::invoke;
    request->createApiLayerInstance = fov::Boundary<fov::createLayerInstance>::invoke;
    return XR_SUCCESS;
}
