#include "D3D12FFR.h"
#include <array>
#include <cstring>
namespace fov {
struct D3D12FFR::Impl {
    ComPtr<ID3D12Device> device;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
    struct Slot {
        ComPtr<ID3D12Resource> image, upload;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12GraphicsCommandList5> command;
        uint64_t completion = 0;
        uint32_t width = 0, height = 0, pitch = 0;
        bool recording = false;
        std::vector<uint8_t> cached;
    };
    std::array<Slot, 8> slots;
    bool supported = false;
    bool complete(const Slot &s) const {
        return !s.recording && (!s.fence || (s.fence->GetCompletedValue() != UINT64_MAX &&
                                             s.fence->GetCompletedValue() >= s.completion));
    }
};
D3D12FFR::D3D12FFR(ID3D12Device *d) : impl(std::make_unique<Impl>()) {
    if (!d)
        throw std::runtime_error("Missing D3D12 FFR device");
    impl->device = d;
    impl->supported = SUCCEEDED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &impl->options,
                                                       sizeof(impl->options))) &&
                      impl->options.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2 &&
                      impl->options.ShadingRateImageTileSize && d->GetNodeCount() == 1;
}
D3D12FFR::~D3D12FFR() {
    // An invalid caller must not cause destruction of possibly in-flight maps.
    // Normal layer teardown refuses while these lifetimes lack completion proof.
    for (auto &s : impl->slots)
        if (!impl->complete(s)) {
            s.image.Detach();
            s.upload.Detach();
            s.fence.Detach();
            s.command.Detach();
        }
}
Json D3D12FFR::capabilities() const {
    return {{"backend", "D3D12 Tier 2 VRS"},
            {"hardwareSupported", impl->supported},
            {"sceneIntegrationSupported", impl->supported},
            {"tileSize", impl->options.ShadingRateImageTileSize},
            {"additionalRates", bool(impl->options.AdditionalShadingRatesSupported)},
            {"supportedRates", impl->supported
                                   ? (impl->options.AdditionalShadingRatesSupported
                                          ? Json::array({"1x1", "1x2", "2x1", "2x2", "2x4", "4x2", "4x4"})
                                          : Json::array({"1x1", "1x2", "2x1", "2x2"}))
                                   : Json::array()},
            {"integrationRequired", true},
            {"reason", impl->supported ? "Explicit scene-recording cooperation required"
                                       : "Single-node D3D12 VRS Tier 2 required"}};
}
bool D3D12FFR::readyForDestroy() const {
    for (auto &s : impl->slots)
        if (!impl->complete(s))
            return false;
    return true;
}
bool D3D12FFR::active() const {
    for (auto &s : impl->slots)
        if (s.recording)
            return true;
    return false;
}
Json D3D12FFR::begin(ID3D12GraphicsCommandList *list, uint32_t index, ID3D12Fence *fence, uint64_t completion,
                     const FFRPlan &plan, uint32_t width, uint32_t height, XrRect2Di viewport) {
    if (!impl->supported)
        throw std::runtime_error("FFR unsupported: VRS Tier 2 required");
    if (!plan.settings.enabled || !plan.valid || !list || !fence || index >= impl->slots.size() ||
        list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        throw std::runtime_error("Invalid FFR scene scope");
    ComPtr<ID3D12Device> owner;
    check(list->GetDevice(IID_PPV_ARGS(&owner)));
    if (owner.Get() != impl->device.Get())
        throw std::runtime_error("FFR command list belongs to another device");
    check(fence->GetDevice(IID_PPV_ARGS(&owner)));
    if (owner.Get() != impl->device.Get())
        throw std::runtime_error("FFR fence belongs to another device");
    auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX || !completion || completion == UINT64_MAX || completed >= completion)
        throw std::runtime_error("FFR requires a future dedicated queue fence value");
    for (auto &s : impl->slots) {
        if (s.recording && s.command.Get() == list)
            throw std::runtime_error("Nested FFR scope");
        if (s.fence.Get() == fence && s.completion > completion)
            throw std::runtime_error("FFR fence values must increase");
    }
    auto &s = impl->slots[index];
    if (!impl->complete(s))
        throw std::runtime_error("FFR map busy: no wait or overwrite");
    ComPtr<ID3D12GraphicsCommandList5> command;
    check(list->QueryInterface(IID_PPV_ARGS(&command)));
    uint32_t tile = impl->options.ShadingRateImageTileSize, cols = (width + tile - 1) / tile,
             rows = (height + tile - 1) / tile;
    auto map = ffrRateMap(plan, width, height, tile, viewport, impl->options.AdditionalShadingRatesSupported);
    bool allocated = false;
    if (s.width != cols || s.height != rows) {
        // The prior use is complete. Allocate transactionally before recording.
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = cols;
        desc.Height = rows;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UINT;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> image, upload;
        check(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&image)));
        uint32_t pitch = (cols + 255) & ~255u;
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = uint64_t(pitch) * rows;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(impl->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&upload)));
        s.image = image;
        s.upload = upload;
        s.width = cols;
        s.height = rows;
        s.pitch = pitch;
        s.cached.clear();
        allocated = true;
    }
    // Finish fallible diagnostics before recording application commands.
    auto report = ffrPlanJson(plan);
    report["recordedRates"] = {"1x1", ffrRateName(ffrRate(plan.middleQuality, plan.settings.policy,
                                                          impl->options.AdditionalShadingRatesSupported))};
    report["state"] = "Scene commands recorded; execution not observed by layer";
    report["hardwareApplicationConfirmed"] = false;
    report["mapAllocated"] = allocated;
    if (s.cached != map) {
        void *mapped = nullptr;
        D3D12_RANGE read{0, 0};
        check(s.upload->Map(0, &read, &mapped));
        for (uint32_t y = 0; y < rows; ++y)
            std::memcpy(static_cast<uint8_t *>(mapped) + size_t(y) * s.pitch, map.data() + size_t(y) * cols,
                        cols);
        s.upload->Unmap(0, nullptr);
        auto barrier = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {s.image.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
            command->ResourceBarrier(1, &b);
        };
        if (!s.cached.empty())
            barrier(D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = s.image.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s.upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UINT, cols, rows, 1, s.pitch};
        command->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
        s.cached = std::move(map);
    }
    D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                            D3D12_SHADING_RATE_COMBINER_OVERRIDE};
    command->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
    command->RSSetShadingRateImage(s.image.Get());
    s.command = command;
    s.fence = fence;
    s.completion = completion;
    s.recording = true;
    return report;
}
void D3D12FFR::end(ID3D12GraphicsCommandList *list) {
    for (auto &s : impl->slots)
        if (s.recording && s.command.Get() == list) {
            D3D12_SHADING_RATE_COMBINER combiners[]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                    D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
            s.command->RSSetShadingRateImage(nullptr);
            s.command->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
            s.recording = false;
            s.command.Reset();
            return;
        }
    throw std::runtime_error("No matching FFR scope");
}
} // namespace fov
