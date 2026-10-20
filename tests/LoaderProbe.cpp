#include "common/Platform.h"
#include "common/ConfigManager.h"
#include <iostream>
#include <vector>
#include <cstring>
int wmain(int argc, wchar_t **argv) {
    if (argc < 3) {
        std::cerr
            << "Usage: LoaderProbe <official openxr_loader.dll> <package directory> [--create-instance]\n";
        return 2;
    }
    auto directory = std::filesystem::absolute(argv[2]);
    SetEnvironmentVariableW(L"XR_API_LAYER_PATH", directory.c_str());
    auto data = directory.parent_path() / L"probe-data";
    SetEnvironmentVariableW(L"FOV_DATA_DIR", data.c_str());
    HMODULE loader = LoadLibraryW(argv[1]);
    if (!loader) {
        std::cerr << "Cannot load official loader: " << GetLastError() << "\n";
        return 1;
    }
    auto enumerate = reinterpret_cast<PFN_xrEnumerateApiLayerProperties>(
        GetProcAddress(loader, "xrEnumerateApiLayerProperties"));
    if (!enumerate)
        return 1;
    uint32_t count = 0;
    auto r = enumerate(0, &count, nullptr);
    if (XR_FAILED(r)) {
        std::cerr << "Layer enumeration failed: " << r << "\n";
        return 1;
    }
    std::vector<XrApiLayerProperties> properties(count, {XR_TYPE_API_LAYER_PROPERTIES});
    r = enumerate(count, &count, properties.data());
    bool found = false;
    for (auto &p : properties) {
        std::cout << p.layerName << "\n";
        found |= !strcmp(p.layerName, "XR_APILAYER_FOV_extension");
    }
    if (XR_FAILED(r) || !found) {
        std::cerr << "Layer not discovered\n";
        return 1;
    }
    std::cout << "PASS: official loader discovered packaged manifest\n";
    if (argc >= 4 && !wcscmp(argv[3], L"--create-instance")) {
        auto create = reinterpret_cast<PFN_xrCreateInstance>(GetProcAddress(loader, "xrCreateInstance"));
        auto destroy = reinterpret_cast<PFN_xrDestroyInstance>(GetProcAddress(loader, "xrDestroyInstance"));
        const char *layers[] = {"XR_APILAYER_FOV_extension"};
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ci.applicationInfo.applicationName, "FOV loader probe");
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ci.enabledApiLayerCount = 1;
        ci.enabledApiLayerNames = layers;
        XrInstance instance{};
        r = create(&ci, &instance);
        std::cout << "Active runtime xrCreateInstance result: " << r << "\n";
        if (XR_FAILED(r))
            return 3;
        auto g = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
        PFN_xrGetInstanceProperties get{};
        g(instance, "xrGetInstanceProperties", reinterpret_cast<PFN_xrVoidFunction *>(&get));
        XrInstanceProperties info{XR_TYPE_INSTANCE_PROPERTIES};
        get(instance, &info);
        std::cout << "Runtime: " << info.runtimeName << "\n";
        r = destroy(instance);
        if (XR_FAILED(r))
            return 1;
        std::cout << "PASS: instance creation and destruction through layer and active runtime\n";
    }
    FreeLibrary(loader);
    return 0;
}
