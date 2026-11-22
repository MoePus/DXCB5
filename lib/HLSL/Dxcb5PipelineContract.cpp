// Distributed under the University of Illinois Open Source License.
// Stage-1 DXCB5 pipeline contract pass: propagates branch-preservation and
// uniformity metadata from specialization/uniform globals onto conditional
// terminators before SimplifyCFG runs.
#include "dxc/HLSL/Dxcb5Contract.h"
#include "dxc/DXIL/DxilMetadataHelper.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace {

class Dxcb5PipelineContractPass : public ModulePass {
public:
  static char ID;
  Dxcb5PipelineContractPass() : ModulePass(ID) {}

  static bool PropagateGlobalBranchTag(Module &M, const char *GlobalMDName,
                                       bool AddBranchHint,
                                       bool RequireAllOperandsTainted,
                                       const char *TagMDName) {
    NamedMDNode *NMD = M.getNamedMetadata(GlobalMDName);
    if (!NMD || NMD->getNumOperands() == 0)
      return false;
    SmallPtrSet<const Value *, 64> Tainted;
    SmallVector<const Value *, 64> Work;
    for (unsigned I = 0; I < NMD->getNumOperands(); ++I) {
      MDNode *Node = NMD->getOperand(I);
      if (!Node || Node->getNumOperands() < 1)
        continue;
      if (auto *NameMD = dyn_cast<MDString>(Node->getOperand(0))) {
        if (GlobalVariable *GV = M.getNamedGlobal(NameMD->getString())) {
          if (Tainted.insert(GV).second)
            Work.push_back(GV);
        }
      }
    }
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      for (BasicBlock &BB : F) {
        for (Instruction &Inst : BB) {
          if (Inst.getMetadata(GlobalMDName)) {
            if (Tainted.insert(&Inst).second)
              Work.push_back(&Inst);
          }
        }
      }
    }
    if (Work.empty())
      return false;
    bool Changed = false;
    LLVMContext &Ctx = M.getContext();
    std::vector<hlsl::DXIL::ControlFlowHint> Hints = {
        hlsl::DXIL::ControlFlowHint::Branch};
    MDNode *BranchHint = hlsl::DxilMDHelper::EmitControlFlowHints(Ctx, Hints);
    MDNode *TagNode = MDNode::get(Ctx, None);
    auto AllOperandsTaintedOrConst = [&](const User *U) -> bool {
      for (const Value *Op : U->operands()) {
        if (!isa<Constant>(Op) && !Tainted.count(Op))
          return false;
      }
      return true;
    };
    auto IsUniformPhi = [&](const PHINode *PN) -> bool {
      if (PN->getNumIncomingValues() == 0)
        return false;
      for (unsigned I = 0, E = PN->getNumIncomingValues(); I < E; ++I) {
        const Value *InVal = PN->getIncomingValue(I);
        if (!isa<Constant>(InVal) && !Tainted.count(InVal))
          return false;
        const BasicBlock *Pred = PN->getIncomingBlock(I);
        const TerminatorInst *PTerm = Pred->getTerminator();
        bool UniformEdge = (PTerm && PTerm->getMetadata(TagMDName) != nullptr);
        if (!UniformEdge) {
          if (const BasicBlock *GrandPred = Pred->getSinglePredecessor()) {
            const TerminatorInst *GTerm = GrandPred->getTerminator();
            UniformEdge = (GTerm && GTerm->getMetadata(TagMDName) != nullptr);
          }
        }
        if (!UniformEdge)
          return false;
      }
      return true;
    };
    for (;;) {
      while (!Work.empty()) {
        const Value *V = Work.pop_back_val();
        for (const User *U : V->users()) {
          if (const auto *BI = dyn_cast<BranchInst>(U)) {
            if (BI->isConditional() && BI->getCondition() == V) {
              if (AddBranchHint &&
                  !BI->getMetadata(hlsl::DxilMDHelper::kDxilControlFlowHintMDName)) {
                const_cast<BranchInst *>(BI)->setMetadata(
                    hlsl::DxilMDHelper::kDxilControlFlowHintMDName, BranchHint);
                Changed = true;
              }
              if (!BI->getMetadata(TagMDName)) {
                const_cast<BranchInst *>(BI)->setMetadata(TagMDName, TagNode);
                Changed = true;
              }
            }
            continue;
          }
          if (const auto *SI = dyn_cast<SwitchInst>(U)) {
            if (SI->getCondition() == V) {
              if (AddBranchHint &&
                  !SI->getMetadata(hlsl::DxilMDHelper::kDxilControlFlowHintMDName)) {
                const_cast<SwitchInst *>(SI)->setMetadata(
                    hlsl::DxilMDHelper::kDxilControlFlowHintMDName, BranchHint);
                Changed = true;
              }
              if (!SI->getMetadata(TagMDName)) {
                const_cast<SwitchInst *>(SI)->setMetadata(TagMDName, TagNode);
                Changed = true;
              }
            }
            continue;
          }
          if (isa<ConstantExpr>(U) || isa<LoadInst>(U) ||
              isa<GetElementPtrInst>(U) || isa<ICmpInst>(U) ||
              isa<FCmpInst>(U) || isa<BinaryOperator>(U) || isa<CastInst>(U) ||
              isa<ExtractElementInst>(U) || isa<ExtractValueInst>(U) ||
              isa<InsertElementInst>(U) || isa<ShuffleVectorInst>(U) ||
              isa<SelectInst>(U) ||
              (!RequireAllOperandsTainted && isa<PHINode>(U))) {
            if (RequireAllOperandsTainted && !isa<ConstantExpr>(U) &&
                !AllOperandsTaintedOrConst(U))
              continue;
            if (Tainted.insert(U).second)
              Work.push_back(U);
          }
        }
      }
      if (!RequireAllOperandsTainted)
        break;
      bool AddedPhi = false;
      for (Function &F : M) {
        if (F.isDeclaration())
          continue;
        for (BasicBlock &BB : F) {
          for (Instruction &Inst : BB) {
            auto *PN = dyn_cast<PHINode>(&Inst);
            if (!PN)
              break;
            if (!Tainted.count(PN) && IsUniformPhi(PN)) {
              Tainted.insert(PN);
              Work.push_back(PN);
              AddedPhi = true;
            }
          }
        }
      }
      if (!AddedPhi)
        break;
    }
    return Changed;
  }

  bool runOnModule(Module &M) override {
    bool Changed = PropagateGlobalBranchTag(
        M, dxcb5_contract::kSpecBranchGlobalsMD, /*AddBranchHint=*/true,
        /*RequireAllOperandsTainted=*/false, dxcb5_contract::kAutoBranchMD);
    Changed |= PropagateGlobalBranchTag(
        M, dxcb5_contract::kUniformGlobalsMD, /*AddBranchHint=*/false,
        /*RequireAllOperandsTainted=*/true, dxcb5_contract::kUniformMD);
    return Changed;
  }
};

char Dxcb5PipelineContractPass::ID = 0;

} // namespace

ModulePass *llvm::createDxcb5PipelineContractPass() {
  return new Dxcb5PipelineContractPass();
}
