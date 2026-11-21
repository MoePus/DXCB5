// Distributed under the University of Illinois Open Source License.
#include "UnityHlslAdapter.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace dxcb5_proxy {
namespace {

enum class SamplerKind { None, Tex2D, Tex3D, TexCube, Tex2DShadow };

struct Token {
  size_t Pos = 0;
  size_t Len = 0;
  bool IsIdent = false;
  char Punct = 0;
};

struct GlobalSampler {
  SamplerKind Kind = SamplerKind::None;
  std::string Name;
  size_t DeclEnd = 0;
};

bool IsIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

SamplerKind ClassifySamplerType(const std::string &s) {
  if (s == "sampler2D" || s == "sampler2D_half" || s == "sampler2D_float" ||
      s == "sampler2D_f" || s == "sampler2D_h")
    return SamplerKind::Tex2D;
  if (s == "sampler3D" || s == "sampler3D_half" || s == "sampler3D_float")
    return SamplerKind::Tex3D;
  if (s == "samplerCUBE" || s == "samplerCUBE_half" || s == "samplerCUBE_float")
    return SamplerKind::TexCube;
  if (s == "sampler2DShadow")
    return SamplerKind::Tex2DShadow;
  return SamplerKind::None;
}

const char *TextureTypeName(SamplerKind k) {
  switch (k) {
  case SamplerKind::Tex2D: return "Texture2D<float4>";
  case SamplerKind::Tex3D: return "Texture3D<float4>";
  case SamplerKind::TexCube: return "TextureCube<float4>";
  case SamplerKind::Tex2DShadow: return "Texture2D<float>";
  default: return "";
  }
}

const char *SamplerStateTypeName(SamplerKind k) {
  return k == SamplerKind::Tex2DShadow ? "SamplerComparisonState"
                                       : "SamplerState";
}

const char *HelperCtorName(SamplerKind k) {
  switch (k) {
  case SamplerKind::Tex2D: return "_dxcb5_s2d";
  case SamplerKind::Tex3D: return "_dxcb5_s3d";
  case SamplerKind::TexCube: return "_dxcb5_scube";
  case SamplerKind::Tex2DShadow: return "_dxcb5_s2ds";
  default: return "";
  }
}

const char kCompatPreamble[] = R"HLSL(
#ifndef DXCB5_LEGACY_SAMPLER_SHIM
#define DXCB5_LEGACY_SAMPLER_SHIM 1
struct sampler2D { Texture2D<float4> t; SamplerState s; };
struct sampler3D { Texture3D<float4> t; SamplerState s; };
struct samplerCUBE { TextureCube<float4> t; SamplerState s; };
struct sampler2DShadow { Texture2D<float> t; SamplerComparisonState s; };
#define sampler2D_half sampler2D
#define sampler2D_float sampler2D
#define sampler3D_half sampler3D
#define sampler3D_float sampler3D
#define samplerCUBE_half samplerCUBE
#define samplerCUBE_float samplerCUBE
inline sampler2D _dxcb5_s2d(Texture2D<float4> t, SamplerState s) { sampler2D r; r.t = t; r.s = s; return r; }
inline sampler3D _dxcb5_s3d(Texture3D<float4> t, SamplerState s) { sampler3D r; r.t = t; r.s = s; return r; }
inline samplerCUBE _dxcb5_scube(TextureCube<float4> t, SamplerState s) { samplerCUBE r; r.t = t; r.s = s; return r; }
inline sampler2DShadow _dxcb5_s2ds(Texture2D<float> t, SamplerComparisonState s) { sampler2DShadow r; r.t = t; r.s = s; return r; }
inline float4 tex2D(sampler2D x, float2 v) { return x.t.Sample(x.s, v); }
inline float4 tex2D(sampler2D x, float2 v, float2 dx, float2 dy) { return x.t.SampleGrad(x.s, v, dx, dy); }
inline float4 tex2Dbias(sampler2D x, float4 v) { return x.t.SampleBias(x.s, v.xy, v.w); }
inline float4 tex2Dlod(sampler2D x, float4 v) { return x.t.SampleLevel(x.s, v.xy, v.w); }
inline float4 tex2Dgrad(sampler2D x, float2 v, float2 dx, float2 dy) { return x.t.SampleGrad(x.s, v, dx, dy); }
inline float4 tex2Dproj(sampler2D x, float3 v) { return x.t.Sample(x.s, v.xy / v.z); }
inline float4 tex2Dproj(sampler2D x, float4 v) { return x.t.Sample(x.s, v.xy / v.w); }
inline float4 tex3D(sampler3D x, float3 v) { return x.t.Sample(x.s, v); }
inline float4 tex3Dlod(sampler3D x, float4 v) { return x.t.SampleLevel(x.s, v.xyz, v.w); }
inline float4 tex3Dbias(sampler3D x, float4 v) { return x.t.SampleBias(x.s, v.xyz, v.w); }
inline float4 texCUBE(samplerCUBE x, float3 v) { return x.t.Sample(x.s, v); }
inline float4 texCUBE(samplerCUBE x, float3 v, float3 dx, float3 dy) { return x.t.SampleGrad(x.s, v, dx, dy); }
inline float4 texCUBEbias(samplerCUBE x, float4 v) { return x.t.SampleBias(x.s, v.xyz, v.w); }
inline float4 texCUBElod(samplerCUBE x, float4 v) { return x.t.SampleLevel(x.s, v.xyz, v.w); }
inline float4 texCUBEgrad(samplerCUBE x, float3 v, float3 dx, float3 dy) { return x.t.SampleGrad(x.s, v, dx, dy); }
inline float4 texCUBEproj(samplerCUBE x, float4 v) { return x.t.Sample(x.s, v.xyz / v.w); }
inline float tex2D(sampler2DShadow x, float3 v) { return x.t.SampleCmpLevelZero(x.s, v.xy, v.z); }
inline float tex2Dproj(sampler2DShadow x, float4 v) { return x.t.SampleCmpLevelZero(x.s, v.xy / v.w, v.z / v.w); }
#endif
#line 1
)HLSL";

std::vector<Token> Tokenize(const char *src, size_t len) {
  std::vector<Token> toks;
  toks.reserve(len / 6 + 16);
  size_t i = 0;
  bool lineStart = true;
  while (i < len) {
    const char c = src[i];
    if (c == '\n') {
      lineStart = true;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      continue;
    }
    if (lineStart && c == '#') {
      while (i < len && src[i] != '\n')
        ++i;
      continue;
    }
    lineStart = false;
    if (c == '/' && i + 1 < len && src[i + 1] == '/') {
      i += 2;
      while (i < len && src[i] != '\n')
        ++i;
      continue;
    }
    if (c == '/' && i + 1 < len && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/'))
        ++i;
      i = (i + 1 < len) ? (i + 2) : len;
      continue;
    }
    if (c == '"' || c == '\'') {
      const char q = c;
      ++i;
      while (i < len && src[i] != q) {
        if (src[i] == '\\' && i + 1 < len)
          ++i;
        ++i;
      }
      if (i < len)
        ++i;
      continue;
    }
    if (IsIdentStart(c)) {
      const size_t start = i++;
      while (i < len && IsIdentChar(src[i]))
        ++i;
      toks.push_back({start, i - start, true, 0});
      continue;
    }
    toks.push_back({i, 1, false, c});
    ++i;
  }
  return toks;
}

std::string TokStr(const char *src, const Token &t) {
  return std::string(src + t.Pos, t.Len);
}

struct Edit {
  size_t Pos = 0;
  size_t Len = 0;
  std::string Replacement;
};

bool ParseRegisterSlot(const char *src, const std::vector<Token> &toks,
                       size_t &idx, std::string &slotNum) {
  // Matches `: register ( sN )`
  if (idx + 4 >= toks.size() || toks[idx].Punct != ':')
    return false;
  if (!toks[idx + 1].IsIdent || TokStr(src, toks[idx + 1]) != "register" ||
      toks[idx + 2].Punct != '(' || !toks[idx + 3].IsIdent ||
      toks[idx + 4].Punct != ')')
    return false;
  const std::string reg = TokStr(src, toks[idx + 3]);
  if (reg.size() >= 2 && (reg[0] == 's' || reg[0] == 't')) {
    slotNum = reg.substr(1);
    idx += 5;
    return true;
  }
  return false;
}

bool ContainsSubstr(const char *src, size_t len, const char *needle) {
  const size_t nlen = std::strlen(needle);
  if (nlen == 0 || nlen > len)
    return false;
  return std::search(src, src + len, needle, needle + nlen) != src + len;
}

bool MapLegacyPsReturnSemantic(const std::string &sem, std::string &mapped) {
  std::string upper = sem;
  for (char &c : upper)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (upper == "COLOR") {
    mapped = "SV_Target0";
    return true;
  }
  if (upper.size() == 6 && upper.compare(0, 5, "COLOR") == 0 &&
      upper[5] >= '0' && upper[5] <= '7') {
    mapped = std::string("SV_Target") + upper[5];
    return true;
  }
  if (upper == "DEPTH") {
    mapped = "SV_Depth";
    return true;
  }
  return false;
}

} // namespace

bool AdaptUnityHlslForDxc(const char *src, size_t len, std::string &out,
                          const char *entrypoint, bool isPixel) {
  if (!src || !len)
    return false;
  const bool hasLegacySampler =
      (ContainsSubstr(src, len, "sampler2D") ||
       ContainsSubstr(src, len, "sampler3D") ||
       ContainsSubstr(src, len, "samplerCUBE")) &&
      !ContainsSubstr(src, len, "struct sampler2D") &&
      !ContainsSubstr(src, len, "DXCB5_LEGACY_SAMPLER_SHIM");
  const bool checkPsSemantic = isPixel && entrypoint && *entrypoint &&
                               (ContainsSubstr(src, len, "COLOR") ||
                                ContainsSubstr(src, len, "color") ||
                                ContainsSubstr(src, len, "DEPTH") ||
                                ContainsSubstr(src, len, "depth"));
  if (!hasLegacySampler && !checkPsSemantic)
    return false;

  const auto toks = Tokenize(src, len);
  std::vector<Edit> edits;
  std::unordered_map<std::string, GlobalSampler> globals;

  int braceDepth = 0;
  int parenDepth = 0;
  for (size_t i = 0; i < toks.size(); ++i) {
    const Token &t = toks[i];
    if (t.Punct == '{') { ++braceDepth; continue; }
    if (t.Punct == '}') { if (braceDepth > 0) --braceDepth; continue; }
    if (t.Punct == '(') { ++parenDepth; continue; }
    if (t.Punct == ')') { if (parenDepth > 0) --parenDepth; continue; }
    if (braceDepth != 0 || parenDepth != 0 || !t.IsIdent)
      continue;

    // Rewrite legacy DX9 PS entry-point return semantic:
    //   float4 frag(...) : COLOR {  -->  float4 frag(...) : SV_Target0 {
    if (checkPsSemantic && TokStr(src, t) == entrypoint &&
        i + 1 < toks.size() && toks[i + 1].Punct == '(') {
      size_t j = i + 2;
      int p = 1;
      while (j < toks.size() && p > 0) {
        if (toks[j].Punct == '(') ++p;
        else if (toks[j].Punct == ')') --p;
        ++j;
      }
      if (p == 0 && j + 2 < toks.size() && toks[j].Punct == ':' &&
          toks[j + 1].IsIdent && toks[j + 2].Punct == '{') {
        std::string mapped;
        if (MapLegacyPsReturnSemantic(TokStr(src, toks[j + 1]), mapped)) {
          edits.push_back({toks[j + 1].Pos, toks[j + 1].Len, mapped});
        }
      }
      continue;
    }

    if (!hasLegacySampler)
      continue;

    size_t declStartIdx = i;
    size_t typeIdx = i;
    if (TokStr(src, t) == "uniform") {
      if (i + 1 >= toks.size() || !toks[i + 1].IsIdent)
        continue;
      typeIdx = i + 1;
    }
    const SamplerKind kind = ClassifySamplerType(TokStr(src, toks[typeIdx]));
    if (kind == SamplerKind::None)
      continue;

    // Parse comma-separated global variable declarations ending with ';'.
    size_t j = typeIdx + 1;
    std::string replacement;
    std::vector<std::string> names;
    bool valid = false;
    while (j < toks.size() && toks[j].IsIdent) {
      const std::string name = TokStr(src, toks[j]);
      ++j;
      if (j < toks.size() && (toks[j].Punct == '(' || toks[j].Punct == '['))
        break; // Function definition or sampler array: bail out cleanly.
      std::string slot;
      ParseRegisterSlot(src, toks, j, slot);
      replacement += TextureTypeName(kind);
      replacement += " ";
      replacement += name;
      if (!slot.empty())
        replacement += " : register(t" + slot + ")";
      replacement += "; ";
      replacement += SamplerStateTypeName(kind);
      replacement += " sampler";
      replacement += name;
      if (!slot.empty())
        replacement += " : register(s" + slot + ")";
      replacement += ";";
      names.push_back(name);
      if (j < toks.size() && toks[j].Punct == ',') {
        ++j;
        replacement += " ";
        continue;
      }
      if (j < toks.size() && toks[j].Punct == ';') {
        valid = true;
        break;
      }
      break;
    }
    if (!valid || names.empty())
      continue;

    const size_t startByte = toks[declStartIdx].Pos;
    const size_t endByte = toks[j].Pos + toks[j].Len;
    edits.push_back({startByte, endByte - startByte, replacement});
    for (const auto &n : names)
      globals[n] = {kind, n, endByte};
    i = j;
  }

  // Rewrite expression references to each rewritten global sampler variable.
  if (!globals.empty()) {
    for (size_t i = 0; i < toks.size(); ++i) {
      const Token &t = toks[i];
      if (!t.IsIdent)
        continue;
      if (i > 0 && toks[i - 1].Punct == '.')
        continue;
      const auto it = globals.find(TokStr(src, t));
      if (it == globals.end() || t.Pos < it->second.DeclEnd)
        continue;
      // Do not rewrite function parameters or local variable declarations.
      if (i > 0 && toks[i - 1].IsIdent &&
          ClassifySamplerType(TokStr(src, toks[i - 1])) != SamplerKind::None)
        continue;
      std::string call = HelperCtorName(it->second.Kind);
      call += "(";
      call += it->second.Name;
      call += ", sampler";
      call += it->second.Name;
      call += ")";
      edits.push_back({t.Pos, t.Len, std::move(call)});
    }
  }

  if (!hasLegacySampler && edits.empty())
    return false;

  std::sort(edits.begin(), edits.end(),
            [](const Edit &a, const Edit &b) { return a.Pos < b.Pos; });

  out.clear();
  out.reserve((hasLegacySampler ? sizeof(kCompatPreamble) : 0) + len +
              edits.size() * 32);
  if (hasLegacySampler)
    out.append(kCompatPreamble);
  size_t cursor = 0;
  for (const auto &e : edits) {
    if (e.Pos < cursor)
      continue;
    out.append(src + cursor, e.Pos - cursor);
    out.append(e.Replacement);
    cursor = e.Pos + e.Len;
  }
  out.append(src + cursor, len - cursor);
  return true;
}

} // namespace dxcb5_proxy
