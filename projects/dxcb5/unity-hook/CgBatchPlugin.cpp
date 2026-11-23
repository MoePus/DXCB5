// Distributed under the University of Illinois Open Source License.
// Native UnityShaderCompiler.exe CgBatch plugin loaded via GetPluginInterface.
// Hooks D3DCompilerWrapper::D3DCompile (vtable slot 3 in-place, preserving
// RTTI and all slots) with zero Editor/Data/Tools patching and full stock FXC fallback.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "dxc/dxcapi.h"
#include "dxcb5/Sha256.h"
#include "dxcb5/dxcb5_spec.h"
#include "SpecCacheBridge.h"
#include "UnityHlslAdapter.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

class Dxcb5MemBlob final : public ID3DBlob {
public:
  explicit Dxcb5MemBlob(std::vector<uint8_t> bytes) : Data(std::move(bytes)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
      *ppv = static_cast<ID3DBlob *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++RefCount; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --RefCount;
    if (r == 0)
      delete this;
    return r;
  }
  LPVOID STDMETHODCALLTYPE GetBufferPointer() override {
    return Data.empty() ? nullptr : Data.data();
  }
  SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return Data.size(); }

private:
  std::atomic<ULONG> RefCount{1};
  std::vector<uint8_t> Data;
};

class D3DIncludeBridge final : public IDxcIncludeHandler {
public:
  D3DIncludeBridge(ID3DInclude *inc, IDxcUtils *utils)
      : Include(inc), Utils(utils) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcIncludeHandler)) {
      *ppv = static_cast<IDxcIncludeHandler *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++RefCount; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --RefCount;
    if (r == 0)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename,
                                       IDxcBlob **ppIncludeSource) override {
    if (!Include || !Utils || !ppIncludeSource)
      return E_FAIL;
    std::string utf8;
    const int n = WideCharToMultiByte(CP_UTF8, 0, pFilename, -1, nullptr, 0,
                                      nullptr, nullptr);
    if (n > 1) {
      utf8.resize(static_cast<size_t>(n - 1));
      WideCharToMultiByte(CP_UTF8, 0, pFilename, -1, &utf8[0], n - 1, nullptr,
                          nullptr);
    }
    if (utf8.size() > 2 && utf8[0] == '.' &&
        (utf8[1] == '/' || utf8[1] == '\\'))
      utf8.erase(0, 2);

    LPCVOID data = nullptr;
    UINT bytes = 0;
    HRESULT hr = Include->Open(D3D_INCLUDE_LOCAL, utf8.c_str(), nullptr, &data,
                               &bytes);
    if (FAILED(hr))
      hr = Include->Open(D3D_INCLUDE_SYSTEM, utf8.c_str(), nullptr, &data,
                         &bytes);
    if (FAILED(hr))
      return hr;

    ComPtr<IDxcBlobEncoding> blob;
    hr = Utils->CreateBlob(data ? data : "", bytes, CP_UTF8, &blob);
    if (data)
      Include->Close(data);
    if (FAILED(hr))
      return hr;
    *ppIncludeSource = blob.Detach();
    return S_OK;
  }

private:
  std::atomic<ULONG> RefCount{1};
  ID3DInclude *Include = nullptr;
  ComPtr<IDxcUtils> Utils;
};

using PFN_GetD3DCompiler = void *(__cdecl *)(void *dispatcher);
using PFN_WrapperD3DCompile = HRESULT(__cdecl *)(
    void *self, const void *pSrcData, unsigned long srcDataSize,
    const char *pSourceName, const D3D_SHADER_MACRO *pDefines,
    ID3DInclude *pInclude, const char *pEntrypoint, const char *pTarget,
    UINT flags1, UINT flags2, ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

using PFN_dxcb5_compile_uber_dxil = int (*)(
    const char *, size_t, const dxcb5_uber_options *, dxcb5_blob *,
    dxcb5_blob *, dxcb5_blob *);
using PFN_dxcb5_specialize_pair = int (*)(
    const void *, size_t, const void *, size_t, const char *, size_t,
    dxcb5_blob *, dxcb5_blob *, dxcb5_blob *, dxcb5_blob *, dxcb5_blob *,
    dxcb5_blob *);

struct LoadedDxcb5Api {
  HMODULE DxcMod = nullptr;
  DxcCreateInstanceProc DxcCreate = nullptr;
  dxcb5_proxy::PFN_dxcb5_specialize_stage SpecStage = nullptr;
  PFN_dxcb5_compile_uber_dxil CompileUber = nullptr;
  PFN_dxcb5_specialize_pair SpecPair = nullptr;
  dxcb5_proxy::PFN_dxcb5_free_blob FreeBlob = nullptr;
  std::wstring SessionDir;
  std::wstring ProjectDir;
  uint64_t CompilerSig = 0;
  std::atomic<bool> Enabled{true};
  std::atomic<bool> NanSafe{false};
  std::atomic<bool> HasManifests{true};
};

std::once_flag gPatchOnce;
std::once_flag gApiOnce;
PFN_WrapperD3DCompile gOrigD3DCompile = nullptr;
LoadedDxcb5Api gApi;

SRWLOCK gStateFileLock = SRWLOCK_INIT;
std::string gLastStateContent;

std::wstring GetEnvW(const wchar_t *name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0)
    return {};
  std::wstring out(n, L'\0');
  DWORD got = GetEnvironmentVariableW(name, &out[0], n);
  if (got == 0 || got >= n)
    return {};
  out.resize(got);
  return out;
}

std::wstring Utf8ToWide(std::string_view s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0)
    return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

bool EqI(const char *a, const char *b) {
  return a && b && _stricmp(a, b) == 0;
}

uint64_t Fnv1a64(const void *data, size_t len,
                 uint64_t seed = 14695981039346656037ull) {
  const auto *p = static_cast<const uint8_t *>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < len; ++i)
    h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

uint64_t Fnv1a64Delim(std::string_view s, uint64_t seed) {
  uint64_t h = Fnv1a64(s.data(), s.size(), seed);
  const uint8_t sep = 0x1Fu;
  return Fnv1a64(&sep, 1, h);
}

std::wstring Hex64W(uint64_t v) {
  wchar_t buf[20];
  std::swprintf(buf, 20, L"%016llx", static_cast<unsigned long long>(v));
  return buf;
}

bool ResolveProjectRootLocal(std::wstring &outRoot) {
  auto trimDir = [](std::wstring s) {
    for (wchar_t &c : s)
      if (c == L'/')
        c = L'\\';
    while (s.size() > 3 && s.back() == L'\\')
      s.pop_back();
    return s;
  };
  if (!gApi.ProjectDir.empty()) {
    outRoot = trimDir(gApi.ProjectDir);
    return true;
  }
  const std::wstring envDir = GetEnvW(L"DXCB5_UNITY_PROJECT_DIR");
  if (!envDir.empty()) {
    outRoot = trimDir(envDir);
    return true;
  }
  wchar_t cwdBuf[4096] = {};
  const DWORD cwdN = GetCurrentDirectoryW(4096, cwdBuf);
  if (cwdN > 0 && cwdN < 4096) {
    outRoot = trimDir(std::wstring(cwdBuf, cwdN));
    return true;
  }
  return false;
}

// Lightweight dynamic toggle check so dxcb5_hook_set_enabled / set_nan_safe /
// uninstall takes effect immediately even when two writes occur within the same
// 15.6ms NTFS timestamp tick (M13).
void RefreshDynamicState() {
  if (gApi.SessionDir.empty())
    return;
  const std::wstring statePath = gApi.SessionDir + L"\\hook_state.txt";
  std::string content;
  if (!dxcb5_proxy::ReadWholeFile(statePath, content) || content.empty())
    return;
  AcquireSRWLockExclusive(&gStateFileLock);
  if (content != gLastStateContent) {
    gLastStateContent = content;
    if (content.find("enabled=0") != std::string::npos)
      gApi.Enabled.store(false, std::memory_order_release);
    else if (content.find("enabled=1") != std::string::npos)
      gApi.Enabled.store(true, std::memory_order_release);
    if (content.find("nan_safe=1") != std::string::npos)
      gApi.NanSafe.store(true, std::memory_order_release);
    else if (content.find("nan_safe=0") != std::string::npos)
      gApi.NanSafe.store(false, std::memory_order_release);
    if (content.find("manifests=0") != std::string::npos)
      gApi.HasManifests.store(false, std::memory_order_release);
    else if (content.find("manifests=1") != std::string::npos)
      gApi.HasManifests.store(true, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&gStateFileLock);
}

uint64_t MixModuleFileStamp(HMODULE mod, uint64_t seed) {
  if (!mod)
    return seed;
  wchar_t path[4096] = {};
  const DWORD n = GetModuleFileNameW(mod, path, 4096);
  if (n == 0 || n >= 4096)
    return seed;
  WIN32_FILE_ATTRIBUTE_DATA attr{};
  if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr))
    return seed;
  seed = Fnv1a64(&attr.nFileSizeLow, sizeof(attr.nFileSizeLow), seed);
  seed = Fnv1a64(&attr.nFileSizeHigh, sizeof(attr.nFileSizeHigh), seed);
  seed = Fnv1a64(&attr.ftLastWriteTime, sizeof(attr.ftLastWriteTime), seed);
  return seed;
}

void EnsureApiLoaded() {
  std::call_once(gApiOnce, [] {
    const std::wstring en = GetEnvW(L"DXCB5_HOOK_ENABLED");
    if (en == L"0")
      gApi.Enabled.store(false, std::memory_order_release);
    const std::wstring ns = GetEnvW(L"DXCB5_NAN_SAFE");
    if (ns == L"1")
      gApi.NanSafe.store(true, std::memory_order_release);
    gApi.SessionDir = GetEnvW(L"DXCB5_HOOK_SESSION_DIR");
    gApi.ProjectDir = GetEnvW(L"DXCB5_UNITY_PROJECT_DIR");
    std::wstring dllPath = GetEnvW(L"DXCB5_DXCOMPILER_PATH");
    if (dllPath.empty())
      dllPath = L"dxcompiler.dll";
    gApi.DxcMod = LoadLibraryW(dllPath.c_str());
    if (!gApi.DxcMod)
      return;
    HMODULE selfMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&EnsureApiLoaded), &selfMod);
    uint64_t sig = 14695981039346656037ull;
    sig = MixModuleFileStamp(gApi.DxcMod, sig);
    sig = MixModuleFileStamp(selfMod, sig);
    gApi.CompilerSig = sig;
    gApi.DxcCreate = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(gApi.DxcMod, "DxcCreateInstance"));
    gApi.SpecStage = reinterpret_cast<dxcb5_proxy::PFN_dxcb5_specialize_stage>(
        GetProcAddress(gApi.DxcMod, "dxcb5_specialize_stage"));
    gApi.CompileUber = reinterpret_cast<PFN_dxcb5_compile_uber_dxil>(
        GetProcAddress(gApi.DxcMod, "dxcb5_compile_uber_dxil"));
    gApi.SpecPair = reinterpret_cast<PFN_dxcb5_specialize_pair>(
        GetProcAddress(gApi.DxcMod, "dxcb5_specialize_pair"));
    gApi.FreeBlob = reinterpret_cast<dxcb5_proxy::PFN_dxcb5_free_blob>(
        GetProcAddress(gApi.DxcMod, "dxcb5_free_blob"));
  });
  RefreshDynamicState();
}

bool MakeBlobFromBytes(const void *data, size_t size, ID3DBlob **ppCode) {
  if (!data || !size || !ppCode)
    return false;
  std::vector<uint8_t> copy(static_cast<const uint8_t *>(data),
                            static_cast<const uint8_t *>(data) + size);
  *ppCode = new Dxcb5MemBlob(std::move(copy));
  return true;
}

bool TryServeSpecStub(const void *pSrcData, size_t srcLen,
                      const D3D_SHADER_MACRO *pDefines, const char *pTarget,
                      UINT flags1, bool nanSafe, ID3DBlob **ppCode,
                      bool &outIsSpecStub) {
  outIsSpecStub = false;
  const bool isPs = EqI(pTarget, "ps_5_0");
  const bool isVs = EqI(pTarget, "vs_5_0");
  if (!isPs && !isVs)
    return false;
  dxcb5_proxy::PreprocMacroOracle oracle;
  oracle.SetFromDefines(pDefines);
  oracle.UnknownAsFalse = true;
  dxcb5_proxy::SpecStubDirective dir;
  if (!dxcb5_proxy::ResolveSpecStubDirective(static_cast<const char *>(pSrcData),
                                             srcLen, std::move(oracle), dir))
    return false;
  outIsSpecStub = true;

  std::wstring projectRoot, cacheRoot;
  if (!ResolveProjectRootLocal(projectRoot) ||
      !dxcb5_proxy::ResolveSpecCacheDir(Utf8ToWide(dir.CacheDir), projectRoot,
                                        cacheRoot))
    return false;

  dxcb5_proxy::ActiveDefineMap active;
  dxcb5_proxy::CollectActiveDefines(pDefines, static_cast<const char *>(pSrcData),
                                    srcLen, active);
  const std::wstring baseDir = cacheRoot + L"\\" + Utf8ToWide(dir.PairKey) +
                               L"\\" + dxcb5_proxy::DetectArchetypeDir(active);
  std::string metaJson;
  if (!dxcb5_proxy::ReadWholeFile(baseDir + L"\\meta.json", metaJson) ||
      metaJson.empty())
    return false;
  std::vector<std::string> specFlags;
  dxcb5_proxy::ParseSpecFlagsList(
      metaJson, specFlags,
      isPs ? "\"ps_live_spec_flags\"" : "\"vs_live_spec_flags\"");
  std::vector<std::pair<std::string, uint32_t>> kv;
  kv.reserve(specFlags.size());
  for (const auto &f : specFlags)
    kv.emplace_back(f, active.IsSet(f.c_str()) ? 1u : 0u);
  const std::wstring defHash = dxcb5_proxy::HashDefinesHex(kv);
  const std::wstring stage = isPs ? L"ps" : L"vs";
  const std::wstring flagsSuf = dxcb5_proxy::BuildSpecFlagsSuffix(flags1, nanSafe);
  const std::wstring dxbcPath =
      baseDir + L"\\" + stage + L"_" + defHash + flagsSuf + L".dxbc";

  std::string dxbc;
  if (dxcb5_proxy::ReadWholeFile(dxbcPath, dxbc) &&
      dxcb5_is_valid_dxbc_container_header(dxbc.data(), dxbc.size()))
    return MakeBlobFromBytes(dxbc.data(), dxbc.size(), ppCode);

  if (!gApi.SpecStage || !gApi.FreeBlob)
    return false;
  std::string optDxil;
  if (!dxcb5_proxy::ReadWholeFile(baseDir + L"\\" + stage + L".opt.dxil",
                                  optDxil) ||
      optDxil.empty())
    return false;
  const std::string manifest = dxcb5_proxy::BuildDefinesManifestJson(kv, nanSafe);
  dxcb5_blob outBlob{}, errBlob{};
  const int rc = gApi.SpecStage(optDxil.data(), optDxil.size(), manifest.data(),
                                manifest.size(), &outBlob, nullptr, &errBlob);
  gApi.FreeBlob(&errBlob);
  if (rc != 0 || !outBlob.data || !outBlob.size) {
    gApi.FreeBlob(&outBlob);
    return false;
  }
  dxcb5_proxy::WriteFileAtomic(dxbcPath, outBlob.data, outBlob.size);
  const bool ok = MakeBlobFromBytes(outBlob.data, outBlob.size, ppCode);
  gApi.FreeBlob(&outBlob);
  return ok;
}

std::string NormalizeAssetPathLocal(std::string_view p) {
  std::string out;
  out.reserve(p.size());
  for (char c : p) {
    if (c == '\\')
      out.push_back('/');
    else
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  while (out.size() >= 2 && out[0] == '.' && out[1] == '/')
    out.erase(0, 2);
  return out;
}

bool TryLoadSessionManifestForNormalized(const std::string &norm,
                                         std::string &outManifestJson,
                                         std::string &outPairEntries) {
  if (norm.empty() || gApi.SessionDir.empty())
    return false;
  const std::wstring ctxPath =
      gApi.SessionDir + L"\\manifest_" +
      Hex64W(Fnv1a64(norm.data(), norm.size())) + L".json";
  std::string raw;
  if (!dxcb5_proxy::ReadWholeFile(ctxPath, raw) || raw.empty())
    return false;
  while (!raw.empty() &&
         (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' ||
          raw.back() == '\n'))
    raw.pop_back();
  dxcb5_proxy::ExtractJsonStringField(raw, "pair", outPairEntries);
  const size_t mPos = raw.find("\"manifest\":");
  if (mPos == std::string::npos)
    return false;
  const size_t objStart = raw.find('{', mPos + 11);
  if (objStart == std::string::npos || raw.size() <= objStart + 1 ||
      raw.back() != '}')
    return false;
  outManifestJson = raw.substr(objStart, raw.size() - objStart - 1);
  return !outManifestJson.empty();
}

bool TryLoadSessionManifestByPath(std::string_view rawPath,
                                  std::string &outManifestJson,
                                  std::string &outPairEntries) {
  const std::string norm = NormalizeAssetPathLocal(rawPath);
  if (TryLoadSessionManifestForNormalized(norm, outManifestJson, outPairEntries))
    return true;
  // Unity #line directives often carry absolute paths ("E:/Proj/Assets/...").
  // Also try project-relative suffixes starting at "assets/" or "packages/".
  const size_t aPos = norm.find("/assets/");
  if (aPos != std::string::npos &&
      TryLoadSessionManifestForNormalized(norm.substr(aPos + 1),
                                          outManifestJson, outPairEntries))
    return true;
  const size_t pPos = norm.find("/packages/");
  if (pPos != std::string::npos &&
      TryLoadSessionManifestForNormalized(norm.substr(pPos + 1),
                                          outManifestJson, outPairEntries))
    return true;
  return false;
}

bool ExtractManifestContext(const char *pSourceName, std::string_view src,
                            std::string &outManifestJson,
                            std::string &outPairEntries) {
  outManifestJson.clear();
  outPairEntries.clear();
  // 1. Check session registry published by dxcb5_unity_hook.dll.
  if (!gApi.SessionDir.empty() &&
      gApi.HasManifests.load(std::memory_order_acquire)) {
    if (pSourceName && *pSourceName &&
        TryLoadSessionManifestByPath(pSourceName, outManifestJson,
                                     outPairEntries))
      return true;
    // UnityShaderCompiler.exe!InvokeCompilerHLSL always passes pSourceName == ""
    // to D3DCompile and embeds the shader asset path in `#line <N> "<path>"`!
    std::vector<std::string> seen;
    seen.reserve(16);
    size_t pos = 0;
    while (pos < src.size() && seen.size() < 16) {
      const size_t lp = src.find("#line", pos);
      if (lp == std::string_view::npos)
        break;
      const size_t nl = src.find('\n', lp);
      const size_t lineEnd = (nl == std::string_view::npos) ? src.size() : nl;
      pos = (nl == std::string_view::npos) ? src.size() : nl + 1;
      const size_t q0 = src.find('"', lp);
      if (q0 == std::string_view::npos || q0 >= lineEnd)
        continue;
      const size_t q1 = src.find('"', q0 + 1);
      if (q1 == std::string_view::npos || q1 >= lineEnd || q1 <= q0 + 1)
        continue;
      std::string_view pathTok = src.substr(q0 + 1, q1 - q0 - 1);
      bool dup = false;
      for (const auto &prev : seen) {
        if (prev == pathTok) {
          dup = true;
          break;
        }
      }
      if (dup)
        continue;
      seen.emplace_back(pathTok);
      if (TryLoadSessionManifestByPath(pathTok, outManifestJson, outPairEntries))
        return true;
    }
  }
  return false;
}

uint32_t MapFlags1ToUberFlags(UINT flags1, bool nanSafe) {
  uint32_t f = DXCB5_UBER_FLAG_UNITY_ADAPT;
  if (nanSafe)
    f |= DXCB5_UBER_FLAG_NAN_SAFE;
  if (flags1 & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR)
    f |= DXCB5_UBER_FLAG_ROW_MAJOR;
  if (flags1 & D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR)
    f |= DXCB5_UBER_FLAG_COL_MAJOR;
  if (flags1 & D3DCOMPILE_IEEE_STRICTNESS)
    f |= DXCB5_UBER_FLAG_IEEE_STRICT;
  if (flags1 & D3DCOMPILE_SKIP_OPTIMIZATION)
    f |= DXCB5_UBER_FLAG_SKIP_OPT;
  return f;
}

int CompileUberWithGecFallback(const char *srcStr, size_t srcLen,
                               dxcb5_uber_options opts, UINT flags1,
                               dxcb5_blob &outDxil) {
  dxcb5_blob err{};
  int rc = gApi.CompileUber(srcStr, srcLen, &opts, &outDxil, nullptr, &err);
  gApi.FreeBlob(&err);
  if (rc != 0 && (flags1 & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)) {
    gApi.FreeBlob(&outDxil);
    opts.hlsl_version = "2016";
    opts.flags |= DXCB5_UBER_FLAG_GEC;
    rc = gApi.CompileUber(srcStr, srcLen, &opts, &outDxil, nullptr, &err);
    gApi.FreeBlob(&err);
  }
  return rc;
}

bool IsJsonBoolOptionTrue(const std::string &json, const char *key) {
  const std::string pat = std::string("\"") + key + "\"";
  const size_t pos = json.find(pat);
  if (pos == std::string::npos)
    return false;
  size_t i = json.find(':', pos + pat.size());
  if (i == std::string::npos)
    return false;
  ++i;
  while (i < json.size() &&
         (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' ||
          json[i] == '\n'))
    ++i;
  return json.compare(i, 4, "true") == 0;
}

bool HasUnpreprocessedStageConditionals(std::string_view srcView) {
  static constexpr const char *kStageMacros[] = {
      "SHADER_STAGE_VERTEX", "SHADER_STAGE_FRAGMENT",
      "UNITY_STAGE_VERTEX",  "UNITY_STAGE_FRAGMENT"};
  size_t pos = 0;
  while (pos < srcView.size()) {
    const size_t nl = srcView.find('\n', pos);
    const size_t end = (nl == std::string_view::npos) ? srcView.size() : nl;
    std::string_view line = srcView.substr(pos, end - pos);
    pos = (nl == std::string_view::npos) ? srcView.size() : nl + 1;
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      ++i;
    if (i >= line.size() || line[i] != '#')
      continue;
    ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      ++i;
    if (line.substr(i).rfind("if", 0) != 0 &&
        line.substr(i).rfind("elif", 0) != 0)
      continue;
    for (const char *m : kStageMacros) {
      if (line.find(m, i) != std::string_view::npos)
        return true;
    }
  }
  return false;
}

std::vector<dxcb5_define> MakeStageDefines(const std::vector<dxcb5_define> &base,
                                           bool isPs) {
  std::vector<dxcb5_define> out;
  out.reserve(base.size() + 1);
  for (const auto &d : base) {
    if (!d.name)
      continue;
    if (std::strcmp(d.name, "SHADER_STAGE_VERTEX") == 0 ||
        std::strcmp(d.name, "SHADER_STAGE_FRAGMENT") == 0)
      continue;
    out.push_back(d);
  }
  out.push_back({isPs ? "SHADER_STAGE_FRAGMENT" : "SHADER_STAGE_VERTEX", "1"});
  return out;
}

std::string InjectNanSafeOptionIfNeeded(const std::string &manifestJson,
                                        bool nanSafe) {
  if (!nanSafe || manifestJson.find("\"nan_safe\"") != std::string::npos)
    return manifestJson;
  auto isWs = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  const size_t optPos = manifestJson.find("\"options\"");
  if (optPos != std::string::npos) {
    const size_t brace = manifestJson.find('{', optPos);
    if (brace != std::string::npos) {
      size_t next = brace + 1;
      while (next < manifestJson.size() && isWs(manifestJson[next]))
        ++next;
      const bool emptyObj = (next < manifestJson.size() && manifestJson[next] == '}');
      std::string out = manifestJson;
      out.insert(brace + 1, emptyObj ? "\"nan_safe\":true" : "\"nan_safe\":true,");
      return out;
    }
  }
  const size_t endBrace = manifestJson.rfind('}');
  if (endBrace != std::string::npos) {
    size_t prev = endBrace;
    while (prev > 0 && isWs(manifestJson[prev - 1]))
      --prev;
    const bool emptyRoot = (prev > 0 && manifestJson[prev - 1] == '{');
    std::string out = manifestJson;
    out.insert(endBrace, emptyRoot ? "\"options\":{\"nan_safe\":true}"
                                   : ",\"options\":{\"nan_safe\":true}");
    return out;
  }
  return manifestJson;
}

bool TryServeManifestMultiPass(const void *pSrcData, size_t srcLen,
                               const char *pSourceName,
                               const D3D_SHADER_MACRO *pDefines,
                               ID3DInclude *pInclude, const char *pEntrypoint,
                               const char *pTarget, UINT flags1, bool nanSafe,
                               ID3DBlob **ppCode) {
  if (!gApi.CompileUber || !gApi.SpecStage || !gApi.SpecPair || !gApi.FreeBlob)
    return false;
  // Custom ID3DInclude is not bridged through dxcb5_compile_uber_dxil; defer to
  // TryCompileDirectDxcb5 so included headers are never missed or mis-cached.
  if (pInclude != nullptr)
    return false;
  const bool isPs = EqI(pTarget, "ps_5_0");
  const bool isVs = EqI(pTarget, "vs_5_0");
  if (!isPs && !isVs)
    return false;

  const std::string_view srcView(static_cast<const char *>(pSrcData), srcLen);
  std::string rawManifestJson, pairEntries;
  if (!ExtractManifestContext(pSourceName, srcView, rawManifestJson,
                              pairEntries))
    return false;
  const std::string manifestJson =
      InjectNanSafeOptionIfNeeded(rawManifestJson, nanSafe);

  std::vector<dxcb5_define> defs;
  if (pDefines) {
    for (const D3D_SHADER_MACRO *m = pDefines; m->Name; ++m)
      defs.push_back({m->Name, m->Definition ? m->Definition : ""});
  }

  const char *entry = (pEntrypoint && *pEntrypoint) ? pEntrypoint : "main";
  const char *srcName = (pSourceName && *pSourceName) ? pSourceName : "unity.hlsl";
  const uint8_t nsByte = nanSafe ? 1u : 0u;

  dxcb5::Sha256 h;
  const uint32_t epoch = DXCB5_CACHE_EPOCH;
  h.UpdatePod(epoch);
  h.UpdatePod(gApi.CompilerSig);
  h.UpdateDelim(srcView);
  h.UpdateDelim(srcName);
  h.UpdateDelim(entry);
  h.UpdateDelim(pTarget);
  h.UpdatePod(flags1);
  h.UpdatePod(nsByte);
  h.UpdateDelim(manifestJson);
  h.UpdateDelim(pairEntries);
  const uint64_t defCount = static_cast<uint64_t>(defs.size());
  h.UpdatePod(defCount);
  for (const auto &d : defs) {
    h.UpdateDelim(d.name);
    h.UpdateDelim(d.value);
  }
  const std::string keyHex = h.Hex32();
  const std::wstring keyHexW(keyHex.begin(), keyHex.end());

  std::string vsEntry, psEntry;
  const size_t colon = pairEntries.find(':');
  if (colon != std::string::npos && colon > 0 && colon + 1 < pairEntries.size()) {
    vsEntry = pairEntries.substr(0, colon);
    psEntry = pairEntries.substr(colon + 1);
  }
  const bool wantPair =
      !vsEntry.empty() && !psEntry.empty() &&
      ((isVs && vsEntry == entry) || (isPs && psEntry == entry));

  // Only cache the exact stage requested by THIS call under THIS call's source +
  // defines + entrypoint + flags1 + nanSafe key (never write the opposite stage
  // under a stage-preprocessed caller's key).
  std::wstring stageCachePath;
  if (!gApi.SessionDir.empty()) {
    const std::wstring cacheDir = gApi.SessionDir + L"\\manifest_dxbc";
    CreateDirectoryW(gApi.SessionDir.c_str(), nullptr);
    CreateDirectoryW(cacheDir.c_str(), nullptr);
    stageCachePath =
        cacheDir + L"\\" + keyHexW + (isPs ? L"_ps.dxbc" : L"_vs.dxbc");
    std::string cached;
    if (dxcb5_proxy::ReadWholeFile(stageCachePath, cached) &&
        dxcb5_is_valid_dxbc_container_header(cached.data(), cached.size()))
      return MakeBlobFromBytes(cached.data(), cached.size(), ppCode);
  }

  const char *srcStr = static_cast<const char *>(pSrcData);
  const uint32_t uberFlags = MapFlags1ToUberFlags(flags1, nanSafe);

  if (wantPair) {
    const std::string &otherEntry = isVs ? psEntry : vsEntry;
    if (srcView.find(otherEntry) != std::string_view::npos &&
        !HasUnpreprocessedStageConditionals(srcView)) {
      const auto vsDefs = MakeStageDefines(defs, false);
      const auto psDefs = MakeStageDefines(defs, true);
      dxcb5_uber_options vsO{vsEntry.c_str(), "vs_5_0", "2018", srcName,
                             nullptr, 0, vsDefs.data(),
                             static_cast<uint32_t>(vsDefs.size()), uberFlags};
      dxcb5_uber_options psO{psEntry.c_str(), "ps_5_0", "2018", srcName,
                             nullptr, 0, psDefs.data(),
                             static_cast<uint32_t>(psDefs.size()), uberFlags};
      dxcb5_blob vsDxil{}, psDxil{};
      const int rc1 = CompileUberWithGecFallback(srcStr, srcLen, vsO, flags1, vsDxil);
      const int rc2 =
          rc1 == 0 ? CompileUberWithGecFallback(srcStr, srcLen, psO, flags1, psDxil)
                   : -1;
      if (rc1 == 0 && rc2 == 0 && vsDxil.data && psDxil.data) {
        dxcb5_blob vsDxbc{}, psDxbc{}, err{};
        const int prc = gApi.SpecPair(
            vsDxil.data, vsDxil.size, psDxil.data, psDxil.size,
            manifestJson.data(), manifestJson.size(), nullptr, nullptr, &vsDxbc,
            &psDxbc, nullptr, &err);
        gApi.FreeBlob(&vsDxil);
        gApi.FreeBlob(&psDxil);
        gApi.FreeBlob(&err);
        if (prc == 0 && vsDxbc.data && psDxbc.data) {
          const dxcb5_blob &chosen = isPs ? psDxbc : vsDxbc;
          if (!stageCachePath.empty())
            dxcb5_proxy::WriteFileAtomic(stageCachePath, chosen.data, chosen.size);
          const bool ok = MakeBlobFromBytes(chosen.data, chosen.size, ppCode);
          gApi.FreeBlob(&vsDxbc);
          gApi.FreeBlob(&psDxbc);
          return ok;
        }
        gApi.FreeBlob(&vsDxbc);
        gApi.FreeBlob(&psDxbc);
      } else {
        gApi.FreeBlob(&vsDxil);
        gApi.FreeBlob(&psDxil);
      }
    }
    // If pairing could not run (e.g. opposite entrypoint stripped), fall through
    // to single-stage specialization unless compact_globals is enabled.
  }

  // Single-stage manifest specialization (skip if compact_globals is true
  // without a VS:PS pair to prevent VS/PS $Globals offset collisions!).
  if (IsJsonBoolOptionTrue(manifestJson, "compact_globals"))
    return false;

  dxcb5_uber_options stO{entry, pTarget, "2018", srcName, nullptr, 0,
                         defs.empty() ? nullptr : defs.data(),
                         static_cast<uint32_t>(defs.size()), uberFlags};
  dxcb5_blob dxil{}, dxbc{}, err{};
  if (CompileUberWithGecFallback(srcStr, srcLen, stO, flags1, dxil) != 0 ||
      !dxil.data) {
    gApi.FreeBlob(&dxil);
    return false;
  }
  const int srcRc = gApi.SpecStage(dxil.data, dxil.size, manifestJson.data(),
                                   manifestJson.size(), &dxbc, nullptr, &err);
  gApi.FreeBlob(&dxil);
  gApi.FreeBlob(&err);
  if (srcRc != 0 || !dxbc.data || !dxbc.size) {
    gApi.FreeBlob(&dxbc);
    return false;
  }
  if (!stageCachePath.empty())
    dxcb5_proxy::WriteFileAtomic(stageCachePath, dxbc.data, dxbc.size);
  const bool ok = MakeBlobFromBytes(dxbc.data, dxbc.size, ppCode);
  gApi.FreeBlob(&dxbc);
  return ok;
}

bool TryCompileDirectDxcb5(const void *pSrcData, size_t srcLen,
                           const char *pSourceName,
                           const D3D_SHADER_MACRO *pDefines,
                           ID3DInclude *pInclude, const char *pEntrypoint,
                           const char *pTarget, UINT flags1, bool nanSafe,
                           ID3DBlob **ppCode) {
  if (!gApi.DxcCreate)
    return false;
  const bool isPs = EqI(pTarget, "ps_5_0");
  const bool isVs = EqI(pTarget, "vs_5_0");
  if (!isPs && !isVs)
    return false;

  // Content-addressed disk dedup cache for repeated identical direct compiles.
  // Bypass disk cache whenever pInclude != nullptr since included headers may change.
  std::wstring cacheFile;
  if (!gApi.SessionDir.empty() && pInclude == nullptr) {
    const uint8_t nsByte = nanSafe ? 1u : 0u;
    const uint32_t epoch = DXCB5_CACHE_EPOCH;
    dxcb5::Sha256 h;
    h.UpdatePod(epoch);
    h.UpdatePod(gApi.CompilerSig);
    h.UpdateDelim(std::string_view(static_cast<const char *>(pSrcData), srcLen));
    h.UpdateDelim(pSourceName ? pSourceName : "");
    h.UpdateDelim(pEntrypoint ? pEntrypoint : "");
    h.UpdateDelim(pTarget);
    h.UpdatePod(flags1);
    h.UpdatePod(nsByte);
    if (pDefines) {
      for (const D3D_SHADER_MACRO *m = pDefines; m->Name; ++m) {
        h.UpdateDelim(m->Name);
        h.UpdateDelim(m->Definition ? m->Definition : "");
      }
    }
    const std::string keyHex = h.Hex32();
    const std::wstring keyHexW(keyHex.begin(), keyHex.end());
    const std::wstring dir = gApi.SessionDir + L"\\direct_dxbc";
    CreateDirectoryW(gApi.SessionDir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    cacheFile = dir + L"\\" + keyHexW + L".dxbc";
    std::string cached;
    if (dxcb5_proxy::ReadWholeFile(cacheFile, cached) &&
        dxcb5_is_valid_dxbc_container_header(cached.data(), cached.size()))
      return MakeBlobFromBytes(cached.data(), cached.size(), ppCode);
  }

  ComPtr<IDxcCompiler3> compiler;
  ComPtr<IDxcUtils> utils;
  if (FAILED(gApi.DxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
      FAILED(gApi.DxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&utils))))
    return false;

  std::string adapted;
  const char *srcPtr = static_cast<const char *>(pSrcData);
  size_t len = srcLen;
  if (dxcb5_proxy::AdaptUnityHlslForDxc(srcPtr, len, adapted, pEntrypoint, isPs)) {
    srcPtr = adapted.data();
    len = adapted.size();
  }

  std::vector<std::wstring> args = {L"-fdxcb5", L"-HV", L"2018", L"-T",
                                    Utf8ToWide(pTarget), L"-E",
                                    Utf8ToWide(pEntrypoint ? pEntrypoint : "main")};
  if (nanSafe)
    args.push_back(L"--nan-safe");
  if (flags1 & D3DCOMPILE_SKIP_OPTIMIZATION) {
    args.push_back(L"-Od");
  } else {
    const UINT optBits = flags1 & D3DCOMPILE_OPTIMIZATION_LEVEL2;
    if (optBits == D3DCOMPILE_OPTIMIZATION_LEVEL0)
      args.push_back(L"-O0");
    else if (optBits == D3DCOMPILE_OPTIMIZATION_LEVEL1)
      args.push_back(L"-O1");
    else if (optBits == D3DCOMPILE_OPTIMIZATION_LEVEL2)
      args.push_back(L"-O2");
    else
      args.push_back(L"-O3");
  }
  if (flags1 & D3DCOMPILE_IEEE_STRICTNESS)
    args.push_back(L"-Gis");
  if (flags1 & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR)
    args.push_back(L"-Zpr");
  if (flags1 & D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR)
    args.push_back(L"-Zpc");
  if (flags1 & D3DCOMPILE_AVOID_FLOW_CONTROL)
    args.push_back(L"-Gfa");
  if (flags1 & D3DCOMPILE_PREFER_FLOW_CONTROL)
    args.push_back(L"-Gfp");
  if (pDefines) {
    for (const D3D_SHADER_MACRO *m = pDefines; m->Name; ++m) {
      std::string def = m->Name;
      if (m->Definition && *m->Definition) {
        def += '=';
        def += m->Definition;
      }
      args.push_back(L"-D");
      args.push_back(Utf8ToWide(def));
    }
  }
  if (pSourceName && *pSourceName)
    args.push_back(Utf8ToWide(pSourceName));

  ComPtr<IDxcIncludeHandler> inc;
  if (pInclude == D3D_COMPILE_STANDARD_FILE_INCLUDE)
    utils->CreateDefaultIncludeHandler(&inc);
  else if (pInclude)
    inc.Attach(new D3DIncludeBridge(pInclude, utils.Get()));

  DxcBuffer buf{srcPtr, len, CP_UTF8};
  auto run = [&](ComPtr<IDxcResult> &res, HRESULT &st) {
    std::vector<LPCWSTR> ptrs;
    ptrs.reserve(args.size());
    for (const auto &a : args)
      ptrs.push_back(a.c_str());
    res.Reset();
    st = E_FAIL;
    if (FAILED(compiler->Compile(&buf, ptrs.data(),
                                 static_cast<UINT32>(ptrs.size()), inc.Get(),
                                 IID_PPV_ARGS(&res))) ||
        !res)
      return false;
    res->GetStatus(&st);
    return true;
  };

  ComPtr<IDxcResult> res;
  HRESULT st = E_FAIL;
  if (!run(res, st))
    return false;
  if (FAILED(st) && (flags1 & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)) {
    args[2] = L"2016";
    args.insert(args.begin() + 3, L"-Gec");
    ComPtr<IDxcResult> retry;
    HRESULT retrySt = E_FAIL;
    if (run(retry, retrySt)) {
      res = std::move(retry);
      st = retrySt;
    }
  }
  if (FAILED(st))
    return false;
  ComPtr<IDxcBlob> obj;
  if (FAILED(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) ||
      !obj || !obj->GetBufferSize())
    return false;
  if (!cacheFile.empty())
    dxcb5_proxy::WriteFileAtomic(cacheFile, obj->GetBufferPointer(),
                                 obj->GetBufferSize());
  return MakeBlobFromBytes(obj->GetBufferPointer(), obj->GetBufferSize(), ppCode);
}

HRESULT __cdecl HookedD3DCompile(
    void *self, const void *pSrcData, unsigned long srcDataSize,
    const char *pSourceName, const D3D_SHADER_MACRO *pDefines,
    ID3DInclude *pInclude, const char *pEntrypoint, const char *pTarget,
    UINT flags1, UINT flags2, ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs) {
  EnsureApiLoaded();
  const bool enabled = gApi.Enabled.load(std::memory_order_acquire);
  const bool nanSafe = gApi.NanSafe.load(std::memory_order_acquire);
  if (enabled && pSrcData && srcDataSize && ppCode) {
    bool isSpecStub = false;
    if (TryServeSpecStub(pSrcData, srcDataSize, pDefines, pTarget, flags1,
                         nanSafe, ppCode, isSpecStub)) {
      if (ppErrorMsgs)
        *ppErrorMsgs = nullptr;
      return S_OK;
    }
    if (!isSpecStub) {
      if (TryServeManifestMultiPass(pSrcData, srcDataSize, pSourceName, pDefines,
                                    pInclude, pEntrypoint, pTarget, flags1,
                                    nanSafe, ppCode)) {
        if (ppErrorMsgs)
          *ppErrorMsgs = nullptr;
        return S_OK;
      }
      if (TryCompileDirectDxcb5(pSrcData, srcDataSize, pSourceName, pDefines,
                                pInclude, pEntrypoint, pTarget, flags1, nanSafe,
                                ppCode)) {
        if (ppErrorMsgs)
          *ppErrorMsgs = nullptr;
        return S_OK;
      }
    }
  }
  return gOrigD3DCompile(self, pSrcData, srcDataSize, pSourceName, pDefines,
                         pInclude, pEntrypoint, pTarget, flags1, flags2, ppCode,
                         ppErrorMsgs);
}

} // namespace

extern "C" __declspec(dllexport) void *GetPluginInterface(int apiVersion,
                                                          void *dispatcher) {
  if (apiVersion != 2 || !dispatcher)
    return nullptr;
  std::call_once(gPatchOnce, [dispatcher] {
    // Pin this DLL permanently in UnityShaderCompiler.exe so returning nullptr
    // from GetPluginInterface can never cause FreeLibrary to unmap HookedD3DCompile.
    HMODULE selfMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&GetPluginInterface),
                       &selfMod);

    auto **dispVtbl = *reinterpret_cast<void ***>(dispatcher);
    if (!dispVtbl || !dispVtbl[0])
      return;
    auto getD3D = reinterpret_cast<PFN_GetD3DCompiler>(dispVtbl[0]);
    void *d3d = getD3D(dispatcher);
    if (!d3d)
      return;
    auto **origVtbl = *reinterpret_cast<void ***>(d3d);
    if (!origVtbl || !origVtbl[3])
      return;

    // Patch D3DCompilerWrapper vtable slot 3 (D3DCompile) in-place:
    // preserves origVtbl[-1] MSVC RTTI and ALL virtual slots across Unity builds.
    gOrigD3DCompile = reinterpret_cast<PFN_WrapperD3DCompile>(origVtbl[3]);
    DWORD oldProt = 0;
    if (VirtualProtect(&origVtbl[3], sizeof(void *), PAGE_READWRITE, &oldProt)) {
      InterlockedExchangePointer(&origVtbl[3],
                                 reinterpret_cast<void *>(&HookedD3DCompile));
      DWORD dummy = 0;
      VirtualProtect(&origVtbl[3], sizeof(void *), oldProt, &dummy);
    }
  });
  return nullptr;
}
