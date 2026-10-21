# FFR evidence

- PNGs are lossless conversions of actual GPU readback PPMs from rasterized D3D12 scenes, not game/headset captures.
- `full-baseline-q65` and `full-ffr-q65` compare uniform shading with three-zone VRS. Each half is one eye with its own projection-aware map.
- `reduced-85-85-ffr-q65` includes existing gradient reconstruction outside the two-zone active source.
- `layer-case-0` is full-FOV FFR without reconstruction; `layer-case-1` is additional crop with reconstruction; `layer-case-4` uses a reduced runtime/streamer baseline without additional application crop.
- `benchmark.json` retains GPU timestamps, pipeline-statistics counts, percentiles, CPU timings and rate plans.
- `layer.json` records actual baseline/active FOVs and output behavior for five integration cases.
- `full-execution.txt` retains detailed output from all nineteen tests, including the actual Vulkan validation-enabled state.
- `execution-summary.txt` records the complete nineteen-test CTest summary; `gui-recheck.txt` records the final GUI-only recheck. Local workspace/SDK paths are normalized.
- `validation.json` records required Vulkan synchronization validation and D3D12 debug-layer activation from the executed fixtures.

Configured preview screenshots are named `ui-*`; their colors describe planned zones, not independently confirmed hardware application.
