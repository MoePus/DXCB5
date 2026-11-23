// Distributed under the University of Illinois Open Source License.
#include "Spec/SpecInternal.h"
#include "dxcb5/Formats.h"
#include "Spec/SpecJson.h"
#include "dxcb5/SpecEngine.h"
#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilResource.h"
#include "dxc/DXIL/DxilShaderModel.h"
#include "dxc/DXIL/DxilTypeSystem.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace dxcb5::spec;
using namespace hlsl;
using namespace llvm;

namespace {

struct VarMeta {
  std::string Policy;
  std::vector<float> RawSpecVals;
};

using CbVarKey = std::pair<std::string, std::string>;

void ParseSpecMetaNode(const MDNode *Node, std::map<CbVarKey, VarMeta> &CbMeta,
                       std::map<std::string, std::string> &ResMeta) {
  if (!Node || Node->getNumOperands() < 3)
    return;
  const auto *Kind = dyn_cast<MDString>(Node->getOperand(0));
  if (!Kind)
    return;
  if (Kind->getString() == "res") {
    const auto *ResName = dyn_cast<MDString>(Node->getOperand(1));
    const auto *Pol = dyn_cast<MDString>(Node->getOperand(2));
    if (ResName && Pol)
      ResMeta[ResName->getString().str()] = Pol->getString().str();
    return;
  }
  if (Kind->getString() != "cb" || Node->getNumOperands() < 4)
    return;
  const auto *CbName = dyn_cast<MDString>(Node->getOperand(1));
  const auto *VarName = dyn_cast<MDString>(Node->getOperand(2));
  const auto *Pol = dyn_cast<MDString>(Node->getOperand(3));
  if (!CbName || !VarName || !Pol)
    return;
  VarMeta VM;
  VM.Policy = Pol->getString().str();
  for (unsigned J = 4; J < Node->getNumOperands(); ++J) {
    const auto *CM = dyn_cast<ConstantAsMetadata>(Node->getOperand(J));
    const auto *CI = CM ? dyn_cast<ConstantInt>(CM->getValue()) : nullptr;
    if (!CI)
      continue;
    const uint32_t Bits = static_cast<uint32_t>(CI->getZExtValue());
    float F = 0.0f;
    std::memcpy(&F, &Bits, sizeof(F));
    VM.RawSpecVals.push_back(F);
  }
  CbMeta[{CbName->getString().str(), VarName->getString().str()}] =
      std::move(VM);
}

void ReadSpecMeta(const Module &M, std::map<CbVarKey, VarMeta> &CbMeta,
                  std::map<std::string, std::string> &ResMeta) {
  const NamedMDNode *NMD = M.getNamedMetadata("dxcb5.spec.meta");
  if (!NMD)
    return;
  for (unsigned I = 0; I < NMD->getNumOperands(); ++I)
    ParseSpecMetaNode(NMD->getOperand(I), CbMeta, ResMeta);
}

const char *ScalarTypeName(const DxilFieldAnnotation &Field, const Type *Ty) {
  using K = DXIL::ComponentType;
  switch (Field.GetCompType().GetKind()) {
  case K::I1: return "bool";
  case K::I16:
  case K::I32: return "i32";
  case K::U16:
  case K::U32: return "u32";
  default: return Ty->getScalarType()->isIntegerTy() ? "i32" : "f32";
  }
}

unsigned FieldComponents(const DxilFieldAnnotation &Field, const Type *Ty) {
  unsigned Elements = 1;
  if (Field.HasMatrixAnnotation()) {
    const auto &Mat = Field.GetMatrixAnnotation();
    const bool RowMaj = Mat.Orientation == MatrixOrientation::RowMajor;
    const unsigned MajorCount = RowMaj ? Mat.Rows : Mat.Cols;
    while (const auto *Arr = dyn_cast<ArrayType>(Ty)) {
      if (MajorCount > 1 && !isa<ArrayType>(Arr->getElementType()))
        break;
      Elements *= static_cast<unsigned>(Arr->getNumElements());
      Ty = Arr->getElementType();
    }
    return Elements * Mat.Rows * Mat.Cols;
  }
  while (const auto *Arr = dyn_cast<ArrayType>(Ty)) {
    Elements *= static_cast<unsigned>(Arr->getNumElements());
    Ty = Arr->getElementType();
  }
  unsigned Comps = Field.GetVectorSize();
  if (const auto *VT = dyn_cast<VectorType>(Ty))
    Comps = VT->getNumElements();
  return Elements * (Comps ? Comps : 1u);
}

const char *ResourceKindName(DXIL::ResourceKind Kind) {
  using K = DXIL::ResourceKind;
  switch (Kind) {
  case K::Texture1D: return "tex1d";
  case K::Texture1DArray: return "tex1d_array";
  case K::Texture2D: return "tex2d";
  case K::Texture2DArray: return "tex2d_array";
  case K::Texture2DMS: return "tex2dms";
  case K::Texture2DMSArray: return "tex2dms_array";
  case K::Texture3D: return "tex3d";
  case K::TextureCube: return "texcube";
  case K::TextureCubeArray: return "texcube_array";
  case K::TypedBuffer: return "typed_buffer";
  case K::StructuredBuffer: return "structured_buffer";
  case K::RawBuffer: return "raw_buffer";
  default: return "unknown";
  }
}

void WriteSpecValues2D(JsonWriter &W, const VarMeta &VM,
                       const DxilFieldAnnotation &Field, unsigned Comps) {
  W.Key("spec_values");
  W.BeginArray();
  if (VM.Policy == "spec_zero_one") {
    for (double V : {0.0, 1.0}) {
      W.BeginArray();
      for (unsigned C = 0; C < Comps; ++C)
        W.Double(V);
      W.EndArray();
    }
  } else if (VM.Policy == "spec_identity") {
    W.BeginArray();
    if (Field.HasMatrixAnnotation()) {
      const auto &Mat = Field.GetMatrixAnnotation();
      const bool RowMaj = Mat.Orientation == MatrixOrientation::RowMajor;
      const unsigned MajorCount = RowMaj ? Mat.Rows : Mat.Cols;
      const unsigned MinorWidth = RowMaj ? Mat.Cols : Mat.Rows;
      const unsigned N = MajorCount * MinorWidth;
      const unsigned Count = N ? Comps / N : 1u;
      for (unsigned E = 0; E < Count; ++E)
        for (unsigned M = 0; M < MajorCount; ++M)
          for (unsigned C = 0; C < MinorWidth; ++C)
            W.Double(M == C ? 1.0 : 0.0);
    } else {
      for (unsigned C = 0; C < Comps; ++C)
        W.Double(1.0);
    }
    W.EndArray();
  } else if (VM.Policy == "spec_values") {
    if (Comps <= 1 || VM.RawSpecVals.size() % Comps != 0) {
      for (float V : VM.RawSpecVals) {
        W.BeginArray();
        W.Double(static_cast<double>(V));
        W.EndArray();
      }
    } else {
      for (size_t I = 0; I < VM.RawSpecVals.size(); I += Comps) {
        W.BeginArray();
        for (unsigned C = 0; C < Comps; ++C)
          W.Double(static_cast<double>(VM.RawSpecVals[I + C]));
        W.EndArray();
      }
    }
  }
  W.EndArray();
}

void EmitVariablesAndFlags(JsonWriter &W, const DxilModule &Dxil,
                           const std::map<CbVarKey, VarMeta> &CbMeta,
                           std::vector<std::string> &SpecFlags) {
  const auto &Types = Dxil.GetTypeSystem();
  W.Key("variables");
  W.BeginArray();
  for (const auto &CB : Dxil.GetCBuffers()) {
    const std::string CbName = CB->GetGlobalName();
    auto *ST = dyn_cast<StructType>(CB->GetHLSLType()->getPointerElementType());
    const auto *Ann = ST ? Types.GetStructAnnotation(ST) : nullptr;
    if (!Ann)
      continue;
    for (unsigned I = 0; I < ST->getNumElements(); ++I) {
      const auto &Field = Ann->GetFieldAnnotation(I);
      const std::string VarName = Field.GetFieldName();
      if (CbName == "DXCB5_SpecFlags") {
        SpecFlags.push_back(VarName);
        continue;
      }
      auto It = CbMeta.find({CbName, VarName});
      if (It == CbMeta.end())
        continue;
      const VarMeta &VM = It->second;
      const unsigned Comps = FieldComponents(Field, ST->getElementType(I));
      const bool IsValuesPolicy =
          (VM.Policy == "spec_values" || VM.Policy == "spec_zero_one" ||
           VM.Policy == "spec_identity");
      W.BeginObject();
      W.Key("name"); W.String(VarName);
      W.Key("cbuffer"); W.String(CbName);
      W.Key("type"); W.String(ScalarTypeName(Field, ST->getElementType(I)));
      W.Key("components"); W.UInt(Comps);
      W.Key("policy"); W.String(IsValuesPolicy ? "spec_values" : VM.Policy);
      if (IsValuesPolicy)
        WriteSpecValues2D(W, VM, Field, Comps);
      W.EndObject();
    }
  }
  W.EndArray();
}

void CollectLiveSpecFlags(const DxilModule &Dxil,
                          std::vector<std::string> &LiveFlags) {
  const Function *Entry = Dxil.GetEntryFunction();
  if (!Entry)
    return;
  const DxilCBuffer *FlagsCb = nullptr;
  for (const auto &CB : Dxil.GetCBuffers())
    if (CB->GetGlobalName() == "DXCB5_SpecFlags") {
      FlagsCb = CB.get();
      break;
    }
  if (!FlagsCb)
    return;
  auto *ST =
      dyn_cast<StructType>(FlagsCb->GetHLSLType()->getPointerElementType());
  const auto *Ann = ST ? Dxil.GetTypeSystem().GetStructAnnotation(ST) : nullptr;
  if (!ST || !Ann)
    return;

  const unsigned FlagsId = FlagsCb->GetID();
  std::set<unsigned> LiveDwords;
  bool AllLive = false;
  for (const BasicBlock &BB : *Entry) {
    for (const Instruction &Inst : BB) {
      const auto *CI = dyn_cast<CallInst>(&Inst);
      if (!CI || !OP::IsDxilOpFuncCallInst(CI, DXIL::OpCode::CBufferLoadLegacy))
        continue;
      const HandleInfo HI = DecodeHandle(CI->getArgOperand(1));
      if (!HI.Valid) {
        AllLive = true;
        continue;
      }
      if (HI.Class != DXIL::ResourceClass::CBuffer || HI.RangeId != FlagsId)
        continue;
      const auto *RowC = dyn_cast<ConstantInt>(CI->getArgOperand(2));
      if (!RowC) {
        AllLive = true;
        continue;
      }
      RecordConstCBufferLoadDwords(
          CI, static_cast<unsigned>(RowC->getZExtValue()), LiveDwords);
    }
  }
  for (unsigned I = 0; I < ST->getNumElements(); ++I) {
    const auto &F = Ann->GetFieldAnnotation(I);
    if (AllLive || IsCBufferFieldUsedByDwords(ST->getElementType(I), F,
                                              Dxil.GetTypeSystem(), LiveDwords))
      LiveFlags.push_back(F.GetFieldName());
  }
}

} // namespace

bool dxcb5::spec::SerializeModuleBitcode(const Module &M, std::string &Out) {
  Out.clear();
  raw_string_ostream OS(Out);
  WriteBitcodeToFile(&M, OS, /*ShouldPreserveUseListOrder=*/false);
  OS.flush();
  return !Out.empty();
}

std::unique_ptr<Module>
dxcb5::spec::ParseModuleBitcode(StringRef Bytes, LLVMContext &Ctx,
                                std::string &Error) {
  auto Buf = MemoryBuffer::getMemBuffer(Bytes, "dxcb5_spec", false);
  ErrorOr<std::unique_ptr<Module>> Parsed =
      parseBitcodeFile(Buf->getMemBufferRef(), Ctx);
  if (!Parsed) {
    Error = "failed to parse DXIL bitcode: " + Parsed.getError().message();
    return nullptr;
  }
  return std::move(Parsed.get());
}

bool dxcb5::spec::InspectDxilModule(const Module &M, StringRef RawHashHex,
                                    std::string &OutJson, std::string &Error) {
  auto &MutM = const_cast<Module &>(M);
  if (!MutM.HasDxilModule()) {
    Error = "LLVM module does not contain DXIL metadata";
    return false;
  }
  const DxilModule &Dxil = MutM.GetDxilModule();
  std::map<CbVarKey, VarMeta> CbMeta;
  std::map<std::string, std::string> ResMeta;
  ReadSpecMeta(M, CbMeta, ResMeta);

  const auto *SM = Dxil.GetShaderModel();
  const char *StageStr = SM->IsPS() ? "ps_5_0" : (SM->IsVS() ? "vs_5_0" : "unknown");
  const Function *EntryFn = Dxil.GetEntryFunction();
  const std::string EntryName =
      EntryFn ? Dxil.GetEntryFunctionName() : std::string();

  JsonWriter W;
  W.BeginObject();
  W.Key("schema_version"); W.UInt(DXCB5_SPEC_SCHEMA_VERSION);
  W.Key("abi_version"); W.UInt(1);
  W.Key("uber_dxil_hash"); W.String(RawHashHex);
  W.Key("stage"); W.String(StageStr);
  W.Key("entry"); W.String(EntryName);

  std::vector<std::string> SpecFlags;
  EmitVariablesAndFlags(W, Dxil, CbMeta, SpecFlags);

  W.Key("spec_flags");
  W.BeginArray();
  for (const auto &F : SpecFlags)
    W.String(F);
  W.EndArray();

  std::vector<std::string> LiveSpecFlags;
  CollectLiveSpecFlags(Dxil, LiveSpecFlags);
  W.Key("live_spec_flags");
  W.BeginArray();
  for (const auto &F : LiveSpecFlags)
    W.String(F);
  W.EndArray();

  W.Key("textures");
  W.BeginArray();
  for (const auto &Srv : Dxil.GetSRVs()) {
    const std::string Name = Srv->GetGlobalName();
    auto It = ResMeta.find(Name);
    W.BeginObject();
    W.Key("name"); W.String(Name);
    W.Key("kind"); W.String(ResourceKindName(Srv->GetKind()));
    W.Key("policy"); W.String(It == ResMeta.end() ? "default" : It->second);
    W.EndObject();
  }
  W.EndArray();

  W.EndObject();
  OutJson = W.Take();
  return true;
}

bool dxcb5::spec::InspectDxilBytes(StringRef DxilBytes, std::string &OutJson,
                                   std::string &Error) {
  LLVMContext Ctx;
  auto Mod = ParseModuleBitcode(DxilBytes, Ctx, Error);
  if (!Mod)
    return false;
  Mod->GetOrCreateDxilModule();
  const std::string HashHex =
      ComputeSha256Hex(DxilBytes.data(), DxilBytes.size());
  return InspectDxilModule(*Mod, HashHex, OutJson, Error);
}
