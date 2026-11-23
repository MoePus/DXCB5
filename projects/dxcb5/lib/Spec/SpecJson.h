// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_SPEC_JSON_H
#define DXCB5_SPEC_JSON_H

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dxcb5 {
namespace spec {

struct JsonValue {
  enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };
  Kind Type = Kind::Null;
  bool BoolVal = false;
  double NumVal = 0.0;
  std::string StrVal;
  std::vector<JsonValue> ArrVal;
  std::map<std::string, JsonValue> ObjVal;

  const JsonValue *Find(const std::string &Key) const {
    if (Type != Kind::Object)
      return nullptr;
    auto It = ObjVal.find(Key);
    return It == ObjVal.end() ? nullptr : &It->second;
  }
};

bool ParseJson(llvm::StringRef Input, JsonValue &Out, std::string &Error);

class JsonWriter {
public:
  void BeginObject();
  void EndObject();
  void BeginArray();
  void EndArray();
  void Key(llvm::StringRef Name);
  void String(llvm::StringRef Val);
  void Int(int64_t Val);
  void UInt(uint64_t Val);
  void Double(double Val);
  void Bool(bool Val);
  void Null();
  std::string Take() { return std::move(Buf); }

private:
  void PrepareValue();
  void AppendEscaped(llvm::StringRef S);
  std::string Buf;
  std::vector<bool> NeedComma;
  bool ExpectValueAfterKey = false;
};

} // namespace spec
} // namespace dxcb5

#endif // DXCB5_SPEC_JSON_H
