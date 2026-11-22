// Distributed under the University of Illinois Open Source License.
#ifndef DXCB5_MIR_COMBINE_COMBINE_COMMON_H
#define DXCB5_MIR_COMBINE_COMBINE_COMMON_H

#include "Mir/OptPipeline.h"
#include "Mir/Analysis/LaneAnalysis.h"
#include "dxcb5/InstructionInfo.h"
#include "llvm/Support/MathExtras.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace dxcb5 {
namespace alg {

// Mirrors DXCB5.RewriteRules.ModeReq from lean/RewriteRules.lean.
enum class SemModeReq : uint8_t {
  Always,
  NotPrecise,
  NotPreciseNotNanSafe,
  NotPreciseAndNonNaNF,
};

inline bool SatisfiesSemMode(SemModeReq Req, bool NanSafe, bool Precise,
                             bool NonNaNF = false) {
  switch (Req) {
  case SemModeReq::Always: return true;
  case SemModeReq::NotPrecise: return !Precise;
  case SemModeReq::NotPreciseNotNanSafe: return !Precise && !NanSafe;
  case SemModeReq::NotPreciseAndNonNaNF:
    return !Precise && (!NanSafe || NonNaNF);
  }
  return false;
}

constexpr uint32_t kPosZeroBits = kFloatPosZeroBits;
constexpr uint32_t kNegZeroBits = kFloatNegZeroBits;
constexpr uint32_t kPosOneBits = kFloatPosOneBits;
constexpr uint32_t kNegOneBits = kFloatNegOneBits;

inline float BitsToFloat(uint32_t Bits) {
  float F;
  std::memcpy(&F, &Bits, sizeof(F));
  return F;
}

inline uint32_t FloatToBits(float F) {
  uint32_t Bits;
  std::memcpy(&Bits, &F, sizeof(Bits));
  return Bits;
}

inline bool TryReciprocalBits(uint32_t Bits, uint32_t &OutBits) {
  const float V = BitsToFloat(Bits);
  if (!std::isfinite(V) || V == 0.0f)
    return false;
  const float R = 1.0f / V;
  if (!std::isfinite(R) || std::fpclassify(R) == FP_SUBNORMAL || R == 0.0f)
    return false;
  OutBits = FloatToBits(R);
  return true;
}

inline bool IsZeroFloatImm(const Operand &Op) {
  return Op.Kind == OperandKind::Immediate &&
         (Op.Index == kPosZeroBits || Op.Index == kNegZeroBits);
}

inline bool IsOneFloatImm(const Operand &Op, bool &Negative) {
  if (Op.Kind != OperandKind::Immediate)
    return false;
  if (Op.Index == kPosOneBits) {
    Negative = false;
    return true;
  }
  if (Op.Index == kNegOneBits) {
    Negative = true;
    return true;
  }
  return false;
}

inline uint32_t ApplyFloatModToBits(uint32_t Bits, uint8_t Modifier) {
  if (Modifier == D3D10_SB_OPERAND_MODIFIER_ABS)
    return Bits & kFloatAbsMask;
  if (Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
    return Bits ^ kFloatSignBit;
  if (Modifier == D3D10_SB_OPERAND_MODIFIER_ABSNEG)
    return Bits | kFloatSignBit;
  return Bits;
}

inline bool IsIdentitySwizzle(uint8_t Swz, uint8_t Mask) {
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if ((Mask & (1u << L)) && GetSwizzleLane(Swz, L) != L)
      return false;
  return true;
}

inline bool IsDynamicCbMov(const SsaInst &Inst) {
  return !Inst.Dead && Inst.Opcode == D3D10_SB_OPCODE_MOV && !Inst.Saturate &&
         !Inst.Sources.empty() &&
         Inst.Sources[0].Kind == OperandKind::ConstantBuffer &&
         !Inst.Sources[0].Modifier &&
         Inst.Sources[0].RelativeRegister != UINT32_MAX;
}

inline bool IsEligibleDynCbConsumer(const SsaInst &Inst) {
  if (IsPseudoOpcode(Inst.Opcode))
    return false;
  if (Inst.Opcode == D3D10_SB_OPCODE_MOV && !Inst.Saturate &&
      Inst.Dest.Kind == OperandKind::Temp)
    return false;
  return InstructionIsComponentwise(Inst.Opcode) ||
         Inst.Opcode == D3D10_SB_OPCODE_DP2 ||
         Inst.Opcode == D3D10_SB_OPCODE_DP3 ||
         Inst.Opcode == D3D10_SB_OPCODE_DP4 ||
         Inst.Opcode == D3D10_SB_OPCODE_IMUL;
}

inline bool InstHasRelativeOperand(const SsaInst &Inst) {
  if (Inst.Dest.RelativeRegister != UINT32_MAX)
    return true;
  for (const auto &S : Inst.Sources)
    if (S.RelativeRegister != UINT32_MAX)
      return true;
  return false;
}

struct UseStats {
  std::vector<uint32_t> Count;
  std::vector<uint8_t> NonFloatUse;
  std::vector<uint8_t> RelativeUse;
  std::vector<uint8_t> NonBoolTestUse;
};

struct AlgContext {
  SsaProgram &Ssa;
  std::vector<SsaDefSite> Defs;
  UseStats Stats;

  void BumpUse(const Operand &Op) {
    if (Op.Kind == OperandKind::Temp && Op.Index < Stats.Count.size())
      ++Stats.Count[Op.Index];
    if (Op.RelativeRegister != UINT32_MAX &&
        Op.RelativeRegister < Stats.Count.size())
      ++Stats.Count[Op.RelativeRegister];
  }

  void RefreshBlockAfterInsert(uint32_t B, uint32_t FirstMovedIdx);
};

void CollectUseStats(const SsaProgram &Ssa, UseStats &Stats);
bool IsBoolMaskValue(const SsaProgram &Ssa,
                     const std::vector<SsaDefSite> &Defs,
                     const Operand &StartOp);
inline bool IsKnownNonNaNFloat(const AlgContext &Ctx, const Operand &Op,
                               uint8_t Mask) {
  if (Op.RelativeRegister != UINT32_MAX)
    return false;
  if (Op.Kind == OperandKind::Immediate)
    return !std::isnan(BitsToFloat(Op.Index));
  if (Op.Kind == OperandKind::ImmediateVector &&
      Op.Index < Ctx.Ssa.Mir.LiteralVectors.size()) {
    const auto &Bits = Ctx.Ssa.Mir.LiteralVectors[Op.Index];
    for (unsigned L = 0; L < kRegisterLanes; ++L)
      if ((Mask & (1u << L)) && std::isnan(BitsToFloat(Bits[L])))
        return false;
    return true;
  }
  if (Op.Kind != OperandKind::Temp)
    return false;
  const auto &D = Ctx.Defs[Op.Index];
  if (!D.Valid() || D.IsPhi || D.IsAux)
    return false;
  const auto &P = Ctx.Ssa.Blocks[D.Block].Insts[D.Index];
  if (P.Dead || !P.Saturate)
    return false;
  for (unsigned L = 0; L < kRegisterLanes; ++L)
    if ((Mask & (1u << L)) &&
        !(P.Dest.Mask & (1u << GetSwizzleLane(Op.Swizzle, L))))
      return false;
  return true;
}

inline bool IsKnownFiniteNonNegFloat(const AlgContext &Ctx, const Operand &Op,
                                     uint8_t Mask) {
  if (Op.RelativeRegister != UINT32_MAX)
    return false;
  if (Op.Kind == OperandKind::Immediate) {
    const float F = BitsToFloat(ApplyFloatModToBits(Op.Index, Op.Modifier));
    return std::isfinite(F) && !std::signbit(F);
  }
  if (Op.Kind == OperandKind::ImmediateVector &&
      Op.Index < Ctx.Ssa.Mir.LiteralVectors.size()) {
    const auto &Bits = Ctx.Ssa.Mir.LiteralVectors[Op.Index];
    for (unsigned L = 0; L < kRegisterLanes; ++L) {
      if (!(Mask & (1u << L)))
        continue;
      const float F = BitsToFloat(ApplyFloatModToBits(Bits[L], Op.Modifier));
      if (!std::isfinite(F) || std::signbit(F))
        return false;
    }
    return true;
  }
  if (Op.Modifier != 0 && Op.Modifier != D3D10_SB_OPERAND_MODIFIER_ABS)
    return false;
  return IsKnownNonNaNFloat(Ctx, Op, Mask);
}
bool MatchZextBool(const AlgContext &Ctx, const Operand &Op, Operand &InnerMask,
                   bool &KnownMask);
bool TryResolveLane(const AlgContext &Ctx, const Operand &Op, unsigned L,
                    bool RestrictBase, Operand &Out);

bool FoldCopiesAndModifiers(SsaProgram &Ssa);
bool FoldBranchCondition(AlgContext &Ctx, SsaBlock &Block);
bool FoldRelativeIndexOffset(AlgContext &Ctx, SsaInst &Inst);
bool ForwardSingleUseDynamicCb(AlgContext &Ctx, uint32_t B, SsaInst &Inst);

bool ExtractLaneImm(const SsaProgram &Ssa, const Operand &Op, unsigned Lane,
                    uint32_t &OutVal);
uint32_t KnownPossibleOnes(const AlgContext &Ctx, const Operand &Op,
                           unsigned Lane, unsigned Depth);
bool TrySimplifyBitfieldAndConstInt(AlgContext &Ctx, SsaInst &Inst);
bool SimplifyIntAndBitInst(AlgContext &Ctx, SsaInst &Inst);
bool SimplifyFloatInst(AlgContext &Ctx, uint32_t B, uint32_t &I);
bool TryReassociateConstFloat(AlgContext &Ctx, SsaInst &Inst);
bool SimplifyAddInst(AlgContext &Ctx, uint32_t B, SsaInst &Inst);
bool SimplifyMadInst(AlgContext &Ctx, SsaInst &Inst);
bool SimplifyMinMaxInst(AlgContext &Ctx, SsaInst &Inst);
bool SimplifyExpInst(AlgContext &Ctx, uint32_t B, uint32_t &I);
bool SimplifyDivInst(AlgContext &Ctx, uint32_t B, SsaInst &Inst);
bool CanonicalizeMulMadNegImm(SsaProgram &Ssa, SsaInst &Inst);
inline bool IsKnownNonNegativeFloat(const AlgContext &Ctx, const Operand &Op,
                                    uint8_t Mask, bool AllowNegZero,
                                    unsigned Depth = 0) {
  return dxcb5::IsKnownNonNegativeFloat(Ctx.Ssa, Ctx.Defs, Op, Mask,
                                        AllowNegZero, Depth);
}
bool SimplifySelectAndDotInst(AlgContext &Ctx, uint32_t B, uint32_t &I);
bool RunGlobalAlgebraicFolds(AlgContext &Ctx);

} // namespace alg
} // namespace dxcb5

#endif
