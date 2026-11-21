// Distributed under the University of Illinois Open Source License.
#include <windows.h>
#include "dxc/dxcapi.h"
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

void Require(bool Condition, const char *Message) {
  if (!Condition)
    throw std::runtime_error(Message);
}

std::string BlobText(IDxcBlob &Blob) {
  return {static_cast<const char *>(Blob.GetBufferPointer()),
          Blob.GetBufferSize()};
}

class Inputs final : public IDxcIncludeHandler {
public:
  explicit Inputs(IDxcUtils &Utils) : Utils(Utils) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void **Out) override {
    if (!Out)
      return E_POINTER;
    *Out = nullptr;
    if (Id != __uuidof(IUnknown) && Id != __uuidof(IDxcIncludeHandler))
      return E_NOINTERFACE;
    *Out = static_cast<IDxcIncludeHandler *>(this);
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++References; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG Remaining = --References;
    if (!Remaining)
      delete this;
    return Remaining;
  }

  HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR Path, IDxcBlob **Out) override {
    const auto *Base = Path;
    for (const auto *Cursor = Path; *Cursor; ++Cursor) {
      if (*Cursor == L'/' || *Cursor == L'\\')
        Base = Cursor + 1;
    }
    *Out = nullptr;
    if (std::wcscmp(Base, L"linked.dxbc") == 0 && Vertex)
      return Vertex.CopyTo(Out);
    if (std::wcscmp(Base, L"color.hlsl") != 0)
      return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    ComPtr<IDxcBlobEncoding> Blob;
    const auto Result = Utils.CreateBlob(Header.data(), static_cast<UINT32>(Header.size()),
                                         DXC_CP_UTF8, Blob.GetAddressOf());
    return FAILED(Result) ? Result : Blob.CopyTo(Out);
  }

  ComPtr<IDxcBlob> Vertex;
  std::string Header = "#define SHARED_COLOR 0.25\n";

private:
  IDxcUtils &Utils;
  std::atomic<ULONG> References{1};
};

enum class Stage { Vertex, Pixel };
enum class Reuse { None, Frontend, Both };
enum class VertexEdit { None, Code, Signature };

std::string Source(VertexEdit Edit = VertexEdit::None, bool PixelEdit = false) {
  // The same program text is used for both stages in each view of the source.
  const char *ExtraMember = Edit == VertexEdit::Signature
                                ? "float4 extra : TEXCOORD0;\n" : "";
  const char *ExtraWrite = Edit == VertexEdit::Signature ? "v.extra = position;\n" : "";
  const char *VertexScale = Edit == VertexEdit::None ? "1.0" : "0.5";
  const char *PixelScale = PixelEdit ? "0.75" : "0.5";
  return std::string(R"(
#include "color.hlsl"
#if __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX
struct Varying {
  float4 position : SV_Position;
)") + ExtraMember + R"(
  float4 color : COLOR0;
};
Varying shade(float4 position : POSITION, float4 color : COLOR0) {
  Varying v;
  v.position = position;
)" + ExtraWrite + R"(
  v.color = color * )" + VertexScale + R"( + SHARED_COLOR;
  return v;
}
#else
float4 shade(float4 color : COLOR0) : SV_Target {
  return color * )" + PixelScale + R"( + SHARED_COLOR;
}
#endif
)";
}

struct Compilation {
  ComPtr<IDxcBlob> Bytecode;
  std::string Trace;
};

Compilation Compile(IDxcCompiler3 &Compiler, Inputs &IncludeHandler, Stage Target,
                    const std::string &Shader, bool Cached, bool DebugOpt = false) {
  std::vector<LPCWSTR> Args = {
      L"session.hlsl", L"-fdxcb5", L"-T",
      Target == Stage::Vertex ? L"vs_5_0" : L"ps_5_0", L"-E", L"shade",
      DebugOpt ? L"-O0" : L"-O3", L"-ftime-trace", L"-ftime-trace-granularity=0"};
  if (Cached)
    Args.push_back(L"-fdxcb5-cache");
  if (Target == Stage::Pixel) {
    Args.push_back(L"--vs-dxbc");
    Args.push_back(L"linked.dxbc");
  }
  DxcBuffer SourceBuffer{Shader.data(), Shader.size(), DXC_CP_UTF8};
  ComPtr<IDxcResult> Result;
  Require(SUCCEEDED(Compiler.Compile(&SourceBuffer, Args.data(),
      static_cast<UINT32>(Args.size()), &IncludeHandler, IID_PPV_ARGS(&Result))),
      "IDxcCompiler3::Compile call failed");
  HRESULT Status;
  Require(SUCCEEDED(Result->GetStatus(&Status)), "no compile status");
  if (FAILED(Status)) {
    ComPtr<IDxcBlobEncoding> Errors;
    Result->GetErrorBuffer(Errors.GetAddressOf());
    if (Errors)
      std::fprintf(stderr, "%s", BlobText(*Errors.Get()).c_str());
    Require(false, "DXCB5 compilation failed");
  }
  Compilation Output;
  Require(SUCCEEDED(Result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&Output.Bytecode), nullptr)) &&
              Output.Bytecode, "no DXBC output");
  ComPtr<IDxcBlob> Trace;
  Require(SUCCEEDED(Result->GetOutput(DXC_OUT_TIME_TRACE, IID_PPV_ARGS(&Trace), nullptr)) &&
              Trace, "no compiler time trace");
  Output.Trace = BlobText(*Trace.Get());
  return Output;
}

bool Saw(const std::string &Trace, const char *Scope) {
  // LLVM retains a total entry even when a hit takes less than one microsecond.
  return Trace.find(std::string("\"Total ") + Scope + '"') != std::string::npos;
}

void CheckTrace(const std::string &Trace, Reuse Expect) {
  struct Check {
    const char *Name;
    bool Present;
  };
  const Check Checks[] = {
      {"DXCB5 Cache Preprocess", true},
      {"DXCB5 Frontend", Expect == Reuse::None},
      {"DXCB5 Expand Trig", Expect == Reuse::None},
      {"DXCB5 SM5 Backend", Expect != Reuse::Both},
      {"DXCB5 Frontend Cache Hit", Expect != Reuse::None},
      {"DXCB5 Backend Cache Hit", Expect == Reuse::Both},
  };
  for (const auto &Item : Checks) {
    if (Saw(Trace, Item.Name) == Item.Present)
      continue;
    std::fprintf(stderr, "%s: expected %s\n", Item.Name,
                 Item.Present ? "scope" : "no scope");
    Require(false, "incorrect pipeline reuse");
  }
}

ComPtr<IDxcBlob> CheckCompile(IDxcCompiler3 &Compiler, Inputs &IncludeHandler,
                              Stage Target, const std::string &Shader,
                              Reuse Expected, bool DebugOpt = false) {
  auto Result = Compile(Compiler, IncludeHandler, Target, Shader, true, DebugOpt);
  CheckTrace(Result.Trace, Expected);
  auto Uncached = Compile(Compiler, IncludeHandler, Target, Shader, false, DebugOpt);
  Require(BlobText(*Result.Bytecode.Get()) == BlobText(*Uncached.Bytecode.Get()),
          "cached and uncached DXBC differ");
  return Result.Bytecode;
}

void RunMatrix(IDxcCompiler3 &Compiler, Inputs &IncludeHandler) {
  struct Case {
    const char *Name;
    VertexEdit Vertex;
    bool Pixel;
    Reuse VertexReuse;
    Reuse PixelReuse;
    bool DebugOpt = false;
  };
  // Reuse::Frontend means the PS linkage changed and only the SM5 backend reruns.
  const Case Cases[] = {
      {"cold compile", VertexEdit::None, false, Reuse::None, Reuse::None},
      {"complete hit", VertexEdit::None, false, Reuse::Both, Reuse::Both},
      {"pixel code edited", VertexEdit::None, true, Reuse::Both, Reuse::None},
      {"vertex code edited", VertexEdit::Code, true, Reuse::None, Reuse::Both},
      {"vertex signature edited", VertexEdit::Signature, true, Reuse::None, Reuse::Frontend},
      {"replay cold source", VertexEdit::None, false, Reuse::Both, Reuse::Both},
      {"optimization option edited", VertexEdit::None, false, Reuse::None, Reuse::None, true},
      {"same optimization option", VertexEdit::None, false, Reuse::Both, Reuse::Both, true},
  };
  for (const auto &Item : Cases) {
    std::printf("Cache case: %s\n", Item.Name);
    std::fflush(stdout);
    const auto Shader = Source(Item.Vertex, Item.Pixel);
    IncludeHandler.Vertex = CheckCompile(Compiler, IncludeHandler, Stage::Vertex,
                                         Shader, Item.VertexReuse, Item.DebugOpt);
    CheckCompile(Compiler, IncludeHandler, Stage::Pixel, Shader,
                 Item.PixelReuse, Item.DebugOpt);
  }
}

void CheckInclude(IDxcCompiler3 &Compiler, Inputs &IncludeHandler) {
  std::puts("Cache case: include contents edited at the same path");
  const auto Shader = Source();
  IncludeHandler.Header = "#define SHARED_COLOR 0.125\n";
  IncludeHandler.Vertex = CheckCompile(Compiler, IncludeHandler, Stage::Vertex,
                                       Shader, Reuse::None);
  CheckCompile(Compiler, IncludeHandler, Stage::Pixel, Shader, Reuse::None);
  CheckCompile(Compiler, IncludeHandler, Stage::Pixel, Shader, Reuse::Both);
}

} // namespace

int main() {
  try {
    ComPtr<IDxcUtils> Utils;
    ComPtr<IDxcCompiler3> Compiler;
    Require(SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utils))),
            "could not create DXC utilities");
    Require(SUCCEEDED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler))),
            "could not create DXC compiler");
    ComPtr<Inputs> IncludeHandler;
    IncludeHandler.Attach(new Inputs(*Utils.Get()));
    RunMatrix(*Compiler.Get(), *IncludeHandler.Get());
    CheckInclude(*Compiler.Get(), *IncludeHandler.Get());
    return 0;
  } catch (const std::exception &Error) {
    std::fprintf(stderr, "%s\n", Error.what());
    return 1;
  }
}
