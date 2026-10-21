# Vulkan reconstruction and developer integration

The GLSL compute backend implements the six existing methods with the same profile fields. It uses the established OpenXR minus-Z-forward coordinates and asymmetric tangent-space projection mapping. The valid current image region is sampled from the original snapshot; synthetic filling is confined to missing periphery. Resizing the output can resample valid content, so arbitrary image pixels are not claimed to remain bit-identical under scaling.

| Method | Vulkan behavior |
|---|---|
| Clamp | Bounded edge extension |
| Gradient | Bounded nonlinear stretching with transition width, strength and 1–16 inward samples |
| Blur | Bounded nine-tap blur weighted by peripheral distance and configured width/strength/radius |
| Mirror | Reflected edge sampling, isolated to the selected subimage |
| Solid | Configured display RGB outside current coverage; opaque black by default |
| Temporal Depth | Original-only per-view history, rotation-only or validated depth reconstruction, confidence/occlusion rejection and adaptive blur fallback |

The temporal port retains quaternion transforms, 2–8 history frames, timestamps/age limits, common LOCAL-space poses, FOV/extent/settings/depth invalidation, recenter/session resets, translation confidence reduction, three bounded depth-correspondence iterations, reprojection-error and silhouette rejection, nearest-surface preference, compatible-history blending, boundary feathering and confidence/coverage/depth debug modes. Standard, reversed-Z and positive infinite clipping conventions use the existing reciprocal-depth mapping. Invalid or unavailable depth uses rotation-only observations or blur, never fabricated geometry.

Source sRGB images are decoded by sampling. The compute target is RGBA8 UNORM, so the shader explicitly encodes sRGB output when needed. BGRA transfer output uses an explicit channel permutation before the byte-preserving copy. Direct RGBA8 UNORM output needs neither encoding nor permutation. Tests compare custom solid colors and constant valid content across all four color formats and both eyes.

## Developer cooperation API

The commands in `layer/VulkanCooperation.h` are experimental layer-specific commands obtained via the layer's `xrGetInstanceProcAddr`. They are not a registered OpenXR extension and are not advertised as universally available runtime functions. Applications must check availability and results.

The developer-owned application must initialize Vulkan 1.1+ and obey [the synchronization contract](VULKAN_SYNCHRONIZATION.md). Before creating its processing swapchains, call:

```cpp
enableProcessing(session, FOV_VULKAN_COOPERATION_VERSION);
```

Before each successful input release, while the image is acquired/waited and the bound queue is externally synchronized, submit all image rendering and queue-family restoration, obtain a completed producer fence, and call:

```cpp
declareImageState(swapchain, acquiredIndex,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  sessionBoundQueue, completedProducerFence);
xrReleaseSwapchainImage(swapchain, &releaseInfo);
```

Depth uses exactly DEPTH_STENCIL_ATTACHMENT_OPTIMAL. The fence must cover all writes and restoration to the bound queue; it may not be reset/reused before release consumes the declaration. The layer does not wait for an incomplete producer fence. Synchronizing a separate queue requires the application to complete the appropriate semaphore/ownership transfers to the bound queue before this contract is satisfied. Arbitrary declaration values do not make unsafe application rendering safe.

Do not enable cooperation in a third-party process through an environment flag, guessed layout, or guessed fence. Without the full contract, leave Vulkan in diagnostic passthrough.

## Reproducible evidence

`vulkan_tests` executes actual SPIR-V on the GPU, including independent eye colors, synthetic grid/foreground-marker frames, direct/intermediate output, all six methods, rotation, translation/depth, reversed-Z, invalid depth/tracking, all four depth formats, coverage counters and resource reuse. CPU-generated source patterns and explicit test readback make expected results reproducible; they are not a rasterized game or a hardware headset test.

`vulkan_layer_tests` negotiates the actual OpenXR layer against a developer-owned runtime fixture with actual Vulkan images. It verifies image ownership, different acquired indices, stereo mappings, output image/FOV pairing, declaration refusal and recovery, KHR depth consumption, policy reload and explicit/implicit teardown. It bypasses a production OpenXR runtime; that limitation is deliberate and is recorded in the evidence.

Normal product processing has no color/depth readback. The 16-byte diagnostic counter block and GPU timestamps are read only after optimizer fences complete. The fixture's full image readbacks and PPM/PNG captures exist solely for validation and are excluded from claims about product rendering cost.
