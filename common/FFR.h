#pragma once
#include "ConfigManager.h"
#include "FovMath.h"
#include <vector>
#include <limits>
namespace fov {
enum class FFRZoneMode { ThreeRenderedZones, TwoRenderedZonesWithCrop };
inline FFRZoneMode DetermineFFRMode(float horizontal, float vertical) {
    constexpr float epsilon = 1e-4f;
    return std::abs(horizontal - 1) <= epsilon && std::abs(vertical - 1) <= epsilon
               ? FFRZoneMode::ThreeRenderedZones
               : FFRZoneMode::TwoRenderedZonesWithCrop;
}
inline const char *ffrModeName(FFRZoneMode mode) {
    return mode == FFRZoneMode::ThreeRenderedZones ? "ThreeRenderedZones" : "TwoRenderedZonesWithCrop";
}
struct FFRPlan {
    XrFovf baseline{}, active{};
    FFRSettings settings;
    FFRZoneMode mode = FFRZoneMode::ThreeRenderedZones;
    float horizontal = 1, vertical = 1;
    // Active viewport in baseline normalized coordinates; never a rendered third zone.
    std::array<float, 4> renderedRect{0, 0, 1, 1};
    float cx = .5f, cy = .5f, centerX{}, centerY{}, middleX{}, middleY{};
    int middleQuality = 65;
    bool valid = false;
};
inline FFRPlan makeFFRPlan(XrFovf baseline, XrFovf active, const FFRSettings &settings) {
    FFRPlan p;
    p.baseline = baseline;
    p.active = active;
    p.settings = settings;
    const float bl = std::tan(baseline.angleLeft), br = std::tan(baseline.angleRight),
                bu = std::tan(baseline.angleUp), bd = std::tan(baseline.angleDown),
                l = std::tan(active.angleLeft), r = std::tan(active.angleRight), u = std::tan(active.angleUp),
                d = std::tan(active.angleDown);
    for (float x : {bl, br, bu, bd, l, r, u, d})
        if (!std::isfinite(x))
            return p;
    if (br - bl < 1e-5f || bu - bd < 1e-5f || r - l < 1e-5f || u - d < 1e-5f || l < bl - 1e-4f ||
        r > br + 1e-4f || u > bu + 1e-4f || d < bd - 1e-4f)
        return p;
    p.horizontal = (r - l) / (br - bl);
    p.vertical = (u - d) / (bu - bd);
    p.mode = DetermineFFRMode(p.horizontal, p.vertical);
    p.renderedRect = {(l - bl) / (br - bl), (bu - u) / (bu - bd), p.horizontal, p.vertical};
    p.cx = std::clamp(-l / (r - l) + settings.offsetX, .02f, .98f);
    p.cy = std::clamp(u / (u - d) + settings.offsetY, .02f, .98f);
    p.middleX = std::min(settings.middleX, std::min(p.cx, 1 - p.cx));
    p.middleY = std::min(settings.middleY, std::min(p.cy, 1 - p.cy));
    p.centerX = std::min(settings.centerX, p.middleX);
    p.centerY = std::min(settings.centerY, p.middleY);
    if (!(p.centerX > 0 && p.centerY > 0))
        return p;
    p.middleQuality = p.mode == FFRZoneMode::ThreeRenderedZones ? settings.fullMiddleQuality
                                                                : settings.reducedMiddleQuality;
    p.valid = true;
    return p;
}
inline int ffrZone(const FFRPlan &p, float x, float y, float paddingX = 0, float paddingY = 0) {
    // Select the finest intersecting zone. Whole VRS tiles crossing the center or
    // a transition band stay at the finer rate; the full-quality center is preserved.
    float dx = std::max(0.f, std::abs(x - p.cx) - paddingX),
          dy = std::max(0.f, std::abs(y - p.cy) - paddingY);
    auto inside = [&](float rx, float ry) {
        rx += p.settings.transition;
        ry += p.settings.transition;
        return p.settings.geometry == "rectangle" ? dx <= rx && dy <= ry
                                                  : dx * dx / (rx * rx) + dy * dy / (ry * ry) <= 1;
    };
    if (inside(p.centerX, p.centerY))
        return 1;
    if (p.mode == FFRZoneMode::TwoRenderedZonesWithCrop || inside(p.middleX, p.middleY))
        return 2;
    return 3;
}
// D3D12 rate encodings. Percentages are targets, never literal pixel fractions.
inline uint8_t ffrRate(int quality, const std::string &policy, bool additionalRates) {
    if (quality >= 100)
        return 0; // 1x1
    if (quality >= 75)
        return policy == "aggressive" ? 4 : 0; // 2x1 or 1x1
    if (quality >= 50)
        return policy == "conservative" ? 1 : 5; // 1x2 or 2x2
    if (!additionalRates)
        return 5;
    return quality <= 10 || policy == "aggressive" ? 10 : 6; // 4x4 or 2x4
}
inline const char *ffrRateName(uint8_t rate) {
    switch (rate) {
    case 0:
        return "1x1";
    case 1:
        return "1x2";
    case 4:
        return "2x1";
    case 5:
        return "2x2";
    case 6:
        return "2x4";
    case 9:
        return "4x2";
    case 10:
        return "4x4";
    default:
        return "unsupported";
    }
}
inline std::vector<uint8_t> ffrRateMap(const FFRPlan &p, uint32_t width, uint32_t height, uint32_t tile,
                                       XrRect2Di viewport, bool additionalRates) {
    if (!p.valid || !tile || !width || !height || width > 16384 || height > 16384 || viewport.offset.x < 0 ||
        viewport.offset.y < 0 || viewport.extent.width <= 0 || viewport.extent.height <= 0 ||
        uint64_t(viewport.offset.x) + viewport.extent.width > width ||
        uint64_t(viewport.offset.y) + viewport.extent.height > height)
        throw std::runtime_error("Invalid FFR projection/viewport");
    uint32_t cols = (width + tile - 1) / tile, rows = (height + tile - 1) / tile;
    std::vector<uint8_t> result(size_t(cols) * rows, 0);
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < cols; ++x) {
            float left = float(x * tile), top = float(y * tile),
                  right = float(std::min((x + 1) * tile, width)),
                  bottom = float(std::min((y + 1) * tile, height));
            // Packed-eye neighbors and tiles crossing a viewport edge remain 1x1.
            if (left < viewport.offset.x || top < viewport.offset.y ||
                right > viewport.offset.x + viewport.extent.width ||
                bottom > viewport.offset.y + viewport.extent.height)
                continue;
            int zone = ffrZone(p, ((left + right) * .5f - viewport.offset.x) / viewport.extent.width,
                               ((top + bottom) * .5f - viewport.offset.y) / viewport.extent.height,
                               (right - left) * .5f / viewport.extent.width,
                               (bottom - top) * .5f / viewport.extent.height);
            result[size_t(y) * cols + x] =
                zone == 1 ? 0
                          : ffrRate(zone == 2 ? p.middleQuality : p.settings.outerQuality, p.settings.policy,
                                    additionalRates);
        }
    return result;
}
inline Json ffrPlanJson(const FFRPlan &p) {
    auto angles = [](XrFovf f) { return Json::array({f.angleLeft, f.angleRight, f.angleUp, f.angleDown}); };
    return {{"valid", p.valid},
            {"mode", ffrModeName(p.mode)},
            {"renderedZones", p.mode == FFRZoneMode::ThreeRenderedZones ? 3 : 2},
            {"effectiveScale", {p.horizontal, p.vertical}},
            {"runtimeBaselineFov", angles(p.baseline)},
            {"activeFov", angles(p.active)},
            {"renderedRect", p.renderedRect},
            {"center", {p.cx, p.cy}},
            {"centerRadii", {p.centerX, p.centerY}},
            {"middleRadii", {p.middleX, p.middleY}},
            {"middleQuality", p.middleQuality},
            {"outerQuality",
             p.mode == FFRZoneMode::ThreeRenderedZones ? Json(p.settings.outerQuality) : Json(nullptr)},
            {"cropIsRenderedZone", false}};
}
} // namespace fov
