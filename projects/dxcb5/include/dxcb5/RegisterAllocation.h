// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_REGISTER_ALLOCATION_H
#define DXCB5_REGISTER_ALLOCATION_H

#include "dxcb5/ControlFlow.h"

namespace dxcb5 {

// Conservatively reuse whole temp registers across CFG live ranges.
// On success rewrites the block operands and reduces Program.TempCount.
bool AllocateRegisters(std::vector<FlowBlock> &Blocks, MirProgram &Program,
                       std::string &Error);

} // namespace dxcb5
#endif
