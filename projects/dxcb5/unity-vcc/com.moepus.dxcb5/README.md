# DXCB5 Shader Compiler for Unity (VCC Package)

Replaces Unity 2022.3's D3D11 SM5.0 `vs_5_0` / `ps_5_0` FXC compiler with **DXCB5** via a safe `D3DCompiler_47.dll` proxy inside `Editor/Data/Tools/`.

- **Target Unity**: `2022.3.22f1` (and other `2022.3.x` Windows x64 Editors)
- **Automatic FXC fallback**: Non-VS/PS stages (`cs_5_0`, `gs_5_0`, `hs_5_0`, `ds_5_0`), unsupported features (e.g. `double`), or any DXC front-end error automatically fall back to the backed-up original `D3DCompiler_47_fxc.dll`.
- **Reversible**: Original `D3DCompiler_47.dll` is backed up once to `D3DCompiler_47_fxc.dll` and can be restored in one click.

## Usage

1. Add this package to your project via VRChat Creator Companion (VCC) or Unity Package Manager.
2. In Unity, open **`Tools -> DXCB5 Shader Compiler`**.
3. Click **Install DXCB5 Proxy** (Windows UAC prompt will appear if Unity is installed under `Program Files`).
4. Optionally click **Clear ShaderCache** or reimport your shaders to compile them with DXCB5.
5. Toggle **Enable DXCB5** or **`--nan-safe`** at any time without restarting Unity.

## Uninstalling / Restoring FXC

- **In Unity**: Open **`Tools -> DXCB5 Shader Compiler`** and click **Uninstall & Restore FXC** (restores `D3DCompiler_47_fxc.dll` back to `D3DCompiler_47.dll` and removes `dxcb5_dxcompiler.dll`).
- **Removed directly via VCC**: By default (*Project-scoped only*), the proxy automatically bypasses DXCB5 and routes 100% directly to stock FXC for any project that no longer has `com.moepus.dxcb5` installed. To also physically restore `Editor/Data/Tools/D3DCompiler_47.dll` outside Unity, run `%LOCALAPPDATA%\DXCB5Unity\Restore_FXC.cmd`.
- To recompile previously cached shaders with FXC, clear `Library/ShaderCache` in your Unity project.
