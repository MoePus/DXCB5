// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_CONTROL_FLOW_H
#define DXCB5_CONTROL_FLOW_H

#include "dxcb5/Mir.h"
#include "dxcb5/ParallelCopy.h"

namespace dxcb5 {

enum class FlowKind { Return, Jump, Branch, Switch };

struct FlowEdge {
  uint32_t Target;
  std::vector<ParallelCopy> Copies;
  uint32_t CaseValue = 0;
};

struct FlowBlock {
  std::vector<Instruction> Instructions;
  // Branch edges are ordered true, false. Switch edges start with the default.
  std::vector<FlowEdge> Edges;
  Operand Condition{OperandKind::Null};
  FlowKind Kind = FlowKind::Return;
  bool UniformBranch = false;
};

// Block zero is the entry. Input blocks must satisfy their FlowKind edge shape,
// conditions, case uniqueness and refer only to existing blocks and temporaries.
// Edge copies address individual lanes of temp registers.
// On success this replaces Program.Instructions and updates Program.TempCount.
bool LowerControlFlow(const std::vector<FlowBlock> &Blocks, MirProgram &Program,
                      std::string &Error);

} // namespace dxcb5
#endif
