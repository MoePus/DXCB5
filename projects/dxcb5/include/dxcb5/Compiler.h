// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_COMPILER_H
#define DXCB5_COMPILER_H

#include "dxc/Support/WinIncludes.h"
#include "dxcb5/Mir.h"
#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class Instruction;
class Module;
class raw_ostream;
}

namespace dxcb5 {

using VectorPhiGroup = std::array<const llvm::Instruction *, 4>;

// VertexOutput is empty for vertex shaders. For pixel shaders it contains the
// decoded output linkage of the SM5.0 vertex producer.
HRESULT Compile(llvm::Module &Module,
                llvm::ArrayRef<SignatureElement> VertexOutput,
                llvm::raw_ostream &Output, llvm::raw_ostream &Diagnostics,
                llvm::ArrayRef<VectorPhiGroup> PhiGroups = {},
                bool NanSafe = false);

} // namespace dxcb5
#endif
