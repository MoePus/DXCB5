// Distributed under the University of Illinois Open Source License.
#include "NativeBridge.h"

namespace {

template <typename Fn> Fn ResolveFxc(const char *name) {
  HMODULE mod = dxcb5_proxy::GetFxcModule();
  if (!mod)
    return nullptr;
  return reinterpret_cast<Fn>(GetProcAddress(mod, name));
}

} // namespace

#define FWD_HR(name, params, args)                                             \
  namespace dxcb5_proxy {                                                      \
  HRESULT WINAPI name##_Native params {                                        \
    using Pfn = HRESULT(WINAPI *) params;                                      \
    static Pfn fn = ResolveFxc<Pfn>(#name);                                    \
    if (!fn) {                                                                 \
      dxcb5_proxy::LogForwardFailure(#name);                                   \
      return E_FAIL;                                                           \
    }                                                                          \
    return fn args;                                                            \
  }                                                                            \
  }                                                                            \
  extern "C" HRESULT WINAPI name params {                                      \
    if (const auto *peerDisp = dxcb5_proxy::GetPeerNativeDispatch())           \
      return peerDisp->name args;                                              \
    return dxcb5_proxy::name##_Native args;                                    \
  }

FWD_HR(D3DAssemble,
       (LPCVOID a, SIZE_T b, LPCSTR c, const D3D_SHADER_MACRO *d,
        ID3DInclude *e, UINT f, ID3DBlob **g, ID3DBlob **h),
       (a, b, c, d, e, f, g, h))

namespace dxcb5_proxy {
void WINAPI DebugSetMute_Native() {
  using Pfn = void(WINAPI *)();
  static Pfn fn = ResolveFxc<Pfn>("DebugSetMute");
  if (fn)
    fn();
}
} // namespace dxcb5_proxy

extern "C" void WINAPI DebugSetMute() {
  if (const auto *d = dxcb5_proxy::GetPeerNativeDispatch()) {
    d->DebugSetMute();
    return;
  }
  dxcb5_proxy::DebugSetMute_Native();
}

FWD_HR(D3DCompileFromFile,
       (LPCWSTR a, const D3D_SHADER_MACRO *b, ID3DInclude *c, LPCSTR d,
        LPCSTR e, UINT f, UINT g, ID3DBlob **h, ID3DBlob **i),
       (a, b, c, d, e, f, g, h, i))

FWD_HR(D3DCompressShaders,
       (UINT a, D3D_SHADER_DATA *b, UINT c, ID3DBlob **d), (a, b, c, d))

FWD_HR(D3DCreateBlob, (SIZE_T a, ID3DBlob **b), (a, b))

FWD_HR(D3DCreateFunctionLinkingGraph,
       (UINT a, ID3D11FunctionLinkingGraph **b), (a, b))

FWD_HR(D3DCreateLinker, (ID3D11Linker **a), (a))

FWD_HR(D3DDecompressShaders,
       (LPCVOID a, SIZE_T b, UINT c, UINT d, UINT *e, UINT f, ID3DBlob **g,
        UINT *h),
       (a, b, c, d, e, f, g, h))

FWD_HR(D3DDisassemble,
       (LPCVOID a, SIZE_T b, UINT c, LPCSTR d, ID3DBlob **e),
       (a, b, c, d, e))

FWD_HR(D3DDisassemble10Effect,
       (ID3D10Effect *a, UINT b, ID3DBlob **c), (a, b, c))

FWD_HR(D3DDisassemble11Trace,
       (LPCVOID a, SIZE_T b, void *c, UINT d, UINT e, UINT f, ID3DBlob **g),
       (a, b, c, d, e, f, g))

FWD_HR(D3DDisassembleRegion,
       (LPCVOID a, SIZE_T b, UINT c, LPCSTR d, SIZE_T e, SIZE_T f, SIZE_T *g,
        ID3DBlob **h),
       (a, b, c, d, e, f, g, h))

FWD_HR(D3DGetBlobPart,
       (LPCVOID a, SIZE_T b, D3D_BLOB_PART c, UINT d, ID3DBlob **e),
       (a, b, c, d, e))

FWD_HR(D3DGetDebugInfo, (LPCVOID a, SIZE_T b, ID3DBlob **c), (a, b, c))

FWD_HR(D3DGetInputAndOutputSignatureBlob,
       (LPCVOID a, SIZE_T b, ID3DBlob **c), (a, b, c))

FWD_HR(D3DGetInputSignatureBlob, (LPCVOID a, SIZE_T b, ID3DBlob **c), (a, b, c))

FWD_HR(D3DGetOutputSignatureBlob, (LPCVOID a, SIZE_T b, ID3DBlob **c),
       (a, b, c))

FWD_HR(D3DGetTraceInstructionOffsets,
       (LPCVOID a, SIZE_T b, UINT c, SIZE_T d, SIZE_T e, SIZE_T *f, SIZE_T *g),
       (a, b, c, d, e, f, g))

FWD_HR(D3DLoadModule, (LPCVOID a, SIZE_T b, ID3D11Module **c), (a, b, c))

FWD_HR(D3DPreprocess,
       (LPCVOID a, SIZE_T b, LPCSTR c, const D3D_SHADER_MACRO *d,
        ID3DInclude *e, ID3DBlob **f, ID3DBlob **g),
       (a, b, c, d, e, f, g))

FWD_HR(D3DReadFileToBlob, (LPCWSTR a, ID3DBlob **b), (a, b))

FWD_HR(D3DReflect, (LPCVOID a, SIZE_T b, REFIID c, void **d), (a, b, c, d))

FWD_HR(D3DReflectLibrary, (LPCVOID a, SIZE_T b, REFIID c, LPVOID *d),
       (a, b, c, d))

FWD_HR(D3DReturnFailure1, (HRESULT a, UINT b, UINT c), (a, b, c))

FWD_HR(D3DSetBlobPart,
       (LPCVOID a, SIZE_T b, D3D_BLOB_PART c, UINT d, LPCVOID e, SIZE_T f,
        ID3DBlob **g),
       (a, b, c, d, e, f, g))

FWD_HR(D3DStripShader, (LPCVOID a, SIZE_T b, UINT c, ID3DBlob **d),
       (a, b, c, d))

FWD_HR(D3DWriteBlobToFile, (ID3DBlob *a, LPCWSTR b, BOOL c), (a, b, c))
