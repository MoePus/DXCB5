// Distributed under the University of Illinois Open Source License.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using Color = std::array<float, 4>;

namespace {
constexpr UINT TargetSize = 8;
constexpr UINT LayerCount = 2;
constexpr UINT BufferSlot = 4;
constexpr UINT TextureSlot = 3;
constexpr UINT SamplerSlot = 2;
constexpr UINT TextureArraySlot = 11;
constexpr UINT DepthArraySlot = 17;
constexpr UINT GatherArraySlot = 18;
constexpr UINT DimBufferSlot = 19;
constexpr UINT DimMS4Slot = 20;
constexpr UINT DimMS0Slot = 21;
constexpr UINT DimMSASlot = 22;
constexpr UINT CompatTypedSlot = 23;
constexpr UINT CompatStructSlot = 24;
constexpr UINT CompatRawSlot = 25;
constexpr UINT CompatCmpSlot = 26;
constexpr UINT CompatMSSlot = 27;
constexpr UINT CompatCmpSamplerSlot = 9;
constexpr UINT SamplerArraySlot = 5;
constexpr UINT TextureLastOffset = 2;
constexpr UINT SamplerLastOffset = 3;
constexpr UINT QuadLoopIterations = 3;
constexpr UINT QuadLoopSlot = 0;
constexpr float Tolerance = 1e-6f;
constexpr std::array<Color, LayerCount> Expected{{
    {0.015625f, 0.15625f, 0.0625f, 0.1875f},
    {0.265625f, 0.875f, 1.0625f, 0.625f}
}};

void Check(HRESULT result, const char *what) {
  if (FAILED(result)) {
    std::fprintf(stderr, "%s failed (HRESULT 0x%08lx)\n", what,
                 static_cast<unsigned long>(result));
    throw std::runtime_error(what);
  }
}

void Require(bool value, const char *what) {
  if (!value)
    throw std::runtime_error(what);
}

void Compile(const wchar_t *source, const char *entry, const char *profile,
             const wchar_t *output, UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1) {
  ComPtr<ID3DBlob> bytecode, errors;
  const auto result = D3DCompileFromFile(
      source, nullptr, nullptr, entry, profile, flags,
      0, bytecode.GetAddressOf(), errors.GetAddressOf());
  if (FAILED(result) && errors)
    std::fputs(static_cast<const char *>(errors->GetBufferPointer()), stderr);
  Check(result, "D3DCompileFromFile");
  std::ofstream file(output, std::ios::binary);
  file.write(static_cast<const char *>(bytecode->GetBufferPointer()),
             bytecode->GetBufferSize());
  Require(static_cast<bool>(file), "write baseline bytecode");
}

ComPtr<ID3DBlob> Read(const wchar_t *name) {
  ComPtr<ID3DBlob> bytes;
  Check(D3DReadFileToBlob(name, bytes.GetAddressOf()), "D3DReadFileToBlob");
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

void CheckBinding(ID3D11ShaderReflection &shader, const char *name,
                  D3D_SHADER_INPUT_TYPE type, UINT slot, UINT count = 1) {
  D3D11_SHADER_INPUT_BIND_DESC binding{};
  Check(shader.GetResourceBindingDescByName(name, &binding), name);
  Require(binding.Type == type && binding.BindPoint == slot &&
              binding.BindCount == count, name);
}

ID3D11ShaderReflectionVariable *CheckVariable(ID3D11ShaderReflectionConstantBuffer &buffer,
                                              const char *name, UINT offset,
                                              UINT size, UINT columns, UINT elements) {
  auto *variable = buffer.GetVariableByName(name);
  D3D11_SHADER_VARIABLE_DESC desc{};
  Check(variable->GetDesc(&desc), name);
  Require(desc.StartOffset == offset && desc.Size == size &&
              (desc.uFlags & D3D_SVF_USED), name);
  D3D11_SHADER_TYPE_DESC type{};
  Check(variable->GetType()->GetDesc(&type), name);
  Require(type.Class == D3D_SVC_VECTOR && type.Type == D3D_SVT_FLOAT &&
              type.Rows == 1 && type.Columns == columns &&
              type.Elements == elements, name);
  return variable;
}

void CheckNestedTypes(ID3D11ShaderReflectionConstantBuffer &buffer) {
  auto *variable = buffer.GetVariableByName("Materials");
  D3D11_SHADER_VARIABLE_DESC desc{};
  Check(variable->GetDesc(&desc), "Materials");
  Require(desc.StartOffset == 64 && desc.Size == 128 &&
              (desc.uFlags & D3D_SVF_USED), "Materials layout");
  auto *type = variable->GetType();
  D3D11_SHADER_TYPE_DESC structure{}, transform{}, bias{};
  Check(type->GetDesc(&structure), "MaterialData");
  Require(structure.Class == D3D_SVC_STRUCT && structure.Elements == 2 &&
              structure.Members == 2, "MaterialData struct array");
  Check(type->GetMemberTypeByName("transform")->GetDesc(&transform), "transform");
  Require(transform.Class == D3D_SVC_MATRIX_ROWS && transform.Offset == 0 &&
              transform.Rows == 3 && transform.Columns == 3 &&
              transform.Elements == 0, "transform matrix type");
  Check(type->GetMemberTypeByName("bias")->GetDesc(&bias), "bias");
  Require(bias.Class == D3D_SVC_VECTOR && bias.Offset == 48 &&
              bias.Rows == 1 && bias.Columns == 4 && bias.Elements == 0,
          "bias vector type");
}

void CheckReflection(ID3DBlob &vs, ID3DBlob &ps) {
  auto vertex = Reflect(vs);
  Require(vertex->GetRequiresFlags() &
              D3D_SHADER_REQUIRES_VIEWPORT_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER_FEEDING_RASTERIZER,
          "VS missing its render-target array-index feature bit");
  D3D11_SIGNATURE_PARAMETER_DESC previous{};
  for (UINT i = 0; i < 4; ++i) {
    D3D11_SIGNATURE_PARAMETER_DESC current{};
    Check(vertex->GetOutputParameterDesc(i, &current), "GetOutputParameterDesc");
    Require(!i || previous.Register < current.Register ||
                (previous.Register == current.Register && previous.Mask < current.Mask),
            "VS reflected signatures must be sorted by register and mask");
    previous = current;
  }
  auto pixel = Reflect(ps);
  CheckBinding(*pixel.Get(), "TestTexture", D3D_SIT_TEXTURE, TextureSlot);
  CheckBinding(*pixel.Get(), "TestSampler", D3D_SIT_SAMPLER, SamplerSlot);
  CheckBinding(*pixel.Get(), "TestBuffer", D3D_SIT_CBUFFER, BufferSlot);
  auto *buffer = pixel->GetConstantBufferByName("TestBuffer");
  D3D11_SHADER_BUFFER_DESC desc{};
  Check(buffer->GetDesc(&desc), "TestBuffer");
  Require(desc.Type == D3D_CT_CBUFFER && desc.Size == 192 && desc.Variables == 3,
          "TestBuffer reflection layout");
  auto *tint = CheckVariable(*buffer, "TintScale", 8, 8, 2, 0);
  D3D11_SHADER_VARIABLE_DESC tintDesc{};
  Check(tint->GetDesc(&tintDesc), "TintScale");
  const std::array<float, 2> expected{0.25f, 0.5f};
  Require(tintDesc.DefaultValue &&
              std::memcmp(tintDesc.DefaultValue, expected.data(), sizeof(expected)) == 0,
          "TintScale initializer reflection");
  CheckVariable(*buffer, "Colors", 16, 48, 4, 3);
  CheckNestedTypes(*buffer);
}

void CheckResourceArrays(ID3DBlob &ps) {
  auto pixel = Reflect(ps);
  D3D11_SHADER_DESC info{};
  Check(pixel->GetDesc(&info), "GetDesc for resource arrays");
  Require(info.BoundResources == 5, "reflect only the slots used by the shader");
  CheckBinding(*pixel.Get(), "TestTextures[0]", D3D_SIT_TEXTURE, TextureArraySlot);
  CheckBinding(*pixel.Get(), "TestTextures[2]", D3D_SIT_TEXTURE,
               TextureArraySlot + TextureLastOffset);
  CheckBinding(*pixel.Get(), "TestSamplers[0][0]", D3D_SIT_SAMPLER, SamplerArraySlot);
  CheckBinding(*pixel.Get(), "TestSamplers[1][1]", D3D_SIT_SAMPLER,
               SamplerArraySlot + SamplerLastOffset);
  D3D11_SHADER_INPUT_BIND_DESC depth{};
  Check(pixel->GetResourceBindingDescByName("TestDepthArray", &depth), "TestDepthArray");
  Require(depth.BindPoint == DepthArraySlot &&
              depth.Dimension == D3D_SRV_DIMENSION_TEXTURE2DARRAY, "depth array binding");
}

ComPtr<ID3D11Texture2D> CreateTexture(ID3D11Device &device,
                                     const D3D11_TEXTURE2D_DESC &desc,
                                     const D3D11_SUBRESOURCE_DATA *data) {
  ComPtr<ID3D11Texture2D> texture;
  Check(device.CreateTexture2D(&desc, data, texture.GetAddressOf()), "CreateTexture2D");
  return texture;
}

void BindResources(ID3D11Device &device, ID3D11DeviceContext &context) {
  constexpr Color constants[] = {
      {0, 0, 0.25f, 0.5f}, {0.0625f, 0.125f, 0.25f, 0.5f},
      {0.125f, 0.25f, 0.5f, 1.0f}, {0.25f, 0.5f, 1.0f, 0.125f},
      // Each material contains three padded matrix rows and a bias.
      {0, 0, 0, 0}, {0, 0.125f, 0, 0}, {0, 0, 0, 0}, {0.0625f, 0, 0, 0},
      {0, 0, 0, 0}, {0, 0.25f, 0, 0}, {0, 0, 0, 0}, {0.25f, 0, 0, 0}};
  constexpr D3D11_BUFFER_DESC bufferDesc{
      sizeof(constants), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
  const D3D11_SUBRESOURCE_DATA bufferData{constants};
  ComPtr<ID3D11Buffer> buffer;
  Check(device.CreateBuffer(&bufferDesc, &bufferData, buffer.GetAddressOf()),
        "CreateBuffer");
  context.PSSetConstantBuffers(BufferSlot, 1, buffer.GetAddressOf());

  constexpr Color texels[] = {
      {0.125f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.25f, 0.0f, 0.0f},
      {0.0f, 0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
  constexpr Color mip1{0, 0, 0.0625f, 0};
  constexpr D3D11_TEXTURE2D_DESC textureDesc{
      2, 2, 2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
      D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
  const D3D11_SUBRESOURCE_DATA data[] = {
      {texels, 2 * sizeof(Color), 0}, {&mip1, sizeof(Color), 0}};
  auto texture = CreateTexture(device, textureDesc, data);
  ComPtr<ID3D11ShaderResourceView> view;
  Check(device.CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()),
        "CreateShaderResourceView");
  context.PSSetShaderResources(TextureSlot, 1, view.GetAddressOf());

  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> state;
  Check(device.CreateSamplerState(&sampler, state.GetAddressOf()), "CreateSamplerState");
  context.PSSetSamplers(SamplerSlot, 1, state.GetAddressOf());
}

void BindResourceArrays(ID3D11Device &device, ID3D11DeviceContext &context) {
  constexpr UINT Width = 2, Height = 4, Mips = 2;
  constexpr D3D11_TEXTURE2D_DESC textureDesc{
      Width, Height, Mips, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
      D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
  constexpr Color texels[] = {{0.125f, 0.25f, 0.5f, 1},
                             {1, 0.5f, 0.25f, 0.125f}};
  constexpr Color borders[] = {{0.25f, 0.5f, 0.75f, 1},
                              {1, 0.75f, 0.5f, 0.25f}};
  constexpr std::array<UINT, 2> textureOffsets{0, TextureLastOffset};
  constexpr std::array<UINT, 2> samplerOffsets{0, SamplerLastOffset};
  for (UINT i = 0; i < textureOffsets.size(); ++i) {
    std::array<Color, Width * Height> mipPixels;
    mipPixels.fill(texels[i]);
    const D3D11_SUBRESOURCE_DATA data[] = {
        {mipPixels.data(), Width * sizeof(Color), 0},
        {mipPixels.data(), Width / 2 * sizeof(Color), 0}};
    auto texture = CreateTexture(device, textureDesc, data);
    ComPtr<ID3D11ShaderResourceView> view;
    Check(device.CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()),
          "CreateShaderResourceView for array");
    context.PSSetShaderResources(TextureArraySlot + textureOffsets[i], 1,
                                 view.GetAddressOf());

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    std::memcpy(sampler.BorderColor, borders[i].data(), sizeof(Color));
    ComPtr<ID3D11SamplerState> state;
    Check(device.CreateSamplerState(&sampler, state.GetAddressOf()),
          "CreateSamplerState for array");
    context.PSSetSamplers(SamplerArraySlot + samplerOffsets[i], 1, state.GetAddressOf());
  }

  constexpr D3D11_TEXTURE2D_DESC depthDesc{
      4, 2, 2, 3, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
      D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
  auto depth = CreateTexture(device, depthDesc, nullptr);
  ComPtr<ID3D11ShaderResourceView> depthView;
  Check(device.CreateShaderResourceView(depth.Get(), nullptr, depthView.GetAddressOf()),
        "CreateShaderResourceView for depth array");
  context.PSSetShaderResources(DepthArraySlot, 1, depthView.GetAddressOf());
}

void BindGatherArray(ID3D11Device &device, ID3D11DeviceContext &context) {
  constexpr UINT Width = 4, Height = 2;
  // Each slice has distinct texels and channels. The offset skips column 0.
  Color texels[LayerCount][Width * Height];
  for (UINT slice = 0; slice != LayerCount; ++slice)
    for (UINT i = 0; i != Width * Height; ++i)
      texels[slice][i] = {0, static_cast<float>(slice * Width * Height + i), 0, 0};
  const D3D11_SUBRESOURCE_DATA data[] = {
      {texels[0], Width * sizeof(Color), 0},
      {texels[1], Width * sizeof(Color), 0}};
  constexpr D3D11_TEXTURE2D_DESC desc{
      Width, Height, 1, LayerCount, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
      D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
  auto texture = CreateTexture(device, desc, data);
  ComPtr<ID3D11ShaderResourceView> view;
  Check(device.CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf()),
        "CreateShaderResourceView for gather");
  context.PSSetShaderResources(GatherArraySlot, 1, view.GetAddressOf());
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
  context.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D11_VIEWPORT viewport{0, 0, TargetSize, TargetSize, 0, 1};
  context.RSSetViewports(1, &viewport);
  D3D11_RASTERIZER_DESC rasterDesc{};
  rasterDesc.FillMode = D3D11_FILL_SOLID;
  rasterDesc.CullMode = D3D11_CULL_NONE;
  rasterDesc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  Check(device.CreateRasterizerState(&rasterDesc, raster.GetAddressOf()),
        "CreateRasterizerState");
  context.RSSetState(raster.Get());
}

constexpr Color ClearColor{-1, -1, -1, -1};

void CheckPixel(UINT layer, UINT x, const Color &actual, const Color &expected) {
  for (size_t c = 0; c != actual.size(); ++c) {
    if (std::abs(actual[c] - expected[c]) <= Tolerance)
      continue;
    std::fprintf(stderr, "Layer %u pixel x=%u channel %zu: got %g, expected %g\n",
                 layer, x, c, actual[c], expected[c]);
    throw std::runtime_error("WARP readback");
  }
}

enum class DrawMode {
  Standard, Discard, ResourceArrays, QuadLoop, LocalArrays, Gather, LoopLanes,
  RetryLoop, Sign, Saturate, LocalConstantTuples, Correctness, Dimensions,
  Compat, Compat2, Compat3, Compat4
};

void BindDimensionResources(ID3D11Device &device, ID3D11DeviceContext &context) {
  std::vector<Color> bufData(17, {1, 2, 3, 4});
  D3D11_BUFFER_DESC bdesc{static_cast<UINT>(bufData.size() * sizeof(Color)),
                          D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
  D3D11_SUBRESOURCE_DATA binit{bufData.data()};
  ComPtr<ID3D11Buffer> buf;
  Check(device.CreateBuffer(&bdesc, &binit, buf.GetAddressOf()), "CreateBuffer (dim)");
  D3D11_SHADER_RESOURCE_VIEW_DESC bsrv{};
  bsrv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  bsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  bsrv.Buffer.NumElements = static_cast<UINT>(bufData.size());
  ComPtr<ID3D11ShaderResourceView> bufView;
  Check(device.CreateShaderResourceView(buf.Get(), &bsrv, bufView.GetAddressOf()),
        "CreateSRV (dim buffer)");
  context.PSSetShaderResources(DimBufferSlot, 1, bufView.GetAddressOf());

  D3D11_TEXTURE2D_DESC msDesc{8, 6, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {4, 0},
                              D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
  auto msTex = CreateTexture(device, msDesc, nullptr);
  D3D11_SHADER_RESOURCE_VIEW_DESC msSrv{};
  msSrv.Format = msDesc.Format;
  msSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
  ComPtr<ID3D11ShaderResourceView> msView;
  Check(device.CreateShaderResourceView(msTex.Get(), &msSrv, msView.GetAddressOf()),
        "CreateSRV (dim MS)");
  context.PSSetShaderResources(DimMS4Slot, 1, msView.GetAddressOf());
  context.PSSetShaderResources(DimMS0Slot, 1, msView.GetAddressOf());

  D3D11_TEXTURE2D_DESC msaDesc{4, 2, 1, 3, DXGI_FORMAT_R32G32B32A32_FLOAT, {4, 0},
                               D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
  auto msaTex = CreateTexture(device, msaDesc, nullptr);
  D3D11_SHADER_RESOURCE_VIEW_DESC msaSrv{};
  msaSrv.Format = msaDesc.Format;
  msaSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
  msaSrv.Texture2DMSArray.ArraySize = 3;
  ComPtr<ID3D11ShaderResourceView> msaView;
  Check(device.CreateShaderResourceView(msaTex.Get(), &msaSrv, msaView.GetAddressOf()),
        "CreateSRV (dim MSA)");
  context.PSSetShaderResources(DimMSASlot, 1, msaView.GetAddressOf());
}

void BindCompatResources(ID3D11Device &device, ID3D11DeviceContext &context) {
  std::array<Color, 2> tbData{{{10.5f, 1.0f, 2.0f, 3.0f}, {20.25f, 4.0f, 5.0f, 6.0f}}};
  D3D11_BUFFER_DESC tbd{static_cast<UINT>(sizeof(tbData)), D3D11_USAGE_IMMUTABLE,
                        D3D11_BIND_SHADER_RESOURCE};
  D3D11_SUBRESOURCE_DATA tbi{tbData.data()};
  ComPtr<ID3D11Buffer> tb;
  Check(device.CreateBuffer(&tbd, &tbi, tb.GetAddressOf()), "CreateBuffer (compat typed)");
  D3D11_SHADER_RESOURCE_VIEW_DESC tbsrv{};
  tbsrv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  tbsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  tbsrv.Buffer.NumElements = 2;
  ComPtr<ID3D11ShaderResourceView> tbView;
  Check(device.CreateShaderResourceView(tb.Get(), &tbsrv, tbView.GetAddressOf()),
        "CreateSRV (compat typed)");
  context.PSSetShaderResources(CompatTypedSlot, 1, tbView.GetAddressOf());

  struct CompatItem {
    float a[4];
    uint32_t b[2];
  };
  std::array<CompatItem, 3> sbData{{
      {{1.0f, 2.5f, 3.0f, 4.0f}, {100u, 200u}},
      {{5.0f, 6.75f, 7.0f, 8.0f}, {300u, 400u}},
      {{9.0f, 10.0f, 11.0f, 12.0f}, {500u, 600u}},
  }};
  D3D11_BUFFER_DESC sbd{static_cast<UINT>(sizeof(sbData)), D3D11_USAGE_IMMUTABLE,
                        D3D11_BIND_SHADER_RESOURCE, 0,
                        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(CompatItem)};
  D3D11_SUBRESOURCE_DATA sbi{sbData.data()};
  ComPtr<ID3D11Buffer> sb;
  Check(device.CreateBuffer(&sbd, &sbi, sb.GetAddressOf()), "CreateBuffer (compat struct)");
  D3D11_SHADER_RESOURCE_VIEW_DESC sbsrv{};
  sbsrv.Format = DXGI_FORMAT_UNKNOWN;
  sbsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  sbsrv.Buffer.NumElements = 3;
  ComPtr<ID3D11ShaderResourceView> sbView;
  Check(device.CreateShaderResourceView(sb.Get(), &sbsrv, sbView.GetAddressOf()),
        "CreateSRV (compat struct)");
  context.PSSetShaderResources(CompatStructSlot, 1, sbView.GetAddressOf());

  std::array<uint32_t, 4> rawData{11u, 22u, 33u, 44u};
  D3D11_BUFFER_DESC rbd{static_cast<UINT>(sizeof(rawData)), D3D11_USAGE_IMMUTABLE,
                        D3D11_BIND_SHADER_RESOURCE, 0,
                        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
  D3D11_SUBRESOURCE_DATA rbi{rawData.data()};
  ComPtr<ID3D11Buffer> rb;
  Check(device.CreateBuffer(&rbd, &rbi, rb.GetAddressOf()), "CreateBuffer (compat raw)");
  D3D11_SHADER_RESOURCE_VIEW_DESC rbsrv{};
  rbsrv.Format = DXGI_FORMAT_R32_TYPELESS;
  rbsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  rbsrv.BufferEx.NumElements = 4;
  rbsrv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  ComPtr<ID3D11ShaderResourceView> rbView;
  Check(device.CreateShaderResourceView(rb.Get(), &rbsrv, rbView.GetAddressOf()),
        "CreateSRV (compat raw)");
  context.PSSetShaderResources(CompatRawSlot, 1, rbView.GetAddressOf());
}

void BindCompat2Resources(ID3D11Device &device, ID3D11DeviceContext &context) {
  BindGatherArray(device, context);
  // 4x2 depth-like texture: cols 0..1 = 0.0, cols 2..3 = 1.0.
  const float cmpTexels[8] = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f};
  D3D11_TEXTURE2D_DESC cmpDesc{4, 2, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
                               D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
  D3D11_SUBRESOURCE_DATA cmpInit{cmpTexels, 4 * sizeof(float), 0};
  auto cmpTex = CreateTexture(device, cmpDesc, &cmpInit);
  ComPtr<ID3D11ShaderResourceView> cmpView;
  Check(device.CreateShaderResourceView(cmpTex.Get(), nullptr, cmpView.GetAddressOf()),
        "CreateSRV (compat cmp)");
  context.PSSetShaderResources(CompatCmpSlot, 1, cmpView.GetAddressOf());

  D3D11_SAMPLER_DESC cs{};
  cs.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
  cs.AddressU = cs.AddressV = cs.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  cs.ComparisonFunc = D3D11_COMPARISON_LESS;
  cs.MaxLOD = D3D11_FLOAT32_MAX;
  ComPtr<ID3D11SamplerState> cmpSampler;
  Check(device.CreateSamplerState(&cs, cmpSampler.GetAddressOf()),
        "CreateSamplerState (compat cmp)");
  context.PSSetSamplers(CompatCmpSamplerSlot, 1, cmpSampler.GetAddressOf());

  // 4x MSAA texture cleared to {3.5, 4.5, 5.5, 6.5} for ldms verification.
  D3D11_TEXTURE2D_DESC msDesc{
      2, 2, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {4, 0}, D3D11_USAGE_DEFAULT,
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
  auto msTex = CreateTexture(device, msDesc, nullptr);
  ComPtr<ID3D11RenderTargetView> msRtv;
  Check(device.CreateRenderTargetView(msTex.Get(), nullptr, msRtv.GetAddressOf()),
        "CreateRTV (compat ms)");
  const Color msClear{3.5f, 4.5f, 5.5f, 6.5f};
  context.ClearRenderTargetView(msRtv.Get(), msClear.data());
  D3D11_SHADER_RESOURCE_VIEW_DESC msSrv{};
  msSrv.Format = msDesc.Format;
  msSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
  ComPtr<ID3D11ShaderResourceView> msView;
  Check(device.CreateShaderResourceView(msTex.Get(), &msSrv, msView.GetAddressOf()),
        "CreateSRV (compat ms)");
  context.PSSetShaderResources(CompatMSSlot, 1, msView.GetAddressOf());
}

void VerifyDxbc(ID3DBlob &vs, ID3DBlob &ps, int argc, wchar_t **argv) {
  ComPtr<ID3DBlob> asmBlob;
  Check(D3DDisassemble(ps.GetBufferPointer(), ps.GetBufferSize(), 0, nullptr,
                       asmBlob.GetAddressOf()), "D3DDisassemble");
  (void)Reflect(ps);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                          D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                          context.GetAddressOf()), "D3D11CreateDevice (verify)");
  BindShaders(*device.Get(), *context.Get(), vs, ps);
  const std::string text(static_cast<const char *>(asmBlob->GetBufferPointer()),
                         asmBlob->GetBufferSize());
  for (int i = 4; i < argc; ++i) {
    std::string needle;
    for (const wchar_t *p = argv[i]; *p; ++p)
      needle.push_back(static_cast<char>(*p));
    if (text.find(needle) == std::string::npos) {
      std::fprintf(stderr, "Missing expected SM5 token '%s' in disassembly:\n%s\n",
                   needle.c_str(), text.c_str());
      throw std::runtime_error("verify-dxbc");
    }
  }
}

void BindQuadLoopControl(ID3D11Device &device, ID3D11DeviceContext &context) {
  const std::array<UINT, 4> constants{QuadLoopIterations};
  constexpr D3D11_BUFFER_DESC desc{
      sizeof(constants), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
  const D3D11_SUBRESOURCE_DATA data{constants.data()};
  ComPtr<ID3D11Buffer> buffer;
  Check(device.CreateBuffer(&desc, &data, buffer.GetAddressOf()),
        "CreateBuffer (quad loop)");
  context.PSSetConstantBuffers(QuadLoopSlot, 1, buffer.GetAddressOf());
}

Color ExpectedPixel(DrawMode mode, UINT layer) {
  if (mode == DrawMode::Gather)
    return layer ? Color{13, 14, 10, 9.125f} : Color{5, 6, 2, 1.125f};
  if (mode == DrawMode::ResourceArrays)
    return layer ? Color{4.125f, 5.5f, 4.75f, 20.25f}
                 : Color{6.125f, 6.5f, 5.75f, 22.25f};
  if (mode == DrawMode::QuadLoop) {
    const UINT iterations = layer * QuadLoopIterations;
    const auto derivative = static_cast<float>((1u << iterations) - 1);
    return {derivative, iterations ? 0.0625f : 0, 0, 1};
  }
  if (mode == DrawMode::Dimensions)
    return {17.0f + 8.0f + 8.0f + 4.0f, 6.0f + 6.0f + 2.0f, 3.0f, 4.0f + 4.0f + 4.0f};
  return Expected[layer];
}

void CheckLoopLanes(UINT layer, UINT center, DrawMode mode,
                    const std::array<Color, TargetSize / 2> &pixels) {
  constexpr std::array<Color, 4> lanes{{
      {1, 2, 3, 4.5f}, {2, 4, 6, 5.5f}, {2, 4, 6, 6.5f}, {11, 7, 12, 7.5f}}};
  constexpr std::array<Color, 4> retry{{
      {0, 0, 0, 4.5f}, {4, 8, 12, 5.5f}, {18, 6, 12, 6.5f}, {24, 8, 16, 7.5f}}};
  static_assert(lanes.size() == pixels.size(), "Test all four columns");
  const auto &expected = mode == DrawMode::LoopLanes ? lanes : retry;
  for (UINT i = 0; i != pixels.size(); ++i)
    CheckPixel(layer, center + i, pixels[i], expected[i]);
}

void CheckSign(UINT layer, UINT center,
               const std::array<Color, TargetSize / 2> &pixels) {
  for (UINT i = 0; i != pixels.size(); ++i) {
    const auto column = center + i;
    const bool odd = (column & 1u) != 0;
    const Color expected{0, 0, odd ? -1.f : 1.f, odd ? 0.f : 1.f};
    CheckPixel(layer, column, pixels[i], expected);
    if (std::signbit(pixels[i][0]) || std::signbit(pixels[i][1]))
      throw std::runtime_error("Ordered sign produced negative zero");
  }
}

void CheckSaturate(UINT layer, UINT center,
                   const std::array<Color, TargetSize / 2> &pixels) {
  constexpr std::array<Color, 4> expected{{
      {0, 0, 0, -2.75f}, {0, 0, 0, -1.75f}, {0, 0, 0.5f, -0.75f}, {0, 0, 1, 1.5f}}};
  static_assert(TargetSize / 2 == expected.size(), "Test all four columns");
  for (UINT i = 0; i != pixels.size(); ++i) {
    const auto &actual = pixels[i];
    if (std::memcmp(actual.data(), expected[i].data(), sizeof(float) * actual.size()) == 0)
      continue;
    CheckPixel(layer, center + i, actual, expected[i]);
    std::fprintf(stderr, "Layer %u pixel x=%u differs at the bit level\n",
                 layer, center + i);
    throw std::runtime_error("WARP saturation bits");
  }
}

void CheckReadback(ID3D11DeviceContext &context, ID3D11Texture2D &staging,
                   DrawMode mode) {
  constexpr Color localArrays[] = {
      {4.5f, 4.5f, 10.25f, 1}, {16.5f, 7.25f, 38.5f, 1},
      {4.5f, 8.5f, 18, 1}, {16.5f, 10.5f, 27, 1}};
  for (UINT layer = 0; layer != LayerCount; ++layer) {
    D3D11_MAPPED_SUBRESOURCE map{};
    Check(context.Map(&staging, layer, D3D11_MAP_READ, 0, &map), "Map (readback)");
    constexpr UINT center = TargetSize / 2;
    static_assert(center % 2 == 0, "Discard fixture starts on an even column");
    const auto *data = static_cast<const char *>(map.pData) + center * map.RowPitch;
    std::array<Color, TargetSize - center> pixels;
    std::memcpy(pixels.data(), data + center * sizeof(Color), sizeof(pixels));
    context.Unmap(&staging, layer);
    if (mode == DrawMode::LoopLanes || mode == DrawMode::RetryLoop) {
      CheckLoopLanes(layer, center, mode, pixels);
      continue;
    }
    if (mode == DrawMode::Saturate) {
      CheckSaturate(layer, center, pixels);
      continue;
    }
    if (mode == DrawMode::Sign) {
      CheckSign(layer, center, pixels);
      continue;
    }
    if (mode == DrawMode::LocalConstantTuples) {
      constexpr Color Expected[] = {
          {2, 6, 9, 6}, {8, 15, 19, 12},
          {8, 12, 15, 12}, {14, 21, 25, 18}};
      static_assert(pixels.size() == std::size(Expected), "Cover both index boundaries");
      for (UINT i = 0; i != pixels.size(); ++i)
        CheckPixel(layer, center + i, pixels[i], Expected[i]);
      continue;
    }
    if (mode == DrawMode::LocalArrays) {
      static_assert(pixels.size() == std::size(localArrays), "Test all four array indices");
      for (UINT i = 0; i != pixels.size(); ++i)
        CheckPixel(layer, center + i, pixels[i], localArrays[i]);
      continue;
    }
    if (mode == DrawMode::Correctness) {
      if (layer == 0) {
        CheckPixel(layer, center + 0, pixels[0], {3.5f, 2.5f, 1.5f, 0.5f});
      } else {
        // x=4.5, y=4.5: ux=616+0x12345=0x125ad (75181), uy=238+0x9876=0x9964 (39268)
        // ub=(0x125ad>>7)&31=11, bf=((0x125ad<<2)&4)|((0x9964&~992)|((0x125ad<<5)&992))=0x99a4 (39332)
        // subV=(75181-39268)&0xffff=35913
        CheckPixel(layer, center + 0, pixels[0], {0.0f, 11.0f, 39332.0f, 35913.0f});
      }
      CheckPixel(layer, center + 1, pixels[1],
                 {layer ? 0.0f : 1.0f, layer ? 1.0f : 0.0f,
                  layer ? 2.0f : 1.0f, 99.0f});
      if (layer == 0) {
        Require(std::isnan(pixels[2][0]) && std::isnan(pixels[2][1]) &&
                    std::isnan(pixels[2][2]),
                "Expected NaN for pow(-2,2), dot(normalize(0)), and (2<NaN?2:NaN)");
        CheckPixel(layer, center + 2, {0, 0, 0, pixels[2][3]}, {0, 0, 0, 0.0f});
      } else {
        Require(std::isnan(pixels[2][0]) && std::isnan(pixels[2][2]),
                "Expected NaN for (false?1:0)*NaN and (2<NaN?2:NaN)");
        CheckPixel(layer, center + 2, {0, pixels[2][1], 0, pixels[2][3]},
                   {0, 1.0f, 0, 33554432.0f});
      }
      CheckPixel(layer, center + 3, pixels[3],
                 {layer ? 20.0f : 42.0f, layer ? 30.0f : 15.0f,
                  layer ? 7.0f : 55.0f, layer ? 55.0f : 8.0f});
      continue;
    }
    if (mode == DrawMode::Compat) {
      // col=0 (depth 0.25 passes): layer 0 -> (-7/3=-2, -7%3=-1, 7/-3=-2, 7%-3=1)
      //                            layer 1 -> (INT32_MIN/-2=1073741824, INT32_MIN%3=-2, -7/-3=2, -7%-3=-1)
      CheckPixel(layer, center + 0, pixels[0],
                 layer == 0 ? Color{-2.0f, -1.0f, -2.0f, 1.0f}
                            : Color{1073741824.0f, -2.0f, 2.0f, -1.0f});
      // col=1 (depth 0.75 rejected by depth<0.5): remains ClearColor.
      CheckPixel(layer, center + 1, pixels[1], ClearColor);
      // col=2 (depth 0.25 passes): tbv.x+sbv.a.y, sbv.b.y+bbv.x, bbv.y, 3+24+16=43.
      CheckPixel(layer, center + 2, pixels[2],
                 layer == 0 ? Color{13.0f, 211.0f, 22.0f, 43.0f}
                            : Color{27.0f, 433.0f, 44.0f, 43.0f});
      // col=3 (depth 0.75 rejected): remains ClearColor.
      CheckPixel(layer, center + 3, pixels[3], ClearColor);
      continue;
    }
    if (mode == DrawMode::Compat2) {
      // col=0: AddUint64(0xfffffffe,10 + 3+eye,20) = (1+eye,31), AddUint64(5+eye,10 + 3,20) = (8+eye,30).
      CheckPixel(layer, center + 0, pixels[0],
                 {layer ? 2.0f : 1.0f, 31.0f, layer ? 9.0f : 8.0f, 30.0f});
      // col=1: GatherGreen with dynamic offset (1+eye, 0) on GatherArray (4x2).
      CheckPixel(layer, center + 1, pixels[1],
                 layer == 0 ? Color{5.0f, 6.0f, 2.0f, 1.0f}
                            : Color{14.0f, 15.0f, 11.0f, 10.0f});
      // col=2: GatherCmpRed (offset 1,0 -> 2 of 4 pass) + GatherCmpRed_PO (offset 1+eye,0 -> 2 or 4 pass) + lod.
      CheckPixel(layer, center + 2, pixels[2],
                 {2.0f, layer == 0 ? 2.0f : 4.0f, 0.0f, -8.0f});
      // col=3: ldms {3.5,4.5,5.5,6.5} + eval_centroid/sample/snapped of tint(0.0625,0.125) + SV_SampleIndex(0).
      CheckPixel(layer, center + 3, pixels[3], {3.5625f, 4.625f, 5.5625f, 6.5f});
      continue;
    }
    if (mode == DrawMode::Compat3) {
      // col=0: min16float(2.0), min16int(-5>>1=-3 or -4>>1=-2), (-5<-4?10:20), min16uint(0xfff0>>4=4095).
      CheckPixel(layer, center + 0, pixels[0],
                 {2.0f, layer ? -2.0f : -3.0f, layer ? 20.0f : 10.0f, 4095.0f});
      // col=1: CompatUavTyped[eye] + InterlockedAdd(u2[eye,0], 5) returning initial 100+eye.
      CheckPixel(layer, center + 1, pixels[1],
                 layer == 0 ? Color{1.5f, 2.5f, 3.5f, 100.0f}
                            : Color{5.5f, 6.5f, 7.5f, 101.0f});
      // col=2: CompatUavStruct[eye].a sum, InterlockedCmpExch old (7 or 8), raw Load2 sum, dims(2+12+32=46).
      CheckPixel(layer, center + 2, pixels[2],
                 layer == 0 ? Color{3.5f, 7.0f, 30.0f, 46.0f}
                            : Color{7.5f, 8.0f, 70.0f, 46.0f});
      // col=3: IncrementCounter() returns 1 (initial counter=1) for layer 0, 0 for layer 1.
      CheckPixel(layer, center + 3, pixels[3],
                 {layer == 0 ? 1.0f : 0.0f, 1.0f, 2.0f, 3.0f});
      continue;
    }
    if (mode == DrawMode::Compat4) {
      // Even cols keep cov=1 and read mutated gMutableArr (layer 0: {35,20}, layer 1: {10,45}) + bool array {1,0,1,0}.
      // Odd cols write oMask=0 and stay ClearColor.
      const Color evenExp = layer == 0 ? Color{35.0f, 20.0f, 1.0f, 1.0f}
                                       : Color{10.0f, 45.0f, 1.0f, 1.0f};
      CheckPixel(layer, center + 0, pixels[0], evenExp);
      CheckPixel(layer, center + 1, pixels[1], ClearColor);
      CheckPixel(layer, center + 2, pixels[2], evenExp);
      CheckPixel(layer, center + 3, pixels[3], ClearColor);
      continue;
    }
    const Color expected = ExpectedPixel(mode, layer);
    CheckPixel(layer, center, pixels[0], mode == DrawMode::Discard ? ClearColor : expected);
    CheckPixel(layer, center + 1, pixels[1], expected);
  }
}

void Draw(ID3DBlob &vs, ID3DBlob &ps, DrawMode mode) {
  if (mode == DrawMode::ResourceArrays)
    CheckResourceArrays(ps);
  else if (mode == DrawMode::Standard || mode == DrawMode::Discard)
    CheckReflection(vs, ps);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                         context.GetAddressOf()), "D3D11CreateDevice");
  BindShaders(*device.Get(), *context.Get(), vs, ps);
  if (mode == DrawMode::ResourceArrays)
    BindResourceArrays(*device.Get(), *context.Get());
  else
    BindResources(*device.Get(), *context.Get());
  if (mode == DrawMode::QuadLoop)
    BindQuadLoopControl(*device.Get(), *context.Get());
  if (mode == DrawMode::Gather)
    BindGatherArray(*device.Get(), *context.Get());
  if (mode == DrawMode::Dimensions)
    BindDimensionResources(*device.Get(), *context.Get());
  if (mode == DrawMode::Compat)
    BindCompatResources(*device.Get(), *context.Get());
  if (mode == DrawMode::Compat2)
    BindCompat2Resources(*device.Get(), *context.Get());
  D3D11_TEXTURE2D_DESC desc{
      TargetSize, TargetSize, 1, LayerCount, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0},
      D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  auto target = CreateTexture(*device.Get(), desc, nullptr);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  auto staging = CreateTexture(*device.Get(), desc, nullptr);
  ComPtr<ID3D11RenderTargetView> view;
  Check(device->CreateRenderTargetView(target.Get(), nullptr, view.GetAddressOf()),
        "CreateRenderTargetView");
  ComPtr<ID3D11DepthStencilView> dsv;
  if (mode == DrawMode::Compat) {
    D3D11_TEXTURE2D_DESC dsDesc{
        TargetSize, TargetSize, 1, LayerCount, DXGI_FORMAT_D32_FLOAT, {1, 0},
        D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL};
    auto dsTex = CreateTexture(*device.Get(), dsDesc, nullptr);
    Check(device->CreateDepthStencilView(dsTex.Get(), nullptr, dsv.GetAddressOf()),
          "CreateDepthStencilView");
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
    D3D11_DEPTH_STENCIL_DESC dssDesc{};
    dssDesc.DepthEnable = TRUE;
    dssDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dssDesc.DepthFunc = D3D11_COMPARISON_LESS;
    ComPtr<ID3D11DepthStencilState> dss;
    Check(device->CreateDepthStencilState(&dssDesc, dss.GetAddressOf()),
          "CreateDepthStencilState");
    context->OMSetDepthStencilState(dss.Get(), 0);
  }
  context->ClearRenderTargetView(view.Get(), ClearColor.data());
  if (mode == DrawMode::Compat3) {
    // Create and bind 5 UAVs (u1..u5): typed buffer, R32_UINT 2D tex, structured, raw, counter.
    std::array<Color, 4> tbInit{{{1.5f, 2.5f, 3.5f, 4.5f},
                                 {5.5f, 6.5f, 7.5f, 8.5f},
                                 {0, 0, 0, 0},
                                 {0, 0, 0, 0}}};
    D3D11_BUFFER_DESC tbd{static_cast<UINT>(sizeof(tbInit)), D3D11_USAGE_DEFAULT,
                          D3D11_BIND_UNORDERED_ACCESS};
    D3D11_SUBRESOURCE_DATA tbi{tbInit.data()};
    ComPtr<ID3D11Buffer> tb;
    Check(device->CreateBuffer(&tbd, &tbi, tb.GetAddressOf()), "CreateBuffer (u1)");
    D3D11_UNORDERED_ACCESS_VIEW_DESC u1d{};
    u1d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    u1d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u1d.Buffer.NumElements = 4;
    ComPtr<ID3D11UnorderedAccessView> u1;
    Check(device->CreateUnorderedAccessView(tb.Get(), &u1d, u1.GetAddressOf()), "CreateUAV (u1)");

    const uint32_t texInit[2] = {100u, 101u};
    D3D11_TEXTURE2D_DESC td{2, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                            D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
    D3D11_SUBRESOURCE_DATA ti{texInit, sizeof(texInit), 0};
    auto u2Tex = CreateTexture(*device.Get(), td, &ti);
    D3D11_UNORDERED_ACCESS_VIEW_DESC u2d{};
    u2d.Format = DXGI_FORMAT_R32_UINT;
    u2d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11UnorderedAccessView> u2;
    Check(device->CreateUnorderedAccessView(u2Tex.Get(), &u2d, u2.GetAddressOf()), "CreateUAV (u2)");

    struct UavItem { float a[2]; uint32_t b; };
    std::array<UavItem, 2> stInit{{{{1.0f, 2.5f}, 7u}, {{3.0f, 4.5f}, 8u}}};
    D3D11_BUFFER_DESC sbd{static_cast<UINT>(sizeof(stInit)), D3D11_USAGE_DEFAULT,
                          D3D11_BIND_UNORDERED_ACCESS, 0,
                          D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(UavItem)};
    D3D11_SUBRESOURCE_DATA sbi{stInit.data()};
    ComPtr<ID3D11Buffer> sb;
    Check(device->CreateBuffer(&sbd, &sbi, sb.GetAddressOf()), "CreateBuffer (u3)");
    D3D11_UNORDERED_ACCESS_VIEW_DESC u3d{};
    u3d.Format = DXGI_FORMAT_UNKNOWN;
    u3d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u3d.Buffer.NumElements = 2;
    ComPtr<ID3D11UnorderedAccessView> u3;
    Check(device->CreateUnorderedAccessView(sb.Get(), &u3d, u3.GetAddressOf()), "CreateUAV (u3)");

    std::array<uint32_t, 8> rawInit{10u, 20u, 30u, 40u, 0u, 0u, 0u, 0u};
    D3D11_BUFFER_DESC rbd{static_cast<UINT>(sizeof(rawInit)), D3D11_USAGE_DEFAULT,
                          D3D11_BIND_UNORDERED_ACCESS, 0,
                          D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    D3D11_SUBRESOURCE_DATA rbi{rawInit.data()};
    ComPtr<ID3D11Buffer> rb;
    Check(device->CreateBuffer(&rbd, &rbi, rb.GetAddressOf()), "CreateBuffer (u4)");
    D3D11_UNORDERED_ACCESS_VIEW_DESC u4d{};
    u4d.Format = DXGI_FORMAT_R32_TYPELESS;
    u4d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u4d.Buffer.NumElements = 8;
    u4d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11UnorderedAccessView> u4;
    Check(device->CreateUnorderedAccessView(rb.Get(), &u4d, u4.GetAddressOf()), "CreateUAV (u4)");

    std::array<uint32_t, 4> cntInit{0u, 0u, 0u, 0u};
    D3D11_BUFFER_DESC cbd{static_cast<UINT>(sizeof(cntInit)), D3D11_USAGE_DEFAULT,
                          D3D11_BIND_UNORDERED_ACCESS, 0,
                          D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(uint32_t)};
    D3D11_SUBRESOURCE_DATA cbi{cntInit.data()};
    ComPtr<ID3D11Buffer> cb;
    Check(device->CreateBuffer(&cbd, &cbi, cb.GetAddressOf()), "CreateBuffer (u5)");
    D3D11_UNORDERED_ACCESS_VIEW_DESC u5d{};
    u5d.Format = DXGI_FORMAT_UNKNOWN;
    u5d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u5d.Buffer.NumElements = 4;
    u5d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> u5;
    Check(device->CreateUnorderedAccessView(cb.Get(), &u5d, u5.GetAddressOf()), "CreateUAV (u5)");

    ID3D11UnorderedAccessView *uavs[5] = {u1.Get(), u2.Get(), u3.Get(), u4.Get(), u5.Get()};
    const UINT initCounts[5] = {0, 0, 0, 0, 1};
    context->OMSetRenderTargetsAndUnorderedAccessViews(
        1, view.GetAddressOf(), nullptr, 1, 5, uavs, initCounts);
    context->DrawInstanced(3, LayerCount, 0, 0);
    context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
    context->CopyResource(staging.Get(), target.Get());
    CheckReadback(*context.Get(), *staging.Get(), mode);

    // Verify UAV side effects by reading back u1, u3, u4, u5!
    auto readBuf = [&](ID3D11Buffer *src, void *dst, UINT bytes) {
      D3D11_BUFFER_DESC sd{bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
      ComPtr<ID3D11Buffer> st;
      Check(device->CreateBuffer(&sd, nullptr, st.GetAddressOf()), "CreateBuffer (uav staging)");
      context->CopyResource(st.Get(), src);
      D3D11_MAPPED_SUBRESOURCE m{};
      Check(context->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m), "Map (uav staging)");
      std::memcpy(dst, m.pData, bytes);
      context->Unmap(st.Get(), 0);
    };
    std::array<Color, 4> tbOut{};
    readBuf(tb.Get(), tbOut.data(), sizeof(tbOut));
    CheckPixel(0, 2, tbOut[2], {10.0f, 20.0f, 30.0f, 40.0f});
    CheckPixel(1, 3, tbOut[3], {11.0f, 20.0f, 30.0f, 40.0f});
    std::array<UavItem, 2> stOut{};
    readBuf(sb.Get(), stOut.data(), sizeof(stOut));
    Require(stOut[0].b == 99u && stOut[1].b == 8u, "UAV InterlockedCompareExchange mismatch");
    std::array<uint32_t, 8> rawOut{};
    readBuf(rb.Get(), rawOut.data(), sizeof(rawOut));
    Require(rawOut[4] == 11u && rawOut[5] == 22u && rawOut[6] == 31u && rawOut[7] == 42u,
            "UAV Store2 raw mismatch");
    std::array<uint32_t, 4> cntOut{};
    readBuf(cb.Get(), cntOut.data(), sizeof(cntOut));
    Require(cntOut[1] == 777u, "UAV IncrementCounter store mismatch");
    return;
  }
  context->OMSetRenderTargets(1, view.GetAddressOf(), dsv.Get());
  context->DrawInstanced(3, LayerCount, 0, 0);
  context->OMSetRenderTargets(0, nullptr, nullptr);
  context->CopyResource(staging.Get(), target.Get());
  CheckReadback(*context.Get(), *staging.Get(), mode);
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    if (argc == 5 && std::wcscmp(argv[1], L"compile") == 0) {
      Compile(argv[2], "VS", "vs_5_0", argv[3]);
      Compile(argv[2], "PS", "ps_5_0", argv[4]);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-resource-arrays") == 0) {
      Compile(argv[2], "PSResources", "ps_5_0", argv[3]);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-quad-loop") == 0) {
      Compile(argv[2], "PSQuadLoop", "ps_5_0", argv[3]);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-local-arrays") == 0) {
      Compile(argv[2], "PSLocalArrays", "ps_5_0", argv[3]);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-gather") == 0) {
      Compile(argv[2], "PSGather", "ps_5_0", argv[3]);
      return 0;
    }
    if (argc >= 4 && std::wcscmp(argv[1], L"verify-dxbc") == 0) {
      VerifyDxbc(*Read(argv[2]).Get(), *Read(argv[3]).Get(), argc, argv);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-correctness") == 0) {
      Compile(argv[2], "PSCorrectness", "ps_5_0", argv[3],
              D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_IEEE_STRICTNESS);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-dimensions") == 0) {
      Compile(argv[2], "PSDimensions", "ps_5_0", argv[3]);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-compat") == 0) {
      Compile(argv[2], "PSCompat", "ps_5_0", argv[3]);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-compat2") == 0) {
      Compile(argv[2], "PSCompat2", "ps_5_0", argv[3],
              D3DCOMPILE_SKIP_OPTIMIZATION);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-compat3") == 0) {
      Compile(argv[2], "PSCompat3", "ps_5_0", argv[3]);
      return 0;
    }
    if (argc == 4 && std::wcscmp(argv[1], L"compile-compat4") == 0) {
      Compile(argv[2], "PSCompat4", "ps_5_0", argv[3]);
      return 0;
    }
    if (argc == 4) {
      DrawMode mode;
      if (std::wcscmp(argv[1], L"draw") == 0)
        mode = DrawMode::Standard;
      else if (std::wcscmp(argv[1], L"draw-discard") == 0)
        mode = DrawMode::Discard;
      else if (std::wcscmp(argv[1], L"draw-resource-arrays") == 0)
        mode = DrawMode::ResourceArrays;
      else if (std::wcscmp(argv[1], L"draw-quad-loop") == 0)
        mode = DrawMode::QuadLoop;
      else if (std::wcscmp(argv[1], L"draw-local-arrays") == 0)
        mode = DrawMode::LocalArrays;
      else if (std::wcscmp(argv[1], L"draw-gather") == 0)
        mode = DrawMode::Gather;
      else if (std::wcscmp(argv[1], L"draw-loop-lanes") == 0)
        mode = DrawMode::LoopLanes;
      else if (std::wcscmp(argv[1], L"draw-retry-loop") == 0)
        mode = DrawMode::RetryLoop;
      else if (std::wcscmp(argv[1], L"draw-sign") == 0)
        mode = DrawMode::Sign;
      else if (std::wcscmp(argv[1], L"draw-saturate") == 0)
        mode = DrawMode::Saturate;
      else if (std::wcscmp(argv[1], L"draw-local-constant-tuples") == 0)
        mode = DrawMode::LocalConstantTuples;
      else if (std::wcscmp(argv[1], L"draw-correctness") == 0)
        mode = DrawMode::Correctness;
      else if (std::wcscmp(argv[1], L"draw-dimensions") == 0)
        mode = DrawMode::Dimensions;
      else if (std::wcscmp(argv[1], L"draw-compat") == 0)
        mode = DrawMode::Compat;
      else if (std::wcscmp(argv[1], L"draw-compat2") == 0)
        mode = DrawMode::Compat2;
      else if (std::wcscmp(argv[1], L"draw-compat3") == 0)
        mode = DrawMode::Compat3;
      else if (std::wcscmp(argv[1], L"draw-compat4") == 0)
        mode = DrawMode::Compat4;
      else
        throw std::invalid_argument("unknown WARP draw mode");
      Draw(*Read(argv[2]).Get(), *Read(argv[3]).Get(), mode);
      return 0;
    }
    throw std::invalid_argument("expected compile SOURCE VS PS or a draw mode with VS PS");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
