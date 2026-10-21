#include "D3D11FFR.h"
#include <nvapi.h>
#include <array>
namespace fov {
namespace {
struct NvApi {
    // Loaded only from System32. Windows owns the module until process exit;
    // never unload global NVAPI state used by the host or another API layer.
    HMODULE module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Query = void *(__cdecl *)(unsigned);
    Query query = module ? reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface")) : nullptr;
    template <class T> T get(unsigned id) {
        return query ? reinterpret_cast<T>(query(id)) : nullptr;
    }
    decltype(&NvAPI_Initialize) initialize = get<decltype(initialize)>(0x0150e828);
    decltype(&NvAPI_D3D1x_GetGraphicsCapabilities) caps = get<decltype(caps)>(0x52b1499a);
    decltype(&NvAPI_D3D11_RSSetViewportsPixelShadingRates) rates = get<decltype(rates)>(0x34f7938f);
    decltype(&NvAPI_D3D11_CreateShadingRateResourceView) create = get<decltype(create)>(0x99ca2dff);
    decltype(&NvAPI_D3D11_RSSetShadingRateResourceView) bind = get<decltype(bind)>(0x1b0c2f83);
    bool ready = initialize && caps && rates && create && bind && initialize() == NVAPI_OK;
};
NvApi &nv() {
    static NvApi api;
    return api;
}
void checkNv(NvAPI_Status status) {
    if (status != NVAPI_OK)
        throw std::runtime_error("NVAPI VRS failed: " + std::to_string(int(status)));
}
} // namespace
struct D3D11FFR::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> map;
    ComPtr<ID3D11NvShadingRateResourceView> view;
    std::vector<uint8_t> cached;
    uint32_t width = 0, height = 0;
    bool supported = false, recording = false;
};
D3D11FFR::D3D11FFR(ID3D11Device *device) : impl(std::make_unique<Impl>()) {
    if (!device)
        throw std::runtime_error("Missing D3D11 device");
    impl->device = device;
    NV_D3D1x_GRAPHICS_CAPS caps{};
    impl->supported = nv().ready && nv().caps(device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps) == NVAPI_OK &&
                      caps.bVariablePixelRateShadingSupported;
}
D3D11FFR::~D3D11FFR() {
    if (impl->recording) {
        try {
            end(impl->context.Get());
        } catch (...) { /* Device loss: no further application work. */
        }
    }
}
bool D3D11FFR::active() const {
    return impl->recording;
}
Json D3D11FFR::capabilities() const {
    return {{"backend", "D3D11 NVIDIA NVAPI VRS"},
            {"hardwareSupported", impl->supported},
            {"sceneIntegrationSupported", impl->supported},
            {"integrationRequired", true},
            {"tileSize", 16},
            {"supportedRates", impl->supported ? Json::array({"1x1", "1x2", "2x1", "2x2"}) : Json::array()},
            {"reason", impl->supported ? "Explicit immediate-context scene cooperation required"
                                       : "NVAPI variable pixel shading unavailable"}};
}
Json D3D11FFR::begin(ID3D11DeviceContext *context, const FFRPlan &plan, uint32_t width, uint32_t height,
                     XrRect2Di viewport) {
    if (!impl->supported)
        throw std::runtime_error("NVAPI VRS unavailable");
    if (impl->recording || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !plan.settings.enabled)
        throw std::runtime_error("Invalid or nested D3D11 scene scope");
    ComPtr<ID3D11Device> owner;
    context->GetDevice(&owner);
    if (owner.Get() != impl->device.Get())
        throw std::runtime_error("Foreign D3D11 context");
    auto map = ffrRateMap(plan, width, height, 16, viewport, false);
    // Indices into our NVAPI table deliberately match D3D12 encodings. No cull rate.
    uint32_t cols = (width + 15) / 16, rows = (height + 15) / 16;
    bool allocated = false;
    if (cols != impl->width || rows != impl->height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = cols;
        desc.Height = rows;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UINT;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> image;
        check(impl->device->CreateTexture2D(&desc, nullptr, &image));
        NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
        vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
        vd.Format = desc.Format;
        vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11NvShadingRateResourceView> view;
        checkNv(nv().create(impl->device.Get(), image.Get(), &vd, &view));
        impl->map = image;
        impl->view = view;
        impl->width = cols;
        impl->height = rows;
        impl->cached.clear();
        allocated = true;
    }
    auto result = ffrPlanJson(plan);
    result["recordedRates"] = {"1x1", ffrRateName(ffrRate(plan.middleQuality, plan.settings.policy, false))};
    result["hardwareApplicationConfirmed"] = false;
    result["mapAllocated"] = allocated;
    result["state"] = "Immediate-context VRS bound; GPU execution not observed by layer";
    if (map != impl->cached) {
        context->UpdateSubresource(impl->map.Get(), 0, nullptr, map.data(), cols, 0);
        impl->cached = map;
    }
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC table{};
    table.enableVariablePixelShadingRate = true;
    for (auto &rate : table.shadingRateTable)
        rate = NV_PIXEL_X1_PER_RASTER_PIXEL;
    table.shadingRateTable[1] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
    table.shadingRateTable[4] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
    table.shadingRateTable[5] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC desc{};
    desc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    desc.numViewports = 1;
    desc.pViewports = &table;
    checkNv(nv().bind(context, impl->view.Get()));
    impl->context = context;
    impl->recording = true;
    auto status = nv().rates(context, &desc);
    if (status != NVAPI_OK) {
        end(context);
        checkNv(status);
    }
    return result;
}
void D3D11FFR::end(ID3D11DeviceContext *context) {
    if (!impl->recording || context != impl->context.Get())
        throw std::runtime_error("D3D11 FFR scope mismatch");
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC disabled{};
    disabled.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    // Attempt both restoration operations even if the device fails one.
    auto rates = nv().rates(context, &disabled);
    auto image = nv().bind(context, nullptr);
    checkNv(rates);
    checkNv(image);
    impl->recording = false;
    impl->context.Reset();
}
} // namespace fov
