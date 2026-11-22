// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "Emit/Oracle.h"
#include "Mir/Memory/MemoryOpt.h"
#include "Mir/PlacementStore.h"
#include "Mir/SsaAnalysis.h"
#include "llvm/Support/TimeProfiler.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace dxcb5 {

namespace {

enum class PassId : uint8_t {
#define DXCB5_PASS(ENUM, NAME, DIS) ENUM,
#include "Mir/Passes.def"
  Count
};

struct PassDesc {
  const char *Name;
  bool Disableable;
};

constexpr PassDesc kPassDescs[] = {
#define DXCB5_PASS(ENUM, NAME, DIS) {NAME, (DIS) != 0},
#include "Mir/Passes.def"
};

bool IsKnownDisableablePass(const char *Start, size_t Len) {
  for (const auto &D : kPassDescs) {
    if (D.Disableable && std::strlen(D.Name) == Len &&
        std::memcmp(Start, D.Name, Len) == 0)
      return true;
  }
  return false;
}

std::string &DisabledPassListStorage() {
  static std::string List;
  return List;
}

bool IsPassEnabled(const std::string &DisableList, PassId Id) {
  const auto &Desc = kPassDescs[static_cast<size_t>(Id)];
  if (!Desc.Disableable || DisableList.empty())
    return true;
  const char *PassName = Desc.Name;
  const size_t NameLen = std::strlen(PassName);
  const char *P = DisableList.c_str();
  while (*P) {
    while (*P == ',' || *P == ' ')
      ++P;
    const char *Start = P;
    while (*P && *P != ',' && *P != ' ')
      ++P;
    if (static_cast<size_t>(P - Start) == NameLen &&
        std::memcmp(Start, PassName, NameLen) == 0)
      return false;
  }
  return true;
}

} // namespace

bool SetDisabledOptimizationPasses(const char *PassList) {
  if (!PassList || !*PassList) {
    DisabledPassListStorage().clear();
    return true;
  }
  const char *P = PassList;
  while (*P) {
    while (*P == ',' || *P == ' ')
      ++P;
    if (!*P)
      break;
    const char *Start = P;
    while (*P && *P != ',' && *P != ' ')
      ++P;
    const size_t Len = static_cast<size_t>(P - Start);
    if (!IsKnownDisableablePass(Start, Len)) {
      std::fprintf(stderr,
                   "error: unknown or non-disableable DXCB5 pass '%.*s'\n",
                   static_cast<int>(Len), Start);
      DisabledPassListStorage().clear();
      return false;
    }
  }
  DisabledPassListStorage() = PassList;
  return true;
}

std::string GetDisabledOptimizationPasses() {
  return DisabledPassListStorage();
}

void OptimizeSsaMir(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Optimize SSA MIR", llvm::StringRef());
  const std::string &DisableList = DisabledPassListStorage();
  auto RunPass = [&](PassId Id, auto &&Fn) -> bool {
    if (!IsPassEnabled(DisableList, Id))
      return false;
    const bool Changed = Fn();
    OracleRecordPass(kPassDescs[static_cast<size_t>(Id)].Name, Changed, Ssa);
    return Changed;
  };
#ifndef NDEBUG
  {
    std::string Err;
    assert(Ssa.Verify(Err) && "SSA MIR invalid before optimization");
  }
#endif
  // Phase 1: Initial indexable-memory canonicalization.
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
  auto RunCfgCleanup = [&]() -> bool {
    const bool Changed =
        RunPass(PassId::CfgCleanup, [&] { return RunOptCfgCleanup(Ssa); });
    if (!Changed)
      return false;
    DomValid = false;
    NeedXOpt = !Ssa.Mir.IndexableTemps.empty();
    return true;
  };
  auto RunGvnCleanup = [&](bool FoldBranchPreds) -> bool {
    bool C = RunPass(PassId::Gvn, [&] {
      return RunOptGvnDce(Ssa, &EnsureDom(), FoldBranchPreds,
                          /*RunMaskedDce=*/false);
    });
    C |= RunPass(PassId::MaskedDce, [&] { return RunOptMaskedDce(Ssa); });
    return C;
  };

  if (NeedXOpt) {
    RunPass(PassId::IndexableCopy, [&] { return RunOptIndexableCopy(Ssa, EnsureDom()); });
    RunPass(PassId::IndexableSsa, [&] { return RunOptIndexableSsa(Ssa, EnsureDom()); });
  }

  // Phase 2: Fixed-point CFG/GVN + SM5 algebraic/dot/SLP combine loop.
  constexpr unsigned kMaxIterations = 10;
  for (unsigned Iter = 0; Iter < kMaxIterations; ++Iter) {
    bool Changed = false;
    if (NeedXOpt) {
      const bool XChanged = RunPass(PassId::IndexableTemps,
                                    [&] { return RunOptIndexableTemps(Ssa); });
      Changed |= XChanged;
      NeedXOpt = XChanged && !Ssa.Mir.IndexableTemps.empty();
    }
    Changed |= RunPass(PassId::Copies, [&] { return RunOptCopiesAndModifiers(Ssa); });
    Changed |= RunPass(PassId::LaneConst,
                       [&] { return RunOptLaneConstProp(Ssa, EnsureDom()); });
    Changed |= RunCfgCleanup();
    if (RunPass(PassId::IfConvert, [&] { return RunOptIfConvert(Ssa); })) {
      Changed = true;
      DomValid = false;
      NeedXOpt = !Ssa.Mir.IndexableTemps.empty();
    }
    Changed |= RunGvnCleanup(/*FoldBranchPreds=*/true);
    const bool DotsChanged =
        RunPass(PassId::Dots, [&] { return RunOptSelectDots(Ssa); });
    const bool AlgChanged =
        RunPass(PassId::Algebraic, [&] { return RunOptAlgebraic(Ssa); });
    if (DotsChanged || AlgChanged) {
      Changed = true;
      if (AlgChanged)
        Changed |= RunPass(PassId::Copies, [&] { return RunOptCopiesAndModifiers(Ssa); });
      Changed |= RunGvnCleanup(/*FoldBranchPreds=*/true);
    }
    Changed |= RunPass(PassId::Slp, [&] { return RunOptSlpVectorize(Ssa); });
    if (!Changed)
      break;
  }
  // Always fold any constant branches created on the final iteration so no
  // `IF <imm>` leaks past Phase 2 (even when `if-convert` is disabled).
  RunCfgCleanup();

  // Phase 3: Post-loop indexable packing, DP2 selection, and cleanup.
  if (Ssa.Mir.IndexableTemps.size() >= 2 &&
      RunPass(PassId::IndexableCopy,
              [&] { return RunOptIndexableCopy(Ssa, EnsureDom()); })) {
    RunPass(PassId::IndexableSsa, [&] { return RunOptIndexableSsa(Ssa, EnsureDom()); });
    RunPass(PassId::IndexableTemps, [&] { return RunOptIndexableTemps(Ssa); });
  }

  if (RunPass(PassId::IndexablePack, [&] { return RunOptIndexablePack(Ssa); })) {
    RunPass(PassId::Copies, [&] { return RunOptCopiesAndModifiers(Ssa); });
    RunPass(PassId::Slp, [&] { return RunOptSlpVectorize(Ssa); });
    RunPass(PassId::Dots, [&] { return RunOptSelectDots(Ssa); });
  }
  RunPass(PassId::Copies, [&] { return RunOptCopiesAndModifiers(Ssa); });
  RunGvnCleanup(/*FoldBranchPreds=*/false);
  if (RunPass(PassId::Dp2, [&] { return RunOptSelectDp2(Ssa); }))
    RunGvnCleanup(/*FoldBranchPreds=*/false);

  // Phase 4: Loop peeling, LICM, cold-branch sinking, output folding, pressure.
  SsaLoopInfo Loops;
  bool LoopsValid = false;
  auto EnsureLoops = [&]() -> const SsaLoopInfo & {
    if (!LoopsValid) {
      ComputeLoopInfo(Ssa, EnsureDom(), Loops);
      LoopsValid = true;
    }
    return Loops;
  };
  if (RunPass(PassId::LoopPeel,
              [&] { return RunOptLoopPeel(Ssa, EnsureDom(), &EnsureLoops()); })) {
    RunPass(PassId::Copies, [&] { return RunOptCopiesAndModifiers(Ssa); });
    RunGvnCleanup(/*FoldBranchPreds=*/false);
  }
  if (RunPass(PassId::GuardDiscard, [&] { return RunOptGuardDiscard(Ssa); })) {
    DomValid = false;
    LoopsValid = false;
  }
  PlacementStore Placement;
  bool Moved = RunPass(PassId::Licm, [&] {
    return RunOptLicm(Ssa, EnsureDom(), &EnsureLoops(), &Placement);
  });
  Moved |= RunPass(PassId::ColdSink, [&] {
    return RunOptColdSink(Ssa, EnsureDom(), &EnsureLoops(), &Placement);
  });
  if (Moved)
    RunPass(PassId::Copies, [&] { return RunOptCopiesAndModifiers(Ssa); });
  if (RunPass(PassId::FoldOutputs, [&] { return RunOptFoldOutputs(Ssa); }))
    RunGvnCleanup(/*FoldBranchPreds=*/false);
  if (RunPass(PassId::PressureShrink, [&] {
        return RunOptPressureShrink(Ssa, EnsureDom(), &EnsureLoops(), &Placement);
      })) {
    RunPass(PassId::Copies, [&] { return RunOptCopiesAndModifiers(Ssa); });
    RunPass(PassId::MaskedDce, [&] { return RunOptMaskedDce(Ssa); });
  }
  RunCfgCleanup();
#ifndef NDEBUG
  {
    std::string Err;
    assert(Ssa.Verify(Err, /*RequireNormalizedCfg=*/true) &&
           "SSA MIR invalid or non-normalized after optimization");
  }
#endif
}

} // namespace dxcb5
