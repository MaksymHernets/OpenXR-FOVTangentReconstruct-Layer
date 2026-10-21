# FFR verification and remaining runtime acceptance

Build with the standard bootstrap/build scripts. CTest adds `ffr_projection_and_config` and `ffr_d3d12_hardware` to the existing regression suite. The hardware test requires an actual D3D12 Tier 2 GPU; an unavailable device produces an explicit skip (77), not a hardware pass.

The CPU test covers the epsilon boundary, independent axes/scales, streamer baseline versus additional crop, asymmetric eyes, contained geometry, packed boundaries, invalid projections, conservative rate fallback and inherited independent quality settings. The GUI test covers dynamic three/two-zone controls, persistent independent qualities, unchanged reconstruction selection, locale switching and profile preservation.

The hardware fixture rasterizes distinct-eye colored grids, uses actual rate-image commands and a pixel shader with arithmetic work, and checks:

- Full FOV baseline versus three-zone FFR, reduced FOV without FFR versus two-zone FFR plus existing reconstruction, several scales/qualities/policies.
- Measured pixel-shader invocation reduction, exact center-patch equality to the non-FFR baseline, complete opaque output, and absence of unintended black gaps in the colored pattern.
- Original/active FOV pairing through the real layer against the existing developer-owned OpenXR runtime fixture, full-FOV/no-reconstruction behavior, anisotropic crop, reconstruction-disabled native fallback, and a reduced streamer baseline without application crop.
- Invalid/stale scene requests, nested scopes, active scene release/destruction, reuse before the promised fence completes, restored 1x1 draw behavior, and completed teardown.
- D3D12 debug-layer errors when the debug layer is available. Actual enabled state is recorded; unavailable debug support is not claimed as validation.

Existing D3D11/D3D12 dispatch/temporal and cooperative Vulkan reconstruction cases also request FFR while checking that unsupported scene integration preserves the existing reconstruction paths. Vulkan cases may be run with required Khronos synchronization validation as described in the Vulkan report.

## Hubris regression — not executed

Hubris is a required **manual production-runtime acceptance case**, not a name assigned to synthetic fixture output. No Hubris/headset rendering session was executed in this release verification. General game FFR remains unsupported without scene integration. Do not enable it using guessed command-list state.

Record Hubris version, runtime/headset, actual graphics binding, driver, source/output dimensions and FOVs. Compare unregistered baseline, registered FFR disabled, full-FOV FFR requested, reduced FOV without FFR, and reduced FOV with FFR requested. For unsupported scene integration, confirm the FFR diagnostic and unchanged reconstruction behavior. With a future validated game integration, confirm the actual rates and GPU pipeline workload before claiming FFR activation.

For all six existing reconstruction algorithms, inspect both eyes with a bright peripheral pattern and normal gameplay, including head rotation, translation, depth changes, UI overlays and application restarts. Verify no new black edges, center damage, projection/convergence changes or double crop. Record GPU frame percentiles, frame deadline misses and compositor timings using the production runtime. Preserve the user's reconstruction choice. These headset/game results remain outstanding and are not implied by the automated passes or synthetic benchmark.
