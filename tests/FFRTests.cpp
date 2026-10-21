#include "common/FFR.h"
#include <iostream>
using namespace fov;
static void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
int main() {
    try {
        FFRSettings settings;
        settings.enabled = true;
        XrFovf baseline{-.9f, .65f, .8f, -.7f};
        require(DetermineFFRMode(1, 1) == FFRZoneMode::TwoRenderedZonesWithOutside, "full mode");
        require(DetermineFFRMode(1.00005f, .99995f) == FFRZoneMode::TwoRenderedZonesWithOutside, "epsilon");
        for (auto scale : {.5f, .7f, .85f, .99f})
            for (int axis = 0; axis < 3; ++axis) {
                auto p = makeFFRPlan(baseline, reduce(baseline, axis == 1 ? 1 : scale, axis == 0 ? 1 : scale),
                                     settings);
                require(p.valid && p.mode == FFRZoneMode::TwoRenderedZonesWithCrop,
                        "per-axis effective reduction");
                require(!ffrPlanJson(p)["cropIsRenderedZone"].get<bool>(), "crop never zone 3");
                for (int y = 0; y < 100; ++y)
                    for (int x = 0; x < 100; ++x)
                        require(ffrZone(p, x / 99.f, y / 99.f) != 3, "two zones throughout active viewport");
                require(std::abs(p.horizontal - (axis == 1 ? 1 : scale)) < 1e-5f,
                        "authoritative horizontal span");
            }
        auto streamed = reduce(baseline, .7f, .8f);
        require(makeFFRPlan(streamed, streamed, settings).mode == FFRZoneMode::TwoRenderedZonesWithOutside,
                "streamer baseline not additional crop");
        auto left = makeFFRPlan(baseline, baseline, settings);
        auto right = makeFFRPlan({-.65f, .9f, .8f, -.7f}, {-.65f, .9f, .8f, -.7f}, settings);
        require(left.cx > right.cx && left.centerX <= left.middleX &&
                    left.middleX <= std::min(left.cx, 1 - left.cx),
                "asymmetric eye geometry and containment");
        require(ffrZone(left, left.cx, left.cy) == 1 && ffrZone(left, 0, 0) == 3,
                "center and outer classification");
        require(!makeFFRPlan(baseline, reduce(baseline, 1.1f, 1), settings).valid,
                "outside baseline rejected");
        auto bad = baseline;
        bad.angleLeft = std::numeric_limits<float>::quiet_NaN();
        require(!makeFFRPlan(bad, baseline, settings).valid, "nonfinite projection rejected");
        auto map = ffrRateMap(left, 1024, 768, 16, {{0, 0}, {512, 768}}, true);
        for (int y = 0; y < 48; ++y)
            for (int x = 32; x < 64; ++x)
                require(map[y * 64 + x] == 0, "packed neighbor stays full quality");
        require(map[0] == 0 && ffrPlanJson(left)["renderedZones"] == 2,
                "Outside is reconstructed, never a coarse VRS region");
        Profile full;
        full.horizontal = full.vertical = 1;
        require(!OutsideRegionManager::enabled(full), "Full FOV idle without FFR");
        full.ffr.enabled = true;
        require(OutsideRegionManager::enabled(full), "Full FOV Outside reconstruction active");
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 32; ++x) {
                if (map[y * 64 + x])
                    for (float px : {float(x * 16), float((x + 1) * 16)})
                        for (float py : {float(y * 16), float((y + 1) * 16)})
                            require(OutsideRegionManager::boundary(left, settings, px / 512, py / 768) <= 1,
                                    "Coarse tiles cannot cross Outside reconstruction mask");
            }
        for (int q : {10, 15, 20, 50, 65, 75, 100})
            require(ffrRate(q, "aggressive", false) <= 5, "optional rates conservative fallback");
        require(ffrRate(100, "aggressive", true) == 0 && ffrRate(75, "conservative", true) == 0 &&
                    ffrRate(10, "aggressive", true) == 10,
                "quality mapping");
        Profile profile;
        profile.method = 5;
        profile.ffr = settings;
        profile.ffr.fullMiddleQuality = 50;
        profile.ffr.reducedMiddleQuality = 75;
        profile.ffr.outerQuality = 10;
        auto loaded = parseProfile(profileJson(profile));
        require(loaded.ffr == profile.ffr && loaded.method == 5,
                "independent FFR/reconstruction persistence");
        auto inherited = parseProfile(
            {{"ffr",
              {{"enabled", false}, {"outerQuality", 99}, {"centerRadiusX", .48}, {"middleRadiusX", .1}}}},
            loaded);
        require(!inherited.ffr.enabled && inherited.ffr.outerQuality == 20 &&
                    inherited.ffr.middleX >= inherited.ffr.centerX &&
                    inherited.ffr.reducedMiddleQuality == 75,
                "profile inheritance and limits");
        std::cout << "PASS: adaptive FFR mode, baseline, asymmetric projection, geometry, rate mapping and "
                     "configuration\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
