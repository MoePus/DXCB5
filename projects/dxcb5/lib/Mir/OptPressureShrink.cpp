// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "Mir/PlacementStore.h"
#include "Mir/SsaAnalysis.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <vector>

using namespace dxcb5;

namespace {

constexpr unsigned kSinkActivationScalars = 96;
constexpr unsigned kRematActivationScalars = 180;
constexpr unsigned kMaxRematTargets = 2;

bool HasNoMemOperands(const SsaInst &Inst) {
  for (const auto &Src : Inst.Sources)
    if (Src.Kind == OperandKind::IndexableTemp || Src.Kind == OperandKind::Uav)
      return false;
  return true;
}

bool IsSinkablePure(const SsaInst &Inst) {
  return !Inst.Dead && Inst.IsPure() && !Inst.HasAuxDest() &&
         Inst.Dest.Kind == OperandKind::Temp &&
         Inst.Dest.RelativeRegister == UINT32_MAX && HasNoMemOperands(Inst);
}

bool IsCheapRematOpcode(uint32_t Op) {
  switch (Op) {
  case D3D10_SB_OPCODE_MOV: case D3D10_SB_OPCODE_ADD: case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_MAD: case D3D10_SB_OPCODE_MIN: case D3D10_SB_OPCODE_MAX:
  case D3D10_SB_OPCODE_LT:  case D3D10_SB_OPCODE_GE:  case D3D10_SB_OPCODE_EQ:
  case D3D10_SB_OPCODE_NE:  case D3D10_SB_OPCODE_MOVC:
  case D3D10_SB_OPCODE_AND: case D3D10_SB_OPCODE_OR:  case D3D10_SB_OPCODE_XOR:
  case D3D10_SB_OPCODE_NOT: case D3D10_SB_OPCODE_IADD: case D3D10_SB_OPCODE_INEG:
  case D3D10_SB_OPCODE_IMAD: case D3D10_SB_OPCODE_IMUL: case D3D10_SB_OPCODE_ISHL:
  case D3D10_SB_OPCODE_ISHR: case D3D10_SB_OPCODE_USHR: case D3D10_SB_OPCODE_IEQ:
  case D3D10_SB_OPCODE_INE: case D3D10_SB_OPCODE_ILT: case D3D10_SB_OPCODE_IGE:
  case D3D10_SB_OPCODE_ULT: case D3D10_SB_OPCODE_UGE: case D3D10_SB_OPCODE_FTOI:
  case D3D10_SB_OPCODE_FTOU: case D3D10_SB_OPCODE_ITOF: case D3D10_SB_OPCODE_UTOF:
  case D3D10_SB_OPCODE_FRC: case D3D10_SB_OPCODE_ROUND_NE: case D3D10_SB_OPCODE_ROUND_NI:
  case D3D10_SB_OPCODE_ROUND_PI: case D3D10_SB_OPCODE_ROUND_Z:
  case D3D10_SB_OPCODE_DP2: case D3D10_SB_OPCODE_DP3: case D3D10_SB_OPCODE_DP4:
  case PseudoCombineVec:
    return true;
  default:
    return false;
  }
}

void CollectInstTempSources(const SsaInst &Inst, uint32_t Limit,
                            llvm::SmallVectorImpl<uint32_t> &Out) {
  auto Add = [&](uint32_t Id) {
    if (Id < Limit && std::find(Out.begin(), Out.end(), Id) == Out.end())
      Out.push_back(Id);
  };
  for (const auto &Src : Inst.Sources) {
    if (Src.Kind == OperandKind::Temp)
      Add(Src.Index);
    if (Src.RelativeRegister != UINT32_MAX)
      Add(Src.RelativeRegister);
  }
}

struct BlockLiveness {
  std::vector<uint8_t> ValWidth;
  std::vector<uint8_t> ValDefMask;
  std::vector<llvm::BitVector> LiveIn;
  std::vector<llvm::BitVector> LiveOut;
  std::vector<unsigned> LiveInScalars;
  std::vector<unsigned> BlockPeakLanes;
  unsigned PeakScalars = 0;
  unsigned ExactPeakLanes = 0;
};

// Exact per-(vreg, lane) intra-block peak live scalar lanes (PL) for one SSA block.
unsigned ComputeSsaBlockPeakLanes(const SsaBlock &Blk, uint32_t VCount,
                                  const llvm::BitVector &LiveOut,
                                  const std::vector<uint8_t> &ValDefMask) {
  std::vector<uint8_t> LiveMask(VCount, 0);
  unsigned Cur = 0;
  unsigned Peak = 0;
  auto SetMask = [&](uint32_t V, uint8_t NewM) {
    if (V >= VCount)
      return;
    NewM &= 0xfu;
    const uint8_t Old = LiveMask[V];
    if (Old == NewM)
      return;
    LiveMask[V] = NewM;
    Cur = Cur + PopcountMask(NewM) - PopcountMask(Old);
    if (Cur > Peak)
      Peak = Cur;
  };
  auto AddMask = [&](uint32_t V, uint8_t M) {
    if (V < VCount)
      SetMask(V, static_cast<uint8_t>(LiveMask[V] | (M & 0xfu)));
  };
  auto ClearMask = [&](uint32_t V, uint8_t M) {
    if (V < VCount)
      SetMask(V, static_cast<uint8_t>(LiveMask[V] & ~(M & 0xfu)));
  };

  for (int V = LiveOut.find_first(); V >= 0; V = LiveOut.find_next(V))
    AddMask(static_cast<uint32_t>(V), ValDefMask[static_cast<uint32_t>(V)]);
  if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch) {
    if (Blk.Condition.Kind == OperandKind::Temp)
      AddMask(Blk.Condition.Index,
              static_cast<uint8_t>(1u << (Blk.Condition.Swizzle & 3u)));
    if (Blk.Condition.RelativeRegister != UINT32_MAX)
      AddMask(Blk.Condition.RelativeRegister,
              static_cast<uint8_t>(1u << Blk.Condition.RelativeLane));
  }
  for (size_t Rev = Blk.Insts.size(); Rev > 0; --Rev) {
    const auto &Inst = Blk.Insts[Rev - 1];
    if (Inst.Dead)
      continue;
    uint8_t DestMask = 0;
    if (Inst.Dest.Kind != OperandKind::Null)
      DestMask |= static_cast<uint8_t>(Inst.Dest.Mask & 0xfu);
    if (Inst.AuxDest.Kind != OperandKind::Null)
      DestMask |= static_cast<uint8_t>(Inst.AuxDest.Mask & 0xfu);
    if (!DestMask)
      DestMask = 0x1u;
    if (Inst.Dest.Kind == OperandKind::Temp)
      AddMask(Inst.Dest.Index, Inst.Dest.Mask);
    if (Inst.AuxDest.Kind == OperandKind::Temp)
      AddMask(Inst.AuxDest.Index, Inst.AuxDest.Mask);
    if (Inst.Dest.Kind == OperandKind::Temp)
      ClearMask(Inst.Dest.Index, Inst.Dest.Mask);
    if (Inst.AuxDest.Kind == OperandKind::Temp)
      ClearMask(Inst.AuxDest.Index, Inst.AuxDest.Mask);
    if (Inst.Dest.RelativeRegister != UINT32_MAX)
      AddMask(Inst.Dest.RelativeRegister,
              static_cast<uint8_t>(1u << Inst.Dest.RelativeLane));
    if (Inst.AuxDest.RelativeRegister != UINT32_MAX)
      AddMask(Inst.AuxDest.RelativeRegister,
              static_cast<uint8_t>(1u << Inst.AuxDest.RelativeLane));
    for (size_t S = 0; S < Inst.Sources.size(); ++S) {
      const auto &Src = Inst.Sources[S];
      if (Src.RelativeRegister != UINT32_MAX)
        AddMask(Src.RelativeRegister,
                static_cast<uint8_t>(1u << Src.RelativeLane));
      if (Src.Kind == OperandKind::Temp)
        AddMask(Src.Index, InstructionSourceReadLanes(Inst.Opcode, DestMask, S,
                                                      Src.Swizzle));
    }
  }
  return Peak;
}

void RecordPhiEdgeUses(
    const SsaPhi &Phi, uint32_t B, uint32_t N, uint32_t VCount,
    const SsaDomTree &Dom,
    std::vector<llvm::SmallVector<std::pair<uint32_t, uint32_t>, 8>> &EdgePhiUses) {
  for (const auto &In : Phi.Inputs) {
    if (In.PredBlock >= N || !Dom.Reachable(In.PredBlock))
      continue;
    if (In.Value.Kind == OperandKind::Temp && In.Value.Index < VCount)
      EdgePhiUses[In.PredBlock].push_back({B, In.Value.Index});
    if (In.Value.RelativeRegister != UINT32_MAX &&
        In.Value.RelativeRegister < VCount)
      EdgePhiUses[In.PredBlock].push_back({B, In.Value.RelativeRegister});
  }
}

void ComputeBlockLiveness(const SsaProgram &Ssa, const SsaDomTree &Dom,
                          BlockLiveness &L) {
  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  const uint32_t VCount = Ssa.NextValueId;
  L.ValWidth.assign(VCount, 1);
  L.ValDefMask.assign(VCount, 1u);
  std::vector<llvm::BitVector> Use(N, llvm::BitVector(VCount));
  std::vector<llvm::BitVector> Def(N, llvm::BitVector(VCount));
  std::vector<llvm::BitVector> PhiDefs(N, llvm::BitVector(VCount));
  std::vector<llvm::SmallVector<std::pair<uint32_t, uint32_t>, 8>> EdgePhiUses(N);

  auto MarkRelUse = [&](uint32_t B, uint32_t Rel) {
    if (Rel != UINT32_MAX && Rel < VCount && !Def[B].test(Rel))
      Use[B].set(Rel);
  };
  auto MarkUse = [&](uint32_t B, const Operand &Op) {
    if (Op.Kind == OperandKind::Temp && Op.Index < VCount && !Def[B].test(Op.Index))
      Use[B].set(Op.Index);
    MarkRelUse(B, Op.RelativeRegister);
  };
  auto MarkDef = [&](uint32_t B, const Operand &Op) {
    if (Op.Kind == OperandKind::Temp && Op.Index < VCount) {
      L.ValDefMask[Op.Index] = static_cast<uint8_t>(Op.Mask & 0xfu);
      L.ValWidth[Op.Index] = static_cast<uint8_t>(PopcountMask(Op.Mask));
      Def[B].set(Op.Index);
    }
  };

  for (uint32_t B = 0; B < N; ++B) {
    if (!Dom.Reachable(B)) continue;
    const auto &Blk = Ssa.Blocks[B];
    for (const auto &Phi : Blk.Phis) {
      if (Phi.Dead || Phi.Result >= VCount) continue;
      L.ValDefMask[Phi.Result] = static_cast<uint8_t>(Phi.Mask & 0xfu);
      L.ValWidth[Phi.Result] = static_cast<uint8_t>(PopcountMask(Phi.Mask));
      PhiDefs[B].set(Phi.Result);
      Def[B].set(Phi.Result);
      RecordPhiEdgeUses(Phi, B, N, VCount, Dom, EdgePhiUses);
    }
    for (const auto &Inst : Blk.Insts) {
      if (Inst.Dead) continue;
      MarkRelUse(B, Inst.Dest.RelativeRegister);
      MarkRelUse(B, Inst.AuxDest.RelativeRegister);
      for (const auto &Src : Inst.Sources) MarkUse(B, Src);
      MarkDef(B, Inst.Dest);
      MarkDef(B, Inst.AuxDest);
    }
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
      MarkUse(B, Blk.Condition);
  }

  L.LiveIn.assign(N, llvm::BitVector(VCount));
  L.LiveOut.assign(N, llvm::BitVector(VCount));
  llvm::BitVector Tmp(VCount);
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto It = Dom.RpoOrder.rbegin(); It != Dom.RpoOrder.rend(); ++It) {
      const uint32_t B = *It;
      Tmp.reset();
      for (uint32_t Succ : Ssa.Blocks[B].Successors) {
        if (Succ >= N || !Dom.Reachable(Succ))
          continue;
        llvm::BitVector SuccIn = L.LiveIn[Succ];
        SuccIn.reset(PhiDefs[Succ]);
        Tmp |= SuccIn;
      }
      for (const auto &EU : EdgePhiUses[B])
        Tmp.set(EU.second);
      if (Tmp != L.LiveOut[B]) {
        L.LiveOut[B] = Tmp;
        Changed = true;
      }
      Tmp.reset(Def[B]);
      Tmp |= Use[B];
      if (Tmp != L.LiveIn[B]) {
        L.LiveIn[B] = Tmp;
        Changed = true;
      }
    }
  }

  L.LiveInScalars.assign(N, 0);
  L.BlockPeakLanes.assign(N, 0);
  L.PeakScalars = 0;
  L.ExactPeakLanes = 0;
  for (uint32_t B : Dom.RpoOrder) {
    unsigned Sum = 0;
    for (int V = L.LiveIn[B].find_first(); V >= 0; V = L.LiveIn[B].find_next(V))
      Sum += L.ValWidth[static_cast<uint32_t>(V)];
    L.LiveInScalars[B] = Sum;
    L.PeakScalars = std::max(L.PeakScalars, Sum);
    const unsigned BPeak =
        ComputeSsaBlockPeakLanes(Ssa.Blocks[B], VCount, L.LiveOut[B], L.ValDefMask);
    L.BlockPeakLanes[B] = BPeak;
    L.ExactPeakLanes = std::max(L.ExactPeakLanes, BPeak);
  }
}

void BuildUseBlockLists(const SsaProgram &Ssa, const SsaDomTree &Dom,
                        std::vector<llvm::SmallVector<uint32_t, 4>> &Uses) {
  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  const uint32_t VCount = Ssa.NextValueId;
  Uses.assign(VCount, {});
  auto Add = [&](const Operand &Op, uint32_t B) {
    if (Op.Kind == OperandKind::Temp && Op.Index < VCount)
      Uses[Op.Index].push_back(B);
    if (Op.RelativeRegister != UINT32_MAX && Op.RelativeRegister < VCount)
      Uses[Op.RelativeRegister].push_back(B);
  };
  for (uint32_t B : Dom.RpoOrder) {
    const auto &Blk = Ssa.Blocks[B];
    for (const auto &Phi : Blk.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        if (In.PredBlock < N && Dom.Reachable(In.PredBlock))
          Add(In.Value, In.PredBlock);
    }
    for (const auto &Inst : Blk.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.RelativeRegister != UINT32_MAX &&
          Inst.Dest.RelativeRegister < VCount)
        Uses[Inst.Dest.RelativeRegister].push_back(B);
      if (Inst.AuxDest.RelativeRegister != UINT32_MAX &&
          Inst.AuxDest.RelativeRegister < VCount)
        Uses[Inst.AuxDest.RelativeRegister].push_back(B);
      for (const auto &Src : Inst.Sources)
        Add(Src, B);
    }
    if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
      Add(Blk.Condition, B);
  }
}

bool RunPressureSinkStep(SsaProgram &Ssa, const SsaDomTree &Dom,
                         const std::vector<uint32_t> &LoopDepth,
                         BlockLiveness &Live, PlacementStore &Store) {
  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  const uint32_t VCount = Ssa.NextValueId;
  std::vector<llvm::SmallVector<uint32_t, 4>> Uses;
  BuildUseBlockLists(Ssa, Dom, Uses);

  auto Retarget = [&](uint32_t V, uint32_t OldB, uint32_t NewB) {
    if (V >= Uses.size())
      return;
    for (uint32_t &U : Uses[V]) {
      if (U == OldB) {
        U = NewB;
        return;
      }
    }
  };

  std::vector<llvm::SmallVector<SsaInst, 8>> SunkTo(N);
  bool SunkAny = false;
  for (auto It = Dom.RpoOrder.rbegin(); It != Dom.RpoOrder.rend(); ++It) {
    const uint32_t B = *It;
    auto &Blk = Ssa.Blocks[B];
    for (size_t I = Blk.Insts.size(); I-- > 0;) {
      auto &Inst = Blk.Insts[I];
      if (!IsSinkablePure(Inst) || Inst.Dest.Index >= VCount)
        continue;
      const auto &UList = Uses[Inst.Dest.Index];
      if (UList.empty())
        continue;
      uint32_t Lca = UINT32_MAX;
      for (uint32_t U : UList)
        Lca = DomLca(Dom, Lca, U);
      if (Lca == UINT32_MAX || Lca == B || !Dom.Dominates(B, Lca) ||
          LoopDepth[Lca] > LoopDepth[B])
        continue;

      llvm::SmallVector<uint32_t, 4> TempSrcs;
      CollectInstTempSources(Inst, VCount, TempSrcs);
      unsigned ExtraScalars = 0;
      for (uint32_t S : TempSrcs)
        if (!Live.LiveIn[Lca].test(S))
          ExtraScalars += Live.ValWidth[S];
      const unsigned DestScalars = PopcountMask(Inst.Dest.Mask);
      if (ExtraScalars > DestScalars)
        continue;

      PlacementMove Move;
      Move.ValId = Inst.Dest.Index;
      Move.Owner = PlacementOwner::PressureShrink;
      Move.SrcDepth = LoopDepth[B];
      Move.DstDepths.push_back(LoopDepth[Lca]);
      const unsigned CurBlockPl = Live.BlockPeakLanes[B];
      Move.PlBefore = CurBlockPl;
      const unsigned Saved = DestScalars - ExtraScalars;
      Move.PlAfter = CurBlockPl > Saved ? CurBlockPl - Saved : 0u;
      Move.Budget = kRematActivationScalars;
      if (!Store.TryAccept(Move))
        continue;

      for (const auto &Src : Inst.Sources) {
        if (Src.Kind == OperandKind::Temp && Src.Index < VCount)
          Retarget(Src.Index, B, Lca);
        if (Src.RelativeRegister != UINT32_MAX && Src.RelativeRegister < VCount)
          Retarget(Src.RelativeRegister, B, Lca);
      }
      for (uint32_t S : TempSrcs)
        Live.LiveIn[Lca].set(S);
      SunkTo[Lca].push_back(Inst);
      Inst.Dead = true;
      SunkAny = true;
    }
  }

  if (SunkAny) {
    for (uint32_t B = 0; B < N; ++B) {
      if (SunkTo[B].empty())
        continue;
      std::reverse(SunkTo[B].begin(), SunkTo[B].end());
      auto &Dst = Ssa.Blocks[B].Insts;
      Dst.insert(Dst.begin(), std::make_move_iterator(SunkTo[B].begin()),
                 std::make_move_iterator(SunkTo[B].end()));
    }
    Ssa.CompactDead();
  }
  return SunkAny;
}

bool ReplaceValInBlock(SsaProgram &Ssa, uint32_t BlockId, uint32_t OldId,
                       uint32_t NewId, size_t &OutFirstInstIdx) {
  auto &Blk = Ssa.Blocks[BlockId];
  bool Any = false;
  OutFirstInstIdx = Blk.Insts.size();
  auto RewriteOp = [&](Operand &Op) {
    bool Hit = false;
    if (Op.Kind == OperandKind::Temp && Op.Index == OldId) { Op.Index = NewId; Hit = true; }
    if (Op.RelativeRegister == OldId) { Op.RelativeRegister = NewId; Hit = true; }
    return Hit;
  };
  for (size_t I = 0; I < Blk.Insts.size(); ++I) {
    auto &Inst = Blk.Insts[I];
    if (Inst.Dead) continue;
    bool Hit = false;
    if (Inst.Dest.RelativeRegister == OldId) { Inst.Dest.RelativeRegister = NewId; Hit = true; }
    if (Inst.AuxDest.RelativeRegister == OldId) { Inst.AuxDest.RelativeRegister = NewId; Hit = true; }
    for (auto &Src : Inst.Sources) Hit |= RewriteOp(Src);
    if (Hit) {
      if (OutFirstInstIdx == Blk.Insts.size()) OutFirstInstIdx = I;
      Any = true;
    }
  }
  if (Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch)
    Any |= RewriteOp(Blk.Condition);
  for (uint32_t Succ : Blk.Successors) {
    if (Succ >= Ssa.Blocks.size()) continue;
    for (auto &Phi : Ssa.Blocks[Succ].Phis) {
      if (Phi.Dead) continue;
      for (auto &In : Phi.Inputs)
        if (In.PredBlock == BlockId) Any |= RewriteOp(In.Value);
    }
  }
  return Any;
}

struct PendingRemat {
  uint32_t ValId = 0;
  uint32_t DefBlock = 0;
  uint32_t DefIndex = 0;
  bool UsedInDefBlk = false;
  SsaInst Orig;
  llvm::SmallVector<uint32_t, 4> RemoteBlocks;
};

bool RunHighPressureRemat(SsaProgram &Ssa, const SsaDomTree &Dom,
                          const std::vector<uint32_t> &LoopDepth,
                          const BlockLiveness &Live, PlacementStore &Store) {
  if (Live.PeakScalars <= kRematActivationScalars)
    return false;
  const uint32_t OrigVCount = Ssa.NextValueId;
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<llvm::SmallVector<uint32_t, 4>> Uses;
  BuildUseBlockLists(Ssa, Dom, Uses);

  llvm::BitVector LiveInHot(OrigVCount);
  std::vector<uint8_t> ReachFromHot(Ssa.Blocks.size(), 0);
  llvm::SmallVector<uint32_t, 32> HotWork;
  for (uint32_t B : Dom.RpoOrder) {
    if (Live.LiveInScalars[B] > kRematActivationScalars) {
      LiveInHot |= Live.LiveIn[B];
      ReachFromHot[B] = 1;
      HotWork.push_back(B);
    }
  }
  while (!HotWork.empty()) {
    const uint32_t Cur = HotWork.pop_back_val();
    for (uint32_t S : Ssa.Blocks[Cur].Successors) {
      if (S < ReachFromHot.size() && Dom.Reachable(S) && !ReachFromHot[S]) {
        ReachFromHot[S] = 1;
        HotWork.push_back(S);
      }
    }
  }

  // Phase 1: Scan while Ssa.Blocks[*].Insts indices (Defs) remain immutable.
  // Track both Rematted and Pinned sources so no value is both rematted and
  // read by another rematted clone regardless of value-ID ordering.
  llvm::BitVector Rematted(OrigVCount);
  llvm::BitVector Pinned(OrigVCount);
  llvm::SmallVector<PendingRemat, 32> Plan;
  for (int VIdx = LiveInHot.find_first(); VIdx >= 0;
       VIdx = LiveInHot.find_next(VIdx)) {
    const uint32_t V = static_cast<uint32_t>(VIdx);
    if (Pinned.test(V))
      continue;
    const auto &D = Defs[V];
    if (!D.Valid() || D.IsPhi || D.IsAux)
      continue;
    const SsaInst &Orig = Ssa.Blocks[D.Block].Insts[D.Index];
    if (!IsSinkablePure(Orig) || !IsCheapRematOpcode(Orig.Opcode))
      continue;

    llvm::SmallVector<uint32_t, 4> TempSrcs;
    CollectInstTempSources(Orig, OrigVCount, TempSrcs);
    bool SrcConflict = false;
    for (uint32_t S : TempSrcs)
      if (Rematted.test(S))
        SrcConflict = true;
    if (SrcConflict)
      continue;

    const bool IsFreeMov =
        Orig.Opcode == D3D10_SB_OPCODE_MOV && TempSrcs.empty();
    PendingRemat Entry;
    Entry.ValId = V;
    Entry.DefBlock = D.Block;
    Entry.DefIndex = D.Index;
    bool AllOk = true;
    llvm::SmallVector<uint32_t, 4> LateBlocks;
    bool LateOk = true;
    for (uint32_t U : Uses[V]) {
      if (U == D.Block) {
        Entry.UsedInDefBlk = true;
        continue;
      }
      // Never remat into a deeper loop than the def (even for cb0/imm MOVs),
      // so OptPressureShrink never undoes LICM hoisting into hot loops (R4).
      if (LoopDepth[U] > LoopDepth[D.Block])
        AllOk = false;
      if (std::find(Entry.RemoteBlocks.begin(), Entry.RemoteBlocks.end(), U) ==
          Entry.RemoteBlocks.end())
        Entry.RemoteBlocks.push_back(U);
      if (ReachFromHot[U]) {
        if (LoopDepth[U] > LoopDepth[D.Block])
          LateOk = false;
        if (std::find(LateBlocks.begin(), LateBlocks.end(), U) ==
            LateBlocks.end())
          LateBlocks.push_back(U);
      }
    }
    const bool FullRemat =
        AllOk && !Entry.RemoteBlocks.empty() &&
        (IsFreeMov || Entry.RemoteBlocks.size() <= kMaxRematTargets);
    if (!FullRemat) {
      // Partial late-use remat: keep the original def for pre-hot uses and
      // only remat uses reachable from hot blocks so V dies before the peak.
      if (!LateOk || LateBlocks.empty() ||
          (!IsFreeMov && LateBlocks.size() > kMaxRematTargets))
        continue;
      Entry.UsedInDefBlk = true;
      Entry.RemoteBlocks = std::move(LateBlocks);
    }

    // Require every Temp source to already be live-in at each remote target so
    // cloning never lengthens another value's live range across hot blocks.
    bool SrcsLive = true;
    for (uint32_t U : Entry.RemoteBlocks) {
      for (uint32_t S : TempSrcs)
        if (!Live.LiveIn[U].test(S))
          SrcsLive = false;
      if (!SrcsLive)
        break;
    }
    if (!SrcsLive)
      continue;

    PlacementMove Move;
    Move.ValId = V;
    Move.Owner = PlacementOwner::PressureShrink;
    Move.SrcDepth = LoopDepth[D.Block];
    for (uint32_t U : Entry.RemoteBlocks)
      Move.DstDepths.push_back(LoopDepth[U]);
    Move.KeepSrc = Entry.UsedInDefBlk;
    Move.PlBefore = Live.PeakScalars;
    const unsigned ValScalars = PopcountMask(Orig.Dest.Mask);
    Move.PlAfter =
        Live.PeakScalars > ValScalars ? Live.PeakScalars - ValScalars : 0u;
    Move.Budget = kRematActivationScalars;
    if (!Store.TryAccept(Move))
      continue;

    Entry.Orig = Orig;
    Rematted.set(V);
    for (uint32_t S : TempSrcs)
      Pinned.set(S);
    Plan.push_back(std::move(Entry));
  }
  if (Plan.empty())
    return false;

  // Phase 2: Mark dead defs before inserting clones (DefIndex still valid).
  for (const auto &P : Plan)
    if (!P.UsedInDefBlk)
      Ssa.Blocks[P.DefBlock].Insts[P.DefIndex].Dead = true;
  for (const auto &P : Plan) {
    for (uint32_t U : P.RemoteBlocks) {
      const uint32_t NewId = Ssa.AllocValue();
      size_t InsertPos = 0;
      if (!ReplaceValInBlock(Ssa, U, P.ValId, NewId, InsertPos))
        continue;
      SsaInst Clone = P.Orig;
      Clone.Dest.Index = NewId;
      auto &DstInsts = Ssa.Blocks[U].Insts;
      DstInsts.insert(DstInsts.begin() + static_cast<ptrdiff_t>(InsertPos),
                      std::move(Clone));
    }
  }
  Ssa.CompactDead();
  return true;
}

bool CondReadsVal(const SsaBlock &Blk, uint32_t V) {
  if (Blk.Kind != FlowKind::Branch && Blk.Kind != FlowKind::Switch)
    return false;
  return (Blk.Condition.Kind == OperandKind::Temp && Blk.Condition.Index == V) ||
         Blk.Condition.RelativeRegister == V;
}

bool InstReadsVal(const SsaInst &Inst, uint32_t V) {
  if (Inst.Dest.RelativeRegister == V || Inst.AuxDest.RelativeRegister == V)
    return true;
  for (const auto &Src : Inst.Sources)
    if ((Src.Kind == OperandKind::Temp && Src.Index == V) ||
        Src.RelativeRegister == V)
      return true;
  return false;
}

bool IsUsedAtOrAfter(const SsaBlock &Blk, size_t StartIdx, uint32_t S) {
  if (CondReadsVal(Blk, S))
    return true;
  for (size_t J = StartIdx; J < Blk.Insts.size(); ++J)
    if (!Blk.Insts[J].Dead && InstReadsVal(Blk.Insts[J], S))
      return true;
  return false;
}

bool SinkOneBlock(SsaBlock &Blk, uint32_t VCount,
                  const llvm::BitVector &LiveOut,
                  const std::vector<uint8_t> &ValWidth,
                  const std::vector<uint8_t> &ValDefMask,
                  unsigned CurBlockPeak) {
  if (Blk.Insts.size() < 8)
    return false;
  bool Changed = false;
  for (size_t I = Blk.Insts.size(); I-- > 0;) {
    const SsaInst &Inst = Blk.Insts[I];
    if (!IsSinkablePure(Inst) || Inst.Dest.Index >= VCount)
      continue;
    const uint32_t V = Inst.Dest.Index;
    size_t FirstUse = Blk.Insts.size();
    for (size_t J = I + 1; J < Blk.Insts.size(); ++J) {
      if (!Blk.Insts[J].Dead && InstReadsVal(Blk.Insts[J], V)) {
        FirstUse = J;
        break;
      }
    }
    if (FirstUse == Blk.Insts.size() && !LiveOut.test(V) && !CondReadsVal(Blk, V))
      continue;
    if (FirstUse <= I + 1)
      continue;
    llvm::SmallVector<uint32_t, 4> TempSrcs;
    CollectInstTempSources(Inst, VCount, TempSrcs);
    unsigned ExtendedScalars = 0;
    for (uint32_t S : TempSrcs)
      if (!LiveOut.test(S) && !IsUsedAtOrAfter(Blk, FirstUse, S))
        ExtendedScalars += ValWidth[S];
    if (ExtendedScalars > PopcountMask(Inst.Dest.Mask))
      continue;
    std::rotate(Blk.Insts.begin() + static_cast<ptrdiff_t>(I),
                Blk.Insts.begin() + static_cast<ptrdiff_t>(I + 1),
                Blk.Insts.begin() + static_cast<ptrdiff_t>(FirstUse));
    // D-grade exact PL guard: when ExtendedScalars == 0, live lanes strictly
    // cannot increase at any program point; otherwise verify exact block PL.
    if (ExtendedScalars > 0) {
      const unsigned NewPeak =
          ComputeSsaBlockPeakLanes(Blk, VCount, LiveOut, ValDefMask);
      if (NewPeak > CurBlockPeak) {
        std::rotate(Blk.Insts.begin() + static_cast<ptrdiff_t>(I),
                    Blk.Insts.begin() + static_cast<ptrdiff_t>(FirstUse - 1),
                    Blk.Insts.begin() + static_cast<ptrdiff_t>(FirstUse));
        continue;
      }
      CurBlockPeak = NewPeak;
    }
    Changed = true;
  }
  return Changed;
}

bool RunIntraBlockSink(SsaProgram &Ssa, const SsaDomTree &Dom,
                       const BlockLiveness &Live) {
  bool Changed = false;
  for (uint32_t B : Dom.RpoOrder) {
    if (Live.BlockPeakLanes[B] <= kSinkActivationScalars &&
        Live.PeakScalars <= kSinkActivationScalars)
      continue;
    Changed |= SinkOneBlock(Ssa.Blocks[B], Ssa.NextValueId, Live.LiveOut[B],
                            Live.ValWidth, Live.ValDefMask,
                            Live.BlockPeakLanes[B]);
  }
  return Changed;
}

} // namespace

bool dxcb5::RunOptPressureShrink(SsaProgram &Ssa, const SsaDomTree &Dom,
                                 const SsaLoopInfo *CachedLoops,
                                 PlacementStore *Placement) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Pressure Shrink", llvm::StringRef());
  if (Ssa.Blocks.empty()) return false;
  BlockLiveness Live;
  ComputeBlockLiveness(Ssa, Dom, Live);
  if (Live.PeakScalars <= kSinkActivationScalars &&
      Live.ExactPeakLanes <= kSinkActivationScalars)
    return false;

  bool Changed = false;
  if (Ssa.Blocks.size() >= 2 && Live.PeakScalars > kSinkActivationScalars) {
    SsaLoopInfo LocalLoops;
    if (!CachedLoops) {
      ComputeLoopInfo(Ssa, Dom, LocalLoops);
      CachedLoops = &LocalLoops;
    }
    PlacementStore LocalPlacement;
    PlacementStore &Store = Placement ? *Placement : LocalPlacement;
    const auto &LoopDepth = CachedLoops->LoopDepth;
    for (unsigned Iter = 0; Iter < 3; ++Iter) {
      if (!RunPressureSinkStep(Ssa, Dom, LoopDepth, Live, Store)) break;
      Changed = true;
      ComputeBlockLiveness(Ssa, Dom, Live);
    }
    for (unsigned Iter = 0; Iter < 2; ++Iter) {
      if (!RunHighPressureRemat(Ssa, Dom, LoopDepth, Live, Store)) break;
      Changed = true;
      ComputeBlockLiveness(Ssa, Dom, Live);
      for (unsigned SIter = 0; SIter < 2; ++SIter) {
        if (!RunPressureSinkStep(Ssa, Dom, LoopDepth, Live, Store)) break;
        ComputeBlockLiveness(Ssa, Dom, Live);
      }
    }
  }
  Changed |= RunIntraBlockSink(Ssa, Dom, Live);
  return Changed;
}
