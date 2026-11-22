-- Distributed under the University of Illinois Open Source License.
-- Formal specification and soundness proofs for DXCB5's post-RA allocation
-- validator (`VerifyAllocation` in `lib/RegAlloc/VerifyAllocation.cpp`).
-- Proves that if an allocation satisfies bounds, per-vreg lane-map injectivity,
-- fixed-lane preservation, and live-slot physical disjointness, then remapping
-- componentwise, fixed-slot, resource-swizzle, and self-mov instructions
-- preserves exact virtual-to-physical state equivalence.
import Init.Omega
import OpcodeSpec

namespace DXCB5.RegAllocValidator

open DXCB5.OpcodeSpec

abbrev VReg := Nat
abbrev PReg := Nat
abbrev Lane := Fin 4
abbrev VSlot := VReg × Lane
abbrev PSlot := PReg × Lane

-- Linear key `4 * reg + lane` used by C++ `VerifyAllocation`.
def slotKey (reg : Nat) (lane : Lane) : Nat :=
  4 * reg + lane.val

theorem slotKey_injective {r1 r2 : Nat} {l1 l2 : Lane}
    (h : slotKey r1 l1 = slotKey r2 l2) : r1 = r2 ∧ l1 = l2 := by
  have h1 := l1.isLt
  have h2 := l2.isLt
  change 4 * r1 + l1.val = 4 * r2 + l2.val at h
  have hl : l1.val = l2.val := by omega
  exact ⟨by omega, Fin.ext hl⟩

structure LaneAlloc where
  physReg : PReg
  laneMap : Lane → Lane
  physMask : BitVec 4

def mapSlot (alloc : VReg → LaneAlloc) (s : VSlot) : PSlot :=
  ((alloc s.1).physReg, (alloc s.1).laneMap s.2)

def laneInMask (m : BitVec 4) (l : Lane) : Bool :=
  m.getLsbD l.val

def mapDestMask (oldMask : BitVec 4) (lm : Lane → Lane) : BitVec 4 :=
  (if laneInMask oldMask 0 then (1#4 <<< (lm 0).val) else 0#4) |||
  (if laneInMask oldMask 1 then (1#4 <<< (lm 1).val) else 0#4) |||
  (if laneInMask oldMask 2 then (1#4 <<< (lm 2).val) else 0#4) |||
  (if laneInMask oldMask 3 then (1#4 <<< (lm 3).val) else 0#4)

-- ============================================================================
-- 1. Validator Invariants (1:1 with `VerifyAllocation.cpp`)
-- ============================================================================

def BoundsValid (active : VReg → BitVec 4) (physCount : Nat)
    (alloc : VReg → LaneAlloc) : Prop :=
  ∀ v, active v ≠ 0#4 → (alloc v).physReg < physCount

def LaneMapInjective (active : VReg → BitVec 4)
    (alloc : VReg → LaneAlloc) : Prop :=
  ∀ v l1 l2,
    laneInMask (active v) l1 = true →
    laneInMask (active v) l2 = true →
    (alloc v).laneMap l1 = (alloc v).laneMap l2 →
    l1 = l2

def FixedLanesValid (active : VReg → BitVec 4) (fixed : VReg → Bool)
    (alloc : VReg → LaneAlloc) : Prop :=
  ∀ v l,
    fixed v = true →
    laneInMask (active v) l = true →
    (alloc v).laneMap l = l

def PhysMaskValid (active : VReg → BitVec 4)
    (alloc : VReg → LaneAlloc) : Prop :=
  ∀ v, (alloc v).physMask = mapDestMask (active v) (alloc v).laneMap

def LiveDisjoint (alloc : VReg → LaneAlloc) (live : VSlot → Prop) : Prop :=
  ∀ s1 s2, live s1 → live s2 → mapSlot alloc s1 = mapSlot alloc s2 → s1 = s2

def WriteDisjoint (alloc : VReg → LaneAlloc) (dstV : VReg) (dstMask : BitVec 4)
    (liveAfter : VSlot → Prop) : Prop :=
  ∀ l other,
    laneInMask dstMask l = true →
    liveAfter other →
    other ≠ (dstV, l) →
    mapSlot alloc (dstV, l) ≠ mapSlot alloc other

-- Virtual/physical state agreement on all live virtual slots.
def StateAgree {α : Type} (alloc : VReg → LaneAlloc) (live : VSlot → Prop)
    (vstate : VSlot → α) (pstate : PSlot → α) : Prop :=
  ∀ s, live s → pstate (mapSlot alloc s) = vstate s

-- ============================================================================
-- 2. Swizzle Remapping Semantics & State-Preservation Theorems
-- ============================================================================

-- Models `RemapComponentwiseSource` on an active virtual lane `vl`.
-- For a virtual lane `vl` written to physical lane `pl = dstAlloc.laneMap vl`,
-- the remapped source swizzle at slot `pl` reads physical lane
-- `srcAlloc.laneMap (oldSwz vl)` of `srcAlloc.physReg`.
theorem remapped_componentwise_read_agrees {α : Type}
    (alloc : VReg → LaneAlloc) (live : VSlot → Prop)
    (vstate : VSlot → α) (pstate : PSlot → α)
    (hAgree : StateAgree alloc live vstate pstate)
    (srcV : VReg) (oldSwz : Lane → Lane) (vl : Lane)
    (hLiveSrc : live (srcV, oldSwz vl)) :
    pstate ((alloc srcV).physReg, (alloc srcV).laneMap (oldSwz vl)) =
      vstate (srcV, oldSwz vl) :=
  hAgree (srcV, oldSwz vl) hLiveSrc

-- Models `RemapFixedSlotSource`: slot `slot` reads `srcAlloc.laneMap (oldSwz slot)`.
theorem remapped_fixed_read_agrees {α : Type}
    (alloc : VReg → LaneAlloc) (live : VSlot → Prop)
    (vstate : VSlot → α) (pstate : PSlot → α)
    (hAgree : StateAgree alloc live vstate pstate)
    (srcV : VReg) (oldSwz : Lane → Lane) (slot : Lane)
    (hLiveSrc : live (srcV, oldSwz slot)) :
    pstate ((alloc srcV).physReg, (alloc srcV).laneMap (oldSwz slot)) =
      vstate (srcV, oldSwz slot) :=
  hAgree (srcV, oldSwz slot) hLiveSrc

-- Virtual update of a masked write to `(dstV, dstMask)`.
def updateVirtual {α : Type} (dstV : VReg) (dstMask : BitVec 4)
    (vals : Lane → α) (vstate : VSlot → α) : VSlot → α :=
  fun s => if s.1 = dstV ∧ laneInMask dstMask s.2 = true then vals s.2 else vstate s

-- Physical update of a remapped masked write to `dstAlloc.physReg` where each
-- active virtual lane `vl` writes `vals vl` to physical lane `dstAlloc.laneMap vl`.
def updatePhysical {α : Type} (alloc : VReg → LaneAlloc)
    (dstV : VReg) (dstMask : BitVec 4)
    (vals : Lane → α) (pstate : PSlot → α) : PSlot → α :=
  fun ps =>
    if ps.1 = (alloc dstV).physReg then
      if laneInMask dstMask 0 = true ∧ (alloc dstV).laneMap 0 = ps.2 then vals 0
      else if laneInMask dstMask 1 = true ∧ (alloc dstV).laneMap 1 = ps.2 then vals 1
      else if laneInMask dstMask 2 = true ∧ (alloc dstV).laneMap 2 = ps.2 then vals 2
      else if laneInMask dstMask 3 = true ∧ (alloc dstV).laneMap 3 = ps.2 then vals 3
      else pstate ps
    else pstate ps

private theorem lane_cases (l : Lane) : l = 0 ∨ l = 1 ∨ l = 2 ∨ l = 3 := by
  rcases l with ⟨v, hv⟩
  match v, hv with
  | 0, _ => exact Or.inl rfl
  | 1, _ => exact Or.inr (Or.inl rfl)
  | 2, _ => exact Or.inr (Or.inr (Or.inl rfl))
  | 3, _ => exact Or.inr (Or.inr (Or.inr rfl))

private theorem updatePhysical_hit {α : Type} (alloc : VReg → LaneAlloc)
    (active : VReg → BitVec 4) (dstV : VReg) (dstMask : BitVec 4)
    (hSub : ∀ l, laneInMask dstMask l = true → laneInMask (active dstV) l = true)
    (hInj : LaneMapInjective active alloc)
    (vals : Lane → α) (pstate : PSlot → α) (vl : Lane)
    (hMem : laneInMask dstMask vl = true) :
    updatePhysical alloc dstV dstMask vals pstate (mapSlot alloc (dstV, vl)) = vals vl := by
  unfold updatePhysical mapSlot
  simp only [↓reduceIte]
  have hne (a b : Lane) (hab : a ≠ b) (ha : laneInMask dstMask a = true)
      (hb : laneInMask dstMask b = true) :
      ¬(laneInMask dstMask a = true ∧ (alloc dstV).laneMap a = (alloc dstV).laneMap b) := by
    intro h
    exact hab (hInj dstV a b (hSub a ha) (hSub b hb) h.2)
  rcases lane_cases vl with rfl | rfl | rfl | rfl
  · simp [hMem]
  · have h0 : ¬(laneInMask dstMask 0 = true ∧ (alloc dstV).laneMap 0 = (alloc dstV).laneMap 1) := by
      intro h; exact hne 0 1 (by decide) h.1 hMem h
    simp [h0, hMem]
  · have h0 : ¬(laneInMask dstMask 0 = true ∧ (alloc dstV).laneMap 0 = (alloc dstV).laneMap 2) := by
      intro h; exact hne 0 2 (by decide) h.1 hMem h
    have h1 : ¬(laneInMask dstMask 1 = true ∧ (alloc dstV).laneMap 1 = (alloc dstV).laneMap 2) := by
      intro h; exact hne 1 2 (by decide) h.1 hMem h
    simp [h0, h1, hMem]
  · have h0 : ¬(laneInMask dstMask 0 = true ∧ (alloc dstV).laneMap 0 = (alloc dstV).laneMap 3) := by
      intro h; exact hne 0 3 (by decide) h.1 hMem h
    have h1 : ¬(laneInMask dstMask 1 = true ∧ (alloc dstV).laneMap 1 = (alloc dstV).laneMap 3) := by
      intro h; exact hne 1 3 (by decide) h.1 hMem h
    have h2 : ¬(laneInMask dstMask 2 = true ∧ (alloc dstV).laneMap 2 = (alloc dstV).laneMap 3) := by
      intro h; exact hne 2 3 (by decide) h.1 hMem h
    simp [h0, h1, h2, hMem]

private theorem updatePhysical_miss {α : Type} (alloc : VReg → LaneAlloc)
    (dstV : VReg) (dstMask : BitVec 4)
    (vals : Lane → α) (pstate : PSlot → α) (ps : PSlot)
    (hMiss : ∀ l, laneInMask dstMask l = true → mapSlot alloc (dstV, l) ≠ ps) :
    updatePhysical alloc dstV dstMask vals pstate ps = pstate ps := by
  unfold updatePhysical
  by_cases hr : ps.1 = (alloc dstV).physReg
  · simp only [hr, ↓reduceIte]
    have hno (l : Lane) :
        ¬(laneInMask dstMask l = true ∧ (alloc dstV).laneMap l = ps.2) := by
      intro ⟨hm, hl⟩
      exact hMiss l hm (Prod.ext hr.symm hl)
    simp [hno 0, hno 1, hno 2, hno 3]
  · simp [hr]

-- Core theorem: a verified allocation preserves exact virtual/physical state
-- equivalence across any remapped masked instruction write.
theorem remapped_write_preserves_agree {α : Type}
    (alloc : VReg → LaneAlloc) (active : VReg → BitVec 4)
    (dstV : VReg) (dstMask : BitVec 4)
    (liveBefore liveAfter : VSlot → Prop)
    (vstate : VSlot → α) (pstate : PSlot → α) (vals : Lane → α)
    (hSub : ∀ l, laneInMask dstMask l = true → laneInMask (active dstV) l = true)
    (hInj : LaneMapInjective active alloc)
    (hDisj : WriteDisjoint alloc dstV dstMask liveAfter)
    (hSurvive : ∀ s, liveAfter s → ¬(s.1 = dstV ∧ laneInMask dstMask s.2 = true) → liveBefore s)
    (hAgree : StateAgree alloc liveBefore vstate pstate) :
    StateAgree alloc liveAfter
      (updateVirtual dstV dstMask vals vstate)
      (updatePhysical alloc dstV dstMask vals pstate) := by
  intro s hLiveAfter
  by_cases hHit : s.1 = dstV ∧ laneInMask dstMask s.2 = true
  · rcases s with ⟨sv, sl⟩
    dsimp at hHit
    rcases hHit with ⟨rfl, hMem⟩
    rw [updatePhysical_hit alloc active sv dstMask hSub hInj vals pstate sl hMem]
    simp [updateVirtual, hMem]
  · have hMiss : ∀ l, laneInMask dstMask l = true → mapSlot alloc (dstV, l) ≠ mapSlot alloc s := by
      intro l hMem
      have hne : s ≠ (dstV, l) := by
        intro heq
        subst heq
        exact hHit ⟨rfl, hMem⟩
      exact hDisj l s hMem hLiveAfter hne
    rw [updatePhysical_miss alloc dstV dstMask vals pstate (mapSlot alloc s) hMiss]
    simp only [updateVirtual, hHit, ↓reduceIte]
    exact hAgree s (hSurvive s hLiveAfter hHit)

-- Self-move elimination soundness (`IsSelfMov` in `RegisterAllocation.cpp`):
-- If a plain `MOV` maps every written lane `(dstV, l)` to the exact same
-- physical slot as its source `(srcV, srcSwz l)` and is disjoint from all other
-- live-after slots, omitting the instruction preserves `StateAgree`.
theorem remapped_self_mov_noop {α : Type}
    (alloc : VReg → LaneAlloc) (dstV srcV : VReg) (dstMask : BitVec 4)
    (srcSwz : Lane → Lane) (liveBefore liveAfter : VSlot → Prop)
    (vstate : VSlot → α) (pstate : PSlot → α)
    (hSelf : ∀ l, laneInMask dstMask l = true →
      mapSlot alloc (dstV, l) = mapSlot alloc (srcV, srcSwz l))
    (hLiveSrc : ∀ l, laneInMask dstMask l = true → liveBefore (srcV, srcSwz l))
    (hSurvive : ∀ s, liveAfter s → ¬(s.1 = dstV ∧ laneInMask dstMask s.2 = true) → liveBefore s)
    (hAgree : StateAgree alloc liveBefore vstate pstate) :
    StateAgree alloc liveAfter
      (updateVirtual dstV dstMask (fun l => vstate (srcV, srcSwz l)) vstate)
      pstate := by
  intro s hLiveAfter
  by_cases hHit : s.1 = dstV ∧ laneInMask dstMask s.2 = true
  · rcases s with ⟨sv, sl⟩
    dsimp at hHit
    rcases hHit with ⟨rfl, hMem⟩
    rw [hSelf sl hMem]
    simp only [updateVirtual, hMem, and_self, ↓reduceIte]
    exact hAgree (srcV, srcSwz sl) (hLiveSrc sl hMem)
  · simp only [updateVirtual, hHit, ↓reduceIte]
    exact hAgree s (hSurvive s hLiveAfter hHit)

-- Bridge to `OpcodeSpec`: resource return swizzle remapping preserves every
-- active channel when `laneMap` is injective on `oldMask`.
theorem remapped_resource_swizzle_agrees
    (oldSwz : Lane → Lane) (oldMask : BitVec 4) (lm : Lane → Lane)
    (hInj : ∀ a b : Lane, oldMask.getLsbD a.val = true → oldMask.getLsbD b.val = true →
      lm a = lm b → a = b)
    (vl : Lane) (hMem : oldMask.getLsbD vl.val = true) :
    remapResourceReturnSwizzle oldSwz oldMask lm (lm vl) = oldSwz vl :=
  remapResourceSwizzle_preserves_channel oldSwz oldMask lm hInj vl hMem

end DXCB5.RegAllocValidator
