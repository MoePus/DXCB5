// Distributed under the University of Illinois Open Source License.
#include "Mir/SsaAnalysis.h"
#include "dxcb5/InstructionInfo.h"

namespace dxcb5 {
namespace {

NaturalLoop BuildNaturalLoop(
    const SsaProgram &Ssa, const SsaDomTree &Dom,
    const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds, uint32_t H,
    llvm::ArrayRef<uint32_t> Latches, std::vector<uint32_t> &LoopDepth) {
  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  NaturalLoop L;
  L.Header = H;
  L.Latches.append(Latches.begin(), Latches.end());
  L.InLoop.assign(N, 0);
  L.InLoop[H] = 1;
  L.Blocks.push_back(H);
  llvm::SmallVector<uint32_t, 16> Work;
  for (uint32_t Latch : Latches) {
    if (!L.InLoop[Latch]) {
      L.InLoop[Latch] = 1;
      L.Blocks.push_back(Latch);
      Work.push_back(Latch);
    }
  }
  while (!Work.empty()) {
    const uint32_t Cur = Work.pop_back_val();
    for (uint32_t Pred : Preds[Cur]) {
      if (Pred >= N || !Dom.Reachable(Pred) || L.InLoop[Pred])
        continue;
      L.InLoop[Pred] = 1;
      L.Blocks.push_back(Pred);
      Work.push_back(Pred);
    }
  }
  for (uint32_t B : L.Blocks)
    ++LoopDepth[B];

  uint32_t ExtPred = UINT32_MAX;
  unsigned ExtCount = 0;
  for (uint32_t Pred : Preds[H]) {
    if (Pred < N && Dom.Reachable(Pred) && !L.InLoop[Pred]) {
      ExtPred = Pred;
      ++ExtCount;
    }
  }
  if (ExtCount == 1 && Ssa.Blocks[ExtPred].Kind == FlowKind::Jump &&
      Ssa.Blocks[ExtPred].Successors.size() == 1 &&
      Ssa.Blocks[ExtPred].Successors[0] == H)
    L.Preheader = ExtPred;
  return L;
}

} // namespace

uint32_t DomLca(const SsaDomTree &Dom, uint32_t A, uint32_t B) {
  if (A == UINT32_MAX)
    return B;
  if (B == UINT32_MAX)
    return A;
  while (A != B) {
    if (Dom.RpoNumber[A] > Dom.RpoNumber[B])
      A = Dom.IDom[A];
    else
      B = Dom.IDom[B];
    if (A == UINT32_MAX || B == UINT32_MAX)
      return UINT32_MAX;
  }
  return A;
}

void BuildReachablePreds(
    const SsaProgram &Ssa, const SsaDomTree &Dom,
    std::vector<llvm::SmallVector<uint32_t, 4>> &ReachPreds) {
  ReachPreds.assign(Ssa.Blocks.size(), {});
  for (uint32_t P = 0; P < Ssa.Blocks.size(); ++P) {
    if (!Dom.Reachable(P))
      continue;
    for (uint32_t Succ : Ssa.Blocks[P].Successors)
      if (Succ < Ssa.Blocks.size())
        ReachPreds[Succ].push_back(P);
  }
}

void ComputeLoopInfo(const SsaProgram &Ssa, const SsaDomTree &Dom,
                     SsaLoopInfo &Out) {
  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  Ssa.BuildPredecessors(Out.Preds);
  Out.LoopDepth.assign(N, 0);
  Out.Loops.clear();
  std::vector<llvm::SmallVector<uint32_t, 2>> LatchesByHeader(N);
  for (uint32_t P = 0; P < N; ++P) {
    if (!Dom.Reachable(P))
      continue;
    for (uint32_t Succ : Ssa.Blocks[P].Successors)
      if (Succ < N && Dom.Dominates(Succ, P))
        LatchesByHeader[Succ].push_back(P);
  }
  for (uint32_t H = 0; H < N; ++H) {
    if (LatchesByHeader[H].empty())
      continue;
    Out.Loops.push_back(BuildNaturalLoop(Ssa, Dom, Out.Preds, H,
                                         LatchesByHeader[H], Out.LoopDepth));
  }
}

bool IsSinkableQuadOp(const SsaInst &Inst) {
  if (!InstructionCanMoveAcrossUniformBranch(Inst.Opcode) ||
      InstructionCanMoveAcrossDivergentBranch(Inst.Opcode))
    return false;
  if (Inst.Dest.Kind != OperandKind::Temp || Inst.HasAuxDest())
    return false;
  for (const auto &Src : Inst.Sources)
    if (Src.Kind == OperandKind::IndexableTemp || Src.Kind == OperandKind::Uav)
      return false;
  return true;
}

} // namespace dxcb5
