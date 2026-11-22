// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_INSTRUCTION_INFO_H
#define DXCB5_INSTRUCTION_INFO_H

#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/Support/MathExtras.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace dxcb5 {

// First-class SSA MIR pseudo-opcode (backed by OpcodeSpec.lean and
// GeneratedOpcodeDesc.inc; eliminated during LowerOutOfSsa).
enum SsaPseudoOpcode : uint32_t {
  PseudoCombineVec = 0x10000u,
};

constexpr uint32_t kPseudoOpcodeBase = PseudoCombineVec;
constexpr uint32_t kNumPseudoOpcodes = 1u;

inline bool IsPseudoOpcode(uint32_t Opcode) {
  return Opcode >= kPseudoOpcodeBase &&
         Opcode < kPseudoOpcodeBase + kNumPseudoOpcodes;
}

enum class LaneRemapKind : uint8_t {
  Fixed = 0,
  Componentwise = 1,
  ResourceReturnSwizzle = 2,
  DotScalar = 3,
};

enum class SourceSlotRule : uint8_t {
  DestMask = 0,
  Fixed2,
  Fixed3,
  Fixed4,
  Scalar,
  Src0Vec4,
  Src01Vec4,
  Src034Vec4,
  Src0Vec4RestDest,
  Src0ScalarRestDest,
  Src01ScalarRestDest,
  Src0DestRestScalar,
  Src0DestRest2,
  PerLaneScalar,
};

enum OpcodeFlags : uint16_t {
  kOpFlagCompare = 1u << 0,
  kOpFlagCommutativeBinary = 1u << 1,
  kOpFlagDerivative = 1u << 2,
  kOpFlagRequiresQuad = 1u << 3,
  kOpFlagComponentwise = 1u << 4,
  kOpFlagAcceptsFloatMods = 1u << 5,
  kOpFlagAcceptsIntNeg = 1u << 6,
  kOpFlagSupportsSaturate = 1u << 7,
  kOpFlagCanEliminateIfDead = 1u << 8,
  kOpFlagCanMoveAcrossDivergentBranch = 1u << 9,
  kOpFlagCanMoveAcrossUniformBranch = 1u << 10,
  kOpFlagIsControlOp = 1u << 11,
  kOpFlagIsPostRaPureEliminable = 1u << 12,
};

struct OpcodeDesc {
  uint8_t NumDests;
  uint16_t Flags;
  LaneRemapKind LaneRemap;
  SourceSlotRule SlotRule;
};

namespace detail {

inline constexpr OpcodeDesc kDefaultOpcodeDesc{
    1u, 0u, LaneRemapKind::Fixed, SourceSlotRule::Fixed4};

inline const std::array<OpcodeDesc, D3D10_SB_NUM_OPCODES> &GetOpcodeTable() {
  static const auto Table = []() {
    std::array<OpcodeDesc, D3D10_SB_NUM_OPCODES> T{};
    for (auto &Entry : T)
      Entry = kDefaultOpcodeDesc;
#define DXCB5_SM5_OPCODE(ENUM, DESTS, FLAGS, REMAP, SLOTRULE)                  \
  T[ENUM] = OpcodeDesc{static_cast<uint8_t>(DESTS),                            \
                       static_cast<uint16_t>(FLAGS), LaneRemapKind::REMAP,     \
                       SourceSlotRule::SLOTRULE};
#define DXCB5_PSEUDO_OPCODE(ENUM, DESTS, FLAGS, REMAP, SLOTRULE)
#include "dxcb5/GeneratedOpcodeDesc.inc"
#undef DXCB5_PSEUDO_OPCODE
#undef DXCB5_SM5_OPCODE
    return T;
  }();
  return Table;
}

inline const std::array<OpcodeDesc, kNumPseudoOpcodes> &GetPseudoOpcodeTable() {
  static const auto Table = []() {
    std::array<OpcodeDesc, kNumPseudoOpcodes> T{};
    for (auto &Entry : T)
      Entry = kDefaultOpcodeDesc;
#define DXCB5_SM5_OPCODE(ENUM, DESTS, FLAGS, REMAP, SLOTRULE)
#define DXCB5_PSEUDO_OPCODE(ENUM, DESTS, FLAGS, REMAP, SLOTRULE)               \
  T[(ENUM) - kPseudoOpcodeBase] = OpcodeDesc{                                  \
      static_cast<uint8_t>(DESTS), static_cast<uint16_t>(FLAGS),               \
      LaneRemapKind::REMAP, SourceSlotRule::SLOTRULE};
#include "dxcb5/GeneratedOpcodeDesc.inc"
#undef DXCB5_PSEUDO_OPCODE
#undef DXCB5_SM5_OPCODE
    return T;
  }();
  return Table;
}

} // namespace detail

inline const OpcodeDesc &GetOpcodeDesc(uint32_t Opcode) {
  const auto &Table = detail::GetOpcodeTable();
  if (Opcode < Table.size())
    return Table[Opcode];
  if (IsPseudoOpcode(Opcode))
    return detail::GetPseudoOpcodeTable()[Opcode - kPseudoOpcodeBase];
  return detail::kDefaultOpcodeDesc;
}

inline bool InstructionIsCompare(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagCompare) != 0;
}

inline bool InstructionIsCommutativeBinary(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagCommutativeBinary) != 0;
}

inline bool InstructionIsDerivative(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagDerivative) != 0;
}

inline bool InstructionRequiresQuad(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagRequiresQuad) != 0;
}

inline uint8_t Popcount4(uint8_t Mask) {
  return static_cast<uint8_t>(
      llvm::countPopulation(static_cast<uint32_t>(Mask & 0xfu)));
}

// Operations that apply the same independent computation to each written lane.
inline bool InstructionIsComponentwise(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagComponentwise) != 0;
}

// Per-operand float modifier legality: MOVC supports NEG/ABS on src1/src2
// (not src0); all other float-modifier ops accept them on every source.
inline bool InstructionOperandAcceptsFloatModifiers(uint32_t Opcode,
                                                    size_t SrcIndex) {
  if ((GetOpcodeDesc(Opcode).Flags & kOpFlagAcceptsFloatMods) != 0)
    return true;
  return Opcode == D3D10_SB_OPCODE_MOVC && SrcIndex != 0;
}

// Integer instructions whose source operands support SM5 2's complement NEG.
inline bool InstructionAcceptsIntNegModifier(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagAcceptsIntNeg) != 0;
}

// Instructions that support the SM5 _sat destination modifier.
inline bool InstructionSupportsSaturate(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagSupportsSaturate) != 0;
}

inline unsigned InstructionDestinationCount(uint32_t Opcode) {
  return GetOpcodeDesc(Opcode).NumDests;
}

// Operations whose destination lanes can be freely permuted in SM5 by
// permuting their source swizzles correspondingly.
inline bool InstructionIsLaneRemappable(uint32_t Opcode) {
  return GetOpcodeDesc(Opcode).LaneRemap == LaneRemapKind::Componentwise;
}

// Texture/load ops whose SM5 resource operand swizzle selects per-lane return
// channels into the destination writemask.
inline bool InstructionHasResourceReturnSwizzle(uint32_t Opcode) {
  return GetOpcodeDesc(Opcode).LaneRemap ==
         LaneRemapKind::ResourceReturnSwizzle;
}

// Texture/load ops whose destination writemask can be narrowed to demanded
// lanes and whose destination lanes can be freely remapped by RegAlloc.
inline bool InstructionSupportsNarrowDestMask(uint32_t Opcode) {
  return InstructionHasResourceReturnSwizzle(Opcode);
}

// Orthogonal effect queries backed by Lean proofs in OpcodeSpec.lean.
inline bool InstructionCanEliminateIfDead(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagCanEliminateIfDead) != 0;
}

inline bool InstructionCanMoveAcrossDivergentBranch(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagCanMoveAcrossDivergentBranch) !=
         0;
}

inline bool InstructionCanMoveAcrossUniformBranch(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagCanMoveAcrossUniformBranch) != 0;
}

inline bool InstructionIsControlOp(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagIsControlOp) != 0;
}

inline bool InstructionIsPostRaPureEliminable(uint32_t Opcode) {
  return (GetOpcodeDesc(Opcode).Flags & kOpFlagIsPostRaPureEliminable) != 0;
}

// Returns the 4-bit slot mask read from source operand `SrcIndex` (0-based
// among source operands after destinations) for an SM5 instruction.
inline uint8_t InstructionSourceSlotMaskSm5(uint32_t Opcode, uint8_t DestMask,
                                            size_t SrcIndex) {
  const uint8_t Mask4 = static_cast<uint8_t>(DestMask & 0xfu);
  switch (GetOpcodeDesc(Opcode).SlotRule) {
#define DXCB5_SLOT_RULE_CASE(RULE, EXPR)                                       \
  case SourceSlotRule::RULE:                                                   \
    return static_cast<uint8_t>(EXPR);
#include "dxcb5/GeneratedOpcodeDesc.inc"
#undef DXCB5_SLOT_RULE_CASE
  }
  return 0xfu;
}

inline bool InstructionUsesDemandedDestMask(uint32_t Opcode) {
  return InstructionIsComponentwise(Opcode) ||
         GetOpcodeDesc(Opcode).SlotRule == SourceSlotRule::PerLaneScalar;
}

// Returns the 4-bit mask of source register lanes read through `Swizzle` for
// the given 4-bit active destination/slot mask.
inline uint8_t SwizzledReadLanes(uint8_t Swizzle, uint8_t ActiveSlots) {
  uint8_t Read = 0;
  for (unsigned Slot = 0; Slot < 4; ++Slot) {
    if (ActiveSlots & (1u << Slot))
      Read |= static_cast<uint8_t>(1u << ((Swizzle >> (Slot * 2)) & 3u));
  }
  return Read;
}

// Returns the 4-bit mask of source register lanes actually read through Swizzle
// for the active slots of source operand `SrcIndex`.
inline uint8_t InstructionSourceReadLanes(uint32_t Opcode, uint8_t DestMask,
                                          size_t SrcIndex, uint8_t Swizzle) {
  return SwizzledReadLanes(
      Swizzle, InstructionSourceSlotMaskSm5(Opcode, DestMask, SrcIndex));
}

} // namespace dxcb5
#endif
