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
