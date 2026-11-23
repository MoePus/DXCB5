// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_SPEC_INTERNAL_H
#define DXCB5_SPEC_INTERNAL_H

#include "dxc/DXIL/DxilConstants.h"
#include "dxc/DXIL/DxilOperations.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include <cstdint>
#include <cstring>
#include <set>

namespace llvm {
class BasicBlock;
class Function;
class Module;
} // namespace llvm

namespace hlsl {
class DxilCBuffer;
class DxilFieldAnnotation;
class DxilModule;
class DxilTypeSystem;
} // namespace hlsl

namespace dxcb5 {
namespace spec {

struct SpecManifest;

struct HandleInfo {
  hlsl::DXIL::ResourceClass Class = hlsl::DXIL::ResourceClass::Invalid;
  unsigned RangeId = UINT32_MAX;
  bool Valid = false;
};

inline HandleInfo DecodeHandle(const llvm::Value *V) {
  const auto *CI = llvm::dyn_cast<llvm::CallInst>(V);
  if (!CI || !hlsl::OP::IsDxilOpFuncCallInst(CI, hlsl::DXIL::OpCode::CreateHandle))
    return {};
  const auto *Cls = llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(1));
  const auto *Id = llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(2));
  if (!Cls || !Id)
    return {};
  return {static_cast<hlsl::DXIL::ResourceClass>(Cls->getZExtValue()),
          static_cast<unsigned>(Id->getZExtValue()), true};
}

inline llvm::Constant *MakeTypedConstantFromBits(llvm::Type *Ty,
                                                 uint32_t Bits) {
  if (Ty->isFloatTy()) {
    float F = 0.0f;
    std::memcpy(&F, &Bits, sizeof(F));
    return llvm::ConstantFP::get(Ty, F);
  }
  if (Ty->isHalfTy()) {
    float F = 0.0f;
    std::memcpy(&F, &Bits, sizeof(F));
    llvm::APFloat Val(F);
    bool Loses = false;
    Val.convert(llvm::APFloat::IEEEhalf, llvm::APFloat::rmNearestTiesToEven,
                &Loses);
    return llvm::ConstantFP::get(Ty->getContext(), Val);
  }
  if (Ty->isIntegerTy(32))
    return llvm::ConstantInt::get(Ty, Bits);
  if (Ty->isIntegerTy(16))
    return llvm::ConstantInt::get(Ty, Bits & 0xffffu);
  if (Ty->isIntegerTy(1))
    return llvm::ConstantInt::get(Ty, Bits != 0 ? 1 : 0);
  return nullptr;
}

bool IsEmptyCBufferField(llvm::Type *Ty);

unsigned ComputeCBufferFieldByteSize(llvm::Type *Ty,
                                     const hlsl::DxilFieldAnnotation *Field,
                                     const hlsl::DxilTypeSystem &Types);

// Shared cbuffer dword liveness helpers (single source of truth across
// SpecDriver, SpecReflect, and SpecCBufferCompact).
inline void RecordConstCBufferLoadDwords(const llvm::CallInst *CI, unsigned Row,
                                         std::set<unsigned> &Dwords) {
  for (const llvm::User *U : CI->users()) {
    const auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(U);
    if (EV && EV->getNumIndices() == 1 && EV->getIndices()[0] < 4)
      Dwords.insert(Row * 4 + EV->getIndices()[0]);
    else
      for (unsigned C = 0; C < 4; ++C)
        Dwords.insert(Row * 4 + C);
  }
}

bool IsCBufferFieldUsedByDwords(llvm::Type *Ty,
                                const hlsl::DxilFieldAnnotation &Field,
                                const hlsl::DxilTypeSystem &Types,
                                const std::set<unsigned> &LiveDwords);

bool ResolveDynamicCBufferField(const llvm::Value *RowVal,
                                const llvm::BasicBlock *AtBB,
                                const hlsl::DxilCBuffer &CB,
                                hlsl::DxilTypeSystem &Types,
                                unsigned &OutFieldIdx);

bool CanRewriteGlobalsLoad(llvm::CallInst *CI,
                           const llvm::DenseMap<unsigned, unsigned> &DwordRemap);

void RewriteOneGlobalsLoad(llvm::CallInst *CI,
                           const llvm::DenseMap<unsigned, unsigned> &DwordRemap);

void RewriteDynamicGlobalsLoad(llvm::CallInst *CI, int RowDelta);

void CompactGlobalsCBuffer(hlsl::DxilModule &Dxil, llvm::Function &Fn);

void CompactSharedGlobalsCBuffer(llvm::Module &VsMod, llvm::Module &PsMod);

bool SubstituteConstTextures(hlsl::DxilModule &Dxil, llvm::Function &Fn,
                             const SpecManifest &Manifest);

} // namespace spec
} // namespace dxcb5

#endif // DXCB5_SPEC_INTERNAL_H
