// Distributed under the University of Illinois Open Source License.
#include "Frontend/LowerToSsaMir.h"
#include "Support/ResourceShape.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/Support/d3d12TokenizedProgramFormat.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

using namespace dxcb5;
using namespace llvm;
using namespace hlsl;

namespace {
using Op = DXIL::OpCode;
using Kind = DXIL::ResourceKind;

const ConstantInt *IntArg(const CallInst &Call, unsigned Index) {
  return dyn_cast<ConstantInt>(Call.getArgOperand(Index));
}

unsigned CoordinateCount(Kind Resource) {
  switch (Resource) {
  case Kind::Texture1D:
  case Kind::TypedBuffer: return 1;
  case Kind::Texture1DArray:
  case Kind::Texture2D:
  case Kind::Texture2DMS: return 2;
  case Kind::Texture2DArray:
  case Kind::Texture2DMSArray:
  case Kind::Texture3D:
  case Kind::TextureCube: return 3;
  case Kind::TextureCubeArray: return 4;
  default: return 0;
  }
}

unsigned LodCoordCount(Kind Resource) {
  switch (Resource) {
  case Kind::Texture1D:
  case Kind::Texture1DArray: return 1;
  case Kind::Texture2D:
  case Kind::Texture2DArray: return 2;
  case Kind::Texture3D:
  case Kind::TextureCube:
  case Kind::TextureCubeArray: return 3;
  default: return 0;
  }
}

Operand EnsureVector4(SsaProgram &Ssa, Operand Op) {
  if (Op.Kind != OperandKind::Immediate)
    return Op;
  const std::array<uint32_t, 4> Bits = {Op.Index, Op.Index, Op.Index, Op.Index};
  return {OperandKind::ImmediateVector, Ssa.InternLiteralVector(Bits), 0, 0xfu,
          kIdentitySwizzle};
}

Operand ClampCoordSwizzle(Operand Op, unsigned Width) {
  if (Op.Kind == OperandKind::Immediate ||
      Op.Kind == OperandKind::ImmediateVector || Width == 0 || Width >= 4)
    return Op;
  const unsigned Fallback = GetSwizzleLane(Op.Swizzle, 0);
  for (unsigned L = Width; L < kRegisterLanes; ++L)
    Op.Swizzle = SetSwizzleLane(Op.Swizzle, L, Fallback);
  return Op;
}

unsigned SampleToken(Op Opcode) {
  switch (Opcode) {
  case Op::Sample: return D3D10_SB_OPCODE_SAMPLE;
  case Op::SampleBias: return D3D10_SB_OPCODE_SAMPLE_B;
  case Op::SampleLevel: return D3D10_SB_OPCODE_SAMPLE_L;
  case Op::SampleCmp: return D3D10_SB_OPCODE_SAMPLE_C;
  case Op::SampleCmpLevelZero: return D3D10_SB_OPCODE_SAMPLE_C_LZ;
  case Op::SampleGrad: return D3D10_SB_OPCODE_SAMPLE_D;
  default: return 0;
  }
}

bool AddOffsets(SsaInst &I, const CallInst &Call, unsigned Start,
                unsigned Count = 3) {
  int8_t Off[3] = {0, 0, 0};
  for (unsigned Axis = 0; Axis < Count; ++Axis) {
    auto *V = Call.getArgOperand(Start + Axis);
    if (isa<UndefValue>(V))
      continue;
    auto *Offset = dyn_cast<ConstantInt>(V);
    if (!Offset || Offset->getSExtValue() < -8 || Offset->getSExtValue() > 7)
      return false;
    Off[Axis] = static_cast<int8_t>(Offset->getSExtValue());
  }
  for (unsigned Axis = 0; Axis < Count; ++Axis)
    I.TexelOffset[Axis] = Off[Axis];
  return true;
}

bool ZeroOrUndef(const Value *V) {
  if (isa<UndefValue>(V))
    return true;
  const auto *C = dyn_cast<Constant>(V);
  return C && C->isNullValue();
}

} // namespace

bool SsaLowerer::LowerSample(CallInst &Call, unsigned OpNumber) {
  const auto Opcode = static_cast<Op>(OpNumber);
  Operand Resource{OperandKind::Null}, Sampler{OperandKind::Null};
  Kind ResourceKind = Kind::Invalid;
  if (!Handle(Call.getArgOperand(1), Resource, &ResourceKind))
    return false;
  const auto Coords = CoordinateCount(ResourceKind);
  if (!Coords)
    return Fail("unsupported texture dimension");
  auto Result = Values.lookup(&Call);
  Result.Mask = 0xfu;
  if (Opcode == Op::TextureLoad) {
    const bool IsUav = Resource.Kind == OperandKind::Uav;
    const bool IsMs = !IsUav && (ResourceKind == Kind::Texture2DMS ||
                                 ResourceKind == Kind::Texture2DMSArray);
    const bool HasMip = !IsUav && !IsMs && ResourceKind != Kind::TypedBuffer;
    SmallVector<Operand, 4> Lanes;
    for (unsigned L = 0; L < Coords; ++L)
      Lanes.push_back(Source(Call.getArgOperand(3 + L)));
    if (HasMip) {
      while (Lanes.size() < 3)
        Lanes.push_back(MakeImm(0));
      Lanes.push_back(Source(Call.getArgOperand(2)));
    }
    const auto Address = EnsureVector4(
        Ssa, ClampCoordSwizzle(CombineOperands(Lanes),
                               static_cast<unsigned>(Lanes.size())));
    SsaInst Inst;
    Inst.Opcode = IsUav ? D3D11_SB_OPCODE_LD_UAV_TYPED
                        : (IsMs ? D3D10_SB_OPCODE_LD_MS : D3D10_SB_OPCODE_LD);
    Inst.Dest = Result;
    Inst.Sources = {Address, Resource};
    if (IsMs)
      Inst.Sources.push_back(Source(Call.getArgOperand(2)));
    if (!AddOffsets(Inst, Call, 6))
      return Fail("unsupported SM5 texture offset");
    CurrentBlock->Insts.push_back(std::move(Inst));
    return Error.empty();
  }
  if (!Handle(Call.getArgOperand(2), Sampler))
    return false;
  const auto Address =
      EnsureVector4(Ssa, ClampCoordSwizzle(VectorArgs(Call, 3, Coords), Coords));
  SsaInst Sample;
  Sample.Opcode = SampleToken(Opcode);
  Sample.Dest = Result;
  Sample.Sources = {Address, Resource, Sampler};
  if (!AddOffsets(Sample, Call, 7))
    return Fail("unsupported SM5 texture offset");
  if (Opcode == Op::SampleGrad) {
    const unsigned GradW = Coords > 3 ? 3 : Coords;
    Sample.Sources.push_back(EnsureVector4(
        Ssa, ClampCoordSwizzle(VectorArgs(Call, 10, GradW), GradW)));
    Sample.Sources.push_back(EnsureVector4(
        Ssa, ClampCoordSwizzle(VectorArgs(Call, 13, GradW), GradW)));
  } else if (Opcode != Op::Sample) {
    Sample.Sources.push_back(Source(Call.getArgOperand(10)));
  }
  const unsigned Clamp =
      Opcode == Op::Sample ? 10 : Opcode == Op::SampleGrad ? 16 : 11;
  if (Clamp < Call.getNumArgOperands() && Opcode != Op::SampleCmpLevelZero &&
      !ZeroOrUndef(Call.getArgOperand(Clamp)))
    return Fail("SM5 texture clamp is unsupported");
  CurrentBlock->Insts.push_back(std::move(Sample));
  return Error.empty();
}

bool SsaLowerer::LowerGather(CallInst &Call, unsigned OpNumber) {
  using namespace DXIL::OperandIndex;
  const bool IsCmp = static_cast<Op>(OpNumber) == Op::TextureGatherCmp;
  Operand Texture{OperandKind::Null}, Sampler{OperandKind::Null};
  Kind ResourceKind = Kind::Invalid;
  if (!Handle(Call.getArgOperand(kTextureGatherTexHandleOpIdx), Texture,
              &ResourceKind) ||
      !Handle(Call.getArgOperand(kTextureGatherSamplerHandleOpIdx), Sampler))
    return false;
  const bool Is2D = ResourceKind == Kind::Texture2D ||
                    ResourceKind == Kind::Texture2DArray;
  const bool IsCube = ResourceKind == Kind::TextureCube ||
                      ResourceKind == Kind::TextureCubeArray;
  if (!Is2D && !IsCube)
    return Fail("SM5 gather requires a Texture2D, Texture2DArray, TextureCube, "
                "or TextureCubeArray resource");
  const auto *Channel = IntArg(Call, kTextureGatherChannelOpIdx);
  if (!Channel || Channel->getZExtValue() >= 4)
    return Fail("SM5 gather requires a constant channel from 0 to 3");
  Sampler.Swizzle = static_cast<uint8_t>(Channel->getZExtValue());
  auto Dest = Values.lookup(&Call);
  Dest.Mask = 0xfu;
  const unsigned GatherCoords = CoordinateCount(ResourceKind);
  const Operand GatherAddr = EnsureVector4(
      Ssa, ClampCoordSwizzle(
               VectorArgs(Call, kTextureGatherCoord0OpIdx, GatherCoords),
               GatherCoords));
  SsaInst Gather;
  Gather.Dest = Dest;
  const bool ImmOffset =
      AddOffsets(Gather, Call, kTextureGatherOffset0OpIdx, 2);
  if (!ImmOffset && !Is2D)
    return Fail("SM5 programmable gather offset requires Texture2D or "
                "Texture2DArray");
  if (ImmOffset && IsCube && (Gather.TexelOffset[0] || Gather.TexelOffset[1]))
    return Fail("SM5 gather offset is unsupported on cube textures");
  if (ImmOffset) {
    Gather.Opcode = IsCmp ? D3D11_SB_OPCODE_GATHER4_C
                          : D3D10_1_SB_OPCODE_GATHER4;
    Gather.Sources = {GatherAddr, Texture, Sampler};
  } else {
    Gather.Opcode = IsCmp ? D3D11_SB_OPCODE_GATHER4_PO_C
                          : D3D11_SB_OPCODE_GATHER4_PO;
    const Operand Offset = EnsureVector4(
        Ssa, ClampCoordSwizzle(VectorArgs(Call, kTextureGatherOffset0OpIdx, 2),
                               2));
    Gather.Sources = {GatherAddr, Offset, Texture, Sampler};
  }
  if (IsCmp)
    Gather.Sources.push_back(Source(Call.getArgOperand(10)));
  CurrentBlock->Insts.push_back(std::move(Gather));
  return Error.empty();
}

bool SsaLowerer::LowerCalculateLod(CallInst &Call) {
  Operand Texture{OperandKind::Null}, Sampler{OperandKind::Null};
  Kind ResourceKind = Kind::Invalid;
  if (!Handle(Call.getArgOperand(1), Texture, &ResourceKind) ||
      !Handle(Call.getArgOperand(2), Sampler))
    return false;
  const unsigned Coords = LodCoordCount(ResourceKind);
  if (!Coords)
    return Fail("SM5 CalculateLOD unsupported texture dimension");
  const auto *Clamped = IntArg(Call, 6);
  if (!Clamped)
    return Fail("SM5 CalculateLOD requires a constant clamped flag");
  const Operand Address = EnsureVector4(
      Ssa, ClampCoordSwizzle(VectorArgs(Call, 3, Coords), Coords));
  Texture.Swizzle = BroadcastLaneSwizzle(Clamped->isZero() ? 1u : 0u);
  Emit(D3D10_1_SB_OPCODE_LOD, Values.lookup(&Call),
       {Address, Texture, Sampler});
  return Error.empty();
}

bool SsaLowerer::LowerBufferLoad(CallInst &Call, unsigned OpNumber) {
  const auto Opcode = static_cast<Op>(OpNumber);
  if (const auto *RetST = dyn_cast<StructType>(Call.getType())) {
    if (RetST->getNumElements() > 0) {
      const Type *ElemTy = RetST->getElementType(0);
      if (!ElemTy->isFloatTy() && !ElemTy->isIntegerTy(32))
        return Fail("non-32-bit SM5 buffer load is unsupported");
    }
  }
  Operand Resource{OperandKind::Null};
  Kind ResourceKind = Kind::Invalid;
  if (!Handle(Call.getArgOperand(1), Resource, &ResourceKind))
    return false;
  auto Dest = Values.lookup(&Call);
  Dest.Mask = 0xfu;
  if (Opcode == Op::RawBufferLoad) {
    if (const auto *M = IntArg(Call, 4)) {
      const uint8_t Req = static_cast<uint8_t>(M->getZExtValue() & 0xfu);
      Dest.Mask = Req ? Req : 0x1u;
    }
  }
  const Operand Index = Source(Call.getArgOperand(2));
  if (ResourceKind == Kind::TypedBuffer) {
    const uint32_t LdOp = Resource.Kind == OperandKind::Uav
                              ? D3D11_SB_OPCODE_LD_UAV_TYPED
                              : D3D10_SB_OPCODE_LD;
    Emit(LdOp, Dest,
         {EnsureVector4(Ssa, ClampCoordSwizzle(Index, 1)), Resource});
    return Error.empty();
  }
  if (ResourceKind == Kind::StructuredBuffer) {
    const auto *OffVal = Call.getArgOperand(3);
    const Operand Offset = isa<UndefValue>(OffVal) ? MakeImm(0) : Source(OffVal);
    Emit(D3D11_SB_OPCODE_LD_STRUCTURED, Dest, {Index, Offset, Resource});
    return Error.empty();
  }
  if (ResourceKind == Kind::RawBuffer) {
    Emit(D3D11_SB_OPCODE_LD_RAW, Dest, {Index, Resource});
    return Error.empty();
  }
  return Fail("unsupported SM5 buffer load resource kind");
}

bool SsaLowerer::LowerUavStore(CallInst &Call, unsigned OpNumber) {
  const auto Opcode = static_cast<Op>(OpNumber);
  Operand Resource{OperandKind::Null};
  Kind ResourceKind = Kind::Invalid;
  if (!Handle(Call.getArgOperand(1), Resource, &ResourceKind))
    return false;
  if (Resource.Kind != OperandKind::Uav)
    return Fail("SM5 store requires a UAV handle");
  if (Opcode == Op::TextureStore) {
    const unsigned Coords = CoordinateCount(ResourceKind);
    if (!Coords)
      return Fail("unsupported SM5 UAV store dimension");
    const auto *TexMaskC = IntArg(Call, 9);
    const uint8_t TexMask =
        TexMaskC ? static_cast<uint8_t>(TexMaskC->getZExtValue() & 0xfu) : 0xfu;
    const Operand Addr = EnsureVector4(
        Ssa, ClampCoordSwizzle(VectorArgs(Call, 2, Coords), Coords));
    SmallVector<Operand, 4> ValLanes;
    for (unsigned L = 0; L < 4; ++L) {
      const Value *V = Call.getArgOperand(5 + L);
      if (isa<UndefValue>(V) || !(TexMask & (1u << L))) {
        ValLanes.push_back(MakeImm(0));
        continue;
      }
      const Type *ValTy = V->getType();
      if (!ValTy->isFloatTy() && !ValTy->isIntegerTy(32))
        return Fail("non-32-bit SM5 texture store is unsupported");
      ValLanes.push_back(Source(V));
    }
    const Operand Val = EnsureVector4(Ssa, CombineOperands(ValLanes));
    Resource.Mask = 0xfu;
    Emit(D3D11_SB_OPCODE_STORE_UAV_TYPED, Resource, {Addr, Val});
    return Error.empty();
  }
  const auto *MaskConst = IntArg(Call, 8);
  const uint8_t Mask =
      MaskConst ? static_cast<uint8_t>(MaskConst->getZExtValue() & 0xfu) : 0xfu;
  for (unsigned L = 0; L < 4; ++L) {
    if (!(Mask & (1u << L)))
      continue;
    const Type *ValTy = Call.getArgOperand(4 + L)->getType();
    if (!ValTy->isFloatTy() && !ValTy->isIntegerTy(32))
      return Fail("non-32-bit SM5 buffer store is unsupported");
  }
  if (ResourceKind == Kind::TypedBuffer) {
    const Operand Addr =
        EnsureVector4(Ssa, ClampCoordSwizzle(Source(Call.getArgOperand(2)), 1));
    SmallVector<Operand, 4> ValLanes;
    for (unsigned L = 0; L < 4; ++L) {
      const Value *V = Call.getArgOperand(4 + L);
      ValLanes.push_back(isa<UndefValue>(V) ? MakeImm(0) : Source(V));
    }
    const Operand Val = EnsureVector4(Ssa, CombineOperands(ValLanes));
    Resource.Mask = 0xfu;
    Emit(D3D11_SB_OPCODE_STORE_UAV_TYPED, Resource, {Addr, Val});
    return Error.empty();
  }
  unsigned Width = 0;
  if (Mask == 0x1u) Width = 1;
  else if (Mask == 0x3u) Width = 2;
  else if (Mask == 0x7u) Width = 3;
  else if (Mask == 0xfu) Width = 4;
  else return Fail("SM5 structured/raw store requires a contiguous write mask");
  SmallVector<Operand, 4> ValLanes;
  for (unsigned L = 0; L < Width; ++L) {
    const Value *V = Call.getArgOperand(4 + L);
    ValLanes.push_back(isa<UndefValue>(V) ? MakeImm(0) : Source(V));
  }
  const Operand Val = EnsureVector4(
      Ssa, ClampCoordSwizzle(CombineOperands(ValLanes), Width));
  Resource.Mask = Mask;
  const Operand Index = Source(Call.getArgOperand(2));
  if (ResourceKind == Kind::StructuredBuffer) {
    const auto *OffVal = Call.getArgOperand(3);
    const Operand Offset = isa<UndefValue>(OffVal) ? MakeImm(0) : Source(OffVal);
    Emit(D3D11_SB_OPCODE_STORE_STRUCTURED, Resource, {Index, Offset, Val});
    return Error.empty();
  }
  if (ResourceKind == Kind::RawBuffer) {
    Emit(D3D11_SB_OPCODE_STORE_RAW, Resource, {Index, Val});
    return Error.empty();
  }
  return Fail("unsupported SM5 buffer store resource kind");
}

bool SsaLowerer::LowerAtomic(CallInst &Call, unsigned OpNumber) {
  const auto Opcode = static_cast<Op>(OpNumber);
  Operand Resource{OperandKind::Null};
  Kind ResourceKind = Kind::Invalid;
  if (!Handle(Call.getArgOperand(1), Resource, &ResourceKind))
    return false;
  if (Resource.Kind != OperandKind::Uav)
    return Fail("SM5 atomic operation requires a UAV handle");
  unsigned AddrCoords = CoordinateCount(ResourceKind);
  if (ResourceKind == Kind::RawBuffer)
    AddrCoords = 1;
  else if (ResourceKind == Kind::StructuredBuffer)
    AddrCoords = 2;
  if (!AddrCoords)
    return Fail("unsupported SM5 atomic UAV dimension");
  Resource.Mask = 0;
  const bool IsCmpExch = Opcode == Op::AtomicCompareExchange;
  const unsigned CoordStart = IsCmpExch ? 2 : 3;
  const Operand Addr = EnsureVector4(
      Ssa, ClampCoordSwizzle(VectorArgs(Call, CoordStart, AddrCoords),
                             AddrCoords));
  if (IsCmpExch) {
    const Operand CmpVal = Source(Call.getArgOperand(5));
    const Operand NewVal = Source(Call.getArgOperand(6));
    if (Call.use_empty()) {
      Emit(D3D11_SB_OPCODE_ATOMIC_CMP_STORE, Resource, {Addr, CmpVal, NewVal});
      return Error.empty();
    }
    SsaInst Inst;
    Inst.Opcode = D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH;
    Inst.Dest = Values.lookup(&Call);
    Inst.AuxDest = Resource;
    Inst.Sources = {Addr, CmpVal, NewVal};
    CurrentBlock->Insts.push_back(std::move(Inst));
    return Error.empty();
  }
  const auto *OpArg = IntArg(Call, 2);
  if (!OpArg)
    return Fail("SM5 atomic requires a constant atomic opcode");
  const auto BinOp = static_cast<DXIL::AtomicBinOpCode>(OpArg->getZExtValue());
  const Operand NewVal = Source(Call.getArgOperand(6));
  const bool NeedRet = !Call.use_empty() ||
                       BinOp == DXIL::AtomicBinOpCode::Exchange;
  uint32_t DxbcOp = 0;
  switch (BinOp) {
  case DXIL::AtomicBinOpCode::Add:
    DxbcOp = NeedRet ? D3D11_SB_OPCODE_IMM_ATOMIC_IADD
                     : D3D11_SB_OPCODE_ATOMIC_IADD;
    break;
  case DXIL::AtomicBinOpCode::And:
    DxbcOp = NeedRet ? D3D11_SB_OPCODE_IMM_ATOMIC_AND
                     : D3D11_SB_OPCODE_ATOMIC_AND;
    break;
  case DXIL::AtomicBinOpCode::Or:
    DxbcOp = NeedRet ? D3D11_SB_OPCODE_IMM_ATOMIC_OR
                     : D3D11_SB_OPCODE_ATOMIC_OR;
    break;
  case DXIL::AtomicBinOpCode::Xor:
    DxbcOp = NeedRet ? D3D11_SB_OPCODE_IMM_ATOMIC_XOR
                     : D3D11_SB_OPCODE_ATOMIC_XOR;
    break;
  case DXIL::AtomicBinOpCode::IMin:
    DxbcOp = NeedRet ? D3D11_SB_OPCODE_IMM_ATOMIC_IMIN
                     : D3D11_SB_OPCODE_ATOMIC_IMIN;
    break;
  case DXIL::AtomicBinOpCode::IMax:
    DxbcOp = NeedRet ? D3D11_SB_OPCODE_IMM_ATOMIC_IMAX
                     : D3D11_SB_OPCODE_ATOMIC_IMAX;
    break;
  case DXIL::AtomicBinOpCode::UMin:
    DxbcOp = NeedRet ? D3D11_SB_OPCODE_IMM_ATOMIC_UMIN
                     : D3D11_SB_OPCODE_ATOMIC_UMIN;
    break;
  case DXIL::AtomicBinOpCode::UMax:
    DxbcOp = NeedRet ? D3D11_SB_OPCODE_IMM_ATOMIC_UMAX
                     : D3D11_SB_OPCODE_ATOMIC_UMAX;
    break;
  case DXIL::AtomicBinOpCode::Exchange:
    DxbcOp = D3D11_SB_OPCODE_IMM_ATOMIC_EXCH;
    break;
  default: return Fail("unsupported SM5 atomic binop");
  }
  if (!NeedRet) {
    Emit(DxbcOp, Resource, {Addr, NewVal});
    return Error.empty();
  }
  SsaInst Inst;
  Inst.Opcode = DxbcOp;
  Inst.Dest = Values.lookup(&Call);
  Inst.AuxDest = Resource;
  Inst.Sources = {Addr, NewVal};
  CurrentBlock->Insts.push_back(std::move(Inst));
  return Error.empty();
}

bool SsaLowerer::LowerBufferCounter(CallInst &Call) {
  Operand Resource{OperandKind::Null};
  if (!Handle(Call.getArgOperand(1), Resource))
    return false;
  if (Resource.Kind != OperandKind::Uav)
    return Fail("SM5 buffer counter requires a UAV handle");
  const auto *Inc = IntArg(Call, 2);
  if (!Inc)
    return Fail("SM5 buffer counter requires a constant increment");
  const int64_t Delta = Inc->getSExtValue();
  if (Delta != 1 && Delta != -1)
    return Fail("SM5 buffer counter requires +1 or -1 increment");
  Resource.Mask = 0;
  SsaInst Inst;
  Inst.Opcode = Delta > 0 ? D3D11_SB_OPCODE_IMM_ATOMIC_ALLOC
                          : D3D11_SB_OPCODE_IMM_ATOMIC_CONSUME;
  Inst.Dest = Values.lookup(&Call);
  Inst.AuxDest = Resource;
  CurrentBlock->Insts.push_back(std::move(Inst));
  return Error.empty();
}

bool SsaLowerer::LowerTextureDimensions(CallInst &Call) {
  Operand Texture{OperandKind::Null};
  Kind ResourceKind = Kind::Invalid;
  uint32_t SampleCount = 0;
  uint32_t Stride = 0;
  if (!Handle(Call.getArgOperand(1), Texture, &ResourceKind, &SampleCount,
              &Stride))
    return false;
  auto Dest = Values.lookup(&Call);
  if (ResourceKind == Kind::TypedBuffer || ResourceKind == Kind::RawBuffer) {
    Dest.Mask = 0x1u;
    Emit(D3D11_SB_OPCODE_BUFINFO, Dest, {Texture});
    return Error.empty();
  }
  if (ResourceKind == Kind::StructuredBuffer) {
    const Operand Buf = AllocTemp(0x1u);
    Emit(D3D11_SB_OPCODE_BUFINFO, Buf, {Texture});
    Dest.Mask = 0x3u;
    Emit(PseudoCombineVec, Dest, {MakeSsaLane(Buf.Index, 0), MakeImm(Stride)});
    return Error.empty();
  }
  if (ResourceKind == Kind::Texture2DMS ||
      ResourceKind == Kind::Texture2DMSArray) {
    const bool IsArr = ResourceKind == Kind::Texture2DMSArray;
    const Operand Dim = AllocTemp(IsArr ? 0x7u : 0x3u);
    Emit(D3D10_SB_OPCODE_RESINFO, Dim, {MakeImm(0), Texture});
    Operand SampVal = MakeImm(SampleCount);
    if (!SampleCount) {
      const Operand Samp = AllocTemp(0x1u);
      Emit(D3D10_1_SB_OPCODE_SAMPLE_INFO, Samp, {Texture});
      SampVal = MakeSsaLane(Samp.Index, 0);
    }
    Dest.Mask = IsArr ? 0xfu : 0xbu;
    Emit(PseudoCombineVec, Dest,
         {MakeSsaLane(Dim.Index, 0), MakeSsaLane(Dim.Index, 1),
          IsArr ? MakeSsaLane(Dim.Index, 2) : MakeImm(0), SampVal});
    return Error.empty();
  }
  const uint8_t ResMask = GetSm5ResourceShape(ResourceKind).ResInfoDestMask;
  if (!ResMask)
    return Fail("SM5 GetDimensions unsupported resource kind");
  Dest.Mask = ResMask;
  Emit(D3D10_SB_OPCODE_RESINFO, Dest, {Source(Call.getArgOperand(2)), Texture});
  return Error.empty();
}
