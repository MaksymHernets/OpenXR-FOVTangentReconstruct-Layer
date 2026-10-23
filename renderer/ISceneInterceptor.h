#pragma once
#include "common/FFR.h"
namespace fov {
class ISceneInterceptor {
  public:
    virtual ~ISceneInterceptor() = default;
    virtual void target(uint32_t, uint32_t, uint32_t) = 0;
    virtual void update(const std::array<FFRPlan, 2> &, bool) = 0;
    virtual Json diagnostics() const = 0;
};
} // namespace fov
