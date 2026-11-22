// Distributed under the University of Illinois Open Source License.
#include "Mir/SsaMir.h"
#include "llvm/ADT/DenseSet.h"
#include <algorithm>
#include <cassert>

namespace dxcb5 {

uint8_t InstructionSourceSlotMask(uint32_t Opcode, uint8_t DestMask,
                                  size_t SrcIndex) {
  return InstructionSourceSlotMaskSm5(Opcode, DestMask, SrcIndex);
}

bool SsaInst::IsPure() const {
  return !IsBarrier();
}

bool SsaInst::IsBarrier() const {
  const bool SrvOrUavBufLoad = Opcode == D3D11_SB_OPCODE_LD_RAW ||
                               Opcode == D3D11_SB_OPCODE_LD_STRUCTURED;
  if (!SrvOrUavBufLoad && !InstructionCanMoveAcrossDivergentBranch(Opcode))
    return true;
  if (Dest.Kind == OperandKind::Output ||
      Dest.Kind == OperandKind::OutputDepth ||
      Dest.Kind == OperandKind::OutputMask ||
      Dest.Kind == OperandKind::IndexableTemp ||
      Dest.Kind == OperandKind::Uav ||
      AuxDest.Kind == OperandKind::Uav)
    return true;
  for (const auto &Src : Sources)
    if (Src.Kind == OperandKind::IndexableTemp ||
        Src.Kind == OperandKind::Uav)
      return true;
  return false;
}

uint32_t SsaProgram::InternLiteralVector(const std::array<uint32_t, 4> &Bits) {
  const uint64_t Hash =
      (uint64_t(Bits[0]) * 0x9e3779b97f4a7c15ULL) ^
      (uint64_t(Bits[1]) * 0xbf58476d1ce4e5b9ULL) ^
      (uint64_t(Bits[2]) * 0x94d049bb133111ebULL) ^
      (uint64_t(Bits[3]) * 0x2545f4914f6cdd1dULL);
  auto &Bucket = LiteralBuckets[Hash];
  for (uint32_t Idx : Bucket) {
    if (Mir.LiteralVectors[Idx] == Bits)
      return Idx;
  }
  const auto Idx = static_cast<uint32_t>(Mir.LiteralVectors.size());
  Mir.LiteralVectors.push_back(Bits);
  Bucket.push_back(Idx);
  return Idx;
}

Operand NegateMirOperand(SsaProgram &Ssa, Operand Op) {
  if (Op.Kind == OperandKind::ImmediateVector &&
      Op.Index < Ssa.Mir.LiteralVectors.size()) {
    auto Bits = Ssa.Mir.LiteralVectors[Op.Index];
    for (uint32_t &Lane : Bits)
      Lane ^= 0x80000000u;
    Op.Index = Ssa.InternLiteralVector(Bits);
    Op.Modifier = 0;
    return Op;
  }
  return NegateMirOperand(Op);
}

Operand AbsMirOperand(SsaProgram &Ssa, Operand Op) {
  if (Op.Kind == OperandKind::ImmediateVector &&
      Op.Index < Ssa.Mir.LiteralVectors.size()) {
    auto Bits = Ssa.Mir.LiteralVectors[Op.Index];
    for (uint32_t &Lane : Bits)
      Lane &= 0x7fffffffu;
    Op.Index = Ssa.InternLiteralVector(Bits);
    Op.Modifier = 0;
    return Op;
  }
  return AbsMirOperand(Op);
}

void SsaProgram::BuildDefTable(std::vector<SsaDefSite> &Defs) const {
  Defs.assign(NextValueId, SsaDefSite{});
  for (uint32_t B = 0; B < Blocks.size(); ++B) {
    const auto &Block = Blocks[B];
    for (uint32_t I = 0; I < Block.Phis.size(); ++I) {
      const auto &Phi = Block.Phis[I];
      if (!Phi.Dead && Phi.Result)
        Defs[Phi.Result] = {B, I, true, false};
    }
    for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
      const auto &Inst = Block.Insts[I];
      if (Inst.Dead)
        continue;
      if (Inst.Dest.Kind == OperandKind::Temp && Inst.Dest.Index)
        Defs[Inst.Dest.Index] = {B, I, false, false};
      if (Inst.AuxDest.Kind == OperandKind::Temp && Inst.AuxDest.Index)
        Defs[Inst.AuxDest.Index] = {B, I, false, true};
    }
  }
}

void SsaProgram::BuildPredecessors(
    std::vector<llvm::SmallVector<uint32_t, 4>> &Preds) const {
  Preds.assign(Blocks.size(), {});
  for (uint32_t B = 0; B < Blocks.size(); ++B) {
    for (uint32_t Succ : Blocks[B].Successors)
      Preds[Succ].push_back(B);
  }
}

bool SsaDomTree::Dominates(uint32_t A, uint32_t B) const {
  if (!Reachable(A) || !Reachable(B))
    return false;
  if (A == B)
    return true;
  const uint32_t RpoA = RpoNumber[A];
  uint32_t Cur = B;
  while (Cur != UINT32_MAX && RpoNumber[Cur] > RpoA)
    Cur = IDom[Cur];
  return Cur == A;
}

void SsaProgram::ComputeDominators(SsaDomTree &Dom) const {
  const size_t N = Blocks.size();
  Dom.IDom.assign(N, UINT32_MAX);
  Dom.RpoNumber.assign(N, UINT32_MAX);
  Dom.RpoOrder.clear();
  Dom.Children.assign(N, {});
  if (N == 0)
    return;

  // Iterative DFS for postorder.
  std::vector<uint8_t> Visited(N, 0);
  std::vector<std::pair<uint32_t, size_t>> Stack;
  Stack.push_back({0, 0});
  Visited[0] = 1;
  while (!Stack.empty()) {
    auto &Top = Stack.back();
    const auto &Succs = Blocks[Top.first].Successors;
    if (Top.second >= Succs.size()) {
      Dom.RpoOrder.push_back(Top.first);
      Stack.pop_back();
      continue;
    }
    const uint32_t Next = Succs[Top.second++];
    if (!Visited[Next]) {
      Visited[Next] = 1;
      Stack.push_back({Next, 0});
    }
  }
  std::reverse(Dom.RpoOrder.begin(), Dom.RpoOrder.end());
  for (uint32_t Pos = 0; Pos < Dom.RpoOrder.size(); ++Pos)
    Dom.RpoNumber[Dom.RpoOrder[Pos]] = Pos;

  std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
  BuildPredecessors(Preds);

  auto Intersect = [&](uint32_t B1, uint32_t B2) {
    while (B1 != B2) {
      while (Dom.RpoNumber[B1] > Dom.RpoNumber[B2])
        B1 = Dom.IDom[B1];
      while (Dom.RpoNumber[B2] > Dom.RpoNumber[B1])
        B2 = Dom.IDom[B2];
    }
    return B1;
  };

  Dom.IDom[0] = 0;
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t I = 1; I < Dom.RpoOrder.size(); ++I) {
      const uint32_t B = Dom.RpoOrder[I];
      uint32_t NewIDom = UINT32_MAX;
      for (uint32_t P : Preds[B]) {
        if (Dom.IDom[P] == UINT32_MAX)
          continue;
        NewIDom = NewIDom == UINT32_MAX ? P : Intersect(P, NewIDom);
      }
      if (NewIDom != UINT32_MAX && Dom.IDom[B] != NewIDom) {
        Dom.IDom[B] = NewIDom;
        Changed = true;
      }
    }
  }

  for (uint32_t B : Dom.RpoOrder) {
    if (B != 0 && Dom.IDom[B] != UINT32_MAX)
      Dom.Children[Dom.IDom[B]].push_back(B);
  }
}

void SsaProgram::CompactDead() {
  for (auto &Block : Blocks) {
    Block.Phis.erase(
        std::remove_if(Block.Phis.begin(), Block.Phis.end(),
                       [](const SsaPhi &Phi) { return Phi.Dead; }),
        Block.Phis.end());
    Block.Insts.erase(
        std::remove_if(Block.Insts.begin(), Block.Insts.end(),
                       [](const SsaInst &Inst) { return Inst.Dead; }),
        Block.Insts.end());
  }
}

bool SsaProgram::Verify(std::string &Error, bool RequireNormalizedCfg) const {
  std::vector<SsaDefSite> Defs(NextValueId);
  std::vector<uint8_t> DefMasks(NextValueId, 0);

  auto RecordDef = [&](uint32_t Id, uint8_t Mask, SsaDefSite Site) -> bool {
    if (Id == 0 || Id >= NextValueId) {
      Error = "SSA value ID out of range";
      return false;
    }
    if (Defs[Id].Valid()) {
      Error = "duplicate SSA definition for value " + std::to_string(Id);
      return false;
    }
    if ((Mask & 0xfu) == 0) {
      Error = "empty write mask for SSA value " + std::to_string(Id);
      return false;
    }
    Defs[Id] = Site;
    DefMasks[Id] = Mask & 0xfu;
    return true;
  };

  for (uint32_t B = 0; B < Blocks.size(); ++B) {
    const auto &Block = Blocks[B];
    for (uint32_t I = 0; I < Block.Phis.size(); ++I) {
      const auto &Phi = Block.Phis[I];
      if (Phi.Dead)
        continue;
      if (!RecordDef(Phi.Result, Phi.Mask, {B, I, true, false}))
        return false;
    }
    for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
      const auto &Inst = Block.Insts[I];
      if (Inst.Dead)
        continue;
      if (Inst.Dest.Kind == OperandKind::Temp &&
          !RecordDef(Inst.Dest.Index, Inst.Dest.Mask, {B, I, false, false}))
        return false;
      if (Inst.AuxDest.Kind == OperandKind::Temp &&
          !RecordDef(Inst.AuxDest.Index, Inst.AuxDest.Mask, {B, I, false, true}))
        return false;
    }
  }

  SsaDomTree Dom;
  ComputeDominators(Dom);

  auto CheckIndexable = [&](const Operand &Op) -> bool {
    if (Op.Kind != OperandKind::IndexableTemp)
      return true;
    if (Op.Index >= Mir.IndexableTemps.size())
      return (Error = "indexable temp index out of range"), false;
    if (Op.RelativeRegister == UINT32_MAX &&
        Op.Element >= Mir.IndexableTemps[Op.Index].Elements)
      return (Error = "indexable temp element out of range"), false;
    return true;
  };

  auto CheckUse = [&](const Operand &Op, uint8_t SlotMask, uint32_t UseBlock,
                      uint32_t UseInstIndex, bool IsPhiEdge) -> bool {
    if (!CheckIndexable(Op))
      return false;
    if (Op.RelativeRegister != UINT32_MAX) {
      if (Op.RelativeRegister == 0 || Op.RelativeRegister >= NextValueId ||
          !Defs[Op.RelativeRegister].Valid()) {
        Error = "undefined relative register SSA value";
        return false;
      }
    }
    if ((Op.Kind == OperandKind::Immediate ||
         Op.Kind == OperandKind::ImmediateVector) &&
        Op.Modifier != 0) {
      Error = "immediate operand carries float modifier";
      return false;
    }
    if (Op.Kind != OperandKind::Temp)
      return true;
    if (Op.Index == 0 || Op.Index >= NextValueId || !Defs[Op.Index].Valid()) {
      Error = "use of undefined SSA value " + std::to_string(Op.Index);
      return false;
    }
    const auto &Def = Defs[Op.Index];
    if (Dom.Reachable(UseBlock)) {
      if (!Dom.Dominates(Def.Block, UseBlock)) {
        Error = "SSA definition does not dominate use for value " +
                std::to_string(Op.Index);
        return false;
      }
      if (!IsPhiEdge && Def.Block == UseBlock && !Def.IsPhi &&
          Def.Index >= UseInstIndex) {
        Error = "same-block SSA use precedes definition for value " +
                std::to_string(Op.Index);
        return false;
      }
    }
    const uint8_t ReadLanes = ReadLanesForMask(Op.Swizzle, SlotMask);
    if (ReadLanes & ~DefMasks[Op.Index]) {
      Error = "SSA use reads undefined lane of value " +
              std::to_string(Op.Index);
      return false;
    }
    return true;
  };

  for (uint32_t B = 0; B < Blocks.size(); ++B) {
    const auto &Block = Blocks[B];
    for (const auto &Phi : Block.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs) {
        if (In.PredBlock >= Blocks.size())
          return (Error = "PHI predecessor out of range"), false;
        if (!CheckUse(In.Value, Phi.Mask, In.PredBlock, UINT32_MAX, true))
          return false;
      }
    }
    for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
      const auto &Inst = Block.Insts[I];
      if (Inst.Dead)
        continue;
      const uint8_t DMask = Inst.Dest.Kind == OperandKind::Null
                                ? Inst.AuxDest.Mask
                                : Inst.Dest.Mask;
      if (!CheckIndexable(Inst.Dest) || !CheckIndexable(Inst.AuxDest))
        return false;
      if (Inst.Dest.RelativeRegister != UINT32_MAX &&
          !CheckUse(Inst.Dest, 0x1u, B, I, false))
        return false;
      for (size_t S = 0; S < Inst.Sources.size(); ++S) {
        const uint8_t SMask = InstructionSourceSlotMask(Inst.Opcode, DMask, S);
        if (!CheckUse(Inst.Sources[S], SMask, B, I, false))
          return false;
      }
    }
    if ((Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch) &&
        !CheckUse(Block.Condition, 0x1u, B, UINT32_MAX, false))
      return false;
  }
  if (RequireNormalizedCfg) {
    std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
    BuildPredecessors(Preds);
    for (uint32_t B = 0; B < Blocks.size(); ++B) {
      if (!Dom.Reachable(B))
        return (Error = "unreachable block in normalized SSA CFG"), false;
      const auto &Block = Blocks[B];
      if ((Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch) &&
          Block.Condition.Kind == OperandKind::Immediate &&
          Block.Condition.Modifier == 0 &&
          Block.Condition.RelativeRegister == UINT32_MAX)
        return (Error = "unfolded constant branch/switch in normalized SSA CFG"),
               false;
      if (Block.Kind == FlowKind::Jump && Block.Successors.size() == 1) {
        const uint32_t Succ = Block.Successors[0];
        if (Succ != 0 && Succ != B && Succ < Blocks.size() &&
            Preds[Succ].size() == 1 && Preds[Succ][0] == B) {
          bool HasLivePhi = false;
          for (const auto &Phi : Blocks[Succ].Phis)
            if (!Phi.Dead)
              HasLivePhi = true;
          if (!HasLivePhi)
            return (Error = "unmerged trivial jump in normalized SSA CFG"),
                   false;
        }
      }
    }
  }
  return true;
}

} // namespace dxcb5
