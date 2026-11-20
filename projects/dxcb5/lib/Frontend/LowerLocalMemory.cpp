// Distributed under the University of Illinois Open Source License.
#include "Frontend/LowerToSsaMir.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

using namespace dxcb5;
using namespace llvm;

namespace {
constexpr unsigned kLocalArrayMaxElements = 4;

bool IsLocalArrayElementType(Type *Ty) {
  return Ty->isFloatTy() || Ty->isHalfTy() || Ty->isIntegerTy(32) ||
         Ty->isIntegerTy(16) || Ty->isIntegerTy(1);
}

bool IsSameSize32PtrCast(const Operator *BC) {
  if (!BC || BC->getOpcode() != llvm::Instruction::BitCast)
    return false;
  auto *SrcPtr = dyn_cast<PointerType>(BC->getOperand(0)->getType());
  auto *DstPtr = dyn_cast<PointerType>(BC->getType());
  if (!SrcPtr || !DstPtr)
    return false;
  Type *S = SrcPtr->getElementType();
  Type *D = DstPtr->getElementType();
  const bool S32 = S->isFloatTy() || S->isIntegerTy(32);
  const bool D32 = D->isFloatTy() || D->isIntegerTy(32);
  return S32 && D32;
}

const Value *Strip32BitCast(const Value *Ptr) {
  while (const auto *BC = dyn_cast<Operator>(Ptr)) {
    if (!IsSameSize32PtrCast(BC))
      break;
    Ptr = BC->getOperand(0);
  }
  return Ptr;
}

const Value *LocalArrayIndex(const GEPOperator &GEP) {
  if (GEP.getNumIndices() != 2)
    return nullptr;
  const auto *Outer = dyn_cast<ConstantInt>(GEP.getOperand(1));
  const auto *Index = GEP.getOperand(2);
  return Outer && Outer->isZero() && Index->getType()->isIntegerTy(32)
             ? Index
             : nullptr;
}

bool IsLocalArrayAccess(const User *Use, const Value *Ptr,
                        const Value *Index, bool AllowDynamicStore) {
  if (const auto *Load = dyn_cast<LoadInst>(Use))
    return Load->isSimple() && Load->getPointerOperand() == Ptr;
  if (const auto *Store = dyn_cast<StoreInst>(Use))
    return Store->isSimple() && Store->getPointerOperand() == Ptr &&
           (AllowDynamicStore || isa<ConstantInt>(Index));
  if (const auto *BC = dyn_cast<BitCastInst>(Use)) {
    if (!IsSameSize32PtrCast(cast<Operator>(BC)))
      return false;
    for (const auto *Sub : BC->users())
      if (!IsLocalArrayAccess(Sub, BC, Index, AllowDynamicStore))
        return false;
    return true;
  }
  return false;
}

bool ValidLocalArrayUses(const AllocaInst &Alloca,
                         bool AllowDynamicStore = false) {
  for (const auto *Use : Alloca.users()) {
    const auto *GEP = dyn_cast<GetElementPtrInst>(Use);
    const auto *Index = GEP ? LocalArrayIndex(*cast<GEPOperator>(GEP)) : nullptr;
    if (!Index || GEP->getPointerOperand() != &Alloca)
      return false;
    for (const auto *Access : GEP->users())
      if (!IsLocalArrayAccess(Access, GEP, Index, AllowDynamicStore))
        return false;
  }
  return true;
}

bool ValidScalarAllocaUses(const AllocaInst &Alloca) {
  for (const auto *U : Alloca.users()) {
    const auto *L = dyn_cast<LoadInst>(U);
    const auto *S = dyn_cast<StoreInst>(U);
    if (!(L && L->isSimple() && L->getPointerOperand() == &Alloca) &&
        !(S && S->isSimple() && S->getPointerOperand() == &Alloca))
      return false;
  }
  return true;
}

bool IsUsedInFunction(const GlobalVariable &GV, const Function &Fn) {
  SmallVector<const User *, 16> Work(GV.user_begin(), GV.user_end());
  SmallPtrSet<const User *, 16> Seen;
  while (!Work.empty()) {
    const User *U = Work.pop_back_val();
    if (!Seen.insert(U).second)
      continue;
    if (const auto *I = dyn_cast<llvm::Instruction>(U)) {
      if (I->getParent()->getParent() == &Fn)
        return true;
      continue;
    }
    if (isa<ConstantExpr>(U))
      Work.append(U->user_begin(), U->user_end());
  }
  return false;
}

struct ResolvedLocal {
  const AllocaInst *Alloca = nullptr;
  const GlobalVariable *Global = nullptr;
  const Value *Index = nullptr;
};

ResolvedLocal ResolveLocalPtr(const Value *Ptr) {
  Ptr = Strip32BitCast(Ptr);
  if (const auto *Direct = dyn_cast<AllocaInst>(Ptr))
    return {Direct, nullptr, nullptr};
  if (const auto *DirectGV = dyn_cast<GlobalVariable>(Ptr))
    return {nullptr, DirectGV, nullptr};
  const auto *GEP = dyn_cast<GEPOperator>(Ptr);
  if (!GEP)
    return {};
  const Value *Index = LocalArrayIndex(*GEP);
  if (!Index)
    return {};
  const Value *Base = Strip32BitCast(GEP->getPointerOperand());
  if (const auto *Alloca = dyn_cast<AllocaInst>(Base))
    return {Alloca, nullptr, Index};
  if (const auto *GV = dyn_cast<GlobalVariable>(Base))
    return {nullptr, GV, Index};
  return {};
}

} // namespace

bool SsaLowerer::InitMutableGlobals() {
  const Function &Entry = *Module.GetEntryFunction();
  for (const GlobalVariable &GV : Module.GetModule()->globals()) {
    if (GV.isConstant() || !GV.hasDefinitiveInitializer() ||
        GV.getType()->getPointerAddressSpace() != 0 ||
        !IsUsedInFunction(GV, Entry))
      continue;
    Type *ValTy = GV.getType()->getPointerElementType();
    const bool IsScalar = IsLocalArrayElementType(ValTy);
    const auto *Arr = dyn_cast<ArrayType>(ValTy);
    if (!IsScalar && (!Arr || !IsLocalArrayElementType(Arr->getElementType()) ||
                      Arr->getNumElements() == 0 ||
                      Arr->getNumElements() > 4096))
      continue;
    const uint32_t Count =
        IsScalar ? 1u : static_cast<uint32_t>(Arr->getNumElements());
    const uint32_t XId = static_cast<uint32_t>(Ssa.Mir.IndexableTemps.size());
    Ssa.Mir.IndexableTemps.push_back({XId, Count, 1u});
    IndexableGlobals[&GV] = XId;
    const Constant *Init = GV.getInitializer();
    for (uint32_t I = 0; I < Count; ++I) {
      const Constant *Elem = IsScalar ? Init : Init->getAggregateElement(I);
      const Operand Dst{OperandKind::IndexableTemp, XId, I, 1u, 0};
      Emit(D3D10_SB_OPCODE_MOV, Dst, {Source(Elem)});
    }
  }
  return Error.empty();
}

bool SsaLowerer::LowerAlloca(AllocaInst &Alloca) {
  const auto *Arr = dyn_cast<ArrayType>(Alloca.getAllocatedType());
  const bool IsScalar = IsLocalArrayElementType(Alloca.getAllocatedType());
  if (!Alloca.isStaticAlloca() || Alloca.isArrayAllocation() ||
      (!IsScalar && (!Arr || !IsLocalArrayElementType(Arr->getElementType()) ||
                     Arr->getNumElements() == 0)))
    return Fail("unsupported LLVM alloca: expected static 32-bit scalar or array");
  const uint64_t Count = IsScalar ? 1u : Arr->getNumElements();
  if (IsScalar ? ValidScalarAllocaUses(Alloca)
               : (Count <= kLocalArrayMaxElements && ValidLocalArrayUses(Alloca, false))) {
    LocalArrays[&Alloca] = static_cast<uint32_t>(Ssa.LocalArrayLengths.size());
    Ssa.LocalArrayLengths.push_back(static_cast<unsigned>(Count));
    return true;
  }
  if (IsScalar || Count > 4096 || !ValidLocalArrayUses(Alloca, true))
    return Fail("unsupported SM5 local array access or size exceeds limit");
  const uint32_t XId = static_cast<uint32_t>(Ssa.Mir.IndexableTemps.size());
  Ssa.Mir.IndexableTemps.push_back({XId, static_cast<uint32_t>(Count), 1u});
  IndexableArrays[&Alloca] = XId;
  return true;
}

bool SsaLowerer::LowerLoad(LoadInst &Load) {
  const ResolvedLocal Loc = ResolveLocalPtr(Load.getPointerOperand());
  uint32_t XId = UINT32_MAX;
  if (Loc.Alloca) {
    auto XIt = IndexableArrays.find(Loc.Alloca);
    if (XIt != IndexableArrays.end())
      XId = XIt->second;
  } else if (Loc.Global) {
    auto GIt = IndexableGlobals.find(Loc.Global);
    if (GIt != IndexableGlobals.end())
      XId = GIt->second;
  }
  const auto Result = Values.lookup(&Load);
  if (XId != UINT32_MAX) {
    const uint32_t Length = Ssa.Mir.IndexableTemps[XId].Elements;
    Operand Src{OperandKind::IndexableTemp, XId, 0, 1, 0};
    if (!Loc.Index) {
      Src.Element = 0;
    } else if (const auto *C = dyn_cast<ConstantInt>(Loc.Index)) {
      if (C->getZExtValue() >= Length)
        return Fail("SM5 indexable temp read index is out of bounds");
      Src.Element = static_cast<uint32_t>(C->getZExtValue());
    } else {
      const Operand DynIdx = RelativeIndex(Loc.Index);
      Src.RelativeRegister = DynIdx.Index;
      Src.RelativeLane = GetSwizzleLane(DynIdx.Swizzle, 0);
    }
    Emit(D3D10_SB_OPCODE_MOV, Result, {Src});
    return Error.empty();
  }
  if (!Loc.Alloca)
    return LowerLocalConstant(Load);
  const uint32_t ArrayId = LocalArrays.lookup(Loc.Alloca);
  const unsigned Length = Ssa.LocalArrayLengths[ArrayId];
  SsaInst Read;
  Read.Opcode = PseudoArrayRead;
  Read.Dest = Result;
  Read.ArrayId = ArrayId;
  if (!Loc.Index) {
    Read.ArrayLane = 0;
  } else if (const auto *C = dyn_cast<ConstantInt>(Loc.Index)) {
    if (C->getZExtValue() >= Length)
      return Fail("SM5 local array read index is out of bounds");
    Read.ArrayLane = static_cast<uint8_t>(C->getZExtValue());
  } else {
    Read.ArrayLane = 0xffu;
    Read.Sources.push_back(Source(Loc.Index));
  }
  CurrentBlock->Insts.push_back(std::move(Read));
  return Error.empty();
}

bool SsaLowerer::LowerStore(StoreInst &Store) {
  const ResolvedLocal Loc = ResolveLocalPtr(Store.getPointerOperand());
  uint32_t XId = UINT32_MAX;
  if (Loc.Alloca) {
    auto XIt = IndexableArrays.find(Loc.Alloca);
    if (XIt != IndexableArrays.end())
      XId = XIt->second;
  } else if (Loc.Global) {
    auto GIt = IndexableGlobals.find(Loc.Global);
    if (GIt != IndexableGlobals.end())
      XId = GIt->second;
  }
  if (XId != UINT32_MAX) {
    const uint32_t Length = Ssa.Mir.IndexableTemps[XId].Elements;
    Operand Dst{OperandKind::IndexableTemp, XId, 0, 1, 0};
    if (!Loc.Index) {
      Dst.Element = 0;
    } else if (const auto *C = dyn_cast<ConstantInt>(Loc.Index)) {
      if (C->getZExtValue() >= Length)
        return Fail("SM5 indexable temp write index is out of bounds");
      Dst.Element = static_cast<uint32_t>(C->getZExtValue());
    } else {
      const Operand DynIdx = RelativeIndex(Loc.Index);
      Dst.RelativeRegister = DynIdx.Index;
      Dst.RelativeLane = GetSwizzleLane(DynIdx.Swizzle, 0);
    }
    Operand Val = Source(Store.getValueOperand());
    if (Val.RelativeRegister != UINT32_MAX ||
        Val.Kind == OperandKind::IndexableTemp || Val.Modifier != 0) {
      const Operand Tmp = AllocTemp(1);
      Emit(D3D10_SB_OPCODE_MOV, Tmp, {Val});
      Val = Tmp;
    }
    Emit(D3D10_SB_OPCODE_MOV, Dst, {Val});
    return Error.empty();
  }
  if (!Loc.Alloca)
    return Fail("unsupported LLVM store: SM5 requires a private local array");
  const uint32_t ArrayId = LocalArrays.lookup(Loc.Alloca);
  const uint64_t Lane = Loc.Index ? cast<ConstantInt>(Loc.Index)->getZExtValue() : 0u;
  if (Lane >= Ssa.LocalArrayLengths[ArrayId])
    return Fail("SM5 local array write index is out of bounds");
  SsaInst Write;
  Write.Opcode = PseudoArrayWrite;
  Write.ArrayId = ArrayId;
  Write.ArrayLane = static_cast<uint8_t>(Lane);
  Write.Sources.push_back(Source(Store.getValueOperand()));
  CurrentBlock->Insts.push_back(std::move(Write));
  return Error.empty();
}
