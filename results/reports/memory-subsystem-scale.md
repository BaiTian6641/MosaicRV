# Memory-subsystem scale: an 8 KB, config-driven, non-blocking L1

Deliverable: enlarge the L1s to the profile's declared geometry, make the L1 hit
path non-blocking with a real outstanding-miss table, and measure the change on
equal resources. This is the "8 KB caches + non-blocking" half of the memory
step; the multi-outstanding allocation work the plan gives to I-043 is discussed
under "Not covered", and coherence is I-065's and is not done.

Toolchain: **Verilator 5.052 2026-09-05**, Python 3.9.6, macOS/arm64.

Files this deliverable owns or changed:

| File | What changed |
|---|---|
| `config/geometry/p1.json` | `caches` block: 64 B lines, 32 sets, 4 ways, 4 MSHRs (8 KB each) |
| `tools/gen_manifest.py` | emits the `caches` block into `mosaic_cfg_pkg` (`MOSAIC_CACHE_*`, `MOSAIC_L1I_*`, `MOSAIC_L1D_*`, `MOSAIC_MSHR_ENTRIES`) |
| `rtl/core/mosaic_cache.sv` | rewritten: set-associative (round-robin), non-blocking read path with an outstanding-miss table, coalescing, and a line-invalidate port |
| `rtl/core/mosaic_mshr.sv` | associativity (`WAYS`) and a flush port; now instantiated as the L1I engine |
| `rtl/core/mosaic_l1_cache_path.sv` | L1I is `mosaic_mshr`, L1D is the non-blocking `mosaic_cache`; geometry from parameters |
| `rtl/core/mosaic_core.sv` | cache geometry read from the generated package; the bypassed-AMO invalidate pulse |
| `rtl/core/mosaic_prefetch.sv` | one-line fix: the line-number pad was the literal `5'b0` (see "A cross-file fix") |
| `sim/tb/mosaic_cache_tb.sv`, `sim/unit/tb_cache.cpp` | new ports; the non-blocking L1D campaign |
| `sim/unit/tb_core_cache_path.cpp`, `tb_core_locality.cpp`, `tb_core_perf.cpp` | drain window (the exit word is a cacheable store now flushed by a 128-line walk) |
| `tools/run_memscale_controls.py`, `tools/run_memscale_measure.py` | the four controls and the before/after measurement |

---

## 1. The geometry, and where it is declared

The profile declares it, once:

```json
"caches": { "line_bytes": 64, "l1i_sets": 32, "l1i_ways": 4,
            "l1d_sets": 32, "l1d_ways": 4, "mshrs": 4 }
```

`tools/gen_manifest.py` turns that block into `mosaic_cfg_pkg` constants
(`MOSAIC_CACHE_LINE_BYTES`, `MOSAIC_L1I_SETS/WAYS`, `MOSAIC_L1D_SETS/WAYS`,
`MOSAIC_MSHR_ENTRIES`), and `mosaic_core.sv` reads them into `CORE_CACHE_*` /
`CORE_L1I_*` / `CORE_L1D_*`, which the two cache-path instantiations and the
`o_geom_cache_*` evidence ports all share. A profile with no `caches` block
(p0) gets the historical direct-mapped 32-byte/8-set stand-in emitted, so a
cache-off machine elaborates exactly as before.

**The layout chosen: 32 sets x 64 B x 4 ways = 8192 B = 8 KB, for both L1I and
L1D.** Four ways rather than one because a replacement policy is written (a
round-robin victim pointer per set, `rr`), so associativity costs nothing extra
in complexity and removes the direct-mapped conflict misses the streaming
program is full of. The L1D is `mosaic_cache`; the L1I is `mosaic_mshr` (below).

The unit-level cache cases keep the 32 B/8-set/1-way wrapper geometry, because
that wrapper is the I-042/I-043 campaign's own DUT and is registered for it.

## 2. The non-blocking mechanism, and the coalescing rule

`mosaic_cache` keeps its write-back/write-allocate policy and its arrays, and
replaces the blocking FSM's "one request, then nothing" with an outstanding-miss
table:

* **A hit is answered from the array in the next cycle and never waits for a
  miss.** `cpu_req_ready` stays high for a hit while the table is full.
* **A miss that names a line the table already holds joins that entry** -- the
  waiter is appended to the entry's small FIFO (word, `we`, `wmask`, `wdata`),
  `ev_coalesce` pulses, and **no second memory read is issued**. One line has at
  most one read in flight, and every waiter on it is answered from that one
  response.
* **A miss that needs a new entry and finds the table full is not accepted**
  (`cpu_req_ready` low): back-pressure, never a dropped miss.
* Every waiter is answered exactly once, with **its own word**; the entry frees
  exactly when its last waiter has been answered. A faulted refill is never
  installed and answers its waiters with a fault, so a later request retries the
  refill.
* Entries are allocated into the **lowest free** entry and issued oldest-first,
  so index order is allocation order and responses come out in the order a
  single-issue consumer expects.

The memory port is single-outstanding (that is the interface the width adapter
and the core's port give it), so a refill is offered one at a time; the table
still *holds* several lines, which is what lets a hit overtake a miss.

**The L1I is I-043's `mosaic_mshr` itself** -- the verified, id-tagged,
coalescing, read-only MSHR -- instantiated in the L1 path (the instruction side,
where the fetch engine has four credits to spend). It gained `WAYS` and a flush
port; its own rules (response identity by address, cancellation, the
conservation identity) are unchanged and its case still passes.

## 3. The outstanding-transaction contract, and why the endpoint stayed at one

The LSU endpoint (`mosaic_lsu_endpoint`) still holds one memory transaction, and
the measurement below is the reason it was **not** raised: on the data side the
binding limit is not the endpoint but the **load queue**, which issues one load
at a time by construction --

> "The memory endpoint (I-033) holds one transaction at a time and this queue
> issues one load at a time, so completion is in order and there is no reorder
> buffer here." -- `rtl/core/mosaic_load_queue.sv`, "not covered"

-- and `mosaic_load_queue.sv` is not this lane's file. Raising the endpoint's
outstanding count alone would change nothing measurable, because nothing
upstream offers it a second request while the first is outstanding; the
`mshr` row of the table is exactly that fact, measured (identical to `before`).
What would add real data-path MLP is the I-043/plan work: a load queue that
issues more than its head, and an endpoint with an id-tagged outstanding table
whose responses the queue can match -- a redesign this lane did not undertake
because the ticket asks for the measurement that shows the limit instead.

The endpoint's fault semantics are **unchanged**: misalignment still traps from
the address alone before the memory system sees the access, a PMP denial still
builds its response without entering `ST_REQ`, and the AMO/LR/SC classes are
untouched. (`lsu.size_fault_boundaries`, `lsu.byte_forwarding`,
`store.wrong_path_visibility`, `amo.linearization`, `lrsc.reservation_progress`
all re-run green.)

## 4. The coherence obligation the bigger L1 exposed

A non-coherent L1 plus a *bypassed* atomic is a real hazard, and the bigger L1
turned a latent one into a live failure. An AMO is bypassed (its
read-modify-write cannot be split across a cache), so it changes memory behind
the cache's back; if the cache still holds that line -- clean, from an earlier
load -- a later load hits the value from before the atomic. The old 256 B L1 hid
this only because the locality program's `F`-load happened to evict `E` first;
with an 8 KB L1 nothing evicts anything, and
`locality.integrated_path`'s "the load after the AMO sees memory (105)" failed.

The fix is the one coherence act a non-coherent L1 owes and no more: on the
atomic's own beat, `mosaic_core` pulses the L1D's new `inv_valid`/`inv_addr`
with the line address, and `mosaic_cache` **drops that line, writing it back
first if it was dirty** (a dirty copy must reach memory before its tag goes, or
the atomic's write would be the only survivor). A store *through* the cache
needs no pulse. This is not a coherence protocol -- there is still no
cross-cache ordering, no snoop, no interconnect (I-065); it is the minimum that
keeps a bypassed atomic from being masked by a stale copy.

## 5. Measurement, on equal resources (I-084)

`tools/run_memscale_measure.py` rebuilds `CASE=perf.equal_resource_compare`
four ways and reads the `stream` (and `alu_chain`) baseline row from each run.
The I-084 harness already asserts equal resources and architectural identity per
configuration, so each number is a measurement of *this* geometry on *these*
programs with seed 1 -- nothing more general.

| arm | L1 | read path | stream cycles | stream insns | IPC | stream dmem beats | imem beats |
|---|---|---|---|---|---|---|---|
| before | 256 B | blocking | 4232 | 370 | 0.087 | 394 | 36 |
| size | 8 KB | blocking | **3458** | 363 | 0.104 | **230** | 40 |
| mshr | 256 B | non-blocking | 4232 | 370 | 0.087 | 394 | 36 |
| both | 8 KB | non-blocking | **3458** | 363 | 0.104 | **230** | 40 |

**What each number is:** `stream` is I-060's own streaming program run in the
perf harness's *baseline* configuration (steering, LLB, prefetch, coalescing and
lane quota all off), with the L1 enabled in every arm; `cycles` is the whole run
to the exit word; `dmem` counts *doubleword beats on the data port*, not
misses -- and the line size differs between arms (8 beats/line at 64 B, 4 at
32 B), so the beat counts are not apples-to-apples across the `before` row. The
comparable statement is the cycle count and the direction of the beat count.

**What it shows.** Enlarging the cache is the whole win: 4232 -> 3458 cycles
(-18.3%) and 394 -> 230 data beats (-41.6%), and the miss rate falls further
than that, since each 64 B refill now costs twice the beats of a 32 B refill.
The non-blocking read path **alone is exactly neutral** on this workload (4232
cycles, 394 beats, byte-identical to `before`) -- the measurement that shows the
load queue, not the cache, is the data-path limit (section 3). `both` is
identical to `size`, so on this workload the MSHR adds nothing measurable: it is
architectural headroom for a load queue that can use it, not a speed-up today.

**The control workload moved, and that is a finding.** `alu_chain` (a dependent
ALU chain *with two loads*) is not unchanged:

| arm | alu_chain cycles | insns | dmem beats |
|---|---|---|---|
| before | 3349 | 545 | 20 |
| size | 3373 | 538 | 40 |
| mshr | 3349 | 545 | 20 |
| both | 3373 | 538 | 40 |

The geometry change moves it (+24 cycles, -7 retired instructions, +20 data
beats) and the read-path change does not. The harness's own architectural
identity check (the retire stream, field by field, through the park loop) still
passes, and `dmem` rises partly because 64 B refills are eight beats instead of
four -- so this is a tail-sampling effect of the program's two loads changing
hit/miss and the harness's fixed drain, not a change to the ALU path. It is
reported here rather than explained away: a memory change that moves an
ALU-dependent chain is exactly the signal the ticket asks to be surfaced.

## 6. Controls (negative), each a `-D` from a deleted build directory

`tools/run_memscale_controls.py` builds `CASE=cache.mshr_nonblocking` once per
defect from an empty directory (its `-D` is recorded in that build's
`build_command.txt`), runs it, and requires a binary that differs from the
shipping one, exit 1, and the named check as the first failure.

Shipping binary sha256:
`679427e8b3e2547bda5a36bb9467b1959741c5f3ff87ee1323f590ada50fc2ce`

| define | defect injected | exit | first failure | binary sha256 |
|---|---|---|---|---|
| `MOSAIC_CACHE_MUTANT_WRONG_WAITER` | a coalesced response carries another waiter's word | 1 | `CHECK FAILED: nb-coalesce-words: every coalesced waiter gets its own word` | `fc631d5777b4778d534a689576ba19b4b9510ef6a284b17fd495a89e78d1c391` |
| `MOSAIC_CACHE_MUTANT_HIT_BLOCKS_MISS` | a hit is stalled behind an unrelated miss | 1 | `CHECK FAILED: nb-hit-during-miss: a hit is accepted while a miss is outstanding` | `409f70beaf5030d4be12906ad64fc0ca2d935e1cd09a06661521d3cd9fe0fcfc` |
| `MOSAIC_CACHE_MUTANT_FAULT_VALID` | a faulted refill is installed as valid | 1 | `MISMATCH nb-refill-fault: poisoned fault response: expected 1, got 0` | `17a83fad8f9ea91f44fdf94ecbfe81dfb04398051ce7b5767058b99884617e6f` |
| `MOSAIC_CACHE_MUTANT_DIRTY_DROP` | an eviction loses its dirty bytes | 1 | `CHECK FAILED: nb-dirty-evict: the victim lost no byte on eviction` | `e3580caca04ed3ec67c5b972b9c7a5e253d702b49372bc7c8817adf7c8bb39bf` |

All four mutate the binary, exit 1, and name the check they break. The
`nb-*` checks are the new non-blocking L1D campaign in `sim/unit/tb_cache.cpp`
(cold reset, refill fault, dirty eviction, hit-during-miss, coalesced words, a
four-entry table filled at once); the shipping run is built and passes first.

The I-042 and I-043 controls are unchanged and still 3/3 each
(`tools/run_cache_controls.py`, `tools/run_mshr_controls.py`).

## 7. A cross-file fix, declared

Making `line_bytes: 64` real exposed a hard-coded 32-byte assumption in
`rtl/core/mosaic_prefetch.sv:295-296`: the line-number pad was the literal
`{5'b0, ...}`. It is now `{(32-ADDR_WIDTH+OFFSET_BITS){1'b0}, ...}` -- a no-op
at 32 B, correct at 64 B. That file belongs to the I-060 locality lane, which is
not on the roster and had not modified it; the one-line edit was flagged to the
integration lead. The LLB is already line-size-parameterised and needed no
change.

## 8. Cases re-run (p1 unless stated)

| Case | Result |
|---|---|
| `cache.refill_evict_fault` (p0) | PASS |
| `cache.mshr_nonblocking` (p0) | PASS |
| `cache.integrated_path` (p1) | PASS |
| `mem.visibility_provenance` (p1) | PASS |
| `store.wrong_path_visibility` (p1) | PASS |
| `lsu.byte_forwarding` (p1) | PASS |
| `lsu.size_fault_boundaries` (p1) | PASS |
| `mmio.exactly_once` (p1) | PASS (after the front-end lane's own regression fix) |
| `sv39.walk_and_faults` (p1) | PASS |
| `tlb.sfence_vma` (p1) | PASS |
| `amo.linearization` (p1) | PASS |
| `lrsc.reservation_progress` (p1) | PASS |
| `fence.code_and_data_order` (p1) | PASS |
| `perf.equal_resource_compare` (p1) | PASS |
| `locality.integrated_path` (p1) | PASS (program updated, section 8) |

`locality.integrated_path` (p1) needed its *program* updated: it was written for
a 256 B L1, where the offsets it chose aliased in an 8-set direct-mapped cache
and forced the evictions the LLB needs. With an 8 KB L1 nothing aliases, so the
LLB was never consulted. The program's address plan was rewritten to force the
same evictions in the new geometry without weakening any check: five lines of
one set (0x440..0x2440, all set 17 at 64 B/32 sets) cycle a four-way set so
every access installs the line the previous one evicted, and the class-check
subject is evicted by four same-set fillers so its bracketed load misses the L1
and is served by the buffer. `llb_hit` is 40 for the `llb` configuration and 53
for `both`, and exactly 0 for `baseline` and `prefetch`; `sig=[0x350 0x69]` =
[848, 105] in all four runs, with the accumulator constant updated to match the
five-line loop. All 21 checks pass, no check weakened. `mmio.exactly_once` now
passes at p0 and p1 (a front-end regression, since fixed by that lane), and
`core.act_dut` (127/127) is re-run by the integration lead's full pass.

### `retire.width_and_order` is not this lane's (bisected)

`retire.width_and_order` (p0) fails in the tree ("event 110 pc expected
0x800001c0, got 0x80000014 lane 1") and passes at clean HEAD. It was attributed
to this lane because `mosaic_core.sv` carries the cache-path instantiation
changes. It is not: **with `mosaic_cache.sv`, `mosaic_l1_cache_path.sv`,
`mosaic_mshr.sv` and `mosaic_prefetch.sv` reverted to HEAD *and* the
cache-path hunks of `mosaic_core.sv` reverted, keeping the rest of the tree, the
case still fails identically** (worktree build, exit 1, same first MISMATCH).
Three further builds all fail identically: `-DMOSAIC_MEM_SMALL_CACHE
-DMOSAIC_CACHE_BLOCKING` (my geometry *and* my non-blocking read path off),
`-DMOSAIC_CACHE_BLOCKING` alone, and a build serving the instruction side from
`mosaic_cache` instead of `mosaic_mshr`. The case runs with `cache_en_i` at its
undriven default, so the cache path is a wire there in any case. The cause is in
the front-end/dispatch work, and the attribution is handed back with this
evidence.

## 9. Not covered (honest list)

* **Multi-outstanding *allocation* on the data path.** The MSHR holds several
  lines, but only one demand is ever in it from the core, because
  `mosaic_load_queue` issues one load at a time (not this lane's file) and the
  LSU endpoint holds one transaction. The `mshr` measurement row is the proof.
  What the plan's I-043/next-step work would add: a load queue that issues past
  its head, an endpoint with an id-tagged outstanding table, and a memory-side
  port that can carry more than one beat outstanding -- none of which this
  deliverable implemented.
* **Coherence (I-065).** There is no cross-cache ordering, snooping or
  interconnect. The one hazard the bigger L1 made live -- a bypassed atomic
  masked by a stale clean copy -- is closed by a targeted line-invalidate; a
  second hart's cache, a DMA, or a shared line between L1I and L1D is still not
  coherent.
* **The endpoint's outstanding-request capability.** Deliberately not raised;
  section 3 gives the measurement and the reason. Fault semantics are unchanged
  (misaligned and PMP refusals still build their response without touching
  memory).
* **The L1I is `mosaic_mshr`, which is a read-only clean cache.** It cannot hold
  a store; an instruction-cache store is answered with a fault by the wrapper,
  as before. It has no write-back and its flush only drops tags.
* **Set-associativity beyond the round-robin policy.** No PLRU, no victim
  buffer, no ECC, no randomised soak.
* **The unit-level cache campaign still runs at the 32 B/8-set/1-way geometry.**
  The new geometry is exercised through the core cases and the perf harness, not
  through the I-042/I-043 unit wrapper (whose geometry is fixed by its own
  registered campaign).
* **`make check` / the full gate** is the integration lead's post-landing run;
  this report's case table is the scoped set the ticket lists.
