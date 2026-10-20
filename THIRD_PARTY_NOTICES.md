# Third-party components

- OpenXR SDK headers and optional test loader: Khronos Group, Apache-2.0 or MIT as specified in each file. Bootstrap pins OpenXR-SDK release-1.1.53, commit `75c53b6e853dc12c7b3c771edc9c9c841b15faaa`. See `external/OpenXR-SDK/LICENSE` and its LICENSES directory after bootstrap.
- `common/json.hpp`: nlohmann/json 3.12.0, copyright Niels Lohmann, MIT license. The complete notice is embedded in the header.
- Qt 6 Core, Gui, Widgets, Network and Windows platform plugin: Qt Group and contributors. Use your Qt distribution's license terms (LGPLv3/GPL/commercial as applicable). This application dynamically links Qt; retain the Qt notices and satisfy redistribution requirements when publishing a package. The repository does not vendor Qt.
- Direct3D, HLSL compiler, Windows SDK and Visual C++ runtime: Microsoft, under their respective SDK and redistribution terms.
