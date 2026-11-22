-- Distributed under the University of Illinois Open Source License.
-- Formal specification and termination/monotonicity proofs for the unified
-- code-motion acceptance rule (PlacementStore) shared by LICM, ColdSink, and
-- PressureShrink (Phase C1 / R4).

namespace DXCB5.Placement

inductive Owner where
  | none
  | licm
  | coldSink
  | pressureShrink
  deriving DecidableEq, Repr

/-- Dynamic execution weight of a block at loop nesting depth `d`, matching
    `Oracle.cpp::ComputeDynamicCost` bit-for-bit (`8 ^ min d 6`). -/
def blockWeight (depth : Nat) : Nat :=
  8 ^ min depth 6

theorem blockWeight_pos (d : Nat) : 0 < blockWeight d := by
  unfold blockWeight
  exact Nat.pow_pos (by decide : 0 < 8)

theorem blockWeight_strict_mono {d1 d2 : Nat} (hLt : d1 < d2) (hCap : d1 < 6) :
    blockWeight d1 < blockWeight d2 := by
  unfold blockWeight
  have hMin : min d1 6 < min d2 6 := by omega
  exact Nat.pow_lt_pow_right (by decide : 1 < 8) hMin

/-- Sum of unweighted Oracle block weights across destination blocks. -/
def oracleCostSum : List Nat -> Nat
  | [] => 0
  | d :: ds => blockWeight d + oracleCostSum ds

/-- Sum of branch-expected weights (scaled by 1 if bypassable cold branch,
    2 otherwise), refining same-depth ColdSink ties when `oracleMu` is equal. -/
def expectedCostSum (bypassable : Bool) : List Nat -> Nat
  | [] => 0
  | d :: ds => (if bypassable then 1 else 2) * blockWeight d + expectedCostSum bypassable ds

structure Move where
  owner : Owner
  srcDepth : Nat
  dstDepths : List Nat
  keepSrc : Bool
  bypassable : Bool
  plBefore : Nat
  plAfter : Nat
  budget : Nat
  deriving Repr

/-- Exact Oracle `mu` contribution before move (`Oracle.cpp::ComputeDynamicCost`). -/
def oracleMuBefore (m : Move) : Nat :=
  blockWeight m.srcDepth

/-- Exact Oracle `mu` contribution after move. -/
def oracleMuAfter (m : Move) : Nat :=
  (if m.keepSrc then oracleMuBefore m else 0) + oracleCostSum m.dstDepths

/-- Branch-probability-weighted dynamic cost used to break equal-`oracleMu` ties
    for same-depth bypassable ColdSink moves. -/
def expectedMuBefore (m : Move) : Nat :=
  2 * blockWeight m.srcDepth

def expectedMuAfter (m : Move) : Nat :=
  (if m.keepSrc then expectedMuBefore m else 0) + expectedCostSum m.bypassable m.dstDepths

def excess (pl budget : Nat) : Nat :=
  pl - budget

def allDstLeSrc (srcDepth : Nat) (dsts : List Nat) : Bool :=
  dsts.all (fun d => decide (d ≤ srcDepth))

/-- Lexicographic strict improvement on `(excess, oracleMu, expectedMu, pl)`. -/
def strictlyImproves (m : Move) : Bool :=
  decide (excess m.plAfter m.budget < excess m.plBefore m.budget) ||
  (decide (excess m.plAfter m.budget = excess m.plBefore m.budget) &&
   decide (oracleMuAfter m < oracleMuBefore m)) ||
  (decide (excess m.plAfter m.budget = excess m.plBefore m.budget) &&
   decide (oracleMuAfter m = oracleMuBefore m) &&
   decide (expectedMuAfter m < expectedMuBefore m)) ||
  (decide (excess m.plAfter m.budget = excess m.plBefore m.budget) &&
   decide (oracleMuAfter m = oracleMuBefore m) &&
   decide (expectedMuAfter m = expectedMuBefore m) &&
   decide (m.plAfter < m.plBefore))

/-- Non-worsening tie-break allowed ONLY on a value's first commit (`curOwner = none`). -/
def nonWorsening (m : Move) : Bool :=
  decide (excess m.plAfter m.budget = excess m.plBefore m.budget) &&
  decide (oracleMuAfter m = oracleMuBefore m) &&
  decide (expectedMuAfter m = expectedMuBefore m) &&
  decide (m.plAfter = m.plBefore)

/-- Single decision gate shared by LICM, ColdSink, and PressureShrink. -/
def acceptMove (curOwner : Owner) (m : Move) : Bool :=
  !(m.owner == Owner.none) &&
  !m.dstDepths.isEmpty &&
  allDstLeSrc m.srcDepth m.dstDepths &&
  !(curOwner != Owner.none && curOwner != m.owner) &&
  (strictlyImproves m || (curOwner == Owner.none && nonWorsening m))

/-- Lexicographic strict order on `(excess, oracleMu, expectedMu, pl, uncommitted)`. -/
def LexLt5 (a b : Nat × Nat × Nat × Nat × Nat) : Prop :=
  a.1 < b.1 ∨
  (a.1 = b.1 ∧ a.2.1 < b.2.1) ∨
  (a.1 = b.1 ∧ a.2.1 = b.2.1 ∧ a.2.2.1 < b.2.2.1) ∨
  (a.1 = b.1 ∧ a.2.1 = b.2.1 ∧ a.2.2.1 = b.2.2.1 ∧ a.2.2.2.1 < b.2.2.2.1) ∨
  (a.1 = b.1 ∧ a.2.1 = b.2.1 ∧ a.2.2.1 = b.2.2.1 ∧ a.2.2.2.1 = b.2.2.2.1 ∧
   a.2.2.2.2 < b.2.2.2.2)

/-- 1. Anti-ping-pong: once owned by proposer A, proposer B != A is rejected. -/
theorem acceptMove_no_pingpong (cur : Owner) (m : Move)
    (hOwned : cur ≠ Owner.none) (hDiff : cur ≠ m.owner) :
    acceptMove cur m = false := by
  unfold acceptMove
  have h1 : (cur != Owner.none) = true := by simp [hOwned]
  have h2 : (cur != m.owner) = true := by simp [hDiff]
  simp [h1, h2]

/-- 2. Loop-depth safety: moving or rematting into ANY deeper loop is rejected
    unconditionally (eliminating the IsFreeMov LICM-undo bug by construction). -/
theorem remat_into_deeper_loop_rejected (cur : Owner) (m : Move)
    (d : Nat) (hMem : d ∈ m.dstDepths) (hDeep : m.srcDepth < d) :
    acceptMove cur m = false := by
  have hAllFalse : allDstLeSrc m.srcDepth m.dstDepths = false := by
    unfold allDstLeSrc
    rw [List.all_eq_false]
    refine ⟨d, hMem, ?_⟩
    simp [Nat.not_le.mpr hDeep]
  unfold acceptMove
  simp [hAllFalse]

/-- 3. Hoisting out of a loop strictly lowers `oracleMu` and is accepted in normal mode. -/
theorem hoist_strictly_improves (d : Nat) (hCap : d < 6) (pl budget : Nat)
    (hWithin : pl ≤ budget) :
    acceptMove Owner.none
      { owner := Owner.licm
        srcDepth := d + 1
        dstDepths := [d]
        keepSrc := false
        bypassable := false
        plBefore := pl
        plAfter := pl
        budget := budget } = true := by
  have hW : blockWeight d < blockWeight (d + 1) :=
    blockWeight_strict_mono (Nat.lt_succ_self d) hCap
  have hEx : pl - budget = 0 := Nat.sub_eq_zero_of_le hWithin
  unfold acceptMove strictlyImproves allDstLeSrc oracleMuBefore oracleMuAfter excess
  simp [oracleCostSum, hEx, Nat.le_succ]
  omega

/-- 4. Normal-regime Oracle-mu monotonicity: when `plBefore ≤ budget`, every
    accepted move keeps `plAfter ≤ budget` and satisfies `oracleMuAfter ≤ oracleMuBefore`. -/
theorem acceptMove_normal_monotone_oracleMu (cur : Owner) (m : Move)
    (hWithin : m.plBefore ≤ m.budget)
    (hAcc : acceptMove cur m = true) :
    m.plAfter ≤ m.budget ∧ oracleMuAfter m ≤ oracleMuBefore m := by
  have hExB : excess m.plBefore m.budget = 0 := by
    unfold excess; omega
  unfold acceptMove strictlyImproves nonWorsening at hAcc
  simp only [Bool.and_eq_true, Bool.or_eq_true, decide_eq_true_eq] at hAcc
  rcases hAcc with ⟨_, hCond⟩
  unfold excess at hCond hExB
  rcases hCond with
      (((h1 | ⟨h2a, h2b⟩) | ⟨⟨h3a, h3b⟩, _⟩) | ⟨⟨⟨h4a, h4b⟩, _⟩, _⟩) |
      ⟨_, ⟨⟨h5a, h5b⟩, _⟩, _⟩ <;> omega

/-- 5. High-pressure remat (`oracleMuAfter > oracleMuBefore`) is accepted ONLY
    when it strictly reduces pressure excess `pl - budget`. -/
theorem acceptMove_cost_increase_requires_pressure_drop (cur : Owner) (m : Move)
    (hCostUp : oracleMuBefore m < oracleMuAfter m)
    (hAcc : acceptMove cur m = true) :
    excess m.plAfter m.budget < excess m.plBefore m.budget := by
  unfold acceptMove strictlyImproves nonWorsening at hAcc
  simp only [Bool.and_eq_true, Bool.or_eq_true, decide_eq_true_eq] at hAcc
  rcases hAcc with ⟨_, hCond⟩
  rcases hCond with
      (((h1 | ⟨_, h2b⟩) | ⟨⟨_, h3b⟩, _⟩) | ⟨⟨⟨_, h4b⟩, _⟩, _⟩) |
      ⟨_, ⟨⟨_, h5b⟩, _⟩, _⟩ <;> omega

/-- 6. Global termination potential: every accepted move strictly decreases the
    well-founded 5-tuple `(excess, oracleMu, expectedMu, pl, uncommitted)` in `Nat^5`. -/
theorem acceptMove_lex_decreases (cur : Owner) (m : Move) (uncommitted : Nat)
    (hUncomm : cur = Owner.none → 0 < uncommitted)
    (hAcc : acceptMove cur m = true) :
    let nextUncomm := if cur == Owner.none then uncommitted - 1 else uncommitted
    LexLt5 (excess m.plAfter m.budget, oracleMuAfter m, expectedMuAfter m, m.plAfter, nextUncomm)
           (excess m.plBefore m.budget, oracleMuBefore m, expectedMuBefore m, m.plBefore, uncommitted) := by
  dsimp only [LexLt5]
  unfold acceptMove strictlyImproves nonWorsening at hAcc
  simp only [Bool.and_eq_true, Bool.or_eq_true, beq_iff_eq, decide_eq_true_eq] at hAcc
  rcases hAcc with ⟨_, hCond⟩
  rcases hCond with
      (((h1 | ⟨h2a, h2b⟩) | ⟨⟨h3a, h3b⟩, h3c⟩) | ⟨⟨⟨h4a, h4b⟩, h4c⟩, h4d⟩) |
      ⟨hNone, ⟨⟨h5a, h5b⟩, h5c⟩, h5d⟩
  · exact Or.inl h1
  · exact Or.inr (Or.inl ⟨h2a, h2b⟩)
  · exact Or.inr (Or.inr (Or.inl ⟨h3a, h3b, h3c⟩))
  · exact Or.inr (Or.inr (Or.inr (Or.inl ⟨h4a, h4b, h4c, h4d⟩)))
  · have hPos : 0 < uncommitted := hUncomm hNone
    refine Or.inr (Or.inr (Or.inr (Or.inr ⟨h5a, h5b, h5c, h5d, ?_⟩)))
    simp [hNone]
    omega

end DXCB5.Placement
