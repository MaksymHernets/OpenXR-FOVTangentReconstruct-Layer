# Third-party components

- Vulkan-Headers: Khronos Group, Apache-2.0 or MIT as specified by each file, pinned to v1.3.290, commit `b379292b2ab6df5771ba9870d53cf8b2c9295daf`. See `external/Vulkan-Headers/LICENSE.md` and `LICENSES` after bootstrap. The Vulkan loader and GPU driver come from the system; no Vulkan SDK is required to run the package.
- glslang 16.6.0: Khronos Group and contributors, used only as a build-time shader compiler. Bootstrap verifies the official Windows archive SHA-256 `82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b`. Its executable/libraries are not included in the application package. See [upstream license](https://github.com/KhronosGroup/glslang/blob/16.6.0/LICENSE.txt).
- OpenXR SDK headers and optional test loader: Khronos Group, Apache-2.0 or MIT as specified in each file. Bootstrap pins OpenXR-SDK release-1.1.53, commit `75c53b6e853dc12c7b3c771edc9c9c841b15faaa`. See `external/OpenXR-SDK/LICENSE` and its LICENSES directory after bootstrap.
- `common/json.hpp`: nlohmann/json 3.12.0, copyright Niels Lohmann, MIT license. The complete notice is embedded in the header.
- Qt 6 Core, Gui, Widgets, Network and Windows platform plugin: Qt Group and contributors. Use your Qt distribution's license terms (LGPLv3/GPL/commercial as applicable). This application dynamically links Qt; retain the Qt notices and satisfy redistribution requirements when publishing a package. The repository does not vendor Qt.
- Direct3D, HLSL compiler, Windows SDK and Visual C++ runtime: Microsoft, under their respective SDK and redistribution terms.

## NVIDIA NVAPI headers (0.1.7)

Official NVIDIA NVAPI headers at commit `70d337db9186e968eab622f7e786de7e437faf3d`, MIT license,
Copyright (c) 2019–2026 NVIDIA CORPORATION & AFFILIATES. Bootstrap pins and verifies each header's SHA-256.
Only declarations are compiled; no NVAPI import library or vendor DLL is distributed.
The installed driver's `nvapi64.dll` is resolved from Windows System32 at runtime.
See `docs/licenses/NVAPI.txt` for the full license.

OpenXR Toolkit's MIT-licensed VRS implementation was studied as a design reference.
No Toolkit implementation source is included. See `docs/FFR_ARCHITECTURE_AUDIT.md` for references and integration limits.

## MinHook 1.3.4 (0.1.8)

D3D12 scene interception uses the official [MinHook source](https://github.com/TsudaKageyu/minhook/tree/c3fcafdc10146beb5919319d0683e44e3c30d537), pinned commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.
The bootstrap verifies SHA-256 for every compiled file. MinHook and its HDE component have BSD notices reproduced in `docs/licenses/MinHook.txt`. No Toolkit implementation source is copied.
