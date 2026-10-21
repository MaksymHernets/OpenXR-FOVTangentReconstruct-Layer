# Vulkan architecture audit — baseline 0.1.3

Audit completed before implementation of 0.1.4. The baseline has no Vulkan backend, SPIR-V toolchain, Vulkan interception, or Vulkan GPU tests. This document distinguishes existing behavior from planned changes.

| Component | Existing implementation | Reuse / required changes |
|---|---|---|
| Loader negotiation | `layer/OpenXRLayer.cpp`, exported `xrNegotiateLoaderApiLayerInterface`, validated next-layer negotiation and exception boundaries | Preserve negotiation and downstream dispatch; add developer cooperation entry points without pretending to install a Vulkan layer. |
| Session detection | `hookCreateSession` scans the binding chain for D3D11 and D3D12 | Add Vulkan binding detection; the installed SDK aliases Vulkan2 structures and tags to Vulkan1. Validate handles, physical-device membership, queue indices and compute capability. |
| Swapchain interception | All six create/enumerate/acquire/wait/release/destroy hooks; FIFO acquisition state; a private output swapchain | Reuse lifecycle and provenance. Runtime images currently use COM pointers: add a Vulkan-specific native-image path and explicit image-state evidence. Never destroy runtime VkImages. |
| D3D11 renderer | `D3D11EdgeRenderer.cpp`: private source/history textures, context-state guard, pixel shader, asynchronous timestamps/counters | Preserve. Its COM resources and implicit context ordering are not a Vulkan synchronization model. |
| D3D12 renderer | `D3D12EdgeRenderer.cpp`: session DIRECT queue, allocator/descriptor ring, fences and state barriers | Preserve. Reuse the ring concept, not the Direct3D resource states or handles. |
| FOV and submission | Tangent-space reduction in `common/FovMath.h`; locate-time provenance; copied projection structures in end-frame; scaled subrect endpoints | Reuse mapping and stereo/subimage validation. Vulkan must retain native FOV and recommended sizes until processing has an explicit valid synchronization contract. |
| Reconstruction | Six methods in `shaders/EdgeFill.hlsl`, with bounded adaptive temporal reconstruction in `Temporal.hlsl` | Port to build-time GLSL/SPIR-V compute. Preserve central content, sRGB handling, eye slices, debug views and bounded sampling. |
| History and depth | `common/Temporal.h`: per-view keys, original-only observations, timestamps, pose/FOV invalidation, depth conventions | Reuse temporal metadata and transforms. Vulkan formats must not be interpreted as DXGI formats; depth sampling and copy aspects require their own validation. |
| Shader compilation | Runtime D3DCompile into shader-model 5 bytecode; CMake embeds HLSL | Vulkan needs build-time SPIR-V compilation and embedded modules. No Vulkan runtime compiler is present. |
| GPU ownership / synchronization | D3D source copy before release; separate output acquisition; private texture ownership and completion-protected reuse | Vulkan requires an explicit old layout, bound-queue ownership and producer-completion proof. OpenXR release alone is not a fence. Default to diagnostic passthrough when evidence is absent. |
| Profiles | `ConfigManager.cpp`: inherited JSON profiles, validated bounds, atomic read/modify/write | Reuse algorithm, FOV, color and temporal settings. Add a nested Vulkan enable / auto-or-passthrough policy, preserving it in the manager. |
| Diagnostics | Shared `IPCServer` worker reloads settings and emits process history, local logs and named-pipe JSON | Reuse worker; add Vulkan capabilities, image/acquisition state, fallback reasons and actual GPU/CPU timing without fabricating measurements. |
| Manager | Qt Widgets tabs, live process diagnostics, eleven languages, icon resources | Existing algorithm editor applies to Vulkan; display Vulkan in history/diagnostics without a separate application. |
| Build / distribution | C++20 Windows x64 CMake/Ninja, OpenXR 1.1.53 headers, vendored JSON, Qt packaging | Add pinned Vulkan headers and a build-time shader compiler. Load the installed Vulkan loader dynamically so non-Vulkan users do not acquire a mandatory loader dependency. |
| Tests | D3D11/D3D12 WARP shader and fake-runtime dispatch tests; GUI localization tests | Keep all regressions. Add real Vulkan GPU execution plus a controlled OpenXR runtime fixture, separate-eye/array mappings, synchronization refusal cases, pixel evidence and measured overhead. |

## Synchronization decision

The OpenXR Vulkan extension specifies queue ownership and a layout *compatible* with attachment layouts at wait/release boundaries, and externally synchronized access to the bound queue for acquire/release/end-frame. It does not expose a producer fence or uniquely identify every legal application layout. A conforming application may also use additional Vulkan queues and optional synchronization facilities that this OpenXR layer does not observe.

The initial processing path therefore requires explicit cooperation from a developer-owned application: it identifies the exact released image layout, bound queue and a completed producer fence. The layer validates that evidence before touching the image. Ordinary third-party Vulkan applications use diagnostic passthrough, with unchanged view FOV and native size recommendations. An OpenXR-only implementation must not claim to hook `vkQueueSubmit`, submit2, barriers, render passes or dynamic rendering. General automatic interception remains a separate Vulkan-layer integration milestone.

## Sources

- [OpenXR Vulkan binding](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrGraphicsBindingVulkanKHR.html)
- [OpenXR Vulkan swapchain images](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSwapchainImageVulkanKHR.html)
- [Normative Vulkan concurrency and image-state contract](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/khr/khr_vulkan_enable.adoc)
- [Vulkan synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)

These sources establish constraints; they do not establish executed game/runtime compatibility.
