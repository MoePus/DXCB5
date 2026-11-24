# DXCB5

DXCB5 is a fork of Microsoft's [DirectX Shader Compiler (DXC)](https://github.com/microsoft/DirectXShaderCompiler) that compiles HLSL directly to Direct3D 11 **Shader Model 5.0 DXBC** (`vs_5_0` / `ps_5_0`) for Unity 2022.3.

## Installation

1. Add `https://moepus.github.io/vpm/index.json` in **VCC** or **ALCOM**, then install **DXCB5** (`com.moepus.dxcb5`) into your project.
2. Open your Unity project — DXCB5 activates automatically inside the Editor without modifying Unity's installation directory. Open **`Tools -> DXCB5 Shader Compiler`** to inspect status, view RAM variant cache hits, or toggle options.

## Highlights

Comparison against Unity 2022.3.22f1's bundled FXC (`D3DCompiler_47.dll`) on full-feature `ForwardBase` (`vs_5_0` + `ps_5_0`, `-O3`):

| Shader (Full Feature) | Metric | Unity FXC | DXCB5 | Diff |
| :--- | :--- | ---: | ---: | ---: |
| **lilToon 2.4.1** | Compile Time (VS + PS) | 1,161 ms | **369 ms** | **3.1× faster** |
| | Instructions (VS + PS) | 3,259 (1,442 + 1,817) | 3,291 (1,449 + 1,842) | +1.0% (24/25 vs 28/28 temps) |
| | GPU Frame Time (RTX 4080, 1024²) | 0.174 ms | **0.172 ms** | **-1.0%** |
| **Poiyomi Pro 8.0** | Compile Time (VS + PS) | 727 ms | **295 ms** | **2.5× faster** |
| | Instructions (VS + PS) | 2,478 (51 + 2,427) | **2,225** (52 + 2,173) | **-10.2%** (4/19 vs 4/19 temps) |
| | GPU Frame Time (RTX 4080, 1024²) | 0.268 ms | **0.227 ms** | **-15.1%** |

*Measured with interleaved dual-order D3D11 hardware timestamp queries (160 samples per shader, 0 disjoint frames); both shaders produce 0.0 maximum pixel delta vs FXC across 1,048,576 pixels.*

## Features

See [features.md](features.md) for the full list of `[[dxcb5::...]]` HLSL annotations, two-stage Uber DXIL specialization C API (`dxcb5_spec.h`), Unity Editor integration, and backend optimizations.

## Current Scope & Limits

- **Supported stages**: `vs_5_0` and `ps_5_0` (Windows x64 Unity 2022.3 Editor).
- **Automatic FXC fallback**: Compute (`cs_5_0`), geometry (`gs_5_0`), hull/domain (`hs_5_0`/`ds_5_0`), SM 4.0 targets, and any shader rejected by DXCB5 fall back to Unity's stock FXC automatically.
- **Not supported in DXCB5**: 64-bit numeric types (`double`, `int64_t`/`uint64_t`), D3D11.2 tiled-resource status/clamp, D3D11.3 `SV_InnerCoverage`, and `tbuffer`.

## License

Built on [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) under the University of Illinois Open Source License. See [LICENSE.TXT](LICENSE.TXT).

The Unity Editor hook (`dxcb5_unity_hook.dll`) vendors [MinHook](https://github.com/TsudaKageyu/minhook) v1.3.4 under the BSD 2-Clause License. See [projects/dxcb5/third_party/minhook/LICENSE.txt](projects/dxcb5/third_party/minhook/LICENSE.txt).
