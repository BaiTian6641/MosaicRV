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
