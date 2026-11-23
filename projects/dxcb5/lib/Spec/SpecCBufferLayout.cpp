// Distributed under the University of Illinois Open Source License.
#include "Spec/SpecInternal.h"
#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilTypeSystem.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include <algorithm>
#include <cstdint>

using namespace dxcb5::spec;
using namespace hlsl;
using namespace llvm;

namespace {

constexpr uint64_t kU32Max = 0xffffffffULL;
constexpr uint64_t kI32Max = 0x7fffffffULL;

unsigned ScalarLegacyByteSize(Type *Ty) {
  while (auto *Arr = dyn_cast<ArrayType>(Ty))
    Ty = Arr->getElementType();
  Type *ST = Ty->getScalarType();
  // Bail out on 64-bit and native 16-bit types in legacy 32-bit cbuffer layout.
  if (ST->isDoubleTy() || ST->isIntegerTy(64) || ST->isHalfTy() ||
      ST->isIntegerTy(16))
    return 0u;
  return 4u;
}

uint64_t ComputePossibleOneBits(const Value *V, unsigned Depth = 0) {
  if (Depth > 6 || !V)
    return kU32Max;
  if (const auto *CI = dyn_cast<ConstantInt>(V))
    return CI->getZExtValue() & kU32Max;
  if (const auto *ZE = dyn_cast<ZExtInst>(V)) {
    const unsigned SrcBits = ZE->getSrcTy()->getIntegerBitWidth();
    const uint64_t Mask = SrcBits >= 32 ? kU32Max : ((1ULL << SrcBits) - 1ULL);
    return ComputePossibleOneBits(ZE->getOperand(0), Depth + 1) & Mask;
  }
  const auto *BO = dyn_cast<BinaryOperator>(V);
  if (!BO)
    return kU32Max;
  const unsigned Op = BO->getOpcode();
  if (Op == Instruction::And)
    return ComputePossibleOneBits(BO->getOperand(0), Depth + 1) &
           ComputePossibleOneBits(BO->getOperand(1), Depth + 1);
  if (Op == Instruction::Or)
    return ComputePossibleOneBits(BO->getOperand(0), Depth + 1) |
           ComputePossibleOneBits(BO->getOperand(1), Depth + 1);
  if (Op == Instruction::Shl) {
    if (const auto *Amt = dyn_cast<ConstantInt>(BO->getOperand(1))) {
      const uint64_t S = Amt->getZExtValue();
      return S >= 32 ? 0ULL
                     : ((ComputePossibleOneBits(BO->getOperand(0), Depth + 1) << S) &
                        kU32Max);
    }
  }
  if (Op == Instruction::LShr) {
    if (const auto *Amt = dyn_cast<ConstantInt>(BO->getOperand(1))) {
      const uint64_t S = Amt->getZExtValue();
      return S >= 32 ? 0ULL
                     : (ComputePossibleOneBits(BO->getOperand(0), Depth + 1) >> S);
    }
  }
  return kU32Max;
}

uint64_t ComputeSmallUpperBound(const Value *V, unsigned Depth = 0) {
  if (Depth > 4 || !V)
    return kU32Max;
  if (const auto *CI = dyn_cast<ConstantInt>(V))
    return CI->getSExtValue() >= 0 ? static_cast<uint64_t>(CI->getSExtValue())
                                   : kU32Max;
  if (const auto *Call = dyn_cast<CallInst>(V)) {
    if (OP::IsDxilOpFuncCallInst(Call, DXIL::OpCode::IMin) ||
        OP::IsDxilOpFuncCallInst(Call, DXIL::OpCode::UMin)) {
      const uint64_t A = ComputeSmallUpperBound(Call->getArgOperand(1), Depth + 1);
      const uint64_t B = ComputeSmallUpperBound(Call->getArgOperand(2), Depth + 1);
      return std::min(A, B);
    }
  }
  return ComputePossibleOneBits(V, Depth);
}

void CollectGuardCond(const Value *Cond, bool ExpectTrue,
                      SmallPtrSetImpl<const Value *> &NonZero, unsigned Depth) {
  if (Depth > 4 || !Cond)
    return;
  if (const auto *IC = dyn_cast<ICmpInst>(Cond)) {
    const auto Pred = IC->getPredicate();
    const Value *LHS = IC->getOperand(0), *RHS = IC->getOperand(1);
    const auto *RC = dyn_cast<ConstantInt>(RHS);
    bool ZeroOnRHS = true;
    if (!RC) {
      RC = dyn_cast<ConstantInt>(LHS);
      LHS = RHS;
      ZeroOnRHS = false;
    }
    if (RC && RC->isZero()) {
      const bool IsNe = (Pred == ICmpInst::ICMP_EQ && !ExpectTrue) ||
                        (Pred == ICmpInst::ICMP_NE && ExpectTrue);
      const bool IsGtZero =
          ZeroOnRHS
              ? (((Pred == ICmpInst::ICMP_UGT || Pred == ICmpInst::ICMP_SGT) &&
                  ExpectTrue) ||
                 ((Pred == ICmpInst::ICMP_ULE || Pred == ICmpInst::ICMP_SLE) &&
                  !ExpectTrue))
              : (((Pred == ICmpInst::ICMP_ULT || Pred == ICmpInst::ICMP_SLT) &&
                  ExpectTrue) ||
                 ((Pred == ICmpInst::ICMP_UGE || Pred == ICmpInst::ICMP_SGE) &&
                  !ExpectTrue));
      if (IsNe || IsGtZero)
        NonZero.insert(LHS);
    }
    return;
  }
  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO)
    return;
  if ((BO->getOpcode() == Instruction::Or && !ExpectTrue) ||
      (BO->getOpcode() == Instruction::And && ExpectTrue)) {
    CollectGuardCond(BO->getOperand(0), ExpectTrue, NonZero, Depth + 1);
    CollectGuardCond(BO->getOperand(1), ExpectTrue, NonZero, Depth + 1);
  }
}

void CollectDominatingNonZero(const BasicBlock *BB,
                              SmallPtrSetImpl<const Value *> &NonZero) {
  const BasicBlock *Cur = BB;
  for (unsigned Step = 0; Step < 8 && Cur; ++Step) {
    const BasicBlock *Pred = Cur->getSinglePredecessor();
    if (!Pred)
      break;
    if (const auto *BI = dyn_cast<BranchInst>(Pred->getTerminator())) {
      if (BI->isConditional() && BI->getSuccessor(0) != BI->getSuccessor(1))
        CollectGuardCond(BI->getCondition(), BI->getSuccessor(0) == Cur,
                         NonZero, 0);
    }
    Cur = Pred;
  }
}

bool ComputeDynamicRange(const Value *V,
                         const SmallPtrSetImpl<const Value *> &NonZero,
                         uint64_t &OutMin, uint64_t &OutMax,
                         unsigned Depth = 0);

bool MatchLoopLatchBound(const BasicBlock *Latch, const BasicBlock *Header,
                         const Value *Iv, const Value *NextIv,
                         uint64_t &OutMax) {
  const auto *BI = dyn_cast<BranchInst>(Latch->getTerminator());
  if (!BI || !BI->isConditional())
    return false;
  const auto *IC = dyn_cast<ICmpInst>(BI->getCondition());
  if (!IC)
    return false;
  const Value *L = IC->getOperand(0), *R = IC->getOperand(1);
  const Value *Bound = nullptr;
  bool VarOnLeft = true;
  if (L == Iv || L == NextIv) {
    Bound = R;
  } else if (R == Iv || R == NextIv) {
    Bound = L;
    VarOnLeft = false;
  } else {
    return false;
  }
  const bool ContOnTrue = BI->getSuccessor(0) == Header;
  const bool ContOnFalse = BI->getSuccessor(1) == Header;
  if (ContOnTrue == ContOnFalse)
    return false;
  const auto Pred = IC->getPredicate();
  const bool UpperBounded =
      (Pred == ICmpInst::ICMP_EQ && ContOnFalse) ||
      (Pred == ICmpInst::ICMP_NE && ContOnTrue) ||
      (VarOnLeft && ContOnTrue &&
       (Pred == ICmpInst::ICMP_ULT || Pred == ICmpInst::ICMP_SLT ||
        Pred == ICmpInst::ICMP_ULE || Pred == ICmpInst::ICMP_SLE)) ||
      (!VarOnLeft && ContOnTrue &&
       (Pred == ICmpInst::ICMP_UGT || Pred == ICmpInst::ICMP_SGT ||
        Pred == ICmpInst::ICMP_UGE || Pred == ICmpInst::ICMP_SGE));
  if (!UpperBounded)
    return false;
  const uint64_t BMax = ComputeSmallUpperBound(Bound);
  if (BMax > 65536ULL)
    return false;
  OutMax = BMax;
  return true;
}

bool ComputeMonotonicPhiRange(const PHINode *PN,
                              const SmallPtrSetImpl<const Value *> &NonZero,
                              uint64_t &OutMin, uint64_t &OutMax,
                              unsigned Depth) {
  if (PN->getNumIncomingValues() != 2)
    return false;
  for (unsigned Arm = 0; Arm < 2; ++Arm) {
    uint64_t InitMin = 0, InitMax = 0;
    if (!ComputeDynamicRange(PN->getIncomingValue(Arm), NonZero, InitMin,
                             InitMax, Depth + 1))
      continue;
    const auto *Add = dyn_cast<BinaryOperator>(PN->getIncomingValue(1 - Arm));
    if (!Add || Add->getOpcode() != Instruction::Add ||
        (!Add->hasNoUnsignedWrap() && !Add->hasNoSignedWrap()))
      continue;
    const Value *Other =
        Add->getOperand(0) == PN
            ? Add->getOperand(1)
            : (Add->getOperand(1) == PN ? Add->getOperand(0) : nullptr);
    const auto *Step = dyn_cast_or_null<ConstantInt>(Other);
    if (!Step || Step->getSExtValue() < 0)
      continue;
    OutMin = InitMin;
    OutMax = Add->hasNoSignedWrap() ? kI32Max : kU32Max;
    const BasicBlock *Latch = PN->getIncomingBlock(1 - Arm);
    uint64_t LatchMax = kU32Max;
    if (MatchLoopLatchBound(Latch, PN->getParent(), PN, Add, LatchMax))
      OutMax = std::max(InitMax, LatchMax);
    return true;
  }
  return false;
}

bool ComputeBinOpRange(const BinaryOperator *BO,
                       const SmallPtrSetImpl<const Value *> &NonZero,
                       uint64_t &OutMin, uint64_t &OutMax, unsigned Depth) {
  const unsigned Op = BO->getOpcode();
  if (Op == Instruction::And) {
    const uint64_t Mask = ComputePossibleOneBits(BO, Depth);
    if (Mask <= kI32Max) {
      OutMin = 0;
      OutMax = Mask;
      return true;
    }
    return false;
  }
  if (Op == Instruction::LShr) {
    const auto *Amt = dyn_cast<ConstantInt>(BO->getOperand(1));
    if (!Amt || Amt->getZExtValue() >= 32)
      return false;
    const uint64_t S = Amt->getZExtValue();
    uint64_t SubMin = 0, SubMax = kU32Max;
    if (ComputeDynamicRange(BO->getOperand(0), NonZero, SubMin, SubMax, Depth + 1)) {
      OutMin = SubMin >> S;
      OutMax = SubMax >> S;
      return true;
    }
    if (S > 0) {
      OutMin = 0;
      OutMax = kU32Max >> S;
      return true;
    }
    return false;
  }
  if (Op == Instruction::Shl) {
    const auto *Amt = dyn_cast<ConstantInt>(BO->getOperand(1));
    uint64_t SubMin = 0, SubMax = kU32Max;
    if (!Amt || Amt->getZExtValue() > 4 ||
        !ComputeDynamicRange(BO->getOperand(0), NonZero, SubMin, SubMax, Depth + 1))
      return false;
    const uint64_t S = Amt->getZExtValue();
    const bool NoWrap = BO->hasNoUnsignedWrap() || BO->hasNoSignedWrap() ||
                        (SubMax << S) <= kU32Max;
    if (!NoWrap)
      return false;
    OutMin = SubMin << S;
    OutMax = std::min(kU32Max, SubMax << S);
    return true;
  }
  const ConstantInt *C = dyn_cast<ConstantInt>(BO->getOperand(1));
  const Value *Other = BO->getOperand(0);
  if (!C) {
    C = dyn_cast<ConstantInt>(BO->getOperand(0));
    Other = BO->getOperand(1);
  }
  if (!C || C->getSExtValue() < 0)
    return false;
  const uint64_t CV = static_cast<uint64_t>(C->getSExtValue());
  if (Op == Instruction::Mul) {
    uint64_t SubMin = 0, SubMax = kU32Max;
    if (CV == 0 || CV > 64 ||
        !ComputeDynamicRange(Other, NonZero, SubMin, SubMax, Depth + 1))
      return false;
    const bool NoWrap = BO->hasNoUnsignedWrap() || BO->hasNoSignedWrap() ||
                        SubMax <= (kU32Max / CV);
    if (!NoWrap)
      return false;
    OutMin = SubMin * CV;
    OutMax = std::min(kU32Max, SubMax * CV);
    return true;
  }
  if (CV > 65536ULL)
    return false;
  if (Op == Instruction::Or) {
    // Bitwise OR equals addition iff Other and CV have no overlapping 1-bits.
    if ((ComputePossibleOneBits(Other, Depth + 1) & CV) != 0)
      return false;
    uint64_t SubMin = 0, SubMax = kU32Max;
    if (!ComputeDynamicRange(Other, NonZero, SubMin, SubMax, Depth + 1))
      return false;
    OutMin = SubMin + CV;
    OutMax = std::min(kU32Max, SubMax + CV);
    return true;
  }
  if (Op == Instruction::Add) {
    uint64_t SubMin = 0, SubMax = kU32Max;
    if (ComputeDynamicRange(Other, NonZero, SubMin, SubMax, Depth + 1)) {
      const bool NoWrap = BO->hasNoUnsignedWrap() || BO->hasNoSignedWrap() ||
                          (SubMax + CV <= kU32Max);
      if (!NoWrap)
        return false;
      OutMin = SubMin + CV;
      OutMax = std::min(kU32Max, SubMax + CV);
      return true;
    }
    if (BO->hasNoUnsignedWrap()) {
      OutMin = CV;
      OutMax = kU32Max;
      return true;
    }
  }
  return false;
}

bool ComputeDynamicRange(const Value *V,
                         const SmallPtrSetImpl<const Value *> &NonZero,
                         uint64_t &OutMin, uint64_t &OutMax, unsigned Depth) {
  if (Depth > 6 || !V)
    return false;
  auto ApplyNonZero = [&]() {
    if (OutMin == 0 && NonZero.count(V))
      OutMin = 1;
    if (OutMax < OutMin)
      OutMax = OutMin;
  };
  if (const auto *CI = dyn_cast<ConstantInt>(V)) {
    if (CI->getSExtValue() < 0)
      return false;
    OutMin = OutMax = static_cast<uint64_t>(CI->getSExtValue());
    return true;
  }
  if (const auto *ZE = dyn_cast<ZExtInst>(V)) {
    const unsigned Bits = ZE->getSrcTy()->getIntegerBitWidth();
    OutMin = 0;
    OutMax = Bits >= 32 ? kU32Max : ((1ULL << Bits) - 1ULL);
    ApplyNonZero();
    return true;
  }
  if (const auto *PN = dyn_cast<PHINode>(V)) {
    if (!ComputeMonotonicPhiRange(PN, NonZero, OutMin, OutMax, Depth))
      return false;
    ApplyNonZero();
    return true;
  }
  if (const auto *Sel = dyn_cast<SelectInst>(V)) {
    uint64_t TMin = 0, TMax = 0, FMin = 0, FMax = 0;
    if (!ComputeDynamicRange(Sel->getTrueValue(), NonZero, TMin, TMax, Depth + 1) ||
        !ComputeDynamicRange(Sel->getFalseValue(), NonZero, FMin, FMax, Depth + 1))
      return false;
    OutMin = std::min(TMin, FMin);
    OutMax = std::max(TMax, FMax);
    ApplyNonZero();
    return true;
  }
  if (const auto *BO = dyn_cast<BinaryOperator>(V)) {
    if (!ComputeBinOpRange(BO, NonZero, OutMin, OutMax, Depth))
      return false;
    ApplyNonZero();
    return true;
  }
  return false;
}

unsigned ComputeFieldElemStrideRows(Type *ElemTy,
                                    const DxilFieldAnnotation &Field,
                                    DxilTypeSystem &Types) {
  if (Field.HasMatrixAnnotation()) {
    const auto &Mat = Field.GetMatrixAnnotation();
    const bool RowMaj = Mat.Orientation == MatrixOrientation::RowMajor;
    const unsigned MajorCount = RowMaj ? Mat.Rows : Mat.Cols;
    unsigned OuterCount = 1;
    Type *Ty = ElemTy;
    while (auto *Arr = dyn_cast<ArrayType>(Ty)) {
      if (MajorCount > 1 && !isa<ArrayType>(Arr->getElementType()))
        break;
      OuterCount *= static_cast<unsigned>(Arr->getNumElements());
      Ty = Arr->getElementType();
    }
    if (OuterCount > 1)
      return MajorCount;
    return MajorCount > 1 ? 1u : 0u;
  }
  if (auto *Arr = dyn_cast<ArrayType>(ElemTy)) {
    if (Arr->getNumElements() <= 1)
      return 0;
    const unsigned ElemSz =
        ComputeCBufferFieldByteSize(Arr->getElementType(), nullptr, Types);
    return ElemSz ? (((ElemSz + 15u) & ~15u) / 16u) : 0u;
  }
  return 0;
}

} // namespace

bool dxcb5::spec::IsEmptyCBufferField(Type *Ty) {
  while (auto *Arr = dyn_cast<ArrayType>(Ty))
    Ty = Arr->getElementType();
  auto *ST = dyn_cast<StructType>(Ty);
  if (!ST)
    return false;
  for (unsigned I = 0; I < ST->getNumElements(); ++I)
    if (!IsEmptyCBufferField(ST->getElementType(I)))
      return false;
  return true;
}

unsigned dxcb5::spec::ComputeCBufferFieldByteSize(
    Type *Ty, const DxilFieldAnnotation *Field, const DxilTypeSystem &Types) {
  unsigned Elements = 1;
  if (Field && Field->HasMatrixAnnotation()) {
    const auto &Mat = Field->GetMatrixAnnotation();
    const bool RowMaj = Mat.Orientation == MatrixOrientation::RowMajor;
    const unsigned MajorCount = RowMaj ? Mat.Rows : Mat.Cols;
    const unsigned MinorWidth = RowMaj ? Mat.Cols : Mat.Rows;
    while (auto *Arr = dyn_cast<ArrayType>(Ty)) {
      if (MajorCount > 1 && !isa<ArrayType>(Arr->getElementType()))
        break;
      Elements *= static_cast<unsigned>(Arr->getNumElements());
      Ty = Arr->getElementType();
    }
    const unsigned ScalarSz = ScalarLegacyByteSize(Ty);
    if (!ScalarSz)
      return 0;
    const unsigned RowStride = ((MinorWidth * ScalarSz + 15u) & ~15u);
    const unsigned MatSz = (MajorCount - 1) * RowStride + MinorWidth * ScalarSz;
    return Elements > 1 ? ((MatSz + 15u) & ~15u) * (Elements - 1) + MatSz : MatSz;
  }
  while (auto *Arr = dyn_cast<ArrayType>(Ty)) {
    Elements *= static_cast<unsigned>(Arr->getNumElements());
    Ty = Arr->getElementType();
  }
  unsigned ElemSz = 0;
  if (auto *ST = dyn_cast<StructType>(Ty)) {
    if (const auto *Ann = Types.GetStructAnnotation(ST)) {
      unsigned MaxEnd = 0;
      for (unsigned I = 0; I < ST->getNumElements(); ++I) {
        if (IsEmptyCBufferField(ST->getElementType(I)))
          continue;
        const auto &F = Ann->GetFieldAnnotation(I);
        if (!F.HasCBufferOffset())
          continue;
        const unsigned SubSz =
            ComputeCBufferFieldByteSize(ST->getElementType(I), &F, Types);
        if (!SubSz)
          return 0;
        MaxEnd = std::max(MaxEnd, F.GetCBufferOffset() + SubSz);
      }
      ElemSz = MaxEnd;
    }
  } else {
    const unsigned ScalarSz = ScalarLegacyByteSize(Ty);
    if (!ScalarSz)
      return 0;
    unsigned Comps = 1;
    if (auto *VT = dyn_cast<VectorType>(Ty))
      Comps = VT->getNumElements();
    else if (Field && Field->GetVectorSize() > 1)
      Comps = Field->GetVectorSize();
    ElemSz = Comps * ScalarSz;
  }
  if (!ElemSz)
    return 0;
  return Elements > 1 ? ((ElemSz + 15u) & ~15u) * (Elements - 1) + ElemSz : ElemSz;
}

bool dxcb5::spec::ResolveDynamicCBufferField(const Value *RowVal,
                                             const BasicBlock *AtBB,
                                             const DxilCBuffer &CB,
                                             DxilTypeSystem &Types,
                                             unsigned &OutFieldIdx) {
  SmallPtrSet<const Value *, 8> NonZero;
  CollectDominatingNonZero(AtBB, NonZero);
  uint64_t MinRow = 0, MaxRow = kU32Max;
  if (!ComputeDynamicRange(RowVal, NonZero, MinRow, MaxRow) || MinRow > 65536ULL)
    return false;
  const unsigned BaseConst = static_cast<unsigned>(MinRow);
  auto *ST = dyn_cast<StructType>(CB.GetHLSLType()->getPointerElementType());
  const auto *Ann = ST ? Types.GetStructAnnotation(ST) : nullptr;
  if (!ST || !Ann)
    return false;
  for (unsigned I = 0; I < ST->getNumElements(); ++I) {
    Type *ElemTy = ST->getElementType(I);
    if (IsEmptyCBufferField(ElemTy))
      continue;
    const auto &F = Ann->GetFieldAnnotation(I);
    if (!F.HasCBufferOffset() || (F.GetCBufferOffset() % 16) != 0)
      continue;
    const unsigned Sz = ComputeCBufferFieldByteSize(ElemTy, &F, Types);
    if (Sz == 0)
      continue;
    const unsigned StartRow = F.GetCBufferOffset() / 16;
    const unsigned EndRow = (F.GetCBufferOffset() + Sz - 1) / 16;
    if (BaseConst < StartRow || BaseConst > EndRow)
      continue;
    const unsigned StrideRows = ComputeFieldElemStrideRows(ElemTy, F, Types);
    if (StrideRows == 0 || (BaseConst - StartRow) >= StrideRows)
      return false;
    OutFieldIdx = I;
    return true;
  }
  return false;
}

bool dxcb5::spec::CanRewriteGlobalsLoad(
    CallInst *CI, const DenseMap<unsigned, unsigned> &DwordRemap) {
  const unsigned OldRow = static_cast<unsigned>(
      cast<ConstantInt>(CI->getArgOperand(2))->getZExtValue());
  for (User *U : CI->users()) {
    const auto *EV = dyn_cast<ExtractValueInst>(U);
    if (!EV || EV->getNumIndices() != 1 || EV->getIndices()[0] >= 4 ||
        !DwordRemap.count(OldRow * 4 + EV->getIndices()[0]))
      return false;
  }
  return true;
}

void dxcb5::spec::RewriteOneGlobalsLoad(
    CallInst *CI, const DenseMap<unsigned, unsigned> &DwordRemap) {
  const unsigned OldRow = static_cast<unsigned>(
      cast<ConstantInt>(CI->getArgOperand(2))->getZExtValue());
  SmallVector<ExtractValueInst *, 4> Extracts;
  for (User *U : CI->users())
    Extracts.push_back(cast<ExtractValueInst>(U));
  Type *I32Ty = Type::getInt32Ty(CI->getContext());
  DenseMap<unsigned, CallInst *> NewLoads;
  for (ExtractValueInst *EV : Extracts) {
    const unsigned NewDword = DwordRemap.lookup(OldRow * 4 + EV->getIndices()[0]);
    const unsigned NewRow = NewDword / 4;
    const unsigned NewCol = NewDword % 4;
    CallInst *&NewCI = NewLoads[NewRow];
    if (!NewCI) {
      Value *Args[] = {CI->getArgOperand(0), CI->getArgOperand(1),
                       ConstantInt::get(I32Ty, NewRow)};
      NewCI = CallInst::Create(CI->getCalledValue(), Args, "", CI);
    }
    auto *NewEV = ExtractValueInst::Create(NewCI, {NewCol}, "", EV);
    EV->replaceAllUsesWith(NewEV);
    EV->eraseFromParent();
  }
  if (CI->use_empty())
    CI->eraseFromParent();
}

void dxcb5::spec::RewriteDynamicGlobalsLoad(CallInst *CI, int RowDelta) {
  if (RowDelta == 0)
    return;
  Value *OldRow = CI->getArgOperand(2);
  auto *BO = dyn_cast<BinaryOperator>(OldRow);
  if (BO && BO->getOpcode() == Instruction::Add) {
    ConstantInt *C = dyn_cast<ConstantInt>(BO->getOperand(1));
    Value *Base = BO->getOperand(0);
    if (!C) {
      C = dyn_cast<ConstantInt>(BO->getOperand(0));
      Base = BO->getOperand(1);
    }
    if (C) {
      const int64_t NewOff = C->getSExtValue() + RowDelta;
      CI->setArgOperand(
          2, NewOff == 0 ? Base
                         : BinaryOperator::CreateAdd(
                               Base, ConstantInt::get(OldRow->getType(),
                                                      static_cast<uint64_t>(NewOff)),
                               "", BO));
      if (BO->use_empty())
        BO->eraseFromParent();
      return;
    }
  }
  CI->setArgOperand(2, BinaryOperator::CreateAdd(
                           OldRow, ConstantInt::get(OldRow->getType(),
                                                    static_cast<uint64_t>(RowDelta)),
                           "", CI));
}
