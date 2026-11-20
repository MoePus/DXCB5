// Distributed under the University of Illinois Open Source License.
#include "Frontend/PackLocalConstants.h"
#include <d3d11.h>
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <array>

using namespace dxcb5;
using namespace llvm;

namespace {
using IRInstruction = llvm::Instruction;
constexpr unsigned RegisterLanes = 4;

struct Access {
  const LoadInst *Load;
  const GetElementPtrInst *Pointer;
  const BinaryOperator *Product;
  const BinaryOperator *Offset;
  const Value *Index;
  unsigned Lane;
  unsigned Width;
};

struct Tuple {
  std::array<const LoadInst *, RegisterLanes> Loads{};
  const LoadInst *First = nullptr;
  const Value *Index = nullptr;
};

bool ConstantOperand(const BinaryOperator &Bin, unsigned Opcode,
                     const Value *&Other, uint64_t &Number) {
  if (Bin.getOpcode() != Opcode || !Bin.getType()->isIntegerTy(32))
    return false;
  for (unsigned I = 0; I != 2; ++I) {
    if (const auto *Const = dyn_cast<ConstantInt>(Bin.getOperand(I))) {
      Other = Bin.getOperand(1 - I);
      Number = Const->getZExtValue();
      return true;
    }
  }
  return false;
}

bool MatchAccess(const GlobalVariable &Global, const User &U,
                 const Function &Entry, Access &Result) {
  const auto *Pointer = dyn_cast<GetElementPtrInst>(&U);
  if (!Pointer || Pointer->getPointerOperand() != &Global ||
      Pointer->getNumIndices() != 2 || !Pointer->hasOneUse())
    return false;
  const auto *Outer = dyn_cast<ConstantInt>(Pointer->getOperand(1));
  const auto *Load = dyn_cast<LoadInst>(*Pointer->user_begin());
  if (!Outer || !Outer->isZero() || !Load || !Load->isSimple() ||
      Load->getType() !=
          cast<ArrayType>(Global.getValueType())->getElementType() ||
      Load->getParent()->getParent() != &Entry)
    return false;

  const Value *Inner = Pointer->getOperand(2);
  uint64_t Lane = 0, Width = 0;
  const auto *Offset = dyn_cast<BinaryOperator>(Inner);
  if (!Offset || !ConstantOperand(*Offset, IRInstruction::Add, Inner, Lane))
    Offset = nullptr;
  const auto *Product = dyn_cast<BinaryOperator>(Inner);
  const Value *Index;
  if (!Product || !ConstantOperand(*Product, IRInstruction::Mul, Index, Width) ||
      Width < 2 || Width > RegisterLanes || Lane >= Width)
    return false;
  Result = {Load, Pointer, Product, Offset, Index,
            static_cast<unsigned>(Lane), static_cast<unsigned>(Width)};
  return true;
}

bool ConstantInRange(const Value *V, uint64_t Min, uint64_t Max) {
  const auto *C = dyn_cast<ConstantInt>(V);
  return C && C->getZExtValue() >= Min && C->getZExtValue() <= Max;
}

bool BoundedLimit(const Value *V, uint64_t Rows) {
  if (const auto *Select = dyn_cast<SelectInst>(V))
    return ConstantInRange(Select->getTrueValue(), 0, Rows) &&
           ConstantInRange(Select->getFalseValue(), 0, Rows);
  return ConstantInRange(V, 0, Rows);
}

bool BoundedBackedge(const PHINode &Phi, const BasicBlock &Latch,
                     uint64_t Rows) {
  const auto *Update =
      dyn_cast<BinaryOperator>(Phi.getIncomingValueForBlock(&Latch));
  const Value *Other;
  uint64_t Increment;
  if (!Update || Update->getParent() != &Latch ||
      !ConstantOperand(*Update, IRInstruction::Add, Other, Increment) ||
      Other != &Phi || Increment != 1)
    return false;

  const auto *Branch = dyn_cast<BranchInst>(Latch.getTerminator());
  if (!Branch || !Branch->isConditional())
    return false;
  const bool TrueCarries = Branch->getSuccessor(0) == Phi.getParent();
  if (TrueCarries == (Branch->getSuccessor(1) == Phi.getParent()))
    return false;
  const auto *Compare = dyn_cast<ICmpInst>(Branch->getCondition());
  if (!Compare)
    return false;
  auto Predicate = Compare->getPredicate();
  const Value *Limit;
  if (Compare->getOperand(0) == Update) {
    Limit = Compare->getOperand(1);
  } else if (Compare->getOperand(1) == Update) {
    Limit = Compare->getOperand(0);
    Predicate = ICmpInst::getSwappedPredicate(Predicate);
  } else {
    return false;
  }
  if (!TrueCarries)
    Predicate = ICmpInst::getInversePredicate(Predicate);
  if (Predicate == ICmpInst::ICMP_NE)
    return ConstantInRange(Limit, 1, Rows);
  return (Predicate == ICmpInst::ICMP_SLT || Predicate == ICmpInst::ICMP_ULT) &&
         BoundedLimit(Limit, Rows);
}

bool IsBoundedCounter(const Value *Index, const LoadInst &Load, uint64_t Rows,
                      const DominatorTree &Dominators) {
  const auto *Phi = dyn_cast<PHINode>(Index);
  if (!Phi || Phi->getParent() != Load.getParent() ||
      Phi->getNumIncomingValues() != 2)
    return false;
  unsigned Initial = ConstantInRange(Phi->getIncomingValue(0), 0, 0) ? 0 : 1;
  const auto *Entry = Phi->getIncomingBlock(Initial);
  const auto *Latch = Phi->getIncomingBlock(1 - Initial);
  return ConstantInRange(Phi->getIncomingValue(Initial), 0, 0) &&
         Dominators.isReachableFromEntry(Entry) && Entry != Latch &&
         !Dominators.dominates(Phi->getParent(), Entry) &&
         Dominators.dominates(Phi->getParent(), Latch) &&
         BoundedBackedge(*Phi, *Latch, Rows);
}

bool IsBoundedIndex(const Value *Index, const LoadInst &Load, uint64_t Rows,
                    const DominatorTree &Dominators) {
  const auto *Block = Load.getParent();
  if (Block != &Block->getParent()->getEntryBlock() &&
      !Dominators.isReachableFromEntry(Block))
    return false;
  if (IsBoundedCounter(Index, Load, Rows, Dominators))
    return true;
  const auto &Layout = Block->getParent()->getParent()->getDataLayout();
  APInt Zero(32, 0), One(32, 0);
  computeKnownBits(const_cast<Value *>(Index), Zero, One, Layout, 0, nullptr,
                   &Load, &Dominators);
  return (~Zero).getZExtValue() < Rows;
}

bool CollectAccesses(const GlobalVariable &Global, const Function &Entry,
                     SmallVectorImpl<Access> &Accesses, uint64_t &Rows) {
  const auto *Array = dyn_cast<ArrayType>(Global.getValueType());
  if (!Array || !Global.isConstant() || !Global.hasDefinitiveInitializer() ||
      (!Array->getElementType()->isFloatTy() &&
       !Array->getElementType()->isIntegerTy(32)))
    return false;
  for (const auto *Use : Global.users()) {
    Access A;
    if (!MatchAccess(Global, *Use, Entry, A) ||
        (!Accesses.empty() && A.Width != Accesses.front().Width))
      return false;
    Accesses.push_back(A);
  }
  if (Accesses.empty() || Array->getNumElements() % Accesses.front().Width)
    return false;
  Rows = Array->getNumElements() / Accesses.front().Width;
  return Rows && Rows <= D3D11_REQ_IMMEDIATE_CONSTANT_BUFFER_ELEMENT_COUNT;
}

bool CollectTuples(ArrayRef<Access> Accesses, uint64_t Rows,
                   const DominatorTree &Dominators,
                   SmallVectorImpl<Tuple> &Tuples) {
  DenseMap<std::pair<const BasicBlock *, const Value *>, size_t> Groups;
  for (const auto &A : Accesses) {
    const auto Key = std::make_pair(A.Load->getParent(), A.Index);
    const auto Insert = Groups.insert(std::make_pair(Key, Tuples.size()));
    if (Insert.second) {
      Tuples.emplace_back();
      Tuples.back().Index = A.Index;
    }
    auto &T = Tuples[Insert.first->second];
    if (T.Loads[A.Lane])
      return false;
    T.Loads[A.Lane] = A.Load;
    if (!T.First || Dominators.dominates(A.Load, T.First))
      T.First = A.Load;
  }
  const auto Width = Accesses.front().Width;
  for (const auto &T : Tuples) {
    if (std::any_of(T.Loads.begin(), T.Loads.begin() + Width,
                    [](const LoadInst *Load) { return !Load; }) ||
        !IsBoundedIndex(T.Index, *T.First, Rows, Dominators))
      return false;
  }
  return true;
}

bool CanOmit(const BinaryOperator *Inst, const LocalConstantPlan &Plan) {
  return Inst && std::all_of(Inst->user_begin(), Inst->user_end(),
                             [&](const User *U) {
                               const auto *Use = dyn_cast<IRInstruction>(U);
                               return Use && Plan.Omitted.count(Use);
                             });
}

} // namespace

void dxcb5::PlanLocalConstants(Function &Entry, DominatorTree &Dominators,
                               LocalConstantPlan &Plan) {
  if (Entry.getParent()->global_empty())
    return;
  TimeTraceScope Timer("DXCB5 Plan Local Constants", StringRef());
  SmallVector<Access, 16> Accepted;
  SmallPtrSet<const Value *, 16> Indices;
  for (const auto &Global : Entry.getParent()->globals()) {
    SmallVector<Access, 8> Accesses;
    uint64_t Rows;
    if (!CollectAccesses(Global, Entry, Accesses, Rows))
      continue;
    if (!Dominators.getRootNode())
      Dominators.recalculate(Entry);
    SmallVector<Tuple, 4> Tuples;
    if (!CollectTuples(Accesses, Rows, Dominators, Tuples))
      continue;
    const unsigned Width = Accesses.front().Width;
    for (const auto &T : Tuples) {
      LocalConstantTuple Info{&Global, T.Index, Width, T.Loads};
      Plan.Loads.insert(std::make_pair(T.First, Info));
      for (unsigned Lane = 0; Lane < Width; ++Lane) {
        if (T.Loads[Lane] != T.First)
          Plan.Omitted.insert(T.Loads[Lane]);
      }
    }
    for (const auto &A : Accesses)
      Plan.Omitted.insert(A.Pointer);
    Accepted.append(Accesses.begin(), Accesses.end());
    for (const auto &A : Accesses)
      Indices.insert(A.Index);
  }
  for (const auto &A : Accepted)
    if (!Indices.count(A.Offset) && CanOmit(A.Offset, Plan))
      Plan.Omitted.insert(A.Offset);
  for (const auto &A : Accepted)
    if (!Indices.count(A.Product) && CanOmit(A.Product, Plan))
      Plan.Omitted.insert(A.Product);
}
