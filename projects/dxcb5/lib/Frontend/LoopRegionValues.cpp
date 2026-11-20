// Distributed under the University of Illinois Open Source License.
#include "dxcb5/LoopRegionValues.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Transforms/Utils/Local.h"
#include <array>
#include <unordered_map>

using namespace dxcb5;
using namespace llvm;

namespace {
using Op = hlsl::DXIL::OpCode;
using Number = unsigned;
constexpr unsigned MaxOperands = 9; // dx.op.dot4: opcode and eight scalars
using Operands = SmallVector<const Value *, MaxOperands>;

struct Recipe {
  const Type *Ty;
  const Value *Identity;
  unsigned Opcode;
  unsigned Detail = 0;
  unsigned Flags = 0;
  std::array<Number, MaxOperands> Inputs{};

  bool operator==(const Recipe &R) const {
    return Ty == R.Ty && Identity == R.Identity && Opcode == R.Opcode &&
           Detail == R.Detail && Flags == R.Flags && Inputs == R.Inputs;
  }
};

struct HashRecipe {
  size_t operator()(const Recipe &R) const {
    return hash_combine(R.Ty, R.Identity, R.Opcode, R.Detail, R.Flags,
                        hash_combine_range(R.Inputs.begin(), R.Inputs.end()));
  }
};

bool IsDxilCall(const CallInst &Call) {
  const auto *Callee = Call.getCalledFunction();
  return Callee && Callee->isDeclaration() && Call.getNumArgOperands() &&
         Call.getAttributes().isEmpty() && Call.getCallingConv() == CallingConv::C &&
         Call.doesNotThrow() && !Call.hasFnAttr(Attribute::Convergent) &&
         !Call.hasFnAttr(Attribute::ReturnsTwice) && hlsl::OP::IsDxilOpFunc(Callee);
}

bool IsConstantBufferHandle(const CallInst &Call) {
  if (!IsDxilCall(Call) || Call.getNumArgOperands() != 5 ||
      Call.getCalledFunction()->getName() != "dx.op.createHandle" ||
      hlsl::OP::getOpCode(&Call) != Op::CreateHandle)
    return false;
  auto *Class = dyn_cast<ConstantInt>(Call.getArgOperand(1));
  return Class && Class->equalsInt(static_cast<unsigned>(hlsl::DXIL::ResourceClass::CBuffer)) &&
         isa<ConstantInt>(Call.getArgOperand(2)) &&
         isa<ConstantInt>(Call.getArgOperand(3)) &&
         isa<ConstantInt>(Call.getArgOperand(4));
}

bool IsConstantBufferLoad(const CallInst &Call) {
  auto *Handle = dyn_cast<CallInst>(Call.getArgOperand(1));
  return Call.getNumArgOperands() == 3 && Handle &&
         IsConstantBufferHandle(*Handle) &&
         Call.getCalledFunction()->getName().startswith("dx.op.cbufferLoadLegacy.") &&
         isa<ConstantInt>(Call.getArgOperand(2));
}

bool StableCall(const CallInst &Call) {
  if (!IsDxilCall(Call))
    return false;
  switch (hlsl::OP::getOpCode(&Call)) {
  case Op::CreateHandle:
    return IsConstantBufferHandle(Call);
  case Op::CBufferLoadLegacy:
    return Call.getNumArgOperands() == 3 && IsConstantBufferLoad(Call);
  case Op::Dot2: case Op::Dot3: case Op::Dot4:
  case Op::IsNaN: case Op::IsFinite: case Op::FAbs:
  case Op::Sqrt: case Op::Rsqrt: case Op::FMad:
  case Op::FMin: case Op::FMax: case Op::Saturate:
    return Call.doesNotAccessMemory();
  default:
    return false;
  }
}

const BasicBlock *ArmPredecessor(const BasicBlock *Head, const BasicBlock *Arm,
                                const BasicBlock *Join) {
  if (Arm == Join)
    return Head;
  const auto *Jump = dyn_cast<BranchInst>(Arm->getTerminator());
  if (Jump && Jump->isUnconditional() && Jump->getSuccessor(0) == Join &&
      Arm->getSinglePredecessor() == Head)
    return Arm;
  return nullptr;
}

bool ReadGuardedPhi(const PHINode &Phi, const DominatorTree &DT,
                    Operands &Args) {
  if (Phi.getNumIncomingValues() != 2)
    return false;
  const auto *Join = Phi.getParent();
  for (const auto *Side : {Phi.getIncomingBlock(0), Phi.getIncomingBlock(1)}) {
    const auto *Head = Side->getSinglePredecessor();
    if (!Head || Head == Join || !DT.dominates(Head, Join))
      continue;
    const auto *Fork = dyn_cast<BranchInst>(Head->getTerminator());
    if (!Fork || !Fork->isConditional())
      continue;
    const auto *True = ArmPredecessor(Head, Fork->getSuccessor(0), Join);
    const auto *False = ArmPredecessor(Head, Fork->getSuccessor(1), Join);
    if (!True || !False || True == False || Phi.getBasicBlockIndex(True) < 0 ||
        Phi.getBasicBlockIndex(False) < 0)
      continue;
    Args.append({Fork->getCondition(), Phi.getIncomingValueForBlock(True),
                 Phi.getIncomingValueForBlock(False)});
    return true;
  }
  return false;
}

bool ReadOperands(const Instruction &I, const DominatorTree &DT,
                  Recipe &R, Operands &Args) {
  if (const auto *Phi = dyn_cast<PHINode>(&I))
    return ReadGuardedPhi(*Phi, DT, Args);
  if (const auto *Call = dyn_cast<CallInst>(&I)) {
    if (!StableCall(*Call))
      return false;
    R.Identity = Call->getCalledFunction();
    for (const Value *Arg : Call->arg_operands())
      Args.push_back(Arg);
    return true;
  }
  if (const auto *Extract = dyn_cast<ExtractValueInst>(&I)) {
    if (Extract->getNumIndices() != 1)
      return false;
    R.Detail = *Extract->idx_begin();
  } else if (const auto *Compare = dyn_cast<CmpInst>(&I)) {
    R.Detail = Compare->getPredicate();
  } else if (!isa<BinaryOperator>(I) && !isa<CastInst>(I) && !isa<SelectInst>(I)) {
    return false;
  }
  Args.append(I.op_begin(), I.op_end());
  return true;
}

class Numbering {
public:
  Numbering(const hlsl::DxilModule &Module, const DominatorTree &DT)
      : Module(Module), DT(DT) {}

  void Visit(Function &Entry) {
    ReversePostOrderTraversal<Function *> Order(&Entry);
    for (auto *Block : Order)
      for (auto &I : *Block)
        Read(I);
  }

  Number Get(const Instruction &I) const { return Candidates.lookup(&I); }

private:
  Number Intern(const Recipe &R) {
    return Table.emplace(R, static_cast<Number>(Table.size() + 1)).first->second;
  }

  Number ReadCandidate(const Instruction &I) {
    if (I.hasMetadataOtherThanDebugLoc())
      return 0;
    Recipe R;
    R.Ty = I.getType();
    R.Identity = nullptr;
    R.Opcode = I.getOpcode();
    R.Flags = (I.getRawSubclassOptionalData() << 1) |
              static_cast<unsigned>(Module.IsPrecise(&I));
    Operands Args;
    if (!ReadOperands(I, DT, R, Args) || Args.size() > R.Inputs.size())
      return 0;
    // Loop-carried values are opaque when their operands are not numbered in
    // reverse postorder. Distinct definitions retain distinct identities.
    for (unsigned K = 0; K != Args.size(); ++K) {
      const Value *V = Args[K];
      if (isa<UndefValue>(V))
        return 0;
      if (isa<Instruction>(V)) {
        R.Inputs[K] = Values.lookup(V);
        if (!R.Inputs[K])
          return 0;
      } else {
        Recipe Leaf;
        Leaf.Ty = V->getType();
        Leaf.Identity = V;
        Leaf.Opcode = 0;
        R.Inputs[K] = Intern(Leaf);
      }
    }
    return Intern(R);
  }

  void Read(const Instruction &I) {
    if (Number ID = ReadCandidate(I)) {
      Values[&I] = ID;
      Candidates[&I] = ID;
    } else {
      Recipe Self;
      Self.Ty = I.getType();
      Self.Identity = &I;
      Self.Opcode = 0;
      Values[&I] = Intern(Self);
    }
  }

  const hlsl::DxilModule &Module;
  const DominatorTree &DT;
  DenseMap<const Value *, Number> Values;
  DenseMap<const Instruction *, Number> Candidates;
  std::unordered_map<Recipe, Number, HashRecipe> Table;
};

using Alias = std::pair<Instruction *, Instruction *>;
struct Frame {
  const DomTreeNode *Node;
  size_t Child = 0;
  SmallVector<Number, 8> Introduced;
};

void RecordAliases(Frame &Current, const Numbering &Numbers,
                   DenseMap<Number, Instruction *> &Available,
                   SmallVectorImpl<Alias> &Aliases) {
  for (auto &I : *Current.Node->getBlock()) {
    if (I.use_empty())
      continue;
    const Number ID = Numbers.Get(I);
    if (!ID)
      continue;
    auto Prior = Available.try_emplace(ID, &I);
    if (Prior.second)
      Current.Introduced.push_back(ID);
    else
      Aliases.emplace_back(&I, Prior.first->second);
  }
}

SmallVector<Alias, 32> PlanAliases(const DominatorTree &DT, const Numbering &Numbers) {
  SmallVector<Alias, 32> Aliases;
  SmallVector<Frame, 16> Worklist;
  DenseMap<Number, Instruction *> Available;
  Worklist.push_back({DT.getRootNode()});
  while (!Worklist.empty()) {
    auto &Current = Worklist.back();
    if (!Current.Child)
      RecordAliases(Current, Numbers, Available, Aliases);
    const auto Child = Current.Child++;
    if (Child < Current.Node->getNumChildren()) {
      Worklist.push_back({*(Current.Node->begin() + Child)});
      continue;
    }
    for (Number ID : Current.Introduced)
      Available.erase(ID);
    Worklist.pop_back();
  }
  return Aliases;
}
bool HasCfgCycle(Function &Entry) {
  for (auto I = scc_begin(&Entry), E = scc_end(&Entry); I != E; ++I) {
    if (I.hasLoop())
      return true;
  }
  return false;
}
} // namespace

bool dxcb5::ReuseLoopRegionValues(Function &Entry, const hlsl::DxilModule &Module) {
  llvm::TimeTraceScope Scope("DXCB5 Reuse Loop Region Values", llvm::StringRef());
  if (Entry.empty() || !HasCfgCycle(Entry))
    return false;
  DominatorTree DT;
  DT.recalculate(Entry);

  Numbering Numbers(Module, DT);
  Numbers.Visit(Entry);
  const auto Aliases = PlanAliases(DT, Numbers);
  SmallVector<WeakVH, 32> Dead;
  for (const auto &A : Aliases) {
    A.first->replaceAllUsesWith(A.second);
    Dead.emplace_back(A.first);
  }
  for (const auto &I : Dead)
    if (I)
      RecursivelyDeleteTriviallyDeadInstructions(I);
#ifndef NDEBUG
  assert((Aliases.empty() || !verifyFunction(Entry, &errs())) &&
         "Loop region value reuse produced invalid LLVM IR");
#endif
  return !Aliases.empty();
}
