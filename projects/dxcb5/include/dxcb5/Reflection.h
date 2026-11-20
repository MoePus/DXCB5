// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_REFLECTION_H
#define DXCB5_REFLECTION_H

#include <cstdint>
#include <string>
#include <vector>

namespace dxcb5 {

struct ConstantVariable {
  std::string Name;
  uint32_t Offset = 0;
  uint32_t Size = 0;
  // Values of D3D_SHADER_VARIABLE_CLASS and D3D_SHADER_VARIABLE_TYPE.
  uint16_t Class = 0;
  uint16_t Type = 3;
  uint16_t Rows = 1;
  uint16_t Columns = 1;
  uint16_t Elements = 0;
  bool Used = true;
  std::vector<uint8_t> DefaultValue;
  std::string TypeName;
  std::vector<ConstantVariable> Members;
};

struct ConstantBuffer {
  std::string Name;
  uint32_t Slot = 0;
  uint32_t Size = 0;
  bool DynamicIndex = false;
  std::vector<ConstantVariable> Variables;
};

struct BoundResource {
  std::string Name;
  uint32_t Slot = 0;
  // Values of D3D_SHADER_INPUT_TYPE, D3D_SRV_DIMENSION and D3D_RESOURCE_RETURN_TYPE.
  uint32_t Type = 2;
  uint32_t Dimension = 4;
  uint32_t ReturnType = 5;
  uint32_t Count = 1;
  bool Comparison = false;
  uint32_t Components = 4;
  uint32_t SampleCount = 0;
  uint32_t Stride = 0;
  uint32_t UavFlags = 0;
};

} // namespace dxcb5
#endif
