# I-042 — Blocking L1I/L1D cache: refill, eviction and refill faults

Task card: `docs/implementation-plan.md` §I-042 (line 677) and
`docs/stage-3-memory-system.md` §I-042 (line 284). Case: `cache.refill_evict_fault`
(`tests/unit/registry.json`). This package is additive: it instantiates no core
module and changes no core file, so it cannot regress any other registered case.

Files owned by this deliverable:

| File | What it is |
|---|---|
| `rtl/core/mosaic_cache.sv` | the cache itself — one parameterised module used as L1I and L1D |
| `sim/tb/mosaic_cache_tb.sv` | `mosaic_cache_tb`, the Verilator top: two instances, all ports exposed |
| `sim/unit/tb_cache.cpp` | `CASE=cache.refill_evict_fault`, the campaign and the cache-off oracle |
| `tools/run_cache_controls.py` | the three negative controls (mutants) of §8 |
| `results/reports/I-042-cache.md` | this file |

Toolchain actually used: **Verilator 5.052 2026-09-05**, Python 3.9.6, macOS/arm64
(Darwin 25.6.0), Yosys 0.69+post (for the reset evidence in §4 only).

Compiled source revisions (working tree, `git rev-parse HEAD` =
`44ac2dab1299a5f09b5a9a6f8be5f178a03c5f5a`):

| File | SHA-256 (prefix) |
|---|---|
| `rtl/core/mosaic_cache.sv` | `8859de139e397ac6…` |
| `sim/tb/mosaic_cache_tb.sv` | `edc7dfe0599cdba3…` |
| `sim/unit/tb_cache.cpp` | `ec3e78dfe6691d59…` |
| `tools/run_cache_controls.py` | `96d10cb3762afc62…` |

---

## 1. The interface

```systemverilog
module mosaic_cache #(
  parameter int  CPU_DATA_WIDTH = 64,     // bits per CPU access (a word)
  parameter int  LINE_BYTES     = 32,     // bytes per line, power of two
  parameter int  SETS           = 8,      // sets, direct-mapped
  parameter int  ADDR_WIDTH     = 32,
  parameter bit  READ_ONLY      = 1'b0,   // 1 = instruction cache
  localparam int CPU_BYTES, LINE_BITS, OFFSET_BITS, INDEX_BITS, TAG_WIDTH, WORD_BITS
) (
  input clk, rst,
  input cpu_req_valid, cpu_req_we, input [ADDR_WIDTH-1:0] cpu_req_addr,
  input [CPU_DATA_WIDTH-1:0] cpu_req_wdata, input [CPU_BYTES-1:0] cpu_req_wmask,
  output cpu_req_ready, cpu_resp_valid, cpu_resp_fault,
  output [CPU_DATA_WIDTH-1:0] cpu_resp_rdata,
  output mem_req_valid, mem_req_we, output [ADDR_WIDTH-1:0] mem_req_addr,
  output [LINE_BITS-1:0] mem_req_wdata, input mem_req_ready,
  input mem_resp_valid, input [LINE_BITS-1:0] mem_resp_rdata, input mem_resp_fault,
  input flush_valid, output flush_ready, flush_done, flush_busy,
  input [INDEX_BITS-1:0] dbg_index,
  output dbg_valid, dbg_dirty, output [TAG_WIDTH-1:0] dbg_tag,
  output [LINE_BITS-1:0] dbg_data,
  output ev_hit, ev_miss, ev_refill, ev_writeback, ev_fault
);
```

The geometry derived in the port list (`CPU_BYTES`, `LINE_BITS`, the three bit
counts) is a `localparam`, not a parameter, so no caller can create an address
width that disagrees with the array depth. Four `generate if` blocks turn a
contract violation into an elaboration error rather than a silent misbuild.

**Blocking by construction.** One request port, one response. A request is
accepted on an edge where `cpu_req_valid && cpu_req_ready`; `cpu_req_ready` is
high only in `ST_IDLE`. The cache then completes that request — hit, or miss and
refill, or miss and dirty-writeback and refill — before it can accept another.
There is no MSHR, no outstanding-miss table, and no reordering. That is the
card's stated trade-off: small and blocking first, correct above fast. I-043
builds the non-blocking path on top.

**Memory port.** Line-wide, one request/response channel. A request is
`{mem_req_we, mem_req_addr, mem_req_wdata}` accepted when
`mem_req_valid && mem_req_ready`; a writeback is complete at acceptance, a
refill's data arrives later as `mem_resp_valid` (latency at least one cycle, so a
response is never in the cycle a request was accepted). `mem_req_ready` may be
low for any number of cycles; the FSM holds the request and holds the CPU.

## 2. The write policy, stated as a rule

**WRITE-BACK, WRITE-ALLOCATE.** The rule is written in the module header and
checked clause by clause by the case:

1. A store that **hits** merges the byte lanes selected by `wmask` into the line
   and marks the line **dirty**. It does not go to memory.
2. A store that **misses** allocates: the line is refilled first
   (write-allocate), then the store bytes are merged and the line is marked
   dirty.
3. A dirty line is written back to memory exactly when it is evicted by a refill
   that needs its way, or by an explicit `flush`. A clean line is dropped without
   a writeback.
4. A `READ_ONLY` cache never marks a line dirty; a store offered to it faults and
   changes nothing; its flush only drops tags.

The alternative (write-through) would need no writeback but would put every store
on the ordered memory port. Write-back is chosen because the card asks for a
*stated* policy and because dirty-eviction and self-modify are precisely the
cases that falsify it. The rule is exercised by clauses 1–3 in
`conflict`/`dirty-eviction`/`self-modify` and clause 4 in `icache`.

## 3. Cases and phases

`CASE=cache.refill_evict_fault`, 167 cycles of the 200 000 the registry allows,
42 checks, 0 failures. Each named case from the card is a phase, and each phase
both drives the DUT and asserts on its results:

| Phase | What it establishes |
|---|---|
| `cold-reset` | after reset every one of the 8 sets in **both** caches reads back invalid (via the `dbg_*` view) |
| `refill` | a cold load misses, refills, and the second load hits; the rest of the line is resident; events are pinned to the accesses (one refill for the line) |
| `conflict` | two addresses in the same set evict each other; a store to A is read back, B's load evicts A, A is reloaded from the writeback |
| `dirty-eviction` | two dirty words in one line survive eviction: `ev_writeback` pulses once, the backing line matches the reference byte for byte, and both words read back |
| `self-modify` | store→load through the same cache; a byte store changes exactly one byte; a store to word2 leaves word0 alone |
| `refill-fault` | a poisoned refill faults the load, `ev_fault` pulses, the line is **not** valid, the retry succeeds and then hits; a poisoned store refill also faults without writing |
| `icache` | a cold fetch refills, the second hits; a store to the I-cache faults and leaves memory unchanged; a store to a cold I-cache line faults and allocates nothing |
| `backpressure` | the memory port is held not-ready for 3 cycles before a miss; the trace is unchanged |
| `valid-init` | load a line (it becomes valid), reset, all sets invalid again, the line is refilled from changed backing memory rather than served stale |
| `final-drain` | flush both caches, compare the full 64 KiB backing images, and check the trace invariants |

**Traceability is asserted, not just printed.** The DUT emits one-cycle
`ev_hit`/`ev_miss`/`ev_refill`/`ev_writeback`/`ev_fault` pulses; the testbench
counts them and, at the end, checks:

* every accepted memory read ends in exactly one refill or one fault
  (`DcMemReads == refill + fault`, same for I);
* every accepted memory write is exactly one writeback
  (`DcMemWrites == writeback`, and the I-cache writes and writes back nothing);
* every miss ends in a refill or a fault (`miss == refill + fault`);
* the program actually reached hit, miss, refill, writeback and fault at least
  once on the D-cache and hit, miss and refill on the I-cache. A rule no
  stimulus reaches fails the case here rather than being skipped.

Measured counts: D `hit=14 miss=15 refill=13 wb=4 fault=2`; I `hit=2 miss=1
refill=1 fault=0`; 167 cycles.

## 4. Reset strategy: valid bits yes, data RAM no

`rst` clears exactly `valid`, `dirty`, the control registers and the event
pulses. It does **not** assign `data_mem` or `tag_mem`: there is no reset
assignment to either array anywhere in the file. Validity is tracked outside the
array, so a `SETS × LINE_BITS` array reset — which would turn an inferrable block
RAM into `SETS × LINE_BITS` reset flops — is neither needed nor present. This is
the same reset contract as the RAM wrapper (I-006, `rtl/common/mosaic_ram.sv`).

The testbench asserts the property directly: `cold-reset` and the mid-run
`valid-init` reset both read back **every set invalid** through the `dbg_valid`
port, and `valid-init` additionally proves the consequence — a store written to
the backing memory while the cache was reset is refetched rather than served
stale from the old line. A cache that failed to clear `valid` would fail both the
`dbg_valid` assertion and the refetch check.

Synthesis evidence that the reset footprint is control-only (Yosys 0.69+post):

```
$ yosys -p 'read_verilog -sv rtl/core/mosaic_cache.sv; hierarchy -top mosaic_cache -check; \
    proc; opt; memory; opt; memory_map; opt_clean; \
    select -count t:$adff t:$adffe t:$dffsr t:$dffsre; \
    select -count t:$sdff t:$sdffe; \
    select -assert-max 32 t:$adff t:$adffe t:$sdff t:$sdffe t:$dffsr t:$dffsre; \
    select -assert-none t:$adff t:$adffe t:$dffsr t:$dffsre'
...
0 objects.          <- async-reset cells
18 objects.         <- sync-reset cells (the control state: FSM, valid[8], dirty[8])
$ echo $?
0
```

18 reset-bearing flops is the entire reset cost and is independent of `LINE_BYTES`
(it is `valid[8]`, `dirty[8]`, the FSM state and one pipeline bit); the storage
arrays elaborate as resetless enable-flops (2056 `$dffe` cells). Eighteen reset
flops cannot cover a 2240-bit array, so no path resets array contents. That is
what "valid bits are initialised without resetting the data RAM" means in cells.
There are zero asynchronous-reset cells.

## 5. Refill error handling

The memory port can fail a refill (`mem_resp_fault`). On a failed refill the
cache **must not** mark the line valid and **must not** answer with data: it
raises `cpu_resp_fault` for that request, leaves tag/valid/data untouched,
and pulses `ev_fault`. A later request to the same line therefore misses again
and retries the refill. `refill-fault` drives this both ways (load and store),
asserts the fault, asserts `dbg_valid == 0` for the failed set, and proves the
retry succeeds and then hits. This is the card's first named Fail mode and the
`MOSAIC_CACHE_MUTANT_FAULT_VALID` control of §8.

## 6. Cache-on vs cache-off: the oracle is the contract, not the RTL

The driver runs every access twice:

* **cache-on** — through the DUT (`mosaic_cache`), with the driver servicing the
  line port from its backing store; and
* **cache-off** — through a flat C++ byte array addressed directly.

The reference knows nothing about tags, sets, hits or misses. It knows "a read
returns the memory word unless this line's first touch faults" and "a write
merges the selected bytes". Fault and (for reads) the returned word must match
**for every access**, so the equivalence is established op by op, not only at the
end. The DUT's dirty lines are its own until it evicts them; after the final
flush the two 64 KiB backing arrays are compared and must be byte-identical. If
the oracle had re-implemented the cache, it would have agreed with a wrong cache;
this one cannot.

The fault policy is line-keyed and first-touch on both sides, so even though the
DUT issues far fewer memory transactions than the reference (hits do not touch
memory), the two sides agree on *which* access faults.

## 7. Gates

| Gate | Result |
|---|---|
| `python3 tools/lint_rtl.py --profile p0` | **PASS** — `lint: 43 source file(s) clean`, including `mosaic_cache.sv` |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl <all rtl>` | **PASS** (exit 0; only the project-wide `STYLE-2` port-suffix warnings that every existing module also emits) |
| `python3 tools/check_records.py` | **PASS** — `50 delivered package(s), 60 registered case(s)`; `cache.refill_evict_fault` remains `"pending": true` because the integration lead clears that marker when recording |
| `python3 tools/run_unit.py --case cache.refill_evict_fault` | **PASS** — `RESULT PASS cache.refill_evict_fault trace equivalent: …` |
| `python3 tools/run_cache_controls.py` | **PASS** — all 3 mutants caught (§8) |
| `make check-config`, `check-contracts`, `check-coverage`, `check-capability-matrix`, `check-isolation`, `check-docs` | **PASS** |
| `make check` (whole) | **RED, not from this package** — `check-event-contract` fails on `rtl/core/mosaic_retire.sv` (`ev_insn`/`ev_len` not mapped to the event schema). That file is not modified by and not part of this package. Reported rather than worked around. |

The case builds and passes from a deleted build directory (the controls script
does exactly that), so the PASS is not a stale-object artifact.

## 8. Negative controls — the mutants

`tools/run_cache_controls.py` rebuilds the case with exactly one defect injected
through a `-DMOSAIC_CACHE_MUTANT_*` define, from a deleted build directory, with
the define on the recorded command line (`build/p0/cache_controls/<mutant>/build_command.txt`).
Each must produce a binary that differs from the shipping one and `exit 1` with a
named first failure:

| # | Define | Injected defect (card's Fail mode) | First failure | Binary `sha256` |
|---|---|---|---|---|
| 0 | *(shipping)* | none | `RESULT PASS …` | `cc337a7d0be89a43…` |
| 1 | `MOSAIC_CACHE_MUTANT_FAULT_VALID` | a faulted refill is still installed as valid, so the next access hits a line memory never delivered — *"an error refill marked valid"* | `MISMATCH refill-fault: retry load read data: expected 0x56d14ccf4ac540c3, got 0x0000000000000000` | `eea7f79f4d2b0be2…` |
| 2 | `MOSAIC_CACHE_MUTANT_DIRTY_DROP` | the eviction is issued with zeroed data, so dirty bytes never reach memory — *"an eviction loses dirty bytes"* | `MISMATCH conflict: reload A after eviction read data: expected 0x1111222233334444, got 0x0000000000000000` | `4062f5cb532dfc7b…` |
| 3 | `MOSAIC_CACHE_MUTANT_WRONG_REFILL` | the refill reads the line after the one that missed — *"a wrong refill address"* | `MISMATCH refill: first load read data: expected 0xfd7ae764e16eeb68, got 0x1d9a0784018e0b88` | `9e669e8f6d74fc9c…` |

All three make the case FAIL (exit 1) on a check that names the defect, and each
binary differs from the shipping one. Mutant 1 is caught twice over: the
`dbg_valid == 0` assertion fires first (a load that never installed), and the
trace comparison catches the subsequent bogus hit. Mutant 2 is caught both by the
per-line backing-memory comparison in `dirty-eviction` and by the final image
comparison. Mutant 3 is caught at the very first refill. The mutants are reached
only by the hand-built control command; `tools/run_unit.py` passes no `-D`, so the
shipping build contains none of them.

Output (verbatim, truncated to the result lines):

```
$ python3 tools/run_cache_controls.py
building the shipping case from an empty directory...
  baseline exit=0 RESULT PASS cache.refill_evict_fault trace equivalent: …
shipping binary sha256: cc337a7d0be89a4317195a98aeb431d30da92924ef6f89b57212380eec97aca8
MOSAIC_CACHE_MUTANT_FAULT_VALID          1     OK
MOSAIC_CACHE_MUTANT_DIRTY_DROP           1     OK
MOSAIC_CACHE_MUTANT_WRONG_REFILL         1     OK
all 3 mutants mutate the binary, exit 1 and name the check they break
```

## 9. Not covered (honest list)

* **Non-blocking / MSHR path.** No outstanding misses, no merge, no reorder:
  that is I-043. This cache serialises misses by construction.
* **I-cache/D-cache coherence after a store (self-modifying code, FENCE.I
  visibility).** The I-cache here is read-only and the D-cache is write-back, and
  nothing forwards a store to the instruction cache or invalidates it. The
  `self-modify` phase is *within one cache* (a store read back through the same
  data cache). FENCE.I architectural visibility is V-016's, and is not claimed
  here.
* **PMA / cacheability decode.** The cache has no cacheability input; the
  decision "this access is cacheable" is made outside the module (I-039/I-093).
  The case exercises only the cacheable path.
* **Partial-line / multi-beat memory interface.** The memory port is line-wide
  (one beat per line). A narrower bus with burst refills is a later interface
  change and is not modelled.
* **Write-through mode, victim buffers, ECC, set-associativity.** Not
  implemented; the module is a direct-mapped write-back cache only.
* **Randomised soak.** The case is deterministic (no RNG; the `seed` option is
  accepted but unused). Conflict/refill/dirty/self-modify/fault are directed;
  a randomised conflict campaign is left to a future suite.
* **`make check` as a whole.** Red on `rtl/core/mosaic_retire.sv` (an event tap
  declaration the schema does not map), a file this package does not touch and
  does not modify; every other `make check` sub-target passes.

## 10. How to reproduce

```sh
cd /Users/flare/MosaicRV
python3 tools/run_unit.py --case cache.refill_evict_fault      # PASS
python3 tools/run_cache_controls.py                            # 3/3 mutants caught
python3 tools/lint_rtl.py --profile p0                         # clean
python3 tools/check_records.py                                 # green
```
