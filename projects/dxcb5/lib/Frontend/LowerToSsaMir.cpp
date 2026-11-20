// Distributed under the University of Illinois Open Source License.
#include "Frontend/LowerToSsaMir.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>

using namespace dxcb5;
using namespace llvm;

namespace {
using I = llvm::Instruction;

bool IsSupportedScalarType(const Type *Ty) {
  if (Ty->isFloatTy() || Ty->isHalfTy())
    return true;
  if (const auto *IT = dyn_cast<IntegerType>(Ty))
    return IT->getBitWidth() <= 32;
  return false;
}

bool TryExtractConstantBits(const Constant *C, uint32_t &Bits) {
  if (isa<UndefValue>(C)) {
    if (!IsSupportedScalarType(C->getType()))
      return false;
    Bits = 0;
    return true;
  }
  if (const auto *CI = dyn_cast<ConstantInt>(C)) {
    if (CI->getBitWidth() > 32)
      return false;
    Bits = CI->getBitWidth() == 1 ? (CI->isZero() ? 0u : UINT32_MAX)
                                  : static_cast<uint32_t>(CI->getZExtValue());
    return true;
  }
  if (const auto *CF = dyn_cast<ConstantFP>(C)) {
    if (CF->getType()->isHalfTy()) {
      APFloat Val = CF->getValueAPF();
      bool LosesInfo = false;
      Val.convert(APFloat::IEEEsingle, APFloat::rmNearestTiesToEven, &LosesInfo);
      Bits = static_cast<uint32_t>(Val.bitcastToAPInt().getZExtValue());
      return true;
    }
    if (!CF->getType()->isFloatTy())
      return false;
    Bits = static_cast<uint32_t>(
        CF->getValueAPF().bitcastToAPInt().getZExtValue());
    return true;
  }
  return false;
}

bool IsPositiveZeroScalar(const Value *V) {
  const auto *C = dyn_cast<ConstantFP>(V);
  return C && (C->getType()->isFloatTy() || C->getType()->isHalfTy()) &&
         C->getValueAPF().isZero() && !C->getValueAPF().isNegative();
}

bool IsPositiveZeroValue(const Value *V) {
  if (IsPositiveZeroScalar(V))
    return true;
  const auto *C = dyn_cast<Constant>(V);
  const auto *VT = C ? dyn_cast<VectorType>(C->getType()) : nullptr;
  if (!VT || (!VT->getElementType()->isFloatTy() &&
              !VT->getElementType()->isHalfTy()))
    return false;
  for (unsigned L = 0; L < VT->getNumElements(); ++L)
    if (!IsPositiveZeroScalar(C->getAggregateElement(L)))
      return false;
  return true;
}

const I *SingleUseInBlock(const Value *V, const BasicBlock *BB) {
  const auto *Inst = dyn_cast<I>(V);
  return Inst && Inst->hasOneUse() && Inst->getParent() == BB ? Inst : nullptr;
}

struct OrderedZeroCmp {
  const FCmpInst *Flag = nullptr;
  const Value *Input = nullptr;
};

OrderedZeroCmp MatchZeroCmp(const I &Ext, CmpInst::Predicate Expected) {
  if (Ext.getOpcode() != I::ZExt ||
      !Ext.getType()->getScalarType()->isIntegerTy(32))
    return {};
  const auto *Cmp = dyn_cast<FCmpInst>(Ext.getOperand(0));
  if (!Cmp || !Cmp->getOperand(0)->getType()->getScalarType()->isFloatTy())
    return {};
  const auto Pred = Cmp->getPredicate();
  if (Pred == Expected && IsPositiveZeroValue(Cmp->getOperand(1)))
    return {Cmp, Cmp->getOperand(0)};
  if (CmpInst::getSwappedPredicate(Pred) == Expected &&
      IsPositiveZeroValue(Cmp->getOperand(0)))
    return {Cmp, Cmp->getOperand(1)};
  return {};
}

Operand BuildSingleSourceShuffle(const ShuffleVectorInst &SV, Operand Base,
                                 unsigned OutW, unsigned Offset) {
  if (Base.Kind == OperandKind::Immediate)
    return Base;
  unsigned Fallback = 0;
  for (unsigned L = 0; L < OutW; ++L) {
    const int M = SV.getMaskValue(L);
    if (M >= 0) {
      Fallback = GetSwizzleLane(Base.Swizzle, static_cast<unsigned>(M) - Offset);
      break;
    }
  }
  uint8_t Swz = 0;
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    unsigned Lane = Fallback;
    if (L < OutW) {
      const int M = SV.getMaskValue(L);
      if (M >= 0)
        Lane = GetSwizzleLane(Base.Swizzle, static_cast<unsigned>(M) - Offset);
    }
    Swz = SetSwizzleLane(Swz, L, Lane);
  }
  Base.Mask = MaskForWidth(OutW);
  Base.Swizzle = Swz;
  return Base;
}

bool TryCombineImmediateLanes(SsaProgram &Ssa, ArrayRef<Operand> Lanes,
                              Operand &Out) {
  std::array<uint32_t, 4> Bits{};
  bool SameImm = true;
  for (unsigned L = 0; L < Lanes.size(); ++L) {
    if (Lanes[L].Kind != OperandKind::Immediate || Lanes[L].Modifier)
      return false;
    Bits[L] = Lanes[L].Index;
    if (L && Bits[L] != Bits[0])
      SameImm = false;
  }
  if (SameImm) {
    Out = MakeImm(Bits[0]);
    return true;
  }
  Out = {OperandKind::ImmediateVector, Ssa.InternLiteralVector(Bits), 0,
         MaskForWidth(static_cast<unsigned>(Lanes.size())), kIdentitySwizzle};
  return true;
}

bool TryCombineDirectLanes(ArrayRef<Operand> Lanes, Operand &Out) {
  const auto K = Lanes[0].Kind;
  if (K != OperandKind::Temp && K != OperandKind::Input &&
      K != OperandKind::ConstantBuffer)
    return false;
  for (unsigned L = 1; L < Lanes.size(); ++L)
    if (!SameOperandBase(Lanes[0], Lanes[L]))
      return false;
  Out = Lanes[0];
  Out.Mask = MaskForWidth(static_cast<unsigned>(Lanes.size()));
  uint8_t Swz = 0;
  const unsigned FirstLane = GetSwizzleLane(Lanes[0].Swizzle, 0);
  for (unsigned L = 0; L < kRegisterLanes; ++L) {
    const unsigned Src =
        L < Lanes.size() ? GetSwizzleLane(Lanes[L].Swizzle, 0) : FirstLane;
    Swz = SetSwizzleLane(Swz, L, Src);
  }
  Out.Swizzle = Swz;
  return true;
}

bool TryBindShuffleAlias(SsaLowerer &Lowerer, const ShuffleVectorInst &SV,
                         bool &Deferred) {
  const auto *VT = dyn_cast<VectorType>(SV.getType());
  const unsigned OutW = VT->getNumElements();
  if (OutW > kRegisterLanes)
    return Lowerer.Fail("shufflevector width exceeds 4");
  const auto *V0 = SV.getOperand(0);
  const auto *V1 = SV.getOperand(1);
  const unsigned InW = cast<VectorType>(V0->getType())->getNumElements();
  if ((!isa<Constant>(V0) && !Lowerer.Values.count(V0)) ||
      (!isa<Constant>(V1) && !Lowerer.Values.count(V1))) {
    Deferred = true;
    return true;
  }
  bool UsesV0 = false, UsesV1 = false;
  for (unsigned L = 0; L < OutW; ++L) {
    const int MaskIdx = SV.getMaskValue(L);
    if (MaskIdx < 0)
      continue;
    if (static_cast<unsigned>(MaskIdx) < InW)
      UsesV0 = true;
    else
      UsesV1 = true;
  }
  if (UsesV0 != UsesV1) {
    const Operand Base = Lowerer.Source(UsesV0 ? V0 : V1);
    if (Base.Kind != OperandKind::ImmediateVector) {
      Lowerer.Values[&SV] =
          BuildSingleSourceShuffle(SV, Base, OutW, UsesV0 ? 0 : InW);
      return true;
    }
  }
  if (!Lowerer.Values.count(&SV)) {
    Lowerer.Values[&SV] = Lowerer.AllocTemp(MaskForWidth(OutW));
    Lowerer.EmittedShuffles.insert(&SV);
  }
  return true;
}

} // namespace

bool SsaLowerer::Fail(const Twine &Reason) {
  if (Error.empty())
    Error = Reason.str();
  return false;
}

Operand SsaLowerer::AllocTemp(uint8_t Mask) {
  const uint32_t Id = Ssa.AllocValue();
  const uint8_t Swizzle = PopcountMask(Mask) == 1 ? 0u : kIdentitySwizzle;
  return MakeSsaTemp(Id, Mask, Swizzle);
}

Operand SsaLowerer::Emit(uint32_t Opcode, Operand Dest,
                         ArrayRef<Operand> Sources, bool Precise,
                         bool Saturate) {
  SsaInst Inst;
  Inst.Opcode = Opcode;
  Inst.Dest = Dest;
  Inst.Sources.append(Sources.begin(), Sources.end());
  Inst.Precise = Precise;
  Inst.Saturate = Saturate;
  CurrentBlock->Insts.push_back(std::move(Inst));
  return Dest;
}

Operand SsaLowerer::Source(const Value *V) {
  if (const auto *C = dyn_cast<Constant>(V)) {
    uint32_t Bits = 0;
    if (TryExtractConstantBits(C, Bits))
      return MakeImm(Bits);
    if (const auto *VT = dyn_cast<VectorType>(C->getType())) {
      const unsigned W = VT->getNumElements();
      std::array<uint32_t, 4> Lanes{};
      bool AllEqual = true;
      for (unsigned Lane = 0; Lane < W; ++Lane) {
        if (!TryExtractConstantBits(C->getAggregateElement(Lane), Lanes[Lane])) {
          Fail("unsupported vector constant element");
          return MakeImm(0);
        }
        if (Lane && Lanes[Lane] != Lanes[0])
          AllEqual = false;
      }
      if (AllEqual)
        return MakeImm(Lanes[0]);
      const uint32_t Idx = Ssa.InternLiteralVector(Lanes);
      return {OperandKind::ImmediateVector, Idx, 0, MaskForWidth(W),
              kIdentitySwizzle};
    }
    Fail("unsupported non-32-bit constant in SM5");
    return MakeImm(0);
  }
  auto It = Values.find(V);
  if (It != Values.end())
    return It->second;
  Fail("DXIL operand has no SSA register");
  return MakeImm(0);
}

Operand SsaLowerer::SourceInt(const Value *V, bool Signed, bool Precise) {
  const auto *IT = dyn_cast<IntegerType>(V->getType()->getScalarType());
  if (!IT || IT->getBitWidth() <= 1 || IT->getBitWidth() >= 32)
    return Source(V);
  const unsigned BW = IT->getBitWidth();
  if (const auto *CI = dyn_cast<ConstantInt>(V)) {
    const uint32_t Bits = static_cast<uint32_t>(
        Signed ? CI->getSExtValue() : CI->getZExtValue());
    return MakeImm(Bits);
  }
  const Operand Raw = Source(V);
  const Operand Out = AllocTemp(Raw.Mask);
  if (Signed)
    Emit(D3D11_SB_OPCODE_IBFE, Out, {MakeImm(BW), MakeImm(0), Raw}, Precise);
  else
    Emit(D3D10_SB_OPCODE_AND, Out, {Raw, MakeImm((1u << BW) - 1u)}, Precise);
  return Out;
}

Operand SsaLowerer::SourceScalarLane(const Value *V, unsigned Lane) {
  Operand Op = Source(V);
  if (Op.Kind == OperandKind::Immediate)
    return Op;
  if (Op.Kind == OperandKind::ImmediateVector) {
    const unsigned SrcLane = GetSwizzleLane(Op.Swizzle, Lane);
    return MakeImm(Ssa.Mir.LiteralVectors[Op.Index][SrcLane]);
  }
  const unsigned SrcLane = GetSwizzleLane(Op.Swizzle, Lane);
  Op.Mask = 1;
  Op.Swizzle = BroadcastLaneSwizzle(SrcLane);
  return Op;
}

Operand SsaLowerer::RelativeIndex(const Value *V) {
  auto Index = Source(V);
  if (Index.Kind == OperandKind::Temp && !Index.Modifier)
    return Index;
  auto Reg = AllocTemp(1);
  Emit(D3D10_SB_OPCODE_MOV, Reg, {Index});
  return Reg;
}

Operand SsaLowerer::CombineOperands(ArrayRef<Operand> Lanes, bool Precise) {
  assert(!Lanes.empty() && Lanes.size() <= kRegisterLanes);
  if (Lanes.size() == 1)
    return Lanes[0];
  Operand Direct;
  if (TryCombineImmediateLanes(Ssa, Lanes, Direct) ||
      TryCombineDirectLanes(Lanes, Direct))
    return Direct;
  auto Dest = AllocTemp(MaskForWidth(static_cast<unsigned>(Lanes.size())));
  Emit(PseudoCombineVec, Dest, Lanes, Precise);
  return Dest;
}

Operand SsaLowerer::VectorArgs(CallInst &Call, unsigned Start, unsigned Count) {
  SmallVector<Operand, 4> Lanes;
  Lanes.reserve(Count);
  for (unsigned L = 0; L < Count; ++L)
    Lanes.push_back(Source(Call.getArgOperand(Start + L)));
  return CombineOperands(Lanes);
}

bool SsaLowerer::TryBindAlias(const llvm::Instruction &Inst, bool &Deferred) {
  if (const auto *Extract = dyn_cast<ExtractValueInst>(&Inst)) {
    const unsigned Lane = *Extract->idx_begin();
    if (Extract->getNumIndices() != 1 || Lane >= kRegisterLanes)
      return Fail("unsupported aggregate component");
    const auto *Agg = Extract->getAggregateOperand();
    if (!isa<Constant>(Agg) && !Values.count(Agg)) {
      Deferred = true;
      return true;
    }
    auto From = Source(Agg);
    From.Mask = 1;
    From.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(From.Swizzle, Lane));
    Values[Extract] = From;
    return Error.empty();
  }
  if (const auto *EE = dyn_cast<ExtractElementInst>(&Inst)) {
    const auto *Idx = dyn_cast<ConstantInt>(EE->getIndexOperand());
    if (!Idx) {
      if (!Values.count(EE))
        Values[EE] = AllocTemp(1);
      return true;
    }
    const unsigned Lane = static_cast<unsigned>(Idx->getZExtValue());
    if (Lane >= kRegisterLanes)
      return Fail("extractelement lane out of SM5 range");
    const auto *Vec = EE->getVectorOperand();
    if (!isa<Constant>(Vec) && !Values.count(Vec)) {
      Deferred = true;
      return true;
    }
    Values[EE] = SourceScalarLane(Vec, Lane);
    return Error.empty();
  }
  if (const auto *SV = dyn_cast<ShuffleVectorInst>(&Inst))
    return TryBindShuffleAlias(*this, *SV, Deferred);
  return true;
}

void SsaLowerer::PlanVectorPhiGroups(ArrayRef<VectorPhiGroup> PhiGroups) {
  for (const auto &Group : PhiGroups) {
    VectorPhiEntry Entry;
    const BasicBlock *Parent = nullptr;
    bool Valid = true;
    for (unsigned Slot = 0; Slot < Group.size(); ++Slot) {
      const auto *Inst = Group[Slot];
      if (!Inst)
        continue;
      const auto *Phi = dyn_cast<PHINode>(Inst);
      if (Slot != Entry.Width || !Phi || GroupedPhis.count(Phi) ||
          (!Phi->getType()->isFloatTy() && !Phi->getType()->isIntegerTy(32)) ||
          (Entry.Width && (Phi->getParent() != Parent ||
                           Phi->getType() != Entry.Phis[0]->getType()))) {
        Valid = false;
        break;
      }
      if (std::find(Entry.Phis.begin(), Entry.Phis.begin() + Slot, Phi) !=
          Entry.Phis.begin() + Slot) {
        Valid = false;
        break;
      }
      Parent = Phi->getParent();
      Entry.Phis[Slot] = Phi;
      ++Entry.Width;
    }
    if (!Valid || Entry.Width < 2)
      continue;
    Entry.ResultId = Ssa.AllocValue();
    for (unsigned Lane = 0; Lane < Entry.Width; ++Lane) {
      GroupedPhis.insert(Entry.Phis[Lane]);
      Values[Entry.Phis[Lane]] = MakeSsaLane(Entry.ResultId, Lane);
    }
    BlockVectorPhis[Parent].push_back(Entry);
  }
}

void SsaLowerer::MatchSignIdioms(BasicBlock &Block) {
  for (const auto &Inst : Block) {
    if (Inst.getOpcode() != I::SIToFP ||
        !Inst.getType()->getScalarType()->isFloatTy())
      continue;
    const auto *Diff = SingleUseInBlock(Inst.getOperand(0), &Block);
    if (!Diff || Diff->getOpcode() != I::Sub)
      continue;
    const auto *Pos = SingleUseInBlock(Diff->getOperand(0), &Block);
    const auto *Neg = SingleUseInBlock(Diff->getOperand(1), &Block);
    if (!Pos || !Neg)
      continue;
    const auto Gt = MatchZeroCmp(*Pos, CmpInst::FCMP_OGT);
    const auto Lt = MatchZeroCmp(*Neg, CmpInst::FCMP_OLT);
    if (!Gt.Input || Gt.Input != Lt.Input)
      continue;
    SignPatterns[&Inst] = {Gt.Flag, Lt.Flag};
    Elided.insert(Diff);
    Elided.insert(Pos);
    Elided.insert(Neg);
  }
}

bool SsaLowerer::LowerBlockPhis(BasicBlock &Block, SsaBlock &Out) {
  auto GroupIt = BlockVectorPhis.find(&Block);
  if (GroupIt != BlockVectorPhis.end()) {
    for (const auto &Entry : GroupIt->second) {
      SsaPhi Phi;
      Phi.Result = Entry.ResultId;
      Phi.Mask = MaskForWidth(Entry.Width);
      const auto *First = Entry.Phis[0];
      for (unsigned L = 0; L < Entry.Width; ++L)
        Phi.Precise |= Module.IsPrecise(Entry.Phis[L]);
      for (unsigned Idx = 0; Idx < First->getNumIncomingValues(); ++Idx) {
        const auto *Pred = First->getIncomingBlock(Idx);
        const uint32_t PredId = BlockIDs.lookup(Pred);
        SsaBlock *Saved = CurrentBlock;
        CurrentBlock = &Ssa.Blocks[PredId];
        SmallVector<Operand, 4> Lanes;
        Lanes.reserve(Entry.Width);
        for (unsigned L = 0; L < Entry.Width; ++L)
          Lanes.push_back(Source(Entry.Phis[L]->getIncomingValueForBlock(Pred)));
        Operand Combined = CombineOperands(Lanes, Phi.Precise);
        CurrentBlock = Saved;
        Phi.Inputs.push_back({PredId, Combined});
      }
      Out.Phis.push_back(std::move(Phi));
    }
  }
  for (auto &Inst : Block) {
    auto *P = dyn_cast<PHINode>(&Inst);
    if (!P)
      break;
    if (GroupedPhis.count(P))
      continue;
    const auto Dest = Values.lookup(P);
    SsaPhi Phi;
    Phi.Result = Dest.Index;
    Phi.Mask = Dest.Mask;
    Phi.Precise = Module.IsPrecise(P);
    for (unsigned Idx = 0; Idx < P->getNumIncomingValues(); ++Idx) {
      const uint32_t PredId = BlockIDs.lookup(P->getIncomingBlock(Idx));
      Phi.Inputs.push_back({PredId, Source(P->getIncomingValue(Idx))});
    }
    Out.Phis.push_back(std::move(Phi));
  }
  return Error.empty();
}

bool SsaLowerer::Run(ArrayRef<VectorPhiGroup> PhiGroups) {
  TimeTraceScope Scope("DXCB5 Lower To SSA MIR", StringRef());
  auto &Entry = *Module.GetEntryFunction();
  Ssa.Blocks.resize(Entry.size());

  uint32_t NextBlock = 0;
  for (auto &Block : Entry) {
    BlockIDs[&Block] = NextBlock++;
    for (auto &Inst : Block) {
      const Type *Ty = Inst.getType();
      if (!Ty->isVoidTy() && !Ty->isPointerTy() && !Ty->isStructTy() &&
          !IsSupportedScalarType(Ty->getScalarType()))
        return Fail("unsupported non-32-bit numeric type in SM5");
      if (!BindValue(Inst))
        return false;
    }
  }

  if (!PhiGroups.empty())
    PlanVectorPhiGroups(PhiGroups);

  DominatorTree Dominators;
  PlanLocalConstants(Entry, Dominators, ConstantTuples);
  for (const auto &KV : ConstantTuples.Loads) {
    const auto &Tuple = KV.second;
    const uint32_t Id = Ssa.AllocValue();
    for (unsigned Lane = 0; Lane < Tuple.Width; ++Lane)
      Values[Tuple.LaneLoads[Lane]] = MakeSsaLane(Id, Lane);
  }

  SmallVector<const llvm::Instruction *, 64> Pending;
  for (const auto &Block : Entry) {
    for (const auto &Inst : Block)
      if (isa<ExtractValueInst>(Inst) || isa<ExtractElementInst>(Inst) ||
          isa<ShuffleVectorInst>(Inst))
        Pending.push_back(&Inst);
  }
  while (!Pending.empty()) {
    SmallVector<const llvm::Instruction *, 32> NextPending;
    for (const auto *Inst : Pending) {
      bool Deferred = false;
      if (!TryBindAlias(*Inst, Deferred))
        return false;
      if (Deferred)
        NextPending.push_back(Inst);
    }
    if (NextPending.size() == Pending.size())
      return Fail("cyclic vector alias in LLVM IR");
    Pending = std::move(NextPending);
  }

  CurrentBlock = &Ssa.Blocks[0];
  if (!InitMutableGlobals())
    return false;
  for (auto &Block : Entry) {
    auto &Out = Ssa.Blocks[BlockIDs.lookup(&Block)];
    CurrentBlock = &Out;
    Elided.clear();
    SignPatterns.clear();
    MatchSignIdioms(Block);
    for (auto &Inst : Block) {
      if ((!ConstantTuples.Omitted.empty() &&
           ConstantTuples.Omitted.count(&Inst)) ||
          (!Elided.empty() && Elided.count(&Inst)))
        continue;
      if (!LowerInstruction(Inst))
        return false;
    }
    if (!LowerTerminator(Block, Out))
      return false;
  }

  for (auto &Block : Entry)
    if (!LowerBlockPhis(Block, Ssa.Blocks[BlockIDs.lookup(&Block)]))
      return false;

  return Error.empty();
}

bool SsaLowerer::LowerSignedDivRem(llvm::Instruction &Inst, Operand Dest,
                                   bool Precise) {
  const auto Left = SourceInt(Inst.getOperand(0), true, Precise);
  const auto Right = SourceInt(Inst.getOperand(1), true, Precise);
  const uint8_t M = Dest.Mask;
  const auto NegA = AllocTemp(M);
  const auto AbsA = AllocTemp(M);
  const auto NegB = AllocTemp(M);
  const auto AbsB = AllocTemp(M);
  Emit(D3D10_SB_OPCODE_INEG, NegA, {Left}, Precise);
  Emit(D3D10_SB_OPCODE_IMAX, AbsA, {Left, NegA}, Precise);
  Emit(D3D10_SB_OPCODE_INEG, NegB, {Right}, Precise);
  Emit(D3D10_SB_OPCODE_IMAX, AbsB, {Right, NegB}, Precise);

  const auto Mag = AllocTemp(M);
  const bool IsRem = (Inst.getOpcode() == llvm::Instruction::SRem);
  SsaInst Div;
  Div.Opcode = D3D10_SB_OPCODE_UDIV;
  Div.Dest = IsRem ? Operand() : Mag;
  Div.AuxDest = IsRem ? Mag : Operand();
  Div.Sources = {AbsA, AbsB};
  Div.Precise = Precise;
  CurrentBlock->Insts.push_back(std::move(Div));

  const auto SignCond = AllocTemp(M);
  if (IsRem) {
    Emit(D3D10_SB_OPCODE_ILT, SignCond, {Left, MakeImm(0)}, Precise);
  } else {
    const auto XorSign = AllocTemp(M);
    Emit(D3D10_SB_OPCODE_XOR, XorSign, {Left, Right}, Precise);
    Emit(D3D10_SB_OPCODE_ILT, SignCond, {XorSign, MakeImm(0)}, Precise);
  }
  const auto NegMag = AllocTemp(M);
  Emit(D3D10_SB_OPCODE_INEG, NegMag, {Mag}, Precise);
  Emit(D3D10_SB_OPCODE_MOVC, Dest, {SignCond, NegMag, Mag}, Precise);
  return Error.empty();
}

bool dxcb5::LowerToSsaMir(hlsl::DxilModule &Module, const Interface &Layout,
                          SsaProgram &Ssa, std::string &Error,
                          ArrayRef<VectorPhiGroup> PhiGroups) {
  return SsaLowerer(Module, Layout, Ssa, Error).Run(PhiGroups);
}
