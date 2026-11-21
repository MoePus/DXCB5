# DXCB5

DXCB5 is a fork of Microsoft's [DirectX Shader Compiler (DXC)](https://github.com/microsoft/DirectXShaderCompiler) that compiles HLSL directly to Direct3D 11 **Shader Model 5.0 DXBC** (`vs_5_0` / `ps_5_0`), packaged as a drop-in `D3DCompiler_47.dll` replacement for Unity 2022.3.

## Installation

1. Add `https://moepus.github.io/vpm/index.json` in **VCC** or **ALCOM**, then find and install **DXCB5** (`com.moepus.dxcb5`) into your project.
2. In Unity, open **`Tools -> DXCB5 Shader Compiler`** and click **Install DXCB5 Proxy**.

## Highlights

Comparison against Unity 2022.3.22f1's bundled FXC (`D3DCompiler_47.dll`) on full-feature `ForwardBase` (`vs_5_0` + `ps_5_0`, `-O3`):

| Shader (Full Feature) | Metric | Unity FXC | DXCB5 | Diff |
| :--- | :--- | ---: | ---: | ---: |
| **lilToon 2.4.1** | Compile Time (VS + PS) | 1,161 ms | **360 ms** | **3.2× faster** |
| | Instructions (VS + PS) | 3,259 (1,442 + 1,817) | 3,317 (1,469 + 1,848) | +1.8% (24/26 vs 28/28 temps) |
| | GPU Frame Time (RTX 4080, 1024²) | 0.180 ms | **0.177 ms** | **-1.5%** |
| **Poiyomi Pro 8.0** | Compile Time (VS + PS) | 714 ms | **289 ms** | **2.5× faster** |
| | Instructions (VS + PS) | 2,478 (51 + 2,427) | **2,378** (52 + 2,326) | **-4.0%** |
| | GPU Frame Time (RTX 4080, 1024²) | 0.287 ms | **0.255 ms** | **-11.1%** |

*Measured with interleaved dual-order D3D11 hardware timestamp queries (160 samples per shader, 0 disjoint frames); both shaders produce 0.0 maximum pixel delta vs FXC across 1,048,576 pixels.*

## Current Scope & Limits

- **Supported stages**: `vs_5_0` and `ps_5_0` (Windows x64 Unity 2022.3 Editor).
- **Automatic FXC fallback**: Compute (`cs_5_0`), geometry (`gs_5_0`), hull/domain (`hs_5_0`/`ds_5_0`), SM 4.0 targets, and any shader rejected by DXCB5 fall back to Unity's original FXC (`D3DCompiler_47_fxc.dll`) automatically.
- **Not supported in DXCB5**: 64-bit numeric types (`double`, `int64_t`/`uint64_t`), D3D11.2 tiled-resource status/clamp, D3D11.3 `SV_InnerCoverage`, and `tbuffer`.

## License

Built on [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) under the University of Illinois Open Source License. See [LICENSE.TXT](LICENSE.TXT).
