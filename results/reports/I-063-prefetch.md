# I-063 — a bounded, switchable prefetch: `prefetch.fault_and_pollution`

Work package **I-063** of `docs/implementation-plan.md` (§3.1). Registered case
`prefetch.fault_and_pollution`, top `mosaic_prefetch_tb`, driver
`sim/unit/tb_prefetch.cpp`, RTL `rtl/core/mosaic_prefetch.sv` (new) plus the
I-060 `rtl/core/mosaic_llb.sv` it is allowed to populate. Run with
`python3 tools/run_unit.py --profile p1 --case prefetch.fault_and_pollution`
(p1 is the profile whose platform map declares RAM cacheable) and
`python3 tools/run_unit.py --profile p0 --case prefetch.fault_and_pollution`
(the case is registered without a `profiles` field, so p0 is the default run).
The case stays registered `"pending": true`; the integration lead clears it when
recording. The registry at the time of writing carries no `profiles` field for
this case; a `"profiles": ["p1"]` field would state the intent that the useful
half of the campaign needs a cacheable region, but the registry is the
integration lead's and was not edited.

**Verdict: PASS** — at p0 and p1, from a deleted build directory, with four
controls that each exit 1 with a named first failure and a binary hash different
from the shipping build.

```
p1  RESULT PASS prefetch.fault_and_pollution prefetch harm bound holds: 13 prefetches
      (useful 5, useless 2, late 1, cancelled 1), 56 demand reads, over 94 cycles
p0  RESULT PASS prefetch.fault_and_pollution prefetch harm bound holds: 22 prefetches
      (useful 0, useless 20, late 1, cancelled 1), 72 demand reads, over 103 cycles
```

## The rule and its bound

`rtl/core/mosaic_prefetch.sv`. A PC-indexed stride table, line-granular:

| property | this implementation |
|---|---|
| table | 16 entries, indexed by `pc[5:2]`, tagged by `pc[31:6]` (one stream per PC) |
| stride | the physical **line** delta between consecutive demands of the same PC, two's complement; the observation is made before the entry is updated |
| confidence | 0..3; a stride change resets it to 1 (0 when the observed stride is zero), a stride match increments it to a ceiling of 3 |
| candidate | the demand's own line plus the *stored* stride, so the prediction is made for the next access, not this one; fired only when `conf >= conf_thresh_i`, the stride is non-zero and the candidate line differs from the demand's |
| the switch | `en_i`. With it low, no candidate and no read; the table still observes |
| the bound | ≤ 16 remembered streams; ≤ 4 outstanding prefetches; **at most one candidate per demand**; a predicted line already outstanding is suppressed; a full outstanding table suppresses (counted `o_full_stall_o`); and **a prefetch never trains the table** |

The last rule is the one that makes the traffic bound a property of the state
machine and not of the workload: only a demand access updates an entry, so a
wrong prediction cannot manufacture more predictions. A burst of demand
accesses can add at most `DEPTH` = 4 outstanding reads.

## The gate: the predictor is never the authority

The candidate is a memory read only when the environment's gate agrees, and the
gate is the *same* predicate the demand path uses. It is supplied to the module
as inputs, never as a parameter:

* `gate_mapped_i` and `gate_perm_ok_i` — the physical address is covered by the
  generated platform map, and the demand's own permission answer permits the
  read. `gate_mapped_i` is computed in the wrapper from
  `mosaic_cfg_pkg::mosaic_pa_cacheable/device/idempotent`; `gate_perm_ok_i` comes
  from the driver, which owns the page table (a faulting demand sets it low).
* `gate_device_i` — the map's device predicate. The platform map marks every
  non-idempotent region as a device, so `mapped && !device` is normal memory;
  the driver's reader independently requires idempotency as a backstop.

A candidate that fails either group is counted in `o_gate_refuse_o` and nothing
else happens: there is no trap path, no architectural state, and no request to a
device.

## The LLB bypass policy

The prefetch's only population route is the LLB fill port, and the LLB's own
rules decide whether a copy is legal: the generated map's cacheability predicate
for a fill, and the same-cycle invalidation refusal. The driver routes the
prefetcher's fill outputs to the LLB fill port (the wrapper keeps them separate
so the arbitration is visible), and returns the LLB's `fill_ok_o` to the
prefetcher. A fill the map refuses is counted in `o_fill_refused_ctr_o` and the
prefetch is terminal `useless` — the honest outcome on a non-cacheable profile.
A cancelled prefetch's response never presents a fill at all.

## The characteristics record (the card's first sentence)

Recorded **with `en_i = 0`, before prediction was enabled**, from the DUT's own
observation counters:

```
[record] p1: en=0 accesses=18 new_pc=3 repeat_pc=15 stride_match=8 stride_mismatch=4
             stride_zero=3 reuse_hit=6 issued=0
[record] p0: en=0 accesses=18 new_pc=3 repeat_pc=15 stride_match=8 stride_mismatch=4
             stride_zero=3 reuse_hit=0 issued=0
```

The workload is three PCs: a strict +1 line stride (10 accesses), a same-line
repeat (4 accesses, stride zero), and an irregular stream (4 accesses, strides
that do not repeat). The record is taken from the DUT, not from a driver-side
model, and the case asserts `issued == 0` at that point: prediction was off, so
the record is the observed profile and not a prediction of it. `reuse_hit`
counts demands that hit the LLB from the driver's demand refills; it is 6 under
p1 and 0 under p0, because p0's map declares RAM non-cacheable and every fill is
refused.

## The usefulness histogram, and prediction on versus off

`en` is the switch; the driver runs the same demand stream with it off and on.

| counter | p1 (RAM cacheable) | p0 (RAM non-cacheable) |
|---|---|---|
| issued | 13 | 22 |
| useful | 5 | 0 |
| useless | 2 | 20 |
| late | 1 | 1 |
| cancelled | 1 | 1 |
| in-flight | 4 | 0 |
| admitted | 13 | 22 |
| gate_refuse | 2 | 2 |
| full_stall | 9 | 0 |
| fill | 11 | 0 |
| fill_refused | 0 | 20 |
| dropped | 2 | 2 |

Conservation holds in both: `issued == useful + useless + late + cancelled +
in-flight` (13 = 5+2+1+1+4; 22 = 0+20+1+1+0).

The architectural identity is checked on a fixed 12-demand trace replayed both
ways: the delivered values and the fault set are **identical**; only the traffic
differs.

```
[identity] p1: values_equal=1 faults_equal=1 demand_reads_off=10 demand_reads_on=8
               prefetch_reads_on=4
[identity] p0: values_equal=1 faults_equal=1 demand_reads_off=12 demand_reads_on=12
               prefetch_reads_on=12
```

**The p0 result is a negative one and it is reported as such.** On a map that
caches nothing, this prefetcher issued 22 reads and bought nothing: 20 landed as
`useless` and the demand-read count did not move (12 → 12). That is a legitimate
finding, not a failure of the case — the card explicitly forbids reading an
energy benefit out of a hit-rate improvement, and here there is not even a
hit-rate improvement. The p1 run is the positive half: 5 useful, 2 useless, and
the same 12-demand trace costs 10 demand reads instead of 8 plus 4 prefetch
reads.

`full_stall = 9` on p1 is the bound at work: during the identity trace the four
outstanding slots filled, so nine candidates were suppressed rather than issued.

## The fail modes, checked where they happen

| check | phase | what it proves |
|---|---|---|
| useful | a +1 stream, the prefetch answered before the demand | a correct prediction lands and the demand hits it; the delivered value is compared with the driver's memory word for word |
| pollution | a stream that jumps after predicting | a wrong prediction is counted `useless`; the architectural result is unchanged |
| unmapped-prefetch | the last RAM lines predict past the end of RAM | no read is presented for an unmapped address (`0x80200000`); the refusal is counted |
| device-read | UART polling predicts the next UART line | no prefetch read reaches the device; the poll's own demand reads stay on the demand path |
| faulting-demand | a store to a read-only page in a trained stride stream | a faulting demand trains nothing and spawns no hint |
| cancel | a prefetch cancelled before its response | no fill lands, the LLB is read out through its debug port and holds nothing, `dropped` increments |
| alias / permission | one line through two VPNs; RW vs RO vs write-only contexts | the fill is physical (the second alias hits); a different permission context does not reuse it |
| late | the demand for the predicted line arrives first | reported `late`, and the late response is dropped |
| identity | the same trace off and on | identical architectural values and fault set |
| histogram | end of run | conservation |

## Mutants (controls)

`python3 tools/run_prefetch_controls.py --profile p1`. Each is built from a
deleted build directory with its `-D` on the Verilator command line, the binary
hash differs from the shipping one, and the run exits 1 with a named first
failure.

| control | `-D` | sha256 | exit | first failure |
|---|---|---|---|---|
| shipping | — | `b63957e686ccb623` | 0 | — |
| PERM_BYPASS | `MOSAIC_PREFETCH_MUTANT_PERM_BYPASS` | `a425c6eb5ee7672e` | 1 | `unmapped-prefetch: a prefetch read was presented for unmapped pa 0x0000000080200000` |
| DEVICE_READ | `MOSAIC_PREFETCH_MUTANT_DEVICE_READ` | `e35a2f0d86796729` | 1 | `device-read: a prefetch read was presented for device pa 0x0000000000100060` |
| CANCEL_LANDS | `MOSAIC_PREFETCH_MUTANT_CANCEL_LANDS` | `d39275f76319c239` | 1 | `cancel: the cancelled response was not dropped` |
| ARCH_DATA | `MOSAIC_PREFETCH_MUTANT_ARCH_DATA` | `366e7e04961dee9a` | 1 | `architectural-hit: a demand hit a copy that differs from memory at pa 0x0000000080002060` |

The four are the card's required controls: the predictor that lets a prefetch
bypass the permission check (dropping the mapped/permission group, and with it
the "a faulting demand spawns no hint" rule); the prefetch that performs an
irreversible device read (dropping the device fact); the cancelled prefetch
whose data lands anyway; and the prediction that changes an architectural result
(corrupting the fill data, caught by the demand's own comparison against memory).

## RTL revisions compiled (sha256, first 16 hex)

| file | hash |
|---|---|
| `rtl/core/mosaic_prefetch.sv` | `be215c1ae22448e0` |
| `sim/tb/mosaic_prefetch_tb.sv` | `896c80680cbf295c` |
| `sim/unit/tb_prefetch.cpp` | `e617db623e1194fb` |
| `tools/run_prefetch_controls.py` | `c2887eea10f98cc6` |
| `build/p1/rtl/mosaic_cfg_pkg.svh` (generated) | `9d47346b819ef8d3` |
| `build/p0/rtl/mosaic_cfg_pkg.svh` (generated) | `48b3b34d9a585818` |

Nothing else was changed. `mosaic_core.sv` and every other shared core file are
untouched.

## Cases re-run after the change

* p1: `prefetch.fault_and_pollution` PASS; `llb.stale_copy_invalidation`,
  `sv39.walk_and_faults`, `tlb.sfence_vma` PASS.
* p0: `prefetch.fault_and_pollution` PASS; `coalesce.element_faults`,
  `memory.qos_no_starvation`, `cache.refill_evict_fault`,
  `cache.mshr_nonblocking`, `core.mem_program`, `core.corpus_sweep` PASS.
* `core.act_dut` (ACT4, p1): **`RESULT PASS core.act_dut ran=127 passed=127
  expected=127`**, exit 0.

## Gates

* `make check PROFILE=p0` rc 0 (config, contracts, event contract + negative,
  records, exclusions + negative, coverage, capability matrix, upstream,
  isolation).
* `python3 tools/lint_rtl.py --profile p0` and `--profile p1`: **62 source files
  clean**; `--self-test` still rejects a latching module.
* `make lint-slang PROFILE=p1` rc 0. `mosaic_prefetch.sv` adds two `STYLE-2`
  port-suffix warnings (`clk`, `rst`), the same warning the whole tree carries.
* `check_records` rc 0 (77 delivered packages, 91 registered cases);
  `check_exclusions` and `--negative` rc 0; `check_docs` rc 0.
* `sim/unit/tb_prefetch.cpp` compiles clean under `-Wall -Wextra -Wshadow`
  against both profiles' generated headers (verified directly).
  `make lint-cpp PROFILE=p1` itself currently fails on `sim/unit/tb_core_vec.cpp`
  because `build/p1/unit/core.vec/obj_dir` holds a `Vmosaic_core_tb.h` older than
  the current core testbench; the compiler picks the stale `-I` first. That is
  not this package's file and not this package's change: `make test` runs `unit`
  before `lint-cpp`, which regenerates every header.

## Not covered (honest list)

* **The LLB is not in the core path, and neither is the prefetcher.** Both are
  module-level DUTs driven by `mosaic_prefetch_tb`. "Prefetch into the LLB" is a
  *module-level interaction*: the driver presents the prefetcher's fill to the
  LLB fill port, exactly as an integrated load path would, but no real load
  flows through either structure. What integration must add: a real demand
  stream from the LSU, a real memory-request path for the prefetch egress (with
  the same arbitration the QoS scheduler owns), a real `satp`/permission source
  for `gate_perm_ok_i`, and the LQ-violation/replay rule the architecture review
  asks for when a speculative fill is consumed.
* **`mosaic_core.sv` is untouched.** The prefetcher is not instantiated in the
  core; there is no integration to point at.
* **No energy claim, and none is inferable from this case.** The card forbids
  reading an energy benefit out of a hit-rate improvement, and this report does
  not. A real energy claim would need per-access switching activity (or a
  characterised power model) at a named node, with the prefetch traffic and the
  demand-read delta measured separately — none of which exists here. What does
  exist is the traffic delta (p1: 10 demand reads off versus 8 + 4 prefetch
  reads on) and the honest p0 negative (22 reads, no benefit).
* **The predictor's accuracy is measured on one workload.** The stride streams
  are directed. The case does not claim a general hit rate, and the p0 run shows
  the predictor can be pure overhead.
* **Single hart.** There is no cross-hart interaction, and the case does not
  drive the LLB's invalidation sources while prefetches are in flight; a
  multi-hart implementation would have to prove a prefetch fill cannot outlive a
  remote invalidation.
* **The LLB's own protocol is I-060's evidence, not re-proven here.** The
  eight invalidation sources, the physical-line keying and the "a cycle that
  invalidates grants no hit" rule are proven by `llb.stale_copy_invalidation`.
  This case drives only the fill path into the LLB (and reads the LLB back
  through its debug port); it does not re-derive the ordering rule.
* **The `device` fact is the map's device predicate.** The map marks every
  non-idempotent region as a device, so `mapped && !device` is normal memory
  here; a future non-idempotent, non-device region would need the module to
  carry an explicit idempotency gate. Today the driver's own reader checks
  idempotency independently, so the mutant that admitted such a read would still
  be caught.
* **The registry entry carries no `profiles` field.** Under `PROFILE=p0` the
  case runs the honest bypass-and-negative path; the useful half needs p1. The
  integration lead owns `tests/unit/registry.json`.
