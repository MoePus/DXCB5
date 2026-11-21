// Distributed under the University of Illinois Open Source License.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdint>

namespace dxcb5_proxy {

struct NativeDispatchTable {
  HRESULT(WINAPI *D3DCompile)(LPCVOID, SIZE_T, LPCSTR,
                              const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                              LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
  HRESULT(WINAPI *D3DCompile2)(LPCVOID, SIZE_T, LPCSTR,
                               const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                               LPCSTR, UINT, UINT, UINT, LPCVOID, SIZE_T,
                               ID3DBlob **, ID3DBlob **);
  HRESULT(WINAPI *D3DAssemble)(LPCVOID, SIZE_T, LPCSTR,
                               const D3D_SHADER_MACRO *, ID3DInclude *, UINT,
                               ID3DBlob **, ID3DBlob **);
  void(WINAPI *DebugSetMute)();
  HRESULT(WINAPI *D3DCompileFromFile)(LPCWSTR, const D3D_SHADER_MACRO *,
                                      ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob **, ID3DBlob **);
  HRESULT(WINAPI *D3DCompressShaders)(UINT, D3D_SHADER_DATA *, UINT,
                                      ID3DBlob **);
  HRESULT(WINAPI *D3DCreateBlob)(SIZE_T, ID3DBlob **);
  HRESULT(WINAPI *D3DCreateFunctionLinkingGraph)(
      UINT, ID3D11FunctionLinkingGraph **);
  HRESULT(WINAPI *D3DCreateLinker)(ID3D11Linker **);
  HRESULT(WINAPI *D3DDecompressShaders)(LPCVOID, SIZE_T, UINT, UINT, UINT *,
                                        UINT, ID3DBlob **, UINT *);
  HRESULT(WINAPI *D3DDisassemble)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob **);
  HRESULT(WINAPI *D3DDisassemble10Effect)(ID3D10Effect *, UINT, ID3DBlob **);
  HRESULT(WINAPI *D3DDisassemble11Trace)(LPCVOID, SIZE_T, void *, UINT, UINT,
                                         UINT, ID3DBlob **);
  HRESULT(WINAPI *D3DDisassembleRegion)(LPCVOID, SIZE_T, UINT, LPCSTR, SIZE_T,
                                        SIZE_T, SIZE_T *, ID3DBlob **);
  HRESULT(WINAPI *D3DGetBlobPart)(LPCVOID, SIZE_T, D3D_BLOB_PART, UINT,
                                  ID3DBlob **);
  HRESULT(WINAPI *D3DGetDebugInfo)(LPCVOID, SIZE_T, ID3DBlob **);
  HRESULT(WINAPI *D3DGetInputAndOutputSignatureBlob)(LPCVOID, SIZE_T,
                                                     ID3DBlob **);
  HRESULT(WINAPI *D3DGetInputSignatureBlob)(LPCVOID, SIZE_T, ID3DBlob **);
  HRESULT(WINAPI *D3DGetOutputSignatureBlob)(LPCVOID, SIZE_T, ID3DBlob **);
  HRESULT(WINAPI *D3DGetTraceInstructionOffsets)(LPCVOID, SIZE_T, UINT, SIZE_T,
                                                 SIZE_T, SIZE_T *, SIZE_T *);
  HRESULT(WINAPI *D3DLoadModule)(LPCVOID, SIZE_T, ID3D11Module **);
  HRESULT(WINAPI *D3DPreprocess)(LPCVOID, SIZE_T, LPCSTR,
                                 const D3D_SHADER_MACRO *, ID3DInclude *,
                                 ID3DBlob **, ID3DBlob **);
  HRESULT(WINAPI *D3DReadFileToBlob)(LPCWSTR, ID3DBlob **);
  HRESULT(WINAPI *D3DReflect)(LPCVOID, SIZE_T, REFIID, void **);
  HRESULT(WINAPI *D3DReflectLibrary)(LPCVOID, SIZE_T, REFIID, LPVOID *);
  HRESULT(WINAPI *D3DReturnFailure1)(HRESULT, UINT, UINT);
  HRESULT(WINAPI *D3DSetBlobPart)(LPCVOID, SIZE_T, D3D_BLOB_PART, UINT, LPCVOID,
                                  SIZE_T, ID3DBlob **);
  HRESULT(WINAPI *D3DStripShader)(LPCVOID, SIZE_T, UINT, ID3DBlob **);
  HRESULT(WINAPI *D3DWriteBlobToFile)(ID3DBlob *, LPCWSTR, BOOL);
};

// Shared between UnityShaderCompiler.exe's in-memory PE loader copy of
// D3DCompiler_47.dll and the native LoadLibraryExW copy of the same DLL that
// UnityShaderCompiler.exe loads immediately afterwards on the same thread.
constexpr uint64_t kBridgeMagic1 = 0x4458434235425247ULL; // "DXCB5BRG"
constexpr uint64_t kBridgeMagic2 = 0x554E49545950454CULL; // "UNITYPEL"

struct PeloaderBridgeSlot {
  uint64_t Magic1;
  uint64_t Magic2;
  DWORD Pid;
  DWORD TlsIndex;
  PeloaderBridgeSlot *Self;
  PVOID volatile NativeDispatch;
};

extern PeloaderBridgeSlot g_BridgeSlot;

inline const NativeDispatchTable *GetPeerNativeDispatch() {
  return static_cast<const NativeDispatchTable *>(g_BridgeSlot.NativeDispatch);
}

HMODULE GetFxcModule();
void LogForwardFailure(const char *name);

// Native implementations provided by D3DCompilerForward.cpp.
HRESULT WINAPI D3DAssemble_Native(LPCVOID, SIZE_T, LPCSTR,
                                  const D3D_SHADER_MACRO *, ID3DInclude *, UINT,
                                  ID3DBlob **, ID3DBlob **);
void WINAPI DebugSetMute_Native();
HRESULT WINAPI D3DCompileFromFile_Native(LPCWSTR, const D3D_SHADER_MACRO *,
                                         ID3DInclude *, LPCSTR, LPCSTR, UINT,
                                         UINT, ID3DBlob **, ID3DBlob **);
HRESULT WINAPI D3DCompressShaders_Native(UINT, D3D_SHADER_DATA *, UINT,
                                         ID3DBlob **);
HRESULT WINAPI D3DCreateBlob_Native(SIZE_T, ID3DBlob **);
HRESULT WINAPI
D3DCreateFunctionLinkingGraph_Native(UINT, ID3D11FunctionLinkingGraph **);
HRESULT WINAPI D3DCreateLinker_Native(ID3D11Linker **);
HRESULT WINAPI D3DDecompressShaders_Native(LPCVOID, SIZE_T, UINT, UINT, UINT *,
                                           UINT, ID3DBlob **, UINT *);
HRESULT WINAPI D3DDisassemble_Native(LPCVOID, SIZE_T, UINT, LPCSTR,
                                     ID3DBlob **);
HRESULT WINAPI D3DDisassemble10Effect_Native(ID3D10Effect *, UINT, ID3DBlob **);
HRESULT WINAPI D3DDisassemble11Trace_Native(LPCVOID, SIZE_T, void *, UINT, UINT,
                                            UINT, ID3DBlob **);
HRESULT WINAPI D3DDisassembleRegion_Native(LPCVOID, SIZE_T, UINT, LPCSTR,
                                           SIZE_T, SIZE_T, SIZE_T *,
                                           ID3DBlob **);
HRESULT WINAPI D3DGetBlobPart_Native(LPCVOID, SIZE_T, D3D_BLOB_PART, UINT,
                                     ID3DBlob **);
HRESULT WINAPI D3DGetDebugInfo_Native(LPCVOID, SIZE_T, ID3DBlob **);
HRESULT WINAPI D3DGetInputAndOutputSignatureBlob_Native(LPCVOID, SIZE_T,
                                                        ID3DBlob **);
HRESULT WINAPI D3DGetInputSignatureBlob_Native(LPCVOID, SIZE_T, ID3DBlob **);
HRESULT WINAPI D3DGetOutputSignatureBlob_Native(LPCVOID, SIZE_T, ID3DBlob **);
HRESULT WINAPI D3DGetTraceInstructionOffsets_Native(LPCVOID, SIZE_T, UINT,
                                                    SIZE_T, SIZE_T, SIZE_T *,
                                                    SIZE_T *);
HRESULT WINAPI D3DLoadModule_Native(LPCVOID, SIZE_T, ID3D11Module **);
HRESULT WINAPI D3DPreprocess_Native(LPCVOID, SIZE_T, LPCSTR,
                                    const D3D_SHADER_MACRO *, ID3DInclude *,
                                    ID3DBlob **, ID3DBlob **);
HRESULT WINAPI D3DReadFileToBlob_Native(LPCWSTR, ID3DBlob **);
HRESULT WINAPI D3DReflect_Native(LPCVOID, SIZE_T, REFIID, void **);
HRESULT WINAPI D3DReflectLibrary_Native(LPCVOID, SIZE_T, REFIID, LPVOID *);
HRESULT WINAPI D3DReturnFailure1_Native(HRESULT, UINT, UINT);
HRESULT WINAPI D3DSetBlobPart_Native(LPCVOID, SIZE_T, D3D_BLOB_PART, UINT,
                                     LPCVOID, SIZE_T, ID3DBlob **);
HRESULT WINAPI D3DStripShader_Native(LPCVOID, SIZE_T, UINT, ID3DBlob **);
HRESULT WINAPI D3DWriteBlobToFile_Native(ID3DBlob *, LPCWSTR, BOOL);

} // namespace dxcb5_proxy
