# I-018 — the undo journal's retirement drain — **PASS**

CASE: `rename.journal_window` (top `mosaic_rename_tb`, driver `sim/unit/tb_rename.cpp`),
plus the whole blast radius of `rtl/core/mosaic_rename.sv` re-run. Work package: I-018.

> **The behaviour is a defect, not a property, and it is fixed.** The window was
> emptied only by a branch checkpoint or the trap path's restore, never by
> retirement, so it accumulated an entry for every allocation since the last
> checkpoint — including allocations whose instructions had already committed.
> A branchless stretch longer than `MOSAIC_ROB_ENTRIES` (64) allocations then
> clamped it, and `journal_overflow` latched. The window is now the set of
> *uncommitted* allocations: a commit removes its own entry, so the bound is a
> consequence of the ROB geometry rather than an assumption that can be exceeded.
> The case is new, the control mutant reproduces the original report
> (`journal_ovf_seen=1`) verbatim, and all 16 registry cases that elaborate the
> module pass (plus I-018's two recovery cases, which do not elaborate it but
> share the package), `trap.precise_state` now reporting `journal_ovf_seen=0`.
> Read "What is NOT verified": the clamp itself is unreachable at p0.

---

## Files

| File | State |
|---|---|
| `rtl/core/mosaic_rename.sv` | **changed**: the undo window is now a FIFO drained by commit (identity-matched), the clamp flag describes the window in flight, the contract paragraphs are rewritten where they stated the old one, one mutant added, `REN_JIDX_W` moved into the geometry block |
| `sim/unit/tb_rename.cpp` | **changed**: shadow model carries the journal head and drains it, `flush_restore` added to the stimulus/model, conservation identity extended with the drained count, phase 15 added, `rename.journal_window` accepted |
| `sim/tb/mosaic_rename_tb.sv` | **changed**: `flush_restore` exposed and connected (one port, added and wired in one piece; `mosaic_rename` itself gained no port) |
| `results/reports/I-018-journal.md` | this file |

Nothing outside those four was touched. `tests/unit/registry.json`,
`config/status/implementation_status.json`, `results/PROGRESS.md`,
`tests/programs/**` and every other case's driver were **not** edited, and no
tree-rewriting `git` command was run.

---

## The contract, established before anything was changed

### What the plan and the review say

* `docs/architecture-review.md:119` — "每次 rename 记录新旧映射。正常提交更新
  committed map，释放被本次提交覆盖的 **old** physical mapping，而不是释放当前
  结果。… 仅恢复一张 free-list snapshot 会漏掉 snapshot 后先释放再分配的寄存器，
  造成泄漏（AR-011）。首版宜采用**逆序 ROB rollback／分配日志，逐项撤销年轻
  分配**；优化为 checkpoint 时必须保留等价的 allocation delta 账本。"
  *"…the first version should use a reverse ROB rollback / allocation log, undoing
  **young** allocations item by item; when this is optimised into a checkpoint, an
  equivalent allocation-delta ledger must be preserved."* The ledger's content is
  fixed by that sentence: the **young** allocations — the ones no recovery point at
  the committed frontier could spare. It says nothing about keeping the ledger's
  entries after their instructions have stopped being young.
* `docs/architecture-review.md:117` — free list / ROB / IQ / LSQ allocation is one
  handshake transaction and no resource shortage may leave half a mapping. That is
  I-014's atomicity rule; the window is part of the same transaction.
* `docs/implementation-plan.md` **I-013** Pass: "每 physical register 精确处于 free
  或一个合法 live ownership" (*every physical register is exactly free or exactly one
  legal live ownership*). **I-014** Fail: "部分 dispatch stall 后 mapping/free-list
  与 ROB 不一致" (*after a partial dispatch stall the mapping/free-list disagree with
  the ROB*). **I-018** Pass: "全部 live tags/credits 守恒" (*every live tag and credit
  is conserved*).
* `rtl/core/mosaic_recovery.sv`'s "the bound, and its report" states the *other*
  reading explicitly: "because a retire inside a window does not retire journal
  entries, a checkpoint left open across many retires can reach the bound sooner
  than the occupancy suggests. The allocation is refused and reported rather than
  corrupting the journal." That module is I-018's *standalone* controller, and its
  header names why it has no choice: it has no checkpoint-release port, so its
  checkpoints never retire and its journal genuinely cannot drain. It is a workaround
  for a missing port, not a contract for a module whose window is emptied at every
  checkpoint. See "findings for other lanes".

### What the four named cases settle

* `rename.single_width_ownership` (I-013) — allocate / write back / **commit** /
  release, 40 times, with no checkpoint anywhere. Its commit is the module's legal
  release path and it is driven in the same window as the allocation. With the old
  RTL the window grew by one entry per iteration and never dropped (40 entries by
  the end of the phase, still inside the 64-entry bound, which is why the phase
  passed); the case passed because its shadow implemented the same non-drain. It
  does not settle the question by itself — but it is the shortest sequence in which
  a committed allocation is still recorded in the window, which is the state the
  contract has to have an opinion about.
* `rename.same_cycle_chain` (I-014) — the two-wide group, the WAW release accounting
  and the 64-deep window (`twowide-random` deliberately drives the window to its
  bound: "the last free tag is journalled as entry 64"). It proves the bound is
  *reachable* and legal; it does not prove what happens past it.
* `recovery.checkpoint_exact_restore` (I-018) — the case that actually walks a
  journal, and the decisive one. It walks a *mark-based* window: `undo_n = j_len -
  ck_jmark[ckpt]`, and it refuses the allocation when the window is full. Its own
  header says a checkpoint is only released by the restore that consumes it — i.e.
  its journal is bounded by *live checkpoints*, not by the ROB, and its refusal is
  the honest report of that missing release.
* `recovery.nested_branch_full_queues` (I-018) — the nested-checkpoint variant of the
  same walk, with the same refusal.

So the two modules answer two different questions, and the question for
`mosaic_rename` is settled by the geometry the design already relies on elsewhere:
**one uncommitted instruction holds at most one physical tag, and the ROB holds at
most `MOSAIC_ROB_ENTRIES` of them.** Therefore a window whose entries are exactly
the uncommitted allocations can never exceed 64 entries — the number it is sized
for — and no refusal or clamp is ever needed. Any other reading makes the window
unbounded: with a branchless stretch, allocations since the last checkpoint grow
without limit while the ROB occupancy stays at its bound, which is precisely the
reproduction.

**The contract, stated once:**

> The undo window holds exactly the allocations that have not committed. A commit
> removes the entry of the instruction that commits; the entry is recognised by
> its destination identity, never by "a commit happened, so an entry is due". The
> window is emptied by a checkpoint, by the trap path's restore, and by a squash
> consuming it. Its bound is the number of uncommitted instructions, which the ROB
> size justifies, and `journal_overflow` means one thing only: an allocation was
> made that the window in flight could not record.

The alternative reading — "the window holds only the most recent allocations, so
retirement need not free records" — is refuted by the undo itself: the undo must
invert *every* allocation younger than the checkpoint being restored to, and
"younger" is a statement about the commit frontier, not about recency. A window
that kept a committed allocation's entry would hand that tag back to the free set
while the committed map still names it — the two-owners state the standing
invariant rejects — and would step a committed generation back down.

---

## The defect

`rtl/core/mosaic_rename.sv` before this change:

```systemverilog
assign j_push0 = alloc_new_valid && (j_tail < REN_JLEN_W'(REN_ROB));
...
if (alloc_new_valid) begin
  if (j_len_q < REN_JLEN_W'(REN_ROB)) j_len_q = j_len_q + 1;
  else j_overflow_q = 1'b1;          // set, and never cleared before reset
end
if (squash_accepted) j_len_q = 0;    // the only decrement in the module
```

Three consequences, in increasing severity:

1. **The window is not what its name says.** It counts allocations since the last
   checkpoint, not uncommitted allocations.
2. **The flag is sticky.** `j_overflow` is set by a clamp and cleared only by reset;
   no checkpoint, flush or squash clears it. Once any branchless stretch exceeds 64
   allocations, `journal_overflow` is 1 for the rest of the run — which is why
   `trap.precise_state`, whose trap path restores from the committed map and is
   otherwise bit-exact against the model, still printed `journal_ovf_seen=1`.
3. **A clamp is silent.** When the window is full the allocation still happens and
   its entry is *not* recorded, while the flag is only raised for the rest of the
   run: a squash to a checkpoint inside that window then restores less than the
   truth while reporting a flag that cannot be told apart from a clamp twenty
   thousand cycles earlier. That is the hazard the ticket names, and it is the
   defect proper — items 1 and 2 are how it hides.

**Not observed architecturally in the integrated core, and this is the honest
scope of the reproduction.** The core takes a checkpoint on the redirect pulse and
squashes one cycle later (the shipping branch of `mosaic_core.sv`'s "the rename
recovery" wiring, under the branch barrier), so the window at a squash holds at
most the checkpoint cycle's own allocations and the undo is exact. V-014 found
exactly that: no architectural difference. The defect is
real for any caller that keeps a checkpoint open across retires — the discipline
the module's own header invites, and the discipline `mosaic_recovery`'s mark-based
window is built around — and it makes the flag unusable as evidence, which is what
"the machine is unaffected" does not excuse.

---

## The fix

`rtl/core/mosaic_rename.sv`, in the module's own terms:

* The window is a **FIFO**: `j_head` (new, `REN_JIDX_W` bits) names the oldest live
  entry and `j_len` counts the live entries, so a retirement advances the head
  without moving storage. `j_idx0 = head + len` is the tail, where a cycle's
  entries append.
* Each entry now stores `j_gen` — the generation the allocation produced — beside
  `j_tag` and `j_prev_valid`. The undo bit is still one bit; the produced generation
  is stored because "is this entry's owner the instruction retiring?" is an
  **identity match**, and `(tag, gen)` is what identifies an allocation.
* `j_pop0` / `j_pop1` remove the oldest live entry on a commit (`commit_accepted`
  for lane 0, `commit2_accepted` for lane 1), gated on the window being live and on
  the front entry's identity matching the commit's. `commit_accepted` already
  excludes x0 commits, and a commit whose instruction was allocated before the
  window was cleared has no entry at all, so in both cases the match fails and
  nothing is dropped — dropping an entry that is not the committer's would lose a
  live record and leave its tag unreleased on a squash.
* A clear point (`ckpt_valid`, `flush_restore`, or a squash consuming the window)
  empties the window first and this cycle's entries append at index 0 — the
  pre-I-014 rule kept, now with a head.
* The clamp flag is raised when an allocation happens that the window cannot
  record, and **cleared wherever the condition no longer holds**: at any clear
  point, and when the window drains to empty. A clamp raised this cycle leaves a
  non-empty window, so the clear cannot erase a live clamp.
* The undo walks `(head + k) mod ROB`, oldest first — the order the generation
  rollback depends on.
* The header's contract paragraphs and the mutant table are rewritten where they
  stated the old rule.

Why this is the *smallest correct* shape rather than a new port: the module already
receives every commit with its destination identity (`commit_tag`/`commit_gen`,
`commit2_tag`/`commit2_gen`, wired from the descriptor store in `mosaic_core.sv`),
so the retirement drain needs no new interface — nothing in the core's port map
changed, and the two places that instantiate the module still elaborate unchanged.

---

## Exact commands and real output

```
$ python3 tools/lint_rtl.py --profile p0
ok   rtl/core/mosaic_rename.sv: clean as mosaic_rename
...
lint: 34 source file(s) clean                                            [exit 0]

$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)
                                                                         [exit 0]
# only the project-wide pre-existing warnings (STYLE-2 port suffixes, STYLE-13
# unnamed always_comb); no error, none new in mosaic_rename.sv

$ python3 tools/check_records.py
ok records agree: 40 delivered package(s), 50 registered case(s), every claimed
case exists and belongs to the package claiming it                        [exit 0]

$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow <lint-cpp includes> sim/unit/tb_rename.cpp
sim/unit/tb_rename.cpp:2378:12: warning: variable 'windows' set but not used
# the one warning is pre-existing (PhaseRandom); no error, no warning from any
# line this change touched
```

The new case, verbatim:

```
$ python3 tools/run_unit.py --profile p0 --case rename.journal_window
PASS rename.journal_window        task=I-018
RESULT PASS rename.journal_window rename contract holds: 5978 shadow comparisons
over 6022 cycles, 96 entries / 4 banks, seed 1
```

### What the new phase drives (332 shadow-compared cycles)

| step | cycles | the claim it pins |
|---|---|---|
| checkpoint at the reset boundary | 1 | `ckpt_committed` is usable; the window is emptied |
| fill: allocate until one tag is free | 63 | with no commit in the stretch the window holds *every* allocation, and the free set drains to 1 tag; the window reaches `ROB-1` = 63 of 64 |
| branchless stretch: 1 retire + 1 allocation every cycle | 192 | **255 allocations with no checkpoint anywhere**, window held at 63, `journal_overflow` never raised, `dbg_j_len` == the uncommitted count **every cycle** |
| checkpoint over a non-empty window, and its settle cycle | 2 | the checkpoint empties the window even though it is not a committed boundary (`ckpt_committed` is reported 0 — correct: writers are outstanding) |
| drain the outstanding writers | 63 | retires with no window entry drop nothing (no underflow) |
| pre-flush allocations | 5 | five entries in the window when the trap path acts |
| trap-path restore | 1 | `flush_restore` empties the window, `spec := cmt`, `free := ~{tags named by cmt}` |
| squash after the restore | 1 | the restore re-established a recovery point, so the squash is accepted |
| two-lane retire of the two oldest live entries, plus settle | 4 | both entries leave the window on one edge (`j_len` 2 → 0) |

The conservation identity is checked on every cycle of the whole campaign
(`Harness::CheckInvariants`), in its generalised form:

```
free_count == baseline_free + un_journalled_returns − live_window − retired_entries
```

where `retired_entries` is the count the window has drained since the baseline
(earlier commits' tags stay owned by the committed map and never come back, so a
window depth alone would misstate the free set). The trap restore re-establishes
the baseline absolutely, so the identity is skipped on the flush cycle itself and
resumed from the state the restore established.

---

## Control table

Method: the case's registry entry is used to assemble the same source list as
`tools/run_unit.py`, the build directory is **deleted first**, the `-D` is on the
record in that build's `build_command.txt`, and the mutant must have a **different
binary SHA-256** from the shipping one and exit non-zero with a named first
failure. The `-D` name appears exactly once in `rtl/core/mosaic_rename.sv`
(`grep -c` = 1), so no mutant can build the shipping design.

| run | binary | SHA-256 | exit | first divergence |
|---|---|---|---|---|
| shipping, `rename.journal_window` | `build/p0/unit/rename.journal_window/rename.journal_window` | `6093b9de15a98796646ffc558d549fd66afbe8a3b095b64134e31fbcb90c2e99` | **0** | PASS — 5978 comparisons, 6022 cycles, 0 mismatches |
| `MOSAIC_RENAME_MUTANT_JOURNAL_NO_RETIRE_FREE` | `build/p0/unit/rename.journal_window_mutant/…` | `1b0efb0dc4fd20b7b548b48bbb83be8e17fd9e831690f79335b8171a84237ecc` | **1** | `journal-window: cycle 72: the undo window holds 64 entries but the shadow says 63` |
| shipping, `trap.precise_state` (the original repro) | `build/p0/unit/trap.precise_state/trap.precise_state` | `cefa3044c75c593ce0a8feccecc0d9fea4937b05a7e120c4ad2e36e87b69b6b9` | 0 | PASS, `journal_ovf_seen=0` |
| the same `-D` on `trap.precise_state` | `build/p0/unit/trap.precise_state_mutant/…` | `bc7f6c167cbbfda13cb1c409a42213cd6072731351a6c8621351cf6539fc5f05` | 0 | PASS, `journal_ovf_seen=1` — **the reported reproduction, reproduced** |

The mutant is "the window is never drained by retirement" — the defect as it
shipped. On the control's own case it dies at the **first cycle of the branchless
stretch** (cycle 72 = the 64th allocation), where the window is one entry deeper
than the contract allows, and the check that names it is the window-depth
comparison the new phase and the shadow both carry. Raw outputs are kept under
`results/unit/mutants/MOSAIC_RENAME_MUTANT_JOURNAL_NO_RETIRE_FREE/` (the case) and
`…/trap.precise_state/` (the machine-level reproduction).

The pre-existing controls whose code this change touched were re-verified so the
edit did not silently break another package's evidence:

| pre-existing control | exit | first divergence after this change |
|---|---|---|
| `MOSAIC_RENAME_MUTANT_NO_FREE_RESTORE` (single-width) | 1 | `squash: cycle 1527: the free set differs from the shadow at tag 35` — **unchanged** from `results/reports/mutant-reverification-2026-10-01.md` |
| `MOSAIC_RENAME_MUTANT_CKPT_ALLOC_LEAK` (single-width and two-wide) | 1 | `random: cycle 1887: the undo window holds 0 entries but the shadow says 1` — **changed** from the recorded `random: cycle 1890: the free set differs from the shadow at tag 39`; see "findings for other lanes" |
| `MOSAIC_RENAME_MUTANT_NO_BOUNDARY_CHECK` (single-width) | 0 | unchanged: that control is two-wide-only, as recorded |

---

## The original reproduction, before and after

```
$ python3 tools/run_unit.py --profile p0 --case trap.precise_state
# before (HEAD record, results/unit/trap.precise_state/run.log):
traps=35 retires=4740 cycles=12081 comparisons=65917 squash=2 muldiv=21 journal_ovf_seen=1
# after:
traps=35 retires=4740 cycles=12081 comparisons=65917 squash=2 muldiv=21 journal_ovf_seen=0
RESULT PASS trap.precise_state checks=167810 comparisons=65917 cycles=12081 retires=4740 traps=35 seed=1
```

Same cycles, same comparison count, same trap/retire counts — the machine's
architectural behaviour is bit-identical; the only change is the flag that named
nothing. Building that same case with the mutant reproduces `journal_ovf_seen=1`
exactly, which is what makes "the fix removed the flag, not the report" a
statement with evidence behind it.

### Every case that elaborates the module was re-run

```
PASS rename.single_width_ownership task=I-013
PASS rename.same_cycle_chain      task=I-014
PASS rename.journal_window        task=I-018
PASS recovery.checkpoint_exact_restore task=I-018
PASS recovery.nested_branch_full_queues task=I-018
PASS fabric.fixed_two_cluster     task=I-023
PASS core.corpus_sweep            task=I-023
PASS trap.precise_state           task=V-014
PASS core.corpus_branch           task=I-023
PASS core.event_payload           task=I-017
PASS core.mem_program             task=I-023
PASS core.trap_csr_program        task=I-023
PASS core.unwritten_reg_read      task=I-023
PASS fence.code_and_data_order    task=I-037
PASS mmio.exactly_once            task=I-038
PASS commit.head_block_and_dual   task=I-017
PASS retire.head_block_and_dual   task=I-017
PASS retire.width_and_order       task=V-013
```

The first eight are the ones the ticket names; the rest are the other registry
cases whose `rtl`/`sv` list contains `mosaic_rename.sv`, run because a module with
16 consumers should not be changed on the strength of eight of them. The
`rename.single_width_ownership` comparison count is unchanged at 5646 comparisons /
5686 cycles, i.e. the harness change did not weaken any existing phase.

---

## Findings for other lanes (not edited here)

1. **`mosaic_recovery.sv` still documents the other contract.** Its header states
   that a retire inside a window does not retire journal entries and that the
   allocation is refused at the bound — correct *for that module*, whose window is
   mark-based against live checkpoints that never retire (its own "checkpoint
   release gap"). The two modules now differ deliberately, and the difference is
   exactly the missing port its header names. Nothing in I-018's recovery case
   asserts the rename window's contract, so both can be true at once; but a reader
   who takes that paragraph as *the* I-018 journal contract should read this report
   first.
2. **`results/reports/mutant-reverification-2026-10-01.md` has two stale rows.** Its
   `CKPT_ALLOC_LEAK` rows for `rename.single_width_ownership` and
   `rename.same_cycle_chain` record the first divergence as
   `random: cycle 1890: the free set differs from the shadow at tag 39`. With the
   window drained, the checkpoint-cycle leak is caught three cycles earlier and by
   a different check: `random: cycle 1887: the undo window holds 0 entries but the
   shadow says 1` on both cases. The mutant still builds and still fails — only the
   named first failure moved. I did not edit that report because it is another
   lane's current output; the two rows above are the replacement.

---

## What is NOT verified

1. **The clamp itself is unreachable at p0 and therefore never exercised.** One
   uncommitted instruction holds at most one tag and the ROB holds 64, so the
   window cannot exceed its 64 entries and `j_overflow` is never raised in the
   shipping build. The flag's *clear* rules (clear point, drain-to-empty) are dead
   logic at p0 and are verified by inspection only. What the case does verify is
   that the flag is **never** raised over 255 allocations, and the harness compares
   the registered flag against the model on every cycle — but the model's flag is
   also never raised, so this is anti-vacuity, not a positive test of the clear.
2. **No phase drives the window to its bound with a commit in the same cycle.** The
   push capacity is tested against the pre-edge window length, so the design is
   conservative by one entry in that corner (a full window cannot use a slot a
   same-cycle retirement frees). That corner is argued unreachable — a full window
   means every allocatable tag is held — but it is not constructed by any stimulus.
3. **The identity match is exercised for single-width commits, a two-lane drain of
   the two oldest entries, and a checkpoint-cycle clear; it is not exercised for a
   lane-1 commit that matches *while lane 0's does not.** The gate is written for
   it and the shadow models it, but no directed phase drives that combination.
4. **The drain is not exercised through the two-wide allocation group.** The new
   phase drives single-width allocations only, so `j_push1`'s interaction with a
   same-cycle pop is covered by `rename.same_cycle_chain`'s soak, not by a directed
   phase.
5. **The integrated core's checkpoint discipline is unchanged and not re-proved
   here.** The core takes a checkpoint and squashes one cycle later under the branch
   barrier, so its window is always tiny; this report does not claim to verify the
   core's recovery, only that the module's window is now the contract the core
   assumed.
6. **The two maps, the generations and the free list are only checked against the
   shadow**, which now implements the new drain. The shadow was written from the
   contract in the header, not from the RTL, but it is the same author's reading of
   the same prose — the mutants are what keep that honest, and the one mutant here
   guards the drain only.
