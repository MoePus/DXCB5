// Distributed under the University of Illinois Open Source License.
#include "Frontend/LowerToSsaMir.h"
#include "dxc/HLSL/Dxcb5Contract.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include <algorithm>

using namespace dxcb5;
using namespace llvm;

namespace {
using I = llvm::Instruction;
constexpr unsigned kUnsupportedOpcode = UINT32_MAX;
constexpr uint32_t kPositiveOneBits = 0x3f800000u;
constexpr uint32_t kNegativeOneBits = 0xbf800000u;

unsigned ValueWidth(const Type *Ty) {
  if (const auto *VT = dyn_cast<VectorType>(Ty))
    return VT->getNumElements();
  return 1;
}

unsigned BinaryOpcode(unsigned Opcode) {
  switch (Opcode) {
  case I::FAdd: return D3D10_SB_OPCODE_ADD;
  case I::FMul: return D3D10_SB_OPCODE_MUL;
  case I::FDiv: return D3D10_SB_OPCODE_DIV;
  case I::FSub: return D3D10_SB_OPCODE_ADD;
  case I::Add:
  case I::Sub: return D3D10_SB_OPCODE_IADD;
  case I::Mul: return D3D10_SB_OPCODE_IMUL;
  case I::UDiv:
  case I::URem: return D3D10_SB_OPCODE_UDIV;
  case I::And: return D3D10_SB_OPCODE_AND;
  case I::Or: return D3D10_SB_OPCODE_OR;
  case I::Xor: return D3D10_SB_OPCODE_XOR;
  case I::Shl: return D3D10_SB_OPCODE_ISHL;
  case I::LShr: return D3D10_SB_OPCODE_USHR;
  case I::AShr: return D3D10_SB_OPCODE_ISHR;
  default: return kUnsupportedOpcode;
  }
}

unsigned CastOpcode(unsigned Opcode) {
  switch (Opcode) {
  case I::BitCast:
  case I::FPExt:
  case I::FPTrunc:
  case I::SExt:
  case I::ZExt:
  case I::Trunc: return D3D10_SB_OPCODE_MOV;
  case I::SIToFP: return D3D10_SB_OPCODE_ITOF;
  case I::UIToFP: return D3D10_SB_OPCODE_UTOF;
  case I::FPToSI: return D3D10_SB_OPCODE_FTOI;
  case I::FPToUI: return D3D10_SB_OPCODE_FTOU;
  default: return kUnsupportedOpcode;
  }
}

bool IsZeroFloatConst(const Value *V, bool AllowPositiveZero) {
  if (const auto *FP = dyn_cast<ConstantFP>(V)) {
    if ((!FP->getType()->isFloatTy() && !FP->getType()->isHalfTy()) ||
        !FP->getValueAPF().isZero())
      return false;
    return AllowPositiveZero || FP->getValueAPF().isNegative();
  }
  const auto *CV = dyn_cast<Constant>(V);
  const auto *VT = CV ? dyn_cast<VectorType>(CV->getType()) : nullptr;
  if (!VT || (!VT->getElementType()->isFloatTy() &&
              !VT->getElementType()->isHalfTy()))
    return false;
  for (unsigned L = 0; L < VT->getNumElements(); ++L)
    if (!IsZeroFloatConst(CV->getAggregateElement(L), AllowPositiveZero))
      return false;
  return true;
}

bool EmitOrderedUnorderedCompare(SsaLowerer &L, CmpInst::Predicate Pred,
                                 Operand Left, Operand Right, Operand Dest,
                                 bool Precise) {
  const auto First = L.AllocTemp(Dest.Mask);
  const auto Second = L.AllocTemp(Dest.Mask);
  const bool NeedsNot =
      (Pred == CmpInst::FCMP_UNO || Pred == CmpInst::FCMP_UEQ);
  const auto Mid = NeedsNot ? L.AllocTemp(Dest.Mask) : Dest;
  if (Pred == CmpInst::FCMP_ORD || Pred == CmpInst::FCMP_UNO) {
    L.Emit(D3D10_SB_OPCODE_EQ, First, {Left, Left}, Precise);
    L.Emit(D3D10_SB_OPCODE_EQ, Second, {Right, Right}, Precise);
    L.Emit(D3D10_SB_OPCODE_AND, Mid, {First, Second}, Precise);
  } else {
    L.Emit(D3D10_SB_OPCODE_LT, First, {Left, Right}, Precise);
    L.Emit(D3D10_SB_OPCODE_LT, Second, {Right, Left}, Precise);
    L.Emit(D3D10_SB_OPCODE_OR, Mid, {First, Second}, Precise);
  }
  if (NeedsNot)
    L.Emit(D3D10_SB_OPCODE_NOT, Dest, {Mid}, Precise);
  return true;
}

bool EmitFloatCompare(SsaLowerer &L, const CmpInst &Inst, Operand Left,
                      Operand Right, Operand Dest) {
  const auto Pred = Inst.getPredicate();
  const bool Precise = L.Module.IsPrecise(&Inst);
  unsigned Opcode = 0;
  bool Invert = false;
  switch (Pred) {
  case CmpInst::FCMP_FALSE:
    L.Emit(D3D10_SB_OPCODE_MOV, Dest, {MakeImm(0)}, Precise);
    return true;
  case CmpInst::FCMP_TRUE:
    L.Emit(D3D10_SB_OPCODE_MOV, Dest, {MakeImm(UINT32_MAX)}, Precise);
    return true;
  case CmpInst::FCMP_OEQ: Opcode = D3D10_SB_OPCODE_EQ; break;
  case CmpInst::FCMP_UNE: Opcode = D3D10_SB_OPCODE_NE; break;
  case CmpInst::FCMP_OGT: std::swap(Left, Right); LLVM_FALLTHROUGH;
  case CmpInst::FCMP_OLT: Opcode = D3D10_SB_OPCODE_LT; break;
  case CmpInst::FCMP_OLE: std::swap(Left, Right); LLVM_FALLTHROUGH;
  case CmpInst::FCMP_OGE: Opcode = D3D10_SB_OPCODE_GE; break;
  case CmpInst::FCMP_ULE: std::swap(Left, Right); LLVM_FALLTHROUGH;
  case CmpInst::FCMP_UGE: Opcode = D3D10_SB_OPCODE_LT; Invert = true; break;
  case CmpInst::FCMP_UGT: std::swap(Left, Right); LLVM_FALLTHROUGH;
  case CmpInst::FCMP_ULT: Opcode = D3D10_SB_OPCODE_GE; Invert = true; break;
  default:
    return EmitOrderedUnorderedCompare(L, Pred, Left, Right, Dest, Precise);
  }
  if (Invert) {
    const auto Mid = L.AllocTemp(Dest.Mask);
    L.Emit(Opcode, Mid, {Left, Right}, Precise);
    L.Emit(D3D10_SB_OPCODE_NOT, Dest, {Mid}, Precise);
  } else {
    L.Emit(Opcode, Dest, {Left, Right}, Precise);
  }
  return true;
}

bool EmitCompare(SsaLowerer &L, CmpInst &Inst, Operand Dest) {
  const bool Precise = L.Module.IsPrecise(&Inst);
  if (isa<FCmpInst>(Inst))
    return EmitFloatCompare(L, Inst, L.Source(Inst.getOperand(0)),
                            L.Source(Inst.getOperand(1)), Dest);
  const auto Pred = Inst.getPredicate();
  const bool Signed = Pred == CmpInst::ICMP_SLT || Pred == CmpInst::ICMP_SLE ||
                      Pred == CmpInst::ICMP_SGT || Pred == CmpInst::ICMP_SGE;
  auto Left = L.SourceInt(Inst.getOperand(0), Signed, Precise);
  auto Right = L.SourceInt(Inst.getOperand(1), Signed, Precise);
  unsigned Opcode = 0;
  switch (Pred) {
  case CmpInst::ICMP_EQ: Opcode = D3D10_SB_OPCODE_IEQ; break;
  case CmpInst::ICMP_NE: Opcode = D3D10_SB_OPCODE_INE; break;
  case CmpInst::ICMP_SGT: std::swap(Left, Right); LLVM_FALLTHROUGH;
  case CmpInst::ICMP_SLT: Opcode = D3D10_SB_OPCODE_ILT; break;
  case CmpInst::ICMP_SLE: std::swap(Left, Right); LLVM_FALLTHROUGH;
  case CmpInst::ICMP_SGE: Opcode = D3D10_SB_OPCODE_IGE; break;
  case CmpInst::ICMP_UGT: std::swap(Left, Right); LLVM_FALLTHROUGH;
  case CmpInst::ICMP_ULT: Opcode = D3D10_SB_OPCODE_ULT; break;
  case CmpInst::ICMP_ULE: std::swap(Left, Right); LLVM_FALLTHROUGH;
  case CmpInst::ICMP_UGE: Opcode = D3D10_SB_OPCODE_UGE; break;
  default: return L.Fail("unsupported comparison predicate");
  }
  L.Emit(Opcode, Dest, {Left, Right}, Precise);
  return true;
}

bool LowerExtractElement(SsaLowerer &L, ExtractElementInst &EE, bool Precise) {
  if (isa<ConstantInt>(EE.getIndexOperand()))
    return true;
  const unsigned W = ValueWidth(EE.getVectorOperand()->getType());
  const auto Selector = L.SourceInt(EE.getIndexOperand(), false, Precise);
  const auto Dest = L.Values.lookup(&EE);
  auto Cur = L.AllocTemp(1);
  L.Emit(D3D10_SB_OPCODE_MOV, Cur, {L.SourceScalarLane(EE.getVectorOperand(), 0)},
         Precise);
  for (unsigned Lane = 1; Lane < W; ++Lane) {
    auto Match = L.AllocTemp(1);
    L.Emit(D3D10_SB_OPCODE_IEQ, Match, {Selector, MakeImm(Lane)}, Precise);
    auto Next = (Lane + 1 == W) ? Dest : L.AllocTemp(1);
    L.Emit(D3D10_SB_OPCODE_MOVC, Next,
           {Match, L.SourceScalarLane(EE.getVectorOperand(), Lane), Cur}, Precise);
    Cur = Next;
  }
  if (W == 1)
    L.Emit(D3D10_SB_OPCODE_MOV, Dest, {Cur}, Precise);
  return L.Error.empty();
}

bool LowerInsertElement(SsaLowerer &L, InsertElementInst &IE, bool Precise) {
  const auto *Idx = dyn_cast<ConstantInt>(IE.getOperand(2));
  if (!Idx)
    return L.Fail("dynamic insertelement index is unsupported in SM5");
  const unsigned W = ValueWidth(IE.getType());
  const unsigned PutLane = static_cast<unsigned>(Idx->getZExtValue());
  if (PutLane >= W)
    return L.Fail("insertelement index out of bounds");
  SmallVector<Operand, 4> Lanes;
  Lanes.reserve(W);
  for (unsigned Lane = 0; Lane < W; ++Lane) {
    if (Lane == PutLane)
      Lanes.push_back(L.Source(IE.getOperand(1)));
    else if (isa<UndefValue>(IE.getOperand(0)))
      Lanes.push_back(MakeImm(0));
    else
      Lanes.push_back(L.SourceScalarLane(IE.getOperand(0), Lane));
  }
  L.Emit(PseudoCombineVec, L.Values.lookup(&IE), Lanes, Precise);
  return L.Error.empty();
}

bool LowerShuffleVector(SsaLowerer &L, ShuffleVectorInst &SV, bool Precise) {
  if (!L.EmittedShuffles.count(&SV))
    return true;
  const unsigned OutW = ValueWidth(SV.getType());
  const unsigned InW = ValueWidth(SV.getOperand(0)->getType());
  const auto *V0 = SV.getOperand(0);
  const auto *V1 = SV.getOperand(1);
  SmallVector<Operand, 4> Lanes;
  Lanes.reserve(OutW);
  for (unsigned Lane = 0; Lane < OutW; ++Lane) {
    const int M = SV.getMaskValue(Lane);
    if (M < 0)
      Lanes.push_back(MakeImm(0));
    else if (static_cast<unsigned>(M) < InW)
      Lanes.push_back(L.SourceScalarLane(V0, static_cast<unsigned>(M)));
    else
      Lanes.push_back(L.SourceScalarLane(V1, static_cast<unsigned>(M) - InW));
  }
  L.Emit(PseudoCombineVec, L.Values.lookup(&SV), Lanes, Precise);
  return L.Error.empty();
}

bool LowerBinaryInst(SsaLowerer &L, llvm::Instruction &Inst, unsigned BinOp,
                     Operand Dest, bool Precise) {
  const unsigned Opc = Inst.getOpcode();
  Operand Left = Opc == I::AShr
                     ? L.SourceInt(Inst.getOperand(0), true, Precise)
                     : ((Opc == I::LShr || Opc == I::UDiv || Opc == I::URem)
                            ? L.SourceInt(Inst.getOperand(0), false, Precise)
                            : L.Source(Inst.getOperand(0)));
  Operand Right = (Opc == I::AShr || Opc == I::LShr || Opc == I::UDiv ||
                   Opc == I::URem)
                      ? L.SourceInt(Inst.getOperand(1), false, Precise)
                      : L.Source(Inst.getOperand(1));
  if (Opc == I::Sub) {
    auto Neg = L.AllocTemp(Dest.Mask);
    L.Emit(D3D10_SB_OPCODE_INEG, Neg, {Right}, Precise);
    Right = Neg;
  } else if (Opc == I::FSub) {
    Right = NegateMirOperand(L.Ssa, Right);
    const bool FastZero = Inst.hasUnsafeAlgebra() && !Precise;
    if (IsZeroFloatConst(Inst.getOperand(0), FastZero)) {
      L.Emit(D3D10_SB_OPCODE_MOV, Dest, {Right}, Precise);
      return L.Error.empty();
    }
  }
  if (BinOp == D3D10_SB_OPCODE_IMUL || Opc == I::URem) {
    SsaInst S;
    S.Opcode = BinOp;
    S.Dest = Operand();
    S.AuxDest = Dest;
    S.Sources = {Left, Right};
    S.Precise = Precise;
    L.CurrentBlock->Insts.push_back(std::move(S));
  } else if (BinOp == D3D10_SB_OPCODE_UDIV) {
    SsaInst S;
    S.Opcode = BinOp;
    S.Dest = Dest;
    S.AuxDest = Operand();
    S.Sources = {Left, Right};
    S.Precise = Precise;
    L.CurrentBlock->Insts.push_back(std::move(S));
  } else {
    L.Emit(BinOp, Dest, {Left, Right}, Precise);
  }
  return L.Error.empty();
}

} // namespace

bool SsaLowerer::LowerInstruction(llvm::Instruction &Inst) {
  const bool FastMath =
      Inst.getMetadata(llvm::dxcb5_contract::kFastMathMD) != nullptr;
  const size_t StartIdx = CurrentBlock->Insts.size();
  struct FastMathTagGuard {
    SsaBlock *Blk;
    size_t Start;
    bool Enable;
    ~FastMathTagGuard() {
      if (!Enable)
        return;
      for (size_t K = Start; K < Blk->Insts.size(); ++K)
        Blk->Insts[K].FastMath = true;
    }
  } Guard{CurrentBlock, StartIdx, FastMath};

  if (auto *Call = dyn_cast<CallInst>(&Inst))
    return LowerCall(*Call);
  if (auto *Alloca = dyn_cast<AllocaInst>(&Inst))
    return LowerAlloca(*Alloca);
  if (auto *Load = dyn_cast<LoadInst>(&Inst))
    return LowerLoad(*Load);
  if (auto *Store = dyn_cast<StoreInst>(&Inst))
    return LowerStore(*Store);
  if (isa<PHINode>(Inst) || isa<GetElementPtrInst>(Inst) ||
      isa<ExtractValueInst>(Inst) || Inst.getType()->isPointerTy() ||
      Inst.isTerminator())
    return true;

  const bool Precise = Module.IsPrecise(&Inst);
  if (auto *EE = dyn_cast<ExtractElementInst>(&Inst))
    return LowerExtractElement(*this, *EE, Precise);
  if (auto *IE = dyn_cast<InsertElementInst>(&Inst))
    return LowerInsertElement(*this, *IE, Precise);
  if (auto *SV = dyn_cast<ShuffleVectorInst>(&Inst))
    return LowerShuffleVector(*this, *SV, Precise);

  auto SignIt = SignPatterns.find(&Inst);
  if (SignIt != SignPatterns.end()) {
    const auto Dest = Values.lookup(&Inst);
    const auto Neg = AllocTemp(Dest.Mask);
    Emit(D3D10_SB_OPCODE_MOVC, Neg,
         {Source(SignIt->second.LessFlag), MakeImm(kNegativeOneBits), MakeImm(0)},
         Precise);
    Emit(D3D10_SB_OPCODE_MOVC, Dest,
         {Source(SignIt->second.GreaterFlag), MakeImm(kPositiveOneBits), Neg},
         Precise);
    return Error.empty();
  }

  const auto Dest = Values.lookup(&Inst);
  if (auto *Cmp = dyn_cast<CmpInst>(&Inst))
    return EmitCompare(*this, *Cmp, Dest);
  if (auto *Sel = dyn_cast<SelectInst>(&Inst)) {
    Emit(D3D10_SB_OPCODE_MOVC, Dest,
         {Source(Sel->getCondition()), Source(Sel->getTrueValue()),
          Source(Sel->getFalseValue())},
         Precise);
    return Error.empty();
  }
  if (Inst.getOpcode() == I::SDiv || Inst.getOpcode() == I::SRem)
    return LowerSignedDivRem(Inst, Dest, Precise);
  if (Inst.getOpcode() == I::FRem) {
    const Operand A = Source(Inst.getOperand(0));
    const Operand B = Source(Inst.getOperand(1));
    const auto Q = AllocTemp(Dest.Mask);
    const auto T = AllocTemp(Dest.Mask);
    Emit(D3D10_SB_OPCODE_DIV, Q, {A, B}, Precise);
    Emit(D3D10_SB_OPCODE_ROUND_Z, T, {Q}, Precise);
    Emit(D3D10_SB_OPCODE_MAD, Dest, {NegateMirOperand(Ssa, T), B, A}, Precise);
    return Error.empty();
  }
  if (const unsigned BinOp = BinaryOpcode(Inst.getOpcode());
      BinOp != kUnsupportedOpcode)
    return LowerBinaryInst(*this, Inst, BinOp, Dest, Precise);
  if (Inst.getOpcode() == I::ZExt &&
      Inst.getOperand(0)->getType()->getScalarType()->isIntegerTy(1))
    return Emit(D3D10_SB_OPCODE_AND, Dest, {Source(Inst.getOperand(0)), MakeImm(1)},
                Precise), Error.empty();
  if (Inst.getOpcode() == I::UIToFP &&
      Inst.getOperand(0)->getType()->getScalarType()->isIntegerTy(1))
    return Emit(D3D10_SB_OPCODE_AND, Dest,
                {Source(Inst.getOperand(0)), MakeImm(kPositiveOneBits)}, Precise),
           Error.empty();
  if (Inst.getOpcode() == I::Trunc &&
      Inst.getType()->getScalarType()->isIntegerTy(1)) {
    const auto LowBit = AllocTemp(Dest.Mask);
    Emit(D3D10_SB_OPCODE_AND, LowBit, {Source(Inst.getOperand(0)), MakeImm(1)},
         Precise);
    Emit(D3D10_SB_OPCODE_INE, Dest, {LowBit, MakeImm(0)}, Precise);
    return Error.empty();
  }
  if (Inst.getOpcode() == I::BitCast) {
    const Type *SrcFullTy = Inst.getOperand(0)->getType();
    const Type *DstFullTy = Inst.getType();
    if (ValueWidth(SrcFullTy) != ValueWidth(DstFullTy))
      return Fail("unsupported width-changing bitcast in SM5");
    const Type *STy = SrcFullTy->getScalarType();
    const Type *DTy = DstFullTy->getScalarType();
    if (STy->isHalfTy() && DTy->isIntegerTy(16))
      return Emit(D3D11_SB_OPCODE_F32TOF16, Dest, {Source(Inst.getOperand(0))},
                  Precise), Error.empty();
    if (STy->isIntegerTy(16) && DTy->isHalfTy())
      return Emit(D3D11_SB_OPCODE_F16TOF32, Dest,
                  {SourceInt(Inst.getOperand(0), false, Precise)}, Precise),
             Error.empty();
    if (STy->getPrimitiveSizeInBits() != DTy->getPrimitiveSizeInBits())
      return Fail("unsupported width-changing scalar bitcast in SM5");
  }
  if (const unsigned CastOp = CastOpcode(Inst.getOpcode());
      CastOp != kUnsupportedOpcode) {
    const unsigned Opc = Inst.getOpcode();
    const bool SignedCast = Opc == I::SExt || Opc == I::SIToFP;
    const bool UnsignedCast = Opc == I::ZExt || Opc == I::UIToFP;
    const Operand Src = (SignedCast || UnsignedCast)
                            ? SourceInt(Inst.getOperand(0), SignedCast, Precise)
                            : Source(Inst.getOperand(0));
    Emit(CastOp, Dest, {Src}, Precise);
    return Error.empty();
  }
  return Fail(Twine("unsupported LLVM instruction: ") + Inst.getOpcodeName());
}

bool SsaLowerer::LowerTerminator(BasicBlock &Block, SsaBlock &Out) {
  auto *Term = Block.getTerminator();
  if (isa<ReturnInst>(Term) || isa<UnreachableInst>(Term)) {
    Out.Kind = FlowKind::Return;
    return true;
  }
  if (auto *Branch = dyn_cast<BranchInst>(Term)) {
    Out.Kind = Branch->isConditional() ? FlowKind::Branch : FlowKind::Jump;
    Out.StrictBranch =
        Branch->isConditional() &&
        Branch->getMetadata(llvm::dxcb5_contract::kStrictBranchMD) != nullptr;
    Out.UniformBranch =
        Branch->isConditional() &&
        Branch->getMetadata(llvm::dxcb5_contract::kUniformMD) != nullptr;
    if (Branch->isConditional())
      Out.Condition = Source(Branch->getCondition());
    for (unsigned Idx = 0; Idx < Branch->getNumSuccessors(); ++Idx)
      Out.Successors.push_back(BlockIDs.lookup(Branch->getSuccessor(Idx)));
    return Error.empty();
  }
  if (auto *Switch = dyn_cast<SwitchInst>(Term)) {
    Out.Kind = FlowKind::Switch;
    Out.StrictBranch =
        Switch->getMetadata(llvm::dxcb5_contract::kStrictBranchMD) != nullptr;
    Out.UniformBranch =
        Switch->getMetadata(llvm::dxcb5_contract::kUniformMD) != nullptr;
    Out.Condition = SourceInt(Switch->getCondition(), false, false);
    Out.Successors.push_back(BlockIDs.lookup(Switch->getDefaultDest()));
    Out.CaseValues.push_back(0);
    for (auto Case : Switch->cases()) {
      Out.Successors.push_back(BlockIDs.lookup(Case.getCaseSuccessor()));
      Out.CaseValues.push_back(
          static_cast<uint32_t>(Case.getCaseValue()->getZExtValue()));
    }
    return Error.empty();
  }
  return Fail("unsupported DXIL terminator");
}
