// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
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
  return Inst.IsPure() ||
         (InstructionRequiresQuad(Inst.Opcode) &&
          Inst.Dest.Kind == OperandKind::Temp &&
          Inst.AuxDest.Kind == OperandKind::Null);
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
    if (!IsPseudoOpcode(Inst.Opcode) &&
        InstructionIsComponentwise(Inst.Opcode) &&
        Inst.Dest.Kind == OperandKind::Temp && PrimaryLive != Inst.Dest.Mask) {
      Inst.Dest.Mask = PrimaryLive;
      Changed = true;
    }
  }
  return Changed;
}

} // namespace

bool dxcb5::RunOptMaskedDce(SsaProgram &Ssa) {
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<uint8_t> LiveLanes(Ssa.NextValueId, 0);
  llvm::SmallVector<uint32_t, 256> Worklist;

  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead || IsRemovableByDce(Inst))
        continue;
      const uint8_t DMask = Inst.Dest.Kind == OperandKind::Null
                                ? Inst.AuxDest.Mask
                                : Inst.Dest.Mask;
      if (Inst.Dest.RelativeRegister != UINT32_MAX)
        MarkOperandLive(Inst.Dest, 0x1u, Ssa.NextValueId, LiveLanes, Worklist);
      for (size_t S = 0; S < Inst.Sources.size(); ++S)
        MarkOperandLive(Inst.Sources[S],
                        InstructionSourceSlotMask(Inst.Opcode, DMask, S),
                        Ssa.NextValueId, LiveLanes, Worklist);
    }
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
      MarkOperandLive(Block.Condition, 0x1u, Ssa.NextValueId, LiveLanes,
                      Worklist);
  }

  while (!Worklist.empty()) {
    const uint32_t Id = Worklist.pop_back_val();
    const auto &Def = Defs[Id];
    if (!Def.Valid())
      continue;
    const uint8_t Need = LiveLanes[Id];
    if (Def.IsPhi) {
      const auto &Phi = Ssa.Blocks[Def.Block].Phis[Def.Index];
      if (Phi.Dead)
        continue;
      const uint8_t Active = static_cast<uint8_t>(Phi.Mask & Need);
      for (const auto &In : Phi.Inputs)
        MarkOperandLive(In.Value, Active, Ssa.NextValueId, LiveLanes, Worklist);
      continue;
    }
    const auto &Inst = Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (Inst.Dead || !IsRemovableByDce(Inst))
      continue;
    if (Inst.Opcode == PseudoCombineVec) {
      for (unsigned L = 0; L < Inst.Sources.size(); ++L)
        if (Need & (1u << L))
          MarkOperandLive(Inst.Sources[L], 0x1u, Ssa.NextValueId, LiveLanes,
                          Worklist);
      continue;
    }
    const uint8_t DMask = InstructionIsComponentwise(Inst.Opcode)
                              ? static_cast<uint8_t>(Inst.Dest.Mask & Need)
                              : (Def.IsAux ? Inst.AuxDest.Mask : Inst.Dest.Mask);
    for (size_t S = 0; S < Inst.Sources.size(); ++S)
      MarkOperandLive(Inst.Sources[S],
                      InstructionSourceSlotMask(Inst.Opcode, DMask, S),
                      Ssa.NextValueId, LiveLanes, Worklist);
  }

  bool Changed = false;
  for (auto &Block : Ssa.Blocks)
    Changed |= SweepDeadInsts(Block, LiveLanes);
  return Changed;
}
