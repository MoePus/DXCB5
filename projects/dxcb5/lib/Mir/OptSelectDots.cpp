// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <array>

using namespace dxcb5;

namespace dxcb5 {
bool MatchHorizontalDots(SsaProgram &Ssa, const std::vector<SsaDefSite> &Defs,
                         const std::vector<uint32_t> &UseCount);
} // namespace dxcb5

namespace {

struct Term {
  Operand A;
  Operand B;
};

struct StepMatch {
  Term Product;
  Operand Addend;
  uint32_t FoldedMulIndex = UINT32_MAX;
};

struct ChainInfo {
  uint32_t RootIdx = UINT32_MAX;
  uint32_t Width = 0;
  std::array<Term, 4> Terms{};
  llvm::SmallVector<uint32_t, 6> ConsumedInsts;
};

struct SharedMember {
  ChainInfo Chain;
  Operand Direct;
  bool DirectOnLeft;
};

struct TupleKey {
  std::array<Operand, 3> Lanes;
  bool operator==(const TupleKey &O) const {
    for (unsigned L = 0; L < 3; ++L)
      if (!EqualCopySources(Lanes[L], O.Lanes[L]))
        return false;
    return true;
  }
};

struct SharedGroup {
  TupleKey Key;
  llvm::SmallVector<SharedMember, 4> Members;
};

Operand NormalizeLaneOp(Operand Op, unsigned Lane) {
  if (Op.Kind != OperandKind::Immediate &&
      Op.Kind != OperandKind::ImmediateVector)
    Op.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(Op.Swizzle, Lane));
  return Op;
}

void CountValueUses(const SsaProgram &Ssa, std::vector<uint32_t> &UseCount) {
  UseCount.assign(Ssa.NextValueId, 0);
  auto CountOp = [&](const Operand &Op) {
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < Ssa.NextValueId)
      ++UseCount[Op.RelativeRegister];
    if (Op.Kind == OperandKind::Temp && Op.Index < Ssa.NextValueId)
      ++UseCount[Op.Index];
  };
  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        CountOp(In.Value);
    }
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.RelativeRegister != UINT32_MAX &&
          Inst.Dest.RelativeRegister < Ssa.NextValueId)
        ++UseCount[Inst.Dest.RelativeRegister];
      for (const auto &Src : Inst.Sources)
        CountOp(Src);
    }
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
      CountOp(Block.Condition);
  }
}

bool TryBuildDirectOperand(SsaProgram &Ssa, llvm::ArrayRef<Operand> Lanes,
                           Operand &Out) {
  const unsigned W = static_cast<unsigned>(Lanes.size());
  bool AllImm = true;
  bool SameImm = true;
  std::array<uint32_t, 4> Bits{};
  for (unsigned L = 0; L < W; ++L) {
    if (Lanes[L].Kind != OperandKind::Immediate || Lanes[L].Modifier) {
      AllImm = false;
      break;
    }
    Bits[L] = Lanes[L].Index;
    if (L && Bits[L] != Bits[0])
      SameImm = false;
  }
  if (AllImm) {
    if (SameImm) {
      Out = MakeImm(Bits[0]);
      return true;
    }
    Out = {OperandKind::ImmediateVector, Ssa.InternLiteralVector(Bits), 0,
           MaskForWidth(W), kIdentitySwizzle};
    return true;
  }

  const auto &First = Lanes[0];
  if (First.Kind != OperandKind::Temp && First.Kind != OperandKind::Input &&
      First.Kind != OperandKind::ConstantBuffer)
    return false;
  for (unsigned L = 1; L < W; ++L)
    if (!SameOperandBase(First, Lanes[L]))
      return false;
  Out = First;
  Out.Mask = MaskForWidth(W);
  uint8_t Swz = 0;
  const unsigned Fallback = GetSwizzleLane(First.Swizzle, 0);
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    const unsigned Src =
        L < W ? GetSwizzleLane(Lanes[L].Swizzle, 0) : Fallback;
    Swz = SetSwizzleLane(Swz, L, Src);
  }
  Out.Swizzle = Swz;
  return true;
}

bool MatchDirectDotSides(SsaProgram &Ssa, llvm::ArrayRef<Term> Terms,
                         Operand &OutA, Operand &OutB) {
  const unsigned W = static_cast<unsigned>(Terms.size());
  const unsigned Perms = 1u << (W - 1);
  for (unsigned Swap = 0; Swap < Perms; ++Swap) {
    std::array<Operand, 4> Left, Right;
    Left[0] = Terms[0].A;
    Right[0] = Terms[0].B;
    for (unsigned L = 1; L < W; ++L) {
      const bool Flip = ((Swap >> (L - 1)) & 1u) != 0;
      Left[L] = Flip ? Terms[L].B : Terms[L].A;
      Right[L] = Flip ? Terms[L].A : Terms[L].B;
    }
    if (TryBuildDirectOperand(Ssa, llvm::makeArrayRef(Left.data(), W), OutA) &&
        TryBuildDirectOperand(Ssa, llvm::makeArrayRef(Right.data(), W), OutB))
      return true;
  }
  return false;
}

bool MatchAddWithMul(const SsaProgram &Ssa,
                     const std::vector<SsaDefSite> &Defs,
                     const std::vector<uint32_t> &UseCount, uint32_t BlockId,
                     const SsaInst &Inst, unsigned DLane, StepMatch &Out) {
  for (unsigned Side = 0; Side < 2; ++Side) {
    const auto &Op = Inst.Sources[Side];
    if (Op.Kind != OperandKind::Temp || Op.RelativeRegister != UINT32_MAX ||
        (Op.Modifier != 0 && Op.Modifier != D3D10_SB_OPERAND_MODIFIER_NEG))
      continue;
    const auto &Def = Defs[Op.Index];
    if (!Def.Valid() || Def.IsPhi || Def.IsAux)
      continue;
    const auto &Mul = Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (Mul.Dead || Mul.Opcode != D3D10_SB_OPCODE_MUL || Mul.Precise ||
        Mul.Saturate || PopcountMask(Mul.Dest.Mask) != 1)
      continue;
    const unsigned MLane = FirstActiveLane(Mul.Dest.Mask);
    if (GetSwizzleLane(Op.Swizzle, DLane) != MLane ||
        Mul.Sources[0].Kind == OperandKind::ImmediateVector ||
        Mul.Sources[1].Kind == OperandKind::ImmediateVector)
      continue;
    Operand A = NormalizeLaneOp(Mul.Sources[0], MLane);
    if (Op.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
      A = NegateMirOperand(A);
    const uint32_t FoldIdx =
        (Def.Block == BlockId && UseCount[Op.Index] == 1) ? Def.Index : UINT32_MAX;
    Out = {{A, NormalizeLaneOp(Mul.Sources[1], MLane)},
           NormalizeLaneOp(Inst.Sources[1 - Side], DLane), FoldIdx};
    return true;
  }
  return false;
}

bool MatchDotStep(const SsaProgram &Ssa, const std::vector<SsaDefSite> &Defs,
                  const std::vector<uint32_t> &UseCount, uint32_t BlockId,
                  uint32_t InstIdx, StepMatch &Out) {
  const auto &Inst = Ssa.Blocks[BlockId].Insts[InstIdx];
  if (Inst.Dead || Inst.Precise || Inst.Dest.Kind != OperandKind::Temp ||
      PopcountMask(Inst.Dest.Mask) != 1)
    return false;
  const unsigned DLane = FirstActiveLane(Inst.Dest.Mask);
  if (Inst.Opcode == D3D10_SB_OPCODE_MAD) {
    if (Inst.Sources[0].Kind == OperandKind::ImmediateVector ||
        Inst.Sources[1].Kind == OperandKind::ImmediateVector)
      return false;
    Out = {{NormalizeLaneOp(Inst.Sources[0], DLane),
            NormalizeLaneOp(Inst.Sources[1], DLane)},
           NormalizeLaneOp(Inst.Sources[2], DLane), UINT32_MAX};
    return true;
  }
  if (Inst.Opcode == D3D10_SB_OPCODE_ADD)
    return MatchAddWithMul(Ssa, Defs, UseCount, BlockId, Inst, DLane, Out);
  return false;
}

bool MatchBaseDotTerm(const SsaProgram &Ssa,
                      const std::vector<SsaDefSite> &Defs,
                      const std::vector<uint32_t> &UseCount, uint32_t BlockId,
                      const Operand &Add, ChainInfo &Out) {
  if (Add.Kind == OperandKind::Temp && Add.RelativeRegister == UINT32_MAX &&
      (Add.Modifier == 0 || Add.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)) {
    const auto &AddDef = Defs[Add.Index];
    const SsaInst *Base =
        (AddDef.Valid() && !AddDef.IsPhi && !AddDef.IsAux)
            ? &Ssa.Blocks[AddDef.Block].Insts[AddDef.Index]
            : nullptr;
    if (Base && !Base->Dead && Base->Opcode == D3D10_SB_OPCODE_MUL &&
        !Base->Precise && !Base->Saturate &&
        PopcountMask(Base->Dest.Mask) == 1 &&
        GetSwizzleLane(Add.Swizzle, 0) == FirstActiveLane(Base->Dest.Mask) &&
        Base->Sources[0].Kind != OperandKind::ImmediateVector &&
        Base->Sources[1].Kind != OperandKind::ImmediateVector) {
      const unsigned BLane = FirstActiveLane(Base->Dest.Mask);
      Operand A = NormalizeLaneOp(Base->Sources[0], BLane);
      if (Add.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
        A = NegateMirOperand(A);
      Out.Terms[0].A = A;
      Out.Terms[0].B = NormalizeLaneOp(Base->Sources[1], BLane);
      if (AddDef.Block == BlockId && UseCount[Add.Index] == 1)
        Out.ConsumedInsts.push_back(AddDef.Index);
      return true;
    }
  }
  if (Add.RelativeRegister != UINT32_MAX ||
      (Add.Modifier != 0 && Add.Modifier != D3D10_SB_OPERAND_MODIFIER_NEG))
    return false;
  Operand A = Add;
  Operand One = MakeImm(0x3f800000u);
  if (A.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG) {
    A.Modifier = 0;
    One = MakeImm(0xbf800000u);
  }
  Out.Terms[0].A = A;
  Out.Terms[0].B = One;
  return true;
}

bool MatchDotChain(const SsaProgram &Ssa, const std::vector<SsaDefSite> &Defs,
                   const std::vector<uint32_t> &UseCount, uint32_t BlockId,
                   uint32_t RootIdx, unsigned TargetWidth, ChainInfo &Out) {
  const auto &Block = Ssa.Blocks[BlockId];
  Out = {};
  Out.RootIdx = RootIdx;
  Out.Width = TargetWidth;
  uint32_t CurIdx = RootIdx;
  for (int Slot = static_cast<int>(TargetWidth) - 1; Slot >= 1; --Slot) {
    StepMatch Step;
    if (!MatchDotStep(Ssa, Defs, UseCount, BlockId, CurIdx, Step))
      return false;
    if (CurIdx != RootIdx && Block.Insts[CurIdx].Saturate)
      return false;
    Out.Terms[Slot] = Step.Product;
    if (Step.FoldedMulIndex != UINT32_MAX)
      Out.ConsumedInsts.push_back(Step.FoldedMulIndex);
    if (CurIdx != RootIdx)
      Out.ConsumedInsts.push_back(CurIdx);
    const auto &Add = Step.Addend;
    if (Slot == 1)
      return MatchBaseDotTerm(Ssa, Defs, UseCount, BlockId, Add, Out);
    if (Add.Kind != OperandKind::Temp || Add.RelativeRegister != UINT32_MAX ||
        Add.Modifier != 0)
      return false;
    const auto &AddDef = Defs[Add.Index];
    if (!AddDef.Valid() || AddDef.IsPhi || AddDef.IsAux ||
        AddDef.Block != BlockId || UseCount[Add.Index] != 1)
      return false;
    const auto &NextInst = Block.Insts[AddDef.Index];
    if (PopcountMask(NextInst.Dest.Mask) != 1 ||
        GetSwizzleLane(Add.Swizzle, 0) != FirstActiveLane(NextInst.Dest.Mask))
      return false;
    CurIdx = AddDef.Index;
  }
  return true;
}

bool ForwardDynamicCbIntoDots(SsaProgram &Ssa,
                              const std::vector<SsaDefSite> &Defs,
                              const std::vector<uint32_t> &UseCount) {
  bool Changed = false;
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Block = Ssa.Blocks[B];
    for (auto &Inst : Block.Insts) {
      if (Inst.Dead || !DotSourceSlotMask(Inst.Opcode))
        continue;
      for (auto &Src : Inst.Sources) {
        if (Src.Kind != OperandKind::Temp || Src.Modifier ||
            Src.RelativeRegister != UINT32_MAX || UseCount[Src.Index] != 1)
          continue;
        const auto &Def = Defs[Src.Index];
        if (!Def.Valid() || Def.IsPhi || Def.IsAux || Def.Block != B)
          continue;
        auto &Move = Block.Insts[Def.Index];
        if (Move.Dead || Move.Opcode != D3D10_SB_OPCODE_MOV || Move.Saturate ||
            Move.Sources[0].Kind != OperandKind::ConstantBuffer ||
            Move.Sources[0].Modifier ||
            Move.Sources[0].RelativeRegister == UINT32_MAX)
          continue;
        const uint8_t Swz = ComposeSwizzle(Move.Sources[0].Swizzle, Src.Swizzle);
        Src = Move.Sources[0];
        Src.Swizzle = Swz;
        Move.Dead = true;
        Changed = true;
      }
    }
  }
  return Changed;
}

bool AllLocalTemps(const std::array<Operand, 3> &Gather,
                   const std::vector<SsaDefSite> &Defs, uint32_t BlockId) {
  for (const auto &Op : Gather)
    if (Op.Kind != OperandKind::Temp || Op.Modifier ||
        Op.RelativeRegister != UINT32_MAX || Defs[Op.Index].Block != BlockId)
      return false;
  return true;
}

void RecordSharedDp3Candidate(SsaProgram &Ssa, uint32_t BlockId,
                              const ChainInfo &Chain,
                              const std::vector<SsaDefSite> &Defs,
                              llvm::SmallVectorImpl<SharedGroup> &Groups) {
  for (unsigned Swap = 0; Swap < 4; ++Swap) {
    std::array<Operand, 3> Left{
        Chain.Terms[0].A,
        (Swap & 1) ? Chain.Terms[1].B : Chain.Terms[1].A,
        (Swap & 2) ? Chain.Terms[2].B : Chain.Terms[2].A};
    std::array<Operand, 3> Right{
        Chain.Terms[0].B,
        (Swap & 1) ? Chain.Terms[1].A : Chain.Terms[1].B,
        (Swap & 2) ? Chain.Terms[2].A : Chain.Terms[2].B};
    Operand DirectL, DirectR;
    const bool OkL = TryBuildDirectOperand(
        Ssa, llvm::makeArrayRef(Left.data(), Left.size()), DirectL);
    const bool OkR = TryBuildDirectOperand(
        Ssa, llvm::makeArrayRef(Right.data(), Right.size()), DirectR);
    if (OkL == OkR)
      continue;
    const auto &Gather = OkL ? Right : Left;
    if (!AllLocalTemps(Gather, Defs, BlockId))
      continue;
    const TupleKey K{Gather};
    auto *Found = std::find_if(Groups.begin(), Groups.end(),
                               [&](const SharedGroup &G) { return G.Key == K; });
    if (Found == Groups.end()) {
      if (Groups.size() >= 32)
        continue;
      Groups.emplace_back();
      Groups.back().Key = K;
      Found = &Groups.back();
    }
    Found->Members.push_back({Chain, OkL ? DirectL : DirectR, OkL});
    return;
  }
}

bool MaterializeSharedDp3Group(SsaProgram &Ssa, SsaBlock &Block,
                               const std::vector<SsaDefSite> &Defs,
                               SharedGroup &Group) {
  if (Group.Members.size() < 3)
    return false;
  uint32_t LatestInputDef = 0;
  for (const auto &Op : Group.Key.Lanes) {
    const auto &D = Defs[Op.Index];
    if (!D.IsPhi)
      LatestInputDef = std::max(LatestInputDef, D.Index);
  }
  llvm::SmallVector<SharedMember *, 4> Live;
  uint32_t EarliestRoot = UINT32_MAX;
  for (auto &M : Group.Members) {
    if (Block.Insts[M.Chain.RootIdx].Dead || M.Chain.RootIdx <= LatestInputDef)
      continue;
    bool ConsumedLive = true;
    for (uint32_t Idx : M.Chain.ConsumedInsts)
      if (Block.Insts[Idx].Dead)
        ConsumedLive = false;
    if (!ConsumedLive)
      continue;
    Live.push_back(&M);
    EarliestRoot = std::min(EarliestRoot, M.Chain.RootIdx);
  }
  if (Live.size() < 3)
    return false;

  const Operand Packed = MakeSsaTemp(Ssa.AllocValue(), 0x7u, kIdentitySwizzle);
  SsaInst Combine;
  Combine.Opcode = PseudoCombineVec;
  Combine.Dest = Packed;
  Combine.Sources.append(Group.Key.Lanes.begin(), Group.Key.Lanes.end());
  for (auto *M : Live) {
    auto &Root = Block.Insts[M->Chain.RootIdx];
    Root.Opcode = D3D10_SB_OPCODE_DP3;
    Root.Sources.clear();
    Root.Sources.push_back(M->DirectOnLeft ? M->Direct : Packed);
    Root.Sources.push_back(M->DirectOnLeft ? Packed : M->Direct);
    for (uint32_t DeadIdx : M->Chain.ConsumedInsts)
      Block.Insts[DeadIdx].Dead = true;
  }
  Block.Insts.insert(Block.Insts.begin() + EarliestRoot, std::move(Combine));
  return true;
}

bool SelectBlockDots(SsaProgram &Ssa, uint32_t B,
                     const std::vector<SsaDefSite> &Defs,
                     const std::vector<uint32_t> &UseCount) {
  auto &Block = Ssa.Blocks[B];
  bool Changed = false;
  for (size_t Rev = Block.Insts.size(); Rev > 0; --Rev) {
    const uint32_t I = static_cast<uint32_t>(Rev - 1);
    if (Block.Insts[I].Dead)
      continue;
    for (unsigned W : {4u, 3u}) {
      ChainInfo Chain;
      if (!MatchDotChain(Ssa, Defs, UseCount, B, I, W, Chain))
        continue;
      Operand A, C;
      if (!MatchDirectDotSides(
              Ssa, llvm::makeArrayRef(Chain.Terms.data(), W), A, C))
        continue;
      auto &Root = Block.Insts[I];
      Root.Opcode = W == 4 ? D3D10_SB_OPCODE_DP4 : D3D10_SB_OPCODE_DP3;
      Root.Sources.assign({A, C});
      for (uint32_t DeadIdx : Chain.ConsumedInsts)
        Block.Insts[DeadIdx].Dead = true;
      Changed = true;
      break;
    }
  }
  llvm::SmallVector<SharedGroup, 8> Groups;
  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    if (Block.Insts[I].Dead)
      continue;
    ChainInfo Chain;
    if (MatchDotChain(Ssa, Defs, UseCount, B, I, 3, Chain))
      RecordSharedDp3Candidate(Ssa, B, Chain, Defs, Groups);
  }
  for (auto &Group : Groups) {
    if (MaterializeSharedDp3Group(Ssa, Block, Defs, Group)) {
      Changed = true;
      break;
    }
  }
  return Changed;
}

} // namespace

bool dxcb5::RunOptSelectDots(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Select Dots", llvm::StringRef());
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<uint32_t> UseCount;
  CountValueUses(Ssa, UseCount);

  bool Changed = ForwardDynamicCbIntoDots(Ssa, Defs, UseCount);
  Changed |= MatchHorizontalDots(Ssa, Defs, UseCount);
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B)
    Changed |= SelectBlockDots(Ssa, B, Defs, UseCount);
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}

bool dxcb5::RunOptSelectDp2(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Select DP2", llvm::StringRef());
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<uint32_t> UseCount;
  CountValueUses(Ssa, UseCount);

  bool Changed = false;
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Block = Ssa.Blocks[B];
    for (size_t Rev = Block.Insts.size(); Rev > 0; --Rev) {
      const uint32_t I = static_cast<uint32_t>(Rev - 1);
      if (Block.Insts[I].Dead)
        continue;
      ChainInfo Chain;
      if (!MatchDotChain(Ssa, Defs, UseCount, B, I, 2u, Chain) ||
          Chain.ConsumedInsts.empty())
        continue;
      Operand A, C;
      if (!MatchDirectDotSides(
              Ssa, llvm::makeArrayRef(Chain.Terms.data(), 2u), A, C))
        continue;
      auto &Root = Block.Insts[I];
      Root.Opcode = D3D10_SB_OPCODE_DP2;
      Root.Sources.assign({A, C});
      for (uint32_t DeadIdx : Chain.ConsumedInsts)
        Block.Insts[DeadIdx].Dead = true;
      Changed = true;
    }
  }
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}
