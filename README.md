# OpenXR FOV Tangent Reconstruct Layer

Version **0.1.8**. Windows x64 implicit OpenXR API layer and Qt 6 **OpenXR FOV Manager**. The layer narrows each eye's projection in tangent space, recommends smaller render dimensions, and reconstructs the original projection using a separate runtime-owned output swapchain. Clamp, Gradient, Blur, Mirror, Solid Color, and **Temporal Depth Reconstruction** are implemented as GPU shaders for D3D11 and D3D12, with an experimental cooperative Vulkan 1.1+ compute backend.

This is an initial implementation with local GPU and dispatch tests. **The RE3 VR mod was checked live through VDXR on D3D12: automatic FFR draws completed and the user confirmed both eyes look correct at 80% FOV with Blur. Broader visual coverage, game performance measurements, and long-running compatibility testing remain required.** See [testing](docs/TESTING.md) and [design and limitations](docs/DESIGN.md). Do not interpret a successful build as headset certification.

## Build

Requirements: Windows 10/11 x64, Visual Studio 2022 or newer with Desktop development with C++ and a Windows 10/11 SDK, CMake 3.24+, Qt 6.5+ MSVC x64 (Widgets and Network). Bootstrap also installs pinned Vulkan headers and a verified glslang shader compiler for build-time SPIR-V validation. Python, glslang and the Vulkan SDK are not required to run the packaged product. The system Vulkan loader/driver is needed only for Vulkan sessions.

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1 -QtPrefix 'C:/Qt/6.8.3/msvc2022_64'
./scripts/package.ps1 -QtPrefix 'C:/Qt/6.8.3/msvc2022_64'
```

The Qt path is an example; supply your installed Qt SDK root. The build script locates Visual Studio through `vswhere`, initializes its x64 tools, builds with Ninja, and runs CTest. You can also use CMake directly from an x64 developer terminal:

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Set `FOV_BUILD_GUI=OFF` to build only the layer/control utility, `FOV_BUILD_TESTS=OFF` to omit tests, or `OPENXR_INCLUDE_DIR` to an existing OpenXR SDK 1.1+ include directory. `scripts/bootstrap.ps1` downloads pinned official SDK sources. nlohmann/json is already vendored.

## Install and use

1. Package the build, then keep the entire `build-release/bin` directory together in a permanent location. Visual C++ x64 Redistributable is required.
2. Run `install_layer.bat` **inside that directory**. The control utility validates the manifest and DLL, asks Windows for elevation, and writes precisely one value under the 64-bit `HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` key. Its name is the absolute manifest path and DWORD data is `0`.
3. Select VDXR in Virtual Desktop Streamer using its normal controls, connect the headset, and start a native OpenXR game. This project never edits the game or runtime files.
4. Open `OpenXRFovManager.exe`. On Games, select the executable and click **Edit selected profile**. FOV Settings and Edge Fill edit the profile named at the top. **Edit global defaults** returns to defaults. Click **Save profile** after editing.
5. Use Dashboard **Disable layer** to set the registry value to `1`, or run `FovLayerControl.exe --disable`. Restart the game. Enabling uses `--enable`. These operations never unload an active DLL.
6. Run `uninstall_layer.bat` to remove only this package's registry value. Configuration and history remain intact. Disable or uninstall an active old copy before enabling a package in another location. Disabled copies do not block registration.

If registration fails, run `FovLayerControl.exe --check` from the extracted package for a read-only manifest/conflict diagnostic. Only an enabled (`REG_DWORD 0`) copy with the same layer name blocks enabling a different copy. The manager and installer now report the actual error instead of attributing every failure to administrator privileges.

Building, packaging and simply opening the manager do not change OpenXR registrations. Registration tests write only an isolated temporary HKCU key and redirect HKLM inside the test process; system loader registrations are untouched. UAC applies only when the user chooses a registration operation. Do not register multiple copies of the same layer. To bypass a registered layer temporarily, define `XR_FOV_DISABLE=1` before launching the game.

Edge Fill shows only settings used by the selected algorithm: Gradient exposes stretch width, gradient strength, and samples; Blur exposes stretch width, radius, and strength; Solid Color exposes the fill color; Temporal Depth Reconstruction exposes its reconstruction block. Clamp and Mirror have no additional parameters. The enable switch and algorithm selector remain available. Switching algorithms preserves parameter values.

## Automatic FFR and persistent Outside reconstruction in 0.1.8

At **100% effective FOV**, the layer keeps the original projection and reconstructs Outside using the selected Edge Fill algorithm. The Outside boundary uses the configured Middle X/Y radii and elliptical or rectangular geometry. Center remains 1×1; supported VRS can reduce Middle shading. Outside is never assigned a coarse VRS rate.

Below 100% on either axis, Center and Middle occupy the active rendered projection. Outside is the missing projection crop and uses the existing reconstruction mapping. The full-FOV peripheral mask and baseline-space crop are separate representations managed by `OutsideRegionManager`. Switching modes preserves the selected algorithm. Edge Fill and profile enable switches still gate reconstruction.

The **FFR** tab exposes both modes, the Outside boundary, selected reconstruction, supported backend/rates, fallback policy, and independent diagnostic views. To isolate unexpected borders, select **Diagnostic Outside color fill** and choose a bright color under Edge Fill → Solid Color, then restore the desired algorithm. The configured color is retained across algorithm changes. Mask, crop, original input, reconstruction-only output and final composite are independently inspectable.

**D3D12 Tier 2 FFR now intercepts ordinary scene draws automatically**, including eligible intermediate render targets. Applications no longer need to call the experimental cooperation API. The layer observes render-target descriptors, projection dimensions, command-list Reset, viewports, existing VRS state and queue submissions. It wraps eligible direct draws with a tiled map and restores state immediately. GPU-completed submission counters appear in the FFR tab. Native render passes, unknown indirect/bundle draws, ambiguous/mismatched targets and foreign VRS state are skipped.

The 0.1.7 prototype's hardware tests exercised cooperating applications only; its scene integration gap prevented FFR in the RE3 VR mod even though Outside reconstruction ran. Version 0.1.8 addresses that gap for D3D12. D3D11 NVIDIA NVAPI still requires explicit scene cooperation. Vulkan scene FFR remains unsupported; its existing cooperative reconstruction remains available.

Turn off another layer's FFR when testing this layer's automatic path. Existing application/Toolkit VRS is preserved and reported as a foreign-state skip rather than overridden. Outside original shading suppression remains **unsupported, with 0% scene savings**: replacing rendered pixels alone does not save that work. Full-FOV reconstruction can increase total GPU time even when Middle VRS reduces shader invocations.

See the [design](docs/FFR_DESIGN.md), [developer contract](docs/FFR_INTEGRATION.md), [tests and Hubris acceptance](docs/FFR_TESTING.md), and [measured benchmark](docs/FFR_BENCHMARK_RESULTS.md). Hardware fixtures verify rendering and state restoration; Hubris/headset acceptance remains outstanding.

## Vulkan support in 0.1.4

Vulkan/enable2 sessions and swapchain lifecycles are detected and tracked. **Ordinary third-party Vulkan games remain in diagnostic passthrough with native FOV and size recommendations.** GPU reconstruction currently requires a developer-owned application that explicitly provides the exact image layout, session queue and completed producer fence through the experimental cooperation API. An OpenXR API layer alone does not observe arbitrary Vulkan layouts or cross-queue synchronization.

The validated cooperative path supports all six algorithms, per-eye temporal rotation/depth reconstruction, RGBA/BGRA UNORM/sRGB, separate-eye/array/packed images, direct storage or private intermediate output, and GPU/CPU/memory diagnostics. Existing D3D11/D3D12 behavior remains available. Selecting an algorithm or setting `auto` cannot enable unverified Vulkan processing.

Global/per-game profiles may contain `"vulkan": {"enabled": true, "processing": "auto"}`; `enabled=false` or `processing="passthrough"` disables the Vulkan path. The manager preserves this advanced policy when saving profiles. These options do not change Direct3D profiles.

See the [architecture audit](docs/VULKAN_ARCHITECTURE_AUDIT.md), [backend design](docs/VULKAN_BACKEND_DESIGN.md), [swapchain handling](docs/VULKAN_SWAPCHAIN_HANDLING.md), [synchronization contract](docs/VULKAN_SYNCHRONIZATION.md), [reconstruction/integration guide](docs/VULKAN_RECONSTRUCTION.md), [compatibility matrix](docs/VULKAN_COMPATIBILITY_MATRIX.md), [measurements](docs/VULKAN_BENCHMARK_RESULTS.md), and [implementation report](docs/VULKAN_IMPLEMENTATION_REPORT.md). Controlled GPU fixtures do not establish SteamVR, VDXR or third-party game compatibility.

## Language and locale

Open **Settings**, choose **Language** and **Locale**, and click **Apply settings**. The interface supports English, German, French, Spanish, Simplified Chinese, Japanese, Russian, Hindi, Arabic, Bengali, and Korean. Language names remain in their native spelling so the selector is always recognizable. Changes apply immediately and preserve unsaved profile edits. Arabic uses a right-to-left interface; technical diagnostic panes retain left-to-right formatting.

Language and regional formatting are independent. Locales include US/UK English, Germany, France, Spain, China, Japan, Russia, India (Hindi), Saudi Arabia (Arabic), Bangladesh (Bengali), and South Korea. The locale controls numeric inputs, displayed measurements, and history dates; history times remain UTC. Preferences default to English / United States and are saved separately in `%LOCALAPPDATA%\OpenXRFovExtension\manager.json`. Missing or malformed preferences use the defaults. Technical JSON, runtime messages, file paths, and on-disk profile data retain their original format.

## Configuration and diagnostics

Files live in `%LOCALAPPDATA%\OpenXRFovExtension`: `config.json`, `history.json`, and process logs. The `config/config.json` in this repository documents the schema; missing settings use validated defaults automatically. To isolate tests or use another directory, set `FOV_DATA_DIR` **before** starting both processes.

Global `enabled` gates processing, while Dashboard's switch controls loader registration. Per-game keys are normalized, case-insensitive absolute executable paths. Profiles inherit omitted fields from global defaults. Reset removes the selected override. The manager and layer use serialized read/modify/write operations with atomic replacement; configuration is separate from history.

The worker reloads settings every half second and publishes newline-delimited JSON over local named pipes, one service per process. Disk and pipe I/O run off the VR thread. The manager can be closed without affecting processing or history. Pipe disconnection removes terminated applications from its dashboard. Render-size recommendations are fixed at instance startup; change them by restarting the game. FOV and algorithm settings apply on subsequent `xrLocateViews` calls.

Choose **Solid Color** on Edge Fill to fill missing regions with a flat color, black by default. **Choose color** sets the RGB color for global defaults or the selected game profile; click **Save profile** to apply it. The original rendered center is preserved. JSON uses `"edgeFillMethod": "solid"` and `"edgeFill": {"color": [0.0, 0.0, 0.0]}` with normalized RGB components from 0 to 1. Existing profiles inherit black. The layer converts the selected display color to linear values when writing to an sRGB render target so its output encoding preserves the chosen color.

**Temporal Depth Reconstruction** maintains 2–8 original observations per eye (default four), reprojects them using submitted poses and optional compatible OpenXR depth, and blends reliable peripheral samples with adaptive blur. Without depth it uses rotation-only history with reduced confidence. The rendered center stays unchanged. Its Edge Fill settings include history length, depth, confidence, feathering, fallback, debug views and an optional GPU budget. See [temporal design, schema and limits](docs/TEMPORAL.md).

Diagnostics shows requested/reconstructed FOV for both eyes, depth availability, reconstruction mode, historical-frame count, invalidations, delayed sampled GPU coverage and per-operation GPU timestamps. Source snapshots, history copies and the fused reconstruction pass are timed separately; the runtime compositor and queue recycling waits are excluded. A null measurement means no completed result is available. Coverage uses one sample per 8×8 peripheral grid, with a 16-byte asynchronous counter readback; color and depth textures remain GPU-resident.

## Supported processing

- Primary stereo projection, two eyes, independent/packed/array color swapchains, one mip, one sample, one face, no protected/static flags or swapchain extension chain.
- D3D11.1 device/context state isolation; D3D12 DIRECT session queue with explicit image states and fence-protected allocator/descriptor reuse.
- RGBA8 and BGRA8 UNORM/sRGB, RGB10A2 UNORM, RGBA16 FLOAT. sRGB views decode during sampling and encode on output.
- Temporal reconstruction consumes a compatible plain `XR_KHR_composition_layer_depth` attachment with D16, D24/S8, D32 or D32/S8 single-sample depth. Standard/reversed-Z and positive infinite clipping planes are supported. Input depth is omitted from widened output views; this release does not reconstruct full-FOV compositor depth.
- Unknown commands are forwarded to the next layer. Unsupported graphics bindings, formats, extra composition chains (including motion vectors), unmatched FOV values, and failed reconstruction keep the application's submitted image/FOV pair. Non-temporal algorithms retain depth-chain passthrough.

Reducing only the projection is not a guaranteed speedup. Savings depend on the game using the smaller recommended dimensions, honoring the returned FOV, its own culling, and the added copy/reconstruction cost. Games that allocate fixed-size targets may save little or no GPU time. Live multiplier changes do not resize existing game render targets.
