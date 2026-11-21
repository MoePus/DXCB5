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

bool ResolveInvariantInitVal(const SsaProgram &Ssa,
                             const std::vector<SsaDefSite> &Defs, Operand Op,
                             Operand &Out) {
  for (unsigned Hop = 0; Hop < 4; ++Hop) {
    if (Op.RelativeRegister != UINT32_MAX)
      return false;
    if (Op.Kind == OperandKind::Immediate) {
      Op.Mask = 1;
      Op.Swizzle = 0;
      Out = Op;
      return true;
    }
    if (Op.Kind == OperandKind::ImmediateVector) {
      if (Op.Modifier != 0 || Op.Index >= Ssa.Mir.LiteralVectors.size())
        return false;
      const uint32_t Bits =
          Ssa.Mir.LiteralVectors[Op.Index][GetSwizzleLane(Op.Swizzle, 0)];
      Out = Operand{OperandKind::Immediate, Bits, 0, 1, 0};
      return true;
    }
    if (Op.Kind == OperandKind::ConstantBuffer ||
        Op.Kind == OperandKind::Input ||
        Op.Kind == OperandKind::ImmediateConstantBuffer) {
      Op.Mask = 1;
      Op.Swizzle = static_cast<uint8_t>(GetSwizzleLane(Op.Swizzle, 0) * 0x55u);
      Out = Op;
      return true;
    }
    if (Op.Kind != OperandKind::Temp || Op.Modifier != 0 ||
        Op.Index >= Defs.size())
      return false;
    const auto &D = Defs[Op.Index];
    if (!D.Valid() || D.IsPhi || D.IsAux)
      return false;
    const auto &DefI = Ssa.Blocks[D.Block].Insts[D.Index];
    const unsigned Lane = GetSwizzleLane(Op.Swizzle, 0);
    if (DefI.Dead || DefI.Saturate || DefI.Opcode != D3D10_SB_OPCODE_MOV ||
        DefI.Sources.size() != 1 || !(DefI.Dest.Mask & (1u << Lane)))
      return false;
    const Operand &Src0 = DefI.Sources[0];
    if (Src0.Kind == OperandKind::Immediate) {
      Op = Src0;
    } else {
      const uint8_t NextLane = GetSwizzleLane(Src0.Swizzle, Lane);
      Op = Src0;
      Op.Swizzle = static_cast<uint8_t>(NextLane * 0x55u);
    }
  }
  return false;
}

struct WriteOnceInit {
  uint32_t X = UINT32_MAX;
  uint32_t StoreBlock = UINT32_MAX;
  uint32_t LastStore = 0;
  std::vector<Operand> Values;
};

bool CollectWriteOnceInit(const SsaProgram &Ssa, const SsaDomTree &Dom,
                          const std::vector<SsaDefSite> &Defs,
                          const ArrayAccessSummary &AInfo, uint32_t X,
                          WriteOnceInit &Out) {
  const auto &Decl = Ssa.Mir.IndexableTemps[X];
  const uint32_t N = Decl.Elements;
  if (Decl.Components != 1 || N == 0 || N > 64 || AInfo.HasDynWrite ||
      AInfo.HasUnsafeUse || AInfo.MultiStoreBlock ||
      AInfo.StoreBlock == UINT32_MAX || AInfo.TotalStores != N ||
      AInfo.ReadBlocks.empty())
    return false;
  const uint32_t B = AInfo.StoreBlock;
  for (uint32_t RB : AInfo.ReadBlocks)
    if (!Dom.Dominates(B, RB))
      return false;

  const auto &Insts = Ssa.Blocks[B].Insts;
  std::vector<uint8_t> Seen(N, 0);
  std::vector<Operand> Vals(N);
  uint32_t LastStore = 0;
  uint32_t FirstRead = UINT32_MAX;
  for (uint32_t I = 0; I < Insts.size(); ++I) {
    const auto &Inst = Insts[I];
    if (Inst.Dead)
      continue;
    for (const auto &S : Inst.Sources)
      if (S.Kind == OperandKind::IndexableTemp && S.Index == X &&
          FirstRead == UINT32_MAX)
        FirstRead = I;
    if (Inst.Dest.Kind != OperandKind::IndexableTemp || Inst.Dest.Index != X)
      continue;
    const uint32_t Elem = Inst.Dest.Element;
    if (Elem >= N || Seen[Elem] || Inst.Dest.Mask != 1)
      return false;
    Operand Resolved;
    if (!ResolveInvariantInitVal(Ssa, Defs, Inst.Sources[0], Resolved))
      return false;
    Seen[Elem] = 1;
    Vals[Elem] = Resolved;
    LastStore = I;
  }
  if (FirstRead <= LastStore)
    return false;
  Out = {X, B, LastStore, std::move(Vals)};
  return true;
}

uint32_t FindFirstReadInBlock(const SsaBlock &Block, uint32_t ArrayX) {
  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    if (Block.Insts[I].Dead)
      continue;
    for (const auto &S : Block.Insts[I].Sources)
      if (S.Kind == OperandKind::IndexableTemp && S.Index == ArrayX)
        return I;
  }
  return UINT32_MAX;
}

bool SameWriteOnceValues(const std::vector<Operand> &A,
                         const std::vector<Operand> &B) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I < A.size(); ++I)
    if (!A[I].SameValue(B[I]))
      return false;
  return true;
}

bool CanCanonDominateDupReads(const SsaProgram &Ssa, const SsaDomTree &Dom,
                              const ArrayAccessSummary &DupInfo,
                              const WriteOnceInit &Canon,
                              const WriteOnceInit &Dup) {
  for (uint32_t RB : DupInfo.ReadBlocks) {
    if (!Dom.Dominates(Canon.StoreBlock, RB))
      return false;
    if (RB == Canon.StoreBlock &&
        FindFirstReadInBlock(Ssa.Blocks[RB], Dup.X) <= Canon.LastStore)
      return false;
  }
  return true;
}

bool DedupWriteOnceArrays(SsaProgram &Ssa, const SsaDomTree &Dom,
                          const std::vector<SsaDefSite> &Defs,
                          std::vector<ArrayAccessSummary> &Info) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  std::vector<uint32_t> RpoPos(Ssa.Blocks.size(), UINT32_MAX);
  for (uint32_t Pos = 0; Pos < Dom.RpoOrder.size(); ++Pos)
    RpoPos[Dom.RpoOrder[Pos]] = Pos;

  std::vector<WriteOnceInit> Inits;
  for (uint32_t X = 0; X < NumX; ++X) {
    WriteOnceInit W;
    if (CollectWriteOnceInit(Ssa, Dom, Defs, Info[X], X, W))
      Inits.push_back(std::move(W));
  }
  if (Inits.size() < 2)
    return false;

  std::sort(Inits.begin(), Inits.end(),
            [&](const WriteOnceInit &A, const WriteOnceInit &B) {
              if (RpoPos[A.StoreBlock] != RpoPos[B.StoreBlock])
                return RpoPos[A.StoreBlock] < RpoPos[B.StoreBlock];
              return A.LastStore < B.LastStore;
            });

  bool Changed = false;
  std::vector<uint8_t> Eliminated(Inits.size(), 0);
  for (size_t J = 1; J < Inits.size(); ++J) {
    const auto &Dup = Inits[J];
    for (size_t I = 0; I < J; ++I) {
      if (Eliminated[I])
        continue;
      const auto &Canon = Inits[I];
      if (!SameWriteOnceValues(Canon.Values, Dup.Values) ||
          !CanCanonDominateDupReads(Ssa, Dom, Info[Dup.X], Canon, Dup))
        continue;
      RewriteCopyReadsAndKillStores(Ssa, Dom, Dup.X, Canon.X, Dup.StoreBlock);
      for (uint32_t RB : Info[Dup.X].ReadBlocks)
        AddUnique(Info[Canon.X].ReadBlocks, RB);
      Info[Dup.X] = {};
      Eliminated[J] = 1;
      Changed = true;
      break;
    }
  }
  return Changed;
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
    bool RoundChanged = DedupWriteOnceArrays(Ssa, Dom, Defs, Info);
    AnyChanged |= RoundChanged;
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
