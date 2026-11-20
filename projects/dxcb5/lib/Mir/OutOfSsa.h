// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_OUT_OF_SSA_H
#define DXCB5_MIR_OUT_OF_SSA_H

#include "Mir/SsaMir.h"
#include "dxcb5/ControlFlow.h"
#include <vector>

namespace dxcb5 {

// Destroys SSA form into FlowBlocks with explicit edge ParallelCopies,
// lowering PseudoCombineVec / PseudoArray ops and compacting virtual temp IDs.
void LowerOutOfSsa(SsaProgram &Ssa, std::vector<FlowBlock> &Blocks);

} // namespace dxcb5

#endif
