// Distributed under the University of Illinois Open Source License.
#include "Mir/Combine/CombineCommon.h"
#include "llvm/Support/TimeProfiler.h"

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

bool OutputFoldShapeOk(const SsaInst &Prod, const SsaInst &Mov,
                       bool &NeedsSwizzle) {
  NeedsSwizzle = false;
  const auto &Src = Mov.Sources[0];
  const bool IsDot = Prod.Opcode == D3D10_SB_OPCODE_DP2 ||
                     Prod.Opcode == D3D10_SB_OPCODE_DP3 ||
                     Prod.Opcode == D3D10_SB_OPCODE_DP4;
  if (IsDot) {
    return PopcountMask(Prod.Dest.Mask) == 1 &&
           PopcountMask(Mov.Dest.Mask) == 1 &&
           GetSwizzleLane(Src.Swizzle, FirstActiveLane(Mov.Dest.Mask)) ==
               FirstActiveLane(Prod.Dest.Mask);
  }
  if (!InstructionIsComponentwise(Prod.Opcode))
    return Prod.Dest.Mask == Mov.Dest.Mask &&
           IsIdentitySwizzle(Src.Swizzle, Mov.Dest.Mask);
  if (PopcountMask(Prod.Dest.Mask) != PopcountMask(Mov.Dest.Mask))
    return false;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(Mov.Dest.Mask & (1u << L)))
      continue;
    const unsigned SL = GetSwizzleLane(Src.Swizzle, L);
    if (!(Prod.Dest.Mask & (1u << SL)))
      return false;
    if (SL != L)
      NeedsSwizzle = true;
  }
  if (NeedsSwizzle) {
    for (const auto &PS : Prod.Sources)
      if (PS.Kind == OperandKind::ImmediateVector)
        return false;
  }
  return true;
}

bool OutputRangeSafe(const SsaBlock &Block, uint32_t From, uint32_t To,
                     const Operand &OutDest) {
  for (uint32_t K = From; K < To; ++K) {
    const auto &Mid = Block.Insts[K];
    if (Mid.Dead)
      continue;
    if (Mid.Opcode == D3D10_SB_OPCODE_DISCARD)
      return false;
    if (Mid.Dest.Kind == OperandKind::Output &&
        Mid.Dest.Index == OutDest.Index && (Mid.Dest.Mask & OutDest.Mask) != 0)
      return false;
  }
  return true;
}

bool CrossBlockOutputRangeSafe(const SsaProgram &Ssa, uint32_t FromBlk,
                               uint32_t FromIdx, uint32_t ToBlk,
                               uint32_t ToIdx, const Operand &OutDest) {
  if (FromBlk == ToBlk)
    return FromIdx < ToIdx &&
           OutputRangeSafe(Ssa.Blocks[FromBlk], FromIdx + 1, ToIdx, OutDest);
  if (!OutputRangeSafe(Ssa.Blocks[FromBlk], FromIdx + 1,
                       static_cast<uint32_t>(Ssa.Blocks[FromBlk].Insts.size()),
                       OutDest) ||
      !OutputRangeSafe(Ssa.Blocks[ToBlk], 0, ToIdx, OutDest))
    return false;
  std::vector<uint8_t> Visited(Ssa.Blocks.size(), 0);
  llvm::SmallVector<uint32_t, 32> Work;
  for (uint32_t S : Ssa.Blocks[FromBlk].Successors)
    Work.push_back(S);
  while (!Work.empty()) {
    const uint32_t Cur = Work.pop_back_val();
    if (Cur == ToBlk || Cur >= Ssa.Blocks.size() || Visited[Cur])
      continue;
    Visited[Cur] = 1;
    const auto &Blk = Ssa.Blocks[Cur];
    if (Blk.Kind == FlowKind::Return ||
        !OutputRangeSafe(Blk, 0, static_cast<uint32_t>(Blk.Insts.size()),
                         OutDest))
      return false;
    for (uint32_t S : Blk.Successors)
      Work.push_back(S);
  }
  return true;
}

bool SameMergeableOutSrc(const Operand &A, const Operand &B) {
  if (A.RelativeRegister != UINT32_MAX || B.RelativeRegister != UINT32_MAX ||
      A.Kind != B.Kind || A.Modifier != B.Modifier)
    return false;
  if (A.Kind == OperandKind::Immediate)
    return A.Index == B.Index;
  return (A.Kind == OperandKind::Temp || A.Kind == OperandKind::Input ||
          A.Kind == OperandKind::ConstantBuffer) &&
         SameOperandBase(A, B);
}

bool MergeBlockOutputMoves(SsaBlock &Block) {
  bool Changed = false;
  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    auto &A = Block.Insts[I];
    if (A.Dead || A.Opcode != D3D10_SB_OPCODE_MOV ||
        A.Dest.Kind != OperandKind::Output ||
        A.Dest.RelativeRegister != UINT32_MAX || A.Sources.size() != 1)
      continue;
    for (uint32_t J = I + 1; J < Block.Insts.size(); ++J) {
      auto &C = Block.Insts[J];
      if (C.Dead)
        continue;
      if (C.Opcode == D3D10_SB_OPCODE_DISCARD)
        break;
      if (C.Dest.Kind == OperandKind::Output && C.Dest.Index == A.Dest.Index &&
          (C.Dest.Mask & A.Dest.Mask) != 0)
        break;
      if (C.Opcode != D3D10_SB_OPCODE_MOV || C.Saturate != A.Saturate ||
          C.Dest.Kind != OperandKind::Output || C.Dest.Index != A.Dest.Index ||
          C.Dest.RelativeRegister != UINT32_MAX || C.Sources.size() != 1 ||
          !SameMergeableOutSrc(A.Sources[0], C.Sources[0]))
        continue;
      if (C.Sources[0].Kind != OperandKind::Immediate) {
        for (unsigned L = 0; L < kRegisterLanes; ++L)
          if (A.Dest.Mask & (1u << L))
            C.Sources[0].Swizzle = SetSwizzleLane(
                C.Sources[0].Swizzle, L, GetSwizzleLane(A.Sources[0].Swizzle, L));
      }
      C.Dest.Mask |= A.Dest.Mask;
      A.Dead = true;
      Changed = true;
      break;
    }
  }
  return Changed;
}

} // namespace

bool dxcb5::RunOptFoldOutputs(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Fold Outputs", llvm::StringRef());
  bool Changed = false;
  for (auto &Block : Ssa.Blocks)
    Changed |= MergeBlockOutputMoves(Block);
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  UseStats Stats;
  CollectUseStats(Ssa, Stats);
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Block = Ssa.Blocks[B];
    for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
      auto &Mov = Block.Insts[I];
      if (Mov.Dead || Mov.Opcode != D3D10_SB_OPCODE_MOV ||
          Mov.Dest.Kind != OperandKind::Output ||
          Mov.Dest.RelativeRegister != UINT32_MAX || Mov.Sources.size() != 1)
        continue;
      const auto &Src = Mov.Sources[0];
      if (Src.Kind != OperandKind::Temp || Src.Modifier ||
          Src.RelativeRegister != UINT32_MAX || Stats.Count[Src.Index] != 1)
        continue;
      const auto &Def = Defs[Src.Index];
      if (!Def.Valid() || Def.IsPhi || Def.IsAux)
        continue;
      auto &Prod = Ssa.Blocks[Def.Block].Insts[Def.Index];
      if (Prod.Dead || IsPseudoOpcode(Prod.Opcode) || !Prod.IsPure() ||
          Prod.HasAuxDest() || Prod.Dest.Kind != OperandKind::Temp ||
          Prod.Dest.Index != Src.Index)
        continue;
      if (Mov.Saturate && !InstructionSupportsSaturate(Prod.Opcode))
        continue;
      bool NeedsSwizzle = false;
      if (!OutputFoldShapeOk(Prod, Mov, NeedsSwizzle) ||
          !CrossBlockOutputRangeSafe(Ssa, Def.Block, Def.Index, B, I, Mov.Dest))
        continue;
      if (NeedsSwizzle) {
        for (auto &PS : Prod.Sources)
          if (PS.Kind != OperandKind::Immediate &&
              PS.Kind != OperandKind::ImmediateVector)
            PS.Swizzle = ComposeSwizzle(PS.Swizzle, Src.Swizzle);
      }
      Prod.Dest = Mov.Dest;
      if (Mov.Saturate)
        Prod.Saturate = true;
      Mov.Dead = true;
      Changed = true;
    }
  }
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}
