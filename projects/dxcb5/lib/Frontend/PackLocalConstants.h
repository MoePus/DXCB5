// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_PACK_LOCAL_CONSTANTS_H
#define DXCB5_PACK_LOCAL_CONSTANTS_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include <array>

namespace llvm {
class DominatorTree;
class Function;
class GlobalVariable;
class LoadInst;
class Instruction;
class Value;
}

namespace dxcb5 {

struct LocalConstantTuple {
  const llvm::GlobalVariable *Global;
  const llvm::Value *Index;
  unsigned Width;
  std::array<const llvm::LoadInst *, 4> LaneLoads{};
};

struct LocalConstantPlan {
  llvm::DenseMap<const llvm::LoadInst *, LocalConstantTuple> Loads;
  llvm::SmallPtrSet<const llvm::Instruction *, 16> Omitted;
};

void PlanLocalConstants(llvm::Function &Entry, llvm::DominatorTree &Dominators,
                        LocalConstantPlan &Plan);

} // namespace dxcb5

#endif
