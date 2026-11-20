// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_TEXT_H
#define DXCB5_MIR_TEXT_H

#include "dxcb5/Mir.h"
#include "llvm/ADT/StringRef.h"

namespace dxcb5 {

std::string PrintMir(const MirProgram &Program);

// On failure, Program retains its original contents.
bool ParseMir(llvm::StringRef Text, MirProgram &Program, std::string &Error);

} // namespace dxcb5
#endif
