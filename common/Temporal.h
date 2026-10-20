#pragma once
#include "ConfigManager.h"
#include "FovMath.h"
#include <array>
#include <vector>
#include <atomic>
namespace fov {
inline bool depthFormat(int64_t f) {
    return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}
inline DXGI_FORMAT depthResourceFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
        return f;
    }
}
inline DXGI_FORMAT sampledFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return f;
    }
}
struct DepthMapping {
    float minDepth = 0, maxDepth = 1, nearZ = .1f, farZ = 100;
    bool valid() const {
        return std::isfinite(minDepth) && std::isfinite(maxDepth) && minDepth >= 0 && maxDepth <= 1 &&
               minDepth < maxDepth && nearZ > 0 && farZ > 0 && nearZ != farZ && !std::isnan(nearZ) &&
               !std::isnan(farZ);
    }
    float distance(float sample) const {
        float t = (sample - minDepth) / (maxDepth - minDepth);
        float inv = std::lerp(1 / nearZ, 1 / farZ, t);
        return inv > 0 ? 1 / inv : INFINITY;
    }
};
struct TemporalFrame {
    XrPosef pose{};
    XrFovf original{}, reduced{};
    XrTime time = 0;
    uint32_t width = 0, height = 0, depthWidth = 0, depthHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DepthMapping depth;
    bool hasDepth = false, trackingValid = false;
};
inline XrQuaternionf conjugate(XrQuaternionf q) {
    return {-q.x, -q.y, -q.z, q.w};
}
inline XrQuaternionf multiply(XrQuaternionf a, XrQuaternionf b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
    auto p = multiply(multiply(q, {v.x, v.y, v.z, 0}), conjugate(q));
    return {p.x, p.y, p.z};
}
inline XrPosef compose(XrPosef parent, XrPosef child) {
    auto position = rotate(parent.orientation, child.position);
    return {multiply(parent.orientation, child.orientation),
            {position.x + parent.position.x, position.y + parent.position.y, position.z + parent.position.z}};
}
inline bool validPose(XrPosef p) {
    auto q = p.orientation;
    float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::isfinite(norm) && std::abs(norm - 1) < .01f && std::isfinite(p.position.x) &&
           std::isfinite(p.position.y) && std::isfinite(p.position.z);
}
inline float separation(XrPosef a, XrPosef b) {
    float x = a.position.x - b.position.x, y = a.position.y - b.position.y, z = a.position.z - b.position.z;
    return std::sqrt(x * x + y * y + z * z);
}
inline float angularSeparation(XrPosef a, XrPosef b) {
    auto x = a.orientation, y = b.orientation;
    float dot = std::abs(x.x * y.x + x.y * y.y + x.z * y.z + x.w * y.w);
    return 2 * std::acos(std::clamp(dot, 0.f, 1.f));
}
struct HistoryEntry {
    uint64_t colorKey = 0, depthKey = 0;
    TemporalFrame frame;
};
struct alignas(16) HistoryGpu {
    float rotation[4]{0, 0, 0, 1}, translationAge[4]{}, tangents[4]{}, depth[4]{}, dimensions[4]{};
};
struct alignas(16) TemporalGpu {
    float outputTangents[4]{}, settings[4]{}, fallback[4]{}, debug[4]{}, outputSize[4]{};
    HistoryGpu history[8];
};
static_assert(sizeof(TemporalGpu) == 720);
struct TemporalInput {
    TemporalGpu gpu{};
    std::array<uint64_t, 8> colors{}, depths{};
    uint64_t token = 0;
};
struct TemporalMetrics {
    bool available = false;
    double reconstructedPercent = 0, fallbackPercent = 100;
    uint32_t samples = 0;
    bool depthUsed = false;
};
class TemporalHistory {
    std::array<HistoryEntry, 8> entries{};
    size_t next = 0, count = 0;
    TemporalSettings settings{};
    bool configured = false;
    uint64_t resets = 0;
    inline static std::atomic<uint64_t> nextToken{1};
    uint64_t token = nextToken.fetch_add(1);

  public:
    uint64_t invalidations() const {
        return resets;
    }
    size_t size() const {
        return count;
    }
    void invalidate() {
        if (count)
            ++resets;
        count = 0;
        next = 0;
    }
    std::vector<uint64_t> resourceKeys() const {
        std::vector<uint64_t> keys;
        for (auto &e : entries) {
            if (e.colorKey)
                keys.push_back(e.colorKey);
            if (e.depthKey)
                keys.push_back(e.depthKey);
        }
        return keys;
    }
    void prepare(const TemporalFrame &current, const TemporalSettings &s) {
        if (!configured || !(settings == s)) {
            invalidate();
            settings = s;
            configured = true;
        }
        if (!current.trackingValid || !validPose(current.pose)) {
            invalidate();
            return;
        }
        if (count) {
            auto &old = entries[(next + settings.historyFrames - 1) % settings.historyFrames].frame;
            double age = double(current.time - old.time) / 1e9;
            if (age <= 0 || age > s.maxHistoryAge || old.width != current.width ||
                old.height != current.height || old.format != current.format ||
                old.hasDepth != current.hasDepth || old.depthWidth != current.depthWidth ||
                old.depthHeight != current.depthHeight || !same(old.reduced, current.reduced) ||
                !same(old.original, current.original) || separation(old.pose, current.pose) > .75f ||
                angularSeparation(old.pose, current.pose) > 1.05f)
                invalidate();
        }
    }
    TemporalInput input(const TemporalFrame &current, const TemporalSettings &s, uint32_t outputWidth,
                        uint32_t outputHeight) const {
        TemporalInput result;
        result.token = token;
        auto &g = result.gpu;
        g.outputTangents[0] = std::tan(current.original.angleLeft);
        g.outputTangents[1] = std::tan(current.original.angleRight);
        g.outputTangents[2] = std::tan(current.original.angleUp);
        g.outputTangents[3] = std::tan(current.original.angleDown);
        g.settings[1] = s.confidenceThreshold;
        g.settings[2] = s.temporalBlendStrength;
        g.settings[3] = s.boundaryFeatherWidth;
        g.fallback[0] = s.fallbackBlurRadius;
        g.fallback[1] = s.fallbackBlurStrength;
        g.fallback[2] = s.fallbackStretchWidth;
        g.fallback[3] = s.transitionWidth;
        g.debug[0] = s.showConfidenceMask;
        g.debug[1] = s.showReprojectionCoverage;
        g.debug[2] = s.showDepth;
        g.debug[3] = 1;
        g.outputSize[0] = (float)outputWidth;
        g.outputSize[1] = (float)outputHeight;
        size_t used = 0;
        if (s.enabled && current.trackingValid)
            for (size_t i = 0; i < count; i++) {
                auto &e = entries[(next + settings.historyFrames - 1 - i) % settings.historyFrames];
                float age = float(double(current.time - e.frame.time) / 1e9);
                if (age <= 0 || age > s.maxHistoryAge)
                    continue;
                auto &h = g.history[used];
                auto inv = conjugate(current.pose.orientation);
                auto q = multiply(inv, e.frame.pose.orientation);
                h.rotation[0] = q.x;
                h.rotation[1] = q.y;
                h.rotation[2] = q.z;
                h.rotation[3] = q.w;
                auto t = rotate(inv, {e.frame.pose.position.x - current.pose.position.x,
                                      e.frame.pose.position.y - current.pose.position.y,
                                      e.frame.pose.position.z - current.pose.position.z});
                h.translationAge[0] = t.x;
                h.translationAge[1] = t.y;
                h.translationAge[2] = t.z;
                h.translationAge[3] = age;
                h.tangents[0] = std::tan(e.frame.reduced.angleLeft);
                h.tangents[1] = std::tan(e.frame.reduced.angleRight);
                h.tangents[2] = std::tan(e.frame.reduced.angleUp);
                h.tangents[3] = std::tan(e.frame.reduced.angleDown);
                h.depth[0] = e.frame.depth.minDepth;
                h.depth[1] = e.frame.depth.maxDepth;
                h.depth[2] = 1 / e.frame.depth.nearZ;
                h.depth[3] = 1 / e.frame.depth.farZ;
                h.dimensions[0] = (float)e.frame.depthWidth;
                h.dimensions[1] = (float)e.frame.depthHeight;
                h.dimensions[2] = s.useDepth && e.frame.hasDepth;
                h.dimensions[3] = s.maxHistoryAge;
                result.colors[used] = e.colorKey;
                result.depths[used] = h.dimensions[2] ? e.depthKey : 0;
                ++used;
            }
        g.settings[0] = (float)used;
        return result;
    }
    HistoryEntry &writable() {
        auto &e = entries[next];
        static std::atomic<uint64_t> key{uint64_t(1) << 63};
        if (!e.colorKey)
            e.colorKey = key.fetch_add(1);
        if (!e.depthKey)
            e.depthKey = key.fetch_add(1);
        return e;
    }
    void commit(const TemporalFrame &frame) {
        entries[next].frame = frame;
        next = (next + 1) % settings.historyFrames;
        count = std::min(count + 1, size_t(settings.historyFrames));
    }
};
} // namespace fov
