# Temporal Depth Reconstruction — 0.1.1

Select **Temporal Depth Reconstruction** on Edge Fill, then save the global defaults or selected game profile. No application/runtime modification or engine resource injection is used. Existing algorithms and old JSON profiles continue to work.

## Pipeline

The layer captures the application's latest released color image while it is still acquired and successfully waited. Compatible submitted depth is captured under the same ownership rules. At end-frame, color/depth subrectangles are copied into private per-eye ring slots, after reconstruction has consumed older slots. Only **original rendered observations** enter history. Generated edges, confidence-debug output, and reconstructed pixels never become history sources.

Each projection-view history has its own identity: source swapchain, reference space, projection index, eye, array slice, and subrect. Frame metadata includes eye pose in a common owned LOCAL space, original/reduced asymmetric FOV, predicted display time, format, dimensions and optional depth mapping. `xrLocateSpace` transforms the submitted projection pose into that common space at the frame's display time, including when the application uses a moving VIEW/action space. Invalid space locations or inability to create this reference space select blur fallback. Application output poses and spaces are preserved. The owned reference space is destroyed with the session.

The default ring holds four observations; supported lengths are two through eight. GPU copies and sampling remain ordered on the same graphics queue/context. D3D12 allocator/descriptor reuse and teardown are fence protected. Normal reuse adds no new history textures after warm-up. Resizing/format changes may allocate new resources and drain previous use.

Current pixels inside the reduced FOV are sampled directly. For peripheral pixels, the shader:

1. Builds the original-FOV viewing ray with OpenXR's right-handed, minus-Z-forward eye coordinates.
2. Transforms that ray into each historical eye using the actual quaternions and asymmetric tangent bounds.
3. Without depth, uses rotation-only correspondence and strongly lowers confidence when the eye has translated.
4. With depth, runs three bounded inverse-correspondence iterations. Each iteration samples historical depth, reconstructs a 3D point, transforms it into the current eye, and updates the historical UV. The final candidate must forward-project within 1.5 output pixels of the target and have valid, locally consistent depth. Current depth is never assumed outside its rendered FOV.
5. Rejects out-of-bounds/behind-eye coordinates, non-finite samples, clear/infinite depth endpoints, and steep contradictory depth neighborhoods. It prefers nearer surfaces when candidates disagree in depth, blends only compatible colors/surfaces, and prefers recent observations.
6. Weights confidence by age, rotation, translation, reprojection error, peripheral distance, and agreement between observations. Confidence crosses a smooth threshold band; boundary feathering affects only generated pixels outside the current FOV.
7. Blends temporal color with a bounded nine-tap adaptive blur of the current boundary. Missing or rejected history uses blur alone. Generated peripheral alpha is opaque; valid current content is untouched.

The shader fuses candidate evaluation, per-pixel confidence, hole handling, blur and final composition in one fullscreen pass. Confidence stays in GPU registers rather than requiring additional intermediate texture passes; debug modes visualize that same value. Histories store source observations with implicit original-pixel provenance, rather than recursively propagating generated confidence.

## Depth requirements and interpretation

The application must have enabled `XR_KHR_composition_layer_depth` and submit a plain `XrCompositionLayerDepthInfoKHR` chain. The layer checks the depth swapchain's session, format, successful capture in the current frame epoch, array slice, subrect, usage, and mapping. Color and depth resolutions may differ. Accepted formats are `D16_UNORM`, `D24_UNORM_S8_UINT`, `D32_FLOAT`, and `D32_FLOAT_S8X24_UINT`, single-sample, one mip/face, no protected/static flags. Private typeless textures expose appropriate depth SRVs. D3D12 sources return to `DEPTH_WRITE` before release; all depth/stencil planes retain correct copy/state handling.

Per the [KHR depth specification](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/khr/khr_composition_layer_depth.adoc), `minDepth < maxDepth` maps the supplied window-depth range, while reversed-Z is represented by `nearZ > farZ`. With `t = (sample-minDepth)/(maxDepth-minDepth)`, positive eye distance is:

```
distance = 1 / ((1-t)/nearZ + t/farZ)
```

Positive infinite near/far planes are supported through reciprocal zero; invalid/non-positive results and clear/infinite sample endpoints are rejected. No substitute depth is invented. Unsupported/missing depth uses rotation-only history and blur. A valid depth attachment does not imply every depth pixel contains usable geometry.

After color reconstruction, the layer removes the **input** depth attachment from its copied output view. Its reduced-FOV depth cannot accompany widened color. This release does not synthesize a full-FOV compositor depth map. Runtime depth-assisted reprojection is therefore unavailable on these reconstructed views; this tradeoff must be tested in the headset. Unknown/extra composition chains still use unchanged passthrough. Other edge algorithms retain their existing depth-chain passthrough behavior.

## Settings

| Setting | Default | Range / meaning |
|---|---:|---|
| History frames | 4 | 2–8 observations per view |
| Use depth | true | Only compatible submitted OpenXR depth |
| Confidence threshold | 0.6 | 0–0.99 |
| Temporal blend strength | 0.85 | 0–1 |
| Boundary feather width | 0.05 | 0.001–0.5 in normalized reduced-image coordinates |
| Fallback blur radius | 6 | 0–32 pixels, nine bounded taps |
| Fallback blur strength | 0.5 | 0–1 |
| Fallback stretch width | 0.15 | 0.01–0.5 |
| Confidence transition width | 0.1 | 0.01–0.5 |
| Maximum history age | 0.15 seconds | 0.02–0.5 |
| GPU budget per eye pass | 0 ms | 0 disables; otherwise 0–20 ms |

The optional budget compares delayed fused-pass timestamps with the configured limit and temporarily selects the existing blur path when over budget. It is a reactive quality fallback, not a hard frame-time guarantee. Rendering workload, source/history copies and the compositor still need to be included in a hardware performance evaluation.

`temporalDepth.enabled=false` selects blur-only operation for this method. Debug switches show confidence, temporal coverage, or reprojected depth (magenta indicates unavailable reconstruction). Nested profile values inherit defaults. See `config/config.json` for the full compatible schema.

## Invalidation and diagnostics

Session begin/end/recreation, reference-space changes at their reported change time, invalid pose/tracking, changed FOV/format/extent/depth availability/settings, old/non-monotonic timestamps, and large pose discontinuities invalidate observations. Discontinuity thresholds are 0.75 metres or about 60 degrees between observations. Failures during history writes invalidate metadata so a partially overwritten texture cannot be used with an older pose.

Diagnostics reports depth availability, mode, valid historical frames, per-view invalidation counts, and delayed GPU coverage. Percentages are **sampled estimates**, not full-resolution statistics: one peripheral pixel per 8×8 grid contributes. Reconstructed percentage counts pixels with nonzero temporal contribution; fallback percentage counts pixels using fallback alone. Confidence blending can mix fallback into reconstructed pixels too. An empty/pending measurement is null, never a fabricated percentage.

Color and depth textures are never read back by the product. Diagnostics asynchronously reads four 32-bit GPU counters and timestamp results after completion; this small metadata transfer is the explicit exception needed to expose GPU-measured statistics. D3D11 uses nonblocking completion checks and `DO_NOT_WAIT` maps. D3D12 reads only completed slots. Measurements are keyed to the history identity to keep eye statistics separate.

GPU timings expose source snapshots, individual history copies, and the fused reprojection/confidence/fallback draw. These are delayed per-operation measurements, not summed per-frame timings; there are multiple copies/views in a frame. The fused shader's internal stages cannot be independently timed with command timestamps. Hardware measurements of total overhead are still required.

## Verification and limits

Automated tests execute real shaders on D3D11/D3D12 WARP with stationary and rotated cameras, translation/parallax, standard/reversed-Z, all four depth formats, invalid-depth/tracking fallback, independent eye colors, ring reuse, GPU counters and context/state validation when Windows debug layers are available. Full hook tests exercise the actual layer's depth acquisition, enumeration, history, output-chain handling and cleanup against controlled runtime fixtures.

No engine motion vectors are available. Independently moving objects, newly disoccluded surfaces and geometry never observed by any history cannot be recovered perfectly. Color/depth consistency and short history reduce some ghosts; blur covers rejected holes. Large translations, silhouettes, steep surfaces, fast turns and the bounded inverse search can deliberately fall back. This is conservative peripheral reuse, not scene reconstruction or predictive motion compensation.

VDXR/Quest 3 stereo comfort, moving-object artifacts, runtime depth behavior, real GPU overhead and long-running leak/frame-pacing checks remain manual hardware validation. The feature makes no guaranteed performance or visual-quality improvement claim before those tests.
