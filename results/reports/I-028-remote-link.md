# I-028 — remote operand/result link

Work package **I-028** of `docs/implementation-plan.md`, implementing the
`CASE=remote.kill_with_delayed_response` obligation:

> 实现至少一拍 registered link 与返回路径，独立背压；运行
> CASE=remote.kill_with_delayed_response，注入 1/2/4/8 拍延迟。
> Pass: 随机背压下数据/credit 守恒；remote response 不写已复用的 home slot。
> Fail: request/response 共用有限资源形成循环等待，或假定永远一拍返回。

## STATUS: COMPLETE

| Acceptance item | Evidence |
| --- | --- |
| Verilator `-Wall` clean | `verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_remote_link` → exit 0; also clean with the wrapper as top. `tools/lint_rtl.py --profile p0` reports `ok rtl/core/mosaic_remote_link.sv: clean as mosaic_remote_link`; the project-wide run reports `1 of 27 source file(s) failed` and the failing file is **another lane's in-flight `rtl/core/mosaic_macro_desc.sv`**, not this package |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv')` | exit 0, **0 errors** project-wide (only STYLE-2 port-suffix warnings, which every `o_*` port in this project produces) |
| Yosys elaboration | `hierarchy -check -top mosaic_remote_link; proc; opt_expr; opt_clean; check -assert; memory -nomap; opt -full; select -assert-none t:$dlatch` → exit 0, no latches, no undriven/multiply-driven wires — see *Synthesis* |
| `python3 tools/run_unit.py --profile p0 --case remote.kill_with_delayed_response` | `RESULT PASS remote.kill_with_delayed_response … 20257 shadow comparisons over 20313 cycles`, **31 checks, 0 failures**, exit 0 |
| ≥1 registered hop each way, independently back-pressured | request and response are separate shift pipelines; `req_ready`/`rem_rsp_ready` depend on their own channel only — see *Two hops* and *Back-pressure* |
| Delay injection 1/2/4/8 works | phase `round-trip` sweeps all four and asserts the exact cycle of each hop — see *What the testbench checks* |
| Credits conserved every cycle, including kill and reset | `o_issued_ctr == o_returned_ctr + o_outstanding` and `o_outstanding == popcount(o_entry_valid)` checked every compared cycle, from the DUT's own ports |
| Late response rejected, not written | phase `kill-late` — the recycled slot keeps its new occupant and both `o_stale_rsp_ctr` and `o_alias_rsp_ctr` move |
| ≥4 mutants, exit 1, with a delta | **six**, each `+1` failure against a base of 0, each with a first mismatch naming its own defect — see *Mutants* |
| Card's fail criteria tested explicitly | *"共用有限资源形成循环等待"* → the directed check in `backpressure-response` plus `MOSAIC_REMOTE_LINK_MUTANT_SHARED_CHANNEL`; *"假定永远一拍返回"* → the 1/2/4/8 sweep plus `MOSAIC_REMOTE_LINK_MUTANT_ONE_CYCLE` |
| Report | this file |

## Files

| File | Contents |
| --- | --- |
| `rtl/core/mosaic_remote_link.sv` | `mosaic_remote_link`: pairing table, two pipelines, credit accounts |
| `sim/tb/mosaic_remote_link_tb.sv` | `mosaic_remote_link_tb`: straight pass-through wiring plus geometry read-back |
| `sim/unit/tb_remote.cpp` | Independent C++ shadow, per-cycle whole-state comparison, seven phases, coverage |
| `results/reports/I-028-remote-link.md` | This file |

`tests/unit/registry.json` was **not** edited: the case was already registered
(`top: mosaic_remote_link_tb`, `rtl: [rtl/core/mosaic_remote_link.sv]`,
`sv: [sim/tb/mosaic_remote_link_tb.sv]`, `cpp: [sim/unit/tb_remote.cpp]`,
`max_cycles 200000`, `seed 1`) and is used unchanged.

## Interface

No parameter has a default a caller could override into a second geometry:
every size is a `localparam` in the parameter list, read from a generated
package or derived from one.

| Name | Source | p0 |
| --- | --- | --- |
| `ENTRIES` | `mosaic_cfg_pkg::MOSAIC_IQ_ENTRIES` | 8 |
| `LATENCY` | `mosaic_cfg_pkg::MOSAIC_REMOTE_LATENCY`, clamped to ≥ 1 | 4 |
| `ID_W` | `mosaic_uop_pkg` `HART_W + ROB_INDEX_W + ROB_GEN_W + UOP_INDEX_W` (= `macro_id_t`) | 17 |
| `DST_W` | `TAG_W + GEN_W` | 15 |
| `OP_W` | `mosaic_pkg::alu_op_e` | 4 |
| `WORD_W` | `mosaic_uop_pkg::XLEN` | 64 |
| `LINK_ID_W` | `$clog2(ENTRIES)` | 3 |
| `REQ_BODY_W` / `REQ_W` | 18 + 15 + 4 + 3×64 / + `LINK_ID_W` | 228 / 231 |
| `RSP_W` | 17 + 15 + 64 + 1 + 3 | 100 |

Payload layout, low bits first, request: `{identity, destination, opcode,
immediate, src1, src2}` with the link id above all of it; response:
`{identity, destination, value, fault}` with the link id above. The DUT reports
every offset through `o_req_*_lo`/`o_rsp_*_lo` and phase `geometry` requires them
to equal the ones the driver packs, so the layout is a checked agreement rather
than a comment.

Both payloads carry identity + destination + value; nothing else about the
request (no PC, no route, no epoch). The link never interprets them.

## Design, in the terms the card asks for

**Pairing, and its bound.** Each accepted request takes one table entry and the
entry index is written into the request as its **link id**, which the remote
unit echoes. The response is accepted only when that link id names a *live*
entry whose stored identity and destination are exactly the ones the response
carries. The link id alone would alias the moment a slot is recycled, which the
lowest-free allocation makes the common case rather than a rare one; the
identity alone would need a search. Both together are O(1) and are what makes a
late response decidable from the link's own state — no knowledge of the core is
needed.

Bound: `ENTRIES = MOSAIC_IQ_ENTRIES`, the destination cluster's issue-queue
depth. A remote request occupies a slot there from acceptance until its result
leaves, so one home cluster cannot have more requests in flight to one remote
cluster than that queue holds. The table is therefore never the first resource
to run out.

**Credits.** One entry is one credit. Taken on acceptance (`o_issued_ctr`),
returned exactly once on release (`o_returned_ctr = o_matched_ctr +
o_killed_ctr`). The link maintains `o_issued_ctr == o_returned_ctr +
o_outstanding` as an invariant of every cycle, and `o_outstanding` is a
separate register from the occupancy bitmap so the two can be checked against
each other.

**Kill and flush.** `kill_valid` kills the one outstanding request with that
identity; `flush_valid` kills all of them. Both resolve against the pre-edge
table, so an entry freed this cycle is not offered to this cycle's allocation
(one cycle of extra back-pressure, in exchange for "released exactly once" being
structural). **A kill outranks a same-cycle response match**: when the two name
the same entry the response is dropped and counted and the kill returns the
credit, so the home side does not have to discard a result it has just declared
dead. A kill does not withdraw the request from the network — a packet in flight
cannot be recalled — so the remote unit still answers and the answer is dropped
as stale. That is what this case exercises.

**Two hops.** Each direction is its own shift pipeline of `LATENCY` stages with
a global stall: the whole pipe advances unless its output stage holds an item
the far side has not accepted. Consequences, both deliberate and both tested:
the delay is a property of the link rather than of the occupancy pattern (an
accepted request is offered to the remote exactly `LATENCY` cycles later), and
the ready path from the far end back to the near end is one gate deep instead of
a ripple through the pipe.

`MOSAIC_REMOTE_LATENCY` is really in the hardware: the pipelines are `LATENCY`
storage words each, and the mutant that forces one stage fails. The design
assumes a **minimum of one cycle per direction** and enforces it by clamping the
parameter to ≥ 1.

**No cycle between the channels.** The request and response channels share no
payload storage, no valid/ready signal and no finite resource other than the
credit table, and the wait-for chain runs one way only:

```
rsp_ready -> rsp_pipe -> match -> entry free -> req_ready -> req_pipe -> rem_req_ready
```

Nothing on the right feeds anything on the left, so under the standard
environment assumption (a stalled side eventually asserts ready; a side with
something to send eventually asserts valid) every stage terminates. A stalled
response path therefore *can* back-pressure the request path through the credit
table, which is exactly the "shared credit pool" the card allows when it cannot
deadlock — and it cannot, because the response path's progress does not depend
on the request path. `MOSAIC_REMOTE_LINK_MUTANT_SHARED_CHANNEL` injects the
forbidden arrangement instead (response storage gating request acceptance with
no causal chain), and the directed check below catches it.

## What the testbench checks

Seven phases, each resetting first, each owning one mechanism; the shadow is
compared **every cycle** — the entry bitmap, both stored identity fields per
entry, both pipeline validity vectors, every pipeline payload, and all twelve
counters — and the credit law is recomputed from the DUT's own ports.

| Phase | What it establishes |
| --- | --- |
| `geometry` | the elaborated sizes are the ones the driver assumes, the wire layout equals the offsets the DUT reports, and the wrapper's arithmetic equals the RTL's |
| `reset-state` | nothing live, nothing counted, the credit law holds, and idle cycles change nothing |
| `round-trip` (×4) | delays 1/2/4/8: the request reaches the remote port exactly `LATENCY` cycles after acceptance, the response reaches home exactly `delay + LATENCY` cycles after the remote took it, and the delivered value is the function of what the remote port received |
| `out-of-order` | four requests answered in reverse order are delivered in reverse order, each with its own identity and value — the link does not assume FIFO from the network |
| `kill-late` | the namesake, plus the same-cycle kill/transfer race, plus an unmatched kill |
| `backpressure-credits` / `-pipe` / `-response` | both bounds at their own limit, no loss, and a stalled response path never refusing a request a free credit could carry |
| `random` | 20000 cycles of random issue/kill/flush/back-pressure at random delays, then a bounded drain |

`kill-late` in detail — the case's namesake. Request **A** is issued and killed
two cycles after the remote took it, while its answer is still in flight; the
credit returns exactly once. Request **B**, with a *different* identity, is
issued immediately and takes the recycled slot (link id 0, asserted). A's
response then arrives while B is live: the DUT must consume it, refuse it, and
count it as an **alias** (`o_alias_rsp_ctr`, the subset of `o_stale_rsp_ctr`
where the link id named a live entry with the wrong identity), while delivering
nothing; B's own response must then arrive intact and uncorrupted. The phase
also asserts that its own schedule leaves B live when A's response lands, so the
test cannot silently degrade into the easier empty-slot case. Two more variants:
a kill asserted in the *same cycle* as the response transfer (kill wins, one
credit returned, nothing delivered afterwards), and a kill naming an identity
that is not in flight (counted, inert).

The directed check for the card's first fail criterion is in
`backpressure-response`: with the response pipeline saturated, `rsp_ready` low
and credits free, `req_ready` must be high and a request must be accepted. It
fails on a design that makes request acceptance depend on response storage, and
that is exactly what `…MUTANT_SHARED_CHANNEL` does.

## Real command output

```
$ python3 tools/run_unit.py --profile p0 --case remote.kill_with_delayed_response
PASS remote.kill_with_delayed_response task=I-028

$ tail -1 results/unit/remote.kill_with_delayed_response/run.log
RESULT PASS remote.kill_with_delayed_response remote link contract holds: 20257 shadow
comparisons over 20313 cycles, 8 credits, latency 4, seed 1; coverage: accepts=7354
matches=5982 kills=34 flushes=213 stale responses=1364 alias refusals=661
deliveries=5982 kill misses=763 request stalls=12748 response stalls=9204 table
full=6160 response path full=9204 out-of-order delivery=1 kill vs response race=6

$ python3 -c "import json;d=json.load(open('results/unit/remote.kill_with_delayed_response/result.json'));print(d['checks'],d['failures'])"
31 0
```

Both fail-criterion coverage counters are non-zero because the *directed* phases
produced them (`out-of-order delivery=1`, `kill vs response race=6`), not the
soak; `phase-coverage` requires every row to be non-zero, so a stimulus change
that stopped reaching a mechanism fails the case.

Other seeds, against the same binary:

```
seed 1 exit=0   seed 2 exit=0   seed 3 exit=0   seed 7 exit=0   seed 99 exit=0   seed 12345 exit=0
```

## Mutants

Each mutant rebuilds the case with one `-D` appended to
`run_unit.VERILATOR_FLAGS`, then runs the binary directly:

```python
import sys; sys.path.insert(0, 'tools'); import run_unit
run_unit.VERILATOR_FLAGS.append('-DMOSAIC_REMOTE_LINK_MUTANT_<NAME>')
run_unit.build_case('p0', CASE, run_unit.load_registry()['cases'][CASE])
```

```
build/p0/unit/remote.kill_with_delayed_response/remote.kill_with_delayed_response \
    --case remote.kill_with_delayed_response --out /tmp/mut-<NAME> --seed 1 --max-cycles 200000
```

| Mutant | Defect injected | exit | failures (base 0) | First mismatch |
| --- | --- | --- | --- | --- |
| `…_STALE_ACCEPT` | the entry's liveness is not consulted at all: any response is matched by index | 1 | +1 | `a response that matched no live entry was loaded into the response pipeline and would be delivered: {id=28480 … lid=0}` |
| `…_MATCH_ID_ONLY` | pairing on the link id alone, identity and destination ignored | 1 | +1 | same invariant: a recycled slot's late response is accepted |
| `…_ONE_CYCLE` | `PIPE_DEPTH` forced to 1 — the latency parameter ignored | 1 | +1 | `geometry: o_pipe_depth 1 != 4` |
| `…_NO_CREDIT_ON_KILL` | the entry is freed but the credit is not accounted as returned | 1 | +1 | `credit conservation broken: issued 1 != returned 0 + outstanding 0` |
| `…_SHARED_CHANNEL` | `req_ready` additionally requires the response pipeline to be empty | 1 | +1 | `req_ready: expected 1, got 0` (a parked result refused a request) |
| `…_ID_DROPPED` | the identity does not survive the response path | 1 | +1 | `a response that matched a live entry was not loaded into the response pipeline unchanged` |

Every `ifdef` body exists in the shipping source and changes a real expression
(`grep -n MOSAIC_LINK_MUT rtl/core/mosaic_remote_link.sv` shows each switch and
its use site); none of them is a no-op, and the base is rebuilt and re-run green
after the campaign so the results directory is left holding the shipping verdict.

`STALE_ACCEPT` and `MATCH_ID_ONLY` are caught by the same invariant, and that is
correct rather than a duplicate: they are two different defects (no liveness
check; no identity check) that both end in an unmatched response being written,
which is the single rule the invariant states.

## Synthesis

`make synth-generic` cannot elaborate the tree today: Yosys 0.69 rejects the
*generated* `build/p0/rtl/mosaic_id_pkg.svh` at its `macro_id_eq` function
(`syntax error, unexpected OP_LAND`) before any module is reached. That is a
tool limitation in a file this package does not own and must not edit, so the
synthesis evidence is scoped: stub packages carrying **the same numbers, read
out of the generated files**, replace the unparseable ones and the shipping
module source is elaborated unchanged otherwise.

```
$ yosys -q -p "read_verilog -sv /tmp/ys/stub_pkgs.sv /tmp/ys/mosaic_remote_link.sv;
              hierarchy -check -top mosaic_remote_link; proc; opt_expr; opt_clean;
              check -assert; memory -nomap; opt -full; select -assert-none t:\$dlatch"
exit 0   (check -assert clean, no $dlatch selected)
```

## Defects found while writing this case, all in the testbench

Recorded because they are the reason to distrust a green result that was never
red:

1. The harness's remote model released its held response on the *home*
   delivery handshake instead of the *remote* response handshake, so a stale
   response was held forever and B's response never arrived. Found by the
   `kill-late` phase failing on a design that was correct.
2. The observation struct read `rsp_payload` (the home-side output) where the
   invariant needed `rem_rsp_payload` (the remote-side input), which made the
   "an unmatched response is not written anywhere" check **vacuous** — it
   compared the wrong port and therefore always passed. Found by the
   `STALE_ACCEPT` mutant *not* producing the message it was written to produce.
   A check that cannot fail is worse than no check, and this one had already
   been run green.
3. The payload-stability check compared against a snapshot taken after the
   observation was overwritten, so it compared values from the wrong cycles.
4. The delay accounting in the remote model was one cycle off
   (`clk_->cycle()` after the edge rather than the index of the cycle driven),
   which made the phase assertion count 6 where the design does 5. The
   *testbench* was wrong: the design's `LATENCY`-cycles-per-direction behaviour
   was correct throughout.

One design decision was also changed because of a test: the request pipeline
holds `LATENCY` items, so with the far end accepting nothing the link takes
`LATENCY` credits and not `ENTRIES`. The first version of the saturation phase
asserted `ENTRIES`; the design is right (a registered hop has that property) and
the phase was split to assert each bound at its own limit, with the bound now
stated in the RTL header.

## What is not verified

- **No remote functional unit and no network.** The far side is a testbench
  model with a selectable fixed delay and a one-at-a-time response port. The
  link has never been connected to a real cluster's issue queue, a router, or a
  second link, and multi-hop/pod behaviour is not exercised.
- **The identity-uniqueness assumption is not testable here.** A re-issued uop
  that kept its identity would be indistinguishable from the killed one whose
  response is still in flight; the RTL header states the assumption and that the
  generation is the home side's to bump. The case tests the card's own scenario
  (a *different* identity for the new request).
- **Liveness is argued structurally and tested, not proved.** The no-cycle
  argument is a statement about the wait-for chain; the tests show a bounded
  drain once the environment stops stalling. A peer that never asserts `ready`
  is outside the model, and no formal property was written.
- **Reset with the far end still driving is not covered.** Every reset in the
  case drives `rem_rsp_valid` low; a response arriving during the reset cycle
  itself is not exercised.
- **Non-power-of-two `ENTRIES` and `MOSAIC_REMOTE_LATENCY = 0` are not
  exercised.** p0 has 8 entries and latency 4; the range-check path and the
  ≥ 1 clamp exist and are guarded at elaboration, but no profile drives them.
- **32-bit counters are not proved non-wrapping in general.** They cannot wrap
  within the runner's 200000-cycle budget, which is the bound claimed.
- **A kill arriving after its response has already been matched** is counted as
  a miss and the result is still delivered; the case asserts that behaviour
  (phase `kill-miss`) but does not claim the link is the last line of defence —
  the writeback path's own discard rule is the consumer's.
- **No route selection.** One link instance is one home/remote pair; there is no
  route field and no steering, which belongs to I-029.
