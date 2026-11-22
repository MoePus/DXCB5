// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_OPT_PIPELINE_H
#define DXCB5_MIR_OPT_PIPELINE_H

#include "Mir/SsaMir.h"

namespace dxcb5 {

struct SsaLoopInfo;
class PlacementStore;

// Phase 1: Initial indexable-memory canonicalization (in MemoryOpt.h):
//   RunOptIndexableCopy, RunOptIndexableSsa.

// Phase 2: Bounded fixed-point (<= 10 iterations) CFG/GVN + SM5 combines:
//   Only RunOptCfgCleanup and RunOptIfConvert mutate the CFG (invalidating
//   dominator trees). RunOptCfgCleanup always runs independent of if-convert.
bool RunOptCfgCleanup(SsaProgram &Ssa);
bool RunOptIfConvert(SsaProgram &Ssa);
bool RunOptLaneConstProp(SsaProgram &Ssa, const SsaDomTree &Dom,
                         bool FoldBranches = true);
bool RunOptGvnDce(SsaProgram &Ssa, const SsaDomTree *CachedDom = nullptr,
                  bool FoldBranchPreds = true, bool RunMaskedDce = true);
bool RunOptMaskedDce(SsaProgram &Ssa);
bool RunOptCopiesAndModifiers(SsaProgram &Ssa);
bool RunOptAlgebraic(SsaProgram &Ssa);
bool RunOptSelectDots(SsaProgram &Ssa);
bool RunOptSlpVectorize(SsaProgram &Ssa);

// Phase 3: Post-loop indexable packing, DP2 selection, and cleanup.
bool RunOptSelectDp2(SsaProgram &Ssa);

// Phase 4: Loop peeling, uniform discard guard, LICM, cold-branch sinking, output folding, pressure.
bool RunOptLoopPeel(SsaProgram &Ssa, const SsaDomTree &Dom,
                    const SsaLoopInfo *CachedLoops = nullptr);
bool RunOptGuardDiscard(SsaProgram &Ssa);
bool RunOptLicm(SsaProgram &Ssa, const SsaDomTree &Dom,
                const SsaLoopInfo *CachedLoops = nullptr,
                PlacementStore *Placement = nullptr);
bool RunOptColdSink(SsaProgram &Ssa, const SsaDomTree &Dom,
                    const SsaLoopInfo *CachedLoops = nullptr,
                    PlacementStore *Placement = nullptr);
bool RunOptFoldOutputs(SsaProgram &Ssa);
bool RunOptPressureShrink(SsaProgram &Ssa, const SsaDomTree &Dom,
                          const SsaLoopInfo *CachedLoops = nullptr,
                          PlacementStore *Placement = nullptr);

// Runs the 4-phase SSA MIR optimization pipeline (Phase 2 bounded to 10 iters).
void OptimizeSsaMir(SsaProgram &Ssa);

// Test-only pass-ablation control (invoked explicitly via dxcb5 CLI
// `--disable-pass <name>`; never driven by ambient environment variables).
// Returns false if any requested pass name is unknown or non-disableable.
bool SetDisabledOptimizationPasses(const char *PassList);
std::string GetDisabledOptimizationPasses();

} // namespace dxcb5

#endif
