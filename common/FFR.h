#pragma once
#include "ConfigManager.h"
#include "OutsideRegion.h"
#include <vector>
#include <limits>
namespace fov {
enum class FFRZoneMode { TwoRenderedZonesWithOutside, TwoRenderedZonesWithCrop };
inline FFRZoneMode DetermineFFRMode(float horizontal, float vertical) {
    constexpr float epsilon = 1e-4f;
    return std::abs(horizontal - 1) <= epsilon && std::abs(vertical - 1) <= epsilon
               ? FFRZoneMode::TwoRenderedZonesWithOutside
               : FFRZoneMode::TwoRenderedZonesWithCrop;
}
inline const char *ffrModeName(FFRZoneMode mode) {
    return mode == FFRZoneMode::TwoRenderedZonesWithOutside ? "TwoRenderedZonesWithOutside"
                                                            : "TwoRenderedZonesWithCrop";
}
struct FFRPlan : OutsideRegion {
    FFRSettings settings;
    FFRZoneMode mode = FFRZoneMode::TwoRenderedZonesWithOutside;
    float centerX{}, centerY{};
    int middleQuality = 65;
};
inline FFRPlan makeFFRPlan(XrFovf baseline, XrFovf active, const FFRSettings &settings) {
    FFRPlan p;
    static_cast<OutsideRegion &>(p) = OutsideRegionManager::plan(baseline, active, settings);
    p.settings = settings;
    p.mode = DetermineFFRMode(p.horizontal, p.vertical);
    p.centerX = std::min(settings.centerX, p.middleX);
    p.centerY = std::min(settings.centerY, p.middleY);
    p.valid &= p.centerX > 0 && p.centerY > 0;
    p.middleQuality = p.cropped() ? settings.reducedMiddleQuality : settings.fullMiddleQuality;
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
    if (p.cropped() || OutsideRegionManager::boundary(p, p.settings, x, y) <= 1)
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
            // Outside and every tile touching it remain 1x1. Coarse Middle tiles
            // must be wholly contained in the reconstruction observation boundary.
            if (!p.cropped()) {
                bool inside = true;
                for (float px : {left, right})
                    for (float py : {top, bottom})
                        inside &= OutsideRegionManager::boundary(
                                      p, p.settings, (px - viewport.offset.x) / viewport.extent.width,
                                      (py - viewport.offset.y) / viewport.extent.height) <= 1;
                if (!inside)
                    continue;
            }
            int zone = ffrZone(p, ((left + right) * .5f - viewport.offset.x) / viewport.extent.width,
                               ((top + bottom) * .5f - viewport.offset.y) / viewport.extent.height,
                               (right - left) * .5f / viewport.extent.width,
                               (bottom - top) * .5f / viewport.extent.height);
            result[size_t(y) * cols + x] =
                zone != 2 ? 0 : ffrRate(p.middleQuality, p.settings.policy, additionalRates);
        }
    return result;
}
inline Json ffrPlanJson(const FFRPlan &p) {
    auto angles = [](XrFovf f) { return Json::array({f.angleLeft, f.angleRight, f.angleUp, f.angleDown}); };
    return {{"valid", p.valid},
            {"mode", ffrModeName(p.mode)},
            {"renderedZones", 2},
            {"logicalRegions", 3},
            {"outsideRegion", p.cropped() ? "projection-crop" : "peripheral-mask"},
            {"outsideSceneSuppression", false},
            {"outsideSceneSavings", 0},
            {"effectiveScale", {p.horizontal, p.vertical}},
            {"runtimeBaselineFov", angles(p.baseline)},
            {"activeFov", angles(p.active)},
            {"renderedRect", p.renderedRect},
            {"center", {p.cx, p.cy}},
            {"centerRadii", {p.centerX, p.centerY}},
            {"middleRadii", {p.middleX, p.middleY}},
            {"middleQuality", p.middleQuality},
            {"cropIsRenderedZone", false}};
}
} // namespace fov
