// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_REGALLOC_LANE_INTERFERENCE_H
#define DXCB5_REGALLOC_LANE_INTERFERENCE_H

#include "RegAlloc/LaneAllocator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include <array>
#include <cstdint>
#include <vector>

namespace dxcb5 {
namespace regalloc {

using BitWord = uint64_t;
constexpr unsigned kWordBits = sizeof(BitWord) * 8;
constexpr unsigned kRegisterLanes = 4;
constexpr uint32_t kNoSlot = UINT32_MAX;
// Dense bitmatrix up to 10,240 slots (<= 12.5 MiB); sparse chunks above that
// so extreme shaders up to kMaxGraphSlots avoid a 72 MiB dense allocation.
constexpr size_t kDenseGraphSlotLimit = 10240;
constexpr size_t kMaxGraphSlots = 24576;
constexpr uint32_t kControlFlowReservedTemps = 3;
constexpr uint32_t kShaderTempLimit = 4096 - kControlFlowReservedTemps;

class SparseSlotState {
public:
  explicit SparseSlotState(size_t SlotCount)
      : Words((SlotCount + kWordBits - 1) / kWordBits, 0),
        Positions(Words.size(), 0) {
    Active.reserve(Words.size());
  }

  void Clear() {
    for (uint32_t W : Active)
      Words[W] = 0;
    Active.clear();
  }

  void AddSlot(uint32_t S) {
    if (S == kNoSlot)
      return;
    const uint32_t W = S / kWordBits;
    const BitWord B = BitWord{1} << (S % kWordBits);
    if (!Words[W]) {
      Positions[W] = Active.size();
      Active.push_back(W);
    }
    Words[W] |= B;
  }

  void KillSlot(uint32_t S) {
    if (S == kNoSlot)
      return;
    const uint32_t W = S / kWordBits;
    const BitWord B = BitWord{1} << (S % kWordBits);
    if (!(Words[W] & B))
      return;
    Words[W] &= ~B;
    if (Words[W])
      return;
    const size_t At = Positions[W];
    const uint32_t Moved = Active.back();
    Active[At] = Moved;
    Positions[Moved] = At;
    Active.pop_back();
  }

  const std::vector<uint32_t> &ActiveWords() const { return Active; }
  BitWord Word(uint32_t W) const { return Words[W]; }

private:
  std::vector<BitWord> Words;
  std::vector<size_t> Positions;
  std::vector<uint32_t> Active;
};

struct AffinityLink {
  uint32_t Peer = 0;
  uint8_t MyLane = 0;
  uint8_t PeerLane = 0;
  uint32_t Weight = 0;
};

class SlotInterferenceGraph {
public:
  struct WordChunk {
    uint16_t Word = 0;
    BitWord Bits = 0;
  };

  explicit SlotInterferenceGraph(size_t SlotCount)
      : UseSparse(SlotCount > kDenseGraphSlotLimit),
        Stride(UseSparse ? 0 : (SlotCount + kWordBits - 1) / kWordBits),
        Rows(UseSparse ? 0 : SlotCount * Stride, 0),
        SparseRows(UseSparse ? SlotCount : 0),
        Degree(SlotCount, 0) {}

  void InterfereAgainst(uint32_t S, const SparseSlotState &State,
                        uint32_t SkipSlot = kNoSlot);
  void RecordLiveInputs(const SparseSlotState &State);
  bool Interferes(uint32_t A, uint32_t B) const {
    if (A == kNoSlot || B == kNoSlot)
      return false;
    const uint32_t W = B / kWordBits;
    const BitWord Mask = BitWord{1} << (B % kWordBits);
    if (!UseSparse)
      return (Row(A)[W] & Mask) != 0;
    return (LookupSparseWord(A, static_cast<uint16_t>(W)) & Mask) != 0;
  }
  void MergeSlots(uint32_t KeepS, uint32_t DeadS);
  unsigned GetDegree(uint32_t S) const { return S == kNoSlot ? 0u : Degree[S]; }

  template <typename Fn> void ForEachNeighbor(uint32_t S, Fn &&Callback) const {
    if (UseSparse) {
      for (const WordChunk &C : SparseRows[S])
        VisitWord(C.Word, C.Bits, Callback);
      return;
    }
    const BitWord *Adj = Row(S);
    for (size_t W = 0; W < Stride; ++W)
      if (Adj[W])
        VisitWord(W, Adj[W], Callback);
  }

private:
  template <typename Fn>
  static void VisitWord(size_t W, BitWord Bits, Fn &&Callback) {
    while (Bits) {
      const uint32_t Other = static_cast<uint32_t>(
          W * kWordBits + llvm::countTrailingZeros(Bits));
      Callback(Other);
      Bits &= Bits - 1;
    }
  }

  BitWord LookupSparseWord(uint32_t S, uint16_t W) const;
  BitWord OrSparseWord(uint32_t S, uint16_t W, BitWord Bits);

  BitWord *Row(uint32_t S) { return Rows.data() + static_cast<size_t>(S) * Stride; }
  const BitWord *Row(uint32_t S) const {
    return Rows.data() + static_cast<size_t>(S) * Stride;
  }

  bool UseSparse;
  size_t Stride;
  std::vector<BitWord> Rows;
  std::vector<llvm::SmallVector<WordChunk, 4>> SparseRows;
  std::vector<unsigned> Degree;
};

struct CoalesceState {
  std::vector<uint32_t> Rep;
  std::vector<std::array<uint8_t, 4>> LaneToRep;
  std::vector<llvm::SmallVector<uint32_t, 2>> Members;
  std::vector<uint8_t> RepFixed;
  std::vector<uint8_t> RepMask;
};

void BuildInterferenceAndAffinities(
    llvm::ArrayRef<FlowBlock> Blocks, const LivenessResult &Live,
    const std::vector<std::array<uint32_t, 4>> &SlotOf, size_t NumSlots,
    SlotInterferenceGraph &Graph,
    std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
    std::vector<uint64_t> &TotalWeight);

void PreCoalesceRegisters(
    const LivenessResult &Live,
    const std::vector<std::array<uint32_t, 4>> &SlotOf,
    const std::vector<uint8_t> &PreColored,
    const std::vector<llvm::SmallVector<AffinityLink, 4>> &Affinities,
    std::vector<uint64_t> &TotalWeight, SlotInterferenceGraph &Graph,
    CoalesceState &State);

} // namespace regalloc
} // namespace dxcb5

#endif
