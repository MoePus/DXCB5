// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_REGALLOC_LIVENESS_H
#define DXCB5_REGALLOC_LIVENESS_H

#include "dxcb5/ControlFlow.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include <cstdint>
#include <vector>

namespace dxcb5 {

// Stores 4-bit lane masks packed 16 global virtual registers per uint64_t word.
struct BlockLiveness {
  std::vector<uint64_t> Uses;
  std::vector<uint64_t> Defs;
  std::vector<uint64_t> In;
  std::vector<uint64_t> Out;
  llvm::SmallVector<size_t, 2> Predecessors;
  bool Pending = true;
};

struct TempInfo {
  uint8_t WriteMask = 0;
  uint8_t ReadMask = 0;
  uint8_t ActiveMask = 0;
  uint8_t Width = 0;
  bool FixedLanes = false;
  uint32_t DefCount = 0;
};

struct LivenessResult {
  std::vector<BlockLiveness> Blocks;
  std::vector<TempInfo> Temps;
  std::vector<uint32_t> ActiveTemps;
  // Cross-block virtual registers tracked by packed BlockLiveness bitvectors.
  std::vector<uint32_t> GlobalTemps;

  template <typename Fn>
  void ForEachLiveInWord(size_t WordIdx, uint64_t Word, Fn &&Callback) const {
    while (Word) {
      const unsigned Slot =
          static_cast<unsigned>(llvm::countTrailingZeros(Word)) >> 2;
      const unsigned Shift = Slot * 4;
      const uint8_t Mask = static_cast<uint8_t>((Word >> Shift) & 0xfu);
      Word &= ~(uint64_t{0xf} << Shift);
      Callback(GlobalTemps[WordIdx * 16 + Slot], Mask);
    }
  }

  template <typename Fn>
  void ForEachLive(const std::vector<uint64_t> &Packed, Fn &&Callback) const {
    for (size_t W = 0; W < Packed.size(); ++W)
      if (Packed[W])
        ForEachLiveInWord(W, Packed[W], Callback);
  }
};

inline bool HasCondition(const FlowBlock &Block) {
  return Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch;
}

// Computes exact per-(vreg, lane) liveness across all FlowBlocks and validates
// all temporary operand bounds.
bool ComputeLiveness(llvm::ArrayRef<FlowBlock> Blocks, uint32_t TempCount,
                     LivenessResult &Result);

// Combined destination mask across all non-null destination operands of Inst.
uint8_t CombinedInstructionDestMask(const Instruction &Inst);

} // namespace dxcb5
#endif
