# I-043 — Non-blocking L1 read path with an MSHR

Task card: `docs/implementation-plan.md` §I-043 (line 687) and
`docs/stage-3-memory-system.md`. Case: `cache.mshr_nonblocking`
(`tests/unit/registry.json`). This package is additive: it instantiates no core
module and modifies no core file, so it cannot regress any other registered case
(§9 proves the source sets are disjoint).

Files owned by this deliverable:

| File | What it is |
|---|---|
| `rtl/core/mosaic_mshr.sv` | the non-blocking read L1: storage, MSHR table, response distributor |
| `sim/tb/mosaic_cache_tb.sv` | `mosaic_cache_tb` gains a third instance (`u_mshr`) beside the two blocking caches |
| `sim/unit/tb_cache.cpp` | the I-043 campaign and its flat-memory oracle, added beside the I-042 campaign; `main` dispatches on `--case` |
| `tools/run_mshr_controls.py` | the three negative controls (mutants) of §8 |
| `results/reports/I-043-mshr.md` | this file |

Toolchain actually used: **Verilator 5.052 2026-09-05**, Python 3.9.6, macOS/arm64
(Darwin 25.6.0).

Compiled source revisions (working tree, `git rev-parse --short HEAD` = `b878fa0`):

| File | SHA-256 (prefix) |
|---|---|
| `rtl/core/mosaic_mshr.sv` | `233b9657a6e80b99…` |
| `sim/tb/mosaic_cache_tb.sv` | `c48a220974ccf791…` |
| `sim/unit/tb_cache.cpp` | `523fefd5d77fdd86…` |
| `tools/run_mshr_controls.py` | `06371cee4a3229cf…` |

---

## 1. Registry entries touched (the shared-wrapper rule)

The case `cache.mshr_nonblocking` and the existing `cache.refill_evict_fault`
compile the **same** wrapper (`sim/tb/mosaic_cache_tb.sv`) and the **same** driver
(`sim/unit/tb_cache.cpp`). Once the wrapper instantiates `mosaic_mshr`, that
module is an elaboration dependency of *both* cases, so the integration lead
added `rtl/core/mosaic_mshr.sv` to `cache.refill_evict_fault`'s `rtl` list as
well. A case's `rtl` list is the elaboration dependency set of everything the
case compiles; two cases sharing a wrapper share a source list. No core file was
touched.

## 2. The interface

```systemverilog
module mosaic_mshr #(
  parameter int CPU_DATA_WIDTH = 64,
  parameter int LINE_BYTES     = 32,
  parameter int SETS           = 8,
  parameter int ADDR_WIDTH     = 32,
  parameter int MSHR_ENTRIES   = 4,
  parameter int ID_WIDTH       = 3,       // 1 << ID_WIDTH request ids
  localparam int CPU_BYTES, LINE_BITS, OFFSET_BITS, INDEX_BITS, TAG_WIDTH,
                 WORD_BITS, WORD_IDX_BITS, IDS, ENT_IDX_BITS, ENT_CNT_BITS, WT_CNT_BITS
) (
  input clk, rst,
  // CPU request (read only) + per-id cancellation
  input req_valid, input [ADDR_WIDTH-1:0] req_addr, input [ID_WIDTH-1:0] req_id,
  output req_ready,
  input cancel_valid, input [ID_WIDTH-1:0] cancel_id,
  // CPU response (one per cycle, no back-pressure)
  output resp_valid, output [ID_WIDTH-1:0] resp_id,
  output [CPU_DATA_WIDTH-1:0] resp_rdata, output resp_fault,
  // memory line port: request carries the address; the response carries it back
  output mem_req_valid, output [ADDR_WIDTH-1:0] mem_req_addr, input mem_req_ready,
  input mem_resp_valid, input [ADDR_WIDTH-1:0] mem_resp_addr,
  input [LINE_BITS-1:0] mem_resp_rdata, input mem_resp_fault,
  // state inspection + trace events
  input [INDEX_BITS-1:0] dbg_index, output dbg_valid, output [TAG_WIDTH-1:0] dbg_tag,
  output [LINE_BITS-1:0] dbg_data, output [ENT_CNT_BITS-1:0] dbg_outstanding,
  output [WT_CNT_BITS-1:0] dbg_waiters,
  output ev_hit, ev_miss, ev_coalesce, ev_refill, ev_fault, ev_cancel, ev_drop
);
```

Derived widths are `localparam`, not parameters, so a caller cannot build an
address width that disagrees with the array depth. Four `generate if` guards turn
a contract violation into an elaboration error.

## 3. The design as a rule

**Storage choice.** `mosaic_mshr` holds its *own* direct-mapped tag/data arrays;
it does not reuse `mosaic_cache`. `mosaic_cache` is blocking by construction (one
request, one FSM, one memory transaction at a time), and a non-blocking path
needs the tag lookup to proceed independently of an outstanding miss, which a
blocking pipeline cannot provide. Reusing the array would therefore have meant
rewriting the blocking cache into the non-blocking one; instead the blocking
cache is left exactly as I-042 delivered it and the non-blocking read path is a
separate module. There is no FP/PRF question here: this package is the cache
lane.

The rules, each checked by the case:

* **R1 — hits do not wait.** A request that hits is answered from the array in
  one cycle and never touches the MSHR. `refill-and-hit`.
* **R2 — coalescing.** A miss allocates an entry; a second request to a line
  already outstanding joins that entry as another waiter and **no second memory
  read is issued**. One line has at most one read in flight.
  `duplicate-miss`, `coalesced-fault`.
* **R3 — back-pressure, never a dropped miss.** A miss that finds every entry
  occupied is not accepted (`req_ready` low). `mshr-full`.
* **R4 — per-waiter word.** Each waiter remembers the word it asked for, so two
  coalesced requests to different words of one line each get their own word.
  `duplicate-miss`.
* **R5 — match by address, not by arrival order.** A response is matched to an
  entry by the line address it carries, so different lines may return out of
  order. Every waiter is answered exactly once; a request that was never
  accepted, or was cancelled, is never answered. `different-lines`, `id-reuse`.
* **R6 — cancellation is per waiter.** Cancelling one waiter never affects
  another waiter on the same line or a different entry. `coalesced-cancel`.
* **R7 — a cancelled refill is absorbed, not installed.** If every waiter on an
  entry was cancelled, the refill response is dropped (`ev_drop`), the line is
  **not** installed as valid, and the entry is freed — the table never leaks.
  `cancel-outstanding`.
* **R8 — a request cancelled before its read was issued** frees its entry and
  withdraws the pending read, so no memory request is ever made. `cancel-before-issue`.
* **R9 — conservation.** Every accepted miss becomes exactly one of a delivered
  response, an applied cancellation, or a live waiter. The testbench checks
  `miss_accepted == responses + cancels + dbg_waiters` **every cycle** (post-edge,
  where the waiter bits and the event pulses are both settled), so a leaked
  entry, a double-answered request or an entry that silently vanishes fails even
  if no directed check catches it. `final`.

**Why the flag-merge rule from the FP card does not apply here:** this package has
no architectural flags. Its precise-commit analogue is R9 — a squashed request
contributes nothing (no response, no install) and leaves no state behind.

## 4. Cases and phases

`CASE=cache.mshr_nonblocking`, 78 cycles of the 200 000 the registry allows,
**59 checks, 0 failures**. Measured trace: `hit=1 miss=21 coalesce=3 refill=14
fault=2 cancel=3 reads=17 max_outstanding=4`.

| Phase | What it establishes |
|---|---|
| `cold-reset` | after reset no set is valid and no entry or waiter is live |
| `refill-and-hit` | a cold load reads memory once, refills, installs; the second load **hits** and issues no read |
| `duplicate-miss` | two requests to one line (different words) issue **one** read, coalesce, and each id gets its own word; the line installs |
| `different-lines` | two lines in one set are both outstanding; their reads are answered **in reverse order** and each response carries its own id and its own data — the ordering property, driven rather than assumed |
| `cancel-outstanding` | a request cancelled while its refill is in flight gets **no** response, its refill is **not** installed (`ev_drop`), the entry is freed, and a retry refills the line a second time |
| `cancel-before-issue` | with the memory port stalled, a request cancelled before its read issued withdraws the read: no memory read, entry freed |
| `faulting-refill` | a poisoned refill faults the waiter, pulses `ev_fault`, installs nothing; the retry succeeds |
| `coalesced-fault` | two coalesced waiters on one poisoned line **both** fault from the single refill, and the entry frees |
| `mshr-full` | all four entries occupied; a fifth miss is back-pressured (not dropped); one response frees exactly one entry; the miss is then accepted; the table drains with no deadlock |
| `coalesced-cancel` | two consumers on one line; one is cancelled; the survivor is still answered and installs the line — *cancelling one load does not cancel other legal consumers* |
| `id-reuse` | an id whose request completed can be reused with no stale-response aliasing |
| `final` | no live entry/waiter; every accepted request answered or cancelled; conservation at rest; hit/miss/coalesce/refill/fault/cancel/drop all reached; `reads == refill + fault + drop` |

## 5. The oracle: the contract, not the RTL

The driver is not its own oracle.

* **Data.** Every non-faulting response's word is compared to a flat C++ byte
  array (`ref_mem_`) read at the *requested* address — not to anything the cache
  holds. The DUT never supplies the expected value.
* **Identity.** Each response id must be one that was accepted and not yet
  answered; a response for an unknown id, or for a cancelled id, or a second
  response for the same id, is a first-class mismatch. This is the identity rule
  the store queue (stale response) and the TLB (install generation) also carry.
* **Faults.** The fault policy is line-level and stated: a request issued while
  its line is poisoned is answered with a fault, and a faulted refill fails
  *every* waiter coalesced on it. The campaign sets the expectation at issue
  time; the memory model consumes the poison at the first read of the line.
* **Conservation** (§3 R9) is checked on every cycle, independent of any phase.

The memory model holds **several** reads and lets the campaign choose which one
to answer, which is what makes "different lines return out of order" real
stimulus instead of a claim about a single outstanding miss.

## 6. Gates

| Gate | Result |
|---|---|
| `python3 tools/run_unit.py --case cache.mshr_nonblocking` | **PASS** — `RESULT PASS cache.mshr_nonblocking mshr: … cycles=78` (59 checks) |
| `python3 tools/run_unit.py --case cache.refill_evict_fault` | **PASS** — the I-042 case is unchanged with the MSHR in the wrapper |
| `python3 tools/run_cache_controls.py` (I-042's own controls) | **PASS** — 3/3 mutants still caught |
| `python3 tools/run_mshr_controls.py` | **PASS** — 3/3 mutants caught (§8) |
| `verilator --lint-only -Wall` on `mosaic_mshr.sv` (shipping **and** all three `-D` variants) | **clean** |
| `python3 tools/lint_rtl.py --profile p0` / `--profile p1` | `mosaic_mshr.sv`: **clean as mosaic_mshr**; the two failures are the FP lane's in-progress `mosaic_core.sv` and `mosaic_fp_unit.sv` |
| `make lint-slang` (whole tree) | **PASS** (exit 0; `mosaic_mshr.sv` emits 5 style warnings, fewer than `mosaic_cache.sv`'s 34) |
| `make lint-cpp` | `sim/unit/tb_cache.cpp`: **clean**; the failure is the FP lane's `tb_core_sv39.cpp` (`Vmosaic_core_tb` is mid-edit) |
| `python3 tools/check_records.py` | **PASS** — `59 delivered package(s), 67 registered case(s)` |
| `python3 tools/check_coverage.py` | **PASS** |
| `python3 tools/check_exclusions.py` | mine is satisfied (a recorded PASS exists); the one remaining failure is `fp.precise_flags_and_boxing`, the FP lane's pending case |
| `make check` | RED **only** on `check-exclusions`, for that same FP case — no other sub-target fails |

The case builds and passes from a deleted build directory: `run_mshr_controls.py`
does exactly that before it builds a single mutant.

## 7. Reset and storage

`rst` clears exactly `valid`, the MSHR entries, the one-deep hit register and the
event pulses. It does **not** assign `data_mem` or `tag_mem`, the same contract as
`mosaic_cache` and `rtl/common/mosaic_ram.sv`. The testbench asserts the property
directly: `cold-reset` reads back every set invalid through `dbg_valid`.

## 8. Negative controls — the mutants

`tools/run_mshr_controls.py` rebuilds the case with exactly one defect injected
through a `-DMOSAIC_MSHR_MUTANT_*` define, from a deleted build directory, with
the define on the recorded command line. Each must produce a binary that differs
from the shipping one and `exit 1` with a named first failure:

| # | Define | Injected defect | First failure | Binary `sha256` |
|---|---|---|---|---|
| 0 | *(shipping)* | none | `RESULT PASS …` | `ac7fc52f1ad09fe9…` |
| 1 | `MOSAIC_MSHR_MUTANT_NO_COALESCE` | a duplicate miss is not coalesced: a second entry is allocated and a second read of the same line is issued — *"a duplicate miss issued twice"* | `CHECK FAILED duplicate-miss: the second request coalesced` | `8c7a83d9591074e6…` |
| 2 | `MOSAIC_MSHR_MUTANT_WRONG_ID` | a coalesced response carries the wrong requester id while its data still belongs to the real waiter — *"a coalesced response delivered to the wrong requester"* | `MISMATCH refill-and-hit: response for an id that was never accepted (id 1)` | `9db03aff019ece1c…` |
| 3 | `MOSAIC_MSHR_MUTANT_CANCEL_IGNORED` | the cancellation is dropped, so the squashed request keeps its waiter bit and its response is delivered and installed — *"a cancelled request's response installed as valid"* | `MISMATCH cancel-outstanding: a cancelled request received a response (id 4)` | `b3a8cbee2909821d…` |

Mutant 1 is also caught by the read-count check when the second read is visible in
time; the first failure it actually produces is the coalesce check, so the
control's expected text is the phase name `duplicate-miss` rather than one of the
two specific checks. Mutant 3 keeps the conservation identity consistent (the
cancel changes nothing at all), which is why the direct identity check fires
rather than the conservation check.

Output (verbatim, truncated to the result lines):

```
$ python3 tools/run_mshr_controls.py
building the shipping case from an empty directory...
  baseline exit=0 RESULT PASS cache.mshr_nonblocking mshr: … cycles=78
shipping binary sha256: ac7fc52f1ad09fe932c4f14410f80c631c0514e293ff960420e465794327a8ad
MOSAIC_MSHR_MUTANT_NO_COALESCE           1     OK
MOSAIC_MSHR_MUTANT_WRONG_ID              1     OK
MOSAIC_MSHR_MUTANT_CANCEL_IGNORED        1     OK
all 3 mutants mutate the binary, exit 1 and name the check they break
```

## 9. Not covered (honest list)

* **Shared return fabric / cross-cache coherence.** The card's "Covers: shared
  return fabric" is not claimed: this is a *unit-level* L1 with its own memory
  port. There is no interconnect, no cross-cache ordering and no coherent agent;
  that is I-039/I-047/I-093's territory.
* **Stores, write-allocate and dirty eviction.** `mosaic_mshr` is the read path
  only (loads/fetches): no store port, no dirty bit, no writeback. A line is
  always clean and may be dropped on eviction. Write-allocate over an MSHR is
  future work; I-042 owns the store/writeback policy.
* **The core is not wired to this cache.** Neither `mosaic_cache` nor
  `mosaic_mshr` is instantiated by `mosaic_core`; both are unit-level modules
  today. No core case compiles either file (§10).
* **Cross-package regressions were not re-run.** `core.act_dut` (p1),
  `core.corpus_sweep` (p0), `core.mem_program` (p0),
  `privilege.permission_matrix` (p1), `csr.rule_ledger`, `fp.operation_matrix`,
  `trap.precise_state`, `compressed.cross_boundary` and `sv39.walk_and_faults`
  (p1) are unaffected by construction (§10), and were deliberately not re-run
  while the FP lane (`FpState`) holds `mosaic_core.sv`, `mosaic_rename.sv`,
  `mosaic_dispatch.sv`, `mosaic_pkg.sv` and the new `mosaic_fp_unit.sv` mid-edit:
  a run now would report their in-progress state, not this package's. The
  integration lead runs the cross-package set once after all lanes land.
* **No external suite covers this.** The p1 ACT4 configuration excludes F/D and
  the caches, so the 127-ELF external suite does not exercise `mosaic_mshr`.
  What would: an ACT4 (or a directed) suite that actually routes load misses
  through this L1 — i.e. wiring it into `mosaic_core`'s LSU endpoint first, then
  re-running the p1 corpus through the cache-on configuration.
* **Multi-beat / narrow memory interface.** The memory port is line-wide (one
  beat per line). A burst refill is a later interface change.
* **Set-associativity, victim buffers, ECC, PMA/cacheability decode,
  flush/invalidate, randomised soak.** Not implemented; the module is a
  direct-mapped, clean, read-only L1 and the campaign is deterministic (the
  `seed` option is accepted but unused).
* **`make check` as a whole** is RED only on `check-exclusions`, for the FP
  lane's `fp.precise_flags_and_boxing`; every sub-target this package can affect
  is green (§6).

## 10. Non-interference, by construction

The nine cross-package cases above compile, between them, only core files. The
files this package changes or adds are `rtl/core/mosaic_mshr.sv`,
`sim/tb/mosaic_cache_tb.sv`, `sim/unit/tb_cache.cpp` and
`tools/run_mshr_controls.py`. Their intersection with the source list of **each**
of those nine cases is the empty set (checked programmatically against
`tests/unit/registry.json`), so none of them can observe this change. The two
cache cases that *do* compile the changed wrapper and driver were both re-run and
are green, with I-042's own mutant controls still 3/3.

## 11. How to reproduce

```sh
cd /Users/flare/MosaicRV
python3 tools/run_unit.py --case cache.mshr_nonblocking     # PASS (59 checks)
python3 tools/run_unit.py --case cache.refill_evict_fault   # PASS (I-042 intact)
python3 tools/run_mshr_controls.py                          # 3/3 mutants caught
python3 tools/run_cache_controls.py                         # 3/3 I-042 mutants caught
verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_mshr \
  -Ibuild/p0/rtl -Irtl/core -Irtl/common rtl/core/mosaic_mshr.sv   # clean
python3 tools/check_records.py                              # green
```
