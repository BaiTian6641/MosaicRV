# I-023 — readiness: a read of a never-written architectural register (CASE=`core.unwritten_reg_read`)

Work package **I-023**, readiness item. The case is registered in
`tests/unit/registry.json` (task `I-023`, top `mosaic_core_tb`, RTL list
identical to `core.corpus_branch`). This report is the wave's evidence: the
defect was reported by the control-path wave and deliberately left for a
readiness wave.

## STATUS: COMPLETE

| Acceptance item | Evidence |
| --- | --- |
| Reproduce the deadlock first, as a first-class observation | pre-fix build (same sources, initial-mapping fold neutralised, deleted build directory): `RESULT FAIL core.unwritten_reg_read contract violated: no retire within 2000 cycles: ROB head pc=0x0000000080000000 head_rd=x5 (program word 0, reads x31) retired=0 allocated=3 inserted=3 occupied=3 stopped=1 wb_wr=1 wb_wake=1 last_progress=cycle 11 \| head macro's sources: x31->(tag 31, generation 0) gen_valid=0 wb_done=0 -- the architectural initial mapping, no wakeup for it can exist \| 28 architectural register(s) are still the initial mapping`, exit 1 — see *Before* |
| The rule, stated in the module header | `rtl/core/mosaic_dispatch.sv` header ("operands"), backed by `rtl/core/mosaic_rename.sv` header ("the architectural initial mapping") — see *The rule* |
| A never-written read yields the defined value 0 | instruction 0 of the case, `add x5, x31, x31` with x31 never written, retires `x5=0x0000000000000000` — see *After* |
| R0 stays a constant zero | the recognition is `!rsN_is_x0 && !gen_valid[rsN_tag]`; x0 is excluded by construction, and `rename`'s own x0 rule (`rs*_is_x0`, "never consumes a physical tag") is untouched |
| A written register is never shadowed by the constant path | `add x7, x31, x6` must read `x6=9` through the ordinary wakeup path; `MOSAIC_DISPATCH_MUTANT_ALL_INIT_CONST` folds every source and the case fails on `x7`'s value — see *Mutants* |
| Registered case passes after the fix | `RESULT PASS core.unwritten_reg_read checks=67 cycles=15 retires=3 (x5=0x0000000000000000 at cycle 11) (x6=0x0000000000000009 at cycle 12) (x7=0x0000000000000009 at cycle 14) stopped=1 free=64`, exit 0 |
| A `-DMOSAIC_*_MUTANT_*` define reintroduces the defect and fails the case | `MOSAIC_DISPATCH_MUTANT_NO_INIT_CONST`: deleted build directory, `-D` present in that build's `build_command.txt`, binary sha256 differs from shipping, exit 1 with `no retire within 2000 cycles` as the first failure — see *Mutants* |
| `fabric.fixed_two_cluster` and `core.corpus_branch` still pass | both `RESULT PASS`, exit 0 — see *No regression* |
| `python3 tools/lint_rtl.py --profile p0` | `ok rtl/core/mosaic_core.sv`, `ok rtl/core/mosaic_dispatch.sv`, `ok rtl/core/mosaic_rename.sv`; `lint: 1 of 33 source file(s) failed` — the only failure is `rtl/core/mosaic_load_queue.sv`, the other lane's in-flight file, which this wave does not touch — see *Lint* |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl` | exit 0, **0 errors** (STYLE-2/7/13 warnings are project-wide and pre-existing: the same rules fire on every module's ports and instantiations) |
| `tests/unit/registry.json` untouched | the case is registered already (task `I-023`, top `mosaic_core_tb`, `rtl` list identical to `core.corpus_branch`, `cpp` = this driver + `memory_model.cpp` + `event_tap.cpp`) and was used unchanged; the lead's files (`config/status/implementation_status.json`, `results/PROGRESS.md`) and `tests/programs/**` were not edited. The runs rewrote `results/unit/*/result.json` and `run.log`, which is what the runner does by design |
| This report | — |

## Files

| File | Change |
| --- | --- |
| `sim/unit/tb_core_ready.cpp` | **new**: the driver — the hand-encoded program, the memory model, the architectural retirement comparison, the bounded stall observation and the readiness diagnosis that names the register and its (tag, generation) |
| `rtl/core/mosaic_dispatch.sv` | the fold: `gen_valid` input, the `!rsN_is_x0 && !gen_valid[rsN_tag]` recognition, the ready-constant capture in the dispatch queue, two `MOSAIC_DISPATCH_MUTANT_*` controls, the header rule |
| `rtl/core/mosaic_rename.sv` | header section "the architectural initial mapping" and the port-group comment: `dbg_gen_valid` is the one verification read-out the design consumes. **No logic, port or reset change** — `gen_valid` is exactly the state the rule rests on |
| `rtl/core/mosaic_core.sv` | routes rename's `dbg_gen_valid` to dispatch and to the case; three readiness probe outputs (`o_dbg_spec_map`, `o_dbg_gen_valid`, `o_dbg_wb_done`) so a stall is diagnosable |
| `sim/tb/mosaic_core_tb.sv` | the three probe outputs, flattened |
| `results/reports/I-023-readiness.md` | this file |

`rtl/core/mosaic_dispatch.sv` is instantiated only by `mosaic_core.sv`, and
`mosaic_core.sv` only by `sim/tb/mosaic_core_tb.sv` (checked with `grep -rn`), so
the two interface additions reach no other case. `mosaic_rename.sv`'s interface
is unchanged, which is why `rename.single_width_ownership` and
`rename.same_cycle_chain` still pass untouched.

## The defect

Reset maps architectural register `a` to physical tag `a` at generation 0
(`mosaic_rename.sv`), and the free list owns those 32 tags. Nothing writes them,
so the writeback arbiter's ready table never marks them written and the register
file never holds a value for them. Dispatch decided readiness only through that
table, so a source resolving to an initial mapping was inserted into the issue
queue **not ready**, to be woken by a broadcast for its `(tag, generation)`.
There is no producer for that identity, so no broadcast can come: the issue
queue waits forever, the ROB head never completes, and nothing retires —
younger instructions may complete, but in-order retire is blocked behind the
head, so the whole machine is dead.

The control-path wave found this and handed it over
(`results/reports/I-023-branches.md`, *Not covered, honestly*):

> **A read of an architectural register that has never been written since reset
> deadlocks the machine.** […] The mechanism: at reset rename maps `x_a → tag a,
> generation 0`; the writeback arbiter's ready table starts empty, so
> `rq_written[tag a]` is false, dispatch marks the source not-ready and the issue
> queue waits for a wakeup that will never come. Reading such a mapping should
> read zero. It needs the reset mappings to be marked written *and* the PRF's
> "never written" response accepted for them — and the first allocation of a tag
> also carries generation 0, so tag-and-generation alone does not identify a
> reset mapping.

The last sentence is the crux, and it rules out the sketch it proposes. Marking
the reset mappings written in the ready table is unsound *unless* the
generation seed also changes: the **first allocation** of tag `a` (after `x_a`'s
initial mapping has been released by a commit) also carries generation 0 —

```systemverilog
gen_q[scan_tag] = gen_valid[scan_tag] ? (gen[scan_tag] + 1) : 0;
```

— so a ready table that answers "written" for `(tag a, gen 0)` would answer it
for a real in-flight producer too, and dispatch would read the PRF's reset-zero
row instead of waiting for the value that producer is about to write. Fixing
that requires seeding `gen_valid` for tags 0..31 at reset, which changes state
the I-013/I-014 shadow model pins. The rule below avoids the ambiguity instead of
re-engineering around it.

## Before

Built from a deleted directory, with the shipping tree's sources except one:
`rtl/core/mosaic_dispatch.sv` replaced by a copy whose initial-mapping branch is
neutralised (`assign s1_init_fold = 1'b0;`), which compiles to exactly the
pre-fix readiness path. The working tree was never modified by this build.

```
$ python3 /tmp/before_run.py
substituting pre-fix /tmp/before/mosaic_dispatch.sv for rtl/core/mosaic_dispatch.sv
built: build/p0/before_ready/core.unwritten_reg_read sha256=b272f448204ccd04.. -> 16b6203e35b6efb0
exit=1
RESULT FAIL core.unwritten_reg_read contract violated: no retire within 2000 cycles: ROB head
pc=0x0000000080000000 head_rd=x5 (program word 0, reads x31) retired=0 allocated=3 inserted=3
occupied=3 stopped=1 wb_wr=1 wb_wake=1 last_progress=cycle 11 | head macro's sources:
x31->(tag 31, generation 0) gen_valid=0 wb_done=0 -- the architectural initial mapping, no wakeup
for it can exist | 28 architectural register(s) are still the initial mapping
```

Read the line: the head is the program's *first* instruction at
`0x80000000`, it reads `x31`, and `x31`'s mapping is `(tag 31, generation 0)`
with `gen_valid=0` (no allocation has ever taken tag 31) and `wb_done=0` (no
writeback has ever been accepted for it). The three allocated macros are all in
the ROB (`occupied=3`), one younger completion did happen (`wb_wr=1`,
`wb_wake=1` — the `addi`), dispatch has stopped at the refused ECALL, and
nothing retired: `retired=0`, and the last cycle in which *anything* moved was
cycle 11. That is the deadlock as an observation, not a hang.

The pre-fix binary and the `NO_INIT_CONST` mutant binary are **byte-identical**
(both `16b6203e35b6efb0`), which is the cross-check that the "before" run is the
defect the mutant also injects and not a different machine.

## The rule

> **A source that resolves to the architectural initial mapping reads as a
> defined zero and needs no wakeup. `mosaic_dispatch.sv` recognises it as
> `!rsN_is_x0 && !gen_valid[rsN_tag]` and folds it into the ready-constant
> operand slot, beside AUIPC's PC and the ALU's second-operand immediate.**

Why this is the exact test, and not a heuristic:

* `gen_valid[tag]` is set by an allocation and cleared only by the undo of that
  same allocation, which also rolls the speculative mapping back
  (`mosaic_rename.sv`, "why a generation exists" and the undo block). So a tag
  with `gen_valid` clear is a tag **no allocation has taken since reset**.
* Tags `0..31` are owned by the initial mappings and are never on the free list,
  so the only way a source can name such a tag is through the initial mapping
  itself. Tags `>= 32` start free, and reaching one requires an allocation,
  which sets `gen_valid`.
* The *first allocation* of a tag carries generation 0 as well, so
  `(tag, generation)` does **not** identify a reset mapping. `gen_valid` is the
  one bit that separates them, which is also why the flag cannot come from the
  writeback arbiter's ready table or from the PRF: both see a never-allocated tag
  and the first allocation of that tag identically, and both must keep seeing
  them identically so that a real producer is still waited for.

Why the architectural value is zero: `mosaic_bringup_core.sv` already models an
architectural register file that reads 0 before any write, the corpus and
`tools/host_oracle.py` assume zero-initialised registers, and the control-path
wave's report states it ("Reading such a mapping should read zero").

Two clauses that are part of the rule rather than consequences of it:

* **R0.** x0 is reported by `rs*_is_x0` and the recognition excludes it. x0
  keeps reading zero without a physical register, writing it still allocates
  nothing, and the two zero rules stay two statements about two different
  things. The fold is captured into the same queue slot the AUIPC PC uses, and
  the two are mutually exclusive (AUIPC uses no `rs1`, so its `rs1_addr` is x0).
* **A written register is never shadowed.** A register with a real producer has
  a mapping that came from an allocation, so its tag's `gen_valid` is set and it
  takes the ordinary ready/wakeup path. The case checks this behaviourally (see
  *After*) and the `ALL_INIT_CONST` mutant breaks it.

The fold does **not** rely on the PRF's power-up contents. The I-015 card's fail
criterion names the opposite shape of mistake — "a simulation-only read of
initialised zero masks an undefined read" — and this change never reads a
never-written PRF row: it supplies a defined architectural constant at dispatch
and leaves the PRF's `rsp_never_written_o` refusal in place as the guard against
a ready table and a register file that disagree.

## What it changes about the machine

* **A never-written register costs no wakeup slot.** The operand is inserted
  **ready**: `rq_valid` is low for it (no ready-table query), `sN_needs_read` is
  false (no PRF read port, no read-bank conflict, no `hold_val` retry), it can
  never be made ready by a broadcast it waits for, and it never appears as a
  wakeup dependency (`o_wu_*`). It still occupies its operand slot in the issue
  queue entry — every uop has two fixed slots, and this case's uop still has two
  sources — so the *entry* cost is unchanged; what disappears is the
  unresolvable dependency.
* **The constant path does not widen.** `disp_ent_t.s1_const/s1_cval` and
  `s2_const/s2_cval` already existed (AUIPC's PC, the second-operand immediate)
  and the queue entry's layout is unchanged. The only new logic is two
  per-source tests at capture time (`!rsN_is_x0`, a `gen_valid` bit select) and
  an OR into the existing constant select.
* **No new state, no reset cost.** No register, counter or FSM was added
  anywhere. The 96-bit `gen_valid` read-out is a wire from rename into dispatch;
  rename's `gen_valid`, the PRF, the free list, the ready table and the
  generation semantics are unchanged, and no reset flop was added.
* **No latency change.** The recognition is combinational at the cycle the
  macro is allocated, the same cycle in which the source tag is already read out
  of the speculative map.
* **The architectural effect** is exactly: a read of a register the program has
  not written now yields 0 deterministically and lets the machine proceed; x0 is
  unchanged; a written register is unchanged.
* The *conservative* half of the old behaviour is kept: the ready table still
  answers "not written" for a never-allocated tag, so if a consumer ever did
  consult it for such a source, it would still wait. The fold is what removes
  the wait; nothing was made to *look* ready that is not.

## The case

`sim/unit/tb_core_ready.cpp`. The program is hand-encoded into
`mosaic::MemoryModel` at the profile's reset vector; nothing under
`tests/programs/` is touched. Assembled with

```
riscv64-elf-as -march=rv64im -o ready.o - <<'EOF'
  add  x5, x31, x31
  addi x6, x0, 9
  add  x7, x31, x6
  ecall
EOF
```

| pc | word | instruction | why it is there |
| --- | --- | --- | --- |
| `0x80000000` | `0x01ff82b3` | `add x5, x31, x31` | the smallest reproducer: the *first* instruction reads `x31`, which the program never writes |
| `0x80000004` | `0x00900313` | `addi x6, x0, 9` | a producer whose value is not zero, so the ordinary wakeup path is exercised too |
| `0x80000008` | `0x006f83b3` | `add x7, x31, x6` | reads the folded `x31` **and** the written `x6`; `x7 = 0 + 9 = 9` fails if a written register is shadowed by the constant |
| `0x8000000c` | `0x00000073` | `ecall` | refused by dispatch, which stops the machine cleanly |

Every word past the program is an ECALL, so a fetch past the end stops the
machine as a refused macro rather than as an illegal decode.

The case checks, with `sim/common/event_tap.cpp`'s canonical event stream as the
expectation:

1. three instructions retire, in program order, at the right PCs, each writing
   the right register with the ISA's value (`x5=0`, `x6=9`, `x7=9`), with
   strictly increasing sequence numbers and no trap and no store;
2. the canonical event stream (`Save` of the expectation, `Compare` of the run)
   is identical;
3. the ECALL is refused rather than executed (`o_unsupported_ctr >= 1`,
   `o_illegal_ctr == 0`), the machine is stopped, the ROB is empty at a rename
   boundary, there is no squash and no journal overflow, and every fetch and
   access stayed inside the memory map;
4. the program really did read a never-written register and never wrote it: at
   the end of the run `x31`'s mapping is still `(tag 31, generation 0)` with
   `gen_valid[31] == 0` and `wb_done[31] == 0`, read straight off the DUT's
   rename state through `o_dbg_spec_map` / `o_dbg_gen_valid` / `o_dbg_wb_done`;
5. the registers the program wrote have mappings from an allocation
   (`gen_valid` set for `x5`, `x6`, `x7`), so the constant path did not shadow
   them, and the writeback path really carried a completion and a wakeup
   (`o_wb_wr_ctr >= 3`, `o_wb_wake_ctr >= 1`);
6. the free set is back to its reset population of 64: the three initial
   mappings the program superseded were released and three real tags are owned;
7. the machine is stopped by the *program's* ECALL, not by anything else.

The observation window is bounded at 2000 cycles (the registry's `max_cycles` is
4,000,000; a correct run needs 15). A stall is reported at cycle 2000 by name,
and it classifies *every* source of the ROB-head macro — including saying so
when a stall is not this defect — and counts how many architectural registers
are still their initial mapping.

## After

```
$ python3 tools/run_unit.py --case core.unwritten_reg_read
PASS core.unwritten_reg_read      task=I-023

RESULT PASS core.unwritten_reg_read checks=67 cycles=15 retires=3
(x5=0x0000000000000000 at cycle 11) (x6=0x0000000000000009 at cycle 12)
(x7=0x0000000000000009 at cycle 14) stopped=1 free=64
```

The retirement stream, as recorded by the driver
(`results/unit/core.unwritten_reg_read/expected_retire.events`):

```
hart=0 seq=0000000000000000 pc=0000000080000000 next_pc=0000000000000000 insn=00000000 rd=x5 val=0000000000000000
hart=0 seq=0000000000000001 pc=0000000080000004 next_pc=0000000000000000 insn=00000000 rd=x6 val=0000000000000009
hart=0 seq=0000000000000002 pc=0000000080000008 next_pc=0000000000000000 insn=00000000 rd=x7 val=0000000000000009
```

Instruction 0 retires with value 0 having read `x31` — the read the control-path
wave's report says "should read zero" — and instruction 2 reads the written `x6`
as 9, so the constant path did not shadow a real producer.

## Mutants

Both controls were built from a **deleted** build directory (with the campaign's
own driver, modelled on `tools/run_core_controls.py`, which stamps the command
it ran into that build's `build_command.txt`), and each binary was compared with
`sha256` against the shipping one:

| define | injects | `-D` in `build_command.txt` | sha256 (16) | shipping | exit | first failure |
| --- | --- | --- | --- | --- | --- | --- |
| `MOSAIC_DISPATCH_MUTANT_NO_INIT_CONST` | the fold is removed: the initial mapping is inserted not-ready again — **the pre-fix defect** | yes | `16b6203e35b6efb0` | `216f9203286c5b32` | 1 | `no retire within 2000 cycles: ROB head pc=0x0000000080000000 head_rd=x5 (program word 0, reads x31) retired=0 allocated=3 inserted=3 occupied=3 stopped=1 wb_wr=1 wb_wake=1 last_progress=cycle 11 -- head macro's sources: x31->(tag 31, generation 0) gen_valid=0 wb_done=0 -- the architectural initial mapping, no wakeup for it can exist` |
| `MOSAIC_DISPATCH_MUTANT_ALL_INIT_CONST` | the fold is applied to every source: a written register is shadowed by the constant zero | yes | `ba332234bebc8fc1` | `216f9203286c5b32` | 1 | `retire 2 carries the ISA's value: x7=0x0000000000000000 expected 0x0000000000000009` |

The shipping binary of the campaign is the same file the registered runner
produces (`sha256 = 216f9203286c5b32...`), and the `NO_INIT_CONST` binary is the
same as the pre-fix build above — so the first mutant is not a look-alike of the
defect, it *is* the defect, and the second mutant is the guard for the other half
of the contract (a written register must not be shadowed).

## No regression

Every build directory below was deleted before the run, so each binary was
compiled against the sources as this wave leaves them (not a stale object file):

```
$ rm -rf build/p0/unit/{core.unwritten_reg_read,core.corpus_branch,fabric.fixed_two_cluster,core.bringup_vs_reference,rename.single_width_ownership,rename.same_cycle_chain}
$ python3 tools/run_unit.py --case core.unwritten_reg_read --case core.corpus_branch \
      --case fabric.fixed_two_cluster --case core.bringup_vs_reference \
      --case rename.single_width_ownership --case rename.same_cycle_chain
PASS core.unwritten_reg_read      RESULT PASS core.unwritten_reg_read checks=67 cycles=15 retires=3 …
PASS core.corpus_branch           RESULT PASS core.corpus_branch checks=28 comparisons=8907 cycles=1783 retires=256 control=96 taken=74 branches=48(taken 26) jal=27 jalr=21 back_edges=35 inputs=3 seed=1
PASS fabric.fixed_two_cluster     RESULT PASS fabric.fixed_two_cluster checks=59 comparisons=678 cycles=129 retires=17 c0_alu=8 c1_alu=8 muldiv=1 dual_grant_cycles=0 max_occupied=6 ooo(index 13 completed at 49, index 12 at 114) seed=1
PASS core.bringup_vs_reference    RESULT PASS core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical
PASS rename.single_width_ownership RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, …
PASS rename.same_cycle_chain      RESULT PASS rename.same_cycle_chain rename contract holds: 9767 shadow comparisons over 9843 cycles, …
```

`core.corpus_branch` and `fabric.fixed_two_cluster` are the two the ticket
requires (the fix is on the readiness path every program uses). The two rename
unit cases are included because this wave edits `mosaic_rename.sv`
(documentation only): their shadow models still match cycle by cycle, which is
the check that the interface and the state really are unchanged.

## Lint

```
$ python3 tools/lint_rtl.py --profile p0
ok   rtl/core/mosaic_core.sv: clean as mosaic_core
ok   rtl/core/mosaic_dispatch.sv: clean as mosaic_dispatch
ok   rtl/core/mosaic_rename.sv: clean as mosaic_rename
FAIL rtl/core/mosaic_load_queue.sv: lint failed        <-- another lane's in-flight file
lint: 1 of 33 source file(s) failed
```

`mosaic_load_queue.sv` fails on two `UNUSEDSIGNAL` warnings in that file
(`rsp_i`, `sel_off_c`), which this wave neither touches nor owns, and the ticket
says to leave it alone. `slang-tidy --std 1800-2017 --single-unit -I
build/p0/rtl $(find rtl -name '*.sv' | sort)` exits 0 with no errors; its 1456
STYLE-2/STYLE-7/STYLE-13 warnings are project-wide and pre-existing (the ports
and instances this change adds are flagged by the same rules that flag every
existing port in the tree).

## Commands

```sh
# the case, through the official runner (shipping tree)
python3 tools/run_unit.py --case core.unwritten_reg_read

# the "before" run: the same sources with the initial-mapping fold neutralised,
# built from a deleted directory into build/p0/before_ready (tree untouched)
python3 /tmp/before_run.py

# the two negative controls, each from a deleted directory, with the -D stamped
python3 /tmp/ready_controls.py

# no regression
python3 tools/run_unit.py --case core.corpus_branch --case fabric.fixed_two_cluster \
    --case core.bringup_vs_reference --case rename.single_width_ownership \
    --case rename.same_cycle_chain

# gates
python3 tools/lint_rtl.py --profile p0
slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)
```

`/tmp/before_run.py` and `/tmp/ready_controls.py` are throwaway campaign drivers
in this wave's scratch space, not repository tooling: the repository's
`tools/run_core_controls.py` has a fixed mutant list for
`fabric.fixed_two_cluster`, and `tools/` is outside this wave's edit scope. The
exact command line of every build quoted above is recorded beside its binary —
`build/p0/unit/core.unwritten_reg_read/build_command.txt` (shipping, by the
registered runner), `build/p0/ready_controls/<define>/build_command.txt` (each
mutant, with its `-D`) and `build/p0/before_ready/build_command.txt` (the
pre-fix build, showing the substitution).

## Not covered, honestly

* **The two-wide lane-1 source path.** The integrated core is one-wide (the ROB
  has a single allocation port) and ties rename's lane-1 read ports off, so the
  initial mapping is recognised only on lane 0's sources. The state the rule
  needs (`gen_valid`) is per tag and is already there for a two-wide dispatch;
  no consumer exists yet, so nothing checks it. The two-wide *bypass* case
  (`rename.same_cycle_chain`, I-014) is untouched by this change and still
  passes, but it does not exercise this rule.
* **The operand collector's retry/latency behaviour (I-027).** Not exercised
  here, and this case cannot exercise it: its operands are the folded constant
  (no read), x0 (no read), an immediate (no read), and one single-cycle ALU
  producer. Dispatch's bank-conflict retry (`src_conflict` / `hold_val`), its
  two-cycle operand fetch and its `src_bad` refusals all need a multi-cycle or
  conflicting read that this program does not create — `add x7, x31, x6` has
  `bank_of_src = tag 31 % 4 = 3` and `tag 33 % 4 = 1`, so the two sources do not
  even share a bank. If anything, the fold *removes* one PRF read, so it cannot
  introduce a new retry path. I-027's own case is the place for that; nothing
  here is a substitute for it.
* **The `(tag, generation)` ambiguity as a defect in its own right.** The rule
  side-steps the fact that a tag's first allocation carries generation 0, but
  nothing checks that the ready table and the PRF keep answering "never written"
  for a never-allocated tag. The `ALL_INIT_CONST` mutant is only an indirect
  guard: it shows that folding a *written* register breaks the case, not that the
  ready table's answer for an unwritten tag is right. That belongs to the
  writeback/PRF cases (I-015, I-026).
* **rename-level verification of the discriminator.** The criterion used to be
  enforced inside rename; it now lives in dispatch and is checked only through
  the core case. `rename`'s unit tests do compare `dbg_gen_valid` against their
  shadow model every cycle, so the *state* is verified, but if the recognition
  were reimplemented inside rename the rename unit tests would not catch a wrong
  version of it. The core case and its two mutants are the guard.
* **A program that writes the register on only one path.** This case's program
  never writes `x31` at all. A register that is *sometimes* written (one arm of
  a branch) is a different stimulus: the fold decision at dispatch is the same,
  but the branch path itself is `core.corpus_branch`'s, not this case's.
* **A RAW chain from a folded producer.** Instruction 0 writes `x5` (whose
  operands are both folded), and instruction 2 reads `x6`, not `x5`; `x5`'s
  retirement proves the folded producer completed and wrote back, but no
  instruction consumes `x5`'s value. The generator/`gen_valid` assertions on
  `x5`, `x6`, `x7` are what stand in for that.
* **Loads, stores, CSRs, traps and the flow past the ECALL.** Not in this
  package: dispatch refuses them and the machine stops, which is why the program
  ends in an ECALL. Nothing here is a substitute for the memory path
  (I-033..I-038) or the trap path (I-019/I-020).
* **A bounded window instead of a progress monitor.** The case calls a stall at
  2000 cycles; it cannot distinguish "deadlocked" from "slower than 2000
  cycles". A correct run needs 15, so the margin is ~130x, and the stall report
  includes the last cycle in which anything moved.
* **Timing, area, synthesis.** No claim.
