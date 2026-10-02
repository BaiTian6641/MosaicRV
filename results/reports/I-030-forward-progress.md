# I-030 — queue/port quotas and a starvation bound

Work package **I-030** of `docs/implementation-plan.md` (section 6; integration
obligations `V-032` credit/fairness contract and `V-033` arbitration
implementation and progress assumptions, section 3.1).

> 实现 queue/port 配额与饥饿上界 — Inputs: arbitration classes、最大 service
> time、请求/完成 credit. Action: 为 ready oldest 与返回通道保留逃生容量，写
> fairness assumptions；测试饱和 short/long FU 混合与持续新请求。
> Outputs: bounded service policy、CASE=arbiter.forward_progress、formal
> assumptions.
> Pass: 在明确的"下游最终 ready/内存有界返回"假设下存在可计算等待上界，并被
> assertion/压力用例覆盖。
> Fail: 用无限 timeout 当公平性证明，或高优先级 scalar 永久饿死 vector。

## STATUS: COMPLETE (module + case + controls), with one integration item left open

| Acceptance item | Evidence |
| --- | --- |
| The bounded-service policy exists as RTL, stated as a rule | `rtl/core/mosaic_arbiter.sv` (new); the rule is the file header and the *The rule* section below |
| Classes, reserved minimum, maximum service per class | four arbitration classes with quotas 4/2/1/1 summing to a window of 8 grant opportunities; `o_quota*`, `o_window`, `o_depth` read back and checked by the driver |
| Reserved escape capacity for the ready-oldest | rule 3: when no class is under quota, the oldest pending request is served over quota; `o_escape_ctr`, exercised in the directed and stress phases (30 and 65 grants in the registered run) |
| Reserved capacity for the return channel | MEMORY (load/store completions) holds a quota of 2 per window that the SCALAR flood cannot take: `directed:return-channel` (53 delivers, worst wait 11 against a bound of 36) |
| The fairness assumption written down | *The assumption* below, and the module header; it is *checked*, not assumed: `o_busy_run_max`, `o_assumption_violated` |
| A computable waiting bound under that assumption | `gap_max * (W * (ceil((n+1)/share_min) + 1) + 2)`; every term measured from the run (see *The bound*) |
| The worst-case wait of each class measured against that bound | `[bound]` lines and the table under *Saturation*; 668/271/200/197 waits measured, worst 17/33/55/55 against 54/78/126/126 |
| A class cannot be starved indefinitely | per-window reservation check at every window close (167 windows in the registered run), plus the bound check per request |
| An assumption violation is reported as a violation, not as a bound | `directed:assumption` holds the port not-ready past `D_MAX`: `o_busy_run_max` 6 against `D_MAX` 4, `o_assumption_violated` set, cleared on reset |
| Fail criterion — an infinite timeout used as a fairness proof | no timeout exists in the case; the bound is computed from the run and the assumption is checked; a violated assumption fails the run named `assumption` |
| Fail criterion — high-priority SCALAR starves VECTOR | `MOSAIC_ARB_MUTANT_STARVE` exit 1, first failure `starvation:`; and the naive control shows the *stimulus* starves VECTOR even though the policy does not |
| Control — loss instead of backpressure | `MOSAIC_ARB_MUTANT_DROP_ON_QUOTA` exit 1, first failure `conservation:` |
| Control — a bound that is not computable from the policy's numbers | `MOSAIC_ARB_MUTANT_REQ_ID_SERVICE` exit 1, first failure `bound:` |
| `python3 tools/run_unit.py --profile p0 --case arbiter.forward_progress` | `RESULT PASS … 62593 per-cycle checks over 3414 cycles … seed 1`, exit 0, shipping sha256 `6ead23d6ddc687728606ecbbd3ace0aa3affa1d45ad4f0fe03ff5b3c95c69aa4` (identical from two independent empty build directories) |
| The same case at four further seeds | 2, 7, 12345, 999983 all PASS with the same shape of result (table below) |
| `python3 tools/run_arbiter_controls.py` | all three mutants rebuild from an empty directory, differ from the shipping binary, exit 1 and name the check they break |
| `tests/unit/registry.json` untouched by this lane | the entry named three sources that did not exist; all three were created at exactly the registered paths |
| Reported per-cycle invariants | conservation (occupancy, delivery order, no loss, counters), window/quota accounting, escape legality, the assumption, the per-window reservation |
| This report | — |

## The registry entry

`tests/unit/registry.json` already carried the case, naming

| field | value | state found |
| --- | --- | --- |
| `task` | `I-030` | — |
| `top` | `mosaic_arbiter_tb` | — |
| `rtl` | `rtl/core/mosaic_arbiter.sv` | **missing** |
| `sv` | `sim/tb/mosaic_arbiter_tb.sv` | **missing** |
| `cpp` | `sim/unit/tb_arbiter.cpp` | **missing** |

All three sources were missing and were created at exactly those paths. The
registry was **not** edited by this lane. The card allows the mechanism to live
"in `mosaic_wb_arbiter.sv`, `mosaic_result_fifo.sv` **and/or** a new quota module
of yours"; the registered topology picks the third, so the policy is a new module
and the integrated wiring is described, not performed, under *Not covered*.

## Files

| file | state | what it is |
| --- | --- | --- |
| `rtl/core/mosaic_arbiter.sv` | new | the policy: four class queues, the windowed quota, the escape, the assumption monitor, three mutants |
| `sim/tb/mosaic_arbiter_tb.sv` | new | the wrapper: flat 32-bit pins, geometry read-back |
| `sim/unit/tb_arbiter.cpp` | new | the driver: the downstream environment, the per-cycle invariants, the measured bound, the naive control, the phases |
| `tools/run_arbiter_controls.py` | new | the mutant campaign: rebuild from an empty directory per mutant, `-D` recorded, hash compared, first failure required |
| `rtl/core/mosaic_wb_arbiter.sv` | **unchanged** | zero lines changed |
| `rtl/core/mosaic_result_fifo.sv` | **unchanged** | zero lines changed |
| `tests/unit/registry.json` | **unchanged** | zero lines changed |

## The rule

One service port — the completion/return path — serves four classes. The classes
are **arbitration** classes of the completion path, not ISA classes; the mapping
to the machine the plan describes is stated so the environment is honest:

| index | class | traffic | service |
| --- | --- | --- | --- |
| 0 | `SCALAR` | the integer ALU/branch completions | short (1 cycle in the case) |
| 1 | `MEMORY` | the **return channel**: load/store completions | 2 cycles |
| 2 | `MULDIV` | the shared iterative MUL/DIV completions | long (3 cycles) |
| 3 | `VECTOR` | the wide/long-latency completions | long (3 cycles) |

```
W  = sum of the quotas = 8.
Q  = [SCALAR 4, MEMORY 2, MULDIV 1, VECTOR 1],  sum Q = W.
A window is W grant opportunities: W cycles in which the arbiter has pending
work and the port is ready. A busy port does not consume the window.

1. Eligibility.  class c is eligible iff it has a pending request and
                 used[c] < Q[c].
2. Normal service. If any class is eligible, the port serves the *oldest*
                 pending request among the eligible classes (smallest arrival
                 timestamp; ties on the lowest class index).
3. Escape.       If no class is eligible but some class has pending work, the
                 port serves the oldest pending request among all classes,
                 over quota. This is the reserved escape capacity: a class is
                 never blocked by the exhaustion of every quota, and the
                 globally oldest request always has a path to service.
4. Window.       Every grant is charged to the granted class; after the W-th
                 grant of a window the window wraps (used[] and the count
                 reset, o_window_index ++).
```

Each class owns a FIFO queue of 4 entries; `req_ready[c]` is low when it is full
and that is the **backpressure** — the producer holds its request and its tag,
the queue never samples a request it cannot keep, and `o_drop_ctr` exists to stay
at zero. `req_ready` deliberately does not consult the downstream, so there is no
combinational path from the port back into the producers.

**Why the reservation is exact.** In a window, the grants charged to classes
other than c number at most `W - Q[c]`, because each such grant is charged to its
own class's quota and a class can be charged at most its quota there. An escape
grant can only happen when *no* class with pending work is under its quota, so
while c is under quota and has work no escape occurs and no non-c grant is
uncharged. Therefore **a class that has pending work throughout a window is
granted at least Q[c] times in it**. Equivalently, if c got fewer than Q[c]
grants in a window, then c was under quota at every opportunity and every grant
was normal, so the W grants of the window were charged `sum Q = W` times to
classes under quota — contradiction. The case checks this invariant at every
window close, from the run.

## The assumption

The bound above is in *windows*, and a window is W opportunities. Turning that
into wall-clock time needs the one thing the arbiter cannot prove about its
downstream, so it is written down, and it is **checked rather than assumed**:

> **ASSUMPTION (bounded downstream).** While the arbiter has pending work, the
> port is not ready for at most `ARB_D_MAX` (4) consecutive cycles; equivalently
> the downstream becomes ready again within 4 cycles of going busy. For the
> memory class this is "memory returns within a bound".

The arbiter observes the longest run of `pending work && !ready`
(`o_busy_run_max`) and sets `o_assumption_violated` — stickily, until reset —
when that run exceeds `D_MAX`. An infinite timeout, a "the simulation ended" or a
large cycle count is **not** a fairness proof: if the assumption is violated the
arbiter reports a **violation**, and any bound computed from it is void for that
stretch. The case asserts both directions (see *Directed phases*).

## The bound, and how the run computes it

The driver measures, **from the run**, every term of the number it holds the DUT
to:

| term | what it is | measured as |
| --- | --- | --- |
| `share_min[c]` | the smallest number of grants class c received in a window in which c had pending work at *every opportunity* of that window | the driver's own per-window counters, evaluated at each window close |
| `W` | the window, in grant opportunities | the driver counts opportunities per window and requires *exactly* `W` in every window that closed |
| `gap_max` | the longest wall-clock distance between two consecutive grant opportunities | the driver's own count of consecutive not-ready cycles with work pending, +1 |
| `n` | the requests of class c ahead of a given request | the driver's queue model at the moment it accepted that request |

and checks each measured wait against

```
    bound = gap_max * ( W * (ceil((n + 1) / share_min) + 1) + 2 )
```

All of it from the run's own numbers. If the run shows a smaller share or a
longer gap, the number the run is held to moves with it. The `+ 1` window is the
arrival that lands at the end of a window whose quota is already spent; the
`+ 2` cycles absorb the distance to the first opportunity. The bound is a
conservative over-approximation of the true worst case, which is what a bound has
to be.

Registered run (seed 1, `[bound]` lines from the run):

| class | waits measured | worst wait | bound for that request | `share_min` | `gap_max` | `W` | `n` ahead |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SCALAR | 668 | **17** | 54 | 4 | 3 | 8 | 3 |
| MEMORY | 271 | **33** | 78 | 2 | 3 | 8 | 3 |
| MULDIV | 200 | **55** | 126 | 1 | 3 | 8 | 3 |
| VECTOR | 197 | **55** | 126 | 1 | 3 | 8 | 3 |

The declared policy is 4/2/1/1, and the run's measured `share_min` is exactly the
declared quota for every class: the reservation is not merely asserted, it is
what the run delivered.

Reproduced at four further seeds (the case is registered at seed 1); worst wait /
bound per class, and the mix's own totals:

| seed | SCALAR | MEMORY | MULDIV | VECTOR | grants | refusals | escapes | windows |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 17 / 54 | 33 / 78 | 55 / 126 | 55 / 126 | 1336 | 5734 | 65 | 167 |
| 2 | 19 / 54 | 33 / 78 | 55 / 126 | 55 / 126 | 1336 | 5657 | 62 | 167 |
| 7 | 18 / 54 | 33 / 78 | 55 / 126 | 55 / 126 | 1335 | 5708 | 63 | 166 |
| 12345 | 23 / 54 | 33 / 78 | 55 / 126 | 55 / 126 | 1335 | 5578 | 64 | 166 |
| 999983 | 20 / 54 | 30 / 78 | 55 / 126 | 55 / 126 | 1335 | 5667 | 65 | 166 |

## The stress mix (saturation)

2400 cycles, every class a continuous stream of new requests, short and long
service, one class (VECTOR) that a naive priority scheme starves:

| measured | value |
| --- | --- |
| cycles | 2400 |
| completions delivered | 1336 (SCALAR 668, MEMORY 271, MULDIV 200, VECTOR 197) |
| requests admitted | 1347 |
| admissions refused (backpressure) | 5734 |
| escape (over-quota) grants | 65 |
| stall cycles (work pending, port not ready) | 1063 |
| windows closed | 167, each with exactly 8 grant opportunities |
| longest not-ready run (driver and DUT) | 2, against `D_MAX` 4 |
| gap between opportunities | 3 |

Demand: SCALAR holds a request in **every** cycle (the card's high-priority
stream); MEMORY returns in bursts of 60 cycles (so its two reserved slots are
sometimes unused, which is what makes the escape reachable in the mix too);
MULDIV 25%, VECTOR 25%. Every class had pending work at every opportunity of at
least one window, so every class's share is measured from the run rather than
assumed.

At the end of the mix the run requires, and got: every class granted (no class
starved), the per-window reservation held at every one of the 167 window closes,
every measured wait within the bound computed from the same run, backpressure
exercised, the escape taken, and a fully-busy window for every class.

## The naive control

A stress case that no scheme could starve proves nothing, so the driver replays
exactly the **offer stream the DUT saw** through a second model of the same
machine — same queue depth, same class-queued handshake, same service times, same
downstream — whose only difference is the selection rule: strict class priority,
no quota, no escape. It is a demonstration that the stimulus reaches the card's
fail mode, not a second DUT (its acceptance times differ from the DUT's because
its queues drain differently).

Same stimulus, registered run:

| class | naive grants | naive max wait | naive requests never served |
| --- | --- | --- | --- |
| SCALAR | 2400 | 0 | 0 |
| MEMORY | 0 | — | 4 |
| MULDIV | 0 | — | 4 |
| VECTOR | 0 | — | 4 |

The naive scheme serves SCALAR on every opportunity and leaves one full queue of
MEMORY, MULDIV and VECTOR requests unserved for the whole run. The same
comparison is made in `directed:reservation` (SCALAR flood + VECTOR: naive leaves
4 VECTOR requests unserved, the DUT serves 40 VECTOR requests, worst wait 15
against a bound of 54).

## Directed phases

| phase | what it witnesses |
| --- | --- |
| `directed:fifo-order` | three requests of one class leave **oldest first** with their own tags (a granted request shares a cycle with an admission, and the check records both); the escape keeps a lone class moving past its quota |
| `directed:credit-backpressure` | a full class queue **refuses** (6 refusals) and the producer's held offer is admitted later (8 admissions); nothing is lost |
| `directed:reservation` | SCALAR holds a request in every cycle and VECTOR holds one too: VECTOR is served 40 times, worst wait 15 against the run's own bound of 54; the same stimulus leaves 4 VECTOR requests unserved under naive priority |
| `directed:return-channel` | the same flood, with MEMORY holding a request every cycle: 53 returns delivered, worst wait 11 against a bound of 36, reserved share measured 4 per window |
| `directed:escape` | one class past its quota is still served: 6 of the 7 grants are over quota and the port is never idle with work pending |
| `directed:assumption` | the port held not-ready for 7 cycles with work pending (past `D_MAX` 4) is reported by the DUT as an **assumption violation** (`o_busy_run_max` 6, `o_assumption_violated` set) and the flag clears on reset. The case fails named `assumption` if the flag stays clear, and fails named `assumption` if the driver's own stimulus ever violates the assumption in the other phases |
| `saturation` | the table above |
| `determinism` | the same stimulus and seed reproduce 170 grants over 300 cycles exactly (cycle, class, tag) |

## Mutants

House rule, all three: the `-D` is asserted to occur in `rtl/core/mosaic_arbiter.sv`
before the campaign; the build directory is deleted and rebuilt per mutant, so no
shipping object can be reused; the `-D` is on the recorded command line in that
build's `build_command.txt`; every mutant binary's sha256 differs from the
shipping binary's; and the shipping build is rebuilt and run first, from an empty
directory.

Base: exit 0, sha256 `6ead23d6ddc687728606ecbbd3ace0aa3affa1d45ad4f0fe03ff5b3c95c69aa4`,
`RESULT PASS … 62593 per-cycle checks over 3414 cycles … seed 1`.

| mutant | exit | sha256 | first failing check | injects |
| --- | --- | --- | --- | --- |
| `MOSAIC_ARB_MUTANT_STARVE` | 1 | `277b0f82552fcb44…` | `directed:reservation: cycle 47 (starvation): starvation: class VECTOR was granted 0 time(s) in window 0 although it had pending work at every opportunity of that window; its reserved share is 1, so a class with continuous work can be starved for ever` | eligibility ignores the quota and the choice is the lowest class index: strict class priority |
| `MOSAIC_ARB_MUTANT_DROP_ON_QUOTA` | 1 | `ebe35cf164c5b3d3…` | `directed:fifo-order: cycle 13 (conservation): conservation: the port is ready with 2 pending request(s) and no transfer happened; an accepted request was neither delivered nor held (loss instead of backpressure)` | the escape pops the selected request and discards it instead of delivering it |
| `MOSAIC_ARB_MUTANT_REQ_ID_SERVICE` | 1 | `bbc3ff77ee63e88a…` | `directed:credit-backpressure: cycle 26 (bound): bound: the DUT's window is index 0 with 1 grant(s), the driver counted index 0 with 2; the window is not the quota's W grant opportunities, so no waiting bound can be computed from it` | a window is charged only when the requester changes, so its length in opportunities is not W and the quota stops bounding a class |

What each control is evidence for, in the card's terms:

* **A high-priority scalar stream starving the vector class.** The mutant removes
  the quota from eligibility and orders by class index, which is the naive scheme
  the card names as the fail mode. It is caught at the first window close in
  which VECTOR had pending work at every opportunity and got nothing, named
  `starvation` in the card's own terms. The naive control shows the *same*
  outcome on the *unmutated* shipping stimulus, so the case's stress is one that
  genuinely reaches this failure.
* **Loss instead of backpressure.** The mutant resolves a spent quota by popping
  and discarding the selected request. It is caught on the cycle it happens: the
  port is ready, requests are pending, and no transfer occurred. The individual
  conservation checks (occupancy against the driver's queue, the delivered
  request against the oldest accepted one, the DUT's own `o_drop_ctr`) would each
  catch it too; the first observable event is reported rather than its aftermath.
* **A bound that is not computable.** The mutant makes the window's opportunity
  accounting depend on the requester rather than on the grant. The driver counts
  the grants itself, so the DUT's window count and its per-class `used`
  counters stop matching the run at the second same-tag grant, and the run fails
  named `bound` with both counts — the failure mode is "no waiting bound can be
  computed", not "the bound was exceeded", which is what the card asks for.

## Gates

| gate | result |
| --- | --- |
| `python3 tools/lint_rtl.py --profile p0` | `rtl/core/mosaic_arbiter.sv: clean as mosaic_arbiter`. The gate reports `4 of 38 source file(s) failed`, **all four in other lanes' in-flight work**: `mosaic_amo_alu.sv` and `mosaic_amo_unit.sv` (missing newline at end of file), `mosaic_core.sv` (`PINMISSING dec_len0/dec_bits0` — its instantiation of `mosaic_dispatch` does not connect ports the dispatch file declares), and `mosaic_lsu_endpoint.sv` (fails on the `mosaic_amo_alu.sv` include). None of the four is touched by this lane. |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_arbiter.sv sim/tb/mosaic_arbiter_tb.sv` | clean, exit 0 — every rule PASS, including `NoLatchesOnDesign`, `AlwaysCombNonBlocking`, `EnforcePortPrefix` |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | **fails on `rtl/core/mosaic_core.sv`**, another lane's in-flight edit: `dbuf_ctl_new.amo_op` and the rest of the vector are driven by two processes (`-Wmultiple-always-assigns`). Not this lane's file; no other file is reported. |
| `python3 tools/check_records.py` | green — `records agree: 42 delivered package(s), 53 registered case(s), every claimed case exists and belongs to the package claiming it` |
| `arbiter.forward_progress` | PASS, exit 0, sha256 `6ead23d6ddc687728606ecbbd3ace0aa3affa1d45ad4f0fe03ff5b3c95c69aa4` |
| `python3 tools/run_arbiter_controls.py` | all 3 mutants OK (exit 1, different binary, named first failure) |

## Regression

| case | result on this working tree |
| --- | --- |
| `wb.same_bank_many_producers` | **PASS** |
| `completion.fu_collision` | **PASS** |
| `steering.capacity_locality` | **PASS** |
| `iq.wakeup_insert_select` | **PASS** |
| `fabric.fixed_two_cluster` | **FAIL — not this lane.** Builds, then fails `cluster-fabric at cycle 151: both issue queues held live work in the same cycle`. This lane's four new files are not in the case's source lists; the case compiles `mosaic_core.sv`, `mosaic_dispatch.sv` and `sim/tb/mosaic_core_tb.sv`, all three of which are **modified by other lanes** in this working tree. Verified at pristine `HEAD` (a scratch `git worktree`, since the working tree must not be disturbed): **PASS**. |
| `core.corpus_sweep` | **FAIL — not this lane.** All 39 runs `STOPPED` on this working tree. Same cause: the case compiles the in-flight `mosaic_core.sv`/`mosaic_dispatch.sv`. Verified at pristine `HEAD` in the same scratch worktree (after `make -C tests/programs all`): **PASS, 39/39**. |

In other words: the two failures are the working tree's concurrent state, not
this change — proven by running the identical cases against `HEAD` without the
sibling edits, where both pass. The four new files are not referenced by either
case.

## Not covered

* **The policy is not wired into the live core.** `mosaic_wb_arbiter.sv` and
  `mosaic_result_fifo.sv` have **zero lines changed**; the quota arbiter is a
  standalone module that the registered case drives. What an integration would
  need, precisely: (1) the completion producers (two clusters, the shared MUL/DIV
  and the memory path) re-expressed as the arbiter's per-class request ports —
  today they are four `wb_event_t` ports selected by the lowest index in
  `mosaic_wb_arbiter.sv`; (2) a decision about what the shipping core's class
  assignment is (which completion is SCALAR, which MEMORY, which VECTOR — p0 has
  no VECTOR producer, and MUL/DIV and the LSU complete into the cluster that owns
  the uop, so the class is currently a property of the producer port, not of the
  uop); (3) the downstream readiness signal, which today is "the PRF/rename/ROB
  handshake for the one published completion" and would have to be stated as a
  bound or it is exactly the assumption this case reports violations of;
  (4) queue depth and quotas as configuration from the same place the rest of the
  geometry comes from. Note that replacing the existing lowest-index selection
  changes what `wb.same_bank_many_producers` measures, so that case would have to
  be re-stated as part of the integration rather than assumed to keep passing.
* **The vector class does not exist yet.** There is no RVV in this core: no
  vector register file, no vector unit, no vector instruction (I-049..I-063 have
  not landed), so nothing in the RTL produces VECTOR traffic today. The case
  models the class it protects as an **arbitration class of long-latency
  completions** with its own queue and a reserved quota of 1 per window, and its
  producer in the case is the driver's offer port `req_valid[3]`. What is
  therefore proven is the *policy* property (a class with a reserved quota cannot
  be starved by a louder class, whatever produces it); what is **not** proven is
  anything about a real vector unit's completion rate, its latency, or its
  interaction with the rest of the pipeline. When I-049..I-063 land, their
  completions attach to `req_valid[3]` and the quota is already reserved for them;
  until then this class is a placeholder with the right shape and no producer.
* **The downstream is a model, not the memory system.** The "memory returns
  within a bound" clause is implemented by the driver's downstream model (a grant
  of class c holds the port for `kService[c]` cycles) and by the memory-class
  bursts in the stress mix. Nothing here measures a real LSU, L1 or DRAM latency,
  and the real memory path's readiness distribution is unknown to this case.
* **`D_MAX = 4` is this case's declared environment, not a measurement of
  silicon.** The bound is conditioned on it. The case measures the actual longest
  not-ready run (2 in the registered run) and the bound uses the measured gap, but
  the *assumption check* is against the declared 4; a downstream whose worst-case
  service exceeds 4 would be reported as an assumption violation, and correctly
  so.
* **The bound is conservative, not tight.** The `+1` window and `+2` cycles are
  slack for the arrival's position in the window; the measured worst waits are
  2–3× below the bound. A tight bound would need a different proof (per-request
  positions rather than per-window shares), which nothing in the card asks for.
* **The escape makes `used[c]` exceed Q[c].** Over-quota (escape) grants are
  charged to the granted class, so `o_used` is a *minimum* witness ("this class
  got at least this many") and not a hard cap; what W bounds is the number of
  grant opportunities in a window. Stated here so `o_used` is not misread.
* **No per-requester fairness inside a class.** A class's queue is FIFO by
  arrival, so a slow requester cannot jump ahead, but there is no claim that
  requesters *within* a class get a fair share of their class's quota. The card's
  unit of fairness is the class; requester-level QoS would be a different policy
  and a different case.
* **No synthesis, timing, area or Fmax claim.** The module is combinational over
  a four-entry queue rebuild and three 32-bit comparisons; whether it fits the
  integrated completion path's cycle is not measured.
* **One geometry.** Four classes, depth 4, window 8, quotas 4/2/1/1, D_MAX 4 are
  built and run. A different geometry is a different instance of the policy; the
  module is written with loops over `ARB_N_CLASSES` and derives its window from
  the quotas, but only this instance is elaborated and measured.
* **Counter widths.** `o_grant_ctr`, `o_refuse_ctr` and the rest are 32 bits; a
  campaign longer than 2^32 grants or admissions would wrap. The registered
  campaign is 1336 grants and far below that.
* **The requester tag is opaque.** `req_src` is an 8-bit value this module
  carries and never compares (except in one mutant that exists to be caught);
  nothing here proves identity semantics, generation or reuse.
* **The naive control is a model.** Its queue acceptance times differ from the
  DUT's (its queues drain differently), so it demonstrates that the *stimulus*
  starves a naive scheme on the same machine shape; it is not a second DUT and its
  grant counts are not comparable to the DUT's.
* **Fairness under an assumption violation is not claimed at all.** If the
  downstream never becomes ready, the arbiter holds every request (nothing is
  lost, the producers are back-pressured) and reports the violation; it does not
  bound anything. That is the card's "infinite timeout is not a fairness proof"
  read literally: the case has no timeout and claims no bound in that state.
