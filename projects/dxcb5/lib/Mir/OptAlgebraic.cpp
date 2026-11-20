// Distributed under the University of Illinois Open Source License.
#include "Mir/OptAlgebraicCommon.h"
#include "llvm/Support/TimeProfiler.h"

using namespace dxcb5;
using namespace dxcb5::alg;

namespace {

bool IsBoolMaskRec(const SsaProgram &Ssa, const std::vector<SsaDefSite> &Defs,
                   const Operand &Op, unsigned Depth) {
  if (Op.Kind == OperandKind::Immediate && !Op.Modifier &&
      (Op.Index == 0u || Op.Index == 0xffffffffu))
    return true;
  if (Depth > 6 || Op.Kind != OperandKind::Temp || Op.Modifier ||
      Op.RelativeRegister != UINT32_MAX || Op.Index >= Defs.size())
    return false;
  const auto &D = Defs[Op.Index];
  if (!D.Valid() || D.IsAux)
    return false;
  if (D.IsPhi) {
    const auto &Phi = Ssa.Blocks[D.Block].Phis[D.Index];
    if (Depth > 3 || Phi.Dead || Phi.Inputs.empty() || Phi.Inputs.size() > 4)
      return false;
    for (const auto &In : Phi.Inputs)
      if (!IsBoolMaskRec(Ssa, Defs, In.Value, Depth + 1))
        return false;
    return true;
  }
  const auto &P = Ssa.Blocks[D.Block].Insts[D.Index];
  if (P.Dead)
    return false;
  if (InstructionIsCompare(P.Opcode))
    return true;
  if (P.Opcode == D3D10_SB_OPCODE_NOT)
    return P.Sources.size() == 1 && IsBoolMaskRec(Ssa, Defs, P.Sources[0], Depth + 1);
  if (P.Opcode == D3D10_SB_OPCODE_AND || P.Opcode == D3D10_SB_OPCODE_OR ||
      P.Opcode == D3D10_SB_OPCODE_XOR)
    return P.Sources.size() == 2 &&
           IsBoolMaskRec(Ssa, Defs, P.Sources[0], Depth + 1) &&
           IsBoolMaskRec(Ssa, Defs, P.Sources[1], Depth + 1);
  if (P.Opcode == D3D10_SB_OPCODE_MOVC)
    return P.Sources.size() == 3 &&
           IsBoolMaskRec(Ssa, Defs, P.Sources[1], Depth + 1) &&
           IsBoolMaskRec(Ssa, Defs, P.Sources[2], Depth + 1);
  return false;
}

bool SimplifySingleInst(AlgContext &Ctx, uint32_t B, uint32_t &I) {
  bool Changed = false;
  auto &Inst = Ctx.Ssa.Blocks[B].Insts[I];
  Changed |= FoldRelativeIndexOffset(Ctx, Inst);
  Changed |= ForwardSingleUseDynamicCb(Ctx, B, Inst);
  Changed |= TrySimplifyBitfieldAndConstInt(Ctx, Inst);
  switch (Inst.Opcode) {
  case D3D10_SB_OPCODE_XOR:
  case D3D10_SB_OPCODE_NOT:
  case D3D10_SB_OPCODE_OR:
  case D3D10_SB_OPCODE_FTOU:
  case D3D10_SB_OPCODE_FTOI:
  case D3D10_SB_OPCODE_ITOF:
  case D3D10_SB_OPCODE_UTOF:
  case D3D10_SB_OPCODE_IMUL:
  case D3D10_SB_OPCODE_INE:
  case D3D10_SB_OPCODE_IEQ:
  case D3D10_SB_OPCODE_IADD:
    Changed |= SimplifyIntAndBitInst(Ctx, Inst);
    break;
  case D3D10_SB_OPCODE_MOV:
  case D3D10_SB_OPCODE_MUL:
  case D3D10_SB_OPCODE_ADD:
  case D3D10_SB_OPCODE_MAD:
  case D3D10_SB_OPCODE_MAX:
  case D3D10_SB_OPCODE_MIN:
  case D3D10_SB_OPCODE_EXP:
  case D3D10_SB_OPCODE_DIV:
  case D3D10_SB_OPCODE_RSQ:
  case D3D10_SB_OPCODE_SQRT:
  case D3D11_SB_OPCODE_RCP:
    Changed |= SimplifyFloatInst(Ctx, B, I);
    break;
  case D3D10_SB_OPCODE_MOVC:
  case D3D10_SB_OPCODE_DP2:
  case D3D10_SB_OPCODE_DP3:
  case D3D10_SB_OPCODE_DP4:
    Changed |= SimplifySelectAndDotInst(Ctx, B, I);
    break;
  default: break;
  }
  return Changed;
}

bool StripZextBoolFromPhi(AlgContext &Ctx, SsaPhi &Phi) {
  if (Phi.Dead || Phi.Result >= Ctx.Stats.Count.size() ||
      Ctx.Stats.Count[Phi.Result] == 0 ||
      Ctx.Stats.NonBoolTestUse[Phi.Result])
    return false;
  bool Changed = false;
  for (auto &In : Phi.Inputs) {
    Operand Inner;
    bool KnownMask = false;
    if (MatchZextBool(Ctx, In.Value, Inner, KnownMask) && KnownMask) {
      In.Value = Inner;
      Ctx.BumpUse(Inner);
      Changed = true;
    }
  }
  return Changed;
}

bool StripZextBoolFromBoolPhis(AlgContext &Ctx) {
  bool Changed = false;
  for (auto &Block : Ctx.Ssa.Blocks)
    for (auto &Phi : Block.Phis)
      Changed |= StripZextBoolFromPhi(Ctx, Phi);
  return Changed;
}

bool SimplifyInstructions(SsaProgram &Ssa) {
  AlgContext Ctx{Ssa, {}, {}};
  Ssa.BuildDefTable(Ctx.Defs);
  CollectUseStats(Ssa, Ctx.Stats);
  bool Changed = StripZextBoolFromBoolPhis(Ctx);
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Block = Ssa.Blocks[B];
    Changed |= FoldBranchCondition(Ctx, Block);
    for (uint32_t I = 0; I < Block.Insts.size(); ++I) {
      if (Block.Insts[I].Dead)
        continue;
      Changed |= SimplifySingleInst(Ctx, B, I);
    }
    Changed |= FoldBranchCondition(Ctx, Block);
  }
  Changed |= RunGlobalAlgebraicFolds(Ctx);
  return Changed;
}

} // namespace

void AlgContext::RefreshBlockAfterInsert(uint32_t B, uint32_t FirstMovedIdx) {
  Defs.resize(Ssa.NextValueId);
  const auto &Block = Ssa.Blocks[B];
  for (uint32_t K = FirstMovedIdx; K < Block.Insts.size(); ++K) {
    const auto &BI = Block.Insts[K];
    if (BI.Dest.Kind == OperandKind::Temp && BI.Dest.Index < Defs.size())
      Defs[BI.Dest.Index] = {B, K, false, false};
    if (BI.AuxDest.Kind == OperandKind::Temp && BI.AuxDest.Index < Defs.size())
      Defs[BI.AuxDest.Index] = {B, K, false, true};
  }
  Stats.Count.resize(Ssa.NextValueId, 0);
  Stats.NonFloatUse.resize(Ssa.NextValueId, 0);
  Stats.RelativeUse.resize(Ssa.NextValueId, 0);
  Stats.NonBoolTestUse.resize(Ssa.NextValueId, 0);
}

void dxcb5::alg::CollectUseStats(const SsaProgram &Ssa, UseStats &Stats) {
  Stats.Count.assign(Ssa.NextValueId, 0);
  Stats.NonFloatUse.assign(Ssa.NextValueId, 0);
  Stats.RelativeUse.assign(Ssa.NextValueId, 0);
  Stats.NonBoolTestUse.assign(Ssa.NextValueId, 0);
  auto Touch = [&](const Operand &Op, bool AcceptsFloatMod,
                   bool BoolTestOnly = false) {
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < Ssa.NextValueId) {
      ++Stats.Count[Op.RelativeRegister];
      Stats.NonFloatUse[Op.RelativeRegister] = 1;
      Stats.RelativeUse[Op.RelativeRegister] = 1;
      Stats.NonBoolTestUse[Op.RelativeRegister] = 1;
    }
    if (Op.Kind == OperandKind::Temp && Op.Index < Ssa.NextValueId) {
      ++Stats.Count[Op.Index];
      if (!AcceptsFloatMod)
        Stats.NonFloatUse[Op.Index] = 1;
      if (!BoolTestOnly || Op.Modifier != 0 ||
          Op.RelativeRegister != UINT32_MAX)
        Stats.NonBoolTestUse[Op.Index] = 1;
    }
  };
  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        Touch(In.Value, false);
    }
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.RelativeRegister != UINT32_MAX &&
          Inst.Dest.RelativeRegister < Ssa.NextValueId) {
        ++Stats.Count[Inst.Dest.RelativeRegister];
        Stats.NonFloatUse[Inst.Dest.RelativeRegister] = 1;
        Stats.RelativeUse[Inst.Dest.RelativeRegister] = 1;
        Stats.NonBoolTestUse[Inst.Dest.RelativeRegister] = 1;
      }
      const bool MovAcceptsMod =
          Inst.Opcode == D3D10_SB_OPCODE_MOV &&
          (Inst.Saturate ||
           (!Inst.Sources.empty() && Inst.Sources[0].Modifier != 0));
      const bool IsZeroCmp =
          (Inst.Opcode == D3D10_SB_OPCODE_INE ||
           Inst.Opcode == D3D10_SB_OPCODE_IEQ) &&
          Inst.Sources.size() == 2;
      for (size_t S = 0; S < Inst.Sources.size(); ++S) {
        const bool AcceptsMod =
            Inst.Opcode == PseudoCombineVec ||
            (!IsPseudoOpcode(Inst.Opcode) &&
             (InstructionOperandAcceptsFloatModifiers(Inst.Opcode, S) ||
              MovAcceptsMod));
        const bool BoolTest =
            (Inst.Opcode == D3D10_SB_OPCODE_MOVC && S == 0) ||
            (IsZeroCmp && Inst.Sources[1 - S].Kind == OperandKind::Immediate &&
             Inst.Sources[1 - S].Index == 0u && !Inst.Sources[1 - S].Modifier);
        Touch(Inst.Sources[S], AcceptsMod, BoolTest);
      }
    }
    if (Block.Kind == FlowKind::Branch)
      Touch(Block.Condition, false, true);
    else if (Block.Kind == FlowKind::Switch)
      Touch(Block.Condition, false, false);
  }
}

bool dxcb5::alg::IsBoolMaskValue(const SsaProgram &Ssa,
                                 const std::vector<SsaDefSite> &Defs,
                                 const Operand &StartOp) {
  return IsBoolMaskRec(Ssa, Defs, StartOp, 0);
}

bool dxcb5::alg::MatchZextBool(const AlgContext &Ctx, const Operand &Op,
                               Operand &InnerMask, bool &KnownMask) {
  if (Op.Kind != OperandKind::Temp || Op.Modifier ||
      Op.RelativeRegister != UINT32_MAX)
    return false;
  const auto &D = Ctx.Defs[Op.Index];
  if (!D.Valid() || D.IsPhi || D.IsAux)
    return false;
  const auto &P = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
  if (P.Dead || P.Opcode != D3D10_SB_OPCODE_AND || P.Sources.size() != 2)
    return false;
  unsigned OneSide = 2;
  for (unsigned K = 0; K < 2; ++K)
    if (P.Sources[K].Kind == OperandKind::Immediate && P.Sources[K].Index == 1u) {
      OneSide = K;
      break;
    }
  if (OneSide == 2)
    return false;
  const Operand &Raw = P.Sources[1 - OneSide];
  if (Raw.RelativeRegister != UINT32_MAX || Raw.Modifier)
    return false;
  InnerMask = Raw;
  if (InnerMask.Kind != OperandKind::Immediate &&
      InnerMask.Kind != OperandKind::ImmediateVector)
    InnerMask.Swizzle = ComposeSwizzle(Raw.Swizzle, Op.Swizzle);
  KnownMask = IsBoolMaskValue(Ctx.Ssa, Ctx.Defs, Raw);
  return true;
}

bool dxcb5::alg::TryResolveLane(const AlgContext &Ctx, const Operand &Op,
                                unsigned L, bool RestrictBase, Operand &Out) {
  if (Op.RelativeRegister != UINT32_MAX)
    return false;
  if (Op.Kind == OperandKind::Immediate) {
    Out = Op;
    return true;
  }
  if (Op.Kind == OperandKind::ImmediateVector) {
    if (Op.Modifier)
      return false;
    Out = MakeImm(
        Ctx.Ssa.Mir.LiteralVectors[Op.Index][GetSwizzleLane(Op.Swizzle, L)]);
    return true;
  }
  if (Op.Kind == OperandKind::Temp && !Op.Modifier &&
      Op.Index < Ctx.Defs.size()) {
    const auto &D = Ctx.Defs[Op.Index];
    if (D.Valid() && !D.IsPhi && !D.IsAux) {
      const auto &P = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
      const unsigned K = GetSwizzleLane(Op.Swizzle, L);
      if (!P.Dead && P.Opcode == PseudoCombineVec && K < P.Sources.size() &&
          P.Sources[K].RelativeRegister == UINT32_MAX) {
        Out = P.Sources[K];
        return true;
      }
    }
  }
  if (RestrictBase && Op.Kind != OperandKind::Temp &&
      Op.Kind != OperandKind::Input && Op.Kind != OperandKind::ConstantBuffer)
    return false;
  Out = Op;
  Out.Mask = 1u;
  Out.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(Op.Swizzle, L));
  return true;
}

bool dxcb5::RunOptAlgebraic(SsaProgram &Ssa) {
  llvm::TimeTraceScope Scope("DXCB5 Opt Algebraic", llvm::StringRef());
  bool Changed = false;
  Changed |= SimplifyInstructions(Ssa);
  Changed |= FoldCopiesAndModifiers(Ssa);
  if (Changed)
    Ssa.CompactDead();
  return Changed;
}
