#pragma once
#include "json.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <array>
namespace fov {
using Json = nlohmann::json;
struct TemporalSettings {
    bool enabled = true, useDepth = true, showConfidenceMask = false, showReprojectionCoverage = false,
         showDepth = false;
    int historyFrames = 4;
    float confidenceThreshold = .6f, temporalBlendStrength = .85f, boundaryFeatherWidth = .05f;
    float fallbackBlurRadius = 6, fallbackBlurStrength = .5f, fallbackStretchWidth = .15f,
          transitionWidth = .1f;
    float maxHistoryAge = .15f, gpuBudgetMs = 0;
    bool operator==(const TemporalSettings &) const = default;
};
struct Profile {
    bool enabled = true, edgeEnabled = true;
    float horizontal = 0.8f, vertical = 0.8f, stretchWidth = 0.15f, gradientStrength = 0.7f,
          blurStrength = 0.5f;
    int method = 1, edgeSamples = 8, blurRadius = 4;
    std::array<float, 3> color{0, 0, 0};
    TemporalSettings temporal;
    bool gpuTiming = true, logging = true, showFovBorders = false;
};
Json profileJson(const Profile &);
Profile parseProfile(const Json &, Profile defaults = {});
std::filesystem::path dataDirectory();
std::string utf8(const std::wstring &);
std::wstring wide(const std::string &);
std::string executablePath();
std::string normalizePath(const std::string &);
std::string utcNow();
Json loadJson(const std::filesystem::path &, Json fallback = Json::object());
void updateJson(const std::filesystem::path &, const std::function<void(Json &)> &);
Profile loadProfile(const std::string &path);
void recordLaunch(const std::string &, const std::string &, const std::string &);
} // namespace fov
