# I-031 — resource-ownership change FSM (`reconfigure.drain_and_generation`)

Work package **I-031** (docs/implementation-plan.md §6): implement the
resource-ownership change FSM — stop-admit, drain, ack, publish — with owner
generations and idempotent control messages, so that a new owner is published
only after every outstanding uop, result and credit of the old owner is settled.

The frozen interface this implements is `reconfigure` in
`config/contracts/interfaces.json`:

> Explicit state machine: STOP_ADMIT -> DRAIN -> ACK -> PUBLISH -> RESUME.
> Cancel: an aborted reconfiguration returns the resource to its original owner
> with its original generation; **nothing may be reused before the barrier is
> acknowledged**. Response: each participant acknowledges the barrier; ids,
> credits and state are only recycled after every acknowledgement.

Status: **delivered** — the registered case passes from a deleted build
directory, six mutants each rebuild from an empty directory, differ from the
shipping binary, exit 1 and name the check they break, and every case the
package touches that already passed still passes.

## 1. What was built

| file | state | what it is |
|---|---|---|
| `rtl/core/mosaic_owner_fsm.sv` | new | the FSM: three outstanding-work counters, the STOP/DRAIN/ACK/PUBLISH/RESUME sequence, the wrapping owner generation, the idempotent control message, the drain watchdog, six mutants |
| `sim/tb/mosaic_owner_fsm_tb.sv` | new | sim wrapper: every port passed straight through, no clock/reset generation of its own (the C++ side owns those), geometry read out of the elaborated instance |
| `sim/unit/tb_owner_fsm.cpp` | new | the driver and the independent shadow: per-cycle combinational and registered comparison, the directed phases, the soak, the semantic property checks |
| `tools/run_owner_controls.py` | new | the mutant campaign: rebuild from an empty directory per mutant, `-D` recorded, binary compared, first failure required |
| `results/reports/I-031-ownership.md` | new | this report |

Nothing else was edited. `mosaic_core.sv` was **not** touched. The change is
purely additive: no existing case lists any of the new files, so no existing
case's elaboration or behaviour can change.

## 2. The contract the case is written against

### 2.1 The state sequence

```
        ctrl_ok            drained ─┐
IDLE ─────────────► STOP ─────► DRAIN ─────► ACK ──ack_valid──► PUBLISH ──► IDLE
  ▲                              │                                (owner_gen+1,
  │                        drain_expired                           stop_admit=0)
  └───────────── abort ──────────┘
```

* **STOP_ADMIT** — `o_stop_admit` is high in STOP, DRAIN and ACK. The accept
  cycle itself is still IDLE (`o_stop_admit` low), so an admit presented in the
  cycle the message arrives is the last legal one and is counted; from the next
  cycle admits are stopped. This is *the* hazard the STOP state exists for: a
  drain decision made without a full cycle of no-admit separation can conclude
  "empty" while an admit is in flight.
* **DRAIN** — waits for **all three** counters (`uop`, `result`, `credit`) to be
  zero. A separate count per class because they are separate obligations: a uop
  can have left the issue queue and still be executing, a result can be
  executing and not yet collected, and a credit can be held by a writeback path
  the queue has never seen. The card's named fail mode — "the issue queue is
  empty, so the pool is drained" — is exactly the line the
  `MOSAIC_OWNER_MUTANT_PUBLISH_EARLY` mutant substitutes, and the case catches
  it.
  A watchdog bounds the drain at `DRAIN_LIMIT = 4 * MOSAIC_ROB_ENTRIES` (= 256
  at p0) cycles. If it expires the reconfiguration **aborts**: the FSM returns
  to IDLE, `o_owner_gen` is unchanged, `o_publish` never pulses and
  `o_drain_stall` latches.
* **ACK** — `o_ack_req` rises and the FSM waits for `ack_valid`. Only then may
  ids, credits and state of the old owner be reused. An `ack_valid` in any other
  state is `o_ack_unexpected`, counted and inert.
* **PUBLISH** — one cycle. `o_publish` is high, `o_owner_gen` is already the new
  generation (`o_old_gen + 1`) and the publish tally has advanced; `o_stop_admit`
  is low (RESUME). `o_owner_gen` has no other writer, so the reachability of
  PUBLISH through DRAIN and ACK *is* the guarantee.

### 2.2 Owner generations

`o_owner_gen` is `GEN_W = clog2(rob_entries) + 1` = 7 bits wide, the same idiom
`mosaic_iq` and `mosaic_lease_alloc` use, and the width the frozen contract
requires. It advances by exactly one per publish and **wraps at 128**; the soak
drives it through a full wrap so the modulus is exercised rather than assumed.
The drain gate makes the width generous: at the instant of publish there is no
old id outstanding at all.

### 2.3 Idempotent control messages

A control message is `ctrl_valid` + `ctrl_seq`; `ctrl_seq` **is** its identity.
The FSM remembers the last accepted sequence (`last_seq`, `seq_seen`):

* a message whose sequence repeats the last accepted one is `ctrl_dup` and does
  **nothing**, in any state, however long it is held — one message is one
  reconfiguration;
* a message with a *new* sequence while busy is `ctrl_reject` (counted, inert);
  `ctrl_ready` is the "may I send now" line — a reconfiguration can never be
  initiated from inside another one;
* the very first message is never a duplicate, so a controller may start at
  sequence 0.

Documented limit: two reconfigurations that reuse the same sequence with no
other sequence accepted in between cannot be told apart, by construction. The
sequence field is 8 bits; the contract only requires consecutive
reconfigurations to differ.

### 2.4 The drain counts

`CNT_W = clog2(rob_entries * max_uops_per_macro + 1)` = 10 bits (the contract's
`outstanding` field). Each class has one count; an admit raises it, a settle
lowers it, and a *same-cycle* admit and settle cancel (the settle consumes the
admit). A settle with nothing available is `o_settle_unmatched_count`: counted,
and it moves no count — so a double-settle cannot borrow against another class.
Counts **saturate** at the architectural bound rather than wrapping; a legal
stimulus never reaches it, and an illegal one fails the shadow comparison
instead of aliasing to a small number. An admit that arrives after the stop is
**still counted as outstanding and flagged** (`o_admit_after_stop_count`) rather
than refused, so no work the fabric has begun can be lost behind the broker's
back; refusing it is the `MOSAIC_OWNER_MUTANT_DROP_ADMIT_AFTER_STOP` mutant.

## 3. The progress assumption and the measured bound

**Assumption (written down, and violated loudly rather than silently):** every
outstanding item is matched by exactly one settle, and every settle arrives
within `DRAIN_LIMIT` cycles of the admit. Under it, a reconfiguration accepted
with `N` items outstanding publishes within

```
bound = 1 (accept takes effect) + 1 (STOP -> DRAIN)
      + N (settles, at most one per cycle) + 1 (the FSM observes the last zero)
      + 1 (ack -> publish)
      = N + 4 cycles.
```

The case derives `N` and the latency from **the run's own numbers** and checks
the inequality; the watchdog is what turns a violation of the assumption into a
named abort instead of an infinite wait.

Measured in the shipping run (`happy` phase, N = 3 uops + 2 results + 2 credits):

```
[drain] outstanding=7 latency=11 bound=11 max-drain-cycles=7
[stall] abort at drain-cycle 256 (limit 256), generation held at 0, credit retained
[soak]  cycles=4000 publishes=232 dup=81 reject=149 admit-after-stop=138 unmatched=169
```

The stall phase drives exactly the assumption violation — one credit that is
never settled — and requires the abort at the declared limit (measured:
drain-cycle 256), no publish, no generation change, and the outstanding credit
**retained** (the old owner keeps its work). It then settles the credit and
reconfigures again, and that one publishes. So "it can fail for ever" is neither
accepted as passing nor a hang: it is a bounded, named, recoverable abort.

## 4. What the case checks

Per cycle, in every phase, against an independent C++ shadow written from the
contract (not from the RTL):

* **message** — `ctrl_ready/ok/dup/reject`;
* **barrier** — `ack_req`, `ack_ok`, `ack_unexpected`;
* **state** — `o_state`, `o_stop_admit`, `o_busy`, `o_publish`;
* **generation** — `o_owner_gen`, `o_old_gen`, `o_new_gen` (mod 128);
* **counters** — the three outstanding counts and the drain cycle count,
  compared against the shadow's **stacks of outstanding items** (push/pop), not
  against a second counter, so a lost or double charge shows up on the cycle it
  happens;
* **accounting** — accepts, duplicates, rejects, acks, unexpected acks,
  admits-after-stop, unmatched settles, publishes, aborts;
* **stall** — `o_drain_stall`.

Plus four properties the shadow cannot state:

* **drain** — the DUT never leaves DRAIN for ACK or PUBLISH while any class is
  outstanding (checked from the pre-edge counters and the post-edge state);
* **barrier** — the DUT never enters PUBLISH without a pre-edge `ack_ok`;
* **idempotent** — a message whose sequence repeats the last accepted one is
  never accepted, however long it is held;
* **accounting** — every accepted message is either published, aborted, or the
  one currently in flight (at most one at a time, and only in STOP/DRAIN/ACK).

Phases: **geometry** (readback and cold ledger), **idle** (per-class admits and
settles, unmatched settle without underflow), **happy** (the mixed population
held back until all three classes drain, then publish generation 1, with the
message held past the publish), **barrier** (ack withheld, then repeated acks
inert), **admit-stop** (an admit during the stop is counted and flagged, and
holds the publish back), **stall** (the watchdog abort and recovery),
**reset** (a reset taken from STOP, DRAIN, ACK and PUBLISH yields the cold
ledger and never advances the generation), and **soak** (4000 cycles of random
control messages, acks, admits and settles, shadow-compared every cycle).

Result: `RESULT PASS` — 9 phase checks, 0 failures, 4354 shadow-exact cycle
comparisons over 4406 cycles; passes for seeds 1, 2, 3, 7 and 11.

## 5. Mutants

`python3 tools/run_owner_controls.py` rebuilds each configuration from an
**empty** directory, records the `-D` on the build command line
(`build/p0/owner_controls/<define>/build_command.txt`), requires the binary to
differ from the shipping one, and requires exit 1 with the named check as the
first failure. Shipping baseline sha256:
`e1d9d3374d71a6db1651e1d01a7443088de5ceb3b5ddbe109341b60f75219b6d`.

| mutant | exit | sha256 | first failing check | injects |
|---|---|---|---|---|
| `MOSAIC_OWNER_MUTANT_PUBLISH_EARLY` | 1 | `00f22d534619cd5d…` | `drain: left DRAIN for ACK at happy: cycle 37 with uop=0 res=2 crd=2 still outstanding` | the drain is judged complete when only the uop class is empty — the card's "issue queue empty means drained" fail mode |
| `MOSAIC_OWNER_MUTANT_NO_ACK` | 1 | `83fccc2758264aec…` | `barrier: entered PUBLISH at happy: cycle 41 without an acknowledged barrier (pre-edge state DRAIN, ack_ok=0)` | DRAIN goes straight to PUBLISH, recycling ids before the barrier |
| `MOSAIC_OWNER_MUTANT_LOST_CREDIT` | 1 | `a066581e7cff1232…` | `counters: o_cnt_crd is 2, expected 1 at happy: cycle 39` | a returned credit never decrements the credit count |
| `MOSAIC_OWNER_MUTANT_DOUBLE_CREDIT` | 1 | `85ef3f1ee36df074…` | `counters: o_cnt_crd is 2, expected 1 at happy: cycle 30` | one admitted credit is charged twice |
| `MOSAIC_OWNER_MUTANT_REPEAT_CTRL` | 1 | `90ab258b960967fd…` | `idempotent: ctrl_ok rose for sequence 5 at happy: cycle 44; that sequence is the last accepted one` | the message sequence is not compared, so a held message starts a second reconfiguration |
| `MOSAIC_OWNER_MUTANT_DROP_ADMIT_AFTER_STOP` | 1 | `3b6214e3939a191a…` | `counters: o_cnt_res is 0, expected 1 at admit-stop: cycle 78` | an admit after the stop is refused instead of counted, so the broker loses work it does not know about |

All six: **OK**. The three the card names (publish before the drain, lost or
double-counted credit, non-idempotent repeated message) are the first three
rows and the fifth; `NO_ACK` and `DROP_ADMIT_AFTER_STOP` are the barrier and the
lost-work controls.

## 6. Regression and gates

Run after the change (all PASS, from the same tree):

| case | verdict |
|---|---|
| `reconfigure.drain_and_generation` | PASS (baseline rebuild from an empty directory) |
| `fabric.fixed_two_cluster` | PASS |
| `core.mem_program` | PASS |
| `core.corpus_sweep` | PASS |
| `fence.code_and_data_order` | PASS |
| `mmio.exactly_once` | PASS |
| `amo.linearization` | PASS |
| `store.wrong_path_visibility` | PASS |
| `lsu.byte_forwarding` | PASS |
| `lsu.size_fault_boundaries` | PASS |

Gates:

* `python3 tools/lint_rtl.py --profile p0` — `rtl/core/mosaic_owner_fsm.sv:
  clean as mosaic_owner_fsm`. **The tree as a whole is not green right now, and
  not because of this package**: three files fail, all owned by another lane
  working concurrently — `rtl/core/mosaic_reservation.sv` (an UNUSEDSIGNAL
  warning in a new file), `rtl/core/mosaic_lsu_endpoint.sv` and
  `rtl/core/mosaic_core.sv` (a half-landed `mosaic_pkg` reference through an
  include). Re-run the gate once that lane lands.
* `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name
  '*.sv' | sort)` — exit 0. The new module produces the same STYLE-2
  (`_i`/`_o` suffix) and STYLE-13 (named `always_comb`) advisories that every
  other module in the repository produces; no errors.
* `python3 tools/check_records.py` — green (45 delivered packages, 55 registered
  cases).
* The new C++ driver is clean under `c++ -std=c++17 -fsyntax-only -Wall -Wextra
  -Wshadow` (no warnings attributable to `sim/unit/tb_owner_fsm.cpp`).
* `tests/unit/registry.json` and `config/status/implementation_status.json`
  were **not** edited (they are the integration lead's).

## 7. Not covered (honest limits)

* **Not instantiated anywhere.** `mosaic_owner_fsm.sv` is a standalone module
  with its own registered case; no fabric or core module drives it yet. The case
  drives it directly, so what is proven is the FSM's contract, not its wiring
  into a real reconfiguration. Wiring it to the real issue queues / result
  credits / network credits, and the ownership FSM in the fabric, belong to
  V-034 / I-090.
* **p0 is single-hart.** There is no second hart and no coherence agent, so
  "another agent invalidates the reservation / writes the granule" does not
  exist here. The participants are modelled by the test's `ack` and by the two
  event classes; the only "other master" is the stimulus itself. Multi-hart
  ownership and cross-hart invalidation are p3 (V-064+).
* **The abort is a watchdog, not a hang detector.** A credit that settles
  *after* the abort leaves the counter nonzero, so the *next* reconfiguration
  will also stall until it is settled. The case covers recovery after the
  settle; it does not claim the FSM can distinguish "slow" from "lost" — it only
  refuses to complete a drain it cannot justify.
* **Generation aliasing bound.** 2^7 = 128 generations. An ack or settle
  produced for a reconfiguration more than 128 generations in the past could in
  principle alias the current one. The drain gate makes this unreachable for
  honours-correct participants, and the soak wraps the generation once, but no
  test drives a message 128 generations stale.
* **No formal properties.** The case is directed plus a random shadow
  comparison; there is no SVA and no bounded model check of the FSM.
* **Saturation is argued, not driven.** The counters saturate at the
  architectural bound (512 at p0); no stimulus reaches it, so the saturation
  branch is justified by the width derivation rather than exercised.
* **The shadow is the only oracle, and it is by the same author.** It was
  written from the contract in the RTL header and uses an independent
  representation (stacks vs counters, a different arbitration of `Eval`/`Apply`),
  but it is not an independently produced reference model.
* **Stop-admit is a promise, checked by counting.** The FSM cannot prevent a
  participant from admitting during the stop; it counts the violation and holds
  the publish back. Enforcement on the participant side is V-034's.
