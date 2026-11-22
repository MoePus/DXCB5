// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_MEMORY_MEMORY_OPT_H
#define DXCB5_MIR_MEMORY_MEMORY_OPT_H

#include "Mir/SsaMir.h"

namespace dxcb5 {

// Encapsulated indexable-temp (x[]) optimization subsystem.
// External callers should use this header rather than internal Memory/ headers.
bool RunOptIndexableCopy(SsaProgram &Ssa, const SsaDomTree &Dom);
bool RunOptIndexableSsa(SsaProgram &Ssa, const SsaDomTree &Dom);
bool RunOptIndexableTemps(SsaProgram &Ssa);
bool RunOptIndexablePack(SsaProgram &Ssa);

} // namespace dxcb5

#endif
