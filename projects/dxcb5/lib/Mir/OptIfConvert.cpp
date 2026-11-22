// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>

using namespace dxcb5;

namespace {

constexpr unsigned kMaxDiamondCost = 8;
constexpr unsigned kMaxDiamondPhis = 6;

bool IsSpeculatableOpcode(uint32_t Opcode) {
  if (Opcode == PseudoCombineVec || InstructionIsCompare(Opcode))
    return true;
  switch (Opcode) {
  case D3D10_SB_OPCODE_MOV:  case D3D10_SB_OPCODE_MOVC:
  case D3D10_SB_OPCODE_ADD:  case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_MAD:  case D3D10_SB_OPCODE_MIN:
  case D3D10_SB_OPCODE_MAX:  case D3D10_SB_OPCODE_FRC:
  case D3D10_SB_OPCODE_DP2:  case D3D10_SB_OPCODE_DP3:
  case D3D10_SB_OPCODE_DP4:  case D3D10_SB_OPCODE_IADD:
  case D3D10_SB_OPCODE_IMAD: case D3D10_SB_OPCODE_INEG:
  case D3D10_SB_OPCODE_IMIN: case D3D10_SB_OPCODE_IMAX:
  case D3D10_SB_OPCODE_UMIN: case D3D10_SB_OPCODE_UMAX:
  case D3D10_SB_OPCODE_UMAD: case D3D10_SB_OPCODE_ISHL:
  case D3D10_SB_OPCODE_ISHR: case D3D10_SB_OPCODE_USHR:
  case D3D10_SB_OPCODE_AND:  case D3D10_SB_OPCODE_OR:
  case D3D10_SB_OPCODE_XOR:  case D3D10_SB_OPCODE_NOT:
  case D3D10_SB_OPCODE_FTOI: case D3D10_SB_OPCODE_FTOU:
  case D3D10_SB_OPCODE_ITOF: case D3D10_SB_OPCODE_UTOF:
  case D3D11_SB_OPCODE_F16TOF32: case D3D11_SB_OPCODE_F32TOF16:
  case D3D11_SB_OPCODE_BFREV:    case D3D11_SB_OPCODE_COUNTBITS:
  case D3D11_SB_OPCODE_BFI:      case D3D11_SB_OPCODE_IBFE:
  case D3D11_SB_OPCODE_UBFE:
  case D3D11_SB_OPCODE_FIRSTBIT_LO: case D3D11_SB_OPCODE_FIRSTBIT_HI:
  case D3D11_SB_OPCODE_FIRSTBIT_SHI:
  case D3D10_SB_OPCODE_ROUND_NE: case D3D10_SB_OPCODE_ROUND_NI:
  case D3D10_SB_OPCODE_ROUND_PI: case D3D10_SB_OPCODE_ROUND_Z:
    return true;
  default:
    return false;
  }
}

bool IsCompareSelectOnlyBlock(const SsaBlock &Block) {
  for (const auto &Inst : Block.Insts) {
    if (Inst.Dead || Inst.Opcode == PseudoCombineVec)
      continue;
    if (Inst.Opcode != D3D10_SB_OPCODE_MOV &&
        Inst.Opcode != D3D10_SB_OPCODE_MOVC &&
        !InstructionIsCompare(Inst.Opcode))
      return false;
  }
  return true;
}

bool IsSpeculatableBlock(const SsaBlock &Block, uint32_t ExpectedPred,
                         const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds,
                         uint32_t BlockId, unsigned &OutCost) {
  if (BlockId == 0 || Preds[BlockId].size() != 1 ||
      Preds[BlockId][0] != ExpectedPred || Block.Kind != FlowKind::Jump ||
      Block.Successors.size() != 1)
    return false;
  for (const auto &Phi : Block.Phis)
    if (!Phi.Dead)
      return false;
  unsigned Cost = 0;
  for (const auto &Inst : Block.Insts) {
    if (Inst.Dead)
      continue;
    if (!IsSpeculatableOpcode(Inst.Opcode) ||
        Inst.Dest.Kind != OperandKind::Temp ||
        Inst.AuxDest.Kind != OperandKind::Null ||
        Inst.Dest.RelativeRegister != UINT32_MAX)
      return false;
    for (const auto &Src : Inst.Sources) {
      if (Src.RelativeRegister != UINT32_MAX ||
          Src.Kind == OperandKind::ImmediateConstantBuffer)
        return false;
    }
    if (Inst.Opcode != PseudoCombineVec) {
      ++Cost;
    } else if (std::any_of(Inst.Sources.begin(), Inst.Sources.end(),
                           [](const Operand &S) {
                             return S.Kind != OperandKind::Temp;
                           })) {
      ++Cost;
    }
  }
  OutCost = Cost;
  return true;
}

bool FindPhiInput(const SsaPhi &Phi, uint32_t Pred, Operand &Out) {
  for (const auto &In : Phi.Inputs) {
    if (In.PredBlock == Pred) {
      Out = In.Value;
      return true;
    }
  }
  return false;
}

bool NeedsEdgeCopy(const SsaProgram &Ssa, const Operand &Val, uint32_t FromBlk) {
  if (Val.Kind != OperandKind::Temp)
    return true;
  for (const auto &Inst : Ssa.Blocks[FromBlk].Insts) {
    if (!Inst.Dead && Inst.Dest.Kind == OperandKind::Temp &&
        Inst.Dest.Index == Val.Index)
      return false;
  }
  return true;
}

void ClearMergedBlock(SsaBlock &Blk) {
  Blk.Phis.clear();
  Blk.Insts.clear();
  Blk.Successors.clear();
  Blk.Kind = FlowKind::Return;
}

void HoistBlockInsts(SsaBlock &From, SsaBlock &Into) {
  for (auto &Inst : From.Insts)
    if (!Inst.Dead)
      Into.Insts.push_back(std::move(Inst));
  ClearMergedBlock(From);
}

void ReplacePhiInputs(SsaPhi &Phi, uint32_t TruePred, uint32_t FalsePred,
                      uint32_t NewPred, const Operand &NewVal) {
  llvm::SmallVector<SsaPhi::Incoming, 4> NextInputs;
  for (const auto &In : Phi.Inputs)
    if (In.PredBlock != TruePred && In.PredBlock != FalsePred)
      NextInputs.push_back(In);
  NextInputs.push_back({NewPred, NewVal});
  Phi.Inputs = std::move(NextInputs);
}

struct IfConvShape {
  bool IsDiamond = false;
  uint32_t T = 0, F = 0, Join = UINT32_MAX;
  uint32_t TruePred = 0, FalsePred = 0;
  unsigned CostT = 0, CostF = 0, TotalCost = 0;
};

bool MatchIfConvShape(const SsaProgram &Ssa, uint32_t H,
                      const std::vector<llvm::SmallVector<uint32_t, 4>> &Preds,
                      IfConvShape &S) {
  const auto &Head = Ssa.Blocks[H];
  if (Head.Kind != FlowKind::Branch || Head.Successors.size() != 2 ||
      Head.StrictBranch ||
      Head.Condition.Kind == OperandKind::Null ||
      Head.Condition.Modifier != 0 ||
      Head.Condition.RelativeRegister != UINT32_MAX)
    return false;
  S.T = Head.Successors[0];
  S.F = Head.Successors[1];
  if (S.T == S.F || S.T == H || S.F == H)
    return false;
  const bool SpecT = IsSpeculatableBlock(Ssa.Blocks[S.T], H, Preds, S.T, S.CostT);
  const bool SpecF = IsSpeculatableBlock(Ssa.Blocks[S.F], H, Preds, S.F, S.CostF);
  if (SpecT && SpecF && Ssa.Blocks[S.T].Successors[0] == Ssa.Blocks[S.F].Successors[0] &&
      Ssa.Blocks[S.T].Successors[0] != H) {
    S.IsDiamond = true;
    S.Join = Ssa.Blocks[S.T].Successors[0];
    S.TruePred = S.T;
    S.FalsePred = S.F;
    S.TotalCost = S.CostT + S.CostF;
  } else if (SpecT && Ssa.Blocks[S.T].Successors[0] == S.F) {
    S.IsDiamond = false;
    S.Join = S.F;
    S.TruePred = S.T;
    S.FalsePred = H;
    S.TotalCost = S.CostT;
  } else if (SpecF && Ssa.Blocks[S.F].Successors[0] == S.T) {
    S.IsDiamond = false;
    S.Join = S.T;
    S.TruePred = H;
    S.FalsePred = S.F;
    S.TotalCost = S.CostF;
  } else {
    return false;
  }
  if (S.Join == 0 || S.Join == H)
    return false;
  if (!S.IsDiamond)
    return S.TotalCost <= 7;
  const bool PureCmpSel = IsCompareSelectOnlyBlock(Ssa.Blocks[S.T]) &&
                          IsCompareSelectOnlyBlock(Ssa.Blocks[S.F]);
  const unsigned MaxCost = PureCmpSel ? 12u : kMaxDiamondCost;
  if (S.TotalCost > MaxCost ||
      (!PureCmpSel && std::min(S.CostT, S.CostF) > 1 && S.TotalCost > 6))
    return false;
  return std::find(Preds[S.Join].begin(), Preds[S.Join].end(), H) ==
         Preds[S.Join].end();
}

bool AreJoinPhisProfitable(const SsaProgram &Ssa, const IfConvShape &S,
                           size_t JoinPredCount) {
  unsigned DiffPhis = 0, EdgeCopiesBefore = 0;
  for (const auto &Phi : Ssa.Blocks[S.Join].Phis) {
    if (Phi.Dead)
      continue;
    Operand VT{OperandKind::Null}, VF{OperandKind::Null};
    if (!FindPhiInput(Phi, S.TruePred, VT) || !FindPhiInput(Phi, S.FalsePred, VF))
      return false;
    if (VT.RelativeRegister != UINT32_MAX || VF.RelativeRegister != UINT32_MAX ||
        ((VT.Modifier || VF.Modifier) && VT.SameValue(VF) && JoinPredCount > 2))
      return false;
    if (!VT.SameValue(VF)) {
      ++DiffPhis;
      EdgeCopiesBefore += NeedsEdgeCopy(Ssa, VT, S.TruePred) ? 1u : 0u;
      EdgeCopiesBefore += NeedsEdgeCopy(Ssa, VF, S.FalsePred) ? 1u : 0u;
    }
  }
  if ((S.IsDiamond && DiffPhis > kMaxDiamondPhis) ||
      (!S.IsDiamond && DiffPhis > (S.TotalCost > 4 ? 1u : 3u)))
    return false;
  const unsigned CfBefore = S.IsDiamond ? 3u : 2u;
  return DiffPhis < CfBefore + EdgeCopiesBefore;
}

void EmitSimpleMov(SsaBlock &Head, const Operand &Dst, const Operand &Src,
                   bool Precise) {
  SsaInst Mov;
  Mov.Opcode = D3D10_SB_OPCODE_MOV;
  Mov.Dest = Dst;
  Mov.Sources.push_back(Src);
  Mov.Precise = Precise;
  Head.Insts.push_back(std::move(Mov));
}

void RewriteJoinPhis(SsaProgram &Ssa, uint32_t H, const IfConvShape &S,
                     const Operand &Cond, bool JoinOnlyTwoPreds) {
  auto &Head = Ssa.Blocks[H];
  for (auto &Phi : Ssa.Blocks[S.Join].Phis) {
    if (Phi.Dead)
      continue;
    Operand VT;
    Operand VF;
    VT.Kind = OperandKind::Null;
    VF.Kind = OperandKind::Null;
    FindPhiInput(Phi, S.TruePred, VT);
    FindPhiInput(Phi, S.FalsePred, VF);
    const uint32_t DstId = JoinOnlyTwoPreds ? Phi.Result : Ssa.AllocValue();
    uint8_t DstSwz = kIdentitySwizzle;
    if (PopcountMask(Phi.Mask) == 1) {
      DstSwz = BroadcastLaneSwizzle(
          static_cast<unsigned>(llvm::countTrailingZeros(Phi.Mask)));
    }
    const Operand Dst = MakeSsaTemp(DstId, Phi.Mask, DstSwz);
    if (VT.SameValue(VF)) {
      if (JoinOnlyTwoPreds) {
        EmitSimpleMov(Head, Dst, VT, Phi.Precise);
        Phi.Dead = true;
      } else {
        ReplacePhiInputs(Phi, S.TruePred, S.FalsePred, H, VT);
      }
      continue;
    }
    SsaInst Sel;
    Sel.Opcode = D3D10_SB_OPCODE_MOVC;
    Sel.Dest = Dst;
    Sel.Sources.assign({Cond, VT, VF});
    Sel.Precise = Phi.Precise;
    Head.Insts.push_back(std::move(Sel));
    if (JoinOnlyTwoPreds)
      Phi.Dead = true;
    else
      ReplacePhiInputs(Phi, S.TruePred, S.FalsePred, H, Dst);
  }
}

void MergeSoleSuccessorJoin(SsaProgram &Ssa, uint32_t H, uint32_t Join) {
  auto &Head = Ssa.Blocks[H];
  auto &JoinBlk = Ssa.Blocks[Join];
  for (auto &Inst : JoinBlk.Insts)
    if (!Inst.Dead)
      Head.Insts.push_back(std::move(Inst));
  Head.Kind = JoinBlk.Kind;
  Head.Condition = JoinBlk.Condition;
  Head.Successors = JoinBlk.Successors;
  Head.CaseValues = JoinBlk.CaseValues;
  Head.StrictBranch = JoinBlk.StrictBranch;
  Head.UniformBranch = JoinBlk.UniformBranch;
  for (uint32_t Succ : Head.Successors) {
    if (Succ >= Ssa.Blocks.size())
      continue;
    for (auto &Phi : Ssa.Blocks[Succ].Phis) {
      if (Phi.Dead)
        continue;
      for (auto &In : Phi.Inputs)
        if (In.PredBlock == Join)
          In.PredBlock = H;
    }
  }
  ClearMergedBlock(JoinBlk);
}

bool CompactReachableBlocks(SsaProgram &Ssa) {
  if (Ssa.Blocks.empty())
    return false;
  const uint32_t N = static_cast<uint32_t>(Ssa.Blocks.size());
  std::vector<uint8_t> Reachable(N, 0);
  llvm::SmallVector<uint32_t, 64> Stack{0};
  Reachable[0] = 1;
  while (!Stack.empty()) {
    const uint32_t B = Stack.pop_back_val();
    for (uint32_t Succ : Ssa.Blocks[B].Successors) {
      if (Succ < N && !Reachable[Succ]) {
        Reachable[Succ] = 1;
        Stack.push_back(Succ);
      }
    }
  }
  std::vector<uint32_t> OldToNew(N, UINT32_MAX);
  uint32_t NewCount = 0;
  for (uint32_t B = 0; B < N; ++B)
    if (Reachable[B])
      OldToNew[B] = NewCount++;
  if (NewCount == N)
    return false;

  std::vector<SsaBlock> Compacted;
  Compacted.reserve(NewCount);
  for (uint32_t B = 0; B < N; ++B) {
    if (!Reachable[B])
      continue;
    SsaBlock Blk = std::move(Ssa.Blocks[B]);
    for (uint32_t &Succ : Blk.Successors)
      Succ = OldToNew[Succ];
    for (auto &Phi : Blk.Phis) {
      if (Phi.Dead)
        continue;
      llvm::SmallVector<SsaPhi::Incoming, 4> LiveInputs;
      for (const auto &In : Phi.Inputs)
        if (In.PredBlock < N && Reachable[In.PredBlock])
          LiveInputs.push_back({OldToNew[In.PredBlock], In.Value});
      Phi.Inputs = std::move(LiveInputs);
    }
    Compacted.push_back(std::move(Blk));
  }
  Ssa.Blocks = std::move(Compacted);
  return true;
}

void UpdatePredsAfterConvert(
    const SsaProgram &Ssa, uint32_t H, const IfConvShape &S,
    bool JoinOnlyTwoPreds,
    std::vector<llvm::SmallVector<uint32_t, 4>> &Preds) {
  if (S.IsDiamond) {
    Preds[S.T].clear();
    Preds[S.F].clear();
  } else {
    Preds[S.TruePred == S.T ? S.T : S.F].clear();
  }
  if (JoinOnlyTwoPreds) {
    Preds[S.Join].clear();
    for (uint32_t Succ : Ssa.Blocks[H].Successors) {
      if (Succ >= Preds.size())
        continue;
      for (uint32_t &P : Preds[Succ])
        if (P == S.Join)
          P = H;
    }
    return;
  }
  auto &JP = Preds[S.Join];
  JP.erase(std::remove_if(JP.begin(), JP.end(),
                          [&](uint32_t P) {
                            return P == S.TruePred || P == S.FalsePred;
                          }),
           JP.end());
  JP.push_back(H);
}

bool TryIfConvertBlock(SsaProgram &Ssa, uint32_t H,
                       std::vector<llvm::SmallVector<uint32_t, 4>> &Preds,
                       bool &MergedJoin) {
  IfConvShape S;
  if (!MatchIfConvShape(Ssa, H, Preds, S) ||
      !AreJoinPhisProfitable(Ssa, S, Preds[S.Join].size()))
    return false;
  auto &Head = Ssa.Blocks[H];
  if (S.IsDiamond) {
    HoistBlockInsts(Ssa.Blocks[S.T], Head);
    HoistBlockInsts(Ssa.Blocks[S.F], Head);
  } else {
    HoistBlockInsts(Ssa.Blocks[S.TruePred == S.T ? S.T : S.F], Head);
  }
  Operand Cond = Head.Condition;
  if (Cond.Kind != OperandKind::Immediate)
    Cond.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(Cond.Swizzle, 0));
  MergedJoin = Preds[S.Join].size() == 2;
  RewriteJoinPhis(Ssa, H, S, Cond, MergedJoin);
  Head.Kind = FlowKind::Jump;
  Head.Condition = Operand{OperandKind::Null};
  Head.Successors = {S.Join};
  if (MergedJoin)
    MergeSoleSuccessorJoin(Ssa, H, S.Join);
  UpdatePredsAfterConvert(Ssa, H, S, MergedJoin, Preds);
  return true;
}

void RemovePhiPredInput(SsaBlock &Blk, uint32_t DeadPred) {
  for (auto &Phi : Blk.Phis) {
    if (Phi.Dead)
      continue;
    llvm::SmallVector<SsaPhi::Incoming, 4> Keep;
    for (const auto &In : Phi.Inputs)
      if (In.PredBlock != DeadPred)
        Keep.push_back(In);
    Phi.Inputs = std::move(Keep);
  }
}

bool TryFoldConstantBranches(SsaProgram &Ssa) {
  bool Changed = false;
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Blk = Ssa.Blocks[B];
    if (Blk.Condition.Kind != OperandKind::Immediate || Blk.Condition.Modifier ||
        Blk.Condition.RelativeRegister != UINT32_MAX)
      continue;
    if (Blk.Kind == FlowKind::Branch && Blk.Successors.size() == 2) {
      const uint32_t Taken = Blk.Successors[Blk.Condition.Index != 0u ? 0 : 1];
      const uint32_t Dead = Blk.Successors[Blk.Condition.Index != 0u ? 1 : 0];
      Blk.Kind = FlowKind::Jump;
      Blk.Condition = Operand{OperandKind::Null};
      Blk.Successors = {Taken};
      if (Dead != Taken && Dead < Ssa.Blocks.size())
        RemovePhiPredInput(Ssa.Blocks[Dead], B);
      Changed = true;
    } else if (Blk.Kind == FlowKind::Switch && !Blk.Successors.empty()) {
      uint32_t Taken = Blk.Successors[0];
      for (size_t K = 1; K < Blk.Successors.size() && K < Blk.CaseValues.size(); ++K)
        if (Blk.CaseValues[K] == Blk.Condition.Index)
          Taken = Blk.Successors[K];
      llvm::SmallVector<uint32_t, 4> OldSuccs = Blk.Successors;
      Blk.Kind = FlowKind::Jump;
      Blk.Condition = Operand{OperandKind::Null};
      Blk.Successors = {Taken};
      Blk.CaseValues.clear();
      for (uint32_t S : OldSuccs)
        if (S != Taken && S < Ssa.Blocks.size())
          RemovePhiPredInput(Ssa.Blocks[S], B);
      Changed = true;
    }
  }
  if (Changed)
    CompactReachableBlocks(Ssa);
  return Changed;
}

bool MergeTrivialJumps(SsaProgram &Ssa,
                       std::vector<llvm::SmallVector<uint32_t, 4>> &Preds) {
  bool Changed = false;
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Blk = Ssa.Blocks[B];
    while (Blk.Kind == FlowKind::Jump && Blk.Successors.size() == 1) {
      const uint32_t Succ = Blk.Successors[0];
      if (Succ == 0 || Succ == B || Succ >= Ssa.Blocks.size() ||
          Preds[Succ].size() != 1 || Preds[Succ][0] != B)
        break;
      bool HasLivePhi = false;
      for (const auto &Phi : Ssa.Blocks[Succ].Phis)
        if (!Phi.Dead)
          HasLivePhi = true;
      if (HasLivePhi)
        break;
      MergeSoleSuccessorJoin(Ssa, B, Succ);
      Preds[Succ].clear();
      for (uint32_t Next : Blk.Successors)
        if (Next < Preds.size())
          for (uint32_t &P : Preds[Next])
            if (P == Succ)
              P = B;
      Changed = true;
    }
  }
  return Changed;
}

bool RunIfConvertPass(SsaProgram &Ssa,
                      std::vector<llvm::SmallVector<uint32_t, 4>> &Preds) {
  bool PassChanged = false;
  for (uint32_t H = 0; H < Ssa.Blocks.size(); ++H) {
    bool MergedJoin = false;
    while (TryIfConvertBlock(Ssa, H, Preds, MergedJoin)) {
      PassChanged = true;
      if (!MergedJoin)
        break;
    }
  }
  return PassChanged;
}

} // namespace

bool dxcb5::RunOptCfgCleanup(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt CFG Cleanup", llvm::StringRef());
  bool AnyChanged = TryFoldConstantBranches(Ssa);
  if (Ssa.Blocks.size() >= 2) {
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    if (Dom.RpoOrder.size() < Ssa.Blocks.size()) {
      CompactReachableBlocks(Ssa);
      AnyChanged = true;
    }
  }
  if (Ssa.Blocks.size() >= 2) {
    std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
    Ssa.BuildPredecessors(Preds);
    while (MergeTrivialJumps(Ssa, Preds))
      AnyChanged = true;
  }
  if (AnyChanged) {
    CompactReachableBlocks(Ssa);
    Ssa.CompactDead();
  }
  return AnyChanged;
}

bool dxcb5::RunOptIfConvert(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt If-Convert", llvm::StringRef());
  bool AnyChanged = false;
  if (Ssa.Blocks.size() >= 2) {
    std::vector<llvm::SmallVector<uint32_t, 4>> Preds;
    Ssa.BuildPredecessors(Preds);
    while (RunIfConvertPass(Ssa, Preds)) {
      AnyChanged = true;
      MergeTrivialJumps(Ssa, Preds);
    }
  }
  if (AnyChanged) {
    CompactReachableBlocks(Ssa);
    Ssa.CompactDead();
  }
  return AnyChanged;
}
