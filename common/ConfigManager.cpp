#include "Platform.h"
#include "ConfigManager.h"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <shlobj.h>
namespace fov {
std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}
std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    std::wstring o(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), o.data(), n);
    return o;
}
std::filesystem::path dataDirectory() {
    wchar_t overridePath[32768];
    DWORD n = GetEnvironmentVariableW(L"FOV_DATA_DIR", overridePath, 32768);
    if (n && n < 32768)
        return overridePath;
    PWSTR p = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p));
    std::filesystem::path dir(p);
    CoTaskMemFree(p);
    return dir / L"OpenXRFovExtension";
}
std::string normalizePath(const std::string &s) {
    auto p = std::filesystem::path(wide(s));
    std::error_code ec;
    auto q = std::filesystem::weakly_canonical(p, ec);
    if (ec)
        q = p.lexically_normal();
    auto w = q.wstring();
    std::replace(w.begin(), w.end(), L'/', L'\\');
    CharLowerBuffW(w.data(), (DWORD)w.size());
    return utf8(w);
}
std::string executablePath() {
    std::wstring s(32768, 0);
    auto n = GetModuleFileNameW(nullptr, s.data(), (DWORD)s.size());
    s.resize(n);
    return normalizePath(utf8(s));
}
std::string utcNow() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char b[32];
    sprintf_s(b, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
Json loadJson(const std::filesystem::path &p, Json fallback) {
    try {
        std::ifstream f(p);
        if (!f)
            return fallback;
        Json j;
        f >> j;
        return j.is_object() ? j : fallback;
    } catch (...) {
        return fallback;
    }
}
class FileLock {
    HANDLE h = INVALID_HANDLE_VALUE;
    OVERLAPPED ov{};

  public:
    explicit FileLock(const std::filesystem::path &p) {
        std::filesystem::create_directories(p.parent_path());
        auto lock = p;
        lock += L".lock";
        h = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open configuration lock");
        if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
            CloseHandle(h);
            throw std::runtime_error("Cannot lock configuration");
        }
    }
    ~FileLock() {
        UnlockFileEx(h, 0, 1, 0, &ov);
        CloseHandle(h);
    }
};
void updateJson(const std::filesystem::path &p, const std::function<void(Json &)> &fn) {
    FileLock lock(p);
    Json j = loadJson(p);
    fn(j);
    auto tmp = p;
    tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    auto content = j.dump(2);
    HANDLE h =
        CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create JSON temporary file");
    DWORD n = 0;
    bool ok = WriteFile(h, content.data(), (DWORD)content.size(), &n, nullptr) && n == content.size() &&
              FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        throw std::runtime_error("Cannot atomically save JSON");
    }
}
static float number(const Json &j, const char *key, float d, float lo, float hi) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return d;
    float v = it->get<float>();
    return std::isfinite(v) ? std::clamp(v, lo, hi) : d;
}
static bool flag(const Json &j, const char *k, bool d) {
    auto i = j.find(k);
    return i != j.end() && i->is_boolean() ? i->get<bool>() : d;
}
Profile parseProfile(const Json &j, Profile p) {
    if (!j.is_object())
        return p;
    p.enabled = flag(j, "enabled", p.enabled);
    p.edgeEnabled = flag(j, "edgeEnabled", p.edgeEnabled);
    if (auto f = j.find("ffr"); f != j.end() && f->is_object()) {
        auto &s = p.ffr;
        s.enabled = flag(*f, "enabled", s.enabled);
        s.debug = flag(*f, "debug", s.debug);
        s.centerX = number(*f, "centerRadiusX", s.centerX, .02f, .49f);
        s.centerY = number(*f, "centerRadiusY", s.centerY, .02f, .49f);
        s.middleX = std::max(s.centerX, number(*f, "middleRadiusX", s.middleX, .02f, .5f));
        s.middleY = std::max(s.centerY, number(*f, "middleRadiusY", s.middleY, .02f, .5f));
        s.offsetX = number(*f, "offsetX", s.offsetX, -.4f, .4f);
        s.offsetY = number(*f, "offsetY", s.offsetY, -.4f, .4f);
        s.transition = number(*f, "transition", s.transition, 0, .15f);
        s.fullMiddleQuality = int(number(*f, "fullMiddleQuality", float(s.fullMiddleQuality), 50, 75));
        s.reducedMiddleQuality =
            int(number(*f, "reducedMiddleQuality", float(s.reducedMiddleQuality), 50, 75));
        s.debugView = int(number(*f, "debugView", float(s.debugView), 0, 7));
        s.outsideFallback = int(number(*f, "outsideFallback", float(s.outsideFallback), 0, 1));
        s.outerQuality = int(number(*f, "outerQuality", float(s.outerQuality), 10, 20));
        if (auto v = f->find("geometry");
            v != f->end() && v->is_string() && (*v == "ellipse" || *v == "rectangle"))
            s.geometry = *v;
        if (auto v = f->find("policy"); v != f->end() && v->is_string() &&
                                        (*v == "conservative" || *v == "balanced" || *v == "aggressive"))
            s.policy = *v;
    }
    if (auto v = j.find("vulkan"); v != j.end() && v->is_object()) {
        p.vulkanEnabled = flag(*v, "enabled", p.vulkanEnabled);
        if (v->contains("processing") && (*v)["processing"].is_string()) {
            auto policy = (*v)["processing"].get<std::string>();
            if (policy == "auto" || policy == "passthrough")
                p.vulkanProcessing = policy;
        }
    }
    p.horizontal = number(j, "horizontalFovMultiplier", p.horizontal, .5f, 1);
    p.vertical = number(j, "verticalFovMultiplier", p.vertical, .5f, 1);
    if (j.contains("edgeFillMethod") && j["edgeFillMethod"].is_string()) {
        std::string m = j["edgeFillMethod"];
        const char *names[] = {"clamp", "gradient", "blur", "mirror", "solid", "temporalDepth"};
        for (int i = 0; i < 6; i++)
            if (m == names[i])
                p.method = i;
    }
    if (auto t = j.find("temporalDepth"); t != j.end() && t->is_object()) {
        auto &s = p.temporal;
        s.enabled = flag(*t, "enabled", s.enabled);
        s.useDepth = flag(*t, "useDepth", s.useDepth);
        s.historyFrames = (int)number(*t, "historyFrames", (float)s.historyFrames, 2, 8);
        s.confidenceThreshold = number(*t, "confidenceThreshold", s.confidenceThreshold, 0, .99f);
        s.temporalBlendStrength = number(*t, "temporalBlendStrength", s.temporalBlendStrength, 0, 1);
        s.boundaryFeatherWidth = number(*t, "boundaryFeatherWidth", s.boundaryFeatherWidth, .001f, .5f);
        s.transitionWidth = number(*t, "transitionWidth", s.transitionWidth, .01f, .5f);
        s.maxHistoryAge = number(*t, "maxHistoryAge", s.maxHistoryAge, .02f, .5f);
        s.gpuBudgetMs = number(*t, "gpuBudgetMs", s.gpuBudgetMs, 0, 20);
        auto f = t->find("fallback");
        if (f != t->end() && f->is_object()) {
            s.fallbackBlurRadius = number(*f, "blurRadius", s.fallbackBlurRadius, 0, 32);
            s.fallbackBlurStrength = number(*f, "blurStrength", s.fallbackBlurStrength, 0, 1);
            s.fallbackStretchWidth = number(*f, "stretchWidth", s.fallbackStretchWidth, .01f, .5f);
        }
        auto d = t->find("debug");
        if (d != t->end() && d->is_object()) {
            s.showConfidenceMask = flag(*d, "showConfidenceMask", s.showConfidenceMask);
            s.showReprojectionCoverage = flag(*d, "showReprojectionCoverage", s.showReprojectionCoverage);
            s.showDepth = flag(*d, "showDepth", s.showDepth);
        }
    }
    auto e = j.value("edgeFill", Json::object());
    if (!e.is_object())
        return p;
    p.stretchWidth = number(e, "stretchWidth", p.stretchWidth, .01f, .5f);
    p.gradientStrength = number(e, "gradientStrength", p.gradientStrength, 0, 1);
    p.edgeSamples = (int)number(e, "edgeSamples", (float)p.edgeSamples, 1, 16);
    p.blurRadius = (int)number(e, "blurRadius", (float)p.blurRadius, 0, 16);
    p.blurStrength = number(e, "blurStrength", p.blurStrength, 0, 1);
    if (e.contains("color") && e["color"].is_array() && e["color"].size() == 3)
        for (size_t i = 0; i < 3; i++)
            if (e["color"][i].is_number()) {
                float value = e["color"][i].get<float>();
                if (std::isfinite(value))
                    p.color[i] = std::clamp(value, 0.f, 1.f);
            }
    return p;
}
Json profileJson(const Profile &p) {
    const char *names[] = {"clamp", "gradient", "blur", "mirror", "solid", "temporalDepth"};
    return {{"enabled", p.enabled},
            {"ffr",
             {{"enabled", p.ffr.enabled},
              {"debug", p.ffr.debug},
              {"debugView", p.ffr.debugView},
              {"outsideFallback", p.ffr.outsideFallback},
              {"centerRadiusX", p.ffr.centerX},
              {"centerRadiusY", p.ffr.centerY},
              {"middleRadiusX", p.ffr.middleX},
              {"middleRadiusY", p.ffr.middleY},
              {"offsetX", p.ffr.offsetX},
              {"offsetY", p.ffr.offsetY},
              {"transition", p.ffr.transition},
              {"fullMiddleQuality", p.ffr.fullMiddleQuality},
              {"reducedMiddleQuality", p.ffr.reducedMiddleQuality},
              {"outerQuality", p.ffr.outerQuality},
              {"geometry", p.ffr.geometry},
              {"policy", p.ffr.policy}}},
            {"vulkan", {{"enabled", p.vulkanEnabled}, {"processing", p.vulkanProcessing}}},
            {"edgeEnabled", p.edgeEnabled},
            {"horizontalFovMultiplier", p.horizontal},
            {"verticalFovMultiplier", p.vertical},
            {"edgeFillMethod", names[std::clamp(p.method, 0, 5)]},
            {"temporalDepth",
             {{"enabled", p.temporal.enabled},
              {"historyFrames", p.temporal.historyFrames},
              {"useDepth", p.temporal.useDepth},
              {"confidenceThreshold", p.temporal.confidenceThreshold},
              {"temporalBlendStrength", p.temporal.temporalBlendStrength},
              {"boundaryFeatherWidth", p.temporal.boundaryFeatherWidth},
              {"transitionWidth", p.temporal.transitionWidth},
              {"maxHistoryAge", p.temporal.maxHistoryAge},
              {"gpuBudgetMs", p.temporal.gpuBudgetMs},
              {"fallback",
               {{"method", "blur"},
                {"blurRadius", p.temporal.fallbackBlurRadius},
                {"blurStrength", p.temporal.fallbackBlurStrength},
                {"stretchWidth", p.temporal.fallbackStretchWidth}}},
              {"debug",
               {{"showConfidenceMask", p.temporal.showConfidenceMask},
                {"showReprojectionCoverage", p.temporal.showReprojectionCoverage},
                {"showDepth", p.temporal.showDepth}}}}},
            {"edgeFill",
             {{"stretchWidth", p.stretchWidth},
              {"gradientStrength", p.gradientStrength},
              {"edgeSamples", p.edgeSamples},
              {"blurRadius", p.blurRadius},
              {"blurStrength", p.blurStrength},
              {"color", p.color}}}};
}
Profile loadProfile(const std::string &path) {
    auto j = loadJson(dataDirectory() / L"config.json");
    auto p = parseProfile(j.value("defaults", Json::object()));
    if (j.contains("games") && j["games"].is_object())
        for (auto &[key, value] : j["games"].items())
            if (normalizePath(key) == path) {
                p = parseProfile(value, p);
                break;
            }
    p.enabled = p.enabled && flag(j, "enabled", true);
    auto debug = j.value("debug", Json::object());
    if (debug.is_object()) {
        p.gpuTiming = flag(debug, "gpuTiming", true);
        p.logging = flag(debug, "logging", true);
        p.showFovBorders = flag(debug, "showFovBorders", false);
    }
    return p;
}
void recordLaunch(const std::string &path, const std::string &app, const std::string &api) {
    updateJson(dataDirectory() / L"history.json", [&](Json &j) {
        auto &e = j[path];
        if (!e.is_object())
            e = Json::object();
        auto now = utcNow();
        if (!e.contains("firstLaunch"))
            e["firstLaunch"] = now;
        e["lastLaunch"] = now;
        e["launchCount"] = (e.contains("launchCount") && e["launchCount"].is_number_unsigned()
                                ? e["launchCount"].get<uint64_t>()
                                : 0) +
                           1;
        e["executablePath"] = path;
        e["applicationName"] = utf8(std::filesystem::path(wide(path)).stem().wstring());
        e["openxrApplicationName"] = app;
        e["graphicsApi"] = api;
    });
}
} // namespace fov
