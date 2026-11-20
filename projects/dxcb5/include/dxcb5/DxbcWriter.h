// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_DXBC_WRITER_H
#define DXCB5_DXBC_WRITER_H

#include "dxcb5/Mir.h"
#include "llvm/ADT/StringRef.h"

namespace dxcb5 {

bool WriteDxbc(const MirProgram &program, std::vector<uint8_t> &output,
               std::string &error);

// Read the output linkage of a previously compiled SM5.0 vertex shader.
// When requested, signaturePart is a view of its original tagged part.
bool ReadVertexOutputSignature(llvm::StringRef bytecode,
                               std::vector<SignatureElement> &output,
                               std::string &error,
                               llvm::StringRef *signaturePart = nullptr);

} // namespace dxcb5
#endif
