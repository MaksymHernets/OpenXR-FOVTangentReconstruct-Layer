#include "Platform.h"
#include "RegistryManager.h"
#include "ConfigManager.h"
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
#include <system_error>
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::wcerr << L"Usage: FovLayerControl --enable|--disable|--uninstall|--status|--check\n";
        return 2;
    }
    std::wstring a = argv[1];
    if (a != L"--enable" && a != L"--disable" && a != L"--uninstall" && a != L"--status" && a != L"--check")
        return 2;
    try {
        auto p = std::filesystem::absolute(fov::moduleDirectory() / L"OpenXRFovExtension.json");
        if (a == L"--check") {
            fov::validateManifest(p);
            auto conflicts = fov::activeRegistrationConflicts(p);
            if (!conflicts.empty())
                throw std::system_error(
                    ERROR_ALREADY_EXISTS, std::system_category(),
                    "Another active copy is already registered. Disable or uninstall that copy first: " +
                        fov::utf8(conflicts.front().wstring()));
            std::wcout << L"Ready to register: " << p.c_str() << L"\n";
            return 0;
        }
        if (a == L"--status") {
            auto s = fov::registration(p);
            std::wcout << (s == fov::Registration::Enabled    ? L"Enabled"
                           : s == fov::Registration::Disabled ? L"Disabled"
                           : s == fov::Registration::Missing  ? L"Unregistered"
                                                              : L"Invalid")
                       << L"\n";
            return 0;
        }
        if (a != L"--uninstall")
            fov::validateManifest(p);
        if (!IsUserAnAdmin()) {
            auto result = fov::runRegistrationHelper(a, true);
            if (result)
                std::cerr << fov::registrationFailureMessage(result) << "\n";
            return result;
        }
        if (a == L"--uninstall")
            fov::unregisterLayer(p);
        else
            fov::setRegistration(p, a == L"--enable");
        std::wcout << L"Success: " << a << L" " << p.c_str() << L"\n";
        return 0;
    } catch (const std::system_error &e) {
        std::cerr << e.what() << "\n";
        return e.code().value();
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return ERROR_INVALID_DATA;
    }
}
