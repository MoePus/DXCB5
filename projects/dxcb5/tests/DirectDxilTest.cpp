// Distributed under the University of Illinois Open Source License.
#include "dxc/Support/Global.h"
#include "dxcb5/Compiler.h"
#include "dxcb5/DxbcWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <d3dcompiler.h>
#include <fstream>
#include <iterator>
#include <string>
#include <wrl/client.h>

using namespace llvm;

namespace {

bool HasDotLiteral(StringRef Assembly, StringRef Opcode, StringRef Literal) {
  for (auto Op = Assembly.find(Opcode); Op != StringRef::npos;
       Op = Assembly.find(Opcode, Op + 1)) {
    if (Assembly.substr(Op + 1).split('\n').first.find(Literal) != StringRef::npos)
      return true;
  }
  return false;
}

bool CheckDotSources(StringRef Assembly) {
  return Assembly.count("\ndp3 ") == 4 && Assembly.count("\nmad ") >= 2 &&
         Assembly.find("\nmul ") != StringRef::npos &&
         HasDotLiteral(Assembly, "\ndp2 ", "l(0x80000000, 0x40000000, 0x00000000, 0x00000000)") &&
         HasDotLiteral(Assembly, "\ndp3 ", "l(0x40400000, 0x40000000, 0x3f800000, 0x00000000)") &&
         HasDotLiteral(Assembly, "\ndp3 ", "l(0x40a00000, 0x40c00000, 0x40e00000, 0x00000000)") &&
         HasDotLiteral(Assembly, "\ndp4 ", "l(0x3f800000, 0x40800000, 0x40000000, 0x40400000)");
}

unsigned CountOpcode(StringRef Assembly, StringRef Opcode,
                     StringRef Mask = {}) {
  unsigned Count = 0;
  while (!Assembly.empty()) {
    const auto Line = Assembly.split('\n');
    const auto Operation = Line.first.trim().split(' ');
    const auto Dest = Operation.second.trim().split(',').first;
    Count += Operation.first == Opcode &&
             (Mask.empty() || Dest.split('.').second == Mask);
    Assembly = Line.second;
  }
  return Count;
}

bool CheckSharedDotSources(StringRef Assembly) {
  const auto Branch = Assembly.find("\nif_");
  if (Branch == StringRef::npos || CountOpcode(Assembly, "dp3") != 3 ||
      CountOpcode(Assembly.substr(0, Branch), "dp3") != 3)
    return false;
  const auto Else = Assembly.find("\nelse", Branch);
  if (Else == StringRef::npos)
    return false;
  const auto Matrix = Assembly.slice(Branch, Else);
  auto CountWidth3 = [&](StringRef Opcode) {
    unsigned Count = 0;
    for (auto Rest = Matrix; !Rest.empty();) {
      const auto Line = Rest.split('\n');
      Rest = Line.second;
      const auto Op = Line.first.trim().split(' ');
      const auto Dest = Op.second.trim().split(',').first;
      Count += Op.first == Opcode && Dest.split('.').second.size() == 3;
    }
    return Count;
  };
  if (CountWidth3("mul") != 1 || CountWidth3("mad") != 2)
    return false;

  StringRef Source;
  unsigned Dots = 0;
  while (!Assembly.empty()) {
    const auto Lines = Assembly.split('\n');
    Assembly = Lines.second;
    const auto Line = Lines.first.trim();
    if (!Line.startswith("dp3 "))
      continue;
    const auto Input = Line.split(',').second.split(',').first.trim();
    if (!Input.startswith("r") || (!Source.empty() && Source != Input))
      return false;
    Source = Input;
    ++Dots;
  }
  return Dots == 3;
}

bool CheckUniformDots(StringRef Assembly) {
  // Global SSA GVN deduplicates both uniform and identical same-block varying dots
  // while keeping precise, signed-zero, swizzled, and sibling-branch dots distinct.
  return CountOpcode(Assembly, "dp2") == 6 &&
         CountOpcode(Assembly, "dp3") == 1 &&
         CountOpcode(Assembly, "dp4") == 2;
}

bool CheckConstantTuples(StringRef Assembly) {
  unsigned VectorLoads = 0, ScalarLoads = 0;
  while (!Assembly.empty()) {
    const auto Lines = Assembly.split('\n');
    Assembly = Lines.second;
    const auto Line = Lines.first.trim();
    if (!Line.startswith("mov ") || Line.find("icb") == StringRef::npos)
      continue;
    const auto Mask = Line.split(',').first.split('.').second;
    VectorLoads += Mask == "xyz" && Line.endswith("].xyzx");
    ScalarLoads += Mask.size() == 1 && (Line.endswith(".xxxx") || Line.endswith(".x"));
  }
  return VectorLoads == 1 && ScalarLoads == 7;
}

bool CheckPhiSources(StringRef Assembly) {
  const auto Branch = Assembly.find("\nif_");
  const auto End = Assembly.find("\nendif");
  if (Branch == StringRef::npos || End == StringRef::npos || Branch >= End)
    return false;
  const auto Head = Assembly.substr(0, Branch);
  const auto Edges = Assembly.slice(Branch, End);
  if (Head.find("cb1[r") == StringRef::npos || Edges.find("cb1[r") != StringRef::npos)
    return false;
  for (const auto *Source : {"cb1[1].y", "cb1[2].z", ", v0.w"}) {
    if (Head.find(Source) != StringRef::npos || Edges.find(Source) == StringRef::npos)
      return false;
  }
  return true;
}

bool HasInPlaceLoopAdd(StringRef Assembly) {
  while (!Assembly.empty()) {
    const auto Line = Assembly.split('\n');
    Assembly = Line.second;
    if (!Line.first.trim().startswith("add r"))
      continue;
    const auto Args = Line.first.trim().drop_front(4).split(',');
    const auto Dest = Args.first.split('.');
    const auto Src0 = Args.second.split(',');
    const auto Left = Src0.first.trim().split('.');
    const auto Right = Src0.second.trim().split('.');
    if (Dest.second == "xyz" && Left.second.startswith("xyz") &&
        Right.second.startswith("xyz") &&
        (Left.first == Dest.first || Right.first == Dest.first))
      return true;
  }
  return false;
}

bool HasInPlaceLoopMad(StringRef Assembly) {
  while (!Assembly.empty()) {
    const auto Line = Assembly.split('\n');
    Assembly = Line.second;
    const auto Text = Line.first.trim();
    if (!Text.startswith("mad r"))
      continue;
    const auto Args = Text.drop_front(4).split(',');
    const auto Dest = Args.first.split('.');
    const auto Scale = Args.second.split(',');
    const auto Contribution = Scale.second.split(',');
    const auto Accumulator = Contribution.second.trim().split('.');
    if (Dest.second == "xyz" && Accumulator.first == Dest.first &&
        Accumulator.second.startswith("xyz") &&
        Contribution.first.trim().split('.').second.startswith("xyz"))
      return true;
  }
  return false;
}

bool CheckDirectLoopRecurrence(StringRef Assembly, StringRef Mode) {
  auto Begin = StringRef::npos;
  auto End = StringRef::npos;
  for (auto Cursor = Assembly; !Cursor.empty();) {
    auto Lines = Cursor.split('\n');
    const auto Op = Lines.first.trim();
    const auto Offset = static_cast<size_t>(Cursor.data() - Assembly.data());
    if (Op == "loop" && Begin == StringRef::npos)
      Begin = Offset;
    if (Op == "endloop")
      End = Offset;
    Cursor = Lines.second;
  }
  if (Begin == StringRef::npos || End == StringRef::npos || Begin >= End)
    return false;
  const auto Code = Assembly.slice(Begin, End);
  if (Mode == "direct-loop-rgb")
    return HasInPlaceLoopMad(Code) && CountOpcode(Code, "mad", "xyz") == 1;
  if (Mode == "guarded-loop-rgb")
    return HasInPlaceLoopAdd(Code) && CountOpcode(Code, "add", "xyz") >= 1;
  // The loop can revisit its latch via the bypass; verify the vector add loop
  // body stays vectorized in .xyz and preserves the iteration-entry state.
  return CountOpcode(Code, "add", "xyz") >= 2;
}

bool CheckSaturateOpcodes(StringRef Assembly) {
  const auto Discard = Assembly.find("\ndiscard_");
  if (Discard == StringRef::npos)
    return false;
  const auto Before = Assembly.substr(0, Discard);
  const auto After = Assembly.drop_front(Discard);
  // Saturated arithmetic folds before discard; multi-use add keeps a separate mov_sat.
  return Before.count("\nmul_sat ") == 1 && Before.count("\ndiv_sat ") == 1 &&
         Before.count("\nadd_sat ") == 1 && After.count("mov_sat ") >= 1;
}

} // namespace

bool ReadVertexOutput(const char *Path, std::vector<dxcb5::SignatureElement> &Vertex) {
  std::ifstream Shader(Path, std::ios::binary);
  if (!Shader) {
    std::fprintf(stderr, "Could not read vertex shader %s\n", Path);
    return false;
  }
  const std::string Bytes{std::istreambuf_iterator<char>(Shader), {}};
  std::string Error;
  if (dxcb5::ReadVertexOutputSignature(Bytes, Vertex, Error))
    return true;
  std::fprintf(stderr, "Could not link vertex shader %s: %s\n", Path, Error.c_str());
  return false;
}

int main(int Argc, char **Argv) {
  if (Argc != 3 && Argc != 5) {
    std::fputs("usage: DXCB5DirectDxilTest input.ll mode [output.dxbc vertex.dxbc]\n",
               stderr);
    return 1;
  }
  if (FAILED(DxcInitThreadMalloc())) {
    std::fputs("Could not initialize DXC allocator\n", stderr);
    return 1;
  }
  struct AllocatorCleanup {
    ~AllocatorCleanup() { DxcCleanupThreadMalloc(); }
  } Cleanup;
  DxcThreadMalloc Allocator(nullptr);
  std::ifstream File(Argv[1], std::ios::binary);
  if (!File) {
    std::fprintf(stderr, "Could not read fixture %s\n", Argv[1]);
    return 1;
  }
  std::string Source{std::istreambuf_iterator<char>(File), {}};
  LLVMContext Context;
  SMDiagnostic ParseError;
  auto Module = parseIR(MemoryBufferRef(Source, Argv[1]), ParseError, Context);
  if (!Module) {
    std::string Diagnostic;
    raw_string_ostream Stream(Diagnostic);
    ParseError.print(Argv[0], Stream);
    Stream.flush();
    std::fputs(Diagnostic.c_str(), stderr);
    return 1;
  }
  std::string Bytecode, Diagnostics;
  raw_string_ostream Object(Bytecode), Errors(Diagnostics);
  std::vector<dxcb5::SignatureElement> Vertex;
  if (Argc == 5 && !ReadVertexOutput(Argv[4], Vertex))
    return 1;
  const auto Status = dxcb5::Compile(*Module, Vertex, Object, Errors);
  Object.flush();
  Errors.flush();

  if (StringRef(Argv[2]) == "wave") {
    const char *Expected =
        "unsupported SM5 DXIL operation: dx.op.waveReadLaneFirst.f32";
    if (FAILED(Status) && Bytecode.empty() &&
        Diagnostics.find(Expected) != std::string::npos)
      return 0;
    std::fprintf(stderr, "Expected a wave-operation error without DXBC, got: %s\n",
                 Diagnostics.c_str());
    return 1;
  }

  Microsoft::WRL::ComPtr<ID3DBlob> Disassembly;
  const bool TestDots = StringRef(Argv[2]) == "dot";
  const bool TestSign = StringRef(Argv[2]) == "sign";
  const UINT Flags = (TestDots || TestSign) ? D3D_DISASM_PRINT_HEX_LITERALS : 0;
  if (FAILED(Status) ||
      FAILED(D3DDisassemble(Bytecode.data(), Bytecode.size(), Flags, nullptr,
                            Disassembly.GetAddressOf()))) {
    std::fprintf(stderr, "Could not lower and disassemble fixed arithmetic DXIL: %s\n",
                 Diagnostics.c_str());
    return 1;
  }
  StringRef Assembly(static_cast<const char *>(Disassembly->GetBufferPointer()),
                     Disassembly->GetBufferSize());
  const StringRef Mode = Argv[2];
  if (Mode == "shared-dot-sources") {
    if (CheckSharedDotSources(Assembly))
      return 0;
    std::fputs("Expected a local shared gather and a branch with packed matrix arithmetic\n", stderr);
    std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
    return 1;
  }
  if (Mode == "uniform-dots") {
    if (CheckUniformDots(Assembly))
      return 0;
    std::fputs("Expected only same-block and dominating uniform dots to share an SSA result\n",
               stderr);
    std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
    return 1;
  }
  if (Mode == "local-constant-tuples" && !CheckConstantTuples(Assembly)) {
    std::fputs("Expected one packed ICB tuple and scalar loads for wrapping, mixed and partial accesses\n", stderr);
    std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
    return 1;
  }
  if (Mode.endswith("-loop-rgb")) {
    if (CheckDirectLoopRecurrence(Assembly, Mode))
      return 0;
    std::fputs("Unexpected RGB recurrence lowering\n", stderr);
    std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
    return 1;
  }
  if (Mode == "loop" || Mode == "loop-retry" || TestSign || Mode == "saturate" ||
      Mode == "local-constant-tuples") {
    if (TestSign &&
        (Assembly.count("\niadd ") > 1 || Assembly.count("\nineg ") != 0 ||
         Assembly.count("\nitof ") != 1 ||
         !HasDotLiteral(Assembly, "\nmovc ", "0xbf800000") ||
         !HasDotLiteral(Assembly, "\nmovc ", "0x3f800000"))) {
      std::fputs("Expected the ordered sign to select MOVC and the mismatched pair to retain integer arithmetic\n", stderr);
      std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
      return 1;
    }
    if (Mode == "loop" && !HasInPlaceLoopAdd(Assembly)) {
      std::fputs("Expected an in-place RGB accumulator update\n", stderr);
      std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
      return 1;
    }
    if (Mode == "saturate" && !CheckSaturateOpcodes(Assembly)) {
      std::fputs("Expected saturated arithmetic before discard and separate clamps for the multi-use and cross-block values\n",
                 stderr);
      std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
      return 1;
    }
    if (Argc != 5)
      return 0;
    std::ofstream Output(Argv[3], std::ios::binary);
    Output.write(Bytecode.data(), Bytecode.size());
    Output.close();
    if (Output)
      return 0;
    std::fprintf(stderr, "Could not write %s\n", Argv[3]);
    return 1;
  }
  if (Mode == "contract") {
    if (Assembly.count("\nmad ") == 1 && Assembly.find("\nmul ") != StringRef::npos &&
        Assembly.find("\nadd ") != StringRef::npos)
      return 0;
    std::fputs("Expected only the single-use fast product to contract\n", stderr);
    std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
    return 1;
  }
  if (StringRef(Argv[2]) == "phi") {
    if (CheckPhiSources(Assembly))
      return 0;
    std::fputs("Expected static PHI sources on the edge and a relative load in the head\n",
               stderr);
    std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
    return 1;
  }
  if (TestDots) {
    if (CheckDotSources(Assembly))
      return 0;
    std::fputs("Expected explicit dots and fast chains with exact literal operands\n",
               stderr);
    std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
    return 1;
  }
  if (Mode == "arithmetic") {
    if (Assembly.find("vs_5_0") != StringRef::npos &&
        Assembly.find("\nadd ") != StringRef::npos)
      return 0;
    std::fputs("Expected an SM5.0 vertex shader with an add instruction\n", stderr);
    std::fwrite(Assembly.data(), 1, Assembly.size(), stderr);
    return 1;
  }
  std::fprintf(stderr, "Unknown DirectDxilTest mode: %s\n", Argv[2]);
  return 1;
}
