// Distributed under the University of Illinois Open Source License.
#include "dxcb5/CompileSession.h"
#include <tuple>
#include <utility>

using llvm::StringRef;

namespace dxcb5 {
namespace {
constexpr size_t MaxFrontends = 16;
constexpr size_t MaxVariantsPerFrontend = 8;
}

bool FrontendKey::operator==(const FrontendKey &Other) const {
  return std::tie(Profile, EntryPoint, SourceName, Arguments, Preprocessed) ==
         std::tie(Other.Profile, Other.EntryPoint, Other.SourceName,
                  Other.Arguments, Other.Preprocessed);
}

const std::string *
CompileSession::Frontend::FindBytecode(StringRef Signature) const {
  for (const auto &Entry : Bytecode)
    if (Entry.first == Signature)
      return &Entry.second;
  return nullptr;
}

void CompileSession::Frontend::SaveBytecode(StringRef Signature,
                                            StringRef Object) {
  if (Bytecode.size() == MaxVariantsPerFrontend)
    Bytecode.pop_front();
  Bytecode.emplace_back(Signature.str(), Object.str());
}

CompileSession::Frontend *CompileSession::FindFrontend(const FrontendKey &Key) {
  for (auto &Entry : Entries)
    if (Entry.Key == Key)
      return &Entry;
  return nullptr;
}

CompileSession::Frontend &CompileSession::SaveFrontend(Frontend Entry) {
  if (Entries.size() == MaxFrontends)
    Entries.pop_front();
  Entries.push_back(std::move(Entry));
  return Entries.back();
}

} // namespace dxcb5
