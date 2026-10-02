# EX-034 — the p1 IALIGN fix: mepc/sepc alignment is derived from the profile

Work packages I-044 (profile configuration) and I-041 (the C extension's
contract). Closes exclusion **EX-034**:

> `mepc[1:0] read-only zero (IALIGN=32 mask) while p1 claims the C extension`

The C extension implies IALIGN=16, so a trap taken on a compressed instruction at
a 2-mod-4 PC must record that exact PC in `mepc` (only bit 0 is read-only zero).
The p1 build did not: it generated the IALIGN=32 mask, so `mepc` lost bit 1.

## Where the alignment assumption lived

IALIGN is a property of the **profile**, but the decision had been written into
two places that could disagree:

1. **`config/csr/mode_m.json` (the CSR table).** `mepc` declared
   `unmodifiable_bits: ["1:0"]`, `writable_fields: ["63:2"]`, and its `spec_clause`
   said "p0 has fixed IALIGN=32, so mepc[1:0] are read-only zero". The same table
   is loaded by p0 *and* p1, so the p1 mask inherited the p0 rule. The same
   hard-coding was in `config/csr/mode_su.json` for `sepc`
   (`unmodifiable_bits: ["0"]`, `writable_fields: ["63:1"]`, and a `spec_clause`
   that asserted "IALIGN is never 32").
2. **`tools/gen_manifest.py`** projected `writable_fields` straight into
   `MOSAIC_CSR_WMASK_MEPC`/`MOSAIC_CSR_WMASK_SEPC` in
   `build/<profile>/rtl/mosaic_csr_pkg.svh`, with no notion of IALIGN. The
   profile's claimed extensions (`config/profiles/p1.json` → `C`) were never
   consulted.
3. **`rtl/core/mosaic_csr.sv`** used that generated mask in both directions —
   trap entry (`mepc_d = trap_epc_i & MOSAIC_CSR_WMASK_MEPC`) and the software
   write path (`mepc_d = csr_op_result & MOSAIC_CSR_WMASK_MEPC`) — and
   `mret_target_o = mepc_q`. The RTL was already correct given a correct mask; the
   defect was entirely the mask.
4. `rtl/core/mosaic_bringup_core.sv` is a separate, p0-only bring-up core with a
   hard-coded IALIGN=32 `mepc`; p0 does not claim C, so it is left as is.

No PC masking exists elsewhere in the trap path: `trap_epc_sync = rob_head_pc`
(the faulting PC, exact) and the handler/MRET use `mepc_q` directly.

## What changed: the choice is derived, in one place

* **`tools/mosaic/config_check.py`** gained `ialign(bundle)`: `16` when the
  profile's claimed extension list contains `C`, else `32`. This is the single
  place the choice is made; it reads the profile's own capability list, so it
  cannot drift from the capability ladder the way a second flag would.
* The CSR table now **declares which bits are IALIGN-derived** instead of
  deciding them. `mepc` and `sepc` carry `ialign_bits: ["1:0"]`; their
  `unmodifiable_bits` no longer contain the alignment bits. `config_check`
  requires this for every PC-valued register (`PC_VALUED_CSRS`), so a table can no
  longer freeze the decision. `config/schema/csr.schema.json` documents the field.
* **`tools/gen_manifest.py`** computes the generated write mask from the profile:
  for an IALIGN-governed register it adds bit 1 exactly when `ialign == 16`, and
  never bit 0. `MOSAIC_IALIGN` is emitted into `mosaic_cfg_pkg.svh`,
  `build/<profile>/sim/mosaic_platform.h` and `manifest.json`, next to `misa`, so
  the value the masks are built from is visible with the capability that decides
  it. The C++ CSR table (`mosaic_csr_table.h`) is generated from the same call.
* **`tools/check_profile.py`** rejects (in its normal `--profile` run, i.e. inside
  `make check`) any profile whose PC-valued register is missing `ialign_bits` or
  freezes bit 1 as writable/unmodifiable — so "claims C while the table says
  IALIGN=32" is a **config-gate** failure, not merely a test failure.

### Generated masks, before → after

| build | `MOSAIC_CSR_WMASK_MEPC` | `MOSAIC_CSR_WMASK_SEPC` |
|---|---|---|
| p0 (does not claim C) | `0xfffffffffffffffc` → `0xfffffffffffffffc` (unchanged) | n/a |
| p1 (claims C) | `0xfffffffffffffffc` → `0xfffffffffffffffe` | `0xfffffffffffffffe` (unchanged for p1) |

p1's `sepc` happened to be p1-correct already because only C-claiming profiles
load `mode_su.json`; the derivation makes that a property of the profile rather
than a coincidence. p0 is byte-for-byte unchanged, so `core.event_payload`
(p0, which pins the IALIGN=32 rule) and the p0 `csr.rule_ledger` are untouched.

## The proof: the p1 IALIGN probe

Extended case: **`privilege.permission_matrix`** (I-044's case; driver
`sim/unit/tb_core_priv.cpp`). A new phase runs only when `MOSAIC_IALIGN == 16`
(a profile that claims C):

* the program jumps to the 2-mod-4 PC `0x80019102`, where the image holds the
  compressed instruction **`c.ebreak`**;
* the breakpoint trap (cause 3, tval 0) is handled in M-mode; the handler saves
  `mepc` in its frame **before** touching anything;
* the frame's `mepc` must equal `0x80019102` **exactly** (bit 1 preserved);
* the frame's resume value is a second 2-mod-4 address, `0x80019112`; the handler
  writes it into `mepc` and `mret`s, and the compressed `c.nop` there falls
  through to the probe's exit stub, which records the marker `0x600d` and writes
  the pass code to `tohost`. If `mret` (or the `mepc` write mask) dropped bit 1
  the run would land two bytes earlier on an `ecall` and the frame would carry
  cause 11 and a different address, so the marker and the frame both fail.

New named checks: `EX-034: mepc names the 2-mod-4 trap PC exactly when the profile
claims C (IALIGN=16)`, `EX-034: the trap on the compressed instruction is a
breakpoint`, `EX-034: mret returned to the 2-mod-4 address mepc named`, and
`EX-034: the IALIGN probe ran for a C-claiming profile`. A profile without C
skips the row (a 2-mod-4 trap PC is rounded there and a 2-mod-4 MRET target is
not representable), so p0 is unaffected.

### Before / after observation (live)

| | `mepc` after the trap on the 2-mod-4 PC |
|---|---|
| **Before** (the pre-fix IALIGN=32 mask; reproduced by the `MEPC_IALIGN32` control) | `0x0000000080019100` — the word-rounded address |
| **After** (shipping p1 build) | `0x0000000080019102` — the exact address, bit 1 preserved |

## Controls

`tools/run_priv_controls.py --profile p1` (every control rebuilt from a deleted
build directory with its `-D` on the Verilator command line):

| control | `-D` | sha256 (12) | exit | first failure |
|---|---|---|---|---|
| shipping | — | `7e7d3e17b57c` | 0 | — |
| LOCK_IGNORED | `MOSAIC_PMP_MUTANT_LOCK_IGNORED` | `1562186fca67` | 1 | `lock-cfg-write-ignored … pmpcfg2=0 expected 0x91909090` |
| OVERLAP_INVERTED | `MOSAIC_PMP_MUTANT_OVERLAP_INVERTED` | `93b299621b54` | 1 | `s-overlap-lower-allows … mcause=5 expected 9` |
| M_MODE_ENFORCED | `MOSAIC_PMP_MUTANT_M_MODE_ENFORCED` | `91ec356b7ee1` | 1 | `max-cycles exhausted` |
| STORE_DENY_NOT_TAKEN | `MOSAIC_PMP_MUTANT_STORE_DENY_NOT_TAKEN` | `67f727d36270` | 1 | `s-store-deny-w … mcause=9 expected 7` |
| FAULT_WRITES | `MOSAIC_PRIV_MUTANT_FAULT_WRITES` | `2d98f89f7fb2` | 1 | `s-load-deny-r … the destination register is untouched` |
| **MEPC_IALIGN32** | **`MOSAIC_CSR_MUTANT_MEPC_IALIGN32`** | **`18884fc15302`** | **1** | **`ialign-odd-pc-mepc (M/fetch/no-entry): mepc names the faulting instruction: mepc=0x80019100 expected 0x80019102`** |

The `MEPC_IALIGN32` control re-injects EX-034 itself in `rtl/core/mosaic_csr.sv`
(the trap epc masked with the IALIGN=32 rule: `… & MOSAIC_CSR_WMASK_MEPC & ~64'h2`).
Its binary hash differs from the shipping one and it exits 1 with the rounded
address named. `0 control(s) did not behave as the case requires`.

## Configuration gate

`python3 tools/check_profile.py --all --negative` → `negative controls: 48/48
illegal configurations rejected` (the `--negative` pass covers p0; `--profile p1
--negative` is 35/35), including the new control **"mepc alignment bits frozen in
the table (EX-034)"** for both profiles:

```
rejected: mepc alignment bits frozen in the table (EX-034)  csr M.mepc: a
program-counter register must declare ialign_bits ["1:0"] so its low bits follow
the profile's IALIGN instead of being frozen in the table
```

That control is exactly the pre-fix configuration (drop `ialign_bits`, set
`unmodifiable_bits: ["1:0"]`), and the normal `check_profile --all` inside
`make check` rejects it — the gate, not a test.

## Exclusion ledger: EX-034

`config/validation/exclusion_ledger.json` — EX-034 moved from `open` to
**`covered`**:

* `alternative_verification.cases`: `privilege.permission_matrix`
* `alternative_verification.checks`: `tools/run_priv_controls.py`
* `end_condition`: **Met** — the mask is derived from the profile's claimed
  extensions, the p1 probe checks the exact `mepc` and the MRET return, the
  `MEPC_IALIGN32` control fails, and the config gate rejects a frozen table.

`tools/check_exclusions.py` (and `--negative`, 13/13) is green; closure counts
moved from 20 open / 32 covered to **19 open / 33 covered**.

## Case re-runs (from clean builds)

| case | profile | verdict |
|---|---|---|
| `privilege.permission_matrix` | p0 | PASS |
| `privilege.permission_matrix` | p1 | PASS (`checks=6411 comparisons=57 cycles=22954 retires=9427 traps=53`; probe observed `ialign-odd-pc-mepc M/fetch/no-entry trap=1 cause=3 from=M`) |
| `compressed.cross_boundary` | p0 | PASS |
| `compressed.cross_boundary` | p1 | PASS |
| `trap.precise_state` | p0 | PASS |
| `trap.precise_state` | p1 | PASS |
| `core.corpus_sweep` | p0 | PASS |
| `sv39.walk_and_faults` | p1 | PASS |
| `tlb.sfence_vma` | p1 | PASS |
| `csr.precise_trap_mret` | p0 | PASS |

### Out-of-scope observations (not caused by this change)

Two p1-only checks fail on inputs this change does not touch; both are outside
the EX-034 gates (`lint_rtl` is run with `--profile p0`, and `csr.precise_trap_mret`
is registered/run at p0):

* `python3 tools/lint_rtl.py --profile p1` reports `rtl/core/mosaic_pmp.sv`
  `store_q0[1:0]`/`store_q1[1:0]` unused (a `-Wall` warning is an error). The
  pmp source is unmodified by EX-034; p0 lint is clean (45/45).
* `csr.precise_trap_mret` at p1 fails with `generated table row sstatus has no
  known role` — the case's role table has no `sstatus` row. The failing input is
  the generated table's `sstatus` entry, unchanged by this change (only the
  `mepc`/`sepc` masks moved); the case passes at p0.


## Gates

* `make check` (PROFILE=p0) — exit 0 (incl. `check_profile --all`,
  `check_records`, `check_exclusions` + `--negative`).
* `python3 tools/lint_rtl.py --profile p0` — 45 sources clean; `--self-test`
  rejects the latching fixture.
* `slang-tidy` (`make lint-slang PROFILE=p0`) — exit 0.
* `python3 tools/check_records.py` — 58 delivered packages, 64 registered cases,
  records agree.
* `python3 tools/check_exclusions.py` + `--negative` — green.

## Revision left

Working tree based on `b0c6ea3fa475c7fa763dd641a11ac36cb2c3d428` (uncommitted;
the ACT4-runner lane's `sim/unit/tb_core_act.cpp` and `tests/act4/` are present
and untouched by this work). Files changed by EX-034:
`config/csr/mode_m.json`, `config/csr/mode_su.json`,
`config/schema/csr.schema.json`, `config/validation/exclusion_ledger.json`,
`tools/mosaic/config_check.py`, `tools/gen_manifest.py`,
`tools/check_profile.py`, `tools/run_priv_controls.py`,
`rtl/core/mosaic_csr.sv`, `sim/unit/tb_core_priv.cpp`.
