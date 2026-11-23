// Distributed under the University of Illinois Open Source License.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "MinHook.h"
#include "UnityTypes.h"
#include "VariantCacheAndContext.h"
#include "dxcb5/UnityHookApi.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace dxcb5_hook;

namespace {

// 1. Test vendored MinHook v1.3.4 end-to-end (create, enable, call, disable, re-enable).
using PFN_AddFn = int(__cdecl *)(int a, int b);
PFN_AddFn gOrigAddTramp = nullptr;
int gDetourCalls = 0;

__declspec(noinline) int __cdecl TargetAddFn(int a, int b) {
  volatile int x = a + b;
  return x;
}

__declspec(noinline) __declspec(guard(nocf)) int CallAddTramp(int a, int b) {
  return gOrigAddTramp(a, b);
}

int __cdecl AddDetour(int a, int b) {
  ++gDetourCalls;
  return CallAddTramp(a, b) + 100;
}

bool TestMinHookLifecycle() {
  if (TargetAddFn(3, 4) != 7) {
    std::cerr << "Pre-hook TargetAddFn failed\n";
    return false;
  }
  if (MH_Initialize() != MH_OK) {
    std::cerr << "MH_Initialize failed\n";
    return false;
  }
  void *orig = nullptr;
  if (MH_CreateHook(reinterpret_cast<void *>(&TargetAddFn),
                    reinterpret_cast<void *>(&AddDetour), &orig) != MH_OK ||
      !orig) {
    std::cerr << "MH_CreateHook failed\n";
    return false;
  }
  RegisterCfgCallTarget(orig);
  gOrigAddTramp = reinterpret_cast<PFN_AddFn>(orig);
  if (MH_EnableHook(reinterpret_cast<void *>(&TargetAddFn)) != MH_OK) {
    std::cerr << "MH_EnableHook failed\n";
    return false;
  }
  if (TargetAddFn(3, 4) != 107 || gDetourCalls != 1) {
    std::cerr << "Post-enable MinHook call failed\n";
    return false;
  }
  if (MH_DisableHook(reinterpret_cast<void *>(&TargetAddFn)) != MH_OK) {
    std::cerr << "MH_DisableHook failed\n";
    return false;
  }
  if (TargetAddFn(3, 4) != 7 || gDetourCalls != 1) {
    std::cerr << "Post-disable TargetAddFn failed\n";
    return false;
  }
  if (MH_EnableHook(reinterpret_cast<void *>(&TargetAddFn)) != MH_OK ||
      TargetAddFn(10, 20) != 130 || gDetourCalls != 2) {
    std::cerr << "Re-enable MinHook failed\n";
    return false;
  }
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();
  return true;
}

bool TestUnityTypesAndReplay() {
  // SSO inline UnityCoreString ("MT2Frag", len = 7 -> rem = 24 - 7 = 17).
  UnityCoreString sso{};
  std::memcpy(sso.Raw, "MT2Frag", 8);
  sso.Raw[0x18] = static_cast<uint8_t>(24 - 7);
  sso.Raw[0x20] = 1;
  if (sso.View() != "MT2Frag") {
    std::cerr << "SSO UnityCoreString mismatch\n";
    return false;
  }

  // Heap UnityCoreString.
  const char *heapStr = "Assets/Shaders/LongShaderName.shader";
  UnityCoreString heap{};
  const uint64_t heapLen = std::strlen(heapStr);
  std::memcpy(heap.Raw + 0x00, &heapStr, sizeof(heapStr));
  std::memcpy(heap.Raw + 0x10, &heapLen, sizeof(heapLen));
  heap.Raw[0x20] = 0;
  if (heap.View() != heapStr) {
    std::cerr << "Heap UnityCoreString mismatch\n";
    return false;
  }

  // RecordingReflectionReport -> ReplayTo round-trip across all 9 callbacks.
  struct VerifyReport final : public UnityReflectionReport {
    int Events = 0;
    bool Ok = true;
    void OnInputBinding(int c, int b) override { ++Events; Ok &= (c == 1 && b == 2); }
    void OnConstantBuffer(const char *n, int s, int v) override {
      ++Events; Ok &= (std::string(n) == "$Globals" && s == 32 && v == 2);
    }
    void OnConstant(const char *n, int idx, int pt, int ct, int r, int c, int a) override {
      ++Events;
      Ok &= (std::string(n) == "_Color" && idx == 0 && pt == 1 && ct == 2 &&
             r == 1 && c == 4 && a == 1);
    }
    void OnCBBinding(const char *n, int idx) override {
      ++Events; Ok &= (std::string(n) == "$Globals" && idx == 0);
    }
    void OnTextureBinding(const char *n, int idx, int si, bool ms, int dim) override {
      ++Events; Ok &= (std::string(n) == "_MainTex" && idx == 0 && si == 0 && !ms && dim == 2);
    }
    void OnInlineSampler(uint32_t st, int idx) override { ++Events; Ok &= (st == 7 && idx == 1); }
    void OnBufferBinding(const char *n, int idx, int ex) override {
      ++Events; Ok &= (std::string(n) == "_Buf" && idx == 3 && ex == 0);
    }
    void OnUAVBinding(const char *n, int idx, int oi) override {
      ++Events; Ok &= (std::string(n) == "_Uav" && idx == 4 && oi == 4);
    }
    void OnStatsInfo(int a, int t, int f, int tmp) override {
      ++Events; Ok &= (a == 10 && t == 2 && f == 1 && tmp == 3);
    }
  };

  VerifyReport live, replayed;
  RecordingReflectionReport rec(&live);
  rec.OnInputBinding(1, 2);
  rec.OnConstantBuffer("$Globals", 32, 2);
  rec.OnConstant("_Color", 0, 1, 2, 1, 4, 1);
  rec.OnCBBinding("$Globals", 0);
  rec.OnTextureBinding("_MainTex", 0, 0, false, 2);
  rec.OnInlineSampler(7, 1);
  rec.OnBufferBinding("_Buf", 3, 0);
  rec.OnUAVBinding("_Uav", 4, 4);
  rec.OnStatsInfo(10, 2, 1, 3);
  rec.Data.ReplayTo(&replayed);
  if (!live.Ok || !replayed.Ok || live.Events != 9 || replayed.Events != 9) {
    std::cerr << "Reflection record/replay mismatch\n";
    return false;
  }

  // Test VariantReplayCache reflection guard: entry inserted without reflection
  // must NEVER satisfy a lookup that requires reflection!
  VariantReplayCache cache;
  CachedSnippetResult noRefl;
  noRefl.OutProgramType = 1;
  noRefl.Code = {1, 2, 3, 4};
  noRefl.HasReflection = false;
  cache.Insert("k1", noRefl);
  CachedSnippetResult got;
  if (cache.Lookup("k1", true, got)) {
    std::cerr << "VariantReplayCache served reflection-less entry to reflection caller\n";
    return false;
  }
  if (!cache.Lookup("k1", false, got) || got.Code.size() != 4) {
    std::cerr << "VariantReplayCache missed reflection-less lookup\n";
    return false;
  }
  CachedSnippetResult withRefl = noRefl;
  withRefl.HasReflection = true;
  withRefl.Reflection = rec.Data;
  cache.Insert("k1", withRefl);
  // Attempting to overwrite reflection-bearing entry with reflection-less must be ignored.
  cache.Insert("k1", noRefl);
  if (!cache.Lookup("k1", true, got) || !got.HasReflection ||
      got.Reflection.Order.size() != 9) {
    std::cerr << "VariantReplayCache reflection preservation failed\n";
    return false;
  }

  // Test ParseSpecFlagsList stage guard: VS must NOT inherit PS live_spec_flags!
  const std::string psMeta =
      "{\"stage\":\"ps_5_0\",\"spec_flags\":[\"VS_ONLY\",\"PS_ONLY\"],"
      "\"live_spec_flags\":[\"PS_ONLY\"]}";
  std::vector<std::string> vsFlags, psFlags;
  dxcb5_proxy::ParseSpecFlagsList(psMeta, vsFlags, "\"vs_live_spec_flags\"");
  dxcb5_proxy::ParseSpecFlagsList(psMeta, psFlags, "\"ps_live_spec_flags\"");
  if (vsFlags.size() != 2 || psFlags.size() != 1 || psFlags[0] != "PS_ONLY") {
    std::cerr << "ParseSpecFlagsList stage isolation failed\n";
    return false;
  }
  return true;
}

UnityCoreString MakeUnityCoreString(const std::string &s,
                                    std::vector<std::string> &heapPool) {
  UnityCoreString u{};
  if (s.size() <= 24) {
    if (!s.empty())
      std::memcpy(u.Raw, s.data(), s.size());
    u.Raw[0x18] = static_cast<uint8_t>(24 - s.size());
    u.Raw[0x20] = 1;
  } else {
    heapPool.push_back(s);
    const char *ptr = heapPool.back().c_str();
    const uint64_t len = static_cast<uint64_t>(s.size());
    std::memcpy(u.Raw + 0x00, &ptr, sizeof(ptr));
    std::memcpy(u.Raw + 0x10, &len, sizeof(len));
    u.Raw[0x20] = 0;
  }
  return u;
}

UnityCoreVector<UnityCoreString>
MakeUnityKeywordVec(std::vector<UnityCoreString> &backing) {
  UnityCoreVector<UnityCoreString> v{};
  v.Data = backing.empty() ? nullptr : backing.data();
  v.Size = static_cast<uint64_t>(backing.size());
  v.CapacityAndFlags = (static_cast<uint64_t>(backing.size()) << 1) | 1ull;
  return v;
}

bool TestSnippetCacheKeyNoKeywordCollisions(const std::wstring &testDir) {
  // Keep heap strings pinned (no reallocation while UnityCoreString pointers refer to them).
  std::vector<std::string> heapPool;
  heapPool.reserve(256);

  auto buildVec = [&](const std::vector<std::string> &names,
                      std::vector<UnityCoreString> &storage) {
    storage.clear();
    storage.reserve(names.size());
    for (const auto &n : names)
      storage.push_back(MakeUnityCoreString(n, heapPool));
    return MakeUnityKeywordVec(storage);
  };

  std::vector<UnityCoreString> platStore;
  const auto platVec = buildVec(
      {"UNITY_ENABLE_REFLECTION_BUFFERS", "UNITY_USE_DITHER_MASK_FOR_ALPHABLENDED_SHADOWS"},
      platStore);

  // 1. Exact MoeToon2 12-keyword collision repro:
  //    Same shader, same enabled count, same disabled count, same union of all
  //    keywords, including both SSO (<=24B) and heap (>24B) strings!
  const std::vector<std::string> enabledA = {
      "DIRECTIONAL", "LIGHTPROBE_SH", "MT2F_ARM_AO", "MT2F_EMISSION",
      "MT2F_MAIN_COLOR_ADJUSTMENT", "MT2F_MAIN_SECOND_LAYER",
      "MT2F_MAIN_THIRD_LAYER", "MT2F_MAIN_UV_ANIMATION", "MT2F_MATCAP",
      "MT2F_NORMAL_MAPPING", "MT2F_RIM", "MT2F_SCATTER"};
  const std::vector<std::string> disabledA = {
      "MT2F_SKIN", "MT2F_SPECULAR", "MT2F_FUR", "SHADOWS_SCREEN"};

  const std::vector<std::string> enabledB = {
      "DIRECTIONAL", "LIGHTPROBE_SH", "MT2F_ARM_AO", "MT2F_EMISSION",
      "MT2F_MAIN_COLOR_ADJUSTMENT", "MT2F_MAIN_UV_ANIMATION", "MT2F_MATCAP",
      "MT2F_NORMAL_MAPPING", "MT2F_RIM", "MT2F_SCATTER", "MT2F_SKIN",
      "MT2F_SPECULAR"};
  const std::vector<std::string> disabledB = {
      "MT2F_MAIN_SECOND_LAYER", "MT2F_MAIN_THIRD_LAYER", "MT2F_FUR",
      "SHADOWS_SCREEN"};

  std::vector<UnityCoreString> enAStore, disAStore, enBStore, disBStore, enARevStore;
  const auto enAVec = buildVec(enabledA, enAStore);
  const auto disAVec = buildVec(disabledA, disAStore);
  const auto enBVec = buildVec(enabledB, enBStore);
  const auto disBVec = buildVec(disabledB, disBStore);
  std::vector<std::string> enabledARev(enabledA.rbegin(), enabledA.rend());
  const auto enARevVec = buildVec(enabledARev, enARevStore);

  const std::string_view code = "float4 frag() : SV_Target { return 1; }";
  const std::string_view path = "Assets/MoeToon2/Shaders/MoeToon2.shader";
  const std::string_view sname = "MoeToon2/MoeToon2";
  const std::string_view entry = "MT2Frag";

  std::string keyA, keyB, keyARev;
  if (!BuildSnippetCacheKey(code, 0x123456789abcdef0ull, path, sname, 42, entry,
                            platVec, enAVec, disAVec, 4, 19, 0, false, false,
                            0x100, 0, kProgramFragment, 3, kShaderCompPlatformD3D11,
                            0, 0, false, testDir, keyA) ||
      !BuildSnippetCacheKey(code, 0x123456789abcdef0ull, path, sname, 42, entry,
                            platVec, enBVec, disBVec, 4, 19, 0, false, false,
                            0x100, 0, kProgramFragment, 3, kShaderCompPlatformD3D11,
                            0, 0, false, testDir, keyB) ||
      !BuildSnippetCacheKey(code, 0x123456789abcdef0ull, path, sname, 42, entry,
                            platVec, enARevVec, disAVec, 4, 19, 0, false, false,
                            0x100, 0, kProgramFragment, 3, kShaderCompPlatformD3D11,
                            0, 0, false, testDir, keyARev)) {
    std::cerr << "BuildSnippetCacheKey failed on valid keyword sets\n";
    return false;
  }
  if (keyA == keyB) {
    std::cerr << "BUG: same-count distinct enabled/disabled keyword sets collided: "
              << keyA << "\n";
    return false;
  }
  if (keyA != keyARev) {
    std::cerr << "Expected order-independent hit for identical keyword set\n";
    return false;
  }

  // 2. Verify VariantReplayCache misses on keyB and hits ONLY on identical keyword set!
  VariantReplayCache cache;
  CachedSnippetResult valA;
  valA.OutProgramType = 1;
  valA.Code = {0xAA, 0xBB, 0xCC, 0xDD};
  valA.HasReflection = true;
  cache.Insert(keyA, valA);

  CachedSnippetResult out;
  uint64_t ramHits = 0;
  if (cache.Lookup(keyB, true, out)) {
    std::cerr << "VariantReplayCache falsely hit on different 12-keyword variant!\n";
    return false;
  }
  if (cache.Lookup(keyARev, true, out))
    ++ramHits;
  if (ramHits != 1 || out.Code != valA.Code) {
    std::cerr << "VariantReplayCache failed to hit on identical keyword set\n";
    return false;
  }

  // 3. Batch of 10 distinct 10-keyword subsets from the same 11-keyword universe
  //    (each drops a different single keyword -> identical counts, identical union!).
  const std::vector<std::string> universe11 = {
      "DIRECTIONAL", "MT2F_ARM_AO", "MT2F_EMISSION", "MT2F_MAIN_COLOR_ADJUSTMENT",
      "MT2F_MAIN_UV_ANIMATION", "MT2F_MATCAP", "MT2F_NORMAL_MAPPING",
      "MT2F_RIM", "MT2F_SCATTER", "MT2F_SKIN", "MT2F_SPECULAR"};
  std::vector<std::string> keys10;
  for (size_t drop = 0; drop < 10; ++drop) {
    std::vector<std::string> en, dis;
    for (size_t i = 0; i < universe11.size(); ++i) {
      if (i == drop)
        dis.push_back(universe11[i]);
      else
        en.push_back(universe11[i]);
    }
    std::vector<UnityCoreString> enS, disS;
    const auto enV = buildVec(en, enS);
    const auto disV = buildVec(dis, disS);
    std::string k;
    if (!BuildSnippetCacheKey(code, 0xfeedfaceull, path, sname, 10, entry,
                              platVec, enV, disV, 1, 19, 0, false, false, 0, 0,
                              kProgramFragment, 3, kShaderCompPlatformD3D11, 0,
                              0, false, testDir, k)) {
      std::cerr << "BuildSnippetCacheKey failed on 10-keyword batch\n";
      return false;
    }
    for (const auto &prev : keys10) {
      if (prev == k) {
        std::cerr << "10-keyword batch collision at drop=" << drop << "\n";
        return false;
      }
    }
    keys10.push_back(std::move(k));
  }

  // 4. Spec-stub fast path MUST ignore disabledKeywords (putting SHADOWS_SCREEN or
  //    STEREO_INSTANCING_ON in disabledKeywords must NOT activate them!).
  const std::wstring stubProj = testDir + L"\\StubProj";
  const std::wstring metaDir = stubProj + L"\\Assets\\SpecCache\\pair1\\desktop";
  const std::wstring spsiDir = stubProj + L"\\Assets\\SpecCache\\pair1\\spsi";
  EnsureDirTreeW(metaDir);
  EnsureDirTreeW(spsiDir);
  const char kMeta[] = "{\"ps_live_spec_flags\":[\"SHADOWS_SCREEN\"]}";
  dxcb5_proxy::WriteFileAtomic(metaDir + L"\\meta.json", kMeta, std::strlen(kMeta));
  dxcb5_proxy::WriteFileAtomic(spsiDir + L"\\meta.json", kMeta, std::strlen(kMeta));
  const std::string_view stubCode =
      "// dxcb5-spec: {\"pair_key\":\"pair1\",\"cache_dir\":\"Assets/SpecCache\"}\n";

  std::vector<UnityCoreString> stEnOffS, stDisOffS, stEnOnS, stDisOnS;
  const auto stEnOff = buildVec({"DIRECTIONAL"}, stEnOffS);
  const auto stDisOff = buildVec({"SHADOWS_SCREEN", "STEREO_INSTANCING_ON"}, stDisOffS);
  const auto stEnOn = buildVec({"DIRECTIONAL", "SHADOWS_SCREEN"}, stEnOnS);
  const auto stDisOn = buildVec({"STEREO_INSTANCING_ON"}, stDisOnS);
  std::string stubKeyOff, stubKeyOn;
  if (!BuildSnippetCacheKey(stubCode, 1, path, sname, 1, entry, platVec, stEnOff,
                            stDisOff, 2, 19, 0, false, false, 0, 0,
                            kProgramFragment, 3, kShaderCompPlatformD3D11, 0, 0,
                            false, stubProj, stubKeyOff) ||
      !BuildSnippetCacheKey(stubCode, 1, path, sname, 1, entry, platVec, stEnOn,
                            stDisOn, 1, 19, 0, false, false, 0, 0,
                            kProgramFragment, 3, kShaderCompPlatformD3D11, 0, 0,
                            false, stubProj, stubKeyOn)) {
    std::cerr << "Spec stub BuildSnippetCacheKey failed\n";
    return false;
  }
  if (stubKeyOff.rfind("stub:", 0) != 0 || stubKeyOn.rfind("stub:", 0) != 0 ||
      stubKeyOff == stubKeyOn) {
    std::cerr << "Spec stub polluted by disabledKeywords: off=" << stubKeyOff
              << " on=" << stubKeyOn << "\n";
    return false;
  }

  // 4b. Multi-stub ForwardAdd conditional dispatch (`#if defined(POINT) ...
  //     #elif defined(SPOT) ... #else ... #endif`) MUST resolve each branch's
  //     exact pair_key & meta.json without colliding POINT / SPOT / DIR, while
  //     still deduplicating irrelevant keywords within each branch and failing
  //     safe to `var:` when branch macros are unknown or ambiguous!
  const std::wstring ptDir = stubProj + L"\\Assets\\SpecCache\\pair_point\\desktop";
  const std::wstring spDir = stubProj + L"\\Assets\\SpecCache\\pair_spot\\desktop";
  const std::wstring drDir = stubProj + L"\\Assets\\SpecCache\\pair_dir\\desktop";
  EnsureDirTreeW(ptDir);
  EnsureDirTreeW(spDir);
  EnsureDirTreeW(drDir);
  const char kPtMeta[] = "{\"ps_live_spec_flags\":[\"SHADOWS_CUBE\",\"FOG_LINEAR\"]}";
  const char kSpMeta[] = "{\"ps_live_spec_flags\":[\"SHADOWS_DEPTH\",\"FOG_LINEAR\"]}";
  const char kDrMeta[] = "{\"ps_live_spec_flags\":[\"SHADOWS_SCREEN\",\"FOG_LINEAR\"]}";
  dxcb5_proxy::WriteFileAtomic(ptDir + L"\\meta.json", kPtMeta, std::strlen(kPtMeta));
  dxcb5_proxy::WriteFileAtomic(spDir + L"\\meta.json", kSpMeta, std::strlen(kSpMeta));
  dxcb5_proxy::WriteFileAtomic(drDir + L"\\meta.json", kDrMeta, std::strlen(kDrMeta));

  const std::string_view fwdAddStub =
      "#pragma multi_compile_fwdadd_fullshadows\n"
      "#if defined(POINT)\n"
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_point\",\"cache_dir\":\"Assets/SpecCache\"}\n"
      "static const string _dxcb5_spec = \"dxcb5-spec: {\\\"v\\\":1,\\\"pair_key\\\":\\\"pair_point\\\",\\\"cache_dir\\\":\\\"Assets/SpecCache\\\"}\";\n"
      "#elif defined(SPOT)\n"
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_spot\",\"cache_dir\":\"Assets/SpecCache\"}\n"
      "static const string _dxcb5_spec = \"dxcb5-spec: {\\\"v\\\":1,\\\"pair_key\\\":\\\"pair_spot\\\",\\\"cache_dir\\\":\\\"Assets/SpecCache\\\"}\";\n"
      "#else\n"
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_dir\",\"cache_dir\":\"Assets/SpecCache\"}\n"
      "static const string _dxcb5_spec = \"dxcb5-spec: {\\\"v\\\":1,\\\"pair_key\\\":\\\"pair_dir\\\",\\\"cache_dir\\\":\\\"Assets/SpecCache\\\"}\";\n"
      "#endif\n";

  auto makeFwdAddKey = [&](const std::vector<std::string> &en,
                           const std::vector<std::string> &dis,
                           std::string_view src, std::string &outKey) {
    std::vector<UnityCoreString> enS, disS;
    const auto enV = buildVec(en, enS);
    const auto disV = buildVec(dis, disS);
    return BuildSnippetCacheKey(
        src, 0x55aa55aaull, path, sname, 100, entry, platVec, enV, disV,
        dis.size(), 19, 0, false, false, 0, 0, kProgramFragment, 3,
        kShaderCompPlatformD3D11, 0, 0, false, stubProj, outKey);
  };

  std::string kPt, kSp, kDr, kSpShad, kSpIrr, kUnknownDir, kIncDir, kAmbig;
  if (!makeFwdAddKey({"POINT"}, {"SPOT", "DIRECTIONAL", "SHADOWS_CUBE", "SHADOWS_DEPTH", "SHADOWS_SCREEN"}, fwdAddStub, kPt) ||
      !makeFwdAddKey({"SPOT"}, {"POINT", "DIRECTIONAL", "SHADOWS_CUBE", "SHADOWS_DEPTH", "SHADOWS_SCREEN"}, fwdAddStub, kSp) ||
      !makeFwdAddKey({"DIRECTIONAL"}, {"POINT", "SPOT", "SHADOWS_CUBE", "SHADOWS_DEPTH", "SHADOWS_SCREEN"}, fwdAddStub, kDr) ||
      !makeFwdAddKey({"SPOT", "SHADOWS_DEPTH"}, {"POINT", "DIRECTIONAL", "SHADOWS_CUBE", "SHADOWS_SCREEN"}, fwdAddStub, kSpShad) ||
      !makeFwdAddKey({"SPOT", "UNITY_HDR_ON"}, {"POINT", "DIRECTIONAL", "SHADOWS_CUBE", "SHADOWS_DEPTH", "SHADOWS_SCREEN"}, fwdAddStub, kSpIrr)) {
    std::cerr << "ForwardAdd multi-stub BuildSnippetCacheKey failed\n";
    return false;
  }
  if (kPt.rfind("stub:", 0) != 0 || kSp.rfind("stub:", 0) != 0 ||
      kDr.rfind("stub:", 0) != 0 || kSpShad.rfind("stub:", 0) != 0) {
    std::cerr << "Expected stub: keys for resolved ForwardAdd branches\n";
    return false;
  }
  if (kPt == kSp || kPt == kDr || kSp == kDr) {
    std::cerr << "BUG: ForwardAdd POINT/SPOT/DIR collided: pt=" << kPt
              << " sp=" << kSp << " dr=" << kDr << "\n";
    return false;
  }
  if (kSp == kSpShad) {
    std::cerr << "BUG: ForwardAdd SPOT + SHADOWS_DEPTH collided with unshadowed SPOT!\n";
    return false;
  }
  if (kSp != kSpIrr) {
    std::cerr << "Expected irrelevant keyword UNITY_HDR_ON to share SPOT stub: key\n";
    return false;
  }

  // Unknown branch macro (POINT/SPOT omitted from disabledKws) MUST fail-safe to `var:`.
  if (!makeFwdAddKey({"DIRECTIONAL"}, {}, fwdAddStub, kUnknownDir) ||
      kUnknownDir.rfind("var:", 0) != 0) {
    std::cerr << "Expected fail-safe var: fallback when POINT/SPOT are unknown, got: "
              << kUnknownDir << "\n";
    return false;
  }

  // `#include` before conditional stub MUST fail-safe to `var:`.
  const std::string incFwdAdd = "#include \"UnityCG.cginc\"\n" + std::string(fwdAddStub);
  if (!makeFwdAddKey({"POINT"}, {"SPOT", "DIRECTIONAL"}, incFwdAdd, kIncDir) ||
      kIncDir.rfind("var:", 0) != 0) {
    std::cerr << "Expected fail-safe var: fallback after #include, got: " << kIncDir << "\n";
    return false;
  }

  // Two distinct active pair_keys in the same scope MUST fail-safe to `var:`.
  const std::string_view ambigStub =
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_point\",\"cache_dir\":\"Assets/SpecCache\"}\n"
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_spot\",\"cache_dir\":\"Assets/SpecCache\"}\n";
  if (!makeFwdAddKey({"POINT"}, {}, ambigStub, kAmbig) ||
      kAmbig.rfind("var:", 0) != 0) {
    std::cerr << "Expected fail-safe var: fallback on conflicting active pair_keys, got: "
              << kAmbig << "\n";
    return false;
  }

  // Conditional stub whose active #else branch has NO dxcb5-spec MUST fall back to `var:`.
  const std::string_view condFallbackStub =
      "#if defined(USE_SPEC)\n"
      "// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_point\",\"cache_dir\":\"Assets/SpecCache\"}\n"
      "#else\n"
      "float4 frag() : SV_Target { return 1; }\n"
      "#endif\n";
  std::string kNoSpecElse;
  if (!makeFwdAddKey({}, {"USE_SPEC"}, condFallbackStub, kNoSpecElse) ||
      kNoSpecElse.rfind("var:", 0) != 0) {
    std::cerr << "Expected var: fallback when active #else has no dxcb5-spec, got: "
              << kNoSpecElse << "\n";
    return false;
  }

  // H4 + L3 regression tests on ResolveSpecStubDirective:
  //   - `#define M (0)` + `#if M` must be Unknown (NOT True!)
  //   - `#define M 0x0` + `#if M` must be Unknown (NOT True!)
  //   - `#define M(x) (x)` + `#if defined(M)` must be True (NOT False!)
  //   - `/* dxcb5-spec: ... */` inside a block comment must be ignored;
  //     `/* comment */ // dxcb5-spec: ...` after closing `*/` must be accepted!
  {
    dxcb5_proxy::SpecStubDirective d;
    auto makeOracle = [](bool unkFalse) {
      dxcb5_proxy::PreprocMacroOracle o;
      o.UnknownAsFalse = unkFalse;
      return o;
    };
    const char kParenZero[] =
        "#define M (0)\n#if M\n// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_point\",\"cache_dir\":\"Assets/SpecCache\"}\n#endif\n";
    if (dxcb5_proxy::ResolveSpecStubDirective(kParenZero, std::strlen(kParenZero), makeOracle(true), d)) {
      std::cerr << "H4 BUG: #define M (0) + #if M falsely evaluated True!\n";
      return false;
    }
    const char kHexZero[] =
        "#define M 0x0\n#if M\n// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_point\",\"cache_dir\":\"Assets/SpecCache\"}\n#endif\n";
    if (dxcb5_proxy::ResolveSpecStubDirective(kHexZero, std::strlen(kHexZero), makeOracle(true), d)) {
      std::cerr << "H4 BUG: #define M 0x0 + #if M falsely evaluated True!\n";
      return false;
    }
    const char kFuncMacro[] =
        "#define M(x) ((x)+1)\n#if defined(M)\n// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_spot\",\"cache_dir\":\"Assets/SpecCache\"}\n#else\n// dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_dir\",\"cache_dir\":\"Assets/SpecCache\"}\n#endif\n";
    if (!dxcb5_proxy::ResolveSpecStubDirective(kFuncMacro, std::strlen(kFuncMacro), makeOracle(true), d) ||
        d.PairKey != "pair_spot") {
      std::cerr << "H4 BUG: #define M(x) + #if defined(M) failed to select pair_spot!\n";
      return false;
    }
    const char kBlockCommentTag[] =
        "/* dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_point\",\"cache_dir\":\"Assets/SpecCache\"} */\n"
        "/* multi\nline */ // dxcb5-spec: {\"v\":1,\"pair_key\":\"pair_spot\",\"cache_dir\":\"Assets/SpecCache\"}\n";
    if (!dxcb5_proxy::ResolveSpecStubDirective(kBlockCommentTag, std::strlen(kBlockCommentTag), makeOracle(true), d) ||
        d.PairKey != "pair_spot") {
      std::cerr << "L3 BUG: block-comment tag handling failed (got " << d.PairKey << ")\n";
      return false;
    }
    // H5: non-default Flags1 (-Zpr / -Gis / -Od) must produce distinct suffixes from default (-O3).
    if (dxcb5_proxy::BuildSpecFlagsSuffix(0, false) != L"" ||
        dxcb5_proxy::BuildSpecFlagsSuffix(D3DCOMPILE_OPTIMIZATION_LEVEL3, false) != L"" ||
        dxcb5_proxy::BuildSpecFlagsSuffix(D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, false) == L"" ||
        dxcb5_proxy::BuildSpecFlagsSuffix(D3DCOMPILE_IEEE_STRICTNESS, false) == L"") {
      std::cerr << "H5 BUG: BuildSpecFlagsSuffix failed to isolate non-default Flags1\n";
      return false;
    }
  }

  // 5. Malformed UnityCoreString (invalid storageKind) must fail-safe (return false).
  std::vector<UnityCoreString> badStore(1);
  badStore[0].Raw[0x20] = 99;
  const auto badVec = MakeUnityKeywordVec(badStore);
  std::string badKey;
  if (BuildSnippetCacheKey(code, 1, path, sname, 1, entry, platVec, badVec,
                           disAVec, 4, 19, 0, false, false, 0, 0,
                           kProgramFragment, 3, kShaderCompPlatformD3D11, 0, 0,
                           false, testDir, badKey)) {
    std::cerr << "BuildSnippetCacheKey accepted malformed UnityCoreString!\n";
    return false;
  }
  return true;
}

bool TestCmdLineEnvAndSafeStaging(const std::wstring &testDir) {
  // Invocation matching must reject unrelated tools referencing UnityShaderCompiler.exe as an arg!
  if (IsUnityShaderCompilerInvocation(nullptr, L"taskkill.exe /F /IM UnityShaderCompiler.exe")) {
    std::cerr << "IsUnityShaderCompilerInvocation falsely matched taskkill\n";
    return false;
  }
  if (!IsUnityShaderCompilerInvocation(
          nullptr, L"\"F:/Program Files/Unity/Editor/Data/Tools/UnityShaderCompiler.exe\" 64")) {
    std::cerr << "IsUnityShaderCompilerInvocation missed real UnityShaderCompiler.exe\n";
    return false;
  }

  // Normal UnityShaderCompiler command line rewrite.
  const std::wstring origCmd =
      L"\"F:/Program Files/Unity 2022.3.22f1/Editor/Data/Tools/UnityShaderCompiler.exe\" "
      L"\"\" \"C:/Users/Test/AppData/Local/Unity/Editor\" \"UnityShaderCompiler-64bit-IPC\" 12345 "
      L"\"F:/Program Files/Unity 2022.3.22f1/Editor/Data/PlaybackEngines\"";
  std::wstring rewritten, origPlugins;
  if (!RewriteShaderCompilerCmdLine(origCmd, L"E:\\Proj\\Temp\\DXCB5CgBatchPlugins",
                                    rewritten, origPlugins) ||
      origPlugins != L"F:/Program Files/Unity 2022.3.22f1/Editor/Data/PlaybackEngines" ||
      rewritten.find(L"\"E:/Proj/Temp/DXCB5CgBatchPlugins\"") == std::wstring::npos) {
    std::cerr << "RewriteShaderCompilerCmdLine failed\n";
    return false;
  }

  // Empty `""` argv[5] must be replaced in-place (not appended as argv[6])!
  const std::wstring emptyArg5 =
      L"\"UnityShaderCompiler.exe\" 64 \"log\" \"ipc\" 1234 \"\"";
  if (!RewriteShaderCompilerCmdLine(emptyArg5, L"E:\\Proj\\Temp\\Stg",
                                    rewritten, origPlugins) ||
      !origPlugins.empty() ||
      ParseWindowsCmdLineSpans(rewritten).size() != 6 ||
      ParseWindowsCmdLineSpans(rewritten)[5].Unquoted != L"E:/Proj/Temp/Stg") {
    std::cerr << "RewriteShaderCompilerCmdLine empty argv[5] failed\n";
    return false;
  }

  // ANSI -> Unicode environment augmentation (no OOB write, preserves =C:= entries).
  const char ansiEnv[] = "=C:=C:\\Work\0FOO=bar\0DXCB5_HOOK_ENABLED=0\0";
  std::vector<wchar_t> envOut;
  BuildAugmentedUnicodeEnv(const_cast<char *>(ansiEnv), 0,
                           {{L"DXCB5_HOOK_ENABLED", L"1"},
                            {L"DXCB5_DXCOMPILER_PATH", L"C:\\dxcompiler.dll"}},
                           envOut);
  bool sawDrive = false, sawFoo = false, sawEnabled1 = false, sawEnabled0 = false, sawDxc = false;
  for (size_t i = 0; i < envOut.size() && envOut[i];) {
    std::wstring entry(&envOut[i]);
    i += entry.size() + 1;
    if (entry == L"=C:=C:\\Work") sawDrive = true;
    if (entry == L"FOO=bar") sawFoo = true;
    if (entry == L"DXCB5_HOOK_ENABLED=1") sawEnabled1 = true;
    if (entry == L"DXCB5_HOOK_ENABLED=0") sawEnabled0 = true;
    if (entry == L"DXCB5_DXCOMPILER_PATH=C:\\dxcompiler.dll") sawDxc = true;
  }
  if (!sawDrive || !sawFoo || !sawEnabled1 || sawEnabled0 || !sawDxc) {
    std::cerr << "BuildAugmentedUnicodeEnv failed\n";
    return false;
  }

  // P0 Safety: staging MUST NEVER create NTFS junctions/symlinks to PlaybackEngines,
  // MUST copy only actual `*/CgBatchPlugins64/*.dll` as plain files, and MUST
  // safely detach any legacy reparse-point junctions without touching targets!
  const std::wstring fakePb = testDir + L"\\FakePlaybackEngines";
  const std::wstring winSupp = fakePb + L"\\WindowsStandaloneSupport";
  const std::wstring consoleCg = fakePb + L"\\FakeConsole\\CgBatchPlugins64";
  const std::wstring staging = testDir + L"\\FakeStaging";
  EnsureDirTreeW(winSupp);
  EnsureDirTreeW(consoleCg);
  EnsureDirTreeW(staging);
  dxcb5_proxy::WriteFileAtomic(winSupp + L"\\UnityEditor.WindowsStandalone.Extensions.dll",
                               "KEEP_ME", 7);
  dxcb5_proxy::WriteFileAtomic(consoleCg + L"\\console_cgbatch.dll", "PLUG", 4);

  // Create a legacy NTFS junction in staging (simulating older hook build) and
  // verify CopyOtherCgBatchPluginsSafe detaches it without touching winSupp!
  const std::wstring legacyJunc = staging + L"\\WindowsStandaloneSupport";
  RemoveDirectoryW(legacyJunc.c_str());
  const std::wstring mklinkCmd =
      L"cmd.exe /c mklink /J \"" + legacyJunc + L"\" \"" + winSupp + L"\" >nul 2>&1";
  std::vector<wchar_t> cmdBuf(mklinkCmd.begin(), mklinkCmd.end());
  cmdBuf.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  const DWORD preAttrs = GetFileAttributesW(legacyJunc.c_str());
  if (preAttrs == INVALID_FILE_ATTRIBUTES ||
      (preAttrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
    std::cerr << "Failed to create test legacy junction fixture\n";
    return false;
  }

  CopyOtherCgBatchPluginsSafe(fakePb, staging);

  // 1. Legacy junction MUST be detached; target WindowsStandaloneSupport MUST be intact!
  if (GetFileAttributesW(legacyJunc.c_str()) != INVALID_FILE_ATTRIBUTES) {
    std::cerr << "Legacy junction was not detached from staging!\n";
    return false;
  }
  std::string winContent;
  if (!dxcb5_proxy::ReadWholeFile(
          winSupp + L"\\UnityEditor.WindowsStandalone.Extensions.dll",
          winContent) ||
      winContent != "KEEP_ME") {
    std::cerr << "Target WindowsStandaloneSupport file damaged during junction detach!\n";
    return false;
  }
  // 2. Sibling CgBatchPlugins64 DLL copied as plain regular file (zero reparse points!).
  const std::wstring stagedConsoleDir = staging + L"\\FakeConsole";
  const DWORD cAttrs = GetFileAttributesW(stagedConsoleDir.c_str());
  if (cAttrs == INVALID_FILE_ATTRIBUTES ||
      (cAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    std::cerr << "Staged sibling dir missing or is a reparse point!\n";
    return false;
  }
  std::string plugContent;
  if (!dxcb5_proxy::ReadWholeFile(
          stagedConsoleDir + L"\\CgBatchPlugins64\\console_cgbatch.dll",
          plugContent) ||
      plugContent != "PLUG") {
    std::cerr << "Sibling CgBatchPlugins64 DLL was not plain-copied!\n";
    return false;
  }
  return true;
}

bool TestUnityHookDllLifecycle(const std::wstring &unityHookDllPath,
                               const std::wstring &testDir) {
  HMODULE mod = LoadLibraryW(unityHookDllPath.c_str());
  if (!mod) {
    std::cerr << "Failed to load dxcb5_unity_hook.dll\n";
    return false;
  }
  auto pAbi = reinterpret_cast<PFN_dxcb5_hook_get_abi_version>(
      GetProcAddress(mod, "dxcb5_hook_get_abi_version"));
  auto pInstall = reinterpret_cast<PFN_dxcb5_hook_install>(
      GetProcAddress(mod, "dxcb5_hook_install"));
  auto pUninstall = reinterpret_cast<PFN_dxcb5_hook_uninstall>(
      GetProcAddress(mod, "dxcb5_hook_uninstall"));
  auto pIsInstalled = reinterpret_cast<PFN_dxcb5_hook_is_installed>(
      GetProcAddress(mod, "dxcb5_hook_is_installed"));
  auto pSetEnabled = reinterpret_cast<PFN_dxcb5_hook_set_enabled>(
      GetProcAddress(mod, "dxcb5_hook_set_enabled"));
  auto pSetNanSafe = reinterpret_cast<PFN_dxcb5_hook_set_nan_safe>(
      GetProcAddress(mod, "dxcb5_hook_set_nan_safe"));
  auto pReg = reinterpret_cast<PFN_dxcb5_hook_register_manifest>(
      GetProcAddress(mod, "dxcb5_hook_register_manifest"));
  auto pClearM = reinterpret_cast<PFN_dxcb5_hook_clear_manifests>(
      GetProcAddress(mod, "dxcb5_hook_clear_manifests"));
  auto pClearCache = reinterpret_cast<PFN_dxcb5_hook_clear_cache>(
      GetProcAddress(mod, "dxcb5_hook_clear_cache"));
  auto pReload = reinterpret_cast<PFN_dxcb5_hook_reload_workers>(
      GetProcAddress(mod, "dxcb5_hook_reload_workers"));
  auto pStats = reinterpret_cast<PFN_dxcb5_hook_get_stats>(
      GetProcAddress(mod, "dxcb5_hook_get_stats"));
  if (!pAbi || !pInstall || !pUninstall || !pIsInstalled || !pSetEnabled ||
      !pSetNanSafe || !pReg || !pClearM || !pClearCache || !pReload ||
      !pStats) {
    std::cerr << "dxcb5_unity_hook.dll missing expected exports\n";
    return false;
  }
  if (pAbi() != DXCB5_UNITY_HOOK_ABI_VERSION) {
    std::cerr << "dxcb5_hook_get_abi_version mismatch: " << pAbi() << "\n";
    return false;
  }
  if (pInstall(nullptr, nullptr, 0) != DXCB5_HOOK_ERR_INVALID_ARG) {
    std::cerr << "dxcb5_hook_install(null) did not return DXCB5_HOOK_ERR_INVALID_ARG\n";
    return false;
  }

  const std::wstring fakeTemp = testDir + L"\\FakeProj\\Temp";
  const std::wstring fakePlugDir = testDir + L"\\FakePlug";
  EnsureDirTreeW(fakeTemp);
  EnsureDirTreeW(fakePlugDir);

  if (pInstall(fakePlugDir.c_str(), fakeTemp.c_str(), 0) != 0 ||
      pIsInstalled() != 1) {
    std::cerr << "dxcb5_hook_install failed\n";
    return false;
  }

  const std::wstring sessionDir =
      fakeTemp + L"\\DXCB5HookSession_" + std::to_wstring(GetCurrentProcessId());
  std::string stateTxt;
  if (!dxcb5_proxy::ReadWholeFile(sessionDir + L"\\hook_state.txt", stateTxt) ||
      stateTxt.find("enabled=1") == std::string::npos) {
    std::cerr << "hook_state.txt missing enabled=1\n";
    return false;
  }

  // Test Unicode non-ASCII shader path manifest registration -> written to disk immediately!
  const std::wstring unicodePath = L"Assets/Shaders/\u89d2\u8272_Toon.shader";
  pReg(unicodePath.c_str(), L"{\"schema_version\":1}", L"VSMain:PSMain");
  const std::string norm = NormalizeAssetPath(WideToUtf8(unicodePath));
  const std::wstring manifestFile =
      sessionDir + L"\\manifest_" + Utf8ToWide(Hex64(Fnv1a64Str(norm))) + L".json";
  std::string manifestDisk;
  if (!dxcb5_proxy::ReadWholeFile(manifestFile, manifestDisk) ||
      manifestDisk.find("\"VSMain:PSMain\"") == std::string::npos) {
    std::cerr << "Unicode manifest registration did not flush to disk\n";
    return false;
  }

  // Clear manifests must delete disk files too!
  pClearM();
  if (GetFileAttributesW(manifestFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
    std::cerr << "dxcb5_hook_clear_manifests did not delete disk manifest\n";
    return false;
  }

  // Toggle enabled/nan_safe and verify state file updates live!
  pSetNanSafe(1);
  pSetEnabled(0);
  if (!dxcb5_proxy::ReadWholeFile(sessionDir + L"\\hook_state.txt", stateTxt) ||
      stateTxt.find("enabled=0") == std::string::npos ||
      stateTxt.find("nan_safe=1") == std::string::npos) {
    std::cerr << "Dynamic state update in hook_state.txt failed: " << stateTxt << "\n";
    return false;
  }

  // Clear cache must delete worker disk caches without uninstalling hooks.
  {
    EnsureDirTreeW(sessionDir + L"\\manifest_dxbc");
    EnsureDirTreeW(sessionDir + L"\\direct_dxbc");
    const std::wstring cMan = sessionDir + L"\\manifest_dxbc\\clear_ps.dxbc";
    const std::wstring cDir = sessionDir + L"\\direct_dxbc\\clear_ps.dxbc";
    dxcb5_proxy::WriteFileAtomic(cMan, "OLD", 3);
    dxcb5_proxy::WriteFileAtomic(cDir, "OLD", 3);
    pClearCache();
    if (FileExistsW(cMan) || FileExistsW(cDir)) {
      std::cerr << "dxcb5_hook_clear_cache did not delete worker disk caches!\n";
      return false;
    }
  }
  // Uninstall & re-install cycle must succeed cleanly without leaking hooks!
  // H6: dxcb5_hook_reload_workers() MUST clear worker-side disk caches too!
  {
    EnsureDirTreeW(sessionDir + L"\\manifest_dxbc");
    EnsureDirTreeW(sessionDir + L"\\direct_dxbc");
    const std::wstring staleMan = sessionDir + L"\\manifest_dxbc\\stale_ps.dxbc";
    const std::wstring staleDir = sessionDir + L"\\direct_dxbc\\stale_ps.dxbc";
    dxcb5_proxy::WriteFileAtomic(staleMan, "OLD", 3);
    dxcb5_proxy::WriteFileAtomic(staleDir, "OLD", 3);
    pReload();
    if (FileExistsW(staleMan) || FileExistsW(staleDir)) {
      std::cerr << "H6 BUG: dxcb5_hook_reload_workers did not invalidate worker disk caches!\n";
      return false;
    }
  }
  pUninstall();
  if (pIsInstalled() != 0) {
    std::cerr << "dxcb5_hook_uninstall failed\n";
    return false;
  }
  if (pInstall(fakePlugDir.c_str(), fakeTemp.c_str(), 0) != 0 ||
      pIsInstalled() != 1) {
    std::cerr << "dxcb5_hook_install re-install failed\n";
    return false;
  }
  uint64_t rh = 99, ipc = 99, wh = 99, sh = 99;
  pStats(&rh, &ipc, &wh, &sh);
  if (sh != 0) {
    std::cerr << "Expected snippetHookActive == 0 outside Unity.exe\n";
    return false;
  }
  // H6: dxcb5_hook_reload_workers() MUST clear worker-side disk caches too!
  {
    EnsureDirTreeW(sessionDir + L"\\manifest_dxbc");
    EnsureDirTreeW(sessionDir + L"\\direct_dxbc");
    const std::wstring staleMan = sessionDir + L"\\manifest_dxbc\\stale_ps.dxbc";
    const std::wstring staleDir = sessionDir + L"\\direct_dxbc\\stale_ps.dxbc";
    dxcb5_proxy::WriteFileAtomic(staleMan, "OLD", 3);
    dxcb5_proxy::WriteFileAtomic(staleDir, "OLD", 3);
    pReload();
    if (FileExistsW(staleMan) || FileExistsW(staleDir)) {
      std::cerr << "H6 BUG: dxcb5_hook_reload_workers did not invalidate worker disk caches!\n";
      return false;
    }
  }
  pUninstall();
  return true;
}

// Mock UnityShaderCompiler.exe D3DCompilerWrapper & Dispatcher for live plugin test!
int gFallbackCalls = 0;

struct MockD3DCompiler {
  virtual bool IsValid() { return true; }
  virtual void SetUseRealDLL(bool) {}
  virtual bool GetUseRealDLL() { return false; }
  virtual HRESULT D3DCompile(const void *, unsigned long, const char *,
                             const D3D_SHADER_MACRO *, ID3DInclude *,
                             const char *, const char *, UINT, UINT,
                             ID3DBlob **ppCode, ID3DBlob **) {
    ++gFallbackCalls;
    if (ppCode)
      *ppCode = nullptr;
    return S_FALSE;
  }
  virtual HRESULT D3DAssemble() { return S_OK; }
  virtual HRESULT D3DDisassemble() { return S_OK; }
  virtual HRESULT D3DPreprocess() { return S_OK; }
  virtual HRESULT D3DReflect() { return S_OK; }
  virtual HRESULT D3DStripShader() { return S_OK; }
  virtual HRESULT D3DGetBlobPart() { return S_OK; }
};

struct MockDispatcher {
  MockD3DCompiler *D3D = nullptr;
  virtual void *GetD3DCompiler() { return D3D; }
};

bool TestCgBatchPluginLive(const std::wstring &pluginPath,
                           const std::wstring &dxcPath,
                           const std::wstring &testDir) {
  const std::wstring sessionDir = testDir + L"\\session";
  EnsureDirTreeW(sessionDir);
  SetEnvironmentVariableW(L"DXCB5_DXCOMPILER_PATH", dxcPath.c_str());
  SetEnvironmentVariableW(L"DXCB5_HOOK_SESSION_DIR", sessionDir.c_str());
  SetEnvironmentVariableW(L"DXCB5_HOOK_ENABLED", L"1");

  HMODULE mod = LoadLibraryW(pluginPath.c_str());
  if (!mod) {
    std::cerr << "Failed to load dxcb5_cgbatch_plugin.dll\n";
    return false;
  }
  using PFN_GetPluginInterface = void *(*)(int, void *);
  auto getIf = reinterpret_cast<PFN_GetPluginInterface>(
      GetProcAddress(mod, "GetPluginInterface"));
  if (!getIf) {
    std::cerr << "GetPluginInterface export missing\n";
    return false;
  }

  MockD3DCompiler mockD3D;
  MockDispatcher mockDisp;
  mockDisp.D3D = &mockD3D;
  void *ret = getIf(2, &mockDisp);
  if (ret != nullptr) {
    std::cerr << "Expected GetPluginInterface to return nullptr\n";
    return false;
  }
  auto *d3d = static_cast<MockD3DCompiler *>(mockDisp.GetD3DCompiler());

  // 1. Direct SM5 PS compile through hooked D3DCompilerWrapper::D3DCompile!
  const char kSimplePs[] =
      "float4 _Color;\n"
      "float4 PSMain() : SV_TARGET { return _Color; }\n";
  ID3DBlob *code = nullptr;
  ID3DBlob *err = nullptr;
  HRESULT hr = d3d->D3DCompile(kSimplePs, static_cast<unsigned long>(std::strlen(kSimplePs)),
                               "simple.hlsl", nullptr, nullptr, "PSMain",
                               "ps_5_0", 0, 0, &code, &err);
  if (FAILED(hr) || !code || code->GetBufferSize() < 32 ||
      std::memcmp(code->GetBufferPointer(), "DXBC", 4) != 0) {
    std::cerr << "Direct DXCB5 compile via CgBatchPlugin failed\n";
    return false;
  }
  code->Release();

  // 2. Zero-Stub Multi-Pass paired VS+PS manifest specialization via sessionDir!
  const char kMultiPass[] =
      "float _Dead0;\n"
      "float _OutlineWidth;\n"
      "float4 _Dead1;\n"
      "float4 _OutlineColor;\n"
      "struct VSOut { float4 pos : SV_POSITION; float4 unused : TEXCOORD1; };\n"
      "VSOut VSMain(float4 p : POSITION, float3 n : NORMAL) {\n"
      "  VSOut o; o.pos = p + float4(n * _OutlineWidth, 0); o.unused = _Dead1; return o;\n"
      "}\n"
      "float4 PSMain(VSOut i) : SV_TARGET { return _OutlineColor; }\n";
  const std::string assetPath = "Assets/Shaders/TestOutline.shader";
  const std::string norm = NormalizeAssetPath(assetPath);
  const std::wstring manifestFile =
      sessionDir + L"\\manifest_" + Utf8ToWide(Hex64(Fnv1a64Str(norm))) + L".json";
  const char kRegPayload[] =
      "{\"pair\":\"VSMain:PSMain\",\"manifest\":{\"schema_version\":1,"
      "\"options\":{\"compact_globals\":true,\"cross_stage_dce\":true}}}\n";
  dxcb5_proxy::WriteFileAtomic(manifestFile, kRegPayload, std::strlen(kRegPayload));

  ID3DBlob *vsCode = nullptr, *psCode = nullptr;
  hr = d3d->D3DCompile(kMultiPass, static_cast<unsigned long>(std::strlen(kMultiPass)),
                       assetPath.c_str(), nullptr, nullptr, "VSMain",
                       "vs_5_0", 0, 0, &vsCode, &err);
  if (FAILED(hr) || !vsCode) {
    std::cerr << "Zero-stub paired VS compile failed\n";
    return false;
  }
  hr = d3d->D3DCompile(kMultiPass, static_cast<unsigned long>(std::strlen(kMultiPass)),
                       assetPath.c_str(), nullptr, nullptr, "PSMain",
                       "ps_5_0", 0, 0, &psCode, &err);
  if (FAILED(hr) || !psCode) {
    std::cerr << "Zero-stub paired PS compile failed\n";
    return false;
  }
  ID3DBlob *dis = nullptr;
  if (FAILED(D3DDisassemble(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                            0, nullptr, &dis)) || !dis) {
    std::cerr << "D3DDisassemble failed on paired PS\n";
    return false;
  }
  const std::string psAsm(static_cast<const char *>(dis->GetBufferPointer()),
                          dis->GetBufferSize());
  dis->Release();
  vsCode->Release();
  psCode->Release();
  if (psAsm.find("_OutlineColor;              // Offset:   16 Size:    16") == std::string::npos ||
      psAsm.find("_Dead0") != std::string::npos ||
      psAsm.find("TEXCOORD                 1") != std::string::npos) {
    std::cerr << "Zero-stub paired PS missing shared compaction or Cross-Stage DCE:\n"
              << psAsm << '\n';
    return false;
  }

  // 2b. Simulate real UnityShaderCompiler.exe!InvokeCompilerHLSL: pSourceName == ""
  // with `#line 1 "E:/Proj/Assets/Shaders/TestGain.shader"` + SHADER_STAGE_FRAGMENT=1
  // and unannotated `float _Gain` folded to 0.75 via manifest.constants!
  const char kGainShader[] =
      "#line 1 \"E:/UnityProjects/MoeToon2/Assets/Shaders/TestGain.shader\"\n"
      "float _Gain;\n"
      "float4 VSMain(float4 p : POSITION) : SV_POSITION { return p; }\n"
      "float4 PSMain() : SV_TARGET { return float4(_Gain, 0, 0, 1); }\n";
  const std::string gainAsset = "Assets/Shaders/TestGain.shader";
  const std::string gainNorm = NormalizeAssetPath(gainAsset);
  const std::wstring gainManifestFile =
      sessionDir + L"\\manifest_" + Utf8ToWide(Hex64(Fnv1a64Str(gainNorm))) + L".json";
  const char kGainPayload[] =
      "{\"pair\":\"VSMain:PSMain\",\"manifest\":{\"schema_version\":1,"
      "\"constants\":{\"_Gain\":{\"type\":\"f32\",\"v\":[0.75]}},"
      "\"options\":{\"compact_globals\":true,\"cross_stage_dce\":true}}}";
  dxcb5_proxy::WriteFileAtomic(gainManifestFile, kGainPayload, std::strlen(kGainPayload));
  const D3D_SHADER_MACRO fragDefs[] = {{"SHADER_STAGE_FRAGMENT", "1"}, {nullptr, nullptr}};
  ID3DBlob *gainPs = nullptr;
  hr = d3d->D3DCompile(kGainShader, static_cast<unsigned long>(std::strlen(kGainShader)),
                       "", fragDefs, nullptr, "PSMain", "ps_5_0", 0, 0,
                       &gainPs, &err);
  if (FAILED(hr) || !gainPs) {
    std::cerr << "Zero-stub #line manifest lookup with pSourceName==\"\" failed\n";
    return false;
  }
  dis = nullptr;
  if (FAILED(D3DDisassemble(gainPs->GetBufferPointer(), gainPs->GetBufferSize(),
                            0, nullptr, &dis)) || !dis) {
    gainPs->Release();
    return false;
  }
  const std::string gainAsm(static_cast<const char *>(dis->GetBufferPointer()),
                            dis->GetBufferSize());
  dis->Release();
  gainPs->Release();
  if (gainAsm.find("0.750000") == std::string::npos ||
      gainAsm.find("_Gain") != std::string::npos) {
    std::cerr << "Zero-stub #line manifest failed to fold _Gain -> 0.75:\n"
              << gainAsm << '\n';
    return false;
  }

  // 3. Dynamic disable via hook_state.txt must immediately bypass to stock
  //    D3DCompile EVEN within the same 15.6ms NTFS mtime tick (no Sleep, M13)!
  const char kDisabled[] = "seq=1\nenabled=0\nnan_safe=0\n";
  dxcb5_proxy::WriteFileAtomic(sessionDir + L"\\hook_state.txt", kDisabled,
                               std::strlen(kDisabled));
  gFallbackCalls = 0;
  hr = d3d->D3DCompile(kSimplePs, static_cast<unsigned long>(std::strlen(kSimplePs)),
                       "simple.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0,
                       &code, &err);
  if (hr != S_FALSE || gFallbackCalls != 1) {
    std::cerr << "Dynamic enabled=0 did not bypass to stock D3DCompile\n";
    return false;
  }

  // Re-enable immediately (same NTFS tick) and verify cs_5_0 still falls back cleanly.
  const char kEnabled[] = "seq=2\nenabled=1\nnan_safe=0\n";
  dxcb5_proxy::WriteFileAtomic(sessionDir + L"\\hook_state.txt", kEnabled,
                               std::strlen(kEnabled));
  gFallbackCalls = 0;
  hr = d3d->D3DCompile(kSimplePs, static_cast<unsigned long>(std::strlen(kSimplePs)),
                       "cs.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0,
                       &code, &err);
  if (hr != S_FALSE || gFallbackCalls != 1) {
    std::cerr << "Expected cs_5_0 fallback to original D3DCompile\n";
    return false;
  }

  // 4. Migrated compiler coverage (formerly in UnityProxyTest):
  //    WARP device creation, DX9 sampler2D + : COLOR + D3DReflect bindings,
  //    ID3DInclude bridge, -Gec mutable global uniform, double fallback,
  //    and all 4 D3DCOMPILE_OPTIMIZATION_LEVEL* encodings.
  using Microsoft::WRL::ComPtr;
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    std::cerr << "D3D11CreateDevice WARP failed\n";
    return false;
  }

  // 4a. Legacy Unity DX9 sampler2D + tex2D + : COLOR + D3DReflect + WARP.
  const char kLegacySrc[] = R"HLSL(
    sampler2D _FootprintSDF;
    inline float4 SampleHelper(sampler2D t, float2 uv) {
      return tex2D(t, uv) + tex2Dlod(t, float4(uv, 0, 1));
    }
    struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
    float4 LegacyPS(PSIn i) : COLOR {
      return SampleHelper(_FootprintSDF, i.uv);
    }
  )HLSL";
  ComPtr<ID3DBlob> legacyBlob, errBlob;
  if (FAILED(d3d->D3DCompile(kLegacySrc, static_cast<unsigned long>(std::strlen(kLegacySrc)),
                             "legacy.hlsl", nullptr, nullptr, "LegacyPS", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3 |
                                 D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY,
                             0, &legacyBlob, &errBlob)) ||
      !legacyBlob) {
    std::cerr << "Legacy sampler2D compile failed\n";
    return false;
  }
  ComPtr<ID3D11ShaderReflection> legacyRefl;
  D3D11_SHADER_INPUT_BIND_DESC texBind{}, sampBind{};
  ComPtr<ID3D11PixelShader> legacyPsObj;
  if (FAILED(D3DReflect(legacyBlob->GetBufferPointer(),
                        legacyBlob->GetBufferSize(), IID_PPV_ARGS(&legacyRefl))) ||
      FAILED(legacyRefl->GetResourceBindingDescByName("_FootprintSDF", &texBind)) ||
      FAILED(legacyRefl->GetResourceBindingDescByName("sampler_FootprintSDF", &sampBind)) ||
      FAILED(dev->CreatePixelShader(legacyBlob->GetBufferPointer(),
                                    legacyBlob->GetBufferSize(), nullptr, &legacyPsObj))) {
    std::cerr << "Legacy sampler2D D3DReflect or WARP validation failed\n";
    return false;
  }

  // 4b. ID3DInclude bridge.
  struct MemoryInclude final : public ID3DInclude {
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID,
                                   LPCVOID *ppData, UINT *pBytes) override {
      if (std::strcmp(pFileName, "common.hlsl") != 0)
        return E_FAIL;
      static const char kHeader[] =
          "static const float4 kTint = float4(0.25, 0.5, 0.75, 1.0);\n";
      *ppData = kHeader;
      *pBytes = static_cast<UINT>(sizeof(kHeader) - 1);
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
  } memInc;
  const char kIncSrc[] =
      "#include \"common.hlsl\"\n"
      "float4 IncPS(float4 pos : SV_POSITION) : SV_TARGET { return kTint; }\n";
  ComPtr<ID3DBlob> incBlob;
  if (FAILED(d3d->D3DCompile(kIncSrc, static_cast<unsigned long>(std::strlen(kIncSrc)),
                             "inc.hlsl", nullptr, &memInc, "IncPS", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &incBlob, &errBlob)) ||
      !incBlob) {
    std::cerr << "ID3DInclude bridge compile failed\n";
    return false;
  }

  // 4c. Legacy -Gec mutable global uniform (lilToon/Poiyomi pattern).
  const char kMutableGlobalSrc[] = R"HLSL(
    float _Flag;
    float _Limit;
    float4 MutableGlobalPS(float4 pos : SV_POSITION) : SV_TARGET {
      if (_Flag == 0.0) { _Limit = 0.5; }
      float r = fmod(pos.x, _Limit);
      return float4(_Limit, r, 0.75, 1.0);
    }
  )HLSL";
  ComPtr<ID3DBlob> gecBlob;
  gFallbackCalls = 0;
  if (FAILED(d3d->D3DCompile(kMutableGlobalSrc,
                             static_cast<unsigned long>(std::strlen(kMutableGlobalSrc)),
                             "gec.hlsl", nullptr, nullptr, "MutableGlobalPS", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3 |
                                 D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY,
                             0, &gecBlob, &errBlob)) ||
      !gecBlob || gFallbackCalls != 0) {
    std::cerr << "Legacy -Gec mutable global uniform compile failed or fell back\n";
    return false;
  }

  // 4d. double ps_5_0 falls back cleanly to stock D3DCompile.
  const char kDoubleSrc[] = R"HLSL(
    cbuffer CB { double v; };
    float4 DoublePS(float4 pos : SV_POSITION) : SV_TARGET {
      double d = v * 2.0 + 1.0;
      return float4((float)d, 0, 0, 1);
    }
  )HLSL";
  ComPtr<ID3DBlob> dblBlob;
  gFallbackCalls = 0;
  hr = d3d->D3DCompile(kDoubleSrc, static_cast<unsigned long>(std::strlen(kDoubleSrc)),
                       "dbl.hlsl", nullptr, nullptr, "DoublePS", "ps_5_0",
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &dblBlob, &errBlob);
  if (hr != S_FALSE || gFallbackCalls != 1) {
    std::cerr << "Expected double ps_5_0 fallback to original D3DCompile\n";
    return false;
  }

  // 4e. All 4 D3DCOMPILE_OPTIMIZATION_LEVEL* bitmask encodings compile cleanly.
  for (UINT optFlag : {D3DCOMPILE_OPTIMIZATION_LEVEL0,
                       D3DCOMPILE_OPTIMIZATION_LEVEL1,
                       D3DCOMPILE_OPTIMIZATION_LEVEL2,
                       D3DCOMPILE_OPTIMIZATION_LEVEL3}) {
    ComPtr<ID3DBlob> optBlob;
    if (FAILED(d3d->D3DCompile(kSimplePs, static_cast<unsigned long>(std::strlen(kSimplePs)),
                               "opt.hlsl", nullptr, nullptr, "PSMain", "ps_5_0",
                               optFlag, 0, &optBlob, &errBlob)) ||
        !optBlob || optBlob->GetBufferSize() < 32) {
      std::cerr << "Opt-level compile failed for flag " << optFlag << '\n';
      return false;
    }
  }
  return true;
}

bool TestSha256AndCacheHardening() {
  auto ShaHex = [](const char *S) {
    dxcb5::Sha256 H;
    H.Update(S, std::strlen(S));
    return H.Hex64();
  };
  if (ShaHex("") !=
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
      ShaHex("abc") !=
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ||
      ShaHex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") !=
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") {
    std::cerr << "SHA-256 NIST test vector mismatch\n";
    return false;
  }
  const std::wstring H1 = dxcb5_proxy::HashDefinesHex({{"A=1", 0u}});
  const std::wstring H2 = dxcb5_proxy::HashDefinesHex({{"A", 1u}});
  if (H1.size() != 32u || H2.size() != 32u || H1 == H2) {
    std::cerr << "HashDefinesHex length or boundary-collision check failed\n";
    return false;
  }
  uint8_t BadBuf[32] = {'D', 'X', 'B', 'C'};
  uint32_t BadSz = 64u;
  std::memcpy(BadBuf + 24, &BadSz, sizeof(BadSz));
  if (dxcb5_is_valid_dxbc_container_header(BadBuf, sizeof(BadBuf)) != 0 ||
      dxcb5_is_valid_dxbc_container_header("NOT_DXBC", 8) != 0) {
    std::cerr << "dxcb5_is_valid_dxbc_container_header failed to reject corrupt container\n";
    return false;
  }
  uint32_t GoodSz = 32u;
  std::memcpy(BadBuf + 24, &GoodSz, sizeof(GoodSz));
  if (dxcb5_is_valid_dxbc_container_header(BadBuf, sizeof(BadBuf)) != 1) {
    std::cerr << "dxcb5_is_valid_dxbc_container_header rejected valid 32-byte header\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 5) {
    std::cerr << "Usage: DXCB5UnityHookTest <unity_hook.dll> <cgbatch_plugin.dll> <dxcompiler.dll> <test_dir>\n";
    return 1;
  }
  auto toW = [](const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1)
      return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.pop_back();
    return w;
  };
  const std::wstring unityHookPath = toW(argv[1]);
  const std::wstring pluginPath = toW(argv[2]);
  const std::wstring dxcPath = toW(argv[3]);
  const std::wstring testDir = toW(argv[4]);
  EnsureDirTreeW(testDir);

  if (!TestSha256AndCacheHardening()) return 1;
  if (!TestMinHookLifecycle()) return 1;
  if (!TestUnityTypesAndReplay()) return 1;
  if (!TestSnippetCacheKeyNoKeywordCollisions(testDir)) return 1;
  if (!TestCmdLineEnvAndSafeStaging(testDir)) return 1;
  if (!TestUnityHookDllLifecycle(unityHookPath, testDir)) return 1;
  if (!TestCgBatchPluginLive(pluginPath, dxcPath, testDir)) return 1;
  return 0;
}
