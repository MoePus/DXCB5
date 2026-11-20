// Distributed under the University of Illinois Open Source License.
#include "Mir/OptIndexableSsa.h"
#include "Mir/OptPipeline.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <vector>

using namespace dxcb5;
using namespace dxcb5::idxssa;

namespace {

void ComputeDominanceFrontiers(
    const SsaDomTree &Dom,
    const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds,
    std::vector<llvm::SmallVector<uint32_t, 4>> &DF) {
  DF.assign(Dom.IDom.size(), {});
  for (uint32_t B : Dom.RpoOrder) {
    if (Preds[B].size() < 2)
      continue;
    for (uint32_t P : Preds[B]) {
      if (!Dom.Reachable(P))
        continue;
      uint32_t Runner = P;
      while (Runner != UINT32_MAX && Runner != Dom.IDom[B]) {
        auto &List = DF[Runner];
        if (std::find(List.begin(), List.end(), B) == List.end())
          List.push_back(B);
        if (Runner == 0)
          break;
        Runner = Dom.IDom[Runner];
      }
    }
  }
}

void SetRangeBits(std::vector<uint64_t> &Live, uint32_t Base, uint32_t N) {
  for (uint32_t E = 0; E < N; ++E) {
    const uint32_t S = Base + E;
    Live[S >> 6] |= (uint64_t{1} << (S & 63u));
  }
}

void TransferInstLiveness(const SsaProgram &Ssa, const SsaInst &Inst,
                          const PromotionPlan &Plan,
                          std::vector<uint64_t> &Live) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  if (Inst.Dest.Kind == OperandKind::IndexableTemp && Inst.Dest.Index < NumX &&
      Plan.SlotBase[Inst.Dest.Index] != UINT32_MAX) {
    const uint32_t X = Inst.Dest.Index;
    const uint32_t Base = Plan.SlotBase[X];
    if (Inst.Dest.RelativeRegister == UINT32_MAX) {
      const uint32_t S = Base + Inst.Dest.Element;
      Live[S >> 6] &= ~(uint64_t{1} << (S & 63u));
    } else if (Plan.Modes[X] == PromoteMode::Full) {
      SetRangeBits(Live, Base, Ssa.Mir.IndexableTemps[X].Elements);
    }
  }
  for (const auto &Src : Inst.Sources) {
    if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX ||
        Plan.SlotBase[Src.Index] == UINT32_MAX)
      continue;
    const uint32_t X = Src.Index;
    const uint32_t Base = Plan.SlotBase[X];
    if (Src.RelativeRegister == UINT32_MAX) {
      const uint32_t S = Base + Src.Element;
      Live[S >> 6] |= (uint64_t{1} << (S & 63u));
    } else if (Plan.Modes[X] == PromoteMode::Full) {
      SetRangeBits(Live, Base, Ssa.Mir.IndexableTemps[X].Elements);
    }
  }
}

bool UpdateBlockSlotLiveness(const SsaProgram &Ssa, uint32_t B,
                             const SsaDomTree &Dom, const PromotionPlan &Plan,
                             std::vector<std::vector<uint64_t>> &LiveIn,
                             std::vector<uint64_t> &Live) {
  std::fill(Live.begin(), Live.end(), uint64_t{0});
  for (uint32_t Succ : Ssa.Blocks[B].Successors) {
    if (Succ >= LiveIn.size() || !Dom.Reachable(Succ))
      continue;
    for (size_t W = 0; W < Live.size(); ++W)
      Live[W] |= LiveIn[Succ][W];
  }
  const auto &Insts = Ssa.Blocks[B].Insts;
  for (const auto &Sp : Plan.SpillsAtBlock[B])
    if (Sp.second == Insts.size())
      SetRangeBits(Live, Plan.SlotBase[Sp.first],
                   Ssa.Mir.IndexableTemps[Sp.first].Elements);

  for (size_t I = Insts.size(); I-- > 0;) {
    if (!Insts[I].Dead)
      TransferInstLiveness(Ssa, Insts[I], Plan, Live);
    for (const auto &Sp : Plan.SpillsAtBlock[B])
      if (Sp.second == I)
        SetRangeBits(Live, Plan.SlotBase[Sp.first],
                     Ssa.Mir.IndexableTemps[Sp.first].Elements);
  }
  if (Live == LiveIn[B])
    return false;
  LiveIn[B] = Live;
  return true;
}

void ComputeSlotLiveness(const SsaProgram &Ssa, const SsaDomTree &Dom,
                         const PromotionPlan &Plan,
                         std::vector<std::vector<uint64_t>> &LiveIn) {
  const size_t NumBlocks = Ssa.Blocks.size();
  const size_t Words = (Plan.TotalSlots + 63u) / 64u;
  LiveIn.assign(NumBlocks, std::vector<uint64_t>(Words, 0));
  std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
  Ssa.BuildPredecessors(Preds);
  std::vector<uint32_t> Worklist(Dom.RpoOrder.begin(), Dom.RpoOrder.end());
  std::vector<uint8_t> InWorklist(NumBlocks, 0);
  for (uint32_t B : Worklist)
    InWorklist[B] = 1;
  std::vector<uint64_t> Live(Words, 0);
  const size_t MaxSteps = std::max<size_t>(4096u, NumBlocks * 64u);
  size_t Steps = 0;
  while (!Worklist.empty() && Steps < MaxSteps) {
    const uint32_t B = Worklist.back();
    Worklist.pop_back();
    InWorklist[B] = 0;
    ++Steps;
    if (!UpdateBlockSlotLiveness(Ssa, B, Dom, Plan, LiveIn, Live))
      continue;
    for (uint32_t P : Preds[B]) {
      if (P < NumBlocks && Dom.Reachable(P) && !InWorklist[P]) {
        InWorklist[P] = 1;
        Worklist.push_back(P);
      }
    }
  }
  if (!Worklist.empty()) {
    for (uint32_t B : Dom.RpoOrder)
      std::fill(LiveIn[B].begin(), LiveIn[B].end(), ~uint64_t{0});
  }
}

struct PlacedPhi {
  uint32_t Slot = 0;
  uint32_t PhiIndex = 0;
};

void RecordSlotDef(std::vector<llvm::SmallVector<uint32_t, 4>> &DefBlocks,
                   uint32_t Slot, uint32_t B) {
  auto &DB = DefBlocks[Slot];
  if (DB.empty() || DB.back() != B)
    DB.push_back(B);
}

void CollectSlotDefBlocks(
    const SsaProgram &Ssa, const SsaDomTree &Dom, const PromotionPlan &Plan,
    std::vector<llvm::SmallVector<uint32_t, 4>> &DefBlocks) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  for (uint32_t B : Dom.RpoOrder) {
    for (const auto &Inst : Ssa.Blocks[B].Insts) {
      if (Inst.Dead || Inst.Dest.Kind != OperandKind::IndexableTemp ||
          Inst.Dest.Index >= NumX || Plan.SlotBase[Inst.Dest.Index] == UINT32_MAX)
        continue;
      const uint32_t X = Inst.Dest.Index;
      const uint32_t Base = Plan.SlotBase[X];
      if (Inst.Dest.RelativeRegister == UINT32_MAX) {
        RecordSlotDef(DefBlocks, Base + Inst.Dest.Element, B);
      } else if (Plan.Modes[X] == PromoteMode::Full) {
        for (uint32_t E = 0; E < Ssa.Mir.IndexableTemps[X].Elements; ++E)
          RecordSlotDef(DefBlocks, Base + E, B);
      }
    }
  }
}

void PlacePhisForSlot(SsaProgram &Ssa, uint32_t Slot,
                      const llvm::SmallVectorImpl<uint32_t> &SlotDefBlocks,
                      const std::vector<llvm::SmallVector<uint32_t, 4>> &DF,
                      const std::vector<std::vector<uint64_t>> &LiveIn,
                      std::vector<uint32_t> &HasPhi,
                      std::vector<uint32_t> &InWork,
                      llvm::SmallVectorImpl<uint32_t> &Work,
                      std::vector<llvm::SmallVector<PlacedPhi, 4>> &BlockPhis) {
  Work.clear();
  for (uint32_t DB : SlotDefBlocks) {
    InWork[DB] = Slot;
    Work.push_back(DB);
  }
  while (!Work.empty()) {
    const uint32_t B = Work.pop_back_val();
    for (uint32_t F : DF[B]) {
      if (HasPhi[F] == Slot)
        continue;
      if ((LiveIn[F][Slot >> 6] & (uint64_t{1} << (Slot & 63u))) == 0)
        continue;
      HasPhi[F] = Slot;
      SsaPhi Phi;
      Phi.Result = Ssa.AllocValue();
      Phi.Mask = 1;
      const uint32_t Idx = static_cast<uint32_t>(Ssa.Blocks[F].Phis.size());
      Ssa.Blocks[F].Phis.push_back(std::move(Phi));
      BlockPhis[F].push_back({Slot, Idx});
      if (InWork[F] != Slot) {
        InWork[F] = Slot;
        Work.push_back(F);
      }
    }
  }
}

void PlaceSlotPhis(SsaProgram &Ssa, const SsaDomTree &Dom,
                   const PromotionPlan &Plan,
                   const std::vector<llvm::SmallVector<uint32_t, 4>> &DF,
                   const std::vector<std::vector<uint64_t>> &LiveIn,
                   std::vector<llvm::SmallVector<PlacedPhi, 4>> &BlockPhis) {
  std::vector<llvm::SmallVector<uint32_t, 4>> DefBlocks(Plan.TotalSlots);
  CollectSlotDefBlocks(Ssa, Dom, Plan, DefBlocks);

  BlockPhis.assign(Ssa.Blocks.size(), {});
  std::vector<uint32_t> HasPhi(Ssa.Blocks.size(), UINT32_MAX);
  std::vector<uint32_t> InWork(Ssa.Blocks.size(), UINT32_MAX);
  llvm::SmallVector<uint32_t, 32> Work;

  for (uint32_t Slot = 0; Slot < Plan.TotalSlots; ++Slot) {
    if (!DefBlocks[Slot].empty())
      PlacePhisForSlot(Ssa, Slot, DefBlocks[Slot], DF, LiveIn, HasPhi, InWork,
                       Work, BlockPhis);
  }
}

struct RenameState {
  SsaProgram &Ssa;
  const SsaDomTree &Dom;
  const PromotionPlan &Plan;
  const std::vector<llvm::SmallVector<PlacedPhi, 4>> &BlockPhis;
  std::vector<llvm::SmallVector<Operand, 8>> DefStack;
  llvm::SmallVector<uint32_t, 64> PushLog;

  void PushDef(uint32_t Slot, Operand Val) {
    DefStack[Slot].push_back(Val);
    PushLog.push_back(Slot);
  }
  void PopTo(size_t Mark) {
    while (PushLog.size() > Mark)
      DefStack[PushLog.pop_back_val()].pop_back();
  }
  void EmitDynamicRead(const SsaInst &Inst, uint32_t Base, uint32_t N,
                       std::vector<SsaInst> &Out);
  void EmitSpillStores(uint32_t X, std::vector<SsaInst> &Out);
  void EmitDynamicWrite(const SsaInst &Inst, uint32_t Base, uint32_t N,
                        std::vector<SsaInst> &Out);
  bool BlockNeedsRebuild(uint32_t B) const;
  bool RenameBlock(uint32_t B);
};

SsaInst MakeUnarySsa(uint32_t Op, Operand Dst, Operand Src) {
  SsaInst I;
  I.Opcode = Op;
  I.Dest = Dst;
  I.Sources.push_back(Src);
  return I;
}

SsaInst MakeBinarySsa(uint32_t Op, Operand Dst, Operand A, Operand B) {
  SsaInst I;
  I.Opcode = Op;
  I.Dest = Dst;
  I.Sources.push_back(A);
  I.Sources.push_back(B);
  return I;
}

SsaInst MakeMovcSsa(Operand Dst, Operand C, Operand T, Operand F) {
  SsaInst I;
  I.Opcode = D3D10_SB_OPCODE_MOVC;
  I.Dest = Dst;
  I.Sources.push_back(C);
  I.Sources.push_back(T);
  I.Sources.push_back(F);
  return I;
}

void RenameState::EmitDynamicRead(const SsaInst &Inst, uint32_t Base,
                                  uint32_t N, std::vector<SsaInst> &Out) {
  const Operand &Src = Inst.Sources[0];
  Operand Sel = MakeSsaLane(Src.RelativeRegister, Src.RelativeLane);
  Operand Acc = DefStack[Base].back();
  if (N == 1) {
    Out.push_back(MakeUnarySsa(D3D10_SB_OPCODE_MOV, Inst.Dest, Acc));
    return;
  }
  for (uint32_t E = 1; E < N; ++E) {
    const Operand Match = MakeSsaTemp(Ssa.AllocValue(), 1, 0);
    Out.push_back(MakeBinarySsa(D3D10_SB_OPCODE_IEQ, Match, Sel, MakeImm(E)));
    const Operand Dst =
        (E + 1 == N) ? Inst.Dest : MakeSsaTemp(Ssa.AllocValue(), 1, 0);
    Out.push_back(MakeMovcSsa(Dst, Match, DefStack[Base + E].back(), Acc));
    Acc = Dst;
  }
}

void RenameState::EmitSpillStores(uint32_t X, std::vector<SsaInst> &Out) {
  const uint32_t Base = Plan.SlotBase[X];
  const uint32_t N = Ssa.Mir.IndexableTemps[X].Elements;
  for (uint32_t E = 0; E < N; ++E) {
    Operand Dst{OperandKind::IndexableTemp, X, E, 1, 0};
    Out.push_back(
        MakeUnarySsa(D3D10_SB_OPCODE_MOV, Dst, DefStack[Base + E].back()));
  }
}

void RenameState::EmitDynamicWrite(const SsaInst &Inst, uint32_t Base,
                                   uint32_t N, std::vector<SsaInst> &Out) {
  Operand Sel = MakeSsaLane(Inst.Dest.RelativeRegister, Inst.Dest.RelativeLane);
  const Operand Val = NormalizeScalar(Inst.Sources[0]);
  for (uint32_t E = 0; E < N; ++E) {
    const Operand Match = MakeSsaTemp(Ssa.AllocValue(), 1, 0);
    Out.push_back(MakeBinarySsa(D3D10_SB_OPCODE_IEQ, Match, Sel, MakeImm(E)));
    const Operand Next = MakeSsaTemp(Ssa.AllocValue(), 1, 0);
    Out.push_back(MakeMovcSsa(Next, Match, Val, DefStack[Base + E].back()));
    PushDef(Base + E, Next);
  }
}

bool RenameState::BlockNeedsRebuild(uint32_t B) const {
  if (!Plan.SpillsAtBlock[B].empty())
    return true;
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  for (const auto &Inst : Ssa.Blocks[B].Insts) {
    if (Inst.Dead)
      continue;
    if (Inst.Dest.Kind == OperandKind::IndexableTemp &&
        Inst.Dest.Index < NumX &&
        Plan.Modes[Inst.Dest.Index] == PromoteMode::Full &&
        Inst.Dest.RelativeRegister != UINT32_MAX)
      return true;
    for (const auto &Src : Inst.Sources) {
      if (Src.Kind == OperandKind::IndexableTemp && Src.Index < NumX &&
          Plan.Modes[Src.Index] == PromoteMode::Full &&
          Src.RelativeRegister != UINT32_MAX)
        return true;
    }
  }
  return false;
}

bool RenameState::RenameBlock(uint32_t B) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  auto &Block = Ssa.Blocks[B];
  for (const auto &PP : BlockPhis[B])
    PushDef(PP.Slot, MakeSsaTemp(Block.Phis[PP.PhiIndex].Result));

  const bool NeedsRebuild = BlockNeedsRebuild(B);
  bool Changed = false;
  std::vector<SsaInst> Rebuilt;
  if (NeedsRebuild)
    Rebuilt.reserve(Block.Insts.size() + 32);

  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    for (const auto &Sp : Plan.SpillsAtBlock[B]) {
      if (Sp.second == I) {
        EmitSpillStores(Sp.first, Rebuilt);
        Changed = true;
      }
    }
    auto &Inst = Block.Insts[I];
    if (Inst.Dead)
      continue;
    uint32_t FullDynReadX = UINT32_MAX;
    for (auto &Src : Inst.Sources) {
      if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX ||
          Plan.SlotBase[Src.Index] == UINT32_MAX)
        continue;
      if (Src.RelativeRegister == UINT32_MAX && Src.Modifier == 0) {
        Src = DefStack[Plan.SlotBase[Src.Index] + Src.Element].back();
        Changed = true;
      } else if (Plan.Modes[Src.Index] == PromoteMode::Full) {
        FullDynReadX = Src.Index;
      }
    }
    if (FullDynReadX != UINT32_MAX) {
      EmitDynamicRead(Inst, Plan.SlotBase[FullDynReadX],
                      Ssa.Mir.IndexableTemps[FullDynReadX].Elements, Rebuilt);
      Changed = true;
      continue;
    }
    if (Inst.Dest.Kind == OperandKind::IndexableTemp &&
        Inst.Dest.Index < NumX &&
        Plan.SlotBase[Inst.Dest.Index] != UINT32_MAX) {
      const uint32_t X = Inst.Dest.Index;
      const uint32_t Base = Plan.SlotBase[X];
      if (Inst.Dest.RelativeRegister != UINT32_MAX) {
        EmitDynamicWrite(Inst, Base, Ssa.Mir.IndexableTemps[X].Elements,
                         Rebuilt);
        Changed = true;
        continue;
      }
      PushDef(Base + Inst.Dest.Element, NormalizeScalar(Inst.Sources[0]));
      const bool KillStore =
          Plan.Modes[X] == PromoteMode::Full ||
          (Plan.Modes[X] == PromoteMode::DomSpill &&
           !Plan.Spills[X].AfterSpill[B] &&
           (B != Plan.Spills[X].Block || I < Plan.Spills[X].InstIdx));
      if (KillStore) {
        Inst.Dead = true;
        Changed = true;
        continue;
      }
    }
    if (NeedsRebuild)
      Rebuilt.push_back(std::move(Inst));
  }
  for (const auto &Sp : Plan.SpillsAtBlock[B]) {
    if (Sp.second == Block.Insts.size()) {
      EmitSpillStores(Sp.first, Rebuilt);
      Changed = true;
    }
  }
  if (NeedsRebuild)
    Block.Insts = std::move(Rebuilt);

  llvm::SmallVector<uint32_t, 4> SeenSucc;
  for (uint32_t Succ : Block.Successors) {
    if (Succ >= Ssa.Blocks.size() || !Dom.Reachable(Succ))
      continue;
    if (std::find(SeenSucc.begin(), SeenSucc.end(), Succ) != SeenSucc.end())
      continue;
    SeenSucc.push_back(Succ);
    for (const auto &PP : BlockPhis[Succ]) {
      Ssa.Blocks[Succ].Phis[PP.PhiIndex].Inputs.push_back(
          {B, DefStack[PP.Slot].back()});
    }
  }
  return Changed;
}

} // namespace

bool dxcb5::RunOptIndexableSsa(SsaProgram &Ssa, const SsaDomTree &Dom) {
  if (Ssa.Mir.IndexableTemps.empty() || Ssa.Blocks.empty())
    return false;
  llvm::TimeTraceScope Scope("DXCB5 Opt Indexable SSA", llvm::StringRef());

  PromotionPlan Plan;
  BuildPromotionPlan(Ssa, Dom, Plan);
  if (Plan.TotalSlots == 0)
    return false;

  std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
  Ssa.BuildPredecessors(Preds);
  std::vector<llvm::SmallVector<uint32_t, 4>> DF;
  ComputeDominanceFrontiers(Dom, Preds, DF);
  std::vector<std::vector<uint64_t>> LiveIn;
  ComputeSlotLiveness(Ssa, Dom, Plan, LiveIn);
  std::vector<llvm::SmallVector<PlacedPhi, 4>> BlockPhis;
  PlaceSlotPhis(Ssa, Dom, Plan, DF, LiveIn, BlockPhis);

  RenameState RS{Ssa, Dom, Plan, BlockPhis, {}, {}};
  RS.DefStack.resize(Plan.TotalSlots);
  for (uint32_t S = 0; S < Plan.TotalSlots; ++S)
    RS.DefStack[S].push_back(MakeImm(0));

  struct Frame {
    uint32_t Block;
    size_t Mark;
    bool Exit;
  };
  llvm::SmallVector<Frame, 64> Stack;
  Stack.push_back({0, 0, false});
  bool Changed = false;
  while (!Stack.empty()) {
    const Frame F = Stack.pop_back_val();
    if (F.Exit) {
      RS.PopTo(F.Mark);
      continue;
    }
    const size_t Mark = RS.PushLog.size();
    Changed |= RS.RenameBlock(F.Block);
    Stack.push_back({F.Block, Mark, true});
    const auto &Kids = Dom.Children[F.Block];
    for (auto It = Kids.rbegin(); It != Kids.rend(); ++It)
      Stack.push_back({*It, 0, false});
  }
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}
