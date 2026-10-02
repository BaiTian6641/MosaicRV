# I-055 — vector floating point and per-element exception flags

Work package **I-055** of `docs/implementation-plan.md` §3.1 (`V-055`),
implementing

> 复用受测 FPU handshake；按 active elements 汇总 flags；测试 masked sNaN、
> 转换、widening、reduction 合法结果规则。
> Outputs: vector FP packet execution、`CASE=rvv.fp_flags_reduction`.
> Pass: 不活跃元素不污染 flags；确定性与规范允许差异由 comparator 显式处理
> 而非粗略浮点容差。
> Fail: 把 vector FP 顺序任意改变到规范不允许的结果。

## STATUS: COMPLETE (unit); NOT WIRED INTO THE CORE

| Acceptance item | Evidence |
| --- | --- |
| The mechanism exists as RTL, driving the I-053 VRF from the I-052 snapshot and reusing the I-049 FPU | `rtl/core/mosaic_vec_fp.sv` (new, 1239 lines); `sim/tb/mosaic_vec_tb.sv` instantiates `u_vec_fp` and muxes its VRF slot into the existing arbiter |
| Every declared family runs and is gated | phase `capgate`: all 10 families run declared and each is refused with its bit clear, no VRF transaction; an unknown family id, an operation id outside the family's table, SEW=16 and widening at LMUL=8 are refused too |
| Element-wise arithmetic in both supported SEW widths | phase `fpelem`: 6 arithmetic + 2 min/max + 3 sign-injection operations, both operand forms, at SEW=32 and SEW=64, plus an LMUL=2 cell |
| A masked sNaN: both directions | phase `masked-snan`: an active sNaN sets `NV` and returns the canonical quiet NaN; a masked-off sNaN sets nothing, performs no FPU operation (`o_fpu_issues` delta = 3, not 4) and leaves its destination undisturbed |
| Conversions, widening and narrowing | phase `fpelem cvt/wide/narrow`: `vfcvt.*` at both widths, `vfwcvt.*` (SEW=32→64) and `vfncvt.*` (64→32, including the `rtz` forms), compared bit-exactly |
| An ordered reduction, bit-exact, ordering stated | phase `fpred … ordered`: the accumulator must equal the strict left fold, and the flag trace must be ascending from `vstart`; the vectors are chosen so the left and right folds differ, so the comparison is discriminating |
| An unordered reduction, constrained to the permitted set | phase `fpred … unordered`: the result must be a member of an enumerated set of tree results (no tolerance); the set is required to hold more than one value for these vectors |
| `vxrm`/`frm` rounding-mode effects on a tie | phase `round`: the same tie rounds to `0x3F800001` under RUP and `0x3F800000` under RNE/RDN/RTZ via `frm`; changing `vxrm` (a fixed-point field) changes nothing |
| The flag aggregate over a whole macro including masked-off elements | phase `agg`: `pending` = the OR over active elements only; a masked-off element contributes nothing |
| Fail criterion — an inactive element pollutes `fflags` | `MOSAIC_VEC_FP_MUTANT_INACTIVE_FLAG`, exit 1 |
| Fail criterion — premature flags | `MOSAIC_VEC_FP_MUTANT_EARLY_COMMIT`, exit 1 |
| Fail criterion — an ordered reduction reassociated | `MOSAIC_VEC_FP_MUTANT_REDUCE_REASSOC`, exit 1 |
| Fail criterion — rounding ignored | `MOSAIC_VEC_FP_MUTANT_RM_IGNORE`, exit 1 |
| `python3 tools/run_unit.py --profile p0 --case rvv.fp_flags_reduction` | `RESULT PASS … checks=13828 families=10 cells=25 cycles=80930`, exit 0 |
| The same binary at seeds 2, 7 and 12345 | PASS, identical 13828 checks / 25 cells / 80930 cycles |
| `python3 tools/run_vec_fp_controls.py` | 4 of 4 mutants mutate the binary, exit 1 and name the check they break |
| `verilator --lint-only -Wall` on the new module, both profiles | clean under p0 and p2 |
| `slang-tidy --std 1800-2017` | exit 0; only the tree-wide STYLE advisories (`STYLE-7` instance prefix, `STYLE-13` unnamed `always_comb`) |
| Sibling cases unchanged | `rvv.integer_mask_permute`, `rvv.memory_modes`, `rvv.descriptor_legality`, `rvv.vset_boundaries`, `rvv.vtype_layout`, `vrf.mapping_aliases`, `fp.operation_matrix`, `fp.precise_flags_and_boxing` PASS |
| ACT4 | `RESULT PASS core.act_dut applicable=127 generated=127 run=127 passed=127 failed=0` (p1) |
| `make check`, `check_records`, `check_exclusions` (+ `--negative`) | green (70 packages / 80 cases; 13/13 illegal ledgers rejected) |
| This report | — |

Artifacts (sha256): `rtl/core/mosaic_vec_fp.sv`
`bd72e14fc1bb95024374a79d4612d4dbeab9f08071687138a4f4f536a55c1749`,
`sim/unit/tb_vec.cpp`
`a9c8e3cf131c39eb27668988bc6a75b3e6b828bcead5c19b93896b49b8a8d2fa`,
`sim/tb/mosaic_vec_tb.sv`
`1cdeda0e029820c100d0bca897c5f04900d570fb377f2e4bb36cbe1427baffcc`,
`tools/run_vec_fp_controls.py`
`a9e31951141d5d1cce8a540243fa6a9b8c1847517db8c44938bc7978080e47db`.

## The registry entry

`tests/unit/registry.json` carried the case and the integration lead added
`rtl/core/mosaic_vec_fp.sv` to its `rtl` list at my request (the entry already
listed `mosaic_fpu.sv` and the other vector files). The `pending` marker was left
as found — the integration lead clears it when the package is recorded. Note
that the case *builds* even without the addition, because Verilator resolves a
missing module from the `-I rtl/core` path; the list is what the negative-control
script asserts on, and an incomplete list is a defect that only shows when the
directory layout moves.

## The mechanism, as a rule

`mosaic_vec_fp` executes one vector *packet*: it walks the destination elements
of one operation, reads the source elements it needs from the banked VRF
(I-053) through a single read slot, writes the destination through a single
write slot, and hands each element's operation to one shared `mosaic_fpu`
instance (I-049). It owns no arithmetic:

* **configuration** — `cfg_vtype_i` / `cfg_vl_i` / `cfg_vstart_i` are the
  snapshot `mosaic_vec_cfg` (I-052) hands out; `cfg_frm_i` is the floating-point
  dynamic rounding mode, which lives in the scalar `fcsr` and not in `vxrm`.
  SEW/LMUL/vta/vma are decoded from `vtype` at the ratified positions and from
  nowhere else.
* **addressing** — the unit names a `(base register, element index, SEW, LMUL)`
  demand and the VRF resolves it, including group alignment and the wide-operand
  case. No physical address is computed here.
* **mask and tail policy** — the same rule I-054 states: an element is active iff
  `vstart <= i < vl` and the mask bit is set; an active element is written with
  the result, an inactive one with vma/vta is unchanged (0) or written all-ones
  (1), and an element below `vstart` is unchanged. A mask destination (the
  comparisons) is written with a read-modify-write of its SEW=8 byte. An
  inactive element performs **no source access and no FPU operation**.

### The flag-aggregation rule

```
element i contributes its {NV,DZ,OF,UF,NX} to this macro's pending flags
    iff  i is active (vstart <= i < vl, mask bit set)
    and  the operation is one the ISA says raises flags;

elements are folded in ascending element order, and the per-element
contribution is published on the trace (o_flag_trace_valid/elem/flags)
in that order;

pending flags become architectural only when the macro retires:
    commit_valid_i while a completed macro is pending  ->  arch |= pending;
    flush_i before that                                ->  pending discarded,
                                                           nothing architectural;
    a squashed macro contributes nothing.
```

The case proves both directions: an active sNaN sets `NV` and a masked-off sNaN
sets nothing; `o_fpu_issues` equals the active-element count, so a masked-off
element is not merely un-flagged but *not computed*; and nothing is architectural
before the commit strobe. The unit exposes `o_inactive_flag_ctr`, which stays 0
in every shipping run.

### The determinism table and the comparator

This is the reusable part of the package. Every deterministic result is compared
with `==` on the bit pattern; the permitted-to-differ results are compared
against an explicitly enumerated set. **No comparison anywhere uses an epsilon.**

| Operation class | Deterministic? | Comparator (in `sim/unit/tb_vec.cpp`) |
| --- | --- | --- |
| `vfadd/vfsub/vfrsub/vfmul/vfdiv/vfrdiv` | yes | bit-exact vs the host FPU under `fesetround`; NaN rules from the bit model |
| `vfmin/vfmax` | yes | bit-exact vs a written-out min/max model (NaN handling, sign of zero) |
| `vfsgnj/vfsgnjn/vfsgnjx` | yes | bit-exact vs a bit model |
| `vmfeq/vmfne/vmflt/vmfle/vmfgt/vmfge` | yes | bit-exact vs a written-out compare model with the ISA's NV split |
| `vfcvt.*`, `vfwcvt.*`, `vfncvt.*` (element-wise) | yes | bit-exact vs the host FPU; fp→int via a written-out saturating model |
| `vfredosum`, `vfwredosum` (**ordered**) | yes, element order fixed | bit-exact vs the strict ascending left fold; the vectors are chosen so the right fold differs, and the case asserts both that they differ and that the DUT is not the reassociated value |
| `vfredusum`, `vfwredusum` (**unordered**) | no — permitted to differ | membership in `UnorderedPermitted`: every binary-tree shape over the active elements plus the scalar, at both permitted internal precisions (SEW and 2·SEW), root rounded to the result format, plus the spec's additive-identity allowance (a zero result may carry either sign) |
| `vfredmin/vfredmax` | yes (order-independent) | bit-exact vs the serial min/max fold |

The chosen ordered-reduction vectors (`1.0, 2^24, 1.0, -2^24`) make the left fold
`0` and the right fold `1`; the case fails if the two agree (so the check can
never silently become non-discriminating) and fails if the DUT returns the
reassociated value. A case that compared with a tolerance would pass on a machine
that reassociated an ordered reduction; this one does not.

## The FPU-reuse decision

**One** `mosaic_fpu` instance is instantiated and driven element by element behind
its tagged request/response handshake. The engine issues one request, waits for
`res_valid` with `res_ready` held high, and consumes the value and the five flag
bits beside it. There is no second floating-point datapath: this module contains
no adder, rounder or NaN classifier.

* **How many instances / contention.** One. The packet engine has one operation in
  flight at a time and the VRF's single read slot serialises the elements anyway,
  so two vector FP macros cannot contend for the datapath — there is no second
  consumer. Two instances would double the area of a unit whose throughput is
  bounded by the VRF slot and by the lane quota work of I-027/I-059, and would
  have to be arbitrated to no benefit. The decision is recorded here rather than
  left implicit.
* **Declared latency and backpressure.** The engine waits for the FPU's
  `res_valid`; it never assumes a fixed number of cycles. The FPU's own
  `o_latency_o` is captured with each response and exposed as `o_last_latency_o`;
  the case checks the declaration (1 cycle for everything but `fdiv`, 66 for
  `fdiv`) and that a 4-element divide packet takes longer than a 4-element add
  packet.
* **Element identity.** The FPU's tag ports carry the element index
  (`req_uop_index_i = cur_q`); the macro identity is the driver's, since the
  packet engine walks one macro at a time.

## The host oracle's identity

The expectation is computed in `sim/unit/tb_vec.cpp`. The value and the flags
come from the host's own IEEE-754 unit through `<fenv.h>` — `fesetround` for the
mode and `fetestexcept` for `FE_INVALID`/`FE_DIVBYZERO`/`FE_OVERFLOW`/
`FE_UNDERFLOW`/`FE_INEXACT` — exactly the style `CASE=fp.operation_matrix`
(I-049) established. Two parts cannot come from the host and are written out
instead:

* **NaN handling** — this design returns one canonical quiet NaN and does not
  propagate payloads, while the host propagates a payload, so the NaN rules
  (canonical result, `NV` for a signalling operand, the `feq`/`flt` NV split) are
  a bit-level model.
* **fp → integer** — C's conversion is undefined out of range, so the RISC-V
  saturating result (with `NV` and no `NX`) is a bit-level model written from the
  ISA.

The register file is modelled as 32 × 128-bit values and addressed with the same
`(base, element, SEW, LMUL)` rule the VRF implements, so the widening/narrowing
source widths and the element order are checked, not merely the arithmetic.

## The mutant table

`python3 tools/run_vec_fp_controls.py --profile p0` — shipping built first from
an empty directory and required to pass, then each mutant built from its own
empty directory with its `-D` on the recorded command line.

| mutant | injects | sha256 | first failing check |
| --- | --- | --- | --- |
| shipping | — | `e3c3befb46d7ac2d554589cf275c93303f92603570030824afc91e48360dc61f` | — (PASS) |
| `MOSAIC_VEC_FP_MUTANT_INACTIVE_FLAG` | a masked-off element still computes and its flags are merged | `a99b6f4de2f6274c3c1114fd43baf1d789e86cd04bec7d485008b86db108f185` | `masked-snan inactive: a masked-off sNaN polluted NV` |
| `MOSAIC_VEC_FP_MUTANT_EARLY_COMMIT` | the macro's flags become architectural at completion, not at retire | `f6f78865bbe815d7164e6be5970a27282ae8c919f91c199bdbb6dffbeed865ac` | `agg: flags became architectural before the macro retired: NDOUX` |
| `MOSAIC_VEC_FP_MUTANT_REDUCE_REASSOC` | an ordered reduction folds in descending element order | `840fcb600d3d82e6f7685e788ad0fb79c37078da6e81af60d08559b79aadb2a8` | `fpred redsum op0 sew32: the element fold order was not ascending` |
| `MOSAIC_VEC_FP_MUTANT_RM_IGNORE` | the rounding mode is ignored; everything rounds to nearest-even | `f06bd4d6fa4b175f2cacadc08656cb139a5d30425ac06861ec142cee32f6e391` | `round frm3: got 0x000000003f800000 expected 0x000000003f800001` |

Each mutant's binary differs from the shipping one (`cmp`) and each exits 1.

Two files were extended, not restructured: `sim/tb/mosaic_vec_tb.sv` gained an
additive `fp_*` port block and the `u_vec_fp` instance, and the existing VRF
arbiter expression became `mem_owner_i > lsu_busy > fp_busy > alu` (only one
engine is ever busy, so the order is unobservable to the other cases, which is
why they were re-run); `sim/unit/tb_vec.cpp` gained the I-055 phase set alongside
the phase sets it already carried.

## Not covered — stated deliberately

A package that claims more than it implements is the card's own fail mode, so:

* **Families not implemented.** `vfsqrt` (the scalar `fsqrt` is declared absent
  by I-049 and a vector form would need a datapath that does not exist); the
  fused multiply-accumulate family (`vfmacc`, `vfnmacc`, `vfmsac`, `vfnmsac`,
  `vfmadd`, `vfnmadd`, `vfmsub`, `vfnmsub`, `vfwmacc*`, …) — the scalar fmadd
  family is declared absent by I-049 and this package carries that absence
  forward rather than writing the datapath twice; `vfncvt.rod.f.f.w`
  (round-to-odd is not one of the five modes the shared FPU implements);
  `vfmerge`/`vfmv.*`; the widening arithmetic forms (`vfwadd`, `vfwsub`,
  `vfwmul`) — only widening *conversions* are implemented. There is **no
  capability bit** for any of them, so they cannot be advertised.
* **Half precision.** SEW=16 is refused: the F/D datapath has no half format.
  Vector FP is supported at SEW=32 and SEW=64 only, and the widening/narrowing
  forms therefore exist for SEW=32 (2·SEW = 64 = ELEN).
* **`vstart` and flags.** The RTL's only use of `vstart` is the active-range
  gate, so an element below `vstart` is inactive and contributes no flag — the
  same rule as a masked-off element. **No phase drives a non-zero `vstart`**:
  the restart path is I-057's, and driving it here would test a state the packet
  engine does not own. What is checked is the rule (masked-off elements
  contribute nothing); what is not is a restart.
* **The unit is not wired into the core.** `mosaic_core.sv` / `mosaic_dispatch.sv`
  are untouched: no decoded vector instruction reaches this unit, and the vector
  capability is not an advertisement. The `commit_valid_i` port is the driver's
  stand-in for the retire signal the core would provide.
* **The descriptor is not driven by this unit.** The packet engine takes its
  configuration from I-052's snapshot directly; binding a descriptor's element
  bitmap and fault progress (I-057) is separate work.
* **One operation per cycle per element.** Throughput (the lane quota of I-059,
  chaining of I-058) is out of scope; the ordered reduction is a serial chain of
  scalar additions, which is also a valid implementation of the unordered one.
* **`vfredusum` is implemented as the ordered serial fold.** That is explicitly a
  valid implementation of `vfredusum` (the spec says so), and the case accepts it
  through the permitted-set comparator; a future tree implementation would also
  be accepted, which is the point of enumerating the set instead of pinning the
  value.
