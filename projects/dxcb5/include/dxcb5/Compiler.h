// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_COMPILER_H
#define DXCB5_COMPILER_H

#include "dxc/Support/WinIncludes.h"
#include "dxcb5/Mir.h"
#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class Instruction;
class Module;
class raw_ostream;
}

namespace dxcb5 {

using VectorPhiGroup = std::array<const llvm::Instruction *, 4>;

struct CompileStats {
  uint32_t Instructions = 0;
  uint32_t Temps = 0;
  uint32_t IndexableTemps = 0;
  uint32_t SfuOps = 0;
  uint32_t TexOps = 0;
  uint32_t BranchOps = 0;
  uint32_t GlobalsBytes = 0;
  uint64_t DynamicCost = 0; // mu: loop-weighted instruction count
  uint32_t PeakLanes = 0;   // PL: exact intra-block peak live scalar lanes
};

// Configures explicit CLI/test oracle diagnostics (no environment variables).
void ConfigureOracle(const char *DumpStagesDir, const char *DumpMetricsPath,
                     bool ListPasses);

// VertexOutput is empty for vertex shaders. For pixel shaders it contains the
// decoded output linkage of the SM5.0 vertex producer.
HRESULT Compile(llvm::Module &Module,
                llvm::ArrayRef<SignatureElement> VertexOutput,
                llvm::raw_ostream &Output, llvm::raw_ostream &Diagnostics,
                llvm::ArrayRef<VectorPhiGroup> PhiGroups = {},
                bool NanSafe = false, CompileStats *Stats = nullptr);

} // namespace dxcb5
#endif
