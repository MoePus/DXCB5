// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_REGALLOC_LANE_ALLOCATOR_H
#define DXCB5_REGALLOC_LANE_ALLOCATOR_H

#include "RegAlloc/Liveness.h"
#include "dxcb5/ControlFlow.h"
#include "llvm/ADT/ArrayRef.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dxcb5 {

struct LaneAllocation {
  uint32_t PhysReg = UINT32_MAX;
  std::array<uint8_t, 4> LaneMap = {0, 1, 2, 3};
  uint8_t PhysMask = 0;
};

// Allocates virtual registers to physical (PhysReg, LaneMap) sub-register slots
// with cross-lane weighted affinity coalescing and tight sub-register packing.
bool AllocateLaneRegisters(llvm::ArrayRef<FlowBlock> Blocks,
                           const LivenessResult &Live,
                           std::vector<LaneAllocation> &Allocation,
                           uint32_t &PhysTempCount);

// Post-RA validator matching `lean/RegAllocValidator.lean`: checks bounds,
// lane-map injectivity, fixed-lane preservation, PhysMask exactness, and zero
// physical-slot interference across all live ranges and edge copies.
bool VerifyAllocation(llvm::ArrayRef<FlowBlock> Blocks,
                      const LivenessResult &Live,
                      llvm::ArrayRef<LaneAllocation> Allocation,
                      uint32_t PhysTempCount, std::string &Error);

} // namespace dxcb5
#endif
