// Distributed under the University of Illinois Open Source License.
#include "ControlFlowPeephole.h"
#include "llvm/ADT/SmallVector.h"
#include <iterator>
#include <limits>

using namespace dxcb5;
using namespace dxcb5::flow;

namespace {

constexpr size_t kNoPos = std::numeric_limits<size_t>::max();

struct IfSpan {
  size_t ElsePos = kNoPos;
  size_t EndPos = kNoPos;
};

// Precompute matching ELSE/ENDIF positions for every IF in one O(N) stack pass.
void BuildIfSpanTable(const std::vector<Instruction> &Insts,
                      std::vector<IfSpan> &Spans) {
  Spans.assign(Insts.size(), IfSpan{});
  llvm::SmallVector<size_t, 32> ScopeStack;
  for (size_t I = 0; I < Insts.size(); ++I) {
    const uint32_t Op = Insts[I].Opcode;
    if (IsControlFlowBegin(Op)) {
      ScopeStack.push_back(I);
    } else if (Op == D3D10_SB_OPCODE_ELSE) {
      if (!ScopeStack.empty() &&
          Insts[ScopeStack.back()].Opcode == D3D10_SB_OPCODE_IF)
        Spans[ScopeStack.back()].ElsePos = I;
    } else if (IsControlFlowEnd(Op)) {
      if (!ScopeStack.empty()) {
        const size_t Open = ScopeStack.pop_back_val();
        if (Insts[Open].Opcode == D3D10_SB_OPCODE_IF)
          Spans[Open].EndPos = I;
      }
    }
  }
}

bool CanSinkMovPast(const std::vector<Instruction> &Insts, size_t MovPos,
                    size_t RangeEnd) {
  const auto &Dst = Insts[MovPos].Operands[0];
  const auto &Src = Insts[MovPos].Operands[1];
  const uint8_t SrcRead =
      Src.Kind == OperandKind::Temp
          ? InstructionSourceReadLanes(D3D10_SB_OPCODE_MOV, Dst.Mask, 0,
                                       Src.Swizzle)
          : 0;
  for (size_t K = MovPos + 1; K < RangeEnd; ++K) {
    const auto &KD = Insts[K].Operands[0];
    const auto &KS = Insts[K].Operands[1];
    if (KD.Index == Dst.Index && (KD.Mask & Dst.Mask))
      return false;
    if (Src.Kind == OperandKind::Temp && KD.Index == Src.Index &&
        (KD.Mask & SrcRead))
      return false;
    if (KS.Kind == OperandKind::Temp && KS.Index == Dst.Index) {
      const uint8_t KRead = InstructionSourceReadLanes(
          D3D10_SB_OPCODE_MOV, KD.Mask, 0, KS.Swizzle);
      if (KRead & Dst.Mask)
        return false;
    }
  }
  return true;
}

bool RangeClearForMovFold(const std::vector<Instruction> &Insts,
                          size_t PrevPos, size_t CurPos, const Operand &Dst,
                          const Operand &OldSrc, uint8_t OldRead) {
  for (size_t K = PrevPos + 1; K < CurPos; ++K) {
    const auto &Mid = Insts[K];
    const unsigned ND = InstructionDestinationCount(Mid.Opcode);
    const uint8_t DM = CombinedDestMask(Mid);
    for (unsigned D = 0; D < ND && D < Mid.Operands.size(); ++D) {
      const auto &MD = Mid.Operands[D];
      if (MD.RelativeRegister == Dst.Index &&
          ((1u << MD.RelativeLane) & Dst.Mask))
        return false;
      if (MD.Kind == OperandKind::Temp && MD.Index == Dst.Index &&
          (MD.Mask & Dst.Mask))
        return false;
      if (OldSrc.Kind == OperandKind::Temp && MD.Kind == OperandKind::Temp &&
          MD.Index == OldSrc.Index && (MD.Mask & OldRead))
        return false;
    }
    for (size_t O = ND; O < Mid.Operands.size(); ++O) {
      const auto &MS = Mid.Operands[O];
      if (MS.RelativeRegister == Dst.Index &&
          ((1u << MS.RelativeLane) & Dst.Mask))
        return false;
      if (MS.Kind == OperandKind::Temp && MS.Index == Dst.Index &&
          (InstructionSourceReadLanes(Mid.Opcode, DM, O - ND, MS.Swizzle) &
           Dst.Mask))
        return false;
    }
  }
  return true;
}

void TryFoldPrecedingMovIntoMovc(std::vector<Instruction> &Insts, size_t &I,
                                 size_t CurPos, bool TestNZ, uint8_t CondBit) {
  if (CurPos == 0)
    return;
  auto &MC = Insts[CurPos];
  const auto &Dst = MC.Operands[0];
  const size_t SelfOpIdx = TestNZ ? 3 : 2;
  const auto &OtherOp = MC.Operands[TestNZ ? 2 : 3];
  const auto &Cond = MC.Operands[1];
  if (Dst.Index == Cond.Index && (Dst.Mask & CondBit))
    return;
  if (OtherOp.Kind == OperandKind::Temp && OtherOp.Index == Dst.Index &&
      (InstructionSourceReadLanes(D3D10_SB_OPCODE_MOV, Dst.Mask, 0,
                                  OtherOp.Swizzle) &
       Dst.Mask))
    return;
  for (size_t Back = 1; Back <= 4 && Back <= CurPos; ++Back) {
    const size_t PrevPos = CurPos - Back;
    const auto &Prev = Insts[PrevPos];
    if (IsHoistableMov(Prev) && Prev.Operands[0] == Dst &&
        Prev.Operands[1].Modifier == 0 &&
        Prev.Operands[1].Kind != OperandKind::ImmediateConstantBuffer) {
      const auto &OldSrc = Prev.Operands[1];
      const uint8_t OldRead =
          OldSrc.Kind == OperandKind::Temp
              ? InstructionSourceReadLanes(D3D10_SB_OPCODE_MOV, Dst.Mask, 0,
                                           OldSrc.Swizzle)
              : 0;
      if (RangeClearForMovFold(Insts, PrevPos, CurPos, Dst, OldSrc, OldRead)) {
        MC.Operands[SelfOpIdx] = OldSrc;
        Insts.erase(Insts.begin() + PrevPos);
        --I;
        return;
      }
    }
    if (Prev.Opcode != D3D10_SB_OPCODE_MOV &&
        Prev.Opcode != D3D10_SB_OPCODE_MOVC)
      return;
  }
}

bool TryFlattenIfMovsToMovc(std::vector<Instruction> &Insts, size_t &I,
                            size_t EndPos) {
  if ((EndPos != I + 2 && EndPos != I + 3) || Insts[I].Operands.size() != 1 ||
      Insts[I].Operands[0].Kind != OperandKind::Temp ||
      Insts[I].Operands[0].Modifier ||
      Insts[I].Operands[0].RelativeRegister != UINT32_MAX)
    return false;
  const auto &IfCond = Insts[I].Operands[0];
  const uint8_t CondLane = static_cast<uint8_t>(IfCond.Swizzle & 3u);
  const uint8_t CondBit = static_cast<uint8_t>(1u << CondLane);
  const size_t NumMovs = EndPos - (I + 1);
  for (size_t M = 0; M < NumMovs; ++M) {
    const auto &MI = Insts[I + 1 + M];
    if (!IsHoistableMov(MI) || MI.Operands[1].Modifier != 0 ||
        MI.Operands[1].Kind == OperandKind::ImmediateConstantBuffer)
      return false;
  }
  if (NumMovs == 2 && Insts[I + 1].Operands[0].Index == IfCond.Index &&
      (Insts[I + 1].Operands[0].Mask & CondBit))
    return false;

  Operand CondBroad = IfCond;
  CondBroad.Swizzle = static_cast<uint8_t>(CondLane * 0x55u);
  const bool TestNZ = Insts[I].TestNonZero;
  std::vector<Instruction> Repl;
  Repl.reserve(NumMovs);
  for (size_t M = 0; M < NumMovs; ++M) {
    const auto &MI = Insts[I + 1 + M];
    Operand Dst = MI.Operands[0];
    Operand Src = MI.Operands[1];
    Operand Self = Dst;
    Self.Swizzle = 0xe4u;
    Instruction Movc;
    Movc.Opcode = D3D10_SB_OPCODE_MOVC;
    Movc.Operands = {Dst, CondBroad, TestNZ ? Src : Self, TestNZ ? Self : Src};
    Repl.push_back(std::move(Movc));
  }
  Insts.erase(Insts.begin() + I, Insts.begin() + EndPos + 1);
  Insts.insert(Insts.begin() + I, std::make_move_iterator(Repl.begin()),
               std::make_move_iterator(Repl.end()));
  for (size_t M = 0; M < NumMovs; ++M)
    TryFoldPrecedingMovIntoMovc(Insts, I, I + M, TestNZ, CondBit);
  return true;
}

size_t MatchTailUnit(const std::vector<Instruction> &Insts, size_t Start,
                     size_t End) {
  if (End <= Start)
    return 0;
  const uint32_t LastOp = Insts[End - 1].Opcode;
  if (!IsControlFlowBegin(LastOp) && !IsControlFlowEnd(LastOp) &&
      LastOp != D3D10_SB_OPCODE_ELSE && LastOp != D3D10_SB_OPCODE_CASE &&
      LastOp != D3D10_SB_OPCODE_DEFAULT)
    return 1;
  if (LastOp != D3D10_SB_OPCODE_ENDIF)
    return 0;
  int D = 0;
  for (size_t K = End; K > Start; --K) {
    const uint32_t Op = Insts[K - 1].Opcode;
    if (IsControlFlowEnd(Op))
      ++D;
    else if (IsControlFlowBegin(Op) && --D == 0)
      return End - (K - 1);
  }
  return 0;
}

bool TryTailMergeIfArms(std::vector<Instruction> &Insts, size_t I,
                        size_t ElsePos, size_t EndPos) {
  const size_t UnitA = MatchTailUnit(Insts, I + 1, ElsePos);
  const size_t UnitB = MatchTailUnit(Insts, ElsePos + 1, EndPos);
  if (UnitA == 0 || UnitA != UnitB)
    return false;
  for (size_t K = 0; K < UnitA; ++K)
    if (!SameInstructionExact(Insts[ElsePos - UnitA + K],
                              Insts[EndPos - UnitA + K]))
      return false;
  std::vector<Instruction> Tail(Insts.begin() + (EndPos - UnitA),
                                Insts.begin() + EndPos);
  Insts.erase(Insts.begin() + (EndPos - UnitA), Insts.begin() + EndPos);
  const size_t NewEnd = EndPos - UnitA;
  Insts.erase(Insts.begin() + (ElsePos - UnitA), Insts.begin() + ElsePos);
  const size_t FinalEnd = NewEnd - UnitA;
  Insts.insert(Insts.begin() + FinalEnd + 1,
               std::make_move_iterator(Tail.begin()),
               std::make_move_iterator(Tail.end()));
  return true;
}

bool TrySinkCommonTailMov(std::vector<Instruction> &Insts, size_t I,
                          size_t ElsePos, size_t EndPos) {
  size_t ThenMovStart = ElsePos;
  while (ThenMovStart > I + 1 && IsHoistableMov(Insts[ThenMovStart - 1]))
    --ThenMovStart;
  size_t ElseMovStart = EndPos;
  while (ElseMovStart > ElsePos + 1 && IsHoistableMov(Insts[ElseMovStart - 1]))
    --ElseMovStart;
  for (size_t A = ElsePos; A > ThenMovStart; --A) {
    const size_t PosA = A - 1;
    if (!CanSinkMovPast(Insts, PosA, ElsePos))
      continue;
    for (size_t B = EndPos; B > ElseMovStart; --B) {
      const size_t PosB = B - 1;
      if (SameInstructionExact(Insts[PosA], Insts[PosB]) &&
          CanSinkMovPast(Insts, PosB, EndPos)) {
        Instruction Mov = Insts[PosA];
        Insts.erase(Insts.begin() + PosB);
        Insts.erase(Insts.begin() + PosA);
        Insts.insert(Insts.begin() + (EndPos - 1), std::move(Mov));
        return true;
      }
    }
  }
  return false;
}

bool ThenBodySafeForBoolFlag(const std::vector<Instruction> &Insts,
                             size_t Begin, size_t End, const Operand &IfCond,
                             uint8_t CondBit, const Operand &Dst,
                             bool SameRegLane) {
  for (size_t K = Begin; K < End; ++K) {
    const auto &CI = Insts[K];
    const unsigned ND = InstructionDestinationCount(CI.Opcode);
    for (unsigned D = 0; D < ND && D < CI.Operands.size(); ++D) {
      const auto &W = CI.Operands[D];
      if (W.Kind == OperandKind::Temp && W.Index == IfCond.Index &&
          (W.Mask & CondBit))
        return false;
      if (!SameRegLane && W.RelativeRegister == Dst.Index &&
          ((1u << W.RelativeLane) & Dst.Mask))
        return false;
      if (!SameRegLane && W.Kind == OperandKind::Temp && W.Index == Dst.Index &&
          (W.Mask & Dst.Mask))
        return false;
    }
    if (SameRegLane)
      continue;
    const uint8_t DM = CombinedDestMask(CI);
    for (size_t O = ND; O < CI.Operands.size(); ++O) {
      const auto &S = CI.Operands[O];
      if (S.RelativeRegister == Dst.Index && ((1u << S.RelativeLane) & Dst.Mask))
        return false;
      if (S.Kind == OperandKind::Temp && S.Index == Dst.Index &&
          (InstructionSourceReadLanes(CI.Opcode, DM, O - ND, S.Swizzle) &
           Dst.Mask))
        return false;
    }
  }
  return true;
}

bool TryEliminateRedundantBoolFlag(std::vector<Instruction> &Insts, size_t I,
                                   size_t ElsePos, size_t EndPos) {
  if (ElsePos + 2 != EndPos || I == 0)
    return false;
  const auto &ElseMov = Insts[ElsePos + 1];
  const auto &ThenLast = Insts[ElsePos - 1];
  const auto &IfCond = Insts[I].Operands[0];
  if (IfCond.Kind != OperandKind::Temp || IfCond.Modifier ||
      IfCond.RelativeRegister != UINT32_MAX || !IsHoistableMov(ElseMov) ||
      !IsHoistableMov(ThenLast) || !(ElseMov.Operands[0] == ThenLast.Operands[0]) ||
      ElseMov.Operands[1].Kind != OperandKind::Immediate ||
      ThenLast.Operands[1].Kind != OperandKind::Immediate)
    return false;
  const uint32_t TVal = Insts[I].TestNonZero ? ThenLast.Operands[1].Index
                                             : ElseMov.Operands[1].Index;
  const uint32_t FVal = Insts[I].TestNonZero ? ElseMov.Operands[1].Index
                                             : ThenLast.Operands[1].Index;
  if (TVal != 0xffffffffu || FVal != 0u)
    return false;
  const uint8_t CondLane = static_cast<uint8_t>(IfCond.Swizzle & 3u);
  const uint8_t CondBit = static_cast<uint8_t>(1u << CondLane);
  const auto &Prev = Insts[I - 1];
  if (!InstructionIsCompare(Prev.Opcode) || Prev.Operands.empty() ||
      Prev.Operands[0].Kind != OperandKind::Temp ||
      Prev.Operands[0].Index != IfCond.Index ||
      !(Prev.Operands[0].Mask & CondBit))
    return false;
  const auto Dst = ElseMov.Operands[0];
  if (Dst.Mask == 0 || (Dst.Mask & (Dst.Mask - 1)) != 0)
    return false;
  const bool SameRegLane = (Dst.Index == IfCond.Index && Dst.Mask == CondBit);
  if (!ThenBodySafeForBoolFlag(Insts, I + 1, ElsePos - 1, IfCond, CondBit, Dst,
                               SameRegLane))
    return false;
  const Operand CondCopy = IfCond;
  Insts.erase(Insts.begin() + ElsePos, Insts.begin() + EndPos);
  Insts.erase(Insts.begin() + ElsePos - 1);
  if (!SameRegLane) {
    Operand Src = CondCopy;
    Src.Swizzle = static_cast<uint8_t>(CondLane * 0x55u);
    Insts.insert(Insts.begin() + I,
                 Instruction{D3D10_SB_OPCODE_MOV, {Dst, Src}});
  }
  return true;
}

bool SimplifyIfNoElse(std::vector<Instruction> &Insts, size_t &I,
                      size_t EndPos) {
  if (EndPos == I + 1) {
    Insts.erase(Insts.begin() + I, Insts.begin() + EndPos + 1);
    return true;
  }
  if (EndPos == I + 2 && IsUnconditionalExitOp(Insts[I + 1].Opcode)) {
    const uint32_t ExitOp = Insts[I + 1].Opcode;
    Insts[I].Opcode = ExitOp == D3D10_SB_OPCODE_BREAK
                          ? D3D10_SB_OPCODE_BREAKC
                          : (ExitOp == D3D10_SB_OPCODE_CONTINUE
                                 ? D3D10_SB_OPCODE_CONTINUEC
                                 : D3D10_SB_OPCODE_RETC);
    Insts.erase(Insts.begin() + I + 1, Insts.begin() + EndPos + 1);
    ++I;
    return true;
  }
  if ((EndPos > I + 1 && TryDropRedundantLeadingMov(Insts, I, I + 1)) ||
      TryDropRedundantTrailingMov(Insts, I, EndPos))
    return true;
  return TryFlattenIfMovsToMovc(Insts, I, EndPos);
}

bool SimplifyIfWithElse(std::vector<Instruction> &Insts, size_t &I,
                        size_t ElsePos, size_t EndPos) {
  if (ElsePos + 1 == EndPos) {
    Insts.erase(Insts.begin() + ElsePos);
    return true;
  }
  if (ElsePos == I + 1) {
    Insts[I].TestNonZero = !Insts[I].TestNonZero;
    Insts.erase(Insts.begin() + ElsePos);
    return true;
  }
  if (TryDropRedundantLeadingMov(Insts, I, I + 1) ||
      TryDropRedundantLeadingMov(Insts, I, ElsePos + 1))
    return true;
  if (IsUnconditionalExitOp(Insts[ElsePos - 1].Opcode)) {
    Insts[ElsePos] = Instruction{D3D10_SB_OPCODE_ENDIF, {}};
    Insts.erase(Insts.begin() + EndPos);
    return true;
  }
  if (IsUnconditionalExitOp(Insts[EndPos - 1].Opcode)) {
    std::vector<Instruction> ElseArm(Insts.begin() + ElsePos + 1,
                                     Insts.begin() + EndPos);
    std::vector<Instruction> ThenArm(Insts.begin() + I + 1,
                                     Insts.begin() + ElsePos);
    Insts[I].TestNonZero = !Insts[I].TestNonZero;
    std::vector<Instruction> Rebuilt;
    Rebuilt.reserve(ElseArm.size() + ThenArm.size() + 1);
    Rebuilt.insert(Rebuilt.end(), std::make_move_iterator(ElseArm.begin()),
                   std::make_move_iterator(ElseArm.end()));
    Rebuilt.push_back(Instruction{D3D10_SB_OPCODE_ENDIF, {}});
    Rebuilt.insert(Rebuilt.end(), std::make_move_iterator(ThenArm.begin()),
                   std::make_move_iterator(ThenArm.end()));
    Insts.erase(Insts.begin() + I + 1, Insts.begin() + EndPos + 1);
    Insts.insert(Insts.begin() + I + 1,
                 std::make_move_iterator(Rebuilt.begin()),
                 std::make_move_iterator(Rebuilt.end()));
    return true;
  }
  if (TryTailMergeIfArms(Insts, I, ElsePos, EndPos) ||
      TrySinkCommonTailMov(Insts, I, ElsePos, EndPos) ||
      TryEliminateRedundantBoolFlag(Insts, I, ElsePos, EndPos))
    return true;
  if (IsSafeToHoistArmBeforeIf(Insts[I], Insts, ElsePos + 1, EndPos, I + 1,
                               ElsePos)) {
    const size_t HoistCount = EndPos - (ElsePos + 1);
    std::vector<Instruction> Hoisted(Insts.begin() + ElsePos + 1,
                                     Insts.begin() + EndPos);
    Insts.erase(Insts.begin() + ElsePos, Insts.begin() + EndPos);
    Insts.insert(Insts.begin() + I, std::make_move_iterator(Hoisted.begin()),
                 std::make_move_iterator(Hoisted.end()));
    I += HoistCount;
    return true;
  }
  if (IsSafeToHoistArmBeforeIf(Insts[I], Insts, I + 1, ElsePos, ElsePos + 1,
                               EndPos)) {
    const size_t HoistCount = ElsePos - (I + 1);
    std::vector<Instruction> Hoisted(Insts.begin() + I + 1,
                                     Insts.begin() + ElsePos);
    Insts[I].TestNonZero = !Insts[I].TestNonZero;
    Insts.erase(Insts.begin() + I + 1, Insts.begin() + ElsePos + 1);
    Insts.insert(Insts.begin() + I, std::make_move_iterator(Hoisted.begin()),
                 std::make_move_iterator(Hoisted.end()));
    I += HoistCount;
    return true;
  }
  return false;
}

IfSpan ScanIfSpan(const std::vector<Instruction> &Insts, size_t I) {
  IfSpan Out;
  int Depth = 0;
  for (size_t J = I + 1; J < Insts.size(); ++J) {
    const uint32_t Op = Insts[J].Opcode;
    if (IsControlFlowBegin(Op)) {
      ++Depth;
    } else if (IsControlFlowEnd(Op)) {
      if (Depth == 0) {
        Out.EndPos = J;
        return Out;
      }
      --Depth;
    } else if (Op == D3D10_SB_OPCODE_ELSE && Depth == 0) {
      Out.ElsePos = J;
    }
  }
  return Out;
}

bool CompactTrivialIfs(std::vector<Instruction> &Insts) {
  bool Changed = false;
  size_t W = 0;
  const size_t N = Insts.size();
  for (size_t R = 0; R < N;) {
    if (Insts[R].Opcode == D3D10_SB_OPCODE_IF && R + 1 < N &&
        Insts[R + 1].Opcode == D3D10_SB_OPCODE_ENDIF) {
      R += 2;
      Changed = true;
      continue;
    }
    if (Insts[R].Opcode == D3D10_SB_OPCODE_IF && R + 2 < N &&
        Insts[R + 2].Opcode == D3D10_SB_OPCODE_ENDIF) {
      const uint32_t Next = Insts[R + 1].Opcode;
      if (Next == D3D10_SB_OPCODE_ELSE) {
        R += 3;
        Changed = true;
        continue;
      }
      const bool UncondDiscard =
          Next == D3D10_SB_OPCODE_DISCARD &&
          Insts[R + 1].Operands.size() == 1 &&
          Insts[R + 1].Operands[0].Kind == OperandKind::Immediate &&
          ((Insts[R + 1].TestNonZero && Insts[R + 1].Operands[0].Index != 0u) ||
           (!Insts[R + 1].TestNonZero && Insts[R + 1].Operands[0].Index == 0u));
      if (IsUnconditionalExitOp(Next) || UncondDiscard) {
        Insts[R].Opcode = Next == D3D10_SB_OPCODE_BREAK
                              ? D3D10_SB_OPCODE_BREAKC
                              : (Next == D3D10_SB_OPCODE_CONTINUE
                                     ? D3D10_SB_OPCODE_CONTINUEC
                                     : (Next == D3D10_SB_OPCODE_DISCARD
                                            ? D3D10_SB_OPCODE_DISCARD
                                            : D3D10_SB_OPCODE_RETC));
        if (W != R)
          Insts[W] = std::move(Insts[R]);
        ++W;
        R += 3;
        Changed = true;
        continue;
      }
    }
    if (W != R)
      Insts[W] = std::move(Insts[R]);
    ++W;
    ++R;
  }
  Insts.resize(W);
  return Changed;
}

} // namespace

void dxcb5::flow::OptimizeStructuredCode(std::vector<Instruction> &Insts) {
  std::vector<IfSpan> Spans;
  for (unsigned Iter = 0; Iter < 32; ++Iter) {
    bool Changed = CompactTrivialIfs(Insts);
    BuildIfSpanTable(Insts, Spans);
    bool LoopChanged = false;
    size_t I = 0;
    while (I < Insts.size()) {
      if (Insts[I].Opcode != D3D10_SB_OPCODE_IF) {
        ++I;
        continue;
      }
      const IfSpan Span = LoopChanged ? ScanIfSpan(Insts, I) : Spans[I];
      if (Span.EndPos == kNoPos) {
        ++I;
        continue;
      }
      const bool Hit = Span.ElsePos == kNoPos
                           ? SimplifyIfNoElse(Insts, I, Span.EndPos)
                           : SimplifyIfWithElse(Insts, I, Span.ElsePos,
                                                Span.EndPos);
      if (Hit)
        LoopChanged = true;
      else
        ++I;
    }
    if (!Changed && !LoopChanged)
      break;
  }
}
