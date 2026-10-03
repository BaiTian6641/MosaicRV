# `vec.mask_prefix_at_core` — the mask families dispatched by the core (I-054)

**STATUS: COMPLETE.** The registered case `vec.mask_prefix_at_core`
(`tests/unit/registry.json`, task `I-054`, top `mosaic_core_tb`, harness
`sim/unit/tb_core_vec.cpp`) is implemented and passes from a deleted build
directory. The core's vector ALU capability word now advertises the two mask
families that have delivered unit-level evidence, the operations are decoded
and dispatched through fetch/decode/rename/issue/the vector engine, and the
`vstart` illegal routing is checked at the core level. Two negative controls
each mutate the binary and fail the case with a named check.

> The registered case was `"pending": true` and had never been run; it is left
> registered and untouched (`tests/unit/registry.json` and
> `config/status/implementation_status.json` were not edited). The integration
> lead clears the marker.

```
RESULT PASS vec.mask_prefix_at_core checks=112 phases=4 seed=1 plain_cycles=7723 masked_cycles=27063 masklog_cycles=5287 vstart_cycles=446 plain_retire=57 masked_retire=226 masklog_retire=37
```

Both the `p0` and `p1` profiles PASS with the identical `checks=112` line.
`p2`/`p3` do not build this case for a **pre-existing** reason unrelated to this
package: their generated `mosaic_cfg_pkg.svh` does not define the core's
geometry macros (`MOSAIC_MSHR_ENTRIES`, …), so every core case fails to
elaborate there (see "Not covered").

---

## 1. What was advertised, and the evidence for each family

`rtl/core/mosaic_core.sv`, the ALU capability word (width and style kept):

```
localparam logic [16:0] VEC_ALU_CAPS = 17'b0001100_0100_0001;
// ADDSUB + LOGIC + MASKLOG + MASKPFX
```

| bit | family | operations | evidence that justifies advertising it |
|-----|--------|-----------|----------------------------------------|
| 0 | ADDSUB | vadd, vsub, vrsub | the families the core dispatched first; CASE=`vec.integrated` |
| 6 | LOGIC | vand, vor, vxor | the families the core dispatched first; CASE=`vec.integrated` |
| 10 | MASKLOG | vmand, vmnand, vmandn, vmxor, vmor, vmnor, vmorn, vmxnor | CASE=`rvv.integer_mask_permute` drives **all eight ops** in `PhaseMaskLane` (`for (op = 0; op < 8; ++op)` with `VF_MASKLOG`), against the host oracle; the case reports `checks=99952 cells=346 families=17` PASS |
| 11 | MASKPFX | vmsbf, vmsif, vmsof | CASE=`rvv.integer_mask_permute` drives all three in `PhaseMaskLane`; CASE=`rvv.mask_prefix_semantics` (`checks=1115 ops=3 positions=16 allzero=3 anchors=9`), `rvv.mask_prefix_masked` (`checks=2870 ops=3 masked_before=45 masked_only=48 vma=12 vta=12`) and `rvv.mask_prefix_vstart` (`checks=79 ops=3 vstart_cells=6`) cover the boundaries, the masked forms and the `vstart` rule |

`MASKLOG` was **not** taken on the strength of the case's name: the stimulus
was read. `PhaseMaskLane` loops `op` over 0..7 for `VF_MASKLOG` and compares each
result and every bit against `OracleElem(VF_MASKLOG, op, …)`, so all eight
operations have delivered unit-level evidence. The `MASKPFX` loop in the same
phase covers ops 0..2.

### The capability word was not the only gate

The gap statement named the capability word. Reading the front end shows a
second, earlier gate: before this change the OP-V decode in `mosaic_core.sv`
handled only OPIVV/OPIVX/OPIVI for `vadd`/`vsub`/`vand`/`vor`/`vxor`, and
`vset{i}vl{i}` and vector loads/stores. **No OPMVV instruction decoded at all**,
so the mask operations were unreachable even with a set capability bit. Both
gates were therefore closed:

* the decode gained the OPMVV (`funct3=010`) arm, from the `riscv-opcodes`
  `rv_v` encodings:

  | instruction | funct6 | rs1 field | family | ALU op |
  |---|---|---|---|---|
  | `vmsbf.m` | 010100 | 00001 | 11 MASKPFX | 0 |
  | `vmsof.m` | 010100 | 00010 | 11 MASKPFX | 2 |
  | `vmsif.m` | 010100 | 00011 | 11 MASKPFX | 1 |
  | `vmandn.mm` | 011000 | vs1 | 10 MASKLOG | 6 |
  | `vmand.mm` | 011001 | vs1 | 10 MASKLOG | 0 |
  | `vmor.mm` | 011010 | vs1 | 10 MASKLOG | 2 |
  | `vmxor.mm` | 011011 | vs1 | 10 MASKLOG | 4 |
  | `vmorn.mm` | 011100 | vs1 | 10 MASKLOG | 7 |
  | `vmnand.mm` | 011101 | vs1 | 10 MASKLOG | 1 |
  | `vmnor.mm` | 011110 | vs1 | 10 MASKLOG | 3 |
  | `vmxnor.mm` | 011111 | vs1 | 10 MASKLOG | 5 |

  `vmsbf/vmsif/vmsof` use descriptor class `VOP_VMASKMV` (dst is a mask group,
  `vd` may not overlap `vs2`); the mask-logical ops use `VOP_VMASK` (dst is a
  mask group, overlap allowed). The mask-logical `vm=0` encodings are reserved,
  so only `vm=1` decodes.

* `VEC_ALU_CAPS` sets bits 10 and 11.

No other family bit was set: the remaining thirteen ALU families
(WIDE, MUL, MULW, SHIFT, NARROW, MINMAX, CMP, SAT, SLIDE, GATHER, COMPRESS,
REDUCE, REDWIDE) are neither decoded by this core nor proven at core level, so
advertising them would be a claim with no evidence.

---

## 2. The core-level checks (112)

Every phase is a fresh reset and a fresh program that hand-encodes RV64I + V and
drives the core's ports; the mask results are read back as **architectural
state** by storing the destination mask register with `vse8.v` and comparing
the 16 bytes it writes against a host model. The ALU is never poked.

### 2.1 The reference model is the unit-level model

The host model is `sim/unit/mask_prefix_ref.h`, and **the unit-level cases now
call it too**: `sim/unit/tb_vec.cpp`'s `OracleElem` `VF_MASKPFX` and
`VF_MASKLOG` branches and its `MaskedPrefixExpectedBits` are thin callers of
`mosaic_maskpfx::Elem` / `MaskLogElem` / `MaskedExpectedBits`. One statement of
the rules, two consumers — the core-level case cannot drift from the
unit-level evidence it advertises. The rules are the pinned V spec's
(`v-spec.adoc`, "Vector Mask Instructions").

| phase | cells | what it checks | model |
|---|---|---|---|
| `prefix-plain` | 24 (3 ops × 8 source patterns) | unmasked `vmsbf`/`vmsif`/`vmsof`, 16-bit destination mask from the `vse8.v` store | `mosaic_maskpfx::ExpectedBits` |
| `prefix-masked` | 45 (3 ops × 15 `k`) | the `v0.t` form with a set bit at element 0 **masked off** and the first active set bit at `k` — the 45 cells the unit-level masked evidence calls interesting; `vma=vta=0`, seed `0x5555` | `mosaic_maskpfx::MaskedExpectedBits` |
| `masklog` | 16 (8 ops × 2 mask pairs) | all eight mask-register logical ops | `mosaic_maskpfx::MaskLogExpectedBits` |
| `vstart` | 9 | the illegal routing (below) | — |

The plain patterns place the first set bit at 0, 1, 7, 8 and 15, include the
all-zero boundary and two dense patterns, so every boundary the semantics case
names is exercised through the core.

Positive evidence that the vector path was taken is required, not assumed:

* `prefix-plain`: `vl = 16` after `vsetvli e8/m1`, **57** vector macros retired
  (vset + 8 loads + 24 ops + 24 stores), **56** descriptors allocated and
  released (the vset allocates none), the last mask-prefix op wrote **16**
  elements, `vec_vrf_bad = 0`.
* `prefix-masked`: **226** macros retired (vset + 45 × (source load + mask load +
  seed load + op + store)), every descriptor released, the last op wrote **15**
  elements (element 0 is masked off), `vec_vrf_bad = 0`.
* `masklog`: **37** macros retired, last op wrote 16 elements, `vec_vrf_bad = 0`.

No phase takes a trap.

---

## 3. The `vstart` rule as tested

The unit-level case `rvv.mask_prefix_vstart` states the rule in three parts
(`results/reports/rvv-mask-prefix-vstart.md`):

* **R1 — illegal, not partial.** `vmsbf`/`vmsif`/`vmsof` raise an
  illegal-instruction exception when `vstart` is non-zero; the rule is a
  property of the whole instruction.
* **R2 — nothing happens.** No source element is read, no destination element
  is written; it is **not** an element fault.
* **R3 — `vstart == 0` is ordinary execution** (phase `prefix-plain`).

The core-level `vstart` phase writes `vstart = 2` through the CSR after the load
that would otherwise clear it, then executes `vmsbf.m v8, v16` (unmasked). The
checks, and what they observed:

| check | observed |
|---|---|
| exactly one trap | 1 |
| `mcause` is illegal-instruction | **2** |
| `mepc` is the instruction's own PC | the emitted `vmsbf` address |
| `mtval` | **0** (no element address) |
| `vstart` read by the handler | **2** |
| destination mask register unchanged | `0x5A5A` (the seed) |
| the vector engine counted one trap | `vec_trap = 1` |
| no element fault | `vec_fault = 0` |
| macros retired | 4 (vset + 2 loads + the store; the refused op did not) |

**No core-vs-unit disagreement was found.** The refusal routes to an
illegal-instruction trap at the instruction's PC, not to an element fault, and
the destination is untouched — R1 and R2 hold at the core level.

On the `vstart` CSR value: the V spec's `vstart` section is explicit — *"`vstart`
is not modified by vector instructions that raise illegal-instruction
exceptions"* — and the core follows it (the handler reads the `2` software
wrote). The mask-instruction section's *"traps on `vmsbf.m` are always reported
with a `vstart` of 0"* is the restart-index statement (these three cannot be
restarted part-way, so there is no element to resume at); it does not require
the trap to overwrite the CSR. The phase tests the CSR section's rule and
records the spec sentence it follows. The handler then clears `vstart` itself,
software's own rule, before the following store runs.

---

## 4. Controls

`tools/run_vec_maskpfx_core_controls.py` (new), modelled on
`tools/run_vec_integrated_controls.py`. Each mutant is built from a **deleted**
build directory with its `-D` on the recorded command line; a control passes
only when the mutant binary differs from the shipping one, exits 1, and the
first failing check names the claim it broke.

Shipping binary (both `build/p0/unit/vec.mask_prefix_at_core` and the controls'
shipping build):
`54b809d930b8b71e14c55ba63e279350f077a7f79532b9dec64354a83f350826`.

| control (`-D`) | injects | exit | binary sha256 | first failure |
|---|---|---|---|---|
| `MOSAIC_CORE_MUTANT_VEC_CAPS_NO_MASKPFX` | clears bit 11, so the ALU refuses `vmsbf`/`vmsif`/`vmsof` again | 1 | `15b05a8b0dd687373adfd8c2b05914b4f369c5cbe819853333b0a0911f49bd17` | `CHECK FAILED: prefix-plain: no trap is taken` |
| `MOSAIC_CORE_MUTANT_VEC_ILLEGAL_AS_FAULT` | a refused vector instruction is reported as a resumable element fault at `vstart` (mcause 5) instead of an illegal instruction | 1 | `369dc6b9f4b96c02e67eddbb123c3fe90938ac15237338f09c5b02a78525edf7` | `CHECK FAILED: vstart: the refusal is an illegal instruction (mcause=2), got 5` |

* The first control is **the advertisement control**: it clears the mask
  family's capability bit, so the case fails at the first plain mask-prefix
  cell. That is what proves the case tests the advertisement rather than merely
  running.
* The second control is **a mutant of the routing**: it changes the *core's*
  reporting of the refusal (a resumable element fault at `vstart` instead of an
  illegal instruction), which is exactly the routing claim the `vstart` phase
  makes. No malformed-input control was needed — a mutant expresses the rule.
* Both controls were also run under `p1` with the same first failures. `p1`
  shipping `b041083d06f22b699831de61be53ece2daefef8a8071d7a9652ceb82abf665f4`;
  caps control `5087d4770f6bc3c518e2ea6789caeaa21fb06f67da06a48482b2d3b24741f13c`;
  routing control `c41980c90e0ac3907df0d1429d51112024a5edfcc5ad320299b5277777938e26`.

The pre-existing unit-level controls (`tools/run_vec_maskpfx_controls.py`) were
re-run after the model refactor: all three ALU `vstart` mutants still mutate the
binary, exit 1 and name their check.

---

## 5. Regression state of the cases this could disturb

| case | result |
|---|---|
| `vec.mask_prefix_at_core` (p0, p1) | **PASS** `checks=112` |
| `vec.integrated` (uses the same `tb_core_vec.cpp`) | PASS |
| `rvv.descriptor_legality` (uses the same `tb_vec.cpp`) | PASS |
| `rvv.integer_mask_permute`, `rvv.mask_prefix_semantics`, `rvv.mask_prefix_masked`, `rvv.mask_prefix_vstart` (the refactored model) | PASS |
| `core.corpus_sweep` | PASS |
| `core.act_dut` | **FAIL — pre-existing, not caused by this change** |

`core.act_dut` fails in the current working tree (all 127 ACT ELFs, trap
signature mismatch in M-mode, `first_fail=.../I/I-add-00.elf`). It is **not**
caused by this package: rebuilding `core.act_dut` with this package's
`mosaic_core.sv` reverted to `HEAD` (only the sibling lanes' committed work and
their in-flight `mosaic_cache.sv` edit present) fails identically. The recorded
`results/unit/core.act_dut/result.json` has been overwritten with the fresh
FAIL by that run; the last committed PASS predates the intervening
front-end/LLB commits.

Gates: `python3 tools/lint_rtl.py --profile p0` → **65/65 clean**,
`--profile p1` → **65/65 clean**; `python3 tools/check_records.py` → ok (81
delivered packages, 97 registered cases); `python3 tools/check_exclusions.py` →
ok; `make check` → exit 0.

---

## 6. Not covered

* **Thirteen ALU families remain unadvertised**: WIDE, MUL, MULW, SHIFT,
  NARROW, MINMAX, CMP, SAT, SLIDE, GATHER, COMPRESS, REDUCE, REDWIDE. Their ALU
  implementations exist and some have unit-level evidence, but the core has no
  decode for their encodings and no core-level case proves them, so they stay
  out of `VEC_ALU_CAPS`.
* **Other mask instructions cannot be dispatched**: `vcpop.m`, `vfirst.m`,
  `viota.m` and `vid.v` have no ALU family at all (no bit in the 17-bit word)
  and are not decoded; they remain unimplemented. The mask-logical
  pseudoinstructions (`vmmv.m`, `vmclr.m`, `vmset.m`, `vmnot.m`) are dispatched
  as their base operations. Mask-register logical encodings with `vm=0` are
  reserved and are refused at decode.
* **The only mask-prefix form that cannot be dispatched is the architecturally
  illegal one**: `vmsbf`/`vmsif`/`vmsof` with a non-zero `vstart` are refused by
  design (section 3). Both the `vm=1` and `vm=0` forms at `vstart == 0` are
  dispatched and checked.
* **`p2`/`p3` do not build this case** (nor any core case): the generated
  `mosaic_cfg_pkg.svh` for those profiles lacks the core geometry macros
  (`MOSAIC_MSHR_ENTRIES`, …), a pre-existing profile-config gap outside this
  package.
* The case exercises one configuration shape, `e8/m1` (VLMAX = 16). Wider SEW
  and LMUL for the mask families are covered at unit level
  (`rvv.integer_mask_permute` sweeps SEW 8..64), not at core level; the
  mask-prefix `vstart` rule is SEW-independent.

---

## 7. Files

* **modified** `rtl/core/mosaic_core.sv` — the `VEC_ALU_CAPS` word (bits 10 and
  11, with the evidence comment), the OPMVV decode arm, and two `-D` controls
  (`MOSAIC_CORE_MUTANT_VEC_CAPS_NO_MASKPFX`,
  `MOSAIC_CORE_MUTANT_VEC_ILLEGAL_AS_FAULT`).
* **new** `sim/unit/mask_prefix_ref.h` — the shared mask-register model.
* **modified** `sim/unit/tb_vec.cpp` — `OracleElem`'s `VF_MASKPFX`/`VF_MASKLOG`
  branches and `MaskedPrefixExpectedBits` now call the shared model.
* **modified** `sim/unit/tb_core_vec.cpp` — the four phases, their dispatch, and
  the `Vle8`/`Vse8`/`VmaskPrefix`/`VmaskLog` encoders.
* **new** `tools/run_vec_maskpfx_core_controls.py` — the two controls.
* **not edited**: `tests/unit/registry.json`,
  `config/status/implementation_status.json`, `results/PROGRESS.md`, `docs/*.md`,
  `rtl/core/mosaic_dispatch.sv`, `tools/run_frontend_controls.py`.
