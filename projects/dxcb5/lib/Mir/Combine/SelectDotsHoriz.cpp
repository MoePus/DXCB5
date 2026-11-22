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

bool IsBroadcastScalar(const Operand &Op, uint8_t Mask, Operand &OutScalar) {
  if (Op.RelativeRegister != UINT32_MAX ||
      Op.Kind == OperandKind::ImmediateVector)
    return false;
  if (Op.Kind == OperandKind::Immediate) {
    OutScalar = Op;
    return true;
  }
  if (Op.Kind != OperandKind::Temp && Op.Kind != OperandKind::Input &&
      Op.Kind != OperandKind::ConstantBuffer)
    return false;
  unsigned Lane = 4;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(Mask & (1u << L)))
      continue;
    const unsigned SL = GetSwizzleLane(Op.Swizzle, L);
    if (Lane == 4)
      Lane = SL;
    else if (SL != Lane)
      return false;
  }
  if (Lane == 4)
    return false;
  OutScalar = Op;
  OutScalar.Mask = 1u;
  OutScalar.Swizzle = BroadcastLaneSwizzle(Lane);
  return true;
}

bool TryBuildDirectVec(SsaProgram &Ssa, llvm::ArrayRef<Operand> Lanes,
                       Operand &Out) {
  const unsigned W = static_cast<unsigned>(Lanes.size());
  const auto &First = Lanes[0];
  if (First.RelativeRegister != UINT32_MAX ||
      (First.Kind != OperandKind::Temp && First.Kind != OperandKind::Input &&
       First.Kind != OperandKind::ConstantBuffer))
    return false;
  for (unsigned L = 1; L < W; ++L)
    if (!SameOperandBase(First, Lanes[L]))
      return false;
  Out = First;
  Out.Mask = MaskForWidth(W);
  uint8_t Swz = 0;
  const unsigned Fallback = GetSwizzleLane(First.Swizzle, 0);
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    const unsigned SL = L < W ? GetSwizzleLane(Lanes[L].Swizzle, 0) : Fallback;
    Swz = SetSwizzleLane(Swz, L, SL);
  }
  Out.Swizzle = Swz;
  return true;
}

bool ExtractColumnLanes(const SsaProgram &Ssa,
                        const std::vector<SsaDefSite> &Defs,
                        const std::vector<uint32_t> &UseCount, uint32_t BlockId,
                        const Operand &Op, unsigned W,
                        std::array<Operand, 4> &Col, uint32_t &CombineIdx) {
  CombineIdx = UINT32_MAX;
  if (Op.RelativeRegister != UINT32_MAX || Op.Modifier != 0)
    return false;
  if (Op.Kind == OperandKind::Temp && Op.Index < Defs.size()) {
    const auto &D = Defs[Op.Index];
    const bool Local = D.Valid() && !D.IsPhi && !D.IsAux && D.Block == BlockId;
    const SsaInst *P = Local ? &Ssa.Blocks[D.Block].Insts[D.Index] : nullptr;
    if (P && !P->Dead && P->Opcode == PseudoCombineVec &&
        P->Sources.size() >= W) {
      for (unsigned R = 0; R < W; ++R) {
        const unsigned K = GetSwizzleLane(Op.Swizzle, R);
        if (K >= P->Sources.size())
          return false;
        Col[R] = P->Sources[K];
      }
      if (UseCount[Op.Index] == 1)
        CombineIdx = D.Index;
      return true;
    }
  }
  if (Op.Kind != OperandKind::Temp && Op.Kind != OperandKind::Input &&
      Op.Kind != OperandKind::ConstantBuffer)
    return false;
  for (unsigned R = 0; R < W; ++R) {
    Col[R] = Op;
    Col[R].Mask = 1u;
    Col[R].Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(Op.Swizzle, R));
  }
  return true;
}

bool TryConvertTransposedMadToDots(SsaProgram &Ssa, SsaBlock &Block,
                                   uint32_t BlockId, uint32_t RootIdx,
                                   const std::vector<SsaDefSite> &Defs,
                                   const std::vector<uint32_t> &UseCount) {
  const auto &Root = Block.Insts[RootIdx];
  const uint8_t Mask = Root.Dest.Mask;
  const unsigned W = (Mask == 0x7u) ? 3u : (Mask == 0xfu ? 4u : 0u);
  if (W == 0 || Root.Dead || Root.Precise ||
      Root.Opcode != D3D10_SB_OPCODE_MAD || Root.Dest.Kind != OperandKind::Temp)
    return false;

  std::array<Operand, 4> Scalars{};
  std::array<std::array<Operand, 4>, 4> Cols{};
  llvm::SmallVector<uint32_t, 8> DeadInsts;
  unsigned GatherCount = 0;
  uint32_t CurIdx = RootIdx;

  for (int Step = static_cast<int>(W) - 1; Step >= 0; --Step) {
    const auto &Inst = Block.Insts[CurIdx];
    const uint32_t ExpectedOp =
        Step == 0 ? D3D10_SB_OPCODE_MUL : D3D10_SB_OPCODE_MAD;
    if (Inst.Dead || Inst.Precise || Inst.Opcode != ExpectedOp ||
        Inst.Dest.Mask != Mask || (CurIdx != RootIdx && Inst.Saturate))
      return false;
    Operand Sc;
    std::array<Operand, 4> Col;
    uint32_t CombIdx = UINT32_MAX;
    bool Matched = false;
    for (unsigned Side = 0; Side < 2; ++Side) {
      if (IsBroadcastScalar(Inst.Sources[Side], Mask, Sc) &&
          ExtractColumnLanes(Ssa, Defs, UseCount, BlockId,
                             Inst.Sources[1 - Side], W, Col, CombIdx)) {
        Matched = true;
        break;
      }
    }
    if (!Matched)
      return false;
    Scalars[Step] = Sc;
    Cols[Step] = Col;
    if (CombIdx != UINT32_MAX) {
      ++GatherCount;
      DeadInsts.push_back(CombIdx);
    }
    if (CurIdx != RootIdx)
      DeadInsts.push_back(CurIdx);
    if (Step == 0)
      break;
    const Operand &Acc = Inst.Sources[2];
    if (Acc.Kind != OperandKind::Temp || Acc.Modifier ||
        Acc.RelativeRegister != UINT32_MAX || UseCount[Acc.Index] != 1)
      return false;
    for (unsigned L = 0; L < W; ++L)
      if (GetSwizzleLane(Acc.Swizzle, L) != L)
        return false;
    const auto &AD = Defs[Acc.Index];
    if (!AD.Valid() || AD.IsPhi || AD.IsAux || AD.Block != BlockId)
      return false;
    CurIdx = AD.Index;
  }
  if (GatherCount < 2)
    return false;

  // Canonicalize term order by Scalar swizzle lane if it's a permutation.
  std::array<unsigned, 4> Perm{0, 1, 2, 3};
  for (unsigned I = 0; I < W; ++I)
    for (unsigned J = I + 1; J < W; ++J)
      if (GetSwizzleLane(Scalars[Perm[J]].Swizzle, 0) <
          GetSwizzleLane(Scalars[Perm[I]].Swizzle, 0))
        std::swap(Perm[I], Perm[J]);

  std::array<Operand, 4> OrderedSc{};
  for (unsigned K = 0; K < W; ++K)
    OrderedSc[K] = Scalars[Perm[K]];
  Operand VecS;
  if (!TryBuildDirectVec(Ssa, llvm::makeArrayRef(OrderedSc.data(), W), VecS))
    return false;

  std::array<Operand, 4> Rows{};
  for (unsigned R = 0; R < W; ++R) {
    std::array<Operand, 4> RowLanes{};
    for (unsigned K = 0; K < W; ++K)
      RowLanes[K] = Cols[Perm[K]][R];
    if (!TryBuildDirectVec(Ssa, llvm::makeArrayRef(RowLanes.data(), W), Rows[R]))
      return false;
  }

  const uint32_t DotOp = W == 3 ? D3D10_SB_OPCODE_DP3 : D3D10_SB_OPCODE_DP4;
  const bool Sat = Root.Saturate;
  const Operand OrigDest = Root.Dest;
  llvm::SmallVector<SsaInst, 5> NewInsts;
  SsaInst Combine;
  Combine.Opcode = PseudoCombineVec;
  Combine.Dest = OrigDest;
  for (unsigned R = 0; R < W; ++R) {
    const uint32_t DotId = Ssa.AllocValue();
    SsaInst D;
    D.Opcode = DotOp;
    D.Saturate = Sat;
    D.Dest = MakeSsaTemp(DotId, 1u, 0u);
    D.Sources.assign({VecS, Rows[R]});
    NewInsts.push_back(std::move(D));
    Combine.Sources.push_back(MakeSsaTemp(DotId, 1u, 0u));
  }
  NewInsts.push_back(std::move(Combine));

  for (uint32_t DeadIdx : DeadInsts)
    Block.Insts[DeadIdx].Dead = true;
  Block.Insts[RootIdx].Dead = true;
  Block.Insts.insert(Block.Insts.begin() + RootIdx + 1,
                     std::make_move_iterator(NewInsts.begin()),
                     std::make_move_iterator(NewInsts.end()));
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

bool MatchTransposedGatherDots(SsaProgram &Ssa,
                               const std::vector<SsaDefSite> &Defs,
                               const std::vector<uint32_t> &UseCount) {
  bool Changed = false;
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Block = Ssa.Blocks[B];
    for (size_t Rev = Block.Insts.size(); Rev > 0; --Rev) {
      const uint32_t RootIdx = static_cast<uint32_t>(Rev - 1);
      if (TryConvertTransposedMadToDots(Ssa, Block, B, RootIdx, Defs, UseCount))
        return true;
    }
  }
  return Changed;
}
} // namespace dxcb5
