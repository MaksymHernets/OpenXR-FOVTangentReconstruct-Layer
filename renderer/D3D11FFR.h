#pragma once
#include "common/Platform.h"
#include "common/FFR.h"
#include <memory>
namespace fov {
// Immediate-context scope with caller-owned, initially disabled NVAPI state.
class D3D11FFR {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit D3D11FFR(ID3D11Device *);
    ~D3D11FFR();
    Json capabilities() const;
    Json begin(ID3D11DeviceContext *, const FFRPlan &, uint32_t, uint32_t, XrRect2Di);
    void end(ID3D11DeviceContext *);
    bool active() const;
};
} // namespace fov
