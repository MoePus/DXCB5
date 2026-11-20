// Distributed under the University of Illinois Open Source License.
#include "Mir/OptAlgebraicCommon.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

bool TryFoldXorBoolNot(AlgContext &Ctx, SsaInst &Inst, unsigned Side) {
  const auto &AndOp = Inst.Sources[1 - Side];
  if (AndOp.Kind != OperandKind::Temp || AndOp.Modifier ||
      AndOp.RelativeRegister != UINT32_MAX || Ctx.Stats.Count[AndOp.Index] != 1)
    return false;
  const auto &AD = Ctx.Defs[AndOp.Index];
  if (!AD.Valid() || AD.IsPhi || AD.IsAux)
    return false;
  auto &AndInst = Ctx.Ssa.Blocks[AD.Block].Insts[AD.Index];
  if (AndInst.Dead || AndInst.Opcode != D3D10_SB_OPCODE_AND ||
      AndInst.Sources.size() != 2 || AndInst.Dest.Mask != Inst.Dest.Mask ||
      !IsIdentitySwizzle(AndOp.Swizzle, Inst.Dest.Mask))
    return false;
  for (unsigned K = 0; K < 2; ++K) {
    if (AndInst.Sources[K].Kind == OperandKind::Immediate &&
        AndInst.Sources[K].Index == 1u && !AndInst.Sources[K].Modifier &&
        IsBoolMaskValue(Ctx.Ssa, Ctx.Defs, AndInst.Sources[1 - K])) {
      Operand RawMask = AndInst.Sources[1 - K];
      AndInst.Opcode = D3D10_SB_OPCODE_NOT;
      AndInst.Sources.assign({RawMask});
      Inst.Opcode = D3D10_SB_OPCODE_AND;
      return true;
    }
  }
  return false;
}

bool SimplifyXorInst(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources.size() != 2) return false;
  for (unsigned Side = 0; Side < 2; ++Side) {
    if (Inst.Sources[Side].Kind != OperandKind::Immediate ||
        Inst.Sources[Side].Modifier) continue;
    if (Inst.Sources[Side].Index == 0xffffffffu) {
      Operand Inner = Inst.Sources[1 - Side];
      Inst.Opcode = D3D10_SB_OPCODE_NOT;
      Inst.Sources.assign({Inner});
      return true;
    }
    if (Inst.Sources[Side].Index == 1u && TryFoldXorBoolNot(Ctx, Inst, Side))
      return true;
  }
  return false;
}

uint32_t InvertedCompareOpcode(uint32_t Op, bool AllowFloat) {
  switch (Op) {
  case D3D10_SB_OPCODE_IEQ: return D3D10_SB_OPCODE_INE;
  case D3D10_SB_OPCODE_INE: return D3D10_SB_OPCODE_IEQ;
  case D3D10_SB_OPCODE_ILT: return D3D10_SB_OPCODE_IGE;
  case D3D10_SB_OPCODE_IGE: return D3D10_SB_OPCODE_ILT;
  case D3D10_SB_OPCODE_ULT: return D3D10_SB_OPCODE_UGE;
  case D3D10_SB_OPCODE_UGE: return D3D10_SB_OPCODE_ULT;
  case D3D10_SB_OPCODE_LT: return AllowFloat ? D3D10_SB_OPCODE_GE : 0u;
  case D3D10_SB_OPCODE_GE: return AllowFloat ? D3D10_SB_OPCODE_LT : 0u;
  case D3D10_SB_OPCODE_EQ: return AllowFloat ? D3D10_SB_OPCODE_NE : 0u;
  case D3D10_SB_OPCODE_NE: return AllowFloat ? D3D10_SB_OPCODE_EQ : 0u;
  default: return 0u;
  }
}

bool SimplifyNotInst(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources[0].Kind != OperandKind::Temp ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX) return false;
  const auto &Def = Ctx.Defs[Inst.Sources[0].Index];
  if (!Def.Valid() || Def.IsPhi || Def.IsAux) return false;
  auto &Prod = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
  if (Prod.Dead) return false;
  if (Prod.Opcode == D3D10_SB_OPCODE_NOT &&
      Prod.Sources[0].RelativeRegister == UINT32_MAX) {
    Operand Orig = Prod.Sources[0];
    Orig.Swizzle = ComposeSwizzle(Orig.Swizzle, Inst.Sources[0].Swizzle);
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Sources.assign({Orig});
    Ctx.BumpUse(Orig);
    return true;
  }
  if (Ctx.Stats.Count[Inst.Sources[0].Index] != 1 ||
      Prod.Dest.Mask != Inst.Dest.Mask ||
      (Inst.Sources[0].Swizzle != kIdentitySwizzle &&
       !(PopcountMask(Inst.Dest.Mask) == 1 && Inst.Sources[0].Swizzle == 0)))
    return false;
  const uint32_t InvOp =
      InvertedCompareOpcode(Prod.Opcode, !Prod.Precise && !Inst.Precise);
  if (!InvOp) return false;
  Inst.Opcode = InvOp;
  Inst.Sources = Prod.Sources;
  Prod.Dead = true;
  return true;
}

bool IsNotOfBoolMask(const AlgContext &Ctx, const Operand &MaybeNot,
                     const Operand &Target) {
  if (MaybeNot.Kind != OperandKind::Temp || MaybeNot.Modifier ||
      MaybeNot.RelativeRegister != UINT32_MAX ||
      !IsBoolMaskValue(Ctx.Ssa, Ctx.Defs, Target))
    return false;
  const auto &ND = Ctx.Defs[MaybeNot.Index];
  if (!ND.Valid() || ND.IsPhi || ND.IsAux)
    return false;
  const auto &NI = Ctx.Ssa.Blocks[ND.Block].Insts[ND.Index];
  if (NI.Dead || NI.Opcode != D3D10_SB_OPCODE_NOT || NI.Sources.size() != 1)
    return false;
  Operand Orig = NI.Sources[0];
  if (Orig.RelativeRegister != UINT32_MAX || Orig.Modifier)
    return false;
  if (Orig.Kind != OperandKind::Immediate &&
      Orig.Kind != OperandKind::ImmediateVector)
    Orig.Swizzle = ComposeSwizzle(Orig.Swizzle, MaybeNot.Swizzle);
  return EqualCopySources(Orig, Target);
}

bool SimplifyBitwiseSelectOr(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources.size() != 2 || Inst.Saturate)
    return false;
  const auto &S0 = Inst.Sources[0];
  const auto &S1 = Inst.Sources[1];
  if (S0.Kind != OperandKind::Temp || S1.Kind != OperandKind::Temp ||
      S0.Modifier || S1.Modifier || S0.RelativeRegister != UINT32_MAX ||
      S1.RelativeRegister != UINT32_MAX)
    return false;
  const auto &D0 = Ctx.Defs[S0.Index];
  const auto &D1 = Ctx.Defs[S1.Index];
  if (!D0.Valid() || D0.IsPhi || D0.IsAux || !D1.Valid() || D1.IsPhi ||
      D1.IsAux)
    return false;
  const auto &A0 = Ctx.Ssa.Blocks[D0.Block].Insts[D0.Index];
  const auto &A1 = Ctx.Ssa.Blocks[D1.Block].Insts[D1.Index];
  if (A0.Dead || A1.Dead || A0.Opcode != D3D10_SB_OPCODE_AND ||
      A1.Opcode != D3D10_SB_OPCODE_AND || A0.Sources.size() != 2 ||
      A1.Sources.size() != 2 || A0.Dest.Mask != Inst.Dest.Mask ||
      A1.Dest.Mask != Inst.Dest.Mask ||
      !IsIdentitySwizzle(S0.Swizzle, Inst.Dest.Mask) ||
      !IsIdentitySwizzle(S1.Swizzle, Inst.Dest.Mask))
    return false;
  for (unsigned Pair = 0; Pair < 4; ++Pair) {
    const unsigned I0 = Pair >> 1;
    const unsigned I1 = Pair & 1u;
    if (IsNotOfBoolMask(Ctx, A1.Sources[I1], A0.Sources[I0])) {
      Operand C = A0.Sources[I0], T = A0.Sources[1 - I0],
              F = A1.Sources[1 - I1];
      Inst.Opcode = D3D10_SB_OPCODE_MOVC;
      Inst.Sources.assign({C, T, F});
      Ctx.BumpUse(C); Ctx.BumpUse(T); Ctx.BumpUse(F);
      return true;
    }
    if (IsNotOfBoolMask(Ctx, A0.Sources[I0], A1.Sources[I1])) {
      Operand C = A1.Sources[I1], T = A1.Sources[1 - I1],
              F = A0.Sources[1 - I0];
      Inst.Opcode = D3D10_SB_OPCODE_MOVC;
      Inst.Sources.assign({C, T, F});
      Ctx.BumpUse(C); Ctx.BumpUse(T); Ctx.BumpUse(F);
      return true;
    }
  }
  return false;
}

bool IsExactIntFloat(uint32_t Bits, bool IsUnsigned) {
  const float F = BitsToFloat(Bits);
  if (!std::isfinite(F) || std::trunc(F) != F)
    return false;
  return IsUnsigned ? (F >= 0.0f && F <= 16777216.0f)
                    : (F >= -16777216.0f && F <= 16777216.0f);
}

uint32_t ConvertFloatBitsToInt(uint32_t Bits, bool IsUnsigned) {
  const float F = BitsToFloat(Bits);
  return IsUnsigned ? static_cast<uint32_t>(F)
                    : static_cast<uint32_t>(static_cast<int32_t>(F));
}

bool CollectIntFloatMovcTree(
    const AlgContext &Ctx, const Operand &Op, uint8_t ReadMask, bool IsUnsigned,
    unsigned Depth, llvm::SmallDenseMap<uint32_t, uint32_t, 8> &TreeUses,
    llvm::SmallVectorImpl<uint32_t> &TreeOrder) {
  if (Depth > 5 || Op.Modifier || Op.RelativeRegister != UINT32_MAX)
    return false;
  if (Op.Kind == OperandKind::Immediate)
    return IsExactIntFloat(Op.Index, IsUnsigned);
  if (Op.Kind == OperandKind::ImmediateVector) {
    const auto &Arr = Ctx.Ssa.Mir.LiteralVectors[Op.Index];
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((ReadMask & (1u << L)) &&
          !IsExactIntFloat(Arr[GetSwizzleLane(Op.Swizzle, L)], IsUnsigned))
        return false;
    return true;
  }
  if (Op.Kind != OperandKind::Temp || Op.Index >= Ctx.Defs.size())
    return false;
  const auto &D = Ctx.Defs[Op.Index];
  if (!D.Valid() || D.IsPhi || D.IsAux)
    return false;
  const auto &Node = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
  if (Node.Dead || Node.Opcode != D3D10_SB_OPCODE_MOVC || Node.Saturate ||
      Node.Precise || Node.Sources.size() != 3 ||
      Node.Dest.Kind != OperandKind::Temp)
    return false;
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if ((ReadMask & (1u << L)) &&
        !(Node.Dest.Mask & (1u << GetSwizzleLane(Op.Swizzle, L))))
      return false;
  const bool FirstVisit = (TreeUses[Op.Index] == 0);
  ++TreeUses[Op.Index];
  if (!FirstVisit)
    return true;
  TreeOrder.push_back(Op.Index);
  return CollectIntFloatMovcTree(Ctx, Node.Sources[1], Node.Dest.Mask,
                                 IsUnsigned, Depth + 1, TreeUses, TreeOrder) &&
         CollectIntFloatMovcTree(Ctx, Node.Sources[2], Node.Dest.Mask,
                                 IsUnsigned, Depth + 1, TreeUses, TreeOrder);
}

void ConvertLeafFloatToInt(SsaProgram &Ssa, Operand &Leaf, uint8_t Mask,
                           bool IsUnsigned) {
  if (Leaf.Kind == OperandKind::Immediate) {
    Leaf.Index = ConvertFloatBitsToInt(Leaf.Index, IsUnsigned);
    return;
  }
  if (Leaf.Kind != OperandKind::ImmediateVector)
    return;
  const auto OrigArr = Ssa.Mir.LiteralVectors[Leaf.Index];
  auto NewArr = OrigArr;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (Mask & (1u << L)) {
      const unsigned SL = GetSwizzleLane(Leaf.Swizzle, L);
      NewArr[SL] = ConvertFloatBitsToInt(OrigArr[SL], IsUnsigned);
    }
  }
  Leaf.Index = Ssa.InternLiteralVector(NewArr);
}

bool FoldResinfoFloatRoundtrip(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Saturate || Inst.Sources.size() != 1 ||
      Inst.Sources[0].Kind != OperandKind::Temp || Inst.Sources[0].Modifier ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX) return false;
  const auto &D1 = Ctx.Defs[Inst.Sources[0].Index];
  if (!D1.Valid() || D1.IsPhi || D1.IsAux) return false;
  const auto &Cvt = Ctx.Ssa.Blocks[D1.Block].Insts[D1.Index];
  if (Cvt.Dead || Cvt.Saturate || Cvt.Sources.size() != 1 ||
      (Cvt.Opcode != D3D10_SB_OPCODE_ITOF && Cvt.Opcode != D3D10_SB_OPCODE_UTOF) ||
      Cvt.Sources[0].Kind != OperandKind::Temp || Cvt.Sources[0].Modifier ||
      Cvt.Sources[0].RelativeRegister != UINT32_MAX) return false;
  const auto &D2 = Ctx.Defs[Cvt.Sources[0].Index];
  if (!D2.Valid() || D2.IsPhi || D2.IsAux) return false;
  const auto &Res = Ctx.Ssa.Blocks[D2.Block].Insts[D2.Index];
  if (Res.Dead || Res.Opcode != D3D10_SB_OPCODE_RESINFO) return false;
  const uint8_t Swz = ComposeSwizzle(Cvt.Sources[0].Swizzle, Inst.Sources[0].Swizzle);
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if ((Inst.Dest.Mask & (1u << L)) &&
        (!(Cvt.Dest.Mask & (1u << GetSwizzleLane(Inst.Sources[0].Swizzle, L))) ||
         !(Res.Dest.Mask & (1u << GetSwizzleLane(Swz, L)))))
      return false;
  Operand Repl = Cvt.Sources[0];
  Repl.Swizzle = Swz;
  Inst.Opcode = D3D10_SB_OPCODE_MOV;
  Inst.Sources.assign({Repl});
  Ctx.BumpUse(Repl);
  return true;
}

bool FoldFtouFtoiMovcTree(AlgContext &Ctx, SsaInst &Inst) {
  if (FoldResinfoFloatRoundtrip(Ctx, Inst))
    return true;
  if (Inst.Saturate || Inst.Sources.size() != 1 ||
      Inst.Sources[0].Kind != OperandKind::Temp || Inst.Sources[0].Modifier ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX)
    return false;
  const bool IsUnsigned = (Inst.Opcode == D3D10_SB_OPCODE_FTOU);
  llvm::SmallDenseMap<uint32_t, uint32_t, 8> TreeUses;
  llvm::SmallVector<uint32_t, 8> TreeOrder;
  if (!CollectIntFloatMovcTree(Ctx, Inst.Sources[0], Inst.Dest.Mask, IsUnsigned,
                               0, TreeUses, TreeOrder) ||
      TreeOrder.empty())
    return false;
  for (uint32_t NodeId : TreeOrder)
    if (TreeUses[NodeId] != Ctx.Stats.Count[NodeId])
      return false;
  for (uint32_t NodeId : TreeOrder) {
    const auto &D = Ctx.Defs[NodeId];
    auto &Node = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
    ConvertLeafFloatToInt(Ctx.Ssa, Node.Sources[1], Node.Dest.Mask, IsUnsigned);
    ConvertLeafFloatToInt(Ctx.Ssa, Node.Sources[2], Node.Dest.Mask, IsUnsigned);
  }
  Inst.Opcode = D3D10_SB_OPCODE_MOV;
  return true;
}

bool TryFoldInegIntoIadd(AlgContext &Ctx, SsaInst &Inst) {
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &Op = Inst.Sources[Side];
    if (Op.Kind != OperandKind::Temp || Op.Modifier ||
        Op.RelativeRegister != UINT32_MAX)
      continue;
    const auto &D = Ctx.Defs[Op.Index];
    if (!D.Valid() || D.IsPhi || D.IsAux)
      continue;
    const auto &Neg = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
    if (Neg.Dead || Neg.Opcode != D3D10_SB_OPCODE_INEG ||
        Neg.Sources.size() != 1 || Neg.Dest.Mask != Inst.Dest.Mask)
      continue;
    const Operand &Inner = Neg.Sources[0];
    if (Inner.Modifier || Inner.RelativeRegister != UINT32_MAX)
      continue;
    if (Inner.Kind == OperandKind::Immediate) {
      Inst.Sources[Side] = MakeImm(0u - Inner.Index);
      return true;
    }
    if (Inner.Kind != OperandKind::Temp && Inner.Kind != OperandKind::Input &&
        Inner.Kind != OperandKind::ConstantBuffer)
      continue;
    Operand Repl = Inner;
    Repl.Swizzle = ComposeSwizzle(Inner.Swizzle, Op.Swizzle);
    Repl.Modifier = D3D10_SB_OPERAND_MODIFIER_NEG;
    Inst.Sources[Side] = Repl;
    Ctx.BumpUse(Repl);
    return true;
  }
  return false;
}

bool SimplifyIaddInst(AlgContext &Ctx, SsaInst &Inst) {
  if (Inst.Sources.size() != 2)
    return false;
  if (TryFoldInegIntoIadd(Ctx, Inst))
    return true;
  for (unsigned Side = 0; Side < 2; ++Side) {
    if (Inst.Sources[Side].Kind != OperandKind::Immediate ||
        Inst.Sources[Side].Index != 0u || Inst.Sources[Side].Modifier)
      continue;
    Operand Other = Inst.Sources[1 - Side];
    if (Other.Modifier)
      continue;
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Sources.assign({Other});
    return true;
  }
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
    if (Inner.Dead || Inner.Opcode != D3D10_SB_OPCODE_IADD ||
        Inner.Dest.Mask != Inst.Dest.Mask ||
        !IsIdentitySwizzle(POp.Swizzle, Inst.Dest.Mask))
      continue;
    for (unsigned K = 0; K < 2; ++K) {
      if (Inner.Sources[K].Kind == OperandKind::Immediate &&
          !Inner.Sources[K].Modifier) {
        const uint32_t Sum = Inner.Sources[K].Index + COp.Index;
        Inst.Sources[0] = Inner.Sources[1 - K];
        Inst.Sources[1] = MakeImm(Sum);
        Inner.Dead = true;
        return true;
      }
    }
  }
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &Op = Inst.Sources[Side];
    if (Op.Kind != OperandKind::Temp || Op.Modifier ||
        Op.RelativeRegister != UINT32_MAX || Ctx.Stats.Count[Op.Index] != 1)
      continue;
    const auto &D = Ctx.Defs[Op.Index];
    if (!D.Valid() || D.IsPhi || !D.IsAux)
      continue;
    auto &Mul = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
    if (Mul.Dead || Mul.Opcode != D3D10_SB_OPCODE_IMUL ||
        Mul.Dest.Kind != OperandKind::Null ||
        Mul.AuxDest.Kind != OperandKind::Temp)
      continue;
    bool MatchLanes = true;
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((Inst.Dest.Mask & (1u << L)) &&
          !(Mul.AuxDest.Mask & (1u << GetSwizzleLane(Op.Swizzle, L))))
        MatchLanes = false;
    if (!MatchLanes)
      continue;
    Operand A = Mul.Sources[0], C = Mul.Sources[1];
    const bool NeedsSwz = Op.Swizzle != kIdentitySwizzle &&
                          !(PopcountMask(Inst.Dest.Mask) == 1 &&
                            PopcountMask(Mul.AuxDest.Mask) == 1 &&
                            Op.Swizzle == 0);
    if (NeedsSwz && (A.Kind == OperandKind::ImmediateVector ||
                     C.Kind == OperandKind::ImmediateVector))
      continue;
    if (NeedsSwz) {
      if (A.Kind != OperandKind::Immediate)
        A.Swizzle = ComposeSwizzle(A.Swizzle, Op.Swizzle);
      if (C.Kind != OperandKind::Immediate)
        C.Swizzle = ComposeSwizzle(C.Swizzle, Op.Swizzle);
    }
    Operand Addend = Inst.Sources[1 - Side];
    Inst.Opcode = D3D10_SB_OPCODE_IMAD;
    Inst.Sources.assign({A, C, Addend});
    Mul.Dead = true;
    return true;
  }
  return false;
}

} // namespace

bool dxcb5::alg::FoldBranchCondition(AlgContext &Ctx, SsaBlock &Block) {
  if (Block.Kind != FlowKind::Branch || Block.Successors.size() != 2 ||
      Block.Condition.Kind != OperandKind::Temp || Block.Condition.Modifier ||
      Block.Condition.RelativeRegister != UINT32_MAX)
    return false;
  const auto &Def = Ctx.Defs[Block.Condition.Index];
  if (!Def.Valid() || Def.IsPhi || Def.IsAux)
    return false;
  const auto &Prod = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
  if (Prod.Dead)
    return false;
  Operand Inner;
  bool KnownMask = false;
  if (MatchZextBool(Ctx, Block.Condition, Inner, KnownMask) && KnownMask) {
    Block.Condition = Inner;
    Ctx.BumpUse(Inner);
    return true;
  }
  if (Prod.Opcode == D3D10_SB_OPCODE_NOT && Prod.Sources.size() == 1 &&
      Prod.Sources[0].RelativeRegister == UINT32_MAX &&
      !Prod.Sources[0].Modifier &&
      IsBoolMaskValue(Ctx.Ssa, Ctx.Defs, Prod.Sources[0])) {
    Operand Orig = Prod.Sources[0];
    Orig.Swizzle = ComposeSwizzle(Orig.Swizzle, Block.Condition.Swizzle);
    Block.Condition = Orig;
    Ctx.BumpUse(Orig);
    std::swap(Block.Successors[0], Block.Successors[1]);
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
    Orig.Swizzle = ComposeSwizzle(Orig.Swizzle, Block.Condition.Swizzle);
    Block.Condition = Orig;
    Ctx.BumpUse(Orig);
    if (Prod.Opcode == D3D10_SB_OPCODE_IEQ)
      std::swap(Block.Successors[0], Block.Successors[1]);
    return true;
  }
  return false;
}

bool dxcb5::alg::FoldRelativeIndexOffset(AlgContext &Ctx, SsaInst &Inst) {
  bool Changed = false;
  for (auto &Src : Inst.Sources) {
    if (Src.RelativeRegister == UINT32_MAX)
      continue;
    const auto &Def = Ctx.Defs[Src.RelativeRegister];
    if (!Def.Valid() || Def.IsPhi || Def.IsAux)
      continue;
    const auto &IdxInst = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (IdxInst.Dead || IdxInst.Saturate ||
        (IdxInst.Opcode != D3D10_SB_OPCODE_IADD &&
         IdxInst.Opcode != D3D10_SB_OPCODE_OR) ||
        !(IdxInst.Dest.Mask & (1u << Src.RelativeLane)))
      continue;
    const Operand *Base = nullptr;
    uint32_t ImmVal = 0;
    if (ExtractLaneImm(Ctx.Ssa, IdxInst.Sources[0], Src.RelativeLane, ImmVal) &&
        IdxInst.Sources[1].Kind == OperandKind::Temp)
      Base = &IdxInst.Sources[1];
    else if (ExtractLaneImm(Ctx.Ssa, IdxInst.Sources[1], Src.RelativeLane,
                            ImmVal) &&
             IdxInst.Sources[0].Kind == OperandKind::Temp)
      Base = &IdxInst.Sources[0];
    if (!Base || Base->Modifier || Base->RelativeRegister != UINT32_MAX)
      continue;
    if (IdxInst.Opcode == D3D10_SB_OPCODE_OR &&
        (KnownPossibleOnes(Ctx, *Base, Src.RelativeLane, 0) & ImmVal) != 0u)
      continue;
    uint32_t *Fixed =
        Src.Kind == OperandKind::ConstantBuffer
            ? &Src.Element
            : (Src.Kind == OperandKind::ImmediateConstantBuffer ? &Src.Index
                                                                : nullptr);
    if (!Fixed || uint64_t(*Fixed) + ImmVal > UINT32_MAX)
      continue;
    *Fixed = static_cast<uint32_t>(uint64_t(*Fixed) + ImmVal);
    Src.RelativeRegister = Base->Index;
    Src.RelativeLane = GetSwizzleLane(Base->Swizzle, Src.RelativeLane);
    Ctx.BumpUse(*Base);
    Changed = true;
  }
  return Changed;
}

bool dxcb5::alg::ForwardSingleUseDynamicCb(AlgContext &Ctx, uint32_t B,
                                           SsaInst &Inst) {
  if (!IsEligibleDynCbConsumer(Inst) || InstHasRelativeOperand(Inst))
    return false;
  for (size_t SI = 0; SI < Inst.Sources.size(); ++SI) {
    auto &Src = Inst.Sources[SI];
    if (Src.Kind != OperandKind::Temp || Src.RelativeRegister != UINT32_MAX ||
        Ctx.Stats.Count[Src.Index] != 1)
      continue;
    if (Src.Modifier && !InstructionOperandAcceptsFloatModifiers(Inst.Opcode, SI))
      continue;
    const auto &Def = Ctx.Defs[Src.Index];
    if (!Def.Valid() || Def.IsPhi || Def.IsAux || Def.Block != B)
      continue;
    auto &Move = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (!IsDynamicCbMov(Move))
      continue;
    const uint8_t Mod = Src.Modifier;
    const uint8_t Swz = ComposeSwizzle(Move.Sources[0].Swizzle, Src.Swizzle);
    Src = Move.Sources[0];
    Src.Swizzle = Swz;
    Src.Modifier = Mod;
    Move.Dead = true;
    return true;
  }
  return false;
}

bool dxcb5::alg::SimplifyIntAndBitInst(AlgContext &Ctx, SsaInst &Inst) {
  switch (Inst.Opcode) {
  case D3D10_SB_OPCODE_XOR: return SimplifyXorInst(Ctx, Inst);
  case D3D10_SB_OPCODE_NOT: return SimplifyNotInst(Ctx, Inst);
  case D3D10_SB_OPCODE_OR: return SimplifyBitwiseSelectOr(Ctx, Inst);
  case D3D10_SB_OPCODE_FTOU:
  case D3D10_SB_OPCODE_FTOI: return FoldFtouFtoiMovcTree(Ctx, Inst);
  case D3D10_SB_OPCODE_ITOF:
  case D3D10_SB_OPCODE_UTOF: {
    Operand Inner;
    bool KnownMask = false;
    if (Inst.Sources.size() == 1 &&
        MatchZextBool(Ctx, Inst.Sources[0], Inner, KnownMask) && KnownMask) {
      Inst.Opcode = D3D10_SB_OPCODE_AND;
      Inst.Sources.assign({Inner, MakeImm(kPosOneBits)});
      Ctx.BumpUse(Inner);
      return true;
    }
    return false;
  }
  case D3D10_SB_OPCODE_IMUL: {
    Operand InnerA, InnerB;
    bool KnownA = false, KnownB = false;
    if (Inst.Dest.Kind == OperandKind::Null &&
        Inst.AuxDest.Kind == OperandKind::Temp && Inst.Sources.size() == 2 &&
        MatchZextBool(Ctx, Inst.Sources[0], InnerA, KnownA) &&
        MatchZextBool(Ctx, Inst.Sources[1], InnerB, KnownB)) {
      Inst.Opcode = D3D10_SB_OPCODE_AND;
      Inst.Dest = Inst.AuxDest;
      Inst.AuxDest = Operand();
      if (KnownA && KnownB) {
        Inst.Sources[1] = InnerB;
        Ctx.BumpUse(InnerB);
      }
      return true;
    }
    return false;
  }
  case D3D10_SB_OPCODE_INE:
  case D3D10_SB_OPCODE_IEQ:
    if (Inst.Sources.size() != 2) return false;
    for (unsigned Side = 0; Side < 2; ++Side) {
      if (Inst.Sources[Side].Kind != OperandKind::Immediate ||
          Inst.Sources[Side].Index != 0u) continue;
      Operand Inner;
      bool KnownMask = false;
      if (MatchZextBool(Ctx, Inst.Sources[1 - Side], Inner, KnownMask) && KnownMask) {
        Inst.Opcode = Inst.Opcode == D3D10_SB_OPCODE_INE ? D3D10_SB_OPCODE_MOV : D3D10_SB_OPCODE_NOT;
        Inst.Sources.assign({Inner});
        Ctx.BumpUse(Inner);
        return true;
      }
    }
    return false;
  case D3D10_SB_OPCODE_IADD: return SimplifyIaddInst(Ctx, Inst);
  default: return false;
  }
}
