#pragma once
#include "common/Platform.h"
// Experimental developer-owned scene-recording integration, not an OpenXR extension.
// Version 1: DIRECT list outside render passes, single-sample stereo color target,
// initially 1x1/no VRS image. Begin before draws; End before reconstruction/compositor.
// Execute on the session-bound queue, signal the dedicated fence AFTER execution,
// and preserve resources until completion. Do not discard/reset the recorded list.
// Slot reuse and session destruction require observed completion; no waits inserted.
constexpr uint32_t FOV_FFR_COOPERATION_VERSION = 1;
struct FovD3D12FFRDrawInfo {
    uint32_t version = FOV_FFR_COOPERATION_VERSION;
    XrTime displayTime{};
    uint32_t eye{}, slot{};
    XrSwapchainSubImage subImage{};
    ID3D12GraphicsCommandList *commands{};
    ID3D12Fence *completionFence{};
    uint64_t completionValue{};
};
using PFN_xrFovBeginD3D12FFR = XrResult(XRAPI_PTR *)(XrSession, const FovD3D12FFRDrawInfo *);
using PFN_xrFovEndD3D12FFR = XrResult(XRAPI_PTR *)(XrSession, ID3D12GraphicsCommandList *);

// D3D11: caller serializes the session's immediate context, binds exactly one
// single-slice RTV and one matching viewport, and starts with NVAPI VRS disabled.
// Deferred contexts, simultaneous stereo RTVs and foreign VRS state are unsupported.
struct FovD3D11FFRDrawInfo {
    uint32_t version = FOV_FFR_COOPERATION_VERSION;
    XrTime displayTime{};
    uint32_t eye{};
    XrSwapchainSubImage subImage{};
    ID3D11DeviceContext *context{};
};
using PFN_xrFovBeginD3D11FFR = XrResult(XRAPI_PTR *)(XrSession, const FovD3D11FFRDrawInfo *);
using PFN_xrFovEndD3D11FFR = XrResult(XRAPI_PTR *)(XrSession, ID3D11DeviceContext *);
