// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_UNITY_HOOK_VARIANT_CACHE_AND_CONTEXT_H
#define DXCB5_UNITY_HOOK_VARIANT_CACHE_AND_CONTEXT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcommon.h>
#include "UnityTypes.h"
#include "SpecCacheBridge.h"
#include "dxcb5/Formats.h"
#include "dxcb5/Sha256.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxcb5_hook {

inline uint64_t Fnv1a64(const void *data, size_t len,
                        uint64_t seed = 14695981039346656037ull) {
  const auto *p = static_cast<const uint8_t *>(data);
  uint64_t h = seed;
  for (size_t i = 0; i < len; ++i)
    h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

inline uint64_t Fnv1a64Str(std::string_view s,
                           uint64_t seed = 14695981039346656037ull) {
  return Fnv1a64(s.data(), s.size(), seed);
}

inline uint64_t Fnv1a64Delim(std::string_view s, uint64_t seed) {
  uint64_t h = Fnv1a64(s.data(), s.size(), seed);
  const uint8_t sep = 0x1Fu;
  return Fnv1a64(&sep, 1, h);
}

inline std::wstring Utf8ToWide(std::string_view s) {
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

inline std::string WideToUtf8(std::wstring_view w) {
  if (w.empty())
    return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                    static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0)
    return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      &out[0], n, nullptr, nullptr);
  return out;
}

inline std::string NormalizeAssetPath(std::string_view p) {
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

inline std::string Hex64(uint64_t v) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

inline bool EqualsI(const std::wstring &a, const std::wstring &b) {
  return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

inline std::wstring GetEnvVarW(const wchar_t *name) {
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

inline bool ResolveProjectRootW(std::wstring &outRoot,
                                const std::wstring &hintDir = {}) {
  auto trimDir = [](std::wstring s) {
    for (wchar_t &c : s)
      if (c == L'/')
        c = L'\\';
    while (s.size() > 3 && s.back() == L'\\')
      s.pop_back();
    return s;
  };
  const std::wstring envDir = GetEnvVarW(L"DXCB5_UNITY_PROJECT_DIR");
  if (!envDir.empty()) {
    outRoot = trimDir(envDir);
    return true;
  }
  if (!hintDir.empty()) {
    outRoot = trimDir(hintDir);
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

// Register a dynamically allocated MinHook trampoline as a valid CFG call
// target when running under Windows Control Flow Guard. Marked noinline +
// guard(nocf) because kernelbase!SetProcessValidCallTargets is CFG-suppressed
// for indirect calls via GetProcAddress.
__declspec(noinline) __declspec(guard(nocf)) inline void
RegisterCfgCallTarget(void *fnAddr) {
  if (!fnAddr)
    return;
  HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
  if (!kb)
    return;
  using PFN_SetProcessValidCallTargets =
      BOOL(WINAPI *)(HANDLE, PVOID, SIZE_T, ULONG, PCFG_CALL_TARGET_INFO);
  auto fn = reinterpret_cast<PFN_SetProcessValidCallTargets>(
      GetProcAddress(kb, "SetProcessValidCallTargets"));
  if (!fn)
    return;
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(fnAddr, &mbi, sizeof(mbi)) == 0 || !mbi.BaseAddress)
    return;
  CFG_CALL_TARGET_INFO info{};
  info.Offset = (reinterpret_cast<uintptr_t>(fnAddr) -
                 reinterpret_cast<uintptr_t>(mbi.BaseAddress)) &
                ~uintptr_t(0xF);
  info.Flags = CFG_CALL_TARGET_VALID;
  fn(GetCurrentProcess(), mbi.BaseAddress, mbi.RegionSize, 1, &info);
}

inline bool FileExistsW(const std::wstring &p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool EnsureDirTreeW(const std::wstring &dir) {
  if (dir.empty())
    return false;
  std::wstring cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    wchar_t c = dir[i];
    if (c == L'/')
      c = L'\\';
    cur.push_back(c);
    if (c == L'\\' && cur.size() > 3)
      CreateDirectoryW(cur.c_str(), nullptr);
  }
  CreateDirectoryW(cur.c_str(), nullptr);
  const DWORD a = GetFileAttributesW(cur.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool CopyFileAtomicIfNewer(const std::wstring &src,
                                  const std::wstring &dst) {
  WIN32_FILE_ATTRIBUTE_DATA sAttr{}, dAttr{};
  if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &sAttr))
    return false;
  auto sameStamp = [&](const WIN32_FILE_ATTRIBUTE_DATA &a,
                       const WIN32_FILE_ATTRIBUTE_DATA &b) {
    return a.nFileSizeLow == b.nFileSizeLow &&
           a.nFileSizeHigh == b.nFileSizeHigh &&
           a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime &&
           a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime;
  };
  if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &dAttr) &&
      sameStamp(sAttr, dAttr)) {
    return true;
  }
  wchar_t suf[64];
  std::swprintf(suf, 64, L".tmp.%lu.%lu",
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long>(GetCurrentThreadId()));
  const std::wstring tmp = dst + suf;
  if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE))
    return false;
  if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    // Succeed ONLY if another concurrent worker already landed the exact same
    // file; never falsely report success when an older locked DLL remains (L1).
    return GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &dAttr) &&
           sameStamp(sAttr, dAttr);
  }
  return true;
}

// Safely detach any legacy NTFS directory junctions / symlinks directly inside
// `dir` using RemoveDirectoryW ONLY (never recursing into reparse points!).
// Returns true iff every discovered reparse-point directory was removed (M4).
inline bool DetachReparsePointDirectoriesInDir(const std::wstring &dir) {
  if (dir.empty())
    return true;
  std::wstring base = dir;
  for (wchar_t &c : base)
    if (c == L'/')
      c = L'\\';
  while (base.size() > 3 && base.back() == L'\\')
    base.pop_back();
  WIN32_FIND_DATAW fd{};
  HANDLE hFind = FindFirstFileW((base + L"\\*").c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE)
    return true;
  bool allRemoved = true;
  do {
    if (std::wcscmp(fd.cFileName, L".") == 0 ||
        std::wcscmp(fd.cFileName, L"..") == 0)
      continue;
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      // RemoveDirectoryW on a reparse point removes ONLY the link shell and
      // never deletes any files in the target directory.
      if (!RemoveDirectoryW((base + L"\\" + fd.cFileName).c_str()))
        allRemoved = false;
    }
  } while (FindNextFileW(hFind, &fd));
  FindClose(hFind);
  return allRemoved;
}

// Copy ONLY actual sibling `*/CgBatchPlugins64/*.dll` files into staging as
// plain regular files. NEVER create NTFS junctions or directory symlinks, and
// NEVER traverse or write through any residual reparse-point directory (M4)!
inline void CopyOtherCgBatchPluginsSafe(const std::wstring &origPluginsDir,
                                        const std::wstring &stagingDir) {
  DetachReparsePointDirectoriesInDir(stagingDir);
  if (origPluginsDir.empty() || EqualsI(origPluginsDir, stagingDir))
    return;
  std::wstring search = origPluginsDir;
  for (wchar_t &c : search)
    if (c == L'/')
      c = L'\\';
  while (search.size() > 3 && search.back() == L'\\')
    search.pop_back();

  WIN32_FIND_DATAW fd{};
  HANDLE hFind = FindFirstFileW((search + L"\\*").c_str(), &fd);
  if (hFind == INVALID_HANDLE_VALUE)
    return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
      continue;
    if (std::wcscmp(fd.cFileName, L".") == 0 ||
        std::wcscmp(fd.cFileName, L"..") == 0 ||
        _wcsicmp(fd.cFileName, L"DXCB5") == 0)
      continue;
    const std::wstring srcPlugDir =
        search + L"\\" + fd.cFileName + L"\\CgBatchPlugins64";
    WIN32_FIND_DATAW dllFd{};
    HANDLE hDll = FindFirstFileW((srcPlugDir + L"\\*.dll").c_str(), &dllFd);
    if (hDll == INVALID_HANDLE_VALUE)
      continue;
    const std::wstring dstSiblingRoot = stagingDir + L"\\" + fd.cFileName;
    const DWORD dstRootAttrs = GetFileAttributesW(dstSiblingRoot.c_str());
    if (dstRootAttrs != INVALID_FILE_ATTRIBUTES &&
        (dstRootAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      // Legacy junction could not be detached (e.g. locked by a worker); refuse
      // to write through it into Unity's PlaybackEngines directory!
      FindClose(hDll);
      continue;
    }
    const std::wstring dstPlugDir = dstSiblingRoot + L"\\CgBatchPlugins64";
    const DWORD dstPlugAttrs = GetFileAttributesW(dstPlugDir.c_str());
    if (dstPlugAttrs != INVALID_FILE_ATTRIBUTES &&
        (dstPlugAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      FindClose(hDll);
      continue;
    }
    if (EnsureDirTreeW(dstPlugDir)) {
      do {
        if ((dllFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (dllFd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
          continue;
        CopyFileAtomicIfNewer(srcPlugDir + L"\\" + dllFd.cFileName,
                              dstPlugDir + L"\\" + dllFd.cFileName);
      } while (FindNextFileW(hDll, &dllFd));
    }
    FindClose(hDll);
  } while (FindNextFileW(hFind, &fd));
  FindClose(hFind);
}

struct CmdArgSpan {
  size_t Start = 0;
  size_t End = 0;
  std::wstring Unquoted;
};

// Parses Windows command-line spans honoring CommandLineToArgvW backslash+quote rules.
inline std::vector<CmdArgSpan> ParseWindowsCmdLineSpans(const std::wstring &cmdLine) {
  std::vector<CmdArgSpan> args;
  size_t i = 0;
  const size_t n = cmdLine.size();
  while (i < n) {
    while (i < n && (cmdLine[i] == L' ' || cmdLine[i] == L'\t'))
      ++i;
    if (i >= n)
      break;
    const size_t start = i;
    std::wstring unq;
    bool inQ = false;
    while (i < n) {
      if (!inQ && (cmdLine[i] == L' ' || cmdLine[i] == L'\t'))
        break;
      if (cmdLine[i] == L'\\') {
        size_t bs = 0;
        while (i < n && cmdLine[i] == L'\\') {
          ++bs;
          ++i;
        }
        if (i < n && cmdLine[i] == L'"') {
          unq.append(bs / 2, L'\\');
          if ((bs & 1u) != 0) {
            unq.push_back(L'"');
          } else {
            inQ = !inQ;
          }
          ++i;
        } else {
          unq.append(bs, L'\\');
        }
        continue;
      }
      if (cmdLine[i] == L'"') {
        if (inQ && i + 1 < n && cmdLine[i + 1] == L'"') {
          unq.push_back(L'"');
          i += 2;
        } else {
          inQ = !inQ;
          ++i;
        }
        continue;
      }
      unq.push_back(cmdLine[i++]);
    }
    args.push_back({start, i, std::move(unq)});
  }
  return args;
}

inline bool BasenameEqualsI(std::wstring_view path, const wchar_t *targetName) {
  if (path.empty() || !targetName)
    return false;
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring_view base =
      (slash == std::wstring_view::npos) ? path : path.substr(slash + 1);
  return base.size() == std::wcslen(targetName) &&
         _wcsnicmp(base.data(), targetName, base.size()) == 0;
}

// Only matches when lpApplicationName or argv[0] is UnityShaderCompiler.exe
// (never matches unrelated tools like `taskkill /im UnityShaderCompiler.exe`).
inline bool IsUnityShaderCompilerInvocation(LPCWSTR lpApplicationName,
                                            LPCWSTR lpCommandLine) {
  if (lpApplicationName && *lpApplicationName &&
      BasenameEqualsI(lpApplicationName, L"UnityShaderCompiler.exe"))
    return true;
  if (!lpCommandLine || !*lpCommandLine)
    return false;
  const auto args = ParseWindowsCmdLineSpans(lpCommandLine);
  return !args.empty() &&
         BasenameEqualsI(args[0].Unquoted, L"UnityShaderCompiler.exe");
}

// Replaces `[pluginsPath]` (argv[5]) in UnityShaderCompiler's command line with
// `stagingDir`, returning the original `pluginsPath` (supports empty `""`).
inline bool RewriteShaderCompilerCmdLine(const std::wstring &cmdLine,
                                         const std::wstring &stagingDir,
                                         std::wstring &outNewCmdLine,
                                         std::wstring &outOrigPluginsDir) {
  const auto args = ParseWindowsCmdLineSpans(cmdLine);
  if (args.size() < 5)
    return false;

  std::wstring fwdStaging = stagingDir;
  for (wchar_t &c : fwdStaging)
    if (c == L'\\')
      c = L'/';
  while (fwdStaging.size() > 3 && fwdStaging.back() == L'/')
    fwdStaging.pop_back();

  // UnityShaderCompiler.exe CLI: <exe> <platform> <logDir> <ipcName> <pid> [pluginsPath] ...
  // Replace argv[5] (even when passed as empty `""`) if it is not a `-flag`.
  int pluginsIdx = -1;
  if (args.size() >= 6 &&
      (args[5].Unquoted.empty() || args[5].Unquoted[0] != L'-')) {
    pluginsIdx = 5;
  } else {
    for (int k = static_cast<int>(args.size()) - 1; k >= 5; --k) {
      if (args[k].Unquoted.empty() || args[k].Unquoted[0] != L'-') {
        pluginsIdx = k;
        break;
      }
    }
  }
  if (pluginsIdx >= 0) {
    outOrigPluginsDir = args[pluginsIdx].Unquoted;
    outNewCmdLine = cmdLine.substr(0, args[pluginsIdx].Start) + L"\"" +
                    fwdStaging + L"\"" + cmdLine.substr(args[pluginsIdx].End);
    return true;
  }
  outOrigPluginsDir.clear();
  outNewCmdLine = cmdLine + L" \"" + fwdStaging + L"\"";
  return true;
}

// Builds a double-null-terminated Unicode environment block preserving existing
// entries (including `=C:=...` drive-cwd entries) while overriding/adding `extras`.
inline void BuildAugmentedUnicodeEnv(
    LPVOID lpEnvironment, DWORD creationFlags,
    const std::vector<std::pair<std::wstring, std::wstring>> &extras,
    std::vector<wchar_t> &outBlock) {
  std::vector<std::wstring> entries;
  if (!lpEnvironment) {
    LPWCH raw = GetEnvironmentStringsW();
    if (raw) {
      for (const wchar_t *p = raw; *p; p += std::wcslen(p) + 1)
        entries.emplace_back(p);
      FreeEnvironmentStringsW(raw);
    }
  } else if (creationFlags & CREATE_UNICODE_ENVIRONMENT) {
    for (const wchar_t *p = static_cast<const wchar_t *>(lpEnvironment); *p;
         p += std::wcslen(p) + 1)
      entries.emplace_back(p);
  } else {
    for (const char *p = static_cast<const char *>(lpEnvironment); *p;
         p += std::strlen(p) + 1) {
      const int n = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
      if (n > 1) {
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, p, -1, &w[0], n);
        w.pop_back();
        entries.push_back(std::move(w));
      }
    }
  }

  auto isOverridden = [&](const std::wstring &line) {
    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos || eq == 0)
      return false;
    const std::wstring key = line.substr(0, eq);
    for (const auto &kv : extras)
      if (EqualsI(key, kv.first))
        return true;
    return false;
  };

  outBlock.clear();
  for (const auto &line : entries) {
    if (isOverridden(line))
      continue;
    outBlock.insert(outBlock.end(), line.begin(), line.end());
    outBlock.push_back(L'\0');
  }
  for (const auto &kv : extras) {
    if (kv.first.empty())
      continue;
    const std::wstring line = kv.first + L"=" + kv.second;
    outBlock.insert(outBlock.end(), line.begin(), line.end());
    outBlock.push_back(L'\0');
  }
  outBlock.push_back(L'\0');
}

struct CachedSnippetResult {
  uint32_t OutProgramType = 0;
  std::vector<uint8_t> Code;
  RecordedReflection Reflection;
  bool HasReflection = false;
  size_t Bytes = 0;
};

class VariantReplayCache {
public:
  static constexpr size_t kMaxEntries = 4096;
  static constexpr size_t kMaxTotalBytes = 192ull * 1024ull * 1024ull;

  VariantReplayCache() { InitializeSRWLock(&Lock); }

  bool Lookup(const std::string &key, bool needReflection,
              CachedSnippetResult &out) {
    AcquireSRWLockShared(&Lock);
    auto it = Map.find(key);
    bool hit = (it != Map.end()) &&
               (!needReflection || it->second->second.HasReflection);
    if (hit)
      out = it->second->second;
    ReleaseSRWLockShared(&Lock);
    return hit;
  }

  void Insert(const std::string &key, CachedSnippetResult val) {
    val.Bytes = val.Code.size() + val.Reflection.ApproxBytes() + key.size() + 64;
    if (val.Bytes > kMaxTotalBytes / 4)
      return;
    AcquireSRWLockExclusive(&Lock);
    auto it = Map.find(key);
    if (it != Map.end()) {
      // Never overwrite a reflection-bearing entry with one lacking reflection.
      if (it->second->second.HasReflection && !val.HasReflection) {
        ReleaseSRWLockExclusive(&Lock);
        return;
      }
      TotalBytes -= it->second->second.Bytes;
      Lru.erase(it->second);
      Map.erase(it);
    }
    while ((Map.size() >= kMaxEntries || TotalBytes + val.Bytes > kMaxTotalBytes) &&
           !Lru.empty()) {
      TotalBytes -= Lru.back().second.Bytes;
      Map.erase(Lru.back().first);
      Lru.pop_back();
    }
    TotalBytes += val.Bytes;
    Lru.emplace_front(key, std::move(val));
    Map[key] = Lru.begin();
    ReleaseSRWLockExclusive(&Lock);
  }

  void Clear() {
    AcquireSRWLockExclusive(&Lock);
    Map.clear();
    Lru.clear();
    TotalBytes = 0;
    ReleaseSRWLockExclusive(&Lock);
  }

private:
  using EntryList = std::list<std::pair<std::string, CachedSnippetResult>>;
  SRWLOCK Lock{};
  EntryList Lru;
  std::unordered_map<std::string, EntryList::iterator> Map;
  size_t TotalBytes = 0;
};

struct RegisteredManifest {
  std::string ManifestJson;
  std::string PairEntries; // Optional "vsEntry:psEntry"
  uint64_t Hash = 0;
};

class ManifestRegistry {
public:
  ManifestRegistry() { InitializeSRWLock(&Lock); }

  std::string Register(std::string_view assetPath, std::string manifestJson,
                       std::string pairEntries) {
    const std::string norm = NormalizeAssetPath(assetPath);
    if (norm.empty() || manifestJson.empty())
      return {};
    uint64_t h = Fnv1a64Delim(manifestJson, 14695981039346656037ull);
    h = Fnv1a64Delim(pairEntries, h);
    AcquireSRWLockExclusive(&Lock);
    Map[norm] = {std::move(manifestJson), std::move(pairEntries), h};
    ReleaseSRWLockExclusive(&Lock);
    return norm;
  }

  bool Find(std::string_view assetPath, RegisteredManifest &out) {
    const std::string norm = NormalizeAssetPath(assetPath);
    if (norm.empty())
      return false;
    AcquireSRWLockShared(&Lock);
    auto it = Map.find(norm);
    bool ok = (it != Map.end());
    if (ok)
      out = it->second;
    ReleaseSRWLockShared(&Lock);
    return ok;
  }

  std::vector<std::pair<std::string, RegisteredManifest>> Snapshot() {
    std::vector<std::pair<std::string, RegisteredManifest>> out;
    AcquireSRWLockShared(&Lock);
    out.reserve(Map.size());
    for (const auto &kv : Map)
      out.emplace_back(kv.first, kv.second);
    ReleaseSRWLockShared(&Lock);
    return out;
  }

  void Clear() {
    AcquireSRWLockExclusive(&Lock);
    Map.clear();
    ReleaseSRWLockExclusive(&Lock);
  }

private:
  SRWLOCK Lock{};
  std::unordered_map<std::string, RegisteredManifest> Map;
};

struct Hash128 {
  dxcb5::Sha256 Hashed;

  Hash128() {
    const uint32_t Epoch = DXCB5_CACHE_EPOCH;
    Hashed.UpdatePod(Epoch);
  }
  void MixBytes(const void *data, size_t len) { Hashed.Update(data, len); }
  template <typename T>
  void MixPod(const T &v) {
    Hashed.UpdatePod(v);
  }
  void MixDelim(std::string_view s) { Hashed.UpdateDelim(s); }
  void MixKeywordList(char tag, const std::vector<std::string_view> &kws) {
    MixPod(tag);
    const uint64_t count = static_cast<uint64_t>(kws.size());
    MixPod(count);
    for (std::string_view kw : kws)
      MixDelim(kw);
  }
  std::string Hex() const { return Hashed.Hex64(); }
};

inline bool CollectSortedKeywords(const UnityCoreVector<UnityCoreString> &v,
                                  std::vector<std::string_view> &out) {
  const size_t n = v.Count();
  if (n > 2048)
    return false;
  out.clear();
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    std::string_view sv;
    if (!v.Data[i].TryView(sv))
      return false;
    if (!sv.empty())
      out.push_back(sv);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

// Verified against Unity 2022.3.22f1 disassembly of ShaderCompilerCompileSnippet:
//   platformKeywords = enabled platform_caps_keywords + graphics tier define
//   enabledKeywords  = enabled shader variant keywords (global + local)
//   disabledKeywords = EnumerateDisabledKeywordsInVariantsArray + disabled caps
//   userDisabledKeywordCount = disabledKeywords.size() before disabled caps
// IMPORTANT: enabledKeywords and disabledKeywords form a partition of all
// declared shader keywords, so they MUST be kept strictly separate!
inline bool BuildSnippetCacheKey(
    std::string_view inputCode, uint64_t codeHash, std::string_view inputPath,
    std::string_view shaderName, int startLine, std::string_view entryName,
    const UnityCoreVector<UnityCoreString> &platformKeywords,
    const UnityCoreVector<UnityCoreString> &enabledKeywords,
    const UnityCoreVector<UnityCoreString> &disabledKeywords,
    uint64_t userDisabledKeywordCount, int buildTarget, uint64_t arg12,
    bool arg13, bool arg14, uint64_t compilationFlags, int sourceLang,
    int programType, uint32_t programMask, int api, uint64_t requirements,
    uint64_t manifestHash, bool nanSafe, const std::wstring &projectDir,
    std::string &outKey) {
  std::vector<std::string_view> platformKws, enabledKws, disabledKws;
  if (!CollectSortedKeywords(platformKeywords, platformKws) ||
      !CollectSortedKeywords(enabledKeywords, enabledKws) ||
      !CollectSortedKeywords(disabledKeywords, disabledKws))
    return false;

  const uint8_t flagsByte =
      static_cast<uint8_t>((arg13 ? 1u : 0u) | (arg14 ? 2u : 0u) |
                           (nanSafe ? 4u : 0u));

  // Fast path for two-stage spec stubs: resolve the active dxcb5-spec directive
  // under preprocessor conditionals (`#if defined(POINT) ... #elif defined(SPOT)
  // ... #else ...`) using tri-state macro knowledge (enabled/platform = True,
  // disabled = False, anything else = Unknown -> fail-safe fallback to `var:`).
  dxcb5_proxy::PreprocMacroOracle oracle;
  oracle.UnknownAsFalse = false;
  for (std::string_view k : disabledKws)
    oracle.Known[std::string(k)] = dxcb5_proxy::MacroDefState::Undefined;
  for (std::string_view k : platformKws)
    oracle.Known[std::string(k)] = dxcb5_proxy::MacroDefState::DefinedNonZero;
  for (std::string_view k : enabledKws)
    oracle.Known[std::string(k)] = dxcb5_proxy::MacroDefState::DefinedNonZero;
  oracle.Known["SHADER_STAGE_VERTEX"] =
      (programType == kProgramVertex)
          ? dxcb5_proxy::MacroDefState::DefinedNonZero
          : dxcb5_proxy::MacroDefState::Undefined;
  oracle.Known["SHADER_STAGE_FRAGMENT"] =
      (programType == kProgramFragment)
          ? dxcb5_proxy::MacroDefState::DefinedNonZero
          : dxcb5_proxy::MacroDefState::Undefined;

  dxcb5_proxy::SpecStubDirective stub;
  if (dxcb5_proxy::ResolveSpecStubDirective(inputCode.data(), inputCode.size(),
                                            std::move(oracle), stub)) {
    dxcb5_proxy::ActiveDefineMap active;
    dxcb5_proxy::CollectActiveDefines(nullptr, inputCode.data(),
                                      inputCode.size(), active);
    for (std::string_view k : platformKws)
      active.Entries[std::string(k)] = true;
    for (std::string_view k : enabledKws)
      active.Entries[std::string(k)] = true;
    const std::wstring archW = dxcb5_proxy::DetectArchetypeDir(active);
    std::wstring projectRoot, cacheRoot;
    if (ResolveProjectRootW(projectRoot, projectDir) &&
        dxcb5_proxy::ResolveSpecCacheDir(Utf8ToWide(stub.CacheDir), projectRoot,
                                         cacheRoot)) {
      const std::wstring metaPath = cacheRoot + L"\\" +
                                    Utf8ToWide(stub.PairKey) + L"\\" + archW +
                                    L"\\meta.json";
      std::string metaJson;
      if (dxcb5_proxy::ReadWholeFile(metaPath, metaJson) && !metaJson.empty()) {
        std::vector<std::string> specFlags;
        const char *stageKey =
            (programType == kProgramFragment) ? "\"ps_live_spec_flags\""
                                              : "\"vs_live_spec_flags\"";
        dxcb5_proxy::ParseSpecFlagsList(metaJson, specFlags, stageKey);
        Hash128 h;
        h.MixDelim(stub.CacheDir);
        h.MixDelim(stub.PairKey);
        h.MixBytes(archW.data(), archW.size() * sizeof(wchar_t));
        h.MixDelim(entryName);
        h.MixPod(programType);
        h.MixPod(api);
        h.MixPod(buildTarget);
        h.MixPod(sourceLang);
        h.MixPod(programMask);
        h.MixPod(compilationFlags);
        h.MixPod(requirements);
        h.MixPod(arg12);
        h.MixPod(flagsByte);
        h.MixDelim(metaJson);
        for (const auto &flag : specFlags) {
          const uint8_t on = active.IsSet(flag.c_str()) ? 1u : 0u;
          h.MixDelim(flag);
          h.MixPod(on);
        }
        outKey = "stub:" + h.Hex();
        return true;
      }
    }
  }

  Hash128 h;
  h.MixPod(codeHash);
  h.MixDelim(inputCode);
  h.MixDelim(inputPath);
  h.MixDelim(shaderName);
  h.MixPod(startLine);
  h.MixDelim(entryName);
  h.MixPod(programType);
  h.MixPod(api);
  h.MixPod(buildTarget);
  h.MixPod(sourceLang);
  h.MixPod(programMask);
  h.MixPod(compilationFlags);
  h.MixPod(requirements);
  h.MixPod(userDisabledKeywordCount);
  h.MixPod(arg12);
  h.MixPod(flagsByte);
  h.MixPod(manifestHash);
  h.MixKeywordList('P', platformKws);
  h.MixKeywordList('E', enabledKws);
  h.MixKeywordList('D', disabledKws);
  outKey = "var:" + h.Hex();
  return true;
}

} // namespace dxcb5_hook

#endif // DXCB5_UNITY_HOOK_VARIANT_CACHE_AND_CONTEXT_H
