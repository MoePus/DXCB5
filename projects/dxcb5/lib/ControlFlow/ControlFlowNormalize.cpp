// Distributed under the University of Illinois Open Source License.
#include "ControlFlowRegions.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <array>
#include <limits>

using namespace dxcb5;
using namespace dxcb5::flow;

namespace {

constexpr uint32_t NoIndex = std::numeric_limits<uint32_t>::max();

struct Node {
  uint32_t ID;
  std::vector<Node *> Successors;
};

struct NodeTraits {
  using NodeType = Node;
  using ChildIteratorType = std::vector<Node *>::iterator;
  static Node *getEntryNode(Node *Entry) { return Entry; }
  static ChildIteratorType child_begin(Node *N) { return N->Successors.begin(); }
  static ChildIteratorType child_end(Node *N) { return N->Successors.end(); }
};

LoopStep FindLoopEdges(const FlowBlock &Block, const DispatchOrder &Order,
                       const Region &R) {
  LoopStep Step{&Block, nullptr, nullptr};
  if (Block.Kind != FlowKind::Jump && Block.Kind != FlowKind::Branch)
    return Step;
  for (const FlowEdge &Edge : Block.Edges) {
    const uint32_t Target = Order.Labels[Edge.Target];
    const bool Inside = Target >= R.Begin && Target < R.End;
    const FlowEdge *&Slot = Inside ? Step.Next : Step.Exit;
    if (Slot)
      return {&Block, nullptr, nullptr};
    Slot = &Edge;
  }
  return Step;
}

std::array<bool, D3D10_SB_NUM_OPCODES> BuildPureArmTable() {
  std::array<bool, D3D10_SB_NUM_OPCODES> Table{};
  constexpr D3D10_SB_OPCODE_TYPE Allowed[] = {
      D3D10_SB_OPCODE_ADD, D3D10_SB_OPCODE_AND, D3D10_SB_OPCODE_DIV,
      D3D10_SB_OPCODE_DP2, D3D10_SB_OPCODE_DP3, D3D10_SB_OPCODE_DP4,
      D3D10_SB_OPCODE_EQ, D3D10_SB_OPCODE_EXP, D3D10_SB_OPCODE_FRC,
      D3D10_SB_OPCODE_FTOI, D3D10_SB_OPCODE_FTOU, D3D10_SB_OPCODE_GE,
      D3D10_SB_OPCODE_IADD, D3D10_SB_OPCODE_IEQ, D3D10_SB_OPCODE_IGE,
      D3D10_SB_OPCODE_ILT, D3D10_SB_OPCODE_IMAD, D3D10_SB_OPCODE_IMAX,
      D3D10_SB_OPCODE_IMIN, D3D10_SB_OPCODE_IMUL, D3D10_SB_OPCODE_INE,
      D3D10_SB_OPCODE_INEG, D3D10_SB_OPCODE_ISHL, D3D10_SB_OPCODE_ISHR,
      D3D10_SB_OPCODE_ITOF, D3D10_SB_OPCODE_LD, D3D10_SB_OPCODE_LOG,
      D3D10_SB_OPCODE_LT, D3D10_SB_OPCODE_MAD, D3D10_SB_OPCODE_MAX,
      D3D10_SB_OPCODE_MIN, D3D10_SB_OPCODE_MOV, D3D10_SB_OPCODE_MOVC,
      D3D10_SB_OPCODE_MUL, D3D10_SB_OPCODE_NE, D3D10_SB_OPCODE_NOT,
      D3D10_SB_OPCODE_OR, D3D10_SB_OPCODE_RESINFO, D3D10_SB_OPCODE_ROUND_NE,
      D3D10_SB_OPCODE_ROUND_NI, D3D10_SB_OPCODE_ROUND_PI,
      D3D10_SB_OPCODE_ROUND_Z, D3D10_SB_OPCODE_RSQ,
      D3D10_SB_OPCODE_SAMPLE_C_LZ, D3D10_SB_OPCODE_SAMPLE_D,
      D3D10_SB_OPCODE_SAMPLE_L, D3D10_SB_OPCODE_SINCOS, D3D10_SB_OPCODE_SQRT,
      D3D10_SB_OPCODE_UDIV, D3D10_SB_OPCODE_UGE, D3D10_SB_OPCODE_ULT,
      D3D10_SB_OPCODE_UMAD, D3D10_SB_OPCODE_UMAX, D3D10_SB_OPCODE_UMIN,
      D3D10_SB_OPCODE_USHR, D3D10_SB_OPCODE_UTOF, D3D10_SB_OPCODE_XOR,
      D3D10_1_SB_OPCODE_GATHER4, D3D11_SB_OPCODE_BFI, D3D11_SB_OPCODE_BFREV,
      D3D11_SB_OPCODE_COUNTBITS, D3D11_SB_OPCODE_F16TOF32,
      D3D11_SB_OPCODE_F32TOF16, D3D11_SB_OPCODE_FIRSTBIT_HI,
      D3D11_SB_OPCODE_FIRSTBIT_LO, D3D11_SB_OPCODE_FIRSTBIT_SHI,
      D3D11_SB_OPCODE_IBFE, D3D11_SB_OPCODE_RCP, D3D11_SB_OPCODE_UBFE,
  };
  for (auto Opcode : Allowed)
    Table[Opcode] = true;
  return Table;
}

struct MultiExitLoop {
  uint32_t Begin;
  uint32_t End;
  llvm::SmallVector<uint32_t, 4> Exits;
};

void CollectLoopExits(const std::vector<FlowBlock> &Blocks,
                      const DispatchOrder &Order, const Region &R,
                      llvm::SmallVectorImpl<uint32_t> &Exits) {
  for (uint32_t I = R.Begin; I < R.End; ++I) {
    for (const FlowEdge &Edge : Blocks[Order.Blocks[I]].Edges) {
      const uint32_t TargetLabel = Order.Labels[Edge.Target];
      if (TargetLabel >= R.Begin && TargetLabel < R.End)
        continue;
      if (std::find(Exits.begin(), Exits.end(), Edge.Target) == Exits.end())
        Exits.push_back(Edge.Target);
    }
  }
}

void RewriteMultiExitLoop(std::vector<FlowBlock> &Blocks,
                          const DispatchOrder &Order, const MultiExitLoop &ML,
                          uint32_t SelectorReg) {
  const uint32_t MergeId = static_cast<uint32_t>(Blocks.size());
  for (uint32_t I = ML.Begin; I < ML.End; ++I) {
    for (FlowEdge &Edge : Blocks[Order.Blocks[I]].Edges) {
      auto It = std::find(ML.Exits.begin(), ML.Exits.end(), Edge.Target);
      if (It == ML.Exits.end())
        continue;
      const uint32_t ExitIdx = static_cast<uint32_t>(It - ML.Exits.begin());
      const uint32_t Val =
          ML.Exits.size() == 2 ? (ExitIdx == 0 ? UINT32_MAX : 0u) : ExitIdx;
      Edge.Copies.emplace_back(SelectorReg, Immediate(Val));
      Edge.Target = MergeId;
    }
  }
  FlowBlock Merge;
  Merge.Condition = Temp(SelectorReg);
  if (ML.Exits.size() == 2) {
    Merge.Kind = FlowKind::Branch;
    Merge.Edges.push_back({ML.Exits[0], {}});
    Merge.Edges.push_back({ML.Exits[1], {}});
  } else {
    Merge.Kind = FlowKind::Switch;
    for (uint32_t K = 0; K < ML.Exits.size(); ++K)
      Merge.Edges.push_back({ML.Exits[K], {}, K});
  }
  Blocks.push_back(std::move(Merge));
}

bool SameFlowEdge(const FlowEdge &A, const FlowEdge &B) {
  if (A.Target != B.Target || A.Copies.size() != B.Copies.size())
    return false;
  for (size_t K = 0; K < A.Copies.size(); ++K) {
    const auto &CA = A.Copies[K];
    const auto &CB = B.Copies[K];
    if (CA.Destination != CB.Destination ||
        CA.DestinationLane != CB.DestinationLane ||
        !CA.Source.SameValue(CB.Source))
      return false;
  }
  return true;
}

} // namespace

bool dxcb5::flow::IdentityCopies(const FlowEdge &Edge) {
  return std::all_of(Edge.Copies.begin(), Edge.Copies.end(),
                     [](const ParallelCopy &Copy) {
                       return Copy.ReadsTemp() &&
                              Copy.Source.Index == Copy.Destination &&
                              Copy.SourceLane() == Copy.DestinationLane;
                     });
}

void dxcb5::flow::GroupSwitchEdges(const FlowBlock &Block,
                                   const DispatchOrder &Order, uint32_t Stop,
                                   llvm::SmallVectorImpl<SwitchGroup> &Groups) {
  auto EmptyEdge = [&](const FlowEdge &Edge) {
    return Order.RegionOf[Edge.Target] == Stop && IdentityCopies(Edge);
  };
  for (size_t I = 1; I < Block.Edges.size(); ++I) {
    size_t Match = Groups.size();
    for (size_t G = 0; G < Groups.size(); ++G) {
      if (SameFlowEdge(Block.Edges[Groups[G].RepEdge], Block.Edges[I])) {
        Match = G;
        break;
      }
    }
    if (Match == Groups.size()) {
      Groups.emplace_back();
      Groups.back().RepEdge = I;
    }
    Groups[Match].CaseValues.push_back(Block.Edges[I].CaseValue);
  }
  const FlowEdge &DefaultEdge = Block.Edges.front();
  bool DefaultGrouped = false;
  for (auto &G : Groups) {
    if (SameFlowEdge(Block.Edges[G.RepEdge], DefaultEdge)) {
      G.HasDefault = true;
      DefaultGrouped = true;
      break;
    }
  }
  if (!DefaultGrouped && !EmptyEdge(DefaultEdge)) {
    Groups.emplace_back();
    Groups.back().HasDefault = true;
  }
}

DispatchOrder::DispatchOrder(const std::vector<FlowBlock> &Flow)
    : Incoming(Flow.size()) {
  const auto Count = static_cast<uint32_t>(Flow.size());
  std::vector<Node> Nodes(Count);
  for (uint32_t I = 0; I < Count; ++I) {
    Nodes[I].ID = I;
    for (const FlowEdge &E : Flow[I].Edges) {
      Nodes[I].Successors.push_back(&Nodes[E.Target]);
      ++Incoming[E.Target];
    }
  }
  using SCC = llvm::scc_iterator<Node *, NodeTraits>;
  for (auto I = SCC::begin(Nodes.data()); !I.isAtEnd(); ++I) {
    const auto End = static_cast<uint32_t>(Blocks.size());
    for (const Node *N : *I)
      Blocks.push_back(N->ID);
    Regions.push_back({End, static_cast<uint32_t>(Blocks.size()), I.hasLoop()});
  }
  const auto Reachable = static_cast<uint32_t>(Blocks.size());
  std::reverse(Blocks.begin(), Blocks.end());
  std::reverse(Regions.begin(), Regions.end());
  Labels.resize(Count);
  RegionOf.resize(Count);
  for (uint32_t I = 0; I < Regions.size(); ++I) {
    Region &R = Regions[I];
    R = {Reachable - R.End, Reachable - R.Begin, R.Cyclic};
    for (uint32_t Label = R.Begin; Label != R.End; ++Label) {
      Labels[Blocks[Label]] = Label;
      RegionOf[Blocks[Label]] = I;
    }
  }
}

std::vector<bool>
DispatchOrder::ExternalEntries(const std::vector<FlowBlock> &Flow) const {
  std::vector<bool> Entries(Blocks.size());
  Entries[Labels[0]] = true;
  for (const Region &R : Regions) {
    for (uint32_t I = R.Begin; I < R.End; ++I) {
      for (const FlowEdge &Edge : Flow[Blocks[I]].Edges) {
        const uint32_t Target = Labels[Edge.Target];
        if (Target >= R.End)
          Entries[Target] = true;
      }
    }
  }
  return Entries;
}

bool dxcb5::flow::HasQuadOperations(const std::vector<FlowBlock> &Flow,
                                    const DispatchOrder &Order,
                                    const Region &R) {
  for (uint32_t I = R.Begin; I < R.End; ++I)
    for (const Instruction &Inst : Flow[Order.Blocks[I]].Instructions)
      if (RequiresQuad(static_cast<D3D10_SB_OPCODE_TYPE>(Inst.Opcode)))
        return true;
  return false;
}

std::vector<LoopStep>
dxcb5::flow::GetLinearLoop(const std::vector<FlowBlock> &Flow,
                           const DispatchOrder &Order, const Region &R,
                           const std::vector<bool> &Entries) {
  uint32_t Entry = NoIndex;
  for (uint32_t I = R.Begin; I < R.End; ++I) {
    if (!Entries[I])
      continue;
    if (Entry != NoIndex)
      return {};
    Entry = I;
  }
  if (Entry == NoIndex)
    return {};
  std::vector<LoopStep> Steps;
  uint32_t Current = Entry;
  for (uint32_t I = R.Begin; I < R.End; ++I) {
    LoopStep Step = FindLoopEdges(Flow[Order.Blocks[Current]], Order, R);
    if (!Step.Next)
      return {};
    Current = Order.Labels[Step.Next->Target];
    if ((Current == Entry) != (I + 1 == R.End))
      return {};
    Steps.push_back(Step);
  }
  return Steps;
}

bool dxcb5::flow::IsPureArmInstruction(const Instruction &Inst) {
  static const auto PureOpcodes = BuildPureArmTable();
  if (Inst.Opcode >= PureOpcodes.size() || !PureOpcodes[Inst.Opcode] ||
      RequiresQuad(static_cast<D3D10_SB_OPCODE_TYPE>(Inst.Opcode)))
    return false;
  const unsigned Destinations = InstructionDestinationCount(Inst.Opcode);
  if (Destinations == 0 || Destinations > Inst.Operands.size())
    return false;
  return std::all_of(
      Inst.Operands.begin(), Inst.Operands.begin() + Destinations,
      [](const Operand &Dest) {
        return Dest.Kind == OperandKind::Temp ||
               Dest.Kind == OperandKind::Null ||
               Dest.Kind == OperandKind::IndexableTemp;
      });
}

bool dxcb5::flow::NormalizeMultiExitLoops(std::vector<FlowBlock> &Blocks,
                                          uint32_t SelectorReg) {
  const DispatchOrder Order(Blocks);
  const auto Entries = Order.ExternalEntries(Blocks);
  llvm::SmallVector<MultiExitLoop, 4> Loops;
  for (const Region &R : Order.Regions) {
    if (!R.Cyclic)
      continue;
    unsigned EntryCount = 0;
    for (uint32_t I = R.Begin; I < R.End; ++I)
      EntryCount += Entries[I] ? 1u : 0u;
    if (EntryCount != 1)
      continue;
    llvm::SmallVector<uint32_t, 4> Exits;
    CollectLoopExits(Blocks, Order, R, Exits);
    if (Exits.size() >= 2)
      Loops.push_back({R.Begin, R.End, std::move(Exits)});
  }
  if (Loops.empty())
    return false;
  for (const auto &ML : Loops)
    RewriteMultiExitLoop(Blocks, Order, ML, SelectorReg);
  return true;
}
