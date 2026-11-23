// Distributed under the University of Illinois Open Source License.
#include "Spec/SpecInternal.h"
#include "Support/ResourceShape.h"
#include "dxcb5/SpecEngine.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilResource.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <array>
#include <cmath>
#include <cstring>
#include <map>

using namespace dxcb5::spec;
using namespace hlsl;
using namespace llvm;

namespace {

using Op = DXIL::OpCode;

struct SrvTarget {
  const SpecTextureValue *Val = nullptr;
  DXIL::ResourceKind Kind = DXIL::ResourceKind::Invalid;
  CompType ElementCompType;
};

Constant *MakeResRetScalar(Type *Ty, float Val, CompType CType) {
  if (Ty->isFloatTy())
    return ConstantFP::get(Ty, Val);
  if (Ty->isHalfTy()) {
    APFloat F(Val);
    bool Loses = false;
    F.convert(APFloat::IEEEhalf, APFloat::rmNearestTiesToEven, &Loses);
    return ConstantFP::get(Ty->getContext(), F);
  }
  if (Ty->isIntegerTy(32)) {
    const double D = static_cast<double>(Val);
    if (!std::isfinite(D))
      return nullptr;
    if (CType.IsSIntTy()) {
      if (D < -2147483648.0 || D > 2147483647.0)
        return nullptr;
      return ConstantInt::get(Ty, static_cast<uint32_t>(static_cast<int32_t>(D)));
    }
    if (D > 4294967295.0)
      return nullptr;
    return ConstantInt::get(Ty, D < 0.0 ? 0u : static_cast<uint32_t>(D));
  }
  if (Ty->isIntegerTy(16)) {
    const double D = static_cast<double>(Val);
    if (!std::isfinite(D))
      return nullptr;
    if (CType.IsSIntTy()) {
      if (D < -32768.0 || D > 32767.0)
        return nullptr;
      return ConstantInt::get(Ty, static_cast<uint16_t>(static_cast<int16_t>(D)));
    }
    if (D > 65535.0)
      return nullptr;
    return ConstantInt::get(Ty, D < 0.0 ? 0u : static_cast<uint16_t>(D));
  }
  if (Ty->isIntegerTy(1))
    return ConstantInt::get(Ty, Val != 0.0f ? 1 : 0);
  return nullptr;
}

Constant *MakeResRetConst(StructType *RetTy, const std::array<float, 4> &Col,
                          CompType CType) {
  if (!RetTy || RetTy->getNumElements() < 4)
    return nullptr;
  SmallVector<Constant *, 5> Elems;
  for (unsigned I = 0; I < 4; ++I) {
    Constant *C = MakeResRetScalar(RetTy->getElementType(I), Col[I], CType);
    if (!C)
      return nullptr;
    Elems.push_back(C);
  }
  for (unsigned I = 4; I < RetTy->getNumElements(); ++I)
    Elems.push_back(Constant::getNullValue(RetTy->getElementType(I)));
  return ConstantStruct::get(RetTy, Elems);
}

bool UsersOnlyExtractBelow(const CallInst *CI, unsigned MaxLane) {
  for (const User *U : CI->users()) {
    const auto *EV = dyn_cast<ExtractValueInst>(U);
    if (!EV || EV->getNumIndices() != 1 || EV->getIndices()[0] >= MaxLane)
      return false;
  }
  return true;
}

bool IsFoldableSampleRead(Op Opcode) {
  return Opcode == Op::Sample || Opcode == Op::SampleLevel ||
         Opcode == Op::SampleBias || Opcode == Op::SampleGrad;
}

Constant *TryFoldGetDimensions(const CallInst *CI, const SrvTarget &Tgt) {
  if (!Tgt.Val->HasDims || CI->getNumArgOperands() < 3)
    return nullptr;
  const unsigned KnownLanes = dxcb5::GetSm5ResourceShape(Tgt.Kind).KnownDimLanes;
  if (KnownLanes == 0 || Tgt.Val->DimCount < KnownLanes)
    return nullptr;
  // Only fold mip 0 / undef queries, and never fold when numLevels (lane 3)
  // or unknown array/sample lanes are extracted.
  const Value *MipArg = CI->getArgOperand(2);
  if (!isa<UndefValue>(MipArg)) {
    const auto *MipC = dyn_cast<ConstantInt>(MipArg);
    if (!MipC || MipC->getSExtValue() != 0)
      return nullptr;
  }
  if (!UsersOnlyExtractBelow(CI, KnownLanes))
    return nullptr;
  auto *RetST = dyn_cast<StructType>(CI->getType());
  if (!RetST || RetST->getNumElements() < 4)
    return nullptr;
  const uint32_t Vals[4] = {Tgt.Val->Width, Tgt.Val->Height, Tgt.Val->Depth, 1u};
  SmallVector<Constant *, 4> Elems;
  for (unsigned I = 0; I < RetST->getNumElements(); ++I)
    Elems.push_back(ConstantInt::get(RetST->getElementType(I), I < 4 ? Vals[I] : 0u));
  return ConstantStruct::get(RetST, Elems);
}

bool ExtractConstCoord(const Value *CoordV, const Value *OffV, int64_t &Out) {
  const auto *C = dyn_cast<ConstantInt>(CoordV);
  if (!C)
    return false;
  int64_t Off = 0;
  if (OffV && !isa<UndefValue>(OffV)) {
    const auto *OC = dyn_cast<ConstantInt>(OffV);
    if (!OC)
      return false;
    Off = OC->getSExtValue();
  }
  Out = C->getSExtValue() + Off;
  return true;
}

Constant *TryFoldTextureLoad(const CallInst *CI, const SrvTarget &Tgt) {
  if (!Tgt.Val->HasDims || CI->getNumArgOperands() < 9)
    return nullptr;
  if (!UsersOnlyExtractBelow(CI, 4))
    return nullptr;
  unsigned Dims = 0;
  if (Tgt.Kind == DXIL::ResourceKind::Texture1D) Dims = 1;
  else if (Tgt.Kind == DXIL::ResourceKind::Texture2D) Dims = 2;
  else if (Tgt.Kind == DXIL::ResourceKind::Texture3D) Dims = 3;
  else return nullptr;
  if (Tgt.Val->DimCount < Dims)
    return nullptr;

  const auto *MipC = dyn_cast<ConstantInt>(CI->getArgOperand(2));
  if (!MipC)
    return nullptr;
  const int64_t Mip = MipC->getSExtValue();
  auto *RetST = dyn_cast<StructType>(CI->getType());
  if (Mip < 0)
    return MakeResRetConst(RetST, {0.0f, 0.0f, 0.0f, 0.0f}, Tgt.ElementCompType);
  if (Mip > 0)
    return nullptr; // Unknown mip chain length/contents: keep hardware ld.

  const int64_t Limits[3] = {Tgt.Val->Width, Tgt.Val->Height, Tgt.Val->Depth};
  bool OutOfBounds = false;
  for (unsigned A = 0; A < Dims; ++A) {
    int64_t Pos = 0;
    if (!ExtractConstCoord(CI->getArgOperand(3 + A), CI->getArgOperand(6 + A), Pos))
      return nullptr; // Dynamic coordinate: keep hardware ld for exact OOB=0.
    if (Pos < 0 || Pos >= Limits[A])
      OutOfBounds = true;
  }
  static constexpr std::array<float, 4> kZero{0.0f, 0.0f, 0.0f, 0.0f};
  return MakeResRetConst(RetST, OutOfBounds ? kZero : Tgt.Val->Color,
                         Tgt.ElementCompType);
}

} // namespace

bool dxcb5::spec::SubstituteConstTextures(DxilModule &Dxil, Function &Fn,
                                          const SpecManifest &Manifest) {
  if (Manifest.ConstTextures.empty())
    return false;
  std::map<unsigned, SrvTarget> FoldSrv;
  for (const auto &Srv : Dxil.GetSRVs()) {
    auto It = Manifest.ConstTextures.find(Srv->GetGlobalName());
    if (It != Manifest.ConstTextures.end())
      FoldSrv[Srv->GetID()] = {&It->second, Srv->GetKind(), Srv->GetCompType()};
  }
  if (FoldSrv.empty())
    return false;

  bool Changed = false;
  SmallVector<CallInst *, 32> Dead;
  for (BasicBlock &BB : Fn) {
    for (llvm::Instruction &Inst : BB) {
      auto *CI = dyn_cast<CallInst>(&Inst);
      if (!CI || !OP::IsDxilOpFuncCallInst(CI) || CI->getNumArgOperands() < 2)
        continue;
      const auto *OpC = dyn_cast<ConstantInt>(CI->getArgOperand(0));
      if (!OpC)
        continue;
      const HandleInfo HI = DecodeHandle(CI->getArgOperand(1));
      if (!HI.Valid || HI.Class != DXIL::ResourceClass::SRV)
        continue;
      auto It = FoldSrv.find(HI.RangeId);
      if (It == FoldSrv.end())
        continue;
      const Op Opcode = static_cast<Op>(OpC->getZExtValue());
      Constant *Repl = nullptr;
      if (IsFoldableSampleRead(Opcode) && !It->second.Val->AddressBorder &&
          It->second.ElementCompType.IsFloatTy() && UsersOnlyExtractBelow(CI, 4))
        Repl = MakeResRetConst(dyn_cast<StructType>(CI->getType()),
                               It->second.Val->Color,
                               It->second.ElementCompType);
      else if (Opcode == Op::GetDimensions)
        Repl = TryFoldGetDimensions(CI, It->second);
      else if (Opcode == Op::TextureLoad)
        Repl = TryFoldTextureLoad(CI, It->second);
      if (Repl) {
        CI->replaceAllUsesWith(Repl);
        Dead.push_back(CI);
        Changed = true;
      }
    }
  }
  for (CallInst *CI : Dead)
    CI->eraseFromParent();
  return Changed;
}
