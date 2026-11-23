// Distributed under the University of Illinois Open Source License.
// Native Unity.exe in-process hook built on MinHook v1.3.4 (BSD-2-Clause):
//   1. Hooks kernel32!CreateProcessW via MinHook to redirect UnityShaderCompiler.exe
//      plugin discovery to a staged CgBatchPlugins dir (zero UAC / zero Tools/ patching);
//   2. Hooks Unity!ShaderCompilerCompileSnippet via MinHook for in-process RAM
//      variant replay + zero-stub manifest context publishing.
#define DXCB5_UNITY_HOOK_EXPORTS
#include "dxcb5/UnityHookApi.h"
#include "VariantCacheAndContext.h"
#include "MinHook.h"
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dxcb5_hook;

namespace {

using PFN_CreateProcessW = BOOL(WINAPI *)(
    LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
    LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

SRWLOCK gLifecycleLock = SRWLOCK_INIT;
SRWLOCK gStateLock = SRWLOCK_INIT;
SRWLOCK gStagingLock = SRWLOCK_INIT;

bool gMinHookReady = false;
bool gSelfPinned = false;
bool gCreateProcessHookCreated = false;
void *gCreateProcessTargetAddr = nullptr;
bool gSnippetHookCreated = false;
void *gSnippetTargetAddr = nullptr;

std::atomic<bool> gInstalled{false};
std::atomic<bool> gEnabled{true};
std::atomic<bool> gNanSafe{false};
std::atomic<bool> gSnippetCacheEnabled{true};
std::atomic<bool> gSnippetHookActive{false};
std::atomic<int64_t> gInFlightCalls{0};

std::atomic<uint64_t> gRamHits{0};
std::atomic<uint64_t> gIpcCalls{0};
std::atomic<uint64_t> gWorkersHooked{0};

std::wstring gPluginDir;
std::wstring gTempDir;
std::wstring gProjectDir;
std::wstring gStagingDir;
std::wstring gSessionDir;

PFN_CreateProcessW gOrigCreateProcessW = nullptr;
PFN_ShaderCompilerCompileSnippet gOrigCompileSnippet = nullptr;
PFN_VectorU8Assign gVectorU8Assign = nullptr;

VariantReplayCache gReplayCache;
ManifestRegistry gManifestRegistry;

// Verified 20-byte prologue of Unity 2022.3.22f1 ShaderCompilerCompileSnippet (0x02195A60).
constexpr uint8_t kExpectedCompileSnippetPrologue[20] = {
    0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18,
    0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};

// Verified 15-byte prologue of core::vector<signed char,0>::assign (0x0097C400).
constexpr uint8_t kExpectedVectorAssignPrologue[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30};

struct InFlightGuard {
  InFlightGuard() { gInFlightCalls.fetch_add(1, std::memory_order_acq_rel); }
  ~InFlightGuard() { gInFlightCalls.fetch_sub(1, std::memory_order_acq_rel); }
};

__declspec(noinline) __declspec(guard(nocf)) BOOL CallOrigCreateProcessW(
    LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
    DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation) {
  return gOrigCreateProcessW(lpApplicationName, lpCommandLine,
                             lpProcessAttributes, lpThreadAttributes,
                             bInheritHandles, dwCreationFlags, lpEnvironment,
                             lpCurrentDirectory, lpStartupInfo,
                             lpProcessInformation);
}

__declspec(noinline) __declspec(guard(nocf)) bool CallOrigCompileSnippet(
    void **conn, const UnityCoreString &inputCode, uint64_t codeHash,
    const UnityCoreString &inputPath, const UnityCoreString &shaderName,
    int startLine, const UnityCoreString &entryName,
    const UnityCoreVector<UnityCoreString> &keywords0,
    const UnityCoreVector<UnityCoreString> &keywords1,
    const UnityCoreVector<UnityCoreString> &keywords2, uint64_t arg10,
    int buildTarget, uint64_t arg12, bool arg13, bool arg14,
    uint64_t compilationFlags, UnityCoreVector<uint8_t> &outputCode,
    uint32_t &outProgramType, int sourceLang, int programType,
    uint32_t programMask, int api, uint64_t requirements,
    UnityReflectionReport *report, PFN_UnityErrorCallback errorCb,
    void *errorCbData) {
  return gOrigCompileSnippet(
      conn, inputCode, codeHash, inputPath, shaderName, startLine, entryName,
      keywords0, keywords1, keywords2, arg10, buildTarget, arg12, arg13, arg14,
      compilationFlags, outputCode, outProgramType, sourceLang, programType,
      programMask, api, requirements, report, errorCb, errorCbData);
}

std::wstring PickFirstExisting(const std::wstring &dir, const wchar_t *a,
                               const wchar_t *b) {
  const std::wstring pa = dir + L"\\" + a;
  if (FileExistsW(pa))
    return pa;
  if (b) {
    const std::wstring pb = dir + L"\\" + b;
    if (FileExistsW(pb))
      return pb;
  }
  return {};
}

std::atomic<uint64_t> gStateSeq{0};

std::wstring CopySessionDir() {
  AcquireSRWLockShared(&gStateLock);
  std::wstring dir = gSessionDir;
  ReleaseSRWLockShared(&gStateLock);
  return dir;
}

void WriteSessionStateFile() {
  const std::wstring sessionDir = CopySessionDir();
  if (sessionDir.empty())
    return;
  EnsureDirTreeW(sessionDir);
  const bool hasManifests = !gManifestRegistry.Snapshot().empty();
  const uint64_t seq = gStateSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
  char buf[128];
  const int n = std::snprintf(
      buf, sizeof(buf), "seq=%llu\nenabled=%d\nnan_safe=%d\nmanifests=%d\n",
      static_cast<unsigned long long>(seq),
      gEnabled.load(std::memory_order_acquire) ? 1 : 0,
      gNanSafe.load(std::memory_order_acquire) ? 1 : 0, hasManifests ? 1 : 0);
  if (n > 0)
    dxcb5_proxy::WriteFileAtomic(sessionDir + L"\\hook_state.txt", buf,
                                 static_cast<size_t>(n));
}

void WriteManifestFileToSession(const std::wstring &sessionDir,
                                const std::string &normPath,
                                const RegisteredManifest &reg) {
  if (sessionDir.empty() || normPath.empty())
    return;
  EnsureDirTreeW(sessionDir);
  const std::wstring ctxPath =
      sessionDir + L"\\manifest_" + Utf8ToWide(Hex64(Fnv1a64Str(normPath))) +
      L".json";
  std::string payload = "{\"pair\":";
  dxcb5_proxy::AppendJsonEscaped(payload, reg.PairEntries);
  payload += ",\"manifest\":";
  payload += reg.ManifestJson;
  payload += "}";
  dxcb5_proxy::WriteFileAtomic(ctxPath, payload.data(), payload.size());
}

void FlushAllManifestsToSession() {
  const std::wstring sessionDir = CopySessionDir();
  if (sessionDir.empty())
    return;
  for (const auto &kv : gManifestRegistry.Snapshot())
    WriteManifestFileToSession(sessionDir, kv.first, kv.second);
}

void DeleteFlatFilesInDirSafe(const std::wstring &dir, const wchar_t *pattern) {
  if (dir.empty())
    return;
  const DWORD attrs = GetFileAttributesW(dir.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES ||
      (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    return;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
      DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

void DeleteAllManifestFilesInSession() {
  DeleteFlatFilesInDirSafe(CopySessionDir(), L"manifest_*.json");
}

void ClearWorkerDiskCachesInSession() {
  const std::wstring sessionDir = CopySessionDir();
  if (sessionDir.empty())
    return;
  DeleteFlatFilesInDirSafe(sessionDir + L"\\manifest_dxbc", L"*");
  DeleteFlatFilesInDirSafe(sessionDir + L"\\direct_dxbc", L"*");
}

void CleanupSessionDirOnUninstall() {
  const std::wstring sessionDir = CopySessionDir();
  if (sessionDir.empty())
    return;
  DeleteFlatFilesInDirSafe(sessionDir + L"\\manifest_dxbc", L"*");
  RemoveDirectoryW((sessionDir + L"\\manifest_dxbc").c_str());
  DeleteFlatFilesInDirSafe(sessionDir + L"\\direct_dxbc", L"*");
  RemoveDirectoryW((sessionDir + L"\\direct_dxbc").c_str());
  DeleteFlatFilesInDirSafe(sessionDir, L"manifest_*.json");
  DeleteFileW((sessionDir + L"\\hook_state.txt").c_str());
  RemoveDirectoryW(sessionDir.c_str());
}

// Serialized under gStagingLock so concurrent UnityShaderCompiler.exe spawns
// never race on DLL staging copies. NEVER creates NTFS junctions or symlinks!
bool PrepareCgBatchStagingDir(const std::wstring &origPluginsDir,
                              std::wstring &outDxcDllPath) {
  AcquireSRWLockExclusive(&gStagingLock);
  AcquireSRWLockShared(&gStateLock);
  const std::wstring pluginDir = gPluginDir;
  const std::wstring staging = gStagingDir;
  const std::wstring session = gSessionDir;
  ReleaseSRWLockShared(&gStateLock);

  bool ok = false;
  do {
    if (pluginDir.empty() || staging.empty())
      break;
    // Immediately detach any legacy NTFS junctions from older hook builds.
    DetachReparsePointDirectoriesInDir(staging);
    EnsureDirTreeW(session);
    const std::wstring cgDir = staging + L"\\DXCB5\\CgBatchPlugins64";
    if (!EnsureDirTreeW(cgDir))
      break;

    const std::wstring srcPlugin = PickFirstExisting(
        pluginDir, L"dxcb5_cgbatch_plugin.dll", L"dxcb5_cgbatch_plugin.dll.bin");
    if (srcPlugin.empty())
      break;
    const std::wstring dstPlugin = cgDir + L"\\dxcb5_cgbatch_plugin.dll";
    if (!CopyFileAtomicIfNewer(srcPlugin, dstPlugin))
      break;

    // Support both dev `dxcompiler.dll` and packaged VCC `dxcb5_dxcompiler.dll.bin`.
    // Always shadow-copy into a sibling DxcBin dir keyed by (size, mtime) so
    // UnityShaderCompiler.exe never locks the package directory's dxcompiler.dll
    // and hot-reloading workers works without closing Unity Editor.
    const std::wstring srcDxc =
        PickFirstExisting(pluginDir, L"dxcompiler.dll", L"dxcb5_dxcompiler.dll.bin");
    if (srcDxc.empty())
      break;
    const std::wstring dxcBinDir = staging + L"\\DXCB5\\DxcBin";
    if (!EnsureDirTreeW(dxcBinDir))
      break;
    WIN32_FILE_ATTRIBUTE_DATA dxcAttr{};
    if (!GetFileAttributesExW(srcDxc.c_str(), GetFileExInfoStandard, &dxcAttr))
      break;
    uint64_t dxcSig = 14695981039346656037ull;
    dxcSig = Fnv1a64(&dxcAttr.nFileSizeLow, sizeof(dxcAttr.nFileSizeLow), dxcSig);
    dxcSig = Fnv1a64(&dxcAttr.nFileSizeHigh, sizeof(dxcAttr.nFileSizeHigh), dxcSig);
    dxcSig = Fnv1a64(&dxcAttr.ftLastWriteTime, sizeof(dxcAttr.ftLastWriteTime),
                     dxcSig);
    const std::wstring dstDxc =
        dxcBinDir + L"\\dxcompiler_" + Utf8ToWide(Hex64(dxcSig)) + L".dll";
    if (!CopyFileAtomicIfNewer(srcDxc, dstDxc))
      break;
    outDxcDllPath = dstDxc;

    // Plain-copy only actual sibling `*/CgBatchPlugins64/*.dll` files (zero junctions).
    CopyOtherCgBatchPluginsSafe(origPluginsDir, staging);
    ok = true;
  } while (false);

  ReleaseSRWLockExclusive(&gStagingLock);
  return ok;
}

BOOL WINAPI HookedCreateProcessW(
    LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
    DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation) {
  InFlightGuard guard;
  const bool isShaderCompiler =
      IsUnityShaderCompilerInvocation(lpApplicationName, lpCommandLine);
  if (!isShaderCompiler || !gEnabled.load(std::memory_order_acquire) ||
      !lpCommandLine) {
    return CallOrigCreateProcessW(lpApplicationName, lpCommandLine,
                                  lpProcessAttributes, lpThreadAttributes,
                                  bInheritHandles, dwCreationFlags, lpEnvironment,
                                  lpCurrentDirectory, lpStartupInfo,
                                  lpProcessInformation);
  }

  AcquireSRWLockShared(&gStateLock);
  const std::wstring stagingDir = gStagingDir;
  const std::wstring sessionDir = gSessionDir;
  const std::wstring projectDir = gProjectDir;
  ReleaseSRWLockShared(&gStateLock);

  std::wstring newCmd, origPluginsDir, dxcDllPath;
  if (!RewriteShaderCompilerCmdLine(lpCommandLine, stagingDir, newCmd,
                                    origPluginsDir) ||
      !PrepareCgBatchStagingDir(origPluginsDir, dxcDllPath)) {
    return CallOrigCreateProcessW(lpApplicationName, lpCommandLine,
                                  lpProcessAttributes, lpThreadAttributes,
                                  bInheritHandles, dwCreationFlags, lpEnvironment,
                                  lpCurrentDirectory, lpStartupInfo,
                                  lpProcessInformation);
  }

  WriteSessionStateFile();
  std::vector<wchar_t> cmdBuf(newCmd.begin(), newCmd.end());
  cmdBuf.push_back(L'\0');
  std::vector<std::pair<std::wstring, std::wstring>> extras = {
      {L"DXCB5_DXCOMPILER_PATH", dxcDllPath},
      {L"DXCB5_HOOK_SESSION_DIR", sessionDir},
      {L"DXCB5_HOOK_ENABLED",
       gEnabled.load(std::memory_order_acquire) ? L"1" : L"0"},
      {L"DXCB5_NAN_SAFE",
       gNanSafe.load(std::memory_order_acquire) ? L"1" : L"0"},
  };
  if (!projectDir.empty())
    extras.emplace_back(L"DXCB5_UNITY_PROJECT_DIR", projectDir);

  std::vector<wchar_t> envBlock;
  BuildAugmentedUnicodeEnv(lpEnvironment, dwCreationFlags, extras, envBlock);
  gWorkersHooked.fetch_add(1, std::memory_order_relaxed);
  return CallOrigCreateProcessW(lpApplicationName, cmdBuf.data(),
                                lpProcessAttributes, lpThreadAttributes,
                                bInheritHandles,
                                dwCreationFlags | CREATE_UNICODE_ENVIRONMENT,
                                envBlock.data(), lpCurrentDirectory,
                                lpStartupInfo, lpProcessInformation);
}

struct ErrorTrackState {
  PFN_UnityErrorCallback Orig = nullptr;
  void *OrigData = nullptr;
  bool SawAnyDiagnostic = false;
};

void __cdecl TrackedErrorCb(void *userData, int errorType, int line,
                            int platform, const char *msg,
                            const char *msgDetails) {
  auto *st = static_cast<ErrorTrackState *>(userData);
  // Skip caching whenever any error OR warning occurs so diagnostics are never lost.
  st->SawAnyDiagnostic = true;
  if (st->Orig)
    st->Orig(st->OrigData, errorType, line, platform, msg, msgDetails);
}

bool __cdecl HookedShaderCompilerCompileSnippet(
    void **conn, const UnityCoreString &inputCode, uint64_t codeHash,
    const UnityCoreString &inputPath, const UnityCoreString &shaderName,
    int startLine, const UnityCoreString &entryName,
    const UnityCoreVector<UnityCoreString> &keywords0,
    const UnityCoreVector<UnityCoreString> &keywords1,
    const UnityCoreVector<UnityCoreString> &keywords2, uint64_t arg10,
    int buildTarget, uint64_t arg12, bool arg13, bool arg14,
    uint64_t compilationFlags, UnityCoreVector<uint8_t> &outputCode,
    uint32_t &outProgramType, int sourceLang, int programType,
    uint32_t programMask, int api, uint64_t requirements,
    UnityReflectionReport *report, PFN_UnityErrorCallback errorCb,
    void *errorCbData) {
  InFlightGuard guard;
  const bool eligible =
      gEnabled.load(std::memory_order_acquire) &&
      gSnippetCacheEnabled.load(std::memory_order_acquire) &&
      gVectorU8Assign != nullptr && api == kShaderCompPlatformD3D11 &&
      (programType == kProgramVertex || programType == kProgramFragment);
  if (!eligible) {
    return CallOrigCompileSnippet(
        conn, inputCode, codeHash, inputPath, shaderName, startLine, entryName,
        keywords0, keywords1, keywords2, arg10, buildTarget, arg12, arg13,
        arg14, compilationFlags, outputCode, outProgramType, sourceLang,
        programType, programMask, api, requirements, report, errorCb,
        errorCbData);
  }

  std::string_view codeView, pathView, shaderView, entryView;
  const bool viewsValid = inputCode.TryView(codeView) &&
                          inputPath.TryView(pathView) &&
                          shaderName.TryView(shaderView) &&
                          entryName.TryView(entryView);

  AcquireSRWLockShared(&gStateLock);
  const std::wstring sessionDir = gSessionDir;
  const std::wstring projectDir = gProjectDir;
  ReleaseSRWLockShared(&gStateLock);

  uint64_t manifestHash = 0;
  RegisteredManifest reg;
  if (viewsValid && !pathView.empty() && gManifestRegistry.Find(pathView, reg))
    manifestHash = reg.Hash;

  std::string cacheKey;
  const bool cacheable =
      viewsValid &&
      BuildSnippetCacheKey(
          codeView, codeHash, pathView, shaderView, startLine, entryView,
          keywords0, keywords1, keywords2, arg10, buildTarget, arg12, arg13,
          arg14, compilationFlags, sourceLang, programType, programMask, api,
          requirements, manifestHash, gNanSafe.load(std::memory_order_acquire),
          projectDir, cacheKey);

  if (cacheable) {
    CachedSnippetResult cached;
    if (gReplayCache.Lookup(cacheKey, report != nullptr, cached)) {
      outProgramType = cached.OutProgramType;
      const uint8_t *p = cached.Code.data();
      gVectorU8Assign(&outputCode, p, p + cached.Code.size());
      if (report)
        cached.Reflection.ReplayTo(report);
      gRamHits.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }

  gIpcCalls.fetch_add(1, std::memory_order_relaxed);
  ErrorTrackState errTrack{errorCb, errorCbData, false};
  RecordingReflectionReport recorder(report);
  UnityReflectionReport *passReport = report ? &recorder : nullptr;
  const bool ok = CallOrigCompileSnippet(
      conn, inputCode, codeHash, inputPath, shaderName, startLine, entryName,
      keywords0, keywords1, keywords2, arg10, buildTarget, arg12, arg13, arg14,
      compilationFlags, outputCode, outProgramType, sourceLang, programType,
      programMask, api, requirements, passReport,
      errorCb ? &TrackedErrorCb : nullptr, errorCb ? &errTrack : nullptr);
  if (cacheable && ok && !errTrack.SawAnyDiagnostic && outputCode.Count() > 0) {
    CachedSnippetResult res;
    res.OutProgramType = outProgramType;
    res.Code.assign(outputCode.Data, outputCode.Data + outputCode.Count());
    res.HasReflection = (report != nullptr);
    if (report)
      res.Reflection = std::move(recorder.Data);
    gReplayCache.Insert(cacheKey, std::move(res));
  }
  return ok;
}

bool TryCreateAndEnableUnitySnippetHook() {
  if (!gSnippetHookCreated) {
    HMODULE unityMod = GetModuleHandleW(nullptr);
    if (!unityMod)
      return false;
    auto *base = reinterpret_cast<uint8_t *>(unityMod);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      return false;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.TimeDateStamp != kUnity2022_3_22f1_TimeDateStamp)
      return false;
    const size_t imgSize = nt->OptionalHeader.SizeOfImage;
    if (imgSize <= kRvaShaderCompilerCompileSnippet +
                       sizeof(kExpectedCompileSnippetPrologue) ||
        imgSize <= kRvaVectorU8Assign + sizeof(kExpectedVectorAssignPrologue))
      return false;

    uint8_t *vecAssignAddr = base + kRvaVectorU8Assign;
    uint8_t *snippetAddr = base + kRvaShaderCompilerCompileSnippet;
    if (std::memcmp(vecAssignAddr, kExpectedVectorAssignPrologue,
                    sizeof(kExpectedVectorAssignPrologue)) != 0 ||
        std::memcmp(snippetAddr, kExpectedCompileSnippetPrologue,
                    sizeof(kExpectedCompileSnippetPrologue)) != 0)
      return false;

    gVectorU8Assign = reinterpret_cast<PFN_VectorU8Assign>(vecAssignAddr);
    void *origSnippet = nullptr;
    if (MH_CreateHook(snippetAddr,
                      reinterpret_cast<void *>(&HookedShaderCompilerCompileSnippet),
                      &origSnippet) != MH_OK ||
        !origSnippet) {
      return false;
    }
    RegisterCfgCallTarget(origSnippet);
    gOrigCompileSnippet =
        reinterpret_cast<PFN_ShaderCompilerCompileSnippet>(origSnippet);
    gSnippetTargetAddr = snippetAddr;
    gSnippetHookCreated = true;
  }

  const MH_STATUS enSt = MH_EnableHook(gSnippetTargetAddr);
  if (enSt != MH_OK && enSt != MH_ERROR_ENABLED)
    return false;
  gSnippetHookActive.store(true, std::memory_order_release);
  return true;
}

std::wstring DeriveProjectDirFromTemp(const std::wstring &tempDir) {
  std::wstring s = tempDir;
  for (wchar_t &c : s)
    if (c == L'/')
      c = L'\\';
  while (s.size() > 3 && s.back() == L'\\')
    s.pop_back();
  const size_t slash = s.find_last_of(L'\\');
  if (slash != std::wstring::npos &&
      _wcsicmp(s.c_str() + slash + 1, L"Temp") == 0) {
    return s.substr(0, slash);
  }
  return {};
}

// Terminate any UnityShaderCompiler.exe child workers owned by this Unity.exe
// PID so Unity respawns fresh workers with newly staged plugin/dxcompiler DLLs.
void RestartPreHookChildWorkers() {
  const DWORD selfPid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return;
  std::vector<HANDLE> handles;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ParentProcessID != selfPid)
        continue;
      if (_wcsicmp(pe.szExeFile, L"UnityShaderCompiler.exe") != 0)
        continue;
      HANDLE hProc =
          OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
      if (hProc) {
        TerminateProcess(hProc, 0);
        handles.push_back(hProc);
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  if (!handles.empty()) {
    const DWORD n = static_cast<DWORD>(
        handles.size() < MAXIMUM_WAIT_OBJECTS ? handles.size()
                                              : MAXIMUM_WAIT_OBJECTS);
    WaitForMultipleObjects(n, handles.data(), TRUE, 500);
    for (HANDLE h : handles)
      CloseHandle(h);
  }
}

} // namespace

extern "C" {

DXCB5_HOOK_API uint32_t dxcb5_hook_get_abi_version(void) {
  return DXCB5_UNITY_HOOK_ABI_VERSION;
}

DXCB5_HOOK_API int dxcb5_hook_install(const wchar_t *pluginDir,
                                      const wchar_t *tempDir,
                                      int enableSnippetCache) {
  if (!pluginDir || !*pluginDir || !tempDir || !*tempDir)
    return DXCB5_HOOK_ERR_INVALID_ARG;

  AcquireSRWLockExclusive(&gLifecycleLock);

  // Pin our own DLL once hooks are installed so Domain Reload / FreeLibrary
  // can never unmap code while threads might still unwind through hooks.
  if (!gSelfPinned) {
    HMODULE selfMod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&dxcb5_hook_install),
                           &selfMod))
      gSelfPinned = true;
  }

  if (!gMinHookReady) {
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
      ReleaseSRWLockExclusive(&gLifecycleLock);
      return DXCB5_HOOK_ERR_MINHOOK_INIT;
    }
    gMinHookReady = true;
  }

  {
    AcquireSRWLockExclusive(&gStateLock);
    gPluginDir = pluginDir;
    gTempDir = tempDir;
    while (gPluginDir.size() > 3 &&
           (gPluginDir.back() == L'\\' || gPluginDir.back() == L'/'))
      gPluginDir.pop_back();
    while (gTempDir.size() > 3 &&
           (gTempDir.back() == L'\\' || gTempDir.back() == L'/'))
      gTempDir.pop_back();
    gProjectDir = DeriveProjectDirFromTemp(gTempDir);
    if (gProjectDir.empty())
      ResolveProjectRootW(gProjectDir);
    gStagingDir = gTempDir + L"\\DXCB5CgBatchPlugins";
    gSessionDir = gTempDir + L"\\DXCB5HookSession_" +
                  std::to_wstring(GetCurrentProcessId());
    DetachReparsePointDirectoriesInDir(gStagingDir);
    ReleaseSRWLockExclusive(&gStateLock);
  }

  const std::wstring envNan = GetEnvVarW(L"DXCB5_NAN_SAFE");
  if (envNan == L"1")
    gNanSafe.store(true, std::memory_order_release);

  gSnippetCacheEnabled.store(enableSnippetCache != 0, std::memory_order_release);
  gEnabled.store(true, std::memory_order_release);
  WriteSessionStateFile();
  FlushAllManifestsToSession();

  if (!gCreateProcessHookCreated) {
    void *origCp = nullptr;
    if (MH_CreateHookApiEx(L"kernel32", "CreateProcessW",
                           reinterpret_cast<void *>(&HookedCreateProcessW),
                           &origCp, &gCreateProcessTargetAddr) != MH_OK ||
        !origCp || !gCreateProcessTargetAddr) {
      ReleaseSRWLockExclusive(&gLifecycleLock);
      return DXCB5_HOOK_ERR_CREATE_HOOK;
    }
    RegisterCfgCallTarget(origCp);
    // Assign trampoline BEFORE enabling hook so concurrent callers never see null!
    gOrigCreateProcessW = reinterpret_cast<PFN_CreateProcessW>(origCp);
    gCreateProcessHookCreated = true;
  }

  const MH_STATUS cpSt = MH_EnableHook(gCreateProcessTargetAddr);
  if (cpSt != MH_OK && cpSt != MH_ERROR_ENABLED) {
    ReleaseSRWLockExclusive(&gLifecycleLock);
    return DXCB5_HOOK_ERR_ENABLE_HOOK;
  }

  if (enableSnippetCache) {
    TryCreateAndEnableUnitySnippetHook();
  } else if (gSnippetHookCreated && gSnippetTargetAddr) {
    MH_DisableHook(gSnippetTargetAddr);
    gSnippetHookActive.store(false, std::memory_order_release);
  }

  const bool wasInstalled = gInstalled.load(std::memory_order_acquire);
  if (!wasInstalled)
    RestartPreHookChildWorkers();

  gInstalled.store(true, std::memory_order_release);
  ReleaseSRWLockExclusive(&gLifecycleLock);
  return DXCB5_HOOK_OK;
}

__declspec(dllexport) void dxcb5_hook_uninstall() {
  AcquireSRWLockExclusive(&gLifecycleLock);
  if (!gInstalled.exchange(false, std::memory_order_acq_rel)) {
    ReleaseSRWLockExclusive(&gLifecycleLock);
    return;
  }
  gEnabled.store(false, std::memory_order_release);
  WriteSessionStateFile();

  if (gMinHookReady)
    MH_DisableHook(MH_ALL_HOOKS);
  gSnippetHookActive.store(false, std::memory_order_release);

  // Wait for any in-flight hook invocations to drain cleanly (up to 2s).
  for (int i = 0; i < 2000 && gInFlightCalls.load(std::memory_order_acquire) > 0;
       ++i) {
    Sleep(1);
  }
  AcquireSRWLockShared(&gStateLock);
  const std::wstring stagingDir = gStagingDir;
  ReleaseSRWLockShared(&gStateLock);
  DetachReparsePointDirectoriesInDir(stagingDir);
  gReplayCache.Clear();
  CleanupSessionDirOnUninstall();
  ReleaseSRWLockExclusive(&gLifecycleLock);
}

__declspec(dllexport) int dxcb5_hook_is_installed() {
  return gInstalled.load(std::memory_order_acquire) ? 1 : 0;
}

__declspec(dllexport) void dxcb5_hook_set_enabled(int enabled) {
  gEnabled.store(enabled != 0, std::memory_order_release);
  WriteSessionStateFile();
}

__declspec(dllexport) void dxcb5_hook_set_nan_safe(int nanSafe) {
  const bool next = (nanSafe != 0);
  if (gNanSafe.exchange(next, std::memory_order_acq_rel) != next) {
    gReplayCache.Clear();
    WriteSessionStateFile();
  }
}

__declspec(dllexport) void dxcb5_hook_register_manifest(
    const wchar_t *shaderAssetPath, const wchar_t *manifestJson,
    const wchar_t *pairEntries) {
  if (!shaderAssetPath || !*shaderAssetPath || !manifestJson || !*manifestJson)
    return;
  const std::string pathU8 = WideToUtf8(shaderAssetPath);
  const std::string manU8 = WideToUtf8(manifestJson);
  const std::string pairU8 = pairEntries ? WideToUtf8(pairEntries) : std::string();
  const std::string norm = gManifestRegistry.Register(pathU8, manU8, pairU8);
  if (!norm.empty()) {
    RegisteredManifest reg;
    if (gManifestRegistry.Find(norm, reg)) {
      AcquireSRWLockShared(&gStateLock);
      const std::wstring sessionDir = gSessionDir;
      ReleaseSRWLockShared(&gStateLock);
      WriteManifestFileToSession(sessionDir, norm, reg);
    }
  }
  WriteSessionStateFile();
  gReplayCache.Clear();
}

__declspec(dllexport) void dxcb5_hook_clear_manifests() {
  gManifestRegistry.Clear();
  DeleteAllManifestFilesInSession();
  WriteSessionStateFile();
  gReplayCache.Clear();
}

__declspec(dllexport) void dxcb5_hook_clear_cache() {
  gReplayCache.Clear();
  ClearWorkerDiskCachesInSession();
}

__declspec(dllexport) void dxcb5_hook_reload_workers() {
  RestartPreHookChildWorkers();
  gReplayCache.Clear();
  ClearWorkerDiskCachesInSession();
}

__declspec(dllexport) void dxcb5_hook_get_stats(uint64_t *ramHits,
                                                uint64_t *ipcCalls,
                                                uint64_t *workersHooked,
                                                uint64_t *snippetHookActive) {
  if (ramHits)
    *ramHits = gRamHits.load(std::memory_order_relaxed);
  if (ipcCalls)
    *ipcCalls = gIpcCalls.load(std::memory_order_relaxed);
  if (workersHooked)
    *workersHooked = gWorkersHooked.load(std::memory_order_relaxed);
  if (snippetHookActive)
    *snippetHookActive =
        gSnippetHookActive.load(std::memory_order_relaxed) ? 1 : 0;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
  // Never run MinHook / SRWLOCK / SuspendThread inside DllMain (Loader Lock &
  // process-termination deadlock hazard). Cleanup is explicit via
  // dxcb5_hook_uninstall() on EditorApplication.quitting, and our module is
  // pinned once installed.
  return TRUE;
}
