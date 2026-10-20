#pragma once
#include "common/Platform.h"
#include "common/ConfigManager.h"
#include "common/FovMath.h"
#include "common/Temporal.h"
#include <memory>
#include <optional>
namespace fov {
struct alignas(16) ShaderConstants {
    float scale[2], bias[2];
    float rect[4];
    float texel[2];
    unsigned slice, method;
    float stretchWidth, gradientStrength, blurRadius, blurStrength;
    unsigned edgeSamples;
    float padding[3]{};
    float fillColor[4]{0, 0, 0, 1};
};
static_assert(sizeof(ShaderConstants) == 96);
inline void setFillColor(ShaderConstants &c, const Profile &p, DXGI_FORMAT format) {
    const bool srgb = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for (size_t i = 0; i < 3; i++) {
        float value = p.color[i];
        // The color picker stores display RGB; sRGB RTVs apply encoding on write.
        c.fillColor[i] =
            srgb ? (value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f)) : value;
    }
    c.fillColor[3] = 1;
}
inline ShaderConstants constants(const XrCompositionLayerProjectionView &view, XrFovf original,
                                 const XrSwapchainCreateInfo &ci, const Profile &p) {
    ShaderConstants c{};
    uvMapping(original, view.fov, c.scale, c.bias);
    auto r = view.subImage.imageRect;
    c.rect[0] = (float)r.offset.x / ci.width;
    c.rect[1] = (float)r.offset.y / ci.height;
    c.rect[2] = (float)r.extent.width / ci.width;
    c.rect[3] = (float)r.extent.height / ci.height;
    c.texel[0] = 1.f / ci.width;
    c.texel[1] = 1.f / ci.height;
    c.slice = view.subImage.imageArrayIndex;
    c.method = p.method;
    c.stretchWidth = p.stretchWidth;
    c.gradientStrength = p.gradientStrength;
    c.blurRadius = (float)p.blurRadius;
    c.blurStrength = p.blurStrength;
    c.edgeSamples = p.edgeSamples;
    c.padding[0] = p.showFovBorders ? 1.f : 0.f;
    setFillColor(c, p, static_cast<DXGI_FORMAT>(ci.format));
    return c;
}
inline bool supportedFormat(int64_t f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R10G10B10A2_UNORM;
}
class IEdgeRenderer {
  public:
    virtual ~IEdgeRenderer() = default;
    virtual void capture(uint64_t key, IUnknown *source, DXGI_FORMAT format) = 0;
    virtual void render(uint64_t key, IUnknown *output, DXGI_FORMAT format, const ShaderConstants &,
                        const XrRect2Di &destination) = 0;
    virtual void forget(uint64_t key) = 0;
    virtual void storeHistory(uint64_t source, uint64_t destination, const XrSwapchainSubImage &region) = 0;
    virtual void setTemporal(const TemporalInput &input) = 0;
    virtual TemporalMetrics temporalMetrics() const = 0;
    virtual void setTiming(bool enabled) = 0;
    virtual std::optional<double> gpuMilliseconds() const = 0;
    virtual std::optional<double> sourceCopyMilliseconds() const = 0;
    virtual std::optional<double> historyCopyMilliseconds() const = 0;
};
std::unique_ptr<IEdgeRenderer> makeD3D11(ID3D11Device *);
std::unique_ptr<IEdgeRenderer> makeD3D12(ID3D12Device *, ID3D12CommandQueue *);
ComPtr<ID3DBlob> compileShader(const char *entry, const char *target);
} // namespace fov
