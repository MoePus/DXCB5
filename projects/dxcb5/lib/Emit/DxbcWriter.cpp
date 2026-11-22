// Distributed under the University of Illinois Open Source License.
#include "dxcb5/DxbcWriter.h"
#include "DxbcParts.h"
#include "Support/DXIncludes.h"
#include "dxcb5/InstructionInfo.h"
#include "dxc/DxilContainer/DxilContainerAssembler.h"
#include "dxc/DxilHash/DxilHash.h"
#include "dxc/Support/FileIOHelper.h"
#include "dxc/Support/Global.h"
#include "llvm/ADT/ArrayRef.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>

using namespace D3D10ShaderBinary;
using namespace hlsl;

namespace dxcb5 {
namespace {
constexpr auto InputSignature = FourCC('I', 'S', 'G', 'N');
constexpr auto OutputSignature = FourCC('O', 'S', 'G', 'N');
constexpr auto ShaderCode = FourCC('S', 'H', 'E', 'X');
constexpr auto ShaderStats = FourCC('S', 'T', 'A', 'T');
constexpr auto RegisterMaskShift = 4;

D3D10_SB_OPERAND_TYPE RegisterType(OperandKind kind) {
  switch (kind) {
  case OperandKind::Temp: return D3D10_SB_OPERAND_TYPE_TEMP;
  case OperandKind::Input: return D3D10_SB_OPERAND_TYPE_INPUT;
  case OperandKind::Output: return D3D10_SB_OPERAND_TYPE_OUTPUT;
  case OperandKind::Resource: return D3D10_SB_OPERAND_TYPE_RESOURCE;
  case OperandKind::Sampler: return D3D10_SB_OPERAND_TYPE_SAMPLER;
  case OperandKind::ConstantBuffer: return D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER;
  case OperandKind::ImmediateConstantBuffer:
    return D3D10_SB_OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER;
  case OperandKind::ImmediateVector:
  case OperandKind::Immediate: return D3D10_SB_OPERAND_TYPE_IMMEDIATE32;
  case OperandKind::IndexableTemp: return D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP;
  case OperandKind::OutputDepth: return D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH;
  case OperandKind::Uav: return D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW;
  case OperandKind::InputCoverage:
    return D3D11_SB_OPERAND_TYPE_INPUT_COVERAGE_MASK;
  case OperandKind::OutputMask:
    return D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK;
  case OperandKind::Null: return D3D10_SB_OPERAND_TYPE_NULL;
  }
  return D3D10_SB_OPERAND_TYPE_NULL;
}

bool IsTestOpcode(uint32_t opcode) {
  switch (opcode) {
  case D3D10_SB_OPCODE_IF:
  case D3D10_SB_OPCODE_BREAKC:
  case D3D10_SB_OPCODE_CONTINUEC:
  case D3D10_SB_OPCODE_DISCARD:
  case D3D10_SB_OPCODE_RETC: return true;
  default: return false;
  }
}

uint32_t ApplyLiteralModifier(uint32_t bits, uint8_t modifier, bool intNeg) {
  if (modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
    return intNeg ? (0u - bits) : (bits ^ 0x80000000u);
  if (modifier == D3D10_SB_OPERAND_MODIFIER_ABS)
    return bits & 0x7fffffffu;
  if (modifier == D3D10_SB_OPERAND_MODIFIER_ABSNEG)
    return bits | 0x80000000u;
  return bits;
}

COperandBase EncodeSource(const MirProgram &program, const Operand &source,
                          bool scalar, bool intNeg = false,
                          bool vec4Imm = false) {
  if (source.Kind == OperandKind::ImmediateVector) {
    assert(source.Index < program.LiteralVectors.size());
    const auto &bits = program.LiteralVectors[source.Index];
    if (scalar)
      return COperand(ApplyLiteralModifier(bits[source.Swizzle & 3u],
                                           source.Modifier, intNeg));
    COperand operand(0, 0, 0, 0);
    for (unsigned lane = 0; lane < 4; ++lane)
      operand.m_Value[lane] = ApplyLiteralModifier(
          bits[(source.Swizzle >> (lane * 2)) & 3u], source.Modifier, intNeg);
    return operand;
  }
  const auto type = RegisterType(source.Kind);
  if (source.Kind == OperandKind::Immediate) {
    const uint32_t v = ApplyLiteralModifier(source.Index, source.Modifier, intNeg);
    if (!scalar && vec4Imm) {
      COperand operand(0, 0, 0, 0);
      for (unsigned lane = 0; lane < 4; ++lane)
        operand.m_Value[lane] = v;
      return operand;
    }
    return COperand(v);
  }
  if (source.Kind == OperandKind::InputCoverage)
    return COperand(D3D11_SB_OPERAND_TYPE_INPUT_COVERAGE_MASK);
  if (source.Kind == OperandKind::Sampler && !scalar)
    return COperand(type, source.Index);
  COperandBase operand = COperand4(type, source.Index);
  const bool twoDim = source.Kind == OperandKind::ConstantBuffer ||
                      source.Kind == OperandKind::IndexableTemp;
  if (twoDim)
    operand = COperand2D(type, source.Index, source.Element);
  if (source.RelativeRegister != UINT32_MAX)
    operand.SetIndex(twoDim ? 1 : 0,
                     twoDim ? source.Element : source.Index,
                     D3D10_SB_OPERAND_TYPE_TEMP, source.RelativeRegister, 0,
                     static_cast<D3D10_SB_4_COMPONENT_NAME>(source.RelativeLane));
  const auto swizzle = source.Swizzle;
  if (scalar)
    operand.SelectComponent(
        static_cast<D3D10_SB_4_COMPONENT_NAME>(swizzle & 3u));
  else
    operand.SetSwizzle(swizzle & 3u, (swizzle >> 2) & 3u,
                       (swizzle >> 4) & 3u, (swizzle >> 6) & 3u);
  operand.SetModifier(static_cast<D3D10_SB_OPERAND_MODIFIER>(source.Modifier));
  return operand;
}

COperandBase EncodeDestination(const Operand &destination) {
  const auto type = RegisterType(destination.Kind);
  if (destination.Kind == OperandKind::Null)
    return COperand(type);
  if (destination.Kind == OperandKind::OutputDepth) {
    if (destination.Index == 1)
      return COperandDst(D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL);
    if (destination.Index == 2)
      return COperandDst(D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL);
    return COperandDst(D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH);
  }
  if (destination.Kind == OperandKind::OutputMask)
    return COperandDst(D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK);
  if (destination.Kind == OperandKind::Uav && destination.Mask == 0)
    return COperand(type, destination.Index);
  if (destination.Kind == OperandKind::IndexableTemp) {
    COperandBase operand =
        COperand2D(type, destination.Index, destination.Element);
    if (destination.RelativeRegister != UINT32_MAX)
      operand.SetIndex(
          1, destination.Element, D3D10_SB_OPERAND_TYPE_TEMP,
          destination.RelativeRegister, 0,
          static_cast<D3D10_SB_4_COMPONENT_NAME>(destination.RelativeLane));
    operand.SetMask(static_cast<UINT>(destination.Mask)
                    << RegisterMaskShift);
    return operand;
  }
  return COperandDst(type, destination.Index,
                      static_cast<UINT>(destination.Mask) << RegisterMaskShift);
}

void EncodeInstruction(CShaderAsm &assembler, const MirProgram &program,
                       const Instruction &instruction) {
  assert(instruction.Opcode < D3D10_SB_NUM_OPCODES);
  auto opcode = static_cast<D3D10_SB_OPCODE_TYPE>(instruction.Opcode);
  assert(instruction.Operands.size() == GetNumInstructionOperands(opcode));
  CInstruction encoded(opcode);
  encoded.SetNumOperands(static_cast<UINT>(instruction.Operands.size()));
  encoded.m_bSaturate = instruction.Saturate;
  if (opcode == D3D10_SB_OPCODE_RESINFO)
    encoded.m_ResInfoReturnType = D3D10_SB_RESINFO_INSTRUCTION_RETURN_UINT;
  if (opcode == D3D10_1_SB_OPCODE_SAMPLE_INFO)
    encoded.m_InstructionReturnType = D3D10_SB_INSTRUCTION_RETURN_UINT;
  const bool test = IsTestOpcode(instruction.Opcode);
  if (instruction.TexelOffset[0] || instruction.TexelOffset[1] ||
      instruction.TexelOffset[2])
    encoded.SetTexelOffset(instruction.TexelOffset);
  if (test)
    encoded.SetTest(instruction.TestNonZero
                        ? D3D10_SB_INSTRUCTION_TEST_NONZERO
                        : D3D10_SB_INSTRUCTION_TEST_ZERO);
  const bool intNeg = InstructionAcceptsIntNegModifier(instruction.Opcode);
  const bool isAtomic =
      opcode == D3D11_SB_OPCODE_ATOMIC_AND ||
      opcode == D3D11_SB_OPCODE_ATOMIC_OR ||
      opcode == D3D11_SB_OPCODE_ATOMIC_XOR ||
      opcode == D3D11_SB_OPCODE_ATOMIC_CMP_STORE ||
      opcode == D3D11_SB_OPCODE_ATOMIC_IADD ||
      opcode == D3D11_SB_OPCODE_ATOMIC_IMAX ||
      opcode == D3D11_SB_OPCODE_ATOMIC_IMIN ||
      opcode == D3D11_SB_OPCODE_ATOMIC_UMAX ||
      opcode == D3D11_SB_OPCODE_ATOMIC_UMIN;
  const bool isImmAtomic =
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_IADD ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_AND ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_OR ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_XOR ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_EXCH ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_IMAX ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_IMIN ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_UMAX ||
      opcode == D3D11_SB_OPCODE_IMM_ATOMIC_UMIN;
  for (unsigned i = 0; i < instruction.Operands.size(); ++i) {
    const auto &operand = instruction.Operands[i];
    const bool gatherChannel =
        (opcode == D3D10_1_SB_OPCODE_GATHER4 ||
         opcode == D3D11_SB_OPCODE_GATHER4_C ||
         opcode == D3D11_SB_OPCODE_GATHER4_PO ||
         opcode == D3D11_SB_OPCODE_GATHER4_PO_C) &&
        operand.Kind == OperandKind::Sampler;
    const bool scalarAddr =
        (opcode == D3D10_SB_OPCODE_LD_MS && i == 3) ||
        (opcode == D3D11_SB_OPCODE_GATHER4_C && i == 4) ||
        (opcode == D3D11_SB_OPCODE_GATHER4_PO_C && i == 5) ||
        (opcode == D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX && i == 2) ||
        (opcode == D3D11_SB_OPCODE_LD_RAW && i == 1) ||
        (opcode == D3D11_SB_OPCODE_LD_STRUCTURED && (i == 1 || i == 2)) ||
        (opcode == D3D11_SB_OPCODE_STORE_RAW && i == 1) ||
        (opcode == D3D11_SB_OPCODE_STORE_STRUCTURED && (i == 1 || i == 2)) ||
        (isAtomic && i >= 2) || (isImmAtomic && i >= 3);
    const bool vec4Imm =
        opcode == D3D11_SB_OPCODE_LD_UAV_TYPED ||
        opcode == D3D11_SB_OPCODE_STORE_UAV_TYPED ||
        opcode == D3D11_SB_OPCODE_STORE_RAW ||
        opcode == D3D11_SB_OPCODE_STORE_STRUCTURED ||
        (isAtomic && i == 1) || (isImmAtomic && i == 2);
    encoded.m_Operands[i] =
        i < InstructionDestinationCount(instruction.Opcode)
            ? EncodeDestination(operand)
            : EncodeSource(program, operand,
                           test || gatherChannel || scalarAddr ||
                               opcode == D3D10_1_SB_OPCODE_SAMPLE_INFO ||
                               opcode == D3D10_SB_OPCODE_SWITCH ||
                               opcode == D3D10_SB_OPCODE_CASE,
                           intNeg, vec4Imm);
  }
  assembler.EmitInstruction(encoded);
}

D3D10_SB_RESOURCE_DIMENSION ResourceDimension(uint32_t dimension) {
  switch (dimension) {
  case D3D_SRV_DIMENSION_BUFFER: return D3D10_SB_RESOURCE_DIMENSION_BUFFER;
  case D3D_SRV_DIMENSION_TEXTURE1D: return D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D;
  case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
    return D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY;
  case D3D_SRV_DIMENSION_TEXTURE2D: return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D;
  case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
    return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY;
  case D3D_SRV_DIMENSION_TEXTURE2DMS:
    return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS;
  case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
    return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY;
  case D3D_SRV_DIMENSION_TEXTURE3D: return D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D;
  case D3D_SRV_DIMENSION_TEXTURECUBE:
    return D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE;
  case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
    return D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY;
  default: return D3D10_SB_RESOURCE_DIMENSION_UNKNOWN;
  }
}

void EmitResource(CShaderAsm &assembler, const BoundResource &resource) {
  assert(resource.Count == 1);
  if (resource.Type == D3D_SIT_SAMPLER) {
    assert(resource.Slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    assembler.EmitSamplerDecl(resource.Slot, resource.Comparison
                                  ? D3D10_SB_SAMPLER_MODE_COMPARISON
                                  : D3D10_SB_SAMPLER_MODE_DEFAULT);
    return;
  }
  if (resource.Type == D3D_SIT_STRUCTURED) {
    assert(resource.Slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT &&
           resource.Stride > 0);
    assembler.EmitStructuredShaderResourceViewDecl(resource.Slot,
                                                   resource.Stride);
    return;
  }
  if (resource.Type == D3D_SIT_BYTEADDRESS) {
    assert(resource.Slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
    assembler.EmitRawShaderResourceViewDecl(resource.Slot);
    return;
  }
  if (resource.Type == D3D_SIT_UAV_RWSTRUCTURED ||
      resource.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER) {
    assert(resource.Slot < D3D11_1_UAV_SLOT_COUNT && resource.Stride > 0);
    assembler.EmitStructuredUnorderedAccessViewDecl(
        resource.Slot, resource.Stride, resource.UavFlags);
    return;
  }
  if (resource.Type == D3D_SIT_UAV_RWBYTEADDRESS) {
    assert(resource.Slot < D3D11_1_UAV_SLOT_COUNT);
    assembler.EmitRawUnorderedAccessViewDecl(resource.Slot, resource.UavFlags);
    return;
  }
  const auto dimension = ResourceDimension(resource.Dimension);
  if (resource.Type == D3D_SIT_UAV_RWTYPED) {
    assert(dimension != D3D10_SB_RESOURCE_DIMENSION_UNKNOWN &&
           resource.Slot < D3D11_1_UAV_SLOT_COUNT &&
           resource.ReturnType >= D3D10_SB_RETURN_TYPE_UNORM &&
           resource.ReturnType <= D3D10_SB_RETURN_TYPE_FLOAT);
    const auto type =
        static_cast<D3D10_SB_RESOURCE_RETURN_TYPE>(resource.ReturnType);
    assembler.EmitTypedUnorderedAccessViewDecl(dimension, resource.Slot, type,
                                               type, type, type,
                                               resource.UavFlags);
    return;
  }
  assert(resource.Type == D3D_SIT_TEXTURE &&
         dimension != D3D10_SB_RESOURCE_DIMENSION_UNKNOWN &&
         resource.Slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT &&
         resource.ReturnType >= D3D10_SB_RETURN_TYPE_UNORM &&
         resource.ReturnType <= D3D10_SB_RETURN_TYPE_FLOAT);
  const auto type = static_cast<D3D10_SB_RESOURCE_RETURN_TYPE>(resource.ReturnType);
  if (dimension == D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS ||
      dimension == D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY) {
    assembler.EmitResourceMSDecl(dimension, resource.Slot, type, type, type,
                                 type, resource.SampleCount);
    return;
  }
  assembler.EmitResourceDecl(dimension, resource.Slot, type, type, type, type);
}

bool IsRasterizerArrayIndex(uint32_t semantic) {
  return semantic == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX ||
         semantic == D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX;
}

void EmitInput(CShaderAsm &assembler, const SignatureElement &element,
               Stage stage) {
  const auto mask = static_cast<UINT>(element.Mask) << RegisterMaskShift;
  const auto semantic = static_cast<D3D10_SB_NAME>(element.SystemValue);
  if (stage == Stage::Vertex) {
    if (semantic == D3D10_SB_NAME_VERTEX_ID ||
        semantic == D3D10_SB_NAME_INSTANCE_ID)
      assembler.EmitInputSystemGeneratedValueDecl(element.Register, mask,
                                                  semantic);
    else
      assembler.EmitInputDecl(D3D10_SB_OPERAND_TYPE_INPUT, element.Register,
                              mask);
    return;
  }
  auto mode = static_cast<D3D10_SB_INTERPOLATION_MODE>(element.Interpolation);
  if (!element.Interpolation)
    mode = element.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32
               ? D3D10_SB_INTERPOLATION_LINEAR
               : D3D10_SB_INTERPOLATION_CONSTANT;
  if (IsRasterizerArrayIndex(semantic))
    mode = D3D10_SB_INTERPOLATION_CONSTANT;
  if (semantic == D3D10_SB_NAME_POSITION || IsRasterizerArrayIndex(semantic))
    assembler.EmitPSInputSystemInterpretedValueDecl(element.Register, mask,
                                                   mode, semantic);
  else if (semantic == D3D10_SB_NAME_IS_FRONT_FACE ||
           semantic == D3D10_SB_NAME_SAMPLE_INDEX ||
           semantic == D3D10_SB_NAME_PRIMITIVE_ID)
    assembler.EmitPSInputSystemGeneratedValueDecl(element.Register, mask,
                                                 mode, semantic);
  else
    assembler.EmitPSInputDecl(element.Register, mask, mode);
}

void EmitOutput(CShaderAsm &assembler, const SignatureElement &element,
                Stage stage) {
  if (stage == Stage::Pixel) {
    if (element.SystemValue == D3D_NAME_DEPTH) {
      assembler.EmitODepthDecl();
      return;
    }
    if (element.SystemValue == D3D_NAME_DEPTH_GREATER_EQUAL) {
      assembler.EmitODepthDeclGE();
      return;
    }
    if (element.SystemValue == D3D_NAME_DEPTH_LESS_EQUAL) {
      assembler.EmitODepthDeclLE();
      return;
    }
    if (element.SystemValue == D3D_NAME_COVERAGE) {
      assembler.EmitOMaskDecl();
      return;
    }
  }
  auto mask = static_cast<UINT>(element.Mask) << RegisterMaskShift;
  const auto semantic = static_cast<D3D10_SB_NAME>(element.SystemValue);
  if (stage == Stage::Vertex && semantic != D3D10_SB_NAME_UNDEFINED)
    assembler.EmitOutputSystemInterpretedValueDecl(element.Register, mask,
                                                   semantic);
  else
    assembler.EmitOutputDecl(element.Register, mask);
}

bool UsesInputCoverage(const MirProgram &program) {
  for (const auto &inst : program.Instructions)
    for (const auto &op : inst.Operands)
      if (op.Kind == OperandKind::InputCoverage)
        return true;
  return false;
}

void EmitDeclarations(CShaderAsm &assembler, const MirProgram &program) {
  for (const auto &buffer : program.ConstantBuffers) {
    assert(buffer.Slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT &&
           buffer.Size && buffer.Size <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16);
    assembler.EmitConstantBufferDecl(
        buffer.Slot, (buffer.Size + 15) / 16,
        buffer.DynamicIndex ? D3D10_SB_CONSTANT_BUFFER_DYNAMIC_INDEXED
                            : D3D10_SB_CONSTANT_BUFFER_IMMEDIATE_INDEXED);
  }
  for (const auto &resource : program.Resources)
    EmitResource(assembler, resource);
  for (const auto &element : program.Inputs)
    EmitInput(assembler, element, program.ShaderStage);
  if (UsesInputCoverage(program))
    assembler.EmitInputCoverageMaskDecl();
  for (const auto &element : program.Outputs)
    EmitOutput(assembler, element, program.ShaderStage);
  if (!program.ImmediateConstants.empty()) {
    assert(program.ImmediateConstants.size() % 4 == 0);
    assembler.EmitImmediateConstantBufferDecl(
        static_cast<UINT>(program.ImmediateConstants.size() / 4),
        program.ImmediateConstants.data());
  }
  if (program.TempCount)
    assembler.EmitTempsDecl(program.TempCount);
  for (const auto &it : program.IndexableTemps)
    assembler.EmitIndexableTempDecl(it.Index, it.Elements, it.Components);
}

void AddPart(DxilContainerWriter &container, uint32_t fourcc,
             llvm::ArrayRef<uint8_t> bytes) {
  container.AddPart(fourcc, static_cast<uint32_t>(bytes.size()),
                    [bytes](AbstractMemoryStream *stream) {
                      ULONG written = 0;
                      IFT(stream->Write(bytes.data(),
                                        static_cast<ULONG>(bytes.size()),
                                        &written));
                    });
}

struct ReleaseStream {
  void operator()(AbstractMemoryStream *stream) const { stream->Release(); }
};

uint64_t ShaderFeatures(const MirProgram &program) {
  uint64_t features = 0;
  if (program.ShaderStage == Stage::Vertex) {
    for (const auto &output : program.Outputs)
      if (IsRasterizerArrayIndex(output.SystemValue))
        features |=
            DXIL::ShaderFeatureInfo_ViewportAndRTArrayIndexFromAnyShaderFeedingRasterizer;
  }
  bool hasUav = false;
  bool hasHighUav = false;
  for (const auto &res : program.Resources) {
    if (res.Type == D3D_SIT_UAV_RWTYPED ||
        res.Type == D3D_SIT_UAV_RWSTRUCTURED ||
        res.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER ||
        res.Type == D3D_SIT_UAV_RWBYTEADDRESS) {
      hasUav = true;
      if (res.Slot >= D3D11_PS_CS_UAV_REGISTER_COUNT)
        hasHighUav = true;
    }
  }
  if (hasUav && program.ShaderStage == Stage::Vertex)
    features |= DXIL::ShaderFeatureInfo_UAVsAtEveryStage;
  if (hasHighUav)
    features |= DXIL::ShaderFeatureInfo_64UAVs;
  for (const auto &inst : program.Instructions) {
    if (inst.Opcode == D3D11_SB_OPCODE_LD_UAV_TYPED) {
      features |= DXIL::ShaderFeatureInfo_TypedUAVLoadAdditionalFormats;
      break;
    }
  }
  return features;
}

bool WriteContainer(const MirProgram &program, CShaderAsm &assembler,
                    std::vector<uint8_t> &output, std::string &error) {
  auto inputs = EncodeSignature(program.Inputs, false);
  auto outputs = EncodeSignature(program.Outputs, true);
  auto reflection = EncodeResourceDefinitions(program);
  auto stats = EncodeStatistics(program);
  std::unique_ptr<DxilContainerWriter> container(NewDxilContainerWriter());
  AddPart(*container, DFCC_ResourceDef, reflection);
  AddPart(*container, InputSignature, inputs);
  AddPart(*container, OutputSignature, outputs);
  AddPart(*container, ShaderCode,
          {reinterpret_cast<const uint8_t *>(assembler.GetShader()),
           assembler.ShaderSizeInBytes()});
  AddPart(*container, ShaderStats, stats);

  const uint64_t features = ShaderFeatures(program);
  if (features)
    AddPart(*container, DFCC_FeatureInfo,
            {reinterpret_cast<const uint8_t *>(&features), sizeof(features)});

  DxcThreadMalloc allocator(nullptr);
  AbstractMemoryStream *storage = nullptr;
  std::vector<uint8_t> bytes(container->size());
  if (FAILED(CreateFixedSizeMemoryStream(bytes.data(), bytes.size(), &storage))) {
    error = "cannot create the DXBC container stream";
    return false;
  }
  std::unique_ptr<AbstractMemoryStream, ReleaseStream> stream(storage);
  container->write(stream.get());
  constexpr size_t HashStart = offsetof(DxilContainerHeader, Version);
  constexpr size_t HashOffset = offsetof(DxilContainerHeader, Hash);
  ComputeHashRetail(bytes.data() + HashStart,
                     static_cast<UINT32>(bytes.size() - HashStart),
                     bytes.data() + HashOffset);
  output = std::move(bytes);
  return true;
}

bool ValidateResource(const BoundResource &r, std::string &error) {
  if (r.Count != 1)
    return error = "SM5 resource array count must be 1", false;
  if (r.Type == D3D_SIT_SAMPLER)
    return r.Slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
               ? true
               : (error = "SM5 sampler slot exceeds limit", false);
  if (r.Type == D3D_SIT_STRUCTURED)
    return (r.Slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT && r.Stride > 0)
               ? true
               : (error = "invalid SM5 structured SRV slot or stride", false);
  if (r.Type == D3D_SIT_BYTEADDRESS)
    return r.Slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
               ? true
               : (error = "SM5 raw SRV slot exceeds limit", false);
  if (r.Type == D3D_SIT_UAV_RWSTRUCTURED ||
      r.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER)
    return (r.Slot < D3D11_1_UAV_SLOT_COUNT && r.Stride > 0)
               ? true
               : (error = "invalid SM5 structured UAV slot or stride", false);
  if (r.Type == D3D_SIT_UAV_RWBYTEADDRESS)
    return r.Slot < D3D11_1_UAV_SLOT_COUNT
               ? true
               : (error = "SM5 raw UAV slot exceeds limit", false);
  const auto dim = ResourceDimension(r.Dimension);
  const bool okRet = r.ReturnType >= D3D10_SB_RETURN_TYPE_UNORM &&
                     r.ReturnType <= D3D10_SB_RETURN_TYPE_FLOAT;
  if (r.Type == D3D_SIT_UAV_RWTYPED)
    return (dim != D3D10_SB_RESOURCE_DIMENSION_UNKNOWN &&
            r.Slot < D3D11_1_UAV_SLOT_COUNT && okRet)
               ? true
               : (error = "invalid SM5 typed UAV declaration", false);
  if (r.Type != D3D_SIT_TEXTURE || dim == D3D10_SB_RESOURCE_DIMENSION_UNKNOWN ||
      r.Slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || !okRet)
    return error = "invalid SM5 texture SRV declaration", false;
  return true;
}

bool ValidateMirProgram(const MirProgram &program, std::string &error) {
  if (program.TempCount > D3D11_COMMONSHADER_TEMP_REGISTER_COUNT)
    return error = "SM5 temp register count exceeds 4096", false;
  uint64_t totalIdx = 0;
  for (const auto &it : program.IndexableTemps)
    totalIdx += it.Elements;
  if (totalIdx > D3D11_COMMONSHADER_TEMP_REGISTER_COUNT)
    return error = "SM5 total indexable temp elements exceed 4096", false;
  if (program.ImmediateConstants.size() % 4 != 0)
    return error = "SM5 immediate constant buffer is not 4-word aligned", false;
  for (const auto &cb : program.ConstantBuffers)
    if (cb.Slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
        !cb.Size || cb.Size > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16)
      return error = "SM5 constant buffer slot or size exceeds limit", false;
  for (const auto &r : program.Resources)
    if (!ValidateResource(r, error))
      return false;
  struct CfFrame {
    uint32_t OpenOp;
    bool SeenElse;
  };
  std::vector<CfFrame> CfStack;
  for (const auto &inst : program.Instructions) {
    if (inst.Opcode >= D3D10_SB_NUM_OPCODES ||
        inst.Operands.size() !=
            GetNumInstructionOperands(static_cast<D3D10_SB_OPCODE_TYPE>(inst.Opcode)))
      return error = "invalid SM5 MIR instruction opcode or operand count", false;
    switch (inst.Opcode) {
    case D3D10_SB_OPCODE_IF:
    case D3D10_SB_OPCODE_LOOP:
    case D3D10_SB_OPCODE_SWITCH:
      CfStack.push_back({inst.Opcode, false});
      break;
    case D3D10_SB_OPCODE_ELSE:
      if (CfStack.empty() || CfStack.back().OpenOp != D3D10_SB_OPCODE_IF ||
          CfStack.back().SeenElse)
        return error = "unbalanced SM5 ELSE instruction", false;
      CfStack.back().SeenElse = true;
      break;
    case D3D10_SB_OPCODE_ENDIF:
      if (CfStack.empty() || CfStack.back().OpenOp != D3D10_SB_OPCODE_IF)
        return error = "unbalanced SM5 ENDIF instruction", false;
      CfStack.pop_back();
      break;
    case D3D10_SB_OPCODE_ENDLOOP:
      if (CfStack.empty() || CfStack.back().OpenOp != D3D10_SB_OPCODE_LOOP)
        return error = "unbalanced SM5 ENDLOOP instruction", false;
      CfStack.pop_back();
      break;
    case D3D10_SB_OPCODE_ENDSWITCH:
      if (CfStack.empty() || CfStack.back().OpenOp != D3D10_SB_OPCODE_SWITCH)
        return error = "unbalanced SM5 ENDSWITCH instruction", false;
      CfStack.pop_back();
      break;
    default:
      break;
    }
    for (const auto &op : inst.Operands) {
      if (op.Kind == OperandKind::ImmediateVector &&
          op.Index >= program.LiteralVectors.size())
        return error = "SM5 literal vector index out of bounds", false;
      if (op.Kind == OperandKind::IndexableTemp &&
          (op.Index >= program.IndexableTemps.size() ||
           (op.RelativeRegister == UINT32_MAX &&
            op.Element >= program.IndexableTemps[op.Index].Elements)))
        return error = "SM5 indexable temp index or element out of bounds", false;
    }
  }
  if (!CfStack.empty())
    return error = "unterminated SM5 structured control-flow block", false;
  return true;
}

} // namespace

bool WriteDxbc(const MirProgram &program, std::vector<uint8_t> &output,
               std::string &error) {
  static const bool initialized = (InitInstructionInfo(), true);
  (void)initialized;
  if (!ValidateMirProgram(program, error))
    return false;
  CShaderAsm assembler;
  assembler.StartShader(program.ShaderStage == Stage::Vertex
                            ? D3D10_SB_VERTEX_SHADER : D3D10_SB_PIXEL_SHADER,
                        5, 0);
  EmitDeclarations(assembler, program);
  for (const auto &instruction : program.Instructions)
    EncodeInstruction(assembler, program, instruction);
  assembler.EndShader();
  return WriteContainer(program, assembler, output, error);
}

} // namespace dxcb5
