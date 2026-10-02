# I-042 — L1 cache integration into the live core: **NOT DELIVERED**

Task card: `docs/implementation-plan.md` §I-042. Case: `cache.integrated_path`
(`tests/unit/registry.json`, `"pending": true`). This report is written because
the run that produced it exhausted its budget before the case passed. It records
what was built, what was verified, and the exact blocker, so the next run can
resume from a known point rather than from scratch.

**Verdict: the registered case does not pass. It has not been recorded, and the
`pending` marker in the registry must stay until it does.** The RTL below is
additive and inert with the caches disabled (the default), so the existing gates
are not expected to have moved; see "Safety of the landed changes".

---

## 1. Topology actually implemented

Two instances of one new wrapper are wired inside `rtl/core/mosaic_core.sv`:

| Where | Module | Notes |
|---|---|---|
| fetch request port → `imem` port | `mosaic_l1_cache_path #(IS_FETCH=1, LINE_BYTES=32, SETS=8)` | the L1I |
| LSU endpoint memory port → the PTE/data arbiter's EP slot | `mosaic_l1_cache_path #(IS_FETCH=0, …)` | the L1D |
| cache line port ↔ the core's ≤8-byte memory service | `mosaic_cache_line_bridge` (one per wrapper) | 4× doubleword beats per 32-byte line |
| storage | `mosaic_cache` (unchanged) | 8 sets × 32-byte line, direct-mapped; `READ_ONLY=1` for I, write-back/write-allocate for D |

The PTE walker is deliberately **not** behind the data cache: page tables are read
directly from `dmem`, so a translation is never served from a stale line.

New files (additive):
- `rtl/core/mosaic_cache_line_bridge.sv`
- `rtl/core/mosaic_l1_cache_path.sv`
- `sim/unit/tb_core_cache_path.cpp` (driver; builds, runs, currently FAILS)

Modified files:
- `rtl/core/mosaic_core.sv` — `cache_en_i` input; two wrapper instances; the
  data-side arbiter's EP slot re-pointed at the D-wrapper; a FENCE.I micro-FSM
  (D-cache write-back, then I-cache invalidate, holding the front end off for the
  whole window).
- `sim/tb/mosaic_core_tb.sv` — `cache_en_i`; the standalone directed cache-path
  DUT (`u_tb_cache_path`) and its ports; `imem_rsp_rdata_i` widened 32→64 bits
  (the instruction cache refills doubleword beats; fetch reads only `[31:0]`, so
  a driver that drives a 32-bit value is unchanged).
- `tools/gen_manifest.py` — emits `MOSAIC_<REGION>_CACHEABLE` and
  `mosaic_pa_cacheable(pa)` into the generated `mosaic_cfg_pkg`.
- `Makefile` — the memory-map file is now a prerequisite of the generated package.

## 2. The cacheability rule and where it comes from

`mosaic_cfg_pkg::mosaic_pa_cacheable(pa)` is **generated from
`config/memory/<profile>.json`**, region by region, from each region's own
`cacheable` flag. The wrapper sends a request to the cache only when
`cache_en_i && mosaic_pa_cacheable(addr) && !amo`; everything else — MMIO, the
boot ROM, an unmapped address, any atomic — is bypassed with its own address,
size and strobes. In p1 this makes RAM cacheable and every device non-cacheable;
in p0 nothing is cacheable, so the caches are inert by construction.

## 3. What was verified before the budget ran out

- `python3 tools/lint_rtl.py --profile p1`: `mosaic_cache.sv`,
  `mosaic_cache_line_bridge.sv`, `mosaic_l1_cache_path.sv`, `mosaic_core.sv` all
  clean. (The run is red only on `rtl/core/mosaic_vec_alu.sv`, another lane's
  in-flight work.)
- `mosaic_core_tb` elaborates with the new ports and the standalone instance.
- `CASE=cache.integrated_path` **builds** from a clean `obj_dir` with the new
  sources, and runs to completion.
- `CASE=core.mem_program` (p0) still **PASS** with the modified core.
- The directed phases already prove, on the standalone instance, that a device
  address is bypassed as an exact word access (not a line refill).

## 4. The blocker (observed)

The hand-built program mis-executes in *both* runs, so the on/off comparison and
every downstream check fail.

- Off run: the machine stops (`o_stopped=1`, `o_illegal=1`, one trap,
  `mcause=5`) after ~10 allocations; the signature and data words are never
  written.
- Root cause found and fixed during the run: the program formed addresses with
  `lui 0x80002`, which sign-extends to `0xFFFFFFFF80002000`; in **Bare** M-mode
  that is not the mapped physical address, so the first load faulted. The
  program now forms addresses with `auipc`/small positive offsets (fix applied
  in the driver), but the off run still does not reach the exit protocol and the
  data port sees no store traffic.
- A second, still-unexplained observation: the retire *event* payloads
  (`ev_rd`/`ev_value`) appear to lag `ev_pc` by one retirement for this program
  (the same decode is exact for the p0 corpus programs, and `core.mem_program`
  passes). This needs to be understood before the "identical retire stream"
  claim can be trusted, because it may mean the driver is reading the payload
  one cycle early rather than the core being wrong.

The directed refill-fault phase also still fails: the standalone instance does
not report a faulted refill as expected, so the standalone plumbing (reset,
`cb_*` handshake, or fault injection) needs debugging.

## 5. Not covered (would still be true once the case passes)

- No MSHR is wired in. The caches are **blocking** on both sides; a miss costs
  the whole cache and duplicate misses do **not** coalesce. I-043's MSHR is not
  integrated.
- No coherence. An external write (`ext_write_valid`) does not invalidate the
  D-cache; the I- and D-caches are made consistent only at `FENCE.I` (D flushed,
  I invalidated). The PTE walker bypasses the D-cache, so a program that stores
  through the LSU to a page table and then relies on the walker is not supported.
- A faulting *writeback* is not reported: the blocking cache's memory contract
  has no writeback response.
- The integrated instance uses a generated `cache_en_i` runtime enable; there is
  no compile-time "cacheless core" build.

## 6. Safety of the landed changes, and how to revert

With `cache_en_i = 0` (the value every pre-existing driver leaves, exactly as
`fab_dyn_i` is left) each wrapper is a wire: request, response and identity pass
through with no added state, and the FENCE.I micro-FSM never leaves `CF_IDLE`.
`core.mem_program` was re-run and passes; lint is clean on every file this change
touches. I have **not** re-run the other fourteen listed cases or ACT4, so their
status with these changes is unverified rather than claimed.

To revert this work entirely to the last green state (HEAD):

```
git checkout -- rtl/core/mosaic_core.sv sim/tb/mosaic_core_tb.sv \
                tools/gen_manifest.py Makefile
rm -f rtl/core/mosaic_cache_line_bridge.sv rtl/core/mosaic_l1_cache_path.sv \
      sim/unit/tb_core_cache_path.cpp results/reports/I-042-cache-path.md
git checkout -- build            # regenerate the generated package afterwards
```

## 7. Next steps to close

1. Debug why the off run stops after the first load (dump `o_dbg_*`/trap and the
   load-queue/store-queue state cycle by cycle).
2. Establish the correct sampling point for the retire event payloads (compare
   against `sim/unit/tb_core_mem.cpp`'s exact observe timing and its `rd` use).
3. Fix the standalone directed fault/eviction phases.
4. Then: the four controls (`MOSAIC_CACHE_MUTANT_FENCEI_NO_FLUSH`,
   `MOSAIC_CACHE_MUTANT_DEVICE_CACHED`, `MOSAIC_CACHE_MUTANT_DIRTY_DROP`,
   `MOSAIC_CACHE_MUTANT_FAULT_VALID`) with a `tools/run_cache_path_controls.py`,
   each rebuilt from a deleted build directory with its `-D` on the command and a
   binary hash differing from the shipping one.
5. Re-run the fifteen listed cases and ACT4 127/127.
6. Registry owner: add `rtl/core/mosaic_l1_cache_path.sv` and
   `rtl/core/mosaic_cache_line_bridge.sv` to the case's `rtl` list, and register
   the case for `p1` (p0 declares no cacheable region, so the cache-use
   assertion cannot hold there).
