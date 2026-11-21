// Distributed under the University of Illinois Open Source License.
#include "ControlFlowRegions.h"
#include "llvm/Support/TimeProfiler.h"

#include <algorithm>
#include <iterator>
#include <limits>

using namespace dxcb5;
using namespace dxcb5::flow;

namespace {

constexpr uint32_t NoIndex = std::numeric_limits<uint32_t>::max();
constexpr unsigned MaxStructuredIfDepth = 64;

bool EmitTransition(const DispatchOrder &Order, const FlowEdge &Edge,
                    const Operand &State, Code &Out) {
  if (!Out.Copies(Edge))
    return false;
  Out.Emit(D3D10_SB_OPCODE_MOV, {State, Immediate(Order.Labels[Edge.Target])});
  return true;
}

bool EmitDispatchBranch(const DispatchOrder &Order, const FlowBlock &Block,
                        const Operand &State, Code &Out) {
  const auto &Taken = Block.Edges[0];
  const auto &Skipped = Block.Edges[1];
  if (Taken.Copies.empty() && Skipped.Copies.empty()) {
    Out.Emit(D3D10_SB_OPCODE_MOVC,
             {State, Block.Condition, Immediate(Order.Labels[Taken.Target]),
              Immediate(Order.Labels[Skipped.Target])});
    return true;
  }
  Out.Emit(D3D10_SB_OPCODE_IF, {Block.Condition});
  if (!EmitTransition(Order, Taken, State, Out))
    return false;
  Out.Emit(D3D10_SB_OPCODE_ELSE);
  if (!EmitTransition(Order, Skipped, State, Out))
    return false;
  Out.Emit(D3D10_SB_OPCODE_ENDIF);
  return true;
}

bool EmitDispatchSwitch(const DispatchOrder &Order, const FlowBlock &Block,
                        const Operand &State, Code &Out) {
  Out.Emit(D3D10_SB_OPCODE_SWITCH, {Block.Condition});
  for (size_t I = 1; I < Block.Edges.size(); ++I) {
    Out.Label(Block.Edges[I].CaseValue);
    if (!EmitTransition(Order, Block.Edges[I], State, Out))
      return false;
    Out.Emit(D3D10_SB_OPCODE_BREAK);
  }
  Out.Emit(D3D10_SB_OPCODE_DEFAULT);
  if (!EmitTransition(Order, Block.Edges.front(), State, Out))
    return false;
  Out.Emit(D3D10_SB_OPCODE_BREAK);
  Out.Emit(D3D10_SB_OPCODE_ENDSWITCH);
  return true;
}

struct AcyclicRegion {
  uint32_t End = 0;
  uint32_t ExitBlock = NoIndex;
  size_t ExitEdges = 0;
  uint32_t Next = NoIndex;
  uint32_t TrueArm = NoIndex;
  uint32_t FalseArm = NoIndex;
  unsigned Depth = 0;
  bool HasArms() const { return TrueArm != NoIndex || FalseArm != NoIndex; }
};

struct Arm {
  uint32_t Region = NoIndex;
  uint32_t ExitBlock;
  size_t ExitEdges = 1;
  unsigned Depth = 0;
};

class AcyclicRegions {
public:
  AcyclicRegions(const DispatchOrder &Order, const std::vector<FlowBlock> &Flow)
      : Order(Order), Flow(Flow), Regions(Order.Regions.size()),
        Purity(Order.Regions.size()) {
    for (uint32_t I = static_cast<uint32_t>(Regions.size()); I != 0;)
      Build(--I);
  }

  const AcyclicRegion &operator[](uint32_t I) const { return Regions[I]; }

  bool OwnsNext(const AcyclicRegion &Plan) const {
    return Plan.ExitBlock != NoIndex &&
           Order.RegionOf[Plan.ExitBlock] == Plan.End &&
           !Order.Regions[Plan.End].Cyclic &&
           Plan.ExitEdges == Order.Incoming[Plan.ExitBlock];
  }

private:
  enum class BodyPurity { Unknown, Pure, Impure };
  const FlowBlock &Block(uint32_t I) const {
    return Flow[Order.Blocks[Order.Regions[I].Begin]];
  }
  bool HasPureHead(uint32_t I);
  Arm FindArm(const FlowEdge &Edge);
  bool AppendArm(uint32_t &End, const Arm &Part) const;
  void FoldBranch(uint32_t I, const FlowBlock &Head, AcyclicRegion &Plan);
  void FoldSuccessor(AcyclicRegion &Plan);
  void Build(uint32_t I);

  const DispatchOrder &Order;
  const std::vector<FlowBlock> &Flow;
  std::vector<AcyclicRegion> Regions;
  std::vector<BodyPurity> Purity;
};

bool AcyclicRegions::HasPureHead(uint32_t I) {
  if (Order.Regions[I].Cyclic)
    return false;
  auto &Value = Purity[I];
  if (Value == BodyPurity::Unknown) {
    const auto &Insts = Block(I).Instructions;
    Value = std::all_of(Insts.begin(), Insts.end(), IsPureArmInstruction)
                ? BodyPurity::Pure
                : BodyPurity::Impure;
  }
  return Value == BodyPurity::Pure;
}

Arm AcyclicRegions::FindArm(const FlowEdge &Edge) {
  const uint32_t I = Order.RegionOf[Edge.Target];
  const auto &Plan = Regions[I];
  if (Order.Incoming[Edge.Target] != 1 || Plan.ExitBlock == NoIndex ||
      !HasPureHead(I))
    return {NoIndex, Edge.Target};
  return {I, Plan.ExitBlock, Plan.ExitEdges, Plan.Depth};
}

bool AcyclicRegions::AppendArm(uint32_t &End, const Arm &Part) const {
  if (Part.Region == NoIndex)
    return true;
  if (Part.Region != End)
    return false;
  End = Regions[Part.Region].End;
  return true;
}

void AcyclicRegions::FoldBranch(uint32_t I, const FlowBlock &Head,
                                AcyclicRegion &Plan) {
  const Arm True = FindArm(Head.Edges[0]);
  const Arm False = FindArm(Head.Edges[1]);
  const unsigned Depth = 1 + std::max(True.Depth, False.Depth);
  if (True.ExitBlock != False.ExitBlock || Depth > MaxStructuredIfDepth)
    return;
  uint32_t End = I + 1;
  const bool TrueFirst = True.Region < False.Region;
  if (!AppendArm(End, TrueFirst ? True : False) ||
      !AppendArm(End, TrueFirst ? False : True) || End == I + 1 ||
      Order.RegionOf[True.ExitBlock] < End)
    return;
  Plan.End = End;
  Plan.ExitBlock = True.ExitBlock;
  Plan.ExitEdges = True.ExitEdges + False.ExitEdges;
  Plan.TrueArm = True.Region;
  Plan.FalseArm = False.Region;
  Plan.Depth = Depth;
}

void AcyclicRegions::FoldSuccessor(AcyclicRegion &Plan) {
  if (!OwnsNext(Plan) || !HasPureHead(Plan.End))
    return;
  const uint32_t Next = Plan.End;
  const auto &Tail = Regions[Next];
  Plan.Next = Next;
  Plan.End = Tail.End;
  Plan.ExitBlock = Tail.ExitBlock;
  Plan.ExitEdges = Tail.ExitEdges;
  Plan.Depth = std::max(Plan.Depth, Tail.Depth);
}

void AcyclicRegions::Build(uint32_t I) {
  auto &Plan = Regions[I];
  Plan.End = I + 1;
  if (Order.Regions[I].Cyclic)
    return;
  const auto &Head = Block(I);
  if (Head.Kind == FlowKind::Jump) {
    Plan.ExitBlock = Head.Edges.front().Target;
    Plan.ExitEdges = 1;
  } else if (Head.Kind == FlowKind::Branch) {
    FoldBranch(I, Head, Plan);
  }
  FoldSuccessor(Plan);
}

class Dispatch {
public:
  Dispatch(const DispatchOrder &Order, uint32_t State, uint32_t Predicate,
           Code &Out)
      : Order(Order), State(Temp(State)), Predicate(Temp(Predicate)), Out(Out) {}

  bool Emit(const std::vector<FlowBlock> &Flow, std::string &Error);

private:
  void WriteState(uint32_t Target) {
    Out.Emit(D3D10_SB_OPCODE_MOV, {State, Immediate(Order.Labels[Target])});
  }
  bool EmitArm(const FlowEdge &Edge, uint32_t Arm,
               const std::vector<FlowBlock> &Flow, const AcyclicRegions &Plans);
  bool EmitIfRegion(const FlowBlock &Head, const AcyclicRegion &Plan,
                    const std::vector<FlowBlock> &Flow,
                    const AcyclicRegions &Plans);
  bool EmitAcyclic(const std::vector<FlowBlock> &Flow,
                   const AcyclicRegions &Plans, uint32_t I);
  bool NativeLoop(const std::vector<LoopStep> &Steps);
  bool TryBranchedLoop(const std::vector<FlowBlock> &Flow, const Region &R,
                       const std::vector<bool> &Entries);
  bool TryExpandedLoop(const std::vector<FlowBlock> &Flow, const Region &R,
                       const std::vector<bool> &Entries);

  const DispatchOrder &Order;
  Operand State;
  Operand Predicate;
  Code &Out;
};

bool Dispatch::EmitArm(const FlowEdge &Edge, uint32_t Arm,
                       const std::vector<FlowBlock> &Flow,
                       const AcyclicRegions &Plans) {
  return Out.Copies(Edge) && (Arm == NoIndex || EmitAcyclic(Flow, Plans, Arm));
}

bool Dispatch::EmitIfRegion(const FlowBlock &Head, const AcyclicRegion &Plan,
                            const std::vector<FlowBlock> &Flow,
                            const AcyclicRegions &Plans) {
  Out.Body(Head);
  Out.Emit(D3D10_SB_OPCODE_IF, {Head.Condition});
  if (!EmitArm(Head.Edges[0], Plan.TrueArm, Flow, Plans))
    return false;
  Out.Emit(D3D10_SB_OPCODE_ELSE);
  if (!EmitArm(Head.Edges[1], Plan.FalseArm, Flow, Plans))
    return false;
  Out.Emit(D3D10_SB_OPCODE_ENDIF);
  return true;
}

bool Dispatch::EmitAcyclic(const std::vector<FlowBlock> &Flow,
                           const AcyclicRegions &Plans, uint32_t I) {
  for (; I != NoIndex; I = Plans[I].Next) {
    const auto &Plan = Plans[I];
    const FlowBlock &Head = Flow[Order.Blocks[Order.Regions[I].Begin]];
    if (Plan.HasArms()) {
      if (!EmitIfRegion(Head, Plan, Flow, Plans))
        return false;
    } else if (Head.Kind == FlowKind::Jump) {
      Out.Body(Head);
      if (!Out.Copies(Head.Edges.front()))
        return false;
    } else {
      return EmitDispatchBlock(Order, Head, State, Out);
    }
  }
  return true;
}

bool Dispatch::NativeLoop(const std::vector<LoopStep> &Steps) {
  Out.Emit(D3D10_SB_OPCODE_LOOP);
  for (const LoopStep &Step : Steps) {
    Out.Body(*Step.Block);
    if (Step.Exit) {
      Out.Emit(D3D10_SB_OPCODE_IF, {Step.Block->Condition});
      Out.Instructions.back().TestNonZero =
          Step.Exit == &Step.Block->Edges.front();
      if (!EmitTransition(Order, *Step.Exit, State, Out))
        return false;
      Out.Emit(D3D10_SB_OPCODE_BREAK);
      Out.Emit(D3D10_SB_OPCODE_ENDIF);
    }
    if (!Out.Copies(*Step.Next))
      return false;
  }
  Out.Emit(D3D10_SB_OPCODE_ENDLOOP);
  return true;
}

bool Dispatch::TryBranchedLoop(const std::vector<FlowBlock> &Flow,
                               const Region &R,
                               const std::vector<bool> &Entries) {
  const auto End = Entries.begin() + R.End;
  const auto Entry = std::find(Entries.begin() + R.Begin, End, true);
  if (Entry == End || std::find(Entry + 1, End, true) != End)
    return false;
  Code Local{Out.Scratch, {}};
  Local.Emit(D3D10_SB_OPCODE_LOOP);
  const auto BlockIDs =
      llvm::makeArrayRef(Order.Blocks).slice(R.Begin, R.End - R.Begin);
  const auto Tail = EmitBranchedIteration(
      Flow, BlockIDs, Order.Blocks[Entry - Entries.begin()], Local);
  if (!Tail.Latch)
    return false;
  Local.Body(*Tail.Latch);
  Local.Emit(D3D10_SB_OPCODE_IF, {Tail.Latch->Condition});
  Local.Instructions.back().TestNonZero =
      Tail.Exit == &Tail.Latch->Edges.front();
  if (!Local.Copies(*Tail.Exit))
    return false;
  Local.Emit(D3D10_SB_OPCODE_MOV,
             {State, Immediate(Order.Labels[Tail.Exit->Target])});
  Local.Emit(D3D10_SB_OPCODE_BREAK);
  Local.Emit(D3D10_SB_OPCODE_ENDIF);
  if (!Local.Copies(*Tail.Back))
    return false;
  Local.Emit(D3D10_SB_OPCODE_ENDLOOP);
  Out.Instructions.insert(Out.Instructions.end(),
                          std::make_move_iterator(Local.Instructions.begin()),
                          std::make_move_iterator(Local.Instructions.end()));
  Out.UsesScratch |= Local.UsesScratch;
  return true;
}

bool Dispatch::TryExpandedLoop(const std::vector<FlowBlock> &Flow,
                               const Region &R,
                               const std::vector<bool> &Entries) {
  const auto End = Entries.begin() + R.End;
  const auto Entry = std::find(Entries.begin() + R.Begin, End, true);
  if (Entry == End || std::find(Entry + 1, End, true) != End)
    return false;
  Code Local{Out.Scratch, {}};
  uint32_t ExitBlock = 0;
  if (!EmitExpandedLoop(Order, Flow, R, Order.Blocks[Entry - Entries.begin()],
                        ExitBlock, Local))
    return false;
  Out.Instructions.insert(Out.Instructions.end(),
                          std::make_move_iterator(Local.Instructions.begin()),
                          std::make_move_iterator(Local.Instructions.end()));
  Out.UsesScratch |= Local.UsesScratch;
  WriteState(ExitBlock);
  return true;
}

bool Dispatch::Emit(const std::vector<FlowBlock> &Flow, std::string &Error) {
  if (Order.Regions.front().Cyclic)
    WriteState(0);
  std::vector<bool> Entries;
  const AcyclicRegions Plans(Order, Flow);
  bool OpenGuard = false;
  bool StartsGroup = true;
  for (uint32_t I = 0; I < Order.Regions.size();) {
    const Region &R = Order.Regions[I];
    const auto &Plan = Plans[I];
    std::vector<LoopStep> Native;
    if (R.Cyclic) {
      if (Entries.empty())
        Entries = Order.ExternalEntries(Flow);
      Native = GetLinearLoop(Flow, Order, R, Entries);
    }
    if (StartsGroup && I != 0) {
      const Region &Last = Order.Regions[Plan.End - 1];
      Out.Emit(D3D10_SB_OPCODE_ULT, {Predicate, State, Immediate(Last.End)});
      Out.Emit(D3D10_SB_OPCODE_IF, {Predicate});
      OpenGuard = true;
    }
    bool Success = false;
    if (!Native.empty()) {
      Success = NativeLoop(Native);
    } else if (R.Cyclic) {
      if (TryBranchedLoop(Flow, R, Entries) ||
          TryExpandedLoop(Flow, R, Entries)) {
        Success = true;
      } else if (HasQuadOperations(Flow, Order, R)) {
        Error =
            "MIR loop requires structured control flow for pixel quad operations";
        return false;
      } else {
        Success = EmitDispatchLoopRegion(Order, Flow, R, State, Predicate, Out);
      }
    } else {
      Success = EmitAcyclic(Flow, Plans, I);
    }
    if (!Success) {
      Error = "conflicting destination in MIR parallel edge copies";
      return false;
    }
    StartsGroup = !Plans.OwnsNext(Plan);
    if (StartsGroup && Plan.ExitBlock != NoIndex)
      WriteState(Plan.ExitBlock);
    if (StartsGroup && OpenGuard) {
      Out.Emit(D3D10_SB_OPCODE_ENDIF);
      OpenGuard = false;
    }
    I = Plan.End;
  }
  Out.Emit(D3D10_SB_OPCODE_RET);
  return true;
}

} // namespace

bool dxcb5::flow::EmitDispatchBlock(const DispatchOrder &Order,
                                    const FlowBlock &Block,
                                    const Operand &State, Code &Out) {
  Out.Body(Block);
  switch (Block.Kind) {
  case FlowKind::Return:
    Out.Emit(D3D10_SB_OPCODE_RET);
    return true;
  case FlowKind::Jump:
    return EmitTransition(Order, Block.Edges.front(), State, Out);
  case FlowKind::Branch:
    return EmitDispatchBranch(Order, Block, State, Out);
  case FlowKind::Switch:
    return EmitDispatchSwitch(Order, Block, State, Out);
  }
  return false;
}

bool dxcb5::flow::EmitDispatchLoopRegion(const DispatchOrder &Order,
                                         const std::vector<FlowBlock> &Flow,
                                         const Region &R, const Operand &State,
                                         const Operand &Predicate, Code &Out) {
  Out.Emit(D3D10_SB_OPCODE_LOOP);
  Out.Emit(D3D10_SB_OPCODE_SWITCH, {State});
  for (uint32_t I = R.Begin; I < R.End; ++I) {
    Out.Label(I);
    if (!EmitDispatchBlock(Order, Flow[Order.Blocks[I]], State, Out))
      return false;
    Out.Emit(D3D10_SB_OPCODE_BREAK);
  }
  Out.Emit(D3D10_SB_OPCODE_ENDSWITCH);
  Out.Emit(D3D10_SB_OPCODE_ULT, {Predicate, State, Immediate(R.End)});
  Out.Emit(D3D10_SB_OPCODE_BREAKC, {Predicate});
  Out.Instructions.back().TestNonZero = false;
  Out.Emit(D3D10_SB_OPCODE_ENDLOOP);
  return true;
}

bool dxcb5::flow::EmitDispatch(const std::vector<FlowBlock> &Blocks,
                               uint32_t State, uint32_t Predicate, Code &Out,
                               bool &UsesDispatchTemps, std::string &Error) {
  llvm::TimeTraceScope Timer("DXCB5 SCC Dispatcher", llvm::StringRef());
  const DispatchOrder Order(Blocks);
  Code Native{State, {}};
  if (EmitCollapsedRegions(Order, Blocks, Native)) {
    Out = std::move(Native);
    UsesDispatchTemps = false;
    return true;
  }
  std::vector<FlowBlock> Normalized = Blocks;
  const bool HasNormalized = NormalizeMultiExitLoops(Normalized, State);
  if (HasNormalized) {
    const DispatchOrder NormOrder(Normalized);
    Code NormNative{Out.Scratch, {}};
    if (EmitCollapsedRegions(NormOrder, Normalized, NormNative)) {
      Out = std::move(NormNative);
      UsesDispatchTemps = true;
      return true;
    }
  }
  Code Hybrid{Out.Scratch, {}};
  if (EmitCollapsedRegions(Order, Blocks, Hybrid, State, Predicate)) {
    Out = std::move(Hybrid);
    UsesDispatchTemps = true;
    return true;
  }
  if (HasNormalized) {
    const DispatchOrder NormOrder(Normalized);
    Code NormHybrid{Out.Scratch, {}};
    if (EmitCollapsedRegions(NormOrder, Normalized, NormHybrid, State,
                             Predicate)) {
      Out = std::move(NormHybrid);
      UsesDispatchTemps = true;
      return true;
    }
  }
  UsesDispatchTemps = true;
  return Dispatch(Order, State, Predicate, Out).Emit(Blocks, Error);
}
