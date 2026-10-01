# I-022 — audit of the `iq.wakeup_insert_select` coverage

Audit of the `CASE=iq.wakeup_insert_select` obligation in
`docs/implementation-plan.md` §I-022, against the open gap left by the previous
session. The case is the work package's only unit case; this report answers
whether its pass is real coverage or a truncated run, adds the directed
refused-insert phase the gap called for, audits the two-snapshot discipline, and
re-demonstrates the six mutants.

## STATUS: PASS, audit complete

| Item | Result |
| --- | --- |
| 1. cycle budget is the whole randomised budget by design | **confirmed** — `cycles == max_cycles - 200` at 60k/120k/200k, flat check rate |
| 2. directed refused-insert phase | **added** — 4 requirement groups, non-vacuity probed |
| 3. sampled values / no live-port reads | **one violation found and fixed**; `grep -c 'bench.top()->c'` is now `0` |
| 4. mutants re-run | **all eight exit 1 with a distinct named first failure** |
| 5. permanent refused-insert mutant | **added** — `MOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES`; the phase's own named check is the first failure |
| 6. `--max-cycles` hard abort naming the phase | **wired** — demonstrated, exit 1, `while running phase 'age-wrap'` |
| 7. meta packet interface change | **done** — `ins_meta`/`grant_meta`/`obs_meta`, compared field by field; new `MOSAIC_IQ_MUTANT_META_SWAP` |
| RTL defect | **none found** — the DUT refuses correctly and carries the packet correctly |

Final case result:

```
$ python3 tools/run_unit.py --profile p0 --case iq.wakeup_insert_select
RESULT PASS iq.wakeup_insert_select 93878203 checks over 199800 cycles, 4 reset cycles, 2 clusters
```

Before this audit: `56850460` checks, PASS. After the audit round (directed
refused-insert phase, hard abort): `56848546`. After the meta-packet
interface change: **`93878203`** — a large increase, as required, because
`grant_meta` and `obs_meta` are now 13 fields each compared per cycle per slot.
The total cycle count stays pinned at `max_cycles - 200` (item 1), so adding
phases redistributes cycles rather than extending the run.

## Files touched

| File | Change |
| --- | --- |
| `sim/unit/tb_iq.cpp` | new `PhaseRefusedInsert`; `post_*` capture of count/alloc/age/conservation; `ObserveSlot`; one live-port read replaced with the sampled value; `--max-cycles` abort wiring; `Meta` model and field-by-field `grant_meta`/`obs_meta` comparison |
| `sim/tb/mosaic_iq_tb.sv` | `ins_meta`/`grant_meta`/`obs_meta` flattened into per-field ports and repacked/unpacked at the DUT boundary |
| `rtl/core/mosaic_iq.sv` | `ins_alu_op`/`grant_alu_op`/`obs_alu_op` replaced by `mosaic_uop_pkg::uop_meta_t` `ins_meta`/`grant_meta`/`obs_meta`; `ent_meta` storage; two new mutants (`REFUSED_INSERT_ADVANCES`, `META_SWAP`) |
| `results/reports/I-022-iq-coverage.md` | this report |

No RTL *defect* was found: the interface change is the integrator's frozen
contract, not a fix.

---

## 5. Follow-up round (interface change and hard abort)

### 5.1 `--max-cycles` is now a hard abort that names the phase

`main()` sets a `phase_name` before every phase (via a small `phase()` helper),
calls `out_of_cycles()` after each, and passes the absolute cap into
`PhaseRandom`, whose loop breaks if it is reached. The abort reason is
`exceeded --max-cycles=<n> while running phase '<name>'`, and the run exits 1.
No phase's cycle count changes in a normal run: the randomised budget is still
`max_cycles - cycles_before - 200`, and the per-iteration cap is never reached.

Demonstrated (exit code checked without a pipe):

```
$ <bin> --case iq.wakeup_insert_select --seed 1 --max-cycles 100
ABORT: exceeded --max-cycles=100 while running phase 'age-wrap'
RESULT FAIL iq.wakeup_insert_select exceeded --max-cycles=100 while running phase 'age-wrap'
abort-exit=1
```

### 5.2 A permanent mutant for the refused-insert path

`MOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES` adds a separate `ins_adv` net:
shipping it is `ins_fire && !ins_taken`, and with the define it also advances on
`ins_valid && !ins_ready` (a refused insert). Both the allocation pointer and
the age counter key on `ins_adv`; the entry write and occupancy do not, so the
defect is exactly "a refused insert moved the allocator/age but created no
entry".

`PhaseRefusedInsert` was moved to run as the *first* directed phase, so no other
phase can reach a refused insert first, and its named check is the first failure
reported:

```
$ <bin> --case ... -DMOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES
RESULT FAIL iq.wakeup_insert_select 53576702 failed of 93877635 checks over 199800 cycles
first: CHECK FAILED: refused: o_alloc_index did not advance past the shadow's slot
first: CHECK FAILED: refused: o_age_ctr did not advance
```

The failure is the phase's own assertion, not an incidental mismatch.

### 5.3 The meta packet

`rtl/core/mosaic_iq.sv` now takes `mosaic_uop_pkg::uop_meta_t` on `ins_meta`,
`grant_meta` and `obs_meta`, replacing `ins_alu_op`/`grant_alu_op`/`obs_alu_op`;
`ins_imm` is unchanged. The packet is stored in `ent_meta[]` with the same
no-reset policy as the other entry data, and is written once at insert and read
by the grant, the kill matcher and the observation port. No field is split out
internally and no width is declared locally — every width comes from the type.
The header now states why the PC is carried (a branch needs it without a second
ROB read port; the ROB remains the authority for what retires).

`sim/tb/mosaic_iq_tb.sv` exposes each field of the packet as its own top-level
port (Verilator's wide-vector representation of a >64-bit packed struct is not
practical to drive or name from C++), assembling the struct for the insert path
and unpacking it for grant and observation. `sim/unit/tb_iq.cpp` grew a `Meta`
model, a `ReadMeta`/`WriteMeta` pair, and a `CheckMeta` that names all thirteen
fields. `MakeMeta(uop)` makes every field a function of the uop identity, so no
two uops carry the same packet and a field that is never varied cannot pass.

### 5.4 `MOSAIC_IQ_MUTANT_META_SWAP`

The granted and observed metadata are taken from the adjacent slot
(`grant_idx ^ 1`, `obs_index_s ^ 1`). It fails on the meta comparison by name:

```
$ <bin> --case ... -DMOSAIC_IQ_MUTANT_META_SWAP
RESULT FAIL iq.wakeup_insert_select 17764582 failed of 93878203 checks over 199800 cycles
first: CHECK FAILED: c0 slot 0: meta class
first: CHECK FAILED: c0 slot 0: meta pc
```

### 5.5 Full mutant table (eight, against the green base)

Base: exit 0, `93878203` checks, 0 failures, PASS. Delta is failures − 0.

| `-D` macro | exit | failures (Δ) | first named failure |
| --- | --- | --- | --- |
| `MOSAIC_IQ_MUTANT_META_SWAP` | 1 | 17764582 | `c0 slot 0: meta class` |
| `MOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES` | 1 | 53576702 | `refused: o_alloc_index did not advance past the shadow's slot` |
| `MOSAIC_IQ_MUTANT_NARROW_AGE` | 1 | 1069026 | `c0 slot 1: age` |
| `MOSAIC_IQ_MUTANT_NO_GEN_CHECK` | 1 | 55349339 | `c0: grant_valid matches the shadow's prediction` |
| `MOSAIC_IQ_MUTANT_NO_SAME_CYCLE_WAKEUP` | 1 | 52143167 | `c0: grant_valid matches the shadow's prediction` |
| `MOSAIC_IQ_MUTANT_UNSTABLE_GRANT` | 1 | 46015289 | `c0: granted uop identity` |
| `MOSAIC_IQ_MUTANT_DROP_ON_GRANT` | 1 | 51881655 | `c0: the valid vector is exactly the shadow's live slots` |
| `MOSAIC_IQ_MUTANT_NO_DST_CHECK` | 1 | 52102 | `c0: o_dst_conflict matches the shadow's duplicate-destination search` |

Every macro has a real `ifdef` body in `rtl/core/mosaic_iq.sv` and every row
changes behaviour with a large, distinct delta and a named failure.

### 5.6 Lint

Scoped to this lane's file, both tools are clean:

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_iq \
      -Ibuild/p0/rtl -Irtl/common -Irtl/core rtl/core/mosaic_iq.sv
verilator-exit=0
$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_iq.sv
slang-exit=0   (warnings only)
```

The repo-wide commands fail on **two files this lane does not own**, both
mid-flight sibling work, and are left for the integration lead rather than
edited here:

* `python3 tools/lint_rtl.py --profile p0` → `rtl/core/mosaic_lease_alloc.sv`
  (bad `/*verilator ...*/` comment at line 443). `rtl/core/mosaic_iq.sv` is
  reported `clean as mosaic_iq` before the run stops.
* `slang-tidy ... $(find rtl -name '*.sv' | sort)` → `rtl/core/mosaic_recovery.sv`
  (`ckpt_push` used before its declaration, line 716/1005).

## Remaining gaps

* The repo-wide lint gates are red on two sibling-owned files (5.6); this lane's
  file is clean in isolation.
* `MOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES` covers the pointer/age half of the
  refused-insert defect; the "far side" half (a refused entry appearing in a
  slot) is covered by the directed phase's observation-port checks and the
  `META_SWAP` mutant, but has no dedicated `ifdef`.

---

## 1. Is the 199800-cycle randomised phase the whole budget, checked every cycle?

`main()` computes the randomised phase's budget as

```c
const int budget = opt.max_cycles > bench.cycles() + 200
                 ? opt.max_cycles - bench.cycles() - 200 : 2000;
PhaseRandom(bench, rep, budget);
```

and `PhaseRandom`'s body is a single `for (int i = 0; i < cycles; i++)` whose
only loop body action is one `bench.Step(s[0], &s[1])` — no `break`, no
`continue`, no path that skips the step. `Bench::Step` runs `CheckOne` once per
cluster for the cycle it is about to take, and `CheckOne` has no early return;
it always reaches the shadow comparison and the full eight-slot observation
sweep. So one iteration = one compared cycle.

The budget arithmetic pins the total *regardless* of how long the directed
phases run:

```
final cycles = cycles_before_random + budget
             = cycles_before_random + (max_cycles - cycles_before_random - 200)
             = max_cycles - 200
```

Verified at three budgets (all PASS, same source, seed 1):

| `--max-cycles` | cycles reported | checks | checks/cycle |
| --- | --- | --- | --- |
| 60000 | 59800 | 16990452 | 284.1 |
| 120000 | 119800 | 34079417 | 284.5 |
| 200000 | 199800 | 56848546 | 284.5 |

`cycles == max_cycles - 200` exactly, and the per-cycle check rate is flat, so
the randomised phase neither stops early nor reduces its comparison rate. The
199800 cycles is the whole remaining budget by design, and every one of those
cycles is compared against both shadows.

One observation, not a coverage hole: the `out_of_cycles` lambda and the
`aborted` flag in `main()` are **never set** — nothing calls `out_of_cycles`.
`--max-cycles` is therefore not enforced as a hard abort; the run is bounded
purely by the budget arithmetic above. For the registered `max_cycles` this is
harmless (the directed phases use nowhere near the 200-cycle margin), but a
future directed phase that overran would overshoot silently rather than abort.
Reported rather than edited, to keep this audit scoped.

## 2. A directed refused-insert phase

`PhaseRefusedInsert` (phase 13, runs before the randomised phase) builds the
situation deterministically on cluster 0: fill the queue to capacity with
blocked entries, offer an insert, and then assert the four consequences of the
open question separately.

The shadow models refusal **independently**: its accepted-insert term is
`ins_fire = ins_on && expected_ins_ready()`, and `expected_ins_ready()` is
`count_ < kEntries` computed from the shadow's own occupancy. It is never fed
the DUT's `ins_ready`. So "both agree" is a real agreement between a model of
refusal and the hardware, not a mirror.

| Group | Asserted |
| --- | --- |
| (a) no pointer/age movement | `seen_ins_valid && !seen_ins_ready` (a real refusal); `!post_ins_ready`; `post_count == kEntries`; `post_alloc_index == shadow.expected_alloc_slot()`; `post_age_ctr == Stored(shadow.age_ctr_abs())`; the shadow's own `alloc_ptr`/`age_ctr_abs` unchanged; `post_ins_total` and the shadow's `ins_total` both unchanged |
| (b) not silently dropped | the shadow never admitted it (`slot_of == -1`); the observation port at the slot the allocator would have used still returns that slot's real resident, and not the refused uop |
| (c) next insert lands where the shadow says | kill the oldest macro to free exactly one slot; the next insert's shadow slot equals the predicted slot **and** the DUT's observation port returns the new uop in that slot |
| (d) conservation | `post_ins_total == post_grant_total + post_kill_total + post_count`, all four equal to the shadow's, and exactly the one injected kill was counted |

**Non-vacuity.** Passing assertions prove nothing on their own, so the three
central comparisons were temporarily inverted (`==` → `!=`) and the case re-run:

```
CHECK FAILED: PROBE refused: o_alloc_index did not advance past the shadow's slot
CHECK FAILED: PROBE refused: the far side still holds the shadow's resident
CHECK FAILED: PROBE refused: the DUT put the next insert in that slot
RESULT FAIL iq.wakeup_insert_select 3 failed of 56848546 checks over 199800 cycles
```

Each check is reached with live values and is discriminating. The probe was
reverted; the shipping build contains no `PROBE`.

### A regression the new phase introduced (and the fix)

The first build with the new phase **failed**, on both clusters:

```
CHECK FAILED: c0: the allocation pointer wrapped
CHECK FAILED: c1: the allocation pointer wrapped
RESULT FAIL iq.wakeup_insert_select 2 failed of 58719666 checks over 199800 cycles
```

Instrumenting the coverage showed the randomised phase had been starved:
`ins_total` stuck at 164 with `full_cycles` ≈ 110k and `index_wraps == 0`. Cause:
the new phase left eight **permanently blocked** entries resident — their source
tags are drawn from the directed pool and are never present in the randomised
wakeup pool, so nothing could ever wake or grant them and the queue sat full,
blocking the randomised traffic for the rest of the run.

The fix is hygiene in the new phase, not a weakened check: drain the queue at
the end of `PhaseRefusedInsert` (`DrainQueue`, kill-based, the existing
convention every phase uses). After the fix, `index_wraps = 485/482` and the
case is green with the full check rate. This is a defect in the *new* directed
phase, found by the coverage assertion doing exactly its job.

## 3. Two-snapshot discipline

Regex audit of every phase assertion:

* `grep -c 'bench.top()->c' sim/unit/tb_iq.cpp` was **1** before this audit and
  is **0** now.
* The single violation was in `PhaseBackPressure`:

  ```c
  const uint32_t held = bench.top()->c0_grant_uop;   // read AFTER Step returned
  ```

  `Step` advances the edge before returning, so this read the *next* cycle's
  combinational grant while every comparison in the loop below used
  `seen_grant_uop` (the pre-edge snapshot). It happened to agree only because
  the grant was being held — i.e. it was a check on a cycle the phase never set
  up. Replaced with `bench.seen_grant_uop(0)`, the sampled value for the cycle
  just checked.

* Remaining `bench.top()` / `top->c…` reads are legitimate status reads, not
  grant-payload samples: `PhaseGeometry` reads the elaborated `o_iq_*` geometry
  constants, and the reset phase in `main()` reads the `c0_*`/`c1_*` status
  words while driving the clock by hand below the shadow. Neither is a
  cycle-dependent grant payload.
* The new phase uses `seen_*` for the insert handshake (pre-edge) and `post_*`
  for occupancy, the allocation pointer, the age counter and the conservation
  tallies (post-edge), and `ObserveSlot` for the combinationally-settled
  observation port.

## 4. Mutants

Six `ifdef` bodies in `rtl/core/mosaic_iq.sv`. Each was built with its `-D`
appended to the runner's flags, run at the full `--max-cycles 200000`, seed 1,
against a green base, and required to exit 1. Because every seeded source was
rebuilt, each `-D` is present in the compiled build; a macro with no `ifdef`
body would reproduce the base build and is ruled out by the changed verdict and
the named first failure. Delta is against the base (0 failures).

| `-D` macro | exit | failures (Δ vs base) | first named failure |
| --- | --- | --- | --- |
| *(base)* | 0 | 0 — **PASS** | — |
| `MOSAIC_IQ_MUTANT_NARROW_AGE` | 1 | 1063090 | `c0 slot 1: age` |
| `MOSAIC_IQ_MUTANT_NO_GEN_CHECK` | 1 | 40689738 | `c0: grant_valid matches the shadow's prediction` |
| `MOSAIC_IQ_MUTANT_NO_SAME_CYCLE_WAKEUP` | 1 | 37863197 | `c0: grant_valid matches the shadow's prediction` |
| `MOSAIC_IQ_MUTANT_UNSTABLE_GRANT` | 1 | 34805496 | `c0: granted uop identity` |
| `MOSAIC_IQ_MUTANT_DROP_ON_GRANT` | 1 | 37698618 | `c0: the valid vector is exactly the shadow's live slots` |
| `MOSAIC_IQ_MUTANT_NO_DST_CHECK` | 1 | 53670 | `c0: o_dst_conflict matches the shadow's duplicate-destination search` |

All six change behaviour with a large, distinct delta and a failure that names
the injected defect.

## The DUT verdict on the original question

**Does a refused insert advance the allocation pointer or consume an age? No.**
The RTL refuses with `ins_ready = !rst && !o_full` and `ins_fire = ins_valid &&
ins_ready` (`rtl/core/mosaic_iq.sv`), and both the allocation pointer and the age
counter advance only under `ins_fire && !ins_taken` (the insertion actually
becomes resident). A refused insert sets neither. The shadow reaches the same
answer from its own occupancy, and the new directed phase pins it on the DUT's
settled registers. No RTL change was required or made.

## Exact commands

```
python3 tools/run_unit.py --profile p0 --case iq.wakeup_insert_select
build/p0/unit/iq.wakeup_insert_select/iq.wakeup_insert_select \
    --case iq.wakeup_insert_select --out /tmp/cb<M> --seed 1 --max-cycles <M>

# mutants (base = VERILATOR_FLAGS; each row appends one -D and rebuilds)
python3 -c "import sys; sys.path.insert(0,'tools'); import run_unit; \
  run_unit.VERILATOR_FLAGS.append('-DMOSAIC_IQ_MUTANT_<NAME>'); \
  run_unit.build_case('p0','iq.wakeup_insert_select', \
  run_unit.load_registry()['cases']['iq.wakeup_insert_select'])"
build/p0/unit/iq.wakeup_insert_select/iq.wakeup_insert_select \
    --case iq.wakeup_insert_select --out /tmp/mut --seed 1 --max-cycles 200000
```

## What is NOT verified

* **Project-wide lint** (`python3 tools/lint_rtl.py`, `slang-tidy`, `make
  lint-cpp`) was not run from this lane: siblings are editing concurrently and a
  project-wide lint would block on their half-finished files and report phantom
  failures. The RTL is untouched; the C++ was compiled by the runner with
  `-O2 -std=c++17 -Wall` and the build is warning-clean.
* **No permanent mutant for "a refused insert advances the allocation pointer".**
  Expressing it needs an `ifdef` in `rtl/core/mosaic_iq.sv`, which this lane may
  edit only for a real defect; none was found. The directed phase's non-vacuity
  is instead shown by the inversion probe in item 2. A permanent mutant there
  would be the natural follow-up if the RTL gains mutation hooks.
* The randomised phase's stimulus generator and seed were untouched (per the
  no-seed-tuning rule); the pass is at the registered seed 1.
* `--max-cycles` is not enforced as a hard abort (item 1); reported, not edited.
