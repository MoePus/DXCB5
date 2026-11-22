// Distributed under the University of Illinois Open Source License.
// Post-RA allocation validator matching `lean/RegAllocValidator.lean`.
// Verifies bounds, lane-map injectivity, fixed-lane preservation, physical
// mask exactness, and zero write-clobber / entry-live / edge-copy aliasing
// across all live ranges (allowing coalesced self-mov chains that share a
// physical slot without intervening redefinition).
#include "RegAlloc/LaneAllocator.h"
#include "RegAlloc/LaneInterference.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace dxcb5;

namespace {

uint64_t PackVSlot(uint32_t VReg, unsigned Lane) {
  return (static_cast<uint64_t>(VReg) << 2) | (Lane & 3u);
}

uint32_t PhysSlotKey(const LaneAllocation &A, unsigned VL) {
  return A.PhysReg * 4u + A.LaneMap[VL];
}

std::string FormatVSlot(uint64_t VKey) {
  return "v" + std::to_string(VKey >> 2) + "." + "xyzw"[VKey & 3u];
}

std::string FormatPSlot(uint32_t PKey) {
  return "r" + std::to_string(PKey / 4u) + "." + "xyzw"[PKey & 3u];
}

struct PhysLiveSet {
  std::vector<llvm::SmallVector<uint64_t, 2>> Slots;
  std::vector<uint32_t> Dirty;

  explicit PhysLiveSet(uint32_t PhysTempCount)
      : Slots(static_cast<size_t>(PhysTempCount) * 4u) {}

  void Clear() {
    for (uint32_t K : Dirty)
      Slots[K].clear();
    Dirty.clear();
  }

  void Add(uint32_t PKey, uint64_t VKey) {
    auto &Vec = Slots[PKey];
    for (uint64_t Cur : Vec)
      if (Cur == VKey)
        return;
    if (Vec.empty())
      Dirty.push_back(PKey);
    Vec.push_back(VKey);
  }

  void Kill(uint32_t PKey, uint64_t VKey) {
    auto &Vec = Slots[PKey];
    for (size_t I = 0; I < Vec.size(); ++I) {
      if (Vec[I] == VKey) {
        Vec[I] = Vec.back();
        Vec.pop_back();
        return;
      }
    }
  }

  bool CheckWriteDisjoint(uint32_t PKey, uint64_t VKey, std::string &Err) const {
    for (uint64_t Cur : Slots[PKey]) {
      if (Cur != VKey) {
        Err = "VerifyAllocation: write to " + FormatVSlot(VKey) +
              " clobbers live " + FormatVSlot(Cur) + " at " + FormatPSlot(PKey);
        return false;
      }
    }
    return true;
  }

  bool CheckAllSingleton(std::string &Err) const {
    for (uint32_t K : Dirty) {
      if (Slots[K].size() > 1) {
        Err = "VerifyAllocation: conflicting live-in virtual slots mapped to " +
              FormatPSlot(K) + " (" + FormatVSlot(Slots[K][0]) + " vs " +
              FormatVSlot(Slots[K][1]) + ")";
        return false;
      }
    }
    return true;
  }
};

void AddLiveRegMask(PhysLiveSet &LiveSet,
                    llvm::ArrayRef<LaneAllocation> Alloc, uint32_t VReg,
                    uint8_t Mask) {
  const LaneAllocation &A = Alloc[VReg];
  for (unsigned L = 0; L < 4; ++L)
    if (Mask & (1u << L))
      LiveSet.Add(PhysSlotKey(A, L), PackVSlot(VReg, L));
}

bool VerifyPerVRegInvariants(const LivenessResult &Live,
                             llvm::ArrayRef<LaneAllocation> Alloc,
                             uint32_t PhysTempCount, std::string &Err) {
  if (Alloc.size() < Live.Temps.size()) {
    Err = "VerifyAllocation: allocation table smaller than temp count";
    return false;
  }
  if (PhysTempCount > regalloc::kShaderTempLimit) {
    Err = "VerifyAllocation: physical temp count exceeds SM5 limit";
    return false;
  }
  for (uint32_t V : Live.ActiveTemps) {
    const TempInfo &T = Live.Temps[V];
    const LaneAllocation &A = Alloc[V];
    if (A.PhysReg >= PhysTempCount) {
      Err = "VerifyAllocation: v" + std::to_string(V) +
            " assigned out-of-bounds physical register r" +
            std::to_string(A.PhysReg);
      return false;
    }
    uint8_t ExpectedMask = 0;
    for (unsigned L = 0; L < 4; ++L) {
      if (!(T.ActiveMask & (1u << L)))
        continue;
      const uint8_t PL = A.LaneMap[L];
      if (PL >= 4) {
        Err = "VerifyAllocation: invalid physical lane in v" + std::to_string(V);
        return false;
      }
      if (T.FixedLanes && PL != L) {
        Err = "VerifyAllocation: fixed-lane v" + std::to_string(V) +
              " lane remapped";
        return false;
      }
      if (ExpectedMask & (1u << PL)) {
        Err = "VerifyAllocation: non-injective lane map on v" + std::to_string(V);
        return false;
      }
      ExpectedMask |= static_cast<uint8_t>(1u << PL);
    }
    if (A.PhysMask != ExpectedMask) {
      Err = "VerifyAllocation: PhysMask mismatch on v" + std::to_string(V);
      return false;
    }
  }
  return true;
}

bool VerifyEdgeCopies(const FlowEdge &Edge, const LivenessResult &Live,
                      llvm::ArrayRef<LaneAllocation> Alloc,
                      PhysLiveSet &LiveSet, std::string &Err) {
  if (Edge.Copies.empty())
    return true;
  LiveSet.Clear();
  Live.ForEachLive(Live.Blocks[Edge.Target].In, [&](uint32_t R, uint8_t Mask) {
    AddLiveRegMask(LiveSet, Alloc, R, Mask);
  });
  for (const ParallelCopy &Copy : Edge.Copies) {
    const uint32_t PKey = PhysSlotKey(Alloc[Copy.Destination], Copy.DestinationLane);
    const uint64_t VKey = PackVSlot(Copy.Destination, Copy.DestinationLane);
    const bool IsSelfCopy =
        Copy.ReadsTemp() &&
        PKey == PhysSlotKey(Alloc[Copy.Source.Index], Copy.SourceLane());
    if (!IsSelfCopy && !LiveSet.CheckWriteDisjoint(PKey, VKey, Err))
      return false;
    LiveSet.Add(PKey, VKey);
  }
  return true;
}

bool VerifyInstructionStep(const Instruction &Inst,
                           llvm::ArrayRef<LaneAllocation> Alloc,
                           PhysLiveSet &LiveSet, std::string &Err) {
  const size_t Dests = std::min<size_t>(
      InstructionDestinationCount(Inst.Opcode), Inst.Operands.size());
  const uint8_t DestMask = CombinedInstructionDestMask(Inst);
  const bool PlainTempMov =
      Inst.Opcode == D3D10_SB_OPCODE_MOV && !Inst.Saturate &&
      Inst.Operands.size() >= 2 &&
      Inst.Operands[0].Kind == OperandKind::Temp &&
      Inst.Operands[1].Kind == OperandKind::Temp &&
      Inst.Operands[1].Modifier == 0 &&
      Inst.Operands[1].RelativeRegister == UINT32_MAX;

  for (size_t I = 0; I < Dests; ++I) {
    const Operand &Dst = Inst.Operands[I];
    if (Dst.Kind != OperandKind::Temp)
      continue;
    for (unsigned L = 0; L < 4; ++L) {
      if (!(Dst.Mask & (1u << L)))
        continue;
      const uint32_t PKey = PhysSlotKey(Alloc[Dst.Index], L);
      const uint64_t VKey = PackVSlot(Dst.Index, L);
      bool IsSelfLaneMov = false;
      if (PlainTempMov && I == 0) {
        const unsigned SL = (Inst.Operands[1].Swizzle >> (2 * L)) & 3u;
        IsSelfLaneMov = (PKey == PhysSlotKey(Alloc[Inst.Operands[1].Index], SL));
      }
      if (!IsSelfLaneMov && !LiveSet.CheckWriteDisjoint(PKey, VKey, Err))
        return false;
      LiveSet.Add(PKey, VKey);
    }
  }
  for (size_t I = 0; I < Dests; ++I) {
    const Operand &Dst = Inst.Operands[I];
    if (Dst.Kind == OperandKind::Temp) {
      for (unsigned L = 0; L < 4; ++L)
        if (Dst.Mask & (1u << L))
          LiveSet.Kill(PhysSlotKey(Alloc[Dst.Index], L), PackVSlot(Dst.Index, L));
    }
    if (Dst.RelativeRegister != UINT32_MAX)
      AddLiveRegMask(LiveSet, Alloc, Dst.RelativeRegister,
                     static_cast<uint8_t>(1u << Dst.RelativeLane));
  }
  for (size_t I = Dests; I < Inst.Operands.size(); ++I) {
    const Operand &Src = Inst.Operands[I];
    if (Src.Kind == OperandKind::Temp) {
      const uint8_t Read = InstructionSourceReadLanes(
          Inst.Opcode, DestMask, I - Dests, Src.Swizzle);
      AddLiveRegMask(LiveSet, Alloc, Src.Index, Read);
    }
    if (Src.RelativeRegister != UINT32_MAX)
      AddLiveRegMask(LiveSet, Alloc, Src.RelativeRegister,
                     static_cast<uint8_t>(1u << Src.RelativeLane));
  }
  return true;
}

} // namespace

bool dxcb5::VerifyAllocation(llvm::ArrayRef<FlowBlock> Blocks,
                             const LivenessResult &Live,
                             llvm::ArrayRef<LaneAllocation> Allocation,
                             uint32_t PhysTempCount, std::string &Error) {
  if (!VerifyPerVRegInvariants(Live, Allocation, PhysTempCount, Error))
    return false;
  PhysLiveSet LiveSet(PhysTempCount);
  for (size_t B = 0; B < Blocks.size(); ++B) {
    const FlowBlock &Block = Blocks[B];
    for (const FlowEdge &Edge : Block.Edges)
      if (!VerifyEdgeCopies(Edge, Live, Allocation, LiveSet, Error))
        return false;
    LiveSet.Clear();
    Live.ForEachLive(Live.Blocks[B].Out, [&](uint32_t R, uint8_t Mask) {
      AddLiveRegMask(LiveSet, Allocation, R, Mask);
    });
    if (HasCondition(Block)) {
      if (Block.Condition.Kind == OperandKind::Temp)
        AddLiveRegMask(LiveSet, Allocation, Block.Condition.Index,
                       static_cast<uint8_t>(1u << (Block.Condition.Swizzle & 3u)));
      if (Block.Condition.RelativeRegister != UINT32_MAX)
        AddLiveRegMask(LiveSet, Allocation, Block.Condition.RelativeRegister,
                       static_cast<uint8_t>(1u << Block.Condition.RelativeLane));
    }
    for (auto It = Block.Instructions.rbegin(); It != Block.Instructions.rend();
         ++It) {
      if (!VerifyInstructionStep(*It, Allocation, LiveSet, Error))
        return false;
    }
    if ((B == 0 || Live.Blocks[B].Predecessors.empty()) &&
        !LiveSet.CheckAllSingleton(Error))
      return false;
  }
  return true;
}
