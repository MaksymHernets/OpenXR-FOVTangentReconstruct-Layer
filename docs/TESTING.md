# Validation

The automated tests do not register the layer or modify the active runtime. Test data is isolated under the build directory or a temporary test directory.

Version 0.1.1 adds **layer_temporal_d3d11**, **layer_temporal_d3d12**, **temporal_d3d11**, and **temporal_d3d12**. Version 0.1.2 adds **gui_localization**; the complete suite has eight tests when the GUI is enabled. New tests cover source-depth acquire/wait/release, output depth-chain removal, owned reference-space lifetime, invalid space/tracking, recenter and session reset, original-only history, stationary/rotated views, translation/parallax, standard/reversed-Z, D16/D24/D32/D32S8, invalid-depth fallback, independent eye content, GPU counter coverage, separate copy timings, and in-flight ring reuse. GPU tests enable Windows graphics debug layers when installed and reject reported errors/corruption.

`ctest --test-dir build-release --output-on-failure` runs:

- **gui_localization:** algorithm-specific parameter and label visibility for all six algorithms, preservation of hidden values, all eleven languages, embedded catalog completeness and placeholder consistency, independent locale selection, Arabic right-to-left switching and left-to-right restoration, technical diagnostic direction, live translation, unsaved editor preservation, localized UTC dates and chronological sorting, persisted settings across window recreation, invalid/malformed preference fallback, and isolation from rendering profiles. The test locates Qt libraries and plugins from the configured SDK.
- **core_and_gpu:** malformed/bounded profiles, path normalization, concurrent atomic updates, asymmetric tangent-space projection, actual HLSL execution on D3D11 and D3D12 WARP, all five algorithms in UNORM and sRGB, array eye selection, bounded output, repeated resource reuse, application context-state restoration, cross-API pixel comparison, black/custom solid edges with unchanged center pixels, and GPU timestamp queries.
- **layer_dispatch** and **layer_dispatch_d3d12:** load the DLL, reject invalid negotiation, negotiate/advance the next-layer chain, dispatch multiple instances, forward unhandled commands, create real D3D11/D3D12 WARP renderers against a test runtime, verify size/FOV reduction, output swapchain replacement, nonblocking output timeout/retry, extension-chain passthrough, pose preservation, immutable application frame structs, and explicit teardown. Connect to the actual named pipe, verify history without a GUI, and verify live configuration reload. The test runtime is a fixture, not a substitute product runtime.

To verify discovery with an official OpenXR loader without registry writes:

```powershell
./build-release/LoaderProbe.exe 'path/to/openxr_loader.dll' './build-release/bin'
# Optional: also create/destroy an instance with the currently active real runtime:
./build-release/LoaderProbe.exe 'path/to/openxr_loader.dll' './build-release/bin' --create-instance
```

The probe sets discovery environment variables only in its own process and writes history to `probe-data` beside `bin`. It does not register the package. Real runtime initialization may fail without its headset/service available; report that result separately from discovery.

GUI rendering smoke test (no registry mutations):

```powershell
$env:QT_QPA_PLATFORM='minimal'
$env:FOV_DATA_DIR="$PWD/build-release/gui-test-data"
./build-release/bin/OpenXRFovManager.exe --smoke-test "$PWD/build-release/gui-qa"
```

This renders each page into a PNG and exits. Clear `QT_QPA_PLATFORM` before normal use. Use the native Windows platform for visual QA when fonts and normal window decorations matter.

## Settings manual acceptance

On Settings, apply each language and locale and visit all six tabs. Confirm readable labels, translated algorithms, decimal separators, UTC date formats, and the color picker. Leave a profile edit unsaved before switching, then confirm it remains. Close and reopen the manager to verify persistence. Select English with a German locale to verify language and formatting are independent.

## VDXR / Quest 3 manual acceptance

1. Record game, VDXR, GPU driver, headset refresh rate, codec and render resolution. Disable other projection-changing layers first, then test them individually for chaining compatibility.
2. Install from a path containing spaces. Verify exactly one absolute manifest REG_DWORD exists in the 64-bit implicit-layer key, with `0` enabled and `1` disabled. Test UAC cancellation and repeated install/uninstall. Check unrelated values remain untouched.
3. Start a D3D11 native OpenXR application. Confirm process/path/API/history while the manager is closed, then open it. Check first/last timestamps and that one process launch increments its count once.
4. Test independent multipliers 1.0, 0.8, and 0.5, asymmetric eye geometry, packed and array eye targets. Use a static grid with clear stereo landmarks. Compare requested/submitted FOV, verify depth/scale/optical center, and inspect corners.
5. Test Clamp, Gradient (width/strength/samples), Blur (radius/strength/width), Mirror, and Solid Color (black/custom RGB). Check for seams, cross-eye contamination, color shifts on sRGB targets, and out-of-bounds borders. Toggle debug FOV borders.
6. Save two distinct game profiles, restart both applications, reset to defaults, and confirm live updates only affect the intended executable. Close/reopen the GUI. Terminate the app and verify it disappears from live status.
7. Repeat with D3D12. Enable the D3D12 debug layer/GPU validation in a test application and check image barriers, queue ordering and descriptor/allocator reuse. Run at least 30 minutes with frequent swapchain/session recreation and check memory use.
8. Exercise supported KHR depth with Temporal Depth Reconstruction, then unsupported extra depth/motion-vector/MSAA/static/secondary-view cases. Confirm unsupported submissions keep their original image/FOV pair. Confirm processed temporal views omit narrow depth from widened output.
9. Measure GPU frame time with layer unregistered, with 1.0 multipliers, then each reduced setting/algorithm. Include source copies and output processing, not only the per-eye timestamp. Confirm the game actually allocated smaller targets.
10. Disable registration during a session; verify the session continues and the layer is absent on the next launch. Uninstall twice, keeping configuration and history.

Record actual results before treating hardware acceptance criteria as met.

## Local results — 2026-10-20

- Fresh CMake/Ninja Release build: passed, MSVC 19.51, Windows SDK 10.0.26100.0, Qt 6.11.1 x64.
- CTest 0.1.1: seven tests passed, including the five existing algorithms in UNORM/sRGB, depth hook integration, and controlled temporal GPU scenarios on both APIs. These are controlled software-GPU tests, not a VDXR headset session.
- Official Khronos OpenXR loader 1.1.53 built from the pinned SDK: discovered this package's manifest using process-local `XR_API_LAYER_PATH`.
- Installed VirtualDesktopXR runtime: `xrCreateInstance` and `xrDestroyInstance` succeeded through the packaged layer with process-local `XR_RUNTIME_JSON` pointing at the installed VDXR manifest. This verifies negotiation/loading and basic instance chaining, not session rendering.
- Qt manager: launched from packaged binaries and rendered all five pages using the native Windows platform plugin.
- HKLM registration/elevation/install/uninstall operations were not executed on this machine. Headset session rendering, real GPU performance, stereo comfort and resource soak tests remain outstanding.

