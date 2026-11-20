// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <tuple>

using namespace dxcb5;

namespace {

struct OpKey {
  uint8_t Kind = 0;
  uint32_t Index = 0, Element = 0;
  uint8_t Swizzle = 0, Modifier = 0;
  uint32_t RelReg = UINT32_MAX;
  uint8_t RelLane = 0;
  auto Tie() const { return std::tie(Kind, Index, Element, Swizzle, Modifier, RelReg, RelLane); }
  bool operator==(const OpKey &O) const { return Tie() == O.Tie(); }
  bool operator<(const OpKey &O) const { return Tie() < O.Tie(); }
};

OpKey MakeWholeOpKey(const Operand &Op, uint8_t ActiveSlots) {
  uint8_t MaskedSwz = 0;
  if (Op.Kind == OperandKind::Sampler) {
    MaskedSwz = Op.Swizzle;
  } else if (Op.Kind != OperandKind::Immediate &&
             Op.Kind != OperandKind::ImmediateVector) {
    for (unsigned Slot = 0; Slot < kRegisterLanes; ++Slot)
      if (ActiveSlots & (1u << Slot))
        MaskedSwz = SetSwizzleLane(MaskedSwz, Slot, GetSwizzleLane(Op.Swizzle, Slot));
  }
  return {static_cast<uint8_t>(Op.Kind), Op.Index, Op.Element, MaskedSwz,
          Op.Modifier, Op.RelativeRegister, Op.RelativeLane};
}

OpKey MakeLaneOpKey(const Operand &Op, unsigned Lane, const MirProgram &Mir) {
  if (Op.Kind == OperandKind::Immediate)
    return {static_cast<uint8_t>(OperandKind::Immediate), Op.Index, 0, 0,
            Op.Modifier, UINT32_MAX, 0};
  const unsigned SrcLane = GetSwizzleLane(Op.Swizzle, Lane);
  if (Op.Kind == OperandKind::ImmediateVector)
    return {static_cast<uint8_t>(OperandKind::Immediate),
            Mir.LiteralVectors[Op.Index][SrcLane], 0, 0, Op.Modifier,
            UINT32_MAX, 0};
  return {static_cast<uint8_t>(Op.Kind), Op.Index, Op.Element,
          static_cast<uint8_t>(SrcLane), Op.Modifier, Op.RelativeRegister,
          Op.RelativeLane};
}

struct ExprKey {
  uint32_t Opcode = 0;
  uint8_t Flags = 0, Mask = 0;
  int8_t Off0 = 0, Off1 = 0, Off2 = 0;
  uint8_t NumOps = 0;
  std::array<OpKey, 5> Ops{};
  bool operator==(const ExprKey &O) const {
    if (Opcode != O.Opcode || Flags != O.Flags || Mask != O.Mask ||
        Off0 != O.Off0 || Off1 != O.Off1 || Off2 != O.Off2 || NumOps != O.NumOps)
      return false;
    for (unsigned I = 0; I < NumOps; ++I)
      if (!(Ops[I] == O.Ops[I])) return false;
    return true;
  }
};

uint64_t HashExprKey(const ExprKey &K) {
  uint64_t H = uint64_t(K.Opcode) * 0x9e3779b97f4a7c15ULL ^
               (uint64_t(K.Flags) << 16) ^ (uint64_t(K.Mask) << 20) ^
               (uint64_t(K.NumOps) << 24);
  H ^= uint64_t(uint8_t(K.Off0)) ^ (uint64_t(uint8_t(K.Off1)) << 8) ^
       (uint64_t(uint8_t(K.Off2)) << 16);
  for (unsigned I = 0; I < K.NumOps; ++I) {
    const auto &O = K.Ops[I];
    const uint64_t OH = (uint64_t(O.Kind) << 28) ^ uint64_t(O.Index) ^
                        (uint64_t(O.Element) * 2654435761ULL) ^
                        (uint64_t(O.Swizzle) << 8) ^
                        (uint64_t(O.Modifier) << 16) ^
                        (uint64_t(O.RelReg) * 40503ULL) ^
                        (uint64_t(O.RelLane) << 24);
    H ^= OH + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
  }
  return H;
}

// Scoped flat-array chained hash table: O(1) stack push on insert, O(1) pop on
// dominator-tree rollback, zero per-entry heap allocation, zero tombstones.
class ScopedGvnTable {
public:
  explicit ScopedGvnTable(size_t InitialBuckets = 4096)
      : Heads(InitialBuckets, UINT32_MAX), Mask(InitialBuckets - 1) {
    Entries.reserve(InitialBuckets);
  }

  size_t Mark() const { return Entries.size(); }

  const Operand *Find(const ExprKey &K) const {
    const uint64_t H = HashExprKey(K);
    uint32_t Idx = Heads[H & Mask];
    while (Idx != UINT32_MAX) {
      const auto &E = Entries[Idx];
      if (E.Hash == H && E.Key == K)
        return &E.Val;
      Idx = E.Prev;
    }
    return nullptr;
  }

  void Insert(const ExprKey &K, const Operand &Val) {
    if (Entries.size() * 4 >= Heads.size() * 3)
      Rehash(Heads.size() * 2);
    const uint64_t H = HashExprKey(K);
    const size_t B = H & Mask;
    const uint32_t Idx = static_cast<uint32_t>(Entries.size());
    Entries.push_back({K, Val, H, Heads[B]});
    Heads[B] = Idx;
  }

  void Rollback(size_t Target) {
    while (Entries.size() > Target) {
      const auto &E = Entries.back();
      Heads[E.Hash & Mask] = E.Prev;
      Entries.pop_back();
    }
  }

private:
  struct Entry {
    ExprKey Key;
    Operand Val;
    uint64_t Hash;
    uint32_t Prev;
  };

  void Rehash(size_t NewBuckets) {
    Heads.assign(NewBuckets, UINT32_MAX);
    Mask = NewBuckets - 1;
    for (uint32_t I = 0; I < Entries.size(); ++I) {
      const size_t B = Entries[I].Hash & Mask;
      Entries[I].Prev = Heads[B];
      Heads[B] = I;
    }
  }

  std::vector<Entry> Entries;
  std::vector<uint32_t> Heads;
  size_t Mask;
};

Operand MakeWholeVal(const Operand &Dest) {
  Operand V = Dest;
  V.Swizzle = PopcountMask(Dest.Mask) == 1
                  ? BroadcastLaneSwizzle(
                        static_cast<unsigned>(llvm::countTrailingZeros(Dest.Mask)))
                  : kIdentitySwizzle;
  return V;
}

bool IsImulAuxOnly(const SsaInst &Inst) {
  return (Inst.Opcode == D3D10_SB_OPCODE_IMUL ||
          Inst.Opcode == D3D10_SB_OPCODE_UMUL) &&
         Inst.Dest.Kind == OperandKind::Null &&
         Inst.AuxDest.Kind == OperandKind::Temp;
}

bool CanGvnOpcode(const SsaInst &Inst) {
  if (!Inst.IsPure() || Inst.Sources.size() > 5)
    return false;
  if (Inst.Opcode == D3D10_SB_OPCODE_SINCOS)
    return (Inst.Dest.Kind == OperandKind::Temp ||
            Inst.AuxDest.Kind == OperandKind::Temp) &&
           Inst.Sources.size() == 1;
  if (IsImulAuxOnly(Inst))
    return Inst.Sources.size() == 2;
  if (Inst.Dest.Kind != OperandKind::Temp ||
      Inst.AuxDest.Kind != OperandKind::Null)
    return false;
  if (Inst.Opcode == PseudoCombineVec)
    return true;
  if (Inst.Opcode == D3D10_SB_OPCODE_MOV && !Inst.Saturate &&
      Inst.Sources[0].RelativeRegister == UINT32_MAX &&
      Inst.Sources[0].Kind != OperandKind::ImmediateConstantBuffer)
    return false;
  return true;
}

void CanonicalizeKeyCommutativity(uint32_t Opcode, ExprKey &K,
                                  bool AllowImadUmad) {
  if (InstructionIsCommutativeBinary(Opcode) && K.NumOps == 2 &&
      K.Ops[1] < K.Ops[0])
    std::swap(K.Ops[0], K.Ops[1]);
  const bool IsMad =
      Opcode == D3D10_SB_OPCODE_MAD ||
      (AllowImadUmad &&
       (Opcode == D3D10_SB_OPCODE_IMAD || Opcode == D3D10_SB_OPCODE_UMAD));
  if (IsMad && K.NumOps == 3 && K.Ops[1] < K.Ops[0])
    std::swap(K.Ops[0], K.Ops[1]);
}

bool ResolveAlias(Operand &Op, const std::vector<Operand> &CopyOf) {
  bool Changed = false;
  if (Op.RelativeRegister != UINT32_MAX && Op.RelativeRegister < CopyOf.size()) {
    const auto &RA = CopyOf[Op.RelativeRegister];
    if (RA.Kind == OperandKind::Temp) {
      Op.RelativeRegister = RA.Index;
      Op.RelativeLane = GetSwizzleLane(RA.Swizzle, Op.RelativeLane);
      Changed = true;
    }
  }
  if (Op.Kind != OperandKind::Temp || Op.RelativeRegister != UINT32_MAX ||
      Op.Index >= CopyOf.size())
    return Changed;
  const auto &A = CopyOf[Op.Index];
  if (A.Kind != OperandKind::Temp)
    return Changed;
  Op.Index = A.Index;
  Op.Swizzle = ComposeSwizzle(A.Swizzle, Op.Swizzle);
  return true;
}

void RecordCopy(uint32_t DstId, Operand Src, std::vector<Operand> &CopyOf,
                bool &AnyAlias) {
  if (DstId >= CopyOf.size() || Src.Kind != OperandKind::Temp || Src.Modifier ||
      Src.RelativeRegister != UINT32_MAX)
    return;
  ResolveAlias(Src, CopyOf);
  if (Src.Index == DstId)
    return;
  CopyOf[DstId] = Src;
  AnyAlias = true;
}

bool ProcessSinCosGvn(SsaBlock &Block, size_t &I, uint8_t Flags,
                      ScopedGvnTable &Scope, std::vector<Operand> &CopyOf,
                      bool &AnyAlias) {
  auto &Inst = Block.Insts[I];
  const bool HasSin = Inst.Dest.Kind == OperandKind::Temp;
  const bool HasCos = Inst.AuxDest.Kind == OperandKind::Temp;
  const uint8_t Mask = HasSin ? Inst.Dest.Mask : Inst.AuxDest.Mask;
  ExprKey SinKey{D3D10_SB_OPCODE_SINCOS, Flags, Mask, 0, 0, 0, 1,
                 {MakeWholeOpKey(Inst.Sources[0], Mask)}};
  ExprKey CosKey = SinKey;
  CosKey.Opcode = D3D10_SB_OPCODE_SINCOS | (1u << 30);

  const Operand *SinHit = HasSin ? Scope.Find(SinKey) : nullptr;
  const Operand *CosHit = HasCos ? Scope.Find(CosKey) : nullptr;
  if (SinHit && CosHit) {
    const Operand CosDst = Inst.AuxDest, CosVal = *CosHit;
    RecordCopy(Inst.Dest.Index, *SinHit, CopyOf, AnyAlias);
    RecordCopy(CosDst.Index, CosVal, CopyOf, AnyAlias);
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Saturate = false;
    Inst.AuxDest = Operand{OperandKind::Null};
    Inst.Sources = {*SinHit};
    Block.Insts.insert(Block.Insts.begin() + (++I),
                       {D3D10_SB_OPCODE_MOV, CosDst, Operand{OperandKind::Null},
                        {CosVal}});
    return true;
  }
  if (SinHit && !HasCos) {
    RecordCopy(Inst.Dest.Index, *SinHit, CopyOf, AnyAlias);
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Saturate = false;
    Inst.Sources = {*SinHit};
    return true;
  }
  if (CosHit && !HasSin) {
    RecordCopy(Inst.AuxDest.Index, *CosHit, CopyOf, AnyAlias);
    Inst.Opcode = D3D10_SB_OPCODE_MOV;
    Inst.Saturate = false;
    Inst.Dest = Inst.AuxDest;
    Inst.AuxDest = Operand{OperandKind::Null};
    Inst.Sources = {*CosHit};
    return true;
  }
  if (SinHit || CosHit) {
    const Operand HitDst = SinHit ? Inst.Dest : Inst.AuxDest;
    const Operand HitVal = *(SinHit ? SinHit : CosHit);
    RecordCopy(HitDst.Index, HitVal, CopyOf, AnyAlias);
    (SinHit ? Inst.Dest : Inst.AuxDest) = Operand{OperandKind::Null};
    const ExprKey &RemKey = SinHit ? CosKey : SinKey;
    const Operand &RemDst = SinHit ? Inst.AuxDest : Inst.Dest;
    if (!Scope.Find(RemKey))
      Scope.Insert(RemKey, MakeWholeVal(RemDst));
    Block.Insts.insert(Block.Insts.begin() + (++I),
                       {D3D10_SB_OPCODE_MOV, HitDst, Operand{OperandKind::Null},
                        {HitVal}});
    return true;
  }
  if (HasSin) Scope.Insert(SinKey, MakeWholeVal(Inst.Dest));
  if (HasCos) Scope.Insert(CosKey, MakeWholeVal(Inst.AuxDest));
  return false;
}

bool ProcessImulAuxGvn(SsaInst &Inst, uint8_t Flags, ScopedGvnTable &Scope,
                       std::vector<Operand> &CopyOf, bool &AnyAlias) {
  ExprKey MulKey;
  MulKey.Opcode = Inst.Opcode | (1u << 30);
  MulKey.Flags = Flags;
  MulKey.Mask = Inst.AuxDest.Mask;
  MulKey.NumOps = 2;
  MulKey.Ops[0] = MakeWholeOpKey(Inst.Sources[0], Inst.AuxDest.Mask);
  MulKey.Ops[1] = MakeWholeOpKey(Inst.Sources[1], Inst.AuxDest.Mask);
  if (MulKey.Ops[1] < MulKey.Ops[0])
    std::swap(MulKey.Ops[0], MulKey.Ops[1]);
  const Operand *Match = Scope.Find(MulKey);
  if (!Match) {
    Scope.Insert(MulKey, MakeWholeVal(Inst.AuxDest));
    return false;
  }
  RecordCopy(Inst.AuxDest.Index, *Match, CopyOf, AnyAlias);
  Inst.Opcode = D3D10_SB_OPCODE_MOV;
  Inst.Saturate = false;
  Inst.Dest = Inst.AuxDest;
  Inst.AuxDest = Operand{OperandKind::Null};
  Inst.Sources = {*Match};
  return true;
}

bool TryPerLaneGvnLookup(SsaInst &Inst, uint8_t Flags, const MirProgram &Mir,
                         const ScopedGvnTable &Scope) {
  if (IsPseudoOpcode(Inst.Opcode) || !InstructionIsComponentwise(Inst.Opcode) ||
      Inst.Dest.Mask == 0) return false;
  uint32_t HitReg = UINT32_MAX;
  uint8_t HitSwz = 0, FirstHitLane = 0;
  for (unsigned Lane = 0; Lane < kRegisterLanes; ++Lane) {
    if (!(Inst.Dest.Mask & (1u << Lane))) continue;
    ExprKey LaneKey{Inst.Opcode, Flags, 0, 0, 0, 0,
                    static_cast<uint8_t>(Inst.Sources.size()), {}};
    for (size_t S = 0; S < Inst.Sources.size(); ++S)
      LaneKey.Ops[S] = MakeLaneOpKey(Inst.Sources[S], Lane, Mir);
    CanonicalizeKeyCommutativity(Inst.Opcode, LaneKey, true);
    const Operand *Match = Scope.Find(LaneKey);
    if (!Match || Match->Kind != OperandKind::Temp) return false;
    const uint32_t Reg = Match->Index;
    const uint8_t SrcLane = GetSwizzleLane(Match->Swizzle, 0);
    if (HitReg == UINT32_MAX) {
      HitReg = Reg;
      FirstHitLane = SrcLane;
    } else if (HitReg != Reg) {
      return false;
    }
    HitSwz = SetSwizzleLane(HitSwz, Lane, SrcLane);
  }
  if (HitReg == UINT32_MAX) return false;
  for (unsigned Lane = 0; Lane < kRegisterLanes; ++Lane)
    if (!(Inst.Dest.Mask & (1u << Lane)))
      HitSwz = SetSwizzleLane(HitSwz, Lane, FirstHitLane);
  Inst.Opcode = D3D10_SB_OPCODE_MOV;
  Inst.Saturate = false;
  Inst.Sources = {MakeSsaTemp(HitReg, Inst.Dest.Mask, HitSwz)};
  return true;
}

void RecordComponentwiseLanes(const SsaInst &Inst, uint8_t Flags,
                              const MirProgram &Mir, ScopedGvnTable &Scope) {
  if (IsPseudoOpcode(Inst.Opcode) || !InstructionIsComponentwise(Inst.Opcode))
    return;
  for (unsigned Lane = 0; Lane < kRegisterLanes; ++Lane) {
    if (!(Inst.Dest.Mask & (1u << Lane))) continue;
    ExprKey LaneKey{Inst.Opcode, Flags, 0, 0, 0, 0,
                    static_cast<uint8_t>(Inst.Sources.size()), {}};
    for (size_t S = 0; S < Inst.Sources.size(); ++S)
      LaneKey.Ops[S] = MakeLaneOpKey(Inst.Sources[S], Lane, Mir);
    CanonicalizeKeyCommutativity(Inst.Opcode, LaneKey, false);
    if (!Scope.Find(LaneKey))
      Scope.Insert(LaneKey, MakeSsaLane(Inst.Dest.Index, Lane));
  }
}

bool ProcessBlockGvn(SsaProgram &Ssa, SsaBlock &Block, ScopedGvnTable &Scope,
                     std::vector<Operand> &CopyOf, bool &AnyAlias) {
  bool Changed = false;
  for (auto &Phi : Block.Phis) {
    if (Phi.Dead) continue;
    for (auto &In : Phi.Inputs)
      Changed |= ResolveAlias(In.Value, CopyOf);
  }
  for (size_t I = 0; I < Block.Insts.size(); ++I) {
    auto &Inst = Block.Insts[I];
    if (Inst.Dead) continue;
    if (Inst.Dest.RelativeRegister != UINT32_MAX)
      Changed |= ResolveAlias(Inst.Dest, CopyOf);
    for (auto &Src : Inst.Sources)
      Changed |= ResolveAlias(Src, CopyOf);
    if (Inst.Opcode == D3D10_SB_OPCODE_MOV && !Inst.Saturate &&
        Inst.Dest.Kind == OperandKind::Temp &&
        Inst.AuxDest.Kind == OperandKind::Null && !Inst.Sources.empty()) {
      RecordCopy(Inst.Dest.Index, Inst.Sources[0], CopyOf, AnyAlias);
      continue;
    }
    if (!CanGvnOpcode(Inst)) continue;
    const uint8_t Flags = (Inst.Saturate ? 1u : 0u) | (Inst.Precise ? 2u : 0u);
    if (Inst.Opcode == D3D10_SB_OPCODE_SINCOS) {
      Changed |= ProcessSinCosGvn(Block, I, Flags, Scope, CopyOf, AnyAlias);
      continue;
    }
    if (IsImulAuxOnly(Inst)) {
      Changed |= ProcessImulAuxGvn(Inst, Flags, Scope, CopyOf, AnyAlias);
      continue;
    }
    ExprKey Whole;
    Whole.Opcode = Inst.Opcode;
    Whole.Flags = Flags;
    Whole.Mask = Inst.Dest.Mask;
    Whole.Off0 = Inst.TexelOffset[0];
    Whole.Off1 = Inst.TexelOffset[1];
    Whole.Off2 = Inst.TexelOffset[2];
    Whole.NumOps = static_cast<uint8_t>(Inst.Sources.size());
    for (size_t S = 0; S < Inst.Sources.size(); ++S)
      Whole.Ops[S] = MakeWholeOpKey(
          Inst.Sources[S],
          InstructionSourceSlotMask(Inst.Opcode, Inst.Dest.Mask, S));
    CanonicalizeKeyCommutativity(Inst.Opcode, Whole, false);
    if (const Operand *Match = Scope.Find(Whole)) {
      Inst.Opcode = D3D10_SB_OPCODE_MOV;
      Inst.Saturate = false;
      Inst.Sources = {*Match};
      RecordCopy(Inst.Dest.Index, *Match, CopyOf, AnyAlias);
      Changed = true;
      continue;
    }
    if (TryPerLaneGvnLookup(Inst, Flags, Ssa.Mir, Scope)) {
      RecordCopy(Inst.Dest.Index, Inst.Sources[0], CopyOf, AnyAlias);
      Changed = true;
      continue;
    }
    Scope.Insert(Whole, MakeWholeVal(Inst.Dest));
    RecordComponentwiseLanes(Inst, Flags, Ssa.Mir, Scope);
  }
  if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
    Changed |= ResolveAlias(Block.Condition, CopyOf);
  return Changed;
}

bool RunGlobalGvn(SsaProgram &Ssa, const SsaDomTree &Dom) {
  if (Dom.RpoOrder.empty()) return false;
  ScopedGvnTable Scope;
  std::vector<Operand> CopyOf(Ssa.NextValueId, Operand{OperandKind::Null});
  bool AnyAlias = false;
  struct Frame {
    uint32_t Block;
    size_t Mark;
    bool Exit;
  };
  llvm::SmallVector<Frame, 64> Stack;
  Stack.push_back({0, 0, false});

  bool Changed = false;
  while (!Stack.empty()) {
    const auto Cur = Stack.pop_back_val();
    if (Cur.Exit) {
      Scope.Rollback(Cur.Mark);
      continue;
    }
    const size_t Mark = Scope.Mark();
    Changed |= ProcessBlockGvn(Ssa, Ssa.Blocks[Cur.Block], Scope, CopyOf, AnyAlias);
    Stack.push_back({Cur.Block, Mark, true});
    for (uint32_t Child : Dom.Children[Cur.Block])
      Stack.push_back({Child, 0, false});
  }
  if (AnyAlias) {
    for (auto &Block : Ssa.Blocks)
      for (auto &Phi : Block.Phis)
        if (!Phi.Dead)
          for (auto &In : Phi.Inputs)
            Changed |= ResolveAlias(In.Value, CopyOf);
  }
  return Changed;
}

} // namespace

bool dxcb5::RunOptGvnDce(SsaProgram &Ssa, const SsaDomTree *CachedDom) {
  llvm::TimeTraceScope Scope("DXCB5 Opt GVN/DCE", llvm::StringRef());
  SsaDomTree LocalDom;
  if (!CachedDom) {
    Ssa.ComputeDominators(LocalDom);
    CachedDom = &LocalDom;
  }
  bool Changed = RunGlobalGvn(Ssa, *CachedDom);
  Changed |= RunOptMaskedDce(Ssa);
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}
