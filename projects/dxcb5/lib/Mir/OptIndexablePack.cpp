// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <array>

using namespace dxcb5;

namespace {

bool IsPackableStoreSrc(const Operand &Op) {
  return Op.Kind == OperandKind::Temp || Op.Kind == OperandKind::Input ||
         Op.Kind == OperandKind::ConstantBuffer ||
         Op.Kind == OperandKind::ImmediateConstantBuffer;
}

bool SameStoreBase(const Operand &A, const Operand &B) {
  return A.Kind == B.Kind && A.Index == B.Index && A.Element == B.Element &&
         A.RelativeRegister == B.RelativeRegister &&
         A.RelativeLane == B.RelativeLane && A.Modifier == B.Modifier;
}

bool CompatibleStoreSrc(const Operand &A, const Operand &B) {
  if (A.Kind == OperandKind::Immediate && B.Kind == OperandKind::Immediate)
    return A.Modifier == 0 && B.Modifier == 0;
  return IsPackableStoreSrc(A) && SameStoreBase(A, B);
}

void CollectEligibleArrays(const SsaProgram &Ssa,
                           std::vector<uint8_t> &Eligible) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  Eligible.assign(NumX, 0);
  for (size_t X = 0; X < NumX; ++X) {
    const auto &D = Ssa.Mir.IndexableTemps[X];
    if (D.Components == 1 && D.Elements >= 2)
      Eligible[X] = 1;
  }
  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Phi : Block.Phis)
      for (const auto &In : Phi.Inputs)
        if (In.Value.Kind == OperandKind::IndexableTemp && In.Value.Index < NumX)
          Eligible[In.Value.Index] = 0;
    if (Block.Condition.Kind == OperandKind::IndexableTemp &&
        Block.Condition.Index < NumX)
      Eligible[Block.Condition.Index] = 0;
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead || Inst.Dest.Kind != OperandKind::IndexableTemp ||
          Inst.Dest.Index >= NumX)
        continue;
      if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Sources.size() != 1 ||
          Inst.Dest.Mask != 1u || Inst.Saturate)
        Eligible[Inst.Dest.Index] = 0;
    }
  }
}

struct DynLoadKey {
  uint32_t Elements;
  uint32_t Elem;
  uint32_t RelReg;
  uint8_t RelLane;
  bool operator==(const DynLoadKey &O) const {
    return Elements == O.Elements && Elem == O.Elem && RelReg == O.RelReg &&
           RelLane == O.RelLane;
  }
};

struct DynKeyInfo {
  static DynLoadKey getEmptyKey() { return {UINT32_MAX, 0, 0, 0}; }
  static DynLoadKey getTombstoneKey() { return {UINT32_MAX - 1, 0, 0, 0}; }
  static unsigned getHashValue(const DynLoadKey &K) {
    return (K.Elements * 131u) ^ (K.Elem * 17u) ^ (K.RelReg * 31u) ^ K.RelLane;
  }
  static bool isEqual(const DynLoadKey &A, const DynLoadKey &B) {
    return A == B;
  }
};

struct PairAff {
  uint32_t Dyn = 0;
  uint32_t Const = 0;
};

uint64_t PairKey(uint32_t A, uint32_t B) {
  return A < B ? ((uint64_t(A) << 32) | B) : ((uint64_t(B) << 32) | A);
}

void AccumulateBlockAffinity(const SsaProgram &Ssa, const SsaBlock &Block,
                             const std::vector<uint8_t> &Eligible,
                             llvm::DenseMap<uint64_t, PairAff> &Aff) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  llvm::DenseMap<DynLoadKey, llvm::SmallVector<uint32_t, 4>, DynKeyInfo> Loads;
  llvm::DenseMap<uint64_t, llvm::SmallVector<std::pair<uint32_t, Operand>, 4>>
      Stores;

  for (const auto &Inst : Block.Insts) {
    if (Inst.Dead)
      continue;
    if (Inst.Dest.Kind == OperandKind::IndexableTemp &&
        Inst.Dest.Index < NumX && Eligible[Inst.Dest.Index] &&
        Inst.Dest.RelativeRegister == UINT32_MAX) {
      const uint32_t X = Inst.Dest.Index;
      const uint64_t SK =
          (uint64_t(Ssa.Mir.IndexableTemps[X].Elements) << 32) |
          Inst.Dest.Element;
      auto &Vec = Stores[SK];
      for (const auto &Prev : Vec) {
        if (Prev.first != X && CompatibleStoreSrc(Prev.second, Inst.Sources[0]))
          ++Aff[PairKey(Prev.first, X)].Const;
      }
      if (Vec.size() < 16)
        Vec.emplace_back(X, Inst.Sources[0]);
      continue;
    }
    if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate ||
        Inst.Dest.Kind != OperandKind::Temp || Inst.Dest.Mask != 1u ||
        Inst.Sources.size() != 1)
      continue;
    const Operand &Src = Inst.Sources[0];
    if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX ||
        !Eligible[Src.Index] || Src.RelativeRegister == UINT32_MAX ||
        Src.Modifier != 0)
      continue;
    const uint32_t X = Src.Index;
    const DynLoadKey Key{Ssa.Mir.IndexableTemps[X].Elements, Src.Element,
                         Src.RelativeRegister, Src.RelativeLane};
    auto &List = Loads[Key];
    if (std::find(List.begin(), List.end(), X) != List.end())
      continue;
    for (uint32_t PrevX : List)
      ++Aff[PairKey(PrevX, X)].Dyn;
    if (List.size() < 8)
      List.push_back(X);
  }
}

struct PackTarget {
  uint32_t NewX = UINT32_MAX;
  uint8_t Lane = 0;
  bool Packed = false;
};

bool BuildPackPlan(const SsaProgram &Ssa, const std::vector<uint8_t> &Eligible,
                   std::vector<PackTarget> &Plan,
                   std::vector<IndexableTempDecl> &NewDecls) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  llvm::DenseMap<uint64_t, PairAff> Aff;
  for (const auto &Block : Ssa.Blocks)
    AccumulateBlockAffinity(Ssa, Block, Eligible, Aff);

  struct Edge {
    uint32_t A, B, Score;
  };
  std::vector<Edge> Edges;
  for (const auto &KV : Aff) {
    if (KV.second.Dyn == 0 && KV.second.Const < 4)
      continue;
    Edges.push_back({static_cast<uint32_t>(KV.first >> 32),
                     static_cast<uint32_t>(KV.first),
                     KV.second.Dyn * 4u + KV.second.Const});
  }
  if (Edges.empty())
    return false;
  std::sort(Edges.begin(), Edges.end(), [](const Edge &X, const Edge &Y) {
    if (X.Score != Y.Score)
      return X.Score > Y.Score;
    return X.A != Y.A ? X.A < Y.A : X.B < Y.B;
  });

  std::vector<uint32_t> GroupId(NumX);
  std::vector<llvm::SmallVector<uint32_t, 4>> Groups(NumX);
  for (uint32_t X = 0; X < NumX; ++X) {
    GroupId[X] = X;
    Groups[X].push_back(X);
  }
  bool AnyMerged = false;
  for (const Edge &E : Edges) {
    uint32_t GA = GroupId[E.A], GB = GroupId[E.B];
    if (GA == GB || Groups[GA].size() + Groups[GB].size() > 4)
      continue;
    if (Ssa.Mir.IndexableTemps[E.A].Elements !=
        Ssa.Mir.IndexableTemps[E.B].Elements)
      continue;
    if (GA > GB)
      std::swap(GA, GB);
    for (uint32_t Member : Groups[GB]) {
      GroupId[Member] = GA;
      Groups[GA].push_back(Member);
    }
    Groups[GB].clear();
    AnyMerged = true;
  }
  if (!AnyMerged)
    return false;

  Plan.assign(NumX, {});
  NewDecls.clear();
  for (uint32_t G = 0; G < NumX; ++G) {
    auto &Members = Groups[G];
    if (Members.empty())
      continue;
    std::sort(Members.begin(), Members.end());
    const uint32_t NewId = static_cast<uint32_t>(NewDecls.size());
    const uint32_t Comps = Members.size() > 1
                               ? static_cast<uint32_t>(Members.size())
                               : Ssa.Mir.IndexableTemps[Members[0]].Components;
    NewDecls.push_back(
        {NewId, Ssa.Mir.IndexableTemps[Members[0]].Elements, Comps});
    for (uint8_t L = 0; L < Members.size(); ++L)
      Plan[Members[L]] = {NewId, L, Members.size() > 1};
  }
  return true;
}

void RewritePackedInst(SsaProgram &Ssa, SsaInst &Inst,
                       const std::vector<PackTarget> &Plan) {
  const size_t NumX = Plan.size();
  for (auto &Src : Inst.Sources) {
    if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX)
      continue;
    const PackTarget &T = Plan[Src.Index];
    Src.Index = T.NewX;
    if (T.Packed)
      Src.Swizzle = static_cast<uint8_t>(T.Lane * 0x55u);
  }
  if (Inst.Dest.Kind != OperandKind::IndexableTemp || Inst.Dest.Index >= NumX)
    return;
  const PackTarget &T = Plan[Inst.Dest.Index];
  Inst.Dest.Index = T.NewX;
  if (!T.Packed)
    return;
  Inst.Dest.Mask = static_cast<uint8_t>(1u << T.Lane);
  if (T.Lane == 0)
    return;
  Operand &Src = Inst.Sources[0];
  if (Src.Kind == OperandKind::ImmediateVector) {
    Src = Operand{OperandKind::Immediate,
                  Ssa.Mir.LiteralVectors[Src.Index][0]};
  } else if (Src.Kind != OperandKind::Immediate) {
    const uint8_t L0 = GetSwizzleLane(Src.Swizzle, 0);
    Src.Swizzle = static_cast<uint8_t>(L0 * 0x55u);
  }
}

void RewritePackedOperands(SsaProgram &Ssa, const std::vector<PackTarget> &Plan,
                           std::vector<IndexableTempDecl> &NewDecls) {
  for (auto &Block : Ssa.Blocks)
    for (auto &Inst : Block.Insts)
      if (!Inst.Dead)
        RewritePackedInst(Ssa, Inst, Plan);
  Ssa.Mir.IndexableTemps = std::move(NewDecls);
}

struct PendingLoadGroup {
  uint32_t NewX = 0;
  uint32_t Elem = 0;
  uint32_t RelReg = UINT32_MAX;
  uint8_t RelLane = 0;
  uint8_t LaneMask = 0;
  uint8_t Count = 0;
  std::array<uint32_t, 4> InstIdx{};
  std::array<uint32_t, 4> DstSsa{};
  std::array<uint8_t, 4> SrcLane{};
};

void FlushLoadGroup(SsaProgram &Ssa, SsaBlock &Block,
                    const PendingLoadGroup &G,
                    llvm::DenseMap<uint32_t, Operand> &Repl) {
  if (G.Count < 2)
    return;
  const uint32_t VecId = Ssa.NextValueId++;
  uint8_t Swiz = 0;
  for (uint8_t M = 0; M < G.Count; ++M)
    Swiz = SetSwizzleLane(Swiz, M, G.SrcLane[M]);
  auto &First = Block.Insts[G.InstIdx[0]];
  First.Dest = Operand{OperandKind::Temp, VecId, 0,
                       static_cast<uint8_t>((1u << G.Count) - 1u), 0xe4u};
  First.Sources[0].Swizzle = Swiz;
  for (uint8_t M = 1; M < G.Count; ++M)
    Block.Insts[G.InstIdx[M]].Dead = true;
  for (uint8_t M = 0; M < G.Count; ++M)
    Repl[G.DstSsa[M]] = Operand{OperandKind::Temp, VecId, 0, 1,
                                static_cast<uint8_t>(M * 0x55u)};
}

void ApplyScalarLoadRepls(SsaProgram &Ssa,
                          const llvm::DenseMap<uint32_t, Operand> &Repl) {
  auto RewriteRel = [&](Operand &Op) {
    if (Op.RelativeRegister == UINT32_MAX)
      return;
    auto It = Repl.find(Op.RelativeRegister);
    if (It == Repl.end())
      return;
    Op.RelativeRegister = It->second.Index;
    Op.RelativeLane = GetSwizzleLane(It->second.Swizzle, 0);
  };
  auto RewriteOp = [&](Operand &Op) {
    RewriteRel(Op);
    if (Op.Kind != OperandKind::Temp)
      return;
    auto It = Repl.find(Op.Index);
    if (It == Repl.end())
      return;
    const uint8_t Mod = Op.Modifier;
    Op = It->second;
    Op.Modifier = Mod;
  };
  for (auto &Block : Ssa.Blocks) {
    for (auto &Phi : Block.Phis)
      if (!Phi.Dead)
        for (auto &In : Phi.Inputs)
          RewriteOp(In.Value);
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
      RewriteOp(Block.Condition);
    for (auto &Inst : Block.Insts) {
      if (Inst.Dead)
        continue;
      RewriteRel(Inst.Dest);
      RewriteRel(Inst.AuxDest);
      for (auto &Src : Inst.Sources)
        RewriteOp(Src);
    }
  }
}

void VectorizeBlockDynamicLoads(SsaProgram &Ssa, SsaBlock &Block,
                                llvm::DenseMap<uint32_t, Operand> &Repl) {
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  llvm::SmallVector<PendingLoadGroup, 8> Active;
  auto FlushArray = [&](uint32_t X) {
    size_t Keep = 0;
    for (size_t K = 0; K < Active.size(); ++K) {
      if (Active[K].NewX == X)
        FlushLoadGroup(Ssa, Block, Active[K], Repl);
      else
        Active[Keep++] = Active[K];
    }
    Active.resize(Keep);
  };
  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    const auto &Inst = Block.Insts[I];
    if (Inst.Dead)
      continue;
    if (Inst.Dest.Kind == OperandKind::IndexableTemp &&
        Inst.Dest.Index < NumX) {
      FlushArray(Inst.Dest.Index);
      continue;
    }
    if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate ||
        Inst.Dest.Kind != OperandKind::Temp || Inst.Dest.Mask != 1u ||
        Inst.Sources.size() != 1)
      continue;
    const Operand &Src = Inst.Sources[0];
    if (Src.Kind != OperandKind::IndexableTemp || Src.Index >= NumX ||
        Src.RelativeRegister == UINT32_MAX || Src.Modifier != 0 ||
        Ssa.Mir.IndexableTemps[Src.Index].Components <= 1)
      continue;
    const uint8_t Lane = GetSwizzleLane(Src.Swizzle, 0);
    PendingLoadGroup *Match = nullptr;
    for (auto &G : Active) {
      if (G.NewX == Src.Index && G.Elem == Src.Element &&
          G.RelReg == Src.RelativeRegister &&
          G.RelLane == Src.RelativeLane && !(G.LaneMask & (1u << Lane))) {
        Match = &G;
        break;
      }
    }
    if (!Match) {
      Active.push_back(PendingLoadGroup{Src.Index, Src.Element,
                                        Src.RelativeRegister, Src.RelativeLane,
                                        0, 0});
      Match = &Active.back();
    }
    Match->InstIdx[Match->Count] = I;
    Match->DstSsa[Match->Count] = Inst.Dest.Index;
    Match->SrcLane[Match->Count] = Lane;
    Match->LaneMask |= static_cast<uint8_t>(1u << Lane);
    ++Match->Count;
  }
  for (const auto &G : Active)
    FlushLoadGroup(Ssa, Block, G, Repl);
}

void VectorizePackedDynamicLoads(SsaProgram &Ssa) {
  llvm::DenseMap<uint32_t, Operand> Repl;
  for (auto &Block : Ssa.Blocks)
    VectorizeBlockDynamicLoads(Ssa, Block, Repl);
  if (!Repl.empty())
    ApplyScalarLoadRepls(Ssa, Repl);
}

struct PendingStoreGroup {
  uint32_t NewX = 0;
  uint32_t Elem = 0;
  bool IsImm = false;
  Operand BaseSrc;
  uint8_t LaneMask = 0;
  uint8_t Count = 0;
  bool EmitAtAnchor = false;
  uint32_t AnchorIdx = 0;
  uint8_t BlockedLanes = 0;
  std::array<uint32_t, 4> InstIdx{};
  std::array<uint8_t, 4> DstLane{};
  std::array<Operand, 4> Srcs{};
};

void FlushStoreGroup(SsaProgram &Ssa, SsaBlock &Block,
                     const PendingStoreGroup &G) {
  if (G.Count < 2)
    return;
  const uint32_t Target = G.EmitAtAnchor ? G.AnchorIdx : G.InstIdx[G.Count - 1];
  for (uint8_t M = 0; M < G.Count; ++M)
    if (G.InstIdx[M] != Target)
      Block.Insts[G.InstIdx[M]].Dead = true;
  Block.Insts[Target].Dest.Mask = G.LaneMask;
  if (G.IsImm) {
    std::array<uint32_t, 4> Bits = {0, 0, 0, 0};
    for (uint8_t M = 0; M < G.Count; ++M)
      Bits[G.DstLane[M]] = G.Srcs[M].Index;
    const uint32_t LitId =
        static_cast<uint32_t>(Ssa.Mir.LiteralVectors.size());
    Ssa.Mir.LiteralVectors.push_back(Bits);
    Block.Insts[Target].Sources[0] =
        Operand{OperandKind::ImmediateVector, LitId, 0, 0xfu, 0xe4u};
    return;
  }
  Operand VecSrc = G.BaseSrc;
  VecSrc.Swizzle = 0;
  for (uint8_t M = 0; M < G.Count; ++M)
    VecSrc.Swizzle = SetSwizzleLane(VecSrc.Swizzle, G.DstLane[M],
                                    GetSwizzleLane(G.Srcs[M].Swizzle, 0));
  Block.Insts[Target].Sources[0] = VecSrc;
}

void FlushStoresMatching(SsaProgram &Ssa, SsaBlock &Block,
                         llvm::SmallVectorImpl<PendingStoreGroup> &Active,
                         uint32_t X, uint32_t Elem = UINT32_MAX) {
  size_t Keep = 0;
  for (size_t K = 0; K < Active.size(); ++K) {
    if (Active[K].NewX == X && (Elem == UINT32_MAX || Active[K].Elem == Elem))
      FlushStoreGroup(Ssa, Block, Active[K]);
    else
      Active[Keep++] = Active[K];
  }
  Active.resize(Keep);
}

uint8_t ComputeIndexableReadLanes(const SsaInst &Inst, size_t SrcIdx,
                                  const Operand &Src) {
  if (IsPseudoOpcode(Inst.Opcode))
    return 0xfu;
  uint8_t DMask = Inst.Dest.Mask;
  if (Inst.AuxDest.Kind != OperandKind::Null)
    DMask |= Inst.AuxDest.Mask;
  return InstructionSourceReadLanes(Inst.Opcode, DMask, SrcIdx, Src.Swizzle);
}

bool IsSrcAvailableAt(uint32_t BlockId, const std::vector<SsaDefSite> &Defs,
                      const Operand &Src, uint32_t TargetIdx) {
  if (Src.RelativeRegister != UINT32_MAX)
    return false;
  if (Src.Kind != OperandKind::Temp)
    return true;
  if (Src.Index >= Defs.size() || !Defs[Src.Index].Valid())
    return false;
  const auto &D = Defs[Src.Index];
  return D.Block != BlockId || D.IsPhi || D.Index < TargetIdx;
}

void NoteArrayRead(llvm::SmallVectorImpl<PendingStoreGroup> &Active,
                   const SsaInst &Inst, size_t SrcIdx, const Operand &Src) {
  const uint8_t RL = ComputeIndexableReadLanes(Inst, SrcIdx, Src);
  for (auto &G : Active) {
    if (G.NewX != Src.Index)
      continue;
    if (G.EmitAtAnchor) {
      G.BlockedLanes |= RL;
    } else if ((G.LaneMask & RL) != 0) {
      G.EmitAtAnchor = true;
      G.AnchorIdx = G.InstIdx[G.Count - 1];
      G.BlockedLanes = RL;
    }
  }
}

void VectorizeBlockConstStores(SsaProgram &Ssa, uint32_t BlockId,
                               const std::vector<SsaDefSite> &Defs) {
  SsaBlock &Block = Ssa.Blocks[BlockId];
  const size_t NumX = Ssa.Mir.IndexableTemps.size();
  llvm::SmallVector<PendingStoreGroup, 16> Active;
  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    const auto &Inst = Block.Insts[I];
    if (Inst.Dead)
      continue;
    for (size_t SI = 0; SI < Inst.Sources.size(); ++SI)
      if (Inst.Sources[SI].Kind == OperandKind::IndexableTemp &&
          Inst.Sources[SI].Index < NumX)
        NoteArrayRead(Active, Inst, SI, Inst.Sources[SI]);
    if (Inst.Dest.Kind != OperandKind::IndexableTemp || Inst.Dest.Index >= NumX)
      continue;
    const uint32_t X = Inst.Dest.Index;
    if (Ssa.Mir.IndexableTemps[X].Components <= 1)
      continue;
    if (Inst.Dest.RelativeRegister != UINT32_MAX ||
        Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate ||
        Inst.Sources.size() != 1 || !llvm::isPowerOf2_32(Inst.Dest.Mask)) {
      FlushStoresMatching(Ssa, Block, Active, X);
      continue;
    }
    const uint8_t Lane =
        static_cast<uint8_t>(llvm::countTrailingZeros(Inst.Dest.Mask));
    const uint32_t Elem = Inst.Dest.Element;
    const Operand &Src = Inst.Sources[0];
    const bool IsImm = Src.Kind == OperandKind::Immediate && Src.Modifier == 0;
    if (!IsImm && !IsPackableStoreSrc(Src)) {
      FlushStoresMatching(Ssa, Block, Active, X, Elem);
      continue;
    }
    for (const auto &G : Active) {
      if (G.NewX == X && G.Elem == Elem && (G.LaneMask & (1u << Lane))) {
        FlushStoresMatching(Ssa, Block, Active, X, Elem);
        break;
      }
    }
    PendingStoreGroup *Match = nullptr;
    for (auto &G : Active) {
      if (G.NewX == X && G.Elem == Elem && G.IsImm == IsImm &&
          (IsImm || SameStoreBase(G.BaseSrc, Src)) &&
          (!G.EmitAtAnchor ||
           (!(G.BlockedLanes & (1u << Lane)) &&
            IsSrcAvailableAt(BlockId, Defs, Src, G.AnchorIdx)))) {
        Match = &G;
        break;
      }
    }
    if (!Match) {
      for (auto &G : Active)
        if (G.NewX == X && G.Elem == Elem && G.EmitAtAnchor)
          G.BlockedLanes |= static_cast<uint8_t>(1u << Lane);
      Active.push_back(PendingStoreGroup{X, Elem, IsImm, Src, 0, 0});
      Match = &Active.back();
    }
    Match->InstIdx[Match->Count] = I;
    Match->DstLane[Match->Count] = Lane;
    Match->Srcs[Match->Count] = Src;
    Match->LaneMask |= static_cast<uint8_t>(1u << Lane);
    ++Match->Count;
  }
  for (const auto &G : Active)
    FlushStoreGroup(Ssa, Block, G);
}

void VectorizePackedConstStores(SsaProgram &Ssa) {
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B)
    VectorizeBlockConstStores(Ssa, B, Defs);
}

} // namespace

bool dxcb5::RunOptIndexablePack(SsaProgram &Ssa) {
  if (Ssa.Mir.IndexableTemps.size() < 2)
    return false;
  std::vector<uint8_t> Eligible;
  CollectEligibleArrays(Ssa, Eligible);
  std::vector<PackTarget> Plan;
  std::vector<IndexableTempDecl> NewDecls;
  if (!BuildPackPlan(Ssa, Eligible, Plan, NewDecls))
    return false;
  RewritePackedOperands(Ssa, Plan, NewDecls);
  VectorizePackedDynamicLoads(Ssa);
  VectorizePackedConstStores(Ssa);
  Ssa.CompactDead();
  return true;
}
