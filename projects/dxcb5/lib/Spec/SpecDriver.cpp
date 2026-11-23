// Distributed under the University of Illinois Open Source License.
#include "Spec/SpecInternal.h"
#include "dxcb5/Compiler.h"
#include "dxcb5/DxbcWriter.h"
#include "dxcb5/SpecEngine.h"
#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilShaderModel.h"
#include "dxc/DXIL/DxilTypeSystem.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <map>
#include <set>

using namespace dxcb5;
using namespace dxcb5::spec;
using namespace hlsl;
using namespace llvm;

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point Start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
}

void MarkLiveCBufferVarsUsed(Module &M) {
  DxilModule &Dxil = M.GetOrCreateDxilModule();
  const Function *Entry = Dxil.GetEntryFunction();
  if (!Entry)
    return;
  auto &Types = Dxil.GetTypeSystem();
  std::map<unsigned, const DxilCBuffer *> CbById;
  for (const auto &CB : Dxil.GetCBuffers())
    CbById[CB->GetID()] = CB.get();
  std::map<unsigned, std::set<unsigned>> LiveDwords;
  std::map<unsigned, std::set<unsigned>> LiveFields;
  std::set<unsigned> DynCbs;
  bool AllCbsDynamic = false;
  for (const BasicBlock &BB : *Entry) {
    for (const llvm::Instruction &Inst : BB) {
      const auto *CI = dyn_cast<CallInst>(&Inst);
      if (!CI || !OP::IsDxilOpFuncCallInst(CI, DXIL::OpCode::CBufferLoadLegacy))
        continue;
      const HandleInfo HI = DecodeHandle(CI->getArgOperand(1));
      if (!HI.Valid) {
        AllCbsDynamic = true;
        continue;
      }
      if (HI.Class != DXIL::ResourceClass::CBuffer)
        continue;
      if (const auto *RowC = dyn_cast<ConstantInt>(CI->getArgOperand(2))) {
        RecordConstCBufferLoadDwords(
            CI, static_cast<unsigned>(RowC->getZExtValue()),
            LiveDwords[HI.RangeId]);
        continue;
      }
      auto It = CbById.find(HI.RangeId);
      unsigned FieldIdx = 0;
      if (It != CbById.end() &&
          ResolveDynamicCBufferField(CI->getArgOperand(2), &BB, *It->second,
                                     Types, FieldIdx)) {
        LiveFields[HI.RangeId].insert(FieldIdx);
      } else {
        DynCbs.insert(HI.RangeId);
      }
    }
  }
  for (const auto &CB : Dxil.GetCBuffers()) {
    auto *ST = dyn_cast<StructType>(CB->GetHLSLType()->getPointerElementType());
    auto *Ann = ST ? Types.GetStructAnnotation(ST) : nullptr;
    if (!Ann)
      continue;
    const unsigned CbId = CB->GetID();
    const bool AllUsed = AllCbsDynamic || DynCbs.count(CbId) != 0;
    const auto &Dwords = LiveDwords[CbId];
    const auto &Fields = LiveFields[CbId];
    for (unsigned I = 0; I < ST->getNumElements(); ++I) {
      auto &F = Ann->GetFieldAnnotation(I);
      Type *ElemTy = ST->getElementType(I);
      if (IsEmptyCBufferField(ElemTy)) {
        F.SetCBVarUsed(false);
        continue;
      }
      if (AllUsed || Fields.count(I)) {
        F.SetCBVarUsed(true);
        continue;
      }
      F.SetCBVarUsed(IsCBufferFieldUsedByDwords(ElemTy, F, Types, Dwords));
    }
  }
}

bool CompileModuleToDxbc(Module &M, ArrayRef<SignatureElement> VertexOutput,
                         bool NanSafe, std::string &OutDxbc,
                         StageStats &Stats, std::string &Error) {
  MarkLiveCBufferVarsUsed(M);
  const auto T0 = Clock::now();
  OutDxbc.clear();
  std::string Diag;
  raw_string_ostream OutOS(OutDxbc);
  raw_string_ostream DiagOS(Diag);
  const HRESULT Hr = dxcb5::Compile(M, VertexOutput, OutOS, DiagOS, {},
                                    NanSafe, &Stats.Backend);
  OutOS.flush();
  DiagOS.flush();
  Stats.BackendMs = ElapsedMs(T0);
  if (FAILED(Hr)) {
    Error = Diag.empty() ? "DXCB5 backend compilation failed" : Diag;
    return false;
  }
  return true;
}

} // namespace

bool dxcb5::spec::SpecializePairBytes(StringRef VsDxil, StringRef PsDxil,
                                      StringRef ManifestJson, bool WantVsOpt,
                                      bool WantPsOpt, bool WantVsDxbc,
                                      bool WantPsDxbc, PairResult &Out,
                                      std::string &Error) {
  SpecManifest Manifest;
  if (!ParseSpecManifest(ManifestJson, Manifest, Error))
    return false;

  const auto TSpecStart = Clock::now();
  LLVMContext VsCtx, PsCtx;
  auto VsMod = ParseModuleBitcode(VsDxil, VsCtx, Error);
  if (!VsMod)
    return false;
  auto PsMod = ParseModuleBitcode(PsDxil, PsCtx, Error);
  if (!PsMod)
    return false;

  Out.VsStats.Stage = "vs_5_0";
  Out.PsStats.Stage = "ps_5_0";

  // Step 1: Material-level specialization ONLY (keep DXCB5_SpecFlags symbolic
  // if intermediate DXIL is requested so cold pDefines cache-misses stay sound).
  // Defer $Globals compaction until after Cross-Stage DCE so VS and PS share
  // one unified, collision-free $Globals layout (Unity merges $Globals per pass).
  const bool BakeDefinesInStep1 = !WantVsOpt && !WantPsOpt;
  if (!SpecializeDxilModule(*PsMod, Manifest, BakeDefinesInStep1,
                            /*CompactGlobals=*/false,
                            Out.PsStats.StrippedTextures, Error))
    return false;
  if (!SpecializeDxilModule(*VsMod, Manifest, BakeDefinesInStep1,
                            /*CompactGlobals=*/false,
                            Out.VsStats.StrippedTextures, Error))
    return false;

  if (Manifest.Options.CrossStageDce) {
    if (!PruneCrossStageSignatures(*VsMod, *PsMod, Out.PsStats.StrippedInputs,
                                   Error))
      return false;
  }
  if (Manifest.Options.CompactGlobals)
    CompactSharedGlobalsCBuffer(*VsMod, *PsMod);

  if (WantVsOpt && !SerializeModuleBitcode(*VsMod, Out.VsDxilOpt)) {
    Error = "failed to serialize optimized VS DXIL";
    return false;
  }
  if (WantPsOpt && !SerializeModuleBitcode(*PsMod, Out.PsDxilOpt)) {
    Error = "failed to serialize optimized PS DXIL";
    return false;
  }

  // Step 2: Apply manifest.defines (DXCB5_SpecFlags) if not yet baked.
  if ((WantVsDxbc || WantPsDxbc) && !BakeDefinesInStep1) {
    if (WantPsDxbc &&
        !SpecializeDxilModule(*PsMod, Manifest, /*ApplyDefines=*/true,
                              /*CompactGlobals=*/false,
                              Out.PsStats.StrippedTextures, Error))
      return false;
    if (WantVsDxbc &&
        !SpecializeDxilModule(*VsMod, Manifest, /*ApplyDefines=*/true,
                              /*CompactGlobals=*/false,
                              Out.VsStats.StrippedTextures, Error))
      return false;
    if (Manifest.Options.CrossStageDce && WantVsDxbc && WantPsDxbc) {
      std::vector<std::string> Step2Stripped;
      if (!PruneCrossStageSignatures(*VsMod, *PsMod, Step2Stripped, Error))
        return false;
      Out.PsStats.StrippedInputs.insert(Out.PsStats.StrippedInputs.end(),
                                        Step2Stripped.begin(),
                                        Step2Stripped.end());
    }
    // Do NOT re-compact $Globals after serializing VsDxilOpt/PsDxilOpt:
    // cold SpecializeStageBytes reloads keep Step 1's shared layout, so
    // re-compacting here would diverge offsets from cold reloads.
  }

  const double SpecTotalMs = ElapsedMs(TSpecStart);
  Out.VsStats.SpecMs = SpecTotalMs * 0.5;
  Out.PsStats.SpecMs = SpecTotalMs * 0.5;

  std::vector<SignatureElement> VsOutputs;
  if (WantVsDxbc) {
    if (!CompileModuleToDxbc(*VsMod, {}, Manifest.Options.NanSafe, Out.VsDxbc,
                             Out.VsStats, Error))
      return false;
    if (WantPsDxbc) {
      std::string SigErr;
      if (!ReadVertexOutputSignature(Out.VsDxbc, VsOutputs, SigErr))
        VsOutputs.clear();
    }
  }
  if (WantPsDxbc) {
    if (!CompileModuleToDxbc(*PsMod, VsOutputs, Manifest.Options.NanSafe,
                             Out.PsDxbc, Out.PsStats, Error))
      return false;
  }
  return true;
}

bool dxcb5::spec::SpecializeStageBytes(StringRef StageDxil,
                                       StringRef ManifestJson,
                                       std::string &OutDxbc,
                                       StageStats &OutStats,
                                       std::string &Error) {
  SpecManifest Manifest;
  if (!ParseSpecManifest(ManifestJson, Manifest, Error))
    return false;
  if (!Manifest.UberDxilHash.empty()) {
    const std::string ActualHash =
        ComputeSha256Hex(StageDxil.data(), StageDxil.size());
    if (ActualHash != Manifest.UberDxilHash) {
      Error = "manifest uber_dxil_hash mismatch (expected " +
              Manifest.UberDxilHash + ", got " + ActualHash + ")";
      return false;
    }
  }

  const auto TSpecStart = Clock::now();
  LLVMContext Ctx;
  auto Mod = ParseModuleBitcode(StageDxil, Ctx, Error);
  if (!Mod)
    return false;
  const auto *SM = Mod->GetOrCreateDxilModule().GetShaderModel();
  if (!SM || (!SM->IsPS() && !SM->IsVS())) {
    Error = "unsupported DXIL shader model (expected vs_5_0 or ps_5_0)";
    return false;
  }
  OutStats.Stage = SM->IsPS() ? "ps_5_0" : "vs_5_0";

  if (!SpecializeDxilModule(*Mod, Manifest, /*ApplyDefines=*/true,
                            Manifest.Options.CompactGlobals,
                            OutStats.StrippedTextures, Error))
    return false;
  OutStats.SpecMs = ElapsedMs(TSpecStart);

  return CompileModuleToDxbc(*Mod, {}, Manifest.Options.NanSafe, OutDxbc,
                             OutStats, Error);
}
