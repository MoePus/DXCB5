// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_INSTRUCTION_INFO_H
#define DXCB5_INSTRUCTION_INFO_H

#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/Support/MathExtras.h"
#include <cstdint>

namespace dxcb5 {

inline bool InstructionIsCompare(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_EQ:  case D3D10_SB_OPCODE_NE:
  case D3D10_SB_OPCODE_LT:  case D3D10_SB_OPCODE_GE:
  case D3D10_SB_OPCODE_IEQ: case D3D10_SB_OPCODE_INE:
  case D3D10_SB_OPCODE_ILT: case D3D10_SB_OPCODE_IGE:
  case D3D10_SB_OPCODE_ULT: case D3D10_SB_OPCODE_UGE:
    return true;
  default:
    return false;
  }
}

inline bool InstructionIsCommutativeBinary(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_ADD:  case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_MIN:  case D3D10_SB_OPCODE_MAX:
  case D3D10_SB_OPCODE_EQ:   case D3D10_SB_OPCODE_NE:
  case D3D10_SB_OPCODE_IADD: case D3D10_SB_OPCODE_IMUL:
  case D3D10_SB_OPCODE_IMIN: case D3D10_SB_OPCODE_IMAX:
  case D3D10_SB_OPCODE_UMIN: case D3D10_SB_OPCODE_UMAX:
  case D3D10_SB_OPCODE_IEQ:  case D3D10_SB_OPCODE_INE:
  case D3D10_SB_OPCODE_AND:  case D3D10_SB_OPCODE_OR:
  case D3D10_SB_OPCODE_XOR:  case D3D10_SB_OPCODE_DP2:
  case D3D10_SB_OPCODE_DP3:  case D3D10_SB_OPCODE_DP4:
    return true;
  default:
    return false;
  }
}

inline bool InstructionIsDerivative(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_DERIV_RTX:
  case D3D10_SB_OPCODE_DERIV_RTY:
  case D3D11_SB_OPCODE_DERIV_RTX_COARSE:
  case D3D11_SB_OPCODE_DERIV_RTY_COARSE:
  case D3D11_SB_OPCODE_DERIV_RTX_FINE:
  case D3D11_SB_OPCODE_DERIV_RTY_FINE:
    return true;
  default:
    return false;
  }
}

inline bool InstructionRequiresQuad(uint32_t Opcode) {
  if (InstructionIsDerivative(Opcode))
    return true;
  switch (Opcode) {
  case D3D10_SB_OPCODE_SAMPLE:
  case D3D10_SB_OPCODE_SAMPLE_B:
  case D3D10_SB_OPCODE_SAMPLE_C:
  case D3D10_1_SB_OPCODE_LOD:
    return true;
  default:
    return false;
  }
}

inline uint8_t Popcount4(uint8_t Mask) {
  return static_cast<uint8_t>(llvm::countPopulation(static_cast<uint32_t>(Mask & 0xfu)));
}

// Operations that apply the same independent computation to each written lane.
inline bool InstructionIsComponentwise(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_ADD: case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_MAD: case D3D10_SB_OPCODE_DIV:
  case D3D10_SB_OPCODE_MIN: case D3D10_SB_OPCODE_MAX:
  case D3D10_SB_OPCODE_MOV: case D3D10_SB_OPCODE_MOVC:
  case D3D10_SB_OPCODE_FRC: case D3D11_SB_OPCODE_RCP:
  case D3D10_SB_OPCODE_EQ: case D3D10_SB_OPCODE_NE:
  case D3D10_SB_OPCODE_LT: case D3D10_SB_OPCODE_GE:
  case D3D10_SB_OPCODE_IADD: case D3D10_SB_OPCODE_IMAD:
  case D3D10_SB_OPCODE_INEG: case D3D10_SB_OPCODE_IMIN:
  case D3D10_SB_OPCODE_IMAX: case D3D10_SB_OPCODE_UMIN:
  case D3D10_SB_OPCODE_UMAX: case D3D10_SB_OPCODE_UMAD:
  case D3D10_SB_OPCODE_ISHL: case D3D10_SB_OPCODE_ISHR:
  case D3D10_SB_OPCODE_USHR:
  case D3D10_SB_OPCODE_IEQ: case D3D10_SB_OPCODE_INE:
  case D3D10_SB_OPCODE_ILT: case D3D10_SB_OPCODE_IGE:
  case D3D10_SB_OPCODE_ULT: case D3D10_SB_OPCODE_UGE:
  case D3D10_SB_OPCODE_AND: case D3D10_SB_OPCODE_OR:
  case D3D10_SB_OPCODE_XOR: case D3D10_SB_OPCODE_NOT:
  case D3D10_SB_OPCODE_FTOI: case D3D10_SB_OPCODE_FTOU:
  case D3D10_SB_OPCODE_ITOF: case D3D10_SB_OPCODE_UTOF:
  case D3D11_SB_OPCODE_F16TOF32: case D3D11_SB_OPCODE_F32TOF16:
  case D3D11_SB_OPCODE_BFREV: case D3D11_SB_OPCODE_COUNTBITS:
  case D3D11_SB_OPCODE_BFI: case D3D11_SB_OPCODE_IBFE:
  case D3D11_SB_OPCODE_UBFE:
  case D3D11_SB_OPCODE_FIRSTBIT_LO: case D3D11_SB_OPCODE_FIRSTBIT_HI:
  case D3D11_SB_OPCODE_FIRSTBIT_SHI:
  case D3D10_SB_OPCODE_EXP: case D3D10_SB_OPCODE_LOG:
  case D3D10_SB_OPCODE_RSQ: case D3D10_SB_OPCODE_SQRT:
  case D3D10_SB_OPCODE_ROUND_NE: case D3D10_SB_OPCODE_ROUND_NI:
  case D3D10_SB_OPCODE_ROUND_PI: case D3D10_SB_OPCODE_ROUND_Z:
    return true;
  default:
    return false;
  }
}

// Floating-point instructions whose source operands support SM5 NEG/ABS modifiers.
inline bool InstructionAcceptsFloatModifiers(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_ADD: case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_MAD: case D3D10_SB_OPCODE_DIV:
  case D3D10_SB_OPCODE_MIN: case D3D10_SB_OPCODE_MAX:
  case D3D10_SB_OPCODE_FRC: case D3D11_SB_OPCODE_RCP:
  case D3D10_SB_OPCODE_EQ: case D3D10_SB_OPCODE_NE:
  case D3D10_SB_OPCODE_GE: case D3D10_SB_OPCODE_LT:
  case D3D10_SB_OPCODE_DP2: case D3D10_SB_OPCODE_DP3: case D3D10_SB_OPCODE_DP4:
  case D3D10_SB_OPCODE_EXP: case D3D10_SB_OPCODE_LOG:
  case D3D10_SB_OPCODE_RSQ: case D3D10_SB_OPCODE_SQRT:
  case D3D10_SB_OPCODE_ROUND_NE: case D3D10_SB_OPCODE_ROUND_NI:
  case D3D10_SB_OPCODE_ROUND_PI: case D3D10_SB_OPCODE_ROUND_Z:
  case D3D10_SB_OPCODE_FTOI: case D3D10_SB_OPCODE_FTOU:
  case D3D11_SB_OPCODE_F32TOF16:
  case D3D10_SB_OPCODE_SINCOS:
  case D3D11_SB_OPCODE_DERIV_RTX_COARSE: case D3D11_SB_OPCODE_DERIV_RTY_COARSE:
  case D3D11_SB_OPCODE_DERIV_RTX_FINE: case D3D11_SB_OPCODE_DERIV_RTY_FINE:
    return true;
  default:
    return false;
  }
}

// Per-operand float modifier legality: MOVC supports NEG/ABS on src1/src2 (not src0).
inline bool InstructionOperandAcceptsFloatModifiers(uint32_t Opcode,
                                                    size_t SrcIndex) {
  if (InstructionAcceptsFloatModifiers(Opcode))
    return true;
  return Opcode == D3D10_SB_OPCODE_MOVC && SrcIndex != 0;
}

// Integer instructions whose source operands support SM5 2's complement NEG.
inline bool InstructionAcceptsIntNegModifier(uint32_t Opcode) {
  return Opcode == D3D10_SB_OPCODE_IADD || Opcode == D3D10_SB_OPCODE_IMAD;
}

// Instructions that support the SM5 _sat destination modifier.
inline bool InstructionSupportsSaturate(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_ADD: case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_MAD: case D3D10_SB_OPCODE_DIV:
  case D3D10_SB_OPCODE_MIN: case D3D10_SB_OPCODE_MAX:
  case D3D10_SB_OPCODE_DP2: case D3D10_SB_OPCODE_DP3: case D3D10_SB_OPCODE_DP4:
  case D3D10_SB_OPCODE_MOV: case D3D10_SB_OPCODE_MOVC:
  case D3D10_SB_OPCODE_FRC: case D3D11_SB_OPCODE_RCP:
  case D3D10_SB_OPCODE_EXP: case D3D10_SB_OPCODE_LOG:
  case D3D10_SB_OPCODE_RSQ: case D3D10_SB_OPCODE_SQRT:
  case D3D10_SB_OPCODE_ROUND_NE: case D3D10_SB_OPCODE_ROUND_NI:
  case D3D10_SB_OPCODE_ROUND_PI: case D3D10_SB_OPCODE_ROUND_Z:
    return true;
  default:
    return false;
  }
}

inline unsigned InstructionDestinationCount(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_BREAK:
  case D3D10_SB_OPCODE_CONTINUE:
  case D3D10_SB_OPCODE_LOOP:
  case D3D10_SB_OPCODE_ENDLOOP:
  case D3D10_SB_OPCODE_ENDSWITCH:
  case D3D10_SB_OPCODE_ELSE:
  case D3D10_SB_OPCODE_ENDIF:
  case D3D10_SB_OPCODE_DEFAULT:
  case D3D10_SB_OPCODE_RET:
  case D3D10_SB_OPCODE_IF:
  case D3D10_SB_OPCODE_BREAKC:
  case D3D10_SB_OPCODE_CONTINUEC:
  case D3D10_SB_OPCODE_DISCARD:
  case D3D10_SB_OPCODE_RETC:
  case D3D10_SB_OPCODE_SWITCH:
  case D3D10_SB_OPCODE_CASE: return 0;
  case D3D10_SB_OPCODE_IMUL:
  case D3D10_SB_OPCODE_UMUL:
  case D3D10_SB_OPCODE_UDIV:
  case D3D10_SB_OPCODE_SINCOS:
  case D3D11_SB_OPCODE_UADDC:
  case D3D11_SB_OPCODE_USUBB:
  case D3D11_SB_OPCODE_IMM_ATOMIC_ALLOC:
  case D3D11_SB_OPCODE_IMM_ATOMIC_CONSUME:
  case D3D11_SB_OPCODE_IMM_ATOMIC_IADD:
  case D3D11_SB_OPCODE_IMM_ATOMIC_AND:
  case D3D11_SB_OPCODE_IMM_ATOMIC_OR:
  case D3D11_SB_OPCODE_IMM_ATOMIC_XOR:
  case D3D11_SB_OPCODE_IMM_ATOMIC_EXCH:
  case D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH:
  case D3D11_SB_OPCODE_IMM_ATOMIC_IMAX:
  case D3D11_SB_OPCODE_IMM_ATOMIC_IMIN:
  case D3D11_SB_OPCODE_IMM_ATOMIC_UMAX:
  case D3D11_SB_OPCODE_IMM_ATOMIC_UMIN: return 2;
  default: return 1;
  }
}

// Operations whose destination lanes can be freely permuted in SM5 by
// permuting their source swizzles correspondingly.
inline bool InstructionIsLaneRemappable(uint32_t Opcode) {
  if (InstructionIsComponentwise(Opcode))
    return true;
  switch (Opcode) {
  case D3D10_SB_OPCODE_SINCOS:
  case D3D10_SB_OPCODE_IMUL:
  case D3D10_SB_OPCODE_UMUL:
  case D3D10_SB_OPCODE_UDIV:
  case D3D11_SB_OPCODE_BFI:
  case D3D11_SB_OPCODE_IBFE:
  case D3D11_SB_OPCODE_UBFE:
  case D3D10_SB_OPCODE_DERIV_RTX:
  case D3D10_SB_OPCODE_DERIV_RTY:
  case D3D11_SB_OPCODE_DERIV_RTX_COARSE:
  case D3D11_SB_OPCODE_DERIV_RTY_COARSE:
  case D3D11_SB_OPCODE_DERIV_RTX_FINE:
  case D3D11_SB_OPCODE_DERIV_RTY_FINE:
    return true;
  default:
    return false;
  }
}

// Returns the 4-bit slot mask read from source operand `SrcIndex` (0-based
// among source operands after destinations) for an SM5 instruction.
inline uint8_t InstructionSourceSlotMaskSm5(uint32_t Opcode, uint8_t DestMask,
                                            size_t SrcIndex) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_DP2: return 0x3u;
  case D3D10_SB_OPCODE_DP3: return 0x7u;
  case D3D10_SB_OPCODE_DP4: return 0xfu;
  case D3D10_SB_OPCODE_SAMPLE:
  case D3D10_SB_OPCODE_LD:
  case D3D10_SB_OPCODE_LD_MS:
  case D3D11_SB_OPCODE_LD_UAV_TYPED:
  case D3D10_1_SB_OPCODE_LOD:
  case D3D10_1_SB_OPCODE_GATHER4:
  case D3D11_SB_OPCODE_GATHER4_C:
  case D3D11_SB_OPCODE_ATOMIC_AND:
  case D3D11_SB_OPCODE_ATOMIC_OR:
  case D3D11_SB_OPCODE_ATOMIC_XOR:
  case D3D11_SB_OPCODE_ATOMIC_CMP_STORE:
  case D3D11_SB_OPCODE_ATOMIC_IADD:
  case D3D11_SB_OPCODE_ATOMIC_IMAX:
  case D3D11_SB_OPCODE_ATOMIC_IMIN:
  case D3D11_SB_OPCODE_ATOMIC_UMAX:
  case D3D11_SB_OPCODE_ATOMIC_UMIN:
  case D3D11_SB_OPCODE_IMM_ATOMIC_IADD:
  case D3D11_SB_OPCODE_IMM_ATOMIC_AND:
  case D3D11_SB_OPCODE_IMM_ATOMIC_OR:
  case D3D11_SB_OPCODE_IMM_ATOMIC_XOR:
  case D3D11_SB_OPCODE_IMM_ATOMIC_EXCH:
  case D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH:
  case D3D11_SB_OPCODE_IMM_ATOMIC_IMAX:
  case D3D11_SB_OPCODE_IMM_ATOMIC_IMIN:
  case D3D11_SB_OPCODE_IMM_ATOMIC_UMAX:
  case D3D11_SB_OPCODE_IMM_ATOMIC_UMIN:
    return SrcIndex == 0 ? 0xfu : 0x1u;
  case D3D11_SB_OPCODE_STORE_UAV_TYPED:
    return SrcIndex == 0 ? 0xfu : DestMask;
  case D3D11_SB_OPCODE_STORE_RAW:
    return SrcIndex == 0 ? 0x1u : DestMask;
  case D3D11_SB_OPCODE_STORE_STRUCTURED:
    return (SrcIndex == 0 || SrcIndex == 1) ? 0x1u : DestMask;
  case D3D11_SB_OPCODE_GATHER4_PO:
  case D3D11_SB_OPCODE_GATHER4_PO_C:
    return (SrcIndex == 0 || SrcIndex == 1) ? 0xfu : 0x1u;
  case D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX:
    return SrcIndex == 0 ? DestMask : 0x1u;
  case D3D11_SB_OPCODE_EVAL_SNAPPED:
    return SrcIndex == 0 ? DestMask : 0x3u;
  case D3D10_SB_OPCODE_SAMPLE_B:
  case D3D10_SB_OPCODE_SAMPLE_L:
  case D3D10_SB_OPCODE_SAMPLE_C:
  case D3D10_SB_OPCODE_SAMPLE_C_LZ:
    return SrcIndex == 0 ? 0xfu : 0x1u;
  case D3D10_SB_OPCODE_SAMPLE_D:
    return (SrcIndex == 0 || SrcIndex == 3 || SrcIndex == 4) ? 0xfu : 0x1u;
  case D3D10_SB_OPCODE_RESINFO:
  case D3D11_SB_OPCODE_BUFINFO:
  case D3D10_1_SB_OPCODE_SAMPLE_INFO:
  case D3D11_SB_OPCODE_LD_RAW:
  case D3D11_SB_OPCODE_LD_STRUCTURED:
  case D3D10_SB_OPCODE_DISCARD:
  case D3D10_SB_OPCODE_IF:
  case D3D10_SB_OPCODE_SWITCH:
  case D3D10_SB_OPCODE_BREAKC:
  case D3D10_SB_OPCODE_CONTINUEC:
  case D3D10_SB_OPCODE_RETC:
    return 0x1u;
  default:
    if (InstructionIsLaneRemappable(Opcode))
      return static_cast<uint8_t>(DestMask & 0xfu);
    return 0xfu;
  }
}

// Returns the 4-bit mask of source register lanes actually read through Swizzle
// for the active slots of source operand `SrcIndex`.
inline uint8_t InstructionSourceReadLanes(uint32_t Opcode, uint8_t DestMask,
                                          size_t SrcIndex, uint8_t Swizzle) {
  const uint8_t Slots = InstructionSourceSlotMaskSm5(Opcode, DestMask, SrcIndex);
  uint8_t Read = 0;
  for (unsigned Slot = 0; Slot < 4; ++Slot) {
    if (Slots & (1u << Slot))
      Read |= static_cast<uint8_t>(1u << ((Swizzle >> (Slot * 2)) & 3u));
  }
  return Read;
}

} // namespace dxcb5
#endif
