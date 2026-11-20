// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_IMPORT_H
#define DXCB5_IMPORT_H

#include "dxcb5/Mir.h"
#include "llvm/ADT/ArrayRef.h"
#include <map>

namespace hlsl {
class DxilModule;
class DxilResourceBase;
}

namespace dxcb5 {

using SignatureMap = std::map<std::pair<unsigned, unsigned>, unsigned>;

struct Interface {
  SignatureMap Input;
  SignatureMap Output;
  std::map<const hlsl::DxilResourceBase *, BoundResource> ResourceArrays;
};

bool ImportInterface(hlsl::DxilModule &Module,
                     llvm::ArrayRef<SignatureElement> Vertex,
                     MirProgram &Program, Interface &Layout, std::string &Error);

} // namespace dxcb5
#endif
