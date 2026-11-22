// Distributed under the University of Illinois Open Source License.
#include "Mir/Analysis/LaneAnalysis.h"
#include "Mir/OptPipeline.h"
#include "Mir/SsaAnalysis.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

using namespace dxcb5;

namespace {

using ScopedRangeMap = ScopedSignedRangeMap;

struct SelectConst {
  Operand Cond{OperandKind::Null};
  uint32_t TrueBits = 0, FalseBits = 0;
  uint8_t Depth = 0;
  bool Valid = false;
};

struct LaneConstTable {
  std::vector<uint8_t> KnownMask;
  std::vector<std::array<uint32_t, 4>> Bits;
  std::vector<std::array<SelectConst, 4>> Selects;
  explicit LaneConstTable(uint32_t N) : KnownMask(N, 0), Bits(N), Selects(N) {}
  void Set(uint32_t Id, unsigned Lane, uint32_t Val) {
    if (Id >= KnownMask.size()) return;
    KnownMask[Id] |= static_cast<uint8_t>(1u << Lane);
    Bits[Id][Lane] = Val;
    Selects[Id][Lane].Valid = false;
  }
  void SetSelect(uint32_t Id, unsigned Lane, const SelectConst &SC) {
    if (Id >= Selects.size() || !SC.Valid) return;
    if (SC.TrueBits == SC.FalseBits) { Set(Id, Lane, SC.TrueBits); return; }
    Selects[Id][Lane] = SC;
  }
  bool GetOperandLane(const SsaProgram &Ssa, const Operand &Op, unsigned Slot,
                      bool IntNeg, uint32_t &Out,
                      const ScopedRangeMap *Rm = nullptr) const {
    if (Op.RelativeRegister != UINT32_MAX) return false;
    if (Op.Kind == OperandKind::Immediate) {
      Out = ApplyLaneMod(Op.Index, Op.Modifier, IntNeg);
      return true;
    }
    const unsigned SL = GetSwizzleLane(Op.Swizzle, Slot);
    if (Op.Kind == OperandKind::ImmediateVector &&
        Op.Index < Ssa.Mir.LiteralVectors.size()) {
      Out = ApplyLaneMod(Ssa.Mir.LiteralVectors[Op.Index][SL], Op.Modifier, IntNeg);
      return true;
    }
    if (Op.Kind == OperandKind::Temp && Op.Index < KnownMask.size() &&
        (KnownMask[Op.Index] & (1u << SL))) {
      Out = ApplyLaneMod(Bits[Op.Index][SL], Op.Modifier, IntNeg);
      return true;
    }
    if (Rm) {
      Operand Bare = Op;
      Bare.Modifier = 0;
      const SignedRange R = Rm->Lookup(MakeLaneRangeKey(Bare, Slot));
      if (R.IsSingle()) {
        Out = ApplyLaneMod(static_cast<uint32_t>(R.Lo), Op.Modifier, IntNeg);
        return true;
      }
    }
    return false;
  }
  SignedRange GetOperandRange(const SsaProgram &Ssa, const ScopedRangeMap &Rm,
                              const Operand &Op, unsigned Slot) const {
    uint32_t C = 0;
    return GetOperandLane(Ssa, Op, Slot, true, C, &Rm)
               ? SignedRange::Single(static_cast<int32_t>(C))
               : Rm.Lookup(MakeLaneRangeKey(Op, Slot));
  }
  bool GetOperandSelect(const Operand &Op, unsigned Slot, bool IntNeg,
                        SelectConst &Out) const {
    if (Op.Kind != OperandKind::Temp || Op.RelativeRegister != UINT32_MAX ||
        Op.Index >= Selects.size())
      return false;
    const auto &SC = Selects[Op.Index][GetSwizzleLane(Op.Swizzle, Slot)];
    if (!SC.Valid) return false;
    Out = SC;
    Out.TrueBits = ApplyLaneMod(SC.TrueBits, Op.Modifier, IntNeg);
    Out.FalseBits = ApplyLaneMod(SC.FalseBits, Op.Modifier, IntNeg);
    return true;
  }
};

SignedRange EvalInstLaneRange(const SsaProgram &Ssa, const SsaInst &Inst,
                              unsigned Lane, const LaneConstTable &Tab,
                              const ScopedRangeMap &Rm) {
  return EvalInstRangeFromFn(Inst, Lane, [&](const Operand &Op, unsigned S) {
    return Tab.GetOperandRange(Ssa, Rm, Op, S);
  });
}

bool EvalSelectLane(const SsaProgram &Ssa, const SsaInst &Inst, unsigned Lane,
                    const LaneConstTable &Tab, const ScopedRangeMap &Rm,
                    SelectConst &Out) {
  const uint32_t Op = Inst.Opcode;
  const bool IntNeg = InstructionAcceptsIntNegModifier(Op);
  if (Op == D3D10_SB_OPCODE_MOVC && !Inst.Saturate && Inst.Sources.size() == 3) {
    const Operand &C = Inst.Sources[0];
    if (C.Modifier || C.RelativeRegister != UINT32_MAX ||
        (C.Kind != OperandKind::Temp && C.Kind != OperandKind::Input &&
         C.Kind != OperandKind::ConstantBuffer))
      return false;
    uint32_t TB = 0, FB = 0;
    if (!Tab.GetOperandLane(Ssa, Inst.Sources[1], Lane, IntNeg, TB, &Rm) ||
        !Tab.GetOperandLane(Ssa, Inst.Sources[2], Lane, IntNeg, FB, &Rm))
      return false;
    Out = {C, TB, FB, 0, true};
    Out.Cond.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(C.Swizzle, Lane));
    return true;
  }
  if (Inst.Sources.empty() || Inst.Sources.size() > 3) return false;
  Operand Cond{OperandKind::Null};
  bool HasCond = false;
  uint8_t MaxDepth = 0;
  uint32_t TV[3] = {0, 0, 0}, FV[3] = {0, 0, 0};
  for (size_t S = 0; S < Inst.Sources.size(); ++S) {
    SelectConst SC;
    uint32_t C = 0;
    if (Tab.GetOperandSelect(Inst.Sources[S], Lane, IntNeg, SC)) {
      if (!HasCond) { Cond = SC.Cond; HasCond = true; }
      else if (!SameOperandBase(Cond, SC.Cond) || Cond.Swizzle != SC.Cond.Swizzle)
        return false;
      TV[S] = SC.TrueBits;
      FV[S] = SC.FalseBits;
      MaxDepth = std::max(MaxDepth, SC.Depth);
    } else if (Tab.GetOperandLane(Ssa, Inst.Sources[S], Lane, IntNeg, C, &Rm)) {
      TV[S] = FV[S] = C;
    } else {
      return false;
    }
  }
  if (!HasCond) return false;
  const size_t NS = Inst.Sources.size();
  uint32_t ResT = 0, ResF = 0;
  if (!EvalPureLaneBits(Op, Inst.Saturate, Inst.Precise, NS >= 1, TV[0], NS >= 2,
                        TV[1], NS >= 3, TV[2], ResT) ||
      !EvalPureLaneBits(Op, Inst.Saturate, Inst.Precise, NS >= 1, FV[0], NS >= 2,
                        FV[1], NS >= 3, FV[2], ResF))
    return false;
  Out = {Cond, ResT, ResF,
         static_cast<uint8_t>(std::min<unsigned>(MaxDepth + 1u, 255u)), true};
  return true;
}

bool EvalLane(const SsaProgram &Ssa, const SsaInst &Inst, unsigned Lane,
              const LaneConstTable &Tab, const ScopedRangeMap &Rm,
              uint32_t &Out) {
  const uint32_t Op = Inst.Opcode;
  const bool IntNeg = InstructionAcceptsIntNegModifier(Op);
  uint32_t A = 0, B = 0, C = 0;
  const bool HaveA = Inst.Sources.size() >= 1 &&
                     Tab.GetOperandLane(Ssa, Inst.Sources[0], Lane, IntNeg, A, &Rm);
  const bool HaveB = Inst.Sources.size() >= 2 &&
                     Tab.GetOperandLane(Ssa, Inst.Sources[1], Lane, IntNeg, B, &Rm);
  const bool HaveC = Inst.Sources.size() >= 3 &&
                     Tab.GetOperandLane(Ssa, Inst.Sources[2], Lane, IntNeg, C, &Rm);
  if (EvalPureLaneBits(Op, Inst.Saturate, Inst.Precise, HaveA, A, HaveB, B, HaveC,
                       C, Out))
    return true;
  if ((Op == D3D10_SB_OPCODE_ILT || Op == D3D10_SB_OPCODE_IGE ||
       Op == D3D10_SB_OPCODE_IEQ || Op == D3D10_SB_OPCODE_INE) &&
      Inst.Sources.size() == 2)
    return EvalSignedCmpRange(Op, Tab.GetOperandRange(Ssa, Rm, Inst.Sources[0], Lane),
                              Tab.GetOperandRange(Ssa, Rm, Inst.Sources[1], Lane), Out);
  return false;
}

bool OperandAcceptsScalarImm(uint32_t Opcode, size_t SrcIdx) {
  if (Opcode == PseudoCombineVec || InstructionIsComponentwise(Opcode) ||
      Opcode == D3D10_SB_OPCODE_DP2 || Opcode == D3D10_SB_OPCODE_DP3 ||
      Opcode == D3D10_SB_OPCODE_DP4)
    return true;
  return (Opcode == D3D10_SB_OPCODE_SINCOS || Opcode == D3D10_SB_OPCODE_IMUL ||
          Opcode == D3D10_SB_OPCODE_UMUL || Opcode == D3D10_SB_OPCODE_UDIV) &&
         SrcIdx < 2;
}

bool TryFoldOperandToScalarImm(const SsaProgram &Ssa, const LaneConstTable &Tab,
                               const ScopedRangeMap &Rm, uint32_t Opcode,
                               uint8_t ActiveSlots, Operand &Op) {
  if (!ActiveSlots || Op.Kind != OperandKind::Temp ||
      Op.RelativeRegister != UINT32_MAX)
    return false;
  const bool IntNeg = InstructionAcceptsIntNegModifier(Opcode);
  bool Have = false;
  uint32_t Val = 0;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(ActiveSlots & (1u << L))) continue;
    uint32_t LaneBits = 0;
    if (!Tab.GetOperandLane(Ssa, Op, L, IntNeg, LaneBits, &Rm)) return false;
    if (!Have) { Val = LaneBits; Have = true; }
    else if (LaneBits != Val) return false;
  }
  if (!Have) return false;
  Op = MakeImm(Val);
  return true;
}

bool TryMaterializeSelectConst(SsaInst &Inst, const LaneConstTable &Tab) {
  if (Inst.Dead || Inst.HasAuxDest() || Inst.Opcode == D3D10_SB_OPCODE_MOVC ||
      Inst.Opcode == D3D10_SB_OPCODE_MOV || Inst.Opcode == PseudoCombineVec ||
      Inst.Dest.Kind != OperandKind::Temp ||
      Inst.Dest.RelativeRegister != UINT32_MAX ||
      Inst.Dest.Index >= Tab.Selects.size())
    return false;
  SelectConst Common;
  bool Have = false;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(Inst.Dest.Mask & (1u << L))) continue;
    const auto &SC = Tab.Selects[Inst.Dest.Index][L];
    if (!SC.Valid || SC.Depth < 2) return false;
    if (!Have) { Common = SC; Have = true; }
    else if (!SameOperandBase(Common.Cond, SC.Cond) ||
             Common.Cond.Swizzle != SC.Cond.Swizzle ||
             Common.TrueBits != SC.TrueBits ||
             Common.FalseBits != SC.FalseBits)
      return false;
  }
  if (!Have) return false;
  Inst.Opcode = D3D10_SB_OPCODE_MOVC;
  Inst.Saturate = false;
  Inst.Sources = {Common.Cond, MakeImm(Common.TrueBits),
                  MakeImm(Common.FalseBits)};
  return true;
}

bool EvalPhiLaneConst(const SsaProgram &Ssa, const SsaPhi &Phi, unsigned Lane,
                      const LaneConstTable &Tab, uint32_t &Out) {
  for (size_t K = 0; K < Phi.Inputs.size(); ++K) {
    uint32_t V = 0;
    if (!Tab.GetOperandLane(Ssa, Phi.Inputs[K].Value, Lane, false, V) ||
        (K > 0 && V != Out))
      return false;
    Out = V;
  }
  return !Phi.Inputs.empty();
}

void AnalyzeInst(const SsaProgram &Ssa, const SsaInst &Inst,
                 LaneConstTable &Tab, ScopedRangeMap &Rm) {
  if (Inst.Opcode == PseudoCombineVec) {
    for (unsigned L = 0; L < Inst.Sources.size() && L < kRegisterLanes; ++L) {
      if (!(Inst.Dest.Mask & (1u << L))) continue;
      uint32_t V = 0;
      if (Tab.GetOperandLane(Ssa, Inst.Sources[L], 0, false, V, &Rm))
        Tab.Set(Inst.Dest.Index, L, V);
      else
        Rm.Refine(MakeLaneRangeKey(MakeSsaLane(Inst.Dest.Index, L), 0),
                  Tab.GetOperandRange(Ssa, Rm, Inst.Sources[L], 0));
    }
    return;
  }
  if (!InstructionIsComponentwise(Inst.Opcode)) return;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(Inst.Dest.Mask & (1u << L))) continue;
    uint32_t V = 0;
    if (EvalLane(Ssa, Inst, L, Tab, Rm, V)) {
      Tab.Set(Inst.Dest.Index, L, V);
      continue;
    }
    const SignedRange R = EvalInstLaneRange(Ssa, Inst, L, Tab, Rm);
    if (!R.IsFull())
      Rm.Refine(MakeLaneRangeKey(MakeSsaLane(Inst.Dest.Index, L), 0), R);
    SelectConst SC;
    if (EvalSelectLane(Ssa, Inst, L, Tab, Rm, SC))
      Tab.SetSelect(Inst.Dest.Index, L, SC);
  }
}

void AnalyzeBlock(const SsaProgram &Ssa, const SsaBlock &Blk,
                  LaneConstTable &Tab, ScopedRangeMap &Rm) {
  for (const auto &Phi : Blk.Phis) {
    if (Phi.Dead || Phi.Inputs.empty()) continue;
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      uint32_t V = 0;
      if ((Phi.Mask & (1u << L)) && EvalPhiLaneConst(Ssa, Phi, L, Tab, V))
        Tab.Set(Phi.Result, L, V);
    }
  }
  for (const auto &Inst : Blk.Insts) {
    if (Inst.Dead || Inst.HasAuxDest() || Inst.Dest.Kind != OperandKind::Temp ||
        Inst.Dest.RelativeRegister != UINT32_MAX)
      continue;
    AnalyzeInst(Ssa, Inst, Tab, Rm);
  }
}

bool RewriteBlock(SsaProgram &Ssa, SsaBlock &Blk, const LaneConstTable &Tab,
                  const ScopedRangeMap &Rm, bool FoldBranches) {
  bool Changed = false;
  for (auto &Inst : Blk.Insts) {
    if (Inst.Dead) continue;
    if (TryMaterializeSelectConst(Inst, Tab)) { Changed = true; continue; }
    const uint8_t DMask = Inst.Dest.Kind == OperandKind::Null
                              ? Inst.AuxDest.Mask : Inst.Dest.Mask;
    for (size_t S = 0; S < Inst.Sources.size(); ++S) {
      if (!OperandAcceptsScalarImm(Inst.Opcode, S)) continue;
      const uint8_t Slots = Inst.Opcode == PseudoCombineVec
                                ? 0x1u : InstructionSourceSlotMask(Inst.Opcode, DMask, S);
      Changed |= TryFoldOperandToScalarImm(Ssa, Tab, Rm, Inst.Opcode, Slots,
                                           Inst.Sources[S]);
    }
  }
  uint32_t V = 0;
  if (FoldBranches &&
      (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch) &&
      Blk.Condition.Kind == OperandKind::Temp &&
      Tab.GetOperandLane(Ssa, Blk.Condition, 0, false, V, &Rm)) {
    Blk.Condition = MakeImm(V);
    Changed = true;
  }
  return Changed;
}

bool ExtractBranchRange(const SsaProgram &Ssa,
                        const std::vector<SsaDefSite> &Defs,
                        const LaneConstTable &Tab, const Operand &Cond,
                        bool IsTrue, uint64_t &OutKey, SignedRange &OutRange) {
  if (Cond.Kind != OperandKind::Temp || Cond.Modifier ||
      Cond.RelativeRegister != UINT32_MAX || Cond.Index >= Defs.size())
    return false;
  auto Def = Defs[Cond.Index];
  unsigned Slot = GetSwizzleLane(Cond.Swizzle, 0);
  if (!Def.Valid() || Def.IsPhi || Def.IsAux) return false;
  const SsaInst *Cmp = &Ssa.Blocks[Def.Block].Insts[Def.Index];
  if (!Cmp->Dead && Cmp->Opcode == D3D10_SB_OPCODE_NOT &&
      Cmp->Sources.size() == 1 && Cmp->Sources[0].Kind == OperandKind::Temp &&
      !Cmp->Sources[0].Modifier &&
      Cmp->Sources[0].RelativeRegister == UINT32_MAX &&
      Cmp->Sources[0].Index < Defs.size()) {
    IsTrue = !IsTrue;
    Slot = GetSwizzleLane(Cmp->Sources[0].Swizzle, Slot);
    Def = Defs[Cmp->Sources[0].Index];
    if (!Def.Valid() || Def.IsPhi || Def.IsAux) return false;
    Cmp = &Ssa.Blocks[Def.Block].Insts[Def.Index];
  }
  if (Cmp->Dead || Cmp->Sources.size() != 2) return false;
  uint32_t Op = Cmp->Opcode;
  if (Op != D3D10_SB_OPCODE_ILT && Op != D3D10_SB_OPCODE_IGE &&
      Op != D3D10_SB_OPCODE_IEQ && Op != D3D10_SB_OPCODE_INE)
    return false;
  if (!IsTrue)
    Op = (Op == D3D10_SB_OPCODE_ILT) ? D3D10_SB_OPCODE_IGE
         : (Op == D3D10_SB_OPCODE_IGE) ? D3D10_SB_OPCODE_ILT
         : (Op == D3D10_SB_OPCODE_IEQ) ? D3D10_SB_OPCODE_INE : D3D10_SB_OPCODE_IEQ;
  uint32_t CBits = 0;
  for (unsigned Side = 0; Side < 2; ++Side) {
    if (!Tab.GetOperandLane(Ssa, Cmp->Sources[1 - Side], Slot, true, CBits))
      continue;
    OutKey = MakeLaneRangeKey(Cmp->Sources[Side], Slot);
    return OutKey &&
           RefineSignedEdge(Op, static_cast<int32_t>(CBits), Side == 1, OutRange);
  }
  return false;
}

} // namespace

bool dxcb5::RunOptLaneConstProp(SsaProgram &Ssa, const SsaDomTree &Dom,
                                bool FoldBranches) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Lane Const Prop", llvm::StringRef());
  if (Dom.RpoOrder.empty()) return false;
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<llvm::SmallVector<uint32_t, 4>> ReachPreds;
  BuildReachablePreds(Ssa, Dom, ReachPreds);

  LaneConstTable Tab(Ssa.NextValueId);
  ScopedRangeMap Ranges;
  struct Frame {
    uint32_t Block;
    size_t Mark;
    uint64_t RefineKey;
    SignedRange RefineRange;
    bool Exit;
  };
  llvm::SmallVector<Frame, 64> Stack;
  Stack.push_back({0, 0, 0, SignedRange::Top(), false});

  bool Changed = false;
  while (!Stack.empty()) {
    const Frame Cur = Stack.pop_back_val();
    if (Cur.Exit) {
      Ranges.Rollback(Cur.Mark);
      continue;
    }
    const size_t Mark = Ranges.Mark();
    if (Cur.RefineKey) Ranges.Refine(Cur.RefineKey, Cur.RefineRange);
    AnalyzeBlock(Ssa, Ssa.Blocks[Cur.Block], Tab, Ranges);
    Changed |= RewriteBlock(Ssa, Ssa.Blocks[Cur.Block], Tab, Ranges, FoldBranches);
    Stack.push_back({Cur.Block, Mark, 0, SignedRange::Top(), true});

    const auto &Blk = Ssa.Blocks[Cur.Block];
    const bool HasBranch =
        Blk.Kind == FlowKind::Branch && Blk.Successors.size() == 2 &&
        Blk.Successors[0] != Blk.Successors[1];
    for (uint32_t Child : Dom.Children[Cur.Block]) {
      uint64_t RKey = 0;
      SignedRange RRange = SignedRange::Top();
      if (HasBranch && EdgeDominatesChild(Dom, ReachPreds, Cur.Block, Child)) {
        const bool IsT = (Child == Blk.Successors[0]);
        const bool IsF = (Child == Blk.Successors[1]);
        if (IsT || IsF)
          ExtractBranchRange(Ssa, Defs, Tab, Blk.Condition, IsT, RKey, RRange);
      }
      Stack.push_back({Child, 0, RKey, RRange, false});
    }
  }
  return Changed;
}
