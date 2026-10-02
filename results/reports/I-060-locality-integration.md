# I-060 — the locality integration: `locality.integrated_path`

Work package I-060 (`docs/implementation-plan.md` §3.1), the integration half. The
structures themselves are I-060's module package (`results/reports/I-060-llb.md`)
and I-063's (`results/reports/I-063-prefetch.md`); this package puts them in the
core's data path and measures the result one variable at a time.

Registered case `locality.integrated_path`, top `mosaic_core_tb`, driver
`sim/unit/tb_core_locality.cpp`, profile **p1** (the profile whose platform map
declares RAM cacheable). Run with
`python3 tools/run_unit.py --profile p1 --case locality.integrated_path`.

**Verdict: PASS** — from a deleted build directory, with four controls that each
exit 1 with a named first failure and a binary hash different from the shipping
build (table below).

```
RESULT PASS locality.integrated_path locality integration: on/off identical,
       structures used, class rule checked, no prefetch reaches a device
locality.integrated_path: retire_width=2 park_pc=0x800000f8
locality.integrated_path: baseline cycles=3016 imem=36 dmem=394 llb{hit=0 miss=0 bypass=0
    fill=0 fill_refused=0 inv=1} line{mem=97} pf{issued=0 useful=0 useless=0 late=0
    cancelled=0 admitted=0 fill=0 fill_refused=0 mem=0} sig=[398 69]
locality.integrated_path: llb      cycles=2236 imem=36 dmem=126 llb{hit=67 miss=24
    bypass=0 fill=24 fill_refused=0 inv=3} line{mem=30} pf{...all 0...} sig=[398 69]
locality.integrated_path: prefetch cycles=3159 imem=36 dmem=446 llb{...all 0...}
    line{mem=97} pf{issued=13 useful=0 useless=13 late=0 cancelled=0 admitted=13
    fill=0 fill_refused=13 mem=13} sig=[398 69]
locality.integrated_path: both     cycles=2223 imem=36 dmem=126 llb{hit=80 miss=154
    bypass=0 fill=24 fill_refused=0 inv=3} line{mem=17} pf{issued=13 useful=13
    useless=0 late=0 cancelled=0 admitted=13 fill=13 fill_refused=0 mem=13} sig=[398 69]
```

## The integration order, and the tree stayed green

The card's rule is one step at a time, with the tree green and ACT4 at 127/127
between steps. Each step below was built and the named cases re-run before the
next one was started; no step was landed half-done.

| # | step | what was added | ACT4 / cases after the step |
|---|---|---|---|
| 1 | the locality path module | `rtl/core/mosaic_locality_path.sv`: the LLB and the prefetcher on the L1 data cache's line port, the line-path mux, the prefetch request queue, the eight invalidation sources | `llb.stale_copy_invalidation` PASS (module case unchanged) |
| 2 | the LLB in the path | `mosaic_l1_cache_path.sv` routes the data side's line port through the path; `mosaic_llb.sv` gains two replacement-observation outputs; the LLB's own case is updated for them | `cache.integrated_path` PASS with the switches off (the baseline is byte-for-byte the machine it was) |
| 3 | the core wiring | `mosaic_core.sv`: the two runtime switches, the eight sources (`inv_store` from the endpoint's commit, `inv_refill` from the cache's own line traffic, `inv_snoop`/`inv_snoop_all`, `FENCE`/`FENCE.I`/`SFENCE.VMA`, `ctx` from `satp`), the context (`vpn` from the served access's `tval`, `asid` from `satp`, `pc` from a new functional ROB read port), and the observation counters | `cache.integrated_path` PASS; core lints clean both profiles |
| 4 | the case and the controls | `sim/unit/tb_core_locality.cpp`, the four controls, `tools/run_locality_controls.py` | the table below |

ACT4 (`core.act_dut`, p1) is **127/127** after the change; see the case table at
the end of this report.

## The topology

```
      LSU endpoint  (64-bit CPU port, one transaction at a time)
            |
            v
   mosaic_l1_cache_path  (IS_FETCH = 0)
            |
     mosaic_cache   (8 sets x 32 B, write-back, line port = 256 bits)
            |
            v
   mosaic_locality_path        <-- the LLB and the prefetcher live here
     |  line read the L1 missed  -> the LLB first; on a hit the line is
     |                              returned from the clean copy and memory
     |                              is never asked
     |  line read the L1 missed and the LLB missed -> the bridge; the line
     |                              the memory service returns is installed
     |                              in the cache *and* in the LLB
     |  line write (dirty writeback) -> the bridge, and the LLB's copy of
     |                              that line is invalidated
     |  a store's read-for-ownership -> the bridge only: the line is about
     |                              to be dirtied and the store's own
     |                              invalidation has already been applied
     |
     |  the prefetcher's line read -> a one-deep request queue -> the same
     |                              bridge, cache first (no fourth master on
     |                              the core's memory arbiter)
            |
            v
   mosaic_cache_line_bridge   (line <-> doubleword beats)
            |
            v
   the core's dmem arbiter  (PTW > VEC > endpoint, unchanged)
```

The position is the card's: between the L1 data cache and the memory service, on
the cache's *line* port. It is also the only position in which the LLB has a
natural line fill and a natural line hit: a load the L1 misses is a line read
the LLB can answer, and the line the memory service returns on a miss is exactly
the clean copy the LLB wants to hold.

## The per-step measurement

`tools/run_fabric_steps.py`'s shape, for the locality structures: one program,
four runs from a fresh reset, one variable at a time, on equal resources (the
same core, the same program, the same seed, the same geometry).

| step | cycles | dmem beats | imem beats | cache line txns to memory | LLB hits | prefetches useful |
|---|---|---|---|---|---|---|
| baseline (both off) | 3016 | 394 | 36 | 97 | 0 | 0 |
| LLB only | 2236 | 126 | 36 | 30 | 67 | 0 |
| prefetcher only | 3159 | 446 | 36 | 97 | 0 | 0 |
| both | 2223 | 126 | 36 | 17 | 80 | 13 |

Read honestly:

* **The LLB pays on this program.** With it on, the data port's traffic falls
  from 394 to 126 beats and the cache's line transactions from 97 to 30; the
  cycles fall from 3016 to 2236. The 67 hits are the reuse loop: two lines that
  collide in one L1 set, so every access misses the L1 and, after the two fills,
  every access is answered from the buffer.
* **The prefetcher alone is pure overhead on this program, and it is reported as
  such.** With it on and the LLB off, it issued 13 line reads, bought nothing
  (`useful = 0`, `useless = 13`), and the data-port traffic *rose* from 394 to
  446 beats and the cycles from 3016 to 3159. That is the honest negative: the
  prefetcher's only population route is the buffer's fill port, so a prefetch
  whose line cannot be held is a read bought with nothing -- the same shape as
  I-063's p0 run (22 issued, 0 useful).
* **Together they compose.** `both` is the cheapest of the four on every axis
  (2223 cycles, 126 beats, 17 line transactions) and the 13 prefetches are all
  useful: a stride of eight lines in one L1 set, so every demand misses the L1
  and the line the prefetcher fetched is the line the next demand wants. The
  line transactions fall from 97 (baseline) to 17 (both) -- the prefetcher turns
  the reuse loop's misses into buffer hits *before* they happen.
* **The architecture is identical in all four.** The retirement stream (pc,
  length, destination, value, in order) through the program's park loop and the
  four signature words are the same in every run; the signature is also checked
  against the arithmetic the program performs (920 and 105), so "all four agree"
  is not the only claim.

## What these numbers are a measurement *of*

This program, on this geometry. Concretely: one hart, one 8-entry fully
associative 32-byte clean-copy buffer, one 16-entry PC-indexed stride table of
depth 4, one 8-set 32-byte write-back L1, a directed workload of two colliding
lines (the reuse loop), a stride stream near the top of RAM, a store whose line
fill is bracketed by markers, and an AMO-then-load. No general hit rate, no
energy claim (the card forbids reading one out of a hit-rate improvement, and
none is made), and no multi-hart claim. The traffic deltas are deltas on *this*
program; a different program would move them, and the `prefetcher only` row is
the evidence that they can move the wrong way.

## The controls

`python3 tools/run_locality_controls.py --profile p1`. Each is built from a
deleted build directory with its `-D` on the Verilator command line (and passed
to the C++ half); the binary hash differs from the shipping one; the run exits 1
with a named first failure.

| control | `-D` | sha256 | exit | first failure |
|---|---|---|---|---|
| shipping | — | `22e02189c017` | 0 | — |
| PERM_BYPASS | `MOSAIC_LOC_MUTANT_PERM_BYPASS` | `7e7c55647ac6` | 1 | the class check: the store's read-for-ownership consulted the buffer (1 lookup where the case requires 0) |
| STORE_NO_INVALIDATE | `MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE` | `f12cf50b60a2` | 1 | the signature: the load after the AMO read `0x64` (the pre-store value) where memory holds `0x69` |
| PREFETCH_PERM_BYPASS | `MOSAIC_PREFETCH_MUTANT_PERM_BYPASS` | `bcd449779d69` | 1 | a prefetch read was presented for `0x80200000`, one line past the top of RAM |
| PREFETCH_ARCH_DATA | `MOSAIC_PREFETCH_MUTANT_ARCH_DATA` | `50d2f6fb8047` | 1 | the retirement stream differs when the structures are on: a demand served a corrupted fill |

The four are the card's required controls. Two of them are the structures' own
mutants (I-060's `STORE_NO_INVALIDATE`, I-063's two), which the module cases also
catch; what this case adds is that they are caught *in the core*, through the
integration:

* **`MOSAIC_LOC_MUTANT_PERM_BYPASS`** (this package) drops the access-class rule
  -- a store's read-for-ownership may consult and populate the buffer -- and
  collapses the permission-context component of the key, so every access class
  shares one context. The case's class-check phase brackets a load and a store
  with device markers and requires the load to consult the buffer exactly once
  (its own positive control) and the store not to consult it at all.
* **`MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE`** is caught *architecturally*: the
  program fills a line into the buffer, cleanly evicts it from the L1, writes it
  with an AMO (which bypasses the cache and writes memory), and loads it. The
  load must see memory; with the mutant it sees the pre-store copy.
* **`MOSAIC_PREFETCH_MUTANT_PERM_BYPASS`** is caught by the case's data-port
  address check: every request the run makes must be inside the program's own
  normal memory or one of its four markers. The prefetch stream's last candidate
  is one line past the top of RAM, and with the mutant that read is presented.
* **`MOSAIC_PREFETCH_MUTANT_ARCH_DATA`** is caught by the on/off comparison: a
  corrupted fill reaches a demand, and the retirement stream differs.

## What this does not cover (honest list)

* **I-084's full comparison is the next step, not this one.** The plan's success
  criterion asks for the steering/locality/fusion comparison on equal resources.
  The steering half is measured (I-090); the locality half could not be measured
  until the structures were in the path, because there was nothing to compare
  against. They are in the path now, and this case is the *instrument*: the
  four-row, one-variable-at-a-time table above is the form I-084 needs, and the
  `prefetcher only` row is the negative that keeps it honest. What I-084 still
  needs, precisely: (a) the same treatment for the fusion arm, so the three
  variables are switched independently; (b) the `perf.equal_resource_compare`
  case (registered, `pending`) extended to include these two switches in its
  sweep; and (c) a stated statement of what the *combination* is measured
  against, which today is only the baseline row of this table.
* **The scheduler (I-062) is deliberately not wired, and here is why.** The
  card permits it only if it composes with the arbiter the core already has. It
  does not. `mosaic_mem_qos` is a *service* model: it owns its own three-class
  request queues, its own arrival timestamps, its own windowed eligibility and
  its own `srv_*` service port, and it decides which of its classes gets the
  port and when. The core's `dmem` arbiter is a hand-written single-outstanding
  ownership mux -- one `mem_owner_q` register that both grants the port (PTW >
  VEC > endpoint) and routes the response back to the queue that owns it, with
  the endpoint's one-transaction-at-a-time rule underneath. Composing the two
  would mean replacing the ownership model with the QoS window model and
  re-deriving the response routing and the PTW priority: a second service model,
  which the card forbids. `memory.qos_no_starvation` keeps it as a module, and
  the fabric integration's "compiled but deliberately not instantiated" decision
  is the precedent.
* **A prefetch to a *device* is unreachable from a program in this geometry, and
  the control uses the reachable form instead.** The prefetcher's stride table
  is one stride per PC: to make the *candidate* a device line, two earlier
  demands of the same PC must be RAM lines whose stride maps to the device --
  and with a single stride the intermediate demand lands on the device too, and
  a device demand never produces a cache line request, so it never trains. What
  *is* reachable is the same gate's other half: a candidate past the top of RAM
  (`0x80200000`), which the demand path would refuse and which
  `MOSAIC_PREFETCH_MUTANT_PERM_BYPASS` presents. The device fact
  (`gate_device_i`, from the platform map's device predicate) is wired and is
  exercised at the module level by `prefetch.fault_and_pollution`; in the core
  it is a constant that the geometry cannot move.
* **A demand fill racing a store to the same line is not covered.** The
  prefetch case *is* covered -- a store cancels an outstanding prefetch through
  the release input, so its response is dropped -- but a *demand* fill whose
  line is written after the read was issued and before it lands would install a
  stale copy. That needs the LQ-violation/replay rule I-060's own report lists
  as unimplemented ("validate to retire"), and this integration does not add it.
* **The prefetcher's observation point is the cache's line request**, so it
  trains on the demands that miss the L1, not on every load. A load that hits
  the L1 is not observed (it never reaches the line port). The stride table's
  `obs_reuse_hit` counter is therefore not driven in the integrated path.
* **`inv_snoop_all` is an input, not a fabric.** There is one hart; the
  broadcast invalidation (a shootdown) is a top-level input a coherent fabric
  would drive, and no case in this tree drives it from a real second agent. The
  LLB's own case exercises the port; this integration only wires it.
* **The permission context is the access's own class**, not the leaf
  permission class: `{x, w, r, u}` with `x` and `u` zero, `w` the access's
  read/write class and `r` always set. It is the class the endpoint publishes,
  it is conservative in the direction that matters (a store can never reuse a
  load's copy, and stores do not consult the buffer at all), and the LLB's key
  format is unchanged by it -- but it is not the leaf's permission class, and a
  case that wanted the leaf's would have to latch the TLB's perms with the
  access.
* **The measurement is one workload.** See "what these numbers are a measurement
  of".
* **No multi-hart, no DMA, no device master, no debugger.** Unchanged from
  I-060's module report.

## RTL revisions compiled (sha256, first 16 hex)

| file | hash |
|---|---|
| `rtl/core/mosaic_locality_path.sv` (new) | `eaf7d564dbdae6ff` |
| `rtl/core/mosaic_l1_cache_path.sv` | `6bd637d331bacd28` |
| `rtl/core/mosaic_core.sv` | `e5fceea8499ba368` |
| `rtl/core/mosaic_rob.sv` (functional PC read port) | `9bae53e491567d0f` |
| `rtl/core/mosaic_llb.sv` (replacement observation) | `8a4bb1c0a9bc5f4c` |
| `sim/tb/mosaic_core_tb.sv` | `e0a1588a2d87ec53` |
| `sim/unit/tb_core_locality.cpp` (new) | `37860d770cb37b63` |
| `tools/run_locality_controls.py` (new) | `f13df897ed4313e0` |

## Cases re-run after the change

```
PASS core.act_dut                 task=V-043   (ACT4 p1: applicable=127 generated=127
                                                run=127 passed=127 failed=0)
PASS core.corpus_sweep            task=I-023
PASS core.mem_program             task=I-023
PASS cache.integrated_path        task=I-042
PASS llb.stale_copy_invalidation  task=I-060
PASS prefetch.fault_and_pollution task=I-063
PASS memory.qos_no_starvation     task=I-062
PASS fabric.integrated            task=I-090
PASS boot.p1_contract             task=I-048
PASS sv39.walk_and_faults         task=I-045
PASS tlb.sfence_vma               task=I-046
PASS mmio.exactly_once            task=I-038
PASS core.trap_csr_program        task=I-023   (p0: the case is p0-only)
PASS locality.integrated_path     task=I-060   (this package)
```

## Gates

* `make check PROFILE=p1` rc 0: config, contracts, the event contract and its 36
  negative controls, records (78 delivered packages, 93 registered cases), the
  exclusion ledger and its 13 negative controls, coverage, the capability
  matrix and its 5 negative controls, upstream, isolation.
* `python3 tools/lint_rtl.py --profile p0` and `--profile p1`: **63 source files
  clean** in both. `mosaic_locality_path.sv` adds no `STYLE-13` (every
  `always_comb` in it is named); the two `STYLE-13` warnings `mosaic_l1_cache_path.sv`
  carries are on the two unnamed blocks that predate this package.
* `make lint-slang PROFILE=p1` rc 0.
* `make lint-cpp PROFILE=p1` rc 0: **43 files clean**. `make lint-cpp PROFILE=p0`
  rc 0: **75 files clean**. The p1 run skips the C++ sources whose cases have
  never been built in this tree (their generated `V*_tb.h` does not exist) --
  the same pre-existing state `results/reports/I-063-prefetch.md` records; `make
  test` runs `unit` before `lint-cpp`, which regenerates every header. The 41
  stale `Vmosaic_core_tb.h` files an earlier `unit` run had left in p1's other
  `obj_dir`s were removed, because `lint-cpp` puts every `obj_dir` on the
  include path and the compiler otherwise picks an obsolete core testbench
  header ahead of the current one; the case that regenerates each is `make
  unit`.
* The new driver `sim/unit/tb_core_locality.cpp` compiles clean under the same
  `-Wall -Wextra -Wshadow` flags `lint-cpp` uses (only Verilator's own headers
  warn).
