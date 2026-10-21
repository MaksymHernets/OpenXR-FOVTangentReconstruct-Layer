# Adaptive FFR — 0.1.5

FFR is independent of the six reconstruction algorithms. It changes scene shading frequency where supported; it does not blur an already rendered image. It is disabled by default. Selecting FFR never changes the reconstruction selection or adds FOV reduction.

## Authoritative projection

The FOV Tangent Manager now exposes the shared `makeFFRPlan` projection calculation. At successful stereo `xrLocateViews`, the layer saves downstream runtime FOV as the baseline and the actual returned application FOV as the active projection. Ratios of tangent spans determine the per-axis effective scale. A streamer-reduced baseline remains 100% when the layer returns that same baseline. The layer does not know a headset's uncropped physical FOV and does not invent one.

Both effective scales within `1e-4` of one select `ThreeRenderedZones`. Otherwise the mode is `TwoRenderedZonesWithCrop`. Invalid/nonfinite projections or projections outside the saved baseline cannot enable a scene scope. Each eye has its own baseline, active projection, optical-axis center and crop rectangle. No UI slider is used as runtime provenance. Disabling reconstruction retains the existing native-projection fallback, so configured 80% sliders do not incorrectly imply actual application cropping when that fallback is active.

All full-FOV pixels remain rendered. Three-zone FFR alone does not call the Reconstruction Manager. Reduced FOV uses two zones throughout the active viewport; the outside area belongs solely to the existing reconstruction mapping. The existing source snapshots, output swapchain, FOV provenance and six shaders remain responsible for reconstruction. FFR contains no reconstruction implementation.

## Zone geometry and configuration

The `ffr` profile object holds independent `enabled` and `debug` flags, center/middle X/Y radii, X/Y offsets, transition width, ellipse/rectangle geometry, mapping policy, full-mode middle quality, reduced-mode middle quality, and full-mode outer quality. Global/game inheritance and atomic persistence are unchanged. Both middle qualities are constrained to 50–75 and outer quality to 10–20; the center always uses 1x1. Switching modes never resets these values or the reconstruction algorithm.

Radii and offsets are fractions of the active viewport, not FOV angles. The default center radii are 0.22/0.22 and middle radii 0.4/0.4. Centers derive from the projection of the forward axis in each asymmetric active frustum, plus offsets, clamped inside the viewport. Applied middle radii are clamped to viewport boundaries; center radii are clamped inside the middle. Configured values are retained. Applied geometry is reported per eye. The two-zone periphery extends to the viewport edge; the middle radius does not introduce an extra rendered zone there.

Transitions conservatively expand finer regions. Any hardware tile touching the center or its transition band remains 1x1. Tiles crossing a packed viewport boundary and tiles outside that eye's viewport remain 1x1. These are discrete shading-rate transitions, not blended resolutions. Elliptical/rectangular previews show green center, amber middle, red rendered outer zone, and purple checked reconstruction crop. The optional grid is a **configured preview** diagnostic. Actual GPU test captures use a separate colored rasterized peripheral grid and reject unexpected black gaps.

## D3D12 rendering integration

The backend requires a single-node D3D12 device, VRS Tier 2 and `ID3D12GraphicsCommandList5`. It records an R8_UINT rate image sized to the render target's reported tile size. Fixed private slots retain their default/upload images until the caller's dedicated queue fence completes; busy reuse is refused without waiting. Unchanged maps reuse their images and content. Dimension/map changes update completed slots. Pixel work is reduced during the scene draw, not during reconstruction. D3D12 debug validation and pipeline statistics exercise that behavior.

Quality targets map to supported discrete rates:

| Target | Conservative | Balanced | Aggressive |
|---|---|---|---|
| 100 | 1x1 | 1x1 | 1x1 |
| 75 | 1x1 | 1x1 | 2x1 |
| 50–74 | 1x2 | 2x2 | 2x2 |
| 11–20 | 2x4 | 2x4 | 4x4 |
| 10 | 4x4 | 4x4 | 4x4 |

Without optional additional rates, requests above 2x2 fall back to 2x2. These percentages are user quality targets, not exact sample density or guaranteed visual quality/savings. VRS does not allocate lower-resolution peripheral textures. No lower-resolution peripheral backend is claimed.

The [cooperation contract](FFR_INTEGRATION.md) is mandatory. Ordinary third-party games have no automatic scene integration. D3D11's vendor-specific path and Vulkan's feature-enabled shading-rate attachment/render-pass path are not implemented. Their GPU capability is reported as unknown, with scene integration unsupported. Vulkan reconstruction cooperation does not authorize Vulkan FFR. Unsupported FFR leaves existing rendering/reconstruction intact.

## UI and diagnostics

The independent FFR tab preserves all eleven languages and existing tab identities. Without live projection evidence it explicitly shows a configured preview. With a single selected live application it uses observed per-eye baseline/active projections. Mixed per-eye modes show both applicable quality controls. The outer quality is hidden when no eye uses three zones; crop/reconstruction status is hidden at full FOV. Settings remain editable without claiming activation.

Diagnostics distinguish configured maps, recorded supported rates, and unconfirmed hardware execution. The OpenXR layer cannot independently prove that a borrowed command list was executed merely because it recorded commands. The controlled benchmark separately confirms hardware application using pipeline statistics. Per-eye logs include effective scales, mode, crop status, selected reconstruction method, backend capability and confirmation status. No permanent color overlay or VRS state is injected into reconstruction or compositor passes.
