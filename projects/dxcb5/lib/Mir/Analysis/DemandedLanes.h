// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_ANALYSIS_DEMANDED_LANES_H
#define DXCB5_MIR_ANALYSIS_DEMANDED_LANES_H

#include "Mir/SsaMir.h"
#include <cstdint>
#include <vector>

namespace dxcb5 {

// Backward per-lane demand analysis backed by OpcodeSpec.lean and
// LaneDomain.lean (`demandedForSrc_sound`). Computes the 4-bit demanded lane
// mask for each SSA value id in [0, Ssa.NextValueId).
void ComputeDemandedLanes(const SsaProgram &Ssa,
                          std::vector<uint8_t> &Demanded);

} // namespace dxcb5

#endif
