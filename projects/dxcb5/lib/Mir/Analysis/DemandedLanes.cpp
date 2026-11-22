// Distributed under the University of Illinois Open Source License.
#include "Mir/Analysis/DemandedLanes.h"
#include "Mir/OptPipeline.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/SmallVector.h"
#include <vector>

using namespace dxcb5;

namespace {

void MarkOperandLive(const Operand &Op, uint8_t SlotMask, uint32_t NumValues,
                     std::vector<uint8_t> &LiveLanes,
                     llvm::SmallVectorImpl<uint32_t> &Worklist) {
  if (Op.RelativeRegister != UINT32_MAX && Op.RelativeRegister < NumValues) {
    const uint8_t Bit = static_cast<uint8_t>(1u << Op.RelativeLane);
    if ((LiveLanes[Op.RelativeRegister] & Bit) != Bit) {
      LiveLanes[Op.RelativeRegister] |= Bit;
      Worklist.push_back(Op.RelativeRegister);
    }
  }
  if (Op.Kind != OperandKind::Temp || Op.Index == 0 || Op.Index >= NumValues)
    return;
  const uint8_t Read = ReadLanesForMask(Op.Swizzle, SlotMask);
  if ((LiveLanes[Op.Index] & Read) != Read) {
    LiveLanes[Op.Index] |= Read;
    Worklist.push_back(Op.Index);
  }
}

bool IsRemovableByDce(const SsaInst &Inst) {
  if (Inst.IsPure())
    return true;
  if (!InstructionCanEliminateIfDead(Inst.Opcode) ||
      Inst.Dest.Kind != OperandKind::Temp ||
      Inst.AuxDest.Kind != OperandKind::Null)
    return false;
  for (const auto &Src : Inst.Sources)
    if (Src.Kind == OperandKind::IndexableTemp || Src.Kind == OperandKind::Uav)
      return false;
  return true;
}

bool SweepDeadInsts(SsaBlock &Block, const std::vector<uint8_t> &LiveLanes) {
  bool Changed = false;
  for (auto &Phi : Block.Phis) {
    if (Phi.Dead)
      continue;
    const uint8_t Used = static_cast<uint8_t>(Phi.Mask & LiveLanes[Phi.Result]);
    if (Used == 0) {
      Phi.Dead = true;
      Changed = true;
    } else if (Used != Phi.Mask) {
      Phi.Mask = Used;
      Changed = true;
    }
  }
  for (auto &Inst : Block.Insts) {
    if (Inst.Dead || !IsRemovableByDce(Inst))
      continue;
    const uint8_t PrimaryLive =
        Inst.Dest.Kind == OperandKind::Temp
            ? static_cast<uint8_t>(Inst.Dest.Mask & LiveLanes[Inst.Dest.Index])
            : 0;
    const uint8_t AuxLive =
        Inst.AuxDest.Kind == OperandKind::Temp
            ? static_cast<uint8_t>(Inst.AuxDest.Mask &
                                   LiveLanes[Inst.AuxDest.Index])
            : 0;
    if (PrimaryLive == 0 && AuxLive == 0) {
      Inst.Dead = true;
      Changed = true;
      continue;
    }
    if (Inst.Opcode == D3D10_SB_OPCODE_SINCOS) {
      if (Inst.Dest.Kind == OperandKind::Temp && PrimaryLive == 0) {
        Inst.Dest = Operand();
        Changed = true;
      }
      if (Inst.AuxDest.Kind == OperandKind::Temp && AuxLive == 0) {
        Inst.AuxDest = Operand();
        Changed = true;
      }
      continue;
    }
    if ((InstructionUsesDemandedDestMask(Inst.Opcode) ||
         InstructionSupportsNarrowDestMask(Inst.Opcode)) &&
        Inst.Dest.Kind == OperandKind::Temp && PrimaryLive != Inst.Dest.Mask) {
      Inst.Dest.Mask = PrimaryLive;
      if (GetOpcodeDesc(Inst.Opcode).SlotRule == SourceSlotRule::PerLaneScalar) {
        for (size_t S = 0; S < Inst.Sources.size(); ++S)
          if (!(PrimaryLive & (1u << S)))
            Inst.Sources[S] = MakeImm(0u);
      }
      Changed = true;
    }
  }
  return Changed;
}

} // namespace

void dxcb5::ComputeDemandedLanes(const SsaProgram &Ssa,
                                 std::vector<uint8_t> &Demanded) {
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  Demanded.assign(Ssa.NextValueId, 0);
  llvm::SmallVector<uint32_t, 256> Worklist;

  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead || IsRemovableByDce(Inst))
        continue;
      const uint8_t DMask = Inst.Dest.Kind == OperandKind::Null
                                ? Inst.AuxDest.Mask
                                : Inst.Dest.Mask;
      if (Inst.Dest.RelativeRegister != UINT32_MAX)
        MarkOperandLive(Inst.Dest, 0x1u, Ssa.NextValueId, Demanded, Worklist);
      for (size_t S = 0; S < Inst.Sources.size(); ++S)
        MarkOperandLive(Inst.Sources[S],
                        InstructionSourceSlotMask(Inst.Opcode, DMask, S),
                        Ssa.NextValueId, Demanded, Worklist);
    }
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
      MarkOperandLive(Block.Condition, 0x1u, Ssa.NextValueId, Demanded,
                      Worklist);
  }

  while (!Worklist.empty()) {
    const uint32_t Id = Worklist.pop_back_val();
    const auto &Def = Defs[Id];
    if (!Def.Valid())
      continue;
    const uint8_t Need = Demanded[Id];
    if (Def.IsPhi) {
      const auto &Phi = Ssa.Blocks[Def.Block].Phis[Def.Index];
      if (Phi.Dead)
        continue;
      const uint8_t Active = static_cast<uint8_t>(Phi.Mask & Need);
      for (const auto &In : Phi.Inputs)
        MarkOperandLive(In.Value, Active, Ssa.NextValueId, Demanded, Worklist);
      continue;
    }
    const auto &Inst = Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (Inst.Dead || !IsRemovableByDce(Inst))
      continue;
    const uint8_t DMask = InstructionUsesDemandedDestMask(Inst.Opcode)
                              ? static_cast<uint8_t>(Inst.Dest.Mask & Need)
                              : (Def.IsAux ? Inst.AuxDest.Mask : Inst.Dest.Mask);
    for (size_t S = 0; S < Inst.Sources.size(); ++S) {
      const uint8_t SlotMask = InstructionSourceSlotMask(Inst.Opcode, DMask, S);
      if (SlotMask)
        MarkOperandLive(Inst.Sources[S], SlotMask, Ssa.NextValueId, Demanded,
                        Worklist);
    }
  }
}

bool dxcb5::RunOptMaskedDce(SsaProgram &Ssa) {
  std::vector<uint8_t> LiveLanes;
  ComputeDemandedLanes(Ssa, LiveLanes);
  bool Changed = false;
  for (auto &Block : Ssa.Blocks)
    Changed |= SweepDeadInsts(Block, LiveLanes);
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}
