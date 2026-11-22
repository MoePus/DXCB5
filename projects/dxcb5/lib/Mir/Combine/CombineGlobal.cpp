// Distributed under the University of Illinois Open Source License.
#include "Mir/Combine/CombineCommon.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TimeProfiler.h"
#include <unordered_map>

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

struct UseRef {
  uint32_t Block;
  uint32_t InstIdx;
  uint8_t SrcIdx;
};

struct MultiUseBuckets {
  llvm::DenseMap<uint32_t, llvm::SmallVector<UseRef, 4>> MulAddUsers;
  llvm::DenseMap<uint32_t, llvm::SmallVector<UseRef, 4>> SqrtDivUsers;
  llvm::DenseMap<uint32_t, llvm::SmallVector<UseRef, 4>> DynCbUsers;
  std::vector<uint8_t> BadMulAdd;
  std::vector<uint8_t> BadSqrtDiv;
  std::vector<uint8_t> BadDynCb;
  uint32_t SingleDestSinCos = 0;
  uint32_t SingleDestUdiv = 0;
};

bool IsCandidateMul(const AlgContext &Ctx, uint32_t Id) {
  const auto &Def = Ctx.Defs[Id];
  if (!Def.Valid() || Def.IsPhi || Def.IsAux)
    return false;
  const auto &P = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
  return !P.Dead && P.Opcode == D3D10_SB_OPCODE_MUL && !P.Precise &&
         !P.Saturate;
}

bool IsCandidateSqrt(const AlgContext &Ctx, uint32_t Id) {
  const auto &Def = Ctx.Defs[Id];
  if (!Def.Valid() || Def.IsPhi || Def.IsAux)
    return false;
  const auto &P = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
  return !P.Dead && P.Opcode == D3D10_SB_OPCODE_SQRT && !P.Precise &&
         !P.Saturate;
}

bool IsCandidateDynCb(const AlgContext &Ctx, uint32_t Id) {
  const auto &Def = Ctx.Defs[Id];
  if (!Def.Valid() || Def.IsPhi || Def.IsAux)
    return false;
  return IsDynamicCbMov(Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index]);
}

void MarkAllBad(MultiUseBuckets &B, uint32_t Id) {
  B.BadMulAdd[Id] = 1;
  B.BadSqrtDiv[Id] = 1;
  B.BadDynCb[Id] = 1;
}

void RecordInstMultiUses(const AlgContext &Ctx, uint32_t B, uint32_t I,
                         const SsaInst &Inst, MultiUseBuckets &Out) {
  const uint32_t N = Ctx.Ssa.NextValueId;
  const bool HasDest = Inst.Dest.Kind != OperandKind::Null;
  const bool HasAux = Inst.AuxDest.Kind != OperandKind::Null;
  if (HasDest != HasAux) {
    if (Inst.Opcode == D3D10_SB_OPCODE_SINCOS && Inst.Sources.size() == 1 &&
        Inst.Sources[0].RelativeRegister == UINT32_MAX)
      ++Out.SingleDestSinCos;
    else if (Inst.Opcode == D3D10_SB_OPCODE_UDIV && Inst.Sources.size() == 2 &&
             Inst.Sources[0].RelativeRegister == UINT32_MAX &&
             Inst.Sources[1].RelativeRegister == UINT32_MAX)
      ++Out.SingleDestUdiv;
  }
  const bool EligibleDyn = IsEligibleDynCbConsumer(Inst);
  const bool HasRel = InstHasRelativeOperand(Inst);
  for (size_t S = 0; S < Inst.Sources.size(); ++S) {
    const auto &Op = Inst.Sources[S];
    if (Op.RelativeRegister != UINT32_MAX && Op.RelativeRegister < N)
      MarkAllBad(Out, Op.RelativeRegister);
    if (Op.Kind != OperandKind::Temp || Op.Index >= N)
      continue;
    const uint32_t Id = Op.Index;
    if (Inst.Opcode == D3D10_SB_OPCODE_ADD && !Inst.Precise &&
        Op.RelativeRegister == UINT32_MAX &&
        (Op.Modifier == 0 || Op.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)) {
      if (!Out.BadMulAdd[Id] && IsCandidateMul(Ctx, Id))
        Out.MulAddUsers[Id].push_back({B, I, static_cast<uint8_t>(S)});
    } else {
      Out.BadMulAdd[Id] = 1;
    }
    if (Inst.Opcode == D3D10_SB_OPCODE_DIV && !Inst.Precise && S == 1 &&
        !Op.Modifier && Op.RelativeRegister == UINT32_MAX) {
      if (!Out.BadSqrtDiv[Id] && IsCandidateSqrt(Ctx, Id))
        Out.SqrtDivUsers[Id].push_back({B, I, 1});
    } else {
      Out.BadSqrtDiv[Id] = 1;
    }
    if (EligibleDyn && !HasRel &&
        (!Op.Modifier ||
         InstructionOperandAcceptsFloatModifiers(Inst.Opcode, S))) {
      if (!Out.BadDynCb[Id] && IsCandidateDynCb(Ctx, Id))
        Out.DynCbUsers[Id].push_back({B, I, static_cast<uint8_t>(S)});
    } else {
      Out.BadDynCb[Id] = 1;
    }
  }
}

void CollectMultiUseBuckets(const AlgContext &Ctx, MultiUseBuckets &Out) {
  const uint32_t N = Ctx.Ssa.NextValueId;
  Out.BadMulAdd.assign(N, 0);
  Out.BadSqrtDiv.assign(N, 0);
  Out.BadDynCb.assign(N, 0);
  for (uint32_t B = 0; B < Ctx.Ssa.Blocks.size(); ++B) {
    const auto &Blk = Ctx.Ssa.Blocks[B];
    for (const auto &Phi : Blk.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        if (In.Value.Kind == OperandKind::Temp && In.Value.Index < N)
          MarkAllBad(Out, In.Value.Index);
    }
    if ((Blk.Kind == FlowKind::Branch || Blk.Kind == FlowKind::Switch) &&
        Blk.Condition.Kind == OperandKind::Temp && Blk.Condition.Index < N)
      MarkAllBad(Out, Blk.Condition.Index);
    for (uint32_t I = 0; I < Blk.Insts.size(); ++I)
      if (!Blk.Insts[I].Dead)
        RecordInstMultiUses(Ctx, B, I, Blk.Insts[I], Out);
  }
}

bool FoldMultiUseMulIntoMads(AlgContext &Ctx, const MultiUseBuckets &Buckets) {
  bool Changed = false;
  for (const auto &KV : Buckets.MulAddUsers) {
    const uint32_t MulId = KV.first;
    const auto &Users = KV.second;
    if (Buckets.BadMulAdd[MulId] || Users.size() < 2 || Users.size() > 4)
      continue;
    const auto &Def = Ctx.Defs[MulId];
    auto &Prod = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (Prod.Dead)
      continue;
    bool AllOk = true;
    for (size_t U = 0; U < Users.size() && AllOk; ++U) {
      for (size_t V = 0; V < U; ++V)
        if (Users[U].Block == Users[V].Block &&
            Users[U].InstIdx == Users[V].InstIdx)
          AllOk = false;
      const auto &Add = Ctx.Ssa.Blocks[Users[U].Block].Insts[Users[U].InstIdx];
      if (Add.Dead || Add.Opcode != D3D10_SB_OPCODE_ADD) {
        AllOk = false;
        break;
      }
      const auto &Op = Add.Sources[Users[U].SrcIdx];
      for (unsigned L = 0; L < kRegisterLanes; ++L)
        if ((Add.Dest.Mask & (1u << L)) &&
            !(Prod.Dest.Mask & (1u << GetSwizzleLane(Op.Swizzle, L))))
          AllOk = false;
      const bool NeedsSwz = Op.Swizzle != kIdentitySwizzle &&
                            !(PopcountMask(Add.Dest.Mask) == 1 &&
                              PopcountMask(Prod.Dest.Mask) == 1 &&
                              Op.Swizzle == 0);
      if (NeedsSwz && (Prod.Sources[0].Kind == OperandKind::ImmediateVector ||
                       Prod.Sources[1].Kind == OperandKind::ImmediateVector))
        AllOk = false;
    }
    if (!AllOk)
      continue;
    for (const auto &U : Users) {
      auto &Add = Ctx.Ssa.Blocks[U.Block].Insts[U.InstIdx];
      const auto Op = Add.Sources[U.SrcIdx];
      Operand A = Prod.Sources[0], C = Prod.Sources[1];
      const bool NeedsSwz = Op.Swizzle != kIdentitySwizzle &&
                            !(PopcountMask(Add.Dest.Mask) == 1 &&
                              PopcountMask(Prod.Dest.Mask) == 1 &&
                              Op.Swizzle == 0);
      if (NeedsSwz) {
        if (A.Kind != OperandKind::Immediate)
          A.Swizzle = ComposeSwizzle(A.Swizzle, Op.Swizzle);
        if (C.Kind != OperandKind::Immediate)
          C.Swizzle = ComposeSwizzle(C.Swizzle, Op.Swizzle);
      }
      if (Op.Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
        A = NegateMirOperand(Ctx.Ssa, A);
      Operand Addend = Add.Sources[1 - U.SrcIdx];
      Add.Opcode = D3D10_SB_OPCODE_MAD;
      Add.Sources.assign({A, C, Addend});
    }
    Prod.Dead = true;
    Changed = true;
  }
  return Changed;
}

bool FoldMultiUseSqrtIntoRsq(AlgContext &Ctx, const MultiUseBuckets &Buckets) {
  bool Changed = false;
  for (const auto &KV : Buckets.SqrtDivUsers) {
    const uint32_t SqrtId = KV.first;
    const auto &Users = KV.second;
    if (Buckets.BadSqrtDiv[SqrtId] || Users.size() < 2 || Users.size() > 6)
      continue;
    const auto &Def = Ctx.Defs[SqrtId];
    auto &Sqrt = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (Sqrt.Dead)
      continue;
    Sqrt.Opcode = D3D10_SB_OPCODE_RSQ;
    for (const auto &U : Users)
      Ctx.Ssa.Blocks[U.Block].Insts[U.InstIdx].Opcode = D3D10_SB_OPCODE_MUL;
    Changed = true;
  }
  return Changed;
}

bool FoldMultiUseDynCbMovs(AlgContext &Ctx, const MultiUseBuckets &Buckets) {
  bool Changed = false;
  for (const auto &KV : Buckets.DynCbUsers) {
    const uint32_t Id = KV.first;
    const auto &Users = KV.second;
    if (Buckets.BadDynCb[Id] || Users.size() < 2 || Users.size() > 4)
      continue;
    const auto &Def = Ctx.Defs[Id];
    auto &Move = Ctx.Ssa.Blocks[Def.Block].Insts[Def.Index];
    if (!IsDynamicCbMov(Move))
      continue;
    bool AllOk = true;
    for (size_t U = 0; U < Users.size() && AllOk; ++U) {
      if (Users[U].Block != Def.Block)
        AllOk = false;
      for (size_t V = 0; V < U; ++V)
        if (Users[U].InstIdx == Users[V].InstIdx)
          AllOk = false;
      if (InstHasRelativeOperand(
              Ctx.Ssa.Blocks[Users[U].Block].Insts[Users[U].InstIdx]))
        AllOk = false;
    }
    if (!AllOk)
      continue;
    for (const auto &U : Users) {
      auto &Src =
          Ctx.Ssa.Blocks[U.Block].Insts[U.InstIdx].Sources[U.SrcIdx];
      const uint8_t Mod = Src.Modifier;
      const uint8_t Swz = ComposeSwizzle(Move.Sources[0].Swizzle, Src.Swizzle);
      Src = Move.Sources[0];
      Src.Swizzle = Swz;
      Src.Modifier = Mod;
    }
    Move.Dead = true;
    Changed = true;
  }
  return Changed;
}

struct DualDestKey {
  uint32_t Opcode;
  OperandKind Kind0, Kind1;
  uint32_t Index0, Index1;
  uint32_t Element0, Element1;
  uint8_t Swizzle0, Swizzle1;
  uint8_t Modifier0, Modifier1;
  uint8_t Mask;
  bool Precise;
  bool operator==(const DualDestKey &O) const {
    return Opcode == O.Opcode && Kind0 == O.Kind0 && Kind1 == O.Kind1 &&
           Index0 == O.Index0 && Index1 == O.Index1 &&
           Element0 == O.Element0 && Element1 == O.Element1 &&
           Swizzle0 == O.Swizzle0 && Swizzle1 == O.Swizzle1 &&
           Modifier0 == O.Modifier0 && Modifier1 == O.Modifier1 &&
           Mask == O.Mask && Precise == O.Precise;
  }
};

struct DualDestKeyHash {
  size_t operator()(const DualDestKey &K) const {
    size_t H = size_t(K.Opcode) * 16777619u;
    H ^= (size_t(K.Index0) * 2654435761u) ^ (size_t(K.Element0) << 7) ^
         (size_t(K.Swizzle0) << 16) ^ (size_t(K.Kind0) << 24) ^
         (size_t(K.Modifier0) << 28);
    H = H * 31u + ((size_t(K.Index1) * 2246822519u) ^ (size_t(K.Element1) << 7) ^
                   (size_t(K.Swizzle1) << 16) ^ (size_t(K.Kind1) << 24) ^
                   (size_t(K.Modifier1) << 28));
    return H ^ K.Mask ^ (K.Precise ? 1u : 0u);
  }
};

struct DualScopeFrame {
  uint32_t Block;
  size_t UndoSize;
  bool Exit;
};

using DualMap = std::unordered_map<DualDestKey, std::pair<uint32_t, uint32_t>,
                                   DualDestKeyHash>;
using DualUndoLog =
    llvm::SmallVector<std::pair<DualDestKey, std::pair<uint32_t, uint32_t>>, 8>;

bool MergeBlockDualDestOps(SsaProgram &Ssa, uint32_t BlockIdx,
                           DualMap &ActiveDual, DualUndoLog &UndoLog) {
  bool Changed = false;
  const Operand ZeroOp = Operand();
  auto &Block = Ssa.Blocks[BlockIdx];
  for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
    auto &Inst = Block.Insts[I];
    if (Inst.Dead)
      continue;
    const bool IsSinCos = Inst.Opcode == D3D10_SB_OPCODE_SINCOS &&
                          Inst.Sources.size() == 1 &&
                          Inst.Sources[0].RelativeRegister == UINT32_MAX;
    const bool IsUdiv = Inst.Opcode == D3D10_SB_OPCODE_UDIV &&
                        Inst.Sources.size() == 2 &&
                        Inst.Sources[0].RelativeRegister == UINT32_MAX &&
                        Inst.Sources[1].RelativeRegister == UINT32_MAX;
    if (!IsSinCos && !IsUdiv)
      continue;
    const bool HasDest = Inst.Dest.Kind != OperandKind::Null;
    const bool HasAux = Inst.AuxDest.Kind != OperandKind::Null;
    if (HasDest == HasAux)
      continue;
    const uint8_t Mask = HasDest ? Inst.Dest.Mask : Inst.AuxDest.Mask;
    const auto &S0 = Inst.Sources[0];
    const auto &S1 = IsUdiv ? Inst.Sources[1] : ZeroOp;
    const DualDestKey Key{Inst.Opcode, S0.Kind,    S1.Kind,    S0.Index,
                          S1.Index,    S0.Element, S1.Element, S0.Swizzle,
                          S1.Swizzle,  S0.Modifier, S1.Modifier, Mask,
                          Inst.Precise};
    auto It = ActiveDual.find(Key);
    if (It == ActiveDual.end()) {
      UndoLog.push_back(
          std::make_pair(Key, std::make_pair(UINT32_MAX, UINT32_MAX)));
      ActiveDual[Key] = std::make_pair(BlockIdx, I);
      continue;
    }
    auto &DomInst = Ssa.Blocks[It->second.first].Insts[It->second.second];
    if (DomInst.Dead ||
        !(HasDest ? DomInst.Dest.Kind == OperandKind::Null
                  : DomInst.AuxDest.Kind == OperandKind::Null))
      continue;
    if (HasDest)
      DomInst.Dest = Inst.Dest;
    else
      DomInst.AuxDest = Inst.AuxDest;
    Inst.Dead = true;
    Changed = true;
  }
  return Changed;
}

bool MergeDominatingDualDestOps(SsaProgram &Ssa) {
  if (Ssa.Blocks.empty())
    return false;
  SsaDomTree Dom;
  Ssa.ComputeDominators(Dom);
  DualMap ActiveDual;
  DualUndoLog UndoLog;
  llvm::SmallVector<DualScopeFrame, 32> Stack;
  Stack.push_back({0, 0, false});
  bool Changed = false;
  while (!Stack.empty()) {
    const auto Frame = Stack.pop_back_val();
    if (Frame.Exit) {
      while (UndoLog.size() > Frame.UndoSize) {
        const auto Entry = UndoLog.pop_back_val();
        if (Entry.second.first == UINT32_MAX)
          ActiveDual.erase(Entry.first);
        else
          ActiveDual[Entry.first] = Entry.second;
      }
      continue;
    }
    const size_t Mark = UndoLog.size();
    Changed |= MergeBlockDualDestOps(Ssa, Frame.Block, ActiveDual, UndoLog);
    Stack.push_back({Frame.Block, Mark, true});
    for (uint32_t Child : Dom.Children[Frame.Block])
      Stack.push_back({Child, 0, false});
  }
  return Changed;
}

bool FoldSharedDivisorToRcp(AlgContext &Ctx) {
  if (Ctx.Ssa.NanSafe || Ctx.Ssa.Blocks.empty())
    return false;
  struct DivSite { uint32_t Block, InstIdx; };
  struct Key {
    uint32_t Reg;
    uint8_t Swz, Mask;
    bool operator==(const Key &O) const {
      return Reg == O.Reg && Swz == O.Swz && Mask == O.Mask;
    }
  };
  struct KeyHash {
    size_t operator()(const Key &K) const {
      return size_t(K.Reg) * 2654435761u ^ (size_t(K.Swz) << 8) ^ K.Mask;
    }
  };
  std::unordered_map<Key, llvm::SmallVector<DivSite, 4>, KeyHash> Groups;
  llvm::SmallVector<Key, 16> GroupOrder;
  for (uint32_t B = 0; B < Ctx.Ssa.Blocks.size(); ++B) {
    const auto &Blk = Ctx.Ssa.Blocks[B];
    for (uint32_t I = 0; I < Blk.Insts.size(); ++I) {
      const auto &Inst = Blk.Insts[I];
      if (Inst.Dead || Inst.Opcode != D3D10_SB_OPCODE_DIV || Inst.Precise ||
          Inst.Sources.size() != 2 || Inst.Dest.Kind != OperandKind::Temp ||
          Inst.Sources[1].Kind != OperandKind::Temp ||
          Inst.Sources[1].Modifier ||
          Inst.Sources[1].RelativeRegister != UINT32_MAX)
        continue;
      const auto &DD = Ctx.Defs[Inst.Sources[1].Index];
      if (DD.Valid() && !DD.IsPhi && !DD.IsAux &&
          Ctx.Ssa.Blocks[DD.Block].Insts[DD.Index].Opcode == D3D10_SB_OPCODE_SQRT)
        continue;
      const Key K{Inst.Sources[1].Index, Inst.Sources[1].Swizzle, Inst.Dest.Mask};
      auto &Vec = Groups[K];
      if (Vec.empty())
        GroupOrder.push_back(K);
      Vec.push_back({B, I});
    }
  }
  SsaDomTree Dom;
  bool HaveDom = false;
  for (const Key &K : GroupOrder) {
    const auto &Sites = Groups[K];
    if (Sites.size() < 3)
      continue;
    if (!HaveDom) {
      Ctx.Ssa.ComputeDominators(Dom);
      HaveDom = true;
    }
    auto DominatesAll = [&](size_t A) {
      for (size_t B = 0; B < Sites.size(); ++B) {
        if (A == B) continue;
        if (Sites[A].Block == Sites[B].Block ? Sites[A].InstIdx > Sites[B].InstIdx
                                             : !Dom.Dominates(Sites[A].Block, Sites[B].Block))
          return false;
      }
      return true;
    };
    size_t DomPos = Sites.size();
    for (size_t A = 0; A < Sites.size(); ++A)
      if (DominatesAll(A)) { DomPos = A; break; }
    if (DomPos == Sites.size())
      continue;
    const uint32_t InsBlk = Sites[DomPos].Block, InsIdx = Sites[DomPos].InstIdx;
    const uint8_t Mask = K.Mask;
    const uint8_t Swz = PopcountMask(Mask) == 1
                            ? BroadcastLaneSwizzle(FirstActiveLane(Mask))
                            : kIdentitySwizzle;
    const Operand Denom = Ctx.Ssa.Blocks[InsBlk].Insts[InsIdx].Sources[1];
    const uint32_t RcpId = Ctx.Ssa.AllocValue();
    const Operand RcpDst = MakeSsaTemp(RcpId, Mask, Swz);
    for (const auto &S : Sites) {
      auto &DivI = Ctx.Ssa.Blocks[S.Block].Insts[S.InstIdx];
      DivI.Opcode = D3D10_SB_OPCODE_MUL;
      DivI.Sources[1] = RcpDst;
    }
    SsaInst RcpInst;
    RcpInst.Opcode = D3D11_SB_OPCODE_RCP;
    RcpInst.Dest = RcpDst;
    RcpInst.Sources.assign({Denom});
    auto &Insts = Ctx.Ssa.Blocks[InsBlk].Insts;
    Insts.insert(Insts.begin() + InsIdx, std::move(RcpInst));
    Ctx.RefreshBlockAfterInsert(InsBlk, InsIdx);
    Ctx.Stats.Count[RcpId] = static_cast<uint32_t>(Sites.size());
    return true;
  }
  return false;
}

} // namespace

bool dxcb5::alg::RunGlobalAlgebraicFolds(AlgContext &Ctx) {
  MultiUseBuckets Buckets;
  CollectMultiUseBuckets(Ctx, Buckets);
  bool Changed = false;
  Changed |= FoldMultiUseMulIntoMads(Ctx, Buckets);
  Changed |= FoldMultiUseSqrtIntoRsq(Ctx, Buckets);
  Changed |= FoldSharedDivisorToRcp(Ctx);
  Changed |= FoldMultiUseDynCbMovs(Ctx, Buckets);
  if (Buckets.SingleDestSinCos >= 2 || Buckets.SingleDestUdiv >= 2)
    Changed |= MergeDominatingDualDestOps(Ctx.Ssa);
  return Changed;
}
