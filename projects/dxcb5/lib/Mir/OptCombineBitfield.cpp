// Distributed under the University of Illinois Open Source License.
#include "Mir/OptAlgebraicCommon.h"

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

bool IsContiguousMask(uint32_t M, unsigned &Width, unsigned &Offset) {
  if (M == 0u || M == 0xffffffffu)
    return false;
  Offset = static_cast<unsigned>(llvm::countTrailingZeros(M));
  const uint32_t Shifted = M >> Offset;
  if ((Shifted & (Shifted + 1u)) != 0u)
    return false;
  Width = static_cast<unsigned>(llvm::countTrailingOnes(Shifted));
  return Width >= 1u && Width <= 31u && Offset <= 31u && Width + Offset <= 32u;
}

uint32_t FieldMaskFor(unsigned Width, unsigned Offset) {
  return static_cast<uint32_t>(((1ULL << Width) - 1ULL) << Offset);
}

bool GetPlainImm(const Operand &Op, uint32_t &Val) {
  if (Op.Kind != OperandKind::Immediate || Op.Modifier ||
      Op.RelativeRegister != UINT32_MAX)
    return false;
  Val = Op.Index;
  return true;
}

const SsaInst *GetPlainSameMaskDef(const AlgContext &Ctx, const Operand &Op,
                                   uint8_t Mask) {
  if (Op.Kind != OperandKind::Temp || Op.Modifier ||
      Op.RelativeRegister != UINT32_MAX || Op.Index >= Ctx.Defs.size() ||
      !IsIdentitySwizzle(Op.Swizzle, Mask))
    return nullptr;
  const auto &D = Ctx.Defs[Op.Index];
  if (!D.Valid() || D.IsPhi || D.IsAux)
    return nullptr;
  const auto &Inst = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
  if (Inst.Dead || Inst.Saturate || Inst.Dest.Mask != Mask)
    return nullptr;
  return &Inst;
}

bool TryFoldConstInt(SsaInst &Inst) {
  if (Inst.Saturate || Inst.Sources.empty())
    return false;
  if (Inst.Opcode == D3D10_SB_OPCODE_MOVC && Inst.Sources.size() == 3) {
    uint32_t C = 0;
    if (!GetPlainImm(Inst.Sources[0], C))
      return false;
    Operand Pick = C != 0u ? Inst.Sources[1] : Inst.Sources[2];
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Sources.assign({Pick});
    return true;
  }
  uint32_t V[4] = {};
  if (Inst.Sources.size() > 4)
    return false;
  for (size_t K = 0; K < Inst.Sources.size(); ++K)
    if (!GetPlainImm(Inst.Sources[K], V[K]))
      return false;
  uint32_t Res = 0;
  switch (Inst.Opcode) {
  case D3D10_SB_OPCODE_IADD: Res = V[0] + V[1]; break;
  case D3D10_SB_OPCODE_INEG: Res = 0u - V[0]; break;
  case D3D10_SB_OPCODE_NOT:  Res = ~V[0]; break;
  case D3D10_SB_OPCODE_AND:  Res = V[0] & V[1]; break;
  case D3D10_SB_OPCODE_OR:   Res = V[0] | V[1]; break;
  case D3D10_SB_OPCODE_XOR:  Res = V[0] ^ V[1]; break;
  case D3D10_SB_OPCODE_ISHL: Res = V[0] << (V[1] & 31u); break;
  case D3D10_SB_OPCODE_USHR: Res = V[0] >> (V[1] & 31u); break;
  case D3D10_SB_OPCODE_ISHR:
    Res = static_cast<uint32_t>(static_cast<int32_t>(V[0]) >> (V[1] & 31u));
    break;
  case D3D10_SB_OPCODE_IEQ:  Res = (V[0] == V[1]) ? 0xffffffffu : 0u; break;
  case D3D10_SB_OPCODE_INE:  Res = (V[0] != V[1]) ? 0xffffffffu : 0u; break;
  case D3D10_SB_OPCODE_ULT:  Res = (V[0] < V[1]) ? 0xffffffffu : 0u; break;
  case D3D10_SB_OPCODE_UGE:  Res = (V[0] >= V[1]) ? 0xffffffffu : 0u; break;
  case D3D10_SB_OPCODE_ILT:
    Res = (static_cast<int32_t>(V[0]) < static_cast<int32_t>(V[1]))
              ? 0xffffffffu : 0u;
    break;
  case D3D10_SB_OPCODE_IGE:
    Res = (static_cast<int32_t>(V[0]) >= static_cast<int32_t>(V[1]))
              ? 0xffffffffu : 0u;
    break;
  case D3D10_SB_OPCODE_UMIN: Res = V[0] < V[1] ? V[0] : V[1]; break;
  case D3D10_SB_OPCODE_UMAX: Res = V[0] > V[1] ? V[0] : V[1]; break;
  case D3D10_SB_OPCODE_IMIN:
    Res = static_cast<int32_t>(V[0]) < static_cast<int32_t>(V[1]) ? V[0] : V[1];
    break;
  case D3D10_SB_OPCODE_IMAX:
    Res = static_cast<int32_t>(V[0]) > static_cast<int32_t>(V[1]) ? V[0] : V[1];
    break;
  case D3D10_SB_OPCODE_IMUL:
    if (Inst.Dest.Kind != OperandKind::Null ||
        Inst.AuxDest.Kind != OperandKind::Temp)
      return false;
    Res = V[0] * V[1];
    Inst.Dest = Inst.AuxDest;
    Inst.AuxDest = Operand();
    break;
  case D3D11_SB_OPCODE_UBFE: {
    const unsigned W = V[0] & 31u, O = V[1] & 31u;
    Res = W == 0u ? 0u : ((V[2] >> O) & static_cast<uint32_t>((1ULL << W) - 1ULL));
    break;
  }
  case D3D11_SB_OPCODE_BFI: {
    const unsigned W = V[0] & 31u, O = V[1] & 31u;
    const uint32_t M = W == 0u ? 0u : FieldMaskFor(W, O);
    Res = ((V[2] << O) & M) | (V[3] & ~M);
    break;
  }
  default: return false;
  }
  Inst.Opcode = D3D10_SB_OPCODE_MOV;
  Inst.Sources.assign({MakeImm(Res)});
  return true;
}

bool TrySelectFromAnd(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources.size() != 2 || Inst.Saturate)
    return false;
  for (unsigned Side = 0; Side < 2; ++Side) {
    uint32_t MaskImm = 0;
    unsigned W = 0, Off = 0;
    if (!GetPlainImm(Inst.Sources[Side], MaskImm) ||
        !IsContiguousMask(MaskImm, W, Off))
      continue;
    const auto &InnerOp = Inst.Sources[1 - Side];
    const SsaInst *Inner = GetPlainSameMaskDef(Ctx, InnerOp, Inst.Dest.Mask);
    if (!Inner || Inner->Sources.size() != 2)
      continue;
    uint32_t ShAmt = 0;
    if (!GetPlainImm(Inner->Sources[1], ShAmt) || ShAmt < 1u || ShAmt > 31u)
      continue;
    if (Inner->Opcode == D3D10_SB_OPCODE_USHR && Off == 0u && W + ShAmt <= 32u) {
      Operand X = Inner->Sources[0];
      Inst.Opcode = D3D11_SB_OPCODE_UBFE;
      Inst.Sources.assign({MakeImm(W), MakeImm(ShAmt), X});
      Ctx.BumpUse(X);
      return true;
    }
    if (Inner->Opcode == D3D10_SB_OPCODE_ISHL && Off == ShAmt && W + Off <= 32u) {
      Operand X = Inner->Sources[0];
      Inst.Opcode = D3D11_SB_OPCODE_BFI;
      Inst.Sources.assign({MakeImm(W), MakeImm(Off), X, MakeImm(0)});
      Ctx.BumpUse(X);
      return true;
    }
  }
  return false;
}

bool TrySelectFromShift(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources.size() != 2 || Inst.Saturate)
    return false;
  uint32_t ShAmt = 0;
  if (!GetPlainImm(Inst.Sources[1], ShAmt) || ShAmt < 1u || ShAmt > 31u)
    return false;
  const SsaInst *Inner = GetPlainSameMaskDef(Ctx, Inst.Sources[0], Inst.Dest.Mask);
  if (!Inner || Inner->Opcode != D3D10_SB_OPCODE_AND || Inner->Sources.size() != 2)
    return false;
  for (unsigned K = 0; K < 2; ++K) {
    uint32_t MaskImm = 0;
    unsigned W = 0, Off = 0;
    if (!GetPlainImm(Inner->Sources[K], MaskImm) ||
        !IsContiguousMask(MaskImm, W, Off))
      continue;
    Operand X = Inner->Sources[1 - K];
    if (Inst.Opcode == D3D10_SB_OPCODE_USHR && Off == ShAmt && W + Off <= 32u) {
      Inst.Opcode = D3D11_SB_OPCODE_UBFE;
      Inst.Sources.assign({MakeImm(W), MakeImm(Off), X});
      Ctx.BumpUse(X);
      return true;
    }
    if (Inst.Opcode == D3D10_SB_OPCODE_ISHL && Off == 0u && W + ShAmt <= 32u) {
      Inst.Opcode = D3D11_SB_OPCODE_BFI;
      Inst.Sources.assign({MakeImm(W), MakeImm(ShAmt), X, MakeImm(0)});
      Ctx.BumpUse(X);
      return true;
    }
  }
  return false;
}

bool MatchInsertField(const AlgContext &Ctx, const Operand &Op, uint8_t Mask,
                      unsigned &W, unsigned &Off, Operand &Val,
                      bool &SingleUse) {
  const SsaInst *P = GetPlainSameMaskDef(Ctx, Op, Mask);
  if (!P)
    return false;
  SingleUse = Ctx.Stats.Count[Op.Index] == 1u;
  if (P->Opcode == D3D11_SB_OPCODE_BFI && P->Sources.size() == 4) {
    uint32_t WI = 0, OI = 0, BI = 0;
    if (GetPlainImm(P->Sources[0], WI) && GetPlainImm(P->Sources[1], OI) &&
        GetPlainImm(P->Sources[3], BI) && BI == 0u && WI >= 1u && WI <= 31u &&
        OI <= 31u && WI + OI <= 32u) {
      W = WI; Off = OI; Val = P->Sources[2];
      return true;
    }
  }
  if (P->Opcode == D3D10_SB_OPCODE_ISHL && P->Sources.size() == 2) {
    uint32_t OI = 0;
    if (GetPlainImm(P->Sources[1], OI) && OI >= 1u && OI <= 31u) {
      W = 32u - OI; Off = OI; Val = P->Sources[0];
      return true;
    }
  }
  if (P->Opcode == D3D10_SB_OPCODE_AND && P->Sources.size() == 2) {
    for (unsigned K = 0; K < 2; ++K) {
      uint32_t M = 0;
      unsigned MW = 0, MO = 0;
      if (GetPlainImm(P->Sources[K], M) && IsContiguousMask(M, MW, MO) && MO == 0u) {
        W = MW; Off = 0u; Val = P->Sources[1 - K];
        return true;
      }
    }
  }
  return false;
}

bool TryStripInverseMask(const AlgContext &Ctx, const Operand &Op, uint8_t Mask,
                         uint32_t FieldMask, Operand &Unmasked) {
  const SsaInst *P = GetPlainSameMaskDef(Ctx, Op, Mask);
  if (!P || P->Opcode != D3D10_SB_OPCODE_AND || P->Sources.size() != 2)
    return false;
  for (unsigned K = 0; K < 2; ++K) {
    uint32_t M = 0;
    if (GetPlainImm(P->Sources[K], M) && M == ~FieldMask) {
      Unmasked = P->Sources[1 - K];
      return true;
    }
  }
  return false;
}

bool BaseHasFieldClear(const AlgContext &Ctx, const Operand &Base, uint8_t Mask,
                       uint32_t FieldMask) {
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if ((Mask & (1u << L)) &&
        (KnownPossibleOnes(Ctx, Base, L, 0) & FieldMask) != 0u)
      return false;
  return true;
}

bool TrySelectBfiOrImadFromOr(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources.size() != 2 || Inst.Saturate ||
      (Inst.Dest.Kind == OperandKind::Temp &&
       Ctx.Stats.RelativeUse[Inst.Dest.Index]))
    return false;
  for (unsigned Side = 0; Side < 2; ++Side) {
    unsigned W = 0, Off = 0;
    Operand Val;
    bool SingleIns = false;
    if (!MatchInsertField(Ctx, Inst.Sources[Side], Inst.Dest.Mask, W, Off, Val,
                          SingleIns))
      continue;
    const uint32_t FMask = FieldMaskFor(W, Off);
    Operand Base = Inst.Sources[1 - Side];
    Operand Unmasked;
    const bool Stripped =
        TryStripInverseMask(Ctx, Base, Inst.Dest.Mask, FMask, Unmasked);
    if (Stripped) {
      Inst.Opcode = D3D11_SB_OPCODE_BFI;
      Inst.Sources.assign({MakeImm(W), MakeImm(Off), Val, Unmasked});
      Ctx.BumpUse(Val);
      Ctx.BumpUse(Unmasked);
      return true;
    }
    if (SingleIns && BaseHasFieldClear(Ctx, Base, Inst.Dest.Mask, FMask)) {
      Inst.Opcode = D3D11_SB_OPCODE_BFI;
      Inst.Sources.assign({MakeImm(W), MakeImm(Off), Val, Base});
      Ctx.BumpUse(Val);
      return true;
    }
  }
  for (unsigned Side = 0; Side < 2; ++Side) {
    uint32_t Imm = 0;
    const auto &MulOp = Inst.Sources[1 - Side];
    if (!GetPlainImm(Inst.Sources[Side], Imm) || Imm == 0u ||
        MulOp.Kind != OperandKind::Temp || MulOp.Modifier ||
        MulOp.RelativeRegister != UINT32_MAX ||
        Ctx.Stats.Count[MulOp.Index] != 1u ||
        !IsIdentitySwizzle(MulOp.Swizzle, Inst.Dest.Mask))
      continue;
    const auto &D = Ctx.Defs[MulOp.Index];
    if (!D.Valid() || D.IsPhi || !D.IsAux)
      continue;
    auto &Mul = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
    if (Mul.Dead || Mul.Opcode != D3D10_SB_OPCODE_IMUL ||
        Mul.Dest.Kind != OperandKind::Null ||
        Mul.AuxDest.Mask != Inst.Dest.Mask ||
        !BaseHasFieldClear(Ctx, MulOp, Inst.Dest.Mask, Imm))
      continue;
    Operand A = Mul.Sources[0], B = Mul.Sources[1];
    Inst.Opcode = D3D10_SB_OPCODE_IMAD;
    Inst.Sources.assign({A, B, MakeImm(Imm)});
    Mul.Dead = true;
    return true;
  }
  return false;
}

} // namespace

bool dxcb5::alg::ExtractLaneImm(const SsaProgram &Ssa, const Operand &Op,
                                unsigned Lane, uint32_t &OutVal) {
  if (Op.Modifier)
    return false;
  if (Op.Kind == OperandKind::Immediate) {
    OutVal = Op.Index;
    return true;
  }
  if (Op.Kind == OperandKind::ImmediateVector) {
    OutVal = Ssa.Mir.LiteralVectors[Op.Index][GetSwizzleLane(Op.Swizzle, Lane)];
    return true;
  }
  return false;
}

uint32_t dxcb5::alg::KnownPossibleOnes(const AlgContext &Ctx, const Operand &Op,
                                       unsigned Lane, unsigned Depth) {
  if (Depth > 6 || Op.Modifier || Op.RelativeRegister != UINT32_MAX)
    return 0xffffffffu;
  uint32_t Imm = 0;
  if (ExtractLaneImm(Ctx.Ssa, Op, Lane, Imm))
    return Imm;
  if (Op.Kind != OperandKind::Temp || Op.Index >= Ctx.Defs.size())
    return 0xffffffffu;
  const auto &D = Ctx.Defs[Op.Index];
  if (!D.Valid() || D.IsPhi)
    return 0xffffffffu;
  const auto &P = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
  if (P.Dead)
    return 0xffffffffu;
  const unsigned SrcL = GetSwizzleLane(Op.Swizzle, Lane);
  if (D.IsAux) {
    if (P.Opcode != D3D10_SB_OPCODE_IMUL || !(P.AuxDest.Mask & (1u << SrcL)))
      return 0xffffffffu;
    uint32_t C = 0;
    if ((ExtractLaneImm(Ctx.Ssa, P.Sources[0], SrcL, C) ||
         ExtractLaneImm(Ctx.Ssa, P.Sources[1], SrcL, C)) && C != 0u) {
      const unsigned Tz = static_cast<unsigned>(llvm::countTrailingZeros(C));
      return Tz == 0u ? 0xffffffffu : ~((1u << Tz) - 1u);
    }
    return 0xffffffffu;
  }
  if (!(P.Dest.Mask & (1u << SrcL)))
    return 0xffffffffu;
  if (P.Opcode == D3D10_SB_OPCODE_AND && P.Sources.size() == 2)
    return KnownPossibleOnes(Ctx, P.Sources[0], SrcL, Depth + 1) &
           KnownPossibleOnes(Ctx, P.Sources[1], SrcL, Depth + 1);
  if (P.Opcode == D3D10_SB_OPCODE_OR && P.Sources.size() == 2)
    return KnownPossibleOnes(Ctx, P.Sources[0], SrcL, Depth + 1) |
           KnownPossibleOnes(Ctx, P.Sources[1], SrcL, Depth + 1);
  uint32_t Sh = 0;
  if (P.Opcode == D3D10_SB_OPCODE_ISHL && P.Sources.size() == 2 &&
      ExtractLaneImm(Ctx.Ssa, P.Sources[1], SrcL, Sh))
    return KnownPossibleOnes(Ctx, P.Sources[0], SrcL, Depth + 1) << (Sh & 31u);
  if (P.Opcode == D3D10_SB_OPCODE_USHR && P.Sources.size() == 2 &&
      ExtractLaneImm(Ctx.Ssa, P.Sources[1], SrcL, Sh))
    return KnownPossibleOnes(Ctx, P.Sources[0], SrcL, Depth + 1) >> (Sh & 31u);
  if (P.Opcode == D3D11_SB_OPCODE_UBFE && P.Sources.size() == 3) {
    uint32_t W = 0, O = 0;
    if (ExtractLaneImm(Ctx.Ssa, P.Sources[0], SrcL, W) &&
        ExtractLaneImm(Ctx.Ssa, P.Sources[1], SrcL, O)) {
      const unsigned Wb = W & 31u, Ob = O & 31u;
      return Wb == 0u ? 0u
                      : ((KnownPossibleOnes(Ctx, P.Sources[2], SrcL, Depth + 1) >> Ob) &
                         static_cast<uint32_t>((1ULL << Wb) - 1ULL));
    }
  }
  if (P.Opcode == D3D11_SB_OPCODE_BFI && P.Sources.size() == 4) {
    uint32_t W = 0, O = 0;
    if (ExtractLaneImm(Ctx.Ssa, P.Sources[0], SrcL, W) &&
        ExtractLaneImm(Ctx.Ssa, P.Sources[1], SrcL, O)) {
      const unsigned Wb = W & 31u, Ob = O & 31u;
      const uint32_t M = Wb == 0u ? 0u : FieldMaskFor(Wb, Ob);
      const uint32_t Ins =
          (KnownPossibleOnes(Ctx, P.Sources[2], SrcL, Depth + 1) << Ob) & M;
      const uint32_t Base =
          KnownPossibleOnes(Ctx, P.Sources[3], SrcL, Depth + 1) & ~M;
      return Ins | Base;
    }
  }
  return 0xffffffffu;
}

bool dxcb5::alg::TrySimplifyBitfieldAndConstInt(AlgContext &Ctx, SsaInst &Inst) {
  if (TryFoldConstInt(Inst))
    return true;
  if (Inst.Opcode == D3D10_SB_OPCODE_AND)
    return TrySelectFromAnd(Ctx, Inst);
  if (Inst.Opcode == D3D10_SB_OPCODE_USHR || Inst.Opcode == D3D10_SB_OPCODE_ISHL)
    return TrySelectFromShift(Ctx, Inst);
  if (Inst.Opcode == D3D10_SB_OPCODE_OR)
    return TrySelectBfiOrImadFromOr(Ctx, Inst);
  return false;
}
