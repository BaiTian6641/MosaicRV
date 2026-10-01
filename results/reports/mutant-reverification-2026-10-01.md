# Mutant re-verification — 2026-10-01

Independent re-run (lane `MutantReverify`) of the mutation evidence claimed by
three unit packages, against the live working-tree revision of 2026-10-01
(19:06–19:07 UTC), Verilator 5.052. No RTL, testbench, registry, config or
progress file was edited; this report is the only file written by the campaign.

| Case | RTL under test | Report whose claim is checked |
|---|---|---|
| `prf.read_bank_collision` | `rtl/core/mosaic_prf.sv` | `results/reports/I-015-prf.md` §8 |
| `csr.precise_trap_mret` | `rtl/core/mosaic_csr.sv` | `results/reports/I-019-csr.md` "Mutants" |
| `interrupt.boundary_replay` | `rtl/core/mosaic_interrupt.sv` | `results/reports/I-020-interrupt.md` "Mutants" |

## Recipe and controls

* Defines were found by grep, not taken from the reports:
  `grep -n 'MOSAIC_.*_MUTANT' rtl/core/mosaic_prf.sv rtl/core/mosaic_csr.sv rtl/core/mosaic_interrupt.sv`
  yields 20 distinct defines (6 PRF / 7 CSR / 7 interrupt). Every one was built
  and run, one at a time.
* Build: `tools/run_unit.py` with `VERILATOR_FLAGS.append('-D<DEFINE>')`, then
  `run_unit.build_case('p0', CASE, run_unit.load_registry()['cases'][CASE])`.
  Run: `build/p0/unit/<CASE>/<CASE> --case <CASE> --out <dir> --seed 1
  --max-cycles 200000`; exit code and stdout captured; failure count read from
  `<dir>/result.json` (`failures`, with `first_mismatch` as the detail).
* **Build directory deleted before the first build of every campaign**, as the
  brief demands: `build/p0/unit/prf.read_bank_collision`,
  `build/p0/unit/csr.precise_trap_mret` and
  `build/p0/unit/interrupt.boundary_replay` all existed and were deleted before
  their first (base) build. The whole directory was deleted again before each
  final base rebuild, so no base binary can share an object with a mutant.
* The runner's own build-command stamp (`build/p0/unit/<CASE>/build_command.txt`)
  was re-read after every build: each mutant's stamp contains its `-D<DEFINE>`
  and no base stamp contains any `-DMOSAIC…` flag, so no run can have executed
  the shipping build under a mutant's name.
* The RTL, its wrapper, its driver and `tests/unit/registry.json` were SHA-256'd
  before and after each campaign and were unchanged; the hashes are listed below
  and still match at the time of writing. The re-verification therefore applies
  to exactly this revision (the worktree carries unstaged edits vs `HEAD` in all
  three RTL files — lint-pragma removals, comment rewording, a moved
  write-through assignment in the PRF, and the CSR's `CYCLE_WRITE_LANDS` block
  itself is a worktree addition; all of it is inside the hashed revision).

Tested sources (SHA-256):

| File | SHA-256 |
|---|---|
| `rtl/core/mosaic_prf.sv` | `1eb2200651c054456a15a871f1306854ea4593683aa98ba0d72e417d8978196f` |
| `sim/tb/mosaic_prf_tb.sv` | `8cb16ea74af407c08792dcea719d05ce124dcdbe4ac99560f95f7a811a8771b8` |
| `sim/unit/tb_prf.cpp` | `f2b374e2b4e48f2fba21c8ce758cc0c8bc81d0bd49b06406ac647a913f85d9b7` |
| `rtl/core/mosaic_pkg.sv` | `96120782f2abca8f4c001e9b5c7eb96960259f220de882ef6434c902493a5e3e` |
| `rtl/core/mosaic_csr.sv` | `76ff0d49d23e397593429a5db8d7565a7de8576c099019419c58c66a7854d449` |
| `sim/tb/mosaic_csr_tb.sv` | `bf921a7ebb3a0f1c8082623a458083256b38561b6d047856184f3f454cedc4e4` |
| `sim/unit/tb_csr.cpp` | `7816eb72b6e4609ce50da270f96eb5eb8b2824f6ce9c00b6777f55aa1b48d0be` |
| `rtl/core/mosaic_interrupt.sv` | `fb4fbfe2378c868b2ad98284a2bfc028853feff4730037b5b633a798c0a4919a` |
| `sim/tb/mosaic_interrupt_tb.sv` | `f9ac3c9fb3aee237ea4a90ff4092df3d35d96d73cf1e7df0e6178db2fbdde41f` |
| `sim/unit/tb_interrupt.cpp` | `5666cd24a478ce45854bb252d53599bde8bc6be3b4b92c9676c9e42df6152130` |
| `tests/unit/registry.json` | `87034162494463298029ea286ff380d343db1cb735e85b2b970b07c1036e6c67` |

All 26 builds (20 mutant builds + 6 base builds, i.e. a base before and a base
after for each of the three cases) succeeded. The two base runs per case
produced byte-identical `RESULT` lines.

## Results

### `prf.read_bank_collision` — `rtl/core/mosaic_prf.sv`

Base row (run before the campaign; the post-campaign rebuild+rerun produced the
identical line, exit 0, `failures=0`, `checks=10`):

| Run | Exit | Failures | RESULT line |
|---|---|---|---|
| base, no define (before and after campaign) | 0 | 0 | `RESULT PASS prf.read_bank_collision prf contract holds: 5263 shadow comparisons over 5311 cycles, 96 entries / 4 banks of 24 rows, seed 1; 822 writes, 1309 granted reads, 568 conflicts, 1101 mismatches, 205 never-written reads` |

| File | Define | Exit | Failures | RESULT line |
|---|---|---|---|---|
| `mosaic_prf.sv` | `MOSAIC_PRF_MUTANT_NO_BANK_CONFLICT` | 1 | 1 | `RESULT FAIL prf.read_bank_collision contract violated after 25 shadow comparisons: bank-collision: cycle 37:  slot 1: rd_ready expected 0, got 1 [wr= rd=s0:(t0,g0) s1:(t4,g0) s2:(t1,g0) s3:(t2,g0) ]` |
| `mosaic_prf.sv` | `MOSAIC_PRF_MUTANT_DROP_BANK0_WRITE` | 1 | 1 | `RESULT FAIL prf.read_bank_collision contract violated after 30 shadow comparisons: write-through: cycle 46: o_wr_ctr expected 1, got 0` |
| `mosaic_prf.sv` | `MOSAIC_PRF_MUTANT_IGNORE_STORED_GEN` | 1 | 1 | `RESULT FAIL prf.read_bank_collision contract violated after 38 shadow comparisons: generation: cycle 58:  slot 0: rsp_gen_mismatch expected 1, got 0 [wr= rd=s0:(t1,g6) ]` |
| `mosaic_prf.sv` | `MOSAIC_PRF_MUTANT_NEVER_WRITTEN_VALID` | 1 | 1 | `RESULT FAIL prf.read_bank_collision contract violated after 1 shadow comparisons: reset-state: cycle 9:  slot 0: rsp_never_written expected 1, got 0 [wr= rd=s0:(t0,g0) s1:(t1,g0) s2:(t2,g0) s3:(t3,g0) ]` |
| `mosaic_prf.sv` | `MOSAIC_PRF_MUTANT_NO_WRITE_THROUGH` | 1 | 1 | `RESULT FAIL prf.read_bank_collision contract violated after 32 shadow comparisons: write-through: cycle 48:  slot 1: rsp_gen_mismatch expected 0, got 1 [wr=b0:(t0,g4,v)=0xfedcba9876543210  rd=s1:(t0,g4) ]` |
| `mosaic_prf.sv` | `MOSAIC_PRF_MUTANT_SLICE_ROW` | 1 | 1 | `RESULT FAIL prf.read_bank_collision contract violated after 34 shadow comparisons: write-through: cycle 50: the validity bit of bank 0 row 1 expected 1, got 0` |

### `csr.precise_trap_mret` — `rtl/core/mosaic_csr.sv`

Base row (identical before and after the campaign, exit 0, `failures=0`,
`checks=10`):

| Run | Exit | Failures | RESULT line |
|---|---|---|---|
| base, no define (before and after campaign) | 0 | 0 | `RESULT PASS csr.precise_trap_mret CSR contract holds: 6571 shadow comparisons over 6881 cycles, 21 CSRs, seed 1` |

| File | Define | Exit | Failures | RESULT line |
|---|---|---|---|---|
| `mosaic_csr.sv` | `MOSAIC_CSR_MUTANT_RO_WRITE_ACCEPTED` | 1 | 1 | `RESULT FAIL csr.precise_trap_mret contract violated: read-only: cycle 31: csr_wr_illegal_o[addr=0x0xf11 we=1 op=1 wdata=0x0xffffffffffffffff tick=0/0 trap=0:0x0x0000000000000000 mret=0]: expected 1, saw 0` |
| `mosaic_csr.sv` | `MOSAIC_CSR_MUTANT_CYCLE_WRITABLE` | 1 | 1 | `RESULT FAIL csr.precise_trap_mret contract violated: read-only: cycle 43: csr_wr_illegal_o[addr=0x0xc00 we=1 op=1 wdata=0x0xffffffffffffffff tick=0/0 trap=0:0x0x0000000000000000 mret=0]: expected 1, saw 0` |
| `mosaic_csr.sv` | `MOSAIC_CSR_MUTANT_READ_AFTER_WRITE` | 1 | 1 | `RESULT FAIL csr.precise_trap_mret contract violated: read-only: cycle 31: csr_rdata_o[addr=0x0xf11 we=1 op=1 wdata=0x0xffffffffffffffff tick=0/0 trap=0:0x0x0000000000000000 mret=0]: expected 0x0x0000000000000000, saw 0x0xffffffffffffffff` |
| `mosaic_csr.sv` | `MOSAIC_CSR_MUTANT_MIE_KEPT` | 1 | 1 | `RESULT FAIL csr.precise_trap_mret contract violated: trap-mret: cycle 454: o_mstatus_o: expected 0x0x0000000000001880, saw 0x0x0000000000001888` |
| `mosaic_csr.sv` | `MOSAIC_CSR_MUTANT_MRET_NO_RESTORE` | 1 | 1 | `RESULT FAIL csr.precise_trap_mret contract violated: trap-mret: cycle 459: o_mstatus_o: expected 0x0x0000000000001888, saw 0x0x0000000000001880` |
| `mosaic_csr.sv` | `MOSAIC_CSR_MUTANT_MPP_CLEARED` | 1 | 1 | `RESULT FAIL csr.precise_trap_mret contract violated: trap-mret: cycle 454: o_mstatus_o: expected 0x0x0000000000001880, saw 0x0x0000000000000080` |
| `mosaic_csr.sv` | `MOSAIC_CSR_MUTANT_CYCLE_WRITE_LANDS` | 1 | 1 | `RESULT FAIL csr.precise_trap_mret contract violated: read-only: cycle 43: o_mcycle_o: expected 0x0x0000000000000000, saw 0x0xffffffffffffffff` |

### `interrupt.boundary_replay` — `rtl/core/mosaic_interrupt.sv`

Base row (identical before and after the campaign, exit 0, `failures=0`,
`checks=1`):

| Run | Exit | Failures | RESULT line |
|---|---|---|---|
| base, no define (before and after campaign) | 0 | 0 | `RESULT PASS interrupt.boundary_replay interrupt contract holds: 17238 comparisons over 5784 cycles, 1471 offered traps (MEI 242 / MSI 703 / MTI 526), 99 legal wakes, seed 1` |

| File | Define | Exit | Failures | RESULT line |
|---|---|---|---|---|
| `mosaic_interrupt.sv` | `MOSAIC_INTERRUPT_MUTANT_NO_SYNC` | 1 | 1 | `RESULT FAIL interrupt.boundary_replay contract violated after 58 comparisons: sync-and-glitch: cycle 30 post-edge mip: expected 0x0000000000000000, got 0x0000000000000080` |
| `mosaic_interrupt.sv` | `MOSAIC_INTERRUPT_MUTANT_PRIORITY_REVERSED` | 1 | 1 | `RESULT FAIL interrupt.boundary_replay contract violated after 127 comparisons: masking: cycle 57 post-edge irq_cause: expected 0x800000000000000b, got 0x8000000000000007` |
| `mosaic_interrupt.sv` | `MOSAIC_INTERRUPT_MUTANT_IGNORE_MIDELEG` | 1 | 1 | `RESULT FAIL interrupt.boundary_replay contract violated after 297 comparisons: masking: cycle 113 pre-edge irq_valid: expected 0, got 1` |
| `mosaic_interrupt.sv` | `MOSAIC_INTERRUPT_MUTANT_MIE_IGNORED` | 1 | 1 | `RESULT FAIL interrupt.boundary_replay contract violated after 264 comparisons: masking: cycle 102 pre-edge irq_valid: expected 0, got 1` |
| `mosaic_interrupt.sv` | `MOSAIC_INTERRUPT_MUTANT_IRQ_OUTSIDE_BOUNDARY` | 1 | 1 | `RESULT FAIL interrupt.boundary_replay contract violated after 339 comparisons: boundary-latency: cycle 131 pre-edge irq_valid: expected 0, got 1` |
| `mosaic_interrupt.sv` | `MOSAIC_INTERRUPT_MUTANT_WFI_WAKE_DISABLED` | 1 | 1 | `RESULT FAIL interrupt.boundary_replay contract violated after 1435 comparisons: halt-resume: cycle 501 o_spurious_wake_ctr: a halt ended without an enabled pending interrupt (DUT count 1)` |
| `mosaic_interrupt.sv` | `MOSAIC_INTERRUPT_MUTANT_IGNORE_SW_WRITE` | 1 | 1 | `RESULT FAIL interrupt.boundary_replay contract violated after 1531 comparisons: mip-software-write: cycle 537 post-edge mip: expected 0x0000000000000080, got 0x0000000000000000` |

Failure counts above are the `failures` field of each run's `result.json`; every
mutant's `first_mismatch` is a single named mismatch, and the `RESULT` line
independently reports the same first failure (and, for PRF/interrupt, the
comparison count at which it occurred).

## Verdict — report claims vs. re-run

### I-015-prf.md, §8

Claim quoted ("Delta statement"): *"the unmodified base completes all 5263
comparisons with 0 failures and exit 0; each mutant above exits 1 with exactly 1
failure and dies at the listed comparison, i.e. on the specific behaviour it
broke."* Per-mutant claim rows quoted against the observation:

| Mutant | Claim (quoted) | Re-run observation | Confirmed |
|---|---|---|---|
| `NO_BANK_CONFLICT` | *"FAIL exit 1"*; first mismatch *"`bank-collision: cycle 37: slot 1: rd_ready expected 0, got 1 [rd=s0:(t0,g0) s1:(t4,g0) …]`"*; *"25"* comparisons before failure | exit 1, failures=1; `contract violated after 25 shadow comparisons: bank-collision: cycle 37: slot 1: rd_ready expected 0, got 1` | yes |
| `DROP_BANK0_WRITE` | *"FAIL exit 1"*; *"`write-through: cycle 46: o_wr_ctr expected 1, got 0`"*; *"30"* | exit 1, failures=1; *"contract violated after 30 shadow comparisons: write-through: cycle 46: o_wr_ctr expected 1, got 0"* | yes |
| `IGNORE_STORED_GEN` | *"FAIL exit 1"*; *"`generation: cycle 58: slot 0: rsp_gen_mismatch expected 1, got 0 [rd=s0:(t1,g6)]`"*; *"38"* | exit 1, failures=1; *"contract violated after 38 shadow comparisons: generation: cycle 58: slot 0: rsp_gen_mismatch expected 1, got 0"* | yes |
| `NEVER_WRITTEN_VALID` | *"FAIL exit 1"*; *"`reset-state: cycle 9: slot 0: rsp_never_written expected 1, got 0`"*; *"1"* | exit 1, failures=1; *"contract violated after 1 shadow comparisons: reset-state: cycle 9: slot 0: rsp_never_written expected 1, got 0"* | yes |
| `NO_WRITE_THROUGH` | *"FAIL exit 1"*; *"`write-through: cycle 48: slot 1: rsp_gen_mismatch expected 0, got 1 [wr=b0:(t0,g4,v)=0xfedcba9876543210 rd=s1:(t0,g4)]`"*; *"32"* | exit 1, failures=1; *"contract violated after 32 shadow comparisons: write-through: cycle 48: slot 1: rsp_gen_mismatch expected 0, got 1 …"* | yes |
| `SLICE_ROW` | *"FAIL exit 1"*; *"`write-through: cycle 50: the validity bit of bank 0 row 1 expected 1, got 0`"*; *"34"* | exit 1, failures=1; *"contract violated after 34 shadow comparisons: write-through: cycle 50: the validity bit of bank 0 row 1 expected 1, got 0"* | yes |

Base claim also confirmed: exactly 5263 comparisons, 0 failures, exit 0. The
only textual difference is cosmetic (the current driver prints an empty `wr=`
context field and a different amount of whitespace than the report's captured
string); cycle, signal, expected/got values and comparison counts are identical.

### I-019-csr.md, "Mutants"

Claim quoted: *"Base: `checks=10, failures=0, exit 0`. Every mutant below:
`failures=1, exit 1`, with a distinct first mismatch naming the injected
defect."* (Status line: *"seven mutants each fail with exit 1 and a one-failure
delta against a zero-failure base."*)

| Mutant | Claim (quoted) | Re-run observation | Confirmed |
|---|---|---|---|
| `MIE_KEPT` | *"exit 1"*; *"`trap-mret: cycle 454 o_mstatus_o: expected 0x…1880, saw 0x…1888`"* | exit 1, failures=1; trap-mret cycle 454, `expected 0x…1880, saw 0x…1888` | yes |
| `MRET_NO_RESTORE` | *"exit 1"*; *"`trap-mret: cycle 459 o_mstatus_o: expected 0x…1888, saw 0x…1880`"* | exit 1, failures=1; trap-mret cycle 459, `expected 0x…1888, saw 0x…1880` | yes |
| `MPP_CLEARED` | *"exit 1"*; *"`trap-mret: cycle 454 o_mstatus_o: expected 0x…1880, saw 0x…0080`"* | exit 1, failures=1; trap-mret cycle 454, `expected 0x…1880, saw 0x…0080` | yes |
| `READ_AFTER_WRITE` | *"exit 1"*; *"`read-only: cycle 31 csr_rdata_o: expected 0x0, saw 0xffffffffffffffff`"* | exit 1, failures=1; read-only cycle 31, `csr_rdata_o … expected 0x…0000, saw 0x…ffff` | yes |
| `RO_WRITE_ACCEPTED` | *"exit 1"*; *"`read-only: cycle 31 csr_wr_illegal_o: expected 1, saw 0`"* | exit 1, failures=1; read-only cycle 31, `csr_wr_illegal_o … expected 1, saw 0` | yes |
| `CYCLE_WRITABLE` | *"exit 1"*; *"`read-only: cycle 43 csr_wr_illegal_o: expected 1, saw 0`"* | exit 1, failures=1; read-only cycle 43, `csr_wr_illegal_o … expected 1, saw 0` | yes |
| `CYCLE_WRITE_LANDS` | *"exit 1"*; *"`read-only: cycle 43 o_mcycle_o: expected 0x0, saw 0xffffffffffffffff`"* | exit 1, failures=1; read-only cycle 43, `o_mcycle_o … expected 0x…0000, saw 0x…ffff` | yes |

Base claim also confirmed: `checks=10, failures=0, exit 0`, 6571 comparisons
over 6881 cycles at seed 1. Textual differences are cosmetic only (the current
result line adds the stimulus context `[addr=… we=1 …]` and prints `got` where
the report's captured string prints `saw`); the cycle, signal and values match.

### I-020-interrupt.md, "Mutants"

Claim quoted: *"Base: `PASS`, exit 0, **failure count 0**, model
`b54cf9434d46c64e`, 17238 comparisons."* and the table header *"| Mutant | Δ
failures | Δ comparisons vs base | exit | First failure (cycle) |"* with
*"Δ failures +1"*, *"exit 1"* in every row.

| Mutant | Claim (quoted Δ comparisons, exit, first failure) | Re-run observation | Confirmed |
|---|---|---|---|
| `NO_SYNC` | *"17238 → 58"*, *"exit 1"*, *"`sync-and-glitch: cycle 30 post-edge mip: expected 0x0, got 0x80`"* | exit 1, failures=1, contract violated after 58 comparisons, same cycle/signal/values | yes |
| `PRIORITY_REVERSED` | *"17238 → 127"*, *"exit 1"*, *"`masking: cycle 57 post-edge irq_cause: expected 0x8000…0b, got 0x8000…07`"* | exit 1, failures=1, after 127 comparisons, cycle 57, `expected 0x800000000000000b, got 0x8000000000000007` | yes |
| `IGNORE_MIDELEG` | *"17238 → 297"*, *"exit 1"*, *"`masking: cycle 113 pre-edge irq_valid: expected 0, got 1`"* | exit 1, failures=1, after 297 comparisons, cycle 113, same signal/values | yes |
| `MIE_IGNORED` | *"17238 → 264"*, *"exit 1"*, *"`masking: cycle 102 pre-edge irq_valid: expected 0, got 1`"* | exit 1, failures=1, after 264 comparisons, cycle 102, same signal/values | yes |
| `IRQ_OUTSIDE_BOUNDARY` | *"17238 → 339"*, *"exit 1"*, *"`boundary-latency: cycle 131 pre-edge irq_valid: expected 0, got 1`"* | exit 1, failures=1, after 339 comparisons, cycle 131, same signal/values | yes |
| `WFI_WAKE_DISABLED` | *"17238 → 1435"*, *"exit 1"*, *"`halt-resume: cycle 501 o_spurious_wake_ctr: a halt ended without an enabled pending interrupt (DUT count 1)`"* | exit 1, failures=1, after 1435 comparisons, cycle 501, identical message | yes |
| `IGNORE_SW_WRITE` | *"17238 → 1531"*, *"exit 1"*, *"`mip-software-write: cycle 537 post-edge mip: expected 0x80, got 0x0`"* | exit 1, failures=1, after 1531 comparisons, cycle 537, `expected 0x…0080, got 0x…0000` | yes |

Base claims also confirmed, including the model hash: the base run exits 0 with
`failures=0` and the exact `RESULT` line above; and
`cat build/p0/unit/interrupt.boundary_replay/obj_dir/Vmosaic_interrupt_tb*.cpp |
sha256sum` = `b54cf9434d46c64e94f74b1e31d578b88e18c5dbb869a1b224a89a85f63bb98c`,
whose prefix is the report's `b54cf9434d46c64e` (the report's
comment-only RTL edits and unchanged model hash check out).

## Summary

* 20 of 20 mutant defines found by grep in the three files were run; all 20
  builds succeeded and all 20 binaries exited 1.
* Every mutant failed with exactly 1 failure (`result.json` `failures=1`)
  against a 0-failure base, i.e. the claimed failure delta 0 → 1.
* Every mutant's first failure is the exact cycle, signal and expected/got
  values the package report lists, and the PRF/interrupt comparison counts at
  failure match the reports' numbers exactly (25/30/32/34/38/1 and
  58/127/264/297/339/1435/1531).
* No mutant exited 0, no build failed, and no number differs from the claim;
  there is no unmatched finding to report.
* The unmodified base was rebuilt (from a deleted build directory) and re-run
  after each campaign for all three cases: all three PASS, exit 0, 0 failures,
  with byte-identical `RESULT` lines to the pre-campaign base.

---

## Round 2

Independent re-run (lane `MutantReverify2`) of the mutation evidence claimed by
the nine remaining unit packages, against the live working-tree revision of
2026-10-01 19:17–19:23 UTC, Verilator 5.052. No RTL, testbench, registry, config
or progress file was edited by this campaign; this report is the only file the
lane wrote. `rtl/core/mosaic_rename.sv` was skipped on purpose (another lane is
mid-edit on it; it is to be covered in round 3).

### Scope — every mutant found by grep

The defines were found by grepping the live sources (`^\s*`(ifdef|ifndef|elsif)
MOSAIC_(…)…_MUTANT…`), not taken from the reports: 60 distinct defines.

| RTL file | Case used | Defines |
|---|---|---|
| `mosaic_bringup_core.sv` | `core.bringup_vs_reference` | 5 (`MOSAIC_BRINGUP_MUTANT_1` … `_5`) |
| `mosaic_alu.sv` | `alu.boundaries` | 5 (`MOSAIC_ALU_MUTANT_1` … `_5`) |
| `mosaic_branch_cmp.sv` | `alu.boundaries` | 1 (`MOSAIC_BRANCH_CMP_MUTANT_1`) |
| `mosaic_branch_target.sv` | `alu.boundaries` | 3 (`MOSAIC_BRANCH_TARGET_MUTANT_1` … `_3`) |
| `mosaic_decoder.sv` | `decode.rv64im_reserved` | 7 (`SRAI_FUNCT3`, `S_IMM_AS_I`, `W_FORMS_ILLEGAL`, `RESERVED_F3_LEGAL`, `SHIFT_UPPER_IGNORED`, `B_IMM_SWAPPED`, `CSR_INTENT`) |
| `mosaic_rob.sv` | `rob.out_of_order_children` | 7 (`COMPLETE_ON_ANY_BIT`, `NO_GEN_CHECK`, `GEN_LOW_BITS_ONLY`, `NO_FULL_CHECK`, `FLUSH_CLEARS_COMMITTED`, `NO_DUP_REPORT`, `RETIRE_OVER_EXCEPTION`) |
| `mosaic_retire.sv` | `retire.head_block_and_dual` | 7 |
| `mosaic_predictor.sv` | `predictor.btb_aliasing` | 5 |
| `mosaic_iq.sv` | `iq.wakeup_insert_select` | 8 |
| `mosaic_muldiv.sv` | `muldiv.kill_and_edges` | 6 |
| `mosaic_result_fifo.sv` | `completion.fu_collision` | 6 |

Where a file is compiled into more than one case, the registered case of the
table above was used: `mosaic_decoder.sv` is also compiled into
`core.bringup_vs_reference`, `mosaic_rob.sv` into
`retire.head_block_and_dual`, and the ALU/branch files into both
`alu.boundaries` and `core.bringup_vs_reference`; every define was run against
the one case the table names. Decoder defines were **not** run against
`core.bringup_vs_reference`, and rob defines were **not** run against
`retire.head_block_and_dual`.

### Recipe and controls

* Build: `tools/run_unit.py`'s own command, with `VERILATOR_FLAGS.append('-D<DEFINE>')`
  (no define for the base) and
  `run_unit.build_case('p0', CASE, run_unit.load_registry()['cases'][CASE])`.
  Each build+run ran in a fresh process, so a define could never leak from one
  row into the next.
* Run: `build/p0/unit/<CASE>/<CASE> --case <CASE> --out /tmp/mutrun2/<CASE>/<TAG>
  --seed 1 --max-cycles <N>`; the exit code and last `RESULT` line were
  captured, the failure count read from `<out>/result.json` (`failures`), and
  the first named failure from `result.json` (or the run log where the case does
  not put it in `result.json`).
* One deviation, and it is forced by the case, not a choice: the brief's run
  template says `--max-cycles 200000`. That is exactly the registered budget for
  the eight cases whose registry entry says 200000. `core.bringup_vs_reference`
  registers `max_cycles: 4000000`, and `I-008-bringup.md` §8.3 ran it with
  `--max-cycles 4000000`. Run directly at the literal 200000 the **base itself**
  fails on the first program — `RESULT FAIL … p01_addsub.i0 … event count
  differs: run has 57152, reference has 71068`, exit 1 — so the bringup campaign
  used the registered 4 000 000. Everything else used 200000.
* Build directories: `build/p0/unit/<CASE>` existed for all nine cases and was
  deleted before the first build of each package's campaign (contents listed per
  package below), then deleted again before the final base rebuild, so no base
  binary can share an object with a mutant and no mutant can reuse a shipping
  object. Every mutant build therefore started from an empty `obj_dir`.
* Command stamp: `run_unit.py` records the build command that produced each
  binary in `build/p0/unit/<CASE>/build_command.txt` and wipes `obj_dir`+binary
  when it changes. Re-read after every build: all 60 mutant stamps contain
  exactly the one `-D…` of that row and no other `-DMOSAIC…` flag; all 18 base
  stamps contain no `-DMOSAIC…` flag at all.
* Revision: the driver SHA-256'd the eleven RTL files under test,
  `mosaic_rename.sv`, `mosaic_pkg.sv`, `mosaic_uop_pkg.sv` and
  `tests/unit/registry.json` before and after every campaign. Every
  before/after pair is equal, so no campaign straddled a source change.
  `mosaic_rename.sv` was being edited by another lane at 19:16 UTC, but was not
  modified during the retire campaign window, which compiles it.
* Base before and after: each case's unmodified build was run before its
  campaign and rebuilt from a deleted directory and re-run after it. All nine
  `RESULT` lines are byte-identical between the two runs; all 18 base runs exit
  0 with `failures=0`.

Tested revision (SHA-256, constant for the whole Round 2 window):

| File | SHA-256 |
|---|---|
| `rtl/core/mosaic_bringup_core.sv` | `1cf47edaee3bf17a7dbfa0104727d694350fb5ba4a9f74a6233190e832493ccc` |
| `rtl/core/mosaic_alu.sv` | `0a92b98eb4c835dc7565b1ebd6cfa5af323c8e203177e0d000d3f23c18c5c38e` |
| `rtl/core/mosaic_branch_cmp.sv` | `cf996100038627df14c2e4db364eaf94944a4bd325b8bb7dac22ed901f7d9f4e` |
| `rtl/core/mosaic_branch_target.sv` | `0e5f8ca86a4f256aaff4564330d91a35ed65450498c46c45a2ebd55c1b569942` |
| `rtl/core/mosaic_decoder.sv` | `f27cdcf1ee28433f0d2b76c396702638563d413e490f133c0952d68380cb09c4` |
| `rtl/core/mosaic_rob.sv` | `afba8cf6f4f954d1dfbbc39796baa2d263c8bbd0c7082b5425c8dcf39dd0d2c1` |
| `rtl/core/mosaic_retire.sv` | `2745da49c116ea3846e74ebc92ef8c54c5b38a41c9d107161e9a992906c66583` |
| `rtl/core/mosaic_predictor.sv` | `bad709b88af95cbccef34ab5c668a630b0fff5e8cbb9e700322fd1cc04b2ed53` |
| `rtl/core/mosaic_iq.sv` | `447bfabdc3dc5b8c6bbf44fd61c5d37934ea7541fe854c5be55f308ada464240` |
| `rtl/core/mosaic_muldiv.sv` | `dd12e9ba3df579593d125790742ed39c5b0b5935be6ef634d2579dbf1f4f2d0d` |
| `rtl/core/mosaic_result_fifo.sv` | `7a59f77a0841dffaf48195bdbd30f7437e0796df038da6df9aed31adb154dfaf` |
| `rtl/core/mosaic_rename.sv` (compiled by the retire case) | `2564b88bed819c8d717f90ddf52c767373942861708bd317870859c3915f9628` |
| `rtl/core/mosaic_pkg.sv` | `846303bfa8031346d2a2cebcdf66b680bafe6d50ce19f42925af7f8782a4b3fb` |
| `rtl/core/mosaic_uop_pkg.sv` | `f74f76e2c13622fcfb4626d87393026764fba3872cd574250347adfcf1028d22` |
| `tests/unit/registry.json` | `5e01360037670a51703a21fdbbc3a62963e2d155b3a3072080b37a8bc3137292` |

`tests/unit/registry.json`'s hash differs from the Round-1 section's
(`5e0136…` here vs `87034162…` there): other lanes edited it between the two
campaigns. It was constant for the whole of Round 2. The worktree also carries
other lanes' live edits (decoder, pkg, lease, recovery, remote-link, rename) —
the hashes above describe the revision actually compiled and run here.

### Results

One table per package; every mutant found by grep in that package's file(s)
appears exactly once. `Exit` and `Failures` are the process exit code and the
`failures` field of the run's `result.json`; `RESULT` is the last `RESULT` line
the binary printed (long context brackets trimmed at 240 characters and marked
`[…]`); the claim column quotes the package report's own row for that mutant.
`Match` is `yes` only when the exit code, the failure count, the build-command
stamp and the claimed first mismatch/values all check out. The per-run
`result.json` and `run.log` files live under `/tmp/mutrun2/<case>/<tag>/`.

Each package section also states the build-directory deletion, the base result
before the campaign, and the byte-identical base result from the rebuild after it.

### `core.bringup_vs_reference` — `mosaic_bringup_core.sv`

Package report: `results/reports/I-008-bringup.md`. Build dir deleted before the campaign: `build/p0/unit/core.bringup_vs_reference` (held: build_command.txt, core.bringup_vs_reference, obj_dir); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=43`:

```
RESULT PASS core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical to the independent reference
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: report §8.3 base is not quoted; the claim checked is that the unmodified build still prints `RESULT PASS core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical to the independent reference` and exits 0.

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_bringup_core.sv` | `MOSAIC_BRINGUP_MUTANT_1` | `core.bringup_vs_reference` | 1 | 1 | `RESULT FAIL core.bringup_vs_reference check failed: p01_addsub.i0: the DUT and the independent reference disagree -- event count differs: run has 88751, reference has 71068` | `MOSAIC_BRINGUP_MUTANT_1` \| `x0` becomes an ordinary writable register \| **FAILS, exit 1** | yes |
| `mosaic_bringup_core.sv` | `MOSAIC_BRINGUP_MUTANT_2` | `core.bringup_vs_reference` | 1 | 3 | `RESULT FAIL core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical to the independent reference` | **FAILS, exit 1** (probe `fext`) | yes |
| `mosaic_bringup_core.sv` | `MOSAIC_BRINGUP_MUTANT_3` | `core.bringup_vs_reference` | 1 | 1 | `RESULT FAIL core.bringup_vs_reference check failed: p08_misaligned.i0: the DUT and the independent reference disagree -- event count differs: run has 71445, reference has 71593` | **FAILS, exit 1** | yes |
| `mosaic_bringup_core.sv` | `MOSAIC_BRINGUP_MUTANT_4` | `core.bringup_vs_reference` | 0 | 0 | `RESULT PASS core.bringup_vs_reference corpus 39 programs, 2774346 architectural events, every stream identical to the independent reference` | `MOSAIC_BRINGUP_MUTANT_4` ... `csrrs`/`csrrc` with a zero source still writes the CSR ... **DOES NOT FAIL, exit 0 — see 8.4** | yes |
| `mosaic_bringup_core.sv` | `MOSAIC_BRINGUP_MUTANT_5` | `core.bringup_vs_reference` | 1 | 1 | `RESULT FAIL core.bringup_vs_reference check failed: p01_addsub.i0: the DUT and the independent reference disagree -- 53250 of 71068 events differ` | **FAILS, exit 1** | yes |

First named failure per mutant observed in this re-run: `MOSAIC_BRINGUP_MUTANT_1` → p01_addsub.i0 retire stream: expected /tmp/mutrun2/core.bringup_vs_reference/MOSAIC_BRINGUP_MUTANT_1/p01_addsub.i0.reference.events.txt, got; `MOSAIC_BRINGUP_MUTANT_2` → probe fext: retire stream matches the independent reference (19 of 6664 events differ); `MOSAIC_BRINGUP_MUTANT_3` → p08_misaligned.i0 retire stream: expected /tmp/mutrun2/core.bringup_vs_reference/MOSAIC_BRINGUP_MUTANT_3/p08_misaligned.i0.reference.events.txt, got; `MOSAIC_BRINGUP_MUTANT_4` → ; `MOSAIC_BRINGUP_MUTANT_5` → p01_addsub.i0 retire stream: expected /tmp/mutrun2/core.bringup_vs_reference/MOSAIC_BRINGUP_MUTANT_5/p01_addsub.i0.reference.events.txt, got event 4: .

**Verdict: **confirmed**, with one row exiting 0 on purpose: `MOSAIC_BRINGUP_MUTANT_4` is the control the package report itself declares vacuous (§8.4, "DOES NOT FAIL, exit 0"), and acceptance criterion 3 is already recorded as NOT MET for it. The re-run reproduces exactly that, plus the other four rows and the base line byte-for-byte.

### `alu.boundaries` — `mosaic_alu.sv / mosaic_branch_cmp.sv / mosaic_branch_target.sv`

Package report: `results/reports/I-010-011-decode-alu.md (Parts I and II)`. Build dir deleted before the campaign: `build/p0/unit/alu.boundaries` (held: alu.boundaries, build_command.txt, obj_dir); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=316022`:

```
RESULT PASS alu.boundaries 108560 ALU and 49536 branch stimulus vectors (59 hand-written invariants, seed 1); alu_ops=add=4895 sub=4738 sll=9855 slt=4767 sltu=4744 xor=4871 srl=10105 sra=10252 or=4732 and=4894 addw=4813 subw=4753 sllw=10288 srlw=10144 sraw=9986 passb=4723; branch_target=22816 compare=23200 chain=3520
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: `RESULT PASS alu.boundaries 108560 ALU and 49536 branch stimulus vectors (59 hand-written invariants, seed 1); …` (I-010-011 line 504).

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_alu.sv` | `MOSAIC_ALU_MUTANT_1` | `alu.boundaries` | 1 | 8 | `RESULT FAIL alu.boundaries 8 failed checks over 4005 ALU and 0 branch vectors` | `addw(0x7fffffff, 1)`: expected `0xffffffff80000000`, got `0x0000000080000000` \| 1 | yes |
| `mosaic_alu.sv` | `MOSAIC_ALU_MUTANT_2` | `alu.boundaries` | 1 | 8 | `RESULT FAIL alu.boundaries 8 failed checks over 2889 ALU and 0 branch vectors` | `sra(0x8000000000000000, 1)`: expected `0xc000000000000000`, got `0x4000000000000000` \| 1 | yes |
| `mosaic_alu.sv` | `MOSAIC_ALU_MUTANT_3` | `alu.boundaries` | 1 | 8 | `RESULT FAIL alu.boundaries 8 failed checks over 4826 ALU and 0 branch vectors` | `sllw(-1, 32)`: expected `0xffffffffffffffff`, got `0x0000000000000000` \| 1 | yes |
| `mosaic_alu.sv` | `MOSAIC_ALU_MUTANT_4` | `alu.boundaries` | 1 | 8 | `RESULT FAIL alu.boundaries 8 failed checks over 1206 ALU and 0 branch vectors` | `slt(-1, 1)`: expected `0x0000000000000001`, got `0x0000000000000000` \| 1 | yes |
| `mosaic_alu.sv` | `MOSAIC_ALU_MUTANT_5` | `alu.boundaries` | 1 | 8 | `RESULT FAIL alu.boundaries 8 failed checks over 4 ALU and 0 branch vectors` | `add(1, -1)`: expected `zero=1`, got `zero=0` \| 1 | yes |
| `mosaic_branch_cmp.sv` | `MOSAIC_BRANCH_CMP_MUTANT_1` | `alu.boundaries` | 1 | 8 | `RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 5226 branch vectors` | `bltu(-1, 1)`: expected `0`, got `1` \| 1 | yes |
| `mosaic_branch_target.sv` | `MOSAIC_BRANCH_TARGET_MUTANT_1` | `alu.boundaries` | 1 | 8 | `RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 92 branch vectors` | `jalr(pc=0, imm=1)`: expected target `0x0000000000000000`, got `0x0000000000000001` \| 1 | yes |
| `mosaic_branch_target.sv` | `MOSAIC_BRANCH_TARGET_MUTANT_2` | `alu.boundaries` | 1 | 23 | `RESULT FAIL alu.boundaries 23 failed checks over 0 ALU and 0 branch vectors` | `JAL at pc=0`: expected link `0x0000000000000004`, got `0x0000000000000002` \| 1 | yes |
| `mosaic_branch_target.sv` | `MOSAIC_BRANCH_TARGET_MUTANT_3` | `alu.boundaries` | 1 | 8 | `RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 33 branch vectors` | `untaken branch pc=0x1000 imm=0x7f8`: expected target `0x0000000000001004`, got `0x00000000000017f8` \| 1 | yes |

First named failure per mutant observed in this re-run: `MOSAIC_ALU_MUTANT_1` → addw sign-extends its 32-bit answer: result: expected 0xffffffff80000000, got 0x0000000080000000; `MOSAIC_ALU_MUTANT_2` → sra(INT64_MIN,1) replicates the sign bit: result: expected 0xc000000000000000, got 0x4000000000000000; `MOSAIC_ALU_MUTANT_3` → sllw by 32 is a shift by 0, not a shift to zero: result: expected 0xffffffffffffffff, got 0x0000000000000000; `MOSAIC_ALU_MUTANT_4` → slt(-1,1) is 1, signed: result: expected 0x0000000000000001, got 0x0000000000000000; `MOSAIC_ALU_MUTANT_5` → add(1,-1) is zero while a != 0: zero flag: expected 1, got 0; `MOSAIC_BRANCH_CMP_MUTANT_1` → bltu(-1,1) is false, -1 is the largest unsigned: expected 0, got 1; `MOSAIC_BRANCH_TARGET_MUTANT_1` → JALR clears bit 0 of the target: target: expected 0x0000000000000000, got 0x0000000000000001; `MOSAIC_BRANCH_TARGET_MUTANT_2` → JAL at pc=0 links to pc+4 and targets pc+imm: link: expected 0x0000000000000004, got 0x0000000000000002; `MOSAIC_BRANCH_TARGET_MUTANT_3` → an untaken forward branch yields pc+4: target: expected 0x0000000000001004, got 0x00000000000017f8.

**Verdict: **confirmed**: nine of nine defects exit 1 with the claimed first mismatch and value pair, and the base line matches byte-for-byte. One textual caveat, not a coverage difference: the report says "the run stops at eight failed checks"; that holds for eight rows, but `MOSAIC_BRANCH_TARGET_MUTANT_2` exits 1 with 23 failed checks (the abort fires at 8, then the directed phase still reports the rest of its link/target vector).

### `decode.rv64im_reserved` — `mosaic_decoder.sv`

Package report: `results/reports/I-010-011-decode-alu.md (decoder half)`. Build dir deleted before the campaign: `build/p0/unit/decode.rv64im_reserved` (held: build_command.txt, decode.rv64im_reserved, obj_dir); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=169534`:

```
RESULT PASS decode.rv64im_reserved 166733 instructions (82384 legal, 84349 illegal), 0 mismatches, 1443 named reserved checks
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: addendum: `RESULT PASS decode.rv64im_reserved  166733 instructions (82384 legal, 84349 illegal), 0 mismatches, 1443 named reserved checks` (checks 169534, failures 0, exit 0).

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_decoder.sv` | `MOSAIC_DECODER_MUTANT_SRAI_FUNCT3` | `decode.rv64im_reserved` | 1 | 2 | `RESULT FAIL decode.rv64im_reserved 4713 instructions compared, 2 failure(s)` | `0x40730293.imm`: expected `0x0000000000000407`, got `0x0000000000000007` \| 1 | yes |
| `mosaic_decoder.sv` | `MOSAIC_DECODER_MUTANT_S_IMM_AS_I` | `decode.rv64im_reserved` | 1 | 2 | `RESULT FAIL decode.rv64im_reserved 1 instructions compared, 2 failure(s)` | `0xfe530823.imm`: expected `0xfffffffffffffff0`, got `0xffffffffffffffe5` \| 1 | yes |
| `mosaic_decoder.sv` | `MOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL` | `decode.rv64im_reserved` | 1 | 2 | `RESULT FAIL decode.rv64im_reserved 44 instructions compared, 2 failure(s)` | (addendum) exit 1, 2 failures, delta +2 against the green base; first mismatch: `insn=0x003100bb.valid: expected 0x0000000000000001, got 0x0000000000000000` | yes |
| `mosaic_decoder.sv` | `MOSAIC_DECODER_MUTANT_RESERVED_F3_LEGAL` | `decode.rv64im_reserved` | 1 | 2 | `RESULT FAIL decode.rv64im_reserved 116 instructions compared, 2 failure(s)` | `0x00017083.valid`: expected `0x0000000000000000`, got `0x0000000000000001` \| 1 | yes |
| `mosaic_decoder.sv` | `MOSAIC_DECODER_MUTANT_SHIFT_UPPER_IGNORED` | `decode.rv64im_reserved` | 1 | 2 | `RESULT FAIL decode.rv64im_reserved 14 instructions compared, 2 failure(s)` | `0x04031293.valid`: expected `0x0000000000000000`, got `0x0000000000000001` \| 1 | yes |
| `mosaic_decoder.sv` | `MOSAIC_DECODER_MUTANT_B_IMM_SWAPPED` | `decode.rv64im_reserved` | 1 | 2 | `RESULT FAIL decode.rv64im_reserved 9830 instructions compared, 2 failure(s)` | `0x007302e3.imm`: expected `0x0000000000000804`, got `0x0000000000000900` \| 1 | yes |
| `mosaic_decoder.sv` | `MOSAIC_DECODER_MUTANT_CSR_INTENT` | `decode.rv64im_reserved` | 1 | 2 | `RESULT FAIL decode.rv64im_reserved 93 instructions compared, 2 failure(s)` | `0x30001073.csr_reads`: expected `0x0000000000000000`, got `0x0000000000000001` \| 1 | yes |

First named failure per mutant observed in this re-run: `MOSAIC_DECODER_MUTANT_SRAI_FUNCT3` → insn=0x40730293.imm: expected 0x0000000000000407, got 0x0000000000000007; `MOSAIC_DECODER_MUTANT_S_IMM_AS_I` → insn=0xfe530823.imm: expected 0xfffffffffffffff0, got 0xffffffffffffffe5; `MOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL` → insn=0x003100bb.valid: expected 0x0000000000000001, got 0x0000000000000000; `MOSAIC_DECODER_MUTANT_RESERVED_F3_LEGAL` → insn=0x00017083.valid: expected 0x0000000000000000, got 0x0000000000000001; `MOSAIC_DECODER_MUTANT_SHIFT_UPPER_IGNORED` → insn=0x04031293.valid: expected 0x0000000000000000, got 0x0000000000000001; `MOSAIC_DECODER_MUTANT_B_IMM_SWAPPED` → insn=0x007302e3.imm: expected 0x0000000000000804, got 0x0000000000000900; `MOSAIC_DECODER_MUTANT_CSR_INTENT` → insn=0x30001073.csr_reads: expected 0x0000000000000000, got 0x0000000000000001.

**Verdict: **confirmed**: the seven defines present in the live RTL — the six surviving body rows plus the addendum's `W_FORMS_ILLEGAL` — each exit 1 with the claimed first mismatch and value pair, and the base line matches the addendum exactly. The body table's `RV32_WORD_LEGAL` no longer exists in the RTL (grep: absent), exactly as the addendum records.

### `rob.out_of_order_children` — `mosaic_rob.sv`

Package report: `results/reports/I-016-rob.md`. Build dir deleted before the campaign: `build/p0/unit/rob.out_of_order_children` (held: build_command.txt, obj_dir, rob.out_of_order_children); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=1`:

```
RESULT PASS rob.out_of_order_children rob contract holds: 1235977 shadow comparisons, 78309 of them across every slot of the buffer, over 14885 cycles and 4882 accepted allocations; soak: 3249 accepted, 563 duplicate, 3014 stale, 2878 out-of-range, 1468 refused-at-capacity, 373 exceptional, 90 flushes, seed 1
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: §10 post-width-fix evidence: `RESULT PASS rob.out_of_order_children … 1235977 shadow comparisons … soak: 3249 accepted, 563 duplicate, 3014 stale, 2878 out-of-range, 1468 refused-at-capacity, 373 exceptional, 90 flushes, seed 1`.

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_rob.sv` | `MOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT` | `rob.out_of_order_children` | 1 | 1 | `RESULT FAIL rob.out_of_order_children contract violated: out-of-order-children: a 3-child macro was called complete after its last-numbered child alone arrived -- exactly the 'last-uop-arrives-is-complete' failure` | `MOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT` ... `complete = (done_mask != 0)` instead of `done_mask == expected_mask` ... run exit=1 | yes |
| `mosaic_rob.sv` | `MOSAIC_ROB_MUTANT_NO_GEN_CHECK` | `rob.out_of_order_children` | 1 | 1 | `RESULT FAIL rob.out_of_order_children contract violated: wrap-generation: cycle 109: cmp_accepted: expected 0, got 1[alloc v=0 tag=0 pc=0x0000000000000000 n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=1 i=0 g=0x0 uop=0 exc=0 \| retire=0 flu […]` | identity is liveness alone; the generation comparison is removed \| `wrap-generation` \| `cycle 109: cmp_accepted: expected 0, got 1` | yes |
| `mosaic_rob.sv` | `MOSAIC_ROB_MUTANT_GEN_LOW_BITS_ONLY` | `rob.out_of_order_children` | 1 | 1 | `RESULT FAIL rob.out_of_order_children contract violated: wrap-generation: cycle 109: cmp_accepted: expected 0, got 1[alloc v=0 tag=0 pc=0x0000000000000000 n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=1 i=0 g=0x0 uop=0 exc=0 \| retire=0 flu […]` | (later section) `MOSAIC_ROB_MUTANT_GEN_LOW_BITS_ONLY` ... run exit=1 \| MISMATCH `wrap-generation: cycle 109: cmp_accepted: expected 0, got 1` | yes |
| `mosaic_rob.sv` | `MOSAIC_ROB_MUTANT_NO_FULL_CHECK` | `rob.out_of_order_children` | 1 | 1 | `RESULT FAIL rob.out_of_order_children contract violated: full: cycle 1205: alloc_ok: expected 0, got 1[alloc v=1 tag=999 pc=0x00000000deadbeef n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=0 i=0 g=0x0 uop=0 exc=0 \| retire=0 flush=0 obs=0]` | `alloc_ok` does not consult capacity \| `full` \| `cycle 1205: alloc_ok: expected 0, got 1` | yes |
| `mosaic_rob.sv` | `MOSAIC_ROB_MUTANT_FLUSH_CLEARS_COMMITTED` | `rob.out_of_order_children` | 1 | 1 | `RESULT FAIL rob.out_of_order_children contract violated: flush: the flush disturbed committed history: retired_total went from 6 to 0` | a flush rewinds `retired_total` **and** `gen_counter` \| `flush` \| `the flush disturbed committed history: retired_total went from 6 to 0` | yes |
| `mosaic_rob.sv` | `MOSAIC_ROB_MUTANT_NO_DUP_REPORT` | `rob.out_of_order_children` | 1 | 1 | `RESULT FAIL rob.out_of_order_children contract violated: duplicate: cycle 33: cmp_duplicate: expected 1, got 0[alloc v=0 tag=0 pc=0x0000000000000000 n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=1 i=0 g=0x0 uop=1 exc=0 \| retire=0 flush=0 o […]` | the duplicate is absorbed silently ... \| `duplicate` \| `cycle 33: cmp_duplicate: expected 1, got 0` | yes |
| `mosaic_rob.sv` | `MOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION` | `rob.out_of_order_children` | 1 | 1 | `RESULT FAIL rob.out_of_order_children contract violated: exception: an exceptional macro is reported ready to retire` | `head_ready` ignores the exception bit \| `exception` \| `an exceptional macro is reported ready to retire` | yes |

First named failure per mutant observed in this re-run: `MOSAIC_ROB_MUTANT_COMPLETE_ON_ANY_BIT` → out-of-order-children: a 3-child macro was called complete after its last-numbered child alone arrived -- exactly the 'last-uop-arrives-is-complete' f; `MOSAIC_ROB_MUTANT_NO_GEN_CHECK` → wrap-generation: cycle 109: cmp_accepted: expected 0, got 1[alloc v=0 tag=0 pc=0x0000000000000000 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=1 i=0; `MOSAIC_ROB_MUTANT_GEN_LOW_BITS_ONLY` → wrap-generation: cycle 109: cmp_accepted: expected 0, got 1[alloc v=0 tag=0 pc=0x0000000000000000 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=1 i=0; `MOSAIC_ROB_MUTANT_NO_FULL_CHECK` → full: cycle 1205: alloc_ok: expected 0, got 1[alloc v=1 tag=999 pc=0x00000000deadbeef n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=0 i=0 g=0x0 uop=0; `MOSAIC_ROB_MUTANT_FLUSH_CLEARS_COMMITTED` → flush: the flush disturbed committed history: retired_total went from 6 to 0: expected contract holds, got contract violated; `MOSAIC_ROB_MUTANT_NO_DUP_REPORT` → duplicate: cycle 33: cmp_duplicate: expected 1, got 0[alloc v=0 tag=0 pc=0x0000000000000000 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=1 i=0 g=0x0; `MOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION` → exception: an exceptional macro is reported ready to retire: expected contract holds, got contract violated.

**Verdict: **confirmed**: seven of seven exit 1 with the claimed named failure and failure delta 0→1, and the base line matches the report's later (post-width-fix) evidence block — 3249 accepted / 563 duplicate / 3014 stale / 2878 out-of-range / 1468 refused-at-capacity / 373 exceptional / 90 flushes — not the earlier body line (3248/563/3017/2876/…); the report carries both and the later one is the live revision.

### `retire.head_block_and_dual` — `mosaic_retire.sv`

Package report: `results/reports/I-017-retire.md`. Build dir deleted before the campaign: `build/p0/unit/retire.head_block_and_dual` (held: build_command.txt, obj_dir, retire.head_block_and_dual); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=1`:

```
RESULT PASS retire.head_block_and_dual retire contract holds: 469499 shadow comparisons over 0 cycles; 117 events (26 in two-wide cycles, 91 alone), 17 traps, 60 committed-map updates, 2 x0 retirements, 14 CSR writes, 12 store authorisations, 399 flushes, 2271 blocked cycles, seed 1
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: §3: `retire contract holds: 469499 shadow comparisons over 0 cycles; 117 events (26 in two-wide cycles, 91 alone), 17 traps, 60 committed-map updates, 2 x0 retirements, 14 CSR writes, 12 store authorisations, 399 flushes, 2271 blocked cycles, seed 1`.

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_retire.sv` | `MOSAIC_RETIRE_MUTANT_RETIRE_OVER_BLOCKED_HEAD` | `retire.head_block_and_dual` | 1 | 1 | `RESULT FAIL retire.head_block_and_dual contract violated: head-block: cycle 675: retire_req: expected 0, got 3[alloc v=0 tag=0 n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=0 i=0 g=0x0 uop=0 exc=0 \| robflush=0 retireflush=0 \| pay v=0x3 we= […]` | exit 1 \| failures 1 \| **+1** \| `head-block`: `retire_req: expected 0, got 3` | yes |
| `mosaic_retire.sv` | `MOSAIC_RETIRE_MUTANT_RETIRE_IN_FLUSH` | `retire.head_block_and_dual` | 1 | 1 | `RESULT FAIL retire.head_block_and_dual contract violated: flush: cycle 806: retire_req: expected 0, got 3[alloc v=0 tag=0 n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=0 i=0 g=0x0 uop=0 exc=0 \| robflush=0 retireflush=1 \| pay v=0x3 we=0x3 r […]` | exit 1 \| 1 \| **+1** \| `flush`: `retire_req: expected 0, got 3` | yes |
| `mosaic_retire.sv` | `MOSAIC_RETIRE_MUTANT_SECOND_LANE_UNORDERED` | `retire.head_block_and_dual` | 1 | 1 | `RESULT FAIL retire.head_block_and_dual contract violated: head-block: cycle 675: retire_req: expected 0, got 2[alloc v=0 tag=0 n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=0 i=0 g=0x0 uop=0 exc=0 \| robflush=0 retireflush=0 \| pay v=0x3 we= […]` | exit 1 \| 1 \| **+1** \| `head-block`: `retire_req: expected 0, got 2` | yes |
| `mosaic_retire.sv` | `MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL` | `retire.head_block_and_dual` | 1 | 1 | `RESULT FAIL retire.head_block_and_dual contract violated: head-block: cycle 698: ev_reg_we: expected 0, got 3[alloc v=0 tag=0 n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=0 i=0 g=0x0 uop=0 exc=0 \| robflush=0 retireflush=0 \| pay v=0x3 we=0 […]` | exit 1 \| 1 \| **+1** \| `head-block`: `ev_reg_we: expected 0, got 3` | yes |
| `mosaic_retire.sv` | `MOSAIC_RETIRE_MUTANT_COMMIT_ON_SQUASH` | `retire.head_block_and_dual` | 1 | 1 | `RESULT FAIL retire.head_block_and_dual contract violated: head-block: cycle 675: commit_valid: expected 0, got 3[alloc v=0 tag=0 n=1 exc=0 open=0 \| close v=0 i=0 g=0x0 \| cmp v=0 i=0 g=0x0 uop=0 exc=0 \| robflush=0 retireflush=0 \| pay v=0x3 w […]` | exit 1 \| 1 \| **+1** \| `head-block`: `commit_valid: expected 0, got 3` | yes |
| `mosaic_retire.sv` | `MOSAIC_RETIRE_MUTANT_CSR_AT_EXECUTE` | `retire.head_block_and_dual` | 1 | 1 | `RESULT FAIL retire.head_block_and_dual contract violated: csr-at-retire: cycle 0: mscratch changed to 0x0000000012345678 before its instruction retired` | exit 1 \| 1 \| **+1** \| `csr-at-retire`: `mscratch changed to 0x…12345678 before its instruction retired` | yes |
| `mosaic_retire.sv` | `MOSAIC_RETIRE_MUTANT_INSTRET_COUNTS_EXEC` | `retire.head_block_and_dual` | 1 | 1 | `RESULT FAIL retire.head_block_and_dual contract violated: in-order: cycle 10: minstret: expected 0, got 1` | exit 1 \| 1 \| **+1** \| `in-order`: `minstret: expected 0, got 1` | yes |

First named failure per mutant observed in this re-run: `MOSAIC_RETIRE_MUTANT_RETIRE_OVER_BLOCKED_HEAD` → head-block: cycle 675: retire_req: expected 0, got 3[alloc v=0 tag=0 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=0 i=0 g=0x0 uop=0 exc=0 | robflush; `MOSAIC_RETIRE_MUTANT_RETIRE_IN_FLUSH` → flush: cycle 806: retire_req: expected 0, got 3[alloc v=0 tag=0 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=0 i=0 g=0x0 uop=0 exc=0 | robflush=0 re; `MOSAIC_RETIRE_MUTANT_SECOND_LANE_UNORDERED` → head-block: cycle 675: retire_req: expected 0, got 2[alloc v=0 tag=0 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=0 i=0 g=0x0 uop=0 exc=0 | robflush; `MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL` → head-block: cycle 698: ev_reg_we: expected 0, got 3[alloc v=0 tag=0 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=0 i=0 g=0x0 uop=0 exc=0 | robflush=; `MOSAIC_RETIRE_MUTANT_COMMIT_ON_SQUASH` → head-block: cycle 675: commit_valid: expected 0, got 3[alloc v=0 tag=0 n=1 exc=0 open=0 | close v=0 i=0 g=0x0 | cmp v=0 i=0 g=0x0 uop=0 exc=0 | robflu; `MOSAIC_RETIRE_MUTANT_CSR_AT_EXECUTE` → csr-at-retire: cycle 0: mscratch changed to 0x0000000012345678 before its instruction retired: expected contract holds, got contract violated; `MOSAIC_RETIRE_MUTANT_INSTRET_COUNTS_EXEC` → in-order: cycle 10: minstret: expected 0, got 1: expected contract holds, got contract violated.

**Verdict: **confirmed**: seven of seven exit 1 with exactly 1 failure (delta +1) and the claimed named first failure; the base detail matches byte-for-byte. Cycle numbers appear in the re-run's first-mismatch context that the report's table does not print; signal, expected/got and the named failure match.

### `predictor.btb_aliasing` — `mosaic_predictor.sv`

Package report: `results/reports/I-021-predictor.md`. Build dir deleted before the campaign: `build/p0/unit/predictor.btb_aliasing` (held: build_command.txt, obj_dir, predictor.btb_aliasing); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=9`:

```
RESULT PASS predictor.btb_aliasing predictor contract holds: 9266 shadow comparisons over 4673 cycles, seed 1
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: §7 line 320: `RESULT PASS predictor.btb_aliasing predictor contract holds: 9266 shadow comparisons over 4673 cycles, seed 1`.

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_predictor.sv` | `MOSAIC_PREDICTOR_MUTANT_NO_BTB_TAG` | `predictor.btb_aliasing` | 1 | 1 | `RESULT FAIL predictor.btb_aliasing contract violated: btb-aliasing: cycle 16: pred_target: expected 0x0000000000000004, got 0x0000000080000300 [q_pc=0x0000000000000000 qv=0 qbr=0 qjmp=0 qret=0 \| upd_pc=0x0000000080000280 uv=1 ubr=1 ujmp=0 u […]` | run exit=1 \| `cycle 16: pred_target: expected 0x0000000000000004, got 0x0000000080000300` | yes |
| `mosaic_predictor.sv` | `MOSAIC_PREDICTOR_MUTANT_NO_RAS_FLUSH` | `predictor.btb_aliasing` | 1 | 1 | `RESULT FAIL predictor.btb_aliasing contract violated: ras-mispredict: cycle 93: pred_target: expected 0x0000000080005004, got 0x0000000080005014 [q_pc=0x0000000080007000 qv=1 qbr=0 qjmp=1 qret=1 \| upd_pc=0x0000000000000000 uv=0 ubr=0 ujmp=0 […]` | run exit=1 \| `cycle 93: pred_target: expected 0x0000000080005004, got 0x0000000080005014` | yes |
| `mosaic_predictor.sv` | `MOSAIC_PREDICTOR_MUTANT_NO_TRAIN` | `predictor.btb_aliasing` | 1 | 1 | `RESULT FAIL predictor.btb_aliasing contract violated: btb-aliasing: cycle 23: pred_taken: expected 1, got 0 [q_pc=0x0000000080000200 qv=1 qbr=1 qjmp=0 qret=0 \| upd_pc=0x0000000000000000 uv=0 ubr=0 ujmp=0 ucall=0 uret=0 utaken=0 ckpt=0 flush […]` | run exit=1 \| `cycle 23: pred_taken: expected 1, got 0` | yes |
| `mosaic_predictor.sv` | `MOSAIC_PREDICTOR_MUTANT_NO_RESET_VALID` | `predictor.btb_aliasing` | 1 | 1 | `RESULT FAIL predictor.btb_aliasing contract violated: mispredict-report: cycle 138: pred_taken: expected 1, got 0 [q_pc=0x000000008000a000 qv=1 qbr=1 qjmp=0 qret=0 \| upd_pc=0x0000000000000000 uv=0 ubr=0 ujmp=0 ucall=0 uret=0 utaken=0 ckpt=0 […]` | run exit=1 \| `cycle 138: pred_taken: expected 1, got 0` | yes |
| `mosaic_predictor.sv` | `MOSAIC_PREDICTOR_MUTANT_NO_RAS_UNDERFLOW_REPORT` | `predictor.btb_aliasing` | 1 | 1 | `RESULT FAIL predictor.btb_aliasing contract violated: ras: cycle 84: ras_underflow: expected 1, got 0` | run exit=1 \| `cycle 84: ras_underflow: expected 1, got 0` | yes |

First named failure per mutant observed in this re-run: `MOSAIC_PREDICTOR_MUTANT_NO_BTB_TAG` → btb-aliasing: cycle 16: pred_target: expected 0x0000000000000004, got 0x0000000080000300 [q_pc=0x0000000000000000 qv=0 qbr=0 qjmp=0 qret=0 | upd_pc=0x; `MOSAIC_PREDICTOR_MUTANT_NO_RAS_FLUSH` → ras-mispredict: cycle 93: pred_target: expected 0x0000000080005004, got 0x0000000080005014 [q_pc=0x0000000080007000 qv=1 qbr=0 qjmp=1 qret=1 | upd_pc=; `MOSAIC_PREDICTOR_MUTANT_NO_TRAIN` → btb-aliasing: cycle 23: pred_taken: expected 1, got 0 [q_pc=0x0000000080000200 qv=1 qbr=1 qjmp=0 qret=0 | upd_pc=0x0000000000000000 uv=0 ubr=0 ujmp=0 ; `MOSAIC_PREDICTOR_MUTANT_NO_RESET_VALID` → mispredict-report: cycle 138: pred_taken: expected 1, got 0 [q_pc=0x000000008000a000 qv=1 qbr=1 qjmp=0 qret=0 | upd_pc=0x0000000000000000 uv=0 ubr=0 u; `MOSAIC_PREDICTOR_MUTANT_NO_RAS_UNDERFLOW_REPORT` → ras: cycle 84: ras_underflow: expected 1, got 0: expected contract holds, got contract violated.

**Verdict: **confirmed**: five of five exit 1 with the claimed cycle, signal and expected/got values; base line matches byte-for-byte.

### `iq.wakeup_insert_select` — `mosaic_iq.sv`

Package report: `results/reports/I-022-iq.md / I-022-iq-coverage.md`. Build dir deleted before the campaign: `build/p0/unit/iq.wakeup_insert_select` (held: build_command.txt, iq.wakeup_insert_select, obj_dir); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=93878203`:

```
RESULT PASS iq.wakeup_insert_select 93878203 checks over 199800 cycles, 4 reset cycles, 2 clusters
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: I-022-iq-coverage final case result: `RESULT PASS iq.wakeup_insert_select 93878203 checks over 199800 cycles, 4 reset cycles, 2 clusters`.

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_iq.sv` | `MOSAIC_IQ_MUTANT_NARROW_AGE` | `iq.wakeup_insert_select` | 1 | 1069026 | `RESULT FAIL iq.wakeup_insert_select 1069026 failed of 93878203 checks over 199800 cycles` | `MOSAIC_IQ_MUTANT_NARROW_AGE` \| 1 \| 1069026 \| `c0 slot 1: age` | yes |
| `mosaic_iq.sv` | `MOSAIC_IQ_MUTANT_NO_GEN_CHECK` | `iq.wakeup_insert_select` | 1 | 55349339 | `RESULT FAIL iq.wakeup_insert_select 55349339 failed of 93971835 checks over 199800 cycles` | `MOSAIC_IQ_MUTANT_NO_GEN_CHECK` \| 1 \| 55349339 \| `c0: grant_valid matches the shadow's prediction` | yes |
| `mosaic_iq.sv` | `MOSAIC_IQ_MUTANT_NO_SAME_CYCLE_WAKEUP` | `iq.wakeup_insert_select` | 1 | 52143167 | `RESULT FAIL iq.wakeup_insert_select 52143167 failed of 93874226 checks over 199800 cycles` | `MOSAIC_IQ_MUTANT_NO_SAME_CYCLE_WAKEUP` \| 1 \| 52143167 \| `c0: grant_valid matches the shadow's prediction` | yes |
| `mosaic_iq.sv` | `MOSAIC_IQ_MUTANT_UNSTABLE_GRANT` | `iq.wakeup_insert_select` | 1 | 46015289 | `RESULT FAIL iq.wakeup_insert_select 46015289 failed of 93878203 checks over 199800 cycles` | `MOSAIC_IQ_MUTANT_UNSTABLE_GRANT` \| 1 \| 46015289 \| `c0: granted uop identity` | yes |
| `mosaic_iq.sv` | `MOSAIC_IQ_MUTANT_DROP_ON_GRANT` | `iq.wakeup_insert_select` | 1 | 51881655 | `RESULT FAIL iq.wakeup_insert_select 51881655 failed of 93858834 checks over 199800 cycles` | `MOSAIC_IQ_MUTANT_DROP_ON_GRANT` \| 1 \| 51881655 \| `c0: the valid vector is exactly the shadow's live slots` | yes |
| `mosaic_iq.sv` | `MOSAIC_IQ_MUTANT_NO_DST_CHECK` | `iq.wakeup_insert_select` | 1 | 52102 | `RESULT FAIL iq.wakeup_insert_select 52102 failed of 93878203 checks over 199800 cycles` | `MOSAIC_IQ_MUTANT_NO_DST_CHECK` \| 1 \| 52102 \| `c0: o_dst_conflict matches the shadow's duplicate-destination search` | yes |
| `mosaic_iq.sv` | `MOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES` | `iq.wakeup_insert_select` | 1 | 53576702 | `RESULT FAIL iq.wakeup_insert_select 53576702 failed of 93877635 checks over 199800 cycles` | `MOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES` \| 1 \| 53576702 \| `refused: o_alloc_index did not advance past the shadow's slot` | yes |
| `mosaic_iq.sv` | `MOSAIC_IQ_MUTANT_META_SWAP` | `iq.wakeup_insert_select` | 1 | 17764582 | `RESULT FAIL iq.wakeup_insert_select 17764582 failed of 93878203 checks over 199800 cycles` | `MOSAIC_IQ_MUTANT_META_SWAP` \| 1 \| 17764582 \| `c0 slot 0: meta class` | yes |

First named failure per mutant observed in this re-run: `MOSAIC_IQ_MUTANT_NARROW_AGE` → c0 slot 1: age; `MOSAIC_IQ_MUTANT_NO_GEN_CHECK` → c0: grant_valid matches the shadow's prediction; `MOSAIC_IQ_MUTANT_NO_SAME_CYCLE_WAKEUP` → c0: grant_valid matches the shadow's prediction; `MOSAIC_IQ_MUTANT_UNSTABLE_GRANT` → c0: granted uop identity; `MOSAIC_IQ_MUTANT_DROP_ON_GRANT` → c0: the valid vector is exactly the shadow's live slots; `MOSAIC_IQ_MUTANT_NO_DST_CHECK` → c0: o_dst_conflict matches the shadow's duplicate-destination search; `MOSAIC_IQ_MUTANT_REFUSED_INSERT_ADVANCES` → refused: o_alloc_index did not advance past the shadow's slot; `MOSAIC_IQ_MUTANT_META_SWAP` → c0 slot 0: meta class.

**Verdict: **confirmed against `I-022-iq-coverage.md` §5.5**: all eight exit 1, every failure count is identical to the table (17764582 / 53576702 / 1069026 / 55349339 / 52143167 / 46015289 / 51881655 / 52102), and the first named failure matches for all eight. The older `I-022-iq.md` table is superseded by that audit: it gave `c0 slot 0: age` for NARROW_AGE/NO_GEN_CHECK/NO_SAME_CYCLE_WAKEUP, while the audit report and this re-run give `c0 slot 1: age` and `c0: grant_valid matches the shadow's prediction` (distinct per mutant).

### `muldiv.kill_and_edges` — `mosaic_muldiv.sv`

Package report: `results/reports/I-012-muldiv.md`. Build dir deleted before the campaign: `build/p0/unit/muldiv.kill_and_edges` (held: build_command.txt, muldiv.kill_and_edges, obj_dir); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=1155304`:

```
RESULT PASS muldiv.kill_and_edges muldiv contract holds: 76192 shadow comparisons over 76228 cycles, 1238 directed operations, 98 cancellations (2 with a computed result), latency 65/33 cycles (64/W), seed 1
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: §“What the case checks”: `Totals: 76 192 shadow comparisons over 76 228 cycles, 1 155 304 named checks, 0 failures`.

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_muldiv.sv` | `MOSAIC_MULDIV_MUTANT_FLUSH_IGNORED` | `muldiv.kill_and_edges` | 1 | 10 | `RESULT FAIL muldiv.kill_and_edges aborted: cancel: request refused after flush` | exit 1 \| 963 936 \| 10 \| **+10** \| `cancel-sweep … flush=1 … o_busy=1 expected 0` | yes |
| `mosaic_muldiv.sv` | `MOSAIC_MULDIV_MUTANT_DROP_UNREADY` | `muldiv.kill_and_edges` | 1 | 6050 | `RESULT FAIL muldiv.kill_and_edges aborted: backpressure: request refused after delivery` | exit 1 \| 1 091 459 \| 6 050 \| **+6 050** \| `cancel-sweep … at iteration 64 … o_busy=0 expected 1` | yes |
| `mosaic_muldiv.sv` | `MOSAIC_MULDIV_MUTANT_RES_STUCK` | `muldiv.kill_and_edges` | 1 | 11 | `RESULT FAIL muldiv.kill_and_edges aborted: directed: directed mul 0x0000000000000000 0x0000000000000001: request refused while idle` | exit 1 \| 1 034 \| 11 \| **+11** \| `directed-edges … o_busy=1 expected 0` (the next request is refused) | yes |
| `mosaic_muldiv.sv` | `MOSAIC_MULDIV_MUTANT_DIV0_ZERO` | `muldiv.kill_and_edges` | 1 | 130 | `RESULT FAIL muldiv.kill_and_edges muldiv contract holds: 76192 shadow comparisons over 76228 cycles, 1238 directed operations, 98 cancellations (2 with a computed result), latency 65/33 cycles (64/W), seed 1` | exit 1 \| 1 155 304 \| 130 \| **+130** \| `directed div 0x0 0x0: result 0x0 expected 0xffffffffffffffff` | yes |
| `mosaic_muldiv.sv` | `MOSAIC_MULDIV_MUTANT_IGNORE_W` | `muldiv.kill_and_edges` | 1 | 113726 | `RESULT FAIL muldiv.kill_and_edges muldiv contract holds: 101760 shadow comparisons over 101796 cycles, 1238 directed operations, 98 cancellations (2 with a computed result), latency 65/65 cycles (64/W), seed 1` | exit 1 \| 1 537 212 \| 113 726 \| **+113 726** \| `directed-edges cycle 4270 [directed mulw …] res_valid_o=0 expected 1, o_iter=33 expected 0` | yes |
| `mosaic_muldiv.sv` | `MOSAIC_MULDIV_MUTANT_UNSTABLE_RESULT` | `muldiv.kill_and_edges` | 1 | 209 | `RESULT FAIL muldiv.kill_and_edges muldiv contract holds: 76192 shadow comparisons over 76228 cycles, 1238 directed operations, 98 cancellations (2 with a computed result), latency 65/33 cycles (64/W), seed 1` | exit 1 \| 1 155 304 \| 209 \| **+209** \| `backpressure: the offered value changed while waiting` | yes |

First named failure per mutant observed in this re-run: `MOSAIC_MULDIV_MUTANT_FLUSH_IGNORED` → cancel-sweep: cycle 63587 [cancel mul at iteration 0] [valid=0 op=mul a=0x0000000000000000 b=0x0000000000000000 id(rob=0, gen=0, uop=0) flush=1 ready=; `MOSAIC_MULDIV_MUTANT_DROP_UNREADY` → cancel-sweep: cycle 70019 [cancel mul at iteration 64] [valid=0 op=mul a=0x0000000000000000 b=0x0000000000000000 id(rob=0, gen=0, uop=0) flush=0 ready; `MOSAIC_MULDIV_MUTANT_RES_STUCK` → directed-edges: cycle 78 [directed mul 0x0000000000000000 0x0000000000000000] [valid=0 op=mul a=0x0000000000000000 b=0x0000000000000000 id(rob=0, gen=; `MOSAIC_MULDIV_MUTANT_DIV0_ZERO` → directed-edges: cycle 29078 [directed div 0x0000000000000000 0x0000000000000000] [valid=0 op=mul a=0x0000000000000000 b=0x0000000000000000 id(rob=0, g; `MOSAIC_MULDIV_MUTANT_IGNORE_W` → directed-edges: cycle 4270 [directed mulw 0x0000000000000000 0x0000000000000000] [valid=0 op=mul a=0x0000000000000000 b=0x0000000000000000 id(rob=0, g; `MOSAIC_MULDIV_MUTANT_UNSTABLE_RESULT` → backpressure: cycle 71874 [back-pressure hold] [valid=1 op=mulw a=0xf0f0f0f0f0f0f0f0 b=0x0f0f0f0f0f0f0f0f id(rob=0, gen=0, uop=1) flush=0 ready=0] res.

**Verdict: **confirmed**: all six failure counts match the report exactly (10 / 6 050 / 11 / 130 / 113 726 / 209), every first failure is the named check the report quotes, and the base totals match (76192 comparisons over 76228 cycles).

### `completion.fu_collision` — `mosaic_result_fifo.sv`

Package report: `results/reports/I-025-result-fifo.md`. Build dir deleted before the campaign: `build/p0/unit/completion.fu_collision` (held: build_command.txt, completion.fu_collision, obj_dir); it was deleted again before the final base rebuild.

Base (no define), before the campaign — exit 0, `failures=0`, `checks=8`:

```
RESULT PASS completion.fu_collision result fifo contract holds: 30057 per-cycle whole-state comparisons over 30121 cycles, 2 entries / 3 producers / 209-bit payload, seed 1
```

Rebuilt from a deleted directory after the campaign and re-run: byte-identical `RESULT` line, exit 0, `failures=0` — identical. Claim checked: §5 line 14: `RESULT PASS completion.fu_collision result fifo contract holds: 30057 per-cycle whole-state comparisons over 30121 cycles, 2 entries / 3 producers / 209-bit payload, seed 1`.

| RTL file | Define | Case | Exit | Failures | RESULT line (quoted, context brackets trimmed where marked `[…]`) | Package report claim (quoted) | Match |
|---|---|---|---|---|---|---|---|
| `mosaic_result_fifo.sv` | `MOSAIC_RESULT_FIFO_MUTANT_EXC_DROPPED` | `completion.fu_collision` | 1 | 1 | `RESULT FAIL completion.fu_collision contract violated: collision: cycle 24: o_exc_occ differs from the shadow at position 1` | `EXC_DROPPED` \| 1 \| 2 \| 1 \| **+1** \| `collision: cycle 24: o_exc_occ differs from the shadow at position 1` | yes |
| `mosaic_result_fifo.sv` | `MOSAIC_RESULT_FIFO_MUTANT_KILL_DELIVERS` | `completion.fu_collision` | 1 | 1 | `RESULT FAIL completion.fu_collision contract violated: kill: cycle 64: c_valid: expected 0, got 1 [p0=1{i2,g0,u0,d0x0x1111000000000002,exc0,cause0,tval0x0x0000000000000000} p1=0 p2=1{i0,g0,u1,d0x0x111100005a5a5a5a,exc0,cause0,tval0x0x000000 […]` | `KILL_DELIVERS` \| 1 \| 4 \| 1 \| **+1** \| `kill: cycle 64: c_valid: expected 0, got 1` — a killed head is presented | yes |
| `mosaic_result_fifo.sv` | `MOSAIC_RESULT_FIFO_MUTANT_VALID_PULSE` | `completion.fu_collision` | 1 | 1 | `RESULT FAIL completion.fu_collision contract violated: collision: cycle 18: p_ready[0]: expected 1, got 0 [p0=1{i1,g0,u0,d0x0x1111000000000001,exc0,cause0,tval0x0x0000000000000000} p1=1{i2,g0,u0,d0x0x1111000000000002,exc0,cause0,tval0x0x000 […]` | `VALID_PULSE` \| 1 \| 2 \| 1 \| **+1** \| `collision: cycle 18: p_ready[0]: expected 1, got 0` — a held `valid` is not re-offered | yes |
| `mosaic_result_fifo.sv` | `MOSAIC_RESULT_FIFO_MUTANT_DROP_ON_COLLISION` | `completion.fu_collision` | 1 | 1 | `RESULT FAIL completion.fu_collision contract violated: collision: cycle 18: p_ready[1]: expected 0, got 1 [p0=1{i1,g0,u0,d0x0x1111000000000001,exc0,cause0,tval0x0x0000000000000000} p1=1{i2,g0,u0,d0x0x1111000000000002,exc0,cause0,tval0x0x000 […]` | `DROP_ON_COLLISION` \| 1 \| 2 \| 1 \| **+1** \| `collision: cycle 18: p_ready[1]: expected 0, got 1` — the loser is told its result was taken | yes |
| `mosaic_result_fifo.sv` | `MOSAIC_RESULT_FIFO_MUTANT_KILL_NOT_COUNTED` | `completion.fu_collision` | 1 | 1 | `RESULT FAIL completion.fu_collision contract violated: kill: cycle 64: o_kill_ctr: expected 1, got 0` | `KILL_NOT_COUNTED` \| 1 \| 4 \| 1 \| **+1** \| `kill: cycle 64: o_kill_ctr: expected 1, got 0` | yes |
| `mosaic_result_fifo.sv` | `MOSAIC_RESULT_FIFO_MUTANT_OCC_OFF_BY_ONE` | `completion.fu_collision` | 1 | 1 | `RESULT FAIL completion.fu_collision contract violated: collision: cycle 17: o_count: expected 1, got 0` | `OCC_OFF_BY_ONE` \| 1 \| 2 \| 1 \| **+1** \| `collision: cycle 17: o_count: expected 1, got 0` | yes |

First named failure per mutant observed in this re-run: `MOSAIC_RESULT_FIFO_MUTANT_EXC_DROPPED` → collision: cycle 24: o_exc_occ differs from the shadow at position 1: expected contract holds, got contract violated; `MOSAIC_RESULT_FIFO_MUTANT_KILL_DELIVERS` → kill: cycle 64: c_valid: expected 0, got 1 [p0=1{i2,g0,u0,d0x0x1111000000000002,exc0,cause0,tval0x0x0000000000000000} p1=0 p2=1{i0,g0,u1,d0x0x11110000; `MOSAIC_RESULT_FIFO_MUTANT_VALID_PULSE` → collision: cycle 18: p_ready[0]: expected 1, got 0 [p0=1{i1,g0,u0,d0x0x1111000000000001,exc0,cause0,tval0x0x0000000000000000} p1=1{i2,g0,u0,d0x0x11110; `MOSAIC_RESULT_FIFO_MUTANT_DROP_ON_COLLISION` → collision: cycle 18: p_ready[1]: expected 0, got 1 [p0=1{i1,g0,u0,d0x0x1111000000000001,exc0,cause0,tval0x0x0000000000000000} p1=1{i2,g0,u0,d0x0x11110; `MOSAIC_RESULT_FIFO_MUTANT_KILL_NOT_COUNTED` → kill: cycle 64: o_kill_ctr: expected 1, got 0: expected contract holds, got contract violated; `MOSAIC_RESULT_FIFO_MUTANT_OCC_OFF_BY_ONE` → collision: cycle 17: o_count: expected 1, got 0: expected contract holds, got contract violated.

**Verdict: **confirmed**: six of six exit 1 with exactly 1 failure (delta +1) and the claimed first mismatch; base line matches byte-for-byte.

### Round 2 summary

* 60 of 60 mutant defines found by grep were built and run; all 60 builds
  succeeded.
* 59 exit 1 with a `RESULT FAIL` line. **One exits 0**: `MOSAIC_BRINGUP_MUTANT_4`
  — the control `I-008-bringup.md` §8.4 itself declares vacuous ("DOES NOT FAIL,
  exit 0", acceptance criterion 3 NOT MET for it). Its exit 0 *is* the claim and
  the re-run confirms it; it is called out here because the brief requires a
  non-failing mutant to be reported rather than smoothed over.
* Every other mutant's exit code, failure count and named first failure match
  the package report's claim row. No number differs; no mutant behaves
  differently from its report.
* 18 of 18 base runs (a base before, and a base rebuilt from a deleted build
  directory after each of the nine campaigns) exit 0 with `failures=0`, and the
  before/after pair is byte-identical for every case.
* The only difference from the brief's run template is the `--max-cycles` value
  for `core.bringup_vs_reference` (registered 4 000 000, because 200000 truncates
  the base itself), documented above. It is a property of that case, not of any
  mutant.

### Per-package verdict

| Package report | Case(s) | Mutants | Verdict |
|---|---|---|---|
| `I-008-bringup.md` | `core.bringup_vs_reference` | 5 | **confirmed** — 4 failures + 1 documented exit-0 control, base PASS |
| `I-010-011-decode-alu.md` (Parts I/II) | `alu.boundaries` | 9 | **confirmed** — 9/9 exit 1 with the claimed mismatches |
| `I-010-011-decode-alu.md` (decoder half, incl. addendum) | `decode.rv64im_reserved` | 7 | **confirmed** — 7/7 exit 1 with the claimed mismatches |
| `I-016-rob.md` | `rob.out_of_order_children` | 7 | **confirmed** — 7/7 exit 1, delta 0→1 |
| `I-017-retire.md` | `retire.head_block_and_dual` | 7 | **confirmed** — 7/7 exit 1, delta 0→1 |
| `I-021-predictor.md` | `predictor.btb_aliasing` | 5 | **confirmed** — 5/5 exit 1 |
| `I-022-iq-coverage.md` (superseding `I-022-iq.md`) | `iq.wakeup_insert_select` | 8 | **confirmed** — 8/8 exit 1, all eight failure counts identical |
| `I-012-muldiv.md` | `muldiv.kill_and_edges` | 6 | **confirmed** — 6/6 exit 1, all six counts identical |
| `I-025-result-fifo.md` | `completion.fu_collision` | 6 | **confirmed** — 6/6 exit 1, delta 0→1 |

### Mutants whose behaviour differs from the claim

None. The only mutant that does not fail is `MOSAIC_BRINGUP_MUTANT_4`, and its
package report claims precisely that (exit 0, vacuous control); it is reported
above as the report's own open finding, not as a new discrepancy.

Two textual caveats, neither a behavioural difference:

* `I-010-011-decode-alu.md` says "the run stops at eight failed checks";
  `MOSAIC_BRANCH_TARGET_MUTANT_2` exits 1 with 23 failed checks (the abort fires
  at 8, then the directed link/target phase still reports the rest of its
  vector). Exit code and first mismatch are as claimed.
* `I-016-rob.md` carries two base lines (an earlier one with 3248 accepted / 3017
  stale / 2876 out-of-range, and the post-width-fix one with 3249 / 3014 /
  2878). This re-run reproduces the later one byte-for-byte.

---

## Round 3

Independent re-run (lane `MutantReverify3-2`) of the mutation evidence claimed
by the two rename packages for `rtl/core/mosaic_rename.sv` — `I-013-rename.md`
§9 and `I-014-rename2w.md` §6 — against the live working-tree revision of
2026-10-01 20:05–20:16 UTC, Verilator 5.052. Every mutant found by grep in the
file was run against **both** registered cases, `rename.single_width_ownership`
(I-013) and `rename.same_cycle_chain` (I-014): 13 defines × 2 cases = 26 mutant
runs plus base runs before and after each campaign. No RTL, testbench, registry,
config or progress file was edited by this campaign; this report is the only file
the lane wrote.

### Scope — every mutant found by grep

`grep -nE '^[[:space:]]*`(ifdef|elsif|ifndef)[[:space:]]+MOSAIC_RENAME_MUTANT_'
rtl/core/mosaic_rename.sv` yields **13 distinct defines** in 15 guard
directives (`NO_GEN_CHECK` and `WAW_COMMIT2_PRE_MAP` each guard two blocks).
The defines were found this way, not taken from the reports.

| Define | Guard lines | Blocks | Claimed by |
|---|---|---|---|
| `NO_EXHAUST_CHECK` | 763 | 1 | I-013 #4 |
| `NONATOMIC_GROUP` | 772 (elsif) | 1 | I-014 #2 |
| `X0_ALLOC` | 803 | 1 | I-013 #2 / I-014 #3 |
| `SAME_TAG_LANE1` | 819 | 1 | I-014 #4 |
| `WAW_OLD_FROM_MAP` | 850 | 1 | I-014 #5 |
| `NO_BYPASS` | 880 | 1 | I-014 #1 |
| `NO_GEN_CHECK` | 955, 985 | 2 | I-013 #1 |
| `NO_DUP_WB_GUARD` | 964 | 1 | I-013 #5 |
| `NO_DOUBLE_FREE_CHECK` | 992 | 1 | I-013 #6 |
| `WAW_COMMIT2_PRE_MAP` | 1063, 1207 | 2 | I-014 #6 |
| `NO_BOUNDARY_CHECK` | 1117 | 1 | none (live RTL control 13) |
| `NO_FREE_RESTORE` | 1230 | 1 | I-013 #3 |
| `CKPT_ALLOC_LEAK` | 1330 | 1 | none (live RTL control 12) |

Two of the 13 (`CKPT_ALLOC_LEAK`, `NO_BOUNDARY_CHECK`) exist only in the live RTL:
no package report carries a row or claim for them (`grep -n
'CKPT_ALLOC_LEAK\|NO_BOUNDARY_CHECK' results/reports/I-013-rename.md
results/reports/I-014-rename2w.md` returns nothing; the only hits in the tree are
the RTL guards and this section). They were run anyway, and their observed
behaviour is reported below without a claim to match.

### Recipe and controls

* Build: `tools/run_unit.py`'s own command with
  `VERILATOR_FLAGS.append('-DMOSAIC_RENAME_MUTANT_<NAME>')` and
  `run_unit.build_case('p0', CASE, run_unit.load_registry()['cases'][CASE])`.
  Each build+run ran in a **fresh process**, so a define could never leak from
  one row into the next.
* Run: `build/p0/unit/<CASE>/<CASE> --case <CASE> --out /tmp/mutrun3/<CASE>/<TAG>
  --seed 1 --max-cycles 200000` (both cases register 200000); the exit code and
  last `RESULT` line were captured, the failure count and first mismatch read
  from `<out>/result.json`.
* Build directories: both `build/p0/unit/rename.single_width_ownership` and
  `build/p0/unit/rename.same_cycle_chain` existed before the campaign (each
  holding `build_command.txt`, `obj_dir` and the case binary), were deleted
  before the first build of each campaign, and deleted again before the final
  base rebuild, so no base binary can share an object with a mutant and no
  mutant can reuse a shipping object.
* Command stamp: `build_command.txt` was re-read after every build. Every
  mutant stamp contains exactly its own `-DMOSAIC…` token and nothing else;
  every base stamp contains no `-DMOSAIC…` flag at all.
* Binary-difference control: after the main campaign a second pass rebuilt the
  base and all 13 mutants from deleted directories and SHA-256'd the produced
  binary. **All 13 mutant binaries differ from the shipping binary in both
  cases** (full hashes below), so each `-D` demonstrably reached the
  elaborator rather than defining an unused macro.
* One pass was **discarded**: the first scripted pass appended the row's short
  tag (`-DX0_ALLOC`) instead of the full macro name. Every row then compiled
  and ran the shipping build and PASSed; the build-command stamp showed no
  `-DMOSAIC…` token, the pass was thrown away and re-run with the full define.
  That is the "a `-D` whose `ifdef` body was never compiled proves nothing"
  trap, caught by the stamp rather than by the green line.
* Revision: the files below were SHA-256'd before and after every campaign
  and are unchanged. `tests/unit/registry.json` was edited by another lane
  (commit `05411a5`, 20:08:20 UTC) *before* the first campaign build; the two
  rename entries are byte-identical in content to the revision seen at the
  start of this session, so the case definitions under test did not move.

Tested revision (SHA-256):

| File | SHA-256 |
|---|---|
| `rtl/core/mosaic_rename.sv` | `2564b88bed819c8d717f90ddf52c767373942861708bd317870859c3915f9628` |
| `sim/tb/mosaic_rename_tb.sv` | `d112b114b4eb3c0a09e69606690446a0de97d8b46b2cf43a61e5a7e6b734c00b` |
| `sim/unit/tb_rename.cpp` | `22ae6712c032159322ae0fe4ce2dccdaac601e86350e3e3cb7c596ab1b5b4229` |
| `tests/unit/registry.json` | `0327dfa51954d32709a4941ad0254958fb4e3bbc44c23d499fb5e7566e007b7d` |
| `rtl/core/mosaic_pkg.sv` (include) | `846303bfa8031346d2a2cebcdf66b680bafe6d50ce19f42925af7f8782a4b3fb` |
| `build/p0/rtl/mosaic_cfg_pkg.svh` (generated include) | `041cf99b17d28632fee55b936f2618a649d49c815ae94f8b5aad57d783ecd986` |

### Results — `rename.single_width_ownership` (I-013's case)

Base (no define), before the campaign — exit 0, `failures=0`,
`checks=9`:

```
RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1
```

Rebuilt from a deleted directory after the campaign and re-run, again for
the binary-difference control, and once more as the final action of the
campaign: all reruns exit 0, `failures=0`, and print a byte-identical
`RESULT` line.

| Define | Exit | Failures | Checks | Binary ≠ shipping | RESULT line (quoted; context brackets trimmed where marked `[…]`) |
|---|---|---|---|---|---|
| `NO_EXHAUST_CHECK` | 1 | 1 | 7 | yes | `RESULT FAIL rename.single_width_ownership contract violated: exhaustion: cycle 1678: alloc_accepted: expected 0, got 1 [alloc=1:x20 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `NONATOMIC_GROUP` | 0 | 0 | 9 | yes | `RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1` |
| `X0_ALLOC` | 1 | 1 | 5 | yes | `RESULT FAIL rename.single_width_ownership contract violated: x0: cycle 1399: alloc_new_valid: expected 0, got 1 [alloc=1:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `SAME_TAG_LANE1` | 0 | 0 | 9 | yes | `RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1` |
| `WAW_OLD_FROM_MAP` | 0 | 0 | 9 | yes | `RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1` |
| `NO_BYPASS` | 0 | 0 | 9 | yes | `RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1` |
| `NO_GEN_CHECK` | 1 | 1 | 2 | yes | `RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 212: free_stale: expected 1, got 0 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=1(32,1) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `NO_DUP_WB_GUARD` | 1 | 1 | 2 | yes | `RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 14: wb_accepted: expected 0, got 1 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=1(32,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `NO_DOUBLE_FREE_CHECK` | 1 | 1 | 2 | yes | `RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 171: free_accepted: expected 0, got 1 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=1(32,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `WAW_COMMIT2_PRE_MAP` | 0 | 0 | 9 | yes | `RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1` |
| `NO_BOUNDARY_CHECK` | 0 | 0 | 9 | yes | `RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1` |
| `NO_FREE_RESTORE` | 1 | 1 | 6 | yes | `RESULT FAIL rename.single_width_ownership contract violated: squash: cycle 1527: the free set differs from the shadow at tag 35` |
| `CKPT_ALLOC_LEAK` | 1 | 1 | 8 | yes | `RESULT FAIL rename.single_width_ownership contract violated: random: cycle 1890: the free set differs from the shadow at tag 39` |

Build-command stamps: every row's stamp contains exactly
`-DMOSAIC_RENAME_MUTANT_<its own name>` and no other `-DMOSAIC…` flag.
Shipping binary SHA-256: `fe8e41d9a050cdcec1b83101098ffe3fde8f93dcabeace86b05b7f4bcc55d900`; all 13 mutant binaries differ (hashes in
`/tmp/mutrun3/rename.single_width_ownership/bindiff.json`).

### Results — `rename.same_cycle_chain` (I-014's case)

Base (no define), before the campaign — exit 0, `failures=0`,
`checks=18`:

```
RESULT PASS rename.same_cycle_chain rename contract holds: 9767 shadow comparisons over 9843 cycles, 96 entries / 4 banks, seed 1
```

Rebuilt from a deleted directory after the campaign and re-run, again for
the binary-difference control, and once more as the final action of the
campaign: all reruns exit 0, `failures=0`, and print a byte-identical
`RESULT` line.

| Define | Exit | Failures | Checks | Binary ≠ shipping | RESULT line (quoted; context brackets trimmed where marked `[…]`) |
|---|---|---|---|---|---|
| `NO_EXHAUST_CHECK` | 1 | 1 | 7 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: exhaustion: cycle 1678: alloc_accepted: expected 0, got 1 [alloc=1:x20 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `NONATOMIC_GROUP` | 1 | 1 | 13 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: twowide-stall: cycle 5781: alloc_accepted: expected 0, got 1 [alloc=1:x20 alloc2=1:x21 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `X0_ALLOC` | 1 | 1 | 5 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: x0: cycle 1399: alloc_new_valid: expected 0, got 1 [alloc=1:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `SAME_TAG_LANE1` | 1 | 1 | 9 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: twowide-raw: cycle 5692: alloc2_new_tag: expected 34, got 33 [alloc=1:x5 alloc2=1:x6 rs=x9,x20 \| x5,x9 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `WAW_OLD_FROM_MAP` | 1 | 1 | 11 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: twowide-waw: cycle 5702: alloc2_old_tag: expected 32, got 7 [alloc=1:x7 alloc2=1:x7 rs=x7,x7 \| x7,x7 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `NO_BYPASS` | 1 | 1 | 9 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: twowide-raw: cycle 5692: rs3_bypass: expected 1, got 0 [alloc=1:x5 alloc2=1:x6 rs=x9,x20 \| x5,x9 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `NO_GEN_CHECK` | 1 | 1 | 2 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: ownership: cycle 212: free_stale: expected 1, got 0 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=1(32,1) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `NO_DUP_WB_GUARD` | 1 | 1 | 2 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: ownership: cycle 14: wb_accepted: expected 0, got 1 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=1(32,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `NO_DOUBLE_FREE_CHECK` | 1 | 1 | 2 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: ownership: cycle 171: free_accepted: expected 0, got 1 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=1(32,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` |
| `WAW_COMMIT2_PRE_MAP` | 1 | 1 | 11 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: twowide-waw: cycle 5706: the free set differs from the shadow at tag 34` |
| `NO_BOUNDARY_CHECK` | 1 | 1 | 16 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: twowide-ckptbad: cycle 5831: squash_accepted: expected 0, got 1 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=1]` |
| `NO_FREE_RESTORE` | 1 | 1 | 6 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: squash: cycle 1527: the free set differs from the shadow at tag 35` |
| `CKPT_ALLOC_LEAK` | 1 | 1 | 8 | yes | `RESULT FAIL rename.same_cycle_chain contract violated: random: cycle 1890: the free set differs from the shadow at tag 39` |

Build-command stamps: every row's stamp contains exactly
`-DMOSAIC_RENAME_MUTANT_<its own name>` and no other `-DMOSAIC…` flag.
Shipping binary SHA-256: `234a2424d85ebd56f00cc6497650093c8f48630c47248ea6fbbb0145be5cb3f8`; all 13 mutant binaries differ (hashes in
`/tmp/mutrun3/rename.same_cycle_chain/bindiff.json`).

### Verdict — report claims vs. re-run

#### `I-013-rename.md` §9

Claim quoted: *"For each one ... 3. the run **exits 1** with a `RESULT FAIL`
line."* and the six-row table with *"exit **1**"*; the report's verbatim
`MISMATCH`/`RESULT FAIL` blocks are the per-row claim. The case is
`rename.single_width_ownership` (the report's own run used that case).

| Mutant | Claim (quoted) | Re-run observation | Confirmed |
|---|---|---|---|
| `NO_GEN_CHECK` | `exit=1`; `MISMATCH ownership: cycle 212: free_stale: expected 1, got 0`; phase `ownership` (first), `generation`; `ifdef` 2 | exit 1, failures=1, checks=2; `RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 212: free_stale: expected 1, got 0 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=1(32,1) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `X0_ALLOC` | `exit=1`; `MISMATCH x0: cycle 1399: alloc_new_valid: expected 0, got 1`; phase `x0`; `ifdef` 1 | exit 1, failures=1, checks=5; `RESULT FAIL rename.single_width_ownership contract violated: x0: cycle 1399: alloc_new_valid: expected 0, got 1 [alloc=1:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `NO_FREE_RESTORE` | `exit=1`; `MISMATCH squash: cycle 1527: the free set differs from the shadow at tag 35`; phase `squash` | exit 1, failures=1, checks=6; `RESULT FAIL rename.single_width_ownership contract violated: squash: cycle 1527: the free set differs from the shadow at tag 35` | yes |
| `NO_EXHAUST_CHECK` | `exit=1`; `MISMATCH exhaustion: cycle 1678: alloc_accepted: expected 0, got 1`; phase `exhaustion` | exit 1, failures=1, checks=7; `RESULT FAIL rename.single_width_ownership contract violated: exhaustion: cycle 1678: alloc_accepted: expected 0, got 1 [alloc=1:x20 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `NO_DUP_WB_GUARD` | `exit=1`; `MISMATCH ownership: cycle 14: wb_accepted: expected 0, got 1`; phase `ownership` | exit 1, failures=1, checks=2; `RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 14: wb_accepted: expected 0, got 1 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=1(32,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `NO_DOUBLE_FREE_CHECK` | `exit=1`; `MISMATCH ownership: cycle 171: free_accepted: expected 0, got 1`; phase `ownership` | exit 1, failures=1, checks=2; `RESULT FAIL rename.single_width_ownership contract violated: ownership: cycle 171: free_accepted: expected 0, got 1 [alloc=0:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=1(32,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |

The base claim (line 326) is byte-identical to the re-run:

```
RESULT PASS rename.single_width_ownership rename contract holds: 5646 shadow comparisons over 5686 cycles, 96 entries / 4 banks, seed 1
```

The only textual difference is the stimulus context bracket: the live driver
prints `[alloc=… alloc2=… commit2=…]` on the `MISMATCH` lines and repeats it on
`RESULT FAIL`, while the report's captured strings predate the two-wide ports
(its `MISMATCH` bracket carries only `alloc`/`wb`/`free`/`commit`/`ckpt`/
`squash`, and its `RESULT FAIL` lines carry no bracket at all). Cycle, signal,
expected/got values, exit code and failure count are identical in all six rows.

#### `I-014-rename2w.md` §6

Claim quoted: *"Six controls ... Shipping build for reference: **exit 0, 16
checks, 0 failures, PASS**"* and the six-row table with *"Delta against the
shipping build: **0 → 1 failing check** in every case"*. The case is
`rename.same_cycle_chain`.

| Mutant | Claim (checks / failures / first mismatch) | Re-run observation | Confirmed |
|---|---|---|---|
| `NO_BYPASS` | checks 9, failures 1, exit 1; `MISMATCH twowide-raw: cycle 5692: rs3_bypass: expected 1, got 0` | exit 1, failures=1, checks=9; `RESULT FAIL rename.same_cycle_chain contract violated: twowide-raw: cycle 5692: rs3_bypass: expected 1, got 0 [alloc=1:x5 alloc2=1:x6 rs=x9,x20 \| x5,x9 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `NONATOMIC_GROUP` | checks 13, failures 1, exit 1; `MISMATCH twowide-stall: cycle 5781: alloc_accepted: expected 0, got 1` | exit 1, failures=1, checks=13; `RESULT FAIL rename.same_cycle_chain contract violated: twowide-stall: cycle 5781: alloc_accepted: expected 0, got 1 [alloc=1:x20 alloc2=1:x21 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `X0_ALLOC` | checks 5, failures 1, exit 1; `MISMATCH x0: cycle 1399: alloc_new_valid: expected 0, got 1`; phase `x0` (single-width phase); also I-013 #2 | exit 1, failures=1, checks=5; `RESULT FAIL rename.same_cycle_chain contract violated: x0: cycle 1399: alloc_new_valid: expected 0, got 1 [alloc=1:x0 alloc2=0:x0 rs=x0,x0 \| x0,x0 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `SAME_TAG_LANE1` | checks 9, failures 1, exit 1; `MISMATCH twowide-raw: cycle 5692: alloc2_new_tag: expected 34, got 33` | exit 1, failures=1, checks=9; `RESULT FAIL rename.same_cycle_chain contract violated: twowide-raw: cycle 5692: alloc2_new_tag: expected 34, got 33 [alloc=1:x5 alloc2=1:x6 rs=x9,x20 \| x5,x9 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `WAW_OLD_FROM_MAP` | checks 11, failures 1, exit 1; `MISMATCH twowide-waw: cycle 5702: alloc2_old_tag: expected 32, got 7` | exit 1, failures=1, checks=11; `RESULT FAIL rename.same_cycle_chain contract violated: twowide-waw: cycle 5702: alloc2_old_tag: expected 32, got 7 [alloc=1:x7 alloc2=1:x7 rs=x7,x7 \| x7,x7 wb=0(0,0) free=0(0,0) commit=0:x0(0,0) commit2=0:x0(0,0) ckpt=0 squash=0]` | yes |
| `WAW_COMMIT2_PRE_MAP` | checks 11, failures 1, exit 1; `MISMATCH twowide-waw: cycle 5706: the free set differs from the shadow at tag 34` | exit 1, failures=1, checks=11; `RESULT FAIL rename.same_cycle_chain contract violated: twowide-waw: cycle 5706: the free set differs from the shadow at tag 34` | yes |

All six rows check out, including the per-row `checks` totals at the point of
abort (9 / 13 / 5 / 9 / 11 / 11) and the failure delta 0 → 1. Unlike the
I-013 strings, the report's §6 `MISMATCH` lines already carry the live
`[alloc=… alloc2=… commit2=…]` context bracket, and the re-run reproduces
them character for character up to the report's own `...` truncation.

Two **staleness** findings against §5/§6 text, not against the mutant claims:

* §5 says the case runs *"the eight single-width phases **and** seven two-wide
  phases"* and reports *"9750 shadow comparisons over 9818 cycles, 16 checks"*.
  The live case runs eight single-width and **nine** two-wide phases and reports
  **9767 comparisons over 9843 cycles, 18 checks** — the two added phases are
  `twowide-ckptalloc` and `twowide-ckptbad` (they are the two checks that
  separate the 16-line `check ok:` transcript in §5 from the live 18). §6's
  *"Shipping build ... 16 checks"* and the `sha256[0:16]` prefixes are stale for
  the same reason; the prefixes cannot be reproduced because the case sources
  changed after the report, so the binary-difference control was re-established
  here with full SHA-256 hashes (all 13 differ from shipping).
* §5's `random` phase transcript (*"112 allocations, 65 accepted writebacks, 791
  stale rejections, 235 squashes, 115 exhaustion reports"*) also predates the
  case change: the live phase prints *"240 allocations, 97 accepted writebacks,
  988 stale rejections, 274 squashes, 0 exhaustion reports"*.

The two controls with no package-report claim, for completeness:

* `CKPT_ALLOC_LEAK` (control 12, the pre-I-014 "checkpoint cycle is not
  journalled" rule) is caught by **both** cases in the single-width `random`
  phase: `random: cycle 1890: the free set differs from the shadow at tag 39`
  (exit 1, failures=1, checks=8).
* `NO_BOUNDARY_CHECK` (control 13, the committed-boundary refusal removed) is
  caught by `rename.same_cycle_chain` in the new `twowide-ckptbad` phase:
  `twowide-ckptbad: cycle 5831: squash_accepted: expected 0, got 1` (exit 1,
  failures=1, checks=16). It is inert in `rename.single_width_ownership`, which
  never presents a squash to a checkpoint taken with writers in flight, and
  passes there — the phase that owns the defect is two-wide only.

### Round 3 summary

* 13 of 13 mutant defines found by grep were built and run against both cases:
  26 builds, all succeeded; 26 runs, each captured with exit code, `failures`,
  `checks` and the first mismatch.
* `rename.single_width_ownership`: 7 of 13 exit 1 with exactly 1 failure — the
  six I-013 rows plus `CKPT_ALLOC_LEAK` — and 6 pass, every one of them a
  two-wide-only define that this case never drives (`alloc2_req` is never
  asserted) or the boundary control, which needs the two-wide checkpoint phase.
* `rename.same_cycle_chain`: **all 13 exit 1** with exactly 1 failure, at the
  claimed phase and cycle in every case.
* Every mutant's build-command stamp names its own define and no other; every
  mutant binary differs from the shipping binary; every base stamp is clean.
* Base runs: before and after each campaign, a fresh rebuild for the
  binary-difference control, and a final rebuild+run — 4 per case, 8 in total;
  all exit 0 with `failures=0`, and all four runs of a case print a
  byte-identical `RESULT` line (the line `I-013-rename.md` quotes for
  `rename.single_width_ownership`; `rename.same_cycle_chain` moved from the
  report's 16-check line to the live 18-check line, see above).
* **Package reports confirmed:** `I-013-rename.md` §9 — **confirmed**, 6/6 rows,
  base line byte-identical. `I-014-rename2w.md` §6 — **confirmed**, 6/6 rows
  including check counts and first mismatches; its §5/§6 base summary numbers
  (16 checks, 9750/9818, binary prefixes) are stale relative to the live case,
  which is a documentation lag from the later `twowide-ckptalloc`/`twowide-ckptbad`
  work, not a mutant-claim discrepancy. No mutant behaves differently from its
  report.

