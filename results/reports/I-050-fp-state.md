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
