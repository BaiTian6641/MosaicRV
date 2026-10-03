# Decoder testbench layout widening: `decode_ctl_t` 143 -> 299 bits

Work item: fix `CASE=decode.rv64im_reserved`, whose wrapper and driver still
broke `mosaic_pkg::decode_ctl_t` out as the old 143-bit layout while the package
had grown to 299 bits. `rtl/core/mosaic_pkg.sv` was **not** changed; the widened
struct is the delivered vector work's and the testbench was the stale side.

Owned files changed: `sim/tb/mosaic_decoder_tb.sv`, `sim/unit/tb_decoder.cpp`.
Report: this file.

## 1. Width before and after

| | width | Verilator `o_ctl_bits` |
|---|---|---|
| before | 143 | `VL_OUTW(&o_ctl_bits, 142, 0, 5)` (5 x 32-bit words) |
| after | 299 | `VL_OUTW(&o_ctl_bits, 298, 0, 10)` (10 x 32-bit words) |

299 is the exact sum of the package's declaration-order field widths (each enum
taken at its declared width: `alu_op_e` 4, `md_op_e` 3, `mem_kind_e` 3,
`amo_op_e` 4, `fp_op_e` 5, `csr_op_e` 2).

The 143-bit point is not the last `decode_ctl_t` the driver knew about plus a
single widening: it is the width at commit `5665fa7` (I-040, the last time
`sim/tb/mosaic_decoder_tb.sv` moved). Between then and now the package gained
**156** bits, of which the vector block is 135:

| added | bits | where it sits (MSB-first bit range) |
|---|---|---|
| I-044 privilege/fetch fields: `is_sret`, `is_fetch_fault`, `is_sfence_vma`, `sfence_has_va`, `sfence_has_asid` | 5 | `[173:172]`, `[170:168]` |
| I-050 F/D fields: `is_fp`, `fp_op`, `fp_fmt`, `fp_rm`, `fp_dst_fp`, `fp_src1_fp`, `fp_src2_fp`, `fp_iw`, `fp_is`, `fp_modifies_state` | 16 | `[150:135]` |
| I-059 vector block: `is_vec` .. `vec_imm` | 135 | `[134:0]` |

The vector block is the struct's **least-significant** 135 bits, contiguous,
starting at bit 0: `vec_imm` is `[63:0]` on a 64-bit boundary and `is_vec` is
`[134]`. That alignment is why the driver's word unpacking has to grow by
exactly the new tail (the top 43 bits of the fifth 64-bit half) rather than
shifting every pre-existing field: nothing above bit 135 moved.

The decoder drives none of the 156 new bits. SRET, the fetch-fault macro,
WFI, SFENCE.VMA, OP-FP and OP-V are all recognised by the core's front end, not
by `mosaic_decoder` (that ownership split is stated in the package comments and
is what `CASE=decode.rv64im_reserved` asserts). Grepping
`rtl/core/mosaic_decoder.sv` for `is_sret|is_fetch_fault|is_wfi|is_sfence|
is_fp|is_vec|vec_` returns nothing, so all of them stay at `CTL_ILLEGAL`'s
values (`'0`, with the enums named: `fp_op` = `FP_ADD`, `amo_op` = `AMO_ADD`,
etc.). The driver therefore pushes them as explicit constants, exactly as the
existing A-extension fields were already handled.

## 2. The three driver changes

1. **`kTotalBits` and the derived field list.** `const int kTotalBits = 143;`
   -> `299`, and the `push(...)` list was rebuilt from
   `rtl/core/mosaic_pkg.sv`'s declaration order (MSB first), not from the old
   list plus a guess. Inserted after `is_mret`: `is_sret`, `is_fetch_fault`
   (before `is_wfi`), then `is_sfence_vma`, `sfence_has_va`,
   `sfence_has_asid`; after `csr_imm_form`: the 16 F/D fields; after those: the
   135 vector bits, ending with `push(0, 64)` for `vec_imm`. Each new group
   carries a comment naming the extension (I-044/I-046/I-050/I-059) and why it
   is a constant. `rep_.Check(nbits == kTotalBits, ...)` is the completeness
   gate: `nbits` reaches exactly 299 only if every field is accounted for, so a
   forgotten field fails even before the bit comparison.

2. **DUT-side unpacking.** 299 bits are ten 32-bit Verilator words, grouped as
   five 64-bit halves: `dut_lo = w0|w1<<32`, `dut_m0 = w2|w3<<32`,
   `dut_m1 = w4|w5<<32`, `dut_m2 = w6|w7<<32`, and
   `dut_top = (w8|w9<<32) & 0x7FFFFFFFFFF` (the top half carries only the
   struct's top 43 bits `[298:256]`, so it is masked to `(1<<43)-1`).

3. **Locally built `chunk[]` side.** `uint64_t chunk[3]` -> `uint64_t chunk[5]`,
   and `my_top = chunk[4] & 0x7FFFFFFFFFF` so both sides are compared over the
   same 299 bits. The mismatch diagnostic now prints all five words and the
   named check still reads `testbench break-out vs decode_ctl_t`.

## 3. The control

A layout check nobody has seen fail is not evidence that it checks anything, so
the break-out itself was perturbed for one build and then reverted:

```diff
-  assign o_ctl_bits = ctl;
+  assign o_ctl_bits = ctl ^ 299'd1;  // CONTROL: temporary perturbation
```

That flips struct bit 0 (`vec_imm[0]`, a bit the decoder always leaves 0). The
run failed on the first instruction, with the named mismatch:

```
MISMATCH insn=0xfe530823 (testbench break-out vs decode_ctl_t): expected
0x0000000000000000|0x0000000000000000|0x0000000000000000|0xfffffffff8048000|0x000005cc507fffff,
got
0x0000000000000001|0x0000000000000000|0x0000000000000000|0xfffffffff8048000|0x000005cc507fffff
CHECK FAILED: testbench break-out does not match decode_ctl_t
CHECK FAILED: testbench break-out does not match decode_ctl_t at insn=0xfe530823
RESULT FAIL decode.rv64im_reserved 1 instructions compared, 2 failure(s)
```

The perturbation was reverted (working tree has the plain `assign o_ctl_bits =
ctl;`); no permanent mutant was left behind.

## 4. Re-run verdicts

`python3 tools/run_unit.py --profile p0 --case decode.rv64im_reserved`, from a
deleted build directory, RESULT line verbatim:

```
RESULT PASS decode.rv64im_reserved 166734 instructions (82915 legal, 83819 illegal), 0 mismatches, 1442 named reserved checks
```

Other gates:

* `python3 tools/lint_rtl.py --profile p0` -> `lint: 65 source file(s) clean`
* `python3 tools/lint_rtl.py --profile p1` -> `lint: 65 source file(s) clean`
* `python3 tools/check_records.py` -> `ok records agree: 85 delivered
  package(s), 102 registered case(s), every claimed case exists and belongs to
  the package claiming it`
* `python3 tools/run_unit.py --profile p1 --case core.act_dut` -> now green:

```
RESULT PASS core.act_dut applicable=127 generated=127 run=127 passed=127 failed=0 act_commit=96493a91448ca50780013fd892daec2c204487ba config=tests/act4/mosaic-p1/test_config.yaml
```

(The earlier 0/127 was concurrent-work noise; this re-run is 127/127, so no
first-failure report is owed.)
