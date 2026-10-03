# I-076 — the PMU and event consistency: `pmu.trace_accounting`

Work package I-076 (`docs/implementation-plan.md` §10): *count per-hart retired
instructions, uOPs, useful elements, stall reasons, occupancy, remote traffic, WB
conflicts, replays and MPKI; align every counter against a hand-computable trace.
Pass: the counters reconcile with the trace and the sums and classification show
no under-counting and no double-counting, and it is stated explicitly whether
each counter is CSR-visible and whether a reference can compare it. Fail: using
vector IPC versus scalar IPC as a direct speed judgement, or presenting toggle
counts as measured power.*

Registered case `pmu.trace_accounting`, top `mosaic_core_tb`, driver
`sim/unit/tb_pmu.cpp`, profile **p1** (the profile whose map declares RAM
cacheable, so the L1D can be engaged for the MPKI half). Run from a deleted build
directory with

```
python3 tools/run_unit.py --profile p1 --case pmu.trace_accounting
```

**Verdict: PASS** (48 checks, 0 failures), from a deleted build directory
(`rm -rf build/p1/unit/pmu.trace_accounting`). The verbatim result line:

```
RESULT PASS pmu.trace_accounting counters reconcile with the hand trace
```

## What the case runs, and the hand derivation

One scalar RV64IM program with a fixed trip count (LOOPS = 6), run twice through
the same elaborated core: *cache_off* (cacheless — the endpoint observation
points) and *cache_on* (the L1D engaged — the cache counters and MPKI). The
measurement stops in the cycle the self-loop park first appears in the retire
stream; the committed counter has not yet counted that park at that edge, so the
hand total is the program **before** the park:

| instruction | count | class | per iteration |
|---|---|---|---|
| `la x10, DATA_A` | 2 | ALU | |
| `la x11, DATA_B` | 2 | ALU | |
| `addi x1, x0, 0` | 1 | ALU | |
| `addi x12, x0, 6` | 1 | ALU | |
| `ld x3, 0(x10)` | 1 | LOAD | ×6 |
| `add x1, x1, x3` | 1 | ALU | ×6 |
| `addi x1, x1, 1` | 1 | ALU | ×6 |
| `addi x12, x12, -1` | 1 | ALU | ×6 |
| `bne x12, x0, loop` | 1 | CTRL | ×6 |
| `sd x1, 0(x11)` | 1 | STORE | |
| `csrr x6, minstret` (A) | 1 | SYS | |
| `addi x7, x0, 0` | 1 | ALU | |
| `addi x7, x7, 1` | 1 | ALU | |
| `csrr x8, minstret` (B) | 1 | SYS | |
| `sd x6, 8(x11)` | 1 | STORE | |
| `sd x8, 16(x11)` | 1 | STORE | |
| `la x9, TOHOST` | 2 | ALU | |
| `addi x5, x0, 1` | 1 | ALU | |
| `sd x5, 0(x9)` | 1 | STORE | |
| `fence.i` | 1 | SYS | |
| **total before park** | **48** | | |

Hand arithmetic, all from the program text and the p1 geometry (64 B line, 32
sets, 4 ways) and nothing from the DUT:

```
retired instructions  18 + 5*6 = 48
ALU                   11 + 3*6 = 29
LOAD                  6        =  6
STORE                 4        =  4
CTRL                  6        =  6
SYS (2x csrr, fence.i)         =  3      (29+6+4+6+3 = 48)
register writes       ALU 29 + LOAD 6 + csrr 2 = 37
loads / stores        6 / 4
distinct L1D lines    DATA_A (0x80000800), DATA_B (0x80000900), TOHOST (0x80001000)
cache_on              txn 10, hits 7, misses 3, refills 3, writebacks 2
MPKI(L1D demand)      3 / 48 * 1000 = 62.50
```

The observed values agree exactly: `retired=48 stream=48`, classes
`ALU=29 LOAD=6 STORE=4 CTRL=6 SYS=3`, writes `37`, `dcache txn=10 hit=7 miss=3
refill=3 wb=2`, `lsu_txn=10`, MPKI `62.50`.

## The card's Action items, each against a counter and a check

`exists` = the observation point was already in the core's output list;
`added` = the I-076 counter wiring added it (all additive, no existing signal
removed). Every debug counter is labelled debug-only below.

| card Action item | counter / port | source | the check that reconciles it |
|---|---|---|---|
| per-hart retired instructions | `o_commit_ctr` (debug) | exists | `== 48` (hand) and `==` the retire-trace length, both phases |
| per-hart retired instructions (architectural) | `minstret` CSR, read by the program | exists (Zicntr) | each CSRR's value `==` that CSRR's index in the retire trace; the delta `==` the instructions between them |
| uOPs | `o_pmu_uop_insert_ctr` / `_issue` / `_kill` | added | `insert == issue + kill + live IQ occupancy` (mosaic_iq's exactly-once conservation) |
| classes of retired instructions | derived from the retire trace | exists (event stream) | `Σ classes == retired` and each class `==` its hand count (no under/double count) |
| useful elements | `o_vec_elem_ctr`, `o_lane_elem_ctr` | exists | present as ports; **not exercised** by this scalar program (stated, not claimed) |
| stall reasons | `o_fab_stall_ctr`, `o_fab_reason_ctr`, `o_dbg_barrier_ctr`, `o_mem_ins_stall` | exists | reported; the barrier and recovery cycles are reported alongside |
| occupancy | `o_dbg_disp_occ_sum`, `o_dbg_iq_occ_sum`, `o_dbg_iqueue_occ_sum` | exists | each `<= capacity × cycles`; mean depth `= sum / cycles` reported |
| remote traffic | `o_fab_grant_ctr` (local cross-cluster steering) | exists | reported; **explicitly not remote/coherence traffic** (see not-covered) |
| WB conflicts | `o_wb_collision_ctr`, plus `o_wb_wr_ctr` | exists | `o_wb_wr_ctr ==` the trace's retiring register-write count (37); conflicts reported |
| replays | `o_mem_lq_replay`, `o_dbg_recover_ctr` | exists | load-queue `done + occupied <= alloc`; replays/recovery reported (0 / 54 cycles) |
| MPKI | `o_pmu_dcache_miss_ctr` | added | `miss ==` the hand cold-line count (3); `refill == miss`; MPKI `= miss / retired × 1000` |
| cache decomposition | `o_pmu_dcache_txn_ctr` / `_hit` / `_refill` / `_wb` | added | `hit + miss == txn`, `refill == miss`, `wb ==` hand dirty lines |
| memory path | `o_mem_lq_alloc/done`, `o_mem_sq_alloc/drain/squash/occupied`, `o_mem_lsu_txn` | exists | `lq_alloc == done == 6`; `sq_alloc == drain + squash + occupied`, `drain == 4`; cache_off `lsu_txn == 10` |
| fetch | `o_dbg_fetch_req_ctr`, `o_dbg_fetch_rsp_ctr` | exists | cache_off: `fetch_req ==` the harness's own instruction-beat count |

## No under-counting, no double-counting: the identities the case asserts

Each is a sum-versus-parts or a conservation closure, so a lost event and a
counted-twice event each break it by their own count:

- **retire domain**: `o_commit_ctr == trace length == 48`; `Σ classes == retired`;
  each class `==` its hand value; `register writes == 37`.
- **uop domain**: `insert == issue + kill + live` (live = `c0_count + c1_count`).
- **store domain**: `sq_alloc == sq_drain + sq_squash + sq_occupied`, and
  `sq_drain == 4` (the retired stores).
- **load domain**: `lq_alloc == lq_done == 6`; `lq_done + lq_occupied <= lq_alloc`.
- **cache domain**: `hit + miss == txn`; `refill == miss`; `miss == 3`,
  `hit == 7`, `wb == 2`.
- **memory/fetch (cacheless)**: `lsu_txn == 10`; `fetch_req ==` the driver's beats
  (an independent measurement, not a DUT counter).
- **occupancy**: every cycle-integrated sum is `<= capacity × cycles`, so the
  mean depth is a real mean and not an over-count.

## CSR visibility and reference-comparability (the Pass clause)

The case prints and this report repeats, per counter, whether it is reachable
through a CSR and whether an ISA reference can reproduce it. Values below are the
cache_off phase's.

| counter | value | unit | CSR-visible | reference-comparable |
|---|---|---|---|---|
| `minstret` (read by the program via CSRR) | 37 | instructions | **yes** (`0xb02`/`0xc02`) | **yes** |
| `mcycle` / `cycle` | 225 | cycles | **yes** (`0xb00`/`0xc00`) | **yes** |
| `time` | 0 | cycles | yes (`0xc01`) | yes |
| Zihpm `hpmcounter3..31` | 0 | — | yes (`0xc03..0xc1f` decode) | no — read-only zero shadow, not a mapping of these events |
| retire trace: class ALU | 29 | instructions | no | **yes** (architectural event stream) |
| retire trace: classes LOAD/STORE/CTRL/SYS | 6/4/6/3 | instructions | no | **yes** |
| retire trace: retiring register writes | 37 | writes | no | yes |
| `o_commit_ctr` (debug retired count) | 48 | instructions | no | no (implementation) |
| `o_pmu_uop_insert_ctr` / `_issue` / `_kill` | 37 / 37 / 0 | uops | no | no |
| `o_pmu_dcache_txn/hit/miss/refill/wb` | 10/7/3/3/2 | requests/lines | no | no |
| `o_pmu_icache_miss_ctr` | 3 | requests | no | no |
| `o_wb_wr_ctr` (PRF writes) | 37 | writes | no | no |
| `o_wb_collision_ctr` | 0 | conflicts | no | no |
| `o_mem_lsu_txn` (cache_off) | 10 | transactions | no | no |
| `o_mem_lq_replay` | 0 | replays | no | no |
| `o_dbg_iq_occ_sum` | 98 | occupancy-cycles | no | no |
| `o_redirect_ctr` | 6 | redirects | no | no |

**The rule this report follows**: a counter that is not CSR-visible is never
presented as architectural evidence. Only `minstret`, `mcycle`/`cycle` and `time`
are architectural; every `o_pmu_*`, `o_dbg_*`, `o_wb_*`, `o_mem_*`, `o_loc_*` and
`o_fab_*` counter is implementation observability. The machine-readable mapping is
`config/pmu/p1.json` (schema `config/schema/pmu.schema.json`), which carries the
same `csr_visible` and `reference_comparable` flags per counter.

## A defect this case found and fixed: `minstret` under-counted dual retires

The first run of the case (before any fix) read `minstret` = 33 at the first CSRR
where 37 instructions had retired, and 36 where 40 had: a constant four short.
The cause was in the counter wiring, not the trace: `mosaic_core.sv` drove the
CSR file's increment input with `rob_retire_ack | rob_retire_ack_next` — an OR, so
a cycle that retired **two** macros advanced `minstret` by one. The retire trace
showed exactly four dual-retire cycles before the first CSRR.

This is the card's own failure mode (a counter under-counting a class of event)
and the wiring is I-076's to own, so it is fixed rather than documented around:

- `mosaic_csr.sv`: `cnt_instret_i` is now `logic [1:0]` (0, 1 or 2) and advances
  `minstret` by that count;
- `mosaic_core.sv`: the input is driven with
  `{1'b0, rob_retire_ack} + {1'b0, rob_retire_ack_next}`;
- `sim/tb/mosaic_csr_tb.sv`: the wrapper's matching input is widened (the case
  that drives it directly writes 0/1, unchanged).

After the fix the case requires `minstret` at each CSRR to equal that CSRR's index
in the retire trace, and the delta to equal the instructions between them — an
architectural, reference-reproducible check. `MOSAIC_PMU_MUTANT_DOUBLE_COMMIT`
below re-injects a counting error in the same domain to prove the case can fail.

## The negative controls

Run with `python3 tools/run_pmu_controls.py --profile p1`. Each control is built
from a **deleted** build directory with its `-D` in that build's command line; the
script requires a differing binary (`cmp` + sha256), exit status 1, and the
expected first-failure text. The shipping build's own binary is
`40b418bc9f9c6f3506c95fb8efbfadf33d24edc9282cc3b814bc5c0638fdd4a6`, and it passes
before either mutant runs.

| mutant | kind | binary sha256 | exit | first failure |
|---|---|---|---|---|
| `MOSAIC_PMU_MUTANT_DOUBLE_COMMIT` | rtl (`mosaic_core.sv`) | `22ad66ca5d987784e64e2aa827f3edaa0114716e849a83d95251a3f1e973cde1` | 1 | `CHECK FAILED: cache_off: retired counter 54 == hand 48` |
| `MOSAIC_PMU_MUTANT_SKIP_CLASS` | driver (`tb_pmu.cpp`) | `8bf2b37102a09369f33475ace4fd30650cf3bdfe9016f61e6ec549beaacdb8b2` | 1 | `CHECK FAILED: cache_off: class sum 42 == retired 48 (no under/double count)` |

- **double count** — every redirect adds one to the committed count, the shape of
  counting a squashed or replayed uop as retired. The retire-domain check sees 54
  where the hand trace says 48.
- **under count** — the classification drops the CTRL class, so the parts sum to
  42 and no longer add up to the whole. The classification identity catches it.

The shipping build passes before either mutant runs, so a control cannot pass by
the baseline being red.

## Not covered (explicit)

- **No vector-IPC-versus-scalar-IPC speed judgement** (the card's first named
  anti-pattern). This case counts retired work, uops and cache events; it never
  compares IPC, and no number here is a speed claim. Performance comparison is
  I-077/I-084's, on equal resources with one variable changed.
- **No toggle count presented as measured power** (the card's second named
  anti-pattern). No switching-activity counter is exported and none is reported
  as power.
- **Remote/coherence traffic is not observed.** The p0/p1 single-hart core has no
  remote link endpoint; the card's remote-traffic item is mapped to
  `o_fab_grant_ctr` (local cross-cluster steering) and said so. True remote
  traffic belongs to the I-029/I-030 fabric, not to this case.
- **Zihpm is not a mapping of these events.** The profile decodes `hpmcounter3..31`
  but this build reads them as zero; the event counters are debug-only and are
  labelled so.
- **Useful elements are not exercised.** `o_vec_elem_ctr` / `o_lane_elem_ctr` are
  present as ports; the p1 program is scalar, so the counter is reported, not
  claimed. FP counters (`o_fp_*`) are likewise out of this case's scope.
- **The p0 cacheless build's cache counters.** p0 claims no cache, so the case
  runs on p1 and every cache number is p1's geometry; a p0 run of this binary
  would report the cache phase as disengaged.

## Gates

| gate | result |
|---|---|
| `python3 tools/lint_rtl.py --profile p0` | 65/65 clean |
| `python3 tools/lint_rtl.py --profile p1` | 65/65 clean |
| `python3 tools/check_records.py` | ok: 83 delivered packages, 101 registered cases, all agree |
| `python3 tools/check_exclusions.py` | ok: 54 registered, 18 open, 36 covered; closure clean |
| `python3 tools/check_contracts.py --all` | contracts OK on p0, p1, p2, p3 |
| `csr.precise_trap_mret` (p0) | PASS |
| `core.trap_csr_program` (p0) | PASS |
| `core.act_dut` (p1) | PASS |
| `core.corpus_sweep` (p1) | PASS |
| `perf.equal_resource_compare` (p1) | PASS |
| `core.bringup_vs_reference` (p1) | PASS (the differential whose reference counts minstret per instruction) |
