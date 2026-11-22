// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_CONTROL_FLOW_ITERATION_H
#define DXCB5_CONTROL_FLOW_ITERATION_H

#include "ControlFlowRegions.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <limits>

namespace dxcb5::flow::expanded_iteration {

constexpr uint32_t NoBlock = std::numeric_limits<uint32_t>::max();
// Bound the copied SCC and all its edge bundles. The largest representative
// outer loop has 95 blocks, 858 body instructions and 349 incoming edge copies.
constexpr unsigned MaxBlocks = 256;
constexpr size_t MaxBodyInstructions = 4096;
constexpr size_t MaxEdgeCopies = 2048;
// Shared-tail predicates use scratch.x. ParallelCopy may use all four lanes of
// scratch during Out.Copies() to coalesce cross-register vector swaps.
// The inner-loop exit selector lives in scratch.y strictly after Out.Copies()
// on the exiting branch until the synthetic merge branch reads it (no edge
// copies run in between).
constexpr uint8_t ExitSelectorMask = 1u << 1;
constexpr uint8_t ExitSelectorSwizzle = 0x55;
constexpr uint32_t SelectFirstExit = 2;
constexpr uint32_t SelectSecondExit = 1;

struct Slice {
  std::vector<FlowBlock> Blocks;
  uint32_t BreakBlock = NoBlock;
  uint32_t ContinueBlock = NoBlock;
  uint32_t ExitSelector = NoBlock;
};

inline bool LoopBounds(const std::vector<FlowBlock> &Flow,
                       const DispatchOrder &Order, const Region &Range,
                       uint32_t &Exit) {
  size_t Body = 0, Copies = 0;
  for (uint32_t I = Range.Begin; I < Range.End; ++I) {
    const auto &Block = Flow[Order.Blocks[I]];
    if (Block.Kind != FlowKind::Jump && Block.Kind != FlowKind::Branch)
      return false;
    Body += Block.Instructions.size();
    if (Body > MaxBodyInstructions)
      return false;
    for (const auto &Edge : Block.Edges) {
      Copies += Edge.Copies.size();
      if (Copies > MaxEdgeCopies)
        return false;
      const auto Label = Order.Labels[Edge.Target];
      if (Label >= Range.Begin && Label < Range.End)
        continue;
      if (Exit != NoBlock && Exit != Edge.Target)
        return false;
      Exit = Edge.Target;
    }
  }
  return Exit != NoBlock;
}

inline bool NormalizeInnerLoop(Slice &Graph, const DispatchOrder &Order,
                               const Region &Range,
                               const std::vector<bool> &Entries,
                               uint32_t Scratch) {
  unsigned EntryCount = 0;
  for (uint32_t I = Range.Begin; I < Range.End; ++I)
    EntryCount += Entries[I] ? 1u : 0u;
  if (EntryCount != 1)
    return false;
  llvm::SmallVector<uint32_t, 2> Exits;
  for (uint32_t I = Range.Begin; I < Range.End; ++I) {
    for (const auto &Edge : Graph.Blocks[Order.Blocks[I]].Edges) {
      const auto Label = Order.Labels[Edge.Target];
      if ((Label < Range.Begin || Label >= Range.End) &&
          std::find(Exits.begin(), Exits.end(), Edge.Target) == Exits.end())
        Exits.push_back(Edge.Target);
    }
  }
  if (Exits.size() <= 1)
    return true;
  if (GetLinearLoop(Graph.Blocks, Order, Range, Entries).empty())
    return false;
  if (Exits.size() != 2 || Graph.ExitSelector != NoBlock)
    return false;
  const uint32_t Merge = static_cast<uint32_t>(Graph.Blocks.size());
  for (uint32_t I = Range.Begin; I < Range.End; ++I) {
    for (auto &Edge : Graph.Blocks[Order.Blocks[I]].Edges) {
      if (Edge.Target == Exits[0] || Edge.Target == Exits[1]) {
        Edge.CaseValue =
            Edge.Target == Exits[0] ? SelectFirstExit : SelectSecondExit;
        Edge.Target = Merge;
      }
    }
  }
  FlowBlock MergeBlock;
  MergeBlock.Kind = FlowKind::Branch;
  MergeBlock.Condition = Temp(Scratch);
  MergeBlock.Condition.Swizzle = ExitSelectorSwizzle;
  MergeBlock.Edges.push_back({Exits[0], {}});
  MergeBlock.Edges.push_back({Exits[1], {}});
  Graph.Blocks.push_back(std::move(MergeBlock));
  Graph.ExitSelector = Merge;
  return true;
}

inline bool Prepare(const std::vector<FlowBlock> &Flow,
                    const DispatchOrder &Order, const Region &Range,
                    uint32_t Entry, uint32_t Scratch, Slice &Iteration) {
  uint32_t Exit = NoBlock;
  if (!LoopBounds(Flow, Order, Range, Exit))
    return false;
  auto &Blocks = Iteration.Blocks;
  const auto N = Range.End - Range.Begin;
  Iteration.BreakBlock = N;
  Iteration.ContinueBlock = N + 1;
  Blocks.resize(N + 2);
  const auto Position = [&](uint32_t ID) {
    const auto Label = Order.Labels[ID];
    if (ID == Entry)
      return 0u;
    return Label - Range.Begin + (Label < Order.Labels[Entry]);
  };
  for (uint32_t I = Range.Begin; I < Range.End; ++I) {
    const auto ID = Order.Blocks[I];
    auto &Block = Blocks[Position(ID)];
    Block = Flow[ID];
    for (auto &Edge : Block.Edges) {
      if (Edge.Target == Entry)
        Edge.Target = N + 1;
      else if (Edge.Target == Exit)
        Edge.Target = N;
      else
        Edge.Target = Position(Edge.Target);
    }
  }
  Blocks[N].Kind = FlowKind::Jump;
  Blocks[N].Edges.push_back({N + 1, {}});
  Blocks[N + 1].Kind = FlowKind::Return;
  const DispatchOrder SliceOrder(Blocks);
  const auto Entries = SliceOrder.ExternalEntries(Blocks);
  for (const Region &R : SliceOrder.Regions)
    if (R.Cyclic &&
        !NormalizeInnerLoop(Iteration, SliceOrder, R, Entries, Scratch))
      return false;
  return true;
}

inline bool SingleForwardLoopExit(const std::vector<FlowBlock> &Flow,
                                  const DispatchOrder &Order,
                                  const Region &Range, uint32_t &ExitBlock) {
  ExitBlock = NoBlock;
  for (uint32_t I = Range.Begin; I < Range.End; ++I) {
    for (const auto &Edge : Flow[Order.Blocks[I]].Edges) {
      const uint32_t Label = Order.Labels[Edge.Target];
      if (Label >= Range.Begin && Label < Range.End)
        continue;
      if (Label < Range.End || (ExitBlock != NoBlock && ExitBlock != Edge.Target))
        return false;
      ExitBlock = Edge.Target;
    }
  }
  return ExitBlock != NoBlock;
}

} // namespace dxcb5::flow::expanded_iteration
#endif
