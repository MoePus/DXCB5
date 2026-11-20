// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <vector>

using namespace dxcb5;

namespace {

struct ArrayAccessSummary {
  bool HasDynWrite = false;
  bool HasUnsafeUse = false;
  uint32_t TotalStores = 0;
  uint32_t StoreBlock = UINT32_MAX;
  bool MultiStoreBlock = false;
  llvm::SmallVector<uint32_t, 4> ReadBlocks;
  llvm::SmallVector<uint32_t, 4> StoreBlocks;
};

void AddUnique(llvm::SmallVectorImpl<uint32_t> &Vec, uint32_t Val) {
  if (std::find(Vec.begin(), Vec.end(), Val) == Vec.end())
    Vec.push_back(Val);
}

void CollectArraySummaries(const SsaProgram &Ssa, const SsaDomTree &Dom,
                           std::vector<ArrayAccessSummary> &Info) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  Info.assign(NumX, {});
  for (uint32_t B : Dom.RpoOrder) {
    const auto &Block = Ssa.Blocks[B];
    for (const auto &Phi : Block.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        if (In.Value.Kind == OperandKind::IndexableTemp && In.Value.Index < NumX)
          Info[In.Value.Index].HasUnsafeUse = true;
    }
    if ((Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch) &&
        Block.Condition.Kind == OperandKind::IndexableTemp &&
        Block.Condition.Index < NumX)
      Info[Block.Condition.Index].HasUnsafeUse = true;

    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead)
        continue;
      for (const auto &Src : Inst.Sources) {
        if (Src.Kind == OperandKind::IndexableTemp && Src.Index < NumX)
          AddUnique(Info[Src.Index].ReadBlocks, B);
      }
      if (Inst.Dest.Kind != OperandKind::IndexableTemp || Inst.Dest.Index >= NumX)
        continue;
      auto &A = Info[Inst.Dest.Index];
      ++A.TotalStores;
      AddUnique(A.StoreBlocks, B);
      if (A.StoreBlock == UINT32_MAX)
        A.StoreBlock = B;
      else if (A.StoreBlock != B)
        A.MultiStoreBlock = true;
      if (Inst.Dest.RelativeRegister != UINT32_MAX || Inst.Saturate ||
          Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Sources.size() != 1)
        A.HasDynWrite = true;
    }
  }
}

struct CopyMatch {
  uint32_t SrcX = UINT32_MAX;
  uint32_t CopyBlock = UINT32_MAX;
  uint32_t FirstSrcLoad = UINT32_MAX;
  uint32_t LastDstStore = 0;
};

bool MatchSingleBlockCopy(const SsaProgram &Ssa,
                          const std::vector<SsaDefSite> &Defs,
                          const ArrayAccessSummary &DstInfo, uint32_t DstX,
                          CopyMatch &Out) {
  const auto &DstDecl = Ssa.Mir.IndexableTemps[DstX];
  const uint32_t N = DstDecl.Elements;
  if (DstDecl.Components != 1 || N == 0 || DstInfo.HasDynWrite ||
      DstInfo.HasUnsafeUse || DstInfo.MultiStoreBlock ||
      DstInfo.StoreBlock == UINT32_MAX || DstInfo.TotalStores != N ||
      DstInfo.ReadBlocks.empty())
    return false;

  const uint32_t CopyB = DstInfo.StoreBlock;
  const auto &Insts = Ssa.Blocks[CopyB].Insts;
  std::vector<uint8_t> SeenElem(N, 0);
  uint32_t SrcX = UINT32_MAX;
  uint32_t FirstLoad = UINT32_MAX;
  uint32_t LastStore = 0;
  uint32_t FirstDstRead = UINT32_MAX;

  for (uint32_t I = 0; I < Insts.size(); ++I) {
    const auto &Inst = Insts[I];
    if (Inst.Dead)
      continue;
    for (const auto &S : Inst.Sources)
      if (S.Kind == OperandKind::IndexableTemp && S.Index == DstX &&
          FirstDstRead == UINT32_MAX)
        FirstDstRead = I;
    if (Inst.Dest.Kind != OperandKind::IndexableTemp || Inst.Dest.Index != DstX)
      continue;
    const uint32_t Elem = Inst.Dest.Element;
    if (Elem >= N || SeenElem[Elem] || Inst.Dest.Mask != 1)
      return false;
    const auto &Val = Inst.Sources[0];
    if (Val.Kind != OperandKind::Temp || Val.Modifier != 0 ||
        Val.RelativeRegister != UINT32_MAX || Val.Index >= Defs.size())
      return false;
    const auto &D = Defs[Val.Index];
    if (!D.Valid() || D.IsPhi || D.IsAux || D.Block != CopyB || D.Index >= I)
      return false;
    const auto &Ld = Insts[D.Index];
    if (Ld.Dead || Ld.Saturate || Ld.Opcode != D3D10_SB_OPCODE_MOV ||
        Ld.Sources.size() != 1 || PopcountMask(Ld.Dest.Mask) != 1)
      return false;
    const unsigned LdLane = static_cast<unsigned>(
        llvm::countTrailingZeros(static_cast<uint32_t>(Ld.Dest.Mask & 0xfu)));
    if (GetSwizzleLane(Val.Swizzle, 0) != LdLane)
      return false;
    const auto &LSrc = Ld.Sources[0];
    if (LSrc.Kind != OperandKind::IndexableTemp || LSrc.Modifier != 0 ||
        LSrc.RelativeRegister != UINT32_MAX || LSrc.Element != Elem ||
        LSrc.Index == DstX || LSrc.Index >= Ssa.Mir.IndexableTemps.size())
      return false;
    if (SrcX == UINT32_MAX)
      SrcX = LSrc.Index;
    else if (SrcX != LSrc.Index)
      return false;
    SeenElem[Elem] = 1;
    FirstLoad = std::min(FirstLoad, D.Index);
    LastStore = std::max(LastStore, I);
  }
  if (SrcX == UINT32_MAX || FirstLoad >= LastStore || FirstDstRead <= LastStore)
    return false;
  const auto &SrcDecl = Ssa.Mir.IndexableTemps[SrcX];
  if (SrcDecl.Elements != N || SrcDecl.Components != 1)
    return false;
  for (uint32_t I = FirstLoad + 1; I < LastStore; ++I) {
    const auto &Inst = Insts[I];
    if (!Inst.Dead && Inst.Dest.Kind == OperandKind::IndexableTemp &&
        Inst.Dest.Index == SrcX)
      return false;
  }
  Out = {SrcX, CopyB, FirstLoad, LastStore};
  return true;
}

bool FindLastReadOfArray(const std::vector<SsaInst> &Insts, uint32_t Start,
                         uint32_t ArrayX, uint32_t &LastRead) {
  bool Found = false;
  for (uint32_t I = Start; I < Insts.size(); ++I) {
    if (Insts[I].Dead)
      continue;
    for (const auto &Op : Insts[I].Sources) {
      if (Op.Kind == OperandKind::IndexableTemp && Op.Index == ArrayX) {
        LastRead = I;
        Found = true;
      }
    }
  }
  return Found;
}

bool HasStoreToArrayInRange(const std::vector<SsaInst> &Insts, uint32_t Start,
                            uint32_t Limit, uint32_t ArrayX) {
  for (uint32_t I = Start; I < Limit; ++I) {
    if (!Insts[I].Dead && Insts[I].Dest.Kind == OperandKind::IndexableTemp &&
        Insts[I].Dest.Index == ArrayX)
      return true;
  }
  return false;
}

bool SrcUnmodifiedWhileDstLive(
    const SsaProgram &Ssa, const SsaDomTree &Dom,
    const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds,
    const ArrayAccessSummary &DstInfo, const ArrayAccessSummary &SrcInfo,
    uint32_t DstX, const CopyMatch &M) {
  for (uint32_t RB : DstInfo.ReadBlocks)
    if (!Dom.Dominates(M.CopyBlock, RB))
      return false;

  std::vector<uint8_t> CanReachRead(Ssa.Blocks.size(), 0);
  llvm::SmallVector<uint32_t, 32> Work;
  for (uint32_t RB : DstInfo.ReadBlocks) {
    if (RB != M.CopyBlock && !CanReachRead[RB]) {
      CanReachRead[RB] = 1;
      Work.push_back(RB);
    }
  }
  while (!Work.empty()) {
    const uint32_t Cur = Work.pop_back_val();
    for (uint32_t P : Preds[Cur]) {
      if (P == M.CopyBlock || !Dom.Reachable(P) || CanReachRead[P])
        continue;
      CanReachRead[P] = 1;
      Work.push_back(P);
    }
  }

  for (uint32_t SB : SrcInfo.StoreBlocks) {
    if (SB != M.CopyBlock && !CanReachRead[SB])
      continue;
    bool SuccReaches = false;
    for (uint32_t S : Ssa.Blocks[SB].Successors)
      if (S != M.CopyBlock && S < CanReachRead.size() && CanReachRead[S])
        SuccReaches = true;
    const auto &Insts = Ssa.Blocks[SB].Insts;
    const uint32_t Start = (SB == M.CopyBlock) ? (M.LastDstStore + 1) : 0u;
    uint32_t Limit = static_cast<uint32_t>(Insts.size());
    if (!SuccReaches) {
      uint32_t LastRead = 0;
      Limit = FindLastReadOfArray(Insts, Start, DstX, LastRead) ? LastRead : 0u;
    }
    if (HasStoreToArrayInRange(Insts, Start, Limit, M.SrcX))
      return false;
  }
  return true;
}

void RewriteCopyReadsAndKillStores(SsaProgram &Ssa, const SsaDomTree &Dom,
                                   uint32_t DstX, uint32_t SrcX,
                                   uint32_t CopyBlock) {
  for (uint32_t B : Dom.RpoOrder) {
    for (auto &Inst : Ssa.Blocks[B].Insts) {
      if (Inst.Dead)
        continue;
      for (auto &Op : Inst.Sources)
        if (Op.Kind == OperandKind::IndexableTemp && Op.Index == DstX)
          Op.Index = SrcX;
      if (B == CopyBlock && Inst.Dest.Kind == OperandKind::IndexableTemp &&
          Inst.Dest.Index == DstX)
        Inst.Dead = true;
    }
  }
}

} // namespace

bool dxcb5::RunOptIndexableCopy(SsaProgram &Ssa, const SsaDomTree &Dom) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  if (NumX < 2 || Ssa.Blocks.empty())
    return false;
  llvm::TimeTraceScope Scope("DXCB5 Opt Indexable Copy", llvm::StringRef());

  std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
  Ssa.BuildPredecessors(Preds);
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);

  bool AnyChanged = false;
  for (unsigned Round = 0; Round < 4; ++Round) {
    std::vector<ArrayAccessSummary> Info;
    CollectArraySummaries(Ssa, Dom, Info);
    bool RoundChanged = false;
    for (uint32_t DstX = 0; DstX < NumX; ++DstX) {
      CopyMatch Match;
      if (!MatchSingleBlockCopy(Ssa, Defs, Info[DstX], DstX, Match))
        continue;
      if (!SrcUnmodifiedWhileDstLive(Ssa, Dom, Preds, Info[DstX],
                                     Info[Match.SrcX], DstX, Match))
        continue;
      RewriteCopyReadsAndKillStores(Ssa, Dom, DstX, Match.SrcX, Match.CopyBlock);
      for (uint32_t RB : Info[DstX].ReadBlocks)
        AddUnique(Info[Match.SrcX].ReadBlocks, RB);
      Info[DstX] = {};
      RoundChanged = true;
      AnyChanged = true;
    }
    if (!RoundChanged)
      break;
  }
  if (AnyChanged)
    Ssa.CompactDead();
  return AnyChanged;
}
