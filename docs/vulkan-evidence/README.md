# Controlled Vulkan evidence

PNG files are lossless conversions of test GPU readback PPMs. They are uploaded synthetic patterns processed by the real Vulkan backend on an RTX 4060; they are not game/headset captures.

- `grid-mode0`, `grid-mode4`, `grid-mode5`: clamp, solid fill, and no-history temporal fallback, respectively; output 160Ã—160.
- `temporal-...`: rotation/depth test cases, per-eye output 128Ã—128. Case details are in `tests/VulkanTests.cpp`.
- `openxr-array-3-eye0/1`: first successful reconstruction through the API layer fixture, reversed eye array slices.
- `openxr-depth-array-3-eye0`: temporal/depth integration fixture.
- `openxr-packed-3-eye0`: both eye rectangles share one output image.
- `openxr-*.json`: runtime-fixture frame submissions, source indices and view mapping.
- `benchmark.json`: performance baseline without runtime validation. See the benchmark report for timing semantics.
- `validation-execution.txt`: complete seventeen-test run with required Khronos synchronization validation for all nine Vulkan tests; includes creation/teardown checks.
- `execution.txt`: complete CTest execution log with local workspace/SDK paths normalized.

Pixel/state assertions and journals provide evidence beyond visual inspection. Runtime/game compatibility is reported separately.
