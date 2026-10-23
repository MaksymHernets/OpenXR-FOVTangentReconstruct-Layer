# Changelog

## 0.1.9

- Added automatic D3D11 NVIDIA NVAPI scene FFR without requiring application cooperation calls.
- Intercepted immediate-context direct, indexed, instanced, automatic and indirect draws; filtered projection-sized render targets and viewports, including single-eye/stereo array views, packed stereo and 2x/4x/8x MSAA.
- Cached immutable maps, retained resources across live profile changes, preserved the conservative full-quality Center, and restored NVAPI state immediately after each eligible draw.
- Added GPU event completion counters, foreign-state refusal and conservative deferred-command-list exclusion; suppressed the layer's own reconstruction draws.
- Verified real shader-invocation reduction, central pixel equality in both eye slices at 1x/2x/4x/8x MSAA, instanced/indirect paths, overlay exclusion, array views, restoration and deferred exclusion on NVIDIA hardware.
- Confirmed GPU-completed automatic draws in Rumble through VDXR; the user accepted both-eye visuals with its 8x MSAA stereo targets.

## 0.1.8

- Fixed the RE3 VR mod integration gap: added automatic D3D12 Tier 2 scene interception rather than requiring application calls to the cooperation API.
- Observed RTV creation/copies, command-list Reset and viewports; applied immutable shading-rate maps around eligible direct scene draws and restored graphics state immediately.
- Added queue-fence execution evidence and UI draw counters; preserved foreign VRS state, skipped unsupported render-pass/indirect/bundle cases, and retained recorded maps with command-list lifetime owners.
- Cached map generation by projection/geometry/viewport rather than per draw; kept Outside reconstruction independent and retained 1x1 Outside shading where the original scene is still rendered.
- Allowed multiple local diagnostic readers so an open manager does not block regression tools.
- Added an actual hardware test with no cooperation API calls, pixel-shader workload queries, full-rate restoration and foreign-state checks.
- Supported RE Engine's MAX-per-primitive combiner without overwriting foreign images or coarse base rates; confirmed automatic GPU execution and user-accepted stereo visuals in the RE3 VR mod through VDXR.

## 0.1.7 — unreleased prototype

- Replaced full-FOV coarse Outer VRS with a reconstructed Outside mask, sharing one projection authority with cropped reconstruction and both VRS backends.
- Reused all six reconstruction shaders for elliptical/rectangular masks; preserved Center/Middle pixels and original projection, per-eye subrects and array layers.
- Added optional D3D11 NVIDIA NVAPI scene scopes with exact target/viewport checks, capability reporting and state restoration; kept D3D12 Tier 2 scopes and conservative tile boundaries.
- Accepted plain KHR depth attachments with every reconstruction algorithm, correcting an algorithm-dependent processing rejection.
- Added independent Center/Middle/mask/crop/input/output/composite and diagnostic color-fill views, explicit adaptive-blur/solid fallback, persistent controls and translations in all eleven languages.
- Added GPU readback, hardware workload, state-restoration and depth-attachment regression coverage. Outside scene exclusion and automatic game VRS integration remain unsupported; no Hubris/headset compatibility claim is made.

## 0.1.6

- Fixed registration after an older installation was disabled: only active same-layer registrations now block enabling a different copy. Disabled installations and other layers are preserved.
- Added read-only `FovLayerControl.exe --check`, specific helper exit codes and actionable registration diagnostics in the manager and installer.
- Added isolated registry tests covering upgrades, duplicate protection, Unicode paths and idempotent removal without changing system HKLM registrations.

## 0.1.5

- Added independent adaptive FFR settings and per-eye zone previews in all eleven languages, with persistent full/reduced-FOV quality settings.
- Selects three rendered zones at full effective FOV and two rendered zones plus existing reconstruction for additional crop, using runtime/returned FOV provenance and preserving streamer baselines.
- Added explicit D3D12 Tier 2 scene-recording VRS integration, capability/rate mapping, fence-protected map reuse and full-rate state restoration.
- Added real rasterized GPU workload/timing measurements, projection/UI/layer regressions, colored peripheral captures and compatibility documentation.
- Third-party scene integration and D3D11/Vulkan FFR remain unsupported. Existing D3D11/D3D12/Vulkan reconstruction is retained. Hubris/headset acceptance remains unverified.

## 0.1.4

- Added Vulkan/enable2 session detection, native image tracking, and explicit diagnostic passthrough for unverified applications.
- Added a cooperative Vulkan 1.1+ GPU backend with build-time validated SPIR-V for all six existing reconstruction methods, per-eye temporal/depth history, and direct/intermediate output paths.
- Added explicit producer-fence/layout cooperation, nonblocking command reuse, checked image usage/format support, and completion-protected cleanup. No Vulkan API interception is installed; third-party games remain in passthrough.
- Added Vulkan profile policy, detailed GPU/CPU/memory diagnostics, controlled GPU/OpenXR fixture tests, synchronization documentation, compatibility reports, frame evidence, and measured fixture overhead.
- Preserved D3D11/D3D12 processing, localization, algorithm-specific settings, and the application icon.

## 0.1.3

- Added the application icon from `Neon VR Focus Grid.png` for the executable, application windows, and taskbar.
- Embedded multiple Windows icon sizes and the original PNG so the icon works without separate image files.
- Added Windows executable version metadata for 0.1.3.

## 0.1.2

- Edge Fill now shows only parameters used by the selected algorithm, including only the color picker for Solid Color and only the reconstruction block for Temporal Depth Reconstruction. Hidden values are preserved when switching algorithms.

- Added a Settings tab with independent application language and regional locale selectors.
- Added English, German, French, Spanish, Simplified Chinese, Japanese, Russian, Hindi, Arabic, Bengali, and Korean interface text, with immediate switching that preserves unsaved profile edits.
- Added Arabic right-to-left layout, independent of the selected regional format; technical diagnostic panes retain left-to-right formatting.
- Added localized numeric controls, measurements, and UTC history dates with chronological sorting.
- Saved manager preferences independently from rendering profiles, with validated fallback for missing or malformed settings.
- Added automated localization, persistence, formatting, and editor-preservation tests.

## 0.1.1

- Added Temporal Depth Reconstruction to global and per-game settings on D3D11 and D3D12.
- Added independent eye history rings (2–8 frames), asymmetric pose reprojection, rotation-only operation without depth, and confidence-weighted adaptive blur fallback.
- Added validated `XR_KHR_composition_layer_depth` snapshots, D16/D24/D32/D32S8 depth formats, reversed-Z and infinite projection-plane interpretation. Narrow input depth is consumed by reconstruction and omitted from widened runtime output.
- Added history invalidation, recenter/session hooks, confidence/coverage/depth debugging, sampled GPU coverage counters, stage timings, and an optional per-eye GPU budget.
- Added controlled WARP reprojection tests, depth-swapchain hook tests, graphics validation checks, and in-flight history reuse tests.

## 0.1.0

- Added Solid Color edge fill for D3D11 and D3D12, defaulting to opaque black.
- Added a color picker and persisted per-game/global RGB settings.
- Preserved valid rendered content; only missing FOV regions receive the selected color.
- Added GPU checks for black/custom colors, unchanged center content, and sRGB encoding.
