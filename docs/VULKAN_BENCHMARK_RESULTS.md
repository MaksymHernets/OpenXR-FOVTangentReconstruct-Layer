# Vulkan benchmark results — 0.1.4

Measured on NVIDIA GeForce RTX 4060, Windows x64, NVIDIA 616.92 driver. The fixture requests Vulkan 1.1. This is a controlled GPU test, not a headset or game benchmark.

## Method

Each mode runs 16 warm-up frames and 64 measured frames, with two 128×128 source views and two 160×160 output views. Sources are animated uploaded grid/marker patterns. Full GPU image readback checks output after each frame. The fixture wall time below includes that readback and excludes application scene upload; it must not be interpreted as game frame time or FPS improvement.

GPU values are the last completed timestamp sample for each operation, not averages across measured frames. Left/right processing timestamps are separate. CPU submission measures queue submission alone. Temporal benchmark mode has no usable history and measures its adaptive fallback; rotation, translation, occlusion and depth were tested separately. Runtime validation is off for this performance baseline.

| Mode | GPU left / right (ms) | Source copy GPU (ms) | CPU submit (ms) | Fixture wall mean (ms) | Warm-state allocations |
|---|---:|---:|---:|---:|---:|
| clamp | 0.010208 / 0.011392 | 0.009248 | 0.0083 | 1.2165 | 0 |
| gradient | 0.010880 / 0.012096 | 0.009408 | 0.0083 | 1.1822 | 0 |
| blur | 0.010336 / 0.011424 | 0.009152 | 0.0084 | 1.1839 | 0 |
| mirror | 0.010208 / 0.011168 | 0.009312 | 0.0084 | 1.2114 | 0 |
| solid | 0.009984 / 0.011104 | 0.009664 | 0.0083 | 1.1825 | 0 |
| temporal-no-history-fallback | 0.017952 / 0.019136 | 0.009632 | 0.0103 | 1.1987 | 0 |

## Interpretation and limits

Tracked explicit optimizer device-memory allocations were 324,096 bytes at the end of the benchmark (peak 324,096). These exclude application/runtime images and opaque driver/pipeline allocations. No optimizer device-memory allocations occurred after warm-up in any measured mode. The total allocation/submission counters in the JSON include preceding format, temporal and lifetime tests.

The normal path polls its own fences and never calls `vkQueueWaitIdle` or `vkDeviceWaitIdle`. Teardown and resource recreation drain outstanding optimizer fences. Tests use blocking readback and producer waits deliberately, so fixture throughput is not normal optimizer throughput. Full GPU frame time is unavailable (`gpuFrameMs: null`); there is no measured headset performance gain. No AMD, Intel, SteamVR or VDXR Vulkan session was measured.

## Evidence

[Raw performance baseline](vulkan-evidence/benchmark.json), [execution log](vulkan-evidence/execution.txt), and [capture descriptions](vulkan-evidence/README.md). Test journals record array, enable2, separate, packed and depth view mappings. Captures are actual GPU readbacks from the controlled fixture, not screenshots of a production game.

All seventeen CTest tests passed on this machine, including the existing D3D11/D3D12 and GUI regression suite. See the [compatibility matrix](VULKAN_COMPATIBILITY_MATRIX.md) for untested environments.
