# FFR audit — 0.1.8

## Reference review

Reviewed [OpenXR Toolkit vrs.cpp](https://github.com/mbucchia/OpenXR-Toolkit/blob/main/XR_APILAYER_MBUCCHIA_toolkit/vrs.cpp) and its [MIT license](https://github.com/mbucchia/OpenXR-Toolkit/blob/main/LICENSE). Toolkit generates per-eye tiled maps, handles double-wide/array targets, caches masks and updates projection centers. Its render-target callbacks filter candidates by dimensions/aspect ratio and enable or disable VRS around eligible rendering. D3D11 uses NVAPI viewport tables/resource views; D3D12 uses command-list shading-rate images. It saves/restores its own graphics work and explicitly disables scene VRS. These dependencies extend beyond an end-frame image filter. Its cull/outer semantics are not this project's Outside policy.

No Toolkit source is copied. The D3D11 backend uses the official [NVIDIA NVAPI declarations](https://github.com/NVIDIA/nvapi/blob/70d337db9186e968eab622f7e786de7e437faf3d/nvapi.h) and interface IDs at pinned commit `70d337db9186e968eab622f7e786de7e437faf3d`. Headers are MIT-licensed, downloaded by a checksum-verifying bootstrap; driver entry points are dynamically resolved from System32. No driver DLL or import library is shipped. Full notice: `licenses/NVAPI.txt`.

## Repository findings and changes

Previously, `allowed()` disabled reconstruction at full FOV, shaders recognized only rectangular projection overflow, and the full-FOV rate map assigned coarse rates to Outer. These independently prevented the requested behavior. `OutsideRegionManager` now owns projection-relative geometry shared by FFR and shader constants. Masked sampling clamps to valid interior observations, while unchanged Center/Middle samples bypass reconstruction. The existing six algorithms remain the only reconstruction implementations.

Plain KHR depth chains were previously accepted only for temporal reconstruction. Other algorithms skipped that entire projection. They now accept that chain and remove it from reconstructed output; they do not misrepresent narrow original depth as reconstructed depth. This is a regression fix based on code and controlled tests, not proof of the reported Hubris symptom's exact cause.

## Integration and exclusion limits

The 0.1.7 prototype did not intercept game draws: only explicit application cooperation activated VRS. The actual RE3 logs showed D3D12, successful reconstruction and `integrationRequired=true`. This explains the reported difference from Toolkit; hardware capability and fixture passes were insufficient to establish game integration.

0.1.8 adds D3D12 scene interception using pinned MinHook 1.3.4 (BSD notices included). It observes device RTV creation and descriptor copies, command-list Reset/Close/ClearState, render-target binding, viewports, VRS state, direct draws and queue submissions. Candidate dimensions derive from actual submitted stereo subimages; proportional intermediate targets are filtered by aspect ratio and scale. Unknown mono-eye identity uses a conservative union of both full-quality centers. Packed targets have separate eye halves.

Only known same-device DIRECT recordings with a 1x1 base and no foreign rate image are eligible. Passthrough combiners and RE Engine's MAX-per-primitive/passthrough pair are supported; the latter composes the image with MAX and restores the original pair. Render passes, foreign VRS state, partial/overlay viewports, unsupported arrays, indirect draws and bundles are skipped. State is restored after each intercepted draw. Immutable map upload commands run privately; a map is never bound until its initialization fence completes. Each observed execution queue receives a completion marker after a submission containing VRS draws. These counters prove submitted commands completed; they do not claim per-game pixel-shader invocation savings or FPS.

D3D11 retains explicit NVAPI cooperation. D3D12's explicit scene API remains available. The automatic backend does not modify game files, resource depth/stencil, geometry or persistent graphics settings. The interceptor module stays loaded until process exit so command-list private COM map owners have valid vtables. Hooks detach before downstream session teardown. Controlled hardware tests cover direct rendering without API cooperation, workload reduction, state restoration, foreign-state refusal and cache behavior.

No safe, verified integration exists here to exclude game shading in Outside. No depth/stencil, render-pass or geometry modifications are introduced. Outside stays 1x1 when rendered and is visually reconstructed afterward. Diagnostics report zero Outside scene savings. NVAPI culling is deliberately absent from the lookup table. Vulkan scene VRS remains unsupported because required device features/render-pass ownership are not available. Existing Vulkan reconstruction still requires exact producer synchronization cooperation.
