// Distributed under the University of Illinois Open Source License.
#include "dxcb5/dxcb5_spec.h"
#include "dxcb5/SpecEngine.h"

namespace dxcb5 {
bool SetDisabledOptimizationPasses(const char *PassList);
}
#include "UnityHlslAdapter.h"
#include "dxc/DxilContainer/DxilContainer.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/Unicode.h"
#include "dxc/Support/WinIncludes.h"
#include "dxc/dxcapi.h"
#include "llvm/ADT/StringRef.h"
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

using namespace llvm;
using namespace hlsl;

namespace {

bool AllocBlob(const void *Src, size_t Size, dxcb5_blob *Out) noexcept {
  if (!Out)
    return true;
  Out->data = nullptr;
  Out->size = 0;
  if (!Size)
    return true;
  void *Mem = std::malloc(Size + 1);
  if (!Mem)
    return false;
  std::memcpy(Mem, Src, Size);
  static_cast<char *>(Mem)[Size] = '\0';
  Out->data = Mem;
  Out->size = Size;
  return true;
}

int FailWithMsg(const char *Msg, dxcb5_blob *OutErrors,
                int Code = DXCB5_ERR_SPEC) noexcept {
  if (OutErrors && Msg && *Msg)
    AllocBlob(Msg, std::strlen(Msg), OutErrors);
  return Code;
}

int FailWithMsg(const std::string &Msg, dxcb5_blob *OutErrors,
                int Code = DXCB5_ERR_SPEC) noexcept {
  if (OutErrors && !Msg.empty())
    AllocBlob(Msg.data(), Msg.size(), OutErrors);
  return Code;
}

int ClassifySpecError(const std::string &Msg) noexcept {
  if (Msg.find("uber_dxil_hash mismatch") != std::string::npos)
    return DXCB5_ERR_HASH_MISMATCH;
  if (Msg.find("parse") != std::string::npos ||
      Msg.find("manifest") != std::string::npos ||
      Msg.find("schema_version") != std::string::npos ||
      Msg.find("JSON") != std::string::npos ||
      Msg.find("must be") != std::string::npos)
    return DXCB5_ERR_PARSE;
  return DXCB5_ERR_SPEC;
}

template <typename Fn>
int RunGuarded(dxcb5_blob *OutErrors, Fn &&Body) noexcept {
  try {
    return Body();
  } catch (const hlsl::Exception &E) {
    return FailWithMsg(
        E.msg.empty() ? "DXIL exception" : E.msg.c_str(), OutErrors,
        DXCB5_ERR_EXCEPTION);
  } catch (const std::exception &E) {
    return FailWithMsg(E.what() ? E.what() : "C++ exception", OutErrors,
                       DXCB5_ERR_EXCEPTION);
  } catch (...) {
    return FailWithMsg("unknown C++ exception", OutErrors, DXCB5_ERR_EXCEPTION);
  }
}

std::wstring Utf8ToWideStr(const char *S) {
  if (!S || !*S)
    return {};
  std::wstring Out;
  Unicode::UTF8ToWideString(S, &Out);
  return Out;
}

bool ExtractBitcodeFromDxilOrRaw(const void *Data, size_t Size,
                                 StringRef &Bitcode) noexcept {
  if (Size >= 4 && std::memcmp(Data, "DXBC", 4) == 0) {
    const auto *Hdr = IsDxilContainerLike(Data, Size);
    if (!Hdr || !IsValidDxilContainer(Hdr, Size))
      return false;
    const DxilPartHeader *Part = GetDxilPartByType(Hdr, DFCC_DXIL);
    if (!Part || Part->PartSize < sizeof(DxilProgramHeader))
      return false;
    const auto *Prog =
        reinterpret_cast<const DxilProgramHeader *>(GetDxilPartData(Part));
    if (!IsValidDxilProgramHeader(Prog, Part->PartSize))
      return false;
    const char *Bc = nullptr;
    uint32_t BcLen = 0;
    GetDxilProgramBitcode(Prog, &Bc, &BcLen);
    Bitcode = StringRef(Bc, BcLen);
    return Bc && BcLen > 0;
  }
  Bitcode = StringRef(static_cast<const char *>(Data), Size);
  return true;
}

} // namespace

extern "C" {

uint32_t dxcb5_get_abi_version(void) { return DXCB5_SPEC_ABI_VERSION; }

void dxcb5_free_blob(dxcb5_blob *blob) {
  if (!blob)
    return;
  if (blob->data)
    std::free(const_cast<void *>(blob->data));
  blob->data = nullptr;
  blob->size = 0;
}

int dxcb5_compile_uber_dxil(const char *hlsl_utf8, size_t hlsl_size,
                            const dxcb5_uber_options *opts,
                            dxcb5_blob *out_dxil,
                            dxcb5_blob *out_reflection_json,
                            dxcb5_blob *out_errors) {
  if (out_dxil) *out_dxil = {nullptr, 0};
  if (out_reflection_json) *out_reflection_json = {nullptr, 0};
  if (out_errors) *out_errors = {nullptr, 0};
  if (!hlsl_utf8 || !hlsl_size || !opts || !opts->target || !out_dxil)
    return FailWithMsg("invalid arguments to dxcb5_compile_uber_dxil",
                       out_errors, DXCB5_ERR_INVALID_ARG);
  if (opts->struct_size != 0 &&
      opts->struct_size != sizeof(dxcb5_uber_options))
    return FailWithMsg("dxcb5_uber_options::struct_size ABI mismatch",
                       out_errors, DXCB5_ERR_ABI_MISMATCH);

  return RunGuarded(out_errors, [&]() -> int {
    CComPtr<IDxcCompiler3> Compiler;
    CComPtr<IDxcUtils> Utils;
    if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler))) ||
        FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utils))))
      return FailWithMsg("failed to instantiate DxcCompiler", out_errors,
                         DXCB5_ERR_INIT);

    std::wstring Target = Utf8ToWideStr(opts->target);
    if (Target == L"vs_6_0") Target = L"vs_5_0";
    else if (Target == L"ps_6_0") Target = L"ps_5_0";
    const bool IsPixel = (Target == L"ps_5_0");

    std::string AdaptedSrc;
    const char *SrcPtr = hlsl_utf8;
    size_t SrcLen = hlsl_size;
    if ((opts->flags & DXCB5_UBER_FLAG_UNITY_ADAPT) &&
        dxcb5_proxy::AdaptUnityHlslForDxc(SrcPtr, SrcLen, AdaptedSrc,
                                          opts->entry ? opts->entry : "main",
                                          IsPixel)) {
      SrcPtr = AdaptedSrc.data();
      SrcLen = AdaptedSrc.size();
    }

    const char *DefaultHv = (opts->flags & DXCB5_UBER_FLAG_GEC) ? "2016" : "2018";
    std::vector<std::wstring> OwnedArgs;
    OwnedArgs.push_back(L"-fdxcb5");
    OwnedArgs.push_back(L"-fdxcb5-emit-uber-dxil");
    OwnedArgs.push_back(L"-T");
    OwnedArgs.push_back(Target);
    OwnedArgs.push_back(L"-E");
    OwnedArgs.push_back(Utf8ToWideStr(opts->entry ? opts->entry : "main"));
    OwnedArgs.push_back(L"-HV");
    OwnedArgs.push_back(
        Utf8ToWideStr(opts->hlsl_version ? opts->hlsl_version : DefaultHv));
    OwnedArgs.push_back((opts->flags & DXCB5_UBER_FLAG_SKIP_OPT) ? L"-Od" : L"-O3");
    if (opts->flags & DXCB5_UBER_FLAG_NAN_SAFE)
      OwnedArgs.push_back(L"--nan-safe");
    if (opts->flags & DXCB5_UBER_FLAG_GEC)
      OwnedArgs.push_back(L"-Gec");
    if (opts->flags & DXCB5_UBER_FLAG_IEEE_STRICT)
      OwnedArgs.push_back(L"-Gis");
    if (opts->flags & DXCB5_UBER_FLAG_ROW_MAJOR)
      OwnedArgs.push_back(L"-Zpr");
    if (opts->flags & DXCB5_UBER_FLAG_COL_MAJOR)
      OwnedArgs.push_back(L"-Zpc");
    for (uint32_t I = 0; I < opts->include_dir_count; ++I) {
      if (opts->include_dirs && opts->include_dirs[I]) {
        OwnedArgs.push_back(L"-I");
        OwnedArgs.push_back(Utf8ToWideStr(opts->include_dirs[I]));
      }
    }
    for (uint32_t I = 0; I < opts->define_count; ++I) {
      if (opts->defines && opts->defines[I].name) {
        std::string Def = opts->defines[I].name;
        if (opts->defines[I].value && *opts->defines[I].value) {
          Def += '=';
          Def += opts->defines[I].value;
        }
        OwnedArgs.push_back(L"-D");
        OwnedArgs.push_back(Utf8ToWideStr(Def.c_str()));
      }
    }
    if (opts->source_name && *opts->source_name)
      OwnedArgs.push_back(Utf8ToWideStr(opts->source_name));

    std::vector<LPCWSTR> ArgPtrs;
    ArgPtrs.reserve(OwnedArgs.size());
    for (const auto &A : OwnedArgs)
      ArgPtrs.push_back(A.c_str());

    CComPtr<IDxcIncludeHandler> IncHandler;
    Utils->CreateDefaultIncludeHandler(&IncHandler);

    DxcBuffer SrcBuf{SrcPtr, SrcLen, DXC_CP_UTF8};
    CComPtr<IDxcResult> Result;
    HRESULT Hr = Compiler->Compile(&SrcBuf, ArgPtrs.data(),
                                   static_cast<UINT32>(ArgPtrs.size()),
                                   IncHandler, IID_PPV_ARGS(&Result));
    if (FAILED(Hr) || !Result)
      return FailWithMsg("DxcCompiler::Compile failed", out_errors,
                         DXCB5_ERR_COMPILE);

    HRESULT Status = E_FAIL;
    Result->GetStatus(&Status);
    if (FAILED(Status)) {
      CComPtr<IDxcBlobUtf8> Errs;
      Result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&Errs), nullptr);
      std::string Msg =
          (Errs && Errs->GetStringLength())
              ? std::string(Errs->GetStringPointer(), Errs->GetStringLength())
              : "Uber DXIL compilation failed";
      return FailWithMsg(Msg, out_errors, DXCB5_ERR_COMPILE);
    }

    CComPtr<IDxcBlob> Obj;
    if (FAILED(Result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&Obj), nullptr)) ||
        !Obj || !Obj->GetBufferSize())
      return FailWithMsg("empty Uber DXIL output", out_errors,
                         DXCB5_ERR_COMPILE);

    std::string RefJson;
    if (out_reflection_json) {
      std::string Err;
      StringRef Bitcode(static_cast<const char *>(Obj->GetBufferPointer()),
                        Obj->GetBufferSize());
      if (!dxcb5::spec::InspectDxilBytes(Bitcode, RefJson, Err))
        return FailWithMsg(Err, out_errors, DXCB5_ERR_PARSE);
    }

    if (!AllocBlob(Obj->GetBufferPointer(), Obj->GetBufferSize(), out_dxil))
      return FailWithMsg("out of memory allocating Uber DXIL blob", out_errors,
                         DXCB5_ERR_OOM);

    if (out_reflection_json) {
      if (!AllocBlob(RefJson.data(), RefJson.size(), out_reflection_json)) {
        dxcb5_free_blob(out_dxil);
        return FailWithMsg("out of memory allocating reflection JSON",
                           out_errors, DXCB5_ERR_OOM);
      }
    }
    return DXCB5_OK;
  });
}

int dxcb5_inspect_dxil(const void *dxil_data, size_t dxil_size,
                       dxcb5_blob *out_reflection_json,
                       dxcb5_blob *out_errors) {
  if (out_reflection_json) *out_reflection_json = {nullptr, 0};
  if (out_errors) *out_errors = {nullptr, 0};
  if (!dxil_data || !dxil_size || !out_reflection_json)
    return FailWithMsg("invalid arguments to dxcb5_inspect_dxil", out_errors,
                       DXCB5_ERR_INVALID_ARG);
  return RunGuarded(out_errors, [&]() -> int {
    StringRef Bitcode;
    if (!ExtractBitcodeFromDxilOrRaw(dxil_data, dxil_size, Bitcode))
      return FailWithMsg("invalid DXIL buffer", out_errors, DXCB5_ERR_PARSE);
    std::string Json, Err;
    if (!dxcb5::spec::InspectDxilBytes(Bitcode, Json, Err))
      return FailWithMsg(Err, out_errors, DXCB5_ERR_PARSE);
    if (!AllocBlob(Json.data(), Json.size(), out_reflection_json))
      return FailWithMsg("out of memory allocating reflection JSON", out_errors,
                         DXCB5_ERR_OOM);
    return DXCB5_OK;
  });
}

int dxcb5_specialize_pair(const void *vs_dxil_data, size_t vs_dxil_size,
                          const void *ps_dxil_data, size_t ps_dxil_size,
                          const char *manifest_json_utf8, size_t manifest_size,
                          dxcb5_blob *out_vs_dxil_opt,
                          dxcb5_blob *out_ps_dxil_opt, dxcb5_blob *out_vs_dxbc,
                          dxcb5_blob *out_ps_dxbc, dxcb5_blob *out_stats_json,
                          dxcb5_blob *out_errors) {
  if (out_vs_dxil_opt) *out_vs_dxil_opt = {nullptr, 0};
  if (out_ps_dxil_opt) *out_ps_dxil_opt = {nullptr, 0};
  if (out_vs_dxbc) *out_vs_dxbc = {nullptr, 0};
  if (out_ps_dxbc) *out_ps_dxbc = {nullptr, 0};
  if (out_stats_json) *out_stats_json = {nullptr, 0};
  if (out_errors) *out_errors = {nullptr, 0};
  if (!vs_dxil_data || !vs_dxil_size || !ps_dxil_data || !ps_dxil_size)
    return FailWithMsg("invalid arguments to dxcb5_specialize_pair", out_errors,
                       DXCB5_ERR_INVALID_ARG);

  return RunGuarded(out_errors, [&]() -> int {
    StringRef VsBc, PsBc;
    if (!ExtractBitcodeFromDxilOrRaw(vs_dxil_data, vs_dxil_size, VsBc) ||
        !ExtractBitcodeFromDxilOrRaw(ps_dxil_data, ps_dxil_size, PsBc))
      return FailWithMsg("invalid VS or PS DXIL buffer", out_errors,
                         DXCB5_ERR_PARSE);

    StringRef Manifest(manifest_json_utf8 ? manifest_json_utf8 : "",
                       manifest_json_utf8 ? manifest_size : 0);
    dxcb5::spec::PairResult Res;
    std::string Err;
    if (!dxcb5::spec::SpecializePairBytes(
            VsBc, PsBc, Manifest, out_vs_dxil_opt != nullptr,
            out_ps_dxil_opt != nullptr, out_vs_dxbc != nullptr,
            out_ps_dxbc != nullptr, Res, Err))
      return FailWithMsg(Err, out_errors, ClassifySpecError(Err));

    std::string StatsJson;
    if (out_stats_json)
      StatsJson = dxcb5::spec::FormatPairStatsJson(Res.VsStats, Res.PsStats);

    auto CleanupOutputs = [&]() noexcept {
      dxcb5_free_blob(out_vs_dxil_opt);
      dxcb5_free_blob(out_ps_dxil_opt);
      dxcb5_free_blob(out_vs_dxbc);
      dxcb5_free_blob(out_ps_dxbc);
      dxcb5_free_blob(out_stats_json);
    };
    if ((out_vs_dxil_opt &&
         !AllocBlob(Res.VsDxilOpt.data(), Res.VsDxilOpt.size(), out_vs_dxil_opt)) ||
        (out_ps_dxil_opt &&
         !AllocBlob(Res.PsDxilOpt.data(), Res.PsDxilOpt.size(), out_ps_dxil_opt)) ||
        (out_vs_dxbc &&
         !AllocBlob(Res.VsDxbc.data(), Res.VsDxbc.size(), out_vs_dxbc)) ||
        (out_ps_dxbc &&
         !AllocBlob(Res.PsDxbc.data(), Res.PsDxbc.size(), out_ps_dxbc)) ||
        (out_stats_json &&
         !AllocBlob(StatsJson.data(), StatsJson.size(), out_stats_json))) {
      CleanupOutputs();
      return FailWithMsg("OOM", out_errors, DXCB5_ERR_OOM);
    }
    return DXCB5_OK;
  });
}

int dxcb5_specialize_stage(const void *stage_dxil_data, size_t stage_dxil_size,
                           const char *manifest_json_utf8, size_t manifest_size,
                           dxcb5_blob *out_dxbc, dxcb5_blob *out_stats_json,
                           dxcb5_blob *out_errors) {
  if (out_dxbc) *out_dxbc = {nullptr, 0};
  if (out_stats_json) *out_stats_json = {nullptr, 0};
  if (out_errors) *out_errors = {nullptr, 0};
  if (!stage_dxil_data || !stage_dxil_size || !out_dxbc)
    return FailWithMsg("invalid arguments to dxcb5_specialize_stage",
                       out_errors, DXCB5_ERR_INVALID_ARG);

  return RunGuarded(out_errors, [&]() -> int {
    StringRef Bc;
    if (!ExtractBitcodeFromDxilOrRaw(stage_dxil_data, stage_dxil_size, Bc))
      return FailWithMsg("invalid stage DXIL buffer", out_errors,
                         DXCB5_ERR_PARSE);
    StringRef Manifest(manifest_json_utf8 ? manifest_json_utf8 : "",
                       manifest_json_utf8 ? manifest_size : 0);
    std::string Dxbc, Err;
    dxcb5::spec::StageStats Stats;
    if (!dxcb5::spec::SpecializeStageBytes(Bc, Manifest, Dxbc, Stats, Err))
      return FailWithMsg(Err, out_errors, ClassifySpecError(Err));

    std::string StatsJson;
    if (out_stats_json)
      StatsJson = dxcb5::spec::FormatStageStatsJson(Stats);

    if (!AllocBlob(Dxbc.data(), Dxbc.size(), out_dxbc))
      return FailWithMsg("OOM", out_errors, DXCB5_ERR_OOM);
    if (out_stats_json &&
        !AllocBlob(StatsJson.data(), StatsJson.size(), out_stats_json)) {
      dxcb5_free_blob(out_dxbc);
      return FailWithMsg("OOM", out_errors, DXCB5_ERR_OOM);
    }
    return DXCB5_OK;
  });
}

int dxcb5_set_disabled_passes(const char *passes) {
  return dxcb5::SetDisabledOptimizationPasses(passes) ? DXCB5_OK
                                                      : DXCB5_ERR_INVALID_PASS;
}

void dxcb5_configure_oracle(const char *dump_stages_dir,
                            const char *dump_metrics_path, int list_passes) {
  dxcb5::ConfigureOracle(dump_stages_dir, dump_metrics_path, list_passes != 0);
}

} // extern "C"
