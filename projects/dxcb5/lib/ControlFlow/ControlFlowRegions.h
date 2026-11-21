// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_CONTROL_FLOW_REGIONS_H
#define DXCB5_CONTROL_FLOW_REGIONS_H

#include "ControlFlowCode.h"
#include "llvm/ADT/SmallVector.h"

namespace dxcb5::flow {

struct Region {
  uint32_t Begin;
  uint32_t End;
  bool Cyclic;
};

struct DispatchOrder {
  std::vector<Region> Regions;
  std::vector<uint32_t> Blocks;
  std::vector<uint32_t> Labels;
  std::vector<uint32_t> RegionOf;
  std::vector<size_t> Incoming;

  explicit DispatchOrder(const std::vector<FlowBlock> &Flow);
  std::vector<bool> ExternalEntries(const std::vector<FlowBlock> &Flow) const;
};

struct LoopStep {
  const FlowBlock *Block;
  const FlowEdge *Next;
  const FlowEdge *Exit;
};

struct SwitchGroup {
  size_t RepEdge = 0;
  bool HasDefault = false;
  llvm::SmallVector<uint32_t, 4> CaseValues;
};

void GroupSwitchEdges(const FlowBlock &Block, const DispatchOrder &Order,
                      uint32_t Stop, llvm::SmallVectorImpl<SwitchGroup> &Out);
bool IdentityCopies(const FlowEdge &Edge);

bool HasQuadOperations(const std::vector<FlowBlock> &Flow,
                       const DispatchOrder &Order, const Region &R);
bool IsPureArmInstruction(const Instruction &Inst);
std::vector<LoopStep> GetLinearLoop(const std::vector<FlowBlock> &Flow,
                                    const DispatchOrder &Order, const Region &R,
                                    const std::vector<bool> &Entries);
bool EmitDispatchBlock(const DispatchOrder &Order, const FlowBlock &Block,
                       const Operand &State, Code &Out);
bool EmitDispatchLoopRegion(const DispatchOrder &Order,
                            const std::vector<FlowBlock> &Flow,
                            const Region &R, const Operand &State,
                            const Operand &Predicate, Code &Out);
bool NormalizeMultiExitLoops(std::vector<FlowBlock> &Blocks,
                             uint32_t SelectorReg);
bool EmitCollapsedRegions(const DispatchOrder &Order,
                          const std::vector<FlowBlock> &Flow, Code &Out,
                          uint32_t DispatchState = UINT32_MAX,
                          uint32_t DispatchPred = UINT32_MAX);
bool EmitExpandedLoop(const DispatchOrder &Order,
                      const std::vector<FlowBlock> &Flow,
                      const Region &LoopRegion, uint32_t Entry,
                      uint32_t &ExitBlock, Code &Out);

} // namespace dxcb5::flow
#endif
