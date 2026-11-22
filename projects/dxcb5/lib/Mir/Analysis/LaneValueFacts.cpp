// Distributed under the University of Illinois Open Source License.
#include "Mir/Analysis/LaneAnalysis.h"
#include "Mir/SsaAnalysis.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dxcb5 {
namespace {

float BitsToFloat(uint32_t Bits) {
  float F;
  std::memcpy(&F, &Bits, sizeof(F));
  return F;
}

struct FloatUpperBound {
  float Max = 0.0f;
  bool Strict = false;
};

struct BoundScope {
  llvm::DenseMap<uint64_t, FloatUpperBound> FloatMax;
  llvm::DenseMap<uint64_t, UnsignedRange> UintRange;
  struct FUndo { uint64_t Key; bool Had; FloatUpperBound Old; };
  struct UUndo { uint64_t Key; bool Had; UnsignedRange Old; };
  llvm::SmallVector<FUndo, 16> FLog;
  llvm::SmallVector<UUndo, 16> ULog;

  static uint64_t Key(uint32_t Id, uint8_t Lane) {
    return (uint64_t(Id) << 2) | (Lane & 3u);
  }
  std::pair<size_t, size_t> Mark() const { return {FLog.size(), ULog.size()}; }
  void Rollback(std::pair<size_t, size_t> M) {
    while (FLog.size() > M.first) {
      const auto E = FLog.pop_back_val();
      if (E.Had) FloatMax[E.Key] = E.Old; else FloatMax.erase(E.Key);
    }
    while (ULog.size() > M.second) {
      const auto E = ULog.pop_back_val();
      if (E.Had) UintRange[E.Key] = E.Old; else UintRange.erase(E.Key);
    }
  }
  void PushFloatMax(uint32_t Id, uint8_t Lane, float Max, bool Strict) {
    const uint64_t K = Key(Id, Lane);
    auto It = FloatMax.find(K);
    if (It != FloatMax.end() &&
        (It->second.Max < Max || (It->second.Max == Max && It->second.Strict)))
      return;
    const FloatUpperBound Prev = It != FloatMax.end() ? It->second : FloatUpperBound();
    FLog.push_back(FUndo{K, It != FloatMax.end(), Prev});
    FloatMax[K] = FloatUpperBound{Max, Strict};
  }
  void PushUintRange(uint32_t Id, uint8_t Lane, UnsignedRange R) {
    if (R.IsFull() || R.IsEmpty())
      return;
    const uint64_t K = Key(Id, Lane);
    auto It = UintRange.find(K);
    const UnsignedRange Old = It != UintRange.end() ? It->second : UnsignedRange::Top();
    const UnsignedRange Next = UnsignedRange::Intersect(Old, R);
    if (Next.IsEmpty() || (It != UintRange.end() && Next == Old))
      return;
    ULog.push_back(UUndo{K, It != UintRange.end(), Old});
    UintRange[K] = Next;
  }
};

bool EdgeDomChild(const SsaDomTree &Dom,
                  const std::vector<llvm::SmallVector<uint32_t, 4>> &ReachPreds,
                  uint32_t Parent, uint32_t Child) {
  if (Child >= ReachPreds.size())
    return false;
  for (uint32_t P : ReachPreds[Child])
    if (P != Parent && !Dom.Dominates(Child, P))
      return false;
  return true;
}

bool ExtractBranchBound(const SsaProgram &Ssa,
                        const std::vector<SsaDefSite> &Defs,
                        const Operand &Cond, bool TakenTrue,
                        BoundScope &Scope) {
  if (Cond.Kind != OperandKind::Temp || Cond.Modifier ||
      Cond.RelativeRegister != UINT32_MAX || Cond.Index >= Defs.size())
    return false;
  const auto &D = Defs[Cond.Index];
  if (!D.Valid() || D.IsPhi || D.IsAux)
    return false;
  const auto &Cmp = Ssa.Blocks[D.Block].Insts[D.Index];
  if (Cmp.Dead || Cmp.Sources.size() != 2)
    return false;
  const unsigned CLane = GetSwizzleLane(Cond.Swizzle, 0);
  if (!(Cmp.Dest.Mask & (1u << CLane)))
    return false;
  const Operand &A = Cmp.Sources[0];
  const Operand &B = Cmp.Sources[1];
  auto GetFloatImm = [&](const Operand &Op, float &Out) {
    if (Op.Modifier || Op.RelativeRegister != UINT32_MAX)
      return false;
    uint32_t Bits = 0;
    if (Op.Kind == OperandKind::Immediate)
      Bits = Op.Index;
    else if (Op.Kind == OperandKind::ImmediateVector &&
             Op.Index < Ssa.Mir.LiteralVectors.size())
      Bits = Ssa.Mir.LiteralVectors[Op.Index][GetSwizzleLane(Op.Swizzle, CLane)];
    else
      return false;
    Out = BitsToFloat(Bits);
    return std::isfinite(Out);
  };
  auto GetUintImm = [&](const Operand &Op, uint32_t &Out) {
    if (Op.Modifier || Op.RelativeRegister != UINT32_MAX)
      return false;
    if (Op.Kind == OperandKind::Immediate) {
      Out = Op.Index;
      return true;
    }
    if (Op.Kind == OperandKind::ImmediateVector &&
        Op.Index < Ssa.Mir.LiteralVectors.size()) {
      Out = Ssa.Mir.LiteralVectors[Op.Index][GetSwizzleLane(Op.Swizzle, CLane)];
      return true;
    }
    return false;
  };
  auto IsPlainTemp = [](const Operand &Op) {
    return Op.Kind == OperandKind::Temp && !Op.Modifier &&
           Op.RelativeRegister == UINT32_MAX;
  };
  float FImm = 0.0f;
  uint32_t UImm = 0;
  if (TakenTrue && Cmp.Opcode == D3D10_SB_OPCODE_GE &&
      GetFloatImm(A, FImm) && IsPlainTemp(B)) {
    Scope.PushFloatMax(B.Index, GetSwizzleLane(B.Swizzle, CLane), FImm, false);
    return true;
  }
  if (TakenTrue && Cmp.Opcode == D3D10_SB_OPCODE_LT &&
      IsPlainTemp(A) && GetFloatImm(B, FImm)) {
    Scope.PushFloatMax(A.Index, GetSwizzleLane(A.Swizzle, CLane), FImm, true);
    return true;
  }
  if (TakenTrue && Cmp.Opcode == D3D10_SB_OPCODE_UGE &&
      GetUintImm(A, UImm) && IsPlainTemp(B)) {
    Scope.PushUintRange(B.Index, GetSwizzleLane(B.Swizzle, CLane), {0u, UImm});
    return true;
  }
  if (TakenTrue && Cmp.Opcode == D3D10_SB_OPCODE_ULT &&
      IsPlainTemp(A) && GetUintImm(B, UImm) && UImm > 0) {
    Scope.PushUintRange(A.Index, GetSwizzleLane(A.Swizzle, CLane),
                        RefineUnsignedLtConst(UImm));
    return true;
  }
  if (!TakenTrue && Cmp.Opcode == D3D10_SB_OPCODE_UGE &&
      IsPlainTemp(A) && GetUintImm(B, UImm) && UImm > 0) {
    Scope.PushUintRange(A.Index, GetSwizzleLane(A.Swizzle, CLane),
                        RefineUnsignedLtConst(UImm));
    return true;
  }
  return false;
}

bool TryProveUintUpperBound(const SsaProgram &Ssa,
                            const std::vector<SsaDefSite> &Defs,
                            const BoundScope &Scope, uint32_t Reg, uint8_t Lane,
                            uint32_t &OutMax, unsigned Depth = 0) {
  if (Depth > 4 || Reg >= Defs.size())
    return false;
  auto UIt = Scope.UintRange.find(BoundScope::Key(Reg, Lane));
  if (UIt != Scope.UintRange.end() && !UIt->second.IsFull()) {
    OutMax = UIt->second.Hi;
    return true;
  }
  const auto &D = Defs[Reg];
  if (!D.Valid() || D.IsPhi || D.IsAux)
    return false;
  const auto &I = Ssa.Blocks[D.Block].Insts[D.Index];
  if (I.Dead || !(I.Dest.Mask & (1u << Lane)))
    return false;
  if (I.Opcode == D3D10_SB_OPCODE_MOV && I.Sources.size() == 1 &&
      !I.Saturate && I.Sources[0].Kind == OperandKind::Temp &&
      !I.Sources[0].Modifier && I.Sources[0].RelativeRegister == UINT32_MAX) {
    return TryProveUintUpperBound(Ssa, Defs, Scope, I.Sources[0].Index,
                                  GetSwizzleLane(I.Sources[0].Swizzle, Lane),
                                  OutMax, Depth + 1);
  }
  if ((I.Opcode == D3D10_SB_OPCODE_AND || I.Opcode == D3D10_SB_OPCODE_UMIN) &&
      I.Sources.size() == 2) {
    const UnsignedRange R0 =
        I.Sources[0].Kind == OperandKind::Immediate
            ? UnsignedRange::Single(I.Sources[0].Index)
            : UnsignedRange::Top();
    const UnsignedRange R1 =
        I.Sources[1].Kind == OperandKind::Immediate
            ? UnsignedRange::Single(I.Sources[1].Index)
            : UnsignedRange::Top();
    const UnsignedRange Out =
        I.Opcode == D3D10_SB_OPCODE_AND ? EvalAndUnsigned(R0, R1)
                                        : EvalUminRange(R0, R1);
    if (!Out.IsFull()) {
      OutMax = Out.Hi;
      return true;
    }
  }
  if (I.Opcode == D3D10_SB_OPCODE_FTOU && I.Sources.size() == 1 &&
      I.Sources[0].Kind == OperandKind::Temp && !I.Sources[0].Modifier &&
      I.Sources[0].RelativeRegister == UINT32_MAX) {
    const uint8_t SL = GetSwizzleLane(I.Sources[0].Swizzle, Lane);
    auto FIt = Scope.FloatMax.find(BoundScope::Key(I.Sources[0].Index, SL));
    if (FIt == Scope.FloatMax.end())
      return false;
    const float F = FIt->second.Max;
    if (!std::isfinite(F) || F >= 2147483648.0f)
      return false;
    if (F <= 0.0f) {
      OutMax = 0u;
      return true;
    }
    double Fl = std::floor(static_cast<double>(F));
    if (FIt->second.Strict && Fl == static_cast<double>(F) && Fl >= 1.0)
      Fl -= 1.0;
    OutMax = static_cast<uint32_t>(Fl);
    return true;
  }
  return false;
}

uint32_t ComputeReadUpperElem(const SsaProgram &Ssa,
                              const std::vector<SsaDefSite> &Defs,
                              const BoundScope &Scope, const Operand &Src,
                              uint32_t Elems) {
  if (Src.RelativeRegister == UINT32_MAX)
    return std::min(Src.Element, Elems - 1u);
  uint32_t DynMax = 0;
  if (!TryProveUintUpperBound(Ssa, Defs, Scope, Src.RelativeRegister,
                              Src.RelativeLane, DynMax))
    return Elems - 1u;
  const uint64_t Sum = uint64_t(Src.Element) + uint64_t(DynMax);
  return static_cast<uint32_t>(std::min<uint64_t>(Sum, Elems - 1u));
}

void RecordBlockIndexReads(const SsaProgram &Ssa,
                           const std::vector<SsaDefSite> &Defs,
                           const BoundScope &Scope, const SsaBlock &Blk,
                           std::vector<uint32_t> &MaxElem) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  for (const auto &Inst : Blk.Insts) {
    if (Inst.Dead)
      continue;
    for (const auto &Src : Inst.Sources) {
      if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX)
        continue;
      const uint32_t Elems = Ssa.Mir.IndexableTemps[Src.Index].Elements;
      if (Elems == 0)
        continue;
      const uint32_t Hi = ComputeReadUpperElem(Ssa, Defs, Scope, Src, Elems);
      if (MaxElem[Src.Index] == UINT32_MAX || Hi > MaxElem[Src.Index])
        MaxElem[Src.Index] = Hi;
    }
  }
}

} // namespace

bool IsKnownBoolMask(const SsaProgram &Ssa,
                     const std::vector<SsaDefSite> &Defs, const Operand &Op,
                     unsigned Depth) {
  if (Op.Kind == OperandKind::Immediate && !Op.Modifier &&
      (Op.Index == 0u || Op.Index == 0xffffffffu))
    return true;
  if (Depth > 6 || Op.Kind != OperandKind::Temp || Op.Modifier ||
      Op.RelativeRegister != UINT32_MAX || Op.Index >= Defs.size())
    return false;
  const auto &D = Defs[Op.Index];
  if (!D.Valid() || D.IsAux)
    return false;
  if (D.IsPhi) {
    const auto &Phi = Ssa.Blocks[D.Block].Phis[D.Index];
    if (Depth > 3 || Phi.Dead || Phi.Inputs.empty() || Phi.Inputs.size() > 4)
      return false;
    for (const auto &In : Phi.Inputs)
      if (!IsKnownBoolMask(Ssa, Defs, In.Value, Depth + 1))
        return false;
    return true;
  }
  const auto &P = Ssa.Blocks[D.Block].Insts[D.Index];
  if (P.Dead)
    return false;
  if (InstructionIsCompare(P.Opcode))
    return true;
  if (P.Opcode == D3D10_SB_OPCODE_NOT)
    return P.Sources.size() == 1 &&
           IsKnownBoolMask(Ssa, Defs, P.Sources[0], Depth + 1);
  if (P.Opcode == D3D10_SB_OPCODE_AND || P.Opcode == D3D10_SB_OPCODE_OR ||
      P.Opcode == D3D10_SB_OPCODE_XOR)
    return P.Sources.size() == 2 &&
           IsKnownBoolMask(Ssa, Defs, P.Sources[0], Depth + 1) &&
           IsKnownBoolMask(Ssa, Defs, P.Sources[1], Depth + 1);
  if (P.Opcode == D3D10_SB_OPCODE_MOVC)
    return P.Sources.size() == 3 &&
           IsKnownBoolMask(Ssa, Defs, P.Sources[1], Depth + 1) &&
           IsKnownBoolMask(Ssa, Defs, P.Sources[2], Depth + 1);
  return false;
}

bool IsKnownNonNegativeFloat(const SsaProgram &Ssa,
                             const std::vector<SsaDefSite> &Defs,
                             const Operand &Op, uint8_t Mask,
                             bool AllowNegZero, unsigned Depth) {
  if (Op.Modifier == D3D10_SB_OPERAND_MODIFIER_ABS)
    return true;
  if (Depth > 6 || Op.Modifier != 0 || Op.RelativeRegister != UINT32_MAX ||
      Mask == 0)
    return false;
  auto BitsNonNeg = [&](uint32_t Bits) {
    const float F = BitsToFloat(Bits);
    if (std::isnan(F))
      return false;
    return F > 0.0f || Bits == kFloatPosZeroBits ||
           (AllowNegZero && Bits == kFloatNegZeroBits);
  };
  if (Op.Kind == OperandKind::Immediate)
    return BitsNonNeg(Op.Index);
  if (Op.Kind == OperandKind::ImmediateVector &&
      Op.Index < Ssa.Mir.LiteralVectors.size()) {
    const auto &Arr = Ssa.Mir.LiteralVectors[Op.Index];
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((Mask & (1u << L)) &&
          !BitsNonNeg(Arr[GetSwizzleLane(Op.Swizzle, L)]))
        return false;
    return true;
  }
  if (Op.Kind != OperandKind::Temp || Op.Index >= Defs.size())
    return false;
  const auto &D = Defs[Op.Index];
  if (!D.Valid() || D.IsPhi || D.IsAux)
    return false;
  const auto &P = Ssa.Blocks[D.Block].Insts[D.Index];
  if (P.Dead)
    return false;
  uint8_t InnerMask = 0;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(Mask & (1u << L)))
      continue;
    const unsigned SL = GetSwizzleLane(Op.Swizzle, L);
    if (!(P.Dest.Mask & (1u << SL)))
      return false;
    InnerMask |= static_cast<uint8_t>(1u << SL);
  }
  if (P.Saturate || P.Opcode == D3D10_SB_OPCODE_EXP)
    return true;
  if (P.Opcode == D3D10_SB_OPCODE_SQRT)
    return AllowNegZero;
  if (P.Opcode == D3D10_SB_OPCODE_MOV && P.Sources.size() == 1)
    return IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[0], InnerMask,
                                   AllowNegZero, Depth + 1);
  if (P.Opcode == PseudoCombineVec) {
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      if (!(InnerMask & (1u << L)))
        continue;
      if (L >= P.Sources.size() ||
          !IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[L], 1u, AllowNegZero,
                                   Depth + 1))
        return false;
    }
    return true;
  }
  if (P.Opcode == D3D10_SB_OPCODE_MAX && P.Sources.size() == 2)
    return IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[0], InnerMask,
                                   AllowNegZero, Depth + 1) ||
           IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[1], InnerMask,
                                   AllowNegZero, Depth + 1);
  if ((P.Opcode == D3D10_SB_OPCODE_MIN || P.Opcode == D3D10_SB_OPCODE_ADD ||
       P.Opcode == D3D10_SB_OPCODE_MUL) &&
      P.Sources.size() == 2) {
    if (P.Opcode == D3D10_SB_OPCODE_MUL &&
        EqualCopySources(P.Sources[0], P.Sources[1]))
      return true;
    return IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[0], InnerMask,
                                   AllowNegZero, Depth + 1) &&
           IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[1], InnerMask,
                                   AllowNegZero, Depth + 1);
  }
  if (P.Opcode == D3D10_SB_OPCODE_MAD && P.Sources.size() == 3) {
    const bool ProdOk =
        EqualCopySources(P.Sources[0], P.Sources[1]) ||
        (IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[0], InnerMask,
                                 AllowNegZero, Depth + 1) &&
         IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[1], InnerMask,
                                 AllowNegZero, Depth + 1));
    return ProdOk && IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[2], InnerMask,
                                             AllowNegZero, Depth + 1);
  }
  if ((P.Opcode == D3D10_SB_OPCODE_DP2 || P.Opcode == D3D10_SB_OPCODE_DP3 ||
       P.Opcode == D3D10_SB_OPCODE_DP4) &&
      P.Sources.size() == 2) {
    if (EqualCopySources(P.Sources[0], P.Sources[1]))
      return true;
    const uint8_t DotMask =
        P.Opcode == D3D10_SB_OPCODE_DP2 ? 0x3u
        : (P.Opcode == D3D10_SB_OPCODE_DP3 ? 0x7u : 0xfu);
    return IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[0], DotMask,
                                   AllowNegZero, Depth + 1) &&
           IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[1], DotMask,
                                   AllowNegZero, Depth + 1);
  }
  // rcp(-0.0) = rsq(-0.0) = -inf (negative!), so input MUST exclude -0.0 (M1).
  if ((P.Opcode == D3D10_SB_OPCODE_RSQ || P.Opcode == D3D11_SB_OPCODE_RCP) &&
      P.Sources.size() == 1)
    return IsKnownNonNegativeFloat(Ssa, Defs, P.Sources[0], InnerMask,
                                   /*AllowNegZero=*/false, Depth + 1);
  return false;
}

void ComputeIndexableMaxReadElements(const SsaProgram &Ssa,
                                     const SsaDomTree &Dom,
                                     std::vector<uint32_t> &MaxElem) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  MaxElem.assign(NumX, UINT32_MAX);
  if (NumX == 0 || Dom.RpoOrder.empty())
    return;
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<llvm::SmallVector<uint32_t, 4>> ReachPreds;
  BuildReachablePreds(Ssa, Dom, ReachPreds);

  BoundScope Scope;
  struct Frame {
    uint32_t Block;
    uint32_t Parent;
    std::pair<size_t, size_t> Mark;
    bool HasEdge;
    bool TakenTrue;
    bool Exit;
  };
  const std::pair<size_t, size_t> ZeroMark(0, 0);
  llvm::SmallVector<Frame, 64> Stack;
  Stack.push_back(Frame{0, UINT32_MAX, ZeroMark, false, false, false});
  while (!Stack.empty()) {
    const auto Cur = Stack.pop_back_val();
    if (Cur.Exit) {
      Scope.Rollback(Cur.Mark);
      continue;
    }
    const auto Mark = Scope.Mark();
    if (Cur.HasEdge)
      ExtractBranchBound(Ssa, Defs, Ssa.Blocks[Cur.Parent].Condition,
                         Cur.TakenTrue, Scope);
    const auto &Blk = Ssa.Blocks[Cur.Block];
    RecordBlockIndexReads(Ssa, Defs, Scope, Blk, MaxElem);
    Stack.push_back(Frame{Cur.Block, Cur.Parent, Mark, false, false, true});
    const bool IsBranch = Blk.Kind == FlowKind::Branch &&
                          Blk.Successors.size() == 2 &&
                          Blk.Successors[0] != Blk.Successors[1];
    for (uint32_t Child : Dom.Children[Cur.Block]) {
      const bool EdgeDom =
          IsBranch && EdgeDomChild(Dom, ReachPreds, Cur.Block, Child);
      const bool IsTrue = EdgeDom && Child == Blk.Successors[0];
      const bool IsFalse = EdgeDom && Child == Blk.Successors[1];
      Stack.push_back(Frame{Child, Cur.Block, ZeroMark, IsTrue || IsFalse, IsTrue, false});
    }
  }
}

} // namespace dxcb5
