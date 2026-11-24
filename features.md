# DXCB5 Features

DXCB5 extends standard HLSL-to-SM5.0 DXBC compilation with `[[dxcb5::...]]` annotations, a two-stage Uber DXIL specialization engine (`dxcb5_spec.h`), native Unity Editor integration, and automatic backend optimizations.

## Predefined Macro & Conventions

| Symbol | Target | Description |
| :--- | :--- | :--- |
| `__DXCB5__` | Preprocessor | Predefined integer macro (`100`) identifying compilation under DXCB5. |
| `cbuffer DXCB5_SpecFlags` | `cbuffer` declaration | Virtual cbuffer whose `uint` members are implicitly treated as `[[dxcb5::immutable]]`, folded from manifest `defines` in Stage 2, and stripped from final `RDEF`. |

## HLSL Annotations (`[[dxcb5::...]]`)

| Annotation | Target | Description |
| :--- | :--- | :--- |
| `[[dxcb5::immutable]]` | `cbuffer` member | Marks a parameter as non-animatable, preserves its branch CFG boundaries in Uber DXIL, and unconditionally folds it during specialization. |
| `[[dxcb5::spec_const]]` | `cbuffer` member | Preserves branch CFG boundaries in Uber DXIL and marks a parameter eligible for full-value constant specialization. |
| `[[dxcb5::spec_values(v0, ...)]]` | `cbuffer` member | Restricts constant specialization metadata to the listed discrete boundary values. |
| `[[dxcb5::spec_zero_one]]` | `cbuffer` member | Shorthand for `[[dxcb5::spec_values(0.0, 1.0)]]`. |
| `[[dxcb5::spec_identity]]` | `cbuffer` member | Shorthand for mathematical multiplicative identity (`1` for scalars/vectors, identity matrix for matrices; use `[[dxcb5::spec_values(1, 1, 0, 0)]]` for scale-offset `*_ST`). |
| `[[dxcb5::dynamic]]` | `cbuffer` member / texture / buffer | Excludes a parameter or resource from automatic static specialization. |
| `[[dxcb5::strict_branch]]` | `if` / `switch` | Prevents both LLVM `SimplifyCFG` and DXCB5 `OptIfConvert` from flattening the branch into `select`/`movc` or hoisting arm code. |
| `[[dxcb5::uniform]]` | `cbuffer` member / `if` / `switch` | Marks a branch condition as quad-uniform for `ControlFlow` quad-operation checks and uniform-path sinking. |
| `[[dxcb5::fast_math]]` | Function / statement / block | Enables local fast-math lowering inside the annotated scope (such as low-latency `cbrt` approximation for `pow(x, 1/3)`). |

## Two-Stage Specialization Engine & C API (`dxcb5_spec.h`)

| Export / Capability | Description |
| :--- | :--- |
| `dxcb5_compile_uber_dxil` | Compiles HLSL (`-HV 2018` / `2021`) into an Uber DXIL bitcode blob and optional reflection JSON (`variables`, `spec_flags`, `live_spec_flags`, `textures`, SHA-256 hash). Supports `DXCB5_UBER_FLAG_{NAN_SAFE, GEC, UNITY_ADAPT, ROW_MAJOR, COL_MAJOR, IEEE_STRICT, SKIP_OPT}`. |
| `dxcb5_inspect_dxil` | Extracts specialization reflection metadata JSON directly from an existing Uber or pruned DXIL blob, including `live_spec_flags` (the subset of `DXCB5_SpecFlags` still read after Stage-1 folding). |
| `dxcb5_specialize_pair` | Specializes a `(VS, PS)` Uber DXIL pair from a JSON manifest (`constants`, `const_textures`, `defines`, `options`), outputting optional intermediate pruned DXIL, final SM5.0 DXBC, and stats JSON. |
| `dxcb5_specialize_stage` | Specializes a single-stage DXIL blob (including pre-pruned intermediate DXIL) into SM5.0 DXBC and optional stats JSON. |
| `dxcb5_free_blob` | Frees buffers returned in `dxcb5_blob` outputs. |
| Constant & Texture Folding | Substitutes manifest cbuffer constants, `const_textures` reads (`Sample`/`SampleLevel`/`SampleBias`/`SampleGrad`/`Load`, plus `GetDimensions` when `dims` is provided), and folds constant DXIL math intrinsics. |
| Cross-Stage DCE | Prunes dead PS inputs and corresponding VS outputs with pinned `ISGN`/`OSGN` register linkage while preserving system values. |
| Shared `$Globals` RDEF Compaction | Removes dead variables and packs live offsets inside `$Globals` with per-field dynamic array range analysis (dynamically indexed arrays keep only their own offset span rather than forcing the whole cbuffer live). `dxcb5_specialize_pair` computes a **single shared compact `$Globals` layout across VS + PS** (matching offsets for shared names, zero collisions in Unity D3D11) and preserves that shared layout in intermediate `.opt.dxil` for Stage-2 specialization. |

## Unity Integration

| Component / API | Description |
| :--- | :--- |
| `dxcb5_unity_hook.dll` | Editor-only x64 native plugin (built on MinHook v1.3.4) loaded in-process by `Unity.exe` via P/Invoke. Routes `UnityShaderCompiler.exe` plugin loading to a staged `Temp/DXCB5CgBatchPlugins` directory (mapping existing `PlaybackEngines` subdirectories via NTFS junctions without modifying Unity's installation directory), and on Unity `2022.3.22f1` hooks `ShaderCompilerCompileSnippet` for in-process RAM variant replay with reflection recording. |
| `dxcb5_cgbatch_plugin.dll` | Native `UnityShaderCompiler.exe` plugin loaded through Unity's `GetPluginInterface` plugin loader. Replaces `D3DCompilerWrapper::D3DCompile` in-place while preserving stock FXC for fallback, reflection, and stripping. Supports runtime enable/`--nan-safe` toggling on persistent workers, two-stage `dxcb5-spec:` stubs, per-shader manifest specialization, and direct SM5.0 DXBC compilation with disk deduplication. |
| `Dxcb5HookBridge` (C#) | Unity Editor bridge (`MoePus.Dxcb5.Editor.Dxcb5HookBridge`) exposing `TryInstall`, `SafeUninstall`, `SetEnabled`, `SetNanSafe`, `RegisterShaderManifest(assetPath, manifestJson, pairEntries)`, `ClearManifests`, `ClearCache`, and `GetStats()` (`RamCacheHits`, `IpcCompiles`, `WorkersHooked`, `SnippetHookActive`). |
| Stub Cache (`// dxcb5-spec:`) | Detects embedded `{pair_key, cache_dir}` stub directives in `dxcb5_cgbatch_plugin.dll`, masking defines via per-stage `vs_live_spec_flags` / `ps_live_spec_flags` (falling back to `spec_flags`) so collapsed variants share cached DXBC and RAM replay keys. |

## Backend Optimizations

| Optimization | Description |
| :--- | :--- |
| Register-Pressure Live-Range Shrinking (`OptPressureShrink`) | On high-pressure shaders, performs two-phase cheap scalar rematerialization, duplicate-source retargeting, intra-block sinking, and pressure-aware `LaneAllocator` coloring priority to reduce peak `dcl_temps`. |
| Automatic Cold Sinking & LICM | Automatically sinks pure computations into genuinely conditional cold branch arms (respecting loop depth and post-dominance) and hoists loop-invariant header ops to unconditional preheaders. |
