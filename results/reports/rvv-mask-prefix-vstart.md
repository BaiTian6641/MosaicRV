# `rvv.mask_prefix_vstart` — the mask-prefix `vstart` rule (I-057)

Work package **I-057** of `docs/implementation-plan.md` §3.1, the mask-prefix
part of the vector restart rule, implementing

> `vmsbf.m` / `vmsif.m` / `vmsof.m` — the three prefix-mask instructions —
> cannot be restarted from an element boundary. The pinned specification makes a
> non-zero `vstart` an **illegal-instruction** exception for exactly these three,
> and `vstart == 0` must still execute normally.

`I-054` (§"Deliberately not covered") and `I-057` both recorded this as an open
gap: *"`vmsof`/`vmsbf`/`vmsif` with `vstart != 0` must raise illegal-instruction
per the specification; the unit implements the ordering but does not raise on a
non-zero `vstart`."* This package closes it and registers the case that proves it.

## STATUS: COMPLETE (unit)

| Acceptance item | Evidence |
| --- | --- |
| `vmsbf`/`vmsif`/`vmsof` with a non-zero `vstart` raise an illegal instruction | `RunMaskPrefixVstartCase`: 3 ops × `vstart` ∈ {1, 3}; `alu_illegal` observed for all six; `rvv.mask_prefix_vstart` PASS |
| The refused instruction is an exception, **not** an element fault | `!alu_trap` for every illegal cell; the case distinguishes the two outputs |
| The refused instruction touches nothing | `src_rd_ctr == 0`, `alu_elems == 0`, and the destination mask byte read back equal to its pre-value for every illegal cell |
| `vstart == 0` still executes normally | 3 ops execute, `alu_elems == 8`, and every destination bit equals the host model's expected prefix mask |
| The rule also guards the boundary element lane | `el_illegal_o` for each of the three ops at `vstart == 2`, and not at `vstart == 0` |
| Fail mode — no exception at all | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_NO_TRAP`, exit 1 on `vmsbf vstart=1: a non-zero vstart did not raise an illegal instruction` |
| Fail mode — exception at `vstart == 0` too | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_TRAP_VSTART0`, exit 1 on `vmsbf vstart=0: the instruction was refused as illegal` |
| Fail mode — exception for the wrong one of the three | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_WRONG_OP`, exit 1 on `vmsof vstart=1: a non-zero vstart did not raise an illegal instruction` |
| Registered case runs and passes | `RESULT PASS rvv.mask_prefix_vstart checks=79 ops=3 vstart_cells=6 cycles=394`, exit 0 |
| The same case under `p1` | `PASS rvv.mask_prefix_vstart`, exit 0 |
| `make check` (records + exclusions + contracts + config) | exit 0 |
| Deterministic across seeds | PASS, identical `checks=79 ops=3 vstart_cells=6` at seeds 1, 2, 7 and 12345 |
| Sibling vector cases unchanged | `rvv.integer_mask_permute`, `rvv.partial_fault_restart`, `rvv.chaining_hazards`, `rvv.memory_modes`, `rvv.fp_flags_reduction`, `rvv.vtype_layout`, `rvv.vset_boundaries`, `rvv.descriptor_legality`, `vrf.mapping_aliases` all PASS after the change |
| The changed RTL lints clean | `verilator --lint-only -Wall -Wno-DECLFILENAME` on `mosaic_vec_alu.sv` under both `p0` and `p1`, exit 0 |
| The changed C++ is warning-clean | `c++ -std=c++17 -Wall -Wextra -fsyntax-only` on `tb_vec.cpp`; only the Verilator runtime headers warn |
| `python3 tools/run_vec_maskpfx_controls.py --profile p0` | 3 of 3 mutants mutate the binary, exit 1 and name the check they break |
| This report | — |

## The rule, as a rule

From the pinned V spec (`v-spec.adoc`, §"Vector Mask Instructions", the three
prefix-mask subsections, which the repo pins at tag `3570f998`):

* **R1 — illegal, not partial.** *"Traps on `vmsbf.m` are always reported with a
  `vstart` of 0. The `vmsbf` instruction will raise an illegal-instruction
  exception if `vstart` is non-zero."* The same sentence appears verbatim for
  `vmsif.m` and `vmsof.m`. The rule is a property of the **whole instruction**,
  decided before the first element is looked at.
* **R2 — nothing happens.** An instruction that raises an illegal-instruction
  exception is not executed: no source element is read, no destination element
  is written, and the destination mask register is left as it was. It is **not**
  an element fault (`vstart` restart is for instructions that *can* be restarted;
  these three cannot).
* **R3 — `vstart == 0` is ordinary execution.** The rule is *only* about a
  non-zero `vstart`; at `vstart == 0` all three instructions execute and write
  their prefix mask exactly as `rvv.integer_mask_permute` already checks.

These three instructions are the only family in this unit with the rule. A
non-zero `vstart` for any other family stays a normal start index; the change
does not touch that path.

## The files, and what each change is

* **modified** `rtl/core/mosaic_vec_alu.sv` — additive:
  * a single `maskpfx_vstart_illegal` decision (`ef_family == F_MASKPFX &&
    cfg_vstart_i != 0`) that the element lane and the packet engine both read;
  * the element lane sets `sel_illegal` from it, so `e_illegal_o` reflects the
    rule at a boundary operand;
  * `S_SETUP` refuses the packet from it (`illegal_r <= 1'b1`, `state_q <=
    S_DONE`) **before** any per-element step, so no VRF transaction and no write
    can follow;
  * the module header's mutant list gains the three new controls.
  * Three `-D` controls mutate this one decision:
    `MOSAIC_VEC_ALU_MUTANT_MASKPFX_NO_TRAP`,
    `MOSAIC_VEC_ALU_MUTANT_MASKPFX_TRAP_VSTART0`,
    `MOSAIC_VEC_ALU_MUTANT_MASKPFX_WRONG_OP`.
* **modified** `sim/unit/tb_vec.cpp` — additive:
  * `ConfigureVecVstart(...)`, which captures a snapshot with an explicit
    `vstart` (write the CSR after the `vset` that would have cleared it, then
    capture), the state the rule is defined against;
  * `RunMaskPrefixVstartCase(...)` and its dispatch and PASS line.
* **new** `tools/run_vec_maskpfx_controls.py` — the three controls, each built
  from a deleted build directory with its `-D` on the recorded command line.

`tests/unit/registry.json` and `config/status/implementation_status.json` were
**not** edited. The case stays marked `"pending": true`; the integration lead
clears that marker when the package is recorded.

## What was reused, and what was added

* **Reused unchanged**: the I-052 configuration snapshot as the sole source of
  `vtype`/`vl`/`vstart` (the rule reads `cfg_vstart_i` and nothing re-derives it);
  the I-054 element lane and its `sel_illegal`/`exec_illegal_o` illegal path; the
  I-054 packet engine's `S_SETUP` → `S_DONE` refusal shape (the same shape the
  capability gate and the unsupported-width check already use); the case's host
  prefix model `ComputeExpected`/`OracleElem` that `rvv.integer_mask_permute`
  already uses.
* **Added**: one whole-instruction predicate, its use at the two points that can
  observe an instruction (the boundary element lane and the packet engine), the
  three negative controls, and the `vstart`-aware snapshot helper.

No new module, no new port, no interface, no second illegal path: the rule is
the same `illegal` channel the rest of the unit already reports on.

## The evidence

### The case

```
RESULT PASS rvv.mask_prefix_vstart checks=79 ops=3 vstart_cells=6 cycles=394
```

Coverage counts are themselves checks: `vstart_cells == 6` (3 operations ×
`vstart` ∈ {1, 3}) and 3 normal cells.

### The mutant table

Built by `python3 tools/run_vec_maskpfx_controls.py --profile p0`; the shipping
binary is built first from an empty directory and must pass. Each mutant is
built from a freshly deleted directory with its `-D` on the recorded command
line, its binary must differ from the shipping one, and it must exit 1 naming
the check it breaks.

| Mutant `-D` | sha256 | exit | first named failure |
| --- | --- | --- | --- |
| *(shipping)* | `45e3eb0ebdbf5660c69de553a2ca249a4d5a42c1d37f27ddc99d94dc0d57237e` | 0 | `RESULT PASS` |
| `MOSAIC_VEC_ALU_MUTANT_MASKPFX_NO_TRAP` | `6224442a66a7a48745aa9de422031a981f5665679132fdfa803e13ab580a79b2` | 1 | `CHECK FAILED: vmsbf vstart=1: a non-zero vstart did not raise an illegal instruction` |
| `MOSAIC_VEC_ALU_MUTANT_MASKPFX_TRAP_VSTART0` | `ba3b2f5bdb6f49aba841d7e4f0976174f3c2bf5fb145ac83af60cbaff0673185` | 1 | `CHECK FAILED: vmsbf vstart=0: the instruction was refused as illegal` |
| `MOSAIC_VEC_ALU_MUTANT_MASKPFX_WRONG_OP` | `c3af83ae4f9d0aeeff1fea2d4737fbaa34aa1d4250f22d919c457eb92aa39c89` | 1 | `CHECK FAILED: vmsof vstart=1: a non-zero vstart did not raise an illegal instruction` |

`NO_TRAP` is the card's "shut it down = discard it" failure in miniature: the
instruction would run from the middle instead of refusing. `TRAP_VSTART0` is a
rule that is too broad — a normal instruction wrongly refused. `WRONG_OP` is a
rule that covers two of the three, so a single unguarded instruction remains.

## Not covered

* **Mask-value semantics are the sibling case's, not this one's.**
  `rvv.integer_mask_permute` owns the value of the prefix mask; this case checks
  the `vstart` rule. The `vstart == 0` readback here uses the same host model
  (`ComputeExpected`/`OracleElem`), so it is a consistency check against that
  model, not an independent conformance check of the mask value.
  *Observation, deliberately not fixed here:* in `mosaic_vec_alu.sv` the
  element lane gives `vmsbf` (op 0) and `vmsif` (op 1) the **same** result
  expression (`~ef_pfx`), whereas the specification distinguishes them at the
  first set source element. Pin-pointing this is outside the `vstart` gap this
  package was scoped to close; it would change the value path and the I-054
  oracle together. It is flagged for a follow-up package rather than silently
  pinned by this one.
* **The core-level routing of the exception is not exercised here.** This case
  observes the ALU's `exec_illegal_o`, which is the illegal-instruction channel
  in this unit. How the integrated core turns that into a precise illegal
  instruction and what it does to `vstart`/retire is `vec.integrated` /
  `core.act_dut` territory.
* **`vstart` out of bounds** (`vstart >= VLMAX`) is not this rule. The
  specification *recommends* a trap there but does not require it, and the unit
  treats such a value as a normal start index for every other family.
* **A multi-lane / multi-hart scheduler** is not involved: the rule is decided
  whole-instruction in one unit, so there is no cross-lane or cross-hart
  renegotiation to test.
* **ACT4** (`core.act_dut`, p1, 127/127) is a scalar-suite run with no vector
  instructions, so this vector-only change cannot affect it; it is re-run in the
  lead's consolidated pass rather than duplicated here (building the whole core
  mid-flight would race the lanes editing `mosaic_core.sv`).
