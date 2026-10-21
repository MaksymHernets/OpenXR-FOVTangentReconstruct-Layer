# Design and implementation boundaries

## Standards review

Reviewed against the current [OpenXR 1.1 specification](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html), [loader design](https://registry.khronos.org/OpenXR/specs/1.1/loader.html), and [VDXR developer documentation](https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki/Developers). The build pins official SDK 1.1.53 headers; no private runtime interfaces are used. VDXR's documented OpenXR support makes this architecture technically applicable, but actual VDXR session/device compatibility must be tested.

The implicit manifest includes its mandatory disable environment variable. Negotiation validates struct types, sizes, interface and API version ranges. `xrCreateInstance` is intercepted through the loader's `createApiLayerInstance` chain, advancing `nextInfo`; a direct replacement for the loader's global `xrCreateInstance` is deliberately not installed. Every instance has its own downstream GIPA and dispatch table. Unknown commands preserve downstream dispatch.

## Projection provenance

`xrLocateViews` stores original/reduced per-eye FOV and a settings snapshot keyed by display time. Eye poses are never changed. Recommended image dimensions are scaled in `xrEnumerateViewConfigurationViews` at startup. The returned projection uses the exact tangent-space formula from the specification supplied for this project.

At `xrEndFrame`, reconstruction requires two primary stereo views, matching display time and all submitted FOV angles (tolerance 0.0001 radians). The layer verifies subrect bounds, array slices, source session, image capture and supported chains. It maps output pixels through original tangent bounds into reduced tangent bounds, retaining asymmetry. It copies composition structs, preserving application memory and poses, replaces reconstructed swapchain/subrect/FOV fields, and submits original FOV only after all output images were released successfully. Temporal reconstruction can consume and remove a plain KHR depth attachment from copied output views; see [its detailed design](TEMPORAL.md).

This proves that the application **submitted** the modified FOV. OpenXR provides no general way to prove which matrix an application used in its shaders. A game could submit the returned FOV while rendering with a cached projection; headset testing is needed to detect that behavior.

## Swapchain ownership

1. Track acquired images FIFO. Only a successful wait (not `XR_TIMEOUT_EXPIRED`) makes an image eligible for capture.
2. Immediately before forwarding the application's release, enqueue a whole-image copy into a private reusable texture. The application swapchain requests transfer-source usage; if the runtime rejects the augmented creation, retry the original creation with processing disabled for that swapchain.
3. Forward the release. No application/runtime swapchain image is sampled after its release. Only the owned snapshot is sampled later.
4. Lazily create a separate runtime swapchain per source chain. Acquire, wait with zero timeout, reconstruct all used rectangles/slices, then release and submit. A timed-out wait retains the acquired image for a later wait; it is never released before a successful wait. Destroying a swapchain disposes outstanding acquisitions through normal swapchain destruction.
5. Keep the most recent released snapshot, as OpenXR submissions refer to the most recently released image. Multiple acquired application images are tracked in FIFO order. Static/protected/MSAA/cube swapchains use passthrough. Compatible depth swapchains are privately snapshotted only for temporal reconstruction.

Output dimensions expand startup-scaled source dimensions so the central region retains approximately its requested pixel density. Both endpoints of packed eye rectangles use the same integer scale. Overlapping references to the same slice in a frame are rejected, since one output image cannot represent different reconstructions of overlapping content.

D3D11 uses `SwapDeviceContextState` around copy/draw operations to retain the application's graphics state. SRVs, private textures and RTVs are cached. D3D12 records copies and draws on the graphics-binding queue. Acquired color images begin/end in `RENDER_TARGET`; snapshots transition through `COPY_DEST` and `PIXEL_SHADER_RESOURCE`, and sources through `COPY_SOURCE` back to `RENDER_TARGET`, as required by [XR_KHR_D3D12_enable](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/khr/khr_d3d12_enable.adoc). Eight command slots each own an allocator, list and descriptor heaps. Fences protect reuse and teardown; slot recycling may wait for the GPU. Runtime image release relies on ordering on this same queue, as specified by OpenXR. No texture readback is used by the product; tests read pixels to verify results.

## Concurrency and diagnostics

Object maps and renderer operations are serialized with a recursive process mutex. OpenXR's application-side external synchronization requirements still apply. Render callbacks publish small diagnostic snapshots via a try-lock; contention drops a snapshot instead of blocking. The worker handles config parsing, history updates, local pipe writes and logging. Configuration/history writers take a file lock and flush a temporary file before atomic replacement. Settings are immutable shared snapshots.

The local pipe rejects remote clients and uses the user's default Windows security descriptor. Payloads contain local paths and diagnostics; this is not a cross-user control interface. Live updates arrive through the persistent config worker; pipe traffic is diagnostics only. A dedicated pipe service is shared by instances in the process. Multiple sessions are tracked correctly, but the dashboard currently presents the latest reporting session per process.

## Remaining validation / limitations

- Real Quest 3 stereo alignment, comfort, peripheral quality, hardware GPU overhead and resource lifetime soak tests remain unverified.
- Motion-vector/extra composition chains use unchanged passthrough. The temporal method consumes compatible plain KHR depth for its own reprojection, then omits that input depth from widened compositor output. Full-FOV compositor depth is not produced. Other algorithms retain depth-chain passthrough.
- Secondary/quad views, quad/cylinder/cube composition reconstruction, OpenGL, protected/static images, MSAA and multisampled resolves are outside the processing path.
- Startup render dimensions remain reduced if processing is subsequently disabled live; restart for native dimensions. A game that ignores recommendations may not gain performance.
- GPU timing reports delayed individual source copies, history copies, and draw passes; it does not sum total frame cost or compositor work. Device removal may stop processing and report an error; it cannot repair the game's device.
- No claims of OpenXR conformance, zero leaks under every game, or VDXR certification are made from the automated tests alone.

## Vulkan 0.1.4

Vulkan uses the same layer, FOV/projection mapping, temporal metadata, profiles and IPC through native renderer entry points. It requires explicit developer cooperation for image layout and producer completion. Other Vulkan applications keep native FOV and diagnostic passthrough; there is no implicit Vulkan dispatch layer. See [the backend design](VULKAN_BACKEND_DESIGN.md) and [synchronization proof](VULKAN_SYNCHRONIZATION.md). With ambiguous Vulkan plus Direct3D extension lists, early size recommendations stay native until a compatible session is known.
