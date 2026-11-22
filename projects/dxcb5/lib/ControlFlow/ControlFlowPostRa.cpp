// Distributed under the University of Illinois Open Source License.
#include "ControlFlowPeephole.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <limits>
#include <vector>

using namespace dxcb5;
using namespace dxcb5::flow;

namespace {

constexpr uint32_t kNoIdx = std::numeric_limits<uint32_t>::max();

bool IsPureEliminableOp(uint32_t Op) {
  return InstructionIsPostRaPureEliminable(Op);
}

bool IsControlOp(uint32_t Op) { return InstructionIsControlOp(Op); }

struct LaneBits {
  std::vector<uint64_t> Words;
  void Init(uint32_t NumRegs) { Words.assign((NumRegs + 15u) / 16u, 0); }
  uint8_t Get(uint32_t Reg) const {
    const size_t W = Reg >> 4;
    if (W >= Words.size())
      return 0;
    return static_cast<uint8_t>((Words[W] >> ((Reg & 15u) * 4u)) & 0xfu);
  }
  void Set(uint32_t Reg, uint8_t Mask) {
    const size_t W = Reg >> 4;
    if (W < Words.size())
      Words[W] |= (uint64_t(Mask & 0xfu) << ((Reg & 15u) * 4u));
  }
  void Clear(uint32_t Reg, uint8_t Mask) {
    const size_t W = Reg >> 4;
    if (W < Words.size())
      Words[W] &= ~(uint64_t(Mask & 0xfu) << ((Reg & 15u) * 4u));
  }
  bool OrFrom(const LaneBits &O) {
    bool Changed = false;
    for (size_t I = 0; I < Words.size(); ++I) {
      const uint64_t Next = Words[I] | O.Words[I];
      if (Next != Words[I]) {
        Words[I] = Next;
        Changed = true;
      }
    }
    return Changed;
  }
  void ClearAll() { std::fill(Words.begin(), Words.end(), uint64_t{0}); }
};

void StepLivenessBackward(const Instruction &Inst, LaneBits &Live) {
  const uint32_t Op = Inst.Opcode;
  const unsigned NumDests = InstructionDestinationCount(Op);
  const uint8_t DMask = CombinedDestMask(Inst);
  for (unsigned D = 0; D < NumDests && D < Inst.Operands.size(); ++D) {
    const auto &Dst = Inst.Operands[D];
    if (Dst.Kind == OperandKind::Temp && Dst.RelativeRegister == UINT32_MAX)
      Live.Clear(Dst.Index, Dst.Mask);
  }
  for (unsigned D = 0; D < NumDests && D < Inst.Operands.size(); ++D) {
    const auto &Dst = Inst.Operands[D];
    if (Dst.RelativeRegister != UINT32_MAX)
      Live.Set(Dst.RelativeRegister, static_cast<uint8_t>(1u << Dst.RelativeLane));
  }
  for (size_t O = NumDests; O < Inst.Operands.size(); ++O) {
    const auto &Src = Inst.Operands[O];
    if (Src.RelativeRegister != UINT32_MAX)
      Live.Set(Src.RelativeRegister, static_cast<uint8_t>(1u << Src.RelativeLane));
    if (Src.Kind == OperandKind::Temp) {
      const uint8_t Lanes =
          InstructionSourceReadLanes(Op, DMask, O - NumDests, Src.Swizzle);
      Live.Set(Src.Index, Lanes);
    }
  }
}

struct Segment {
  uint32_t Begin = 0;
  uint32_t End = 0; // exclusive
  llvm::SmallVector<uint32_t, 2> Succs;
};

struct NestEntry {
  uint32_t Kind = 0;
  uint32_t StartPos = 0;
  uint32_t ElsePos = kNoIdx;
  uint32_t EndPos = kNoIdx;
  bool HasDefault = false;
  llvm::SmallVector<uint32_t, 8> Cases;
};

bool BuildSegments(const std::vector<Instruction> &Insts,
                   std::vector<Segment> &Segs, uint32_t &MaxReg) {
  const uint32_t N = static_cast<uint32_t>(Insts.size());
  MaxReg = 0;
  for (const auto &I : Insts) {
    for (const auto &Op : I.Operands) {
      if (Op.Kind == OperandKind::Temp)
        MaxReg = std::max(MaxReg, Op.Index + 1u);
      if (Op.RelativeRegister != UINT32_MAX)
        MaxReg = std::max(MaxReg, Op.RelativeRegister + 1u);
    }
  }
  if (N == 0 || MaxReg == 0 || MaxReg > MaxTempRegisters)
    return false;

  // Match structured control flow spans.
  std::vector<NestEntry> AllNests;
  std::vector<uint32_t> NestOf(N, kNoIdx);
  std::vector<uint32_t> EnclosingLoop(N, kNoIdx);
  std::vector<uint32_t> EnclosingBreak(N, kNoIdx);
  llvm::SmallVector<uint32_t, 32> Stack;
  llvm::SmallVector<uint32_t, 16> LoopStack;
  llvm::SmallVector<uint32_t, 16> BreakStack;

  for (uint32_t I = 0; I < N; ++I) {
    EnclosingLoop[I] = LoopStack.empty() ? kNoIdx : LoopStack.back();
    EnclosingBreak[I] = BreakStack.empty() ? kNoIdx : BreakStack.back();
    const uint32_t Op = Insts[I].Opcode;
    if (IsControlFlowBegin(Op)) {
      const uint32_t Id = static_cast<uint32_t>(AllNests.size());
      NestEntry Entry;
      Entry.Kind = Op;
      Entry.StartPos = I;
      AllNests.push_back(std::move(Entry));
      NestOf[I] = Id;
      Stack.push_back(Id);
      if (Op == D3D10_SB_OPCODE_LOOP) {
        LoopStack.push_back(Id);
        BreakStack.push_back(Id);
      } else if (Op == D3D10_SB_OPCODE_SWITCH) {
        BreakStack.push_back(Id);
      }
    } else if (Op == D3D10_SB_OPCODE_ELSE) {
      if (Stack.empty() || AllNests[Stack.back()].Kind != D3D10_SB_OPCODE_IF)
        return false;
      AllNests[Stack.back()].ElsePos = I;
      NestOf[I] = Stack.back();
    } else if (Op == D3D10_SB_OPCODE_CASE || Op == D3D10_SB_OPCODE_DEFAULT) {
      if (Stack.empty() ||
          AllNests[Stack.back()].Kind != D3D10_SB_OPCODE_SWITCH)
        return false;
      AllNests[Stack.back()].Cases.push_back(I);
      if (Op == D3D10_SB_OPCODE_DEFAULT)
        AllNests[Stack.back()].HasDefault = true;
      NestOf[I] = Stack.back();
    } else if (IsControlFlowEnd(Op)) {
      if (Stack.empty())
        return false;
      const uint32_t Id = Stack.pop_back_val();
      const uint32_t K = AllNests[Id].Kind;
      if ((K == D3D10_SB_OPCODE_IF && Op != D3D10_SB_OPCODE_ENDIF) ||
          (K == D3D10_SB_OPCODE_LOOP && Op != D3D10_SB_OPCODE_ENDLOOP) ||
          (K == D3D10_SB_OPCODE_SWITCH && Op != D3D10_SB_OPCODE_ENDSWITCH))
        return false;
      AllNests[Id].EndPos = I;
      NestOf[I] = Id;
      if (K == D3D10_SB_OPCODE_LOOP) {
        LoopStack.pop_back();
        BreakStack.pop_back();
      } else if (K == D3D10_SB_OPCODE_SWITCH) {
        BreakStack.pop_back();
      }
    }
  }
  if (!Stack.empty())
    return false;

  std::vector<uint8_t> IsLeader(N, 0);
  IsLeader[0] = 1;
  for (uint32_t I = 0; I < N; ++I) {
    if (IsControlOp(Insts[I].Opcode)) {
      IsLeader[I] = 1;
      if (I + 1 < N)
        IsLeader[I + 1] = 1;
    }
  }
  std::vector<uint32_t> SegOf(N, kNoIdx);
  for (uint32_t I = 0; I < N; ++I) {
    if (IsLeader[I]) {
      if (!Segs.empty())
        Segs.back().End = I;
      Segment NewSeg;
      NewSeg.Begin = I;
      NewSeg.End = N;
      Segs.push_back(std::move(NewSeg));
    }
    SegOf[I] = static_cast<uint32_t>(Segs.size() - 1);
  }

  auto AddEdge = [&](Segment &S, uint32_t TargetInst) {
    if (TargetInst < N)
      S.Succs.push_back(SegOf[TargetInst]);
  };
  for (auto &S : Segs) {
    const uint32_t Last = S.End - 1;
    const uint32_t Op = Insts[Last].Opcode;
    if (Op == D3D10_SB_OPCODE_IF) {
      const auto &E = AllNests[NestOf[Last]];
      AddEdge(S, Last + 1);
      AddEdge(S, E.ElsePos != kNoIdx ? E.ElsePos + 1 : E.EndPos + 1);
    } else if (Op == D3D10_SB_OPCODE_ELSE) {
      AddEdge(S, AllNests[NestOf[Last]].EndPos + 1);
    } else if (Op == D3D10_SB_OPCODE_ENDLOOP) {
      AddEdge(S, AllNests[NestOf[Last]].StartPos + 1);
    } else if (Op == D3D10_SB_OPCODE_SWITCH) {
      const auto &E = AllNests[NestOf[Last]];
      for (uint32_t C : E.Cases)
        AddEdge(S, C);
      if (!E.HasDefault)
        AddEdge(S, E.EndPos + 1);
    } else if (Op == D3D10_SB_OPCODE_BREAK || Op == D3D10_SB_OPCODE_BREAKC) {
      if (EnclosingBreak[Last] == kNoIdx)
        return false;
      AddEdge(S, AllNests[EnclosingBreak[Last]].EndPos + 1);
      if (Op == D3D10_SB_OPCODE_BREAKC)
        AddEdge(S, Last + 1);
    } else if (Op == D3D10_SB_OPCODE_CONTINUE ||
               Op == D3D10_SB_OPCODE_CONTINUEC) {
      if (EnclosingLoop[Last] == kNoIdx)
        return false;
      AddEdge(S, AllNests[EnclosingLoop[Last]].StartPos + 1);
      if (Op == D3D10_SB_OPCODE_CONTINUEC)
        AddEdge(S, Last + 1);
    } else if (Op == D3D10_SB_OPCODE_RET) {
      // No successors.
    } else {
      AddEdge(S, Last + 1);
    }
  }
  return true;
}

bool HasObservableOutputs(const std::vector<Instruction> &Insts) {
  for (const auto &I : Insts) {
    const unsigned ND = InstructionDestinationCount(I.Opcode);
    for (unsigned D = 0; D < ND && D < I.Operands.size(); ++D) {
      const auto K = I.Operands[D].Kind;
      if (K == OperandKind::Output || K == OperandKind::OutputDepth ||
          K == OperandKind::OutputMask || K == OperandKind::Uav)
        return true;
    }
    if (I.Opcode == D3D11_SB_OPCODE_STORE_UAV_TYPED ||
        I.Opcode == D3D11_SB_OPCODE_STORE_RAW ||
        I.Opcode == D3D11_SB_OPCODE_STORE_STRUCTURED)
      return true;
  }
  return false;
}

void SolveLiveness(const std::vector<Instruction> &Insts,
                   const std::vector<Segment> &Segs, uint32_t MaxReg,
                   bool TempsLiveAtRet, std::vector<LaneBits> &LiveIn,
                   std::vector<LaneBits> &LiveOut) {
  const size_t NumSegs = Segs.size();
  LiveIn.resize(NumSegs);
  LiveOut.resize(NumSegs);
  for (size_t S = 0; S < NumSegs; ++S) {
    LiveIn[S].Init(MaxReg);
    LiveOut[S].Init(MaxReg);
  }
  LaneBits AllLive;
  AllLive.Init(MaxReg);
  if (TempsLiveAtRet)
    for (uint32_t R = 0; R < MaxReg; ++R)
      AllLive.Set(R, 0xfu);
  LaneBits Cur;
  Cur.Init(MaxReg);
  for (unsigned Iter = 0; Iter < 64; ++Iter) {
    bool Changed = false;
    for (size_t S = NumSegs; S-- > 0;) {
      Cur.ClearAll();
      for (uint32_t Succ : Segs[S].Succs)
        Cur.OrFrom(LiveIn[Succ]);
      const uint32_t LastOp = Insts[Segs[S].End - 1].Opcode;
      if (TempsLiveAtRet &&
          (Segs[S].Succs.empty() || LastOp == D3D10_SB_OPCODE_RETC))
        Cur.OrFrom(AllLive);
      LiveOut[S].Words = Cur.Words;
      for (uint32_t I = Segs[S].End; I-- > Segs[S].Begin;)
        StepLivenessBackward(Insts[I], Cur);
      if (Cur.Words != LiveIn[S].Words) {
        LiveIn[S].Words = Cur.Words;
        Changed = true;
      }
    }
    if (!Changed)
      break;
  }
}

uint8_t ComposeMod(uint8_t Inner, uint8_t Outer) {
  if (!Inner)
    return Outer;
  if (!Outer)
    return Inner;
  if (Outer == D3D10_SB_OPERAND_MODIFIER_ABS)
    return D3D10_SB_OPERAND_MODIFIER_ABS;
  if (Outer == D3D10_SB_OPERAND_MODIFIER_ABSNEG)
    return D3D10_SB_OPERAND_MODIFIER_ABSNEG;
  // Outer == NEG
  if (Inner == D3D10_SB_OPERAND_MODIFIER_NEG)
    return 0;
  if (Inner == D3D10_SB_OPERAND_MODIFIER_ABS)
    return D3D10_SB_OPERAND_MODIFIER_ABSNEG;
  return D3D10_SB_OPERAND_MODIFIER_ABS;
}

bool CanForwardIntoOperand(const Instruction &Consumer, size_t OpIdx,
                           const Operand &Src) {
  const uint32_t Op = Consumer.Opcode;
  if (!IsPureEliminableOp(Op))
    return false;
  const unsigned NumDests = InstructionDestinationCount(Op);
  if (OpIdx < NumDests)
    return false;
  const Operand &UseOp = Consumer.Operands[OpIdx];
  if (UseOp.RelativeRegister != UINT32_MAX)
    return false;
  if (Src.Kind == OperandKind::Immediate)
    return !Src.Modifier && !UseOp.Modifier;
  if (Src.Kind != OperandKind::Temp && Src.Kind != OperandKind::Input &&
      Src.Kind != OperandKind::ConstantBuffer)
    return false;
  if ((Src.Modifier || UseOp.Modifier) &&
      !InstructionOperandAcceptsFloatModifiers(Op, OpIdx - NumDests))
    return false;
  return true;
}

struct ForwardSite { uint32_t InstIdx; size_t OpIdx; };

bool CollectConsumerReads(const Instruction &CI, uint32_t J,
                          const Operand &Dst, const Operand &Src,
                          uint8_t RemMask,
                          llvm::SmallVectorImpl<ForwardSite> &Sites) {
  const unsigned ND = InstructionDestinationCount(CI.Opcode);
  const uint8_t DMask = CombinedDestMask(CI);
  for (unsigned D = 0; D < ND && D < CI.Operands.size(); ++D)
    if (CI.Operands[D].RelativeRegister == Dst.Index)
      return false;
  for (size_t O = ND; O < CI.Operands.size(); ++O) {
    const auto &U = CI.Operands[O];
    if (U.RelativeRegister == Dst.Index)
      return false;
    if (U.Kind != OperandKind::Temp || U.Index != Dst.Index)
      continue;
    const uint8_t Read =
        InstructionSourceReadLanes(CI.Opcode, DMask, O - ND, U.Swizzle);
    if ((Read & ~RemMask) != 0 || !CanForwardIntoOperand(CI, O, Src))
      return false;
    Sites.push_back(ForwardSite{J, O});
  }
  return true;
}

bool ForwardSegmentCopies(std::vector<Instruction> &Insts, const Segment &Seg,
                          const LaneBits &SegLiveOut,
                          std::vector<uint8_t> &Dead) {
  if (Seg.End <= Seg.Begin + 1)
    return false;
  const uint32_t Len = Seg.End - Seg.Begin;
  std::vector<LaneBits> LiveAfter(Len);
  LaneBits Cur = SegLiveOut;
  for (uint32_t K = Len; K-- > 0;) {
    LiveAfter[K] = Cur;
    StepLivenessBackward(Insts[Seg.Begin + K], Cur);
  }

  bool Changed = false;
  for (uint32_t K = 0; K + 1 < Len; ++K) {
    const uint32_t I = Seg.Begin + K;
    if (Dead[I] || !IsHoistableMov(Insts[I]))
      continue;
    const Operand Dst = Insts[I].Operands[0];
    const Operand Src = Insts[I].Operands[1];
    if (Src.Kind != OperandKind::Temp && Src.Kind != OperandKind::Input &&
        Src.Kind != OperandKind::ConstantBuffer &&
        Src.Kind != OperandKind::Immediate)
      continue;
    const uint8_t SrcReadLanes =
        Src.Kind == OperandKind::Temp
            ? InstructionSourceReadLanes(D3D10_SB_OPCODE_MOV, Dst.Mask, 0,
                                         Src.Swizzle)
            : 0u;
    if (Src.Kind == OperandKind::Temp && Src.Index == Dst.Index)
      continue;

    uint8_t RemMask = Dst.Mask;
    llvm::SmallVector<ForwardSite, 4> Sites;
    bool Valid = false;

    for (uint32_t M = K + 1; M < Len; ++M) {
      const uint32_t J = Seg.Begin + M;
      if (Dead[J])
        continue;
      const auto &CI = Insts[J];
      if (!CollectConsumerReads(CI, J, Dst, Src, RemMask, Sites))
        break;
      const unsigned ND = InstructionDestinationCount(CI.Opcode);
      uint8_t SrcClob = 0;
      for (unsigned D = 0; D < ND && D < CI.Operands.size(); ++D) {
        const auto &W = CI.Operands[D];
        if (W.Kind != OperandKind::Temp || W.RelativeRegister != UINT32_MAX)
          continue;
        if (W.Index == Dst.Index)
          RemMask &= static_cast<uint8_t>(~W.Mask);
        if (Src.Kind == OperandKind::Temp && W.Index == Src.Index)
          SrcClob |= W.Mask;
      }
      if ((LiveAfter[M].Get(Dst.Index) & RemMask) == 0) {
        Valid = !Sites.empty();
        break;
      }
      if (SrcClob & SrcReadLanes)
        break;
    }

    if (!Valid)
      continue;
    for (const auto &S : Sites) {
      auto &UseOp = Insts[S.InstIdx].Operands[S.OpIdx];
      if (Src.Kind == OperandKind::Immediate) {
        UseOp = Src;
        continue;
      }
      Operand Repl = Src;
      Repl.Mask = UseOp.Mask;
      Repl.Modifier = ComposeMod(Src.Modifier, UseOp.Modifier);
      uint8_t NewSwz = 0;
      for (unsigned L = 0; L < 4; ++L) {
        const unsigned Mid = (UseOp.Swizzle >> (2u * L)) & 3u;
        const unsigned Fin = (Src.Swizzle >> (2u * Mid)) & 3u;
        NewSwz |= static_cast<uint8_t>(Fin << (2u * L));
      }
      Repl.Swizzle = NewSwz;
      UseOp = Repl;
    }
    Dead[I] = 1;
    Changed = true;
  }
  return Changed;
}

bool SweepSegmentDeadWrites(std::vector<Instruction> &Insts, const Segment &Seg,
                            const LaneBits &SegLiveOut,
                            std::vector<uint8_t> &Dead) {
  bool Changed = false;
  LaneBits Live = SegLiveOut;
  for (uint32_t I = Seg.End; I-- > Seg.Begin;) {
    auto &Inst = Insts[I];
    const unsigned ND = InstructionDestinationCount(Inst.Opcode);
    if (ND == 1 && IsPureEliminableOp(Inst.Opcode) && !Inst.Operands.empty() &&
        Inst.Operands[0].Kind == OperandKind::Temp &&
        Inst.Operands[0].RelativeRegister == UINT32_MAX) {
      const uint8_t Needed = static_cast<uint8_t>(
          Live.Get(Inst.Operands[0].Index) & Inst.Operands[0].Mask);
      if (Needed == 0) {
        Dead[I] = 1;
        Changed = true;
        continue;
      }
      if (Inst.Opcode == D3D10_SB_OPCODE_MOV &&
          Needed != Inst.Operands[0].Mask) {
        Inst.Operands[0].Mask = Needed;
        Changed = true;
      }
    }
    StepLivenessBackward(Inst, Live);
  }
  return Changed;
}

bool RunOnePostRaPass(std::vector<Instruction> &Insts) {
  std::vector<Segment> Segs;
  uint32_t MaxReg = 0;
  if (!BuildSegments(Insts, Segs, MaxReg))
    return false;
  const bool TempsLiveAtRet = !HasObservableOutputs(Insts);
  std::vector<LaneBits> LiveIn, LiveOut;
  SolveLiveness(Insts, Segs, MaxReg, TempsLiveAtRet, LiveIn, LiveOut);

  std::vector<uint8_t> Dead(Insts.size(), 0);
  bool Changed = false;
  for (size_t S = 0; S < Segs.size(); ++S)
    Changed |= SweepSegmentDeadWrites(Insts, Segs[S], LiveOut[S], Dead);
  if (!Changed) {
    for (size_t S = 0; S < Segs.size(); ++S)
      Changed |= ForwardSegmentCopies(Insts, Segs[S], LiveOut[S], Dead);
  }
  if (!Changed)
    return false;
  std::vector<Instruction> Compacted;
  Compacted.reserve(Insts.size());
  for (size_t I = 0; I < Insts.size(); ++I)
    if (!Dead[I])
      Compacted.push_back(std::move(Insts[I]));
  Insts = std::move(Compacted);
  return true;
}

} // namespace

void dxcb5::flow::RunPostRaCleanup(std::vector<Instruction> &Insts) {
  for (unsigned Iter = 0; Iter < 4; ++Iter)
    if (!RunOnePostRaPass(Insts))
      break;
}
