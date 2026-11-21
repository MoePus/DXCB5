#include "dxcb5/ParallelCopy.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

using dxcb5::Operand;
using dxcb5::OperandKind;
using dxcb5::ParallelCopy;
using dxcb5::ResolveParallelCopies;
using dxcb5::VerifyParallelCopies;

namespace {

constexpr uint32_t NumRegisters = 4;
constexpr uint32_t Scratch = NumRegisters;
constexpr uint32_t Omitted = NumRegisters;

// Assign disjoint symbolic values to each register and immutable operand.
template <size_t Count>
uint64_t Read(const Operand &Source, const std::array<uint64_t, Count> &Registers) {
  if (Source.Kind == OperandKind::Temp)
    return Registers.at(Source.Index);
  const uint64_t Bank = static_cast<uint64_t>(Source.Kind) << 56;
  return Bank | (uint64_t{Source.Index} << 24) |
         (uint64_t{Source.Element} << 16) |
         (uint64_t{Source.Swizzle} << 8) | Source.Modifier;
}

// Enumerate omitted, register and immutable sources for four destinations.
bool CheckSchedules() {
  constexpr std::array<Operand, 6> Immutable = {{
      {OperandKind::Immediate, 0}, {OperandKind::Immediate, Scratch},
      {OperandKind::Immediate, UINT32_MAX}, {OperandKind::Input, Scratch, 0, 8, 0xff},
      {OperandKind::ConstantBuffer, Scratch, 1, 1, 0x55},
      {OperandKind::ConstantBuffer, Scratch, 2, 1, 0xaa, 1},
  }};
  constexpr uint32_t FirstImmutable = Omitted + 1;
  constexpr uint32_t Choices = FirstImmutable + Immutable.size();
  constexpr uint32_t NumCases = Choices * Choices * Choices * Choices;
  for (uint32_t Case = 0; Case != NumCases; ++Case) {
    std::vector<ParallelCopy> Copies;
    std::array<uint64_t, NumRegisters + 1> Initial = {11, 33, 55, 77, 99};
    auto Expected = Initial;
    auto Actual = Initial;
    for (uint32_t Destination = 0, Encoded = Case; Destination != NumRegisters;
         ++Destination, Encoded /= Choices) {
      const uint32_t Source = Encoded % Choices;
      if (Source == Omitted)
        continue;
      const Operand Value = Source < FirstImmutable
                                ? Operand{OperandKind::Temp, Source}
                                : Immutable[Source - FirstImmutable];
      Copies.emplace_back(Destination, Value);
      Expected[Destination] = Read(Value, Initial);
    }

    std::vector<ParallelCopy> Sequence;
    if (!ResolveParallelCopies(Copies, Scratch, Sequence))
      return false;
    for (const ParallelCopy &Move : Sequence)
      Actual.at(Move.Destination) =
          Read(Move.Source, Actual);
    for (uint32_t Reg = 0; Reg != NumRegisters; ++Reg) {
      if (Actual[Reg] != Expected[Reg])
        return false;
    }
  }
  return true;
}

bool CheckCycles() {
  const std::vector<ParallelCopy> Swaps = {{1, 0}, {0, 1}, {3, 2}, {2, 3}};
  std::vector<ParallelCopy> Sequence;
  if (!ResolveParallelCopies(Swaps, Scratch, Sequence) || Sequence.size() != 6 ||
      !VerifyParallelCopies(Swaps, Scratch, Sequence))
    return false;
  // Vector 3-lane swap r0.xyz <-> r1.xyz should emit 3 grouped Scratch saves,
  // 3 grouped r0<-r1 moves, and 3 grouped r1<-Scratch moves (coalescing to 3 movs).
  std::vector<ParallelCopy> VecSwap;
  for (uint8_t L = 0; L < 3; ++L) {
    const uint8_t Swz = static_cast<uint8_t>(L * 0x55u);
    VecSwap.emplace_back(0, Operand{OperandKind::Temp, 1, 0, 1, Swz}, L);
    VecSwap.emplace_back(1, Operand{OperandKind::Temp, 0, 0, 1, Swz}, L);
  }
  Sequence.clear();
  if (!ResolveParallelCopies(VecSwap, Scratch, Sequence) || Sequence.size() != 9 ||
      !VerifyParallelCopies(VecSwap, Scratch, Sequence))
    return false;
  for (size_t I = 0; I < 3; ++I)
    if (Sequence[I].Destination != Scratch || Sequence[I + 3].Destination != 0 ||
        Sequence[I + 6].Destination != 1)
      return false;
  return true;
}

bool CheckLaneCycles() {
  constexpr uint32_t LaneCount = 4;
  constexpr uint32_t Register = 1;
  constexpr unsigned Choices = LaneCount * 2;
  constexpr unsigned Cases = Choices * Choices * Choices * Choices;
  for (unsigned Case = 0; Case != Cases; ++Case) {
    const std::array<std::array<uint32_t, LaneCount>, Scratch + 1> Initial = {{
        {{1, 2, 3, 4}}, {{5, 6, 7, 8}}, {{9, 10, 11, 12}},
        {{13, 14, 15, 16}}, {{17, 18, 19, 20}},
    }};
    auto Expected = Initial;
    std::vector<ParallelCopy> Copies;
    unsigned Encoded = Case;
    for (uint8_t Lane = 0; Lane < LaneCount; ++Lane, Encoded /= Choices) {
      const auto Source = Encoded % Choices;
      const auto SourceReg = Source / LaneCount;
      const auto SourceLane = Source % LaneCount;
      const Operand Op{OperandKind::Temp, SourceReg, 0, 1,
                       static_cast<uint8_t>(SourceLane * 0x55u)};
      Copies.emplace_back(Register, Op, Lane);
      Expected[Register][Lane] = Initial[SourceReg][SourceLane];
    }
    std::vector<ParallelCopy> Moves;
    if (!ResolveParallelCopies(Copies, Scratch, Moves))
      return false;
    auto Actual = Initial;
    for (const auto &Move : Moves)
      Actual.at(Move.Destination).at(Move.DestinationLane) =
          Actual.at(Move.Source.Index).at(Move.Source.Swizzle & 3);
    Actual[Scratch][0] = Initial[Scratch][0];
    if (Actual != Expected)
      return false;
  }
  return true;
}

bool CheckMixedSchedule() {
  constexpr uint32_t MixedScratch = 7;
  const Operand BufferY{OperandKind::ConstantBuffer, 7, 1, 1, 0x55};
  const Operand BufferZ{OperandKind::ConstantBuffer, 7, 1, 1, 0xaa};
  const Operand OtherRow{OperandKind::ConstantBuffer, 7, 2, 1, 0x55, 1};
  const Operand Input{OperandKind::Input, 7, 0, 8, 0xff};
  const std::vector<ParallelCopy> Copies = {
      {0, 1}, {1, 0}, {2, BufferY}, {3, 2}, {4, BufferZ},
      {5, OtherRow}, {6, Input}};
  std::vector<ParallelCopy> Moves;
  if (!ResolveParallelCopies(Copies, MixedScratch, Moves) || Moves.size() != 8)
    return false;
  const std::array<uint64_t, 8> Before = {1, 2, 3, 4, 5, 6, 7, 0};
  auto After = Before;
  for (const auto &Move : Moves)
    After.at(Move.Destination) = Read(Move.Source, After);
  if (After[0] != Before[1] || After[1] != Before[0] ||
      After[2] != Read(BufferY, Before) || After[3] != Before[2] ||
      After[4] != Read(BufferZ, Before) ||
      After[5] != Read(OtherRow, Before) ||
      After[6] != Read(Input, Before))
    return false;
  const auto Move = std::find_if(Moves.begin(), Moves.end(),
                                 [](const ParallelCopy &Copy) {
                                   return Copy.Destination == 5;
                                 });
  if (Move == Moves.end())
    return false;
  Move->Source.Element = BufferY.Element;
  if (VerifyParallelCopies(Copies, MixedScratch, Moves))
    return false;
  Move->Source = OtherRow;
  Move->Source.Modifier = BufferY.Modifier;
  return !VerifyParallelCopies(Copies, MixedScratch, Moves);
}

bool CheckInvalidCopies() {
  std::vector<ParallelCopy> Sequence = {{3, 2}};
  const std::vector<ParallelCopy> Duplicate = {{0, 1}, {0, 2}};
  const std::vector<ParallelCopy> Reserved = {{0, Scratch}};
  const std::vector<ParallelCopy> ReservedLane = {
      {0, {OperandKind::Temp, Scratch, 0, 1, 0x55}, 2}};
  const std::vector<ParallelCopy> InvalidLane = {{0, {OperandKind::Temp, 1}, 4}};
  const std::vector<ParallelCopy> NonBroadcast = {
      {0, {OperandKind::Temp, 1, 0, 1, 0xe4}, 2}};
  const std::vector<ParallelCopy> Immediate = {{0, {OperandKind::Immediate, 1}}};
  const std::vector<ParallelCopy> Register = {{0, 1}};
  const std::vector<ParallelCopy> Relative = {
      {0, {OperandKind::ConstantBuffer, 1, 0, 1, 0, 0, 1}}};
  const std::vector<ParallelCopy> Swap = {{0, 1}, {1, 0}};
  const std::vector<ParallelCopy> ReadsScratch = {{0, Scratch}};
  const std::vector<ParallelCopy> ClobbersOther = {
      {Scratch, 0}, {0, 1}, {1, Scratch}, {2, 0}};
  return !ResolveParallelCopies(Duplicate, Scratch, Sequence) &&
         !ResolveParallelCopies(Reserved, Scratch, Sequence) &&
         !ResolveParallelCopies(ReservedLane, Scratch, Sequence) &&
         !ResolveParallelCopies(InvalidLane, Scratch, Sequence) &&
         !ResolveParallelCopies(NonBroadcast, Scratch, Sequence) &&
         !ResolveParallelCopies(Relative, Scratch, Sequence) &&
         Sequence.size() == 1 && Sequence[0].Destination == 3 &&
         Sequence[0].Source.Index == 2 &&
         !VerifyParallelCopies(Swap, Scratch, Swap) &&
         !VerifyParallelCopies(Swap, Scratch, ReadsScratch) &&
         !VerifyParallelCopies(Immediate, Scratch, Register) &&
         !VerifyParallelCopies(Register, Scratch, Immediate) &&
         !VerifyParallelCopies(Swap, Scratch, ClobbersOther);
}

} // namespace

int main() {
  if (!CheckSchedules()) {
    std::cerr << "Parallel copy schedule corrupted an original register value\n";
    return 1;
  }
  if (!CheckCycles() || !CheckLaneCycles()) {
    std::cerr << "Disjoint copy cycles cannot reuse the scratch register\n";
    return 1;
  }
  if (!CheckMixedSchedule()) {
    std::cerr << "Mixed edge copies lost an operand origin\n";
    return 1;
  }
  if (!CheckInvalidCopies()) {
    std::cerr << "Invalid copy graph or schedule passed verification\n";
    return 1;
  }
}
