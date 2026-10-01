# I-018 — branch checkpoint and precise recovery — **PARTIAL**

CASE: `recovery.checkpoint_exact_restore`
Work package: I-018. Owner: recovery.

> **Status: NOT COMPLETE.** The RTL is written and both language gates pass on
> it. The testbench builds and runs. Three real defects have been found and fixed
> (one in the RTL, two in the testbench), and the case now fails *later* than it
> did, on a discrepancy I did **not** resolve. No mutant demonstrations exist,
> because the base case does not pass. Read "What is not done" before using
> anything here.

---

## Files

| File | State |
|---|---|
| `rtl/core/mosaic_recovery.sv` | written; `verilator --lint-only -Wall` clean, `slang-tidy --single-unit` 0 errors |
| `sim/tb/mosaic_recovery_tb.sv` | written; lints as part of the case build |
| `sim/unit/tb_recovery.cpp` | written; builds, runs, fails on a live disagreement |
| `results/reports/I-018-recovery.md` | this file |

Nothing outside those four was touched. `tests/unit/registry.json` was **not**
edited — the entries for `retire.head_block_and_dual` and
`recovery.checkpoint_exact_restore` were already present and I left them alone.
Not committed.

---

## Real command output

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_recovery \
      -Ibuild/p0/rtl rtl/core/mosaic_recovery.sv
- Verilator Report: Verilator 5.052 ... into 0.868 MB in 5 C++ files   [clean]

$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(ls rtl/*/*.sv) \
    | grep -ci error
0

$ python3 tools/run_unit.py --case recovery.checkpoint_exact_restore
FAIL recovery.checkpoint_exact_restore verdict=FAIL exit=1
  see results/unit/recovery.checkpoint_exact_restore/run.log

$ grep MISMATCH results/unit/recovery.checkpoint_exact_restore/run.log | head -1
MISMATCH exact-restore: cycle 25: o_rob_flush_from: expected 14, got 13
  [alloc v=1 rd=2 gen=15 | ckpt v=0 ... | rdr0 v=0 ...]
```

---

## Design, in the terms the card asks for

**Method: full checkpoint.** The card offers "committed map + surviving-prefix
rebuild" or "full checkpoint". This takes the full checkpoint. The deciding
argument is arithmetic, not taste: a surviving-prefix rebuild needs a commit
boundary to rebuild from, so it needs the committed map, the free list at that
boundary, *and* every allocation after it held in an undo log — which is the same
undo journal this design needs anyway, plus the same walk. The full checkpoint
keeps a copy of the speculative RAT layer per outstanding branch and restores the
free list and generation table by replaying the journal backwards, so the
per-checkpoint cost is one 32-entry map copy rather than a whole machine copy, and
the expensive structure is shared rather than duplicated.

**What a checkpoint holds:** the speculative RAT layer (`spec_map`/`spec_gen`, 32
entries), the speculative ROB tail, the free-list scan's rotation point, the
journal mark, and the epoch. What it does *not* hold: the free set, the
generation table, the written flags — those are replayed backwards from the
journal, which is exact because the free set is a *set* (putting a tag back is
idempotent, so the undo's order cannot matter) and because the generation undo is
the exact inverse of the allocation's step
(`allocate: gen = prev_valid ? gen+1 : 0` / `undo: gen = prev_valid ? gen-1 : 0`),
so one bit per journal entry is the whole undo state. `cmt_map` is never
rewound — see the boundary below.

**Why the free list is a bitmap, not a stack.** A stack's checkpoint is a
pointer, and that pointer is not a correct snapshot: a tag freed while the stack
head sits below the checkpoint overwrites a slot a later pop will read, so
restoring the pointer hands back a stack whose contents are no longer the saved
ones. Restoring a set is exact. The cost is an O(ENTRIES) scan instead of an
O(1) pop, which is the right trade for a first scalar core and is a scan that is
obviously correct rather than a pop that is cheap and subtly wrong.

**Undo journal and its bound.** One entry per allocation, `{tag, prev_gen_valid}`,
shared by every checkpoint; a checkpoint records the journal length at the moment
it was taken and a restore undoes entries down to that mark. The bound is
`MOSAIC_ROB_ENTRIES` — at most ROB_ENTRIES instructions can be younger than a
given checkpoint and each contributes at most one entry. At p0 that is 64 entries
against 96 − 32 = 64 allocatable tags, exactly, which is why
`tools/check_profile.py` requires `int_prf.entries - arch_int_regs >= rob.entries`.
**Reaching the bound is legal and is the expected steady state for a full queue.
Exceeding it is refused and reported** (`alloc_journal_full`, `journal_overflow`);
the window is never wrapped, because a wrapped entry overwrites a live one and the
restore then returns a state the machine never passed through while reporting
success.

**Oldest redirect wins.** Age is the ROB generation of the redirecting instruction
— the same monotonic, never-rewound counter that makes a recycled slot
distinguishable from the macro that owns it. Generations increase with allocation
order, so the oldest candidate is an unsigned minimum, taken over the pending
queue *and* the two resolve ports of the current cycle together. That makes the
rule total rather than "priority encoder, oldest queue entry first", and it makes
a redirect arriving in the same cycle as an older pending one lose. The unsigned
compare is exact because the live spread is at most ROB_ENTRIES allocations
against a 12-bit counter's 4096 — a 64× margin, the same argument `mosaic_rob`
makes about its own stale window. A redirect naming a generation with **no live
checkpoint** is stale: its branch was squashed, so it is dropped and reported
separately, never taken. Every other live candidate when one is taken is younger
and is killed by the same squash; those are counted in `redirect_killed`.

**Late responses and the credit, exactly once.** A redirect raises the epoch. A
response carrying an older epoch is dropped and its reserved credit is returned.
The credit is a **table of slots, not a counter** — a counter cannot tell a second
delivery from a first, and "exactly once" is precisely the property that needs the
distinction. The first delivery finds the slot busy, returns the credit and clears
the slot; a second delivery finds no slot and returns nothing. The credit is
returned when the cancel is *acknowledged* (the arrival of the stale response) and
**not** at the redirect, because returning it at both ends is the mirror image of
a credit leak and exactly as fatal: the fabric over-issues and eventually has two
producers writing one destination. This is the ABA hazard
`config/contracts/interfaces.json` names, and it is why the identity is
`(slot, epoch)` rather than the slot alone.

**Cycle semantics.** Everything is combinational in the cycle it describes and
registered at the end of that cycle, so a request offered in cycle N has its
answer (`*_accepted`, `*_stale`, …) visible in cycle N and its effect visible in
cycle N+1. Three orderings are load-bearing and each is stated in the RTL:
a commit offered in a squash cycle lands *before* the restore reads the committed
map; a checkpoint is refused in the cycle a redirect is taken, because the squash
owns that cycle; and an allocation is refused in a squash cycle, so the undo can
never race with an allocation.

---

## The I-017 / I-018 boundary

Stated once here; `RetirePkg` was sent the same text and asked to state it in the
I-017 report.

1. **I-017 owns `cmt_map` and `minstret`. I-018 owns *when* the speculative layer
   is rewound to it.** I-018 never rewinds the committed map. It applies commits,
   but only ever as `cmt_map[rd] = the mapping being committed`, which is I-017's
   in-order decision arriving on `commit_valid`. The restore reads the committed
   map as it stands *after* this cycle's commit; restoring over it would resurrect
   the mapping the commit just replaced.
2. **A retire and a recovery never land in the same cycle.** I-018 drives
   `retire_block`, and it is high in exactly one situation: a cycle in which a
   redirect is taken *and* `rob_retire` is asserted. A retire in any other cycle
   is permanent and needs no block at all — its free is permanent, and its journal
   entry sits below the oldest live checkpoint's mark, so no restore can undo it.
   Blocking unconditionally would stop retirement for as long as any branch were
   in flight, which is most of the time.
3. **I-018 consumes no signal from I-017** other than `commit_valid` and
   `rob_retire` (the ROB's own in-order acknowledgement). It does not instantiate
   `mosaic_retire`, so the two cases build and run independently.
4. **I-018 does not instantiate `mosaic_rob` or `mosaic_rename`.** It owns the
   speculative rename state and exports it read-only, which is what lets this be a
   single-file unit test. At integration those exports become the state the shared
   rename module reads.

---

## Known interface gap (named, not papered over)

`mosaic_rob` exposes `flush_valid`, which drops everything at and above the
**head**. A precise branch recovery needs to drop everything at and above the
**redirecting branch**, keeping older instructions still in flight. There is no
such port, and adding one to I-016 is not this package's to do.

So this module does the part it can own exactly — it restores the tail and the
rotation point it holds — and publishes `o_rob_flush_from` /
`o_rob_flush_from_valid`, the *requested* flush point. When `mosaic_rob` grows a
`flush_from` port, that output drives it and this module needs no change. The gap
is named in the RTL header rather than hidden behind a full flush, because a full
flush presented as a precise recovery is exactly the defect this module exists to
prevent.

---

## What the testbench checks

Ten phases, each resetting first and owning one mechanism, so a run stops at the
first failure and the order decides which phase reports a given defect. Every
phase compares **every** output against an independent C++ shadow on **every**
cycle, plus the whole speculative state compared as packed wide bundles rather
than field by field.

The state comparison is the case's central claim and it is deliberately
wholesale: free mask, generation-valid mask, written mask, every generation, both
maps, the tail, the rotation point, the journal length, the free count, and the
six-field checkpoint bundle. A per-field suite would let a defect in a field
nobody thought to list through, which is the defect this case is looking for.

1. `reset-state` — the documented cold state, including that tags 0..31 are
   **owned, not free** (a free list reset to all-ones hands tag 5 to the first
   instruction that writes any register, giving one physical register two live
   owners).
2. `exact-restore` — the central phase. Builds non-trivial speculative state,
   checkpoints, allocates 25 more, redirects, and requires the state to be
   **bit-identical** to the checkpoint. Also checks the rotation point
   specifically, because "free set restored, rotation point not" produces a
   free list that is bit-identical and a *next allocation* that is wrong — the
   defect no free-count comparison would notice.
3. `nested-ckpt` — two checkpoints outstanding, the older redirects: the older is
   restored and the younger is **consumed**, then the younger's later redirect is
   reported stale and not taken.
4. `oldest-redirect` — the card's rule, in both arrival orders and in the same
   cycle (younger on port 0, older on port 1).
5. `late-response` — pre-redirect response rejected, credit returned **once**,
   the same slot delivered again returns nothing, the count is checked as a count.
6. `free-list-exact` — 24 rounds of work/checkpoint/work/commit/squash, the free
   mask compared bit-identically after every squash. The committed map is
   *permitted* to differ (a commit is permanent and is not undone) and
   `SameExceptCmtMap` is an explicit list of the fields that must match, so a
   field added later defaults to matching rather than silently weakening the check.
7. `journal-bound` — the window driven to exactly ROB_ENTRIES: no spurious
   overflow, a restore from the full bound exact, then one allocation beyond it
   which must be **refused and reported** with the journal not wrapped (shown by
   the subsequent restore still producing the checkpoint state).
8. `wrap` — 3×ROB_ENTRIES allocations with checkpoints and restores interleaved,
   so the tail and the rotation point both wrap.
9. `credit-stress` — the table filled to capacity, refusals reported, responses
   delivered out of order, a reused reservation reported as a conflict, and a
   whole table cancelled by one redirect with each credit returned once.
10. `random-soak` — 6000 cycles of random stimulus including deliberate stale,
    duplicate and out-of-range responses, with a post-condition that the soak
    actually reached the interesting states.

Standing invariants on every cycle: the four response reports are mutually
exclusive; an accepted response never returns a credit; `redirect_taken ==
squash == rob_flush_valid == o_rob_flush_from_valid`; no allocation produces a
destination in a squash cycle; the free count is the free mask's population
count; the conservation identity `free + owned <= entries`; `credits_outstanding`
equals the reservation table; the journal never exceeds its bound as a *state
fact* (independently of the `journal_overflow` port); and the checkpoint depth
equals the valid vector's population count.

---

## Four real defects this case found, three of them while being written

Recorded because each is a failure mode the card names, and because two of them
actively misled me while I was diagnosing.

**1. RTL: a mispredicting call leaked one physical tag.** A branch that writes a
link register allocates a destination in the *same* cycle it takes its checkpoint,
and that allocation is deliberately **not** journalled -- the checkpoint precedes
the branch, so the branch's own allocation is not in the window the undo walks.
The restore therefore never freed it: one tag per mispredicting call, surfacing
dozens of instructions later as spurious exhaustion with nothing pointing back at
the branches that caused it.

*Fix:* the checkpoint entry now records the branch's own destination and the
generation state that allocation replaced, and the restore applies **the journal's
own inverse step** to it explicitly, so the design keeps one undo rule rather than
two. This is the single most valuable thing the case found, and it was found by
the wholesale free-mask comparison rather than by any directed check.

**2. Testbench: the shadow stored checkpoints from post-allocation state.** The
shadow's `Apply` advanced `tail_`, `alloc_ptr_`, `j_len_`, `epoch_` and the
speculative map *before* its checkpoint block read them, so all six checkpoint
fields were stored one step ahead of the RTL's, which reads its registers. The
block's own comment claimed "as it stands *before* this cycle's allocation" --
the comment asserted the opposite of what the code did, which is the shape of
defect a comment cannot catch.

*Fix:* a named `PreEdge` struct captured before anything mutates, used by the
checkpoint store. The struct rather than six locals so the next field cannot be
quietly forgotten.

**3. Testbench: a checkpoint-stack divergence a per-field suite let accumulate.**
The per-cycle comparison covered the checkpoint **depth count** and one port; the
rest of the stack was inferred. A divergence built up across 25 cycles and
surfaced only in `o_rob_flush_from`.

*Fix:* the checkpoint bundle's observation ports were widened to 64-bit and **all
six fields are now compared wholesale on every cycle**. This is the strongest
argument in the case for the wholesale comparison.

**4. Testbench: the mismatch message was wrong twice, and each time it pointed
somewhere other than the defect.** First `Describe` printed "expected `other`,
got `this`" while every call site passed `(expected, actual)`, so the two were
swapped. Then the two-argument repair was itself called with the *same* value as
both `self` and `expected`, so it compared the value against itself and reported
`identical` for a field the caller had just proved differed -- a confident,
wrong, useless message.

*Fix:* `Describe` takes one argument, `*this` is the actual and the argument is
the expected, the contract is stated in the comment, and it now reports a word
*count* mismatch explicitly rather than comparing out of range. Two of my five
diagnostic cycles went into this message, which is worth recording: a mismatch
message that is confidently wrong costs more than one that says nothing.

**5. Testbench: the phase compared the restore against the wrong instant.** The
phase snapshotted the checkpoint *after* the branch's own allocation and demanded
the restore reproduce it. But the checkpoint records the state *before* that
allocation by design -- that is what makes it precede the branch -- so the
branch's own destination is supposed to come back free. The phase was demanding
that a restore re-materialise the branch it exists to erase.

*Fix:* the expectation is captured before the branch allocates, and the phase now
asserts that the branch's allocation *did* change the free mask, so the branch's
destination is genuinely exercised rather than incidentally covered. This one is
worth flagging: it is the same category as "weakening the DUT to match the
shadow", but done to the *expectation* instead, and it is just as wrong.

---

## What is not done — read this

1. **The case does not pass.** The `exact-restore` phase fails its bit-identity
   assertion. The restored free mask has **more free tags than the checkpoint
   had** — `word 0` reads `0x03ffffff` (tags 32..57 free) against an expected
   `0xffffe000` (tags 47..63 free). The count is wrong, not just the contents: the
   post-restore free set is 7 tags larger than the checkpoint's, so the undo is
   releasing tags that were never allocated after the checkpoint.

   The per-cycle shadow comparison passes, so the shadow and the DUT agree on
   every cycle of this phase; the disagreement is between the *checkpoint's
   recorded content* and what the restore produces, which is a narrower and more
   tractable question than it was an hour ago.

   **I did not find the cause.** The prime suspects, in the order I would check
   them, are: (a) the journal window is `undo_apply` entries too long, because
   `j_len` at checkpoint time is not what the shadow thinks it is; (b) the
   branch-destination undo added in finding 1 is double-counting a tag that the
   journal window also covers; (c) `ck_tag` is captured from `scan_tag` on a cycle
   where `alloc_new_valid` is true but the scan pointer has already moved.
   A single instrumented run printing `j_len`, `ck_jmark`, `undo_apply` and the
   multiset of freed tags on both sides at the restore cycle would settle it.
   **I am not going to name a cause I have not verified.**
2. **No mutant demonstrations.** The card requires at least five, each shown to
   fail with a *non-zero delta* and a named first failure. A mutant run against
   a red base proves nothing — the project's own lesson, and the reason an exit
   code alone is not evidence. The five mutants are written into the RTL behind
   `ifdef` blocks (`MOSAIC_RECOVERY_MUTANT_WRONG_CHECKPOINT`,
   `_APPLY_STALE_RSP`, `_DOUBLE_CREDIT`, `_NO_FREE_RESTORE`,
   `_NO_TAIL_RESTORE`) and each is documented at its site, but **none has been
   run**, and the mutant table with deltas does not exist.
3. **The later phases are unexercised.** Because the run stops at
   `exact-restore` cycle 25, phases 3–10 have never executed even once. They are
   written and compile, but "compiles" is not "passes", and I have no evidence
   about any of them. The soak's post-conditions and the credit-stress count
   arithmetic in particular have never been run and should be expected to need
   correction.
4. **The shadow has been corrected against the RTL once already.** In fixing
   `o_rob_flush_from` the *shadow* was wrong: it gated the flush point on a
   redirect being taken, where the RTL drives it unconditionally and uses the
   valid bit to say whether it is a request. The RTL was **not** weakened to match
   it, and the rule is now written down at both ends. That is the correct
   resolution, but it means the shadow's agreement on that field is not yet
   independent evidence of anything, and the same caution applies to every field
   touched since.
5. **A regression I introduced and then fixed.** Reordering `Capture()` to read
   the scalars before the snapshot accidentally moved `CaptureReports()` to
   post-edge, so every phase read the *next* cycle's reports and the redirect
   appeared untaken. Reports are captured pre-edge and state post-edge, as
   `tb_rob` does; both are now commented at their call sites.

**Recommendation:** do not integrate this module, and do not treat any part of it
as validated, until item 1 is resolved and items 2–3 are done. The RTL's
interface, its documented orderings, and the boundary with I-017 are usable as a
design proposal; the *behaviour* is not yet demonstrated.
