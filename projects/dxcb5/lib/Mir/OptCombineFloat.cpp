// Distributed under the University of Illinois Open Source License.
#include "Mir/OptAlgebraicCommon.h"
#include "llvm/ADT/SmallVector.h"
#include <array>

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

bool AllLanesInUnitRange(const SsaProgram &Ssa, const Operand &V, uint8_t Mask) {
  const auto &Arr = Ssa.Mir.LiteralVectors[V.Index];
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (Mask & (1u << L)) {
      const float F = BitsToFloat(Arr[GetSwizzleLane(V.Swizzle, L)]);
      if (!(F >= 0.0f && F <= 1.0f))
        return false;
    }
  }
  return true;
}

bool FoldSaturatePhi(AlgContext &Ctx, SsaInst &Inst, const SsaPhi &Phi) {
  if (Phi.Dead || Phi.Mask != Inst.Dest.Mask || Phi.Inputs.empty() ||
      !IsIdentitySwizzle(Inst.Sources[0].Swizzle, Inst.Dest.Mask))
    return false;
  llvm::SmallVector<std::pair<uint32_t, uint32_t>, 4> ToSat;
  for (const auto &In : Phi.Inputs) {
    const auto &V = In.Value;
    if (V.Modifier || V.RelativeRegister != UINT32_MAX)
      return false;
    if (V.Kind == OperandKind::Immediate) {
      const float F = BitsToFloat(V.Index);
      if (!(F >= 0.0f && F <= 1.0f))
        return false;
      continue;
    }
    if (V.Kind == OperandKind::ImmediateVector) {
      if (!AllLanesInUnitRange(Ctx.Ssa, V, Phi.Mask))
        return false;
      continue;
    }
    if (V.Kind != OperandKind::Temp)
      return false;
    const auto &ID = Ctx.Defs[V.Index];
    if (!ID.Valid() || ID.IsPhi || ID.IsAux)
      return false;
    const auto &P = Ctx.Ssa.Blocks[ID.Block].Insts[ID.Index];
    if (P.Dead || !P.IsPure() || P.Precise)
      return false;
    if (P.Saturate)
      continue;
    if (Ctx.Stats.Count[V.Index] != 1 || P.Dest.Mask != Phi.Mask ||
        !IsIdentitySwizzle(V.Swizzle, Phi.Mask) ||
        !InstructionSupportsSaturate(P.Opcode))
      return false;
    ToSat.push_back(std::make_pair(ID.Block, ID.Index));
  }
  for (const auto &Loc : ToSat)
    Ctx.Ssa.Blocks[Loc.first].Insts[Loc.second].Saturate = true;
  Inst.Saturate = false;
  return true;
}

bool IsKnownUnitInterval(const AlgContext &Ctx, const Operand &Op, uint8_t Mask,
                         unsigned Depth) {
  if (Depth > 4 || Op.Modifier != 0 || Op.RelativeRegister != UINT32_MAX)
    return false;
  if (Op.Kind == OperandKind::Immediate) {
    const float F = BitsToFloat(Op.Index);
    return F >= 0.0f && F <= 1.0f;
  }
  if (Op.Kind == OperandKind::ImmediateVector)
    return Op.Index < Ctx.Ssa.Mir.LiteralVectors.size() &&
           AllLanesInUnitRange(Ctx.Ssa, Op, Mask);
  if (Op.Kind != OperandKind::Temp || Op.Index >= Ctx.Defs.size())
    return false;
  const auto &D = Ctx.Defs[Op.Index];
  if (!D.Valid() || D.IsAux)
    return false;
  uint8_t InnerMask = 0;
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if (Mask & (1u << L))
      InnerMask |= static_cast<uint8_t>(1u << GetSwizzleLane(Op.Swizzle, L));
  if (D.IsPhi) {
    const auto &Phi = Ctx.Ssa.Blocks[D.Block].Phis[D.Index];
    if (Phi.Dead || (Phi.Mask & InnerMask) != InnerMask ||
        Phi.Inputs.empty() || Phi.Inputs.size() > 4)
      return false;
    for (const auto &In : Phi.Inputs)
      if (!IsKnownUnitInterval(Ctx, In.Value, InnerMask, Depth + 1))
        return false;
    return true;
  }
  const auto &P = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
  if (P.Dead || (P.Dest.Mask & InnerMask) != InnerMask)
    return false;
  if (P.Saturate)
    return true;
  if (P.Opcode == D3D10_SB_OPCODE_MOV && P.Sources.size() == 1)
    return IsKnownUnitInterval(Ctx, P.Sources[0], InnerMask, Depth + 1);
  if ((P.Opcode == D3D10_SB_OPCODE_MIN || P.Opcode == D3D10_SB_OPCODE_MAX ||
       P.Opcode == D3D10_SB_OPCODE_MUL) &&
      P.Sources.size() == 2)
    return IsKnownUnitInterval(Ctx, P.Sources[0], InnerMask, Depth + 1) &&
           IsKnownUnitInterval(Ctx, P.Sources[1], InnerMask, Depth + 1);
  if (P.Opcode == D3D10_SB_OPCODE_MOVC && P.Sources.size() == 3)
    return IsKnownUnitInterval(Ctx, P.Sources[1], InnerMask, Depth + 1) &&
           IsKnownUnitInterval(Ctx, P.Sources[2], InnerMask, Depth + 1);
  return false;
}

bool SimplifyMovSatInst(AlgContext &Ctx, uint32_t B, SsaInst &Inst) {
  if (!Inst.Saturate)
    return false;
  const auto &Src = Inst.Sources[0];
  if (Src.Kind == OperandKind::Immediate && !Src.Modifier) {
    const float V = BitsToFloat(Src.Index);
    if (!std::isfinite(V))
      return false;
    const float Clamped = V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V);
    Inst.Saturate = false;
    Inst.Sources[0] = MakeImm(FloatToBits(Clamped));
    return true;
  }
  if (!Inst.Precise && IsKnownUnitInterval(Ctx, Src, Inst.Dest.Mask, 0)) {
    Inst.Saturate = false;
    return true;
  }
  if (Src.Kind != OperandKind::Temp || Src.Modifier ||
      Src.RelativeRegister != UINT32_MAX)
    return false;
  const auto &Def = Ctx.Defs[Src.Index];
  if (!Def.Valid() || Def.IsAux)
    return false;
  if (Def.IsPhi) {
    if (Ctx.Stats.Count[Src.Index] != 1 || Inst.Precise)
      return false;
    return FoldSaturatePhi(Ctx, Inst, Ctx.Ssa.Blocks[Def.Block].Phis[Def.Index]);
  }
  auto &Prod = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
  if (Prod.Dead)
    return false;
  if (Prod.Saturate) {
    bool AllInProd = true;
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((Inst.Dest.Mask & (1u << L)) &&
          !(Prod.Dest.Mask & (1u << GetSwizzleLane(Src.Swizzle, L))))
        AllInProd = false;
    if (AllInProd) {
      Inst.Saturate = false;
      return true;
    }
  }
  if ((Def.Block == B || !Inst.Precise) && Ctx.Stats.Count[Src.Index] == 1 &&
      Prod.IsPure() && InstructionSupportsSaturate(Prod.Opcode) &&
      Prod.Dest.Mask == Inst.Dest.Mask &&
      IsIdentitySwizzle(Src.Swizzle, Inst.Dest.Mask) &&
      (Def.Block == B || !Prod.Precise)) {
    Prod.Saturate = true;
    Inst.Saturate = false;
    return true;
  }
  return false;
}

bool EvalUnitChoice(const AlgContext &Ctx, const Operand &Unit,
                    const Operand &Y, uint8_t Mask, bool StrictNan,
                    Operand &Out) {
  if (Unit.Kind == OperandKind::Immediate && Unit.Index == kPosZeroBits &&
      (!StrictNan || IsKnownFiniteNonNegFloat(Ctx, Y, Mask))) {
    Out = MakeImm(kPosZeroBits);
    return true;
  }
  bool Neg = false;
  if (IsOneFloatImm(Unit, Neg)) {
    Out = Neg ? NegateMirOperand(Ctx.Ssa, Y) : Y;
    return true;
  }
  return false;
}

bool TryFoldMulTwoLevelSign(AlgContext &Ctx, uint32_t B, uint32_t &I,
                            SsaInst &Sel, const Operand &Cond,
                            const Operand &Y) {
  auto &Block = Ctx.Ssa.Blocks[B];
  const uint8_t DMask = Block.Insts[I].Dest.Mask;
  const bool StrictNan = Ctx.Ssa.NanSafe || Block.Insts[I].Precise;
  for (unsigned Arm = 1; Arm <= 2; ++Arm) {
    Operand DirectArm;
    if (!EvalUnitChoice(Ctx, Sel.Sources[Arm], Y, DMask, StrictNan, DirectArm))
      continue;
    const auto &SubOp = Sel.Sources[3 - Arm];
    if (SubOp.Kind != OperandKind::Temp || SubOp.Modifier ||
        SubOp.RelativeRegister != UINT32_MAX ||
        Ctx.Stats.Count[SubOp.Index] != 1)
      continue;
    const auto &SubD = Ctx.Defs[SubOp.Index];
    if (!SubD.Valid() || SubD.IsPhi || SubD.IsAux)
      continue;
    auto &Sub = Ctx.Ssa.Blocks[SubD.Block].Insts[SubD.Index];
    if (Sub.Dead || Sub.Opcode != D3D10_SB_OPCODE_MOVC || Sub.Saturate ||
        Sub.Dest.Mask != DMask || !IsIdentitySwizzle(SubOp.Swizzle, DMask))
      continue;
    Operand ST, SF;
    if (!EvalUnitChoice(Ctx, Sub.Sources[1], Y, DMask, StrictNan, ST) ||
        !EvalUnitChoice(Ctx, Sub.Sources[2], Y, DMask, StrictNan, SF))
      continue;
    const uint32_t TmpId = Ctx.Ssa.AllocValue();
    const uint8_t TmpSwz = PopcountMask(DMask) == 1
                               ? BroadcastLaneSwizzle(FirstActiveLane(DMask))
                               : kIdentitySwizzle;
    const Operand TmpDest = MakeSsaTemp(TmpId, DMask, TmpSwz);
    SsaInst NewSub;
    NewSub.Opcode = D3D10_SB_OPCODE_MOVC;
    NewSub.Dest = TmpDest;
    NewSub.Sources.assign({Sub.Sources[0], ST, SF});
    NewSub.Precise = Block.Insts[I].Precise;

    auto &Inst = Block.Insts[I];
    Inst.Opcode = D3D10_SB_OPCODE_MOVC;
    Inst.Sources.clear();
    Inst.Sources.push_back(Cond);
    Inst.Sources.push_back(Arm == 1 ? DirectArm : TmpDest);
    Inst.Sources.push_back(Arm == 1 ? TmpDest : DirectArm);
    Sel.Dead = true;
    Sub.Dead = true;
    Block.Insts.insert(Block.Insts.begin() + I, std::move(NewSub));
    ++I;
    Ctx.RefreshBlockAfterInsert(B, I - 1);
    Ctx.Stats.Count[TmpId] = 1;
    return true;
  }
  return false;
}

bool TryFoldMulUnitMovc(AlgContext &Ctx, uint32_t B, uint32_t &I) {
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &SelOp = Inst.Sources[Side];
    const auto &Y = Inst.Sources[1 - Side];
    if (SelOp.Kind != OperandKind::Temp || SelOp.Modifier ||
        SelOp.RelativeRegister != UINT32_MAX ||
        Y.RelativeRegister != UINT32_MAX || Ctx.Stats.Count[SelOp.Index] != 1)
      continue;
    const auto &SD = Ctx.Defs[SelOp.Index];
    if (!SD.Valid() || SD.IsPhi || SD.IsAux)
      continue;
    auto &Sel = Ctx.Ssa.Blocks[SD.Block].Insts[SD.Index];
    if (Sel.Dead || Sel.Opcode != D3D10_SB_OPCODE_MOVC || Sel.Saturate)
      continue;
    const bool ScalarBroadcast = PopcountMask(Sel.Dest.Mask) == 1 &&
                                 GetSwizzleLane(SelOp.Swizzle, 0) == 0 &&
                                 SelOp.Swizzle == 0;
    const bool MaskOk =
        ScalarBroadcast || (Sel.Dest.Mask == Inst.Dest.Mask &&
                            IsIdentitySwizzle(SelOp.Swizzle, Inst.Dest.Mask));
    if (!MaskOk)
      continue;
    Operand Cond = Sel.Sources[0];
    if (ScalarBroadcast && Cond.Kind != OperandKind::Immediate &&
        Cond.Kind != OperandKind::ImmediateVector)
      Cond.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(Cond.Swizzle, 0));
    Operand TOut, FOut;
    const bool StrictNan = Ctx.Ssa.NanSafe || Inst.Precise;
    if (EvalUnitChoice(Ctx, Sel.Sources[1], Y, Inst.Dest.Mask, StrictNan, TOut) &&
        EvalUnitChoice(Ctx, Sel.Sources[2], Y, Inst.Dest.Mask, StrictNan, FOut)) {
      Inst.Opcode = D3D10_SB_OPCODE_MOVC;
      Inst.Sources.assign({Cond, TOut, FOut});
      Sel.Dead = true;
      return true;
    }
    if (!ScalarBroadcast && TryFoldMulTwoLevelSign(Ctx, B, I, Sel, Cond, Y))
      return true;
  }
  return false;
}

bool TryDistributeMulAddConst(AlgContext &Ctx, SsaInst &Inst) {
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &COp = Inst.Sources[Side];
    const auto &AOp = Inst.Sources[1 - Side];
    if (COp.Kind != OperandKind::Immediate || COp.Modifier ||
        AOp.Kind != OperandKind::Temp || AOp.RelativeRegister != UINT32_MAX ||
        (AOp.Modifier != 0 && AOp.Modifier != D3D10_SB_OPERAND_MODIFIER_NEG) ||
        Ctx.Stats.Count[AOp.Index] != 1)
      continue;
    const auto &Def = Ctx.Defs[AOp.Index];
    if (!Def.Valid() || Def.IsPhi || Def.IsAux)
      continue;
    auto &Add = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (Add.Dead || Add.Opcode != D3D10_SB_OPCODE_ADD || Add.Precise ||
        Add.Saturate)
      continue;
    unsigned ImmSide = 2;
    for (unsigned K = 0; K < 2; ++K)
      if (Add.Sources[K].Kind == OperandKind::Immediate &&
          !Add.Sources[K].Modifier) {
        ImmSide = K;
        break;
      }
    if (ImmSide == 2)
      continue;
    bool MatchLanes = true;
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((Inst.Dest.Mask & (1u << L)) &&
          !(Add.Dest.Mask & (1u << GetSwizzleLane(AOp.Swizzle, L))))
        MatchLanes = false;
    if (!MatchLanes)
      continue;
    Operand X = Add.Sources[1 - ImmSide];
    const bool NeedsSwz =
        AOp.Swizzle != kIdentitySwizzle &&
        !(PopcountMask(Inst.Dest.Mask) == 1 &&
          PopcountMask(Add.Dest.Mask) == 1 && AOp.Swizzle == 0);
    if (NeedsSwz && X.Kind == OperandKind::ImmediateVector)
      continue;
    float C2 = BitsToFloat(COp.Index);
    if (AOp.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
      C2 = -C2;
    const float ProdConst = BitsToFloat(Add.Sources[ImmSide].Index) * C2;
    if (!std::isfinite(ProdConst))
      continue;
    if (NeedsSwz && X.Kind != OperandKind::Immediate)
      X.Swizzle = ComposeSwizzle(X.Swizzle, AOp.Swizzle);
    Inst.Opcode = D3D10_SB_OPCODE_MAD;
    Inst.Sources.assign(
        {X, MakeImm(FloatToBits(C2)), MakeImm(FloatToBits(ProdConst))});
    Add.Dead = true;
    return true;
  }
  return false;
}

bool TryExtractCombineNegMul(const SsaProgram &Ssa, const SsaInst &Comb,
                             const Operand &VOp, const Operand &COp,
                             uint8_t Mask, Operand &OutRaw,
                             std::array<uint32_t, 4> &NewBits) {
  bool HasNeg = false, FirstSet = false;
  uint8_t NewSwz = 0, FirstLaneSrc = 0;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(Mask & (1u << L)))
      continue;
    const unsigned CL = GetSwizzleLane(VOp.Swizzle, L);
    if (CL >= Comb.Sources.size())
      return false;
    const Operand &LaneOp = Comb.Sources[CL];
    if (LaneOp.Modifier != 0 &&
        LaneOp.Modifier != D3D10_SB_OPERAND_MODIFIER_NEG)
      return false;
    const bool IsNeg = (LaneOp.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG);
    HasNeg |= IsNeg;
    const uint32_t CBits =
        COp.Kind == OperandKind::Immediate
            ? COp.Index
            : Ssa.Mir.LiteralVectors[COp.Index][GetSwizzleLane(COp.Swizzle, L)];
    NewBits[L] = IsNeg ? (CBits ^ 0x80000000u) : CBits;
    Operand Raw = LaneOp;
    Raw.Modifier = 0;
    if (Raw.Kind != OperandKind::Temp && Raw.Kind != OperandKind::Input &&
        Raw.Kind != OperandKind::ConstantBuffer)
      return false;
    if (!FirstSet) {
      OutRaw = Raw;
      FirstLaneSrc = GetSwizzleLane(Raw.Swizzle, 0);
      FirstSet = true;
    } else if (!SameOperandBase(OutRaw, Raw)) {
      return false;
    }
    NewSwz = SetSwizzleLane(NewSwz, L, GetSwizzleLane(Raw.Swizzle, 0));
  }
  if (!HasNeg || !FirstSet)
    return false;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(Mask & (1u << L))) {
      NewSwz = SetSwizzleLane(NewSwz, L, FirstLaneSrc);
      NewBits[L] = NewBits[0];
    }
  }
  OutRaw.Mask = Mask;
  OutRaw.Swizzle = NewSwz;
  return true;
}

bool TryPushCombineVecNegIntoMulConst(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources.size() != 2)
    return false;
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &VOp = Inst.Sources[Side];
    const auto &COp = Inst.Sources[1 - Side];
    if (VOp.Kind != OperandKind::Temp || VOp.Modifier ||
        VOp.RelativeRegister != UINT32_MAX || COp.Modifier ||
        (COp.Kind != OperandKind::Immediate &&
         COp.Kind != OperandKind::ImmediateVector))
      continue;
    const auto &VD = Ctx.Defs[VOp.Index];
    if (!VD.Valid() || VD.IsPhi || VD.IsAux)
      continue;
    const auto &Comb = Ctx.Ssa.Blocks[VD.Block].Insts[VD.Index];
    if (Comb.Dead || Comb.Opcode != PseudoCombineVec || Comb.Sources.size() < 2)
      continue;
    Operand FirstRaw;
    std::array<uint32_t, 4> NewBits{};
    if (!TryExtractCombineNegMul(Ctx.Ssa, Comb, VOp, COp, Inst.Dest.Mask,
                                 FirstRaw, NewBits))
      continue;
    Inst.Sources[Side] = FirstRaw;
    Inst.Sources[1 - Side] = {OperandKind::ImmediateVector,
                              Ctx.Ssa.InternLiteralVector(NewBits), 0,
                              Inst.Dest.Mask, kIdentitySwizzle};
    return true;
  }
  return false;
}

bool SimplifyMulInst(AlgContext &Ctx, uint32_t B, uint32_t &I) {
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  bool Changed = false;
  if (Inst.Sources.size() == 2)
    Changed |= CanonicalizeMulMadNegImm(Ctx.Ssa, Inst);
  if (Inst.Precise)
    return Changed;
  for (unsigned Side = 0; Side < 2; ++Side) {
    bool Neg = false;
    if (IsOneFloatImm(Inst.Sources[Side], Neg)) {
      Operand Other = Inst.Sources[1 - Side];
      if (Neg)
        Other = NegateMirOperand(Ctx.Ssa, Other);
      Inst.Opcode = D3D10_SB_OPCODE_MOV;
      Inst.Sources.assign({Other});
      return true;
    }
    if (IsZeroFloatImm(Inst.Sources[Side])) {
      Inst.Opcode = D3D10_SB_OPCODE_MOV;
      Inst.Sources.assign({MakeImm(kPosZeroBits)});
      return true;
    }
  }
  if (TryFoldMulUnitMovc(Ctx, B, I))
    return true;
  auto &Cur = Ctx.Ssa.Blocks[B].Insts[I];
  if (TryDistributeMulAddConst(Ctx, Cur))
    return true;
  Changed |= TryReassociateConstFloat(Ctx, Cur);
  if (Cur.Opcode == D3D10_SB_OPCODE_MUL)
    Changed |= TryPushCombineVecNegIntoMulConst(Ctx, Cur);
  return Changed;
}

} // namespace

bool dxcb5::alg::CanonicalizeMulMadNegImm(SsaProgram &Ssa, SsaInst &Inst) {
  for (unsigned Side = 0; Side < 2; ++Side) {
    auto &Var = Inst.Sources[Side];
    auto &Imm = Inst.Sources[1 - Side];
    if ((Var.Modifier != D3D10_SB_OPERAND_MODIFIER_NEG &&
         Var.Modifier != D3D10_SB_OPERAND_MODIFIER_ABSNEG) ||
        Imm.Modifier ||
        (Imm.Kind != OperandKind::Immediate &&
         Imm.Kind != OperandKind::ImmediateVector))
      continue;
    if (Imm.Kind == OperandKind::Immediate) {
      Imm.Index ^= 0x80000000u;
    } else {
      auto Bits = Ssa.Mir.LiteralVectors[Imm.Index];
      for (uint32_t &BVal : Bits)
        BVal ^= 0x80000000u;
      Imm.Index = Ssa.InternLiteralVector(Bits);
    }
    Var.Modifier = (Var.Modifier == D3D10_SB_OPERAND_MODIFIER_ABSNEG)
                       ? D3D10_SB_OPERAND_MODIFIER_ABS
                       : 0;
    return true;
  }
  return false;
}

bool dxcb5::alg::SimplifyFloatInst(AlgContext &Ctx, uint32_t B, uint32_t &I) {
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  switch (Inst.Opcode) {
  case D3D10_SB_OPCODE_MOV: return SimplifyMovSatInst(Ctx, B, Inst);
  case D3D10_SB_OPCODE_MUL: return SimplifyMulInst(Ctx, B, I);
  case D3D10_SB_OPCODE_ADD: return SimplifyAddInst(Ctx, B, Inst);
  case D3D10_SB_OPCODE_MAD: return SimplifyMadInst(Ctx, Inst);
  case D3D10_SB_OPCODE_MAX:
  case D3D10_SB_OPCODE_MIN: return SimplifyMinMaxInst(Ctx, Inst);
  case D3D10_SB_OPCODE_EXP: return SimplifyExpInst(Ctx, Inst);
  case D3D10_SB_OPCODE_DIV: return SimplifyDivInst(Ctx, B, Inst);
  case D3D10_SB_OPCODE_RSQ:
  case D3D10_SB_OPCODE_SQRT:
  case D3D11_SB_OPCODE_RCP: {
    bool Neg = false;
    if (!Inst.Precise && IsOneFloatImm(Inst.Sources[0], Neg) && !Neg) {
      Inst.Opcode = D3D10_SB_OPCODE_MOV;
      Inst.Sources.assign({MakeImm(kPosOneBits)});
      return true;
    }
    return false;
  }
  default: return false;
  }
}
