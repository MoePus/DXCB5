// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "llvm/Support/TimeProfiler.h"
#include <cassert>

namespace dxcb5 {

void OptimizeSsaMir(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Optimize SSA MIR", llvm::StringRef());
#ifndef NDEBUG
  {
    std::string Err;
    assert(Ssa.Verify(Err) && "SSA MIR invalid before optimization");
  }
#endif
  RunOptPromoteLocalArrays(Ssa);

  SsaDomTree CachedDom;
  bool DomValid = false;
  auto EnsureDom = [&]() -> const SsaDomTree & {
    if (!DomValid) {
      Ssa.ComputeDominators(CachedDom);
      DomValid = true;
    }
    return CachedDom;
  };

  bool NeedXOpt = !Ssa.Mir.IndexableTemps.empty();
  if (NeedXOpt) {
    RunOptIndexableCopy(Ssa, EnsureDom());
    RunOptIndexableSsa(Ssa, EnsureDom());
  }
  constexpr unsigned kMaxIterations = 10;
  for (unsigned Iter = 0; Iter < kMaxIterations; ++Iter) {
    bool Changed = false;
    if (NeedXOpt) {
      const bool XChanged = RunOptIndexableTemps(Ssa);
      Changed |= XChanged;
      NeedXOpt = XChanged && !Ssa.Mir.IndexableTemps.empty();
    }
    Changed |= RunOptCopiesAndModifiers(Ssa);
    if (RunOptIfConvert(Ssa)) {
      Changed = true;
      DomValid = false;
      NeedXOpt = !Ssa.Mir.IndexableTemps.empty();
    }
    Changed |= RunOptGvnDce(Ssa, &EnsureDom());
    const bool DotsChanged = RunOptSelectDots(Ssa);
    const bool AlgChanged = RunOptAlgebraic(Ssa);
    if (DotsChanged || AlgChanged) {
      Changed = true;
      Changed |= RunOptGvnDce(Ssa, &EnsureDom());
    }
    Changed |= RunOptSlpVectorize(Ssa);
    if (!Changed)
      break;
  }

  if (Ssa.Mir.IndexableTemps.size() >= 2 &&
      RunOptIndexableCopy(Ssa, EnsureDom()))
    RunOptIndexableTemps(Ssa);

  if (RunOptIndexablePack(Ssa)) {
    RunOptCopiesAndModifiers(Ssa);
    RunOptSlpVectorize(Ssa);
  }
  RunOptCopiesAndModifiers(Ssa);
  RunOptGvnDce(Ssa, &EnsureDom());
  if (RunOptSelectDp2(Ssa))
    RunOptGvnDce(Ssa, &EnsureDom());
  if (RunOptFoldOutputs(Ssa))
    RunOptGvnDce(Ssa, &EnsureDom());
#ifndef NDEBUG
  {
    std::string Err;
    assert(Ssa.Verify(Err) && "SSA MIR invalid after optimization");
  }
#endif
}

} // namespace dxcb5
