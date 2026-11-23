// Distributed under the University of Illinois Open Source License.
#include "Mir/Analysis/LaneAnalysis.h"
#include "Mir/Memory/MemoryOpt.h"
#include "Mir/OptPipeline.h"
#include "Mir/OutOfSsa.h"
#include "Mir/PlacementStore.h"
#include "Mir/SsaMir.h"
#include "RegAlloc/Liveness.h"
#include <cassert>
#include <cstdio>
#include <string>

using namespace dxcb5;

namespace {

bool TestCrossBlockModifierAndGvn() {
  MirProgram Mir;
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(3);

  // Block 0: v1 = MOV |v0.x|, v2 = MUL v1, v1, branch v1.x ? Block 1 : Block 2
  // Block 1: v3 = MUL |v0.x|, |v0.x| (duplicate across blocks!), o0.x = ADD v2, v3
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};
  const uint32_t V1 = Ssa.AllocValue();
  const uint32_t V2 = Ssa.AllocValue();
  const uint32_t V3 = Ssa.AllocValue();
  const uint32_t V4 = Ssa.AllocValue();

  Ssa.Blocks[0].Kind = FlowKind::Branch;
  Ssa.Blocks[0].Condition = In1;
  Ssa.Blocks[0].Successors = {1, 2};
  Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V1),
                                 Operand{OperandKind::Null}, {AbsMirOperand(In0)}});
  Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(V2),
                                 Operand{OperandKind::Null},
                                 {MakeSsaTemp(V1), MakeSsaTemp(V1)}});

  Ssa.Blocks[1].Kind = FlowKind::Return;
  Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(V3),
                                 Operand{OperandKind::Null},
                                 {AbsMirOperand(In0), AbsMirOperand(In0)}});
  Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(V4),
                                 Operand{OperandKind::Null},
                                 {MakeSsaTemp(V2), MakeSsaTemp(V3)}});
  Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                 Operand{OperandKind::Output, 0, 0, 1, 0},
                                 Operand{OperandKind::Null}, {MakeSsaTemp(V4)}});
  Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                 Operand{OperandKind::Output, 0, 0, 2, 0},
                                 Operand{OperandKind::Null}, {MakeSsaTemp(V2)}});

  Ssa.Blocks[2].Kind = FlowKind::Return;
  Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                 Operand{OperandKind::Output, 0, 0, 1, 0},
                                 Operand{OperandKind::Null}, {MakeSsaTemp(V2)}});

  OptimizeSsaMir(Ssa);
  std::string Err;
  if (!Ssa.Verify(Err)) {
    std::fprintf(stderr, "SSA verify failed: %s\n", Err.c_str());
    return false;
  }
  const auto &B0 = Ssa.Blocks[0];
  const auto &B1 = Ssa.Blocks[1];
  // B0 should have ONLY 1 MUL (ABS MOV folded), and B1 should have ADD directly
  // writing o0.x (duplicate MUL eliminated by GVN, trailing output MOV folded) + output MOV for o0.y!
  if (B0.Insts.size() != 1 || B0.Insts[0].Opcode != D3D10_SB_OPCODE_MUL ||
      B0.Insts[0].Sources[0].Modifier != D3D10_SB_OPERAND_MODIFIER_ABS) {
    std::fprintf(stderr, "Expected cross-block ABS folding in B0\n");
    return false;
  }
  if (B1.Insts.size() != 2 || B1.Insts[0].Opcode != D3D10_SB_OPCODE_ADD ||
      B1.Insts[0].Dest.Kind != OperandKind::Output ||
      B1.Insts[0].Sources[0].Index != B1.Insts[0].Sources[1].Index) {
    std::fprintf(stderr, "Expected cross-block GVN to reuse V2 in B1\n");
    return false;
  }
  return true;
}

bool TestFSubMadAndSlpPack() {
  MirProgram Mir;
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(1);
  auto &B = Ssa.Blocks[0];
  B.Kind = FlowKind::Return;

  const Operand InA{OperandKind::Input, 0, 0, 0x7, kIdentitySwizzle};
  const Operand InB{OperandKind::Input, 1, 0, 0x7, kIdentitySwizzle};
  const Operand InC{OperandKind::Input, 2, 0, 0x7, kIdentitySwizzle};

  uint32_t OutVals[3];
  for (unsigned L = 0; L < 3; ++L) {
    Operand AL = InA; AL.Mask = 1; AL.Swizzle = BroadcastLaneSwizzle(L);
    Operand BL = InB; BL.Mask = 1; BL.Swizzle = BroadcastLaneSwizzle(L);
    Operand CL = InC; CL.Mask = 1; CL.Swizzle = BroadcastLaneSwizzle(L);
    const uint32_t Prod = Ssa.AllocValue();
    const uint32_t Diff = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(Prod),
                       Operand{OperandKind::Null}, {AL, BL}});
    B.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Diff),
                       Operand{OperandKind::Null},
                       {CL, NegateMirOperand(MakeSsaTemp(Prod))}});
    OutVals[L] = Diff;
  }
  for (unsigned L = 0; L < 3; ++L) {
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0,
                               static_cast<uint8_t>(1u << L),
                               BroadcastLaneSwizzle(L)},
                       Operand{OperandKind::Null}, {MakeSsaTemp(OutVals[L])}});
  }

  OptimizeSsaMir(Ssa);
  std::vector<FlowBlock> Flow;
  LowerOutOfSsa(Ssa, Flow);

  // Expect 1 packed MAD.xyz (with negated first operand) + 1 merged output MOV.xyz (or direct MAD to o0.xyz)!
  unsigned Mads = 0;
  for (const auto &Inst : Flow[0].Instructions) {
    if (Inst.Opcode == D3D10_SB_OPCODE_MAD && Inst.Operands[0].Mask == 0x7)
      ++Mads;
  }
  if (Mads != 1 || Flow[0].Instructions.size() > 2) {
    std::fprintf(stderr, "Expected 1 packed MAD.xyz, got %zu insts (%u mads)\n",
                 Flow[0].Instructions.size(), Mads);
    return false;
  }
  return true;
}

bool TestNotPolarityAndSinCosMerge() {
  MirProgram Mir;
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(3);

  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const uint32_t Cmp = Ssa.AllocValue();
  const uint32_t NotC = Ssa.AllocValue();
  const uint32_t SinV = Ssa.AllocValue();
  const uint32_t CosV = Ssa.AllocValue();
  const uint32_t SumV = Ssa.AllocValue();

  auto &B0 = Ssa.Blocks[0];
  B0.Kind = FlowKind::Branch;
  B0.Successors = {1, 2};
  B0.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(Cmp),
                      Operand{OperandKind::Null}, {In0, MakeImm(0)}});
  B0.Insts.push_back({D3D10_SB_OPCODE_NOT, MakeSsaTemp(NotC),
                      Operand{OperandKind::Null}, {MakeSsaTemp(Cmp)}});
  B0.Insts.push_back({D3D10_SB_OPCODE_SINCOS, MakeSsaTemp(SinV),
                      Operand{OperandKind::Null}, {In0}});
  B0.Condition = MakeSsaTemp(NotC);

  auto &B1 = Ssa.Blocks[1];
  B1.Kind = FlowKind::Return;
  B1.Insts.push_back({D3D10_SB_OPCODE_SINCOS, Operand{OperandKind::Null},
                      MakeSsaTemp(CosV), {In0}});
  B1.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(SumV),
                      Operand{OperandKind::Null},
                      {MakeSsaTemp(SinV), MakeSsaTemp(CosV)}});
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                      Operand{OperandKind::Null}, {MakeSsaTemp(SumV)}});

  auto &B2 = Ssa.Blocks[2];
  B2.Kind = FlowKind::Return;
  B2.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                      Operand{OperandKind::Null}, {MakeSsaTemp(SinV)}});

  OptimizeSsaMir(Ssa);
  if (B0.Successors[0] != 2 || B0.Successors[1] != 1) {
    std::fprintf(stderr, "Expected NOT polarity to invert branch targets\n");
    return false;
  }
  unsigned SinCosCount = 0;
  unsigned NotCount = 0;
  for (const auto &Blk : Ssa.Blocks) {
    for (const auto &I : Blk.Insts) {
      SinCosCount += I.Opcode == D3D10_SB_OPCODE_SINCOS;
      NotCount += I.Opcode == D3D10_SB_OPCODE_NOT;
    }
  }
  if (SinCosCount != 1 || NotCount != 0) {
    std::fprintf(stderr, "Expected 1 merged SINCOS and 0 NOT, got %u/%u\n",
                 SinCosCount, NotCount);
    return false;
  }
  return true;
}

bool TestIfConvertAndMovcModifiers() {
  MirProgram Mir;
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(4);

  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};
  const uint32_t Cond = Ssa.AllocValue();
  const uint32_t NegVal = Ssa.AllocValue();
  const uint32_t PhiVal = Ssa.AllocValue();

  auto &B0 = Ssa.Blocks[0];
  B0.Kind = FlowKind::Branch;
  B0.Successors = {1, 2};
  B0.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(Cond),
                      Operand{OperandKind::Null}, {In0, MakeImm(0)}});
  B0.Condition = MakeSsaTemp(Cond);

  auto &B1 = Ssa.Blocks[1];
  B1.Kind = FlowKind::Jump;
  B1.Successors = {3};
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(NegVal),
                      Operand{OperandKind::Null}, {NegateMirOperand(In1)}});

  auto &B2 = Ssa.Blocks[2];
  B2.Kind = FlowKind::Jump;
  B2.Successors = {3};

  auto &B3 = Ssa.Blocks[3];
  B3.Kind = FlowKind::Return;
  SsaPhi Phi;
  Phi.Result = PhiVal;
  Phi.Mask = 1u;
  Phi.Inputs = {{1, MakeSsaTemp(NegVal)}, {2, In1}};
  B3.Phis.push_back(std::move(Phi));
  B3.Insts.push_back({D3D10_SB_OPCODE_MOV,
                      Operand{OperandKind::Output, 0, 0, 1, 0},
                      Operand{OperandKind::Null}, {MakeSsaTemp(PhiVal)}});

  OptimizeSsaMir(Ssa);
  std::string Err;
  if (!Ssa.Verify(Err)) {
    std::fprintf(stderr, "IfConvert SSA verify failed: %s\n", Err.c_str());
    return false;
  }
  unsigned MovcCount = 0;
  for (const auto &Blk : Ssa.Blocks)
    for (const auto &I : Blk.Insts)
      MovcCount += (I.Opcode == D3D10_SB_OPCODE_MOVC);
  if (Ssa.Blocks[0].Kind != FlowKind::Return || MovcCount != 1) {
    std::fprintf(stderr, "Expected diamond flattened to 1 MOVC, got kind=%d movcs=%u\n",
                 static_cast<int>(Ssa.Blocks[0].Kind), MovcCount);
    return false;
  }
  return true;
}

bool TestAlgebraicIdentitiesAndFolds() {
  MirProgram Mir;
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(1);
  auto &B = Ssa.Blocks[0];
  B.Kind = FlowKind::Return;

  const Operand In0{OperandKind::Input, 0, 0, 0x7, kIdentitySwizzle};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};
  const Operand In2{OperandKind::Input, 2, 0, 1, 0};
  const Operand In3{OperandKind::Input, 3, 0, 1, 0};

  const Operand NullOp{OperandKind::Null};
  // 1. normalize(normalize(v)) must NOT fold dot(norm, norm) to 1.0 (zero vector -> NaN).
  const uint32_t Dp1 = Ssa.AllocValue();
  const uint32_t Rsq1 = Ssa.AllocValue();
  const uint32_t Norm1 = Ssa.AllocValue();
  const uint32_t Dp2 = Ssa.AllocValue();
  const uint32_t Rsq2 = Ssa.AllocValue();
  B.Insts.push_back({D3D10_SB_OPCODE_DP3, MakeSsaTemp(Dp1), NullOp, {In0, In0}});
  B.Insts.push_back({D3D10_SB_OPCODE_RSQ, MakeSsaTemp(Rsq1), NullOp, {MakeSsaTemp(Dp1)}});
  B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(Norm1, 0x7, kIdentitySwizzle), NullOp,
                     {In0, MakeSsaTemp(Rsq1, 0x7, 0)}});
  B.Insts.push_back({D3D10_SB_OPCODE_DP3, MakeSsaTemp(Dp2), NullOp,
                     {MakeSsaTemp(Norm1, 0x7, kIdentitySwizzle),
                      MakeSsaTemp(Norm1, 0x7, kIdentitySwizzle)}});
  B.Insts.push_back({D3D10_SB_OPCODE_RSQ, MakeSsaTemp(Rsq2), NullOp, {MakeSsaTemp(Dp2)}});
  B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                     {MakeSsaTemp(Rsq2)}});

  // 2. pow(abs(x), -0.25) = EXP(MUL(LOG(|x|), -0.25)) -> RSQ(SQRT(|x|))!
  const uint32_t LogV = Ssa.AllocValue();
  const uint32_t MulPow = Ssa.AllocValue();
  const uint32_t ExpV = Ssa.AllocValue();
  B.Insts.push_back({D3D10_SB_OPCODE_LOG, MakeSsaTemp(LogV), NullOp, {AbsMirOperand(In1)}});
  B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(MulPow), NullOp,
                     {MakeSsaTemp(LogV), MakeImm(0xbe800000u)}}); // -0.25f
  B.Insts.push_back({D3D10_SB_OPCODE_EXP, MakeSsaTemp(ExpV), NullOp, {MakeSsaTemp(MulPow)}});
  B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                     {MakeSsaTemp(ExpV)}});

  // 3. OR(AND(c, a), AND(NOT(c), b)) -> MOVC(c, a, b)!
  const uint32_t Cmp = Ssa.AllocValue();
  const uint32_t NotC = Ssa.AllocValue();
  const uint32_t AndA = Ssa.AllocValue();
  const uint32_t AndB = Ssa.AllocValue();
  const uint32_t SelOr = Ssa.AllocValue();
  B.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(Cmp), NullOp, {In1, MakeImm(0)}});
  B.Insts.push_back({D3D10_SB_OPCODE_NOT, MakeSsaTemp(NotC), NullOp, {MakeSsaTemp(Cmp)}});
  B.Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(AndA), NullOp, {MakeSsaTemp(Cmp), In2}});
  B.Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(AndB), NullOp, {MakeSsaTemp(NotC), In3}});
  B.Insts.push_back({D3D10_SB_OPCODE_OR, MakeSsaTemp(SelOr), NullOp,
                     {MakeSsaTemp(AndA), MakeSsaTemp(AndB)}});
  B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 2, 0, 1, 0}, NullOp,
                     {MakeSsaTemp(SelOr)}});

  // 4. FTOU(MOVC(c, 2.0f, 5.0f)) -> MOVC(c, 2u, 5u) with zero FTOU!
  const uint32_t FSel = Ssa.AllocValue();
  const uint32_t USel = Ssa.AllocValue();
  B.Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(FSel), NullOp,
                     {MakeSsaTemp(Cmp), MakeImm(0x40000000u), MakeImm(0x40a00000u)}});
  B.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(USel), NullOp, {MakeSsaTemp(FSel)}});
  B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 3, 0, 1, 0}, NullOp,
                     {MakeSsaTemp(USel)}});

  // 5. KnownPossibleOnes OR(ISHL(idx, 4), 3) folded into relative CB offset +3!
  const uint32_t ShIdx = Ssa.AllocValue();
  const uint32_t OrIdx = Ssa.AllocValue();
  const uint32_t CbLoad = Ssa.AllocValue();
  B.Insts.push_back({D3D10_SB_OPCODE_ISHL, MakeSsaTemp(ShIdx), NullOp, {In2, MakeImm(4)}});
  B.Insts.push_back({D3D10_SB_OPCODE_OR, MakeSsaTemp(OrIdx), NullOp,
                     {MakeSsaTemp(ShIdx), MakeImm(3)}});
  Operand DynCb{OperandKind::ConstantBuffer, 0, 10, 1, 0, 0, OrIdx, 0};
  B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(CbLoad), NullOp, {DynCb}});
  B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 4, 0, 1, 0}, NullOp,
                     {MakeSsaTemp(CbLoad)}});

  OptimizeSsaMir(Ssa);
  std::string Err;
  if (!Ssa.Verify(Err)) {
    std::fprintf(stderr, "Algebraic SSA verify failed: %s\n", Err.c_str());
    return false;
  }
  unsigned LogCount = 0, ExpCount = 0, FtouCount = 0, OrCount = 0, Dp3Count = 0;
  bool FoundCbOffset13 = false;
  for (const auto &I : B.Insts) {
    LogCount += I.Opcode == D3D10_SB_OPCODE_LOG;
    ExpCount += I.Opcode == D3D10_SB_OPCODE_EXP;
    FtouCount += I.Opcode == D3D10_SB_OPCODE_FTOU;
    OrCount += I.Opcode == D3D10_SB_OPCODE_OR;
    Dp3Count += I.Opcode == D3D10_SB_OPCODE_DP3;
    for (const auto &S : I.Sources)
      if (S.Kind == OperandKind::ConstantBuffer && S.Element == 13 &&
          S.RelativeRegister == ShIdx)
        FoundCbOffset13 = true;
  }
  if (LogCount != 0 || ExpCount != 0 || FtouCount != 0 || OrCount != 0 ||
      Dp3Count != 2 || !FoundCbOffset13) {
    std::fprintf(stderr,
                 "Algebraic folds missed: log=%u exp=%u ftou=%u or=%u dp3=%u cb13=%d\n",
                 LogCount, ExpCount, FtouCount, OrCount, Dp3Count, FoundCbOffset13);
    return false;
  }
  return true;
}

bool TestDualDestGvnAndMovcNarrow() {
  MirProgram Mir;
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(1);
  auto &B = Ssa.Blocks[0];
  B.Kind = FlowKind::Return;

  const Operand In0{OperandKind::Input, 0, 0, 0x3, kIdentitySwizzle};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};

  const Operand NullOp{OperandKind::Null};
  // Duplicate dual-dest SINCOS on same input should GVN into 1 SINCOS!
  const uint32_t S1 = Ssa.AllocValue(), C1 = Ssa.AllocValue();
  const uint32_t S2 = Ssa.AllocValue(), C2 = Ssa.AllocValue();
  const uint32_t Sum1 = Ssa.AllocValue(), Sum2 = Ssa.AllocValue();
  B.Insts.push_back({D3D10_SB_OPCODE_SINCOS, MakeSsaTemp(S1), MakeSsaTemp(C1), {In1}});
  B.Insts.push_back({D3D10_SB_OPCODE_SINCOS, MakeSsaTemp(S2), MakeSsaTemp(C2), {In1}});
  B.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Sum1), NullOp,
                     {MakeSsaTemp(S1), MakeSsaTemp(C2)}});
  B.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Sum2), NullOp,
                     {MakeSsaTemp(S2), MakeSsaTemp(C1)}});

  // Vector MOVC where lane .y is unchanged (In0.y in both arms) should narrow to scalar!
  const uint32_t Comb = Ssa.AllocValue();
  const uint32_t Sel = Ssa.AllocValue();
  Operand In0Y = In0;
  In0Y.Mask = 1;
  In0Y.Swizzle = BroadcastLaneSwizzle(1);
  B.Insts.push_back({PseudoCombineVec, MakeSsaTemp(Comb, 0x3, kIdentitySwizzle), NullOp,
                     {MakeSsaTemp(Sum1), In0Y}});
  B.Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Sel, 0x3, kIdentitySwizzle), NullOp,
                     {MakeSsaTemp(Sum2, 0x3, 0), MakeSsaTemp(Comb, 0x3, kIdentitySwizzle),
                      In0}});
  B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                     {MakeSsaTemp(Sel, 1, 0)}});
  B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 2, 0}, NullOp,
                     {MakeSsaTemp(Sel, 1, BroadcastLaneSwizzle(1))}});

  OptimizeSsaMir(Ssa);
  unsigned SinCosCount = 0;
  unsigned ScalarMovc = 0;
  for (const auto &I : B.Insts) {
    SinCosCount += I.Opcode == D3D10_SB_OPCODE_SINCOS;
    if (I.Opcode == D3D10_SB_OPCODE_MOVC && PopcountMask(I.Dest.Mask) == 1)
      ++ScalarMovc;
  }
  if (SinCosCount != 1 || ScalarMovc != 1) {
    std::fprintf(stderr, "Expected 1 SINCOS and 1 scalar MOVC, got %u/%u\n",
                 SinCosCount, ScalarMovc);
    return false;
  }
  return true;
}

bool TestPromoteLocalArraysUdivAndDp2() {
  MirProgram Mir;
  Mir.IndexableTemps = {{0u, 4u, 1u}, {1u, 4u, 1u}};
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(2);

  const Operand In0{OperandKind::Input, 0, 0, 0xf, kIdentitySwizzle};
  const Operand In1{OperandKind::Input, 1, 0, 0xf, kIdentitySwizzle};
  const Operand In2{OperandKind::Input, 2, 0, 0xf, kIdentitySwizzle};
  const Operand In3{OperandKind::Input, 3, 0, 0xf, kIdentitySwizzle};
  const Operand Sel{OperandKind::Input, 4, 0, 1, 0};
  const Operand Den{OperandKind::Input, 5, 0, 1, 0};
  const Operand NullOp{OperandKind::Null};

  auto LaneOf = [](Operand Op, unsigned L) {
    Op.Mask = 1;
    Op.Swizzle = BroadcastLaneSwizzle(L);
    return Op;
  };

  auto &B0 = Ssa.Blocks[0];
  B0.Kind = FlowKind::Jump;
  B0.Successors = {1};
  const Operand Vecs[4] = {In0, In1, In2, In3};
  for (uint32_t L = 0; L < 4; ++L) {
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::IndexableTemp, 0, L, 1, 0},
                        NullOp,
                        {LaneOf(Vecs[L], 0)}});
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::IndexableTemp, 1, L, 1, 0},
                        NullOp,
                        {LaneOf(Vecs[L], 1)}});
  }

  auto &B1 = Ssa.Blocks[1];
  B1.Kind = FlowKind::Return;
  const uint32_t SelReg = Ssa.AllocValue();
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(SelReg), NullOp, {Sel}});
  const uint32_t Rx = Ssa.AllocValue();
  const uint32_t Ry = Ssa.AllocValue();
  Operand SrcX0{OperandKind::IndexableTemp, 0, 0, 1, 0};
  SrcX0.RelativeRegister = SelReg;
  Operand SrcX1{OperandKind::IndexableTemp, 1, 0, 1, 0};
  SrcX1.RelativeRegister = SelReg;
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(Rx), NullOp, {SrcX0}});
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(Ry), NullOp, {SrcX1}});

  // Dual-dest UDIV merge: separate quotient and remainder -> single UDIV.
  const uint32_t Quot = Ssa.AllocValue();
  const uint32_t Rem = Ssa.AllocValue();
  B1.Insts.push_back({D3D10_SB_OPCODE_UDIV, MakeSsaTemp(Quot), NullOp, {Sel, Den}});
  B1.Insts.push_back({D3D10_SB_OPCODE_UDIV, NullOp, MakeSsaTemp(Rem), {Sel, Den}});

  // Post-SLP DP2: standalone scalar MUL(In0.x, In0.x) + MAD(In0.y, In0.y, mul) -> DP2.
  const uint32_t SqX = Ssa.AllocValue();
  const uint32_t Len2 = Ssa.AllocValue();
  B1.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(SqX), NullOp,
                      {LaneOf(In0, 0), LaneOf(In0, 0)}});
  B1.Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(Len2), NullOp,
                      {LaneOf(In0, 1), LaneOf(In0, 1), MakeSsaTemp(SqX)}});

  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                      NullOp, {MakeSsaTemp(Rx)}});
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 2, 0},
                      NullOp, {MakeSsaTemp(Ry)}});
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0},
                      NullOp, {MakeSsaTemp(Quot)}});
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 2, 0},
                      NullOp, {MakeSsaTemp(Rem)}});
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 2, 0, 1, 0},
                      NullOp, {MakeSsaTemp(Len2)}});

  OptimizeSsaMir(Ssa);
  std::string Err;
  if (!Ssa.Verify(Err)) {
    std::fprintf(stderr, "Promote/UDIV/DP2 SSA verify failed: %s\n", Err.c_str());
    return false;
  }
  unsigned PseudoArr = 0, IeqCount = 0, UdivCount = 0, Dp2Count = 0, VecMovc = 0;
  for (const auto &Blk : Ssa.Blocks) {
    for (const auto &I : Blk.Insts) {
      if (I.Dest.Kind == OperandKind::IndexableTemp)
        ++PseudoArr;
      for (const auto &S : I.Sources)
        if (S.Kind == OperandKind::IndexableTemp)
          ++PseudoArr;
      if (I.Opcode == D3D10_SB_OPCODE_IEQ)
        ++IeqCount;
      if (I.Opcode == D3D10_SB_OPCODE_UDIV)
        ++UdivCount;
      if (I.Opcode == D3D10_SB_OPCODE_DP2)
        ++Dp2Count;
      if (I.Opcode == D3D10_SB_OPCODE_MOVC && PopcountMask(I.Dest.Mask) == 2)
        ++VecMovc;
    }
  }
  if (PseudoArr != 0 || IeqCount != 1 || UdivCount != 1 || Dp2Count != 1 ||
      VecMovc != 3) {
    std::fprintf(stderr,
                 "Expected pseudo=0 ieq=1 udiv=1 dp2=1 vecMovc=3, got %u/%u/%u/%u/%u\n",
                 PseudoArr, IeqCount, UdivCount, Dp2Count, VecMovc);
    return false;
  }
  return true;
}

bool TestOptimizeIndexableTemps() {
  MirProgram Mir;
  Mir.ShaderStage = Stage::Pixel;
  Mir.IndexableTemps.push_back({0, 8, 1}); // x0[8] - partially live (survives for dyn read)
  Mir.IndexableTemps.push_back({1, 8, 1}); // x1[8] - completely unread
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(3);

  const Operand In0{OperandKind::Input, 0, 0, 0xf, 0xe4};
  const Operand NullOp{OperandKind::Null};
  auto LaneOf = [](Operand Op, uint8_t Lane) {
    Op.Swizzle = static_cast<uint8_t>(Lane * 0x55u);
    return Op;
  };
  auto MakeX = [](uint32_t XId, uint32_t Elem, uint32_t RelReg = UINT32_MAX) {
    Operand Op{OperandKind::IndexableTemp, XId, Elem, 1, 0};
    Op.RelativeRegister = RelReg;
    return Op;
  };

  // Block 0: zero-init x0[0..1] and x1[0], jump to Block 1.
  auto &B0 = Ssa.Blocks[0];
  B0.Kind = FlowKind::Jump;
  B0.Successors = {1};
  // Dead store: x0[0] is overwritten in B1 before any read.
  B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeX(0, 0), NullOp, {MakeImm(0)}});
  // Live store: x0[1] is read in B2 (const) and dynamically.
  B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeX(0, 1), NullOp, {LaneOf(In0, 1)}});
  // Unread array x1: should be eliminated and compacted away.
  B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeX(1, 0), NullOp, {LaneOf(In0, 0)}});

  // Block 1: overwrite x0[0] = In0.y (non-x lane to test splat normalization!),
  // also write x0[2] = 0x3f800000 (1.0f), jump to Block 2.
  auto &B1 = Ssa.Blocks[1];
  B1.Kind = FlowKind::Jump;
  B1.Successors = {2};
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeX(0, 0), NullOp, {LaneOf(In0, 2)}});
  B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeX(0, 2), NullOp, {MakeImm(0x3f800000u)}});

  // Block 2: constant read of x0[0] (should forward to In0.z) + dynamic read of x0[idx].
  auto &B2 = Ssa.Blocks[2];
  B2.Kind = FlowKind::Return;
  const uint32_t Idx = Ssa.AllocValue();
  B2.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {LaneOf(In0, 3)}});
  const uint32_t ConstLoad = Ssa.AllocValue();
  const uint32_t DynLoad = Ssa.AllocValue();
  B2.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(ConstLoad), NullOp, {MakeX(0, 0)}});
  B2.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(DynLoad), NullOp, {MakeX(0, 0, Idx)}});
  B2.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                      NullOp, {MakeSsaTemp(ConstLoad)}});
  B2.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 2, 0},
                      NullOp, {MakeSsaTemp(DynLoad)}});

  OptimizeSsaMir(Ssa);
  std::string Err;
  if (!Ssa.Verify(Err)) {
    std::fprintf(stderr, "OptimizeIndexableTemps SSA verify failed: %s\n", Err.c_str());
    return false;
  }
  if (Mir.IndexableTemps.size() != 1 || Mir.IndexableTemps[0].Index != 0) {
    std::fprintf(stderr, "Expected 1 compacted IndexableTemp, got %zu\n",
                 Mir.IndexableTemps.size());
    return false;
  }
  unsigned ConstReads = 0, DynReads = 0, Stores = 0;
  for (const auto &Blk : Ssa.Blocks) {
    for (const auto &I : Blk.Insts) {
      if (I.Dest.Kind == OperandKind::IndexableTemp)
        ++Stores;
      for (const auto &S : I.Sources) {
        if (S.Kind == OperandKind::IndexableTemp) {
          if (S.RelativeRegister == UINT32_MAX)
            ++ConstReads;
          else
            ++DynReads;
        }
      }
    }
  }
  // B0's x0[0]=0 and x1[0]=In0.x must be dead; x0[1], x0[0]=In0.z, x0[2] remain
  // live for the dynamic read, and the constant read of x0[0] must be forwarded.
  if (ConstReads != 0 || DynReads != 1 || Stores != 3) {
    std::fprintf(stderr,
                 "Expected constReads=0 dynReads=1 stores=3, got %u/%u/%u\n",
                 ConstReads, DynReads, Stores);
    return false;
  }
  return true;
}

bool TestCorrectnessRegressions() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};
  const Operand In2{OperandKind::Input, 2, 0, 1, 0};

  // 1. Duplicate identical indexable-temp writes before cross-block dynamic read
  // must retain one live write (never delete both).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 8, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(2);
    Operand X0{OperandKind::IndexableTemp, 0, 0, 1, 0};
    Ssa.Blocks[0].Kind = FlowKind::Jump;
    Ssa.Blocks[0].Successors = {1};
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MOV, X0, NullOp, {MakeImm(42)}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MOV, X0, NullOp, {MakeImm(42)}});
    Ssa.Blocks[1].Kind = FlowKind::Return;
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t Val = Ssa.AllocValue();
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In0}});
    Operand DynX = X0;
    DynX.RelativeRegister = Idx;
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(Val), NullOp, {DynX}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 0, 0, 1, 0},
                                   NullOp, {MakeSsaTemp(Val)}});
    RunOptIndexableTemps(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "DupStore SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned Stores = 0;
    for (const auto &Blk : Ssa.Blocks)
      for (const auto &I : Blk.Insts)
        Stores += (I.Dest.Kind == OperandKind::IndexableTemp);
    if (Stores != 1) {
      std::fprintf(stderr, "Expected 1 surviving store to x0[0], got %u\n", Stores);
      return false;
    }
  }

  // 2. Dynamic indexable-temp write index also written to shader output must
  // NOT be retargeted into o0 during OutOfSsa (leaving x0[r_idx] undefined).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 4, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t Loaded = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(Idx), NullOp, {In0, MakeImm(1)}});
    Operand XDst{OperandKind::IndexableTemp, 0, 0, 1, 0};
    XDst.RelativeRegister = Idx;
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, XDst, NullOp, {In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                       NullOp, {MakeSsaTemp(Idx)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(Loaded), NullOp,
                       {Operand{OperandKind::IndexableTemp, 0, 0, 1, 0}}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0},
                       NullOp, {MakeSsaTemp(Loaded)}});
    std::vector<FlowBlock> Flow;
    LowerOutOfSsa(Ssa, Flow);
    bool DefFound = false;
    uint32_t RelReg = UINT32_MAX;
    for (const auto &Inst : Flow[0].Instructions) {
      if (Inst.Operands[0].Kind == OperandKind::IndexableTemp &&
          Inst.Operands[0].RelativeRegister != UINT32_MAX)
        RelReg = Inst.Operands[0].RelativeRegister;
    }
    for (const auto &Inst : Flow[0].Instructions) {
      if (Inst.Operands[0].Kind == OperandKind::Temp &&
          Inst.Operands[0].Index == RelReg)
        DefFound = true;
    }
    if (RelReg == UINT32_MAX || !DefFound) {
      std::fprintf(stderr, "Dynamic array write index lost definition in OutOfSsa\n");
      return false;
    }
  }

  // 3. Unsound float folds must NOT fire:
  //    (a) exp2(2 * log2(x)) for unrestricted x (negative x -> NaN)
  //    (b) MOVC(LT(2.0, x), 2.0, x) (x = NaN -> NaN, not 2.0)
  //    (c) MAD(b - a, MOVC(c, 1.0, 0.0), a) (finite a=2^24, b=1 -> 0, not 1)
  //    While sound MOVC(LT(x, 2.0), x, 2.0) DOES fold to MIN(x, 2.0).
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    Ssa.NanSafe = true;
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;

    // (a) unsigned/signed pow(x, 2) without non-negative proof
    const uint32_t LogX = Ssa.AllocValue();
    const uint32_t Mul2 = Ssa.AllocValue();
    const uint32_t Exp2 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_LOG, MakeSsaTemp(LogX), NullOp, {In0}});
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(Mul2), NullOp,
                       {MakeSsaTemp(LogX), MakeImm(0x40000000u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_EXP, MakeSsaTemp(Exp2), NullOp, {MakeSsaTemp(Mul2)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                       NullOp, {MakeSsaTemp(Exp2)}});

    // (b) unsafe (2 < x ? 2 : x) vs safe (x < 2 ? x : 2)
    const uint32_t CmpUnsafe = Ssa.AllocValue();
    const uint32_t SelUnsafe = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(CmpUnsafe), NullOp,
                       {MakeImm(0x40000000u), In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(SelUnsafe), NullOp,
                       {MakeSsaTemp(CmpUnsafe), MakeImm(0x40000000u), In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0},
                       NullOp, {MakeSsaTemp(SelUnsafe)}});

    const uint32_t CmpSafe = Ssa.AllocValue();
    const uint32_t SelSafe = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(CmpSafe), NullOp,
                       {In2, MakeImm(0x40000000u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(SelSafe), NullOp,
                       {MakeSsaTemp(CmpSafe), In2, MakeImm(0x40000000u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 2, 0, 1, 0},
                       NullOp, {MakeSsaTemp(SelSafe)}});

    // (c) MAD(In1 - In0, MOVC(CmpUnsafe, 1.0, 0.0), In0)
    const uint32_t Diff = Ssa.AllocValue();
    const uint32_t OneZero = Ssa.AllocValue();
    const uint32_t Lerp = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Diff), NullOp,
                       {In1, NegateMirOperand(In0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(OneZero), NullOp,
                       {MakeSsaTemp(CmpUnsafe), MakeImm(0x3f800000u), MakeImm(0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(Lerp), NullOp,
                       {MakeSsaTemp(Diff), MakeSsaTemp(OneZero), In0}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 3, 0, 1, 0},
                       NullOp, {MakeSsaTemp(Lerp)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "FloatRegressions SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned LogC = 0, ExpC = 0, MinC = 0, MovcC = 0, MadC = 0;
    for (const auto &I : B.Insts) {
      LogC += I.Opcode == D3D10_SB_OPCODE_LOG;
      ExpC += I.Opcode == D3D10_SB_OPCODE_EXP;
      MinC += I.Opcode == D3D10_SB_OPCODE_MIN;
      MovcC += I.Opcode == D3D10_SB_OPCODE_MOVC;
      MadC += I.Opcode == D3D10_SB_OPCODE_MAD;
    }
    if (LogC != 1 || ExpC != 1 || MinC != 1 || MovcC < 2 || MadC != 1) {
      std::fprintf(stderr,
                   "Unexpected float fold results: log=%u exp=%u min=%u movc=%u mad=%u\n",
                   LogC, ExpC, MinC, MovcC, MadC);
      return false;
    }
  }

  // 4. NegateMirOperand / AbsMirOperand on ImmediateVector folds sign bits directly.
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    const std::array<uint32_t, 4> Pos{0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
    const Operand Vec{OperandKind::ImmediateVector, Ssa.InternLiteralVector(Pos), 0, 0xf,
                      kIdentitySwizzle};
    const Operand Neg = NegateMirOperand(Ssa, Vec);
    const Operand Back = AbsMirOperand(Ssa, Neg);
    if (Neg.Modifier != 0 || Back.Modifier != 0 ||
        Mir.LiteralVectors[Neg.Index][0] != 0xbf800000u ||
        Mir.LiteralVectors[Neg.Index][3] != 0xc0800000u ||
        Mir.LiteralVectors[Back.Index] != Pos) {
      std::fprintf(stderr, "ImmediateVector negate/abs failed to fold bits\n");
      return false;
    }
  }

  return true;
}

bool TestIndexableCopyAndSsaPromotion() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};
  const Operand In2{OperandKind::Input, 2, 0, 1, 0};

  // 1. Read-only copy array x1[6] = x0[6] with no intervening write to x0
  // must be forwarded to x0 and eliminated.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 6, 1});
    Mir.IndexableTemps.push_back({1, 6, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(2);
    auto &B0 = Ssa.Blocks[0];
    B0.Kind = FlowKind::Jump;
    B0.Successors = {1};
    for (uint32_t E = 0; E < 6; ++E)
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                          Operand{OperandKind::IndexableTemp, 0, E, 1, 0},
                          NullOp, {MakeImm(E + 10)}});
    for (uint32_t E = 0; E < 6; ++E) {
      const uint32_t T = Ssa.AllocValue();
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(T), NullOp,
                          {Operand{OperandKind::IndexableTemp, 0, E, 1, 0}}});
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                          Operand{OperandKind::IndexableTemp, 1, E, 1, 0},
                          NullOp, {MakeSsaTemp(T)}});
    }
    auto &B1 = Ssa.Blocks[1];
    B1.Kind = FlowKind::Return;
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t Val = Ssa.AllocValue();
    B1.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In0}});
    Operand DynX1{OperandKind::IndexableTemp, 1, 0, 1, 0};
    DynX1.RelativeRegister = Idx;
    B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(Val), NullOp, {DynX1}});
    B1.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                        {MakeSsaTemp(Val)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err) || Mir.IndexableTemps.size() != 1) {
      std::fprintf(stderr, "CopyForward failed: err='%s' arrays=%zu\n",
                   Err.c_str(), Mir.IndexableTemps.size());
      return false;
    }
  }

  // 2. Copy array x1[6] = x0[6] when x0 IS modified before x1[idx] is read
  // must NOT be forwarded to x0.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 6, 1});
    Mir.IndexableTemps.push_back({1, 6, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B0 = Ssa.Blocks[0];
    B0.Kind = FlowKind::Return;
    for (uint32_t E = 0; E < 6; ++E)
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                          Operand{OperandKind::IndexableTemp, 0, E, 1, 0},
                          NullOp, {MakeImm(E + 10)}});
    for (uint32_t E = 0; E < 6; ++E) {
      const uint32_t T = Ssa.AllocValue();
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(T), NullOp,
                          {Operand{OperandKind::IndexableTemp, 0, E, 1, 0}}});
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                          Operand{OperandKind::IndexableTemp, 1, E, 1, 0},
                          NullOp, {MakeSsaTemp(T)}});
    }
    // Mutate x0[0] before reading x1[idx]!
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::IndexableTemp, 0, 0, 1, 0},
                        NullOp, {In1}});
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t V0 = Ssa.AllocValue();
    const uint32_t V1 = Ssa.AllocValue();
    B0.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In0}});
    Operand DynX0{OperandKind::IndexableTemp, 0, 0, 1, 0};
    DynX0.RelativeRegister = Idx;
    Operand DynX1{OperandKind::IndexableTemp, 1, 0, 1, 0};
    DynX1.RelativeRegister = Idx;
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V0), NullOp, {DynX0}});
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V1), NullOp, {DynX1}});
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                        {MakeSsaTemp(V0)}});
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                        {MakeSsaTemp(V1)}});
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    if (RunOptIndexableCopy(Ssa, Dom)) {
      std::fprintf(stderr, "CopyForward unsoundly forwarded mutated source\n");
      return false;
    }
  }

  // 3. [2 x float] dynamic-store / constant-load array promotes completely out
  // of IndexableTemp memory into SSA IEQ + MOVC.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 2, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t R0 = Ssa.AllocValue();
    const uint32_t R1 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In0}});
    Operand DynDst{OperandKind::IndexableTemp, 0, 0, 1, 0};
    DynDst.RelativeRegister = Idx;
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, DynDst, NullOp, {In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(R0), NullOp,
                       {Operand{OperandKind::IndexableTemp, 0, 0, 1, 0}}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(R1), NullOp,
                       {Operand{OperandKind::IndexableTemp, 0, 1, 1, 0}}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                        {MakeSsaTemp(R0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                        {MakeSsaTemp(R1)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err) || !Mir.IndexableTemps.empty()) {
      std::fprintf(stderr, "[2 x float] dyn-store promotion failed: err='%s' arrays=%zu\n",
                   Err.c_str(), Mir.IndexableTemps.size());
      return false;
    }
  }

  // 4. DomSpill: array written in diamond branches (more stores than elements)
  // before a single LCA dynamic read spills once at LCA and drops pre-LCA stores.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 6, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(4);
    const uint32_t Cond = Ssa.AllocValue();
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Successors = {1, 2};
    for (uint32_t E = 0; E < 6; ++E)
      Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                     Operand{OperandKind::IndexableTemp, 0, E, 1, 0},
                                     NullOp, {MakeImm(E)}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(Cond), NullOp,
                                   {In0, MakeImm(0)}});
    Ssa.Blocks[0].Condition = MakeSsaTemp(Cond);
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {3};
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::IndexableTemp, 0, 0, 1, 0},
                                   NullOp, {In1}});
    Ssa.Blocks[2].Kind = FlowKind::Jump;
    Ssa.Blocks[2].Successors = {3};
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::IndexableTemp, 0, 0, 1, 0},
                                   NullOp, {In2}});
    Ssa.Blocks[3].Kind = FlowKind::Return;
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t Val = Ssa.AllocValue();
    Ssa.Blocks[3].Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In0}});
    Operand DynX{OperandKind::IndexableTemp, 0, 0, 1, 0};
    DynX.RelativeRegister = Idx;
    Ssa.Blocks[3].Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(Val), NullOp, {DynX}});
    Ssa.Blocks[3].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 0, 0, 1, 0},
                                   NullOp, {MakeSsaTemp(Val)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "DomSpill SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned Stores = 0;
    for (const auto &Blk : Ssa.Blocks)
      for (const auto &I : Blk.Insts)
        Stores += (I.Dest.Kind == OperandKind::IndexableTemp);
    if (Stores != 6) {
      std::fprintf(stderr, "DomSpill expected 6 LCA stores (down from 8), got %u\n",
                   Stores);
      return false;
    }
  }

  return true;
}

bool TestIndexablePack() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In0Y{OperandKind::Input, 0, 0, 1, 0x55};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};

  // 1. Two sibling [6 x float] arrays with shared dynamic index and mergeable
  // constant stores pack into one [6 x 2] array with vectorized load & stores.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 6, 1});
    Mir.IndexableTemps.push_back({1, 6, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    // Same-register input store on elem 0 (v0.x / v0.y), immediate stores on 1..5.
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::IndexableTemp, 0, 0, 1, 0}, NullOp,
                       {In0}});
    for (uint32_t E = 1; E < 6; ++E)
      B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                         Operand{OperandKind::IndexableTemp, 0, E, 1, 0},
                         NullOp, {MakeImm(E + 10)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::IndexableTemp, 1, 0, 1, 0}, NullOp,
                       {In0Y}});
    for (uint32_t E = 1; E < 6; ++E)
      B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                         Operand{OperandKind::IndexableTemp, 1, E, 1, 0},
                         NullOp, {MakeImm(E + 20)}});
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t V0 = Ssa.AllocValue();
    const uint32_t V1 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In1}});
    Operand Dyn0{OperandKind::IndexableTemp, 0, 0, 1, 0};
    Dyn0.RelativeRegister = Idx;
    Operand Dyn1{OperandKind::IndexableTemp, 1, 0, 1, 0};
    Dyn1.RelativeRegister = Idx;
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V0), NullOp, {Dyn0}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V1), NullOp, {Dyn1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(V0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(V1)}});
    std::string Err;
    if (!RunOptIndexablePack(Ssa) || !Ssa.Verify(Err) ||
        Mir.IndexableTemps.size() != 1 ||
        Mir.IndexableTemps[0].Components != 2) {
      std::fprintf(stderr, "IndexablePack failed: err='%s' count=%zu\n",
                   Err.c_str(), Mir.IndexableTemps.size());
      return false;
    }
    unsigned VecStores = 0, VecLoads = 0;
    for (const auto &I : B.Insts) {
      if (I.Dest.Kind == OperandKind::IndexableTemp && I.Dest.Mask == 0x3u)
        ++VecStores;
      if (I.Dest.Kind == OperandKind::Temp && I.Dest.Mask == 0x3u &&
          I.Sources.size() == 1 &&
          I.Sources[0].Kind == OperandKind::IndexableTemp)
        ++VecLoads;
    }
    if (VecStores != 6 || VecLoads != 1) {
      std::fprintf(stderr, "IndexablePack expected 6 vec stores & 1 vec load, got %u/%u\n",
                   VecStores, VecLoads);
      return false;
    }
  }

  // 2. Intervening store between paired loads must block load hoisting/merging.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 6, 1});
    Mir.IndexableTemps.push_back({1, 6, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    for (uint32_t E = 0; E < 6; ++E)
      B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                         Operand{OperandKind::IndexableTemp, 0, E, 1, 0},
                         NullOp, {MakeImm(E)}});
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t V0 = Ssa.AllocValue();
    const uint32_t V1 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In1}});
    Operand Dyn0{OperandKind::IndexableTemp, 0, 0, 1, 0};
    Dyn0.RelativeRegister = Idx;
    Operand Dyn1{OperandKind::IndexableTemp, 1, 0, 1, 0};
    Dyn1.RelativeRegister = Idx;
    // Load x0 BEFORE x1 is initialized!
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V0), NullOp, {Dyn0}});
    for (uint32_t E = 0; E < 6; ++E)
      B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                         Operand{OperandKind::IndexableTemp, 1, E, 1, 0},
                         NullOp, {MakeImm(E + 100)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V1), NullOp, {Dyn1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(V0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(V1)}});
    std::string Err;
    RunOptIndexablePack(Ssa);
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "IndexablePack barrier verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned DynLoads = 0;
    for (const auto &I : B.Insts)
      if (I.Sources.size() == 1 &&
          I.Sources[0].Kind == OperandKind::IndexableTemp &&
          I.Sources[0].RelativeRegister != UINT32_MAX)
        ++DynLoads;
    if (DynLoads != 2) {
      std::fprintf(stderr, "IndexablePack unsoundly merged loads across store (%u)\n",
                   DynLoads);
      return false;
    }
  }
  return true;
}

bool TestRound2Regressions() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};

  // P1 #1: Chained array copy x1=x2; x0=x1 followed by x2[0]=20 then read x0[0]
  // in a successor block must NOT forward x0 all the way to mutated x2.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 2, 1});
    Mir.IndexableTemps.push_back({1, 2, 1});
    Mir.IndexableTemps.push_back({2, 2, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(2);
    auto &B0 = Ssa.Blocks[0];
    B0.Kind = FlowKind::Jump;
    B0.Successors = {1};
    // Initialize x2[0..1] = 10, 11.
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::IndexableTemp, 2, 0, 1, 0}, NullOp,
                        {MakeImm(10)}});
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::IndexableTemp, 2, 1, 1, 0}, NullOp,
                        {MakeImm(11)}});
    // Copy x1 = x2.
    for (uint32_t E = 0; E < 2; ++E) {
      const uint32_t T = Ssa.AllocValue();
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(T), NullOp,
                          {Operand{OperandKind::IndexableTemp, 2, E, 1, 0}}});
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                          Operand{OperandKind::IndexableTemp, 1, E, 1, 0},
                          NullOp, {MakeSsaTemp(T)}});
    }
    // Copy x0 = x1.
    for (uint32_t E = 0; E < 2; ++E) {
      const uint32_t T = Ssa.AllocValue();
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(T), NullOp,
                          {Operand{OperandKind::IndexableTemp, 1, E, 1, 0}}});
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                          Operand{OperandKind::IndexableTemp, 0, E, 1, 0},
                          NullOp, {MakeSsaTemp(T)}});
    }
    // Mutate x2[0] = 20 after both copies.
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::IndexableTemp, 2, 0, 1, 0}, NullOp,
                        {MakeImm(20)}});
    auto &B1 = Ssa.Blocks[1];
    B1.Kind = FlowKind::Return;
    const uint32_t OutV = Ssa.AllocValue();
    B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(OutV), NullOp,
                        {Operand{OperandKind::IndexableTemp, 0, 0, 1, 0}}});
    B1.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                        {MakeSsaTemp(OutV)}});
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    RunOptIndexableCopy(Ssa, Dom);
    // B1 must NOT read x2[0] (which was mutated to 20).
    for (const auto &I : B1.Insts) {
      for (const auto &S : I.Sources)
        if (S.Kind == OperandKind::IndexableTemp && S.Index == 2) {
          std::fprintf(stderr, "Chained copy forwarded across later mutation of x2\n");
          return false;
        }
    }
  }

  // P1 #2: 35-block conditional chain inside a loop must converge liveness and
  // preserve a PHI instead of folding exit read to initial constant 1.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 2, 1});
    SsaProgram Ssa(Mir);
    constexpr uint32_t kChain = 35;
    // Block 0: entry; Blocks 1..kChain: chain inside loop; Block kChain+1: update;
    // Block kChain+2: latch -> {1, kChain+3}; Block kChain+3: exit.
    const uint32_t UpdateB = kChain + 1;
    const uint32_t LatchB = kChain + 2;
    const uint32_t ExitB = kChain + 3;
    Ssa.Blocks.resize(ExitB + 1);
    Ssa.Blocks[0].Kind = FlowKind::Jump;
    Ssa.Blocks[0].Successors = {1};
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::IndexableTemp, 0, 0, 1, 0},
                                   NullOp, {MakeImm(1)}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::IndexableTemp, 0, 1, 1, 0},
                                   NullOp, {MakeImm(0)}});
    for (uint32_t K = 1; K <= kChain; ++K) {
      Ssa.Blocks[K].Kind = FlowKind::Branch;
      Ssa.Blocks[K].Condition = In0;
      Ssa.Blocks[K].Successors = {K + 1, LatchB};
    }
    Ssa.Blocks[UpdateB].Kind = FlowKind::Jump;
    Ssa.Blocks[UpdateB].Successors = {LatchB};
    Ssa.Blocks[UpdateB].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::IndexableTemp, 0, 0, 1, 0},
         NullOp, {MakeImm(2)}});
    Ssa.Blocks[LatchB].Kind = FlowKind::Branch;
    Ssa.Blocks[LatchB].Condition = In1;
    Ssa.Blocks[LatchB].Successors = {1, ExitB};
    Ssa.Blocks[ExitB].Kind = FlowKind::Return;
    const uint32_t OutV = Ssa.AllocValue();
    Ssa.Blocks[ExitB].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, MakeSsaTemp(OutV), NullOp,
         {Operand{OperandKind::IndexableTemp, 0, 0, 1, 0}}});
    Ssa.Blocks[ExitB].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
         {MakeSsaTemp(OutV)}});
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    RunOptIndexableSsa(Ssa, Dom);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Deep-loop IndexableSsa verify failed: %s\n", Err.c_str());
      return false;
    }
    // Exit read must NOT have been folded to literal 1 (LatchB must hold a PHI).
    if (Ssa.Blocks[LatchB].Phis.empty()) {
      std::fprintf(stderr, "Deep-loop IndexableSsa dropped required PHI\n");
      return false;
    }
  }

  // P1 #4 & #5: under --nan-safe, (cond ? 1.0 : 0.0) * x must NOT drop NaN,
  // and max(min(x,1),0) must NOT fold to mov_sat when x can be NaN;
  // min(max(x,0),1) MUST still fold. Under default (NanSafe=false), both fold.
  for (bool NanSafe : {true, false}) {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.NanSafe = NanSafe;
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Sel = Ssa.AllocValue();
    const uint32_t Prod = Ssa.AllocValue();
    const uint32_t Min1 = Ssa.AllocValue();
    const uint32_t Max0 = Ssa.AllocValue();
    const uint32_t Max0Ok = Ssa.AllocValue();
    const uint32_t Min1Ok = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Sel), NullOp,
                       {In0, MakeImm(0x3f800000u), MakeImm(0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(Prod), NullOp,
                       {MakeSsaTemp(Sel), In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MIN, MakeSsaTemp(Min1), NullOp,
                       {In1, MakeImm(0x3f800000u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MAX, MakeSsaTemp(Max0), NullOp,
                       {MakeSsaTemp(Min1), MakeImm(0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MAX, MakeSsaTemp(Max0Ok), NullOp,
                       {In1, MakeImm(0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MIN, MakeSsaTemp(Min1Ok), NullOp,
                       {MakeSsaTemp(Max0Ok), MakeImm(0x3f800000u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(Prod)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(Max0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 2, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(Min1Ok)}});
    OptimizeSsaMir(Ssa);
    bool HasMul = false, HasMax = false, HasSat = false;
    for (const auto &I : B.Insts) {
      HasMul |= (I.Opcode == D3D10_SB_OPCODE_MUL);
      HasMax |= (I.Opcode == D3D10_SB_OPCODE_MAX);
      HasSat |= (I.Saturate);
    }
    const bool Ok = NanSafe ? (HasMul && HasMax && HasSat)
                            : (!HasMul && !HasMax && HasSat);
    if (!Ok) {
      std::fprintf(stderr, "Float NanSafe=%d check failed: mul=%d max=%d sat=%d\n",
                   NanSafe, HasMul, HasMax, HasSat);
      return false;
    }
  }
  return true;
}

bool TestBatch2Optimizations() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};

  // 1. SM5 bitfield selection (UBFE, BFI with 0, BFI with ~mask, (x<<1)|1, (x*6)|1, INEG->IADD -src).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;

    const uint32_t Sh0 = Ssa.AllocValue(), Ub0 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_USHR, MakeSsaTemp(Sh0), NullOp, {In0, MakeImm(7)}});
    B.Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(Ub0), NullOp, {MakeSsaTemp(Sh0), MakeImm(31)}});

    const uint32_t Sh1 = Ssa.AllocValue(), Bf0 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_ISHL, MakeSsaTemp(Sh1), NullOp, {In0, MakeImm(2)}});
    B.Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(Bf0), NullOp, {MakeSsaTemp(Sh1), MakeImm(4)}});

    const uint32_t Sh2 = Ssa.AllocValue(), Ins2 = Ssa.AllocValue(), Clr2 = Ssa.AllocValue(), Bf1 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_ISHL, MakeSsaTemp(Sh2), NullOp, {In0, MakeImm(5)}});
    B.Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(Ins2), NullOp, {MakeSsaTemp(Sh2), MakeImm(992)}});
    B.Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(Clr2), NullOp, {In1, MakeImm(~992u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_OR, MakeSsaTemp(Bf1), NullOp, {MakeSsaTemp(Clr2), MakeSsaTemp(Ins2)}});

    const uint32_t Sh3 = Ssa.AllocValue(), Bf2 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_ISHL, MakeSsaTemp(Sh3), NullOp, {In0, MakeImm(1)}});
    B.Insts.push_back({D3D10_SB_OPCODE_OR, MakeSsaTemp(Bf2), NullOp, {MakeSsaTemp(Sh3), MakeImm(1)}});

    const uint32_t M6 = Ssa.AllocValue(), Imad = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_IMUL, NullOp, MakeSsaTemp(M6), {In0, MakeImm(6)}});
    B.Insts.push_back({D3D10_SB_OPCODE_OR, MakeSsaTemp(Imad), NullOp, {MakeSsaTemp(M6), MakeImm(1)}});

    const uint32_t Neg = Ssa.AllocValue(), Sub = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_INEG, MakeSsaTemp(Neg), NullOp, {In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(Sub), NullOp, {In0, MakeSsaTemp(Neg)}});

    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(Ub0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp, {MakeSsaTemp(Bf0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 2, 0, 1, 0}, NullOp, {MakeSsaTemp(Bf1)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 3, 0, 1, 0}, NullOp, {MakeSsaTemp(Bf2)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 4, 0, 1, 0}, NullOp, {MakeSsaTemp(Imad)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 5, 0, 1, 0}, NullOp, {MakeSsaTemp(Sub)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Bitfield test SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned Ubfe = 0, BfiLanes = 0, ImadC = 0, InegC = 0;
    for (const auto &I : Ssa.Blocks[0].Insts) {
      Ubfe += I.Opcode == D3D11_SB_OPCODE_UBFE ? PopcountMask(I.Dest.Mask) : 0u;
      BfiLanes += I.Opcode == D3D11_SB_OPCODE_BFI ? PopcountMask(I.Dest.Mask) : 0u;
      ImadC += I.Opcode == D3D10_SB_OPCODE_IMAD;
      InegC += I.Opcode == D3D10_SB_OPCODE_INEG;
    }
    if (Ubfe != 1 || BfiLanes != 3 || ImadC != 1 || InegC != 0) {
      std::fprintf(stderr, "Bitfield/IMAD/INEG test failed: ubfe=%u bfiLanes=%u imad=%u ineg=%u\n",
                   Ubfe, BfiLanes, ImadC, InegC);
      return false;
    }
  }

  // 2. Exact IEEE empty-clamp collapse: min(max(ddx(x), 0.0001), 0.0) -> 0.0
  //    and DCE sweeps the now-unused derivative instruction.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Dx = Ssa.AllocValue(), Mx = Ssa.AllocValue(), Mn = Ssa.AllocValue();
    B.Insts.push_back({D3D11_SB_OPCODE_DERIV_RTX_COARSE, MakeSsaTemp(Dx), NullOp, {In0}});
    B.Insts.push_back({D3D10_SB_OPCODE_MAX, MakeSsaTemp(Mx), NullOp, {MakeSsaTemp(Dx), MakeImm(0x38d1b717u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MIN, MakeSsaTemp(Mn), NullOp, {MakeSsaTemp(Mx), MakeImm(0u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(Mn)}});
    OptimizeSsaMir(Ssa);
    if (Ssa.Blocks[0].Insts.size() != 1 ||
        Ssa.Blocks[0].Insts[0].Opcode != D3D10_SB_OPCODE_MOV ||
        Ssa.Blocks[0].Insts[0].Sources[0].Kind != OperandKind::Immediate ||
        Ssa.Blocks[0].Insts[0].Sources[0].Index != 0u) {
      std::fprintf(stderr, "Empty clamp + dead deriv DCE failed (insts=%zu)\n",
                   Ssa.Blocks[0].Insts.size());
      return false;
    }
  }

  // 3. Constant integer folding + dead branch pruning: IADD 2, 3 -> if_nz collapses to 1 block.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const uint32_t C = Ssa.AllocValue();
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeSsaTemp(C);
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(C), NullOp, {MakeImm(2), MakeImm(3)}});
    Ssa.Blocks[1].Kind = FlowKind::Return;
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeImm(0x3f800000u)}});
    Ssa.Blocks[2].Kind = FlowKind::Return;
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeImm(0u)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err) || Ssa.Blocks.size() != 1 ||
        Ssa.Blocks[0].Insts.size() != 1 ||
        Ssa.Blocks[0].Insts[0].Sources[0].Index != 0x3f800000u) {
      std::fprintf(stderr, "Constant branch folding failed: blocks=%zu insts=%zu err='%s'\n",
                   Ssa.Blocks.size(), Ssa.Blocks[0].Insts.size(), Err.c_str());
      return false;
    }
  }

  return true;
}

bool TestVsOptimizations() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};
  auto MakeCb = [](uint32_t Elem, unsigned Lane) {
    return Operand{OperandKind::ConstantBuffer, 0, Elem, 1,
                   BroadcastLaneSwizzle(Lane)};
  };
  auto MakeIn = [](uint32_t Reg, unsigned Lane) {
    return Operand{OperandKind::Input, Reg, 0, 1, BroadcastLaneSwizzle(Lane)};
  };

  // 1. Cross-block MUL dot selection + GVN collapses duplicate dp3 inside if_nz.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Vertex;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const uint32_t M0 = Ssa.AllocValue(), A1 = Ssa.AllocValue(), A2 = Ssa.AllocValue();
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = In0;
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M0), NullOp,
                                   {MakeCb(4, 0), MakeIn(1, 0)}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(A1), NullOp,
                                   {MakeCb(4, 1), MakeIn(1, 1), MakeSsaTemp(M0)}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(A2), NullOp,
                                   {MakeCb(4, 2), MakeIn(1, 2), MakeSsaTemp(A1)}});

    const uint32_t B1 = Ssa.AllocValue(), B2 = Ssa.AllocValue();
    const uint32_t BOut = Ssa.AllocValue();
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(B1), NullOp,
                                   {MakeIn(1, 1), MakeCb(4, 1), MakeSsaTemp(M0)}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(B2), NullOp,
                                   {MakeIn(1, 2), MakeCb(4, 2), MakeSsaTemp(B1)}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(BOut), NullOp,
                                   {MakeSsaTemp(B2), MakeImm(0x3f800000u)}});

    const uint32_t P = Ssa.AllocValue();
    Ssa.Blocks[2].Kind = FlowKind::Return;
    SsaPhi PPhi;
    PPhi.Result = P;
    PPhi.Mask = 1;
    PPhi.Inputs = {{0, MakeSsaTemp(A2)}, {1, MakeSsaTemp(BOut)}};
    Ssa.Blocks[2].Phis.push_back(std::move(PPhi));
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                                   {MakeSsaTemp(A2)}});
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                                   {MakeSsaTemp(P)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    unsigned Dp3 = 0, Mul = 0, Mad = 0;
    for (const auto &Blk : Ssa.Blocks)
      for (const auto &I : Blk.Insts) {
        Dp3 += I.Opcode == D3D10_SB_OPCODE_DP3;
        Mul += I.Opcode == D3D10_SB_OPCODE_MUL;
        Mad += I.Opcode == D3D10_SB_OPCODE_MAD;
      }
    if (!Ssa.Verify(Err) || Dp3 != 1 || Mul != 0 || Mad != 0) {
      std::fprintf(stderr, "Cross-block dot+GVN failed: dp3=%u mul=%u mad=%u err='%s'\n",
                   Dp3, Mul, Mad, Err.c_str());
      return false;
    }
  }

  // 2. Output retargeting: lane-remapped sqrt -> o3.w across blocks + disjoint MOV o4 merge.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Vertex;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const uint32_t Sq = Ssa.AllocValue();
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = In0;
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_SQRT, MakeSsaTemp(Sq), NullOp, {In1}});
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[2].Kind = FlowKind::Return;
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 3, 0, 0x8u, kIdentitySwizzle},
                                   NullOp, {MakeSsaTemp(Sq, 1, 0)}});
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 4, 0, 0x7u, kIdentitySwizzle},
                                   NullOp, {MakeImm(0)}});
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 5, 0, 0x1u, kIdentitySwizzle},
                                   NullOp, {In0}});
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 4, 0, 0x8u, kIdentitySwizzle},
                                   NullOp, {MakeImm(0)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    bool HasSqrtToO3W = false;
    unsigned O4Movs = 0;
    for (const auto &Blk : Ssa.Blocks)
      for (const auto &I : Blk.Insts) {
        if (I.Opcode == D3D10_SB_OPCODE_SQRT && I.Dest.Kind == OperandKind::Output &&
            I.Dest.Index == 3 && I.Dest.Mask == 0x8u)
          HasSqrtToO3W = true;
        if (I.Opcode == D3D10_SB_OPCODE_MOV && I.Dest.Kind == OperandKind::Output &&
            I.Dest.Index == 4 && I.Dest.Mask == 0xfu)
          ++O4Movs;
      }
    if (!Ssa.Verify(Err) || !HasSqrtToO3W || O4Movs != 1) {
      std::fprintf(stderr, "Output fold/merge failed: sqrt=%d o4=%u err='%s'\n",
                   HasSqrtToO3W, O4Movs, Err.c_str());
      return false;
    }
  }

  // 3. Bool-test-only PHI strips AND(bool_phi, 1) and FTOI(ITOF(RESINFO)) folds to RESINFO.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Vertex;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(4);
    const uint32_t C1 = Ssa.AllocValue(), C2 = Ssa.AllocValue();
    const uint32_t BPhi = Ssa.AllocValue(), Z1 = Ssa.AllocValue();
    const uint32_t Z2 = Ssa.AllocValue(), ZPhi = Ssa.AllocValue();
    const uint32_t Res = Ssa.AllocValue(), Flt = Ssa.AllocValue(), Back = Ssa.AllocValue();
    const uint32_t Sel = Ssa.AllocValue();
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = In0;
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_RESINFO,
                                   MakeSsaTemp(Res, 0xbu, kIdentitySwizzle), NullOp,
                                   {MakeImm(0), Operand{OperandKind::Resource, 0, 0, 0xf, kIdentitySwizzle}}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_ITOF, MakeSsaTemp(Flt), NullOp,
                                   {MakeSsaTemp(Res, 1, 0)}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_FTOI, MakeSsaTemp(Back), NullOp,
                                   {MakeSsaTemp(Flt)}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_ILT, MakeSsaTemp(C1), NullOp,
                                   {MakeImm(16), MakeSsaTemp(Back)}});
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 2, 0, 1, 0}, NullOp, {In0}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_NE, MakeSsaTemp(C2), NullOp, {In1, MakeImm(0)}});
    Ssa.Blocks.resize(5);
    Ssa.Blocks[2].Kind = FlowKind::Branch;
    Ssa.Blocks[2].Condition = In1;
    Ssa.Blocks[2].Successors = {3, 4};
    SsaPhi BPhiNode;
    BPhiNode.Result = BPhi;
    BPhiNode.Mask = 1;
    BPhiNode.Inputs = {{0, MakeSsaTemp(C1)}, {1, MakeSsaTemp(C2)}};
    Ssa.Blocks[2].Phis.push_back(std::move(BPhiNode));
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(Z1), NullOp,
                                   {MakeSsaTemp(BPhi), MakeImm(1)}});
    Ssa.Blocks[3].Kind = FlowKind::Jump;
    Ssa.Blocks[3].Successors = {4};
    Ssa.Blocks[3].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp, {In1}});
    Ssa.Blocks[3].Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(Z2), NullOp,
                                   {MakeSsaTemp(C1), MakeImm(1)}});
    Ssa.Blocks[4].Kind = FlowKind::Return;
    SsaPhi ZPhiNode;
    ZPhiNode.Result = ZPhi;
    ZPhiNode.Mask = 1;
    ZPhiNode.Inputs = {{2, MakeSsaTemp(Z1)}, {3, MakeSsaTemp(Z2)}};
    Ssa.Blocks[4].Phis.push_back(std::move(ZPhiNode));
    Ssa.Blocks[4].Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Sel), NullOp,
                                   {MakeSsaTemp(ZPhi), In0, In1}});
    Ssa.Blocks[4].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                   Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                                   {MakeSsaTemp(Sel)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    unsigned AndC = 0, ItofC = 0, FtoiC = 0;
    for (const auto &Blk : Ssa.Blocks)
      for (const auto &I : Blk.Insts) {
        AndC += I.Opcode == D3D10_SB_OPCODE_AND;
        ItofC += I.Opcode == D3D10_SB_OPCODE_ITOF;
        FtoiC += I.Opcode == D3D10_SB_OPCODE_FTOI;
      }
    if (!Ssa.Verify(Err) || AndC != 0 || ItofC != 0 || FtoiC != 0) {
      std::fprintf(stderr, "BoolPhi/Resinfo fold failed: and=%u itof=%u ftoi=%u err='%s'\n",
                   AndC, ItofC, FtoiC, Err.c_str());
      return false;
    }
  }

  // 4. Tree SLP rejects non-temp CB gather on round_ne so SameFamilyPack emits 2 round_ne, 0 gather combines.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Vertex;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t R0 = Ssa.AllocValue(), R1 = Ssa.AllocValue();
    const uint32_t R2 = Ssa.AllocValue(), R3 = Ssa.AllocValue();
    const uint32_t Comb = Ssa.AllocValue(), Dot = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_ROUND_NE, MakeSsaTemp(R0), NullOp, {MakeCb(143, 3)}});
    B.Insts.push_back({D3D10_SB_OPCODE_ROUND_NE, MakeSsaTemp(R1), NullOp, {MakeCb(144, 0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_ROUND_NE, MakeSsaTemp(R2), NullOp, {MakeCb(144, 1)}});
    B.Insts.push_back({D3D10_SB_OPCODE_ROUND_NE, MakeSsaTemp(R3), NullOp, {MakeCb(144, 2)}});
    B.Insts.push_back({PseudoCombineVec, MakeSsaTemp(Comb, 0xfu, kIdentitySwizzle), NullOp,
                       {MakeSsaTemp(R0), MakeSsaTemp(R1), MakeSsaTemp(R2), MakeSsaTemp(R3)}});
    const std::array<uint32_t, 4> Wts{0x3f800000u, 0x40000000u, 0x40800000u, 0x41000000u};
    B.Insts.push_back({D3D10_SB_OPCODE_DP4, MakeSsaTemp(Dot), NullOp,
                       {MakeSsaTemp(Comb, 0xfu, kIdentitySwizzle),
                        Operand{OperandKind::ImmediateVector, Ssa.InternLiteralVector(Wts), 0, 0xf,
                                kIdentitySwizzle}}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(Dot)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    unsigned RoundC = 0, CbCombines = 0;
    for (const auto &I : B.Insts) {
      RoundC += I.Opcode == D3D10_SB_OPCODE_ROUND_NE;
      if (I.Opcode == PseudoCombineVec && !I.Sources.empty() &&
          I.Sources[0].Kind == OperandKind::ConstantBuffer)
        ++CbCombines;
    }
    if (!Ssa.Verify(Err) || RoundC != 2 || CbCombines != 0) {
      std::fprintf(stderr, "Tree SLP CB-gather rejection failed: round=%u cbcomb=%u err='%s'\n",
                   RoundC, CbCombines, Err.c_str());
      return false;
    }
  }

  // 5. Pure-ALU 6-inst single-PHI triangle if-converts cleanly.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Vertex;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const uint32_t T0 = Ssa.AllocValue(), T1 = Ssa.AllocValue(), T2 = Ssa.AllocValue();
    const uint32_t T3 = Ssa.AllocValue(), T4 = Ssa.AllocValue(), T5 = Ssa.AllocValue();
    const uint32_t PhiV = Ssa.AllocValue();
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = In0;
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(T0), NullOp, {In1, MakeCb(1, 0)}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(T1), NullOp, {In1, MakeSsaTemp(T0)}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(T2), NullOp, {MakeImm(0), In1}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(T3), NullOp, {MakeSsaTemp(T1), MakeSsaTemp(T2)}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(T4), NullOp, {In1, MakeCb(1, 1), MakeCb(1, 2)}});
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(T5), NullOp, {MakeSsaTemp(T3), MakeSsaTemp(T4), In1}});
    Ssa.Blocks[2].Kind = FlowKind::Return;
    SsaPhi P;
    P.Result = PhiV;
    P.Mask = 1;
    P.Inputs = {{0, In1}, {1, MakeSsaTemp(T5)}};
    Ssa.Blocks[2].Phis.push_back(std::move(P));
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(PhiV)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err) || Ssa.Blocks.size() != 1) {
      std::fprintf(stderr, "6-inst single-PHI triangle did not if-convert: blocks=%zu err='%s'\n",
                   Ssa.Blocks.size(), Err.c_str());
      return false;
    }
  }

  return true;
}

bool TestRound3RegressionsAndPerf() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 0xf, kIdentitySwizzle};
  const Operand In1{OperandKind::Input, 1, 0, 0xf, kIdentitySwizzle};
  auto Lane = [](Operand Op, unsigned L) {
    Op.Mask = 1;
    Op.Swizzle = BroadcastLaneSwizzle(L);
    return Op;
  };

  // 1. P1: Tree SLP combining IADD with integer NEG and positive immediates
  // must pre-negate immediates with two's complement (0u - c), NOT float XOR.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t T = Ssa.AllocValue();
    const uint32_t C0 = Ssa.AllocValue(), C1 = Ssa.AllocValue();
    const uint32_t C2 = Ssa.AllocValue(), C3 = Ssa.AllocValue();
    const uint32_t Comb = Ssa.AllocValue(), Loaded = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_XOR, MakeSsaTemp(T), NullOp,
                       {Lane(In1, 1), MakeImm(7)}});
    Operand NegT = MakeSsaTemp(T);
    NegT.Modifier = D3D10_SB_OPERAND_MODIFIER_NEG;
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(C0), NullOp,
                       {Lane(In0, 1), NegT}});
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(C1), NullOp,
                       {Lane(In0, 0), MakeImm(3)}});
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(C2), NullOp,
                       {Lane(In0, 2), MakeImm(1)}});
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(C3), NullOp,
                       {Lane(In0, 3), MakeImm(2)}});
    B.Insts.push_back({PseudoCombineVec, MakeSsaTemp(Comb, 0xf, kIdentitySwizzle),
                       NullOp,
                       {MakeSsaTemp(C0), MakeSsaTemp(C1), MakeSsaTemp(C2),
                        MakeSsaTemp(C3)}});
    B.Insts.push_back({D3D10_SB_OPCODE_LD, MakeSsaTemp(Loaded, 0xf, kIdentitySwizzle),
                       NullOp,
                       {MakeSsaTemp(Comb, 0xf, kIdentitySwizzle),
                        Operand{OperandKind::Resource, 0, 0, 0xf, kIdentitySwizzle}}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 0xf, kIdentitySwizzle},
                       NullOp, {MakeSsaTemp(Loaded, 0xf, kIdentitySwizzle)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "IntNeg SLP SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    bool SawNegPackedIadd = false;
    for (const auto &I : B.Insts) {
      if (I.Opcode == PseudoCombineVec && I.Sources.size() == 4) {
        if (I.Sources[1].Kind == OperandKind::Immediate &&
            I.Sources[1].Index != static_cast<uint32_t>(-3)) {
          std::fprintf(stderr, "IntNeg SLP emitted wrong immediate 0x%08x (expected 0xfffffffd)\n",
                       I.Sources[1].Index);
          return false;
        }
      }
      if (I.Opcode == D3D10_SB_OPCODE_IADD && I.Dest.Mask == 0xfu &&
          I.Sources[1].Modifier == D3D10_SB_OPERAND_MODIFIER_NEG)
        SawNegPackedIadd = true;
    }
    if (!SawNegPackedIadd) {
      std::fprintf(stderr, "Expected 4-wide packed IADD with NEG combine\n");
      return false;
    }
  }

  // 2. Sibling P1: iadd(-iadd(x, 3), 5) must NOT fold into iadd(x, 8).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Inner = Ssa.AllocValue(), Outer = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(Inner), NullOp,
                       {Lane(In0, 0), MakeImm(3)}});
    Operand NegInner = MakeSsaTemp(Inner);
    NegInner.Modifier = D3D10_SB_OPERAND_MODIFIER_NEG;
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(Outer), NullOp,
                       {NegInner, MakeImm(5)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(Outer)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    unsigned IaddCount = 0;
    for (const auto &I : B.Insts)
      IaddCount += (I.Opcode == D3D10_SB_OPCODE_IADD);
    if (!Ssa.Verify(Err) || IaddCount != 2) {
      std::fprintf(stderr, "Negated inner iadd mis-folded: iadds=%u err='%s'\n",
                   IaddCount, Err.c_str());
      return false;
    }
  }

  // 3. P2: Single RunOptGvnDce collapses a 40-layer duplicate shift chain from
  // 80 ISHLs down to 40 ISHLs in ONE pass without relying on outer iterations.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    Operand PrevA = Lane(In0, 0);
    Operand PrevB = Lane(In0, 0);
    for (unsigned Layer = 0; Layer < 40; ++Layer) {
      const uint32_t A = Ssa.AllocValue();
      const uint32_t BV = Ssa.AllocValue();
      B.Insts.push_back({D3D10_SB_OPCODE_ISHL, MakeSsaTemp(A), NullOp,
                         {PrevA, MakeImm(1)}});
      B.Insts.push_back({D3D10_SB_OPCODE_ISHL, MakeSsaTemp(BV), NullOp,
                         {PrevB, MakeImm(1)}});
      PrevA = MakeSsaTemp(A);
      PrevB = MakeSsaTemp(BV);
    }
    const uint32_t Sum = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_IADD, MakeSsaTemp(Sum), NullOp,
                       {PrevA, PrevB}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(Sum)}});
    RunOptGvnDce(Ssa);
    std::string Err;
    unsigned IshlCount = 0;
    for (const auto &I : B.Insts)
      IshlCount += (I.Opcode == D3D10_SB_OPCODE_ISHL);
    if (!Ssa.Verify(Err) || IshlCount != 40) {
      std::fprintf(stderr, "40-layer GVN chain expected 40 ISHLs in 1 pass, got %u (err='%s')\n",
                   IshlCount, Err.c_str());
      return false;
    }
  }

  return true;
}

bool TestWriteOnceDedupLanePackAndUnitSat() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In0Y{OperandKind::Input, 0, 0, 1, 0x55};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};

  // 1. Two identical write-once [6 x float] arrays (x1 dominates x0) deduplicate to 1.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 6, 1});
    Mir.IndexableTemps.push_back({1, 6, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(2);
    auto &B0 = Ssa.Blocks[0];
    B0.Kind = FlowKind::Jump;
    B0.Successors = {1};
    for (uint32_t E = 0; E < 6; ++E) {
      Operand Val = E < 3 ? Operand{OperandKind::ConstantBuffer, 0, E, 1, 0}
                          : MakeImm(0);
      B0.Insts.push_back({D3D10_SB_OPCODE_MOV,
                          Operand{OperandKind::IndexableTemp, 1, E, 1, 0},
                          NullOp, {Val}});
    }
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t V1 = Ssa.AllocValue();
    B0.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In0}});
    Operand Dyn1{OperandKind::IndexableTemp, 1, 0, 1, 0};
    Dyn1.RelativeRegister = Idx;
    B0.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V1), NullOp, {Dyn1}});

    auto &B1 = Ssa.Blocks[1];
    B1.Kind = FlowKind::Return;
    for (uint32_t E = 0; E < 6; ++E) {
      Operand Val = E < 3 ? Operand{OperandKind::ConstantBuffer, 0, E, 1, 0}
                          : MakeImm(0);
      B1.Insts.push_back({D3D10_SB_OPCODE_MOV,
                          Operand{OperandKind::IndexableTemp, 0, E, 1, 0},
                          NullOp, {Val}});
    }
    const uint32_t V0 = Ssa.AllocValue();
    Operand Dyn0{OperandKind::IndexableTemp, 0, 0, 1, 0};
    Dyn0.RelativeRegister = Idx;
    B1.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(V0), NullOp, {Dyn0}});
    B1.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                        {MakeSsaTemp(V1)}});
    B1.Insts.push_back({D3D10_SB_OPCODE_MOV,
                        Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                        {MakeSsaTemp(V0)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err) || Mir.IndexableTemps.size() != 1) {
      std::fprintf(stderr, "WriteOnceDedup failed: err='%s' arrays=%zu\n",
                   Err.c_str(), Mir.IndexableTemps.size());
      return false;
    }
  }

  // 2. Lane-aware store vectorization merges .y stores across a .x-only dynamic
  // read when the .y source is already available before the .x read.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 4, 1});
    Mir.IndexableTemps.push_back({1, 4, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    for (uint32_t E = 0; E < 4; ++E)
      B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                         Operand{OperandKind::IndexableTemp, 0, E, 1, 0},
                         NullOp, {In0}});
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t R0 = Ssa.AllocValue();
    const uint32_t R1 = Ssa.AllocValue();
    const uint32_t R2 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In1}});
    Operand Dyn0{OperandKind::IndexableTemp, 0, 0, 1, 0};
    Dyn0.RelativeRegister = Idx;
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(R0), NullOp, {Dyn0}});
    for (uint32_t E = 0; E < 4; ++E)
      B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                         Operand{OperandKind::IndexableTemp, 1, E, 1, 0},
                         NullOp, {In0Y}});
    Operand Dyn1{OperandKind::IndexableTemp, 1, 0, 1, 0};
    Dyn1.RelativeRegister = Idx;
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(R1), NullOp, {Dyn0}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(R2), NullOp, {Dyn1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(R0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(R1)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 2, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(R2)}});
    std::string Err;
    if (!RunOptIndexablePack(Ssa) || !Ssa.Verify(Err)) {
      std::fprintf(stderr, "LanePack verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned VecStores = 0;
    for (const auto &I : B.Insts)
      VecStores += (I.Dest.Kind == OperandKind::IndexableTemp && I.Dest.Mask == 0x3u);
    if (VecStores != 4) {
      std::fprintf(stderr, "LanePack expected 4 merged .xy stores, got %u\n", VecStores);
      return false;
    }
  }

  // 3. Redundant mov_sat drops on min(sat(a), sat(b)), stays on add(sat(a), sat(b)).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t S0 = Ssa.AllocValue(), S1 = Ssa.AllocValue();
    const uint32_t Mn = Ssa.AllocValue(), SatMn = Ssa.AllocValue();
    const uint32_t Sm = Ssa.AllocValue(), SatSm = Ssa.AllocValue();
    SsaInst M0{D3D10_SB_OPCODE_MOV, MakeSsaTemp(S0), NullOp, {In0}};
    M0.Saturate = true;
    SsaInst M1{D3D10_SB_OPCODE_MOV, MakeSsaTemp(S1), NullOp, {In1}};
    M1.Saturate = true;
    B.Insts.push_back(M0);
    B.Insts.push_back(M1);
    B.Insts.push_back({D3D10_SB_OPCODE_MIN, MakeSsaTemp(Mn), NullOp,
                       {MakeSsaTemp(S0), MakeSsaTemp(S1)}});
    SsaInst ClampMn{D3D10_SB_OPCODE_MOV, MakeSsaTemp(SatMn), NullOp,
                    {MakeSsaTemp(Mn)}};
    ClampMn.Saturate = true;
    B.Insts.push_back(ClampMn);
    B.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Sm), NullOp,
                       {MakeSsaTemp(S0), MakeSsaTemp(S1)}});
    SsaInst ClampSm{D3D10_SB_OPCODE_MOV, MakeSsaTemp(SatSm), NullOp,
                    {MakeSsaTemp(Sm)}};
    ClampSm.Saturate = true;
    B.Insts.push_back(ClampSm);
    // Keep Mn multi-used so sat cannot simply fold onto MIN; must be proved redundant.
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(Mn)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(SatMn)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 2, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(Sm)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 3, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(SatSm)}});
    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "UnitSat verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned MovSatCount = 0;
    for (const auto &I : B.Insts)
      MovSatCount += (I.Opcode == D3D10_SB_OPCODE_MOV && I.Saturate);
    // Expect 3 mov_sat: S0, S1, and SatSm (NOT SatMn!).
    if (MovSatCount != 3) {
      std::fprintf(stderr, "UnitSat expected 3 mov_sat (dropped SatMn, kept SatSm), got %u\n",
                   MovSatCount);
      return false;
    }
  }
  return true;
}

bool TestPhase2ColdSinkStrictBranchAndPow() {
  const Operand NullOp{OperandKind::Null};
  // 1. pow(|x|, 0.125) -> 3x SQRT; fast_math pow(|x|, 1/3) -> UMUL+RCP;
  //    normal pow(|x|, 1/3) keeps LOG+EXP.
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const Operand In0{OperandKind::Input, 0, 0, 1, 0};
    const Operand In1{OperandKind::Input, 1, 0, 1, 0};
    const Operand In2{OperandKind::Input, 2, 0, 1, 0};

    // pow(|In0|, 0.125)
    const uint32_t L0 = Ssa.AllocValue(), M0 = Ssa.AllocValue(), E0 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_LOG, MakeSsaTemp(L0), NullOp, {AbsMirOperand(In0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M0), NullOp,
                       {MakeSsaTemp(L0), MakeImm(0x3e000000u)}}); // 0.125f
    B.Insts.push_back({D3D10_SB_OPCODE_EXP, MakeSsaTemp(E0), NullOp, {MakeSsaTemp(M0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(E0)}});

    // fast_math pow(|In1|, 1/3)
    const uint32_t L1 = Ssa.AllocValue(), M1 = Ssa.AllocValue(), E1 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_LOG, MakeSsaTemp(L1), NullOp, {AbsMirOperand(In1)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M1), NullOp,
                       {MakeSsaTemp(L1), MakeImm(0x3eaaaaabu)}}); // 1/3f
    SsaInst FastExp{D3D10_SB_OPCODE_EXP, MakeSsaTemp(E1), NullOp, {MakeSsaTemp(M1)}};
    FastExp.FastMath = true;
    B.Insts.push_back(std::move(FastExp));
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(E1)}});

    // non-fast_math pow(|In2|, 1/3)
    const uint32_t L2 = Ssa.AllocValue(), M2 = Ssa.AllocValue(), E2 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_LOG, MakeSsaTemp(L2), NullOp, {AbsMirOperand(In2)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M2), NullOp,
                       {MakeSsaTemp(L2), MakeImm(0x3eaaaaabu)}}); // 1/3f
    B.Insts.push_back({D3D10_SB_OPCODE_EXP, MakeSsaTemp(E2), NullOp, {MakeSsaTemp(M2)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 2, 0, 1, 0}, NullOp,
                       {MakeSsaTemp(E2)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Phase2 pow verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned SqrtC = 0, UmulC = 0, RcpC = 0, LogC = 0, ExpC = 0;
    for (const auto &I : B.Insts) {
      SqrtC += I.Opcode == D3D10_SB_OPCODE_SQRT;
      UmulC += I.Opcode == D3D10_SB_OPCODE_UMUL;
      RcpC += I.Opcode == D3D11_SB_OPCODE_RCP;
      LogC += I.Opcode == D3D10_SB_OPCODE_LOG;
      ExpC += I.Opcode == D3D10_SB_OPCODE_EXP;
    }
    if (SqrtC != 3 || UmulC != 1 || RcpC != 1 || LogC != 1 || ExpC != 1) {
      std::fprintf(stderr,
                   "Phase2 pow mismatch: sqrt=%u umul=%u rcp=%u log=%u exp=%u\n",
                   SqrtC, UmulC, RcpC, LogC, ExpC);
      return false;
    }
  }

  // 2. StrictBranch blocks OptIfConvert AND OptColdSink sinks cold RSQ into arm!
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const Operand In0{OperandKind::Input, 0, 0, 1, 0};
    const Operand In1{OperandKind::Input, 1, 0, 1, 0};
    const uint32_t ColdRsq = Ssa.AllocValue();
    const uint32_t ColdAdd = Ssa.AllocValue();
    const uint32_t PhiRes = Ssa.AllocValue();

    // Block 0: defines ColdRsq, strict_branch on In0 -> [1, 2]
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].StrictBranch = true;
    Ssa.Blocks[0].Condition = In0;
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_RSQ, MakeSsaTemp(ColdRsq), NullOp, {In1}});

    // Block 1 (cold arm): uses ColdRsq
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(ColdAdd), NullOp,
         {MakeSsaTemp(ColdRsq), MakeImm(0x3f800000u)}});

    // Block 2 (join): phi + output
    Ssa.Blocks[2].Kind = FlowKind::Return;
    SsaPhi Phi;
    Phi.Result = PhiRes;
    Phi.Mask = 1;
    Phi.Inputs.push_back({1, MakeSsaTemp(ColdAdd)});
    Phi.Inputs.push_back({0, In1});
    Ssa.Blocks[2].Phis.push_back(std::move(Phi));
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
         {MakeSsaTemp(PhiRes)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Phase2 cold-sink verify failed: %s\n", Err.c_str());
      return false;
    }
    if (Ssa.Blocks.size() != 3 || Ssa.Blocks[0].Kind != FlowKind::Branch) {
      std::fprintf(stderr, "StrictBranch was erroneously flattened (blocks=%zu)\n",
                   Ssa.Blocks.size());
      return false;
    }
    bool RsqInEntry = false, RsqInArm = false;
    for (const auto &I : Ssa.Blocks[0].Insts)
      RsqInEntry |= (I.Opcode == D3D10_SB_OPCODE_RSQ);
    for (const auto &I : Ssa.Blocks[1].Insts)
      RsqInArm |= (I.Opcode == D3D10_SB_OPCODE_RSQ);
    if (RsqInEntry || !RsqInArm) {
      std::fprintf(stderr, "OptColdSink failed: inEntry=%d inArm=%d\n",
                   RsqInEntry, RsqInArm);
      return false;
    }
  }

  // 3. Loop safety: never sink preheader value into loop; hoist loop-invariant from header!
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const Operand In0{OperandKind::Input, 0, 0, 1, 0};
    const Operand In1{OperandKind::Input, 1, 0, 1, 0};
    const uint32_t PreVal = Ssa.AllocValue();
    const uint32_t InvInLoop = Ssa.AllocValue();
    const uint32_t AccPhi = Ssa.AllocValue();
    const uint32_t AccNext = Ssa.AllocValue();
    const uint32_t AccStep = Ssa.AllocValue();
    const uint32_t Cmp = Ssa.AllocValue();

    // Block 0 (preheader)
    Ssa.Blocks[0].Kind = FlowKind::Jump;
    Ssa.Blocks[0].Successors = {1};
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_RSQ, MakeSsaTemp(PreVal), NullOp, {In0}});

    // Block 1 (loop header + latch)
    Ssa.Blocks[1].Kind = FlowKind::Branch;
    Ssa.Blocks[1].Condition = MakeSsaTemp(Cmp);
    Ssa.Blocks[1].Successors = {1, 2};
    SsaPhi Phi;
    Phi.Result = AccPhi;
    Phi.Mask = 1;
    Phi.Inputs.push_back({0, MakeImm(0)});
    Phi.Inputs.push_back({1, MakeSsaTemp(AccNext)});
    Ssa.Blocks[1].Phis.push_back(std::move(Phi));
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_SQRT, MakeSsaTemp(InvInLoop), NullOp, {In1}});
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(AccStep), NullOp,
         {MakeSsaTemp(AccPhi), MakeSsaTemp(PreVal)}});
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(AccNext), NullOp,
         {MakeSsaTemp(AccStep), MakeSsaTemp(InvInLoop)}});
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_LT, MakeSsaTemp(Cmp), NullOp,
         {MakeSsaTemp(AccNext), MakeImm(0x41200000u)}});

    // Block 2 (exit)
    Ssa.Blocks[2].Kind = FlowKind::Return;
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp,
         {MakeSsaTemp(AccNext)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Phase2 loop verify failed: %s\n", Err.c_str());
      return false;
    }
    bool RsqInPre = false, SqrtInPre = false, SqrtInLoop = false;
    for (const auto &I : Ssa.Blocks[0].Insts) {
      RsqInPre |= (I.Opcode == D3D10_SB_OPCODE_RSQ);
      SqrtInPre |= (I.Opcode == D3D10_SB_OPCODE_SQRT);
    }
    for (const auto &I : Ssa.Blocks[1].Insts)
      SqrtInLoop |= (I.Opcode == D3D10_SB_OPCODE_SQRT);
    if (!RsqInPre || !SqrtInPre || SqrtInLoop) {
      std::fprintf(stderr, "Phase2 LICM/loop-depth failed: rsqPre=%d sqrtPre=%d sqrtLoop=%d\n",
                   RsqInPre, SqrtInPre, SqrtInLoop);
      return false;
    }
  }
  return true;
}

bool TestAuditBugRegressions() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};

  // 1. UTOF_sat(zext_bool) -> AND must clear Saturate (no illegal and_sat!).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Cmp = Ssa.AllocValue(), Zext = Ssa.AllocValue(), Flt = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(Cmp), NullOp, {In0, In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_AND, MakeSsaTemp(Zext), NullOp, {MakeSsaTemp(Cmp), MakeImm(1u)}});
    SsaInst Utof{D3D10_SB_OPCODE_UTOF, MakeSsaTemp(Flt), NullOp, {MakeSsaTemp(Zext)}};
    Utof.Saturate = true;
    B.Insts.push_back(Utof);
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(Flt)}});
    OptimizeSsaMir(Ssa);
    for (const auto &I : Ssa.Blocks[0].Insts) {
      if (I.Opcode == D3D10_SB_OPCODE_AND && I.Saturate) {
        std::fprintf(stderr, "Audit regression failed: emitted illegal and_sat\n");
        return false;
      }
    }
  }

  // 2. NanSafe must preserve mul(x, 0), mad(0, x, y), and NOT(LT(a, b)).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.NanSafe = true;
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t M0 = Ssa.AllocValue(), Mad0 = Ssa.AllocValue();
    const uint32_t Lt = Ssa.AllocValue(), NotLt = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M0), NullOp, {In0, MakeImm(0u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(Mad0), NullOp, {MakeImm(0u), In0, In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(Lt), NullOp, {In0, In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_NOT, MakeSsaTemp(NotLt), NullOp, {MakeSsaTemp(Lt)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(M0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp, {MakeSsaTemp(Mad0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 2, 0, 1, 0}, NullOp, {MakeSsaTemp(NotLt)}});
    OptimizeSsaMir(Ssa);
    bool HasMul = false, HasMad = false, HasGe = false;
    for (const auto &I : Ssa.Blocks[0].Insts) {
      HasMul |= (I.Opcode == D3D10_SB_OPCODE_MUL);
      HasMad |= (I.Opcode == D3D10_SB_OPCODE_MAD);
      HasGe |= (I.Opcode == D3D10_SB_OPCODE_GE);
    }
    if (!HasMul || !HasMad || HasGe) {
      std::fprintf(stderr, "Audit NanSafe failed: mul=%d mad=%d ge=%d\n", HasMul, HasMad, HasGe);
      return false;
    }
  }

  // 3. OptColdSink must NOT sink DERIV_RTX past DISCARD in entry block!
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const uint32_t Ddx = Ssa.AllocValue();
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = In0;
    Ssa.Blocks[0].UniformBranch = true;
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back({D3D11_SB_OPCODE_DERIV_RTX_COARSE, MakeSsaTemp(Ddx), NullOp, {In1}});
    Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_DISCARD, NullOp, NullOp, {In0}});
    Ssa.Blocks[1].Kind = FlowKind::Return;
    Ssa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(Ddx)}});
    Ssa.Blocks[2].Kind = FlowKind::Return;
    Ssa.Blocks[2].Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeImm(0u)}});
    OptimizeSsaMir(Ssa);
    bool DdxInBlock0 = false;
    for (const auto &I : Ssa.Blocks[0].Insts)
      DdxInBlock0 |= (I.Opcode == D3D11_SB_OPCODE_DERIV_RTX_COARSE);
    if (!DdxInBlock0) {
      std::fprintf(stderr, "Audit ColdSink failed: sank DERIV_RTX past DISCARD\n");
      return false;
    }
  }

  // 4. FlushStoreGroup must read lane G.DstLane[M] (not hardcoded 0) for
  //    pre-existing multi-component IndexableTemp stores!
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 4, 1});
    Mir.IndexableTemps.push_back({1, 4, 1});
    Mir.IndexableTemps.push_back({2, 2, 4});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    for (uint32_t E = 0; E < 4; ++E) {
      B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::IndexableTemp, 0, E, 1, 0}, NullOp, {MakeImm(E)}});
      B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::IndexableTemp, 1, E, 1, 0}, NullOp, {MakeImm(E + 10)}});
    }
    const Operand V0All{OperandKind::Input, 0, 0, 0xfu, kIdentitySwizzle};
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::IndexableTemp, 2, 0, 1, 0}, NullOp, {V0All}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::IndexableTemp, 2, 0, 2, 0}, NullOp, {V0All}});
    const uint32_t Idx = Ssa.AllocValue(), R0 = Ssa.AllocValue(), R1 = Ssa.AllocValue(), R2 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp, {In0}});
    Operand D0{OperandKind::IndexableTemp, 0, 0, 1, 0}; D0.RelativeRegister = Idx;
    Operand D1{OperandKind::IndexableTemp, 1, 0, 1, 0}; D1.RelativeRegister = Idx;
    Operand D2{OperandKind::IndexableTemp, 2, 0, 3, kIdentitySwizzle}; D2.RelativeRegister = Idx;
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(R0), NullOp, {D0}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(R1), NullOp, {D1}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(R2, 3), NullOp, {D2}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(R0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0}, NullOp, {MakeSsaTemp(R1)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 2, 0, 3, 0}, NullOp, {MakeSsaTemp(R2, 3)}});
    std::string Err;
    if (!RunOptIndexablePack(Ssa) || !Ssa.Verify(Err)) {
      std::fprintf(stderr, "Audit multi-comp pack failed: %s\n", Err.c_str());
      return false;
    }
    bool CheckedVecStore = false;
    for (const auto &I : B.Insts) {
      if (I.Dest.Kind == OperandKind::IndexableTemp && I.Dest.Mask == 3 &&
          I.Sources[0].Kind == OperandKind::Input && I.Sources[0].Index == 0) {
        if (GetSwizzleLane(I.Sources[0].Swizzle, 0) != 0 ||
            GetSwizzleLane(I.Sources[0].Swizzle, 1) != 1) {
          std::fprintf(stderr, "Audit multi-comp pack wrong swizzle: 0x%x\n",
                       I.Sources[0].Swizzle);
          return false;
        }
        CheckedVecStore = true;
      }
    }
    if (!CheckedVecStore) {
      std::fprintf(stderr, "Audit multi-comp pack: expected merged store\n");
      return false;
    }
  }

  // 5. TryFoldMovcToMinMax must NOT fold MOVC_sat(ILT) into illegal IMIN_sat.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Cmp = Ssa.AllocValue(), Sel = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_ILT, MakeSsaTemp(Cmp), NullOp, {In0, In1}});
    SsaInst Movc{D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Sel), NullOp, {MakeSsaTemp(Cmp), In0, In1}};
    Movc.Saturate = true;
    B.Insts.push_back(Movc);
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(Sel)}});
    OptimizeSsaMir(Ssa);
    for (const auto &I : B.Insts) {
      if ((I.Opcode == D3D10_SB_OPCODE_IMIN || I.Opcode == D3D10_SB_OPCODE_IMAX) &&
          I.Saturate) {
        std::fprintf(stderr, "Audit MOVC_sat folded into illegal IMIN_sat/IMAX_sat\n");
        return false;
      }
    }
  }

  // 6. TryCollapseSignCbrtMovc must respect NanSafe = true (NaN -> 0 vs NaN).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.NanSafe = true;
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Lg = Ssa.AllocValue(), Mul = Ssa.AllocValue(), Pow = Ssa.AllocValue();
    const uint32_t Pos = Ssa.AllocValue(), Neg = Ssa.AllocValue();
    const uint32_t Sub = Ssa.AllocValue(), Out = Ssa.AllocValue();
    Operand AbsIn0 = In0; AbsIn0.Modifier = D3D10_SB_OPERAND_MODIFIER_ABS;
    Operand NegPow = MakeSsaTemp(Pow); NegPow.Modifier = D3D10_SB_OPERAND_MODIFIER_NEG;
    B.Insts.push_back({D3D10_SB_OPCODE_LOG, MakeSsaTemp(Lg), NullOp, {AbsIn0}});
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(Mul), NullOp, {MakeSsaTemp(Lg), MakeImm(0x3f000000u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_EXP, MakeSsaTemp(Pow), NullOp, {MakeSsaTemp(Mul)}});
    B.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(Pos), NullOp, {MakeImm(0u), In0}});
    B.Insts.push_back({D3D10_SB_OPCODE_LT, MakeSsaTemp(Neg), NullOp, {In0, MakeImm(0u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Sub), NullOp, {MakeSsaTemp(Neg), NegPow, MakeImm(0u)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Out), NullOp, {MakeSsaTemp(Pos), MakeSsaTemp(Pow), MakeSsaTemp(Sub)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0}, NullOp, {MakeSsaTemp(Out)}});
    OptimizeSsaMir(Ssa);
    unsigned MovcCount = 0;
    for (const auto &I : B.Insts)
      MovcCount += (I.Opcode == D3D10_SB_OPCODE_MOVC);
    if (MovcCount < 2) {
      std::fprintf(stderr, "Audit NanSafe SignCbrtMovc collapsed under NanSafe!\n");
      return false;
    }
  }

  // 7. OptPressureShrink sinks pure post-dominated instructions past high-pressure
  //    middle blocks to the merge LCA (reducing peak live scalars with 0 extra insts).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(4);
    auto &B0 = Ssa.Blocks[0];
    auto &B1 = Ssa.Blocks[1];
    auto &B2 = Ssa.Blocks[2];
    auto &B3 = Ssa.Blocks[3];
    B0.Kind = FlowKind::Branch;
    B0.Condition = In0;
    B0.Successors = {1, 2};
    B1.Kind = FlowKind::Jump;
    B1.Successors = {3};
    B2.Kind = FlowKind::Jump;
    B2.Successors = {3};
    B3.Kind = FlowKind::Return;

    // Create 30 live vec4 values (120 scalars > kSinkActivationScalars=96)
    // defined in B0 and used only in B3 (post-dominating merge block).
    std::vector<uint32_t> Vals;
    for (unsigned K = 0; K < 30; ++K) {
      const uint32_t V = Ssa.AllocValue();
      Vals.push_back(V);
      B0.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(V, 0xfu), NullOp,
                          {Operand{OperandKind::ConstantBuffer, 0, K, 0xfu, kIdentitySwizzle},
                           MakeImm(0x3f800000u)}});
    }
    const uint32_t BrV = Ssa.AllocValue();
    B1.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(BrV, 1), NullOp, {In0, In1}});
    const uint32_t BrV2 = Ssa.AllocValue();
    B2.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(BrV2, 1), NullOp, {In0, In1}});
    SsaPhi Phi;
    Phi.Result = Ssa.AllocValue();
    Phi.Mask = 1;
    Phi.Inputs = {{1, MakeSsaTemp(BrV, 1)}, {2, MakeSsaTemp(BrV2, 1)}};
    B3.Phis.push_back(Phi);
    uint32_t Acc = Phi.Result;
    for (uint32_t V : Vals) {
      const uint32_t Next = Ssa.AllocValue();
      B3.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Next, 1), NullOp,
                          {MakeSsaTemp(Acc, 1), MakeSsaTemp(V, 1, 0)}});
      Acc = Next;
    }
    B3.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                        NullOp, {MakeSsaTemp(Acc, 1)}});
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    std::string Err;
    if (!RunOptPressureShrink(Ssa, Dom) || !Ssa.Verify(Err)) {
      std::fprintf(stderr, "OptPressureShrink failed: %s\n", Err.c_str());
      return false;
    }
    if (!B0.Insts.empty()) {
      std::fprintf(stderr, "OptPressureShrink expected B0 insts sunk to B3, left=%zu\n",
                   B0.Insts.size());
      return false;
    }
  }

  // 8. OptPressureShrink duplicate-source cascade sinking + two-phase remat
  //    (where a second rematted value is defined inside the first's target block).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(5);
    auto &B0 = Ssa.Blocks[0], &B1 = Ssa.Blocks[1], &B2 = Ssa.Blocks[2],
         &B3 = Ssa.Blocks[3], &B4 = Ssa.Blocks[4];
    B0.Kind = FlowKind::Branch; B0.Condition = In0; B0.Successors = {1, 2};
    B1.Kind = FlowKind::Jump;   B1.Successors = {2};
    B2.Kind = FlowKind::Branch; B2.Condition = In1; B2.Successors = {3, 4};
    B3.Kind = FlowKind::Jump;   B3.Successors = {4};
    B4.Kind = FlowKind::Return;

    // 50 live vec4s (200 scalars > kRematActivationScalars=180) live across B1..B4
    // (DERIV_RTX anchors cannot sink out of B0 because they require quad uniformity).
    std::vector<uint32_t> Anchors;
    for (unsigned K = 0; K < 50; ++K) {
      const uint32_t V = Ssa.AllocValue();
      Anchors.push_back(V);
      B0.Insts.push_back({D3D10_SB_OPCODE_DERIV_RTX, MakeSsaTemp(V, 0xfu), NullOp,
                          {Operand{OperandKind::ConstantBuffer, 0, K, 0xfu, kIdentitySwizzle}}});
    }
    // Duplicate-source chain in B0: S -> Mul(S, S) used only in B4 (both must cascade-sink!).
    const uint32_t DupSrc = Ssa.AllocValue(), DupSq = Ssa.AllocValue();
    B0.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(DupSrc, 1), NullOp,
                        {Operand{OperandKind::ConstantBuffer, 0, 60, 1, 0}, MakeImm(0x3f800000u)}});
    B0.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(DupSq, 1), NullOp,
                        {MakeSsaTemp(DupSrc, 1), MakeSsaTemp(DupSrc, 1)}});

    // Remat1 defined in B0, used in B2 and B4 (cloned into B2, shifting B2 indices!).
    // Remat2 defined in B2 (after Remat1's use in B2!), used in B3 and B4.
    const uint32_t Remat1 = Ssa.AllocValue();
    B0.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Remat1, 1), NullOp,
                        {MakeSsaTemp(Anchors[0], 1, 0), MakeImm(0x40000000u)}});
    const uint32_t UseR1InB2 = Ssa.AllocValue(), Remat2 = Ssa.AllocValue();
    B2.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(UseR1InB2, 1), NullOp,
                        {MakeSsaTemp(Remat1, 1), In0}});
    B2.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Remat2, 1), NullOp,
                        {MakeSsaTemp(Anchors[1], 1, 0), MakeImm(0x40400000u)}});
    const uint32_t UseR2InB3 = Ssa.AllocValue();
    B3.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(UseR2InB3, 1), NullOp,
                        {MakeSsaTemp(Remat2, 1), In1}});
    B1.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 1, 0, 1, 0},
                        NullOp, {MakeSsaTemp(Anchors[0], 1, 0)}});
    uint32_t Acc = DupSq;
    for (uint32_t V : Anchors) {
      const uint32_t N = Ssa.AllocValue();
      B4.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(N, 1), NullOp,
                          {MakeSsaTemp(Acc, 1), MakeSsaTemp(V, 1, 0)}});
      Acc = N;
    }
    const uint32_t Fin1 = Ssa.AllocValue(), Fin2 = Ssa.AllocValue(), Fin3 = Ssa.AllocValue();
    B4.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Fin1, 1), NullOp,
                        {MakeSsaTemp(Acc, 1), MakeSsaTemp(Remat1, 1)}});
    B4.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Fin2, 1), NullOp,
                        {MakeSsaTemp(Fin1, 1), MakeSsaTemp(Remat2, 1)}});
    B4.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Fin3, 1), NullOp,
                        {MakeSsaTemp(Fin2, 1), MakeSsaTemp(UseR1InB2, 1)}});
    B4.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                        NullOp, {MakeSsaTemp(Fin3, 1)}});

    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    std::string Err;
    if (!RunOptPressureShrink(Ssa, Dom) || !Ssa.Verify(Err)) {
      std::fprintf(stderr, "OptPressureShrink two-phase remat/dup-sink failed: %s\n",
                   Err.c_str());
      return false;
    }
    // DupSrc & DupSq must both have sunk out of B0; Remat1 must have been rematted out of B0.
    if (B0.Insts.size() != 50) {
      std::fprintf(stderr, "OptPressureShrink expected 50 RSQ anchors left in B0, got %zu\n",
                   B0.Insts.size());
      return false;
    }
  }
  return true;
}

bool TestCodexAuditOptimizations() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1, 0};
  const Operand In1{OperandKind::Input, 1, 0, 1, 0};
  const Operand In2{OperandKind::Input, 2, 0, 1, 0};
  using PhiInputT = decltype(SsaPhi::Inputs)::value_type;
  auto MakePhi = [](uint32_t Result, uint8_t Mask,
                    std::initializer_list<PhiInputT> Inputs) {
    SsaPhi P;
    P.Result = Result;
    P.Mask = Mask;
    P.Inputs.append(Inputs.begin(), Inputs.end());
    return P;
  };
  auto MakeImmVec = [](MirProgram &Mir, uint32_t X, uint32_t Y, uint32_t Z,
                       uint32_t W, uint8_t Mask = 0xfu) {
    const uint32_t Idx = static_cast<uint32_t>(Mir.LiteralVectors.size());
    Mir.LiteralVectors.push_back({X, Y, Z, W});
    return Operand{OperandKind::ImmediateVector, Idx, 0, Mask, kIdentitySwizzle};
  };

  // 1. Dominator-path branch predicate propagation folds dominated MOVC and
  //    nested redundant if_nz on the same condition.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(5);
    const uint32_t Cond = Ssa.AllocValue();
    const uint32_t Sel = Ssa.AllocValue();
    const uint32_t TrueV = Ssa.AllocValue();
    const uint32_t FalseV = Ssa.AllocValue();
    const uint32_t OutPhi = Ssa.AllocValue();

    // B0: Cond = NE(In0, 0); if_nz Cond -> B1 else B4
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeSsaTemp(Cond);
    Ssa.Blocks[0].Successors = {1, 4};
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_NE, MakeSsaTemp(Cond), NullOp, {In0, MakeImm(0u)}});

    // B1: Sel = MOVC(Cond, In1, In2); redundant inner if_nz Cond -> B2 else B3
    Ssa.Blocks[1].Kind = FlowKind::Branch;
    Ssa.Blocks[1].Condition = MakeSsaTemp(Cond);
    Ssa.Blocks[1].Successors = {2, 3};
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Sel), NullOp,
         {MakeSsaTemp(Cond), In1, In2}});

    // B2 (reachable): non-speculatable LD + ADD(Sel, In2) -> B4
    const Operand Tex0{OperandKind::Resource, 0, 0, 0xfu, kIdentitySwizzle};
    const uint32_t Ld2 = Ssa.AllocValue(), Ld3 = Ssa.AllocValue();
    Ssa.Blocks[2].Kind = FlowKind::Jump;
    Ssa.Blocks[2].Successors = {4};
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_LD, MakeSsaTemp(Ld2), NullOp,
         {Operand{OperandKind::Input, 1, 0, 0xfu, kIdentitySwizzle}, Tex0}});
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(TrueV), NullOp,
         {MakeSsaTemp(Sel), MakeSsaTemp(Ld2)}});

    // B3 (unreachable): non-speculatable LD + MUL(In1, In2) -> B4
    Ssa.Blocks[3].Kind = FlowKind::Jump;
    Ssa.Blocks[3].Successors = {4};
    Ssa.Blocks[3].Insts.push_back(
        {D3D10_SB_OPCODE_LD, MakeSsaTemp(Ld3), NullOp,
         {Operand{OperandKind::Input, 2, 0, 0xfu, kIdentitySwizzle}, Tex0}});
    Ssa.Blocks[3].Insts.push_back(
        {D3D10_SB_OPCODE_MUL, MakeSsaTemp(FalseV), NullOp,
         {MakeSsaTemp(Ld3), In2}});

    // B4: return Phi(B0: 0, B2: TrueV, B3: FalseV)
    Ssa.Blocks[4].Kind = FlowKind::Return;
    Ssa.Blocks[4].Phis.push_back(
        MakePhi(OutPhi, 1, {{0, MakeImm(0u)}, {2, MakeSsaTemp(TrueV)},
                            {3, MakeSsaTemp(FalseV)}}));
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
         NullOp, {MakeSsaTemp(OutPhi)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Codex #1 SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned Branches = 0, Movcs = 0, Muls = 0;
    for (const auto &Blk : Ssa.Blocks) {
      if (Blk.Kind == FlowKind::Branch)
        ++Branches;
      for (const auto &I : Blk.Insts) {
        if (I.Opcode == D3D10_SB_OPCODE_MOVC)
          ++Movcs;
        if (I.Opcode == D3D10_SB_OPCODE_MUL)
          ++Muls;
      }
    }
    if (Branches > 1 || Movcs != 0 || Muls != 0) {
      std::fprintf(stderr, "Codex #1 pred prop failed: branches=%u movcs=%u muls=%u\n",
                   Branches, Movcs, Muls);
      return false;
    }
  }

  // 2. Unconditional non-header loop-block scalar-chain LICM hoists
  //    `any(InVec != 0)` (4 ops -> 1 scalar bool) out of a post-diamond loop block.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(6);
    const Operand InVec{OperandKind::Input, 1, 0, 0xfu, kIdentitySwizzle};
    const uint32_t IdxPhi = Ssa.AllocValue(), AccPhi = Ssa.AllocValue();
    const uint32_t LoopCond = Ssa.AllocValue();
    const uint32_t B3Val = Ssa.AllocValue(), MergePhi = Ssa.AllocValue();
    const uint32_t VNe = Ssa.AllocValue(), VOr1 = Ssa.AllocValue();
    const uint32_t VOr2 = Ssa.AllocValue(), VAny = Ssa.AllocValue();
    const uint32_t Added = Ssa.AllocValue(), NextAcc = Ssa.AllocValue();
    const uint32_t NextIdx = Ssa.AllocValue();

    // B0 (preheader) -> B1
    Ssa.Blocks[0].Kind = FlowKind::Jump;
    Ssa.Blocks[0].Successors = {1};
    // B1 (loop header): if_nz (Idx < 4) -> B2 else B5
    Ssa.Blocks[1].Kind = FlowKind::Branch;
    Ssa.Blocks[1].Condition = MakeSsaTemp(LoopCond);
    Ssa.Blocks[1].Successors = {2, 5};
    Ssa.Blocks[1].Phis.push_back(
        MakePhi(IdxPhi, 1, {{0, MakeImm(0u)}, {4, MakeSsaTemp(NextIdx)}}));
    Ssa.Blocks[1].Phis.push_back(
        MakePhi(AccPhi, 1, {{0, MakeImm(0u)}, {4, MakeSsaTemp(NextAcc)}}));
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_ULT, MakeSsaTemp(LoopCond), NullOp,
         {MakeSsaTemp(IdxPhi), MakeImm(4u)}});
    // B2 (inner branch inside loop): if_nz In0 -> B3 else B4
    Ssa.Blocks[2].Kind = FlowKind::Branch;
    Ssa.Blocks[2].Condition = In0;
    Ssa.Blocks[2].Successors = {3, 4};
    // B3 -> B4
    Ssa.Blocks[3].Kind = FlowKind::Jump;
    Ssa.Blocks[3].Successors = {4};
    Ssa.Blocks[3].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(B3Val), NullOp,
         {MakeSsaTemp(AccPhi), In2}});
    // B4 (unconditional non-header loop merge + backedge to B1)
    Ssa.Blocks[4].Kind = FlowKind::Jump;
    Ssa.Blocks[4].Successors = {1};
    Ssa.Blocks[4].Phis.push_back(
        MakePhi(MergePhi, 1, {{2, MakeSsaTemp(AccPhi)}, {3, MakeSsaTemp(B3Val)}}));
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_NE, MakeSsaTemp(VNe, 0xfu), NullOp,
         {InVec, MakeImmVec(Mir, 0u, 0u, 0u, 0u)}});
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_OR, MakeSsaTemp(VOr1, 1), NullOp,
         {MakeSsaTemp(VNe, 1, 0x00u), MakeSsaTemp(VNe, 1, 0x55u)}});
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_OR, MakeSsaTemp(VOr2, 1), NullOp,
         {MakeSsaTemp(VOr1, 1), MakeSsaTemp(VNe, 1, 0xaau)}});
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_OR, MakeSsaTemp(VAny, 1), NullOp,
         {MakeSsaTemp(VOr2, 1), MakeSsaTemp(VNe, 1, 0xffu)}});
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(Added, 1), NullOp,
         {MakeSsaTemp(MergePhi, 1), In0}});
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_MOVC, MakeSsaTemp(NextAcc, 1), NullOp,
         {MakeSsaTemp(VAny, 1), MakeSsaTemp(Added, 1), MakeSsaTemp(MergePhi, 1)}});
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_IADD, MakeSsaTemp(NextIdx, 1), NullOp,
         {MakeSsaTemp(IdxPhi, 1), MakeImm(1u)}});
    // B5 (exit)
    Ssa.Blocks[5].Kind = FlowKind::Return;
    Ssa.Blocks[5].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
         NullOp, {MakeSsaTemp(AccPhi, 1)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Codex #2 LICM SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned PreheaderOrs = 0, LoopOrs = 0;
    for (const auto &I : Ssa.Blocks[0].Insts)
      PreheaderOrs += (I.Opcode == D3D10_SB_OPCODE_OR);
    for (size_t B = 1; B < Ssa.Blocks.size(); ++B)
      for (const auto &I : Ssa.Blocks[B].Insts)
        LoopOrs += (I.Opcode == D3D10_SB_OPCODE_OR);
    if (PreheaderOrs < 3 || LoopOrs != 0) {
      std::fprintf(stderr, "Codex #2 scalar-chain LICM failed: preheaderOrs=%u loopOrs=%u\n",
                   PreheaderOrs, LoopOrs);
      return false;
    }
  }

  // 3. deriv_rtx_coarse(l(0)) / deriv_rty_coarse(l(0)) -> 0.0 (+ add_sat cascade).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Dx = Ssa.AllocValue(), Dy = Ssa.AllocValue(), Sum = Ssa.AllocValue();
    B.Insts.push_back(
        {D3D11_SB_OPCODE_DERIV_RTX_COARSE, MakeSsaTemp(Dx), NullOp, {MakeImm(0u)}});
    B.Insts.push_back(
        {D3D11_SB_OPCODE_DERIV_RTY_COARSE, MakeSsaTemp(Dy), NullOp, {MakeImm(0u)}});
    SsaInst AddSat{D3D10_SB_OPCODE_ADD, MakeSsaTemp(Sum), NullOp,
                   {AbsMirOperand(MakeSsaTemp(Dx)), AbsMirOperand(MakeSsaTemp(Dy))}};
    AddSat.Saturate = true;
    B.Insts.push_back(AddSat);
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
         NullOp, {MakeSsaTemp(Sum)}});

    OptimizeSsaMir(Ssa);
    if (B.Insts.size() != 1 || B.Insts[0].Opcode != D3D10_SB_OPCODE_MOV ||
        B.Insts[0].Sources[0].Kind != OperandKind::Immediate ||
        B.Insts[0].Sources[0].Index != 0u) {
      std::fprintf(stderr, "Codex #3a deriv(0) fold failed (insts=%zu)\n", B.Insts.size());
      return false;
    }
  }

  // 4. udiv by power-of-two immediate: rem-only -> AND, quot-only -> USHR.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Rem = Ssa.AllocValue(), Quot = Ssa.AllocValue();
    B.Insts.push_back(
        {D3D10_SB_OPCODE_UDIV, NullOp, MakeSsaTemp(Rem), {In0, MakeImm(4u)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_UDIV, MakeSsaTemp(Quot), NullOp, {In1, MakeImm(8u)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
         NullOp, {MakeSsaTemp(Rem)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 2, 0},
         NullOp, {MakeSsaTemp(Quot)}});

    OptimizeSsaMir(Ssa);
    unsigned Udivs = 0, Ands = 0, Ushrs = 0;
    for (const auto &I : B.Insts) {
      if (I.Opcode == D3D10_SB_OPCODE_UDIV) ++Udivs;
      if (I.Opcode == D3D10_SB_OPCODE_AND &&
          I.Sources[1].Kind == OperandKind::Immediate &&
          I.Sources[1].Index == 3u) ++Ands;
      if (I.Opcode == D3D10_SB_OPCODE_USHR &&
          I.Sources[1].Kind == OperandKind::Immediate &&
          I.Sources[1].Index == 3u) ++Ushrs;
    }
    if (Udivs != 0 || Ands != 1 || Ushrs != 1) {
      std::fprintf(stderr, "Codex #3b udiv pow2 failed: udiv=%u and=%u ushr=%u\n",
                   Udivs, Ands, Ushrs);
      return false;
    }
  }

  // 5. Non-negative float propagation across max(rgb, 0) + positive-coeff dp3
  //    eliminates lt & movc on clamped Oklab, while unclamped input preserves them.
  for (int Clamped = 0; Clamped <= 1; ++Clamped) {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const Operand InRgb{OperandKind::Input, 0, 0, 0x7u, kIdentitySwizzle};
    Operand DpSrc = InRgb;
    if (Clamped) {
      const uint32_t Cl = Ssa.AllocValue();
      B.Insts.push_back(
          {D3D10_SB_OPCODE_MAX, MakeSsaTemp(Cl, 0x7u), NullOp,
           {InRgb, MakeImmVec(Mir, 0u, 0u, 0u, 0u, 0x7u)}});
      DpSrc = MakeSsaTemp(Cl, 0x7u, kIdentitySwizzle);
    }
    const uint32_t Dot = Ssa.AllocValue();
    const uint32_t Lg = Ssa.AllocValue(), Mul = Ssa.AllocValue(), Pow = Ssa.AllocValue();
    const uint32_t Pos = Ssa.AllocValue(), Neg = Ssa.AllocValue();
    const uint32_t Sub = Ssa.AllocValue(), Out = Ssa.AllocValue();
    // Positive Oklab matrix row: (0.41222147, 0.53633255, 0.05144599)
    B.Insts.push_back(
        {D3D10_SB_OPCODE_DP3, MakeSsaTemp(Dot), NullOp,
         {DpSrc, MakeImmVec(Mir, 0x3ed30f62u, 0x3f094d13u, 0x3d52ba34u, 0u, 0x7u)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_LOG, MakeSsaTemp(Lg), NullOp, {AbsMirOperand(MakeSsaTemp(Dot))}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MUL, MakeSsaTemp(Mul), NullOp,
         {MakeSsaTemp(Lg), MakeImm(0x3eaaaaabu)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_EXP, MakeSsaTemp(Pow), NullOp, {MakeSsaTemp(Mul)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_LT, MakeSsaTemp(Pos), NullOp, {MakeImm(0u), MakeSsaTemp(Dot)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_LT, MakeSsaTemp(Neg), NullOp, {MakeSsaTemp(Dot), MakeImm(0u)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Sub), NullOp,
         {MakeSsaTemp(Neg), NegateMirOperand(MakeSsaTemp(Pow)), MakeImm(0u)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MOVC, MakeSsaTemp(Out), NullOp,
         {MakeSsaTemp(Pos), MakeSsaTemp(Pow), MakeSsaTemp(Sub)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
         NullOp, {MakeSsaTemp(Out)}});

    OptimizeSsaMir(Ssa);
    unsigned Lts = 0, Movcs = 0;
    for (const auto &I : B.Insts) {
      Lts += (I.Opcode == D3D10_SB_OPCODE_LT);
      Movcs += (I.Opcode == D3D10_SB_OPCODE_MOVC);
    }
    if (Clamped && (Lts != 0 || Movcs != 0)) {
      std::fprintf(stderr, "Codex #4 clamped Oklab failed: lt=%u movc=%u\n", Lts, Movcs);
      return false;
    }
    if (!Clamped && (Lts == 0 || Movcs == 0)) {
      std::fprintf(stderr, "Codex #4 unclamped Oklab lost sign guard: lt=%u movc=%u\n",
                   Lts, Movcs);
      return false;
    }
  }

  // 6. Shared-divisor >= 3 DIVs -> 1 RCP + N MULs when !NanSafe, kept when NanSafe.
  for (int NanSafe = 0; NanSafe <= 1; ++NanSafe) {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.NanSafe = (NanSafe != 0);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t Den = Ssa.AllocValue();
    const uint32_t Q0 = Ssa.AllocValue(), Q1 = Ssa.AllocValue(), Q2 = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(Den), NullOp, {In0, In1}});
    B.Insts.push_back({D3D10_SB_OPCODE_DIV, MakeSsaTemp(Q0), NullOp, {In0, MakeSsaTemp(Den)}});
    B.Insts.push_back({D3D10_SB_OPCODE_DIV, MakeSsaTemp(Q1), NullOp, {In1, MakeSsaTemp(Den)}});
    B.Insts.push_back({D3D10_SB_OPCODE_DIV, MakeSsaTemp(Q2), NullOp, {In2, MakeSsaTemp(Den)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1, 0},
                       NullOp, {MakeSsaTemp(Q0)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 2, 0},
                       NullOp, {MakeSsaTemp(Q1)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 4, 0},
                       NullOp, {MakeSsaTemp(Q2)}});

    OptimizeSsaMir(Ssa);
    unsigned Divs = 0, Rcps = 0, Muls = 0;
    for (const auto &I : B.Insts) {
      Divs += (I.Opcode == D3D10_SB_OPCODE_DIV);
      Rcps += (I.Opcode == D3D11_SB_OPCODE_RCP);
      Muls += (I.Opcode == D3D10_SB_OPCODE_MUL);
    }
    if (!NanSafe && (Divs != 0 || Rcps != 1 || Muls != 3)) {
      std::fprintf(stderr, "Codex #2b shared-div rcp failed: div=%u rcp=%u mul=%u\n",
                   Divs, Rcps, Muls);
      return false;
    }
    if (NanSafe && (Divs != 3 || Rcps != 0)) {
      std::fprintf(stderr, "Codex #2b NanSafe shared-div failed: div=%u rcp=%u\n",
                   Divs, Rcps);
      return false;
    }
  }

  // 7. Dominator-guarded `ge(3.0, idx) -> ftou(idx) -> x0[idx]` deletes dead
  //    stores x0[4..11] and shrinks x0[12], 3 -> x0[4], 3.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 12, 3});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const uint32_t Idx = Ssa.AllocValue();
    const uint32_t Cmp = Ssa.AllocValue(), UIdx = Ssa.AllocValue();
    const uint32_t Loaded = Ssa.AllocValue(), OutPhi = Ssa.AllocValue();

    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeSsaTemp(Cmp);
    Ssa.Blocks[0].Successors = {1, 2};
    for (uint32_t E = 0; E < 12; ++E) {
      Operand Dst{OperandKind::IndexableTemp, 0, E, 0x7u, kIdentitySwizzle};
      Operand Val = E < 4
                        ? Operand{OperandKind::ConstantBuffer, 0, E, 0x7u, kIdentitySwizzle}
                        : MakeImmVec(Mir, 0u, 0u, 0u, 0u, 0x7u);
      Ssa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MOV, Dst, NullOp, {Val}});
    }
    // Idx = In0 - 1.0f; Cmp = (3.0f >= Idx)
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(Idx), NullOp, {In0, MakeImm(0xbf800000u)}});
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_GE, MakeSsaTemp(Cmp), NullOp,
         {MakeImm(0x40400000u), MakeSsaTemp(Idx)}});

    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_FTOU, MakeSsaTemp(UIdx), NullOp, {MakeSsaTemp(Idx)}});
    Operand DynRead{OperandKind::IndexableTemp, 0, 0, 0x7u, kIdentitySwizzle};
    DynRead.RelativeRegister = UIdx;
    DynRead.RelativeLane = 0;
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, MakeSsaTemp(Loaded, 0x7u), NullOp, {DynRead}});

    Ssa.Blocks[2].Kind = FlowKind::Return;
    Ssa.Blocks[2].Phis.push_back(
        MakePhi(OutPhi, 0x7u,
                {{0, MakeImmVec(Mir, 0u, 0u, 0u, 0u, 0x7u)},
                 {1, MakeSsaTemp(Loaded, 0x7u)}}));
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 0x7u, kIdentitySwizzle},
         NullOp, {MakeSsaTemp(OutPhi, 0x7u)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Codex R2 #3 SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    unsigned XStores = 0;
    for (const auto &Blk : Ssa.Blocks)
      for (const auto &I : Blk.Insts)
        if (I.Dest.Kind == OperandKind::IndexableTemp)
          ++XStores;
    if (Mir.IndexableTemps.size() != 1 || Mir.IndexableTemps[0].Elements != 4 ||
        XStores != 4) {
      std::fprintf(stderr, "Codex R2 #3 x1 shrink failed: elems=%u stores=%u\n",
                   Mir.IndexableTemps.empty() ? 0u : Mir.IndexableTemps[0].Elements,
                   XStores);
      return false;
    }
  }

  // 8. Transposed 3x3 gather PseudoCombineVec + mul/mad/mad -> 3 DP3s.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const Operand V0{OperandKind::Input, 0, 0, 0x7u, kIdentitySwizzle};
    const Operand V1{OperandKind::Input, 1, 0, 0x7u, kIdentitySwizzle};
    const Operand V2{OperandKind::Input, 2, 0, 0x7u, kIdentitySwizzle};
    const Operand Vec{OperandKind::Input, 3, 0, 0x7u, kIdentitySwizzle};
    auto LaneOf = [](Operand Op, unsigned L) {
      Op.Mask = 1u;
      Op.Swizzle = BroadcastLaneSwizzle(L);
      return Op;
    };
    const uint32_t G0 = Ssa.AllocValue(), G1 = Ssa.AllocValue(), G2 = Ssa.AllocValue();
    const uint32_t M0 = Ssa.AllocValue(), M1 = Ssa.AllocValue(), M2 = Ssa.AllocValue();
    B.Insts.push_back({PseudoCombineVec, MakeSsaTemp(G0, 0x7u), NullOp,
                       {LaneOf(V0, 1), LaneOf(V1, 1), LaneOf(V2, 1)}});
    B.Insts.push_back({PseudoCombineVec, MakeSsaTemp(G1, 0x7u), NullOp,
                       {LaneOf(V0, 0), LaneOf(V1, 0), LaneOf(V2, 0)}});
    B.Insts.push_back({PseudoCombineVec, MakeSsaTemp(G2, 0x7u), NullOp,
                       {LaneOf(V0, 2), LaneOf(V1, 2), LaneOf(V2, 2)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M0, 0x7u, kIdentitySwizzle), NullOp,
                       {LaneOf(Vec, 1), MakeSsaTemp(G0, 0x7u, kIdentitySwizzle)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(M1, 0x7u, kIdentitySwizzle), NullOp,
                       {LaneOf(Vec, 0), MakeSsaTemp(G1, 0x7u, kIdentitySwizzle),
                        MakeSsaTemp(M0, 0x7u, kIdentitySwizzle)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MAD, MakeSsaTemp(M2, 0x7u, kIdentitySwizzle), NullOp,
                       {LaneOf(Vec, 2), MakeSsaTemp(G2, 0x7u, kIdentitySwizzle),
                        MakeSsaTemp(M1, 0x7u, kIdentitySwizzle)}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 0x7u, kIdentitySwizzle},
                       NullOp, {MakeSsaTemp(M2, 0x7u, kIdentitySwizzle)}});

    OptimizeSsaMir(Ssa);
    unsigned Dp3s = 0, Mads = 0;
    for (const auto &I : B.Insts) {
      Dp3s += (I.Opcode == D3D10_SB_OPCODE_DP3);
      Mads += (I.Opcode == D3D10_SB_OPCODE_MAD);
    }
    if (Dp3s != 3 || Mads != 0) {
      std::fprintf(stderr, "Codex R2 #4 transposed gather->dp3 failed: dp3=%u mad=%u\n",
                   Dp3s, Mads);
      return false;
    }
  }

  // 9. Per-lane const prop on mixed (dyn, 0, 0, 0) vector folds lt/or/if_nz
  //    and eliminates unreachable sample_l.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const Operand Tex0{OperandKind::Resource, 0, 0, 0xfu, kIdentitySwizzle};
    const Operand Smp0{OperandKind::Sampler, 0, 0, 0xfu, kIdentitySwizzle};
    const uint32_t Vec = Ssa.AllocValue(), Cmp = Ssa.AllocValue();
    const uint32_t SmpDead = Ssa.AllocValue(), OutPhi = Ssa.AllocValue();

    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeSsaTemp(Cmp, 1u, BroadcastLaneSwizzle(1));
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back(
        {PseudoCombineVec, MakeSsaTemp(Vec, 0xfu, kIdentitySwizzle), NullOp,
         {In0, MakeImm(0u), MakeImm(0u), MakeImm(0u)}});
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_LT, MakeSsaTemp(Cmp, 0xfu, kIdentitySwizzle), NullOp,
         {MakeImm(0u), MakeSsaTemp(Vec, 0xfu, kIdentitySwizzle)}});

    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_SAMPLE_L, MakeSsaTemp(SmpDead, 0xfu, kIdentitySwizzle),
         NullOp, {Operand{OperandKind::Input, 1, 0, 0x3u, kIdentitySwizzle},
                  Tex0, Smp0, MakeImm(0u)}});

    Ssa.Blocks[2].Kind = FlowKind::Return;
    Ssa.Blocks[2].Phis.push_back(
        MakePhi(OutPhi, 1u,
                {{0, MakeImm(0u)},
                 {1, MakeSsaTemp(SmpDead, 1u, BroadcastLaneSwizzle(1))}}));
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0},
         NullOp, {MakeSsaTemp(OutPhi)}});

    OptimizeSsaMir(Ssa);
    unsigned Samples = 0;
    for (const auto &Blk : Ssa.Blocks)
      for (const auto &I : Blk.Insts)
        Samples += (I.Opcode == D3D10_SB_OPCODE_SAMPLE_L);
    if (Samples != 0) {
      std::fprintf(stderr, "Codex R3 #1 unreachable sample_l remained: %u\n", Samples);
      return false;
    }
  }

  // 10. Demanded texture writemask narrowing + OutOfSsa combine retargeting
  //     packs 3 single-channel samples (R, G, B) directly with 0 MOVs.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const Operand Tex0{OperandKind::Resource, 0, 0, 0xfu, kIdentitySwizzle};
    const Operand Smp0{OperandKind::Sampler, 0, 0, 0xfu, kIdentitySwizzle};
    const uint32_t S0 = Ssa.AllocValue(), S1 = Ssa.AllocValue(), S2 = Ssa.AllocValue();
    const uint32_t Rgb = Ssa.AllocValue();
    B.Insts.push_back({D3D10_SB_OPCODE_SAMPLE, MakeSsaTemp(S0, 0xfu, kIdentitySwizzle),
                       NullOp, {In0, Tex0, Smp0}});
    B.Insts.push_back({D3D10_SB_OPCODE_SAMPLE, MakeSsaTemp(S1, 0xfu, kIdentitySwizzle),
                       NullOp, {In1, Tex0, Smp0}});
    B.Insts.push_back({D3D10_SB_OPCODE_SAMPLE, MakeSsaTemp(S2, 0xfu, kIdentitySwizzle),
                       NullOp, {In2, Tex0, Smp0}});
    B.Insts.push_back({PseudoCombineVec, MakeSsaTemp(Rgb, 0x7u, kIdentitySwizzle), NullOp,
                       {MakeSsaTemp(S0, 1u, BroadcastLaneSwizzle(0)),
                        MakeSsaTemp(S1, 1u, BroadcastLaneSwizzle(1)),
                        MakeSsaTemp(S2, 1u, BroadcastLaneSwizzle(2))}});
    B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                       Operand{OperandKind::Output, 0, 0, 0x7u, kIdentitySwizzle},
                       NullOp, {MakeSsaTemp(Rgb, 0x7u, kIdentitySwizzle)}});

    OptimizeSsaMir(Ssa);
    unsigned NarrowedSamples = 0;
    for (const auto &I : B.Insts)
      if (I.Opcode == D3D10_SB_OPCODE_SAMPLE && PopcountMask(I.Dest.Mask) == 1)
        ++NarrowedSamples;
    std::vector<FlowBlock> Flow;
    LowerOutOfSsa(Ssa, Flow);
    unsigned TempMovs = 0;
    for (const auto &I : Flow[0].Instructions)
      if (I.Opcode == D3D10_SB_OPCODE_MOV && I.Operands[0].Kind == OperandKind::Temp)
        ++TempMovs;
    if (NarrowedSamples != 3 || TempMovs != 0) {
      std::fprintf(stderr, "Codex R3 #2 tex mask/coalesce failed: narrowed=%u movs=%u\n",
                   NarrowedSamples, TempMovs);
      return false;
    }
  }

  // 11. Fixed-trip-count loop (N=10) with pure ExitSlice and back-only sample_d
  //     peels last iteration (N=9 in loop + peeled ExitSlice in exit block).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const Operand Tex0{OperandKind::Resource, 0, 0, 0xfu, kIdentitySwizzle};
    const Operand Smp0{OperandKind::Sampler, 0, 0, 0xfu, kIdentitySwizzle};
    const uint32_t IdxPhi = Ssa.AllocValue(), StatePhi = Ssa.AllocValue();
    const uint32_t Uv = Ssa.AllocValue(), TexR = Ssa.AllocValue();
    const uint32_t NextState = Ssa.AllocValue(), NextIdx = Ssa.AllocValue();
    const uint32_t Done = Ssa.AllocValue();

    // B0 preheader -> B1
    Ssa.Blocks[0].Kind = FlowKind::Jump;
    Ssa.Blocks[0].Successors = {1};
    // B1 loop header/latch: Uv = State + In0; sample_d(Uv) feeds NextState;
    // break when NextIdx == 10.
    Ssa.Blocks[1].Kind = FlowKind::Branch;
    Ssa.Blocks[1].Condition = MakeSsaTemp(Done);
    Ssa.Blocks[1].Successors = {2, 1};
    Ssa.Blocks[1].Phis.push_back(
        MakePhi(IdxPhi, 1u, {{0, MakeImm(0u)}, {1, MakeSsaTemp(NextIdx)}}));
    Ssa.Blocks[1].Phis.push_back(
        MakePhi(StatePhi, 1u, {{0, In1}, {1, MakeSsaTemp(NextState)}}));
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(Uv), NullOp,
         {MakeSsaTemp(StatePhi), In0}});
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_SAMPLE_D, MakeSsaTemp(TexR, 0xfu, kIdentitySwizzle),
         NullOp, {MakeSsaTemp(Uv), Tex0, Smp0, In1, In2}});
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(NextState), NullOp,
         {MakeSsaTemp(StatePhi), MakeSsaTemp(TexR, 1u, 0)}});
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_IADD, MakeSsaTemp(NextIdx), NullOp,
         {MakeSsaTemp(IdxPhi), MakeImm(1u)}});
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_IEQ, MakeSsaTemp(Done), NullOp,
         {MakeSsaTemp(NextIdx), MakeImm(10u)}});
    // B2 exit: returns ONLY Uv (NOT TexR or NextState).
    Ssa.Blocks[2].Kind = FlowKind::Return;
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0},
         NullOp, {MakeSsaTemp(Uv)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err)) {
      std::fprintf(stderr, "Codex R3 #3 loop peel SSA verify failed: %s\n", Err.c_str());
      return false;
    }
    bool SawBound9 = false;
    for (const auto &I : Ssa.Blocks[1].Insts)
      if (I.Opcode == D3D10_SB_OPCODE_IEQ &&
          I.Sources[1].Kind == OperandKind::Immediate &&
          I.Sources[1].Index == 9u)
        SawBound9 = true;
    if (!SawBound9) {
      std::fprintf(stderr, "Codex R3 #3 loop peel did not decrement bound 10->9\n");
      return false;
    }
  }

  // 12. Local array construction & pure producers sink into the sole reading branch.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    Mir.IndexableTemps.push_back({0, 4, 1});
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const uint32_t BaseV = Ssa.AllocValue(), DynIdx = Ssa.AllocValue();
    const uint32_t Loaded = Ssa.AllocValue(), Res = Ssa.AllocValue();
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = In0;
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(BaseV), NullOp, {In1, MakeImm(0x3f800000u)}});
    for (uint32_t K = 0; K < 4; ++K)
      Ssa.Blocks[0].Insts.push_back(
          {D3D10_SB_OPCODE_MOV, Operand{OperandKind::IndexableTemp, 0, K, 1u, 0},
           NullOp, {K == 0 ? MakeSsaTemp(BaseV) : MakeImm(K)}});
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_FTOU, MakeSsaTemp(DynIdx), NullOp, {In2}});
    Operand ArrRead{OperandKind::IndexableTemp, 0, 0, 1u, 0};
    ArrRead.RelativeRegister = DynIdx;
    Ssa.Blocks[1].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, MakeSsaTemp(Loaded), NullOp, {ArrRead}});
    Ssa.Blocks[2].Kind = FlowKind::Return;
    Ssa.Blocks[2].Phis.push_back(
        MakePhi(Res, 1u, {{0, MakeImm(0u)}, {1, MakeSsaTemp(Loaded)}}));
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0},
         NullOp, {MakeSsaTemp(Res)}});

    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    RunOptColdSink(Ssa, Dom);
    std::string Err;
    if (!Ssa.Verify(Err) || !Ssa.Blocks[0].Insts.empty() ||
        Ssa.Blocks[1].Insts.size() < 7) {
      std::fprintf(stderr, "Codex R4 #2 array sink failed: err='%s' b0=%zu b1=%zu\n",
                   Err.c_str(), Ssa.Blocks[0].Insts.size(),
                   Ssa.Blocks[1].Insts.size());
      return false;
    }
  }

  // 13. Signed range across dominating ilt + imin eliminates dead 0 < imin(n, 8).
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(5);
    const Operand CbN{OperandKind::ConstantBuffer, 0, 132, 1u, 0x55u};
    const uint32_t C1 = Ssa.AllocValue(), Clamped = Ssa.AllocValue();
    const uint32_t C2 = Ssa.AllocValue(), OutPhi = Ssa.AllocValue();
    // B0: ilt CbN, 1 ? B1 : B2
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeSsaTemp(C1);
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_ILT, MakeSsaTemp(C1), NullOp, {CbN, MakeImm(1u)}});
    // B1: early default -> B4
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {4};
    // B2 (CbN >= 1): Clamped = imin(CbN, 8); C2 = ilt(0, Clamped) ? B3 : B4
    Ssa.Blocks[2].Kind = FlowKind::Branch;
    Ssa.Blocks[2].Condition = MakeSsaTemp(C2);
    Ssa.Blocks[2].Successors = {3, 4};
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_IMIN, MakeSsaTemp(Clamped), NullOp, {CbN, MakeImm(8u)}});
    Ssa.Blocks[2].Insts.push_back(
        {D3D10_SB_OPCODE_ILT, MakeSsaTemp(C2), NullOp, {MakeImm(0u), MakeSsaTemp(Clamped)}});
    // B3: live arm -> B4
    Ssa.Blocks[3].Kind = FlowKind::Jump;
    Ssa.Blocks[3].Successors = {4};
    // B4: return
    Ssa.Blocks[4].Kind = FlowKind::Return;
    Ssa.Blocks[4].Phis.push_back(
        MakePhi(OutPhi, 1u, {{1, In0}, {2, In1}, {3, MakeSsaTemp(Clamped)}}));
    Ssa.Blocks[4].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0},
         NullOp, {MakeSsaTemp(OutPhi)}});

    OptimizeSsaMir(Ssa);
    std::string Err;
    if (!Ssa.Verify(Err) || Ssa.Blocks[2].Kind == FlowKind::Branch) {
      std::fprintf(stderr, "Codex R4 #3b signed range imin failed: err='%s' b2kind=%d\n",
                   Err.c_str(), static_cast<int>(Ssa.Blocks[2].Kind));
      return false;
    }
  }

  // 14. Pure polynomial over two-constant movc collapses to single movc.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    auto &B = Ssa.Blocks[0];
    B.Kind = FlowKind::Return;
    const uint32_t T = Ssa.AllocValue(), T3 = Ssa.AllocValue();
    const uint32_t T5 = Ssa.AllocValue(), T2 = Ssa.AllocValue();
    const uint32_t Poly = Ssa.AllocValue();
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MOVC, MakeSsaTemp(T), NullOp,
         {In0, MakeImm(0x3ee4f92eu), MakeImm(0x3f800000u)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MUL, MakeSsaTemp(T3), NullOp,
         {MakeSsaTemp(T), MakeImm(0x40400000u)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MUL, MakeSsaTemp(T5), NullOp,
         {MakeSsaTemp(T), MakeImm(0x40a00000u)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MUL, MakeSsaTemp(T2), NullOp,
         {MakeSsaTemp(T), MakeSsaTemp(T)}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MAD, MakeSsaTemp(Poly), NullOp,
         {MakeSsaTemp(T5), MakeSsaTemp(T2), NegateMirOperand(MakeSsaTemp(T3))}});
    B.Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0},
         NullOp, {MakeSsaTemp(Poly)}});

    OptimizeSsaMir(Ssa);
    unsigned MulMad = 0;
    for (const auto &I : B.Insts)
      MulMad += (I.Opcode == D3D10_SB_OPCODE_MUL || I.Opcode == D3D10_SB_OPCODE_MAD);
    if (MulMad != 0) {
      std::fprintf(stderr, "Codex R4 #4b select-const poly failed: mul/mad=%u\n", MulMad);
      return false;
    }
  }
  return true;
}

// Codex #2 counterexample: ordinary `sample` can be eliminated when dead,
// can sink across uniform branches, MUST NOT sink across divergent branches,
// while explicit-LOD `sample_l` CAN sink across divergent branches.
bool TestOpcodeSpecEffectContracts() {
  if (!InstructionCanEliminateIfDead(D3D10_SB_OPCODE_SAMPLE) ||
      InstructionCanMoveAcrossDivergentBranch(D3D10_SB_OPCODE_SAMPLE) ||
      !InstructionCanMoveAcrossUniformBranch(D3D10_SB_OPCODE_SAMPLE) ||
      !InstructionCanMoveAcrossDivergentBranch(D3D10_SB_OPCODE_SAMPLE_L)) {
    std::fprintf(stderr, "OpcodeSpec orthogonal effect flags mismatch\n");
    return false;
  }

  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 0x3u, 0x04u};
  const Operand Cond{OperandKind::Input, 1, 0, 1u, 0};
  const Operand Tex0{OperandKind::Resource, 0, 0, 0xfu, kIdentitySwizzle};
  const Operand Smp0{OperandKind::Sampler, 0, 0, 0, 0};

  auto BuildDiamond = [&](uint32_t SampleOp, bool UniformBranch,
                          bool UseResult) {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Pixel;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(4);
    const uint32_t TexVal = Ssa.AllocValue();
    const uint32_t PhiVal = Ssa.AllocValue();

    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].UniformBranch = UniformBranch;
    Ssa.Blocks[0].Condition = Cond;
    Ssa.Blocks[0].Successors = {1, 2};
    if (SampleOp == D3D10_SB_OPCODE_SAMPLE_L) {
      Ssa.Blocks[0].Insts.push_back(
          {SampleOp, MakeSsaTemp(TexVal, 0xfu), NullOp,
           {In0, Tex0, Smp0, MakeImm(0u)}});
    } else {
      Ssa.Blocks[0].Insts.push_back(
          {SampleOp, MakeSsaTemp(TexVal, 0xfu), NullOp, {In0, Tex0, Smp0}});
    }

    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {3};
    Ssa.Blocks[2].Kind = FlowKind::Jump;
    Ssa.Blocks[2].Successors = {3};
    Ssa.Blocks[3].Kind = FlowKind::Return;
    SsaPhi Phi;
    Phi.Result = PhiVal;
    Phi.Mask = 1u;
    Phi.Inputs.push_back(
        {1, UseResult ? MakeSsaTemp(TexVal, 1u) : MakeImm(0x3f800000u)});
    Phi.Inputs.push_back({2, MakeImm(0u)});
    Ssa.Blocks[3].Phis.push_back(std::move(Phi));
    Ssa.Blocks[3].Insts.push_back(
        {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0}, NullOp,
         {MakeSsaTemp(PhiVal)}});
    return std::make_pair(std::move(Mir), std::move(Ssa));
  };

  // (a) Divergent branch: ordinary SAMPLE MUST stay in B0.
  {
    auto Pair = BuildDiamond(D3D10_SB_OPCODE_SAMPLE, false, true);
    auto &Ssa = Pair.second;
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    RunOptColdSink(Ssa, Dom);
    std::string Err;
    if (!Ssa.Verify(Err) || Ssa.Blocks[0].Insts.size() != 1 ||
        Ssa.Blocks[0].Insts[0].Opcode != D3D10_SB_OPCODE_SAMPLE ||
        !Ssa.Blocks[1].Insts.empty()) {
      std::fprintf(stderr, "ColdSink unsoundly sunk SAMPLE across divergent branch\n");
      return false;
    }
  }

  // (b) Uniform branch: ordinary SAMPLE CAN sink into B1.
  {
    auto Pair = BuildDiamond(D3D10_SB_OPCODE_SAMPLE, true, true);
    auto &Ssa = Pair.second;
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    RunOptColdSink(Ssa, Dom);
    std::string Err;
    if (!Ssa.Verify(Err) || !Ssa.Blocks[0].Insts.empty() ||
        Ssa.Blocks[1].Insts.size() != 1 ||
        Ssa.Blocks[1].Insts[0].Opcode != D3D10_SB_OPCODE_SAMPLE) {
      std::fprintf(stderr, "ColdSink failed to sink SAMPLE across uniform branch\n");
      return false;
    }
  }

  // (c) Dead ordinary SAMPLE: MaskedDce MUST eliminate it.
  {
    auto Pair = BuildDiamond(D3D10_SB_OPCODE_SAMPLE, false, false);
    auto &Ssa = Pair.second;
    RunOptMaskedDce(Ssa);
    Ssa.CompactDead();
    std::string Err;
    if (!Ssa.Verify(Err) || !Ssa.Blocks[0].Insts.empty()) {
      std::fprintf(stderr, "MaskedDce failed to eliminate dead SAMPLE\n");
      return false;
    }
  }

  // (d) Divergent branch: explicit-LOD SAMPLE_L CAN sink into B1.
  {
    auto Pair = BuildDiamond(D3D10_SB_OPCODE_SAMPLE_L, false, true);
    auto &Ssa = Pair.second;
    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    RunOptColdSink(Ssa, Dom);
    std::string Err;
    if (!Ssa.Verify(Err) || !Ssa.Blocks[0].Insts.empty() ||
        Ssa.Blocks[1].Insts.size() != 1 ||
        Ssa.Blocks[1].Insts[0].Opcode != D3D10_SB_OPCODE_SAMPLE_L) {
      std::fprintf(stderr, "ColdSink failed to sink explicit-LOD SAMPLE_L\n");
      return false;
    }
  }

  return true;
}

// Codex #3 counterexample: loop counter `i = phi(0, i + 1)` with a 4th-iter
// branch `ilt(i, 4)` MUST widen to an unbounded upper bound (or revert to Top()
// if iteration budget is exhausted before convergence), NEVER folding `i < 4`
// to constant true using an unconverged snapshot like `i <= 2`.
bool TestLoopWideningAndBudgetFallback() {
  const Operand NullOp{OperandKind::Null};
  const Operand In0{OperandKind::Input, 0, 0, 1u, 0};

  MirProgram Mir;
  Mir.ShaderStage = Stage::Pixel;
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(5);
  // B0 (preheader) -> B1 (header)
  // B1: i = phi(B0: 0, B2: i_next); c = ilt(i, 4); branch c ? B2 : B4
  // B2: i_next = iadd(i, 1); cont = ilt(i_next, 10); branch cont ? B1 : B3
  // B3: jump B4
  // B4: ret o0 = i
  const uint32_t I = Ssa.AllocValue();
  const uint32_t Cmp4 = Ssa.AllocValue();
  const uint32_t INext = Ssa.AllocValue();
  const uint32_t Cont = Ssa.AllocValue();

  Ssa.Blocks[0].Kind = FlowKind::Jump;
  Ssa.Blocks[0].Successors = {1};

  Ssa.Blocks[1].Kind = FlowKind::Branch;
  Ssa.Blocks[1].Condition = MakeSsaTemp(Cmp4);
  Ssa.Blocks[1].Successors = {2, 4};
  SsaPhi PhiI;
  PhiI.Result = I;
  PhiI.Mask = 1u;
  PhiI.Inputs.push_back({0, MakeImm(0u)});
  PhiI.Inputs.push_back({2, MakeSsaTemp(INext)});
  Ssa.Blocks[1].Phis.push_back(std::move(PhiI));
  Ssa.Blocks[1].Insts.push_back(
      {D3D10_SB_OPCODE_ILT, MakeSsaTemp(Cmp4), NullOp,
       {MakeSsaTemp(I), MakeImm(4u)}});

  Ssa.Blocks[2].Kind = FlowKind::Branch;
  Ssa.Blocks[2].Condition = MakeSsaTemp(Cont);
  Ssa.Blocks[2].Successors = {1, 3};
  Ssa.Blocks[2].Insts.push_back(
      {D3D10_SB_OPCODE_IADD, MakeSsaTemp(INext), NullOp,
       {MakeSsaTemp(I), MakeImm(1u)}});
  Ssa.Blocks[2].Insts.push_back(
      {D3D10_SB_OPCODE_ILT, MakeSsaTemp(Cont), NullOp,
       {MakeSsaTemp(INext), MakeImm(10u)}});

  Ssa.Blocks[3].Kind = FlowKind::Jump;
  Ssa.Blocks[3].Successors = {4};

  Ssa.Blocks[4].Kind = FlowKind::Return;
  Ssa.Blocks[4].Insts.push_back(
      {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0}, NullOp,
       {MakeSsaTemp(I)}});

  SsaDomTree Dom;
  Ssa.ComputeDominators(Dom);

  // Running RunOptLaneConstProp MUST preserve the dynamic `ilt(i, 4)` branch!
  RunOptLaneConstProp(Ssa, Dom, /*FoldBranches=*/true);
  if (Ssa.Blocks[1].Condition.Kind != OperandKind::Temp) {
    std::fprintf(stderr, "RunOptLaneConstProp unsoundly folded loop branch ilt(i, 4)\n");
    return false;
  }
  return true;
}

bool TestAuditFixesH2M1M2H3() {
  // H3: gather4* must be eliminable if dead, but NOT movable across divergent branches!
  for (uint32_t Op : {static_cast<uint32_t>(D3D10_1_SB_OPCODE_GATHER4),
                      static_cast<uint32_t>(D3D11_SB_OPCODE_GATHER4_C),
                      static_cast<uint32_t>(D3D11_SB_OPCODE_GATHER4_PO),
                      static_cast<uint32_t>(D3D11_SB_OPCODE_GATHER4_PO_C)}) {
    if (!InstructionCanEliminateIfDead(Op) ||
        InstructionCanMoveAcrossDivergentBranch(Op)) {
      std::fprintf(stderr, "H3 FAIL: gather4 opcode %u movable across divergent branch\n", Op);
      return false;
    }
  }

  // M1: rcp(-0.0) = -inf (negative!), so lt(rcp(v0.x), 0.0) where v0.x may be -0.0
  // (e.g. from MUL(-0.0, 1.0) or unknown input) MUST NOT fold to 0!
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    const Operand In0{OperandKind::Input, 0, 0, 1, 0};
    const uint32_t AbsV = Ssa.AllocValue(); // |x| >= 0 (never -0.0)
    const uint32_t NegZ = Ssa.AllocValue(); // max(x, -0.0) may be -0.0 -> rcp may be -inf
    const uint32_t RcpNegZ = Ssa.AllocValue();
    const uint32_t LtNegZ = Ssa.AllocValue();
    auto &B0 = Ssa.Blocks[0];
    // vNegZ = max(in0, -0.0f) -> AllowNegZero=true only, NOT AllowNegZero=false!
    SsaInst Max0;
    Max0.Opcode = D3D10_SB_OPCODE_MAX;
    Max0.Dest = MakeSsaTemp(NegZ, 1, 0);
    Max0.Sources = {In0, MakeImm(0x80000000u)};
    B0.Insts.push_back(Max0);
    SsaInst Rcp;
    Rcp.Opcode = D3D11_SB_OPCODE_RCP;
    Rcp.Dest = MakeSsaTemp(RcpNegZ, 1, 0);
    Rcp.Sources = {MakeSsaTemp(NegZ, 1, 0)};
    B0.Insts.push_back(Rcp);
    SsaInst Lt;
    Lt.Opcode = D3D10_SB_OPCODE_LT;
    Lt.Dest = MakeSsaTemp(LtNegZ, 1, 0);
    Lt.Sources = {MakeSsaTemp(RcpNegZ, 1, 0), MakeImm(0u)};
    B0.Insts.push_back(Lt);
    SsaInst Out;
    Out.Opcode = D3D10_SB_OPCODE_MOV;
    Out.Dest = {OperandKind::Output, 0, 0, 1, 0};
    Out.Sources = {MakeSsaTemp(LtNegZ, 1, 0)};
    B0.Insts.push_back(Out);
    (void)AbsV;
    B0.Kind = FlowKind::Return;
    RunOptAlgebraic(Ssa);
    bool SawLt = false;
    for (const auto &I : Ssa.Blocks[0].Insts)
      if (!I.Dead && I.Opcode == D3D10_SB_OPCODE_LT)
        SawLt = true;
    if (!SawLt) {
      std::fprintf(stderr, "M1 FAIL: lt(rcp(max(x, -0.0)), 0.0) was wrongly folded!\n");
      return false;
    }
  }

  // H2: Fixed-trip loop with side-effecting store + texture op MUST NOT peel!
  {
    MirProgram Mir;
    Mir.IndexableTemps = {{0u, 4u, 1u}};
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3); // 0: preheader, 1: header/latch, 2: exit
    Ssa.Blocks[0].Kind = FlowKind::Jump;
    Ssa.Blocks[0].Successors = {1};

    const uint32_t IPhi = Ssa.AllocValue();
    const uint32_t AccPhi = Ssa.AllocValue();
    const uint32_t NextI = Ssa.AllocValue();
    const uint32_t NextAcc = Ssa.AllocValue();
    const uint32_t TexVal = Ssa.AllocValue();
    const uint32_t Cmp = Ssa.AllocValue();
    Ssa.Blocks[1].Phis.push_back({IPhi, 1, false, false, {{0, MakeImm(0u)}, {1, MakeSsaTemp(NextI, 1, 0)}}});
    Ssa.Blocks[1].Phis.push_back({AccPhi, 1, false, false, {{0, MakeImm(0u)}, {1, MakeSsaTemp(NextAcc, 1, 0)}}});

    SsaInst AddAcc;
    AddAcc.Opcode = D3D10_SB_OPCODE_ADD;
    AddAcc.Dest = MakeSsaTemp(NextAcc, 1, 0);
    AddAcc.Sources = {MakeSsaTemp(AccPhi, 1, 0), MakeImm(0x3f800000u)};
    Ssa.Blocks[1].Insts.push_back(AddAcc);

    SsaInst ArrW;
    ArrW.Opcode = D3D10_SB_OPCODE_MOV;
    ArrW.Dest = Operand{OperandKind::IndexableTemp, 0, 0, 1, 0};
    ArrW.Dest.RelativeRegister = IPhi;
    ArrW.Sources = {MakeSsaTemp(NextAcc, 1, 0)};
    Ssa.Blocks[1].Insts.push_back(ArrW);

    SsaInst Tex;
    Tex.Opcode = D3D10_SB_OPCODE_SAMPLE_L;
    Tex.Dest = MakeSsaTemp(TexVal, 1, 0);
    Tex.Sources = {Operand{OperandKind::Input, 0, 0, 0xf, 0xe4},
                   Operand{OperandKind::Resource, 0, 0, 0xf, 0xe4},
                   Operand{OperandKind::Sampler, 0, 0, 0xf, 0xe4},
                   MakeImm(0u)};
    Ssa.Blocks[1].Insts.push_back(Tex);

    SsaInst Inc;
    Inc.Opcode = D3D10_SB_OPCODE_IADD;
    Inc.Dest = MakeSsaTemp(NextI, 1, 0);
    Inc.Sources = {MakeSsaTemp(IPhi, 1, 0), MakeImm(1u)};
    Ssa.Blocks[1].Insts.push_back(Inc);

    SsaInst Lt;
    Lt.Opcode = D3D10_SB_OPCODE_ILT;
    Lt.Dest = MakeSsaTemp(Cmp, 1, 0);
    Lt.Sources = {MakeSsaTemp(NextI, 1, 0), MakeImm(4u)};
    Ssa.Blocks[1].Insts.push_back(Lt);
    Ssa.Blocks[1].Kind = FlowKind::Branch;
    Ssa.Blocks[1].Condition = MakeSsaTemp(Cmp, 1, 0);
    Ssa.Blocks[1].Successors = {1, 2};

    SsaInst Out;
    Out.Opcode = D3D10_SB_OPCODE_MOV;
    Out.Dest = {OperandKind::Output, 0, 0, 1, 0};
    Out.Sources = {MakeSsaTemp(NextAcc, 1, 0)};
    Ssa.Blocks[2].Insts.push_back(Out);
    Ssa.Blocks[2].Kind = FlowKind::Return;

    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    if (RunOptLoopPeel(Ssa, Dom)) {
      std::fprintf(stderr, "H2 FAIL: RunOptLoopPeel peeled a loop containing PseudoArrayWrite!\n");
      return false;
    }
  }

  // M2: Loop peel remapping an immediate latch phi into a negated use MUST preserve NEG!
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    Ssa.Blocks[0].Kind = FlowKind::Jump;
    Ssa.Blocks[0].Successors = {1};

    const uint32_t IPhi = Ssa.AllocValue();
    const uint32_t FlagPhi = Ssa.AllocValue();
    const uint32_t NegRes = Ssa.AllocValue();
    const uint32_t NextI = Ssa.AllocValue();
    const uint32_t TexVal = Ssa.AllocValue();
    const uint32_t Cmp = Ssa.AllocValue();
    Ssa.Blocks[1].Phis.push_back({IPhi, 1, false, false, {{0, MakeImm(0u)}, {1, MakeSsaTemp(NextI, 1, 0)}}});
    // Latch input is +1.0f (0x3f800000); Exit slice uses -FlagPhi!
    Ssa.Blocks[1].Phis.push_back({FlagPhi, 1, false, false, {{0, MakeImm(0u)}, {1, MakeImm(0x3f800000u)}}});

    Operand NegFlag = MakeSsaTemp(FlagPhi, 1, 0);
    NegFlag.Modifier = D3D10_SB_OPERAND_MODIFIER_NEG;
    SsaInst AddNeg;
    AddNeg.Opcode = D3D10_SB_OPCODE_ADD;
    AddNeg.Dest = MakeSsaTemp(NegRes, 1, 0);
    AddNeg.Sources = {NegFlag, MakeImm(0x40000000u)}; // -1.0f + 2.0f
    Ssa.Blocks[1].Insts.push_back(AddNeg);

    SsaInst Tex;
    Tex.Opcode = D3D10_SB_OPCODE_SAMPLE_L;
    Tex.Dest = MakeSsaTemp(TexVal, 1, 0);
    Tex.Sources = {Operand{OperandKind::Input, 0, 0, 0xf, 0xe4},
                   Operand{OperandKind::Resource, 0, 0, 0xf, 0xe4},
                   Operand{OperandKind::Sampler, 0, 0, 0xf, 0xe4},
                   MakeImm(0u)};
    Ssa.Blocks[1].Insts.push_back(Tex);

    SsaInst Inc;
    Inc.Opcode = D3D10_SB_OPCODE_IADD;
    Inc.Dest = MakeSsaTemp(NextI, 1, 0);
    Inc.Sources = {MakeSsaTemp(IPhi, 1, 0), MakeImm(1u)};
    Ssa.Blocks[1].Insts.push_back(Inc);

    SsaInst Lt;
    Lt.Opcode = D3D10_SB_OPCODE_ILT;
    Lt.Dest = MakeSsaTemp(Cmp, 1, 0);
    Lt.Sources = {MakeSsaTemp(NextI, 1, 0), MakeImm(4u)};
    Ssa.Blocks[1].Insts.push_back(Lt);
    Ssa.Blocks[1].Kind = FlowKind::Branch;
    Ssa.Blocks[1].Condition = MakeSsaTemp(Cmp, 1, 0);
    Ssa.Blocks[1].Successors = {1, 2};

    SsaInst Out;
    Out.Opcode = D3D10_SB_OPCODE_MOV;
    Out.Dest = {OperandKind::Output, 0, 0, 1, 0};
    Out.Sources = {MakeSsaTemp(NegRes, 1, 0)};
    Ssa.Blocks[2].Insts.push_back(Out);
    Ssa.Blocks[2].Kind = FlowKind::Return;

    SsaDomTree Dom;
    Ssa.ComputeDominators(Dom);
    if (!RunOptLoopPeel(Ssa, Dom)) {
      std::fprintf(stderr, "M2 FAIL: expected peel to trigger\n");
      return false;
    }
    // Cloned ADD at start of Block 2 must have -1.0f (0xbf800000u), NOT +1.0f!
    const auto &Cloned = Ssa.Blocks[2].Insts[0];
    if (Cloned.Opcode != D3D10_SB_OPCODE_ADD ||
        Cloned.Sources[0].Kind != OperandKind::Immediate ||
        Cloned.Sources[0].Index != 0xbf800000u) {
      std::fprintf(stderr, "M2 FAIL: immediate modifier lost on peel (got 0x%x)\n",
                   Cloned.Sources[0].Index);
      return false;
    }
  }

  // R7 (IndexablePack): ImmediateVector store swizzle must be preserved when packing lane > 0!
  {
    MirProgram Mir;
    Mir.IndexableTemps = {{0, 2, 1}, {1, 2, 1}};
    SsaProgram Ssa(Mir);
    const uint32_t LitIdx = Ssa.InternLiteralVector({0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u});
    Ssa.Blocks.resize(1);
    auto &B0 = Ssa.Blocks[0];
    B0.Kind = FlowKind::Return;
    // Write x0[0] = 1.0f, x1[0] = l(1,2,3,4).z (= 3.0f = 0x40400000u), then read both.
    SsaInst W0;
    W0.Opcode = D3D10_SB_OPCODE_MOV;
    W0.Dest = {OperandKind::IndexableTemp, 0, 0, 1, 0};
    W0.Sources = {MakeImm(0x3f800000u)};
    B0.Insts.push_back(W0);
    SsaInst W1;
    W1.Opcode = D3D10_SB_OPCODE_MOV;
    W1.Dest = {OperandKind::IndexableTemp, 1, 0, 1, 0};
    W1.Sources = {Operand{OperandKind::ImmediateVector, LitIdx, 0, 1, BroadcastLaneSwizzle(2)}};
    B0.Insts.push_back(W1);
    const uint32_t Idx = Ssa.AllocValue();
    SsaInst MkIdx;
    MkIdx.Opcode = D3D10_SB_OPCODE_FTOU;
    MkIdx.Dest = MakeSsaTemp(Idx, 1, 0);
    MkIdx.Sources = {Operand{OperandKind::Input, 0, 0, 1, 0}};
    B0.Insts.push_back(MkIdx);
    const uint32_t R0 = Ssa.AllocValue();
    const uint32_t R1 = Ssa.AllocValue();
    SsaInst L0;
    L0.Opcode = D3D10_SB_OPCODE_MOV;
    L0.Dest = MakeSsaTemp(R0, 1, 0);
    Operand S0{OperandKind::IndexableTemp, 0, 0, 1, 0};
    S0.RelativeRegister = Idx;
    L0.Sources = {S0};
    B0.Insts.push_back(L0);
    SsaInst L1;
    L1.Opcode = D3D10_SB_OPCODE_MOV;
    L1.Dest = MakeSsaTemp(R1, 1, 0);
    Operand S1{OperandKind::IndexableTemp, 1, 0, 1, 0};
    S1.RelativeRegister = Idx;
    L1.Sources = {S1};
    B0.Insts.push_back(L1);
    SsaInst Out0;
    Out0.Opcode = D3D10_SB_OPCODE_ADD;
    Out0.Dest = {OperandKind::Output, 0, 0, 1, 0};
    Out0.Sources = {MakeSsaTemp(R0, 1, 0), MakeSsaTemp(R1, 1, 0)};
    B0.Insts.push_back(Out0);
    if (!RunOptIndexablePack(Ssa)) {
      std::fprintf(stderr, "R7 FAIL: expected x0+x1 to pack into 2-component x0\n");
      return false;
    }
    uint32_t Lane1Bits = 0;
    for (const auto &I : B0.Insts) {
      if (I.Dead || I.Dest.Kind != OperandKind::IndexableTemp ||
          !(I.Dest.Mask & 2u))
        continue;
      if (I.Sources[0].Kind == OperandKind::Immediate)
        Lane1Bits = I.Sources[0].Index;
      else if (I.Sources[0].Kind == OperandKind::ImmediateVector)
        Lane1Bits = Mir.LiteralVectors[I.Sources[0].Index][1];
    }
    if (Lane1Bits != 0x40400000u) {
      std::fprintf(stderr, "R7 FAIL: IndexablePack dropped ImmediateVector swizzle (got 0x%x)\n",
                   Lane1Bits);
      return false;
    }
  }

  // C5: RunOptCfgCleanup folds constant branch + removes dead block even without if-convert!
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeImm(1u);
    Ssa.Blocks[0].Successors = {1, 2};
    SsaInst Ret1;
    Ret1.Opcode = D3D10_SB_OPCODE_MOV;
    Ret1.Dest = {OperandKind::Output, 0, 0, 1, 0};
    Ret1.Sources = {MakeImm(0x3f800000u)};
    Ssa.Blocks[1].Insts.push_back(Ret1);
    Ssa.Blocks[1].Kind = FlowKind::Return;
    Ssa.Blocks[2].Kind = FlowKind::Return;
    if (!RunOptCfgCleanup(Ssa) || Ssa.Blocks.size() != 1 ||
        Ssa.Blocks[0].Kind != FlowKind::Return) {
      std::fprintf(stderr, "C5 FAIL: RunOptCfgCleanup did not fold constant branch & merge!\n");
      return false;
    }
  }

  // C6: SsaProgram::Verify rejects out-of-bounds IndexableTemp index/element.
  {
    MirProgram Mir;
    Mir.IndexableTemps = {{0, 2, 1}};
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(1);
    SsaInst Bad;
    Bad.Opcode = D3D10_SB_OPCODE_MOV;
    Bad.Dest = {OperandKind::IndexableTemp, 0, 2, 1, 0}; // Element 2 >= Elements(2)
    Bad.Sources = {MakeImm(0u)};
    Ssa.Blocks[0].Insts.push_back(Bad);
    Ssa.Blocks[0].Kind = FlowKind::Return;
    std::string Err;
    if (Ssa.Verify(Err)) {
      std::fprintf(stderr, "C6 FAIL: SsaProgram::Verify accepted out-of-bounds x0[2]!\n");
      return false;
    }
  }

  // Phase B1/B2 (Lean OpcodeSpec): PseudoCombineVec is the sole first-class
  // table-driven MIR pseudo opcode backed by GeneratedOpcodeDesc.inc.
  {
    const auto &CombD = GetOpcodeDesc(PseudoCombineVec);
    if (CombD.NumDests != 1 || !InstructionCanEliminateIfDead(PseudoCombineVec) ||
        !InstructionCanMoveAcrossDivergentBranch(PseudoCombineVec) ||
        CombD.SlotRule != SourceSlotRule::PerLaneScalar ||
        InstructionSourceSlotMask(PseudoCombineVec, 0x5u, 0) != 0x1u ||
        InstructionSourceSlotMask(PseudoCombineVec, 0x5u, 1) != 0x0u ||
        InstructionSourceSlotMask(PseudoCombineVec, 0x5u, 2) != 0x1u) {
      std::fprintf(stderr, "Phase B1 FAIL: Lean pseudo-opcode table contract mismatch!\n");
      return false;
    }
    // Phase B2 W-grade witness: 4-element array where each lane is written
    // TWICE across a branch and dynamically read (old LocalArrays locked it out
    // because WriteCount > 1; unified IndexablePlan/IndexableSsa promotes it to
    // SSA PHIs + MOVC with zero remaining IndexableTemp!).
    {
      MirProgram WMir;
      WMir.IndexableTemps = {{0u, 4u, 1u}};
      SsaProgram WSsa(WMir);
      WSsa.Blocks.resize(4); // 0: entry (write1 + branch), 1: then (write2), 2: else, 3: join (dyn read)
      const Operand Cond{OperandKind::Input, 0, 0, 1, 0};
      const Operand IdxIn{OperandKind::Input, 1, 0, 1, 0};
      const Operand NullOp{OperandKind::Null};
      const uint32_t IdxR = WSsa.AllocValue();
      WSsa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(IdxR), NullOp, {IdxIn}});
      for (uint32_t L = 0; L < 4; ++L)
        WSsa.Blocks[0].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                        Operand{OperandKind::IndexableTemp, 0, L, 1, 0},
                                        NullOp, {MakeImm(0x3f800000u + L)}});
      WSsa.Blocks[0].Kind = FlowKind::Branch;
      WSsa.Blocks[0].Condition = Cond;
      WSsa.Blocks[0].Successors = {1, 2};
      for (uint32_t L = 0; L < 4; ++L)
        WSsa.Blocks[1].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                        Operand{OperandKind::IndexableTemp, 0, L, 1, 0},
                                        NullOp, {MakeImm(0x40000000u + L)}});
      WSsa.Blocks[1].Kind = FlowKind::Jump;
      WSsa.Blocks[1].Successors = {3};
      WSsa.Blocks[2].Kind = FlowKind::Jump;
      WSsa.Blocks[2].Successors = {3};
      const uint32_t OutV = WSsa.AllocValue();
      Operand DynRd{OperandKind::IndexableTemp, 0, 0, 1, 0};
      DynRd.RelativeRegister = IdxR;
      WSsa.Blocks[3].Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(OutV), NullOp, {DynRd}});
      WSsa.Blocks[3].Insts.push_back({D3D10_SB_OPCODE_MOV,
                                      Operand{OperandKind::Output, 0, 0, 1, 0},
                                      NullOp, {MakeSsaTemp(OutV)}});
      WSsa.Blocks[3].Kind = FlowKind::Return;
      OptimizeSsaMir(WSsa);
      std::string WErr;
      if (!WSsa.Verify(WErr, /*RequireNormalizedCfg=*/true) ||
          !WMir.IndexableTemps.empty()) {
        std::fprintf(stderr, "Phase B2 witness FAIL: 4-elem multi-write array not promoted (x=%zu err=%s)\n",
                     WMir.IndexableTemps.size(), WErr.c_str());
        return false;
      }
    }
    // N-grade CFG normal form check in SsaProgram::Verify.
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeImm(1u);
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[1].Kind = FlowKind::Return;
    Ssa.Blocks[2].Kind = FlowKind::Return;
    std::string Err;
    if (Ssa.Verify(Err, /*RequireNormalizedCfg=*/true)) {
      std::fprintf(stderr, "Phase B1 FAIL: Verify(normalized) accepted IF <imm>!\n");
      return false;
    }
    RunOptCfgCleanup(Ssa);
    if (!Ssa.Verify(Err, /*RequireNormalizedCfg=*/true)) {
      std::fprintf(stderr, "Phase B1 FAIL: Verify(normalized) rejected cleaned CFG: %s\n",
                   Err.c_str());
      return false;
    }
    // Phase C1 W-grade witness: PlacementStore enforces Lean Placement.lean
    // invariants (no cross-pass ping-pong, no deeper-loop remat, monotone mu in
    // normal regime, pressure-excess reduction in high-pressure regime).
    {
      PlacementStore Store;
      // 1. LICM hoist depth 1 -> 0 accepted.
      PlacementMove Hoist;
      Hoist.ValId = 7;
      Hoist.Owner = PlacementOwner::Licm;
      Hoist.SrcDepth = 1;
      Hoist.DstDepths = {0};
      if (!Store.TryAccept(Hoist) || Store.OwnerOf(7) != PlacementOwner::Licm) {
        std::fprintf(stderr, "Phase C1 FAIL: LICM hoist rejected\n");
        return false;
      }
      // 2. PressureShrink attempting to remat LICM-owned value 7 into depth 0
      //    even under high pressure is rejected (anti-ping-pong).
      PlacementMove PingPong;
      PingPong.ValId = 7;
      PingPong.Owner = PlacementOwner::PressureShrink;
      PingPong.SrcDepth = 0;
      PingPong.DstDepths = {0, 0};
      PingPong.PlBefore = 220;
      PingPong.PlAfter = 216;
      PingPong.Budget = 180;
      if (Store.TryAccept(PingPong)) {
        std::fprintf(stderr, "Phase C1 FAIL: anti-ping-pong violated\n");
        return false;
      }
      // 3. Remat of an uncommitted value from depth 0 into depth 1 (old IsFreeMov
      //    LICM-undo bug) is rejected unconditionally, even when PL > budget.
      PlacementMove DeepRemat;
      DeepRemat.ValId = 8;
      DeepRemat.Owner = PlacementOwner::PressureShrink;
      DeepRemat.SrcDepth = 0;
      DeepRemat.DstDepths = {1, 1};
      DeepRemat.PlBefore = 240;
      DeepRemat.PlAfter = 236;
      DeepRemat.Budget = 180;
      if (Store.TryAccept(DeepRemat)) {
        std::fprintf(stderr, "Phase C1 FAIL: deeper-loop remat accepted\n");
        return false;
      }
      // 4. Cloning at normal pressure (PL <= budget) increases mu -> rejected.
      PlacementMove NormalClone;
      NormalClone.ValId = 9;
      NormalClone.Owner = PlacementOwner::PressureShrink;
      NormalClone.SrcDepth = 0;
      NormalClone.DstDepths = {0, 0};
      NormalClone.PlBefore = 150;
      NormalClone.PlAfter = 146;
      NormalClone.Budget = 180;
      if (Store.TryAccept(NormalClone)) {
        std::fprintf(stderr, "Phase C1 FAIL: normal-regime mu-increasing clone accepted\n");
        return false;
      }
      // 5. Same-depth remat at high pressure (PL > budget) strictly reduces
      //    pressure excess -> accepted.
      PlacementMove ValidRemat = NormalClone;
      ValidRemat.PlBefore = 220;
      ValidRemat.PlAfter = 216;
      if (!Store.TryAccept(ValidRemat)) {
        std::fprintf(stderr, "Phase C1 FAIL: valid high-pressure remat rejected\n");
        return false;
      }
    }
    // Phase C2 W-grade witness: SLP must NOT pack scalar MULs whose consumers
    // form a dot redex (even alongside an unrelated 4th MUL), allowing
    // SelectDots to collapse them directly into DP3 without undo/repack.
    {
      MirProgram DMir;
      SsaProgram DSsa(DMir);
      DSsa.Blocks.resize(1);
      auto &B = DSsa.Blocks[0];
      B.Kind = FlowKind::Return;
      const Operand NullOp{OperandKind::Null};
      auto InLane = [](uint32_t Reg, uint8_t Lane) {
        return Operand{OperandKind::Input, Reg, 0, 1u, BroadcastLaneSwizzle(Lane)};
      };
      const uint32_t M0 = DSsa.AllocValue();
      const uint32_t M1 = DSsa.AllocValue();
      const uint32_t M2 = DSsa.AllocValue();
      const uint32_t M3 = DSsa.AllocValue();
      const uint32_t S1 = DSsa.AllocValue();
      const uint32_t S2 = DSsa.AllocValue();
      B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M0), NullOp, {InLane(0, 0), InLane(1, 0)}});
      B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M1), NullOp, {InLane(0, 1), InLane(1, 1)}});
      B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M2), NullOp, {InLane(0, 2), InLane(1, 2)}});
      B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(M3), NullOp, {InLane(0, 3), InLane(1, 3)}});
      B.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(S1), NullOp, {MakeSsaTemp(M0), MakeSsaTemp(M1)}});
      B.Insts.push_back({D3D10_SB_OPCODE_ADD, MakeSsaTemp(S2), NullOp, {MakeSsaTemp(S1), MakeSsaTemp(M2)}});
      B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0}, NullOp, {MakeSsaTemp(S2)}});
      B.Insts.push_back({D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 8u, 0}, NullOp, {MakeSsaTemp(M3)}});
      if (RunOptSlpVectorize(DSsa)) {
        std::fprintf(stderr, "Phase C2 witness FAIL: SLP packed dot-redex MULs!\n");
        return false;
      }
      if (!RunOptSelectDots(DSsa)) {
        std::fprintf(stderr, "Phase C2 witness FAIL: SelectDots did not form DP3!\n");
        return false;
      }
      unsigned Dp3 = 0, Mul = 0;
      for (const auto &I : B.Insts) {
        if (!I.Dead && I.Opcode == D3D10_SB_OPCODE_DP3) ++Dp3;
        if (!I.Dead && I.Opcode == D3D10_SB_OPCODE_MUL) ++Mul;
      }
      if (Dp3 != 1 || Mul != 1) {
        std::fprintf(stderr, "Phase C2 witness FAIL: expected 1 DP3 + 1 MUL, got %u/%u\n", Dp3, Mul);
        return false;
      }
    }
    // Phase D1 W-grade witness: single-block shader with LiveInScalars == 0
    // but intra-block exact PL > 96 (28 vec4 live values = 112 lanes). Old
    // OptPressureShrink skipped it entirely; exact-PL triggers intra-block sink
    // and strictly reduces exact PL measured via RegAlloc/Liveness!
    {
      MirProgram PMir;
      SsaProgram PSsa(PMir);
      PSsa.Blocks.resize(1);
      auto &B = PSsa.Blocks[0];
      B.Kind = FlowKind::Return;
      const Operand NullOp{OperandKind::Null};
      const Operand In0{OperandKind::Input, 0, 0, 0xfu, kIdentitySwizzle};
      std::vector<uint32_t> Vals;
      for (unsigned K = 0; K < 28; ++K) {
        const uint32_t V = PSsa.AllocValue();
        Vals.push_back(V);
        B.Insts.push_back({D3D10_SB_OPCODE_ADD,
                           MakeSsaTemp(V, 0xfu, kIdentitySwizzle), NullOp,
                           {In0, MakeImm(0x3f800000u + K)}});
      }
      uint32_t Acc = Vals[0];
      for (unsigned K = 1; K < 28; ++K) {
        const uint32_t Nxt = PSsa.AllocValue();
        B.Insts.push_back({D3D10_SB_OPCODE_ADD,
                           MakeSsaTemp(Nxt, 0xfu, kIdentitySwizzle), NullOp,
                           {MakeSsaTemp(Acc, 0xfu, kIdentitySwizzle),
                            MakeSsaTemp(Vals[K], 0xfu, kIdentitySwizzle)}});
        Acc = Nxt;
      }
      B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                         Operand{OperandKind::Output, 0, 0, 0xfu, kIdentitySwizzle},
                         NullOp, {MakeSsaTemp(Acc, 0xfu, kIdentitySwizzle)}});
      std::vector<FlowBlock> FlowBefore;
      LowerOutOfSsa(PSsa, FlowBefore);
      const uint32_t PlBefore = ComputeExactPeakLanes(FlowBefore, PMir.TempCount);

      SsaDomTree PDom;
      PSsa.ComputeDominators(PDom);
      if (!RunOptPressureShrink(PSsa, PDom)) {
        std::fprintf(stderr, "Phase D1 witness FAIL: exact-PL OptPressureShrink did not trigger on 1-block high-PL shader (PL=%u)\n", PlBefore);
        return false;
      }
      std::vector<FlowBlock> FlowAfter;
      LowerOutOfSsa(PSsa, FlowAfter);
      const uint32_t PlAfter = ComputeExactPeakLanes(FlowAfter, PMir.TempCount);
      if (PlAfter >= PlBefore) {
        std::fprintf(stderr, "Phase D1 witness FAIL: PL did not strictly drop (%u -> %u)\n",
                     PlBefore, PlAfter);
        return false;
      }
    }
    // Audit #5 pin: 4-element IndexableTemp with 2 dynamic reads promotes to
    // SSA (0 x#), while 3 dynamic reads stays as x# (1 x#) to prevent code bloat.
    for (unsigned DynReads : {2u, 3u}) {
      MirProgram PMir;
      PMir.IndexableTemps = {{0u, 4u, 1u}};
      SsaProgram PSsa(PMir);
      PSsa.Blocks.resize(1);
      auto &B = PSsa.Blocks[0];
      B.Kind = FlowKind::Return;
      const Operand NullOp{OperandKind::Null};
      for (uint32_t E = 0; E < 4; ++E)
        B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                           Operand{OperandKind::IndexableTemp, 0, E, 1u, 0},
                           NullOp, {MakeImm(0x3f800000u + E)}});
      for (unsigned R = 0; R < DynReads; ++R) {
        const uint32_t Idx = PSsa.AllocValue();
        const uint32_t Val = PSsa.AllocValue();
        B.Insts.push_back({D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Idx), NullOp,
                           {Operand{OperandKind::Input, R, 0, 1u, 0}}});
        Operand Dyn{OperandKind::IndexableTemp, 0, 0, 1u, 0};
        Dyn.RelativeRegister = Idx;
        B.Insts.push_back({D3D10_SB_OPCODE_MOV, MakeSsaTemp(Val), NullOp, {Dyn}});
        B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                           Operand{OperandKind::Output, R, 0, 1u, 0}, NullOp,
                           {MakeSsaTemp(Val)}});
      }
      OptimizeSsaMir(PSsa);
      const size_t ExpectX = (DynReads <= 2u) ? 0u : 1u;
      if (PMir.IndexableTemps.size() != ExpectX) {
        std::fprintf(stderr, "Audit #5 FAIL: N=4 DynReads=%u expected %zu x#, got %zu\n",
                     DynReads, ExpectX, PMir.IndexableTemps.size());
        return false;
      }
    }
    // Audit #10 negative C2 test: 3 unrelated scalar MULs (not feeding a dot
    // redex) must NOT be over-blocked by FindDotRedexMuls and must still pack.
    {
      MirProgram NMir;
      SsaProgram NSsa(NMir);
      NSsa.Blocks.resize(1);
      auto &B = NSsa.Blocks[0];
      B.Kind = FlowKind::Return;
      const Operand NullOp{OperandKind::Null};
      auto InLane = [](uint32_t Reg, uint8_t Lane) {
        return Operand{OperandKind::Input, Reg, 0, 1u, BroadcastLaneSwizzle(Lane)};
      };
      uint32_t MIds[3] = {};
      for (uint8_t L = 0; L < 3; ++L) {
        MIds[L] = NSsa.AllocValue();
        B.Insts.push_back({D3D10_SB_OPCODE_MUL, MakeSsaTemp(MIds[L]), NullOp,
                           {InLane(0, L), InLane(1, L)}});
      }
      for (uint8_t L = 0; L < 3; ++L) {
        B.Insts.push_back({D3D10_SB_OPCODE_MOV,
                           Operand{OperandKind::Output, 0, 0,
                                   static_cast<uint8_t>(1u << L), 0},
                           NullOp, {MakeSsaTemp(MIds[L])}});
      }
      if (!RunOptSlpVectorize(NSsa)) {
        std::fprintf(stderr, "Audit #10 FAIL: SLP over-blocked unrelated scalar MULs\n");
        return false;
      }
      unsigned VecMul = 0;
      for (const auto &I : B.Insts)
        if (!I.Dead && I.Opcode == D3D10_SB_OPCODE_MUL && PopcountMask(I.Dest.Mask) == 3)
          ++VecMul;
      if (VecMul != 1) {
        std::fprintf(stderr, "Audit #10 FAIL: expected 1 packed vec3 MUL, got %u\n", VecMul);
        return false;
      }
    }
    // Audit #29/#34: Passes.def rejects unknown and non-disableable pass names.
    if (SetDisabledOptimizationPasses("bogus-pass-name") ||
        SetDisabledOptimizationPasses("cfg-cleanup") ||
        !SetDisabledOptimizationPasses("dp2,loop-peel,guard-discard") ||
        !SetDisabledOptimizationPasses("")) {
      std::fprintf(stderr, "Audit #29 FAIL: pass disable validation mismatch\n");
      return false;
    }
  }
  return true;
}

bool TestGuardUniformDiscard() {
  const Operand NullOp{OperandKind::Null};
  auto MakeCb = [](uint32_t Row, uint8_t Lane) {
    return Operand{OperandKind::ConstantBuffer, 0, Row, 1u,
                   BroadcastLaneSwizzle(Lane)};
  };
  const Operand In0{OperandKind::Input, 0, 0, 1u, 0};

  auto BuildOneSidedDiscard = [&](MirProgram &Mir, SsaProgram &Ssa, int ChainLen,
                                  bool UseRawCb, bool AddJoinPhi, bool TwoSidedElse) {
    Mir.ShaderStage = Stage::Pixel;
    Ssa.Blocks.resize(TwoSidedElse ? 4 : 3);
    Operand UniformOp;
    uint32_t EqVec = 0;
    if (UseRawCb) {
      UniformOp = MakeCb(0, 0);
    } else {
      EqVec = Ssa.AllocValue();
      Operand CbRow{OperandKind::ConstantBuffer, 0, 154, 0xfu, 0x54u};
      Ssa.Blocks[0].Insts.push_back(
          {D3D10_SB_OPCODE_EQ, MakeSsaTemp(EqVec, 0xfu, kIdentitySwizzle),
           NullOp, {CbRow, MakeImm(0x3f800000u)}});
      UniformOp = MakeSsaLane(EqVec, 0);
    }

    uint32_t Cur = Ssa.AllocValue();
    if (EqVec) {
      Ssa.Blocks[0].Insts.push_back(
          {D3D10_SB_OPCODE_AND, MakeSsaTemp(Cur), NullOp,
           {MakeSsaLane(EqVec, 1), MakeImm(0x3f800000u)}});
    } else {
      Ssa.Blocks[0].Insts.push_back(
          {D3D10_SB_OPCODE_FTOU, MakeSsaTemp(Cur), NullOp, {In0}});
    }
    for (int K = 0; K < ChainLen; ++K) {
      const uint32_t Next = Ssa.AllocValue();
      Ssa.Blocks[0].Insts.push_back(
          {D3D10_SB_OPCODE_ADD, MakeSsaTemp(Next), NullOp, {MakeSsaTemp(Cur), In0}});
      Cur = Next;
    }
    const uint32_t PixelPred = Ssa.AllocValue();
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_LT, MakeSsaTemp(PixelPred), NullOp,
         {MakeImm(0x3a83126fu), MakeSsaTemp(Cur)}});
    const uint32_t Combined = Ssa.AllocValue();
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_AND, MakeSsaTemp(Combined), NullOp,
         {UniformOp, MakeSsaTemp(PixelPred)}});
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeSsaTemp(Combined);
    Ssa.Blocks[0].Successors = {1, 2};

    SsaInst Disc{D3D10_SB_OPCODE_DISCARD, NullOp, NullOp, {MakeImm(0xffffffffu)}};
    Disc.TestNonZero = true;
    Ssa.Blocks[1].Insts.push_back(Disc);
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {TwoSidedElse ? 3u : 2u};

    if (TwoSidedElse) {
      Ssa.Blocks[2].Kind = FlowKind::Jump;
      Ssa.Blocks[2].Successors = {3};
      Ssa.Blocks[3].Kind = FlowKind::Return;
    } else {
      if (AddJoinPhi) {
        SsaPhi JoinPhi;
        JoinPhi.Result = Ssa.AllocValue();
        JoinPhi.Mask = 1u;
        JoinPhi.Inputs.push_back({0, In0});
        JoinPhi.Inputs.push_back({1, MakeImm(0u)});
        Ssa.Blocks[2].Phis.push_back(JoinPhi);
      }
      Ssa.Blocks[2].Kind = FlowKind::Return;
      Ssa.Blocks[2].Insts.push_back(
          {D3D10_SB_OPCODE_MOV, Operand{OperandKind::Output, 0, 0, 1u, 0},
           NullOp, {In0}});
    }
  };

  // 1. Positive case (lilToon UDIM shape): GuardDiscard splits the one-sided branch
  //    and RunOptColdSink sinks the 11-inst pixel cone while keeping EqVec in bb0.
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    BuildOneSidedDiscard(Mir, Ssa, /*ChainLen=*/9, /*UseRawCb=*/false,
                         /*AddJoinPhi=*/false, /*TwoSidedElse=*/false);
    SsaDomTree Dom;
    std::string Err;
    if (!RunOptGuardDiscard(Ssa) || !Ssa.Verify(Err)) {
      std::fprintf(stderr, "GuardDiscard positive split failed: %s\n", Err.c_str());
      return false;
    }
    Ssa.ComputeDominators(Dom);
    if (!RunOptColdSink(Ssa, Dom) || !Ssa.Verify(Err)) {
      std::fprintf(stderr, "GuardDiscard + ColdSink failed: %s\n", Err.c_str());
      return false;
    }
    if (Ssa.Blocks.size() != 4 || !Ssa.Blocks[0].UniformBranch ||
        Ssa.Blocks[0].Insts.size() != 1 || Ssa.Blocks[3].Insts.size() != 11) {
      std::fprintf(stderr,
                   "GuardDiscard positive layout mismatch: blocks=%zu b0=%zu mid=%zu\n",
                   Ssa.Blocks.size(), Ssa.Blocks[0].Insts.size(),
                   Ssa.Blocks.size() > 3 ? Ssa.Blocks[3].Insts.size() : 0);
      return false;
    }
  }

  // 2. Cost model: tiny 2-inst pixel cone must NOT split.
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    BuildOneSidedDiscard(Mir, Ssa, /*ChainLen=*/0, /*UseRawCb=*/false,
                         /*AddJoinPhi=*/false, /*TwoSidedElse=*/false);
    if (RunOptGuardDiscard(Ssa) || Ssa.Blocks.size() != 3) {
      std::fprintf(stderr, "GuardDiscard should not split tiny cone\n");
      return false;
    }
  }

  // 3. Soundness: raw non-boolean uniform cb0[0].x must NOT split.
  {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    BuildOneSidedDiscard(Mir, Ssa, /*ChainLen=*/9, /*UseRawCb=*/true,
                         /*AddJoinPhi=*/false, /*TwoSidedElse=*/false);
    if (RunOptGuardDiscard(Ssa) || Ssa.Blocks.size() != 3) {
      std::fprintf(stderr, "GuardDiscard soundness bug: split non-boolean mask\n");
      return false;
    }
  }

  // 4. Safety: join block with live PHIs or two-sided if/else must NOT split.
  for (bool TwoSided : {false, true}) {
    MirProgram Mir;
    SsaProgram Ssa(Mir);
    BuildOneSidedDiscard(Mir, Ssa, /*ChainLen=*/9, /*UseRawCb=*/false,
                         /*AddJoinPhi=*/!TwoSided, /*TwoSidedElse=*/TwoSided);
    if (RunOptGuardDiscard(Ssa)) {
      std::fprintf(stderr, "GuardDiscard should not split %s\n",
                   TwoSided ? "two-sided if/else" : "join with live PHIs");
      return false;
    }
  }

  // 5. All-uniform AND(uniformA, heavyUniformConeB) must NOT split.
  {
    MirProgram Mir;
    Mir.ShaderStage = Stage::Vertex;
    SsaProgram Ssa(Mir);
    Ssa.Blocks.resize(3);
    const uint32_t U = Ssa.AllocValue();
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_EQ, MakeSsaTemp(U), NullOp,
         {MakeCb(0, 0), MakeImm(0x3f800000u)}});
    uint32_t Cur = Ssa.AllocValue();
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_ADD, MakeSsaTemp(Cur), NullOp,
         {MakeCb(1, 0), MakeCb(1, 1)}});
    for (int K = 0; K < 10; ++K) {
      const uint32_t Next = Ssa.AllocValue();
      Ssa.Blocks[0].Insts.push_back(
          {D3D10_SB_OPCODE_ADD, MakeSsaTemp(Next), NullOp,
           {MakeSsaTemp(Cur), MakeCb(2, 0)}});
      Cur = Next;
    }
    const uint32_t UnifPred = Ssa.AllocValue();
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_LT, MakeSsaTemp(UnifPred), NullOp,
         {MakeImm(0u), MakeSsaTemp(Cur)}});
    const uint32_t Combined = Ssa.AllocValue();
    Ssa.Blocks[0].Insts.push_back(
        {D3D10_SB_OPCODE_AND, MakeSsaTemp(Combined), NullOp,
         {MakeSsaTemp(U), MakeSsaTemp(UnifPred)}});
    Ssa.Blocks[0].Kind = FlowKind::Branch;
    Ssa.Blocks[0].Condition = MakeSsaTemp(Combined);
    Ssa.Blocks[0].Successors = {1, 2};
    Ssa.Blocks[1].Kind = FlowKind::Jump;
    Ssa.Blocks[1].Successors = {2};
    Ssa.Blocks[2].Kind = FlowKind::Return;
    if (RunOptGuardDiscard(Ssa) || Ssa.Blocks.size() != 3) {
      std::fprintf(stderr, "GuardDiscard must not split all-uniform AND chains\n");
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  struct TestCase {
    const char *Name;
    bool (*Fn)();
  };
  const TestCase Cases[] = {
      {"TestCrossBlockModifierAndGvn", TestCrossBlockModifierAndGvn},
      {"TestFSubMadAndSlpPack", TestFSubMadAndSlpPack},
      {"TestNotPolarityAndSinCosMerge", TestNotPolarityAndSinCosMerge},
      {"TestIfConvertAndMovcModifiers", TestIfConvertAndMovcModifiers},
      {"TestAlgebraicIdentitiesAndFolds", TestAlgebraicIdentitiesAndFolds},
      {"TestDualDestGvnAndMovcNarrow", TestDualDestGvnAndMovcNarrow},
      {"TestPromoteLocalArraysUdivAndDp2", TestPromoteLocalArraysUdivAndDp2},
      {"TestOptimizeIndexableTemps", TestOptimizeIndexableTemps},
      {"TestIndexablePack", TestIndexablePack},
      {"TestCorrectnessRegressions", TestCorrectnessRegressions},
      {"TestIndexableCopyAndSsaPromotion", TestIndexableCopyAndSsaPromotion},
      {"TestRound2Regressions", TestRound2Regressions},
      {"TestBatch2Optimizations", TestBatch2Optimizations},
      {"TestVsOptimizations", TestVsOptimizations},
      {"TestRound3RegressionsAndPerf", TestRound3RegressionsAndPerf},
      {"TestWriteOnceDedupLanePackAndUnitSat", TestWriteOnceDedupLanePackAndUnitSat},
      {"TestPhase2ColdSinkStrictBranchAndPow", TestPhase2ColdSinkStrictBranchAndPow},
      {"TestAuditBugRegressions", TestAuditBugRegressions},
      {"TestCodexAuditOptimizations", TestCodexAuditOptimizations},
      {"TestOpcodeSpecEffectContracts", TestOpcodeSpecEffectContracts},
      {"TestLoopWideningAndBudgetFallback", TestLoopWideningAndBudgetFallback},
      {"TestAuditFixesH2M1M2H3", TestAuditFixesH2M1M2H3},
      {"TestGuardUniformDiscard", TestGuardUniformDiscard},
  };
  for (const TestCase &Case : Cases) {
    if (!Case.Fn()) {
      std::fprintf(stderr, "SsaMirOptTest failed in %s\n", Case.Name);
      return 1;
    }
  }
  return 0;
}
