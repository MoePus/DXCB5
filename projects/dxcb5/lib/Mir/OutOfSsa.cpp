// Distributed under the University of Illinois Open Source License.
#include "Mir/OutOfSsa.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TimeProfiler.h"
#include <algorithm>
#include <array>

using namespace dxcb5;

namespace {

Instruction ToMirInst(const SsaInst &S) {
  Instruction Out;
  Out.Opcode = S.Opcode;
  Out.TestNonZero = S.TestNonZero;
  Out.Saturate = S.Saturate;
  Out.TexelOffset[0] = S.TexelOffset[0];
  Out.TexelOffset[1] = S.TexelOffset[1];
  Out.TexelOffset[2] = S.TexelOffset[2];
  const unsigned Dests = InstructionDestinationCount(S.Opcode);
  if (Dests >= 1)
    Out.Operands.push_back(S.Dest);
  if (Dests >= 2)
    Out.Operands.push_back(S.AuxDest);
  for (const auto &Src : S.Sources)
    Out.Operands.push_back(Src);
  return Out;
}

void LowerCombineVec(const SsaInst &Inst, SsaProgram &Ssa, uint8_t SkipMask,
                     std::vector<Instruction> &Out) {
  const uint32_t DstReg = Inst.Dest.Index;
  const unsigned W = static_cast<unsigned>(Inst.Sources.size());
  uint8_t Remaining =
      static_cast<uint8_t>((Inst.Dest.Mask & MaskForWidth(W)) & ~SkipMask);
  if (!Remaining)
    return;

  uint8_t ImmMask = 0;
  std::array<uint32_t, 4> ImmBits{};
  bool AllSameImm = true;
  uint32_t FirstImm = 0;
  for (unsigned L = 0; L < W; ++L) {
    if (!(Remaining & (1u << L)))
      continue;
    const auto &Op = Inst.Sources[L];
    if (Op.Kind != OperandKind::Immediate || Op.Modifier)
      continue;
    if (!ImmMask)
      FirstImm = Op.Index;
    else if (Op.Index != FirstImm)
      AllSameImm = false;
    ImmMask |= static_cast<uint8_t>(1u << L);
    ImmBits[L] = Op.Index;
  }
  if (ImmMask) {
    Operand Dst = MakeSsaTemp(DstReg, ImmMask, kIdentitySwizzle);
    Operand Src = AllSameImm
                      ? MakeImm(FirstImm)
                      : Operand{OperandKind::ImmediateVector,
                                Ssa.InternLiteralVector(ImmBits), 0, ImmMask,
                                kIdentitySwizzle};
    Out.push_back({D3D10_SB_OPCODE_MOV, {Dst, Src}});
    Remaining &= static_cast<uint8_t>(~ImmMask);
  }

  while (Remaining) {
    const unsigned First =
        static_cast<unsigned>(llvm::countTrailingZeros(Remaining));
    const auto &Base = Inst.Sources[First];
    uint8_t GroupMask = static_cast<uint8_t>(1u << First);
    uint8_t Swz = BroadcastLaneSwizzle(GetSwizzleLane(Base.Swizzle, 0));
    for (unsigned L = First + 1; L < W; ++L) {
      if ((Remaining & (1u << L)) && SameOperandBase(Base, Inst.Sources[L])) {
        GroupMask |= static_cast<uint8_t>(1u << L);
        Swz = SetSwizzleLane(Swz, L, GetSwizzleLane(Inst.Sources[L].Swizzle, 0));
      }
    }
    Operand Dst = MakeSsaTemp(DstReg, GroupMask, kIdentitySwizzle);
    Operand Src = Base;
    Src.Swizzle = Swz;
    Out.push_back({D3D10_SB_OPCODE_MOV, {Dst, Src}});
    Remaining &= static_cast<uint8_t>(~GroupMask);
  }
}

void CountSsaUses(const SsaProgram &Ssa, std::vector<uint32_t> &UseCount) {
  UseCount.assign(Ssa.NextValueId, 0);
  auto Touch = [&](const Operand &Op) {
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < Ssa.NextValueId)
      ++UseCount[Op.RelativeRegister];
    if (Op.Kind == OperandKind::Temp && Op.Index < Ssa.NextValueId)
      ++UseCount[Op.Index];
  };
  for (const auto &Block : Ssa.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        Touch(In.Value);
    }
    for (const auto &Inst : Block.Insts) {
      if (Inst.Dead)
        continue;
      if (Inst.Dest.RelativeRegister != UINT32_MAX &&
          Inst.Dest.RelativeRegister < Ssa.NextValueId)
        ++UseCount[Inst.Dest.RelativeRegister];
      if (Inst.AuxDest.RelativeRegister != UINT32_MAX &&
          Inst.AuxDest.RelativeRegister < Ssa.NextValueId)
        ++UseCount[Inst.AuxDest.RelativeRegister];
      for (const auto &Src : Inst.Sources)
        Touch(Src);
    }
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
      Touch(Block.Condition);
  }
}

bool TryRetargetOutputMov(SsaBlock &Block, uint32_t BlockId, size_t I,
                          size_t LastDiscard,
                          const std::vector<SsaDefSite> &Defs,
                          const std::vector<uint32_t> &UseCount,
                          const llvm::SmallVectorImpl<std::array<size_t, 4>> &LastWrite) {
  auto &Inst = Block.Insts[I];
  if (Inst.Opcode != D3D10_SB_OPCODE_MOV || Inst.Saturate ||
      Inst.Dest.Kind != OperandKind::Output ||
      Inst.Sources[0].Kind != OperandKind::Temp || Inst.Sources[0].Modifier ||
      Inst.Sources[0].RelativeRegister != UINT32_MAX)
    return false;
  const uint32_t SrcId = Inst.Sources[0].Index;
  const auto &Def = Defs[SrcId];
  if (!Def.Valid() || Def.IsPhi || Def.IsAux || Def.Block != BlockId ||
      Def.Index < LastDiscard || UseCount[SrcId] != 1)
    return false;
  auto &Prod = Block.Insts[Def.Index];
  const bool Retargetable =
      !Prod.Dead && Prod.IsPure() &&
      (DotSourceSlotMask(Prod.Opcode) ||
       (!IsPseudoOpcode(Prod.Opcode) && InstructionIsComponentwise(Prod.Opcode)));
  if (!Retargetable)
    return false;
  if (Inst.Dest.Index < LastWrite.size()) {
    const auto &OutPos = LastWrite[Inst.Dest.Index];
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((Inst.Dest.Mask & (1u << L)) && OutPos[L] > Def.Index)
        return false;
  }
  if (InstructionIsComponentwise(Prod.Opcode)) {
    bool NeedsSwizzle = false;
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((Inst.Dest.Mask & (1u << L)) &&
          GetSwizzleLane(Inst.Sources[0].Swizzle, L) != L)
        NeedsSwizzle = true;
    if (NeedsSwizzle) {
      for (const auto &PS : Prod.Sources)
        if (PS.Kind == OperandKind::ImmediateVector)
          return false;
    }
    for (auto &PS : Prod.Sources) {
      if (PS.Kind != OperandKind::Immediate &&
          PS.Kind != OperandKind::ImmediateVector)
        PS.Swizzle = ComposeSwizzle(PS.Swizzle, Inst.Sources[0].Swizzle);
    }
  }
  Prod.Dest = Inst.Dest;
  Inst.Dead = true;
  return true;
}

void RetargetOutputs(SsaProgram &Ssa) {
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<uint32_t> UseCount;
  CountSsaUses(Ssa, UseCount);

  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &Block = Ssa.Blocks[B];
    size_t LastDiscard = 0;
    llvm::SmallVector<std::array<size_t, 4>, 8> LastWrite;
    auto RecordOut = [&](const Operand &Dst, size_t Pos) {
      if (Dst.Index >= LastWrite.size())
        LastWrite.resize(Dst.Index + 1, {0, 0, 0, 0});
      for (unsigned L = 0; L < kRegisterLanes; ++L)
        if (Dst.Mask & (1u << L))
          LastWrite[Dst.Index][L] = Pos;
    };
    for (size_t I = 0; I < Block.Insts.size(); ++I) {
      const auto &Inst = Block.Insts[I];
      if (Inst.Dead)
        continue;
      if (Inst.Opcode == D3D10_SB_OPCODE_DISCARD) {
        LastDiscard = I + 1;
        continue;
      }
      if (TryRetargetOutputMov(Block, B, I, LastDiscard, Defs, UseCount,
                               LastWrite) ||
          Inst.Dest.Kind == OperandKind::Output)
        RecordOut(Inst.Dest, I + 1);
    }
  }
  Ssa.CompactDead();
}

bool TryRetargetCombineLane(SsaBlock &SB, size_t BlockId, size_t CombineIdx,
                            unsigned Lane, uint32_t DstReg,
                            const Operand &Src,
                            const std::vector<SsaDefSite> &Defs,
                            const std::vector<uint32_t> &UseCount) {
  if (Src.Kind != OperandKind::Temp || Src.Modifier ||
      Src.RelativeRegister != UINT32_MAX ||
      GetSwizzleLane(Src.Swizzle, 0) != 0 || UseCount[Src.Index] != 1)
    return false;
  const auto &D = Defs[Src.Index];
  if (!D.Valid() || D.IsPhi || D.IsAux || D.Block != BlockId ||
      D.Index >= CombineIdx)
    return false;
  auto &Prod = SB.Insts[D.Index];
  if (Prod.Dead || !Prod.IsPure() || Prod.Dest.Kind != OperandKind::Temp ||
      Prod.Dest.Mask != 1u || IsPseudoOpcode(Prod.Opcode))
    return false;
  const bool IsDot = DotSourceSlotMask(Prod.Opcode) != 0;
  const bool IsComp = InstructionIsComponentwise(Prod.Opcode);
  if (!IsDot && !IsComp)
    return false;
  if (IsComp && Lane != 0) {
    for (const auto &PS : Prod.Sources)
      if (PS.Kind == OperandKind::ImmediateVector)
        return false;
    for (auto &PS : Prod.Sources)
      if (PS.Kind != OperandKind::Immediate)
        PS.Swizzle = SetSwizzleLane(PS.Swizzle, Lane,
                                    GetSwizzleLane(PS.Swizzle, 0));
  }
  Prod.Dest = MakeSsaLane(DstReg, Lane);
  return true;
}

void RetargetCombineProducers(SsaProgram &Ssa,
                              std::vector<std::vector<uint8_t>> &CombineSkip) {
  std::vector<SsaDefSite> Defs;
  Ssa.BuildDefTable(Defs);
  std::vector<uint32_t> UseCount;
  CountSsaUses(Ssa, UseCount);

  CombineSkip.resize(Ssa.Blocks.size());
  for (size_t B = 0; B < Ssa.Blocks.size(); ++B) {
    auto &SB = Ssa.Blocks[B];
    CombineSkip[B].assign(SB.Insts.size(), 0);
    for (size_t I = 0; I < SB.Insts.size(); ++I) {
      auto &Inst = SB.Insts[I];
      if (Inst.Dead || Inst.Opcode != PseudoCombineVec ||
          Inst.Dest.Kind != OperandKind::Temp)
        continue;
      const uint32_t DstReg = Inst.Dest.Index;
      for (unsigned L = 0; L < Inst.Sources.size(); ++L) {
        if ((Inst.Dest.Mask & (1u << L)) &&
            TryRetargetCombineLane(SB, B, I, L, DstReg, Inst.Sources[L], Defs,
                                   UseCount))
          CombineSkip[B][I] |= static_cast<uint8_t>(1u << L);
      }
    }
  }
}

void LowerLocalArrayRead(const SsaInst &Inst, uint32_t ArrReg, SsaProgram &Ssa,
                         std::vector<Instruction> &Out) {
  if (Inst.ArrayLane != 0xffu) {
    Out.push_back(
        {D3D10_SB_OPCODE_MOV, {Inst.Dest, MakeSsaLane(ArrReg, Inst.ArrayLane)}});
    return;
  }
  const unsigned Len = Ssa.LocalArrayLengths[Inst.ArrayId];
  if (Len <= 1) {
    Out.push_back({D3D10_SB_OPCODE_MOV, {Inst.Dest, MakeSsaLane(ArrReg, 0)}});
    return;
  }
  const Operand Sel = Inst.Sources[0];
  const uint8_t CmpMask = static_cast<uint8_t>((1u << (Len - 1)) - 1u);
  const Operand MatchVec =
      MakeSsaTemp(Ssa.AllocValue(), CmpMask, kIdentitySwizzle);
  if (Len == 2) {
    Out.push_back({D3D10_SB_OPCODE_IEQ, {MatchVec, Sel, MakeImm(1u)}});
  } else {
    std::array<uint32_t, 4> Lits{1u, 2u, 3u, 1u};
    Operand LitVec{OperandKind::ImmediateVector, Ssa.InternLiteralVector(Lits),
                   0, CmpMask, kIdentitySwizzle};
    Out.push_back({D3D10_SB_OPCODE_IEQ, {MatchVec, Sel, LitVec}});
  }
  Operand Acc = MakeSsaLane(ArrReg, 0);
  for (unsigned L = 1; L < Len; ++L) {
    Operand MatchLane = MakeSsaLane(MatchVec.Index, L - 1);
    Out.push_back({D3D10_SB_OPCODE_MOVC,
                   {Inst.Dest, MatchLane, MakeSsaLane(ArrReg, L), Acc}});
    Acc = Inst.Dest;
  }
}

void MergeAdjacentOutputMoves(std::vector<Instruction> &Code) {
  size_t Write = 0;
  for (size_t Read = 0; Read < Code.size(); ++Read) {
    auto &Inst = Code[Read];
    if (Write > 0 && Inst.Opcode == D3D10_SB_OPCODE_MOV &&
        Code[Write - 1].Opcode == D3D10_SB_OPCODE_MOV &&
        Inst.Saturate == Code[Write - 1].Saturate &&
        Inst.Operands[0].Kind == OperandKind::Output &&
        Code[Write - 1].Operands[0].Kind == OperandKind::Output &&
        Inst.Operands[0].Index == Code[Write - 1].Operands[0].Index &&
        !(Inst.Operands[0].Mask & Code[Write - 1].Operands[0].Mask) &&
        SameOperandBase(Code[Write - 1].Operands[1], Inst.Operands[1])) {
      auto &PrevDst = Code[Write - 1].Operands[0];
      auto &PrevSrc = Code[Write - 1].Operands[1];
      for (unsigned L = 0; L < kRegisterLanes; ++L)
        if (Inst.Operands[0].Mask & (1u << L))
          PrevSrc.Swizzle = SetSwizzleLane(
              PrevSrc.Swizzle, L, GetSwizzleLane(Inst.Operands[1].Swizzle, L));
      PrevDst.Mask |= Inst.Operands[0].Mask;
      continue;
    }
    if (Write != Read)
      Code[Write] = std::move(Inst);
    ++Write;
  }
  Code.resize(Write);
}

void LowerPhiIncoming(SsaProgram &Ssa, size_t BlockId, const SsaPhi &Phi,
                      const SsaPhi::Incoming &In,
                      std::vector<FlowBlock> &Blocks) {
  auto &PredBlock = Blocks[In.PredBlock];
  Operand Src = In.Value;
  const bool NeedsMaterialize =
      Src.Modifier != 0 || Src.RelativeRegister != UINT32_MAX ||
      Src.Kind == OperandKind::ImmediateVector ||
      (Src.Kind != OperandKind::Temp && Src.Kind != OperandKind::Input &&
       Src.Kind != OperandKind::ConstantBuffer &&
       Src.Kind != OperandKind::Immediate);
  if (NeedsMaterialize) {
    const Operand Mat = MakeSsaTemp(Ssa.AllocValue(), Phi.Mask, kIdentitySwizzle);
    PredBlock.Instructions.push_back({D3D10_SB_OPCODE_MOV, {Mat, Src}});
    Src = Mat;
  }
  for (auto &Edge : PredBlock.Edges) {
    if (Edge.Target != BlockId)
      continue;
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      if (!(Phi.Mask & (1u << L)))
        continue;
      Operand LaneSrc = Src;
      if (Src.Kind != OperandKind::Immediate) {
        LaneSrc.Mask = 1;
        LaneSrc.Swizzle = BroadcastLaneSwizzle(GetSwizzleLane(Src.Swizzle, L));
      }
      Edge.Copies.emplace_back(Phi.Result, LaneSrc, static_cast<uint8_t>(L));
    }
  }
}

void CompactTempIds(SsaProgram &Ssa, std::vector<FlowBlock> &Blocks) {
  std::vector<uint32_t> Dense(Ssa.NextValueId, UINT32_MAX);
  uint32_t DenseCount = 0;
  auto MapReg = [&](uint32_t &Id) {
    if (Dense[Id] == UINT32_MAX)
      Dense[Id] = DenseCount++;
    Id = Dense[Id];
  };
  auto MapOp = [&](Operand &Op) {
    if (Op.Kind == OperandKind::Temp)
      MapReg(Op.Index);
    if (Op.RelativeRegister != UINT32_MAX)
      MapReg(Op.RelativeRegister);
  };
  for (auto &Block : Blocks) {
    if (Block.Kind == FlowKind::Branch || Block.Kind == FlowKind::Switch)
      MapOp(Block.Condition);
    for (auto &Inst : Block.Instructions)
      for (auto &Op : Inst.Operands)
        MapOp(Op);
    for (auto &Edge : Block.Edges) {
      for (auto &Copy : Edge.Copies) {
        MapReg(Copy.Destination);
        if (Copy.ReadsTemp())
          MapReg(Copy.Source.Index);
        if (Copy.Source.RelativeRegister != UINT32_MAX)
          MapReg(Copy.Source.RelativeRegister);
      }
    }
  }
  Ssa.Mir.TempCount = DenseCount;
}

} // namespace

void dxcb5::LowerOutOfSsa(SsaProgram &Ssa, std::vector<FlowBlock> &Blocks) {
  llvm::TimeTraceScope Scope("DXCB5 Out Of SSA", llvm::StringRef());
  RetargetOutputs(Ssa);

  std::vector<uint32_t> ArrayRegs(Ssa.LocalArrayLengths.size());
  for (size_t I = 0; I < Ssa.LocalArrayLengths.size(); ++I)
    ArrayRegs[I] = Ssa.AllocValue();

  std::vector<std::vector<uint8_t>> CombineSkip;
  RetargetCombineProducers(Ssa, CombineSkip);

  Blocks.assign(Ssa.Blocks.size(), FlowBlock{});
  for (size_t B = 0; B < Ssa.Blocks.size(); ++B) {
    const auto &SB = Ssa.Blocks[B];
    auto &FB = Blocks[B];
    FB.Kind = SB.Kind;
    FB.Condition = SB.Condition;
    for (size_t E = 0; E < SB.Successors.size(); ++E) {
      const uint32_t CaseVal = E < SB.CaseValues.size() ? SB.CaseValues[E] : 0u;
      FB.Edges.push_back({SB.Successors[E], {}, CaseVal});
    }
    for (size_t I = 0; I < SB.Insts.size(); ++I) {
      const auto &Inst = SB.Insts[I];
      if (Inst.Dead)
        continue;
      if (Inst.Opcode == PseudoCombineVec) {
        LowerCombineVec(Inst, Ssa, CombineSkip[B][I], FB.Instructions);
      } else if (Inst.Opcode == PseudoArrayWrite) {
        Instruction Mov;
        Mov.Opcode = D3D10_SB_OPCODE_MOV;
        Mov.Operands.push_back(MakeSsaLane(ArrayRegs[Inst.ArrayId], Inst.ArrayLane));
        Mov.Operands.push_back(Inst.Sources[0]);
        FB.Instructions.push_back(std::move(Mov));
      } else if (Inst.Opcode == PseudoArrayRead) {
        LowerLocalArrayRead(Inst, ArrayRegs[Inst.ArrayId], Ssa, FB.Instructions);
      } else {
        FB.Instructions.push_back(ToMirInst(Inst));
      }
    }
    MergeAdjacentOutputMoves(FB.Instructions);
  }

  for (size_t B = 0; B < Ssa.Blocks.size(); ++B) {
    for (const auto &Phi : Ssa.Blocks[B].Phis) {
      if (Phi.Dead)
        continue;
      for (const auto &In : Phi.Inputs)
        LowerPhiIncoming(Ssa, B, Phi, In, Blocks);
    }
  }
  CompactTempIds(Ssa, Blocks);
}
