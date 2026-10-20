# Changelog

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
