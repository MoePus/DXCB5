// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_SPEC_C_API_H
#define DXCB5_SPEC_C_API_H

#include "dxcb5/Diagnostics.h"
#include "dxcb5/Formats.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DXCB5_UBER_FLAG_NAN_SAFE    (1u << 0)
#define DXCB5_UBER_FLAG_GEC         (1u << 1)
#define DXCB5_UBER_FLAG_UNITY_ADAPT (1u << 2)
#define DXCB5_UBER_FLAG_ROW_MAJOR   (1u << 3)
#define DXCB5_UBER_FLAG_COL_MAJOR   (1u << 4)
#define DXCB5_UBER_FLAG_IEEE_STRICT (1u << 5)
#define DXCB5_UBER_FLAG_SKIP_OPT    (1u << 6)

typedef struct dxcb5_blob {
  const void *data;
  size_t size;
} dxcb5_blob;

typedef struct dxcb5_define {
  const char *name;
  const char *value; // nullable -> "1"
} dxcb5_define;

typedef struct dxcb5_uber_options {
  const char *entry;        // e.g. "PSMain"
  const char *target;       // "vs_5_0" | "ps_5_0"
  const char *hlsl_version; // "2018" | "2021" (nullable -> "2018")
  const char *source_name;  // nullable
  const char *const *include_dirs;
  uint32_t include_dir_count;
  const dxcb5_define *defines; // archetype defines (stereo / instancing)
  uint32_t define_count;
  uint32_t flags;       // DXCB5_UBER_FLAG_*
  uint32_t struct_size; // 0 (legacy/zero-init) or sizeof(dxcb5_uber_options)
} dxcb5_uber_options;

// Returns DXCB5_SPEC_ABI_VERSION so external callers can verify DLL ABI compatibility.
uint32_t dxcb5_get_abi_version(void);

// 0 = DXCB5_OK, non-zero = dxcb5_status error code (details in out_errors)
int dxcb5_compile_uber_dxil(
    const char *hlsl_utf8,
    size_t hlsl_size,
    const dxcb5_uber_options *opts,
    dxcb5_blob *out_dxil,
    dxcb5_blob *out_reflection_json, // optional
    dxcb5_blob *out_errors           // optional
);

int dxcb5_inspect_dxil(
    const void *dxil_data,
    size_t dxil_size,
    dxcb5_blob *out_reflection_json,
    dxcb5_blob *out_errors // optional
);

// Paired VS+PS specialization:
// 1) Material-level specialization (constants + const_textures, keeping
//    DXCB5_SpecFlags symbolic) + Cross-Stage DCE + pinned ISGN/OSGN layout
//    -> optional out_vs_dxil_opt / out_ps_dxil_opt
// 2) Apply manifest.defines (DXCB5_SpecFlags) + compile SM5.0 DXBC
//    -> optional out_vs_dxbc / out_ps_dxbc
int dxcb5_specialize_pair(
    const void *vs_dxil_data, size_t vs_dxil_size,
    const void *ps_dxil_data, size_t ps_dxil_size,
    const char *manifest_json_utf8, size_t manifest_size,
    dxcb5_blob *out_vs_dxil_opt, // optional
    dxcb5_blob *out_ps_dxil_opt, // optional
    dxcb5_blob *out_vs_dxbc,     // optional
    dxcb5_blob *out_ps_dxbc,     // optional
    dxcb5_blob *out_stats_json,  // optional (JSON array [vs_stats, ps_stats])
    dxcb5_blob *out_errors       // optional
);

// Single-stage specialization (used by Proxy on pDefines cache-miss over
// intermediate DXIL, or standalone single-stage specialization).
int dxcb5_specialize_stage(
    const void *stage_dxil_data,
    size_t stage_dxil_size,
    const char *manifest_json_utf8,
    size_t manifest_size,
    dxcb5_blob *out_dxbc,
    dxcb5_blob *out_stats_json, // optional
    dxcb5_blob *out_errors      // optional
);

void dxcb5_free_blob(dxcb5_blob *blob);

// Diagnostics / Oracle instrumentation exports (also listed in DXCompiler.dxcb5.def).
// Returns DXCB5_OK on valid pass names, DXCB5_ERR_INVALID_PASS otherwise.
int dxcb5_set_disabled_passes(const char *passes);
void dxcb5_configure_oracle(const char *dump_stages_dir,
                            const char *dump_metrics_path, int list_passes);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DXCB5_SPEC_C_API_H
