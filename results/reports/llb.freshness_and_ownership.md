# V-061 — LLB freshness, invalidation and ownership: `llb.freshness_and_ownership`

Work package V-061 (`docs/validation-plan.md` §V-061, `docs/stage-4-vector-locality.md`
§V-061). Registered case `llb.freshness_and_ownership`, top `mosaic_llb_tb`,
driver `sim/unit/tb_llb.cpp` (the V-061 campaign added beside the I-060 one; the
two cases share the DUT, the harness and the oracle).

* **Case id:** `llb.freshness_and_ownership`
* **Profile:** **p1** — the LLB may only hold what the generated platform map
  declares cacheable, and p1 (like p2/p3) is the profile whose map declares RAM
  cacheable. Under p0 RAM is non-cacheable, so the freshness campaign has no
  subject and the case reduces to the map-driven bypass (branch kept, and
  smoke-run). Register `"profiles": ["p1"]`.
* **Run:** `python3 tools/run_unit.py --profile p1 --case llb.freshness_and_ownership`
* **Controls:** `python3 tools/run_llb_freshness_controls.py --profile p1`
  (builds shipping + five `-D` controls, each from a deleted build directory).

```
RESULT PASS llb.freshness_and_ownership
  llb-fresh: hit=31 miss=17 bypass=6 fill=31 fill_refused=12 inv=14
             race_refuse=1 checks=0 cycles=107
```

The campaign reuses the I-060 `Harness`, the independent host-memory oracle
`mem_` and every primitive (`Fill`, `ProbeLookup`, `PulseStore`, `PulseRefill`,
`PulseSnoop`, `PulseSnoopAll`, `PulseFence`, `PulseCtx`, `ConflictProbe`,
`Dbg`, `FindEntry`) rather than standing up a second model. One primitive was
added for this card: `Harness::FillRacingInvalidate`, which presents a
speculative fill in the *same cycle* as one of the eight invalidations. The SV
wrapper and the RTL are **unchanged**.

## The observation model (what "allowed memory observation" means here)

The oracle is `mem_` alone. Every fill takes its data from `mem_`; every writer
(this hart's store, another hart, the DMA engine, an L1 refill) changes `mem_`
*before* the invalidation pulse. Under this single-hart sequential model the
only observation a load to `PA` is allowed to see is the current content of
`mem_` at that PA's line. So:

* every hit is compared **word for word** against `mem_` at the instant of the
  hit (`ExpectHitCurrent`, used in every phase); and
* at rest, **every resident entry** is read out of the LLB's contents through the
  debug port and compared against `mem_` again (`provenance-invariant`). An LLB
  that treats a clean copy as needing no coherence fails here as a stale
  resident line, not merely as a stale return.

## The card's Action items, each mapped to a check

| # | card Action item | phase / check | what it drives |
|---|---|---|---|
| 1 | LLB hit/miss/bypass | `hit-miss-bypass` | miss on an unfilled line; hit equals memory; atomic bypassed without evicting; device lookup bypassed |
| 2 | another hart's store | `remote-hart-store` | hart 1 writes, snoop arrives; the copy is removed, a lookup misses, a refill brings hart 1's value |
| 3 | DMA | `dma-write` | DMA engine writes (snoop) removes the copy; a DMA flush/shootdown (`snoop_all`) removes everything |
| 4 | same-address aliasing | `same-address-aliasing` | same PA/two VAs share one copy; same VA/different PA misses; sub-line offset hits its line and does not match another; different ASID misses |
| 5 | PMA non-cacheable regions | `pma-non-cacheable` | every non-idempotent region is non-cacheable (map agreement), and each of uart/test-harness/clint/boot-rom is refused a fill and bypassed; a cacheable RAM line is accepted (non-vacuous) |
| 6 | lane reassignment | **not covered** — see below | the delivered `mosaic_llb` has no owner/lane field or port; the nearest covered mechanism is the translation-context reconfiguration in `ownership-context-change` |
| 7 | misprediction racing an invalidation | `misprediction-race` | a speculative fill and a speculative hit each presented in the invalidating cycle for all eight sources; the fill is refused and the hit is refused-or-already-fresh |
| — | clean copy needs coherence (supporting) | `local-writer-freshness` | this hart's own store removes the copy; an unrelated copy survives |
| — | ownership (supporting) | `ownership-context-change` | a `satp`/ASID change makes every translation-backed copy unreachable, even under a reused ASID |
| — | "every hit provable" (supporting) | `provenance-invariant` | at rest every resident entry equals memory's line |
| — | coverage (supporting) | `conservation` | `o_count` equals the debug port's live entries; every counter the case claims moved |

## Enumeration: address-domain and ownership relations (observed)

`PA` values are the case's RAM lines; `VPN`/`ASID` are the request's translation
context; "resident" is the state before the lookup. Every "hit" row also
requires the returned line to equal `mem_`.

| class | request | resident | observed | check |
|---|---|---|---|---|
| same PA, different VPN (physical alias) | `(PA=L1, VPN=B, ASID=1)` | `(L1, VPN=A, ASID=1)` | **hit**, one physical copy | `same-address-aliasing` |
| same VA, different PA | `(PA=L2, VPN=A, ASID=1)` | `(L1, VPN=A, ASID=1)` | **miss** (VA is not the identity) | `same-address-aliasing` |
| sub-line offset of a resident line | `(PA=L1+0x18, VPN=A, ASID=1)` | `(L1, VPN=A, ASID=1)` | **hit** (offset not in the identity) | `same-address-aliasing` |
| same offset, different line | `(PA=L2+0x18, VPN=A, ASID=1)` | `(L1, VPN=A, ASID=1)` | **miss** (offset alone does not match) | `same-address-aliasing` |
| same PA, different ASID | `(PA=L1, VPN=A, ASID=2)` | `(L1, VPN=A, ASID=1)` | **miss** (owner differs) | `same-address-aliasing` |
| cacheable RAM | fill `(RAM, ASID=1)` | — | **accepted**, then **hit** | `pma-non-cacheable` |
| uart / test-harness / clint / boot-rom | fill + lookup | — | fill **refused**, lookup **bypassed**, never resident | `pma-non-cacheable` |
| atomic (AMO/LR/SC) | lookup on a resident line | resident | **bypass**, resident copy untouched | `hit-miss-bypass` |
| non-idempotent device | lookup | — | **bypass** | `hit-miss-bypass`, `pma-non-cacheable` |

Permission-context ownership (a copy under `{r,w}` is not reused under `{r}` or
`{r,w,x}`) is the I-060 case's `alias` phase and is not re-claimed here; the two
cases share the DUT, so it is covered in the same package.

## Invalidation completeness and the two races

* **Same-cycle removal, all eight sources.** `misprediction-race` drives a
  speculative fill against store, refill, snoop, snoop-all, FENCE, FENCE.I,
  SFENCE.VMA (by ASID) and context change; each must be refused, and the entry
  must not be resident afterwards. `ConflictProbe` drives the speculative *hit*
  variant: a hit granted in the invalidating cycle is only legal if it already
  carries the post-write value.
* **The predictor changes latency, not values.** After a refused speculative
  fill the demand access misses (latency) and the value it finally sees is
  `mem_`'s; a hit is accepted only if it equals `mem_`. This is the card's
  "predictor error must not produce a wrong value" fail mode, made observable.

## Controls (negative), each built from a deleted build directory

`python3 tools/run_llb_freshness_controls.py --profile p1`. Each control carries
its `-D` on the Verilator command line *and* in `-CFLAGS`, so the binary differs
from shipping; shipping is printed first because a control is only evidence if
shipping passes. All six binaries have distinct hashes.

| control | `-D` | sha256 | exit | first failure (named) |
|---|---|---|---|---|
| shipping | — | `fb6b1dd90314ddab` | 0 | — |
| STORE_NO_INVALIDATE | `MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE` | `1bbb8fa57ee907f0` | 1 | `local-writer-freshness`: the store removed the clean copy — the clean copy survived the store |
| MMIO_HIT | `MOSAIC_LLB_MUTANT_MMIO_HIT` | `14cb02cd088b60ba` | 1 | `hit-miss-bypass`: a device lookup is bypassed — a device lookup was served |
| RACE_STALE_HIT | `MOSAIC_LLB_MUTANT_RACE_STALE_HIT` | `7b55ede690dd7ec4` | 1 | `misprediction-race/hit-store`: no hit in the invalidating cycle — a hit with the pre-store value |
| ASID_SWITCH_NO_INVALIDATE | `MOSAIC_LLB_MUTANT_ASID_SWITCH_NO_INVALIDATE` | `babc65022ef8d729` | 1 | `ownership-context-change`: the context change left no copy reachable — a copy survived |
| VA_KEYED | `MOSAIC_LLB_MUTANT_VA_KEYED` | `8eec1f8401f87e90` | 1 | `same-address-aliasing`: a hit on `0x80002100` — a miss |

Each is one of the card's fail modes:

* **STORE_NO_INVALIDATE** — "a clean copy treated as needing no coherence" (the
  card's central failure). First caught by `local-writer-freshness`.
* **MMIO_HIT** — a device line filled and hit; the PMA/non-idempotent rule.
  First caught by `hit-miss-bypass` (the device-bypass check), before
  `pma-non-cacheable`.
* **RACE_STALE_HIT** — "a predictor error producing a wrong value rather than
  only a latency change": the speculative hit returns the pre-store value.
* **ASID_SWITCH_NO_INVALIDATE** — "stale-owner data leaking after
  reconfiguration" (translation-context form).
* **VA_KEYED** — the identity is the physical line, not the VA; a virtual-keyed
  LLB misses the second alias.

No control died in the compiler and none is inert: each names the check that
caught it and each is reached by a phase the case is *about*.

## Not covered (honest list)

* **Lane reassignment / owner-domain transfer.** The card's Action item 6 and
  the fail mode "stale-owner data leaking after reconfiguration" name a lane
  reassignment. The delivered `rtl/core/mosaic_llb.sv` has **no owner, lane or
  domain field and no port** for one (its key is `{physical line, ASID, leaf
  perms}`); the lane broker (I-059) does not connect to it and the LLB is not
  instantiated in `mosaic_core.sv`. There is therefore no mechanism to drive a
  lane reassignment or to observe a leaked owner's data, and no check is
  invented for it. The *translation-context* reconfiguration that does exist
  (`ctx_valid_i`, a `satp`/ASID change) is covered by `ownership-context-change`;
  a real lane-reassignment rule would be a new RTL obligation.
* **Version / generation checking.** The card's Pass criterion names "version
  checking complete". The design has no version/generation field: it takes the
  conservative flush-and-refuse path (I-060 report, "the version/generation
  alternative is not implemented"). What is checked is the consequence —
  invalidations are complete and same-cycle — not a version compare. No version
  check exists to test.
* **Invalidation acknowledgement.** There is no ack output; an invalidation
  removes the entry on the invalidating rising edge. The case checks that
  removal (the closest observable to an ack) but cannot check a handshake that
  does not exist.
* **Cross-hart shootdown protocol.** Single hart. `inv_snoop`/`inv_snoop_all`
  are the *interfaces* a coherence protocol would drive; the case shows the LLB
  reacts correctly when told, not that a remote store reaches every hart or that
  the shootdown acknowledges before the writer's store is visible.
* **DMA / device masters / debugger topologies.** Not in the harness; the snoop
  port stands in for all of them.
* **The prefetcher itself.** The card's "misprediction" is modelled as a
  speculative fill/hit presented to the LLB. The delivered `mosaic_prefetch`
  (I-063) and the `o_evict_valid_o` observation port are not driven here; a
  wrong *prediction* in the prefetcher is I-063's subject. What this case proves
  is the LLB's half: a speculative copy can never change a value.
* **A non-idempotent region that is cacheable.** The LLB gates a fill on
  `mosaic_pa_cacheable` only. The case checks the map agreement that makes this
  sufficient in this platform (every non-idempotent region is non-cacheable). A
  future region that is non-idempotent *and* cacheable would not be excluded by
  the LLB; that is a boundary to record, not a defect in the map as configured.
* **Formal proof.** Every property here is directed evidence, not a
  machine-checked invariant; the same-cycle refusal in particular has no
  formal proof.
* **The LLB is module-level, not integrated in the core** (carried over from
  I-060): the case proves the protocol, not a load that flows through it.

## RTL revisions compiled (sha256, first 16 hex)

| file | hash |
|---|---|
| `rtl/core/mosaic_llb.sv` (unchanged, at `HEAD`) | `8a4bb1c0a9bc5f4c` |
| `sim/tb/mosaic_llb_tb.sv` (unchanged) | `5ac58687e564b4e2` |
| `sim/unit/tb_llb.cpp` | `b2914035764c71f1` |
| `tools/run_llb_freshness_controls.py` | `8cea1e7bf0532e43` |
| `build/p1/rtl/mosaic_cfg_pkg.svh` (generated) | `4349e23c46486801` |

No RTL and no SV was changed: the case reuses `mosaic_llb_tb` and the I-060
sources, and adds only the V-061 campaign to `tb_llb.cpp` plus the control
runner. `rtl/core/mosaic_dispatch.sv`, `mosaic_core.sv`'s `VEC_ALU_CAPS` line and
`tools/run_frontend_controls.py` were not touched.

## Cases re-run after the change

* `llb.stale_copy_invalidation` (p1) PASS, from a deleted build directory —
  the I-060 case is unaffected by the added campaign.
* `llb.freshness_and_ownership` (p0) PASS, defensive non-cacheable path
  (`hit=0 miss=0 bypass=1 fill=0 fill_refused=1`), confirming the driver still
  builds and runs under a profile the case is not registered for.

## Gates

* `lint_rtl` is unaffected (no RTL changed).
* `sim/unit/tb_llb.cpp` compiles clean under the `make lint-cpp` flags
  (`-std=c++17 -fsyntax-only -Wall -Wextra -Wshadow`); the only warnings come
  from the Verilator runtime headers, none from the driver.
* `check_records`, `check_exclusions` do not scan `results/reports/` or
  `tools/`, so neither is disturbed by the new report or runner; the registry
  entry is the integration lead's to add.
