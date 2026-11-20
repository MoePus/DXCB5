// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_H
#define DXCB5_MIR_H

#include "dxcb5/Reflection.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dxcb5 {

enum class Stage { Vertex, Pixel };
enum class OperandKind {
  Temp, Input, Output, Immediate, ConstantBuffer, Resource, Sampler, Null,
  FrontFace, ImmediateConstantBuffer, ImmediateVector, IndexableTemp,
  OutputDepth, Uav, InputCoverage, OutputMask
};

struct IndexableTempDecl {
  uint32_t Index = 0;
  uint32_t Elements = 0;
  uint32_t Components = 1;
};

struct Operand {
  OperandKind Kind = OperandKind::Null;
  uint32_t Index = 0;
  uint32_t Element = 0;
  uint8_t Mask = 1;
  uint8_t Swizzle = 0;
  uint8_t Modifier = 0;
  uint32_t RelativeRegister = UINT32_MAX;
  uint8_t RelativeLane = 0;

  bool SameValue(const Operand &O) const {
    return Kind == O.Kind && Index == O.Index && Element == O.Element &&
           Swizzle == O.Swizzle && Modifier == O.Modifier &&
           RelativeRegister == O.RelativeRegister &&
           RelativeLane == O.RelativeLane;
  }

  bool operator==(const Operand &O) const {
    return Mask == O.Mask && SameValue(O);
  }
};

struct Instruction {
  uint32_t Opcode;
  std::vector<Operand> Operands;
  bool TestNonZero = true;
  bool Saturate = false;
  int8_t TexelOffset[3] = {};
};

struct SignatureElement {
  std::string Semantic;
  uint32_t SemanticIndex = 0;
  uint32_t Register = 0;
  uint32_t SystemValue = 0;
  uint32_t ComponentType = 3;
  uint32_t Interpolation = 0;
  uint8_t Mask = 0;
  uint8_t UsedMask = 0;
};

struct MirProgram {
  Stage ShaderStage = Stage::Vertex;
  uint32_t TempCount = 0;
  std::vector<SignatureElement> Inputs;
  std::vector<SignatureElement> Outputs;
  std::vector<Instruction> Instructions;
  std::vector<ConstantBuffer> ConstantBuffers;
  std::vector<BoundResource> Resources;
  // Each SM5 immediate constant buffer row holds four raw 32-bit values.
  std::vector<uint32_t> ImmediateConstants;
  std::vector<std::array<uint32_t, 4>> LiteralVectors;
  std::vector<IndexableTempDecl> IndexableTemps;
};

} // namespace dxcb5
#endif
