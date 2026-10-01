# I-009 — bounded fetch requests, redirect, and late responses

Work package I-009, card `### I-009` in `docs/stage-1-scalar-control.md`.
Case `CASE=fetch.redirect_late_response`.

Files delivered:

| File | Role |
|---|---|
| `rtl/core/mosaic_fetch.sv` | the fetch request engine: bounded slot table, epoch, classify-before-consume response path, one owned `mosaic_predictor` |
| `sim/tb/mosaic_fetch_tb.sv` | pass-through wrapper; also derives the three narrow port widths from the generated package and reads the geometry back out of the elaborated instance |
| `sim/unit/tb_fetch.cpp` | C++ driver, independent shadow (slots, epochs, credit ledger, predictor), eleven phases |
| `results/reports/I-009-fetch.md` | this file |

Headline, from the run in §9: **PASS**, 235,600 shadow comparisons over 6,098
cycles, `verilator --lint-only -Wall` clean, `slang-tidy --single-unit` 0 errors,
and seven mutants that each fail with a non-zero delta.

---

## 1. Ports, and what a cycle means

All ports are sampled on the rising edge of `clk`. `rst` is synchronous and
active high; during the reset cycle the outputs still show the pre-reset state
and are clean from the first cycle after the reset edge.

### 1.1 Request issue — the fetch unit is the *consumer* here

```
req_valid, req_pc  ->  req_ready, req_id, req_epoch
```

One request is taken on a cycle where `req_valid && req_ready`. `req_id` and
`req_epoch` describe **the request that was taken** and are meaningful in that
cycle only; when `req_ready` is low they are not a promise and must not be used.

`req_ready` is low when the slot table has no free slot *and* no slot is being
freed by this cycle's response, and it is unconditionally low on a cycle where
`redirect_valid` is high. That second rule is a choice, not an accident: a
request taken on a redirect cycle would be issued under an epoch that the
redirect is retiring at that very edge, and there is no epoch value that is
correct for it. Refusing is visible — `req_ready` low, and `deny_count`
increments — so a refusal is never silent. The alternative (accept it and cancel
it a cycle later) buys one request of speculative bandwidth and costs a third
meaning for "cancelled".

**Why fetch consumes requests rather than generating them.** The work package
is a request *engine*, not a fetch sequencer: the PC stream belongs to whoever
will eventually drive it (I-018/I-021 integration), and the thing that has to be
correct here is the accounting around the requests. Having the testbench be the
requester is also the only way the named case is constructible at all: a
self-driven unit cannot be made to answer a redirect in a chosen order.

### 1.2 Response intake

```
rsp_valid, rsp_id, rsp_epoch, rsp_data, rsp_len, rsp_fault
  ->  rsp_ready, rsp_squashed
```

`rsp_ready` depends only on the output register (`!out_reg_valid || out_ready`)
and on nothing in the payload, so there is no combinational path from the
payload into readiness and no way for a requester and this unit to deadlock on
each other's logic. A response whose id names nothing in particular is still
consumed: it is classified, counted and reported, never ignored.

Responses may arrive in any order. The payload must be stable while
`rsp_valid && !rsp_ready`, per the `fetch_decode` transfer rule; the testbench's
responder holds a response until it is taken for exactly this reason.

Every accepted response falls into exactly one of three classes, and the classes
partition:

| Class | Condition | Effect | Credit |
|---|---|---|---|
| **live** | the slot owns this epoch's request, the slot is not cancelled, the epoch is current, and no redirect is retiring it this cycle | delivered (`out_valid`, or `out_illegal` / `out_fault`) | returned |
| **stale** | the slot owns this response but it is not live — or the epoch has been superseded | dropped, counted, never delivered | returned **iff** the slot still owns it |
| **squashed** | the epoch is current but no slot owns the response | dropped, counted, `rsp_squashed` pulsed | not returned — nothing is owed |

Two questions are deliberately kept apart, and keeping them apart was the one
non-obvious thing in the design:

* **ownership** (`slot_busy[id] && slot_epoch[id] == rsp_epoch`) asks *whose*
  credit this response settles;
* **liveness** (ownership *and* `!slot_cancelled[id]` *and*
  `rsp_epoch == epoch_now` *and* `!redirect_valid`) asks whether the instruction
  it carries still exists.

Folding the cancelled bit into the ownership test looks harmless and is not: a
slot cancelled by a redirect still holds its credit until its late response
arrives, so a response that fails the ownership test would be dropped with the
credit still held, and the slot would leak forever. The test caught exactly that
during bring-up (§8).

### 1.3 Redirect

```
redirect_valid, redirect_pc  ->  fetch_pc
```

`redirect_valid` is authoritative: it is the only thing that advances the epoch,
the only thing that cancels slots, and it is never refused, delayed or overridden
by a prediction. In one cycle it (a) advances the epoch, (b) marks every occupied
slot cancelled, (c) refuses the issue port, and (d) makes any response arriving
that cycle stale.

`redirect_pc` is published on `fetch_pc`, the PC the requester should restart
from. `fetch_pc` holds its value between redirects.

Note that a response arriving *on* the redirect cycle is stale even though its
epoch is the one in force right now. Comparing against the next epoch value
instead would be the single most damaging mistake available here: it is exactly
the cycle a mispredict test looks at, and the failure is a squashed instruction
reaching the decoder.

### 1.4 Delivered instruction event

```
out_valid | out_illegal | out_fault      (at most one, all gated by out_ready)
  out_pc, out_bits, out_len, out_cause
```

Exactly one of the three is high whenever any is, and the group holds its payload
until `out_ready`, so the whole event group obeys the "payload stable while valid
&& !ready" rule with one handshake rather than three.

* `out_valid` — a 32-bit instruction, `out_len == 4`, for `out_pc`. This is the
  only kind a decoder may consume.
* `out_illegal` — a rejected encoding. `out_len` and `out_bits` report what
  arrived; `out_cause` is `mosaic_pkg::EXC_ILLEGAL_INSN`.
* `out_fault` — an instruction access fault. `out_cause` is
  `mosaic_pkg::EXC_INSN_ACCESS`. Not an architectural trap: taking the trap is
  I-018's job, on the reported cause.

`out_pc` is the PC recorded in the request slot, never an address derived from
the response payload. That is the reporting contract I-010 depends on: an
instruction that straddled a fetch boundary is still attributed to the address it
was fetched for, with its original bits and length, so the decoder never
re-fetches anything.

### 1.5 The owned predictor

Fetch instantiates one `mosaic_predictor` per hart and forwards its ports:

* **query** — `pred_valid, pred_pc, pred_is_branch, pred_is_jump,
  pred_is_return` in; `pred_taken`, `pred_btb_hit`, `pred_btb_miss`,
  `pred_ras_valid`, `pred_ras_underflow`, `ras_overflow`, `ras_underflow` out
  unchanged, plus the derived `pred_next_valid`, `pred_next_pc`, `pred_squashed`.
* **update and RAS recovery** — `upd_*`, `ckpt_valid`, `flush` forwarded
  unchanged.

The class inputs are ports rather than sniffed from the encoding, because
`mosaic_decoder` (I-010) is the one decoder in this design and a second one here
would be a second opinion about which encodings are control transfers. This is
the integration cost I-021's report named ("fetch must know the class before it
can query, and I-009 owns that"), and it is paid here.

Every report the predictor makes is forwarded. Fetch acts on exactly one
(`pred_redirect`, in the next-PC rule) and swallows nothing, because a predictor
that is silently wrong is worse than one that never predicts.

### 1.6 Observability

`outstanding_count` and `cancel_pending` are a census of the slot table, not
running totals, so the counter the testbench watches cannot disagree with the
state that enforces the bound. `epoch_now` is the epoch in force. The 32-bit
counters are `issued_count`, `accept_count`, `drop_count`, `stale_drop_count`,
`squashed_drop_count`, `credit_drop_count`, `delivered_count`, `fault_count`,
`illegal_count`, `deny_count` and `cancel_count`.

`accept_count` counts live responses **including** faulting ones — a faulting
response consumed its request exactly once, and folding that into a separate
counter would make the credit identity need a fourth term. `delivered_count` is
the count of instructions actually handed to a decoder.

---

## 2. The credit rule, stated once

> Every issued request occupies exactly one request slot until exactly one event
> ends that occupancy — the response that accepts it, or the drop that discards
> it — and the slot is reusable only afterwards.

Two consequences are asserted on **every cycle of every phase**:

```
issued == accepted + credit_returned_by_drops + outstanding
outstanding <= MOSAIC_FETCH_OUTSTANDING
```

"Not returning the credit at all" leaks a slot and eventually stops fetch
forever. "Returning it twice" lets one response pay for two requests, which is
how a live response for a *different* request ends up matched against a slot
that has already been reused — and that failure looks exactly like "the wrong
instruction reached the decoder", which is the worst kind of bug to debug.

The rule is implemented as one statement in the RTL: `slot_busy[i] <= 0` fires
only on `rsp_retire`, which is `rsp_fire && rsp_slot_owns`. Delivering and
dropping are two *reasons* to retire a request; they are not two places where a
credit is returned. The `credit_drop_count` output counts the subset that came
back through a drop, which is what makes "the credit returned by drops equals
the number of drops" checkable as a number rather than as a rate.

### 2.1 The cancel policy, and what it costs

A redirect does **not** return the credits of the requests it retires. It marks
their slots cancelled, and each of those credits comes back when that request's
late response arrives and is dropped.

The cost is real and is not hidden: after a redirect, fetch can be at the
outstanding bound with every slot cancelled and cannot issue until the late
responses drain — up to `MOSAIC_FETCH_OUTSTANDING` response latencies of stall.
That is the card's own trade (bounded outstanding fetch, bandwidth sacrificed for
recovery clarity), and the card's fallback, one fetch at a time, is the right
answer if a future responder cannot promise what this one does.

The precondition that makes the policy safe is stated rather than assumed:
**every issued request produces exactly one response**. That is the `fetch_decode`
response rule; without it a cancelled request would hold its credit forever. The
alternative — return the credit at the redirect and refuse to return it again at
the drop — is the design this one deliberately is not, because then
"returned exactly once" would have to be maintained across two statements, and a
late response would arrive at a slot that had already been handed to somebody
else.

The benefit of the chosen policy is that a slot is never handed to a new request
while an old one is still answering for it, which is the only way an ABA can
survive the epoch counter (§3).

---

## 3. The epoch: why it exists, and why it is 7 bits

A request id alone cannot survive a redirect. A redirect retires every in-flight
request; *if* it also returned their credits, the slots would be immediately
reusable and a late response for slot 2 would be indistinguishable from the
response for whatever new request then held slot 2. That is the first
counterexample in `config/contracts/interfaces.json`: an ABA bug where the id
repeats and the request does not. It is constructed explicitly in the
`same-cycle` phase, in the `stale-recycled-slot` case.

So every request carries the epoch it was issued under and every response echoes
it back:

```
EPOCH_W = $clog2(MOSAIC_ROB_ENTRIES) + 1
```

which is the `epoch` identity field of the `fetch_decode` contract
(`expr: clog2(rob_entries)+1`, `min_bits: 2`). For p0 that is
`$clog2(64)+1 = 7` bits, a modulus of 128.

**The width is derived, not picked.** The epoch is a generation compared for
**equality only** — nothing in this design ever asks which of two epochs is
older — so it is an `identity` counter and the binding rule from
`config/contracts/counters.json` is *modulus strictly exceeds the number of
values that can be live at once* (an `order` counter would need twice the compare
distance, but forcing that rule onto a generation would make it wider than it can
ever need, which the contract's own comment says explicitly).

The live population of the fetch epoch is at most one epoch per outstanding
request plus the current epoch:

```
max_live = MOSAIC_FETCH_OUTSTANDING + 1 = 5      (p0)
```

so a modulus of 8 would satisfy the arithmetic, and 128 does with room to spare.
It is also strictly wider than the fetch side of the contract's own shared-id
rule: `memory_transaction_generation` is
`2*(fetch_outstanding+lq_entries+sq_entries+mshrs)+1 = 45` with at most 21 live
owners. `python3 tools/check_contracts.py` proves those inequalities against the
real geometry (`profile p0: contracts OK`), and the testbench independently
re-checks the shape of the elaborated widths it is given: a request id that
cannot name the last slot, a count that cannot say "full", or an epoch modulus
that does not exceed the live set are each a geometry failure, not a comment.

**The arithmetic bound is necessary but not the only defence, and the RTL does
not rely on it alone.** Each slot also carries a `cancelled` bit, set by the
redirect that retired it and cleared only when its late response is dropped. A
slot that has been through a redirect is therefore never deliverable again,
whatever the epoch counter happens to say — so wrapping the counter cannot turn
a squashed instruction into a delivered one. The epoch still does the one thing
the `cancelled` bit cannot: it separates a *recycled* slot's late response
(`stale`, no credit owed, because the slot has moved on) from a response for an
id that was never outstanding in this epoch (`squashed`, a protocol error). Both
are dropped; only the first is a credit event, and the two are counted apart so a
protocol error is visible rather than silent.

Residual assumption, stated rather than glossed: a false live classification
would require a request to survive `2^EPOCH_W` redirects while its slot is then
re-issued *and* the responder to deliver two responses for one slot and epoch.
The second of those is a protocol violation on the responder's side; the first is
bounded by the request latency, and the `cancelled` bit is what keeps it from
mattering.

---

## 4. What is advisory

`pred_next_pc` and `pred_squashed` are **hints**. Nothing else in this module's
outputs is.

* `pred_next_pc` is the predicted next PC: `pred_target` when there is a
  prediction and no reported miss, and `pred_pc + 4` otherwise.
* On `pred_btb_miss` the fall-through is written out **explicitly** rather than
  by trusting that `pred_target` happens to hold the same value. On a miss the
  predictor returns `pc + 4`, which is bit-identical to a real fall-through
  prediction; the miss bit is the only thing that says which one it is, and it is
  a report fetch cannot reconstruct on its own.
* `pred_squashed` pulses when a redirect in the same cycle voids the hint, so a
  hint can never be applied and then contradicted one cycle later.
* `redirect_valid` is authoritative and has no hint attached.

A wrong hint costs a squash; the resolved target comes from execution. The
predictor's `pred_btb_miss` and `pred_ras_underflow` reports are forwarded rather
than acted on, because a unit that swallowed them would leave the core unable to
tell "I have no target" from "my target is the fall-through address".

---

## 5. 16-bit instructions: reported, not decoded

A 16-bit (compressed) instruction is **rejected as illegal**. The C extension is
I-041 and is p1; this build does not implement C, and fetch recognises the
encoding in order to report it, not in order to execute it.

A 32-bit instruction is the 4-byte form whose low two bits are `11`. Anything
else is reported illegal, with the length and bits that arrived preserved on the
report. Two cases are tested, because they fail differently:

1. `rsp_len == 2` with `rsp_data[1:0] != 2'b11` — a genuine compressed
   instruction. The length alone would catch this one.
2. `rsp_len == 4` with `rsp_data[1:0] != 2'b11` — four bytes returned for an
   instruction whose *encoding* is still 16 bits, which is what a real fetch
   produces when a compressed instruction sits in a four-byte slot. A check on
   the word count alone would deliver this one, so fetch tests the encoding and
   not the length.

Both cost exactly one credit, returned once, exactly like an accepted
instruction.

---

## 6. What the testbench asserts

The driver compares **every** output against an independent C++ shadow on every
cycle: the combinational ones before the edge, the registered ones after it. The
shadow is written from the contract prose, keeps its own slot table, epoch,
credit ledger and predictor model, and reads its geometry from the elaborated
DUT, so a profile change moves hardware and model together.

Two invariants are checked on every cycle of every phase, in addition to the
comparison:

| Invariant | Catches |
|---|---|
| `issued == accepted + credit_returned_by_drops + outstanding` | a lost release **and** a double release, including every same-cycle combination |
| `drop == stale + squashed` | an unclassified response |
| `credit_returned_by_drops <= stale` | credit returned for a response no slot owned |
| `outstanding <= MOSAIC_FETCH_OUTSTANDING` | the bound |
| `cancel_pending <= outstanding` | a cancelled slot with nothing in it |

### 6.1 The eleven phases

| Phase | What it discriminates |
|---|---|
| `reset-state` | the documented cold state |
| `redirect-late-response` | **the card's named case.** Issue 3, redirect (with an issue offered on the same cycle), answer the pre-redirect requests **out of order**, then issue and answer two of the new epoch. Asserts the drop count, the stale count, the squashed count, the credit-drop count and the delivered PC of each accepted instruction — counts, not "a drop happened". |
| `late-but-live` | 16 idle cycles between issue and response: latency is not a timeout, and only a *stale epoch* is dropped |
| `credit-exactly-once` | 120 cycles of randomised traffic with three redirects; ends with `outstanding == 0`, `issued == accept + credit_drop`, `credit_drop == drop` |
| `bound-enforced` | the issue port hammered with a responder that never answers; asserts the bound, that the table really did fill, that it really did refuse, and that every refusal was counted |
| `same-cycle` | **all 40 combinations**: {no response, live, stale-own-slot, stale-recycled-slot, squashed} × {redirect on/off} × {issue on/off} × {out_ready on/off}. Each case asserts the conservation law across the cycle and the specific consequence of that combination |
| `epoch-wrap` | 260 redirects with 2 requests kept outstanding across each, so the counter wraps twice; responses stamped before the wrap are still rejected afterwards |
| `illegal-16bit` | both compressed forms of §5, with the reported length and cause |
| `fault-path` | cause 1, credit returned once, the machine still issues afterwards |
| `predictor-advisory` | cold query reports a miss and falls through to `pc+4`; a trained jump supplies its target; a redirect squashes the hint; a return on an empty stack is reported |
| `random` | 4,000 cycles: random issues, out-of-order responses to retired requests, random redirects, random predictor queries and updates, random stalls. Fails if the campaign never dropped, never accepted, never faulted, never rejected an encoding, or never reached the bound — a soak that did nothing would otherwise pass vacuously |

---

## 7. Mutation testing

`MOSAIC_FETCH_MUTANT_<n>` blocks are **off in the shipping build**. For each one
three separate facts are recorded, because a `-D` whose `` `ifdef `` body was
never written compiles to the shipping build and "passes" vacuously — that has
happened twice in this project:

1. the `` `ifdef `` block **exists** in the RTL (grep count),
2. the mutant binary **differs** from the shipping binary (`cmp`),
3. the run **exits 1** with a `RESULT FAIL` line, **and the delta against the
   base is non-zero**.

The driver reports how many comparisons it completed before failing, so the
delta is a number and not just a verdict: the base completes 235,600 comparisons
over 6,098 cycles and every mutant aborts early at a named field.

Build command per mutant (the runner's flags reproduced, plus `-D`):

```sh
verilator --cc --exe --build -j 0 -O2 --x-assign unique --x-initial unique \
  -D<DEFINE> --top-module mosaic_fetch_tb -Mdir /tmp/fetch_mut/<DEFINE>/obj_dir \
  -Ibuild/p0/rtl -Ibuild/p0/sim -Irtl/core -Irtl/common \
  -CFLAGS -Isim/common -CFLAGS "-O2 -std=c++17 -Wall" \
  -o /tmp/fetch_mut/<DEFINE>/fetch.redirect_late_response \
  rtl/core/mosaic_fetch.sv sim/tb/mosaic_fetch_tb.sv \
  sim/unit/tb_fetch.cpp sim/common/sim_common.cpp
/tmp/fetch_mut/<DEFINE>/fetch.redirect_late_response --case fetch.redirect_late_response \
  --seed 1 --max-cycles 200000
```

### 7.1 Mutant table

| # | Define | Injected defect | Caught by | Comparisons before failing (base: 235,600) | Observed failure |
|---|---|---|---|---|---|
| 1 | `MOSAIC_FETCH_MUTANT_ACCEPT_STALE` | the response is accepted on the strength of the slot alone — cancelled bit, epoch comparison and redirect guard all removed | `redirect-late-response` | 223 (cycle 18) | `out_valid(registered): expected 0, got 1` — a pre-redirect response delivered |
| 2 | `MOSAIC_FETCH_MUTANT_DOUBLE_CREDIT` | a stale drop also frees a slot that does not own the response: one drop, two credits | `redirect-late-response` | 242 (cycle 18) | `req_id: expected 0x1, got 0x0` — the slot census no longer matches the ledger |
| 3 | `MOSAIC_FETCH_MUTANT_IGNORE_BOUND` | the bound is not enforced: every slot reads as free, so the table keeps accepting past `MOSAIC_FETCH_OUTSTANDING` and clobbers slot 0 | `credit-exactly-once` | 1,521 (cycle 58) | `req_ready: expected 0, got 1` |
| 4 | `MOSAIC_FETCH_MUTANT_NO_EPOCH_ADVANCE` | a redirect does not advance the epoch | `redirect-late-response` | 203 (cycle 17) | `req_epoch: expected 0x1, got 0x0` |
| 5 | `MOSAIC_FETCH_MUTANT_DELIVER_16BIT` | the compressed-form check is removed entirely, so a 16-bit instruction is handed to the decoder as a 32-bit one | `illegal-16bit` | 74,783 (cycle 2,066) | `out_valid(registered): expected 0, got 1` |
| 6 | `MOSAIC_FETCH_MUTANT_FAULT_AS_INSN` | a faulting response is delivered as an ordinary instruction, with no fault event and no cause | `fault-path` | 74,943 (cycle 2,074) | `out_valid(registered): expected 0, got 1` |
| 7 | `MOSAIC_FETCH_MUTANT_CANCEL_RELEASES_CREDIT` | the redirect returns the credit for every slot it cancels instead of the drop doing it | `redirect-late-response` | 202 (cycle 17) | `req_id: expected 0x2, got 0x0` |

Per-mutant block presence, build, exit status and binary difference, from one
run:

```
BASE: exit=0 verdict=PASS comparisons=235600

MUTANT: -DMOSAIC_FETCH_MUTANT_ACCEPT_STALE
  ifdef block: present (1 occurrence(s))
  build: ok
  run exit=1 verdict=FAIL
  binary differs from shipping build: YES
  delta vs base: aborted after 223 comparisons / 18 cycles (base: 235600 / 6098)
  failed at: cycle 17 / out_valid(registered)
MUTANT: -DMOSAIC_FETCH_MUTANT_DOUBLE_CREDIT
  ifdef block: present (1 occurrence(s))
  build: ok
  run exit=1 verdict=FAIL
  binary differs from shipping build: YES
  delta vs base: aborted after 242 comparisons / 18 cycles (base: 235600 / 6098)
  failed at: cycle 18 / req_id
MUTANT: -DMOSAIC_FETCH_MUTANT_IGNORE_BOUND
  ifdef block: present (1 occurrence(s))
  build: ok
  run exit=1 verdict=FAIL
  binary differs from shipping build: YES
  delta vs base: aborted after 1521 comparisons / 58 cycles (base: 235600 / 6098)
  failed at: cycle 58 / req_ready
MUTANT: -DMOSAIC_FETCH_MUTANT_NO_EPOCH_ADVANCE
  ifdef block: present (1 occurrence(s))
  build: ok
  run exit=1 verdict=FAIL
  binary differs from shipping build: YES
  delta vs base: aborted after 203 comparisons / 17 cycles (base: 235600 / 6098)
  failed at: cycle 17 / req_epoch
MUTANT: -DMOSAIC_FETCH_MUTANT_DELIVER_16BIT
  ifdef block: present (1 occurrence(s))
  build: ok
  run exit=1 verdict=FAIL
  binary differs from shipping build: YES
  delta vs base: aborted after 74783 comparisons / 2066 cycles (base: 235600 / 6098)
  failed at: cycle 2065 / out_valid(registered)
MUTANT: -DMOSAIC_FETCH_MUTANT_FAULT_AS_INSN
  ifdef block: present (1 occurrence(s))
  build: ok
  run exit=1 verdict=FAIL
  binary differs from shipping build: YES
  delta vs base: aborted after 74943 comparisons / 2074 cycles (base: 235600 / 6098)
  failed at: cycle 2073 / out_valid(registered)
MUTANT: -DMOSAIC_FETCH_MUTANT_CANCEL_RELEASES_CREDIT
  ifdef block: present (1 occurrence(s))
  build: ok
  run exit=1 verdict=FAIL
  binary differs from shipping build: YES
  delta vs base: aborted after 202 comparisons / 17 cycles (base: 235600 / 6098)
  failed at: cycle 17 / req_id
```

### 7.2 Two things this table is honest about

**Some mutants are caught by a comparison other than the one they were written
for, and that is a fact about phase order, not a defect.** Mutants 2, 4 and 7
are caught in `redirect-late-response` on `req_id` / `req_epoch` rather than by
the credit invariant being checked at the drop: a slot that is freed one moment
too early changes which slot the allocator picks on the *next* cycle, and the
shadow's `req_id` is what notices. The credit identity still breaks in every one
of them — the `same-cycle` phase would say so in words — but the driver stops at
the first failure, and the earliest observable is the allocator.

**Mutant 4 is caught by observation rather than by misdelivery, deliberately.**
Because the per-slot `cancelled` bit refuses a retired request independently of
the epoch, removing the epoch increment does not make a squashed instruction
deliverable — it makes two epochs share a value, which is a contract violation
that shows up as `req_epoch` disagreeing with the shadow. That is the intended
consequence of the design in §3 (two independent lines of defence), and it is
recorded here rather than papered over by claiming the mutant would have been
caught by a misdelivery.

---

## 8. Two defects the testbench found, and one a mutant found

* **The accept path did not free its slot.** The first implementation returned
  credit only from the drop path, so an accepted response consumed its request
  and never released the slot. The credit identity caught it at cycle 23 of the
  named phase (`issued 5 != accepted 1 + credit from drops 3 + outstanding 2`).
  The fix was to make `rsp_retire` the single statement that frees a slot.
* **Ownership was folded into liveness.** With `!slot_cancelled` inside the
  ownership test, a cancelled slot's late response found nothing to own it, was
  dropped, and kept its credit — a permanent leak that only shows up when the
  redirect happened at least one cycle before the response, which is exactly what
  the named case does.
* **The shadow's RAS was wrong, not the RTL.** In the random phase a return
  prediction came back one entry off. Instrumenting both sides showed the shadow
  was writing `ras_ckpt` from the *current* pointer before flushing, so a
  same-cycle flush restored the pointer it was supposed to undo. The RTL restores
  the *registered* checkpoint; the shadow now captures it before the update. The
  lesson the project already had applies here and is why the mismatch line prints
  the entire stimulus: the failure looked like a predictor bug and was not one.

---

## 9. Command output, verbatim

### 9.1 The named case

```
$ python3 tools/run_unit.py --case fetch.redirect_late_response
PASS fetch.redirect_late_response task=I-009
$ echo $?
0
```

```
$ cat results/unit/fetch.redirect_late_response/run.log
$ /Users/flare/MosaicRV/build/p0/unit/fetch.redirect_late_response/fetch.redirect_late_response --case fetch.redirect_late_response --out /Users/flare/MosaicRV/results/unit/fetch.redirect_late_response --seed 1 --max-cycles 200000
RESULT PASS fetch.redirect_late_response fetch contract holds: 235600 comparisons over 6098 cycles, seed 1
```

Eight seeds, all PASS (the registry pins seed 1; the rest were run to show the
random phase is not seed-locked):

```
$ for s in 1 2 3 4 5 6 7 8; do python3 tools/run_unit.py --case fetch.redirect_late_response --seed $s; done
seed 1: PASS fetch.redirect_late_response task=I-009
seed 2: PASS fetch.redirect_late_response task=I-009
seed 3: PASS fetch.redirect_late_response task=I-009
seed 4: PASS fetch.redirect_late_response task=I-009
seed 5: PASS fetch.redirect_late_response task=I-009
seed 6: PASS fetch.redirect_late_response task=I-009
seed 7: PASS fetch.redirect_late_response task=I-009
seed 8: PASS fetch.redirect_late_response task=I-009
```

### 9.2 Lint

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_fetch \
    -Ibuild/p0/rtl -Irtl/common -Irtl/core rtl/core/mosaic_fetch.sv rtl/core/mosaic_pkg.sv
- V e r i l a t i o n   R e p o r t: Verilator 5.052 2026-09-05 rev vUNKNOWN-built20260905
- Verilator: Built from 0.113 MB sources in 5 modules, into 0.114 MB in 3 C++ files needing 0.000 MB
- Verilator: Walltime 0.019 s (elab=0.003, cvt=0.009, bld=0.000); cpu 0.019 s on 1 threads; allocated 13.312 MB
$ echo $?
0
```

```
$ python3 tools/lint_rtl.py | tail -3
ok   rtl/core/mosaic_fetch.sv: clean as mosaic_fetch
lint: 16 source file(s) clean
```

```
$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(ls rtl/*/*.sv)
... (one pre-existing [STYLE-2] port-suffix *warning* in rtl/common/mosaic_skid_buffer.sv; no errors anywhere)
[RegisterHasNoReset] PASS
[OnlyAssignedOnReset] PASS
[XilinxDoNotCareValues] PASS
[NoLatchesOnDesign] PASS
[AlwaysCombNonBlocking] PASS
[EnforcePortPrefix] PASS
$ echo $?
0
```

The C++ driver is held to the stricter standard the `lint-cpp` target uses
(`-Wall -Wextra -Wshadow`, no runtime headers in the way) and is clean:

```
$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -Ibuild/p0/sim -Isim/common \
    -I/opt/homebrew/Cellar/verilator/5.052/share/verilator/include \
    -Ibuild/p0/unit/fetch.redirect_late_response/obj_dir sim/unit/tb_fetch.cpp 2>&1 | grep 'tb_fetch.cpp:[0-9]*:'
(no diagnostics)
```

### 9.3 Contracts

```
$ python3 tools/check_contracts.py
profile p0: contracts OK
$ echo $?
0
```

---

## 10. Limits, and what this package does not do

* **No PC sequencing.** Fetch does not generate the PC stream; it publishes
  `fetch_pc` after a redirect and an advisory `pred_next_pc`, and the requester
  drives `req_pc`. Sequencing belongs to whoever owns the front end's flow.
* **No C extension.** Reported illegal, by design (§5).
* **No architectural trap.** `out_fault` reports cause 1 and stops; taking the
  trap is I-018's job.
* **The cancel policy depends on the response rule** — every issued request
  produces exactly one response (§2.1). The fallback if that cannot be promised
  is the card's: one fetch at a time.
* **`mosaic_pkg` is `include`d** rather than imported-and-assumed-present,
  because this work package's unit build compiles `mosaic_fetch.sv` on its own
  and an unresolved package reference is an elaboration error there. The package
  has its own include guard, so the include is a no-op in builds that also pass
  the file on the command line (`tools/lint_rtl.py`, the bringup case).
* **The predictor's own behaviour is not re-verified here.** The fetch driver
  models enough of the documented predictor contract to make `pred_next_pc`
  non-trivial and compares it every cycle; the predictor's tables are I-021's
  tested contract (`CASE=predictor.btb_aliasing`).