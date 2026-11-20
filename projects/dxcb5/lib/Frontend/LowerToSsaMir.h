// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_FRONTEND_LOWER_TO_SSA_MIR_H
#define DXCB5_FRONTEND_LOWER_TO_SSA_MIR_H

#include "Frontend/Import.h"
#include "Frontend/PackLocalConstants.h"
#include "Mir/SsaMir.h"
#include "dxcb5/Compiler.h"
#include "dxc/DXIL/DxilModule.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <array>
#include <string>

namespace llvm {
class AllocaInst;
class BasicBlock;
class CallInst;
class Constant;
class GlobalVariable;
class Instruction;
class LoadInst;
class PHINode;
class StoreInst;
class Twine;
class Value;
} // namespace llvm

namespace dxcb5 {

// Lowers an LLVM/DXIL entry function (either vector-preserving or scalarized)
// into SSA Vector MIR.
bool LowerToSsaMir(hlsl::DxilModule &Module, const Interface &Layout,
                   SsaProgram &Ssa, std::string &Error,
                   llvm::ArrayRef<VectorPhiGroup> PhiGroups = {});

class SsaLowerer {
public:
  hlsl::DxilModule &Module;
  const Interface &Layout;
  SsaProgram &Ssa;
  std::string &Error;

  SsaBlock *CurrentBlock = nullptr;
  llvm::DenseMap<const llvm::BasicBlock *, uint32_t> BlockIDs;
  llvm::DenseMap<const llvm::Value *, Operand> Values;
  llvm::DenseMap<const llvm::AllocaInst *, uint32_t> LocalArrays;
  llvm::DenseMap<const llvm::AllocaInst *, uint32_t> IndexableArrays;
  llvm::DenseMap<const llvm::GlobalVariable *, uint32_t> IndexableGlobals;
  llvm::DenseMap<const llvm::GlobalVariable *, uint32_t> LocalConstants;
  llvm::DenseMap<const hlsl::DxilResourceBase *, std::vector<uint32_t>>
      UsedResourceSlots;
  LocalConstantPlan ConstantTuples;
  llvm::DenseSet<const llvm::Instruction *> Elided;
  llvm::DenseSet<const llvm::Instruction *> EmittedShuffles;

  struct SignPattern {
    const llvm::Value *GreaterFlag = nullptr;
    const llvm::Value *LessFlag = nullptr;
  };
  llvm::DenseMap<const llvm::Instruction *, SignPattern> SignPatterns;

  struct VectorPhiEntry {
    uint32_t ResultId = 0;
    unsigned Width = 0;
    std::array<const llvm::PHINode *, 4> Phis{};
  };
  llvm::DenseMap<const llvm::BasicBlock *, llvm::SmallVector<VectorPhiEntry, 4>>
      BlockVectorPhis;
  llvm::DenseSet<const llvm::PHINode *> GroupedPhis;

  SsaLowerer(hlsl::DxilModule &Module, const Interface &Layout, SsaProgram &Ssa,
             std::string &Error)
      : Module(Module), Layout(Layout), Ssa(Ssa), Error(Error) {}

  bool Run(llvm::ArrayRef<VectorPhiGroup> PhiGroups);

  bool Fail(const llvm::Twine &Reason);
  Operand AllocTemp(uint8_t Mask = 1);
  Operand Emit(uint32_t Opcode, Operand Dest,
               llvm::ArrayRef<Operand> Sources, bool Precise = false,
               bool Saturate = false);
  Operand Source(const llvm::Value *V);
  Operand SourceInt(const llvm::Value *V, bool Signed, bool Precise);
  Operand SourceScalarLane(const llvm::Value *V, unsigned Lane);
  Operand RelativeIndex(const llvm::Value *V);
  Operand CombineOperands(llvm::ArrayRef<Operand> Lanes, bool Precise = false);
  Operand VectorArgs(llvm::CallInst &Call, unsigned Start, unsigned Count);

  bool BindValue(llvm::Instruction &Inst);
  bool TryBindAlias(const llvm::Instruction &Inst, bool &Deferred);
  void PlanVectorPhiGroups(llvm::ArrayRef<VectorPhiGroup> PhiGroups);
  void MatchSignIdioms(llvm::BasicBlock &Block);

  bool LowerInstruction(llvm::Instruction &Inst);
  bool LowerSignedDivRem(llvm::Instruction &Inst, Operand Dest, bool Precise);
  bool LowerCall(llvm::CallInst &Call);
  bool LowerMath(llvm::CallInst &Call, unsigned OpNumber);
  bool LowerSample(llvm::CallInst &Call, unsigned OpNumber);
  bool LowerGather(llvm::CallInst &Call, unsigned OpNumber);
  bool LowerCalculateLod(llvm::CallInst &Call);
  bool LowerEvalAttribute(llvm::CallInst &Call, unsigned OpNumber);
  bool LowerBufferLoad(llvm::CallInst &Call, unsigned OpNumber);
  bool LowerUavStore(llvm::CallInst &Call, unsigned OpNumber);
  bool LowerAtomic(llvm::CallInst &Call, unsigned OpNumber);
  bool LowerBufferCounter(llvm::CallInst &Call);
  bool LowerTextureDimensions(llvm::CallInst &Call);
  bool LowerConstantBuffer(llvm::CallInst &Call);
  bool InitMutableGlobals();
  bool LowerAlloca(llvm::AllocaInst &Alloca);
  bool LowerLoad(llvm::LoadInst &Load);
  bool LowerStore(llvm::StoreInst &Store);
  bool LowerLocalConstant(llvm::LoadInst &Load);
  bool AddLocalConstant(const llvm::GlobalVariable &Global, uint32_t &Base,
                        unsigned Width = 1);
  bool Handle(llvm::Value *Value, Operand &Result,
              hlsl::DXIL::ResourceKind *Resource = nullptr,
              uint32_t *SampleCount = nullptr, uint32_t *Stride = nullptr);
  bool SignatureRegister(const llvm::CallInst &Call, bool Output,
                         Operand &Register, bool MarkUsed = false);
  void UseResourceSlot(const hlsl::DxilResourceBase &Binding, uint32_t Slot);
  bool LowerTerminator(llvm::BasicBlock &Block, SsaBlock &Out);
  bool LowerBlockPhis(llvm::BasicBlock &Block, SsaBlock &Out);
};

} // namespace dxcb5

#endif
