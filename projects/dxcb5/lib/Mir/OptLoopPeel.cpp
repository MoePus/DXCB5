// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "Mir/SsaAnalysis.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <vector>

using namespace dxcb5;

namespace {

bool IsTexOp(uint32_t Op) {
  switch (Op) {
  case D3D10_SB_OPCODE_SAMPLE:
  case D3D10_SB_OPCODE_SAMPLE_B:
  case D3D10_SB_OPCODE_SAMPLE_C:
  case D3D10_SB_OPCODE_SAMPLE_C_LZ:
  case D3D10_SB_OPCODE_SAMPLE_D:
  case D3D10_SB_OPCODE_SAMPLE_L:
  case D3D10_SB_OPCODE_LD:
  case D3D10_SB_OPCODE_LD_MS:
  case D3D10_1_SB_OPCODE_GATHER4:
  case D3D11_SB_OPCODE_GATHER4_C:
  case D3D11_SB_OPCODE_GATHER4_PO:
  case D3D11_SB_OPCODE_GATHER4_PO_C:
    return true;
  default:
    return false;
  }
}

bool HasLoopSideEffect(const SsaProgram &Ssa, const NaturalLoop &Loop) {
  for (uint32_t B : Loop.Blocks) {
    for (const auto &I : Ssa.Blocks[B].Insts) {
      if (I.Dead)
        continue;
      // Reject any non-eliminable op (discard, store, atomic, sync) using the
      // Lean-backed OpcodeSpec table.
      if (!InstructionCanEliminateIfDead(I.Opcode))
        return true;
      const auto DK = I.Dest.Kind;
      if (DK == OperandKind::Output || DK == OperandKind::OutputDepth ||
          DK == OperandKind::OutputMask || DK == OperandKind::Uav ||
          DK == OperandKind::IndexableTemp ||
          I.AuxDest.Kind == OperandKind::Uav)
        return true;
    }
  }
  return false;
}

bool FindSingleLoopExit(const SsaProgram &Ssa, const SsaDomTree &Dom,
                        const SsaLoopInfo &Loops, const NaturalLoop &Loop,
                        uint32_t &Latch, uint32_t &ExitBlk) {
  if (Loop.Preheader == UINT32_MAX || Loop.Latches.size() != 1)
    return false;
  Latch = Loop.Latches[0];
  ExitBlk = UINT32_MAX;
  for (uint32_t B : Loop.Blocks) {
    for (uint32_t S : Ssa.Blocks[B].Successors) {
      if (S >= Ssa.Blocks.size() || Loop.InLoop[S])
        continue;
      if (B != Latch || ExitBlk != UINT32_MAX)
        return false;
      ExitBlk = S;
    }
  }
  if (ExitBlk == UINT32_MAX)
    return false;
  const auto &LB = Ssa.Blocks[Latch];
  if (LB.Kind != FlowKind::Branch || LB.Successors.size() != 2)
    return false;
  if (!((LB.Successors[0] == Loop.Header && LB.Successors[1] == ExitBlk) ||
        (LB.Successors[0] == ExitBlk && LB.Successors[1] == Loop.Header)))
    return false;
  // ExitBlk must have Latch as its sole reachable predecessor and no Phis.
  if (!Ssa.Blocks[ExitBlk].Phis.empty())
    return false;
  for (uint32_t P : Loops.Preds[ExitBlk])
    if (Dom.Reachable(P) && P != Latch)
      return false;
  return true;
}

struct TripInfo {
  uint32_t CmpBlock = 0;
  uint32_t CmpInstIdx = 0;
  unsigned BoundSrcIdx = 0;
  uint32_t BoundVal = 0;
};

bool MatchFixedTripCount(const SsaProgram &Ssa, const NaturalLoop &Loop,
                         uint32_t Latch, uint32_t ExitBlk,
                         const std::vector<SsaDefSite> &Defs,
                         const std::vector<uint32_t> &UseCount,
                         TripInfo &Out) {
  const auto &LB = Ssa.Blocks[Latch];
  const auto &Cond = LB.Condition;
  if (Cond.Kind != OperandKind::Temp || Cond.Modifier ||
      Cond.RelativeRegister != UINT32_MAX || Cond.Index >= Defs.size() ||
      UseCount[Cond.Index] != 1)
    return false;
  const auto &CD = Defs[Cond.Index];
  if (!CD.Valid() || CD.IsPhi || CD.IsAux || !Loop.InLoop[CD.Block])
    return false;
  const auto &Cmp = Ssa.Blocks[CD.Block].Insts[CD.Index];
  if (Cmp.Dead || Cmp.Sources.size() != 2 || PopcountMask(Cmp.Dest.Mask) != 1)
    return false;

  const bool BreakOnTrue = LB.Successors[0] == ExitBlk;
  uint32_t NextIdxReg = 0;
  unsigned BoundSide = 2;
  uint32_t Bound = 0;
  if (BreakOnTrue && Cmp.Opcode == D3D10_SB_OPCODE_IEQ) {
    for (unsigned S = 0; S < 2; ++S) {
      if (Cmp.Sources[S].Kind == OperandKind::Immediate &&
          Cmp.Sources[1 - S].Kind == OperandKind::Temp &&
          !Cmp.Sources[1 - S].Modifier) {
        BoundSide = S;
        Bound = Cmp.Sources[S].Index;
        NextIdxReg = Cmp.Sources[1 - S].Index;
        break;
      }
    }
  } else if (!BreakOnTrue &&
             (Cmp.Opcode == D3D10_SB_OPCODE_INE ||
              Cmp.Opcode == D3D10_SB_OPCODE_ILT ||
              Cmp.Opcode == D3D10_SB_OPCODE_ULT)) {
    if (Cmp.Sources[0].Kind == OperandKind::Temp && !Cmp.Sources[0].Modifier &&
        Cmp.Sources[1].Kind == OperandKind::Immediate) {
      BoundSide = 1;
      Bound = Cmp.Sources[1].Index;
      NextIdxReg = Cmp.Sources[0].Index;
    }
  }
  if (BoundSide == 2 || NextIdxReg >= Defs.size())
    return false;

  const auto &ND = Defs[NextIdxReg];
  if (!ND.Valid() || ND.IsPhi || ND.IsAux || !Loop.InLoop[ND.Block])
    return false;
  const auto &Add = Ssa.Blocks[ND.Block].Insts[ND.Index];
  if (Add.Dead || Add.Opcode != D3D10_SB_OPCODE_IADD || Add.Sources.size() != 2)
    return false;
  uint32_t PhiId = 0;
  for (unsigned S = 0; S < 2; ++S) {
    if (Add.Sources[S].Kind == OperandKind::Immediate &&
        Add.Sources[S].Index == 1u &&
        Add.Sources[1 - S].Kind == OperandKind::Temp &&
        !Add.Sources[1 - S].Modifier) {
      PhiId = Add.Sources[1 - S].Index;
      break;
    }
  }
  if (PhiId == 0 || PhiId >= Defs.size())
    return false;
  const auto &PD = Defs[PhiId];
  if (!PD.Valid() || !PD.IsPhi || PD.Block != Loop.Header)
    return false;
  const auto &Phi = Ssa.Blocks[Loop.Header].Phis[PD.Index];
  if (Phi.Dead || Phi.Inputs.size() != 2)
    return false;

  bool HasStart = false, HasLatch = false;
  uint32_t Start = 0;
  for (const auto &In : Phi.Inputs) {
    if (In.PredBlock == Loop.Preheader &&
        In.Value.Kind == OperandKind::Immediate && !In.Value.Modifier) {
      Start = In.Value.Index;
      HasStart = true;
    } else if (In.PredBlock == Latch && In.Value.Kind == OperandKind::Temp &&
               In.Value.Index == NextIdxReg && !In.Value.Modifier) {
      HasLatch = true;
    }
  }
  if (!HasStart || !HasLatch || Start >= Bound || (Bound - Start) < 2u ||
      (Bound - Start) > 256u)
    return false;

  Out = {CD.Block, CD.Index, BoundSide, Bound};
  return true;
}

void RewriteOperandFromRemap(Operand &Op,
                             const llvm::DenseMap<uint32_t, Operand> &Remap,
                             bool IntNeg = false) {
  if (Op.RelativeRegister != UINT32_MAX) {
    auto RIt = Remap.find(Op.RelativeRegister);
    if (RIt != Remap.end() && RIt->second.Kind == OperandKind::Temp) {
      Op.RelativeRegister = RIt->second.Index;
      Op.RelativeLane = GetSwizzleLane(RIt->second.Swizzle, Op.RelativeLane);
    }
  }
  if (Op.Kind != OperandKind::Temp)
    return;
  auto It = Remap.find(Op.Index);
  if (It == Remap.end())
    return;
  const Operand &Repl = It->second;
  if (Repl.Kind == OperandKind::Immediate) {
    const uint8_t EffMod =
        IntNeg ? Op.Modifier : ComposeFloatModifier(Repl.Modifier, Op.Modifier);
    uint32_t Bits = Repl.Index;
    if (EffMod == D3D10_SB_OPERAND_MODIFIER_NEG)
      Bits = IntNeg ? (0u - Bits) : (Bits ^ kFloatSignBit);
    else if (EffMod == D3D10_SB_OPERAND_MODIFIER_ABS)
      Bits &= kFloatAbsMask;
    else if (EffMod == D3D10_SB_OPERAND_MODIFIER_ABSNEG)
      Bits |= kFloatSignBit;
    Op = MakeImm(Bits);
    return;
  }
  Operand Next = Repl;
  Next.Mask = Op.Mask;
  Next.Swizzle = ComposeSwizzle(Repl.Swizzle, Op.Swizzle);
  Next.Modifier =
      IntNeg ? Op.Modifier : ComposeFloatModifier(Repl.Modifier, Op.Modifier);
  Op = Next;
}

void RewriteDestRelative(Operand &Dst,
                         const llvm::DenseMap<uint32_t, Operand> &Remap) {
  if (Dst.RelativeRegister == UINT32_MAX)
    return;
  auto RIt = Remap.find(Dst.RelativeRegister);
  if (RIt != Remap.end() && RIt->second.Kind == OperandKind::Temp) {
    Dst.RelativeRegister = RIt->second.Index;
    Dst.RelativeLane = GetSwizzleLane(RIt->second.Swizzle, Dst.RelativeLane);
  }
}

bool TryPeelLoopLastIter(SsaProgram &Ssa, const SsaDomTree &Dom,
                         const SsaLoopInfo &Loops, const NaturalLoop &Loop,
                         const std::vector<SsaDefSite> &Defs,
                         const std::vector<uint32_t> &UseCount) {
  uint32_t Latch = UINT32_MAX, ExitBlk = UINT32_MAX;
  if (!FindSingleLoopExit(Ssa, Dom, Loops, Loop, Latch, ExitBlk) ||
      HasLoopSideEffect(Ssa, Loop))
    return false;
  TripInfo Trip;
  if (!MatchFixedTripCount(Ssa, Loop, Latch, ExitBlk, Defs, UseCount, Trip))
    return false;

  // Collect all loop-defined values used outside the loop.
  llvm::SmallVector<uint32_t, 8> ExitLive;
  auto RecordExitUse = [&](const Operand &Op) {
    auto Touch = [&](uint32_t Id) {
      if (Id >= Defs.size() || !Defs[Id].Valid() || !Loop.InLoop[Defs[Id].Block])
        return;
      if (std::find(ExitLive.begin(), ExitLive.end(), Id) == ExitLive.end())
        ExitLive.push_back(Id);
    };
    if (Op.Kind == OperandKind::Temp)
      Touch(Op.Index);
    if (Op.RelativeRegister != UINT32_MAX)
      Touch(Op.RelativeRegister);
  };
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    if (Loop.InLoop[B] || !Dom.Reachable(B))
      continue;
    const auto &Blk = Ssa.Blocks[B];
    for (const auto &Phi : Blk.Phis)
      if (!Phi.Dead)
        for (const auto &In : Phi.Inputs)
          RecordExitUse(In.Value);
    for (const auto &Inst : Blk.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.RelativeRegister != UINT32_MAX)
        RecordExitUse(MakeSsaTemp(Inst.Dest.RelativeRegister));
      for (const auto &Src : Inst.Sources)
        RecordExitUse(Src);
    }
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
      RecordExitUse(Blk.Condition);
  }
  if (ExitLive.empty())
    return false;

  // Build backward slice of ExitLive strictly within Loop.Header (pure ALU).
  llvm::SmallVector<uint32_t, 24> SliceIndices;
  llvm::SmallVector<uint32_t, 24> Work(ExitLive.begin(), ExitLive.end());
  std::vector<uint8_t> Visited(Ssa.NextValueId, 0);
  while (!Work.empty()) {
    const uint32_t V = Work.pop_back_val();
    if (V >= Visited.size() || Visited[V])
      continue;
    Visited[V] = 1;
    const auto &D = Defs[V];
    if (!D.Valid() || !Loop.InLoop[D.Block])
      continue;
    if (D.IsPhi) {
      if (D.Block != Loop.Header)
        return false;
      continue;
    }
    if (D.IsAux || D.Block != Loop.Header)
      return false;
    const auto &Inst = Ssa.Blocks[Loop.Header].Insts[D.Index];
    if (Inst.Dead || !Inst.IsPure() || Inst.HasAuxDest())
      return false;
    SliceIndices.push_back(D.Index);
    if (SliceIndices.size() > 24)
      return false;
    for (const auto &Src : Inst.Sources) {
      if (Src.Kind == OperandKind::Temp && Src.Index < Defs.size() &&
          Defs[Src.Index].Valid() && Loop.InLoop[Defs[Src.Index].Block])
        Work.push_back(Src.Index);
      if (Src.RelativeRegister != UINT32_MAX &&
          Src.RelativeRegister < Defs.size() &&
          Defs[Src.RelativeRegister].Valid() &&
          Loop.InLoop[Defs[Src.RelativeRegister].Block])
        Work.push_back(Src.RelativeRegister);
    }
  }

  // Only peel if there is a texture op inside the loop OUTSIDE ExitSlice.
  bool HasBackOnlyTex = false;
  for (uint32_t B : Loop.Blocks) {
    const auto &Insts = Ssa.Blocks[B].Insts;
    for (uint32_t I = 0; I < Insts.size(); ++I) {
      if (Insts[I].Dead || !IsTexOp(Insts[I].Opcode))
        continue;
      if (B != Loop.Header ||
          std::find(SliceIndices.begin(), SliceIndices.end(), I) ==
              SliceIndices.end()) {
        HasBackOnlyTex = true;
        break;
      }
    }
    if (HasBackOnlyTex)
      break;
  }
  if (!HasBackOnlyTex)
    return false;

  // Map every Header phi to its Latch back-edge value (which dominates Exit).
  llvm::DenseMap<uint32_t, Operand> Remap;
  for (const auto &Phi : Ssa.Blocks[Loop.Header].Phis) {
    if (Phi.Dead)
      continue;
    for (const auto &In : Phi.Inputs) {
      if (In.PredBlock == Latch) {
        if (In.Value.Kind != OperandKind::Temp &&
            In.Value.Kind != OperandKind::Immediate)
          return false;
        Remap[Phi.Result] = In.Value;
        break;
      }
    }
  }

  std::sort(SliceIndices.begin(), SliceIndices.end());
  llvm::SmallVector<SsaInst, 24> Cloned;
  for (uint32_t Idx : SliceIndices) {
    SsaInst Copy = Ssa.Blocks[Loop.Header].Insts[Idx];
    const uint32_t OldId = Copy.Dest.Index;
    const uint32_t NewId = Ssa.AllocValue();
    const bool CopyIntNeg = InstructionAcceptsIntNegModifier(Copy.Opcode);
    for (auto &Src : Copy.Sources)
      RewriteOperandFromRemap(Src, Remap, CopyIntNeg);
    Copy.Dest.Index = NewId;
    const uint8_t Swz = PopcountMask(Copy.Dest.Mask) == 1
                            ? BroadcastLaneSwizzle(FirstActiveLane(Copy.Dest.Mask))
                            : kIdentitySwizzle;
    Remap[OldId] = MakeSsaTemp(NewId, Copy.Dest.Mask, Swz);
    Cloned.push_back(std::move(Copy));
  }

  // Decrement trip count by 1 and prepend peeled last-iteration slice to Exit.
  Ssa.Blocks[Trip.CmpBlock].Insts[Trip.CmpInstIdx].Sources[Trip.BoundSrcIdx] =
      MakeImm(Trip.BoundVal - 1u);
  auto &ExitInsts = Ssa.Blocks[ExitBlk].Insts;
  ExitInsts.insert(ExitInsts.begin(), std::make_move_iterator(Cloned.begin()),
                   std::make_move_iterator(Cloned.end()));

  // Rewrite all post-loop uses of ExitLive values to the peeled results.
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    if (Loop.InLoop[B] || !Dom.Reachable(B))
      continue;
    auto &Blk = Ssa.Blocks[B];
    const size_t StartIdx = (B == ExitBlk) ? SliceIndices.size() : 0;
    for (auto &Phi : Blk.Phis)
      if (!Phi.Dead)
        for (auto &In : Phi.Inputs)
          RewriteOperandFromRemap(In.Value, Remap);
    for (size_t I = StartIdx; I < Blk.Insts.size(); ++I) {
      auto &Inst = Blk.Insts[I];
      if (Inst.Dead)
        continue;
      RewriteDestRelative(Inst.Dest, Remap);
      const bool InstIntNeg = InstructionAcceptsIntNegModifier(Inst.Opcode);
      for (auto &Src : Inst.Sources)
        RewriteOperandFromRemap(Src, Remap, InstIntNeg);
    }
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
      RewriteOperandFromRemap(Blk.Condition, Remap);
  }
  return true;
}

} // namespace

bool dxcb5::RunOptLoopPeel(SsaProgram &Ssa, const SsaDomTree &Dom,
                           const SsaLoopInfo *CachedLoops) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Loop Peel", llvm::StringRef());
  if (Ssa.Blocks.size() < 3)
    return false;
  SsaLoopInfo LocalLoops;
  if (!CachedLoops) {
    ComputeLoopInfo(Ssa, Dom, LocalLoops);
    CachedLoops = &LocalLoops;
  }
  if (CachedLoops->Loops.empty())
    return false;
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<uint32_t> UseCount(Ssa.NextValueId, 0);
  auto Bump = [&](const Operand &Op) {
    if (Op.Kind == OperandKind::Temp && Op.Index < UseCount.size())
      ++UseCount[Op.Index];
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < UseCount.size())
      ++UseCount[Op.RelativeRegister];
  };
  for (const auto &Blk : Ssa.Blocks) {
    for (const auto &Phi : Blk.Phis)
      if (!Phi.Dead)
        for (const auto &In : Phi.Inputs)
          Bump(In.Value);
    for (const auto &Inst : Blk.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.RelativeRegister != UINT32_MAX &&
          Inst.Dest.RelativeRegister < UseCount.size())
        ++UseCount[Inst.Dest.RelativeRegister];
      for (const auto &Src : Inst.Sources)
        Bump(Src);
    }
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
      Bump(Blk.Condition);
  }

  for (const auto &Loop : CachedLoops->Loops)
    if (TryPeelLoopLastIter(Ssa, Dom, *CachedLoops, Loop, Defs, UseCount))
      return true;
  return false;
}
