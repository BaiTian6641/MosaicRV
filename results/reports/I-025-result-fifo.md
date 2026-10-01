# I-025 — per-cluster result FIFO

Work package **I-025** of `docs/stage-2-execution-fabric.md`, implementing the
`CASE=completion.fu_collision` obligation in `docs/implementation-plan.md` §I-025
(“实现 per-cluster result FIFO”: 多 FU 同周期完成时按实际端口 buffer/arbitrate；
满队列不丢值，异常不被普通结果覆盖).

## STATUS: COMPLETE

| Acceptance item | Evidence |
| --- | --- |
| 1. Verilator `-Wall` clean | `python3 tools/lint_rtl.py --profile p0` → `ok rtl/core/mosaic_result_fifo.sv: clean as mosaic_result_fifo`; the run reports `1 of 25 source file(s) failed`, and the one failure is **another lane's in-flight `rtl/core/mosaic_remote_link.sv`**, not this package — see *Lint* |
| 2. `slang-tidy --std 1800-2017 --single-unit` clean | same: this file contributes **0 errors**; the only errors in the project-wide run are in `mosaic_remote_link.sv` |
| 3. `python3 tools/run_unit.py --case completion.fu_collision` | `RESULT PASS completion.fu_collision result fifo contract holds: 30057 per-cycle whole-state comparisons over 30121 cycles, 2 entries / 3 producers / 209-bit payload, seed 1`, exit 0 |
| 4. Discriminating coverage | seven phases, each resetting first, with the coverage counters printed and asserted — see *Coverage* |
| 5. Mutants fail with exit 1, with a delta against a green base | six, each `+1` failure against a base of 0, each with a distinct first mismatch naming its own defect — see *Mutants* |
| 6. The card’s Fail criterion tested explicitly | “multiply/load never return together” is the collision phase (all three producers at once); “valid pulse that cannot be held” is the valid-pulse phase — see *The card’s Fail criterion* |
| 7. `make lint-cpp` for the driver | this file contributes no diagnostic; the target is red on another lane’s `sim/unit/tb_remote.cpp` — see *Lint* |
| 8. This report | — |

## Files

| File | Contents |
| --- | --- |
| `rtl/core/mosaic_result_fifo.sv` | `mosaic_result_fifo`: one cluster’s completion buffer |
| `sim/tb/mosaic_result_fifo_tb.sv` | `mosaic_result_fifo_tb`: straight pass-through wiring plus geometry read-back |
| `sim/unit/tb_result_fifo.cpp` | Independent C++ shadow, per-cycle whole-state comparison, seven phases, coverage |
| `results/reports/I-025-result-fifo.md` | This file |

`tests/unit/registry.json` was **not** edited: the case was already registered
(`top: mosaic_result_fifo_tb`, `rtl: [rtl/core/mosaic_result_fifo.sv]`,
`sv: [sim/tb/mosaic_result_fifo_tb.sv]`, `cpp: [sim/unit/tb_result_fifo.cpp]`,
`max_cycles 200000`, `seed 1`) and is used unchanged.

## Interface

`mosaic_result_fifo` takes **no** parameters. Every size is a `localparam` read
from a generated package, so a caller cannot override it into a second geometry:

| Field | Source | p0 |
| --- | --- | --- |
| `ENTRIES` | `mosaic_cfg_pkg::MOSAIC_RESULT_FIFO` | 2 |
| `XLEN` | `mosaic_cfg_pkg::MOSAIC_XLEN` | 64 |
| `ROB_INDEX_W` | `mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX` | 6 |
| `ROB_GEN_W` | `mosaic_id_pkg::MOSAIC_ID_W_ROB_GEN` | 7 |
| `UOP_W` | `mosaic_id_pkg::MOSAIC_ID_W_UOP_INDEX` | 3 |
| `N_ALU` / `N_MULDIV` / `N_LSU` | `MOSAIC_ALU_PER_CLUSTER` / `MOSAIC_MULDIV_UNITS` / `MOSAIC_LSU_UNITS` | 1 / 1 / 1 |
| `PRODUCERS` | the sum above | 3 |
| `CNT_W` | `$clog2(ENTRIES+1)` | 2 |
| `ENTRY_W` | `ROB_INDEX_W + ROB_GEN_W + UOP_W + 3*XLEN + 1` | 209 |

`MOSAIC_CLUSTERS` is deliberately **not** referenced: this module is one
cluster’s buffer and knows nothing about how many exist.

Payload layout, most significant field first, mirroring `mosaic_iq`’s
`{rob_index, rob_gen, uop_index}` word:

```
| rob_index | rob_gen | uop_index | data | exc_valid | exc_cause | exc_tval |
```

Ports: `p_valid`/`p_ready`/`p_pay` (one payload word per producer, producer 0 in
the least significant slice), `c_valid`/`c_ready`/`c_pay` (the consumer, position
0), `kill_valid`/`kill_all`/`kill_rob_index`/`kill_rob_gen`, and the `o_*`
status/observation set: `o_count`, `o_occ`, `o_exc_occ`, `o_push_ctr`,
`o_pop_ctr`, `o_kill_ctr`, `o_kill_offer_ctr`, `o_kill_total`, `o_entry_pay`.

## Contract, in prose

**Producer handshake.** A transfer is exactly `p_valid[p] && p_ready[p]`. A
producer must hold `p_valid` and its payload until `p_ready` is high; the FIFO
refuses the results it cannot take this cycle rather than sampling them, so a
refusal loses nothing and costs at most one cycle. `p_ready` is a function of the
registered occupancy and the kill inputs — **never** of `c_ready` — so there is no
combinational path from the writeback arbiter back into the execution units. The
consequence is deliberate and is tested directly: a same-cycle pop (or a
same-cycle kill) does **not** free a slot for a same-cycle push.

**Arbitration.** Fixed priority by producer index: producer 0 first, then 1, …,
up to `ENTRIES - o_count`. Deterministic rather than rotating, because the
queue’s contents must be a defined function of the input history and the pipeline
needs no fairness beyond “a refused producer is refused only while the queue is
full”.

**Exception payload.** Each entry owns its `exc_valid`, `exc_cause` and
`exc_tval`; a push writes all three (or clears all three) atomically. Nothing in
the module can overwrite an exception with a later ordinary result — the failure
the package exists to prevent — and the exception flag is also exported as
`o_exc_occ` so the property is visible, not argued.

**Kill.** The predicate is exactly

```
kill hits an entry  <=>  kill_valid && (kill_all
                         || (entry.rob_index == kill_rob_index
                             && entry.rob_gen == kill_rob_gen))
```

`uop_index` is deliberately not compared: a kill names a *macro*, and every child
goes — the same rule and the same reason as `mosaic_iq`’s `UopIsMacro`. In the
cycle a kill is asserted:

* every matching **buffered** entry is dropped before it can be presented; a
  killed head is masked combinationally on `c_valid`, so a killed entry is never
  delivered even if the consumer is ready that cycle, and a pop and a kill can
  never both apply to one entry. Each buffered drop increments `o_kill_ctr`.
* a producer offering a **matching** result is **absorbed and dropped**:
  `p_ready` is asserted, the result is not stored, and `o_kill_offer_ctr`
  increments. It is not refused, because refusing it is a livelock — the producer
  would hold a result for an instruction that no longer exists, no event would
  ever release it, and it has no other way to learn the uop is dead. `mosaic_iq`
  documents the same exception for its grant port.

**Ordering.** The queue is an ordered compacted sequence, position 0 oldest. On
every edge it is rebuilt as *survivors (oldest first, minus pops and kills) ++
granted pushes (ascending producer index)*. There is no pointer pair to alias, so
“pop frees the head slot while a push fills the tail slot” is unrepresentable.
The cost is O(ENTRIES) write ports per position — the right trade at
`MOSAIC_RESULT_FIFO = 2`, and stated in the RTL header rather than discovered
later: a deep result FIFO would use a circular buffer with the same interface.

**Reset.** Synchronous, active high; clears `n` and the counters, and **nothing
else**. Entry storage is data and validity lives entirely in `n`. Positions at or
above `o_count` are zero on the observation port, so the whole state vector is
comparable every cycle without reading unreset storage.

**Conservation.** `o_push_ctr == o_pop_ctr + o_kill_ctr + o_count` is checked
every cycle of every phase from the DUT’s own ports. An absorbed in-flight offer
is not a push (it was never stored) and appears only in `o_kill_offer_ctr`, which
is why it does not disturb the identity. A pop that was never pushed, a duplicate
pop and a lost push each break the identity by exactly their own count.

## Coverage

`RESULT PASS`, exit 0, `checks 8`, `failures 0`, **30057 per-cycle whole-state
comparisons over 30121 cycles** (each comparison covers `c_valid`, `c_pay`,
`p_ready[0..2]`, `o_count`, `o_occ`, `o_exc_occ`, every position of
`o_entry_pay`, all five counters, and the conservation identity). The phase
messages, printed verbatim by `--verbose`:

```
reset-state: empty, no head, every producer refused, counters zero
collision: one slot, 3 simultaneous completions, one accepted, losers held and
           delivered unchanged in priority order, exception intact
fill-drain: filled, 12 refused pushes with nothing lost, 3 stalled cycles with a
           stable head, drained in order with exceptions interleaved
kill: matching buffered drop counted, matching in-flight offer absorbed and
      counted, unrelated result refused then taken, near-miss kill inert, no
      killed result delivered
pop-push-same-cycle: a full queue's pop did not admit a same-cycle push, and the
      refused result was taken the next cycle
valid-pulse: a result held across 5 refused cycles was neither sampled nor lost,
      and entered unchanged once there was room
random: 18392 pushes, 15881 pops, 2513 buffered kills, 2862 absorbed offers,
      42565 refused offers, 13816 contended cycles, 4362/3273 exceptional
      pushes/pops over 30000 cycles
```

The random phase’s anti-vacuity thresholds are asserted, not decorative: pushes,
pops, buffered kills, absorbed offers, refused offers (> cycles/8), contended
cycles (> cycles/20), exceptional pushes and exceptional pops must all be
non-zero, so a campaign that did nothing cannot pass.

The campaign is not tuned to seed 1. It was run at seeds `1, 2, 3, 7, 11, 555,
1234, 99991`; all eight PASS with 30056–30057 comparisons.

## Mutants

Each mutant was rebuilt from the frozen sources with one extra
`-D…` flag, run against the unmodified base (base: exit 0, `checks 8`,
`failures 0`), and required to exit 1. The delta is the column that carries the
evidence; the first mismatch names the defect.

| mutant | exit | checks | failures | delta | first mismatch |
| --- | --- | --- | --- | --- | --- |
| `DROP_ON_COLLISION` | 1 | 2 | 1 | **+1** | `collision: cycle 18: p_ready[1]: expected 0, got 1` — the loser is told its result was taken |
| `VALID_PULSE` | 1 | 2 | 1 | **+1** | `collision: cycle 18: p_ready[0]: expected 1, got 0` — a held `valid` is not re-offered |
| `EXC_DROPPED` | 1 | 2 | 1 | **+1** | `collision: cycle 24: o_exc_occ differs from the shadow at position 1` |
| `OCC_OFF_BY_ONE` | 1 | 2 | 1 | **+1** | `collision: cycle 17: o_count: expected 1, got 0` |
| `KILL_DELIVERS` | 1 | 4 | 1 | **+1** | `kill: cycle 64: c_valid: expected 0, got 1` — a killed head is presented |
| `KILL_NOT_COUNTED` | 1 | 4 | 1 | **+1** | `kill: cycle 64: o_kill_ctr: expected 1, got 0` |

Bodies were checked to exist, not assumed: the sweep asserts
`` `ifdef <NAME> `` is present in the RTL before building, and **it caught a real
instance of this project’s known trap** — `KILL_DELIVERS` was listed in the RTL
header and in the plan for this report but had never been written into the source.
The sweep aborted with `AssertionError: no ifdef body for
MOSAIC_RESULT_FIFO_MUTANT_KILL_DELIVERS`; the control was then written and the
whole sweep re-run. Had it not been asserted, that mutant would have compiled the
shipping build, passed, and proved nothing.

Exact reproduction (the flags list is appended to, then the binary is run
directly):

```python
import sys; sys.path.insert(0, 'tools'); import run_unit
run_unit.VERILATOR_FLAGS.append('-DMOSAIC_RESULT_FIFO_MUTANT_KILL_DELIVERS')
run_unit.build_case('p0', 'completion.fu_collision',
                    run_unit.load_registry()['cases']['completion.fu_collision'])
```
```sh
build/p0/unit/completion.fu_collision/completion.fu_collision \
    --case completion.fu_collision --out /tmp/mut --seed 1 --max-cycles 200000; echo $?
```

## The card’s Fail criterion

* “assume multiply/load never return at the same time” — the interface has one
  port per possible completing source (`PRODUCERS = ALU + MULDIV + LSU = 3`), so
  the assumption is representable and therefore falsifiable. The **collision**
  phase drives all three in one cycle into one free slot: exactly one acceptance
  (producer 0, the documented priority), the losers still offered with
  byte-identical payloads, and the exception-bearing result delivered intact
  later. `DROP_ON_COLLISION` is the mutant for the opposite behaviour.
* “a `valid` pulse that cannot be held” — the **valid-pulse** phase holds a result
  across five refused cycles with the queue full and the consumer stalled: it is
  never sampled without a transfer, never appears in the queue, never advances
  `o_push_ctr`, and is accepted byte-identically once there is room.
  `VALID_PULSE` is the mutant for the opposite behaviour.

## Lint

* Verilator: `python3 tools/lint_rtl.py --profile p0` → `ok
  rtl/core/mosaic_result_fifo.sv: clean as mosaic_result_fifo`. The run summary
  is `lint: 1 of 25 source file(s) failed`; the failure is
  `rtl/core/mosaic_result_fifo.sv`’s **sibling** `rtl/core/mosaic_remote_link.sv`
  (`Can't find definition of variable: 'alloc_ptr'` at line 796), which appeared
  under `rtl/core/` while this package was being written and belongs to another
  lane. Not edited, per the file-ownership rule; reported here instead.
* slang: `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl
  -name '*.sv' | sort)` → this file contributes **0 errors**; the only two errors
  in the project-wide run are `use of undeclared identifier 'alloc_ptr'` in the
  same `mosaic_remote_link.sv`.
* C++: `make lint-cpp` fails on another lane’s `sim/unit/tb_remote.cpp`
  (`fatal error: 'Vmosaic_remote_link_tb.h' file not found` — a case that has not
  been built yet). The same command compiles `sim/unit/tb_result_fifo.cpp` with
  **no diagnostic of its own**; the only warnings from that translation unit come
  from Verilator’s own headers, which every file in the project shares.
* Synthesis: `python3 tools/synth_check.py --profile p0` is **BLOCKED before it
  reaches this file** — Yosys 0.69 cannot parse the assignment patterns in
  `mosaic_decoder.sv`, and it also cannot parse the generated
  `mosaic_id_pkg.svh` (`return (a) && (b)` bodies). As a scoped substitute, the
  module was elaborated by Yosys directly with the real config package and a
  scratch copy of the identity package reduced to the three widths this module
  reads (the generated one does not elaborate in Yosys at all): `hierarchy
  -check`, `proc`, `check -assert`, `memory -nomap`, `opt -full`, `select
  -assert-none t:$dlatch` all pass, so the module elaborates with no undriven or
  multiply-driven wire and **no latch**. It is not a vendor synthesis result and
  no timing, area or Fmax claim is made.

## What this package decided, and what it rejected

* **One port per unit, not a folded result port.** The card’s Fail criterion is
  only testable if a collision is representable; folding the sources onto fewer
  ports would make the failure mode untestable rather than handled.
* **The losing result is held by its producer, not buffered by the FIFO.** The
  FIFO samples nothing on a refusal, so “loses nothing” is a property of the
  handshake plus the producer’s obligation to hold, not of extra storage.
* **A killed in-flight offer is absorbed, not refused.** Refusal is a livelock
  for an instruction that no longer exists; the handshake completes and the
  result is accounted as killed. This is a documented deviation from “hold until
  accepted”, copied from `mosaic_iq`’s grant port.
* **`p_ready` never consults `c_ready`, and a kill frees space at the edge only.**
  Both keep the writeback arbiter off the execution units’ combinational path;
  the price is one cycle of latency on a same-cycle pop or kill, paid by the
  producer holding its result. Both consequences are tested directly.
* **An ordered compacted queue, not a circular pointer pair.** At two entries the
  compaction is trivial muxing and the aliasing class of bug is unrepresentable;
  the header states the depth at which that trade reverses.
* **`uop_index` is outside the kill predicate.** A kill names a macro, matching
  `mosaic_iq`; the field still travels with the result so the consumer can act on
  it.

## What is NOT verified

1. **No consumer exists.** I-026’s writeback arbiter is not written, so the
   module is verified standalone through its ports only: result → PRF write,
   value-visible wakeup and bank arbitration are not covered here.
2. **One cluster.** `MOSAIC_CLUSTERS` is not referenced and only one instance is
   elaborated. The routing that brings the shared MUL/DIV’s and the LSU’s
   completions to the owning cluster is I-028’s contract and is not tested.
3. **Only the p0 geometry.** `ENTRIES = 2`, `PRODUCERS = 3`, `ENTRY_W = 209` are
   the only values ever elaborated; the guards reject a vacuous geometry at
   elaboration, but no second profile was built or run.
4. **No timing, area or Fmax claim.** The compaction is O(ENTRIES) muxing per
   position; whether that meets a clock target at a deeper profile is untested.
5. **Generation checking is the ROB’s, not this module’s.** The queue preserves
   `rob_gen` and delivers it, but it has no reference to compare against, so a
   stale-generation result that arrives *after* a kill is rejected by I-016/I-018,
   not here. This is a boundary, not an omission, but it is a boundary.
6. **Exception cause/tval encodings are not checked against the RISC-V
   privileged spec** — only transport and integrity. `mosaic_pkg::EXC_*` is where
   the encoding lives.
7. **`exc_pushes`/`exc_pops` coverage counters are derived from the stimulus and
   the observed `p_ready`**, backed by the per-cycle equality of `p_ready` and of
   the stored payloads, because the DUT exports no exception counter. They are
   coverage, not a contract check.
8. **The `o_*` observation port is a verification port.** No functional consumer
   reads it, and no protocol is claimed for it beyond “a total function of the
   state, zero-padded above `o_count`”.
9. **The whole project suite was not run.** `make test` is the integration
   owner’s gate and is red for reasons outside this package (see *Lint*).

## One honest note on the test itself

The `pop-push-same-cycle` phase failed the first time it was run — at **every**
seed. The DUT was right and the test was wrong: the phase recorded the delivery
from the pop cycle but forgot to record the delivery that the *next* cycle makes
while the consumer is still ready, so its expected-delivery sequence was missing
an element. The fix was to record that delivery, not to weaken the comparison or
drop the phase. No DUT-versus-shadow disagreement was found in this package; the
shadow was wrong zero times and the hardware was wrong zero times.
