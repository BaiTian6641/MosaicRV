# The vector ALU families the core advertises, and the evidence for each

**STATUS: COMPLETE.** This package advertises **twelve more** of the vector
ALU families, bringing the core's word to **sixteen of the seventeen**; the last
one (NARROW) is **not** advertised because reading its stimulus shows the
delivered evidence never drives the `2*SEW` source that defines the family. The
decode gained arms for the twelve from `riscv-opcodes`' `rv_v` extension, the
registered core-level case `vec.selfcheck_corpus` now runs twelve new
self-checking programs on the integrated core, and one mutant control per new
family shows each capability bit is load-bearing.

```
RESULT PASS vec.selfcheck_corpus 19/19 self-checking RVV programs reached the pass trap
```

`rtl/core/mosaic_core.sv`'s advertised word:

```
localparam logic [16:0] VEC_ALU_CAPS = 17'h1FFDF;   // sixteen families, all but NARROW (bit 5)
```

---

## 1. The family/bit/case table, from the stimulus

The unit-level case for every one of these families is
`rvv.integer_mask_permute` (`tests/unit/registry.json`, top `mosaic_vec_tb`,
`sim/unit/tb_vec.cpp`); the evidence below is what its **stimulus** drives, read
from the source, not from the case's name. `Vec`'s per-element `Cycle` and
per-packet `RunPacket` are compared against the host oracle `OracleElem` (the
same file) or against `ComputeExpected`.

| bit | family | operations the stimulus drives | verdict |
|-----|--------|--------------------------------|---------|
| 1 | WIDE | `PhaseElementLane`: ops 0-3 = `vwaddu/vwadd/vwsubu/vwsub`, SEW 8/16/32, forms `.vv` and `.vx`, against `OracleElem`; `PhaseCoverage` re-checks op 0 as a packet | **advertise** |
| 2 | MUL | `PhaseElementLane`: ops 0-3 = `vmul/vmulh/vmulhu/vmulhsu`, SEW 8..64, `.vv`/`.vx` | **advertise** |
| 3 | MULW | `PhaseElementLane`: ops 0-2 = `vwmulu/vwmulsu/vwmul`, SEW 8..32, `.vv`/`.vx` | **advertise** |
| 4 | SHIFT | `PhaseElementLane`: ops 0-2 = `vsll/vsrl/vsra`, SEW 8..64, `.vv`/`.vx` | **advertise** |
| 5 | NARROW | `PhaseElementLane` names ops 0-3, but it feeds `vs2 = vals[xi]` (SEW-bounded) and `OracleElem` masks the source to SEW (`uint64_t a = vs2 & MaskW(sew)`); `PhaseCoverage` primes a `2*SEW` source but compares against the *same* SEW-masked model. The `2*SEW` source that defines `vnsrl/vnsra/vnclip` is therefore never driven through a check, and the ALU truncates it too (`mosaic_vec_alu.sv:542`, `bb = ef_vs2 & width_mask(sew)`) | **do not advertise** |
| 7 | MINMAX | `PhaseElementLane`: ops 0-3 = `vminu/vmin/vmaxu/vmax`, SEW 8..64, `.vv`/`.vx` | **advertise** |
| 8 | CMP | `PhaseElementLane`: ops 0-7 = `vmseq/vmsne/vmsltu/vmslt/vmsleu/vmsle/vmsgtu/vmsgt`, SEW 8..64, `.vv`/`.vx`; `PhaseCoverage` exercises the masked form | **advertise** |
| 9 | SAT | `PhaseElementLane`: ops 0-7 = `vsaddu/vsadd/vssubu/vssub` and `vaaddu/vaadd/vasubu/vasub`, SEW 8..64, `.vv`/`.vx`, with `vxrm` 2 (rdn) and 0 (rnu) | **advertise** |
| 12 | SLIDE | `PhasePermuteLane`: ops 0-1 = `vslideup/vslidedown`, form `.vx`, checks the read index (`el_rd2`) and access for every destination element; `PhaseCoverage` checks op 0's destination values | **advertise** |
| 13 | GATHER | `PhasePermuteLane`: op 0 = `vrgather.vv`, checks `el_rd2`/access including the out-of-range index; `PhaseCoverage` checks op 0's values | **advertise** |
| 14 | COMPRESS | `PhasePermuteLane`: op 0 = `vcompress`, checks the ascending source order; `PhaseCoverage` checks op 0's values | **advertise** |
| 15 | REDUCE | `PhaseReduceLane`: ops 0-7 = `vredsum/vredand/vredor/vredxor/vredminu/vredmin/vredmaxu/vredmax`, SEW 8..64, each fold step against `OracleElem` with the accumulator from `vs1[0]` | **advertise** |
| 16 | REDWIDE | `PhaseReduceLane`: ops 0-1 = `vwredsumu/vwredsum`, SEW 8..32, `2*SEW` accumulator | **advertise** |

Why NARROW is the one refusal: the family is defined by narrowing a `2*SEW`
source, and neither the stimulus's model nor the ALU ever consumes the upper
half of `vs2` — the ALU masks `vs2` to SEW for every family
(`rtl/core/mosaic_vec_alu.sv:539-542`) and then re-widens it (`sext`/`uext`),
so `vnsra` is always a logical shift and `vnsrl`/`vnclipu`/`vnclip` ignore the
wide bits. The delivered evidence agrees with that implementation because it
uses the same truncation, so it proves the delivered behavior but not the
family's defining behavior. A program that used a genuine `2*SEW` source was
written and is what surfaced this (`v14_narrow_e32` was removed; its failure at
byte offset 0 is the observation).

Bits 0 (ADDSUB), 6 (LOGIC) stay as they were (`vec.integrated`); 10 (MASKLOG)
and 11 (MASKPFX) stay as recorded in `results/reports/mask-prefix-at-core.md`.

---

## 2. The decode arms added

Before this change the OP-V decode handled only OPIVV/OPIVX/OPIVI for
`vadd/vsub/vand/vor/vxor`, OPMVV for the two mask families, and vector
loads/stores — so twelve implemented families had **no decode arm at all** and
were unreachable even with their capability bit set. The arms added, their
encodings from `riscv-opcodes`' `rv_v` extension (funct6 = `riscv-opcodes`
`31..26`; the ALU operation index is the one in `rvv.integer_mask_permute` and
`mosaic_vec_alu.sv`):

| funct3 class | funct6 → family/op |
|---|---|
| OPIVV `000` / OPIVX `100` | `0x04/0x05/0x06/0x07` → MINMAX 0/1/2/3; `0x18..0x1f` → CMP 0..7; `0x20/0x21/0x22/0x23` → SAT 0/1/2/3; `0x25/0x28/0x29` → SHIFT 0/1/2 |
| OPIVV `000` only | `0x0c` → GATHER 0 (`vrgather.vv`); `0x30/0x31` → REDWIDE 0/1 (`vwredsumu/vwredsum.vs`) |
| OPIVX `100` only | `0x0e/0x0f` → SLIDE 0/1 (`vslideup/vslidedown.vx`) |
| OPMVV `010` only | `0x00/0x01/0x02/0x03/0x04/0x05/0x06/0x07` → REDUCE 0/5/6/7/3/4/1/2; `0x17` (vm=1) → COMPRESS 0 |
| OPMVV `010` / OPMVX `110` | `0x08/0x09/0x0a/0x0b` → SAT 4/5/6/7; `0x24/0x25/0x26/0x27` → MUL 2/0/3/1; `0x30..0x33` → WIDE 0..3; `0x38/0x3a/0x3b` → MULW 0/1/2 |

Descriptor classes were set to the operation's shape so the descriptor's
legality matrix is the right one: CMP → `VOP_VMASK` (mask destination),
WIDE/MULW → `VOP_VWIDE` (`2*SEW` destination, no partial source overlap),
MUL → `VOP_VMUL`, REDUCE/REDWIDE → `VOP_VRED` (`vd[0]`, no source overlap),
SLIDE/GATHER/COMPRESS → `VOP_VSLIDE`, everything else → `VOP_IVV`.

**Refused rather than guessed.** Every encoding without delivered evidence falls
through to `default` and is refused at decode: the `.vi` forms of the added
families; `vrgather.vx/.vi`; `vrgatherei16.vv` (`0x0e` OPIVV); the
`vslide1up/vslide1down` OPMVX forms (`0x0e/0x0f`, OPMVX — no ALU family
implements them); the `.wv`/`.wx` widening mixtures (`0x34..0x37`); the reserved
`vm=0` encodings of `vcompress` and the mask-logical ops; `viota/vid`,
`vzext/vsext`, `vmv.x.s`, divide/remainder and the multiply-accumulate forms;
and the NARROW arms `0x2c..0x2f` (no advertised family).

---

## 3. The core-level case and its checks

The registered case is **`vec.selfcheck_corpus`** (`tests/unit/registry.json`,
task `I-054`, profile `p2`, top `mosaic_core_tb`, driver
`sim/unit/tb_core_vecselfcheck.cpp`). It was extended, not replaced: twelve new
self-checking programs were added under `tests/programs/vec/src/` and their
names added to the driver's `kPrograms[]` and the corpus `Makefile`'s
`PROGRAMS`. The case now runs 19 programs and requires every one to reach its
PASS trap, with at least one vector macro retired, no vector trap and no vector
fault.

Each program is a directed RV64I+V program with **hand-derived** expected values
carried in its `.rodata` (the corpus's documented oracle — the human derivation;
see `results/reports/vector-selfcheck-corpus.md` for that weakness). The unit
reference `OracleElem` (`sim/unit/tb_vec.cpp`) is not reusable from a freestanding
assembly program, so the second option the assignment allows was taken: the
self-checking style with hand-derived constants. The constants were derived from
the RVV rules and cross-checked with a throwaway arithmetic aid (since deleted);
a wrong constant cannot pass silently, because the DUT is correct and the
program's own byte comparison would fail loudly.

| program | family | configuration | operations | checks |
|---|---|---|---|---|
| `v10_wide_e32` | WIDE | e32/m1, VL 4, dst e64/m2 | `vwaddu.vv`, `vwadd.vv`, `vwsubu.vx`, `vwsub.vx` | 16 64-bit elements |
| `v11_mul_e32` | MUL | e32/m1, VL 4 | `vmul.vv`, `vmulh.vv`, `vmulhu.vv`, `vmulhsu.vx` | 16 words |
| `v12_mulw_e16` | MULW | e16/m1, VL 8, dst e32/m2 | `vwmulu.vv`, `vwmulsu.vv`, `vwmul.vv` | 24 words |
| `v13_shift_e32` | SHIFT | e32/m1, VL 4 | `vsll.vv`, `vsrl.vv`, `vsra.vv`, `vsll.vx` | 16 words |
| `v15_minmax_e32` | MINMAX | e32/m1, VL 4 | `vminu/vmin/vmaxu/vmax.vv` | 16 words |
| `v16_cmp_e32` | CMP | e32/m1, VL 4; stored under e8 | all eight `.vv` compares | 8 masks × 16 bytes |
| `v17_sat_e32` | SAT | e32/m1, VL 4, vxrm = rdn | all eight `.vv` avg/sat ops | 32 words |
| `v18_slide_e32` | SLIDE | e32/m1, VL 4 | `vslideup.vx` (2), `vslidedown.vx` (1) | 8 words |
| `v19_gather_e32` | GATHER | e32/m1, VL 4 | `vrgather.vv` (incl. out-of-range index) | 4 words |
| `v20_compress_e8` | COMPRESS | e8/m1, VL 16 | `vcompress.vm` | 16 bytes |
| `v21_reduce_e32` | REDUCE | e32/m1, VL 4 | all eight `.vs` reductions | 8 words |
| `v22_redwide_e16` | REDWIDE | e16/m1, VL 8 | `vwredsumu.vs`, `vwredsum.vs` | 2 words |

The programs use `vtypei` from `tests/programs/vec/vec.h` (`VTYPE_E8_M1` 0x00,
`_E16` 0x08, `_E32` 0x10, `_E64` 0x18), i.e. the **RVV 1.0** encoding that
`FixVsewEncoding` landed: `vsew = log2(SEW) - 3`, width `vsew + 3`. No `vsew`
assumption of this package's own is encoded anywhere.

Reading the stimulus also corrected two of the hand derivations before they
shipped: `vasubu` extends both operands to `2*SEW` before the right shift (so
`0x80000000 - 0xFFFFFFFF` truncates to `0xC0000000`, not the 32-bit-modular
`0x40000000`), and `vxrm`/`vcsr` must be set to `0x4` (*rdn* = 2), not `0x6`
(*rod* = 3) — both were caught by the case failing with a named byte offset.

---

## 4. Controls

`tools/run_vector_family_caps_controls.py` (new) builds two binaries **from
deleted directories** — the shipping DUT and a mutant with
`-DMOSAIC_CORE_MUTANT_VEC_CAPS_NEW_OFF`, which clears all twelve newly
advertised family bits — and requires their sha256 to differ:

```
shipping driver sha256: 4ee9d00beaacbae4a0600daa6b9e504a7a8a6a35e4d438ee1bf4af633c9fbad5
mutant   driver sha256: 978ea74ce9247349887ad8aee234ba333b7d930542bf4c7e35d7867cbb65794e
```

The baseline requires the shipping case to PASS 19/19. Then, for each advertised
family, it runs that family's program **alone** against both binaries. Each
program uses exactly one of the cleared families, so running it alone against
the all-cleared mutant isolates that family's capability bit:

| program | family | shipping | mutant (exit 1, named) |
|---|---|---|---|
| `v10_wide_e32` | WIDE | PASS | `CHECK FAILED: v10_wide_e32: … trapped: mcause=2` |
| `v11_mul_e32` | MUL | PASS | `CHECK FAILED: v11_mul_e32: … mcause=2` |
| `v12_mulw_e16` | MULW | PASS | `CHECK FAILED: v12_mulw_e16: … mcause=2` |
| `v13_shift_e32` | SHIFT | PASS | `CHECK FAILED: v13_shift_e32: … mcause=2` |
| `v15_minmax_e32` | MINMAX | PASS | `CHECK FAILED: v15_minmax_e32: … mcause=2` |
| `v16_cmp_e32` | CMP | PASS | `CHECK FAILED: v16_cmp_e32: … mcause=2` |
| `v17_sat_e32` | SAT | PASS | `CHECK FAILED: v17_sat_e32: … mcause=2` |
| `v18_slide_e32` | SLIDE | PASS | `CHECK FAILED: v18_slide_e32: … mcause=2` |
| `v19_gather_e32` | GATHER | PASS | `CHECK FAILED: v19_gather_e32: … mcause=2` |
| `v20_compress_e8` | COMPRESS | PASS | `CHECK FAILED: v20_compress_e8: … mcause=2` |
| `v21_reduce_e32` | REDUCE | PASS | `CHECK FAILED: v21_reduce_e32: … mcause=2` |
| `v22_redwide_e16` | REDWIDE | PASS | `CHECK FAILED: v22_redwide_e16: … mcause=2` |

The named first failure on the mutant is the illegal-instruction trap
(`mcause=2`) the ALU's capability gate raises when the bit is clear, which is
exactly the claim: the family is refused without its bit. `make unit
… CASE=vec.selfcheck_corpus` does not rebuild the mutant; the control tool
does, from a deleted directory, on both builds.

---

## 5. Re-verification

| gate | result |
|---|---|
| `vec.selfcheck_corpus` (p2, 19 programs) | **PASS 19/19** |
| `vec.mask_prefix_at_core` (p0 and p1) | PASS |
| `rvv.descriptor_legality` (p0) | PASS |
| `core.corpus_sweep` (p0) | PASS |
| `core.act_dut` (p0) | **FAIL — pre-existing, not caused by this change** (see below) |
| `tools/run_vector_family_caps_controls.py --profile p2` | exit 0, 12/12 families isolated |
| `lint_rtl --profile p0` / `--profile p1` | **65/65 clean** each |
| `check_records` | ok (84 packages, 101 cases) |
| `check_exclusions` | ok (54 registered, 18 open, 36 covered) |
| `check_contracts --all` | p0…p3 contracts OK |
| `host_oracle.py --all` / `--check-golden-only` | ok |

`core.act_dut` fails on all 127 **scalar** ACT ELFs (`first_fail=…/rv64i/I/I-add-00.elf`,
trap in M-mode) in the current tree. It is not caused by this change: the ACT
corpus is scalar RV64I, the change adds vector decode arms inside the `OP_V`
opcode arm and one capability literal, and the failure is the same one recorded
as pre-existing in `results/reports/mask-prefix-at-core.md` after the
intervening front-end/LLB work.

---

## 6. Not covered

* **NARROW (bit 5) is not advertised** and its decode arms are not present. The
  delivered unit evidence and the ALU both truncate the `2*SEW` source to SEW
  (`mosaic_vec_alu.sv:539-542`), so `vnsrl`/`vnsra`/`vnclipu`/`vnclip` do not
  consume a wide source; `vnsra` is therefore always a logical shift. This is a
  finding about the delivered ALU, found by reading the stimulus; the family
  should be advertised only after the wide-source path is implemented and
  covered.
* **No implementation at all** (no ALU family bit, no decode): integer divide
  and remainder; the multiply-accumulate forms; scaling shifts
  (`vssrl`/`vssra`); `vsmul`; integer extension (`vzext`/`vsext`); `viota`/`vid`;
  `vrgatherei16`; `vmerge`/`vmv*` and the scalar-vector moves; every vector FP
  class; and every vector memory class other than unit-stride.
* **Unproven forms of advertised families are refused, not decoded**: the `.vi`
  forms; `vrgather.vx/.vi`; `vslide1up`/`vslide1down` (the ALU's SLIDE ops 2/3,
  which the delivered stimulus does not drive); the `.wv`/`.wx` widening
  mixtures; `vm=0` on `vcompress` and the mask-logical ops.
* **Masked forms at the core level**: the `vm` bit is decoded for every new
  family and the destination policies follow it, but the twelve new core-level
  programs are unmasked. Masked destination policies are covered at unit level
  (`PhaseCoverage` runs masked packets); they are not covered end-to-end at the
  core.
* **Width/configuration corners**: each core-level program exercises one SEW/
  LMUL shape (listed in §3); the unit case sweeps SEW 8..64 and LMUL. Widening
  shapes stop at `2*SEW = ELEN` as the descriptor requires.
* **`p0`/`p1`/`p3` for `vec.selfcheck_corpus`**: the case is registered for
  `p2` only and `run_unit` refuses the other profiles; the vector engine is
  present in all profiles (`CORE_VEC_VLEN` is hardcoded), but the registered
  run is `p2`.
* **The oracle's nature**: the twelve new programs check against hand-derived
  constants, not a reference model (the project has no RVV reference model). A
  wrong derivation fails loudly rather than passing silently, but it is a weaker
  oracle than a model; this is the corpus's documented limitation, unchanged.

---

## 7. Files

* **modified** `rtl/core/mosaic_core.sv` — the `VEC_ALU_CAPS` word
  (`17'h1FFDF`), its evidence comment, the new OPIVV/OPIVX/OPIVI decode arms,
  the new OPMVV/OPMVX decode arms (including the new OPMVX funct3 class), the
  `MOSAIC_CORE_MUTANT_VEC_CAPS_NEW_OFF` control, and the widened
  `MOSAIC_CORE_MUTANT_VEC_CAPS_NO_MASKPFX` literal. Nothing outside the
  capability literal and the vector decode `case` was touched.
* **new** `tests/programs/vec/src/v10_wide_e32.S`, `v11_mul_e32.S`,
  `v12_mulw_e16.S`, `v13_shift_e32.S`, `v15_minmax_e32.S`, `v16_cmp_e32.S`,
  `v17_sat_e32.S`, `v18_slide_e32.S`, `v19_gather_e32.S`, `v20_compress_e8.S`,
  `v21_reduce_e32.S`, `v22_redwide_e16.S`.
* **modified** `tests/programs/vec/Makefile` — the twelve names added to
  `PROGRAMS`.
* **modified** `sim/unit/tb_core_vecselfcheck.cpp` — the twelve names added to
  `kPrograms[]` (the driver's structure is unchanged).
* **new** `tools/run_vector_family_caps_controls.py` — the twelve family
  capability controls.
* **not edited**: `tests/unit/registry.json`,
  `config/status/implementation_status.json`, `results/PROGRESS.md`,
  `rtl/core/mosaic_vec_alu.sv`, `tests/programs/src/`,
  `tests/programs/golden.json`, `tools/host_oracle.py`.
