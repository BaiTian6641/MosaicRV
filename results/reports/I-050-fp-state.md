# I-050 — FP state, precise `fflags` and boxing: implementation report (INCOMPLETE)

**Status: NOT DELIVERED.** The case `fp.precise_flags_and_boxing` does not pass; the driver
`sim/unit/tb_core_fp.cpp` does not exist yet. This report records exactly what was built, what
is verified, and what remains, so the next owner does not repeat the work or trust an
unverified claim.

---

## 1. What was implemented (RTL, lint-clean)

### 1.1 FP register file — decision: widen the existing PRF's *namespace*

`mosaic_prf.sv` is 64 bits wide and holds raw bits, so no second bank is instantiated. Instead
`mosaic_rename.sv` gains a **second architectural map family** (`spec_map_fp`/`spec_gen_fp`,
`cmt_map_fp`/`cmt_gen_fp`) sharing the same physical tag space, free list, generation table and
undo journal. `f0..f31` are a distinct namespace: they are mapped, committed and freed
independently of `x0..x31`, so a physical tag is never named by both an integer and an FP
mapping at once. A destination is FP or integer per instruction, so the allocator still hands
out at most one tag per macro.

The reset encoding of "this f-register has never been written" is **(tag 0, gen 0)** — the
initial mapping of `x0`, which is never superseded (x0 commits are dropped) and never in the
free set. A source resolving to it is folded to a ready zero by dispatch's existing
`!gen_valid[tag]` rule, so an unwritten f-register reads as zero **without pre-committing 32
extra tags**. This matters: pre-committing `f0..f31` to tags 32..63 would drop the allocatable
pool from 64 to 32 and violate `tools/mosaic/config_check.py`'s
`int_prf.entries - arch_int_regs >= rob.entries` rule. Every FP-specific path in `mosaic_rename`
is a no-op when its namespace input is low, and the new inputs carry port defaults, so the
integer-only design is bit-identical.

### 1.2 NaN-boxing (core side)

`mosaic_fp_unit.sv` (new) applies the two register-file rules I-049 deliberately left to its
consumer:

* a single-precision FP **operand** whose upper 32 bits are not all ones is treated as the
  canonical quiet NaN (`0x7FC00000` in the low half);
* a single-precision FP **result** is written NaN-boxed (upper 32 bits set to all ones).

`flw` is boxed in the core at the load completion (`fp_load_box_mem`), because the load path
zero-extends a word and the upper half must be set.

### 1.3 `fcsr`/`frm`/`fflags` and `mstatus.FS`

* `config/csr/mode_m.json` gains generated `fflags` (0x001), `frm` (0x002) and `fcsr` (0x003)
  rows with a new `fp-state` role (`tools/mosaic/config_check.py`); `config/csr/rule_ledger.json`
  gains three rules. `python3 tools/check_profile.py --all` passes; `make manifest` regenerates.
* `mosaic_csr.sv` stores one 8-bit `fcsr_q = {frm[7:5], fflags[4:0]}`; the three addresses are
  views of it (field masks from the generated table, not literals).
* `mstatus.FS` is set to Dirty (bits 14:13 = 11) on `fp_fs_dirty_i`, driven by the core from a
  per-ROB-slot record of "this instruction modifies FP state". It is a set, never a clear.

### 1.4 Precise `fflags` — rule chosen: per-uop flag sideband merged at retire

`mosaic_fp_unit` produces the operation's five flags with its speculative completion and never
writes `fcsr`. The core records them against the ROB slot when the writeback path takes the
completion (`fp_flag_mem[slot]`, `fp_flag_gen_mem[slot]`, `fp_flag_v_mem[slot]`), clears the
slot's record at allocation, and clears **all** records on every redirect or trap
(`rob_flush_pulse`). At retire, a slot's flags are merged into `fcsr` only if the slot retires
with the generation its record was written for (`fp_merge0_v`/`fp_merge1_v`). A squashed FP
operation's record is cleared by the flush and its slot never becomes the head, so its flags
never reach the architectural `fflags`. The core orders the merge against a same-cycle
`csrw fcsr`/`fflags` by ORing only the lanes younger than that write.

### 1.5 Decode / dispatch / execution path

* OP-FP (1010011), OP-FP-LOAD (0000111) and OP-FP-STORE (0100111) are recognised in
  `mosaic_core.sv` section 2a (the WFI/SRET/SFENCE/AMO pattern), so
  `CASE=decode.rv64im_reserved` is untouched. All arithmetic/min/max/compare/classify/move/
  convert/sign-injection forms are decoded; reserved funct7/funct3/rs2 combinations stay illegal.
* `uop_class_e.UOP_FP = 3'd6`; dispatch classifies an OP-FP macro to it and routes it to cluster
  0 (which is the queue whose grant feeds the shared FP unit), exactly as UOP_MULDIV feeds the
  shared MUL/DIV unit. `uop_meta_t` and `decode_ctl_t` gain `fp_op/fp_fmt/fp_rm/fp_dst_fp/
  fp_src1_fp/fp_src2_fp/fp_iw/fp_is` (and `fp_modifies_state`).
* `mosaic_cluster.sv` gains an FP request port mirroring the MUL/DIV one; cluster 1 ties it off.
* `mosaic_fp_unit.sv` drives `mosaic_fpu`, resolves a dynamic rm from `fcsr.frm`, and returns an
  ordinary `wb_event_t` completion. It shares writeback arbiter **port 2** with the MUL/DIV unit
  (FP wins, MUL/DIV holds), so `mosaic_wb_arbiter.sv` is unchanged.
* New core/wrapper observation ports: `o_csr_fcsr/fflags/frm`, `o_fp_issue_ctr`,
  `o_fp_commit_ctr`, `o_fp_flags_ctr`, `o_fp_merge_ctr`.

## 2. Verification actually run

| Command | Result |
|---|---|
| `python3 tools/lint_rtl.py --profile p0` | **clean** (all 47 RTL sources) |
| `python3 tools/check_profile.py --all` | **pass** (all four profiles) |
| `python3 tools/gen_manifest.py --profile p0` | regenerates with the three FP CSRs |
| `python3 tools/run_unit.py --case core.unwritten_reg_read` | **PASS** (FP namespace inputs default low ⇒ integer design unchanged) |
| `python3 tools/run_unit.py --case csr.precise_trap_mret` | **PASS** (after updating `tb_csr.cpp`'s 50-row expectation to 53) |
| `python3 tools/run_unit.py --case csr.rule_ledger` | **FAIL** — see §3 |

Files changed: `rtl/core/{mosaic_pkg,mosaic_uop_pkg,mosaic_decoder,mosaic_rename,mosaic_core,
mosaic_dispatch,mosaic_cluster,mosaic_csr}.sv`, new `rtl/core/mosaic_fp_unit.sv`,
`config/csr/{mode_m,rule_ledger}.json`, `tools/mosaic/config_check.py`,
`sim/tb/{mosaic_core_tb,mosaic_csr_tb,mosaic_rename_tb,mosaic_retire_tb}.sv`,
`sim/unit/tb_csr.cpp`, `tests/unit/registry.json` (added `mosaic_fpu.sv`/`mosaic_fp_unit.sv` to
the 24 core-top cases and dropped `pending` from the FP case).

## 3. Blocker: `csr.rule_ledger` fails on the new `fflags.value` rule

```
MISMATCH csr-rule-ledger at cycle 3459: fflags.value positive:
  the read-back: read 0x0000000000000000, want 0x000000000000001f
```

The generated stimulus is `csrrw x0, 0x001, x5` (x5 = 0x1f) then `csrr x7, 0x001`; the read
returns 0. The standalone CSR case passes and the `mstatus.FS` rule (same mechanism, address
0x300) passes, so the difference is specific to the new 0x001/0x002/0x003 rows and was **not
root-caused before the budget was exhausted**. Prime suspects: the write case arm not being
reached, or `csr_wdata_i` not carrying the write operand for a write-only `csrrw`, or the read
mux being overwritten later in `mosaic_csr.sv`. This must be fixed before anything else.

## 4. Not covered / remaining work

* **`sim/unit/tb_core_fp.cpp` is not written.** The case cannot pass. It must hand-encode an
  RV64 F/D program (the corpus audit forbids F/D in `tests/programs`), use the host FPU through
  `<cfenv>`/`fetestexcept` as the arithmetic+flags oracle in the style of `tb_fpu.cpp`, and use
  bit-level ISA models for boxing/NaN. It must cover: a wrong-path FP operation whose flags do
  not appear; a rounding-mode tie; sNaN/qNaN result bits and NV; a subnormal result; signed
  zero; conversion overflow and its NV; `fmv.w.x` of an unboxed value; an FP load/store through
  the memory path; and `mstatus.FS` dirtying (dirty after an FP write, not after a read).
* **No mutants** were built (`MOSAIC_CORE_MUTANT_FFLAGS_EARLY`, `..._FP_SQUASH_WRITES`,
  `..._FP_NO_UNBOX`, `..._FS_NO_DIRTY`) — the `-D` sites are not yet written.
* The rename/retire testbench port-connection edits are **unverified** (not run).
* `fsqrt` and the fmadd family remain absent (I-049 declared them); FP is not in the ACT4 set
  (the p1 ACT config excludes F/D), so no external suite covers this yet.
* `frm` is not renamed: a dynamically-rounded FP operation younger than a pending `frm` write
  may use the old mode (documented limitation).

---

# I-050 attempt 2 (FpStateRetry): the red check fixed, two real RTL defects found, the FP case still not delivered

**Status: NOT DELIVERED.** `csr.rule_ledger` is green again and two genuine RTL
defects in the FP integration are fixed, but CASE=`fp.precise_flags_and_boxing`
does **not** pass: 105 of its 106 checks pass and the one that fails is the
phase's own anti-vacuity check, so the case correctly reports FAIL rather than
passing without evidence. The four mutants are **not written** and the eleven
cases are **not re-run** (see "Not done" below).

## 1. The red check: root cause was the *stimulus*, and behind it an RTL defect

`csr.rule_ledger` failed at `fflags.value positive` because the case's stimulus
addressed its operand and read-back slots with **signed 12-bit displacements that
overflowed**. `sim/unit/tb_core_csr_rules.cpp` emitted `ld x5, 8*index(x31)` and
`sd x7, 8*slot(x30)`; with 172 examples the indices reach 343, so `8*343 = 2744`
does not fit `[-2048, 2047]`. The encoder masks the field to 12 bits and the
*decoder* sign-extends it, so the fflags example loaded its operand from
`0x80007a60` (unwritten RAM, hence zero) instead of `0x80008a60`, and stored the
read-back to `0x8000fa60` while the harness read `0x80010a60` -- a stimulus that
examined nothing while reporting a mismatch against a correct ledger rule. The
ledger rule itself (`fflags` RW at 0x001, write 0x1f reads back 0x1f) is correct
per the F extension, and the machine was correct too.

Fixes in `sim/unit/tb_core_csr_rules.cpp`:

* each pool is now **two windows of 256 entries** (`kLiteralBase`/`kLiteralBase2`,
  `kSlotBase`/`kSlotBase2`, one page apart) with a second base register per pool
  (x29, x27), so every displacement is `8*(index % 256) <= 2040` by construction;
  `LitAddr`/`SlotAddr` and the instruction addressing are derived from the same
  window rule, so the pool the DUT writes is the pool the harness reads;
* a permanent guard in the assembler (`Asm::CheckDisp`) makes a future pool that
  outgrows the window a *build-time failure of the stimulus*, not a silent read
  of zero. This is the class of defect the previous attempt could not see.

With that fixed the mismatch moved to `frm.value`, which exposed a **real RTL
defect**: the `frm` write arm in `rtl/core/mosaic_csr.sv` took the operand's bits
`[7:5]` -- where `frm` sits *inside `fcsr`* -- instead of bits `[2:0]`, where the
`frm` CSR's own three bits are. `csrw frm, x5` therefore multiplied by zero and
silently did nothing. Fixed; both CSR cases pass.

Note what the stimulus fix also revealed: the `hpmcounter*.value` rules
(index >= 64, i.e. example index >= 128) had been *passing vacuously*, because
their expected read is 0 and the overflowing store meant the harness read a slot
the DUT never wrote -- also 0. They are real comparisons now.

## 2. A second RTL defect: the second FP operand was read from the integer map

Writing the FP driver and driving a two-operand FP instruction showed that
`fadd.s f3, f1, f2` computed `f1 + x2`: the *second source of every OP-FP form
that has one was read from the integer register file*. In
`rtl/core/mosaic_core.sv` the OP-FP decode arm sets `fp_s1_is_fp_c = 1'b1` but
never sets `fp_s2_is_fp_c`, so it stayed at its default 0 and dispatch resolved
`rs2` through the integer map (`rs2_is_fp = uses_rs2 && fp_src2_fp`). In a
program that never touches x2 the second operand is zero, which is how it hid:
any single-source FP form, and any pair whose integer registers happened to hold
the right bits, looked correct. Fixed with a single statement
(`fp_s2_is_fp_c = fp_uses_rs2_c` after the funct7 case), with the evidence in the
comment. Every OP-FP form that uses rs2 names an f-register, and the forms whose
rs2 is a *field* (the conversions, the moves, fclass) set `fp_uses_rs2_c` low and
present an unused source as address x0, so the assignment is exact.

Evidence that this is a fix and not a coincidence: with it, `f1+f2 = 3.0`,
`f2+f1 = 3.0`, `f1-f2 = -1.0`, `f1+f1 = 2.0`, `f2+f2 = 4.0`, and the same holds
when the operands come from loads instead of from the FP unit; before it, the
second operand read as zero in every one of those cases except the pair whose
integer registers held the right bits. `core.unwritten_reg_read` and
`core.mem_program` still pass, and no p0 case executes an FP instruction (the
corpus audit forbids F/D in `tests/programs`), so the change cannot affect them.

## 3. `sim/unit/tb_core_fp.cpp` -- written, and where it stands

The driver exists (about 1400 lines) and drives the real `mosaic_core_tb` with
hand-encoded RV64 F/D programs. Seven phases, each a fresh reset and a fresh
program:

| phase | what it drives | oracle |
|---|---|---|
| `arith` | 19 vectors: add/sub/mul/div, both formats, RNE/RTZ/RDN/RUP, ties, overflow, subnormal, signed zero, sNaN/qNaN, `inf + -inf` | the HOST's FPU through `fesetround`/`fetestexcept`; a NaN result is compared against the canonical NaN, which the F extension permits and this design documents |
| `box-nan` | `fmv.w.x` boxing, an unboxed operand, `flw` boxing, `fsw`/`fsd`/`fld` bits, `fsgnj.d` of -0.0, sNaN vs qNaN (bits and NV) | bit-level models written from the F extension, plus the ISA's NV rules; the NV half agrees with the host |
| `cvt` | `fcvt.w.s`/`fcvt.wu.s` saturation and NV, a negative value that rounds to zero (no NV), `fcvt.w.s` of 1.5 at RTZ (NX), `fcvt.s.w` of 16777217 | the ISA's saturation rule for the out-of-range vectors (C has no defined cast there); the in-range int->fp vector is the host's conversion and `fetestexcept` |
| `mem` | `flw`/`fsw`/`fld`/`fsd` round trips, a double add from memory to memory | bit-level models |
| `wrong-path` | a mispredicted taken branch whose fall-through is `fmul.s f5` = FLT_MAX*2 (+inf, OF\|NX); the branch is gated on a long-latency integer divide so the squashed operation has time to execute | the machine's own architectural `fflags`, made non-circular by an **anti-vacuity check**: the FP unit must report that the operation completed and produced its flags. f5 must still read as the never-written register, and the stale-writeback counter must not move |
| `fs-dirty` | `mstatus.FS` after an FP read (`fmv.x.w`), an FP write (`fadd.s`), an FP store (`fsw`) | the privileged spec's FS state machine, read back with `csrr mstatus` |
| `frm` | a tie resolved by the instruction's own rm field in all four host modes, then `frm=111` resolved dynamically, with the FP operation's operands *indexed by the value the CSR read back* so the operation is ordered behind the write | the host's FPU in each mode |

The failing check is the `wrong-path` anti-vacuity one: with the current program
the squashed multiply does not complete before the redirect (0 flagged
completions), so the phase proves nothing and the case fails. This is the honest
outcome -- the check exists precisely so that a vacuous phase cannot pass -- and
it is the first thing the next owner should finish. The rest of the wrong-path
checks (no fflags, no destination, no stale writeback) pass, and the other six
phases pass in full.

A defect in this driver was found and fixed while doing this: `Reporter::Check`
records a failure without stopping the run and the verdict was the local flag
alone, so the case printed `RESULT PASS` while carrying failed checks. `main()`
now ANDs `reporter.failures() == 0` into the verdict, which is why this case
reports FAIL today instead of a silent pass.

Also of interest, and documented rather than "fixed": an f-register that has
never been written reads as architectural zero, and a *single-precision* operand
whose upper half is not all ones is the canonical quiet NaN by the design's own
unboxing rule -- so an unwritten f-register used as an `fadd.s` operand is the
canonical NaN, not zero. The driver asserts that (it is the value the
wrong-path phase's destination check compares against), and the wrong-path
operation's result is deliberately +inf so that "the squashed operation wrote its
destination" is distinguishable from "the destination was never written".

## 4. Cases actually re-run

| case | profile | result |
|---|---|---|
| `csr.rule_ledger` | p0 | **PASS** (green again) |
| `csr.precise_trap_mret` | p0 | **PASS** |
| `core.unwritten_reg_read` | p0 | **PASS** (smoke test of the FP rename namespace) |
| `core.mem_program` | p0 | **PASS** (smoke test after the RTL fixes) |
| `fp.precise_flags_and_boxing` | p0 | **FAIL** -- 105/106 checks; the anti-vacuity check of the `wrong-path` phase |

Both profiles' manifests were regenerated (`gen_manifest.py --profile p0` and
`p1`) before these runs.

## 5. Not done (this attempt ran out of budget)

* **The wrong-path anti-vacuity failure.** The squashed operation must be made to
  execute before the redirect. The current program gates the mispredicted branch
  on an integer divide (a long, non-FP delay) and the fall-through multiply still
  does not complete; the next thing to check is whether a younger FP macro is
  issued at all behind an unresolved branch, and if not, what does gate it.
* **The four mutants** (`MOSAIC_CORE_MUTANT_FFLAGS_EARLY`,
  `MOSAIC_CORE_MUTANT_FP_SQUASH_WRITES`, `MOSAIC_CORE_MUTANT_FP_NO_UNBOX`,
  `MOSAIC_CORE_MUTANT_FS_NO_DIRTY`) are not written, and neither is
  `tools/run_fp_controls.py`. The `-D` sites do not exist yet. None of the four
  is therefore evidence.
* **`core.act_dut` (p1) was not re-run.** The previous attempt also left it
  unverified and the assignment requires it to stay 127/127; it is the strongest
  evidence in the tree and it is still missing. The RTL changes in this attempt
  touch only the FP decode path (`fp_s2_is_fp_c`), the `frm` CSR write arm and
  the CSR rule ledger's stimulus, and the p1 ACT4 configuration excludes F/D, so
  the risk is low -- but "low risk" is not a run.
* Also not re-run: `core.corpus_sweep`, `privilege.permission_matrix`,
  `fp.operation_matrix`, `trap.precise_state`, `compressed.cross_boundary`,
  `sv39.walk_and_faults`.
* **No section of this report is re-run evidence for the FP case.** The seven
  phases' results above are from a single run of the shipping build.

## 6. Still not covered (unchanged from attempt 1, restated)

* `fsqrt` and the fmadd family remain absent (I-049 declared them).
* `frm` is not renamed: a dynamically-rounded operation younger than a pending
  `frm` write may use the old mode. The `frm` phase makes its ordering
  deterministic by indexing the operand pool with the value the `frm` read-back
  returned, so the test is not a race -- but the machine's limitation stands.
* RMM has no host oracle on this platform (`FE_TONEARESTFROMZERO` is undefined),
  so this case does not drive it; `fp.operation_matrix` owns it.
* **No external suite covers FP**: the p1 ACT4 configuration excludes F/D, so its
  127 ELFs never execute an FP instruction. Covering FP externally needs an
  F/D-enabled ACT4 configuration (and a reference model that executes F/D), or an
  external FP conformance suite; neither exists in this tree.

---

# I-050 attempt 3 (FpStateFinal): the case is delivered

**Status: DELIVERED.** `fp.precise_flags_and_boxing` passes (`RESULT PASS ... checks=106
phases=7`), the wrong-path phase is non-vacuous, five controls each rebuild from an empty
directory, differ from the shipping binary and exit 1 on a named failure, and all eleven
required cases were re-run. The two defects attempt 2 found are still fixed; this attempt
changed no shipping RTL behaviour (every RTL edit is inside an `ifdef` that no shipping
build defines).

## 1. The wrong-path phase: why it is a trap and not a branch

The first thing to establish is that the branch construction the card suggested **cannot
work in this machine**, and that this is a property of the machine rather than of the
stimulus. `mosaic_dispatch`'s allocation gate includes a `barrier` input, and
`rtl/core/mosaic_core.sv` drives it as `.barrier(br_inflight | wfi_halt)`. The barrier
holds allocation for **every instruction younger than an unresolved branch**. So while a
branch is in flight, nothing younger than it is allocated -- and therefore nothing younger
than it can issue, execute, complete or produce flags. A mispredicted branch in this design
can only discard *fetched-but-unallocated* work.

That is exactly what the previous stimulus hit. Instrumenting the old phase showed
`fp_issue=2 fp_flags=0`: the only two FP operations that issued were the two `fmv.x.w`
moves, and the squashed `fmul.s` never reached the FP unit at all. It was not a stimulus
that was too short; there is no branch-based stimulus that would have worked. (This is the
conservative recovery the core's own header documents: "a branch is a barrier".)

A **synchronous trap has no such barrier.** The faulting instruction is allocated, issued,
and (for a load fault) carries its exception on its ROB entry; younger instructions
allocate, issue and execute normally, and the trap is taken when the faulting instruction
reaches the head. Everything younger is then squashed by the same `rob_flush_pulse` a
redirect uses. The phase is therefore built as:

1. `mtvec` is pointed at a handler at a fixed offset (`reset_vector + 0x400`) so the
   post-trap architectural state can be read.
2. `f1 = FLT_MAX`, `f2 = 2.0` (both NaN-boxed singles), `fflags <- 0`, and the
   pre-state (`fflags`, `f5`) is stored.
3. An older 32-step integer `divw` (0 / 1) is the delay. It is not an FP operation, so it
   does not occupy the shared FP unit, and being older than the faulting load it keeps the
   load -- and its trap -- behind it in retirement order.
4. `ld x5, 1(x31)` -- a **misaligned** load -- is the faulting instruction. The core decides
   a load's misalignment from the address alone, before the memory map is consulted, so the
   fault is deterministic. (An unmapped address would not do: address 0 is the boot ROM and
   is readable, so a load there does not fault.)
5. Younger than the load, `fmul.s f5, f1, f2` (FLT_MAX * 2 = +inf, `OF|NX`) **completes and
   flags** inside the divide's window, and `fdiv.s f6, f1, f2` (65 iterative steps) is
   **still in flight** when the trap fires.
6. The trap at the load's retire squashes both and clears the recorded flags.
7. The handler reads `fflags` and `f5` and exits. A no-trap path writes the same two slots,
   so a machine that never trapped fails the claim checks explicitly instead of passing on
   unwritten memory.

Observed on the shipping build: `fp_issue=4 fp_commit=3 fp_flags=1 fp_merges=2 traps=1
redirects=2 wb_stale=0`, architectural `fflags=0`, `f5` = the canonical NaN single. That is
one FP operation completed with flags and never merged, and one cancelled in flight.

**The anti-vacuity checks were not relaxed; one was added.** The phase now requires:
`fp_flags >= 1` (the squashed multiply really executed and produced its `OF|NX`),
`traps.size() == 1` at the faulting load's PC (the trap is the load's), and
`fp_issue >= fp_commit + 1` (an FP operation was accepted by the unit and never completed,
so the flush really had something in flight to cancel). The last one is what makes the
`FP_SQUASH_WRITES` control meaningful: without it, that control would have nothing to
catch.

## 2. The controls

`tools/run_fp_controls.py` (new) rebuilds the case from an empty directory per defect, with
the `-D` on the recorded command line, requires the mutant binary to differ from the
shipping one (`cmp`), and requires exit 1. Each mutant's first failure is printed.

| define | file | exit | first failure |
|---|---|---|---|
| `MOSAIC_CORE_MUTANT_FFLAGS_EARLY` | `rtl/core/mosaic_core.sv` | 1 | `arith vector 0 ...: fflags -- got -, host NX` |
| `MOSAIC_CORE_MUTANT_FP_SQUASH_WRITES` | `rtl/core/mosaic_fp_unit.sv` | 1 | `wrong-path: an FP operation was in flight at the squash (4 issued, 4 completed)` |
| `MOSAIC_CORE_MUTANT_FP_NO_UNBOX` | `rtl/core/mosaic_fp_unit.sv` | 1 | `box-nan slot 3 (an unboxed operand is the canonical quiet NaN)` |
| `MOSAIC_CORE_MUTANT_FP_NO_BOX` | `rtl/core/mosaic_fp_unit.sv` | 1 | `arith vector 0 ...: bits -- got 0x000000007fc00000, host 0x000000003f800000` |
| `MOSAIC_CSR_MUTANT_FS_NO_DIRTY` | `rtl/core/mosaic_csr.sv` | 1 | `fs-dirty: an FP write sets mstatus.FS = Dirty: reads 0x0000000000003800` |

| binary | sha256 |
|---|---|
| shipping (controls build) | `3c84a43d2a556bdaac77ad0df0a0ce3c932cbaa4a28bd917774d640db0b469fc` |
| `MOSAIC_CORE_MUTANT_FFLAGS_EARLY` | `595662b3fa9b80a1a52c559f96b3b88ff6e947345219fc05161c461f2309cabe` |
| `MOSAIC_CORE_MUTANT_FP_SQUASH_WRITES` | `a104234de56a97033bc69826914a0c3b2fe06f2d993c447ea0efa0b11c82e417` |
| `MOSAIC_CORE_MUTANT_FP_NO_UNBOX` | `da372c81ef5e248e0295cc3cc5da920179f4199fef3d9910821b3cfea26fa375` |
| `MOSAIC_CORE_MUTANT_FP_NO_BOX` | `408d762e707b3a5f926c16a9dbd75827bad5ed6fe9d7b9dc555836c89d7ce6f7` |
| `MOSAIC_CSR_MUTANT_FS_NO_DIRTY` | `c8e952df0131deb2d393851bde9bb6852b0694739170d01958463c135e51f343` |

What each control means, and where the intended check fires:

* **`FFLAGS_EARLY`** is the card's fail mode: the completion's flags are ORed into `fcsr`
  when the FP unit signals done, not when the operation retires. Its *first* failure is in
  `arith`, where the early OR races an older `csrw fflags, 0` and the flags are then
  cleared -- the early write makes the flags imprecise in program order. The wrong-path
  phase also catches it, with exactly the intended message: `wrong-path: a squashed FP
  operation contributes no fflags -- got OF|NX`. So the card's fail mode is caught both by
  an independent oracle (the host FPU) and by the squashed-operation claim.
* **`FP_SQUASH_WRITES`** removes the flush's cancellation of the operation in flight. Both
  the in-flight anti-vacuity check and the mechanism check fire; the latter reports
  `no writeback is published for a squashed operation (stale 1)`, i.e. the squashed divide
  completes after the squash and publishes a stale completion, which the writeback path
  refuses and counts. This is the evidence that "a squashed FP operation writes its
  destination" is a real, observable fail mode of the shipped design.
* **`FP_NO_UNBOX`** drops the single-precision operand rule; `box-nan` catches the unboxed
  operand no longer being the canonical NaN.
* **`FP_NO_BOX`** drops the single-precision *result* rule; `arith` catches the unboxed
  result bits. This is the **fifth control**: none of the four named in the card covers
  result NaN-boxing, and the case claims it, so the claim needed a control.
* **`FS_NO_DIRTY`** drops the `mstatus.FS = Dirty` set; `fs-dirty` catches the FP write
  leaving FS at Initial.

**No driver-level control was needed.** Every rule this case claims is a machine rule and is
expressible as an RTL `-D`; the driver's anti-vacuity checks are guards on the *stimulus*,
not claims, and `FP_SQUASH_WRITES` exercises the one of them that could otherwise be
vacuous. A control that lived only in the driver would be a control on the test, not on the
DUT, which is not what the card asks for.

## 3. The eleven cases, re-run

All four profiles' manifests were regenerated first. p0 and p1 regenerate cleanly; p2 and p3
fail manifest generation for a **pre-existing** reason unrelated to this work
(`csr vtype (0xc21): the table declares it writable but the address encodes a read-only
register`, from `config/csr/vector.json`; `tools/check_profile.py --all` still passes for
all four profiles, so no gate is affected).

| case | profile | result |
|---|---|---|
| `core.act_dut` | p1 | **PASS 127/127** (`RESULT PASS core.act_dut ran=127 passed=127 expected=127`) |
| `core.corpus_sweep` | p0 | PASS |
| `core.mem_program` | p0 | PASS |
| `privilege.permission_matrix` | p1 | PASS |
| `fp.operation_matrix` | p0 | PASS |
| `trap.precise_state` | p0 | PASS |
| `compressed.cross_boundary` | p0 | PASS |
| `sv39.walk_and_faults` | p1 | PASS |
| `csr.rule_ledger` | p0 | PASS |
| `csr.precise_trap_mret` | p0 | PASS |
| `core.unwritten_reg_read` | p0 | PASS |
| `fp.precise_flags_and_boxing` | p0 | PASS (checks=106) |

`core.act_dut` was run with `--no-generate`, reusing the pinned ELF set
(`act_commit 96493a91448ca50780013fd892daec2c204487ba`) the same way the recorded run did;
the runner still executed all 127 ELFs against the DUT and recorded PASS 127/127 in
`results/unit/core.act_dut/result.json`. Note that
`results/unit/core.act_dut/run.log` is a **stale** artifact from an older failing run
(passed=122 failed=5) that this runner does not write -- it writes `results/v043/run.log` --
so a reader should trust `result.json`/`results/v043/run.log` and not that file.

Gates re-run and green: `make check` (exit 0), `tools/lint_rtl.py --profile p0` and
`--profile p1` (47 sources clean each), `slang-tidy` (exit 0), `check_records`,
`check_exclusions` and `check_exclusions --negative`.

## 4. Files changed this attempt

* `sim/unit/tb_core_fp.cpp` -- the wrong-path phase rebuilt on the trap construction; a
  `Divw` assembler helper; the header's phase table.
* `rtl/core/mosaic_core.sv` -- `MOSAIC_CORE_MUTANT_FFLAGS_EARLY` `ifdef` at `fp_fflags_or`.
* `rtl/core/mosaic_fp_unit.sv` -- `MOSAIC_CORE_MUTANT_FP_SQUASH_WRITES` (`flush_eff`),
  `MOSAIC_CORE_MUTANT_FP_NO_UNBOX` and `MOSAIC_CORE_MUTANT_FP_NO_BOX` `ifdef`s.
* `rtl/core/mosaic_csr.sv` -- `MOSAIC_CSR_MUTANT_FS_NO_DIRTY` `ifdef`.
* `tools/run_fp_controls.py` -- new; the five controls.

No shipping RTL behaviour changed: every RTL edit is inside an `ifdef` no shipping build
defines, and `make check`, both profiles' lint, `slang-tidy` and every re-run case are
green.

## 5. Not covered (carried forward, with the new findings)

* `fsqrt` and the fmadd family remain absent (I-049 declared them).
* `frm` is not renamed, so a dynamically-rounded operation younger than a pending `frm`
  write may use the old mode. The `frm` phase makes its ordering deterministic by indexing
  the operand pool with the value the `frm` read-back returned, so the test is not a race --
  but the machine's limitation stands.
* RMM is not driven by this case: this platform's `<fenv.h>` has no
  `FE_TONEARESTFROMZERO`. `fp.operation_matrix` owns it with a derived tie expectation.
* **No external suite covers FP.** The p1 ACT4 configuration excludes F/D (`DEFERRED_REASONS`
  in `tools/run_act_dut.py` names F and D "not in the p1 profile"), so its 127 ELFs execute
  no FP instruction and the 127/127 above is not FP evidence. Covering FP externally would
  take either an F/D-enabled ACT4 configuration -- the same UDB/Sail flow, with F and D added
  to the advertised set and a Sail build that executes them (Sail 0.14.1 does support F/D, so
  this is configuration and generation work, not a new reference model) -- or a dedicated FP
  conformance suite. Neither exists in this tree, and adding one is outside I-050's
  ownership.
* **New, and a machine finding rather than a gap:** a mispredicted branch cannot produce an
  executed-and-squashed operation, because dispatch's `barrier` (`br_inflight`) stops
  allocation for everything younger than an unresolved branch. The wrong-path phase therefore
  proves the precise-`fflags` rule through a trap. If a future change removes the barrier
  (the saved-map recovery the core header describes), the branch construction becomes
  available and this phase should gain it as a second wrong path -- but it is not a defect
  today, it is the documented conservative recovery.
* The wrong-path phase's timing uses directed instruction latencies (a 32-step `divw` for the
  delay, a 65-step `fdiv.s` for the in-flight operation). It is deterministic for the fixed
  program the phase builds, and the `fp_issue >= fp_commit + 1` check fails loudly if a future
  latency change moved the in-flight operation out of the window rather than passing
  vacuously.
