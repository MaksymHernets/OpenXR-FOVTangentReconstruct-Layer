#pragma once
#include "ISceneInterceptor.h"
#include <memory>
namespace fov {
// Automatic interception of the session device's immediate context. Deferred
// command lists and observed foreign NVAPI state are never overwritten.
class D3D11SceneInterceptor : public ISceneInterceptor {
    struct Impl;
    std::shared_ptr<Impl> impl;

  public:
    explicit D3D11SceneInterceptor(ID3D11Device *);
    ~D3D11SceneInterceptor();
    void target(uint32_t, uint32_t, uint32_t) override;
    void update(const std::array<FFRPlan, 2> &, bool) override;
    Json diagnostics() const override;
    class Suppress {
      public:
        Suppress();
        ~Suppress();
    };
};
} // namespace fov
