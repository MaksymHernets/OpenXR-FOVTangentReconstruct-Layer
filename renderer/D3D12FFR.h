#pragma once
#include "common/FFR.h"
#include <memory>
namespace fov {
class D3D12FFR {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit D3D12FFR(ID3D12Device *);
    ~D3D12FFR();
    Json capabilities() const;
    bool readyForDestroy() const;
    bool active() const;
    // Borrowed DIRECT command list, outside render passes, initially 1x1/no image.
    // A dedicated monotonic queue fence must be signaled AFTER this list executes.
    // No CPU signal, unrelated queue signal or fence reset is permitted.
    Json begin(ID3D12GraphicsCommandList *, uint32_t slot, ID3D12Fence *, uint64_t completion,
               const FFRPlan &, uint32_t width, uint32_t height, XrRect2Di viewport);
    void end(ID3D12GraphicsCommandList *);
};
} // namespace fov
