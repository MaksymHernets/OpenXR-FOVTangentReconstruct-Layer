#pragma once
#include "common/FFR.h"
#include <memory>
namespace fov {
// Intercepts only same-device DIRECT scene draws. Maps are immutable GPU resources;
// foreign VRS state and render passes are left untouched. No application opt-in API.
class D3D12SceneInterceptor {
    struct Impl;
    std::shared_ptr<Impl> impl;

  public:
    D3D12SceneInterceptor(ID3D12Device *, ID3D12CommandQueue *);
    ~D3D12SceneInterceptor();
    void target(uint32_t width, uint32_t height, uint32_t arrays);
    void update(const std::array<FFRPlan, 2> &, bool enabled);
    Json diagnostics() const;
    // Exclude layer reconstruction and explicitly cooperating recordings.
    class Suppress {
      public:
        Suppress();
        ~Suppress();
    };
};
} // namespace fov
