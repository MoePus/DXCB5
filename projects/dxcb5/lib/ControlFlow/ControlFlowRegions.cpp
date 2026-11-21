// Distributed under the University of Illinois Open Source License.
#include "ControlFlowRegions.h"
#include "ControlFlowIteration.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <cassert>
#include <limits>

using namespace dxcb5;
using namespace dxcb5::flow;

namespace {

constexpr uint32_t NoIndex = std::numeric_limits<uint32_t>::max();
constexpr size_t MaxRepeatedRegions = 64;
constexpr size_t MaxRepeatedBodyInstructions = 192;
constexpr unsigned MaxCollapsedFlowDepth = 64;
constexpr size_t MinSharedTailInstructions = 16;

struct RegionNode {
  uint32_t FirstSuccessor = NoIndex;
  uint32_t Join = NoIndex;
  unsigned Depth = 0;
  unsigned Emissions = 0;
  uint32_t PredCount = 0;
  uint32_t LastPredSeen = NoIndex;
  bool Terminates = false;
};

struct SharedTail {
  uint32_t ForkIndex;
  uint32_t Tail;
  unsigned ForkSide;
  unsigned TowardsSide;
};

class CollapsedFlow {
public:
  CollapsedFlow(const DispatchOrder &Order, const std::vector<FlowBlock> &Flow,
                uint32_t Scratch,
                const expanded_iteration::Slice *Slice = nullptr,
                uint32_t DispatchState = UINT32_MAX,
                uint32_t DispatchPred = UINT32_MAX, bool EarlyExitJoin = false)
      : Order(Order), Flow(Flow), Out{Scratch, {}},
        Regions(Order.Regions.size() + 1), Slice(Slice),
        Exit(static_cast<uint32_t>(Order.Regions.size())),
        DispatchState(DispatchState), DispatchPred(DispatchPred),
        EarlyExitJoin(EarlyExitJoin) { Regions[Exit].Join = Exit; }

  bool Build(Code &Result);
  bool BuildIteration(Code &Result) {
    Entries = Order.ExternalEntries(Flow);
    if (!Graph() || !Walk(0, Order.RegionOf[Slice->ContinueBlock], 0))
      return false;
    Result = std::move(Out);
    return true;
  }

private:
  bool AddGraphEdge(uint32_t Index, uint32_t Target, bool Cyclic,
                    uint32_t &NonTermJoin, bool &AnyTarget, bool &AllTerm);
  bool Graph();
  uint32_t CommonAncestor(uint32_t Left, uint32_t Right) const;
  bool Walk(uint32_t Region, uint32_t Stop, unsigned Depth);
  bool Note(uint32_t Index);
  bool Block(uint32_t Index, uint32_t &Next, bool &Terminated, unsigned Depth);
  bool Arm(const FlowEdge &Edge, uint32_t Stop, unsigned Depth) {
    return Out.Copies(Edge) && Walk(Order.RegionOf[Edge.Target], Stop, Depth + 1);
  }
  const FlowBlock *AcyclicBlock(uint32_t Index) const {
    const auto &R = Order.Regions[Index];
    return R.Cyclic ? nullptr : &Flow[Order.Blocks[R.Begin]];
  }
  bool IsProfitableTail(uint32_t Tail, uint32_t Stop) const;
  bool FindSharedTail(const FlowBlock &Head, uint32_t Stop, unsigned ForkSide,
                      SharedTail &Shape) const;
  bool EmitSharedTail(const FlowBlock &Head, const SharedTail &Shape,
                      uint32_t Stop, unsigned Depth);
  bool Branch(const FlowBlock &Block, uint32_t Stop, unsigned Depth);
  void If(const Operand &Condition, bool OnNonZero) {
    Out.Emit(D3D10_SB_OPCODE_IF, {Condition});
    Out.Instructions.back().TestNonZero = OnNonZero;
  }
  bool Switch(const FlowBlock &Block, uint32_t Stop, unsigned Depth);
  bool Loop(const Region &LoopRegion);
  bool Linear(const std::vector<LoopStep> &Steps);
  bool Iteration(const Region &LoopRegion, uint32_t Entry);
  bool ExpandedIteration(const Region &Range, uint32_t Entry);
  bool LocalDispatchLoop(const Region &LoopRegion, uint32_t Entry);
  bool Break(const FlowBlock &Block, const FlowEdge &ExitEdge);

public:
  static bool BuildExpandedBody(const DispatchOrder &Order,
                                const std::vector<FlowBlock> &Flow,
                                const Region &Range, uint32_t Entry,
                                Code &Out);

private:

  const DispatchOrder &Order;
  const std::vector<FlowBlock> &Flow;
  Code Out;
  std::vector<RegionNode> Regions;
  std::vector<bool> Entries;
  const expanded_iteration::Slice *const Slice;
  const uint32_t Exit;
  const uint32_t DispatchState;
  const uint32_t DispatchPred;
  const bool EarlyExitJoin;
  size_t RepeatedInstructions = 0;
  size_t RepeatedRegions = 0;
};

uint32_t CollapsedFlow::CommonAncestor(uint32_t Left, uint32_t Right) const {
  while (Left != Right)
    if (Regions[Left].Depth >= Regions[Right].Depth) Left = Regions[Left].Join;
    else Right = Regions[Right].Join;
  return Left;
}

bool CollapsedFlow::AddGraphEdge(uint32_t Index, uint32_t Target, bool Cyclic,
                                 uint32_t &NonTermJoin, bool &AnyTarget,
                                 bool &AllDirectExit) {
  if (Target == Index)
    return true;
  if (Target < Index || Target >= Exit)
    return false;
  AnyTarget = true;
  if (!Regions[Target].Terminates)
    NonTermJoin =
        NonTermJoin == NoIndex ? Target : CommonAncestor(NonTermJoin, Target);
  const auto &TR = Order.Regions[Target];
  if (Slice && (TR.Cyclic || (Order.Blocks[TR.Begin] != Slice->BreakBlock &&
                              Order.Blocks[TR.Begin] != Slice->ContinueBlock)))
    AllDirectExit = false;
  if (Regions[Target].LastPredSeen != Index) {
    Regions[Target].LastPredSeen = Index;
    ++Regions[Target].PredCount;
  }
  auto &Node = Regions[Index];
  if (Node.FirstSuccessor == NoIndex) {
    Node.FirstSuccessor = Target;
    Node.Join = Target;
  } else if (Target != Node.FirstSuccessor) {
    if (Cyclic)
      return false;
    Node.Join = CommonAncestor(Node.Join, Target);
  }
  return true;
}

bool CollapsedFlow::Graph() {
  for (uint32_t Index = Exit; Index != 0;) {
    --Index;
    const auto &Range = Order.Regions[Index];
    uint32_t NonTermJoin = NoIndex;
    bool AnyTarget = false, AllDirectExit = true;
    for (uint32_t I = Range.Begin; I < Range.End; ++I)
      for (const auto &Edge : Flow[Order.Blocks[I]].Edges)
        if (!AddGraphEdge(Index, Order.RegionOf[Edge.Target], Range.Cyclic,
                          NonTermJoin, AnyTarget, AllDirectExit))
          return false;
    auto &Node = Regions[Index];
    if (Node.FirstSuccessor == NoIndex) {
      if (Range.Cyclic)
        return false;
      Node.Join = Exit;
    } else if (EarlyExitJoin && !Range.Cyclic && NonTermJoin != NoIndex) {
      Node.Join = NonTermJoin;
    }
    Node.Depth = Regions[Node.Join].Depth + 1;
    if (EarlyExitJoin && !Range.Cyclic) {
      const uint32_t BID = Order.Blocks[Range.Begin];
      const auto &Insts = Flow[BID].Instructions;
      Node.Terminates = BID == Slice->BreakBlock ||
                        BID == Slice->ContinueBlock ||
                        (AnyTarget && AllDirectExit && Insts.size() <= 8 &&
                         llvm::all_of(Insts, IsPureArmInstruction));
    }
  }
  return true;
}

bool CollapsedFlow::Note(uint32_t Index) {
  const auto &Range = Order.Regions[Index];
  auto &Node = Regions[Index];
  if (Node.Emissions++ == 0)
    return true;
  if (Slice && !Range.Cyclic &&
      (Order.Blocks[Range.Begin] == Slice->BreakBlock ||
       Order.Blocks[Range.Begin] == Slice->ContinueBlock))
    return true;
  const auto &Instructions = Flow[Order.Blocks[Range.Begin]].Instructions;
  if (Range.Cyclic ||
      !std::all_of(Instructions.begin(), Instructions.end(),
                   IsPureArmInstruction))
    return false;
  if (RepeatedRegions >= MaxRepeatedRegions ||
      Instructions.size() > MaxRepeatedBodyInstructions - RepeatedInstructions)
    return false;
  RepeatedInstructions += Instructions.size();
  ++RepeatedRegions;
  return true;
}

bool CollapsedFlow::IsProfitableTail(uint32_t Tail, uint32_t Stop) const {
  llvm::SmallVector<uint32_t, 16> Pending;
  Pending.push_back(Tail);
  llvm::DenseSet<uint32_t> Seen;
  size_t BodySize = 0;
  bool HasQuadOrImpure = false;
  while (!Pending.empty()) {
    const auto Index = Pending.pop_back_val();
    if (Index == Stop || !Seen.insert(Index).second)
      continue;
    if (Index > Stop || Index == Exit || Seen.size() > MaxRepeatedRegions)
      return false;
    const auto *Body = AcyclicBlock(Index);
    if (!Body || Body->Kind == FlowKind::Return)
      return false;
    if (!llvm::all_of(Body->Instructions, IsPureArmInstruction))
      HasQuadOrImpure = true;
    BodySize += Body->Instructions.size();
    for (const auto &Edge : Body->Edges)
      Pending.push_back(Order.RegionOf[Edge.Target]);
  }
  return HasQuadOrImpure || BodySize >= MinSharedTailInstructions;
}

bool CollapsedFlow::FindSharedTail(const FlowBlock &Head, uint32_t Stop,
                                   unsigned Side, SharedTail &Shape) const {
  uint32_t Tail = Order.RegionOf[Head.Edges[1 - Side].Target];
  for (unsigned Hops = 0; Hops < 4 && Tail < Stop; ++Hops) {
    if (Regions[Tail].PredCount == 2)
      break;
    if (Regions[Tail].PredCount != 1)
      return false;
    const auto *B = AcyclicBlock(Tail);
    if (!B || B->Kind != FlowKind::Jump)
      return false;
    Tail = Order.RegionOf[B->Edges.front().Target];
  }
  if (Tail >= Stop || Regions[Tail].PredCount != 2)
    return false;

  uint32_t ForkIndex = Order.RegionOf[Head.Edges[Side].Target];
  const FlowBlock *Fork = nullptr;
  for (unsigned Hops = 0; Hops < 5 && ForkIndex < Stop && ForkIndex != Tail;
       ++Hops) {
    if (Regions[ForkIndex].PredCount != 1)
      return false;
    if (Order.Regions[ForkIndex].Cyclic) {
      ForkIndex = Regions[ForkIndex].FirstSuccessor;
      continue;
    }
    const auto *B = AcyclicBlock(ForkIndex);
    if (!B)
      return false;
    if (B->Kind == FlowKind::Branch) {
      Fork = B;
      break;
    }
    if (B->Kind != FlowKind::Jump)
      return false;
    ForkIndex = Order.RegionOf[B->Edges.front().Target];
  }
  if (!Fork || ForkIndex >= Stop || ForkIndex == Tail)
    return false;

  uint32_t Targets[2] = {NoIndex, NoIndex};
  for (unsigned I = 0; I < 2; ++I) {
    const auto Index = Order.RegionOf[Fork->Edges[I].Target];
    if (Index == Tail || Index == Stop) {
      Targets[I] = Index;
      continue;
    }
    if (Index > Stop || Regions[Index].PredCount != 1)
      return false;
    const auto *ArmBlk = AcyclicBlock(Index);
    if (!ArmBlk || ArmBlk->Kind != FlowKind::Jump)
      return false;
    Targets[I] = Order.RegionOf[ArmBlk->Edges.front().Target];
  }
  const unsigned Towards = Targets[0] == Tail ? 0 : 1;
  if (Targets[Towards] != Tail || Targets[1 - Towards] != Stop ||
      !IsProfitableTail(Tail, Stop))
    return false;
  Shape = {ForkIndex, Tail, Side, Towards};
  return true;
}

bool CollapsedFlow::EmitSharedTail(const FlowBlock &Head,
                                   const SharedTail &Shape, uint32_t Stop,
                                   unsigned Depth) {
  const auto &Entry = Head.Edges[Shape.ForkSide];
  const auto &Fork = *AcyclicBlock(Shape.ForkIndex);
  const auto *StopBlk = Stop < Regions.size() ? AcyclicBlock(Stop) : nullptr;
  const bool DirectRet =
      !Slice &&
      (Stop == Exit ||
       (StopBlk && StopBlk->Kind == FlowKind::Return && StopBlk->Instructions.empty()));
  auto NeedsTailArm = [&](const FlowEdge &E) {
    return !IdentityCopies(E) || Order.RegionOf[E.Target] != Shape.Tail;
  };
  If(Head.Condition, Shape.ForkSide == 0);
  if (!Arm(Entry, Shape.ForkIndex, Depth + 1) || !Note(Shape.ForkIndex))
    return false;
  Out.Body(Fork);
  if (DirectRet) {
    If(Fork.Condition, Shape.TowardsSide != 0);
    if (!Arm(Fork.Edges[1 - Shape.TowardsSide], Stop, Depth + 1)) return false;
    if (Stop != Exit) Out.Emit(D3D10_SB_OPCODE_RET);
    if (NeedsTailArm(Fork.Edges[Shape.TowardsSide])) {
      Out.Emit(D3D10_SB_OPCODE_ELSE);
      if (!Arm(Fork.Edges[Shape.TowardsSide], Shape.Tail, Depth + 1)) return false;
    }
    Out.Emit(D3D10_SB_OPCODE_ENDIF);
    if (NeedsTailArm(Head.Edges[1 - Shape.ForkSide])) {
      Out.Emit(D3D10_SB_OPCODE_ELSE);
      if (!Arm(Head.Edges[1 - Shape.ForkSide], Shape.Tail, Depth + 1)) return false;
    }
    Out.Emit(D3D10_SB_OPCODE_ENDIF);
    return Walk(Shape.Tail, Stop, Depth + 1);
  }
  const auto Predicate = Temp(Out.Scratch);
  Out.UsesScratch = true;
  If(Fork.Condition, Shape.TowardsSide == 0);
  if (!Arm(Fork.Edges[Shape.TowardsSide], Shape.Tail, Depth + 1)) return false;
  Out.Emit(D3D10_SB_OPCODE_MOV, {Predicate, Immediate(UINT32_MAX)});
  Out.Emit(D3D10_SB_OPCODE_ELSE);
  if (!Arm(Fork.Edges[1 - Shape.TowardsSide], Stop, Depth + 1)) return false;
  Out.Emit(D3D10_SB_OPCODE_MOV, {Predicate, Immediate(0)});
  Out.Emit(D3D10_SB_OPCODE_ENDIF);
  Out.Emit(D3D10_SB_OPCODE_ELSE);
  if (!Arm(Head.Edges[1 - Shape.ForkSide], Shape.Tail, Depth + 1)) return false;
  Out.Emit(D3D10_SB_OPCODE_MOV, {Predicate, Immediate(UINT32_MAX)});
  Out.Emit(D3D10_SB_OPCODE_ENDIF);
  If(Predicate, true);
  if (!Walk(Shape.Tail, Stop, Depth + 1)) return false;
  Out.Emit(D3D10_SB_OPCODE_ENDIF);
  return true;
}

bool CollapsedFlow::Branch(const FlowBlock &Block, uint32_t Stop, unsigned Depth) {
  SharedTail Shape{};
  if (Out.Scratch < MaxTempRegisters && Depth + 2 < MaxCollapsedFlowDepth &&
      (FindSharedTail(Block, Stop, 0, Shape) || FindSharedTail(Block, Stop, 1, Shape)))
    return EmitSharedTail(Block, Shape, Stop, Depth);
  auto Empty = [&](const FlowEdge &E) { return Order.RegionOf[E.Target] == Stop && IdentityCopies(E); };
  const bool Invert = Empty(Block.Edges[0]) && !Empty(Block.Edges[1]);
  const auto &Primary = Block.Edges[Invert ? 1 : 0], &Secondary = Block.Edges[Invert ? 0 : 1];
  if (Empty(Primary) && Empty(Secondary)) return true;
  If(Block.Condition, !Invert);
  if (!Arm(Primary, Stop, Depth)) return false;
  if (!Empty(Secondary)) {
    Out.Emit(D3D10_SB_OPCODE_ELSE);
    if (!Arm(Secondary, Stop, Depth)) return false;
  }
  Out.Emit(D3D10_SB_OPCODE_ENDIF);
  return true;
}

bool CollapsedFlow::Switch(const FlowBlock &Block, uint32_t Stop,
                           unsigned Depth) {
  auto EmptyEdge = [&](const FlowEdge &Edge) {
    return Order.RegionOf[Edge.Target] == Stop && IdentityCopies(Edge);
  };
  llvm::SmallVector<SwitchGroup, 8> Groups;
  GroupSwitchEdges(Block, Order, Stop, Groups);
  const bool EmptyDefault = EmptyEdge(Block.Edges.front());
  Out.Emit(D3D10_SB_OPCODE_SWITCH, {Block.Condition});
  for (const auto &G : Groups) {
    if (EmptyDefault && !G.HasDefault && EmptyEdge(Block.Edges[G.RepEdge]))
      continue;
    for (uint32_t Val : G.CaseValues) Out.Label(Val);
    if (G.HasDefault) Out.Emit(D3D10_SB_OPCODE_DEFAULT);
    if (!Arm(Block.Edges[G.RepEdge], Stop, Depth)) return false;
    Out.Emit(D3D10_SB_OPCODE_BREAK);
  }
  Out.Emit(D3D10_SB_OPCODE_ENDSWITCH);
  return true;
}

bool CollapsedFlow::Block(uint32_t Index, uint32_t &Next, bool &Terminated,
                          unsigned Depth) {
  const auto &Range = Order.Regions[Index];
  if (Range.Cyclic) {
    Next = Regions[Index].FirstSuccessor;
    return Loop(Range);
  }
  const auto BlockID = Order.Blocks[Range.Begin];
  if (Slice && BlockID == Slice->BreakBlock) {
    Out.Emit(D3D10_SB_OPCODE_BREAK);
    Terminated = true;
    return true;
  }
  if (Slice && BlockID == Slice->ContinueBlock) {
    Out.Emit(D3D10_SB_OPCODE_CONTINUE);
    Terminated = true;
    return true;
  }
  const auto &Head = Flow[BlockID];
  Out.Body(Head);
  switch (Head.Kind) {
  case FlowKind::Return:
    Out.Emit(D3D10_SB_OPCODE_RET);
    Next = Exit;
    return true;
  case FlowKind::Jump:
    Next = Regions[Index].FirstSuccessor;
    return Out.Copies(Head.Edges.front());
  case FlowKind::Branch:
    Next = Regions[Index].Join;
    return Branch(Head, Next, Depth);
  case FlowKind::Switch:
    Next = Regions[Index].Join;
    return Switch(Head, Next, Depth);
  }
  return false;
}

bool CollapsedFlow::Walk(uint32_t Index, uint32_t Stop, unsigned Depth) {
  if (Depth >= MaxCollapsedFlowDepth)
    return false;
  while (Index != Stop && Index != Exit) {
    uint32_t Next = Exit;
    bool Terminated = false;
    if (!Note(Index) || !Block(Index, Next, Terminated, Depth))
      return false;
    if (Terminated)
      return true;
    Index = Next;
  }
  return Index == Stop;
}

bool CollapsedFlow::Break(const FlowBlock &Block, const FlowEdge &ExitEdge) {
  const bool OnNonZero = &ExitEdge == &Block.Edges.front();
  const bool SelectExit = Slice && ExitEdge.Target == Slice->ExitSelector;
  if (!SelectExit && IdentityCopies(ExitEdge)) {
    Out.Emit(D3D10_SB_OPCODE_BREAKC, {Block.Condition});
    Out.Instructions.back().TestNonZero = OnNonZero;
    return true;
  }
  If(Block.Condition, OnNonZero);
  if (!Out.Copies(ExitEdge))
    return false;
  if (SelectExit) {
    using namespace expanded_iteration;
    auto Selector = Temp(Out.Scratch);
    Selector.Mask = ExitSelectorMask;
    const auto Value = ExitEdge.CaseValue == SelectFirstExit ? UINT32_MAX : 0;
    Out.Emit(D3D10_SB_OPCODE_MOV, {Selector, Immediate(Value)});
    Out.UsesScratch = true;
  }
  Out.Emit(D3D10_SB_OPCODE_BREAK);
  Out.Emit(D3D10_SB_OPCODE_ENDIF);
  return true;
}

bool CollapsedFlow::Linear(const std::vector<LoopStep> &Steps) {
  for (const auto &Step : Steps) {
    Out.Body(*Step.Block);
    if ((Step.Exit && !Break(*Step.Block, *Step.Exit)) || !Out.Copies(*Step.Next))
      return false;
  }
  return true;
}

bool CollapsedFlow::Iteration(const Region &LoopRegion, uint32_t Entry) {
  const auto IDs = llvm::makeArrayRef(Order.Blocks)
                       .slice(LoopRegion.Begin, LoopRegion.End - LoopRegion.Begin);
  const auto Start = Out.Instructions.size();
  const bool OriginalScratch = Out.UsesScratch;
  const auto Tail = EmitBranchedIteration(Flow, IDs, Entry, Out);
  if (Tail.Latch) {
    Out.Body(*Tail.Latch);
    if (Break(*Tail.Latch, *Tail.Exit) && Out.Copies(*Tail.Back))
      return true;
  }
  Out.Instructions.resize(Start);
  Out.UsesScratch = OriginalScratch;
  return false;
}

bool CollapsedFlow::ExpandedIteration(const Region &Range, uint32_t Entry) {
  if (Slice || Out.Scratch >= MaxTempRegisters ||
      Range.End - Range.Begin > expanded_iteration::MaxBlocks)
    return false;
  llvm::TimeTraceScope Timer("DXCB5 Structure Nested Iteration", [&] {
    return std::to_string(Range.End - Range.Begin) + " blocks";
  });
  return BuildExpandedBody(Order, Flow, Range, Entry, Out);
}

bool CollapsedFlow::BuildExpandedBody(const DispatchOrder &Order,
                                      const std::vector<FlowBlock> &Flow,
                                      const Region &Range, uint32_t Entry,
                                      Code &Out) {
  expanded_iteration::Slice Iteration;
  if (!expanded_iteration::Prepare(Flow, Order, Range, Entry, Out.Scratch, Iteration))
    return false;
  const DispatchOrder SliceOrder(Iteration.Blocks);
  Code Body{Out.Scratch, {}};
  if (!CollapsedFlow(SliceOrder, Iteration.Blocks, Out.Scratch, &Iteration).BuildIteration(Body) &&
      !CollapsedFlow(SliceOrder, Iteration.Blocks, Out.Scratch, &Iteration,
                     UINT32_MAX, UINT32_MAX, true).BuildIteration(Body))
    return false;
  Out.UsesScratch |= Body.UsesScratch;
  Out.Instructions.insert(Out.Instructions.end(), Body.Instructions.begin(), Body.Instructions.end());
  return true;
}

bool CollapsedFlow::LocalDispatchLoop(const Region &LoopRegion, uint32_t Entry) {
  uint32_t ExitBlock = NoIndex;
  if (Slice || DispatchState == UINT32_MAX || DispatchPred == UINT32_MAX ||
      HasQuadOperations(Flow, Order, LoopRegion) ||
      !expanded_iteration::SingleForwardLoopExit(Flow, Order, LoopRegion, ExitBlock))
    return false;
  const Operand State = Temp(DispatchState);
  Out.Emit(D3D10_SB_OPCODE_MOV, {State, Immediate(Order.Labels[Entry])});
  return EmitDispatchLoopRegion(Order, Flow, LoopRegion, State, Temp(DispatchPred), Out);
}

bool CollapsedFlow::Loop(const Region &LoopRegion) {
  const auto Begin = Entries.begin() + LoopRegion.Begin;
  const auto End = Entries.begin() + LoopRegion.End;
  const auto Entry = std::find(Begin, End, true);
  if (Entry == End || std::find(Entry + 1, End, true) != End) return false;
  const uint32_t EntryBlock = Order.Blocks[Entry - Entries.begin()];
  const auto Steps = GetLinearLoop(Flow, Order, LoopRegion, Entries);
  if (!Steps.empty()) {
    Out.Emit(D3D10_SB_OPCODE_LOOP);
    if (!Linear(Steps)) return false;
    Out.Emit(D3D10_SB_OPCODE_ENDLOOP);
    return true;
  }
  const size_t Start = Out.Instructions.size();
  const bool OriginalScratch = Out.UsesScratch;
  Out.Emit(D3D10_SB_OPCODE_LOOP);
  if (Iteration(LoopRegion, EntryBlock) || ExpandedIteration(LoopRegion, EntryBlock)) {
    Out.Emit(D3D10_SB_OPCODE_ENDLOOP);
    return true;
  }
  Out.Instructions.resize(Start);
  Out.UsesScratch = OriginalScratch;
  return LocalDispatchLoop(LoopRegion, EntryBlock);
}

bool CollapsedFlow::Build(Code &Result) {
  Entries = Order.ExternalEntries(Flow);
  if (!Graph() || !Walk(0, Exit, 0)) return false;
#ifndef NDEBUG
  for (uint32_t Index = 0; Index < Exit; ++Index)
    assert(Regions[Index].Emissions != 0 && "each reachable region must be emitted");
#endif
  if (Out.Instructions.back().Opcode != D3D10_SB_OPCODE_RET)
    Out.Emit(D3D10_SB_OPCODE_RET);
  Result = std::move(Out);
  return true;
}

} // namespace

bool dxcb5::flow::EmitCollapsedRegions(const DispatchOrder &Order,
                                       const std::vector<FlowBlock> &Flow,
                                       Code &Out, uint32_t DispatchState,
                                       uint32_t DispatchPred) {
  llvm::TimeTraceScope Timer("DXCB5 Structure Collapsed CFG", llvm::StringRef());
  return CollapsedFlow(Order, Flow, Out.Scratch, nullptr, DispatchState, DispatchPred).Build(Out);
}

bool dxcb5::flow::EmitExpandedLoop(const DispatchOrder &Order,
                                   const std::vector<FlowBlock> &Flow,
                                   const Region &LoopRegion, uint32_t Entry,
                                   uint32_t &ExitBlock, Code &Out) {
  ExitBlock = expanded_iteration::NoBlock;
  if (Out.Scratch >= MaxTempRegisters ||
      LoopRegion.End - LoopRegion.Begin > expanded_iteration::MaxBlocks ||
      !expanded_iteration::LoopBounds(Flow, Order, LoopRegion, ExitBlock))
    return false;
  const size_t Start = Out.Instructions.size();
  const bool OriginalScratch = Out.UsesScratch;
  Out.Emit(D3D10_SB_OPCODE_LOOP);
  if (!CollapsedFlow::BuildExpandedBody(Order, Flow, LoopRegion, Entry, Out)) {
    Out.Instructions.resize(Start);
    Out.UsesScratch = OriginalScratch;
    return false;
  }
  Out.Emit(D3D10_SB_OPCODE_ENDLOOP);
  return true;
}
