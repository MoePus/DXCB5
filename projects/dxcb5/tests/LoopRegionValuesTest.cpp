// Distributed under the University of Illinois Open Source License.
#include "dxcb5/LoopRegionValues.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/WinIncludes.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include <cstdio>
#include <string>

using namespace llvm;

namespace {
const char *Fixture = R"(
define float @main(i1 %cond, i1 %repeat, float %x) {
entry:
  br label %loop
loop:
  %prior = phi float [ %x, %entry ], [ %next, %join2 ]
  br i1 %cond, label %arm1, label %join1
arm1:
  %scaled1 = fmul float %prior, 2.0
  br label %join1
join1:
  %first = phi float [ %scaled1, %arm1 ], [ %prior, %loop ]
  br i1 GUARD, label %arm2, label %join2
arm2:
  %scaled2 = fmul FLAGS float INPUT, 2.0
  br label %join2
join2:
  %second = phi float ARM2, FALLBACK
  %next = fadd float %first, %second
  br i1 %repeat, label %loop, label %exit
exit:
  ret float %next
}
)";

void Replace(std::string &Source, StringRef Token, StringRef Value) {
  Source.replace(Source.find(Token.str()), Token.size(), Value.str());
}

struct Test {
  const char *Name;
  const char *Guard;
  const char *Flags;
  const char *Input;
  const char *DirectValue;
  bool Reverse;
  bool Reused;
};

bool Run(const Test &Case) {
  std::string Source = Fixture;
  Replace(Source, "GUARD", Case.Guard);
  Replace(Source, "FLAGS", Case.Flags);
  Replace(Source, "INPUT", Case.Input);
  const std::string Direct = std::string("[ ") + Case.DirectValue + ", %join1 ]";
  Replace(Source, "ARM2", Case.Reverse ? Direct : "[ %scaled2, %arm2 ]");
  Replace(Source, "FALLBACK", Case.Reverse ? "[ %scaled2, %arm2 ]" : Direct);

  LLVMContext Context;
  SMDiagnostic Diag;
  auto Module = parseAssemblyString(Source, Diag, Context);
  if (!Module || verifyModule(*Module))
    return false;
  auto &Entry = *Module->getFunction("main");
  const bool Changed = dxcb5::ReuseLoopRegionValues(Entry, Module->GetOrCreateDxilModule(true));
  if (verifyModule(*Module) || Changed != Case.Reused)
    return false;
  for (auto &Block : Entry)
    for (auto &I : Block)
      if (I.getName() == "next")
        return (I.getOperand(0) == I.getOperand(1)) == Case.Reused;
  return false;
}
} // namespace

int main() {
  if (FAILED(DxcInitThreadMalloc()))
    return 1;
  struct Cleanup { ~Cleanup() { DxcCleanupThreadMalloc(); } } Guard;
  DxcThreadMalloc Allocator(nullptr);
  const Test Cases[] = {
      {"ordered guarded PHI", "%cond", "", "%prior", "%prior", false, true},
      {"incoming list order is irrelevant", "%cond", "", "%prior", "%prior", true, true},
      {"fast and strict operations remain independent", "%cond", "fast", "%prior", "%prior", false, false},
      {"distinct loop-carried value", "%cond", "", "%x", "%prior", false, false},
      {"distinct guard remains independent", "%repeat", "", "%prior", "%prior", false, false},
      {"undefined arm stays opaque", "%cond", "", "%prior", "undef", false, false},
  };
  for (const auto &Case : Cases) {
    if (Run(Case))
      continue;
    std::fprintf(stderr, "Loop region value test failed: %s\n", Case.Name);
    return 1;
  }
  return 0;
}
