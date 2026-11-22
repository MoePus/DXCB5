// Distributed under the University of Illinois Open Source License.
// Single Source of Truth for DXCB5 ABI/schema versions and container magic checks.
#ifndef DXCB5_FORMATS_H
#define DXCB5_FORMATS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DXCB5_SPEC_ABI_VERSION 1u
#define DXCB5_SPEC_SCHEMA_VERSION 1u
#define DXCB5_ORACLE_SCHEMA_VERSION 1u
#define DXCB5_CACHE_EPOCH 2u
#define DXCB5_DXBC_MIN_CONTAINER_BYTES 32u

#ifdef __cplusplus
extern "C" {
#endif

// Validates the 32-byte DXBC container header magic ("DXBC") and declared total
// byte length before handing cached disk bytes back to a caller.
static inline int dxcb5_is_valid_dxbc_container_header(const void *data,
                                                       size_t size) {
  if (!data || size < DXCB5_DXBC_MIN_CONTAINER_BYTES)
    return 0;
  const uint8_t *bytes = (const uint8_t *)data;
  if (bytes[0] != 'D' || bytes[1] != 'X' || bytes[2] != 'B' || bytes[3] != 'C')
    return 0;
  uint32_t declared_size = 0;
  memcpy(&declared_size, bytes + 24, sizeof(declared_size));
  return declared_size == size ? 1 : 0;
}

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DXCB5_FORMATS_H
