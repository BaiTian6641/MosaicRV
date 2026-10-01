# I-021 — bimodal BPU + BTB + RAS

Work package I-021, card `### I-021` in `docs/stage-1-scalar-control.md`.
Case `CASE=predictor.btb_aliasing`.

Files delivered:

| File | Role |
|---|---|
| `rtl/core/mosaic_predictor.sv` | the predictor: bimodal direction table, BTB, RAS |
| `sim/tb/mosaic_predictor_tb.sv` | simulation wrapper; also reads the elaborated geometry back out |
| `sim/unit/tb_predictor.cpp` | C++ driver + independent shadow model + nine phases |
| `results/reports/I-021-predictor.md` | this file |

---

## 1. The interface, and its cycle semantics

One module, `mosaic_predictor`, with **two independent ports**. Both are sampled
on the rising edge of `clk`; `rst` is synchronous and active high.

### Query port — combinational, zero latency

```
q_pc, q_valid, q_is_branch, q_is_jump, q_is_return
  -> pred_taken, pred_target, pred_redirect,
     pred_btb_hit, pred_btb_miss, pred_ras_valid, pred_ras_underflow
```

A PC presented in cycle N produces `pred_*` **in cycle N**. The port reads the
bimodal counter, the BTB entry and the RAS top directly; there is no read
latency and nothing to stall. Fetch presents the PC it is fetching and can
redirect on the same cycle. This is a deliberate choice for the first predictor:
the tables are small, and a registered-read BTB would add a cycle of latency that
nothing in I-021 needs and that I-009's fetch request queue would have to model.

The class inputs (`q_is_branch`, `q_is_jump`, `q_is_return`) are **supplied by the
owner, not sniffed from the encoding.** The predictor deliberately does not decode
instructions: `mosaic_decoder` already does that, and a predictor that re-decoded
would be a second decoder that could disagree with the first one on a reserved
encoding. The cost of this choice is stated plainly in §7: fetch must know the
class before it can query, and I-009 owns that.

### Update port — one resolved transfer per cycle

```
upd_valid, upd_pc, {upd_is_branch, upd_is_jump, upd_is_call, upd_is_return},
upd_taken, upd_target
ckpt_valid, flush
  -> ras_overflow, ras_underflow
```

`upd_valid` with any class bit set is **one** resolved control transfer, applied at
the edge ending that cycle. Two updates in one cycle is not a supported event;
the core serialises them. That is a deliberate simplicity, not an oversight: a
multi-entry update port would be untested width in the first predictor.

`ras_overflow` / `ras_underflow` are combinational and describe **the edge that
has not happened yet**, which is why the driver checks them before the clock edge.
Both are suppressed by `flush`, because a flushed update does not occur at all and
reporting an anomaly for a discarded event would corrupt the caller's counters.

`ckpt_valid` / `flush` are the RAS recovery ports, specified in §4.

---

## 2. What is advisory, and what enforces correctness

**This module has no architectural state.** Every prediction is a hint, and the
resolved truth comes from execution: the target from `mosaic_branch_target`, the
outcome from `mosaic_branch_cmp`. So a wrong prediction cannot change
architectural behaviour — it costs a squash and a refetch.

What the predictor must never be is *silently* wrong. That is why the reports are
outputs rather than internal details:

| Output | Meaning | Why it has to be a port |
|---|---|---|
| `pred_taken` | advisory direction | wrong ⇒ mispredict, recovered |
| `pred_target` | advisory next PC | wrong ⇒ mispredict, recovered |
| `pred_redirect` | advisory "take the target now" | wrong ⇒ mispredict, recovered |
| `pred_btb_hit` | the BTB supplied `pred_target` | attribution |
| **`pred_btb_miss`** | **a target was needed and none existed** | **a fetch unit cannot reconstruct this** |
| `pred_ras_valid` | `pred_target` came from the RAS | attribution |
| `pred_ras_underflow` | a return was predicted on an empty RAS | a RAS anomaly that must not be silent |
| `ras_overflow` | a resolved call found the RAS full | a lost push must not be silent |
| `ras_underflow` | a resolved return found the RAS empty | a dropped pop must not be silent |

`pred_btb_miss` is the load-bearing one. On a miss, `pred_target` is the
sequential `pc + 4` — a perfectly ordinary address. Without the report, "I have no
target" and "my target is the fall-through address" are the *same 64-bit value*,
and a fetch unit that trusts `pred_target` would follow a fall-through it believes
was a prediction. The testbench enforces two invariants on **every cycle of every
phase** because of this:

1. `pred_redirect` and `pred_btb_miss` are never both high — they are
   contradictory.
2. `pred_redirect` never rises without a reported source (`pred_btb_hit` or
   `pred_ras_valid`) — a redirect nobody can attribute is a mispredict nobody can
   count.

Invariant 2 is the acceptance criterion "wrong predictions are recoverable" in
mechanical form: a predictor that is wrong without saying so is worse than one
that never predicts, so nothing may redirect on an unreported guess.

---

## 3. State kept, and why

| State | Width | Reset? | Why it exists |
|---|---|---|---|
| `bpu_ctr[BPU_ENTRIES]` | 2 b/entry | **no** | direction data |
| `bpu_valid[BPU_ENTRIES]` | 1 b/entry | yes | validity held **outside** the array |
| `btb_tag`, `btb_target`, `btb_kind` | 64/64/2 b/entry | **no** | target data |
| `btb_valid[BTB_ENTRIES]` | 1 b/entry | yes | validity outside the array |
| `ras_mem[RAS_ENTRIES]` | XLEN b/entry | **no** | return addresses |
| `ras_sp`, `ras_ckpt` | `$clog2(N+1)` | yes | stack pointer + recovery point |

Reset clears the valid bits and the RAS pointers and **nothing else**, which is the
rule `rtl/common/mosaic_ram.sv` documents: clearing a DEPTH×WIDTH array turns
inferred RAM into DEPTH×WIDTH resettable flip-flops. Validity is tracked outside
the arrays in explicit packed vectors, exactly as the RAM's worked example
prescribes.

### A defect this rule exposed, and the fix

Because the counter array is deliberately never reset, an untrained entry holds
whatever the silicon powered up with. The first version **incremented** that value,
which made the first trained prediction a function of power-up contents rather
than of the branch. The shadow model caught it:

```
MISMATCH reset-determinism: cycle 444: pred_taken: expected 0, got 1
  [q_pc=0x000000009000009c qv=1 qbr=1 qjmp=0 qret=0 | ...]
```

The fix is in the RTL: the **first** training write is an *absolute* value
(`10` for taken, `01` for not-taken), never an increment. Every later update is a
saturating step from a value this module itself wrote. The first trained state is
now a function of the resolved branch alone.

---

## 4. Index truncation, decided explicitly

The index of any structure is the PC's bits `[IDX_W:1]` where `IDX_W = $clog2(DEPTH)`.
Bit 0 is dropped because every instruction address is 4-byte aligned and bit 0 is
therefore always zero — including it would halve the table for nothing. Note that
bit 1 **is** part of the index, which is why an aliasing partner is one whole
index period away, not `+4`.

`$clog2` of a non-power-of-two depth names more indices than the array has
entries. That is a correctness-relevant decision, so it is made here rather than
left to the arithmetic:

* An index `>= DEPTH` is a **guaranteed miss**. The BTB never returns a target for
  it and the direction table never trains it.
* An update landing on such an index is **dropped, never wrapped**. Wrapping would
  alias one PC's tag onto a different PC's entry and manufacture a false hit —
  the one outcome a predictor must never produce.
* The consequence is a **loss of coverage, never a loss of correctness**: with
  `DEPTH = 3`, `$clog2` gives 4 indices, so a quarter of the address space can
  never be allocated. Those PCs take the sequential path and are reported as
  misses every time, which costs fetch bandwidth and nothing else, because a
  prediction is advisory and a miss is reported.

For a power-of-two depth — every profile in `config/geometry/` today — the range
check is a tautology, so it is not elaborated at all:

```systemverilog
generate
  if ((BPU_ENTRIES == (1 << BPU_IDX_W)) && (BTB_ENTRIES == (1 << BTB_IDX_W)))
    begin : g_idx_never_truncates
      assign q_bpu_idx_ok = 1'b1;   // constant, folds away
      ...
```

Emitting a comparison Verilator can prove constant is exactly the dead logic
`-Wall` exists to report (`UNSIGNED: Comparison is constant due to unsigned
arithmetic` — the first draft did emit it, four times).

**The truncation path is proven, not just asserted.** No profile uses a
non-power-of-two depth, so `BTB_ENTRIES=3` / `BPU_ENTRIES=5` were elaborated in a
throwaway harness outside the repository (§8.5).

---

## 5. RAS policy, including the two edge cases the card names

The RAS is updated on **resolve**, not on prediction, and read on prediction. It
therefore lags fetch by the resolve latency and holds no speculative state of its
own. The checkpoint/flush ports cover the one case that still needs them: a
transfer that resolved speculatively and was later squashed.

* `ckpt_valid` saves the **pre-edge** pointer into `ras_ckpt`.
* `flush` restores the pointer from `ras_ckpt` and discards any update offered in
  the same cycle.

When `flush` and `ckpt_valid` arrive together, both read pre-edge values, so the
restore uses the checkpoint as it was *before* that edge. The shadow models this
from a single pre-edge copy; getting it wrong was a real bug in the shadow (§8.3).

**Caller obligation** (I-018 recovery owns it, but the RAS is useless without it):
assert `ckpt_valid` in the cycle of the resolve whose RAS effect may later need
undoing, and `flush` when that resolve turns out to have been squashed. Do **not**
assert `flush` for a committed transfer — undoing a committed call's push loses a
real return address.

### Full stack — `ras_sp == RAS_ENTRIES`

The push is **dropped** and `ras_overflow` pulses. The stack saturates: it does not
wrap, does not overwrite an older entry, and does not move the pointer. All three
alternatives turn a lost push into a *plausible wrong address*, and a plausible
wrong address is precisely what a return-address stack must never emit. The oldest
`RAS_ENTRIES` entries survive; the newest is lost.

### Empty stack — `ras_sp == 0`

The pop is **dropped**, `ras_underflow` pulses, the pointer stays at 0, and the
target comes from the BTB instead — a real second chance, not a dead end. The stack
never underflows past zero and never reads a stale entry. The read address is
guarded to 0 on an empty stack so the arithmetic never forms a negative entry
number.

Both are defined, both are reported, and both are tested (§8, phases 4 and 5).

---

## 6. Training and mispredicts

The direction counter trains on **every** resolved conditional branch, taken or
not, so the counter moves *down* as well as up and a branch that changes
behaviour is followed rather than frozen. Untrained entries are **not-taken** — a
cold table must never hand out an optimistic guess, or the first fetch of every
cold branch pays for it.

The BTB is rewritten from the **resolved** target on every resolving transfer, so
an indirect jump or return that goes somewhere new is self-correcting on its next
resolve. A tag *and* a kind are stored: without the kind, a call's target could be
served for a conditional branch that happened to share the index.

**Mispredict-driven reallocation is deliberately not implemented.** The card makes
it optional; it needs an eviction policy nothing in I-021 can justify, and a second
allocation path would pollute the miss behaviour the aliasing test measures. Every
entry here is allocated by one rule.

The module takes **no `upd_mispredict` input**, and that is deliberate rather than
an oversight. The predictor corrects itself by being rewritten from resolved
values, so it has no use for the signal; `upd_mispredict` belongs to the recovery
path (I-018), not here. The first draft carried the port and `-Wall` correctly
rejected it as `UNUSEDSIGNAL`. Inventing a use for an unused input to silence a
warning would have been the wrong fix.

---

## 7. Known limitations, stated rather than buried

* **Fetch must classify before it can query.** `q_is_branch` / `q_is_jump` /
  `q_is_return` are inputs, so I-009's fetch needs the instruction class available
  at fetch time. In the first integration that is a small local decode or a
  "query and let the BTB decide" mode; either way it is I-009's decision, not a
  hidden assumption here.
* **The RAS lags fetch by the resolve latency**, so a return immediately following
  a call in program order can miss in the RAS. This is a performance cost, not a
  correctness one, and the RAS is a hint. Moving to speculative push/pop is a later
  work package; `ckpt_valid`/`flush` are already in place for it.
* **No multi-entry update port** (§1).
* **A full BTB overwrites on every allocating resolve**; there is no replacement
  policy, so a working set larger than the BTB thrashes. Reported as misses, never
  as wrong answers.
* **`btb_kind` is 2 bits and the kind space is 2** (conditional / unconditional). A
  future caller/return distinction in the BTB would widen it.

---

## 8. Commands run, with output

All commands are from `/Users/flare/MosaicRV`.

### 8.1 Lint — acceptance criterion 1

```
$ python3 tools/lint_rtl.py
ok   rtl/common/mosaic_fifo.sv: clean as mosaic_fifo
ok   rtl/common/mosaic_ram.sv: clean as mosaic_ram
ok   rtl/common/mosaic_skid_buffer.sv: clean as mosaic_skid_buffer
ok   rtl/core/mosaic_alu.sv: clean as mosaic_alu
ok   rtl/core/mosaic_branch_cmp.sv: clean as mosaic_branch_cmp
ok   rtl/core/mosaic_branch_target.sv: clean as mosaic_branch_target
ok   rtl/core/mosaic_bringup_core.sv: clean as mosaic_bringup_core
ok   rtl/core/mosaic_decoder.sv: clean as mosaic_decoder
ok   rtl/core/mosaic_predictor.sv: clean as mosaic_predictor
lint: 10 source file(s) clean
exit=0
```

`rtl/core/mosaic_predictor.sv` is `-Wall` clean with zero warnings. One
`verilator lint_off UNUSEDPARAM` wraps the generated-header `` `include ``: the
generated file carries one localparam per project-wide configuration knob, and
this module names three of them. Without the guard, `-Wall` reports the other 27.

The module's own depths are **not** re-declared as parameters with different
defaults — the `MOSAIC_*` localparams *are* the defaults, so there is exactly one
copy of the geometry in the design.

### 8.2 The case — acceptance criterion 2

```
$ python3 tools/run_unit.py --case predictor.btb_aliasing
PASS predictor.btb_aliasing       task=I-021
exit=0
```

`results/unit/predictor.btb_aliasing/run.log`:

```
$ /Users/flare/MosaicRV/build/p0/unit/predictor.btb_aliasing/predictor.btb_aliasing \
    --case predictor.btb_aliasing --out /Users/flare/MosaicRV/results/unit/predictor.btb_aliasing \
    --seed 1 --max-cycles 200000
RESULT PASS predictor.btb_aliasing predictor contract holds: 9266 shadow comparisons over 4673 cycles, seed 1
```

Seeds 1, 2, 3, 7 and 12345 all pass. The comparison count is identical across
seeds because every phase runs a fixed number of cycles; the seed changes *which*
PCs and outcomes are driven, not how many cycles the campaign takes.

```
seed 1      RESULT PASS ... 9266 shadow comparisons over 4673 cycles, seed 1
seed 2      RESULT PASS ... 9266 shadow comparisons over 4673 cycles, seed 2
seed 3      RESULT PASS ... 9266 shadow comparisons over 4673 cycles, seed 3
seed 7      RESULT PASS ... 9266 shadow comparisons over 4673 cycles, seed 7
seed 12345  RESULT PASS ... 9266 shadow comparisons over 4673 cycles, seed 12345
```

### 8.3 The shadow model is independent, and it earned its keep

The shadow (`ShadowPredictor` in `sim/unit/tb_predictor.cpp`) is written from the
prose contract: it derives the index from the documented bit range, the tag and
kind checks from the documented rule, the saturating counter and the saturating
RAS push from the documented policy. It shares no code with the RTL and never
touches Verilator internals.

It sizes itself from the elaborated DUT (`o_bpu_entries` and friends, read back
from the instance parameters in the wrapper), so **`sim/unit/tb_predictor.cpp`
contains no geometry at all**. A profile with a different BTB depth needs no edit
there, and the shadow cannot disagree with the hardware about how big it is.

It found three real defects during development, all in the direction of the RTL
being wrong or under-specified:

| Found | Defect | Resolution |
|---|---|---|
| `reset-determinism: cycle 444: pred_taken: expected 0, got 1` | RTL incremented an uninitialised counter | RTL §3: first train writes an absolute value |
| `random: cycle 1151: pred_target: expected 0x80000104, got 0x8000023c` | the *shadow* applied `ckpt_valid` before `flush`, so a simultaneous pair restored the new checkpoint | shadow §5; RTL was right |
| `btb-no-false-miss: the untrained probe does not share the index` | the *test* used `pc + 4` as an aliasing partner, but bit 1 is part of the index | probe is now one whole index period away |

The third one is worth calling out: an anti-vacuity guard in the testbench caught a
bug in the testbench. That is the guard doing its job.

### 8.4 Coverage that discriminates — acceptance criterion 3

| # | Phase | What it pins down |
|---|---|---|
| 1 | `reset-state` | cold = not-taken, miss reported, sequential target, no redirect |
| 2 | `btb-aliasing` | two PCs one index period apart, trained apart; the second **must** report a miss and **must not** receive the first's target |
| 3 | `btb-no-false-miss` | the converse: retrained PCs **must hit**; a predictor reporting every query as a miss fails here; an untrained PC sharing an occupied index still misses |
| 4 | `ras` | push `RAS_ENTRIES + 4`, asserting exactly 4 reported overflows; pop the whole stack checking each pushed address; resolve-time and query-time underflow |
| 5 | `ras-mispredict` | a speculative push is undone by `flush`; the next return must read the *committed* call's address |
| 6 | `training` | taken×4 must become taken, then not-taken×4 must become not-taken — both directions |
| 7 | `mispredict-report` | forces genuine mispredicts, asserts none is silent, and **fails if it produced none** |
| 8 | `reset-determinism` | 250-step program, transcript, reset, identical replay, byte-identical |
| 9 | `random` | 4000-cycle soak, shadow compared every cycle; asserts redirects and misses both occurred |

Phase 6's stimulus is not "alternate the branch". A perfect alternation is
followed *exactly* by a 2-bit counter — every not-taken step lands on `01`, every
taken step back on `10` — so an alternating phase produces **zero** mispredicts and
would pass vacuously. That was caught during development:

```
MISMATCH mispredict-report: the phase produced no wrong prediction at all, so it
proves nothing: the stimulus has to make the predictor wrong
```

The phase now drives three taken resolves to saturate the counter at `11` and then
a single not-taken, the shortest sequence that forces a wrong prediction (`11 →
10`, still weakly taken). The `wrong > 0` and `redirects > 0` guards keep that
honest.

Phase 7's guards are the acceptance criterion "a mispredict is reported for every
case the DUT gets wrong, not just some". Phase 9 asserts both `redirects > 0` and
`misses > 0`, so a campaign in which nothing ever hit *and* nothing ever missed
cannot pass by doing nothing.

### 8.5 Non-power-of-two truncation, proven

No profile uses a non-power-of-two depth, so the truncation arm was elaborated and
run in a throwaway harness outside the repository (`/tmp/npot`, not committed),
instantiating `BTB_ENTRIES=3` and `BPU_ENTRIES=5`:

```
allocatable BTB index hits after training                    ok
allocatable BPU index trained taken                          ok
BTB index >= DEPTH is a reported MISS, never a hit           ok
untrained BTB index is a reported MISS                       ok
BPU index >= DEPTH never trains: untrained means not-taken   ok
PASS: 0 failure(s)
run exit=0
```

`$clog2(3) = 2` names indices 0–3 for 3 entries, so index 3 is the unreachable
one — exactly the "quarter of the address space" in §4 — and it neither hits nor
trains.

### 8.6 C++ is held to the project's own stricter standard

```
$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow \
    -Ibuild/p0/sim -Isim/common -Ibuild/p0/unit/predictor.btb_aliasing/obj_dir \
    -I$(verilator -getenv VERILATOR_ROOT)/include sim/unit/tb_predictor.cpp

sim/unit/tb_predictor.cpp: total=64 warnings, 0 in this file
sim/unit/tb_ram.cpp:       total=56 warnings, 0 in this file   (pre-existing)
```

All of them are inside Verilator's own runtime headers, which is why the Makefile
compiles our sources separately from the simulator build. Nothing in
`sim/unit/tb_predictor.cpp` produces a warning.

---

## 9. Mutation testing — acceptance criterion 4

`MOSAIC_PREDICTOR_MUTANT_<n>` blocks are **off in the shipping build**. For each
one the evidence below records three separate facts, because a `-D` whose `ifdef`
block was never written compiles to the shipping build and "passes" vacuously —
that has happened twice in this project:

1. the `` `ifdef `` block **exists** in the RTL (`grep` count),
2. the mutant binary **differs** from the shipping binary (`cmp`), so the define
   changed code rather than defining nothing,
3. the run **exits 1** with a `RESULT FAIL` line.

Build command per mutant (the runner's flags reproduced, plus `-D`):

```sh
verilator --cc --exe --build -j 0 -O2 --x-assign unique --x-initial unique \
  -D<DEFINE> --top-module mosaic_predictor_tb -Mdir /tmp/mut/<DEFINE>/obj_dir \
  -Ibuild/p0/rtl -Ibuild/p0/sim -Irtl/core -Irtl/common \
  -CFLAGS -Isim/common -CFLAGS "-O2 -std=c++17 -Wall" \
  -o /tmp/mut/<DEFINE>/predictor.btb_aliasing \
  rtl/core/mosaic_predictor.sv sim/tb/mosaic_predictor_tb.sv \
  sim/unit/tb_predictor.cpp sim/common/sim_common.cpp
/tmp/mut/<DEFINE>/predictor.btb_aliasing --case predictor.btb_aliasing \
  --seed 1 --max-cycles 200000
```

### Mutant table

| # | Define | Injected defect | Phase that catches it | Observed failure |
|---|---|---|---|---|
| 1 | `MOSAIC_PREDICTOR_MUTANT_NO_BTB_TAG` | the tag comparison is removed: `btb_hit = btb_valid[idx]`, so an index hit counts regardless of which PC is in the entry | `btb-aliasing` | `cycle 16: pred_target: expected 0x0000000000000004, got 0x0000000080000300` |
| 2 | `MOSAIC_PREDICTOR_MUTANT_NO_RAS_FLUSH` | `flush` is ignored, so a squashed call's speculative push stays on the stack | `ras-mispredict` | `cycle 93: pred_target: expected 0x0000000080005004, got 0x0000000080005014` |
| 3 | `MOSAIC_PREDICTOR_MUTANT_NO_TRAIN` | the direction table is never written — a static predictor | `btb-aliasing` | `cycle 23: pred_taken: expected 1, got 0` |
| 4 | `MOSAIC_PREDICTOR_MUTANT_NO_RESET_VALID` | reset clears the RAS pointers but forgets `bpu_valid` / `btb_valid`, so pre-reset entries survive | `mispredict-report` | `cycle 138: pred_taken: expected 1, got 0` |
| 5 | `MOSAIC_PREDICTOR_MUTANT_NO_RAS_UNDERFLOW_REPORT` | a return on an empty stack does not report the underflow | `ras` | `cycle 84: ras_underflow: expected 1, got 0` |

Per-mutant `` `ifdef `` presence and binary-difference checks, from one run:

```
MUTANT: -DMOSAIC_PREDICTOR_MUTANT_NO_BTB_TAG
  ifdef block: present (1 occurrence)
  build: ok
  run exit=1
  binary differs from shipping build: YES
MUTANT: -DMOSAIC_PREDICTOR_MUTANT_NO_RAS_FLUSH
  ifdef block: present (1 occurrence)
  build: ok
  run exit=1
  binary differs from shipping build: YES
MUTANT: -DMOSAIC_PREDICTOR_MUTANT_NO_TRAIN
  ifdef block: present (1 occurrence)
  build: ok
  run exit=1
  binary differs from shipping build: YES
MUTANT: -DMOSAIC_PREDICTOR_MUTANT_NO_RESET_VALID
  ifdef block: present (1 occurrence)
  build: ok
  run exit=1
  binary differs from shipping build: YES
MUTANT: -DMOSAIC_PREDICTOR_MUTANT_NO_RAS_UNDERFLOW_REPORT
  ifdef block: present (1 occurrence)
  build: ok
  run exit=1
  binary differs from shipping build: YES
```

### Full output, verbatim

Mutant 1 — `-DMOSAIC_PREDICTOR_MUTANT_NO_BTB_TAG` (tag comparison removed). This is
the hazard the card names. The failing cycle is the resolve that trains
`pc_a = 0x80000200`, whose BTB index is 0. The query port is idle that cycle
(`q_valid = 0`, `q_pc = 0`), and `pc = 0` lands on the *same* index — so the
mutant's unchecked `btb_hit = btb_valid[0]` claims a hit and returns
`0x80000300`, which is `pc_a`'s target. The shipping build misses there, because
the stored tag `0x80000200` does not match `0`, and returns the sequential
`0 + 4`. It is a false hit handed out on index alone, with no miss reported.

```
MISMATCH btb-aliasing: cycle 16: pred_target: expected 0x0000000000000004, got 0x0000000080000300 [q_pc=0x0000000000000000 qv=0 qbr=0 qjmp=0 qret=0 | upd_pc=0x0000000080000280 uv=1 ubr=1 ujmp=0 ucall=0 uret=0 utaken=0 ckpt=0 flush=0]: expected contract holds, got contract violated
CHECK FAILED: no contract violation in any phase
RESULT FAIL predictor.btb_aliasing contract violated: btb-aliasing: cycle 16: pred_target: expected 0x0000000000000004, got 0x0000000080000300 [...]
```

Mutant 2 — `-DMOSAIC_PREDICTOR_MUTANT_NO_RAS_FLUSH` (RAS not corrected on a
mispredict). The return after the flush yields `0x80005014` — the **squashed**
call's return address — instead of `0x80005004`, the committed one. This is
exactly the silently-wrong-fetch the card warns about.

```
MISMATCH ras-mispredict: cycle 93: pred_target: expected 0x0000000080005004, got 0x0000000080005014 [q_pc=0x0000000080007000 qv=1 qbr=0 qjmp=1 qret=1 | upd_pc=0x0000000000000000 uv=0 ubr=0 ujmp=0 ucall=0 uret=0 utaken=0 ckpt=0 flush=0]: expected contract holds, got contract violated
CHECK FAILED: no contract violation in any phase
RESULT FAIL predictor.btb_aliasing contract violated: ras-mispredict: cycle 93: pred_target: expected 0x0000000080005004, got 0x0000000080005014 [...]
```

Mutant 3 — `-DMOSAIC_PREDICTOR_MUTANT_NO_TRAIN` (static predictor).

```
MISMATCH btb-aliasing: cycle 23: pred_taken: expected 1, got 0 [q_pc=0x0000000080000200 qv=1 qbr=1 qjmp=0 qret=0 | upd_pc=0x0000000000000000 uv=0 ubr=0 ujmp=0 ucall=0 uret=0 utaken=0 ckpt=0 flush=0]: expected contract holds, got contract violated
CHECK FAILED: no contract violation in any phase
RESULT FAIL predictor.btb_aliasing contract violated: btb-aliasing: cycle 23: pred_taken: expected 1, got 0 [...]
```

Mutant 4 — `-DMOSAIC_PREDICTOR_MUTANT_NO_RESET_VALID` (reset forgets the valid bits).

```
MISMATCH mispredict-report: cycle 138: pred_taken: expected 1, got 0 [q_pc=0x000000008000a000 qv=1 qbr=1 qjmp=0 qret=0 | upd_pc=0x0000000000000000 uv=0 ubr=0 ujmp=0 ucall=0 uret=0 utaken=0 ckpt=0 flush=0]: expected contract holds, got contract violated
CHECK FAILED: no contract violation in any phase
RESULT FAIL predictor.btb_aliasing contract violated: mispredict-report: cycle 138: pred_taken: expected 1, got 0 [...]
```

Mutant 5 — `-DMOSAIC_PREDICTOR_MUTANT_NO_RAS_UNDERFLOW_REPORT` (return on an empty
stack unreported).

```
MISMATCH ras: cycle 84: ras_underflow: expected 1, got 0: expected contract holds, got contract violated
CHECK FAILED: no contract violation in any phase
RESULT FAIL predictor.btb_aliasing contract violated: ras: cycle 84: ras_underflow: expected 1, got 0
```

### Honest note on phase attribution

A run stops at the first failure, so the phase that *reports* a mutant is decided
by phase order, not by which phase was designed for it. Two mutants are therefore
reported by an earlier phase than intended:

* **Mutant 3** is caught by `btb-aliasing` rather than `training`, because the
  per-cycle shadow comparison runs before any named assertion. (`btb-aliasing` no
  longer asserts direction at all — `training` owns that — but the shadow
  comparison still sees the divergence first.)
* **Mutant 4** is caught by `mispredict-report` rather than `reset-determinism`,
  for the same reason.

To show that `reset-determinism` specifically detects a predictor that does not
fully reset, it was run first in a throwaway copy of the driver with the same
mutant:

```
$ ./obj/case --case predictor.btb_aliasing --seed 1 --max-cycles 200000
MISMATCH reset-determinism: cycle 272: pred_target: expected 0x00000000900000a0, got 0x00000000900000ac [q_pc=0x000000009000009c qv=1 qbr=0 qjmp=1 qret=1 | upd_pc=0x0000000000000000 uv=0 ubr=0 ujmp=0 ucall=0 uret=0 utaken=0 ckpt=0 flush=0]
CHECK FAILED: no contract violation in any phase
RESULT FAIL predictor.btb_aliasing contract violated: reset-determinism: cycle 272: pred_target: expected 0x00000000900000a0, got 0x00000000900000ac [...]
run exit=1
```

A stale RAS entry surviving reset, by contrast. The shipped phase order runs the
directed phases first precisely so that each one can name the structure at fault;
the reasoning is recorded in a comment beside the phase list in
`sim/unit/tb_predictor.cpp`.

---

## 10. One change outside this package's file list

`tools/run_unit.py` did not pass `-Ibuild/<profile>/rtl`, so
`` `include "mosaic_cfg_pkg.svh" `` could not resolve and no RTL could read the
generated geometry. `tools/lint_rtl.py` and the Makefile's `lint-slang` target both
passed it; the runner was the only one missing it. Reported during the work and
fixed by Main, who added the include directory to `build_case()` with a comment
recording the cause. The alternative considered and rejected was parameterising
with defaults and letting the testbench pass the values — but
`build/<profile>/sim/mosaic_platform.h` carries only the memory map, so the C++
shadow would have had no generated source for the sizes either, and 512/64/16 would
have been duplicated in RTL defaults and in C++ expectations. Two copies of the
geometry is how geometry and hardware drift apart, and this project has been
bitten twice by a constant that was correct when written.
