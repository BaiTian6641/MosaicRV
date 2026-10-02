# I-052 — vsetvl/vsetvli/vsetivli and the vector CSRs

Case: `rvv.vset_boundaries` (top `mosaic_vec_tb`, driver `sim/unit/tb_vec.cpp`).
Verdict: **PASS** on p0 and p2, from a deleted build directory with the four
mutants below caught.

| item | value |
|---|---|
| `rtl/core/mosaic_vec_cfg.sv` (new) | `06027b4f450a97ba9c32c163bb530cd5e05e8dd763a77076d7676e048aaed8ae` |
| `rtl/core/mosaic_vec_desc.sv` (I-051, unchanged) | `b5a450c0f8e8d87738511488b9298e35522804f5c3ba8eff0d959e2d68c14ccc` |
| `sim/tb/mosaic_vec_tb.sv` | `a490670f162b0a989b2475dddb43eb1179eb233b57bf6e07dfc522e7ea98b337` |
| `sim/unit/tb_vec.cpp` | `050930aebd1e3977845165e05457d547582b37160fb5d452061d0fd8294a5b32` |
| `tools/run_vec_cfg_controls.py` (new) | `ee485f3d0635f7928a9bd865ffce8636db2b1806d5b9e571cb867a61453a1c81` |
| shipping binary sha256 (p0 == p2) | `4f24c98a8265d78a38c24f041d05f145f89229b9b21315a52bc8afa9bb60dbb4` |
| case result (p0, p2) | `RESULT PASS rvv.vset_boundaries checks=571 vtypes=64 avl_bands=36 cycles=276` |

`tests/unit/registry.json` and `config/status/implementation_status.json` were
not edited: the registry entry was registered before this package and its
`"pending": true` marker is the integration lead's to clear. The report is
additive; the existing `tools/run_vec_controls.py` (I-051) and its recorded hash
were left untouched, and I-052's controls live in a new tool.

## 1. The unit

`mosaic_vec_cfg` is the vector configuration unit and vector-CSR state: it
decodes and executes `vsetvli`/`vsetivli`/`vsetvl`, holds `vtype`, `vl`,
`vstart`, `vxrm`, `vxsat`, `vcsr` and `vlenb`, and hands the I-051 descriptor a
configuration snapshot. It is a module-level package like the descriptor and the
caches: it does not wire itself into `mosaic_core`, so it elaborates under every
profile (the shipped binary is byte-identical for p0 and p2). VLEN = 128,
ELEN = 64, `vlenb` = 16 are the frozen profile constants (as in I-051);
`vlenb` is a constant output and no configuration action can change it.

The vset interface takes the instruction's `rd`/`rs1` fields, `x[rs1]` (the AVL
source), `x[rs2]` (the `vsetvl` vtype source), the `vtypei` immediate, the
`vsetivli` `uimm[4:0]`, and `mstatus.VS == Off`. It returns the illegal-
instruction signal, the commit pulse, the `rd` write-enable and the `rd` value.

## 2. `vtype`, `vill` and the unsupported-vtype rule

A `vtype` argument is supported only if **all** its bits are legal: `vill` clear,
bits 62:8 zero, `vsew` ∈ {3,4,5,6} (SEW 8/16/32/64), `vlmul` ≠ `100`, and
SEW ≤ LMUL·ELEN. This is the V spec v1.0 rule "All bits of the vtype argument
must be considered" plus the field legality rules. The legal set is the same
**22** of 64 `vsew × vlmul` pairs I-051 enumerated.

* **Supported** — `vtype` takes the argument (bits 7:0; `vta`/`vma` preserved),
  `vill = 0`.
* **Unsupported** — `vill = 1`, `vtype[62:0] = 0` and `vl = 0` (V spec
  "Unsupported vtype Values"). This is a *normal* instruction: it commits, resets
  `vstart`, and writes `vl = 0` to `rd`.
* **`vill` blocks execution** — `exec_illegal_o` is asserted for a
  vtype-dependent instruction while `vill` is set ("If the vill bit is set, then
  any attempt to execute a vector instruction that depends upon vtype will raise
  an illegal-instruction exception"). `vset{i}vl{i}` and whole-register
  load/store/move do not depend on `vtype` and are passed (the `vset` path itself
  never raises illegal except on `VS == Off`). Reset leaves `vtype.vill = 1`
  (`vtype` = `0x8000_0000_0000_0000`), the spec's recommendation and the reset
  `config/csr/vector.json` declares.

## 3. AVL rules, the deterministic choice, and the rd/rs1 table

`VLMAX = VLEN · LMUL / SEW = 2^(log2(VLEN) + lmul_exp − vsew)`; for this profile
it is the same value the descriptor reports as `elem_count`.

The AVL source (V spec "AVL encoding", `vsetvli`/`vsetvl`):

| `rd` | `rs1` | AVL | effect on `vl` |
|---|---|---|---|
| any | `!x0` | `x[rs1]` | stripmining |
| `!x0` | `x0` | `~0` | `vl = VLMAX`, written to `rd` |
| `x0` | `x0` | current `vl` | keep `vl`, not written to a register |

`vsetivli` has no `x0` special case: its AVL is the zero-extended `uimm[4:0]`.

The spec allows a *family* of legal `vl` values; the unit's fixed, reproducible
choice is

$$\text{vl} = \min(\text{AVL},\ \text{VLMAX})$$

which satisfies all three bands (`vl = AVL` for `AVL ≤ VLMAX`; for
`VLMAX < AVL < 2·VLMAX` the spec permits `ceil(AVL/2) ≤ vl ≤ VLMAX`, and VLMAX
is in range; `vl = VLMAX` for `AVL ≥ 2·VLMAX`), and satisfies `vl = 0 ⇔ AVL = 0`
and `vl ≤ AVL`. The case asserts the *bands* and determinism, never one blessed
value (the package's declared fail mode is forcing every implementation onto the
reference's choice).

The `rd = x0, rs1 = x0` form is reserved when the new SEW/LMUL ratio changes
VLMAX. The spec says implementations *may* set `vill`; this unit does (and also
when the previous `vtype` was itself unsupported, so there is no valid old
VLMAX). Otherwise it keeps the current `vl`.

## 4. Vector CSR state and permissions

| CSR | addr | access | fields |
|---|---|---|---|
| `vstart` | 0x008 | read-write | `[6:0]` (largest index 127) |
| `vxsat` | 0x009 | read-write | `[0]` |
| `vxrm` | 0x00A | read-write | `[1:0]` |
| `vcsr` | 0x00F | read-write | `[2:1] = vxrm`, `[0] = vxsat` |
| `vl` | 0xC20 | **read-only** | software writes raise illegal |
| `vtype` | 0xC21 | **read-only** | only `vset{i}vl{i}` writes it |
| `vlenb` | 0xC22 | **read-only** const | 16 |

* All seven are unprivileged, so privilege never gates an access; an address
  that is none of the seven is illegal.
* Accessing **any** vector CSR while `mstatus.VS == Off` raises
  illegal-instruction (V spec: "Attempts to execute any vector instruction, or to
  access the vector CSRs, raise an illegal-instruction exception when
  mstatus.VS is set to Off").
* `vstart` is reset to zero by **every** committed vector instruction, including
  `vset{i}vl{i}`, and is **not** modified by the illegal-instruction path.
* `vcsr` is the same storage as `vxrm`/`vxsat`: writing either updates the
  other's view.

## 5. Configuration snapshot and replay

* On `snap_capture_i` (the descriptor-allocation handshake) the unit latches
  `{vtype, vl, vstart}` and the committed-configuration generation, and exposes
  them on `snap_*_o`. A capture in the same cycle as a committing `vset` captures
  the new configuration.
* `o_cfg_gen_o` counts committed `vset` instructions. A replay presents the
  generation it was issued under; `replay_ok_o` accepts it and the replay is
  served the **captured** `vtype`/`vl`/`vstart`, never the live (possibly newer)
  `vtype`. An unknown generation is refused.
* The descriptor is fed exactly this snapshot: the case allocates the I-051
  descriptor with the unit's `vtype`/`vl`/`vstart` and asserts the descriptor's
  stored snapshot matches them.

## 6. What the case covers

`sim/unit/tb_vec.cpp` dispatches on `--case`; `rvv.vset_boundaries` runs eight
phases (571 checks). The oracle for legality/AVL is written from the spec, not
read from the DUT.

| phase | what it enumerates |
|---|---|
| `avl-band` | 4 configurations × 9 AVL values = **36 bands**: 0, 1, VLMAX−1, VLMAX, VLMAX+1, 2·VLMAX−1, 2·VLMAX, 2·VLMAX+1, `~0`. Each checked for the band bound, determinism (same input twice) and idempotence (`vl` reused as AVL). |
| `vtype-support` | all **64** `vsew × vlmul` pairs against the spec oracle (22 legal / 42 illegal asserted), plus `vta`/`vma` preservation, reserved `vtypei` bits 10/9/8, and a reserved `vsetvl` bit. |
| `rd-rs1` | all four `rd`/`rs1` rows, the reserved `x0/x0` VLMAX-change case, `vsetivli` `uimm`, and `vsetvl` `rs2`. |
| `vstart` | reset by a committed vset, software write, upper-bit WARL, an illegal (`VS = Off`) instruction not modifying it, and an unsupported vtype still resetting it. |
| `csr` | read of each CSR, write to read-only rejected, WARL fields, `vcsr` mirroring, `VS = Off` on all seven, unknown address. |
| `snapshot` | capture, descriptor feed, replay after a config change, wrong-generation refusal. |
| `vill-blocks` | blocked/unblocked by dependency, and the reset `vill` state. |
| `vlen-invariance` | `vlenb` = 16 and VLMAX ≤ 128 across every supported configuration. |

## 7. Mutant table

`python3 tools/run_vec_cfg_controls.py --profile p2`. Each mutant is rebuilt from
a **deleted** directory with its `-D` on that build's own recorded command line
(`build/p2/vec_cfg_controls/<define>/build_command.txt`), must differ from the
shipping binary, must exit 1, and its first `CHECK FAILED` must name the check.

| mutant | defect | exit | sha256 | first failure |
|---|---|---|---|---|
| `MOSAIC_VEC_MUTANT_AVL_UNCLAMPED` | an AVL band returns the unclamped AVL | 1 | `748971ae8c4907c6be51d48ec835a0ff86ff5c48d3f3329fd50f6585a86a273b` | `avl-band vsew=3 vlmul=0 avl=0x0011 vlmax=16: vl 17 is not a legal choice` |
| `MOSAIC_VEC_MUTANT_VILL_NO_BLOCK` | `vill` does not block execution | 1 | `e4cc95a39665c28091fb356ec8fbfe77983eff9244614ec1009816d07a504979` | `vill-blocks: a vtype-dependent instruction is not blocked by vill` |
| `MOSAIC_VEC_MUTANT_REPLAY_NEW_VTYPE` | a replay is served the newer `vtype` | 1 | `a3539289679c3e0b4e3fd595ff1e27e1e2632ff3669cda749c61f0c0c94faf66` | `replay: a replay used the newer vtype 0x00000000000000a0, expected the captured 0x0000000000000060` |
| `MOSAIC_VEC_MUTANT_SILENT_M1` | an unsupported `vtype` is accepted as m1 | 1 | `21accc23477d82611f769cf8542cf532e627f00b7a27c3de7ed0205a32d9b4c7` | `vtype-support vsew=0 vlmul=0: an unsupported vtype did not set vill` |

Shipping baseline: `RESULT PASS rvv.vset_boundaries checks=571 vtypes=64
avl_bands=36 cycles=276`, binary sha256 `4f24c98a8265d78a38c24f041d05f145f89229b9b21315a52bc8afa9bb60dbb4`.
All four mutants mutate the binary, exit 1 and name the check they break.

## 8. Keep-passing evidence (re-run this wave)

| case | profile | result |
|---|---|---|
| `rvv.vset_boundaries` | p0, p2 | PASS `checks=571 vtypes=64 avl_bands=36 cycles=276` |
| `rvv.descriptor_legality` | p0, p2 | PASS (I-051 unchanged; the descriptor oracle still passes) |
| `core.act_dut` | p1 | **PASS `applicable=127 generated=127 run=127 passed=127 failed=0`**, `act_commit=96493a91448ca50780013fd892daec2c204487ba` |
| `core.corpus_sweep` | p0 | PASS |

The core, its testbench and the ACT4 harness were not edited, so ACT4 is
unchanged at 127/127.

## 9. Gates

* `tools/lint_rtl.py --profile p0`: **50 of 50 clean**.
* `tools/lint_rtl.py --profile p2`: `mosaic_vec_cfg.sv` clean; the profile also
  reports `mosaic_core.sv`, `mosaic_csr.sv`, `mosaic_interrupt.sv` and
  `mosaic_pmp.sv` failing on a missing `mosaic_csr_pkg`. The p2 generated
  `build/p2/rtl/mosaic_csr_pkg.svh` is **0 bytes** in this working tree
  (`build/p0/rtl/mosaic_csr_pkg.svh` is 37 kB) and `config/csr/mode_m.json`,
  `tools/gen_manifest.py` and `rtl/core/mosaic_interrupt.sv` are modified by a
  concurrent CSR/interrupt change. Not caused by this package — this package's
  files lint clean under both profiles.
* `slang-tidy --std 1800-2017 --single-unit` over all RTL: exit 0; the only line
  naming `mosaic_vec_cfg.sv` is the same unnamed-`always_comb` style warning the
  existing vector/cache modules carry.
* `tools/check_records.py`: **green** (`63 delivered, 71 registered, records
  agree`).
* `tools/check_exclusions.py` and `--negative`: **green** (`52 registered, 19
  open, 33 covered`; `13/13 illegal ledgers rejected`). The case has a recorded
  PASS, so it needs no ledger entry.

## 10. Not covered (honest list)

* **`vtype` bit positions diverge from the ratified manual.** The pinned v1.0
  `vtype-format.adoc` places `vsew` at bits 5:3 with `vma` at 7 and `vta` at 6.
  The delivered `mosaic_vec_desc` (I-051) decodes `vsew` at bits **7:5**, and
  `config/csr/vector.json` records the same ("vma = bit 3, vta = bit 4,
  vsew[2:0] = bits 7:5"). This unit follows the family's recorded layout so the
  snapshot it hands the descriptor is interpreted consistently; the AVL, `vill`
  and permission semantics are independent of the bit positions. **Finding:** if
  the family moves to the ratified positions, `mosaic_vec_desc.sv`,
  `tb_vec.cpp`'s `Vtypei`/oracle, `config/csr/vector.json` and
  `mosaic_vec_cfg.sv` must change together. No test here checks the positions
  against the ratified manual.
* **`vtype` software-write modelling.** `config/csr/vector.json` marks `vtype`
  `access: rwr` with writable fields 7:0/63 (the storage `vset` writes); this
  unit implements the spec's read-only rule for software CSR writes. A future
  core CSR integration must reconcile the two.
* **No core integration.** The unit is not wired into `mosaic_core`; the CSR
  port and the `vset` execute handshake are a module contract, not a decode path.
  `mstatus.VS` is an input, not state owned here.
* **`vstart` out of bounds.** `vstart` values above the current SEW's largest
  index are *reserved*; the spec recommends (does not require) trapping and this
  unit does not trap on them.
* **Fault-only-first `vl` update** (the other architectural writer of `vl`) is
  not modelled; it belongs to the vector memory package (I-056).
* **One snapshot register.** The baseline is one in-flight vector macro; the
  snapshot/replay path does not cover multiple outstanding macros (I-058).
* **No execution units.** This package is configuration only; the descriptor
  legality matrix (I-051) and the datapaths (I-053..I-059) are separate.
* **Reserved `vcsr` bits** 63:3 read zero; no test exercises non-zero writes to
  them beyond the field mask.
