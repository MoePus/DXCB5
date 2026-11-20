// Distributed under the University of Illinois Open Source License.
#include "Mir/OptIndexableSsa.h"
#include <algorithm>

using namespace dxcb5;
using namespace dxcb5::idxssa;

namespace {

uint32_t DomLCA(const SsaDomTree &Dom, uint32_t A, uint32_t B) {
  while (A != B) {
    while (Dom.RpoNumber[A] > Dom.RpoNumber[B])
      A = Dom.IDom[A];
    while (Dom.RpoNumber[B] > Dom.RpoNumber[A])
      B = Dom.IDom[B];
  }
  return A;
}

uint32_t FindFirstDynLoad(const std::vector<SsaInst> &Insts, uint32_t X) {
  for (uint32_t I = 0; I < Insts.size(); ++I) {
    if (Insts[I].Dead)
      continue;
    for (const auto &Src : Insts[I].Sources)
      if (Src.Kind == OperandKind::IndexableTemp && Src.Index == X &&
          Src.RelativeRegister != UINT32_MAX)
        return I;
  }
  return static_cast<uint32_t>(Insts.size());
}

void ScanInstAccesses(const SsaProgram &Ssa, const SsaDomTree &Dom, uint32_t B,
                      const SsaInst &Inst, std::vector<ArrayStats> &Stats) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  for (const auto &Src : Inst.Sources) {
    if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX)
      continue;
    auto &St = Stats[Src.Index];
    if (Src.Modifier != 0) {
      St.BadAccess = true;
      continue;
    }
    if (Src.RelativeRegister == UINT32_MAX) {
      ++St.ConstLoads;
      continue;
    }
    ++St.DynLoads;
    St.DynLoadLca =
        St.DynLoadLca == UINT32_MAX ? B : DomLCA(Dom, St.DynLoadLca, B);
    if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate ||
        Inst.Sources.size() != 1 || Inst.Dest.Kind != OperandKind::Temp ||
        PopcountMask(Inst.Dest.Mask) != 1)
      St.BadAccess = true;
  }
  if (Inst.Dest.Kind != OperandKind::IndexableTemp || Inst.Dest.Index >= NumX)
    return;
  auto &St = Stats[Inst.Dest.Index];
  if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate ||
      Inst.Sources.size() != 1 || Inst.Dest.Mask != 1 ||
      !IsSsaSlotValue(Inst.Sources[0])) {
    St.BadAccess = true;
    return;
  }
  if (Inst.Dest.RelativeRegister != UINT32_MAX)
    ++St.DynStores;
  else if (Inst.Dest.Element >= Ssa.Mir.IndexableTemps[Inst.Dest.Index].Elements)
    St.BadAccess = true;
  else
    ++St.ConstStores;
}

void AnalyzeArrays(const SsaProgram &Ssa, const SsaDomTree &Dom,
                   std::vector<ArrayStats> &Stats) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  Stats.assign(NumX, {});
  for (uint32_t B : Dom.RpoOrder) {
    const auto &Block = Ssa.Blocks[B];
    for (const auto &Phi : Block.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        if (In.Value.Kind == OperandKind::IndexableTemp && In.Value.Index < NumX)
          Stats[In.Value.Index].BadAccess = true;
    }
    for (const auto &Inst : Block.Insts)
      if (!Inst.Dead)
        ScanInstAccesses(Ssa, Dom, B, Inst, Stats);
  }
  for (uint32_t X = 0; X < NumX; ++X) {
    if (Stats[X].DynLoadLca != UINT32_MAX)
      Stats[X].FirstDynLoadIdx =
          FindFirstDynLoad(Ssa.Blocks[Stats[X].DynLoadLca].Insts, X);
  }
}

void ComputeReachableFromSuccs(const SsaProgram &Ssa, const SsaDomTree &Dom,
                               uint32_t StartBlock,
                               std::vector<uint8_t> &Reach) {
  const size_t NumBlocks = Ssa.Blocks.size();
  Reach.assign(NumBlocks, 0);
  llvm::SmallVector<uint32_t, 32> Work;
  for (uint32_t S : Ssa.Blocks[StartBlock].Successors) {
    if (S < NumBlocks && Dom.Reachable(S) && !Reach[S]) {
      Reach[S] = 1;
      Work.push_back(S);
    }
  }
  while (!Work.empty()) {
    const uint32_t Cur = Work.pop_back_val();
    for (uint32_t S : Ssa.Blocks[Cur].Successors) {
      if (S < NumBlocks && Dom.Reachable(S) && !Reach[S]) {
        Reach[S] = 1;
        Work.push_back(S);
      }
    }
  }
}

bool TryBuildDomSpill(const SsaProgram &Ssa, const SsaDomTree &Dom, uint32_t X,
                      const ArrayStats &St, SpillPlan &Out) {
  if (St.DynLoadLca == UINT32_MAX)
    return false;
  uint32_t SpillB = St.DynLoadLca;
  uint32_t SpillIdx = St.FirstDynLoadIdx;
  std::vector<uint8_t> Reach;
  while (true) {
    ComputeReachableFromSuccs(Ssa, Dom, SpillB, Reach);
    if (!Reach[SpillB])
      break;
    if (SpillB == 0)
      return false;
    SpillB = Dom.IDom[SpillB];
    SpillIdx = static_cast<uint32_t>(Ssa.Blocks[SpillB].Insts.size());
  }

  uint32_t StoresBefore = 0;
  for (uint32_t B : Dom.RpoOrder) {
    if (Reach[B])
      continue;
    const auto &Insts = Ssa.Blocks[B].Insts;
    const uint32_t Lim =
        (B == SpillB) ? SpillIdx : static_cast<uint32_t>(Insts.size());
    for (uint32_t I = 0; I < Lim; ++I)
      if (!Insts[I].Dead && Insts[I].Dest.Kind == OperandKind::IndexableTemp &&
          Insts[I].Dest.Index == X)
        ++StoresBefore;
  }
  const uint32_t N = Ssa.Mir.IndexableTemps[X].Elements;
  if (StoresBefore <= N)
    return false;
  Out.Block = SpillB;
  Out.InstIdx = SpillIdx;
  Out.AfterSpill = std::move(Reach);
  return true;
}

PromoteMode ChooseMode(const SsaProgram &Ssa, const SsaDomTree &Dom, uint32_t X,
                       const ArrayStats &St, SpillPlan &Plan) {
  const auto &Decl = Ssa.Mir.IndexableTemps[X];
  if (St.BadAccess || Decl.Components != 1 || Decl.Elements == 0)
    return PromoteMode::None;
  if (St.ConstStores == 0 && St.DynStores == 0 && St.ConstLoads == 0 &&
      St.DynLoads == 0)
    return PromoteMode::None;
  const uint32_t N = Decl.Elements;
  if (St.DynStores > 0) {
    if (N <= 2 && St.DynStores <= 4 && St.DynLoads == 0)
      return PromoteMode::Full;
    return PromoteMode::None;
  }
  if (St.DynLoads == 0)
    return N <= kMaxConstPromoteElems ? PromoteMode::Full : PromoteMode::None;
  if (N <= 3 && St.DynLoads <= 4)
    return PromoteMode::Full;
  if (N == 4 && (St.DynLoads <= 2 || St.ConstStores > 4 * St.DynLoads))
    return PromoteMode::Full;
  if (N <= kMaxConstPromoteElems && TryBuildDomSpill(Ssa, Dom, X, St, Plan))
    return PromoteMode::DomSpill;
  if (N <= kMaxConstPromoteElems && St.ConstLoads > 0)
    return PromoteMode::ConstLoads;
  return PromoteMode::None;
}

} // namespace

void dxcb5::idxssa::BuildPromotionPlan(const SsaProgram &Ssa,
                                       const SsaDomTree &Dom,
                                       PromotionPlan &Plan) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  std::vector<ArrayStats> Stats;
  AnalyzeArrays(Ssa, Dom, Stats);

  Plan.Modes.assign(NumX, PromoteMode::None);
  Plan.Spills.assign(NumX, {});
  Plan.SlotBase.assign(NumX, UINT32_MAX);
  Plan.SpillsAtBlock.assign(Ssa.Blocks.size(), {});
  Plan.TotalSlots = 0;

  for (uint32_t X = 0; X < NumX; ++X) {
    const PromoteMode M = ChooseMode(Ssa, Dom, X, Stats[X], Plan.Spills[X]);
    const uint32_t N = Ssa.Mir.IndexableTemps[X].Elements;
    if (M == PromoteMode::None || N > kMaxSsaTrackedSlots - Plan.TotalSlots)
      continue;
    Plan.Modes[X] = M;
    Plan.SlotBase[X] = Plan.TotalSlots;
    Plan.TotalSlots += N;
    if (M == PromoteMode::DomSpill)
      Plan.SpillsAtBlock[Plan.Spills[X].Block].push_back(
          {X, Plan.Spills[X].InstIdx});
  }
}
