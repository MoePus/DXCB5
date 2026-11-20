// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_DXBC_PARTS_H
#define DXCB5_DXBC_PARTS_H

#include "dxcb5/Mir.h"

namespace dxcb5 {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
         (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

std::vector<uint8_t> EncodeSignature(const std::vector<SignatureElement> &elements,
                                     bool output);
std::vector<uint8_t> EncodeResourceDefinitions(const MirProgram &program);

} // namespace dxcb5
#endif
