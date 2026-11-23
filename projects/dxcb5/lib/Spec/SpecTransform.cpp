// Distributed under the University of Illinois Open Source License.
#include "Spec/SpecInternal.h"
#include "dxcb5/SpecEngine.h"
#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilMetadataHelper.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilResource.h"
#include "dxc/DXIL/DxilSampler.h"
#include "dxc/DXIL/DxilTypeSystem.h"
#include "dxc/HLSL/Dxcb5Contract.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Scalar.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace dxcb5::spec;
using namespace hlsl;
using namespace llvm;

namespace {

using Op = DXIL::OpCode;

uint32_t EncodeScalarBits(ConstType Kind, double Val) {
  uint32_t Bits = 0;
  if (Kind == ConstType::F32) {
    const float F = static_cast<float>(Val);
    std::memcpy(&Bits, &F, sizeof(Bits));
  } else if (Kind == ConstType::I32) {
    const int32_t S =
        !std::isfinite(Val) ? 0
        : (Val <= -2147483648.0 ? INT32_MIN
           : (Val >= 2147483647.0 ? INT32_MAX : static_cast<int32_t>(Val)));
    std::memcpy(&Bits, &S, sizeof(Bits));
  } else if (Kind == ConstType::U32) {
    Bits = (!std::isfinite(Val) || Val <= 0.0) ? 0u
           : (Val >= 4294967295.0 ? UINT32_MAX : static_cast<uint32_t>(Val));
  } else {
    Bits = (Val != 0.0) ? 1u : 0u;
  }
  return Bits;
}

using SlotKey = std::pair<unsigned, unsigned>; // (cbRangeId, dwordOffset)

void PopulateMatrixSlots(Type *Ty, const DxilMatrixAnnotation &Mat,
                         unsigned RangeId, unsigned BaseDword,
                         const SpecConstValue &CV, size_t &ValIdx,
                         std::map<SlotKey, uint32_t> &SlotBits) {
  const bool RowMaj = Mat.Orientation == MatrixOrientation::RowMajor;
  const unsigned MajorCount = RowMaj ? Mat.Rows : Mat.Cols;
  const unsigned MinorWidth = RowMaj ? Mat.Cols : Mat.Rows;
  unsigned OuterCount = 1;
  while (auto *Arr = dyn_cast<ArrayType>(Ty)) {
    if (MajorCount > 1 && !isa<ArrayType>(Arr->getElementType()))
      break;
    OuterCount *= static_cast<unsigned>(Arr->getNumElements());
    Ty = Arr->getElementType();
  }
  const unsigned MatStrideDwords = MajorCount * 4;
  for (unsigned E = 0; E < OuterCount && ValIdx < CV.Values.size(); ++E) {
    const unsigned MatBase = BaseDword + E * MatStrideDwords;
    for (unsigned M = 0; M < MajorCount && ValIdx < CV.Values.size(); ++M)
      for (unsigned C = 0; C < MinorWidth && ValIdx < CV.Values.size(); ++C)
        SlotBits[SlotKey(RangeId, MatBase + M * 4 + C)] =
            EncodeScalarBits(CV.Type, CV.Values[ValIdx++]);
  }
}

void PopulateFieldSlots(Type *Ty, const DxilFieldAnnotation *Field,
                        DxilTypeSystem &Types, unsigned RangeId,
                        unsigned BaseDword, const SpecConstValue &CV,
                        size_t &ValIdx,
                        std::map<SlotKey, uint32_t> &SlotBits) {
  if (ValIdx >= CV.Values.size())
    return;
  if (Field && Field->HasMatrixAnnotation()) {
    PopulateMatrixSlots(Ty, Field->GetMatrixAnnotation(), RangeId, BaseDword,
                        CV, ValIdx, SlotBits);
    return;
  }
  if (auto *Arr = dyn_cast<ArrayType>(Ty)) {
    Type *ElemTy = Arr->getElementType();
    const unsigned ElemBytes = ComputeCBufferFieldByteSize(ElemTy, nullptr, Types);
    if (ElemBytes == 0)
      return; // Unsupported 64-bit / 16-bit element size in 32-bit cbuffer layout (M8).
    const unsigned StrideDwords = ((ElemBytes + 15) & ~15u) / 4;
    for (uint64_t E = 0; E < Arr->getNumElements() && ValIdx < CV.Values.size(); ++E) {
      PopulateFieldSlots(ElemTy, nullptr, Types, RangeId,
                         BaseDword + static_cast<unsigned>(E) * StrideDwords,
                         CV, ValIdx, SlotBits);
    }
    return;
  }
  if (auto *ST = dyn_cast<StructType>(Ty)) {
    const auto *Ann = Types.GetStructAnnotation(ST);
    if (!Ann)
      return;
    for (unsigned I = 0; I < ST->getNumElements() && ValIdx < CV.Values.size(); ++I) {
      const auto &F = Ann->GetFieldAnnotation(I);
      if (!F.HasCBufferOffset() || (F.GetCBufferOffset() % 4) != 0)
        return;
      PopulateFieldSlots(ST->getElementType(I), &F, Types, RangeId,
                         BaseDword + F.GetCBufferOffset() / 4, CV, ValIdx,
                         SlotBits);
    }
    return;
  }
  if (ComputeCBufferFieldByteSize(Ty, Field, Types) == 0)
    return;
  unsigned Width = 1;
  if (auto *VT = dyn_cast<VectorType>(Ty))
    Width = VT->getNumElements();
  else if (Field && Field->GetVectorSize() > 1)
    Width = Field->GetVectorSize();
  for (unsigned C = 0; C < Width && ValIdx < CV.Values.size(); ++C)
    SlotBits[{RangeId, BaseDword + C}] =
        EncodeScalarBits(CV.Type, CV.Values[ValIdx++]);
}

void AddCBufferFieldSlots(const DxilStructAnnotation &Ann,
                          const StructType *ST, unsigned RangeId,
                          const std::string &CbName,
                          const std::map<std::string, unsigned> &FieldCounts,
                          const SpecManifest &Manifest, bool ApplyDefines,
                          DxilTypeSystem &Types,
                          std::map<SlotKey, uint32_t> &SlotBits) {
  const bool IsFlagsCb = (CbName == "DXCB5_SpecFlags");
  for (unsigned I = 0; I < ST->getNumElements(); ++I) {
    const auto &Field = Ann.GetFieldAnnotation(I);
    const std::string Name = Field.GetFieldName();
    const unsigned BaseDword = Field.GetCBufferOffset() / 4;
    if (IsFlagsCb) {
      if (!ApplyDefines)
        continue;
      auto It = Manifest.Defines.find(Name);
      SlotBits[{RangeId, BaseDword}] =
          (It != Manifest.Defines.end()) ? It->second : 0u;
      continue;
    }
    auto It = Manifest.Constants.find(CbName + "." + Name);
    if (It == Manifest.Constants.end()) {
      auto CIt = FieldCounts.find(Name);
      if (CIt == FieldCounts.end() || CIt->second == 1u)
        It = Manifest.Constants.find(Name);
    }
    if (It == Manifest.Constants.end())
      continue;
    size_t ValIdx = 0;
    PopulateFieldSlots(ST->getElementType(I), &Field, Types, RangeId, BaseDword,
                       It->second, ValIdx, SlotBits);
  }
}

void BuildConstantSlotMap(DxilModule &Dxil, const SpecManifest &Manifest,
                          bool ApplyDefines,
                          std::map<SlotKey, uint32_t> &SlotBits) {
  auto &Types = Dxil.GetTypeSystem();
  std::map<std::string, unsigned> FieldCounts;
  for (const auto &CB : Dxil.GetCBuffers()) {
    if (CB->GetGlobalName() == "DXCB5_SpecFlags")
      continue;
    auto *ST = dyn_cast<StructType>(CB->GetHLSLType()->getPointerElementType());
    const auto *Ann = ST ? Types.GetStructAnnotation(ST) : nullptr;
    if (!Ann)
      continue;
    for (unsigned I = 0; I < ST->getNumElements(); ++I)
      ++FieldCounts[Ann->GetFieldAnnotation(I).GetFieldName()];
  }
  for (const auto &CB : Dxil.GetCBuffers()) {
    auto *ST = dyn_cast<StructType>(CB->GetHLSLType()->getPointerElementType());
    const auto *Ann = ST ? Types.GetStructAnnotation(ST) : nullptr;
    if (Ann)
      AddCBufferFieldSlots(*Ann, ST, CB->GetID(), CB->GetGlobalName(),
                           FieldCounts, Manifest, ApplyDefines, Types, SlotBits);
  }
}

bool SubstituteCBufferLoadInst(CallInst *CI,
                               const std::map<SlotKey, uint32_t> &SlotBits,
                               SmallVectorImpl<Instruction *> &DeadExtracts) {
  const HandleInfo HI = DecodeHandle(CI->getArgOperand(1));
  if (!HI.Valid || HI.Class != DXIL::ResourceClass::CBuffer)
    return false;
  const auto *RowC = dyn_cast<ConstantInt>(CI->getArgOperand(2));
  if (!RowC)
    return false;
  const unsigned Row = static_cast<unsigned>(RowC->getZExtValue());
  bool Changed = false;
  for (User *U : CI->users()) {
    auto *EV = dyn_cast<ExtractValueInst>(U);
    if (!EV || EV->getNumIndices() != 1 || EV->getIndices()[0] >= 4)
      continue;
    auto It = SlotBits.find({HI.RangeId, Row * 4 + EV->getIndices()[0]});
    if (It == SlotBits.end())
      continue;
    if (Constant *C = MakeTypedConstantFromBits(EV->getType(), It->second)) {
      EV->replaceAllUsesWith(C);
      DeadExtracts.push_back(EV);
      Changed = true;
    }
  }
  return Changed;
}

bool SubstituteCBufferLoads(Function &Fn,
                            const std::map<SlotKey, uint32_t> &SlotBits) {
  if (SlotBits.empty())
    return false;
  bool Changed = false;
  SmallVector<Instruction *, 32> DeadExtracts;
  for (BasicBlock &BB : Fn)
    for (Instruction &Inst : BB)
      if (auto *CI = dyn_cast<CallInst>(&Inst))
        if (OP::IsDxilOpFuncCallInst(CI, Op::CBufferLoadLegacy))
          Changed |= SubstituteCBufferLoadInst(CI, SlotBits, DeadExtracts);
  for (Instruction *I : DeadExtracts)
    I->eraseFromParent();
  return Changed;
}

bool IsDeadRemovableTextureOp(Op Opcode) {
  return Opcode == Op::Sample || Opcode == Op::SampleLevel ||
         Opcode == Op::SampleBias || Opcode == Op::SampleGrad ||
         Opcode == Op::TextureLoad;
}

void StripAutoHintsOnConstantBranches(Function &Fn) {
  for (BasicBlock &BB : Fn) {
    TerminatorInst *TI = BB.getTerminator();
    if (!TI)
      continue;
    const bool HasAuto =
        TI->getMetadata(llvm::dxcb5_contract::kAutoBranchMD) != nullptr;
    const bool HasStrict =
        TI->getMetadata(llvm::dxcb5_contract::kStrictBranchMD) != nullptr;
    if (!HasAuto && !HasStrict)
      continue;
    Value *Cond = nullptr;
    if (auto *BI = dyn_cast<BranchInst>(TI)) {
      if (BI->isConditional())
        Cond = BI->getCondition();
    } else if (auto *SI = dyn_cast<SwitchInst>(TI)) {
      Cond = SI->getCondition();
    }
    if (Cond && isa<ConstantInt>(Cond)) {
      TI->setMetadata(DxilMDHelper::kDxilControlFlowHintMDName, nullptr);
      if (HasAuto)
        TI->setMetadata(llvm::dxcb5_contract::kAutoBranchMD, nullptr);
      if (HasStrict)
        TI->setMetadata(llvm::dxcb5_contract::kStrictBranchMD, nullptr);
    }
  }
}

void EraseDeadDxilCalls(Function &Fn) {
  bool Progress = true;
  while (Progress) {
    Progress = false;
    SmallVector<CallInst *, 32> Dead;
    for (BasicBlock &BB : Fn)
      for (Instruction &Inst : BB) {
        auto *CI = dyn_cast<CallInst>(&Inst);
        if (!CI || !CI->use_empty() || !OP::IsDxilOpFuncCallInst(CI))
          continue;
        const auto *OpC = dyn_cast<ConstantInt>(CI->getArgOperand(0));
        if (!OpC)
          continue;
        const Op Opcode = static_cast<Op>(OpC->getZExtValue());
        if (Opcode == Op::CreateHandle || Opcode == Op::CBufferLoadLegacy ||
            Opcode == Op::CBufferLoad || Opcode == Op::GetDimensions ||
            IsDeadRemovableTextureOp(Opcode))
          Dead.push_back(CI);
      }
    for (CallInst *CI : Dead) {
      CI->eraseFromParent();
      Progress = true;
    }
  }
}

void RunCleanupLoop(Module &M, Function &Fn) {
  for (unsigned Round = 0; Round < 4; ++Round) {
    const bool Folded = FoldDxilMathIntrinsics(M);
    StripAutoHintsOnConstantBranches(Fn);
    legacy::FunctionPassManager Fpm(&M);
    Fpm.add(createSCCPPass());
    Fpm.add(createInstructionCombiningPass());
    Fpm.add(createCFGSimplificationPass());
    Fpm.add(createAggressiveDCEPass());
    Fpm.doInitialization();
    const bool PassChanged = Fpm.run(Fn);
    Fpm.doFinalization();
    EraseDeadDxilCalls(Fn);
    if (!Folded && !PassChanged)
      break;
  }
}

template <typename TRes>
void ReindexResourceList(const std::vector<std::unique_ptr<TRes>> &Vec,
                         DenseMap<unsigned, unsigned> &Remap) {
  for (unsigned I = 0; I < Vec.size(); ++I) {
    const unsigned OldId = Vec[I]->GetID();
    Remap[OldId] = I;
    Vec[I]->SetID(I);
  }
}

bool HasNonStaticCreateHandle(const Function &Fn) {
  for (const BasicBlock &BB : Fn)
    for (const Instruction &Inst : BB) {
      const auto *CI = dyn_cast<CallInst>(&Inst);
      if (CI && OP::IsDxilOpFuncCallInst(CI, Op::CreateHandle) &&
          !DecodeHandle(CI).Valid)
        return true;
    }
  return false;
}

void PruneUnusedResourcesAndRemap(DxilModule &Dxil, Function &Fn,
                                  std::vector<std::string> &StrippedTextures) {
  EraseDeadDxilCalls(Fn);
  if (HasNonStaticCreateHandle(Fn))
    return;
  std::vector<std::string> BeforeSrvs;
  for (const auto &Srv : Dxil.GetSRVs())
    BeforeSrvs.push_back(Srv->GetGlobalName());

  Dxil.RemoveUnusedResources();

  std::set<std::string> AfterSrvs;
  for (const auto &Srv : Dxil.GetSRVs())
    AfterSrvs.insert(Srv->GetGlobalName());
  for (const auto &Name : BeforeSrvs)
    if (!AfterSrvs.count(Name) &&
        std::find(StrippedTextures.begin(), StrippedTextures.end(), Name) ==
            StrippedTextures.end())
      StrippedTextures.push_back(Name);

  DenseMap<unsigned, unsigned> SrvMap, UavMap, SampMap, CbMap;
  ReindexResourceList(Dxil.GetSRVs(), SrvMap);
  ReindexResourceList(Dxil.GetUAVs(), UavMap);
  ReindexResourceList(Dxil.GetSamplers(), SampMap);
  ReindexResourceList(Dxil.GetCBuffers(), CbMap);
  Type *I32Ty = Type::getInt32Ty(Fn.getContext());
  for (BasicBlock &BB : Fn)
    for (Instruction &Inst : BB) {
      auto *CI = dyn_cast<CallInst>(&Inst);
      if (!CI || !OP::IsDxilOpFuncCallInst(CI, Op::CreateHandle))
        continue;
      const HandleInfo HI = DecodeHandle(CI);
      if (!HI.Valid)
        continue;
      const DenseMap<unsigned, unsigned> *Map = nullptr;
      if (HI.Class == DXIL::ResourceClass::SRV) Map = &SrvMap;
      else if (HI.Class == DXIL::ResourceClass::UAV) Map = &UavMap;
      else if (HI.Class == DXIL::ResourceClass::Sampler) Map = &SampMap;
      else if (HI.Class == DXIL::ResourceClass::CBuffer) Map = &CbMap;
      if (Map) {
        auto It = Map->find(HI.RangeId);
        if (It != Map->end() && It->second != HI.RangeId)
          CI->setArgOperand(2, ConstantInt::get(I32Ty, It->second));
      }
    }
}

} // namespace

bool dxcb5::spec::SpecializeDxilModule(
    Module &M, const SpecManifest &Manifest, bool ApplyDefines,
    bool CompactGlobals, std::vector<std::string> &StrippedTextures,
    std::string &Error) {
  DxilModule &Dxil = M.GetOrCreateDxilModule();
  Function *EntryFn = const_cast<Function *>(Dxil.GetEntryFunction());
  if (!EntryFn) { Error = "DXIL module has no entry function"; return false; }
  std::map<SlotKey, uint32_t> SlotBits;
  BuildConstantSlotMap(Dxil, Manifest, ApplyDefines, SlotBits);
  SubstituteCBufferLoads(*EntryFn, SlotBits);
  SubstituteConstTextures(Dxil, *EntryFn, Manifest);
  RunCleanupLoop(M, *EntryFn);
  PruneUnusedResourcesAndRemap(Dxil, *EntryFn, StrippedTextures);
  if (CompactGlobals) CompactGlobalsCBuffer(Dxil, *EntryFn);
  Dxil.ReEmitDxilResources();
  return true;
}
