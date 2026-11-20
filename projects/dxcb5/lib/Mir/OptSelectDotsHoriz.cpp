// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "llvm/ADT/SmallVector.h"
#include <array>

using namespace dxcb5;

namespace {

Operand ApplySignFlip(Operand Eff, bool Neg) {
  if (!Neg)
    return Eff;
  if (Eff.Modifier == D3D10_SB_OPERAND_MODIFIER_ABS) {
    Eff.Modifier = D3D10_SB_OPERAND_MODIFIER_ABSNEG;
    return Eff;
  }
  if (Eff.Modifier == D3D10_SB_OPERAND_MODIFIER_ABSNEG) {
    Eff.Modifier = D3D10_SB_OPERAND_MODIFIER_ABS;
    return Eff;
  }
  return NegateMirOperand(Eff);
}

Operand StripNegModifier(Operand Op) {
  if (Op.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
    Op.Modifier = 0;
  else if (Op.Modifier == D3D10_SB_OPERAND_MODIFIER_ABSNEG)
    Op.Modifier = D3D10_SB_OPERAND_MODIFIER_ABS;
  return Op;
}

struct AddTreeState {
  const SsaBlock &Block;
  const std::vector<SsaDefSite> &Defs;
  const std::vector<uint32_t> &UseCount;
  uint32_t BlockId;
  uint32_t RootIdx;
  llvm::SmallVector<Operand, 4> Leaves;
  llvm::SmallVector<uint32_t, 3> MidAdds;
  bool Valid = true;
};

void CollectAddLeaves(AddTreeState &S, const Operand &Op, bool Neg) {
  if (!S.Valid || S.Leaves.size() > 4) {
    S.Valid = false;
    return;
  }
  const Operand Eff = ApplySignFlip(Op, Neg);
  if (Eff.Kind == OperandKind::Temp && Eff.RelativeRegister == UINT32_MAX &&
      (Eff.Modifier == 0 || Eff.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG) &&
      GetSwizzleLane(Eff.Swizzle, 0) == 0 && S.UseCount[Eff.Index] == 1) {
    const auto &D = S.Defs[Eff.Index];
    if (D.Valid() && !D.IsPhi && !D.IsAux && D.Block == S.BlockId &&
        D.Index < S.RootIdx) {
      const auto &Sub = S.Block.Insts[D.Index];
      if (!Sub.Dead && Sub.Opcode == D3D10_SB_OPCODE_ADD && !Sub.Precise &&
          !Sub.Saturate && Sub.Dest.Mask == 1u) {
        S.MidAdds.push_back(D.Index);
        const bool SubNeg = Eff.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG;
        CollectAddLeaves(S, Sub.Sources[0], SubNeg);
        CollectAddLeaves(S, Sub.Sources[1], SubNeg);
        return;
      }
    }
  }
  S.Leaves.push_back(Eff);
}

struct LeafVectorInfo {
  Operand FirstBase;
  unsigned Width = 0;
  bool AnyNeg = false;
  bool AllNeg = true;
  std::array<uint8_t, 4> Lanes{};
  std::array<bool, 4> Neg{};
};

bool ExtractSameBaseLeaves(llvm::ArrayRef<Operand> Leaves,
                           LeafVectorInfo &Out) {
  Out.Width = static_cast<unsigned>(Leaves.size());
  Out.FirstBase = StripNegModifier(Leaves[0]);
  const auto K = Out.FirstBase.Kind;
  if ((K != OperandKind::Temp && K != OperandKind::Input &&
       K != OperandKind::ConstantBuffer) ||
      Out.FirstBase.RelativeRegister != UINT32_MAX)
    return false;
  for (unsigned L = 0; L < Out.Width; ++L) {
    if (!SameOperandBase(Out.FirstBase, StripNegModifier(Leaves[L])))
      return false;
    Out.Lanes[L] = GetSwizzleLane(Leaves[L].Swizzle, 0);
    const bool IsN = Leaves[L].Modifier == D3D10_SB_OPERAND_MODIFIER_NEG ||
                     Leaves[L].Modifier == D3D10_SB_OPERAND_MODIFIER_ABSNEG;
    Out.Neg[L] = IsN;
    Out.AnyNeg |= IsN;
    Out.AllNeg &= IsN;
  }
  return true;
}

bool BuildSwizzledSource(SsaProgram &Ssa, const LeafVectorInfo &Info,
                         const Operand &Src, bool ApplyLeafNeg, Operand &Out) {
  if (Src.RelativeRegister != UINT32_MAX)
    return false;
  const unsigned W = Info.Width;
  if (Src.Kind == OperandKind::Immediate) {
    if (Src.Modifier)
      return false;
    if (!ApplyLeafNeg || !Info.AnyNeg) {
      Out = Src;
      return true;
    }
    if (Info.AllNeg) {
      Out = MakeImm(Src.Index ^ 0x80000000u);
      return true;
    }
    std::array<uint32_t, 4> Bits{};
    for (unsigned L = 0; L < 4; ++L) {
      const unsigned Idx = L < W ? L : 0;
      Bits[L] = Src.Index ^ (Info.Neg[Idx] ? 0x80000000u : 0u);
    }
    Out = {OperandKind::ImmediateVector, Ssa.InternLiteralVector(Bits), 0,
           MaskForWidth(W), kIdentitySwizzle};
    return true;
  }
  if (Src.Kind == OperandKind::ImmediateVector) {
    if (Src.Modifier)
      return false;
    const auto &Arr = Ssa.Mir.LiteralVectors[Src.Index];
    std::array<uint32_t, 4> Bits{};
    bool AllSame = true;
    for (unsigned L = 0; L < 4; ++L) {
      const unsigned Idx = L < W ? L : 0;
      uint32_t V = Arr[GetSwizzleLane(Src.Swizzle, Info.Lanes[Idx])];
      if (ApplyLeafNeg && Info.Neg[Idx])
        V ^= 0x80000000u;
      Bits[L] = V;
      if (L && Bits[L] != Bits[0])
        AllSame = false;
    }
    Out = AllSame ? MakeImm(Bits[0])
                  : Operand{OperandKind::ImmediateVector,
                            Ssa.InternLiteralVector(Bits), 0, MaskForWidth(W),
                            kIdentitySwizzle};
    return true;
  }
  if (ApplyLeafNeg && Info.AnyNeg && !Info.AllNeg)
    return false;
  Out = Src;
  Out.Mask = MaskForWidth(W);
  uint8_t Swz = 0;
  for (unsigned L = 0; L < 4; ++L) {
    const unsigned Idx = L < W ? L : 0;
    Swz = SetSwizzleLane(Swz, L,
                         GetSwizzleLane(Src.Swizzle, Info.Lanes[Idx]));
  }
  Out.Swizzle = Swz;
  if (ApplyLeafNeg && Info.AllNeg)
    Out = NegateMirOperand(Out);
  return true;
}

bool TryMatchVectorMulDot(SsaProgram &Ssa, SsaBlock &Block, SsaInst &Root,
                          const LeafVectorInfo &Info,
                          const std::vector<SsaDefSite> &Defs,
                          const std::vector<uint32_t> &UseCount,
                          llvm::ArrayRef<uint32_t> MidAdds) {
  if (Info.FirstBase.Kind != OperandKind::Temp || Info.FirstBase.Modifier != 0)
    return false;
  const auto &MD = Defs[Info.FirstBase.Index];
  if (!MD.Valid() || MD.IsPhi || MD.IsAux)
    return false;
  const auto &Mul = Ssa.Blocks[MD.Block].Insts[MD.Index];
  const unsigned W = Info.Width;
  if ((W < 3 && UseCount[Info.FirstBase.Index] != W) || Mul.Dead ||
      Mul.Opcode != D3D10_SB_OPCODE_MUL || Mul.Precise || Mul.Saturate ||
      Mul.Sources.size() != 2)
    return false;
  for (unsigned L = 0; L < W; ++L)
    if (!(Mul.Dest.Mask & (1u << Info.Lanes[L])))
      return false;
  Operand A, C;
  const bool Built =
      (BuildSwizzledSource(Ssa, Info, Mul.Sources[0], true, A) &&
       BuildSwizzledSource(Ssa, Info, Mul.Sources[1], false, C)) ||
      (BuildSwizzledSource(Ssa, Info, Mul.Sources[0], false, A) &&
       BuildSwizzledSource(Ssa, Info, Mul.Sources[1], true, C));
  if (!Built)
    return false;
  Root.Opcode = W == 2 ? D3D10_SB_OPCODE_DP2
                       : (W == 3 ? D3D10_SB_OPCODE_DP3 : D3D10_SB_OPCODE_DP4);
  Root.Sources = {A, C};
  for (uint32_t Mid : MidAdds)
    Block.Insts[Mid].Dead = true;
  return true;
}

bool EmitUnitWeightDot(SsaProgram &Ssa, SsaBlock &Block, SsaInst &Root,
                       const LeafVectorInfo &Info,
                       llvm::ArrayRef<uint32_t> MidAdds) {
  const unsigned W = Info.Width;
  if (W < 3)
    return false;
  Operand Vec = Info.FirstBase;
  Vec.Mask = MaskForWidth(W);
  uint8_t Swz = 0;
  std::array<uint32_t, 4> Ones{};
  bool AllSameOne = true;
  for (unsigned L = 0; L < 4; ++L) {
    const unsigned Idx = L < W ? L : 0;
    Swz = SetSwizzleLane(Swz, L, Info.Lanes[Idx]);
    Ones[L] = Info.Neg[Idx] ? 0xbf800000u : 0x3f800000u;
    if (L && Ones[L] != Ones[0])
      AllSameOne = false;
  }
  Vec.Swizzle = Swz;
  const Operand Lit = AllSameOne
                          ? MakeImm(Ones[0])
                          : Operand{OperandKind::ImmediateVector,
                                    Ssa.InternLiteralVector(Ones), 0,
                                    MaskForWidth(W), kIdentitySwizzle};
  Root.Opcode = W == 3 ? D3D10_SB_OPCODE_DP3 : D3D10_SB_OPCODE_DP4;
  Root.Sources = {Vec, Lit};
  for (uint32_t Mid : MidAdds)
    Block.Insts[Mid].Dead = true;
  return true;
}

} // namespace

namespace dxcb5 {
bool MatchHorizontalDots(SsaProgram &Ssa, const std::vector<SsaDefSite> &Defs,
                         const std::vector<uint32_t> &UseCount) {
  bool Changed = false;
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Block = Ssa.Blocks[B];
    for (size_t Rev = Block.Insts.size(); Rev > 0; --Rev) {
      const uint32_t RootIdx = static_cast<uint32_t>(Rev - 1);
      auto &Root = Block.Insts[RootIdx];
      if (Root.Dead || Root.Precise || Root.Opcode != D3D10_SB_OPCODE_ADD ||
          Root.Dest.Kind != OperandKind::Temp || Root.Dest.Mask != 1u)
        continue;
      AddTreeState Tree{Block, Defs, UseCount, B, RootIdx};
      CollectAddLeaves(Tree, Root.Sources[0], false);
      CollectAddLeaves(Tree, Root.Sources[1], false);
      if (!Tree.Valid || Tree.Leaves.size() < 2 || Tree.Leaves.size() > 4)
        continue;
      LeafVectorInfo Info;
      if (!ExtractSameBaseLeaves(Tree.Leaves, Info))
        continue;
      if (TryMatchVectorMulDot(Ssa, Block, Root, Info, Defs, UseCount,
                               Tree.MidAdds) ||
          EmitUnitWeightDot(Ssa, Block, Root, Info, Tree.MidAdds))
        Changed = true;
    }
  }
  return Changed;
}
} // namespace dxcb5
