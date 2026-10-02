# I-054 — vector integer, mask, permute and reduction

Work package **I-054** of `docs/implementation-plan.md` §3.1 (`V-054`), implementing

> 分 operation family 实现 packet execution；包括 widening/narrowing、saturation、
> slide/gather/compress、mask 与 reduction，逐类 capability gating。
> Outputs: vector integer lanes、`CASE=rvv.integer_mask_permute`.
> Pass: 每个声明 family 的边界/重叠/masked-off 元素行为通过 V suite，未完成
> family 不进入 p2 gate。
> Fail: 只实现 vadd/vmul 就宣称 V 完成，或 masked-off 元素发起异常访问。

## STATUS: COMPLETE (unit); NOT WIRED INTO THE CORE

| Acceptance item | Evidence |
| --- | --- |
| The mechanism exists as RTL, driving the I-053 VRF from the I-052 snapshot | `rtl/core/mosaic_vec_alu.sv` (new); `sim/tb/mosaic_vec_tb.sv` instantiates `mosaic_vrf` + `mosaic_vec_alu` and wires the ALU's configuration to `cfg_snap_*` |
| Every declared family's boundary behaviour | phase `element-lane` / `sat-boundary` / `wide-width`: every family at every SEW, every operation, both operand forms, crossed over an 8-value boundary set (0, 1, max, max-1, sign, sign±1, alternating) |
| Masked-off elements cause neither a trap nor an access | phase `masked-off`: a gather whose masked-off element carries an out-of-range index; the source-read count equals the active-element count, the VRF bad-demand counter is unchanged, no trap, and the destination is undisturbed (vma=0) or all-ones (vma=1) |
| Capability gating refuses an undeclared family | phase `capability-gate`: each of the 17 families runs declared, and is refused with no VRF transaction when its bit is cleared; an unknown family id is refused too |
| Reduction ordering declared and checked | the header states ascending element order from `vstart`; the case checks the running accumulator element by element and the engine's element trace is exactly `vstart..vl-1` |
| Coverage asserted, not implied | 346 cells = every family × every SEW × every LMUL the family's legality admits, each compared element by element against the host oracle; the cell count is itself a check |
| Fail criterion — only `vadd`/`vmul` declared complete | 17 families are gated individually; the unimplemented RVV classes have no capability bit and cannot be advertised (see "not covered") |
| Fail criterion — a masked-off element traps or accesses | `MOSAIC_VEC_ALU_MUTANT_MASKED_ACCESS`, exit 1 |
| `python3 tools/run_unit.py --profile p0 --case rvv.integer_mask_permute` | `RESULT PASS … checks=99952 cells=346 families=17 cycles=106824`, exit 0, sha256 `95e1063703c8e69e…` |
| The same binary at seeds 2, 7 and 12345 | PASS, identical 99952 checks and 346 cells |
| `verilator --lint-only -Wall` on the new module, shipping and all five `-D` controls | clean under every definition |
| `slang-tidy --std 1800-2017` on the module and the wrapper | exit 0 (only the tree-wide STYLE advisories) |
| `python3 tools/run_vec_alu_controls.py` | 5 of 5 mutants mutate the binary, exit 1 and name the check they break |
| Sibling cases unchanged | `rvv.descriptor_legality`, `rvv.vset_boundaries`, `rvv.vtype_layout`, `vrf.mapping_aliases`, `core.corpus_sweep` PASS |
| ACT4 | `RESULT PASS core.act_dut applicable=127 generated=127 run=127 passed=127 failed=0` (p1) |
| This report | — |

## The registry entry

`tests/unit/registry.json` already carried the case, naming

| field | value | state found |
| --- | --- | --- |
| `task` | `I-054` | — |
| `top` | `mosaic_vec_tb` | — |
| `rtl` | `mosaic_pkg.sv`, `mosaic_vec_desc.sv`, `mosaic_vec_cfg.sv`, `mosaic_vrf.sv`, `mosaic_vec_alu.sv` | `mosaic_vec_alu.sv` **missing** |
| `sv` | `sim/tb/mosaic_vec_tb.sv` | present (extended) |
| `cpp` | `sim/unit/tb_vec.cpp` | present (extended) |
| `pending` | `true` | left as found |

`mosaic_vec_alu.sv` was created at exactly the registered path; the wrapper and
the driver were extended. The registry was **not** edited — the `pending` marker
is the integration lead's to clear when the package is recorded.

## The mechanism, as a rule

`mosaic_vec_alu` executes one vector *packet*: it walks the destination elements
of one operation, reads the source elements it needs from the banked VRF through
one read slot, and writes the destination through one write slot. It owns no
storage and no second configuration path:

* **configuration** — `cfg_vtype_i` / `cfg_vl_i` / `cfg_vstart_i` / `cfg_vxrm_i`
  are the snapshot `mosaic_vec_cfg` hands out. SEW/LMUL/vta/vma are decoded from
  `vtype` at the ratified positions (vsew 5:3, vlmul 2:0, vta 6, vma 7) and from
  nowhere else.
* **addressing** — the unit names a `(base register, element index, SEW, LMUL)`
  demand and the VRF (I-053) resolves it, including group alignment and the
  wide-operand case. No physical address is computed here.

### 1. Mask and tail policy

`vma` = `vtype[7]`, `vta` = `vtype[6]`:

```
element i is active  iff  vstart <= i < vl  AND  (v0.mask[i] or unmasked)
an active element                          is written with the result;
an inactive element with vma/vta == 0      is unchanged;
an inactive element with vma/vta == 1      is written with all-ones
                                           (masked-off uses vma, i >= vl uses vta);
an element below vstart                    is unchanged, whatever vma/vta say.
```

The mask register `v0` is read as a SEW=8, LMUL=1 register group: bit `i` is bit
`i%8` of element `i/8`. A **mask destination** (comparisons, mask-logical,
mask-prefix) is written with a read-modify-write of its SEW=8 byte, because the
other bits of that byte belong to other elements.

An inactive element performs **no source access**: the engine never issues a
source read for it, so neither a trap nor a register-file access can follow from
a masked-off element. The case observes the unit's own source-read counter, the
engine's access trace, and the VRF's bad-demand counter.

`vslideup` is the one operation with an extra unchanged range: destination
elements below `OFFSET` are unchanged (not "masked off"), so no source is read
and no update is produced for them.

### 2. Reduction ordering

A reduction folds the source elements in **strictly ascending element index
order**, starting at `vstart`, over active elements only, with `vs1[0]` as the
initial accumulator. There is no tree, no reassociation and no parked partial
accumulator. `vwredsum[u]` collects into a 2·SEW accumulator.

Integer additions are associative, so the *order* is observable only through the
sequence, which is why the unit exposes it: `o_acc` carries the running
accumulator and `o_trace_valid`/`o_trace_elem` carry each folded element index.
The case requires the trace to be exactly `vstart, vstart+1, …, vl-1` and the
running accumulator to match the specification's fold at every step. This is
also the sequence a partial-trap restart (I-057) has to reproduce, so it is
architectural, not diagnostic.

### 3. Capability gating

`caps_i` has one bit per operation family (17 bits). A family whose bit is clear
is **refused**: `o_illegal` asserts, the packet issues no VRF transaction, and it
retires with no architectural effect. `o_illegal` is also set for an unknown
family id and for a widening/narrowing form whose effective element width or
effective LMUL would leave the descriptor's admitted range (EEW ∈ [8, ELEN],
EMUL ∈ [1/8, 8]).

## Element, rounding and saturation rules

* `vxrm` comes from the configuration snapshot and follows the spec's table:
  rnu adds `v[d-1]`; rne adds `v[d-1] & (v[d-2:0] != 0 | v[d])`; rdn adds 0;
  rod adds `!v[d] & (v[d-1:0] != 0)`.
* Saturating forms clamp at the signed or unsigned boundary and set `o_sat`; the
  value is never wrapped. `vsaddu`/`vsadd`/`vssubu`/`vssub` clamp;
  `vaaddu`/`vaadd`/`vasubu`/`vasub` average with vxrm rounding at 2·SEW and
  truncate to SEW (so `vasub` wraps only where the specification says it does).
* `vnclipu`/`vnclip` round per vxrm *before* saturating; `vnsrl`/`vnsra` do not
  saturate.
* `vrgather` returns zero for an index at or above VLMAX and makes **no access**
  for it.
* `vcompress` packs the active source elements into consecutive destination
  elements and leaves the rest to the tail policy.

## The host oracle's identity

The expectation is computed in `sim/unit/tb_vec.cpp` element by element from the
V specification's rules — `OracleElem` is written from the operation table in
`src/v-spec.adoc` of the pinned tag (`~/mosaic-ref/riscv-v-spec`, the same
revision the descriptor cites), not from a second copy of the RTL's expressions.
The register file is modelled as 32 × 128-bit values and addressed with the same
`(base, element, SEW, LMUL)` rule the VRF implements, so the operations that are
*about* element order (slide, gather, compress, reduction) are checked as
sequences and not merely as values.

## Coverage

346 cells, each a full packet compared element by element, asserted at the end
to equal the count the scope implies (`coverage: 346 cells ran, expected 346`).

| SEW | LMUL 1/8, 1/4, 1/2, 1, 2, 4, 8 | cells per widening/narrowing family |
| --- | --- | --- |
| 8 | all seven | 1/8 … 4 (six: 1/8–4; 8 is out of range) |
| 16 | 1/4 … 8 (six) | 1/4 … 4 (five) |
| 32 | 1/2 … 8 (five) | 1/2 … 4 (four) |
| 64 | 1 … 8 (four) | none (2·SEW > ELEN) |

* 13 families (`ADDSUB`, `MUL`, `SHIFT`, `LOGIC`, `MINMAX`, `CMP`, `SAT`,
  `MASKLOG`, `MASKPFX`, `SLIDE`, `GATHER`, `COMPRESS`, `REDUCE`) × 22 = 286.
* 4 widening/narrowing families (`WIDE`, `MULW`, `NARROW`, `REDWIDE`) × 15 = 60.
* 286 + 60 = **346**.

Every cell configures the operation through `vsetvli` + snapshot capture, primes
the whole source group (a permute may read anywhere below VLMAX), runs the
packet, reads the destination group back through the VRF, and compares. Cells the
legality matrix calls illegal (a widening form at LMUL=8, SEW=64 for a widening
form) are not in scope and are not counted; the unit refuses them.

## The mutant table

`python3 tools/run_vec_alu_controls.py --profile p0` — shipping built first from
an empty directory and required to pass, then each mutant built from its own
empty directory with its `-D` on the recorded command line.

| mutant | injects | sha256 | first failing check |
| --- | --- | --- | --- |
| shipping | — | `95e1063703c8e69e281e7ae4b78526daa2cfff6cc3875b3810e31f6d9dae4927` | — (PASS) |
| `MOSAIC_VEC_ALU_MUTANT_SAT_WRAP` | a saturating add/sub/clip wraps at the element boundary instead of clamping; vxsat is never set | `9451814f0adb1a0bf532714784995d6659996830189521abed5ff0b0b65beaae` | `sat-boundary sat op0 sew8 form0 vxrm2: got 0x…00 expected 0x…ff` |
| `MOSAIC_VEC_ALU_MUTANT_WIDE_WIDTH` | a widening form zero-extends both sources, losing the sign of a negative source | `ad61cb2be118e0d2e721b9657ad6ca548d39d02b2ee499bccd045ee81cad4630` | `wide-width wide op1 sew8 form0 vxrm2: got 0x…ff expected 0x…ffff` |
| `MOSAIC_VEC_ALU_MUTANT_PERMUTE_ORDER` | a permute scales the element index by LMUL, ordering by the register grouping instead of by index | `188cd38dfd4fbdb974b702348ab2debb37dc2c5ac0c4a7b4533cf72613890281` | `permute-order gather lmul1 i0: rd2 2 expected 1` |
| `MOSAIC_VEC_ALU_MUTANT_MASKED_ACCESS` | a masked-off element still issues its source access | `d4472aa212ab48c8d2060e188fd0e4a1049fa9df75abf42df2a4fd56c4a946ec` | `masked-off vma0: a masked-off element made a bad access` |
| `MOSAIC_VEC_ALU_MUTANT_REDUCE_ORDER` | a reduction folds the source elements in descending order | `5aab7e9a0776a656269b04c0a9adbbfe8cd74125c2d25bd8720776ef00364aec` | `reduce-order packet op0: the fold order was not ascending from vstart` |

Each mutant's binary differs from the shipping one (`cmp`), and each exits 1.

Two units of the tree this package depends on were extended, not replaced:
`sim/tb/mosaic_vec_tb.sv` now instantiates `mosaic_vrf` and `mosaic_vec_alu` and
muxes the VRF's single read/write slot between the ALU and the driver (the two
owners are never active at once), and `sim/unit/tb_vec.cpp` grew the I-054 phase
set alongside the I-051/I-052 phase sets it already carried.

## Not covered — stated deliberately

A package that claims more than it implements is the card's own fail mode, so:

* **Families not implemented.** Integer divide and remainder (`vdiv[u]`,
  `vrem[u]`); the multiply-accumulate forms (`vmacc`, `vnmsac`, `vwmacc*`,
  `vwmaccsu`, `vwmaccus`); scaling shifts (`vssrl`, `vssra`); `vsmul`; integer
  extension (`vzext`, `vsext`, including the 4:1 and 8:1 ratios); `viota`,
  `vid`; `vrgatherei16`; the `vmerge`/`vmv*` moves; vector AMO. There is **no
  capability bit** for any of them, so they cannot be advertised: a machine
  cannot claim what the word does not name.
* **The vector memory classes** (`VLOAD`, `VSTORE`, `VWHOLE`) and the floating
  point classes (`VFP`, `VFPWIDE`, `VFPRED`) are other packages (I-055, I-056,
  I-058) and are not touched here.
* **The unit is not wired into the core.** `mosaic_core.sv` /
  `mosaic_dispatch.sv` are untouched: no decoded vector instruction reaches this
  unit, and the descriptor is not driven from a real decode. That integration is
  separate work, and until it exists the vector capability is not an
  advertisement.
* **The descriptor is not driven by this unit.** The packet engine takes its
  configuration from I-052's snapshot directly; binding a descriptor's element
  bitmap and fault progress to the engine (the partial-trap restart of I-057) is
  not part of this package.
* **Vstart restart is not exercised.** The policy is implemented (`vstart` gates
  the active range), but no phase drives a non-zero `vstart` into the engine; the
  restart path belongs with I-057.
* **One LMUL is out of scope for the widening families** (LMUL=8: the effective
  LMUL would be 16, outside `[1/8, 8]`). The legality matrix already calls that
  configuration illegal, and the unit refuses it, so it is excluded from the
  coverage count rather than quietly skipped.
* **The ALU reads one element per cycle.** Throughput (the lane quota of I-059,
  the operand collector of I-026/I-027) is out of scope; this package is about
  what is computed, not how fast.
* **`vmsof`/`vmsbf`/`vmsif` with `vstart != 0`** must raise illegal-instruction
  per the specification; the unit implements the ordering but does not raise on a
  non-zero `vstart`, because `vstart` restart belongs to I-057.

## Tree state at the time of writing

Two tree-wide gates are red for reasons outside this package, and are reported
rather than worked around, because this package owns neither file:

* `make lint-slang` fails on `rtl/core/mosaic_core.sv:1323`: the identifier
  `cache_fence_busy` is used before its declaration at line 1379. Verilator
  accepts the forward reference (module-scope declarations are hoisted);
  slang-tidy does not. `rtl/core/mosaic_core.sv` is another lane's file and this
  package must not edit the core. `slang-tidy` exits 0 on
  `rtl/core/mosaic_vec_alu.sv` and on the extended wrapper on their own.
* `make lint-cpp` fails on `sim/unit/tb_core_cache_path.cpp` (members
  `cb_cpu_req_size_i`, `cb_cpu_req_wstrb_i` do not exist in the generated
  `Vmosaic_core_tb`), another lane's file. `sim/unit/tb_vec.cpp` compiles clean
  under the same `-Wall -Wextra -Wshadow`.

Passing gates for this package: `make check`, `make lint PROFILE=p0`,
`make lint PROFILE=p2` (Verilator `--lint-only -Wall`, both profiles),
`python3 tools/check_records.py`, `python3 tools/check_exclusions.py` and
`--negative` (13/13 controls rejected).
