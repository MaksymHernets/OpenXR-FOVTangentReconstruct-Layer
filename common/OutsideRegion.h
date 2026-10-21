#pragma once
#include "ConfigManager.h"
#include "FovMath.h"
namespace fov {
// One projection authority for FOV crop, full-FOV reconstruction and VRS boundaries.
// Crop geometry is a baseline-space rectangle; the peripheral mask is active-eye space.
struct OutsideRegion {
    XrFovf baseline{}, active{};
    float horizontal = 1, vertical = 1;
    std::array<float, 4> renderedRect{0, 0, 1, 1};
    float cx = .5f, cy = .5f, middleX{}, middleY{};
    bool valid = false;
    bool cropped() const {
        return std::abs(horizontal - 1) > 1e-4f || std::abs(vertical - 1) > 1e-4f;
    }
};
class OutsideRegionManager {
  public:
    static bool enabled(const Profile &p) {
        return p.enabled && p.edgeEnabled && (p.horizontal < 1 || p.vertical < 1 || p.ffr.enabled);
    }
    static OutsideRegion plan(XrFovf baseline, XrFovf active, const FFRSettings &settings) {
        OutsideRegion p;
        p.baseline = baseline;
        p.active = active;
        const float bl = std::tan(baseline.angleLeft), br = std::tan(baseline.angleRight),
                    bu = std::tan(baseline.angleUp), bd = std::tan(baseline.angleDown),
                    l = std::tan(active.angleLeft), r = std::tan(active.angleRight),
                    u = std::tan(active.angleUp), d = std::tan(active.angleDown);
        for (float x : {bl, br, bu, bd, l, r, u, d})
            if (!std::isfinite(x))
                return p;
        if (br - bl < 1e-5f || bu - bd < 1e-5f || r - l < 1e-5f || u - d < 1e-5f || l < bl - 1e-4f ||
            r > br + 1e-4f || u > bu + 1e-4f || d < bd - 1e-4f)
            return p;
        p.horizontal = (r - l) / (br - bl);
        p.vertical = (u - d) / (bu - bd);
        p.renderedRect = {(l - bl) / (br - bl), (bu - u) / (bu - bd), p.horizontal, p.vertical};
        p.cx = std::clamp(-l / (r - l) + settings.offsetX, .02f, .98f);
        p.cy = std::clamp(u / (u - d) + settings.offsetY, .02f, .98f);
        p.middleX = std::min(settings.middleX, std::min(p.cx, 1 - p.cx));
        p.middleY = std::min(settings.middleY, std::min(p.cy, 1 - p.cy));
        p.valid = p.middleX > 0 && p.middleY > 0;
        return p;
    }
    static float boundary(const OutsideRegion &p, const FFRSettings &s, float x, float y) {
        float dx = std::abs(x - p.cx) / p.middleX, dy = std::abs(y - p.cy) / p.middleY;
        return s.geometry == "rectangle" ? std::max(dx, dy) : std::sqrt(dx * dx + dy * dy);
    }
};
} // namespace fov
