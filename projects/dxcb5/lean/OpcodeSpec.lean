-- Distributed under the University of Illinois Open Source License.
-- Single Source of Truth for SM5 instruction properties, slot-read semantics,
-- lane/resource-swizzle remapping, and orthogonal DCE vs code-motion effects.

namespace DXCB5.OpcodeSpec

inductive LaneRemapKind where
  | fixed
  | componentwise
  | resourceReturnSwizzle
  | dotScalar
  deriving DecidableEq, Repr

inductive SourceSlotRule where
  | destMask
  | fixed2
  | fixed3
  | fixed4
  | scalar
  | src0Vec4
  | src01Vec4
  | src034Vec4
  | src0Vec4RestDest
  | src0ScalarRestDest
  | src01ScalarRestDest
  | src0DestRestScalar
  | src0DestRest2
  | perLaneScalar
  deriving DecidableEq, Repr

def SourceSlotRule.all : List SourceSlotRule := [
  .destMask, .fixed2, .fixed3, .fixed4, .scalar,
  .src0Vec4, .src01Vec4, .src034Vec4, .src0Vec4RestDest,
  .src0ScalarRestDest, .src01ScalarRestDest,
  .src0DestRestScalar, .src0DestRest2, .perLaneScalar
]

theorem SourceSlotRule.mem_all (r : SourceSlotRule) : r ∈ SourceSlotRule.all := by
  cases r <;> simp [SourceSlotRule.all]

inductive Sm5Opcode where
  | add | mul | mad | div | min | max | mov | movc | frc | rcp
  | eq | ne | lt | ge
  | iadd | imad | ineg | imin | imax | umin | umax | umad
  | ishl | ishr | ushr
  | ieq | ine | ilt | ige | ult | uge
  | and | or | xor | not
  | ftoi | ftou | itof | utof | f16tof32 | f32tof16
  | bfrev | countbits | bfi | ibfe | ubfe
  | firstbitLo | firstbitHi | firstbitShi
  | exp | log | rsq | sqrt | roundNe | roundNi | roundPi | roundZ
  | dp2 | dp3 | dp4
  | sincos | imul | umul | udiv | uaddc | usubb
  | derivRtx | derivRty | derivRtxCoarse | derivRtyCoarse | derivRtxFine | derivRtyFine
  | sample | sampleB | sampleC | sampleCLz | sampleD | sampleL
  | ld | ldMs | ldUavTyped | ldRaw | ldStructured
  | lod | gather4 | gather4C | gather4Po | gather4PoC
  | resinfo | bufinfo | sampleInfo
  | evalCentroid | evalSampleIndex | evalSnapped
  | storeUavTyped | storeRaw | storeStructured
  | atomicAnd | atomicOr | atomicXor | atomicCmpStore | atomicIadd
  | atomicImax | atomicImin | atomicUmax | atomicUmin
  | immAtomicAlloc | immAtomicConsume | immAtomicIadd | immAtomicAnd
  | immAtomicOr | immAtomicXor | immAtomicExch | immAtomicCmpExch
  | immAtomicImax | immAtomicImin | immAtomicUmax | immAtomicUmin
  | sync | discard
  | opIf | opElse | endif | loop | endloop | opSwitch | opCase | opDefault | endswitch
  | opBreak | breakc | opContinue | continuec | ret | retc
  | combineVec
  deriving DecidableEq, Repr

def Sm5Opcode.all : List Sm5Opcode :=
  [ .add, .mul, .mad, .div, .min, .max, .mov, .movc, .frc, .rcp,
    .eq, .ne, .lt, .ge,
    .iadd, .imad, .ineg, .imin, .imax, .umin, .umax, .umad,
    .ishl, .ishr, .ushr,
    .ieq, .ine, .ilt, .ige, .ult, .uge,
    .and, .or, .xor, .not,
    .ftoi, .ftou, .itof, .utof, .f16tof32, .f32tof16,
    .bfrev, .countbits, .bfi, .ibfe, .ubfe,
    .firstbitLo, .firstbitHi, .firstbitShi,
    .exp, .log, .rsq, .sqrt, .roundNe, .roundNi, .roundPi, .roundZ,
    .dp2, .dp3, .dp4,
    .sincos, .imul, .umul, .udiv, .uaddc, .usubb,
    .derivRtx, .derivRty, .derivRtxCoarse, .derivRtyCoarse, .derivRtxFine, .derivRtyFine,
    .sample, .sampleB, .sampleC, .sampleCLz, .sampleD, .sampleL,
    .ld, .ldMs, .ldUavTyped, .ldRaw, .ldStructured,
    .lod, .gather4, .gather4C, .gather4Po, .gather4PoC,
    .resinfo, .bufinfo, .sampleInfo,
    .evalCentroid, .evalSampleIndex, .evalSnapped,
    .storeUavTyped, .storeRaw, .storeStructured,
    .atomicAnd, .atomicOr, .atomicXor, .atomicCmpStore, .atomicIadd,
    .atomicImax, .atomicImin, .atomicUmax, .atomicUmin,
    .immAtomicAlloc, .immAtomicConsume, .immAtomicIadd, .immAtomicAnd,
    .immAtomicOr, .immAtomicXor, .immAtomicExch, .immAtomicCmpExch,
    .immAtomicImax, .immAtomicImin, .immAtomicUmax, .immAtomicUmin,
    .sync, .discard,
    .opIf, .opElse, .endif, .loop, .endloop, .opSwitch, .opCase, .opDefault, .endswitch,
    .opBreak, .breakc, .opContinue, .continuec, .ret, .retc,
    .combineVec ]

def Sm5Opcode.isPseudo : Sm5Opcode → Bool
  | .combineVec => true
  | _ => false

def Sm5Opcode.cppName : Sm5Opcode → String
  | .add => "D3D10_SB_OPCODE_ADD"
  | .mul => "D3D10_SB_OPCODE_MUL"
  | .mad => "D3D10_SB_OPCODE_MAD"
  | .div => "D3D10_SB_OPCODE_DIV"
  | .min => "D3D10_SB_OPCODE_MIN"
  | .max => "D3D10_SB_OPCODE_MAX"
  | .mov => "D3D10_SB_OPCODE_MOV"
  | .movc => "D3D10_SB_OPCODE_MOVC"
  | .frc => "D3D10_SB_OPCODE_FRC"
  | .rcp => "D3D11_SB_OPCODE_RCP"
  | .eq => "D3D10_SB_OPCODE_EQ"
  | .ne => "D3D10_SB_OPCODE_NE"
  | .lt => "D3D10_SB_OPCODE_LT"
  | .ge => "D3D10_SB_OPCODE_GE"
  | .iadd => "D3D10_SB_OPCODE_IADD"
  | .imad => "D3D10_SB_OPCODE_IMAD"
  | .ineg => "D3D10_SB_OPCODE_INEG"
  | .imin => "D3D10_SB_OPCODE_IMIN"
  | .imax => "D3D10_SB_OPCODE_IMAX"
  | .umin => "D3D10_SB_OPCODE_UMIN"
  | .umax => "D3D10_SB_OPCODE_UMAX"
  | .umad => "D3D10_SB_OPCODE_UMAD"
  | .ishl => "D3D10_SB_OPCODE_ISHL"
  | .ishr => "D3D10_SB_OPCODE_ISHR"
  | .ushr => "D3D10_SB_OPCODE_USHR"
  | .ieq => "D3D10_SB_OPCODE_IEQ"
  | .ine => "D3D10_SB_OPCODE_INE"
  | .ilt => "D3D10_SB_OPCODE_ILT"
  | .ige => "D3D10_SB_OPCODE_IGE"
  | .ult => "D3D10_SB_OPCODE_ULT"
  | .uge => "D3D10_SB_OPCODE_UGE"
  | .and => "D3D10_SB_OPCODE_AND"
  | .or => "D3D10_SB_OPCODE_OR"
  | .xor => "D3D10_SB_OPCODE_XOR"
  | .not => "D3D10_SB_OPCODE_NOT"
  | .ftoi => "D3D10_SB_OPCODE_FTOI"
  | .ftou => "D3D10_SB_OPCODE_FTOU"
  | .itof => "D3D10_SB_OPCODE_ITOF"
  | .utof => "D3D10_SB_OPCODE_UTOF"
  | .f16tof32 => "D3D11_SB_OPCODE_F16TOF32"
  | .f32tof16 => "D3D11_SB_OPCODE_F32TOF16"
  | .bfrev => "D3D11_SB_OPCODE_BFREV"
  | .countbits => "D3D11_SB_OPCODE_COUNTBITS"
  | .bfi => "D3D11_SB_OPCODE_BFI"
  | .ibfe => "D3D11_SB_OPCODE_IBFE"
  | .ubfe => "D3D11_SB_OPCODE_UBFE"
  | .firstbitLo => "D3D11_SB_OPCODE_FIRSTBIT_LO"
  | .firstbitHi => "D3D11_SB_OPCODE_FIRSTBIT_HI"
  | .firstbitShi => "D3D11_SB_OPCODE_FIRSTBIT_SHI"
  | .exp => "D3D10_SB_OPCODE_EXP"
  | .log => "D3D10_SB_OPCODE_LOG"
  | .rsq => "D3D10_SB_OPCODE_RSQ"
  | .sqrt => "D3D10_SB_OPCODE_SQRT"
  | .roundNe => "D3D10_SB_OPCODE_ROUND_NE"
  | .roundNi => "D3D10_SB_OPCODE_ROUND_NI"
  | .roundPi => "D3D10_SB_OPCODE_ROUND_PI"
  | .roundZ => "D3D10_SB_OPCODE_ROUND_Z"
  | .dp2 => "D3D10_SB_OPCODE_DP2"
  | .dp3 => "D3D10_SB_OPCODE_DP3"
  | .dp4 => "D3D10_SB_OPCODE_DP4"
  | .sincos => "D3D10_SB_OPCODE_SINCOS"
  | .imul => "D3D10_SB_OPCODE_IMUL"
  | .umul => "D3D10_SB_OPCODE_UMUL"
  | .udiv => "D3D10_SB_OPCODE_UDIV"
  | .uaddc => "D3D11_SB_OPCODE_UADDC"
  | .usubb => "D3D11_SB_OPCODE_USUBB"
  | .derivRtx => "D3D10_SB_OPCODE_DERIV_RTX"
  | .derivRty => "D3D10_SB_OPCODE_DERIV_RTY"
  | .derivRtxCoarse => "D3D11_SB_OPCODE_DERIV_RTX_COARSE"
  | .derivRtyCoarse => "D3D11_SB_OPCODE_DERIV_RTY_COARSE"
  | .derivRtxFine => "D3D11_SB_OPCODE_DERIV_RTX_FINE"
  | .derivRtyFine => "D3D11_SB_OPCODE_DERIV_RTY_FINE"
  | .sample => "D3D10_SB_OPCODE_SAMPLE"
  | .sampleB => "D3D10_SB_OPCODE_SAMPLE_B"
  | .sampleC => "D3D10_SB_OPCODE_SAMPLE_C"
  | .sampleCLz => "D3D10_SB_OPCODE_SAMPLE_C_LZ"
  | .sampleD => "D3D10_SB_OPCODE_SAMPLE_D"
  | .sampleL => "D3D10_SB_OPCODE_SAMPLE_L"
  | .ld => "D3D10_SB_OPCODE_LD"
  | .ldMs => "D3D10_SB_OPCODE_LD_MS"
  | .ldUavTyped => "D3D11_SB_OPCODE_LD_UAV_TYPED"
  | .ldRaw => "D3D11_SB_OPCODE_LD_RAW"
  | .ldStructured => "D3D11_SB_OPCODE_LD_STRUCTURED"
  | .lod => "D3D10_1_SB_OPCODE_LOD"
  | .gather4 => "D3D10_1_SB_OPCODE_GATHER4"
  | .gather4C => "D3D11_SB_OPCODE_GATHER4_C"
  | .gather4Po => "D3D11_SB_OPCODE_GATHER4_PO"
  | .gather4PoC => "D3D11_SB_OPCODE_GATHER4_PO_C"
  | .resinfo => "D3D10_SB_OPCODE_RESINFO"
  | .bufinfo => "D3D11_SB_OPCODE_BUFINFO"
  | .sampleInfo => "D3D10_1_SB_OPCODE_SAMPLE_INFO"
  | .evalCentroid => "D3D11_SB_OPCODE_EVAL_CENTROID"
  | .evalSampleIndex => "D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX"
  | .evalSnapped => "D3D11_SB_OPCODE_EVAL_SNAPPED"
  | .storeUavTyped => "D3D11_SB_OPCODE_STORE_UAV_TYPED"
  | .storeRaw => "D3D11_SB_OPCODE_STORE_RAW"
  | .storeStructured => "D3D11_SB_OPCODE_STORE_STRUCTURED"
  | .atomicAnd => "D3D11_SB_OPCODE_ATOMIC_AND"
  | .atomicOr => "D3D11_SB_OPCODE_ATOMIC_OR"
  | .atomicXor => "D3D11_SB_OPCODE_ATOMIC_XOR"
  | .atomicCmpStore => "D3D11_SB_OPCODE_ATOMIC_CMP_STORE"
  | .atomicIadd => "D3D11_SB_OPCODE_ATOMIC_IADD"
  | .atomicImax => "D3D11_SB_OPCODE_ATOMIC_IMAX"
  | .atomicImin => "D3D11_SB_OPCODE_ATOMIC_IMIN"
  | .atomicUmax => "D3D11_SB_OPCODE_ATOMIC_UMAX"
  | .atomicUmin => "D3D11_SB_OPCODE_ATOMIC_UMIN"
  | .immAtomicAlloc => "D3D11_SB_OPCODE_IMM_ATOMIC_ALLOC"
  | .immAtomicConsume => "D3D11_SB_OPCODE_IMM_ATOMIC_CONSUME"
  | .immAtomicIadd => "D3D11_SB_OPCODE_IMM_ATOMIC_IADD"
  | .immAtomicAnd => "D3D11_SB_OPCODE_IMM_ATOMIC_AND"
  | .immAtomicOr => "D3D11_SB_OPCODE_IMM_ATOMIC_OR"
  | .immAtomicXor => "D3D11_SB_OPCODE_IMM_ATOMIC_XOR"
  | .immAtomicExch => "D3D11_SB_OPCODE_IMM_ATOMIC_EXCH"
  | .immAtomicCmpExch => "D3D11_SB_OPCODE_IMM_ATOMIC_CMP_EXCH"
  | .immAtomicImax => "D3D11_SB_OPCODE_IMM_ATOMIC_IMAX"
  | .immAtomicImin => "D3D11_SB_OPCODE_IMM_ATOMIC_IMIN"
  | .immAtomicUmax => "D3D11_SB_OPCODE_IMM_ATOMIC_UMAX"
  | .immAtomicUmin => "D3D11_SB_OPCODE_IMM_ATOMIC_UMIN"
  | .sync => "D3D11_SB_OPCODE_SYNC"
  | .discard => "D3D10_SB_OPCODE_DISCARD"
  | .opIf => "D3D10_SB_OPCODE_IF"
  | .opElse => "D3D10_SB_OPCODE_ELSE"
  | .endif => "D3D10_SB_OPCODE_ENDIF"
  | .loop => "D3D10_SB_OPCODE_LOOP"
  | .endloop => "D3D10_SB_OPCODE_ENDLOOP"
  | .opSwitch => "D3D10_SB_OPCODE_SWITCH"
  | .opCase => "D3D10_SB_OPCODE_CASE"
  | .opDefault => "D3D10_SB_OPCODE_DEFAULT"
  | .endswitch => "D3D10_SB_OPCODE_ENDSWITCH"
  | .opBreak => "D3D10_SB_OPCODE_BREAK"
  | .breakc => "D3D10_SB_OPCODE_BREAKC"
  | .opContinue => "D3D10_SB_OPCODE_CONTINUE"
  | .continuec => "D3D10_SB_OPCODE_CONTINUEC"
  | .ret => "D3D10_SB_OPCODE_RET"
  | .retc => "D3D10_SB_OPCODE_RETC"
  | .combineVec => "PseudoCombineVec"

structure OpcodeDesc where
  numDests : Nat := 1
  isCompare : Bool := false
  isCommutativeBinary : Bool := false
  isDerivative : Bool := false
  requiresQuad : Bool := false
  isComponentwise : Bool := false
  acceptsFloatMods : Bool := false
  acceptsIntNeg : Bool := false
  supportsSaturate : Bool := false
  laneRemap : LaneRemapKind := .fixed
  slotRule : SourceSlotRule := .fixed4
  canEliminateIfDead : Bool := false
  canMoveAcrossDivergentBranch : Bool := false
  canMoveAcrossUniformBranch : Bool := false
  isControlOp : Bool := false
  isPostRaPureEliminable : Bool := false
  deriving DecidableEq, Repr

def isCompareOp : Sm5Opcode → Bool
  | .eq | .ne | .lt | .ge | .ieq | .ine | .ilt | .ige | .ult | .uge => true
  | _ => false

def isCommutativeBinaryOp : Sm5Opcode → Bool
  | .add | .mul | .min | .max | .eq | .ne
  | .iadd | .imul | .imin | .imax | .umin | .umax
  | .ieq | .ine | .and | .or | .xor | .dp2 | .dp3 | .dp4 => true
  | _ => false

def isDerivativeOp : Sm5Opcode → Bool
  | .derivRtx | .derivRty | .derivRtxCoarse | .derivRtyCoarse
  | .derivRtxFine | .derivRtyFine => true
  | _ => false

def requiresQuadOp : Sm5Opcode → Bool
  | .sample | .sampleB | .sampleC | .lod
  | .gather4 | .gather4C | .gather4Po | .gather4PoC => true
  | op => isDerivativeOp op

def isComponentwiseOp : Sm5Opcode → Bool
  | .add | .mul | .mad | .div | .min | .max | .mov | .movc | .frc | .rcp
  | .eq | .ne | .lt | .ge
  | .iadd | .imad | .ineg | .imin | .imax | .umin | .umax | .umad
  | .ishl | .ishr | .ushr | .ieq | .ine | .ilt | .ige | .ult | .uge
  | .and | .or | .xor | .not
  | .ftoi | .ftou | .itof | .utof | .f16tof32 | .f32tof16
  | .bfrev | .countbits | .bfi | .ibfe | .ubfe
  | .firstbitLo | .firstbitHi | .firstbitShi
  | .exp | .log | .rsq | .sqrt | .roundNe | .roundNi | .roundPi | .roundZ => true
  | _ => false

def acceptsFloatModsOp : Sm5Opcode → Bool
  | .add | .mul | .mad | .div | .min | .max | .frc | .rcp
  | .eq | .ne | .ge | .lt | .dp2 | .dp3 | .dp4
  | .exp | .log | .rsq | .sqrt | .roundNe | .roundNi | .roundPi | .roundZ
  | .ftoi | .ftou | .f32tof16 | .sincos
  | .derivRtxCoarse | .derivRtyCoarse | .derivRtxFine | .derivRtyFine => true
  | _ => false

def acceptsIntNegOp : Sm5Opcode → Bool
  | .iadd | .imad => true
  | _ => false

def supportsSaturateOp : Sm5Opcode → Bool
  | .add | .mul | .mad | .div | .min | .max | .dp2 | .dp3 | .dp4
  | .mov | .movc | .frc | .rcp | .exp | .log | .rsq | .sqrt
  | .roundNe | .roundNi | .roundPi | .roundZ => true
  | _ => false

def isControlFlowOp : Sm5Opcode → Bool
  | .opIf | .opElse | .endif | .loop | .endloop
  | .opSwitch | .opCase | .opDefault | .endswitch
  | .opBreak | .breakc | .opContinue | .continuec | .ret | .retc => true
  | _ => false

def numDestsOp : Sm5Opcode → Nat
  | .opBreak | .opContinue | .loop | .endloop | .endswitch | .opElse | .endif
  | .opDefault | .ret | .opIf | .breakc | .continuec | .discard | .retc
  | .opSwitch | .opCase | .sync => 0
  | .imul | .umul | .udiv | .sincos | .uaddc | .usubb
  | .immAtomicAlloc | .immAtomicConsume | .immAtomicIadd | .immAtomicAnd
  | .immAtomicOr | .immAtomicXor | .immAtomicExch | .immAtomicCmpExch
  | .immAtomicImax | .immAtomicImin | .immAtomicUmax | .immAtomicUmin => 2
  | _ => 1

def laneRemapOp : Sm5Opcode → LaneRemapKind
  | .dp2 | .dp3 | .dp4 => .dotScalar
  | .sample | .sampleB | .sampleC | .sampleCLz | .sampleD | .sampleL
  | .ld | .ldMs | .ldUavTyped => .resourceReturnSwizzle
  | .sincos | .imul | .umul | .udiv => .componentwise
  | op =>
    if isComponentwiseOp op || isDerivativeOp op then .componentwise
    else .fixed

def slotRuleOp : Sm5Opcode → SourceSlotRule
  | .dp2 => .fixed2
  | .dp3 => .fixed3
  | .dp4 => .fixed4
  | .sample | .sampleB | .sampleL | .sampleC | .sampleCLz
  | .ld | .ldMs | .ldUavTyped | .lod | .gather4 | .gather4C
  | .atomicAnd | .atomicOr | .atomicXor | .atomicCmpStore | .atomicIadd
  | .atomicImax | .atomicImin | .atomicUmax | .atomicUmin
  | .immAtomicIadd | .immAtomicAnd | .immAtomicOr | .immAtomicXor
  | .immAtomicExch | .immAtomicCmpExch | .immAtomicImax | .immAtomicImin
  | .immAtomicUmax | .immAtomicUmin => .src0Vec4
  | .sampleD => .src034Vec4
  | .gather4Po | .gather4PoC => .src01Vec4
  | .storeUavTyped => .src0Vec4RestDest
  | .storeRaw => .src0ScalarRestDest
  | .storeStructured => .src01ScalarRestDest
  | .evalSampleIndex => .src0DestRestScalar
  | .evalSnapped => .src0DestRest2
  | .resinfo | .bufinfo | .sampleInfo | .ldRaw | .ldStructured
  | .discard | .opIf | .opSwitch | .breakc | .continuec | .retc => .scalar
  | .combineVec => .perLaneScalar
  | op =>
    if laneRemapOp op == .componentwise then .destMask else .fixed4

-- Orthogonal effect queries (Codex #2): dead-code elimination is strictly wider
-- than moving across pixel-divergent branches (implicit-quad ops can be deleted
-- when dead, but must NOT be moved across non-uniform branches!).
def canEliminateIfDeadOp : Sm5Opcode → Bool
  | .storeUavTyped | .storeRaw | .storeStructured
  | .atomicAnd | .atomicOr | .atomicXor | .atomicCmpStore | .atomicIadd
  | .atomicImax | .atomicImin | .atomicUmax | .atomicUmin
  | .immAtomicAlloc | .immAtomicConsume | .immAtomicIadd | .immAtomicAnd
  | .immAtomicOr | .immAtomicXor | .immAtomicExch | .immAtomicCmpExch
  | .immAtomicImax | .immAtomicImin | .immAtomicUmax | .immAtomicUmin
  | .sync | .discard => false
  | op => !isControlFlowOp op

def canMoveAcrossDivergentBranchOp : Sm5Opcode → Bool
  | .ldUavTyped | .ldRaw | .ldStructured => false
  | op => canEliminateIfDeadOp op && !requiresQuadOp op

def canMoveAcrossUniformBranchOp : Sm5Opcode → Bool
  | .ldUavTyped | .ldRaw | .ldStructured => false
  | op => canEliminateIfDeadOp op

def isPostRaPureEliminableOp : Sm5Opcode → Bool
  | .mov | .movc | .add | .mul | .mad | .div | .min | .max | .sqrt | .rsq
  | .rcp | .log | .exp | .frc | .roundNe | .roundNi | .roundPi | .roundZ
  | .dp2 | .dp3 | .dp4 | .eq | .ne | .lt | .ge
  | .and | .or | .xor | .not
  | .iadd | .ineg | .imad | .imul | .imin | .imax | .umin | .umax
  | .ishl | .ishr | .ushr | .ieq | .ine | .ilt | .ige | .ult | .uge
  | .ftoi | .ftou | .itof | .utof
  | .ubfe | .ibfe | .bfi | .bfrev | .countbits
  | .firstbitHi | .firstbitLo | .firstbitShi => true
  | _ => false

def desc (op : Sm5Opcode) : OpcodeDesc :=
  { numDests := numDestsOp op
    isCompare := isCompareOp op
    isCommutativeBinary := isCommutativeBinaryOp op
    isDerivative := isDerivativeOp op
    requiresQuad := requiresQuadOp op
    isComponentwise := isComponentwiseOp op
    acceptsFloatMods := acceptsFloatModsOp op
    acceptsIntNeg := acceptsIntNegOp op
    supportsSaturate := supportsSaturateOp op
    laneRemap := laneRemapOp op
    slotRule := slotRuleOp op
    canEliminateIfDead := canEliminateIfDeadOp op
    canMoveAcrossDivergentBranch := canMoveAcrossDivergentBranchOp op
    canMoveAcrossUniformBranch := canMoveAcrossUniformBranchOp op
    isControlOp := isControlFlowOp op
    isPostRaPureEliminable := isPostRaPureEliminableOp op }

def slotMaskOfRule (rule : SourceSlotRule) (destMask : BitVec 4) (srcIdx : Nat) : BitVec 4 :=
  match rule with
  | .destMask => destMask
  | .fixed2 => 0x3#4
  | .fixed3 => 0x7#4
  | .fixed4 => 0xf#4
  | .scalar => 0x1#4
  | .src0Vec4 => if srcIdx == 0 then 0xf#4 else 0x1#4
  | .src01Vec4 => if srcIdx == 0 || srcIdx == 1 then 0xf#4 else 0x1#4
  | .src034Vec4 => if srcIdx == 0 || srcIdx == 3 || srcIdx == 4 then 0xf#4 else 0x1#4
  | .src0Vec4RestDest => if srcIdx == 0 then 0xf#4 else destMask
  | .src0ScalarRestDest => if srcIdx == 0 then 0x1#4 else destMask
  | .src01ScalarRestDest => if srcIdx == 0 || srcIdx == 1 then 0x1#4 else destMask
  | .src0DestRestScalar => if srcIdx == 0 then destMask else 0x1#4
  | .src0DestRest2 => if srcIdx == 0 then destMask else 0x3#4
  | .perLaneScalar =>
    if srcIdx < 4 && destMask.getLsbD srcIdx then 0x1#4 else 0x0#4

def sourceSlotMask (op : Sm5Opcode) (destMask : BitVec 4) (srcIdx : Nat) : BitVec 4 :=
  slotMaskOfRule (desc op).slotRule destMask srcIdx

def sourceReadLanes (op : Sm5Opcode) (destMask : BitVec 4) (srcIdx : Nat)
    (swz : Fin 4 → Fin 4) : BitVec 4 :=
  let slots := sourceSlotMask op destMask srcIdx
  let b0 : BitVec 4 := if slots.getLsbD 0 then (1#4) <<< (swz 0).val else 0#4
  let b1 : BitVec 4 := if slots.getLsbD 1 then (1#4) <<< (swz 1).val else 0#4
  let b2 : BitVec 4 := if slots.getLsbD 2 then (1#4) <<< (swz 2).val else 0#4
  let b3 : BitVec 4 := if slots.getLsbD 3 then (1#4) <<< (swz 3).val else 0#4
  b0 ||| b1 ||| b2 ||| b3

-- ============================================================================
-- Independent executable evaluation semantics (Codex #1: NO self-proving!).
-- Every opcode's concrete slot usage is defined directly here from SM5 ISA
-- semantics, completely independently of `slotRuleOp` / `desc`.
-- ============================================================================

abbrev Vec4 := Fin 4 → BitVec 32

def swizzleVec (v : Vec4) (swz : Fin 4 → Fin 4) : Vec4 :=
  fun slot => v (swz slot)

def mixVec4 (v : Vec4) : BitVec 32 :=
  v 0 + v 1 * 3#32 + v 2 * 5#32 + v 3 * 7#32

def mixMask (v : Vec4) (m : BitVec 4) : BitVec 32 :=
  (if m.getLsbD 0 then v 0 else 0#32) +
  (if m.getLsbD 1 then v 1 * 3#32 else 0#32) +
  (if m.getLsbD 2 then v 2 * 5#32 else 0#32) +
  (if m.getLsbD 3 then v 3 * 7#32 else 0#32)

-- Concrete SM5 evaluation per output lane: directly expresses which input
-- slots affect output lanes according to the SM5 hardware specification.
def evalOp (op : Sm5Opcode) (destMask : BitVec 4) (srcs : Nat → Vec4) : Vec4 :=
  fun outLane =>
    if !destMask.getLsbD outLane.val then 0#32
    else match op with
    -- Dot products: exact SM5 component counts (2 / 3 / 4)
    | .dp2 => srcs 0 0 * srcs 1 0 + srcs 0 1 * srcs 1 1
    | .dp3 => srcs 0 0 * srcs 1 0 + srcs 0 1 * srcs 1 1 + srcs 0 2 * srcs 1 2
    | .dp4 => srcs 0 0 * srcs 1 0 + srcs 0 1 * srcs 1 1 +
              srcs 0 2 * srcs 1 2 + srcs 0 3 * srcs 1 3
    -- Componentwise & lane-independent multi-dest / derivative ops
    | .add | .mul | .mad | .div | .min | .max | .mov | .movc | .frc | .rcp
    | .eq | .ne | .lt | .ge
    | .iadd | .imad | .ineg | .imin | .imax | .umin | .umax | .umad
    | .ishl | .ishr | .ushr | .ieq | .ine | .ilt | .ige | .ult | .uge
    | .and | .or | .xor | .not
    | .ftoi | .ftou | .itof | .utof | .f16tof32 | .f32tof16
    | .bfrev | .countbits | .bfi | .ibfe | .ubfe
    | .firstbitLo | .firstbitHi | .firstbitShi
    | .exp | .log | .rsq | .sqrt | .roundNe | .roundNi | .roundPi | .roundZ
    | .sincos | .imul | .umul | .udiv
    | .derivRtx | .derivRty | .derivRtxCoarse | .derivRtyCoarse
    | .derivRtxFine | .derivRtyFine =>
      srcs 0 outLane + srcs 1 outLane * 3#32 + srcs 2 outLane * 5#32 + srcs 3 outLane * 7#32
    -- Sample / Load / Atomic ops: src0 = 4-lane addr, rest = scalar
    | .sample | .sampleB | .sampleL | .sampleC | .sampleCLz
    | .ld | .ldMs | .ldUavTyped | .lod | .gather4 | .gather4C
    | .atomicAnd | .atomicOr | .atomicXor | .atomicCmpStore | .atomicIadd
    | .atomicImax | .atomicImin | .atomicUmax | .atomicUmin
    | .immAtomicIadd | .immAtomicAnd | .immAtomicOr | .immAtomicXor
    | .immAtomicExch | .immAtomicCmpExch | .immAtomicImax | .immAtomicImin
    | .immAtomicUmax | .immAtomicUmin =>
      mixVec4 (srcs 0) + srcs 1 0 + srcs 2 0 + srcs 3 0 + srcs 4 0
    -- sample_d: src0 = coord(4), src3 = ddx(4), src4 = ddy(4), rest = scalar
    | .sampleD =>
      mixVec4 (srcs 0) + srcs 1 0 + srcs 2 0 + mixVec4 (srcs 3) + mixVec4 (srcs 4)
    -- gather4_po / gather4_po_c: src0 = coord(4), src1 = offset(4), rest = scalar
    | .gather4Po | .gather4PoC =>
      mixVec4 (srcs 0) + mixVec4 (srcs 1) + srcs 2 0 + srcs 3 0 + srcs 4 0
    -- Stores: addr + destMask-controlled data
    | .storeUavTyped =>
      mixVec4 (srcs 0) + mixMask (srcs 1) destMask
    | .storeRaw =>
      srcs 0 0 + mixMask (srcs 1) destMask
    | .storeStructured =>
      srcs 0 0 + srcs 1 0 + mixMask (srcs 2) destMask
    -- Interpolation eval
    | .evalSampleIndex =>
      mixMask (srcs 0) destMask + srcs 1 0
    | .evalSnapped =>
      mixMask (srcs 0) destMask + srcs 1 0 + srcs 1 1 * 3#32
    -- Scalar-only query / raw load / conditional control ops
    | .resinfo | .bufinfo | .sampleInfo | .ldRaw | .ldStructured
    | .discard | .opIf | .opSwitch | .breakc | .continuec | .retc =>
      srcs 0 0 + srcs 1 0 + srcs 2 0
    -- MIR vector-combine IR op: lane L reads scalar lane 0 of source L
    | .combineVec =>
      srcs outLane.val 0
    -- Conservative 4-lane fallback ops
    | .uaddc | .usubb | .evalCentroid
    | .immAtomicAlloc | .immAtomicConsume
    | .sync | .opElse | .endif | .loop | .endloop | .opCase
    | .opDefault | .endswitch | .opBreak | .opContinue | .ret =>
      mixVec4 (srcs 0) + mixVec4 (srcs 1) + mixVec4 (srcs 2)

def replaceSrc (srcs : Nat → Vec4) (srcIdx : Nat) (v : Vec4) : Nat → Vec4 :=
  fun k => if k == srcIdx then v else srcs k

private theorem readLanes_has_slot (op : Sm5Opcode) (destMask : BitVec 4)
    (srcIdx : Nat) (swz : Fin 4 → Fin 4) (slot : Fin 4)
    (hSlot : (sourceSlotMask op destMask srcIdx).getLsbD slot.val = true) :
    (sourceReadLanes op destMask srcIdx swz).getLsbD (swz slot).val = true := by
  unfold sourceReadLanes
  rcases slot with ⟨v, hv⟩
  match v with
  | 0 => simp [hSlot]
  | 1 => simp [hSlot]
  | 2 => simp [hSlot]
  | 3 => simp [hSlot]
  | _ + 4 => omega

private theorem swz_eq_at_slot (op : Sm5Opcode) (destMask : BitVec 4)
    (srcIdx : Nat) (swz : Fin 4 → Fin 4) (a b : Vec4)
    (hEq : ∀ lane : Fin 4,
      (sourceReadLanes op destMask srcIdx swz).getLsbD lane.val = true →
      b lane = a lane)
    (slot : Fin 4)
    (hSlot : (sourceSlotMask op destMask srcIdx).getLsbD slot.val = true) :
    swizzleVec b swz slot = swizzleVec a swz slot :=
  hEq (swz slot) (readLanes_has_slot op destMask srcIdx swz slot hSlot)

private theorem mixVec4_eq (u v : Vec4)
    (h0 : v 0 = u 0) (h1 : v 1 = u 1) (h2 : v 2 = u 2) (h3 : v 3 = u 3) :
    mixVec4 v = mixVec4 u := by
  simp [mixVec4, h0, h1, h2, h3]

private theorem mixMask_eq (u v : Vec4) (m : BitVec 4)
    (h : ∀ s : Fin 4, m.getLsbD s.val = true → v s = u s) :
    mixMask v m = mixMask u m := by
  unfold mixMask
  have h0 : (if m.getLsbD 0 then v 0 else 0#32) = (if m.getLsbD 0 then u 0 else 0#32) := by
    cases hm : m.getLsbD 0 with
    | false => rfl
    | true => simp [h 0 hm]
  have h1 : (if m.getLsbD 1 then v 1 * 3#32 else 0#32) = (if m.getLsbD 1 then u 1 * 3#32 else 0#32) := by
    cases hm : m.getLsbD 1 with
    | false => rfl
    | true => simp [h 1 hm]
  have h2 : (if m.getLsbD 2 then v 2 * 5#32 else 0#32) = (if m.getLsbD 2 then u 2 * 5#32 else 0#32) := by
    cases hm : m.getLsbD 2 with
    | false => rfl
    | true => simp [h 2 hm]
  have h3 : (if m.getLsbD 3 then v 3 * 7#32 else 0#32) = (if m.getLsbD 3 then u 3 * 7#32 else 0#32) := by
    cases hm : m.getLsbD 3 with
    | false => rfl
    | true => simp [h 3 hm]
  rw [h0, h1, h2, h3]

-- Helper: prove equality of `replaceSrc ... k` at any slot covered by `sourceSlotMask`.
private theorem repl_eq_slot (op : Sm5Opcode) (destMask : BitVec 4)
    (srcs : Nat → Vec4) (srcIdx k : Nat) (swz : Fin 4 → Fin 4) (a b : Vec4)
    (hEq : ∀ lane : Fin 4,
      (sourceReadLanes op destMask srcIdx swz).getLsbD lane.val = true →
      b lane = a lane)
    (slot : Fin 4)
    (hSlot : k = srcIdx → (sourceSlotMask op destMask srcIdx).getLsbD slot.val = true) :
    replaceSrc srcs srcIdx (swizzleVec b swz) k slot =
    replaceSrc srcs srcIdx (swizzleVec a swz) k slot := by
  unfold replaceSrc
  by_cases hk : (k == srcIdx) = true
  · have heq : k = srcIdx := beq_iff_eq.mp hk
    simp only [hk, ↓reduceIte]
    exact swz_eq_at_slot op destMask srcIdx swz a b hEq slot (hSlot heq)
  · simp [hk]

private theorem repl_eq_mixVec4 (op : Sm5Opcode) (destMask : BitVec 4)
    (srcs : Nat → Vec4) (srcIdx k : Nat) (swz : Fin 4 → Fin 4) (a b : Vec4)
    (hEq : ∀ lane : Fin 4,
      (sourceReadLanes op destMask srcIdx swz).getLsbD lane.val = true →
      b lane = a lane)
    (hAll : k = srcIdx → ∀ s : Fin 4, (sourceSlotMask op destMask srcIdx).getLsbD s.val = true) :
    mixVec4 (replaceSrc srcs srcIdx (swizzleVec b swz) k) =
    mixVec4 (replaceSrc srcs srcIdx (swizzleVec a swz) k) :=
  mixVec4_eq _ _
    (repl_eq_slot op destMask srcs srcIdx k swz a b hEq 0 (fun h => hAll h 0))
    (repl_eq_slot op destMask srcs srcIdx k swz a b hEq 1 (fun h => hAll h 1))
    (repl_eq_slot op destMask srcs srcIdx k swz a b hEq 2 (fun h => hAll h 2))
    (repl_eq_slot op destMask srcs srcIdx k swz a b hEq 3 (fun h => hAll h 3))

private theorem repl_eq_mixMask (op : Sm5Opcode) (destMask : BitVec 4)
    (srcs : Nat → Vec4) (srcIdx k : Nat) (swz : Fin 4 → Fin 4) (a b : Vec4)
    (hEq : ∀ lane : Fin 4,
      (sourceReadLanes op destMask srcIdx swz).getLsbD lane.val = true →
      b lane = a lane)
    (hMask : k = srcIdx → ∀ s : Fin 4, destMask.getLsbD s.val = true →
      (sourceSlotMask op destMask srcIdx).getLsbD s.val = true) :
    mixMask (replaceSrc srcs srcIdx (swizzleVec b swz) k) destMask =
    mixMask (replaceSrc srcs srcIdx (swizzleVec a swz) k) destMask :=
  mixMask_eq _ _ destMask (fun s hs => repl_eq_slot op destMask srcs srcIdx k swz a b hEq s (fun h => hMask h s hs))

-- Theorem 1 (Codex #1): Non-circular soundness of `sourceReadLanes` against
-- the independent SM5 operational semantics `evalOp`.
-- Corrupting e.g. `dp3` from `.fixed3` to `.scalar` breaks this proof!
theorem sourceReadLanes_sound (op : Sm5Opcode) (destMask : BitVec 4)
    (srcs : Nat → Vec4) (srcIdx : Nat) (swz : Fin 4 → Fin 4)
    (a b : Vec4)
    (hEq : ∀ lane : Fin 4,
      (sourceReadLanes op destMask srcIdx swz).getLsbD lane.val = true →
      b lane = a lane) :
    ∀ outLane : Fin 4, destMask.getLsbD outLane.val = true →
      evalOp op destMask (replaceSrc srcs srcIdx (swizzleVec b swz)) outLane =
      evalOp op destMask (replaceSrc srcs srcIdx (swizzleVec a swz)) outLane := by
  intro outLane hOut
  cases op <;> simp only [evalOp, hOut, Bool.not_true, Bool.false_eq_true, ↓reduceIte]
  all_goals
    repeat first
      | rw [repl_eq_mixVec4 _ _ _ _ _ _ _ _ hEq]
      | rw [repl_eq_mixMask _ _ _ _ _ _ _ _ hEq]
      | rw [repl_eq_slot _ _ _ _ _ _ _ _ hEq]
    all_goals (
      intro h; subst h;
      simp only [sourceSlotMask, desc, slotRuleOp, laneRemapOp,
                 isComponentwiseOp, isDerivativeOp, slotMaskOfRule];
      first
      | exact hOut
      | exact fun _ hs => hs
      | decide
      | (simp [hOut, outLane.isLt]; try decide) )

-- ============================================================================
-- Theorem 2: Resource return swizzle remapping & consistency (prevents
-- `SAMPLE_C` / `GATHER4` mismatches across Liveness, OutOfSsa, and RegAlloc).
-- ============================================================================

def hasResourceReturnSwizzle (op : Sm5Opcode) : Bool :=
  (desc op).laneRemap == .resourceReturnSwizzle

def remapResourceReturnSwizzle (resSwz : Fin 4 → Fin 4) (oldMask : BitVec 4)
    (laneMap : Fin 4 → Fin 4) : Fin 4 → Fin 4 :=
  fun pl =>
    if oldMask.getLsbD 0 && laneMap 0 == pl then resSwz 0
    else if oldMask.getLsbD 1 && laneMap 1 == pl then resSwz 1
    else if oldMask.getLsbD 2 && laneMap 2 == pl then resSwz 2
    else if oldMask.getLsbD 3 && laneMap 3 == pl then resSwz 3
    else resSwz 0

theorem remapResourceSwizzle_preserves_channel (resSwz : Fin 4 → Fin 4)
    (oldMask : BitVec 4) (laneMap : Fin 4 → Fin 4)
    (hInj : ∀ a b : Fin 4, oldMask.getLsbD a.val = true →
      oldMask.getLsbD b.val = true → laneMap a = laneMap b → a = b)
    (vl : Fin 4) (hActive : oldMask.getLsbD vl.val = true) :
    remapResourceReturnSwizzle resSwz oldMask laneMap (laneMap vl) = resSwz vl := by
  unfold remapResourceReturnSwizzle
  have neq_of_diff : ∀ (i j : Fin 4), i ≠ j →
      oldMask.getLsbD i.val = true → oldMask.getLsbD j.val = true →
      (laneMap i == laneMap j) = false := by
    intro i j hij hi hj
    cases hc : (laneMap i == laneMap j) with
    | false => rfl
    | true => exact absurd (hInj i j hi hj (beq_iff_eq.mp hc)) hij
  match vl with
  | ⟨0, _⟩ => simp [hActive]
  | ⟨1, _⟩ =>
    cases h0 : oldMask.getLsbD 0 with
    | false => simp [hActive]
    | true => simp [neq_of_diff 0 1 (by decide) h0 hActive, hActive]
  | ⟨2, _⟩ =>
    cases h0 : oldMask.getLsbD 0 with
    | false =>
      cases h1 : oldMask.getLsbD 1 with
      | false => simp [hActive]
      | true => simp [neq_of_diff 1 2 (by decide) h1 hActive, hActive]
    | true =>
      have hne0 := neq_of_diff 0 2 (by decide) h0 hActive
      cases h1 : oldMask.getLsbD 1 with
      | false => simp [hne0, hActive]
      | true => simp [hne0, neq_of_diff 1 2 (by decide) h1 hActive, hActive]
  | ⟨3, _⟩ =>
    cases h0 : oldMask.getLsbD 0 with
    | false =>
      cases h1 : oldMask.getLsbD 1 with
      | false =>
        cases h2 : oldMask.getLsbD 2 with
        | false => simp [hActive]
        | true => simp [neq_of_diff 2 3 (by decide) h2 hActive, hActive]
      | true =>
        have hne1 := neq_of_diff 1 3 (by decide) h1 hActive
        cases h2 : oldMask.getLsbD 2 with
        | false => simp [hne1, hActive]
        | true => simp [hne1, neq_of_diff 2 3 (by decide) h2 hActive, hActive]
    | true =>
      have hne0 := neq_of_diff 0 3 (by decide) h0 hActive
      cases h1 : oldMask.getLsbD 1 with
      | false =>
        cases h2 : oldMask.getLsbD 2 with
        | false => simp [hne0, hActive]
        | true => simp [hne0, neq_of_diff 2 3 (by decide) h2 hActive, hActive]
      | true =>
        have hne1 := neq_of_diff 1 3 (by decide) h1 hActive
        cases h2 : oldMask.getLsbD 2 with
        | false => simp [hne0, hne1, hActive]
        | true => simp [hne0, hne1, neq_of_diff 2 3 (by decide) h2 hActive, hActive]

theorem sampleC_uses_resource_swizzle :
    hasResourceReturnSwizzle .sampleC = true ∧
    hasResourceReturnSwizzle .sampleCLz = true ∧
    (desc .gather4).laneRemap = .fixed ∧
    (desc .gather4C).laneRemap = .fixed ∧
    (desc .gather4Po).laneRemap = .fixed ∧
    (desc .gather4PoC).laneRemap = .fixed := by
  decide

-- ============================================================================
-- Theorem 3 (Codex #2): Orthogonal DCE vs Divergent Code-Motion Invariants.
-- ============================================================================

theorem divergentMove_sound (op : Sm5Opcode) :
    (desc op).canMoveAcrossDivergentBranch = true →
    (desc op).requiresQuad = false ∧ (desc op).canEliminateIfDead = true := by
  cases op <;> decide

theorem implicitQuad_dead_only :
    (desc .sample).canEliminateIfDead = true ∧
    (desc .sample).canMoveAcrossDivergentBranch = false ∧
    (desc .sampleB).canEliminateIfDead = true ∧
    (desc .sampleB).canMoveAcrossDivergentBranch = false ∧
    (desc .sampleC).canEliminateIfDead = true ∧
    (desc .sampleC).canMoveAcrossDivergentBranch = false ∧
    (desc .gather4).canEliminateIfDead = true ∧
    (desc .gather4).canMoveAcrossDivergentBranch = false ∧
    (desc .gather4C).canEliminateIfDead = true ∧
    (desc .gather4C).canMoveAcrossDivergentBranch = false ∧
    (desc .gather4Po).canEliminateIfDead = true ∧
    (desc .gather4Po).canMoveAcrossDivergentBranch = false ∧
    (desc .gather4PoC).canEliminateIfDead = true ∧
    (desc .gather4PoC).canMoveAcrossDivergentBranch = false ∧
    (desc .derivRtx).canEliminateIfDead = true ∧
    (desc .derivRtx).canMoveAcrossDivergentBranch = false ∧
    (desc .sampleL).canMoveAcrossDivergentBranch = true ∧
    (desc .sampleD).canMoveAcrossDivergentBranch = true := by
  decide

theorem all_opcodes_complete :
    ∀ op : Sm5Opcode, op ∈ Sm5Opcode.all := by
  intro op
  cases op <;> decide

-- ============================================================================
-- Theorem 4 (Phase B1): First-class IR pseudo-opcode contracts & lane projection
-- ============================================================================

theorem combineVec_pure_and_movable :
    (desc .combineVec).numDests = 1 ∧
    (desc .combineVec).canEliminateIfDead = true ∧
    (desc .combineVec).canMoveAcrossDivergentBranch = true ∧
    (desc .combineVec).canMoveAcrossUniformBranch = true ∧
    (desc .combineVec).requiresQuad = false ∧
    (desc .combineVec).slotRule = .perLaneScalar := by
  decide

-- Lane projection composition laws (basis for CombineVec projection passes).
theorem swizzle_compose (v : Vec4) (s1 s2 : Fin 4 → Fin 4) :
    swizzleVec (swizzleVec v s1) s2 = swizzleVec v (fun i => s1 (s2 i)) := by
  rfl

theorem project_combineVec (destMask : BitVec 4) (srcs : Nat → Vec4)
    (l : Fin 4) (hActive : destMask.getLsbD l.val = true) :
    evalOp .combineVec destMask srcs l = srcs l.val 0 := by
  unfold evalOp
  simp only [hActive, Bool.not_true, Bool.false_eq_true, ↓reduceIte]

end DXCB5.OpcodeSpec
