// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_CONTROL_FLOW_CODE_H
#define DXCB5_CONTROL_FLOW_CODE_H

#include "dxcb5/ControlFlow.h"
#include "dxcb5/InstructionInfo.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/ADT/ArrayRef.h"

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
    std::vector<ParallelCopy> Sequence;
    if (!ResolveParallelCopies(Edge.Copies, Scratch, Sequence))
      return false;
    const auto First = Instructions.size();
    for (const ParallelCopy &Move : Sequence) {
      UsesScratch |= Move.Destination == Scratch;
      if (Instructions.size() != First &&
          TryMergeCopy(Instructions.back(), Move))
        continue;
      auto Destination = Temp(Move.Destination);
      Destination.Mask = Move.DestinationMask();
      auto Source = Move.Source;
      PutSourceLane(Source, Move.DestinationLane, Move.SourceLane());
      Emit(D3D10_SB_OPCODE_MOV, {Destination, Source});
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
  static void PutSourceLane(Operand &Source, uint8_t OutputLane,
                            uint8_t InputLane) {
    constexpr unsigned LaneIndexBits = 2;
    constexpr unsigned LaneIndexMask = 3;
    const auto Offset = OutputLane * LaneIndexBits;
    Source.Swizzle = static_cast<uint8_t>(
        (Source.Swizzle & ~(LaneIndexMask << Offset)) | (InputLane << Offset));
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
