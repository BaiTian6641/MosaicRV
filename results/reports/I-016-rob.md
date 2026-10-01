# I-016 — architectural ROB with macro completion

Work package I-016, card `### I-016` in `docs/stage-1-scalar-control.md`.
Case `CASE=rob.out_of_order_children`.

Files delivered:

| File | Role |
|---|---|
| `rtl/core/mosaic_rob.sv` | the ROB: circular macro descriptors, completion bitmaps, generation-checked completion, in-order retirement, flush |
| `sim/tb/mosaic_rob_tb.sv` | simulation wrapper; also reads the elaborated geometry back out |
| `sim/unit/tb_rob.cpp` | C++ driver + independent shadow model + ten phases |
| `results/reports/I-016-rob.md` | this file |

Nothing outside that list was edited. No commit was made.

---

## 1. The interface, and its cycle semantics

One module, `mosaic_rob`. Every input is sampled on the rising edge of `clk`;
`rst` is synchronous and active high. **Everything else is combinational**: the
allocation decision, the four completion reports, the head view and the
observation view all describe the edge that has not happened yet, from the state
that exists now. There is no read latency anywhere, and therefore no
read-during-write mode to specify and no queueing to model.

### Allocate — one macro descriptor per cycle

```
alloc_valid, alloc_tag, alloc_pc, alloc_num_uops, alloc_exc, alloc_open
  -> alloc_ok, alloc_refused, alloc_full, alloc_bad_uops, alloc_index, alloc_gen
```

A macro is one **architectural instruction**, not one micro-operation. An
instruction that expands into *N* children occupies exactly one slot and carries
an *N*-bit completion bitmap. `alloc_num_uops` is the child count, so the set of
children the descriptor waits for is known from the moment the slot is written.
`alloc_open` allocates the descriptor *open* instead, for a producer that streams
children and closes the macro afterwards (§5).

`alloc_index` / `alloc_gen` are the **identity** of the macro that was just
created. The producer latches them and has to name both on every completion and
on every close. A macro allocated in cycle N is visible at the head from cycle
N+1.

Three refusals, all reported and all distinguishable, because "the ROB is full"
and "you sent nonsense" are different problems for the producer:

| Report | Meaning |
|---|---|
| `alloc_full` | the ROB is at capacity; the ROB's state, not the request's |
| `alloc_bad_uops` | `alloc_num_uops` is 0 or above `MAX_UOPS_PER_MACRO` |
| `alloc_ok` | this allocation happened and wrote the slot |

`alloc_refused` is asserted for a **resource or format** refusal only. An
allocation discarded because a flush arrived in the same cycle is not reported
as refused: that allocation was not something the producer could have avoided,
and counting it as a refusal would make the port's credit accounting describe
events that never happened.

### Complete — one child completion per cycle

```
cmp_valid, cmp_index, cmp_gen, cmp_uop, cmp_exc
  -> cmp_accepted, cmp_duplicate, cmp_stale, cmp_bad_uop
```

Exactly one of the four reports is asserted, and exactly one is asserted whenever
a completion is offered and no flush is in the same cycle. The order of decision
is identity first, because a completion aimed at a slot that is dead, or that
now holds a *different* macro, says nothing about child indices — its addressing
is not about this macro at all:

| Report | Condition | Why it is not silently absorbed |
|---|---|---|
| `cmp_stale` | the slot is not live, or its generation is not the one named | an old result landing in a new instruction is a wrong write, not a lost packet |
| `cmp_bad_uop` | identity holds, `cmp_uop` is outside the macro's own child count | setting a bit outside the expansion would make the bitmap claim a child the instruction never had |
| `cmp_duplicate` | identity and range hold, the bit is already set | the same child delivered twice by two producers is a producer bug, and only a report can find it |
| `cmp_accepted` | identity and range hold, the bit is clear | the only state change |

The card's blocking rule is *"duplicate completion, wrap aliasing, exception
loss"*; each of the three is a named output rather than an internal detail.

**Single completion port.** One completion per cycle is a deliberate choice, not
an oversight: p0 has two ALUs, and the result FIFO
(`MOSAIC_RESULT_FIFO`) is where a second result waits its turn. Widening the port
to two would mean arbitrating *two* writes to the same slot's bitmap in one
cycle, which is a read-modify-write collision with no read-during-write mode to
resolve it. One write port per slot removes the question rather than answering
it badly.

### Retire — one in-order pop

```
retire_req -> retire_ack, and the head view:
             head_valid, head_ready, head_replay, head_complete, head_exc,
             head_closed, head_index, head_gen, head_tag, head_pc,
             head_num_uops, head_done_mask, head_done_cnt
```

`head_ready` is the retirement predicate, stated once and used once:

```
head_ready = head_valid && head_complete && !head_exc && head_closed
```

So retirement advances **only** past a macro that is complete, carries no
exception and has a final expansion. `retire_ack` says whether the request was
accepted. `head_replay` names the head when it is exceptional, which is the one
output recovery needs to redirect to the exacting instruction.

Every head field is gated on `head_valid`: an empty ROB has no descriptor at the
head, and reading one would report whatever the silicon powered up with. The gate
costs no latency — the view is combinational either way — and it makes every head
field *total*, which is what lets the testbench compare it on **every** cycle
rather than only when the ROB happens to be non-empty.

### Flush

`flush_valid` drops every speculative macro — everything at and above the head —
in one cycle. It has priority over an allocation, a close, a completion or a
retirement offered in the same cycle: those events belong to a window that the
flush is destroying, and honouring any of them would leave a macro that is both
squashed and alive.

### Observation (verification and debug)

`obs_index` → the same descriptor fields, for an arbitrary slot, also gated on
validity. A functional consumer has no business reading it: the head view is the
ROB's interface. It exists so the unit test can check *every* slot rather than
only the one something is currently looking at.

---

## 2. The rule the card is about, written as an equation

```
complete  <=>  done_mask == expected_mask(num_uops)
```

A three-child expansion that completes in the order **2, 0, 1** does not have
its last *arriving* child as its last *completing* child. Any design that retires
a macro when the last-to-arrive child reports is therefore wrong in the common
case, and wrong *silently*: the head advances over two children that are still
outstanding and their results are written into architectural state nobody will
ever read them for. So retirement is a predicate over the whole bitmap, never
over an arrival count and never over an arrival index.

`expected_mask` is derived from the descriptor's **own** child count, so a
descriptor can neither wait for a child that does not exist nor be satisfied by a
child from outside its expansion.

Phase `out-of-order-children` builds exactly this case and fails the first draft
of the RTL that used "any bit set":

```
$ ./build/p0/unit/rob.out_of_order_children/rob.out_of_order_children \
      --case rob.out_of_order_children --seed 1 --max-cycles 200000     # with -DMOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT
MISMATCH out-of-order-children: a 3-child macro was called complete after its
last-numbered child alone arrived -- exactly the 'last-uop-arrives-is-complete'
failure: expected contract holds, got contract violated
RESULT FAIL rob.out_of_order_children contract violated: out-of-order-children:
a 3-child macro was called complete after its last-numbered child alone arrived
-- exactly the 'last-uop-arrives-is-complete' failure
```

and covers the widest macro the geometry allows (8 children) completing in strict
reverse order, asserting *not complete* after each of the first seven
completions.

---

## 3. Identity: why a 64-entry circular buffer needs a generation

`ROB_ENTRIES = 64` and the allocation pointer is a 6-bit circular index, so slot
63 is followed by slot 0 **every 64 allocations**. An index alone therefore
cannot identify an in-flight macro. After the wrap, index 0 names a different
macro than it did a moment ago, and a completion still in flight for the old
macro 0 is delivered into the new macro 0.

Every slot therefore carries a **generation**, and a completion names both:

```
slot_gen[i]      <= gen_counter[GEN_W-1:0]      at the allocation of macro i
cmp_identifies   =  slot_valid[cmp_idx] && (slot_gen[cmp_idx] == cmp_gen)
```

`gen_counter` advances on exactly one event — an accepted allocation — and is
**never rewound**: not by reset, not by a retirement, and not by a flush. So the
macro occupying a slot now always has a generation different from the one any
in-flight completion for a previous occupant of that slot carried.

Widths are derived by rule rather than written down, so the testbench wrapper can
reproduce them from the same generated package instead of carrying a second copy:

| Field | Rule | p0 value | What it buys |
|---|---|---|---|
| `TAG_W` | `2 * ROB_INDEX_W` | 12 | 64× margin over the 64 macros that can be live at once; the tag only has to be unique among them |
| `GEN_W` | `2 * ROB_INDEX_W` | 12 | 4096 allocations of aliasing margin per slot |

The 4096-allocation window is a deliberate, documented bound, not an accident: a
completion that has been in flight for longer than that, to the same slot, could
alias. It is the same trade every tag-based structure makes, and 4096
allocations of ageing is far beyond anything a recovery path leaves outstanding.
A larger profile widens it without anyone editing a number.

### The test proves the wrap happened, rather than assuming it

Phase `wrap-generation` does two things, in this order.

**First, the card's exact scenario.** Retire a macro, then compute how many
allocations it takes for the allocation pointer to *write* the slot that macro
just vacated, and make exactly that many:

```
distance = (victim_slot + ROB_ENTRIES - alloc_ptr) % ROB_ENTRIES + 1
```

The `+ 1` is not decoration: an allocation writes the slot the pointer names and
only then advances it, so the pointer *arriving* at slot S is one allocation
short of slot S being recycled. The first draft of this test got it wrong, the
scenario recycled nothing, and the phase failed with

```
MISMATCH wrap-generation: slot 0 was not reallocated, so the wrap did not happen
```

which is exactly the failure mode the phase exists to rule out. After the
allocations, the test asserts the occupant's generation differs from the victim's
and the occupant's tag differs from the victim's — so a ROB that recycled the
slot but kept the generation would fail here too.

Then it fires the victim's late completion at that **live** slot, and requires
`cmp_stale`, `cmp_accepted == 0` and an unchanged bitmap. The slot being live is
what makes the check sharp: a design that validated liveness alone would accept
it. The phase then fires a completion at the *same slot* with the *current*
generation and requires it to be accepted, so the phase cannot pass by rejecting
everything.

**Second, the sustained case.** 274 allocations over the 64-entry buffer
(1 victim + 64 to recycle its slot + 209 further rounds). The allocation pointer
is strictly round-robin through this phase — no flush, no refusal — so the
per-slot allocation counts are a lower bound, not an estimate; the phase asserts
`min over all 64 slots of (times allocated) >= 2` and the run passes. Every slot
is then probed with the generation from its *first* allocation ever, and all of
them must still be rejected. That is the aliasing property stated over the whole
buffer rather than over one hand-built case.

---

## 4. State kept, and why

| State | Width | Reset? | Why it exists |
|---|---|---|---|
| `slot_gen[64]` | 12 b | **no** | identity; what makes a recycled slot distinguishable |
| `slot_tag[64]` | 12 b | **no** | the producer's name for the instruction; I-017 needs it at retire |
| `slot_pc[64]` | 64 b | **no** | the replay target when a macro is exceptional |
| `slot_count[64]` | 4 b | **no** | the child count; the expected bitmap is derived from it |
| `slot_done[64]` | 8 b | **no** | the completion bitmap |
| `slot_exc[64]`, `slot_closed[64]` | 1 b each | **no** | descriptor state that blocks retirement |
| `slot_valid[64]` | 1 b | **yes** | validity held **outside** the descriptor |
| `head_ptr`, `alloc_ptr` | 6 b each | yes | the two ends of the circular window |
| `occ_cnt` | 7 b | yes | occupancy, including the "full" case |
| `alloc_total`, `retired_total`, `squashed_total` | 32 b each | yes | the conservation law (§6) |
| `gen_counter` | 32 b | yes | monotonic for the life of the machine |

Reset clears the pointers, the occupancy, the four counters and the valid vector.
It clears **nothing else**, which is the rule `rtl/common/mosaic_ram.sv`
documents: clearing a 64-entry descriptor array would give every slot a full
reset. Validity is tracked outside the arrays in an explicit packed vector,
exactly as the RAM's worked example prescribes.

### The descriptor arrays are registers, and that is stated, not glossed over

A slot is written by an allocation, written again by any number of child
completions and by a close, and read by the head view and the observation port
in arbitrary order. That is multi-port register state, not RAM, and it will infer
as registers. The consequence is written into the header rather than left for a
consumer to discover: the head view and the observation port are **combinational
reads of register state**, which is what the silicon does. There is no read
latency to model and no read-during-write collision to specify, because there is
one write port per slot per cycle by construction.

### Occupancy is computed once, from one copy of the pre-edge value

```systemverilog
always_comb begin
  if (flush_valid) occ_cnt_next = '0;
  else begin
    occ_cnt_next = occ_cnt;
    if (alloc_ok)   occ_cnt_next = occ_cnt_next + 1;
    if (retire_ack) occ_cnt_next = occ_cnt_next - 1;
  end
end
```

One statement doing `occ_cnt + 1` and another doing `occ_cnt - 1` would make the
result depend on the simulator's ordering of two non-blocking assignments to the
same element: correct in one simulator and not in another, and invisible to a
testbench that only ever changes one of them per cycle.

---

## 5. Open/closed: why a descriptor has to be told its expansion is final

An allocation with `alloc_open` leaves the macro unclosed. It accumulates child
completions normally but **cannot retire**, because the set of children it is
waiting for is not final. Retiring on a bitmap that is still growing is the same
failure as retiring on an arrival count, one step later: the producer has not
said how many children to expect, so "all bits set" means "all the ones I have
seen so far".

`close_valid / close_index / close_gen` marks the expansion final, and is
generation-checked like a completion: a close aimed at a recycled slot is
reported stale, not applied. Phase `close-gate` covers the complete case, the
stale-generation close, and the close into a dead slot.

---

## 6. Conservation, and what a flush must not touch

Three counters are exported so the ROB's conservation law is checkable rather
than asserted in prose:

```
alloc_total == retired_total + squashed_total + occupied
```

Every allocated macro is accounted for exactly once: retired, squashed, or still
held. This is what makes *"the free count is restored exactly after a flush"* a
measurement rather than a claim.

The architectural history of a ROB **is** these counters. A flush therefore
touches none of them, and the phase checks all of them:

```
MISMATCH flush: the flush disturbed committed history: retired_total went from
6 to 0                                                      <- mutant 4, below
```

Phase `flush` retires six macros, allocates twenty speculative ones (one
exceptional, half completed), flushes, and then requires: `occupied == 0`;
`retired_total` unchanged; `squashed_total == 20`;
`alloc_total` unchanged; `gen_counter` unchanged; `o_free == ROB_ENTRIES`; no
valid head; no retirement acknowledged; the pre-flush completion still rejected;
and the first allocation *after* the flush carrying generation
`gen_counter_before_flush`, i.e. the sequence continued rather than restarted.
The ROB is then refilled to capacity to show it is fully usable again.

---

## 7. Invariants asserted on every cycle of every phase

Not in one place — in `Harness::Invariants`, called from the per-cycle
comparison, so they hold in the random soak as much as in the directed phases.

| # | Invariant | What it catches |
|---|---|---|
| I1 | `alloc_total == retired_total + squashed_total + occupied` | a macro lost or double-counted anywhere |
| I2 | `occupied <= ROB_ENTRIES` and `o_free == ROB_ENTRIES - occupied` | capacity not enforced, free count drifting |
| I3 | `gen_counter == alloc_total` | a generation counter that moves on something other than an allocation — **a flush that rewinds it breaks this on the flush cycle itself** |
| I4 | the four completion reports are mutually exclusive, and exactly one is present iff a completion was offered without a flush | overlapping or missing reports |
| I5 | `retire_ack` implies `head_ready` | retirement past a macro that is not ready |
| I6 | `head_done_cnt <= head_num_uops`, `head_done_cnt == popcount(head_done_mask)`, and the mask has no bits outside the child count | **the card's duplicate-arrival failure in mechanical form** |
| I7 | `head_replay` implies an exceptional live head, and an exceptional macro is never ready | head blocking, and an exception being lost |
| I8 | an unclosed macro is never ready | retirement on a bitmap that is still growing |
| I9 | `retired_total` and `squashed_total` are monotonic | **a flush that cleared committed history** |

Beyond the per-cycle comparison, phase `full-depth` holds all 64 slots live,
keeps the buffer at capacity while it wraps repeatedly, **sweeps every slot
through the observation port on every cycle**, and checks the retirement order
against a queue of tags in allocation order — at every retirement, not only at
the head. The sweep is the difference between "the head is right" and "the
descriptor three slots behind the head is right too".

---

## 8. Coverage that discriminates

`sim/unit/tb_rob.cpp` compares **every output against an independent C++ shadow
on every cycle** of every phase. The shadow is written from the contract prose in
`rtl/core/mosaic_rob.sv`, not from the RTL's structure, and shares no code with
it. Its one simplification is stated rather than hidden: it models the
elaboration in which the entry count fills the slot-index space, which is true of
every profile in `config/geometry/` today; the start-up geometry check fails
loudly rather than letting the shadow quietly model the wrong thing.

Geometry is read from the elaborated DUT (`o_rob_entries`, `o_index_w`,
`o_max_uops`, `o_id_w`, `o_pc_w`, `o_num_uops_w`), so **the driver contains no
ROB depth at all**.

| Phase | What it discriminates |
|---|---|
| `reset-state` | the documented cold state; nothing retirable; every slot invalid with a zeroed descriptor |
| `out-of-order-children` | 3 children in the order 2, 0, 1 (the card's case); 8 children in strict reverse; a child index outside the expansion |
| `duplicate` | the same child three times: reported every time, counted once, bitmap and completeness unchanged |
| `wrap-generation` | the card's recycle scenario, then 274 allocations with **every** slot recycled, then every live slot probed with its oldest generation |
| `full` | capacity: refusal reported, occupancy and every slot proven undisturbed, then a strictly in-order drain of all 64 |
| `flush` | committed history, exact free-count restoration, generation continuity, post-flush usability |
| `exception` | exception at dispatch and exception at a child completion; eight retirement requests that must all be refused; the macro behind it complete and still not retiring |
| `close-gate` | an open macro that cannot retire, a stale close, a close into a dead slot |
| `full-depth` | all 64 slots live, kept full while wrapping, swept every cycle, retirement order checked at every step |
| `random-soak` | 12000 cycles of mixed stimulus with deliberate stale, duplicate, dead-slot and out-of-range completions |

The soak asserts its own coverage rather than assuming it — a random campaign
that never reached capacity and never produced a duplicate would pass while
testing nothing. The coverage it achieved in this run:

```
soak: 3248 accepted, 563 duplicate, 3017 stale, 2876 out-of-range,
      1468 refused-at-capacity, 373 exceptional, 90 flushes, seed 1
```

Three real defects in the *testbench* were found this way and fixed in the test,
each of which had been quietly weakening a check:

1. `Alloc()` read `alloc_index` / `alloc_gen` **after** the edge. Those are
   combinational from the allocation pointer, which has already moved, so every
   completion was aimed at the next macro's slot. The failure surfaced as
   `expected 1 child complete, got 0`.
2. `Retire()` returned the live `retire_ack` read after the edge, by which time
   the head has moved and the answer has fallen again.
3. The wrap scenario computed the recycle distance without the `+ 1` described
   in §3, so it recycled nothing while looking like it did.

The driver now reads event reports with the clock low and state from a snapshot
taken after the rising edge, because those are two different questions and mixing
them silently weakens a check.

---

## 9. Commands run, with output

All commands are from `/Users/flare/MosaicRV`.

### 9.1 Lint — acceptance criterion 1

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_rob \
      -Ibuild/p0/rtl -Irtl/core -Irtl/common rtl/core/mosaic_rob.sv
- V e r i l a t o r   R e p o r t: Verilator 5.052 2026-09-05 rev vUNKNOWN-built20260905
- Verilator: Built from 0.052 MB sources in 4 modules, into 0.069 MB in 3 C++ files needing 0.000 MB
- Verilator: Walltime 0.012 s (elab=0.001, cvt=0.006, bld=0.000); cpu 0.012 s on 1 threads; allocated 11.812 MB
exit=0
```

Zero warnings. `slang-tidy` is a second opinion on the same file:

```
$ slang-tidy --std 1800-2017 -I build/p0/rtl rtl/core/mosaic_rob.sv
[LoopBeforeResetCheck] PASS          [NoLatchesOnDesign] PASS
[UndrivenRange] PASS                 [AlwaysCombNonBlocking] PASS
[AlwaysFFAssignmentOutsideConditional] PASS   [RegisterHasNoReset] PASS
[UnusedSensitiveSignal] PASS         [OnlyAssignedOnReset] PASS
[XilinxDoNotCareValues] PASS         [AlwaysFFBlocking] PASS
[NoDotVarInPortConnection] PASS      [EnforceModuleInstantiationPrefix] PASS
[GenerateNamed] PASS                 [CastSignedIndex] PASS
[OnlyANSIPortDecl] PASS              [NoImplicitPortNameInPortConnection] PASS
[NoDotStarInPortConnection] PASS     [NoLegacyGenerate] WARN
[NoOldAlwaysSyntax] PASS             [AlwaysCombBlockNamed] WARN
[EnforcePortSuffix] WARN             [EnforcePortPrefix] PASS
slang exit=0
```

The three WARNs are identical to the ones `rtl/core/mosaic_predictor.sv` produces
— verified by running the tool on both files side by side — so this module is at
the project's existing standard and not introducing a new one:

```
$ for f in rtl/core/mosaic_predictor.sv rtl/core/mosaic_rob.sv; do ... done
--- rtl/core/mosaic_predictor.sv
[NoLegacyGenerate] WARN
[AlwaysCombBlockNamed] WARN
[EnforcePortSuffix] WARN
--- rtl/core/mosaic_rob.sv
[NoLegacyGenerate] WARN
[AlwaysCombBlockNamed] WARN
[EnforcePortSuffix] WARN
```

### 9.2 Other geometries elaborate clean

```
$ verilator --lint-only -Wall ... -GROB_ENTRIES=48 rtl/core/mosaic_rob.sv            # truncation arm
exit=0
$ verilator --lint-only -Wall ... -GROB_ENTRIES=8 -GROB_INDEX_W=3 rtl/core/mosaic_rob.sv
exit=0
$ verilator --lint-only -Wall ... -GROB_ENTRIES=8 -GROB_INDEX_W=3 -GMAX_UOPS=5 rtl/core/mosaic_rob.sv
exit=0
```

The 48-entry run is what proves the `g_idx_may_truncate` arm is elaborated and
type-correct, since no shipping profile reaches it. It found a real latent defect:
`squashed_total + {24'd0, occ_cnt}` silently truncated for any `OCC_W != 7`.
It is now an explicit width-relative zero-extension.

**Known limitation, stated rather than buried:** an index space *wider* than the
entry count needs (8 entries with `ROB_INDEX_W = 6`) elaborates the range check
but trips 41 `WIDTHTRUNC` warnings, because Verilator's static array-index width
check cannot see through the range ternary. No profile configures a ROB that way
— the geometry file pairs the two — and the runtime behaviour is correct; it is
the lint that cannot follow it. Recorded rather than papered over with a local
`lint_off`.

### 9.3 The registered case — acceptance criterion 2

```
$ python3 tools/run_unit.py --case rob.out_of_order_children
PASS rob.out_of_order_children    task=I-016

$ cat results/unit/rob.out_of_order_children/run.log
$ /Users/flare/MosaicRV/build/p0/unit/rob.out_of_order_children/rob.out_of_order_children --case rob.out_of_order_children --out /Users/flare/MosaicRV/results/unit/rob.out_of_order_children --seed 1 --max-cycles 200000
RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 4882 accepted allocations; soak: 3248 accepted, 563 duplicate, 3017 stale, 2876 out-of-range, 1468 refused-at-capacity, 373 exceptional, 90 flushes, seed 1
```

Exit 0.

### 9.4 Not seed-1-fragile

```
$ for s in 1 2 3 7 12345; do ./build/p0/unit/rob.out_of_order_children/rob.out_of_order_children --case rob.out_of_order_children --seed $s --max-cycles 200000 | tail -1; echo "  exit=$?"; done
RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 4882 accepted allocations; soak: 3248 accepted, 563 duplicate, 3017 stale, 2876 out-of-range, 1468 refused-at-capacity, 373 exceptional, 90 flushes, seed 1
  exit=0
RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 6452 accepted allocations; soak: 3269 accepted, 441 duplicate, 3035 stale, 2846 out-of-range, 800 refused-at-capacity, 461 exceptional, 148 flushes, seed 2
  exit=0
RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 5288 accepted allocations; soak: 3182 accepted, 501 duplicate, 3098 stale, 2890 out-of-range, 1253 refused-at-capacity, 414 exceptional, 120 flushes, seed 3
  exit=0
RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 5895 accepted allocations; soak: 3264 accepted, 414 duplicate, 3059 stale, 2855 out-of-range, 1017 refused-at-capacity, 427 exceptional, 128 flushes, seed 7
  exit=0
RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 5276 accepted allocations; soak: 3206 accepted, 499 duplicate, 3066 stale, 2812 out-of-range, 1246 refused-at-capacity, 383 exceptional, 117 flushes, seed 12345
  exit=0
```

Only the soak's random stimulus is seeded; the directed phases are identical
across all five, which is why the comparison count and cycle count do not move.

### 9.5 The driver is held to the project's own stricter standard

The Makefile's `lint-cpp` target compiles `sim/**.cpp` on their own, without the
generated runtime and without Verilator's headers:

```
$ clang++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -isystem /opt/homebrew/Cellar/verilator/5.052/share/verilator/include \
      -I build/p0/unit/rob.out_of_order_children/obj_dir -I sim/common \
      -I build/p0/sim -fsyntax-only sim/unit/tb_rob.cpp
exit=0
```

Zero warnings from this file. (Verilator's own headers are `-isystem` for the
same reason the Makefile gives: they are not `-Wextra` clean.)

---

## 10. Mutation testing — acceptance criterion 4

`MOSAIC_ROB_MUTANT_<n>` blocks are **off in the shipping build**. For each one
three separate facts are recorded, because a `-D` whose `` `ifdef `` block was
never written compiles to the shipping build and "passes" vacuously — that has
happened twice in this project:

1. the `` `ifdef `` block **exists** in the RTL,
2. the mutant binary **differs** from the shipping binary, so the define changed
   code rather than defining nothing,
3. the run **exits 1** with a `RESULT FAIL` line.

The harness is reproduced in full at the end of this section so the evidence is
re-runnable.

### Mutant table

| # | Define | Injected defect | Phase that catches it | Observed failure |
|---|---|---|---|---|
| 1 | `MOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT` | `complete = (done_mask != 0)` instead of `done_mask == expected_mask` — the card's "last-uop-arrives-is-complete" | `out-of-order-children` | `a 3-child macro was called complete after its last-numbered child alone arrived -- exactly the 'last-uop-arrives-is-complete' failure` |
| 2 | `MOSAIC_ROB_MUTANT_NO_GEN_CHECK` | identity is liveness alone; the generation comparison is removed | `wrap-generation` | `cycle 109: cmp_accepted: expected 0, got 1` — the victim's late completion accepted into the live macro that recycled its slot |
| 3 | `MOSAIC_ROB_MUTANT_NO_FULL_CHECK` | `alloc_ok` does not consult capacity | `full` | `cycle 1205: alloc_ok: expected 0, got 1` for the allocation offered into the full ROB |
| 4 | `MOSAIC_ROB_MUTANT_FLUSH_CLEARS_COMMITTED` | a flush rewinds `retired_total` **and** `gen_counter` | `flush` | `the flush disturbed committed history: retired_total went from 6 to 0` |
| 5 | `MOSAIC_ROB_MUTANT_NO_DUP_REPORT` | the duplicate is absorbed silently — not counted twice, but not reported either | `duplicate` | `cycle 33: cmp_duplicate: expected 1, got 0` |
| 6 | `MOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION` | `head_ready` ignores the exception bit | `exception` | `an exceptional macro is reported ready to retire` |

Mutant 4 is worth spelling out: the rewound *generation counter* is the dangerous
half, not the counter that reads zero. A completion still in flight for a
squashed macro then matches the generation of whatever macro next lands in that
slot and is accepted as a real child completion of it — the wrap hazard, reached
through the flush path instead of the wrap path. Invariant I3 catches it on the
flush cycle itself, and phase `flush` catches it by name.

### Per-mutant evidence, verbatim

```
=== shipping build ===
shipping run exit=0
RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 4882 accepted allocations; soak: 3248 accepted, 563 duplicate, 3017 stale, 2876 out-of-range, 1468 refused-at-capacity, 373 exceptional, 90 flushes, seed 1

MUTANT: -DMOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT
  ifdef block: present (1 occurrence)
  build: ok
  binary differs from shipping build: YES
  run exit=1
  MISMATCH out-of-order-children: a 3-child macro was called complete after its last-numbered child alone arrived -- exactly the 'last-uop-arrives-is-complete' failure: expected contract holds, got contract violated
  RESULT FAIL rob.out_of_order_children contract violated: out-of-order-children: a 3-child macro was called complete after its last-numbered child alone arrived -- exactly the 'last-uop-arrives-is-complete' failure

MUTANT: -DMOSAIC_ROB_MUTANT_NO_GEN_CHECK
  ifdef block: present (1 occurrence)
  build: ok
  binary differs from shipping build: YES
  run exit=1
  MISMATCH wrap-generation: cycle 109: cmp_accepted: expected 0, got 1[alloc v=0 tag=0 pc=0x0000000000000000 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=1 i=0 g=0x0 uop=0 exc=0 | retire=0 flush=0 obs=0]: expected contract holds, got contract violated
  RESULT FAIL rob.out_of_order_children contract violated: wrap-generation: cycle 109: cmp_accepted: expected 0, got 1[...]

MUTANT: -DMOSAIC_ROB_MUTANT_NO_FULL_CHECK
  ifdef block: present (1 occurrence)
  build: ok
  binary differs from shipping build: YES
  run exit=1
  MISMATCH full: cycle 1205: alloc_ok: expected 0, got 1[alloc v=1 tag=999 pc=0x00000000deadbeef n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=0 i=0 g=0x0 uop=0 exc=0 | retire=0 flush=0 obs=0]: expected contract holds, got contract violated
  RESULT FAIL rob.out_of_order_children contract violated: full: cycle 1205: alloc_ok: expected 0, got 1[...]

MUTANT: -DMOSAIC_ROB_MUTANT_FLUSH_CLEARS_COMMITTED
  ifdef block: present (1 occurrence)
  build: ok
  binary differs from shipping build: YES
  run exit=1
  MISMATCH flush: the flush disturbed committed history: retired_total went from 6 to 0: expected contract holds, got contract violated
  RESULT FAIL rob.out_of_order_children contract violated: flush: the flush disturbed committed history: retired_total went from 6 to 0

MUTANT: -DMOSAIC_ROB_MUTANT_NO_DUP_REPORT
  ifdef block: present (1 occurrence)
  build: ok
  binary differs from shipping build: YES
  run exit=1
  MISMATCH duplicate: cycle 33: cmp_duplicate: expected 1, got 0[alloc v=0 tag=0 pc=0x0000000000000000 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=1 i=0 g=0x0 uop=1 exc=0 | retire=0 flush=0 obs=0]: expected contract holds, got contract violated
  RESULT FAIL rob.out_of_order_children contract violated: duplicate: cycle 33: cmp_duplicate: expected 1, got 0[...]

MUTANT: -DMOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION
  ifdef block: present (1 occurrence)
  build: ok
  binary differs from shipping build: YES
  run exit=1
  MISMATCH exception: an exceptional macro is reported ready to retire: expected contract holds, got contract violated
  RESULT FAIL rob.out_of_order_children contract violated: exception: an exceptional macro is reported ready to retire
```

Every mutant is caught by the phase it was designed for. No mutant is caught
"by accident, somewhere earlier" — which is a consequence of putting the directed
phases before the random soak, and of the fact that the per-cycle shadow
comparison and the named assertions live in the same phase for each mechanism.

### The harness, for re-running

```sh
#!/bin/bash
set -u
cd /Users/flare/MosaicRV
ROOT=/tmp/robmut; rm -rf "$ROOT"; mkdir -p "$ROOT"

build() {  # $1 = define ("" for shipping), $2 = output dir
  local define="$1" out="$2"; mkdir -p "$out"
  local cmd=(verilator --cc --exe --build -j 0 -O2 --x-assign unique --x-initial unique
             --top-module mosaic_rob_tb -Mdir "$out/obj_dir"
             -I/Users/flare/MosaicRV/build/p0/rtl -I/Users/flare/MosaicRV/build/p0/sim
             -I/Users/flare/MosaicRV/rtl/core -I/Users/flare/MosaicRV/rtl/common
             -CFLAGS -I/Users/flare/MosaicRV/sim/common -CFLAGS "-O2 -std=c++17 -Wall"
             -o "$out/rob.out_of_order_children")
  [ -n "$define" ] && cmd+=("-D$define")
  cmd+=(rtl/core/mosaic_rob.sv sim/tb/mosaic_rob_tb.sv sim/unit/tb_rob.cpp
        sim/common/sim_common.cpp)
  "${cmd[@]}" > "$out/build.log" 2>&1
}

build "" "$ROOT/shipping" || exit 1
"$ROOT/shipping/rob.out_of_order_children" --case rob.out_of_order_children \
    --seed 1 --max-cycles 200000; echo "shipping run exit=$?"

for D in MOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT MOSAIC_ROB_MUTANT_NO_GEN_CHECK \
         MOSAIC_ROB_MUTANT_NO_FULL_CHECK MOSAIC_ROB_MUTANT_FLUSH_CLEARS_COMMITTED \
         MOSAIC_ROB_MUTANT_NO_DUP_REPORT MOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION; do
  out="$ROOT/$D"; echo; echo "MUTANT: -D$D"
  n=$(grep -c "\`ifdef $D" rtl/core/mosaic_rob.sv)
  echo "  ifdef block: $( [ "$n" -ge 1 ] && echo "present ($n occurrence)" || echo ABSENT )"
  build "$D" "$out" || { echo "  build: FAILED"; continue; }
  echo "  build: ok"
  cmp -s "$ROOT/shipping/rob.out_of_order_children" "$out/rob.out_of_order_children" \
    && echo "  binary differs from shipping build: NO  <-- changed nothing" \
    || echo "  binary differs from shipping build: YES"
  "$out/rob.out_of_order_children" --case rob.out_of_order_children \
      --seed 1 --max-cycles 200000 > "$out/run.log" 2>&1
  echo "  run exit=$?"
  grep -E '^(MISMATCH|RESULT)' "$out/run.log" | sed 's/^/  /'
done
```

---

## 11. Two decisions worth Main's attention

1. **`rtl/core/filelist.f` does not list `mosaic_rob.sv`.** That file is not in
   this package's ownership list and a sibling may be editing it, so it was left
   alone. Nothing in the build reads it — `Makefile`'s `RTL_SRCS` and
   `tools/lint_rtl.py` both glob `rtl/**/*.sv`, so lint, slang and the runner
   all pick the module up automatically — but for tidiness it should be added
   next to the other core sources.

2. **The testbench wrapper reaches the geometry by package *scope reference*, not
   by a second `` `include ``.** `sim/tb/mosaic_rob_tb.sv` needs the ROB's port
   widths to narrow its fixed 32-bit driver interface, and the generated
   `mosaic_cfg_pkg.svh` has **no include guard**. Including it in a second file
   in the same compilation is a duplicate package declaration — Verilator
   `MODDUP` and slang `-Wduplicate-definition`, both errors here. Verified:

   ```
   $ verilator --lint-only --top-module a -I/tmp/inc /tmp/inc/a.sv /tmp/inc/b.sv
   %Warning-MODDUP: Duplicate declaration of package: 'mosaic_cfg_pkg'
   $ slang-tidy --std 1800-2017 -I /tmp/inc /tmp/inc/a.sv /tmp/inc/b.sv
   error: duplicate definition of 'mosaic_cfg_pkg' [-Wduplicate-definition]
   ```

   Referring to `mosaic_cfg_pkg::MOSAIC_ROB_ENTRIES` after the package has been
   declared needs no include at all, and both tools accept it; Verilator
   requires the declaring file to come first on the command line, which
   `tools/run_unit.py` already guarantees for every case. So there is exactly
   one geometry in the compilation — the generated package — and the wrapper
   derives its port widths from it with the same rules `mosaic_rob` uses, then
   reads the elaborated values back out as `o_*_w` so the driver checks the two
   against each other instead of trusting the derivation. The alternative
   considered and rejected was hardcoding `6` and `12` in the wrapper, which is
   the second copy of the geometry this project has been bitten by before.

---

## 12. Known limitations, stated rather than buried

* **One completion port per cycle.** p0 has two ALUs; the result FIFO is where a
  second result waits. Widening the port would mean arbitrating two writes to one
  slot's bitmap in a single cycle, which has no defined resolution (§1).
* **One allocation and one retirement per cycle**, while `MOSAIC_RENAME_WIDTH`
  and `MOSAIC_RETIRE_WIDTH` are 2. Dual retirement is I-017's to add; the ROB
  exposes `head_ready` so a second head can be qualified once the first has been
  accepted. Dual allocation needs two live identities per cycle, which is a port
  width decision I-016 deliberately does not make unilaterally.
* **No oldest-exception search.** The card asks for "the earliest exception /
  replay needed". The ROB is in order, so the earliest exception *is* the head:
  `head_replay` names it, and `head_pc` gives recovery its target. A scan would
  be a redundant search over an already-ordered queue.
* **The shadow models one elaboration.** See §8. A non-power-of-two entry count
  would need the index-truncation policy modelled in C++ as well.
* **An index space wider than the entry count needs trips 41 `WIDTHTRUNC`
  warnings** (§9.2). Runtime behaviour is correct; the lint cannot follow the
  range ternary. No profile configures a ROB that way.
* **The observation port is a verification surface.** It is total and gated, but
  nothing functional should read it; the head view is the interface.

---

## identity width correction (integration follow-up)

### The defect

`config/contracts/interfaces.json` (the frozen I-002 contract, the authority)
declares, for the dispatch/ROB identity:

- `rob_index` = `clog2(rob_entries)` = 6
- `rob_gen` = `clog2(rob_entries)+1` = 7

and `build/p0/rtl/mosaic_id_pkg.svh` carries exactly `MOSAIC_ID_W_ROB_INDEX = 6`
and `MOSAIC_ID_W_ROB_GEN = 7`. `mosaic_iq.sv` builds its uop identity as
`{rob_index(6), rob_gen(7), uop_index(3)}`; `mosaic_retire.sv` reads `rob_id` as
`{RET_GEN_W=7, RET_TAG_W=7}`; `mosaic_uop_pkg` aliases
`mosaic_id_pkg::macro_id_t`.

`rtl/core/mosaic_rob.sv` was the outlier. It re-derived its own widths instead of
reading the contract:

```
localparam int unsigned TAG_W = 2 * ROB_INDEX_W,   // 12
localparam int unsigned GEN_W = 2 * ROB_INDEX_W    // 12
```

A completion that round-trips through the IQ therefore carries a 7-bit
`rob_gen`, while the ROB stored and compared a 12-bit one. Zero-extending the
7-bit value is not a fix: the contract's 7-bit counter puts generation 128 into
slot 0 (and 128 aliases 0), so the ROB's 12-bit compare and the producers'
7-bit value disagree after 128 allocations, for the life of the program. The
symptom would be a stale completion rejected when it should be accepted, or
accepted when it should be rejected -- precisely the ABA failure the generation
exists to prevent. The real defect is the disagreement itself: a width two
documents disagree about is what I-002 exists to prevent.

### The contract line violated

`config/contracts/interfaces.json`, the dispatch interface's `identity_fields`:
`rob_index` has `expr: "clog2(rob_entries)"` / `min_bits: 6`, and `rob_gen` has
`expr: "clog2(rob_entries)+1"` / `min_bits: 7`. The generated
`build/p0/rtl/mosaic_id_pkg.svh` is that line materialised, and the ROB now reads
it rather than writing the same width down a second time in a different
expression.

### The fix

`rtl/core/mosaic_rob.sv`:

- includes `mosaic_id_pkg.svh` at file scope (the header carries its own include
  guard and lint pragmas now);
- `TAG_W = mosaic_id_pkg::MOSAIC_ID_W_PRF_TAG` (7) and
  `GEN_W = mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN` (7);
- the header and parameter rationale for `2 * ROB_INDEX_W` is replaced by the
  correct argument: 7 bits gives 128 generations against the 64-entry ROB, and
  `2**GEN_W > ROB_ENTRIES` is the *proof*, not a margin chosen by taste -- a slot
  is not handed out again until its previous owner has retired or been squashed,
  so a completion can only be outstanding for a slot recycled at most
  `ROB_ENTRIES` allocations ago;
- one new negative control, `MOSAIC_ROB_MUTANT_GEN_LOW_BITS_ONLY` (below).

Every other behaviour is unchanged: this is a width correction, not a redesign.

`sim/tb/mosaic_rob_tb.sv`:

- `ID_W = 2 * INDEX_W` is gone. `TAG_W` and `GEN_W` each name their own contract
  constant, and every port is narrowed with its own width -- `alloc_tag`,
  `head_tag`, `obs_tag`, `head1_tag` are tags; `close_gen`, `cmp_gen`,
  `alloc_gen`, `head_gen`, `obs_gen`, `head1_gen` are generations.
- `o_id_w_o` is replaced by `o_tag_w_o` and `o_gen_w_o`.
- the header comment records that the wrapper must derive from the contract and
  never from a formula it invented. The old `ID_W = 2 * INDEX_W` repeated the
  RTL's mistake exactly, which is why the wrapper could not catch it.

`sim/unit/tb_rob.cpp`:

- `ShadowRob` takes `tag_w` and `gen_w` instead of one `id_w`; a tag is masked
  with `tag_w_` and a generation with `gen_w_` (no literal mask anywhere).
- the start-up geometry check replaces the invented `id_w == 2 * index_w` with
  the contract relation `gen_w == index_w + 1` (rob_gen = clog2(rob_entries)+1,
  rob_index = clog2(rob_entries)) plus `tag_w >= index_w`.
- the wrap probe now names the **previous** occupant's generation (tracked per
  slot), not the first-ever one. With the 7-bit modulus, a run of 274 allocations
  reaches 128 allocations past a slot's first occupant and the first-ever
  generation legally comes round again; the immediately previous occupation is
  always inside the `2**GEN_W > ROB_ENTRIES` window and is stale by
  construction. `Alloc` records the tag the slot actually stores (masked), and
  the full-depth retirement-order queue stores masked tags.

### Evidence

Registration (`python3 tools/run_unit.py --profile p0 --case
rob.out_of_order_children`): `PASS rob.out_of_order_children task=I-016`, exit 0.
Verilator lint on `rtl/core/mosaic_rob.sv` and slang-tidy on the same file:
`exit=0`, with only the three pre-existing WARN classes (`NoLegacyGenerate`,
`AlwaysCombBlockNamed`, `EnforcePortSuffix`) the module already carried. Scoped
`clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow` on `tb_rob.cpp`:
`exit=0`.

The full mutant sweep, re-run against the corrected widths, reproducing the
report's §10 harness with the new control added:

```
=== shipping build ===
shipping run exit=0
  RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 4882 accepted allocations; soak: 3249 accepted, 563 duplicate, 3014 stale, 2878 out-of-range, 1468 refused-at-capacity, 373 exceptional, 90 flushes, seed 1

MUTANT: -DMOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT
  ifdef block: present (1 occurrence); build: ok; binary differs: YES; run exit=1
  MISMATCH out-of-order-children: a 3-child macro was called complete after its last-numbered child alone arrived -- exactly the 'last-uop-arrives-is-complete' failure
MUTANT: -DMOSAIC_ROB_MUTANT_NO_GEN_CHECK
  ifdef block: present (1 occurrence); build: ok; binary differs: YES; run exit=1
  MISMATCH wrap-generation: cycle 109: cmp_accepted: expected 0, got 1
MUTANT: -DMOSAIC_ROB_MUTANT_NO_FULL_CHECK
  ifdef block: present (1 occurrence); build: ok; binary differs: YES; run exit=1
  MISMATCH full: cycle 1205: alloc_ok: expected 0, got 1
MUTANT: -DMOSAIC_ROB_MUTANT_FLUSH_CLEARS_COMMITTED
  ifdef block: present (1 occurrence); build: ok; binary differs: YES; run exit=1
  MISMATCH flush: the flush disturbed committed history: retired_total went from 6 to 0
MUTANT: -DMOSAIC_ROB_MUTANT_NO_DUP_REPORT
  ifdef block: present (1 occurrence); build: ok; binary differs: YES; run exit=1
  MISMATCH duplicate: cycle 33: cmp_duplicate: expected 1, got 0
MUTANT: -DMOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION
  ifdef block: present (1 occurrence); build: ok; binary differs: YES; run exit=1
  MISMATCH exception: an exceptional macro is reported ready to retire
MUTANT: -DMOSAIC_ROB_MUTANT_GEN_LOW_BITS_ONLY
  ifdef block: present (1 occurrence); build: ok; binary differs: YES; run exit=1
  MISMATCH wrap-generation: cycle 109: cmp_accepted: expected 0, got 1
```

Each mutant is rebuilt, differs from the shipping binary, and exits 1; the
base build exits 0 with zero failing checks, so every mutant's failure count
delta is **+1 failing check against the base**. The six existing mutants fail in
exactly the phase recorded in §10.

**The new control, and why the width needs one.** The wrap scenario's victim
carries generation 0 and the occupant that recycles its slot carries 64, so it
is bit 6 of the corrected 7-bit value that separates them.
`MOSAIC_ROB_MUTANT_GEN_LOW_BITS_ONLY` compares `slot_gen[GEN_W-2:0]` against
`cmp_gen[GEN_W-2:0]`, dropping that bit, and the stale completion is accepted at
cycle 109 -- the same failure the missing-generation-check control produces. A
12-bit generation would have separated the pair in a lower bit and this mutant
would not have been caught, so the control fails exactly when the generation
width is wrong. (Stated as design intent: the mutant is verified to fail under
the 7-bit contract; it is not claimed to fail under a 12-bit build, which would
require re-editing the width.)

### The two retire cases, and whose they are

`commit.head_block_and_dual` and `retire.head_block_and_dual` instantiate this
ROB through `mosaic_retire`. After the width correction both fail at cycle 393,
deterministically:

```
MISMATCH in-order: cycle 393: slot 0: generation disagrees: shadow 128, buffer 0
```

The failure is in `sim/unit/tb_retire.cpp`, not in the ROB. That file's shadow
passes a mask *value* where a width is expected: `gen_mask_ = Mask(~0u, gen_w)`
is `0x7f`, and `e.gen = Mask(next_alloc_gen_++, gen_mask_)` (and the
reconciliation `Mask(e->gen, shadow_->gen_mask())`) treat `0x7f` as a bit count
that is `>= 32`, so `Mask()` returns its argument unchanged and the shadow's
generation never wraps. With the old 12-bit ROB generation the shadow never
reached 128; with the contract's 7-bit generation the DUT wraps at 128 while the
shadow keeps counting. `sim/tb/mosaic_retire_tb.sv` already names the contract
widths (`TB_ROB_TAG_W`/`TB_ROB_GEN_W` from `mosaic_id_pkg`), so that lane is
mid-migration. The retile lane was told; `sim/unit/tb_retire.cpp` was **not**
edited here, per the "do not touch another lane's file" rule. This is the only
acceptance item not green, and it is red because of another lane's in-flight
edits.

### Files changed by this follow-up

| File | Change |
|---|---|
| `rtl/core/mosaic_rob.sv` | derive `TAG_W`/`GEN_W` from `mosaic_id_pkg`; include `mosaic_id_pkg.svh`; corrected rationale; new mutant |
| `sim/tb/mosaic_rob_tb.sv` | split `ID_W` into contract-derived `TAG_W`/`GEN_W`; geometry readback `o_tag_w_o`/`o_gen_w_o`; corrected comment |
| `sim/unit/tb_rob.cpp` | shadow takes `tag_w`/`gen_w`; contract geometry check; previous-generation wrap probe; masked tags |
| `results/reports/I-016-rob.md` | this section (appended; the report above is unchanged) |

### Commands run

```
python3 tools/run_unit.py --profile p0 --case rob.out_of_order_children      # PASS
python3 tools/run_unit.py --profile p0 --case commit.head_block_and_dual     # FAIL (retire lane)
python3 tools/run_unit.py --profile p0 --case retire.head_block_and_dual     # FAIL (retire lane)
verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_rob \
    -Ibuild/p0/rtl -Irtl/core -Irtl/common rtl/core/mosaic_rob.sv            # exit=0
slang-tidy --std 1800-2017 -I build/p0/rtl rtl/core/mosaic_rob.sv            # exit=0
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -isystem $VERILATOR_ROOT/include \
    -I build/p0/sim -I sim/common -I build/p0/unit/rob.out_of_order_children/obj_dir \
    sim/unit/tb_rob.cpp                                                      # exit=0
make lint-cpp    # fails on sim/unit/tb_fifo.cpp: 'Vmosaic_fifo_tb.h' not found --
                 # its obj_dir was never built in this workspace, unrelated to this change
bash /tmp/robmut.sh   # shipping PASS, seven mutants each exit=1
```
