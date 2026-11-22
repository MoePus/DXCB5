// Distributed under the University of Illinois Open Source License.
#include "Mir/Memory/IndexableSsa.h"
#include "Mir/Memory/MemoryOpt.h"
#include "Mir/Analysis/LaneAnalysis.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <utility>

using namespace dxcb5;
using dxcb5::idxssa::IsSsaSlotValue;
using dxcb5::idxssa::NormalizeScalar;

namespace {

constexpr uint32_t kMaxTrackedSlots = 4096;

uint64_t SlotKey(uint32_t XId, uint32_t Elem) {
  return (uint64_t(XId) << 32) | uint64_t(Elem);
}

template <typename MapT> void ClearArrayKeys(MapT &Map, uint32_t XId) {
  llvm::SmallVector<uint64_t, 16> EraseKeys;
  for (const auto &KV : Map)
    if ((KV.first >> 32) == XId)
      EraseKeys.push_back(KV.first);
  for (uint64_t K : EraseKeys)
    Map.erase(K);
}

bool RunIntraBlockBlock(SsaProgram &Ssa, SsaBlock &Block,
                        std::vector<uint8_t> &HasDynWrite) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  llvm::DenseMap<uint64_t, Operand> Known;
  llvm::DenseMap<uint64_t, uint32_t> LastStore;
  bool Changed = false;
  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    auto &Inst = Block.Insts[I];
    if (Inst.Dead)
      continue;
    for (auto &Src : Inst.Sources) {
      if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX ||
          Ssa.Mir.IndexableTemps[Src.Index].Components != 1)
        continue;
      if (Src.RelativeRegister != UINT32_MAX) {
        ClearArrayKeys(LastStore, Src.Index);
        continue;
      }
      const uint64_t K = SlotKey(Src.Index, Src.Element);
      auto It = Known.find(K);
      if (It != Known.end() && !Src.Modifier) {
        Src = It->second;
        Changed = true;
      } else {
        LastStore.erase(K);
      }
    }
    if (Inst.Dest.Kind != OperandKind::IndexableTemp ||
        Inst.Dest.Index >= NumX ||
        Ssa.Mir.IndexableTemps[Inst.Dest.Index].Components != 1)
      continue;
    const uint32_t XId = Inst.Dest.Index;
    const uint32_t ElemCount = Ssa.Mir.IndexableTemps[XId].Elements;
    if (Inst.Dest.RelativeRegister != UINT32_MAX ||
        Inst.Dest.Element >= ElemCount || Inst.Opcode != D3D10_SB_OPCODE_MOV ||
        Inst.Sources.size() != 1) {
      HasDynWrite[XId] = 1;
      ClearArrayKeys(Known, XId);
      ClearArrayKeys(LastStore, XId);
      continue;
    }
    const uint64_t K = SlotKey(XId, Inst.Dest.Element);
    if (!Inst.Saturate && IsSsaSlotValue(Inst.Sources[0])) {
      Operand Norm = NormalizeScalar(Inst.Sources[0]);
      auto KnownIt = Known.find(K);
      if (KnownIt != Known.end() && KnownIt->second.SameValue(Norm)) {
        Inst.Dead = true;
        Changed = true;
        continue;
      }
      auto Prev = LastStore.find(K);
      if (Prev != LastStore.end()) {
        Block.Insts[Prev->second].Dead = true;
        Changed = true;
      }
      Known[K] = Norm;
    } else {
      auto Prev = LastStore.find(K);
      if (Prev != LastStore.end()) {
        Block.Insts[Prev->second].Dead = true;
        Changed = true;
      }
      Known.erase(K);
    }
    LastStore[K] = I;
  }
  return Changed;
}

bool RunIntraBlockPass(SsaProgram &Ssa, std::vector<uint8_t> &HasDynWrite) {
  bool Changed = false;
  for (auto &Block : Ssa.Blocks)
    Changed |= RunIntraBlockBlock(Ssa, Block, HasDynWrite);
  return Changed;
}

using StateList = std::vector<std::pair<uint32_t, Operand>>;

void IntersectStateLists(StateList &Cur, const StateList &Other) {
  StateList Next;
  size_t IA = 0, IB = 0;
  while (IA < Cur.size() && IB < Other.size()) {
    if (Cur[IA].first < Other[IB].first) {
      ++IA;
    } else if (Other[IB].first < Cur[IA].first) {
      ++IB;
    } else {
      if (Cur[IA].second.SameValue(Other[IB].second))
        Next.push_back(Cur[IA]);
      ++IA;
      ++IB;
    }
  }
  Cur.swap(Next);
}

struct ForwardContext {
  const SsaProgram &Ssa;
  const std::vector<uint32_t> &SlotBase;
  const std::vector<llvm::SmallVector<uint32_t, 2>> &Preds;
  std::vector<StateList> OutState;
  std::vector<uint8_t> Visited;
  std::vector<Operand> Avail;
  std::vector<uint8_t> HasAvail;
  std::vector<uint32_t> DirtySlots;

  ForwardContext(const SsaProgram &Ssa, const std::vector<uint32_t> &SlotBase,
                 const std::vector<llvm::SmallVector<uint32_t, 2>> &Preds,
                 uint32_t TotalSlots)
      : Ssa(Ssa), SlotBase(SlotBase), Preds(Preds),
        OutState(Ssa.Blocks.size()), Visited(Ssa.Blocks.size(), 0),
        Avail(TotalSlots, Operand{OperandKind::Null}),
        HasAvail(TotalSlots, 0) {
    DirtySlots.reserve(TotalSlots);
  }

  void LoadMeet(uint32_t B) {
    for (uint32_t S : DirtySlots)
      HasAvail[S] = 0;
    DirtySlots.clear();
    if (B == 0)
      return;
    uint32_t FirstPred = UINT32_MAX;
    for (uint32_t P : Preds[B]) {
      if (Visited[P]) {
        FirstPred = P;
        break;
      }
    }
    if (FirstPred == UINT32_MAX)
      return;
    StateList Cur = OutState[FirstPred];
    for (uint32_t P : Preds[B]) {
      if (P == FirstPred || !Visited[P])
        continue;
      if (Cur.empty())
        break;
      IntersectStateLists(Cur, OutState[P]);
    }
    for (const auto &KV : Cur) {
      Avail[KV.first] = KV.second;
      HasAvail[KV.first] = 1;
      DirtySlots.push_back(KV.first);
    }
  }

  bool UpdateBlockOut(uint32_t B) {
    std::sort(DirtySlots.begin(), DirtySlots.end());
    DirtySlots.erase(std::unique(DirtySlots.begin(), DirtySlots.end()),
                     DirtySlots.end());
    StateList NewOut;
    NewOut.reserve(DirtySlots.size());
    for (uint32_t S : DirtySlots)
      if (HasAvail[S])
        NewOut.emplace_back(S, Avail[S]);
    bool Same = Visited[B] && NewOut.size() == OutState[B].size();
    for (size_t K = 0; Same && K < NewOut.size(); ++K) {
      if (NewOut[K].first != OutState[B][K].first ||
          !NewOut[K].second.SameValue(OutState[B][K].second))
        Same = false;
    }
    if (Same)
      return false;
    OutState[B].swap(NewOut);
    Visited[B] = 1;
    return true;
  }

  void TransferStore(const SsaInst &Inst) {
    const size_t NumX = Ssa.Mir.IndexableTemps.size();
    if (Inst.Dead || Inst.Dest.Kind != OperandKind::IndexableTemp ||
        Inst.Dest.Index >= NumX)
      return;
    const uint32_t Base = SlotBase[Inst.Dest.Index];
    if (Base == UINT32_MAX)
      return;
    const uint32_t Slot = Base + Inst.Dest.Element;
    if (!Inst.Saturate && Inst.Opcode == D3D10_SB_OPCODE_MOV &&
        Inst.Sources.size() == 1 && IsSsaSlotValue(Inst.Sources[0])) {
      if (!HasAvail[Slot])
        DirtySlots.push_back(Slot);
      Avail[Slot] = NormalizeScalar(Inst.Sources[0]);
      HasAvail[Slot] = 1;
    } else {
      HasAvail[Slot] = 0;
    }
  }
};

bool ValueDominatesSite(const Operand &V, uint32_t BlockId, uint32_t InstIdx,
                        const std::vector<SsaDefSite> &Defs,
                        const SsaDomTree &Dom) {
  if (V.Kind != OperandKind::Temp)
    return true;
  if (V.Index >= Defs.size() || !Defs[V.Index].Valid())
    return false;
  const auto &DS = Defs[V.Index];
  return DS.Block == BlockId ? (DS.IsPhi || DS.Index < InstIdx)
                             : Dom.Dominates(DS.Block, BlockId);
}

bool RewriteBlockForward(SsaProgram &Ssa, uint32_t B, ForwardContext &Ctx,
                         const std::vector<SsaDefSite> &Defs,
                         const SsaDomTree &Dom) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  bool Changed = false;
  Ctx.LoadMeet(B);
  auto &Block = Ssa.Blocks[B];
  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    auto &Inst = Block.Insts[I];
    if (Inst.Dead)
      continue;
    for (auto &Src : Inst.Sources) {
      if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX ||
          Src.RelativeRegister != UINT32_MAX || Src.Modifier)
        continue;
      const uint32_t Base = Ctx.SlotBase[Src.Index];
      if (Base == UINT32_MAX ||
          Src.Element >= Ssa.Mir.IndexableTemps[Src.Index].Elements)
        continue;
      const uint32_t Slot = Base + Src.Element;
      if (Ctx.HasAvail[Slot] &&
          ValueDominatesSite(Ctx.Avail[Slot], B, I, Defs, Dom)) {
        Src = Ctx.Avail[Slot];
        Changed = true;
      }
    }
    if (Inst.Dest.Kind != OperandKind::IndexableTemp ||
        Inst.Dest.Index >= NumX ||
        Ssa.Mir.IndexableTemps[Inst.Dest.Index].Components != 1)
      continue;
    const uint32_t Base = Ctx.SlotBase[Inst.Dest.Index];
    if (Base == UINT32_MAX)
      continue;
    const uint32_t Slot = Base + Inst.Dest.Element;
    if (!Inst.Saturate && Inst.Opcode == D3D10_SB_OPCODE_MOV &&
        Inst.Sources.size() == 1 && IsSsaSlotValue(Inst.Sources[0])) {
      Operand Norm = NormalizeScalar(Inst.Sources[0]);
      if (Ctx.HasAvail[Slot] && Ctx.Avail[Slot].SameValue(Norm)) {
        Inst.Dead = true;
        Changed = true;
        continue;
      }
      if (!Ctx.HasAvail[Slot])
        Ctx.DirtySlots.push_back(Slot);
      Ctx.Avail[Slot] = Norm;
      Ctx.HasAvail[Slot] = 1;
    } else {
      Ctx.HasAvail[Slot] = 0;
    }
  }
  return Changed;
}

bool RunGlobalForwardPass(SsaProgram &Ssa, const SsaDomTree &Dom,
                          const std::vector<uint32_t> &SlotBase,
                          uint32_t TotalSlots) {
  const uint32_t NumBlocks = static_cast<uint32_t>(Ssa.Blocks.size());
  std::vector<llvm::SmallVector<uint32_t, 2>> Preds(NumBlocks);
  for (uint32_t B : Dom.RpoOrder) {
    for (uint32_t S : Ssa.Blocks[B].Successors)
      if (S < NumBlocks && Dom.Reachable(S))
        Preds[S].push_back(B);
  }

  ForwardContext Ctx(Ssa, SlotBase, Preds, TotalSlots);
  bool Converged = false;
  for (unsigned Iter = 0; Iter < 10; ++Iter) {
    bool IterChanged = false;
    for (uint32_t B : Dom.RpoOrder) {
      Ctx.LoadMeet(B);
      for (const auto &Inst : Ssa.Blocks[B].Insts)
        Ctx.TransferStore(Inst);
      IterChanged |= Ctx.UpdateBlockOut(B);
    }
    if (!IterChanged) {
      Converged = true;
      break;
    }
  }
  if (!Converged)
    return false;

  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  bool Changed = false;
  for (uint32_t B : Dom.RpoOrder)
    Changed |= RewriteBlockForward(Ssa, B, Ctx, Defs, Dom);
  return Changed;
}

void MarkLiveSources(const SsaProgram &Ssa, const SsaInst &Inst,
                     const std::vector<uint32_t> &SlotBase,
                     std::vector<uint64_t> &Live) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  for (const auto &Src : Inst.Sources) {
    if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX)
      continue;
    const uint32_t Base = SlotBase[Src.Index];
    if (Base == UINT32_MAX)
      continue;
    const uint32_t ElemCount = Ssa.Mir.IndexableTemps[Src.Index].Elements;
    if (Src.RelativeRegister == UINT32_MAX && Src.Element < ElemCount) {
      const uint32_t Slot = Base + Src.Element;
      Live[Slot >> 6] |= (uint64_t{1} << (Slot & 63u));
      continue;
    }
    for (uint32_t E = 0; E < ElemCount; ++E) {
      const uint32_t Slot = Base + E;
      Live[Slot >> 6] |= (uint64_t{1} << (Slot & 63u));
    }
  }
}

void UnionSuccLiveIn(const SsaProgram &Ssa, uint32_t B, const SsaDomTree &Dom,
                     const std::vector<std::vector<uint64_t>> &LiveIn,
                     std::vector<uint64_t> &Live) {
  std::fill(Live.begin(), Live.end(), uint64_t{0});
  const uint32_t NumBlocks = static_cast<uint32_t>(Ssa.Blocks.size());
  for (uint32_t S : Ssa.Blocks[B].Successors) {
    if (S >= NumBlocks || !Dom.Reachable(S))
      continue;
    for (size_t W = 0; W < Live.size(); ++W)
      Live[W] |= LiveIn[S][W];
  }
}

void ClearLiveBit(std::vector<uint64_t> &Live, uint32_t Slot) {
  Live[Slot >> 6] &= ~(static_cast<uint64_t>(1) << (Slot & 63u));
}

bool TestLiveBit(const std::vector<uint64_t> &Live, uint32_t Slot) {
  return (Live[Slot >> 6] & (static_cast<uint64_t>(1) << (Slot & 63u))) != 0;
}

bool UpdateBlockLiveIn(const SsaProgram &Ssa, uint32_t B, const SsaDomTree &Dom,
                       const std::vector<uint32_t> &SlotBase,
                       std::vector<std::vector<uint64_t>> &LiveIn,
                       std::vector<uint64_t> &Live) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  UnionSuccLiveIn(Ssa, B, Dom, LiveIn, Live);
  const auto &Insts = Ssa.Blocks[B].Insts;
  for (size_t I = Insts.size(); I-- > 0;) {
    const auto &Inst = Insts[I];
    if (Inst.Dead)
      continue;
    if (Inst.Dest.Kind == OperandKind::IndexableTemp &&
        Inst.Dest.Index < NumX && SlotBase[Inst.Dest.Index] != UINT32_MAX)
      ClearLiveBit(Live, SlotBase[Inst.Dest.Index] + Inst.Dest.Element);
    MarkLiveSources(Ssa, Inst, SlotBase, Live);
  }
  if (Live == LiveIn[B])
    return false;
  LiveIn[B] = Live;
  return true;
}

bool SweepBlockDeadStores(SsaProgram &Ssa, uint32_t B, const SsaDomTree &Dom,
                          const std::vector<uint32_t> &SlotBase,
                          const std::vector<std::vector<uint64_t>> &LiveIn,
                          std::vector<uint64_t> &Live) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  bool Changed = false;
  UnionSuccLiveIn(Ssa, B, Dom, LiveIn, Live);
  auto &Insts = Ssa.Blocks[B].Insts;
  for (size_t I = Insts.size(); I-- > 0;) {
    auto &Inst = Insts[I];
    if (Inst.Dead)
      continue;
    if (Inst.Dest.Kind == OperandKind::IndexableTemp &&
        Inst.Dest.Index < NumX && SlotBase[Inst.Dest.Index] != UINT32_MAX) {
      const uint32_t Slot = SlotBase[Inst.Dest.Index] + Inst.Dest.Element;
      if (!TestLiveBit(Live, Slot)) {
        Inst.Dead = true;
        Changed = true;
        continue;
      }
      ClearLiveBit(Live, Slot);
    }
    MarkLiveSources(Ssa, Inst, SlotBase, Live);
  }
  return Changed;
}

bool RunGlobalDeadStorePass(SsaProgram &Ssa, const SsaDomTree &Dom,
                            const std::vector<uint32_t> &SlotBase,
                            uint32_t TotalSlots) {
  const size_t Words = (TotalSlots + 63u) / 64u;
  std::vector<uint32_t> PostOrder(Dom.RpoOrder.rbegin(), Dom.RpoOrder.rend());
  std::vector<std::vector<uint64_t>> LiveIn(Ssa.Blocks.size(),
                                            std::vector<uint64_t>(Words, 0));
  std::vector<uint64_t> Live(Words, 0);

  bool Converged = false;
  for (unsigned Iter = 0; Iter < 16; ++Iter) {
    bool IterChanged = false;
    for (uint32_t B : PostOrder)
      IterChanged |= UpdateBlockLiveIn(Ssa, B, Dom, SlotBase, LiveIn, Live);
    if (!IterChanged) {
      Converged = true;
      break;
    }
  }
  if (!Converged)
    return false;

  bool Changed = false;
  for (uint32_t B : PostOrder)
    Changed |= SweepBlockDeadStores(Ssa, B, Dom, SlotBase, LiveIn, Live);
  return Changed;
}

bool PruneAndCompactArrays(SsaProgram &Ssa) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  bool Changed = false;
  std::vector<uint8_t> AnyRead(NumX, 0);
  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead)
        continue;
      for (const auto &Src : Inst.Sources)
        if (Src.Kind == OperandKind::IndexableTemp && Src.Index < NumX)
          AnyRead[Src.Index] = 1;
    }
  }
  for (auto &Block : Ssa.Blocks) {
    for (auto &Inst : Block.Insts) {
      if (!Inst.Dead && Inst.Dest.Kind == OperandKind::IndexableTemp &&
          Inst.Dest.Index < NumX && !AnyRead[Inst.Dest.Index]) {
        Inst.Dead = true;
        Changed = true;
      }
    }
  }
  if (Changed)
    Ssa.CompactDead();

  std::vector<uint8_t> UsedX(NumX, 0);
  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dest.Kind == OperandKind::IndexableTemp && Inst.Dest.Index < NumX)
        UsedX[Inst.Dest.Index] = 1;
      for (const auto &Src : Inst.Sources)
        if (Src.Kind == OperandKind::IndexableTemp && Src.Index < NumX)
          UsedX[Src.Index] = 1;
    }
  }
  std::vector<uint32_t> Remap(NumX, UINT32_MAX);
  std::vector<IndexableTempDecl> Kept;
  for (size_t X = 0; X < NumX; ++X) {
    if (!UsedX[X])
      continue;
    Remap[X] = static_cast<uint32_t>(Kept.size());
    IndexableTempDecl Decl = Ssa.Mir.IndexableTemps[X];
    Decl.Index = Remap[X];
    Kept.push_back(Decl);
  }
  if (Kept.size() == NumX)
    return Changed;

  for (auto &Block : Ssa.Blocks) {
    for (auto &Inst : Block.Insts) {
      if (Inst.Dest.Kind == OperandKind::IndexableTemp && Inst.Dest.Index < NumX)
        Inst.Dest.Index = Remap[Inst.Dest.Index];
      for (auto &Src : Inst.Sources)
        if (Src.Kind == OperandKind::IndexableTemp && Src.Index < NumX)
          Src.Index = Remap[Src.Index];
    }
  }
  Ssa.Mir.IndexableTemps = std::move(Kept);
#ifndef NDEBUG
  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.Kind == OperandKind::IndexableTemp) {
        assert(Inst.Dest.Index < Ssa.Mir.IndexableTemps.size());
        assert(Inst.Dest.RelativeRegister != UINT32_MAX ||
               Inst.Dest.Element < Ssa.Mir.IndexableTemps[Inst.Dest.Index].Elements);
      }
      for (const auto &Src : Inst.Sources) {
        if (Src.Kind == OperandKind::IndexableTemp) {
          assert(Src.Index < Ssa.Mir.IndexableTemps.size());
          assert(Src.RelativeRegister != UINT32_MAX ||
                 Src.Element < Ssa.Mir.IndexableTemps[Src.Index].Elements);
        }
      }
    }
  }
#endif
  return true;
}

bool ShrinkArraysByReadBounds(SsaProgram &Ssa, const SsaDomTree &Dom) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  std::vector<uint32_t> MaxElem;
  ComputeIndexableMaxReadElements(Ssa, Dom, MaxElem);
  std::vector<uint8_t> AnyDynWrite(NumX, 0);
  for (const auto &Blk : Ssa.Blocks) {
    for (const auto &Inst : Blk.Insts) {
      if (!Inst.Dead && Inst.Dest.Kind == OperandKind::IndexableTemp &&
          Inst.Dest.Index < NumX && Inst.Dest.RelativeRegister != UINT32_MAX)
        AnyDynWrite[Inst.Dest.Index] = 1;
    }
  }
  bool Changed = false;
  for (auto &Blk : Ssa.Blocks) {
    for (auto &Inst : Blk.Insts) {
      if (Inst.Dead || Inst.Dest.Kind != OperandKind::IndexableTemp ||
          Inst.Dest.Index >= NumX || Inst.Dest.RelativeRegister != UINT32_MAX)
        continue;
      const uint32_t MaxR = MaxElem[Inst.Dest.Index];
      if (MaxR == UINT32_MAX || Inst.Dest.Element > MaxR) {
        Inst.Dead = true;
        Changed = true;
      }
    }
  }
  for (size_t X = 0; X < NumX; ++X) {
    if (AnyDynWrite[X] || MaxElem[X] == UINT32_MAX)
      continue;
    const uint32_t NewElems = MaxElem[X] + 1u;
    if (NewElems < Ssa.Mir.IndexableTemps[X].Elements) {
      Ssa.Mir.IndexableTemps[X].Elements = NewElems;
      Changed = true;
    }
  }
  return Changed;
}

} // namespace

bool dxcb5::RunOptIndexableTemps(SsaProgram &Ssa) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  if (NumX == 0)
    return false;
  std::vector<uint8_t> HasDynWrite(NumX, 0);
  bool Changed = RunIntraBlockPass(Ssa, HasDynWrite);

  std::vector<uint32_t> SlotBase(NumX, UINT32_MAX);
  uint32_t TotalSlots = 0;
  for (size_t X = 0; X < NumX; ++X) {
    if (HasDynWrite[X] || Ssa.Mir.IndexableTemps[X].Components != 1)
      continue;
    const uint32_t N = Ssa.Mir.IndexableTemps[X].Elements;
    if (N == 0 || N > kMaxTrackedSlots - TotalSlots)
      continue;
    SlotBase[X] = TotalSlots;
    TotalSlots += N;
  }

  if (!Ssa.Blocks.empty()) {
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    if (TotalSlots > 0) {
      Changed |= RunGlobalForwardPass(Ssa, Dom, SlotBase, TotalSlots);
      Changed |= RunGlobalDeadStorePass(Ssa, Dom, SlotBase, TotalSlots);
    }
    Changed |= ShrinkArraysByReadBounds(Ssa, Dom);
  }
  if (Changed)
    Ssa.CompactDead();
  Changed |= PruneAndCompactArrays(Ssa);
  return Changed;
}
