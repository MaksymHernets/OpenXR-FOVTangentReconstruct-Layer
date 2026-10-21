# Vulkan compatibility matrix â€” 0.1.4

| Environment / feature | Evidence | Status |
|---|---|---|
| Windows x64 / NVIDIA GeForce RTX 4060 | Actual Vulkan GPU shader tests and controlled OpenXR fixture | Executed |
| Vulkan API available on test GPU | Physical API 1.4.351; fixture requests 1.1 | Executed Vulkan 1.1 device path |
| OpenXR Vulkan enable / enable2 bindings | Separate fixture runs, installed-header alias assertions | Executed |
| Separate-eye, array, reversed eye slices, packed rectangles | Actual GPU output readbacks through the layer fixture | Executed |
| Solid, Clamp, Gradient, Blur, Mirror | SPIR-V pixel tests; RGBA/BGRA UNORM/sRGB | Executed |
| Rotation-only / depth reconstruction | Actual GPU history, standard/reversed-Z, all four supported depth formats | Executed |
| Missing/stale/wrong-index/wrong-queue/unverified-layout evidence | Controlled declaration/refusal/recovery checks | Executed refusal paths |
| Incomplete producer fence | Actual unsignaled Vulkan fence; no wait in optimizer | Executed passthrough |
| Output wait timeout | Fixture timeout then successful retry | Executed |
| Repeated/implicit session teardown | Two sessions per fixture, explicit and implicit chain cleanup | Executed |
| D3D11 and D3D12 | Existing WARP shader, temporal and dispatch regression suites | Executed |
| Qt manager / eleven locales | Existing GUI suite plus Vulkan policy preservation | Executed |
| SPIR-V validation | glslang `--spirv-val`, Vulkan 1.1 target | Executed |
| Khronos Vulkan runtime/synchronization validation layer | Source-built v1.3.290; all nine Vulkan tests with synchronization validation required | Executed; zero validation errors |
| SteamVR OpenXR runtime + headset | No production-runtime session execution evidence | Not tested |
| VDXR / Quest 3 | VDXR installed; no Vulkan headset/session execution evidence | Not tested |
| Third-party Vulkan OpenXR games | No installed Vulkan dispatch layer or producer-state integration | Diagnostic passthrough; reconstruction disabled |
| AMD / Intel / other driver versions | No execution evidence | Not tested |
| Device-loss injection / GPU reset | No deliberate device reset performed | Error handling implemented, not injected/tested |
| HDR, MSAA resolve, protected/static/secondary views | Outside this backend's supported processing contract | Passthrough |

The driver reported NVIDIA 616.92.0.0 / 616.92, Vulkan instance 1.4.341 and device 1.4.351. These values describe this execution environment, not a required driver version or a general compatibility guarantee. No FPS gain, headset comfort, production runtime conformance or third-party compatibility claim follows from the fixture results.
