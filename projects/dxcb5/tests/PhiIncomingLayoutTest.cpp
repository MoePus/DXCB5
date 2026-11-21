// Distributed under the University of Illinois Open Source License.
#include "dxc/Support/Global.h"
#include "dxcb5/Compiler.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <string>
#include <d3dcompiler.h>
#include <fstream>
#include <iterator>
#include <wrl/client.h>

using namespace llvm;

namespace {

bool FindGroup(Module &Module, dxcb5::VectorPhiGroup &Group) {
  const auto *Entry = Module.getFunction("shade");
  if (!Entry)
    return false;
  for (const auto &Block : *Entry) {
    if (Block.getName() != "join")
      continue;
    auto Phi = Block.begin();
    for (unsigned Lane = 0; Lane < 3; ++Lane, ++Phi) {
      if (Phi == Block.end() || !isa<PHINode>(&*Phi))
        return false;
      Group[Lane] = &*Phi;
    }
    return true;
  }
  return false;
}

bool CompileAssembly(const char *Path, std::string &Listing) {
  std::ifstream File(Path, std::ios::binary);
  if (!File) {
    std::fprintf(stderr, "Could not read %s\n", Path);
    return false;
  }
  const std::string Source{std::istreambuf_iterator<char>(File), {}};
  LLVMContext Context;
  SMDiagnostic ParseError;
  auto Module = parseIR(MemoryBufferRef(Source, Path), ParseError, Context);
  if (!Module) {
    std::string Diagnostic;
    raw_string_ostream Stream(Diagnostic);
    ParseError.print("DXCB5PhiIncomingLayoutTest", Stream);
    Stream.flush();
    std::fputs(Diagnostic.c_str(), stderr);
    return false;
  }
  dxcb5::VectorPhiGroup Group{};
  if (!FindGroup(*Module, Group)) {
    std::fputs("Fixture must expose the three vector PHIs at the join\n", stderr);
    return false;
  }
  std::string Bytes;
  raw_string_ostream Output(Bytes);
  std::string ErrorText;
  raw_string_ostream Errors(ErrorText);
  const HRESULT Status = dxcb5::Compile(*Module, {}, Output, Errors, {Group});
  Errors.flush();
  if (FAILED(Status)) {
    std::fprintf(stderr, "Compile: %08lx (%s)\n", Status, ErrorText.c_str());
    return false;
  }
  Output.flush();
  Microsoft::WRL::ComPtr<ID3DBlob> Asm;
  const HRESULT DisasmStatus =
      D3DDisassemble(Bytes.data(), Bytes.size(), 0, nullptr, Asm.GetAddressOf());
  if (FAILED(DisasmStatus)) {
    std::fprintf(stderr, "Disassembly: %08lx (%zu bytes)\n", DisasmStatus, Bytes.size());
    return false;
  }
  Listing.assign(static_cast<const char *>(Asm->GetBufferPointer()), Asm->GetBufferSize());
  return true;
}

StringRef TakeInstruction(StringRef &Assembly, StringRef Opcode) {
  while (!Assembly.empty()) {
    const auto Parts = Assembly.split('\n');
    Assembly = Parts.second;
    const auto Instruction = Parts.first.trim().split(' ');
    if (Instruction.first == Opcode)
      return Instruction.second.trim();
  }
  return {};
}

StringRef TakeOperand(StringRef &Operands) {
  const auto Item = Operands.split(',');
  Operands = Item.second;
  return Item.first.trim();
}

bool MatchesLane(StringRef Operand, StringRef Register, StringRef Mask) {
  const auto Parts = Operand.split('.');
  return Parts.first == Register && Parts.second == Mask;
}

bool CheckSingleUseEdge(StringRef Edge, StringRef Register) {
  const char *Masks[] = {"x", "y", "z"};
  const char *Inputs[] = {"v0.xxxx", "v0.yyyy", "v0.zzzz"};
  const char *Rows[] = {"cb1[1].xxxx", "cb1[2].yyyy", "cb1[3].zzzz"};
  for (unsigned Lane = 0; Lane < 3; ++Lane) {
    auto Add = TakeInstruction(Edge, "add");
    const auto Target = TakeOperand(Add);
    const auto Left = TakeOperand(Add);
    const auto Right = TakeOperand(Add);
    const auto Input = Inputs[Lane];
    const auto Row = Rows[Lane];
    if (!MatchesLane(Target, Register, Masks[Lane]) ||
        !((Left == Input && Right == Row) || (Left == Row && Right == Input)))
      return false;
  }
  return true;
}

bool HasEdgeMove(StringRef Edge, StringRef Register, StringRef Lane,
                 StringRef Source) {
  for (auto Move = TakeInstruction(Edge, "mov"); !Move.empty();
       Move = TakeInstruction(Edge, "mov")) {
    const auto Target = TakeOperand(Move);
    if (MatchesLane(Target, Register, Lane) && TakeOperand(Move) == Source)
      return true;
  }
  return false;
}

bool CheckExtraUseEdge(StringRef Edge, StringRef Join) {
  auto Product = TakeInstruction(Edge, "mul");
  const auto Target = TakeOperand(Product);
  const auto Produced = Target.split('.');
  const auto Left = TakeOperand(Product);
  const auto Right = TakeOperand(Product);
  if (!Produced.first.startswith("r") || Produced.second != "x" ||
      !((Left == "cb1[2].yyyy" && Right == "v0.xxxx") ||
        (Left == "v0.xxxx" && Right == "cb1[2].yyyy")))
    return false;
  auto Subtract = TakeInstruction(Edge, "add");
  TakeOperand(Subtract);
  const auto Source = TakeOperand(Subtract);
  const auto Amount = TakeOperand(Subtract);
  return MatchesLane(Source, Produced.first, "xxxx") &&
         Amount == "-cb1[1].xxxx" &&
         (Produced.first == Join || HasEdgeMove(Edge, Join, "x", Source));
}

bool CheckSelectEdge(StringRef Edge, StringRef Join) {
  const char *Masks[] = {"x", "y", "z"};
  const char *TrueValues[] = {"cb1[2].yyyy", "v0.yyyy", "cb1[1].xxxx"};
  const char *FalseValues[] = {"v0.xxxx", "cb1[3].zzzz", "v0.zzzz"};
  StringRef Packed;
  for (unsigned Lane = 0; Lane < 3; ++Lane) {
    auto Select = TakeInstruction(Edge, "movc");
    const auto Target = TakeOperand(Select).split('.');
    const auto Condition = TakeOperand(Select);
    const auto TrueValue = TakeOperand(Select);
    const auto FalseValue = TakeOperand(Select);
    if (Lane == 0)
      Packed = Target.first;
    if (!Packed.startswith("r") || Target.first != Packed ||
        Target.second != Masks[Lane] || !Condition.startswith("r") ||
        TrueValue != TrueValues[Lane] || FalseValue != FalseValues[Lane])
      return false;
  }
  if (Packed == Join)
    return true;
  auto Move = TakeInstruction(Edge, "mov");
  const auto Target = TakeOperand(Move);
  const auto Source = TakeOperand(Move).split('.');
  return MatchesLane(Target, Join, "xyz") && Source.first == Packed &&
         Source.second.startswith("xyz");
}

bool CheckAssembly(StringRef Assembly, bool HasSelectEdge) {
  const auto If = Assembly.find("\nif_");
  const auto Else = Assembly.find("\nelse");
  const auto End = Assembly.find("\nendif");
  if (If == StringRef::npos || Else == StringRef::npos ||
      End == StringRef::npos || If >= Else || Else >= End)
    return false;
  auto Tail = Assembly.drop_front(End);
  auto Dot = TakeInstruction(Tail, "dp3");
  const auto Output = TakeOperand(Dot);
  const auto Incoming = TakeOperand(Dot).split('.');
  if (Output != "o0.x" || !Incoming.first.startswith("r") ||
      !Incoming.second.startswith("xyz"))
    return false;
  const bool SingleUse = CheckSingleUseEdge(Assembly.slice(If, Else), Incoming.first);
  const auto OtherEdge = Assembly.slice(Else, End);
  const bool Other = HasSelectEdge ? CheckSelectEdge(OtherEdge, Incoming.first)
                                   : CheckExtraUseEdge(OtherEdge, Incoming.first);
  if (!SingleUse)
    std::fputs("Expected three single-use results to define the PHI lanes directly\n",
               stderr);
  if (!Other)
    std::fputs(HasSelectEdge
                   ? "Expected three select results to share a packed PHI edge\n"
                   : "Expected the multiply to feed both the subtraction and the PHI edge\n",
               stderr);
  return SingleUse && Other;
}

} // namespace

int main(int Argc, char **Argv) {
  const bool HasSelectEdge = Argc == 3 && StringRef(Argv[2]) == "select";
  if (Argc != 2 && !HasSelectEdge) {
    std::fprintf(stderr, "Usage: DXCB5PhiIncomingLayoutTest <fixture.ll> [select]\n");
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
  std::string Listing;
  if (!CompileAssembly(Argv[1], Listing))
    return 1;
  if (CheckAssembly(Listing, HasSelectEdge))
    return 0;
  std::fwrite(Listing.data(), 1, Listing.size(), stderr);
  return 1;
}
