// Distributed under the University of Illinois Open Source License.
// 1:1 C++ implementation of the Lean-verified abstract domain in
// projects/dxcb5/lean/LaneDomain.lean.
#ifndef DXCB5_MIR_ANALYSIS_LANE_DOMAIN_H
#define DXCB5_MIR_ANALYSIS_LANE_DOMAIN_H

#include "Mir/SsaMir.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dxcb5 {

struct SignedRange {
  int32_t Lo = INT32_MIN;
  int32_t Hi = INT32_MAX;

  static constexpr SignedRange Top() { return {INT32_MIN, INT32_MAX}; }
  static constexpr SignedRange Single(int32_t V) { return {V, V}; }

  bool IsFull() const { return Lo == INT32_MIN && Hi == INT32_MAX; }
  bool IsEmpty() const { return Lo > Hi; }
  bool IsSingle() const { return Lo == Hi; }
  bool Contains(int32_t X) const { return Lo <= X && X <= Hi; }
  bool operator==(const SignedRange &O) const { return Lo == O.Lo && Hi == O.Hi; }
  bool operator!=(const SignedRange &O) const { return !(*this == O); }

  static SignedRange Intersect(SignedRange A, SignedRange B) {
    return {std::max(A.Lo, B.Lo), std::min(A.Hi, B.Hi)};
  }
  static SignedRange Union(SignedRange A, SignedRange B) {
    return {std::min(A.Lo, B.Lo), std::max(A.Hi, B.Hi)};
  }
  // Lean-verified widening operator (SignedRange.widen): at most 2 widenings
  // reach a fixpoint on any ascending chain.
  static SignedRange Widen(SignedRange Old, SignedRange New) {
    return {New.Lo < Old.Lo ? INT32_MIN : Old.Lo,
            Old.Hi < New.Hi ? INT32_MAX : Old.Hi};
  }
};

struct UnsignedRange {
  uint32_t Lo = 0u;
  uint32_t Hi = UINT32_MAX;

  static constexpr UnsignedRange Top() { return {0u, UINT32_MAX}; }
  static constexpr UnsignedRange Single(uint32_t V) { return {V, V}; }

  bool IsFull() const { return Lo == 0u && Hi == UINT32_MAX; }
  bool IsEmpty() const { return Lo > Hi; }
  bool IsSingle() const { return Lo == Hi; }
  bool Contains(uint32_t X) const { return Lo <= X && X <= Hi; }
  bool operator==(const UnsignedRange &O) const { return Lo == O.Lo && Hi == O.Hi; }
  bool operator!=(const UnsignedRange &O) const { return !(*this == O); }

  static UnsignedRange Intersect(UnsignedRange A, UnsignedRange B) {
    return {std::max(A.Lo, B.Lo), std::min(A.Hi, B.Hi)};
  }
  static UnsignedRange Union(UnsignedRange A, UnsignedRange B) {
    return {std::min(A.Lo, B.Lo), std::max(A.Hi, B.Hi)};
  }
  static UnsignedRange Widen(UnsignedRange Old, UnsignedRange New) {
    return {New.Lo < Old.Lo ? 0u : Old.Lo,
            Old.Hi < New.Hi ? UINT32_MAX : Old.Hi};
  }
};

struct LaneFact {
  bool HasConst = false;
  uint32_t ConstBits = 0u;
  SignedRange SRange = SignedRange::Top();
  UnsignedRange URange = UnsignedRange::Top();
  bool IsBoolMask = false;

  static constexpr LaneFact Top() { return {}; }
  static constexpr LaneFact FromConst(uint32_t C) {
    return {true, C, SignedRange::Single(static_cast<int32_t>(C)),
            UnsignedRange::Single(C), C == 0u || C == 0xffffffffu};
  }
  static LaneFact Join(const LaneFact &A, const LaneFact &B) {
    const bool SameC = A.HasConst && B.HasConst && A.ConstBits == B.ConstBits;
    return {SameC, SameC ? A.ConstBits : 0u,
            SignedRange::Union(A.SRange, B.SRange),
            UnsignedRange::Union(A.URange, B.URange),
            A.IsBoolMask && B.IsBoolMask};
  }
  static LaneFact Widen(const LaneFact &Old, const LaneFact &New) {
    const bool SameC =
        Old.HasConst && New.HasConst && Old.ConstBits == New.ConstBits;
    return {SameC, SameC ? Old.ConstBits : 0u,
            SignedRange::Widen(Old.SRange, New.SRange),
            UnsignedRange::Widen(Old.URange, New.URange),
            Old.IsBoolMask && New.IsBoolMask};
  }
};

// Verified interval transfer functions (LaneDomain.lean section 3 & 4).
inline SignedRange EvalIminRange(SignedRange A, SignedRange B) {
  return {std::min(A.Lo, B.Lo), std::min(A.Hi, B.Hi)};
}

inline SignedRange EvalImaxRange(SignedRange A, SignedRange B) {
  return {std::max(A.Lo, B.Lo), std::max(A.Hi, B.Hi)};
}

inline UnsignedRange EvalUminRange(UnsignedRange A, UnsignedRange B) {
  return {std::min(A.Lo, B.Lo), std::min(A.Hi, B.Hi)};
}

inline UnsignedRange EvalUmaxRange(UnsignedRange A, UnsignedRange B) {
  return {std::max(A.Lo, B.Lo), std::max(A.Hi, B.Hi)};
}

inline SignedRange EvalIaddRange(SignedRange A, SignedRange B) {
  const int64_t Lo = int64_t(A.Lo) + int64_t(B.Lo);
  const int64_t Hi = int64_t(A.Hi) + int64_t(B.Hi);
  if (Lo >= INT32_MIN && Hi <= INT32_MAX)
    return {static_cast<int32_t>(Lo), static_cast<int32_t>(Hi)};
  return SignedRange::Top();
}

inline UnsignedRange EvalAndUnsigned(UnsignedRange A, UnsignedRange B) {
  return {0u, std::min(A.Hi, B.Hi)};
}

inline bool EvalSignedCmpRange(uint32_t Op, SignedRange A, SignedRange B,
                               uint32_t &Out) {
  if (Op == D3D10_SB_OPCODE_ILT) {
    if (A.Hi < B.Lo) { Out = 0xffffffffu; return true; }
    if (A.Lo >= B.Hi) { Out = 0u; return true; }
  } else if (Op == D3D10_SB_OPCODE_IGE) {
    if (A.Lo >= B.Hi) { Out = 0xffffffffu; return true; }
    if (A.Hi < B.Lo) { Out = 0u; return true; }
  } else if (Op == D3D10_SB_OPCODE_IEQ) {
    if (A.IsSingle() && B.IsSingle() && A.Lo == B.Lo) {
      Out = 0xffffffffu;
      return true;
    }
    if (A.Hi < B.Lo || B.Hi < A.Lo) { Out = 0u; return true; }
  } else if (Op == D3D10_SB_OPCODE_INE) {
    if (A.Hi < B.Lo || B.Hi < A.Lo) { Out = 0xffffffffu; return true; }
    if (A.IsSingle() && B.IsSingle() && A.Lo == B.Lo) {
      Out = 0u;
      return true;
    }
  }
  return false;
}

inline bool RefineSignedEdge(uint32_t Op, int32_t K, bool ConstOnLhs,
                             SignedRange &Out) {
  if (Op == D3D10_SB_OPCODE_IEQ) {
    Out = SignedRange::Single(K);
    return true;
  }
  if (!ConstOnLhs) {
    if (Op == D3D10_SB_OPCODE_ILT && K > INT32_MIN) {
      Out = {INT32_MIN, K - 1};
      return true;
    }
    if (Op == D3D10_SB_OPCODE_IGE) {
      Out = {K, INT32_MAX};
      return true;
    }
  } else {
    if (Op == D3D10_SB_OPCODE_ILT && K < INT32_MAX) {
      Out = {K + 1, INT32_MAX};
      return true;
    }
    if (Op == D3D10_SB_OPCODE_IGE) {
      Out = {INT32_MIN, K};
      return true;
    }
  }
  return false;
}

inline UnsignedRange RefineUnsignedLtConst(uint32_t K) {
  return K > 0u ? UnsignedRange{0u, K - 1u} : UnsignedRange::Top();
}

inline float LaneBitsToF32(uint32_t Bits) {
  float F;
  std::memcpy(&F, &Bits, 4);
  return F;
}

inline uint32_t LaneF32ToBits(float F) {
  uint32_t B;
  std::memcpy(&B, &F, 4);
  return B;
}

inline uint32_t ApplyLaneMod(uint32_t Bits, uint8_t Mod, bool IntNeg) {
  if (IntNeg && Mod == D3D10_SB_OPERAND_MODIFIER_NEG) return 0u - Bits;
  if (Mod == D3D10_SB_OPERAND_MODIFIER_NEG) return Bits ^ kFloatSignBit;
  if (Mod == D3D10_SB_OPERAND_MODIFIER_ABS) return Bits & kFloatAbsMask;
  if (Mod == D3D10_SB_OPERAND_MODIFIER_ABSNEG) return Bits | kFloatSignBit;
  return Bits;
}

inline bool ApplyLaneSatIf(uint32_t In, bool Sat, uint32_t &Out) {
  if (!Sat) { Out = In; return true; }
  const float F = LaneBitsToF32(In);
  if (!std::isfinite(F)) return false;
  Out = LaneF32ToBits(std::min(1.0f, std::max(0.0f, F)));
  return true;
}

inline bool EvalPureFloatLane(uint32_t Op, bool Sat, bool Precise, bool HaveA,
                              uint32_t A, bool HaveB, uint32_t B, bool HaveC,
                              uint32_t C, uint32_t &Out) {
  if (!HaveA || !HaveB) return false;
  const float FA = LaneBitsToF32(A), FB = LaneBitsToF32(B);
  if (Op == D3D10_SB_OPCODE_LT) { Out = (FA < FB) ? ~0u : 0u; return true; }
  if (Op == D3D10_SB_OPCODE_GE) { Out = (FA >= FB) ? ~0u : 0u; return true; }
  if (Op == D3D10_SB_OPCODE_EQ) { Out = (FA == FB) ? ~0u : 0u; return true; }
  if (Op == D3D10_SB_OPCODE_NE) { Out = (FA != FB) ? ~0u : 0u; return true; }
  if (Precise || !std::isfinite(FA) || !std::isfinite(FB)) return false;
  float Res = 0.0f;
  if (Op == D3D10_SB_OPCODE_ADD) Res = FA + FB;
  else if (Op == D3D10_SB_OPCODE_MUL) Res = FA * FB;
  else if (Op == D3D10_SB_OPCODE_MIN) Res = std::min(FA, FB);
  else if (Op == D3D10_SB_OPCODE_MAX) Res = std::max(FA, FB);
  else if (Op == D3D10_SB_OPCODE_MAD && HaveC && std::isfinite(LaneBitsToF32(C)))
    Res = FA * FB + LaneBitsToF32(C);
  else return false;
  return std::isfinite(Res) && ApplyLaneSatIf(LaneF32ToBits(Res), Sat, Out);
}

inline bool EvalPureLaneBits(uint32_t Op, bool Sat, bool Precise, bool HaveA,
                             uint32_t A, bool HaveB, uint32_t B, bool HaveC,
                             uint32_t C, uint32_t &Out) {
  if (Op == D3D10_SB_OPCODE_MOV && HaveA) return ApplyLaneSatIf(A, Sat, Out);
  if (Op == D3D10_SB_OPCODE_MOVC && HaveA) {
    const bool T = A != 0u;
    return (T ? HaveB : HaveC) && ApplyLaneSatIf(T ? B : C, Sat, Out);
  }
  if (Op == D3D10_SB_OPCODE_AND && ((HaveA && !A) || (HaveB && !B))) { Out = 0u; return true; }
  if (Op == D3D10_SB_OPCODE_OR && ((HaveA && A == ~0u) || (HaveB && B == ~0u))) { Out = ~0u; return true; }
  if (Op == D3D10_SB_OPCODE_NOT && HaveA) { Out = ~A; return true; }
  if (Op == D3D10_SB_OPCODE_INEG && HaveA) { Out = 0u - A; return true; }
  if (HaveA && HaveB) {
    const auto SA = static_cast<int32_t>(A), SB = static_cast<int32_t>(B);
    switch (Op) {
    case D3D10_SB_OPCODE_AND: Out = A & B; return true;
    case D3D10_SB_OPCODE_OR: Out = A | B; return true;
    case D3D10_SB_OPCODE_XOR: Out = A ^ B; return true;
    case D3D10_SB_OPCODE_IADD: Out = A + B; return true;
    case D3D10_SB_OPCODE_ISHL: Out = A << (B & 31u); return true;
    case D3D10_SB_OPCODE_USHR: Out = A >> (B & 31u); return true;
    case D3D10_SB_OPCODE_IMIN: Out = static_cast<uint32_t>(std::min(SA, SB)); return true;
    case D3D10_SB_OPCODE_IMAX: Out = static_cast<uint32_t>(std::max(SA, SB)); return true;
    case D3D10_SB_OPCODE_IEQ: Out = (A == B) ? ~0u : 0u; return true;
    case D3D10_SB_OPCODE_INE: Out = (A != B) ? ~0u : 0u; return true;
    case D3D10_SB_OPCODE_ULT: Out = (A < B) ? ~0u : 0u; return true;
    case D3D10_SB_OPCODE_UGE: Out = (A >= B) ? ~0u : 0u; return true;
    case D3D10_SB_OPCODE_ILT: Out = (SA < SB) ? ~0u : 0u; return true;
    case D3D10_SB_OPCODE_IGE: Out = (SA >= SB) ? ~0u : 0u; return true;
    default: break;
    }
  }
  return EvalPureFloatLane(Op, Sat, Precise, HaveA, A, HaveB, B, HaveC, C, Out);
}

template <typename GetRangeFn>
inline SignedRange EvalInstRangeFromFn(const SsaInst &Inst, unsigned Lane,
                                       GetRangeFn &&GetRange) {
  const uint32_t Op = Inst.Opcode;
  if (Op == D3D10_SB_OPCODE_MOV && !Inst.Saturate && Inst.Sources.size() == 1)
    return GetRange(Inst.Sources[0], Lane);
  if (Op == D3D10_SB_OPCODE_MOVC && !Inst.Saturate && Inst.Sources.size() == 3)
    return SignedRange::Union(GetRange(Inst.Sources[1], Lane),
                              GetRange(Inst.Sources[2], Lane));
  if (Inst.Sources.size() < 2) return SignedRange::Top();
  const auto RA = GetRange(Inst.Sources[0], Lane);
  const auto RB = GetRange(Inst.Sources[1], Lane);
  if (Op == D3D10_SB_OPCODE_IMIN) return EvalIminRange(RA, RB);
  if (Op == D3D10_SB_OPCODE_IMAX) return EvalImaxRange(RA, RB);
  if (Op == D3D10_SB_OPCODE_IADD) return EvalIaddRange(RA, RB);
  return SignedRange::Top();
}

inline uint64_t MakeLaneRangeKey(const Operand &Op, unsigned Slot) {
  if (Op.Modifier != 0 || Op.RelativeRegister != UINT32_MAX) return 0;
  const uint64_t Lane = GetSwizzleLane(Op.Swizzle, Slot);
  if (Op.Kind == OperandKind::Temp)
    return (1ULL << 60) | (uint64_t(Op.Index) << 2) | Lane;
  if (Op.Kind == OperandKind::Input)
    return (2ULL << 60) | (uint64_t(Op.Index) << 2) | Lane;
  if (Op.Kind == OperandKind::ConstantBuffer && Op.Index < 256u)
    return (3ULL << 60) | (uint64_t(Op.Index) << 34) |
           (uint64_t(Op.Element) << 2) | Lane;
  return 0;
}

struct ScopedSignedRangeMap {
  struct Undo { uint64_t Key; SignedRange Prev; bool HadPrev; };
  llvm::DenseMap<uint64_t, SignedRange> Map;
  llvm::SmallVector<Undo, 64> Trail;
  size_t Mark() const { return Trail.size(); }
  void Refine(uint64_t Key, SignedRange R) {
    if (!Key || R.IsFull() || R.IsEmpty()) return;
    auto It = Map.find(Key);
    const bool Had = It != Map.end();
    const SignedRange Old = Had ? It->second : SignedRange::Top();
    const SignedRange Next = SignedRange::Intersect(Old, R);
    if (Next.IsEmpty() || (Had && Next == Old)) return;
    Trail.push_back({Key, Old, Had});
    Map[Key] = Next;
  }
  void Rollback(size_t M) {
    while (Trail.size() > M) {
      const Undo E = Trail.pop_back_val();
      if (E.HadPrev) Map[E.Key] = E.Prev;
      else Map.erase(E.Key);
    }
  }
  SignedRange Lookup(uint64_t Key) const {
    auto It = Key ? Map.find(Key) : Map.end();
    return It == Map.end() ? SignedRange::Top() : It->second;
  }
};

} // namespace dxcb5
#endif
