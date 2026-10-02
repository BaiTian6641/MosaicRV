# I-048 — p1 boot contract

Case: `boot.p1_contract` (top `mosaic_core_tb`, driver `sim/unit/tb_core_boot.cpp`).
Result: **Stage A delivered and passing at p1** (`results/unit/boot.p1_contract/p1/result.json`).
Stage B (the Linux contract) **cannot be run in this environment** and is not claimed;
it is recorded as the open exclusion **EX-053** in `config/validation/exclusion_ledger.json`.

`make unit CASE=boot.p1_contract PROFILE=p1` → `RESULT PASS`, 29 checks, no mismatch.

## 1. What the case runs

One firmware image, assembled by the driver, is started at the p1 reset vector
(`0x8000_0000`). The M-mode firmware:

1. installs `mtvec` and a PMP entry (TOR, R/W/X, `[0, 0x8020_0000)`);
2. delegates to S-mode the exceptions the profile allows — `medeleg = 0xB3FF`
   (illegal 2, load/store access 5/7, ECALL-S 9, page faults 12/13/15);
   `medeleg[11]` is never writable;
3. delegates the supervisor interrupts — `mideleg = 0x22` (SSI 1, STI 5);
4. sets `stvec`, `sie = 0x22`, `mie.MTIE = 1`, `sscratch`;
5. **builds its own Sv39 page table** in RAM with `sd`: a level-1 2 MiB identity
   leaf over the first 2 MiB of RAM, and a three-level test subtree under root[1]
   (`VA 0x4000_0000..0x401F_FFFF`, `vpn2=1, vpn1=0, vpn0=0`);
6. writes `satp = Sv39 | (root>>12)`, `sfence.vma`, then `mret` to S-mode with
   `mstatus.MPP=S` and `mstatus.SIE=1`.

The S-mode payload then exercises: a translated load and store, two load page
faults and a store page fault, a load access fault, an `ecall`, an illegal
instruction, a mapping change observed through `sfence.vma`, a machine timer
interrupt, an instruction access fault taken in M-mode (then `mret` back to
S-mode), a supervisor timer interrupt and a supervisor software interrupt. The
trap log is written by whichever handler took each trap; the payload hashes it
and writes four signature words.

## 2. The boot chain, state at each step

| step | hart | PC | key architectural state |
|---|---|---|---|
| reset | M | `0x8000_0000` | `misa = 0x8000…1105`, `satp = 0` |
| firmware setup | M | `0x8000_00xx` | `mtvec`, `pmpcfg0=0x0F`, `pmpaddr0=0x2008_0000` |
| delegation | M | | `medeleg=0xB3FF`, `mideleg=0x22`, `mie=0x80`, `sie=0x22`, `stvec=s_handler` |
| page tables | M | | `root[2]→L1id[0]=2 MiB leaf`, `root[1]→L1t[0]→L0t[0..5]` |
| `satp`+`sfence.vma` | M | | `satp=0x8000_0000_0008_0003` (Sv39, ASID 0, PPN `0x80003`) |
| `mret` | M→S | `0x8000_8000` | `MPP=S`, `SPP` unused, `sstatus.SIE=1` |
| payload | S | `0x8000_80xx` | identity 2 MiB superpage + test subtree |
| `satp=0` | S | | translation off for the rest |
| exit | S | | `tohost = 1` |

## 3. Every trap the payload takes

Compared twice: at the DUT's trap port (`o_trap_cause/epc/tval/target`, taken
*from* S-mode in every row) and in the payload's own log. `scause` is the value
the handler read; `handler` is `stvec` (S) or `mtvec` (M).

| # | event | cause | sepc | stval | handler |
|---|---|---|---|---|---|
| 1 | load page fault, invalid PTE (`VA 0x4000_4000`) | 13 | `ld` PC | `0x4000_4000` | S |
| 2 | store page fault, read-only page (`VA 0x4000_3000`) | 15 | `sd` PC | `0x4000_3000` | S |
| 3 | load page fault, execute-only page (`VA 0x4000_5000`) | 13 | `ld` PC | `0x4000_5000` | S |
| 4 | load access fault outside PMP (`0x9000_0000`) | 5 | `ld` PC | `0x9000_0000` | S |
| 5 | `ecall` from S | 9 | `ecall` PC | 0 | S |
| 6 | illegal instruction (`mret` in S) | 2 | `mret` PC | 0 | S |
| 7 | machine timer interrupt, taken while in S | `0x8000…07` | spin PC | 0 | M |
| 8 | instruction access fault (jump outside PMP) | 1 | `0x9000_0000` | `0x9000_0000` | M |
| 9 | supervisor timer interrupt | `0x8000…05` | spin PC | 0 | S |
| 10 | supervisor software interrupt | `0x8000…01` | first post-write PC | 0 | S |

Trap 3 is the card's **deliberate illegal access**: the access is refused and the
fault is taken by the handler the profile's `medeleg` names. Trap 7/8 are the
"return to M-mode where the profile requires it": both are taken in M-mode and
the M handler `mret`s back into S-mode. `stval` for a system illegal-instruction
trap is 0 in this implementation (the documented gap EX-046), and the case
asserts the profile's own encoding rather than the ISA's preferred one.

The machine-timer and supervisor-timer spins are single-instruction self loops;
the driver raises the timer only after it *observes the spin retire*, so the
interrupted PC is a program fact, not a timing observation.

## 4. The signature, and how it is computed independently

The payload (and both handlers) append `(cause, epc, tval)` to one log. The
payload then computes

```
h = 0x243F6A8885A308D3
h = rotl64(h ^ count, 7)
for each record word w in order: h = rotl64(h ^ w, 7)
```

and writes `sig[0..3] = { hash, count, value loaded from page A, value loaded
from page D after sfence.vma }` at `MOSAIC_SIGNATURE_ADDR`.

The **driver computes the same hash before the DUT runs**, from an expectation
table built from the ISA and from the labels of the program it assembled
(`BuildExpected` / `ExpectedHash`), and requires equality: `sig[0] =
0x9E0C6D2F8EB81EB1`, `sig[1] = 10`, `sig[2] = 0xA5A5A5A5`, `sig[3] =
0xD0D0D0D0`. Nothing the DUT printed feeds the driver's hash. The `sfence.vma`
mapping change is also checked on the data port: the last read of `VA 0x4000_2000`
is physical `0x8001_3000` (page D), never page C after the fence.

## 5. Enabled ISA versus the platform description

The driver hands the firmware an FDT-shaped blob (`0xD00DFEED`, the manifest's
ISA string, the RAM base/size) and reads the DUT's `misa` (read by the M-mode
firmware, compared against `build/p1/manifest.json`'s `misa_reset` and the ISA
string's letters). This check **found a real inconsistency**: the generated CSR
package reported `misa = 0x8000…1100` (I, M) while the manifest said
`0x8000…1105` (I, M, A, C) — the CSR table's `misa.reset` row was the last
hand-kept copy of a derived fact and had drifted. `tools/gen_manifest.py` now
derives `misa`'s reset from the advertised capability list, the same derivation
the manifest uses; p0's generated CSR package is byte-identical (I, M), p1's is
now `0x8000…1105`. `check_profile --all` and the p1 case both agree afterwards.

## 6. Controls

`tools/run_boot_controls.py --profile p1` (each rebuilt from a deleted build
directory with its `-D` on the Verilator command line; exit 0 means the case
passed, exit 1 with a named first failure means the control was caught):

| control | binary sha256 | exit | first failure |
|---|---|---|---|
| shipping | `d5444409cbd0` | 0 | — |
| `MOSAIC_CSR_MUTANT_NO_DELEGATION` | `2f07300512f6` | 1 | trap taken in M where S should take it |
| `MOSAIC_CSR_MUTANT_SRET_NO_RESTORE` | `4d972ddee03e` | 1 | `sstatus.SIE = 0` after `sret` |
| `MOSAIC_BOOT_MUTANT_BAD_PAGETABLE` | `49412be6557e` | 1 | trap count 14 vs 10 (first access faults) |
| `MOSAIC_BOOT_MUTANT_BANNER_ACCEPTS` | `1714c4b81bb1` | 1 | expected a Linux banner, got the self-check hash |

The last control is the card's own failure mode made executable: the *checker*
accepts a Linux banner as the pass criterion, and the run fails the real check.
`0 control(s) did not behave as the case requires`.

## 7. The profile change this package made

The card's Stage A cannot be met by a machine that cannot take its own page
faults in S-mode, so the p1 delegation masks were widened **from the profile's
claimed capabilities** (`tools/gen_manifest.py`), not by hand:

* `medeleg[12]`, `[13]`, `[15]` (page faults) are writable exactly when the
  profile's CSR table owns `satp` (translation). `medeleg[11]` stays read-only
  zero in every profile (the specification says so outright).
* `mideleg[5]`, `mie[5]`, `mip[5]` (STI/STIE/STIP) are writable exactly when the
  profile has a less-privileged mode.
* `rtl/core/mosaic_interrupt.sv` gained the supervisor timer source: `mip[5]`
  (STIP) is raised by the platform timer and by software, with the specification's
  priority order `MEI > MSI > MTI > SEI > SSI > STI`.

p1 masks before → after: `medeleg 0x3FF → 0xB3FF`, `mideleg 0x2 → 0x22`,
`mie`/`mip 0x8A → 0xAA`. **p0's generated CSR package is byte-identical** to
before the change (diffed); only the p1 package changed.

STIP latch semantics: STIP is the OR of the platform timer level and a
software latch, exactly like MTIP. A timer event arriving while STIP is already
asserted leaves it asserted (the pending bit coalesces and cannot be lost while
the source is high); software clears it by writing `mip[5]=0`, which takes effect
once the platform level drops. It is not an event counter, and it does not
re-arm on a second edge while pending — the same contract MTIP has.

`tb_interrupt.cpp` (I-020) was made profile-derived as part of this: its `mip`
shadow and its expectations now read the writable mask and the "has a supervisor
timer" fact from the generated CSR table, and it drives `priv=M` explicitly so
the p1 decision reduces to the one it already modelled. `interrupt.boundary_replay`
now passes at p0 and p1 (17238 comparisons).

## 8. Revision hashes

| file | sha256 (16) |
|---|---|
| `config/csr/mode_m.json` | `a0aec5926f1aa2fc` |
| `config/csr/rule_ledger.json` | `98a0fe00d79eb262` |
| `tools/gen_manifest.py` | `d1e962fece9a2394` |
| `rtl/core/mosaic_interrupt.sv` | `af6084dcb0682fe3` |
| `rtl/core/mosaic_csr.sv` | `e2457ffb78055c52` |
| `sim/unit/tb_core_boot.cpp` | `c8b581da5d25717d` |
| `sim/unit/tb_interrupt.cpp` | `5d701174032f1e6e` |
| `tools/run_boot_controls.py` | `31851ef4b0d7c547` |
| `build/p1/rtl/mosaic_csr_pkg.svh` | `62710a17feeba29d` |
| `build/p0/rtl/mosaic_csr_pkg.svh` | `9135f2ebf632bcee` |

## 9. Boot artifacts (Stage B)

**None exist in this environment.** Searched the whole tree: no `*.dtb`, no
firmware image, no kernel image, no rootfs, no OpenSBI tree.
`build/p1/manifest.json` is a configuration manifest, and the ACT4 ELF set is a
test corpus, not a boot image. Therefore Stage B has no hashes to record and is
not claimed; EX-053 names the four missing artifacts (firmware, DTB matching the
enabled ISA, kernel image, rootfs), each of which would need a recorded sha256,
and the end condition (the case runs a userspace self-check against them). This
package does the work that *can* be done here — the supervisor-mode contract
Linux depends on — and says exactly where the chain stops.

## 10. Not covered

* **A real Linux boot** — no artifact (EX-053).
* **`Sstc`/`stimecmp`** — STIP is platform/software-driven; there is no S-mode
  timer comparator in the memory map, so the standard Sstc path is absent.
* **Translated instruction fetch** — this core fetches untranslated, so the
  S-mode code is identity-mapped and a fetch-side page fault cannot be taken.
* **A U-mode phase** — the payload stays in S-mode; U-mode traps are I-044's
  case's.
* **`mtval` for a system illegal-instruction trap** — the implementation writes 0
  (EX-046); the case asserts that documented encoding.
* **Interrupts during device transactions**, SMP, and IRQ latency — out of scope.
* Observation (not a claim): a software interrupt taken at a one-instruction
  self-loop *immediately* after the `csrw sip` that raised it left the core in
  `recovering` without progress; inserting instructions between the write and the
  loop makes it retire normally. The case names the first post-write instruction
  as the interrupted PC and does not exhibit the stall, but the observation is
  recorded here for I-020/I-046.

## 11. Gates and re-runs

* `check_profile --all`: OK (p0..p3).
* `check_records`: OK (64 delivered packages, 72 registered cases).
* `check_exclusions` and `--negative`: OK (54 entries; 13/13 negative controls
  rejected). EX-053 (Stage B) and EX-054 (`rvv.vset_boundaries`, a pending case
  with no driver, owned by the I-052 lane) were added to keep the closure green.
* `mosaic_interrupt.sv` lints clean at p0 and p1; the p2/p3 `gen_manifest`
  failure (`csr vtype (0xc21)`) and the `mosaic_vec_cfg.sv` `UNUSEDSIGNAL` lint
  failure are the vector lane's in-flight I-052 work, not this package's files.
* `make check`: PASS (config, contracts, event contract, records, exclusions,
  coverage, isolation, plan documents).
* Re-runs, every one **PASS** (verdicts under
  `results/unit/<case>/<profile>/result.json`):
  `core.act_dut` p1 — **applicable 127, generated 127, run 127, passed 127,
  failed 0**, so the external ACT4 baseline is unaffected by the STIP addition;
  `privilege.permission_matrix` p0 and p1; `sv39.walk_and_faults` p1;
  `tlb.sfence_vma` p1; `irq.replay_timeline` p0; `trap.precise_state` p0;
  `core.corpus_sweep` p0; `soc.bus_errors_and_ids` p0;
  `csr.rule_ledger` p0; `interrupt.boundary_replay` p0 and p1;
  `boot.p1_contract` p0 and p1. `tools/run_irq_controls.py` still reports all 3
  I-020 mutants caught.
