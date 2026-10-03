# tests/programs/vec — self-checking RVV programs

This directory holds the vector programs the scalar corpus cannot hold. They are
**not** the `p01..p13` firmware corpus and are deliberately kept out of it.

## Why they are not in `tests/programs/src/`

`tests/programs/` and its goldens change only through
`tools/host_oracle.py --record`, and `tools/host_oracle.py` is an 806-line
**scalar** RV64IM reference: `grep -cE "vadd|vsetvli|VECTOR|OP_V"
tools/host_oracle.py` returns 0. It models no vector register file, no `vtype`,
no vector mask, and therefore cannot produce a golden signature for a vector
program. A vector program dropped into `src/` could not be oracle-recorded, and
recording a value the oracle did not compute would make the golden file lie.

So these programs live in their own directory, the same way the malformed-image
fixtures in `tests/programs/termination/` do:

* they are **not** listed in `tests/programs/corpus.json`;
* they are **not** built by `make -C tests/programs all`;
* they do **not** touch `tests/programs/golden.json` or `tools/host_oracle.py`;
* they are built by `make -C tests/programs/vec all` into
  `tests/programs/vec/build/`.

Because no reference in this project models RVV, each program's expected values
are **hand-derived** from the RVV specification and written into the program as
constants; the program computes the result on the DUT and compares. The human
derivation is the oracle here. That is the honest substitute for a reference
model — not a convenience — and it is why these programs are evidence of
**execution and arithmetic**, not of **conformance**: a wrong hand derivation
would pass or fail for the wrong reason, and nothing here cross-checks the
derivation against an independent model of RVV.

## The machine these programs target

The delivered vector engine is the one `rtl/core/mosaic_core.sv` wires today.
Its capability word `VEC_ALU_CAPS` advertises **sixteen** of the seventeen ALU
families — `ADDSUB` (0), `WIDE` (1), `MUL` (2), `MULW` (3), `SHIFT` (4),
`LOGIC` (6), `MINMAX` (7), `CMP` (8), `SAT` (9), `MASKLOG` (10), `MASKPFX` (11),
`SLIDE` (12), `GATHER` (13), `COMPRESS` (14), `REDUCE` (15) and `REDWIDE` (16)
— and the vector load/store unit advertises unit-stride only
(`VEC_LSU_CAPS = 8'b0000_0001`). Only those are exercised here. The seventeenth,
`NARROW` (5), is deliberately **not** advertised: the delivered ALU and its
unit-level model both truncate the narrowing source to SEW, so the family's
defining `2*SEW` behavior is not proven (see
`results/reports/vector-family-advertisement.md`). A family the core does not
advertise is refused at execution, so a program for one would trap and a
trapping program is not evidence.

* VLEN = 128 bits, ELEN = 64 bits (the value `CORE_VEC_VLEN` hardcodes and
  `config/geometry/p2.json` declares).
* `mstatus.VS` is Off at reset, so `crt0.S` sets it to Initial before the first
  `vsetvli`; a `vsetvli` with VS = Off is an illegal instruction.
* A vector load/store's EEW must equal the current SEW; the core refuses a
  mismatch as an illegal instruction. A mask register is therefore loaded under
  SEW = 8 (the packed 8-bit mask layout) and the `vsetvli` is changed afterwards.

### The vtype SEW encoding

The `vsew` field bits [5:3] use the RVV 1.0 `SMALLEST_SEW` encoding: 0/1/2/3 for
e8/e16/e32/e64, so a plain GAS `vsetvli rd, rs1, e32, m1` encodes exactly the
value the programs pass. The programs still pass the raw 11-bit `vtypei` so the
assumption is explicit:

* `0x00` (e8), `0x08` (e16), `0x10` (e32), `0x18` (e64), all with `vlmul = m1`,
  `vta = vma = 0`.

(An earlier revision of the core encoded the field as log2(SEW), 3..6; that was
fixed to the ratified encoding, and these programs were updated to match. See
`results/reports/vector-selfcheck-corpus.md` for the history.)

## The programs

| program | capability | SEW / VL | what it checks |
|---|---|---|---|
| `v01_addsub_e64` | ADDSUB: `vadd.vv`, `vsub.vv`, `vadd.vx`, `vsub.vx` | e64 / 2 | 64-bit add/sub and wraparound, vector-vector and vector-scalar |
| `v02_addsub_e32` | ADDSUB: `vadd.vv`, `vsub.vv`, `vadd.vi`, `vsub.vx` | e32 / 4 | 32-bit wrap at `0xFFFFFFFF`/`0x80000000`, immediate and scalar forms |
| `v03_logic_e16` | LOGIC: `vand.vv`, `vor.vv`, `vxor.vv`, `vand.vx`, `vor.vi`, `vxor.vi` | e16 / 8 | all three bitwise ops, vector-vector, scalar and immediate |
| `v04_masklog_e8` | MASKLOG: all eight `vmand/vmnand/vmor/vmnor/vmxor/vmxnor/vmandn/vmorn` | e8 / 16 | two source-mask pairs over 16 elements |
| `v05_maskpfx_e8` | MASKPFX: `vmsbf.m`, `vmsif.m`, `vmsof.m` | e8 / 16 | eight source patterns incl. first set at 0/7/8/15 and the all-zero row |
| `v06_loadstore` | vector load/store, unit-stride | e8/e16/e32/e64 | byte-exact round trip at every wired width, plus a masked store |
| `v08_masked_addsub_e32` | ADDSUB masked (`v0.t`) | e32 / 4 | masked-off elements undisturbed (`vma = 0`) under a seeded destination |
| `v10_wide_e32` | WIDE: `vwaddu.vv`, `vwadd.vv`, `vwsubu.vx`, `vwsub.vx` | e32 / 4, dst e64 | widening add/sub, signed and unsigned, `.vv` and `.vx` |
| `v11_mul_e32` | MUL: `vmul.vv`, `vmulh.vv`, `vmulhu.vv`, `vmulhsu.vx` | e32 / 4 | low/high-half products, all four signednesses |
| `v12_mulw_e16` | MULW: `vwmulu.vv`, `vwmulsu.vv`, `vwmul.vv` | e16 / 8, dst e32 | widening products, all three signednesses |
| `v13_shift_e32` | SHIFT: `vsll.vv`, `vsrl.vv`, `vsra.vv`, `vsll.vx` | e32 / 4 | logical and arithmetic shifts, vector and scalar amounts |
| `v15_minmax_e32` | MINMAX: `vminu/vmin/vmaxu/vmax.vv` | e32 / 4 | signed and unsigned min/max at every element |
| `v16_cmp_e32` | CMP: all eight `.vv` compares | e32 / 4 | each mask, stored and compared byte-exact |
| `v17_sat_e32` | SAT: `vsaddu/vsadd/vssubu/vssub/vaaddu/vaadd/vasubu/vasub.vv` | e32 / 4, vxrm = rdn | saturation and the `2*SEW` averaging forms |
| `v18_slide_e32` | SLIDE: `vslideup.vx`, `vslidedown.vx` | e32 / 4 | both offsets, including the unwritten `vslideup` prefix |
| `v19_gather_e32` | GATHER: `vrgather.vv` | e32 / 4 | in-range reads and an out-of-range index yielding 0 |
| `v20_compress_e8` | COMPRESS: `vcompress.vm` | e8 / 16 | a mask that packs elements across two mask bytes |
| `v21_reduce_e32` | REDUCE: all eight `.vs` reductions | e32 / 4 | each fold from `vs1[0]`, result in `vd[0]` |
| `v22_redwide_e16` | REDWIDE: `vwredsumu.vs`, `vwredsum.vs` | e16 / 8 | widening reduction, signed and unsigned |
| `v07_control_wrong_addsub` | **negative control** | e64 / 2 | a copy of `v01` with one wrong expected constant; not part of the registered run |

`v07` is built but is not in the Makefile's `PROGRAMS` list nor in the case
driver's default program list. `tools/run_vector_selfcheck_controls.py` runs it
alone and requires the case to fail and name the first failure.

## Build and run

```
make -C tests/programs/vec all            # build every ELF into build/
make -C tests/programs/vec dis            # disassemble
python3 tools/run_vector_selfcheck_controls.py --profile p2
```

The case itself is `vec.selfcheck_corpus`, driven by
`sim/unit/tb_core_vecselfcheck.cpp`; see `results/reports/vector-selfcheck-corpus.md`
for the result line, the control and the not-covered list.
