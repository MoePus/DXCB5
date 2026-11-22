// Distributed under the University of Illinois Open Source License.
#include "Mir/Combine/CombineCommon.h"
#include "llvm/ADT/SmallVector.h"
#include <array>

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

bool FoldMovcCond(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources[0].Kind != OperandKind::Temp || Inst.Sources[0].Modifier ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX)
    return false;
  const auto &Def = Ctx.Defs[Inst.Sources[0].Index];
  if (!Def.Valid() || Def.IsPhi || Def.IsAux)
    return false;
  const auto &Prod = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
  if (Prod.Dead)
    return false;
  if (Prod.Opcode == D3D10_SB_OPCODE_NOT &&
      Prod.Sources[0].RelativeRegister == UINT32_MAX &&
      IsBoolMaskValue(Ctx.Ssa, Ctx.Defs, Prod.Sources[0])) {
    Operand Orig = Prod.Sources[0];
    Orig.Swizzle = ComposeSwizzle(Orig.Swizzle, Inst.Sources[0].Swizzle);
    Inst.Sources[0] = Orig;
    Ctx.BumpUse(Orig);
    std::swap(Inst.Sources[1], Inst.Sources[2]);
    return true;
  }
  if ((Prod.Opcode != D3D10_SB_OPCODE_INE &&
       Prod.Opcode != D3D10_SB_OPCODE_IEQ) ||
      Prod.Sources.size() != 2)
    return false;
  for (unsigned Side = 0; Side < 2; ++Side) {
    if (Prod.Sources[Side].Kind != OperandKind::Immediate ||
        Prod.Sources[Side].Index != 0u || Prod.Sources[Side].Modifier)
      continue;
    const auto &Raw = Prod.Sources[1 - Side];
    if (Raw.RelativeRegister != UINT32_MAX || Raw.Modifier ||
        (Raw.Kind != OperandKind::Temp && Raw.Kind != OperandKind::Input &&
         Raw.Kind != OperandKind::ConstantBuffer))
      continue;
    Operand Orig = Raw;
    Orig.Swizzle = ComposeSwizzle(Orig.Swizzle, Inst.Sources[0].Swizzle);
    Inst.Sources[0] = Orig;
    Ctx.BumpUse(Orig);
    if (Prod.Opcode == D3D10_SB_OPCODE_IEQ)
      std::swap(Inst.Sources[1], Inst.Sources[2]);
    return true;
  }
  return false;
}

bool TryNarrowVectorMovc(AlgContext &Ctx, uint32_t B, uint32_t &I) {
  auto &Block = Ctx.Ssa.Blocks[B];
  auto &Inst = Block.Insts[I];
  if (Inst.Saturate || Inst.Dest.Kind != OperandKind::Temp ||
      Inst.Sources.size() != 3 ||
      (Inst.Dest.Mask != 0x3u && Inst.Dest.Mask != 0x7u &&
       Inst.Dest.Mask != 0xfu) ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX ||
      Inst.Sources[1].RelativeRegister != UINT32_MAX ||
      Inst.Sources[2].RelativeRegister != UINT32_MAX)
    return false;
  const unsigned W = PopcountMask(Inst.Dest.Mask);
  std::array<Operand, 4> TLanes{}, FLanes{};
  uint8_t VaryMask = 0;
  unsigned VaryLane = 0;
  for (unsigned L = 0; L < W; ++L) {
    if (!TryResolveLane(Ctx, Inst.Sources[1], L, true, TLanes[L]) ||
        !TryResolveLane(Ctx, Inst.Sources[2], L, true, FLanes[L]))
      return false;
    if (!EqualCopySources(TLanes[L], FLanes[L])) {
      VaryMask |= static_cast<uint8_t>(1u << L);
      VaryLane = L;
    }
  }
  Operand CondV;
  if (PopcountMask(VaryMask) != 1 ||
      !TryResolveLane(Ctx, Inst.Sources[0], VaryLane, true, CondV) ||
      CondV.Modifier)
    return false;
  const uint32_t NarrowId = Ctx.Ssa.AllocValue();
  const Operand NarrowDst = MakeSsaTemp(NarrowId, 1u, 0);
  SsaInst Narrow;
  Narrow.Opcode = D3D10_SB_OPCODE_MOVC;
  Narrow.Dest = NarrowDst;
  Narrow.Sources.assign({CondV, TLanes[VaryLane], FLanes[VaryLane]});
  Narrow.Precise = Inst.Precise;

  Inst.Opcode = PseudoCombineVec;
  Inst.Sources.clear();
  for (unsigned L = 0; L < W; ++L)
    Inst.Sources.push_back(L == VaryLane ? NarrowDst : TLanes[L]);
  Block.Insts.insert(Block.Insts.begin() + I, std::move(Narrow));
  ++I;
  Ctx.RefreshBlockAfterInsert(B, I - 1);
  Ctx.Stats.Count[NarrowId] = 1;
  return true;
}

bool IsPowAbsZeroAtZero(const AlgContext &Ctx, const Operand &YVal,
                        const Operand &XVal) {
  if (YVal.Kind != OperandKind::Temp || YVal.Modifier ||
      YVal.RelativeRegister != UINT32_MAX)
    return false;
  const auto &YD = Ctx.Defs[YVal.Index];
  if (!YD.Valid() || YD.IsPhi || YD.IsAux)
    return false;
  const auto &YInst = Ctx.Ssa.Blocks[YD.Block].Insts[YD.Index];
  if (YInst.Dead || YInst.Opcode != D3D10_SB_OPCODE_EXP ||
      YInst.Sources.size() != 1 ||
      YInst.Sources[0].Kind != OperandKind::Temp || YInst.Sources[0].Modifier)
    return false;
  const auto &MD = Ctx.Defs[YInst.Sources[0].Index];
  if (!MD.Valid() || MD.IsPhi || MD.IsAux)
    return false;
  const auto &MInst = Ctx.Ssa.Blocks[MD.Block].Insts[MD.Index];
  if (MInst.Dead || MInst.Opcode != D3D10_SB_OPCODE_MUL ||
      MInst.Sources.size() != 2)
    return false;
  for (unsigned K = 0; K < 2; ++K) {
    if (MInst.Sources[K].Kind != OperandKind::Immediate ||
        MInst.Sources[K].Modifier || BitsToFloat(MInst.Sources[K].Index) <= 0.0f ||
        MInst.Sources[1 - K].Kind != OperandKind::Temp ||
        MInst.Sources[1 - K].Modifier)
      continue;
    const auto &LD = Ctx.Defs[MInst.Sources[1 - K].Index];
    if (!LD.Valid() || LD.IsPhi || LD.IsAux)
      continue;
    const auto &LInst = Ctx.Ssa.Blocks[LD.Block].Insts[LD.Index];
    if (!LInst.Dead && LInst.Opcode == D3D10_SB_OPCODE_LOG &&
        LInst.Sources.size() == 1 &&
        EqualCopySources(LInst.Sources[0], AbsMirOperand(XVal)))
      return true;
  }
  return false;
}

bool TryCollapseSignCbrtMovc(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Precise || Ctx.Ssa.NanSafe ||
      Inst.Sources[0].Kind != OperandKind::Temp ||
      Inst.Sources[0].Modifier ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX)
    return false;
  const auto &PD = Ctx.Defs[Inst.Sources[0].Index];
  if (!PD.Valid() || PD.IsPhi || PD.IsAux)
    return false;
  const auto &PosCmp = Ctx.Ssa.Blocks[PD.Block].Insts[PD.Index];
  if (PosCmp.Dead || PosCmp.Opcode != D3D10_SB_OPCODE_LT ||
      PosCmp.Dest.Mask != Inst.Dest.Mask || !IsZeroFloatImm(PosCmp.Sources[0]) ||
      !IsIdentitySwizzle(Inst.Sources[0].Swizzle, Inst.Dest.Mask))
    return false;
  const Operand &XVal = PosCmp.Sources[1];
  const Operand &YVal = Inst.Sources[1];
  // When X >= 0 is proven, movc(0 < X, exp(log(|X|) * k), 0) == Y everywhere
  // (at X == 0, exp(-inf * k) == 0 for k > 0).
  if (IsZeroFloatImm(Inst.Sources[2]) &&
      IsKnownNonNegativeFloat(Ctx, XVal, Inst.Dest.Mask, /*AllowNegZero=*/true) &&
      IsPowAbsZeroAtZero(Ctx, YVal, XVal)) {
    Operand Keep = YVal;
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Sources.assign({Keep});
    return true;
  }
  if (Inst.Sources[2].Kind != OperandKind::Temp || Inst.Sources[2].Modifier ||
      Inst.Sources[2].RelativeRegister != UINT32_MAX ||
      Ctx.Stats.Count[Inst.Sources[2].Index] != 1)
    return false;
  const auto &SD = Ctx.Defs[Inst.Sources[2].Index];
  if (!SD.Valid() || SD.IsPhi || SD.IsAux)
    return false;
  auto &SubSel = Ctx.Ssa.Blocks[SD.Block].Insts[SD.Index];
  if (SubSel.Dead || SubSel.Opcode != D3D10_SB_OPCODE_MOVC || SubSel.Saturate ||
      SubSel.Dest.Mask != Inst.Dest.Mask || !IsZeroFloatImm(SubSel.Sources[2]) ||
      SubSel.Sources[0].Kind != OperandKind::Temp ||
      SubSel.Sources[0].Modifier ||
      SubSel.Sources[0].RelativeRegister != UINT32_MAX)
    return false;
  const auto &ND = Ctx.Defs[SubSel.Sources[0].Index];
  if (!ND.Valid() || ND.IsPhi || ND.IsAux)
    return false;
  const auto &NegCmp = Ctx.Ssa.Blocks[ND.Block].Insts[ND.Index];
  if (!IsIdentitySwizzle(Inst.Sources[2].Swizzle, Inst.Dest.Mask) ||
      !IsIdentitySwizzle(SubSel.Sources[0].Swizzle, Inst.Dest.Mask) ||
      NegCmp.Dead || NegCmp.Opcode != D3D10_SB_OPCODE_LT ||
      NegCmp.Dest.Mask != Inst.Dest.Mask || !IsZeroFloatImm(NegCmp.Sources[1]) ||
      !EqualCopySources(NegCmp.Sources[0], XVal) ||
      !EqualCopySources(SubSel.Sources[1], NegateMirOperand(YVal)) ||
      !IsPowAbsZeroAtZero(Ctx, YVal, XVal))
    return false;
  Operand NegCond = SubSel.Sources[0], NegY = SubSel.Sources[1], PosY = YVal;
  Inst.Sources.assign({NegCond, NegY, PosY});
  SubSel.Dead = true;
  return true;
}

bool TryFoldMovcToMinMax(const AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources[0].Kind != OperandKind::Temp || Inst.Sources[0].Modifier ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX)
    return false;
  const auto &CD = Ctx.Defs[Inst.Sources[0].Index];
  if (!CD.Valid() || CD.IsPhi || CD.IsAux)
    return false;
  const auto &Cmp = Ctx.Ssa.Blocks[CD.Block].Insts[CD.Index];
  if (Cmp.Dead || Cmp.Sources.size() != 2 || Cmp.Dest.Mask != Inst.Dest.Mask ||
      (Inst.Sources[0].Swizzle != kIdentitySwizzle &&
       !(PopcountMask(Inst.Dest.Mask) == 1 && Inst.Sources[0].Swizzle == 0)))
    return false;
  // Table generated from Lean 4 `DXCB5.RewriteRules.movcMinMaxRules`.
  uint32_t DirectOp = 0, SwappedOp = 0;
  const bool AnyPrecise = Inst.Precise || Cmp.Precise;
  const bool NonNaNF =
      !Ctx.Ssa.NanSafe || IsKnownNonNaNFloat(Ctx, Inst.Sources[2], Inst.Dest.Mask);
  switch (Cmp.Opcode) {
#define DXCB5_MOVC_MINMAX_RULE(CMP_OP, DIR_OP, SWP_OP, IS_FLT, REQ)           \
  case CMP_OP:                                                                \
    if ((IS_FLT || !Inst.Saturate) &&                                         \
        SatisfiesSemMode(SemModeReq::REQ, Ctx.Ssa.NanSafe, AnyPrecise,        \
                         NonNaNF)) {                                          \
      DirectOp = DIR_OP;                                                      \
      SwappedOp = SWP_OP;                                                     \
    }                                                                         \
    break;
#include "Mir/Combine/GeneratedRules.inc"
#undef DXCB5_MOVC_MINMAX_RULE
  default: return false;
  }
  if (!DirectOp)
    return false;
  const bool MatchDirect =
      EqualCopySources(Cmp.Sources[0], Inst.Sources[1]) &&
      EqualCopySources(Cmp.Sources[1], Inst.Sources[2]);
  const bool MatchSwap =
      !MatchDirect && EqualCopySources(Cmp.Sources[0], Inst.Sources[2]) &&
      EqualCopySources(Cmp.Sources[1], Inst.Sources[1]);
  if (!MatchDirect && !MatchSwap)
    return false;
  Inst.Opcode = MatchDirect ? DirectOp : SwappedOp;
  Inst.Sources.assign({Cmp.Sources[0], Cmp.Sources[1]});
  return true;
}

bool SimplifyMovcInst(AlgContext &Ctx, uint32_t B, uint32_t &I) {
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  bool Changed = FoldMovcCond(Ctx, Inst);
  Operand Inner;
  bool KnownMask = false;
  if (MatchZextBool(Ctx, Inst.Sources[0], Inner, KnownMask) && KnownMask) {
    Inst.Sources[0] = Inner;
    Ctx.BumpUse(Inner);
    Changed = true;
  }
  if (EqualCopySources(Inst.Sources[1], Inst.Sources[2])) {
    Operand Same = Inst.Sources[1];
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Sources.assign({Same});
    return true;
  }
  if (Inst.Sources[0].Kind == OperandKind::Immediate) {
    Operand Pick = Inst.Sources[0].Index ? Inst.Sources[1] : Inst.Sources[2];
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Sources.assign({Pick});
    return true;
  }
  if (!Inst.Saturate && Inst.Sources[1].Kind == OperandKind::Immediate &&
      Inst.Sources[2].Kind == OperandKind::Immediate &&
      !Inst.Sources[1].Modifier && !Inst.Sources[2].Modifier &&
      ((Inst.Sources[1].Index == UINT32_MAX && Inst.Sources[2].Index == 0u) ||
       (Inst.Sources[1].Index == 0u && Inst.Sources[2].Index == UINT32_MAX)) &&
      IsBoolMaskValue(Ctx.Ssa, Ctx.Defs, Inst.Sources[0])) {
    Operand Cond = Inst.Sources[0];
    Inst.Opcode = (Inst.Sources[1].Index == UINT32_MAX) ? D3D10_SB_OPCODE_MOV
                                                        : D3D10_SB_OPCODE_NOT;
    Inst.Sources.assign({Cond});
    return true;
  }
  if (TryNarrowVectorMovc(Ctx, B, I))
    return true;
  auto &Cur = Ctx.Ssa.Blocks[B].Insts[I];
  if (TryCollapseSignCbrtMovc(Ctx, Cur))
    return true;
  if (TryFoldMovcToMinMax(Ctx, Cur))
    return true;
  return Changed;
}

bool IsFullGatherCombine(const AlgContext &Ctx, const Operand &Op, unsigned W,
                         std::array<Operand, 4> &OutLanes) {
  if (Op.Kind != OperandKind::Temp || Op.RelativeRegister != UINT32_MAX ||
      (Op.Modifier != 0 && Op.Modifier != D3D10_SB_OPERAND_MODIFIER_NEG) ||
      Ctx.Stats.Count[Op.Index] != 1)
    return false;
  const auto &D = Ctx.Defs[Op.Index];
  if (!D.Valid() || D.IsPhi || D.IsAux)
    return false;
  const auto &C = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
  if (C.Dead || C.Opcode != PseudoCombineVec || C.Sources.size() < W)
    return false;
  unsigned DistinctBases = 0;
  std::array<Operand, 4> SeenBases{};
  for (unsigned L = 0; L < W; ++L) {
    const unsigned K = GetSwizzleLane(Op.Swizzle, L);
    if (K >= C.Sources.size())
      return false;
    Operand LaneOp = C.Sources[K];
    if (LaneOp.RelativeRegister != UINT32_MAX ||
        LaneOp.Kind == OperandKind::Immediate)
      return false;
    if (Op.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
      LaneOp = NegateMirOperand(LaneOp);
    OutLanes[L] = LaneOp;
    if (LaneOp.Kind == OperandKind::Temp &&
        Ctx.Stats.Count[LaneOp.Index] == 1 && Ctx.Defs[LaneOp.Index].Valid()) {
      const auto &LD = Ctx.Defs[LaneOp.Index];
      const uint8_t DMask =
          LD.IsPhi ? Ctx.Ssa.Blocks[LD.Block].Phis[LD.Index].Mask
          : LD.IsAux ? Ctx.Ssa.Blocks[LD.Block].Insts[LD.Index].AuxDest.Mask
                     : Ctx.Ssa.Blocks[LD.Block].Insts[LD.Index].Dest.Mask;
      if (PopcountMask(DMask) == 1)
        return false;
    }
    bool Found = false;
    for (unsigned J = 0; J < DistinctBases; ++J)
      if (SameOperandBase(SeenBases[J], LaneOp))
        Found = true;
    if (!Found)
      SeenBases[DistinctBases++] = LaneOp;
  }
  return DistinctBases == W;
}

bool TryUnpackGatherDot(AlgContext &Ctx, uint32_t B, uint32_t &I) {
  auto &Block = Ctx.Ssa.Blocks[B];
  auto &Inst = Block.Insts[I];
  if (Inst.Precise ||
      (Inst.Opcode != D3D10_SB_OPCODE_DP3 &&
       Inst.Opcode != D3D10_SB_OPCODE_DP4) ||
      Inst.Dest.Kind != OperandKind::Temp ||
      PopcountMask(Inst.Dest.Mask) != 1 || Inst.Sources.size() != 2)
    return false;
  const unsigned W = (Inst.Opcode == D3D10_SB_OPCODE_DP4) ? 4u : 3u;
  std::array<Operand, 4> ALanes{}, BLanes{};
  bool Unpack = false;
  if (IsFullGatherCombine(Ctx, Inst.Sources[0], W, ALanes)) {
    Unpack = true;
    for (unsigned L = 0; L < W && Unpack; ++L)
      Unpack = TryResolveLane(Ctx, Inst.Sources[1], L, false, BLanes[L]);
  } else if (IsFullGatherCombine(Ctx, Inst.Sources[1], W, BLanes)) {
    Unpack = true;
    for (unsigned L = 0; L < W && Unpack; ++L)
      Unpack = TryResolveLane(Ctx, Inst.Sources[0], L, false, ALanes[L]);
  }
  if (!Unpack)
    return false;
  llvm::SmallVector<SsaInst, 3> Prefix;
  Operand PrevOp = MakeSsaTemp(Ctx.Ssa.AllocValue(), 1u, 0);
  SsaInst Mul0;
  Mul0.Opcode = D3D10_SB_OPCODE_MUL;
  Mul0.Dest = PrevOp;
  Mul0.Sources.assign({ALanes[0], BLanes[0]});
  Prefix.push_back(std::move(Mul0));
  for (unsigned L = 1; L + 1 < W; ++L) {
    const Operand NextOp = MakeSsaTemp(Ctx.Ssa.AllocValue(), 1u, 0);
    SsaInst Mad;
    Mad.Opcode = D3D10_SB_OPCODE_MAD;
    Mad.Dest = NextOp;
    Mad.Sources.assign({ALanes[L], BLanes[L], PrevOp});
    Prefix.push_back(std::move(Mad));
    PrevOp = NextOp;
  }
  Inst.Opcode = D3D10_SB_OPCODE_MAD;
  Inst.Sources.assign({ALanes[W - 1], BLanes[W - 1], PrevOp});
  const uint32_t InsCount = static_cast<uint32_t>(Prefix.size());
  Block.Insts.insert(Block.Insts.begin() + I,
                     std::make_move_iterator(Prefix.begin()),
                     std::make_move_iterator(Prefix.end()));
  I += InsCount;
  Ctx.RefreshBlockAfterInsert(B, I - InsCount);
  return true;
}

} // namespace

bool dxcb5::alg::SimplifySelectAndDotInst(AlgContext &Ctx, uint32_t B,
                                          uint32_t &I) {
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  if (Inst.Opcode == D3D10_SB_OPCODE_MOVC)
    return SimplifyMovcInst(Ctx, B, I);
  if (Inst.Opcode == D3D10_SB_OPCODE_DP2 ||
      Inst.Opcode == D3D10_SB_OPCODE_DP3 ||
      Inst.Opcode == D3D10_SB_OPCODE_DP4)
    return TryUnpackGatherDot(Ctx, B, I);
  return false;
}
