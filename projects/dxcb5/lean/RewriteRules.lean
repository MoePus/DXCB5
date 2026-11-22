-- Distributed under the University of Illinois Open Source License.
-- Formal specification and `bv_decide` soundness proofs for DXCB5's declarative
-- SM5 algebraic, bitwise, bitfield, compare, and select rewrite rules.
-- Exports `projects/dxcb5/lib/Mir/Combine/GeneratedRules.inc` via Codegen.lean.
import Std.Tactic.BVDecide
import OpcodeSpec

namespace DXCB5.RewriteRules

open DXCB5.OpcodeSpec

-- ============================================================================
-- 1. Explicit Semantic Mode (`SemMode`) and Rule Preconditions
-- ============================================================================

structure SemMode where
  nanSafe : Bool
  fastMath : Bool
  precise : Bool
  deriving DecidableEq, Repr, Inhabited

inductive ModeReq where
  | always
  | notPrecise
  | notPreciseNotNanSafe
  | notPreciseAndNonNaNF
  deriving DecidableEq, Repr, Inhabited

def ModeReq.satisfied (req : ModeReq) (m : SemMode) (nonNaNF : Bool := false) : Bool :=
  match req with
  | .always => true
  | .notPrecise => !m.precise
  | .notPreciseNotNanSafe => !m.precise && !m.nanSafe
  | .notPreciseAndNonNaNF => !m.precise && (!m.nanSafe || nonNaNF)

-- ============================================================================
-- 2. Exact SM5 32-bit Scalar Lane Semantics
-- ============================================================================

def kBoolTrue : BitVec 32 := 0xffffffff#32
def kBoolFalse : BitVec 32 := 0#32

def isBoolMask (m : BitVec 32) : Bool :=
  m == kBoolFalse || m == kBoolTrue

def sm5Bool (b : Bool) : BitVec 32 :=
  if b then kBoolTrue else kBoolFalse

def sm5Not (a : BitVec 32) : BitVec 32 := ~~~a
def sm5And (a b : BitVec 32) : BitVec 32 := a &&& b
def sm5Or (a b : BitVec 32) : BitVec 32 := a ||| b
def sm5Xor (a b : BitVec 32) : BitVec 32 := a ^^^ b
def sm5Iadd (a b : BitVec 32) : BitVec 32 := a + b
def sm5ImulLo (a b : BitVec 32) : BitVec 32 := a * b
def sm5Ishl (a s : BitVec 32) : BitVec 32 := a <<< (s &&& 31#32)
def sm5Ushr (a s : BitVec 32) : BitVec 32 := a >>> (s &&& 31#32)
def sm5Imin (a b : BitVec 32) : BitVec 32 := if a.slt b then a else b
def sm5Imax (a b : BitVec 32) : BitVec 32 := if a.slt b then b else a
def sm5Umin (a b : BitVec 32) : BitVec 32 := if a.ult b then a else b
def sm5Umax (a b : BitVec 32) : BitVec 32 := if a.ult b then b else a
def sm5Ieq (a b : BitVec 32) : BitVec 32 := sm5Bool (a == b)
def sm5Ine (a b : BitVec 32) : BitVec 32 := sm5Bool (a != b)
def sm5Ilt (a b : BitVec 32) : BitVec 32 := sm5Bool (a.slt b)
def sm5Ige (a b : BitVec 32) : BitVec 32 := sm5Bool (b.sle a)
def sm5Ult (a b : BitVec 32) : BitVec 32 := sm5Bool (a.ult b)
def sm5Uge (a b : BitVec 32) : BitVec 32 := sm5Bool (b.ule a)
def sm5Movc (c t f : BitVec 32) : BitVec 32 := if c != 0#32 then t else f

def fieldMask (w off : BitVec 32) : BitVec 32 :=
  if w == 0#32 then 0#32 else ((1#32 <<< w) - 1#32) <<< off

def sm5Ubfe (w off x : BitVec 32) : BitVec 32 :=
  let wb := w &&& 31#32
  let ob := off &&& 31#32
  if wb == 0#32 then 0#32 else (x >>> ob) &&& ((1#32 <<< wb) - 1#32)

def sm5Bfi (w off ins base : BitVec 32) : BitVec 32 :=
  let wb := w &&& 31#32
  let ob := off &&& 31#32
  let m := fieldMask wb ob
  ((ins <<< ob) &&& m) ||| (base &&& ~~~m)

def validBitfield (w off : BitVec 32) : Bool :=
  (1#32).ule w && w.ule 31#32 && off.ule 31#32 && (w + off).ule 32#32

-- ============================================================================
-- 3. Verified Rule Family A: Inverted Compare (`NOT(cmp(a, b)) -> inv(a, b)`)
-- ============================================================================

theorem not_ieq_eq_ine (a b : BitVec 32) : sm5Not (sm5Ieq a b) = sm5Ine a b := by
  unfold sm5Not sm5Ieq sm5Ine sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem not_ine_eq_ieq (a b : BitVec 32) : sm5Not (sm5Ine a b) = sm5Ieq a b := by
  unfold sm5Not sm5Ine sm5Ieq sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem not_ilt_eq_ige (a b : BitVec 32) : sm5Not (sm5Ilt a b) = sm5Ige a b := by
  unfold sm5Not sm5Ilt sm5Ige sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem not_ige_eq_ilt (a b : BitVec 32) : sm5Not (sm5Ige a b) = sm5Ilt a b := by
  unfold sm5Not sm5Ige sm5Ilt sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem not_ult_eq_uge (a b : BitVec 32) : sm5Not (sm5Ult a b) = sm5Uge a b := by
  unfold sm5Not sm5Ult sm5Uge sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem not_uge_eq_ult (a b : BitVec 32) : sm5Not (sm5Uge a b) = sm5Ult a b := by
  unfold sm5Not sm5Uge sm5Ult sm5Bool kBoolTrue kBoolFalse; bv_decide

structure InvCmpRule where
  srcOp : Sm5Opcode
  invOp : Sm5Opcode
  req : ModeReq
  deriving DecidableEq, Repr

def invCmpRules : List InvCmpRule := [
  ⟨.ieq, .ine, .always⟩,
  ⟨.ine, .ieq, .always⟩,
  ⟨.ilt, .ige, .always⟩,
  ⟨.ige, .ilt, .always⟩,
  ⟨.ult, .uge, .always⟩,
  ⟨.uge, .ult, .always⟩,
  -- IEEE-754: !(a == b) == (a != b) holds even for NaN; guarded by !precise.
  ⟨.eq,  .ne,  .notPrecise⟩,
  ⟨.ne,  .eq,  .notPrecise⟩,
  -- Ordered float compares: !(NaN < b) = true != (NaN >= b) = false, so
  -- requires !precise && !nanSafe.
  ⟨.lt,  .ge,  .notPreciseNotNanSafe⟩,
  ⟨.ge,  .lt,  .notPreciseNotNanSafe⟩
]

-- ============================================================================
-- 4. Verified Rule Family B: `MOVC(cmp(a, b), a, b) -> min/max(a, b)`
-- ============================================================================

theorem movc_ilt_direct (a b : BitVec 32) : sm5Movc (sm5Ilt a b) a b = sm5Imin a b := by
  unfold sm5Movc sm5Ilt sm5Imin sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem movc_ilt_swap (a b : BitVec 32) : sm5Movc (sm5Ilt a b) b a = sm5Imax a b := by
  unfold sm5Movc sm5Ilt sm5Imax sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem movc_ige_direct (a b : BitVec 32) : sm5Movc (sm5Ige a b) a b = sm5Imax a b := by
  unfold sm5Movc sm5Ige sm5Imax sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem movc_ige_swap (a b : BitVec 32) : sm5Movc (sm5Ige a b) b a = sm5Imin a b := by
  unfold sm5Movc sm5Ige sm5Imin sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem movc_ult_direct (a b : BitVec 32) : sm5Movc (sm5Ult a b) a b = sm5Umin a b := by
  unfold sm5Movc sm5Ult sm5Umin sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem movc_ult_swap (a b : BitVec 32) : sm5Movc (sm5Ult a b) b a = sm5Umax a b := by
  unfold sm5Movc sm5Ult sm5Umax sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem movc_uge_direct (a b : BitVec 32) : sm5Movc (sm5Uge a b) a b = sm5Umax a b := by
  unfold sm5Movc sm5Uge sm5Umax sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem movc_uge_swap (a b : BitVec 32) : sm5Movc (sm5Uge a b) b a = sm5Umin a b := by
  unfold sm5Movc sm5Uge sm5Umin sm5Bool kBoolTrue kBoolFalse; bv_decide

structure MovcMinMaxRule where
  cmpOp : Sm5Opcode
  directOp : Sm5Opcode
  swappedOp : Sm5Opcode
  isFloat : Bool
  req : ModeReq
  deriving DecidableEq, Repr

def movcMinMaxRules : List MovcMinMaxRule := [
  ⟨.ilt, .imin, .imax, false, .always⟩,
  ⟨.ige, .imax, .imin, false, .always⟩,
  ⟨.ult, .umin, .umax, false, .always⟩,
  ⟨.uge, .umax, .umin, false, .always⟩,
  -- SM5 min/max returns the non-NaN operand whereas movc(lt/ge, T, F) returns
  -- F on NaN; under nanSafe, folding requires F to be provably non-NaN.
  ⟨.lt,  .min,  .max,  true,  .notPreciseAndNonNaNF⟩,
  ⟨.ge,  .max,  .min,  true,  .notPreciseAndNonNaNF⟩
]

-- ============================================================================
-- 5. Verified Rule Family C: Bitwise, Integer, BoolMask & Select Identities
-- ============================================================================

theorem xor_allones_eq_not (x : BitVec 32) : sm5Xor x 0xffffffff#32 = sm5Not x := by
  unfold sm5Xor sm5Not; bv_decide

theorem xor_zero_eq_id (x : BitVec 32) : sm5Xor x 0#32 = x := by
  unfold sm5Xor; bv_decide

theorem not_not_eq_id (x : BitVec 32) : sm5Not (sm5Not x) = x := by
  unfold sm5Not; bv_decide

theorem iadd_zero_eq_id (x : BitVec 32) : sm5Iadd x 0#32 = x := by
  unfold sm5Iadd; bv_decide

theorem iadd_assoc_const (x c1 c2 : BitVec 32) :
    sm5Iadd (sm5Iadd x c1) c2 = sm5Iadd x (c1 + c2) := by
  unfold sm5Iadd; bv_decide

theorem and_allones_eq_id (x : BitVec 32) : sm5And x 0xffffffff#32 = x := by
  unfold sm5And; bv_decide

theorem and_zero_eq_zero (x : BitVec 32) : sm5And x 0#32 = 0#32 := by
  unfold sm5And; bv_decide

theorem and_self_eq_id (x : BitVec 32) : sm5And x x = x := by
  unfold sm5And; bv_decide

theorem or_zero_eq_id (x : BitVec 32) : sm5Or x 0#32 = x := by
  unfold sm5Or; bv_decide

theorem or_allones_eq_allones (x : BitVec 32) : sm5Or x 0xffffffff#32 = 0xffffffff#32 := by
  unfold sm5Or; bv_decide

theorem or_self_eq_id (x : BitVec 32) : sm5Or x x = x := by
  unfold sm5Or; bv_decide

theorem xor_bool_zext_eq_and_not (m : BitVec 32) :
    isBoolMask m = true → sm5Xor (sm5And m 1#32) 1#32 = sm5And (sm5Not m) 1#32 := by
  unfold isBoolMask kBoolFalse kBoolTrue sm5Xor sm5And sm5Not; bv_decide

theorem or_and_not_eq_movc (m t f : BitVec 32) :
    isBoolMask m = true →
    sm5Or (sm5And m t) (sm5And (sm5Not m) f) = sm5Movc m t f := by
  unfold isBoolMask kBoolFalse kBoolTrue sm5Or sm5And sm5Not sm5Movc; bv_decide

theorem movc_not_bool (m t f : BitVec 32) :
    isBoolMask m = true → sm5Movc (sm5Not m) t f = sm5Movc m f t := by
  unfold isBoolMask kBoolFalse kBoolTrue sm5Movc sm5Not; bv_decide

theorem movc_zext_bool (m t f : BitVec 32) :
    isBoolMask m = true → sm5Movc (sm5And m 1#32) t f = sm5Movc m t f := by
  unfold isBoolMask kBoolFalse kBoolTrue sm5Movc sm5And; bv_decide

theorem movc_bool_id (m : BitVec 32) :
    isBoolMask m = true → sm5Movc m kBoolTrue kBoolFalse = m := by
  unfold isBoolMask kBoolFalse kBoolTrue sm5Movc; bv_decide

theorem movc_bool_not (m : BitVec 32) :
    isBoolMask m = true → sm5Movc m kBoolFalse kBoolTrue = sm5Not m := by
  unfold isBoolMask kBoolFalse kBoolTrue sm5Movc sm5Not; bv_decide

theorem movc_same_arms (c x : BitVec 32) : sm5Movc c x x = x := by
  unfold sm5Movc; bv_decide

theorem movc_const_nonzero (c t f : BitVec 32) :
    c ≠ 0#32 → sm5Movc c t f = t := by
  unfold sm5Movc; bv_decide

theorem movc_const_zero (t f : BitVec 32) : sm5Movc 0#32 t f = f := by
  unfold sm5Movc; bv_decide

theorem movc_ine_zero (x t f : BitVec 32) :
    sm5Movc (sm5Ine x 0#32) t f = sm5Movc x t f := by
  unfold sm5Movc sm5Ine sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem movc_ieq_zero (x t f : BitVec 32) :
    sm5Movc (sm5Ieq x 0#32) t f = sm5Movc x f t := by
  unfold sm5Movc sm5Ieq sm5Bool kBoolTrue kBoolFalse; bv_decide

theorem ine_zext_bool_zero (m : BitVec 32) :
    isBoolMask m = true → sm5Ine (sm5And m 1#32) 0#32 = m := by
  unfold isBoolMask sm5Ine sm5And sm5Bool kBoolFalse kBoolTrue; bv_decide

theorem ieq_zext_bool_zero (m : BitVec 32) :
    isBoolMask m = true → sm5Ieq (sm5And m 1#32) 0#32 = sm5Not m := by
  unfold isBoolMask sm5Ieq sm5And sm5Not sm5Bool kBoolFalse kBoolTrue; bv_decide

theorem imul_zext_bool (a b : BitVec 32) :
    isBoolMask a = true → isBoolMask b = true →
    sm5ImulLo (sm5And a 1#32) (sm5And b 1#32) = sm5And (sm5And a 1#32) b := by
  simp only [isBoolMask, kBoolFalse, kBoolTrue, sm5ImulLo, sm5And]; bv_decide

theorem or_disjoint_eq_iadd (a b : BitVec 32) :
    (a &&& b) = 0#32 → sm5Or a b = sm5Iadd a b := by
  simp only [sm5Or, sm5Iadd]; bv_decide

-- ============================================================================
-- 6. Verified Rule Family D: Power-of-Two `UDIV`/`UREM` & SM5 Bitfields
-- ============================================================================

theorem udiv_pow2_eq_ushr (x : BitVec 32) (k : Nat) :
    x.toNat / (2 ^ k) = (x >>> k).toNat := by
  simp [BitVec.toNat_ushiftRight, Nat.shiftRight_eq_div_pow]

theorem urem_pow2_eq_and (x : BitVec 32) (k : Nat) (hk : k < 32) :
    x.toNat % (2 ^ k) = (x &&& ((1#32 <<< k) - 1#32)).toNat := by
  have hpow : 2 ^ k < 2 ^ 32 := Nat.pow_lt_pow_right (by omega) hk
  have h1 : (1#32 <<< k).toNat = 2 ^ k := by
    simp [BitVec.toNat_shiftLeft, Nat.shiftLeft_eq, Nat.mod_eq_of_lt hpow]
  have h2 : ((1#32 <<< k) - 1#32).toNat = 2 ^ k - 1 := by
    rw [BitVec.toNat_sub, h1]
    have hpos : 1 ≤ 2 ^ k := Nat.one_le_two_pow
    have hlt : 2 ^ k - 1 < 2 ^ 32 := by omega
    have : (2 ^ 32 - 1 + 2 ^ k) % 2 ^ 32 = 2 ^ k - 1 := by omega
    simpa [BitVec.toNat_ofNat] using this
  rw [BitVec.toNat_and, h2, Nat.and_two_pow_sub_one_eq_mod]

theorem and_ushr_eq_ubfe (x w s : BitVec 32) :
    validBitfield w s = true → (1#32).ule s = true →
    sm5And (sm5Ushr x s) (fieldMask w 0#32) = sm5Ubfe w s x := by
  simp only [validBitfield, sm5And, sm5Ushr, sm5Ubfe, fieldMask]; bv_decide

theorem ushr_and_eq_ubfe (x w s : BitVec 32) :
    validBitfield w s = true → (1#32).ule s = true →
    sm5Ushr (sm5And x (fieldMask w s)) s = sm5Ubfe w s x := by
  simp only [validBitfield, sm5Ushr, sm5And, sm5Ubfe, fieldMask]; bv_decide

theorem and_ishl_eq_bfi0 (x w s : BitVec 32) :
    validBitfield w s = true → (1#32).ule s = true →
    sm5And (sm5Ishl x s) (fieldMask w s) = sm5Bfi w s x 0#32 := by
  simp only [validBitfield, sm5And, sm5Ishl, sm5Bfi, fieldMask]; bv_decide

theorem ishl_and_eq_bfi0 (x w s : BitVec 32) :
    validBitfield w s = true → (1#32).ule s = true →
    sm5Ishl (sm5And x (fieldMask w 0#32)) s = sm5Bfi w s x 0#32 := by
  simp only [validBitfield, sm5Ishl, sm5And, sm5Bfi, fieldMask]; bv_decide

theorem or_bfi0_and_inv_eq_bfi (v base w s : BitVec 32) :
    validBitfield w s = true →
    sm5Or (sm5Bfi w s v 0#32) (sm5And base (~~~(fieldMask w s))) = sm5Bfi w s v base := by
  simp only [validBitfield, sm5Or, sm5Bfi, sm5And, fieldMask]; bv_decide

theorem or_bfi0_clear_base_eq_bfi (v base w s : BitVec 32) :
    validBitfield w s = true → (base &&& fieldMask w s) = 0#32 →
    sm5Or (sm5Bfi w s v 0#32) base = sm5Bfi w s v base := by
  simp only [validBitfield, sm5Or, sm5Bfi, fieldMask]; bv_decide

theorem or_ishl_and_inv_eq_bfi (v base s : BitVec 32) :
    (1#32).ule s = true → s.ule 31#32 = true →
    sm5Or (sm5Ishl v s) (sm5And base (~~~(fieldMask (32#32 - s) s))) =
      sm5Bfi (32#32 - s) s v base := by
  simp only [sm5Or, sm5Ishl, sm5And, sm5Bfi, fieldMask]; bv_decide

theorem or_and_and_inv_eq_bfi (v base w : BitVec 32) :
    (1#32).ule w = true → w.ule 31#32 = true →
    sm5Or (sm5And v (fieldMask w 0#32)) (sm5And base (~~~(fieldMask w 0#32))) =
      sm5Bfi w 0#32 v base := by
  simp only [sm5Or, sm5And, sm5Bfi, fieldMask]; bv_decide

-- ============================================================================
-- 7. Declarative Local Rule Table (Exported to C++ `GeneratedRules.inc`)
-- ============================================================================

inductive LocalRuleAction where
  | movOtherSide         -- Replace binary op with MOV(otherSide)
  | movConstImm          -- Replace binary op with MOV(immValue)
  | unaryOpOtherSide     -- Replace binary op with unaryOp(otherSide)
  deriving DecidableEq, Repr

structure BinaryImmRule where
  name : String
  opcode : Sm5Opcode
  matchImm : UInt32
  action : LocalRuleAction
  outOp : Sm5Opcode
  outImm : UInt32
  req : ModeReq
  deriving DecidableEq, Repr

def binaryImmRules : List BinaryImmRule := [
  ⟨"xor_allones_eq_not",    .xor,  0xffffffff, .unaryOpOtherSide, .not, 0,          .always⟩,
  ⟨"xor_zero_eq_id",        .xor,  0x00000000, .movOtherSide,     .mov, 0,          .always⟩,
  ⟨"iadd_zero_eq_id",       .iadd, 0x00000000, .movOtherSide,     .mov, 0,          .always⟩,
  ⟨"and_allones_eq_id",     .and,  0xffffffff, .movOtherSide,     .mov, 0,          .always⟩,
  ⟨"and_zero_eq_zero",      .and,  0x00000000, .movConstImm,      .mov, 0x00000000, .always⟩,
  ⟨"or_zero_eq_id",         .or,   0x00000000, .movOtherSide,     .mov, 0,          .always⟩,
  ⟨"or_allones_eq_allones", .or,   0xffffffff, .movConstImm,      .mov, 0xffffffff, .always⟩
]

end DXCB5.RewriteRules
