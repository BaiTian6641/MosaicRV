# I-084 — the equal-resource comparison: `perf.equal_resource_compare`

Work package I-084 (`docs/implementation-plan.md` §3.1): *change exactly one
variable per measurement — a scheduling, locality or fusion variable — and report
a legal signature, the actual work rate, and the per-workload result including
negative ones; pure IPC is not a signoff.*

Registered case `perf.equal_resource_compare`, top `mosaic_core_tb`, driver
`sim/unit/tb_core_perf.cpp`, profile **p1** (the profile whose platform map
declares RAM cacheable, so the locality structures can act). Run with

```
python3 tools/run_unit.py --profile p1 --case perf.equal_resource_compare
```

**Verdict: PASS**, 132 checks, from a deleted build directory. The three negative
controls each exit 1 with a named first failure and a binary hash different from
the shipping build (table at the end). The verbatim result line:

```
RESULT PASS perf.equal_resource_compare workloads=3 configs=7
       baseline_cycles=2389/3088/466
       resources=rob=64 iq=8 prf=96 banks=4 clusters=2 l1=32B/8sets/1way
                 lanes=8 reset=8 vlenb=16 arch_identical=yes
```

## The headline: equal resources, asserted and controlled

The card's fail mode is a comparison that secretly changes a resource count to
make the dynamic configuration look better. So the harness **asserts, per cell**,
that the resource bundle read back from the DUT is byte-identical across all
twenty-one cells (three workloads × seven configurations):

| resource | read from | shipping value (p1) |
|---|---|---|
| ROB entries | `o_geom_rob_entries_o` | 64 |
| issue-queue entries | `o_geom_iq_entries_o` | 8 |
| PRF entries / banks | `o_geom_prf_entries_o` / `o_geom_prf_banks_o` | 96 / 4 |
| clusters | `o_geom_clusters_o` | 2 |
| L1 line / sets / ways | `o_geom_cache_line_bytes_o` / `o_geom_cache_sets_o` / `o_geom_cache_ways_o` | 32 B / 8 / 1 |
| lane budget (max / reset) | `o_lane_quota_max_o` / `o_lane_quota_reset_o` | 8 / 8 |
| vector register width | `o_vec_vlenb_o` | 16 B |

The numbers are **not restated by the harness**. They are the core's own
parameters, exported as evidence and single-sourced in `mosaic_core.sv` (the L1
line/set geometry is one localparam used by both cache-path instantiations; the
lane budget is read from the broker's own ports). The harness compares the
bundle every cell against the baseline's and fails if any field moves. A
configuration that changed a resource count is therefore not a configuration —
it is a different machine, and the comparison refuses it.

This check is **falsifiable, not merely present**: the control
`MOSAIC_PERF_MUTANT_RESOURCE_DRIFT` drifts the bundle the harness reads for every
configuration after the first (ROB 64 → 80, lane budget 8 → 12) and the run
exits 1 on `the resource bundle of +steering on alu_chain` — see the controls
table. Because all seven configurations run on one elaborated core, the drift is
injected at the one place a real geometry change could enter the comparison: the
harness's read-back.

## What each configuration changes — exactly one variable

| configuration | the one switch it flips | what that switch gates |
|---|---|---|
| `baseline` | every policy switch off | no steering, no LLB, no prefetcher, coalescing disabled, lane share at reset |
| `+steering` | `fab_dyn_i = 1` | I-090's one runtime strategy bit: dynamic steering (I-029), the bank preference it drives (I-032), the cluster bypass (I-027) |
| `+locality` | `loc_llb_en_i = 1` | I-060's clean-copy locality buffer |
| `+prefetch` | `loc_pf_en_i = 1` | I-060's bounded prefetcher |
| `+both` | `loc_llb_en_i = 1`, `loc_pf_en_i = 1` | the one *interaction* the locality step table says behaves differently from either structure alone |
| `+coalescing` | `vec_coalesce_dis_i = 0` | I-061's same-hart, same-beat line coalescing in the vector memory path (the baseline runs it disabled) |
| `+lanequota` | `lane_quota_req_i = 1`, `lane_quota_val_i = 4` | I-059's runtime lane share (reset share 8) |

`+steering` is honestly **one runtime bit that gates three structures**. The plan
treats `fab_dyn_i` as one toggle ("the same core, the same program, the same
seed, one bit"), and I-090's step table (`tools/run_fabric_steps.py`) is where the
three sub-steps are isolated. This package measures the toggle as the plan
defines it; it does not re-split it.

`cache_en_i` is **held high in every configuration**. It is not one of the seven
variables: the LLB and the prefetcher live inside the L1 path and act only on
cacheable RAM, so a comparison that flipped the cache while flipping the LLB
would be measuring two things at once. The L1 is part of the fixed platform, and
its geometry is inside the equal-resource assertion above.

### What was added to the RTL, and why nothing existing moved

* `rtl/core/mosaic_core.sv`: a new input `vec_coalesce_dis_i`. I-061's coalescing
  was tied high (`exec_coalesce_i (1'b1)`); it is now `~vec_coalesce_dis_i`, so a
  driver that leaves the new input at its reset value of 0 builds **exactly** the
  machine it built before, and this package's baseline (and only its baseline)
  disables it. New evidence outputs: `o_vec_lsu_merge_ctr` (the elements the
  coalescer removed from the request stream; the port was previously
  unconnected), `o_lane_quota_max`, `o_lane_quota_reset`, `o_geom_prf_banks`,
  `o_geom_cache_line_bytes`, `o_geom_cache_sets`, `o_geom_cache_ways`. New
  localparams (`CORE_CACHE_LINE_BYTES`/`_SETS`/`_WAYS`, `CORE_VEC_VLEN`,
  `CORE_LANE_QUOTA_MAX`) are now the single source used by the two cache-path
  instantiations, the vector instantiations and the broker.
* `rtl/core/mosaic_lane_broker.sv`: two evidence outputs, `o_quota_max` and
  `o_quota_reset`, so the lane budget is read from the broker rather than
  restated.
* `sim/tb/mosaic_core_tb.sv`: the matching wrapper input and evidence outputs.

No structure's behaviour changed: the only behavioural edit is the polarity of
the coalescing tie, and its default preserves the previous machine.

## The workloads

| workload | what it is | what it exercises |
|---|---|---|
| `alu_chain` | a dependent ALU chain (with a non-commutative `sub`, so the operand-swap control has something to break), a 48-iteration branch loop, two loads | scheduling / steering |
| `stream` | I-060's own streaming program, copied verbatim | the LLB and the prefetcher — and the negative below |
| `vec_stream` | a vector contiguous load/add/store at `e16,m1` (vl = 8) | coalescing and the lane quota |

## The table

Cycles, retired instructions, IPC, and the raw work counters (data-port beats,
instruction beats, vector element completions, coalesced elements removed). IPC
is reported, not used as a signoff; every configuration that is slower is
reported as slower.

```
workload alu_chain (park_pc=0x80000058)
  config           cycles    insns     IPC     dmem     imem   vec_elem     merged
  baseline           2389      494   0.206       20       20          0          0
  +steering          2389      494   0.206       20       20          0          0
  +locality          2389      494   0.206       20       20          0          0
  +prefetch          2389      494   0.206       20       20          0          0
  +both              2389      494   0.206       20       20          0          0
  +coalescing        2389      494   0.206       20       20          0          0
  +lanequota         2389      494   0.206       20       20          0          0

workload stream (park_pc=0x800000e4)
  config           cycles    insns     IPC     dmem     imem   vec_elem     merged
  baseline           3088      319   0.103      394       36          0          0
  +steering          3088      319   0.103      394       36          0          0
  +locality          2308      319   0.138      126       36          0          0
  +prefetch          3231      319   0.098      446       36          0          0
  +both              2295      319   0.138      126       36          0          0
  +coalescing        3088      319   0.103      394       36          0          0
  +lanequota         3088      319   0.103      394       36          0          0

workload vec_stream (park_pc=0x80000058)
  config           cycles    insns     IPC     dmem     imem   vec_elem     merged
  baseline            466       24   0.051       40       20         32          0
  +steering           466       24   0.051       40       20         32          0
  +locality           466       24   0.051       40       20         32          0
  +prefetch           466       24   0.051       40       20         32          0
  +both               466       24   0.051       40       20         32          0
  +coalescing         471       24   0.050       22       20         32          6
  +lanequota          466       24   0.051       40       20         32          0
```

### The negative column, in the table rather than averaged away

| workload | configuration | Δcycles vs baseline | the feature's own work | read |
|---|---|---|---|---|
| `stream` | `+prefetch` | **+143 (slower)** | 13 issued, **0 useful**, 13 useless | the prefetcher alone is harmful |
| `stream` | `+locality` | −780 (faster) | LLB hit+miss = 91 | the buffer is what makes reuse pay |
| `stream` | `+both` | −793 (faster) | LLB hit+miss = 234, 13 issued, **13 useful** | with the LLB every issue is useful |
| `alu_chain` | `+steering` | +0 (same) | 396 grants, 48 bypass hits, banks 247/248 | the fabric is engaged; on this short chain its effect on cycles is not measurable |
| `vec_stream` | `+coalescing` | +5 (slower) | 6 elements merged, dmem 40 → 22 | fewer requests, but this workload is too short for the saved beats to pay back the group bookkeeping |
| `vec_stream` | `+lanequota` | 0 (same) | quota = 4, per-lane elements [8 8 8 8 0 0 0 0] | the share changed *which lanes* worked, not the cycles: this single-issue engine does not turn lanes into throughput |

The first three rows reproduce I-060's step table on this harness — `+prefetch`
alone is slower with zero useful prefetches, `+both` is the fastest — and they
agree with `results/reports/I-060-locality-integration.md` (which measured the
same program: 3016 / 3159 / 2223 / 2236 cycles). The absolute cycles differ
(2394 vs 3016 for `alu_chain` is a different program; for `stream` 3088 vs 3016
is the same program under this driver's reset/settle and bus latency), so the
*sign and ordering* of the deltas is what is carried forward, not the literal
counts.

**A table averaged over configurations would have hidden the point.** The
prefetcher's mean effect across all three workloads is mildly negative; averaged
with `+both` it disappears; only the per-workload row shows that the prefetcher
alone is a pessimisation with zero useful work while the same prefetcher behind
the LLB is entirely useful. That is why the card says "including negative ones".

## The architectural-identity column

For every cell the harness compares, against the baseline, field by field through
the program's park instruction:

* the whole retire stream — sequence, PC, instruction bits, length, register
  write, destination, value, store address/data/size, CSR write, trap
  cause/tval;
* the four signature words the program publishes;
* the memory result the workload leaves (for `vec_stream`, the 16 bytes of C).

All 132 checks pass: the architecture is identical in every configuration. This
matters because a performance measurement that does not also show the
architecture unchanged is a measurement of a *different machine*. It is
controlled too: `MOSAIC_PERF_MUTANT_ARCH_DRIFT` perturbs the first retiring
register-write's value for every configuration after the first, and the identity
check catches it (exit 1 on `retires the same architecture as the baseline`).

An earlier revision used I-090's existing RTL control `MOSAIC_FAB_MUTANT_DYN_SWAP_SRC`
for this. On these workloads it does not reach the identity check: the swap also
crosses the operand *values* of an immediate ALU op, so a program's address setup
(`auipc`/`addi`) is corrupted in the dynamic configuration, the run derails into
unmapped memory and aborts before any retire stream is compared. That is an
honest property of that control on these programs (it is caught on I-090's
`p02_branch`, whose compiler output survives it), and it is why the architectural
control here is the driver-level drift: it proves the identity check itself fires.

The vector workload's result is observed from memory, not by a scalar load issued
behind the vector store. An earlier revision did read it back with a scalar load;
the load has no dependence on the store, so *which* value it saw depended on
timing, and coalescing changes timing — a property of the program's ordering, not
of coalescing. The result is now the memory image after the `FENCE.I` flush, and
it is identical in all seven configurations:

```
memory result of vec_stream at 0x80020020
    baseline     0300060009000c000f00120015001800
    +steering    0300060009000c000f00120015001800
    +locality    0300060009000c000f00120015001800
    +prefetch    0300060009000c000f00120015001800
    +both        0300060009000c000f00120015001800
    +coalescing  0300060009000c000f00120015001800
    +lanequota   0300060009000c000f00120015001800
```

## The engagement check (the `NO_DELTA` idea, generalised)

Each configuration's own evidence counter is required to be non-zero on the
workload that exercises it:

| configuration | primary workload | engagement evidence |
|---|---|---|
| `+steering` | `alu_chain` | 396 grants, 48 bypass hits |
| `+locality` | `stream` | LLB hit+miss = 91 |
| `+prefetch` | `stream` | 13 prefetches issued |
| `+both` | `stream` | LLB hit+miss = 234, 13 prefetches |
| `+coalescing` | `vec_stream` | 6 elements merged out of the request stream |
| `+lanequota` | `vec_stream` | quota = 4, 1 publish |

and the baseline is required to be inert (no grants, no LLB fills, no
prefetches, no merges) on every workload. A "configuration" that is really the
baseline under another name fails this check — which is the I-090 `NO_DELTA` idea
applied to every variable, and is controlled by `MOSAIC_FAB_MUTANT_NO_DELTA`
(exit 1 on `+steering actually engaged`).

## What each number is a measurement of

* This **geometry**: p1, 64 ROB entries, 8 issue-queue entries, 96 PRF entries in
  4 banks, two clusters, a direct-mapped 8-set × 32 B L1, an 8-lane vector budget
  at VLEN = 128 bits, single hart.
* These **three workloads**, each fixed in the driver and run from reset under the
  same seed (1) and the same cycle budget.
* A **single-hart** machine whose "dynamic" dimension today is a steering policy
  plus locality structures in the data path. This is **not** eight parallel lanes
  of real work and **not** a reconfigurable ownership fabric.
* Cycles are simulation cycles of this RTL, not frequency-scaled time; IPC is
  reported alongside the raw counts because the card says pure IPC is not a
  signoff, and no number here is a claim about silicon Fmax, area, network bytes,
  tail latency or power.

## Not covered (part of the measurement, not a footnote)

* **The dynamic-ownership / reconfiguration dimension is not in this
  comparison.** I-031's ownership FSM is compiled but deliberately **not
  instantiated** in the core, and no configuration here changes ownership or
  reallocates a resource at runtime. A reader must not conclude from this report
  that resource reconfiguration was demonstrated; it was not measured because it
  is not in the machine.
* **Distributed-ROB recovery and exception counter-examples are not part of this
  package's comparison.** The card asks a distributed-ROB experiment to carry its
  own recovery and exception counter-examples; this comparison is the
  scheduling/locality/fusion half and carries none, because there is no
  distributed ROB in the core to exercise.
* **No area / Fmax / network-bytes / tail-latency / power measurement.** Those
  need the physical flow (H-039 / I-084's ASIC inputs), which does not exist yet.
* **One seed, one profile.** Reproducibility across seeds is not measured; the
  workloads are deterministic and seed-independent here.
* **`+steering` is one toggle, not a per-structure measurement.** The three
  structures it gates are isolated by I-090's step table, not by this one.
* **The coalescing result is a single short workload.** `+coalescing` is 5 cycles
  slower on `vec_stream`; that is an honest negative, not a claim that coalescing
  never helps. A longer vector stream is not in this workload set.

## The controls

`tools/run_perf_controls.py` builds the shipping case from an empty directory,
then each control from its own empty directory with one `-D` in the build
command, and requires the binary hash to differ from the shipping binary, exit 1,
and the named first failure. `MOSAIC_PERF_MUTANT_RESOURCE_DRIFT` and
`MOSAIC_PERF_MUTANT_ARCH_DRIFT` are *driver* defines (passed through `-CFLAGS`,
since both drifts are in the harness's comparison inputs);
`MOSAIC_FAB_MUTANT_NO_DELTA` is the existing RTL control (I-090).

```
$ python3 tools/run_perf_controls.py --profile p1
building the shipping case from an empty directory...
  baseline exit=0 RESULT PASS perf.equal_resource_compare ...
shipping binary sha256: 88d324c0a5e74e497f5fc5402a52a6cdc238331a16b1bd7a047967efa6801989

mutant                                   exit  result
-----------------------------------------------------------------------
MOSAIC_PERF_MUTANT_RESOURCE_DRIFT        1     OK
    sha256:  235c1297378a91f9bbb2200a7cc04f5396d65814bc50f0a82569761617ba7022
    MISMATCH the resource bundle of +steering on alu_chain: expected rob=64
             ... lanes=8 reset=8 vlenb=16, got rob=80 ... lanes=12 reset=8 vlenb=16
MOSAIC_PERF_MUTANT_ARCH_DRIFT            1     OK
    sha256:  97b65040ee9639178a3ef92c9a2202b8ca427de38d1d1b9ab0b4691be8782824
    MISMATCH +steering on alu_chain retirement 0: expected the same architecture
             as the baseline, got a different retire stream
MOSAIC_FAB_MUTANT_NO_DELTA               1     OK
    sha256:  52741c3bc4cc6ac047817aae53b5393d38351c5b17d6ac2b5954b0b6c539b593
    MISMATCH +steering on alu_chain: expected the steering variable did work,
             got fabric grants=0
-----------------------------------------------------------------------
all 3 mutants mutate the binary, exit 1 and name the check they break
```

Each row corresponds to a card requirement: the resource-count control proves the
equal-resource assertion fires; the architectural control proves the
architectural-identity assertion fires; the `NO_DELTA` control proves the
engagement assertion fires (a "configuration" that is really the baseline is
caught, not averaged away).

## The tree stayed green

The cases the package must keep passing were re-run after the change (the RTL
edits are additive: a new input whose default preserves the previous machine, and
new evidence outputs).

| case | profile | result |
|---|---|---|
| `core.act_dut` | p1 | **PASS, 127/127** (generated=127 run=127 passed=127 failed=0) |
| `fabric.integrated` | p1 | PASS |
| `locality.integrated_path` | p1 | PASS |
| `cache.integrated_path` | p1 | PASS |
| `llb.stale_copy_invalidation` | p1 | PASS |
| `prefetch.fault_and_pollution` | p1 | PASS |
| `core.corpus_sweep` | p0 | PASS |
| `core.mem_program` | p0 | PASS |
| `vec.integrated` | p0 | PASS |
| `rvv.lane_resize_boundary` | p0 | PASS |
| `coalesce.element_faults` | p0 | PASS |
| `memory.qos_no_starvation` | p0 | PASS |

Gate checks run in this session: `tools/lint_rtl.py --profile p1` (63 files
clean), `slang-tidy --single-unit` over all RTL (exit 0, no errors), `make check`
(exit 0), `make lint-cpp PROFILE=p1` (44 files clean; unbuilt cases named as
skipped, as the target documents). `make lint-cpp` for p0 skips
`sim/unit/tb_core_perf.cpp` (the case is p1-only, so it has no p0 build
directory) and is otherwise unaffected.

