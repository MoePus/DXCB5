// Distributed under the University of Illinois Open Source License.
#include "NativeBridge.h"
#include <wrl/client.h>
#include "UnityHlslAdapter.h"
#include "dxc/dxcapi.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using PFN_D3DCompile = HRESULT(WINAPI *)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
    LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
using PFN_D3DCompile2 = HRESULT(WINAPI *)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
    LPCSTR, UINT, UINT, UINT, LPCVOID, SIZE_T, ID3DBlob **, ID3DBlob **);
using PFN_D3DCreateBlob = HRESULT(WINAPI *)(SIZE_T, ID3DBlob **);
using PFN_DxcCreateInstance = HRESULT(WINAPI *)(REFCLSID, REFIID, LPVOID *);

struct ProxyConfig {
  bool Enabled = true;
  bool NanSafe = false;
  bool LogStats = true;
  bool RequirePackage = true;
};

std::wstring GetModuleDirectory() {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&GetModuleDirectory), &mod);
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
  std::wstring path(buf, n);
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring GetConfigDir() {
  wchar_t buf[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableW(L"DXCB5_UNITY_CONFIG_DIR", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
    return std::wstring(buf, n);
  n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
    return std::wstring(buf, n) + L"\\DXCB5Unity";
  n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
    return std::wstring(buf, n) + L"\\AppData\\Local\\DXCB5Unity";
  return L".";
}

ProxyConfig LoadConfig() {
  ProxyConfig cfg;
  const std::wstring iniPath = GetConfigDir() + L"\\config.ini";
  cfg.Enabled =
      GetPrivateProfileIntW(L"DXCB5", L"enabled", 1, iniPath.c_str()) != 0;
  cfg.NanSafe =
      GetPrivateProfileIntW(L"DXCB5", L"nan_safe", 0, iniPath.c_str()) != 0;
  cfg.LogStats =
      GetPrivateProfileIntW(L"DXCB5", L"log_stats", 1, iniPath.c_str()) != 0;
  cfg.RequirePackage =
      GetPrivateProfileIntW(L"DXCB5", L"require_package", 1, iniPath.c_str()) !=
      0;
  wchar_t envBuf[16] = {};
  if (GetEnvironmentVariableW(L"DXCB5_UNITY_ENABLE", envBuf, 16) > 0)
    cfg.Enabled = (envBuf[0] != L'0');
  return cfg;
}

bool DirExists(const std::wstring &path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::wstring &path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileContainsAscii(const std::wstring &path, const char *needle) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER sz{};
  bool found = false;
  if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 1024 * 1024) {
    std::string buf(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD read = 0;
    if (ReadFile(h, &buf[0], static_cast<DWORD>(buf.size()), &read, nullptr)) {
      buf.resize(read);
      found = buf.find(needle) != std::string::npos;
    }
  }
  CloseHandle(h);
  return found;
}

bool ProjectHasDxcb5Package(const std::wstring &projectRoot) {
  if (FileExists(projectRoot + L"\\Packages\\com.moepus.dxcb5\\package.json"))
    return true;
  if (FileContainsAscii(projectRoot + L"\\Packages\\vpm-manifest.json",
                        "\"com.moepus.dxcb5\""))
    return true;
  if (FileContainsAscii(projectRoot + L"\\Packages\\manifest.json",
                        "\"com.moepus.dxcb5\""))
    return true;
  return false;
}

bool FindUnityProjectRoot(std::wstring &outRoot) {
  wchar_t buf[4096] = {};
  DWORD n = GetEnvironmentVariableW(L"DXCB5_UNITY_PROJECT_DIR", buf, 4096);
  if (n > 0 && n < 4096) {
    std::wstring cand(buf, n);
    while (!cand.empty() && (cand.back() == L'\\' || cand.back() == L'/'))
      cand.pop_back();
    if (DirExists(cand + L"\\Packages")) {
      outRoot = std::move(cand);
      return true;
    }
  }
  n = GetCurrentDirectoryW(4096, buf);
  if (n > 0 && n < 4096) {
    std::wstring cand(buf, n);
    while (!cand.empty() && (cand.back() == L'\\' || cand.back() == L'/'))
      cand.pop_back();
    if (DirExists(cand + L"\\Packages")) {
      outRoot = std::move(cand);
      return true;
    }
  }
  return false;
}

bool IsUnityShaderCompilerProcess() {
  wchar_t exePath[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  if (!n || n >= MAX_PATH)
    return false;
  std::wstring s(exePath, n);
  const size_t slash = s.find_last_of(L"\\/");
  std::wstring name = slash == std::wstring::npos ? s : s.substr(slash + 1);
  return _wcsicmp(name.c_str(), L"UnityShaderCompiler.exe") == 0;
}

bool ShouldActivateForCurrentProject(const ProxyConfig &cfg) {
  if (!cfg.RequirePackage)
    return true;
  std::wstring projectRoot;
  if (FindUnityProjectRoot(projectRoot))
    return ProjectHasDxcb5Package(projectRoot);
  // If running inside UnityShaderCompiler.exe without a detectable package,
  // default safely to stock FXC so uninstalled/unrelated projects never use
  // DXCB5 unexpectedly.
  return !IsUnityShaderCompilerProcess();
}

std::wstring Utf8ToWide(const char *s);
std::string WideToUtf8(const wchar_t *s);

SRWLOCK g_LogLock = SRWLOCK_INIT;

void AppendLogLine(const std::string &line) {
  AcquireSRWLockExclusive(&g_LogLock);
  const std::wstring dir = GetConfigDir();
  CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring logPath = dir + L"\\stats.log";
  // Rotate stats.log -> stats.log.1 around 512 KB so it never grows unbounded.
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad) &&
      (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 512 * 1024)) {
    const std::wstring prevPath = dir + L"\\stats.log.1";
    MoveFileExW(logPath.c_str(), prevPath.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  HANDLE h = CreateFileW(
      logPath.c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written,
              nullptr);
    CloseHandle(h);
  }
  ReleaseSRWLockExclusive(&g_LogLock);
}

bool IsTraceEnabled() {
  wchar_t buf[8] = {};
  return GetEnvironmentVariableW(L"DXCB5_UNITY_TRACE", buf, 8) > 0 &&
         buf[0] != L'0';
}

void AppendDiagLine(const std::string &line) {
  AppendLogLine(line);
}

struct LoadedLibraries {
  HMODULE FxcMod = nullptr;
  HMODULE DxcMod = nullptr;
  PFN_D3DCompile FxcCompile = nullptr;
  PFN_D3DCompile2 FxcCompile2 = nullptr;
  PFN_D3DCreateBlob FxcCreateBlob = nullptr;
  PFN_DxcCreateInstance DxcCreateInstanceFn = nullptr;

  LoadedLibraries() {
    const std::wstring dir = GetModuleDirectory();
    const std::wstring fxcPath = dir + L"\\D3DCompiler_47_fxc.dll";
    FxcMod = LoadLibraryExW(fxcPath.c_str(), nullptr,
                            LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD fxcErr = FxcMod ? 0 : GetLastError();
    if (!FxcMod) {
      FxcMod = LoadLibraryW(fxcPath.c_str());
      fxcErr = FxcMod ? 0 : GetLastError();
    }
    if (FxcMod) {
      FxcCompile = reinterpret_cast<PFN_D3DCompile>(
          GetProcAddress(FxcMod, "D3DCompile"));
      FxcCompile2 = reinterpret_cast<PFN_D3DCompile2>(
          GetProcAddress(FxcMod, "D3DCompile2"));
      FxcCreateBlob = reinterpret_cast<PFN_D3DCreateBlob>(
          GetProcAddress(FxcMod, "D3DCreateBlob"));
    }
    const std::wstring dxcPath = dir + L"\\dxcb5_dxcompiler.dll";
    DxcMod = LoadLibraryExW(dxcPath.c_str(), nullptr,
                            LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD dxcErr = DxcMod ? 0 : GetLastError();
    if (!DxcMod) {
      DxcMod = LoadLibraryW(dxcPath.c_str());
      dxcErr = DxcMod ? 0 : GetLastError();
    }
    if (DxcMod) {
      DxcCreateInstanceFn = reinterpret_cast<PFN_DxcCreateInstance>(
          GetProcAddress(DxcMod, "DxcCreateInstance"));
    }
    if (!FxcMod || !FxcCompile || !DxcMod || !DxcCreateInstanceFn ||
        IsTraceEnabled()) {
      char msg[1024];
      std::snprintf(
          msg, sizeof(msg),
          "DIAG\tinit\tdir=%s\tfxc=%p(err=%lu,fn=%p)\tdxc=%p(err=%lu,fn=%p)\n",
          WideToUtf8(dir.c_str()).c_str(), static_cast<void *>(FxcMod),
          static_cast<unsigned long>(fxcErr),
          reinterpret_cast<void *>(FxcCompile), static_cast<void *>(DxcMod),
          static_cast<unsigned long>(dxcErr),
          reinterpret_cast<void *>(DxcCreateInstanceFn));
      AppendDiagLine(msg);
    }
  }
};

LoadedLibraries &GetLibs() {
  static LoadedLibraries libs;
  return libs;
}

} // namespace

namespace dxcb5_proxy {
HMODULE GetFxcModule() { return GetLibs().FxcMod; }
void LogForwardFailure(const char *name) {
  char line[160];
  std::snprintf(line, sizeof(line), "DIAG\tforward_null\t%s\n", name);
  AppendDiagLine(line);
}
} // namespace dxcb5_proxy

namespace {

std::wstring Utf8ToWide(const char *s) {
  if (!s || !*s)
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 1)
    return {};
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n - 1);
  return out;
}

std::string WideToUtf8(const wchar_t *s) {
  if (!s || !*s)
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1)
    return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n - 1, nullptr, nullptr);
  return out;
}

bool EqualsIgnoreCase(const char *a, const char *b) {
  if (!a || !b)
    return false;
  return _stricmp(a, b) == 0;
}

class D3DIncludeBridge final : public IDxcIncludeHandler {
  std::atomic<ULONG> RefCount{1};
  ID3DInclude *Include;
  IDxcUtils *Utils;

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
    std::string utf8 = WideToUtf8(pFilename);
    // Strip leading ./ that DXC sometimes adds.
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
    hr = Utils->CreateBlob(data, bytes, CP_UTF8, &blob);
    Include->Close(data);
    if (FAILED(hr))
      return hr;
    *ppIncludeSource = blob.Detach();
    return S_OK;
  }
};

std::string FirstErrorLine(IDxcBlobUtf8 *errors) {
  if (!errors || !errors->GetStringLength())
    return "unknown";
  const char *s = errors->GetStringPointer();
  std::string msg(s, errors->GetStringLength());
  for (size_t pos = 0; pos < msg.size();) {
    size_t nl = msg.find('\n', pos);
    std::string line =
        msg.substr(pos, nl == std::string::npos ? nl : nl - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.find("error") != std::string::npos)
      return line.size() > 160 ? line.substr(0, 160) : line;
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  size_t nl = msg.find('\n');
  std::string first = msg.substr(0, nl == std::string::npos ? 160 : nl);
  return first.size() > 160 ? first.substr(0, 160) : first;
}

bool TryCompileWithDxcb5(LPCVOID pSrcData, SIZE_T SrcDataSize,
                         LPCSTR pSourceName, const D3D_SHADER_MACRO *pDefines,
                         ID3DInclude *pInclude, LPCSTR pEntrypoint,
                         LPCSTR pTarget, UINT Flags1, const ProxyConfig &cfg,
                         ID3DBlob **ppCode, std::string &failReason) {
  auto &libs = GetLibs();
  if (!libs.DxcCreateInstanceFn || !libs.FxcCreateBlob) {
    failReason = "dxcb5_dxcompiler.dll not loaded";
    return false;
  }
  ComPtr<IDxcCompiler3> compiler;
  ComPtr<IDxcUtils> utils;
  if (FAILED(libs.DxcCreateInstanceFn(CLSID_DxcCompiler,
                                      IID_PPV_ARGS(&compiler))) ||
      FAILED(libs.DxcCreateInstanceFn(CLSID_DxcUtils,
                                      IID_PPV_ARGS(&utils)))) {
    failReason = "DxcCreateInstance failed";
    return false;
  }

  std::string adapted;
  const char *srcPtr = static_cast<const char *>(pSrcData);
  SIZE_T srcLen = SrcDataSize;
  const bool isPixel = EqualsIgnoreCase(pTarget, "ps_5_0");
  if (dxcb5_proxy::AdaptUnityHlslForDxc(srcPtr, srcLen, adapted, pEntrypoint,
                                        isPixel)) {
    srcPtr = adapted.data();
    srcLen = adapted.size();
  }

  std::vector<std::wstring> ownedArgs;
  ownedArgs.reserve(32);
  ownedArgs.push_back(L"-fdxcb5");
  ownedArgs.push_back(L"-HV");
  ownedArgs.push_back(L"2018");
  ownedArgs.push_back(L"-T");
  ownedArgs.push_back(Utf8ToWide(pTarget));
  ownedArgs.push_back(L"-E");
  ownedArgs.push_back(Utf8ToWide(pEntrypoint ? pEntrypoint : "main"));
  if (cfg.NanSafe)
    ownedArgs.push_back(L"--nan-safe");
  if (Flags1 & D3DCOMPILE_SKIP_OPTIMIZATION)
    ownedArgs.push_back(L"-Od");
  else if (Flags1 & D3DCOMPILE_OPTIMIZATION_LEVEL0)
    ownedArgs.push_back(L"-O0");
  else if (Flags1 & D3DCOMPILE_OPTIMIZATION_LEVEL2)
    ownedArgs.push_back(L"-O2");
  else
    ownedArgs.push_back(L"-O3");
  if (Flags1 & D3DCOMPILE_IEEE_STRICTNESS)
    ownedArgs.push_back(L"-Gis");
  if (Flags1 & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR)
    ownedArgs.push_back(L"-Zpr");
  if (Flags1 & D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR)
    ownedArgs.push_back(L"-Zpc");
  if (Flags1 & D3DCOMPILE_AVOID_FLOW_CONTROL)
    ownedArgs.push_back(L"-Gfa");
  if (Flags1 & D3DCOMPILE_PREFER_FLOW_CONTROL)
    ownedArgs.push_back(L"-Gfp");
  // Note: Do NOT pass -Gec under -HV 2018 (DXC rejects "/Gec is not supported
  // with HLSLVersion 2018"). UnityShaderCompiler sets BACKWARDS_COMPATIBILITY
  // routinely; legacy DX9 sampler2D and `: COLOR` PS semantics are handled by
  // UnityHlslAdapter, and anything still unsupported falls back cleanly to FXC.

  if (pDefines) {
    for (const D3D_SHADER_MACRO *m = pDefines; m->Name; ++m) {
      std::string def = m->Name;
      if (m->Definition && *m->Definition) {
        def += '=';
        def += m->Definition;
      }
      ownedArgs.push_back(L"-D");
      ownedArgs.push_back(Utf8ToWide(def.c_str()));
    }
  }
  if (pSourceName && *pSourceName)
    ownedArgs.push_back(Utf8ToWide(pSourceName));

  std::vector<LPCWSTR> argPtrs;
  argPtrs.reserve(ownedArgs.size());
  for (const auto &a : ownedArgs)
    argPtrs.push_back(a.c_str());

  ComPtr<IDxcIncludeHandler> incHandler;
  if (pInclude == D3D_COMPILE_STANDARD_FILE_INCLUDE) {
    utils->CreateDefaultIncludeHandler(&incHandler);
  } else if (pInclude) {
    incHandler.Attach(new D3DIncludeBridge(pInclude, utils.Get()));
  }

  DxcBuffer srcBuf{};
  srcBuf.Ptr = srcPtr;
  srcBuf.Size = srcLen;
  srcBuf.Encoding = CP_UTF8;

  ComPtr<IDxcResult> result;
  HRESULT hr = compiler->Compile(
      &srcBuf, argPtrs.data(), static_cast<UINT32>(argPtrs.size()),
      incHandler.Get(), IID_PPV_ARGS(&result));
  if (FAILED(hr) || !result) {
    failReason = "IDxcCompiler3::Compile failed";
    return false;
  }

  HRESULT status = E_FAIL;
  result->GetStatus(&status);
  if (FAILED(status)) {
    ComPtr<IDxcBlobUtf8> errs;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr);
    failReason = FirstErrorLine(errs.Get());
    return false;
  }

  ComPtr<IDxcBlob> obj;
  if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) ||
      !obj || !obj->GetBufferSize()) {
    failReason = "empty DXBC object";
    return false;
  }

  ComPtr<ID3DBlob> outBlob;
  if (FAILED(libs.FxcCreateBlob(obj->GetBufferSize(), &outBlob))) {
    failReason = "D3DCreateBlob failed";
    return false;
  }
  std::memcpy(outBlob->GetBufferPointer(), obj->GetBufferPointer(),
              obj->GetBufferSize());
  *ppCode = outBlob.Detach();
  return true;
}

} // namespace

namespace dxcb5_proxy {

PeloaderBridgeSlot g_BridgeSlot = {0, 0, 0, TLS_OUT_OF_INDEXES, nullptr,
                                   nullptr};
bool g_IsNativeInstance = false;

HRESULT WINAPI D3DCompile_Native(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint,
    LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs) {
  auto &libs = GetLibs();
  if (!libs.FxcCompile) {
    AppendDiagLine("DIAG\tD3DCompile\tFxcCompile is null -> E_FAIL\n");
    return E_FAIL;
  }
  const bool trace = IsTraceEnabled();
  const bool supportedTarget =
      EqualsIgnoreCase(pTarget, "vs_5_0") || EqualsIgnoreCase(pTarget, "ps_5_0");
  if (!supportedTarget || !pSrcData || !SrcDataSize || !ppCode) {
    const HRESULT hr = libs.FxcCompile(pSrcData, SrcDataSize, pSourceName,
                                       pDefines, pInclude, pEntrypoint, pTarget,
                                       Flags1, Flags2, ppCode, ppErrorMsgs);
    if (trace) {
      char line[256];
      std::snprintf(line, sizeof(line),
                    "TRACE\tD3DCompile_pass\t%s\t%s\thr=0x%08lx\n",
                    pTarget ? pTarget : "", pEntrypoint ? pEntrypoint : "",
                    static_cast<unsigned long>(hr));
      AppendDiagLine(line);
    }
    return hr;
  }
  const ProxyConfig cfg = LoadConfig();
  const bool active = cfg.Enabled && ShouldActivateForCurrentProject(cfg);
  if (!active) {
    const HRESULT hr = libs.FxcCompile(pSrcData, SrcDataSize, pSourceName,
                                       pDefines, pInclude, pEntrypoint, pTarget,
                                       Flags1, Flags2, ppCode, ppErrorMsgs);
    if (trace) {
      wchar_t cwd[512] = {};
      GetCurrentDirectoryW(512, cwd);
      char line[512];
      std::snprintf(
          line, sizeof(line),
          "TRACE\tD3DCompile_bypass\t%s\t%s\ten=%d\treq=%d\tcwd=%s\thr=0x%08lx\n",
          pTarget ? pTarget : "", pEntrypoint ? pEntrypoint : "",
          cfg.Enabled ? 1 : 0, cfg.RequirePackage ? 1 : 0,
          WideToUtf8(cwd).c_str(), static_cast<unsigned long>(hr));
      AppendDiagLine(line);
    }
    return hr;
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::string failReason;
  if (TryCompileWithDxcb5(pSrcData, SrcDataSize, pSourceName, pDefines,
                          pInclude, pEntrypoint, pTarget, Flags1, cfg, ppCode,
                          failReason)) {
    if (ppErrorMsgs)
      *ppErrorMsgs = nullptr;
    if (cfg.LogStats) {
      const double ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - t0)
              .count();
      char line[256];
      std::snprintf(line, sizeof(line), "HIT\t%s\t%s\t%.2f\n",
                    pTarget ? pTarget : "", pEntrypoint ? pEntrypoint : "", ms);
      AppendLogLine(line);
    }
    return S_OK;
  }

  if (cfg.LogStats) {
    std::string line = "FALLBACK\t";
    line += (pTarget ? pTarget : "");
    line += '\t';
    line += (pEntrypoint ? pEntrypoint : "");
    line += '\t';
    line += failReason;
    line += '\n';
    AppendLogLine(line);
  }
  return libs.FxcCompile(pSrcData, SrcDataSize, pSourceName, pDefines, pInclude,
                         pEntrypoint, pTarget, Flags1, Flags2, ppCode,
                         ppErrorMsgs);
}

HRESULT WINAPI D3DCompile2_Native(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint,
    LPCSTR pTarget, UINT Flags1, UINT Flags2, UINT SecondaryDataFlags,
    LPCVOID pSecondaryData, SIZE_T SecondaryDataSize, ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs) {
  if (!SecondaryDataFlags && !pSecondaryData && !SecondaryDataSize) {
    return D3DCompile_Native(pSrcData, SrcDataSize, pSourceName, pDefines,
                             pInclude, pEntrypoint, pTarget, Flags1, Flags2,
                             ppCode, ppErrorMsgs);
  }
  auto &libs = GetLibs();
  if (!libs.FxcCompile2) {
    AppendDiagLine("DIAG\tD3DCompile2\tFxcCompile2 is null -> E_FAIL\n");
    return E_FAIL;
  }
  return libs.FxcCompile2(pSrcData, SrcDataSize, pSourceName, pDefines,
                          pInclude, pEntrypoint, pTarget, Flags1, Flags2,
                          SecondaryDataFlags, pSecondaryData, SecondaryDataSize,
                          ppCode, ppErrorMsgs);
}

namespace {

const NativeDispatchTable g_LocalDispatch = {
    &D3DCompile_Native,
    &D3DCompile2_Native,
    &D3DAssemble_Native,
    &DebugSetMute_Native,
    &D3DCompileFromFile_Native,
    &D3DCompressShaders_Native,
    &D3DCreateBlob_Native,
    &D3DCreateFunctionLinkingGraph_Native,
    &D3DCreateLinker_Native,
    &D3DDecompressShaders_Native,
    &D3DDisassemble_Native,
    &D3DDisassemble10Effect_Native,
    &D3DDisassemble11Trace_Native,
    &D3DDisassembleRegion_Native,
    &D3DGetBlobPart_Native,
    &D3DGetDebugInfo_Native,
    &D3DGetInputAndOutputSignatureBlob_Native,
    &D3DGetInputSignatureBlob_Native,
    &D3DGetOutputSignatureBlob_Native,
    &D3DGetTraceInstructionOffsets_Native,
    &D3DLoadModule_Native,
    &D3DPreprocess_Native,
    &D3DReadFileToBlob_Native,
    &D3DReflect_Native,
    &D3DReflectLibrary_Native,
    &D3DReturnFailure1_Native,
    &D3DSetBlobPart_Native,
    &D3DStripShader_Native,
    &D3DWriteBlobToFile_Native,
};

PeloaderBridgeSlot *g_ConnectedPeerSlot = nullptr;

bool TryClaimBridgeSlot(void *val, DWORD pid, DWORD idx) {
  __try {
    auto *slot = static_cast<PeloaderBridgeSlot *>(val);
    if (slot->Magic1 == kBridgeMagic1 && slot->Magic2 == kBridgeMagic2 &&
        slot->Pid == pid && slot->Self == slot && slot->TlsIndex == idx &&
        slot != &g_BridgeSlot) {
      InterlockedExchangePointer(
          &slot->NativeDispatch,
          const_cast<NativeDispatchTable *>(&g_LocalDispatch));
      slot->TlsIndex = TLS_OUT_OF_INDEXES;
      g_ConnectedPeerSlot = slot;
      return true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

void SafeDisconnectPeerSlot(PeloaderBridgeSlot *slot) {
  if (!slot)
    return;
  __try {
    if (slot->Magic1 == kBridgeMagic1 && slot->Magic2 == kBridgeMagic2 &&
        slot->Self == slot) {
      InterlockedCompareExchangePointer(
          &slot->NativeDispatch, nullptr,
          const_cast<NativeDispatchTable *>(&g_LocalDispatch));
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void ConnectPeerPeloaderInstance() {
  const DWORD savedErr = GetLastError();
  const DWORD pid = GetCurrentProcessId();
  // Windows supports up to 1088 TLS slots (64 primary + 1024 expansion).
  for (DWORD idx = 0; idx < 1088; ++idx) {
    void *val = TlsGetValue(idx);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(val);
    if (!val || (addr & 7) != 0 || addr < 0x10000)
      continue;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(val, &mbi, sizeof(mbi)))
      continue;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
      continue;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE)
      continue;
    const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionBase + mbi.RegionSize;
    if (addr < regionBase || regionEnd < addr ||
        (regionEnd - addr) < sizeof(PeloaderBridgeSlot))
      continue;
    if (TryClaimBridgeSlot(val, pid, idx)) {
      TlsSetValue(idx, nullptr);
      TlsFree(idx);
      break;
    }
  }
  SetLastError(savedErr);
}

} // namespace

} // namespace dxcb5_proxy

extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason,
                               LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hInst);
    // UnityShaderCompiler.exe's custom in-memory PE loader stubs
    // GetModuleHandleA to return nullptr, whereas Windows LoadLibraryExW
    // resolves the real KERNEL32!GetModuleHandleA which returns the host EXE.
    dxcb5_proxy::g_IsNativeInstance = (GetModuleHandleA(nullptr) != nullptr);
    if (!dxcb5_proxy::g_IsNativeInstance) {
      const DWORD idx = TlsAlloc();
      if (idx != TLS_OUT_OF_INDEXES) {
        dxcb5_proxy::g_BridgeSlot.Magic1 = dxcb5_proxy::kBridgeMagic1;
        dxcb5_proxy::g_BridgeSlot.Magic2 = dxcb5_proxy::kBridgeMagic2;
        dxcb5_proxy::g_BridgeSlot.Pid = GetCurrentProcessId();
        dxcb5_proxy::g_BridgeSlot.TlsIndex = idx;
        dxcb5_proxy::g_BridgeSlot.Self = &dxcb5_proxy::g_BridgeSlot;
        dxcb5_proxy::g_BridgeSlot.NativeDispatch = nullptr;
        TlsSetValue(idx, &dxcb5_proxy::g_BridgeSlot);
      }
    } else {
      dxcb5_proxy::ConnectPeerPeloaderInstance();
    }
  } else if (reason == DLL_PROCESS_DETACH) {
    if (!dxcb5_proxy::g_IsNativeInstance &&
        dxcb5_proxy::g_BridgeSlot.TlsIndex != TLS_OUT_OF_INDEXES) {
      const DWORD idx = dxcb5_proxy::g_BridgeSlot.TlsIndex;
      dxcb5_proxy::g_BridgeSlot.TlsIndex = TLS_OUT_OF_INDEXES;
      TlsSetValue(idx, nullptr);
      TlsFree(idx);
    } else if (dxcb5_proxy::g_IsNativeInstance && reserved == nullptr) {
      dxcb5_proxy::SafeDisconnectPeerSlot(dxcb5_proxy::g_ConnectedPeerSlot);
      dxcb5_proxy::g_ConnectedPeerSlot = nullptr;
    }
  }
  return TRUE;
}

extern "C" HRESULT WINAPI D3DCompile(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint,
    LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs) {
  if (const auto *d = dxcb5_proxy::GetPeerNativeDispatch()) {
    return d->D3DCompile(pSrcData, SrcDataSize, pSourceName, pDefines, pInclude,
                         pEntrypoint, pTarget, Flags1, Flags2, ppCode,
                         ppErrorMsgs);
  }
  if (!dxcb5_proxy::IsNativeInstance())
    return E_FAIL;
  return dxcb5_proxy::D3DCompile_Native(
      pSrcData, SrcDataSize, pSourceName, pDefines, pInclude, pEntrypoint,
      pTarget, Flags1, Flags2, ppCode, ppErrorMsgs);
}

extern "C" HRESULT WINAPI D3DCompile2(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude, LPCSTR pEntrypoint,
    LPCSTR pTarget, UINT Flags1, UINT Flags2, UINT SecondaryDataFlags,
    LPCVOID pSecondaryData, SIZE_T SecondaryDataSize, ID3DBlob **ppCode,
    ID3DBlob **ppErrorMsgs) {
  if (const auto *d = dxcb5_proxy::GetPeerNativeDispatch()) {
    return d->D3DCompile2(pSrcData, SrcDataSize, pSourceName, pDefines,
                          pInclude, pEntrypoint, pTarget, Flags1, Flags2,
                          SecondaryDataFlags, pSecondaryData, SecondaryDataSize,
                          ppCode, ppErrorMsgs);
  }
  if (!dxcb5_proxy::IsNativeInstance())
    return E_FAIL;
  return dxcb5_proxy::D3DCompile2_Native(
      pSrcData, SrcDataSize, pSourceName, pDefines, pInclude, pEntrypoint,
      pTarget, Flags1, Flags2, SecondaryDataFlags, pSecondaryData,
      SecondaryDataSize, ppCode, ppErrorMsgs);
}
