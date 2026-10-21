# FFR verification — 0.1.8

Run the pinned bootstrap and standard Release build. The automated suite covers CPU projection geometry, asymmetric eyes, streamer baseline, no double crop, inherited profiles, exact Outside VRS tile containment, algorithm persistence and all eleven UI languages.

Actual GPU coverage includes:

- Automatic D3D12 direct draws with no cooperation calls: reduced/full-FOV maps, actual pipeline statistics, cache-generation counts, RE Engine MAX-per-primitive combiner compatibility, partial viewport exclusion, post-draw 1x1 restoration and foreign application VRS preservation.
- D3D11 NVIDIA NVAPI and D3D12 Tier 2 pixel-shader invocation queries, GPU timestamps and a draw after End proving restoration to 1x1. Device unavailability is an explicit skip, not a hardware pass.
- Full-FOV elliptical and rectangular masks with all six algorithms on D3D11/D3D12, UNORM/sRGB readback parity, opaque coverage, inner content preservation and separate mask/crop/input/output/color-fill diagnostics.
- The real layer DLL against controlled OpenXR dispatch, full-FOV reconstruction, reduced/anisotropic crop, streamer-reduced baseline, reconstruction-disabled fallback, acquired/waited image checks, nested/stale scope rejection and teardown.
- D3D11 layer scene scopes with both array-eye slices, all six algorithms at full/reduced FOV and a plain KHR depth chain. Solid color is checked numerically; every algorithm must return nonblack opaque Outside for the bright fixture.
- Cooperative Vulkan full masks and crops, RGBA/BGRA UNORM/sRGB, direct storage/private intermediate output, temporal depth/rotation, separate/array/packed eye layouts, producer fence/layout validation and required Khronos synchronization validation.
- Isolated registration upgrade regression under process-local HKLM redirection to a temporary HKCU key. The test does not change real loader registrations.

The fixture records observed GPU execution separately from the layer's configured/recorded-but-unconfirmed diagnostics. Source-to-output copies and postprocessing costs remain in total frame timing. Reports and raw measurements for this version live in `docs/ffr-018-evidence`. Earlier `ffr-evidence` files document 0.1.5 and must not be used as evidence for this version's full-FOV mask.

## Hubris and production runtime acceptance

Hubris was launched through its OpenXR option and the new layer reported D3D11/VDXR full-FOV Solid Color reconstruction. Desktop mirror capture failed and headset visual correctness was not confirmed. No application files or real loader registrations were changed for verification. Automated fixtures cannot establish SteamVR/VDXR production compatibility or the exact cause of the reported Hubris black borders.

Required live matrix: unregistered baseline; registered reconstruction with FFR disabled; full-FOV FFR requested; reduced-FOV reconstruction; and reduced-FOV FFR requested. Test all six algorithms, both eyes, head rotation/translation, UI, depth discontinuities and application restarts. Inspect bright diagnostic color fill, Outside mask, actual crop, reconstruction input/output and final composite to isolate application-provided black observations from compositing or algorithm faults. Record runtime/headset/game/driver versions, active graphics binding, FOVs, subrects, GPU/frame percentile costs and compositor misses. D3D12 direct-draw VRS is now automatic for eligible targets; other unsupported scene paths and Outside shading suppression remain explicitly unavailable; do not infer scene savings from the selected profile or replaced pixels.

## RE3 VR mod regression

The user reported FFR working in Toolkit and failing in 0.1.7. Local game logs establish D3D12 and the prototype's explicit-cooperation requirement. 0.1.8 adds the missing automatic scene path. The no-cooperation GPU fixture verifies actual shading work reduction. Production-game execution and headset observations must be recorded separately; a configured rate map or a fixture result alone does not establish RE3 visual correctness or FPS improvement.


On 2026-10-21, an isolated RE3 VR mod launch loaded the 0.1.8 DLL with D3D12 and Virtual Desktop OpenXR. Toolkit was absent from loaded modules. With 80% horizontal/vertical FOV and Blur reconstruction, the corrected backend recorded 46,985 draws and confirmed 46,980 through completed queue fences, with four immutable map builds and no foreign-state skips. Reconstruction reported matching FOVs and no error. The first live attempt exposed RE Engine's MAX/passthrough combiner pair; compatibility was corrected and verified by the real GPU fixture before this capture. The user then confirmed both eyes look correct in the running corrected setup. This is a visual acceptance of this RE3/VDXR/80%-FOV/Blur configuration; game FPS and the remaining live matrix were not measured. See [sanitized live evidence](ffr-018-evidence/re3-live.json).
