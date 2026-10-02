# p1 hygiene — the two p1-only defects EX-034 found

Both defects were found by the lane that closed EX-034 (`results/reports/EX-034-ialign.md`,
section "Out-of-scope observations"). Both were pre-existing, p1-only and unrelated
to the IALIGN change: the p0 gate is what `make check` runs, so neither had been
hit. This pass fixes the lint defect, gives the generated CSR table a declared
*role* for every row, moves the "a table row no model can classify" failure out of
a unit case and into the configuration gate, and re-runs the gates and cases.

Nothing here is a suppression: no `lint_off` was added, no signal was marked used
with a dummy assignment, and no existing check was weakened or deleted. The
changes are additive apart from the deletion of the two dead signals themselves.

## 1. Defect 1 — `store_q0`/`store_q1` in `rtl/core/mosaic_pmp.sv`

### 1.1 Symptom (before)

`python3 tools/lint_rtl.py --profile p1` (p0 lint was already 45/45 clean):

```
FAIL rtl/core/mosaic_pmp.sv: lint failed
%Warning-UNUSEDSIGNAL: rtl/core/mosaic_pmp.sv:418:19: Bits of signal are not used: 'store_q0'[1:0]
%Warning-UNUSEDSIGNAL: rtl/core/mosaic_pmp.sv:418:29: Bits of signal are not used: 'store_q1'[1:0]
%Error: Exiting due to 2 warning(s)
lint: 2 of 45 source file(s) failed
```

### 1.2 Investigation — which of the two explanations applies

The two candidate readings the card offered were "they should be driving
something" or "they are dead weight from the D5 fix". Read against the module and
its consumer, it is unambiguously the second:

* **The interface has no sink for the missing fields.** The store-commit port is
  `sc_req_addr{0,1}_i`, `sc_req_bytes{0,1}_i`, `sc_req_priv{0,1}_i` in and
  `sc_allow{0,1}_o` out, and nothing else (`mosaic_pmp.sv`, "store-commit query
  (D5)"). A store-lane answer carries `allow`, `matched` and `locked`; only
  `allow` has an output. The core's only use is the deny decision
  (`mosaic_core.sv`: `store0_pmp_deny_c = … && !pmp_store_allow0_c` and the lane-1
  counterpart), which is `!allow` alone — `matched` and `locked` cannot change
  it, because `allow` already folds in the "no entry matched and the mode is
  S/U" rule and the locked/M-mode rule.
* **The dead bits are an artefact of the return type, not of the port.** The D5
  fix asked the store-commit question through the same `pmp_query_t` record as
  the data and fetch ports (so the search runs once per call), and bound the
  record to a named signal per lane. The record's `matched`/`locked` fields are
  therefore *computed* and *stored* for the store lanes and never read. That is
  exactly what `UNUSEDSIGNAL` reported: bits `[1:0]` = `matched`,`locked` (the
  struct packs `allow`/`matched`/`locked`).

So: **dead weight introduced by the D5 fix** (the shared return shape carried two
fields the store-commit port has no output for), not signals that should have been
driving something. Deleting the computation is the fix; adding outputs for them
would have created an interface with no consumer, and a lint suppression would have
converted a real finding into a permanent blind spot.

### 1.3 Change

`rtl/core/mosaic_pmp.sv`, in the engine's `always_comb`:

* deleted `pmp_query_t store_q0, store_q1;`
* the two store lanes now take `.allow` directly from the query instead of binding
  the whole record:

```systemverilog
        sc_allow0_o = pmp_query(sc_req_addr0_i, sc_req_bytes0_i, 1'b0, 1'b1, 1'b0,
                                sc_req_priv0_i).allow;
        sc_allow1_o = pmp_query(sc_req_addr1_i, sc_req_bytes1_i, 1'b0, 1'b1, 1'b0,
                                sc_req_priv1_i).allow;
```

* a `-D` switch `MOSAIC_PMP_MUTANT_STORE_COMMIT_UNGATED` was added in its place
  (see the control, §1.5). The comment above the lanes now states why only
  `allow` is taken.

The search still runs exactly once per lane per cycle, so nothing about the query's
cost or its "lowest-numbered matching entry" behaviour changed.

### 1.4 Before / after observation

| `python3 tools/lint_rtl.py --profile …` | before | after |
|---|---|---|
| p1 | `FAIL rtl/core/mosaic_pmp.sv` — 2 `UNUSEDSIGNAL`, "2 of 45 source file(s) failed" | `lint: 45 source file(s) clean` (exit 0) |
| p0 | `lint: 45 source file(s) clean` | `lint: 45 source file(s) clean` (exit 0) |

The deletion is itself guarded by the gate: re-introducing the two dead signals
(anywhere, under any name) trips `UNUSEDSIGNAL` again. Verified explicitly by
temporarily restoring the pre-fix shape (`pmp_query_t store_q0, store_q1;` bound
to the two lanes) and running the p1 lint: it exited 1 with
`Bits of signal are not used: 'store_q0'[1:0]` / `'store_q1'[1:0]`. The shape was
then removed again; the shipping tree has neither declaration.

### 1.5 Control — `MOSAIC_PMP_MUTANT_STORE_COMMIT_UNGATED`

The required control for a deleted signal is "a mutant that reintroduces the
behaviour they were supposed to drive". What those lanes exist to drive is the
store-commit permission answer; the mutant discards that answer at the PMP
boundary (both lanes pinned to "allowed"), which is the state the pre-fix shape
left behind and is defect D5 reproduced in the unit that had the unused fields.

Built and run by `python3 tools/run_priv_controls.py --profile p1`
(CASE=`privilege.permission_matrix`; every control from a deleted build directory
with its `-D` on the Verilator command line and echoed to the C++ half):

| control | `-D` | sha256 (12) | exit | first failure |
|---|---|---|---|---|
| shipping | — | `7e7d3e17b57c` | 0 | — |
| **STORE_COMMIT_UNGATED** | `MOSAIC_PMP_MUTANT_STORE_COMMIT_UNGATED` | **`8cac663b4f3a`** | **1** | **`s-store-deny-w (S/store/napot): mcause is the ISA's: mcause=0x…9 expected 0x…7`** |

The mutant's binary hash differs from the shipping one, it exits 1, and it names
`s-store-deny-w` first. `privilege.permission_matrix` is the case whose shipping
run passes at p1 (`checks=6411 comparisons=57 cycles=22954 retires=9427 traps=53`),
so the control is evidence rather than an echo of a failing baseline. The full run
reported `0 control(s) did not behave as the case requires` (the pre-existing
LOCK_IGNORED / OVERLAP_INVERTED / M_MODE_ENFORCED / STORE_DENY_NOT_TAKEN /
FAULT_WRITES / MEPC_IALIGN32 controls still exit 1 with their named first
failures).

## 2. Defect 2 — `sstatus` had no declared role

### 2.1 Symptom (before)

`RESULT FAIL csr.precise_trap_mret contract violated: geometry: generated table row
sstatus has no known role` under `--profile p1`. `tools/gen_manifest.py --profile
p1` itself already exited 0: the generator emitted a 50-row `mosaic_csr_table.h`
whose `sstatus` row carried a name, address, reset and mask but no role, and the
case's `TableIndex` — which required every generated row to map onto one of the 21
machine-mode roles the model implements — refused to classify it.

### 2.2 Investigation

The card attributed the role table to the generator; in fact the classification
lived *only* in the case (`sim/unit/tb_csr.cpp`, `kRoleName`) and the generated
table had no role column at all. Read against the configuration, the defect is a
missing **declaration**: a CSR's *role* — what the register is — was never stated
anywhere a tool could check, so p1's privilege work could add supervisor registers
(`config/csr/mode_su.json`, and `mode_pmp.json`) to a table that the unit model had
no way to classify, and the only place that noticed was a unit case. The fix had to
(a) make the role a declared property of the configuration, (b) generate it into
the table, and (c) make a CSR-without-a-role a **configuration-gate** failure, so a
register added later cannot reach a case unclassified. That is the durable fix the
card asks for.

A role is *not* the register's name: two registers share one role when the same
machinery reads and writes them across privilege levels — `mstatus` and `sstatus`
are both `status`, `mepc` and `sepc` are both `trap-pc` — and `pmpaddr0..15` are
all `pmp-address` while being 16 distinct registers.

### 2.3 Change

* **`config/csr/mode_m.json`, `mode_su.json`, `mode_pmp.json`, `vector.json`:**
  every CSR entry (21 + 14 + 18 + 7 = 60) now declares `"role": "<class>"`.
  `sstatus` declares `"status"`.
* **`config/schema/csr.schema.json`:** `role` is a required string property with a
  description naming the closed set; a table entry without it fails schema
  validation inside `check_profile.py`.
* **`tools/mosaic/config_check.py`:** new closed set `CSR_ROLES` (20 classes:
  `status`, `isa`, `delegation`, `intr-enable`, `intr-pending`, `trap-vector`,
  `counter-enable`, `scratch`, `trap-pc`, `trap-cause`, `trap-value`, `machine-id`,
  `cycle-counter`, `instret-counter`, `counter-shadow`, `address-translation`,
  `env-config`, `pmp-config`, `pmp-address`, `vector-state`). `_check_csr` rejects a
  CSR whose entry has no role, and one whose role is outside the set — so "a CSR
  nobody can reason about" is a configuration error, not a decode error later.
* **`tools/gen_manifest.py`:** `mosaic_csr_desc_t` gained `const char *role;` and
  every generated row copies the declared role through, so the table *declares* it.
* **`sim/unit/tb_csr.cpp`:** the model now consumes the generated role. Each row
  must declare a role (a generator sanity check), and each register the model
  drives must declare the class the model expects (`kRoleClass`, compared against
  the table) — so a mis-classified table fails in the model, while a register the
  model does not implement no longer fails *there*. The runtime check "every row
  maps to a modelled role" is replaced by the gate's "every CSR declares a role
  from the closed set". The header names the profile scope.
* **`tools/check_profile.py`:** two new negative controls, additive; no existing
  control touched.

### 2.4 Before / after observation

| | generated `sstatus` row | `csr.precise_trap_mret` under p1 |
|---|---|---|
| **before** | `{ "sstatus", 0x100, … }` — no role | `FAIL … generated table row sstatus has no known role` |
| **after** | `{ "sstatus", "status", 0x100, … }` | `FAIL … table: the generated table has 50 rows, the implementation table declares 21` |

The generated table now classifies every row, and the p1 run no longer fails on the
defect this pass is about. What remained was the second half of the finding, and it
is a scope error rather than a hygiene defect: **the case is machine-mode-only by
construction** (a hand-derived 21-row oracle, standing invariants `mstatus.MPP == 3`
and `mepc[1:0] == 0` for IALIGN=32, and an M-only shadow), while p1's CSR file owns
S/U privilege (`rtl/core/mosaic_csr.sv`'s `priv_q`, writable `mstatus.MPP`, CSR
privilege checks, MRET setting MPP to the profile's least-privileged mode, `satp`,
delegation). Observed at p1 before the registry change: the table phase reports
**50 generated rows against the case's 21**. Making it pass at p1 would mean
modelling supervisor-mode CSR state — a new verification package, not hygiene — so
this lane deliberately did **not** write one.

The integration lead took the scope finding and added a **per-case profile set** to
`tests/unit/registry.json` (the right place for it): `csr.precise_trap_mret` is now
`"profiles": ["p0"]` (`core.trap_csr_program` too, for the same reason — it
hardcodes the p0 `mstatus` read-back). The runner honours it: after that change
`python3 tools/run_unit.py --profile p1 --case csr.precise_trap_mret` prints
`FAIL csr.precise_trap_mret is registered for p0 only, not p1` and exits 1 — an
explicit refusal rather than a run against a model that does not exist — while
`--profile p0` passes. `check_records.py` validates the field.

The p0 run and its recorded PASS (which EX-009 and EX-025 cite) are unaffected; the
one p1 run performed before the registry change was sent to a scratch `--out`
directory so it could not overwrite that record (see §5.2).

### 2.5 Control — the configuration gate rejects a CSR with no role

`python3 tools/check_profile.py` (the gate inside `make check`):

| control | mutation | required outcome |
|---|---|---|
| CSR without a role | pop `role` from the first CSR entry | rejected |
| CSR with an unknown role | set `role` to `not-a-declared-role` | rejected |

Observed:

```
rejected: CSR without a role        csr table csr/mode_m.json$.modes[0].csrs[0]:
                                    missing required property 'role'
rejected: CSR with an unknown role  csr M.mstatus: unknown role 'not-a-declared-role';
                                    declared roles are address-translation, … vector-state
negative controls: 50/50 illegal configurations rejected   (p0)
negative controls: 37/37 illegal configurations rejected   (p1)
```

The "no role" control is the durable fix for this class of defect: the pre-fix
configuration — a CSR that declares no role — is now rejected by the gate, not
discovered later inside a unit case. The generator also carries the declaration
into `mosaic_csr_desc_t`, so the role is not dead data: `tb_csr.cpp` checks it.

Both controls were added to `tools/check_profile.py` and **each rejection was
observed**, for p0 and for p1 (`--all --negative` exercises p0; `--profile p1
--negative` exercises p1), in the run quoted above. No existing control was
removed or weakened; the totals moved 48→50 (p0) and 35→37 (p1).

## 3. Gates re-run

| gate | result |
|---|---|
| `python3 tools/lint_rtl.py --profile p0` | exit 0 — `lint: 45 source file(s) clean` |
| `python3 tools/lint_rtl.py --profile p1` | exit 0 — `lint: 45 source file(s) clean` |
| `python3 tools/lint_rtl.py --profile p1 --self-test` | exit 0 — `a latching module is rejected (exit 1, LATCH reported)` |
| `slang-tidy` (`make lint-slang PROFILE=p0` / `p1`) | exit 0 both; only the pre-existing STYLE-7/STYLE-16 advisory warnings |
| `python3 tools/check_profile.py --all` | exit 0 — p0/p1/p2/p3 configuration OK |
| `python3 tools/check_profile.py --all --negative` | exit 0 — 50/50 rejected |
| `python3 tools/check_profile.py --profile p1 --negative` | exit 0 — 37/37 rejected |
| `python3 tools/check_exclusions.py` | exit 0 — 52 registered, 19 open, 33 covered |
| `python3 tools/check_exclusions.py --negative` | exit 0 — 13/13 rejected |
| `make check` | exit 0 |
| `python3 tools/check_records.py` | exit 0 — 59 delivered packages, 65 registered cases, records agree |
| `python3 tools/gen_manifest.py --profile p1` | exit 0 (50-row table, every row with a role) |

All of the above was re-run **after the last edit**, on `HEAD = 5baa7cf` (the
per-case-profile-set commit) with the ACT4 lane active in the same core files:
lint p0 and p1 both report 45/45 clean, `make check` exits 0 and `check_records`
exits 0. My `rtl/core/mosaic_pmp.sv` edit and the ACT4 lane's edits do not overlap
(that lane is in `mosaic_core.sv`/decode side), so there was nothing to reconcile.

## 4. Cases re-run

| case | profile | verdict |
|---|---|---|
| `csr.precise_trap_mret` | p0 | **PASS** (`CSR contract holds: 6571 shadow comparisons over 6881 cycles, 21 CSRs`) |
| `csr.precise_trap_mret` | p1 | **REFUSED** — `is registered for p0 only, not p1` (registry now scopes it to p0; before that change it ran and failed at the table phase, 50 rows vs 21 — §2.4) |
| `privilege.permission_matrix` | p0 | **PASS** |
| `privilege.permission_matrix` | p1 | **PASS** (`checks=6411 comparisons=57 cycles=22954 retires=9427 traps=53`) |
| `core.trap_csr_program` | p0 | **PASS** |
| `sv39.walk_and_faults` | p1 | **PASS** |
| `tlb.sfence_vma` | p1 | **PASS** |

## 5. Found along the way, not fixed

1. **`csr.precise_trap_mret` is a p0 case with no profile scope in the registry.**
   It cannot pass at p1 without a supervisor-mode CSR model (§2.4).
   `tests/unit/registry.json` had no per-case profile field, so nothing stopped a p1
   run. I did not edit the registry; the integration lead added the field and set
   this case (and `core.trap_csr_program`) to `["p0"]`, which is the fix. No S-mode
   CSR model was written by this lane — that is a verification work package.
2. **The unit-result record is profile-agnostic.** `results/unit/<case>/result.json`
   is overwritten by *any* profile's run. My first p1 run of
   `csr.precise_trap_mret` clobbered its p0 PASS and made `check_exclusions`
   report `alternative verification names case csr.precise_trap_mret, whose
   recorded result is FAIL` for EX-009 and EX-025. I restored the p0 record and
   sent the p1 run to a scratch `--out`; the underlying one-record-per-case layout
   is untouched (fixing it means recording per profile in `tools/run_unit.py`).
3. **`mosaic_csr_desc_t.min_priv_r`/`min_priv_w` are generated and read by no
   consumer** — not the CSR model, not any other case (verified by grep). They are
   the same family of finding as defect 1's dead fields (a generated field nothing
   reads), but widening this pass to them is out of scope; noted so they are not
   mistaken for used data.
4. **`slang-tidy` reports pre-existing STYLE-7 (instance prefix) and STYLE-16
   (generate block) advisories** across the RTL, including files I did not touch.
   It exits 0, and they predate this pass; left alone.

## 6. Revision left

Working tree at **`5baa7cf`** ("Per-case profile sets: the registry can say a case
is p0-only"), clean apart from this report (untracked) and a re-stamped
`timestamp` in `results/unit/csr.precise_trap_mret/result.json` from the p0 re-run
(verdict and detail unchanged: PASS). A sibling lane's commit `2636051` swept this
pass's edits into the tree along with its own; the files changed here are:

* `rtl/core/mosaic_pmp.sv`
* `tools/mosaic/config_check.py`
* `tools/gen_manifest.py`
* `tools/check_profile.py`
* `tools/run_priv_controls.py` (new `STORE_COMMIT_UNGATED` control)
* `sim/unit/tb_csr.cpp` (the driver of `csr.precise_trap_mret`, the case under
  repair — not another case's driver)
* `config/csr/mode_m.json`, `mode_su.json`, `mode_pmp.json`, `vector.json`
* `config/schema/csr.schema.json`
* this report

`tests/unit/registry.json`, `config/status/implementation_status.json`,
`results/PROGRESS.md`, `tests/programs/**` and `config/validation/exclusion_ledger.json`
were not touched.
