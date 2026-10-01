# I-005 — valid/ready FIFO and skid buffer

Work package **I-005** of `docs/stage-0-contracts-bringup.md`, implementing the
"CASE=fifo.backpressure" obligation in `docs/implementation-plan.md` §I-005 and the
packet-transfer contract in §1.3.

## What was built

| File | Contents |
| --- | --- |
| `rtl/common/mosaic_fifo.sv` | `mosaic_fifo`, parameterised by `WIDTH` and `DEPTH` |
| `rtl/common/mosaic_skid_buffer.sv` | `mosaic_skid_buffer`, a one-deep fall-through elastic buffer |
| `sim/tb/mosaic_fifo_tb.sv` | Top-level wrapper instantiating depths 1, 2, 3, 8 plus the skid buffer |
| `sim/unit/tb_fifo.cpp` | Driver, independent shadow model, directed and randomised stimulus |

Depths 1, 2, 3 and 8 are all instantiated **in the same top-level module** and driven
from the same binary in the same run, so one execution cross-checks five instances at
five different occupancies. Each instance has its own stimulus ports and its own
random stream (seeded `seed + 0x9e37 * n`), so a bug that only manifests at one depth
under one stimulus pattern cannot hide behind the others. The skid buffer is exercised
inside the registered case rather than in a separate ad-hoc build, because a stimulus
path that no registry entry runs is not stimulus a later gate will run.

### Interface

`mosaic_fifo #(.WIDTH(int), .DEPTH(int))`

| Port | Dir | Width | Meaning |
| --- | --- | --- | --- |
| `clk` | in | 1 | rising edge |
| `rst` | in | 1 | synchronous, active high |
| `in_valid` | in | 1 | producer offers an item |
| `in_ready` | out | 1 | FIFO can take it; 0 exactly when full |
| `in_payload` | in | `WIDTH` | offered item; must be held while `in_valid && !in_ready` |
| `out_valid` | out | 1 | consumer is offered an item; 1 exactly when non-empty |
| `out_ready` | in | 1 | consumer takes it |
| `out_payload` | out | `WIDTH` | offered item; don't-care when `out_valid` is 0 |
| `count` | out | `$clog2(DEPTH+1)` | entries currently held |

`mosaic_skid_buffer #(.WIDTH(int))` has the same ports minus `count`.

## Timing contract, in prose

A transfer happens on the input port on exactly those rising edges where
`in_valid && in_ready` are both high; likewise on the output port for
`out_valid && out_ready`. Nothing else moves data, in either direction.

`in_ready` is a function of the registered `count` alone: it is 0 if and only if
`count == DEPTH`. `out_valid` is 0 if and only if `count == 0`. Consequently no
combinational path leaves this module toward either neighbour, and **no combinational
path enters it from `out_ready`**. That is a deliberate trade: it is what makes the
FIFO's input ready safe to hang on a long ready path, and it is what costs the FIFO a
bubble in the full-and-draining corner (see *Redundancy* below).

`out_payload` is the storage word selected by the read pointer. Because the read
pointer advances only on an output transfer, and because a concurrent write can only
target the word selected by the write pointer — which coincides with the read pointer
only when the FIFO is full, i.e. exactly when `in_ready` is 0 — the head item cannot be
overwritten during a stall. **Payload stability under stall is therefore structural,
not a check that happens to hold today.**

Reset is synchronous. On a rising edge with `rst` high, `count`, `rd_ptr` and `wr_ptr`
clear. The payload array is not reset: reset cost is `$clog2(DEPTH+1) + 2*PtrW` state
words, independent of `DEPTH`, and the array is data rather than control state. Because
reset is synchronous, the outputs still show pre-reset state *for the reset cycle
itself*; they are clean from the first cycle after the reset edge. Consumers must not
treat a transfer during a reset cycle as a transfer, and the testbench models it that
way. `DEPTH == 1` is supported without a degenerate zero-width pointer wrap: the
pointer width is forced to 1 and the wrap is an explicit comparison against `DEPTH-1`,
never a truncating increment. `DEPTH == 3` is covered because its pointer is 2 bits wide
while only 0..2 are reachable, so a naive `ptr + 1` wrap would alias — it is in the
sweep for that reason.

The skid buffer is fall-through: `out_valid = valid_r || in_valid` and
`out_payload = valid_r ? payload_r : in_payload`, so while it holds nothing the producer
and consumer can transfer in the same cycle with no latency, and once it holds an item
the output is that item. `in_ready = !valid_r || out_ready`, which gives the three
properties the test asserts directly: with the output stalled and the buffer empty it
still absorbs one item; it presents the first-in item; and while that item is held and
the output is still stalled it refuses a second item.

**Where the skid buffer may not be used**, and why: it must not be used to bridge clock
domains (it is a single-domain elastic buffer with no synchroniser; a CDC crossing needs
a handshake or an async FIFO, per the reset/CDC obligations in I-024); it must not be
used to absorb more than one cycle of downstream stall, since it holds exactly one item
and a burst of N stalled cycles still reaches the producer after the first one (use
`mosaic_fifo` with `DEPTH >= N` for that); and it must not be used to hide an
arbitrarily long combinational ready path. `out_ready -> in_ready` is combinational
*through* this module and so is `in_payload -> out_payload`. One instance therefore
costs exactly one extra stage of ready/payload combinational delay, and chaining
instances multiplies that. It is legal only where the registered timing budget has room
for the single extra stage. Where it does not, `mosaic_fifo` adds no combinational path
at all. It is also not a credit source: it stores no credit, it only avoids a bubble.

## Invariants asserted

Checked **every cycle, on every instance**, against a shadow model that is structurally
different from the DUT. The DUT is a ring of two pointers plus an occupancy counter; the
model is a `std::deque` of payload values with its own full/empty predicate. Nothing in
the checker reuses the DUT's arithmetic, so a bug in the pointer wrap, the full/empty
compare or the occupancy update cannot cancel against the same bug in the checker.

- `in_ready` equals the model's `items.size() < depth` (for the fall-through skid
  buffer, `items.empty() || out_ready`).
- `out_valid` equals the model's prediction (`!items.empty()`, or `!items.empty() ||
  in_valid` for the skid buffer).
- `count` equals the number of items held.
- `out_payload` equals the **first-in** item — the packet sequence check, and during a
  stall the "no stall changes the head item" check. For an empty fall-through skid buffer
  it equals the producer's own bus, which is what catches a non-fall-through regression.
- **Payload stability**: if the previous cycle was `out_valid && !out_ready`, this cycle
  must still be `out_valid` with an unchanged payload. Sampled every stalled cycle, not
  inferred.
- **Conservation**, every cycle: `accepted == emitted + buffered + explicitly_cancelled`,
  where `cancelled` is incremented by exactly the items discarded on a reset edge and
  nowhere else. This is the card's acceptance criterion, evaluated continuously rather
  than only at the end.
- **No combinational dependence on `out_ready`**: within each settled cycle the test
  perturbs `out_ready` and re-evaluates, requiring that no presented output of a FIFO
  moves. This is asserted, not merely documented.
- **Post-reset**: on the first settled cycle after a reset edge, `out_valid == 0`,
  `in_ready == 1`, `count == 0`, for every instance.
- **Coverage floors**, asserted at the end so a stimulus change that stops reaching a
  state fails the case instead of passing over a shorter path: every instance must have
  reached full (FIFOs), empty (FIFOs), a stall, and a stall *while holding an item it
  could not hand on*, and must have both accepted and emitted.

## Negative controls

Each mutant is one `-D` macro selecting exactly one broken behaviour, confined to an
`ifdef` that is **off in the shipping build**. The shipping build defines none of them.

| # | Macro | Injected fault | What catches it | Result |
| --- | --- | --- | --- | --- |
| 1 | `MOSAIC_FIFO_MUTANT_DROP_ON_FULL` | `in_ready` is one entry too optimistic; the acked transfer is then discarded | `in_ready` and `count` vs. shadow, **and** conservation: `accepted=1907` vs `emitted=867 + buffered=126 + cancelled=914` | FAIL, exit 1 |
| 2 | `MOSAIC_FIFO_MUTANT_PHANTOM_POP` | `out_valid` ignores emptiness and offers a phantom transfer | `out_valid` vs. shadow, **and** conservation in the other direction: `emitted=1778` exceeds `accepted=993` — data from nowhere | FAIL, exit 1 |
| 3 | `MOSAIC_FIFO_MUTANT_UNSTABLE_OUT` | output mux fed by the live input bus, so the head item is re-sampled every cycle | the dedicated **`out_payload held during stall`** check fires, plus sequence mismatch | FAIL, exit 1 |
| 4 | `MOSAIC_FIFO_MUTANT_STALE_RESET` | reset clears `wr_ptr` but forgets `count` and `rd_ptr`, so pre-reset items escape as fresh phantom transfers | **`no stale valid or payload escapes reset`**: `out_valid=1`, `count=8` on `fifo_d8` immediately after the reset edge | FAIL, exit 1 |

Negative control 4 is the mirror of a positive check: the same `CheckPostResetClean`
assertion runs at power-on reset *and* after every mid-stall reset in the randomised
phase, so the mutant is caught by a check the shipping build also runs.

The skid buffer's three usage properties were likewise confirmed non-vacuous. With a
throwaway build in which `in_ready` was hardwired to 1 (ignoring backpressure), the case
failed with 5112 failed checks, the first being `skid.in_ready (cycle 6)`. That throwaway
was deleted and is not part of the deliverable; it exists to show the skid properties are
really exercised.

## Redundancy: is the skid buffer earning its place?

**At the depths the card names, it is not redundant — but only because of one specific
corner.** A throwaway probe drove every instance full, then asserted `out_ready` and
`in_valid` together in the same cycle (the "full while draining" corner):

```
after fill: d1 ready=0 d2 ready=0 d3 ready=0 d8 ready=0
drain+offer (out_ready=1, in_valid=1) from full:
  d1 depth=1 in_ready=0 out_valid=1  -> same-cycle accept+emit = NO (bubble)
  d2 depth=2 in_ready=0 out_valid=1  -> same-cycle accept+emit = NO (bubble)
  d3 depth=3 in_ready=0 out_valid=1  -> same-cycle accept+emit = NO (bubble)
  d8 depth=8 in_ready=0 out_valid=1  -> same-cycle accept+emit = NO (bubble)
  skid      in_ready=1 out_valid=1   -> same-cycle accept+emit = YES
```

So the condition under which the skid buffer earns its place is precisely: **a channel
whose producer cannot tolerate one cycle of bubble at the full-to-draining
transition.** Everywhere else it is redundant with `mosaic_fifo` at any depth >= 1, and
it is strictly the more expensive option because it reintroduces a combinational ready
path (`out_ready -> in_ready`) and a combinational payload path that the FIFO does not
have. The per-instance coverage counters show the same thing from the other direction:
`fifo_d1 push_and_pop=0` over the whole run — a depth-1 FIFO can *never* accept and emit
in the same cycle — while the skid buffer does so 997 times.

This module is **not currently instantiated by any other RTL in the repository**;
`mosaic_fifo` is. It is kept, tested and documented here because the card names it as
part of I-005, but it has no downstream user yet, and that is a decision for the
integrator rather than something to assume.

## Commands run, with real output

Tool version: `Verilator 5.052 2026-09-05 rev vUNKNOWN-built20260905`, Python 3.9.6,
macOS 25.6.0 arm64.

### Lint — zero warnings, warnings treated as errors

```
$ verilator --lint-only -Wall --top-module mosaic_fifo_tb \
    sim/tb/mosaic_fifo_tb.sv rtl/common/mosaic_fifo.sv rtl/common/mosaic_skid_buffer.sv
- V e r i l a t i o n   R e p o r t: Verilator 5.052 2026-09-05 rev vUNKNOWN-built20260905
- Verilator: Built from 0.084 MB sources in 4 modules, into 0.052 MB in 3 C++ files needing 0.000 MB
- Verilator: Walltime 0.009 s (elab=0.001, cvt=0.004, bld=0.000); cpu 0.009 s on 1 threads; allocated 11.062 MB
lint exit=0
```

### Unit case

```
$ python3 tools/run_unit.py --case fifo.backpressure
PASS fifo.backpressure            task=I-005
unit exit=0
```

### Mutants

Build command per mutant (N in 1..4, MACRO as tabulated above):

```
verilator --cc --exe --build -j 0 -O2 \
  -CFLAGS "-O2 -std=c++17 -Wall -Wextra" \
  --x-assign unique --x-initial unique \
  --top-module mosaic_fifo_tb -Mdir /tmp/mutN/obj_dir \
  -CFLAGS "-I$PWD/sim/common" -o /tmp/mutN/tb \
  -DMOSAIC_FIFO_MUTANT_<NAME> \
  sim/tb/mosaic_fifo_tb.sv rtl/common/mosaic_fifo.sv rtl/common/mosaic_skid_buffer.sv \
  sim/unit/tb_fifo.cpp sim/common/sim_common.cpp
/tmp/mutN/tb --case fifo.backpressure --seed 1 --max-cycles 20000 --out /tmp/mutN/out
```

Results, verbatim RESULT lines and exit codes:

```
MUTANT 1 (MOSAIC_FIFO_MUTANT_DROP_ON_FULL)  exit=1 -> RESULT FAIL fifo.backpressure 37936 failed of 99848 checks over 3189 cycles
MUTANT 2 (MOSAIC_FIFO_MUTANT_PHANTOM_POP)  exit=1 -> RESULT FAIL fifo.backpressure 32068 failed of 97198 checks over 3189 cycles
MUTANT 3 (MOSAIC_FIFO_MUTANT_UNSTABLE_OUT) exit=1 -> RESULT FAIL fifo.backpressure 9319 failed of 97069 checks over 3189 cycles
MUTANT 4 (MOSAIC_FIFO_MUTANT_STALE_RESET)  exit=1 -> RESULT FAIL fifo.backpressure 21200 failed of 96943 checks over 3189 cycles
```

First failing checks, verbatim:

```
# Mutant 1 — full-drop
MISMATCH fifo_d1.in_ready (cycle 6): expected 0x0000000000000000, got 0x0000000000000001
CHECK FAILED: fifo_d1.count (cycle 7): expected 0x0000000000000002, got 0x0000000000000001
  fifo_d1  depth=1 accepted=1907 emitted=867 cancelled=914 buffered=126

# Mutant 2 — phantom pop
MISMATCH fifo_d1.out_valid (cycle 0): expected 0x0000000000000000, got 0x0000000000000001
  fifo_d1  depth=1 accepted=993 emitted=1778 cancelled=3 buffered=0

# Mutant 3 — unstable output
MISMATCH fifo_d1.out_payload (cycle 6): expected 0x0000000000000001, got 0x0000000000000002
CHECK FAILED: fifo_d2.out_payload held during stall (cycle 7): expected 0x0000000000000002, got 0x0000000000000003

# Mutant 4 — stale valid out of reset
MISMATCH fifo_d1.in_ready (cycle 192): expected 0x0000000000000001, got 0x0000000000000000
CHECK FAILED: fifo_d1.out_valid (cycle 192): expected 0x0000000000000000, got 0x0000000000000001
CHECK FAILED: fifo_d8.count (cycle 192): expected 0x0000000000000000, got 0x0000000000000008
```

### Seed sweep

The case is seed-driven; the registry pins seed 1, but the shadow model and the
randomised phase are not seed-specific:

```
seed=0     exit=0 -> RESULT PASS fifo.backpressure 97307 checks over 3189 cycles, 16 reset cycles
seed=1     exit=0 -> RESULT PASS fifo.backpressure 97069 checks over 3189 cycles, 16 reset cycles
seed=2     exit=0 -> RESULT PASS fifo.backpressure 97408 checks over 3189 cycles, 16 reset cycles
seed=7     exit=0 -> RESULT PASS fifo.backpressure 97561 checks over 3189 cycles, 16 reset cycles
seed=12345 exit=0 -> RESULT PASS fifo.backpressure 97557 checks over 3189 cycles, 16 reset cycles
seed=999983 exit=0 -> RESULT PASS fifo.backpressure 97380 checks over 3189 cycles, 16 reset cycles
```

### Coverage reached at seed 1 (printed by the case itself)

```
  fifo_d1  depth=1 accepted=873 emitted=867 cancelled=6 buffered=0 full=1611 empty=1562 stall=744 push_and_pop=0
  fifo_d2  depth=2 accepted=1350 emitted=1338 cancelled=12 buffered=0 full=846 empty=736 stall=1099 push_and_pop=548
  fifo_d3  depth=3 accepted=1509 emitted=1491 cancelled=18 buffered=0 full=649 empty=411 stall=1271 push_and_pop=749
  fifo_d8  depth=8 accepted=1682 emitted=1634 cancelled=48 buffered=0 full=396 empty=202 stall=1337 push_and_pop=886
  skid     depth=1 accepted=1376 emitted=1370 cancelled=6 buffered=0 full=0 empty=0 stall=1169 push_and_pop=997
```

## Deliberately not done, with reasons

- **No flush / cancel port on the FIFO.** The port list is fixed by the interface
  contract, and discarding buffered data is done with `rst`. This keeps every discarded
  item accounted exactly once as the `explicitly_cancelled` term of the conservation
  invariant, rather than introducing a second, separately-tracked discard path that the
  card's acceptance criterion would then have to reconcile.
- **No block-RAM (registered read) mode.** It would add a cycle of read latency and so
  change the output handshake. That is a different module with a different contract, not
  a mode of this one; conflating them would make the interface ambiguous.
- **No asynchronous-FIFO variant.** CDC is I-024's obligation, and a single-domain
  elastic buffer with a synchroniser bolted on would be a claim not backed by evidence
  here.
- **The skid buffer has no downstream user yet.** Stated plainly above rather than
  quietly retained; it is the integrator's call whether to keep it.
- **No formal proof.** Properties are checked by simulation and by the mutation table
  above. The card's verification basis is a bounded model plus lint, which is what is
  delivered; a formal claim would need its own evidence.

## Two defects found and fixed during bring-up

Recorded because both are the kind that a happy-path-only test would have shipped.

1. **Skid buffer captured an item it had not accepted.** The original stalled branch was
   `valid_r <= in_valid; if (in_valid) payload_r <= in_payload;`, which latched the
   payload on every cycle `in_valid` was high regardless of `in_ready`. With the output
   stalled and the buffer already holding an item, that overwrote the legitimately held
   item with the producer's *unaccepted* offer, and then presented it. Caught by the
   directed `refuses a second item while one is held` property. Fixed to latch only when
   `in_ready` is high — i.e. `valid_r <= valid_r || in_valid; if (!valid_r && in_valid)`.
2. **Post-reset assertion sampled one cycle late.** The reset state machine in the
   randomised phase consumed a cycle of fresh randomised stimulus after the reset burst
   before taking the post-reset assertion, so a FIFO could legitimately have been
   refilled by the time it was checked. The mutation was invisible for that reason.
   Rewritten as an explicit cycle sequence that asserts on the first settled cycle after
   the final reset edge.

Both were defects in the testbench or in the skid buffer, not in `mosaic_fifo`; the FIFO
RTL passed as first written. Neither would have been caught by a happy-path-only case.