// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_UNITY_HOOK_UNITY_TYPES_H
#define DXCB5_UNITY_HOOK_UNITY_TYPES_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace dxcb5_hook {

// Verified against Unity 2022.3.22f1 (Unity_x64.pdb, TimeDateStamp 0x65F0FF60):
// core::basic_string<char, core::StringStorageDefault<char>> (sizeof == 0x30):
//   +0x00: inline buf[24] OR heap char* ptr (8 bytes)
//   +0x10: heap size_t len (when byte[+0x20] != 1)
//   +0x18: inline remaining = int8_t(24 - len) (when byte[+0x20] == 1)
//   +0x20: uint8_t storageKind (1 == inline SSO, 0 == heap, 2 == external)
struct alignas(8) UnityCoreString {
  uint8_t Raw[0x30];

  bool TryView(std::string_view &out) const {
    const uint8_t kind = Raw[0x20];
    if (kind == 1) {
      const int8_t rem = static_cast<int8_t>(Raw[0x18]);
      const int len = 24 - static_cast<int>(rem);
      if (len < 0 || len > 24)
        return false;
      out = {reinterpret_cast<const char *>(Raw), static_cast<size_t>(len)};
      return true;
    }
    if (kind == 0 || kind == 2) {
      const char *ptr = nullptr;
      uint64_t len = 0;
      std::memcpy(&ptr, Raw + 0x00, sizeof(ptr));
      std::memcpy(&len, Raw + 0x10, sizeof(len));
      if (len == 0) {
        out = {};
        return true;
      }
      if (!ptr || len > (1ull << 30))
        return false;
      out = {ptr, static_cast<size_t>(len)};
      return true;
    }
    return false;
  }

  std::string_view View() const {
    std::string_view sv;
    return TryView(sv) ? sv : std::string_view{};
  }
};
static_assert(sizeof(UnityCoreString) == 0x30, "UnityCoreString must be 0x30");

// Verified against Unity 2022.3.22f1 disassembly (ShaderCompilerCompileSnippet):
// core::vector<T, 0> (sizeof == 0x28):
//   +0x00: T* m_data
//   +0x08: MemLabelId m_label (12 bytes + 4 bytes padding = 16 bytes)
//   +0x18: size_t m_size
//   +0x20: size_t m_capacity (bit 0 = non-owned/inline flag)
template <typename T>
struct UnityCoreVector {
  T *Data;
  uint8_t MemLabel[16];
  uint64_t Size;
  uint64_t CapacityAndFlags;

  size_t Count() const { return Data ? static_cast<size_t>(Size) : 0u; }
};
static_assert(sizeof(UnityCoreVector<uint8_t>) == 0x28,
              "UnityCoreVector must be 0x28");
static_assert(offsetof(UnityCoreVector<uint8_t>, Size) == 0x18,
              "UnityCoreVector::Size must be at +0x18");

// Verified exact 10-slot vtable layout of Unity 2022.3.22f1
// GpuProgramReflectionReportImpl / ShaderCompilerReflectionReport (0x03953FE0).
struct UnityReflectionReport {
  virtual ~UnityReflectionReport() = default;
  virtual void OnInputBinding(int channel, int bindComp) = 0;
  virtual void OnConstantBuffer(const char *name, int size, int varCount) = 0;
  virtual void OnConstant(const char *name, int index, int paramType,
                          int constType, int rows, int cols,
                          int arraySize) = 0;
  virtual void OnCBBinding(const char *name, int index) = 0;
  virtual void OnTextureBinding(const char *name, int index, int samplerIndex,
                                bool multisampled, int texDim) = 0;
  virtual void OnInlineSampler(uint32_t samplerType, int index) = 0;
  virtual void OnBufferBinding(const char *name, int index, int extra) = 0;
  virtual void OnUAVBinding(const char *name, int index, int origIndex) = 0;
  virtual void OnStatsInfo(int alu, int tex, int flow, int temp) = 0;
};

struct RecordedReflection {
  struct InputRec { int Channel, BindComp; };
  struct CbRec { std::string Name; int Size, VarCount; };
  struct ConstRec {
    std::string Name;
    int Index, ParamType, ConstType, Rows, Cols, ArraySize;
  };
  struct BindRec { std::string Name; int Index, Extra; };
  struct TexRec {
    std::string Name;
    int Index, SamplerIndex, TexDim;
    bool Multisampled;
  };
  struct InlineSampRec { uint32_t SamplerType; int Index; };
  struct StatsRec { int Alu, Tex, Flow, Temp; bool Present = false; };

  enum class OpKind : uint8_t {
    Input, ConstantBuffer, Constant, CBBinding, TextureBinding,
    InlineSampler, BufferBinding, UAVBinding, StatsInfo
  };
  struct Event { OpKind Kind; uint32_t Index; };

  std::vector<InputRec> Inputs;
  std::vector<CbRec> Cbs;
  std::vector<ConstRec> Consts;
  std::vector<BindRec> CbBinds;
  std::vector<TexRec> TexBinds;
  std::vector<InlineSampRec> InlineSamps;
  std::vector<BindRec> BufBinds;
  std::vector<BindRec> UavBinds;
  StatsRec Stats{};
  std::vector<Event> Order;

  size_t ApproxBytes() const {
    size_t n = Order.size() * sizeof(Event) + Inputs.size() * sizeof(InputRec) +
               Cbs.size() * 48 + Consts.size() * 64 + CbBinds.size() * 48 +
               TexBinds.size() * 56 + BufBinds.size() * 48 + UavBinds.size() * 48;
    return n + 128;
  }

  void ReplayTo(UnityReflectionReport *dst) const {
    if (!dst)
      return;
    for (const auto &ev : Order) {
      switch (ev.Kind) {
      case OpKind::Input: {
        const auto &r = Inputs[ev.Index];
        dst->OnInputBinding(r.Channel, r.BindComp);
        break;
      }
      case OpKind::ConstantBuffer: {
        const auto &r = Cbs[ev.Index];
        dst->OnConstantBuffer(r.Name.c_str(), r.Size, r.VarCount);
        break;
      }
      case OpKind::Constant: {
        const auto &r = Consts[ev.Index];
        dst->OnConstant(r.Name.c_str(), r.Index, r.ParamType, r.ConstType,
                        r.Rows, r.Cols, r.ArraySize);
        break;
      }
      case OpKind::CBBinding: {
        const auto &r = CbBinds[ev.Index];
        dst->OnCBBinding(r.Name.c_str(), r.Index);
        break;
      }
      case OpKind::TextureBinding: {
        const auto &r = TexBinds[ev.Index];
        dst->OnTextureBinding(r.Name.c_str(), r.Index, r.SamplerIndex,
                              r.Multisampled, r.TexDim);
        break;
      }
      case OpKind::InlineSampler: {
        const auto &r = InlineSamps[ev.Index];
        dst->OnInlineSampler(r.SamplerType, r.Index);
        break;
      }
      case OpKind::BufferBinding: {
        const auto &r = BufBinds[ev.Index];
        dst->OnBufferBinding(r.Name.c_str(), r.Index, r.Extra);
        break;
      }
      case OpKind::UAVBinding: {
        const auto &r = UavBinds[ev.Index];
        dst->OnUAVBinding(r.Name.c_str(), r.Index, r.Extra);
        break;
      }
      case OpKind::StatsInfo:
        dst->OnStatsInfo(Stats.Alu, Stats.Tex, Stats.Flow, Stats.Temp);
        break;
      }
    }
  }
};

class RecordingReflectionReport final : public UnityReflectionReport {
public:
  explicit RecordingReflectionReport(UnityReflectionReport *inner)
      : Inner(inner) {}

  void OnInputBinding(int channel, int bindComp) override {
    Data.Order.push_back({RecordedReflection::OpKind::Input,
                          static_cast<uint32_t>(Data.Inputs.size())});
    Data.Inputs.push_back({channel, bindComp});
    if (Inner)
      Inner->OnInputBinding(channel, bindComp);
  }
  void OnConstantBuffer(const char *name, int size, int varCount) override {
    Data.Order.push_back({RecordedReflection::OpKind::ConstantBuffer,
                          static_cast<uint32_t>(Data.Cbs.size())});
    Data.Cbs.push_back({name ? name : "", size, varCount});
    if (Inner)
      Inner->OnConstantBuffer(name, size, varCount);
  }
  void OnConstant(const char *name, int index, int paramType, int constType,
                  int rows, int cols, int arraySize) override {
    Data.Order.push_back({RecordedReflection::OpKind::Constant,
                          static_cast<uint32_t>(Data.Consts.size())});
    Data.Consts.push_back(
        {name ? name : "", index, paramType, constType, rows, cols, arraySize});
    if (Inner)
      Inner->OnConstant(name, index, paramType, constType, rows, cols, arraySize);
  }
  void OnCBBinding(const char *name, int index) override {
    Data.Order.push_back({RecordedReflection::OpKind::CBBinding,
                          static_cast<uint32_t>(Data.CbBinds.size())});
    Data.CbBinds.push_back({name ? name : "", index, 0});
    if (Inner)
      Inner->OnCBBinding(name, index);
  }
  void OnTextureBinding(const char *name, int index, int samplerIndex,
                        bool multisampled, int texDim) override {
    Data.Order.push_back({RecordedReflection::OpKind::TextureBinding,
                          static_cast<uint32_t>(Data.TexBinds.size())});
    Data.TexBinds.push_back(
        {name ? name : "", index, samplerIndex, texDim, multisampled});
    if (Inner)
      Inner->OnTextureBinding(name, index, samplerIndex, multisampled, texDim);
  }
  void OnInlineSampler(uint32_t samplerType, int index) override {
    Data.Order.push_back({RecordedReflection::OpKind::InlineSampler,
                          static_cast<uint32_t>(Data.InlineSamps.size())});
    Data.InlineSamps.push_back({samplerType, index});
    if (Inner)
      Inner->OnInlineSampler(samplerType, index);
  }
  void OnBufferBinding(const char *name, int index, int extra) override {
    Data.Order.push_back({RecordedReflection::OpKind::BufferBinding,
                          static_cast<uint32_t>(Data.BufBinds.size())});
    Data.BufBinds.push_back({name ? name : "", index, extra});
    if (Inner)
      Inner->OnBufferBinding(name, index, extra);
  }
  void OnUAVBinding(const char *name, int index, int origIndex) override {
    Data.Order.push_back({RecordedReflection::OpKind::UAVBinding,
                          static_cast<uint32_t>(Data.UavBinds.size())});
    Data.UavBinds.push_back({name ? name : "", index, origIndex});
    if (Inner)
      Inner->OnUAVBinding(name, index, origIndex);
  }
  void OnStatsInfo(int alu, int tex, int flow, int temp) override {
    Data.Order.push_back({RecordedReflection::OpKind::StatsInfo, 0});
    Data.Stats = {alu, tex, flow, temp, true};
    if (Inner)
      Inner->OnStatsInfo(alu, tex, flow, temp);
  }

  RecordedReflection Data;

private:
  UnityReflectionReport *Inner = nullptr;
};

// Verified Unity 2022.3.22f1 constants & function signatures.
constexpr uint32_t kUnity2022_3_22f1_TimeDateStamp = 0x65F0FF60u;
constexpr uintptr_t kRvaShaderCompilerCompileSnippet = 0x02195A60u;
constexpr uintptr_t kRvaVectorU8Assign = 0x0097C400u;
constexpr int kShaderCompPlatformD3D11 = 4;
constexpr int kProgramVertex = 0;
constexpr int kProgramFragment = 1;

using PFN_UnityErrorCallback = void(__cdecl *)(void *userData, int errorType,
                                               int line, int platform,
                                               const char *msg,
                                               const char *msgDetails);

using PFN_VectorU8Assign = void(__cdecl *)(UnityCoreVector<uint8_t> *vec,
                                           const uint8_t *first,
                                           const uint8_t *last);

using PFN_ShaderCompilerCompileSnippet = bool(__cdecl *)(
    void **conn, const UnityCoreString &inputCode, uint64_t codeHash,
    const UnityCoreString &inputPath, const UnityCoreString &shaderName,
    int startLine, const UnityCoreString &entryName,
    const UnityCoreVector<UnityCoreString> &platformKeywords,
    const UnityCoreVector<UnityCoreString> &enabledKeywords,
    const UnityCoreVector<UnityCoreString> &disabledKeywords,
    uint64_t userDisabledKeywordCount, int buildTarget, uint64_t arg12,
    bool arg13, bool arg14, uint64_t compilationFlags,
    UnityCoreVector<uint8_t> &outputCode, uint32_t &outProgramType,
    int sourceLang, int programType, uint32_t programMask, int api,
    uint64_t requirements, UnityReflectionReport *report,
    PFN_UnityErrorCallback errorCb, void *errorCbData);

} // namespace dxcb5_hook

#endif // DXCB5_UNITY_HOOK_UNITY_TYPES_H
