// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "Mir/PlacementStore.h"
#include "Mir/SsaAnalysis.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <vector>

using namespace dxcb5;

namespace {

bool IsLoopInvariantInst(const SsaInst &Inst,
                         const std::vector<SsaDefSite> &Defs,
                         const NaturalLoop &Loop) {
  if (Inst.Dead || Inst.HasAuxDest() || Inst.Dest.Kind != OperandKind::Temp ||
      !IsMovablePureOrQuadInst(Inst))
    return false;
  for (const auto &Src : Inst.Sources) {
    if (Src.Kind == OperandKind::Temp && Src.Index < Defs.size()) {
      const auto &D = Defs[Src.Index];
      if (D.Valid() && Loop.InLoop[D.Block])
        return false;
    }
    if (Src.RelativeRegister != UINT32_MAX &&
        Src.RelativeRegister < Defs.size()) {
      const auto &D = Defs[Src.RelativeRegister];
      if (D.Valid() && Loop.InLoop[D.Block])
        return false;
    }
  }
  return true;
}

bool HoistLoopHeader(SsaProgram &Ssa, const NaturalLoop &Loop,
                     const std::vector<uint32_t> &LoopDepth,
                     std::vector<SsaDefSite> &Defs, PlacementStore &Placement) {
  auto &HeaderBlk = Ssa.Blocks[Loop.Header];
  auto &PreBlk = Ssa.Blocks[Loop.Preheader];
  bool AnyChanged = false;
  bool LoopChanged = true;
  while (LoopChanged) {
    LoopChanged = false;
    for (size_t I = 0; I < HeaderBlk.Insts.size(); ++I) {
      auto &Inst = HeaderBlk.Insts[I];
      if (!Inst.Dead && Inst.Opcode == D3D10_SB_OPCODE_DISCARD)
        break;
      if (!IsLoopInvariantInst(Inst, Defs, Loop))
        continue;
      const uint32_t ValId = Inst.Dest.Index;
      PlacementMove Move;
      Move.ValId = ValId;
      Move.Owner = PlacementOwner::Licm;
      Move.SrcDepth = LoopDepth[Loop.Header];
      Move.DstDepths.push_back(LoopDepth[Loop.Preheader]);
      if (!Placement.TryAccept(Move))
        continue;
      const uint32_t NewIdx = static_cast<uint32_t>(PreBlk.Insts.size());
      PreBlk.Insts.push_back(Inst);
      Inst.Dead = true;
      if (ValId < Defs.size()) {
        auto &Slot = Defs[ValId];
        Slot.Block = Loop.Preheader;
        Slot.Index = NewIdx;
        Slot.IsPhi = false;
        Slot.IsAux = false;
      }
      LoopChanged = true;
      AnyChanged = true;
    }
  }
  return AnyChanged;
}

bool IsUncondLoopBlock(const SsaDomTree &Dom, const NaturalLoop &Loop,
                       uint32_t B) {
  if (B == Loop.Header || !Dom.Dominates(Loop.Header, B))
    return false;
  for (uint32_t Latch : Loop.Latches)
    if (!Dom.Dominates(B, Latch))
      return false;
  return true;
}

bool IsInvariantInSet(const SsaInst &Inst, const std::vector<SsaDefSite> &Defs,
                      const NaturalLoop &Loop,
                      const std::vector<uint8_t> &InvVal) {
  if (Inst.Dead || Inst.HasAuxDest() || Inst.Dest.Kind != OperandKind::Temp ||
      !Inst.IsPure())
    return false;
  for (const auto &Src : Inst.Sources) {
    if (Src.Kind == OperandKind::Temp && Src.Index < Defs.size()) {
      const auto &D = Defs[Src.Index];
      if (D.Valid() && Loop.InLoop[D.Block] && !InvVal[Src.Index])
        return false;
    }
    if (Src.RelativeRegister != UINT32_MAX &&
        Src.RelativeRegister < Defs.size()) {
      const auto &D = Defs[Src.RelativeRegister];
      if (D.Valid() && Loop.InLoop[D.Block] && !InvVal[Src.RelativeRegister])
        return false;
    }
  }
  return true;
}

bool HoistLoopBodyScalarChains(SsaProgram &Ssa, const SsaDomTree &Dom,
                               const NaturalLoop &Loop,
                               const std::vector<uint32_t> &LoopDepth,
                               std::vector<SsaDefSite> &Defs,
                               PlacementStore &Placement) {
  llvm::SmallVector<uint32_t, 8> UncondBlks;
  for (uint32_t B : Dom.RpoOrder)
    if (Loop.InLoop[B] && IsUncondLoopBlock(Dom, Loop, B))
      UncondBlks.push_back(B);
  if (UncondBlks.empty())
    return false;

  std::vector<uint8_t> InvVal(Ssa.NextValueId, 0);
  llvm::SmallVector<std::pair<uint32_t, uint32_t>, 16> InvOrder;
  bool Found = true;
  while (Found) {
    Found = false;
    for (uint32_t B : UncondBlks) {
      const auto &Blk = Ssa.Blocks[B];
      for (uint32_t I = 0; I < Blk.Insts.size(); ++I) {
        const auto &Inst = Blk.Insts[I];
        if (Inst.Dead || Inst.Dest.Kind != OperandKind::Temp ||
            Inst.Dest.Index >= InvVal.size() || InvVal[Inst.Dest.Index])
          continue;
        if (!IsInvariantInSet(Inst, Defs, Loop, InvVal))
          continue;
        InvVal[Inst.Dest.Index] = 1;
        InvOrder.emplace_back(B, I);
        Found = true;
      }
    }
  }
  if (InvOrder.empty())
    return false;

  // Only hoist subgraphs whose live-out root is narrow (<= 2 lanes) and
  // collapses >= 2 instructions (e.g. `any(r9 != 0)` -> 1 scalar bool).
  std::vector<uint8_t> UsedOutside(Ssa.NextValueId, 0);
  auto MarkIfInv = [&](const Operand &Op) {
    if (Op.Kind == OperandKind::Temp && Op.Index < InvVal.size() &&
        InvVal[Op.Index])
      UsedOutside[Op.Index] = 1;
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < InvVal.size() && InvVal[Op.RelativeRegister])
      UsedOutside[Op.RelativeRegister] = 1;
  };
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    const auto &Blk = Ssa.Blocks[B];
    for (const auto &Phi : Blk.Phis)
      if (!Phi.Dead)
        for (const auto &In : Phi.Inputs)
          MarkIfInv(In.Value);
    for (const auto &Inst : Blk.Insts) {
      if (Inst.Dead)
        continue;
      const bool IsInv = Inst.Dest.Kind == OperandKind::Temp &&
                         Inst.Dest.Index < InvVal.size() &&
                         InvVal[Inst.Dest.Index];
      if (IsInv)
        continue;
      for (const auto &Src : Inst.Sources)
        MarkIfInv(Src);
    }
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
      MarkIfInv(Blk.Condition);
  }

  std::vector<uint8_t> HoistVal(Ssa.NextValueId, 0);
  for (const auto &Loc : InvOrder) {
    const auto &Root = Ssa.Blocks[Loc.first].Insts[Loc.second];
    const uint32_t RootId = Root.Dest.Index;
    if (!UsedOutside[RootId] || PopcountMask(Root.Dest.Mask) > 2)
      continue;
    llvm::SmallVector<uint32_t, 8> Closure;
    llvm::SmallVector<uint32_t, 8> Work;
    Work.push_back(RootId);
    bool Valid = true;
    while (!Work.empty()) {
      const uint32_t V = Work.pop_back_val();
      if (std::find(Closure.begin(), Closure.end(), V) != Closure.end())
        continue;
      if (V != RootId && UsedOutside[V] && !HoistVal[V]) {
        Valid = false;
        break;
      }
      const auto &D = Defs[V];
      const PlacementOwner CurOwner = Placement.OwnerOf(V);
      if ((CurOwner != PlacementOwner::None &&
           CurOwner != PlacementOwner::Licm) ||
          LoopDepth[Loop.Preheader] >= LoopDepth[D.Block]) {
        Valid = false;
        break;
      }
      Closure.push_back(V);
      const auto &I = Ssa.Blocks[D.Block].Insts[D.Index];
      for (const auto &Src : I.Sources) {
        if (Src.Kind == OperandKind::Temp && Src.Index < InvVal.size() &&
            InvVal[Src.Index])
          Work.push_back(Src.Index);
        if (Src.RelativeRegister != UINT32_MAX &&
            Src.RelativeRegister < InvVal.size() && InvVal[Src.RelativeRegister])
          Work.push_back(Src.RelativeRegister);
      }
    }
    if (Valid && Closure.size() >= 2)
      for (uint32_t V : Closure)
        HoistVal[V] = 1;
  }

  auto &PreBlk = Ssa.Blocks[Loop.Preheader];
  std::vector<uint8_t> Hoisted(Ssa.NextValueId, 0);
  bool Changed = false;
  for (const auto &Loc : InvOrder) {
    auto &Inst = Ssa.Blocks[Loc.first].Insts[Loc.second];
    if (Inst.Dead || !HoistVal[Inst.Dest.Index])
      continue;
    bool DepsReady = true;
    for (const auto &Src : Inst.Sources) {
      if (Src.Kind == OperandKind::Temp && Src.Index < InvVal.size() &&
          InvVal[Src.Index] && !Hoisted[Src.Index])
        DepsReady = false;
      if (Src.RelativeRegister != UINT32_MAX &&
          Src.RelativeRegister < InvVal.size() &&
          InvVal[Src.RelativeRegister] && !Hoisted[Src.RelativeRegister])
        DepsReady = false;
    }
    if (!DepsReady)
      continue;
    const uint32_t ValId = Inst.Dest.Index;
    PlacementMove Move;
    Move.ValId = ValId;
    Move.Owner = PlacementOwner::Licm;
    Move.SrcDepth = LoopDepth[Loc.first];
    Move.DstDepths.push_back(LoopDepth[Loop.Preheader]);
    if (!Placement.TryAccept(Move))
      continue;
    const uint32_t NewIdx = static_cast<uint32_t>(PreBlk.Insts.size());
    PreBlk.Insts.push_back(Inst);
    Inst.Dead = true;
    Defs[ValId] = {Loop.Preheader, NewIdx, false, false};
    Hoisted[ValId] = 1;
    Changed = true;
  }
  return Changed;
}

} // namespace

bool dxcb5::RunOptLicm(SsaProgram &Ssa, const SsaDomTree &Dom,
                       const SsaLoopInfo *CachedLoops,
                       PlacementStore *Placement) {
  llvm::TimeTraceScope Scope("DXCB5 Opt LICM", llvm::StringRef());
  if (Ssa.Blocks.size() < 2)
    return false;
  SsaLoopInfo LocalLoops;
  if (!CachedLoops) {
    ComputeLoopInfo(Ssa, Dom, LocalLoops);
    CachedLoops = &LocalLoops;
  }
  if (CachedLoops->Loops.empty())
    return false;
  PlacementStore LocalPlacement;
  PlacementStore &Store = Placement ? *Placement : LocalPlacement;
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  bool Changed = false;
  for (const auto &Loop : CachedLoops->Loops) {
    if (Loop.Preheader != UINT32_MAX) {
      Changed |= HoistLoopHeader(Ssa, Loop, CachedLoops->LoopDepth, Defs, Store);
      Changed |= HoistLoopBodyScalarChains(Ssa, Dom, Loop, CachedLoops->LoopDepth,
                                           Defs, Store);
    }
  }
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}
