// Distributed under the University of Illinois Open Source License.
#include "dxcb5/Sha256.h"
#include "dxcb5/SpecEngine.h"

std::string dxcb5::spec::ComputeSha256Hex(const void *Data, size_t Size) {
  dxcb5::Sha256 H;
  H.Update(Data, Size);
  return H.Hex64();
}
