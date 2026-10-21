# Registration fix — 0.1.6

A disabled older installation (`REG_DWORD 1`) was incorrectly treated as an active duplicate. This made enabling a newly extracted package fail even with administrator approval. The observed older package was disabled; the new helper's read-only check now reports ready.

The helper now considers only valid enabled DWORD-zero registrations with the same layer name to be competing active copies. Disabled copies and unrelated layers remain untouched. Active duplicates are still blocked, with the conflicting manifest path identified; disable or uninstall the active old copy before enabling another location.

`FovLayerControl.exe --check` validates the adjacent manifest/DLL and inspects conflicts without writing registry values or requesting elevation. The installer runs that preflight first. The manager now reports specific registration reasons and helper exit codes instead of calling every failure an administrator-permission problem.

Verification uses real Windows registry APIs against an isolated temporary HKCU key with process-local HKLM redirection. The old helper reproduced the failure; the fixed helper passes upgrade, duplicate protection, Unicode and idempotency tests. Actual system registrations were inspected read-only and were not altered during verification. See the full test summary beside this report. Registration still takes effect only for newly launched OpenXR applications.

The enabled-value rule follows the [Khronos loader registry specification](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/specification/loader/api_layer.adoc).
