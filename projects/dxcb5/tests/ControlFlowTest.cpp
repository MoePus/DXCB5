// Distributed under the University of Illinois Open Source License.
#include "dxcb5/ControlFlow.h"
#include "dxcb5/RegisterAllocation.h"

#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"

#include <algorithm>
#include <array>
#include <iostream>

using namespace dxcb5;

namespace {

bool HasMove(const Instruction &Inst, uint32_t Destination,
             OperandKind SourceKind, uint32_t Source) {
  return Inst.Opcode == D3D10_SB_OPCODE_MOV && Inst.Operands.size() == 2 &&
         Inst.Operands[0].Kind == OperandKind::Temp &&
         Inst.Operands[0].Index == Destination &&
         Inst.Operands[1].Kind == SourceKind && Inst.Operands[1].Index == Source;
}

bool CheckConditionalDiscard(bool DiscardOnTrue, bool SameCopies,
                             bool DifferentLane = false) {
  const unsigned Side = DiscardOnTrue ? 0 : 1;
  std::vector<FlowBlock> Blocks(3);
  auto &Head = Blocks[0];
  Head.Kind = FlowKind::Branch;
  Head.Condition = {OperandKind::Temp, 2};
  Head.Edges.resize(2);
  Head.Edges[Side].Target = 1;
  Head.Edges[1 - Side] = {2, {{0, 1}}, 0};

  auto &Arm = Blocks[1];
  Arm.Kind = FlowKind::Jump;
  Arm.Instructions.push_back(
      {D3D10_SB_OPCODE_DISCARD, {{OperandKind::Immediate, UINT32_MAX}}});
  Arm.Edges = {{2, {{0, {OperandKind::Temp, SameCopies ? 1u : 2u},
                     static_cast<uint8_t>(DifferentLane)}}, 0}};
  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 3;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto &Code = Program.Instructions;
  if (!SameCopies || DifferentLane)
    return !Code.empty() && Code[0].Opcode == D3D10_SB_OPCODE_IF;
  return Code.size() == 3 && Code[0].Opcode == D3D10_SB_OPCODE_DISCARD &&
         Code[0].Operands[0].Kind == OperandKind::Temp &&
         Code[0].Operands[0].Index == 2 && Code[0].TestNonZero == DiscardOnTrue &&
         HasMove(Code[1], 0, OperandKind::Temp, 1) &&
         Code[2].Opcode == D3D10_SB_OPCODE_RET;
}

bool CheckBranch() {
  std::vector<FlowBlock> Blocks(3);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = {OperandKind::Temp, 2};
  Blocks[0].Edges = {{1, {{0, 1}, {1, 0}}}, {2, {{1, 0}}}};
  MirProgram Program;
  Program.TempCount = 3;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error) || Program.TempCount != 4) {
    std::cerr << Error << '\n';
    return false;
  }

  const auto &Code = Program.Instructions;
  const auto Branch = std::find_if(Code.begin(), Code.end(),
                                   [](const Instruction &Inst) {
                                     return Inst.Opcode == D3D10_SB_OPCODE_IF;
                                   });
  constexpr std::ptrdiff_t BranchInstructions = 8;
  if (Code.end() - Branch < BranchInstructions || Branch->Operands.size() != 1 ||
      Branch->Operands.front().Index != 2 || !Branch->TestNonZero)
    return false;
  return HasMove(Branch[1], 3, OperandKind::Temp, 0) &&
         HasMove(Branch[2], 0, OperandKind::Temp, 1) &&
         HasMove(Branch[3], 1, OperandKind::Temp, 3) &&
         Branch[4].Opcode == D3D10_SB_OPCODE_RET &&
         Branch[5].Opcode == D3D10_SB_OPCODE_ENDIF &&
         HasMove(Branch[6], 1, OperandKind::Temp, 0) &&
         Branch[7].Opcode == D3D10_SB_OPCODE_RET;
}

using CodeIterator = std::vector<Instruction>::const_iterator;

constexpr unsigned LaneCount = 4;
using RegisterLanes = std::array<uint32_t, LaneCount>;

uint8_t SwizzleLane(const Operand &Source, unsigned Lane) {
  return static_cast<uint8_t>((Source.Swizzle >> (Lane * 2)) & (LaneCount - 1));
}

Operand Register(uint32_t ID, uint8_t Lane = 0) {
  return {OperandKind::Temp, ID, 0, static_cast<uint8_t>(1u << Lane),
          static_cast<uint8_t>(Lane * 0x55u)};
}

bool CheckSymbolicCopies(CodeIterator First, CodeIterator Last,
                         const FlowEdge &Edge, uint32_t Scratch) {
  std::vector<RegisterLanes> Initial(Scratch + 1);
  for (uint32_t Reg = 0; Reg <= Scratch; ++Reg) {
    for (unsigned Lane = 0; Lane != LaneCount; ++Lane)
      Initial[Reg][Lane] = Reg * LaneCount + Lane;
  }
  auto Expected = Initial;
  for (const auto &Copy : Edge.Copies)
    Expected.at(Copy.Destination).at(Copy.DestinationLane) =
        Initial.at(Copy.Source.Index)[Copy.Source.Swizzle & (LaneCount - 1)];
  auto Actual = Initial;
  for (; First != Last; ++First) {
    if (First->Opcode != D3D10_SB_OPCODE_MOV ||
        First->Operands.size() != 2 ||
        First->Operands[0].Kind != OperandKind::Temp ||
        First->Operands[1].Kind != OperandKind::Temp ||
        !First->Operands[0].Mask ||
        First->Operands[0].Mask >= (1u << LaneCount) ||
        First->Operands[1].Modifier)
      return false;
    const auto &Dst = First->Operands[0];
    const auto &Src = First->Operands[1];
    const auto Old = Actual.at(Src.Index);
    for (unsigned Lane = 0; Lane != LaneCount; ++Lane) {
      if (Dst.Mask & (1u << Lane))
        Actual.at(Dst.Index)[Lane] = Old[SwizzleLane(Src, Lane)];
    }
  }
  Actual[Scratch][0] = Initial[Scratch][0];
  return Actual == Expected;
}

bool CheckCriticalEdgeCopies() {
  std::vector<FlowBlock> Blocks(3);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = {OperandKind::Input, 0};
  Blocks[0].Edges = {{2, {{0, Register(0, 2), 1},
                         {0, Register(0, 1), 2}}}, {1, {}}};
  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Edges = {{2, {{0, Register(2, 3), 1},
                         {0, Register(2, 1), 2}}}};
  MirProgram Program;
  constexpr uint32_t FirstScratch = 3;
  Program.TempCount = FirstScratch;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;

  const auto &Code = Program.Instructions;
  const auto Find = [&](uint32_t Opcode) {
    return std::find_if(Code.begin(), Code.end(), [&](const Instruction &Inst) {
      return Inst.Opcode == Opcode;
    });
  };
  const auto If = Find(D3D10_SB_OPCODE_IF);
  const auto Else = Find(D3D10_SB_OPCODE_ELSE);
  const auto End = Find(D3D10_SB_OPCODE_ENDIF);
  return If < Else && Else < End && End != Code.end() &&
         Else - If == 4 && End - Else == 2 &&
         CheckSymbolicCopies(If + 1, Else, Blocks[0].Edges[0], FirstScratch) &&
         CheckSymbolicCopies(Else + 1, End, Blocks[1].Edges[0], FirstScratch);
}

enum class Exit { End, Break, Continue, Return, Invalid };

class TraceExecution {
public:
  TraceExecution(const MirProgram &Program, uint32_t Selector)
      : TraceExecution(Program, RegisterLanes{Selector}) {}

  TraceExecution(const MirProgram &Program, const RegisterLanes &Selectors)
      : Code(Program.Instructions), Temps(Program.TempCount) {
    Temps.at(0) = Selectors;
  }

  bool Run() { return RunRange(0, Code.size()) == Exit::Return; }
  std::vector<uint32_t> Output;

private:
  using Position = std::size_t;

  uint32_t Read(const Operand &Op, unsigned Lane = 0) const {
    return Op.Kind == OperandKind::Immediate
               ? Op.Index
               : Temps.at(Op.Index)[SwizzleLane(Op, Lane)];
  }

  void Move(const Operand &Dst, const Operand &Src) {
    if (Dst.Kind == OperandKind::Output) {
      Output.push_back(Read(Src));
      return;
    }
    auto Result = Temps.at(Dst.Index);
    for (unsigned Lane = 0; Lane != LaneCount; ++Lane) {
      if (Dst.Mask & (1u << Lane))
        Result[Lane] = Read(Src, Lane);
    }
    Temps.at(Dst.Index) = Result;
  }

  Position Find(Position Begin, uint32_t Wanted, uint32_t Open,
                uint32_t Close) const {
    unsigned Depth = 0;
    for (Position I = Begin; I != Code.size(); ++I) {
      if (Depth == 0 && (Code[I].Opcode == Wanted || Code[I].Opcode == Close))
        return I;
      if (Code[I].Opcode == Open)
        ++Depth;
      if (Code[I].Opcode == Close)
        --Depth;
    }
    return Code.size();
  }

  Exit Branch(const Instruction &Inst, Position &PC) {
    const auto Else = Find(PC, D3D10_SB_OPCODE_ELSE, D3D10_SB_OPCODE_IF,
                           D3D10_SB_OPCODE_ENDIF);
    const auto End = Find(PC, D3D10_SB_OPCODE_ENDIF, D3D10_SB_OPCODE_IF,
                          D3D10_SB_OPCODE_ENDIF);
    const bool Taken = bool(Read(Inst.Operands[0])) == Inst.TestNonZero;
    const auto Result = Taken ? RunRange(PC, Else)
                              : RunRange(std::min(Else + 1, End), End);
    PC = End + 1;
    return Result;
  }

  Exit Switch(const Instruction &Inst, Position &PC) {
    const auto End = Find(PC, D3D10_SB_OPCODE_ENDSWITCH, D3D10_SB_OPCODE_SWITCH,
                          D3D10_SB_OPCODE_ENDSWITCH);
    const auto Selector = Read(Inst.Operands[0]);
    auto Default = End;
    auto Selected = End;
    unsigned Depth = 0;
    for (Position I = PC; I < End; ++I) {
      const auto &Label = Code[I];
      if (Label.Opcode == D3D10_SB_OPCODE_SWITCH)
        ++Depth;
      if (Label.Opcode == D3D10_SB_OPCODE_ENDSWITCH)
        --Depth;
      if (Depth)
        continue;
      if (Label.Opcode == D3D10_SB_OPCODE_DEFAULT)
        Default = I + 1;
      if (Label.Opcode == D3D10_SB_OPCODE_CASE &&
          Read(Label.Operands[0]) == Selector)
        Selected = I + 1;
    }
    PC = End + 1;
    const auto Result = RunRange(Selected == End ? Default : Selected, End);
    return Result == Exit::Break ? Exit::End : Result;
  }

  Exit Loop(Position &PC) {
    const auto Begin = PC;
    const auto End = Find(PC, D3D10_SB_OPCODE_ENDLOOP, D3D10_SB_OPCODE_LOOP,
                          D3D10_SB_OPCODE_ENDLOOP);
    PC = End + 1;
    if (End == Code.size())
      return Exit::Invalid;
    for (auto Result = RunRange(Begin, End);; Result = RunRange(Begin, End)) {
      if (Result == Exit::Continue)
        continue;
      if (Result != Exit::End)
        return Result == Exit::Break ? Exit::End : Result;
    }
  }

  Exit RunRange(Position PC, Position End) {
    while (PC < End && PC < Code.size()) {
      if (Remaining == 0)
        return Exit::Invalid;
      --Remaining;
      const auto &Inst = Code[PC++];
      const auto &Ops = Inst.Operands;
      Exit Result = Exit::End;
      switch (Inst.Opcode) {
      case D3D10_SB_OPCODE_MOV:
        Move(Ops[0], Ops[1]);
        break;
      case D3D10_SB_OPCODE_MOVC:
        Temps.at(Ops[0].Index)[0] = Read(Ops[Read(Ops[1]) ? 2 : 3]);
        break;
      case D3D10_SB_OPCODE_IADD:
        Temps.at(Ops[0].Index)[0] = Read(Ops[1]) + Read(Ops[2]);
        break;
      case D3D10_SB_OPCODE_ULT:
        Temps.at(Ops[0].Index)[0] = -(Read(Ops[1]) < Read(Ops[2]));
        break;
      case D3D10_SB_OPCODE_IF: Result = Branch(Inst, PC); break;
      case D3D10_SB_OPCODE_LOOP: Result = Loop(PC); break;
      case D3D10_SB_OPCODE_SWITCH: Result = Switch(Inst, PC); break;
      case D3D10_SB_OPCODE_BREAK: return Exit::Break;
      case D3D10_SB_OPCODE_BREAKC:
        if (bool(Read(Ops[0])) == Inst.TestNonZero)
          return Exit::Break;
        break;
      case D3D10_SB_OPCODE_CONTINUE: return Exit::Continue;
      case D3D10_SB_OPCODE_CONTINUEC:
        if (bool(Read(Ops[0])) == Inst.TestNonZero)
          return Exit::Continue;
        break;
      case D3D10_SB_OPCODE_RET: return Exit::Return;
      case D3D10_SB_OPCODE_RETC:
        if (bool(Read(Ops[0])) == Inst.TestNonZero)
          return Exit::Return;
        break;
      default: return Exit::Invalid;
      }
      if (Result != Exit::End)
        return Result;
    }
    return Exit::End;
  }

  const std::vector<Instruction> &Code;
  std::vector<RegisterLanes> Temps;
  static constexpr unsigned MaxSteps = 1024;
  unsigned Remaining = MaxSteps;
};

Operand Literal(uint32_t Value) { return {OperandKind::Immediate, Value}; }

Instruction Observe(Operand Value) {
  return {D3D10_SB_OPCODE_MOV, {{OperandKind::Output, 0}, Value}};
}

bool CheckSwitchAndBackEdge() {
  std::vector<FlowBlock> Blocks(4);
  Blocks[0].Kind = FlowKind::Jump;
  Blocks[0].Edges = {{1, {{3, Register(1), 1}, {3, Register(2), 2}}}};
  Blocks[0].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(11)}},
      {D3D10_SB_OPCODE_MOV, {Register(2), Literal(33)}},
      {D3D10_SB_OPCODE_MOV, {Register(3, 3), Literal(77)}}};
  Blocks[1].Kind = FlowKind::Switch;
  Blocks[1].Condition = Register(0);
  Blocks[1].Edges = {{2, {}}, {3, {}, 0}};
  Blocks[1].Instructions = {Observe(Register(3, 1))};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Edges = {{1, {{3, Register(3, 2), 1},
                         {3, Register(3, 1), 2}}}};
  Blocks[2].Instructions = {
      Observe(Register(3, 2)),
      {D3D10_SB_OPCODE_IADD, {Register(0), Register(0), Literal(UINT32_MAX)}}};
  Blocks[3].Instructions =
      {Observe(Literal(99)), Observe(Register(3, 2)), Observe(Register(3, 3))};

  MirProgram Program;
  Program.TempCount = 5;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  const auto State = Program.TempCount;
  if (!LowerControlFlow(Blocks, Program, Error)) {
    std::cerr << Error << '\n';
    return false;
  }

  const auto &Code = Program.Instructions;
  const auto BackEdge = std::find_if(Code.begin(), Code.end(),
      [](const Instruction &Inst) { return Inst.Opcode == D3D10_SB_OPCODE_IADD; });
  if (BackEdge == Code.end())
    return false;
  const auto Transition = std::find_if(BackEdge + 1, Code.end(),
      [State](const Instruction &Inst) {
        return Inst.Opcode == D3D10_SB_OPCODE_MOV && Inst.Operands.size() == 2 &&
               Inst.Operands[0].Kind == OperandKind::Temp &&
               Inst.Operands[0].Index == State &&
               Inst.Operands[1].Kind == OperandKind::Immediate;
      });
  if (Transition == Code.end() ||
      !CheckSymbolicCopies(BackEdge + 1, Transition, Blocks[2].Edges[0],
                           Program.TempCount - 1))
    return false;

  TraceExecution NoIterations(Program, 0);
  TraceExecution TwoIterations(Program, 2);
  return NoIterations.Run() &&
         NoIterations.Output == std::vector<uint32_t>{11, 99, 33, 77} &&
         TwoIterations.Run() &&
         TwoIterations.Output == std::vector<uint32_t>{11, 33, 33, 11, 11, 99, 33, 77};
}

bool CheckQuadAfterLoop() {
  std::vector<FlowBlock> Blocks(4);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Edges = {{1, {}}, {2, {}}};
  Blocks[0].Condition = {OperandKind::Input, 0};
  Blocks[1].Kind = FlowKind::Branch;
  Blocks[1].Edges = {{1, {}}, {2, {}}};
  Blocks[1].Condition = {OperandKind::Input, 1};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Edges = {{3, {}}};
  Blocks[3].Instructions = {
      {D3D10_SB_OPCODE_DERIV_RTX,
       {Register(0), {OperandKind::Input, 0}}}};

  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 1;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;

  const auto &Code = Program.Instructions;
  const auto IsOpcode = [](uint32_t Opcode) {
    return [Opcode](const Instruction &Inst) { return Inst.Opcode == Opcode; };
  };
  const auto EndLoop = std::find_if(Code.begin(), Code.end(),
                                    IsOpcode(D3D10_SB_OPCODE_ENDLOOP));
  return EndLoop != Code.end() &&
         std::find_if(Code.begin(), EndLoop, IsOpcode(D3D10_SB_OPCODE_DERIV_RTX)) ==
             EndLoop &&
         std::count_if(EndLoop, Code.end(), IsOpcode(D3D10_SB_OPCODE_DERIV_RTX)) == 1;
}

bool CheckQuadJoin() {
  std::vector<FlowBlock> Blocks(4);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = {OperandKind::Input, 0};
  Blocks[0].Edges = {{1, {}}, {2, {}}};
  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Edges = {{3, {}}};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Edges = {{3, {}}};
  Blocks[3].Instructions = {
      {D3D10_SB_OPCODE_DERIV_RTX,
       {{OperandKind::Temp, 0}, {OperandKind::Input, 0}}}};
  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 1;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error) || Program.TempCount != 1)
    return false;
  const auto &Code = Program.Instructions;
  return Code.size() == 2 && Code[0].Opcode == D3D10_SB_OPCODE_DERIV_RTX &&
         Code[1].Opcode == D3D10_SB_OPCODE_RET;
}

bool CheckNativeQuadLoop() {
  std::vector<FlowBlock> Blocks(4);
  Blocks[0].Kind = FlowKind::Jump;
  Blocks[0].Edges = {{1, {}}};
  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Edges = {{2, {}}};
  Blocks[1].Instructions = {
      {D3D10_SB_OPCODE_DERIV_RTX, {Register(3), {OperandKind::Input, 0}}}};
  Blocks[2].Kind = FlowKind::Branch;
  Blocks[2].Condition = Register(0);
  Blocks[2].Edges = {{3, {{3, 1}}}, {1, {{1, 2}, {2, 1}}}};
  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 4;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;

  const auto &Code = Program.Instructions;
  const auto Loop = std::find_if(Code.begin(), Code.end(), [](const Instruction &I) {
    return I.Opcode == D3D10_SB_OPCODE_LOOP;
  });
  if (Loop == Code.end() || Code.end() - Loop < 10)
    return false;
  const uint32_t Scratch = Program.TempCount - 1;
  return Loop[1].Opcode == D3D10_SB_OPCODE_DERIV_RTX &&
         Loop[2].Opcode == D3D10_SB_OPCODE_IF && Loop[2].TestNonZero &&
         Loop[4].Opcode == D3D10_SB_OPCODE_BREAK &&
         Loop[5].Opcode == D3D10_SB_OPCODE_ENDIF &&
         Loop[9].Opcode == D3D10_SB_OPCODE_ENDLOOP &&
         CheckSymbolicCopies(Loop + 3, Loop + 4, Blocks[2].Edges[0], Scratch) &&
         CheckSymbolicCopies(Loop + 6, Loop + 9, Blocks[2].Edges[1], Scratch);
}

bool CheckNativeLinearExits() {
  enum BlockID { Entry, First, Second, Exit, BlockCount };
  std::vector<FlowBlock> Blocks(BlockCount);
  Blocks[Entry].Kind = FlowKind::Jump;
  Blocks[Entry].Edges = {{First, {}}};
  Blocks[First].Kind = FlowKind::Branch;
  Blocks[First].Condition = Register(0);
  Blocks[First].Edges = {{Exit, {{1, Literal(31)}}}, {Second, {}}};
  Blocks[First].Instructions = {Observe(Literal(11))};
  Blocks[Second].Kind = FlowKind::Branch;
  Blocks[Second].Condition = Register(0);
  Blocks[Second].Edges = {{First, {}}, {Exit, {{1, Literal(32)}}}};
  Blocks[Second].Instructions = {Observe(Literal(22))};
  Blocks[Exit].Instructions = {Observe(Register(1))};

  MirProgram Program;
  Program.TempCount = 2;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  TraceExecution FirstExit(Program, 1), SecondExit(Program, 0);
  return std::count_if(Program.Instructions.begin(), Program.Instructions.end(),
                       [](const Instruction &Inst) {
                         return Inst.Opcode == D3D10_SB_OPCODE_BREAK ||
                                Inst.Opcode == D3D10_SB_OPCODE_BREAKC;
                       }) == 2 &&
         FirstExit.Run() && SecondExit.Run() &&
         FirstExit.Output == std::vector<uint32_t>{11, 31} &&
         SecondExit.Output == std::vector<uint32_t>{11, 22, 32};
}

bool CheckBranchedLoop() {
  enum BlockID { Entry, Header, Arm, Latch, Exit, BlockCount };
  std::vector<FlowBlock> Blocks(BlockCount);
  Blocks[Entry].Kind = FlowKind::Jump;
  Blocks[Entry].Edges = {{Header, {}}};
  Blocks[Entry].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1, 1), Literal(11)}},
      {D3D10_SB_OPCODE_MOV, {Register(1, 2), Literal(33)}},
      {D3D10_SB_OPCODE_MOV, {Register(1, 3), Literal(77)}},
      {D3D10_SB_OPCODE_MOV, {Register(3), Literal(2)}}};
  Blocks[Header].Kind = FlowKind::Branch;
  Blocks[Header].Condition = Register(0);
  Blocks[Header].Edges = {{Arm, {{2, Register(1, 1)}}},
                           {Latch, {{1, Register(1, 2), 1}}}};
  Blocks[Arm].Kind = FlowKind::Jump;
  Blocks[Arm].Instructions = {Observe(Register(2))};
  Blocks[Arm].Edges = {{Latch, {{1, Literal(55), 1}}}};
  Blocks[Latch].Kind = FlowKind::Branch;
  Blocks[Latch].Condition = Register(3);
  Blocks[Latch].Instructions = {
      Observe(Register(1, 1)), Observe(Register(1, 2)),
      {D3D10_SB_OPCODE_IADD, {Register(3), Register(3), Literal(UINT32_MAX)}}};
  Blocks[Latch].Edges = {{Header, {{1, Register(1, 2), 1}, {1, Register(1, 1), 2},
                                   {0, Literal(0)}}},
                          {Exit, {{2, Register(1, 1)}}}};
  Blocks[Exit].Instructions = {Observe(Register(2)), Observe(Register(1, 3))};

  MirProgram Program;
  Program.TempCount = 4;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto CountOpcode = [&](uint32_t Opcode) {
    return std::count_if(Program.Instructions.begin(), Program.Instructions.end(),
                         [Opcode](const Instruction &I) { return I.Opcode == Opcode; });
  };
  if (CountOpcode(D3D10_SB_OPCODE_LOOP) != 1 ||
      (CountOpcode(D3D10_SB_OPCODE_BREAK) + CountOpcode(D3D10_SB_OPCODE_BREAKC)) != 1 ||
      CountOpcode(D3D10_SB_OPCODE_SWITCH) != 0 || Program.TempCount != 5)
    return false;
  TraceExecution WithArm(Program, 1), WithoutArm(Program, 0);
  return WithArm.Run() && WithoutArm.Run() &&
         WithArm.Output == std::vector<uint32_t>{11, 55, 33, 55, 55, 55, 77} &&
         WithoutArm.Output == std::vector<uint32_t>{33, 33, 33, 33, 33, 77};
}

bool CheckGatherInBranchedLoop() {
  // The loop header can skip the gather and join at the loop latch.
  std::vector<FlowBlock> Blocks(5);
  Blocks[0].Kind = FlowKind::Jump;
  Blocks[0].Edges = {{1, {}}};
  Blocks[1].Kind = FlowKind::Branch;
  Blocks[1].Condition = {OperandKind::Input, 0};
  Blocks[1].Edges = {{2, {}}, {3, {}}};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Edges = {{3, {}}};
  Blocks[3].Kind = FlowKind::Branch;
  Blocks[3].Condition = {OperandKind::Input, 1};
  Blocks[3].Edges = {{1, {}}, {4, {}}};

  constexpr uint32_t Gathers[] = {
      D3D10_1_SB_OPCODE_GATHER4, D3D11_SB_OPCODE_GATHER4_C,
      D3D11_SB_OPCODE_GATHER4_PO, D3D11_SB_OPCODE_GATHER4_PO_C};
  for (auto Opcode : Gathers)
    Blocks[2].Instructions.push_back({Opcode, {}});

  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto IsOpcode = [](uint32_t Opcode) {
    return [Opcode](const Instruction &Inst) { return Inst.Opcode == Opcode; };
  };
  const auto &Code = Program.Instructions;
  const auto Loop = std::find_if(Code.begin(), Code.end(), IsOpcode(D3D10_SB_OPCODE_LOOP));
  const auto End = std::find_if(Loop, Code.end(), IsOpcode(D3D10_SB_OPCODE_ENDLOOP));
  if (End == Code.end() ||
      std::find_if(Loop, End, IsOpcode(D3D10_SB_OPCODE_SWITCH)) != End)
    return false;
  return std::all_of(std::begin(Gathers), std::end(Gathers), [&](uint32_t Opcode) {
    return std::count_if(Loop, End, IsOpcode(Opcode)) == 1;
  });
}

bool CheckQuadNeedsStructuredControlFlow() {
  std::vector<FlowBlock> Blocks(3);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = {OperandKind::Input, 0};
  Blocks[0].Edges = {{1, {}}, {2, {}}};
  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Edges = {{2, {}}};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Edges = {{1, {}}};
  Blocks[2].Instructions = {
      {D3D10_SB_OPCODE_DERIV_RTX,
       {{OperandKind::Temp, 0}, {OperandKind::Input, 0}}}};
  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 1;
  std::string Error;
  return !LowerControlFlow(Blocks, Program, Error) &&
         Error.find("quad operations") != std::string::npos &&
         Program.Instructions.empty() && Program.TempCount == 1;
}

bool CheckTransactionalFailure() {
  MirProgram Program;
  Program.TempCount = 2;
  Program.Instructions = {{D3D10_SB_OPCODE_RET, {}}};
  std::vector<FlowBlock> Blocks(2);
  Blocks[0].Kind = FlowKind::Jump;
  Blocks[0].Edges = {{1, {{0, 1}, {0, 0}}}};
  std::string Error;
  return !LowerControlFlow(Blocks, Program, Error) && !Error.empty() &&
         Program.TempCount == 2 && Program.Instructions.size() == 1 &&
         Program.Instructions.front().Opcode == D3D10_SB_OPCODE_RET;
}

bool CheckStructuredArms(bool TrueHasArm, bool FalseHasArm,
                         bool ArmWritesOutput = false) {
  constexpr uint32_t Head = 0, True = 1, False = 2, Join = 3, ExitBlock = 4;
  constexpr uint32_t State = 6;
  std::vector<FlowBlock> Blocks(5);
  Blocks[Head].Kind = FlowKind::Branch;
  Blocks[Head].Condition = Register(0);
  Blocks[Head].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(11)}},
      {D3D10_SB_OPCODE_MOV, {Register(2), Literal(29)}}};
  Blocks[Head].Edges = {{TrueHasArm ? True : Join, {{1, 2}, {2, 1}}},
                        {FalseHasArm ? False : Join, {{1, 2}, {3, 1}}}};
  if (TrueHasArm) {
    Blocks[True].Kind = FlowKind::Jump;
    Blocks[True].Edges = {{Join, {{1, 3}, {3, 2}}}};
    Blocks[True].Instructions = {
        {D3D10_SB_OPCODE_IADD, {Register(3), Register(1), Literal(1)}}};
  }
  if (FalseHasArm) {
    Blocks[False].Kind = FlowKind::Jump;
    Blocks[False].Edges = {{Join, {{1, 3}, {3, 2}}}};
    Blocks[False].Instructions = {
        {D3D10_SB_OPCODE_IADD, {Register(3), Register(1), Literal(2)}}};
  }
  if (ArmWritesOutput)
    Blocks[True].Instructions.push_back(Observe(Register(3)));

  // The back edge exercises SCC region structuring. The input register
  // is zero during both traces, so the join exits after one invocation.
  Blocks[Join].Kind = FlowKind::Branch;
  Blocks[Join].Condition = Register(5);
  Blocks[Join].Edges = {{Join, {}}, {ExitBlock, {}}};
  Blocks[Join].Instructions = {
      Observe(Register(1)), Observe(Register(2)), Observe(Register(3))};

  MirProgram Program;
  Program.TempCount = State;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto &Code = Program.Instructions;
  const auto Guards = std::count_if(Code.begin(), Code.end(), [](const Instruction &I) {
    return I.Opcode == D3D10_SB_OPCODE_ULT;
  });
  const auto StateWrites = std::count_if(Code.begin(), Code.end(), [State](const Instruction &I) {
    return I.Opcode == D3D10_SB_OPCODE_MOV && I.Operands.size() == 2 &&
           I.Operands[0].Kind == OperandKind::Temp && I.Operands[0].Index == State &&
           I.Operands[1].Kind == OperandKind::Immediate;
  });
  if (Guards != 0 || StateWrites != 0)
    return false;
  TraceExecution TruePath(Program, 1), FalsePath(Program, 0);
  std::vector<uint32_t> ExpectedTrue;
  if (ArmWritesOutput)
    ExpectedTrue.push_back(30);
  ExpectedTrue.insert(ExpectedTrue.end(),
                      {TrueHasArm ? 30u : 29u, 11u, TrueHasArm ? 11u : 0u});
  const std::vector<uint32_t> ExpectedFalse = {
      FalseHasArm ? 31u : 29u, 29, FalseHasArm ? 29u : 11u};
  return TruePath.Run() && FalsePath.Run() && TruePath.Output == ExpectedTrue &&
         FalsePath.Output == ExpectedFalse;
}

bool CheckSharedTail(bool UsesQuad) {
  enum BlockID { Head, Left, Right, Shared, Join, ExitBlock, BlockCount };
  std::vector<FlowBlock> Blocks(BlockCount);
  Blocks[Head].Kind = FlowKind::Branch;
  Blocks[Head].Condition = Register(0);
  Blocks[Head].Edges = {{Left, {}}, {Right, {}}};
  Blocks[Head].Instructions = {{D3D10_SB_OPCODE_MOV, {Register(1), Literal(0)}}};
  for (const auto Arm : {Left, Right}) {
    Blocks[Arm].Kind = FlowKind::Branch;
    Blocks[Arm].Condition = Register(0);
    Blocks[Arm].Edges = {{Shared, {}}, {Join, {}}};
  }
  Blocks[Shared].Kind = FlowKind::Jump;
  Blocks[Shared].Edges = {{Join, {}}};
  const uint32_t TailOpcode = UsesQuad ? D3D10_SB_OPCODE_DERIV_RTX
                                  : D3D10_SB_OPCODE_IADD;
  Blocks[Shared].Instructions = UsesQuad
      ? std::vector<Instruction>{{TailOpcode, {Register(1), Register(0)}}}
      : std::vector<Instruction>{{TailOpcode, {Register(1), Register(0), Literal(6)}}};
  // A false loop condition makes the join observable without repeating it.
  Blocks[Join].Kind = FlowKind::Branch;
  Blocks[Join].Condition = Register(2);
  Blocks[Join].Edges = {{Join, {}}, {ExitBlock, {}}};
  Blocks[ExitBlock].Instructions = {Observe(Register(1))};

  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 3;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto TailCount = std::count_if(Program.Instructions.begin(),
      Program.Instructions.end(),
      [TailOpcode](const Instruction &Inst) { return Inst.Opcode == TailOpcode; });
  if (UsesQuad)
    return TailCount == 1 && Program.TempCount == 5;

  TraceExecution WithTail(Program, 1), WithoutTail(Program, 0);
  return (TailCount == 1 || TailCount == 2) && Program.TempCount == 3 &&
         WithTail.Run() && WithoutTail.Run() &&
         WithTail.Output == std::vector<uint32_t>{7} &&
         WithoutTail.Output == std::vector<uint32_t>{0};
}

constexpr uint32_t SharedTailBaseTemps = 5;
constexpr uint32_t SharedTailLength = 20;

std::vector<FlowBlock> MakeSharedTailGraph(bool InvertOuter, bool InvertInner,
                                          bool UsesQuad) {
  enum BlockID { Head, Fork, Via, Skip, Tail, JoinLoop, ExitBlock, BlockCount };
  std::vector<FlowBlock> Blocks(BlockCount);
  Blocks[Head].Kind = FlowKind::Branch;
  Blocks[Head].Condition = Register(0);
  Blocks[Head].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(11)}},
      {D3D10_SB_OPCODE_MOV, {Register(2), Literal(29)}},
      {D3D10_SB_OPCODE_MOV, {Register(3), Literal(47)}},
      {D3D10_SB_OPCODE_MOV, {Register(4), Literal(0)}}};
  Blocks[Head].Edges = {{Fork, {}}, {Tail, {{1, 2}, {2, 1}}}};
  Blocks[Fork].Kind = FlowKind::Branch;
  Blocks[Fork].Condition = Register(0, 1);
  Blocks[Fork].Edges = {{Via, {}}, {Skip, {}}};
  if (InvertOuter)
    std::swap(Blocks[Head].Edges[0], Blocks[Head].Edges[1]);
  if (InvertInner)
    std::swap(Blocks[Fork].Edges[0], Blocks[Fork].Edges[1]);
  Blocks[Via].Kind = FlowKind::Jump;
  Blocks[Via].Edges = {{Tail, {{1, 3}, {2, 1}, {3, 2}}}};
  Blocks[Skip].Kind = FlowKind::Jump;
  Blocks[Skip].Edges = {{JoinLoop, {}}};
  Blocks[Tail].Kind = FlowKind::Jump;
  Blocks[Tail].Edges = {{JoinLoop, {}}};
  Blocks[Tail].Instructions.assign(SharedTailLength,
      {D3D10_SB_OPCODE_IADD, {Register(1), Register(1), Literal(1)}});
  if (UsesQuad)
    Blocks[Tail].Instructions.front() =
        {D3D10_SB_OPCODE_DERIV_RTX, {Register(1), Register(1)}};

  // A back edge selects SCC structuring. The loop exits on its first check.
  Blocks[JoinLoop].Kind = FlowKind::Branch;
  Blocks[JoinLoop].Condition = Register(4);
  Blocks[JoinLoop].Edges = {{JoinLoop, {}}, {ExitBlock, {}}};
  Blocks[ExitBlock].Instructions = {
      Observe(Register(1)), Observe(Register(2)), Observe(Register(3))};
  return Blocks;
}

bool CheckGuardedSharedTail(bool InvertOuter, bool InvertInner,
                            bool UsesQuad = false) {
  const auto Blocks = MakeSharedTailGraph(InvertOuter, InvertInner, UsesQuad);
  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = SharedTailBaseTemps;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error)) {
    std::cerr << Error << '\n';
    return false;
  }
  const auto &Code = Program.Instructions;
  const auto Count = [&](uint32_t Opcode) {
    return std::count_if(Code.begin(), Code.end(),
        [Opcode](const Instruction &Inst) { return Inst.Opcode == Opcode; });
  };
  if (UsesQuad)
    return Count(D3D10_SB_OPCODE_DERIV_RTX) == 1 &&
           Program.TempCount <= SharedTailBaseTemps + 2;
  if (Count(D3D10_SB_OPCODE_IADD) != SharedTailLength ||
      Program.TempCount != SharedTailBaseTemps + 1)
    return false;

  const uint32_t TakeFork = !InvertOuter, Direct = InvertOuter;
  const uint32_t TakeTail = !InvertInner, SkipTail = InvertInner;
  TraceExecution DirectTrue(Program, RegisterLanes{Direct, 1});
  TraceExecution DirectFalse(Program, RegisterLanes{Direct, 0});
  TraceExecution ViaFork(Program, RegisterLanes{TakeFork, TakeTail});
  TraceExecution Bypass(Program, RegisterLanes{TakeFork, SkipTail});
  const std::vector<uint32_t> ExpectedDirect{29 + SharedTailLength, 11, 47};
  return DirectTrue.Run() && DirectTrue.Output == ExpectedDirect &&
         DirectFalse.Run() && DirectFalse.Output == ExpectedDirect &&
         ViaFork.Run() &&
         ViaFork.Output == std::vector<uint32_t>{47 + SharedTailLength, 11, 29} &&
         Bypass.Run() && Bypass.Output == std::vector<uint32_t>{11, 29, 47};
}

bool CheckSharedTailSwitch() {
  constexpr uint32_t CaseValue = 7, UnmatchedNonzero = 3;
  auto Blocks = MakeSharedTailGraph(false, false, false);
  Blocks.front().Kind = FlowKind::Switch;
  Blocks.front().Edges[1].CaseValue = CaseValue;

  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = SharedTailBaseTemps;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error)) {
    std::cerr << Error << '\n';
    return false;
  }

  TraceExecution Zero(Program, RegisterLanes{0, 0});
  TraceExecution Matched(Program, RegisterLanes{CaseValue, 0});
  TraceExecution Unmatched(Program, RegisterLanes{UnmatchedNonzero, 0});
  TraceExecution UnmatchedViaFork(Program, RegisterLanes{UnmatchedNonzero, 1});
  const std::vector<uint32_t> ExpectedDefault{11, 29, 47};
  return Zero.Run() && Zero.Output == ExpectedDefault &&
         Matched.Run() && Matched.Output == std::vector<uint32_t>{29 + SharedTailLength, 11, 47} &&
         Unmatched.Run() && Unmatched.Output == ExpectedDefault &&
         UnmatchedViaFork.Run() &&
         UnmatchedViaFork.Output == std::vector<uint32_t>{47 + SharedTailLength, 11, 29};
}

bool CheckExpandedNestedLoop() {
  enum BlockID { Entry, Outer, Reset, InnerA, InnerB, ExitA, ExitB, Latch, Done, Count };
  constexpr uint32_t BaseTemps = 6;
  std::vector<FlowBlock> Blocks(Count);
  Blocks[Entry].Kind = FlowKind::Jump;
  Blocks[Entry].Edges = {{Outer, {}}};
  Blocks[Entry].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(0)}}};
  Blocks[Outer].Kind = FlowKind::Branch;
  Blocks[Outer].Condition = Register(5);
  Blocks[Outer].Edges = {{Reset, {}}, {Done, {}}};
  Blocks[Outer].Instructions = {
      {D3D10_SB_OPCODE_ULT, {Register(5), Register(1), Register(0)}}};
  Blocks[Reset].Kind = FlowKind::Jump;
  Blocks[Reset].Edges = {{InnerA, {}}};
  Blocks[Reset].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(2), Literal(11)}},
      {D3D10_SB_OPCODE_MOV, {Register(3), Literal(29)}},
      {D3D10_SB_OPCODE_MOV, {Register(4), Literal(0)}}};
  Blocks[InnerA].Kind = FlowKind::Branch;
  Blocks[InnerA].Condition = Register(4);
  Blocks[InnerA].Edges = {{ExitA, {{2, 3}, {3, 2}}}, {InnerB, {}}};
  Blocks[InnerB].Kind = FlowKind::Branch;
  Blocks[InnerB].Condition = Register(1);
  Blocks[InnerB].Edges = {{ExitB, {{2, Literal(47), 0}, {3, 2}}},
                          {InnerA, {{4, Literal(1), 0}}}};
  for (const auto Block : {ExitA, ExitB}) {
    Blocks[Block].Kind = FlowKind::Jump;
    Blocks[Block].Edges = {{Latch, {}}};
    Blocks[Block].Instructions = {Observe(Register(2)), Observe(Register(3))};
  }
  Blocks[Latch].Kind = FlowKind::Jump;
  Blocks[Latch].Edges = {{Outer, {}}};
  Blocks[Latch].Instructions = {
      {D3D10_SB_OPCODE_IADD, {Register(1), Register(1), Literal(1)}}};
  Blocks[Done].Instructions = {Observe(Register(1))};

  MirProgram Program;
  Program.TempCount = BaseTemps;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto &Code = Program.Instructions;
  const auto CountOpcode = [&](uint32_t Opcode) {
    return std::count_if(Code.begin(), Code.end(),
        [Opcode](const Instruction &Inst) { return Inst.Opcode == Opcode; });
  };
  if (CountOpcode(D3D10_SB_OPCODE_LOOP) != 2 ||
      CountOpcode(D3D10_SB_OPCODE_SWITCH) != 0 || Program.TempCount != BaseTemps + 1)
    return false;

  const std::vector<uint32_t> Expected[]{
      {0}, {29, 11, 1}, {29, 11, 47, 11, 2}, {29, 11, 47, 11, 47, 11, 3}};
  for (unsigned I = 0; I < std::size(Expected); ++I) {
    TraceExecution Trace(Program, I);
    if (!Trace.Run() || Trace.Output != Expected[I])
      return false;
  }
  return true;
}

bool CheckNestedFragments(bool InnerTaken) {
  enum BlockID { Head, Inner, One, Two, InnerJoin, TrueTail,
                 FalseTail, Join, Loop, Exit, BlockCount };
  constexpr uint32_t State = 6;
  std::vector<FlowBlock> Blocks(BlockCount);
  Blocks[Head].Kind = FlowKind::Branch;
  Blocks[Head].Condition = Register(0);
  Blocks[Head].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(11)}},
      {D3D10_SB_OPCODE_MOV, {Register(2), Literal(29)}},
      {D3D10_SB_OPCODE_MOV, {Register(4), Literal(InnerTaken)}}};
  Blocks[Head].Edges = {{Inner, {{1, 2}, {2, 1}}},
                        {FalseTail, {{1, 2}, {3, 1}}}};
  Blocks[Inner].Kind = FlowKind::Branch;
  Blocks[Inner].Condition = Register(4);
  Blocks[Inner].Edges = {{One, {}}, {Two, {}}};
  for (const auto ID : {One, Two}) {
    Blocks[ID].Kind = FlowKind::Jump;
    Blocks[ID].Edges = {{InnerJoin, {{1, 3}, {3, 2}}}};
    Blocks[ID].Instructions = {
        {D3D10_SB_OPCODE_IADD,
         {Register(3), Register(1), Literal(ID == One ? 1 : 2)}}};
  }
  Blocks[InnerJoin].Kind = FlowKind::Jump;
  Blocks[InnerJoin].Edges = {{TrueTail, {}}};
  Blocks[TrueTail].Kind = FlowKind::Jump;
  Blocks[TrueTail].Edges = {{Join, {}}};
  Blocks[FalseTail].Kind = FlowKind::Jump;
  Blocks[FalseTail].Edges = {{Join, {{3, 2}}}};
  Blocks[FalseTail].Instructions = {
      {D3D10_SB_OPCODE_IADD, {Register(1), Register(3), Literal(3)}}};
  Blocks[Join].Kind = FlowKind::Jump;
  Blocks[Join].Edges = {{Loop, {}}};
  Blocks[Join].Instructions = {
      {D3D10_SB_OPCODE_IADD, {Register(1), Register(1), Literal(4)}}};

  // A loop forces SCC region structuring and provides an observable join for the
  // nested arms and their exclusive straight-line tails.
  Blocks[Loop].Kind = FlowKind::Branch;
  Blocks[Loop].Condition = Register(5);
  Blocks[Loop].Edges = {{Loop, {}}, {Exit, {}}};
  Blocks[Loop].Instructions = {Observe(Register(1)), Observe(Register(3))};

  MirProgram Program;
  Program.TempCount = State;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto &Code = Program.Instructions;
  const auto Guards = std::count_if(Code.begin(), Code.end(),
      [](const Instruction &Inst) { return Inst.Opcode == D3D10_SB_OPCODE_ULT; });
  TraceExecution TruePath(Program, 1), FalsePath(Program, 0);
  return Guards == 0 && TruePath.Run() && FalsePath.Run() &&
         TruePath.Output == std::vector<uint32_t>{InnerTaken ? 34u : 35u, 11} &&
         FalsePath.Output == std::vector<uint32_t>{18, 29};
}

bool CheckTailSinkAndMovcFlatten() {
  // Diamond whose both arms assign identical r2=99 in different orders plus
  // distinct r1 values: Rule 3a sinks `mov r2, 99` past endif, Rule 4 hoists
  // one arm, and Rule 1c flattens `if { mov r1, 10 } endif` + folds `mov r1, 20`
  // into a single `movc r1, r0, 10, 20` with ZERO branches!
  std::vector<FlowBlock> Blocks(4);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = Register(0);
  Blocks[0].Edges = {{1, {}}, {2, {}}};
  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Edges = {{3, {{2, Literal(99)}, {1, Literal(10)}}}};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Edges = {{3, {{1, Literal(20)}, {2, Literal(99)}}}};
  Blocks[3].Kind = FlowKind::Return;
  Blocks[3].Instructions = {Observe(Register(1)), Observe(Register(2))};

  MirProgram Program;
  Program.TempCount = 3;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error)) {
    std::cerr << Error << '\n';
    return false;
  }
  const auto &Code = Program.Instructions;
  const auto Ifs = std::count_if(Code.begin(), Code.end(), [](const Instruction &I) {
    return I.Opcode == D3D10_SB_OPCODE_IF;
  });
  const auto Movcs = std::count_if(Code.begin(), Code.end(), [](const Instruction &I) {
    return I.Opcode == D3D10_SB_OPCODE_MOVC;
  });
  TraceExecution T1(Program, 1), T0(Program, 0);
  if (Ifs != 0 || Movcs != 1 || !T1.Run() || !T0.Run() ||
      T1.Output != std::vector<uint32_t>{10, 99} ||
      T0.Output != std::vector<uint32_t>{20, 99}) {
    std::cerr << "Expected 0 ifs and 1 movc, got ifs=" << Ifs
              << " movcs=" << Movcs << '\n';
    return false;
  }
  return true;
}

bool CheckSwitchFallthroughGrouping() {
  // 3 cases share an identical target containing a pixel quad op (DERIV_RTX),
  // while default jumps straight to Join with no copies. CollapsedFlow::Switch
  // must group the 3 cases via SM5 fallthrough labels so DERIV_RTX is emitted
  // ONCE and the empty default is omitted.
  enum BlockID { Entry, SharedArm, Join, Loop, Exit, BlockCount };
  std::vector<FlowBlock> Blocks(BlockCount);
  Blocks[Entry].Kind = FlowKind::Switch;
  Blocks[Entry].Condition = Register(0);
  Blocks[Entry].Edges = {{Join, {}}, {SharedArm, {}, 1}, {SharedArm, {}, 2}, {SharedArm, {}, 3}};
  Blocks[SharedArm].Kind = FlowKind::Jump;
  Blocks[SharedArm].Edges = {{Join, {}}};
  Blocks[SharedArm].Instructions = {
      {D3D10_SB_OPCODE_DERIV_RTX, {Register(1), Register(0)}}};
  // Trailing loop forces SCC/CollapsedFlow path instead of EmitStructured.
  Blocks[Join].Kind = FlowKind::Jump;
  Blocks[Join].Edges = {{Loop, {}}};
  Blocks[Loop].Kind = FlowKind::Branch;
  Blocks[Loop].Condition = Register(2);
  Blocks[Loop].Edges = {{Loop, {}}, {Exit, {}}};
  Blocks[Exit].Instructions = {Observe(Register(1))};

  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 3;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error)) {
    std::cerr << Error << '\n';
    return false;
  }
  const auto &Code = Program.Instructions;
  const auto Count = [&](uint32_t Op) {
    return std::count_if(Code.begin(), Code.end(),
                         [Op](const Instruction &I) { return I.Opcode == Op; });
  };
  return Count(D3D10_SB_OPCODE_DERIV_RTX) == 1 &&
         Count(D3D10_SB_OPCODE_CASE) == 3 &&
         Count(D3D10_SB_OPCODE_DEFAULT) == 0 &&
         Count(D3D10_SB_OPCODE_ULT) == 0;
}

bool CheckHybridLocalDispatchLoopWithQuadLoop() {
  // A pixel shader with BOTH:
  // (1) an unstructured non-quad loop (switch + backedge) requiring a local
  //     dispatch loop, AND
  // (2) a structured branched loop containing a pixel quad op (DERIV_RTX).
  // Hybrid CollapsedFlow must keep the outer shader structured (only 1 ULT for
  // the local dispatch loop) and lower the quad loop natively.
  enum BlockID { Entry, SwLoop, SwBody, Mid, QHead, QArm, QLatch, Exit, BlockCount };
  std::vector<FlowBlock> Blocks(BlockCount);
  Blocks[Entry].Kind = FlowKind::Jump;
  Blocks[Entry].Edges = {{SwLoop, {}}};
  Blocks[Entry].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(0)}},
      {D3D10_SB_OPCODE_MOV, {Register(2), Literal(0)}}};
  // Loop 1: switch-headed loop (cannot use ExpandedIteration -> LocalDispatchLoop).
  Blocks[SwLoop].Kind = FlowKind::Switch;
  Blocks[SwLoop].Condition = Register(0);
  Blocks[SwLoop].Edges = {{SwBody, {}}, {Mid, {}, 0}};
  Blocks[SwBody].Kind = FlowKind::Jump;
  Blocks[SwBody].Edges = {{SwLoop, {}}};
  Blocks[SwBody].Instructions = {
      {D3D10_SB_OPCODE_IADD, {Register(0), Register(0), Literal(UINT32_MAX)}},
      {D3D10_SB_OPCODE_IADD, {Register(1), Register(1), Literal(1)}}};
  // Acyclic bridge between loops.
  Blocks[Mid].Kind = FlowKind::Jump;
  Blocks[Mid].Edges = {{QHead, {}}};
  // Loop 2: branched loop with a pixel quad op (DERIV_RTX).
  Blocks[QHead].Kind = FlowKind::Branch;
  Blocks[QHead].Condition = Register(1);
  Blocks[QHead].Edges = {{QArm, {}}, {QLatch, {}}};
  Blocks[QArm].Kind = FlowKind::Jump;
  Blocks[QArm].Edges = {{QLatch, {}}};
  Blocks[QArm].Instructions = {
      {D3D10_SB_OPCODE_DERIV_RTX, {Register(2), Register(1)}}};
  Blocks[QLatch].Kind = FlowKind::Branch;
  Blocks[QLatch].Condition = Register(2);
  Blocks[QLatch].Edges = {{QHead, {}}, {Exit, {}}};
  Blocks[Exit].Instructions = {Observe(Register(2))};

  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 3;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error)) {
    std::cerr << Error << '\n';
    return false;
  }
  const auto &Code = Program.Instructions;
  const auto Count = [&](uint32_t Op) {
    return std::count_if(Code.begin(), Code.end(),
                         [Op](const Instruction &I) { return I.Opcode == Op; });
  };
  return Count(D3D10_SB_OPCODE_LOOP) == 2 &&
         Count(D3D10_SB_OPCODE_DERIV_RTX) == 1 &&
         Count(D3D10_SB_OPCODE_ULT) == 1;
}

bool CheckHoistRespectsDestRelativeIndex() {
  // Block 0: r1 = 0; if (r0) -> Block 1 else -> Block 2
  // Block 1 (small arm): mov r1, 1; jump Block 3
  // Block 2 (other arm): mov x0[r1.x], 77; mov r1, 2; jump Block 3
  // Block 3: ret
  // Hoisting "mov r1, 1" before the IF would clobber x0[r1.x] in Block 2 from
  // x0[0] to x0[1]!
  std::vector<FlowBlock> Blocks(4);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = Register(0);
  Blocks[0].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(0)}}};
  Blocks[0].Edges = {{1, {}}, {2, {}}};

  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(1)}}};
  Blocks[1].Edges = {{3, {}}};

  Operand XDst{OperandKind::IndexableTemp, 0, 0, 1, 0};
  XDst.RelativeRegister = 1;
  XDst.RelativeLane = 0;
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Instructions = {
      {D3D10_SB_OPCODE_MOV, {XDst, Literal(77)}},
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(2)}}};
  Blocks[2].Edges = {{3, {}}};

  Blocks[3].Kind = FlowKind::Return;
  Blocks[3].Instructions = {Observe(Register(1))};

  MirProgram Program;
  Program.ShaderStage = Stage::Pixel;
  Program.TempCount = 2;
  Program.IndexableTemps.push_back({0, 4, 1});
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error)) {
    std::cerr << Error << '\n';
    return false;
  }
  // Find the first IF; there must NOT be a "mov r1, 1" before it!
  for (const auto &Inst : Program.Instructions) {
    if (Inst.Opcode == D3D10_SB_OPCODE_IF)
      return true;
    if (Inst.Opcode == D3D10_SB_OPCODE_MOV && Inst.Operands.size() == 2 &&
        Inst.Operands[0].Kind == OperandKind::Temp &&
        Inst.Operands[0].Index == 1 &&
        Inst.Operands[1].Kind == OperandKind::Immediate &&
        Inst.Operands[1].Index == 1)
      return false;
  }
  return false;
}

bool CheckEarlyExitAndLatchContinueLoop() {
  // Models a loop with an early break arm and a skip-to-latch arm both
  // bypassing a large shared body (like Unity's clustered light loop).
  // Both arms terminate or fall through to Body, so ExpandedIteration must
  // emit Body once with zero switch/case dispatch fallback.
  enum BlockID { Entry, Header, Left, Right, Body, Latch, ExitBlk, BlockCount };
  std::vector<FlowBlock> Blocks(BlockCount);
  Blocks[Entry].Kind = FlowKind::Jump;
  Blocks[Entry].Edges = {{Header, {}}};
  Blocks[Entry].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(2)}}};
  Blocks[Header].Kind = FlowKind::Branch;
  Blocks[Header].Condition = Register(0, 0);
  Blocks[Header].Edges = {{Left, {}}, {Right, {}}};
  // Left skips Body straight to Latch when r0.y != 0, otherwise falls to Body.
  Blocks[Left].Kind = FlowKind::Branch;
  Blocks[Left].Condition = Register(0, 1);
  Blocks[Left].Instructions = {Observe(Literal(10))};
  Blocks[Left].Edges = {{Latch, {}}, {Body, {}}};
  // Right breaks immediately when r0.z != 0, otherwise falls to Body.
  Blocks[Right].Kind = FlowKind::Branch;
  Blocks[Right].Condition = Register(0, 2);
  Blocks[Right].Instructions = {Observe(Literal(20))};
  Blocks[Right].Edges = {{ExitBlk, {}}, {Body, {}}};
  // Large shared Body (exceeds MaxRepeatedBodyInstructions if duplicated).
  Blocks[Body].Kind = FlowKind::Jump;
  Blocks[Body].Instructions.push_back(Observe(Literal(30)));
  for (unsigned I = 0; I < 200; ++I)
    Blocks[Body].Instructions.push_back(
        {D3D10_SB_OPCODE_IADD, {Register(2), Register(2), Literal(1)}});
  Blocks[Body].Edges = {{Latch, {}}};
  // Small pure terminating Latch: increments r2 by 100, decrements r1, and
  // loops while r1 != 0.
  Blocks[Latch].Kind = FlowKind::Branch;
  Blocks[Latch].Condition = Register(1);
  Blocks[Latch].Instructions = {
      {D3D10_SB_OPCODE_IADD, {Register(2), Register(2), Literal(100)}},
      {D3D10_SB_OPCODE_IADD, {Register(1), Register(1), Literal(UINT32_MAX)}}};
  Blocks[Latch].Edges = {{Header, {}}, {ExitBlk, {}}};
  Blocks[ExitBlk].Instructions = {Observe(Register(2)), Observe(Literal(99))};

  MirProgram Program;
  Program.TempCount = 3;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto Count = [&](uint32_t Op) {
    return std::count_if(Program.Instructions.begin(), Program.Instructions.end(),
                         [Op](const Instruction &I) { return I.Opcode == Op; });
  };
  if (Count(D3D10_SB_OPCODE_LOOP) != 1 || Count(D3D10_SB_OPCODE_SWITCH) != 0)
    return false;
  TraceExecution LeftSkip(Program, RegisterLanes{1, 1, 0, 0});
  TraceExecution LeftBody(Program, RegisterLanes{1, 0, 0, 0});
  TraceExecution RightBreak(Program, RegisterLanes{0, 0, 1, 0});
  TraceExecution RightBody(Program, RegisterLanes{0, 0, 0, 0});
  return LeftSkip.Run() &&
         LeftSkip.Output == std::vector<uint32_t>{10, 10, 200, 99} &&
         LeftBody.Run() &&
         LeftBody.Output == std::vector<uint32_t>{10, 30, 10, 30, 600, 99} &&
         RightBreak.Run() &&
         RightBreak.Output == std::vector<uint32_t>{20, 0, 99} &&
         RightBody.Run() &&
         RightBody.Output == std::vector<uint32_t>{20, 30, 20, 30, 600, 99};
}

bool CheckHoistRespectsDualDestRead() {
  // UDIV with null first dest and active second dest reads r1.x and r2.x;
  // hoisting "mov r1.x, 1" from Else before IF would corrupt UDIV's input.
  std::vector<FlowBlock> Blocks(4);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = Register(0);
  Blocks[0].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(29)}},
      {D3D10_SB_OPCODE_MOV, {Register(2), Literal(10)}}};
  Blocks[0].Edges = {{1, {}}, {2, {}}};
  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Instructions = {
      {D3D10_SB_OPCODE_UDIV,
       {{OperandKind::Null, 0, 0, 0, 0}, Register(3), Register(1), Register(2)}},
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(7)}}};
  Blocks[1].Edges = {{3, {}}};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Register(1), Literal(1)}},
      {D3D10_SB_OPCODE_MOV, {Register(3), Literal(0)}}};
  Blocks[2].Edges = {{3, {}}};
  Blocks[3].Instructions = {Observe(Register(3)), Observe(Register(1))};

  MirProgram Program;
  Program.TempCount = 4;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  // "mov r1.x, 1" must NOT appear before the IF.
  for (const auto &Inst : Program.Instructions) {
    if (Inst.Opcode == D3D10_SB_OPCODE_IF)
      return true;
    if (Inst.Opcode == D3D10_SB_OPCODE_MOV && Inst.Operands.size() == 2 &&
        Inst.Operands[0].Kind == OperandKind::Temp &&
        Inst.Operands[0].Index == 1 &&
        Inst.Operands[1].Kind == OperandKind::Immediate &&
        Inst.Operands[1].Index == 1)
      return false;
  }
  return false;
}

bool CheckIfDiscardFold() {
  std::vector<FlowBlock> Blocks(3);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = Register(0);
  Blocks[0].Edges = {{1, {}}, {2, {}}};
  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Edges = {{2, {}}};
  Blocks[1].Instructions = {{D3D10_SB_OPCODE_DISCARD, {Literal(0xffffffffu)}, true}};
  Blocks[2].Kind = FlowKind::Return;
  Blocks[2].Instructions = {Observe(Register(0))};

  MirProgram Program;
  Program.TempCount = 1;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  unsigned Ifs = 0, Discards = 0;
  for (const auto &I : Program.Instructions) {
    Ifs += I.Opcode == D3D10_SB_OPCODE_IF;
    if (I.Opcode == D3D10_SB_OPCODE_DISCARD && I.TestNonZero &&
        I.Operands.size() == 1 && I.Operands[0].Kind == OperandKind::Temp &&
        I.Operands[0].Index == 0)
      ++Discards;
  }
  return Ifs == 0 && Discards == 1;
}

bool CheckEarlyRetSharedTail() {
  enum BlockID { Head, Fork, EarlyExit, Tail, EmptyRet, Count };
  std::vector<FlowBlock> Blocks(Count);
  Blocks[Head].Kind = FlowKind::Branch;
  Blocks[Head].Condition = Register(0);
  Blocks[Head].Edges = {{Fork, {}}, {Tail, {}}};
  Blocks[Fork].Kind = FlowKind::Branch;
  Blocks[Fork].Condition = Register(0, 1);
  Blocks[Fork].Edges = {{EarlyExit, {}}, {Tail, {}}};
  Blocks[EarlyExit].Kind = FlowKind::Jump;
  Blocks[EarlyExit].Edges = {{EmptyRet, {}}};
  Blocks[EarlyExit].Instructions = {Observe(Literal(99))};
  Blocks[Tail].Kind = FlowKind::Jump;
  Blocks[Tail].Edges = {{EmptyRet, {}}};
  Blocks[Tail].Instructions.assign(
      SharedTailLength,
      {D3D10_SB_OPCODE_IADD, {Register(1), Register(1), Literal(1)}});
  Blocks[Tail].Instructions.push_back(Observe(Register(1)));
  Blocks[EmptyRet].Kind = FlowKind::Return;

  MirProgram Program;
  Program.ShaderStage = Stage::Vertex;
  Program.TempCount = 2;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  const auto CountOp = [&](uint32_t Op) {
    return std::count_if(Program.Instructions.begin(), Program.Instructions.end(),
                         [Op](const Instruction &I) { return I.Opcode == Op; });
  };
  // Direct early RET avoids allocating a scratch predicate temp and keeps Tail un-duplicated.
  if (Program.TempCount != 2 || CountOp(D3D10_SB_OPCODE_IADD) != SharedTailLength ||
      CountOp(D3D10_SB_OPCODE_RET) < 2)
    return false;
  TraceExecution Early(Program, RegisterLanes{1, 1});
  TraceExecution InnerSkip(Program, RegisterLanes{1, 0});
  TraceExecution OuterSkip(Program, RegisterLanes{0, 1});
  const std::vector<uint32_t> TailExpected{SharedTailLength};
  return Early.Run() && Early.Output == std::vector<uint32_t>{99} &&
         InnerSkip.Run() && InnerSkip.Output == TailExpected &&
         OuterSkip.Run() && OuterSkip.Output == TailExpected;
}

bool CheckDropRedundantTrailingMov() {
  // Dominating mov r1.xyzw, r2.xyzw before if_nz makes trailing mov r1.yzw, r2.xyzw redundant
  // when the if body only modifies r1.x.
  std::vector<FlowBlock> Blocks(3);
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = Register(0);
  Blocks[0].Instructions = {
      {D3D10_SB_OPCODE_MOV, {Operand{OperandKind::Temp, 2, 0, 0xf, 0xe4}, Literal(7)}},
      {D3D10_SB_OPCODE_MOV, {Operand{OperandKind::Temp, 1, 0, 0xf, 0xe4},
                             Operand{OperandKind::Temp, 2, 0, 0xf, 0xe4}}}};
  Blocks[0].Edges = {{1, {}}, {2, {}}};
  Blocks[1].Kind = FlowKind::Jump;
  Blocks[1].Instructions = {
      {D3D10_SB_OPCODE_IADD, {Register(1, 0), Register(2, 0), Literal(1)}},
      {D3D10_SB_OPCODE_MOV, {Operand{OperandKind::Temp, 1, 0, 0xe, 0xe4},
                             Operand{OperandKind::Temp, 2, 0, 0xe, 0xe4}}}};
  Blocks[1].Edges = {{2, {}}};
  Blocks[2].Kind = FlowKind::Return;
  Blocks[2].Instructions = {Observe(Register(1, 0)), Observe(Register(1, 1))};

  MirProgram Program;
  Program.TempCount = 3;
  std::string Error;
  if (!LowerControlFlow(Blocks, Program, Error))
    return false;
  for (const auto &I : Program.Instructions)
    if (I.Opcode == D3D10_SB_OPCODE_MOV && I.Operands.size() == 2 &&
        I.Operands[0].Kind == OperandKind::Temp && I.Operands[0].Index == 1 &&
        I.Operands[0].Mask == 0xe)
      return false;
  TraceExecution Taken(Program, 1), Skipped(Program, 0);
  return Taken.Run() && Taken.Output == std::vector<uint32_t>{8, 7} &&
         Skipped.Run() && Skipped.Output == std::vector<uint32_t>{7, 7};
}

} // namespace

int main() {
  if (!CheckSwitchFallthroughGrouping() ||
      !CheckHybridLocalDispatchLoopWithQuadLoop() ||
      !CheckHoistRespectsDestRelativeIndex() ||
      !CheckHoistRespectsDualDestRead() ||
      !CheckEarlyExitAndLatchContinueLoop() ||
      !CheckIfDiscardFold() ||
      !CheckEarlyRetSharedTail() ||
      !CheckDropRedundantTrailingMov()) {
    std::cerr << "Incorrect switch grouping or hybrid local dispatch loop\n";
    return 1;
  }
  if (!CheckTailSinkAndMovcFlatten()) {
    std::cerr << "Incorrect tail-sink / movc-flatten optimization\n";
    return 1;
  }
  if (!CheckExpandedNestedLoop()) {
    std::cerr << "Incorrect expanded nested-loop control flow\n";
    return 1;
  }
  if (!CheckGuardedSharedTail(false, false) || !CheckGuardedSharedTail(false, true) ||
      !CheckGuardedSharedTail(true, false) || !CheckGuardedSharedTail(true, true) ||
      !CheckGuardedSharedTail(false, false, true)) {
    std::cerr << "Incorrect guarded shared tail\n";
    return 1;
  }
  if (!CheckSharedTailSwitch()) {
    std::cerr << "Incorrect shared-tail switch selection\n";
    return 1;
  }
  if (!CheckNestedFragments(true) || !CheckNestedFragments(false) ||
      !CheckStructuredArms(true, false) || !CheckStructuredArms(false, true) ||
      !CheckStructuredArms(true, true) || !CheckStructuredArms(true, false, true)) {
    std::cerr << "Incorrect MIR structured branch arms\n";
    return 1;
  }
  if (!CheckConditionalDiscard(true, true) || !CheckConditionalDiscard(false, true) ||
      !CheckConditionalDiscard(true, false) ||
      !CheckConditionalDiscard(true, true, true) || !CheckBranch() ||
      !CheckCriticalEdgeCopies() || !CheckSwitchAndBackEdge() ||
      !CheckQuadJoin() || !CheckQuadAfterLoop() || !CheckNativeQuadLoop() ||
      !CheckNativeLinearExits() || !CheckBranchedLoop() ||
      !CheckGatherInBranchedLoop() ||
      !CheckQuadNeedsStructuredControlFlow() ||
      !CheckTransactionalFailure() || !CheckSharedTail(false) || !CheckSharedTail(true)) {
    std::cerr << "Incorrect MIR control-flow lowering\n";
    return 1;
  }
}
