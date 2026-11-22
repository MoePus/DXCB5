// Distributed under the University of Illinois Open Source License.
#include "dxcb5/ControlFlow.h"
#include "ControlFlowCode.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/TimeProfiler.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <unordered_set>
#include <utility>

using namespace dxcb5;
using namespace dxcb5::flow;

namespace {

bool ValidShape(const FlowBlock &Block) {
  switch (Block.Kind) {
  case FlowKind::Return: return Block.Edges.empty();
  case FlowKind::Jump: return Block.Edges.size() == 1;
  case FlowKind::Branch:
    return Block.Edges.size() == 2 && Block.Condition.Kind != OperandKind::Null;
  case FlowKind::Switch:
    return !Block.Edges.empty() && Block.Condition.Kind != OperandKind::Null;
  }
  return false;
}

bool ValidateBlock(const FlowBlock &Block, size_t BlockCount,
                   uint32_t TempCount, std::string &Error) {
  if (!ValidShape(Block)) {
    Error = "invalid MIR block terminator";
    return false;
  }
  std::unordered_set<uint32_t> Cases;
  for (size_t I = 0; I < Block.Edges.size(); ++I) {
    const FlowEdge &Edge = Block.Edges[I];
    if (Edge.Target >= BlockCount) {
      Error = "MIR control-flow edge is outside the block list";
      return false;
    }
    if (Block.Kind == FlowKind::Switch && I != 0 &&
        !Cases.insert(Edge.CaseValue).second) {
      Error = "duplicate MIR switch case value";
      return false;
    }
    for (const ParallelCopy &Copy : Edge.Copies) {
      if (Copy.DestinationLane >= ParallelCopy::LaneCount ||
          Copy.Destination >= TempCount ||
          (Copy.ReadsTemp() && Copy.Source.Index >= TempCount)) {
        Error = "MIR edge copy uses an unallocated temporary";
        return false;
      }
    }
  }
  return true;
}

std::vector<uint32_t> TopologicalOrder(const std::vector<FlowBlock> &Blocks) {
  std::vector<uint32_t> Incoming(Blocks.size());
  for (const FlowBlock &Block : Blocks)
    for (const FlowEdge &Edge : Block.Edges)
      ++Incoming[Edge.Target];

  std::vector<uint32_t> Order;
  for (uint32_t Block = 0; Block < Blocks.size(); ++Block)
    if (Incoming[Block] == 0)
      Order.push_back(Block);
  for (size_t I = 0; I < Order.size(); ++I)
    for (const FlowEdge &Edge : Blocks[Order[I]].Edges)
      if (--Incoming[Edge.Target] == 0)
        Order.push_back(Edge.Target);
  return Order;
}

uint32_t CommonAncestor(uint32_t A, uint32_t B,
                        const std::vector<uint32_t> &Parent,
                        const std::vector<uint32_t> &Depth) {
  while (A != B) {
    if (Depth[A] >= Depth[B])
      A = Parent[A];
    else
      B = Parent[B];
  }
  return A;
}

std::vector<uint32_t> Postdominators(const std::vector<FlowBlock> &Blocks,
                                     const std::vector<uint32_t> &Order) {
  const auto Exit = static_cast<uint32_t>(Blocks.size());
  std::vector<uint32_t> Parent(Blocks.size() + 1, Exit);
  std::vector<uint32_t> Depth(Blocks.size() + 1);
  for (auto I = Order.rbegin(); I != Order.rend(); ++I) {
    const auto &Edges = Blocks[*I].Edges;
    if (!Edges.empty()) {
      uint32_t Join = Edges.front().Target;
      for (const FlowEdge &Edge : Edges)
        Join = CommonAncestor(Join, Edge.Target, Parent, Depth);
      Parent[*I] = Join;
    }
    Depth[*I] = Depth[Parent[*I]] + 1;
  }
  return Parent;
}

bool EqualCopies(const FlowEdge &A, const FlowEdge &B) {
  return A.Copies.size() == B.Copies.size() &&
         std::equal(A.Copies.begin(), A.Copies.end(), B.Copies.begin(),
                    [](const ParallelCopy &Left, const ParallelCopy &Right) {
                      return Left.Destination == Right.Destination &&
                             Left.DestinationLane == Right.DestinationLane &&
                             EqualCopySources(Left.Source, Right.Source);
                    });
}

bool IsUnconditionalDiscard(const Instruction &Inst) {
  return Inst.Opcode == D3D10_SB_OPCODE_DISCARD &&
         Inst.Operands.front().Kind == OperandKind::Immediate &&
         (Inst.Operands.front().Index != 0) == Inst.TestNonZero;
}

struct Structured {
  const std::vector<FlowBlock> &Blocks;
  const std::vector<uint32_t> &Join;
  Code &Out;
  std::vector<bool> Emitted;

  bool Arm(const FlowEdge &Edge, uint32_t Stop) {
    return Out.Copies(Edge) && Region(Edge.Target, Stop);
  }
  bool TryDiscard(const FlowBlock &Block, uint32_t Stop, unsigned Side);
  bool Branch(const FlowBlock &Block, uint32_t Stop);
  bool Switch(const FlowBlock &Block, uint32_t Stop);
  bool Region(uint32_t BlockID, uint32_t Stop);
};

bool Structured::TryDiscard(const FlowBlock &Block, uint32_t Stop,
                            unsigned Side) {
  const auto &Entry = Block.Edges[Side];
  const auto &Bypass = Block.Edges[1 - Side];
  if (Entry.Target == Stop || Bypass.Target != Stop || !Entry.Copies.empty() ||
      Emitted[Entry.Target])
    return false;
  const auto &Discard = Blocks[Entry.Target];
  if (Discard.Kind != FlowKind::Jump || Discard.Instructions.size() != 1 ||
      !IsUnconditionalDiscard(Discard.Instructions.front()) ||
      Discard.Edges.front().Target != Stop ||
      !EqualCopies(Discard.Edges.front(), Bypass))
    return false;
  Emitted[Entry.Target] = true;
  Out.Instructions.push_back(
      {D3D10_SB_OPCODE_DISCARD, {Block.Condition}, Side == 0});
  return true;
}

bool Structured::Branch(const FlowBlock &Block, uint32_t Stop) {
  if (TryDiscard(Block, Stop, 0) || TryDiscard(Block, Stop, 1))
    return Out.Copies(Block.Edges[Block.Edges[0].Target == Stop ? 0 : 1]);
  const auto Empty = [Stop](const FlowEdge &Edge) {
    return Edge.Target == Stop && Edge.Copies.empty();
  };
  const bool Invert = Empty(Block.Edges[0]) && !Empty(Block.Edges[1]);
  const auto &Primary = Block.Edges[Invert ? 1 : 0];
  const auto &Secondary = Block.Edges[Invert ? 0 : 1];
  if (Empty(Primary) && Empty(Secondary))
    return true;
  Out.Instructions.push_back({D3D10_SB_OPCODE_IF, {Block.Condition}, !Invert});
  if (!Arm(Primary, Stop))
    return false;
  if (!Empty(Secondary)) {
    Out.Emit(D3D10_SB_OPCODE_ELSE);
    if (!Arm(Secondary, Stop))
      return false;
  }
  Out.Emit(D3D10_SB_OPCODE_ENDIF);
  return true;
}

bool Structured::Switch(const FlowBlock &Block, uint32_t Stop) {
  Out.Instructions.push_back({D3D10_SB_OPCODE_SWITCH, {Block.Condition}});
  for (size_t I = 1; I < Block.Edges.size(); ++I) {
    const FlowEdge &Edge = Block.Edges[I];
    Out.Label(Edge.CaseValue);
    if (!Arm(Edge, Stop))
      return false;
    Out.Emit(D3D10_SB_OPCODE_BREAK);
  }
  Out.Emit(D3D10_SB_OPCODE_DEFAULT);
  if (!Arm(Block.Edges.front(), Stop))
    return false;
  Out.Emit(D3D10_SB_OPCODE_BREAK);
  Out.Emit(D3D10_SB_OPCODE_ENDSWITCH);
  return true;
}

bool Structured::Region(uint32_t BlockID, uint32_t Stop) {
  while (BlockID != Stop) {
    if (BlockID >= Blocks.size() || Emitted[BlockID])
      return false;
    Emitted[BlockID] = true;
    const FlowBlock &Block = Blocks[BlockID];
    Out.Body(Block);
    switch (Block.Kind) {
    case FlowKind::Return:
      Out.Emit(D3D10_SB_OPCODE_RET);
      return true;
    case FlowKind::Jump:
      if (!Out.Copies(Block.Edges.front()))
        return false;
      BlockID = Block.Edges.front().Target;
      break;
    case FlowKind::Branch:
      if (!Branch(Block, Join[BlockID]))
        return false;
      BlockID = Join[BlockID];
      break;
    case FlowKind::Switch:
      if (!Switch(Block, Join[BlockID]))
        return false;
      BlockID = Join[BlockID];
      break;
    }
  }
  return true;
}

bool EmitStructured(const std::vector<FlowBlock> &Blocks, Code &Out) {
  const auto Order = TopologicalOrder(Blocks);
  if (Order.size() != Blocks.size())
    return false;
  const auto Join = Postdominators(Blocks, Order);
  Structured Writer{Blocks, Join, Out, std::vector<bool>(Blocks.size())};
  if (!Writer.Region(0, static_cast<uint32_t>(Blocks.size())))
    return false;
  if (Out.Instructions.back().Opcode != D3D10_SB_OPCODE_RET)
    Out.Emit(D3D10_SB_OPCODE_RET);
  return true;
}

bool Publish(Code &Result, uint64_t TempCount, MirProgram &Program,
             std::string &Error) {
  OptimizeStructuredFlow(Result.Instructions);
  if (TempCount > MaxTempRegisters - static_cast<uint32_t>(Result.UsesScratch)) {
    Error = "DXBC control flow exceeds the temporary register limit";
    return false;
  }
  Program.TempCount = static_cast<uint32_t>(TempCount) + Result.UsesScratch;
  Program.Instructions = std::move(Result.Instructions);
  return true;
}

using LoopPositions = llvm::SmallDenseMap<uint32_t, uint32_t, 8>;

bool FindIterationExit(const std::vector<FlowBlock> &Flow,
                       llvm::ArrayRef<uint32_t> Blocks,
                       const LoopPositions &Positions, uint32_t Entry,
                       IterationExit &Tail) {
  for (auto ID : Blocks) {
    const auto &Block = Flow[ID];
    if (Block.Kind != FlowKind::Jump && Block.Kind != FlowKind::Branch)
      return false;
    for (const auto &Edge : Block.Edges) {
      const bool Back = Edge.Target == Entry;
      if (!Back && Positions.count(Edge.Target))
        continue;
      if (Block.Kind != FlowKind::Branch || (Tail.Latch && Tail.Latch != &Block))
        return false;
      auto *&Slot = Back ? Tail.Back : Tail.Exit;
      if (Slot)
        return false;
      Slot = &Edge;
      Tail.Latch = &Block;
    }
  }
  return Tail.Back && Tail.Exit;
}

} // namespace

IterationExit dxcb5::flow::EmitBranchedIteration(
    const std::vector<FlowBlock> &Flow, llvm::ArrayRef<uint32_t> Blocks,
    uint32_t Entry, Code &Out) {
  constexpr size_t MaxIterationBlocks = 64;
  if (Blocks.size() < 2 || Blocks.size() > MaxIterationBlocks)
    return {};
  LoopPositions Positions;
  for (uint32_t I = 0; I < Blocks.size(); ++I)
    Positions[Blocks[I]] = I;
  IterationExit Tail;
  if (!FindIterationExit(Flow, Blocks, Positions, Entry, Tail))
    return {};

  const auto LatchID = static_cast<uint32_t>(Tail.Latch - Flow.data());
  const auto Stop = static_cast<uint32_t>(Blocks.size() - 1);
  if (LatchID == Entry)
    return {};
  Positions[Entry] = 0;
  Positions[LatchID] = Stop;
  uint32_t Next = 1;
  for (auto ID : Blocks)
    if (ID != Entry && ID != LatchID)
      Positions[ID] = Next++;

  std::vector<FlowBlock> Iteration(Blocks.size());
  for (auto ID : Blocks) {
    if (ID == LatchID)
      continue;
    auto &Block = Iteration[Positions.lookup(ID)];
    Block = Flow[ID];
    for (auto &Edge : Block.Edges)
      Edge.Target = Positions.lookup(Edge.Target);
  }
  const auto Order = TopologicalOrder(Iteration);
  if (Order.size() != Iteration.size())
    return {};
  const auto Join = Postdominators(Iteration, Order);
  Structured Writer{Iteration, Join, Out, std::vector<bool>(Iteration.size())};
  return Writer.Region(0, Stop) ? Tail : IterationExit{};
}

bool dxcb5::LowerControlFlow(const std::vector<FlowBlock> &Blocks,
                             MirProgram &Program, std::string &Error) {
  llvm::TimeTraceScope Timer("DXCB5 Structure Control Flow", llvm::StringRef());
  if (Blocks.empty() || Blocks.size() > std::numeric_limits<uint32_t>::max()) {
    Error = "invalid MIR block count";
    return false;
  }
  for (const FlowBlock &Block : Blocks)
    if (!ValidateBlock(Block, Blocks.size(), Program.TempCount, Error))
      return false;
  Code Native{Program.TempCount, {}};
  if (EmitStructured(Blocks, Native))
    return Publish(Native, Program.TempCount, Program, Error);
  constexpr uint32_t DispatchTemps = 2;
  const uint64_t FallbackCount =
      static_cast<uint64_t>(Program.TempCount) + DispatchTemps;
  if (FallbackCount > UINT32_MAX)
    return Publish(Native, FallbackCount, Program, Error);
  Code Fallback{static_cast<uint32_t>(FallbackCount), {}};
  bool UsesDispatchTemps;
  if (!EmitDispatch(Blocks, Program.TempCount, Program.TempCount + 1, Fallback,
                    UsesDispatchTemps, Error))
    return false;
  return Publish(Fallback, UsesDispatchTemps ? FallbackCount : Program.TempCount,
                 Program, Error);
}
