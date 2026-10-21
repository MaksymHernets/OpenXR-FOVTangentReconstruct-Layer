#include "Platform.h"
#include "ConfigManager.h"
#include "RegistryManager.h"
#include <shellapi.h>
#include <system_error>
namespace fov {
static constexpr auto key = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
std::filesystem::path moduleDirectory() {
    wchar_t p[32768];
    GetModuleFileNameW(nullptr, p, 32768);
    return std::filesystem::path(p).parent_path();
}
void validateManifest(const std::filesystem::path &manifest) {
    auto j = loadJson(manifest);
    if (!j.contains("api_layer"))
        throw std::runtime_error("Manifest missing or invalid");
    auto a = j["api_layer"];
    if (a.value("name", "") != "XR_APILAYER_FOV_extension" ||
        a.value("disable_environment", "") != "XR_FOV_DISABLE")
        throw std::runtime_error("Unexpected layer manifest");
    auto dll = (manifest.parent_path() / wide(a.value("library_path", ""))).lexically_normal();
    if (dll.filename() != L"OpenXRFovExtension.dll" || dll.parent_path() != manifest.parent_path() ||
        !std::filesystem::is_regular_file(dll))
        throw std::runtime_error("Manifest must point to adjacent OpenXRFovExtension.dll");
}
Registration registration(const std::filesystem::path &p) {
    HKEY h{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return Registration::Missing;
    DWORD type = 0, v = 0, n = sizeof(v);
    auto r = RegQueryValueExW(h, p.c_str(), nullptr, &type, (BYTE *)&v, &n);
    RegCloseKey(h);
    if (r == ERROR_FILE_NOT_FOUND)
        return Registration::Missing;
    if (r != ERROR_SUCCESS || type != REG_DWORD || n != 4)
        return Registration::Invalid;
    return v ? Registration::Disabled : Registration::Enabled;
}
std::vector<std::filesystem::path> activeRegistrationConflicts(const std::filesystem::path &p) {
    HKEY existing{};
    auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &existing);
    if (opened == ERROR_FILE_NOT_FOUND)
        return {};
    if (opened != ERROR_SUCCESS)
        throw std::system_error(opened, std::system_category(),
                                "Cannot inspect existing layer registrations");
    std::vector<std::filesystem::path> conflicts;
    try {
        for (DWORD index = 0;; ++index) {
            wchar_t name[32768];
            DWORD length = 32768;
            auto status = RegEnumValueW(existing, index, name, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status != ERROR_SUCCESS)
                throw std::system_error(status, std::system_category(),
                                        "Cannot enumerate layer registrations");
            std::wstring candidate(name, length);
            if (normalizePath(utf8(candidate)) == normalizePath(utf8(p.wstring())))
                continue;
            DWORD type = 0, value = 1, bytes = sizeof(value);
            status = RegQueryValueExW(existing, candidate.c_str(), nullptr, &type,
                                      reinterpret_cast<BYTE *>(&value), &bytes);
            // Match loader activation semantics: only REG_DWORD zero is enabled.
            // A disabled old installation is not a competing active layer.
            if (status == ERROR_FILE_NOT_FOUND || status == ERROR_MORE_DATA)
                continue;
            if (status != ERROR_SUCCESS)
                throw std::system_error(status, std::system_category(), "Cannot read layer activation state");
            if (type != REG_DWORD || bytes != sizeof(value) || value != 0)
                continue;
            auto manifest = loadJson(candidate);
            if (manifest.contains("api_layer") && manifest["api_layer"].is_object() &&
                manifest["api_layer"].value("name", "") == "XR_APILAYER_FOV_extension")
                conflicts.emplace_back(candidate);
        }
    } catch (...) {
        RegCloseKey(existing);
        throw;
    }
    RegCloseKey(existing);
    return conflicts;
}
void setRegistration(const std::filesystem::path &p, bool enabled) {
    validateManifest(p);
    if (enabled) {
        auto conflicts = activeRegistrationConflicts(p);
        if (!conflicts.empty())
            throw std::system_error(
                ERROR_ALREADY_EXISTS, std::system_category(),
                "Another active copy is already registered. Disable or uninstall that copy first: " +
                    utf8(conflicts.front().wstring()));
    }
    HKEY h{};
    auto r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                             &h, nullptr);
    if (r == ERROR_SUCCESS) {
        DWORD v = enabled ? 0 : 1;
        r = RegSetValueExW(h, p.c_str(), 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(h);
    }
    if (r != ERROR_SUCCESS)
        throw std::system_error(r, std::system_category(), "Registry write failed");
}
void unregisterLayer(const std::filesystem::path &p) {
    HKEY h{};
    auto r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &h);
    if (r == ERROR_FILE_NOT_FOUND)
        return;
    if (r == ERROR_SUCCESS) {
        r = RegDeleteValueW(h, p.c_str());
        RegCloseKey(h);
    }
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND)
        throw std::system_error(r, std::system_category(), "Registry removal failed");
}
std::string registrationFailureMessage(int code) {
    if (code == ERROR_CANCELLED)
        return "Windows administrator approval was cancelled.";
    if (code == ERROR_ALREADY_EXISTS) {
        try {
            auto conflicts = activeRegistrationConflicts(moduleDirectory() / L"OpenXRFovExtension.json");
            if (!conflicts.empty())
                return "Another active copy is already registered. Disable or uninstall that copy first: " +
                       utf8(conflicts.front().wstring());
        } catch (const std::exception &e) {
            return e.what();
        }
    }
    if (code == ERROR_INVALID_DATA) {
        try {
            validateManifest(moduleDirectory() / L"OpenXRFovExtension.json");
        } catch (const std::exception &e) {
            return e.what();
        }
    }
    if (code == ERROR_ACCESS_DENIED)
        return "Windows denied the registry write. Run the extracted package's installation helper with "
               "administrator approval.";
    return std::system_category().message(code);
}
std::wstring activeRuntime() {
    wchar_t b[32768]{};
    DWORD n = sizeof(b);
    auto r = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                          RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, b, &n);
    return r == ERROR_SUCCESS ? b : L"No active runtime registered";
}
int runRegistrationHelper(const std::wstring &action, bool elevate) {
    auto exe = moduleDirectory() / L"FovLayerControl.exe";
    SHELLEXECUTEINFOW s{sizeof(s)};
    s.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    s.lpVerb = elevate ? L"runas" : L"open";
    s.lpFile = exe.c_str();
    s.lpParameters = action.c_str();
    s.nShow = SW_HIDE;
    if (!ShellExecuteExW(&s))
        return (int)GetLastError();
    WaitForSingleObject(s.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(s.hProcess, &code);
    CloseHandle(s.hProcess);
    return (int)code;
}
} // namespace fov
