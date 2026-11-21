// Distributed under the University of Illinois Open Source License.
#include "RegAlloc/LaneInterference.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>

using namespace dxcb5;
using namespace dxcb5::regalloc;

namespace {

void AddAffinity(std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
                 std::vector<uint64_t> &TotalWeight, uint32_t A, uint8_t LaneA,
                 uint32_t B, uint8_t LaneB, uint32_t Weight) {
  if (A == B)
    return;
  auto Insert = [&](uint32_t From, uint8_t FromL, uint32_t To, uint8_t ToL) {
    for (auto &Link : Affinities[From]) {
      if (Link.Peer == To && Link.MyLane == FromL && Link.PeerLane == ToL) {
        Link.Weight += Weight;
        TotalWeight[From] += Weight;
        return;
      }
    }
    Affinities[From].push_back({To, FromL, ToL, Weight});
    TotalWeight[From] += Weight;
  };
  Insert(A, LaneA, B, LaneB);
  Insert(B, LaneB, A, LaneA);
}

void AddRegMask(SparseSlotState &State,
                const std::vector<std::array<uint32_t, 4>> &SlotOf,
                uint32_t R, uint8_t Mask) {
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if (Mask & (1u << L))
      State.AddSlot(SlotOf[R][L]);
}

void KillRegMask(SparseSlotState &State,
                 const std::vector<std::array<uint32_t, 4>> &SlotOf,
                 uint32_t R, uint8_t Mask) {
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if (Mask & (1u << L))
      State.KillSlot(SlotOf[R][L]);
}

void ResetFromLive(SparseSlotState &State, const LivenessResult &Live,
                   const std::vector<std::array<uint32_t, 4>> &SlotOf,
                   const std::vector<uint64_t> &Packed) {
  State.Clear();
  Live.ForEachLive(Packed, [&](uint32_t R, uint8_t Mask) {
    AddRegMask(State, SlotOf, R, Mask);
  });
}

void RecordInstInterference(
    const Instruction &Inst, SparseSlotState &State,
    const std::vector<std::array<uint32_t, 4>> &SlotOf,
    SlotInterferenceGraph &Graph,
    std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
    std::vector<uint64_t> &TotalWeight) {
  const size_t Dests = std::min<size_t>(
      InstructionDestinationCount(Inst.Opcode), Inst.Operands.size());
  const uint8_t DestMask = CombinedInstructionDestMask(Inst);
  const bool PlainTempMov =
      Inst.Opcode == D3D10_SB_OPCODE_MOV && !Inst.Saturate &&
      Inst.Operands.size() >= 2 &&
      Inst.Operands[0].Kind == OperandKind::Temp &&
      Inst.Operands[1].Kind == OperandKind::Temp &&
      Inst.Operands[1].Modifier == 0 &&
      Inst.Operands[1].RelativeRegister == UINT32_MAX;

  if (PlainTempMov) {
    const Operand &Dst = Inst.Operands[0];
    const Operand &Src = Inst.Operands[1];
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      if (Dst.Mask & (1u << L)) {
        const uint8_t SL = static_cast<uint8_t>((Src.Swizzle >> (2 * L)) & 3u);
        AddAffinity(Affinities, TotalWeight, Dst.Index, static_cast<uint8_t>(L),
                    Src.Index, SL, 8);
      }
    }
  }
  for (size_t I = 0; I < Dests; ++I)
    if (Inst.Operands[I].Kind == OperandKind::Temp)
      AddRegMask(State, SlotOf, Inst.Operands[I].Index, Inst.Operands[I].Mask);

  for (size_t I = 0; I < Dests; ++I) {
    const Operand &Dst = Inst.Operands[I];
    if (Dst.Kind != OperandKind::Temp)
      continue;
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      if (!(Dst.Mask & (1u << L)))
        continue;
      uint32_t SkipS = kNoSlot;
      if (PlainTempMov && I == 0) {
        const unsigned SL = (Inst.Operands[1].Swizzle >> (2 * L)) & 3u;
        SkipS = SlotOf[Inst.Operands[1].Index][SL];
      }
      Graph.InterfereAgainst(SlotOf[Dst.Index][L], State, SkipS);
    }
  }
  for (size_t I = 0; I < Dests; ++I) {
    const Operand &Dst = Inst.Operands[I];
    if (Dst.Kind == OperandKind::Temp)
      KillRegMask(State, SlotOf, Dst.Index, Dst.Mask);
    if (Dst.RelativeRegister != UINT32_MAX)
      State.AddSlot(SlotOf[Dst.RelativeRegister][Dst.RelativeLane]);
  }
  for (size_t I = Dests; I < Inst.Operands.size(); ++I) {
    const Operand &Src = Inst.Operands[I];
    if (Src.Kind == OperandKind::Temp) {
      const uint8_t Read = InstructionSourceReadLanes(
          Inst.Opcode, DestMask, I - Dests, Src.Swizzle);
      AddRegMask(State, SlotOf, Src.Index, Read);
    }
    if (Src.RelativeRegister != UINT32_MAX)
      State.AddSlot(SlotOf[Src.RelativeRegister][Src.RelativeLane]);
  }
}

struct CoalescePair {
  uint32_t A;
  uint32_t B;
  uint64_t Weight;
};

bool BuildLaneBijection(
    const CoalescePair &P, uint32_t RA, uint32_t RB, uint8_t W,
    const CoalesceState &State,
    const std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
    std::array<int8_t, 4> &BtoA) {
  BtoA = {-1, -1, -1, -1};
  if (W == 1) {
    const uint8_t LA = static_cast<uint8_t>(llvm::countTrailingZeros(State.RepMask[RA]));
    const uint8_t LB = static_cast<uint8_t>(llvm::countTrailingZeros(State.RepMask[RB]));
    BtoA[LB] = static_cast<int8_t>(LA);
    return true;
  }
  std::array<std::array<uint64_t, 4>, 4> Votes{};
  for (const AffinityLink &L : Affinities[P.A]) {
    if (L.Peer == P.B) {
      const uint8_t RaL = State.LaneToRep[P.A][L.MyLane];
      const uint8_t RbL = State.LaneToRep[P.B][L.PeerLane];
      Votes[RbL][RaL] += L.Weight;
    }
  }
  uint8_t UsedA = 0;
  for (unsigned RbL = 0; RbL < kRegisterLanes; ++RbL) {
    if (!(State.RepMask[RB] & (1u << RbL)))
      continue;
    uint64_t BestV = 0;
    int8_t BestA = -1;
    for (unsigned RaL = 0; RaL < kRegisterLanes; ++RaL) {
      if ((State.RepMask[RA] & (1u << RaL)) && !(UsedA & (1u << RaL)) &&
          Votes[RbL][RaL] > BestV) {
        BestV = Votes[RbL][RaL];
        BestA = static_cast<int8_t>(RaL);
      }
    }
    if (BestA < 0)
      return false;
    BtoA[RbL] = BestA;
    UsedA |= static_cast<uint8_t>(1u << BestA);
  }
  return true;
}

void TrySameWidthMerge(
    const CoalescePair &P, const LivenessResult &Live,
    const std::vector<std::array<uint32_t, 4>> &SlotOf,
    const std::vector<uint8_t> &PreColored,
    const std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
    std::vector<uint64_t> &TotalWeight, SlotInterferenceGraph &Graph,
    CoalesceState &State) {
  uint32_t RA = State.Rep[P.A];
  uint32_t RB = State.Rep[P.B];
  if (RA == RB || PreColored[RA] || PreColored[RB])
    return;
  const uint8_t W = Live.Temps[RA].Width;
  if (W != Live.Temps[RB].Width)
    return;
  if (State.RepFixed[RA] && State.RepFixed[RB] &&
      State.RepMask[RA] != State.RepMask[RB])
    return;
  std::array<int8_t, 4> BtoA;
  if (!BuildLaneBijection(P, RA, RB, W, State, Affinities, BtoA))
    return;
  if (!State.RepFixed[RA] && State.RepFixed[RB]) {
    std::array<int8_t, 4> Inv = {-1, -1, -1, -1};
    for (unsigned RbL = 0; RbL < kRegisterLanes; ++RbL)
      if (BtoA[RbL] >= 0)
        Inv[BtoA[RbL]] = static_cast<int8_t>(RbL);
    std::swap(RA, RB);
    BtoA = Inv;
  }
  if (State.RepFixed[RA] && State.RepFixed[RB]) {
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((State.RepMask[RB] & (1u << L)) && BtoA[L] != static_cast<int8_t>(L))
        return;
  }
  for (unsigned RbL = 0; RbL < kRegisterLanes; ++RbL) {
    if ((State.RepMask[RB] & (1u << RbL)) &&
        Graph.Interferes(SlotOf[RA][BtoA[RbL]], SlotOf[RB][RbL]))
      return;
  }
  for (unsigned RbL = 0; RbL < kRegisterLanes; ++RbL)
    if (State.RepMask[RB] & (1u << RbL))
      Graph.MergeSlots(SlotOf[RA][BtoA[RbL]], SlotOf[RB][RbL]);
  for (uint32_t M : State.Members[RB]) {
    State.Rep[M] = RA;
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      const uint8_t OldRbL = State.LaneToRep[M][L];
      if (BtoA[OldRbL] >= 0)
        State.LaneToRep[M][L] = static_cast<uint8_t>(BtoA[OldRbL]);
    }
    State.Members[RA].push_back(M);
  }
  TotalWeight[RA] += TotalWeight[RB];
  State.Members[RB].clear();
}

void TryScalarIntoVectorMerge(
    const CoalescePair &P,
    const std::vector<std::array<uint32_t, 4>> &SlotOf,
    const std::vector<uint8_t> &PreColored,
    const std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
    std::vector<uint64_t> &TotalWeight, SlotInterferenceGraph &Graph,
    CoalesceState &State) {
  uint32_t RA = State.Rep[P.A];
  uint32_t RB = State.Rep[P.B];
  if (RA == RB || PreColored[RA] || PreColored[RB])
    return;
  const uint8_t WA = Popcount4(State.RepMask[RA]);
  const uint8_t WB = Popcount4(State.RepMask[RB]);
  if (!((WA >= 2 && WB == 1) || (WB >= 2 && WA == 1)))
    return;
  uint32_t WideOrig = P.A, ScalarOrig = P.B;
  if (WA == 1) {
    std::swap(RA, RB);
    std::swap(WideOrig, ScalarOrig);
  }
  if (State.RepFixed[RB])
    return;
  const uint8_t LB =
      static_cast<uint8_t>(llvm::countTrailingZeros(State.RepMask[RB]));
  std::array<uint64_t, 4> LaneVotes{};
  for (const AffinityLink &L : Affinities[WideOrig]) {
    if (L.Peer == ScalarOrig) {
      const uint8_t RaL = State.LaneToRep[WideOrig][L.MyLane];
      if (State.RepMask[RA] & (1u << RaL))
        LaneVotes[RaL] += L.Weight;
    }
  }
  int8_t BestRaL = -1;
  uint64_t BestW = 0;
  for (unsigned RaL = 0; RaL < kRegisterLanes; ++RaL) {
    if (LaneVotes[RaL] > BestW) {
      BestW = LaneVotes[RaL];
      BestRaL = static_cast<int8_t>(RaL);
    }
  }
  if (BestRaL < 0)
    return;
  const uint32_t WideS = SlotOf[RA][BestRaL];
  const uint32_t ScalarS = SlotOf[RB][LB];
  if (Graph.Interferes(WideS, ScalarS) ||
      Graph.GetDegree(WideS) + Graph.GetDegree(ScalarS) > 96)
    return;
  Graph.MergeSlots(WideS, ScalarS);
  for (uint32_t M : State.Members[RB]) {
    State.Rep[M] = RA;
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if (State.LaneToRep[M][L] == LB)
        State.LaneToRep[M][L] = static_cast<uint8_t>(BestRaL);
    State.Members[RA].push_back(M);
  }
  TotalWeight[RA] += TotalWeight[RB];
  State.Members[RB].clear();
}

} // namespace

BitWord SlotInterferenceGraph::LookupSparseWord(uint32_t S, uint16_t W) const {
  const auto &Row = SparseRows[S];
  auto It = std::lower_bound(
      Row.begin(), Row.end(), W,
      [](const WordChunk &C, uint16_t Target) { return C.Word < Target; });
  return (It != Row.end() && It->Word == W) ? It->Bits : 0;
}

BitWord SlotInterferenceGraph::OrSparseWord(uint32_t S, uint16_t W,
                                            BitWord Bits) {
  auto &Row = SparseRows[S];
  if (!Row.empty() && Row.back().Word <= W) {
    if (Row.back().Word == W) {
      const BitWord Added = Bits & ~Row.back().Bits;
      Row.back().Bits |= Bits;
      return Added;
    }
    Row.push_back({W, Bits});
    return Bits;
  }
  auto It = std::lower_bound(
      Row.begin(), Row.end(), W,
      [](const WordChunk &C, uint16_t Target) { return C.Word < Target; });
  if (It != Row.end() && It->Word == W) {
    const BitWord Added = Bits & ~It->Bits;
    It->Bits |= Bits;
    return Added;
  }
  Row.insert(It, {W, Bits});
  return Bits;
}

void SlotInterferenceGraph::InterfereAgainst(uint32_t S,
                                             const SparseSlotState &State,
                                             uint32_t SkipSlot) {
  if (S == kNoSlot)
    return;
  const uint32_t OwnW = S / kWordBits;
  const BitWord OwnBit = BitWord{1} << (S % kWordBits);
  const uint32_t SkipW = SkipSlot == kNoSlot ? UINT32_MAX : SkipSlot / kWordBits;
  const BitWord SkipBit =
      SkipSlot == kNoSlot ? 0 : (BitWord{1} << (SkipSlot % kWordBits));

  if (UseSparse) {
    for (const uint32_t W : State.ActiveWords()) {
      BitWord Cand = State.Word(W);
      if (W == OwnW)
        Cand &= ~OwnBit;
      if (W == SkipW)
        Cand &= ~SkipBit;
      if (!Cand)
        continue;
      BitWord Added = OrSparseWord(S, static_cast<uint16_t>(W), Cand);
      while (Added) {
        const uint32_t Other =
            W * kWordBits + static_cast<uint32_t>(llvm::countTrailingZeros(Added));
        OrSparseWord(Other, static_cast<uint16_t>(OwnW), OwnBit);
        ++Degree[S];
        ++Degree[Other];
        Added &= Added - 1;
      }
    }
    return;
  }

  BitWord *Adj = Row(S);
  for (const uint32_t W : State.ActiveWords()) {
    BitWord NewBits = State.Word(W) & ~Adj[W];
    if (W == OwnW)
      NewBits &= ~OwnBit;
    if (W == SkipW)
      NewBits &= ~SkipBit;
    if (!NewBits)
      continue;
    Adj[W] |= NewBits;
    while (NewBits) {
      const uint32_t Other =
          W * kWordBits + static_cast<uint32_t>(llvm::countTrailingZeros(NewBits));
      Row(Other)[OwnW] |= OwnBit;
      ++Degree[S];
      ++Degree[Other];
      NewBits &= NewBits - 1;
    }
  }
}

void SlotInterferenceGraph::RecordLiveInputs(const SparseSlotState &State) {
  for (const uint32_t W : State.ActiveWords()) {
    BitWord Bits = State.Word(W);
    while (Bits) {
      const uint32_t S =
          W * kWordBits + static_cast<uint32_t>(llvm::countTrailingZeros(Bits));
      InterfereAgainst(S, State);
      Bits &= Bits - 1;
    }
  }
}

void SlotInterferenceGraph::MergeSlots(uint32_t KeepS, uint32_t DeadS) {
  if (KeepS == kNoSlot || DeadS == kNoSlot || KeepS == DeadS)
    return;
  const uint32_t KeepW = KeepS / kWordBits;
  const BitWord KeepBit = BitWord{1} << (KeepS % kWordBits);
  if (UseSparse) {
    const auto DeadCopy = SparseRows[DeadS];
    for (const WordChunk &C : DeadCopy) {
      BitWord Cand = C.Bits;
      if (C.Word == KeepW)
        Cand &= ~KeepBit;
      if (!Cand)
        continue;
      BitWord Added = OrSparseWord(KeepS, C.Word, Cand);
      while (Added) {
        const uint32_t Other =
            static_cast<uint32_t>(C.Word) * kWordBits +
            static_cast<uint32_t>(llvm::countTrailingZeros(Added));
        OrSparseWord(Other, static_cast<uint16_t>(KeepW), KeepBit);
        ++Degree[KeepS];
        ++Degree[Other];
        Added &= Added - 1;
      }
    }
    return;
  }

  BitWord *KeepRow = Row(KeepS);
  const BitWord *DeadRow = Row(DeadS);
  for (size_t W = 0; W < Stride; ++W) {
    BitWord Added = DeadRow[W] & ~KeepRow[W];
    if (W == KeepW)
      Added &= ~KeepBit;
    if (!Added)
      continue;
    KeepRow[W] |= Added;
    while (Added) {
      const uint32_t Other = static_cast<uint32_t>(
          W * kWordBits + llvm::countTrailingZeros(Added));
      Row(Other)[KeepW] |= KeepBit;
      ++Degree[KeepS];
      ++Degree[Other];
      Added &= Added - 1;
    }
  }
}

void dxcb5::regalloc::BuildInterferenceAndAffinities(
    llvm::ArrayRef<FlowBlock> Blocks, const LivenessResult &Live,
    const std::vector<std::array<uint32_t, 4>> &SlotOf, size_t NumSlots,
    SlotInterferenceGraph &Graph,
    std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
    std::vector<uint64_t> &TotalWeight) {
  llvm::TimeTraceScope Timer("DXCB5 Register Interference", llvm::StringRef());
  SparseSlotState State(NumSlots);
  for (uint32_t B = 0; B < Blocks.size(); ++B) {
    const FlowBlock &Block = Blocks[B];
    for (const FlowEdge &Edge : Block.Edges) {
      if (Edge.Copies.empty())
        continue;
      ResetFromLive(State, Live, SlotOf, Live.Blocks[Edge.Target].In);
      for (const ParallelCopy &Copy : Edge.Copies)
        State.AddSlot(SlotOf[Copy.Destination][Copy.DestinationLane]);
      const uint32_t CopyWeight =
          Edge.Target <= B ? 100u : (Edge.Copies.size() <= 4 ? 40u : 10u);
      for (const ParallelCopy &Copy : Edge.Copies) {
        Graph.InterfereAgainst(SlotOf[Copy.Destination][Copy.DestinationLane],
                               State);
        if (Copy.ReadsTemp()) {
          AddAffinity(Affinities, TotalWeight, Copy.Destination,
                      Copy.DestinationLane, Copy.Source.Index,
                      Copy.SourceLane(), CopyWeight);
        }
      }
    }
    ResetFromLive(State, Live, SlotOf, Live.Blocks[B].Out);
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch) {
      if (Block.Condition.Kind == OperandKind::Temp)
        State.AddSlot(SlotOf[Block.Condition.Index][Block.Condition.Swizzle & 3u]);
      if (Block.Condition.RelativeRegister != UINT32_MAX)
        State.AddSlot(SlotOf[Block.Condition.RelativeRegister][Block.Condition.RelativeLane]);
    }
    for (auto It = Block.Instructions.rbegin(); It != Block.Instructions.rend(); ++It)
      RecordInstInterference(*It, State, SlotOf, Graph, Affinities, TotalWeight);
    if (B == 0 || Live.Blocks[B].Predecessors.empty())
      Graph.RecordLiveInputs(State);
  }
}

void dxcb5::regalloc::PreCoalesceRegisters(
    const LivenessResult &Live,
    const std::vector<std::array<uint32_t, 4>> &SlotOf,
    const std::vector<uint8_t> &PreColored,
    const std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
    std::vector<uint64_t> &TotalWeight, SlotInterferenceGraph &Graph,
    CoalesceState &State) {
  llvm::TimeTraceScope MergeTimer("DXCB5 Register Pre-Coalesce", llvm::StringRef());
  llvm::DenseMap<uint64_t, uint64_t> PairWeights;
  for (uint32_t R : Live.ActiveTemps) {
    for (const AffinityLink &L : Affinities[R]) {
      if (R < L.Peer)
        PairWeights[(uint64_t(R) << 32) | L.Peer] += L.Weight;
    }
  }
  std::vector<CoalescePair> Pairs;
  Pairs.reserve(PairWeights.size());
  for (const auto &KV : PairWeights) {
    Pairs.push_back({static_cast<uint32_t>(KV.first >> 32),
                     static_cast<uint32_t>(KV.first), KV.second});
  }
  std::sort(Pairs.begin(), Pairs.end(), [](const CoalescePair &X, const CoalescePair &Y) {
    if (X.Weight != Y.Weight)
      return X.Weight > Y.Weight;
    return X.A != Y.A ? X.A < Y.A : X.B < Y.B;
  });
  for (const CoalescePair &P : Pairs)
    TrySameWidthMerge(P, Live, SlotOf, PreColored, Affinities, TotalWeight,
                      Graph, State);
  for (const CoalescePair &P : Pairs)
    TryScalarIntoVectorMerge(P, SlotOf, PreColored, Affinities, TotalWeight,
                             Graph, State);
}
