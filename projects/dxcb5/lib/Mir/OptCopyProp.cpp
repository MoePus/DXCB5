// Distributed under the University of Illinois Open Source License.
#include "Mir/OptAlgebraicCommon.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/TimeProfiler.h"
#include <array>

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

uint32_t ApplyModToLiteral(uint32_t Bits, uint8_t Mod, bool IntNeg) {
  if (IntNeg && Mod == D3D10_SB_OPERAND_MODIFIER_NEG)
    return 0u - Bits;
  return ApplyFloatModToBits(Bits, Mod);
}

bool RewriteOperand(Operand &Op, const Operand &Repl, uint8_t ActiveSlots,
                    SsaProgram &Ssa, bool IntNeg = false) {
  if (Op.RelativeRegister != UINT32_MAX)
    return false;
  if (Repl.Kind == OperandKind::Immediate) {
    Op = MakeImm(ApplyModToLiteral(Repl.Index, Op.Modifier, IntNeg));
    return true;
  }
  if (Repl.Kind == OperandKind::ImmediateVector) {
    const auto &Bits = Ssa.Mir.LiteralVectors[Repl.Index];
    std::array<uint32_t, 4> Perm{};
    unsigned FirstSlot = 4;
    bool AllSame = true;
    bool Identity = !Op.Modifier;
    for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot) {
      if (!(ActiveSlots & (1u << Slot)))
        continue;
      const unsigned SrcLane = GetSwizzleLane(Op.Swizzle, Slot);
      const uint32_t B = ApplyModToLiteral(Bits[SrcLane], Op.Modifier, IntNeg);
      Perm[Slot] = B;
      if (FirstSlot == 4)
        FirstSlot = Slot;
      else if (B != Perm[FirstSlot])
        AllSame = false;
      if (SrcLane != Slot)
        Identity = false;
    }
    if (FirstSlot == 4)
      return false;
    if (AllSame) {
      Op = MakeImm(Perm[FirstSlot]);
      return true;
    }
    if (Identity) {
      Op = Repl;
      return true;
    }
    for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot)
      if (!(ActiveSlots & (1u << Slot)))
        Perm[Slot] = Perm[FirstSlot];
    Op = {OperandKind::ImmediateVector, Ssa.InternLiteralVector(Perm), 0,
          ActiveSlots, kIdentitySwizzle};
    return true;
  }
  if (Repl.RelativeRegister != UINT32_MAX && Op.Kind == OperandKind::Temp)
    return false;
  if (IntNeg && Repl.Modifier != 0)
    return false;
  Operand Next = Repl;
  Next.Mask = Op.Mask;
  Next.Swizzle = ComposeSwizzle(Repl.Swizzle, Op.Swizzle);
  Next.Modifier = IntNeg ? Op.Modifier
                         : ComposeFloatModifier(Repl.Modifier, Op.Modifier);
  Op = Next;
  return true;
}

bool TryProjectCombineVec(const SsaInst &Combine, const Operand &Op,
                          uint8_t ActiveSlots, bool AcceptsFloatMod,
                          bool AllowImmVec, SsaProgram &Ssa, Operand &Out,
                          bool IntNeg = false) {
  if (!ActiveSlots || Op.RelativeRegister != UINT32_MAX)
    return false;
  const unsigned W = static_cast<unsigned>(Combine.Sources.size());
  bool AllImm = true;
  bool SameImm = true;
  std::array<uint32_t, 4> Bits{};
  unsigned FirstSlot = 4;
  Operand Base;
  bool SameBase = true;
  uint8_t Swz = 0;
  unsigned FirstSrcLane = 0;

  for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot) {
    if (!(ActiveSlots & (1u << Slot)))
      continue;
    const unsigned K = GetSwizzleLane(Op.Swizzle, Slot);
    if (K >= W)
      return false;
    const Operand &LaneOp = Combine.Sources[K];
    if (LaneOp.RelativeRegister != UINT32_MAX)
      return false;
    if (LaneOp.Kind == OperandKind::Immediate && !LaneOp.Modifier) {
      const uint32_t B = ApplyModToLiteral(LaneOp.Index, Op.Modifier, IntNeg);
      Bits[Slot] = B;
      if (FirstSlot != 4 && B != Bits[FirstSlot])
        SameImm = false;
      SameBase = false;
    } else {
      AllImm = false;
      const auto Knd = LaneOp.Kind;
      if (Knd != OperandKind::Temp && Knd != OperandKind::Input &&
          Knd != OperandKind::ConstantBuffer) {
        SameBase = false;
      } else if (Base.Kind == OperandKind::Null) {
        Base = LaneOp;
        FirstSrcLane = GetSwizzleLane(LaneOp.Swizzle, 0);
        Swz = SetSwizzleLane(Swz, Slot, FirstSrcLane);
      } else {
        if (!SameOperandBase(Base, LaneOp))
          SameBase = false;
        Swz = SetSwizzleLane(Swz, Slot, GetSwizzleLane(LaneOp.Swizzle, 0));
      }
    }
    if (FirstSlot == 4)
      FirstSlot = Slot;
  }

  if (FirstSlot == 4)
    return false;
  if (AllImm) {
    if (SameImm) {
      Out = MakeImm(Bits[FirstSlot]);
      return true;
    }
    if (!AllowImmVec)
      return false;
    for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot)
      if (!(ActiveSlots & (1u << Slot)))
        Bits[Slot] = Bits[FirstSlot];
    Out = {OperandKind::ImmediateVector, Ssa.InternLiteralVector(Bits), 0,
           ActiveSlots, kIdentitySwizzle};
    return true;
  }
  if (!SameBase || Base.Kind == OperandKind::Null)
    return false;
  if (IntNeg && Base.Modifier != 0)
    return false;
  const uint8_t Mod =
      IntNeg ? Op.Modifier : ComposeFloatModifier(Base.Modifier, Op.Modifier);
  if (Mod && !AcceptsFloatMod && !(IntNeg && Mod == D3D10_SB_OPERAND_MODIFIER_NEG))
    return false;
  for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot)
    if (!(ActiveSlots & (1u << Slot)))
      Swz = SetSwizzleLane(Swz, Slot, FirstSrcLane);
  Out = Base;
  Out.Mask = Op.Mask;
  Out.Swizzle = Swz;
  Out.Modifier = Mod;
  return true;
}

bool TrySimplifyCombineVec(SsaProgram &Ssa, SsaInst &Inst, Operand &OutRepl) {
  if (Inst.Opcode != PseudoCombineVec || Inst.Dest.Kind != OperandKind::Temp ||
      Inst.Sources.empty())
    return false;
  const unsigned W = static_cast<unsigned>(Inst.Sources.size());
  bool AllImm = true;
  bool SameImm = true;
  std::array<uint32_t, 4> Bits{};
  for (unsigned L = 0; L < W; ++L) {
    if (Inst.Sources[L].Kind != OperandKind::Immediate ||
        Inst.Sources[L].Modifier) {
      AllImm = false;
      break;
    }
    Bits[L] = Inst.Sources[L].Index;
    if (L && Bits[L] != Bits[0])
      SameImm = false;
  }
  if (AllImm) {
    if (SameImm) {
      OutRepl = MakeImm(Bits[0]);
      return true;
    }
    OutRepl = {OperandKind::ImmediateVector, Ssa.InternLiteralVector(Bits), 0,
               Inst.Dest.Mask, kIdentitySwizzle};
    return true;
  }
  const auto &First = Inst.Sources[0];
  if (First.Kind != OperandKind::Temp && First.Kind != OperandKind::Input &&
      First.Kind != OperandKind::ConstantBuffer)
    return false;
  for (unsigned L = 1; L < W; ++L)
    if (!SameOperandBase(First, Inst.Sources[L]))
      return false;
  Operand Direct = First;
  Direct.Mask = Inst.Dest.Mask;
  uint8_t Swz = 0;
  const unsigned Fallback = GetSwizzleLane(First.Swizzle, 0);
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    const unsigned Src =
        L < W ? GetSwizzleLane(Inst.Sources[L].Swizzle, 0) : Fallback;
    Swz = SetSwizzleLane(Swz, L, Src);
  }
  Direct.Swizzle = Swz;
  OutRepl = Direct;
  return true;
}

bool FlattenCombineSrc(Operand &Src, const std::vector<SsaInst *> &CombineDefs,
                       uint32_t NextValueId) {
  bool Changed = false;
  unsigned Hops = 0;
  while (Src.Kind == OperandKind::Temp && Src.RelativeRegister == UINT32_MAX &&
         Src.Index < NextValueId && CombineDefs[Src.Index] && Hops++ < 16) {
    const SsaInst *Sub = CombineDefs[Src.Index];
    const unsigned K = GetSwizzleLane(Src.Swizzle, 0);
    if (K >= Sub->Sources.size())
      break;
    const Operand &Inner = Sub->Sources[K];
    if (Inner.RelativeRegister != UINT32_MAX)
      break;
    if (!Src.Modifier) {
      Src = Inner;
      Changed = true;
      continue;
    }
    if (Inner.Kind == OperandKind::Immediate && !Inner.Modifier) {
      Src = MakeImm(ApplyFloatModToBits(Inner.Index, Src.Modifier));
      return true;
    }
    if (Inner.Kind != OperandKind::Temp && Inner.Kind != OperandKind::Input &&
        Inner.Kind != OperandKind::ConstantBuffer)
      break;
    Operand Next = Inner;
    Next.Modifier = ComposeFloatModifier(Inner.Modifier, Src.Modifier);
    Src = Next;
    Changed = true;
  }
  return Changed;
}

struct BranchJoinInfo {
  uint32_t Head = UINT32_MAX;
  uint32_t TruePred = UINT32_MAX;
  uint32_t FalsePred = UINT32_MAX;
};

bool MatchTriangleJoin(const SsaProgram &Ssa,
                       const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds,
                       uint32_t B, uint32_t CandidateHead, uint32_t OtherPred,
                       BranchJoinInfo &Out) {
  const auto &HB = Ssa.Blocks[CandidateHead];
  if (HB.Kind != FlowKind::Branch || HB.Successors.size() != 2)
    return false;
  const uint32_t T = HB.Successors[0];
  const uint32_t F = HB.Successors[1];
  if (T == B && F == OtherPred && Preds[F].size() == 1 &&
      Preds[F][0] == CandidateHead) {
    Out = {CandidateHead, CandidateHead, F};
    return true;
  }
  if (F == B && T == OtherPred && Preds[T].size() == 1 &&
      Preds[T][0] == CandidateHead) {
    Out = {CandidateHead, T, CandidateHead};
    return true;
  }
  return false;
}

BranchJoinInfo FindBranchJoin(
    const SsaProgram &Ssa,
    const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds, uint32_t B) {
  BranchJoinInfo Out;
  if (Preds[B].size() != 2)
    return Out;
  const uint32_t P0 = Preds[B][0];
  const uint32_t P1 = Preds[B][1];
  if (MatchTriangleJoin(Ssa, Preds, B, P0, P1, Out) ||
      MatchTriangleJoin(Ssa, Preds, B, P1, P0, Out))
    return Out;
  if (Preds[P0].size() != 1 || Preds[P1].size() != 1 ||
      Preds[P0][0] != Preds[P1][0])
    return Out;
  const uint32_t H = Preds[P0][0];
  const auto &HB = Ssa.Blocks[H];
  if (HB.Kind != FlowKind::Branch || HB.Successors.size() != 2)
    return Out;
  if (HB.Successors[0] == P0 && HB.Successors[1] == P1)
    Out = {H, P0, P1};
  else if (HB.Successors[0] == P1 && HB.Successors[1] == P0)
    Out = {H, P1, P0};
  return Out;
}

void TryCollectSinglePhiReplacement(const SsaProgram &Ssa,
                                    const UseStats &Stats,
                                    const std::vector<SsaDefSite> &Defs,
                                    const BranchJoinInfo &Join,
                                    const SsaPhi &Phi,
                                    llvm::DenseMap<uint32_t, Operand> &Repl) {
  if (Phi.Dead || Phi.Inputs.empty())
    return;
  Operand Common;
  bool Trivial = true;
  for (const auto &In : Phi.Inputs) {
    if (In.Value.Kind == OperandKind::Temp && In.Value.Index == Phi.Result &&
        !In.Value.Modifier && In.Value.RelativeRegister == UINT32_MAX &&
        IsIdentitySwizzle(In.Value.Swizzle, Phi.Mask))
      continue;
    if (Common.Kind == OperandKind::Null) {
      Common = In.Value;
    } else if (!EqualCopySources(Common, In.Value)) {
      Trivial = false;
      break;
    }
  }
  if (Trivial && Common.Kind != OperandKind::Null &&
      Common.RelativeRegister == UINT32_MAX &&
      (!Common.Modifier || !Stats.NonFloatUse[Phi.Result])) {
    Repl[Phi.Result] = Common;
    return;
  }
  if (Join.Head == UINT32_MAX || Phi.Inputs.size() != 2 ||
      PopcountMask(Phi.Mask) != 1)
    return;
  const auto &Cond = Ssa.Blocks[Join.Head].Condition;
  if (Cond.Kind != OperandKind::Temp || Cond.Modifier ||
      Cond.RelativeRegister != UINT32_MAX || !IsBoolMaskValue(Ssa, Defs, Cond))
    return;
  Operand VT, VF;
  for (const auto &In : Phi.Inputs) {
    if (In.PredBlock == Join.TruePred)
      VT = In.Value;
    else if (In.PredBlock == Join.FalsePred)
      VF = In.Value;
  }
  if (VT.Kind == OperandKind::Immediate && VT.Index == 0xffffffffu &&
      VF.Kind == OperandKind::Immediate && VF.Index == 0u) {
    Operand CondOp = Cond;
    CondOp.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(Cond.Swizzle, 0));
    Repl[Phi.Result] = CondOp;
  }
}

void CollectPhiReplacements(
    const SsaProgram &Ssa, const UseStats &Stats,
    const std::vector<SsaDefSite> &Defs,
    const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds,
    llvm::DenseMap<uint32_t, Operand> &Repl) {
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    const BranchJoinInfo Join = FindBranchJoin(Ssa, Preds, B);
    for (const auto &Phi : Ssa.Blocks[B].Phis)
      TryCollectSinglePhiReplacement(Ssa, Stats, Defs, Join, Phi, Repl);
  }
}

void CollectInstReplacements(SsaProgram &Ssa, const UseStats &Stats,
                             llvm::DenseMap<uint32_t, Operand> &Repl) {
  for (auto &Block : Ssa.Blocks) {
    for (auto &Inst : Block.Insts) {
      if (Inst.Dead || Inst.Dest.Kind != OperandKind::Temp)
        continue;
      const uint32_t Id = Inst.Dest.Index;
      if (Inst.Opcode == PseudoCombineVec) {
        Operand Direct;
        if (TrySimplifyCombineVec(Ssa, Inst, Direct) &&
            (!Direct.Modifier || !Stats.NonFloatUse[Id]))
          Repl[Id] = Direct;
        continue;
      }
      if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate)
        continue;
      const auto &Src = Inst.Sources[0];
      if (Src.RelativeRegister != UINT32_MAX ||
          Src.Kind == OperandKind::ImmediateConstantBuffer ||
          Src.Kind == OperandKind::IndexableTemp ||
          (Src.Kind == OperandKind::Temp && Src.Index == Id) ||
          (Src.Modifier && Stats.NonFloatUse[Id]) ||
          (Stats.RelativeUse[Id] &&
           (Src.Kind != OperandKind::Temp || Src.Modifier)))
        continue;
      Repl[Id] = Src;
    }
  }
}

bool RewriteRelativeReg(const SsaProgram &Ssa,
                        const llvm::DenseMap<uint32_t, Operand> &Repl,
                        const std::vector<SsaInst *> &CombineDefs,
                        Operand &Op) {
  if (Op.RelativeRegister == UINT32_MAX)
    return false;
  auto RIt = Repl.find(Op.RelativeRegister);
  if (RIt != Repl.end() && RIt->second.Kind == OperandKind::Temp &&
      !RIt->second.Modifier && RIt->second.RelativeRegister == UINT32_MAX) {
    Op.RelativeRegister = RIt->second.Index;
    Op.RelativeLane = GetSwizzleLane(RIt->second.Swizzle, Op.RelativeLane);
    return true;
  }
  if (Op.RelativeRegister >= Ssa.NextValueId || !CombineDefs[Op.RelativeRegister])
    return false;
  const SsaInst *Sub = CombineDefs[Op.RelativeRegister];
  if (Op.RelativeLane >= Sub->Sources.size())
    return false;
  const Operand &Inner = Sub->Sources[Op.RelativeLane];
  if (Inner.Kind != OperandKind::Temp || Inner.Modifier ||
      Inner.RelativeRegister != UINT32_MAX)
    return false;
  Op.RelativeRegister = Inner.Index;
  Op.RelativeLane = GetSwizzleLane(Inner.Swizzle, 0);
  return true;
}

bool ApplyOpReplacement(SsaProgram &Ssa,
                        const llvm::DenseMap<uint32_t, Operand> &Repl,
                        const std::vector<SsaInst *> &CombineDefs, Operand &Op,
                        uint8_t ActiveSlots, bool AllowNonTemp,
                        bool AcceptsFloatMod, bool AllowImmVec,
                        bool IntNeg = false) {
  const bool Changed = RewriteRelativeReg(Ssa, Repl, CombineDefs, Op);
  if (Op.Kind != OperandKind::Temp)
    return Changed;
  auto It = Repl.find(Op.Index);
  if (It != Repl.end()) {
    if (!AllowNonTemp && It->second.Kind != OperandKind::Temp)
      return Changed;
    return RewriteOperand(Op, It->second, ActiveSlots, Ssa, IntNeg) || Changed;
  }
  if (Op.Index < Ssa.NextValueId && CombineDefs[Op.Index]) {
    Operand Projected;
    if (TryProjectCombineVec(*CombineDefs[Op.Index], Op, ActiveSlots,
                             AcceptsFloatMod, AllowImmVec, Ssa, Projected,
                             IntNeg) &&
        (AllowNonTemp || Projected.Kind == OperandKind::Temp)) {
      Op = Projected;
      return true;
    }
  }
  return Changed;
}

bool ApplyPhiInputReplacement(SsaProgram &Ssa,
                              const llvm::DenseMap<uint32_t, Operand> &Repl,
                              const std::vector<SsaInst *> &CombineDefs,
                              uint8_t PhiMask, Operand &Val) {
  if (Val.Kind != OperandKind::Temp)
    return false;
  auto It = Repl.find(Val.Index);
  if (It != Repl.end()) {
    if (!It->second.Modifier &&
        It->second.Kind != OperandKind::ImmediateVector &&
        It->second.RelativeRegister == UINT32_MAX)
      return RewriteOperand(Val, It->second, PhiMask, Ssa);
    return false;
  }
  if (Val.Index < Ssa.NextValueId && CombineDefs[Val.Index]) {
    Operand Projected;
    if (TryProjectCombineVec(*CombineDefs[Val.Index], Val, PhiMask, false,
                             false, Ssa, Projected) &&
        !Projected.Modifier &&
        Projected.Kind != OperandKind::ImmediateVector &&
        Projected.RelativeRegister == UINT32_MAX) {
      Val = Projected;
      return true;
    }
  }
  return false;
}

bool IsSampleLikeOpcode(uint32_t Op) {
  return Op == D3D10_SB_OPCODE_SAMPLE || Op == D3D10_SB_OPCODE_SAMPLE_B ||
         Op == D3D10_SB_OPCODE_SAMPLE_L || Op == D3D10_SB_OPCODE_SAMPLE_C ||
         Op == D3D10_SB_OPCODE_SAMPLE_C_LZ || Op == D3D10_SB_OPCODE_SAMPLE_D ||
         Op == D3D10_1_SB_OPCODE_GATHER4 || Op == D3D11_SB_OPCODE_GATHER4_C ||
         Op == D3D11_SB_OPCODE_GATHER4_PO || Op == D3D11_SB_OPCODE_GATHER4_PO_C ||
         Op == D3D10_1_SB_OPCODE_LOD;
}

bool ApplyInstReplacements(SsaProgram &Ssa,
                           const llvm::DenseMap<uint32_t, Operand> &Repl,
                           const std::vector<SsaInst *> &CombineDefs,
                           SsaInst &Inst) {
  bool Changed = false;
  if (Inst.Dest.RelativeRegister != UINT32_MAX)
    Changed |= ApplyOpReplacement(Ssa, Repl, CombineDefs, Inst.Dest, 0x1u,
                                  false, false, false);
  const uint8_t DMask = Inst.Dest.Kind == OperandKind::Null ? Inst.AuxDest.Mask
                                                            : Inst.Dest.Mask;
  const bool AllowLiteral = !IsSampleLikeOpcode(Inst.Opcode);
  const bool MovAcceptsMod =
      Inst.Opcode == D3D10_SB_OPCODE_MOV &&
      (Inst.Saturate ||
       (!Inst.Sources.empty() && Inst.Sources[0].Modifier != 0));
  const bool IntNeg = InstructionAcceptsIntNegModifier(Inst.Opcode);
  for (size_t S = 0; S < Inst.Sources.size(); ++S) {
    const uint8_t SMask = InstructionSourceSlotMask(Inst.Opcode, DMask, S);
    const bool CanLit = AllowLiteral || S != 0;
    const bool AcceptsMod =
        Inst.Opcode == PseudoCombineVec ||
        (!IsPseudoOpcode(Inst.Opcode) &&
         (InstructionOperandAcceptsFloatModifiers(Inst.Opcode, S) ||
          MovAcceptsMod));
    if (!CanLit && Inst.Sources[S].Kind == OperandKind::Temp) {
      auto It = Repl.find(Inst.Sources[S].Index);
      if (It != Repl.end() &&
          (It->second.Kind == OperandKind::Immediate ||
           It->second.Kind == OperandKind::ImmediateVector))
        continue;
    }
    Changed |= ApplyOpReplacement(Ssa, Repl, CombineDefs, Inst.Sources[S],
                                  SMask, true, AcceptsMod, CanLit, IntNeg);
  }
  return Changed;
}

bool ApplyAllReplacements(SsaProgram &Ssa,
                          const llvm::DenseMap<uint32_t, Operand> &Repl,
                          const std::vector<SsaInst *> &CombineDefs) {
  bool Changed = false;
  for (auto &Block : Ssa.Blocks) {
    for (auto &Phi : Block.Phis) {
      if (Phi.Dead)
        continue;
      for (auto &In : Phi.Inputs)
        Changed |= ApplyPhiInputReplacement(Ssa, Repl, CombineDefs, Phi.Mask,
                                            In.Value);
    }
    for (auto &Inst : Block.Insts)
      if (!Inst.Dead)
        Changed |= ApplyInstReplacements(Ssa, Repl, CombineDefs, Inst);
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
      Changed |= ApplyOpReplacement(Ssa, Repl, CombineDefs, Block.Condition,
                                    0x1u, true, false, false);
  }
  return Changed;
}

} // namespace

bool dxcb5::alg::FoldCopiesAndModifiers(SsaProgram &Ssa) {
  UseStats Stats;
  CollectUseStats(Ssa, Stats);

  std::vector<SsaInst *> CombineDefs(Ssa.NextValueId, nullptr);
  for (auto &Block : Ssa.Blocks) {
    for (auto &Inst : Block.Insts)
      if (!Inst.Dead && Inst.Opcode == PseudoCombineVec &&
          Inst.Dest.Kind == OperandKind::Temp &&
          Inst.Dest.Index < Ssa.NextValueId)
        CombineDefs[Inst.Dest.Index] = &Inst;
  }

  bool Changed = false;
  for (auto &Block : Ssa.Blocks) {
    for (auto &Inst : Block.Insts) {
      if (Inst.Dead || Inst.Opcode != PseudoCombineVec)
        continue;
      for (auto &Src : Inst.Sources)
        Changed |= FlattenCombineSrc(Src, CombineDefs, Ssa.NextValueId);
    }
  }

  std::vector<SsaDefSite> Defs;
  std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
  Ssa.BuildDefTable(Defs);
  Ssa.BuildPredecessors(Preds);

  llvm::DenseMap<uint32_t, Operand> Repl;
  CollectPhiReplacements(Ssa, Stats, Defs, Preds, Repl);
  CollectInstReplacements(Ssa, Stats, Repl);
  Changed |= ApplyAllReplacements(Ssa, Repl, CombineDefs);
  return Changed;
}

bool dxcb5::RunOptCopiesAndModifiers(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Copies/Modifiers", llvm::StringRef());
  return FoldCopiesAndModifiers(Ssa);
}
