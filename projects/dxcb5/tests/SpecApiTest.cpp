// Distributed under the University of Illinois Open Source License.
#include "dxcb5/dxcb5_spec.h"
#include <windows.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include "SpecCacheBridge.h"
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const char kUberShader[] = R"HLSL(
#ifdef __DXCB5__
#define DXCB5_IMMUTABLE     [[dxcb5::immutable]]
#define DXCB5_SPEC_CONST    [[dxcb5::spec_const]]
#define DXCB5_SPEC_ZERO_ONE [[dxcb5::spec_zero_one]]
#define DXCB5_SPEC_IDENTITY [[dxcb5::spec_identity]]
#define DXCB5_SPEC_ST       [[dxcb5::spec_values(1.0, 1.0, 0.0, 0.0)]]
#define DXCB5_SPEC_VALS     [[dxcb5::spec_values(0.0, 0.5, 1.0)]]
#define DXCB5_DYNAMIC       [[dxcb5::dynamic]]
#else
#error "__DXCB5__ macro must be predefined"
#endif

cbuffer UnityPerDraw : register(b1) {
  float4 unity_ObjectToWorld0;
  float4 unity_WorldScale;
};

cbuffer DXCB5_SpecFlags {
  DXCB5_IMMUTABLE uint SHADOWS_SCREEN;
  DXCB5_IMMUTABLE uint FOG_EXP2;
};

DXCB5_IMMUTABLE float _ToneLift;
DXCB5_IMMUTABLE uint _UseRim;
DXCB5_SPEC_CONST float _HueAngle;
DXCB5_SPEC_ZERO_ONE float _RimStrength;
DXCB5_SPEC_ST float4 _MainTex_ST;
DXCB5_SPEC_IDENTITY float4 _TintScale;
DXCB5_SPEC_VALS float _MixStep;
DXCB5_DYNAMIC float _UdonLightVolumeEnabled;
float4 _Color;

Texture2D _MainTex;
Texture2D _RimMask;
Texture2D _BumpMap;
DXCB5_DYNAMIC Texture2D _AudioLinkTex;
SamplerState sampler_MainTex;

struct VSIn {
  float4 pos : POSITION;
  float2 uv  : TEXCOORD0;
  float3 nrm : NORMAL;
};

struct VSOut {
  float4 pos     : SV_Position;
  float2 uv      : TEXCOORD0;
  float3 rimNrm  : TEXCOORD1;
  float  fogVal  : TEXCOORD2;
};

VSOut VSMain(VSIn i) {
  VSOut o;
  o.pos = i.pos + unity_ObjectToWorld0;
  o.uv = i.uv * _MainTex_ST.xy + _MainTex_ST.zw;
  o.rimNrm = normalize(i.nrm * unity_WorldScale.xyz + float3(0.1, 0.2, 0.3));
  o.fogVal = FOG_EXP2 != 0 ? (i.pos.z * 0.25) : 0.0;
  return o;
}

float4 PSMain(VSOut i) : SV_Target {
  float4 base = _MainTex.Sample(sampler_MainTex, i.uv) * _Color * _TintScale;
  float mask = _RimMask.Sample(sampler_MainTex, i.uv).r;
  float rad = _HueAngle * 0.0174532925;
  float lift = sqrt(_ToneLift) + cos(rad);
  float3 col = base.rgb * mask * lift;
  if (_UseRim != 0) {
    float3 bump = _BumpMap.Sample(sampler_MainTex, i.uv).xyz;
    col += saturate(dot(i.rimNrm, bump)) * _RimStrength;
  }
  if (SHADOWS_SCREEN != 0) {
    col *= 0.75;
  }
  if (FOG_EXP2 != 0) {
    col = lerp(col, float3(0.2, 0.3, 0.4), saturate(i.fogVal));
  }
  if (_UdonLightVolumeEnabled > 0.5) {
    col += _AudioLinkTex.Sample(sampler_MainTex, i.uv).rgb * _MixStep;
  }
  return float4(col, base.a);
}
)HLSL";

std::string DisasmDxbc(const void *Data, size_t Size) {
  ComPtr<ID3DBlob> Dis;
  if (FAILED(D3DDisassemble(Data, Size, 0, nullptr, &Dis)) || !Dis)
    return {};
  return std::string(static_cast<const char *>(Dis->GetBufferPointer()),
                     Dis->GetBufferSize());
}

bool WriteBytes(const std::string &Path, const void *Data, size_t Size) {
  std::ofstream F(Path, std::ios::binary);
  if (!F)
    return false;
  F.write(static_cast<const char *>(Data), static_cast<std::streamsize>(Size));
  return F.good();
}

void EnsureDir(const std::string &Path) {
  CreateDirectoryA(Path.c_str(), nullptr);
}

bool CheckContains(const std::string &Haystack, const char *Needle,
                   const char *Context) {
  if (Haystack.find(Needle) == std::string::npos) {
    std::cerr << Context << ": expected '" << Needle << "' in:\n"
              << Haystack << '\n';
    return false;
  }
  return true;
}

bool CheckNotContains(const std::string &Haystack, const char *Needle,
                      const char *Context) {
  if (Haystack.find(Needle) != std::string::npos) {
    std::cerr << Context << ": unexpected '" << Needle << "' in:\n"
              << Haystack << '\n';
    return false;
  }
  return true;
}

struct OwnedBlob {
  dxcb5_blob B{};
  OwnedBlob() = default;
  ~OwnedBlob() { dxcb5_free_blob(&B); }
  OwnedBlob(const OwnedBlob &) = delete;
  OwnedBlob &operator=(const OwnedBlob &) = delete;
  std::string Str() const {
    return B.data ? std::string(static_cast<const char *>(B.data), B.size) : std::string{};
  }
};

bool CompileUber(const char *Src, const char *Entry, const char *Target,
                 const char *Name, OwnedBlob &OutDxil,
                 OwnedBlob *OutRefl = nullptr, const char *Hv = "2021",
                 uint32_t Flags = 0) {
  dxcb5_uber_options Opts{};
  Opts.entry = Entry;
  Opts.target = Target;
  Opts.hlsl_version = Hv;
  Opts.source_name = Name;
  Opts.flags = Flags;
  OwnedBlob Err;
  if (dxcb5_compile_uber_dxil(Src, std::strlen(Src), &Opts, &OutDxil.B,
                              OutRefl ? &OutRefl->B : nullptr, &Err.B) != 0) {
    std::cerr << Name << " uber compile failed: " << Err.Str() << '\n';
    return false;
  }
  return true;
}

bool SpecializeStageAsm(const OwnedBlob &Dxil, const char *Manifest,
                        const char *Ctx, std::string &OutAsm,
                        OwnedBlob *OutDxbc = nullptr) {
  OwnedBlob LocalDxbc, Err;
  OwnedBlob &Dst = OutDxbc ? *OutDxbc : LocalDxbc;
  if (dxcb5_specialize_stage(Dxil.B.data, Dxil.B.size, Manifest,
                             std::strlen(Manifest), &Dst.B, nullptr,
                             &Err.B) != 0) {
    std::cerr << Ctx << " specialize failed: " << Err.Str() << '\n';
    return false;
  }
  OutAsm = DisasmDxbc(Dst.B.data, Dst.B.size);
  return true;
}

bool CompileAndSpecializePsAsm(const char *Src, const char *Name,
                               const char *Manifest, std::string &OutAsm,
                               const char *Hv = "2021", uint32_t Flags = 0,
                               OwnedBlob *OutRefl = nullptr,
                               OwnedBlob *OutDxbc = nullptr) {
  OwnedBlob Dxil;
  return CompileUber(Src, "PSMain", "ps_5_0", Name, Dxil, OutRefl, Hv, Flags) &&
         SpecializeStageAsm(Dxil, Manifest, Name, OutAsm, OutDxbc);
}

struct MockSpecD3DCompiler {
  int FallbackCalls = 0;
  virtual bool IsValid() { return true; }
  virtual void SetUseRealDLL(bool) {}
  virtual bool GetUseRealDLL() { return false; }
  virtual HRESULT D3DCompile(const void *, unsigned long, const char *,
                             const D3D_SHADER_MACRO *, ID3DInclude *,
                             const char *, const char *, UINT, UINT,
                             ID3DBlob **ppCode, ID3DBlob **) {
    ++FallbackCalls;
    if (ppCode)
      *ppCode = nullptr;
    return S_FALSE;
  }
  virtual HRESULT D3DAssemble() { return S_OK; }
  virtual HRESULT D3DDisassemble() { return S_OK; }
  virtual HRESULT D3DPreprocess() { return S_OK; }
  virtual HRESULT D3DReflect() { return S_OK; }
  virtual HRESULT D3DStripShader() { return S_OK; }
  virtual HRESULT D3DGetBlobPart() { return S_OK; }
};

struct MockSpecDispatcher {
  MockSpecD3DCompiler *D3D = nullptr;
  virtual void *GetD3DCompiler() { return D3D; }
};

bool TestCgBatchSpecStubCache(const std::string &PluginDll,
                              const std::string &DxcDll,
                              const std::string &WorkDir,
                              const dxcb5_blob &PsOptDxil,
                              const dxcb5_blob &PsDxbc,
                              const std::string &PsReflJson) {
  const std::string SessionDir = WorkDir + "\\session";
  const std::string CacheDir = WorkDir + "\\cache";
  const std::string PairDir = CacheDir + "\\testpair123\\desktop";
  EnsureDir(WorkDir);
  EnsureDir(SessionDir);
  EnsureDir(CacheDir);
  EnsureDir(CacheDir + "\\testpair123");
  EnsureDir(PairDir);

  const std::string InitState = "seq=1\nenabled=1\nnan_safe=0\n";
  WriteBytes(SessionDir + "\\hook_state.txt", InitState.data(), InitState.size());
  WriteBytes(PairDir + "\\meta.json", PsReflJson.data(), PsReflJson.size());
  WriteBytes(PairDir + "\\ps.opt.dxil", PsOptDxil.data, PsOptDxil.size);

  // Pre-seed cache for FOG_EXP2=0, SHADOWS_SCREEN=1 using SpecCacheBridge::HashDefinesHex.
  auto HashKV = [](uint32_t Fog, uint32_t Shad) {
    const std::vector<std::pair<std::string, uint32_t>> Items = {
        {"FOG_EXP2", Fog}, {"SHADOWS_SCREEN", Shad}};
    const std::wstring W = dxcb5_proxy::HashDefinesHex(Items);
    return std::string(W.begin(), W.end());
  };
  WriteBytes(PairDir + "\\ps_" + HashKV(0, 1) + ".dxbc", PsDxbc.data, PsDxbc.size);
  DeleteFileA((PairDir + "\\ps_" + HashKV(1, 0) + ".dxbc").c_str());

  SetEnvironmentVariableA("DXCB5_DXCOMPILER_PATH", DxcDll.c_str());
  SetEnvironmentVariableA("DXCB5_HOOK_SESSION_DIR", SessionDir.c_str());
  SetEnvironmentVariableA("DXCB5_HOOK_ENABLED", "1");
  SetEnvironmentVariableA("DXCB5_NAN_SAFE", "0");
  SetEnvironmentVariableA("DXCB5_UNITY_PROJECT_DIR", WorkDir.c_str());

  HMODULE Mod = LoadLibraryA(PluginDll.c_str());
  if (!Mod) {
    std::cerr << "Failed to load dxcb5_cgbatch_plugin.dll for spec cache test\n";
    return false;
  }
  using PFN_GetPluginInterface = void *(*)(int, void *);
  auto GetIf = reinterpret_cast<PFN_GetPluginInterface>(
      GetProcAddress(Mod, "GetPluginInterface"));
  if (!GetIf) {
    std::cerr << "GetPluginInterface export missing\n";
    return false;
  }
  MockSpecD3DCompiler MockD3D;
  MockSpecDispatcher MockDisp;
  MockDisp.D3D = &MockD3D;
  GetIf(2, &MockDisp);
  auto *D3D = static_cast<MockSpecD3DCompiler *>(MockDisp.GetD3DCompiler());

  auto CompileFn = [D3D](const void *Src, size_t Len, const char *Name,
                         const D3D_SHADER_MACRO *Defs, ID3DInclude *Inc,
                         const char *Entry, const char *Target, UINT F1,
                         UINT F2, ID3DBlob **OutCode, ID3DBlob **OutErr) {
    return D3D->D3DCompile(Src, static_cast<unsigned long>(Len), Name, Defs,
                           Inc, Entry, Target, F1, F2, OutCode, OutErr);
  };

  const std::string Stub =
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"testpair123\",\"cache_dir\":\"cache\"}\n"
      "float4 PSMain() : SV_Target { return 0; }\n";

  // 1. Cache HIT (SHADOWS_SCREEN=1, FOG_EXP2 unset -> 0)
  const D3D_SHADER_MACRO HitDefs[] = {{"SHADOWS_SCREEN", "1"}, {nullptr, nullptr}};
  ComPtr<ID3DBlob> HitBlob, ErrBlob;
  if (FAILED(CompileFn(Stub.data(), Stub.size(), "stub.hlsl", HitDefs, nullptr,
                       "PSMain", "ps_5_0", 0, 0, &HitBlob, &ErrBlob)) ||
      !HitBlob || HitBlob->GetBufferSize() != PsDxbc.size ||
      std::memcmp(HitBlob->GetBufferPointer(), PsDxbc.data, PsDxbc.size) != 0) {
    std::cerr << "CgBatch SPEC_HIT did not return pre-seeded DXBC\n";
    return false;
  }

  // 2. Cold Cache MISS (FOG_EXP2=1, SHADOWS_SCREEN=0 -> specializes ps.opt.dxil!)
  const D3D_SHADER_MACRO MissDefs[] = {{"FOG_EXP2", "1"}, {nullptr, nullptr}};
  ComPtr<ID3DBlob> MissBlob;
  if (FAILED(CompileFn(Stub.data(), Stub.size(), "stub.hlsl", MissDefs, nullptr,
                       "PSMain", "ps_5_0", 0, 0, &MissBlob, &ErrBlob)) ||
      !MissBlob || !MissBlob->GetBufferSize()) {
    std::cerr << "CgBatch SPEC_MISS fallback failed\n";
    return false;
  }
  const std::string MissAsm =
      DisasmDxbc(MissBlob->GetBufferPointer(), MissBlob->GetBufferSize());
  if (!CheckContains(MissAsm, "TEXCOORD                 2", "CgBatch SPEC_MISS FOG_EXP2=1") ||
      !CheckNotContains(MissAsm, "TEXCOORD                 1", "CgBatch SPEC_MISS stripped rim"))
    return false;

  // Verify SPEC_MISS persisted the newly specialized DXBC to cache.
  const std::string ExpectedCached = PairDir + "\\ps_" + HashKV(1, 0) + ".dxbc";
  if (GetFileAttributesA(ExpectedCached.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::cerr << "CgBatch SPEC_MISS did not persist cached DXBC: "
              << ExpectedCached << '\n';
    return false;
  }

  // 3. Toggle nan_safe=1 on a dedicated stub whose bytecode differs under NaN-safe mode.
  const char kNanHlsl[] = R"HLSL(
float4 PSMain(float4 v : TEXCOORD0) : SV_Target {
  return max(min(v, 1.0f), 0.0f);
}
)HLSL";
  dxcb5_uber_options NanOpts{};
  NanOpts.entry = "PSMain";
  NanOpts.target = "ps_5_0";
  NanOpts.hlsl_version = "2021";
  NanOpts.source_name = "nan_cache.hlsl";
  dxcb5_blob NanDxil{}, NanErr{};
  if (dxcb5_compile_uber_dxil(kNanHlsl, sizeof(kNanHlsl) - 1, &NanOpts,
                              &NanDxil, nullptr, &NanErr) != 0) {
    dxcb5_free_blob(&NanErr);
    return false;
  }
  const std::string NanPairDir = CacheDir + "\\nanpair\\desktop";
  EnsureDir(CacheDir + "\\nanpair");
  EnsureDir(NanPairDir);
  const std::wstring EmptyHashW = dxcb5_proxy::HashDefinesHex({});
  const std::string EmptyHash(EmptyHashW.begin(), EmptyHashW.end());
  DeleteFileA((NanPairDir + "\\ps_" + EmptyHash + ".dxbc").c_str());
  DeleteFileA((NanPairDir + "\\ps_" + EmptyHash + "_nansafe.dxbc").c_str());
  const std::string EmptyMeta = "{\"spec_flags\":[]}";
  WriteBytes(NanPairDir + "\\meta.json", EmptyMeta.data(), EmptyMeta.size());
  WriteBytes(NanPairDir + "\\ps.opt.dxil", NanDxil.data, NanDxil.size);
  dxcb5_free_blob(&NanDxil);
  dxcb5_free_blob(&NanErr);

  const std::string NanStub =
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"nanpair\",\"cache_dir\":\"cache\"}\n"
      "float4 PSMain() : SV_Target { return 0; }\n";
  const D3D_SHADER_MACRO NoDefs[] = {{nullptr, nullptr}};
  ComPtr<ID3DBlob> FastBlob, SafeBlob;
  if (FAILED(CompileFn(NanStub.data(), NanStub.size(), "nan.hlsl", NoDefs,
                       nullptr, "PSMain", "ps_5_0", 0, 0, &FastBlob, &ErrBlob)) ||
      !FastBlob) {
    std::cerr << "CgBatch nan_safe=0 compile failed\n";
    return false;
  }
  const std::string FastAsm =
      DisasmDxbc(FastBlob->GetBufferPointer(), FastBlob->GetBufferSize());
  if (!CheckContains(FastAsm, "mov_sat", "nan_safe=0 folds max(min(v,1),0) to mov_sat"))
    return false;

  // Switch hook_state.txt to nan_safe=1 -> MUST NOT hit old fast cache!
  const std::string NanState = "seq=2\nenabled=1\nnan_safe=1\n";
  WriteBytes(SessionDir + "\\hook_state.txt", NanState.data(), NanState.size());
  if (FAILED(CompileFn(NanStub.data(), NanStub.size(), "nan.hlsl", NoDefs,
                       nullptr, "PSMain", "ps_5_0", 0, 0, &SafeBlob, &ErrBlob)) ||
      !SafeBlob) {
    std::cerr << "CgBatch nan_safe=1 compile failed\n";
    return false;
  }
  const std::string SafeAsm =
      DisasmDxbc(SafeBlob->GetBufferPointer(), SafeBlob->GetBufferSize());
  if (!CheckNotContains(SafeAsm, "mov_sat", "nan_safe=1 must NOT hit stale non-nan-safe cache") ||
      !CheckContains(SafeAsm, "max ", "nan_safe=1 preserves max against NaN"))
    return false;

  // 4. Unity-style in-source #define (pDefines==nullptr), SPS vs SPS-I isolation,
  //    #line preprocessed-proof directive, and _dxcb5_def_ sentinel.
  const std::string RestoreState = "seq=3\nenabled=1\nnan_safe=0\n";
  WriteBytes(SessionDir + "\\hook_state.txt", RestoreState.data(), RestoreState.size());
  const std::string SpsDir = CacheDir + "\\testpair123\\sps";
  const std::string SpsiDir = CacheDir + "\\testpair123\\spsi";
  EnsureDir(SpsDir);
  EnsureDir(SpsiDir);
  WriteBytes(SpsDir + "\\meta.json", PsReflJson.data(), PsReflJson.size());
  WriteBytes(SpsiDir + "\\meta.json", PsReflJson.data(), PsReflJson.size());
  // Seed distinct valid 32-byte DXBC headers in sps/ vs spsi/ for SHADOWS_SCREEN=1, FOG_EXP2=0!
  auto MakeFakeDxbcContainer = [](const char *Tag16) {
    std::string Buf(32, '\0');
    std::memcpy(&Buf[0], "DXBC", 4);
    std::memcpy(&Buf[4], Tag16, std::min<size_t>(16, std::strlen(Tag16)));
    uint32_t Ver = 1u, TotalSize = 32u, Chunks = 0u;
    std::memcpy(&Buf[20], &Ver, 4);
    std::memcpy(&Buf[24], &TotalSize, 4);
    std::memcpy(&Buf[28], &Chunks, 4);
    return Buf;
  };
  const std::string SpsTag = MakeFakeDxbcContainer("DXBC_SPS_PAYLD!");
  const std::string SpsiTag = MakeFakeDxbcContainer("DXBC_SPSI_PAYLD");
  WriteBytes(SpsDir + "\\ps_" + HashKV(0, 1) + ".dxbc", SpsTag.data(), SpsTag.size());
  WriteBytes(SpsiDir + "\\ps_" + HashKV(0, 1) + ".dxbc", SpsiTag.data(), SpsiTag.size());

  // Unity 2022.3 style: pDefines == nullptr, variant keywords as #define in pSrcData!
  const std::string SpsStub =
      "#define UNITY_SINGLE_PASS_STEREO 1\n"
      "#define SHADOWS_SCREEN 1\n"
      "#ifdef FOG_EXP2\n"
      "static const int _dxcb5_def_FOG_EXP2 = 1;\n" // inside #ifdef -> MUST NOT activate!
      "#endif\n"
      "/*\n"
      "#define FOG_EXP2 1\n" // inside multi-line block comment -> MUST NOT activate!
      "*/\n"
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"testpair123\",\"cache_dir\":\"cache\"}\n"
      "float4 PSMain() : SV_Target { return 0; }\n";
  ComPtr<ID3DBlob> SpsBlob, SpsiBlob;
  if (FAILED(CompileFn(SpsStub.data(), SpsStub.size(), "sps.hlsl", nullptr,
                       nullptr, "PSMain", "ps_5_0", 0, 0, &SpsBlob, &ErrBlob)) ||
      !SpsBlob || SpsBlob->GetBufferSize() != SpsTag.size() ||
      std::memcmp(SpsBlob->GetBufferPointer(), SpsTag.data(), SpsTag.size()) != 0) {
    std::cerr << "SPS in-source #define cache lookup failed\n";
    return false;
  }

  // Preprocessed-proof style: #line directive + top-level _dxcb5_def_ sentinels, no // comment!
  const std::string SpsiStub =
      "#line 1 \"dxcb5-spec:{\\\"v\\\":1,\\\"pair_key\\\":\\\"testpair123\\\",\\\"cache_dir\\\":\\\"cache\\\"}\"\n"
      "static const int _dxcb5_def_STEREO_INSTANCING_ON = 1;\n"
      "static const int _dxcb5_def_SHADOWS_SCREEN = 1;\n"
      "float4 PSMain() : SV_Target { return 0; }\n";
  if (FAILED(CompileFn(SpsiStub.data(), SpsiStub.size(), "spsi.hlsl", nullptr,
                       nullptr, "PSMain", "ps_5_0", 0, 0, &SpsiBlob, &ErrBlob)) ||
      !SpsiBlob || SpsiBlob->GetBufferSize() != SpsiTag.size() ||
      std::memcmp(SpsiBlob->GetBufferPointer(), SpsiTag.data(), SpsiTag.size()) != 0) {
    std::cerr << "SPS-I preprocessed-proof cache lookup failed\n";
    return false;
  }

  // 5. Reject cache_dir path traversal attempts (must compile fallback stub, not return SpsTag).
  const std::string BadStub =
      "#define UNITY_SINGLE_PASS_STEREO 1\n"
      "#define SHADOWS_SCREEN 1\n"
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"testpair123\",\"cache_dir\":\"../cache\"}\n"
      "float4 PSMain() : SV_Target { return float4(0.25, 0.5, 0.75, 1.0); }\n";
  ComPtr<ID3DBlob> BadBlob;
  if (FAILED(CompileFn(BadStub.data(), BadStub.size(), "bad.hlsl", nullptr,
                       nullptr, "PSMain", "ps_5_0", 0, 0, &BadBlob, &ErrBlob)) ||
      !BadBlob ||
      (BadBlob->GetBufferSize() == SpsTag.size() &&
       std::memcmp(BadBlob->GetBufferPointer(), SpsTag.data(), SpsTag.size()) == 0)) {
    std::cerr << "Path traversal cache_dir was not rejected\n";
    return false;
  }
  const std::string BadAsm =
      DisasmDxbc(BadBlob->GetBufferPointer(), BadBlob->GetBufferSize());
  if (!CheckContains(BadAsm, "0.250000", "Path traversal compiled fallback body"))
    return false;
  return true;
}

bool TestArrayMatrixAndTextureSafety() {
  // A. Arrays, 2D arrays, row/column-major matrices, and arrays of matrices.
  {
    const char kCbHlsl[] = R"HLSL(
cbuffer $Globals {
  [[dxcb5::immutable]] float _Arr1[3];
  [[dxcb5::immutable]] float2 _Arr2[2];
  [[dxcb5::immutable]] float3 _Arr3[2];
  [[dxcb5::immutable]] column_major float3x3 _Mat33;
  [[dxcb5::immutable]] row_major float2x3 _RMat23;
  [[dxcb5::immutable]] column_major float2x2 _MatArr[2];
  [[dxcb5::immutable]] float _A2D[2][3];
  float _Live;
};
float4 PSMain() : SV_Target {
  float r = _Arr1[2] + _Arr2[1].y;      // 30.0 + 4.0 = 34.0
  float g = _Arr3[1].z + _Mat33[2][1];  // 60.0 + 15.0 = 75.0
  float b = _RMat23[1][2] + _MatArr[1][1][0]; // 26.0 + 106.0 = 132.0
  float a = _A2D[1][2] + _Live;         // 55.0 + _Live
  return float4(r, g, b, a);
}
)HLSL";
    const char kManifest[] = R"JSON({
      "schema_version": 1,
      "constants": {
        "_Arr1":   { "type": "f32", "v": [10, 20, 30] },
        "_Arr2":   { "type": "f32", "v": [1, 2, 3, 4] },
        "_Arr3":   { "type": "f32", "v": [10, 20, 30, 40, 50, 60] },
        "_Mat33":  { "type": "f32", "v": [10, 11, 12, 13, 14, 15, 16, 17, 18] },
        "_RMat23": { "type": "f32", "v": [21, 22, 23, 24, 25, 26] },
        "_MatArr": { "type": "f32", "v": [101, 102, 103, 104, 105, 106, 107, 108] },
        "_A2D":    { "type": "f32", "v": [50, 51, 52, 53, 54, 55] }
      },
      "options": { "compact_globals": true }
    })JSON";
    OwnedBlob Refl;
    std::string Asm;
    if (!CompileAndSpecializePsAsm(kCbHlsl, "cb_layout.hlsl", kManifest, Asm,
                                   "2021", 0, &Refl))
      return false;
    const std::string ReflStr = Refl.Str();
    if (!CheckContains(ReflStr, "\"name\":\"_Arr1\",\"cbuffer\":\"$Globals\",\"type\":\"f32\",\"components\":3", "Refl _Arr1 comps") ||
        !CheckContains(ReflStr, "\"name\":\"_MatArr\",\"cbuffer\":\"$Globals\",\"type\":\"f32\",\"components\":8", "Refl _MatArr comps") ||
        !CheckContains(Asm, "34.000000", "Array folded r=34") ||
        !CheckContains(Asm, "75.000000", "Matrix folded g=75") ||
        !CheckContains(Asm, "132.000000", "RowMat+MatArr folded b=132") ||
        !CheckContains(Asm, "55.000000", "2D array folded a=55") ||
        !CheckNotContains(Asm, "_Mat33", "Folded _Mat33 compacted out"))
      return false;
  }

  // B. Texture mip GetDimensions & OOB/dynamic TextureLoad safety.
  {
    const char kTexHlsl[] = R"HLSL(
Texture2D _TexMip : register(t0);
Texture2D _TexLvls : register(t1);
Texture2D _TexStatic : register(t2);
Texture2D _TexDyn : register(t3);
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target {
  uint w1, h1, l1;
  _TexMip.GetDimensions(1, w1, h1, l1);
  uint w2, h2, l2;
  _TexLvls.GetDimensions(0, w2, h2, l2);
  uint ws, hs;
  _TexStatic.GetDimensions(ws, hs);
  float inB = _TexStatic.Load(int3(1, 2, 0)).x;  // 0.75
  float oob = _TexStatic.Load(int3(9, 2, 0)).x;  // MUST be 0.0 (OOB!)
  float dyn = _TexDyn.Load(int3(int(uv.x), int(uv.y), 0)).x;
  return float4(float(w1 + l1), float(w2 + l2), inB + oob + float(ws + hs), dyn);
}
)HLSL";
    const char kManifest[] = R"JSON({
      "schema_version": 1,
      "const_textures": {
        "_TexMip":    { "v": [0.5, 0.5, 0.5, 1.0], "dims": [4, 4] },
        "_TexLvls":   { "v": [0.5, 0.5, 0.5, 1.0], "dims": [4, 4] },
        "_TexStatic": { "v": [0.75, 0.25, 0.0, 1.0], "dims": [4, 4] },
        "_TexDyn":    { "v": [0.5, 0.5, 0.5, 1.0], "dims": [4, 4] }
      }
    })JSON";
    std::string Asm;
    if (!CompileAndSpecializePsAsm(kTexHlsl, "tex_safety.hlsl", kManifest, Asm))
      return false;
    // _TexStatic folds: inB(0.75) + oob(0.0) + ws(4) + hs(4) = 8.75, and _TexStatic stripped!
    // _TexMip, _TexLvls keep resinfo; _TexDyn keeps ld!
    if (!CheckContains(Asm, "_TexMip", "_TexMip kept for mip=1 query") ||
        !CheckContains(Asm, "_TexLvls", "_TexLvls kept for numLevels query") ||
        !CheckContains(Asm, "_TexDyn", "_TexDyn kept for dynamic Load") ||
        !CheckNotContains(Asm, "_TexStatic", "_TexStatic fully folded and stripped") ||
        !CheckContains(Asm, "8.750000", "OOB Load folded to 0.0 (0.75+0+4+4=8.75)") ||
        !CheckContains(Asm, "resinfo", "resinfo preserved for mip/numLevels") ||
        !CheckContains(Asm, "\nld ", "ld preserved for dynamic Load"))
      return false;
  }

  // C. DXCB5_UBER_FLAG_UNITY_ADAPT compiles legacy Unity sampler2D/tex2D shaders.
  {
    const char kLegacyHlsl[] = R"HLSL(
sampler2D _MainTex;
float4 _Tint;
float4 PSMain(float2 uv : TEXCOORD0) : COLOR {
  return tex2D(_MainTex, uv) * _Tint;
}
)HLSL";
    const char kManifest[] = R"JSON({
      "schema_version": 1,
      "const_textures": { "_MainTex": { "v": [0.5, 0.5, 0.5, 1.0] } },
      "constants": { "_Tint": { "type": "f32", "v": [2.0, 4.0, 6.0, 1.0] } }
    })JSON";
    std::string Asm;
    if (!CompileAndSpecializePsAsm(kLegacyHlsl, "legacy_unity.hlsl", kManifest,
                                   Asm, "2018", DXCB5_UBER_FLAG_UNITY_ADAPT))
      return false;
    if (!CheckContains(Asm, "1.000000", "UNITY_ADAPT folded r=1") ||
        !CheckContains(Asm, "2.000000", "UNITY_ADAPT folded g=2") ||
        !CheckContains(Asm, "3.000000", "UNITY_ADAPT folded b=3") ||
        !CheckNotContains(Asm, "_MainTex", "UNITY_ADAPT stripped _MainTex"))
      return false;
  }
  return true;
}

} // namespace

bool TestPhase2Annotations() {
  const char kPhase2Hlsl[] = R"HLSL(
Texture2D _Tex : register(t0);
SamplerState _Samp : register(s0);

cbuffer $Globals {
  [[dxcb5::uniform]] float _UniformGate;
  float _DynamicCond;
};

[[dxcb5::fast_math]]
float FastCbrtFn(float x) {
  return pow(abs(x), 1.0f / 3.0f);
}

float4 PSMain(float4 uv : TEXCOORD0) : SV_Target {
  float a = FastCbrtFn(uv.x);
  float b = 0.0f;
  [[dxcb5::fast_math]] {
    b = pow(abs(uv.y), 0.125f);
  }
  float c = pow(abs(uv.z), 1.0f / 3.0f);

  float d = uv.w;
  [[dxcb5::strict_branch]]
  if (_DynamicCond > 0.5f) {
    d = d + 1.0f;
  }

  float4 t = float4(0, 0, 0, 0);
  [[dxcb5::uniform]]
  if (_UniformGate > 0.5f) {
    t = _Tex.Sample(_Samp, uv.xy) + ddx(uv);
  }
  return float4(a, b, c, d) + t;
}
)HLSL";

  const char kEmptyManifest[] = R"JSON({"schema_version":1})JSON";
  std::string Asm;
  if (!CompileAndSpecializePsAsm(kPhase2Hlsl, "phase2.hlsl", kEmptyManifest, Asm))
    return false;

  auto CountSubstr = [&](const char *Needle) {
    size_t Cnt = 0, Pos = 0;
    while ((Pos = Asm.find(Needle, Pos)) != std::string::npos) {
      ++Cnt;
      Pos += std::strlen(Needle);
    }
    return Cnt;
  };
  const size_t UmulCount = CountSubstr("\numul ");
  const size_t SqrtCount = CountSubstr("\nsqrt ");
  const size_t LogCount = CountSubstr("\nlog ");
  const size_t ExpCount = CountSubstr("\nexp ");
  const size_t IfCount = CountSubstr("\nif_nz ") + CountSubstr("\nif_z ");
  if (UmulCount != 1 || SqrtCount != 3 || LogCount != 1 || ExpCount != 1 ||
      IfCount < 2) {
    std::cerr << "Phase2 asm check failed: umul=" << UmulCount
              << " sqrt=" << SqrtCount << " log=" << LogCount
              << " exp=" << ExpCount << " if=" << IfCount << "\n"
              << Asm << '\n';
    return false;
  }

  // Audit #24 & #16: Verify [[dxcb5::uniform]] contract sensitivity end-to-end:
  // 1) A uniform-gated Sample+ddx sinks inside if_nz/if_z;
  // 2) An identical non-uniform-gated Sample+ddx does NOT sink inside if;
  // 3) Uniform globals combined via `select` (ternary) propagate uniformity to
  //    the branch, while mixing a divergent condition does NOT.
  auto CompileSingleStageAsm = [&](const char *Src, const char *Name,
                                   std::string &OutAsm) -> bool {
    return CompileAndSpecializePsAsm(Src, Name, kEmptyManifest, OutAsm);
  };
  auto IsSampleInsideIf = [](const std::string &Text) -> bool {
    const size_t IfNz = Text.find("if_nz ");
    const size_t IfZ = Text.find("if_z ");
    const size_t FirstIf = (IfNz < IfZ) ? IfNz : IfZ;
    const size_t FirstSample = Text.find("sample ");
    return FirstIf != std::string::npos && FirstSample != std::string::npos &&
           FirstIf < FirstSample;
  };

  {
    const char kUniformDirect[] = R"HLSL(
Texture2D _Tex : register(t0);
SamplerState _Samp : register(s0);
cbuffer $Globals { [[dxcb5::uniform]] float _Gate; };
float4 PSMain(float4 uv : TEXCOORD0) : SV_Target {
  float4 s = _Tex.Sample(_Samp, uv.xy);
  float4 t = float4(0, 0, 0, 0);
  [[dxcb5::strict_branch]] if (_Gate > 0.5f) { t = s + float4(1, 2, 3, 4); }
  return t;
}
)HLSL";
    const char kNonUniformDirect[] = R"HLSL(
Texture2D _Tex : register(t0);
SamplerState _Samp : register(s0);
cbuffer $Globals { float _Gate; };
float4 PSMain(float4 uv : TEXCOORD0) : SV_Target {
  float4 s = _Tex.Sample(_Samp, uv.xy);
  float4 t = float4(0, 0, 0, 0);
  [[dxcb5::strict_branch]] if (_Gate > 0.5f) { t = s + float4(1, 2, 3, 4); }
  return t;
}
)HLSL";
    const char kUniformSelect[] = R"HLSL(
Texture2D _Tex : register(t0);
SamplerState _Samp : register(s0);
cbuffer $Globals {
  [[dxcb5::uniform]] float _Pick;
  [[dxcb5::uniform]] float _GateA;
  [[dxcb5::uniform]] float _GateB;
};
float4 PSMain(float4 uv : TEXCOORD0) : SV_Target {
  float4 s = _Tex.Sample(_Samp, uv.xy);
  float g = (_Pick > 0.5f) ? _GateA : _GateB;
  float4 t = float4(0, 0, 0, 0);
  [[dxcb5::strict_branch]] if (g > 0.5f) { t = s + float4(1, 2, 3, 4); }
  return t;
}
)HLSL";
    const char kDivergentSelect[] = R"HLSL(
Texture2D _Tex : register(t0);
SamplerState _Samp : register(s0);
cbuffer $Globals {
  [[dxcb5::uniform]] float _GateA;
  [[dxcb5::uniform]] float _GateB;
};
float4 PSMain(float4 uv : TEXCOORD0) : SV_Target {
  float4 s = _Tex.Sample(_Samp, uv.xy);
  float g = (uv.z > 0.5f) ? _GateA : _GateB;
  float4 t = float4(0, 0, 0, 0);
  [[dxcb5::strict_branch]] if (g > 0.5f) { t = s + float4(1, 2, 3, 4); }
  return t;
}
)HLSL";
    std::string AsmU, AsmNU, AsmUSel, AsmDSel;
    if (!CompileSingleStageAsm(kUniformDirect, "uniform_direct.hlsl", AsmU) ||
        !CompileSingleStageAsm(kNonUniformDirect, "nonuniform_direct.hlsl", AsmNU) ||
        !CompileSingleStageAsm(kUniformSelect, "uniform_select.hlsl", AsmUSel) ||
        !CompileSingleStageAsm(kDivergentSelect, "divergent_select.hlsl", AsmDSel))
      return false;
    if (!IsSampleInsideIf(AsmU)) {
      std::cerr << "Audit #24 failed: [[dxcb5::uniform]] global did not sink Sample into if\n"
                << AsmU << '\n';
      return false;
    }
    if (IsSampleInsideIf(AsmNU)) {
      std::cerr << "Audit #24 failed: non-uniform branch unexpectedly sunk Sample into if\n"
                << AsmNU << '\n';
      return false;
    }
    if (!IsSampleInsideIf(AsmUSel)) {
      std::cerr << "Audit #16 failed: [[dxcb5::uniform]] through select did not propagate uniformity\n"
                << AsmUSel << '\n';
      return false;
    }
    if (IsSampleInsideIf(AsmDSel)) {
      std::cerr << "Audit #16 soundness failed: divergent select condition wrongly marked uniform\n"
                << AsmDSel << '\n';
      return false;
    }

    // Audit #18: [[dxcb5::strict_branch]] whose condition specializes to a
    // constant in Stage-2 must have its strict_branch/controlflow hint stripped
    // so the dead branch folds away completely.
    const char kStrictConstHlsl[] = R"HLSL(
cbuffer $Globals { [[dxcb5::immutable]] float _Flag; };
float4 PSMain(float4 uv : TEXCOORD0) : SV_Target {
  float4 r = uv;
  [[dxcb5::strict_branch]]
  if (_Flag > 0.5f) {
    r = r * 2.0f + float4(1, 2, 3, 4);
  }
  return r;
}
)HLSL";
    const char kFlagZeroManifest[] =
        R"JSON({"schema_version":1,"constants":{"_Flag":{"type":"f32","v":[0.0]}}})JSON";
    std::string StrictFoldAsm;
    if (!CompileAndSpecializePsAsm(kStrictConstHlsl, "strict_const.hlsl",
                                   kFlagZeroManifest, StrictFoldAsm))
      return false;
    if (StrictFoldAsm.find("if_nz ") != std::string::npos ||
        StrictFoldAsm.find("if_z ") != std::string::npos ||
        StrictFoldAsm.find("1.000000") != std::string::npos) {
      std::cerr << "Audit #18 failed: constant [[dxcb5::strict_branch]] did not fold away\n"
                << StrictFoldAsm << '\n';
      return false;
    }
  }
  return true;
}

bool TestAuditRegressions() {
  // 1. Integer Texture2D<uint4>::Load folding must write integer 1 (NOT float bits 0x3f800000),
  //    address_border=true must NOT fold Sample, and isnormal(f32 subnormal) must be false!
  {
    const char kHlsl[] = R"HLSL(
Texture2D<uint4> _IntTex;
Texture2D<float4> _BorderTex;
SamplerState sampler_BorderTex;
[[dxcb5::immutable]] float _Subnormal;
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target {
  uint4 v = _IntTex.Load(int3(0, 0, 0));
  float4 b = _BorderTex.Sample(sampler_BorderTex, uv);
  uint isNorm = isnormal(_Subnormal) ? 1u : 0u;
  return float4(float(v.x), float(v.y), float(isNorm), b.r);
}
)HLSL";
    // 1.40129846e-45 is smallest positive f32 subnormal (0x00000001).
    const char kManifest[] = R"JSON({
      "schema_version": 1,
      "constants": { "_Subnormal": { "type": "f32", "v": [1.40129846e-45] } },
      "const_textures": {
        "_IntTex": { "v": [1.0, 2.0, 3.0, 4.0], "dims": [4, 4] },
        "_BorderTex": { "v": [0.5, 0.5, 0.5, 1.0], "address_border": true }
      }
    })JSON";
    std::string Asm;
    if (!CompileAndSpecializePsAsm(kHlsl, "audit_tex_subnorm.hlsl", kManifest, Asm))
      return false;
    // _IntTex folded to (1.0, 2.0) (NOT 1065353216.0!), isNorm folded to 0.0, _BorderTex still sampled!
    if (!CheckContains(Asm, "sample", "address_border=true must keep hardware sample") ||
        !CheckNotContains(Asm, "ld ", "const Texture2D<uint4>::Load must fold") ||
        !CheckNotContains(Asm, "1.06535", "int texture Load must NOT fold float IEEE bits") ||
        !CheckContains(Asm, "mov o0.x, l(1.000000)", "int tex lane x == 1") ||
        !CheckContains(Asm, "mov o0.y, l(2.000000)", "int tex lane y == 2") ||
        !CheckContains(Asm, "mov o0.z, l(0)", "subnormal isnormal == false"))
      return false;
  }

  // 2. Cross-Stage DCE must preserve PS inputs read via EvaluateAttributeCentroid,
  //    and $Globals compaction must align scalar after struct to 16-byte boundary AND
  //    persist the compacted layout into intermediate DXIL!
  {
    const char kHlsl[] = R"HLSL(
struct PairS { float a; };
[[dxcb5::immutable]] float _DeadConst;
PairS _LiveStruct;
float _AfterStruct;
struct VSOut {
  float4 pos : SV_Position;
  float2 cen : TEXCOORD0;
  float2 unused : TEXCOORD1;
};
VSOut VSMain(float4 p : POSITION, float2 uv : TEXCOORD0) {
  VSOut o;
  o.pos = p;
  o.cen = uv;
  o.unused = uv * 2.0;
  return o;
}
float4 PSMain(VSOut i) : SV_Target {
  float2 c = EvaluateAttributeCentroid(i.cen);
  return float4(c, _LiveStruct.a + _DeadConst, _AfterStruct);
}
)HLSL";
    dxcb5_uber_options VsO{"VSMain", "vs_5_0", "2021", "audit_pair.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_uber_options PsO{"PSMain", "ps_5_0", "2021", "audit_pair.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_blob VsDxil{}, PsDxil{}, Err{};
    if (dxcb5_compile_uber_dxil(kHlsl, sizeof(kHlsl) - 1, &VsO, &VsDxil, nullptr, &Err) != 0 ||
        dxcb5_compile_uber_dxil(kHlsl, sizeof(kHlsl) - 1, &PsO, &PsDxil, nullptr, &Err) != 0)
      return false;
    const char kM[] = R"JSON({
      "schema_version": 1,
      "constants": { "_DeadConst": { "type": "f32", "v": [0.0] } },
      "options": { "compact_globals": true, "cross_stage_dce": true, "emit_intermediate_dxil": true }
    })JSON";
    dxcb5_blob VsOpt{}, PsOpt{}, VsDxbc{}, PsDxbc{}, PErr{};
    if (dxcb5_specialize_pair(VsDxil.data, VsDxil.size, PsDxil.data, PsDxil.size,
                              kM, sizeof(kM) - 1, &VsOpt, &PsOpt, &VsDxbc, &PsDxbc,
                              nullptr, &PErr) != 0) {
      std::cerr << "Audit pair spec failed: "
                << (PErr.data ? static_cast<const char *>(PErr.data) : "") << '\n';
      return false;
    }
    // Reload intermediate PsOpt and compile stage 2: RDEF must show _AfterStruct at Offset 16!
    const char kM2[] = "{\"schema_version\":1,\"options\":{\"compact_globals\":true}}";
    dxcb5_blob Stage2Dxbc{}, S2Err{};
    if (dxcb5_specialize_stage(PsOpt.data, PsOpt.size, kM2, sizeof(kM2) - 1,
                               &Stage2Dxbc, nullptr, &S2Err) != 0)
      return false;
    const std::string S2Asm = DisasmDxbc(Stage2Dxbc.data, Stage2Dxbc.size);
    const std::string VsAsm = DisasmDxbc(VsDxbc.data, VsDxbc.size);
    dxcb5_free_blob(&VsDxil); dxcb5_free_blob(&PsDxil); dxcb5_free_blob(&Err);
    dxcb5_free_blob(&VsOpt); dxcb5_free_blob(&PsOpt);
    dxcb5_free_blob(&VsDxbc); dxcb5_free_blob(&PsDxbc); dxcb5_free_blob(&PErr);
    dxcb5_free_blob(&Stage2Dxbc); dxcb5_free_blob(&S2Err);
    if (!CheckContains(S2Asm, "eval_centroid", "Cross-Stage DCE must preserve EvalCentroid") ||
        !CheckContains(VsAsm, "TEXCOORD                 0", "VS must keep centroid TEXCOORD0") ||
        !CheckNotContains(VsAsm, "TEXCOORD                 1", "VS must prune unused TEXCOORD1") ||
        !CheckContains(S2Asm, "_AfterStruct;                // Offset:   16",
                       "Compacted $Globals must align scalar after struct to offset 16 and update GlobalSymbol"))
      return false;
  }

  // E. Round-2 audit regressions: out-of-order packoffset, Step 2 Cross-Stage DCE,
  //    column-major non-square spec_identity, and out-of-range manifest defines.
  {
    const char kHlsl[] = R"HLSL(
      cbuffer OrderedTest : register(b1) {
        float4x4 _BigMat : packoffset(c2);
        float4 _EarlyVec : packoffset(c0);
      };
      cbuffer DXCB5_SpecFlags : register(b2) {
        uint USE_EXTRA;
      };
      [[dxcb5::spec_identity]] column_major float3x4 _NonSqMat;
      struct VSIn { float4 p : POSITION; float2 uv : TEXCOORD0; };
      struct PSIn { float4 p : SV_POSITION; float2 uv : TEXCOORD0; float4 extra : TEXCOORD1; };
      PSIn VSMain(VSIn i) {
        PSIn o;
        o.p = i.p;
        o.uv = i.uv;
        o.extra = float4(i.uv, 1.0, 2.0);
        return o;
      }
      float4 PSMain(PSIn i) : SV_TARGET {
        float4 c = _BigMat[2] + _EarlyVec + mul(float3(i.uv, 1.0), _NonSqMat);
        if (USE_EXTRA != 0) c += i.extra;
        return c;
      }
    )HLSL";
    dxcb5_uber_options VsO{"VSMain", "vs_5_0", "2021", "audit2.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_uber_options PsO{"PSMain", "ps_5_0", "2021", "audit2.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_blob VsDxil{}, PsDxil{}, PsRefl{}, Err{};
    if (dxcb5_compile_uber_dxil(kHlsl, sizeof(kHlsl) - 1, &VsO, &VsDxil, nullptr, &Err) != 0 ||
        dxcb5_compile_uber_dxil(kHlsl, sizeof(kHlsl) - 1, &PsO, &PsDxil, &PsRefl, &Err) != 0) {
      std::cerr << "Audit2 uber failed: "
                << (Err.data ? static_cast<const char *>(Err.data) : "") << '\n';
      return false;
    }
    const std::string ReflStr(static_cast<const char *>(PsRefl.data), PsRefl.size);
    const char kM[] = R"JSON({
      "schema_version": 1,
      "defines": { "USE_EXTRA": 0 },
      "options": { "compact_globals": true, "cross_stage_dce": true, "emit_intermediate_dxil": true }
    })JSON";
    dxcb5_blob VsOpt{}, PsOpt{}, VsDxbc{}, PsDxbc{}, Stats{}, PErr{};
    if (dxcb5_specialize_pair(VsDxil.data, VsDxil.size, PsDxil.data, PsDxil.size,
                              kM, sizeof(kM) - 1, &VsOpt, &PsOpt, &VsDxbc, &PsDxbc,
                              &Stats, &PErr) != 0) {
      std::cerr << "Audit2 pair failed: "
                << (PErr.data ? static_cast<const char *>(PErr.data) : "") << '\n';
      return false;
    }
    const std::string VsAsm = DisasmDxbc(VsDxbc.data, VsDxbc.size);
    const std::string PsAsm = DisasmDxbc(PsDxbc.data, PsDxbc.size);
    // Also check out-of-range define value fails cleanly.
    const char kBadDef[] = "{\"schema_version\":1,\"defines\":{\"USE_EXTRA\":1e20}}";
    dxcb5_blob BadOut{}, BadErr{};
    const int BadRc = dxcb5_specialize_stage(
        PsDxil.data, PsDxil.size, kBadDef, sizeof(kBadDef) - 1, &BadOut, nullptr, &BadErr);
    dxcb5_free_blob(&VsDxil); dxcb5_free_blob(&PsDxil); dxcb5_free_blob(&PsRefl);
    dxcb5_free_blob(&Err); dxcb5_free_blob(&VsOpt); dxcb5_free_blob(&PsOpt);
    dxcb5_free_blob(&VsDxbc); dxcb5_free_blob(&PsDxbc); dxcb5_free_blob(&Stats);
    dxcb5_free_blob(&PErr); dxcb5_free_blob(&BadOut); dxcb5_free_blob(&BadErr);
    if (BadRc == 0) {
      std::cerr << "Audit2: expected out-of-range define to fail\n";
      return false;
    }
    if (!CheckContains(ReflStr, "\"spec_values\":[[1,0,0,0,1,0,0,0,1,0,0,0]]",
                       "Column-major float3x4 spec_identity must use major-minor order") ||
        !CheckNotContains(PsAsm, "_BigMat;                    // Offset:   32 Size:    64 [unused]",
                          "Out-of-order packoffset must keep _BigMat marked used") ||
        !CheckNotContains(VsAsm, "TEXCOORD                 1",
                          "Step 2 Cross-Stage DCE must prune TEXCOORD1 killed by define") ||
        !CheckNotContains(PsAsm, "TEXCOORD                 1",
                          "Step 2 Cross-Stage DCE must prune PS TEXCOORD1 killed by define"))
      return false;

    // Verify SpecCacheBridge ignores trailing // _dxcb5_def_ comments and
    // JSON-escapes special characters in define names.
    const char *kCommentSrc =
        "float x = 1.0; // _dxcb5_def_FAKE_FLAG\n"
        "static const int _dxcb5_def_REAL_FLAG = 1;\n";
    dxcb5_proxy::ActiveDefineMap Defs;
    dxcb5_proxy::CollectActiveDefines(nullptr, kCommentSrc,
                                      std::strlen(kCommentSrc), Defs);
    if (Defs.IsSet("FAKE_FLAG") || !Defs.IsSet("REAL_FLAG")) {
      std::cerr << "Audit2: trailing // _dxcb5_def_ handling failed\n";
      return false;
    }
    const std::string EscJson =
        dxcb5_proxy::BuildDefinesManifestJson({{"A\"B\\C", 1u}}, false);
    if (!CheckContains(EscJson, "\"A\\\"B\\\\C\":1",
                       "BuildDefinesManifestJson must JSON-escape define names"))
      return false;
  }

  // F. $Globals compaction with zero-byte empty struct (Unity sampler2D) +
  //    dynamic array indexing (including 1-based slot-1 offset folding).
  {
    const char kDynCbHlsl[] = R"HLSL(
      struct EmptySampler { Texture2D t; SamplerState s; };
      float4 _Dead0;
      float4 _Dead1;
      EmptySampler _UnitySampler;
      float4 _LiveColor;
      float4 _Dead2;
      float4x4 _BoneMats[4];
      float4 _Dead3;
      float4 PSMain(float4 p : SV_POSITION, nointerpolation uint slot : TEXCOORD0) : SV_TARGET {
        float4 res = _LiveColor;
        uint s = slot & 3;
        if (s != 0)
          res += mul(float4(p.xyz, 1.0), _BoneMats[s - 1]);
        return res;
      }
    )HLSL";
    const char kM[] = "{\"schema_version\":1,\"options\":{\"compact_globals\":true}}";
    std::string Asm;
    if (!CompileAndSpecializePsAsm(kDynCbHlsl, "dyncb.hlsl", kM, Asm))
      return false;
    if (!CheckContains(Asm, "dcl_constantbuffer CB0[17], dynamicIndexed",
                       "DynCB must compact $Globals to 1 + 16 = 17 rows") ||
        !CheckNotContains(Asm, "_Dead0", "DynCB must strip _Dead0") ||
        !CheckNotContains(Asm, "_Dead1", "DynCB must strip _Dead1") ||
        !CheckNotContains(Asm, "_Dead2", "DynCB must strip _Dead2") ||
        !CheckNotContains(Asm, "_Dead3", "DynCB must strip _Dead3") ||
        !CheckContains(Asm, "_LiveColor;                 // Offset:    0 Size:    16",
                       "DynCB must relocate _LiveColor to offset 0") ||
        !CheckContains(Asm, "_BoneMats[4];             // Offset:   16 Size:   256",
                       "DynCB must relocate _BoneMats to offset 16"))
      return false;
  }

  // G. 16-byte best-fit bin-packing of scattered scalars/vectors + exact dword
  //    RDEF liveness when compact_globals=false.
  {
    const char kPackHlsl[] = R"HLSL(
      float _S1;
      float _DeadSameRow;
      float4 _DeadVec;
      float3 _V3;
      float2 _V2a;
      float4 _DeadVec2;
      float2 _V2b;
      float4 PSMain() : SV_TARGET {
        return float4(_V3 + _S1, _V2a.x + _V2a.y + _V2b.x + _V2b.y);
      }
    )HLSL";
    OwnedBlob PsDxil;
    std::string AsmComp, AsmRaw;
    if (!CompileUber(kPackHlsl, "PSMain", "ps_5_0", "packcb.hlsl", PsDxil) ||
        !SpecializeStageAsm(PsDxil, "{\"schema_version\":1,\"options\":{\"compact_globals\":true}}", "packcb comp", AsmComp) ||
        !SpecializeStageAsm(PsDxil, "{\"schema_version\":1,\"options\":{\"compact_globals\":false}}", "packcb raw", AsmRaw))
      return false;
    if (!CheckContains(AsmComp, "dcl_constantbuffer CB0[2], immediateIndexed",
                       "Bin-packing must pack float+float3 and float2+float2 into CB0[2]") ||
        !CheckNotContains(AsmComp, "_DeadSameRow", "Compacted $Globals must strip _DeadSameRow") ||
        !CheckContains(AsmRaw, "_DeadSameRow;                // Offset:    4 Size:     4 [unused]",
                       "Exact dword liveness must mark row-sharing _DeadSameRow [unused]"))
      return false;
  }

  // H. Shared VS+PS $Globals compaction in dxcb5_specialize_pair: VS-only and
  //    PS-only variables must NEVER collide at offset 0 (neither in pair output
  //    nor when reloading vs.opt.dxil / ps.opt.dxil via dxcb5_specialize_stage).
  {
    const char kOutlineHlsl[] = R"HLSL(
      cbuffer DXCB5_SpecFlags : register(b2) { uint FOG_EXP2; };
      float _Dead0;
      float _OutlineWidth;
      float _OutlineZOffset;
      float4 _Dead1;
      float4 _OutlineColor;
      struct VSOut { float4 pos : SV_POSITION; };
      VSOut VSMain(float4 p : POSITION, float3 n : NORMAL) {
        VSOut o;
        o.pos = p + float4(n * _OutlineWidth, _OutlineZOffset);
        return o;
      }
      float4 PSMain(VSOut i) : SV_TARGET {
        float4 c = _OutlineColor;
        if (FOG_EXP2 != 0) c.rgb *= 0.5;
        return c;
      }
    )HLSL";
    dxcb5_uber_options VsO{"VSMain", "vs_5_0", "2021", "outline_shared.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_uber_options PsO{"PSMain", "ps_5_0", "2021", "outline_shared.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_blob VsDxil{}, PsDxil{}, Err{};
    if (dxcb5_compile_uber_dxil(kOutlineHlsl, sizeof(kOutlineHlsl) - 1, &VsO, &VsDxil, nullptr, &Err) != 0 ||
        dxcb5_compile_uber_dxil(kOutlineHlsl, sizeof(kOutlineHlsl) - 1, &PsO, &PsDxil, nullptr, &Err) != 0)
      return false;
    const char kMPair[] = R"JSON({
      "schema_version": 1,
      "defines": { "FOG_EXP2": 0 },
      "options": { "compact_globals": true, "cross_stage_dce": true, "emit_intermediate_dxil": true }
    })JSON";
    dxcb5_blob VsOpt{}, PsOpt{}, VsDxbc{}, PsDxbc{}, PErr{};
    if (dxcb5_specialize_pair(VsDxil.data, VsDxil.size, PsDxil.data, PsDxil.size,
                              kMPair, sizeof(kMPair) - 1, &VsOpt, &PsOpt,
                              &VsDxbc, &PsDxbc, nullptr, &PErr) != 0)
      return false;
    const char kMCold[] = R"JSON({"schema_version":1,"defines":{"FOG_EXP2":1},"options":{"compact_globals":true}})JSON";
    dxcb5_blob ColdVs{}, ColdPs{}, CErr{};
    if (dxcb5_specialize_stage(VsOpt.data, VsOpt.size, kMCold, sizeof(kMCold) - 1, &ColdVs, nullptr, &CErr) != 0 ||
        dxcb5_specialize_stage(PsOpt.data, PsOpt.size, kMCold, sizeof(kMCold) - 1, &ColdPs, nullptr, &CErr) != 0)
      return false;
    const std::string VsAsm = DisasmDxbc(VsDxbc.data, VsDxbc.size);
    const std::string PsAsm = DisasmDxbc(PsDxbc.data, PsDxbc.size);
    const std::string ColdVsAsm = DisasmDxbc(ColdVs.data, ColdVs.size);
    const std::string ColdPsAsm = DisasmDxbc(ColdPs.data, ColdPs.size);
    dxcb5_free_blob(&VsDxil); dxcb5_free_blob(&PsDxil); dxcb5_free_blob(&Err);
    dxcb5_free_blob(&VsOpt); dxcb5_free_blob(&PsOpt);
    dxcb5_free_blob(&VsDxbc); dxcb5_free_blob(&PsDxbc); dxcb5_free_blob(&PErr);
    dxcb5_free_blob(&ColdVs); dxcb5_free_blob(&ColdPs); dxcb5_free_blob(&CErr);
    if (!CheckContains(VsAsm, "_OutlineWidth;               // Offset:    0 Size:     4",
                       "Shared $Globals VS _OutlineWidth @ 0") ||
        !CheckContains(VsAsm, "_OutlineZOffset;             // Offset:    4 Size:     4",
                       "Shared $Globals VS _OutlineZOffset @ 4") ||
        !CheckContains(PsAsm, "_OutlineColor;              // Offset:   16 Size:    16",
                       "Shared $Globals PS _OutlineColor @ 16 (must NOT collide with VS @ 0)") ||
        !CheckContains(VsAsm, "dcl_constantbuffer CB0[2], immediateIndexed",
                       "Shared $Globals VS size == CB0[2]") ||
        !CheckContains(PsAsm, "dcl_constantbuffer CB0[2], immediateIndexed",
                       "Shared $Globals PS size == CB0[2]") ||
        !CheckContains(ColdVsAsm, "_OutlineWidth;               // Offset:    0 Size:     4",
                       "Cold Stage2 VS must keep shared _OutlineWidth @ 0") ||
        !CheckContains(ColdPsAsm, "_OutlineColor;              // Offset:   16 Size:    16",
                       "Cold Stage2 PS must keep shared _OutlineColor @ 16"))
      return false;
  }

  // I. M5 & M6: DXIL math const eval precision & denormal safety.
  //    - mad(1+2^-23, 1-2^-23, -1) rounds product to 1.0f -> 0.0f (NOT -1.42e-14).
  //    - frac(-1e-30) must NOT fold to 1.0f.
  //    - log2(1e-40) (denormal input) must NOT fold to -132.877.
  {
    const char kMathHlsl[] = R"HLSL(
      float _A;
      float _B;
      float _NegTiny;
      float _Denorm;
      float4 PSMain() : SV_TARGET {
        float m = mad(_A, _B, -1.0f);
        float f = frac(_NegTiny);
        float l = log2(_Denorm);
        return float4(m, f, l, 1.0f);
      }
    )HLSL";
    const char kMathManifest[] = R"JSON({
      "schema_version": 1,
      "constants": {
        "_A": { "type": "f32", "v": [1.0000001192092896] },
        "_B": { "type": "f32", "v": [0.9999998807907104] },
        "_NegTiny": { "type": "f32", "v": [-1e-30] },
        "_Denorm": { "type": "f32", "v": [1e-40] }
      },
      "options": { "compact_globals": true }
    })JSON";
    std::string Asm;
    if (!CompileAndSpecializePsAsm(kMathHlsl, "math_audit.hlsl", kMathManifest, Asm))
      return false;
    if (!CheckContains(Asm, "frc ", "M6: frac(-1e-30) must NOT fold to 1.0") ||
        !CheckContains(Asm, "log ", "M6: log2(denormal) must NOT fold with host non-FTZ") ||
        !CheckNotContains(Asm, "-1.42108547e-014", "M5: mad must not use exact FMA rounding"))
      return false;
  }

  // J. L4: compact_globals=true when ALL $Globals variables fold away must succeed cleanly.
  {
    const char kAllFoldHlsl[] = R"HLSL(
      float4 _C0;
      float4 PSMain() : SV_TARGET { return _C0; }
    )HLSL";
    const char kM[] = R"JSON({
      "schema_version": 1,
      "constants": { "_C0": { "type": "f32", "v": [0.25, 0.5, 0.75, 1.0] } },
      "options": { "compact_globals": true }
    })JSON";
    std::string Asm;
    if (!CompileAndSpecializePsAsm(kAllFoldHlsl, "allfold.hlsl", kM, Asm))
      return false;
  }

  // K. M9: Cross-stage DCE with mixed interpolation modes (linear + nointerpolation)
  //    must keep PS input interpolation modes in separate register rows.
  {
    const char kInterpHlsl[] = R"HLSL(
      struct VSOut {
        float4 pos : SV_POSITION;
        float2 uv : TEXCOORD0;
        nointerpolation uint id : TEXCOORD1;
        float2 deadUv : TEXCOORD2;
      };
      VSOut VSMain(float4 p : POSITION, float2 uv : TEXCOORD0, uint id : TEXCOORD1) {
        VSOut o;
        o.pos = p;
        o.uv = uv;
        o.id = id;
        o.deadUv = uv * 2.0;
        return o;
      }
      float4 PSMain(VSOut i) : SV_TARGET {
        return float4(i.uv, (float)i.id, 1.0);
      }
    )HLSL";
    dxcb5_uber_options VsO{"VSMain", "vs_5_0", "2021", "interp_m9.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_uber_options PsO{"PSMain", "ps_5_0", "2021", "interp_m9.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_blob VsDxil{}, PsDxil{}, Err{}, VsDxbc{}, PsDxbc{}, PErr{};
    if (dxcb5_compile_uber_dxil(kInterpHlsl, sizeof(kInterpHlsl) - 1, &VsO, &VsDxil, nullptr, &Err) != 0 ||
        dxcb5_compile_uber_dxil(kInterpHlsl, sizeof(kInterpHlsl) - 1, &PsO, &PsDxil, nullptr, &Err) != 0)
      return false;
    const char kM[] = "{\"schema_version\":1,\"options\":{\"cross_stage_dce\":true}}";
    if (dxcb5_specialize_pair(VsDxil.data, VsDxil.size, PsDxil.data, PsDxil.size,
                              kM, sizeof(kM) - 1, nullptr, nullptr, &VsDxbc,
                              &PsDxbc, nullptr, &PErr) != 0) {
      std::cerr << "M9 cross-stage interp test failed: "
                << (PErr.data ? static_cast<const char *>(PErr.data) : "") << '\n';
      return false;
    }
    const std::string PsAsm = DisasmDxbc(PsDxbc.data, PsDxbc.size);
    dxcb5_free_blob(&VsDxil); dxcb5_free_blob(&PsDxil); dxcb5_free_blob(&Err);
    dxcb5_free_blob(&VsDxbc); dxcb5_free_blob(&PsDxbc); dxcb5_free_blob(&PErr);
    if (!CheckContains(PsAsm, "dcl_input_ps linear", "M9 PS keeps linear") ||
        !CheckContains(PsAsm, "dcl_input_ps constant", "M9 PS keeps constant/nointerpolation"))
      return false;
  }

  // L. Tier1: Pair vs Cold-Reload $Globals layout consistency when a define kills
  //    a $Globals field (SpecializePairBytes must NOT re-compact after opt.dxil).
  {
    const char kLayoutHlsl[] = R"HLSL(
      cbuffer DXCB5_SpecFlags : register(b2) { uint USE_EXTRA; };
      float4 _ExtraOnlyWhenFlag;
      float4 _AlwaysLive;
      struct VSOut { float4 pos : SV_POSITION; };
      VSOut VSMain(float4 p : POSITION) { VSOut o; o.pos = p; return o; }
      float4 PSMain(VSOut i) : SV_TARGET {
        float4 c = _AlwaysLive;
        if (USE_EXTRA != 0) c += _ExtraOnlyWhenFlag;
        return c;
      }
    )HLSL";
    dxcb5_uber_options VsO{"VSMain", "vs_5_0", "2021", "layout_cons.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_uber_options PsO{"PSMain", "ps_5_0", "2021", "layout_cons.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_blob VsDxil{}, PsDxil{}, Err{};
    if (dxcb5_compile_uber_dxil(kLayoutHlsl, sizeof(kLayoutHlsl) - 1, &VsO, &VsDxil, nullptr, &Err) != 0 ||
        dxcb5_compile_uber_dxil(kLayoutHlsl, sizeof(kLayoutHlsl) - 1, &PsO, &PsDxil, nullptr, &Err) != 0)
      return false;
    const char kMPair[] = R"JSON({
      "schema_version": 1,
      "defines": { "USE_EXTRA": 0 },
      "options": { "compact_globals": true, "emit_intermediate_dxil": true }
    })JSON";
    dxcb5_blob VsOpt{}, PsOpt{}, VsDxbc{}, PsDxbc{}, PErr{}, ColdPs{}, CErr{};
    if (dxcb5_specialize_pair(VsDxil.data, VsDxil.size, PsDxil.data, PsDxil.size,
                              kMPair, sizeof(kMPair) - 1, &VsOpt, &PsOpt,
                              &VsDxbc, &PsDxbc, nullptr, &PErr) != 0 ||
        dxcb5_specialize_stage(PsOpt.data, PsOpt.size, kMPair, sizeof(kMPair) - 1,
                               &ColdPs, nullptr, &CErr) != 0)
      return false;
    const std::string PairAsm = DisasmDxbc(PsDxbc.data, PsDxbc.size);
    const std::string ColdAsm = DisasmDxbc(ColdPs.data, ColdPs.size);
    dxcb5_free_blob(&VsDxil); dxcb5_free_blob(&PsDxil); dxcb5_free_blob(&Err);
    dxcb5_free_blob(&VsOpt); dxcb5_free_blob(&PsOpt);
    dxcb5_free_blob(&VsDxbc); dxcb5_free_blob(&PsDxbc); dxcb5_free_blob(&PErr);
    dxcb5_free_blob(&ColdPs); dxcb5_free_blob(&CErr);
    if (!CheckContains(PairAsm, "_AlwaysLive;                // Offset:   16 Size:    16",
                       "Pair PS must keep Step-1 shared offset 16 for _AlwaysLive") ||
        !CheckContains(ColdAsm, "_AlwaysLive;                // Offset:   16 Size:    16",
                       "Cold reload PS must match Pair PS offset 16 for _AlwaysLive"))
      return false;
  }

  // M. Tier1 (latent H): SpecReflect live_spec_flags must scan the full
  //    [Off, Off + Size) dword range for multi-dword flags members (e.g. uint2).
  {
    const char kMultiFlagHlsl[] = R"HLSL(
      cbuffer DXCB5_SpecFlags : register(b2) {
        uint2 MULTI_FLAG;
        uint DEAD_FLAG;
      };
      float4 PSMain() : SV_TARGET {
        return MULTI_FLAG.y != 0 ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1);
      }
    )HLSL";
    dxcb5_uber_options PsO{"PSMain", "ps_5_0", "2021", "multi_flag.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_blob PsDxil{}, Refl{}, Err{};
    if (dxcb5_compile_uber_dxil(kMultiFlagHlsl, sizeof(kMultiFlagHlsl) - 1, &PsO, &PsDxil, &Refl, &Err) != 0)
      return false;
    const std::string ReflStr(static_cast<const char *>(Refl.data), Refl.size);
    dxcb5_free_blob(&PsDxil); dxcb5_free_blob(&Refl); dxcb5_free_blob(&Err);
    if (!CheckContains(ReflStr, "\"live_spec_flags\":[\"MULTI_FLAG\"]",
                       "SpecReflect must detect MULTI_FLAG live via .y dword"))
      return false;
  }

  // N. RDEF type record deduplication + post-MIR unused PS input pruning.
  {
    const char kRdefPruneHlsl[] = R"HLSL(
      cbuffer CB0 : register(b0) {
        float _A0; float _A1; float _A2; float _A3;
        float4 _V0; float4 _V1; float4 _V2; float4 _V3;
        float4x4 _M0; float4x4 _M1;
      };
      cbuffer CB1 : register(b1) {
        float _B0; float _B1;
        float4 _W0; float4 _W1;
      };
      cbuffer DXCB5_SpecFlags : register(b2) { uint USE_DEAD_UV; };
      struct PSIn {
        float4 pos : SV_POSITION;
        float2 uv  : TEXCOORD0;
        float  deadFog : TEXCOORD5;
      };
      float4 PSMain(PSIn i) : SV_TARGET {
        float s = _A0 + _A1 + _A2 + _A3 + _B0 + _B1;
        float4 v = _V0 + _V1 + _V2 + _V3 + _W0 + _W1 + mul(_M0, float4(i.uv, s, 1.0)) + _M1[0];
        if (USE_DEAD_UV != 0)
          v.x += i.deadFog;
        return v;
      }
    )HLSL";
    dxcb5_uber_options PsO{"PSMain", "ps_5_0", "2021", "rdef_prune.hlsl", nullptr, 0, nullptr, 0, 0};
    dxcb5_blob PsDxil{}, Err{}, Dxbc{}, SErr{};
    if (dxcb5_compile_uber_dxil(kRdefPruneHlsl, sizeof(kRdefPruneHlsl) - 1, &PsO, &PsDxil, nullptr, &Err) != 0)
      return false;
    const char kM[] = "{\"schema_version\":1,\"defines\":{\"USE_DEAD_UV\":0}}";
    if (dxcb5_specialize_stage(PsDxil.data, PsDxil.size, kM, sizeof(kM) - 1, &Dxbc, nullptr, &SErr) != 0)
      return false;
    const std::string Asm = DisasmDxbc(Dxbc.data, Dxbc.size);

    // 1) Verify D3DReflect resolves every variable's type identically after RDEF dedup.
    ComPtr<ID3D11ShaderReflection> Refl;
    if (FAILED(D3DReflect(Dxbc.data, Dxbc.size, IID_PPV_ARGS(&Refl))) || !Refl) {
      std::cerr << "RDEF dedup: D3DReflect failed\n";
      return false;
    }
    auto CheckVarType = [&](const char *CbName, const char *VarName,
                            D3D_SHADER_VARIABLE_CLASS Cls,
                            D3D_SHADER_VARIABLE_TYPE Ty, UINT Rows, UINT Cols) {
      auto *CB = Refl->GetConstantBufferByName(CbName);
      auto *Var = CB ? CB->GetVariableByName(VarName) : nullptr;
      auto *Type = Var ? Var->GetType() : nullptr;
      D3D11_SHADER_TYPE_DESC Desc{};
      if (!Type || FAILED(Type->GetDesc(&Desc)) || Desc.Class != Cls ||
          Desc.Type != Ty || Desc.Rows != Rows || Desc.Columns != Cols) {
        std::cerr << "RDEF dedup type mismatch for " << CbName << "." << VarName << '\n';
        return false;
      }
      return true;
    };
    bool Ok =
        CheckVarType("CB0", "_A0", D3D_SVC_SCALAR, D3D_SVT_FLOAT, 1, 1) &&
        CheckVarType("CB0", "_A3", D3D_SVC_SCALAR, D3D_SVT_FLOAT, 1, 1) &&
        CheckVarType("CB1", "_B1", D3D_SVC_SCALAR, D3D_SVT_FLOAT, 1, 1) &&
        CheckVarType("CB0", "_V0", D3D_SVC_VECTOR, D3D_SVT_FLOAT, 1, 4) &&
        CheckVarType("CB1", "_W1", D3D_SVC_VECTOR, D3D_SVT_FLOAT, 1, 4) &&
        CheckVarType("CB0", "_M0", D3D_SVC_MATRIX_COLUMNS, D3D_SVT_FLOAT, 4, 4) &&
        CheckVarType("CB0", "_M1", D3D_SVC_MATRIX_COLUMNS, D3D_SVT_FLOAT, 4, 4);

    // 2) Verify RDEF chunk is deduplicated (14 variables sharing 3 types -> well under 1150 B).
    uint32_t RdefSize = 0;
    const auto *Raw = static_cast<const uint8_t *>(Dxbc.data);
    if (Dxbc.size >= 32) {
      uint32_t ChunkCount = 0;
      std::memcpy(&ChunkCount, Raw + 28, 4);
      for (uint32_t C = 0; C < ChunkCount && 32 + (C + 1) * 4 <= Dxbc.size; ++C) {
        uint32_t Off = 0;
        std::memcpy(&Off, Raw + 32 + C * 4, 4);
        if (Off + 8 <= Dxbc.size && std::memcmp(Raw + Off, "RDEF", 4) == 0) {
          std::memcpy(&RdefSize, Raw + Off + 4, 4);
          break;
        }
      }
    }
    if (RdefSize == 0 || RdefSize >= 1150) {
      std::cerr << "RDEF dedup: expected compact RDEF (<1150 B), got "
                << RdefSize << '\n';
      Ok = false;
    }

    // 3) Verify dead PS input TEXCOORD5 (killed by USE_DEAD_UV=0) is pruned from ISGN & dcl_input_ps.
    Ok = Ok &&
         CheckContains(Asm, "TEXCOORD                 0", "PS keeps live TEXCOORD0") &&
         CheckNotContains(Asm, "TEXCOORD                 5", "PS prunes dead TEXCOORD5");

    dxcb5_free_blob(&PsDxil); dxcb5_free_blob(&Err);
    dxcb5_free_blob(&Dxbc); dxcb5_free_blob(&SErr);
    if (!Ok)
      return false;
  }
  return true;
}

int main(int argc, char **argv) {
  // 1. Compile Uber VS & PS DXIL (test -HV 2021 for PS and -HV 2018 for VS).
  dxcb5_uber_options VsOpts{};
  VsOpts.entry = "VSMain";
  VsOpts.target = "vs_5_0";
  VsOpts.hlsl_version = "2018";
  VsOpts.source_name = "uber_test.hlsl";

  dxcb5_uber_options PsOpts{};
  PsOpts.entry = "PSMain";
  PsOpts.target = "ps_5_0";
  PsOpts.hlsl_version = "2021";
  PsOpts.source_name = "uber_test.hlsl";

  dxcb5_blob VsDxil{}, VsRefl{}, VsErr{};
  dxcb5_blob PsDxil{}, PsRefl{}, PsErr{};
  if (dxcb5_compile_uber_dxil(kUberShader, sizeof(kUberShader) - 1, &VsOpts,
                              &VsDxil, &VsRefl, &VsErr) != 0) {
    std::cerr << "VS Uber DXIL failed: "
              << (VsErr.data ? static_cast<const char *>(VsErr.data) : "") << '\n';
    return 1;
  }
  if (dxcb5_compile_uber_dxil(kUberShader, sizeof(kUberShader) - 1, &PsOpts,
                              &PsDxil, &PsRefl, &PsErr) != 0) {
    std::cerr << "PS Uber DXIL failed: "
              << (PsErr.data ? static_cast<const char *>(PsErr.data) : "") << '\n';
    return 1;
  }

  // 2. Verify Reflection JSON via dxcb5_inspect_dxil.
  dxcb5_blob InspJson{}, InspErr{};
  if (dxcb5_inspect_dxil(PsDxil.data, PsDxil.size, &InspJson, &InspErr) != 0) {
    std::cerr << "Inspect failed: "
              << (InspErr.data ? static_cast<const char *>(InspErr.data) : "")
              << '\n';
    return 1;
  }
  const std::string ReflStr(static_cast<const char *>(InspJson.data),
                            InspJson.size);
  if (!CheckContains(ReflStr, "\"policy\":\"immutable\"", "Refl immutable") ||
      !CheckContains(ReflStr, "\"policy\":\"spec_const\"", "Refl spec_const") ||
      !CheckContains(ReflStr, "\"spec_values\":[[0],[0.5],[1]]", "Refl spec_values") ||
      !CheckContains(ReflStr, "\"spec_values\":[[1,1,1,1]]", "Refl spec_identity") ||
      !CheckContains(ReflStr, "\"spec_values\":[[0],[1]]", "Refl zero_one") ||
      !CheckContains(ReflStr, "\"policy\":\"dynamic\"", "Refl dynamic") ||
      !CheckContains(ReflStr, "\"SHADOWS_SCREEN\"", "Refl spec_flags") ||
      !CheckContains(ReflStr, "\"FOG_EXP2\"", "Refl spec_flags") ||
      !CheckContains(ReflStr, "\"live_spec_flags\":", "Refl live_spec_flags") ||
      !CheckContains(ReflStr, "\"name\":\"_AudioLinkTex\",\"kind\":\"tex2d\",\"policy\":\"dynamic\"",
                     "Refl dynamic tex"))
    return 1;

  // 3. Specialize pair: _UseRim=0, _ToneLift=1.44, _HueAngle=60, _RimMask=white,
  //    _MainTex_ST=identity, SHADOWS_SCREEN=1, FOG_EXP2=0.
  const char kManifest[] = R"JSON({
    "schema_version": 1,
    "constants": {
      "_ToneLift":   { "type": "f32", "v": [1.44] },
      "_UseRim":     { "type": "u32", "v": [0] },
      "_HueAngle":   { "type": "f32", "v": [60.0] },
      "_MainTex_ST": { "type": "f32", "v": [1.0, 1.0, 0.0, 0.0] },
      "\u005fTintScale": { "type": "f32", "v": [1.0, 1.0, 1.0, 1.0] },
      "_MixStep":    { "type": "f32", "v": [0.5] }
    },
    "const_textures": {
      "_RimMask": { "v": [1.0, 1.0, 1.0, 1.0] }
    },
    "defines": {
      "SHADOWS_SCREEN": 1,
      "FOG_EXP2": 0
    },
    "options": {
      "compact_globals": true,
      "cross_stage_dce": true,
      "emit_intermediate_dxil": true,
      "emit_stats": true
    }
  })JSON";

  dxcb5_blob VsOpt{}, PsOpt{}, VsDxbc{}, PsDxbc{}, StatsJson{}, PairErr{};
  if (dxcb5_specialize_pair(VsDxil.data, VsDxil.size, PsDxil.data, PsDxil.size,
                            kManifest, sizeof(kManifest) - 1, &VsOpt, &PsOpt,
                            &VsDxbc, &PsDxbc, &StatsJson, &PairErr) != 0) {
    std::cerr << "SpecializePair failed: "
              << (PairErr.data ? static_cast<const char *>(PairErr.data) : "")
              << '\n';
    return 1;
  }

  const std::string VsAsm = DisasmDxbc(VsDxbc.data, VsDxbc.size);
  const std::string PsAsm = DisasmDxbc(PsDxbc.data, PsDxbc.size);
  const std::string StatsStr(static_cast<const char *>(StatsJson.data),
                             StatsJson.size);

  // Verify DXIL math const eval folded sqrt(1.44) + cos(radians(60)) -> no sqrt/sincos!
  if (!CheckNotContains(PsAsm, "sqrt ", "PS math const eval") ||
      !CheckNotContains(PsAsm, "sincos ", "PS math const eval"))
    return 1;
  // Verify stripped textures & dead DXCB5_SpecFlags & untouched UnityPerDraw!
  if (!CheckNotContains(PsAsm, "_RimMask", "PS stripped _RimMask") ||
      !CheckNotContains(PsAsm, "_BumpMap", "PS stripped _BumpMap") ||
      !CheckNotContains(PsAsm, "DXCB5_SpecFlags", "PS stripped SpecFlags") ||
      !CheckNotContains(VsAsm, "DXCB5_SpecFlags", "VS stripped SpecFlags") ||
      !CheckContains(VsAsm, "UnityPerDraw", "VS kept UnityPerDraw"))
    return 1;
  // Verify Cross-Stage DCE stripped TEXCOORD1 from both VS and PS!
  if (!CheckNotContains(PsAsm, "TEXCOORD                 1", "PS cross-stage pruned TEXCOORD1") ||
      !CheckNotContains(VsAsm, "TEXCOORD                 1", "VS cross-stage pruned TEXCOORD1") ||
      !CheckContains(StatsStr, "\"_RimMask\"", "Stats stripped _RimMask") ||
      !CheckContains(StatsStr, "\"_BumpMap\"", "Stats stripped _BumpMap") ||
      !CheckContains(StatsStr, "\"TEXCOORD1\"", "Stats stripped TEXCOORD1"))
    return 1;
  // Verify $Globals compaction removed folded _ToneLift / _HueAngle / _UseRim,
  // and surviving _Color is marked USED (no [unused] tag in RDEF)!
  if (!CheckNotContains(PsAsm, "_ToneLift", "PS compacted _ToneLift") ||
      !CheckNotContains(PsAsm, "_HueAngle", "PS compacted _HueAngle") ||
      !CheckContains(PsAsm, "_Color", "PS kept live _Color") ||
      !CheckNotContains(PsAsm, "_Color;                     // Offset:   16 Size:    16 [unused]",
                        "PS _Color must be marked D3D_SVF_USED"))
    return 1;

  // 4. Cold define cache-miss over intermediate DXIL: FOG_EXP2=1 must STILL work!
  const char kColdFogManifest[] = R"JSON({
    "schema_version": 1,
    "defines": { "SHADOWS_SCREEN": 0, "FOG_EXP2": 1 },
    "options": { "compact_globals": true }
  })JSON";
  dxcb5_blob ColdVsDxbc{}, ColdPsDxbc{}, ColdStats{}, ColdErr{};
  if (dxcb5_specialize_stage(VsOpt.data, VsOpt.size, kColdFogManifest,
                             sizeof(kColdFogManifest) - 1, &ColdVsDxbc,
                             &ColdStats, &ColdErr) != 0 ||
      dxcb5_specialize_stage(PsOpt.data, PsOpt.size, kColdFogManifest,
                             sizeof(kColdFogManifest) - 1, &ColdPsDxbc, nullptr,
                             &ColdErr) != 0) {
    std::cerr << "Cold FOG_EXP2=1 specialize_stage failed: "
              << (ColdErr.data ? static_cast<const char *>(ColdErr.data) : "")
              << '\n';
    return 1;
  }
  const std::string ColdPsAsm = DisasmDxbc(ColdPsDxbc.data, ColdPsDxbc.size);
  const std::string ColdVsAsm = DisasmDxbc(ColdVsDxbc.data, ColdVsDxbc.size);
  if (!CheckContains(ColdPsAsm, "TEXCOORD                 2", "Cold FOG_EXP2=1 PS kept fog") ||
      !CheckContains(ColdVsAsm, "TEXCOORD                 2", "Cold FOG_EXP2=1 VS kept fog") ||
      !CheckNotContains(ColdPsAsm, "TEXCOORD                 1", "Cold FOG_EXP2=1 PS kept rim pruned") ||
      !CheckNotContains(ColdPsAsm, "DXCB5_SpecFlags", "Cold FOG_EXP2=1 stripped SpecFlags"))
    return 1;

  // 5. CgBatch Plugin Spec Stub Cache HIT + Cold MISS + nan_safe + SPS/SPS-I test.
  if (argc >= 4) {
    if (!TestCgBatchSpecStubCache(argv[1], argv[2], argv[3], PsOpt, PsDxbc,
                                  ReflStr))
      return 1;
  }

  // 6. Phase 2 annotations E2E test (strict_branch, uniform, fast_math).
  if (!TestPhase2Annotations())
    return 1;

  // 7. Regression tests: array/matrix cbuffer layout & texture mip/OOB load safety.
  if (!TestArrayMatrixAndTextureSafety())
    return 1;
  if (!TestAuditRegressions())
    return 1;

  // 8. Negative manifest, ABI version, struct_size, and dxcb5_status checks (Audit #13/#14/#33/#36/#37/#38).
  {
    if (dxcb5_get_abi_version() != DXCB5_SPEC_ABI_VERSION) {
      std::cerr << "dxcb5_get_abi_version mismatch: " << dxcb5_get_abi_version()
                << '\n';
      return 1;
    }
    dxcb5_uber_options BadAbiOpts{};
    BadAbiOpts.entry = "PSMain";
    BadAbiOpts.target = "ps_5_0";
    BadAbiOpts.struct_size = 4; // intentionally wrong size
    dxcb5_blob BadOut{}, BadErr{};
    const int AbiRc = dxcb5_compile_uber_dxil(
        kUberShader, sizeof(kUberShader) - 1, &BadAbiOpts, &BadOut, nullptr,
        &BadErr);
    dxcb5_free_blob(&BadOut);
    dxcb5_free_blob(&BadErr);
    if (AbiRc != DXCB5_ERR_ABI_MISMATCH) {
      std::cerr << "Expected DXCB5_ERR_ABI_MISMATCH, got " << AbiRc << '\n';
      return 1;
    }
    if (dxcb5_compile_uber_dxil(nullptr, 0, nullptr, nullptr, nullptr, nullptr) !=
        DXCB5_ERR_INVALID_ARG) {
      std::cerr << "Expected DXCB5_ERR_INVALID_ARG on null args\n";
      return 1;
    }
    if (dxcb5_set_disabled_passes("not_a_real_pass_name") !=
        DXCB5_ERR_INVALID_PASS) {
      std::cerr << "Expected DXCB5_ERR_INVALID_PASS on bogus pass name\n";
      return 1;
    }
    dxcb5_set_disabled_passes("");

    const struct {
      const char *Manifest;
      int ExpectedRc;
    } kBadManifests[] = {
        {R"JSON({"schema_version":2})JSON", DXCB5_ERR_PARSE},
        {R"JSON({"schema_version":1,"uber_dxil_hash":123})JSON", DXCB5_ERR_PARSE},
        {R"JSON({"schema_version":1,"options":{"nan_safe":"yes"}})JSON", DXCB5_ERR_PARSE},
        {R"JSON({"schema_version":1,"uber_dxil_hash":"0000000000000000000000000000000000000000000000000000000000000000"})JSON",
         DXCB5_ERR_HASH_MISMATCH},
    };
    for (const auto &Case : kBadManifests) {
      dxcb5_blob OutDxbc{}, OutErr{};
      const int Rc = dxcb5_specialize_stage(
          PsOpt.data, PsOpt.size, Case.Manifest, std::strlen(Case.Manifest),
          &OutDxbc, nullptr, &OutErr);
      dxcb5_free_blob(&OutDxbc);
      dxcb5_free_blob(&OutErr);
      if (Rc != Case.ExpectedRc) {
        std::cerr << "Expected status " << Case.ExpectedRc << " got " << Rc
                  << " for: " << Case.Manifest << '\n';
        return 1;
      }
    }
  }

  dxcb5_free_blob(&VsDxil);
  dxcb5_free_blob(&VsRefl);
  dxcb5_free_blob(&VsErr);
  dxcb5_free_blob(&PsDxil);
  dxcb5_free_blob(&PsRefl);
  dxcb5_free_blob(&PsErr);
  dxcb5_free_blob(&InspJson);
  dxcb5_free_blob(&InspErr);
  dxcb5_free_blob(&VsOpt);
  dxcb5_free_blob(&PsOpt);
  dxcb5_free_blob(&VsDxbc);
  dxcb5_free_blob(&PsDxbc);
  dxcb5_free_blob(&StatsJson);
  dxcb5_free_blob(&PairErr);
  dxcb5_free_blob(&ColdVsDxbc);
  dxcb5_free_blob(&ColdPsDxbc);
  dxcb5_free_blob(&ColdStats);
  dxcb5_free_blob(&ColdErr);
  return 0;
}
