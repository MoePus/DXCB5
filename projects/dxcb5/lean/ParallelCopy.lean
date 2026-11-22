import Init.Omega

namespace DXCB5.ParallelCopy

abbrev Register := Nat
abbrev State (α : Type) := Register → α

-- Match the C++ key r * 4 + lane. Four lanes never alias across registers.
def componentKey (register : Nat) (lane : Fin 4) : Nat :=
  4 * register + lane.val

theorem componentKey_injective {left right : Nat} {x y : Fin 4}
    (equal : componentKey left x = componentKey right y) :
    left = right ∧ x = y := by
  have hx := x.isLt
  have hy := y.isLt
  change 4 * left + x.val = 4 * right + y.val at equal
  have lanes : x.val = y.val := by omega
  exact ⟨by omega, Fin.ext lanes⟩

structure Copy where
  destination : Register
  source : Register

def step (copy : Copy) (initial : State α) : State α :=
  fun r => if r = copy.destination then initial copy.source else initial r

def execute : List Copy → State α → State α
  | [], initial => initial
  | copy :: rest, initial => execute rest (step copy initial)

-- Each destination in a well-formed parallel assignment occurs at most once.
def parallelOrigin : List Copy → Register → Register
  | [], r => r
  | copy :: rest, r => if r = copy.destination then copy.source else parallelOrigin rest r

-- A masked MOV reads both inputs before it writes either result.
-- The emission merger rejects exactly the new read-after-write dependency.
theorem adjacentMoves_canShareInstruction (first second : Copy)
    (destinations : first.destination ≠ second.destination)
    (readBeforeWrite : second.source ≠ first.destination)
    (initial : State α) (r : Register) :
    execute [first, second] initial r =
      initial (parallelOrigin [first, second] r) := by
  by_cases hitFirst : r = first.destination
  · subst r
    simp [execute, step, parallelOrigin, destinations]
  · by_cases hitSecond : r = second.destination
    · subst r
      simp [execute, step, parallelOrigin, Ne.symm destinations, readBeforeWrite]
    · simp [execute, step, parallelOrigin, hitFirst, hitSecond]

def Writes (copies : List Copy) (r : Register) : Prop :=
  ∃ copy ∈ copies, copy.destination = r

theorem step_map (copy : Copy) (initial : State α) (map : α → β) :
    step copy (fun r => map (initial r)) = fun r => map (step copy initial r) := by
  funext r
  by_cases address : r = copy.destination <;> simp [step, address]

theorem execute_map (sequence : List Copy) (initial : State α) (map : α → β) :
    execute sequence (fun r => map (initial r)) = fun r => map (execute sequence initial r) := by
  induction sequence generalizing initial with
  | nil => rfl
  | cons copy rest ih =>
    change execute rest (step copy (fun r => map (initial r))) =
      fun r => map (execute rest (step copy initial) r)
    rw [step_map]
    exact ih (step copy initial)

private theorem writes_cons (copy : Copy) (rest : List Copy) (r : Register)
    (untouched : ¬ Writes (copy :: rest) r) :
    r ≠ copy.destination ∧ ¬ Writes rest r := by
  constructor
  · intro equal
    exact untouched ⟨copy, List.mem_cons_self, equal.symm⟩
  · intro ⟨other, membership, equal⟩
    exact untouched ⟨other, List.mem_cons_of_mem _ membership, equal⟩

theorem execute_untouched (sequence : List Copy) (initial : State α) (r : Register)
    (untouched : ¬ Writes sequence r) : execute sequence initial r = initial r := by
  induction sequence generalizing initial with
  | nil => rfl
  | cons copy rest ih =>
    obtain ⟨head, tail⟩ := writes_cons copy rest r untouched
    change execute rest (step copy initial) r = initial r
    rw [ih (step copy initial) tail]
    simp [step, head]

theorem parallel_untouched (copies : List Copy) (r : Register) (untouched : ¬ Writes copies r) :
    parallelOrigin copies r = r := by
  induction copies with
  | nil => rfl
  | cons copy rest ih =>
    obtain ⟨head, tail⟩ := writes_cons copy rest r untouched
    simp [parallelOrigin, head, ih tail]

-- The verifier compares original register IDs on every possibly changed register.
-- Matching origins imply matching values for every possible initial register state.
theorem checkedSequence_preserves (copies sequence : List Copy) (scratch : Register)
    (checked : ∀ r, r ≠ scratch → (Writes copies r ∨ Writes sequence r) →
      execute sequence (fun i => i) r = parallelOrigin copies r)
    (initial : State α) (r : Register) (live : r ≠ scratch) :
    execute sequence initial r = initial (parallelOrigin copies r) := by
  have symbolic := congrFun (execute_map sequence (fun i : Register => i) initial) r
  change execute sequence initial r = initial (execute sequence (fun i => i) r) at symbolic
  rw [symbolic]
  by_cases written : Writes copies r ∨ Writes sequence r
  · rw [checked r live written]
  · have noExpected : ¬ Writes copies r := fun h => written (Or.inl h)
    have noActual : ¬ Writes sequence r := fun h => written (Or.inr h)
    rw [execute_untouched sequence _ r noActual, parallel_untouched copies r noExpected]

theorem swap_uses_saved_input (initial : State α) :
    let sequence : List Copy := [⟨2, 0⟩, ⟨0, 1⟩, ⟨1, 2⟩]
    execute sequence initial 0 = initial 1 ∧ execute sequence initial 1 = initial 0 := by
  simp [execute, step]

def HasRead (copies : List Copy) (r : Register) : Prop :=
  ∃ copy ∈ copies, copy.source = r

private theorem origin_of_destination (before after : List Copy) (chosen : Copy)
    (unique : ¬ Writes before chosen.destination) :
    parallelOrigin (before ++ chosen :: after) chosen.destination = chosen.source := by
  induction before with
  | nil => simp [parallelOrigin]
  | cons head rest ih =>
    obtain ⟨other, noTail⟩ := writes_cons head rest chosen.destination unique
    simp [parallelOrigin, other, ih noTail]

private theorem origin_remove_other (before after : List Copy) (chosen : Copy)
    (r : Register) (other : r ≠ chosen.destination) :
    parallelOrigin (before ++ chosen :: after) r =
      parallelOrigin (before ++ after) r := by
  induction before with
  | nil => simp [parallelOrigin, other]
  | cons head rest ih =>
    simp [parallelOrigin, ih]

private theorem origin_not_read (copies : List Copy) (d r : Register)
    (noRead : ¬ HasRead copies d) (other : r ≠ d) :
    parallelOrigin copies r ≠ d := by
  induction copies with
  | nil => exact other
  | cons head tail ih =>
    have noHead : head.source ≠ d := fun h => noRead ⟨head, List.mem_cons_self, h⟩
    have noTail : ¬ HasRead tail d := fun ⟨copy, h, eq⟩ =>
      noRead ⟨copy, List.mem_cons_of_mem _ h, eq⟩
    by_cases dst : r = head.destination <;> simp [parallelOrigin, dst, noHead, ih noTail]

theorem emit_ready_preserves (before after : List Copy) (chosen : Copy)
    (unique : ¬ Writes (before ++ after) chosen.destination)
    (ready : ¬ HasRead (before ++ after) chosen.destination)
    (initial : State α) (r : Register) :
    step chosen initial (parallelOrigin (before ++ after) r) =
      initial (parallelOrigin (before ++ chosen :: after) r) := by
  by_cases destination : r = chosen.destination
  · subst r
    have noBefore : ¬ Writes before chosen.destination :=
      fun ⟨copy, member, eq⟩ => unique ⟨copy, List.mem_append_left _ member, eq⟩
    rw [origin_of_destination before after chosen noBefore]
    have unaffected := parallel_untouched (before ++ after) chosen.destination unique
    rw [unaffected]
    simp [step]
  · rw [origin_remove_other before after chosen r destination]
    simp [step, origin_not_read (before ++ after) chosen.destination r ready destination]

def redirect (saved scratch : Register) (copy : Copy) : Copy :=
  { copy with source := if copy.source = saved then scratch else copy.source }

theorem spill_preserves (copies : List Copy) (saved scratch : Register)
    (fresh : ¬ HasRead copies scratch)
    (initial : State α) (r : Register) (live : r ≠ scratch) :
    step ⟨scratch, saved⟩ initial (parallelOrigin (copies.map (redirect saved scratch)) r) =
      initial (parallelOrigin copies r) := by
  induction copies with
  | nil => simp [parallelOrigin, step, live]
  | cons head tail ih =>
    have noHead : head.source ≠ scratch := fun h => fresh ⟨head, List.mem_cons_self, h⟩
    have noTail : ¬ HasRead tail scratch := fun ⟨copy, member, eq⟩ =>
      fresh ⟨copy, List.mem_cons_of_mem _ member, eq⟩
    by_cases dst : r = head.destination
    · by_cases src : head.source = saved <;>
          simp [parallelOrigin, redirect, dst, src, step, noHead]
    · simpa [parallelOrigin, redirect, dst] using ih noTail

-- The trace relation follows the C++ scheduler after filtering self-copies.
-- Each move records the pending list before and after the corresponding mutation.
inductive Schedule (scratch : Register) : List Copy → List Copy → Prop
  | done : Schedule scratch [] []
  | ready (before after : List Copy) (chosen : Copy) (rest : List Copy)
      (unique : ¬ Writes (before ++ after) chosen.destination)
      (ready : ¬ HasRead (before ++ after) chosen.destination)
      (next : Schedule scratch (before ++ after) rest) :
      Schedule scratch (before ++ chosen :: after) (chosen :: rest)
  | spill (copies : List Copy) (saved : Register) (rest : List Copy)
      (distinct : saved ≠ scratch) (fresh : ¬ HasRead copies scratch)
      (next : Schedule scratch (copies.map (redirect saved scratch)) rest) :
      Schedule scratch copies (⟨scratch, saved⟩ :: rest)

theorem schedule_preserves (scratch : Register) (copies sequence : List Copy)
    (schedule : Schedule scratch copies sequence) (initial : State α)
    (r : Register) (live : r ≠ scratch) :
    execute sequence initial r = initial (parallelOrigin copies r) := by
  induction schedule generalizing initial with
  | done => rfl
  | ready before after chosen rest unique ready next ih =>
    change execute rest (step chosen initial) r =
      initial (parallelOrigin (before ++ chosen :: after) r)
    rw [ih]
    exact emit_ready_preserves before after chosen unique ready initial r
  | spill copies saved rest distinct fresh next ih =>
    change execute rest (step ⟨scratch, saved⟩ initial) r =
      initial (parallelOrigin copies r)
    rw [ih]
    exact spill_preserves copies saved scratch fresh initial r live

-- Dropping identity moves is valid under the same destination uniqueness
-- invariant that the C++ resolver checks when it collects the pending copies.
def UniqueDestinations (copies : List Copy) : Prop :=
  (copies.map (·.destination)).Nodup

def Pending (copies : List Copy) : List Copy :=
  copies.filter (fun copy => copy.destination != copy.source)

theorem pendingOrigin_preserves (copies : List Copy) (unique : UniqueDestinations copies)
    (r : Register) : parallelOrigin (Pending copies) r = parallelOrigin copies r := by
  induction copies with
  | nil => rfl
  | cons head tail ih =>
    simp only [UniqueDestinations, List.map_cons, List.nodup_cons] at unique
    have tailResult := ih unique.2
    by_cases identity : head.destination = head.source
    · simp only [Pending, List.filter_cons, identity, bne_self_eq_false, Bool.false_eq_true,
        ite_false, parallelOrigin]
      rw [show List.filter (fun copy => copy.destination != copy.source) tail =
          Pending tail from rfl, tailResult]
      split
      next address =>
        have noWrite : ¬ Writes tail r := by
          intro ⟨copy, member, dest⟩
          exact unique.1 (List.mem_map.mpr ⟨copy, member, dest.trans (address.trans identity.symm)⟩)
        exact parallel_untouched tail r noWrite |>.trans address
      next => rfl
    · simp only [Pending, List.filter_cons, bne_iff_ne.mpr identity, ite_true, parallelOrigin]
      rw [show List.filter (fun copy => copy.destination != copy.source) tail =
          Pending tail from rfl, tailResult]

theorem pendingSchedule_preserves (scratch : Register) (copies sequence : List Copy)
    (unique : UniqueDestinations copies) (schedule : Schedule scratch (Pending copies) sequence)
    (initial : State α) (r : Register) (live : r ≠ scratch) :
    execute sequence initial r = initial (parallelOrigin copies r) := by
  rw [schedule_preserves scratch (Pending copies) sequence schedule initial r live,
    pendingOrigin_preserves copies unique r]



namespace ReadOnly

inductive Source where
  | register (index : Register)
  | immutable (origin : Nat)

structure MixedCopy where
  destination : Register
  source : Source

def sourceValue (initial : State α) (immutable : Nat → α) : Source → α
  | .register index => initial index
  | .immutable origin => immutable origin

def mixedStep (copy : MixedCopy) (initial : State α) (immutable : Nat → α) : State α :=
  fun r => if r = copy.destination then sourceValue initial immutable copy.source else initial r

def mixedExecute : List MixedCopy → State α → (Nat → α) → State α
  | [], initial, _ => initial
  | copy :: rest, initial, immutable => mixedExecute rest (mixedStep copy initial immutable) immutable

def parallelValue (initial : State α) (immutable : Nat → α) : List MixedCopy → Register → α
  | [], r => initial r
  | copy :: rest, r =>
    if r = copy.destination then sourceValue initial immutable copy.source
    else parallelValue initial immutable rest r

-- Use even slots for writable registers and odd slots for read-only origins.
-- Encode each C++ Origin by its observed fields (kind, index, element,
-- swizzle, modifier, relative register, relative lane) concatenated as
-- fixed-width integers to make the immutable key injective. The relative
-- register must be unset, and temp operands use registerSlot.
def registerSlot (r : Register) : Register := 2 * r

def sourceSlot : Source → Register
  | .register r => registerSlot r
  | .immutable origin => 2 * origin + 1

def encode (copy : MixedCopy) : Copy := ⟨registerSlot copy.destination, sourceSlot copy.source⟩

def extend (initial : State α) (immutable : Nat → α) : State α :=
  fun slot => if slot % 2 = 0 then initial (slot / 2) else immutable (slot / 2)

@[simp] private theorem registerSlot_eq (r s : Register) :
    registerSlot r = registerSlot s ↔ r = s := by
  exact Nat.mul_left_cancel_iff (by decide : 0 < 2)

@[simp] private theorem extend_register (initial : State α) (immutable : Nat → α) (r : Register) :
    extend initial immutable (registerSlot r) = initial r := by
  simp [extend, registerSlot, Nat.mul_div_cancel_left r (by decide : 0 < 2)]

@[simp] private theorem extend_source (initial : State α) (immutable : Nat → α) (source : Source) :
    extend initial immutable (sourceSlot source) = sourceValue initial immutable source := by
  cases source with
  | register index => simp [sourceSlot, sourceValue]
  | immutable origin =>
    change (if (2 * origin + 1) % 2 = 0 then initial ((2 * origin + 1) / 2)
      else immutable ((2 * origin + 1) / 2)) = immutable origin
    simp [Nat.add_comm (2 * origin) 1, Nat.add_mul_div_left 1 origin (by decide : 0 < 2)]

private theorem encodeStep (copy : MixedCopy) (initial : State α) (immutable : Nat → α) :
    step (encode copy) (extend initial immutable) = extend (mixedStep copy initial immutable) immutable := by
  funext slot
  by_cases even : slot % 2 = 0
  · have register : slot = registerSlot (slot / 2) := by
      simpa [registerSlot, even] using (Nat.div_add_mod slot 2).symm
    rw [register]
    simp [step, encode, mixedStep]
  · have different : slot ≠ registerSlot copy.destination := by
      intro equal
      apply even
      simp [equal, registerSlot]
    simp [step, encode, different, extend, even]

private theorem encodeExecute (sequence : List MixedCopy) (initial : State α) (immutable : Nat → α) :
    execute (sequence.map encode) (extend initial immutable) =
      extend (mixedExecute sequence initial immutable) immutable := by
  induction sequence generalizing initial with
  | nil => rfl
  | cons copy rest ih =>
    simp only [List.map_cons, execute, mixedExecute, encodeStep, ih]

private theorem encodeParallel (copies : List MixedCopy) (initial : State α)
    (immutable : Nat → α) (r : Register) :
    extend initial immutable (parallelOrigin (copies.map encode) (registerSlot r)) =
      parallelValue initial immutable copies r := by
  induction copies with
  | nil => simp [parallelOrigin, parallelValue]
  | cons copy rest ih =>
    by_cases written : r = copy.destination <;> simp [parallelOrigin, parallelValue, encode, written, ih]

theorem mixedSchedule_preserves (scratch : Register) (copies sequence : List MixedCopy)
    (unique : (copies.map MixedCopy.destination).Nodup)
    (schedule : Schedule (registerSlot scratch) (Pending (copies.map encode)) (sequence.map encode))
    (initial : State α) (immutable : Nat → α) (r : Register) (live : r ≠ scratch) :
    mixedExecute sequence initial immutable r = parallelValue initial immutable copies r := by
  have encodedUnique : UniqueDestinations (copies.map encode) := by
    simpa [UniqueDestinations, List.Nodup, List.map_map, encode, Function.comp_def] using
      unique.map registerSlot (fun _ _ different equal => different ((registerSlot_eq _ _).mp equal))
  have result := pendingSchedule_preserves (registerSlot scratch) (copies.map encode)
    (sequence.map encode) encodedUnique schedule (extend initial immutable)
    (registerSlot r) (fun equal => live ((registerSlot_eq _ _).mp equal))
  rwa [encodeExecute, extend_register, encodeParallel] at result

theorem mixedSchedule_preserves_untouched
    (scratch : Register) (copies sequence : List MixedCopy)
    (unique : (copies.map MixedCopy.destination).Nodup)
    (schedule : Schedule (registerSlot scratch) (Pending (copies.map encode)) (sequence.map encode))
    (initial : State α) (immutable : Nat → α) (r : Register) (live : r ≠ scratch)
    (untouched : ∀ copy ∈ copies, copy.destination ≠ r) :
    mixedExecute sequence initial immutable r = initial r := by
  rw [mixedSchedule_preserves scratch copies sequence unique schedule initial immutable r live]
  clear unique schedule
  induction copies with
  | nil => rfl
  | cons copy rest ih =>
    have other : r ≠ copy.destination := (untouched copy (by simp)).symm
    have tail : ∀ copy ∈ rest, copy.destination ≠ r :=
      fun copy mem => untouched copy (List.mem_cons_of_mem _ mem)
    simp [parallelValue, other, ih tail]

end ReadOnly

-- Multi-lane scratch schedule: mirrors ParallelCopy.cpp breaking cross-register
-- vector cycles across any lane of a dedicated 4-lane scratch register.
inductive ScheduleSet (isScratch : Register → Prop) : List Copy → List Copy → Prop
  | done : ScheduleSet isScratch [] []
  | ready (before after : List Copy) (chosen : Copy) (rest : List Copy)
      (unique : ¬ Writes (before ++ after) chosen.destination)
      (ready : ¬ HasRead (before ++ after) chosen.destination)
      (next : ScheduleSet isScratch (before ++ after) rest) :
      ScheduleSet isScratch (before ++ chosen :: after) (chosen :: rest)
  | spill (copies : List Copy) (saved scratch : Register) (rest : List Copy)
      (inScratch : isScratch scratch)
      (distinct : saved ≠ scratch) (fresh : ¬ HasRead copies scratch)
      (next : ScheduleSet isScratch (copies.map (redirect saved scratch)) rest) :
      ScheduleSet isScratch copies (⟨scratch, saved⟩ :: rest)

theorem scheduleSet_preserves (isScratch : Register → Prop)
    (copies sequence : List Copy)
    (schedule : ScheduleSet isScratch copies sequence) (initial : State α)
    (r : Register) (live : ¬ isScratch r) :
    execute sequence initial r = initial (parallelOrigin copies r) := by
  induction schedule generalizing initial with
  | done => rfl
  | ready before after chosen rest unique ready next ih =>
    change execute rest (step chosen initial) r =
      initial (parallelOrigin (before ++ chosen :: after) r)
    rw [ih]
    exact emit_ready_preserves before after chosen unique ready initial r
  | spill copies saved scratch rest inScratch distinct fresh next ih =>
    have notScratch : r ≠ scratch := fun h => live (h ▸ inScratch)
    change execute rest (step ⟨scratch, saved⟩ initial) r =
      initial (parallelOrigin copies r)
    rw [ih]
    exact spill_preserves copies saved scratch fresh initial r notScratch

def isScratchLane (scratchReg : Nat) (k : Register) : Prop :=
  ∃ lane : Fin 4, k = componentKey scratchReg lane

theorem fourLaneScratch_preserves (scratchReg : Nat)
    (copies sequence : List Copy) (unique : UniqueDestinations copies)
    (schedule : ScheduleSet (isScratchLane scratchReg) (Pending copies) sequence)
    (initial : State α) (reg : Nat) (lane : Fin 4) (live : reg ≠ scratchReg) :
    execute sequence initial (componentKey reg lane) =
      initial (parallelOrigin copies (componentKey reg lane)) := by
  have notScratch : ¬ isScratchLane scratchReg (componentKey reg lane) := by
    intro ⟨sLane, eq⟩
    exact live (componentKey_injective eq).1
  rw [scheduleSet_preserves (isScratchLane scratchReg) (Pending copies) sequence
    schedule initial (componentKey reg lane) notScratch,
    pendingOrigin_preserves copies unique (componentKey reg lane)]

end DXCB5.ParallelCopy
