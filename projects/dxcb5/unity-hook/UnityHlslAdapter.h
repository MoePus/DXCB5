// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_UNITY_HLSL_ADAPTER_H
#define DXCB5_UNITY_HLSL_ADAPTER_H

#include <cstddef>
#include <string>

namespace dxcb5_proxy {

// Adapts FXC/Unity-preprocessed HLSL (which may contain legacy DX9 sampler2D /
// samplerCUBE / sampler3D / sampler2DShadow declarations, tex2D* helpers, and
// legacy PS entry semantics like `: COLOR`) into DXC-compatible HLSL while
// preserving Unity's RDEF texture/sampler names.
// Returns true if the source was modified and written to `out`.
bool AdaptUnityHlslForDxc(const char *src, size_t len, std::string &out,
                          const char *entrypoint = nullptr,
                          bool isPixel = false);

} // namespace dxcb5_proxy

#endif
