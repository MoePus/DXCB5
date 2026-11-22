// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "Mir/PlacementStore.h"
#include "Mir/SsaAnalysis.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <vector>

using namespace dxcb5;

namespace {

bool SameOrOuterLoops(const SsaLoopInfo &Loops, uint32_t SrcBlk,
                      uint32_t DstBlk) {
  for (const auto &L : Loops.Loops) {
    if (DstBlk < L.InLoop.size() && L.InLoop[DstBlk] &&
        (SrcBlk >= L.InLoop.size() || !L.InLoop[SrcBlk]))
      return false;
  }
  return true;
}

bool IsSinkableArrayStore(const SsaInst &Inst) {
  return !Inst.HasAuxDest() &&
         Inst.Dest.Kind == OperandKind::IndexableTemp &&
         (Inst.Opcode == D3D10_SB_OPCODE_MOV ||
          InstructionIsComponentwise(Inst.Opcode));
}

bool CanBypassTarget(const SsaProgram &Ssa, const SsaDomTree &Dom, uint32_t Start,
                     uint32_t Target) {
  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  std::vector<uint8_t> Visited(N, 0);
  llvm::SmallVector<uint32_t, 32> Work;
  Visited[Start] = 1;
  Visited[Target] = 1;
  Work.push_back(Start);
  while (!Work.empty()) {
    const uint32_t Cur = Work.pop_back_val();
    const auto &Blk = Ssa.Blocks[Cur];
    if (Blk.Kind == FlowKind::Return)
      return true;
    for (uint32_t Succ : Blk.Successors) {
      if (Succ < N && Dom.Reachable(Succ) && !Visited[Succ]) {
        Visited[Succ] = 1;
        Work.push_back(Succ);
      }
    }
  }
  return false;
}

bool BlockHasDiscardFrom(const SsaBlock &Blk, size_t StartIdx) {
  for (size_t K = StartIdx; K < Blk.Insts.size(); ++K)
    if (!Blk.Insts[K].Dead && Blk.Insts[K].Opcode == D3D10_SB_OPCODE_DISCARD)
      return true;
  return false;
}

bool IsQuadSinkSafe(const SsaProgram &Ssa, const SsaDomTree &Dom,
                    const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds,
                    uint32_t Start, size_t InstIdx, uint32_t Target) {
  if (BlockHasDiscardFrom(Ssa.Blocks[Start], InstIdx + 1))
    return false;
  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  std::vector<uint8_t> CanReach(N, 0);
  llvm::SmallVector<uint32_t, 32> Work;
  CanReach[Target] = 1;
  Work.push_back(Target);
  while (!Work.empty()) {
    const uint32_t Cur = Work.pop_back_val();
    if (Cur == Start)
      continue;
    for (uint32_t P : Preds[Cur]) {
      if (P < N && Dom.Reachable(P) && !CanReach[P]) {
        CanReach[P] = 1;
        Work.push_back(P);
      }
    }
  }
  if (!CanReach[Start])
    return false;
  std::vector<uint8_t> Visited(N, 0);
  Visited[Start] = 1;
  Visited[Target] = 1;
  Work.push_back(Start);
  while (!Work.empty()) {
    const uint32_t Cur = Work.pop_back_val();
    const auto &Blk = Ssa.Blocks[Cur];
    if (Cur != Start && BlockHasDiscardFrom(Blk, 0))
      return false;
    if ((Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch) &&
        !Blk.UniformBranch)
      return false;
    for (uint32_t Succ : Blk.Successors) {
      if (Succ < N && Dom.Reachable(Succ) && CanReach[Succ] && !Visited[Succ]) {
        Visited[Succ] = 1;
        Work.push_back(Succ);
      }
    }
  }
  return true;
}

} // namespace

bool dxcb5::RunOptColdSink(SsaProgram &Ssa, const SsaDomTree &Dom,
                           const SsaLoopInfo *CachedLoops,
                           PlacementStore *Placement) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Cold Sink", llvm::StringRef());
  if (Ssa.Blocks.size() < 2)
    return false;

  PlacementStore LocalPlacement;
  PlacementStore &Store = Placement ? *Placement : LocalPlacement;

  SsaLoopInfo LocalLoops;
  if (!CachedLoops) {
    ComputeLoopInfo(Ssa, Dom, LocalLoops);
    CachedLoops = &LocalLoops;
  }
  const auto &Preds = CachedLoops->Preds;
  const auto &LoopDepth = CachedLoops->LoopDepth;

  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  const uint32_t NumArrs = static_cast<uint32_t>(Ssa.Mir.IndexableTemps.size());
  constexpr uint32_t kArrMulti = UINT32_MAX - 1u;
  std::vector<uint32_t> ArrStoreBlk(NumArrs, UINT32_MAX);
  std::vector<uint32_t> ArrReadLca(NumArrs, UINT32_MAX);
  std::vector<uint8_t> ArrBad(NumArrs, 0);
  auto RecordArrRead = [&](const Operand &Op, uint32_t UseBlk) {
    if (Op.Kind == OperandKind::IndexableTemp && Op.Index < NumArrs)
      ArrReadLca[Op.Index] = DomLca(Dom, ArrReadLca[Op.Index], UseBlk);
  };

  std::vector<llvm::SmallVector<uint32_t, 4>> Uses(Ssa.NextValueId);
  auto RecordUse = [&](const Operand &Op, uint32_t UseBlk) {
    if (Op.Kind == OperandKind::Temp && Op.Index < Ssa.NextValueId)
      Uses[Op.Index].push_back(UseBlk);
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < Ssa.NextValueId)
      Uses[Op.RelativeRegister].push_back(UseBlk);
    RecordArrRead(Op, UseBlk);
  };
  for (uint32_t B = 0; B < N; ++B) {
    if (!Dom.Reachable(B))
      continue;
    const auto &Blk = Ssa.Blocks[B];
    for (const auto &Phi : Blk.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        if (In.PredBlock < N && Dom.Reachable(In.PredBlock))
          RecordUse(In.Value, In.PredBlock);
    }
    for (const auto &Inst : Blk.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.Kind == OperandKind::IndexableTemp &&
          Inst.Dest.Index < NumArrs) {
        const uint32_t A = Inst.Dest.Index;
        if (!IsSinkableArrayStore(Inst))
          ArrBad[A] = 1;
        else if (ArrStoreBlk[A] == UINT32_MAX)
          ArrStoreBlk[A] = B;
        else if (ArrStoreBlk[A] != B)
          ArrStoreBlk[A] = kArrMulti;
      }
      if (Inst.AuxDest.Kind == OperandKind::IndexableTemp &&
          Inst.AuxDest.Index < NumArrs)
        ArrBad[Inst.AuxDest.Index] = 1;
      if (Inst.Dest.RelativeRegister != UINT32_MAX &&
          Inst.Dest.RelativeRegister < Ssa.NextValueId)
        Uses[Inst.Dest.RelativeRegister].push_back(B);
      if (Inst.AuxDest.RelativeRegister != UINT32_MAX &&
          Inst.AuxDest.RelativeRegister < Ssa.NextValueId)
        Uses[Inst.AuxDest.RelativeRegister].push_back(B);
      for (const auto &Src : Inst.Sources)
        RecordUse(Src, B);
    }
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
      RecordUse(Blk.Condition, B);
  }

  std::vector<uint32_t> ArrSinkTarget(NumArrs, UINT32_MAX);
  for (uint32_t A = 0; A < NumArrs; ++A) {
    const uint32_t SB = ArrStoreBlk[A];
    const uint32_t RL = ArrReadLca[A];
    if (ArrBad[A] || SB >= N || RL >= N || SB == RL || !Dom.Dominates(SB, RL))
      continue;
    if (!SameOrOuterLoops(*CachedLoops, SB, RL))
      continue;
    if (LoopDepth[RL] == LoopDepth[SB] && !CanBypassTarget(Ssa, Dom, SB, RL))
      continue;
    PlacementMove ArrMove;
    ArrMove.Owner = PlacementOwner::ColdSink;
    ArrMove.SrcDepth = LoopDepth[SB];
    ArrMove.DstDepths.push_back(LoopDepth[RL]);
    ArrMove.Bypassable = (LoopDepth[RL] == LoopDepth[SB]);
    if (!Store.TryAccept(ArrMove))
      continue;
    ArrSinkTarget[A] = RL;
  }

  auto RetargetUse = [&](uint32_t ValId, uint32_t OldBlk, uint32_t NewBlk) {
    if (ValId >= Uses.size())
      return;
    for (uint32_t &U : Uses[ValId]) {
      if (U == OldBlk) {
        U = NewBlk;
        return;
      }
    }
  };

  // Bottom-up RPO walk so sunk consumers cascade their single-use producers.
  std::vector<llvm::SmallVector<SsaInst, 8>> SunkTo(N);
  bool SunkAny = false;
  for (auto It = Dom.RpoOrder.rbegin(); It != Dom.RpoOrder.rend(); ++It) {
    const uint32_t B = *It;
    auto &Blk = Ssa.Blocks[B];
    for (size_t I = Blk.Insts.size(); I-- > 0;) {
      auto &Inst = Blk.Insts[I];
      if (Inst.Dead || Inst.HasAuxDest())
        continue;
      if (Inst.Dest.Kind == OperandKind::IndexableTemp &&
          Inst.Dest.Index < NumArrs &&
          ArrSinkTarget[Inst.Dest.Index] != UINT32_MAX) {
        const uint32_t Lca = ArrSinkTarget[Inst.Dest.Index];
        if (Inst.Dest.RelativeRegister != UINT32_MAX)
          RetargetUse(Inst.Dest.RelativeRegister, B, Lca);
        for (const auto &Src : Inst.Sources) {
          if (Src.Kind == OperandKind::Temp)
            RetargetUse(Src.Index, B, Lca);
          if (Src.RelativeRegister != UINT32_MAX)
            RetargetUse(Src.RelativeRegister, B, Lca);
        }
        SunkTo[Lca].push_back(Inst);
        Inst.Dead = true;
        SunkAny = true;
        continue;
      }
      if (Inst.Dest.Kind != OperandKind::Temp ||
          Inst.Dest.Index >= Uses.size())
        continue;
      if (!IsMovablePureOrQuadInst(Inst))
        continue;
      const bool IsQuad = !Inst.IsPure();
      const auto &UList = Uses[Inst.Dest.Index];
      if (UList.empty())
        continue;
      uint32_t Lca = UINT32_MAX;
      for (uint32_t U : UList)
        Lca = DomLca(Dom, Lca, U);
      if (Lca == UINT32_MAX || Lca == B || !Dom.Dominates(B, Lca) ||
          !SameOrOuterLoops(*CachedLoops, B, Lca))
        continue;
      if (LoopDepth[Lca] == LoopDepth[B] && !CanBypassTarget(Ssa, Dom, B, Lca))
        continue;
      if (IsQuad && !IsQuadSinkSafe(Ssa, Dom, Preds, B, I, Lca))
        continue;

      PlacementMove Move;
      Move.ValId = Inst.Dest.Index;
      Move.Owner = PlacementOwner::ColdSink;
      Move.SrcDepth = LoopDepth[B];
      Move.DstDepths.push_back(LoopDepth[Lca]);
      Move.Bypassable = (LoopDepth[Lca] == LoopDepth[B]);
      if (!Store.TryAccept(Move))
        continue;

      for (const auto &Src : Inst.Sources) {
        if (Src.Kind == OperandKind::Temp)
          RetargetUse(Src.Index, B, Lca);
        if (Src.RelativeRegister != UINT32_MAX)
          RetargetUse(Src.RelativeRegister, B, Lca);
      }
      SunkTo[Lca].push_back(Inst);
      Inst.Dead = true;
      SunkAny = true;
    }
  }

  if (SunkAny) {
    for (uint32_t B = 0; B < N; ++B) {
      if (SunkTo[B].empty())
        continue;
      std::reverse(SunkTo[B].begin(), SunkTo[B].end());
      auto &Dst = Ssa.Blocks[B].Insts;
      Dst.insert(Dst.begin(),
                 std::make_move_iterator(SunkTo[B].begin()),
                 std::make_move_iterator(SunkTo[B].end()));
    }
    Ssa.CompactDead();
  }
  return SunkAny;
}

namespace {

constexpr unsigned kMinGuardDiscardCost = 8;

Operand SelectScalarLane(const SsaProgram &Ssa, Operand Op, unsigned Lane) {
  if (Op.Kind == OperandKind::Immediate) {
    Op.Mask = 1u;
    Op.Swizzle = 0u;
    return Op;
  }
  const unsigned SrcLane = GetSwizzleLane(Op.Swizzle, Lane);
  if (Op.Kind == OperandKind::ImmediateVector &&
      Op.Index < Ssa.Mir.LiteralVectors.size() && Op.Modifier == 0) {
    return MakeImm(Ssa.Mir.LiteralVectors[Op.Index][SrcLane]);
  }
  Op.Mask = 1u;
  Op.Swizzle = BroadcastLaneSwizzle(SrcLane);
  return Op;
}

bool IsUniformLane(const SsaProgram &Ssa, const std::vector<SsaDefSite> &Defs,
                   const Operand &Op, unsigned Lane,
                   std::vector<uint8_t> &State) {
  if (Op.Kind == OperandKind::Immediate ||
      Op.Kind == OperandKind::ImmediateVector)
    return true;
  if (Op.Kind == OperandKind::ConstantBuffer ||
      Op.Kind == OperandKind::ImmediateConstantBuffer) {
    if (Op.RelativeRegister == UINT32_MAX)
      return true;
    return IsUniformLane(Ssa, Defs,
                         MakeSsaLane(Op.RelativeRegister, Op.RelativeLane), 0,
                         State);
  }
  if (Op.Kind != OperandKind::Temp || Op.Index >= Defs.size())
    return false;
  const unsigned SrcLane = GetSwizzleLane(Op.Swizzle, Lane);
  const size_t Key = static_cast<size_t>(Op.Index) * 4u + SrcLane;
  if (Key >= State.size())
    return false;
  if (State[Key] != 0)
    return State[Key] == 2;
  State[Key] = 1; // visiting (guards cycles)

  bool Uniform = false;
  const SsaDefSite &Def = Defs[Op.Index];
  if (Def.Valid() && !Def.IsPhi && !Def.IsAux) {
    const SsaInst &Inst = Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (!Inst.Dead && Inst.Dest.RelativeRegister == UINT32_MAX) {
      if (Inst.Opcode == PseudoCombineVec) {
        if (SrcLane < Inst.Sources.size())
          Uniform = IsUniformLane(Ssa, Defs, Inst.Sources[SrcLane], 0, State);
      } else if (Inst.IsPure()) {
        Uniform = true;
        const bool LaneLocal = InstructionIsComponentwise(Inst.Opcode) ||
                               Inst.Opcode == D3D10_SB_OPCODE_MOV ||
                               Inst.Opcode == D3D10_SB_OPCODE_MOVC;
        for (size_t S = 0; S < Inst.Sources.size() && Uniform; ++S) {
          const Operand &Src = Inst.Sources[S];
          if (Src.RelativeRegister != UINT32_MAX &&
              !IsUniformLane(Ssa, Defs,
                             MakeSsaLane(Src.RelativeRegister, Src.RelativeLane),
                             0, State)) {
            Uniform = false;
            break;
          }
          if (LaneLocal) {
            Uniform = IsUniformLane(Ssa, Defs, Src, SrcLane, State);
          } else {
            const uint8_t SlotMask =
                InstructionSourceSlotMask(Inst.Opcode, Inst.Dest.Mask, S);
            for (unsigned L = 0; L < 4 && Uniform; ++L) {
              if (SlotMask & (1u << L))
                Uniform = IsUniformLane(Ssa, Defs, Src, L, State);
            }
          }
        }
      }
    }
  }
  State[Key] = Uniform ? 2 : 3;
  return Uniform;
}

bool IsBoolMaskLane(const SsaProgram &Ssa, const std::vector<SsaDefSite> &Defs,
                    const Operand &Op, unsigned Lane, unsigned Depth = 0) {
  if (Depth > 16 || Op.Modifier != 0)
    return false;
  if (Op.Kind == OperandKind::Immediate)
    return Op.Index == 0u || Op.Index == 0xffffffffu;
  const unsigned SrcLane = GetSwizzleLane(Op.Swizzle, Lane);
  if (Op.Kind == OperandKind::ImmediateVector &&
      Op.Index < Ssa.Mir.LiteralVectors.size()) {
    const uint32_t Bits = Ssa.Mir.LiteralVectors[Op.Index][SrcLane];
    return Bits == 0u || Bits == 0xffffffffu;
  }
  if (Op.Kind != OperandKind::Temp || Op.Index >= Defs.size())
    return false;
  const SsaDefSite &Def = Defs[Op.Index];
  if (!Def.Valid() || Def.IsPhi || Def.IsAux)
    return false;
  const SsaInst &Inst = Ssa.Blocks[Def.Block].Insts[Def.Index];
  if (Inst.Dead)
    return false;
  if (InstructionIsCompare(Inst.Opcode))
    return true;
  if (Inst.Opcode == PseudoCombineVec) {
    return SrcLane < Inst.Sources.size() &&
           IsBoolMaskLane(Ssa, Defs, Inst.Sources[SrcLane], 0, Depth + 1);
  }
  if (Inst.Opcode == D3D10_SB_OPCODE_NOT && Inst.Sources.size() == 1)
    return IsBoolMaskLane(Ssa, Defs, Inst.Sources[0], SrcLane, Depth + 1);
  if ((Inst.Opcode == D3D10_SB_OPCODE_AND ||
       Inst.Opcode == D3D10_SB_OPCODE_OR ||
       Inst.Opcode == D3D10_SB_OPCODE_XOR) &&
      Inst.Sources.size() == 2) {
    return IsBoolMaskLane(Ssa, Defs, Inst.Sources[0], SrcLane, Depth + 1) &&
           IsBoolMaskLane(Ssa, Defs, Inst.Sources[1], SrcLane, Depth + 1);
  }
  if (Inst.Opcode == D3D10_SB_OPCODE_MOV && Inst.Sources.size() == 1 &&
      !Inst.Saturate)
    return IsBoolMaskLane(Ssa, Defs, Inst.Sources[0], SrcLane, Depth + 1);
  return false;
}

void CountAllTempUses(const SsaProgram &Ssa, std::vector<uint32_t> &TotalUses) {
  TotalUses.assign(Ssa.NextValueId, 0);
  auto Touch = [&](const Operand &Op) {
    if (Op.Kind == OperandKind::Temp && Op.Index < TotalUses.size())
      ++TotalUses[Op.Index];
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < TotalUses.size())
      ++TotalUses[Op.RelativeRegister];
  };
  for (const auto &Blk : Ssa.Blocks) {
    for (const auto &Phi : Blk.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        Touch(In.Value);
    }
    for (const auto &Inst : Blk.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.RelativeRegister != UINT32_MAX &&
          Inst.Dest.RelativeRegister < TotalUses.size())
        ++TotalUses[Inst.Dest.RelativeRegister];
      if (Inst.AuxDest.RelativeRegister != UINT32_MAX &&
          Inst.AuxDest.RelativeRegister < TotalUses.size())
        ++TotalUses[Inst.AuxDest.RelativeRegister];
      for (const auto &Src : Inst.Sources)
        Touch(Src);
    }
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
      Touch(Blk.Condition);
  }
}

bool HasLivePhis(const SsaBlock &Blk) {
  for (const auto &Phi : Blk.Phis)
    if (!Phi.Dead)
      return true;
  return false;
}

unsigned EstimateExclusiveConeCost(const SsaBlock &Blk, size_t CombIdx,
                                   const Operand &PixelOp,
                                   const std::vector<uint32_t> &TotalUses) {
  std::vector<uint32_t> ConeUses(TotalUses.size(), 0);
  auto AddUse = [&](const Operand &Op) {
    if (Op.Kind == OperandKind::Temp && Op.Index < ConeUses.size())
      ++ConeUses[Op.Index];
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < ConeUses.size())
      ++ConeUses[Op.RelativeRegister];
  };
  AddUse(PixelOp);

  unsigned Cost = 0;
  for (size_t K = Blk.Insts.size(); K-- > 0;) {
    if (K == CombIdx)
      continue;
    const SsaInst &Inst = Blk.Insts[K];
    if (Inst.Dead || Inst.HasAuxDest() || !Inst.IsPure() ||
        Inst.Dest.Kind != OperandKind::Temp ||
        Inst.Dest.RelativeRegister != UINT32_MAX ||
        Inst.Dest.Index >= TotalUses.size())
      continue;
    if (TotalUses[Inst.Dest.Index] == 0 ||
        ConeUses[Inst.Dest.Index] != TotalUses[Inst.Dest.Index])
      continue;
    if (Inst.Opcode != PseudoCombineVec)
      ++Cost;
    for (const auto &Src : Inst.Sources)
      AddUse(Src);
  }
  return Cost;
}

} // namespace

// Splits a one-sided branch `if (uniformBool && heavyVaryingCone) { then }`
// (where `then` rejoins `join` or returns, and neither `then` nor `join` has
// live PHIs) into `if (uniformBool) { if (heavyVaryingCone) { then } }`.
// Leaves instruction movement entirely to RunOptColdSink immediately after.
bool dxcb5::RunOptGuardDiscard(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Guard Discard", llvm::StringRef());
  const size_t OrigCount = Ssa.Blocks.size();
  if (OrigCount < 2)
    return false;

  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<uint32_t> TotalUses;
  CountAllTempUses(Ssa, TotalUses);
  std::vector<uint8_t> UniformState(static_cast<size_t>(Ssa.NextValueId) * 4u, 0);

  bool Changed = false;
  for (uint32_t B = 0; B < OrigCount; ++B) {
    const auto &Blk = Ssa.Blocks[B];
    if (Blk.Kind != FlowKind::Branch || Blk.Successors.size() != 2)
      continue;
    const uint32_t ThenId = Blk.Successors[0];
    const uint32_t JoinId = Blk.Successors[1];
    if (ThenId == JoinId || ThenId >= Ssa.Blocks.size() ||
        JoinId >= Ssa.Blocks.size())
      continue;
    const auto &ThenBlk = Ssa.Blocks[ThenId];
    const bool OneSidedArm =
        ThenBlk.Kind == FlowKind::Return ||
        (ThenBlk.Kind == FlowKind::Jump && ThenBlk.Successors.size() == 1 &&
         ThenBlk.Successors[0] == JoinId);
    if (!OneSidedArm || HasLivePhis(ThenBlk) || HasLivePhis(Ssa.Blocks[JoinId]))
      continue;

    const Operand &Pred = Blk.Condition;
    if (Pred.Kind != OperandKind::Temp || Pred.Modifier != 0 ||
        Pred.Index >= Defs.size())
      continue;
    const SsaDefSite &AndDef = Defs[Pred.Index];
    if (!AndDef.Valid() || AndDef.IsPhi || AndDef.IsAux || AndDef.Block != B)
      continue;
    const SsaInst &AndInst = Blk.Insts[AndDef.Index];
    if (AndInst.Dead || AndInst.Opcode != D3D10_SB_OPCODE_AND ||
        AndInst.Sources.size() != 2 || PopcountMask(AndInst.Dest.Mask) != 1 ||
        TotalUses[AndInst.Dest.Index] != 1)
      continue;

    const unsigned Lane = GetSwizzleLane(Pred.Swizzle, 0);
    unsigned BestCost = 0;
    unsigned BestUI = UINT32_MAX;
    for (unsigned UI = 0; UI < 2; ++UI) {
      const Operand &U = AndInst.Sources[UI];
      const Operand &V = AndInst.Sources[1 - UI];
      if (U.Modifier != 0 || V.Modifier != 0)
        continue;
      if (U.Kind != OperandKind::Temp && U.Kind != OperandKind::ConstantBuffer)
        continue;
      if (!IsUniformLane(Ssa, Defs, U, Lane, UniformState) ||
          !IsBoolMaskLane(Ssa, Defs, U, Lane) ||
          IsUniformLane(Ssa, Defs, V, Lane, UniformState))
        continue;
      const unsigned Cost =
          EstimateExclusiveConeCost(Blk, AndDef.Index, V, TotalUses);
      if (Cost >= kMinGuardDiscardCost && Cost > BestCost) {
        BestCost = Cost;
        BestUI = UI;
      }
    }
    if (BestUI == UINT32_MAX)
      continue;

    const Operand UniformCond =
        SelectScalarLane(Ssa, AndInst.Sources[BestUI], Lane);
    const Operand PixelCond =
        SelectScalarLane(Ssa, AndInst.Sources[1 - BestUI], Lane);

    const uint32_t MidId = static_cast<uint32_t>(Ssa.Blocks.size());
    Ssa.Blocks.emplace_back();
    SsaBlock &Orig = Ssa.Blocks[B];
    SsaBlock &MidBlk = Ssa.Blocks[MidId];

    Orig.Insts[AndDef.Index].Dead = true;
    MidBlk.Kind = FlowKind::Branch;
    MidBlk.Condition = PixelCond;
    MidBlk.Successors = {ThenId, JoinId};
    MidBlk.StrictBranch = Orig.StrictBranch;
    MidBlk.UniformBranch = Orig.UniformBranch;

    Orig.Condition = UniformCond;
    Orig.Successors = {MidId, JoinId};
    Orig.StrictBranch = false;
    Orig.UniformBranch = true;
    Changed = true;
  }
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}
