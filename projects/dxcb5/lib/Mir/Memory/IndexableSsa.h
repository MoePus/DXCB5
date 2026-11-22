// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_OPT_INDEXABLE_SSA_H
#define DXCB5_MIR_OPT_INDEXABLE_SSA_H

#include "Mir/SsaMir.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <utility>
#include <vector>

namespace dxcb5 {
namespace idxssa {

constexpr uint32_t kMaxSsaTrackedSlots = 2048;
constexpr uint32_t kMaxConstPromoteElems = 32;

enum class PromoteMode : uint8_t { None = 0, ConstLoads, DomSpill, Full };

struct ArrayStats {
  bool BadAccess = false;
  uint32_t ConstStores = 0;
  uint32_t DynStores = 0;
  uint32_t ConstLoads = 0;
  uint32_t DynLoads = 0;
  uint32_t DynLoadLca = UINT32_MAX;
  uint32_t FirstDynLoadIdx = UINT32_MAX;
};

struct SpillPlan {
  uint32_t Block = UINT32_MAX;
  uint32_t InstIdx = UINT32_MAX;
  std::vector<uint8_t> AfterSpill;
};

struct PromotionPlan {
  std::vector<PromoteMode> Modes;
  std::vector<SpillPlan> Spills;
  std::vector<uint32_t> SlotBase;
  std::vector<llvm::SmallVector<std::pair<uint32_t, uint32_t>, 2>> SpillsAtBlock;
  uint32_t TotalSlots = 0;
};

inline bool IsSsaSlotValue(const Operand &Val) {
  return Val.Modifier == 0 && Val.RelativeRegister == UINT32_MAX &&
         (Val.Kind == OperandKind::Temp || Val.Kind == OperandKind::Input ||
          Val.Kind == OperandKind::ConstantBuffer ||
          Val.Kind == OperandKind::Immediate);
}

// Shared store-shape predicate used by both IndexableCopy and IndexablePlan
// so array merging and SSA promotion agree on valid scalar array stores (R3).
inline bool IsSimpleScalarArrayStore(const SsaInst &Inst) {
  return Inst.Opcode == D3D10_SB_OPCODE_MOV && !Inst.Saturate &&
         Inst.Sources.size() == 1 && Inst.Dest.Mask == 1 &&
         IsSsaSlotValue(Inst.Sources[0]);
}

inline Operand NormalizeScalar(Operand Val) {
  if (Val.Kind != OperandKind::Immediate)
    Val.Swizzle = BroadcastLaneSwizzle(Val.Swizzle & 3u);
  return Val;
}

void BuildPromotionPlan(const SsaProgram &Ssa, const SsaDomTree &Dom,
                        PromotionPlan &Plan);

} // namespace idxssa
} // namespace dxcb5

#endif
