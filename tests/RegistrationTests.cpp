#include "common/Platform.h"
#include "common/ConfigManager.h"
#include "common/RegistryManager.h"
#include <fstream>
#include <iostream>
using namespace fov;
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
class TestRegistry {
    HKEY root{};
    std::wstring path;

  public:
    TestRegistry() {
        path = L"Software\\OpenXRFovExtension\\RegistrationTests\\" + std::to_wstring(GetCurrentProcessId()) +
               L"-" + std::to_wstring(GetTickCount64());
        require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                                KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &root, nullptr) == ERROR_SUCCESS,
                "Create isolated user registry test key");
        require(RegOverridePredefKey(HKEY_LOCAL_MACHINE, root) == ERROR_SUCCESS,
                "Redirect HKLM inside test process only");
    }
    ~TestRegistry() {
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        if (root)
            RegCloseKey(root);
        if (path.starts_with(L"Software\\OpenXRFovExtension\\RegistrationTests\\"))
            RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};
static std::filesystem::path package(const std::filesystem::path &root, const std::wstring &name) {
    auto dir = root / name;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / L"OpenXRFovExtension.dll") << "fixture";
    auto file = dir / L"OpenXRFovExtension.json";
    updateJson(file, [](Json &j) {
        j = {{"api_layer",
              {{"name", "XR_APILAYER_FOV_extension"},
               {"disable_environment", "XR_FOV_DISABLE"},
               {"library_path", ".\\OpenXRFovExtension.dll"}}}};
    });
    return file;
}
int main() {
    try {
        TestRegistry registry;
        auto root = std::filesystem::absolute("test-data-registration");
        auto old = package(root, L"old package"), current = package(root, L"new package Ω"),
             third = package(root, L"third package");
        setRegistration(old, false);
        require(activeRegistrationConflicts(current).empty(), "Disabled copies are not active conflicts");
        setRegistration(current, true);
        require(registration(old) == Registration::Disabled && registration(current) == Registration::Enabled,
                "Disabled prior package must not block new registration");
        auto conflicts = activeRegistrationConflicts(third);
        require(conflicts.size() == 1 && conflicts.front() == current,
                "Report exact active conflicting package");
        setRegistration(current, true);
        require(registration(current) == Registration::Enabled, "Repeated enable is idempotent");
        bool blocked = false;
        try {
            setRegistration(third, true);
        } catch (const std::exception &e) {
            blocked = std::string(e.what()).find("already registered") != std::string::npos;
        }
        require(blocked && registration(third) == Registration::Missing,
                "Active duplicate must remain blocked without mutations");
        setRegistration(current, false);
        setRegistration(third, true);
        unregisterLayer(current);
        unregisterLayer(current);
        require(registration(current) == Registration::Missing &&
                    registration(old) == Registration::Disabled &&
                    registration(third) == Registration::Enabled,
                "Uninstall affects only selected copy and is idempotent");
        std::filesystem::remove(third.parent_path() / L"OpenXRFovExtension.dll");
        require(registration(third) == Registration::Enabled,
                "Registry status is independent of package file availability");
        unregisterLayer(third);
        require(registration(third) == Registration::Missing, "Broken package can still unregister");
        std::cout << "PASS: real registration functions in process-local HKLM override; disabled upgrade, "
                     "active conflict, Unicode paths, enable/disable/uninstall; system HKLM untouched\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
