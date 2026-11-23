// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_UNITY_PROXY_SPEC_CACHE_BRIDGE_H
#define DXCB5_UNITY_PROXY_SPEC_CACHE_BRIDGE_H

#include "dxcb5/Sha256.h"
#include "dxcb5/dxcb5_spec.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxcb5_proxy {

using PFN_dxcb5_specialize_stage = int (*)(const void *, size_t, const char *,
                                           size_t, dxcb5_blob *, dxcb5_blob *,
                                           dxcb5_blob *);
using PFN_dxcb5_free_blob = void (*)(dxcb5_blob *);

struct SpecStubDirective {
  std::string PairKey;
  std::string CacheDir;
};

inline bool IsIdentCharBridge(char C) {
  const unsigned char U = static_cast<unsigned char>(C);
  return std::isalnum(U) != 0 || C == '_';
}

inline bool ExtractJsonStringField(const std::string &Json, const char *Key,
                                   std::string &Out) {
  std::string Pattern = std::string("\"") + Key + "\"";
  size_t Pos = Json.find(Pattern);
  if (Pos == std::string::npos)
    return false;
  Pos = Json.find(':', Pos + Pattern.size());
  if (Pos == std::string::npos)
    return false;
  Pos = Json.find('"', Pos + 1);
  if (Pos == std::string::npos)
    return false;
  ++Pos;
  Out.clear();
  while (Pos < Json.size() && Json[Pos] != '"') {
    if (Json[Pos] == '\\' && Pos + 1 < Json.size()) {
      ++Pos;
      const char Esc = Json[Pos++];
      switch (Esc) {
      case '"': Out.push_back('"'); break;
      case '\\': Out.push_back('\\'); break;
      case '/': Out.push_back('/'); break;
      case 'n': Out.push_back('\n'); break;
      case 'r': Out.push_back('\r'); break;
      case 't': Out.push_back('\t'); break;
      case 'b': Out.push_back('\b'); break;
      case 'f': Out.push_back('\f'); break;
      default: Out.push_back(Esc); break;
      }
      continue;
    }
    Out.push_back(Json[Pos++]);
  }
  return Pos < Json.size() && !Out.empty();
}

inline bool IsSafePathComponent(const std::string &S) {
  if (S.empty() || S.size() > 128)
    return false;
  for (unsigned char C : S) {
    const bool Ok = (C >= '0' && C <= '9') || (C >= 'a' && C <= 'z') ||
                    (C >= 'A' && C <= 'Z') || C == '_' || C == '-';
    if (!Ok)
      return false;
  }
  return true;
}

inline std::string UnescapeLineString(std::string_view S) {
  std::string Out;
  Out.reserve(S.size());
  for (size_t I = 0; I < S.size(); ++I) {
    if (S[I] == '\\' && I + 1 < S.size() &&
        (S[I + 1] == '"' || S[I + 1] == '\\')) {
      Out.push_back(S[++I]);
      continue;
    }
    if (S[I] == '"')
      break;
    Out.push_back(S[I]);
  }
  return Out;
}

inline bool IsSafeRelativeCacheDir(const std::string &S) {
  if (S.empty() || S.size() > 256)
    return false;
  if (S[0] == '/' || S[0] == '\\')
    return false;
  size_t SegStart = 0;
  for (size_t I = 0; I <= S.size(); ++I) {
    const bool End = (I == S.size() || S[I] == '/' || S[I] == '\\');
    if (!End) {
      const unsigned char C = static_cast<unsigned char>(S[I]);
      if (C < 0x20u || C == ':' || C == '*' || C == '?' || C == '"' ||
          C == '<' || C == '>' || C == '|')
        return false;
      continue;
    }
    const size_t SegLen = I - SegStart;
    if (SegLen == 2 && S[SegStart] == '.' && S[SegStart + 1] == '.')
      return false;
    SegStart = I + 1;
  }
  return true;
}

inline bool TryParseSpecDirectiveLine(std::string_view RawLine,
                                      SpecStubDirective &Out) {
  const size_t Tag = RawLine.find("dxcb5-spec:");
  if (Tag == std::string_view::npos)
    return false;
  const std::string_view After = RawLine.substr(Tag + 11);
  std::string LineStr;
  if (Tag > 0 && RawLine[Tag - 1] == '"')
    LineStr = UnescapeLineString(After);
  else
    LineStr.assign(After);
  const size_t Brace = LineStr.find('{');
  if (Brace == std::string::npos)
    return false;
  const std::string JsonLine = LineStr.substr(Brace);
  SpecStubDirective Cand;
  if (ExtractJsonStringField(JsonLine, "pair_key", Cand.PairKey) &&
      ExtractJsonStringField(JsonLine, "cache_dir", Cand.CacheDir) &&
      IsSafePathComponent(Cand.PairKey) &&
      IsSafeRelativeCacheDir(Cand.CacheDir)) {
    Out = std::move(Cand);
    return true;
  }
  return false;
}

enum class PreprocTri : uint8_t { False = 0, True = 1, Unknown = 2 };
enum class MacroDefState : uint8_t {
  Undefined = 0,
  DefinedZero = 1,
  DefinedNonZero = 2,
  DefinedUnknown = 3
};

// Classifies a macro body for C/HLSL `#if` truthiness:
//   - literal decimal "0" -> DefinedZero
//   - literal non-zero decimal integer (e.g. "1", "42") -> DefinedNonZero
//   - empty body: DefinedNonZero when `EmptyIsOne` (D3D_SHADER_MACRO `-D FOO`),
//     DefinedUnknown in source `#define FOO` (`defined(FOO)` true, `#if FOO` unknown)
//   - anything else (e.g. "(0)", "0x0", "(SHADER_TARGET>=40)") -> DefinedUnknown
inline MacroDefState ClassifyMacroBody(std::string_view Body, bool EmptyIsOne) {
  size_t B = 0, E = Body.size();
  while (B < E && (Body[B] == ' ' || Body[B] == '\t' || Body[B] == '\r'))
    ++B;
  while (E > B && (Body[E - 1] == ' ' || Body[E - 1] == '\t' || Body[E - 1] == '\r'))
    --E;
  if (B == E)
    return EmptyIsOne ? MacroDefState::DefinedNonZero
                      : MacroDefState::DefinedUnknown;
  const std::string_view Trimmed = Body.substr(B, E - B);
  uint64_t Val = 0;
  for (char C : Trimmed) {
    if (!std::isdigit(static_cast<unsigned char>(C)))
      return MacroDefState::DefinedUnknown;
    Val = Val * 10 + static_cast<uint64_t>(C - '0');
    if (Val > 0x7fffffffULL)
      return MacroDefState::DefinedUnknown;
  }
  if (Trimmed.size() > 1 && Trimmed[0] == '0')
    return MacroDefState::DefinedUnknown; // octal literal
  return Val == 0 ? MacroDefState::DefinedZero : MacroDefState::DefinedNonZero;
}

struct PreprocMacroOracle {
  std::unordered_map<std::string, MacroDefState> Known;
  bool UnknownAsFalse = false;
  bool SawInclude = false;

  void SetFromDefines(const D3D_SHADER_MACRO *Defs) {
    if (!Defs)
      return;
    for (const D3D_SHADER_MACRO *M = Defs; M->Name; ++M) {
      const std::string_view Body = M->Definition ? std::string_view(M->Definition)
                                                  : std::string_view();
      Known[M->Name] = ClassifyMacroBody(Body, /*EmptyIsOne=*/true);
    }
  }

  PreprocTri EvalDefined(std::string_view Name) const {
    if (SawInclude)
      return PreprocTri::Unknown;
    auto It = Known.find(std::string(Name));
    if (It != Known.end())
      return It->second == MacroDefState::Undefined ? PreprocTri::False
                                                    : PreprocTri::True;
    return UnknownAsFalse ? PreprocTri::False : PreprocTri::Unknown;
  }

  PreprocTri EvalValue(std::string_view Name) const {
    if (SawInclude)
      return PreprocTri::Unknown;
    auto It = Known.find(std::string(Name));
    if (It != Known.end()) {
      if (It->second == MacroDefState::DefinedNonZero)
        return PreprocTri::True;
      if (It->second == MacroDefState::Undefined ||
          It->second == MacroDefState::DefinedZero)
        return PreprocTri::False;
      return PreprocTri::Unknown;
    }
    return UnknownAsFalse ? PreprocTri::False : PreprocTri::Unknown;
  }
};

struct PreprocCondParser {
  std::string_view S;
  size_t Pos = 0;
  const PreprocMacroOracle &Oracle;
  unsigned Depth = 0;

  void SkipWs() {
    while (Pos < S.size() &&
           (S[Pos] == ' ' || S[Pos] == '\t' || S[Pos] == '\r'))
      ++Pos;
  }

  static PreprocTri TriNot(PreprocTri V) {
    if (V == PreprocTri::True) return PreprocTri::False;
    if (V == PreprocTri::False) return PreprocTri::True;
    return PreprocTri::Unknown;
  }

  static PreprocTri TriAnd(PreprocTri A, PreprocTri B) {
    if (A == PreprocTri::False || B == PreprocTri::False) return PreprocTri::False;
    if (A == PreprocTri::True && B == PreprocTri::True) return PreprocTri::True;
    return PreprocTri::Unknown;
  }

  static PreprocTri TriOr(PreprocTri A, PreprocTri B) {
    if (A == PreprocTri::True || B == PreprocTri::True) return PreprocTri::True;
    if (A == PreprocTri::False && B == PreprocTri::False) return PreprocTri::False;
    return PreprocTri::Unknown;
  }

  bool ParseIdent(std::string_view &Out) {
    SkipWs();
    if (Pos >= S.size())
      return false;
    const unsigned char C0 = static_cast<unsigned char>(S[Pos]);
    if (!(std::isalpha(C0) || C0 == '_'))
      return false;
    const size_t Start = Pos++;
    while (Pos < S.size() && IsIdentCharBridge(S[Pos]))
      ++Pos;
    Out = S.substr(Start, Pos - Start);
    return true;
  }

  PreprocTri ParseUnary() {
    if (++Depth > 32)
      return PreprocTri::Unknown;
    SkipWs();
    PreprocTri Res = PreprocTri::Unknown;
    if (Pos < S.size() && S[Pos] == '!') {
      ++Pos;
      Res = TriNot(ParseUnary());
    } else if (Pos < S.size() && S[Pos] == '(') {
      ++Pos;
      Res = ParseOr();
      SkipWs();
      if (Pos >= S.size() || S[Pos] != ')')
        Res = PreprocTri::Unknown;
      else
        ++Pos;
    } else if (Pos < S.size() && std::isdigit(static_cast<unsigned char>(S[Pos]))) {
      uint64_t Val = 0;
      bool Valid = true;
      if (Pos + 1 < S.size() && S[Pos] == '0' && (S[Pos + 1] == 'x' || S[Pos + 1] == 'X')) {
        Valid = false;
      } else {
        while (Pos < S.size() && std::isdigit(static_cast<unsigned char>(S[Pos])))
          Val = Val * 10 + static_cast<uint64_t>(S[Pos++] - '0');
        if (Pos < S.size() && IsIdentCharBridge(S[Pos]))
          Valid = false;
      }
      Res = Valid ? (Val != 0 ? PreprocTri::True : PreprocTri::False)
                  : PreprocTri::Unknown;
    } else {
      std::string_view Id;
      if (ParseIdent(Id)) {
        if (Id == "defined") {
          SkipWs();
          const bool Paren = (Pos < S.size() && S[Pos] == '(');
          if (Paren)
            ++Pos;
          std::string_view Macro;
          if (ParseIdent(Macro)) {
            SkipWs();
            if (!Paren || (Pos < S.size() && S[Pos] == ')')) {
              if (Paren)
                ++Pos;
              Res = Oracle.EvalDefined(Macro);
            }
          }
        } else {
          Res = Oracle.EvalValue(Id);
        }
      }
    }
    --Depth;
    return Res;
  }

  PreprocTri ParseAnd() {
    PreprocTri V = ParseUnary();
    for (;;) {
      SkipWs();
      if (Pos + 1 < S.size() && S[Pos] == '&' && S[Pos + 1] == '&') {
        Pos += 2;
        V = TriAnd(V, ParseUnary());
      } else {
        break;
      }
    }
    return V;
  }

  PreprocTri ParseOr() {
    PreprocTri V = ParseAnd();
    for (;;) {
      SkipWs();
      if (Pos + 1 < S.size() && S[Pos] == '|' && S[Pos + 1] == '|') {
        Pos += 2;
        V = TriOr(V, ParseAnd());
      } else {
        break;
      }
    }
    return V;
  }

  PreprocTri EvalFull() {
    const PreprocTri V = ParseOr();
    SkipWs();
    return Pos == S.size() ? V : PreprocTri::Unknown;
  }
};

// Preprocessor-aware spec-stub directive resolver with strict tri-state safety.
// Correctly dispatches multi-stub conditional passes (e.g., ForwardAdd with
// `#if defined(POINT) ... #elif defined(SPOT) ... #else ... #endif`) while
// failing safe (returning false -> caller falls back to full `var:` key) if:
//   - any `dxcb5-spec:` sits inside an unevaluable / unknown `#if` branch;
//   - an `#include` precedes a conditional `dxcb5-spec:`;
//   - two active directives disagree on `(PairKey, CacheDir)`;
//   - the active branch contains no `dxcb5-spec:` directive.
inline bool ResolveSpecStubDirective(const char *Src, size_t Len,
                                     PreprocMacroOracle Oracle,
                                     SpecStubDirective &Out) {
  if (!Src || Len < 16)
    return false;
  constexpr size_t kMaxScanLen = 16u * 1024u * 1024u;
  const size_t ScanLen = Len < kMaxScanLen ? Len : kMaxScanLen;
  const std::string_view View(Src, ScanLen);
  if (View.find("dxcb5-spec:") == std::string_view::npos)
    return false;

  struct CondFrame {
    bool ParentActive = true;
    bool ParentUnknown = false;
    bool BranchTaken = false;
    bool AnyPriorUnknown = false;
    bool CurActive = true;
    bool CurUnknown = false;
    bool SeenElse = false;
  };
  std::vector<CondFrame> Stack;
  bool InBlockComment = false;
  bool HasActiveDir = false;
  SpecStubDirective ActiveDir;

  auto IsDirectiveKw = [](std::string_view Rest, const char *Kw, size_t KwLen) {
    return Rest.size() >= KwLen && Rest.compare(0, KwLen, Kw) == 0 &&
           (Rest.size() == KwLen || !IsIdentCharBridge(Rest[KwLen]));
  };

  size_t Pos = 0;
  while (Pos < View.size()) {
    size_t LineEnd = View.find('\n', Pos);
    if (LineEnd == std::string_view::npos)
      LineEnd = View.size();
    const std::string_view RawLine = View.substr(Pos, LineEnd - Pos);
    Pos = LineEnd + 1;

    // Strip `/* ... */` block comments (respecting string literals) into
    // `NoBlockLine` (preserving `// ...` and strings for `dxcb5-spec:` tags),
    // and also strip `// ...` into `CleanLine` for `#if/#define` directives.
    std::string NoBlockLine;
    std::string CleanLine;
    bool UseClean = InBlockComment;
    if (!UseClean && (RawLine.find("/*") != std::string_view::npos ||
                      RawLine.find("//") != std::string_view::npos))
      UseClean = true;
    if (UseClean) {
      NoBlockLine.reserve(RawLine.size());
      CleanLine.reserve(RawLine.size());
      bool InLineComment = false;
      bool InString = false;
      for (size_t K = 0; K < RawLine.size();) {
        if (InBlockComment) {
          if (RawLine[K] == '*' && K + 1 < RawLine.size() && RawLine[K + 1] == '/') {
            InBlockComment = false;
            K += 2;
            NoBlockLine.push_back(' ');
            CleanLine.push_back(' ');
          } else {
            ++K;
          }
        } else if (InLineComment) {
          NoBlockLine.push_back(RawLine[K++]);
        } else if (InString) {
          if (RawLine[K] == '\\' && K + 1 < RawLine.size()) {
            NoBlockLine.push_back(RawLine[K]);
            CleanLine.push_back(RawLine[K]);
            ++K;
          } else if (RawLine[K] == '"') {
            InString = false;
          }
          NoBlockLine.push_back(RawLine[K]);
          CleanLine.push_back(RawLine[K]);
          ++K;
        } else if (RawLine[K] == '"') {
          InString = true;
          NoBlockLine.push_back(RawLine[K]);
          CleanLine.push_back(RawLine[K]);
          ++K;
        } else if (RawLine[K] == '/' && K + 1 < RawLine.size() &&
                   RawLine[K + 1] == '/') {
          InLineComment = true;
          NoBlockLine.push_back(RawLine[K++]);
          NoBlockLine.push_back(RawLine[K++]);
        } else if (RawLine[K] == '/' && K + 1 < RawLine.size() &&
                   RawLine[K + 1] == '*') {
          InBlockComment = true;
          K += 2;
        } else {
          NoBlockLine.push_back(RawLine[K]);
          CleanLine.push_back(RawLine[K]);
          ++K;
        }
      }
    }
    const std::string_view TagLine = UseClean ? std::string_view(NoBlockLine) : RawLine;
    const std::string_view Line = UseClean ? std::string_view(CleanLine) : RawLine;

    const bool CurActive = Stack.empty() ? true : Stack.back().CurActive;
    const bool CurUnknown = Stack.empty() ? false : Stack.back().CurUnknown;

    if (TagLine.find("dxcb5-spec:") != std::string_view::npos) {
      SpecStubDirective Cand;
      if (TryParseSpecDirectiveLine(TagLine, Cand)) {
        if (CurUnknown)
          return false;
        if (CurActive) {
          if (HasActiveDir &&
              (Cand.PairKey != ActiveDir.PairKey ||
               Cand.CacheDir != ActiveDir.CacheDir))
            return false;
          ActiveDir = std::move(Cand);
          HasActiveDir = true;
        }
      }
    }

    size_t I = 0;
    while (I < Line.size() &&
           (Line[I] == ' ' || Line[I] == '\t' || Line[I] == '\r'))
      ++I;
    if (I >= Line.size() || Line[I] != '#')
      continue;
    ++I;
    while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t'))
      ++I;
    const std::string_view Rest = Line.substr(I);

    if (IsDirectiveKw(Rest, "ifdef", 5) || IsDirectiveKw(Rest, "ifndef", 6) ||
        IsDirectiveKw(Rest, "if", 2)) {
      CondFrame F;
      F.ParentActive = CurActive;
      F.ParentUnknown = CurUnknown;
      if (!CurActive && !CurUnknown) {
        F.CurActive = false;
        F.CurUnknown = false;
      } else if (CurUnknown) {
        F.CurActive = false;
        F.CurUnknown = true;
        F.AnyPriorUnknown = true;
      } else {
        PreprocTri V = PreprocTri::Unknown;
        if (IsDirectiveKw(Rest, "ifdef", 5) || IsDirectiveKw(Rest, "ifndef", 6)) {
          const bool IsN = Rest[2] == 'n';
          PreprocCondParser P{Rest.substr(IsN ? 6 : 5), 0, Oracle};
          std::string_view Id;
          if (P.ParseIdent(Id)) {
            P.SkipWs();
            if (P.Pos == P.S.size())
              V = IsN ? PreprocCondParser::TriNot(Oracle.EvalDefined(Id))
                      : Oracle.EvalDefined(Id);
          }
        } else {
          PreprocCondParser P{Rest.substr(2), 0, Oracle};
          V = P.EvalFull();
        }
        if (V == PreprocTri::True) {
          F.CurActive = true;
          F.BranchTaken = true;
        } else if (V == PreprocTri::False) {
          F.CurActive = false;
        } else {
          F.CurActive = false;
          F.CurUnknown = true;
          F.AnyPriorUnknown = true;
        }
      }
      Stack.push_back(F);
      continue;
    }

    if (IsDirectiveKw(Rest, "elif", 4)) {
      if (Stack.empty() || Stack.back().SeenElse)
        return false;
      auto &F = Stack.back();
      if ((!F.ParentActive && !F.ParentUnknown) || F.BranchTaken) {
        F.CurActive = false;
        F.CurUnknown = false;
      } else if (F.ParentUnknown) {
        F.CurActive = false;
        F.CurUnknown = true;
      } else {
        PreprocCondParser P{Rest.substr(4), 0, Oracle};
        const PreprocTri V = P.EvalFull();
        if (F.AnyPriorUnknown) {
          F.CurActive = false;
          F.CurUnknown = (V != PreprocTri::False);
        } else if (V == PreprocTri::True) {
          F.CurActive = true;
          F.CurUnknown = false;
          F.BranchTaken = true;
        } else if (V == PreprocTri::False) {
          F.CurActive = false;
          F.CurUnknown = false;
        } else {
          F.CurActive = false;
          F.CurUnknown = true;
          F.AnyPriorUnknown = true;
        }
      }
      continue;
    }

    if (IsDirectiveKw(Rest, "else", 4)) {
      if (Stack.empty() || Stack.back().SeenElse)
        return false;
      auto &F = Stack.back();
      F.SeenElse = true;
      if ((!F.ParentActive && !F.ParentUnknown) || F.BranchTaken) {
        F.CurActive = false;
        F.CurUnknown = false;
      } else if (F.ParentUnknown || F.AnyPriorUnknown) {
        F.CurActive = false;
        F.CurUnknown = true;
      } else {
        F.CurActive = true;
        F.CurUnknown = false;
        F.BranchTaken = true;
      }
      continue;
    }

    if (IsDirectiveKw(Rest, "endif", 5)) {
      if (Stack.empty())
        return false;
      Stack.pop_back();
      continue;
    }

    if (!CurActive && !CurUnknown)
      continue;

    if (IsDirectiveKw(Rest, "include", 7)) {
      Oracle.SawInclude = true;
      continue;
    }

    const bool IsDef = IsDirectiveKw(Rest, "define", 6);
    const bool IsUndef = !IsDef && IsDirectiveKw(Rest, "undef", 5);
    if (!IsDef && !IsUndef)
      continue;
    size_t J = IsDef ? 6 : 5;
    while (J < Rest.size() && (Rest[J] == ' ' || Rest[J] == '\t'))
      ++J;
    const size_t NameStart = J;
    while (J < Rest.size() && IsIdentCharBridge(Rest[J]))
      ++J;
    if (J == NameStart)
      continue;
    const std::string Name(Rest.substr(NameStart, J - NameStart));
    const bool IsFuncMacro = IsDef && J < Rest.size() && Rest[J] == '(';
    if (CurUnknown) {
      Oracle.Known.erase(Name);
      continue;
    }
    if (IsUndef) {
      Oracle.Known[Name] = MacroDefState::Undefined;
      continue;
    }
    if (IsFuncMacro) {
      // Function-like macro: `defined(Name)` is true, value in `#if Name` is unknown.
      Oracle.Known[Name] = MacroDefState::DefinedUnknown;
      continue;
    }
    while (J < Rest.size() && (Rest[J] == ' ' || Rest[J] == '\t'))
      ++J;
    Oracle.Known[Name] = ClassifyMacroBody(Rest.substr(J), /*EmptyIsOne=*/false);
  }

  if (!Stack.empty() || !HasActiveDir)
    return false;
  Out = std::move(ActiveDir);
  return true;
}

inline bool ResolveSpecCacheDir(const std::wstring &CacheDirW,
                                const std::wstring &ProjectRoot,
                                std::wstring &OutResolved) {
  if (ProjectRoot.empty() || CacheDirW.empty())
    return false;
  wchar_t RootFull[4096] = {};
  const DWORD RootN = GetFullPathNameW(ProjectRoot.c_str(), 4096, RootFull, nullptr);
  if (RootN == 0 || RootN >= 4096)
    return false;
  std::wstring Root(RootFull, RootN);
  while (!Root.empty() && (Root.back() == L'\\' || Root.back() == L'/'))
    Root.pop_back();
  const std::wstring Combined = Root + L"\\" + CacheDirW;
  wchar_t FullBuf[4096] = {};
  const DWORD FullN = GetFullPathNameW(Combined.c_str(), 4096, FullBuf, nullptr);
  if (FullN == 0 || FullN >= 4096)
    return false;
  std::wstring Resolved(FullBuf, FullN);
  if (Resolved.size() <= Root.size() ||
      _wcsnicmp(Resolved.c_str(), Root.c_str(), Root.size()) != 0 ||
      (Resolved[Root.size()] != L'\\' && Resolved[Root.size()] != L'/'))
    return false;
  OutResolved = std::move(Resolved);
  return true;
}

struct ActiveDefineMap {
  std::unordered_map<std::string, bool> Entries;

  bool IsSet(const char *Name) const {
    auto It = Entries.find(Name);
    return It != Entries.end() && It->second;
  }
};

inline void CollectActiveDefines(const D3D_SHADER_MACRO *Defs, const char *Src,
                                 size_t SrcLen, ActiveDefineMap &Out) {
  Out.Entries.clear();
  if (Defs) {
    for (const D3D_SHADER_MACRO *M = Defs; M->Name; ++M) {
      const bool Active =
          (!M->Definition || !*M->Definition || std::strcmp(M->Definition, "0") != 0);
      Out.Entries[M->Name] = Active;
    }
  }
  if (!Src || !SrcLen)
    return;
  constexpr size_t kMaxScanLen = 16u * 1024u * 1024u;
  const size_t ScanLen = SrcLen < kMaxScanLen ? SrcLen : kMaxScanLen;
  const std::string_view View(Src, ScanLen);
  int PreprocDepth = 0;
  bool InBlockComment = false;
  size_t Pos = 0;
  while (Pos < View.size()) {
    size_t LineEnd = View.find('\n', Pos);
    if (LineEnd == std::string_view::npos)
      LineEnd = View.size();
    std::string_view Line = View.substr(Pos, LineEnd - Pos);
    Pos = LineEnd + 1;
    // Strip multi-line /* ... */ and trailing // comments before parsing directives.
    std::string CleanLine;
    bool UseClean = InBlockComment;
    if (!UseClean && (Line.find("/*") != std::string_view::npos ||
                      Line.find("//") != std::string_view::npos))
      UseClean = true;
    if (UseClean) {
      CleanLine.reserve(Line.size());
      for (size_t K = 0; K < Line.size();) {
        if (InBlockComment) {
          if (Line[K] == '*' && K + 1 < Line.size() && Line[K + 1] == '/') {
            InBlockComment = false;
            K += 2;
            CleanLine.push_back(' ');
          } else {
            ++K;
          }
        } else if (Line[K] == '/' && K + 1 < Line.size() && Line[K + 1] == '/') {
          break;
        } else if (Line[K] == '/' && K + 1 < Line.size() && Line[K + 1] == '*') {
          InBlockComment = true;
          K += 2;
        } else {
          CleanLine.push_back(Line[K++]);
        }
      }
      Line = CleanLine;
    }
    size_t I = 0;
    while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t' || Line[I] == '\r'))
      ++I;
    if (I >= Line.size())
      continue;
    if (Line[I] == '#') {
      ++I;
      while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t'))
        ++I;
      const std::string_view Rest = Line.substr(I);
      if (Rest.rfind("if", 0) == 0 &&
          (Rest.size() == 2 || !IsIdentCharBridge(Rest[2]) ||
           Rest.rfind("ifdef", 0) == 0 || Rest.rfind("ifndef", 0) == 0)) {
        ++PreprocDepth;
        continue;
      }
      if (Rest.rfind("endif", 0) == 0 &&
          (Rest.size() == 5 || !IsIdentCharBridge(Rest[5]))) {
        if (PreprocDepth > 0)
          --PreprocDepth;
        continue;
      }
      if (PreprocDepth != 0)
        continue;
      const bool IsDef = Rest.rfind("define", 0) == 0 &&
                         I + 6 < Line.size() &&
                         (Line[I + 6] == ' ' || Line[I + 6] == '\t');
      const bool IsUndef = !IsDef && Rest.rfind("undef", 0) == 0 &&
                           I + 5 < Line.size() &&
                           (Line[I + 5] == ' ' || Line[I + 5] == '\t');
      if (!IsDef && !IsUndef)
        continue;
      I += IsDef ? 6 : 5;
      while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t'))
        ++I;
      const size_t NameStart = I;
      while (I < Line.size() && IsIdentCharBridge(Line[I]))
        ++I;
      if (I == NameStart || (I < Line.size() && Line[I] == '('))
        continue;
      std::string Name(Line.substr(NameStart, I - NameStart));
      if (IsUndef) {
        Out.Entries[std::move(Name)] = false;
        continue;
      }
      while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t'))
        ++I;
      size_t ValEnd = I;
      while (ValEnd < Line.size() && Line[ValEnd] != '\r' &&
             Line[ValEnd] != ' ' && Line[ValEnd] != '\t' &&
             !(Line[ValEnd] == '/' && ValEnd + 1 < Line.size() &&
               (Line[ValEnd + 1] == '/' || Line[ValEnd + 1] == '*')))
        ++ValEnd;
      const std::string_view Val = Line.substr(I, ValEnd - I);
      Out.Entries[std::move(Name)] = (Val.empty() || Val != "0");
      continue;
    }
    if (PreprocDepth != 0)
      continue;
    // Preprocessor-proof sentinel: `static const int _dxcb5_def_<NAME> = 1;`
    if (Line[I] == '/' && I + 1 < Line.size() &&
        (Line[I + 1] == '/' || Line[I + 1] == '*'))
      continue;
    const size_t Marker = Line.find("_dxcb5_def_", I);
    if (Marker != std::string_view::npos) {
      const size_t NameStart = Marker + 11;
      size_t NameEnd = NameStart;
      while (NameEnd < Line.size() && IsIdentCharBridge(Line[NameEnd]))
        ++NameEnd;
      if (NameEnd > NameStart)
        Out.Entries[std::string(Line.substr(NameStart, NameEnd - NameStart))] = true;
    }
  }
}

inline std::wstring DetectArchetypeDir(const ActiveDefineMap &Defs) {
  const bool Spsi = Defs.IsSet("STEREO_INSTANCING_ON");
  const bool Sps = Defs.IsSet("UNITY_SINGLE_PASS_STEREO");
  const bool Inst = Defs.IsSet("INSTANCING_ON");
  if (Spsi)
    return Inst ? L"spsi_instancing" : L"spsi";
  if (Sps)
    return Inst ? L"sps_instancing" : L"sps";
  return Inst ? L"instancing" : L"desktop";
}

inline bool ReadWholeFile(const std::wstring &Path, std::string &Out) {
  Out.clear();
  HANDLE H = CreateFileW(Path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (H == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER Sz{};
  bool Ok = false;
  if (GetFileSizeEx(H, &Sz) && Sz.QuadPart > 0 && Sz.QuadPart <= 64 * 1024 * 1024) {
    Out.resize(static_cast<size_t>(Sz.QuadPart));
    DWORD Read = 0;
    Ok = ReadFile(H, &Out[0], static_cast<DWORD>(Out.size()), &Read, nullptr) &&
         Read == Out.size();
    if (!Ok)
      Out.clear();
  }
  CloseHandle(H);
  return Ok;
}

inline void WriteFileAtomic(const std::wstring &Path, const void *Data,
                            size_t Size) {
  wchar_t Suffix[64];
  std::swprintf(Suffix, 64, L".tmp.%lu.%lu",
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long>(GetCurrentThreadId()));
  const std::wstring TmpPath = Path + Suffix;
  HANDLE H = CreateFileW(TmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (H == INVALID_HANDLE_VALUE)
    return;
  DWORD Written = 0;
  const BOOL Ok = WriteFile(H, Data, static_cast<DWORD>(Size), &Written, nullptr);
  CloseHandle(H);
  if (!(Ok && Written == Size &&
        MoveFileExW(TmpPath.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING)))
    DeleteFileW(TmpPath.c_str());
}

inline void ParseSpecFlagsList(const std::string &MetaJson,
                               std::vector<std::string> &Flags,
                               const char *StageKey = nullptr) {
  Flags.clear();
  size_t Key = std::string::npos;
  if (StageKey && *StageKey) {
    Key = MetaJson.find(StageKey);
    // Only fall back to single-stage "live_spec_flags" when MetaJson declares
    // that exact stage; otherwise fall back to full "spec_flags" so VS never
    // inherits PS-only live flags (or vice versa).
    if (Key == std::string::npos) {
      std::string MetaStage;
      if (ExtractJsonStringField(MetaJson, "stage", MetaStage)) {
        const bool WantPs = std::strstr(StageKey, "ps_") != nullptr;
        const bool WantVs = std::strstr(StageKey, "vs_") != nullptr;
        if ((WantPs && MetaStage.rfind("ps_", 0) == 0) ||
            (WantVs && MetaStage.rfind("vs_", 0) == 0))
          Key = MetaJson.find("\"live_spec_flags\"");
      }
    }
  } else {
    Key = MetaJson.find("\"live_spec_flags\"");
  }
  if (Key == std::string::npos)
    Key = MetaJson.find("\"spec_flags\"");
  if (Key == std::string::npos)
    return;
  const size_t Open = MetaJson.find('[', Key);
  const size_t Close = Open == std::string::npos ? std::string::npos
                                                 : MetaJson.find(']', Open);
  if (Close == std::string::npos)
    return;
  size_t Pos = Open + 1;
  while (Pos < Close) {
    const size_t Q0 = MetaJson.find('"', Pos);
    if (Q0 == std::string::npos || Q0 >= Close)
      break;
    const size_t Q1 = MetaJson.find('"', Q0 + 1);
    if (Q1 == std::string::npos || Q1 >= Close)
      break;
    Flags.push_back(MetaJson.substr(Q0 + 1, Q1 - Q0 - 1));
    Pos = Q1 + 1;
  }
  std::sort(Flags.begin(), Flags.end());
  Flags.erase(std::unique(Flags.begin(), Flags.end()), Flags.end());
}

inline std::wstring HashDefinesHex(const std::vector<std::pair<std::string, uint32_t>> &KV) {
  dxcb5::Sha256 H;
  const uint32_t Epoch = DXCB5_CACHE_EPOCH;
  H.UpdatePod(Epoch);
  const uint64_t Count = static_cast<uint64_t>(KV.size());
  H.UpdatePod(Count);
  for (const auto &Item : KV) {
    H.UpdateDelim(Item.first);
    H.UpdatePod(Item.second);
  }
  const std::string Hex = H.Hex32();
  return std::wstring(Hex.begin(), Hex.end());
}

// Normalizes D3DCompile `Flags1` to the non-default semantic bits that change
// compiled DXBC behavior (matrix packing, IEEE strictness, opt level, flow
// control). Standard Unity defaults (column-major / -O3 / backwards-compat)
// normalize to 0 so default pre-baked `stage_<defHash>.dxbc` filenames remain
// directly usable while non-default flags (e.g. `-Zpr`, `-Gis`, `-Od`) get
// isolated cache keys instead of colliding with default blobs (H5).
inline uint32_t NormalizeSpecSemanticFlags(uint32_t Flags1) {
  uint32_t Out = 0u;
  if (Flags1 & (1u << 2))  // D3DCOMPILE_SKIP_OPTIMIZATION (-Od)
    Out |= (1u << 2);
  if (Flags1 & (1u << 3))  // D3DCOMPILE_PACK_MATRIX_ROW_MAJOR (-Zpr)
    Out |= (1u << 3);
  if (Flags1 & (1u << 9))  // D3DCOMPILE_AVOID_FLOW_CONTROL (-Gfa)
    Out |= (1u << 9);
  if (Flags1 & (1u << 10)) // D3DCOMPILE_PREFER_FLOW_CONTROL (-Gfp)
    Out |= (1u << 10);
  if (Flags1 & (1u << 13)) // D3DCOMPILE_IEEE_STRICTNESS (-Gis)
    Out |= (1u << 13);
  const uint32_t OptBits = Flags1 & ((1u << 14) | (1u << 15));
  if (OptBits != 0u && OptBits != (1u << 15)) // non-O3 explicit level (O0/O1/O2)
    Out |= OptBits;
  return Out;
}

inline std::wstring BuildSpecFlagsSuffix(uint32_t Flags1, bool NanSafe) {
  std::wstring Suf = NanSafe ? L"_nansafe" : L"";
  const uint32_t Sem = NormalizeSpecSemanticFlags(Flags1);
  if (Sem != 0u) {
    wchar_t Buf[16];
    std::swprintf(Buf, 16, L"_f%x", static_cast<unsigned>(Sem));
    Suf += Buf;
  }
  return Suf;
}

inline void AppendJsonEscaped(std::string &Out, std::string_view S) {
  Out.push_back('"');
  for (unsigned char C : S) {
    switch (C) {
    case '"': Out += "\\\""; break;
    case '\\': Out += "\\\\"; break;
    case '\n': Out += "\\n"; break;
    case '\r': Out += "\\r"; break;
    case '\t': Out += "\\t"; break;
    default:
      if (C < 0x20u) {
        char Esc[8];
        std::snprintf(Esc, sizeof(Esc), "\\u%04x", C);
        Out += Esc;
      } else {
        Out.push_back(static_cast<char>(C));
      }
      break;
    }
  }
  Out.push_back('"');
}

inline std::string BuildDefinesManifestJson(
    const std::vector<std::pair<std::string, uint32_t>> &KV, bool NanSafe) {
  std::string Json = "{\"schema_version\":1,\"defines\":{";
  for (size_t I = 0; I < KV.size(); ++I) {
    if (I)
      Json.push_back(',');
    AppendJsonEscaped(Json, KV[I].first);
    Json.push_back(':');
    Json += std::to_string(KV[I].second);
  }
  Json += "},\"options\":{\"compact_globals\":true,\"nan_safe\":";
  Json += NanSafe ? "true" : "false";
  Json += "}}";
  return Json;
}

} // namespace dxcb5_proxy

#endif // DXCB5_UNITY_PROXY_SPEC_CACHE_BRIDGE_H
