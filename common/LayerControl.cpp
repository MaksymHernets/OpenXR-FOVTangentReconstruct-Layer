#include "Platform.h"
#include "RegistryManager.h"
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) {
        std::wcerr << L"Usage: FovLayerControl --enable|--disable|--uninstall|--status\n";
        return 2;
    }
    std::wstring a = argv[1];
    if (a != L"--enable" && a != L"--disable" && a != L"--uninstall" && a != L"--status")
        return 2;
    try {
        auto p = std::filesystem::absolute(fov::moduleDirectory() / L"OpenXRFovExtension.json");
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
        if (!IsUserAnAdmin())
            return fov::runRegistrationHelper(a, true);
        if (a == L"--uninstall")
            fov::unregisterLayer(p);
        else
            fov::setRegistration(p, a == L"--enable");
        std::wcout << L"Success: " << a << L" " << p.c_str() << L"\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
