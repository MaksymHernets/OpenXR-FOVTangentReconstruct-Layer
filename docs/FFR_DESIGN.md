# FFR and Outside design — 0.1.9

## Projection authority

`common/OutsideRegion.h` is shared by FOV processing eligibility, FFR plans and renderer constants. It computes tangent-span ratios against the actual downstream runtime baseline, the active rectangle in baseline coordinates and the forward-axis center in active-eye coordinates. Both axes within 1e-4 of one select `TwoRenderedZonesWithOutside`; any actual reduction selects `TwoRenderedZonesWithCrop`. Both have two rendered VRS quality regions and three logical regions. A streamer baseline is never treated as an additional application crop.

At full effective FOV, Outside is an ellipse or rectangle around the per-eye projected center, bounded by configured Middle radii and clamped to the viewport. At reduced FOV, Outside is only the projection-missing area; Middle covers the active periphery. The full-FOV mask is not applied a second time over that crop. Original projection, source subrects, array slices and the existing output swapchain mapping are retained.

## Shared reconstruction

`ShaderConstants` carries the per-eye mask, shape, diagnostic view, configured safe fallback and Center parameters. C++/HLSL/GLSL layouts are synchronized (144-byte constants; 896-byte Vulkan parameters). All backends invoke their existing reconstruction shader. Clamp/Gradient/Blur/Mirror/Solid/Temporal Depth use the same algorithm implementations for masks and crops. Full-FOV spatial samples are constrained inside the observation mask with a filtering guard. Masked temporal observations are rejected; missing history uses the configured adaptive-blur or solid fallback. Original source observations alone enter history. Mask setting changes invalidate history, and the existing projection/pose/time/depth checks remain active.

Center/Middle bypass reconstruction unchanged when FFR is enabled. A diagnostic original-input view bypasses the mask sampler. Nonfinite algorithm output uses the configured safe fallback instead of propagating undefined pixels. The layer reports policy/capability separately from measured execution; configured fallback is not reported as measured per-pixel failure coverage.

## VRS maps and scene state

Center and transition-intersecting tiles stay 1x1. Coarse Middle tiles must be wholly inside the full-FOV observation mask. Outside and tiles touching it stay 1x1. Packed-eye neighboring tiles and partial viewport tiles also stay 1x1. Discrete Middle targets map conservatively to supported rates, not promised quality percentages. The obsolete `outerQuality` JSON field is accepted for old profiles but has no rendering effect and no UI control.

D3D12 retains bounded slots, cached R8_UINT maps and completion-fence validation. D3D11 uses optional driver NVAPI, a cached private R8_UINT map and per-viewport lookup table. Its serialized immediate-context contract starts with NVAPI VRS disabled; End disables viewport VRS and unbinds the image. Other application graphics state is not changed by VRS. Layer capture, end-frame and destruction reject active scopes. GPU tests draw again after End to verify full-rate restoration.

Outside suppression remains unavailable. Map generation, scene shading, source copies and reconstruction have separate costs. No postprocess-only GPU saving is claimed.

## Automatic D3D12 scene path

`D3D12SceneInterceptor` installs method-entry hooks through the pinned MinHook library; it does not replace COM object vtables. Device descriptors are tracked without retaining scene textures. Candidate single-sample targets match the aspect and scale of real submitted eye rectangles. All MRTs must agree. Full-target viewports and packed eye halves are eligible; partial viewports are excluded. An unknown mono-eye assignment uses the finer rate from both per-eye plans, preserving both Center regions at some cost in savings.

Successful command-list Reset establishes known default VRS state. Observed application shading-rate/image calls update that state. Bound foreign images, coarse base rates and unsupported combiner modes are preserved and skipped. RE Engine's 1x1/no-image MAX-per-primitive state is supported: retain its first combiner, combine our image using MAX, and restore both original combiners afterward. Native render passes, indirect commands and bundles do not receive injected state. Eligible direct draws bind their immutable rate map immediately before drawing and restore the original rate, combiners and image immediately afterward. Layer reconstruction is suppressed from interception.

Map lookup keys include geometry generation, target dimensions, viewport and packed layout. CPU map generation occurs once per new shape, not per draw. GPU map resources are immutable, initialized privately on the session queue and used only after observed fence completion. There are at most 64 cached maps, 16 target shapes and 2,048 tracked recordings per automatic session. Unsupported/budget cases skip optimization. One automatic D3D12 session per process is supported; other sessions retain existing rendering and optional explicit cooperation.

Queue submission hooks record executed draw evidence with dedicated queue fences. Recorded command lists retain map COM owners under a private GUID. On session teardown, hook entries are disabled, in-flight callbacks finish, hooks are removed, and observed queues are checked for completion before ordinary backend ownership is released. Device loss/timeout retains resources rather than freeing potentially in-flight memory. Closed lists keep their referenced maps alive through their own private lifetime owner. There are no per-frame queue-idle waits.

## UI and diagnostics

FFR keeps separate full/reduced Middle qualities, configured radii, offsets, geometry, policy and reconstruction selection. Outside reconstruction remains visible in both modes. Debug view indices are 0 final composite, 1 Center, 2 Middle, 3 Outside mask, 4 actual crop, 5 original input, 6 reconstructed Outside on black, 7 diagnostic Outside color fill. Color fill uses the existing persistent Edge Fill color. `outsideFallback` is 0 adaptive blur, 1 configured solid color. Normal Solid Color's default black remains intentional.

IPC separates effective per-eye geometry, backend support, supported rates, recorded rates, fence-observed draw execution, selected method, fallback policy and Outside suppression (false, 0% savings). A preview never establishes actual hardware activation.

## Automatic D3D11 NVAPI integration — 0.1.9

The session device's immediate context is intercepted using the existing pinned MinHook dependency. Render-target binding (including OM UAV binding), viewports, direct/indexed/instanced/indirect/DrawAuto calls, ClearState and deferred command-list execution are observed. Only matching proportional projection-sized texture targets, one full viewport and up to two RTV array slices are eligible. Non-MSAA and 2x/4x/8x MSAA layouts are supported; both slices share the conservative center-preserving map. Packed targets use eye halves; unknown mono-eye identity conservatively keeps both eye centers at full rate. No game/runtime files are modified.

NVAPI viewport-rate and map entry points are also observed. Foreign rates/images cause refusal; an already loaded OpenXR Toolkit blocks automatic installation because no general preinstallation NVAPI state getter is available. The normal fresh-context baseline is disabled rates and no map. Each eligible draw binds a cached immutable map/table and restores that baseline immediately afterward. Both texture and view stay alive across concurrent profile changes. The layer's reconstruction is excluded using thread-local suppression. Explicit cooperating scopes still work and are observed as foreign ownership during their draws.

D3D11 GPU event queries are emitted in bounded batches and polled with DONOTFLUSH on the application's immediate-context draw thread. Diagnostics never issue context calls from the IPC worker. No GPU wait or forced Flush is introduced. Completed markers confirm NVAPI-wrapped draws executed; they do not measure game FPS or shading-frequency savings. A deferred command-list execution suspends the automatic path because extension state cannot be established safely. Deferred recording, arrays beyond two slices, unsupported sample counts and ambiguous/partial viewports remain unsupported. Outside original scene suppression remains unavailable.
