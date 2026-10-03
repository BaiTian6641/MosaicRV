# V-062 — coalescer split/merge and address-domain boundaries: `cache.coalescer_boundaries`

Work package V-062 (`docs/validation-plan.md` §V-062, source SRC-03 §Memory
Packetizer / §Cross-Hart Coalescing). Registered case **`cache.coalescer_boundaries`**,
top `mosaic_cache_tb`, driver `sim/unit/tb_cache.cpp` (the V-062 campaign added
beside the I-042/I-043 campaigns; the three cases share the DUT, the harnesses
and the oracle).

* **Case id:** `cache.coalescer_boundaries`
* **Profile:** **p1** — the case drives the L1 data cache and, for the
  MMIO/AMO clause, the I-042 access path. Under **p0 RAM is non-cacheable**
  (`MOSAIC_RAM_CACHEABLE = 0`), so every access would bypass and the
  "a cacheable load enters the cache" control could not hold. p1 is the first
  profile whose map declares RAM cacheable (`MOSAIC_RAM_CACHEABLE = 1`).
  Register `"profiles": ["p1"]`.
* **Run:** `python3 tools/run_unit.py --profile p1 --case cache.coalescer_boundaries`
* **Controls:** `python3 tools/run_coalescer_boundary_controls.py --profile p1`
  (builds shipping + four `-D` controls, each from a deleted build directory).

```
RESULT PASS cache.coalescer_boundaries
  coalescer: miss=18 coalesce=9 refill=8 fault=1 wb=1 reads=9 writes=1 cycles=108
             | cancel-keeps-others
85 checks, 0 failures
```

## The DUT and the observation model

The coalescer under V-062 is `mosaic_cache`'s **per-line waiter FIFO**: a request
that misses a line already being fetched joins one MSHR entry, and each waiter
keeps its own word index and, for a store, its own byte enables (`wmask`). That
is the split/merge under test. Its CPU request port carries exactly
`{we, pa_line, word, wmask, wdata}` — no permission, translation or ordering tag.
The oracle is the flat byte array the I-042/I-043 campaigns already use, so a
merged load is compared **word for word against memory**, never against the
DUT's own line. The per-consumer cancel port and the tag-comparing response are
properties of the id-tagged `mosaic_mshr`, so the cancel clause is driven there
after the L1D phases.

The campaign reuses `Harness` (I-042's cache-on/cache-off oracle and
`Offer`/`Withdraw`/`TickAccepted` primitives) and `MshrHarness` rather than
standing up a second model. The driver gained one primitive set for the I-042
access path (`LpSet*`/`Lp*`), driven a cycle at a time.

## The card's Action items, each mapped to a check

| # | card Action item | phase / check | what it drives |
|---|---|---|---|
| 1 | same line, different bytes | `same-line-different-bytes` | three words of one line join one entry; one memory read; three answers, each its own word; coalesce pulse +2 |
| 2 | duplicate bytes | `duplicate-bytes` | an exact duplicate plus another word of the line: one entry, one read, three answers — the duplicate is answered once in its own right |
| 3 | cross line / page | `cross-line-same-set`, `cross-page` | two lines that share a set and an offset (and two lines in different 4 KiB pages) are two entries, two reads, zero coalesces, each its own word |
| 4 | differing permissions | **not covered** — see below | no permission tag on the coalescer's request port |
| 5 | same VA, different PA | **not covered** — see below | the coalescer's key is a physical line; there is no VA field (covered at the LLB by V-061's `same-address-aliasing`) |
| 6 | same PA, different VA | **not covered** — see below | same reason (V-061 `same-address-aliasing`) |
| — | merge preserves per-request return | `same-line-different-bytes`, `duplicate-bytes` | every waiter's returned word equals memory at its own address, not a sibling's slot |
| — | merge preserves per-request fault | `coalesced-fault-per-request` | a faulted refill answers **every** coalesced consumer with its own fault, one read, installs nothing, and the retry returns each word |
| — | stores merge only by the defined rules | `store-byte-merge` | two partial stores to one word (masks `0x03`, `0x0c`) plus a whole store to the next word merge on install; only selected lanes change, flushed and compared byte for byte |
| — | MMIO / AMO requests are never merged | `bypass-not-merged/amo-cacheable`, `bypass-not-merged/mmio-device` | an atomic to a cacheable PA and a non-cacheable MMIO access reach the memory service exactly (addr/size/wstrb/wdata/amo) and never enter the cache (0 line transactions); a cacheable non-atomic load is the non-vacuous control (it *does* enter the cache) |
| — | cancelling one consumer does not cancel others | `cancel-keeps-others` (on `mosaic_mshr`) | two ids coalesce on one line; id 1 is cancelled; id 0 is answered once with its own word and the entry frees; the cancelled id gets nothing |
| — | coverage | `coalescer-final` | no live entry at rest; every L1D memory read is a refill or a fault; every write a writeback; refill/fault/writeback/coalesce all reached |

## Enumeration: merge/split relations (observed)

| class | request(s) | observed | check |
|---|---|---|---|
| same line, different bytes/words | words 0, 1, 2 of line `0x1540`, offered while the refill is held | **merged**: 1 entry, 1 memory read, 3 answers, each its own word | `same-line-different-bytes` |
| duplicate bytes/word | word 2, word 2 (exact duplicate), word 0 of line `0x1680` | **merged**: 1 entry, 1 read, 3 answers (the duplicate answered once) | `duplicate-bytes` |
| cross line, same set + offset | `0x3000` vs `0x3100` (tag differs, set/offset equal, low 8 bits equal) | **not merged**: 2 entries, 2 reads, 0 coalesces, each line's own word | `cross-line-same-set` |
| cross page, same set + offset | `0x4000` vs `0x5000` (different 4 KiB pages, low bits equal) | **not merged**: 2 entries, 2 reads | `cross-page` |
| store byte merge | partial store bytes 0–1, partial store bytes 2–3, whole store word 1 of line `0x17a0` | **merged by rule**: only selected lanes change; both partial stores survive; untouched bytes and words unchanged | `store-byte-merge` |
| coalesced fault | words 0, 1, 2 of a poisoned line `0x18c0` | **one fault per consumer**: 1 read, 3 fault answers, line not installed; retry returns words 0 and 1 | `coalesced-fault-per-request` |
| cancel one of two consumers | ids 0, 1 to two words of line `0x6000`, then cancel id 1 | **survivor served**: 1 read, id 0 answered with its own word, id 1 nothing, entry frees | `cancel-keeps-others` |
| atomic to a cacheable PA | AMO to `0x80001000` | **bypassed**: 1 bypass txn, 0 line txns, memory sees the exact request | `bypass-not-merged/amo-cacheable` |
| non-cacheable MMIO | word to `0x00100000` (UART) | **bypassed**: 1 bypass txn, 0 line txns | `bypass-not-merged/mmio-device` |
| cacheable non-atomic (control) | load from `0x80001000` | **entered the cache**: line txn > 0, 0 bypasses | `bypass-not-merged` |

The card's "matching on VA or low address bits alone" fail mode is made
observable twice: the merge key is the **whole physical line address**, so two
lines that share every low bit are two entries, and the `COALESCE_LOW_BITS`
control shows that a low-bit key returns the wrong line's data.

## Controls (negative), each from a deleted build directory

`python3 tools/run_coalescer_boundary_controls.py --profile p1`. Each control
carries its `-D` on the Verilator command line *and* in `-CFLAGS`, so the binary
differs from shipping; shipping is printed first because a control is only
evidence if shipping passes. All five binaries have distinct hashes.

| control | `-D` | sha256 (16) | exit | first failure (named) |
|---|---|---|---|---|
| shipping | — | `9623e30f8a072eb1` | 0 | — |
| COALESCE_LOW_BITS | `MOSAIC_CACHE_MUTANT_COALESCE_LOW_BITS` | `7afbe386c2be7d03` | 1 | `cross-line-same-set: two different lines are two entries` |
| STORE_MASK_DROP | `MOSAIC_CACHE_MUTANT_STORE_MASK_DROP` | `eca1f9a654f77c26` | 1 | `store-byte-merge: only the selected byte lanes changed (wmask honoured)` |
| WRONG_WAITER | `MOSAIC_CACHE_MUTANT_WRONG_WAITER` | `e8904a65b26e6c77` | 1 | `same-line-different-bytes: word 1 returned to its own request` |
| FAULT_VALID | `MOSAIC_CACHE_MUTANT_FAULT_VALID` | `ed1986620a1e5057` | 1 | `coalesced-fault-per-request: every consumer is faulted, not just the first` |

Each is one of the card's fail modes:

* **COALESCE_LOW_BITS** (new, `rtl/core/mosaic_cache.sv`) — the merge key is the
  low address bits instead of the whole line, so two lines that share a set and
  an offset coalesce and the second consumer receives the first line's word:
  *"matching on VA or low address bits alone"* and *"fewer requests treated as
  correctness"*. First caught by `cross-line-same-set`; the value check would
  also catch it.
* **STORE_MASK_DROP** (new, `rtl/core/mosaic_cache.sv`) — a coalesced store
  writes every byte lane whatever `wmask` says, so a partial store clobbers the
  bytes it did not select: *"losing byte enables"*. First caught by
  `store-byte-merge`.
* **WRONG_WAITER** (`rtl/core/mosaic_cache.sv`, added for I-043) — every waiter
  on a coalesced line is answered from the FIFO's first slot, so a request that
  joined an in-flight miss gets another requester's word: *"a coalesced response
  delivered to the wrong requester"*. First caught by
  `same-line-different-bytes`.
* **FAULT_VALID** (`rtl/core/mosaic_cache.sv`, added for I-042) — a faulted
  refill is installed as if it had succeeded, so the consumers that should each
  receive the line's fault instead receive data: the fault-sharing fail mode.
  First caught by `coalesced-fault-per-request`.

No control died in the compiler and none is inert: each names the check that
caught it, and each is reached by a phase the case is *about*.

## Not covered (honest list)

* **Differing permissions (Action item 4).** The coalescer's CPU request port
  (`mosaic_cache`) carries `{we, addr, wdata, wmask}` only; there is no
  permission/PMT field, and the access path classifies by cacheability and
  `amo`, not by a permission class. Permission faults are raised upstream (LSU /
  `mosaic_pmp.sv`) before a request can reach the coalescer, so two requests
  *with different permissions* cannot be presented to it or distinguished by it.
  No check is invented; a permission-tagged coalescer key would be new RTL.
* **Same VA / different PA, and same PA / different VA (Action items 5, 6).**
  The coalescer's key and the access path's request are physical addresses; the
  DUT has no VA field, so these translation-identity relations are not
  derivable here. They are covered where the translation context exists: V-061's
  `llb.freshness_and_ownership` `same-address-aliasing` phase (same PA/two VAs
  hit one copy; same VA/two PAs miss) and `pma-non-cacheable`. What V-062 proves
  is the complement: the merge key is the full **physical** line, so nothing is
  merged on a virtual or low-bit partial match.
* **Incompatible ordering.** The only "incompatible class" on the coalescer/
  access-path request is `amo`; the access path's `aq`/`rl` are tied off in the
  harness and are not part of any merge key, and the coalescer has no ordering
  tag at all. "Incompatible-order requests are not merged" is therefore checked
  only to the extent that the AMO class bypasses; a real ordering-class merge
  rule does not exist to test.
* **Write-to-write ordering on the same byte, and store merge across an
  eviction.** `store-byte-merge` covers disjoint partial stores merged on
  install. A same-byte pair's last-writer order and a store merging into a line
  that is being written back are not exercised; the I-042 campaign covers
  store-hit merge and dirty-eviction.
* **Cross-hart / cohort coalescing.** The coalescer is per hart; the cross-hart
  version is V-070 (`docs/validation-plan.md` §V-070), which depends on V-062.
  Nothing here says two harts' requests merge safely.
* **The access path's invalidate for a bypassed atomic.** `mosaic_l1_cache_path`
  has an `inv_valid`/`inv_pa` coherence act for a bypassed atomic to a
  *cached* line; the bypass phase presents the atomic to an empty cache, so that
  path is not driven. It is a coherence act (V-061/V-067), not a merge rule.
* **Integration.** The case is unit-level: it proves the module's rules and the
  access path's classification, not a load that flows through the core.
  `cache.integrated_path` (I-042) is the core-level integration case.
* **Formal proof.** Every property here is directed evidence, not a machine-
  checked invariant.

## Registry entry the integration lead should add

```json
"cache.coalescer_boundaries": {
  "task": "V-062",
  "profiles": ["p1"],
  "top": "mosaic_cache_tb",
  "rtl": [
    "rtl/core/mosaic_pkg.sv",
    "rtl/core/mosaic_uop_pkg.sv",
    "rtl/core/mosaic_cache.sv",
    "rtl/core/mosaic_mshr.sv",
    "rtl/core/mosaic_cache_line_bridge.sv",
    "rtl/core/mosaic_llb.sv",
    "rtl/core/mosaic_prefetch.sv",
    "rtl/core/mosaic_locality_path.sv",
    "rtl/core/mosaic_l1_cache_path.sv"
  ],
  "sv": ["sim/tb/mosaic_cache_tb.sv"],
  "cpp": ["sim/unit/tb_cache.cpp"],
  "max_cycles": 200000,
  "seed": 1
}
```

`sim/tb/mosaic_cache_tb.sv` includes the four `mosaic_l1_cache_path` dependency
files under their own include guards, so the existing `cache.mshr_nonblocking`
and `cache.refill_evict_fault` entries (which list only the three original
sources) still build unchanged — verified below. The one side effect is that
their binaries now elaborate the inert (en-low) access path, so the *binary
hashes* cited in the I-042/I-043 reports are historical; their checks and traces
are unchanged.

## RTL revisions compiled (sha256, first 16 hex)

| file | hash | note |
|---|---|---|
| `rtl/core/mosaic_cache.sv` | `b6e449a0bf10a937` | two V-062 `-D` mutants added |
| `rtl/core/mosaic_mshr.sv` | `7e273bae79e6c353` | unchanged |
| `sim/tb/mosaic_cache_tb.sv` | `b3241d5637f51a78` | access-path instance + guarded includes |
| `sim/unit/tb_cache.cpp` | `2a7b0d54366fa37e` | V-062 campaign |
| `tools/run_coalescer_boundary_controls.py` | `9604a21ced885c6c` | control runner |

`rtl/core/mosaic_dispatch.sv`, `rtl/core/mosaic_core.sv`'s `VEC_ALU_CAPS` line
and `tools/run_frontend_controls.py` were not touched.

## Gates

* `python3 tools/lint_rtl.py --profile p0` → **65 source file(s) clean**;
  `--profile p1` → **65 clean** (run after the `mosaic_cache.sv` mutants).
* `python3 tools/check_records.py` → `ok records agree: 81 delivered package(s),
  97 registered case(s)`.
* `python3 tools/check_exclusions.py` → `ok exclusions: 54 registered …`;
  `ok closure …`.
* Gate cases re-run from deleted build directories (output to a scratch dir, so
  the evidence ledger is not churned): `cache.mshr_nonblocking` (p0) **PASS**,
  `cache.refill_evict_fault` (p0) **PASS** — both with their original registry
  entries; `lsu.byte_forwarding` (p0) **PASS**, `store.wrong_path_visibility`
  (p0) **PASS**.
* `sim/unit/tb_cache.cpp` compiles clean under `-std=c++17 -fsyntax-only -Wall
  -Wextra -Wshadow` (exit 0; the only warnings are from the Verilator runtime
  headers).
* `amo.linearization` and `cache.integrated_path` were **not** re-run: both build
  `rtl/core/mosaic_core.sv`, which another lane currently has modified
  (`git status` shows ` M rtl/core/mosaic_core.sv`). Neither depends on any file
  this package changed, and the `mosaic_cache.sv` change is confined to
  unset `ifdef` branches, so the shipping elaboration of both is unchanged.

## RTL change in this package

Two `-DMOSAIC_CACHE_MUTANT_*` variants were added to `rtl/core/mosaic_cache.sv`:
`COALESCE_LOW_BITS` changes the duplicate-miss comparison to the low
`set+offset` bits, and `STORE_MASK_DROP` drops the `wmask` test when a coalesced
store merges into the installed line. The shipping build defines neither, so no
existing behaviour changes; `lint_rtl` is clean on both profiles and the two
cases that build this file still pass.
