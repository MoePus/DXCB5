// Distributed under the University of Illinois Open Source License.
#include "Spec/SpecInternal.h"
#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilTypeSystem.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace dxcb5::spec;
using namespace hlsl;
using namespace llvm;

namespace {

using Op = DXIL::OpCode;

constexpr const char *kCompactName = "$Globals.dxcb5.compact";
constexpr const char *kSharedName = "$Globals.dxcb5.shared";

struct DynGlobalsLoad {
  CallInst *CI = nullptr;
  unsigned FieldIdx = 0;
};

struct StageFieldInfo {
  unsigned OldIdx = 0;
  unsigned OldOff = 0;
  unsigned Size = 0;
  bool IsAggregate = false;
  bool Live = false;
  bool DynIndexed = false;
  std::string Name;
};

struct StageGlobalsState {
  DxilModule *Dxil = nullptr;
  Function *Fn = nullptr;
  DxilCBuffer *Globals = nullptr;
  StructType *OldST = nullptr;
  const DxilStructAnnotation *OldAnn = nullptr;
  std::set<unsigned> LiveDwords;
  std::set<unsigned> LiveFields;
  SmallVector<CallInst *, 32> ConstLoads;
  SmallVector<DynGlobalsLoad, 16> DynLoads;
  std::vector<StageFieldInfo> Fields;
  std::map<std::string, size_t> ByName;
};

DxilCBuffer *FindGlobalsCBuffer(DxilModule &Dxil) {
  for (const auto &CB : Dxil.GetCBuffers())
    if (CB->GetGlobalName() == "$Globals")
      return CB.get();
  return nullptr;
}

bool CollectStageGlobalsState(Module &M, StageGlobalsState &S) {
  S.Dxil = &M.GetOrCreateDxilModule();
  S.Fn = const_cast<Function *>(S.Dxil->GetEntryFunction());
  S.Globals = FindGlobalsCBuffer(*S.Dxil);
  if (!S.Fn || !S.Globals)
    return false;
  S.OldST =
      dyn_cast<StructType>(S.Globals->GetHLSLType()->getPointerElementType());
  auto &Types = S.Dxil->GetTypeSystem();
  S.OldAnn = S.OldST ? Types.GetStructAnnotation(S.OldST) : nullptr;
  if (!S.OldST || !S.OldAnn || S.OldST->getNumElements() == 0)
    return false;

  const unsigned GlobalsId = S.Globals->GetID();
  for (BasicBlock &BB : *S.Fn) {
    for (Instruction &Inst : BB) {
      auto *CI = dyn_cast<CallInst>(&Inst);
      if (!CI || !OP::IsDxilOpFuncCallInst(CI, Op::CBufferLoadLegacy))
        continue;
      const HandleInfo HI = DecodeHandle(CI->getArgOperand(1));
      if (!HI.Valid)
        return false;
      if (HI.Class != DXIL::ResourceClass::CBuffer || HI.RangeId != GlobalsId)
        continue;
      if (const auto *RowC = dyn_cast<ConstantInt>(CI->getArgOperand(2))) {
        S.ConstLoads.push_back(CI);
        RecordConstCBufferLoadDwords(
            CI, static_cast<unsigned>(RowC->getZExtValue()), S.LiveDwords);
        continue;
      }
      unsigned FieldIdx = 0;
      if (!ResolveDynamicCBufferField(CI->getArgOperand(2), &BB, *S.Globals,
                                      Types, FieldIdx))
        return false;
      S.LiveFields.insert(FieldIdx);
      S.DynLoads.push_back({CI, FieldIdx});
    }
  }

  for (unsigned I = 0; I < S.OldST->getNumElements(); ++I) {
    Type *ElemTy = S.OldST->getElementType(I);
    if (IsEmptyCBufferField(ElemTy))
      continue;
    const auto &F = S.OldAnn->GetFieldAnnotation(I);
    if (!F.HasCBufferOffset() || (F.GetCBufferOffset() % 4) != 0)
      return false;
    const unsigned OldOff = F.GetCBufferOffset();
    const unsigned Sz = ComputeCBufferFieldByteSize(ElemTy, &F, Types);
    if (Sz == 0 || (Sz % 4) != 0)
      return false;
    const bool IsAgg = isa<StructType>(ElemTy) || isa<ArrayType>(ElemTy) ||
                       F.HasMatrixAnnotation() || Sz > 16;
    if (IsAgg && (OldOff % 16) != 0)
      return false;
    bool Live = S.LiveFields.count(I) != 0;
    for (unsigned B = OldOff; !Live && B < OldOff + Sz; B += 4)
      if (S.LiveDwords.count(B / 4))
        Live = true;
    StageFieldInfo Info;
    Info.OldIdx = I;
    Info.OldOff = OldOff;
    Info.Size = Sz;
    Info.IsAggregate = IsAgg;
    Info.Live = Live;
    Info.DynIndexed = S.LiveFields.count(I) != 0;
    Info.Name = F.GetFieldName();
    S.ByName[Info.Name] = S.Fields.size();
    S.Fields.push_back(std::move(Info));
  }
  return true;
}

unsigned AllocLooseFieldOffset(
    unsigned Sz, SmallVectorImpl<std::pair<unsigned, unsigned>> &LooseRows,
    unsigned &CurOff) {
  int BestRow = -1;
  unsigned BestRem = 17;
  for (int R = 0; R < static_cast<int>(LooseRows.size()); ++R) {
    const unsigned Rem = 16u - LooseRows[R].second;
    if (Rem >= Sz && Rem < BestRem) {
      BestRem = Rem;
      BestRow = R;
    }
  }
  if (BestRow >= 0) {
    const unsigned Off = LooseRows[BestRow].first + LooseRows[BestRow].second;
    LooseRows[BestRow].second += Sz;
    return Off;
  }
  const unsigned Off = CurOff;
  LooseRows.push_back({CurOff, Sz});
  CurOff += 16u;
  return Off;
}

struct KeptStageField {
  unsigned OldIdx = 0;
  unsigned OldOff = 0;
  unsigned NewOff = 0;
  unsigned Size = 0;
};

struct StageRewritePlan {
  SmallVector<KeptStageField, 32> Kept;
  DenseMap<unsigned, unsigned> DwordRemap;
  DenseMap<unsigned, int> FieldRowDelta;
};

bool ValidateStageRewrite(const StageGlobalsState &S,
                          const StageRewritePlan &Plan) {
  for (CallInst *CI : S.ConstLoads)
    if (!CanRewriteGlobalsLoad(CI, Plan.DwordRemap))
      return false;
  for (const auto &D : S.DynLoads)
    if (!Plan.FieldRowDelta.count(D.FieldIdx))
      return false;
  return true;
}

void ApplyStageCompaction(StageGlobalsState &S, const StageRewritePlan &Plan,
                          unsigned NewSize, const char *StructName) {
  for (CallInst *CI : S.ConstLoads)
    RewriteOneGlobalsLoad(CI, Plan.DwordRemap);
  for (const auto &D : S.DynLoads)
    RewriteDynamicGlobalsLoad(D.CI, Plan.FieldRowDelta.lookup(D.FieldIdx));

  SmallVector<Type *, 32> NewElemTys;
  for (const auto &K : Plan.Kept)
    NewElemTys.push_back(S.OldST->getElementType(K.OldIdx));
  StructType *NewST =
      StructType::create(S.Fn->getContext(), NewElemTys, StructName);
  auto &Types = S.Dxil->GetTypeSystem();
  DxilStructAnnotation *NewAnn = Types.AddStructAnnotation(NewST);
  NewAnn->SetCBufferSize(NewSize);
  for (unsigned NI = 0; NI < Plan.Kept.size(); ++NI) {
    DxilFieldAnnotation FieldCopy =
        S.OldAnn->GetFieldAnnotation(Plan.Kept[NI].OldIdx);
    FieldCopy.SetCBufferOffset(Plan.Kept[NI].NewOff);
    FieldCopy.SetCBVarUsed(true);
    NewAnn->GetFieldAnnotation(NI) = FieldCopy;
  }
  S.Globals->SetSize(NewSize);
  PointerType *NewPtrTy = PointerType::getUnqual(NewST);
  S.Globals->SetHLSLType(NewPtrTy);
  if (auto *OldGV =
          dyn_cast_or_null<GlobalVariable>(S.Globals->GetGlobalSymbol())) {
    auto *NewGV = new GlobalVariable(
        *S.Fn->getParent(), NewST, OldGV->isConstant(), OldGV->getLinkage(),
        nullptr, "", nullptr, OldGV->getThreadLocalMode(),
        OldGV->getType()->getAddressSpace());
    NewGV->takeName(OldGV);
    S.Globals->SetGlobalSymbol(NewGV);
    S.Globals->SetHLSLType(NewGV->getType());
    if (OldGV->use_empty())
      OldGV->eraseFromParent();
  } else {
    S.Globals->SetGlobalSymbol(UndefValue::get(NewPtrTy));
  }
}

void CompactSingleStageImpl(Module &M, bool MarkShared) {
  StageGlobalsState S;
  if (!CollectStageGlobalsState(M, S) ||
      (S.ConstLoads.empty() && S.DynLoads.empty()))
    return;
  if (!MarkShared && S.OldST->getName().startswith(kSharedName))
    return;

  StageRewritePlan Plan;
  SmallVector<std::pair<unsigned, unsigned>, 16> LooseRows;
  unsigned CurOff = 0;
  for (const auto &F : S.Fields) {
    if (!F.Live)
      continue;
    const unsigned NewOff =
        F.IsAggregate ? CurOff : AllocLooseFieldOffset(F.Size, LooseRows, CurOff);
    if (F.IsAggregate)
      CurOff += (F.Size + 15u) & ~15u;
    if (F.DynIndexed && ((F.OldOff % 16) != 0 || (NewOff % 16) != 0))
      return;
    Plan.Kept.push_back({F.OldIdx, F.OldOff, NewOff, F.Size});
    Plan.FieldRowDelta[F.OldIdx] =
        static_cast<int>(NewOff / 16) - static_cast<int>(F.OldOff / 16);
    for (unsigned D = 0; D * 4 < F.Size; ++D)
      Plan.DwordRemap[F.OldOff / 4 + D] = NewOff / 4 + D;
  }
  const unsigned NewSize = (CurOff + 15u) & ~15u;
  if (Plan.Kept.empty())
    return;
  const bool AlreadyOptimal = Plan.Kept.size() == S.OldST->getNumElements() &&
                              NewSize >= S.Globals->GetSize() &&
                              (!MarkShared || S.OldST->getName().startswith(kSharedName));
  if (AlreadyOptimal)
    return;
  std::sort(Plan.Kept.begin(), Plan.Kept.end(),
            [](const KeptStageField &A, const KeptStageField &B) {
              return A.NewOff < B.NewOff;
            });
  if (!ValidateStageRewrite(S, Plan))
    return;
  ApplyStageCompaction(S, Plan, NewSize, MarkShared ? kSharedName : kCompactName);
  if (MarkShared)
    S.Dxil->ReEmitDxilResources();
}

void RecordStageFieldMapping(const StageFieldInfo &F, unsigned NewOff,
                             StageRewritePlan &Plan) {
  Plan.Kept.push_back({F.OldIdx, F.OldOff, NewOff, F.Size});
  Plan.FieldRowDelta[F.OldIdx] =
      static_cast<int>(NewOff / 16) - static_cast<int>(F.OldOff / 16);
  for (unsigned D = 0; D * 4 < F.Size; ++D)
    Plan.DwordRemap[F.OldOff / 4 + D] = NewOff / 4 + D;
}

} // namespace

bool dxcb5::spec::IsCBufferFieldUsedByDwords(
    Type *Ty, const DxilFieldAnnotation &Field, const DxilTypeSystem &Types,
    const std::set<unsigned> &LiveDwords) {
  if (!Field.HasCBufferOffset())
    return true;
  const unsigned Off = Field.GetCBufferOffset();
  const unsigned Sz = ComputeCBufferFieldByteSize(Ty, &Field, Types);
  if (Sz == 0 || (Off % 4) != 0 || (Sz % 4) != 0)
    return true;
  for (unsigned B = Off; B < Off + Sz; B += 4)
    if (LiveDwords.count(B / 4) != 0)
      return true;
  return false;
}

void dxcb5::spec::CompactGlobalsCBuffer(DxilModule &Dxil, Function &Fn) {
  if (Module *M = Fn.getParent())
    CompactSingleStageImpl(*M, /*MarkShared=*/false);
}

void dxcb5::spec::CompactSharedGlobalsCBuffer(Module &VsMod, Module &PsMod) {
  DxilModule &VsDxil = VsMod.GetOrCreateDxilModule();
  DxilModule &PsDxil = PsMod.GetOrCreateDxilModule();
  DxilCBuffer *VsCb = FindGlobalsCBuffer(VsDxil);
  DxilCBuffer *PsCb = FindGlobalsCBuffer(PsDxil);
  if (!VsCb && !PsCb)
    return;
  if (!VsCb) {
    CompactSingleStageImpl(PsMod, /*MarkShared=*/true);
    return;
  }
  if (!PsCb) {
    CompactSingleStageImpl(VsMod, /*MarkShared=*/true);
    return;
  }

  StageGlobalsState Vs, Ps;
  if (!CollectStageGlobalsState(VsMod, Vs) || !CollectStageGlobalsState(PsMod, Ps))
    return;

  struct SharedField {
    std::string Name;
    unsigned Size = 0;
    bool IsAggregate = false;
    bool Live = false;
    int VsIdx = -1;
    int PsIdx = -1;
    unsigned NewOff = 0;
  };
  std::vector<SharedField> Shared;
  std::map<std::string, size_t> SharedByName;
  for (size_t I = 0; I < Vs.Fields.size(); ++I) {
    const auto &F = Vs.Fields[I];
    if (F.Name.empty())
      return;
    SharedByName[F.Name] = Shared.size();
    Shared.push_back({F.Name, F.Size, F.IsAggregate, F.Live, static_cast<int>(I), -1, 0});
  }
  for (size_t I = 0; I < Ps.Fields.size(); ++I) {
    const auto &F = Ps.Fields[I];
    if (F.Name.empty())
      return;
    auto It = SharedByName.find(F.Name);
    if (It != SharedByName.end()) {
      auto &SF = Shared[It->second];
      if (SF.Size != F.Size || SF.IsAggregate != F.IsAggregate)
        return;
      SF.Live |= F.Live;
      SF.PsIdx = static_cast<int>(I);
    } else {
      SharedByName[F.Name] = Shared.size();
      Shared.push_back({F.Name, F.Size, F.IsAggregate, F.Live, -1, static_cast<int>(I), 0});
    }
  }

  SmallVector<std::pair<unsigned, unsigned>, 16> LooseRows;
  unsigned CurOff = 0;
  StageRewritePlan VsPlan, PsPlan;
  for (auto &SF : Shared) {
    if (!SF.Live)
      continue;
    SF.NewOff = SF.IsAggregate ? CurOff : AllocLooseFieldOffset(SF.Size, LooseRows, CurOff);
    if (SF.IsAggregate)
      CurOff += (SF.Size + 15u) & ~15u;
    if (SF.VsIdx >= 0) {
      const auto &VF = Vs.Fields[static_cast<size_t>(SF.VsIdx)];
      if (VF.DynIndexed && ((VF.OldOff % 16) != 0 || (SF.NewOff % 16) != 0))
        return;
      RecordStageFieldMapping(VF, SF.NewOff, VsPlan);
    }
    if (SF.PsIdx >= 0) {
      const auto &PF = Ps.Fields[static_cast<size_t>(SF.PsIdx)];
      if (PF.DynIndexed && ((PF.OldOff % 16) != 0 || (SF.NewOff % 16) != 0))
        return;
      RecordStageFieldMapping(PF, SF.NewOff, PsPlan);
    }
  }

  const unsigned NewSize = (CurOff + 15u) & ~15u;
  if (VsPlan.Kept.empty() && PsPlan.Kept.empty())
    return;
  auto ByOffset = [](const KeptStageField &A, const KeptStageField &B) {
    return A.NewOff < B.NewOff;
  };
  std::sort(VsPlan.Kept.begin(), VsPlan.Kept.end(), ByOffset);
  std::sort(PsPlan.Kept.begin(), PsPlan.Kept.end(), ByOffset);
  if (!ValidateStageRewrite(Vs, VsPlan) || !ValidateStageRewrite(Ps, PsPlan))
    return;

  if (!VsPlan.Kept.empty()) {
    ApplyStageCompaction(Vs, VsPlan, NewSize, kSharedName);
    VsDxil.ReEmitDxilResources();
  }
  if (!PsPlan.Kept.empty()) {
    ApplyStageCompaction(Ps, PsPlan, NewSize, kSharedName);
    PsDxil.ReEmitDxilResources();
  }
}
