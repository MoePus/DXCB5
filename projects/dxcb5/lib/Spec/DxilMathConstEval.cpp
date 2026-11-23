// Distributed under the University of Illinois Open Source License.
// All standard DXIL math ops (Sin/Cos/Sqrt/Frc/FMad/Fma/Dot/BitOps/Min/Max)
// are folded by upstream DXC's DxilConstantFolding during SCCP/InstCombine.
// This helper folds ONLY the 4 isSpecialFloat predicates not covered there.
#include "dxcb5/SpecEngine.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

using namespace hlsl;
using namespace llvm;

namespace {

Constant *TryEvalFloatPred(const CallInst &Call) {
  if (!OP::IsDxilOpFuncCallInst(&Call) || Call.getNumArgOperands() < 2)
    return nullptr;
  const auto *OpC = dyn_cast<ConstantInt>(Call.getArgOperand(0));
  const auto *CF = dyn_cast<ConstantFP>(Call.getArgOperand(1));
  if (!OpC || !CF || !Call.getType()->isIntegerTy(1))
    return nullptr;
  const APFloat &F = CF->getValueAPF();
  bool Res = false;
  switch (static_cast<DXIL::OpCode>(OpC->getZExtValue())) {
  case DXIL::OpCode::IsNaN:    Res = F.isNaN(); break;
  case DXIL::OpCode::IsInf:    Res = F.isInfinity(); break;
  case DXIL::OpCode::IsFinite: Res = !F.isNaN() && !F.isInfinity(); break;
  case DXIL::OpCode::IsNormal: Res = F.isNormal(); break;
  default: return nullptr;
  }
  return ConstantInt::get(Call.getType(), Res ? 1 : 0);
}

} // namespace

bool dxcb5::spec::FoldDxilMathIntrinsics(Module &M) {
  const DxilModule *DM = M.HasDxilModule() ? &M.GetDxilModule() : nullptr;
  bool Changed = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    SmallVector<CallInst *, 16> Dead;
    for (BasicBlock &BB : F) {
      for (llvm::Instruction &Inst : BB) {
        auto *CI = dyn_cast<CallInst>(&Inst);
        if (!CI || (DM && DM->IsPrecise(CI)))
          continue;
        if (Constant *C = TryEvalFloatPred(*CI)) {
          CI->replaceAllUsesWith(C);
          Dead.push_back(CI);
          Changed = true;
        }
      }
    }
    for (CallInst *CI : Dead)
      CI->eraseFromParent();
  }
  return Changed;
}
