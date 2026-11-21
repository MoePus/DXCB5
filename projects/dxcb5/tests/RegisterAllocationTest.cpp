// Distributed under the University of Illinois Open Source License.
#include "RegAlloc/LaneInterference.h"
#include "dxcb5/InstructionInfo.h"
#include "dxcb5/RegisterAllocation.h"

#include <iostream>

using namespace dxcb5;

namespace {

Instruction Mov(Operand Destination, Operand Source) {
  return {D3D10_SB_OPCODE_MOV, {Destination, Source}};
}

bool SameSlot(const Operand &A, const Operand &B) {
  return A.Kind == OperandKind::Temp && B.Kind == OperandKind::Temp &&
         A.Index == B.Index && (A.Mask & B.Mask) != 0;
}

bool CheckStraightLineReuse() {
  std::vector<FlowBlock> Blocks(1);
  Blocks[0].Instructions = {
      Mov({OperandKind::Temp, 5}, {OperandKind::Immediate, 11}),
      Mov({OperandKind::Output, 0}, {OperandKind::Temp, 5}),
      Mov({OperandKind::Temp, 12}, {OperandKind::Immediate, 22}),
      Mov({OperandKind::Output, 1}, {OperandKind::Temp, 12})};
  MirProgram Program;
  Program.TempCount = 17;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  const auto &Code = Blocks[0].Instructions;
  return Program.TempCount == 1 && Code[0].Operands[0].Index == 0 &&
         Code[1].Operands[1].Index == 0 && Code[2].Operands[0].Index == 0 &&
         Code[3].Operands[1].Index == 0;
}

bool CheckLiveAcrossBackEdge() {
  std::vector<FlowBlock> Blocks(4);
  Blocks[0].Instructions = {
      Mov({OperandKind::Temp, 7, 0, 2}, {OperandKind::Immediate, 11})};
  Blocks[0].Kind = FlowKind::Jump;
  Blocks[0].Edges = {{1, {}}};
  Operand Indirect{OperandKind::ConstantBuffer, 2, 3};
  Indirect.RelativeRegister = 7;
  Indirect.RelativeLane = 1;
  Blocks[1].Instructions = {Mov({OperandKind::Output, 0}, Indirect)};
  Blocks[1].Kind = FlowKind::Branch;
  Blocks[1].Condition = {OperandKind::Immediate, 1};
  Blocks[1].Edges = {{2, {}}, {3, {}}};
  Blocks[2].Instructions = {
      Mov({OperandKind::Temp, 9}, {OperandKind::Immediate, 22}),
      Mov({OperandKind::Output, 1}, {OperandKind::Temp, 9}),
      Mov({OperandKind::Temp, 7}, {OperandKind::Immediate, 33})};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Edges = {{1, {}}};

  MirProgram Program;
  Program.TempCount = 10;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  const Operand PersistentY = Blocks[0].Instructions[0].Operands[0];
  const Operand PersistentX = Blocks[2].Instructions[2].Operands[0];
  const Operand DeadNext = Blocks[2].Instructions[0].Operands[0];
  const Operand Result = Blocks[1].Instructions[0].Operands[1];
  // t7.y (live across the backedge) and t9.x pack into 1 physical register on
  // disjoint lanes; t7.x is written only after t9 dies so exact slot liveness
  // may reuse t9's lane for t7.x.
  return Program.TempCount == 1 &&
         PersistentY.Index == PersistentX.Index &&
         Result.RelativeRegister == PersistentY.Index &&
         ((1u << Result.RelativeLane) == PersistentY.Mask) &&
         !SameSlot(PersistentY, DeadNext);
}

bool CheckLiveThroughReorderedBlocks() {
  std::vector<FlowBlock> Blocks(4);
  const Operand WriteY{OperandKind::Temp, 5, 0, 0b0010};
  auto ReadY = WriteY;
  ReadY.Swizzle = 0x55;
  auto OtherY = WriteY;
  OtherY.Index = 8;
  auto ReadOtherY = ReadY;
  ReadOtherY.Index = OtherY.Index;

  // The live value flows from block three through block two to block one.
  Blocks[0].Kind = FlowKind::Jump;
  Blocks[0].Edges = {{3, {}}};
  Blocks[1].Instructions = {Mov({OperandKind::Output, 0}, ReadY)};
  Blocks[2].Kind = FlowKind::Jump;
  Blocks[2].Edges = {{1, {}}};
  Blocks[3].Kind = FlowKind::Jump;
  Blocks[3].Edges = {{2, {}}};
  Blocks[3].Instructions = {
      Mov(WriteY, {OperandKind::Immediate, 11}),
      Mov(OtherY, {OperandKind::Immediate, 22}),
      Mov({OperandKind::Output, 1}, ReadOtherY)};

  MirProgram Program;
  Program.TempCount = 9;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  const Operand Live = Blocks[3].Instructions[0].Operands[0];
  const Operand Other = Blocks[3].Instructions[1].Operands[0];
  const Operand UseLive = Blocks[1].Instructions[0].Operands[1];
  const Operand UseOther = Blocks[3].Instructions[2].Operands[1];
  // Two interfering scalars pack into 1 physical temp on disjoint lanes.
  return Program.TempCount == 1 && !SameSlot(Live, Other) &&
         UseLive.Index == Live.Index &&
         (1u << (UseLive.Swizzle & 3u)) == Live.Mask &&
         UseOther.Index == Other.Index &&
         (1u << (UseOther.Swizzle & 3u)) == Other.Mask;
}

bool CheckParallelDestinations() {
  std::vector<FlowBlock> Blocks(2);
  Blocks[0].Instructions = {
      Mov({OperandKind::Temp, 4}, {OperandKind::Immediate, 11}),
      Mov({OperandKind::Temp, 5}, {OperandKind::Immediate, 22})};
  Blocks[0].Kind = FlowKind::Jump;
  Blocks[0].Edges = {{1, {{11, 4}, {12, 5}}}};
  Blocks[1].Instructions = {
      Mov({OperandKind::Output, 0}, {OperandKind::Temp, 11}),
      Mov({OperandKind::Output, 1}, {OperandKind::Temp, 12})};
  MirProgram Program;
  Program.TempCount = 15;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;

  // Both edge copies coalesce away AND the two live scalars share 1 physical temp!
  if (Program.TempCount != 1 || !Blocks[0].Edges[0].Copies.empty())
    return false;
  const Operand Def0 = Blocks[0].Instructions[0].Operands[0];
  const Operand Def1 = Blocks[0].Instructions[1].Operands[0];
  const Operand Use0 = Blocks[1].Instructions[0].Operands[1];
  const Operand Use1 = Blocks[1].Instructions[1].Operands[1];
  if (SameSlot(Def0, Def1) || Use0.Index != Def0.Index ||
      Use1.Index != Def1.Index ||
      (1u << (Use0.Swizzle & 3u)) != Def0.Mask ||
      (1u << (Use1.Swizzle & 3u)) != Def1.Mask)
    return false;
  return LowerControlFlow(Blocks, Program, Error) && Program.TempCount == 1 &&
         Program.Instructions.size() == 5;
}

bool CheckSourceLiveOnSiblingEdge() {
  std::vector<FlowBlock> Blocks(3);
  Blocks[0].Instructions = {
      Mov({OperandKind::Temp, 4}, {OperandKind::Immediate, 11})};
  Blocks[0].Kind = FlowKind::Branch;
  Blocks[0].Condition = {OperandKind::Input, 0};
  Blocks[0].Edges = {{1, {{7, 4}}}, {2, {}}};
  Blocks[1].Instructions = {
      Mov({OperandKind::Output, 0}, {OperandKind::Temp, 7})};
  Blocks[2].Instructions = {
      Mov({OperandKind::Output, 0}, {OperandKind::Temp, 4})};

  MirProgram Program;
  Program.TempCount = 8;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;

  // Coalescing eliminates the edge copy completely into 1 temp.
  return Program.TempCount == 1 && Blocks[0].Edges[0].Copies.empty() &&
         Blocks[1].Instructions[0].Operands[1].Index ==
             Blocks[0].Instructions[0].Operands[0].Index &&
         Blocks[2].Instructions[0].Operands[1].Index ==
             Blocks[0].Instructions[0].Operands[0].Index;
}

bool CheckImmediateEdgeCopy() {
  std::vector<FlowBlock> Blocks(2);
  Blocks[0].Kind = FlowKind::Jump;
  Blocks[0].Edges = {{1, {{7, {OperandKind::Immediate, UINT32_MAX}}}}};
  Blocks[1].Instructions = {
      Mov({OperandKind::Output, 0}, {OperandKind::Temp, 7})};

  MirProgram Program;
  Program.TempCount = 8;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error) || Program.TempCount != 1 ||
      !LowerControlFlow(Blocks, Program, Error) || Program.Instructions.size() != 3)
    return false;

  const Instruction &Copy = Program.Instructions[0];
  return Copy.Opcode == D3D10_SB_OPCODE_MOV &&
         Copy.Operands[0].Kind == OperandKind::Temp &&
         Copy.Operands[0].Index == 0 &&
         Copy.Operands[1].Kind == OperandKind::Immediate &&
         Copy.Operands[1].Index == UINT32_MAX;
}

bool CheckSubRegScalarPacking4To1() {
  // Four simultaneously live scalars must pack into a single physical r0 (.x/.y/.z/.w).
  std::vector<FlowBlock> Blocks(1);
  Blocks[0].Instructions = {
      Mov({OperandKind::Temp, 0}, {OperandKind::Immediate, 1}),
      Mov({OperandKind::Temp, 1}, {OperandKind::Immediate, 2}),
      Mov({OperandKind::Temp, 2}, {OperandKind::Immediate, 3}),
      Mov({OperandKind::Temp, 3}, {OperandKind::Immediate, 4}),
      Mov({OperandKind::Output, 0}, {OperandKind::Temp, 0}),
      Mov({OperandKind::Output, 1}, {OperandKind::Temp, 1}),
      Mov({OperandKind::Output, 2}, {OperandKind::Temp, 2}),
      Mov({OperandKind::Output, 3}, {OperandKind::Temp, 3})};
  MirProgram Program;
  Program.TempCount = 4;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  if (Program.TempCount != 1)
    return false;
  uint8_t AllMasks = 0;
  for (unsigned I = 0; I < 4; ++I) {
    const Operand &Def = Blocks[0].Instructions[I].Operands[0];
    const Operand &Use = Blocks[0].Instructions[I + 4].Operands[1];
    if (Def.Index != 0 || Use.Index != 0 ||
        (1u << (Use.Swizzle & 3u)) != Def.Mask)
      return false;
    AllMasks |= Def.Mask;
  }
  return AllMasks == 0xfu;
}

bool CheckVec3PlusScalarPacking() {
  // A live float3 (.xyz) + a live scalar must pack into 1 physical register (.xyz + .w).
  std::vector<FlowBlock> Blocks(1);
  Operand V3{OperandKind::Temp, 0, 0, 0x7u, 0x24u};
  Operand S1{OperandKind::Temp, 1, 0, 0x1u, 0x00u};
  Operand Out3{OperandKind::Output, 0, 0, 0x7u, 0x24u};
  Blocks[0].Instructions = {
      {D3D10_SB_OPCODE_ADD, {V3, {OperandKind::Input, 0, 0, 0x7u, 0x24u},
                             {OperandKind::Input, 1, 0, 0x7u, 0x24u}}},
      Mov(S1, {OperandKind::Input, 2, 0, 0x1u, 0x00u}),
      Mov(Out3, V3),
      Mov({OperandKind::Output, 1}, S1)};
  MirProgram Program;
  Program.TempCount = 2;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  if (Program.TempCount != 1)
    return false;
  const Operand &DefV3 = Blocks[0].Instructions[0].Operands[0];
  const Operand &DefS1 = Blocks[0].Instructions[1].Operands[0];
  return DefV3.Index == 0 && DefS1.Index == 0 &&
         (DefV3.Mask & DefS1.Mask) == 0 && (DefV3.Mask | DefS1.Mask) == 0xfu;
}

bool CheckCrossLanePhiCoalescing() {
  // Edge copy copies t0.z (lane 2) -> t1.x (lane 0). Sub-register allocator
  // must coalesce t1 into t0's physical lane 2, eliminating the edge copy!
  std::vector<FlowBlock> Blocks(2);
  Operand T0Z{OperandKind::Temp, 0, 0, 0x4u, 0xaau};
  Blocks[0].Instructions = {Mov(T0Z, {OperandKind::Immediate, 42})};
  Blocks[0].Kind = FlowKind::Jump;
  ParallelCopy CrossLane(1, T0Z);
  CrossLane.DestinationLane = 0;
  Blocks[0].Edges = {{1, {CrossLane}}};
  Blocks[1].Instructions = {
      Mov({OperandKind::Output, 0}, {OperandKind::Temp, 1})};
  MirProgram Program;
  Program.TempCount = 2;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  return Program.TempCount == 1 && Blocks[0].Edges[0].Copies.empty() &&
         Blocks[0].Instructions[0].Operands[0].Index ==
             Blocks[1].Instructions[0].Operands[1].Index &&
         (1u << (Blocks[1].Instructions[0].Operands[1].Swizzle & 3u)) ==
             Blocks[0].Instructions[0].Operands[0].Mask;
}

bool CheckIntraBlockMovCoalescing() {
  // Intra-block plain MOV between non-interfering temps coalesces and deletes itself.
  std::vector<FlowBlock> Blocks(1);
  Blocks[0].Instructions = {
      {D3D10_SB_OPCODE_ADD, {{OperandKind::Temp, 0}, {OperandKind::Input, 0},
                             {OperandKind::Input, 1}}},
      Mov({OperandKind::Temp, 1}, {OperandKind::Temp, 0}),
      Mov({OperandKind::Output, 0}, {OperandKind::Temp, 1})};
  MirProgram Program;
  Program.TempCount = 2;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  return Program.TempCount == 1 && Blocks[0].Instructions.size() == 2;
}

bool CheckFixedLaneTextureDest() {
  // SAMPLE dest writes .yz (mask 0x6) and must keep fixed lanes .yz; a live
  // scalar packs into .x or .w of the same physical register.
  std::vector<FlowBlock> Blocks(1);
  Operand TexDst{OperandKind::Temp, 0, 0, 0x6u, 0x55u};
  Operand Scalar{OperandKind::Temp, 1, 0, 0x1u, 0x00u};
  Blocks[0].Instructions = {
      {D3D10_SB_OPCODE_SAMPLE,
       {TexDst, {OperandKind::Input, 0, 0, 0x3u, 0x04u},
        {OperandKind::Resource, 0, 0, 0xfu, 0xe4u},
        {OperandKind::Sampler, 0, 0, 0, 0}}},
      Mov(Scalar, {OperandKind::Input, 1}),
      Mov({OperandKind::Output, 0, 0, 0x6u, 0xe4u},
          {OperandKind::Temp, 0, 0, 0x6u, 0xe4u}),
      Mov({OperandKind::Output, 1}, Scalar)};
  MirProgram Program;
  Program.TempCount = 2;
  std::string Error;
  if (!AllocateRegisters(Blocks, Program, Error))
    return false;
  const Operand &DefTex = Blocks[0].Instructions[0].Operands[0];
  const Operand &DefS = Blocks[0].Instructions[1].Operands[0];
  return Program.TempCount == 1 && DefTex.Mask == 0x6u &&
         (DefS.Mask & 0x6u) == 0;
}

bool CheckInvalidRelativeRegister() {
  std::vector<FlowBlock> Blocks(1);
  Operand Indirect{OperandKind::ConstantBuffer, 0};
  Indirect.RelativeRegister = 5;
  Blocks[0].Instructions = {Mov({OperandKind::Temp, 3}, Indirect)};
  MirProgram Program;
  Program.TempCount = 4;
  std::string Error;
  return !AllocateRegisters(Blocks, Program, Error) && !Error.empty() &&
         Program.TempCount == 4 &&
         Blocks[0].Instructions[0].Operands[0].Index == 3;
}

bool CheckSparseInterferenceGraph() {
  using namespace regalloc;
  const size_t N = kDenseGraphSlotLimit + 128;
  SlotInterferenceGraph Graph(N);
  SparseSlotState State(N);

  const uint32_t A = 1;
  const uint32_t B = 65;
  const uint32_t C = static_cast<uint32_t>(kDenseGraphSlotLimit + 5);
  const uint32_t D = static_cast<uint32_t>(kDenseGraphSlotLimit + 70);
  const uint32_t E = static_cast<uint32_t>(kDenseGraphSlotLimit + 120);

  State.AddSlot(A);
  State.AddSlot(B);
  State.AddSlot(C);
  Graph.RecordLiveInputs(State);
  if (!Graph.Interferes(A, B) || !Graph.Interferes(B, C) ||
      !Graph.Interferes(A, C) || Graph.Interferes(A, A) ||
      Graph.GetDegree(A) != 2 || Graph.GetDegree(C) != 2)
    return false;

  State.Clear();
  State.AddSlot(B);
  State.AddSlot(D);
  State.AddSlot(E);
  Graph.InterfereAgainst(C, State, /*SkipSlot=*/D);
  if (Graph.Interferes(C, D) || !Graph.Interferes(C, E) ||
      !Graph.Interferes(E, C) || Graph.GetDegree(C) != 3)
    return false;

  // Merge D (interfering with E) into A (interfering with B, C).
  State.Clear();
  State.AddSlot(E);
  Graph.InterfereAgainst(D, State);
  Graph.MergeSlots(A, D);
  if (!Graph.Interferes(A, E) || !Graph.Interferes(E, A) ||
      !Graph.Interferes(A, B) || !Graph.Interferes(A, C) ||
      Graph.GetDegree(A) != 3)
    return false;

  std::vector<uint32_t> Neighbors;
  Graph.ForEachNeighbor(A, [&](uint32_t Other) { Neighbors.push_back(Other); });
  return Neighbors.size() == 3 && Neighbors[0] == B && Neighbors[1] == C &&
         Neighbors[2] == E;
}

} // namespace

int main() {
  if (!CheckStraightLineReuse() || !CheckLiveAcrossBackEdge() ||
      !CheckLiveThroughReorderedBlocks() || !CheckParallelDestinations() ||
      !CheckSourceLiveOnSiblingEdge() || !CheckImmediateEdgeCopy() ||
      !CheckSubRegScalarPacking4To1() || !CheckVec3PlusScalarPacking() ||
      !CheckCrossLanePhiCoalescing() || !CheckIntraBlockMovCoalescing() ||
      !CheckFixedLaneTextureDest() || !CheckInvalidRelativeRegister() ||
      !CheckSparseInterferenceGraph()) {
    std::cerr << "MIR register allocation violated a live range or packing invariant\n";
    return 1;
  }
}
