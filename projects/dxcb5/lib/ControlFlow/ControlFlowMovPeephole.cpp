// Distributed under the University of Illinois Open Source License.
#include "ControlFlowPeephole.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <limits>

using namespace dxcb5;
using namespace dxcb5::flow;

namespace {

bool IsExitOrDiscardOp(uint32_t Op) {
  return Op == D3D10_SB_OPCODE_BREAK || Op == D3D10_SB_OPCODE_BREAKC ||
         Op == D3D10_SB_OPCODE_CONTINUE || Op == D3D10_SB_OPCODE_CONTINUEC ||
         Op == D3D10_SB_OPCODE_RET || Op == D3D10_SB_OPCODE_RETC ||
         Op == D3D10_SB_OPCODE_DISCARD;
}

bool InstReadsRemaining(
    const Instruction &Inst,
    const llvm::SmallDenseMap<uint32_t, uint8_t, 4> &WrittenLanes,
    const llvm::SmallDenseMap<uint32_t, uint8_t, 4> &DefWritten) {
  const uint32_t Op = Inst.Opcode;
  const unsigned NumDests = InstructionDestinationCount(Op);
  const uint8_t DMask = CombinedDestMask(Inst);
  for (unsigned D = 0; D < NumDests && D < Inst.Operands.size(); ++D) {
    const auto &Dst = Inst.Operands[D];
    if (Dst.RelativeRegister != UINT32_MAX) {
      const uint8_t Remaining = static_cast<uint8_t>(
          WrittenLanes.lookup(Dst.RelativeRegister) &
          ~DefWritten.lookup(Dst.RelativeRegister));
      if (Remaining & static_cast<uint8_t>(1u << Dst.RelativeLane))
        return true;
    }
  }
  for (size_t O = NumDests; O < Inst.Operands.size(); ++O) {
    const auto &Src = Inst.Operands[O];
    if (Src.RelativeRegister != UINT32_MAX) {
      const uint8_t Remaining = static_cast<uint8_t>(
          WrittenLanes.lookup(Src.RelativeRegister) &
          ~DefWritten.lookup(Src.RelativeRegister));
      if (Remaining & static_cast<uint8_t>(1u << Src.RelativeLane))
        return true;
    }
    if (Src.Kind == OperandKind::Temp) {
      const uint8_t Remaining = static_cast<uint8_t>(
          WrittenLanes.lookup(Src.Index) & ~DefWritten.lookup(Src.Index));
      if (Remaining != 0 &&
          (Remaining &
           InstructionSourceReadLanes(Op, DMask, O - NumDests, Src.Swizzle)))
        return true;
    }
  }
  return false;
}

bool SameMovSourceBase(const Operand &A, const Operand &B) {
  if (A.Kind != B.Kind || A.Modifier != B.Modifier ||
      A.RelativeRegister != B.RelativeRegister ||
      A.RelativeLane != B.RelativeLane)
    return false;
  if (A.Kind == OperandKind::Immediate)
    return A.Index == B.Index;
  if (A.Kind == OperandKind::Temp || A.Kind == OperandKind::Input ||
      A.Kind == OperandKind::ConstantBuffer)
    return A.Index == B.Index && A.Element == B.Element;
  return false;
}

bool VerifyAndCollectLoopExitMovs(const std::vector<Instruction> &Insts,
                                  size_t LoopPos, size_t EndLoop,
                                  const Instruction &CandidateMov,
                                  llvm::SmallVectorImpl<size_t> &MatchingMovs) {
  const auto &Dst = CandidateMov.Operands[0];
  int SubLS = 0;
  for (size_t K = LoopPos + 1; K < EndLoop; ++K) {
    const auto &Inst = Insts[K];
    const uint32_t KOp = Inst.Opcode;
    if (KOp == D3D10_SB_OPCODE_LOOP || KOp == D3D10_SB_OPCODE_SWITCH)
      ++SubLS;
    else if (KOp == D3D10_SB_OPCODE_ENDLOOP || KOp == D3D10_SB_OPCODE_ENDSWITCH)
      --SubLS;
    const unsigned NumDests = InstructionDestinationCount(KOp);
    const uint8_t DMask = CombinedDestMask(Inst);
    for (size_t O = NumDests; O < Inst.Operands.size(); ++O) {
      const auto &S = Inst.Operands[O];
      if (S.RelativeRegister == Dst.Index && (Dst.Mask & (1u << S.RelativeLane)))
        return false;
      if (S.Kind == OperandKind::Temp && S.Index == Dst.Index &&
          (InstructionSourceReadLanes(KOp, DMask, O - NumDests, S.Swizzle) &
           Dst.Mask))
        return false;
    }
    for (unsigned D = 0; D < NumDests && D < Inst.Operands.size(); ++D) {
      const auto &W = Inst.Operands[D];
      if (W.RelativeRegister == Dst.Index && (Dst.Mask & (1u << W.RelativeLane)))
        return false;
      if (W.Kind != OperandKind::Temp || W.Index != Dst.Index ||
          !(W.Mask & Dst.Mask))
        continue;
      const bool IsExitMov =
          SubLS == 0 && K + 1 < EndLoop &&
          Insts[K + 1].Opcode == D3D10_SB_OPCODE_BREAK &&
          KOp == D3D10_SB_OPCODE_MOV && !Inst.Saturate && W.Mask == Dst.Mask &&
          W.RelativeRegister == UINT32_MAX;
      if (!IsExitMov)
        return false;
      if (SameInstructionExact(Inst, CandidateMov))
        MatchingMovs.push_back(K);
    }
    if (SubLS == 0 &&
        (KOp == D3D10_SB_OPCODE_BREAK || KOp == D3D10_SB_OPCODE_BREAKC)) {
      if (KOp == D3D10_SB_OPCODE_BREAKC || K == LoopPos + 1 ||
          Insts[K - 1].Opcode != D3D10_SB_OPCODE_MOV ||
          Insts[K - 1].Operands[0].Kind != OperandKind::Temp ||
          Insts[K - 1].Operands[0].Index != Dst.Index ||
          Insts[K - 1].Operands[0].Mask != Dst.Mask)
        return false;
    }
  }
  return !MatchingMovs.empty();
}

bool TryHoistOneLoopExitFlag(std::vector<Instruction> &Insts, size_t &LoopPos,
                             size_t EndLoop) {
  int NestedLoopOrSwitch = 0;
  for (size_t J = LoopPos + 1; J + 3 < EndLoop; ++J) {
    const uint32_t Op = Insts[J].Opcode;
    if (Op == D3D10_SB_OPCODE_LOOP || Op == D3D10_SB_OPCODE_SWITCH) {
      ++NestedLoopOrSwitch;
      continue;
    }
    if (Op == D3D10_SB_OPCODE_ENDLOOP || Op == D3D10_SB_OPCODE_ENDSWITCH) {
      --NestedLoopOrSwitch;
      continue;
    }
    if (NestedLoopOrSwitch != 0 || Op != D3D10_SB_OPCODE_IF)
      continue;
    if (Insts[J + 1].Opcode != D3D10_SB_OPCODE_MOV || Insts[J + 1].Saturate ||
        Insts[J + 1].Operands.size() != 2 ||
        Insts[J + 2].Opcode != D3D10_SB_OPCODE_BREAK ||
        Insts[J + 3].Opcode != D3D10_SB_OPCODE_ENDIF)
      continue;
    const auto &Dst = Insts[J + 1].Operands[0];
    const auto &Src = Insts[J + 1].Operands[1];
    if (Dst.Kind != OperandKind::Temp || Dst.RelativeRegister != UINT32_MAX ||
        Dst.Mask == 0 || Src.Kind != OperandKind::Immediate)
      continue;
    llvm::SmallVector<size_t, 4> MatchingMovs;
    if (!VerifyAndCollectLoopExitMovs(Insts, LoopPos, EndLoop, Insts[J + 1],
                                      MatchingMovs))
      continue;
    const Instruction InitMov = Insts[J + 1];
    for (auto It = MatchingMovs.rbegin(); It != MatchingMovs.rend(); ++It)
      Insts.erase(Insts.begin() + *It);
    Insts.insert(Insts.begin() + LoopPos, InitMov);
    ++LoopPos;
    return true;
  }
  return false;
}

bool WritesOverlapDstOrSrc(const Instruction &CI, const Operand &Dst,
                           const Operand &Src, uint8_t SrcRead) {
  const unsigned ND = InstructionDestinationCount(CI.Opcode);
  for (unsigned D = 0; D < ND && D < CI.Operands.size(); ++D) {
    const auto &W = CI.Operands[D];
    if (W.Kind != OperandKind::Temp)
      continue;
    if (W.Index == Dst.Index && (W.Mask & Dst.Mask))
      return true;
    if (Src.Kind == OperandKind::Temp && W.Index == Src.Index &&
        (W.Mask & SrcRead))
      return true;
  }
  return false;
}

bool SameMovCoveredByPrev(const Instruction &Prev, const Instruction &Inner) {
  const auto &Dst = Inner.Operands[0];
  const auto &Src = Inner.Operands[1];
  if (!IsHoistableMov(Prev) || Prev.Operands[0].Index != Dst.Index ||
      (Prev.Operands[0].Mask & Dst.Mask) != Dst.Mask ||
      !SameMovSourceBase(Prev.Operands[1], Src))
    return false;
  if (Src.Kind == OperandKind::Immediate)
    return true;
  for (unsigned L = 0; L < 4u; ++L)
    if ((Dst.Mask & (1u << L)) &&
        ((Prev.Operands[1].Swizzle >> (2u * L)) & 3u) !=
            ((Src.Swizzle >> (2u * L)) & 3u))
      return false;
  return true;
}

bool AllWrittenOverwritten(
    const llvm::SmallDenseMap<uint32_t, uint8_t, 4> &WrittenLanes,
    const llvm::SmallDenseMap<uint32_t, uint8_t, 4> &DefWritten) {
  for (const auto &KV : WrittenLanes)
    if ((DefWritten.lookup(KV.first) & KV.second) != KV.second)
      return false;
  return true;
}

bool FindMatchingControlSpan(const std::vector<Instruction> &Code, size_t I,
                             size_t End, size_t &ElsePos, size_t &EndPos) {
  ElsePos = std::numeric_limits<size_t>::max();
  EndPos = std::numeric_limits<size_t>::max();
  int Depth = 0;
  for (size_t J = I + 1; J < End; ++J) {
    const uint32_t Op = Code[J].Opcode;
    if (IsControlFlowBegin(Op)) {
      ++Depth;
    } else if (IsControlFlowEnd(Op)) {
      if (Depth == 0) {
        EndPos = J;
        return true;
      }
      --Depth;
    } else if (Op == D3D10_SB_OPCODE_ELSE && Depth == 0 &&
               Code[I].Opcode == D3D10_SB_OPCODE_IF) {
      ElsePos = J;
    }
  }
  return false;
}

bool CheckOverwrittenRange(
    const std::vector<Instruction> &Code, size_t Begin, size_t End,
    const llvm::SmallDenseMap<uint32_t, uint8_t, 4> &WrittenLanes,
    llvm::SmallDenseMap<uint32_t, uint8_t, 4> &DefWritten, unsigned Depth) {
  if (Depth > 16)
    return false;
  for (size_t I = Begin; I < End; ++I) {
    const auto &Inst = Code[I];
    const uint32_t Op = Inst.Opcode;
    if (InstReadsRemaining(Inst, WrittenLanes, DefWritten))
      return false;
    if (IsExitOrDiscardOp(Op) &&
        !AllWrittenOverwritten(WrittenLanes, DefWritten))
      return false;
    if (Op == D3D10_SB_OPCODE_IF) {
      size_t ElsePos = 0, EndPos = 0;
      if (!FindMatchingControlSpan(Code, I, End, ElsePos, EndPos))
        return false;
      auto ThenDef = DefWritten;
      if (ElsePos == std::numeric_limits<size_t>::max()) {
        if (!CheckOverwrittenRange(Code, I + 1, EndPos, WrittenLanes, ThenDef,
                                   Depth + 1))
          return false;
      } else {
        auto ElseDef = DefWritten;
        if (!CheckOverwrittenRange(Code, I + 1, ElsePos, WrittenLanes, ThenDef,
                                   Depth + 1) ||
            !CheckOverwrittenRange(Code, ElsePos + 1, EndPos, WrittenLanes,
                                   ElseDef, Depth + 1))
          return false;
        for (const auto &KV : WrittenLanes)
          DefWritten[KV.first] |=
              static_cast<uint8_t>(ThenDef.lookup(KV.first) &
                                   ElseDef.lookup(KV.first));
      }
      I = EndPos;
      continue;
    }
    if (IsControlFlowBegin(Op)) {
      size_t UnusedElse = 0, EndPos = 0;
      if (!FindMatchingControlSpan(Code, I, End, UnusedElse, EndPos))
        return false;
      for (size_t K = I + 1; K < EndPos; ++K) {
        if (InstReadsRemaining(Code[K], WrittenLanes, DefWritten))
          return false;
        if (IsExitOrDiscardOp(Code[K].Opcode) &&
            !AllWrittenOverwritten(WrittenLanes, DefWritten))
          return false;
      }
      I = EndPos;
      continue;
    }
    const unsigned NumDests = InstructionDestinationCount(Op);
    for (unsigned D = 0; D < NumDests && D < Inst.Operands.size(); ++D) {
      const auto &Dst = Inst.Operands[D];
      if (Dst.Kind == OperandKind::Temp &&
          Dst.RelativeRegister == UINT32_MAX && WrittenLanes.count(Dst.Index))
        DefWritten[Dst.Index] |= Dst.Mask;
    }
  }
  return true;
}

size_t FindMatchingIfBack(const std::vector<Instruction> &Insts,
                          size_t ElsePos) {
  int Depth = 0;
  for (size_t K = ElsePos; K > 0; --K) {
    const uint32_t Op = Insts[K - 1].Opcode;
    if (IsControlFlowEnd(Op)) {
      ++Depth;
    } else if (IsControlFlowBegin(Op)) {
      if (Depth == 0)
        return Op == D3D10_SB_OPCODE_IF ? (K - 1)
                                        : std::numeric_limits<size_t>::max();
      --Depth;
    }
  }
  return std::numeric_limits<size_t>::max();
}

bool HasDominatingCoveredMov(const std::vector<Instruction> &Insts,
                             size_t DomAnchor, const Instruction &Inner,
                             const Operand &Dst, const Operand &Src,
                             uint8_t SrcRead) {
  for (size_t P = DomAnchor, Steps = 0; P > 0 && Steps < 32; ++Steps) {
    const auto &Prev = Insts[P - 1];
    const uint32_t Op = Prev.Opcode;
    if (Op == D3D10_SB_OPCODE_IF) {
      --P;
      continue;
    }
    if (Op == D3D10_SB_OPCODE_ELSE) {
      const size_t MatchIf = FindMatchingIfBack(Insts, P - 1);
      if (MatchIf == std::numeric_limits<size_t>::max())
        return false;
      P = MatchIf;
      continue;
    }
    if (IsControlFlowBegin(Op) || IsControlFlowEnd(Op) ||
        Op == D3D10_SB_OPCODE_CASE || Op == D3D10_SB_OPCODE_DEFAULT ||
        IsExitOrDiscardOp(Op))
      return false;
    if (SameMovCoveredByPrev(Prev, Inner))
      return true;
    if (WritesOverlapDstOrSrc(Prev, Dst, Src, SrcRead))
      return false;
    --P;
  }
  return false;
}

} // namespace

bool dxcb5::flow::TryDropRedundantLeadingMov(std::vector<Instruction> &Insts,
                                             size_t DomAnchor, size_t MovPos) {
  if (DomAnchor == 0 || MovPos >= Insts.size() ||
      !IsHoistableMov(Insts[MovPos]))
    return false;
  const auto &Inner = Insts[MovPos];
  const auto &Dst = Inner.Operands[0];
  const auto &Src = Inner.Operands[1];
  const uint8_t SrcRead =
      Src.Kind == OperandKind::Temp
          ? InstructionSourceReadLanes(D3D10_SB_OPCODE_MOV, Dst.Mask, 0,
                                       Src.Swizzle)
          : 0;
  if (Src.Kind == OperandKind::Temp && Src.Index == Dst.Index &&
      (Dst.Mask & SrcRead) != 0)
    return false;
  if (!HasDominatingCoveredMov(Insts, DomAnchor, Inner, Dst, Src, SrcRead))
    return false;
  Insts.erase(Insts.begin() + MovPos);
  return true;
}

bool dxcb5::flow::TryDropRedundantTrailingMov(std::vector<Instruction> &Insts,
                                              size_t I, size_t EndPos) {
  if (I == 0 || EndPos <= I + 1 || !IsHoistableMov(Insts[EndPos - 1]))
    return false;
  const auto &Inner = Insts[EndPos - 1];
  const auto &Dst = Inner.Operands[0];
  const auto &Src = Inner.Operands[1];
  const uint8_t SrcRead =
      Src.Kind == OperandKind::Temp
          ? InstructionSourceReadLanes(D3D10_SB_OPCODE_MOV, Dst.Mask, 0,
                                       Src.Swizzle)
          : 0;
  if (Src.Kind == OperandKind::Temp && Src.Index == Dst.Index &&
      (Dst.Mask & SrcRead) != 0)
    return false;
  for (size_t K = I + 1; K + 1 < EndPos; ++K)
    if (WritesOverlapDstOrSrc(Insts[K], Dst, Src, SrcRead))
      return false;
  if (!HasDominatingCoveredMov(Insts, I, Inner, Dst, Src, SrcRead))
    return false;
  Insts.erase(Insts.begin() + (EndPos - 1));
  return true;
}

bool dxcb5::flow::IsSafeToHoistArmBeforeIf(const Instruction &IfInst,
                                           const std::vector<Instruction> &Code,
                                           size_t SmallBegin, size_t SmallEnd,
                                           size_t OtherBegin, size_t OtherEnd) {
  const size_t Count = SmallEnd - SmallBegin;
  if (Count == 0 || Count > 3)
    return false;
  llvm::SmallDenseMap<uint32_t, uint8_t, 4> WrittenLanes;
  for (size_t I = SmallBegin; I < SmallEnd; ++I) {
    if (!IsHoistableMov(Code[I]))
      return false;
    const auto &Dst = Code[I].Operands[0];
    WrittenLanes[Dst.Index] |= Dst.Mask;
  }
  const auto &Cond = IfInst.Operands[0];
  if (Cond.RelativeRegister != UINT32_MAX)
    return false;
  if (Cond.Kind == OperandKind::Temp &&
      (WrittenLanes.lookup(Cond.Index) &
       static_cast<uint8_t>(1u << (Cond.Swizzle & 3u))))
    return false;

  llvm::SmallDenseMap<uint32_t, uint8_t, 4> DefWritten;
  if (!CheckOverwrittenRange(Code, OtherBegin, OtherEnd, WrittenLanes,
                             DefWritten, 0))
    return false;
  return AllWrittenOverwritten(WrittenLanes, DefWritten);
}

void dxcb5::flow::HoistLoopExitFlags(std::vector<Instruction> &Insts) {
  size_t I = 0;
  while (I < Insts.size()) {
    if (Insts[I].Opcode != D3D10_SB_OPCODE_LOOP) {
      ++I;
      continue;
    }
    int Depth = 0;
    size_t EndLoop = std::numeric_limits<size_t>::max();
    for (size_t J = I + 1; J < Insts.size(); ++J) {
      if (Insts[J].Opcode == D3D10_SB_OPCODE_LOOP)
        ++Depth;
      else if (Insts[J].Opcode == D3D10_SB_OPCODE_ENDLOOP) {
        if (Depth == 0) {
          EndLoop = J;
          break;
        }
        --Depth;
      }
    }
    if (EndLoop == std::numeric_limits<size_t>::max() ||
        !TryHoistOneLoopExitFlag(Insts, I, EndLoop))
      ++I;
  }
}

void dxcb5::flow::MergeConsecutiveMovs(std::vector<Instruction> &Insts) {
  size_t I = 0;
  while (I + 1 < Insts.size()) {
    auto &A = Insts[I];
    const auto &B = Insts[I + 1];
    if (A.Opcode != D3D10_SB_OPCODE_MOV || B.Opcode != D3D10_SB_OPCODE_MOV ||
        A.Saturate != B.Saturate || A.Operands.size() != 2 ||
        B.Operands.size() != 2) {
      ++I;
      continue;
    }
    const auto &DstA = A.Operands[0];
    const auto &DstB = B.Operands[0];
    const auto &SrcA = A.Operands[1];
    const auto &SrcB = B.Operands[1];
    if ((DstA.Kind != OperandKind::Output && DstA.Kind != OperandKind::Temp) ||
        DstA.Kind != DstB.Kind || DstA.Index != DstB.Index ||
        DstA.RelativeRegister != UINT32_MAX ||
        DstB.RelativeRegister != UINT32_MAX || (DstA.Mask & DstB.Mask) != 0 ||
        !SameMovSourceBase(SrcA, SrcB)) {
      ++I;
      continue;
    }
    if (DstA.Kind == OperandKind::Temp && SrcB.Kind == OperandKind::Temp &&
        SrcB.Index == DstA.Index &&
        (DstA.Mask & InstructionSourceReadLanes(D3D10_SB_OPCODE_MOV, DstB.Mask,
                                                0, SrcB.Swizzle))) {
      ++I;
      continue;
    }
    if (DstA.Kind == OperandKind::Temp && SrcB.RelativeRegister == DstA.Index &&
        (DstA.Mask & (1u << SrcB.RelativeLane))) {
      ++I;
      continue;
    }
    const uint8_t MergedMask = static_cast<uint8_t>(DstA.Mask | DstB.Mask);
    if (SrcA.Kind != OperandKind::Immediate) {
      uint8_t MergedSwz = 0;
      uint8_t Fallback = 0;
      for (unsigned L = 0; L < 4; ++L) {
        if (DstA.Mask & (1u << L)) {
          Fallback = static_cast<uint8_t>((SrcA.Swizzle >> (2 * L)) & 3u);
          break;
        }
      }
      for (unsigned L = 0; L < 4; ++L) {
        uint8_t SL = Fallback;
        if (DstA.Mask & (1u << L))
          SL = static_cast<uint8_t>((SrcA.Swizzle >> (2 * L)) & 3u);
        else if (DstB.Mask & (1u << L))
          SL = static_cast<uint8_t>((SrcB.Swizzle >> (2 * L)) & 3u);
        MergedSwz |= static_cast<uint8_t>(SL << (2 * L));
      }
      A.Operands[1].Swizzle = MergedSwz;
    }
    A.Operands[0].Mask = MergedMask;
    Insts.erase(Insts.begin() + I + 1);
  }
}

void dxcb5::flow::OptimizeStructuredFlow(std::vector<Instruction> &Insts) {
  OptimizeStructuredCode(Insts);
  HoistLoopExitFlags(Insts);
  OptimizeStructuredCode(Insts);
  MergeConsecutiveMovs(Insts);
  RunPostRaCleanup(Insts);
}
