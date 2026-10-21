# Vulkan implementation report â€” 0.1.4

## Delivered

- Extended the existing OpenXR API layer, dispatch and `IEdgeRenderer`; no separate optimizer application or replacement layer.
- Added Vulkan/enable2 binding detection, borrowed-device dispatch, checked queue/capability initialization and explicit diagnostic passthrough.
- Added runtime image enumeration, FIFO acquire/wait/release generations, exact cooperative layout/fence declarations and original-only GPU snapshots.
- Added a build-time validated SPIR-V compute backend for all six existing algorithms, including bounded per-eye temporal rotation and depth reconstruction.
- Added direct RGBA8 storage and private-intermediate/transfer paths with checked usage/format support and explicit barriers.
- Added completion-protected command/descriptor reuse, owned-resource cleanup, swapchain recreation handling and nonblocking ring-capacity fallback.
- Added nested Vulkan profile policy, manager preservation, local diagnostics, sampled GPU coverage, per-view timestamps, CPU measurements and memory/allocation counters.
- Added real-GPU tests and a developer-owned OpenXR runtime fixture for arrays, enable2 aliases, separate-eye, packed and depth configurations.
- Added the architecture audit, backend design, swapchain handling, synchronization proof, reconstruction/integration guide, compatibility matrix, benchmark results, frame captures and fixture journals.

## Milestone status

| Milestone | Status |
|---|---|
| Architecture audit | Completed before implementation |
| Vulkan session support | Implemented and exercised in the fixture |
| Swapchain tracking | Implemented and exercised |
| Synchronization design | Explicit cooperative contract; documented proof and refusal cases |
| Minimal solid fill | Real GPU execution and center/periphery pixel checks |
| Existing reconstruction modes | Ported and exercised on the GPU |
| Performance and compatibility | Controlled measurements available; production-runtime/game validation remains separate |
| Regression and documentation | D3D11/D3D12/GUI tests retained and extended |

## Deliberate limits

General third-party Vulkan image modification is **not enabled**. This OpenXR layer does not observe arbitrary Vulkan command recording, enabled device features, layouts or cross-queue synchronization. It does not install a Vulkan API layer, replace global Vulkan functions, assume release is a fence, or fabricate completion. The developer cooperation API supplies the missing facts only for applications that explicitly obey its contract. Other Vulkan sessions keep native FOV/recommendations and emit diagnostics. Merely selecting `auto` does not bypass the contract.

The backend does not reconstruct compositor depth, resolve MSAA, process HDR/10-bit color, infer missing surfaces, repair a lost application device, or guarantee reduced application work. Synthetic fixture scenes are uploaded source patterns, not a rasterized game benchmark. Full image readbacks are confined to the test fixture. Khronos validation v1.3.290 was built from its official pinned source and run with synchronization validation required across all nine Vulkan tests. Creation, processing and teardown completed with zero reported validation errors. Shader validation and GPU pixel/state/refusal checks also passed. These controlled tests do not establish production-runtime compatibility.

## Reproduce

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1 -QtPrefix 'C:/Qt/6.8.3/msvc2022_64'
ctest --test-dir build-release -R vulkan --output-on-failure
```

The Qt path is illustrative. CTest's Vulkan GPU tests return an explicit skip code (77) if the Vulkan loader/driver/GPU is unavailable; a skip is not a GPU pass. Other Vulkan failures remain test failures. A capable GPU is required to reproduce the executed results. Set `VK_LAYER_PATH` to an installed validation-layer manifest directory to run the fixture with Khronos validation; it reports whether validation was actually enabled. Set `FOV_REQUIRE_VULKAN_VALIDATION=1` to make missing validation, loader or GPU a failure instead of an unavailable/skip result.

Runtime validation used official Vulkan-ValidationLayers v1.3.290 with its matching `known_good.json` Vulkan-Utility-Libraries and SPIRV-Headers/Tools revisions. Optional mimalloc and robin-hood hashing were disabled; the layer was built locally without installing it globally or redistributing it in the product. To require validation in an existing build:

```powershell
$env:VK_LAYER_PATH = 'C:/path/to/validation-layer-manifest-and-dll'
$env:FOV_REQUIRE_VULKAN_VALIDATION = '1'
ctest --test-dir build-release -R '^vulkan' --output-on-failure
```

The [required-validation execution log](vulkan-evidence/validation-execution.txt) records the actual enabled state. The [performance baseline](vulkan-evidence/benchmark.json) deliberately has runtime validation disabled.

Full generated artifacts reside under `build-release/vulkan-evidence` and `build-release/Testing/Temporary/LastTest.log`. Selected portable evidence is retained in `docs/vulkan-evidence`; production application data and absolute user paths are not published.
