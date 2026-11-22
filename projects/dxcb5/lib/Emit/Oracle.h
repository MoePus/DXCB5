// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_EMIT_ORACLE_H
#define DXCB5_EMIT_ORACLE_H

#include "dxcb5/Compiler.h"
#include "dxcb5/ControlFlow.h"
#include "Mir/SsaMir.h"
#include "llvm/ADT/ArrayRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dxcb5 {

// Deterministic integer dynamic cost metric:
//   mu = sum_{inst in reachable blocks} 8^(min(LoopDepth[block], 6))
uint64_t ComputeDynamicCost(const SsaProgram &Ssa);

struct OraclePassStat {
  std::string Name;
  uint32_t Calls = 0;
  uint32_t Changes = 0;
};

struct OracleStageRecord {
  uint32_t Seq = 0;
  std::string Name;
  std::string Sha256;
};

// Scoped oracle session bound to the current compiling thread. When no oracle
// flags are configured via ConfigureOracle(), Active() is false and all hooks
// are single-branch no-ops.
class OracleSession {
public:
  OracleSession();
  ~OracleSession();

  OracleSession(const OracleSession &) = delete;
  OracleSession &operator=(const OracleSession &) = delete;

  bool Active() const { return IsActive; }

  void RecordSsaStage(const char *Name, const SsaProgram &Ssa);
  void RecordPass(const char *Name, bool Changed, const SsaProgram &Ssa);
  void RecordFlowStage(const char *Name, llvm::ArrayRef<FlowBlock> Blocks);
  void RecordMirStage(const char *Name, const MirProgram &Program);
  void Finish(const CompileStats &Stats, llvm::ArrayRef<uint8_t> Dxbc);

  static OracleSession *Current();

private:
  void EmitStageText(const char *Name, const std::string &Text);

  OracleSession *Prev = nullptr;
  bool IsActive = false;
  bool DumpStages = false;
  bool DumpMetrics = false;
  bool ListPasses = false;
  std::string StagesTarget;
  std::string MetricsTarget;
  uint32_t NextSeq = 0;
  std::vector<OraclePassStat> Passes;
  std::vector<OracleStageRecord> Stages;
};

inline void OracleRecordPass(const char *Name, bool Changed,
                             const SsaProgram &Ssa) {
  if (OracleSession *S = OracleSession::Current())
    S->RecordPass(Name, Changed, Ssa);
}

} // namespace dxcb5

#endif
