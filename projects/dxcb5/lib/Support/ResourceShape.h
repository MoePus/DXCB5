// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_SUPPORT_RESOURCE_SHAPE_H
#define DXCB5_SUPPORT_RESOURCE_SHAPE_H

#include "dxc/DXIL/DxilConstants.h"
#include <cstdint>

namespace dxcb5 {

// Single source of truth for SM5 GetDimensions / resinfo shape properties
// shared between SpecTextureFold and Frontend LowerDxilResources.
struct Sm5ResourceShape {
  uint8_t KnownDimLanes = 0;   // Spatial dimensions (W[, H[, D]]) for mip-0 fold
  uint8_t ResInfoDestMask = 0; // SM5 resinfo destination writemask (0 = non-resinfo)
};

inline Sm5ResourceShape GetSm5ResourceShape(hlsl::DXIL::ResourceKind Kind) {
  using K = hlsl::DXIL::ResourceKind;
  switch (Kind) {
  case K::Texture1D:        return {1u, 0x9u};
  case K::Texture1DArray:   return {0u, 0xbu};
  case K::Texture2D:        return {2u, 0xbu};
  case K::TextureCube:      return {2u, 0xbu};
  case K::Texture2DArray:   return {0u, 0xfu};
  case K::Texture3D:        return {3u, 0xfu};
  case K::TextureCubeArray: return {0u, 0xfu};
  default:                  return {0u, 0x0u};
  }
}

} // namespace dxcb5

#endif
