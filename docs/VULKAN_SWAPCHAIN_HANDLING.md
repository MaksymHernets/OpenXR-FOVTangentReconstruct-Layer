# Vulkan swapchain handling

`XR_KHR_vulkan_enable` and `XR_KHR_vulkan_enable2` sessions use the installed SDK's Vulkan structures. In the pinned OpenXR 1.1.53 headers, Vulkan2 binding/image types and structure tags alias their Vulkan1 equivalents. One detection/enumeration branch handles both; the fixture compiles and tests these aliases explicitly.

## Input lifecycle

1. Create forwards the application's creation information. Transfer-source usage is added only for an explicitly cooperating session and supported input configuration. If that request fails, the original request is retried and processing is disabled for that chain. Ordinary diagnostic-only Vulkan creation does not add optimizer usage flags.
2. Enumeration preserves downstream results and records runtime `VkImage` handles. The optimizer never destroys those images or their memory. Internally requested enumerations use correctly initialized Vulkan image headers.
3. Acquire records the returned index and a monotonically increasing generation. No association between eye number and acquired image index is assumed.
4. A successful wait marks the oldest outstanding acquisition ready. Timeout leaves it unready. A declaration made before successful wait, for a different index, or after release is rejected.
5. A cooperating application declares the exact layout, bound queue and completed producer fence. Release rechecks the declaration against the acquisition generation, checks fence status without waiting, copies the image and restores its layout. Missing/incomplete evidence forwards release with no GPU image access.
6. Successful release consumes the declaration. Failed release retains acquisition state for a legitimate retry. Capture provenance includes the current frame epoch; stale snapshots cannot stand in for a newly released Vulkan image.
7. Destruction drains optimizer work, frees owned snapshots/history/scratch and borrowed-image views, then forwards runtime swapchain destruction. Implicit session teardown follows the same ordering before the runtime destroys remaining chains.

The layer serializes its object tracking. The application must still satisfy Vulkan/OpenXR external synchronization of its own queue and object access.

## Stereo and output ownership

The copied `XrSwapchainSubImage` supplies the actual array index and rectangle. Supported patterns include separate per-eye chains, array images usable by multiview applications, and packed rectangles. The compute shader itself does not require the multiview feature. Reversed eye-to-array mappings and nonzero acquired image indices are tested. Multiple nonoverlapping projection subimages can share an output image. Overlapping conflicting reconstructions, unsupported extra view/layer chains, invalid rectangles and unmatched FOV provenance retain the submitted source/FOV pair.

The optimizer creates a separate output chain with expanded dimensions. It first requests color-attachment, transfer-destination and storage usage, then retries without storage if rejected. It only selects direct storage when the accepted usage and format features permit it. Otherwise, private RGBA8 compute output is copied to the runtime transfer destination. Runtime images are not assumed to be storage images. If neither legal path is available, reconstruction is declined.

Output acquire/wait is tracked independently. A timeout is retried without double acquisition. The first write to an output layer can legally discard previous contents; later packed writes preserve previous regions. Every processed output layer returns to COLOR_ATTACHMENT_OPTIMAL before release. Only fully submitted output views replace the application's copied submission structures.

## Supported formats and fallback

| Kind | Supported formats |
|---|---|
| Color | RGBA8 UNORM/sRGB, BGRA8 UNORM/sRGB |
| Depth | D16 UNORM, D32 SFLOAT, D24 UNORM/S8, D32 SFLOAT/S8, when format features and compatible KHR depth metadata are available |
| Direct storage output | RGBA8 UNORM with accepted storage usage |
| Intermediate output | The supported color formats with legal transfer-destination usage |

All require one sample, mip and face; positive device-supported dimensions/array size; no protected/static flags or unsupported creation chain. HDR/10-bit color, MSAA resolve, secondary/quad views and arbitrary view extensions are not processed. Depth is never inferred from a color image. A narrowed depth attachment is consumed only by compatible temporal reconstruction and removed from widened color output; no full-FOV compositor depth is synthesized.
