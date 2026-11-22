// Distributed under the University of Illinois Open Source License.
// Unified diagnostic & status code definitions shared across DXCB5 C APIs
// (dxcompiler.dll spec/oracle exports and dxcb5_unity_hook.dll).
#ifndef DXCB5_DIAGNOSTICS_H
#define DXCB5_DIAGNOSTICS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dxcb5_status {
  DXCB5_OK = 0,
  DXCB5_ERR_INVALID_ARG = 1,
  DXCB5_ERR_ABI_MISMATCH = 2,
  DXCB5_ERR_INIT = 3,
  DXCB5_ERR_COMPILE = 4,
  DXCB5_ERR_PARSE = 5,
  DXCB5_ERR_SPEC = 6,
  DXCB5_ERR_HASH_MISMATCH = 7,
  DXCB5_ERR_OOM = 8,
  DXCB5_ERR_EXCEPTION = 9,
  DXCB5_ERR_INVALID_PASS = 10
} dxcb5_status;

typedef enum dxcb5_hook_status {
  DXCB5_HOOK_OK = 0,
  DXCB5_HOOK_ERR_INVALID_ARG = -1,
  DXCB5_HOOK_ERR_MINHOOK_INIT = -2,
  DXCB5_HOOK_ERR_CREATE_HOOK = -3,
  DXCB5_HOOK_ERR_ENABLE_HOOK = -4
} dxcb5_hook_status;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DXCB5_DIAGNOSTICS_H
