// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_CONTROL_FLOW_PEEPHOLE_H
#define DXCB5_CONTROL_FLOW_PEEPHOLE_H

#include "ControlFlowCode.h"
#include <vector>

namespace dxcb5::flow {

inline bool SameInstructionExact(const Instruction &A, const Instruction &B) {
  if (A.Opcode != B.Opcode || A.Saturate != B.Saturate ||
      A.TestNonZero != B.TestNonZero ||
      A.TexelOffset[0] != B.TexelOffset[0] ||
      A.TexelOffset[1] != B.TexelOffset[1] ||
      A.TexelOffset[2] != B.TexelOffset[2] ||
      A.Operands.size() != B.Operands.size())
    return false;
  for (size_t I = 0; I < A.Operands.size(); ++I)
    if (!(A.Operands[I] == B.Operands[I]))
      return false;
  return true;
}

inline bool IsUnconditionalExitOp(uint32_t Opcode) {
  return Opcode == D3D10_SB_OPCODE_BREAK ||
         Opcode == D3D10_SB_OPCODE_CONTINUE || Opcode == D3D10_SB_OPCODE_RET;
}

inline bool IsControlFlowBegin(uint32_t Opcode) {
  return Opcode == D3D10_SB_OPCODE_IF || Opcode == D3D10_SB_OPCODE_LOOP ||
         Opcode == D3D10_SB_OPCODE_SWITCH;
}

inline bool IsControlFlowEnd(uint32_t Opcode) {
  return Opcode == D3D10_SB_OPCODE_ENDIF || Opcode == D3D10_SB_OPCODE_ENDLOOP ||
         Opcode == D3D10_SB_OPCODE_ENDSWITCH;
}

inline uint8_t CombinedDestMask(const Instruction &Inst) {
  const size_t Dests = std::min<size_t>(
      InstructionDestinationCount(Inst.Opcode), Inst.Operands.size());
  uint8_t Mask = 0;
  for (size_t I = 0; I < Dests; ++I)
    if (Inst.Operands[I].Kind != OperandKind::Null)
      Mask |= static_cast<uint8_t>(Inst.Operands[I].Mask & 0xfu);
  return Mask ? Mask : 0x1u;
}

inline bool IsHoistableMov(const Instruction &Inst) {
  if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate ||
      Inst.Operands.size() != 2)
    return false;
  const auto &Dst = Inst.Operands[0];
  const auto &Src = Inst.Operands[1];
  return Dst.Kind == OperandKind::Temp &&
         Dst.RelativeRegister == UINT32_MAX && Dst.Mask != 0 &&
         Src.RelativeRegister == UINT32_MAX;
}

bool IsSafeToHoistArmBeforeIf(const Instruction &IfInst,
                              const std::vector<Instruction> &Code,
                              size_t SmallBegin, size_t SmallEnd,
                              size_t OtherBegin, size_t OtherEnd);
bool TryDropRedundantLeadingMov(std::vector<Instruction> &Insts,
                                size_t DomAnchor, size_t MovPos);
bool TryDropRedundantTrailingMov(std::vector<Instruction> &Insts, size_t I,
                                 size_t EndPos);
void OptimizeStructuredCode(std::vector<Instruction> &Insts);
void HoistLoopExitFlags(std::vector<Instruction> &Insts);
void MergeConsecutiveMovs(std::vector<Instruction> &Insts);
void RunPostRaCleanup(std::vector<Instruction> &Insts);

} // namespace dxcb5::flow

#endif
