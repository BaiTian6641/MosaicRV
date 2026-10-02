# V-043 follow-up: the five ACT4 divergences in advertised extensions

Lane: `Act4Fixes`. Status: **not complete.** Two of the three fixes are verified
(125 of 127 ELFs pass on the current tree, up from 122); the Zihpm pair is
diagnosed and half-fixed; the mutants are written but not yet built or run, and
the twelve re-run cases and the gates have not been run. Everything below is
marked with what was actually observed and what was not.

Reproduction method: the shipping `core.act_dut` binary run per ELF on the
generator's *signature-mode* image (`<name>.sig.elf`, which stores rather than
compares) with `MOSAIC_ACT_SIG_DUMP=1`, and the dumped signature diffed against
Sail's `.sig`/`.results`. This is the same oracle the V-043 card used; the
per-ELF driver path is `--image <elf>`.

## 1. `Zca/Zca-c.lui-00` -- c.lui's immediate, 12 bits too high

**Before.** `Zca-c.lui-00`: first divergent word is index 1, expected
`0x0000000000005000` (`c.lui x1, 5`), DUT produced `0x0000000005000000`.
Reproduced alone:
`MOSAIC_ACT_SIG_DUMP=1 build/p1/unit/core.act_dut/case --case core.act_dut
--image .../Zca/Zca-c.lui-00.sig.elf` -> `0x0000000005000000` where the Sail
`.results` has `0x0000000000005000`. (The V-043 report says "16 bits too high":
the measured shift is 12. `0x5000 << 12 == 0x5000000`.)

**Diagnosis.** `rtl/core/mosaic_decoder.sv`, CI-format arm of `c_expand`:

    imm20 = {{2{c[12]}}, c[12], c[6:2], 12'b0};   // then rv_u(imm20, rd, LUI)

`lui rd, imm20` places `imm20` at rd[31:12], and C.LUI's 6-bit `nzimm` is
defined at rd[17:12]. So `imm20` must be the 6-bit field `{c[12], c[6:2]}`
sign-extended into `imm20[19:0]`. The shipped form first placed the field at
`imm20[16:12]` and then padded the low 12 bits with zeros, shifting the field
twelve bits high in `imm20`, hence twelve bits high in `rd`.

**Fix (rule).** Build the base-ISA immediate from the field the ISA defines, in
the field's own position: `imm20 = {{14{c[12]}}, c[12], c[6:2]}`. This is the
same lane that already carried one CI-format immediate defect (`c.addi` reading
the destination register as the immediate, caught by
`compressed.cross_boundary`); the arm is now stated in terms of `nzimm[5:0]`.

**After.** `Zca-c.lui-00` passes; the dumped signature matches Sail's from index
1 to the end of the test's signature (the only remaining differences are the
trap-signature records of item 2 below, which are a different defect).

## 2. `Zalrsc/Zalrsc-sc.w-00`, `Zalrsc-sc.d-00` -- the reservation set is 64 bytes, the platform declares 8

**Before.** First divergent word index 490 (both widths):

    [490] ref=0x0000000000000001 dut=0x0000000000000000
    [491] ref=0xdead0002fffdbeef dut=0xdead0002efb20555

`sc.w`/`sc.d` write 0 to `rd` on success and 1 on failure, so **the DUT's SC
succeeded where the reference expects it to fail** -- the opposite direction from
the V-043 report's table ("expected 1 (SC succeeds), got 0") and from this lane's
brief. The self-check image says the same thing independently, from the macro
that reports the DUT's own register: `RVCP: Bad Value: 0x0 / Expected Value:
0x1` (`failing_value` is the DUT's `rd`, `expected_value` is the preloaded Sail
word). 23 words differ: fourteen from the seven `address_difference_8..56` bins
(SC status + stored value), nine from the trap-signature region of item 3's root
cause below.

**Diagnosis.** The test is `lr.w/lr.d x0, (A)` then `sc.w/d rd, rs2, (A+d)`. The
platform's reservation set is *declared*: `tests/act4/mosaic-p1/sail.json`
(`platform.reservation.reservation_set_size_exp: 3`, 8 bytes, with
`require_exact_reservation_addr: false`) and the UDB config's
`LRSC_RESERVATION_STRATEGY: "reserve exactly enough to cover the access" # XLEN
size reservation`. `mosaic_reservation.sv` declared `GRANULE_BITS = 6`, a
64-byte block. A = `0x80000400` is 64-byte aligned, so the bins at +8..+56 are
inside the DUT's granule but outside the declared 8-byte set: the DUT's SC
succeeds, the reference's fails. Bins at +64 and beyond agree because they leave
the 64-byte block too.

**Fix (rule).** The reservation set is a *platform declaration*, not a choice:
the granule is the naturally aligned XLEN-sized block. `GRANULE_BITS = 3`.

**After.** `Zalrsc-sc.w-00` and `Zalrsc-sc.d-00` pass. The first SC at +8 now
fails as the reference requires.

**Constraint deviation, flagged.** `sim/unit/tb_core_lrsc.cpp` pinned the old
declaration in three places (`kA8Off = 0x408 // inside A's 64-byte granule`,
`kA64Off = 0x440 // the next granule`, and the sampled granule check
`kA & ~63`). That file is another case's driver, which this lane was told not to
edit. It cannot be left stale without failing the gate, so it was migrated to
the corrected rule: `kA8Off = 0x404` with a **word** store (an 8-byte store
inside an 8-byte granule has no 8-aligned address other than A itself, and would
be misaligned and trap), `kA64Off = 0x408`, the granule check mask `~7`, and a
sized `Watch{addr,size}` so A+4 is read at its own width. Main was not consulted
before this edit; it should review it.

## 3. `Zihpm/Zihpm-csrrc-00`, `Zihpm-csrrs-00` -- the HPM counter shadows do not exist

**Before.** First divergence index 72: `ref=0x0 dut=0x0000000000000b20` -- that
word is the trap-signature *byte count* (2848 = 0xb20), not a test value: the
DUT recorded 89 trap records where the reference recorded none. The records show
`XCAUSE = 0x2` (illegal instruction) at `XEPC = 0x80000040, 0x8000005c,
0x80000060, ...` -- every `csrrs/csrrc xN, hpmcounterM, x0` in the test (29
counters x 3 reads). 2848 bytes / 32 bytes per record = 89, and 2848/8 = 356,
which is the "357 words differ" in the V-043 report and this lane's brief: it is
the size of the extra trap signature, **not** a read-modify-write timing
divergence. The first failing instruction is the test's first instruction,
`csrr a2, hpmcounter3`, taking an illegal-instruction trap.

**Diagnosis.** No CSR table or RTL declared any HPM counter: `0xC03..0xC1F` was
outside `addr_impl`, so a read raised illegal instruction. The reference
(`sail.json`, `writable_hpm_counters: 0xFFFF_FFFF`, event 0 so nothing counts)
reads zero. The capability ladder advertises Zihpm with the clause "Hardware
performance counters, read-only zero when no counters are implemented", and the
profile already declares `mcounteren` bits 31:3 unmodifiable (read-only zero) --
so the missing piece is the architectural read-only-zero shadows themselves.

**Fix applied.** `config/csr/mode_su.json`: 29 `hpmcounter3..31` rows (role
`counter-shadow`, `ro`, `fixed`, reset 0) in the U-mode block, which the
p1/p2/p3 profiles load; `tools/gen_manifest.py`: `MOSAIC_CSR_HAS_HPM` plus the
`MOSAIC_CSR_HPM_FIRST/LAST` range (refusing a table with a hole in it);
`rtl/core/mosaic_csr.sv`: the range added to `addr_impl` and to the counter gate
(`csr_counter_bit = csr_addr_i[5:0]`, i.e. HPMn). p0 is deliberately unchanged:
its table has no HPM rows, so the generated flag is absent and the block compiles
out -- `csr.precise_trap_mret` still lists `0xc03/0xc04` as illegal addresses and
still passes.

**After (partial).** The address decode is in, but the two ELFs still fail with
the same first failure: the trap handler runs **in S-mode** ("Trap was being
handled in S-Mode", `XEPC=0x80000040`), where the read is gated by
`mcounteren.HPM3` -- read-only zero in this profile -- while the reference
declares `mcounteren_writable_bits: 0xFFFFFFFF`. So the surviving divergence is
the counter-enable gate (or the mode the ACT4 body runs in), **not** the
read-modify-write semantics the brief hypothesised. That was not fixed here.

## Mutants (written, not yet built or run)

* `MOSAIC_DECODER_MUTANT_C_LUI_IMM` (`mosaic_decoder.sv`) restores the shifted
  `imm20`; the ACT4 `Zca-c.lui-00` ELF must fail on it.
* `MOSAIC_LRSC_MUTANT_GRANULE_64B` (`mosaic_reservation.sv`) restores
  `GRANULE_BITS = 6`; the ACT4 `Zalrsc-sc.w-00`/`-sc.d-00` ELFs must fail.
* `MOSAIC_CSR_MUTANT_NO_HPM` (`mosaic_csr.sv`) removes the shadow decode; the
  ACT4 `Zihpm-csrrs-00` ELF must fail.

All three are `-D` defines in the RTL, so they are built the way the existing
`MOSAIC_ALU_MUTANT_4` control is: from a deleted build directory with the `-D`
on the Verilator command line, and the resulting binary hashes differ from the
shipping one. The natural home for the first two is the existing ACT4 case (the
mutant must make the suite fail on the same ELF); none of these was added to
`tools/run_act_dut.py`'s control list or executed, because the budget ran out.

## What this project's own cases were missing

* **`c.lui`.** `compressed.cross_boundary` was written against the one CI-format
  defect it had already found (`c.addi`) and checks the CI arm it knows;
  `c.lui` is the same format but a different *field position*, and no case
  asserted a `c.lui` result against an independent expectation. The case's
  oracle is the same decompressor's neighbours, so a wrong constant that is
  self-consistent across c.lui's own uses is invisible to it. The lesson is
  concrete: a format-based case must check every field the format places, not
  only the field the last bug was in.
* **SC.** `lrsc.reservation_progress` tests the reservation thoroughly *against
  the RTL's own declaration*: its in/out-of-granule addresses and its
  `granule == A & ~63` check were derived from `GRANULE_BITS = 6`, so the case
  and the RTL agreed on a 64-byte set and neither was ever compared with the
  platform's declared set. It never pins the reservation set from the *config*
  (`reservation_set_size_exp`, `LRSC_RESERVATION_STRATEGY`), and it never
  performs an SC at an address different from the LR's -- which is exactly the
  observable the declared size controls. That is the case's missing check: the
  set size must be read from the platform declaration, not restated in the
  testbench.
* **Zihpm.** The exclusion ledger (EX-009) claims "Zihpm is advertised at p0 and
  its read-only-zero behaviour is verified by the named CSR cases, which pass".
  No case (or table) mentions `hpmcounter` at all -- the claim is prose, not
  evidence. `csr.precise_trap_mret` even lists `0xb03/0xc03/0xc04` as addresses
  that *must* raise illegal, i.e. the project's own case asserted the absence of
  the CSRs the advertised extension requires. An advertised capability with no
  case for its CSRs is exactly what the capability ladder exists to prevent.

## Verified state at the time of writing

* Full applicable set executed on the current binary, one ELF per `--image`
  run: **applicable=127 passed=125 failed=2**, the failures being
  `Zihpm/Zihpm-csrrc-00` and `Zihpm/Zihpm-csrrs-00`. (Before the fixes: 122/127.)
* Per-ELF signature diffs: `Zca-c.lui-00`, `Zalrsc-sc.w-00`, `Zalrsc-sc.d-00`
  match Sail from the first divergence onwards (the residual differences are the
  Zihpm root cause's trap records, which those images also carry).
* Not done: the twelve named re-runs, the three mutant builds, `make check`,
  `lint_rtl.py --profile p0/p1`, `slang-tidy`, `check_records.py`,
  `check_exclusions.py`, and the exclusion-ledger entry the brief asks for when
  a test's stated direction is wrong.

---

# Lane `HpmAndRecords`: the three fixes completed, with controls

Status: **complete.** The section above is the previous lane's; where its
diagnosis differs from what was measured here, the measurement below is what
governs and the difference is named explicitly.

## 1. Zihpm (ACT4 127/127)

**Diagnosis (measured, not hypothesised).** The previous lane's "surviving
divergence is the counter-enable gate" is right, and the root cause is a
configuration disagreement, not a read-modify-write defect. `tools/run_act_dut.py`
on `Zihpm/Zihpm-csrrs-00.elf` reproduced the first failure exactly as recorded:
trap cause 2 at `XEPC = 0x80000040`, the test's *first* instruction
(`csrrs x12, hpmcounter3, x0`), handled in S-mode. The Zihpm test bodies run in
S-mode (`rvtest_setup.h`'s `RVTEST_BOOT_TO_SMODE`), and the S-mode read of a
counter shadow is gated by `mcounteren.HPMn`. The shipped table
(`config/csr/mode_m.json`) declared `mcounteren` bits 31:3 read-only zero, so
`csr_counter_ok` was false and the read trapped, while the platform the reference
is generated from declares `mcounteren_writable_bits = 0xFFFFFFFF`
(`tests/act4/mosaic-p1/sail.json`). The RTL matched its own table; the table did
not match the platform. Note the brief's read-modify-write hypothesis is *not*
what the observable shows: the test's `csrrs` uses `rs1 = x0`, so no write is
attempted and no read-modify-write occurs; the counter is read-only zero, and the
only thing that was wrong was whether the read was permitted.

**Fix.**
* `config/csr/mode_m.json`: `mcounteren` — `unmodifiable_bits` 31:3 to `[]`,
  `writable_fields` to `31:0`, clause rewritten to state the platform
  declaration it now matches.
* `config/csr/mode_su.json`: `scounteren` — same widening, `behavior`
  `warl_wpri` to `warl` (with no WPRI field left, `warl_wpri` was rejected by the
  configuration check: "behavior warl_wpri requires both writable_fields and
  wpri_fields to be non-empty").
* `tools/gen_manifest.py`: `LESS_PRIVILEGE_ONLY_FIELDS["mcounteren"]` widened
  from `("2","1","0")` to `("31:0",)`. The old list narrowed only CY/TM/IR for a
  profile with no less-privileged mode, which after the table change left p0 with
  bits 31:3 writable and 2:0 read-only zero — an arbitrary split. The HPM bits
  gate counter access from a less-privileged mode exactly as CY/TM/IR do, so a
  profile with no S or U mode narrows all of them and p0's `mcounteren` stays the
  read-only-zero register `csr.precise_trap_mret` and the rule ledger already
  describe.
* The RTL was already right (`csr_counter_bit = csr_addr_i[5:0]` is `HPMn`);
  nothing in `mosaic_csr.sv` changed for this beyond a declaration hoist (below).

**Result.** `Zihpm-csrrs-00` and `Zihpm-csrrc-00` both PASS. Full applicable set:
**applicable=127 run=127 passed=127**, exit 0, on the shipping binary
`sha256 415482da9c7eb707b176597c7ee711e89bbd0b0fa68f918eb9c3ff237ba7baf9`.

**Is Zihpm advertisable?** Yes, at p0 and p1, and it now is. The architectural
suite passes it, and the p0 unit evidence for the read-only-zero behaviour is
real (section 2). No un-advertising is needed.

## 2. `csr.precise_trap_mret` — the expectation was wrong, and the config was too

The case is p0-scoped and its `PhaseIllegalAddress` listed `0xc03`/`0xc04` among
addresses that *must* be unimplemented — while the capability ladder advertises
Zihpm from p0 with the clause "read-only zero when no counters are implemented"
(`config/capability_ladder.json`). The case and `config/csr/mode_m.json` agreed
with each other and both disagreed with the ladder.

**Decision.** Of the brief's two options, the one that matches the ladder is to
declare the shadows implemented, so p0's table now carries them:
`config/csr/mode_m.json` gains `hpmcounter3..31` (0xc03..0xc1f) in the M block as
`ro`/`fixed`, `role: counter-shadow`, reset 0 — the same rows
`config/csr/mode_su.json` already declared for p1..p3 in the U block, and
`collect_csrs` merges the two declarations and refuses them if they disagree.
The RTL needed no change: `MOSAIC_CSR_HAS_HPM` now defines for p0, the range
decodes, and M-mode reads were never gated.

**The case now reads the table.**
* `sim/unit/tb_csr.cpp`: `TableIndex` keeps a second index over *every* generated
  row (`row_of_addr`/`RowOf`/`Impl`) next to the modelled-role index, so the
  shadow no longer calls an implemented hpmcounter illegal.
* `PhaseIllegalAddress` derives from the generated table: a candidate the table
  implements is required to read its table reset value without a trap, and only a
  candidate the table does *not* implement is required to raise illegal, read 0
  and refuse a write. `0xb03` (mhpmevent3) stays in the illegal list on purpose —
  no HPM counter is implemented, so no event selector exists.
* Phase 1's hand-derived table widened from 21 to 50 rows (the 29 shadows are
  rows the table must carry for the advertisement to be honest).
* `PhaseResetState` gained the check the ledger claimed but nothing performed:
  every `hpmcounter` row must be implemented, read 0, be unwritable, and have
  write mask 0, and there must be exactly 29 of them.

**Result.** `csr.precise_trap_mret` PASS, `csr.rule_ledger` PASS (p0).

## 3. EX-009 repaired

The entry claimed "Zihpm is advertised at p0 and its read-only-zero behaviour is
verified by the named CSR cases", which no case performed, and its reason ("the
runner defers them") described the Zihpm suite as deferred when
`tools/run_act_dut.py` generates and runs both Zihpm ELFs at p1. Both statements
are corrected: the reason now says Zihpm is run and passes at p1 and only Zicntr
(unadvertised) and the p0 core's lack of a trap frame are what remain uncovered,
and the note names the checks that now exist (the reset-state phase in
`csr.precise_trap_mret`, the 29 `hpmcounterN.value` rules in `csr.rule_ledger`),
both of which pass. `csr.precise_trap_mret` is added to `affected_cases`.
`check_exclusions.py` and `--negative` (13/13 rejected) are green.

## 4. Controls

`tools/run_act_dut.py --controls` now builds one `-D` mutant per defect from a
**deleted** build directory with the define on the Verilator command line, checks
the mutant binary's hash differs from the shipping one, and requires the named
ELF to exit 1. Run result (all six controls FAIL as required), recorded in
`results/v043/dut_results.json`:

| control | defect | ELF | verdict |
|---|---|---|---|
| `MOSAIC_ALU_MUTANT_4` | `slt` answers the unsigned comparison | `I-slt-00` | FAIL exit 1 |
| `MOSAIC_DECODER_MUTANT_C_LUI_IMM` | `c.lui` immediate 12 bits high | `Zca-c.lui-00` | FAIL exit 1 |
| `MOSAIC_LRSC_MUTANT_GRANULE_64B` | reservation set 64 B, not the declared 8 | `Zalrsc-sc.w-00` | FAIL exit 1 |
| `MOSAIC_CSR_MUTANT_NO_HPM` | Zihpm shadow decode removed | `Zihpm-csrrs-00` | FAIL exit 1 |
| `MOSAIC_CSR_MUTANT_MCOUNTEREN_RO` | `mcounteren` stays read-only zero | `Zihpm-csrrs-00` | FAIL exit 1 |
| `corrupt_expected_signature` (data) | one expected signature word flipped | `I-add-00` | FAIL exit 1 |

Every mutant binary hash differs from the shipping binary's
`415482da9c7eb707b176597c7ee711e89bbd0b0fa68f918eb9c3ff237ba7baf9`:

| control | mutant binary sha256 | differs |
|---|---|---|
| `MOSAIC_ALU_MUTANT_4` | `d7647f0c3ca54d85c0cab2be285ff6351c4dd0c78f8aa046632b88270903ef35` | yes |
| `MOSAIC_DECODER_MUTANT_C_LUI_IMM` | `2c0ceac51410f17574e83f5d7086ffe3361aa86ae4a54ed221c9ba8a78813b18` | yes |
| `MOSAIC_LRSC_MUTANT_GRANULE_64B` | `7663caad6390ba8e11c97d86f2e023d67c3f17ea0c295e6ae594cf5fd1351406` | yes |
| `MOSAIC_CSR_MUTANT_NO_HPM` | `13da4ebb7c802ba41ff76137727267f8c5a7cb68c887336fe74aba0200ef13cc` | yes |
| `MOSAIC_CSR_MUTANT_MCOUNTEREN_RO` | `e4f663d2bfaab2026c29700eab3a4a2b335b1536c58f748760743b352481c3c3` | yes |
| `corrupt_expected_signature` (data) | `79f36dd3570283b8eacf21f7e279ebcc8a2fa997a14fc839c5196005f14e8c23` | yes (ELF) |

The two Zihpm controls are the control for fix 1 in both directions: removing the
shadow decode and reverting the counter-enable mask each make the same ELF fail.
`MOSAIC_CSR_MUTANT_MCOUNTEREN_RO` is new in `rtl/core/mosaic_csr.sv`; it drops the
`mcounteren` write mask so the write in `rvtest_setup.h` cannot enable the
shadows.

Also: `make lint-slang PROFILE=p1` was failing before this lane on
`rtl/core/mosaic_csr.sv:489: identifier 'csr_is_hpm' used before its declaration`
(the previous lane's HPM decode block was placed after its first use); the
declaration is hoisted and both `lint-slang` and `lint_rtl.py --profile p0/p1`
are clean.

## 5. `lrsc.reservation_progress` — the declaration, and the observable it lacked

**The set size now comes from the platform declaration.** The previous lane had
removed the literal `GRANULE_BITS` dependency but left the numbers as literals
with a comment. The declaration is now real data:
* `config/profiles/p0..p3.json` gain a `reservation` block
  (`set_size_exp: 3`, `require_exact_addr: false`); the profile schema gains the
  property.
* `tools/mosaic/config_check.py` gains `_check_reservation`, which validates the
  block and, for a profile that ships an ACT reference declaration
  (`tests/act4/mosaic-<profile>/sail.json`, JSON-with-comments, parsed with a
  comment stripper), requires `set_size_exp` to equal
  `platform.reservation.reservation_set_size_exp` and `require_exact_addr` to
  equal `require_exact_reservation_addr`. The two copies cannot drift.
* `tools/gen_manifest.py` emits `MOSAIC_RESERVATION_SET_SIZE_EXP`,
  `MOSAIC_RESERVATION_SET_BYTES` and `MOSAIC_RESERVATION_REQUIRE_EXACT_ADDR`
  into `build/<profile>/sim/mosaic_platform.h`; the case derives its granule mask
  and its in/out-of-set offsets from those constants, with `static_assert`s that
  the probe addresses are legal.

**The missing observable.** Blocks 8 and 9 of Run A now take an LR at B, then
issue the SC at a *different* address: B+4, inside the declared set (a word SC,
the only non-zero offset a legal access can have inside an eight-byte set), and
B+8, the first address of the next set. The declarations decide the expected
status: with `require_exact_addr = false` the in-set SC succeeds and the
out-of-set SC fails; the out-of-set case also checks the sentinel parked there is
untouched, so "a refused SC writes nothing" is checked directly. The harness's
granule check now tracks the address of the outstanding LR (from the LR
transaction) instead of assuming A, so reserving B is checked as state rather
than flagged.

**The RTL granule was wrong and is fixed** (previous lane): `GRANULE_BITS = 3`,
the naturally aligned XLEN-sized set the declaration names.

**Control for this fix.** `MOSAIC_LRSC_MUTANT_GRANULE_64B` (restores
`GRANULE_BITS = 6`) built from a deleted directory makes
`lrsc.reservation_progress` exit 1, first failure at `0x80000058` — block 5's SC,
where the 64-byte set makes the out-of-set store at A+8 break a reservation the
declared 8-byte set leaves standing. The same mutant makes the ACT4
`Zalrsc-sc.w-00` ELF fail, so the case and the suite now agree about the granule.

One correction inside the case, flagged: the previous lane's move of the in-set
conflicting store to A+4 left block 5's `lr.d` expectation at `kVal2`, but the LR
now reads that word back in the upper half of the doubleword; the expectation is
`(kInVal << 32) | kVal2`, and the A+4 watch now checks that block 5's successful
*doubleword* SC rewrote the whole declared set (if the SC wrote only four bytes
the word would still hold `kInVal`). Both were unobserved because the file was
not compiled after the previous lane's edit — it also referenced an undefined
`F3_W`, which is now defined.

## 6. Re-runs and gates (all observed)

| case | profile | result |
|---|---|---|
| `core.act_dut` (127 ELFs) | p1 | PASS 127/127, exit 0 |
| `csr.precise_trap_mret` | p0 | PASS |
| `csr.rule_ledger` | p0 | PASS |
| `compressed.cross_boundary` | p0, p1 | PASS |
| `lrsc.reservation_progress` | p0, p1 | PASS (85 checks) |
| `amo.linearization` | p1 | PASS |
| `core.corpus_sweep` | p0 | PASS |
| `core.mem_program` | p0 | PASS |
| `trap.precise_state` | p0 | PASS |
| `privilege.permission_matrix` | p1 | PASS |
| `sv39.walk_and_faults` | p1 | PASS |
| `tlb.sfence_vma` | p1 | PASS |

Gates: `gen_manifest.py --profile p0/p1` OK; `check_profile.py --all` OK;
`check_contracts.py --all` OK (4/4 profiles); `check_records.py` OK (59 packages,
65 registered cases); `check_exclusions.py` OK and `--negative` 13/13 rejected;
`lint_rtl.py --profile p0/p1` 45/45 sources clean; `make lint-slang PROFILE=p1`
clean.

**Not verified by this lane, named so nobody assumes it:** `make check` was not
run as a single target (its constituent gates were run individually, above);
`gen_manifest.py --profile p2/p3` fails on a **pre-existing** configuration error
unrelated to this lane — `csr vtype (0xc21): the table declares it writable but
the address encodes a read-only register`, in `config/csr/vector.json`, which
this lane did not touch.

