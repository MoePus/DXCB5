// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "Mir/OptSlpCommon.h"
#include "llvm/Support/TimeProfiler.h"

using namespace dxcb5;
using namespace dxcb5::slp;

namespace {

// Tight local scheduling window for 2-wide float ALU avoids splitting 3-wide
// trees or stretching live ranges on scalar SIMT GPUs. Genuine 3..4-wide float
// vector/matrix chains, derivative pairs, and related integer hash/noise chains
// may pack wider.
constexpr size_t kMaxFloatAluWindow = 3;
constexpr size_t kMaxSharedTempPairWindow = 8;
constexpr size_t kMaxFloatVec34Window = 16;
constexpr size_t kMaxZeroRegPairWindow = 48;
constexpr size_t kMaxDerivWindow = 24;
constexpr size_t kMaxIntRelatedWindow = 320;

bool IsIntBitwiseOp(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_IADD:
  case D3D10_SB_OPCODE_IMAD:
  case D3D10_SB_OPCODE_UMAD:
  case D3D10_SB_OPCODE_IMUL:
  case D3D10_SB_OPCODE_XOR:
  case D3D10_SB_OPCODE_AND:
  case D3D10_SB_OPCODE_OR:
  case D3D10_SB_OPCODE_NOT:
  case D3D10_SB_OPCODE_ISHL:
  case D3D10_SB_OPCODE_ISHR:
  case D3D10_SB_OPCODE_USHR:
  case D3D11_SB_OPCODE_BFI:
  case D3D11_SB_OPCODE_IBFE:
  case D3D11_SB_OPCODE_UBFE:
  case D3D10_SB_OPCODE_FTOI:
  case D3D10_SB_OPCODE_FTOU:
  case D3D10_SB_OPCODE_ITOF:
  case D3D10_SB_OPCODE_UTOF:
  case D3D10_SB_OPCODE_ROUND_NI:
  case D3D10_SB_OPCODE_ROUND_NE:
  case D3D10_SB_OPCODE_ROUND_PI:
  case D3D10_SB_OPCODE_ROUND_Z:
    return true;
  default:
    return false;
  }
}

bool IsCommutativePairOp(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_ADD:
  case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_MIN:
  case D3D10_SB_OPCODE_MAX:
  case D3D10_SB_OPCODE_EQ:
  case D3D10_SB_OPCODE_NE:
  case D3D10_SB_OPCODE_IADD:
  case D3D10_SB_OPCODE_IMUL:
  case D3D10_SB_OPCODE_IMIN:
  case D3D10_SB_OPCODE_IMAX:
  case D3D10_SB_OPCODE_UMIN:
  case D3D10_SB_OPCODE_UMAX:
  case D3D10_SB_OPCODE_IEQ:
  case D3D10_SB_OPCODE_INE:
  case D3D10_SB_OPCODE_AND:
  case D3D10_SB_OPCODE_OR:
  case D3D10_SB_OPCODE_XOR:
  case D3D10_SB_OPCODE_MAD:
  case D3D10_SB_OPCODE_IMAD:
  case D3D10_SB_OPCODE_UMAD:
    return true;
  default:
    return false;
  }
}

bool IsMatrixCompareAlu(uint32_t Opcode) {
  switch (Opcode) {
  case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_MAD:
  case D3D10_SB_OPCODE_ADD:
  case D3D10_SB_OPCODE_IEQ:
  case D3D10_SB_OPCODE_INE:
  case D3D10_SB_OPCODE_EQ:
  case D3D10_SB_OPCODE_NE:
    return true;
  default:
    return false;
  }
}

bool FeedsNearbyIntConvert(const SsaBlock &Block, uint32_t InstIdx,
                           uint32_t DestId) {
  if (Block.Insts[InstIdx].Opcode != D3D10_SB_OPCODE_ADD)
    return false;
  const size_t End = std::min<size_t>(Block.Insts.size(), InstIdx + 5);
  for (size_t K = InstIdx + 1; K < End; ++K) {
    const auto &U = Block.Insts[K];
    if (U.Dead || U.Sources.empty())
      continue;
    if ((U.Opcode == D3D10_SB_OPCODE_FTOI ||
         U.Opcode == D3D10_SB_OPCODE_FTOU) &&
        U.Sources[0].Kind == OperandKind::Temp &&
        U.Sources[0].Index == DestId)
      return true;
  }
  return false;
}

struct CompatInfo {
  bool SharesTempOrInput = false;
  bool SharesTemp = false;
  bool ReadsDistinctTempLanes = false;
  bool ZeroNewLiveRegs = false;
};

bool CheckSourcesCompatible(const SsaInst &First, const SsaInst &Next,
                            bool Swap01, CompatInfo &Info) {
  Info = {};
  bool AllNonImmIdentical = true;
  unsigned SharedTemps = 0;
  for (size_t S = 0; S < First.Sources.size(); ++S) {
    const size_t NextS = (Swap01 && S < 2) ? (1 - S) : S;
    const auto &A = First.Sources[S];
    const auto &C = Next.Sources[NextS];
    const bool ImmA = (A.Kind == OperandKind::Immediate ||
                       A.Kind == OperandKind::ImmediateVector) &&
                      !A.Modifier;
    const bool ImmC = (C.Kind == OperandKind::Immediate ||
                       C.Kind == OperandKind::ImmediateVector) &&
                      !C.Modifier;
    if (ImmA && ImmC)
      continue;
    if (!SameOperandBase(A, C))
      return false;
    if (A.Kind == OperandKind::Temp || A.Kind == OperandKind::Input)
      Info.SharesTempOrInput = true;
    if (A.Kind == OperandKind::Temp)
      ++SharedTemps;
    if (A.Swizzle != C.Swizzle) {
      AllNonImmIdentical = false;
      if (A.Kind == OperandKind::Temp)
        Info.ReadsDistinctTempLanes = true;
    }
  }
  Info.SharesTemp = SharedTemps >= 1;
  Info.ZeroNewLiveRegs = AllNonImmIdentical && SharedTemps >= 1;
  return true;
}

struct FamilyCandidate {
  llvm::SmallVector<uint32_t, 4> Group;
  llvm::SmallVector<uint8_t, 4> Swap;
  uint32_t Anchor = 0;
};

uint64_t ComputeShapeKey(const SsaInst &Inst) {
  return (uint64_t(Inst.Opcode) << 12) |
         (uint64_t(Inst.Sources.size()) << 3) | (Inst.Saturate ? 1u : 0u) |
         (Inst.Precise ? 2u : 0u) | (IsImulLow(Inst) ? 4u : 0u);
}

bool HasTempOrInputSource(const SsaInst &Inst) {
  for (const auto &Src : Inst.Sources)
    if (Src.Kind == OperandKind::Temp || Src.Kind == OperandKind::Input)
      return true;
  return false;
}

struct BlockPackIndex {
  std::vector<uint32_t> SlpBarPref;
  std::vector<uint32_t> BarPref;
  llvm::DenseMap<uint64_t, llvm::SmallVector<uint32_t, 8>> Buckets;
  std::vector<uint32_t> PosInBucket;

  void Build(const SsaBlock &Block, uint32_t BlockId,
             const std::vector<SsaDefSite> &Defs,
             const std::vector<uint8_t> &Escapes) {
    const size_t N = Block.Insts.size();
    SlpBarPref.assign(N + 1, 0);
    BarPref.assign(N + 1, 0);
    Buckets.clear();
    PosInBucket.assign(N, UINT32_MAX);
    std::vector<uint8_t> DotRedexMul;
    FindDotRedexMuls(Block, Defs, BlockId, Escapes, DotRedexMul);
    for (uint32_t I = 0; I < N; ++I) {
      const auto &Inst = Block.Insts[I];
      SlpBarPref[I + 1] = SlpBarPref[I] + (!Inst.Dead && IsSlpBarrier(Inst));
      BarPref[I + 1] = BarPref[I] + (!Inst.Dead && Inst.IsBarrier());
      if (Inst.Dead || DotRedexMul[I] || !IsVectorizableInst(Inst) ||
          PackDest(Inst).Kind != OperandKind::Temp)
        continue;
      auto &B = Buckets[ComputeShapeKey(Inst)];
      PosInBucket[I] = static_cast<uint32_t>(B.size());
      B.push_back(I);
    }
  }
};

bool CollectFamilyGroup(const SsaBlock &Block, uint32_t BlockId, uint32_t Start,
                        const BlockPackIndex &Idx,
                        const std::vector<SsaDefSite> &Defs,
                        const FirstLocalUseCache &UseCache,
                        const std::vector<uint8_t> &Used,
                        FamilyCandidate &Out) {
  const auto &First = Block.Insts[Start];
  const auto &FirstDest = PackDest(First);
  unsigned TotalLanes = PopcountMask(FirstDest.Mask);
  if (TotalLanes >= kRegisterLanes)
    return false;
  auto BIt = Idx.Buckets.find(ComputeShapeKey(First));
  if (BIt == Idx.Buckets.end() || Idx.PosInBucket[Start] + 1 >= BIt->second.size())
    return false;

  const bool Comm = IsCommutativePairOp(First.Opcode) &&
                    First.Sources.size() >= 2;
  const bool IsDeriv = InstructionIsDerivative(First.Opcode);
  const bool IsIntOp = IsIntBitwiseOp(First.Opcode) ||
                       FeedsNearbyIntConvert(Block, Start, FirstDest.Index);
  const bool IsMatAlu = IsMatrixCompareAlu(First.Opcode);
  const size_t Win = IsDeriv ? kMaxDerivWindow : kMaxFloatAluWindow;
  const size_t MaxWin =
      HasTempOrInputSource(First)
          ? kMaxIntRelatedWindow
          : (std::max(Win, IsMatAlu ? kMaxFloatVec34Window : Win) + 1);
  const uint32_t Limit = static_cast<uint32_t>(
      std::min<size_t>(Block.Insts.size(), Start + MaxWin));
  const auto &ScanBar = IsDeriv ? Idx.SlpBarPref : Idx.BarPref;
  const uint32_t StartBar = ScanBar[Start + 1];
  const auto &Bucket = BIt->second;

  Out.Group = {Start};
  Out.Swap = {0};
  bool AnyDistinctTempLane = false;
  bool AllZeroNewLiveRegs = true;
  bool AllShareTemp = true;

  for (size_t P = Idx.PosInBucket[Start] + 1;
       P < Bucket.size() && TotalLanes < kRegisterLanes; ++P) {
    const uint32_t J = Bucket[P];
    if (J >= Limit || ScanBar[J + 1] != StartBar)
      break;
    if (Used[J] || Block.Insts[J].Dead)
      continue;
    const auto &Next = Block.Insts[J];
    const unsigned NextLanes = PopcountMask(PackDest(Next).Mask);
    if (TotalLanes + NextLanes > kRegisterLanes)
      continue;

    CompatInfo Info;
    bool Swap01 = false;
    bool Compat = CheckSourcesCompatible(First, Next, false, Info);
    if (!Compat && Comm) {
      Compat = CheckSourcesCompatible(First, Next, true, Info);
      Swap01 = Compat;
    }
    if (!Compat)
      continue;
    if (J - Start > Win && !Info.SharesTempOrInput &&
        !(IsMatAlu && (J - Start) <= kMaxFloatVec34Window))
      continue;

    llvm::SmallVector<uint32_t, 4> Trial = Out.Group;
    Trial.push_back(J);
    uint32_t Anchor = 0;
    if (!FindPackAnchor(Block, Defs, BlockId, Trial, UseCache, Anchor,
                        &Idx.SlpBarPref))
      continue;
    Out.Group = std::move(Trial);
    Out.Swap.push_back(Swap01 ? 1u : 0u);
    Out.Anchor = Anchor;
    TotalLanes += NextLanes;
    AnyDistinctTempLane |= Info.ReadsDistinctTempLanes;
    AllZeroNewLiveRegs &= Info.ZeroNewLiveRegs;
    AllShareTemp &= Info.SharesTemp;
  }

  if (Out.Group.size() < 2)
    return false;
  const size_t Span = Out.Group.back() - Out.Group.front();
  const bool AllowZeroRegPair =
      AllZeroNewLiveRegs && Span <= kMaxZeroRegPairWindow;
  const bool AllowSharedTempPair =
      AllShareTemp && Span <= kMaxSharedTempPairWindow;
  if (!IsDeriv && !IsIntOp && !AnyDistinctTempLane && !AllowZeroRegPair &&
      !AllowSharedTempPair && TotalLanes < 3 && Span > Win)
    return false;
  return true;
}

bool TryEmitFamilyPack(SsaProgram &Ssa, SsaBlock &Block,
                       const FamilyCandidate &Cand, std::vector<uint8_t> &Used,
                       llvm::DenseMap<uint32_t, LaneRemap> &AllRemaps,
                       SsaInst &OutPacked) {
  for (size_t G = 0; G < Cand.Group.size(); ++G)
    if (Cand.Swap[G])
      std::swap(Block.Insts[Cand.Group[G]].Sources[0],
                Block.Insts[Cand.Group[G]].Sources[1]);

  llvm::SmallVector<PackedSlot, 4> Slots;
  uint8_t NextNewLane = 0;
  for (uint32_t Idx : Cand.Group) {
    PackedSlot S;
    S.InstIdx = Idx;
    S.SrcMask = PackDest(Block.Insts[Idx]).Mask;
    for (unsigned OldL = 0; OldL < kRegisterLanes; ++OldL)
      if (S.SrcMask & (1u << OldL))
        S.OldToNew[OldL] = NextNewLane++;
    Slots.push_back(S);
  }

  const auto &First = Block.Insts[Cand.Group[0]];
  llvm::SmallVector<Operand, 5> MergedSources;
  bool Ok = true;
  for (size_t S = 0; S < First.Sources.size(); ++S) {
    Operand M{OperandKind::Null};
    if (!BuildMergedOperand(Ssa, Block, Slots, S, M, nullptr)) {
      Ok = false;
      break;
    }
    MergedSources.push_back(M);
  }
  if (!Ok) {
    for (size_t G = 0; G < Cand.Group.size(); ++G)
      if (Cand.Swap[G])
        std::swap(Block.Insts[Cand.Group[G]].Sources[0],
                  Block.Insts[Cand.Group[G]].Sources[1]);
    return false;
  }

  const uint32_t NewId = Ssa.AllocValue();
  OutPacked = First;
  PackDest(OutPacked) =
      MakeSsaTemp(NewId, MaskForWidth(NextNewLane), kIdentitySwizzle);
  OutPacked.Sources = std::move(MergedSources);

  for (const auto &Slot : Slots) {
    const uint32_t OldId = PackDest(Block.Insts[Slot.InstIdx]).Index;
    Block.Insts[Slot.InstIdx].Dead = true;
    Used[Slot.InstIdx] = 1;
    AllRemaps[OldId] = {NewId, Slot.OldToNew};
  }
  return true;
}

struct PackInsertion {
  uint32_t Anchor = 0;
  SsaInst Inst;
};

bool RunSameFamilyPackBlock(SsaProgram &Ssa, uint32_t BlockId,
                            const std::vector<SsaDefSite> &Defs,
                            const std::vector<uint8_t> &Escapes,
                            FirstLocalUseCache &UseCache,
                            BlockPackIndex &PackIdx,
                            llvm::DenseMap<uint32_t, LaneRemap> &AllRemaps) {
  auto &Block = Ssa.Blocks[BlockId];
  if (Block.Insts.size() < 2)
    return false;
  PackIdx.Build(Block, BlockId, Defs, Escapes);
  if (PackIdx.Buckets.empty())
    return false;
  UseCache.Reset(Block, Ssa.NextValueId);
  std::vector<uint8_t> Used(Block.Insts.size(), 0);
  llvm::SmallVector<PackInsertion, 16> Insertions;

  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    if (Used[I] || PackIdx.PosInBucket[I] == UINT32_MAX || Block.Insts[I].Dead)
      continue;
    FamilyCandidate Cand;
    if (!CollectFamilyGroup(Block, BlockId, I, PackIdx, Defs, UseCache, Used,
                            Cand))
      continue;
    SsaInst Packed;
    if (TryEmitFamilyPack(Ssa, Block, Cand, Used, AllRemaps, Packed))
      Insertions.push_back({Cand.Anchor, std::move(Packed)});
  }

  if (Insertions.empty())
    return false;
  std::stable_sort(Insertions.begin(), Insertions.end(),
                   [](const PackInsertion &A, const PackInsertion &B) {
                     return A.Anchor < B.Anchor;
                   });
  std::vector<SsaInst> Rebuilt;
  Rebuilt.reserve(Block.Insts.size());
  size_t InsIdx = 0;
  for (size_t I = 0; I <= Block.Insts.size(); ++I) {
    while (InsIdx < Insertions.size() && Insertions[InsIdx].Anchor == I)
      Rebuilt.push_back(std::move(Insertions[InsIdx++].Inst));
    if (I < Block.Insts.size() && !Block.Insts[I].Dead)
      Rebuilt.push_back(std::move(Block.Insts[I]));
  }
  Block.Insts = std::move(Rebuilt);
  return true;
}

void BuildEscapesTable(const SsaProgram &Ssa,
                       const std::vector<SsaDefSite> &Defs,
                       std::vector<uint8_t> &Escapes) {
  Escapes.assign(Ssa.NextValueId, 0);
  auto MarkIfCross = [&](uint32_t Id, uint32_t UseBlock, bool Force) {
    if (Id >= Escapes.size()) return;
    if (Force || !Defs[Id].Valid() || Defs[Id].Block != UseBlock)
      Escapes[Id] = 1;
  };
  auto MarkOp = [&](const Operand &Op, uint32_t UseBlock, bool Force) {
    if (Op.Kind == OperandKind::Temp)
      MarkIfCross(Op.Index, UseBlock, Force);
    if (Op.RelativeRegister != UINT32_MAX)
      MarkIfCross(Op.RelativeRegister, UseBlock, Force);
  };
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    const auto &Block = Ssa.Blocks[B];
    for (const auto &Phi : Block.Phis) {
      if (Phi.Dead) continue;
      for (const auto &In : Phi.Inputs)
        MarkOp(In.Value, B, true);
    }
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead) continue;
      if (Inst.Dest.RelativeRegister != UINT32_MAX)
        MarkIfCross(Inst.Dest.RelativeRegister, B, false);
      for (const auto &Src : Inst.Sources)
        MarkOp(Src, B, false);
    }
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
      MarkOp(Block.Condition, B, false);
  }
}

void RefreshDefsForModified(SsaProgram &Ssa,
                            const llvm::SmallVectorImpl<uint32_t> &Modified,
                            const llvm::DenseMap<uint32_t, LaneRemap> &Remaps,
                            std::vector<SsaDefSite> &Defs) {
  Defs.resize(Ssa.NextValueId);
  for (const auto &KV : Remaps)
    if (KV.first < Defs.size())
      Defs[KV.first] = SsaDefSite{};
  for (uint32_t B : Modified) {
    const auto &Insts = Ssa.Blocks[B].Insts;
    for (uint32_t I = 0; I < Insts.size(); ++I) {
      const auto &Inst = Insts[I];
      if (Inst.Dest.Kind == OperandKind::Temp && Inst.Dest.Index)
        Defs[Inst.Dest.Index] = {B, I, false, false};
      if (Inst.AuxDest.Kind == OperandKind::Temp && Inst.AuxDest.Index)
        Defs[Inst.AuxDest.Index] = {B, I, false, true};
    }
  }
}

bool RunSameFamilyPack(SsaProgram &Ssa, FirstLocalUseCache &UseCache,
                       RemapTable &RemapCache) {
  bool AnyChanged = false;
  std::vector<uint8_t> Dirty(Ssa.Blocks.size(), 1);
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<uint8_t> Escapes;
  BuildEscapesTable(Ssa, Defs, Escapes);
  BlockPackIndex PackIdx;

  for (unsigned Iter = 0; Iter < 40; ++Iter) {
    llvm::DenseMap<uint32_t, LaneRemap> AllRemaps;
    std::vector<uint8_t> NextDirty(Ssa.Blocks.size(), 0);
    llvm::SmallVector<uint32_t, 16> ModifiedBlocks;
    for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
      if (!Dirty[B])
        continue;
      if (RunSameFamilyPackBlock(Ssa, B, Defs, Escapes, UseCache, PackIdx,
                                 AllRemaps)) {
        ModifiedBlocks.push_back(B);
        NextDirty[B] = 1;
      }
    }
    if (ModifiedBlocks.empty())
      break;
    Escapes.resize(Ssa.NextValueId, 0);
    bool AnyEscaped = false;
    for (const auto &KV : AllRemaps) {
      if (KV.first < Escapes.size() && Escapes[KV.first]) {
        AnyEscaped = true;
        if (KV.second.NewId < Escapes.size())
          Escapes[KV.second.NewId] = 1;
      }
    }
    llvm::ArrayRef<uint32_t> Scope =
        AnyEscaped ? llvm::ArrayRef<uint32_t>{}
                   : llvm::ArrayRef<uint32_t>(ModifiedBlocks);
    ApplyLaneRemaps(Ssa, AllRemaps, RemapCache, &NextDirty, Scope);
    RefreshDefsForModified(Ssa, ModifiedBlocks, AllRemaps, Defs);
    AnyChanged = true;
    Dirty = std::move(NextDirty);
  }
  return AnyChanged;
}

} // namespace

bool dxcb5::RunOptSlpVectorize(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt SLP Vectorize", llvm::StringRef());
  FirstLocalUseCache UseCache;
  RemapTable RemapCache;
  bool Changed = false;
  Changed |= GroupScalarPhis(Ssa, RemapCache);
  Changed |= RunTreeSlp(Ssa, UseCache, RemapCache);
  Changed |= RunSameFamilyPack(Ssa, UseCache, RemapCache);
  return Changed;
}
