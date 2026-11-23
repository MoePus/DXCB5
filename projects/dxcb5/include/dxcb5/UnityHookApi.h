// Distributed under the University of Illinois Open Source License.
// Single Source of Truth C ABI header for dxcb5_unity_hook.dll.
// Must match P/Invoke declarations in Dxcb5HookBridge.cs.
#ifndef DXCB5_UNITY_HOOK_API_H
#define DXCB5_UNITY_HOOK_API_H

#include "dxcb5/Diagnostics.h"
#include <stddef.h>
#include <stdint.h>

#define DXCB5_UNITY_HOOK_ABI_VERSION 1u

#ifdef DXCB5_UNITY_HOOK_EXPORTS
#define DXCB5_HOOK_API __declspec(dllexport)
#else
#define DXCB5_HOOK_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

DXCB5_HOOK_API uint32_t dxcb5_hook_get_abi_version(void);

DXCB5_HOOK_API int dxcb5_hook_install(const wchar_t *pluginDir,
                                      const wchar_t *tempDir,
                                      int enableSnippetCache);
DXCB5_HOOK_API void dxcb5_hook_uninstall(void);
DXCB5_HOOK_API int dxcb5_hook_is_installed(void);
DXCB5_HOOK_API void dxcb5_hook_set_enabled(int enabled);
DXCB5_HOOK_API void dxcb5_hook_set_nan_safe(int nanSafe);
DXCB5_HOOK_API void dxcb5_hook_register_manifest(const wchar_t *shaderAssetPath,
                                                 const wchar_t *manifestJson,
                                                 const wchar_t *pairEntries);
DXCB5_HOOK_API void dxcb5_hook_clear_manifests(void);
DXCB5_HOOK_API void dxcb5_hook_clear_cache(void);
DXCB5_HOOK_API void dxcb5_hook_reload_workers(void);
DXCB5_HOOK_API void dxcb5_hook_get_stats(uint64_t *ramHits, uint64_t *ipcCalls,
                                         uint64_t *workersHooked,
                                         uint64_t *snippetHookActive);

typedef uint32_t (*PFN_dxcb5_hook_get_abi_version)(void);
typedef int (*PFN_dxcb5_hook_install)(const wchar_t *, const wchar_t *, int);
typedef void (*PFN_dxcb5_hook_uninstall)(void);
typedef int (*PFN_dxcb5_hook_is_installed)(void);
typedef void (*PFN_dxcb5_hook_set_enabled)(int);
typedef void (*PFN_dxcb5_hook_set_nan_safe)(int);
typedef void (*PFN_dxcb5_hook_register_manifest)(const wchar_t *,
                                                 const wchar_t *,
                                                 const wchar_t *);
typedef void (*PFN_dxcb5_hook_clear_manifests)(void);
typedef void (*PFN_dxcb5_hook_clear_cache)(void);
typedef void (*PFN_dxcb5_hook_reload_workers)(void);
typedef void (*PFN_dxcb5_hook_get_stats)(uint64_t *, uint64_t *, uint64_t *,
                                         uint64_t *);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DXCB5_UNITY_HOOK_API_H
