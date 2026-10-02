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

---

# I-042, second attempt — the case passes: `cache.integrated_path` (p1)

**Verdict: PASS.** `results/unit/cache.integrated_path/p1/result.json`, 36 checks, 0
failures, `RESULT PASS cache.integrated_path cache integration: on/off identical,
caches used, FENCE.I visible, MMIO bypassed, directed controls clean`. The registry
entry is now `"profiles": ["p1"]` and lists
`rtl/core/mosaic_l1_cache_path.sv` and `rtl/core/mosaic_cache_line_bridge.sv`
(integration lead, verified from a clean build).

Sections 1–7 above are the first attempt's topology and diagnosis and are left as
written; where this attempt's evidence contradicts them, the correction is stated
here rather than edited into them.

## 1. The retire-payload observation: closed — **a harness defect, not a DUT defect**

The first attempt flagged that `ev_rd`/`ev_value` appeared to lag `ev_pc` by one
retirement for this program while the same decode is exact for the p0 corpus. The
verdict is that **the DUT is not at fault: the driver's bus model accepted and queued
memory responses during reset, and the core then matched those stale responses to
post-reset requests.** Every field was individually correct — the pc came from the
request slot, the rd/value from the instruction the machine actually executed — but
they described *different instructions*, which is exactly what "the payload lags the
pc" looks like from outside.

Evidence, in the order it was obtained:

1. **The DUT presents fetch requests while `rst` is high.** The fetch unit's pc
   register is held at the reset vector and its request slot is not yet recorded
   busy, so the request is re-offered every reset cycle. The pre-fix cycle trace
   shows it: `[imem-req] cyc=1 addr=80000000`, … `cyc=7 addr=80000000` — seven
   requests accepted during the eight reset cycles.
2. **The driver queued a response for each of them.** `Bus::Accept` computes the
   response immediately and hands it out two cycles later. The queue therefore held
   **seven responses for the reset vector**, and the trace shows them being consumed
   one per cycle — `[imem-pop] cyc=8 for=80000000 data=00000417`, then again at
   cycles 9 and 10 — while the DUT was already requesting `80000004` and `80000008`.
   Each pop is a distinct queued response, all for the same address, and the fetch
   unit accepted them for the pcs it had requested since.
3. **The core cannot reject it.** `mosaic_fetch` matches a response to a slot by
   `{id, epoch}` (`rsp_slot_owns`). Every request in that window carries `id=0,
   epoch=0`, and only a redirect advances the epoch, so a response from before reset
   is indistinguishable from a fresh one. The word delivered for pc *X* is the word
   that was fetched earlier, and the machine executes it *as* pc *X*.
4. **That is the whole observation.** Under the shift, the retired pc is right and
   the retired rd/value belong to the instruction whose word was re-delivered — so
   the payload appears to lag by one retirement. The first attempt's own trace is
   consistent with this and not with a lag: at the second retirement it reports
   `pc=0x80000004 rd=8 val=0x80000004`, which is the *first* instruction (an
   `auipc` with a pc-relative value) executed at the second pc, and the subsequent
   architectural evidence agrees — the load that followed faulted at
   `0x7ffff844`, i.e. with `s0 = 0x7ffff804`, the value the *shifted* stream
   reported, not the value the program's arithmetic gives.
5. **Order-swap experiment (same RTL, same program, same reset):** running with
   `cache_en_i=1` *first* is exact; running with `cache_en_i=0` shows the shift.
   The anomaly follows the harness window, not `cache_en_i` — the cache only changes
   how many stale responses are outstanding when the fetch unit starts. With the
   fix (the bus is silent during reset) the cache-off run is exact, and **no RTL
   line was changed to achieve that.**
6. **The smallest reproduction is two instructions.** In the pre-fix trace the shift
   is already visible at the *second* retirement (cycles 15–16 of that run). It is
   not a property of the program: it needs only that the first instruction after
   reset not redirect. The p0 corpus is immune by luck of its stimulus — its entry
   is a redirect (the reset-vector brick is a `JAL`, crt0 likewise), which advances
   the epoch and makes the queued stale responses `stale` and therefore dropped.
   `tb_core_mem.cpp` and the other core drivers accept requests during reset in
   exactly the same way; they do not exhibit the defect because their programs
   start with a jump. That is a latent hazard in the harness family, not a defect
   they currently show.
7. **Hardening note (not changed here, not required by any case):** the core's
   `{id, epoch}` space cannot distinguish a response that predates reset, because a
   reset does not advance the epoch. A future change could advance the fetch epoch
   on reset, which would make the core reject pre-reset responses on its own; the
   harness must be correct regardless, because it is the only party that knows the
   response predates reset.

Because the verdict is *not* a DUT defect, no case that reads the payload needed to
be re-run for correctness — but the four payload-reading cases that are cheap were
re-run anyway, unchanged, and pass (§7): `core.event_payload`,
`retire.width_and_order`, `mem.visibility_provenance`, `core.trap_csr_program`, plus
`core.corpus_sweep` from the required list.

## 2. The driver program: four defects, all in the stimulus

The program mis-executed in the baseline (cache-off) configuration for reasons that
had nothing to do with caching. All four are fixed in
`sim/unit/tb_core_cache_path.cpp`:

1. **`addi s0, s0, 0x800` is `s0 - 2048`.** A 12-bit immediate with bit 11 set is
   negative; the data base became `0x7ffff800` and the first load faulted outside
   the mapped RAM. The 0x800 adjustment is now `+0x7ff` then `+1`. (The first
   attempt's `lui` sign-extension fix was correct and is kept.)
2. **The self-modifying patch was an eight-byte store over a four-byte instruction**
   (`sd` where `sw` was meant), which zeroed the `jalr` that follows the patched
   word in the slot: the slot returned nowhere. Now `sw`.
3. **The image had a hole behind the slot.** The front end fetches past the slot's
   `jalr` and delivers what it finds before that jump's redirect lands; unfilled
   memory reads as zero, and a zero word is a *compressed illegal instruction*,
   which stops the core (`disp_unsupported`) even though the flow never falls
   through to it. The image now carries valid encodings past the slot. This is a
   property of the machine worth knowing (an unsupported encoding on a path the
   program never takes still stops it if the front end delivers it first); it is
   not a defect for this package to fix, and every other image in the tree is
   padded with valid encodings by its linker.
4. **The exit word is in cacheable RAM.** With the data cache on, the `tohost` store
   sits dirty in the cache and the harness — which observes memory — never sees it,
   so the cache-on run never reached the exit protocol. The program now ends with a
   `FENCE.I` after the `tohost` store: this core's FENCE.I writes the data cache
   back and then invalidates the instruction cache, which is the only data-cache
   writeback a program can ask for. The *same* program runs in both runs, so the
   on/off comparison is unaffected; with the caches off the fence is an ordinary
   retired instruction. **This is a real consequence of the integration and is
   stated as such**: a program whose exit word lives in a write-back cacheable
   region must flush before a memory-observing harness can see it.

Two further driver defects were found in the *directed* phases and are described in
§3.

The driver's stream comparison was also corrected to match the event contract: it
compared `gpr_write_rd` and `gpr_write_data` on every record, but
`config/contracts/event_v1.json` declares both valid only when `gpr_write_valid` is
1. On a record that writes no register the payload bus carries whatever the last
writer at that reorder-buffer index left, which is not an architectural value and is
not equal between two runs. The comparison now checks pc/len/`reg_we` on every
record and rd/value whenever the instruction writes a register.

## 3. The refill-fault phase: the driver's fault predicate was unsatisfiable

`Directed::SetFault(c & ~0x1full, ~0x1full)` injected no fault at all. The bus's
predicate is `addr & ~mask == base`, so the mask is the set of bits that do *not*
matter: passing `~0x1f` made it compare the low five bits of the address against the
line base, which is never true, and the "faulted" refill succeeded. It is now
`SetFault(c, 0x1full)`, which fails every beat of the line. With that, the standalone
phase proves what it was written to prove: a faulted refill reaches the CPU as a
fault, does **not** mark the line valid (`cb_dbg_valid_o == 0`), is retried rather
than hit on the next access, and installs the line once the memory is good. The
`MOSAIC_CACHE_MUTANT_FAULT_VALID` control (§6) is aimed at exactly this phase and is
caught by it, which is what makes the phase evidence rather than decoration.

## 4. The DUT defect found and fixed: a flush deadlock in `mosaic_l1_cache_path`

The path wrapper deadlocked on FENCE.I. The mechanism, and it is a real defect in the
integration (not in `mosaic_cache`, whose own `cache.refill_evict_fault` case still
passes):

* A flush of a dirty line is issued as *doubleword beats* through the bridge, and the
  bridge waits for the memory service's acknowledgement of **every** beat, writes
  included (`ST_WR_W`). The cache itself treats a writeback as complete when it is
  *accepted*, so the two disagree about when a writeback is over.
* The wrapper's `mem_rsp_ready_o` and the bridge's response input were gated on
  `S_CWAIT`/`S_BWAIT` only, so during `S_FLUSH` no response could be accepted — the
  first writeback beat was never acknowledged.
* The wrapper also left `S_FLUSH` as soon as the *cache's* flush completed
  (`cache_flush_done`), without waiting for the bridge to drain, which put it back in
  a state that does not accept responses with beats still in flight.

Result: `o_busy` never fell, `flush_done` never rose, the core's FENCE.I micro-FSM sat
in `CF_D` with `cache_fence_busy` high, and the front end was held off forever — a
hang, observed as `deliver=0`, no memory traffic and a stuck commit counter.

The fix (three lines, all in `rtl/core/mosaic_l1_cache_path.sv`):

* accept memory responses in `S_FLUSH` (`mem_rsp_ready_o` and `bridge_mem_resp_valid`
  include `S_FLUSH`; `capture_c` still excludes it, so a flush's writeback response
  is never mistaken for a CPU response);
* latch the cache's flush completion in `flush_ack_r` (it is a one-cycle pulse) and
  leave `S_FLUSH` only when `flush_ack_r && !bridge_mem_busy`.

The module header's rule R4 now states the drain requirement, because it is
load-bearing rather than incidental.

## 5. On/off evidence

One hand-built program, two runs from a fresh reset, `cache_en_i` low then high. All
of the following are checks in the case:

* **Architectural equivalence.** The retirement streams are identical through the
  program's own park loop (`jal x0, .`), which both runs reach at the same retirement
  index and then spin in; the comparison is bounded there and everything after it is
  required to be the park itself, so a machine that ran off past the park would fail.
  The four signature words, the four data words, and the deterministic values
  (DATA0 = 256, DATA1 = 768, MAGIC = 0x5a, IO_REC) agree, and the same instructions
  write them.
* **The caches are used.** The memory-side transaction counts are measured **on the
  DUT's own two memory ports** — every request the DUT offers and the driver accepts
  on `imem` (`imem_req_valid_o`/`imem_req_ready_i`) and on `dmem`
  (`dmem_req_valid_o`/`dmem_req_ready_i`) — with a **fresh bus model per run**, so the
  count is per run and counts *requests*, not lines: a 32-byte line refill is four
  counted beats, not one. Cache-off `4186` (`imem` 3147 + `dmem` 1039); cache-on
  `107` (`imem` 40 + `dmem` 67); ratio 0.026, a **39×** reduction. The case asserts
  the count *fell* and that it at least halved, per port and in total; a cache wired
  in but never hitting cannot pass. The device accesses are excluded from that
  conclusion: they bypass the cache and are counted exactly (below).
* **FENCE.I makes patched bytes visible.** The program stores a new instruction over
  its own code, executes FENCE.I and calls the slot again; the patched instruction
  writes 0x5a into MAGIC and the signature. Both runs execute the patched bytes
  (`sig[2] == 0x5a`, `MAGIC == 0x5a`), which requires the data cache's writeback to
  have reached memory before the instruction cache's refill — the ordering rule R4
  exists for.
* **A device access is not cached.** Three UART accesses appear on the data port
  exactly once each, with their own size, and no device address ever appears as a
  line refill; the directed phase proves the same thing on the standalone wrapper
  (one memory request, exact word access, never a line).
* **Directed phases** (standalone instance): a device address bypasses; a dirty line
  evicted by a conflict miss is written back byte for byte and reads back its stored
  value; a faulted refill does not mark the line valid, is retried, and installs the
  line once the memory is good.

**The honest cycle effect.** Cache-off `11337` cycles, cache-on `11587` cycles: the
caches cut memory-side traffic 39× and make the run **250 cycles (2.2%) slower**. The
causes are visible in the numbers and are not a surprise given what is wired: both
caches are blocking with no MSHR, so a *hit* still costs a full round trip through the
wrapper's `S_CWAIT` state and the cache's own hit path, while the modelled memory
latency is only two cycles. This program is latency-bound, not bandwidth-bound, so
fewer transactions buy no time. The transaction reduction is the integration's claim;
a speed claim is not, and is not made.

## 6. The four controls

`tools/run_cache_path_controls.py` (p1; each mutant built from a **deleted** build
directory with its `-D` on the build command, the command written next to the binary).
Shipping binary sha256
`f4290de0f021dbf3dad398fde6f0f8a4328ea9e921f164b19bd8f804e840f2e6` — byte-identical
to the binary the registered case builds (`build/p1/unit/cache.integrated_path/`), so
the controls' baseline *is* the registered build and not a lookalike. Every mutant's
hash differs from it, every mutant exits 1, and every mutant names the check it
breaks. The first-failure column is the first line the mutant's log carries that
names a failure.

| mutant | source | sha256 (first 16) | exit | first failure it produces |
|---|---|---|---|---|
| `MOSAIC_CACHE_MUTANT_FENCEI_NO_FLUSH` | `mosaic_core.sv` (fence FSM) | `4a6d6d03102db4cc` | 1 | `MISMATCH cache-on retirement stream: … retirement 2062: off pc=0x80000100 rd=10 we=1 val=0x5a, on … val=0x0` (the patched instruction's own result), then `SMC: FENCE.I made the patched instruction visible (cache-on)` |
| `MOSAIC_CACHE_MUTANT_DEVICE_CACHED` | `mosaic_l1_cache_path.sv` | `84e3318ff9439880` | 1 | `MISMATCH cache-on retirement stream: … retirement 2069: off pc=0x80000064 rd=14 we=1 val=0x0, on … val=0x2a` (the UART read's result), then `the three UART accesses reach memory once with the cache on (on)` |
| `MOSAIC_CACHE_MUTANT_DIRTY_DROP` | `mosaic_cache.sv` | `86ce0fac1d285bcc` | 1 | `MISMATCH cache-on retirement stream: … retirement 2062: off pc=0x80000100 rd=10 we=1 val=0x5a, on <end>` (the zeroed writeback destroys the patched word in memory and the instruction cache refills it) |
| `MOSAIC_CACHE_MUTANT_FAULT_VALID` | `mosaic_cache.sv` | `99524c874f294f54` | 1 | `CHECK FAILED: the faulted refill did not mark the line valid` |

`MOSAIC_CACHE_MUTANT_FENCEI_NO_FLUSH` and `MOSAIC_CACHE_MUTANT_DEVICE_CACHED` are new
(the first attempt named them as a to-do); the other two already existed in
`mosaic_cache.sv` for `cache.refill_evict_fault` and are reused rather than
duplicated. Two mutants are caught by the architectural comparison rather than by the
check the control was aimed at, and that is reported rather than tidied: a cached
device register and a zeroed writeback both change what the program computes, and the
first thing that notices is the on/off stream comparison — naming the instruction, its
pc and both values — with the aimed check failing immediately after.

## 7. Cases re-run

Every one of the fifteen was re-run **on the final tree** (after the last RTL edit, not
during it), with the profile its recorded result uses (`p1` where a `p1` result
directory exists, `p0` otherwise); none is assumed from an earlier run, and the
payload-reading cases named in §1 are re-run as well. `core.mem_program` failed once
in an earlier pass of this batch because the RTL was edited *while* that batch was
building it (a duplicate declaration existed for the seconds between two edits);
re-run on the frozen tree it passes, and the transient is recorded here rather than
quietly dropped.

| case | profile | verdict |
|---|---|---|
| `core.act_dut` | p1 | PASS (ACT4: applicable 127, generated 127, run 127, passed **127**, failed 0, `act_commit=96493a91448ca50780013fd892daec2c204487ba`, `config=tests/act4/mosaic-p1/test_config.yaml`) |
| `core.corpus_sweep` | p0 | PASS |
| `core.mem_program` | p0 | PASS |
| `boot.p1_contract` | p1 | PASS |
| `privilege.permission_matrix` | p1 | PASS |
| `sv39.walk_and_faults` | p1 | PASS |
| `tlb.sfence_vma` | p1 | PASS |
| `fence.code_and_data_order` | p0 | PASS |
| `mmio.exactly_once` | p0 | PASS |
| `amo.linearization` | p1 | PASS |
| `lrsc.reservation_progress` | p1 | PASS |
| `soc.bus_errors_and_ids` | p1 | PASS |
| `cache.refill_evict_fault` | p0 | PASS |
| `cache.mshr_nonblocking` | p0 | PASS |
| `fabric.integrated` | p0 | PASS |
| `core.event_payload` | p0 | PASS (payload-reading, §1) |
| `retire.width_and_order` | p0 | PASS (payload-reading, §1) |
| `mem.visibility_provenance` | p0 | PASS (payload-reading, §1) |
| `core.trap_csr_program` | p0 | PASS (payload-reading, §1) |

**Gates**, run on the final tree:

* `lint_rtl.py --profile p1` and `--profile p0`: **54 sources clean, both profiles**.
  The vector lane's `mosaic_vec_alu.sv` was in flight at dispatch and is clean now;
  nothing here touched `rtl/core/mosaic_vec_*.sv` or `sim/tb/mosaic_vec_tb.sv`.
* `lint-slang` (`slang-tidy`, `make lint-slang PROFILE=p1`): **exit 0**. It did reject
  the landed cache wiring, and that is worth recording: `mosaic_core.sv` used
  `cache_fence_busy` in `want_imem_req` at line 1323 and declared it at line 1379.
  Verilator accepts a use before its declaration; slang does not. Fixed by moving the
  declaration above its use (a declaration move, no logic change), which is the only
  RTL edit made after the first control run. The remaining slang output is the tree's
  pre-existing style warnings (`STYLE-16` generate blocks, `STYLE-7` `u_` instance
  prefixes, which every existing instance in this file carries) and does not fail the
  target.
* `make check`: exit 0. `check_records`: green — 68 delivered packages, 78 registered
  cases, every claimed case exists and belongs to the package claiming it.
  `check_exclusions` and `--negative`: green, 13/13 illegal ledgers rejected.
  `check_event_contract --profile p1` and `--negative`: green, 36/36 illegal
  interfaces rejected.
* `sim/unit/tb_core_cache_path.cpp` compiles clean under `-Wall -Wextra -Wshadow`
  (the same flags `make lint-cpp` uses); the dead helpers the first attempt left in it
  (an unused encoder, two unused address constants, an unused scalar payload overload,
  an unused retire-width constant, an unused `Cycle` parameter and an unused `Flush`
  helper) are removed rather than left to warn.

## 8. Not covered (still true)

* **No MSHR is wired in.** Both caches are blocking; a miss costs the whole cache and
  duplicate misses do not coalesce. I-043's MSHR is not integrated. The measured
  transaction reduction above is what a blocking cache buys, and no more.
* **No coherence.** `ext_write_valid` does not invalidate the data cache; the I- and
  D-caches are made consistent only at FENCE.I (D flushed, I invalidated). The PTE
  walker bypasses the data cache, so a program that stores through the LSU to a page
  table and then relies on the walker is not supported.
* **No writeback-fault path.** A faulting *writeback* is not reported: the cache's
  memory contract has no writeback response and the bridge treats acceptance as
  completion.
* **The exit word needs a flush** (§2.4): with the caches on, a store to a cacheable
  region is not visible to a memory-observing harness until the program fences.
* **`cache_en_i` is a runtime enable**; there is no compile-time cacheless build.
* A program whose sequential fall-through holds an unsupported encoding can stop the
  core even when the flow never reaches it (§2.3).
