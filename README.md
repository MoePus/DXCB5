# DXCB5

DXCB5 is an experimental fork of Microsoft's [DirectX Shader Compiler (DXC)](https://github.com/microsoft/DirectXShaderCompiler) that compiles HLSL directly to native **Shader Model 5.0 DXBC** (`vs_5_0` / `ps_5_0`) for Direct3D 11.

It pairs DXC's modern HLSL frontend and LLVM middle-end with a dedicated Vector SSA MIR backend, lane-aware register allocator, structured control-flow emitter, and DXBC container writer (`RDEF`, `ISGN`, `OSGN`, `SHEX`, `SFI0`).

## Highlights

- **Code quality & GPU speed**: Matches or beats FXC instruction counts across heavy Unity/VRChat shaders (e.g., 99.26% of FXC total instructions across MoeToon2 passes; 0.984× FXC frame time on RTX 4080 in pixel-exact scissor benchmarks).
- **Broad SM5.0 coverage**: Supports textures/samplers/arrays, `Buffer`/`StructuredBuffer`/`ByteAddressBuffer` SRVs, full UAVs (typed/structured/raw, atomics, counters), `gather4*`, `ldms`, `lod`, `GetDimensions`, `EvaluateAttribute*`, `SV_Depth*`, `SV_Coverage`, `SV_SampleIndex`, `min16*` types, `sdiv`/`srem`, `uaddc`/`usubb`, bitfield ops, and mutable `static` / local arrays.
- **Verified**: Validated against FXC via D3D11 WARP pixel-exact readback tests, Lean 4 formal proofs for parallel copy lowering, and 740/796 (93.0%) FXC-valid SM5 shaders in DXC's test suite.

## Building

Requires Visual Studio (MSVC x64), CMake, Ninja, and Windows SDK (`d3d11`, `d3dcompiler`).

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -C cmake/caches/PredefinedParams.cmake
cmake --build build --target dxcb5 DXCB5WarpABITest -j
ctest --test-dir build/projects/dxcb5 --output-on-failure
```

## Usage

```powershell
# Compile vertex shader to SM5.0 DXBC
dxcb5.exe -T vs_5_0 -E VSMain -Fo vs.dxbc shader.hlsl

# Compile pixel shader linked against a vertex shader's output signature
dxcb5.exe -T ps_5_0 -E PSMain --vs-dxbc vs.dxbc -Fo ps.dxbc shader.hlsl

# Optional: preserve strict NaN semantics in FP optimizations (auto-enabled on -Od / -Gis)
dxcb5.exe -T ps_5_0 -E PSMain --vs-dxbc vs.dxbc --nan-safe -Fo ps.dxbc shader.hlsl
```

## Current Scope & Limits

- **Targets**: `vs_5_0` and `ps_5_0` (compute/geometry/hull/domain shaders are not targeted yet).
- **Explicitly rejected**: 64-bit numeric types (`double`, `int64_t`/`uint64_t`), D3D11.2 tiled-resource status/clamp, D3D11.3 `SV_InnerCoverage`, and `tbuffer`.

## License

Built on [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) under the University of Illinois Open Source License (see [LICENSE.TXT](LICENSE.TXT)).
