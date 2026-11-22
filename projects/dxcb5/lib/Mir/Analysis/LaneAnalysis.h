// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_ANALYSIS_LANE_ANALYSIS_H
#define DXCB5_MIR_ANALYSIS_LANE_ANALYSIS_H

#include "Mir/Analysis/LaneDomain.h"
#include "Mir/SsaAnalysis.h"
#include "Mir/SsaMir.h"
#include "llvm/ADT/ArrayRef.h"
#include <array>
#include <cstdint>
#include <vector>

namespace dxcb5 {

bool IsKnownBoolMask(const SsaProgram &Ssa,
                     const std::vector<SsaDefSite> &Defs, const Operand &Op,
                     unsigned Depth = 0);

bool IsKnownNonNegativeFloat(const SsaProgram &Ssa,
                             const std::vector<SsaDefSite> &Defs,
                             const Operand &Op, uint8_t Mask,
                             bool AllowNegZero, unsigned Depth = 0);

// Computes provable maximum read element index per IndexableTemp (or
// UINT32_MAX if never read, or Decl.Elements - 1 if an unbounded dynamic read
// exists), propagating dominated branch bounds (e.g. `ge(C, x)` -> `ftou(x)`).
void ComputeIndexableMaxReadElements(const SsaProgram &Ssa,
                                     const SsaDomTree &Dom,
                                     std::vector<uint32_t> &MaxElem);

} // namespace dxcb5

#endif
