// Distributed under the University of Illinois Open Source License.
#include "Mir/Combine/CombineCommon.h"

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

bool TryContractAddMul(AlgContext &Ctx, SsaInst &Inst) {
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &Op = Inst.Sources[Side];
    if (Op.Kind != OperandKind::Temp || Op.RelativeRegister != UINT32_MAX ||
        (Op.Modifier != 0 && Op.Modifier != D3D10_SB_OPERAND_MODIFIER_NEG) ||
        Ctx.Stats.Count[Op.Index] != 1)
      continue;
    const auto &Def = Ctx.Defs[Op.Index];
    if (!Def.Valid() || Def.IsPhi || Def.IsAux)
      continue;
    auto &Prod = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (Prod.Dead || Prod.Opcode != D3D10_SB_OPCODE_MUL || Prod.Precise ||
        Prod.Saturate)
      continue;
    bool MatchLanes = true;
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((Inst.Dest.Mask & (1u << L)) &&
          !(Prod.Dest.Mask & (1u << GetSwizzleLane(Op.Swizzle, L))))
        MatchLanes = false;
    if (!MatchLanes)
      continue;
    Operand A = Prod.Sources[0], C = Prod.Sources[1];
    const bool NeedsSwz =
        Op.Swizzle != kIdentitySwizzle &&
        !(PopcountMask(Inst.Dest.Mask) == 1 &&
          PopcountMask(Prod.Dest.Mask) == 1 && Op.Swizzle == 0);
    if (NeedsSwz && (A.Kind == OperandKind::ImmediateVector ||
                     C.Kind == OperandKind::ImmediateVector))
      continue;
    if (NeedsSwz) {
      if (A.Kind != OperandKind::Immediate)
        A.Swizzle = ComposeSwizzle(A.Swizzle, Op.Swizzle);
      if (C.Kind != OperandKind::Immediate)
        C.Swizzle = ComposeSwizzle(C.Swizzle, Op.Swizzle);
    }
    if (Op.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
      A = NegateMirOperand(Ctx.Ssa, A);
    Operand Addend = Inst.Sources[1 - Side];
    Inst.Opcode = D3D10_SB_OPCODE_MAD;
    Inst.Sources.assign({A, C, Addend});
    Prod.Dead = true;
    return true;
  }
  return false;
}

uint32_t FindMadChainBaseMul(const AlgContext &Ctx, uint32_t B,
                             uint32_t TopIdx, uint8_t DestMask) {
  uint32_t CurIdx = TopIdx;
  for (unsigned Step = 0; Step < 3; ++Step) {
    const auto &AddOp = Ctx.Ssa.Blocks[B].Insts[CurIdx].Sources[2];
    if (AddOp.Kind != OperandKind::Temp || AddOp.Modifier ||
        AddOp.RelativeRegister != UINT32_MAX ||
        Ctx.Stats.Count[AddOp.Index] != 1 ||
        !IsIdentitySwizzle(AddOp.Swizzle, DestMask))
      return UINT32_MAX;
    const auto &AD = Ctx.Defs[AddOp.Index];
    if (!AD.Valid() || AD.IsPhi || AD.IsAux || AD.Block != B)
      return UINT32_MAX;
    const auto &Next = Ctx.Ssa.Blocks[B].Insts[AD.Index];
    if (Next.Dead || Next.Precise || Next.Saturate ||
        Next.Dest.Mask != DestMask)
      return UINT32_MAX;
    if (Next.Opcode == D3D10_SB_OPCODE_MUL)
      return AD.Index;
    if (Next.Opcode != D3D10_SB_OPCODE_MAD)
      return UINT32_MAX;
    CurIdx = AD.Index;
  }
  return UINT32_MAX;
}

bool TryFoldAddIntoMadChainBase(AlgContext &Ctx, uint32_t B, SsaInst &Inst) {
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &ChainOp = Inst.Sources[Side];
    const auto &Extra = Inst.Sources[1 - Side];
    if (ChainOp.Kind != OperandKind::Temp || ChainOp.Modifier ||
        ChainOp.RelativeRegister != UINT32_MAX ||
        Extra.RelativeRegister != UINT32_MAX ||
        Ctx.Stats.Count[ChainOp.Index] != 1 ||
        !IsIdentitySwizzle(ChainOp.Swizzle, Inst.Dest.Mask))
      continue;
    const auto &TopDef = Ctx.Defs[ChainOp.Index];
    if (!TopDef.Valid() || TopDef.IsPhi || TopDef.IsAux || TopDef.Block != B)
      continue;
    const auto &TopInst = Ctx.Ssa.Blocks[B].Insts[TopDef.Index];
    if (TopInst.Dead || TopInst.Opcode != D3D10_SB_OPCODE_MAD ||
        TopInst.Precise || TopInst.Saturate ||
        TopInst.Dest.Mask != Inst.Dest.Mask)
      continue;
    const uint32_t BaseMulIdx =
        FindMadChainBaseMul(Ctx, B, TopDef.Index, Inst.Dest.Mask);
    if (BaseMulIdx == UINT32_MAX)
      continue;
    if (Extra.Kind == OperandKind::Temp) {
      const auto &ED = Ctx.Defs[Extra.Index];
      if (ED.Valid() && !ED.IsPhi && ED.Block == B && ED.Index >= BaseMulIdx)
        continue;
    }
    auto &BaseMul = Ctx.Ssa.Blocks[B].Insts[BaseMulIdx];
    BaseMul.Opcode = D3D10_SB_OPCODE_MAD;
    BaseMul.Sources.push_back(Extra);
    if (Inst.Saturate)
      Ctx.Ssa.Blocks[B].Insts[TopDef.Index].Saturate = true;
    Operand TopVal = ChainOp;
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Saturate = false;
    Inst.Sources.assign({TopVal});
    return true;
  }
  return false;
}

bool TryEmitFastCbrt(AlgContext &Ctx, uint32_t B, uint32_t &I, SsaInst &Mul,
                     SsaInst &Log, Operand X) {
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  if (X.RelativeRegister != UINT32_MAX ||
      X.Kind == OperandKind::ImmediateVector)
    return false;
  const uint8_t M = Inst.Dest.Mask;
  const uint8_t Swz = PopcountMask(M) == 1
                          ? BroadcastLaneSwizzle(FirstActiveLane(M))
                          : kIdentitySwizzle;
  auto MakeTmp = [&]() { return MakeSsaTemp(Ctx.Ssa.AllocValue(), M, Swz); };
  llvm::SmallVector<SsaInst, 10> Ins;
  auto AddOp = [&](uint32_t Opc, Operand Dst,
                   std::initializer_list<Operand> Srcs, bool Sat = false) {
    SsaInst S;
    S.Opcode = Opc;
    S.Dest = Dst;
    S.Sources.append(Srcs.begin(), Srcs.end());
    S.Saturate = Sat;
    Ins.push_back(std::move(S));
  };
  Operand XPos = X;
  if (X.Modifier != 0) {
    XPos = MakeTmp();
    AddOp(D3D10_SB_OPCODE_MOV, XPos, {X});
  }
  const Operand Hi = MakeTmp();
  {
    SsaInst Umul;
    Umul.Opcode = D3D10_SB_OPCODE_UMUL;
    Umul.Dest = Hi;
    Umul.AuxDest = Operand{OperandKind::Null};
    Umul.Sources = {XPos, MakeImm(0x55555556u)};
    Ins.push_back(std::move(Umul));
  }
  const Operand Y0 = MakeTmp();
  AddOp(D3D10_SB_OPCODE_IADD, Y0, {Hi, MakeImm(0x2a512000u)});
  const Operand Y2 = MakeTmp();
  AddOp(D3D10_SB_OPCODE_MUL, Y2, {Y0, Y0});
  const Operand RcpY2 = MakeTmp();
  AddOp(D3D11_SB_OPCODE_RCP, RcpY2, {Y2});
  const Operand XR = MakeTmp();
  AddOp(D3D10_SB_OPCODE_MUL, XR, {XPos, RcpY2});
  const Operand XR3 = MakeTmp();
  AddOp(D3D10_SB_OPCODE_MUL, XR3, {XR, MakeImm(FloatToBits(1.0f / 3.0f))});
  const Operand Y1 = MakeTmp();
  AddOp(D3D10_SB_OPCODE_MAD, Y1,
        {Y0, MakeImm(FloatToBits(2.0f / 3.0f)), XR3}, Inst.Saturate);
  const Operand IsZero = MakeTmp();
  AddOp(D3D10_SB_OPCODE_EQ, IsZero, {XPos, MakeImm(kPosZeroBits)});

  Inst.Opcode = D3D10_SB_OPCODE_MOVC;
  Inst.Saturate = false;
  Inst.Sources.assign({IsZero, MakeImm(kPosZeroBits), Y1});
  Mul.Dead = true;
  Log.Dead = true;

  const uint32_t OrigI = I;
  auto &Insts = Ctx.Ssa.Blocks[B].Insts;
  Insts.insert(Insts.begin() + I, Ins.begin(), Ins.end());
  I += static_cast<uint32_t>(Ins.size());
  Ctx.RefreshBlockAfterInsert(B, OrigI);
  Ctx.BumpUse(X);
  return true;
}

bool TryFoldPowConst(AlgContext &Ctx, uint32_t B, uint32_t &I, SsaInst &Mul,
                     SsaInst &Log, const Operand &LOp, float ExpC) {
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  const Operand X = Log.Sources[0];
  const bool IsFastCbrt =
      Inst.FastMath && std::fabs(ExpC - (1.0f / 3.0f)) < 1e-6f;
  const bool AllowNegZero = (ExpC == 2.0f || ExpC == 4.0f || IsFastCbrt);
  if (!IsKnownNonNegativeFloat(Ctx, X, Inst.Dest.Mask, AllowNegZero))
    return false;
  if (ExpC == 0.5f || ExpC == -0.5f || ExpC == -1.0f || ExpC == 1.0f ||
      ExpC == 2.0f) {
    if (ExpC == 2.0f) {
      Inst.Opcode = D3D10_SB_OPCODE_MUL;
      Inst.Sources.assign({X, X});
    } else {
      Inst.Opcode = ExpC == 0.5f    ? D3D10_SB_OPCODE_SQRT
                    : ExpC == -0.5f ? D3D10_SB_OPCODE_RSQ
                    : ExpC == -1.0f ? D3D11_SB_OPCODE_RCP
                                    : D3D10_SB_OPCODE_MOV;
      Inst.Sources.assign({X});
    }
    Mul.Dead = true;
    Log.Dead = true;
    return true;
  }
  if (ExpC == 0.25f || ExpC == -0.25f || ExpC == 4.0f) {
    if (ExpC == 4.0f) {
      Log.Opcode = D3D10_SB_OPCODE_MUL;
      Log.Sources.assign({X, X});
      Inst.Opcode = D3D10_SB_OPCODE_MUL;
      Inst.Sources.assign({LOp, LOp});
    } else {
      Log.Opcode = D3D10_SB_OPCODE_SQRT;
      Log.Sources.assign({X});
      Inst.Opcode = (ExpC == 0.25f) ? D3D10_SB_OPCODE_SQRT : D3D10_SB_OPCODE_RSQ;
      Inst.Sources.assign({LOp});
    }
    Mul.Dead = true;
    return true;
  }
  if (ExpC == 0.125f) {
    const Operand MOp = Inst.Sources[0];
    Log.Opcode = D3D10_SB_OPCODE_SQRT;
    Log.Sources.assign({X});
    Mul.Opcode = D3D10_SB_OPCODE_SQRT;
    Mul.Sources.assign({LOp});
    Inst.Opcode = D3D10_SB_OPCODE_SQRT;
    Inst.Sources.assign({MOp});
    return true;
  }
  if (IsFastCbrt)
    return TryEmitFastCbrt(Ctx, B, I, Mul, Log, X);
  return false;
}

} // namespace

bool dxcb5::alg::TryReassociateConstFloat(AlgContext &Ctx, SsaInst &Inst) {
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &COp = Inst.Sources[Side];
    const auto &POp = Inst.Sources[1 - Side];
    if (COp.Kind != OperandKind::Immediate || COp.Modifier ||
        POp.Kind != OperandKind::Temp || POp.Modifier ||
        POp.RelativeRegister != UINT32_MAX || Ctx.Stats.Count[POp.Index] != 1)
      continue;
    const auto &D = Ctx.Defs[POp.Index];
    if (!D.Valid() || D.IsPhi || D.IsAux)
      continue;
    auto &Inner = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
    if (Inner.Dead || Inner.Precise || Inner.Saturate ||
        Inner.Dest.Mask != Inst.Dest.Mask ||
        !IsIdentitySwizzle(POp.Swizzle, Inst.Dest.Mask))
      continue;
    const float C2 = BitsToFloat(COp.Index);
    if (Inner.Opcode == Inst.Opcode && Inner.Sources.size() == 2) {
      const unsigned InC =
          (Inner.Sources[0].Kind == OperandKind::Immediate &&
           !Inner.Sources[0].Modifier)
              ? 0u
          : (Inner.Sources[1].Kind == OperandKind::Immediate &&
             !Inner.Sources[1].Modifier)
              ? 1u
              : 2u;
      if (InC < 2) {
        const float C1 = BitsToFloat(Inner.Sources[InC].Index);
        const float Comb =
            Inst.Opcode == D3D10_SB_OPCODE_ADD ? (C1 + C2) : (C1 * C2);
        if (std::isfinite(Comb)) {
          Inst.Sources[0] = Inner.Sources[1 - InC];
          Inst.Sources[1] = MakeImm(FloatToBits(Comb));
          Inner.Dead = true;
          return true;
        }
      }
    } else if (Inst.Opcode == D3D10_SB_OPCODE_ADD &&
               Inner.Opcode == D3D10_SB_OPCODE_MAD &&
               Inner.Sources.size() == 3 &&
               Inner.Sources[2].Kind == OperandKind::Immediate &&
               !Inner.Sources[2].Modifier) {
      const float Comb = BitsToFloat(Inner.Sources[2].Index) + C2;
      if (std::isfinite(Comb)) {
        Inst.Opcode = D3D10_SB_OPCODE_MAD;
        Inst.Sources.resize(3);
        Inst.Sources[0] = Inner.Sources[0];
        Inst.Sources[1] = Inner.Sources[1];
        Inst.Sources[2] = MakeImm(FloatToBits(Comb));
        Inner.Dead = true;
        return true;
      }
    }
  }
  return false;
}

bool dxcb5::alg::SimplifyAddInst(AlgContext &Ctx, uint32_t B, SsaInst &Inst) {
  if (Inst.Precise)
    return false;
  for (unsigned Side = 0; Side < 2; ++Side) {
    if (IsZeroFloatImm(Inst.Sources[Side])) {
      Operand Other = Inst.Sources[1 - Side];
      Inst.Opcode = D3D10_SB_OPCODE_MOV;
      Inst.Sources.assign({Other});
      return true;
    }
  }
  bool Changed = TryReassociateConstFloat(Ctx, Inst);
  if (Inst.Opcode == D3D10_SB_OPCODE_ADD)
    Changed |= TryContractAddMul(Ctx, Inst);
  if (Inst.Opcode == D3D10_SB_OPCODE_ADD)
    Changed |= TryFoldAddIntoMadChainBase(Ctx, B, Inst);
  return Changed;
}

bool dxcb5::alg::SimplifyMadInst(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources.size() != 3)
    return false;
  bool Changed = CanonicalizeMulMadNegImm(Ctx.Ssa, Inst);
  if (Inst.Precise)
    return Changed;
  for (unsigned Side = 0; Side < 2; ++Side) {
    if (!Ctx.Ssa.NanSafe && IsZeroFloatImm(Inst.Sources[Side])) {
      Operand Addend = Inst.Sources[2];
      Inst.Opcode = D3D10_SB_OPCODE_MOV;
      Inst.Sources.assign({Addend});
      return true;
    }
    bool Neg = false;
    if (IsOneFloatImm(Inst.Sources[Side], Neg)) {
      Operand Other = Inst.Sources[1 - Side];
      if (Neg)
        Other = NegateMirOperand(Ctx.Ssa, Other);
      Operand Addend = Inst.Sources[2];
      Inst.Opcode = D3D10_SB_OPCODE_ADD;
      Inst.Sources.assign({Other, Addend});
      return true;
    }
  }
  if (IsZeroFloatImm(Inst.Sources[2])) {
    Operand A = Inst.Sources[0], C = Inst.Sources[1];
    Inst.Opcode = D3D10_SB_OPCODE_MUL;
    Inst.Sources.assign({A, C});
    return true;
  }
  return Changed;
}

bool TryFoldMinMaxPair(AlgContext &Ctx, SsaInst &Inst, const SsaInst &P,
                       bool OuterMax, uint32_t B2, float F2) {
  const bool InnerMax = (P.Opcode == D3D10_SB_OPCODE_MAX);
  for (unsigned K = 0; K < 2; ++K) {
    const auto &C1 = P.Sources[K];
    if (C1.Kind != OperandKind::Immediate)
      continue;
    const uint32_t B1 = ApplyFloatModToBits(C1.Index, C1.Modifier);
    const float F1 = BitsToFloat(B1);
    if (std::isnan(F1))
      continue;
    if (OuterMax != InnerMax) {
      if ((OuterMax && F1 < F2) || (!OuterMax && F1 > F2)) {
        Inst.Opcode = D3D10_SB_OPCODE_MOV;
        Inst.Sources.assign({MakeImm(B2)});
        return true;
      }
      continue;
    }
    if (F1 == 0.0f && F2 == 0.0f && B1 != B2)
      continue;
    const uint32_t Best = OuterMax ? (F1 > F2 ? B1 : B2) : (F1 < F2 ? B1 : B2);
    Operand X = P.Sources[1 - K];
    Inst.Sources[0] = X;
    Inst.Sources[1] = MakeImm(Best);
    Ctx.BumpUse(X);
    return true;
  }
  return false;
}

bool TryFoldNestedConstMinMax(AlgContext &Ctx, SsaInst &Inst) {
  const bool OuterMax = (Inst.Opcode == D3D10_SB_OPCODE_MAX);
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &C2 = Inst.Sources[Side];
    const auto &V = Inst.Sources[1 - Side];
    if (C2.Kind != OperandKind::Immediate || V.Kind != OperandKind::Temp ||
        V.Modifier || V.RelativeRegister != UINT32_MAX)
      continue;
    const uint32_t B2 = ApplyFloatModToBits(C2.Index, C2.Modifier);
    const float F2 = BitsToFloat(B2);
    if (std::isnan(F2))
      continue;
    const auto &VD = Ctx.Defs[V.Index];
    if (!VD.Valid() || VD.IsPhi || VD.IsAux)
      continue;
    const auto &P = Ctx.Ssa.Blocks[VD.Block].Insts[VD.Index];
    if (P.Dead || P.Precise || P.Saturate ||
        (P.Opcode != D3D10_SB_OPCODE_MIN && P.Opcode != D3D10_SB_OPCODE_MAX) ||
        P.Dest.Mask != Inst.Dest.Mask ||
        !IsIdentitySwizzle(V.Swizzle, Inst.Dest.Mask))
      continue;
    if (TryFoldMinMaxPair(Ctx, Inst, P, OuterMax, B2, F2))
      return true;
  }
  return false;
}

bool dxcb5::alg::SimplifyMinMaxInst(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Precise)
    return false;
  if (TryFoldNestedConstMinMax(Ctx, Inst))
    return true;
  const bool IsMax = (Inst.Opcode == D3D10_SB_OPCODE_MAX);
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &C = Inst.Sources[Side];
    const auto &V = Inst.Sources[1 - Side];
    bool Neg = false;
    const bool MatchBound =
        IsMax ? IsZeroFloatImm(C) : (IsOneFloatImm(C, Neg) && !Neg);
    if (!MatchBound || V.Kind != OperandKind::Temp || V.Modifier ||
        V.RelativeRegister != UINT32_MAX)
      continue;
    const auto &VD = Ctx.Defs[V.Index];
    if (!VD.Valid() || VD.IsPhi || VD.IsAux)
      continue;
    auto &P = Ctx.Ssa.Blocks[VD.Block].Insts[VD.Index];
    if (P.Dead)
      continue;
    if (P.Saturate) {
      bool AllInP = true;
      for (unsigned L = 0; L < kRegisterLanes; ++L)
        if ((Inst.Dest.Mask & (1u << L)) &&
            !(P.Dest.Mask & (1u << GetSwizzleLane(V.Swizzle, L))))
          AllInP = false;
      if (AllInP) {
        Operand Kept = V;
        Inst.Opcode = D3D10_SB_OPCODE_MOV;
        Inst.Sources.assign({Kept});
        return true;
      }
    }
    const uint32_t InnerOp = IsMax ? D3D10_SB_OPCODE_MIN : D3D10_SB_OPCODE_MAX;
    if (P.Opcode != InnerOp || P.Precise || P.Saturate ||
        Ctx.Stats.Count[V.Index] != 1 || P.Dest.Mask != Inst.Dest.Mask ||
        !IsIdentitySwizzle(V.Swizzle, Inst.Dest.Mask))
      continue;
    for (unsigned K = 0; K < 2; ++K) {
      bool InNeg = false;
      const bool InnerMatch = IsMax
                                  ? (IsOneFloatImm(P.Sources[K], InNeg) && !InNeg)
                                  : IsZeroFloatImm(P.Sources[K]);
      if (InnerMatch) {
        Operand X = P.Sources[1 - K];
        if (IsMax && Ctx.Ssa.NanSafe && !IsKnownNonNaNFloat(Ctx, X, Inst.Dest.Mask))
          continue;
        Inst.Opcode = D3D10_SB_OPCODE_MOV;
        Inst.Saturate = true;
        Inst.Sources.assign({X});
        P.Dead = true;
        return true;
      }
    }
  }
  return false;
}

bool dxcb5::alg::SimplifyExpInst(AlgContext &Ctx, uint32_t B, uint32_t &I) {
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  if (Inst.Precise || Inst.Sources.size() != 1 ||
      Inst.Sources[0].Kind != OperandKind::Temp || Inst.Sources[0].Modifier ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX ||
      Ctx.Stats.Count[Inst.Sources[0].Index] != 1)
    return false;
  const auto &MD = Ctx.Defs[Inst.Sources[0].Index];
  if (!MD.Valid() || MD.IsPhi || MD.IsAux)
    return false;
  auto &Mul = Ctx.Ssa.Blocks[MD.Block].Insts[MD.Index];
  if (Mul.Dead || Mul.Opcode != D3D10_SB_OPCODE_MUL || Mul.Precise ||
      Mul.Saturate || Mul.Dest.Mask != Inst.Dest.Mask ||
      !IsIdentitySwizzle(Inst.Sources[0].Swizzle, Inst.Dest.Mask))
    return false;
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &COp = Mul.Sources[Side];
    const auto &LOp = Mul.Sources[1 - Side];
    if (COp.Kind != OperandKind::Immediate || COp.Modifier ||
        LOp.Kind != OperandKind::Temp || LOp.Modifier ||
        LOp.RelativeRegister != UINT32_MAX || Ctx.Stats.Count[LOp.Index] != 1)
      continue;
    const auto &LD = Ctx.Defs[LOp.Index];
    if (!LD.Valid() || LD.IsPhi || LD.IsAux)
      continue;
    auto &Log = Ctx.Ssa.Blocks[LD.Block].Insts[LD.Index];
    if (Log.Dead || Log.Opcode != D3D10_SB_OPCODE_LOG || Log.Precise ||
        Log.Saturate || Log.Dest.Mask != Inst.Dest.Mask ||
        !IsIdentitySwizzle(LOp.Swizzle, Inst.Dest.Mask))
      continue;
    if (TryFoldPowConst(Ctx, B, I, Mul, Log, LOp, BitsToFloat(COp.Index)))
      return true;
  }
  return false;
}

bool dxcb5::alg::SimplifyDivInst(AlgContext &Ctx, uint32_t B, SsaInst &Inst) {
  if (Inst.Precise)
    return false;
  bool Neg = false;
  if (IsOneFloatImm(Inst.Sources[1], Neg)) {
    Operand Other = Inst.Sources[0];
    if (Neg)
      Other = NegateMirOperand(Ctx.Ssa, Other);
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Sources.assign({Other});
    return true;
  }
  if (IsOneFloatImm(Inst.Sources[0], Neg)) {
    Operand Den = Inst.Sources[1];
    if (Neg)
      Den = NegateMirOperand(Ctx.Ssa, Den);
    Inst.Opcode = D3D11_SB_OPCODE_RCP;
    Inst.Sources.assign({Den});
    return true;
  }
  if (Inst.Sources[1].Kind == OperandKind::Immediate &&
      !Inst.Sources[1].Modifier) {
    uint32_t RcpBits = 0;
    if (TryReciprocalBits(Inst.Sources[1].Index, RcpBits)) {
      Inst.Opcode = D3D10_SB_OPCODE_MUL;
      Inst.Sources[1] = MakeImm(RcpBits);
      return true;
    }
  }
  if (Inst.Sources[1].Kind == OperandKind::Temp && !Inst.Sources[1].Modifier &&
      Inst.Sources[1].RelativeRegister == UINT32_MAX &&
      Ctx.Stats.Count[Inst.Sources[1].Index] == 1) {
    const auto &SD = Ctx.Defs[Inst.Sources[1].Index];
    if (SD.Valid() && !SD.IsPhi && !SD.IsAux && SD.Block == B) {
      auto &Sqrt = Ctx.Ssa.Blocks[SD.Block].Insts[SD.Index];
      if (!Sqrt.Dead && Sqrt.Opcode == D3D10_SB_OPCODE_SQRT && !Sqrt.Precise &&
          !Sqrt.Saturate) {
        Sqrt.Opcode = D3D10_SB_OPCODE_RSQ;
        Inst.Opcode = D3D10_SB_OPCODE_MUL;
        return true;
      }
    }
  }
  return false;
}
