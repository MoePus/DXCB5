// Distributed under the University of Illinois Open Source License.
// Opt-in integration test using generated MoeToon2 ForwardBase bytecode.
// Usage: MoeToonWarp mono|stereo BASELINE_VS BASELINE_PS [VS PS]
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using Color = std::array<float, 4>;

namespace {
constexpr UINT Side = 16;
constexpr float MatchTolerance = 1e-3f;
constexpr Color Clear{-1, -1, -1, -1};
constexpr Color MainColor{0.44f, 0.31f, 0.21f, 1};
constexpr std::array<float, 16> Identity{
    1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
using Frame = std::vector<Color>;

void Check(HRESULT result, const char *what) {
  if (FAILED(result)) {
    std::fprintf(stderr, "%s failed (HRESULT 0x%08lx)\n", what,
                 static_cast<unsigned long>(result));
    throw std::runtime_error(what);
  }
}

ComPtr<ID3DBlob> Read(const wchar_t *path) {
  ComPtr<ID3DBlob> bytes;
  Check(D3DReadFileToBlob(path, bytes.GetAddressOf()), "D3DReadFileToBlob");
  return bytes;
}

ComPtr<ID3D11ShaderReflection> Reflect(ID3DBlob &bytes) {
  ComPtr<ID3D11ShaderReflection> reflection;
  Check(D3DReflect(bytes.GetBufferPointer(), bytes.GetBufferSize(),
                   __uuidof(ID3D11ShaderReflection),
                   reinterpret_cast<void **>(reflection.GetAddressOf())),
        "D3DReflect");
  return reflection;
}

struct Vertex {
  Color Position;
  std::array<float, 3> Normal{0, 0, 1};
  std::array<float, 2> Uv{0.25f, 0.25f};
  std::array<float, 2> Uv1{};
  Color Surface{};
  Color Contact{};
  Color Tangent{1, 0, 0, 1};
};

constexpr D3D11_INPUT_ELEMENT_DESC InputFields[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, Position), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, Normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, Uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, Uv1), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, Surface), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, Contact), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, Tangent), D3D11_INPUT_PER_VERTEX_DATA, 0},
};

ComPtr<ID3D11Buffer> MakeBuffer(ID3D11Device &device, UINT bindFlags,
                               UINT size, const void *data) {
  const D3D11_BUFFER_DESC desc{size, D3D11_USAGE_IMMUTABLE, bindFlags};
  const D3D11_SUBRESOURCE_DATA initial{data};
  ComPtr<ID3D11Buffer> buffer;
  Check(device.CreateBuffer(&desc, &initial, buffer.GetAddressOf()), "CreateBuffer");
  return buffer;
}

void BindGeometry(ID3D11Device &device, ID3D11DeviceContext &context, ID3DBlob &vs,
                  ID3D11ShaderReflection &reflection) {
  D3D11_SHADER_DESC desc{};
  Check(reflection.GetDesc(&desc), "VS GetDesc");
  std::vector<D3D11_INPUT_ELEMENT_DESC> input;
  for (UINT i = 0; i < desc.InputParameters; ++i) {
    D3D11_SIGNATURE_PARAMETER_DESC field{};
    Check(reflection.GetInputParameterDesc(i, &field), "GetInputParameterDesc");
    if (field.SystemValueType != D3D_NAME_UNDEFINED)
      continue;
    const auto *match = std::find_if(std::begin(InputFields), std::end(InputFields),
        [&](const D3D11_INPUT_ELEMENT_DESC &candidate) {
          return _stricmp(candidate.SemanticName, field.SemanticName) == 0 &&
                 candidate.SemanticIndex == field.SemanticIndex;
        });
    if (match == std::end(InputFields))
      throw std::runtime_error(field.SemanticName);
    input.push_back(*match);
  }
  ComPtr<ID3D11InputLayout> layout;
  Check(device.CreateInputLayout(input.data(), static_cast<UINT>(input.size()),
                                 vs.GetBufferPointer(), vs.GetBufferSize(),
                                 layout.GetAddressOf()), "CreateInputLayout");
  const Vertex vertices[] = {{{-1, -1, 0.4f, 1}, {0, 0, 1}}, {{-1, 3, 0.4f, 1}, {0, 0.7f, 0.7f}}, {{3, -1, 0.4f, 1}, {0.5f, 0, 0.9f}}};
  auto buffer = MakeBuffer(device, D3D11_BIND_VERTEX_BUFFER, sizeof(vertices), vertices);
  constexpr UINT Stride = sizeof(Vertex);
  constexpr UINT Offset = 0;
  context.IASetVertexBuffers(0, 1, buffer.GetAddressOf(), &Stride, &Offset);
  context.IASetInputLayout(layout.Get());
  context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

bool SetMatrix(std::string_view name, const D3D11_SHADER_TYPE_DESC &type,
               const D3D11_SHADER_VARIABLE_DESC &var, uint8_t *data) {
  if (name == "unity_Builtins0Array") {
    for (UINT offset = 0; offset + sizeof(Identity) <= var.Size; offset += sizeof(Identity))
      std::memcpy(data + offset, Identity.data(), sizeof(Identity));
    return true;
  }
  if ((type.Class == D3D_SVC_MATRIX_ROWS || type.Class == D3D_SVC_MATRIX_COLUMNS) &&
      type.Rows == 4 && type.Columns == 4 && name.substr(0, 5) == "unity") {
    for (UINT offset = 0; offset + sizeof(Identity) <= var.Size; offset += sizeof(Identity))
      std::memcpy(data + offset, Identity.data(), sizeof(Identity));
    return true;
  }
  if (name == "glstate_matrix_projection") {
    std::memcpy(data, Identity.data(), sizeof(Identity));
    return true;
  }
  return false;
}

struct Value { std::string_view Name; Color Data; };
constexpr Value Values[] = {
    {"_Color", {1, 1, 1, 1}}, {"_WorldSpaceCameraPos", {0, 0, 3, 0}},
    {"unity_StereoWorldSpaceCameraPos", {0, 0, 3, 0}},
    {"_MainTex_ST", {1, 1, 0, 0}}, {"_MainTexHSVG", {0, 1, 1, 1}},
    {"_LightColor0", {1, 0.9f, 0.8f, 1}}, {"_WorldSpaceLightPos0", {0.48f, 0.36f, 0.8f, 0}},
    {"unity_SHAr", {0, 0, 0, 0.2f}}, {"unity_SHAg", {0, 0, 0, 0.2f}},
    {"unity_SHAb", {0, 0, 0, 0.2f}}, {"_GamutLUT_TexelSize", {1, 1, 1, 1}},
    {"_LightMaxLimit", {2, 0, 0, 0}}, {"_LightMinLimit", {0.02f, 0, 0, 0}},
    {"_NormalScale", {1, 0, 0, 0}}, {"_Cutoff", {0.2f, 0, 0, 0}},
    {"unity_FogParams", {0, 0, 0, 1}}, {"_ShadowStrength", {0.7f, 0, 0, 0}},
    {"_ShapeStrength", {0.5f, 0, 0, 0}}, {"_ShadowReceive", {0.7f, 0, 0, 0}},
    {"_ShadowBorder", {0.4f, 0, 0, 0}}, {"_ShadowBlur", {0.1f, 0, 0, 0}},
    {"_EnvironmentShape", {0.3f, 0, 0, 0}}, {"_FillFloor", {0.55f, 0, 0, 0}},
    {"_EmissionColor", {0.02f, 0.03f, 0.01f, 1}}, {"_RimStrength", {0.12f, 0, 0, 0}},
    {"_RimPower", {1.6f, 0, 0, 0}}, {"_RimBorder", {0.3f, 0, 0, 0}},
    {"_RimBlur", {0.18f, 0, 0, 0}}, {"_RimTint", {0.2f, 0.1f, 0.4f, 1}},
};

void WriteVariable(ID3D11ShaderReflectionVariable &variable, std::vector<uint8_t> &data) {
  D3D11_SHADER_VARIABLE_DESC var{};
  Check(variable.GetDesc(&var), "GetVariableDesc");
  if (!(var.uFlags & D3D_SVF_USED) || var.StartOffset + var.Size > data.size())
    return;
  auto *dest = data.data() + var.StartOffset;
  D3D11_SHADER_TYPE_DESC type{};
  Check(variable.GetType()->GetDesc(&type), "GetTypeDesc");
  if (SetMatrix(var.Name, type, var, dest))
    return;
  std::string_view name(var.Name);
  const auto *match = std::find_if(std::begin(Values), std::end(Values),
      [&](const Value &value) { return value.Name == name; });
  if (match != std::end(Values)) {
    constexpr UINT RowBytes = sizeof(Color);
    for (UINT offset = 0; offset < var.Size; offset += RowBytes)
      std::memcpy(dest + offset, match->Data.data(), std::min(RowBytes, var.Size - offset));
  } else if (var.DefaultValue) {
    std::memcpy(dest, var.DefaultValue, var.Size);
  }
}

ComPtr<ID3D11Buffer> MakeConstants(ID3D11Device &device,
                                    ID3D11ShaderReflectionConstantBuffer &cb) {
  D3D11_SHADER_BUFFER_DESC desc{};
  Check(cb.GetDesc(&desc), "GetBufferDesc");
  const UINT size = (desc.Size + 15u) & ~15u;
  std::vector<uint8_t> data(size);
  for (UINT i = 0; i < desc.Variables; ++i)
    WriteVariable(*cb.GetVariableByIndex(i), data);
  return MakeBuffer(device, D3D11_BIND_CONSTANT_BUFFER, size, data.data());
}

ComPtr<ID3D11ShaderResourceView> MakeTexture(ID3D11Device &device,
                                           const D3D11_SHADER_INPUT_BIND_DESC &binding) {
  const std::string_view name(binding.Name);
  const Color pixel = name == "_MainTex" ? MainColor
      : name == "_NormalMap" ? Color{0.5f, 0.5f, 1, 0.5f} : Color{1, 1, 1, 1};
  const std::array<uint32_t, 4> zero{};
  const bool isUint = binding.ReturnType == D3D_RETURN_TYPE_UINT;
  const DXGI_FORMAT format = isUint ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
  const D3D11_SUBRESOURCE_DATA data{isUint ? static_cast<const void *>(zero.data()) : pixel.data(),
                                  sizeof(Color), sizeof(Color)};
  ComPtr<ID3D11Resource> texture;
  if (binding.Dimension == D3D_SRV_DIMENSION_TEXTURE3D) {
    const D3D11_TEXTURE3D_DESC desc{1, 1, 1, 1, format,
        D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture3D> tex;
    Check(device.CreateTexture3D(&desc, &data, tex.GetAddressOf()), "CreateTexture3D");
    texture = tex;
  } else if (binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D ||
             binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2DARRAY) {
    const D3D11_TEXTURE2D_DESC desc{1, 1, 1, 1, format, {1, 0},
        D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> tex;
    Check(device.CreateTexture2D(&desc, &data, tex.GetAddressOf()), "CreateTexture2D");
    texture = tex;
  } else {
    throw std::runtime_error(binding.Name);
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC desc{format, binding.Dimension};
  desc.Texture2DArray.MipLevels = 1;
  desc.Texture2DArray.ArraySize = 1;
  ComPtr<ID3D11ShaderResourceView> view;
  Check(device.CreateShaderResourceView(texture.Get(), &desc, view.GetAddressOf()),
        "CreateShaderResourceView");
  return view;
}

ComPtr<ID3D11SamplerState> MakeSampler(ID3D11Device &device) {
  D3D11_SAMPLER_DESC desc{};
  desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  desc.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> sampler;
  Check(device.CreateSamplerState(&desc, sampler.GetAddressOf()), "CreateSamplerState");
  return sampler;
}

void BindResources(ID3D11Device &device, ID3D11DeviceContext &context,
                   ID3D11ShaderReflection &reflection, bool vertex) {
  D3D11_SHADER_DESC desc{};
  Check(reflection.GetDesc(&desc), "shader GetDesc");
  auto cbSet = vertex ? &ID3D11DeviceContext::VSSetConstantBuffers : &ID3D11DeviceContext::PSSetConstantBuffers;
  auto texSet = vertex ? &ID3D11DeviceContext::VSSetShaderResources : &ID3D11DeviceContext::PSSetShaderResources;
  auto samplerSet = vertex ? &ID3D11DeviceContext::VSSetSamplers : &ID3D11DeviceContext::PSSetSamplers;
  auto sampler = MakeSampler(device);
  for (UINT i = 0; i < desc.BoundResources; ++i) {
    D3D11_SHADER_INPUT_BIND_DESC binding{};
    Check(reflection.GetResourceBindingDesc(i, &binding), "GetResourceBindingDesc");
    if (binding.Type == D3D_SIT_CBUFFER) {
      auto cb = MakeConstants(device, *reflection.GetConstantBufferByName(binding.Name));
      (context.*cbSet)(binding.BindPoint, 1, cb.GetAddressOf());
    } else if (binding.Type == D3D_SIT_TEXTURE) {
      auto srv = MakeTexture(device, binding);
      (context.*texSet)(binding.BindPoint, 1, srv.GetAddressOf());
    } else if (binding.Type == D3D_SIT_SAMPLER) {
      (context.*samplerSet)(binding.BindPoint, 1, sampler.GetAddressOf());
    } else {
      throw std::runtime_error(binding.Name);
    }
  }
}

void BindShaders(ID3D11Device &device, ID3D11DeviceContext &context,
                 ID3DBlob &vertex, ID3DBlob &pixel) {
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  Check(device.CreateVertexShader(vertex.GetBufferPointer(), vertex.GetBufferSize(),
                                  nullptr, vs.GetAddressOf()), "CreateVertexShader");
  Check(device.CreatePixelShader(pixel.GetBufferPointer(), pixel.GetBufferSize(),
                                 nullptr, ps.GetAddressOf()), "CreatePixelShader");
  context.VSSetShader(vs.Get(), nullptr, 0);
  context.PSSetShader(ps.Get(), nullptr, 0);
  const D3D11_VIEWPORT viewport{0, 0, Side, Side, 0, 1};
  context.RSSetViewports(1, &viewport);
  D3D11_RASTERIZER_DESC desc{};
  desc.FillMode = D3D11_FILL_SOLID;
  desc.CullMode = D3D11_CULL_NONE;
  desc.FrontCounterClockwise = TRUE;
  desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  Check(device.CreateRasterizerState(&desc, raster.GetAddressOf()),
        "CreateRasterizerState");
  context.RSSetState(raster.Get());
}

Frame Readback(ID3D11DeviceContext &context, ID3D11Texture2D &image, UINT layers) {
  Frame frame;
  for (UINT eye = 0; eye < layers; ++eye) {
    D3D11_MAPPED_SUBRESOURCE map{};
    Check(context.Map(&image, eye, D3D11_MAP_READ, 0, &map), "Map (readback)");
    const auto *data = static_cast<const uint8_t *>(map.pData);
    const auto offset = frame.size();
    frame.resize(offset + Side * Side);
    for (UINT y = 0; y < Side; ++y)
      std::memcpy(frame.data() + offset + y * Side, data + y * map.RowPitch, Side * sizeof(Color));
    context.Unmap(&image, eye);
  }
  return frame;
}

Frame Draw(ID3D11Device *device, ID3D11DeviceContext *context,
           ID3DBlob &vs, ID3DBlob &ps, UINT layers) {
  BindShaders(*device, *context, vs, ps);
  auto vertReflection = Reflect(vs);
  auto fragReflection = Reflect(ps);
  BindGeometry(*device, *context, vs, *vertReflection.Get());
  BindResources(*device, *context, *vertReflection.Get(), true);
  BindResources(*device, *context, *fragReflection.Get(), false);

  D3D11_TEXTURE2D_DESC desc{Side, Side, 1, layers, DXGI_FORMAT_R32G32B32A32_FLOAT,
      {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  Check(device->CreateTexture2D(&desc, nullptr, target.GetAddressOf()), "CreateTexture2D (render)");
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Check(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "CreateTexture2D (staging)");
  ComPtr<ID3D11RenderTargetView> view;
  Check(device->CreateRenderTargetView(target.Get(), nullptr, view.GetAddressOf()), "CreateRenderTargetView");
  context->ClearRenderTargetView(view.Get(), Clear.data());
  context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
  context->DrawInstanced(3, layers, 0, 0);
  context->OMSetRenderTargets(0, nullptr, nullptr);
  context->CopyResource(staging.Get(), target.Get());
  return Readback(*context, *staging.Get(), layers);
}

void CheckFrame(const Frame &frame, UINT layers) {
  for (UINT eye = 0; eye < layers; ++eye) {
    const auto first = frame.begin() + eye * Side * Side;
    const auto end = first + Side * Side;
    const auto count = std::count_if(first, end, [](const auto &p) { return p != Clear; });
    const auto &center = first[Side / 2 + Side * (Side / 2)];
    if (count != Side * Side || center == Clear)
      throw std::runtime_error("incomplete fullscreen render");
    if (std::max({center[0], center[1], center[2]}) <= 0)
      throw std::runtime_error("test geometry did not receive light");
  }
}

float Compare(const Frame &baseline, const Frame &candidate) {
  float maximum = 0;
  for (size_t i = 0; i < baseline.size(); ++i)
    for (size_t c = 0; c < baseline[i].size(); ++c) {
      const float difference = std::abs(baseline[i][c] - candidate[i][c]);
      if (!std::isfinite(difference))
        throw std::runtime_error("nonfinite render");
      maximum = std::max(maximum, difference);
    }
  if (maximum > MatchTolerance)
    throw std::runtime_error("render differs from FXC baseline");
  return maximum;
}
} // namespace

float CompareMoeToonShaders(ID3D11Device &device, ID3D11DeviceContext &context,
                            unsigned layers, const wchar_t *const *paths) {
  const auto baseline = Draw(&device, &context, *Read(paths[0]).Get(),
                             *Read(paths[1]).Get(), layers);
  CheckFrame(baseline, layers);
  const auto candidate = Draw(&device, &context, *Read(paths[2]).Get(),
                              *Read(paths[3]).Get(), layers);
  CheckFrame(candidate, layers);
  return Compare(baseline, candidate);
}

#ifndef DXCB5_UNITY_RENDER_PLUGIN
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc != 4 && argc != 6)
      throw std::invalid_argument("expected mono|stereo BASELINE_VS BASELINE_PS [VS PS]");
    const std::wstring_view variant(argv[1]);
    if (variant != L"mono" && variant != L"stereo")
      throw std::invalid_argument("expected mono|stereo");
    const UINT layers = variant == L"stereo" ? 2 : 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                           context.GetAddressOf()), "D3D11CreateDevice");
    if (argc == 6) {
      float maximum = CompareMoeToonShaders(*device.Get(), *context.Get(),
                                           layers, argv + 2);
      std::printf("%u layers, %u pixels\n", layers, layers * Side * Side);
      std::printf("Maximum absolute RGBA difference: %g\n", maximum);
    } else {
      CheckFrame(Draw(device.Get(), context.Get(), *Read(argv[2]).Get(),
                      *Read(argv[3]).Get(), layers), layers);
      std::printf("%u layers, %u pixels\n", layers, layers * Side * Side);
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
#endif