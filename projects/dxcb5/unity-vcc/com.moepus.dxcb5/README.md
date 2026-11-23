# DXCB5 Shader Compiler for Unity (VCC Package)

Compiles Unity 2022.3 D3D11 SM5.0 `vs_5_0` / `ps_5_0` shaders with **DXCB5**, with automatic fallback to Unity's stock FXC for non-VS/PS stages (`cs_5_0`, `gs_5_0`, `hs_5_0`, `ds_5_0`), unsupported features (e.g. `double`), or front-end errors.

- **Target Unity**: `2022.3.22f1` (and other `2022.3.x` Windows x64 Editors)
- **Non-invasive & Project-Scoped**: Loads in-process inside the Unity Editor (`dxcb5_unity_hook.dll` + `dxcb5_cgbatch_plugin.dll`) without modifying Unity's installation directory or requiring administrator privileges.
- **In-Process RAM Variant Deduplication**: On Unity `2022.3.22f1`, caches compiled snippets and reflection in Editor RAM to skip redundant `UnityShaderCompiler.exe` IPC compiles.

## Usage

1. Add this package to your project via VRChat Creator Companion (VCC) or Unity Package Manager.
2. Open your Unity project — DXCB5 activates automatically on Editor load.
3. Open **`Tools -> DXCB5 Shader Compiler`** to inspect status, view session statistics (`Workers Hooked`, `RAM Hits`, `IPC Compiles`), clear caches, or toggle **Enable DXCB5** / **`--nan-safe`** at any time without restarting Unity.
4. Optionally click **Clear Project Library/ShaderCache** to recompile previously cached shaders with DXCB5.

## Uninstalling

- Remove `com.moepus.dxcb5` from your project via VCC or Unity Package Manager. Unity's installation directory is never modified.
- To recompile previously cached shaders with stock FXC, delete `Library/ShaderCache` in your Unity project.

## Third-Party Notices

Includes [MinHook](https://github.com/TsudaKageyu/minhook) v1.3.4 (BSD 2-Clause License).
