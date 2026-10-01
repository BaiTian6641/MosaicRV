# I-017 — in-order retire and the committed map

Cases: `retire.head_block_and_dual`, `commit.head_block_and_dual` (alias).
Top: `sim/tb/mosaic_retire_tb.sv`; driver: `sim/unit/tb_retire.cpp`; DUT: `rtl/core/mosaic_retire.sv`.

**Verdict: both cases PASS.** The DUT was right; the testbench was wrong. `rtl/core/mosaic_retire.sv`
was not modified (`git status` shows only `sim/tb/mosaic_retire_tb.sv` and `sim/unit/tb_retire.cpp`).

The task's quoted first failure (`head-block: cycle 695: rob_squashed_total: expected 2, got 0`) was
against an intermediate state; this report describes the state the case reached after each defect was
removed, and every one of them was on the testbench side.

---

## 1. What decides "which side"

The module's header is the contract, and it pins every disputed behaviour:

* **The trap is decided on the head alone.** Rule 4, and the RTL comment at `head_trap`:
  "A trap is decided on the head alone and is not gated on `flush_valid`: an exception that has reached
  the head is architecturally final." Implementation: `head_trap = rob_valid[0] && rob_exc[0]`.
  It is deliberately **not** gated on completeness: a macro whose one child faults may never complete
  its siblings, so requiring `head_complete` would deadlock the head. The testbench had assumed the trap
  waits for a completion/payload cycle.
* **The trap takes priority over a recovery flush** — the header's cycle-semantics section says so
  explicitly, so a flush cycle may emit lane 0's trap event and nothing else.
* **The events follow the acknowledgement, not the request.** The buffer never acknowledges in a cycle
  it is flushing (`retire_ack = retire_req && head_ready && !flush_valid`, and the wrapper ORs
  `flush_valid_i`/`rob_flush_i`/`trap_flush` into the ROB's `flush_valid`), so a purely-`rob_flush` cycle
  emits no event even though the retire unit's *request* is still asserted — the unit cannot see
  `rob_flush`.
* **A `flush_valid` cycle retires nothing** — stated about the retire unit's own port, not about
  `rob_flush`.
* **The CSR read is the pre-edge state**: "a write that retires in cycle N is first read in cycle N+1".
* **Commit**: "One commit per retired instruction that wrote an architectural register … a write to x0
  retires and commits nothing".

Every failure below is one of these rules being asserted wrongly by the harness, or a comparison reading
the wrong cycle.

---

## 2. Exact commands

```
# the two required cases (each rebuilds from source, then runs)
python3 tools/run_unit.py --profile p0 --case retire.head_block_and_dual
python3 tools/run_unit.py --profile p0 --case commit.head_block_and_dual

# must stay green
python3 tools/run_unit.py --profile p0 --case rename.single_width_ownership
python3 tools/run_unit.py --profile p0 --case rob.out_of_order_children

# C++ for the file this package changed (project-wide lint-cpp stops earlier on an
# unrelated missing build dir, tb_fifo.cpp, so the file is linted on its own)
make lint-cpp CPP_SRCS="sim/unit/tb_retire.cpp"
```

Mutant recipe (per mutant; base first with no `-D`):

```python
import sys; sys.path.insert(0, 'tools'); import run_unit
run_unit.VERILATOR_FLAGS.append('-DMOSAIC_RETIRE_MUTANT_<n>')
run_unit.build_case('p0', 'retire.head_block_and_dual',
                    run_unit.load_registry()['cases']['retire.head_block_and_dual'])
# then:
# build/p0/unit/retire.head_block_and_dual/retire.head_block_and_dual \
#   --case retire.head_block_and_dual --out /tmp/mut --seed 1 --max-cycles 200000
```

## 3. Results

```
PASS retire.head_block_and_dual   task=I-017
PASS commit.head_block_and_dual   task=I-017
PASS rename.single_width_ownership task=I-013
PASS rob.out_of_order_children    task=I-016
lint-cpp: 1 file(s) clean
```

Base detail (`retire.head_block_and_dual`, seed 1):

```
retire contract holds: 469499 shadow comparisons over 0 cycles; 117 events
(26 in two-wide cycles, 91 alone), 17 traps, 60 committed-map updates,
2 x0 retirements, 14 CSR writes, 12 store authorisations, 399 flushes,
2271 blocked cycles, seed 1
```

`checks: 1` in `result.json` is the reporter's phase-level verdict count; the meaningful check count is
the 469 499 per-cycle shadow comparisons above. `commit.head_block_and_dual` produces the identical
detail (same top, same sources, same seed).

First mismatch on a green build: none.

---

## 4. Defects fixed — all testbench

1. **Trap timing.** `PhaseHeadBlock` (c), `PhaseMinstret` and `PhaseTrap` allocated an entry with
   `exc=true` and then expected the trap only after a `Complete` + payload cycle. Per the contract the
   trap fires the instant the exceptional entry *is* the head, i.e. the cycle after allocation, and it
   flushed the buffer before the later requires. Rewritten to raise the fault via the entry's own child
   completion (`Complete(id, 0, /*exc=*/true)`), so the trap cycle is the controlled payload cycle and
   the older head is complete-and-closed apart from the fault — which is exactly the card's "a complete
   younger entry must not retire over a pending fault".
2. **Two-snapshot reads.** `PcOfLane`, `SeqOfLane`, `TrapCause`, `TrapTval`, `CsrRead`,
   `CsrReadUnsupported` read live DUT ports *after* the clock edge, so they described the next cycle:
   `ev_pc` read 0 for both lanes (the pair had already left the buffer), `trap_cause` read 0 (the trap
   had flushed its entry), and the CSR read would have shown the write that had just retired. All are
   now captured with the clock low in `CaptureReports()` and read from that snapshot.
3. **Shadow acknowledgement model.** The shadow treated `retire_ack == retire_req`, so it predicted a
   retirement on a `rob_flush` cycle that the buffer had already erased. It now gates the ack on the
   buffer's flush (`rob_flush || flush_valid || trap_flush`), matching "the events follow the
   acknowledgement".
4. **Flush invariant too strong.** `retire_req == 0` was required on `rob_flush` too, but the retire
   unit's only flush input is `flush_valid` — on a buffer-only flush it still requests and the buffer
   refuses. Scoped the request requirement to `flush_valid`; both flush forms still require no ordinary
   retire/commit, and now allow only lane 0's trap (the documented exception).
5. **Completion cross-check false positives.** It treated a live shadow entry + `!accepted` as a defect,
   which fires on a trap/buffer-flush cycle (the buffer refuses every completion) and on a `bad_uop`
   completion. Rewritten to fail only when the shadow would *accept* the child and the buffer is not
   flushing.
6. **Lane-mask and event comparisons.** `o_pay_missing`, `o_csr_unsupported` and `o_exc_queued` are
   packed one 32-bit word per lane, but were compared as scalars — i.e. lane 0 only. `ev_valid` and
   `ev_trap` were **never compared per cycle**, and `retire_req` was never compared at all. All are now
   read lane by lane / compared every cycle.
7. **"Idle" cycle retired.** `PhaseMinstret`'s "nothing retires" cycle presented lane 0's payload, which
   retires the head (a retirable entry with a visible payload retires whether or not it writes a
   register). It now offers no payload.
8. **Soak addressed by position, not slot.** Completions and closes used the queue *position* as
   `cmp_index`, while `GenOf` looks up by *slot*; after the first retire these disagree, so almost every
   completion was stale and the soak retired 7 instructions. Fixed to the entry's own `slot`/`gen`.
9. **Committed-tag expectations.** The phase compared the 7-bit committed tag against the raw 12-bit
   allocation tag. Added `CmtTagOf()` (mask to the DUT's tag width) and used it.
10. **Shadow generation mask-as-width (introduced by I-016's GEN_W 12→7).** `Mask(value, width)` takes a
    *bit width*; the shadow passed `gen_mask_` (0x7F ≥ 32), so `Mask` returned the value unmasked and the
    shadow's generation never wrapped (`shadow 128, buffer 0`). All `Mask(..., gen_mask_)` call sites now
    use `gen_w_`, and `ReconcileSlots` uses a new `gen_width()`.
11. **Cross-lane interfaces** (not defects in this package, but the case must build against them):
    `mosaic_rename` gained the frozen I-014 ports (`alloc2_*`, `rs3_*`/`rs4_*`, `rs1_ready`/`rs2_ready`,
    `dbg_j_len`) — tied off in the wrapper; `mosaic_rob` moved to the generated contract identity widths
    (`TAG_W = MOSAIC_ID_W_PRF_TAG = 7`, `GEN_W = MOSAIC_ID_W_ROB_GEN = 7`) — the wrapper now reads
    `u_rob.TAG_W`/`u_rob.GEN_W` instead of assuming `2 * ROB_INDEX_W`, and the driver's geometry checks
    were rewritten for the new relationship (`rob_gen_w > rob_index_w`, `tag_w <= rob_tag_w`).

## 5. Checks added for the card's Pass criterion

* **Two same-rd commits in one cycle** (`PhaseCommittedOnce` step 2): both commit lanes set, the map ends
  on the *younger* tag, exactly as the card requires.
* **No younger instruction retires over a pending fault** (head-block (c), rebuilt): the younger entry is
  complete and the older is complete-and-closed except for its exception; the trap emits lane 0 only and
  the younger does not retire.
* **Events match a reference every cycle**: `ev_valid` and `ev_trap` are now compared per lane on every
  cycle (every other event field is gated on the event, so an eventless phantom would otherwise pass).
* **The request is a checked output**: `retire_req` is compared per lane every cycle.
* **x0**: a directed sub-case retires a write to x0 and requires `commit_valid == 0`, no map change, and
  `o_x0_retired`; the soak also draws `rd` from 0..31 so the summary's x0 count is non-zero.

## 6. Mutants

Base is green (`failures = 0`, exit 0), so the delta is the mutant's failure count. The driver stops at
the first failure, so `fail` is 1 whenever the mutant is detected; the *first distinct failure* is the
evidence.

| `-D` | exit | failures | delta vs base | first distinct failure |
|---|---|---|---|---|
| *(base, none)* | 0 | 0 | – | – |
| `MOSAIC_RETIRE_MUTANT_RETIRE_OVER_BLOCKED_HEAD` | 1 | 1 | **+1** | `head-block`: `retire_req: expected 0, got 3` |
| `MOSAIC_RETIRE_MUTANT_RETIRE_IN_FLUSH` | 1 | 1 | **+1** | `flush`: `retire_req: expected 0, got 3` |
| `MOSAIC_RETIRE_MUTANT_SECOND_LANE_UNORDERED` | 1 | 1 | **+1** | `head-block`: `retire_req: expected 0, got 2` |
| `MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL` | 1 | 1 | **+1** | `head-block`: `ev_reg_we: expected 0, got 3` |
| `MOSAIC_RETIRE_MUTANT_COMMIT_ON_SQUASH` | 1 | 1 | **+1** | `head-block`: `commit_valid: expected 0, got 3` |
| `MOSAIC_RETIRE_MUTANT_CSR_AT_EXECUTE` | 1 | 1 | **+1** | `csr-at-retire`: `mscratch changed to 0x…12345678 before its instruction retired` |
| `MOSAIC_RETIRE_MUTANT_INSTRET_COUNTS_EXEC` | 1 | 1 | **+1** | `in-order`: `minstret: expected 0, got 1` |

**Outcome change caused by this package.** Two of these mutants —
`RETIRE_OVER_BLOCKED_HEAD` and `SECOND_LANE_UNORDERED` — originally exited **0 with delta 0**: both only
weaken the retire unit's *request*, and the real ROB acknowledges from its own `head_ready`, so no wrong
event was ever emitted and the defect was invisible. Adding the per-cycle `retire_req` comparison (which
the case previously omitted entirely) makes both visible; they now exit 1 at delta +1. The other five
were already detected and are unchanged apart from cycle numbers.

## 7. What is NOT verified

* **No external reference model.** The oracle is this file's own C++ shadow plus the real ROB/rename;
  there is no NEMU/Spike/Sail comparison here. "Events matching a reference" means matching that shadow.
* **RTL lint not run by this package.** No RTL file was changed; `tools/lint_rtl.py` and `slang-tidy` are
  the integration lead's project-wide pass. The case's own Verilator elaboration is warning-clean.
* **`make lint-cpp` (whole project) does not complete** — it stops on `sim/unit/tb_fifo.cpp`
  (`Vmosaic_fifo_tb.h` not found, i.e. that case has no build dir). `sim/unit/tb_retire.cpp` is clean on
  its own with the same flags.
* **The soak is 4000 random steps** (117 events): it does not exhaust the state space; the directed
  phases carry the specific rules.
* **The alias registry entry** (`commit.head_block_and_dual`) lists only `rtl/core/mosaic_retire.sv`; it
  builds because Verilator resolves `mosaic_rob`/`mosaic_rename`/`mosaic_pkg` from `-Irtl/core`. That is
  either an intentional minimal entry or an incompletely-specified one — reported, not edited (the
  registry is off-limits to this package). Both entries were rebuilt from scratch after this change and
  produce identical results.
* **Interfaces are in flux.** `mosaic_rob` and `mosaic_rename` were both edited by their owners during
  this package; the wrapper now derives the ROB identity widths from the elaborated module, but a further
  change to their port lists would require another tie-off.
* Failure counts are *first-failure* counts (the driver throws on the first mismatch), so `failures == 1`
  says "detected", not "how many checks would fail".

## 8. Files changed

* `sim/unit/tb_retire.cpp` — the fixes and added checks above.
* `sim/tb/mosaic_retire_tb.sv` — rename I-014 port tie-offs; ROB identity widths from
  `u_rob.TAG_W`/`u_rob.GEN_W`; two geometry outputs (`o_rob_tag_w_o`, `o_rob_gen_w_o`).
* `results/reports/I-017-retire.md` — this report.
* No change to `rtl/core/mosaic_retire.sv`, `mosaic_rob.sv`, `mosaic_rename.sv`,
  `tests/unit/registry.json`, `config/status/implementation_status.json` or `results/PROGRESS.md`.
