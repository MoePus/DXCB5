// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include <array>

using namespace dxcb5;

namespace {

struct ArrayInfo {
  bool Eligible = true;
  std::array<uint8_t, kRegisterLanes> WriteCount{};
  std::array<uint32_t, kRegisterLanes> WriteBlock{};
  std::array<uint32_t, kRegisterLanes> WriteInst{};
  std::array<Operand, kRegisterLanes> WriteVal{
      MakeImm(0), MakeImm(0), MakeImm(0), MakeImm(0)};
};

bool IsPromotableValue(const Operand &Val) {
  return Val.Modifier == 0 && Val.RelativeRegister == UINT32_MAX &&
         (Val.Kind == OperandKind::Temp || Val.Kind == OperandKind::Input ||
          Val.Kind == OperandKind::ConstantBuffer ||
          Val.Kind == OperandKind::Immediate);
}

void ScanArrayWrites(const SsaProgram &Ssa, std::vector<ArrayInfo> &Info) {
  const size_t NumArrays = Ssa.LocalArrayLengths.size();
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    const auto &Insts = Ssa.Blocks[B].Insts;
    for (uint32_t I = 0; I < Insts.size(); ++I) {
      const auto &Inst = Insts[I];
      if (Inst.Dead || Inst.Opcode != PseudoArrayWrite ||
          Inst.ArrayId >= NumArrays)
        continue;
      auto &A = Info[Inst.ArrayId];
      const unsigned Len = Ssa.LocalArrayLengths[Inst.ArrayId];
      if (Inst.ArrayLane >= Len || Inst.Sources.empty() ||
          A.WriteCount[Inst.ArrayLane] != 0 ||
          !IsPromotableValue(Inst.Sources[0])) {
        A.Eligible = false;
        continue;
      }
      A.WriteCount[Inst.ArrayLane] = 1;
      A.WriteBlock[Inst.ArrayLane] = B;
      A.WriteInst[Inst.ArrayLane] = I;
      A.WriteVal[Inst.ArrayLane] = Inst.Sources[0];
    }
  }
}

bool WriteDominatesRead(const SsaDomTree &Dom, const ArrayInfo &A,
                        unsigned Lane, uint32_t RB, uint32_t RI) {
  if (!A.WriteCount[Lane])
    return true;
  if (A.WriteBlock[Lane] == RB)
    return A.WriteInst[Lane] < RI;
  return Dom.Dominates(A.WriteBlock[Lane], RB);
}

void ValidateArrayReads(const SsaProgram &Ssa, const SsaDomTree &Dom,
                        std::vector<ArrayInfo> &Info) {
  const size_t NumArrays = Ssa.LocalArrayLengths.size();
  for (uint32_t B = 0; B < Ssa.Blocks.size(); ++B) {
    const auto &Insts = Ssa.Blocks[B].Insts;
    for (uint32_t I = 0; I < Insts.size(); ++I) {
      const auto &Inst = Insts[I];
      if (Inst.Dead || Inst.Opcode != PseudoArrayRead ||
          Inst.ArrayId >= NumArrays)
        continue;
      auto &A = Info[Inst.ArrayId];
      if (!A.Eligible)
        continue;
      const unsigned Len = Ssa.LocalArrayLengths[Inst.ArrayId];
      if (Inst.ArrayLane != 0xffu) {
        if (Inst.ArrayLane >= Len ||
            !WriteDominatesRead(Dom, A, Inst.ArrayLane, B, I))
          A.Eligible = false;
        continue;
      }
      for (unsigned L = 0; L < Len; ++L)
        if (!WriteDominatesRead(Dom, A, L, B, I))
          A.Eligible = false;
    }
  }
}

void EmitPromotedRead(SsaProgram &Ssa, const SsaInst &Inst, const ArrayInfo &A,
                      unsigned Len, std::vector<SsaInst> &Rebuilt) {
  if (Inst.ArrayLane != 0xffu || Len <= 1) {
    const unsigned Lane = Inst.ArrayLane != 0xffu ? Inst.ArrayLane : 0u;
    SsaInst Mov;
    Mov.Opcode = D3D10_SB_OPCODE_MOV;
    Mov.Dest = Inst.Dest;
    Mov.Sources = {A.WriteVal[Lane]};
    Rebuilt.push_back(std::move(Mov));
    return;
  }
  const Operand Sel = Inst.Sources[0];
  Operand Acc = A.WriteVal[0];
  for (unsigned L = 1; L < Len; ++L) {
    const Operand Match = MakeSsaTemp(Ssa.AllocValue(), 1, 0);
    SsaInst Eq;
    Eq.Opcode = D3D10_SB_OPCODE_IEQ;
    Eq.Dest = Match;
    Eq.Sources = {Sel, MakeImm(L)};
    Rebuilt.push_back(std::move(Eq));

    const Operand NextDest =
        (L + 1 == Len) ? Inst.Dest : MakeSsaTemp(Ssa.AllocValue(), 1, 0);
    SsaInst Movc;
    Movc.Opcode = D3D10_SB_OPCODE_MOVC;
    Movc.Dest = NextDest;
    Movc.Sources = {Match, A.WriteVal[L], Acc};
    Rebuilt.push_back(std::move(Movc));
    Acc = NextDest;
  }
}

bool RewriteBlockArrays(SsaProgram &Ssa, SsaBlock &Block,
                        const std::vector<ArrayInfo> &Info) {
  const size_t NumArrays = Ssa.LocalArrayLengths.size();
  bool Touches = false;
  for (const auto &Inst : Block.Insts) {
    if (!Inst.Dead &&
        (Inst.Opcode == PseudoArrayWrite || Inst.Opcode == PseudoArrayRead) &&
        Inst.ArrayId < NumArrays && Info[Inst.ArrayId].Eligible) {
      Touches = true;
      break;
    }
  }
  if (!Touches)
    return false;

  std::vector<SsaInst> Rebuilt;
  Rebuilt.reserve(Block.Insts.size() + 16);
  for (auto &Inst : Block.Insts) {
    if (Inst.Dead ||
        (Inst.Opcode != PseudoArrayWrite && Inst.Opcode != PseudoArrayRead) ||
        Inst.ArrayId >= NumArrays || !Info[Inst.ArrayId].Eligible) {
      Rebuilt.push_back(std::move(Inst));
      continue;
    }
    if (Inst.Opcode == PseudoArrayRead) {
      EmitPromotedRead(Ssa, Inst, Info[Inst.ArrayId],
                       Ssa.LocalArrayLengths[Inst.ArrayId], Rebuilt);
    }
  }
  Block.Insts = std::move(Rebuilt);
  return true;
}

} // namespace

bool dxcb5::RunOptPromoteLocalArrays(SsaProgram &Ssa) {
  const size_t NumArrays = Ssa.LocalArrayLengths.size();
  if (NumArrays == 0)
    return false;
  std::vector<ArrayInfo> Info(NumArrays);
  ScanArrayWrites(Ssa, Info);
  SsaDomTree Dom;
  Ssa.ComputeDominators(Dom);
  ValidateArrayReads(Ssa, Dom, Info);

  bool AnyEligible = false;
  for (const auto &A : Info)
    AnyEligible |= A.Eligible;
  if (!AnyEligible)
    return false;

  bool Changed = false;
  for (auto &Block : Ssa.Blocks)
    Changed |= RewriteBlockArrays(Ssa, Block, Info);
  return Changed;
}
