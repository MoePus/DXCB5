// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_COMPILE_SESSION_H
#define DXCB5_COMPILE_SESSION_H

#include "dxcb5/Compiler.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include <deque>
#include <string>
#include <vector>

namespace dxcb5 {

struct FrontendKey {
  std::string Preprocessed;
  std::string SourceName;
  std::string EntryPoint;
  std::string Profile;
  std::vector<std::string> Arguments;

  bool operator==(const FrontendKey &Other) const;
};

// One synchronous cache per compiler instance. The loaded frontend and backend
// revisions are immutable for that lifetime, so they namespace every key.
class CompileSession {
public:
  struct Frontend {
    FrontendKey Key;
    std::unique_ptr<llvm::LLVMContext> Context;
    std::unique_ptr<llvm::Module> Module;
    std::deque<std::pair<std::string, std::string>> Bytecode;

    const std::string *FindBytecode(llvm::StringRef Signature) const;
    void SaveBytecode(llvm::StringRef Signature, llvm::StringRef Object);
  };

  Frontend *FindFrontend(const FrontendKey &Key);
  Frontend &SaveFrontend(Frontend Entry);

private:
  std::deque<Frontend> Entries;
};

} // namespace dxcb5
#endif
