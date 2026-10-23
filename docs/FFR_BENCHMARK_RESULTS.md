# Automatic D3D11 measurements — 0.1.9

The NVIDIA RTX 4060 fixture renders a 512×512 scene using ordinary immediate-context draws without cooperation calls. FFR disabled measures 262,144 pixel-shader invocations; automatic FFR at 80% FOV measures 89,344 (65.9% fewer). The 8x MSAA stereo fixture measures 146,944 invocations per eye versus 262,144 disabled (43.9% fewer). Instanced and indirect paths are checked independently. Central pixel readbacks match the baseline, a 64×64 partial viewport stays at 4,096 invocations, and disabled FFR restores 262,144. These are workload counts in a controlled GPU fixture, not Rumble FPS. No in-game performance improvement is inferred. Both eye slices at 1x/2x/4x/8x MSAA passed workload, center-pixel equality and opaque-coverage checks. A separate [Rumble live capture](ffr-019-evidence/rumble-live.json) confirms executed draws and user-accepted stereo visuals.

Current raw evidence: [automatic D3D11](ffr-019-evidence/automatic-d3d11.json), [regression execution](ffr-019-evidence/27-tests.txt).

# FFR measurements — 0.1.8

NVIDIA GeForce RTX 4060, Windows x64, D3D12 debug validation enabled. The automatic test uses ordinary direct command-list draws and **makes no cooperation API calls**. Sixteen measured frames follow four warm-up frames. The scene is a 512×768 target with actual pipeline-statistics queries. All values below are measured shader invocations, not modeled quality percentages or headset FPS.

| Automatic test | Mean pixel-shader invocations |
|---|---:|
| FFR disabled; 80% FOV | 393,216 |
| Automatic FFR; 80% FOV | 200,448 |
| Automatic FFR; 80% FOV, RE Engine primitive MAX | 200,448 |
| Automatic FFR; full FOV | 382,464 |
| Partial overlay viewport (64×64) | 4,096 |
| Foreign application-owned 2×2 VRS | 98,304 |

The reduced-FOV fixture records **49.0% fewer pixel-shader invocations**. Both recorded draw commands and completed queue-fence evidence are checked. A following small viewport draw remains 1×1; application-owned VRS is preserved. Exactly two CPU map builds serve the repeated stable geometries, preventing per-draw map generation. Unknown mono-eye identity conservatively combines both per-eye Centers, so full-FOV savings are lower than an exact-eye map.

The separate cooperative D3D12 benchmark still includes source copies and reconstruction in its GPU timeline. Full-FOV reconstruction adds cost; scene shader savings alone do not promise lower total frame time. D3D11 NVAPI timing and workload measurements remain explicit-cooperation fixtures. Outside scene suppression is unavailable and attributed **0% original scene savings** on every backend.

Raw current evidence: [automatic D3D12](ffr-018-evidence/automatic-d3d12.json), [D3D12 timelines](ffr-018-evidence/benchmark.json), [D3D11 NVAPI](ffr-018-evidence/d3d11-nvapi.json), [layer integration](ffr-018-evidence/layer.json), [D3D11 layer](ffr-018-evidence/d3d11-layer.json), [27-test execution](ffr-018-evidence/27-tests.txt). A separate [RE3 live capture](ffr-018-evidence/re3-live.json) confirms automatic D3D12 draws completed through VDXR with Toolkit absent. Controlled GPU results and command execution counters do not establish headset visuals or game FPS.

<!-- HISTORICAL_015 --># FFR benchmark results — 0.1.5

Executed on NVIDIA GeForce RTX 4060, Windows x64, with the D3D12 debug layer enabled. These are actual rasterized scene draws using Tier 2 shading-rate images, not blur or synthetic timing estimates. They are **controlled fixture measurements**, not Hubris/headset performance.

## Method and timing limits

Each case renders two distinct-eye grid patterns into a packed target. Full source/output size is 1024×768; reduced source dimensions are recorded below. A pixel shader executes arithmetic work. The existing gradient Reconstruction Manager reconstructs reduced-FOV source images into the full output. Each case uses 16 warm-up and 64 measured frames. No GPU map allocation occurs after warm-up. Cold shader/pipeline compilation is excluded.

Pipeline-statistics queries count actual pixel-shader invocations. Scene GPU timestamps bracket rate-map updates, scene rendering and state restoration. Frame GPU **timeline elapsed** timestamps bracket the scene and subsequent reconstruction submissions; elapsed time can include queue idle while the CPU records work. Post-scene timeline time includes source copy/reconstruction and that possible idle; it is not a pure isolated reconstruction shader cost. CPU record/submit and the fixture's explicit CPU fence wait are measured separately. FFR itself never waits for a busy map; it refuses that scope. These small GPU timings and the debug-enabled environment must not be extrapolated to production games.

| Case | Source | Scene GPU mean ms | Frame timeline mean ms | Pixel shader invocations | Invocation reduction |
|---|---|---:|---:|---:|---:|
| full-baseline-q65 | 1024×768 | 0.1872 | 0.1907 | 786,432 | — |
| full-ffr-q65 | 1024×768 | 0.0795 | 0.0830 | 286,208 | 63.6% |
| reduced-85-85-baseline-q65 | 870×652 | 0.1456 | 0.2086 | 567,240 | — |
| reduced-85-85-ffr-q65 | 870×652 | 0.0773 | 0.1441 | 246,594 | 56.5% |
| reduced-70-100-baseline-q65 | 716×768 | 0.1435 | 0.2069 | 549,888 | — |
| reduced-70-100-ffr-q65 | 716×768 | 0.0721 | 0.1353 | 241,152 | 56.1% |
| full-ffr-q75 | 1024×768 | 0.1296 | 0.1331 | 476,352 | — |
| reduced-85-70-ffr-q50 | 870×537 | 0.0661 | 0.1304 | 203,711 | — |

## Frame distribution and CPU overhead

| Case | GPU timeline p50 / p95 / p99 ms | Post-scene timeline mean ms | CPU record/submit mean ms | Fixture CPU fence-wait mean ms | 90 / 120 Hz timeline budget overruns |
|---|---:|---:|---:|---:|---:|
| full-baseline-q65 | 0.1905 / 0.1935 / 0.1976 | 0.0035 | 0.1322 | 1.3061 | 0 / 0 |
| full-ffr-q65 | 0.0819 / 0.0881 / 0.0881 | 0.0035 | 0.1895 | 1.0791 | 0 / 0 |
| reduced-85-85-baseline-q65 | 0.2079 / 0.2161 / 0.2181 | 0.0630 | 0.3231 | 1.0930 | 0 / 0 |
| reduced-85-85-ffr-q65 | 0.1475 / 0.1505 / 0.1526 | 0.0668 | 0.3782 | 0.9756 | 0 / 0 |
| reduced-70-100-baseline-q65 | 0.2089 / 0.2109 / 0.2130 | 0.0634 | 0.3236 | 1.1286 | 0 / 0 |
| reduced-70-100-ffr-q65 | 0.1341 / 0.1403 / 0.1403 | 0.0632 | 0.3706 | 0.9640 | 0 / 0 |
| full-ffr-q75 | 0.1352 / 0.1362 / 0.1382 | 0.0035 | 0.1956 | 1.1685 | 0 / 0 |
| reduced-85-70-ffr-q50 | 0.1321 / 0.1341 / 0.1352 | 0.0643 | 0.3696 | 0.9743 | 0 / 0 |

The paired full-FOV and reduced-FOV cases demonstrate real shading-workload and GPU timeline reductions in this fixture. The conservative 75% and aggressive 50% cases demonstrate different policies; they are not promises of literal 75%/50% shading quality. Hardware-rate selection is recorded in the raw JSON.

Timeline budget counts compare the measured GPU interval with 90/120 Hz budgets. They are **not actual missed headset deadlines**. `headsetMissedDeadlines` is null. The fixture blocks for result collection, so wall-clock throughput includes test synchronization and must not be reported as game FPS.

Center-patch RGB values match the non-FFR baseline exactly in paired cases. Actual GPU readbacks assert opaque coverage and no unintended black gaps in the colored pattern. A post-End draw in the previously coarse viewport executes at full rate. Full-FOV mode retains all rendered pixels and avoids reconstruction; reduced mode reconstructs only its established crop mapping. The streamer-baseline integration case keeps three zones and no added crop.

## Verification and remaining acceptance

All nineteen CTest tests passed, including the real D3D12 FFR fixture, existing D3D11/D3D12 reconstruction regressions, GUI localization and nine Vulkan cases with Khronos synchronization validation required. D3D12 debug validation reported no errors. The GUI was rechecked after final text/localization adjustments.

[Raw measurements](ffr-evidence/benchmark.json), [layer cases](ffr-evidence/layer.json), [execution summary](ffr-evidence/execution-summary.txt), [validation state](ffr-evidence/validation.json), and [capture descriptions](ffr-evidence/README.md).

Hubris, production OpenXR runtime rendering, compositor frame deadlines, headset comfort, AMD/Intel devices and third-party scene integration were not tested. D3D11/Vulkan scene FFR is unsupported. See the [manual Hubris acceptance procedure](FFR_TESTING.md); these remaining checks are not counted as automated passes.
