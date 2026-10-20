#pragma once
#include "Platform.h"
#include <cmath>
#include <algorithm>
namespace fov {
inline XrFovf reduce(XrFovf v, float h, float y) {
    v.angleLeft = std::atan(std::tan(v.angleLeft) * h);
    v.angleRight = std::atan(std::tan(v.angleRight) * h);
    v.angleUp = std::atan(std::tan(v.angleUp) * y);
    v.angleDown = std::atan(std::tan(v.angleDown) * y);
    return v;
}
inline bool same(XrFovf a, XrFovf b) {
    return std::abs(a.angleLeft - b.angleLeft) < 1e-4f && std::abs(a.angleRight - b.angleRight) < 1e-4f &&
           std::abs(a.angleUp - b.angleUp) < 1e-4f && std::abs(a.angleDown - b.angleDown) < 1e-4f;
}
// Tangent coordinates: left -> right, up -> down. Works for asymmetric eyes.
inline void uvMapping(XrFovf original, XrFovf reduced, float *scale, float *bias) {
    const float l = std::tan(reduced.angleLeft), r = std::tan(reduced.angleRight),
                u = std::tan(reduced.angleUp), d = std::tan(reduced.angleDown);
    scale[0] = (std::tan(original.angleRight) - std::tan(original.angleLeft)) / (r - l);
    bias[0] = (std::tan(original.angleLeft) - l) / (r - l);
    scale[1] = (std::tan(original.angleUp) - std::tan(original.angleDown)) / (u - d);
    bias[1] = (u - std::tan(original.angleUp)) / (u - d);
}
} // namespace fov
