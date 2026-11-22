// Distributed under the University of Illinois Open Source License.
// Unified code-motion acceptance gate shared by LICM, ColdSink, and
// PressureShrink. Mirrors the formally verified rule in lean/Placement.lean.
#ifndef DXCB5_MIR_PLACEMENT_STORE_H
#define DXCB5_MIR_PLACEMENT_STORE_H

#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace dxcb5 {

enum class PlacementOwner : uint8_t {
  None = 0,
  Licm = 1,
  ColdSink = 2,
  PressureShrink = 3,
};

struct PlacementMove {
  uint32_t ValId = UINT32_MAX;
  PlacementOwner Owner = PlacementOwner::None;
  uint32_t SrcDepth = 0;
  llvm::SmallVector<uint32_t, 4> DstDepths;
  bool KeepSrc = false;    // True for partial remat (original def retained).
  bool Bypassable = false; // True when target is dominated & bypassable (ColdSink).
  uint32_t PlBefore = 0;
  uint32_t PlAfter = 0;
  uint32_t Budget = 180;
};

class PlacementStore {
public:
  // Exact same weight as Emit/Oracle.cpp::ComputeDynamicCost (`8 ^ min(d, 6)`).
  static uint64_t BlockWeight(uint32_t Depth) {
    const uint32_t Clamped = std::min<uint32_t>(Depth, 6u);
    return uint64_t(1) << (3u * Clamped);
  }

  static uint64_t OracleMuBefore(const PlacementMove &M) {
    return BlockWeight(M.SrcDepth);
  }

  static uint64_t OracleMuAfter(const PlacementMove &M) {
    uint64_t Cost = M.KeepSrc ? OracleMuBefore(M) : 0u;
    for (uint32_t D : M.DstDepths)
      Cost += BlockWeight(D);
    return Cost;
  }

  static uint64_t ExpectedMuBefore(const PlacementMove &M) {
    return 2u * BlockWeight(M.SrcDepth);
  }

  static uint64_t ExpectedMuAfter(const PlacementMove &M) {
    uint64_t Cost = M.KeepSrc ? ExpectedMuBefore(M) : 0u;
    const uint64_t Scale = M.Bypassable ? 1u : 2u;
    for (uint32_t D : M.DstDepths)
      Cost += Scale * BlockWeight(D);
    return Cost;
  }

  PlacementOwner OwnerOf(uint32_t ValId) const {
    return ValId < Owners_.size() ? Owners_[ValId] : PlacementOwner::None;
  }

  // Evaluates lean/Placement.lean's `acceptMove` and commits ownership on accept.
  bool TryAccept(const PlacementMove &M) {
    if (M.Owner == PlacementOwner::None || M.DstDepths.empty())
      return false;
    for (uint32_t D : M.DstDepths)
      if (D > M.SrcDepth)
        return false;
    const PlacementOwner Cur = OwnerOf(M.ValId);
    if (Cur != PlacementOwner::None && Cur != M.Owner)
      return false;

    const uint32_t ExB = M.PlBefore > M.Budget ? M.PlBefore - M.Budget : 0u;
    const uint32_t ExA = M.PlAfter > M.Budget ? M.PlAfter - M.Budget : 0u;
    const uint64_t OMuB = OracleMuBefore(M);
    const uint64_t OMuA = OracleMuAfter(M);
    const uint64_t EMuB = ExpectedMuBefore(M);
    const uint64_t EMuA = ExpectedMuAfter(M);

    const bool StrictlyImproves =
        (ExA < ExB) ||
        (ExA == ExB && OMuA < OMuB) ||
        (ExA == ExB && OMuA == OMuB && EMuA < EMuB) ||
        (ExA == ExB && OMuA == OMuB && EMuA == EMuB && M.PlAfter < M.PlBefore);
    const bool NonWorsening =
        (ExA == ExB && OMuA == OMuB && EMuA == EMuB && M.PlAfter == M.PlBefore);

    if (!StrictlyImproves && !(Cur == PlacementOwner::None && NonWorsening))
      return false;

    if (M.ValId != UINT32_MAX) {
      if (M.ValId >= Owners_.size())
        Owners_.resize(M.ValId + 1u, PlacementOwner::None);
      Owners_[M.ValId] = M.Owner;
    }
    ++Accepted_;
    return true;
  }

  uint32_t AcceptedCount() const { return Accepted_; }

private:
  std::vector<PlacementOwner> Owners_;
  uint32_t Accepted_ = 0;
};

} // namespace dxcb5

#endif
