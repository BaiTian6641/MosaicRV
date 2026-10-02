# I-060 — a strictly restricted LLB: `llb.stale_copy_invalidation`

Work package I-060 (`docs/implementation-plan.md` §3.1). Registered case
`llb.stale_copy_invalidation`, top `mosaic_llb_tb`, driver `sim/unit/tb_llb.cpp`.
Run with `python3 tools/run_unit.py --profile p1 --case llb.stale_copy_invalidation`
(the LLB is a locality structure over the L1: the profile whose platform map
declares RAM cacheable is p1, and that is the profile this package claims). The
case stays registered `"pending": true`; the integration lead clears it when
recording.

**Verdict: PASS** — from a deleted build directory, with five controls that each
exit 1 with a named first failure and a binary hash different from the shipping
build (table below).

```
RESULT PASS llb.stale_copy_invalidation llb: hit=29 miss=14 bypass=3 fill=37
       fill_refused=3 inv=23 race_refuse=8 checks=0 cycles=104
```

## The structure

`rtl/core/mosaic_llb.sv`: 8 fully associative entries, each holding a 32-byte
line (the same line size as `mosaic_cache`). It is a **clean-copy** buffer in the
only sense that matters: it never owns a dirty byte, never writes back and never
issues a memory request. "Clean" does not mean fresh, so the module is not
exempt from coherence — it is legal only because its invalidation sources are
enumerable, and the enumeration below is the deliverable that makes it legal.

| property | this implementation |
|---|---|
| key | `{physical line = pa[31:5], permission context}` |
| permission context | `{asid[15:0], leaf perms {x,w,r,u}}` — the translation that produced the copy |
| line identity | 32-byte physical line, `pa[31:5]`. **No virtual address is in the key.** |
| what is stored | `valid`, line, vpn, asid, perms, 256-bit data. The vpn is stored and used by a by-page `SFENCE.VMA`; it is *not* part of the lookup key |
| replacement | an invalid entry, then the entry matching the fill's own key, then a round-robin bit |
| observability | a debug read port (`dbg_valid/line/asid/perms/vpn/data`) so a case can check the **contents**, plus counters for hit/miss/bypass/fill/fill-refused/invalidation/refused-race |
| data-side invalidation match | **physical line only** — a store observed under one context removes the copies under every context, because they are copies of the same physical line |
| context-side invalidation match | ASID and/or page for `SFENCE.VMA`; everything for `FENCE`, `FENCE.I` and a context change |

The card permits the LLB only for immutable/read-only regions, or for normal RAM
with a complete invalidation protocol. This implementation takes the second path
for exactly the regions the **generated platform map** declares cacheable, and
refuses the rest. The predicate is not a hand-written list:
`mosaic_cfg_pkg::mosaic_pa_cacheable(pa)`, emitted by `tools/gen_manifest.py`
from `config/memory/<profile>.json`, is asked directly for both a fill and a
lookup. Under p1 that is RAM only; the boot ROM, UART, test harness and CLINT are
non-cacheable and therefore refused/bypassed. Under p0 RAM itself is
non-cacheable, so nothing may be held — the case checks that too (see
"non-cacheable profile" below).

## The invalidation-source enumeration

Every source is a port, a rule in the RTL, and a phase of the case. This is the
list that makes a non-coherent structure defensible.

| # | source | port | what it matches | case phase |
|---|---|---|---|---|
| 1 | this hart's store / AMO commit | `inv_store_valid_i`, `inv_store_pa_i` | the physical line, **ignoring context** | `store-invalidates`, `in-flight-race/store` |
| 2 | L1 refill / replacement | `inv_refill_valid_i`, `inv_refill_pa_i` | the physical line | `refill`, `in-flight-race/refill` |
| 3 | external writer: another hart, DMA, device, debugger | `inv_snoop_valid_i`, `inv_snoop_pa_i` | the physical line | `snoop`, `in-flight-race/snoop` |
| 4 | broadcast / shootdown / global flush | `inv_snoop_all_i` | every entry | `snoop` (broadcast), `in-flight-race/snoop-all` |
| 5 | `FENCE` (memory) | `fence_valid_i`, `fence_kind_i=0` | every entry | `fence`, `in-flight-race/fence` |
| 6 | `FENCE.I` | `fence_kind_i=1` | every entry | `fence`, `in-flight-race/fence-i` |
| 7 | `SFENCE.VMA` | `fence_kind_i=2`, `fence_asid_i`/`fence_has_asid_i`, `fence_vpn_i`/`fence_has_vpn_i` | the named ASID and/or page, and nothing else | `fence`, `in-flight-race/sfence` |
| 8 | `satp` write / ASID switch | `ctx_valid_i` | every entry | `asid-switch`, `in-flight-race/ctx` |

Two deliberate non-sources, stated so they are not mistaken for omissions:

* **A privilege or `mstatus` (SUM/MXR) change is not an invalidation source.**
  The permission decision is made upstream on every access, and the permission
  context is part of the key, so a change of privilege is a change of context
  and cannot reuse a copy taken under another one. This mirrors the TLB's rule
  (`results/reports/I-046-tlb.md`).
* **A `SFENCE.VMA` is not a memory-data fence.** It removes translation-backed
  copies only. Nothing in the design relies on it to flush stale *data*; the
  data sources (1–4) do that. This is the plan's explicit fail mode
  (`docs/implementation-plan.md` §9, "SFENCE 被当 memory-data fence").

## The rules as rules

**Keying.** A lookup hits only when the physical line, the ASID and the leaf
permission class all agree. Over-missing is legal and is the direction chosen
here: a copy taken under a read-only mapping is not handed to a write-permitted
access, and a copy taken under one address space is not reachable from another.
What "physical line identity" means concretely is `pa[31:5]`; what the case can
observe is that two virtual aliases (two VPNs, modelled on the host) that
translate to the same PA **share one entry**, while two permission contexts on
the same PA do **not**. The `VA_KEYED` control is the one build in which the VPN
enters the key, and the case names it: the alias phase's second lookup misses.

**Bypass.** A non-cacheable/device line is refused a fill *and* bypassed on a
lookup; an atomic is bypassed and neither hits nor evicts the entry it bypassed.
The decision is the generated map's, so the LLB and the SoC device decode cannot
disagree. A device line can therefore never become resident, which is what makes
"an MMIO hit" unreachable rather than merely untested.

**In-flight ordering.** The rule is one-directional and is enforced
combinationally with priority over the tag match: **a cycle that invalidates an
entry grants no hit on it.** Either the hit completed in an earlier cycle (the
store was not yet visible then), or it is refused here and the consumer must
refetch; the entry is cleared on the same rising edge. The case drives this for
all eight sources, and additionally checks that the refusal is specific — a
lookup on a different line still hits in the same cycle as a store to another
line. The `RACE_STALE_HIT` control removes the refusal, and the case catches it
returning the pre-store value in the invalidating cycle.

**Context change.** A `satp` write or an ASID switch flushes the whole buffer.
This is deliberately stronger than keying alone, because an ASID number may be
reused for a *different* address space: with only the ASID in the key, an entry
from the old space would still match. The case fills under ASID 7, changes
context, rewrites the line, and requires the entry to be gone and the same-key
lookup to miss; the `ASID_SWITCH_NO_INVALIDATE` control is caught by exactly
that.

**Fence.** `FENCE` and `FENCE.I` remove everything (a structure that is not
itself ordered must not be the way a fence is bypassed). `SFENCE.VMA` removes
exactly what it names: the case checks that a per-ASID fence leaves a *different*
ASID and an unrelated context resident and hitting, and that a by-page fence
removes only the named page of the named ASID. The all-zero form removes
everything.

## The case's hit-before-invalidate evidence

For every source, and in every phase, the case does two things:

1. **asserts the LLB was hit on that line before the invalidation** — a probe
   that must report `req_hit_o = 1` with data equal to the host memory's current
   line, word for word; and
2. **asserts the invalidation removed the entry** — read out of the LLB's
   contents through the debug port (`dbg_valid`, `dbg_line`, `dbg_asid`,
   `dbg_perms`), not inferred from returned data.

The oracle is an independent host model, `mem_`, and nothing else: fills are
taken from `mem_`, every writer changes `mem_` first, and every hit is compared
against `mem_`'s current content. So a hit that returns anything other than what
memory holds now is reported as a stale read by name. An LLB that "returns the
right value because the consumer re-read memory" fails step 1 (the probe would
not hit); an LLB that keeps a stale entry after a writer fails step 2 or the
value comparison.

Counter evidence from the passing p1 run: `hit=29 miss=14 bypass=3 fill=37
fill_refused=3 inv=23 race_refuse=8` — every counter the case claims to exercise
moved, and the conservation phase requires that.

## Non-cacheable profile

Under a profile whose map declares RAM non-cacheable (p0), the freshness campaign
has nothing the LLB is permitted to hold. The case does not pass vacuously: it
runs the geometry phase and the bypass phase, and additionally requires that a
RAM fill is **refused** and a RAM lookup is **bypassed** under that map, with the
buffer left empty.

```
RESULT PASS llb.stale_copy_invalidation (p0)
  llb: hit=0 miss=0 bypass=3 fill=0 fill_refused=3 inv=0 race_refuse=0 cycles=11
  (profile declares RAM non-cacheable: the map-driven bypass is verified; the
   freshness rules need a cacheable region)
```

## Mutants (controls)

`python3 tools/run_llb_controls.py --profile p1`. Each is built from a deleted
build directory with its `-D` on the Verilator command line (and passed to the
C++ half); the binary hash differs from the shipping one; the run exits 1 with a
named first failure.

| control | `-D` | sha256 | exit | first failure |
|---|---|---|---|---|
| shipping | — | `bbdb36839bbe` | 0 | — |
| STORE_NO_INVALIDATE | `MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE` | `3d2d47ebd2ba` | 1 | `store-invalidates`: the store removed the line it wrote — the written line is still resident |
| MMIO_HIT | `MOSAIC_LLB_MUTANT_MMIO_HIT` | `9635bca9772d` | 1 | `mmio-bypass`: a device fill is refused — the device fill was accepted |
| RACE_STALE_HIT | `MOSAIC_LLB_MUTANT_RACE_STALE_HIT` | `077d090b46ca` | 1 | `in-flight-race/store`: no hit in the invalidating cycle — a hit with the pre-store value |
| ASID_SWITCH_NO_INVALIDATE | `MOSAIC_LLB_MUTANT_ASID_SWITCH_NO_INVALIDATE` | `0af3073f581b` | 1 | `asid-switch`: the context change left no translation-backed copy reachable — an entry survived |
| VA_KEYED | `MOSAIC_LLB_MUTANT_VA_KEYED` | `5c10694be058` | 1 | `alias`: a hit through the second virtual alias — a miss |

The first four are the card's required controls verbatim (a store that does not
invalidate; an MMIO/atomic access served from the LLB; an in-flight hit that
returns the stale value after the store is visible; an ASID switch leaving an
entry reachable). The fifth substantiates the card's "an LLB keyed on a virtual
address is wrong, and the case must be able to show it".

## RTL revisions compiled (sha256, first 16 hex)

| file | hash |
|---|---|
| `rtl/core/mosaic_llb.sv` | `571184589ae1d122` |
| `sim/tb/mosaic_llb_tb.sv` | `0e81e741c4dbeb5f` |
| `sim/unit/tb_llb.cpp` | `58257d17637999a8` |
| `tools/run_llb_controls.py` | `64f441c2bd2c3067` |
| `build/p1/rtl/mosaic_cfg_pkg.svh` (generated) | `73eebb96add3d35d` |

Nothing else was changed. The LLB is **module-level**: it is not instantiated in
`mosaic_core.sv`, and no shared core file (`mosaic_core.sv`,
`mosaic_dispatch.sv`, `mosaic_csr.sv`, `mosaic_pkg.sv`,
`sim/tb/mosaic_core_tb.sv`) was touched — the plan's path is module-level first,
and the vector lane owns those files this week.

## Cases re-run after the change

* p1: `llb.stale_copy_invalidation` PASS; `cache.integrated_path`,
  `sv39.walk_and_faults`, `tlb.sfence_vma` PASS.
* p0: `llb.stale_copy_invalidation` PASS (bypass path);
  `cache.refill_evict_fault`, `cache.mshr_nonblocking`, `mmio.exactly_once`,
  `mem.visibility_provenance`, `core.mem_program` PASS.
* `core.act_dut` (ACT4, p1): **`ran=127 passed=127 expected=127`**, exit 0.
  The LLB is not in that case's source list, so this is a regression check of the
  shared tree rather than of this module.

## Gates

`make check` rc 0 (config, contracts, event contract + negative, records,
exclusions + negative, coverage, capability matrix, upstream, isolation);
`python3 tools/lint_rtl.py --profile p0` and `--profile p1`: 59 source files
clean, `--self-test` still rejects a latching module; `make lint-slang
PROFILE=p1` rc 0 (the only warnings `mosaic_llb.sv` introduces are `STYLE-2`
port-suffix warnings, of which the tree already carries 2057 — the module's
`always_comb` blocks are all named, so it adds no `STYLE-13`); `check_records`
rc 0 (72 delivered packages, 84 registered cases); `check_exclusions` and
`--negative` rc 0.

`make lint-cpp PROFILE=p1` is clean (39 files, `sim/unit/tb_llb.cpp` among them).
`make lint-cpp PROFILE=p0` currently fails on `sim/unit/tb_vec.cpp` and
`sim/unit/tb_core_vec.cpp` because the `rvv.*` and one `core.*` `obj_dir`s under
`build/p0/unit/` hold Verilator-generated headers older than the vector lane's
current `sim/tb/mosaic_vec_tb.sv`; the compiler picks the stale `-I` first. That
is an artifact of an incomplete `make unit` run in a tree the vector lane is
editing, not of this package: `tb_llb.cpp` compiles clean under the identical
`-Wall -Wextra -Wshadow` flags for both profiles (verified directly), and
`make test` runs `unit` before `lint-cpp`, which regenerates every header.

## Not covered (honest list)

* **The LLB is not integrated into the core.** It is a module with its own DUT,
  driven by `mosaic_llb_tb`; the case proves the *protocol*, not a load that
  actually flows through it. What integration must add: a real fill source on the
  L1 refill path, a real store-side pulse from the store queue's commit, a real
  `SFENCE.VMA`/`satp` connection, and the LQ-violation/replay rule the
  architecture review asks for when an already-consumed stale value must not
  retire. None of that exists here.
* **Single hart.** There is no cross-hart shootdown. Source 3 (`inv_snoop`) and
  source 4 (`inv_snoop_all`) are the *interfaces* a coherence protocol would
  drive, exercised by the case as pulses; a multi-hart implementation would have
  to prove that a remote store reaches every hart's LLB, that the shootdown
  acknowledges before the writer's store is visible, and that the race between a
  remote invalidation and a local in-flight hit is decided at one serialization
  point. None of that is testable with one hart, and the case does not claim it.
* **DMA, device masters and the debugger are not in the harness.** `inv_snoop` is
  a stand-in for all of them; the case shows the LLB reacts correctly when told,
  not that the platform tells it.
* **The refill source is a pulse, not the L1.** The case shows a refill
  invalidates; it does not show the L1 raising the pulse at the right moment.
* **The permission-context key is conservative**, not minimal: an entry filled
  under `{r,w}` is not reused for `{r}` or `{r,w,x}` even though the data is
  identical. That over-misses by design and is stated rather than optimised.
* **No formal proof of the ordering rule.** "A cycle that invalidates grants no
  hit" is enforced combinationally and is exercised for all eight sources plus
  the unrelated-line case, and its removal is caught by a control — but it is
  directed evidence, not a machine-checked invariant.
* **The version/generation alternative is not implemented.** The architecture
  review's "validate to retire" scheme (a version bit that must be protected
  against re-invalidation and wrap/ABA) is not built; this package takes the
  conservative flush/refuse path instead.
* **The case needs a cacheable region**, which under this platform is p1's RAM.
  The registry entry currently carries no `profiles` field, so
  `make unit --all PROFILE=p0` would run the bypass-only path; adding
  `"profiles": ["p1"]` to the entry would make the intent explicit. The
  integration lead owns `tests/unit/registry.json`.
* **A store's context is not modelled.** `inv_store` carries only a physical
  line, which is the stronger and simpler contract (the LSQ has already resolved
  the physical line before it can commit). The case exercises the consequence —
  a store removes every context's copy — but not a store that arrives with a
  context to be matched against.
