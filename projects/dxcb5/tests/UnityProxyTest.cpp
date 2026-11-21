// Distributed under the University of Illinois Open Source License.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include "../unity-proxy/NativeBridge.h"
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

void Require(bool cond, const char *msg) {
  if (!cond)
    throw std::runtime_error(msg);
}

std::wstring ToWide(const char *s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n - 1);
  return out;
}

std::string ReadFileText(const std::wstring &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void CopyOrFail(const std::wstring &src, const std::wstring &dst) {
  if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
    throw std::runtime_error("CopyFileW failed");
}

class MemoryInclude final : public ID3DInclude {
public:
  HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID,
                                 LPCVOID *ppData, UINT *pBytes) override {
    if (std::strcmp(pFileName, "common.hlsl") != 0)
      return E_FAIL;
    static const char kHeader[] =
        "static const float4 kTint = float4(0.25, 0.5, 0.75, 1.0);\n";
    *ppData = kHeader;
    *pBytes = static_cast<UINT>(sizeof(kHeader) - 1);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};

using PFN_D3DCompile = HRESULT(WINAPI *)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
    LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
using PFN_D3DCompile2 = HRESULT(WINAPI *)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
    LPCSTR, UINT, UINT, UINT, LPCVOID, SIZE_T, ID3DBlob **, ID3DBlob **);
using PFN_D3DReflect = HRESULT(WINAPI *)(LPCVOID, SIZE_T, REFIID, void **);
using PFN_D3DStripShader = HRESULT(WINAPI *)(LPCVOID, SIZE_T, UINT, ID3DBlob **);
using PFN_D3DDisassemble = HRESULT(WINAPI *)(LPCVOID, SIZE_T, UINT, LPCSTR,
                                             ID3DBlob **);

} // namespace

int main(int argc, char **argv) {
  try {
    Require(argc >= 5,
            "Usage: DXCB5UnityProxyTest <proxy.dll> <fxc.dll> <dxcompiler.dll> "
            "<stageDir> [optionalHlslFile]");
    const std::wstring proxySrc = ToWide(argv[1]);
    const std::wstring fxcSrc = ToWide(argv[2]);
    const std::wstring dxcSrc = ToWide(argv[3]);
    const std::wstring stageDir = ToWide(argv[4]);
    const std::wstring cfgDir = stageDir + L"\\cfg";

    CreateDirectoryW(stageDir.c_str(), nullptr);
    CreateDirectoryW(cfgDir.c_str(), nullptr);
    DeleteFileW((cfgDir + L"\\stats.log").c_str());
    DeleteFileW((cfgDir + L"\\config.ini").c_str());
    SetEnvironmentVariableW(L"DXCB5_UNITY_CONFIG_DIR", cfgDir.c_str());
    SetEnvironmentVariableW(L"DXCB5_UNITY_ENABLE", nullptr);

    const std::wstring stagedProxy = stageDir + L"\\D3DCompiler_47.dll";
    CopyOrFail(proxySrc, stagedProxy);
    CopyOrFail(fxcSrc, stageDir + L"\\D3DCompiler_47_fxc.dll");
    CopyOrFail(dxcSrc, stageDir + L"\\dxcb5_dxcompiler.dll");

    // Simulate UnityShaderCompiler.exe's custom in-memory PE loader copy
    // publishing a PeloaderBridgeSlot in TLS immediately before the native
    // LoadLibraryExW("Tools/D3DCompiler_47.dll") call on the same thread.
    dxcb5_proxy::PeloaderBridgeSlot peerSlot{};
    const DWORD peerTls = TlsAlloc();
    Require(peerTls != TLS_OUT_OF_INDEXES, "TlsAlloc failed for bridge test");
    peerSlot.Magic1 = dxcb5_proxy::kBridgeMagic1;
    peerSlot.Magic2 = dxcb5_proxy::kBridgeMagic2;
    peerSlot.Pid = GetCurrentProcessId();
    peerSlot.TlsIndex = peerTls;
    peerSlot.Self = &peerSlot;
    peerSlot.NativeDispatch = nullptr;
    Require(TlsSetValue(peerTls, &peerSlot) != 0, "TlsSetValue failed");

    HMODULE proxy = LoadLibraryW(stagedProxy.c_str());
    Require(proxy != nullptr, "Failed to load staged proxy D3DCompiler_47.dll");
    Require(peerSlot.NativeDispatch != nullptr,
            "Native DllMain failed to connect peer PeloaderBridgeSlot");
    Require(peerSlot.TlsIndex == TLS_OUT_OF_INDEXES,
            "Native DllMain failed to clear/free peer bridge TLS slot");
    const auto *peerDisp =
        static_cast<const dxcb5_proxy::NativeDispatchTable *>(
            peerSlot.NativeDispatch);
    Require(peerDisp->D3DCompile && peerDisp->D3DPreprocess &&
                peerDisp->D3DReflect,
            "Peer NativeDispatchTable missing required entries");

    auto fnCompile = reinterpret_cast<PFN_D3DCompile>(
        GetProcAddress(proxy, "D3DCompile"));
    auto fnCompile2 = reinterpret_cast<PFN_D3DCompile2>(
        GetProcAddress(proxy, "D3DCompile2"));
    auto fnReflect = reinterpret_cast<PFN_D3DReflect>(
        GetProcAddress(proxy, "D3DReflect"));
    auto fnStrip = reinterpret_cast<PFN_D3DStripShader>(
        GetProcAddress(proxy, "D3DStripShader"));
    auto fnDisasm = reinterpret_cast<PFN_D3DDisassemble>(
        GetProcAddress(proxy, "D3DDisassemble"));
    Require(fnCompile && fnCompile2 && fnReflect && fnStrip && fnDisasm,
            "Missing expected exports from proxy D3DCompiler_47.dll");

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                        0, &fl, 1, D3D11_SDK_VERSION, &dev,
                                        nullptr, &ctx)),
            "D3D11CreateDevice WARP failed");

    // 1. Native DX11 VS + standalone PS + D3DReflect(STAT) + D3DStripShader +
    //    D3DDisassemble + WARP creation.
    const char *kDx11Src = R"HLSL(
      cbuffer PerFrame : register(b0) { float4x4 mvp; float4 color; };
      Texture2D<float4> _MainTex;
      SamplerState sampler_MainTex;
      struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
      struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
      PSIn VSMain(VSIn i) {
        PSIn o;
        o.pos = mul(mvp, float4(i.pos, 1.0));
        o.uv = i.uv;
        return o;
      }
      float4 PSMain(PSIn i) : SV_TARGET {
        return _MainTex.Sample(sampler_MainTex, i.uv) * color;
      }
    )HLSL";
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    Require(SUCCEEDED(fnCompile(kDx11Src, std::strlen(kDx11Src), "dx11.hlsl",
                                nullptr, nullptr, "VSMain", "vs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob,
                                &errBlob)),
            "Proxy vs_5_0 compile failed");
    Require(SUCCEEDED(fnCompile2(kDx11Src, std::strlen(kDx11Src), "dx11.hlsl",
                                 nullptr, nullptr, "PSMain", "ps_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, 0, nullptr,
                                 0, &psBlob, &errBlob)),
            "Proxy ps_5_0 compile failed");

    ComPtr<ID3D11ShaderReflection> refl;
    Require(SUCCEEDED(fnReflect(psBlob->GetBufferPointer(),
                                psBlob->GetBufferSize(), IID_PPV_ARGS(&refl))),
            "Forwarded D3DReflect failed on DXCB5 PS");
    D3D11_SHADER_DESC desc{};
    Require(SUCCEEDED(refl->GetDesc(&desc)), "GetDesc failed");
    Require(desc.InstructionCount > 0, "Expected non-zero STAT InstructionCount");

    ComPtr<ID3DBlob> stripped, disasm;
    Require(SUCCEEDED(fnStrip(psBlob->GetBufferPointer(),
                              psBlob->GetBufferSize(),
                              D3DCOMPILER_STRIP_REFLECTION_DATA, &stripped)),
            "Forwarded D3DStripShader failed");
    Require(SUCCEEDED(fnDisasm(psBlob->GetBufferPointer(),
                               psBlob->GetBufferSize(), 0, nullptr, &disasm)),
            "Forwarded D3DDisassemble failed");

    ComPtr<ID3D11VertexShader> vsObj;
    ComPtr<ID3D11PixelShader> psObj;
    Require(SUCCEEDED(dev->CreateVertexShader(vsBlob->GetBufferPointer(),
                                              vsBlob->GetBufferSize(), nullptr,
                                              &vsObj)),
            "WARP CreateVertexShader failed");
    Require(SUCCEEDED(dev->CreatePixelShader(psBlob->GetBufferPointer(),
                                             psBlob->GetBufferSize(), nullptr,
                                             &psObj)),
            "WARP CreatePixelShader failed");

    // 2. Legacy Unity DX9 sampler2D + tex2D + `: COLOR` PS semantic +
    //    D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY (as Unity passes).
    const char *kLegacySrc = R"HLSL(
      sampler2D _FootprintSDF;
      inline float4 SampleHelper(sampler2D t, float2 uv) {
        return tex2D(t, uv) + tex2Dlod(t, float4(uv, 0, 1));
      }
      struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
      float4 LegacyPS(PSIn i) : COLOR {
        return SampleHelper(_FootprintSDF, i.uv);
      }
    )HLSL";
    ComPtr<ID3DBlob> legacyBlob;
    Require(SUCCEEDED(fnCompile(
                kLegacySrc, std::strlen(kLegacySrc), "legacy.hlsl", nullptr,
                nullptr, "LegacyPS", "ps_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3 |
                    D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY,
                0, &legacyBlob, &errBlob)),
            "Proxy legacy sampler2D compile failed");
    ComPtr<ID3D11ShaderReflection> legacyRefl;
    Require(SUCCEEDED(fnReflect(legacyBlob->GetBufferPointer(),
                                legacyBlob->GetBufferSize(),
                                IID_PPV_ARGS(&legacyRefl))),
            "D3DReflect failed on legacy PS");
    D3D11_SHADER_INPUT_BIND_DESC texBind{}, sampBind{};
    Require(SUCCEEDED(legacyRefl->GetResourceBindingDescByName("_FootprintSDF",
                                                               &texBind)),
            "Expected _FootprintSDF texture binding");
    Require(SUCCEEDED(legacyRefl->GetResourceBindingDescByName(
                "sampler_FootprintSDF", &sampBind)),
            "Expected sampler_FootprintSDF sampler binding");
    ComPtr<ID3D11PixelShader> legacyPsObj;
    Require(SUCCEEDED(dev->CreatePixelShader(legacyBlob->GetBufferPointer(),
                                             legacyBlob->GetBufferSize(),
                                             nullptr, &legacyPsObj)),
            "WARP CreatePixelShader failed for legacy sampler2D");

    // 3. ID3DInclude bridge.
    const char *kIncSrc = R"HLSL(
      #include "common.hlsl"
      float4 IncPS(float4 pos : SV_POSITION) : SV_TARGET { return kTint; }
    )HLSL";
    MemoryInclude memInc;
    ComPtr<ID3DBlob> incBlob;
    Require(SUCCEEDED(fnCompile(kIncSrc, std::strlen(kIncSrc), "inc.hlsl",
                                nullptr, &memInc, "IncPS", "ps_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &incBlob,
                                &errBlob)),
            "Proxy ID3DInclude compile failed");

    // 4. Automatic FXC fallback: cs_5_0 and double ps_5_0.
    const char *kCsSrc = R"HLSL(
      RWStructuredBuffer<float> buf : register(u0);
      [numthreads(1,1,1)]
      void CSMain(uint3 id : SV_DispatchThreadID) { buf[id.x] = 1.0; }
    )HLSL";
    ComPtr<ID3DBlob> csBlob;
    Require(SUCCEEDED(fnCompile(kCsSrc, std::strlen(kCsSrc), "cs.hlsl", nullptr,
                                nullptr, "CSMain", "cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob,
                                &errBlob)),
            "FXC fallback cs_5_0 failed");

    const char *kDoubleSrc = R"HLSL(
      cbuffer CB { double v; };
      float4 DoublePS(float4 pos : SV_POSITION) : SV_TARGET {
        double d = v * 2.0 + 1.0;
        return float4((float)d, 0, 0, 1);
      }
    )HLSL";
    ComPtr<ID3DBlob> dblBlob;
    Require(SUCCEEDED(fnCompile(kDoubleSrc, std::strlen(kDoubleSrc),
                                "dbl.hlsl", nullptr, nullptr, "DoublePS",
                                "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &dblBlob, &errBlob)),
            "FXC fallback double ps_5_0 failed");

    // Verify stats.log recorded HITs and FALLBACK.
    const std::string log1 = ReadFileText(cfgDir + L"\\stats.log");
    Require(log1.find("HIT\tvs_5_0\tVSMain") != std::string::npos,
            "Missing HIT log for VSMain");
    Require(log1.find("HIT\tps_5_0\tPSMain") != std::string::npos,
            "Missing HIT log for PSMain");
    Require(log1.find("HIT\tps_5_0\tLegacyPS") != std::string::npos,
            "Missing HIT log for LegacyPS");
    Require(log1.find("HIT\tps_5_0\tIncPS") != std::string::npos,
            "Missing HIT log for IncPS");
    Require(log1.find("FALLBACK\tps_5_0\tDoublePS") != std::string::npos,
            "Missing FALLBACK log for DoublePS");

    // 5. DXCB5_UNITY_ENABLE=0 bypasses DXCB5 cleanly.
    SetEnvironmentVariableW(L"DXCB5_UNITY_ENABLE", L"0");
    ComPtr<ID3DBlob> bypassBlob;
    Require(SUCCEEDED(fnCompile(kDx11Src, std::strlen(kDx11Src), "dx11.hlsl",
                                nullptr, nullptr, "PSMain", "ps_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bypassBlob,
                                &errBlob)),
            "Bypass compile failed");
    const std::string log2 = ReadFileText(cfgDir + L"\\stats.log");
    Require(log2.size() == log1.size(),
            "Expected no DXCB5 log entries when DXCB5_UNITY_ENABLE=0");

    // 6. Project-scoped auto-revert on VCC package uninstall:
    //    When a Unity project has Packages/com.moepus.dxcb5/package.json -> HIT.
    //    When that package is deleted (VCC uninstall) -> automatic FXC bypass!
    SetEnvironmentVariableW(L"DXCB5_UNITY_ENABLE", L"1");
    const std::wstring fakeProj = stageDir + L"\\fake-unity-project";
    const std::wstring fakePkgs = fakeProj + L"\\Packages";
    const std::wstring fakePkgDir = fakePkgs + L"\\com.moepus.dxcb5";
    const std::wstring fakePkgJson = fakePkgDir + L"\\package.json";
    CreateDirectoryW(fakeProj.c_str(), nullptr);
    CreateDirectoryW(fakePkgs.c_str(), nullptr);
    CreateDirectoryW(fakePkgDir.c_str(), nullptr);
    {
      std::ofstream f(fakePkgJson, std::ios::binary);
      f << "{\"name\":\"com.moepus.dxcb5\"}\n";
    }
    SetEnvironmentVariableW(L"DXCB5_UNITY_PROJECT_DIR", fakeProj.c_str());

    ComPtr<ID3DBlob> projHitBlob;
    Require(SUCCEEDED(fnCompile(kDx11Src, std::strlen(kDx11Src), "dx11.hlsl",
                                nullptr, nullptr, "PSMain", "ps_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &projHitBlob,
                                &errBlob)),
            "Project-scoped HIT compile failed");
    const std::string log3 = ReadFileText(cfgDir + L"\\stats.log");
    Require(log3.size() > log2.size(),
            "Expected DXCB5 HIT when com.moepus.dxcb5 is installed in project");

    // Simulate VCC removing com.moepus.dxcb5 while Unity is closed.
    DeleteFileW(fakePkgJson.c_str());
    RemoveDirectoryW(fakePkgDir.c_str());

    ComPtr<ID3DBlob> projRemovedBlob;
    Require(SUCCEEDED(fnCompile(kDx11Src, std::strlen(kDx11Src), "dx11.hlsl",
                                nullptr, nullptr, "PSMain", "ps_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &projRemovedBlob, &errBlob)),
            "Post-uninstall FXC fallback compile failed");
    const std::string log4 = ReadFileText(cfgDir + L"\\stats.log");
    Require(log4.size() == log3.size(),
            "Expected automatic FXC bypass after com.moepus.dxcb5 is removed");
    SetEnvironmentVariableW(L"DXCB5_UNITY_PROJECT_DIR", nullptr);

    // 7. Peer NativeDispatchTable (peloader bridge) compile + preprocess.
    {
      ComPtr<ID3DBlob> bridgeBlob, preBlob;
      Require(SUCCEEDED(peerDisp->D3DPreprocess(
                  kDx11Src, std::strlen(kDx11Src), "dx11.hlsl", nullptr,
                  nullptr, &preBlob, &errBlob)),
              "Peer NativeDispatchTable D3DPreprocess failed");
      Require(SUCCEEDED(peerDisp->D3DCompile(
                  kDx11Src, std::strlen(kDx11Src), "dx11.hlsl", nullptr,
                  nullptr, "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                  0, &bridgeBlob, &errBlob)),
              "Peer NativeDispatchTable D3DCompile failed");
    }

    // 8. stats.log rotation to stats.log.1 once size exceeds 512 KB.
    {
      const std::wstring statsPath = cfgDir + L"\\stats.log";
      const std::wstring prevPath = cfgDir + L"\\stats.log.1";
      DeleteFileW(prevPath.c_str());
      {
        std::ofstream f(statsPath, std::ios::binary | std::ios::trunc);
        std::string pad(520 * 1024, 'x');
        f.write(pad.data(), static_cast<std::streamsize>(pad.size()));
      }
      ComPtr<ID3DBlob> rotBlob;
      Require(SUCCEEDED(fnCompile(kDx11Src, std::strlen(kDx11Src), "dx11.hlsl",
                                  nullptr, nullptr, "PSMain", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &rotBlob,
                                  &errBlob)),
              "Rotation trigger compile failed");
      const std::string rotatedPrev = ReadFileText(prevPath);
      const std::string rotatedCurr = ReadFileText(statsPath);
      Require(rotatedPrev.size() >= 520 * 1024,
              "Expected stats.log.1 to hold previous log after rotation");
      Require(rotatedCurr.size() < 4096 &&
                  rotatedCurr.find("HIT\tps_5_0\tPSMain") != std::string::npos,
              "Expected fresh stats.log with new HIT after rotation");
    }

    // Optional real Unity preprocessed HLSL file check (argv[5..]).
    SetEnvironmentVariableW(L"DXCB5_UNITY_ENABLE", L"1");
    for (int i = 5; i + 2 < argc; i += 3) {
      const std::wstring path = ToWide(argv[i]);
      const char *entry = argv[i + 1];
      const char *target = argv[i + 2];
      const std::string src = ReadFileText(path);
      Require(!src.empty(), "Failed to read optional HLSL file");
      ComPtr<ID3DBlob> blob;
      Require(SUCCEEDED(fnCompile(
                  src.data(), src.size(), argv[i], nullptr, nullptr, entry,
                  target,
                  D3DCOMPILE_OPTIMIZATION_LEVEL3 |
                      D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY,
                  0, &blob, &errBlob)),
              "Optional HLSL proxy compile failed");
      if (std::strcmp(target, "vs_5_0") == 0) {
        ComPtr<ID3D11VertexShader> v;
        Require(SUCCEEDED(dev->CreateVertexShader(blob->GetBufferPointer(),
                                                  blob->GetBufferSize(),
                                                  nullptr, &v)),
                "Optional HLSL WARP VS failed");
      } else {
        ComPtr<ID3D11PixelShader> p;
        Require(SUCCEEDED(dev->CreatePixelShader(blob->GetBufferPointer(),
                                                 blob->GetBufferSize(),
                                                 nullptr, &p)),
                "Optional HLSL WARP PS failed");
      }
      std::printf("PASS optional %s (%s %s)\n", argv[i], target, entry);
    }

    // Release all COM objects backed by the proxy's loaded FXC/DXC DLLs before
    // unloading the proxy DLL and verifying clean peer-slot disconnect.
    vsBlob.Reset();
    psBlob.Reset();
    errBlob.Reset();
    refl.Reset();
    Require(FreeLibrary(proxy) != 0, "FreeLibrary(proxy) failed");
    Require(peerSlot.NativeDispatch == nullptr,
            "Expected FreeLibrary(proxy) to clear peer PeloaderBridgeSlot");

    std::printf("DXCB5UnityProxyTest PASSED\n");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "DXCB5UnityProxyTest FAILED: %s\n", e.what());
    return 1;
  }
}
