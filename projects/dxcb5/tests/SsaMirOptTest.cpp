// Distributed under the University of Illinois Open Source License.
#include "Mir/OptPipeline.h"
#include "Mir/OutOfSsa.h"
#include "Mir/SsaMir.h"
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
  SsaProgram Ssa(Mir);
  Ssa.Blocks.resize(2);
  Ssa.LocalArrayLengths = {4u, 4u};

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
  for (uint8_t L = 0; L < 4; ++L) {
    SsaInst W0;
    W0.Opcode = PseudoArrayWrite;
    W0.ArrayId = 0;
    W0.ArrayLane = L;
    W0.Sources = {LaneOf(Vecs[L], 0)};
    B0.Insts.push_back(std::move(W0));

    SsaInst W1;
    W1.Opcode = PseudoArrayWrite;
    W1.ArrayId = 1;
    W1.ArrayLane = L;
    W1.Sources = {LaneOf(Vecs[L], 1)};
    B0.Insts.push_back(std::move(W1));
  }

  auto &B1 = Ssa.Blocks[1];
  B1.Kind = FlowKind::Return;
  const uint32_t Rx = Ssa.AllocValue();
  const uint32_t Ry = Ssa.AllocValue();
  SsaInst R0;
  R0.Opcode = PseudoArrayRead;
  R0.Dest = MakeSsaTemp(Rx);
  R0.ArrayId = 0;
  R0.ArrayLane = 0xffu;
  R0.Sources = {Sel};
  B1.Insts.push_back(std::move(R0));

  SsaInst R1;
  R1.Opcode = PseudoArrayRead;
  R1.Dest = MakeSsaTemp(Ry);
  R1.ArrayId = 1;
  R1.ArrayLane = 0xffu;
  R1.Sources = {Sel};
  B1.Insts.push_back(std::move(R1));

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
      if (I.Opcode == PseudoArrayRead || I.Opcode == PseudoArrayWrite)
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

} // namespace

int main() {
  if (!TestCrossBlockModifierAndGvn())
    return 1;
  if (!TestFSubMadAndSlpPack())
    return 1;
  if (!TestNotPolarityAndSinCosMerge())
    return 1;
  if (!TestIfConvertAndMovcModifiers())
    return 1;
  if (!TestAlgebraicIdentitiesAndFolds())
    return 1;
  if (!TestDualDestGvnAndMovcNarrow())
    return 1;
  if (!TestPromoteLocalArraysUdivAndDp2())
    return 1;
  if (!TestOptimizeIndexableTemps())
    return 1;
  if (!TestIndexablePack())
    return 1;
  if (!TestCorrectnessRegressions())
    return 1;
  if (!TestIndexableCopyAndSsaPromotion())
    return 1;
  if (!TestRound2Regressions())
    return 1;
  if (!TestBatch2Optimizations())
    return 1;
  if (!TestVsOptimizations())
    return 1;
  if (!TestRound3RegressionsAndPerf())
    return 1;
  return 0;
}
