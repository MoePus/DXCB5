// Distributed under the University of Illinois Open Source License.
#include "DxbcParts.h"
#include "dxcb5/DxbcWriter.h"
#include "Support/DXIncludes.h"
#include "dxc/DxilContainer/DxilContainer.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <tuple>

using namespace hlsl;

namespace dxcb5 {
namespace {
constexpr auto LegacyOutputSignature = FourCC('O', 'S', 'G', 'N');
constexpr auto StreamOutputSignature = FourCC('O', 'S', 'G', '5');
constexpr auto ExtendedOutputSignature = FourCC('O', 'S', 'G', '1');
constexpr auto ShaderCode = FourCC('S', 'H', 'E', 'X');
constexpr auto LegacyShaderCode = FourCC('S', 'H', 'D', 'R');
constexpr auto ResourceDefinitionTag = FourCC('R', 'D', '1', '1');
constexpr uint32_t RdefHeaderSize = 60;
constexpr uint32_t RdefBufferSize = 24;
constexpr uint32_t RdefBindingSize = 32;
constexpr uint32_t RdefVariableSize = 40;
constexpr uint32_t RdefTypeSize = 36;
constexpr uint32_t RdefMemberSize = 12;

uint32_t ReadWord(llvm::StringRef data, size_t offset) {
  uint32_t word;
  std::memcpy(&word, data.data() + offset, sizeof(word));
  return word;
}

class BytePart {
public:
  std::vector<uint8_t> Data;

  uint32_t Size() const { return static_cast<uint32_t>(Data.size()); }

  void WriteWords(std::initializer_list<uint32_t> words) {
    auto *start = reinterpret_cast<const uint8_t *>(words.begin());
    Data.insert(Data.end(), start, start + words.size() * sizeof(uint32_t));
  }

  uint32_t AppendString(llvm::StringRef str) {
    auto offset = Size();
    Data.insert(Data.end(), str.bytes_begin(), str.bytes_end());
    Data.push_back(0);
    return offset;
  }

  void PatchWord(uint32_t offset, uint32_t value) {
    std::memcpy(Data.data() + offset, &value, sizeof(value));
  }

  uint32_t AppendBytes(const std::vector<uint8_t> &bytes) {
    const auto offset = Size();
    Data.insert(Data.end(), bytes.begin(), bytes.end());
    Align();
    return offset;
  }

  void Align() {
    constexpr uint32_t Alignment = sizeof(uint32_t);
    Data.resize((Size() + Alignment - 1) & ~(Alignment - 1));
  }
};

const DxilPartHeader *FindPart(const DxilContainerHeader *container,
                              std::initializer_list<uint32_t> tags) {
  for (auto tag : tags)
    if (auto *part = GetDxilPartByType(container, static_cast<DxilFourCC>(tag)))
      return part;
  return nullptr;
}

bool HasVertexCode(const DxilContainerHeader *container) {
  const auto *part = FindPart(container, {ShaderCode, LegacyShaderCode});
  if (!part || part->PartSize < 2 * sizeof(uint32_t))
    return false;
  llvm::StringRef code(GetDxilPartData(part), part->PartSize);
  return ReadWord(code, 0) == ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(
                                  D3D10_SB_VERTEX_SHADER, 5, 0);
}

bool ReadSignatureElement(llvm::StringRef data, size_t offset,
                          SignatureElement &element, std::string &error) {
  const uint32_t stringOffset = ReadWord(data, offset);
  if (stringOffset >= data.size()) {
    error = "DXBC signature name points outside its part";
    return false;
  }
  auto name = data.drop_front(stringOffset);
  const auto end = name.find('\0');
  if (end == 0 || end == llvm::StringRef::npos) {
    error = "DXBC signature has an invalid semantic name";
    return false;
  }
  element.Semantic = name.substr(0, end).str();
  element.SemanticIndex = ReadWord(data, offset + 4);
  element.SystemValue = ReadWord(data, offset + 8);
  element.ComponentType = ReadWord(data, offset + 12);
  element.Register = ReadWord(data, offset + 16);
  if (element.Register >= D3D11_VS_OUTPUT_REGISTER_COUNT) {
    error = "DXBC vertex output register exceeds SM5 limits";
    return false;
  }
  uint32_t masks = ReadWord(data, offset + 20);
  element.Mask = masks & 0xfu;
  element.UsedMask = element.Mask & ~(masks >> 8);
  if (!element.Mask || element.ComponentType < D3D_REGISTER_COMPONENT_UINT32 ||
      element.ComponentType > D3D_REGISTER_COMPONENT_FLOAT32) {
    error = "DXBC signature has an unsupported component type or mask";
    return false;
  }
  return true;
}

uint32_t EncodedSemantic(const SignatureElement &element) {
  if (element.SystemValue == D3D_NAME_TARGET ||
      element.SystemValue == D3D_NAME_DEPTH ||
      element.SystemValue == D3D_NAME_DEPTH_GREATER_EQUAL ||
      element.SystemValue == D3D_NAME_DEPTH_LESS_EQUAL ||
      element.SystemValue == D3D_NAME_COVERAGE)
    return D3D_NAME_UNDEFINED;
  return element.SystemValue;
}

uint32_t PackedType(uint16_t low, uint16_t high) {
  return static_cast<uint32_t>(low) | (static_cast<uint32_t>(high) << 16);
}

void WriteBinding(BytePart &out, uint32_t type, uint32_t returnType,
                  uint32_t dimension, uint32_t samples, uint32_t slot,
                  uint32_t count, uint32_t flags) {
  out.WriteWords({0, type, returnType, dimension, samples, slot, count, flags});
}

uint32_t BindingSamples(const BoundResource &resource) {
  if (resource.Type == D3D_SIT_STRUCTURED ||
      resource.Type == D3D_SIT_UAV_RWSTRUCTURED ||
      resource.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER)
    return resource.Stride;
  if (resource.Type == D3D_SIT_BYTEADDRESS ||
      resource.Type == D3D_SIT_UAV_RWBYTEADDRESS)
    return 0;
  if (resource.Dimension == D3D_SRV_DIMENSION_TEXTURE2DMS ||
      resource.Dimension == D3D_SRV_DIMENSION_TEXTURE2DMSARRAY)
    return resource.SampleCount;
  return (resource.Type == D3D_SIT_TEXTURE ||
          resource.Type == D3D_SIT_UAV_RWTYPED)
             ? UINT32_MAX
             : 0;
}

uint32_t ResourceFlags(const BoundResource &resource) {
  if (resource.Type == D3D_SIT_TEXTURE ||
      resource.Type == D3D_SIT_UAV_RWTYPED)
    return (resource.Components - 1) * D3D_SIF_TEXTURE_COMPONENT_0;
  if (resource.Type == D3D_SIT_STRUCTURED ||
      resource.Type == D3D_SIT_BYTEADDRESS ||
      resource.Type == D3D_SIT_UAV_RWSTRUCTURED ||
      resource.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER ||
      resource.Type == D3D_SIT_UAV_RWBYTEADDRESS)
    return 0u;
  return resource.Comparison ? D3D_SIF_COMPARISON_SAMPLER : 0u;
}

struct RdefMemberKey {
  std::string Name;
  uint32_t TypeOffset;
  uint32_t Offset;
  bool operator<(const RdefMemberKey &other) const {
    return std::tie(TypeOffset, Offset, Name) <
           std::tie(other.TypeOffset, other.Offset, other.Name);
  }
};

struct RdefTypeKey {
  uint16_t Class;
  uint16_t Type;
  uint16_t Rows;
  uint16_t Columns;
  uint16_t Elements;
  std::string TypeName;
  std::vector<RdefMemberKey> Members;
  bool operator<(const RdefTypeKey &other) const {
    return std::tie(Class, Type, Rows, Columns, Elements, TypeName, Members) <
           std::tie(other.Class, other.Type, other.Rows, other.Columns,
                    other.Elements, other.TypeName, other.Members);
  }
};

using RdefTypeCache = std::map<RdefTypeKey, uint32_t>;

uint32_t WriteType(BytePart &out, const ConstantVariable &variable,
                   RdefTypeCache &typeCache) {
  RdefTypeKey key{variable.Class, variable.Type, variable.Rows,
                  variable.Columns, variable.Elements, variable.TypeName, {}};
  key.Members.reserve(variable.Members.size());
  for (const auto &member : variable.Members) {
    key.Members.push_back(
        {member.Name, WriteType(out, member, typeCache), member.Offset});
  }
  auto it = typeCache.find(key);
  if (it != typeCache.end())
    return it->second;

  struct MemberDesc { uint32_t Name, Type, Offset; };
  std::vector<MemberDesc> members;
  members.reserve(key.Members.size());
  for (const auto &member : key.Members) {
    const auto name = out.AppendString(member.Name);
    members.push_back({name, member.TypeOffset, member.Offset});
  }
  const auto name = variable.TypeName.empty() ? 0 : out.AppendString(variable.TypeName);
  out.Align();
  const auto fields = members.empty() ? 0 : out.Size();
  for (const auto &member : members)
    out.WriteWords({member.Name, member.Type, member.Offset});
  const auto type = out.Size();
  out.WriteWords({PackedType(variable.Class, variable.Type),
                  PackedType(variable.Rows, variable.Columns),
                  PackedType(variable.Elements, static_cast<uint16_t>(members.size())),
                  fields, 0, 0, 0, 0, name});
  typeCache.emplace(std::move(key), type);
  return type;
}

void WriteVariables(BytePart &out, const ConstantBuffer &buffer,
                    uint32_t descriptor, RdefTypeCache &typeCache) {
  const auto firstVariable = out.Size();
  out.PatchWord(descriptor + 8, buffer.Variables.empty() ? 0 : firstVariable);
  for (const auto &variable : buffer.Variables) {
    out.WriteWords({0, variable.Offset, variable.Size,
                    variable.Used ? D3D_SVF_USED : 0u,
                    0, 0, UINT32_MAX, 0, UINT32_MAX, 0});
  }
  auto offset = firstVariable;
  for (const auto &variable : buffer.Variables) {
    out.PatchWord(offset, out.AppendString(variable.Name));
    out.Align();
    out.PatchWord(offset + 16, WriteType(out, variable, typeCache));
    if (!variable.DefaultValue.empty())
      out.PatchWord(offset + 20, out.AppendBytes(variable.DefaultValue));
    offset += RdefVariableSize;
  }
}
} // namespace

std::vector<uint8_t>
EncodeSignature(const std::vector<SignatureElement> &elements, bool output) {
  // SM5 signature consumers expect register order with shared lanes adjacent.
  std::vector<const SignatureElement *> ordered;
  ordered.reserve(elements.size());
  for (const auto &element : elements)
    ordered.push_back(&element);
  std::stable_sort(ordered.begin(), ordered.end(), [](auto *left, auto *right) {
    return std::tie(left->Register, left->Mask) < std::tie(right->Register, right->Mask);
  });

  BytePart part;
  part.WriteWords({static_cast<uint32_t>(ordered.size()),
                   sizeof(D3D10_INTERNALSHADER_SIGNATURE)});
  for (const auto *element : ordered) {
    const uint32_t used =
        output ? (~element->UsedMask & 0xfu) : element->UsedMask;
    part.WriteWords({0, element->SemanticIndex, EncodedSemantic(*element),
                     element->ComponentType, element->Register,
                     element->Mask | (used << 8)});
  }
  uint32_t offset = sizeof(D3D10_INTERNALSHADER_SIGNATURE);
  for (const auto *element : ordered) {
    part.PatchWord(offset, part.AppendString(element->Semantic));
    offset += sizeof(D3D10_INTERNALSHADER_PARAMETER);
  }
  part.Align();
  return std::move(part.Data);
}

bool ReadVertexOutputSignature(llvm::StringRef bytecode,
                               std::vector<SignatureElement> &output,
                               std::string &error,
                               llvm::StringRef *signaturePart) {
  auto *container = IsDxilContainerLike(bytecode.data(), bytecode.size());
  if (!container || !IsValidDxilContainer(container, bytecode.size()) ||
      !HasVertexCode(container)) {
    error = "link input is not an SM5.0 DXBC vertex shader";
    return false;
  }
  const auto *part = FindPart(container, {LegacyOutputSignature,
                                         StreamOutputSignature,
                                         ExtendedOutputSignature});
  if (!part || part->PartSize < sizeof(D3D10_INTERNALSHADER_SIGNATURE)) {
    error = "DXBC vertex shader has no output signature";
    return false;
  }
  llvm::StringRef data(GetDxilPartData(part), part->PartSize);
  bool streamed = part->PartFourCC != LegacyOutputSignature;
  size_t stride = sizeof(D3D10_INTERNALSHADER_PARAMETER);
  if (streamed)
    stride = part->PartFourCC == StreamOutputSignature
                 ? sizeof(D3D11_INTERNALSHADER_PARAMETER_FOR_GS)
                 : sizeof(D3D11_INTERNALSHADER_PARAMETER_11_1);
  const auto count = ReadWord(data, 0);
  auto offset = ReadWord(data, 4);
  if (offset < sizeof(D3D10_INTERNALSHADER_SIGNATURE) || offset > data.size() ||
      count > (data.size() - offset) / stride) {
    error = "DXBC signature table is out of bounds";
    return false;
  }
  std::vector<SignatureElement> elements;
  elements.reserve(count);
  for (uint32_t index = 0; index < count; ++index, offset += stride) {
    SignatureElement element;
    if (streamed && ReadWord(data, offset)) {
      error = "DXBC vertex shader output signature has a nonzero stream";
      return false;
    }
    if (!ReadSignatureElement(data, offset + (streamed ? 4 : 0), element, error))
      return false;
    elements.push_back(std::move(element));
  }
  output = std::move(elements);
  if (signaturePart)
    *signaturePart = llvm::StringRef(reinterpret_cast<const char *>(part),
                                    sizeof(DxilPartHeader) + part->PartSize);
  return true;
}

std::vector<uint8_t> EncodeResourceDefinitions(const MirProgram &program) {
  BytePart out;
  const auto bufferCount = static_cast<uint32_t>(program.ConstantBuffers.size());
  const auto bindingCount = bufferCount + program.Resources.size();
  const auto firstBuffer =
      static_cast<uint32_t>(RdefHeaderSize + bindingCount * RdefBindingSize);
  const uint32_t target = program.ShaderStage == Stage::Pixel ? 0xffff0500u
                                                               : 0xfffe0500u;
  out.WriteWords({bufferCount, bufferCount ? firstBuffer : 0,
                  static_cast<uint32_t>(bindingCount),
                  bindingCount ? RdefHeaderSize : 0, target, 0, 0,
                  ResourceDefinitionTag, RdefHeaderSize, RdefBufferSize,
                  RdefBindingSize, RdefVariableSize, RdefTypeSize, RdefMemberSize,
                  0});
  for (const auto &buffer : program.ConstantBuffers)
    WriteBinding(out, D3D_SIT_CBUFFER, 0, 0, 0, buffer.Slot, 1, 0);
  for (const auto &resource : program.Resources)
    WriteBinding(out, resource.Type, resource.ReturnType, resource.Dimension,
                 BindingSamples(resource), resource.Slot, resource.Count,
                 ResourceFlags(resource));
  for (const auto &buffer : program.ConstantBuffers) {
    out.WriteWords({0, static_cast<uint32_t>(buffer.Variables.size()), 0,
                    buffer.Size, 0, D3D_CT_CBUFFER});
  }
  RdefTypeCache typeCache;
  uint32_t binding = RdefHeaderSize, descriptor = firstBuffer;
  for (const auto &buffer : program.ConstantBuffers) {
    const auto name = out.AppendString(buffer.Name);
    out.PatchWord(binding, name);
    out.PatchWord(descriptor, name);
    out.Align();
    WriteVariables(out, buffer, descriptor, typeCache);
    binding += RdefBindingSize;
    descriptor += RdefBufferSize;
  }
  for (const auto &resource : program.Resources) {
    out.PatchWord(binding, out.AppendString(resource.Name));
    binding += RdefBindingSize;
  }
  out.PatchWord(24, out.AppendString("DXCB5"));
  out.Align();
  return std::move(out.Data);
}

std::vector<uint8_t> EncodeStatistics(const MirProgram &program) {
  std::array<uint32_t, 37> stats{};
  stats[0] = static_cast<uint32_t>(program.Instructions.size());
  stats[1] = program.TempCount;
  stats[3] = static_cast<uint32_t>(
      program.ConstantBuffers.size() + program.Resources.size() +
      program.Inputs.size() + program.Outputs.size() +
      (program.TempCount ? 1u : 0u) + program.IndexableTemps.size());
  stats[10] = static_cast<uint32_t>(program.IndexableTemps.size());
  for (const auto &inst : program.Instructions) {
    switch (inst.Opcode) {
    case D3D10_SB_OPCODE_MOV: ++stats[19]; break;
    case D3D10_SB_OPCODE_MOVC: ++stats[20]; break;
    case D3D10_SB_OPCODE_FTOI: case D3D10_SB_OPCODE_FTOU:
    case D3D10_SB_OPCODE_ITOF: case D3D10_SB_OPCODE_UTOF:
    case D3D11_SB_OPCODE_F16TOF32: case D3D11_SB_OPCODE_F32TOF16:
      ++stats[21]; break;
    case D3D10_SB_OPCODE_IF: case D3D10_SB_OPCODE_SWITCH:
    case D3D10_SB_OPCODE_BREAKC: case D3D10_SB_OPCODE_CONTINUEC:
    case D3D10_SB_OPCODE_RETC:
      ++stats[8]; ++stats[7]; break;
    case D3D10_SB_OPCODE_ELSE: case D3D10_SB_OPCODE_ENDIF:
    case D3D10_SB_OPCODE_LOOP: case D3D10_SB_OPCODE_ENDLOOP:
    case D3D10_SB_OPCODE_ENDSWITCH: case D3D10_SB_OPCODE_RET:
      ++stats[7]; break;
    case D3D10_SB_OPCODE_SAMPLE: case D3D10_SB_OPCODE_SAMPLE_L:
    case D3D10_1_SB_OPCODE_GATHER4: case D3D11_SB_OPCODE_GATHER4_PO:
    case D3D10_1_SB_OPCODE_LOD:
      ++stats[14]; break;
    case D3D10_SB_OPCODE_LD: case D3D10_SB_OPCODE_LD_MS:
    case D3D11_SB_OPCODE_LD_RAW: case D3D11_SB_OPCODE_LD_STRUCTURED:
    case D3D11_SB_OPCODE_LD_UAV_TYPED:
      ++stats[15]; break;
    case D3D10_SB_OPCODE_SAMPLE_C: case D3D10_SB_OPCODE_SAMPLE_C_LZ:
    case D3D11_SB_OPCODE_GATHER4_C: case D3D11_SB_OPCODE_GATHER4_PO_C:
      ++stats[16]; break;
    case D3D10_SB_OPCODE_SAMPLE_B: ++stats[17]; break;
    case D3D10_SB_OPCODE_SAMPLE_D: ++stats[18]; break;
    case D3D10_SB_OPCODE_ADD: case D3D10_SB_OPCODE_DIV:
    case D3D10_SB_OPCODE_DP2: case D3D10_SB_OPCODE_DP3:
    case D3D10_SB_OPCODE_DP4: case D3D10_SB_OPCODE_EQ:
    case D3D10_SB_OPCODE_EXP: case D3D10_SB_OPCODE_FRC:
    case D3D10_SB_OPCODE_GE: case D3D10_SB_OPCODE_LOG:
    case D3D10_SB_OPCODE_LT: case D3D10_SB_OPCODE_MAD:
    case D3D10_SB_OPCODE_MIN: case D3D10_SB_OPCODE_MAX:
    case D3D10_SB_OPCODE_MUL: case D3D10_SB_OPCODE_NE:
    case D3D10_SB_OPCODE_ROUND_NE: case D3D10_SB_OPCODE_ROUND_NI:
    case D3D10_SB_OPCODE_ROUND_PI: case D3D10_SB_OPCODE_ROUND_Z:
    case D3D10_SB_OPCODE_RSQ: case D3D10_SB_OPCODE_SINCOS:
    case D3D10_SB_OPCODE_SQRT: case D3D11_SB_OPCODE_RCP:
    case D3D10_SB_OPCODE_DERIV_RTX: case D3D10_SB_OPCODE_DERIV_RTY:
    case D3D11_SB_OPCODE_DERIV_RTX_COARSE: case D3D11_SB_OPCODE_DERIV_RTX_FINE:
    case D3D11_SB_OPCODE_DERIV_RTY_COARSE: case D3D11_SB_OPCODE_DERIV_RTY_FINE:
      ++stats[4]; break;
    case D3D10_SB_OPCODE_IADD: case D3D10_SB_OPCODE_IEQ:
    case D3D10_SB_OPCODE_IGE: case D3D10_SB_OPCODE_ILT:
    case D3D10_SB_OPCODE_IMAD: case D3D10_SB_OPCODE_IMAX:
    case D3D10_SB_OPCODE_IMIN: case D3D10_SB_OPCODE_IMUL:
    case D3D10_SB_OPCODE_INE: case D3D10_SB_OPCODE_INEG:
    case D3D10_SB_OPCODE_ISHL: case D3D10_SB_OPCODE_ISHR:
    case D3D11_SB_OPCODE_IBFE: case D3D11_SB_OPCODE_FIRSTBIT_SHI:
      ++stats[5]; break;
    case D3D10_SB_OPCODE_AND: case D3D10_SB_OPCODE_NOT:
    case D3D10_SB_OPCODE_OR: case D3D10_SB_OPCODE_XOR:
    case D3D10_SB_OPCODE_UDIV: case D3D10_SB_OPCODE_UGE:
    case D3D10_SB_OPCODE_ULT: case D3D10_SB_OPCODE_UMAD:
    case D3D10_SB_OPCODE_UMAX: case D3D10_SB_OPCODE_UMIN:
    case D3D10_SB_OPCODE_UMUL: case D3D10_SB_OPCODE_USHR:
    case D3D11_SB_OPCODE_BFI: case D3D11_SB_OPCODE_BFREV:
    case D3D11_SB_OPCODE_COUNTBITS: case D3D11_SB_OPCODE_FIRSTBIT_HI:
    case D3D11_SB_OPCODE_FIRSTBIT_LO: case D3D11_SB_OPCODE_UBFE:
    case D3D11_SB_OPCODE_UADDC: case D3D11_SB_OPCODE_USUBB:
      ++stats[6]; break;
    case D3D11_SB_OPCODE_SYNC:
      ++stats[34]; break;
    case D3D11_SB_OPCODE_ATOMIC_AND: case D3D11_SB_OPCODE_ATOMIC_OR:
    case D3D11_SB_OPCODE_ATOMIC_XOR: case D3D11_SB_OPCODE_ATOMIC_CMP_STORE:
    case D3D11_SB_OPCODE_ATOMIC_IADD: case D3D11_SB_OPCODE_ATOMIC_IMAX:
    case D3D11_SB_OPCODE_ATOMIC_IMIN: case D3D11_SB_OPCODE_ATOMIC_UMAX:
    case D3D11_SB_OPCODE_ATOMIC_UMIN: case D3D11_SB_OPCODE_IMM_ATOMIC_IADD:
    case D3D11_SB_OPCODE_IMM_ATOMIC_AND: case D3D11_SB_OPCODE_IMM_ATOMIC_OR:
    case D3D11_SB_OPCODE_IMM_ATOMIC_XOR: case D3D11_SB_OPCODE_IMM_ATOMIC_EXCH:
    case D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH:
    case D3D11_SB_OPCODE_IMM_ATOMIC_IMAX: case D3D11_SB_OPCODE_IMM_ATOMIC_IMIN:
    case D3D11_SB_OPCODE_IMM_ATOMIC_UMAX: case D3D11_SB_OPCODE_IMM_ATOMIC_UMIN:
    case D3D11_SB_OPCODE_IMM_ATOMIC_ALLOC:
    case D3D11_SB_OPCODE_IMM_ATOMIC_CONSUME:
      ++stats[35]; break;
    case D3D11_SB_OPCODE_STORE_UAV_TYPED: case D3D11_SB_OPCODE_STORE_RAW:
    case D3D11_SB_OPCODE_STORE_STRUCTURED:
      ++stats[36]; break;
    default: break;
    }
  }
  std::vector<uint8_t> bytes(stats.size() * sizeof(uint32_t));
  std::memcpy(bytes.data(), stats.data(), bytes.size());
  return bytes;
}

} // namespace dxcb5
