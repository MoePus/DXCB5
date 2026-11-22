// Distributed under the University of Illinois Open Source License.
#include "RegAlloc/LaneAllocator.h"
#include "RegAlloc/LaneInterference.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <cassert>
#include <limits>

using namespace dxcb5;
using namespace dxcb5::regalloc;

namespace {

bool TryMatchLanes(uint8_t ActiveMask, bool FixedLanes,
                   const std::array<uint8_t, 4> &FreeForLane,
                   const std::array<int8_t, 4> &PreferredLane,
                   std::array<uint8_t, 4> &OutMap, uint8_t &OutPhysMask) {
  if (FixedLanes) {
    uint8_t First = 0;
    bool HaveFirst = false;
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      if (!(ActiveMask & (1u << L)))
        continue;
      if (!(FreeForLane[L] & (1u << L)))
        return false;
      if (!HaveFirst) {
        First = static_cast<uint8_t>(L);
        HaveFirst = true;
      }
    }
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      OutMap[L] = (ActiveMask & (1u << L)) ? static_cast<uint8_t>(L) : First;
    OutPhysMask = ActiveMask;
    return true;
  }

  uint8_t ActiveVLs[4];
  unsigned W = 0;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(ActiveMask & (1u << L)))
      continue;
    if (!FreeForLane[L])
      return false;
    ActiveVLs[W++] = static_cast<uint8_t>(L);
  }

  int BestScore = -1;
  uint8_t BestAssigned[4] = {};
  uint8_t CurAssigned[4] = {};
  auto Search = [&](auto &Self, unsigned Idx, uint8_t UsedPMask, int Score) -> void {
    if (Idx == W) {
      if (Score > BestScore) {
        BestScore = Score;
        for (unsigned K = 0; K < W; ++K)
          BestAssigned[K] = CurAssigned[K];
      }
      return;
    }
    const uint8_t VL = ActiveVLs[Idx];
    uint8_t Avail = static_cast<uint8_t>(FreeForLane[VL] & ~UsedPMask);
    while (Avail) {
      const uint8_t PL = static_cast<uint8_t>(llvm::countTrailingZeros(Avail));
      Avail &= static_cast<uint8_t>(Avail - 1);
      int Bonus = static_cast<int>(3u - PL) + (PL == VL ? 10 : 0) +
                  (PreferredLane[VL] == static_cast<int8_t>(PL) ? 1000 : 0);
      CurAssigned[Idx] = PL;
      Self(Self, Idx + 1, static_cast<uint8_t>(UsedPMask | (1u << PL)),
           Score + Bonus);
    }
  };
  Search(Search, 0, 0, 0);
  if (BestScore < 0)
    return false;

  const uint8_t First = BestAssigned[0];
  uint8_t PMask = 0;
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    OutMap[L] = First;
  for (unsigned K = 0; K < W; ++K) {
    OutMap[ActiveVLs[K]] = BestAssigned[K];
    PMask |= static_cast<uint8_t>(1u << BestAssigned[K]);
  }
  OutPhysMask = PMask;
  return true;
}

#ifndef NDEBUG
bool VerifySlotAllocation(const SlotInterferenceGraph &Graph,
                          const LivenessResult &Live,
                          const std::vector<std::array<uint32_t, 4>> &SlotOf,
                          const std::vector<uint32_t> &SlotVreg,
                          const std::vector<uint8_t> &SlotLane,
                          const CoalesceState &State,
                          const std::vector<LaneAllocation> &Alloc) {
  for (uint32_t R : Live.ActiveTemps) {
    const TempInfo &Info = Live.Temps[R];
    const LaneAllocation &A = Alloc[R];
    if (A.PhysReg == UINT32_MAX || Popcount4(A.PhysMask) != Info.Width)
      return false;
    if (Info.FixedLanes && A.PhysMask != Info.ActiveMask)
      return false;
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      const uint32_t S = SlotOf[R][L];
      if (S == kNoSlot)
        continue;
      const uint8_t MyPL = A.LaneMap[L];
      bool Conflict = false;
      Graph.ForEachNeighbor(S, [&](uint32_t OtherS) {
        const uint32_t OR = SlotVreg[OtherS];
        const uint8_t OL = SlotLane[OtherS];
        if (State.Rep[OR] == State.Rep[R] &&
            State.LaneToRep[OR][OL] == State.LaneToRep[R][L])
          return;
        if (Alloc[OR].PhysReg == A.PhysReg && Alloc[OR].LaneMap[OL] == MyPL)
          Conflict = true;
      });
      if (Conflict)
        return false;
    }
  }
  return true;
}
#endif

struct LinearSpan {
  uint64_t First = std::numeric_limits<uint64_t>::max();
  uint64_t Last = 0;
  void Touch(uint64_t P) {
    First = std::min(First, P);
    Last = std::max(Last, P);
  }
};

void CollectLinearSpans(llvm::ArrayRef<FlowBlock> Blocks,
                        const LivenessResult &Live,
                        std::vector<LinearSpan> &Spans) {
  std::vector<LinearSpan> Incoming(Blocks.size());
  std::vector<uint64_t> BlockFirst(Blocks.size()), BlockLast(Blocks.size());
  uint64_t Pos = 0;
  for (size_t B = 0; B < Blocks.size(); ++B) {
    BlockFirst[B] = Pos++;
    for (const Instruction &Inst : Blocks[B].Instructions) {
      const uint64_t At = Pos++;
      for (const Operand &Op : Inst.Operands) {
        if (Op.Kind == OperandKind::Temp)
          Spans[Op.Index].Touch(At);
        if (Op.RelativeRegister != UINT32_MAX)
          Spans[Op.RelativeRegister].Touch(At);
      }
    }
    BlockLast[B] = Pos++;
    ++Pos;
    if (Blocks[B].Kind == FlowKind::Branch || Blocks[B].Kind == FlowKind::Switch) {
      if (Blocks[B].Condition.Kind == OperandKind::Temp)
        Spans[Blocks[B].Condition.Index].Touch(BlockLast[B]);
      if (Blocks[B].Condition.RelativeRegister != UINT32_MAX)
        Spans[Blocks[B].Condition.RelativeRegister].Touch(BlockLast[B]);
    }
    for (const FlowEdge &Edge : Blocks[B].Edges) {
      Incoming[Edge.Target].Touch(BlockLast[B] + 1);
      for (const ParallelCopy &Copy : Edge.Copies) {
        if (Copy.ReadsTemp())
          Spans[Copy.Source.Index].Touch(BlockLast[B]);
        Spans[Copy.Destination].Touch(BlockLast[B] + 1);
      }
    }
  }
  for (size_t B = 0; B < Blocks.size(); ++B) {
    Live.ForEachLive(Live.Blocks[B].In, [&](uint32_t R, uint8_t) {
      Spans[R].Touch(BlockFirst[B]);
      if (Incoming[B].First != std::numeric_limits<uint64_t>::max()) {
        Spans[R].Touch(Incoming[B].First);
        Spans[R].Touch(Incoming[B].Last);
      }
    });
    Live.ForEachLive(Live.Blocks[B].Out, [&](uint32_t R, uint8_t) {
      Spans[R].Touch(BlockLast[B]);
    });
  }
}

bool AllocateLinearFallback(llvm::ArrayRef<FlowBlock> Blocks,
                            const LivenessResult &Live,
                            std::vector<LaneAllocation> &Allocation,
                            uint32_t &PhysTempCount) {
  std::vector<LinearSpan> Spans(Live.Temps.size());
  CollectLinearSpans(Blocks, Live, Spans);
  std::vector<uint32_t> Order = Live.ActiveTemps;
  std::stable_sort(Order.begin(), Order.end(), [&](uint32_t A, uint32_t B) {
    if (Spans[A].First != Spans[B].First)
      return Spans[A].First < Spans[B].First;
    return Live.Temps[A].Width > Live.Temps[B].Width;
  });

  std::vector<std::array<uint64_t, 4>> LaneEnd;
  const std::array<int8_t, 4> NoPref = {-1, -1, -1, -1};
  for (uint32_t R : Order) {
    const uint64_t Start = Spans[R].First, End = Spans[R].Last;
    const TempInfo &Info = Live.Temps[R];
    uint32_t BestReg = UINT32_MAX;
    uint8_t BestFreeCount = 5;
    std::array<uint8_t, 4> BestMap = {0, 1, 2, 3};
    uint8_t BestMask = 0;
    for (uint32_t P = 0; P < LaneEnd.size(); ++P) {
      uint8_t Free = 0;
      for (unsigned L = 0; L < kRegisterLanes; ++L)
        if (LaneEnd[P][L] < Start)
          Free |= static_cast<uint8_t>(1u << L);
      const uint8_t FC = Popcount4(Free);
      if (FC >= BestFreeCount)
        continue;
      const std::array<uint8_t, 4> FreeArr = {Free, Free, Free, Free};
      std::array<uint8_t, 4> CandMap;
      uint8_t CandMask = 0;
      if (TryMatchLanes(Info.ActiveMask, Info.FixedLanes, FreeArr, NoPref,
                        CandMap, CandMask)) {
        BestReg = P;
        BestFreeCount = FC;
        BestMap = CandMap;
        BestMask = CandMask;
        if (FC == Info.Width)
          break;
      }
    }
    if (BestReg == UINT32_MAX) {
      if (LaneEnd.size() >= kShaderTempLimit)
        return false;
      BestReg = static_cast<uint32_t>(LaneEnd.size());
      LaneEnd.push_back({0, 0, 0, 0});
      const std::array<uint8_t, 4> AllFree = {0xfu, 0xfu, 0xfu, 0xfu};
      TryMatchLanes(Info.ActiveMask, Info.FixedLanes, AllFree, NoPref, BestMap,
                    BestMask);
    }
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if (BestMask & (1u << L))
        LaneEnd[BestReg][L] = End;
    Allocation[R] = {BestReg, BestMap, BestMask};
  }
  PhysTempCount = static_cast<uint32_t>(LaneEnd.size());
  return true;
}

struct RepAffinity {
  uint32_t PeerRep;
  uint8_t MyRepLane;
  uint8_t PeerRepLane;
  uint32_t Weight;
};

struct ColorBusyState {
  std::vector<std::array<uint8_t, 4>> BusyPerLane;
  std::vector<uint32_t> BusyEpoch;
  uint32_t Epoch = 1;

  std::array<uint8_t, 4> GetFree(uint32_t P) const {
    if (P >= BusyEpoch.size() || BusyEpoch[P] != Epoch)
      return {0xfu, 0xfu, 0xfu, 0xfu};
    return {static_cast<uint8_t>(~BusyPerLane[P][0] & 0xfu),
            static_cast<uint8_t>(~BusyPerLane[P][1] & 0xfu),
            static_cast<uint8_t>(~BusyPerLane[P][2] & 0xfu),
            static_cast<uint8_t>(~BusyPerLane[P][3] & 0xfu)};
  }

  void RecordBusyLane(uint32_t P, unsigned L, uint8_t OtherPL) {
    if (BusyEpoch[P] != Epoch) {
      BusyEpoch[P] = Epoch;
      BusyPerLane[P].fill(0);
    }
    BusyPerLane[P][L] |= static_cast<uint8_t>(1u << OtherPL);
  }

  void MarkNeighborBusy(uint32_t RA, uint32_t NumPhys,
                        const SlotInterferenceGraph &Graph,
                        const std::vector<std::array<uint32_t, 4>> &SlotOf,
                        const std::vector<uint32_t> &SlotVreg,
                        const std::vector<uint8_t> &SlotLane,
                        const CoalesceState &State,
                        const std::vector<LaneAllocation> &Allocation) {
    ++Epoch;
    if (BusyEpoch.size() < NumPhys) {
      BusyEpoch.resize(NumPhys, 0);
      BusyPerLane.resize(NumPhys, {0, 0, 0, 0});
    }
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      const uint32_t S = SlotOf[RA][L];
      if (S == kNoSlot)
        continue;
      Graph.ForEachNeighbor(S, [&](uint32_t OtherS) {
        const uint32_t OR = State.Rep[SlotVreg[OtherS]];
        const uint32_t P = Allocation[OR].PhysReg;
        if (P == UINT32_MAX)
          return;
        const uint8_t OtherRepL =
            State.LaneToRep[SlotVreg[OtherS]][SlotLane[OtherS]];
        RecordBusyLane(P, L, Allocation[OR].LaneMap[OtherRepL]);
      });
    }
  }
};

std::array<int8_t, 4> ComputePreferredLanes(
    uint32_t P, const std::array<uint8_t, 4> &FreeArr,
    llvm::ArrayRef<RepAffinity> Links,
    const std::vector<LaneAllocation> &Allocation) {
  std::array<int8_t, 4> Pref = {-1, -1, -1, -1};
  std::array<std::array<uint64_t, 4>, 4> Votes{};
  for (const auto &Link : Links) {
    if (Allocation[Link.PeerRep].PhysReg != P)
      continue;
    const uint8_t TargetPL = Allocation[Link.PeerRep].LaneMap[Link.PeerRepLane];
    if (FreeArr[Link.MyRepLane] & (1u << TargetPL))
      Votes[Link.MyRepLane][TargetPL] += Link.Weight;
  }
  for (unsigned ML = 0; ML < kRegisterLanes; ++ML) {
    uint64_t Best = 0;
    for (unsigned PL = 0; PL < kRegisterLanes; ++PL) {
      if (Votes[ML][PL] > Best) {
        Best = Votes[ML][PL];
        Pref[ML] = static_cast<int8_t>(PL);
      }
    }
  }
  return Pref;
}

bool PickAffinityColor(uint32_t RA, uint8_t ActiveMask, bool Fixed,
                       const ColorBusyState &Busy, const CoalesceState &State,
                       const std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
                       const std::vector<LaneAllocation> &Allocation,
                       LaneAllocation &Out) {
  llvm::SmallVector<RepAffinity, 8> Links;
  llvm::SmallVector<uint32_t, 8> CandidateRegs;
  for (uint32_t M : State.Members[RA]) {
    for (const AffinityLink &L : Affinities[M]) {
      const uint32_t PeerRep = State.Rep[L.Peer];
      if (PeerRep == RA)
        continue;
      const uint32_t P = Allocation[PeerRep].PhysReg;
      if (P == UINT32_MAX)
        continue;
      Links.push_back({PeerRep, State.LaneToRep[M][L.MyLane],
                       State.LaneToRep[L.Peer][L.PeerLane], L.Weight});
      if (std::find(CandidateRegs.begin(), CandidateRegs.end(), P) ==
          CandidateRegs.end())
        CandidateRegs.push_back(P);
    }
  }

  uint32_t ChosenReg = UINT32_MAX;
  std::array<uint8_t, 4> ChosenMap = {0, 1, 2, 3};
  uint8_t ChosenMask = 0;
  uint64_t BestScore = 0;
  for (uint32_t P : CandidateRegs) {
    const auto FreeArr = Busy.GetFree(P);
    const auto Pref = ComputePreferredLanes(P, FreeArr, Links, Allocation);
    std::array<uint8_t, 4> CandMap;
    uint8_t CandMask = 0;
    if (!TryMatchLanes(ActiveMask, Fixed, FreeArr, Pref, CandMap, CandMask))
      continue;
    uint64_t Score = 0;
    for (const auto &Link : Links) {
      if (Allocation[Link.PeerRep].PhysReg == P &&
          CandMap[Link.MyRepLane] ==
              Allocation[Link.PeerRep].LaneMap[Link.PeerRepLane])
        Score += Link.Weight;
    }
    if (Score > BestScore ||
        (Score > 0 && Score == BestScore && P < ChosenReg)) {
      BestScore = Score;
      ChosenReg = P;
      ChosenMap = CandMap;
      ChosenMask = CandMask;
    }
  }
  if (ChosenReg == UINT32_MAX)
    return false;
  Out = {ChosenReg, ChosenMap, ChosenMask};
  return true;
}

bool PickBestFitColor(uint8_t ActiveMask, bool Fixed, uint8_t Width,
                      const ColorBusyState &Busy, uint32_t &NumPhys,
                      LaneAllocation &Out) {
  const std::array<int8_t, 4> NoPref = {-1, -1, -1, -1};
  uint32_t ChosenReg = UINT32_MAX;
  std::array<uint8_t, 4> ChosenMap = {0, 1, 2, 3};
  uint8_t ChosenMask = 0;
  uint8_t BestFreeCount = 5;
  for (uint32_t P = 0; P < NumPhys; ++P) {
    const auto FreeArr = Busy.GetFree(P);
    uint8_t UnionFree = 0;
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if (ActiveMask & (1u << L))
        UnionFree |= FreeArr[L];
    const uint8_t FC = Popcount4(UnionFree);
    if (FC < Width || FC >= BestFreeCount)
      continue;
    std::array<uint8_t, 4> CandMap;
    uint8_t CandMask = 0;
    if (TryMatchLanes(ActiveMask, Fixed, FreeArr, NoPref, CandMap, CandMask)) {
      ChosenReg = P;
      ChosenMap = CandMap;
      ChosenMask = CandMask;
      BestFreeCount = FC;
      if (FC == Width)
        break;
    }
  }
  if (ChosenReg == UINT32_MAX) {
    if (NumPhys >= kShaderTempLimit)
      return false;
    ChosenReg = NumPhys++;
    const std::array<uint8_t, 4> AllFree = {0xfu, 0xfu, 0xfu, 0xfu};
    TryMatchLanes(ActiveMask, Fixed, AllFree, NoPref, ChosenMap, ChosenMask);
  }
  Out = {ChosenReg, ChosenMap, ChosenMask};
  return true;
}

} // namespace

bool dxcb5::AllocateLaneRegisters(llvm::ArrayRef<FlowBlock> Blocks,
                                  const LivenessResult &Live,
                                  std::vector<LaneAllocation> &Allocation,
                                  uint32_t &PhysTempCount) {
  const size_t Count = Live.Temps.size();
  Allocation.assign(Count, LaneAllocation{});
  PhysTempCount = 0;
  if (Live.ActiveTemps.empty())
    return true;

  std::vector<std::array<uint32_t, 4>> SlotOf(
      Count, {kNoSlot, kNoSlot, kNoSlot, kNoSlot});
  std::vector<uint32_t> SlotVreg;
  std::vector<uint8_t> SlotLane;
  SlotVreg.reserve(Live.ActiveTemps.size() * 2);
  SlotLane.reserve(Live.ActiveTemps.size() * 2);
  for (uint32_t R : Live.ActiveTemps) {
    const uint8_t AM = Live.Temps[R].ActiveMask;
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      if (AM & (1u << L)) {
        SlotOf[R][L] = static_cast<uint32_t>(SlotVreg.size());
        SlotVreg.push_back(R);
        SlotLane.push_back(static_cast<uint8_t>(L));
      }
    }
  }
  if (SlotVreg.size() > kMaxGraphSlots)
    return AllocateLinearFallback(Blocks, Live, Allocation, PhysTempCount);

  SlotInterferenceGraph Graph(SlotVreg.size());
  std::vector<llvm::SmallVector<AffinityLink, 4>> Affinities(Count);
  std::vector<uint64_t> TotalWeight(Count, 0);
  BuildInterferenceAndAffinities(Blocks, Live, SlotOf, SlotVreg.size(), Graph,
                                 Affinities, TotalWeight);

  uint32_t NumPhys = 0;
  std::vector<uint8_t> PreColored(Count, 0);
  if (!Blocks.empty()) {
    Live.ForEachLive(Live.Blocks[0].In, [&](uint32_t R, uint8_t) {
      Allocation[R] = {NumPhys++, {0, 1, 2, 3}, Live.Temps[R].ActiveMask};
      PreColored[R] = 1;
    });
  }

  CoalesceState State;
  State.Rep.resize(Count);
  State.LaneToRep.assign(Count, {0, 1, 2, 3});
  State.Members.resize(Count);
  State.RepFixed.assign(Count, 0);
  State.RepMask.assign(Count, 0);
  for (uint32_t R : Live.ActiveTemps) {
    State.Rep[R] = R;
    State.Members[R].push_back(R);
    State.RepFixed[R] = Live.Temps[R].FixedLanes ? 1 : 0;
    State.RepMask[R] = Live.Temps[R].ActiveMask;
  }
  PreCoalesceRegisters(Live, SlotOf, PreColored, Affinities, TotalWeight, Graph,
                       State);

  llvm::TimeTraceScope ColorTimer("DXCB5 Register Lane Coloring", llvm::StringRef());
  std::vector<uint32_t> Roots;
  Roots.reserve(Live.ActiveTemps.size());
  std::vector<unsigned> RootDegree(Count, 0);
  unsigned MaxRootDegree = 0;
  for (uint32_t R : Live.ActiveTemps) {
    if (State.Rep[R] != R || PreColored[R])
      continue;
    Roots.push_back(R);
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      RootDegree[R] += Graph.GetDegree(SlotOf[R][L]);
    MaxRootDegree = std::max(MaxRootDegree, RootDegree[R]);
  }
  const bool HighPressure = MaxRootDegree >= 200;
  std::stable_sort(Roots.begin(), Roots.end(), [&](uint32_t A, uint32_t B) {
    if (Live.Temps[A].Width != Live.Temps[B].Width)
      return Live.Temps[A].Width > Live.Temps[B].Width;
    if (HighPressure) {
      if (RootDegree[A] != RootDegree[B])
        return RootDegree[A] > RootDegree[B];
      return TotalWeight[A] > TotalWeight[B];
    }
    if (TotalWeight[A] != TotalWeight[B])
      return TotalWeight[A] > TotalWeight[B];
    return RootDegree[A] > RootDegree[B];
  });

  ColorBusyState Busy;
  for (uint32_t RA : Roots) {
    Busy.MarkNeighborBusy(RA, NumPhys, Graph, SlotOf, SlotVreg, SlotLane, State,
                          Allocation);
    LaneAllocation Chosen;
    if (!PickAffinityColor(RA, State.RepMask[RA], State.RepFixed[RA] != 0, Busy,
                           State, Affinities, Allocation, Chosen) &&
        !PickBestFitColor(State.RepMask[RA], State.RepFixed[RA] != 0,
                          Live.Temps[RA].Width, Busy, NumPhys, Chosen))
      return false;
    Allocation[RA] = Chosen;
  }

  for (uint32_t R : Live.ActiveTemps) {
    const uint32_t RA = State.Rep[R];
    if (R == RA)
      continue;
    const LaneAllocation &RootA = Allocation[RA];
    LaneAllocation MemberA{RootA.PhysReg, {}, 0};
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      const uint8_t PL = RootA.LaneMap[State.LaneToRep[R][L]];
      MemberA.LaneMap[L] = PL;
      if (Live.Temps[R].ActiveMask & (1u << L))
        MemberA.PhysMask |= static_cast<uint8_t>(1u << PL);
    }
    Allocation[R] = MemberA;
  }

  assert(VerifySlotAllocation(Graph, Live, SlotOf, SlotVreg, SlotLane, State,
                              Allocation));
  PhysTempCount = NumPhys;
  return true;
}
