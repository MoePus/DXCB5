// Distributed under the University of Illinois Open Source License.
#include "dxcb5/SpecEngine.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilSignature.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Scalar.h"
#include <algorithm>
#include <set>
#include <utility>

using namespace dxcb5::spec;
using namespace hlsl;
using namespace llvm;

namespace {

using Op = DXIL::OpCode;

bool IsAlwaysLiveSystemSemantic(const DxilSignatureElement &Elem) {
  const auto Kind = Elem.GetKind();
  return Kind != DXIL::SemanticKind::Arbitrary &&
         Kind != DXIL::SemanticKind::Invalid;
}

std::string SemanticKey(StringRef Name, unsigned Index) {
  std::string Out = Name.lower();
  Out.push_back('#');
  Out += std::to_string(Index);
  return Out;
}

bool IsPsInputReadOp(const CallInst *CI) {
  return OP::IsDxilOpFuncCallInst(CI, Op::LoadInput) ||
         OP::IsDxilOpFuncCallInst(CI, Op::EvalSnapped) ||
         OP::IsDxilOpFuncCallInst(CI, Op::EvalSampleIndex) ||
         OP::IsDxilOpFuncCallInst(CI, Op::EvalCentroid);
}

unsigned SemanticIndexForRow(const DxilSignatureElement &Elem, unsigned Row) {
  const auto &IdxVec = Elem.GetSemanticIndexVec();
  return Row < IdxVec.size() ? IdxVec[Row] : Row;
}

void CollectLivePsInputElements(const Function &PsFn,
                                std::set<unsigned> &LiveSigIds) {
  for (const BasicBlock &BB : PsFn) {
    for (const Instruction &Inst : BB) {
      const auto *CI = dyn_cast<CallInst>(&Inst);
      if (!CI || !IsPsInputReadOp(CI))
        continue;
      if (const auto *SigId = dyn_cast<ConstantInt>(CI->getArgOperand(1)))
        LiveSigIds.insert(static_cast<unsigned>(SigId->getZExtValue()));
    }
  }
}

void RemapSigCallIds(Function &Fn, bool IsPsInput,
                     const DenseMap<unsigned, unsigned> &IdRemap,
                     bool EraseDropped) {
  SmallVector<CallInst *, 32> ToErase;
  Type *I32Ty = Type::getInt32Ty(Fn.getContext());
  for (BasicBlock &BB : Fn) {
    for (Instruction &Inst : BB) {
      auto *CI = dyn_cast<CallInst>(&Inst);
      if (!CI)
        continue;
      const bool Match = IsPsInput
                             ? IsPsInputReadOp(CI)
                             : OP::IsDxilOpFuncCallInst(CI, Op::StoreOutput);
      if (!Match)
        continue;
      const auto *SigId = dyn_cast<ConstantInt>(CI->getArgOperand(1));
      if (!SigId)
        continue;
      const unsigned OldId = static_cast<unsigned>(SigId->getZExtValue());
      auto It = IdRemap.find(OldId);
      if (It == IdRemap.end()) {
        if (EraseDropped)
          ToErase.push_back(CI);
        continue;
      }
      if (It->second != OldId)
        CI->setArgOperand(1, ConstantInt::get(I32Ty, It->second));
    }
  }
  for (CallInst *CI : ToErase)
    CI->eraseFromParent();
}

void RecordDroppedElem(const DxilSignatureElement &Elem,
                       std::vector<std::string> &DroppedNames) {
  const auto &IdxVec = Elem.GetSemanticIndexVec();
  for (unsigned Row = 0; Row < Elem.GetRows(); ++Row) {
    std::string Label = Elem.GetSemanticName().str();
    const unsigned SemIdx = Row < IdxVec.size() ? IdxVec[Row] : Row;
    Label += std::to_string(SemIdx);
    DroppedNames.push_back(std::move(Label));
  }
}

void RebuildSignatureKeeping(const DxilSignature &OldSig,
                             const std::set<unsigned> &KeepIds,
                             DxilSignature &NewSig,
                             DenseMap<unsigned, unsigned> &IdRemap,
                             std::vector<std::string> *DroppedNames) {
  for (const auto &Elem : OldSig.GetElements()) {
    const unsigned OldId = Elem->GetID();
    if (!KeepIds.count(OldId)) {
      if (DroppedNames)
        RecordDroppedElem(*Elem, *DroppedNames);
      continue;
    }
    auto Copy = NewSig.CreateElement();
    Copy->Initialize(Elem->GetName(), Elem->GetCompType(),
                     *Elem->GetInterpolationMode(), Elem->GetRows(),
                     Elem->GetCols(), Elem->GetStartRow(), Elem->GetStartCol(),
                     DxilSignatureElement::kUndefinedID,
                     Elem->GetSemanticIndexVec());
    Copy->SetOutputStream(Elem->GetOutputStream());
    Copy->SetDynIdxCompMask(Elem->GetDynIdxCompMask());
    Copy->SetUsageMask(Elem->GetUsageMask());
    const unsigned NewId = NewSig.AppendElement(std::move(Copy), /*bSetID=*/true);
    IdRemap[OldId] = NewId;
  }
}

bool FindFreeRowRange(const uint8_t UsedCols[32], unsigned Rows, int &OutRow) {
  for (unsigned R = 0; R + Rows <= 32; ++R) {
    bool Free = true;
    for (unsigned K = 0; K < Rows; ++K)
      if (UsedCols[R + K] != 0) { Free = false; break; }
    if (Free) {
      OutRow = static_cast<int>(R);
      return true;
    }
  }
  return false;
}

bool HasRowInterpConflict(const DXIL::InterpolationMode RowInterp[32],
                          int BaseRow, unsigned Rows,
                          DXIL::InterpolationMode Mode) {
  if (BaseRow < 0 || BaseRow + static_cast<int>(Rows) > 32)
    return false;
  for (unsigned Row = 0; Row < Rows; ++Row) {
    const auto Prev = RowInterp[BaseRow + static_cast<int>(Row)];
    if (Prev != DXIL::InterpolationMode::Invalid && Prev != Mode)
      return true;
  }
  return false;
}

bool PinMatchingPsInputsToVsOutputs(DxilSignature &VsOut,
                                    DxilSignature &PsIn, std::string &Error) {
  struct VsSlot { int Row; int Col; unsigned Cols; DxilSignatureElement *Elem; };
  std::map<std::string, VsSlot> VsAlloc;
  uint8_t UsedCols[32] = {};
  DXIL::InterpolationMode RowInterp[32];
  for (unsigned R = 0; R < 32; ++R)
    RowInterp[R] = DXIL::InterpolationMode::Invalid;
  int MaxVsRow = -1;
  for (const auto &E : VsOut.GetElements()) {
    if (!E->IsAllocated())
      continue;
    const int BaseRow = E->GetStartRow();
    const int StartCol = E->GetStartCol();
    const unsigned Rows = E->GetRows();
    const unsigned Cols = E->GetCols();
    if (BaseRow < 0 || StartCol < 0 || StartCol + static_cast<int>(Cols) > 4 ||
        BaseRow + static_cast<int>(Rows) > 32)
      continue;
    MaxVsRow = std::max(MaxVsRow, BaseRow + static_cast<int>(Rows) - 1);
    const uint8_t Mask = static_cast<uint8_t>(((1u << Cols) - 1u) << StartCol);
    for (unsigned Row = 0; Row < Rows; ++Row) {
      UsedCols[BaseRow + static_cast<int>(Row)] |= Mask;
      const std::string Key =
          SemanticKey(E->GetSemanticName(), SemanticIndexForRow(*E, Row));
      VsAlloc[Key] = {BaseRow + static_cast<int>(Row), StartCol, Cols, E.get()};
    }
  }
  int NextRow = MaxVsRow + 1;
  for (const auto &E : PsIn.GetElements()) {
    const unsigned Rows = E->GetRows();
    const unsigned Cols = E->GetCols();
    if (Rows == 0 || Cols == 0 || Cols > 4)
      continue;
    const DXIL::InterpolationMode ElemInterp =
        E->GetInterpolationMode()->GetKind();
    bool AllMatched = true;
    int MatchedRow = -1;
    int MatchedCol = -1;
    DxilSignatureElement *MatchedVsElem = nullptr;
    for (unsigned Row = 0; Row < Rows; ++Row) {
      const std::string Key =
          SemanticKey(E->GetSemanticName(), SemanticIndexForRow(*E, Row));
      auto It = VsAlloc.find(Key);
      if (It == VsAlloc.end() ||
          It->second.Col + static_cast<int>(Cols) > 4 ||
          It->second.Cols < Cols ||
          (Row > 0 && (It->second.Row != MatchedRow + static_cast<int>(Row) ||
                       It->second.Col != MatchedCol))) {
        AllMatched = false;
        break;
      }
      if (Row == 0) {
        MatchedRow = It->second.Row;
        MatchedCol = It->second.Col;
        MatchedVsElem = It->second.Elem;
      }
    }
    // Never place two PS inputs with conflicting interpolation modes in the
    // same register row (DxilValidation hard constraint, M9). If VS packed
    // them together, relocate both the PS input and matching VS output element
    // to a fresh register row so linkage and interpolation modes both hold.
    const bool InterpConflict =
        AllMatched &&
        HasRowInterpConflict(RowInterp, MatchedRow, Rows, ElemInterp);
    if (AllMatched && !InterpConflict && MatchedRow >= 0 &&
        MatchedRow + static_cast<int>(Rows) <= 32) {
      E->SetStartRow(MatchedRow);
      E->SetStartCol(MatchedCol);
      const uint8_t Mask = static_cast<uint8_t>(((1u << Cols) - 1u) << MatchedCol);
      for (unsigned Row = 0; Row < Rows; ++Row) {
        UsedCols[MatchedRow + static_cast<int>(Row)] |= Mask;
        RowInterp[MatchedRow + static_cast<int>(Row)] = ElemInterp;
      }
    } else if (InterpConflict || E->IsAllocated()) {
      const uint8_t Mask = static_cast<uint8_t>((1u << Cols) - 1u);
      int TargetRow = -1;
      if (NextRow + static_cast<int>(Rows) <= 32) {
        TargetRow = NextRow;
        NextRow += static_cast<int>(Rows);
      } else if (!FindFreeRowRange(UsedCols, Rows, TargetRow)) {
        Error = "PS input signatures exceed SM5 32-register limit after cross-stage DCE";
        return false;
      }
      if (InterpConflict &&
          (!MatchedVsElem || MatchedVsElem->GetRows() != Rows)) {
        Error = "PS input interpolation mode conflict cannot be relocated across VS/PS";
        return false;
      }
      if (InterpConflict) {
        MatchedVsElem->SetStartRow(TargetRow);
        MatchedVsElem->SetStartCol(0);
      }
      E->SetStartRow(TargetRow);
      E->SetStartCol(0);
      for (unsigned Row = 0; Row < Rows; ++Row) {
        UsedCols[TargetRow + static_cast<int>(Row)] |= Mask;
        RowInterp[TargetRow + static_cast<int>(Row)] = ElemInterp;
      }
    }
  }
  return true;
}

} // namespace

bool dxcb5::spec::PruneCrossStageSignatures(
    Module &VsMod, Module &PsMod, std::vector<std::string> &StrippedInputs,
    std::string &Error) {
  StrippedInputs.clear();
  DxilModule &VsDxil = VsMod.GetOrCreateDxilModule();
  DxilModule &PsDxil = PsMod.GetOrCreateDxilModule();
  Function *VsFn = const_cast<Function *>(VsDxil.GetEntryFunction());
  Function *PsFn = const_cast<Function *>(PsDxil.GetEntryFunction());
  if (!VsFn || !PsFn) {
    Error = "missing entry function for cross-stage signature pruning";
    return false;
  }

  // 1. Determine live PS input elements.
  std::set<unsigned> LivePsIds;
  CollectLivePsInputElements(*PsFn, LivePsIds);
  const DxilSignature &OldPsIn = PsDxil.GetInputSignature();
  std::set<unsigned> KeepPsIds;
  std::set<std::string> LivePsSemKeys;
  for (const auto &E : OldPsIn.GetElements()) {
    const bool Keep =
        LivePsIds.count(E->GetID()) != 0 || IsAlwaysLiveSystemSemantic(*E);
    if (!Keep)
      continue;
    KeepPsIds.insert(E->GetID());
    for (unsigned Row = 0; Row < E->GetRows(); ++Row)
      LivePsSemKeys.insert(
          SemanticKey(E->GetSemanticName(), SemanticIndexForRow(*E, Row)));
  }

  // 2. Rebuild PS InputSignature keeping only live inputs.
  DenseMap<unsigned, unsigned> PsIdRemap;
  DxilSignature NewPsIn(DXIL::ShaderKind::Pixel, DxilSignature::Kind::Input,
                        PsDxil.GetUseMinPrecision());
  RebuildSignatureKeeping(OldPsIn, KeepPsIds, NewPsIn, PsIdRemap,
                          &StrippedInputs);

  // 3. Determine live VS output elements (system outputs + matched PS inputs).
  const DxilSignature &OldVsOut = VsDxil.GetOutputSignature();
  std::set<unsigned> KeepVsIds;
  for (const auto &E : OldVsOut.GetElements()) {
    if (IsAlwaysLiveSystemSemantic(*E)) {
      KeepVsIds.insert(E->GetID());
      continue;
    }
    bool Matched = false;
    for (unsigned Row = 0; Row < E->GetRows(); ++Row) {
      if (LivePsSemKeys.count(
              SemanticKey(E->GetSemanticName(), SemanticIndexForRow(*E, Row)))) {
        Matched = true;
        break;
      }
    }
    if (Matched)
      KeepVsIds.insert(E->GetID());
  }

  // 4. Rebuild VS OutputSignature keeping only live outputs, and pin PS inputs.
  DenseMap<unsigned, unsigned> VsIdRemap;
  DxilSignature NewVsOut(DXIL::ShaderKind::Vertex, DxilSignature::Kind::Output,
                         VsDxil.GetUseMinPrecision());
  RebuildSignatureKeeping(OldVsOut, KeepVsIds, NewVsOut, VsIdRemap, nullptr);
  if (!PinMatchingPsInputsToVsOutputs(NewVsOut, NewPsIn, Error))
    return false;

  // 5. Commit updated signatures, remap loadInput/eval*/storeOutput IDs, and DCE VS.
  PsDxil.GetInputSignature() = std::move(NewPsIn);
  VsDxil.GetOutputSignature() = std::move(NewVsOut);
  RemapSigCallIds(*PsFn, /*IsPsInput=*/true, PsIdRemap, /*EraseDropped=*/false);
  RemapSigCallIds(*VsFn, /*IsPsInput=*/false, VsIdRemap, /*EraseDropped=*/true);

  legacy::FunctionPassManager VsFpm(&VsMod);
  VsFpm.add(createAggressiveDCEPass());
  VsFpm.add(createCFGSimplificationPass());
  VsFpm.add(createAggressiveDCEPass());
  VsFpm.doInitialization();
  VsFpm.run(*VsFn);
  VsFpm.doFinalization();

  PsDxil.ReEmitDxilResources();
  VsDxil.ReEmitDxilResources();
  return true;
}
