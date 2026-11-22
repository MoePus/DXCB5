-- Distributed under the University of Illinois Open Source License.
-- Formal specification and soundness proofs for DXCB5's per-lane abstract
-- domain (reduced product of ConstBits, SignedRange, UnsignedRange, BoolMask),
-- branch-edge refinement, loop-header widening & budget-exhaustion fallback
-- contract (Codex #3), and backward DemandedLanes transfer soundness.
import Std.Tactic.BVDecide
import OpcodeSpec

namespace DXCB5.LaneDomain

open DXCB5.OpcodeSpec

-- ============================================================================
-- 1. Signed & Unsigned 32-bit interval domains
-- ============================================================================

def kInt32Min : BitVec 32 := 0x80000000#32
def kInt32Max : BitVec 32 := 0x7fffffff#32
def kUInt32Min : BitVec 32 := 0x00000000#32
def kUInt32Max : BitVec 32 := 0xffffffff#32

structure SignedRange where
  lo : BitVec 32
  hi : BitVec 32
  deriving DecidableEq, Repr

namespace SignedRange

def top : SignedRange := ⟨kInt32Min, kInt32Max⟩
def single (v : BitVec 32) : SignedRange := ⟨v, v⟩

def contains (r : SignedRange) (x : BitVec 32) : Bool :=
  r.lo.sle x && x.sle r.hi

def isEmpty (r : SignedRange) : Bool :=
  r.hi.slt r.lo

def join (a b : SignedRange) : SignedRange :=
  ⟨if a.lo.sle b.lo then a.lo else b.lo,
   if a.hi.sle b.hi then b.hi else a.hi⟩

def meet (a b : SignedRange) : SignedRange :=
  ⟨if a.lo.sle b.lo then b.lo else a.lo,
   if a.hi.sle b.hi then a.hi else b.hi⟩

-- Codex #3 loop-header widening operator (∇):
-- Once past the widening threshold, any decreasing lower bound jumps straight
-- to INT32_MIN and any increasing upper bound jumps straight to INT32_MAX.
def widen (old new : SignedRange) : SignedRange :=
  ⟨if new.lo.slt old.lo then kInt32Min else old.lo,
   if old.hi.slt new.hi then kInt32Max else old.hi⟩

theorem top_contains (x : BitVec 32) : top.contains x = true := by
  unfold top contains kInt32Min kInt32Max
  bv_decide

theorem single_contains (v : BitVec 32) : (single v).contains v = true := by
  unfold single contains
  bv_decide

theorem join_sound_left (a b : SignedRange) (x : BitVec 32) :
    a.contains x = true → (join a b).contains x = true := by
  unfold contains join
  bv_decide

theorem join_sound_right (a b : SignedRange) (x : BitVec 32) :
    b.contains x = true → (join a b).contains x = true := by
  unfold contains join
  bv_decide

theorem meet_sound (a b : SignedRange) (x : BitVec 32) :
    a.contains x = true → b.contains x = true → (meet a b).contains x = true := by
  unfold contains meet
  bv_decide

theorem widen_sound_left (old new : SignedRange) (x : BitVec 32) :
    old.contains x = true → (widen old new).contains x = true := by
  unfold contains widen kInt32Min kInt32Max
  bv_decide

theorem widen_sound_right (old new : SignedRange) (x : BitVec 32) :
    new.contains x = true → (widen old new).contains x = true := by
  unfold contains widen kInt32Min kInt32Max
  bv_decide

-- Ascending-chain termination measure: `widen` strictly decreases the number of
-- non-extremal endpoints (at most 2 steps to reach fixpoint on any loop PHI).
def nonExtremalCount (r : SignedRange) : Nat :=
  (if r.lo == kInt32Min then 0 else 1) + (if r.hi == kInt32Max then 0 else 1)

theorem widen_strict_progress (old new : SignedRange)
    (hNeq : widen old new ≠ old) :
    nonExtremalCount (widen old new) < nonExtremalCount old := by
  unfold nonExtremalCount widen
  by_cases h1 : new.lo.slt old.lo = true <;> by_cases h2 : old.hi.slt new.hi = true
  · have hlo : old.lo ≠ kInt32Min := by
      intro heq; rw [heq] at h1; revert h1; unfold kInt32Min; bv_decide
    have hhi : old.hi ≠ kInt32Max := by
      intro heq; rw [heq] at h2; revert h2; unfold kInt32Max; bv_decide
    simp [h1, h2, hlo, hhi]
  · have hlo : old.lo ≠ kInt32Min := by
      intro heq; rw [heq] at h1; revert h1; unfold kInt32Min; bv_decide
    simp [h1, h2, hlo]
  · have hhi : old.hi ≠ kInt32Max := by
      intro heq; rw [heq] at h2; revert h2; unfold kInt32Max; bv_decide
    simp [h1, h2, hhi]
  · exfalso; apply hNeq; simp [widen, h1, h2]

end SignedRange

structure UnsignedRange where
  lo : BitVec 32
  hi : BitVec 32
  deriving DecidableEq, Repr

namespace UnsignedRange

def top : UnsignedRange := ⟨kUInt32Min, kUInt32Max⟩
def single (v : BitVec 32) : UnsignedRange := ⟨v, v⟩

def contains (r : UnsignedRange) (x : BitVec 32) : Bool :=
  r.lo.ule x && x.ule r.hi

def join (a b : UnsignedRange) : UnsignedRange :=
  ⟨if a.lo.ule b.lo then a.lo else b.lo,
   if a.hi.ule b.hi then b.hi else a.hi⟩

def meet (a b : UnsignedRange) : UnsignedRange :=
  ⟨if a.lo.ule b.lo then b.lo else a.lo,
   if a.hi.ule b.hi then a.hi else b.hi⟩

def widen (old new : UnsignedRange) : UnsignedRange :=
  ⟨if new.lo.ult old.lo then kUInt32Min else old.lo,
   if old.hi.ult new.hi then kUInt32Max else old.hi⟩

theorem top_contains (x : BitVec 32) : top.contains x = true := by
  unfold top contains kUInt32Min kUInt32Max
  bv_decide

theorem join_sound_left (a b : UnsignedRange) (x : BitVec 32) :
    a.contains x = true → (join a b).contains x = true := by
  unfold contains join
  bv_decide

theorem join_sound_right (a b : UnsignedRange) (x : BitVec 32) :
    b.contains x = true → (join a b).contains x = true := by
  unfold contains join
  bv_decide

theorem meet_sound (a b : UnsignedRange) (x : BitVec 32) :
    a.contains x = true → b.contains x = true → (meet a b).contains x = true := by
  unfold contains meet
  bv_decide

theorem widen_sound_left (old new : UnsignedRange) (x : BitVec 32) :
    old.contains x = true → (widen old new).contains x = true := by
  unfold contains widen kUInt32Min kUInt32Max
  bv_decide

theorem widen_sound_right (old new : UnsignedRange) (x : BitVec 32) :
    new.contains x = true → (widen old new).contains x = true := by
  unfold contains widen kUInt32Min kUInt32Max
  bv_decide

def nonExtremalCount (r : UnsignedRange) : Nat :=
  (if r.lo == kUInt32Min then 0 else 1) + (if r.hi == kUInt32Max then 0 else 1)

theorem widen_strict_progress (old new : UnsignedRange)
    (hNeq : widen old new ≠ old) :
    nonExtremalCount (widen old new) < nonExtremalCount old := by
  unfold nonExtremalCount widen
  by_cases h1 : new.lo.ult old.lo = true <;> by_cases h2 : old.hi.ult new.hi = true
  · have hlo : old.lo ≠ kUInt32Min := by
      intro heq; rw [heq] at h1; revert h1; unfold kUInt32Min; bv_decide
    have hhi : old.hi ≠ kUInt32Max := by
      intro heq; rw [heq] at h2; revert h2; unfold kUInt32Max; bv_decide
    simp [h1, h2, hlo, hhi]
  · have hlo : old.lo ≠ kUInt32Min := by
      intro heq; rw [heq] at h1; revert h1; unfold kUInt32Min; bv_decide
    simp [h1, h2, hlo]
  · have hhi : old.hi ≠ kUInt32Max := by
      intro heq; rw [heq] at h2; revert h2; unfold kUInt32Max; bv_decide
    simp [h1, h2, hhi]
  · exfalso; apply hNeq; simp [widen, h1, h2]

end UnsignedRange

-- ============================================================================
-- 2. Reduced Product `LaneFact` & Soundness of Lattice + Transfer Functions
-- ============================================================================

structure LaneFact where
  constBits : Option (BitVec 32) := none
  sRange : SignedRange := SignedRange.top
  uRange : UnsignedRange := UnsignedRange.top
  isBoolMask : Bool := false
  deriving DecidableEq, Repr

namespace LaneFact

def top : LaneFact :=
  { constBits := none, sRange := SignedRange.top, uRange := UnsignedRange.top,
    isBoolMask := false }

def fromConst (c : BitVec 32) : LaneFact :=
  { constBits := some c
    sRange := SignedRange.single c
    uRange := UnsignedRange.single c
    isBoolMask := (c == 0#32) || (c == 0xffffffff#32) }

def gamma (f : LaneFact) (x : BitVec 32) : Prop :=
  (∀ c, f.constBits = some c → x = c) ∧
  f.sRange.contains x = true ∧
  f.uRange.contains x = true ∧
  (f.isBoolMask = true → x = 0#32 ∨ x = 0xffffffff#32)

-- Budget-exhaustion fallback contract (Codex #3):
-- Reverting any unconverged loop-header PHI or dependent value to `top` is
-- unconditionally sound for all concrete 32-bit values.
theorem top_sound (x : BitVec 32) : top.gamma x := by
  refine ⟨(fun _ h => nomatch h), SignedRange.top_contains x, UnsignedRange.top_contains x,
          (fun h => nomatch h)⟩

theorem fromConst_sound (c : BitVec 32) : (fromConst c).gamma c := by
  refine ⟨(fun _ h => Option.some.inj h),
          SignedRange.single_contains c, ?_, ?_⟩
  · unfold fromConst UnsignedRange.single UnsignedRange.contains; bv_decide
  · intro h
    unfold fromConst at h
    simp only [Bool.or_eq_true, beq_iff_eq] at h
    exact h

def join (a b : LaneFact) : LaneFact :=
  { constBits := if a.constBits == b.constBits then a.constBits else none
    sRange := SignedRange.join a.sRange b.sRange
    uRange := UnsignedRange.join a.uRange b.uRange
    isBoolMask := a.isBoolMask && b.isBoolMask }

def widen (old new : LaneFact) : LaneFact :=
  { constBits := if old.constBits == new.constBits then old.constBits else none
    sRange := SignedRange.widen old.sRange new.sRange
    uRange := UnsignedRange.widen old.uRange new.uRange
    isBoolMask := old.isBoolMask && new.isBoolMask }

theorem join_sound_left (a b : LaneFact) (x : BitVec 32) :
    a.gamma x → (join a b).gamma x := by
  rintro ⟨hc, hs, hu, hb⟩
  refine ⟨?_, SignedRange.join_sound_left _ _ _ hs,
          UnsignedRange.join_sound_left _ _ _ hu, ?_⟩
  · intro c hEq
    unfold join at hEq
    split at hEq
    · exact hc c hEq
    · cases hEq
  · intro hbm
    unfold join at hbm
    simp only [Bool.and_eq_true] at hbm
    exact hb hbm.1

theorem join_sound_right (a b : LaneFact) (x : BitVec 32) :
    b.gamma x → (join a b).gamma x := by
  rintro ⟨hc, hs, hu, hb⟩
  refine ⟨?_, SignedRange.join_sound_right _ _ _ hs,
          UnsignedRange.join_sound_right _ _ _ hu, ?_⟩
  · intro c hEq
    unfold join at hEq
    split at hEq
    · rename_i hSame
      have hSame' : a.constBits = b.constBits := beq_iff_eq.mp hSame
      exact hc c (hSame' ▸ hEq)
    · cases hEq
  · intro hbm
    unfold join at hbm
    simp only [Bool.and_eq_true] at hbm
    exact hb hbm.2

theorem widen_sound_left (old new : LaneFact) (x : BitVec 32) :
    old.gamma x → (widen old new).gamma x := by
  rintro ⟨hc, hs, hu, hb⟩
  refine ⟨?_, SignedRange.widen_sound_left _ _ _ hs,
          UnsignedRange.widen_sound_left _ _ _ hu, ?_⟩
  · intro c hEq
    unfold widen at hEq
    split at hEq
    · exact hc c hEq
    · cases hEq
  · intro hbm
    unfold widen at hbm
    simp only [Bool.and_eq_true] at hbm
    exact hb hbm.1

theorem widen_sound_right (old new : LaneFact) (x : BitVec 32) :
    new.gamma x → (widen old new).gamma x := by
  rintro ⟨hc, hs, hu, hb⟩
  refine ⟨?_, SignedRange.widen_sound_right _ _ _ hs,
          UnsignedRange.widen_sound_right _ _ _ hu, ?_⟩
  · intro c hEq
    unfold widen at hEq
    split at hEq
    · rename_i hSame
      have hSame' : old.constBits = new.constBits := beq_iff_eq.mp hSame
      exact hc c (hSame' ▸ hEq)
    · cases hEq
  · intro hbm
    unfold widen at hbm
    simp only [Bool.and_eq_true] at hbm
    exact hb hbm.2

end LaneFact

-- ============================================================================
-- 3. SM5 Arithmetic & Comparison Transfer Functions (bv_decide verified)
-- ============================================================================

def sm5Imin (a b : BitVec 32) : BitVec 32 := if a.sle b then a else b
def sm5Imax (a b : BitVec 32) : BitVec 32 := if a.sle b then b else a
def sm5Umin (a b : BitVec 32) : BitVec 32 := if a.ule b then a else b
def sm5Umax (a b : BitVec 32) : BitVec 32 := if a.ule b then b else a

def evalIminRange (a b : SignedRange) : SignedRange :=
  ⟨sm5Imin a.lo b.lo, sm5Imin a.hi b.hi⟩

def evalImaxRange (a b : SignedRange) : SignedRange :=
  ⟨sm5Imax a.lo b.lo, sm5Imax a.hi b.hi⟩

def evalUminRange (a b : UnsignedRange) : UnsignedRange :=
  ⟨sm5Umin a.lo b.lo, sm5Umin a.hi b.hi⟩

def evalUmaxRange (a b : UnsignedRange) : UnsignedRange :=
  ⟨sm5Umax a.lo b.lo, sm5Umax a.hi b.hi⟩

-- Overflow-checked signed addition in 64-bit sign-extended arithmetic.
def evalIaddRange (a b : SignedRange) : SignedRange :=
  let lo64 : BitVec 64 := a.lo.signExtend 64 + b.lo.signExtend 64
  let hi64 : BitVec 64 := a.hi.signExtend 64 + b.hi.signExtend 64
  let min64 : BitVec 64 := kInt32Min.signExtend 64
  let max64 : BitVec 64 := kInt32Max.signExtend 64
  if min64.sle lo64 && lo64.sle max64 && min64.sle hi64 && hi64.sle max64 then
    ⟨a.lo + b.lo, a.hi + b.hi⟩
  else
    SignedRange.top

def evalAndUnsigned (a b : UnsignedRange) : UnsignedRange :=
  ⟨0#32, sm5Umin a.hi b.hi⟩

theorem evalIminRange_sound (a b : SignedRange) (x y : BitVec 32) :
    a.contains x = true → b.contains y = true →
    (evalIminRange a b).contains (sm5Imin x y) = true := by
  unfold SignedRange.contains evalIminRange sm5Imin
  bv_decide

theorem evalImaxRange_sound (a b : SignedRange) (x y : BitVec 32) :
    a.contains x = true → b.contains y = true →
    (evalImaxRange a b).contains (sm5Imax x y) = true := by
  unfold SignedRange.contains evalImaxRange sm5Imax
  bv_decide

theorem evalUminRange_sound (a b : UnsignedRange) (x y : BitVec 32) :
    a.contains x = true → b.contains y = true →
    (evalUminRange a b).contains (sm5Umin x y) = true := by
  unfold UnsignedRange.contains evalUminRange sm5Umin
  bv_decide

theorem evalUmaxRange_sound (a b : UnsignedRange) (x y : BitVec 32) :
    a.contains x = true → b.contains y = true →
    (evalUmaxRange a b).contains (sm5Umax x y) = true := by
  unfold UnsignedRange.contains evalUmaxRange sm5Umax
  bv_decide

theorem evalIaddRange_sound (a b : SignedRange) (x y : BitVec 32) :
    a.contains x = true → b.contains y = true →
    (evalIaddRange a b).contains (x + y) = true := by
  simp only [SignedRange.contains, evalIaddRange, SignedRange.top, kInt32Min, kInt32Max]
  bv_decide

theorem evalAndUnsigned_sound (a b : UnsignedRange) (x y : BitVec 32) :
    a.contains x = true → b.contains y = true →
    (evalAndUnsigned a b).contains (x &&& y) = true := by
  unfold UnsignedRange.contains evalAndUnsigned sm5Umin
  bv_decide

-- Signed comparison folding from SignedRange
def evalIltRange (a b : SignedRange) : Option (BitVec 32) :=
  if a.hi.slt b.lo then some 0xffffffff#32
  else if b.hi.sle a.lo then some 0#32
  else none

def evalIgeRange (a b : SignedRange) : Option (BitVec 32) :=
  if b.hi.sle a.lo then some 0xffffffff#32
  else if a.hi.slt b.lo then some 0#32
  else none

def evalIeqRange (a b : SignedRange) : Option (BitVec 32) :=
  if a.lo == a.hi && b.lo == b.hi && a.lo == b.lo then some 0xffffffff#32
  else if a.hi.slt b.lo || b.hi.slt a.lo then some 0#32
  else none

def evalUltRange (a b : UnsignedRange) : Option (BitVec 32) :=
  if a.hi.ult b.lo then some 0xffffffff#32
  else if b.hi.ule a.lo then some 0#32
  else none

theorem evalIltRange_sound (a b : SignedRange) (x y c : BitVec 32) :
    a.contains x = true → b.contains y = true →
    evalIltRange a b = some c →
    (if x.slt y then 0xffffffff#32 else 0#32) = c := by
  intro hx hy hEq
  unfold evalIltRange at hEq
  split at hEq
  · injection hEq with hc; subst hc
    rename_i h1; revert hx hy h1; unfold SignedRange.contains; bv_decide
  · split at hEq
    · injection hEq with hc; subst hc
      rename_i h1 h2; revert hx hy h2; unfold SignedRange.contains; bv_decide
    · cases hEq

theorem evalIgeRange_sound (a b : SignedRange) (x y c : BitVec 32) :
    a.contains x = true → b.contains y = true →
    evalIgeRange a b = some c →
    (if y.sle x then 0xffffffff#32 else 0#32) = c := by
  intro hx hy hEq
  unfold evalIgeRange at hEq
  split at hEq
  · injection hEq with hc; subst hc
    rename_i h1; revert hx hy h1; unfold SignedRange.contains; bv_decide
  · split at hEq
    · injection hEq with hc; subst hc
      rename_i h1 h2; revert hx hy h2; unfold SignedRange.contains; bv_decide
    · cases hEq

theorem evalIeqRange_sound (a b : SignedRange) (x y c : BitVec 32) :
    a.contains x = true → b.contains y = true →
    evalIeqRange a b = some c →
    (if x == y then 0xffffffff#32 else 0#32) = c := by
  intro hx hy hEq
  unfold evalIeqRange at hEq
  split at hEq
  · injection hEq with hc; subst hc
    rename_i h1; revert hx hy h1; unfold SignedRange.contains; bv_decide
  · split at hEq
    · injection hEq with hc; subst hc
      rename_i h1 h2; revert hx hy h2; unfold SignedRange.contains; bv_decide
    · cases hEq

theorem evalUltRange_sound (a b : UnsignedRange) (x y c : BitVec 32) :
    a.contains x = true → b.contains y = true →
    evalUltRange a b = some c →
    (if x.ult y then 0xffffffff#32 else 0#32) = c := by
  intro hx hy hEq
  unfold evalUltRange at hEq
  split at hEq
  · injection hEq with hc; subst hc
    rename_i h1; revert hx hy h1; unfold UnsignedRange.contains; bv_decide
  · split at hEq
    · injection hEq with hc; subst hc
      rename_i h1 h2; revert hx hy h2; unfold UnsignedRange.contains; bv_decide
    · cases hEq

-- ============================================================================
-- 4. Branch-Edge Refinement Soundness (INT32_MIN / INT32_MAX / 0 safe)
-- ============================================================================

inductive CmpPred where
  | ilt | ige | ieq | ine
  deriving DecidableEq, Repr

def evalPredBits (p : CmpPred) (lhs rhs : BitVec 32) : Bool :=
  match p with
  | .ilt => lhs.slt rhs
  | .ige => rhs.sle lhs
  | .ieq => lhs == rhs
  | .ine => lhs != rhs

def refineSignedEdge (p : CmpPred) (k : BitVec 32) (constOnLhs : Bool) : SignedRange :=
  match p, constOnLhs with
  | .ieq, _ => SignedRange.single k
  | .ilt, false => if k != kInt32Min then ⟨kInt32Min, k - 1#32⟩ else SignedRange.top
  | .ige, false => ⟨k, kInt32Max⟩
  | .ilt, true  => if k != kInt32Max then ⟨k + 1#32, kInt32Max⟩ else SignedRange.top
  | .ige, true  => ⟨kInt32Min, k⟩
  | .ine, _     => SignedRange.top

theorem refineSignedEdge_sound (p : CmpPred) (x k : BitVec 32) (constOnLhs : Bool) :
    (if constOnLhs then evalPredBits p k x else evalPredBits p x k) = true →
    (refineSignedEdge p k constOnLhs).contains x = true := by
  cases p <;> cases constOnLhs <;>
    simp only [evalPredBits, refineSignedEdge, SignedRange.single, SignedRange.top,
               SignedRange.contains, kInt32Min, kInt32Max] <;>
    bv_decide

def refineUnsignedLtConst (k : BitVec 32) : UnsignedRange :=
  if k != 0#32 then ⟨0#32, k - 1#32⟩ else UnsignedRange.top

theorem refineUnsignedLtConst_sound (x k : BitVec 32) :
    x.ult k = true → (refineUnsignedLtConst k).contains x = true := by
  unfold refineUnsignedLtConst UnsignedRange.top UnsignedRange.contains kUInt32Min kUInt32Max
  bv_decide

-- ============================================================================
-- 5. BoolMask Transfer Soundness
-- ============================================================================

def isBoolBitVec (x : BitVec 32) : Prop := x = 0#32 ∨ x = 0xffffffff#32

theorem boolMask_not_sound (x : BitVec 32) :
    isBoolBitVec x → isBoolBitVec (~~~x) := by
  rintro (rfl | rfl) <;> (unfold isBoolBitVec; decide)

theorem boolMask_and_sound (x y : BitVec 32) :
    isBoolBitVec x → isBoolBitVec y → isBoolBitVec (x &&& y) := by
  rintro (rfl | rfl) (rfl | rfl) <;> (unfold isBoolBitVec; decide)

theorem boolMask_or_sound (x y : BitVec 32) :
    isBoolBitVec x → isBoolBitVec y → isBoolBitVec (x ||| y) := by
  rintro (rfl | rfl) (rfl | rfl) <;> (unfold isBoolBitVec; decide)

theorem boolMask_xor_sound (x y : BitVec 32) :
    isBoolBitVec x → isBoolBitVec y → isBoolBitVec (x ^^^ y) := by
  rintro (rfl | rfl) (rfl | rfl) <;> (unfold isBoolBitVec; decide)

theorem boolMask_movc_sound (c x y : BitVec 32) :
    isBoolBitVec x → isBoolBitVec y →
    isBoolBitVec (if c != 0#32 then x else y) := by
  intro hx hy
  by_cases hc : c != 0#32
  · simp [hc, hx]
  · simp [hc, hy]

-- ============================================================================
-- 6. Codex #3 Concrete Counterexample Verification:
--    For `i = φ(0, i + 1)`, widening [0, 0] with [0, 1] produces [0, INT32_MAX],
--    which contains 4 and does NOT fold `ilt(i, 4)` to constant true!
-- ============================================================================

theorem loop_inc_widen_preserves_iter4 :
    let r0 := SignedRange.single 0#32
    let r1 := evalIaddRange r0 (SignedRange.single 1#32)
    let rW := SignedRange.widen r0 (SignedRange.join r0 r1)
    rW = ⟨0#32, kInt32Max⟩ ∧
    rW.contains 4#32 = true ∧
    evalIltRange rW (SignedRange.single 4#32) = none := by
  decide

-- ============================================================================
-- 7. Backward DemandedLanes Transfer Soundness (built on OpcodeSpec.evalOp)
-- ============================================================================

def usesDemandedDestMask (op : Sm5Opcode) : Bool :=
  (desc op).isComponentwise || ((desc op).slotRule == SourceSlotRule.perLaneScalar)

def demandedForSrc (op : Sm5Opcode) (destMask demandedDest : BitVec 4)
    (srcIdx : Nat) (swz : Fin 4 → Fin 4) : BitVec 4 :=
  let activeDest :=
    if usesDemandedDestMask op then destMask &&& demandedDest else destMask
  sourceReadLanes op activeDest srcIdx swz

-- Soundness of backward demanded-lanes propagation: modifying any source lanes
-- outside `demandedForSrc` cannot change any demanded output lane of `evalOp`.
theorem demandedForSrc_sound (op : Sm5Opcode) (destMask demandedDest : BitVec 4)
    (srcs : Nat → Vec4) (srcIdx : Nat) (swz : Fin 4 → Fin 4) (a b : Vec4)
    (hEq : ∀ lane : Fin 4,
      (demandedForSrc op destMask demandedDest srcIdx swz).getLsbD lane.val = true →
      b lane = a lane) :
    ∀ outLane : Fin 4, (destMask &&& demandedDest).getLsbD outLane.val = true →
      evalOp op destMask (replaceSrc srcs srcIdx (swizzleVec b swz)) outLane =
      evalOp op destMask (replaceSrc srcs srcIdx (swizzleVec a swz)) outLane := by
  intro outLane hOut
  have hDest : destMask.getLsbD outLane.val = true := by
    simp only [BitVec.getLsbD_and, Bool.and_eq_true] at hOut
    exact hOut.1
  by_cases hDem : usesDemandedDestMask op = true
  · have hEq' : ∀ lane : Fin 4,
        (sourceReadLanes op (destMask &&& demandedDest) srcIdx swz).getLsbD lane.val = true →
        b lane = a lane := by
      intro lane hl
      apply hEq lane
      unfold demandedForSrc
      simp [hDem, hl]
    have hSound := sourceReadLanes_sound op (destMask &&& demandedDest) srcs srcIdx swz a b hEq' outLane hOut
    -- For componentwise and perLaneScalar (.combineVec) ops, `evalOp op destMask`
    -- and `evalOp op (destMask &&& demandedDest)` agree on every lane where both
    -- masks are 1 (`hDest` and `hOut`).
    cases op <;> simp_all [usesDemandedDestMask, desc, isComponentwiseOp, slotRuleOp, evalOp]
  · have hDemF : usesDemandedDestMask op = false := Bool.eq_false_iff.mpr hDem
    have hEq' : ∀ lane : Fin 4,
        (sourceReadLanes op destMask srcIdx swz).getLsbD lane.val = true →
        b lane = a lane := by
      intro lane hl
      apply hEq lane
      unfold demandedForSrc
      simp [hDemF, hl]
    exact sourceReadLanes_sound op destMask srcs srcIdx swz a b hEq' outLane hDest

end DXCB5.LaneDomain
