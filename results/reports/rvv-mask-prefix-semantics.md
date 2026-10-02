# `rvv.mask_prefix_semantics` — the three mask-prefix rules and their boundaries

The registered case **`rvv.mask_prefix_semantics`** (top `mosaic_vec_tb`, driver
`sim/unit/tb_vec.cpp`, phase `RunMaskPrefixSemanticsCase`). It encodes the
specification's three mask-prefix rules, fixes the element lane that gave
`vmsbf` and `vmsif` the same expression, and closes the first-set-bit coverage
the integer/mask/permute package left open.

## STATUS: COMPLETE (unit)

| Acceptance item | Evidence |
| --- | --- |
| The three rules are established from the specification, not the RTL | §"The rules, as rules"; anchored to the spec's own worked examples below |
| All three correct in RTL | `rtl/core/mosaic_vec_alu.sv` `F_MASKPFX`: `vmsbf = ~(pfx\|bbit)`, `vmsif = ~pfx`, `vmsof = bbit & ~pfx` |
| First set bit at every addressable position, all three ops | `RunMaskPrefixSemanticsCase`: 3 ops × 16 positions = 48 cells, plus a coverage assertion; PASS |
| Degenerate cases: all-zero, position 0, bit above `vl` | explicit, spec-derived checks (not the shared oracle); PASS |
| Oracle is the host model, not a second RTL copy | `ComputeExpected`+`OracleElem` for the sweep, plus 9 spec-anchor cells and the degenerate cells stated directly |
| `rvv.mask_prefix_semantics` runs and passes | `RESULT PASS … checks=1115 ops=3 positions=16 allzero=3 anchors=9 cycles=12454`, exit 0 |
| Deterministic | PASS with identical `checks=1115` at seeds 1, 2, 7, 12345 |
| Fail mode — `vmsbf`/`vmsif` identical again (the defect as found) | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_SAME_EXPR`, exit 1 first failure `spec-anchor vmsbf src=148 k-bit2: 1 expected 0` |
| Fail mode — all-zero mask wrong | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_ALLZERO`, exit 1 first failure `all-zero vmsbf bit15: 0 expected 1 (all-zero is not symmetric among the three)` |
| Fail mode — position 0 wrong | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_POS0`, exit 1 first failure `spec-anchor vmsbf src=149 k-bit0: 1 expected 0` |
| Mask-logic neighbours checked against the specification | §"Second reader"; no disagreement found |
| Further disagreement found and reported (not fixed silently) | §"Second finding": mask destination byte addressing for element ≥ 8; fixed because the case cannot reach position 12 otherwise |
| Sibling vector cases unchanged | `rvv.integer_mask_permute`, `rvv.mask_prefix_vstart`, `rvv.memory_modes`, `rvv.partial_fault_restart`, `rvv.fp_flags_reduction`, `rvv.chaining_hazards`, `rvv.descriptor_legality`, `rvv.vset_boundaries`, `rvv.vtype_layout`, `rvv.lane_resize_boundary`, `vrf.mapping_aliases`, `vec.integrated` all PASS |
| `core.act_dut` (p1) | `RESULT PASS core.act_dut applicable=127 generated=127 run=127 passed=127 failed=0 act_commit=96493a91448ca50780013fd892daec2c204487ba` |
| `make check` | exit 0 |
| `lint_rtl` p0/p1 | `60 source file(s) clean` under both profiles, exit 0 |
| `slang-tidy` (`make lint-slang`) | exit 0 |
| `make lint-cpp` | `72 file(s) clean`, exit 0 |
| `check_records`, `check_exclusions`, `--negative` | exit 0 (the new case is registered `pending`, so the exclusion ledger does not need it yet) |

## The rules, as rules

Source: the pinned V spec, `v-spec.adoc` (repo pin `3570f998`), the three
subsections `vmsbf.m` set-before-first, `vmsif.m` set-including-first,
`vmsof.m` set-only-first. Let the source mask's **first set bit over the active
elements** `[0, vl)` be at position `k`; "no `k`" means every active source bit
is zero. Then, for every active element `i`:

* **`vmsbf.m`** — *before the first*: `vd[i] = 1` iff `i < k`.
  With no `k`, the specification says so explicitly: *"If there is no set bit in
  the active elements of the source vector, then all active elements in the
  destination are written with a 1."*
* **`vmsif.m`** — *through the first*: `vd[i] = 1` iff `i <= k`.
  The specification defines it as *"similar to set-before-first, except it also
  includes the element with a set bit"*. With no such element there is nothing to
  include, so it coincides with set-before-first: **all active elements set**.
* **`vmsof.m`** — *only the first*: `vd[i] = 1` iff `i == k`.
  With no `k`, *"it only sets the first element with a bit set, if any"* — no
  such element, so **no active element is set**.

Expressed through the running prefix `pfx` = OR of the source bits strictly
before element `i`, and this element's source bit `bb`:

| instruction | rule | closed form | all-zero |
| --- | --- | --- | --- |
| `vmsbf` | `1` iff `i < k` | `~(pfx \| bb)` | all-ones |
| `vmsif` | `1` iff `i <= k` | `~pfx` | all-ones |
| `vmsof` | `1` iff `i == k` | `bb & ~pfx` | all-zeros |

**The all-zero source is not symmetric among the three.** `vmsbf` and `vmsif`
are all-ones; `vmsof` is all-zeros. This is the specification's answer, it
surprised this lane, and it is stated here rather than smoothed over. Two
independent reference implementations agree: Spike `riscv/insns/vmsbf_m.h`,
`vmsif_m.h`, `vmsof_m.h` and NEMU `vsrc/isa/riscv64/instr/rvv/vcompute.h`
(`def_EHelper(vmsbf)`, `(vmsif)`, `(vmsof)`), both of which leave the
set-before/including-first result at 1 when `has_one` never becomes true and
leave set-only-first at 0.

The previous package's defect follows directly: it gave `vmsbf` and `vmsif` the
same expression `~pfx`, which is `vmsif`'s rule. `vmsbf` is wrong at `i == k`
(and, through the same bit, at position 0 for a source whose first set bit is at
0) for every source that has a set bit. The old expression happened to be right
on the all-zero source for both, so an all-zero-only test would have missed it —
which is why the case sweeps every position and not just the boundary.

## Degenerate cases, spec vs machine

| Case | Specification | Machine (after the fix) |
| --- | --- | --- |
| all-zero active slice | `vmsbf` all-ones, `vmsif` all-ones, `vmsof` all-zeros | same; asserted directly, not via the shared oracle |
| first set bit at position 0 (`k = 0`) | `vmsbf` all-zeros, `vmsif` bit 0 only, `vmsof` bit 0 only | same; `vmsbf` all-zeros is exactly where the old `~pfx` expression failed |
| set bit above `vl` only | not an active source element: the active slice is all-zero, so the rules apply over `[0, vl)`; bits `>= vl` are tail | same; `vl = 8`, bit 12 set ⇒ bits 0..7 all-ones for `vmsbf`/`vmsif`, all-zeros for `vmsof`; bits 8..15 undisturbed (`vta = 0`) |
| set bit below and above `vl` | the search is bounded by `[0, vl)`, so the lower set bit is `k` | same; bit 5 and bit 12 with `vl = 8` ⇒ `k = 5` (`vmsbf 0x1F`, `vmsif 0x3F`, `vmsof 0x20`), bit 12 ignored |
| tail (`vta = 1`) | mask tails are agnostic; this unit writes all-ones | same; bits 8..15 = 1 while the active bits follow the rule |

## Coverage table — operation × first-set-bit position

`k` is the position of the source's only set bit. Active span is `[0, 16)`, the
mask addressable range for SEW=8, LMUL=1 at VLEN=128. Every cell is driven by
`RunMaskPrefixSemanticsCase` and compared to the host model; the bound is
asserted (`position_cells == 3 × 16`, `pos0_cells == 3`).

| op | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
| --- | - | - | - | - | - | - | - | - | - | - | - | - | -- | -- | -- | -- |
| `vmsbf` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `vmsif` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `vmsof` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

Each cell's expected result is the active mask `i < k` / `i <= k` / `i == k`;
the case's coverage assertion fails if any cell does not run.

## Oracle identity

* The sweep's expectation is the shared host model
  `ComputeExpected(...)` → `OracleElem(VF_MASKPFX, ...)` in `sim/unit/tb_vec.cpp`,
  whose `VF_MASKPFX` arm now states the three rules in the closed forms above.
  It is a C++ model written from the specification; it is not derived from the
  RTL and does not share the RTL's expression shapes.
* The nine **spec-anchor** cells do not use the oracle at all: they are the
  specification's own printed input/output pairs —
  `v3 = 10010100 → vmsbf 0000011 / vmsif 0000111 / vmsof 0000100`,
  `v3 = 10010101 → 0000000 / 0000001 / 0000001`,
  first bit at the top `→ 0111111 / 1111111 / 1000000`.
  A shared misreading of the rules cannot make both sides agree, because the
  anchors come from the document and the sweep comes from the model.
* The all-zero, above-`vl` and tail cells likewise state their expected value
  directly from the rules.

## Changes

* **`rtl/core/mosaic_vec_alu.sv`** — the `F_MASKPFX` element-lane arm:
  `vmsbf` becomes `~(ef_pfx | bbit)`; `vmsif` stays `~ef_pfx`; `vmsof` stays
  `bbit & ~ef_pfx`. Three `-D` controls are added in the same arm, and the
  module header's mutant list gains them.
* **`rtl/core/mosaic_vec_alu.sv`** — the mask-destination write element in
  `wr_elem_c` (see §"Second finding"): `cur_q >> 3`, not `(cur_q >> 3) << 3`.
* **`sim/unit/tb_vec.cpp`** — the `OracleElem` `VF_MASKPFX` arm corrected to the
  three rules; `RunMaskPrefixSemanticsCase(...)` appended additively before
  `main`, with its dispatch branch and PASS summary. Existing phases untouched.
* **`tools/run_vec_maskpfx_controls.py`** — generalised with `--case` so one
  tool controls both mask-prefix cases; the vstart case set and its default
  behaviour are unchanged, and a new `MUTANTS_SEMANTICS` set carries the three
  mutants below.

## Mutant table

Each mutant is built from a **deleted** build directory
(`build/p0/vec_maskpfx_controls/<define>/`), has its `-D` on the recorded
`build_command.txt`, produces a binary whose sha256 differs from the shipping
one, exits 1, and names the first failing check. Tool:
`python3 tools/run_vec_maskpfx_controls.py --profile p0 --case rvv.mask_prefix_semantics`.

Shipping binary sha256:
`d6c05e9453d18539df2cdb6408c4d4849681c79851445b24c3144875b9b02295`
(carries `checks=1115`).

| mutant | defect injected | exit | binary sha256 | named first failure |
| --- | --- | --- | --- | --- |
| `MASKPFX_SAME_EXPR` | `vmsbf` and `vmsif` share the set-including-first expression again — the defect as found | 1 | `254874bcdf0c6ca758a1bfcd1df54e5a102ec187030695ed15529fdfde684979` | `spec-anchor vmsbf src=148 k-bit2: 1 expected 0` |
| `MASKPFX_ALLZERO` | an all-zero active source is cleared for `vmsbf`/`vmsif` instead of all-ones | 1 | `65e48ab46262324040f5e5fc5acf290be93706fff660ddaa7bef44d30f17f87c` | `all-zero vmsbf bit15: 0 expected 1 (all-zero is not symmetric among the three)` |
| `MASKPFX_POS0` | `vmsbf` reports a bit set before the first element when element 0 is itself the first set bit | 1 | `c4973ea465c108c9d707a8dee080e39885c9114513317f81853ff802d380b1bf` | `spec-anchor vmsbf src=149 k-bit0: 1 expected 0` |

The same tool still reports **3 of 3** for the existing
`CASE=rvv.mask_prefix_vstart` mutants (`MASKPFX_NO_TRAP`, `MASKPFX_TRAP_VSTART0`,
`MASKPFX_WRONG_OP`), so the generalisation did not change their behaviour.

Note on the all-zero mutant: "all-zero" is a **whole-instruction** property, not
an element-local one — for a source whose first set bit is `k > i`, element `i`
sees exactly the same local state `(pfx = 0, bb = 0)` as it does for an all-zero
source, and the correct output is 1 in both cases. No per-element function can
distinguish them, so the control injects the defect where the whole active-slice
OR becomes known, at the final active element. It is therefore wrong for the
all-zero source and correct for every source with a set bit; it is not
"wrong everywhere".

## Second reader: the mask family's neighbours

Read against `v-spec.adoc` §"Mask-Register Logical Instructions":

| instruction | spec | RTL (`F_MASKLOG`) | agree |
| --- | --- | --- | --- |
| `vmand` | `vs2 & vs1` | `bbit & abit` | ✓ |
| `vmnand` | `!(vs2 & vs1)` | `~(bbit & abit)` | ✓ |
| `vmor` | `vs2 \| vs1` | `bbit \| abit` | ✓ |
| `vmnor` | `!(vs2 \| vs1)` | `~(bbit \| abit)` | ✓ |
| `vmxor` | `vs2 ^ vs1` | `bbit ^ abit` | ✓ |
| `vmxnor` | `!(vs2 ^ vs1)` | `~(bbit ^ abit)` | ✓ |
| `vmandn` | `vs2 & !vs1` | `bbit & ~abit` | ✓ |
| `vmorn` | `vs2 \| !vs1` | `bbit \| ~abit` | ✓ |

with `bbit` = `vs2` (the `TAG_SRC2` read), `abit` = `vs1` (`TAG_SRC1`), and the
mask-logical masking/tail policy exactly as for `vmsbf`/`vmsif`/`vmsof`
(masked-off ⇒ `vma`, tail ⇒ `vta`). No disagreement found; the neighbours are
not modified.

`vmerge`/`vmv*` are deliberately not implemented and carry no capability bit
(`mosaic_vec_alu.sv` header, "Deliberately NOT implemented"), so the
`vmerge`-adjacent behaviour those moves imply cannot be checked here — it is not
in the machine, not a silent divergence.

## Second finding: mask destination byte addressing (reported, and fixed because the case needs it)

The case failed before any RTL semantics fix because the packet engine's mask
**destination write element** was wrong for element ≥ 8:

```
wr_elem_c = (dst_kind == DST_MASK) ? {cur_q[6:3], 3'b000} : cur_q[6:0];
```

`{cur_q[6:3], 3'b000}` is `(i/8)*8` used as the VRF element, while the
destination **read** path (`E_DSTRD`) uses `cur_q >> 3`. For `i` in 0..7 both are
0, which is why every existing packet mask case (which stops at `vl = 8`) passed;
for `i` in 8..15 the write landed on element 8 and the destination bytes 1..15
kept their stale value. This is a real disagreement with the specification for
any mask-destination instruction whose `vl` exceeds 8 — not only the
mask-prefix family. It is fixed here (`7'(cur_q >> 3)`, matching the read path
and the code's own comment) because `rvv.mask_prefix_semantics` cannot reach
first-set-bit positions 8..15 without it. With the fix, positions 8..15 pass.

No other mask-destination instruction in the current suite drives `vl > 8`
through the packet path, so this finding was invisible until now; it is recorded
here rather than folded into the semantics claim.

## Not covered

* **`vta`/destination-agnostic interaction.** The case exercises `vta = 0`
  (tail undisturbed) and `vta = 1` (tail all-ones) for one configuration; the
  specification permits either as an agnostic choice and allows mixing, so this
  pins the choice the unit makes, not the full agnostic space. `vma` on
  masked-off mask destinations is not driven (mask-enable is false throughout;
  the mask **source** is what the prefix rules read). Masked mask-prefix
  sources — where the first set bit is searched over *active* elements only, as
  the spec's `vmsbf.m v2, v3, v0.t` example shows — are **not** covered by this
  case; the case is unmasked.
* **`vstart` interaction at the core level.** `rvv.mask_prefix_vstart` (I-057)
  covers the ALU-level illegal-instruction output, including the boundary lane.
  The *core* path — `vec_unit_illegal` → `VEC_TRAP` in `mosaic_core.sv`
  (`vec_alu_illegal` at the `mosaic_vec_alu` instance) — is not driven by any
  case: `vec.integrated`/`tb_core_vec.cpp` exercises the VS=Off illegal vector
  path only. So a mask-prefix instruction with non-zero `vstart` reaching the
  core and taking a trap is still untested at the core level. This was noted by
  the previous lane and remains open.
* **Register-overlap constraints.** The specification forbids `vd` overlapping
  `vs2` (and `v0` when masked) for `vmsbf`/`vmsif`/`vmsof`; this unit takes the
  (vd, vs1, vs2) operand bases from the packet and does not itself enforce
  overlap. Enforcement, if any, is a decode/core legality concern and is not
  exercised here.
* **Only SEW=8, LMUL=1.** The mask register's addressable range is 16 bits in
  this unit (`vlmax = VLEN/SEW·LMUL` with SEW=8, LMUL=1); the case covers all 16.
  Mask instruction behaviour at other SEW/LMUL is not driven because mask
  instructions do not depend on SEW/LMUL beyond `vlmax`.
* **Floor of the gate set.** Green at the recorded point: `make check` exit 0
  (records, exclusions, exclusions `--negative`, contracts, config, coverage,
  capability matrix, upstream, isolation); `lint_rtl` p0 and p1 `60 source
  file(s) clean`; `make lint-slang` exit 0; `make lint-cpp` `72 file(s) clean`.
  Mid-flight the concurrent I-061 (`CoalesceVec`) edits to `mosaic_vec_lsu.sv`
  transiently broke p0/p1 lint and `vec.integrated`; once that lane landed, all
  of the above were re-run green. This change touches only the mask-prefix arm
  and the mask-destination address in `mosaic_vec_alu.sv`, the `VF_MASKPFX`
  oracle and the new case in `tb_vec.cpp`, and the controls tool.

## Evidence commands

```
python3 tools/run_unit.py --profile p0 --case rvv.mask_prefix_semantics
python3 tools/run_unit.py --profile p1 --case core.act_dut
python3 tools/run_vec_maskpfx_controls.py --profile p0 --case rvv.mask_prefix_semantics
make check
python3 tools/check_records.py && python3 tools/check_exclusions.py && python3 tools/check_exclusions.py --negative
```
