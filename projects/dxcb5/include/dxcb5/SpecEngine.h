// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_SPEC_ENGINE_H
#define DXCB5_SPEC_ENGINE_H

#include "dxcb5/Compiler.h"
#include "llvm/ADT/StringRef.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace dxcb5 {
namespace spec {

enum class ConstType : uint8_t { F32, I32, U32, Bool };

struct SpecConstValue {
  ConstType Type = ConstType::F32;
  std::vector<double> Values;
};

struct SpecTextureValue {
  std::array<float, 4> Color{0.0f, 0.0f, 0.0f, 1.0f};
  bool HasDims = false;
  bool AddressBorder = false;
  uint8_t DimCount = 0;
  uint32_t Width = 0;
  uint32_t Height = 0;
  uint32_t Depth = 1;
};

struct SpecOptions {
  bool CompactGlobals = true;
  bool CrossStageDce = true;
  bool EmitIntermediateDxil = false;
  bool EmitStats = false;
  bool NanSafe = false;
};

struct SpecManifest {
  uint32_t SchemaVersion = 1;
  std::string UberDxilHash;
  std::map<std::string, SpecConstValue> Constants;
  std::map<std::string, SpecTextureValue> ConstTextures;
  std::map<std::string, uint32_t> Defines;
  SpecOptions Options;
};

struct StageStats {
  std::string Stage;
  CompileStats Backend;
  std::vector<std::string> StrippedTextures;
  std::vector<std::string> StrippedInputs;
  double SpecMs = 0.0;
  double BackendMs = 0.0;
  bool CacheHit = false;
};

std::string ComputeSha256Hex(const void *Data, size_t Size);

bool ParseSpecManifest(llvm::StringRef Json, SpecManifest &Out,
                       std::string &Error);
std::string FormatStageStatsJson(const StageStats &Stats);
std::string FormatPairStatsJson(const StageStats &VsStats,
                                const StageStats &PsStats);

bool SerializeModuleBitcode(const llvm::Module &M, std::string &Out);
std::unique_ptr<llvm::Module> ParseModuleBitcode(llvm::StringRef Bytes,
                                                 llvm::LLVMContext &Ctx,
                                                 std::string &Error);

bool InspectDxilBytes(llvm::StringRef DxilBytes, std::string &OutJson,
                      std::string &Error);
bool InspectDxilModule(const llvm::Module &M, llvm::StringRef RawHashHex,
                       std::string &OutJson, std::string &Error);

bool FoldDxilMathIntrinsics(llvm::Module &M);

bool SpecializeDxilModule(llvm::Module &M, const SpecManifest &Manifest,
                          bool ApplyDefines, bool CompactGlobals,
                          std::vector<std::string> &StrippedTextures,
                          std::string &Error);

bool PruneCrossStageSignatures(llvm::Module &VsMod, llvm::Module &PsMod,
                               std::vector<std::string> &StrippedInputs,
                               std::string &Error);

struct PairResult {
  std::string VsDxilOpt;
  std::string PsDxilOpt;
  std::string VsDxbc;
  std::string PsDxbc;
  StageStats VsStats;
  StageStats PsStats;
};

bool SpecializePairBytes(llvm::StringRef VsDxil, llvm::StringRef PsDxil,
                         llvm::StringRef ManifestJson, bool WantVsOpt,
                         bool WantPsOpt, bool WantVsDxbc, bool WantPsDxbc,
                         PairResult &Out, std::string &Error);

bool SpecializeStageBytes(llvm::StringRef StageDxil,
                          llvm::StringRef ManifestJson, std::string &OutDxbc,
                          StageStats &OutStats, std::string &Error);

} // namespace spec
} // namespace dxcb5

#endif // DXCB5_SPEC_ENGINE_H
