# I-013 — RAT, free list, x0, and the generation

Work package I-013, card `### I-013` in `docs/stage-1-scalar-control.md`.
Case `CASE=rename.single_width_ownership`.

Files delivered:

| File | Role |
|---|---|
| `rtl/core/mosaic_rename.sv` | the rename stage: committed RAT, speculative RAT, free set, generation table, undo journal |
| `sim/tb/mosaic_rename_tb.sv` | simulation wrapper; also hands the elaborated geometry to the driver |
| `sim/unit/tb_rename.cpp` | C++ driver + independent shadow model + eight phases |
| `results/reports/I-013-rename.md` | this file |

All commands are from `/Users/flare/MosaicRV`.

---

## 1. The interface, and its cycle semantics

One module, `mosaic_rename`, one clock, synchronous active-high `rst`. Five
request ports and four report ports, all sampled on the rising edge. Every
report is **combinational in the cycle it describes**: a request offered in
cycle N has its answer (`alloc_accepted`, `wb_stale`, …) visible in cycle N and
its effect visible in cycle N+1. That is what lets a caller decide in cycle N
whether its request will land, which is what makes the refusals usable as
back-pressure rather than as surprises one cycle later.

| Port | Direction | Meaning |
|---|---|---|
| `alloc_req`, `alloc_rd` | in | rename one destination, single width |
| `alloc_accepted` / `_exhausted` / `_squashed` / `_is_x0` | out | the answer, **with its reason** |
| `alloc_new_valid`, `alloc_new_{tag,gen}` | out | the allocated destination identity |
| `alloc_old_valid`, `alloc_old_{tag,gen}` | out | the mapping `rd` held before — the displaced mapping |
| `rs1_addr`/`rs2_addr` → `rs{1,2}_{tag,gen,is_x0}` | in → out | combinational source reads, zero latency |
| `wb_valid`, `wb_{tag,gen}` → `wb_accepted`/`_stale`/`_duplicate` | in → out | a result lands on exactly one destination |
| `free_valid`, `free_{tag,gen}` → `free_accepted`/`_stale`/`_double` | in → out | release a live mapping |
| `commit_valid`, `commit_rd`, `commit_{tag,gen}` → `commit_accepted`/`_x0_dropped` | in → out | in-order retire; installs the committed mapping, releases the one it supersedes |
| `ckpt_valid`, `squash` → `squash_accepted`/`_underflow`/`journal_overflow` | in → out | the recovery points I-018 snaps |
| `free_count` | out | free tags available, defined as the population count of the free set itself |

Three decisions in that table are load-bearing and are stated as rules rather
than left to be discovered:

**Every refusal names its reason.** `alloc_accepted` low is not an answer a
caller can act on. The three reasons — exhausted, squashed, and the x0 case
where there is nothing to refuse — need different responses from everything
upstream: retry later, do not retry, do not retry because there was never a
resource involved. The two real refusal reasons are **mutually exclusive**, and a
squash wins; reporting exhaustion during a squash would leave the caller unable
to tell "retry after the squash" from "retry when a tag frees". The test asserts
the exclusivity on every cycle, and the random campaign found the violation when
it was not enforced.

**A squash cycle refuses allocation.** Restore and allocate are mutually
exclusive, so their ordering is total rather than "restore wins, mostly". The
core is squashing in that cycle anyway; one bubble costs nothing and removes a
same-cycle write conflict on the speculative map from the design.

**A commit in a squash cycle is applied before the restore**, so the restored
speculative map reflects the commit. The alternative — restoring over the commit
— would resurrect the mapping the commit had just replaced.

---

## 2. State kept, and why

| State | Width | Reset? | Why it exists |
|---|---|---|---|
| `spec_map[32]`, `spec_gen[32]` | 14 b/reg | yes | what an instruction about to issue must read |
| `cmt_map[32]`, `cmt_gen[32]` | 14 b/reg | yes | what the ISA says the architectural registers hold |
| `free_bits[96]` | 1 b/tag | yes | the free list: a tag is free or owned, nothing else |
| `gen_valid[96]` | 1 b/tag | yes | whether a tag's generation has ever been allocated |
| `wb_done[96]` | 1 b/tag | yes | whether this generation has already been written |
| `gen[96]` | 7 b/tag | **no** | the allocation generation |
| `j_tag[64]`, `j_prev_valid[64]` | 8 b/entry | **no** | the undo journal |
| `alloc_ptr`, `j_len`, `ckpt_seen`, `j_overflow` | 7/7/1/1 b | yes | scan rotation, window length, recovery state |

Reset clears 4 × 96 bits of control state and 64 × 14 bits of map, and nothing
else. `gen` and the journal storage are never reset, which is the rule
`rtl/common/mosaic_ram.sv` documents: reset cost is control state, not
DEPTH × WIDTH of storage, and validity for both lives outside the arrays in
`gen_valid` and `j_len`.

The two maps **are** reset, and that is a deliberate departure. A 32-entry file
with two combinational read ports and one write port is flip-flops, not an
inferred RAM, so the no-full-array-reset rule — which exists to stop DEPTH ×
WIDTH resettable arrays — does not apply. Its reset state is architecturally
defined, so leaving it to power-up contents would make the first read of every
architectural register undefined.

### The reset free set is not all-ones, and that was a real defect

The first version reset `free_bits` to all ones while the maps pointed x0..x31 at
tags 0..31. That gives one physical register **two owners from cycle 0**: x5 is
mapped to tag 5 *and* tag 5 is on the free list, so the first instruction to write
any register could be handed tag 5 and a later writeback would silently corrupt an
architectural value.

Tags `0..ARCH_REGS-1` are therefore **owned at reset, not free**. The initial
mapping of x5 is released the moment the first write to x5 *commits*, through the
ordinary commit path — no special case.

It also makes the geometry add up, which is the check that convinced me it was
right rather than merely safe:

```
ARCH_REGS owned + (ENTRIES - ARCH_REGS) free = 96 - 32 = 64 = MOSAIC_ROB_ENTRIES
```

The free count at reset is **64, not 96**, and it is exactly the number of
instructions that can be in flight, which is exactly the bound the undo journal is
sized for. A program that has dispatched as many instructions as the ROB can hold
is genuinely out of tags.

### Why the generation exists, and why it is on the tag

`MOSAIC_INT_PRF_TAG_W = $clog2(96) = 7` bits names 128 values for 96 tags, and a
tag is handed out again as soon as its previous owner retires. **A tag on its own
cannot identify in-flight work.** The ABA case is not exotic: it needs 96
allocations, which is a few dozen instructions of straight-line code.

So a destination identity is the pair `(tag, generation)`, and the generation is
part of what the core passes around:

* an allocation of tag T produces `(T, gen(T)+1)`;
* a writeback is accepted only if it carries `(T, gen(T))` as it stands *now*;
* a late writeback from the previous owner of T carries the old generation and is
  rejected, so it cannot overwrite the new owner.

`gen` advances only on **allocation**, never on free, so a tag that cycles
free/allocate/free keeps counting up and a stale write from any earlier
incarnation is rejected.

The same mechanism makes a **squash** safe, which is why there is only one rule
to get right rather than two: rolling the generation back to its pre-allocation
value means a writeback escaping from a squashed instruction is rejected by
exactly the same check as one that was merely late.

### The free list is a set, not a stack — and the reason is recovery

A free list is normally a stack: O(1) pop. Its checkpoint is a single pointer,
and **that pointer is not a correct snapshot**. Consider a tag freed while the
stack head sits below the checkpoint: the push overwrites a stack slot that a
later pop will read, so restoring the pointer hands back a stack whose contents
are not the ones that were saved. That is the leak the architecture review names
as AR-011 — a free-list snapshot silently losing a register.

Restoring a *set* is exact. So this module restores a set, and the cost is that
allocation is a rotating priority scan over 96 bits rather than a pop: O(ENTRIES)
combinational logic for a stage that retires one allocation per cycle. For a first
scalar core that is the right trade — a scan that is obviously correct and
obviously restorable beats a pop that is cheap and subtly wrong. A banked or
hierarchical free list is a later optimisation and must keep the set semantics to
stay compatible with this interface.

---

## 3. Bank decoding, 96 entries over 4 banks

96 is deliberately not a power of two, so the decode is stated rather than
assumed:

```
ROW_W  = $clog2(ENTRIES / BANKS) = $clog2(24) = 5
bank   = tag[BANK_W + ROW_W - 1 : ROW_W]      bank index, 0..BANKS-1
row    = tag[ROW_W - 1 : 0]                   row inside the bank
```

`BANK_W + ROW_W == TAG_W` exactly (2 + 5 = 7), so the two fields together *are*
the tag: the decode is a **partition, not a hash**, and every tag has exactly one
home bank. Entries per bank are `ENTRIES / BANKS = 24`.

Because `ROW_W` is a `$clog2`, a row can name 32 rows of which only 24 exist, so
**tags 96..127 have no home bank**. They are never allocated (the free set only
ever contains 0..95) and any writeback or release aimed at one is rejected as out
of range. The rule is **refused, never wrapped**, for the reason
`rtl/core/mosaic_rename.sv`'s sibling `mosaic_predictor.sv` states for its own
truncation: wrapping an out-of-range index onto a valid one manufactures an alias
onto somebody else's register.

The bank decode is enforced as a **partition on the tag value itself** — no
separate bank/row state to disagree with — and the ownership checks in the
testbench re-derive the partition from the observed geometry, so a profile that
stopped dividing evenly would fail the case rather than mis-decode silently. The
divisibility, the tag width, the bank width, the row width, the
`BANK_W + ROW_W == TAG_W` identity, the architectural address width and the
occupancy width are all checked at **elaboration** (§5).

---

## 4. Recovery: the undo journal

The restore is an **undo journal**: one entry per allocation, holding the tag and
whether that tag had a valid generation before the allocation. A squash walks the
journal back to the checkpoint, returning each tag to the free set and stepping its
generation back down.

**One bit per entry is the whole undo state**, because the generation step is
exactly invertible:

```
allocate:  gen = prev_valid ? gen + 1 : 0,  gen_valid = 1
undo:      gen = prev_valid ? gen - 1 : 0,  gen_valid = prev_valid
```

Nothing else ever writes a generation, so `gen - 1` is the value the allocation
found, however many later allocations of the same tag came after it. The undo is
therefore exact for *any* interleaving of frees and re-allocations inside the
window, not just for the tidy case. An earlier draft stored the previous
generation explicitly; that was both wider and less obviously correct, because
correctness then depended on the stored copy agreeing with the arithmetic.

**Only allocations are journalled, and that is a proof, not an omission.** Every
free in this design is caused by a commit or is an explicit release of a mapping
nobody references. A commit is permanent — retire is in-order, so a commit landing
after a checkpoint belongs to an instruction *older* than the checkpointing
branch, and undoing it would resurrect a mapping the ISA has committed. Every
allocation after a checkpoint belongs to an instruction *younger* than it, and a
younger instruction cannot have committed: if it had, the checkpointing branch
would have committed too and there would be nothing to squash to. So the journal
is exactly the set of undoable events.

### The window is emptied by a checkpoint, and the first draft got that wrong

The first version kept a *checkpoint position* and undid back to it. That fills
the journal from reset onwards, because nothing ever drains it, and the case
failed with:

```
MISMATCH generation: cycle 475: the DUT reported journal_overflow: the undo window was
exceeded, so a squash would restore less than the truth
```

on a machine that had squashed correctly every time. The bound was being measured
from reset rather than from the checkpoint.

The fix is in the RTL: **a checkpoint starts a new undo window** rather than
marking a point inside it, and an allocation in the *same* cycle as a checkpoint is
not journalled — it is already younger than that checkpoint. There is consequently
no checkpoint *position* to save, and `j_ckpt` is gone from the design. The window's
bound is now "allocations since the last checkpoint", which is the quantity the
register-file size actually justifies, and the shadow found the matching
same-cycle rule independently (§8.3).

`journal_overflow` is a **report**, and the test asserts it never fires. A design
that silently restored less than the truth while reporting success is the failure
this whole mechanism exists to prevent.

---

## 5. Elaboration-time geometry checks

Seven preconditions are checked by `generate` branches that instantiate a module
which does not exist, so a violation is a build error rather than a mis-decode:

| Check | Why it must fail loudly |
|---|---|
| `ENTRIES % BANKS == 0` | the bank split is a partition, not a rounded approximation |
| `2^TAG_W >= ENTRIES` | the tag must be able to name every entry |
| `2^BANK_W >= BANKS`, `2^ROW_W >= ROWS` | each field must reach its range |
| `BANK_W + ROW_W == TAG_W` | bank and row must together *be* the tag |
| `ARCH_W == 5` | the 5-bit architectural address must not name a register that does not exist |
| `2^FCNT_W > ENTRIES` | the occupancy report must be able to say "full" |

A `for` loop with a `1 << WIDTH` comparison against a non-power-of-two would emit
`UNSIGNED: Comparison is constant due to unsigned arithmetic`, which is exactly the
dead logic `-Wall` exists to report; the `generate` form has no such comparison.

---

## 6. Lint — acceptance criterion 1

```
$ python3 tools/lint_rtl.py
ok   rtl/common/mosaic_fifo.sv: clean as mosaic_fifo
ok   rtl/common/mosaic_ram.sv: clean as mosaic_ram
ok   rtl/common/mosaic_skid_buffer.sv: clean as mosaic_skid_buffer
ok   rtl/core/mosaic_alu.sv: clean as mosaic_alu
ok   rtl/core/mosaic_branch_cmp.sv: clean as mosaic_branch_cmp
ok   rtl/core/mosaic_branch_target.sv: clean as mosaic_branch_target
ok   rtl/core/mosaic_bringup_core.sv: clean as mosaic_bringup_core
ok   rtl/core/mosaic_decoder.sv: clean as mosaic_decoder
ok   rtl/core/mosaic_predictor.sv: clean as mosaic_predictor
ok   rtl/core/mosaic_rename.sv: clean as mosaic_rename
lint: 13 source file(s) clean
exit=0
```

`rtl/core/mosaic_rename.sv` is `-Wall` clean with zero warnings. Two pragmas, both
narrow:

* `lint_off UNUSEDPARAM` around the generated-header `` `include ``. The generated
  file carries one localparam per project-wide configuration knob and this module
  names the rename/PRF subset; without the guard `-Wall` reports the other 20.
* `lint_off MODDUP` in the wrapper, because `mosaic_rename.sv` includes the same
  header and the generated file carries **no include guard**. The bodies are
  identical so there is nothing for the duplicate to disagree about. The
  alternative — a locally-defined guard macro — would make the *second* includer
  silently miss the package if the first were ever removed, which is a worse
  failure than the warning.

**Sizes come from the generated package and nowhere else.** The RTL has no
`parameter` for any geometry; the widths are file-scope `localparam`s reading
`mosaic_cfg_pkg::MOSAIC_*` directly, because a module's port list cannot see
declarations in its own body. There is therefore exactly one copy of the geometry
in the design, and a profile that changed it could not leave a stale default
behind.

---

## 7. The case — acceptance criterion 2

```
$ python3 tools/run_unit.py --case rename.single_width_ownership
PASS rename.single_width_ownership       task=I-013
exit=0
```

`results/unit/rename.single_width_ownership/run.log`:

```
$ /Users/flare/MosaicRV/build/p0/unit/rename.single_width_ownership/rename.single_width_ownership \
   --case rename.single_width_ownership --out /Users/flare/MosaicRV/results/unit/rename.single_width_ownership \
   --seed 1 --max-cycles 200000
RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1
```

Seeds 1, 2, 3, 7, 99 and 12345 all pass. The comparison count is identical across
seeds because every phase runs a fixed number of cycles; the seed changes *which*
requests are driven, not how long the campaign takes.

```
seed 1      PASS  seed 2      PASS  seed 3      PASS
seed 7      PASS  seed 99     PASS  seed 12345  PASS
```

---

## 8. Coverage that discriminates — acceptance criterion 3

The driver never uses the DUT as its own oracle. Every cycle it compares **every**
output against an independent shadow (`ShadowRename`), written from the prose above:
its own committed and speculative maps, its own free set as a `std::vector<bool>`
scanned in rotation order rather than a bitmap with a priority encoder, its own
generation table, its own journal as a `std::vector` walked oldest-first. It shares
no code with the RTL and never looks at Verilator internals.

It sizes itself from the elaborated DUT and **contains no geometry at all** — no
depth, no tag width, no bank count. The only geometry-derived constants in the
driver are *assertions* about the observed numbers (the free count at reset equals
`ENTRIES - ARCH_REGS`, the register file divides evenly into banks).

Comparison is three-way and happens every cycle of every phase:

1. **outputs**, pre-edge, against the shadow's prediction from the same pre-edge
   state;
2. **state**, post-edge — the free set, `gen_valid`, `wb_done`, the generation
   table and both maps;
3. **invariants**, recomputed from the DUT's own observation ports, not the
   shadow's.

### 8.1 The phases

| # | Phase | What it pins down |
|---|---|---|
| 1 | `reset-state` | arch reg i → tag i at gen 0; those tags **owned, not free**; everything else free; no generation valid; the bank split partitions the file |
| 2 | `ownership` | one tag per allocation; the displaced mapping is reported; a commit releases exactly one mapping; a **second** writeback for the same identity is refused as a duplicate; a double release is refused; the free count's *delta* is checked so a double release cannot hide |
| 3 | `generation` | the ABA case, driven to a real recycle (§8.2) |
| 4 | `wrap` | the rotation point wrapped ≥ 2 times; every allocatable tag recycled; a stale generation rejected **on every reuse**, not once at the end |
| 5 | `x0` | 24 writes to x0 allocate nothing and the free count does not move; reads return the zero identity; a commit to x0 frees nothing; **x0 still works with an empty free list** |
| 6 | `squash` | the committed map survives; the free set is restored **exactly, tag by tag**; escaped writebacks are rejected; a squash cycle refuses allocation; a squash with no checkpoint is refused and reported |
| 7 | `exhaustion` | the 64 allocatable tags are consumed; the next request is refused and reported; nothing changed; **retiring the head instruction clears it** |
| 8 | `random` | 4000 cycles of all request kinds against the shadow, with deliberate stale generations and out-of-range tags |

### 8.2 The wrap and the generation are reached, not assumed

This is the point of the whole package, so it is asserted rather than hoped for.

The **scan rotates** (`alloc_ptr` follows the last allocation and wraps to zero at
the end of the file), which is what makes a wrap reachable in a bounded number of
cycles instead of something a campaign has to run for a million cycles to find. It
also means a released tag is *not* handed straight back — it comes round again
when the scan wraps — so the `generation` phase drives a bounded campaign
(alloc/write/commit, checkpointing each step) until the tag it is holding comes
back, and **fails if it does not within one full pass over the register file**:

```
MISMATCH generation: tag 32 was not handed out again within 96 allocations: the phase
never exercised a recycled tag
```

That is the guard against a phase that ends without having tested anything.

The `wrap` phase asserts its own reach:

```
Require(pointer_wraps >= 2, ...)   // the rotation point went backwards past the end twice
Require(min_reuse >= 2, ...)       // every *allocatable* tag was handed out at least twice
```

The reuse minimum is taken over `ARCH_REGS..ENTRIES` only, because tags 0..31 are
the architectural reset mappings and can never be allocated; averaging over the
whole file would quietly dilute the evidence.

The `generation` phase then establishes, in order, that the *original* producer of a
destination is accepted, that a recycle really happened, that the generation
advanced by **exactly one** across that recycle, and only then:

```
Stim stale;  stale.wb_valid = true;  stale.wb = old;     // the OLD generation
Outputs stale_o = h->Cycle(stale);
Require(!stale_o.wb_accepted, "generation",
        "a writeback with the stale generation (32,0) was accepted after the tag was "
        "recycled to (32,1): a late result overwrote the new owner of tag 32");
Require(stale_o.wb_stale, ...);
```

followed by the current owner's writeback, which must still be accepted — so the
test proves rejection does not consume the destination, and it would fail on a
module that rejects everything. It also delivers the stale identity on the
**release** path and asserts the new owner's tag did not become free, because a
caller must not be able to free the current owner's register on the strength of a
superseded identity.

The `wrap` phase asserts the same property on **every** reuse rather than once, so
a defect that only appears on the second pass cannot hide behind the first. It also
states its own limit: a 7-bit generation distinguishes 128 allocations of one tag,
and the phase asserts it stayed below that, because past it a generation that has
wrapped cannot reject anything and the check would be claiming more than it tested.

### 8.3 The shadow earned its keep

Every defect below was found by the shadow or by a standing invariant, and every
one is in the direction of the hardware being wrong or under-specified — except
where noted.

| Found | Defect | Resolution |
|---|---|---|
| `reset-state: tag 0 is free at reset, but x0 is mapped to it: one physical register has two owners from cycle 0` | **RTL.** Free set reset to all ones while the maps own tags 0..31 | RTL §2: tags `0..ARCH_REGS-1` are owned at reset |
| `the DUT reported journal_overflow: the undo window was exceeded` on a machine that squashed correctly every time | **RTL.** A checkpoint marked a position inside a journal nothing ever drained | RTL §4: a checkpoint empties the window; `j_ckpt` deleted |
| `alloc_exhausted and alloc_squashed are both high for one request` | **RTL.** Both refusal reasons fired together, leaving the caller unable to choose a response | RTL §1: mutually exclusive, squash wins |
| `cycle 1527: the free set differs from the shadow at tag 35` | **RTL** (via `NO_FREE_RESTORE`-shaped behaviour), then **shadow**: the shadow journalled an allocation made in the same cycle as a checkpoint, which the hardware treats as already younger than that checkpoint | shadow §4, RTL §4 |
| `cycle 2088: the free set differs from the shadow at tag 52` | **shadow.** Its free-set update was a sequence of in-place edits, so two events touching one tag in a cycle resolved differently than the hardware's whole-vector next-state | shadow §8: advance the free set as one next-state vector in the documented order |
| `free_stale: expected 1, got 0` on a release of a free tag | **shadow.** It conflated "identity invalid" with "already free" into one `CurrentOwner` test, losing the distinction the caller needs — a stale release means "wrong identity", a double free means "your lifetime accounting is wrong" | shadow: separate `IdentityValid` from liveness |
| `x1 maps to tag 32, which is also in the free set` | **test.** The stimulus released a tag an architectural register still pointed at | phases retargeted onto the commit path, which is the only legal release; the invariant was right and the stimulus was wrong |
| `a duplicate writeback was refused without reporting wb_duplicate` | **test.** `Cycle` returned a reference to the harness's `observed_`, which the next call overwrote, so phases asserted against the *next* cycle's answers | `Cycle` returns by value, and `CaptureObserved` records the DUT's real outputs rather than the shadow's prediction |

That last pair deserves calling out, because they are the two ways a testbench
quietly stops testing anything. Returning a reference to a member that the next call
overwrites means a phase holding an answer across another cycle is reading
something else; and recording the *shadow's prediction* into the field the phases
read means every phase-level assertion is a tautology, since the prediction is
already compared field by field. Both are now impossible by construction, and both
were caught only because the case failed in a phase where neither should have
mattered.

### 8.4 Standing invariants, every cycle of every phase

* `free_count` equals the population count of the free mask the DUT reports — so
  the two cannot drift.
* **No tag is both free and named by a live speculative mapping.** This is
  exactly-once ownership in its sharpest form: a tag named by a mapping and also
  free would be handed to the next allocation while an instruction can still read
  it.
* No committed mapping's tag is free — a commit would have released a register the
  architecture still points at.
* No mapping names a tag outside the register file.
* A reported allocation never returns a currently-owned tag; a reported writeback
  is never accepted for a free tag.
* A refused allocation names its reason; the two reasons are never both high.
* `journal_overflow` never fires.
* The generation table agrees with the shadow **where `gen_valid` holds**, and the
  comparison is scoped that way deliberately: `gen[]` is never reset, so an
  untouched entry holds whatever the silicon powered up with and a C++ model cannot
  mirror it. What replaces the raw comparison is stronger — every *use* of a
  generation is gated on `gen_valid`, so a stale entry is not merely unread, it is
  unreachable, and the `generation` phase pins that from the consumer side with a
  writeback aimed at a never-allocated tag.

### 8.5 The C++ is held to the project's stricter standard

```
$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow \
   -Ibuild/p0/sim -Isim/common -Ibuild/p0/unit/rename.single_width_ownership/obj_dir \
   -I$(verilator -getenv VERILATOR_ROOT)/include sim/unit/tb_rename.cpp

(no output)
```

Zero warnings from this file under `-Wall -Wextra -Wshadow`, including no unused
parameter and no unused function.

---

## 9. Mutation testing — acceptance criterion 4

`MOSAIC_RENAME_MUTANT_*` blocks are **off in the shipping build**. For each one
three separate facts are recorded, because a `-D` whose `` `ifdef `` block was
never written compiles to the shipping build and "passes" vacuously — that has
happened twice in this project:

1. the `` `ifdef `` block **exists** in the RTL (grep count),
2. the mutant binary **differs** from the shipping binary (`cmp`), so the define
   changed code rather than defining nothing,
3. the run **exits 1** with a `RESULT FAIL` line.

Build and run per mutant (the runner's flags reproduced, plus `-D`):

```sh
sh /tmp/mut/run_mutants.sh MOSAIC_RENAME_MUTANT_NO_GEN_CHECK
```

which runs, per define:

```sh
verilator --cc --exe --build -j 0 -O2 --x-assign unique --x-initial unique \
  -D<DEFINE> --top-module mosaic_rename_tb -Mdir /tmp/mut/<DEFINE>/obj_dir \
  -Ibuild/p0/rtl -Ibuild/p0/sim -Irtl/core -Irtl/common \
  -CFLAGS -Isim/common -CFLAGS "-O2 -std=c++17 -Wall" \
  -o /tmp/mut/<DEFINE>/rt \
  rtl/core/mosaic_rename.sv sim/tb/mosaic_rename_tb.sv \
  sim/unit/tb_rename.cpp sim/common/sim_common.cpp
/tmp/mut/<DEFINE>/rt --case rename.single_width_ownership \
  --out /tmp/mut/<DEFINE>/out --seed 1 --max-cycles 200000
```

### Mutant table

| # | Define | Injected defect | Phase that catches it | `ifdef` | binary differs | exit |
|---|---|---|---|---|---|---|
| 1 | `NO_GEN_CHECK` | the generation is not compared: a writeback or release carrying a superseded generation is taken | `ownership` (first), `generation` | 2 | yes | **1** |
| 2 | `X0_ALLOC` | a write to x0 allocates a physical tag like any other destination | `x0` | 1 | yes | **1** |
| 3 | `NO_FREE_RESTORE` | the free set is not restored on squash; the tags stay marked allocated | `squash` | 1 | yes | **1** |
| 4 | `NO_EXHAUST_CHECK` | exhaustion is not detected: a request is taken with an empty free list | `exhaustion` | 1 | yes | **1** |
| 5 | `NO_DUP_WB_GUARD` | the "already written" guard is gone, so a second producer for one destination is accepted | `ownership` | 1 | yes | **1** |
| 6 | `NO_DOUBLE_FREE_CHECK` | releasing an already-free tag is accepted | `ownership` | 1 | yes | **1** |

### Observed output, verbatim

```
=== MOSAIC_RENAME_MUTANT_NO_GEN_CHECK
ifdef_blocks=2
binary_differs_from_shipping=yes
exit=1
MISMATCH ownership: cycle 212: free_stale: expected 1, got 0 [alloc=0:x0 rs=x0,x0 wb=0(0,0) free=1(32,1) commit=0:x0(0,0) ckpt=0 squash=0]
RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 212: free_stale: expected 1, got 0
```

The stimulus names `(32,1)` for a tag the DUT believes is at generation 0 — the
stale identity, delivered on the release path.

```
=== MOSAIC_RENAME_MUTANT_X0_ALLOC
ifdef_blocks=1
binary_differs_from_shipping=yes
exit=1
MISMATCH x0: cycle 1399: alloc_new_valid: expected 0, got 1 [alloc=1:x0 rs=x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) ckpt=0 squash=0]
RESULT FAIL rename.single_width_ownership contract violated: x0: cycle 1399: alloc_new_valid: expected 0, got 1
```

```
=== MOSAIC_RENAME_MUTANT_NO_FREE_RESTORE
ifdef_blocks=1
binary_differs_from_shipping=yes
exit=1
MISMATCH squash: cycle 1527: the free set differs from the shadow at tag 35
RESULT FAIL rename.single_width_ownership contract violated: squash: cycle 1527: the free set differs from the shadow at tag 35
```

Tag 35 is the first tag allocated after the checkpoint, so it is the first the
squash has to return. One cycle after the restore the free set is already wrong,
which is exactly the "leaks one tag per squashed instruction" failure.

```
=== MOSAIC_RENAME_MUTANT_NO_EXHAUST_CHECK
ifdef_blocks=1
binary_differs_from_shipping=yes
exit=1
MISMATCH exhaustion: cycle 1678: alloc_accepted: expected 0, got 1 [alloc=1:x20 rs=x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) ckpt=0 squash=0]
RESULT FAIL rename.single_width_ownership contract violated: exhaustion: cycle 1678: alloc_accepted: expected 0, got 1
```

The 65th allocation of the campaign was taken with an empty free list, so it was
handed a tag somebody else owns.

```
=== MOSAIC_RENAME_MUTANT_NO_DUP_WB_GUARD
ifdef_blocks=1
binary_differs_from_shipping=yes
exit=1
MISMATCH ownership: cycle 14: wb_accepted: expected 0, got 1 [alloc=0:x0 rs=x0,x0 wb=1(32,0) free=0(0,0) commit=0:x0(0,0) ckpt=0 squash=0]
RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 14: wb_accepted: expected 0, got 1
```

Cycle 14 — the second writeback for `(32,0)`, one cycle after the first was
accepted. The earliest of the six, because the duplicate guard is the first thing
the ownership phase checks.

```
=== MOSAIC_RENAME_MUTANT_NO_DOUBLE_FREE_CHECK
ifdef_blocks=1
binary_differs_from_shipping=yes
exit=1
MISMATCH ownership: cycle 171: free_accepted: expected 0, got 1 [alloc=0:x0 rs=x0,x0 wb=0(0,0) free=1(32,0) commit=0:x0(0,0) ckpt=0 squash=0]
RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 171: free_accepted: expected 0, got 1
```

---

## 10. Known limitations, stated rather than buried

* **A generation that has wrapped rejects nothing.** `GEN_W = 7` distinguishes 128
  allocations of one tag. Beyond that a stale writeback can match a current owner.
  The bound that makes this safe is a *caller* obligation and belongs to I-018:
  an instruction's writeback must reach the rename stage before its destination tag
  is re-allocated 128 times. With 64 in-flight tags that is roughly 8000
  allocations — orders of magnitude beyond any pipeline latency in this design —
  but it is an argument, not a proof, and a future design with unbounded-latency
  agents would need a wider generation or a quiesce before reuse, as
  `docs/architecture-review.md` requires.
* **The free list is a bitmap, so allocation is O(ENTRIES) combinational.** A
  96-bit rotating priority scan. Deliberate (§2), and the thing to revisit first if
  this ever becomes a timing problem — the interface does not have to change.
* **The undo journal is sized in entries of the ROB.** `journal_overflow` reports
  the bound being exceeded rather than absorbing it, so a caller that dispatches
  more younger instructions than the ROB can hold sees the report instead of a
  silently partial restore.
* **`dbg_*` ports are verification-only.** They exist so the case can compare the
  whole state rather than a projection of it. Nothing in the design consumes them,
  and they should be removed or excluded from synthesis once the module is
  integrated.
* **Two source read ports, single-width allocation.** "Single width" here is the
  *allocation* width: an instruction needs two sources, so there are two read
  ports, and there is no same-cycle bypass between them. I-014 owns the bypass and
  the all-or-none two-wide group; adding one destination per cycle here would
  change the free-set and journal accounting, not just add a port.
* **The explicit `free` port exists for a caller that knows a mapping is dead**, and
  the testbench deliberately never drives it at a *current* identity — doing so is
  a caller bug that the ownership invariant is right to reject. I-014's partial-
  group rollback and I-018's recovery are the intended users.
