#include "dxcb5/ParallelCopy.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dxcb5 {
namespace {

using LaneKey = uint64_t;
constexpr unsigned RegisterLanes = ParallelCopy::LaneCount;
constexpr unsigned BroadcastSwizzle = ParallelCopy::BroadcastStride;

LaneKey Key(uint32_t Register, uint8_t Lane) {
  return static_cast<LaneKey>(Register) * RegisterLanes + Lane;
}

LaneKey DestinationKey(const ParallelCopy &Copy) {
  return Key(Copy.Destination, Copy.DestinationLane);
}

LaneKey SourceKey(const ParallelCopy &Copy) {
  return Key(Copy.Source.Index, Copy.SourceLane());
}

Operand TempSource(LaneKey Location) {
  auto Index = static_cast<uint32_t>(Location / RegisterLanes);
  auto Swizzle =
      static_cast<uint8_t>((Location % RegisterLanes) * BroadcastSwizzle);
  return {OperandKind::Temp, Index, 0, 1, Swizzle};
}

bool RecordDestination(const ParallelCopy &Copy, uint32_t Scratch,
                       std::unordered_set<LaneKey> &Destinations) {
  return Copy.DestinationLane < RegisterLanes &&
         ParallelCopy::SupportsSource(Copy.Source) &&
         Copy.Destination != Scratch &&
         (!Copy.ReadsTemp() || Copy.Source.Index != Scratch) &&
         Destinations.insert(DestinationKey(Copy)).second;
}

struct Origin {
  Operand Source;

  bool operator==(const Origin &Other) const {
    return EqualCopySources(Source, Other.Source);
  }
};

} // namespace

bool EqualCopySources(const Operand &A, const Operand &B) {
  return A.Kind == B.Kind && A.Index == B.Index && A.Element == B.Element &&
         A.Swizzle == B.Swizzle && A.Modifier == B.Modifier &&
         A.RelativeRegister == B.RelativeRegister &&
         A.RelativeLane == B.RelativeLane;
}

bool ResolveParallelCopies(const std::vector<ParallelCopy> &Copies,
                           uint32_t Scratch,
                           std::vector<ParallelCopy> &Sequence) {
  std::unordered_set<LaneKey> Destinations;
  std::vector<ParallelCopy> Pending;
  std::unordered_map<LaneKey, std::size_t> Reads;
  for (const auto &Copy : Copies) {
    if (!RecordDestination(Copy, Scratch, Destinations))
      return false;
    if (Copy.ReadsTemp() && DestinationKey(Copy) == SourceKey(Copy))
      continue;
    Pending.push_back(Copy);
    if (Copy.ReadsTemp())
      ++Reads[SourceKey(Copy)];
  }

  std::vector<ParallelCopy> Result;
  while (!Pending.empty()) {
    auto Ready = Pending.end();
    for (auto It = Pending.begin(); It != Pending.end(); ++It) {
      if (Reads[DestinationKey(*It)] != 0)
        continue;
      if (Ready == Pending.end())
        Ready = It;
      if (!Result.empty() && It->Destination == Result.back().Destination &&
          It->Source.Kind == Result.back().Source.Kind &&
          It->Source.Index == Result.back().Source.Index) {
        Ready = It;
        break;
      }
    }
    if (Ready != Pending.end()) {
      Result.push_back(*Ready);
      if (Ready->ReadsTemp())
        --Reads[SourceKey(*Ready)];
      Pending.erase(Ready);
      continue;
    }

    // Every pending destination is still read. Break a cross-register vector
    // cycle across matching lanes so adjacent moves coalesce, or fall back to
    // a single lane for intra-register lane permutations.
    const auto &First = Pending.front();
    const uint32_t SavedReg = First.Destination;
    const bool CrossRegVec = First.ReadsTemp() && First.Source.Index != SavedReg;
    bool SavedAny = false;
    for (const auto &Copy : Pending) {
      if (Copy.Destination != SavedReg)
        continue;
      if (!CrossRegVec && SavedAny)
        break;
      if (CrossRegVec && (!Copy.ReadsTemp() || Copy.Source.Index == SavedReg))
        continue;
      const uint8_t ScratchLane = CrossRegVec ? Copy.DestinationLane : 0u;
      const auto ScratchKey = Key(Scratch, ScratchLane);
      if (Reads[ScratchKey] != 0)
        continue;
      const auto Saved = DestinationKey(Copy);
      if (Reads[Saved] == 0)
        continue;
      Result.emplace_back(Scratch, TempSource(Saved), ScratchLane);
      for (auto &P : Pending) {
        if (P.ReadsTemp() && SourceKey(P) == Saved)
          P.Source = TempSource(ScratchKey);
      }
      Reads[ScratchKey] = Reads[Saved];
      Reads[Saved] = 0;
      SavedAny = true;
    }
    if (!SavedAny) {
      const auto Saved = DestinationKey(First);
      uint8_t FreeLane = static_cast<uint8_t>(RegisterLanes);
      for (uint8_t L = 0; L < RegisterLanes; ++L) {
        if (Reads[Key(Scratch, L)] == 0) {
          FreeLane = L;
          break;
        }
      }
      if (FreeLane >= RegisterLanes)
        return false;
      const auto ScratchKey = Key(Scratch, FreeLane);
      Result.emplace_back(Scratch, TempSource(Saved), FreeLane);
      for (auto &P : Pending) {
        if (P.ReadsTemp() && SourceKey(P) == Saved)
          P.Source = TempSource(ScratchKey);
      }
      Reads[ScratchKey] = Reads[Saved];
      Reads[Saved] = 0;
    }
  }

  if (!VerifyParallelCopies(Copies, Scratch, Result))
    return false;
  Sequence = std::move(Result);
  return true;
}

bool VerifyParallelCopies(const std::vector<ParallelCopy> &Copies,
                          uint32_t Scratch,
                          const std::vector<ParallelCopy> &Sequence) {
  std::unordered_set<LaneKey> Destinations;
  std::unordered_map<LaneKey, Origin> Origins;
  for (const auto &Copy : Copies) {
    if (!RecordDestination(Copy, Scratch, Destinations))
      return false;
    const auto Dest = DestinationKey(Copy);
    Origins.emplace(Dest, Origin{TempSource(Dest)});
    if (Copy.ReadsTemp())
      Origins.emplace(SourceKey(Copy), Origin{TempSource(SourceKey(Copy))});
  }

  auto Expected = Origins;
  for (const auto &Copy : Copies) {
    auto From = Copy.ReadsTemp() ? TempSource(SourceKey(Copy)) : Copy.Source;
    Expected.at(DestinationKey(Copy)) = {From};
  }

  for (const auto &Move : Sequence) {
    const auto Dest = DestinationKey(Move);
    if (Move.DestinationLane >= RegisterLanes ||
        !ParallelCopy::SupportsSource(Move.Source) ||
        (Move.Destination != Scratch && Origins.count(Dest) == 0))
      return false;
    Origin Value{Move.Source};
    if (Move.ReadsTemp()) {
      const auto Source = Origins.find(SourceKey(Move));
      if (Source == Origins.end())
        return false;
      Value = Source->second;
    }
    Origins.insert_or_assign(Dest, Value);
  }

  for (uint8_t Lane = 0; Lane < RegisterLanes; ++Lane)
    Origins.erase(Key(Scratch, Lane));
  return Origins == Expected;
}

} // namespace dxcb5
