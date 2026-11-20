// Distributed under the University of Illinois Open Source License.
#include "Import.h"
#include "dxcb5/DxbcWriter.h"
#include "dxcb5/InstructionInfo.h"
#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilResource.h"
#include "dxc/DXIL/DxilSampler.h"
#include "dxc/DXIL/DxilShaderModel.h"
#include "dxc/DXIL/DxilTypeSystem.h"
#include "dxc/DxilContainer/DxilContainer.h"
#include "dxc/Support/WinIncludes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Endian.h"
#include <algorithm>
#include <d3d11.h>
#include <d3d11shader.h>

using namespace dxcb5;
using namespace hlsl;
using namespace llvm;

namespace {

bool RasterizerInput(const SignatureElement &Element) {
  return Element.SystemValue == D3D_NAME_IS_FRONT_FACE ||
         Element.SystemValue == D3D_NAME_SAMPLE_INDEX ||
         Element.SystemValue == D3D_NAME_PRIMITIVE_ID;
}

bool IsSpecialPixelOutput(const SignatureElement &Element) {
  return Element.SystemValue == D3D_NAME_DEPTH ||
         Element.SystemValue == D3D_NAME_DEPTH_GREATER_EQUAL ||
         Element.SystemValue == D3D_NAME_DEPTH_LESS_EQUAL ||
         Element.SystemValue == D3D_NAME_COVERAGE;
}

bool LinkInput(SignatureElement &Element,
               ArrayRef<SignatureElement> Vertex,
               uint32_t &Next, std::string &Error) {
  if (RasterizerInput(Element)) {
    Element.Register = Next++;
    return true;
  }
  auto Match = std::find_if(Vertex.begin(), Vertex.end(), [&](const SignatureElement &Out) {
    return StringRef(Out.Semantic).equals_lower(Element.Semantic) &&
           Out.SemanticIndex == Element.SemanticIndex;
  });
  if (Match == Vertex.end() || Match->ComponentType != Element.ComponentType ||
      Popcount4(Match->Mask) < Popcount4(Element.Mask)) {
    Error = "incompatible vertex output for pixel input " + Element.Semantic +
            std::to_string(Element.SemanticIndex);
    return false;
  }
  Element.Register = Match->Register;
  const uint8_t LogicalUsage = Element.UsedMask;
  const unsigned Width = Popcount4(Element.Mask);
  Element.Mask = 0;
  Element.UsedMask = 0;
  unsigned Column = 0;
  for (unsigned Lane = 0; Lane < 4 && Column < Width; ++Lane) {
    const unsigned Bit = 1u << Lane;
    if (!(Match->Mask & Bit))
      continue;
    Element.Mask |= Bit;
    if (LogicalUsage & (1u << Column))
      Element.UsedMask |= Bit;
    ++Column;
  }
  return true;
}

uint32_t SignatureComponent(const DxilSignatureElement &Element) {
  switch (Element.GetCompType().GetKind()) {
  case DXIL::ComponentType::I1:
  case DXIL::ComponentType::U16:
  case DXIL::ComponentType::U32: return D3D_REGISTER_COMPONENT_UINT32;
  case DXIL::ComponentType::I16:
  case DXIL::ComponentType::I32: return D3D_REGISTER_COMPONENT_SINT32;
  case DXIL::ComponentType::F16:
  case DXIL::ComponentType::F32:
  case DXIL::ComponentType::SNormF16:
  case DXIL::ComponentType::UNormF16:
  case DXIL::ComponentType::SNormF32:
  case DXIL::ComponentType::UNormF32: return D3D_REGISTER_COMPONENT_FLOAT32;
  default: return D3D_REGISTER_COMPONENT_UNKNOWN;
  }
}

bool ImportSignature(const DxilSignature &Source,
                     ArrayRef<SignatureElement> Vertex,
                     bool LinkPixel, unsigned RegisterCount,
                     std::vector<SignatureElement> &Result,
                     SignatureMap &Indices, std::string &Error) {
  uint32_t Next = Source.NumVectorsUsed();
  for (const auto &Element : Vertex)
    Next = std::max(Next, Element.Register + 1);
  for (const auto &Element : Source.GetElements()) {
    const auto &IndicesInSource = Element->GetSemanticIndexVec();
    const auto Kind = Element->GetKind();
    const auto System = static_cast<unsigned>(
        SemanticKindToSystemValue(Kind, DXIL::TessellatorDomain::Undefined));
    for (unsigned Row = 0; Row < Element->GetRows(); ++Row) {
      SignatureElement Out;
      Out.Semantic = Element->GetSemanticName().str();
      Out.SemanticIndex = IndicesInSource[Row];
      Out.SystemValue = System;
      Out.ComponentType = SignatureComponent(*Element);
      Out.Interpolation = static_cast<unsigned>(Element->GetInterpolationMode()->GetKind());
      if (LinkPixel && Out.SystemValue == D3D_NAME_COVERAGE)
        continue;
      const bool SpecialOut = !LinkPixel && IsSpecialPixelOutput(Out);
      if (!Out.ComponentType ||
          (!Element->IsAllocated() && !RasterizerInput(Out) && !SpecialOut)) {
        Error = "unsupported SM5 signature element " + Out.Semantic;
        return false;
      }
      if (SpecialOut) {
        Out.Register = UINT32_MAX;
        Out.Mask = 1;
        Out.UsedMask = 1;
        Indices.emplace(std::make_pair(Element->GetID(), Row), Result.size());
        Result.push_back(std::move(Out));
        continue;
      }
      Out.Register = Element->IsAllocated() ? Element->GetStartRow() + Row : Next;
      Out.Mask = Element->IsAllocated() ? Element->GetColsAsMask() : 1;
      Out.UsedMask = RasterizerInput(Out) ? Out.Mask : Element->GetUsageMask();
      if (LinkPixel) {
        if (!LinkInput(Out, Vertex, Next, Error))
          return false;
      } else {
        Out.UsedMask = (Out.UsedMask << Element->GetStartCol()) & Out.Mask;
      }
      if (Out.Register >= RegisterCount) {
        Error = "signature register exceeds SM5 limits for " + Out.Semantic +
                std::to_string(Out.SemanticIndex);
        return false;
      }
      Indices.emplace(std::make_pair(Element->GetID(), Row), Result.size());
      Result.push_back(std::move(Out));
    }
  }
  return true;
}

uint32_t ReflectionDimension(DXIL::ResourceKind Kind) {
  using K = DXIL::ResourceKind;
  switch (Kind) {
  case K::Texture1D: return D3D_SRV_DIMENSION_TEXTURE1D;
  case K::Texture1DArray: return D3D_SRV_DIMENSION_TEXTURE1DARRAY;
  case K::Texture2D: return D3D_SRV_DIMENSION_TEXTURE2D;
  case K::Texture2DArray: return D3D_SRV_DIMENSION_TEXTURE2DARRAY;
  case K::Texture2DMS: return D3D_SRV_DIMENSION_TEXTURE2DMS;
  case K::Texture2DMSArray: return D3D_SRV_DIMENSION_TEXTURE2DMSARRAY;
  case K::Texture3D: return D3D_SRV_DIMENSION_TEXTURE3D;
  case K::TextureCube: return D3D_SRV_DIMENSION_TEXTURECUBE;
  case K::TextureCubeArray: return D3D_SRV_DIMENSION_TEXTURECUBEARRAY;
  case K::TypedBuffer:
  case K::StructuredBuffer:
  case K::RawBuffer: return D3D_SRV_DIMENSION_BUFFER;
  default: return D3D_SRV_DIMENSION_UNKNOWN;
  }
}

uint32_t ReflectionScalar(const DxilFieldAnnotation &Field, Type *Ty) {
  using K = DXIL::ComponentType;
  switch (Field.GetCompType().GetKind()) {
  case K::I1: return D3D_SVT_BOOL;
  case K::I16:
  case K::I32: return D3D_SVT_INT;
  case K::U16:
  case K::U32: return D3D_SVT_UINT;
  default: return Ty->getScalarType()->isIntegerTy() ? D3D_SVT_INT : D3D_SVT_FLOAT;
  }
}

// Optimized DXIL represents a matrix as a vector or as vectors inside arrays.
// Stop unwrapping the outer array dimensions once the annotated element fits.
bool MatchesNumericShape(Type *Ty, unsigned Components, unsigned Dimensions) {
  if (auto *Array = dyn_cast<ArrayType>(Ty)) {
    const auto Count = Array->getNumElements();
    return Dimensions && Count && Components % Count == 0 &&
           MatchesNumericShape(Array->getElementType(),
                               Components / Count, Dimensions - 1);
  }
  if (auto *Vector = dyn_cast<VectorType>(Ty))
    return Dimensions && Vector->getNumElements() == Components;
  return Components == 1;
}

unsigned NumericSize(ConstantVariable &Result, Type *Ty,
                     const DxilFieldAnnotation &Field) {
  Result.Type = static_cast<uint16_t>(ReflectionScalar(Field, Ty));
  if (Field.HasMatrixAnnotation()) {
    const auto &Matrix = Field.GetMatrixAnnotation();
    const bool RowMajor = Matrix.Orientation == MatrixOrientation::RowMajor;
    Result.Class = RowMajor ? D3D_SVC_MATRIX_ROWS : D3D_SVC_MATRIX_COLUMNS;
    Result.Rows = static_cast<uint16_t>(Matrix.Rows);
    Result.Columns = static_cast<uint16_t>(Matrix.Cols);
    const auto Rows = RowMajor ? Matrix.Rows : Matrix.Cols;
    const auto Width = RowMajor ? Matrix.Cols : Matrix.Rows;
    return (Rows - 1) * 16 + Width * sizeof(uint32_t);
  }
  auto Width = Field.GetVectorSize();
  if (!Width)
    if (auto *Vector = dyn_cast<VectorType>(Ty))
      Width = Vector->getNumElements();
  if (Width) {
    Result.Class = D3D_SVC_VECTOR;
    Result.Columns = static_cast<uint16_t>(Width);
  }
  return Result.Columns * sizeof(uint32_t);
}

ConstantVariable ImportVariable(Type *Ty, const DxilFieldAnnotation &Field,
                                DxilTypeSystem &Types);

unsigned ImportMembers(ConstantVariable &Result, StructType *Struct,
                       const DxilStructAnnotation &Annotation, DxilTypeSystem &Types) {
  Result.Type = D3D_SVT_VOID;
  Result.Class = D3D_SVC_STRUCT;
  Result.Columns = 0;
  StringRef TypeName = Struct->getName();
  for (StringRef Prefix : {"dx.alignment.legacy.", "hostlayout.", "struct."})
    if (TypeName.startswith(Prefix))
      TypeName = TypeName.drop_front(Prefix.size());
  Result.TypeName = TypeName.str();
  unsigned Size = 0;
  for (unsigned I = 0; I < Struct->getNumElements(); ++I) {
    const auto &Field = Annotation.GetFieldAnnotation(I);
    if (!Field.HasCBufferOffset())
      continue;
    auto Member = ImportVariable(Struct->getElementType(I), Field, Types);
    Result.Columns += Member.Rows * Member.Columns * std::max<uint16_t>(1, Member.Elements);
    Size = std::max(Size, Member.Offset + Member.Size);
    Result.Members.push_back(std::move(Member));
  }
  return Size;
}

ConstantVariable ImportVariable(Type *Ty, const DxilFieldAnnotation &Field,
                                DxilTypeSystem &Types) {
  ConstantVariable Result;
  Result.Name = Field.GetFieldName();
  Result.Offset = Field.GetCBufferOffset();
  Result.Used = Field.IsCBVarUsed();
  unsigned Components = Field.GetVectorSize();
  unsigned Dimensions = 1;
  if (Field.HasMatrixAnnotation()) {
    const auto &Matrix = Field.GetMatrixAnnotation();
    Components = Matrix.Rows * Matrix.Cols;
    Dimensions = 2;
  }
  while (auto *Array = dyn_cast<ArrayType>(Ty)) {
    if (Components && MatchesNumericShape(Array, Components, Dimensions))
      break;
    Result.Elements = static_cast<uint16_t>(
        std::max<uint16_t>(1, Result.Elements) * Array->getNumElements());
    Ty = Array->getElementType();
  }
  auto *Struct = dyn_cast<StructType>(Ty);
  auto *StructInfo = Struct ? Types.GetStructAnnotation(Struct) : nullptr;
  const auto ElementSize = StructInfo && !Field.HasMatrixAnnotation()
                               ? ImportMembers(Result, Struct, *StructInfo, Types)
                               : NumericSize(Result, Ty, Field);
  Result.Size = Result.Elements
                    ? ((ElementSize + 15) & ~15u) * (Result.Elements - 1) + ElementSize
                    : ElementSize;
  return Result;
}

using Defaults = std::map<std::pair<StringRef, StringRef>, const Constant *>;

Defaults ReadDefaults(const Module &Module) {
  Defaults Result;
  if (const auto *Variables = Module.getNamedMetadata("dxcb5.cbuffer.defaults")) {
    for (const auto *Variable : Variables->operands()) {
      auto Buffer = cast<MDString>(Variable->getOperand(0))->getString();
      auto Name = cast<MDString>(Variable->getOperand(1))->getString();
      auto *Value = cast<ConstantAsMetadata>(Variable->getOperand(2))->getValue();
      Result.emplace(std::make_pair(Buffer, Name), Value);
    }
  }
  return Result;
}

bool WriteDefaultScalar(uint8_t *Bytes, const Constant *Value) {
  if (auto *Int = dyn_cast_or_null<ConstantInt>(Value)) {
    if (Int->getBitWidth() <= 32) {
      support::endian::write32le(Bytes, static_cast<uint32_t>(Int->getZExtValue()));
      return true;
    }
  }
  if (auto *Float = dyn_cast_or_null<ConstantFP>(Value)) {
    if (Float->getType()->isHalfTy()) {
      APFloat Val = Float->getValueAPF();
      bool LosesInfo = false;
      Val.convert(APFloat::IEEEsingle, APFloat::rmNearestTiesToEven, &LosesInfo);
      support::endian::write32le(
          Bytes, static_cast<uint32_t>(Val.bitcastToAPInt().getZExtValue()));
      return true;
    }
    const auto Bits = Float->getValueAPF().bitcastToAPInt();
    if (Bits.getBitWidth() == 32) {
      support::endian::write32le(Bytes, static_cast<uint32_t>(Bits.getZExtValue()));
      return true;
    }
  }
  return false;
}

bool WriteDefaultElement(uint8_t *Bytes, const Constant *Value,
                         const ConstantVariable &Variable) {
  if (!Value)
    return false;
  if (Variable.Class == D3D_SVC_SCALAR)
    return WriteDefaultScalar(Bytes, Value);
  if (Variable.Class != D3D_SVC_VECTOR)
    return false;
  for (unsigned Lane = 0; Lane < Variable.Columns; ++Lane)
    if (!WriteDefaultScalar(Bytes + sizeof(uint32_t) * Lane,
                            Value->getAggregateElement(Lane)))
      return false;
  return true;
}

bool ImportDefault(const Constant *Value, ConstantVariable &Variable) {
  Variable.DefaultValue.resize(Variable.Size);
  if (!Variable.Elements)
    return WriteDefaultElement(Variable.DefaultValue.data(), Value, Variable);
  for (unsigned Element = 0; Element < Variable.Elements; ++Element)
    if (!WriteDefaultElement(Variable.DefaultValue.data() + 16 * Element,
                             Value->getAggregateElement(Element), Variable))
      return false;
  return true;
}

bool ImportConstantBuffer(const DxilCBuffer &CB, DxilTypeSystem &Types,
                          const Defaults &Initializers, ConstantBuffer &Buffer,
                          std::string &Error) {
  Buffer.Name = CB.GetGlobalName();
  Buffer.Slot = CB.GetLowerBound();
  Buffer.Size = (CB.GetSize() + 15) & ~15u;
  auto *ST = dyn_cast<StructType>(CB.GetHLSLType()->getPointerElementType());
  const auto *Annotation = ST ? Types.GetStructAnnotation(ST) : nullptr;
  if (!Annotation)
    return true;
  for (unsigned I = 0; I < ST->getNumElements(); ++I) {
    auto Variable = ImportVariable(ST->getElementType(I),
                                   Annotation->GetFieldAnnotation(I), Types);
    if (ST->getNumElements() == 1)
      Variable.Used = true;
    auto Default = Initializers.find({Buffer.Name, Variable.Name});
    if (Default != Initializers.end() && !ImportDefault(Default->second, Variable)) {
      Error = "unsupported SM5 constant-buffer initializer for " + Buffer.Name +
              "." + Variable.Name;
      return false;
    }
    Buffer.Variables.push_back(std::move(Variable));
  }
  return true;
}

bool ImportBinding(const DxilResourceBase &Binding, unsigned SlotCount,
                   bool AllowArray, std::string &Error) {
  const auto First = Binding.GetLowerBound();
  const auto Count = Binding.GetRangeSize();
  const bool IsArray = Binding.GetHLSLType()->getPointerElementType()->isArrayTy();
  if (!Binding.GetSpaceID() && First < SlotCount && Count &&
      Count <= SlotCount - First && (AllowArray || (!IsArray && Count == 1)))
    return true;
  Error = "unsupported SM5 binding for " + Binding.GetGlobalName();
  if (!AllowArray && (IsArray || Count != 1))
    Error += ": constant-buffer resource arrays are unsupported";
  else
    Error += ": expected a finite binding range within the SM5 slots in space 0";
  return false;
}

void ImportShaderResource(const DxilResourceBase &Resource, BoundResource Binding,
                          MirProgram &Program, Interface &Layout) {
  // Only statically referenced elements of SRV/sampler arrays become bindings.
  if (Resource.GetHLSLType()->getPointerElementType()->isArrayTy())
    Layout.ResourceArrays.emplace(&Resource, std::move(Binding));
  else
    Program.Resources.push_back(std::move(Binding));
}

uint32_t ResourceReturnType(DXIL::ComponentType Component) {
  if (Component == DXIL::ComponentType::U32 ||
      Component == DXIL::ComponentType::U16)
    return D3D_RETURN_TYPE_UINT;
  if (Component == DXIL::ComponentType::I32 ||
      Component == DXIL::ComponentType::I16)
    return D3D_RETURN_TYPE_SINT;
  return D3D_RETURN_TYPE_FLOAT;
}

bool ImportUAVs(DxilModule &Module, MirProgram &Program, Interface &Layout,
                std::string &Error) {
  for (const auto &Uav : Module.GetUAVs()) {
    if (!ImportBinding(*Uav, D3D11_1_UAV_SLOT_COUNT, true, Error))
      return false;
    BoundResource Binding;
    Binding.Name = Uav->GetGlobalName();
    Binding.Slot = Uav->GetLowerBound();
    const auto ResKind = Uav->GetKind();
    Binding.Dimension = ReflectionDimension(ResKind);
    if (Binding.Dimension == D3D_SRV_DIMENSION_UNKNOWN) {
      Error = "unsupported SM5 UAV dimension for " + Binding.Name;
      return false;
    }
    if (Uav->IsGloballyCoherent())
      Binding.UavFlags |= D3D11_SB_GLOBALLY_COHERENT_ACCESS;
    if (Uav->HasCounter())
      Binding.UavFlags |= D3D11_SB_UAV_HAS_ORDER_PRESERVING_COUNTER;
    if (ResKind == DXIL::ResourceKind::StructuredBuffer) {
      Binding.Type = Uav->HasCounter() ? D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER
                                       : D3D_SIT_UAV_RWSTRUCTURED;
      Binding.ReturnType = D3D_RETURN_TYPE_MIXED;
      Binding.Components = 1;
      Binding.Stride = Uav->GetElementStride();
    } else if (ResKind == DXIL::ResourceKind::RawBuffer) {
      Binding.Type = D3D_SIT_UAV_RWBYTEADDRESS;
      Binding.ReturnType = D3D_RETURN_TYPE_MIXED;
      Binding.Components = 1;
    } else {
      Binding.Type = D3D_SIT_UAV_RWTYPED;
      Binding.ReturnType = ResourceReturnType(Uav->GetCompType().GetKind());
      const auto *Return = dyn_cast<VectorType>(Uav->GetRetType());
      Binding.Components = Return ? Return->getNumElements() : 1;
    }
    ImportShaderResource(*Uav, std::move(Binding), Program, Layout);
  }
  return true;
}

bool ImportResources(DxilModule &Module, MirProgram &Program, Interface &Layout,
                     std::string &Error) {
  if (!ImportUAVs(Module, Program, Layout, Error))
    return false;
  for (const auto &Texture : Module.GetSRVs()) {
    if (!ImportBinding(*Texture, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,
                       true, Error))
      return false;
    BoundResource Binding;
    Binding.Name = Texture->GetGlobalName();
    Binding.Slot = Texture->GetLowerBound();
    const auto ResKind = Texture->GetKind();
    Binding.Dimension = ReflectionDimension(ResKind);
    if (Binding.Dimension == D3D_SRV_DIMENSION_UNKNOWN) {
      Error = "unsupported SM5 texture dimension for " + Binding.Name;
      return false;
    }
    if (ResKind == DXIL::ResourceKind::StructuredBuffer) {
      Binding.Type = D3D_SIT_STRUCTURED;
      Binding.ReturnType = D3D_RETURN_TYPE_MIXED;
      Binding.Components = 1;
      Binding.Stride = Texture->GetElementStride();
    } else if (ResKind == DXIL::ResourceKind::RawBuffer) {
      Binding.Type = D3D_SIT_BYTEADDRESS;
      Binding.ReturnType = D3D_RETURN_TYPE_MIXED;
      Binding.Components = 1;
    } else {
      Binding.ReturnType = ResourceReturnType(Texture->GetCompType().GetKind());
      const auto *Return = dyn_cast<VectorType>(Texture->GetRetType());
      Binding.Components = Return ? Return->getNumElements() : 1;
      Binding.SampleCount = Texture->GetSampleCount();
    }
    ImportShaderResource(*Texture, std::move(Binding), Program, Layout);
  }
  for (const auto &Sampler : Module.GetSamplers()) {
    if (!ImportBinding(*Sampler, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT,
                       true, Error))
      return false;
    BoundResource Binding;
    Binding.Name = Sampler->GetGlobalName();
    Binding.Slot = Sampler->GetLowerBound();
    Binding.Type = D3D_SIT_SAMPLER;
    Binding.Dimension = D3D_SRV_DIMENSION_UNKNOWN;
    Binding.ReturnType = 0;
    Binding.Comparison = Sampler->GetSamplerKind() == DXIL::SamplerKind::Comparison;
    ImportShaderResource(*Sampler, std::move(Binding), Program, Layout);
  }
  const auto Initializers = ReadDefaults(*Module.GetModule());
  for (const auto &CB : Module.GetCBuffers()) {
    ConstantBuffer Buffer;
    constexpr auto MaxBufferSize = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16;
    if (!ImportBinding(*CB, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT,
                       false, Error))
      return false;
    if (!CB->GetSize() || CB->GetSize() > MaxBufferSize) {
      Error = "constant buffer exceeds SM5 limits: " + CB->GetGlobalName();
      return false;
    }
    if (!ImportConstantBuffer(*CB, Module.GetTypeSystem(), Initializers, Buffer,
                              Error))
      return false;
    Program.ConstantBuffers.push_back(std::move(Buffer));
  }
  return true;
}
} // namespace

bool dxcb5::ImportInterface(DxilModule &Module, ArrayRef<SignatureElement> Vertex,
                            MirProgram &Program, Interface &Layout,
                            std::string &Error) {
  const bool Pixel = Module.GetShaderModel()->IsPS();
  Program.ShaderStage = Pixel ? Stage::Pixel : Stage::Vertex;
  const auto InputRegisters = Pixel ? D3D11_PS_INPUT_REGISTER_COUNT
                                    : D3D11_VS_INPUT_REGISTER_COUNT;
  const auto OutputRegisters = Pixel ? D3D11_PS_OUTPUT_REGISTER_COUNT
                                     : D3D11_VS_OUTPUT_REGISTER_COUNT;
  return ImportSignature(Module.GetInputSignature(), Vertex, Pixel,
                         InputRegisters,
                         Program.Inputs, Layout.Input, Error) &&
         ImportSignature(Module.GetOutputSignature(), {}, false,
                         OutputRegisters,
                         Program.Outputs, Layout.Output, Error) &&
         ImportResources(Module, Program, Layout, Error);
}
