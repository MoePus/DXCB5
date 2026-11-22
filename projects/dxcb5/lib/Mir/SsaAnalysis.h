// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_SSA_ANALYSIS_H
#define DXCB5_MIR_SSA_ANALYSIS_H

#include "Mir/SsaMir.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <vector>

namespace dxcb5 {

struct NaturalLoop {
  uint32_t Header = UINT32_MAX;
  uint32_t Preheader = UINT32_MAX;
  std::vector<uint8_t> InLoop;
  llvm::SmallVector<uint32_t, 8> Blocks;
  llvm::SmallVector<uint32_t, 2> Latches;
};

struct SsaLoopInfo {
  std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
  std::vector<uint32_t> LoopDepth;
  llvm::SmallVector<NaturalLoop, 8> Loops;
};

uint32_t DomLca(const SsaDomTree &Dom, uint32_t A, uint32_t B);
void BuildReachablePreds(
    const SsaProgram &Ssa, const SsaDomTree &Dom,
    std::vector<llvm::SmallVector<uint32_t, 4>> &ReachPreds);
void ComputeLoopInfo(const SsaProgram &Ssa, const SsaDomTree &Dom,
                     SsaLoopInfo &Out);
bool IsSinkableQuadOp(const SsaInst &Inst);

inline bool IsMovablePureOrQuadInst(const SsaInst &Inst) {
  return Inst.IsPure() || IsSinkableQuadOp(Inst);
}

inline bool EdgeDominatesChild(
    const SsaDomTree &Dom,
    const std::vector<llvm::SmallVector<uint32_t, 4>> &ReachPreds,
    uint32_t Parent, uint32_t Child) {
  bool SawParent = false;
  for (uint32_t P : ReachPreds[Child]) {
    if (P == Parent)
      SawParent = true;
    else if (!Dom.Dominates(Child, P))
      return false;
  }
  return SawParent;
}

} // namespace dxcb5

#endif
