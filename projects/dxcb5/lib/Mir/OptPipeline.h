// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_OPT_PIPELINE_H
#define DXCB5_MIR_OPT_PIPELINE_H

#include "Mir/SsaMir.h"

namespace dxcb5 {

// Individual SSA MIR optimization passes (each returns true if it changed Ssa).
bool RunOptPromoteLocalArrays(SsaProgram &Ssa);
bool RunOptIndexableCopy(SsaProgram &Ssa, const SsaDomTree &Dom);
bool RunOptIndexableSsa(SsaProgram &Ssa, const SsaDomTree &Dom);
bool RunOptIndexableTemps(SsaProgram &Ssa);
bool RunOptIndexablePack(SsaProgram &Ssa);
bool RunOptCopiesAndModifiers(SsaProgram &Ssa);
bool RunOptIfConvert(SsaProgram &Ssa);
bool RunOptAlgebraic(SsaProgram &Ssa);
bool RunOptFoldOutputs(SsaProgram &Ssa);
bool RunOptGvnDce(SsaProgram &Ssa, const SsaDomTree *CachedDom = nullptr);
bool RunOptMaskedDce(SsaProgram &Ssa);
bool RunOptSelectDots(SsaProgram &Ssa);
bool RunOptSelectDp2(SsaProgram &Ssa);
bool RunOptSlpVectorize(SsaProgram &Ssa);

// Runs the fixed-point SSA MIR optimization pipeline until convergence.
void OptimizeSsaMir(SsaProgram &Ssa);

} // namespace dxcb5

#endif
