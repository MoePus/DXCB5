// Distributed under the University of Illinois Open Source License.
#include <d3d11_1.h>
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

float CompareMoeToonShaders(ID3D11Device &device, ID3D11DeviceContext &context,
                            unsigned layers, const wchar_t *const *paths);

namespace {
enum Result { Failed = -1, Pending, Passed };
IUnityInterfaces *Interfaces = nullptr;
struct DrawRequest {
  unsigned Layers = 0;
  std::array<std::wstring, 4> Paths;
  float MaximumDifference = 0;
  std::atomic<int> Status{Passed};
};
DrawRequest Request;

class ScopedContextState {
public:
  ScopedContextState(ID3D11DeviceContext1 &context, ID3DDeviceContextState *state)
      : Context(context) {
    Context.SwapDeviceContextState(state, Previous.GetAddressOf());
  }
  ~ScopedContextState() { Context.SwapDeviceContextState(Previous.Get(), nullptr); }

private:
  ID3D11DeviceContext1 &Context;
  ComPtr<ID3DDeviceContextState> Previous;
};

void DrawOnUnityDevice() {
  auto *graphics = Interfaces ? Interfaces->Get<IUnityGraphicsD3D11>() : nullptr;
  auto *device = graphics ? graphics->GetDevice() : nullptr;
  ComPtr<ID3D11Device1> device1;
  if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
    throw std::runtime_error("Unity must use a D3D11.1 device");

  const auto level = device->GetFeatureLevel();
  ComPtr<ID3DDeviceContextState> state;
  if (FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                    __uuidof(ID3D11Device), nullptr, state.GetAddressOf())))
    throw std::runtime_error("CreateDeviceContextState failed");
  ComPtr<ID3D11DeviceContext1> context;
  device1->GetImmediateContext1(context.GetAddressOf());
  ScopedContextState restore(*context.Get(), state.Get());

  std::array<const wchar_t *, 4> paths;
  for (size_t i = 0; i < paths.size(); ++i)
    paths[i] = Request.Paths[i].c_str();
  Request.MaximumDifference = CompareMoeToonShaders(*device, *context.Get(),
                                                    Request.Layers, paths.data());
}

void UNITY_INTERFACE_API OnRender(int) {
  if (Request.Status.load() != Pending)
    return;
  try {
    DrawOnUnityDevice();
    Request.Status.store(Passed);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "DXCB5 Unity draw: %s\n", error.what());
    Request.Status.store(Failed);
  }
}
} // namespace

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces *interfaces) {
  Interfaces = interfaces;
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload() {
  Interfaces = nullptr;
}

extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
PrepareDxcB5Draw(unsigned layers, const wchar_t *baselineVS, const wchar_t *baselinePS,
                 const wchar_t *candidateVS, const wchar_t *candidatePS) {
  if (Request.Status.load() == Pending || layers < 1 || layers > 2)
    return Failed;
  Request.Layers = layers;
  Request.Paths = {baselineVS, baselinePS, candidateVS, candidatePS};
  Request.Status.store(Pending);
  return Pending;
}

extern "C" int UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
DxcB5DrawStatus(float *maximumDifference) {
  const int status = Request.Status.load();
  if (status == Passed)
    *maximumDifference = Request.MaximumDifference;
  return status;
}

extern "C" UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
GetDxcB5RenderEvent() {
  return OnRender;
}
