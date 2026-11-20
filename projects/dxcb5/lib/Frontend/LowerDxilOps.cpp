// Distributed under the University of Illinois Open Source License.
#include "Frontend/LowerToSsaMir.h"
#include "dxc/DXIL/DxilCBuffer.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/DXIL/DxilResource.h"
#include "dxc/DXIL/DxilSampler.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include <algorithm>
#include <d3d11.h>
#include <d3d11shader.h>

using namespace dxcb5;
using namespace llvm;
using namespace hlsl;

namespace {
using Op = DXIL::OpCode;
using Kind = DXIL::ResourceKind;

const ConstantInt *IntArg(const CallInst &Call, unsigned Index) {
  return dyn_cast<ConstantInt>(Call.getArgOperand(Index));
}

unsigned SignatureLane(uint8_t Mask, unsigned Logical) {
  for (unsigned Lane = 0; Lane < 4; ++Lane)
    if ((Mask & (1u << Lane)) && Logical-- == 0)
      return Lane;
  return 4;
}

struct MathMapping {
  Op Dxil;
  unsigned Dxbc;
  unsigned Sources;
};

const MathMapping kMathTable[] = {
    {Op::FAbs, D3D10_SB_OPCODE_MOV, 1}, {Op::Saturate, D3D10_SB_OPCODE_MOV, 1}, {Op::Exp, D3D10_SB_OPCODE_EXP, 1},
    {Op::Log, D3D10_SB_OPCODE_LOG, 1}, {Op::Frc, D3D10_SB_OPCODE_FRC, 1}, {Op::Sqrt, D3D10_SB_OPCODE_SQRT, 1},
    {Op::Rsqrt, D3D10_SB_OPCODE_RSQ, 1}, {Op::Round_ne, D3D10_SB_OPCODE_ROUND_NE, 1}, {Op::Round_ni, D3D10_SB_OPCODE_ROUND_NI, 1},
    {Op::Round_pi, D3D10_SB_OPCODE_ROUND_PI, 1}, {Op::Round_z, D3D10_SB_OPCODE_ROUND_Z, 1}, {Op::Bfrev, D3D11_SB_OPCODE_BFREV, 1},
    {Op::Countbits, D3D11_SB_OPCODE_COUNTBITS, 1}, {Op::FirstbitLo, D3D11_SB_OPCODE_FIRSTBIT_LO, 1}, {Op::FirstbitHi, D3D11_SB_OPCODE_FIRSTBIT_HI, 1},
    {Op::FirstbitSHi, D3D11_SB_OPCODE_FIRSTBIT_SHI, 1}, {Op::FMin, D3D10_SB_OPCODE_MIN, 2}, {Op::FMax, D3D10_SB_OPCODE_MAX, 2},
    {Op::IMin, D3D10_SB_OPCODE_IMIN, 2}, {Op::IMax, D3D10_SB_OPCODE_IMAX, 2}, {Op::UMin, D3D10_SB_OPCODE_UMIN, 2},
    {Op::UMax, D3D10_SB_OPCODE_UMAX, 2}, {Op::FMad, D3D10_SB_OPCODE_MAD, 3}, {Op::IMad, D3D10_SB_OPCODE_IMAD, 3},
    {Op::UMad, D3D10_SB_OPCODE_UMAD, 3}, {Op::Bfi, D3D11_SB_OPCODE_BFI, 4},
    {Op::LegacyF16ToF32, D3D11_SB_OPCODE_F16TOF32, 1}, {Op::LegacyF32ToF16, D3D11_SB_OPCODE_F32TOF16, 1},
    {Op::DerivCoarseX, D3D11_SB_OPCODE_DERIV_RTX_COARSE, 1}, {Op::DerivCoarseY, D3D11_SB_OPCODE_DERIV_RTY_COARSE, 1},
    {Op::DerivFineX, D3D11_SB_OPCODE_DERIV_RTX_FINE, 1}, {Op::DerivFineY, D3D11_SB_OPCODE_DERIV_RTY_FINE, 1},
};

bool IsScalar32(Type *Ty) {
  return Ty->isFloatTy() || Ty->isHalfTy() || Ty->isIntegerTy(32) ||
         Ty->isIntegerTy(16);
}

bool IsScalarOrVec32(Type *Ty, unsigned &Width) {
  if (IsScalar32(Ty)) {
    Width = 1;
    return true;
  }
  if (const auto *VT = dyn_cast<VectorType>(Ty)) {
    Width = VT->getNumElements();
    return Width >= 1 && Width <= kRegisterLanes &&
           IsScalar32(VT->getElementType());
  }
  return false;
}

constexpr unsigned kImmediateConstantLanes = 4;
constexpr unsigned kMaxImmediateScalarRows =
    D3D11_REQ_IMMEDIATE_CONSTANT_BUFFER_ELEMENT_COUNT;

} // namespace

void SsaLowerer::UseResourceSlot(const DxilResourceBase &Binding,
                                 uint32_t Slot) {
  auto Prototype = Layout.ResourceArrays.find(&Binding);
  if (Prototype == Layout.ResourceArrays.end())
    return;
  auto &Used = UsedResourceSlots[&Binding];
  if (std::find(Used.begin(), Used.end(), Slot) != Used.end())
    return;
  Used.push_back(Slot);

  SmallVector<uint32_t, 4> Dimensions;
  auto *Element = Binding.GetHLSLType()->getPointerElementType();
  while (auto *Array = dyn_cast<ArrayType>(Element)) {
    Dimensions.push_back(static_cast<uint32_t>(Array->getNumElements()));
    Element = Array->getElementType();
  }
  auto Index = Slot - Binding.GetLowerBound();
  SmallVector<uint32_t, 4> Subscripts;
  for (auto Dim = Dimensions.rbegin(); Dim != Dimensions.rend(); ++Dim) {
    Subscripts.push_back(Index % *Dim);
    Index /= *Dim;
  }

  BoundResource Resource = Prototype->second;
  Resource.Slot = Slot;
  for (auto Sub = Subscripts.rbegin(); Sub != Subscripts.rend(); ++Sub)
    Resource.Name += '[' + std::to_string(*Sub) + ']';
  Ssa.Mir.Resources.push_back(std::move(Resource));
}

bool SsaLowerer::Handle(Value *V, Operand &Result, Kind *Resource,
                        uint32_t *SampleCount, uint32_t *Stride) {
  auto *Call = dyn_cast<CallInst>(V);
  if (!Call || !OP::IsDxilOpFuncCallInst(Call, Op::CreateHandle))
    return Fail("SM5 requires a statically bound resource handle");
  const auto *Class = IntArg(*Call, 1), *ID = IntArg(*Call, 2),
             *Index = IntArg(*Call, 3);
  if (!Class || !ID)
    return Fail("SM5 requires a constant resource class and range ID");
  if (!Index)
    return Fail("SM5 resource array requires a compile-time constant binding index");
  const auto Range = static_cast<unsigned>(ID->getZExtValue());
  const DxilResourceBase *Binding;
  OperandKind OperandType;
  switch (static_cast<DXIL::ResourceClass>(Class->getZExtValue())) {
  case DXIL::ResourceClass::SRV: {
    const auto &Texture = Module.GetSRV(Range);
    Binding = &Texture;
    if (Resource) *Resource = Texture.GetKind();
    if (SampleCount) *SampleCount = Texture.GetSampleCount();
    if (Stride) *Stride = Texture.GetElementStride();
    OperandType = OperandKind::Resource;
    break;
  }
  case DXIL::ResourceClass::UAV: {
    const auto &Uav = Module.GetUAV(Range);
    Binding = &Uav;
    if (Resource) *Resource = Uav.GetKind();
    if (SampleCount) *SampleCount = Uav.GetSampleCount();
    if (Stride) *Stride = Uav.GetElementStride();
    OperandType = OperandKind::Uav;
    break;
  }
  case DXIL::ResourceClass::Sampler:
    Binding = &Module.GetSampler(Range);
    OperandType = OperandKind::Sampler;
    break;
  case DXIL::ResourceClass::CBuffer:
    Binding = &Module.GetCBuffer(Range);
    OperandType = OperandKind::ConstantBuffer;
    break;
  default:
    return Fail("unsupported SM5 resource class");
  }
  const auto Slot = static_cast<unsigned>(Index->getZExtValue());
  const auto First = Binding->GetLowerBound();
  if (Slot < First || Slot - First >= Binding->GetRangeSize())
    return Fail(Twine("SM5 resource index is outside the binding range for ") +
                Binding->GetGlobalName());
  if (!Layout.ResourceArrays.empty())
    UseResourceSlot(*Binding, Slot);
  Result = {OperandType, Slot};
  if (OperandType == OperandKind::Resource || OperandType == OperandKind::Uav)
    Result.Swizzle = kIdentitySwizzle;
  return true;
}

bool SsaLowerer::SignatureRegister(const CallInst &Call, bool Output,
                                   Operand &Register, bool MarkUsed) {
  const auto *ID = IntArg(Call, 1), *Row = IntArg(Call, 2),
             *Column = IntArg(Call, 3);
  if (!ID || !Row || !Column)
    return Fail("dynamic shader signature addressing is unsupported");
  const auto &Indices = Output ? Layout.Output : Layout.Input;
  auto Match = Indices.find({static_cast<unsigned>(ID->getZExtValue()),
                             static_cast<unsigned>(Row->getZExtValue())});
  if (Match == Indices.end())
    return Fail("DXIL signature references an unknown element");
  auto &Element = (Output ? Ssa.Mir.Outputs : Ssa.Mir.Inputs)[Match->second];
  if (Output && Element.SystemValue == D3D_NAME_COVERAGE) {
    Register = {OperandKind::OutputMask, 0, 0, 1u, 0};
    return true;
  }
  if (Output && (Element.SystemValue == D3D_NAME_DEPTH ||
                 Element.SystemValue == D3D_NAME_DEPTH_GREATER_EQUAL ||
                 Element.SystemValue == D3D_NAME_DEPTH_LESS_EQUAL)) {
    const uint32_t DepthKind =
        Element.SystemValue == D3D_NAME_DEPTH_GREATER_EQUAL
            ? 1u
            : (Element.SystemValue == D3D_NAME_DEPTH_LESS_EQUAL ? 2u : 0u);
    Register = {OperandKind::OutputDepth, DepthKind, 0, 1u, 0};
    return true;
  }
  unsigned Lane =
      SignatureLane(Element.Mask, static_cast<unsigned>(Column->getZExtValue()));
  if (Lane >= 4)
    return Fail("DXIL signature references an unknown lane");
  const uint8_t LaneBit = static_cast<uint8_t>(1u << Lane);
  if (MarkUsed)
    Element.UsedMask |= LaneBit;
  Register = {Output ? OperandKind::Output : OperandKind::Input,
              Element.Register, 0, LaneBit, BroadcastLaneSwizzle(Lane)};
  return true;
}

bool SsaLowerer::BindValue(llvm::Instruction &Inst) {
  if (Inst.getType()->isVoidTy() || Inst.getType()->isPointerTy() ||
      isa<ExtractValueInst>(Inst) || isa<ExtractElementInst>(Inst) ||
      isa<ShuffleVectorInst>(Inst))
    return true;
  auto *Call = dyn_cast<CallInst>(&Inst);
  if (!Call || !Call->getNumArgOperands() || !OP::IsDxilOpFuncCallInst(Call)) {
    unsigned Width = 1;
    if (const auto *VT = dyn_cast<VectorType>(Inst.getType())) {
      Width = VT->getNumElements();
      if (Width == 0 || Width > kRegisterLanes)
        return Fail("unsupported LLVM vector width in SM5");
    }
    Values[&Inst] = AllocTemp(MaskForWidth(Width));
    return true;
  }
  const auto Opcode = OP::getOpCode(Call);
  if (Opcode == Op::LoadInput)
    return SignatureRegister(*Call, false, Values[Call]);
  if (Opcode == Op::SampleIndex) {
    for (const auto &Elem : Ssa.Mir.Inputs) {
      if (Elem.SystemValue == D3D_NAME_SAMPLE_INDEX) {
        Values[Call] = {OperandKind::Input, Elem.Register, 0, 1u,
                        BroadcastLaneSwizzle(0)};
        return true;
      }
    }
    return Fail("SV_SampleIndex input is missing from signature");
  }
  const auto *Row =
      Opcode == Op::CBufferLoadLegacy ? IntArg(*Call, 2) : nullptr;
  if (Row) {
    Operand Buffer{OperandKind::Null};
    if (!Handle(Call->getArgOperand(1), Buffer))
      return false;
    Buffer.Element = static_cast<uint32_t>(Row->getZExtValue());
    Buffer.Swizzle = kIdentitySwizzle;
    Values[&Inst] = Buffer;
    return true;
  }
  // Aggregate DXIL return types (Sample/LD/Gather/GetDimensions/Dynamic CB) produce 4-lane values.
  uint8_t Mask = 1;
  if (Inst.getType()->isStructTy())
    Mask = 0xfu;
  else if (const auto *VT = dyn_cast<VectorType>(Inst.getType()))
    Mask = MaskForWidth(VT->getNumElements());
  Values[&Inst] = AllocTemp(Mask);
  return true;
}

bool SsaLowerer::LowerConstantBuffer(CallInst &Call) {
  if (IntArg(Call, 2))
    return true;
  Operand Buffer{OperandKind::Null};
  if (!Handle(Call.getArgOperand(1), Buffer))
    return false;
  const auto Index = RelativeIndex(Call.getArgOperand(2));
  Buffer.RelativeRegister = Index.Index;
  Buffer.RelativeLane = GetSwizzleLane(Index.Swizzle, 0);
  for (auto &CB : Ssa.Mir.ConstantBuffers)
    if (CB.Slot == Buffer.Index)
      CB.DynamicIndex = true;
  Buffer.Swizzle = kIdentitySwizzle;
  auto Dest = Values.lookup(&Call);
  Dest.Mask = 0xfu;
  Emit(D3D10_SB_OPCODE_MOV, Dest, {Buffer});
  return Error.empty();
}

bool SsaLowerer::LowerMath(CallInst &Call, unsigned OpNumber) {
  const auto Opcode = static_cast<Op>(OpNumber);
  const auto Result = Values.lookup(&Call);
  const bool Precise = Module.IsPrecise(&Call);
  for (const auto &Entry : kMathTable) {
    if (Entry.Dxil != Opcode)
      continue;
    const bool SignedInt = Opcode == Op::IMin || Opcode == Op::IMax;
    const bool UnsignedInt = Opcode == Op::UMin || Opcode == Op::UMax;
    SmallVector<Operand, 4> Sources;
    for (unsigned I = 1; I <= Entry.Sources; ++I) {
      const Value *Arg = Call.getArgOperand(I);
      Sources.push_back((SignedInt || UnsignedInt)
                            ? SourceInt(Arg, SignedInt, Precise)
                            : Source(Arg));
    }
    if (Opcode == Op::FAbs)
      Sources.back() = AbsMirOperand(Ssa, Sources.back());
    Emit(Entry.Dxbc, Result, Sources, Precise, Opcode == Op::Saturate);
    return Error.empty();
  }
  if (Opcode == Op::Sin || Opcode == Op::Cos) {
    SsaInst Inst;
    Inst.Opcode = D3D10_SB_OPCODE_SINCOS;
    Inst.Dest = Opcode == Op::Sin ? Result : Operand{OperandKind::Null};
    Inst.AuxDest = Opcode == Op::Cos ? Result : Operand{OperandKind::Null};
    Inst.Sources = {Source(Call.getArgOperand(1))};
    Inst.Precise = Precise;
    CurrentBlock->Insts.push_back(std::move(Inst));
    return Error.empty();
  }
  if (Opcode == Op::IsNaN) {
    const auto From = Source(Call.getArgOperand(1));
    Emit(D3D10_SB_OPCODE_NE, Result, {From, From}, Precise);
    return Error.empty();
  }
  if (Opcode == Op::Dot2 || Opcode == Op::Dot3 || Opcode == Op::Dot4) {
    const unsigned Count = OpNumber - static_cast<unsigned>(Op::Dot2) + 2;
    const auto A = VectorArgs(Call, 1, Count);
    const auto B = VectorArgs(Call, 1 + Count, Count);
    Emit(D3D10_SB_OPCODE_DP2 + Count - 2, Result, {A, B}, Precise);
    return Error.empty();
  }
  if (Opcode == Op::UAddc || Opcode == Op::USubb) {
    const Operand Sum = AllocTemp(1);
    const Operand CarryRaw = AllocTemp(1);
    const Operand CarryBool = AllocTemp(1);
    SsaInst Inst;
    Inst.Opcode = Opcode == Op::UAddc ? D3D11_SB_OPCODE_UADDC
                                      : D3D11_SB_OPCODE_USUBB;
    Inst.Dest = Sum;
    Inst.AuxDest = CarryRaw;
    Inst.Sources = {Source(Call.getArgOperand(1)), Source(Call.getArgOperand(2))};
    Inst.Precise = Precise;
    CurrentBlock->Insts.push_back(std::move(Inst));
    Emit(D3D10_SB_OPCODE_INEG, CarryBool, {CarryRaw}, Precise);
    Operand Dest = Result;
    Dest.Mask = 0x3u;
    Emit(PseudoCombineVec, Dest, {Sum, CarryBool}, Precise);
    return Error.empty();
  }
  return Fail(Twine("unsupported SM5 DXIL operation: ") +
              Call.getCalledFunction()->getName() + " #" + Twine(OpNumber));
}

bool SsaLowerer::LowerEvalAttribute(CallInst &Call, unsigned OpNumber) {
  const auto Opcode = static_cast<Op>(OpNumber);
  Operand Input{OperandKind::Null};
  if (!SignatureRegister(Call, false, Input, true))
    return false;
  const Operand Dest = Values.lookup(&Call);
  if (Opcode == Op::EvalCentroid) {
    Emit(D3D11_SB_OPCODE_EVAL_CENTROID, Dest, {Input});
    return Error.empty();
  }
  if (Opcode == Op::EvalSampleIndex) {
    Emit(D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX, Dest,
         {Input, Source(Call.getArgOperand(4))});
    return Error.empty();
  }
  Operand Offset = VectorArgs(Call, 4, 2);
  if (Offset.Kind == OperandKind::Immediate) {
    const std::array<uint32_t, 4> Bits = {Offset.Index, Offset.Index, 0u, 0u};
    Offset = {OperandKind::ImmediateVector, Ssa.InternLiteralVector(Bits), 0,
              0xfu, kIdentitySwizzle};
  }
  Emit(D3D11_SB_OPCODE_EVAL_SNAPPED, Dest, {Input, Offset});
  return Error.empty();
}

bool SsaLowerer::AddLocalConstant(const GlobalVariable &Global, uint32_t &Base,
                                  unsigned Width) {
  auto Match = LocalConstants.find(&Global);
  if (Match != LocalConstants.end()) {
    Base = Match->second;
    return true;
  }
  const auto *Init = Global.getInitializer();
  auto *Array = dyn_cast<ArrayType>(Init->getType());
  unsigned ElemWidth = 1;
  if (!Array || !IsScalarOrVec32(Array->getElementType(), ElemWidth))
    return Fail("SM5 local constants require an array of 32-bit scalars or vectors");
  Base = static_cast<uint32_t>(Ssa.Mir.ImmediateConstants.size() /
                               kImmediateConstantLanes);
  const auto Count = Array->getNumElements();
  if (ElemWidth > 1) {
    if (Count > kMaxImmediateScalarRows - Base)
      return Fail("local constants exceed the SM5 immediate constant buffer");
    Ssa.Mir.ImmediateConstants.resize((Base + Count) * kImmediateConstantLanes);
    for (uint64_t Row = 0; Row < Count; ++Row) {
      const auto *Vec = Init->getAggregateElement(Row);
      for (unsigned Lane = 0; Lane < ElemWidth; ++Lane) {
        const auto *Elt = Vec->getAggregateElement(Lane);
        Ssa.Mir.ImmediateConstants[(Base + Row) * kImmediateConstantLanes + Lane] =
            Source(Elt).Index;
      }
    }
    LocalConstants[&Global] = Base;
    return Error.empty();
  }
  const auto Rows = Count / Width;
  if (Rows > kMaxImmediateScalarRows - Base)
    return Fail("local constants exceed the SM5 immediate constant buffer");
  Ssa.Mir.ImmediateConstants.resize((Base + Rows) * kImmediateConstantLanes);
  for (uint64_t Index = 0; Index < Count; ++Index) {
    const auto *V = Init->getAggregateElement(Index);
    const auto Row = Base + Index / Width;
    const auto Lane = Index % Width;
    Ssa.Mir.ImmediateConstants[Row * kImmediateConstantLanes + Lane] =
        Source(V).Index;
  }
  LocalConstants[&Global] = Base;
  return Error.empty();
}

bool SsaLowerer::LowerLocalConstant(LoadInst &Load) {
  const auto Packed = ConstantTuples.Loads.find(&Load);
  if (Packed != ConstantTuples.Loads.end()) {
    const auto &Tuple = Packed->second;
    uint32_t Base;
    if (!AddLocalConstant(*Tuple.Global, Base, Tuple.Width))
      return false;
    Operand Input{OperandKind::ImmediateConstantBuffer, Base};
    const auto Index = RelativeIndex(Tuple.Index);
    Input.RelativeRegister = Index.Index;
    Input.RelativeLane = GetSwizzleLane(Index.Swizzle, 0);
    Input.Swizzle =
        static_cast<uint8_t>(kIdentitySwizzle & ((1u << (2 * Tuple.Width)) - 1));
    auto Dest = Values.lookup(&Load);
    Dest.Mask = MaskForWidth(Tuple.Width);
    Emit(D3D10_SB_OPCODE_MOV, Dest, {Input});
    return Error.empty();
  }
  auto *Pointer = dyn_cast<GEPOperator>(Load.getPointerOperand());
  unsigned LoadWidth = 1;
  if (!Pointer || Pointer->getNumIndices() != 2 || !Load.isSimple() ||
      !IsScalarOrVec32(Load.getType(), LoadWidth))
    return Fail("SM5 load requires a 32-bit scalar or vector in a local constant array");
  const auto *Global = dyn_cast<GlobalVariable>(Pointer->getPointerOperand());
  const auto *Outer = dyn_cast<ConstantInt>(Pointer->getOperand(1));
  if (!Global || !Global->isConstant() || !Global->hasDefinitiveInitializer() ||
      !Outer || !Outer->isZero())
    return Fail("SM5 load requires a defined immutable local constant array");
  uint32_t Base;
  if (!AddLocalConstant(*Global, Base, LoadWidth))
    return false;
  Operand Input{OperandKind::ImmediateConstantBuffer, Base};
  const auto *Index = Pointer->getOperand(2);
  if (auto *Offset = dyn_cast<ConstantInt>(Index)) {
    Input.Index += static_cast<uint32_t>(Offset->getZExtValue());
  } else {
    const auto Reg = RelativeIndex(Index);
    Input.RelativeRegister = Reg.Index;
    Input.RelativeLane = GetSwizzleLane(Reg.Swizzle, 0);
  }
  if (LoadWidth > 1)
    Input.Swizzle =
        static_cast<uint8_t>(kIdentitySwizzle & ((1u << (2 * LoadWidth)) - 1));
  Emit(D3D10_SB_OPCODE_MOV, Values.lookup(&Load), {Input});
  return Error.empty();
}

bool SsaLowerer::LowerCall(CallInst &Call) {
  auto *Fn = Call.getCalledFunction();
  if (!Fn)
    return Fail("unsupported indirect DXIL call");
  if (Fn->getName().startswith("llvm.dbg.") ||
      Fn->getName().startswith("llvm.lifetime."))
    return true;
  if (!Fn->getName().startswith("dx.op.") || !IntArg(Call, 0))
    return Fail(Twine("unsupported DXIL call: ") + Fn->getName());
  const auto Number = static_cast<unsigned>(IntArg(Call, 0)->getZExtValue());
  const auto Opcode = static_cast<Op>(Number);
  switch (Opcode) {
  case Op::LoadInput:
  case Op::SampleIndex: return true;
  case Op::Coverage: {
    const Operand Cov{OperandKind::InputCoverage, 0, 0, 1u, 0};
    Emit(D3D10_SB_OPCODE_MOV, Values.lookup(&Call), {Cov});
    return Error.empty();
  }
  case Op::EvalSnapped:
  case Op::EvalSampleIndex:
  case Op::EvalCentroid: return LowerEvalAttribute(Call, Number);
  case Op::StoreOutput: {
    Operand Register{OperandKind::Null};
    if (!SignatureRegister(Call, true, Register))
      return false;
    const Value *Val = Call.getArgOperand(4);
    bool SignedOut = false;
    if (const auto *ID = IntArg(Call, 1), *Row = IntArg(Call, 2); ID && Row) {
      auto It = Layout.Output.find({static_cast<unsigned>(ID->getZExtValue()),
                                    static_cast<unsigned>(Row->getZExtValue())});
      if (It != Layout.Output.end())
        SignedOut = Ssa.Mir.Outputs[It->second].ComponentType ==
                    D3D_REGISTER_COMPONENT_SINT32;
    }
    Emit(D3D10_SB_OPCODE_MOV, Register, {SourceInt(Val, SignedOut, false)});
    return Error.empty();
  }
  case Op::CreateHandle: return true;
  case Op::CBufferLoadLegacy: return LowerConstantBuffer(Call);
  case Op::GetDimensions: return LowerTextureDimensions(Call);
  case Op::CalculateLOD: return LowerCalculateLod(Call);
  case Op::BufferLoad:
  case Op::RawBufferLoad: return LowerBufferLoad(Call, Number);
  case Op::TextureStore:
  case Op::BufferStore:
  case Op::RawBufferStore: return LowerUavStore(Call, Number);
  case Op::AtomicBinOp:
  case Op::AtomicCompareExchange: return LowerAtomic(Call, Number);
  case Op::BufferUpdateCounter: return LowerBufferCounter(Call);
  case Op::TextureGather:
  case Op::TextureGatherCmp: return LowerGather(Call, Number);
  case Op::TextureLoad:
  case Op::Sample:
  case Op::SampleLevel:
  case Op::SampleBias:
  case Op::SampleGrad:
  case Op::SampleCmp:
  case Op::SampleCmpLevelZero: return LowerSample(Call, Number);
  case Op::Discard: {
    SsaInst D;
    D.Opcode = D3D10_SB_OPCODE_DISCARD;
    D.Sources = {Source(Call.getArgOperand(1))};
    D.TestNonZero = true;
    CurrentBlock->Insts.push_back(std::move(D));
    return Error.empty();
  }
  default: return LowerMath(Call, Number);
  }
}
