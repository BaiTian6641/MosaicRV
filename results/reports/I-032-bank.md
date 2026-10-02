# I-032 — an optional bank preference in physical allocation — **PASS**

Work package I-032 (`docs/implementation-plan.md` §6, "实现 bank-aware physical
allocation"). Case `CASE=rename.bank_bias_exhaustion` (top `mosaic_rename_tb`,
driver `sim/unit/tb_rename.cpp`), plus the whole blast radius of
`rtl/core/mosaic_rename.sv` re-run.

All commands are from `/Users/flare/MosaicRV`, Verilator 5.052 (macOS arm64).

> **The card.** *Inputs:* the ordinary free-list baseline, bank pressure and
> producer locality. *Action:* add a switchable bank preference that keeps legal
> tag allocation, falling back to any legal free bank on conflict, and run
> `CASE=rename.bank_bias_exhaustion`. *Pass:* no bias leaks a free register, and
> the change in stall / writeback / read conflicts under the same resources is
> **measured, not assumed**. *Fail:* the designated bank being empty deadlocks
> allocation, or an unreleased physical tag is used.

The implementation is a *preference* with a proved fallback: the biased search
set is the free set intersected with the preferred bank, and it is used only when
that intersection is non-empty. Acceptance is still the free *count* against the
group's requirement, so the preference can change only *which* legal free tag is
chosen — never whether an allocation succeeds, and never the architectural
result. The case measures the delta (below) and drives every mutant that the
card's Fail modes name.

---

## 1. Files

| File | State |
|---|---|
| `rtl/core/mosaic_rename.sv` | **changed**: two new inputs (`alloc_bias_en`, `alloc_bias_bank`); the allocation scan's search set is now bank-restricted with a fallback; three `MOSAIC_RENAME_MUTANT_BIAS_*` controls; a header section stating the rule; `REN_BANK_MASK` |
| `rtl/core/mosaic_core.sv` | **changed**: the two new inputs tied off (`1'b0`, bank 0) — the p0 core has no consumer of the policy, and off is the pre-I-032 allocator |
| `sim/tb/mosaic_rename_tb.sv` | **changed**: the two inputs passed straight through, widths from the generated package |
| `sim/tb/mosaic_retire_tb.sv` | **changed**: the two inputs tied off (it instantiates `mosaic_rename`) |
| `sim/unit/tb_rename.cpp` | **changed**: `Stim`/shadow carry the preference; the shadow's scan implements it; the harness drives it; a new `bank-bias` phase; `PhaseRandom` gains an optional bias mode; one latent free-stimulus defect fixed (§5) |
| `tools/run_rename_controls.py` | **new**: the reproducible control runner for this and the pre-existing rename mutants |
| `results/reports/I-032-bank.md` | this file |

Not touched: `tests/unit/registry.json` (the case is registered and still
`"pending": true` — the integration lead clears that marker when it records the
package; this lane does not edit the registry),
`config/status/implementation_status.json`, `results/PROGRESS.md`,
`tests/programs/**`, and every other case's driver.

**No configuration change.** The entry count and the bank count already come from
`config/geometry/p0.json` (`int_prf.entries = 96`, `banks = 4`), which
`tools/gen_manifest.py` turns into `mosaic_cfg_pkg::MOSAIC_INT_PRF_ENTRIES` /
`MOSAIC_PRF_BANKS` / `MOSAIC_PRF_BANK_W`. Nothing here adds a CSR, a mode or a
profile knob, so no `config/csr/p1*` table and no `gen_manifest.py` change was
needed.

## 2. The interface delta

| New port | Dir | Width | Meaning |
|---|---|---|---|
| `alloc_bias_en` | in | 1 | enable the preference for this cycle's group |
| `alloc_bias_bank` | in | `REN_BANK_W` (2) | the preferred home bank |

`alloc_bias_en` low is bit-identical to the pre-I-032 allocator (the search set
is `free_bits` unchanged). The three instantiators of `mosaic_rename`
(`sim/tb/mosaic_rename_tb.sv`, `rtl/core/mosaic_core.sv`,
`sim/tb/mosaic_retire_tb.sv`) were all updated in the same edit — Verilator's
`PINMISSING` is fatal here, which is why the tree cannot be left half-wired.

## 3. The policy, stated once

The header's decode is `bank = tag[TAG_W-1:ROW_W]` with
`ROW_W = $clog2(ENTRIES/BANKS) = $clog2(24) = 5`, so `bank = tag >> 5`. For each
lane the search set is:

```
preferred       = free_bits & bank_mask(alloc_bias_bank)
lane search set = (alloc_bias_en && preferred != 0) ? preferred : free_bits
```

Lane 1 uses the same rule over its own base set (the free set with lane 0's tag
removed), so a two-wide group is still "two consecutive single-width allocations"
under the bias. Three properties follow from the shape, not from a check bolted
on:

* **No leak, no theft.** The search set is always a subset of the free set, so
  the scan can never return an owned tag.
* **No deadlock.** The restriction applies only when the intersection is
  non-empty; an empty preferred bank falls back to any legal free tag.
* **No change of decision.** Acceptance is still `free_count >= group_need`, so
  the bias cannot turn an allocation into a refusal or the reverse.

A 96-entry file over 4 banks is not uniform: with `bank = tag >> 5` the tags
`0..95` occupy banks 0, 1 and 2, and **bank 3 holds no tags at all** — the
natural "the preferred bank is empty" case, which the case drives.

## 4. The case

`rename.bank_bias_exhaustion` runs its own `bank-bias` phase first (a bias defect
is then named by the phase that owns it, not by whichever later phase trips over
it), then the whole single-width campaign with the bias **off** — which is the
pre-I-032 allocator and therefore re-proves the I-013/I-018 behaviour under the
new code — and finally the random campaign again with the bias **on**.

```
$ python3 tools/run_unit.py --profile p0 --case rename.bank_bias_exhaustion
PASS rename.bank_bias_exhaustion  task=I-032
RESULT PASS rename.bank_bias_exhaustion rename contract holds: 10195 shadow
comparisons over 10275 cycles, 96 entries / 4 banks, seed 1
```

11 checks, 0 failures. The `bank-bias` phase does three things:

1. **Preference honoured, fallback taken.** One biased allocation is driven for
   every bank. For each bank the phase first asks (from the shadow's pre-edge
   free set) whether that bank has a free tag: if it does, the reported tag must
   be in it; if it does not (bank 0 at reset, and bank 3 always), the allocation
   must still be accepted and must land elsewhere. The free count must move by
   exactly one.
2. **Exhaustion of the preferred bank.** The preferred bank is filled to empty
   (32 tags), then a biased allocation is driven again: it must be accepted, land
   outside the preferred bank, and take exactly one tag. Then a two-wide group is
   driven when the preferred bank has exactly one free tag: lane 0 must take it,
   lane 1 must fall back on its own, and the two tags must differ.
3. **The measured delta.** One deterministic 240-cycle campaign is run twice
   with identical stimulus, once bias-off and once bias-on with a fixed preferred
   bank, under closed-loop occupancy (below 40 free tags it retires, above it
   allocates) so the preferred bank stays available for most of the run. The
   counts are reported; the conservation quantities are *asserted*:

```
check ok: bank-bias: the preference was honoured for every bank with a free tag
and fell back for the bank with none; a fully emptied preferred bank (32 tags)
did not stall the next allocation, and lane 1 fell back on its own; and over 240
identical cycles the measured delta was stalls 0->0, read-port conflicts
313->259, lane-pair bank conflicts 9->9, with 75 preferred-bank hits and 53
fallbacks, bank histogram [31,55,42,0]->[0,75,53,0], and an identical free-count
trajectory
```

| metric (240 identical cycles) | bias off | bias on | delta |
|---|---|---|---|
| allocation stalls (`alloc_exhausted` cycles) | 0 | 0 | 0 |
| read-port conflicts (the two source tags of one macro share a bank) | 313 | 259 | **−54** |
| lane-pair bank conflicts (both lanes of a group in one bank) | 9 | 9 | 0 |
| preferred-bank hits | — | 75 | |
| fallbacks (preferred bank empty) | — | 53 | |
| bank histogram of accepted lane-0 allocations | `[31,55,42,0]` | `[0,75,53,0]` | |
| free-count trajectory | — | — | **identical** |

The assertion is *not* that conflicts improved — it is that the trajectory of the
free count, the accepted-allocation counts and the stall count are identical
between the two runs, and that the bias actually took effect (75 hits, 53
fallbacks, and a completely different bank histogram: bank 0 is never chosen when
the bias is on, bank 1 and bank 2 take the load). The conflict counts are a
**proxy**: the rename presents two source tags per macro, and this PRF has one
read port per bank, so two source tags in one bank are a read-port conflict. The
PRF's own arbitration is measured separately by `CASE=prf.read_bank_collision`;
this tb does not instantiate the PRF, and the report says so in §7.

The random phase with the bias on shadow-compares every cycle (free set, both
maps, generations, `wb_done`, the undo window) and asserts the campaign actually
drove the feature:

```
check ok: random: 238 allocations, 110 accepted writebacks, 897 stale
rejections, 263 squashes, 0 exhaustion reports over 4000 cycles, bias on 2351
cycles (109 preferred, 28 fallback)
```

The standing invariants of the harness run on every cycle of the whole case:
`free_count` equals the popcount of the free mask; every live speculative mapping
and every committed mapping names a tag that is **not** free; no allocation
returns a tag that is free-owned or that lies outside the file; the two lanes of a
group never share a tag; the undo window never overflows; and the free-set
conservation identity over the recovery window holds.

## 5. Defect found and fixed — in the campaign, not the RTL

The first run of the bias-on random phase failed:

```
MISMATCH bank-bias-random: cycle 7016: x14 maps to tag 68, which is also in the
free set: one physical register has two owners
```

The shadow agreed with the DUT (the whole-state comparison passed on that cycle),
so the *shared* state was inconsistent — which meant the campaign had driven the
module into a state its own contract forbids. `PhaseRandom` releases a tag with a
generation "nudged off the handed value":

```cpp
s.free_.gen = (s.free_.gen + 1 + rng.Below(3)) & h->shadow_gen_mask();
```

That is only guaranteed to be stale while the handed tag has not been recycled.
Once the tag has been allocated again, the *current* generation is one to three
above the handed one, the nudged identity is a live, current identity, and the
module **correctly** accepts a release of a register an architectural register
still points at — the caller bug the campaign's own comment says it never drives.
The bias did not cause it; shifting the RNG stream (the bias draws consume RNG
values) merely walked the campaign into it. The fix makes the release stale **by
construction**, stepping the generation off the tag's *current* value read from
the shadow, in both the handed branch and the random branch, with the same number
and bound of RNG draws so no other case's stream moves:

```cpp
const uint32_t cur = h->shadow_gen_of(s.free_.tag);
s.free_.gen = (cur + 1 + rng.Below(3)) & h->shadow_gen_mask();
```

This is a real latent defect in the I-013/I-014/I-018 random campaign: it could
drive an accepted release of a live register on some other seed. It is fixed here
because this card is what exposed it, and because a campaign that can drive an
illegal stimulus is not evidence about the contract. The existing rename cases
were re-run and their `RESULT` lines are unchanged (same comparison counts:
5646/5686, 9767/9843), which shows the stream and the decisions did not move.

## 6. Mutation testing

`tools/run_rename_controls.py` builds the shipping case from an **empty**
directory, then each mutant from its own empty directory with its `-D` on the
recorded command line (`build/p0/rename_controls/<define>/build_command.txt`),
requires the binary to differ (`cmp`) from the shipping one, and requires exit 1
with a named first failure. The three I-032 mutants run against the bank case; the
seven rows after them re-verify the pre-existing rename controls whose guard
regions this card edited (the scan, the group-accept decision and the allocated
destination), on the cases that own them.

Shipping, from an empty directory:

| case | binary SHA-256 | exit | result |
|---|---|---|---|
| `rename.bank_bias_exhaustion` | `a22a460be01450624341d8cc82c03bec2425d8d5aafaef305964740e7ddf9bc3` | 0 | PASS — 10195 comparisons, 10275 cycles |
| `rename.same_cycle_chain` | `43761d5c54b17c2b7b91042dd922e7def9079a9bff6badb24c3d5232b1389e73` | 0 | PASS — 9767 comparisons, 9843 cycles |
| `rename.single_width_ownership` | `67db74e580e9f0ac26af4cbfd5153f7098755a817b148077acbcf36e3022f799` | 0 | PASS — 5646 comparisons, 5686 cycles |

Mutants (every one: binary ≠ shipping, exit 1):

| # | Define | Case | Injected defect | SHA-256 | First failure |
|---|---|---|---|---|---|
| 1 | `MOSAIC_RENAME_MUTANT_BIAS_DEADLOCK` | bank | the preference becomes a requirement: an empty preferred bank refuses the group | `c1df02b9640bc41f…` | `bank-bias: cycle 12: alloc_accepted: expected 1, got 0 [alloc=1:x5 … bias=b0]` |
| 2 | `MOSAIC_RENAME_MUTANT_BIAS_UNRELEASED` | bank | the preferred mask is used without intersecting the free set | `15c1def7adcf50b4…` | `bank-bias: cycle 12: alloc_new_tag: expected 32, got 0 [alloc=1:x5 … bias=b0]` |
| 3 | `MOSAIC_RENAME_MUTANT_BIAS_ARCH` | bank | the bias leaks into the destination identity (generation from the unbiased tag) | `0ba7856580564a18…` | `bank-bias: cycle 428: alloc_new_gen: expected 1, got 0 [alloc=1:x18 … bias=b1]` |
| 4 | `MOSAIC_RENAME_MUTANT_NO_EXHAUST_CHECK` | 2w | exhaustion not detected | `2845409359ccc10b…` | `exhaustion: cycle 1678: alloc_accepted: expected 0, got 1` |
| 5 | `MOSAIC_RENAME_MUTANT_NONATOMIC_GROUP` | 2w | the group is not atomic | `eb9ac9245480fa9d…` | `twowide-stall: cycle 5781: alloc_accepted: expected 0, got 1` |
| 6 | `MOSAIC_RENAME_MUTANT_X0_ALLOC` | 2w | x0 allocates a tag | `667553258cc41b68…` | `x0: cycle 1399: alloc_new_valid: expected 0, got 1` |
| 7 | `MOSAIC_RENAME_MUTANT_SAME_TAG_LANE1` | 2w | lane 1 reuses lane 0's tag | `a779943906f8e62d…` | `twowide-raw: cycle 5692: alloc2_new_tag: expected 34, got 33` |
| 8 | `MOSAIC_RENAME_MUTANT_WAW_OLD_FROM_MAP` | 2w | lane 1 reports the wrong displaced mapping | `786fc8e4f13db3c4…` | `twowide-waw: cycle 5702: alloc2_old_tag: expected 32, got 7` |
| 9 | `MOSAIC_RENAME_MUTANT_NO_FREE_RESTORE` | 1w | a squash does not restore the free set | `bf7d4ef33178a72e…` | `squash: cycle 1527: the free set differs from the shadow at tag 35` |
| 10 | `MOSAIC_RENAME_MUTANT_CKPT_ALLOC_LEAK` | 1w | a checkpoint-cycle allocation is not journalled | `33621dde8ca20068…` | `random: cycle 1887: the undo window holds 0 entries but the shadow says 1` |

`2w` = `rename.same_cycle_chain`, `1w` = `rename.single_width_ownership`.

Delta against the shipping build: **0 → 1 failing check** in every row, the run
aborting at the first failure. Rows 4–10 reproduce the failures recorded in
`results/reports/mutant-reverification-2026-10-01.md` (Round 3) exactly, except
row 10, whose first divergence is the post-I-018 form (`random: cycle 1887: the
undo window holds 0 entries but the shadow says 1`) that `I-018-journal.md`'s
staleness note already points to — the recorded `random: cycle 1890: the free set
differs…` was the pre-journal-FIFO behaviour.

The three new mutants are exactly the card's Fail modes plus its "bias must not
change the architectural result" safety claim:

* **1** is "仅指定 bank 空就永久卡住" — the preferred bank being empty deadlocks
  allocation. Caught on the very first bank that has no free tag (bank 0 at
  reset), in the cycle the bias is first offered.
* **2** is "使用未释放 physical tag" — the preference picks a tag that is still
  owned. Caught because the same first biased allocation reports tag 0, which the
  reset mapping owns, where the fallback must have reported tag 32.
* **3** is a bias that changes the architectural result: the produced
  `(tag, generation)` is not the chosen tag's next generation, so the writeback
  that should deliver the register's value is judged stale and the register is
  never written. Caught once the biased and unbiased tags have diverged
  generations (cycle 428 of the delta campaign).

## 7. What is NOT verified

* **Translation.** Nothing here translates: `satp`/`Sv39` are I-045's and are not
  implemented, so no address in this case is virtual and none is claimed to be.
  The case is a rename/PRF-allocator unit case; it has no address space at all,
  which is how a "virtual-address-shaped" test is kept from passing vacuously —
  there is no such test here, and the report does not claim one.
* **A real PRF read-port conflict.** The read-port delta above is computed from
  the two source tags' banks, which is the condition under which a one-port-per-
  bank PRF would need two reads; the PRF itself is not instantiated in
  `mosaic_rename_tb` (it is listed in the registry entry but the tb is
  rename-only). The PRF's arbitration and collision behaviour is measured by
  `CASE=prf.read_bank_collision`, not here.
* **The bias in the core.** `mosaic_core.sv` ties the preference off, so the
  integrated core's architectural behaviour is unchanged by this card and is
  *not* re-measured with the bias on. There is no consumer of the policy in p0;
  the policy is delivered and proved in isolation, as the plan's I-032 asks.
* **Other profiles.** Only `p0` was built. Everything is derived from the
  generated package (`MOSAIC_INT_PRF_ENTRIES`, `MOSAIC_PRF_BANKS`,
  `MOSAIC_PRF_BANK_W`), and p1–p3 have the same `int_prf` geometry, but no p1–p3
  run was made. No p1 CSR table or profile change was needed.
* **The core-level blast radius after the last edit.** `retire.head_block_and_dual`,
  `commit.head_block_and_dual`, `core.trap_csr_program`, `core.mem_program`,
  `core.unwritten_reg_read`, `fabric.fixed_two_cluster`, `trap.precise_state`,
  `core.corpus_sweep` and `core.corpus_branch` were run against the revision
  before the last two cosmetic edits (the `bank_of`→shift form and the driver's
  unused-member removal) and all **PASS**; the two retire cases were re-run after
  them and still PASS. The core cases could not be re-run afterwards because a
  concurrent lane (I-044) left `mosaic_csr.sv`/`mosaic_core.sv` mid-edit
  (`mosaic_csr_pkg::MOSAIC_CSR_WMASK_SEPC` undefined, `mstatus_sie_i`
  unconnected), which makes `mosaic_core.sv` non-elaborating for reasons that are
  not this change. The rename cases (which do not compile `mosaic_core.sv`) were
  re-run last and pass.
* **`rename.bank_bias_exhaustion`'s registry marker.** Left `"pending": true`.
  The registry is the integration lead's file; this lane does not edit it.

## 8. Gates

* Scoped Verilator lint of the changed module:
  `verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_rename
  -Ibuild/p0/rtl -Irtl/common -Irtl/core -Irtl/fabric -Irtl/vector -Irtl/soc
  rtl/core/mosaic_rename.sv` → exit 0, clean.
* `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl -I rtl/common
  -I rtl/core rtl/core/mosaic_rename.sv` → exit 0 (only the file's pre-existing
  `STYLE-2` port-suffix warnings).
* `c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow` on `sim/unit/tb_rename.cpp`
  → the only warning is the pre-existing `variable 'windows' set but not used`
  in `PhaseRandom`; no warning from any line this change added.
* `python3 tools/check_records.py` → `ok records agree: 48 delivered package(s),
  57 registered case(s)` (exit 0). The registry/status files were not edited, so
  this is a check that nothing was accidentally recorded.
* The project-wide `tools/lint_rtl.py --profile p0` was **not** run to a verdict
  because the tree was mid-edit by the I-044 lane at the time (§7); the scoped
  lint above is the evidence for this card's file.

## 9. How to reproduce

```
python3 tools/run_unit.py --profile p0 --case rename.bank_bias_exhaustion
python3 tools/run_unit.py --profile p0 --case rename.single_width_ownership
python3 tools/run_unit.py --profile p0 --case rename.same_cycle_chain
python3 tools/run_unit.py --profile p0 --case rename.journal_window
python3 tools/run_rename_controls.py
```
