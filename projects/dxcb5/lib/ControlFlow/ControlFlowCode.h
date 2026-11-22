// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_CONTROL_FLOW_CODE_H
#define DXCB5_CONTROL_FLOW_CODE_H

#include "dxcb5/ControlFlow.h"
#include "dxcb5/InstructionInfo.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/ADT/ArrayRef.h"
#include <algorithm>

namespace dxcb5::flow {

constexpr uint32_t MaxTempRegisters = 4096;

inline Operand Temp(uint32_t Register) { return {OperandKind::Temp, Register}; }
inline Operand Immediate(uint32_t Bits) {
  return {OperandKind::Immediate, Bits};
}

struct Code {
  uint32_t Scratch;
  std::vector<Instruction> Instructions;
  bool UsesScratch = false;

  void Emit(uint32_t Opcode, std::initializer_list<Operand> Operands = {}) {
    Instructions.push_back({Opcode, Operands});
  }

  bool Copies(const FlowEdge &Edge) {
    std::vector<ParallelCopy> Pending;
    uint8_t SeenDests[MaxTempRegisters] = {};
    for (const ParallelCopy &Copy : Edge.Copies) {
      if (Copy.DestinationLane >= ParallelCopy::LaneCount ||
          !ParallelCopy::SupportsSource(Copy.Source) ||
          Copy.Destination == Scratch || Copy.Destination >= MaxTempRegisters ||
          (Copy.ReadsTemp() && Copy.Source.Index == Scratch))
        return false;
      const uint8_t Bit = static_cast<uint8_t>(1u << Copy.DestinationLane);
      if (SeenDests[Copy.Destination] & Bit)
        return false;
      SeenDests[Copy.Destination] |= Bit;
      if (Copy.ReadsTemp() && Copy.Destination == Copy.Source.Index &&
          Copy.DestinationLane == Copy.SourceLane())
        continue;
      Pending.push_back(Copy);
    }

    const auto First = Instructions.size();
    while (!Pending.empty()) {
      if (EmitReadyCopy(Pending, First))
        continue;
      if (EmitSelfSwizzle(Pending))
        continue;
      break;
    }
    if (Pending.empty())
      return true;
    std::vector<ParallelCopy> Sequence;
    if (!ResolveParallelCopies(Pending, Scratch, Sequence))
      return false;
    for (const ParallelCopy &Move : Sequence) {
      UsesScratch |= Move.Destination == Scratch;
      EmitOneCopy(Move, First);
    }
    return true;
  }

  void Body(const FlowBlock &Block) {
    Instructions.insert(Instructions.end(), Block.Instructions.begin(),
                        Block.Instructions.end());
  }

  void Label(uint32_t Value) {
    Emit(D3D10_SB_OPCODE_CASE, {Immediate(Value)});
  }

private:
  void EmitOneCopy(const ParallelCopy &Move, size_t First) {
    if (Instructions.size() != First && TryMergeCopy(Instructions.back(), Move))
      return;
    auto Destination = Temp(Move.Destination);
    Destination.Mask = Move.DestinationMask();
    auto Source = Move.Source;
    PutSourceLane(Source, Move.DestinationLane, Move.SourceLane());
    Emit(D3D10_SB_OPCODE_MOV, {Destination, Source});
  }

  static bool ReadsLane(const ParallelCopy &Copy, uint32_t Reg, uint8_t Mask) {
    if (Copy.ReadsTemp() && Copy.Source.Index == Reg &&
        (Mask & static_cast<uint8_t>(1u << Copy.SourceLane())))
      return true;
    return Copy.Source.RelativeRegister == Reg &&
           (Mask & static_cast<uint8_t>(1u << Copy.Source.RelativeLane));
  }

  static bool IsBlockedInPending(const std::vector<ParallelCopy> &Pending,
                                 size_t SkipIdx, uint32_t Reg, uint8_t Mask) {
    for (size_t J = 0; J < Pending.size(); ++J)
      if (J != SkipIdx && ReadsLane(Pending[J], Reg, Mask))
        return true;
    return false;
  }

  bool EmitReadyCopy(std::vector<ParallelCopy> &Pending, size_t First) {
    size_t Best = Pending.size();
    for (size_t I = 0; I < Pending.size(); ++I) {
      const auto &C = Pending[I];
      if (IsBlockedInPending(Pending, I, C.Destination, C.DestinationMask()))
        continue;
      if (Best == Pending.size())
        Best = I;
      if (Instructions.size() != First &&
          Instructions.back().Opcode == D3D10_SB_OPCODE_MOV &&
          Instructions.back().Operands[0].Index == C.Destination &&
          Instructions.back().Operands[1].Kind == C.Source.Kind &&
          Instructions.back().Operands[1].Index == C.Source.Index) {
        Best = I;
        break;
      }
    }
    if (Best == Pending.size())
      return false;
    const ParallelCopy Chosen = Pending[Best];
    Pending.erase(Pending.begin() + Best);
    EmitOneCopy(Chosen, First);
    return true;
  }

  bool TryEmitSelfSwizzleForReg(std::vector<ParallelCopy> &Pending,
                                uint32_t Reg, uint8_t Mod) {
    uint8_t Mask = 0;
    uint8_t Swz = 0;
    unsigned Count = 0;
    for (const auto &C : Pending) {
      if (C.Destination != Reg)
        continue;
      if (!C.ReadsTemp() || C.Source.Index != Reg ||
          C.Source.Modifier != Mod ||
          C.Source.RelativeRegister != UINT32_MAX)
        return false;
      Mask |= C.DestinationMask();
      PutSourceLane(Swz, C.DestinationLane, C.SourceLane());
      ++Count;
    }
    if (Count < 2)
      return false;
    for (const auto &C : Pending)
      if (C.Destination != Reg && ReadsLane(C, Reg, Mask))
        return false;
    uint8_t Fallback = 0;
    for (unsigned L = 0; L < 4; ++L) {
      if (Mask & (1u << L)) {
        Fallback = static_cast<uint8_t>((Swz >> (2u * L)) & 3u);
        break;
      }
    }
    for (unsigned L = 0; L < 4; ++L)
      if (!(Mask & (1u << L)))
        PutSourceLane(Swz, static_cast<uint8_t>(L), Fallback);
    auto Dst = Temp(Reg);
    Dst.Mask = Mask;
    auto Src = Temp(Reg);
    Src.Swizzle = Swz;
    Src.Modifier = Mod;
    Emit(D3D10_SB_OPCODE_MOV, {Dst, Src});
    Pending.erase(
        std::remove_if(Pending.begin(), Pending.end(),
                       [Reg](const ParallelCopy &C) {
                         return C.Destination == Reg;
                       }),
        Pending.end());
    return true;
  }

  bool EmitSelfSwizzle(std::vector<ParallelCopy> &Pending) {
    for (size_t I = 0; I < Pending.size(); ++I) {
      const uint32_t Reg = Pending[I].Destination;
      if (!Pending[I].ReadsTemp() || Pending[I].Source.Index != Reg ||
          Pending[I].Source.RelativeRegister != UINT32_MAX)
        continue;
      if (TryEmitSelfSwizzleForReg(Pending, Reg, Pending[I].Source.Modifier))
        return true;
    }
    return false;
  }

  static void PutSourceLane(uint8_t &Swizzle, uint8_t OutputLane,
                            uint8_t InputLane) {
    constexpr unsigned LaneIndexBits = 2;
    constexpr unsigned LaneIndexMask = 3;
    const auto Offset = OutputLane * LaneIndexBits;
    Swizzle = static_cast<uint8_t>(
        (Swizzle & ~(LaneIndexMask << Offset)) | (InputLane << Offset));
  }

  static void PutSourceLane(Operand &Source, uint8_t OutputLane,
                            uint8_t InputLane) {
    PutSourceLane(Source.Swizzle, OutputLane, InputLane);
  }

  static bool TryMergeCopy(Instruction &Previous, const ParallelCopy &Copy) {
    auto &Destination = Previous.Operands[0];
    auto &Source = Previous.Operands[1];
    if (Destination.Index != Copy.Destination ||
        (Destination.Mask & Copy.DestinationMask()))
      return false;
    auto SameSwizzle = Copy.Source;
    SameSwizzle.Swizzle = Source.Swizzle;
    if (!EqualCopySources(Source, SameSwizzle))
      return false;
    const bool DependsOnPrevious =
        Copy.ReadsTemp() && Copy.Source.Index == Destination.Index &&
        (Destination.Mask & (1u << Copy.SourceLane()));
    if (DependsOnPrevious)
      return false;
    PutSourceLane(Source, Copy.DestinationLane, Copy.SourceLane());
    Destination.Mask |= Copy.DestinationMask();
    return true;
  }
};

struct IterationExit {
  const FlowBlock *Latch = nullptr;
  const FlowEdge *Back = nullptr;
  const FlowEdge *Exit = nullptr;
};

IterationExit EmitBranchedIteration(const std::vector<FlowBlock> &Flow,
                                    llvm::ArrayRef<uint32_t> Blocks,
                                    uint32_t Entry, Code &Out);

inline bool RequiresQuad(D3D10_SB_OPCODE_TYPE Opcode) {
  return InstructionRequiresQuad(static_cast<uint32_t>(Opcode));
}

bool EmitDispatch(const std::vector<FlowBlock> &Blocks, uint32_t State,
                  uint32_t Predicate, Code &Out, bool &UsesDispatchTemps,
                  std::string &Error);

void OptimizeStructuredFlow(std::vector<Instruction> &Insts);

} // namespace dxcb5::flow

#endif
