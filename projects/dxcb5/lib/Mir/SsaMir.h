// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_SSA_MIR_H
#define DXCB5_MIR_SSA_MIR_H

#include "dxcb5/ControlFlow.h"
#include "dxcb5/InstructionInfo.h"
#include "dxcb5/Mir.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dxcb5 {

constexpr unsigned kRegisterLanes = 4;
constexpr unsigned kLaneBits = 2;
constexpr unsigned kLaneIndexMask = 3;
constexpr uint8_t kBroadcastSwizzle = 0x55;
constexpr uint8_t kIdentitySwizzle = 0xe4;
constexpr uint32_t kFloatSignBit = 0x80000000u;
constexpr uint32_t kFloatAbsMask = 0x7fffffffu;
constexpr uint32_t kFloatPosZeroBits = 0x00000000u;
constexpr uint32_t kFloatNegZeroBits = kFloatSignBit;
constexpr uint32_t kFloatPosOneBits = 0x3f800000u;
constexpr uint32_t kFloatNegOneBits = 0xbf800000u;

// Pseudo-opcodes living only inside SSA MIR (eliminated before or during OutOfSsa).
enum SsaPseudoOpcode : uint32_t {
  // Assembles 2..4 scalar/sub-vector sources into one SSA vector value.
  // Sources[i] supplies lane i (only lanes present in Dest.Mask are populated).
  PseudoCombineVec = 0x10000u,
  // Private local array (1..4 elements) read/write for SM5 alloca lowering.
  PseudoArrayRead = 0x10001u,
  PseudoArrayWrite = 0x10002u,
};

inline bool IsPseudoOpcode(uint32_t Opcode) {
  return Opcode >= PseudoCombineVec;
}

inline unsigned GetSwizzleLane(uint8_t Swizzle, unsigned Slot) {
  return (Swizzle >> (kLaneBits * Slot)) & kLaneIndexMask;
}

inline uint8_t SetSwizzleLane(uint8_t Swizzle, unsigned Slot, unsigned Lane) {
  const unsigned Shift = kLaneBits * Slot;
  return static_cast<uint8_t>((Swizzle & ~(kLaneIndexMask << Shift)) |
                              ((Lane & kLaneIndexMask) << Shift));
}

inline uint8_t BroadcastLaneSwizzle(unsigned Lane) {
  return static_cast<uint8_t>((Lane & kLaneIndexMask) * kBroadcastSwizzle);
}

inline uint8_t ComposeSwizzle(uint8_t Inner, uint8_t Outer) {
  uint8_t Result = 0;
  for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot)
    Result |= static_cast<uint8_t>(
        GetSwizzleLane(Inner, GetSwizzleLane(Outer, Slot)) << (kLaneBits * Slot));
  return Result;
}

inline uint8_t ComposeFloatModifier(uint8_t Inner, uint8_t Outer) {
  if (!Outer)
    return Inner;
  if (Outer == D3D10_SB_OPERAND_MODIFIER_ABS ||
      Outer == D3D10_SB_OPERAND_MODIFIER_ABSNEG)
    return Outer;
  // Outer == NEG
  switch (Inner) {
  case D3D10_SB_OPERAND_MODIFIER_NEG: return 0;
  case D3D10_SB_OPERAND_MODIFIER_ABS: return D3D10_SB_OPERAND_MODIFIER_ABSNEG;
  case D3D10_SB_OPERAND_MODIFIER_ABSNEG: return D3D10_SB_OPERAND_MODIFIER_ABS;
  default: return D3D10_SB_OPERAND_MODIFIER_NEG;
  }
}

inline Operand NegateMirOperand(Operand Op) {
  if (Op.Kind == OperandKind::Immediate) {
    Op.Index ^= kFloatSignBit;
    return Op;
  }
  Op.Modifier = ComposeFloatModifier(Op.Modifier, D3D10_SB_OPERAND_MODIFIER_NEG);
  return Op;
}

inline Operand AbsMirOperand(Operand Op) {
  if (Op.Kind == OperandKind::Immediate) {
    Op.Index &= kFloatAbsMask;
    return Op;
  }
  Op.Modifier = ComposeFloatModifier(Op.Modifier, D3D10_SB_OPERAND_MODIFIER_ABS);
  return Op;
}

inline bool SameOperandBase(const Operand &A, const Operand &B) {
  return A.Kind == B.Kind && A.Index == B.Index && A.Element == B.Element &&
         A.Modifier == B.Modifier && A.RelativeRegister == B.RelativeRegister &&
         A.RelativeLane == B.RelativeLane;
}

inline unsigned PopcountMask(uint8_t Mask) { return Popcount4(Mask); }

inline unsigned FirstActiveLane(uint8_t Mask) {
  return Mask ? static_cast<unsigned>(llvm::countTrailingZeros(Mask)) : 0u;
}

inline uint8_t MaskForWidth(unsigned Width) {
  return static_cast<uint8_t>((1u << Width) - 1u);
}

inline uint8_t ReadLanesForMask(uint8_t Swizzle, uint8_t ActiveSlots) {
  uint8_t Lanes = 0;
  for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot) {
    if (ActiveSlots & (1u << Slot))
      Lanes |= static_cast<uint8_t>(1u << GetSwizzleLane(Swizzle, Slot));
  }
  return Lanes;
}

inline uint8_t DotSourceSlotMask(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_DP2: return 0x3u;
  case D3D10_SB_OPCODE_DP3: return 0x7u;
  case D3D10_SB_OPCODE_DP4: return 0xfu;
  default: return 0;
  }
}

// Returns the slot mask read by a source operand at `SrcIndex` (0-based among Sources).
uint8_t InstructionSourceSlotMask(uint32_t Opcode, uint8_t DestMask,
                                  size_t SrcIndex);

inline Operand MakeSsaTemp(uint32_t ValueId, uint8_t Mask = 1,
                           uint8_t Swizzle = 0) {
  return {OperandKind::Temp, ValueId, 0, Mask, Swizzle};
}

inline Operand MakeSsaLane(uint32_t ValueId, unsigned Lane) {
  return {OperandKind::Temp, ValueId, 0, static_cast<uint8_t>(1u << Lane),
          BroadcastLaneSwizzle(Lane)};
}

inline Operand MakeImm(uint32_t Bits) {
  return {OperandKind::Immediate, Bits};
}

struct SsaPhi {
  struct Incoming {
    uint32_t PredBlock = 0;
    Operand Value{OperandKind::Null};
  };

  uint32_t Result = 0;   // Unique 1-based SSA ValueId
  uint8_t Mask = 1;      // Active lanes defined by this PHI (e.g. 0x1, 0x7, 0xf)
  bool Precise = false;
  bool Dead = false;
  llvm::SmallVector<Incoming, 4> Inputs;
};

struct SsaInst {
  uint32_t Opcode = 0;
  Operand Dest{OperandKind::Null};
  Operand AuxDest{OperandKind::Null}; // Second dest for SINCOS / IMUL / UDIV
  llvm::SmallVector<Operand, 5> Sources;
  bool TestNonZero = true;
  bool Saturate = false;
  bool Precise = false;
  bool Dead = false;
  int8_t TexelOffset[3] = {};
  uint32_t ArrayId = UINT32_MAX; // For PseudoArrayRead / PseudoArrayWrite
  uint8_t ArrayLane = 0;         // Constant lane for PseudoArrayWrite/Read (0xff = dynamic)

  bool HasAuxDest() const {
    return !IsPseudoOpcode(Opcode) && InstructionDestinationCount(Opcode) == 2;
  }
  bool HasPrimaryDest() const {
    return IsPseudoOpcode(Opcode)
               ? Opcode != PseudoArrayWrite
               : InstructionDestinationCount(Opcode) >= 1;
  }
  bool IsPure() const;
  bool IsBarrier() const;
};

struct SsaBlock {
  std::vector<SsaPhi> Phis;
  std::vector<SsaInst> Insts;
  FlowKind Kind = FlowKind::Return;
  Operand Condition{OperandKind::Null};
  // Branch: [true, false]. Switch: [default, case0, ...]. Jump: [target].
  llvm::SmallVector<uint32_t, 4> Successors;
  llvm::SmallVector<uint32_t, 4> CaseValues;
};

struct SsaDefSite {
  uint32_t Block = UINT32_MAX;
  uint32_t Index = UINT32_MAX;
  bool IsPhi = false;
  bool IsAux = false;

  bool Valid() const { return Block != UINT32_MAX; }
};

struct SsaDomTree {
  std::vector<uint32_t> IDom;      // Immediate dominator per block (UINT32_MAX = unreachable)
  std::vector<uint32_t> RpoOrder;  // Reachable blocks in reverse postorder
  std::vector<uint32_t> RpoNumber; // Position in RpoOrder (UINT32_MAX = unreachable)
  std::vector<llvm::SmallVector<uint32_t, 4>> Children;

  bool Reachable(uint32_t Block) const {
    return Block < RpoNumber.size() && RpoNumber[Block] != UINT32_MAX;
  }
  bool Dominates(uint32_t A, uint32_t B) const;
};

class SsaProgram {
public:
  explicit SsaProgram(MirProgram &Mir) : Mir(Mir) {}

  MirProgram &Mir;
  std::vector<SsaBlock> Blocks;
  uint32_t NextValueId = 1; // 1-based SSA value IDs
  std::vector<unsigned> LocalArrayLengths;
  bool NanSafe = false;

  uint32_t AllocValue() { return NextValueId++; }
  uint32_t InternLiteralVector(const std::array<uint32_t, 4> &Bits);

  void BuildDefTable(std::vector<SsaDefSite> &Defs) const;
  void BuildPredecessors(
      std::vector<llvm::SmallVector<uint32_t, 4>> &Preds) const;
  void ComputeDominators(SsaDomTree &Dom) const;

  // Removes Dead phis and Dead instructions from every block.
  void CompactDead();

  // Debug-mode structural & SSA invariant checker.
  bool Verify(std::string &Error) const;

private:
  llvm::DenseMap<uint64_t, llvm::SmallVector<uint32_t, 2>> LiteralBuckets;
};

// Modifier helpers that also fold ImmediateVector literals (SM5 vector literals
// cannot carry NEG/ABS operand modifiers).
Operand NegateMirOperand(SsaProgram &Ssa, Operand Op);
Operand AbsMirOperand(SsaProgram &Ssa, Operand Op);

} // namespace dxcb5

#endif
