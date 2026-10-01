# I-018 — branch checkpoint and precise recovery — **PASS**

CASE: `recovery.checkpoint_exact_restore` and its alias `recovery.nested_branch_full_queues`
(one binary, two registered names). Work package: I-018.

> **Both registered cases pass**, the two language gates are clean on the module,
> the driver is clean under the `lint-cpp` flags, and **eight mutants** were
> rebuilt, run, and each shown to fail with a named first divergence against a
> **green** base. Read "What is NOT verified" before using any of it: the case
> proves the module's own contract, not the machine around it.

---

## Files

| File | State |
|---|---|
| `rtl/core/mosaic_recovery.sv` | **changed**: three real defects fixed, the credit kill rule changed from epoch-only to age-bounded, two mutants re-expressed, header rewritten where it documented the old rule |
| `sim/tb/mosaic_recovery_tb.sv` | **unchanged** (no port was added or removed) |
| `sim/unit/tb_recovery.cpp` | **changed**: six harness/shadow defects fixed, four phase expectations corrected to the contract, one phase added |
| `results/reports/I-018-recovery.md` | this file |

Nothing outside those four was touched. `tests/unit/registry.json`,
`config/status/implementation_status.json` and `results/PROGRESS.md` were **not**
edited (the two case entries were already present and correct). No `git` command
that rewrites the working tree was run.

---

## Exact commands and real output

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_recovery \
      -Ibuild/p0/rtl rtl/core/mosaic_recovery.sv
- Verilator Report: Verilator 5.052 ... into 1.143 MB in 5 C++ files   [exit 0]

$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_recovery.sv
0 errors (STYLE-2 port-suffix warnings only, as for every core module)

$ python3 tools/run_unit.py --profile p0 --case recovery.checkpoint_exact_restore
PASS recovery.checkpoint_exact_restore task=I-018

$ python3 tools/run_unit.py --profile p0 --case recovery.nested_branch_full_queues
PASS recovery.nested_branch_full_queues task=I-018

$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow <lint-cpp includes> sim/unit/tb_recovery.cpp
clean (the `make lint-cpp` target as a whole reports four errors, all in
sim/unit/tb_rename.cpp -- another lane's file, mid-flight, referencing signals
that lane's RTL edit has not landed yet; nothing of mine is implicated)
```

The green line, verbatim, for both names:

```
RESULT PASS recovery.checkpoint_exact_restore recovery contract holds: 419470 shadow
comparisons over 6810 cycles; directed: 175 restores, 1058 stale redirects, 12 killed
redirects, 730 stale responses, 598 duplicate responses, 730 credits returned, 1812
journal-bound refusals; soak: 208 checkpoints, seed 1
```

**First mismatch: none.** The base case is green, so every mutant's exit code is
evidence on its own — unlike a mutant run against a red base, which is not. The
driver is fail-fast (one `MISMATCH` line then exit), so a mutant's "delta" is
`1` mismatch against the base's `0`; what carries the information is *which named
check* fires and *how far* the run got. The cycle positions below are that
progress measure, and they are printed by the driver itself.

---

## Which side was wrong, for every disagreement this package resolved

The project's rule is to decide from the specification and the contract, with the
DUT not privileged by default. Every entry below names the side judged wrong and
the authority used.

**1. Shadow wrong — the undo journal recorded the wrong `prev_valid`.** The
shadow wrote `j_prev_` *after* step 1 had already set `gen_valid[scan] = true`, so
every journal entry claimed "this tag was valid before", and a restore then marked
never-allocated tags valid again. The RTL reads its register (the pre-edge value),
which is what the documented rule says (`undo: gen_valid = prev_valid`). The
failure the ticket quoted — `dbg_gen_valid` expecting `0xffffdfff00000000` and
getting `0x00001fff00000000` — is the shadow *adding* validity the checkpoint
never had; the DUT's value was the checkpoint's own, and the phase's independent
bit-identity assertion had already passed at that instant. Fixed in the shadow.

**2. RTL wrong — a restore leaked one tag per *killed* checkpoint.** A squash
consumes its own checkpoint *and every younger one*, and each of those branches
allocated its own destination, which is deliberately not journalled. The restore
undid only the restored checkpoint's own tag, so the younger branches' tags stayed
allocated forever with their mappings rewound away: a leak that only surfaces as
spurious exhaustion, which is the failure class the module's own header names.
The contract is unambiguous (`the restore returns the state to the checkpoint
exactly`), so the RTL was wrong. Fixed in the RTL, modelled in the shadow, and now
caught by a named check (`dbg_free_mask` in the new phase; mutant 8 exists to prove
it). Note the *shadow* had copied the same behaviour — an independent model is only
independent if it is written from the contract, and this one had not been.

**3. RTL wrong — an allocation at the undo bound was accepted, and the journal
index wrapped.** The block's own comment says "An allocation at the bound is
refused, so `alloc_new_valid` is false and nothing is journalled". The code
reported the refusal (`alloc_journal_full`, `journal_overflow`) but did not apply
it, so the allocation was taken and the journal write landed on the index its
6-bit width wrapped to — overwriting a live entry, which makes a later restore
return a state the machine never passed through while reporting success. Found by
AddressSanitizer, not by reading: the shadow mirrored the RTL's acceptance and then
wrote `j_tag_[64]` out of bounds, and the heap corruption that followed is what
produced the *changing* first divergence the integration lane reported
(`dbg_ckpt_epoch` at one build, `dbg_ckpt_alloc_ptr` at another, same cycle). Both
sides were wrong against the documented contract; both now refuse. Mutant 7.

**4. RTL wrong — reset left the allocation-order tracker set.** `alloc_seen` and
`last_alloc_gen` are control state whose only job is to report a producer that
presents generations out of order, and reset must clear the *validity* bit or the
first allocations after reset are compared against the previous run and report
`alloc_gen_regress` on a producer that did nothing wrong — a status output that
lies for `ROB_ENTRIES` allocations after every reset. The design rule is that
reset cost is control state; the value is data guarded by that bit.

**5. RTL wrong — the credit kill was by epoch, and it killed older work.** This is
the one design-level change. The module documented "a redirect raises the epoch; a
response carrying an older epoch is dropped", which discards the result of any
instruction that was already in flight — including instructions *older* than the
branch, which the squash does not own. `docs/implementation-plan.md` §1.3 forbids
exactly that ("the PRF/result epoch check must respect the older instruction that
is still live across a redirect, and must not simply reject every old epoch"), the
I-018 card lists "killing all old-epoch work" as a blocking failure ("older head
deadlock"), and `docs/architecture-review.md` §89 says the same in the design's own
words. The authority above the module's self-documentation is the plan, so the
**documented rule was wrong**, not just the shadow. The kill is now an **age
boundary**: a redirect cancels the reservations whose recorded *owner generation*
is at or above the redirecting branch's, and leaves older reservations alone; the
epoch stays as the `(slot, epoch)` identity that stops a response for a recycled
slot being read as the live reservation's. The header section that documented the
old rule was rewritten rather than left to disagree with the code. Mutant 6 is the
old rule, and it now fails on the check that exists to forbid it.

**6. Shadow wrong — the commit was applied to the wrong register.** `Apply` used
the *allocation's* destination (`rd = alloc_rd`) for the commit path instead of
`commit_rd`, so a commit landing in a cycle with an unrelated allocation was
published to that allocation's register, and the supersede/free used the same
wrong index. Fixed (the RTL uses `commit_rd`). This is why the free-list phase's
commit "did not move the committed map".

**7. Shadow wrong — the soak's counters were computed after the state changed.**
`Tally` re-derived the view from the shadow *after* `Apply`, so a redirect appeared
to have consumed the checkpoint it was restoring to: `restores` stayed at zero
through hundreds of restores and every one of them was booked as a stale redirect.
The view is now taken once per cycle, before anything mutates, and shared by the
comparison and the tally.

**8. Shadow wrong — the credit kill used post-mutation checkpoint validity.** The
checkpoint-stack block runs before the credit block inside `Apply`, so
`ck_valid_[p.ck]` was already false by the time the kill tested it and the kill
never fired; the RTL reads its *register*, which is still true in that cycle. The
captured pre-edge `restore` flag is now used. Same class as (7): asking the model
about the state the cycle is producing instead of the state it acted on.

**9. Harness wrong — the `retire_block` invariant compared two different cycles.**
It tested this cycle's combinational port against the retire bit remembered from
the *previous* redirect cycle. It now uses the current cycle's stimulus, which is
what the port is a function of.

**10. Harness wrong — a don't-care was compared.** `o_rob_flush_from` is a
(value, valid) request pair: the RTL drives the value unconditionally from
`ck_tail[restore_ck]`, and with no request `restore_ck` falls back to slot 0 — a
slot the contract deliberately does not reset (validity is `ck_valid`). Comparing
that value against the shadow's zeroed model of an unreset array compares two
don't-cares. The value is now compared only where it is a value; the valid bit is
still compared every cycle. Weakening the port to drive zero when it is not
meaningful would instead make every consumer learn a second rule.

**11. Harness wrong — the checkpoint bundle's dead slots were compared raw.**
`Snapshot::Key()` appended the raw bundle despite its own comment claiming the
contents are masked, and `Harness::Compare` compared the six bundles raw as well;
a dead slot's leftover contents are declared don't-care by the RTL (the arrays are
not reset and a consumed checkpoint is not scrubbed). Both now project to the live
slots, exactly as `CkptEqual` and `FirstDifference` already did. Validity is still
compared raw — *which* checkpoints are live is the fact that must not drift.

**12. Harness wrong — four phase expectations encoded rules the design cannot or
must not implement.** Each was fixed by moving the expectation to the contract,
never by loosening a comparison:
* `oldest-redirect` asserted that a lone younger redirect is *held* because an
  older **checkpoint** is live. An older checkpoint is not a pending redirect, and
  the only event that resolves the older branch is the redirect the unit would be
  refusing to take — the deferral would be permanent. The phase now checks the
  rule that exists (age orders the redirects offered in a cycle, the older wins,
  the younger is reported killed) and that the older checkpoint survives a younger
  branch's squash.
* `late-response` and `credit-stress` asserted that any pre-redirect response is
  stale — the forbidden rule of defect 5 — so their reservations had to be built on
  the correct side of the age boundary, and both halves are now checked.
* `wrap` asserted `Tail() != 0 || CkptDepth() == 0`, which is true of no invariant
  in the design (a wrapped tail of zero is legal), and that the free list cannot
  drain — but nothing in that phase commits or retires, so surviving allocations
  legitimately accumulate. It now checks what it claims: every rewind inside the
  wrap is bit-identical to its checkpoint, and more tags were handed out than the
  ring holds.
* `journal-bound` captured the *post*-allocation instant as the checkpoint's
  content and had its branch allocate a tag, which made the undo bound unreachable
  (the free list ran out first). It now takes the checkpoint instant before the
  branch, uses a branch that writes x0, and reaches the bound exactly.
* `free-list-exact` also captured after the branch's allocation, and committed
  x10's *live speculative* mapping — which, after a live checkpoint, can be a
  younger instruction's mapping that the next restore then frees. It now commits a
  pre-checkpoint mapping and expects the free mask to be the checkpoint's **plus**
  the tag the commit superseded, because a commit's free is permanent and is not
  undone.
* The soak drove two inputs the design declares invalid: a commit of an arbitrary
  tag (installing a committed mapping onto a tag nobody owns) and a live release
  of a tag a mapping still names (one physical register with two owners). Both are
  now constructed legally, and the soak still drives the *stale* forms of
  everything — a stale release now derives its wrong generation from the tag's own,
  because a random draw that happens to match is a live release of whatever tag it
  landed on, i.e. the corruption itself rather than a test.

**13. Harness wrong — the soak could never exercise the kill path.** It drove one
resolve port at a time, and one live candidate is simply taken, so `redirect_killed`
was structurally zero. It now sometimes presents two live redirects in one cycle.
(This is the coverage check doing its job: it caught a stimulus gap, not a DUT
defect.)

---

## The new phase (the card's own case name), and the card's two blocking rules

`nested-branch-full-queues` runs after the directed phases. "Full" is stated, not
implied: the checkpoint stack is driven to `CKPT_DEPTH` and the next request is
*refused and reported* while the same cycle's allocation still succeeds, and the
free list is driven until the machine cannot allocate at all — the allocation
after that is refused for **exhaustion** and not for the undo bound, which is
possible because a checkpoint branch's own destination consumes a tag the journal
never counts. At p0 the two bounds *cannot* coincide: the journal bound equals the
number of allocatable tags (`entries - arch == rob`), so every tag would have to be
a journalled allocation to reach it.

Then an **inner checkpoint is restored first and an outer one after it** — the
sequence the card's case name asks for, and one that a "hold the younger redirect
until the older branch resolves" rule would make impossible. Each restore is
compared wholesale against its checkpoint's own instant (free mask, generations,
generation-valid, speculative map, tail, rotation point, journal).

The card's blocking rules are checked directly, by name:

* **"Not by clearing the whole PRF/free list."** After the inner squash the phase
  asserts the free count is below the reset population, and that a tag owned by an
  instruction older than the restored branch, the surviving outer branch's own
  destination, and tags allocated before the checkpoint are all still allocated.
  A recovery that cleared everything would fail here even if its "restore" were
  self-consistent.
* **"Not by killing older-epoch work it does not own."** The older instruction's
  slow result is delivered after the squash and must be **accepted**, its writeback
  must land, and a replay of that writeback must be reported as a duplicate. The
  same half is checked in `late-response` and `credit-stress`.

The card's pass criterion — mispredict **plus an older exception plus a same-cycle
allocation** — is one cycle: the older exception on port 0 and a younger mispredict
on port 1, with `alloc_valid` and `rob_retire` asserted. The phase asserts the older
(exception) wins and publishes its fault flag, the younger is reported killed, the
allocation is refused as *squashed* (not as exhausted or journal-full), the retire
is blocked, and the state is still the outermost checkpoint's. Tags and credits are
conserved in both directions: `free + owned <= entries`, the free count equals its
mask, credits outstanding is zero, and the cumulative return count is zero because
every reservation the phase made was accepted.

---

## Mutant table

Method, per mutant: `run_unit.VERILATOR_FLAGS.append('-DMOSAIC_RECOVERY_MUTANT_…')`,
`run_unit.build_case('p0', 'recovery.checkpoint_exact_restore', …)`, then the
binary directly with `--case recovery.checkpoint_exact_restore --seed 1
--max-cycles 200000`, requiring exit 1. Every `-D` name below was confirmed to
appear in the RTL (`grep -c`, each exactly once) so none of them builds the
shipping design.

| mutant | exit | first divergence (named check, cycle) |
|---|---|---|
| *(base, no define)* | **0** | PASS — 419,470 comparisons, 6,810 cycles, 0 mismatches |
| `WRONG_CHECKPOINT` | 1 | `nested-ckpt: cycle 61: o_restore_ckpt: expected 0, got 1` |
| `APPLY_STALE_RSP` | 1 | `late-response: cycle 100: rsp_accepted: expected 0, got 1` |
| `DOUBLE_CREDIT` | 1 | `late-response: cycle 101: credit_return: expected 0, got 1` |
| `NO_FREE_RESTORE` | 1 | `exact-restore: not bit-identical — free_mask word 0: expected 0xffffe00000000000, got 0x0000200000000000` |
| `NO_TAIL_RESTORE` | 1 | `exact-restore: not bit-identical — free_mask word 0: expected 0xffffe00000000000, got 0xffffc00000000000` |
| `EPOCH_ONLY_RSP` | 1 | `late-response: cycle 99: rsp_accepted: expected 1, got 0` |
| `ACCEPT_AT_BOUND` | 1 | `free-list-exact: cycle 419: alloc_accepted: expected 0, got 1` |
| `RESTORE_SELF_ONLY` | 1 | `nested-ckpt: cycle 64: free_count: expected 63, got 62` |

Two of the five mutants delivered with the module **did not elaborate** and had
never been run, so they proved nothing at all: `WRONG_CHECKPOINT` scanned downward
over an `int unsigned` (`c >= 0` is constant-true — Verilator stops with an
infinite-loop error and then an internal error), and `NO_FREE_RESTORE` drove its
count to a constant, so the undo loop's own comparison folded and the build stopped
on the warning. Both were re-expressed so that they build and still inject the
described defect: the scan runs upward and keeps the highest valid index (the same
wrong answer), and the negative control is now an *enable on the undo's effects*
rather than a zero count. Two mutants were added (`EPOCH_ONLY_RSP`,
`ACCEPT_AT_BOUND`) plus one for the leak (`RESTORE_SELF_ONLY`), so the three
defects fixed in the RTL each have a mutant that fails on the check written for it.

---

## What is NOT verified

1. **The orphan-response path is unreachable at p0.** A credit id is 4 bits and the
   table is 16 slots, so `CRED_NEVER_TRUNCATES` folds `rsp_slot_in_range` to a
   constant and no stimulus can name a slot outside the table. The driver no longer
   pretends to check it (the old phase asserted an orphan for `0x3f`, which the
   wrapper narrows to slot 15). It is verified by inspection only.
2. **The pending-redirect queue is unreachable.** A live, non-stale candidate is
   taken in the cycle it is presented, so nothing is ever appended: `o_rdq_depth` is
   zero in every cycle of every phase, and the queue half of the candidate set never
   decides an arbitration. The per-cycle comparison agrees with a shadow that
   implements the same rule, so what is verified is *that the rule implies an empty
   queue* — the queue's own pointer, count and storage are not exercised at all.
3. **No checkpoint is ever released on a correct resolution.** Nothing but a
   restore consumes one, so after `CKPT_DEPTH` concurrent branches every push is
   refused forever. The refusal is exercised and reported, but the machine it
   implies cannot run. Fixing it needs a port this module does not have (a branch
   identity at commit); `rob_retire` is one bit with no generation, so "retire
   releases the oldest checkpoint" would release a checkpoint for a branch that has
   not necessarily retired.
4. **`o_rob_flush_from` has no consumer.** The precise ROB flush is still a gap in
   I-016 (the module publishes the requested point, `flush_valid`, as before).
5. **Nothing outside this unit is verified.** The card's recovery closure spans
   frontend, IQ, LSQ, FU and result queues; this case covers the rename layer, the
   free list, the generations, the journal, the epoch and the credit table only.
   The integration boundary with I-017 is a design statement here, not a test.
6. **One response per cycle**, no fabric back-pressure or multi-cycle response
   latency; the credit table's port behaviour under a real FIFO is not modelled.
7. **Single allocation per cycle**, so nothing here exercises I-014's two-wide
   rename or the same-cycle bypass.
8. **The checkpoint's stored `epoch` is dead state**: captured, exported and
   compared, but nothing reads it — the restore raises the epoch rather than
   restoring it. It is listed for deletion below.
9. **Cycle-level timing is not measured.** `make lint-cpp` as a whole currently
   reports four errors in `sim/unit/tb_rename.cpp`, another lane's in-flight file;
   the command above runs the same flags on this package's driver alone and it is
   clean.

---

## duplicated state and the split

**State this module currently duplicates** with `mosaic_rename` (I-013/I-014), in
full: the speculative RAT layer (`spec_map`/`spec_gen` per architectural
register), the committed map (`cmt_map`/`cmt_gen`, written only from
`commit_valid`), the free-list bitmap (`free_bits`), the per-tag generation table
with its `gen_valid` validity bits (`gen`/`gen_valid`) and the written flags
(`wb_done`), the undo journal (`j_tag`/`j_prev_valid`/`j_len`), the checkpoint
stack's recorded speculative-map copies (`ck_spec`) together with the tail,
rotation point, journal mark and epoch each one snapshots, and the allocation
order pointer (`alloc_ptr`). That is two implementations of one piece of
architectural state, and the failure mode is exactly the one described: two owners
of the speculative map means a commit that updates one and not the other.

**Load-bearing for I-018 whatever the split is**, and what I would keep here:

* the checkpoint *controller* — push, refuse when full or in a squash cycle,
  consume the restored checkpoint and every younger one;
* the **age-minimum arbiter** over the queue and both resolve ports, with the stale
  classification (a redirect naming a generation with no live checkpoint is
  dropped and reported) and the killed count;
* **epoch publication and the flush fan-out** — `squash`, `retire_block`,
  `o_restore_ckpt`, `o_rob_flush_from_valid` / `o_rob_flush_from`;
* the **in-flight result/credit reservation table** — one slot per reservation, the
  `(slot, epoch)` identity, the credit returned exactly once on the response that
  acknowledges the cancel, and the age-bounded cancel.

**What I would delete from this module** once rename owns the maps, the free list,
the generation table and the journal: `spec_map`/`spec_gen`, `cmt_map`/`cmt_gen`,
`free_bits`/`gen`/`gen_valid`/`wb_done`, `j_tag`/`j_prev_valid`/`j_len`/`j_overflow`,
`alloc_ptr` and the tail, `ck_spec` (if rename holds the per-branch snapshot),
`ck_epoch` (dead as noted above), and the **pending-redirect queue** with its
storage, count and pointer — it is the same kind of dead weight, unreachable under
the arbitration rule, and it is the one deletion I did *not* make because it is an
interface change (`o_rdq_depth`, `o_redirect_src`) and the integration lead is
sequencing a restructuring of exactly this boundary.

**The one substantive argument I owe you, and it is against a constraint you
relayed.** "Rename's squash restores the speculative map **from the committed
map**, so a squash is only exact when the redirecting branch is the ROB head" is
true of that restore method, and it is why I have not merged the two: I-018 is
*required* to restore a branch that is not the head. The card's pass criterion is
that an older slow result still completes, and this case restores an **inner**
checkpoint while an **outer** branch is still live and then restores the outer one
— a head-only squash rule makes that sequence impossible, and the older result's
completion under a younger squash is precisely what the plan's §1.3 demands. The
exact restore of a non-head squash comes from the **per-checkpoint speculative-map
snapshot** (this module's `ck_spec`) or an equivalent reversible delta; it cannot
come from the committed map, which does not know the younger mappings the branch
is about to erase. So if rename is to own the state, rename must keep a per-branch
snapshot and the restore must be a copy-back.

What *can* be checked in hardware, and cheaply, is what this module already checks:
a redirect must name a live checkpoint (otherwise it is stale, reported, and not
taken), the rollback boundary is the oldest *redirect* offered in the cycle (the
arbiter's total order), and the flush point published with a redirect is the
checkpoint's own tail, not the ROB head. If the integrated machine wants to forbid
non-head squashes outright, that is a *narrowing* of I-018's contract, not a
refinement of it, and it should be decided against the card's pass criterion rather
than added as a precondition.

---

## Two named gaps the case now states instead of hiding

**Checkpoint release.** Recorded in the module header; see "What is NOT verified"
item 3 for the port that would close it.

**The undo bound is a throttle, and the case now proves it is one.** Over a long
run of branches the journal's marks advance with the allocation count and nothing
retires journal entries (a retire inside a window does not advance the oldest
checkpoint's mark), so the window reaches its bound and allocations are **refused
and reported** until a restore rewinds to an older mark. `free-list-exact` asserts
that this is reached (`journal-bound refusals: 1812` in the soak line, plus the
phase's own coverage assertion) because a long run that never reached it would pass
every check above while proving less than it looks.
