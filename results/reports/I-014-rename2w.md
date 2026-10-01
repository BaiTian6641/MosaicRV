# I-014 — two-wide rename and the same-cycle dependency

Work package I-014, card `### I-014` in `docs/implementation-plan.md`.
Case `CASE=rename.same_cycle_chain` (new), `CASE=rename.single_width_ownership`
(I-013, kept green).

Files touched:

| File | Role |
|---|---|
| `rtl/core/mosaic_rename.sv` | extended: second allocation lane, second scan, same-cycle bypass, group-atomic accept, two undo entries per group |
| `sim/tb/mosaic_rename_tb.sv` | extended: the new ports passed straight through, both commit lanes now driven |
| `sim/unit/tb_rename.cpp` | extended: the shadow models a two-lane group; seven new phases |
| `results/reports/I-014-rename2w.md` | this file |

Files **not** touched, per the integration rules: `tests/unit/registry.json`
(the case was already registered), `config/status/implementation_status.json`,
`results/PROGRESS.md`, and every module/testbench owned by another package —
including `sim/tb/mosaic_retire_tb.sv`, whose instantiator had to tie the new
inputs off (see §7).

All commands are from `/Users/flare/MosaicRV`.

```
python3 tools/run_unit.py --profile p0 --case rename.same_cycle_chain
python3 tools/run_unit.py --profile p0 --case rename.single_width_ownership
verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_rename \
    -Ibuild/p0/rtl -Irtl/common -Irtl/core rtl/core/mosaic_rename.sv
slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_rename.sv
```

Result: **both cases PASS**; Verilator lint exit 0, slang-tidy exit 0 (the only
slang warnings are the file's pre-existing style classes: `NoLegacyGenerate`,
`AlwaysCombBlockNamed`, `EnforcePortSuffix`); the driver compiles clean under
`-Wall -Wextra -Wshadow` on its own, with no warning attributable to
`sim/unit/tb_rename.cpp`.

---

## 1. The interface delta

Geometry is unchanged and still comes from the generated package: tag and
generation are 7 bits, `rd`/source addresses 5 bits, 96 entries, 64 ROB entries,
so `dbg_j_len` is 7 bits (`$clog2(64+1)`).

| New port | Dir | Width | Meaning |
|---|---|---|---|
| `alloc2_req`, `alloc2_rd` | in | 1, 5 | lane 1 of the group: valid, destination |
| `alloc2_accepted` | out | 1 | `alloc_accepted && alloc2_req` |
| `alloc2_exhausted`, `alloc2_squashed` | out | 1 | the group's refusal reason, on lane 1 |
| `alloc2_is_x0`, `alloc2_new_valid`, `alloc2_new_{tag,gen}` | out | 1,1,7,7 | lane 1's answer and destination |
| `alloc2_old_valid`, `alloc2_old_{tag,gen}` | out | 1,7,7 | the mapping lane 1 displaces |
| `rs3_addr`, `rs4_addr` | in | 5,5 | lane 1's two sources |
| `rs3_is_x0`/`rs4_is_x0`, `rs3_ready`/`rs4_ready`, `rs3_bypass`/`rs4_bypass`, `rs{3,4}_{tag,gen}` | out | | lane 1's resolved sources |
| `rs1_ready`, `rs2_ready` | out | 1 | readiness for the **pre-existing** lane-0 sources |
| `dbg_j_len` | out | 7 | undo-window depth (observation only) |

`commit2_*` already existed (I-017) and is unchanged; in this testbench it is now
driven by the driver instead of being tied low inside the wrapper, because the
WAW release contract is a statement about what the two commit lanes release.

## 2. How a group of one reduces to I-013

The single-width port set is unchanged and means "a group of one": `alloc_req` is
lane 0's valid bit, and with `alloc2_req` low the acceptance test
`free_count >= need` has `need ∈ {0,1}` — identical to the old
`has_free || rd == 0`. Lane 1's scan is computed either way but has no effect,
the journal pushes one entry, the rotation point follows lane 0, and `rs1`/`rs2`
carry the same values as before. The only addition on the old ports is
`rs1_ready`/`rs2_ready`.

`CASE=rename.single_width_ownership` — the same eight phases, the same shadow
file — passes, which is the evidence for that claim. The shadow is now the
*two-lane* model with lane 1 permanently inactive, so the single-width path is
checked against the same model that checks the group rather than against a
separately maintained one.

## 3. What the group does

**Two scans, one function.** `scan_free(mask, ptr)` is called twice; lane 1's
mask has lane 0's tag cleared and its pointer starts one past it, so a two-wide
group takes exactly the two tags two consecutive single-width allocations would
have taken, in the same order, and the rotation point ends where they would have
left it. The exclusion is what makes "the two lanes never take the same tag" a
property of the scan rather than a check bolted on.

**Atomicity.** `group_need` is the two-bit count of lanes writing a non-x0
destination; `alloc_accepted` is `need == 0 || free_count >= need`. A group of two
with one free tag is refused whole: neither lane allocates, the free set, the
speculative map, the committed map and the undo window are all untouched, and the
tag is still there for the next cycle's single-width allocation. `alloc2_accepted`
is the same decision gated on lane 1 being present, so a lane-1-without-lane-0
group is refused rather than invented.

**The same-cycle bypass.** Lane 1's sources read the start-of-cycle map, except
that a source naming lane 0's destination (`alloc_new_valid && rs3_addr ==
alloc_rd`) resolves to lane 0's new *(tag, generation)*, sets `rs3_bypass`, and
reports `rs3_ready = 0`. Readiness in general is `x0 || wb_done[tag]` — the
producer's result is in the PRF — and the bypass *means* that condition is false,
so it is 0 by construction rather than by a lookup. The bypass follows the
allocation, not the request: in a refused group, and when lane 0 writes x0, there
is no new destination and lane 1 reads the map.

**WAW.** Lane 1 lands on the speculative map after lane 0, so the younger macro
wins the register; `alloc2_old` is lane 0's *new* destination when both lanes
write the same rd (that is the mapping lane 1 supersedes), and the start-of-cycle
mapping otherwise. The two releases then happen one per commit lane.

**The journal.** A group pushes lane 0's entry and then lane 1's, so the window
stays in allocation order and the existing oldest-first undo — whose generation
rollback relies on exactly that order — inverts both allocations. `dbg_j_len`
makes the depth observable, so "two allocations in one cycle were journalled as
two entries" is asserted in the cycle it happens rather than inferred one squash
later.

## 4. Defect found and fixed: the second commit lane released nothing

The first run of the new WAW phase failed against the shadow at tag 34. The RTL
was wrong, and the shadow was right.

`commit2_supersedes` (added by I-017, inside the file this card owns) read
`cmt_map_q[commit2_rd]`. `cmt_map_q` is a variable assigned by the *same*
combinational block that later writes lane 1's own install into it, and a
continuous assign reading such a variable sees the settled, post-block value.
Measured, not assumed — a minimal experiment settles it:

```systemverilog
module probe(input logic clk, input logic a, output logic out);
  logic [3:0] v;
  always_comb begin
    v = 4'd0;
    if (a) v[0] = 1'b1;   // v[0] becomes 1 after the block
  end
  assign out = v[0];      // does the continuous assign see the settled value?
endmodule
```
```
$ verilator --cc --exe --build -o probe probe.sv main.cpp && ./obj_dir/probe
out=1
```

So whenever lane 1 was accepted, `cmt_map_q[commit2_rd]` *was* `commit2_tag` and
`cmt_gen_q[commit2_rd]` *was* `commit2_gen`: both disjuncts of the test compared
`commit2_tag` with itself, and `commit2_supersedes` was therefore **false on every
cycle**. Lane 1 installed its mapping and released nothing — a leaked physical tag
per two-wide retirement, invisible until spurious exhaustion dozens of
instructions later. The file's own comment claimed the opposite ("compared against
`cmt_map_q`, not `cmt_map` … lane 1 must see the map as lane 0 left it"), which is
what made the intent clear and the implementation wrong.

Fix: `cmt_map_l0`/`cmt_gen_l0`, the committed map as lane 0 leaves it (pre-edge
map plus lane 0's install), computed in its own combinational block. Lane 1's
`supersedes` test and the tag it releases both read it. This is a behaviour change
to a lane I-017 owns textually but which this card's acceptance criteria govern
("macro 1's old mapping at macro 1's commit"). I-013's phases never drive
`commit2_valid`, so the single-width case is unaffected; `commit2_supersedes` is
now exactly the documented rule.

The mutant `WAW_COMMIT2_PRE_MAP` reproduces the original symptom (a free set that
differs from the shadow) by putting lane 1 back on the pre-lane-0 map.

## 5. The case: phases, comparisons, first mismatch

`rename.same_cycle_chain` runs the eight single-width phases **and** seven
two-wide phases, in that order, so a two-wide change that broke the single-width
path is caught by the phase that owns that path. 9750 shadow comparisons over
9818 cycles, 16 checks, 0 failures:

```
check ok: reset-state: the cold state is the documented one
check ok: ownership: every tag had exactly one owner at every cycle, and duplicate producers and double frees were reported
check ok: generation: tag 32 was recycled from (32,0) to (32,1) after 65 allocations, ...
check ok: wrap: the rotation point wrapped 2 times over 200 allocations, ...
check ok: x0: writes to x0 allocated nothing, the free count never moved, ...
check ok: squash: the committed map survived, the free set was restored exactly, ...
check ok: exhaustion: the 64 allocatable tags were consumed, ...
check ok: random: 112 allocations, 65 accepted writebacks, 791 stale rejections, 235 squashes, 115 exhaustion reports over 4000 cycles
check ok: twowide-raw: lane 1's source x5 resolved to lane 0's in-flight destination (33,0) and was reported not-ready, while the other three sources came from the map with writeback-derived readiness
check ok: twowide-war: lane 0's source x6 stayed on the start-of-cycle mapping (6,0) while lane 1 allocated (33,0) for x6 in the same group
check ok: twowide-waw: both macros of a WAW pair got distinct tags ((32,0) and (33,0)), each commit released exactly its own predecessor, and a same-cycle pair retirement released exactly two mappings
check ok: twowide-x0: a group's tag requirement was 1 for {x0, rd} and {rd, x0} and 0 for {x0, x0}, and x0 never consumed a tag on either lane
check ok: twowide-stall: a two-tag group with one free tag stalled whole (63 undo entries before and after), changed nothing, and left the tag for the next single-width allocation
check ok: twowide-ckpt: a two-wide group pushed two undo entries, and the squash returned both tags, stepped both generations back and restored the maps exactly
check ok: twowide-random: 2206 accepted groups (1165 of them two-tag), 1303 bypasses, 1021 group tag stalls, 61 same-cycle pair retirements, 653 x0 lanes, 45 recovery windows/44 squashes, over 4000 shadow-compared cycles
check ok: no contract violation in any phase
```

Per-cycle, the shadow is compared field by field on every output (including the
two lanes' tags, generations, displaced mappings, readiness and bypass flags) and
on the whole state (free set, `wb_done`, `gen_valid`, generations of valid tags,
both maps, undo depth). The standing invariants add what a shadow comparison
cannot: `free_count` equals the popcount of the mask; no live speculative mapping
and no committed mapping names a free tag; no tag outside the file is allocated;
the two lanes of a group are accepted together and never share a tag; the journal
never overflows.

The two-snapshot rule is respected throughout: `observed()` holds the pre-edge
combinational answers (grant payloads, the bypass) and is what the phase
assertions read, while occupancies, counters and the maps are read from the
registers after the edge.

### The card's Fail criterion, explicitly

`twowide-stall` fills the register file down to **one** free tag, presents a
group that needs two, and then asserts, after the edge, that the free mask, the
speculative map, the committed map and the undo depth are all unchanged — no tag
allocated that no macro owns, none leaked, none allocated twice — and that the
very next *single-width* allocation is accepted and takes that one tag. The
invariant `!free_mask[spec[a].tag]` and the shadow comparison run on those cycles
too, so a leak would have to be invisible to both to survive.

## 6. Mutation testing

Six controls, each rebuilt with one extra define on the case's Verilator command
line and run directly:

```python
import sys; sys.path.insert(0, 'tools'); import run_unit
run_unit.VERILATOR_FLAGS.append('-DMOSAIC_RENAME_MUTANT_<NAME>')
run_unit.build_case('p0', CASE, run_unit.load_registry()['cases'][CASE])
# then: build/p0/unit/<CASE>/<CASE> --case <CASE> --out /tmp/mut --seed 1 --max-cycles 200000
```

Shipping build for reference: **exit 0, 16 checks, 0 failures, PASS**,
sha256[0:16] `cd6f15e15ea0697d`.

| # | Define | Injected defect | Phase that catches it | `ifdef` | binary ≠ shipping | exit | checks | failures |
|---|---|---|---|---|---|---|---|---|
| 1 | `NO_BYPASS` | no same-cycle bypass: lane 1 reads the start-of-cycle map | `twowide-raw` | 1 | yes (`e41e07db6367bc83`) | **1** | 9 | 1 |
| 2 | `NONATOMIC_GROUP` | lanes accepted on their own requirement, so a two-tag group half-allocates | `twowide-stall` | 1 (`elsif`) | yes (`8282f24af18b1014`) | **1** | 13 | 1 |
| 3 | `X0_ALLOC` | a write to x0 allocates a tag, on either lane | `x0` (single-width phase) | 1 | yes (`a50c5ed748fda129`) | **1** | 5 | 1 |
| 4 | `SAME_TAG_LANE1` | lane 1 reuses lane 0's tag instead of the second scan | `twowide-raw` | 1 | yes (`f1f1d827049d4e3c`) | **1** | 9 | 1 |
| 5 | `WAW_OLD_FROM_MAP` | lane 1 reports the start-of-cycle mapping as the one it displaces | `twowide-waw` | 1 | yes (`23dd81d16d3ae05a`) | **1** | 11 | 1 |
| 6 | `WAW_COMMIT2_PRE_MAP` | the second commit lane works from the pre-lane-0 map, comparison *and* released tag | `twowide-waw` | 2 | yes (`936c5d61ef7dec30`) | **1** | 11 | 1 |

Delta against the shipping build: **0 → 1 failing check** in every case, with the
run aborting at the first failure (hence the lower `checks` totals), and the
phase named in the failure is the phase that owns the injected defect.

First mismatch, verbatim:

```
MISMATCH twowide-raw: cycle 5692: rs3_bypass: expected 1, got 0 [alloc=1:x5 alloc2=1:x6 rs=x9,x20 | x5,x9 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]
MISMATCH twowide-stall: cycle 5781: alloc_accepted: expected 0, got 1 [alloc=1:x20 alloc2=1:x21 rs=x0,x0 | x0,x0 ...]
MISMATCH x0: cycle 1399: alloc_new_valid: expected 0, got 1 [alloc=1:x0 alloc2=0:x0 ...]
MISMATCH twowide-raw: cycle 5692: alloc2_new_tag: expected 34, got 33 [alloc=1:x5 alloc2=1:x6 ...]
MISMATCH twowide-waw: cycle 5702: alloc2_old_tag: expected 32, got 7 [alloc=1:x7 alloc2=1:x7 rs=x7,x7 ...]
MISMATCH twowide-waw: cycle 5706: the free set differs from the shadow at tag 34 [alloc=1:x8 alloc2=1:x8 ...]
```

Two notes on honesty here. First, mutant 6 initially had a body that changed only
the *comparison* while still releasing the tag from `cmt_map_l0` — for a WAW pair
that is the correct tag, so the mutant was behaviourally identical to the shipping
build and **passed**. That is the "an `ifdef` whose body does not exist proves
nothing" trap, one step along: the body existed and was inert. It now changes both
halves (comparison *and* released tag) and fails with a 0 → 1 delta. Second, the
binary-difference column is the check that each define really reached the
elaborator; a define that compiled the shipping build would show up there.

Mutant 3 also fails in the *single-width* phase `x0`, which is the I-013 case's
own control, now covering both lanes.

## 7. Effect on other packages, and what they must do

Adding ports to `mosaic_rename` is a cutover: every instantiator must connect
them, because Verilator treats an unconnected input as a PINMISSING warning and
warnings are fatal by default in this build.

* `sim/tb/mosaic_retire_tb.sv` (owned by the retire package, **not edited here**)
  instantiates `mosaic_rename`. Its new inputs are `alloc2_req`, `alloc2_rd`,
  `rs3_addr`, `rs4_addr` and the four new outputs plus `dbg_j_len`. The owner
  (`RetireFix`) was told the port list is frozen and is tying them off; a build
  of `retire.head_block_and_dual` during this work aborted on ten unrelated
  `WIDTHTRUNC` warnings **inside that file's own `mosaic_rob` instance** and
  reported no `mosaic_rename` pin warning, so that case is in flux for reasons
  that are not this change. With `alloc2_req`/`rs3_addr`/`rs4_addr` at 0 the
  retire case sees the pre-I-014 machine.
* The retire case does **not** check the rename free set, which is why the §4
  defect went unnoticed by it; now that lane 1 releases a tag per dual commit, any
  future check there must expect two releases per same-cycle WAW pair.

## 8. What is NOT verified

* **Other profiles.** Only `p0` was built and run. The module derives everything
  from the generated package, and `p1`/`p2`/`p3` have the same geometry, but no
  run was made against them.
* **The `retire.head_block_and_dual` case with the new ports tied off.** Its
  build was failing inside its own file when this report was written (see §7), so
  the claim "the retire case still passes" is *not* made here.
* **`journal_overflow` under a two-wide group.** The bound is 64 entries and a
  group of two adds two; the overflow path is exercised by the single-width
  campaign only in the sense that it never fires. The random soak reaches
  exhaustion (and therefore the journal bound) exactly when the free set empties,
  and the allocation that would exceed the bound is refused first, so overflow is
  unreachable from a drained checkpoint — but that argument is a design argument,
  not a run.
* **A checkpoint taken in the same cycle as an allocation.** The module's
  documented rule is that a checkpoint starts a new window, so that cycle's
  allocation is *not* journalled; whether that is coherent is discussed in §9 and
  no phase drives it.
* **Bank-awareness.** Nothing here is bank-aware; the two tags a group takes can
  land in the same bank. That is I-032's card.
* **Physical cost.** No synthesis, no timing, no area: two 96-bit scans and two
  32×7 map copies per cycle are added combinational logic, and whether they fit
  the target is not measured here.
* **X-propagation.** All runs are with `--x-initial unique` / `--x-assign unique`
  and a reset schedule, so nothing here says what the module does with X on its
  inputs.

## 9. Findings outside this card (reported, not changed)

1. **The same-cycle checkpoint-and-allocation corner** (pre-existing, I-013
   rule). `j_push0`/`j_push1` and the register block both skip journalling when
   `ckpt_valid` is high, and the same edge clears the window, so an allocation in
   a checkpoint cycle is neither journalled nor counted: a later squash to that
   checkpoint returns the map to the committed map (which does not contain it)
   while the tag stays allocated — a tag neither free nor owned, the state the
   module's own reset section calls out. I preserved the existing rule (extended
   to lane 1) rather than changing it: it is a journal-control change that I-018
   owns, and it would alter behaviour the I-013 report documents. Suggested fix,
   for that package: when `ckpt_valid && !squash` and a lane allocates, set
   `j_len_q` to the number of allocating lanes (and write their entries at
   indices 0 and 1) rather than to 0. No test in either case drives this corner.
2. **A squash restores the speculative map from the *committed* map**, so it can
   only be taken where the two agree. A checkpoint taken with older instructions
   still in flight loses their mappings on a squash — their tags stay allocated
   (they were allocated before the checkpoint and are not journalled) while
   nothing points at them. This is the module's documented behaviour and I-018's
   integration point (the checkpointed map is what the shared rename state will
   read); the soak therefore takes checkpoints only at a drained, committed
   boundary and says so in the phase.
3. **I-017's second commit lane had no test of its release.** The §4 defect is an
   example: the retire case checks that `commit2_accepted` is asserted and reads
   the committed map, but never the free set. Nothing in this card changes
   `sim/unit/tb_retire.cpp`, but a free-count check there would have caught a
   one-tag-per-dual-retirement leak.

## 10. What would change with more time

* Make `alloc2_old` unnecessary by defining lane 1's displaced mapping as "the
  map lane 0 left", i.e. the same `cmt_map_l0`-style intermediate used for
  commits, so the allocation and commit paths describe supersession the same way.
* Fold the two scans into one bank-strided scan for I-032, keeping the set
  semantics and the "lane 1 takes the next tag after lane 0" rule explicit.
* Extend the soak to drive a real ROB-shaped retire queue (per-macro sequence
  numbers) instead of per-register FIFOs, so that cross-register ordering effects
  are exercised as well.
