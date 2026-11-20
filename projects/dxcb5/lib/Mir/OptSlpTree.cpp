// Distributed under the University of Illinois Open Source License.
#include "Mir/OptSlpCommon.h"
#include "llvm/ADT/DenseSet.h"

using namespace dxcb5;
using namespace dxcb5::slp;

namespace {

void CollectPerLaneOperands(const SsaProgram &Ssa, const SsaBlock &Block,
                            llvm::ArrayRef<PackedSlot> Slots, size_t SrcSlot,
                            std::array<Operand, 4> &PerLane,
                            uint8_t &ActiveMask) {
  ActiveMask = 0;
  for (const auto &Slot : Slots) {
    const auto &Op = Block.Insts[Slot.InstIdx].Sources[SrcSlot];
    for (unsigned OldL = 0; OldL < kRegisterLanes; ++OldL) {
      if (!(Slot.SrcMask & (1u << OldL)))
        continue;
      const unsigned NewL = Slot.OldToNew[OldL];
      ActiveMask |= static_cast<uint8_t>(1u << NewL);
      if (Op.Kind == OperandKind::Immediate) {
        PerLane[NewL] = Op;
      } else if (Op.Kind == OperandKind::ImmediateVector) {
        const unsigned SrcL = GetSwizzleLane(Op.Swizzle, OldL);
        PerLane[NewL] = MakeImm(Ssa.Mir.LiteralVectors[Op.Index][SrcL]);
      } else {
        Operand Single = Op;
        Single.Mask = 1;
        Single.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(Op.Swizzle, OldL));
        PerLane[NewL] = Single;
      }
    }
  }
}

bool TryMergeImmediates(SsaProgram &Ssa, const std::array<Operand, 4> &PerLane,
                        uint8_t ActiveMask, unsigned FirstLane, Operand &Out) {
  bool SameImm = true;
  std::array<uint32_t, 4> Bits{};
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    if (!(ActiveMask & (1u << L))) continue;
    if (PerLane[L].Kind != OperandKind::Immediate || PerLane[L].Modifier)
      return false;
    Bits[L] = PerLane[L].Index;
    if (L != FirstLane && Bits[L] != Bits[FirstLane]) SameImm = false;
  }
  Out = SameImm ? MakeImm(Bits[FirstLane])
                : Operand{OperandKind::ImmediateVector,
                          Ssa.InternLiteralVector(Bits), 0, ActiveMask,
                          kIdentitySwizzle};
  return true;
}

bool TryMergeSameBase(const std::array<Operand, 4> &PerLane, uint8_t ActiveMask,
                      unsigned FirstLane, Operand &Out) {
  const auto &Base = PerLane[FirstLane];
  if (Base.Kind != OperandKind::Temp && Base.Kind != OperandKind::Input &&
      Base.Kind != OperandKind::ConstantBuffer) return false;
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if ((ActiveMask & (1u << L)) && !SameOperandBase(Base, PerLane[L]))
      return false;
  Out = Base;
  Out.Mask = ActiveMask;
  uint8_t Swz = 0;
  const unsigned Fallback = GetSwizzleLane(Base.Swizzle, 0);
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    Swz = SetSwizzleLane(Swz, L, (ActiveMask & (1u << L))
                                     ? GetSwizzleLane(PerLane[L].Swizzle, 0)
                                     : Fallback);
  Out.Swizzle = Swz;
  return true;
}

bool SynthesizeCombineOperand(SsaProgram &Ssa, uint32_t Opcode, size_t SrcSlot,
                              const std::array<Operand, 4> &PerLane,
                              uint8_t ActiveMask, unsigned FirstLane,
                              Operand &Out,
                              llvm::SmallVectorImpl<SsaInst> &EmitCombines) {
  const bool IntNeg = InstructionAcceptsIntNegModifier(Opcode);
  const bool FloatMod = InstructionOperandAcceptsFloatModifiers(Opcode, SrcSlot);
  const uint8_t Mod = PerLane[FirstLane].Modifier;
  if (Mod != 0 && (IntNeg ? Mod != D3D10_SB_OPERAND_MODIFIER_NEG : !FloatMod))
    return false;
  unsigned Width = 0;
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if (ActiveMask & (1u << L))
      Width = L + 1;
  llvm::SmallVector<Operand, 4> CombLanes(Width, MakeImm(0));
  for (unsigned L = 0; L < Width; ++L) {
    if (!(ActiveMask & (1u << L))) continue;
    Operand Raw = PerLane[L];
    if (Raw.Modifier != Mod) {
      if (Raw.Kind != OperandKind::Immediate || Raw.Modifier != 0 ||
          Mod != D3D10_SB_OPERAND_MODIFIER_NEG) return false;
      Raw.Index = IntNeg ? (0u - Raw.Index) : (Raw.Index ^ kFloatSignBit);
    }
    Raw.Modifier = 0;
    CombLanes[L] = Raw;
  }
  const Operand CombDest =
      MakeSsaTemp(Ssa.AllocValue(), MaskForWidth(Width), kIdentitySwizzle);
  SsaInst Comb;
  Comb.Opcode = PseudoCombineVec;
  Comb.Dest = CombDest;
  Comb.Sources.append(CombLanes.begin(), CombLanes.end());
  EmitCombines.push_back(std::move(Comb));
  Out = CombDest;
  Out.Modifier = Mod;
  return true;
}

bool IsPhiTupleEligible(const SsaProgram &Ssa,
                        const std::vector<SsaDefSite> &Defs,
                        const llvm::DenseSet<uint32_t> &Claimed,
                        llvm::ArrayRef<uint32_t> PhiIds) {
  if (PhiIds.size() < 2 || PhiIds.size() > 4) return false;
  const uint32_t HomeBlock = Defs[PhiIds[0]].Block;
  for (size_t I = 0; I < PhiIds.size(); ++I) {
    const uint32_t Id = PhiIds[I];
    if (!Defs[Id].Valid() || !Defs[Id].IsPhi || Defs[Id].Block != HomeBlock ||
        Claimed.count(Id)) return false;
    const auto &Phi = Ssa.Blocks[HomeBlock].Phis[Defs[Id].Index];
    if (Phi.Dead || Phi.Mask != 1u) return false;
    for (size_t J = 0; J < I; ++J)
      if (PhiIds[J] == Id) return false;
  }
  const auto &First = Ssa.Blocks[HomeBlock].Phis[Defs[PhiIds[0]].Index];
  for (size_t I = 1; I < PhiIds.size(); ++I) {
    const auto &Other = Ssa.Blocks[HomeBlock].Phis[Defs[PhiIds[I]].Index];
    if (Other.Inputs.size() != First.Inputs.size()) return false;
    for (size_t E = 0; E < First.Inputs.size(); ++E)
      if (Other.Inputs[E].PredBlock != First.Inputs[E].PredBlock) return false;
  }
  return true;
}

uint64_t ComputePhiEdgeTag(const SsaProgram &Ssa,
                           const std::vector<SsaDefSite> &Defs,
                           const Operand &Val, bool &HasInteresting) {
  if (Val.Kind == OperandKind::Input || Val.Kind == OperandKind::ConstantBuffer) {
    HasInteresting = true;
    return (uint64_t(Val.Kind) << 40) ^ (uint64_t(Val.Index) << 16) ^ Val.Element;
  }
  if (Val.Kind != OperandKind::Temp || Val.RelativeRegister != UINT32_MAX) return 0;
  const auto &D = Defs[Val.Index];
  if (!D.Valid()) return 0;
  if (D.IsPhi) {
    const auto &SrcPhi = Ssa.Blocks[D.Block].Phis[D.Index];
    if (SrcPhi.Dead || PopcountMask(SrcPhi.Mask) <= 1) return 0;
    HasInteresting = true;
    return (2ULL << 48) ^ (uint64_t(Val.Index) << 8);
  }
  if (D.IsAux) return 0;
  const auto &Prod = Ssa.Blocks[D.Block].Insts[D.Index];
  if (Prod.Dead) return 0;
  if (PopcountMask(Prod.Dest.Mask) > 1) {
    HasInteresting = true;
    return (1ULL << 48) ^ (uint64_t(Val.Index) << 8);
  }
  if (!IsVectorizableInst(Prod)) return 0;
  HasInteresting = true;
  return (uint64_t(Prod.Opcode) << 8) ^ (uint64_t(D.Block) << 24) ^
         (Prod.Saturate ? 1u : 0u) ^ (Prod.Precise ? 2u : 0u);
}

uint64_t ComputePhiSignature(const SsaProgram &Ssa,
                             const std::vector<SsaDefSite> &Defs,
                             const SsaPhi &Phi) {
  uint64_t Sig = 0;
  bool HasInteresting = false;
  for (const auto &In : Phi.Inputs) {
    const uint64_t EdgeTag =
        ComputePhiEdgeTag(Ssa, Defs, In.Value, HasInteresting);
    Sig = Sig * 0x9e3779b97f4a7c15ULL ^ (EdgeTag + In.PredBlock * 31u);
  }
  return HasInteresting ? Sig : 0;
}

bool ExtractCombinePhiIds(const SsaInst &Inst,
                          const std::vector<SsaDefSite> &Defs,
                          llvm::SmallVectorImpl<uint32_t> &Ids) {
  for (const auto &Src : Inst.Sources) {
    if (Src.Kind != OperandKind::Temp || Src.Modifier ||
        Src.RelativeRegister != UINT32_MAX || !Defs[Src.Index].Valid() ||
        !Defs[Src.Index].IsPhi)
      return false;
    Ids.push_back(Src.Index);
  }
  return true;
}

void CollectCombineSeedPhis(const SsaProgram &Ssa,
                            const std::vector<SsaDefSite> &Defs,
                            llvm::DenseSet<uint32_t> &Claimed,
                            llvm::SmallVectorImpl<llvm::SmallVector<uint32_t, 4>> &Out) {
  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead || Inst.Opcode != PseudoCombineVec ||
          Inst.Sources.size() < 2 || Inst.Sources.size() > 4) continue;
      llvm::SmallVector<uint32_t, 4> Ids;
      if (!ExtractCombinePhiIds(Inst, Defs, Ids) ||
          !IsPhiTupleEligible(Ssa, Defs, Claimed, Ids)) continue;
      for (uint32_t Id : Ids) Claimed.insert(Id);
      Out.push_back(std::move(Ids));
    }
  }
}

void CollectParallelSeedPhis(const SsaProgram &Ssa,
                             const std::vector<SsaDefSite> &Defs,
                             llvm::DenseSet<uint32_t> &Claimed,
                             llvm::SmallVectorImpl<llvm::SmallVector<uint32_t, 4>> &Out) {
  for (const auto &Block : Ssa.Blocks) {
    llvm::DenseMap<uint64_t, llvm::SmallVector<uint32_t, 4>> BySig;
    for (const auto &Phi : Block.Phis) {
      if (Phi.Dead || Phi.Mask != 1u || Claimed.count(Phi.Result)) continue;
      if (const uint64_t Sig = ComputePhiSignature(Ssa, Defs, Phi))
        BySig[Sig].push_back(Phi.Result);
    }
    for (auto &KV : BySig) {
      auto &Group = KV.second;
      for (size_t Offset = 0; Offset + 1 < Group.size(); Offset += 4) {
        const size_t Count = std::min<size_t>(4, Group.size() - Offset);
        auto Slice = llvm::makeArrayRef(Group.data() + Offset, Count);
        if (!IsPhiTupleEligible(Ssa, Defs, Claimed, Slice)) continue;
        for (uint32_t Id : Slice) Claimed.insert(Id);
        Out.emplace_back(Slice.begin(), Slice.end());
      }
    }
  }
}

bool IsNonTempFullGather(const SsaInst &C) {
  std::array<Operand, 4> Seen{};
  unsigned Distinct = 0;
  for (const auto &Op : C.Sources) {
    if (Op.Kind == OperandKind::Temp) return false;
    bool Found = false;
    for (unsigned J = 0; J < Distinct; ++J)
      if (EqualCopySources(Seen[J], Op) ||
          (Op.Kind != OperandKind::Immediate &&
           Seen[J].Kind != OperandKind::Immediate &&
           SameOperandBase(Seen[J], Op)))
        Found = true;
    if (!Found) Seen[Distinct++] = Op;
  }
  return Distinct == C.Sources.size();
}

bool IsTreeCombineProfitable(const SsaBlock &Block,
                             const std::vector<SsaDefSite> &Defs,
                             uint32_t BlockId,
                             llvm::ArrayRef<SsaInst> Combines) {
  if (Combines.empty()) return true;
  if (Combines.size() == 1) {
    for (const auto &Op : Combines[0].Sources)
      if (Op.Kind == OperandKind::Temp) return true;
    return false;
  }
  unsigned PackableColumns = 0;
  for (const auto &C : Combines) {
    if (IsNonTempFullGather(C)) return false;
    bool AllSameOp = true;
    uint32_t FirstOp = 0;
    for (unsigned L = 0; L < C.Sources.size(); ++L) {
      const auto &Op = C.Sources[L];
      if (Op.Kind != OperandKind::Temp || !Defs[Op.Index].Valid() ||
          Defs[Op.Index].IsPhi || Defs[Op.Index].Block != BlockId) {
        AllSameOp = false;
        break;
      }
      const auto &P = Block.Insts[Defs[Op.Index].Index];
      if (!IsVectorizableInst(P) || (L != 0 && P.Opcode != FirstOp)) {
        AllSameOp = false;
        break;
      }
      FirstOp = P.Opcode;
    }
    if (AllSameOp)
      ++PackableColumns;
  }
  return Combines.size() - PackableColumns <= 1;
}

bool CollectTreeMembers(const SsaBlock &Block,
                        const std::vector<SsaDefSite> &Defs, uint32_t BlockId,
                        const llvm::DenseSet<uint32_t> &PackedIdx,
                        const SsaInst &Seed,
                        llvm::SmallVectorImpl<uint32_t> &Members) {
  for (unsigned L = 0; L < Seed.Sources.size(); ++L) {
    const auto &Src = Seed.Sources[L];
    if (Src.Kind != OperandKind::Temp || Src.Modifier ||
        Src.RelativeRegister != UINT32_MAX) return false;
    const auto &D = Defs[Src.Index];
    if (!D.Valid() || D.IsPhi || D.Block != BlockId || PackedIdx.count(D.Index))
      return false;
    const auto &Prod = Block.Insts[D.Index];
    if (Prod.Dead || !IsVectorizableInst(Prod) || D.IsAux != IsImulLow(Prod) ||
        PackDest(Prod).Mask != 1u || GetSwizzleLane(Src.Swizzle, 0) != 0)
      return false;
    if (L != 0 && !SameInstructionShape(Block.Insts[Members[0]], Prod))
      return false;
    if (std::find(Members.begin(), Members.end(), D.Index) != Members.end())
      return false;
    Members.push_back(D.Index);
  }
  return true;
}

struct TreeInsertion {
  uint32_t Anchor = 0;
  llvm::SmallVector<SsaInst, 4> Insts;
};

bool PackTreeBlock(SsaProgram &Ssa, uint32_t BlockId,
                   const std::vector<SsaDefSite> &Defs,
                   FirstLocalUseCache &UseCache,
                   llvm::DenseMap<uint32_t, LaneRemap> &AllRemaps) {
  auto &Block = Ssa.Blocks[BlockId];
  const bool HasCombine =
      std::any_of(Block.Insts.begin(), Block.Insts.end(), [](const SsaInst &I) {
        return !I.Dead && I.Opcode == PseudoCombineVec &&
               I.Sources.size() >= 2 && I.Sources.size() <= 4;
      });
  if (!HasCombine)
    return false;

  UseCache.Reset(Block, Ssa.NextValueId);
  std::vector<uint32_t> SlpBarPref(Block.Insts.size() + 1, 0);
  for (size_t I = 0; I < Block.Insts.size(); ++I)
    SlpBarPref[I + 1] = SlpBarPref[I] +
                        (!Block.Insts[I].Dead && IsSlpBarrier(Block.Insts[I]));
  llvm::DenseSet<uint32_t> PackedIdx;
  llvm::SmallVector<TreeInsertion, 8> Insertions;

  for (size_t Rev = Block.Insts.size(); Rev > 0; --Rev) {
    const auto &Seed = Block.Insts[Rev - 1];
    if (Seed.Dead || Seed.Opcode != PseudoCombineVec ||
        Seed.Sources.size() < 2 || Seed.Sources.size() > 4) continue;
    llvm::SmallVector<uint32_t, 4> Members;
    if (!CollectTreeMembers(Block, Defs, BlockId, PackedIdx, Seed, Members))
      continue;
    uint32_t Anchor = 0;
    if (!FindPackAnchor(Block, Defs, BlockId, Members, UseCache, Anchor,
                        &SlpBarPref)) continue;

    const unsigned W = static_cast<unsigned>(Members.size());
    llvm::SmallVector<PackedSlot, 4> Slots;
    for (unsigned L = 0; L < W; ++L)
      Slots.push_back({Members[L], 1u, {static_cast<uint8_t>(L), 0, 0, 0}});

    const auto &Proto = Block.Insts[Members[0]];
    llvm::SmallVector<SsaInst, 4> Combines;
    llvm::SmallVector<Operand, 5> MergedSources;
    bool Ok = true;
    for (size_t S = 0; S < Proto.Sources.size(); ++S) {
      Operand M{OperandKind::Null};
      if (!BuildMergedOperand(Ssa, Block, Slots, S, M, &Combines)) {
        Ok = false;
        break;
      }
      MergedSources.push_back(M);
    }
    if (!Ok || !IsTreeCombineProfitable(Block, Defs, BlockId, Combines)) continue;

    const uint32_t NewId = Ssa.AllocValue();
    SsaInst Packed = Proto;
    PackDest(Packed) = MakeSsaTemp(NewId, MaskForWidth(W), kIdentitySwizzle);
    Packed.Sources = std::move(MergedSources);
    for (unsigned L = 0; L < W; ++L) {
      const uint32_t OldIdx = Members[L];
      const uint32_t OldId = PackDest(Block.Insts[OldIdx]).Index;
      Block.Insts[OldIdx].Dead = true;
      PackedIdx.insert(OldIdx);
      LaneRemap R;
      R.NewId = NewId;
      R.Map.fill(static_cast<uint8_t>(L));
      AllRemaps[OldId] = R;
    }

    TreeInsertion Ins;
    Ins.Anchor = Anchor;
    for (auto &C : Combines) Ins.Insts.push_back(std::move(C));
    Ins.Insts.push_back(std::move(Packed));
    Insertions.push_back(std::move(Ins));
  }

  if (Insertions.empty()) return false;
  std::stable_sort(Insertions.begin(), Insertions.end(),
                   [](const TreeInsertion &A, const TreeInsertion &B) {
                     return A.Anchor < B.Anchor;
                   });
  std::vector<SsaInst> Rebuilt;
  Rebuilt.reserve(Block.Insts.size() + Insertions.size() * 2);
  size_t InsIdx = 0;
  for (size_t I = 0; I <= Block.Insts.size(); ++I) {
    while (InsIdx < Insertions.size() && Insertions[InsIdx].Anchor == I) {
      for (auto &Inst : Insertions[InsIdx].Insts)
        Rebuilt.push_back(std::move(Inst));
      ++InsIdx;
    }
    if (I < Block.Insts.size() && !Block.Insts[I].Dead)
      Rebuilt.push_back(std::move(Block.Insts[I]));
  }
  Block.Insts = std::move(Rebuilt);
  return true;
}

bool RewriteBlockWithRemap(SsaBlock &Block, const RemapTable &Table) {
  bool Changed = false;
  for (auto &Phi : Block.Phis) {
    if (Phi.Dead) continue;
    for (auto &In : Phi.Inputs)
      Changed |= RewriteOperandWithRemap(In.Value, Table);
  }
  for (auto &Inst : Block.Insts) {
    if (Inst.Dead) continue;
    if (Inst.Dest.RelativeRegister != UINT32_MAX) {
      if (const LaneRemap *R = Table.Find(Inst.Dest.RelativeRegister)) {
        Inst.Dest.RelativeRegister = R->NewId;
        Inst.Dest.RelativeLane = R->Map[Inst.Dest.RelativeLane & 3u];
        Changed = true;
      }
    }
    for (auto &Src : Inst.Sources)
      Changed |= RewriteOperandWithRemap(Src, Table);
  }
  if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
    Changed |= RewriteOperandWithRemap(Block.Condition, Table);
  return Changed;
}

} // namespace

void dxcb5::slp::ApplyLaneRemaps(
    SsaProgram &Ssa, const llvm::DenseMap<uint32_t, LaneRemap> &Remaps,
    RemapTable &Table, std::vector<uint8_t> *DirtyBlocks,
    llvm::ArrayRef<uint32_t> OnlyBlocks) {
  if (Remaps.empty()) return;
  Table.Load(Ssa.NextValueId, Remaps);
  if (!OnlyBlocks.empty()) {
    for (uint32_t B : OnlyBlocks)
      if (RewriteBlockWithRemap(Ssa.Blocks[B], Table) && DirtyBlocks &&
          B < DirtyBlocks->size())
        (*DirtyBlocks)[B] = 1;
    return;
  }
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B)
    if (RewriteBlockWithRemap(Ssa.Blocks[B], Table) && DirtyBlocks &&
        B < DirtyBlocks->size())
      (*DirtyBlocks)[B] = 1;
}

bool dxcb5::slp::BuildMergedOperand(
    SsaProgram &Ssa, const SsaBlock &Block, llvm::ArrayRef<PackedSlot> Slots,
    size_t SrcSlot, Operand &Out, llvm::SmallVectorImpl<SsaInst> *EmitCombines) {
  std::array<Operand, 4> PerLane{};
  uint8_t ActiveMask = 0;
  CollectPerLaneOperands(Ssa, Block, Slots, SrcSlot, PerLane, ActiveMask);
  if (!ActiveMask)
    return false;
  const unsigned FirstLane =
      static_cast<unsigned>(llvm::countTrailingZeros(ActiveMask));
  if (TryMergeImmediates(Ssa, PerLane, ActiveMask, FirstLane, Out))
    return true;
  if (TryMergeSameBase(PerLane, ActiveMask, FirstLane, Out))
    return true;
  if (!EmitCombines)
    return false;
  const uint32_t Opcode = Block.Insts[Slots[0].InstIdx].Opcode;
  return SynthesizeCombineOperand(Ssa, Opcode, SrcSlot, PerLane, ActiveMask,
                                  FirstLane, Out, *EmitCombines);
}

bool dxcb5::slp::FindPackAnchor(const SsaBlock &Block,
                                const std::vector<SsaDefSite> &Defs,
                                uint32_t BlockId, llvm::ArrayRef<uint32_t> Indices,
                                const FirstLocalUseCache &FirstLocalUse,
                                uint32_t &OutAnchor,
                                const std::vector<uint32_t> *SlpBarPref) {
  uint32_t MinIdx = UINT32_MAX, MaxIdx = 0;
  llvm::SmallVector<uint32_t, 4> GroupVals;
  for (uint32_t Idx : Indices) {
    MinIdx = std::min(MinIdx, Idx);
    MaxIdx = std::max(MaxIdx, Idx);
    GroupVals.push_back(PackDest(Block.Insts[Idx]).Index);
  }
  if (SlpBarPref) {
    if ((*SlpBarPref)[MaxIdx + 1] != (*SlpBarPref)[MinIdx])
      return false;
  } else {
    for (uint32_t I = MinIdx; I <= MaxIdx; ++I)
      if (!Block.Insts[I].Dead && IsSlpBarrier(Block.Insts[I]))
        return false;
  }
  auto InGroup = [&](uint32_t ValId) {
    return std::find(GroupVals.begin(), GroupVals.end(), ValId) != GroupVals.end();
  };
  uint32_t Earliest = 0;
  uint32_t Latest = static_cast<uint32_t>(Block.Insts.size());
  for (uint32_t Idx : Indices) {
    const auto &Inst = Block.Insts[Idx];
    for (const auto &Src : Inst.Sources) {
      if (Src.Kind == OperandKind::Temp) {
        if (InGroup(Src.Index)) return false;
        const auto &D = Defs[Src.Index];
        if (D.Valid() && !D.IsPhi && D.Block == BlockId)
          Earliest = std::max(Earliest, D.Index + 1);
      }
      if (Src.RelativeRegister != UINT32_MAX) {
        if (InGroup(Src.RelativeRegister)) return false;
        const auto &D = Defs[Src.RelativeRegister];
        if (D.Valid() && !D.IsPhi && D.Block == BlockId)
          Earliest = std::max(Earliest, D.Index + 1);
      }
    }
    Latest = std::min(Latest, FirstLocalUse.Get(PackDest(Inst).Index));
  }
  if (Earliest > Latest) return false;
  OutAnchor = std::min(std::max(Earliest, MaxIdx), Latest);
  return true;
}

bool dxcb5::slp::GroupScalarPhis(SsaProgram &Ssa, RemapTable &RemapCache) {
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  llvm::SmallVector<llvm::SmallVector<uint32_t, 4>, 16> Candidates;
  llvm::DenseSet<uint32_t> Claimed;
  CollectCombineSeedPhis(Ssa, Defs, Claimed, Candidates);
  CollectParallelSeedPhis(Ssa, Defs, Claimed, Candidates);
  if (Candidates.empty()) return false;

  llvm::DenseMap<uint32_t, LaneRemap> Remaps;
  for (const auto &Group : Candidates) {
    const unsigned W = static_cast<unsigned>(Group.size());
    const uint32_t HomeBlock = Defs[Group[0]].Block;
    const uint32_t NewId = Ssa.AllocValue();
    SsaPhi VecPhi{NewId, MaskForWidth(W), false, false, {}};
    const auto &First = Ssa.Blocks[HomeBlock].Phis[Defs[Group[0]].Index];
    for (unsigned L = 0; L < W; ++L) {
      auto &OldPhi = Ssa.Blocks[HomeBlock].Phis[Defs[Group[L]].Index];
      VecPhi.Precise |= OldPhi.Precise;
      OldPhi.Dead = true;
      const auto NL = static_cast<uint8_t>(L);
      Remaps[Group[L]] = {NewId, {NL, NL, NL, NL}};
    }
    for (size_t E = 0; E < First.Inputs.size(); ++E) {
      const uint32_t Pred = First.Inputs[E].PredBlock;
      const Operand CombDest =
          MakeSsaTemp(Ssa.AllocValue(), VecPhi.Mask, kIdentitySwizzle);
      SsaInst Comb;
      Comb.Opcode = PseudoCombineVec;
      Comb.Dest = CombDest;
      Comb.Precise = VecPhi.Precise;
      for (unsigned L = 0; L < W; ++L)
        Comb.Sources.push_back(
            Ssa.Blocks[HomeBlock].Phis[Defs[Group[L]].Index].Inputs[E].Value);
      Ssa.Blocks[Pred].Insts.push_back(std::move(Comb));
      VecPhi.Inputs.push_back({Pred, CombDest});
    }
    Ssa.Blocks[HomeBlock].Phis.push_back(std::move(VecPhi));
  }
  ApplyLaneRemaps(Ssa, Remaps, RemapCache);
  Ssa.CompactDead();
  return true;
}

bool dxcb5::slp::RunTreeSlp(SsaProgram &Ssa, FirstLocalUseCache &UseCache,
                            RemapTable &RemapCache) {
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  llvm::DenseMap<uint32_t, LaneRemap> AllRemaps;
  bool AnyChanged = false;
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B)
    AnyChanged |= PackTreeBlock(Ssa, B, Defs, UseCache, AllRemaps);
  if (!AnyChanged) return false;
  ApplyLaneRemaps(Ssa, AllRemaps, RemapCache);
  Ssa.CompactDead();
  return true;
}
