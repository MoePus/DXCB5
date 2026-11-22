// Distributed under the University of Illinois Open Source License.
#include "dxcb5/RegisterAllocation.h"
#include "RegAlloc/LaneAllocator.h"
#include "RegAlloc/Liveness.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <array>

using namespace dxcb5;

namespace {

constexpr unsigned kRegisterLanes = 4;
constexpr uint8_t kBroadcastSwizzle = 0x55u;
constexpr uint8_t kIdentitySwizzle = 0xe4u;

unsigned GetLane(uint8_t Swizzle, unsigned Slot) {
  return (Swizzle >> (2 * Slot)) & 3u;
}

uint8_t SetLane(uint8_t Swizzle, unsigned Slot, unsigned Lane) {
  const unsigned Shift = 2 * Slot;
  return static_cast<uint8_t>((Swizzle & ~(3u << Shift)) | ((Lane & 3u) << Shift));
}

class LiteralVectorInterner {
public:
  explicit LiteralVectorInterner(MirProgram &Program) : Program(Program) {
    for (uint32_t I = 0; I < Program.LiteralVectors.size(); ++I)
      Buckets[HashBits(Program.LiteralVectors[I])].push_back(I);
  }

  uint32_t Intern(const std::array<uint32_t, 4> &Bits) {
    const uint64_t H = HashBits(Bits);
    auto &Bucket = Buckets[H];
    for (uint32_t Idx : Bucket)
      if (Program.LiteralVectors[Idx] == Bits)
        return Idx;
    const auto Idx = static_cast<uint32_t>(Program.LiteralVectors.size());
    Program.LiteralVectors.push_back(Bits);
    Bucket.push_back(Idx);
    return Idx;
  }

private:
  static uint64_t HashBits(const std::array<uint32_t, 4> &Bits) {
    return (uint64_t(Bits[0]) * 0x9e3779b97f4a7c15ULL) ^
           (uint64_t(Bits[1]) * 0xbf58476d1ce4e5b9ULL) ^
           (uint64_t(Bits[2]) * 0x94d049bb133111ebULL) ^
           (uint64_t(Bits[3]) * 0x2545f4914f6cdd1dULL);
  }

  MirProgram &Program;
  llvm::DenseMap<uint64_t, llvm::SmallVector<uint32_t, 2>> Buckets;
};

void RemapRelative(Operand &Op, const std::vector<LaneAllocation> &Alloc) {
  if (Op.RelativeRegister == UINT32_MAX)
    return;
  const LaneAllocation &Rel = Alloc[Op.RelativeRegister];
  Op.RelativeRegister = Rel.PhysReg;
  Op.RelativeLane = Rel.LaneMap[Op.RelativeLane];
}

void RemapImmediateVectorSource(Operand &Src, uint8_t OldDestMask,
                                const LaneAllocation &DestAlloc,
                                const MirProgram &Program,
                                LiteralVectorInterner &Interner) {
  const auto &OldBits = Program.LiteralVectors[Src.Index];
  std::array<uint32_t, 4> NewBits{};
  uint32_t FirstVal = 0;
  bool HaveFirst = false;
  bool AllSame = true;
  for (unsigned VL = 0; VL < kRegisterLanes; ++VL) {
    if (!(OldDestMask & (1u << VL)))
      continue;
    const unsigned PL = DestAlloc.LaneMap[VL];
    const uint32_t Val = OldBits[GetLane(Src.Swizzle, VL)];
    NewBits[PL] = Val;
    if (!HaveFirst) {
      FirstVal = Val;
      HaveFirst = true;
    } else if (Val != FirstVal) {
      AllSame = false;
    }
  }
  if (HaveFirst && AllSame) {
    Src = {OperandKind::Immediate, FirstVal};
    return;
  }
  for (unsigned PL = 0; PL < kRegisterLanes; ++PL)
    if (!(DestAlloc.PhysMask & (1u << PL)))
      NewBits[PL] = FirstVal;
  Src.Index = Interner.Intern(NewBits);
  Src.Swizzle = kIdentitySwizzle;
}

void RemapComponentwiseSource(Operand &Src, uint8_t OldDestMask,
                              const LaneAllocation &DestAlloc,
                              const std::vector<LaneAllocation> &Alloc,
                              const MirProgram &Program,
                              LiteralVectorInterner &Interner) {
  RemapRelative(Src, Alloc);
  if (Src.Kind == OperandKind::Immediate || Src.Kind == OperandKind::Null ||
      Src.Kind == OperandKind::Resource || Src.Kind == OperandKind::Sampler ||
      Src.Kind == OperandKind::Uav || Src.Kind == OperandKind::InputCoverage)
    return;
  if (Src.Kind == OperandKind::ImmediateVector) {
    RemapImmediateVectorSource(Src, OldDestMask, DestAlloc, Program, Interner);
    return;
  }
  uint8_t NewSwz = 0;
  uint8_t WrittenSlots = 0;
  unsigned FirstSrcLane = 0;
  bool HaveFirst = false;
  const LaneAllocation *SrcAlloc =
      Src.Kind == OperandKind::Temp ? &Alloc[Src.Index] : nullptr;
  for (unsigned VL = 0; VL < kRegisterLanes; ++VL) {
    if (!(OldDestMask & (1u << VL)))
      continue;
    const unsigned PL = DestAlloc.LaneMap[VL];
    const unsigned OldSL = GetLane(Src.Swizzle, VL);
    const unsigned NewSL = SrcAlloc ? SrcAlloc->LaneMap[OldSL] : OldSL;
    NewSwz = SetLane(NewSwz, PL, NewSL);
    WrittenSlots |= static_cast<uint8_t>(1u << PL);
    if (!HaveFirst) {
      FirstSrcLane = NewSL;
      HaveFirst = true;
    }
  }
  for (unsigned PL = 0; PL < kRegisterLanes; ++PL)
    if (!(WrittenSlots & (1u << PL)))
      NewSwz = SetLane(NewSwz, PL, FirstSrcLane);
  if (SrcAlloc)
    Src.Index = SrcAlloc->PhysReg;
  Src.Swizzle = NewSwz;
}

void RemapFixedSlotSource(Operand &Src, uint8_t ActiveSlots,
                          const std::vector<LaneAllocation> &Alloc) {
  RemapRelative(Src, Alloc);
  if (Src.Kind != OperandKind::Temp)
    return;
  const LaneAllocation &SrcAlloc = Alloc[Src.Index];
  uint8_t NewSwz = 0;
  unsigned FirstSrcLane = 0;
  bool HaveFirst = false;
  for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot) {
    if (!(ActiveSlots & (1u << Slot)))
      continue;
    const unsigned NewSL = SrcAlloc.LaneMap[GetLane(Src.Swizzle, Slot)];
    NewSwz = SetLane(NewSwz, Slot, NewSL);
    if (!HaveFirst) {
      FirstSrcLane = NewSL;
      HaveFirst = true;
    }
  }
  for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot)
    if (!(ActiveSlots & (1u << Slot)))
      NewSwz = SetLane(NewSwz, Slot, FirstSrcLane);
  Src.Index = SrcAlloc.PhysReg;
  Src.Swizzle = NewSwz;
}

bool IsSelfMov(const Instruction &Inst) {
  if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate ||
      Inst.Operands.size() < 2)
    return false;
  const Operand &Dst = Inst.Operands[0];
  const Operand &Src = Inst.Operands[1];
  if (Dst.Kind != OperandKind::Temp || Src.Kind != OperandKind::Temp ||
      Dst.Index != Src.Index || Src.Modifier != 0 ||
      Src.RelativeRegister != UINT32_MAX || Dst.RelativeRegister != UINT32_MAX)
    return false;
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if ((Dst.Mask & (1u << L)) && GetLane(Src.Swizzle, L) != L)
      return false;
  return true;
}

void RemapResourceReturnSwizzle(Operand &Res, uint8_t OldMask,
                                const LaneAllocation &DestAlloc) {
  uint8_t NewSwz = 0;
  uint8_t WrittenSlots = 0;
  unsigned FirstChan = 0;
  bool HaveFirst = false;
  for (unsigned VL = 0; VL < kRegisterLanes; ++VL) {
    if (!(OldMask & (1u << VL)))
      continue;
    const unsigned PL = DestAlloc.LaneMap[VL];
    const unsigned Chan = GetLane(Res.Swizzle, VL);
    NewSwz = SetLane(NewSwz, PL, Chan);
    WrittenSlots |= static_cast<uint8_t>(1u << PL);
    if (!HaveFirst) {
      FirstChan = Chan;
      HaveFirst = true;
    }
  }
  for (unsigned PL = 0; PL < kRegisterLanes; ++PL)
    if (!(WrittenSlots & (1u << PL)))
      NewSwz = SetLane(NewSwz, PL, FirstChan);
  Res.Swizzle = NewSwz;
}

uint8_t MapDestMask(uint8_t OldMask, const LaneAllocation &DestAlloc) {
  uint8_t NewMask = 0;
  for (unsigned VL = 0; VL < kRegisterLanes; ++VL)
    if (OldMask & (1u << VL))
      NewMask |= static_cast<uint8_t>(1u << DestAlloc.LaneMap[VL]);
  return NewMask;
}

void RemapInstruction(Instruction &Inst,
                      const std::vector<LaneAllocation> &Alloc,
                      const MirProgram &Program,
                      LiteralVectorInterner &Interner) {
  const size_t Dests = std::min<size_t>(
      InstructionDestinationCount(Inst.Opcode), Inst.Operands.size());
  const uint8_t CombinedMask = CombinedInstructionDestMask(Inst);

  int SingleTempDest = -1;
  unsigned TempDestCount = 0;
  for (size_t I = 0; I < Dests; ++I) {
    if (Inst.Operands[I].Kind == OperandKind::Temp) {
      SingleTempDest = static_cast<int>(I);
      ++TempDestCount;
    }
  }
  if (InstructionIsLaneRemappable(Inst.Opcode) && TempDestCount == 1) {
    Operand &Dst = Inst.Operands[SingleTempDest];
    const LaneAllocation &DestAlloc = Alloc[Dst.Index];
    const uint8_t OldMask = static_cast<uint8_t>(Dst.Mask & 0xfu);
    for (size_t I = Dests; I < Inst.Operands.size(); ++I)
      RemapComponentwiseSource(Inst.Operands[I], OldMask, DestAlloc, Alloc,
                               Program, Interner);
    RemapRelative(Dst, Alloc);
    Dst.Index = DestAlloc.PhysReg;
    Dst.Mask = MapDestMask(OldMask, DestAlloc);
    return;
  }

  if (InstructionHasResourceReturnSwizzle(Inst.Opcode) && TempDestCount == 1) {
    const Operand &DstRef = Inst.Operands[SingleTempDest];
    const LaneAllocation &DestAlloc = Alloc[DstRef.Index];
    const uint8_t OldMask = static_cast<uint8_t>(DstRef.Mask & 0xfu);
    for (size_t I = Dests; I < Inst.Operands.size(); ++I)
      if (Inst.Operands[I].Kind == OperandKind::Resource ||
          Inst.Operands[I].Kind == OperandKind::Uav)
        RemapResourceReturnSwizzle(Inst.Operands[I], OldMask, DestAlloc);
  }

  for (size_t I = Dests; I < Inst.Operands.size(); ++I) {
    const uint8_t Slots =
        InstructionSourceSlotMaskSm5(Inst.Opcode, CombinedMask, I - Dests);
    RemapFixedSlotSource(Inst.Operands[I], Slots, Alloc);
  }
  for (size_t I = 0; I < Dests; ++I) {
    Operand &Dst = Inst.Operands[I];
    RemapRelative(Dst, Alloc);
    if (Dst.Kind != OperandKind::Temp)
      continue;
    const LaneAllocation &DestAlloc = Alloc[Dst.Index];
    Dst.Index = DestAlloc.PhysReg;
    Dst.Mask = MapDestMask(Dst.Mask, DestAlloc);
  }
}

void RemapBlock(FlowBlock &Block, const std::vector<LaneAllocation> &Alloc,
                const MirProgram &Program, LiteralVectorInterner &Interner) {
  if (HasCondition(Block))
    RemapFixedSlotSource(Block.Condition, 0x1u, Alloc);
  std::vector<Instruction> Cleaned;
  Cleaned.reserve(Block.Instructions.size());
  for (Instruction &Inst : Block.Instructions) {
    RemapInstruction(Inst, Alloc, Program, Interner);
    if (!IsSelfMov(Inst))
      Cleaned.push_back(std::move(Inst));
  }
  Block.Instructions = std::move(Cleaned);

  for (FlowEdge &Edge : Block.Edges) {
    std::vector<ParallelCopy> Copies;
    Copies.reserve(Edge.Copies.size());
    for (ParallelCopy Copy : Edge.Copies) {
      if (Copy.ReadsTemp()) {
        const LaneAllocation &SrcAlloc = Alloc[Copy.Source.Index];
        const uint8_t NewSL = SrcAlloc.LaneMap[Copy.SourceLane()];
        Copy.Source.Index = SrcAlloc.PhysReg;
        Copy.Source.Swizzle = static_cast<uint8_t>(NewSL * kBroadcastSwizzle);
      }
      RemapRelative(Copy.Source, Alloc);
      const LaneAllocation &DstAlloc = Alloc[Copy.Destination];
      Copy.Destination = DstAlloc.PhysReg;
      Copy.DestinationLane = DstAlloc.LaneMap[Copy.DestinationLane];
      if (!Copy.ReadsTemp() || Copy.Destination != Copy.Source.Index ||
          Copy.DestinationLane != Copy.SourceLane())
        Copies.push_back(std::move(Copy));
    }
    Edge.Copies = std::move(Copies);
  }
}

} // namespace

bool dxcb5::AllocateRegisters(std::vector<FlowBlock> &Blocks,
                              MirProgram &Program, std::string &Error) {
  llvm::TimeTraceScope Timer("DXCB5 Allocate Registers", llvm::StringRef());
  LivenessResult Live;
  if (!ComputeLiveness(Blocks, Program.TempCount, Live)) {
    Error = "MIR register allocation found an invalid temporary or edge";
    return false;
  }
  std::vector<LaneAllocation> Allocation;
  uint32_t PhysTempCount = 0;
  if (!AllocateLaneRegisters(Blocks, Live, Allocation, PhysTempCount)) {
    Error = "MIR register allocation exceeded SM5 temporary register limit";
    return false;
  }
  if (!VerifyAllocation(Blocks, Live, Allocation, PhysTempCount, Error))
    return false;
  Program.TempCount = PhysTempCount;
  LiteralVectorInterner Interner(Program);
  for (FlowBlock &Block : Blocks)
    RemapBlock(Block, Allocation, Program, Interner);
  return true;
}
