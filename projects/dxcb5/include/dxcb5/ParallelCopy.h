#ifndef DXCB5_PARALLEL_COPY_H
#define DXCB5_PARALLEL_COPY_H

#include "dxcb5/Mir.h"
#include <cstdint>
#include <vector>

namespace dxcb5 {

struct ParallelCopy {
  static constexpr unsigned LaneCount = 4;
  static constexpr unsigned BroadcastStride = 0x55;

  uint32_t Destination;
  Operand Source;
  uint8_t DestinationLane = 0;

  ParallelCopy(uint32_t Destination, uint32_t Source)
      : ParallelCopy(Destination, Operand{OperandKind::Temp, Source}) {}
  ParallelCopy(uint32_t Destination, Operand Source, uint8_t DestinationLane = 0)
      : Destination(Destination), Source(Source),
        DestinationLane(DestinationLane) {}

  bool ReadsTemp() const { return Source.Kind == OperandKind::Temp; }
  uint8_t SourceLane() const { return Source.Swizzle & (LaneCount - 1); }
  uint8_t DestinationMask() const {
    return static_cast<uint8_t>(1u << DestinationLane);
  }

  static bool SupportsSource(const Operand &Source) {
    if (Source.RelativeRegister != UINT32_MAX)
      return false;
    switch (Source.Kind) {
    case OperandKind::Temp: {
      const auto Lane = Source.Swizzle & (LaneCount - 1);
      return Source.Swizzle == Lane * BroadcastStride &&
             Source.Modifier == 0 && Source.Element == 0 &&
             Source.RelativeLane == 0;
    }
    case OperandKind::Immediate:
    case OperandKind::ConstantBuffer:
    case OperandKind::Input:
      return true;
    default:
      return false;
    }
  }
};

bool EqualCopySources(const Operand &A, const Operand &B);

// Each destination lane may occur once. Scratch cannot be a register operand.
// Sequence changes only on success.
bool ResolveParallelCopies(const std::vector<ParallelCopy> &Copies,
                           uint32_t Scratch,
                           std::vector<ParallelCopy> &Sequence);

// Compare sequential and simultaneous copies using symbolic operand origins.
bool VerifyParallelCopies(const std::vector<ParallelCopy> &Copies,
                          uint32_t Scratch,
                          const std::vector<ParallelCopy> &Sequence);

} // namespace dxcb5

#endif
