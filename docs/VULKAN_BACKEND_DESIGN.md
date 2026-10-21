# Vulkan backend design — 0.1.4

The existing Windows x64 C++20 OpenXR layer gains a Vulkan implementation of `IEdgeRenderer`. D3D11/D3D12 retain their existing entry points and HLSL. `captureVulkan` / `renderVulkan` carry native `VkImage`, creation information and explicit layout instead of coercing Vulkan resources into COM pointers. Shared `ShaderConstants`, `TemporalHistory`, `TemporalGpu`, projection mapping, profiles and IPC remain in use.

## Initialization and capabilities

`VulkanContext` loads the system `vulkan-1.dll` from System32 and resolves instance/device dispatch. It borrows the application's instance, physical device, device and bound queue; it does not create a second device. Null handles, physical-device membership, physical API support, queue-family/index bounds and graphics/compute queue capability are checked. The application remains responsible for supplying valid created device/queue handles and initializing Vulkan 1.1 or later. Device creation parameters cannot be reconstructed from arbitrary borrowed handles.

The backend uses Vulkan 1.1 core facilities, ordinary fences/barriers, fixed descriptor arrays and literal shader resource indices. It does not enable device extensions, synchronization2, timeline semaphores, descriptor indexing, multiview shaders or optional device features after device creation. Physical format capabilities are checked before processing. The combined temporal pipeline needs at least 17 sampled-image/sampler descriptors per compute stage; devices below that bound use passthrough, including for simpler modes in this release.

Backend GPU resources are allocated only after explicit developer cooperation is enabled. Detection-only sessions query the borrowed binding but do not create optimizer pipelines or submission buffers.

## Resources and submissions

`VulkanEdgeRenderer` owns private sampled source/history images, optional RGBA8 storage scratch images, image views, one compute pipeline, two samplers and a 32-slot command/descriptor/uniform/fence ring. Runtime images and all application Vulkan handles remain borrowed. A slot is polled with `vkGetFenceStatus`; pending slots are skipped without waiting. Ring exhaustion reports nonblocking fallback. Format/extent changes and teardown drain optimizer fences before releasing resources. An unexpected teardown wait failure retains resources rather than freeing potentially pending objects; device loss follows Vulkan's device-loss cleanup rules.

Only declaration-backed source images may be copied before their OpenXR release. End-frame samples these private snapshots, never a released runtime source image. Original subregions are copied into per-eye history after reconstruction. Output is a separate acquired/waited OpenXR swapchain, following the existing architecture.

Direct storage is restricted to accepted RGBA8 UNORM output storage usage. The storage view covers exactly one array layer so its descriptor layout agrees with every subresource in that view. Other accepted outputs use a private RGBA8 compute target followed by a transfer copy. Packed views preserve earlier regions; each array layer is tracked independently.

## Build-time shaders and shared layout

Bootstrap pins Vulkan-Headers v1.3.290 (`b379292b2ab6df5771ba9870d53cf8b2c9295daf`) and the verified official glslang 16.6.0 Windows archive. CMake compiles `shaders/Vulkan.comp` using:

```text
glslang --target-env vulkan1.1 --spirv-val -V shaders/Vulkan.comp -o Vulkan.spv
```

`scripts/embed-spirv.cmake` verifies the SPIR-V header and emits an aligned `uint32_t` module. The module is embedded into the layer; no runtime shader compiler or SDK is shipped or required. Override `VULKAN_INCLUDE_DIR` and `FOV_GLSLANG` to use installed compatible tools.

The std140 parameter buffer is 848 bytes, checked with C++ static assertions:

| Offset | Content |
|---:|---|
| 0 | 96-byte shared source mapping, subrect, slice, method, fill and blur parameters |
| 96 | 720-byte shared temporal parameters, including eight 80-byte history entries |
| 816 | Four 32-bit destination offset/extent values |
| 832 | Four 32-bit output layer, sRGB and channel-order flags |

Bindings are source sampler (0), output storage image (1), parameters (2), eight history-color samplers (3), eight history-depth samplers (4), and four diagnostic counters (5). Depth uses nearest sampling; source and color history use bounded linear sampling. No nonuniform descriptor indexing is used.

## Configuration and manager

The existing global and per-game profile schema gains:

```json
"vulkan": { "enabled": true, "processing": "auto" }
```

`auto` permits only the verified cooperative path; it does not authorize unknown third-party synchronization. `enabled=false` or `processing="passthrough"` disables Vulkan FOV changes and reconstruction without changing D3D behavior. Invalid values inherit validated defaults. The manager preserves this nested policy when saving its existing algorithm controls. Edit these advanced Vulkan options in the profile JSON; no unverified automatic-processing toggle is presented in the manager.

The existing named-pipe diagnostics show Vulkan as the graphics API, device/queue details, swapchain dimensions/usage/format, acquisition evidence, fallback reasons, memory/allocation/submission counters, per-view CPU and GPU measurements, and temporal coverage. Unsupported extensions/functions continue to downstream OpenXR dispatch.
