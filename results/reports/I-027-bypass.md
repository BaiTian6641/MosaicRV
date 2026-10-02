# I-027 — the local bypass fast path

Work package **I-027** of `docs/stage-2-execution-fabric.md`, implementing

> 实现 local bypass 快路径 — 限定连线范围；bypass 保留 producer identity，
> miss/冲突回退 PRF；运行 `CASE=bypass.local_raw_chain`.
> Pass: 开关 bypass trace 一致；连续 RAW chain 得到正确值且 reported latency
> 来自 measured cycles。

## STATUS: COMPLETE

| Acceptance item | Evidence |
| --- | --- |
| The mechanism exists as RTL with its wiring bound and fallback rule in the header | `rtl/core/mosaic_cluster_bypass.sv` (new) |
| A consecutive RAW chain, 32 links, gets the correct values | the chain's value model is asserted per link (`uop j` must observe `0x1000+j`) and the run passes |
| Bypass on vs off: the architectural event streams agree | 66 events compared field by field, twice (uncontended and contended): same uop, same source identity, same value, same order |
| The reported latency comes from measured cycles | `span=32` vs `span=64` (uncontended) and `span=32` vs `span=235` (contended) cycles over 32 links, taken from the run |
| The measured count is lower with bypass on | yes: 1.000 vs 2.000 and 1.000 vs 7.343 cycles/link (a different design could have shown no gain; see the finding note below) |
| Fail criterion — borrowing identity/timing to reach a single-cycle bypass | `MOSAIC_BYPASS_MUTANT_EARLY_TAP`, exit 1 |
| Fail criterion — a combinational ready loop | prevented by construction, argued below; **not** claimed as a mutant |
| `python3 tools/run_unit.py --profile p0 --case bypass.local_raw_chain` | `RESULT PASS … 1593 per-cycle comparisons over 1645 cycles … seed 1`, exit 0, sha256 `f21936db65591ca3` |
| The revision this was measured against | git HEAD `2197dfd2c9d9e474bf6511516385a2eb13a6d13d` plus the working tree of the day (the I-038 lane's uncommitted edits to `mosaic_core.sv`, `mosaic_core_tb.sv`, `mosaic_uop_pkg.sv` and the memory modules). See *The revision measured* |
| The other cases the brief names | all re-run on that revision and PASS: `fabric.fixed_two_cluster`, `core.unwritten_reg_read`, `core.corpus_branch`, `core.mem_program`, `core.trap_csr_program`, `core.corpus_sweep`, `wb.same_bank_many_producers`, `iq.wakeup_insert_select` |
| `python3 tools/lint_rtl.py --profile p0` | `lint: 34 source file(s) clean` (includes the new module) |
| `python3 tools/check_records.py` | green: `37 delivered package(s), 49 registered case(s), every claimed case exists and belongs to the package claiming it` |
| `tests/unit/registry.json` untouched | the entry named three sources that did not exist; all three were created at exactly the registered paths (see *The registry entry*) |
| This report | — |

The case passes on the first run of the final driver; the mutants below are what
makes that pass meaningful.

## The registry entry

`tests/unit/registry.json` already carried the case, naming

| field | value | state found |
| --- | --- | --- |
| `top` | `mosaic_cluster_bypass_tb` | — |
| `rtl` | `rtl/core/mosaic_cluster_bypass.sv` | **missing** |
| `sv` | `sim/tb/mosaic_cluster_bypass_tb.sv` | **missing** |
| `cpp` | `sim/unit/tb_bypass.cpp` | **missing** |

The three sources were missing and were created at exactly those paths. The
registry was **not** edited, and the topology it names is the topology that was
built: a standalone module, its wrapper, and this driver. The card says the
mechanism lives "in `rtl/core/mosaic_cluster.sv` **and/or** a new module of
yours"; the registered topology picks the second, so the mechanism is a new
module and the integrated wiring is described (not performed) under *Not
covered*. `grep mosaic_cluster_bypass tests/unit/registry.json` matches this one
entry only.

## The mechanism, as a rule

`mosaic_cluster_bypass` taps the cluster's **local functional-unit result in the
cycle it is computed** — before the cluster's result register — registers it
once into a single-entry slot, and resolves the operands of the uop the cluster
is about to issue from that slot *or*, on any miss, from the register file's
value-visible wakeup.

1. **Capture.** At an edge, the producer enters the slot iff the bypass is armed
   (`bp_en`), a producer is present (`p_valid`), its macro is **authorised**
   (`p_authorised` — the ROB identity is live), and no redirect is in flight
   (`flush`). A redirect clears the slot; disarming clears the slot.
2. **Match.** A source operand is served by the bypass iff the bypass is armed,
   the slot is valid, the candidate declares the operand outstanding
   (`c_sN_need`), and **both** the tag and the generation equal the slot's.
3. **Precedence.** For the same identity the bypass wins: the value is already
   local, so the durable broadcast is not waited for.
4. **Fallback.** For every outstanding operand the bypass does not serve,
   `fb_sN_sel` is asserted and the operand is resolved from the durable broadcast
   `w_*` (matched on the same `(tag, generation)`). A miss **never** stalls.
5. **Identity.** The slot carries `{tag, gen, rob_index, rob_gen, uop_index}` and
   exports them, so a consumer sees *whose* value it has, not only which physical
   register it will eventually land in.
6. **No wakeup rule is weakened.** The module publishes no broadcast of its own.
   Every producer still becomes durable and raises exactly one value-visible
   wakeup on I-026's terms; the bypass only decides whether one identified
   consumer may use the value earlier. The slot is deliberately *lossy* (a new
   producer overwrites it, a redirect clears it) and that is safe precisely
   because the durable path carries every result independently.

## The wiring bound

Stated in the file header and fixed by the port list:

* **one producer port** — this cluster's own result stage; no cross-cluster path,
  no CAM, no second producer;
* **one bypass slot** — one tag/generation/ROB identity and one value;
* **one candidate consumer** — the cluster's single grant port, two source
  operands, no per-entry array;
* **three inputs of the environment** — the redirect, the durable broadcast, and
  the arm bit; no credit, no lease, no arbitration request.

Where it plugs in: the tap is the cluster's result stage (`mosaic_cluster.sv`,
the `wb_ev_q` capture), the candidate port is the uop at the cluster's grant
port (the ready/val view the issue queue already computes), and the fallback is
the `wu_*` broadcast the cluster already forwards into `mosaic_iq`. Nothing in
the module reaches outside one cluster.

## The durable path it is measured against

The driver does not invent a latency. The integrated core has two register
boundaries on the way from a local result to a woken consumer, and the harness
models exactly those:

| boundary | where | cycle |
| --- | --- | --- |
| the FU computes the result | `mosaic_cluster.sv`, combinational ALU | N |
| the cluster's result register takes it | `mosaic_cluster.sv`, `wb_ev_q`, edge-written | N+1 |
| the writeback arbiter's per-producer pending register takes it | `mosaic_wb_arbiter.sv`, `pend_v`/`pend_ev`, edge-written | N+1 |
| publication: PRF write + `wu_valid` | `mosaic_wb_arbiter.sv`, one producer per cycle | N+2 |
| the issue queue applies the wakeup combinationally | `mosaic_iq.sv`, `wu_hit1`/`wu_hit2` | N+2 |

So a consecutive RAW chain through the register file runs at **two cycles per
link** with the durable path idle, and the bypass — tapping at N and registering
once — makes it **one**. The run, not this table, is the evidence.

## The measured chain

Thirty-two consecutive RAW links. Uop *j*'s source is uop *j−1*'s destination;
uop 0's source is already in the queue. Each uop computes `src + 1`, and the
driver requires uop *j* to observe `0x1000 + j` independently of the DUT. The
chain's cycle count is the span between the first and the last grant **recorded
from the run**, so every reported number is measured:

```
  [raw-chain] armed=1 links=32 span=32 cycles per-link=1.000 (bypass=32 durable=0 root=1)
  [raw-chain] armed=0 links=32 span=64 cycles per-link=2.000 (bypass=0 durable=32 root=1)
  [raw-chain-contended] armed=1 links=32 span=32 cycles per-link=1.000 (bypass=32 durable=0 root=1)
  [raw-chain-contended] armed=0 links=32 span=235 cycles per-link=7.343 (bypass=0 durable=32 root=1)
  [latency] uncontended armed=1.000 disarmed=2.000 cycles/link; contended armed=1.000 disarmed=7.343 cycles/link
```

| run | links | span (cycles) | cycles/link (measured) | resolved by bypass | resolved by PRF |
| --- | --- | --- | --- | --- | --- |
| uncontended, bypass armed | 32 | 32 | **1.000** | 32 | 0 |
| uncontended, bypass disarmed | 32 | 64 | **2.000** | 0 | 32 |
| contended, bypass armed | 32 | 32 | **1.000** | 32 | 0 |
| contended, bypass disarmed | 32 | 235 | **7.343** | 0 | 32 |

The contended runs share the durable path with another cluster's results (one
competing producer per cycle, offered while up to 8 of its results are
outstanding — that cluster's own result register, so it stalls rather than
filling an unbounded queue). The disarmed chain then waits behind that traffic:
the fallback is a *throughput* limit, not only a fixed latency.

**What these numbers are a measurement of.** Each figure above is *cycles per
link in a 32-link consecutive RAW chain in one execution cluster, at this
geometry* (XLEN 64, 7-bit PRF tags, 8-bit PRF generations, 6-bit ROB index,
7-bit ROB generation), *with the durable path carrying one publication per cycle,
and — in the contended runs — one further producer per cycle from another cluster
while up to eight of that cluster's results are outstanding*. They are not a
claim about clock frequency, about a different geometry, about a chain
interleaved with independent uops, or about a machine under any other
backpressure. A figure that travels without those qualifiers would be exactly the
kind of number this project refuses to inherit from a document.

Both cells are asserted as facts about the run, not as limitations of the case:
the armed chain **must** show 32 bypass resolutions and the disarmed chain **must**
show 32 register-file ones, or the run fails. If the armed chain had not been
faster, the driver would print `FINDING: the bypass was NOT faster …` and still
pass, because that would be a property of the design rather than a defect of the
case; it does not arise here.

The measured number is a measurement, not "whatever the DUT did": the run's
timeline is the contract model's, and every cycle is compared against the DUT, so
a DUT that resolved an operand in a different cycle than the run does fails *at
that cycle* (which is exactly how `NO_FALLBACK` is caught) instead of silently
producing a different cycle count.

## The on/off trace comparison

The armed and disarmed runs are compared event by event. An event is
`(uop index, source tag, source generation, value observed)`; the *resolution
path* is recorded but deliberately excluded from the comparison, because
changing it is the entire point of the switch.

| run pair | events | architectural content | resolution path |
| --- | --- | --- | --- |
| uncontended | 33 vs 33 | identical | 32 bypass + 1 root vs 32 PRF + 1 root |
| contended | 33 vs 33 | identical | 32 bypass + 1 root vs 32 PRF + 1 root |

The driver fails on the first differing event, and additionally requires that the
two runs did *not* take the same path (a switch that changed nothing would
otherwise pass vacuously).

## The card's fail criterion

> 为追求单周期绕过 credit/identity 或形成组合 ready loop.

* **Borrowing identity/timing for a single-cycle bypass.**
  `MOSAIC_BYPASS_MUTANT_EARLY_TAP` points the match at the producer's
  combinational result instead of the registered slot: a consumer can then be
  woken in the producer's own execution cycle. It exits 1 at
  `registered-budget: cycle 8: bp_s1_hit: expected 0, got 1`.
* **A combinational ready loop is prevented by construction, not by a check.**
  The match reads only (a) the registered slot, (b) the candidate's inputs and
  (c) the durable broadcast, which is an input. It never reads the producer's
  result in the cycle it is computed, and no output of the module is ever fed
  back into one of its own inputs. The loop the card names —
  *consumer ready → FU issue → FU result → consumer ready* — needs a path from
  `p_*` to `sN_rdy` and back to `c_*`; this module has neither, so the loop
  cannot close **through this module**, and the register is what makes that
  structural. This is stated rather than demonstrated because the loop would need
  the FU and the issue queue as well: the mutant that removes the register
  (`EARLY_TAP`) shows the timing rule being broken, but it cannot exhibit a loop
  inside a two-input module.

## Mutants

House rule, all four: the `ifdef` name is asserted to occur exactly once in
`rtl/core/mosaic_cluster_bypass.sv` before the build; the build directory was
deleted once before the campaign and `tools/run_unit.py` wipes it again whenever
the command changes (which appending a `-D` does — checked in each build's
`build_command.txt`); every mutant binary's sha256 differs from the shipping
binary's; the shipping binary was rebuilt afterwards and its hash reproduced.

Base: exit 0, sha256 `f21936db65591ca3395124f3baafb3dc47c7157c295d4086f04e9eea0d5d68ec`,
`RESULT PASS … 1593 per-cycle comparisons over 1645 cycles … seed 1`.

| mutant | exit | sha256 (16) | first failing check | injects |
| --- | --- | --- | --- | --- |
| `MOSAIC_BYPASS_MUTANT_UNAUTH_FORWARD` | 1 | `a083bf2f5de13d91` | `unauthorised: cycle 24: o_slot_captured: expected 0, got 1` | capture ignores `p_authorised` |
| `MOSAIC_BYPASS_MUTANT_TAG_ONLY` | 1 | `d5e80ae7cb13e097` | `identity-reject: cycle 17: bp_s1_hit: expected 0, got 1` | the match compares the tag and not the generation |
| `MOSAIC_BYPASS_MUTANT_NO_FALLBACK` | 1 | `552c74bd40e9ea41` | `unauthorised: cycle 26: s1_rdy (bypass or register-file fallback): expected 1, got 0` | the durable term is dropped from `sN_rdy`, so a miss is never resolved |
| `MOSAIC_BYPASS_MUTANT_EARLY_TAP` | 1 | `9407aa2704577c6a` | `registered-budget: cycle 8: bp_s1_hit: expected 0, got 1` | the match reads the combinational producer, not the registered slot |

Each mutant's first failure is on the rule it injects, and none passes
identically to the shipping build, so none is a redundancy probe.

What each control is evidence for, in the card's terms:

* **Unauthorised forward** — "a bypass that forwards a value whose producer has
  not yet been authorised/committed in a way the consumer can observe". The
  identity discipline is the guard: the driver offers a present-but-withdrawn
  grant and the shipping design never captures it (`o_slot_captured` 0), leaves
  the slot invalid, counts it (`o_unauth_ctr`), and resolves the consumer through
  the register file instead. The mutant captures it and is caught on the capture
  itself.
* **Identity confusion** — "a bypass that fires for a different tag with the same
  numeric index". The driver presents a consumer whose tag matches the slot at
  another generation; the shipping design rejects it and counts the rejection
  separately (`o_id_reject_ctr`), and the mutant fires.
* **No fallback** — "a bypass whose fallback is removed so a miss stalls for ever
  rather than reading the PRF". The mutant's consumer never becomes ready on a
  miss; it is caught on the `sN_rdy` comparison at the first miss.
* **Early tap** — above.

## Coverage

Twelve phases, each resetting first; every cycle in every phase compares the
DUT's whole output surface against the shadow — the thirteen combinational
fields, the slot's five identity fields while the entry is valid (before *and*
after the edge), and the five counters — and the whole run is **1593 compared
cycles over 1645 total cycles**.

| phase | what it witnesses |
| --- | --- |
| `geometry` | the identity widths the driver drives are the DUT's own |
| `registered-budget` | a producer's own cycle cannot serve a consumer; the next cycle it does; a producer arriving in the same cycle does not disturb the value being read; the slot carries the full identity |
| `identity-reject` | tag-but-other-generation and other-tag are rejected, exactly one identity rejection is counted, and the exact identity is still served (positive control) |
| `unauthorised` | a withdrawn grant is not captured, not forwarded, is counted, and the register-file path resolves its consumer |
| `flush-cancel` | a redirect clears the slot, a value does not survive its producer's squash, and a producer is not captured against a redirect |
| `enable-off` | disarmed serves nothing and empties the slot; re-arming does not resurrect a pre-disable value; the mechanism works again after re-arming |
| `two-source-conflict` | one slot, two operands: the held one is bypassed, the other selects the register file, a mixed cycle reports the right source per operand, and the bypass defers the durable broadcast for an identity it already has |
| `raw-chain` | the measured chain, armed and disarmed, uncontended |
| `raw-chain-contended` | the same with the durable path shared |
| `trace-agreement` | the two runs' architectural event streams are identical |
| `random` | 600 seeded cycles over a four-value generation space (so identity aliasing is constant), with a quarter of the cycles forced to be mixed-path; coverage of hit, miss, identity rejection, unauthorised, flush and mixed paths is **required** of the run, not hoped for. Witnessed: 128 hits, 644 misses, 85 identity rejections, 50 unauthorised producers, 24 flush kills |
| `determinism` | the same seed reproduces the run byte for byte (hash `0x27fd016329b73a62` over every output and counter), after asserting the counters are zero following a reset |

## Gates

All of the following were run on the revision recorded under *The revision
measured*, after the I-038 lane landed the core interface in one piece.

| gate | result |
| --- | --- |
| `python3 tools/lint_rtl.py --profile p0` | green — `lint: 34 source file(s) clean`, including `ok rtl/core/mosaic_cluster_bypass.sv: clean as mosaic_cluster_bypass` |
| `python3 tools/check_records.py` | green — `records agree: 37 delivered package(s), 49 registered case(s), every claimed case exists and belongs to the package claiming it` |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | green — exit 0, no errors (it was briefly red on duplicate `rob_head_id`/`rob_boundary_ok` declarations in `mosaic_core.sv` while the I-038 lane's edit was in flight; that was never an I-027 file, and `slang-tidy … rtl/core/mosaic_cluster_bypass.sv` alone was clean throughout) |
| `bypass.local_raw_chain` | PASS, exit 0 |
| `wb.same_bank_many_producers` | PASS |
| `iq.wakeup_insert_select` | PASS |
| `fabric.fixed_two_cluster` | PASS |
| `core.unwritten_reg_read` | PASS |
| `core.corpus_branch` | PASS |
| `core.mem_program` | PASS |
| `core.trap_csr_program` | PASS |
| `core.corpus_sweep` | PASS (13 programs × 3 inputs) |

The six core cases were re-run **after** the I-038 lane connected its new
observation outputs in `sim/tb/mosaic_core_tb.sv`; before that they failed at
elaboration (`PINMISSING` is fatal in this tree), and that was recorded here
rather than left implied-green.

## The revision measured

| item | value |
| --- | --- |
| git HEAD | `2197dfd2c9d9e474bf6511516385a2eb13a6d13d` |
| working tree | HEAD plus the other lanes' uncommitted edits of that day: `rtl/core/mosaic_core.sv`, `sim/tb/mosaic_core_tb.sv`, `rtl/core/mosaic_uop_pkg.sv`, `rtl/core/mosaic_store_queue.sv`, `rtl/core/mosaic_load_queue.sv`, `rtl/core/mosaic_lsu_endpoint.sv` and their testbenches (the I-038 MMIO/observation work) |
| bypass binary sha256 | `f21936db65591ca3395124f3baafb3dc47c7157c295d4086f04e9eea0d5d68ec` |
| re-run on that revision | the four chain lines, the reported latency and the binary hash reproduced **exactly** after the I-038 lane's fix landed |

The measured cycle counts are only meaningful if the rest of the machine is the
one they were measured on, so: the bypass case compiles **only**
`rtl/core/mosaic_cluster_bypass.sv`, `sim/tb/mosaic_cluster_bypass_tb.sv` and
`sim/unit/tb_bypass.cpp`, plus the shared harness support
(`sim/common/sim_common.cpp`) that the runner adds to every case — the core is
not in its source list at all — so its numbers are a property of this module and
this geometry and cannot move with the MMIO wiring. They were nevertheless re-run
on the post-fix revision and are identical. If the core's timing changes later (a
different durable-path shape, a different geometry), the figures above must be
re-measured rather than reused, because the durable path's two register
boundaries are exactly what the disarmed numbers measure.

## Commands

```sh
# shipping base, through the official runner
python3 tools/run_unit.py --profile p0 --case bypass.local_raw_chain

# build + run one mutant (the extra -D makes the runner wipe and rebuild)
python3 - <<'PY'
import sys; sys.path.insert(0, 'tools'); import run_unit
run_unit.VERILATOR_FLAGS.append('-DMOSAIC_BYPASS_MUTANT_TAG_ONLY')
run_unit.build_case('p0', 'bypass.local_raw_chain',
                    run_unit.load_registry()['cases']['bypass.local_raw_chain'])
PY
build/p0/unit/bypass.local_raw_chain/bypass.local_raw_chain \
    --case bypass.local_raw_chain --out /tmp/bypass_mut_tag_only --seed 1 --max-cycles 200000; echo $?

# the other cases the brief names (all PASS on the recorded revision)
for c in fabric.fixed_two_cluster core.unwritten_reg_read core.corpus_branch \
         core.mem_program core.trap_csr_program core.corpus_sweep \
         wb.same_bank_many_producers iq.wakeup_insert_select; do
    python3 tools/run_unit.py --profile p0 --case "$c"
done

# gates
python3 tools/lint_rtl.py --profile p0
python3 tools/check_records.py
slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)
```

## Files

| file | change |
| --- | --- |
| `rtl/core/mosaic_cluster_bypass.sv` | **new** — the mechanism, its wiring bound, its fallback rule, the no-loop argument and the mutant list in the header |
| `sim/tb/mosaic_cluster_bypass_tb.sv` | **new** — the wrapper: every pin flattened to a fixed-width vector, geometry exported from the packages |
| `sim/unit/tb_bypass.cpp` | **new** — the driver: the environment, the shadow, the measured chains, twelve phases |
| `results/reports/I-027-bypass.md` | this file |

No existing file was modified. `docs/implementation-plan.md` and
`tests/unit/registry.json` are untouched (the card was already registered).

## Not verified

* **Not wired into the live cluster.** The registry names a standalone case, so
  the module is verified on its own; the integrated path (`mosaic_cluster.sv`
  instantiating it and `mosaic_iq.sv` taking a second, earlier wakeup source) is
  described above but not exercised. The integration must also fix what this case
  cannot: the tap must be driven only for a result the cluster really has (a
  value-writing uop that did not fault), because the module takes `p_valid` at
  face value; and the issue queue must ignore the later durable broadcast for an
  operand already resolved from the bypass — that "already ready, ignore the
  duplicate" rule is I-022's, and this case does not check it.
* **The durable path is a model, not the elaborated arbiter and PRF.** Its shape
  is the two register boundaries documented above, read out of
  `mosaic_cluster.sv` and `mosaic_wb_arbiter.sv`; the arbiter and register file
  themselves are I-026's and I-015's evidence. The *relative* gain is therefore
  this case's claim; the absolute durable latency is not.
* **The producer is never backpressured.** The harness always accepts a produced
  result (`wb_ready` is not modelled), so the armed chain's 1.000 cycles/link is
  the gain at the *consumer* end only. With a full result register the producer
  would stall on the durable path and even a bypassed chain would be gated by its
  throughput — a real limit of the mechanism that this case does not measure.
* **The competing producer is bounded, not calibrated.** It offers one result per
  cycle while up to 8 of its own are outstanding. The contended figure is a
  throughput limit of a shared one-publication-per-cycle path, not a calibrated
  system cycle count.
* **The bypass is not exercised under issue backpressure.** The candidate is
  always taken when the DUT says it is ready; a cycle in which the issue queue
  presents a candidate it then does not select is not modelled. The module holds
  no state that depends on selection, so the exposure is the integration's, not
  this module's — but it is untested here.
* **Can it starve the PRF path?** No for the writes: the driver pushes exactly
  one durable publication per produced result and serves them in order, so every
  producer still becomes durable and raises one wakeup — the bypass changes only
  whether a local consumer waits. What a bypassing consumer *skips* is the read
  side: it never reads the register file for that operand. That is the intended
  optimisation, and it means the PRF read path is exercised only by misses (32 of
  32 links in the disarmed runs, 0 in the armed runs). A design that bypassed
  *everything* and had no fallback would be untestable by construction — which is
  why `NO_FALLBACK` exists as a control.
* **Only a single-cycle producer.** The ALU's combinational result is the tap
  exercised. The module is latency-agnostic (it captures whenever `p_valid`), but
  a late MUL/DIV result is not covered.
* **Only p0, one slot, one seed.** Four identity widths, one bypass slot, one
  producer, one candidate per cycle; `seed 1`, as registered. Cross-cluster or
  multiple-slot bypass is out of scope (`fabric.*`).
* **No timing, area, synthesis or Fmax claim.**
