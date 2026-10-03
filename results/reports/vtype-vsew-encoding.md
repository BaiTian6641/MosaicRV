# `vtype.vsew` stored the wrong value: the log2(SEW) encoding instead of RVV 1.0's

Work package: FixVsewEncoding. Owned files: `rtl/core/mosaic_vec_cfg.sv`, the
`vsew` readers in `rtl/core/mosaic_vec_desc.sv`, `mosaic_vec_alu.sv`,
`mosaic_vec_lsu.sv`, `mosaic_vec_fp.sv`, `mosaic_vec_restart.sv` and
`rtl/core/mosaic_core.sv`, `config/csr/vector.json`, and the affected
cases/controls (`sim/unit/tb_vec.cpp`, `sim/unit/tb_core_vec.cpp`,
`sim/unit/tb_multihart_ctx.cpp`, `sim/unit/tb_core_perf.cpp`,
`tools/run_vec_layout_controls.py`).

## The defect

RVV 1.0 encodes `vtype.vsew[2:0]` as **log2(SEW) − 3**: `0 = e8, 1 = e16,
2 = e32, 3 = e64`; `4..7` are reserved. The field is architecturally visible —
software reads `vtype` back and decodes SEW from it, and libraries derive VLMAX
from it. `mosaic_vec_cfg` instead treated the field as **log2(SEW) unshifted**
(`vsew_ok` accepted `3..6`, `vlmax_of` computed `2^(VLEN_LOG2 + lmul_exp − vsew)`)
and every other unit decoded it the same way. So the machine (a) rejected the
encoding every compiler emits for `vsetvli e32,m1` (`vsew = 2`) and (b) reported
a `vsew` field value that the specification does not define (`5` for e32). The
field *position* (bits 5:3) was correct; the *value* was not.

The root cause is that every unit case drove the module with the same encoding
the module already used, so nothing could disagree with the DUT. Fixing the
oracle (`sim/unit/tb_vec.cpp`) to take and produce the spec field value is the
part that makes the case able to catch the defect again.

## Before / after, observed

A directed stimulus (`build/scratch_vsew/tb_vsew_probe.sv`, a throwaway) drives
`mosaic_vec_cfg` with a real `vsetvli` argument word (`vsetvli rd, x0, vtypei`:
AVL = ~0 → `vl = VLMAX`) and prints the architectural read-back. It was built
twice: against the pre-fix RTL (`git show HEAD:rtl/core/mosaic_vec_cfg.sv`,
HEAD `4ab0018`) and against the fixed RTL.

Command (each build from its own empty `obj` directory):

```
verilator --binary --timing -j 0 -O2 -Wno-fatal --top-module tb_vsew_probe \
  -Mdir <v>/obj -o probe tb_vsew_probe.sv <v>/mosaic_vec_cfg.sv && ./<v>/obj/probe
```

| vtypei | denotes | before (HEAD) | after |
|---|---|---|---|
| `0x010` | spec `e32,m1` (vsew = 2) | `vill=1 vl=0 vlmax=0 vtype=0x8000_0000_0000_0000` | `vill=0 vl=4 vlmax=4 vtype=0x10 vsew_field=2` |
| `0x028` | pre-fix `e32,m1` (vsew = 5) | `vill=0 vl=4 vlmax=4 vtype=0x28 vsew_field=5` | `vill=1 vl=0 vlmax=0` (reserved field) |
| `0x000` | spec `e8,m1` (vsew = 0) | `vill=1 vl=0` (rejected) | `vill=0 vl=16 vlmax=16 vtype=0x00 vsew_field=0` |
| `0x018` | spec `e64,m1` (vsew = 3) | `vill=0 vl=16 vtype=0x18 vsew_field=3` (**sized as SEW = 8**, VLMAX 16) | `vill=0 vl=2 vlmax=2 vtype=0x18 vsew_field=3` |

That is the "before": a spec-legal `vsetvli e32,m1` sets `vill` (so the next
vtype-dependent instruction traps), and a successful configuration reads back
`vsew = 5`, not `2`. It is also the "after": `e32,m1` configures with
**VLMAX = 4** (VLEN = 128 / 32), which is the explicit check the lead asked for;
`e8,m1` gives 16 and `e64,m1` gives 2.

## Every reader of `vtype[5:3]`, and what it wanted

Classification is the reusable part: a reader that wants a **width** must
convert the field (`+3`); a reader that wants the **architectural value** must
not.

| Site | Reads | Wants | Change |
|---|---|---|---|
| `mosaic_vec_cfg.sv` `vsew_ok` | field | encoding validity | `3..6` → `0..3` (4..7 reserved) |
| `mosaic_vec_cfg.sv` `sew_log2_of` (new) | field | width | `= vsew + 3` |
| `mosaic_vec_cfg.sv` `vlmax_of` | field | width | `−(sew_log2_of(vsew))` |
| `mosaic_vec_cfg.sv` `emul_ok` | field | width | `lmul_exp + ELEN_LOG2 >= sew_log2_of(vsew)` |
| `mosaic_vec_cfg.sv` `vtype_supported` / `arg_vsew` / `old_valid` | field | encoding (+ width via helpers) | position kept; validity/width via the two helpers |
| `mosaic_vec_cfg.sv` `o_vtype_o`, CSR read, snapshot | whole word | **architectural encoding** | unchanged — the spec field is stored verbatim |
| `mosaic_vec_desc.sv` `sew_raw`/`sew_l` | field | width | `sew_l = field + 3`; `vsew_valid` `0..3`; `o_sew_log2_o = sew_l` |
| `mosaic_vec_alu.sv` `sew_l` | field | width | `cfg_vtype_i[5:3] + 3'd3` |
| `mosaic_vec_lsu.sv` `cfg_vsew` | field | width | `{1'b0,field} + 4'd3` (4-bit so 4..7 → 7..10 is still range-rejected, not wrapped) |
| `mosaic_vec_fp.sv` `sew_l` | field | width | `+ 3'd3` (so `sew64`/`sew32` still name e64/e32) |
| `mosaic_vec_restart.sv` `sew_l` | field | width | `+ 3` |
| `mosaic_core.sv` `vec_eew_mismatch` | `vec_vtype[5:3]` | width | compared as `{1'b0,eew_sew} != ({1'b0,field} + 4'd3)` (both 4 bits, no `WIDTHEXPAND`) |

`mosaic_core.sv:4604` is the only line changed in that file; it was coordinated
with the `ImplementPMU` lane (its counter wiring is elsewhere) and with
`AdvertiseVectorFamilies` (its decode arms are elsewhere). The width fix there
is on the `vtype` side only: the packet's `eew_sew` stays `log2(EEW)` (`3..6`)
because `mosaic_core.sv:4600`'s vstart byte offset shifts by `eew_sew − 3` and
`mosaic_vec_lsu` uses it as a width.

## Cases updated, and why the old expectation was wrong

`sim/unit/tb_vec.cpp` is the *test's model of the specification*, so it now takes
and produces the spec field value and derives widths with `+ 3`:
`VsewValid` `0..3`; `VlmaxOf`, `VtypeReason`, `OracleElemCount`, `OracleReason`
convert; `Vtype`/`Vtypei` place the field; `ConfigureVec`/`LsuConfig`/`FpConfig`
take the field (call sites that carry a width use the new `SewField()` boundary
helper). The old expectations that moved:

* `rvv.vtype_layout` round-trip and descriptor phases enumerated `vsew 3..6`
  and asserted `SpecVsew(vtype) == vsew` with `sew_log2 == vsew`. **Old
  expectation:** a legal field is 3..6 and the read-back field equals log2(SEW).
  **New:** a legal field is 0..3 and the read-back field equals the spec value
  (0..3); the descriptor's `sew_log2` is the width and is compared to
  `vsew + 3`. The position intent (bits 5:3, bits 2:0, bit 6, bit 7, bit 63) is
  unchanged.
* The "unsupported" case was `Vtypei(0,0)` meaning SEW = 1 (log2 0) — which is
  itself the wrong model, since field 0 is e8. **New:** the unsupported case is
  the reserved field `Vtypei(4,0)`.
* Every raw `vtypei` constant now denotes a spec configuration: `0x10` e32,
  `0x08` e16, `0x00` e8, `0x18` e64 (was `0x28`/`0x20`/`0x18`/`0x30`).
  Updated in `tb_vec.cpp` literals, `tb_core_vec.cpp` (`0x28→0x10`,
  `0x20→0x08`, `0x18→0x00`, and the vtype read-back check `== 0x10`),
  `tb_multihart_ctx.cpp` (`0x28→0x10`), `tb_core_perf.cpp` (`0x20→0x08`).
* `config/csr/vector.json`: the `vtype` `spec_clause` said the legal set is
  "vsew in {3,4,5,6}" and justified the `vill=1` reset by "vtype 0 decodes to
  SEW = 2^0 = 1, which is not supported". Both are the defect written down.
  **New:** legal set `{0,1,2,3}` with `SEW = 2^(vsew+3)`; the reset stays
  `vill = 1` but for the specification's own recommendation (L680-L681), not
  because `vtype = 0` would be unsupported (it is legal `e8,m1`).

No check was weakened and no mutant was deleted.

## Control observed to fail

New mutant `MOSAIC_VEC_MUTANT_VTYPE_SEW_UNSHIFTED` in `mosaic_vec_cfg.sv`
re-injects the pre-fix interpretation: `vsew_ok` accepts `3..6` and
`sew_log2_of` returns the field unshifted. `tools/run_vec_layout_controls.py`
gained a fourth entry for it. Result (`python3 tools/run_vec_layout_controls.py
--profile p2`, each mutant built from a deleted directory):

```
baseline exit=0 RESULT PASS rvv.vtype_layout checks=1253 words=88 descriptors=88 cycles=193
shipping binary sha256: 19bfeaef2fe2ad0603b066fe6049674697bca40dafaa1650b829427c72fcad0e

MOSAIC_VEC_MUTANT_VTYPE_LEGACY_75      1  OK   sha256 e55104fa...
MOSAIC_VEC_MUTANT_VTYPE_SEW_WRONG      1  OK   sha256 f07e8cee...
MOSAIC_VEC_MUTANT_VTYPE_SW_WRITE       1  OK   sha256 08e2d683...
MOSAIC_VEC_MUTANT_VTYPE_SEW_UNSHIFTED  1  OK   sha256 6a96c08d4093f66968404532776704db468ea33c6cdae7bb0b5f56c8fdd6fed4
    CHECK FAILED: vtype-roundtrip vsew=0 vlmul=0 ta=0 ma=0: a legal configuration was not accepted
all 4 mutants mutate the binary, exit 1 and name the check they break
```

The new mutant's binary differs from the shipping binary, exits 1, and its first
failure is named (`vtype-roundtrip vsew=0 ...: a legal configuration was not
accepted`) — the exact symptom a compiler would hit.

The other control runners whose cases this change moved were re-run and still
bite (each mutant from a deleted directory, distinct binary, exit 1, named
check): `tools/run_vec_cfg_controls.py` (vset_boundaries) 4/4, and
`tools/run_vec_alu_controls.py` (integer_mask_permute) 5/5,
`tools/run_vec_maskpfx_controls.py` (mask_prefix_*) 3/3. One mutant's
*substituted* configuration was corrected so its documented meaning survives the
encoding change: `MOSAIC_VEC_MUTANT_SILENT_M1` accepted an unsupported vtype "as
SEW=8, LMUL=1" by writing field `3` (which now means e64); it writes field `0`
(e8) again. Its first failure moved from `vsew=0 vlmul=0` to `vsew=0 vlmul=4`
(the first unsupported cell now that e8 is a legal SEW); the check it breaks is
unchanged.

## Gates re-run (each from a deleted build directory)

| Gate | Result |
|---|---|
| `rvv.vtype_layout` (p2) | PASS, 1253 checks, 88 words, 88 descriptors, 193 cycles |
| `rvv.descriptor_legality` (p2) | PASS, 336 checks |
| `rvv.vset_boundaries` (p2) | PASS |
| `rvv.mask_prefix_semantics` (p2) | PASS, 1115 checks |
| `rvv.mask_prefix_masked` (p2) | PASS |
| `rvv.mask_prefix_vstart` (p2) | PASS |
| `rvv.integer_mask_permute` (p2) | PASS, 99952 checks |
| `rvv.memory_modes`, `rvv.fp_flags_reduction`, `rvv.partial_fault_restart`, `rvv.chaining_hazards`, `rvv.stop_path_inflight`, `coalesce.element_faults` (p2) | PASS |
| `vec.mask_prefix_at_core` (p0 and p1) | PASS, 113 checks |
| `vec.integrated` (p2) | PASS, 112 checks |
| `rvv.lane_resize_boundary` (p2) | PASS |
| `multihart.context_isolation` (p3) | PASS |
| `perf.equal_resource_compare`, `frontend.width_buffering` (p1) | PASS |
| `core.corpus_sweep` (p0) | PASS |
| `core.act_dut` (p1) | PASS |
| `lint_rtl --profile p0` / `--profile p1` | 65/65 clean on both |
| `check_records` | ok |
| `check_exclusions` | ok (54 registered, 18 open, 36 covered) |
| `check_contracts --all` | OK p0/p1/p2/p3 |
| `host_oracle.py --all` / `--check-golden-only` | agrees; corpus did not move |

`tests/programs/vec` (the real-program corpus) is owned by the
`VectorSelfCheckCorpus` lane and was updated there; it reports
`vec.selfcheck_corpus` PASS 7/7 on p0/p1/p2/p3 with the spec constants
(e8=0x00, e16=0x08, e32=0x10, e64=0x18) and its wrong-constant control still
failing.

## Not covered / open findings

* **A vector load's EEW must equal `vtype.SEW` — a separate defect, not the
  same root cause.** `rtl/core/mosaic_core.sv:4604` refuses a memory macro
  (`kind == 2 || kind == 3`) whose instruction EEW differs from SEW:
  `vec_eew_mismatch` forces equality, and the descriptor's `VLOAD`/`VSTORE`
  class fixes `EEW = SEW` (`dst_sew_off = 0`). RVV 1.0 does **not** require
  EEW = SEW: a vector memory instruction selects its EEW from the encoding and
  the EMUL is adjusted (`EMUL = (EEW/SEW) · LMUL`), so e.g. `vle64.v` under
  `vsetvli e32, m1` (EMUL = 2) is legal and currently traps as an illegal
  instruction. This is a distinct implementation restriction (the packetizer
  uses the configuration SEW for the address and refuses a disagreeing suffix),
  not the field-value confusion this package fixed; the two only meet because
  both live near the `vtype` word. **Reproduction recipe (not executed here,
  left for its own package):** run `vsetvli x7, x0, e32, m1` then `vle64.v v1,
  (a0)` on the integrated core (the `vec.integrated` harness in
  `sim/unit/tb_core_vec.cpp` already loads/executes hand-encoded V); the load
  raises an illegal-instruction trap where RVV 1.0 expects a 2-register-group
  load. Status: **open finding**, out of scope for FixVsewEncoding.
* `tests/programs/vec` was updated by its owning lane, not by this package.
* The historical report `results/reports/rvv-vtype-layout.md` states
  `SEW = 2^vsew` (line 42) and a `vsew 3..6` legal set; both statements are
  superseded by this report. That report is I-051's record and is left as
  written; the corrected layout is recorded here and in
  `config/csr/vector.json`.
* The descriptor's reason name `RSN_RESERVED_VSEW` and its numeric code are
  unchanged; only the value set it fires on changed (4..7 instead of 0..2/7).
* Configurations that still set `vill` for a different, intended reason:
  fractional LMUL below `SEW_MIN/ELEN` (the EMUL floor), `vlmul = 100`, and any
  non-zero reserved bit `vtype[62:8]` / `vtypei[10:8]`.
* The `MOSAIC_VEC_MUTANT_VTYPE_LEGACY_75` / `_SEW_WRONG` mutants now bite at
  different named checks than before (LEGACY_75 first fails in the cfg
  round-trip on `ta`/`ma`, SEW_WRONG first fails on a `vlmul = 5` descriptor
  cell); both still exit 1 with a named first failure and are reported above.
