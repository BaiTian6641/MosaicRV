# I-062 — criticality/QoS memory arbitration (MEF scheduler)

Work package **I-062** of `docs/implementation-plan.md` (section 3.1; validation
obligation `V-063`; sources SRC-03 §Memory Fabric 三层调度, §Critical Load,
VR-014).

> 实现 criticality/QoS memory 仲裁 — Inputs: scalar dependent load、vector
> bulk、prefetch 类别、finite queue bounds. Action: 采用 age-based escape +
> scalar reservation + bulk 配额；测试 scalar chain 与持续 streaming 的混合压力，
> 记录 p50/p99 latency. Outputs: MEF scheduler、CASE=memory.qos_no_starvation.
> Pass: 各类在声明的 service assumptions 下满足 finite progress bound；数据结果
> 不受优先级影响. Fail: scalar 永久抢占 bulk，或 performance policy 改变 memory
> ordering.

## STATUS: COMPLETE (module + case + 4 controls + report)

| Acceptance item | Evidence |
| --- | --- |
| The scheduler exists as RTL, its mechanisms stated as rules | `rtl/core/mosaic_mem_qos.sv` (new); *The rule* below and the file header |
| Three classes from the request's **identity** | the class is the request's lane/kind: SCALAR, PREFETCH, BULK; no priority field decides it (*Classes*) |
| **Age-based escape** | an eligible-under-ceiling request is served oldest-first; when no class is eligible, the oldest request that has **aged** (age ≥ AGE_LIMIT = 32) is served over quota. This is the only over-quota path (`o_escape_ctr`); the directed age phase and the stress mix exercise it (17 and 48 grants) |
| **Scalar reservation** | SCALAR quota 4 of W = 8 grants: a class with pending work at every grant of a window is granted ≥ Q per window (proof in the header); `share_min[SCALAR] = 4` measured at every phase |
| **Bulk quota** | BULK quota 2 of 8: a normal grant never charges a class at its ceiling; an over-quota grant must be an age escape (`quota:` check) |
| `CASE=memory.qos_no_starvation` | `tests/unit/registry.json` names top `mosaic_qos_tb`, driver `sim/unit/tb_qos.cpp`, RTL `mosaic_mem_qos.sv`; registered `"pending": true` (the registry is the integration lead's; this lane did not edit it) |
| The case runs a **scalar dependent chain** against sustained bulk | `chain` phase: each load's result is the next load's address; 344 links over 1200 cycles against a 100 % bulk stream |
| **p50/p99 per class** measured and printed | `[lat]` lines; the table below |
| With the policy **off** the bulk class starves the scalar chain | `naive` phase (`qos_en = 0`): scalar chain advances **0** links while bulk is served 400 times — the stimulus provably reaches a fail mode |
| The **data result is identical** with the policy off and on | `data_identity`: 200 requests, all 200 delivered with identical payload both ways, service order differs |
| The age escape fires for an old **bulk** request | `directed:age_escape`: first over-quota grant after a wait of **32** cycles (= AGE_LIMIT), serving the aged BULK head |
| Every class exercised | `chain` and `saturation` assert `accepted > 0` and `granted > 0` for SCALAR, PREFETCH, BULK |
| A finite progress bound under a **declared and checked** assumption | *The assumption* and *The bound*; every phase asserts each measured wait ≤ the declared bound, and the assumption monitor is asserted in both directions |
| Fail — scalar permanently preempting bulk | `MOSAIC_QOS_MUTANT_SCALAR_STARVE` exit 1, first failure `starvation:` — caught by the reservation bound, not a timeout |
| Fail — a policy that changes a result/ordering | `MOSAIC_QOS_MUTANT_DATA` exit 1, first failure `data:` |
| Fail — an old request waiting for ever | `MOSAIC_QOS_MUTANT_NO_AGE_ESCAPE` exit 1, first failure `escape:` (213 idle cycles with an aged request pending) |
| Fail — bulk monopolising | `MOSAIC_QOS_MUTANT_BULK_MONOPOLY` exit 1, first failure `quota:` |
| `python3 tools/run_unit.py --profile p0 --case memory.qos_no_starvation` | `RESULT PASS … 112152 per-cycle checks over 6819 cycles`, exit 0, shipping sha256 `56751ac4e259a5e77a59c87107d34698ec8e53f9a22d6fd3bf770da4175fda84` |
| The same case at p1 | PASS |
| `python3 tools/run_qos_controls.py` | all 4 mutants rebuild from an empty directory, differ from the shipping binary, exit 1 with a named first failure |
| `python3 tools/lint_rtl.py --profile p0 / p1` | `ok rtl/core/mosaic_mem_qos.sv: clean as mosaic_mem_qos`; both profiles `lint: 61 source file(s) clean` |
| `slang-tidy` on the new RTL | exit 0 (same two style WARNs as `mosaic_arbiter.sv`) |
| `make lint-cpp` | `73 file(s) clean`; `tb_qos.cpp` compiles with `-Wall -Wextra` |
| This report | — |

## Files

| file | state | what it is |
| --- | --- | --- |
| `rtl/core/mosaic_mem_qos.sv` | new | the scheduler: 3 identity classes, windowed quota, scalar reservation, bulk ceiling, age escape, checked assumption, 4 mutants |
| `sim/tb/mosaic_qos_tb.sv` | new | the wrapper: flat pins, geometry read-back |
| `sim/unit/tb_qos.cpp` | new | the driver: the environment, the per-cycle invariants, the dependent chain, the p50/p99 measurement, the naive control, the data-identity A/B, the phases |
| `tools/run_qos_controls.py` | new | the mutant campaign: rebuild from an empty directory per mutant, `-D` recorded, hash compared, first failure required |
| `rtl/core/mosaic_arbiter.sv` | **unchanged** | zero lines changed |
| `tests/unit/registry.json`, `config/status/*` | **unchanged** | zero lines changed by this lane |

## Classes

The class is the request's **own identity** — which lane the request arrives on
*is* what the request is. There is no priority field a producer can set.

| class | identity | policy role | quota / W |
| --- | --- | --- | --- |
| 0 `SCALAR` | a latency-critical load (the dependent chain) | reservation floor | 4 / 8 |
| 1 `PREFETCH` | a prefetch — **placeholder until I-063** | bounded share | 2 / 8 |
| 2 `BULK` | streaming vector traffic (`mosaic_vec_lsu` bulk) | ceiling | 2 / 8 |

## The rule

```
W      = sum of the quotas = 8. A **window** is W grants: the grant stream is
         partitioned into consecutive groups of W grants (not W wall cycles; a
         not-ready port or an idle stretch does not spend a grant).
Q[c]   = SCALAR 4, PREFETCH 2, BULK 2. used[c] = grants charged to c this window.

1. Eligibility.  c is eligible iff it has a pending request and used[c] < Q[c].
2. Normal.       If any class is eligible, serve the **oldest eligible** request
                 (smallest arrival timestamp; ties on the lowest class index).
3. Age escape.   Else, if some pending request has aged (now - arrival >= 32),
                 serve the **oldest aged** request, over quota, regardless of
                 class. This is the only over-quota path.
4. Idle.         Else the port is idle this cycle: every pending class is at its
                 ceiling and nothing has aged yet. The oldest pending request is
                 at most AGE_LIMIT cycles from being served.
5. Wrap.         A grant charges used[sel] and the window's count; the W-th
                 grant wraps the window and clears used[].
```

**Why the reservation is exact.** In a window where class c had pending work at
every grant, suppose c received fewer than Q[c]. Then `used[c]` was below Q[c]
throughout, so c was eligible at every grant, so no grant was an age escape and
every grant was charged to a class under its quota. A non-c class can be charged
at most Q[.] times per window, so at most W − Q[c] grants went elsewhere — fewer
than W, so c received ≥ Q[c] of the W. Contradiction. Hence a class with pending
work throughout a window receives at least its quota in it. This is the same
argument as I-030's, and the driver checks it at every window close
(`[bound] … share_min`).

**The cost of a hard ceiling.** I-030's escape fires the moment no class is
eligible; I-062's fires only when a request has aged. That makes the bulk
ceiling hard (bulk cannot exceed Q[BULK] per window except through an age
escape), and in exchange the port idles when every pending class is at its
ceiling and nothing has aged. The idle is bounded by AGE_LIMIT and is measured:
168 idle cycles in the directed age phase, 541 in the 2400-cycle stress mix.

## The assumption

> **ASSUMPTION (bounded downstream).** While the scheduler has pending work, the
> memory port is not ready for at most `QOS_D_MAX` (4) consecutive cycles;
> equivalently memory becomes ready again within 4 cycles of going busy.

It is written into the module header, exposed as `o_busy_run_max` and
`o_assumption_violated` (sticky until reset), and **checked**, not assumed: the
driver asserts the not-ready run never exceeds 4 while pending, the DUT never
reports a violation the stimulus did not cause, and a directed phase holds the
port not-ready for 7 cycles and requires the DUT to report the violation
(`o_busy_run_max` 6, flag set, cleared on reset). A cycle count or "the
simulation ended" is not a fairness proof.

## The bound

```
BOUND = AGE_LIMIT + (D_MAX + 1) * ( N*DEPTH + W * (ceil(DEPTH / min Q) + 1) )
      = 32        + 5           * ( 3*4    + 8 * (ceil(4/2) + 1) )
      = 32        + 5           * ( 12     + 24 )
      = 212 cycles
```

computed from the module's own constants (read back from the DUT), not from the
design's intent. A request is either served through its quota within
`ceil((n+1)/Q[c]) + 1` windows, or it ages (≤ AGE_LIMIT) and is served. Every
measured wait in every phase is checked against this bound; the per-class
`share_min` is checked against the reservation at every window close.

## p50/p99 (registered run, seed 1, p0)

Command: `python3 tools/run_unit.py --profile p0 --case memory.qos_no_starvation`

| phase | class | n | p50 | p99 | max | bound |
| --- | --- | --- | --- | --- | --- | --- |
| `chain` (scalar dependent chain vs 100 % bulk, 1200 cycles) | SCALAR | 344 | **2** | **6** | 6 | 212 |
| | PREFETCH | 172 | 2 | 4 | 5 | 212 |
| | BULK | 172 | 27 | 27 | 27 | 212 |
| `saturation` (mixed stream, 2400 cycles) | SCALAR | 628 | **11** | **32** | 32 | 212 |
| | PREFETCH | 265 | 23 | 23 | 23 | 212 |
| | BULK | 362 | 23 | **33** | 33 | 212 |

The scalar chain's tail (p99 6 cycles in the chain phase, 32 in the stress mix)
is what the reservation buys: the same-shaped scalar traffic with the policy off
is starved outright (below). BULK's p99 stays at 33, i.e. streaming is not
consumed by the reservation.

Stress mix totals: 2400 cycles, 1255 grants, 1263 accepted, 4799 refusals
(backpressure), 48 age escapes, 156 windows closed, 627 stalled cycles, 541 idle
cycles. Reservation held: `share_min = quota` for every class.

## On/off data identity (a QoS policy is a scheduling choice, not a memory model)

`data_identity` builds a fixed 200-request mixed stream (unique tags, payloads),
offers each request and holds it until accepted, and drains. It is replayed
twice — policy off (`qos_en = 0`, throughput-first) then policy on — and the
delivered `(tag → payload)` map is required to be identical, every payload to
equal the offer, and the two service orders to differ.

Result: `delivered=200 identical=yes order-differs=yes`. Priority changed **when**
each request was served, never **what** it returned. The DUT's per-cycle `data:`
check (the payload delivered with a grant is the payload of the request it is
delivered for) holds in both modes and in every phase.

## The naive control (the stimulus reaches the fail mode)

* On the DUT itself (`qos_en = 0`, throughput-first BULK > PREFETCH > SCALAR):
  the scalar dependent chain advances **0** links over 800 cycles while bulk is
  served **400** times — a sustained bulk stream starves the latency-critical
  chain.
* A second, independent model of the same machine (same depth, same service
  times, same downstream; only the rule differs) replays the stress mix's own
  offer stream: SCALAR 0 grants / 4 stuck, PREFETCH 0 grants / 4 stuck, BULK
  1200 grants. The registered policy serves all three classes.

## Mutants

House rule: the `-D` is asserted to occur in `rtl/core/mosaic_mem_qos.sv` before
the campaign; the build directory is deleted and rebuilt per mutant; the `-D` is
on that build's recorded command line; each mutant binary's sha256 differs from
the shipping binary's; the shipping build is rebuilt and run first from an empty
directory. Command: `python3 tools/run_qos_controls.py`.

Base: exit 0, sha256 `56751ac4e259a5e77a59c87107d34698ec8e53f9a22d6fd3bf770da4175fda84`,
`RESULT PASS … 112152 per-cycle checks over 6819 cycles`.

| mutant | exit | sha256 | first failing check | injects |
| --- | --- | --- | --- | --- |
| `MOSAIC_QOS_MUTANT_SCALAR_STARVE` | 1 | `c8a1b6f437cc51f4bf9edc86e5b54bdfa456432357e158311ba00412be94feec` | `directed:fifo: starvation: only 0 of 3 BULK requests were delivered while the class held pending work throughout; a class with continuous work can be starved for ever` | the bulk class is excluded from both the eligible and the aged set — scalar permanently takes the port (**card fail mode 1**) |
| `MOSAIC_QOS_MUTANT_BULK_MONOPOLY` | 1 | `b9c25e078f31fb516c3a4fba2f0409825a4705db78ed946a55a3cc21b1b61f7f` | `directed:fifo: cycle 13 (quota): quota: class BULK was granted while already at 2 grant(s) in this window, and the DUT did not report an age escape; the class's ceiling was not enforced …` | the bulk ceiling is removed from eligibility — bulk monopolises |
| `MOSAIC_QOS_MUTANT_NO_AGE_ESCAPE` | 1 | `46c63b56d840d75e40e7ac62b250bbe4d810263ccb98e6137e0a4c14cccb1d15` | `directed:fifo: cycle 225 (escape): escape: the port has been idle for 213 consecutive cycles with a request that has aged past the trigger pending; the age escape did not fire, so the request has no waiting bound` | nothing ever ages — the only over-quota path is gone, so a request waits for ever (caught by the bound, not a timeout) |
| `MOSAIC_QOS_MUTANT_DATA` | 1 | `1972ec8701cb2b4382c8524cd5d42dff5adb4f2b0019a2699694a98b9bc183ca` | `directed:fifo: cycle 9 (data): data: class BULK delivered tag 17 with payload 2862612644, the request with that tag was offered with payload 2862612481; a scheduling choice changed a data result` | the delivered payload is corrupted (**card fail mode 2**: a performance policy changes a result) |

What the first two are evidence for, in the card's terms: the naive-priority
control shows the *shipping stimulus* starves the scalar chain with the policy
off; `SCALAR_STARVE` shows the mirror fail mode — a policy that never admits the
streaming class — is caught by the reservation bound. `BULK_MONOPOLY` shows the
ceiling is load-bearing and its removal is caught by the quota check.

## What was reused from I-030, and what was added

**Reused** (I-030's bounded-service model, cited in the module header):
the windowed-quota discipline (`used[c] < Q[c]` eligibility, the W-th grant
wraps the window); the exactness argument that a class with pending work
throughout a window receives at least its quota; the per-class FIFO queue with
`req_ready` backpressure and no combinational path from the port back into the
producers; the checked downstream assumption and its sticky monitor; and the
shape of the declared waiting bound.

**Added**: the class is the request's identity and is computed by the module,
not handed a priority; the escape is **age-triggered** and is the *only*
over-quota path (a hard ceiling, at the cost of a bounded idle); the latency
measurement (a scalar dependent chain, p50/p99); and the policy-off/on
data-identity A/B.

`rtl/core/mosaic_arbiter.sv` is named in the case's registered source list; it is
compiled by the case but **not instantiated**, because I-030's engine is the
*completion-path* arbiter with a `localparam` geometry, and its
quota-exhaustion escape fires immediately — under it the age-triggered
starvation the card asks this package to demonstrate cannot occur. Re-instantiating
it here would be a decorative second service model, which the card explicitly
asks not to invent. The reuse is the model, and it is stated above.

## Gates and regression

| gate | result |
| --- | --- |
| `python3 tools/run_unit.py --profile p0 --case memory.qos_no_starvation` | PASS, exit 0, seed 1, 112152 per-cycle checks over 6819 cycles, shipping sha256 `56751ac4e259a5e77a59c87107d34698ec8e53f9a22d6fd3bf770da4175fda84` |
| `python3 tools/run_unit.py --profile p1 --case memory.qos_no_starvation` | PASS |
| `python3 tools/run_qos_controls.py` | all 4 mutants OK (exit 1, different binary, named first failure) |
| `python3 tools/lint_rtl.py --profile p0` / `--profile p1` | `ok rtl/core/mosaic_mem_qos.sv: clean as mosaic_mem_qos`; both `lint: 61 source file(s) clean` |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_mem_qos.sv` | exit 0 |
| `make lint-cpp` | `73 file(s) clean`; `tb_qos.cpp` compiles under `-Wall -Wextra` |
| `make check` | green: contracts p0–p3 OK, event contract ±negative OK, `check_records` `76 delivered package(s), 90 registered case(s)` agree, `check_exclusions` ±negative OK, `check_isolation` `PASS 11 isolation checks` |
| `core.act_dut` (p1) | PASS: `applicable=127 generated=127 run=127 passed=127 failed=0`, act_commit `96493a91448ca50780013fd892daec2c204487ba` |
| p0 regression set | `arbiter.forward_progress` PASS, `coalesce.element_faults` PASS, `rvv.memory_modes` PASS, `mmio.exactly_once` PASS, `core.mem_program` PASS, `core.corpus_sweep` PASS, `fabric.integrated` PASS |
| p1 regression | `cache.integrated_path` PASS |

## Not covered (honest list)

* **Not integrated into the core's memory path.** This is a module-level
  scheduler (`mosaic_mem_qos`) with its own case; `mosaic_core.sv`,
  `mosaic_lsu_endpoint.sv` and the vector packetizer are untouched. Wiring it
  between the LSU/vector requesters and the bank/LLB backpressure is I-056/I-061
  integration work and was not in this card's scope.
* **The numbers are a measurement of this geometry and this traffic mix**, not
  of the core: depth 4 per class, W = 8, Q = 4/2/2, AGE_LIMIT = 32, D_MAX = 4,
  a 1200- or 2400-cycle synthetic mix. They are a floor for the reservation's
  effect, not a core performance claim.
* **The prefetch class is a placeholder** until I-063 exists: PREFETCH requests
  are offered and served, but nothing in the tree produces a real prefetch yet.
* **No formal proof.** The reservation argument is written in the header and
  checked against the run's own `share_min`; it is not machine-checked, and the
  bound is a checked over-approximation rather than a theorem prover's output.
* **The `qos_en = 0` control is a DUT mode** (throughput-first), not a second
  reference model; it exists to make policy-off/on data identity and the
  starvation of the scalar chain observable in the same DUT.
* The idle the hard ceiling causes (see *The cost of a hard ceiling*) is
  measured but not optimised away.
