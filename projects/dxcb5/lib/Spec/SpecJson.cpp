// Distributed under the University of Illinois Open Source License.
#include "Spec/SpecJson.h"
#include "dxcb5/Formats.h"
#include "dxcb5/SpecEngine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>

using namespace dxcb5::spec;

namespace {

bool ParseHex4(llvm::StringRef S, size_t Pos, uint32_t &Out) {
  if (Pos + 4 > S.size())
    return false;
  uint32_t V = 0;
  for (size_t I = 0; I < 4; ++I) {
    const char C = S[Pos + I];
    uint32_t D = 0;
    if (C >= '0' && C <= '9') D = static_cast<uint32_t>(C - '0');
    else if (C >= 'a' && C <= 'f') D = 10u + static_cast<uint32_t>(C - 'a');
    else if (C >= 'A' && C <= 'F') D = 10u + static_cast<uint32_t>(C - 'A');
    else return false;
    V = (V << 4) | D;
  }
  Out = V;
  return true;
}

void AppendUtf8Codepoint(std::string &Out, uint32_t Cp) {
  if (Cp <= 0x7fu) {
    Out.push_back(static_cast<char>(Cp));
  } else if (Cp <= 0x7ffu) {
    Out.push_back(static_cast<char>(0xc0u | (Cp >> 6)));
    Out.push_back(static_cast<char>(0x80u | (Cp & 0x3fu)));
  } else if (Cp <= 0xffffu) {
    Out.push_back(static_cast<char>(0xe0u | (Cp >> 12)));
    Out.push_back(static_cast<char>(0x80u | ((Cp >> 6) & 0x3fu)));
    Out.push_back(static_cast<char>(0x80u | (Cp & 0x3fu)));
  } else {
    Out.push_back(static_cast<char>(0xf0u | (Cp >> 18)));
    Out.push_back(static_cast<char>(0x80u | ((Cp >> 12) & 0x3fu)));
    Out.push_back(static_cast<char>(0x80u | ((Cp >> 6) & 0x3fu)));
    Out.push_back(static_cast<char>(0x80u | (Cp & 0x3fu)));
  }
}

class JsonParser {
public:
  JsonParser(llvm::StringRef In, std::string &Err) : S(In), Error(Err) {}

  bool Parse(JsonValue &Out) {
    SkipWs();
    if (!ParseValue(Out, 0))
      return false;
    SkipWs();
    if (Pos != S.size())
      return Fail("trailing characters after JSON value");
    return true;
  }

private:
  llvm::StringRef S;
  size_t Pos = 0;
  std::string &Error;

  bool Fail(const char *Msg) {
    Error = std::string("JSON parse error at offset ") + std::to_string(Pos) +
            ": " + Msg;
    return false;
  }

  void SkipWs() {
    while (Pos < S.size() &&
           (S[Pos] == ' ' || S[Pos] == '\t' || S[Pos] == '\r' || S[Pos] == '\n'))
      ++Pos;
  }

  bool ParseValue(JsonValue &Out, unsigned Depth) {
    if (Depth > 32)
      return Fail("maximum nesting depth exceeded");
    SkipWs();
    if (Pos >= S.size())
      return Fail("unexpected end of input");
    const char C = S[Pos];
    if (C == '{')
      return ParseObject(Out, Depth + 1);
    if (C == '[')
      return ParseArray(Out, Depth + 1);
    if (C == '"') {
      Out.Type = JsonValue::Kind::String;
      return ParseString(Out.StrVal);
    }
    if (C == 't' && S.substr(Pos).startswith("true")) {
      Pos += 4;
      Out.Type = JsonValue::Kind::Bool;
      Out.BoolVal = true;
      return true;
    }
    if (C == 'f' && S.substr(Pos).startswith("false")) {
      Pos += 5;
      Out.Type = JsonValue::Kind::Bool;
      Out.BoolVal = false;
      return true;
    }
    if (C == 'n' && S.substr(Pos).startswith("null")) {
      Pos += 4;
      Out.Type = JsonValue::Kind::Null;
      return true;
    }
    if (C == '-' || (C >= '0' && C <= '9'))
      return ParseNumber(Out);
    return Fail("unexpected token");
  }

  bool ParseUnicodeEscape(std::string &Out) {
    uint32_t Cp = 0;
    if (!ParseHex4(S, Pos, Cp))
      return Fail("invalid \\uXXXX escape");
    Pos += 4;
    if (Cp >= 0xd800u && Cp <= 0xdbffu && Pos + 6 <= S.size() &&
        S[Pos] == '\\' && S[Pos + 1] == 'u') {
      uint32_t Low = 0;
      if (!ParseHex4(S, Pos + 2, Low) || Low < 0xdc00u || Low > 0xdfffu)
        return Fail("invalid UTF-16 surrogate pair");
      Pos += 6;
      Cp = 0x10000u + (((Cp - 0xd800u) << 10) | (Low - 0xdc00u));
    }
    AppendUtf8Codepoint(Out, Cp);
    return true;
  }

  bool ParseString(std::string &Out) {
    if (Pos >= S.size() || S[Pos] != '"')
      return Fail("expected '\"'");
    ++Pos;
    Out.clear();
    while (Pos < S.size()) {
      const char C = S[Pos++];
      if (C == '"')
        return true;
      if (C != '\\') {
        Out.push_back(C);
        continue;
      }
      if (Pos >= S.size())
        return Fail("unterminated escape sequence");
      const char Esc = S[Pos++];
      switch (Esc) {
      case '"': Out.push_back('"'); break;
      case '\\': Out.push_back('\\'); break;
      case '/': Out.push_back('/'); break;
      case 'b': Out.push_back('\b'); break;
      case 'f': Out.push_back('\f'); break;
      case 'n': Out.push_back('\n'); break;
      case 'r': Out.push_back('\r'); break;
      case 't': Out.push_back('\t'); break;
      case 'u':
        if (!ParseUnicodeEscape(Out))
          return false;
        break;
      default: return Fail("unsupported string escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseNumber(JsonValue &Out) {
    const size_t Start = Pos;
    if (S[Pos] == '-')
      ++Pos;
    while (Pos < S.size() && ((S[Pos] >= '0' && S[Pos] <= '9') ||
                              S[Pos] == '.' || S[Pos] == 'e' || S[Pos] == 'E' ||
                              S[Pos] == '+' || S[Pos] == '-'))
      ++Pos;
    const std::string NumStr = S.slice(Start, Pos).str();
    std::istringstream ISS(NumStr);
    ISS.imbue(std::locale::classic());
    double Val = 0.0;
    if (!(ISS >> Val) || !ISS.eof() || !std::isfinite(Val))
      return Fail("invalid number");
    Out.Type = JsonValue::Kind::Number;
    Out.NumVal = Val;
    return true;
  }

  bool ParseArray(JsonValue &Out, unsigned Depth) {
    ++Pos;
    Out.Type = JsonValue::Kind::Array;
    SkipWs();
    if (Pos < S.size() && S[Pos] == ']') {
      ++Pos;
      return true;
    }
    while (true) {
      JsonValue Elem;
      if (!ParseValue(Elem, Depth))
        return false;
      Out.ArrVal.push_back(std::move(Elem));
      SkipWs();
      if (Pos >= S.size())
        return Fail("unterminated array");
      if (S[Pos] == ']') {
        ++Pos;
        return true;
      }
      if (S[Pos] != ',')
        return Fail("expected ',' or ']' in array");
      ++Pos;
    }
  }

  bool ParseObject(JsonValue &Out, unsigned Depth) {
    ++Pos;
    Out.Type = JsonValue::Kind::Object;
    SkipWs();
    if (Pos < S.size() && S[Pos] == '}') {
      ++Pos;
      return true;
    }
    while (true) {
      SkipWs();
      std::string Key;
      if (!ParseString(Key))
        return false;
      SkipWs();
      if (Pos >= S.size() || S[Pos] != ':')
        return Fail("expected ':' after object key");
      ++Pos;
      JsonValue Val;
      if (!ParseValue(Val, Depth))
        return false;
      Out.ObjVal[std::move(Key)] = std::move(Val);
      SkipWs();
      if (Pos >= S.size())
        return Fail("unterminated object");
      if (S[Pos] == '}') {
        ++Pos;
        return true;
      }
      if (S[Pos] != ',')
        return Fail("expected ',' or '}' in object");
      ++Pos;
    }
  }
};

bool DoubleToUInt32Checked(double V, uint32_t &Out) {
  if (!std::isfinite(V) || V < 0.0 || V > 4294967295.0)
    return false;
  Out = static_cast<uint32_t>(V);
  return true;
}

bool ParseConstType(llvm::StringRef S, ConstType &Out) {
  if (S == "f32") { Out = ConstType::F32; return true; }
  if (S == "i32") { Out = ConstType::I32; return true; }
  if (S == "u32") { Out = ConstType::U32; return true; }
  if (S == "bool") { Out = ConstType::Bool; return true; }
  return false;
}

bool ParseConstantsSection(const JsonValue &Obj, SpecManifest &Out,
                           std::string &Error) {
  if (Obj.Type != JsonValue::Kind::Object) {
    Error = "'constants' must be a JSON object";
    return false;
  }
  for (const auto &KV : Obj.ObjVal) {
    const JsonValue &Entry = KV.second;
    const JsonValue *TyVal = Entry.Find("type");
    const JsonValue *VArr = Entry.Find("v");
    if (!TyVal || TyVal->Type != JsonValue::Kind::String || !VArr ||
        VArr->Type != JsonValue::Kind::Array || VArr->ArrVal.empty()) {
      Error = "invalid constant entry for '" + KV.first + "'";
      return false;
    }
    SpecConstValue CV;
    if (!ParseConstType(TyVal->StrVal, CV.Type)) {
      Error = "unsupported constant type '" + TyVal->StrVal + "' for '" +
              KV.first + "'";
      return false;
    }
    for (const auto &Elem : VArr->ArrVal) {
      if (Elem.Type == JsonValue::Kind::Number)
        CV.Values.push_back(Elem.NumVal);
      else if (Elem.Type == JsonValue::Kind::Bool)
        CV.Values.push_back(Elem.BoolVal ? 1.0 : 0.0);
      else {
        Error = "non-numeric constant element for '" + KV.first + "'";
        return false;
      }
    }
    Out.Constants[KV.first] = std::move(CV);
  }
  return true;
}

bool ParseTextureDims(const JsonValue &Dims, const std::string &Key,
                      SpecTextureValue &TV, std::string &Error) {
  uint32_t W = 0, H = 0;
  if (Dims.Type != JsonValue::Kind::Array || Dims.ArrVal.size() < 2 ||
      Dims.ArrVal[0].Type != JsonValue::Kind::Number ||
      Dims.ArrVal[1].Type != JsonValue::Kind::Number ||
      !DoubleToUInt32Checked(Dims.ArrVal[0].NumVal, W) ||
      !DoubleToUInt32Checked(Dims.ArrVal[1].NumVal, H)) {
    Error = "invalid const_textures dims for '" + Key + "'";
    return false;
  }
  TV.HasDims = true;
  TV.DimCount = 2;
  TV.Width = W;
  TV.Height = H;
  if (Dims.ArrVal.size() >= 3) {
    uint32_t D = 0;
    if (Dims.ArrVal[2].Type != JsonValue::Kind::Number ||
        !DoubleToUInt32Checked(Dims.ArrVal[2].NumVal, D)) {
      Error = "invalid const_textures depth for '" + Key + "'";
      return false;
    }
    TV.Depth = D;
    TV.DimCount = 3;
  }
  return true;
}

bool ParseTexturesSection(const JsonValue &Obj, SpecManifest &Out,
                          std::string &Error) {
  if (Obj.Type != JsonValue::Kind::Object) {
    Error = "'const_textures' must be a JSON object";
    return false;
  }
  for (const auto &KV : Obj.ObjVal) {
    const JsonValue &Entry = KV.second;
    const JsonValue *VArr = Entry.Find("v");
    if (!VArr || VArr->Type != JsonValue::Kind::Array ||
        VArr->ArrVal.empty() || VArr->ArrVal.size() > 4) {
      Error = "invalid const_textures entry for '" + KV.first + "'";
      return false;
    }
    SpecTextureValue TV;
    for (size_t I = 0; I < VArr->ArrVal.size(); ++I) {
      if (VArr->ArrVal[I].Type != JsonValue::Kind::Number) {
        Error = "non-numeric const_textures color for '" + KV.first + "'";
        return false;
      }
      TV.Color[I] = static_cast<float>(VArr->ArrVal[I].NumVal);
    }
    if (const JsonValue *Dims = Entry.Find("dims")) {
      if (!ParseTextureDims(*Dims, KV.first, TV, Error))
        return false;
    }
    if (const JsonValue *B = Entry.Find("address_border")) {
      if (B->Type == JsonValue::Kind::Bool)
        TV.AddressBorder = B->BoolVal;
    }
    Out.ConstTextures[KV.first] = TV;
  }
  return true;
}

} // namespace

bool dxcb5::spec::ParseJson(llvm::StringRef Input, JsonValue &Out,
                            std::string &Error) {
  JsonParser P(Input, Error);
  return P.Parse(Out);
}

void JsonWriter::PrepareValue() {
  if (ExpectValueAfterKey) {
    ExpectValueAfterKey = false;
    return;
  }
  if (!NeedComma.empty() && NeedComma.back())
    Buf.push_back(',');
  if (!NeedComma.empty())
    NeedComma.back() = true;
}

void JsonWriter::AppendEscaped(llvm::StringRef S) {
  Buf.push_back('"');
  for (unsigned char C : S) {
    switch (C) {
    case '"': Buf += "\\\""; break;
    case '\\': Buf += "\\\\"; break;
    case '\n': Buf += "\\n"; break;
    case '\r': Buf += "\\r"; break;
    case '\t': Buf += "\\t"; break;
    default:
      if (C < 0x20u) {
        char Esc[8];
        std::snprintf(Esc, sizeof(Esc), "\\u%04x", C);
        Buf += Esc;
      } else {
        Buf.push_back(static_cast<char>(C));
      }
      break;
    }
  }
  Buf.push_back('"');
}

void JsonWriter::BeginObject() {
  PrepareValue();
  Buf.push_back('{');
  NeedComma.push_back(false);
}

void JsonWriter::EndObject() {
  Buf.push_back('}');
  NeedComma.pop_back();
}

void JsonWriter::BeginArray() {
  PrepareValue();
  Buf.push_back('[');
  NeedComma.push_back(false);
}

void JsonWriter::EndArray() {
  Buf.push_back(']');
  NeedComma.pop_back();
}

void JsonWriter::Key(llvm::StringRef Name) {
  if (!NeedComma.empty() && NeedComma.back())
    Buf.push_back(',');
  if (!NeedComma.empty())
    NeedComma.back() = true;
  AppendEscaped(Name);
  Buf.push_back(':');
  ExpectValueAfterKey = true;
}

void JsonWriter::String(llvm::StringRef Val) {
  PrepareValue();
  AppendEscaped(Val);
}

void JsonWriter::Int(int64_t Val) {
  PrepareValue();
  Buf += std::to_string(Val);
}

void JsonWriter::UInt(uint64_t Val) {
  PrepareValue();
  Buf += std::to_string(Val);
}

void JsonWriter::Double(double Val) {
  PrepareValue();
  if (!std::isfinite(Val)) {
    Buf += "null";
    return;
  }
  std::ostringstream OSS;
  OSS.imbue(std::locale::classic());
  OSS << std::setprecision(9) << Val;
  Buf += OSS.str();
}

void JsonWriter::Bool(bool Val) {
  PrepareValue();
  Buf += Val ? "true" : "false";
}

void JsonWriter::Null() {
  PrepareValue();
  Buf += "null";
}

bool dxcb5::spec::ParseSpecManifest(llvm::StringRef Json, SpecManifest &Out,
                                    std::string &Error) {
  Out = {};
  if (Json.trim().empty())
    return true;
  JsonValue Root;
  if (!ParseJson(Json, Root, Error))
    return false;
  if (Root.Type != JsonValue::Kind::Object) {
    Error = "manifest root must be a JSON object";
    return false;
  }
  if (const JsonValue *V = Root.Find("schema_version")) {
    if (V->Type != JsonValue::Kind::Number ||
        V->NumVal != static_cast<double>(DXCB5_SPEC_SCHEMA_VERSION)) {
      Error = "unsupported manifest schema_version (expected 1)";
      return false;
    }
    Out.SchemaVersion = DXCB5_SPEC_SCHEMA_VERSION;
  }
  if (const JsonValue *H = Root.Find("uber_dxil_hash")) {
    if (H->Type != JsonValue::Kind::String) {
      Error = "'uber_dxil_hash' must be a JSON string";
      return false;
    }
    Out.UberDxilHash = H->StrVal;
  }
  if (const JsonValue *C = Root.Find("constants")) {
    if (!ParseConstantsSection(*C, Out, Error))
      return false;
  }
  if (const JsonValue *T = Root.Find("const_textures")) {
    if (!ParseTexturesSection(*T, Out, Error))
      return false;
  }
  if (const JsonValue *D = Root.Find("defines")) {
    if (D->Type != JsonValue::Kind::Object) {
      Error = "'defines' must be a JSON object";
      return false;
    }
    for (const auto &KV : D->ObjVal) {
      if (KV.second.Type == JsonValue::Kind::Number) {
        uint32_t Val = 0;
        if (!DoubleToUInt32Checked(KV.second.NumVal, Val)) {
          Error = "define '" + KV.first + "' value out of uint32 range";
          return false;
        }
        Out.Defines[KV.first] = Val;
      } else if (KV.second.Type == JsonValue::Kind::Bool) {
        Out.Defines[KV.first] = KV.second.BoolVal ? 1u : 0u;
      } else {
        Error = "define '" + KV.first + "' must be numeric or boolean";
        return false;
      }
    }
  }
  if (const JsonValue *O = Root.Find("options")) {
    if (O->Type != JsonValue::Kind::Object) {
      Error = "'options' must be a JSON object";
      return false;
    }
    auto ReadBool = [&](const char *K, bool &Dst) -> bool {
      if (const JsonValue *V = O->Find(K)) {
        if (V->Type != JsonValue::Kind::Bool) {
          Error = std::string("'options.") + K + "' must be a JSON boolean";
          return false;
        }
        Dst = V->BoolVal;
      }
      return true;
    };
    if (!ReadBool("compact_globals", Out.Options.CompactGlobals) ||
        !ReadBool("cross_stage_dce", Out.Options.CrossStageDce) ||
        !ReadBool("emit_intermediate_dxil", Out.Options.EmitIntermediateDxil) ||
        !ReadBool("emit_stats", Out.Options.EmitStats) ||
        !ReadBool("nan_safe", Out.Options.NanSafe))
      return false;
  }
  return true;
}

static void WriteStageStatsObj(JsonWriter &W, const StageStats &S) {
  W.BeginObject();
  W.Key("stage"); W.String(S.Stage);
  W.Key("instructions"); W.UInt(S.Backend.Instructions);
  W.Key("temps"); W.UInt(S.Backend.Temps);
  W.Key("indexable_temps"); W.UInt(S.Backend.IndexableTemps);
  W.Key("sfu_ops"); W.UInt(S.Backend.SfuOps);
  W.Key("tex_ops"); W.UInt(S.Backend.TexOps);
  W.Key("branch_ops"); W.UInt(S.Backend.BranchOps);
  W.Key("globals_bytes"); W.UInt(S.Backend.GlobalsBytes);
  W.Key("dynamic_cost"); W.UInt(S.Backend.DynamicCost);
  W.Key("peak_lanes"); W.UInt(S.Backend.PeakLanes);
  W.Key("stripped_textures");
  W.BeginArray();
  for (const auto &T : S.StrippedTextures)
    W.String(T);
  W.EndArray();
  W.Key("stripped_inputs");
  W.BeginArray();
  for (const auto &I : S.StrippedInputs)
    W.String(I);
  W.EndArray();
  W.Key("spec_ms"); W.Double(S.SpecMs);
  W.Key("backend_ms"); W.Double(S.BackendMs);
  W.Key("cache_hit"); W.Bool(S.CacheHit);
  W.EndObject();
}

std::string dxcb5::spec::FormatStageStatsJson(const StageStats &Stats) {
  JsonWriter W;
  WriteStageStatsObj(W, Stats);
  return W.Take();
}

std::string dxcb5::spec::FormatPairStatsJson(const StageStats &VsStats,
                                             const StageStats &PsStats) {
  JsonWriter W;
  W.BeginArray();
  WriteStageStatsObj(W, VsStats);
  WriteStageStatsObj(W, PsStats);
  W.EndArray();
  return W.Take();
}
