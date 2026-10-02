# I-090 / V-034 — the fabric integration: bank-aware allocation, bypass and steering, wired and measured — **PASS**

Work package I-090 (`docs/implementation-plan.md` §3.1 integration obligations; the
`fabric.integrated`/V-034 card). Case `CASE=fabric.integrated` (top
`mosaic_core_tb`, driver `sim/unit/tb_core_fabric.cpp`).

All commands are from `/Users/flare/MosaicRV`, Verilator 5.052 (macOS arm64),
measured at git `916584f` plus the working tree (the fabric edits are
uncommitted as of this writing).

> **The card.** *Action:* wire the five verified fabric modules into the live core
> one at a time, keeping the tree green; run one ELF through a fixed and a dynamic
> configuration on equal resources and measure the difference; add three controls.
> *Pass:* the two configurations retire the same architecture (retire stream and
> signatures), the dynamic one demonstrably changed something, the invariants
> still hold, and the comparison is falsifiable. *Fail:* a "dynamic" mode that
> never steers, or a dynamic mode that changes what the program computes.

## 0. Status, in one paragraph

**Three of the five modules are wired in**: the I-032 bank-aware allocation (a
preference inside rename, driven by the routing decision), the I-027 cluster
bypass (its early value-visible wakeup reaches the issue queue as a second
wakeup port), and the I-029 steering policy (replacing dispatch's alternating
`aff_toggle`, with the fixed baseline selectable by the same one-bit input).
**Two are not**, and this report says why in *Not covered*: the I-030
bounded-service arbiter and the I-031 ownership FSM. All three wired steps are
enabled by one runtime input, `fab_dyn_i`, which is **low by default** — so every
other case in the tree, and ACT4, is bit-for-bit the machine it was, and only
`fabric.integrated` turns the fabric on. The measured fixed-versus-dynamic
comparison on `p02_branch.i0.elf` is: identical architecture (71 123 retire
events, event for event, and the same four signature words), a changed issue
distribution (c0/c1 integer-ALU 214/17 624 → 8 498/9 340) and **62 more cycles**
in the dynamic configuration. The dynamic policy is *slower* on this program. That
is the finding, and it is reported rather than hidden (see §4).

## 1. Files

| File | State |
|---|---|
| `rtl/core/mosaic_dispatch.sv` | **changed**: the fabric strategy input; the I-029 steering instantiation; the class/room/occupancy/capability inputs; the routing decision (the router's grant *is* the path decision); the fabric observation outputs; the macro age field; `MOSAIC_FAB_MUTANT_DYN_SWAP_SRC` |
| `rtl/core/mosaic_core.sv` | **changed**: `fab_dyn_i` input; the three per-step enables (`MOSAIC_FAB_STEP_NO_*`, measurement instrumentation) and `MOSAIC_FAB_MUTANT_NO_DELTA`; the I-032 preference driven from the routing decision; the fabric observation outputs |
| `rtl/core/mosaic_cluster.sv` | **changed**: the I-027 bypass instantiation and its tap; the issue queue's second wakeup port wired to the bypass source; fabric observation |
| `rtl/core/mosaic_iq.sv` | **changed**: a second, earlier value-visible wakeup port (`wu2_*`, defaulted inactive) with the durable port's own matching rules; `o_wu2_matched` |
| `rtl/core/mosaic_cluster_bypass.sv` | **changed**: four observation outputs (`bp_src_*`) exposing the tap the core takes as a wakeup |
| `rtl/core/mosaic_steering.sv` | **unchanged** (its existing `MOSAIC_STEER_MUTANT_IGNORE_CAPABILITY` is one of this case's controls) |
| `rtl/core/mosaic_arbiter.sv`, `rtl/core/mosaic_owner_fsm.sv` | **unchanged** — not wired; see *Not covered* |
| `sim/tb/mosaic_core_tb.sv` | **changed**: `fab_dyn_i` input and the fabric observation outputs |
| `sim/tb/mosaic_iq_tb.sv`, `sim/tb/mosaic_cluster_bypass_tb.sv` | **changed**: the new ports connected (the IQ's second wakeup port is left at its inactive default, so `iq.wakeup_insert_select` still exercises the durable path exactly as before) |
| `sim/unit/tb_core_fabric.cpp` | **new**: the driver |
| `tools/run_fabric_controls.py` | **new**: the reproducible control runner |
| `tests/unit/registry.json` | **changed**: `rtl/core/mosaic_steering.sv` and `rtl/core/mosaic_cluster_bypass.sv` added to the 26 cases that build the core, next to their consumer, so every case that elaborates `mosaic_core`/`mosaic_dispatch`/`mosaic_cluster` can find the new modules. No task, status or `pending` field was touched; the integration lead owns the case's records. |

Not touched: `config/status/implementation_status.json`, `rtl/core/mosaic_vec_*`,
`rtl/core/mosaic_vrf.sv`, `results/PROGRESS.md`, `tests/programs/**`.

**No configuration change.** The unit count and the capability matrix are stated
in dispatch, the bank geometry comes from `config/geometry/p0.json` (already used
by I-032), and the strategy is a runtime input. No CSR, mode or profile knob was
added.

## 2. The integration, step by step, and what each step did to the numbers

The steps were landed in the card's risk order: (a) bank-aware allocation, then
(c) steering, then (b) bypass. The tree was green after each landing (the
required cases were re-run; §5). The per-step attribution below is measured with
the three `MOSAIC_FAB_STEP_NO_*` builds, which disable one step inside the
dynamic configuration while leaving the others on, plus one build with only the
bypass on (steering and bank off); each row is the *dynamic* run of that build.
These are instrumentation, not controls: a partial fabric fails this case's
dynamic-activity check by construction, so only the numbers are read from them.

```
$ python3 tools/run_fabric_steps.py    # builds build/p0/fabric_steps/<step>/ and runs each
```

| dynamic configuration | cycles | c0/c1 integer-ALU | c0/c1 branch | router grants | bypass captured/hit |
|---|---:|---:|---:|---:|---:|
| no step (= the fixed baseline) | 448 406 | 214 / 17 624 | 17 767 / 17 769 | 0 | 0 / 0 |
| **+ steering** (I-029) | 448 406 | 8 359 / 9 479 | 19 417 / 16 119 | 57 201 | 0 / 0 |
| **+ steering + bank preference** (I-032) | 448 486 | 8 530 / 9 308 | 18 804 / 16 732 | 57 281 | 0 / 0 |
| **+ steering + bank + bypass** (I-027) | 448 468 | 8 498 / 9 340 | 18 790 / 16 746 | 57 281 | 17 849 / 33 |

What each step did:

* **(a) the bank-aware allocation (I-032).** Wired by driving rename's
  `alloc_bias_en`/`alloc_bias_bank` from the routing decision: the preferred home
  bank is the routed cluster's (producer locality — a policy choice, since the
  card fixes only that it is a preference with a fallback). Its own case proves
  it cannot change a decision, and here it costs **+80 cycles** and shifts the
  distribution slightly. It never changes the architecture (§3).
* **(c) the steering policy (I-029).** Replaces dispatch's alternating
  `aff_toggle` when `fab_dyn_i` is high. It **changes nothing about the cycle
  count** on this program and everything about *where* work goes: the fixed
  affinity is badly skewed (cluster 1 executes 17 624 of 17 838 integer-ALU uops
  — `crt0`'s store loop lands every ALU macro on the same parity), and the
  policy balances it (8 359/9 479). That is the "changed something measurable"
  the card requires, and it comes from the policy, not from the toggle existing.
* **(b) the bypass (I-027).** Its slot is offered to the issue queue as a second,
  earlier value-visible wakeup. It taps 17 849 producers and **resolves 33
  operands early**, recovering 18 of the 80 cycles the bank bias cost. Measured
  separately (bypass on, steering off, fixed affinity), it resolves only **2**
  operands: under the fixed affinity's extreme skew a consumer waits in the
  dispatch queue until its producer has already written, so there is nothing for a
  one-cycle-earlier wakeup to do. The bypass's per-candidate operand-resolution
  ports (`s1_*`/`s2_*`/`fb_*`) are *not* used by the core — this issue queue
  resolves operands from wakeups, not from per-candidate reads — and their
  evidence remains the module's own case.

## 3. The fixed-versus-dynamic comparison

```
$ python3 tools/run_unit.py --profile p0 --case fabric.integrated
PASS fabric.integrated            task=I-090
```

The same binary, the same ELF, the same seed, one toggle. The driver runs the
image twice — `fab_dyn_i = 0`, then a full reset and `fab_dyn_i = 1` — and compares
the whole retire event stream and the four signature words.

| | fixed (`fab_dyn_i=0`) | dynamic (`fab_dyn_i=1`) |
|---|---:|---:|
| cycles | 448 406 | 448 468 |
| retired instructions | 71 123 | 71 123 |
| IPC (retired / cycle) | 0.15861 | 0.15859 |
| redirects | 17 788 | 17 788 |
| integer-ALU issues, cluster 0 / cluster 1 | 214 / 17 624 | 8 498 / 9 340 |
| branch issues, cluster 0 / cluster 1 | 17 767 / 17 769 | 18 790 / 16 746 |
| MUL/DIV issues | 0 | 0 |
| router grants (steering) | 0 (idle: the strategy is the affinity) | 57 281 |
| router grant reasons (none/affinity/class/locality/load/age) | all 0 | 0/0/0/0/**14**/**57 267** |
| bypass captures / early-resolved operands | 0 / 0 | 17 849 / 33 |
| signature words identical | — | yes |
| retire stream identical | — | yes (71 123 events, field for field) |
| architectural invariants | clean | clean |

The comparison is exact, not "close": every architectural field of every retire
event (sequence, PC, instruction word, length, register write, value, store
address/data/size, CSR write, trap cause/tval) and all four signature words are
equal. The **physical destination identity** the event port also carries is
deliberately *not* compared: it is the `{generation, tag}` rename allocated, it
legitimately moves when the allocator's bank preference changes, and the ISA
cannot observe it. Excluding it is stated here rather than left implicit.

The issue counts are reported per *class*, not only per unit: the machine's own
per-resource counters give the integer ALU and branch classes (the two clusters'
`o_c0_alu`/`o_c1_alu`/`o_c0_br`/`o_c1_br`), the MUL/DIV class its own counter, and
the LSU class the memory-path counters. The router's four units own the classes
its capability matrix states — unit 0/1 the integer ALU and branch classes, unit
2 the shared MUL/DIV and FP classes, unit 3 the load/store classes — so
`unit_issues` above is a per-class split as well as a per-unit one.

The dynamic configuration is **62 cycles (0.014 %) slower** on this program:
+80 from the bank preference, −18 recovered by the bypass. A steering policy that
helps on one program and hurts on another is exactly what the plan's "measured,
not inherited" requirement exists for; on this program it hurts, and the reason
is visible in the table — balancing the two clusters costs more in cross-cluster
operand latency than the skew cost.

## 4. Controls

```
$ python3 tools/run_fabric_controls.py
```

Each mutant is built from an empty directory with exactly one `-D`, must differ
from the shipping binary, and must exit 1 naming the check it breaks. All three
do (`all 3 mutants mutate the binary, exit 1 and name the check they break`).

| `-D` mutant | what it injects | result | the check it breaks |
|---|---|---|---|
| `MOSAIC_STEER_MUTANT_IGNORE_CAPABILITY` (in `mosaic_steering.sv`) | the capability matrix is ignored; the router's grant is the path decision, so an integer ALU macro granted the LSU is executed there as a memory access | exit 1 | `dynamic run: the cycle bound was reached` — the program never computes its result. The card's "an incompatible unit receives work" fail mode |
| `MOSAIC_FAB_MUTANT_DYN_SWAP_SRC` (in `mosaic_dispatch.sv`) | the dynamic route's two operand-value wires cross, so a two-register ALU op computes a different result in the dynamic configuration only | exit 1 | `retire the same architecture` — the retire streams diverge: the dynamic run reaches the exit protocol after 72 instructions instead of 71 123, with a different signature. The fail mode that matters most: dynamic scheduling must never change what the program computes |
| `MOSAIC_FAB_MUTANT_NO_DELTA` (in `mosaic_core.sv`) | the strategy the fabric sees is held low, so the "dynamic" run is the fixed machine | exit 1 | `changed something measurable` — 448 406 = 448 406 cycles and identical distributions. The measurement control: the comparison itself is falsifiable |

The shipping run is built and run first from an empty directory
(`RESULT PASS ... arch_identical=yes`), so a mutant means something.

The driver also carries `--control-fixed-is-dynamic`, which runs the *fixed*
configuration through the dynamic path so the two runs are identical by
construction; both the "changed something measurable" and the "router granted"
checks fail on it. It is a second, driver-side falsifiability control on the same
property as `NO_DELTA`.

## 5. Regression

Every case the brief names was re-run on this revision, all PASS:

| case | result | | case | result |
|---|---|---|---|---|
| `fabric.integrated` | PASS | | `fence.code_and_data_order` | PASS |
| `fabric.fixed_two_cluster` | PASS | | `mmio.exactly_once` | PASS |
| `core.corpus_sweep` | PASS | | `amo.linearization` | PASS |
| `core.corpus_branch` | PASS | | `lrsc.reservation_progress` | PASS |
| `core.mem_program` | PASS | | `rename.bank_bias_exhaustion` | PASS |
| `core.unwritten_reg_read` | PASS | | `bypass.local_raw_chain` | PASS |
| `iq.wakeup_insert_select` | PASS | | `steering.capacity_locality` | PASS |
| `boot.p1_contract` (p1) | PASS | | `arbiter.forward_progress` | PASS |
| `privilege.permission_matrix` (p1) | PASS | | `sv39.walk_and_faults` (p1) | PASS |

ACT4, 127/127:

```
$ python3 tools/run_act_dut.py --no-generate
RESULT PASS core.act_dut ran=127 passed=127 expected=127
```

ACT4 runs with `fab_dyn_i` low (its driver predates the fabric), which is the
point: the fabric is off by default and the integrated machine is unchanged.
The run refreshed the tracked record (`results/v043/**`,
`results/unit/core.act_dut/result.json`) so the recorded DUT hash is this
revision's.

Gates, all green: `make check` (config, contracts, event contract, records,
exclusions + `--negative`, coverage, capability matrix, upstream, isolation) exit
0; `make lint PROFILE=p0` and `PROFILE=p1` — `lint: 51 source file(s) clean`, each
with its `--self-test`; `make lint-slang PROFILE=p0` exit 0.

## 6. Not covered

* **The I-030 bounded-service arbiter is not wired in.** Its own report says why
  it is not a wire, and this lane confirms it: the arbiter's request model is
  four per-class queues with their own arrival timestamps and one service port,
  while the completion path's four producers (`mosaic_wb_arbiter`) are four
  `wb_event_t` ports selected by lowest index and coupled to the PRF write, the
  rename offer, the ROB completion and a same-bank collision rule *in the same
  decision*. Inserting the quota policy means re-expressing every completion
  producer as the arbiter's class ports, restating the downstream bound the
  arbiter checks as an assumption, and re-stating what
  `wb.same_bank_many_producers` measures. That is a redesign of the durable
  completion path, not a wiring change, and doing it half-way risks the one path
  every instruction depends on. `arbiter.forward_progress` remains the evidence
  for the module's internal rules. Class assignment is also not free: p0 has no
  VECTOR producer, and MUL/DIV and the LSU complete into the cluster that owns
  the uop, so the class is a property of the producer port rather than of the
  uop — a mapping this integration would have to invent.
* **The I-031 ownership FSM is not wired in.** It exists for reconfiguration;
  p0 and p1 never reconfigure, so wiring it to the real issue queues and result
  pipeline would be dead logic in this machine. The card allows leaving it out
  in exactly this case, and it is left out rather than added as a no-op.
  `reconfigure.drain_and_generation` remains the evidence for its contract.
* **The locality key of the steering policy is not exercised.** Dispatch carries
  tags and generations, not the producing cluster of a macro's operands, so
  `req_locality_en` is tied low and the locality key discriminates nothing here:
  `reasons` shows load (14) and age (57 267), never locality (0) or capability
  (0). The policy as written and the policy as wired differ in this one key, and
  that is stated rather than implied. Adding it needs producer provenance from
  rename, a new field that does not exist today.
* **The steering is not asked about the LSU.** Loads and stores keep the single
  fixed path they have always had. Two reasons: the router replaces the *cluster*
  affinity, and the LSU is not a cluster; and the core's memory-insert readiness
  is combinational in the offer it would gate (a store that faults is ready by a
  different arm), so offering the LSU to the router would close a combinational
  loop through the router and back into the memory path. The LSU is still in the
  capability matrix, so the router's model of the machine is complete; it is
  never asked. `unit_issues[3]` is therefore 0, and the card's "issue
  distribution across units" is measured for the two clusters. The default
  image issues no MUL/DIV macros, so unit 2 is 0 there; `p11_bigmuldiv.i0` does,
  and shows the router granting it: `unit_issues=[27886 29332 6 0]`.
* **The bypass is exercised, but barely, and its candidate ports are not.**
  17 849 captures, 33 early-resolved operands, 2 under the fixed affinity. The
  reason is an interpretation, not a measurement: with the four-deep dispatch
  queue most consumers are inserted *after* their producer has written, so the
  operand is already durable and there is nothing for a one-cycle-earlier wakeup
  to do. The candidate-based resolution ports (`s1_*`/`s2_*`/`fb_*`) are dead in
  this core because its issue queue resolves from wakeups; only the slot's
  identity and value are used, as a second wakeup port.
* **One program, one seed.** Every number above is `p02_branch.i0.elf`, seed 1.
  The five other corpus images I ran by hand (`p01`, `p06`, `p07`, `p10`, `p11`)
  all pass the same checks with the same shape — dynamic is 62–74 cycles slower,
  the ALU distribution moves from ≈200/17 600 to ≈8 500/9 300, bypass hits
  14–27 — so the finding is not a single-program accident, but the registered
  case measures one ELF, and a wider sweep is not claimed.
* **No timing, area or Fmax claim.** The router is combinational over four units
  and the bypass adds a second wakeup port to the issue queue; whether either
  fits the cycle in the integrated machine is not measured.
* **The arbiter and the owner FSM keep their own cases as their evidence.** Each
  module's internal rules remain proven by `arbiter.forward_progress`,
  `reconfigure.drain_and_generation`, `steering.capacity_locality`,
  `bypass.local_raw_chain` and `rename.bank_bias_exhaustion`; this package is
  about their *wiring*, and it leaves those cases untouched and passing.
