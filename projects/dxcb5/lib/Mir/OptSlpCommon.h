// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_OPT_SLP_COMMON_H
#define DXCB5_MIR_OPT_SLP_COMMON_H

#include "Mir/SsaMir.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace dxcb5 {
namespace slp {

struct LaneRemap {
  uint32_t NewId = 0;
  std::array<uint8_t, 4> Map{0, 1, 2, 3};
};

// Reusable O(1) generation-stamped remap lookup table (avoids DenseMap hashing
// on every operand during ApplyLaneRemaps).
class RemapTable {
public:
  void Load(uint32_t NumValues,
            const llvm::DenseMap<uint32_t, LaneRemap> &Remaps) {
    if (Stamp.size() < NumValues) {
      Stamp.resize(NumValues, 0);
      Values.resize(NumValues);
    }
    if (++Gen == 0) {
      std::fill(Stamp.begin(), Stamp.end(), 0u);
      Gen = 1;
    }
    for (const auto &KV : Remaps) {
      if (KV.first >= Stamp.size())
        continue;
      Stamp[KV.first] = Gen;
      Values[KV.first] = KV.second;
    }
  }

  const LaneRemap *Find(uint32_t Id) const {
    return (Id < Stamp.size() && Stamp[Id] == Gen) ? &Values[Id] : nullptr;
  }

private:
  std::vector<uint32_t> Stamp;
  std::vector<LaneRemap> Values;
  uint32_t Gen = 0;
};

inline bool RewriteOperandWithRemap(Operand &Op, const RemapTable &Table) {
  bool Changed = false;
  if (Op.RelativeRegister != UINT32_MAX) {
    if (const LaneRemap *R = Table.Find(Op.RelativeRegister)) {
      Op.RelativeRegister = R->NewId;
      Op.RelativeLane = R->Map[Op.RelativeLane & 3u];
      Changed = true;
    }
  }
  if (Op.Kind != OperandKind::Temp)
    return Changed;
  const LaneRemap *R = Table.Find(Op.Index);
  if (!R)
    return Changed;
  Op.Index = R->NewId;
  uint8_t Swz = 0;
  for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot)
    Swz = SetSwizzleLane(Swz, Slot, R->Map[GetSwizzleLane(Op.Swizzle, Slot)]);
  Op.Swizzle = Swz;
  return true;
}

struct PackedSlot {
  uint32_t InstIdx = 0;
  uint8_t SrcMask = 0;
  std::array<uint8_t, 4> OldToNew{};
};

// O(Block.Insts.size()) reusable per-block first-use table using generation
// stamps so tiny blocks never clear an O(NextValueId) buffer.
class FirstLocalUseCache {
public:
  void Reset(const SsaBlock &Block, uint32_t NumValues) {
    if (Pos.size() < NumValues) {
      Pos.resize(NumValues, 0);
      Stamp.resize(NumValues, 0);
    }
    if (++Gen == 0) {
      std::fill(Stamp.begin(), Stamp.end(), 0u);
      Gen = 1;
    }
    BlockEnd = static_cast<uint32_t>(Block.Insts.size());
    for (uint32_t I = 0; I < BlockEnd; ++I)
      RecordInst(Block.Insts[I], I);
  }

  uint32_t Get(uint32_t ValueId) const {
    return (ValueId < Stamp.size() && Stamp[ValueId] == Gen) ? Pos[ValueId]
                                                             : BlockEnd;
  }

private:
  void Touch(uint32_t ValueId, uint32_t InstIdx) {
    if (ValueId < Stamp.size() && Stamp[ValueId] != Gen) {
      Stamp[ValueId] = Gen;
      Pos[ValueId] = InstIdx;
    }
  }

  void RecordInst(const SsaInst &Inst, uint32_t InstIdx) {
    if (Inst.Dead)
      return;
    if (Inst.Dest.RelativeRegister != UINT32_MAX)
      Touch(Inst.Dest.RelativeRegister, InstIdx);
    for (const auto &Src : Inst.Sources) {
      if (Src.Kind == OperandKind::Temp)
        Touch(Src.Index, InstIdx);
      if (Src.RelativeRegister != UINT32_MAX)
        Touch(Src.RelativeRegister, InstIdx);
    }
  }

  std::vector<uint32_t> Pos;
  std::vector<uint32_t> Stamp;
  uint32_t Gen = 0;
  uint32_t BlockEnd = 0;
};

inline bool IsImulLow(const SsaInst &Inst) {
  return Inst.Opcode == D3D10_SB_OPCODE_IMUL &&
         Inst.Dest.Kind == OperandKind::Null &&
         Inst.AuxDest.Kind == OperandKind::Temp;
}

inline bool IsSlpBarrier(const SsaInst &Inst) {
  if (Inst.Opcode == D3D10_SB_OPCODE_DISCARD)
    return true;
  if (Inst.Dest.Kind == OperandKind::Output ||
      Inst.Dest.Kind == OperandKind::OutputDepth ||
      Inst.Dest.Kind == OperandKind::OutputMask ||
      Inst.Dest.Kind == OperandKind::IndexableTemp ||
      Inst.Dest.Kind == OperandKind::Uav ||
      Inst.AuxDest.Kind == OperandKind::Uav)
    return true;
  for (const auto &Src : Inst.Sources)
    if (Src.Kind == OperandKind::IndexableTemp ||
        Src.Kind == OperandKind::Uav)
      return true;
  return false;
}

inline const Operand &PackDest(const SsaInst &Inst) {
  return IsImulLow(Inst) ? Inst.AuxDest : Inst.Dest;
}

inline Operand &PackDest(SsaInst &Inst) {
  return IsImulLow(Inst) ? Inst.AuxDest : Inst.Dest;
}

inline bool IsVectorizableInst(const SsaInst &Inst) {
  if (IsPseudoOpcode(Inst.Opcode))
    return false;
  if (IsImulLow(Inst))
    return true;
  if (InstructionIsDerivative(Inst.Opcode))
    return Inst.Dest.Kind == OperandKind::Temp &&
           Inst.AuxDest.Kind == OperandKind::Null;
  if (!InstructionIsComponentwise(Inst.Opcode))
    return false;
  if (Inst.Opcode == D3D10_SB_OPCODE_MOV)
    return Inst.Saturate;
  return Inst.Dest.Kind == OperandKind::Temp &&
         Inst.AuxDest.Kind == OperandKind::Null;
}

inline bool SameInstructionShape(const SsaInst &A, const SsaInst &B) {
  return A.Opcode == B.Opcode && A.Saturate == B.Saturate &&
         A.Precise == B.Precise && A.Sources.size() == B.Sources.size() &&
         IsImulLow(A) == IsImulLow(B);
}

void ApplyLaneRemaps(SsaProgram &Ssa,
                     const llvm::DenseMap<uint32_t, LaneRemap> &Remaps,
                     RemapTable &Table,
                     std::vector<uint8_t> *DirtyBlocks = nullptr,
                     llvm::ArrayRef<uint32_t> OnlyBlocks = {});

bool BuildMergedOperand(SsaProgram &Ssa, const SsaBlock &Block,
                        llvm::ArrayRef<PackedSlot> Slots, size_t SrcSlot,
                        Operand &Out,
                        llvm::SmallVectorImpl<SsaInst> *EmitCombines);

bool FindPackAnchor(const SsaBlock &Block, const std::vector<SsaDefSite> &Defs,
                    uint32_t BlockId, llvm::ArrayRef<uint32_t> Indices,
                    const FirstLocalUseCache &FirstLocalUse,
                    uint32_t &OutAnchor,
                    const std::vector<uint32_t> *SlpBarPref = nullptr);

void FindDotRedexMuls(const SsaBlock &Block,
                      const std::vector<SsaDefSite> &Defs, uint32_t BlockId,
                      const std::vector<uint8_t> &Escapes,
                      std::vector<uint8_t> &OutDotMul);

bool GroupScalarPhis(SsaProgram &Ssa, RemapTable &RemapCache);
bool RunTreeSlp(SsaProgram &Ssa, FirstLocalUseCache &UseCache,
                RemapTable &RemapCache);

} // namespace slp
} // namespace dxcb5

#endif
