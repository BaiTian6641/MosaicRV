# I-057 — the stop path: the disposition of an in-flight load

Work package **I-057** (the partial-trap / restart controller and the
packetizer's stop handshake), case **`rvv.stop_path_inflight`** (top
`mosaic_vec_tb`, driver `sim/unit/tb_vec.cpp`, still marked `"pending": true`
in `tests/unit/registry.json`; the integration lead clears the marker).

The defect this package owns was recorded by the I-061 coalescing lane rather
than hidden: *an uncoalesced load whose response is still in flight when a stop
is taken is discarded rather than written back*, because the write-back push was
gated by the abort signal (`wf_push_c` gated by `!abort_q`).

| | |
|---|---|
| Verdict | **The finding was real for one stop kind and is fixed**; the other kinds are correct as they stood, and the case proves each by construction |
| Case | `RESULT PASS rvv.stop_path_inflight checks=63 kinds=4 cells=7 resumes=5 inflight=3 cycles=626` |
| Seeds | 2, 7, 12345 — identical 63 checks (the case is deterministic) |
| Profiles | p0 and p2 PASS |
| Controls | `python3 tools/run_stop_path_controls.py --profile p0` → 3/3 mutants caught |
| RTL | `rtl/core/mosaic_vec_lsu.sv` — `discard_q` splits "stop issuing" from "discard the in-flight responses"; the boundary stop no longer discards |
| Protected cases | `rvv.partial_fault_restart`, `rvv.memory_modes`, `coalesce.element_faults`, `rvv.integer_mask_permute`, `rvv.mask_prefix_semantics`, `rvv.mask_prefix_vstart`, `rvv.fp_flags_reduction`, `rvv.chaining_hazards`, `vec.integrated`, `rvv.lane_resize_boundary`, `core.act_dut` — all PASS |
| ACT4 | `applicable=127 generated=127 run=127 passed=127 failed=0` (p1) |
| Gates | `make check` green; `lint_rtl` p0 and p2 green (61 files); `slang-tidy` exit 0; `check_records` green; `check_exclusions` + `--negative` 13/13; `make lint-cpp` 73 files clean |

---

## 1. The rule, established before anything changed

The V spec's normative text (the pinned revision's rendered lines are quoted in
`results/reports/I-057-vector-restart.md`; the same sentences were read from the
upstream `src/unpriv/vector-common.adoc` while writing this report):

* **On any trap — synchronous or interrupt — `vstart` is the element index on
  which the trap was taken.** "On a trap during a vector instruction (caused by
  either a synchronous exception or an asynchronous interrupt), the existing
  `epc` CSR is written with a pointer to the trapping vector instruction, while
  the `vstart` CSR contains the element index on which the trap was taken."
* **Precise traps require the prefix committed.** "Precise vector traps require
  that: … (3) any operations within the trapping vector instruction affecting
  result elements preceding the index in the `vstart` CSR have committed their
  results; (4) no operations within the trapping vector instruction affecting
  elements at or following the `vstart` CSR have altered architectural state
  except if restarting and completing the affected vector instruction will
  nevertheless produce the correct final state."
* **The clause-(4) exception is a permission, not a licence.** "We relax the
  last requirement to allow elements following `vstart` to have been updated at
  the time the trap is reported, provided that re-executing the instruction from
  the given `vstart` will correctly overwrite those elements."
* **Stores in non-idempotent memory are the sharp case.** "Non-idempotent memory
  regions must not have been updated for indices equal to or greater than the
  element that caused a synchronous trap during a vector store instruction."
* **Fault-only-first.** "If element 0 raises an exception, `vl` is not modified,
  and the trap is taken. If an element > 0 raises an exception, the corresponding
  trap is not taken, and the vector length `vl` is reduced to the index of the
  element that would have raised an exception." And for an interrupt: "implementations
  should not reduce `vl` and should instead set a `vstart` value."

I-057's own contract (the header of `mosaic_vec_restart.sv`, R1) states the
implementation's rule more strongly than the spec's permission:

> **R1 (partial trap)** A synchronous fault at element k commits the elements
> strictly before k … and leaves element k and every later element unperformed.

So the rule this package implements, stated once:

> **When a macro stops, an in-flight response for an element that has already
> committed must be written back; an in-flight response for an element that will
> be re-executed must be discarded.** The spec *requires* the first (clause 3)
> and only *permits* the second for an idempotent load (the clause-4 exception);
> this unit does not take the permission — it implements R1's stronger statement
> — because the hardware cannot guarantee the restart that the exception is
> conditioned on (the trap handler may abandon the instruction, or resume with a
> different `vstart`, or the element may be a masked/tail element the restart
> does not re-issue).

### The stop-kind table

| stop kind | RTL path | committed? | re-executed? | in-flight disposition | basis |
|---|---|---|---|---|---|
| **element fault at k** (vstart = k) | `mem_rsp_fault_i` → `abort_q`+`discard_q`, `trap_elem` = k | `[0,k)` | `[k,vl)` | **`[0,k)` written back; `>= k` discarded** | precise-trap clauses 3–4; R1 |
| **whole-macro trap** (nothing committed) | fault at element 0; the coalesced-group fallback that reports the group's first element | none | everything | **everything discarded** | clause 4 exception is vacuous (all re-executed); R1 with k = 0 |
| **redirect / cancel at b** (a precise interrupt's boundary stop) | `stop_i` → `stopped_o`/`stop_elem_o` = b, `RC_INTR` trap with vstart = b | `[0,b)` | `[b,vl)` (nothing issued at/after b) | **every in-flight response is `< b` and written back** | clause 3 (an interrupt writes `vstart`; the prefix must have committed); R1 |
| **lane broker drain (I-059)** | no packetizer input at all: the broker gates new admission and waits for the macro to become idle | everything | nothing | **written back — the macro completes normally** | the drain is not a stop; I-059 §1 "the old quota's lane state is acknowledged … no element that a lane still owed is discarded" |
| *(a true pipeline squash)* | **no such input exists on this unit** | none | everything | would be **discard all** (the abort path as it was) | not claimed — see §6 |

### The one invariant that makes the in-order response stream sufficient

The unit offers items in ascending logical order, so a response for element j
proves every response for an element < j has already been processed. A stop that
keeps a *prefix* therefore never sees a prefix response after the stop; only the
fault path can have a later-element response outstanding, and it discards it.

## 2. What the current behaviour actually was

The probe (I-061's `coalesce-stop` phase, uncoalesced row, memory kept **ready**
so the request pipeline stays full): the packetizer reported `stop_elem` = b and
the destination element b−1 — a load that had already been issued to memory and
whose response was still in flight — was **not** written back. `wf_push_c` was
`rsp_c && … && !abort_q && …`, and the boundary stop sets `abort_q`, so the
response was consumed (the outstanding count decremented) but its data was
dropped. The element was lost: a resume from b never re-executes b−1, so the
destination element stayed at its pre-instruction value for ever.

The fault path was **already correct**, and the reason is the in-order response
stream: when the fault response for k arrives, every response for an element < k
has already been written back, and the only responses that can follow belong to
elements > k, which are exactly the ones that must be discarded. The two kinds
were conflated by a single `abort_q` gate, and only the boundary stop was wrong.

## 3. What changed

`rtl/core/mosaic_vec_lsu.sv` — one new register splits the two meanings:

* `abort_q` (unchanged meaning) stops the unit offering new work;
* `discard_q` (new) additionally says the in-flight responses belong to elements
  that will be re-executed.

`rsp_push_c`/`wf_push_c` is now gated by `!discard_q` instead of `!abort_q`.
`discard_q` is set by exactly the two paths that *re-execute* the outstanding
elements — the element-fault path and the coalesced-group fallback trap — and is
cleared at reset and at each macro launch. The three boundary-stop paths
(`ST_SCAN`, `ST_REQ`, and the coalesced `RET_STOP`) set only `abort_q`, so their
in-flight responses are written back. The rule and the per-kind table are stated
in the module header.

No change was needed in `mosaic_vec_restart.sv`: the controller already latches
the boundary into `vstart`, walks the committed prefix into the descriptor, and
publishes the restart point as the bitmap's first clear bit; the defect was
purely the packetizer's disposition of the in-flight response.

`sim/unit/tb_vec.cpp` — the `Lsu` harness gains an in-flight observation
(`flight`, `flight_head`), an optional hold on a faulting response
(`fault_hold`, so a later element is deterministically in flight when the fault
is reported), and `ClearFaults` now *empties* the byte-granular fault map instead
of filling it with `false` (a non-empty all-false map silently disabled the
element-keyed path — `Faults` chooses the byte path by `!fault_bytes.empty()`).

`coalesce.element_faults`'s `coalesce-stop` phase: the pre-boundary prefix is now
asserted for the **uncoalesced** run too. The I-061 lane's probe is a check now,
and it fails on the re-injected defect (see §5).

## 4. The coverage table (stop kind × in-flight position)

The case drives, per stop kind, a load in flight at the moment of the stop, and
checks the destination element, the memory side effect and the re-execution
against an expectation computed in `tb_vec.cpp` from the rule above.

| stop kind | macro | in-flight element at the stop | observed disposition | resume | cell |
|---|---|---|---|---|---|
| element fault at k = 2 | unit load, vl = 6, sew = 8 | flight = 1, head = 3 (> k) | `[0,2)` loaded; 2,3 untouched | from 2, issues 2..5, prefix 6 | 1 |
| element fault at k = 3 | unit load, vl = 6 | flight = 1, head = 4 (> k) | `[0,3)` loaded; 3,4,5 untouched | from 3, issues 3..5, prefix 6 | 2 |
| whole-macro trap (fault at 0) | unit load, vl = 6 | flight = 1, head = 1 (> 0) | nothing committed; all six untouched | from 0, issues 0..5 | 3 |
| redirect/cancel at b = 5 | unit **load**, vl = 8, memory ready | flight = 1, head = 4 (< b) | `[0,5)` loaded **including 4**; 5,6,7 untouched | from 5, issues 5..7, prefix 8 | 4 |
| redirect/cancel at b = 4 | unit **store**, vl = 8 | none (stores are issued strictly, limit 1) | `[0,4)` written exactly once; 4..7 untouched | from 4, issues 4..7, every byte once | 5 |
| lane-broker drain | unit load, vl = 8, memory paced, latency 4 | max flight = 2 while the macro runs | no stop; every element written back | — | 6 |
| lane-broker drain | unit store, vl = 8, paced | max flight = 1 (limit 1) | no stop; every byte written exactly once | — | 7 |

The scenario is asserted, not assumed: the fault rows require `flight >= 1` with
`head > k`, the load boundary row requires `flight >= 1` with `head < b`, and the
drain rows require the pipeline to have been genuinely occupied. Three cells
observed a real in-flight response and five exercised a resume; those counts are
checks, so a stop kind that stopped being reachable fails rather than being
implied. The `offered == b` check states the boundary against the request stream
(exactly `[0,b)` offered), and the resume rows check the re-execution issues
exactly `[b,vl)`.

## 5. The mutant table

`python3 tools/run_stop_path_controls.py --profile p0`. The shipping build is
built first from an empty directory and required to pass; each mutant is built
from its own empty directory with its `-D` on the recorded command line.

| mutant | injects | sha256 | first failing check |
| --- | --- | --- | --- |
| shipping | — | `08e477524228de1bbc07cfc901d7d29e76acc0997f8ccc130f7a07780fbd70a6` | — (PASS, 63 checks) |
| `MOSAIC_VEC_LSU_MUTANT_STOP_DISCARD` | the write-back push is gated by the abort signal, so a boundary stop discards an in-flight load whose element has already committed (the reported defect) | `af9a5fae70f1cc277b7eb1c6d04c410dd06d0d56588f237955dfe443abcf19e0` | `redirect/cancel boundary stop load: the partial state after the stop is wrong …: dest4` |
| `MOSAIC_VEC_LSU_MUTANT_STOP_WRITEBACK` | the discard is dropped, so a response that arrives after a fault — for an element that will be re-executed — is written back with a stale value | `c4d099cce48b56c4a2b402db622ebb7161f418dbdc855864a6a401299917800b` | `element fault at 2: the partial destination is wrong …: dest3` |
| `MOSAIC_VEC_LSU_MUTANT_STOP_REISSUE` | the boundary stop names the element before the first one not performed, so the resume re-issues an element that already completed, duplicating an irreversible effect | `a7e32f09be6e47491f81bb685358f784c6ba62d21fb6959259ae17c952ecdc33` | `redirect/cancel boundary stop load: the stop reported boundary 4 but 5 elements had been offered -- the resume would re-issue an element that already completed and duplicate its effect` |

Each binary differs from the shipping one (`cmp`), each exits 1, and each names
the check its defect breaks.

**Cross-checks of the two directions against the sibling cases.**

* The re-injected defect (`STOP_DISCARD`) is now caught by the I-061 case as
  well: `coalesce.element_faults` built with that `-D` exits 1 on
  `coalesce-stop unit e8 load uncoalesced: load element 8 (stop_elem 9) is
  0x…b5 expected 0x…fe`. That is the probe becoming a check.
* The mirror defect (`STOP_WRITEBACK`) is also caught by
  `rvv.partial_fault_restart` (built with that `-D`, exit 1 on
  `fault-boundary unit-load at 0 code page: the partial state is wrong: dest1`,
  and on the `fault-boundary unit-load at 2 … dest3` and `fof later fault` rows)
  — the discard direction is part of I-057's contract, not an invention of this
  case.

The `STOP_REISSUE` mutant is caught one check before the store row's direct
`byte_writes == 1` assertion, by the boundary-against-request-stream check whose
message names the duplication; the direct assertion is exercised positively in
the shipping run and would also catch it.

## 6. Not covered — stated deliberately

* **There is no redirect/cancel input on this unit.** The packetizer has exactly
  two stop inputs: the fault response and `stop_i`. A true pipeline squash (kill
  the macro before it retires, re-fetch it) has no port, so the case cannot drive
  one; the abort-only path added here would write back, which is right for a
  partial stop and would be wrong for a squash. The header says so, and any
  future input must set `discard_q` as the fault path does. The closest
  reachable instance of "nothing committed, everything re-executed" is a fault
  at element 0, which is the whole-macro row.
* **The lane broker is not instantiated in `mosaic_vec_tb`.** The drain row is
  the property the broker's drain relies on — a paced macro with no stop
  completes with every element written back and no side effect lost — not the
  broker's own protocol, which is I-059's case (`rvv.lane_resize_boundary`).
* **Stores are issued strictly (limit 1), so no store response is ever in flight
  at a boundary stop.** The store rows therefore carry the resume /
  no-duplicate-side-effect half of the rule; the in-flight write-back half is the
  load row.
* **The fault response is held (`fault_hold = 4`) to make the later element's
  flight deterministic.** At the default latency the "later element in flight at
  the fault" property is a race: with vl = 6 it holds naturally for k = 0 and
  k = 2 and not for k = 1 or k = 3. The hold models a memory that reports the
  fault after the next element has been issued, which is a legitimate timing and
  makes the scenario reproducible rather than incidental.
* **Only unit-stride macros are driven.** The stop rule is a property of the
  item sequence, not of the address mode, and the abort path is shared; but the
  strided, indexed, segmented, whole-register and mask forms are not swept with a
  stop here (their no-stop behaviour is `rvv.memory_modes`').
* **The vector unit is still not wired into the core** (`mosaic_core.sv` has zero
  `mosaic_vec` references), and interrupts at element boundaries are still a unit
  handshake, not a core interrupt controller — both as I-057 already reported.
* **The clause-4 permission is deliberately not taken.** The spec would allow an
  in-flight element ≥ k to be written back provided the restart overwrites it;
  this unit implements R1's stronger "unperformed" statement. A case that
  *accepted* the permitted set would be a different check, not this one.

## 7. Files

| file | change |
|---|---|
| `rtl/core/mosaic_vec_lsu.sv` | `discard_q` splits stop-issuing from discard; the write-back push is gated by `!discard_q`; the three boundary-stop paths set only `abort_q`; the stop rule and the per-kind table are in the header; three new `-D` controls |
| `sim/unit/tb_vec.cpp` | the in-flight observation, `fault_hold`, the `ClearFaults` fix, the `coalesce-stop` prefix assertion for the uncoalesced run, and the `rvv.stop_path_inflight` phase set (`RunStopPathCase`) |
| `tools/run_stop_path_controls.py` | new: the three controls |
| `results/reports/I-057-stop-path.md` | this report |

`tests/unit/registry.json` and `config/status/implementation_status.json` were
**not** edited; the case stays `"pending": true`.

## 8. How to reproduce

```
python3 tools/run_unit.py --profile p0 --case rvv.stop_path_inflight
python3 tools/run_unit.py --profile p2 --case rvv.stop_path_inflight
python3 tools/run_stop_path_controls.py --profile p0
python3 tools/run_unit.py --profile p0 --case rvv.partial_fault_restart
python3 tools/run_unit.py --profile p0 --case coalesce.element_faults
python3 tools/run_unit.py --profile p0 --case rvv.memory_modes
python3 tools/run_unit.py --profile p1 --case core.act_dut
make check PROFILE=p0
python3 tools/lint_rtl.py --profile p0
python3 tools/lint_rtl.py --profile p2
make lint-cpp PROFILE=p0
```
