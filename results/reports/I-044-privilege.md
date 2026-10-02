# I-044 — S/U privilege and PMP: **not delivered**

**Case** `privilege.permission_matrix` (`sim/unit/tb_core_priv.cpp`),
**RTL** `rtl/core/mosaic_pmp.sv` + the privilege paths in `mosaic_csr.sv`,
`mosaic_interrupt.sv`, `mosaic_lsu_endpoint.sv`, `mosaic_core.sv`,
**config** `config/csr/mode_m.json`, `config/csr/mode_su.json`,
`config/csr/mode_pmp.json`, `config/geometry/p{1,2,3}.json`,
`tools/gen_manifest.py`.

## 1. Verdict

The case exists, builds, and is a real matrix. It **passes under
`--profile p0`** (52 checks, 1415 cycles) and **fails under `--profile p1`**,
which is the profile the card names (`docs/implementation-plan.md` I-044:
"Inputs: p1 CSR/access matrix … PMP granularity/entry count"). Under p1, **71 of
75 matrix rows match the ISA and 4 do not** — and all four are the same machine
defect:

> **A store that the PMP refuses does not raise the store access fault.** The
> endpoint refuses it (nothing is written — the "no side effect" half of the card
> holds) but the fault never reaches the ROB, because a store is *authorised and
> retired before* it is presented to the endpoint. `s-store-deny-w` and
> `u-store-deny-w` therefore retire the trailing `ecall` (cause 9/8) where the ISA
> requires cause 7; `lock-deny-m-store` and `mprv-store-uses-mpp` produce no trap
> at all.

A case that cannot pass in the profile it was written for is not delivered, so
**I-044 is not delivered and S/U/PMP must not be advertised** in
`config/capability_ladder.json` / `misa` / the compiler `-march` string.

What *is* delivered is the case, the enumeration, and five machine defects found
in the process — four of them fixed here with evidence (§7), one left open with
its exact evidence and its owner (§7 D5).

## 2. What the case is, and how the matrix runs

One hand-assembled program is the whole run. It is entered once at the reset
vector; M-mode code installs the trap vector, clears the delegation registers,
programs the PMP entries for each row and then either performs the access itself
or `mret`s into the row's stub so the access executes in S- or U-mode.

* **Nothing is delegated** (`medeleg = mideleg = 0`), so every trap is taken in
  M-mode and one handler serves every row. It writes a 128-byte frame per row —
  `mcause`, `mtval`, `mepc`, `mstatus` and x5..x12 — *before* it touches any of
  them, then sets `mepc` from the frame's resume address, forces
  `mstatus.MPP = M`, clears `MPRV` and `mret`s. Each row's `keep` label is the
  address the handler resumes at, so the run always returns to M-mode.
* **The bootstrap is part of the matrix, not a precondition.** To reach S-mode the
  machine must execute M-mode code that writes `mstatus.MPP` and executes `MRET`;
  the `boot-mret-mpp-s` / `boot-mret-mpp-u` rows assert that transition directly —
  the cause of the `ecall` the target stub executes (9 / 8) *is* the evidence of
  which mode the machine resumed in, and the frame's `mstatus.MPP` is the ISA's
  own record of the mode the trap came from. In a profile with no S/U the same
  rows assert the opposite outcome (cause 11, MPP = M), so "the core cannot enter
  S-mode" would be a *result* of this case rather than a silent skip. Under p1
  the machine enters both S and U.
* **The handler clears `MPRV` before its first data access.** "When MPRV is set,
  load and store instructions in M-mode use MPP as the effective privilege", and
  that applies to the handler too: a frame store with MPRV still set is checked as
  an MPP-mode access and can be refused by the very entry the trap is about, which
  turns one trap into an endless one. The case's own MPRV rows are what found it
  (the pre-trap `mstatus` is read into a register the frame does not carry, then
  MPRV is cleared, then the frame is written).

### Where the expectations come from

Nothing in the comparison is read out of the DUT:

1. every row carries a hand-written expectation from the privileged
   specification's rules (quoted in the `rule` field of the row and in §6);
2. a second, independent model of the same rules (`PmpModel` in the driver,
   written from the same text) computes the same answer from the entry values the
   driver programmed, and the driver **requires the two to agree before it
   compares the machine with either** — a row whose hand-written expectation and
   model disagree is a driver bug and fails as one. It earned its keep: it caught
   two rows whose *shape* was wrong (an NA4 entry cannot cover an 8-byte access,
   and `W=1,R=0` is the reserved combination that WARL canonicalises to `R=1`);
3. "no side effect" is checked three ways — the destination register in the frame
   still holds its pre-value, the memory word at the accessed address is
   unchanged, and no transaction for that address ever reached the data port;
4. the mode a trap was taken from is `mstatus.MPP` in the frame, so a row that
   claims to run in S-mode and whose frame says M is a failure, not a skip.

## 3. The access matrix under `--profile p1`

75 rows: modes M/S/U × access classes load/store/fetch/AMO/LR/SC/CSR × PMP
shapes no-entry/OFF/NA4/NAPOT/TOR/locked/overlap/part-word, plus the WARL,
lock, delegation, MPRV and xRET rows. Coverage counts from the run:
`mode M=33 S=31 U=10`; `class load=25 csr=20 fetch=8 store=7 amo=4 lr=3 lr-sc=2 sc=1`;
`shape no-entry=22 napot=18 na4=12 locked=8 tor=4 xret=4 granularity=3 overlap=2 delegation=1`;
`mode×class M-csr=14 S-load=16 S-store=2 S-fetch=3 S-amo=2 S-lr=2 S-sc=1 U-load=2 U-store=1 U-fetch=1 U-csr=4 …`.

The driver *asserts* the coverage: every access class the card names must appear,
the M, S and U rows must all be present in a profile that has them, every S/U
class must appear, all three address-matching modes plus the lock and overlap
shapes must appear, and the machine must have been observed in M, S and U. A row
that is inapplicable to the profile is excluded by a *named* machine fact
(`MOSAIC_PMP_ENTRIES == 0`, no supervisor CSR file), never silently.

| # | scenario | mode | class | PMP shape | observed | ISA expects | verdict |
|---|----------|------|-------|-----------|----------|-------------|---------|
| 1 | `m-load-nomatch` | M | load | no-entry | no trap | allow | PASS |
| 2 | `m-store-nomatch` | M | store | no-entry | no trap | allow | PASS |
| 3 | `m-fetch-nomatch` | M | fetch | no-entry | cause 11 from M | allow | PASS |
| 4 | `m-amoadd-nomatch` | M | amo | no-entry | no trap | allow | PASS |
| 5 | `m-lr-nomatch` | M | lr | no-entry | no trap | allow | PASS |
| 6 | `m-lr-sc-nomatch` | M | lr-sc | no-entry | no trap | allow | PASS |
| 7 | `csr-pmpcfg0-read` | M | csr | no-entry | no trap | allow | PASS |
| 8 | `csr-pmpaddr0-read` | M | csr | no-entry | no trap | allow | PASS |
| 9 | `csr-pmpcfg1-illegal` | M | csr | no-entry | cause 2 from M | refuse cause 5/7/1/2 | PASS |
| 10 | `csr-pmpaddr16-illegal` | M | csr | no-entry | cause 2 from M | refuse cause 5/7/1/2 | PASS |
| 11 | `m-unlocked-deny-load` | M | load | na4 | no trap | allow | PASS |
| 12 | `m-unlocked-deny-store` | M | store | na4 | no trap | allow | PASS |
| 13 | `m-unlocked-deny-fetch` | M | fetch | na4 | cause 11 from M | allow | PASS |
| 14 | `m-unlocked-deny-amo` | M | amo | na4 | no trap | allow | PASS |
| 15 | `csr-pmpcfg0-read-late` | M | csr | no-entry | no trap | allow | PASS |
| 16 | `csr-pmpaddr0-read-late` | M | csr | no-entry | no trap | allow | PASS |
| 17 | `boot-mret-mpp-s` | S | nop | xret | cause 9 from S | allow | PASS |
| 18 | `boot-mret-mpp-u` | U | nop | xret | cause 8 from U | allow | PASS |
| 19 | `boot-mstatus-mpp-warl` | M | csr | no-entry | no trap | allow | PASS |
| 20 | `s-load-allow` | S | load | napot | cause 9 from S | allow | PASS |
| 21 | `s-load-deny-r` | S | load | napot | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 22 | `s-store-allow` | S | store | napot | cause 9 from S | allow | PASS |
| 23 | `s-store-deny-w` | S | store | napot | cause 9 from S | refuse cause 5/7/1/2 | **FAIL** |
| 24 | `s-fetch-allow` | S | fetch | na4 | cause 9 from S | allow | PASS |
| 25 | `s-fetch-deny-x` | S | fetch | na4 | cause 1 from S | refuse cause 5/7/1/2 | PASS |
| 26 | `s-amoadd-allow` | S | amo | napot | cause 9 from S | allow | PASS |
| 27 | `s-amoadd-deny-r` | S | amo | napot | cause 7 from S | refuse cause 5/7/1/2 | PASS |
| 28 | `s-lr-allow` | S | lr | napot | cause 9 from S | allow | PASS |
| 29 | `s-lr-deny-r` | S | lr | napot | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 30 | `s-sc-deny-w` | S | sc | napot | cause 7 from S | refuse cause 5/7/1/2 | PASS |
| 31 | `s-sc-lr-allow` | S | lr-sc | napot | cause 9 from S | allow | PASS |
| 32 | `s-nomatch-load` | S | load | no-entry | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 33 | `s-nomatch-fetch` | S | fetch | no-entry | cause 1 from S | refuse cause 5/7/1/2 | PASS |
| 34 | `s-na4-exact-allow` | S | load | na4 | cause 9 from S | allow | PASS |
| 35 | `s-na4-next-word-deny` | S | load | na4 | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 36 | `s-napot-last-word-allow` | S | load | napot | cause 9 from S | allow | PASS |
| 37 | `s-napot-past-end-deny` | S | load | napot | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 38 | `s-na4-span-first-deny` | S | load | granularity | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 39 | `s-na4-span-last-deny` | S | load | granularity | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 40 | `s-napot-span-allow` | S | load | granularity | cause 9 from S | allow | PASS |
| 41 | `s-tor-allow` | S | load | tor | cause 9 from S | allow | PASS |
| 42 | `s-tor-below-deny` | S | load | tor | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 43 | `s-tor-at-top-deny` | S | load | tor | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 44 | `s-overlap-lower-denies` | S | load | overlap | cause 5 from S | refuse cause 5/7/1/2 | PASS |
| 45 | `s-overlap-lower-allows` | S | load | overlap | cause 9 from S | allow | PASS |
| 46 | `u-load-allow` | U | load | napot | cause 8 from U | allow | PASS |
| 47 | `u-store-deny-w` | U | store | napot | cause 8 from U | refuse cause 5/7/1/2 | **FAIL** |
| 48 | `u-fetch-deny-x` | U | fetch | na4 | cause 1 from U | refuse cause 5/7/1/2 | PASS |
| 49 | `u-nomatch-load` | U | load | no-entry | cause 5 from U | refuse cause 5/7/1/2 | PASS |
| 50 | `lock-deny-m-load` | M | load | locked | cause 5 from M | refuse cause 5/7/1/2 | PASS |
| 51 | `lock-deny-m-store` | M | store | locked | no trap | refuse cause 5/7/1/2 | **FAIL** |
| 52 | `lock-deny-m-fetch` | M | fetch | locked | cause 1 from M | refuse cause 5/7/1/2 | PASS |
| 53 | `lock-allow-m-load` | M | load | locked | no trap | allow | PASS |
| 54 | `lock-cfg-write-ignored` | M | csr | locked | no trap | allow | PASS |
| 55 | `lock-addr-write-ignored` | M | csr | locked | no trap | allow | PASS |
| 56 | `lock-off-matches-nothing` | M | load | locked | no trap | allow | PASS |
| 57 | `lock-tor-prev-addr-ignored` | M | csr | locked | no trap | allow | PASS |
| 58 | `tor-unlocked-prev-addr-writable` | M | csr | tor | no trap | allow | PASS |
| 59 | `warl-cfg-reserved-bits` | M | csr | na4 | no trap | allow | PASS |
| 60 | `warl-cfg-w-only` | M | csr | na4 | no trap | allow | PASS |
| 61 | `warl-addr-high-bits` | M | csr | na4 | no trap | allow | PASS |
| 62 | `s-csr-mstatus-read-illegal` | S | csr | no-entry | cause 2 from S | refuse cause 5/7/1/2 | PASS |
| 63 | `s-csr-mstatus-write-illegal` | S | csr | no-entry | cause 2 from S | refuse cause 5/7/1/2 | PASS |
| 64 | `s-mret-illegal` | S | mret | xret | cause 2 from S | refuse cause 5/7/1/2 | PASS |
| 65 | `u-sret-illegal` | U | sret | xret | cause 2 from U | refuse cause 5/7/1/2 | PASS |
| 66 | `u-csr-mstatus-read-illegal` | U | csr | no-entry | cause 2 from U | refuse cause 5/7/1/2 | PASS |
| 67 | `u-csr-cycle-gated` | U | csr | no-entry | cause 2 from U | refuse cause 5/7/1/2 | PASS |
| 68 | `u-csr-cycle-allowed` | U | csr | no-entry | cause 8 from U | allow | PASS |
| 69 | `u-csr-pmpaddr-read-illegal` | U | csr | no-entry | cause 2 from U | refuse cause 5/7/1/2 | PASS |
| 70 | `mprv-load-uses-mpp` | M | load | napot | cause 5 from M | refuse cause 5/7/1/2 | PASS |
| 71 | `mprv-store-uses-mpp` | M | store | napot | no trap | refuse cause 5/7/1/2 | **FAIL** |
| 72 | `mprv-off-m-mode-control` | M | load | napot | no trap | allow | PASS |
| 73 | `mprv-does-not-apply-to-fetch` | M | fetch | napot | cause 11 from M | allow | PASS |
| 74 | `deleg-off-s-trap-to-m` | S | load | delegation | cause 5 from S | refuse cause 5/7/1/2 | PASS |

## 4. The access matrix under `--profile p0`

`config/geometry/p0.json` has no `pmp` block, so `MOSAIC_PMP_ENTRIES == 0`:
p0 implements **zero** PMP entries and has **no** supervisor CSR file. The PMP
rows are therefore inapplicable *by construction* (the RTL's no-PMP branch is a
generate arm, not a degenerate engine), and the case says so instead of pretending
to cover them. What p0 does exercise is the M-only, no-entry row of the matrix and
the CSR table:

| # | scenario | mode | class | PMP shape | observed | ISA expects (p0) | verdict |
|---|----------|------|-------|-----------|----------|------------------|---------|
| 1 | `m-load-nomatch` | M | load | no-entry | no trap | allow | PASS |
| 2 | `m-store-nomatch` | M | store | no-entry | no trap | allow | PASS |
| 3 | `m-fetch-nomatch` | M | fetch | no-entry | cause 11 from M | allow | PASS |
| 4 | `m-amoadd-nomatch` | M | amo | no-entry | no trap | allow | PASS |
| 5 | `m-lr-nomatch` | M | lr | no-entry | no trap | allow | PASS |
| 6 | `m-lr-sc-nomatch` | M | lr-sc | no-entry | no trap | allow | PASS |
| 7 | `csr-pmpcfg0-read` | M | csr | no-entry | cause 2 from M | refuse | PASS |
| 8 | `csr-pmpcfg1-illegal` | M | csr | no-entry | cause 2 from M | refuse | PASS |
| 9 | `csr-pmpaddr16-illegal` | M | csr | no-entry | cause 2 from M | refuse | PASS |
| 10 | `boot-mret-mpp-s` | S | nop | xret | cause 11 from M | allow | PASS |
| 11 | `boot-mret-mpp-u` | U | nop | xret | cause 11 from M | allow | PASS |
| 12 | `boot-mstatus-mpp-warl` | M | csr | no-entry | no trap | allow | PASS |

Coverage: `mode M=10 S=1 U=1` (the two S/U rows are the bootstrap rows asserting
the *opposite* outcome — cause 11 and `mstatus.MPP = M`), `class load/store/fetch/
amo/lr/sc/csr` all present, `shape no-entry=10 xret=2`.

So, stated exactly: under p0 the case exercises the M-mode rows, the PMP CSR
existence rows (as *absent*) and the mode bootstrap; **every row that involves a
PMP entry, S-mode or U-mode is exercised only under a p1-shaped configuration.**
The p0 run is not evidence about the PMP engine at all, and the report does not
claim otherwise.

## 5. The CSR permission table

Asserted from S and U mode, with the expected cause of a refusal. `csr[9:8]` is
the minimum privilege of the address; `csr[11:10] == 11` means *read-only*, which
is a statement about writes and not about reads (defect D3 below).

| access | mode | address | expected | observed |
|---|---|---|---|---|
| read `mstatus` | S | 0x300 (min 3) | illegal (2) | cause 2 from S |
| write `mstatus` | S | 0x300 | illegal (2) | cause 2 from S |
| write `sstatus` | S | 0x100 (min 1) | allowed | cause 9 (the stub's ecall) |
| read `mstatus` | U | 0x300 | illegal (2) | cause 2 from U |
| read `cycle` | U | 0xC00, `mcounteren.CY=0` | illegal (2) | cause 2 from U |
| read `cycle` | U | 0xC00, `mcounteren.CY=1` **and** `scounteren.CY=1` | allowed | cause 8 (the stub's ecall) |
| read `pmpaddr0` | U | 0x3B0 (min 3) | illegal (2) | cause 2 from U |
| `mret` | S | — | illegal (2) | cause 2 from S |
| `sret` | U | — | illegal (2) | cause 2 from U |
| read `pmpcfg0` | M | 0x3A0 | implemented, reads the installed byte | 0x1d |
| read `pmpaddr0` | M | 0x3B0 | implemented | 0x20000fff |
| read `pmpcfg1` | M | 0x3A1 | illegal — for RV64 the odd pmpcfg numbers are illegal | cause 2 |
| read `pmpaddr16` | M | 0x3C0 | illegal — beyond the entry count | cause 2 |
| read `pmpcfg0` | M, profile with no `pmp` block | 0x3A0 | illegal | cause 2 (p0) |

The counter gate is the ISA's: a U-mode counter read needs the bit in
`mcounteren` **and** in `scounteren`; an S-mode read needs only `mcounteren`
(asserted by construction of the p0/p1 profiles, not by a row of its own).

## 6. The PMP rules, as verified

Each rule below is asserted by a row of the matrix, against the specification text
and not against the RTL. Attempt 1's rule list was right about the *rules* — every
rule it stated is confirmed by a passing row — and wrong about two things in the
implementation, both found by this case (§7 D1, D2). The list's one imprecision is
worth stating: it says "entry 0's lower bound 0" for TOR, which is right, but it
does not say that `pmpaddr[i-1]` is read *regardless of `pmpcfg[i-1]`* — the
matrix asserts that too (`s-tor-allow` sets entry 1 to A=OFF and entry 2 to TOR
and still matches `[pmpaddr1, pmpaddr2)`).

| rule (Priv v1.12 §2.7.1) | row(s) | observed |
|---|---|---|
| A=OFF matches nothing | `lock-off-matches-nothing`, every "no-entry" row | as stated |
| NA4 matches exactly one word | `s-na4-exact-allow`, `s-na4-next-word-deny` | as stated |
| NAPOT: n trailing ones → 2^(n+3)-byte aligned region | `s-load-allow`, `s-napot-last-word-allow`, `s-napot-past-end-deny` | as stated |
| TOR: `pmpaddr[i-1] <= y < pmpaddr[i]`, entry 0's bound is 0 | `s-tor-allow`, `s-tor-below-deny`, `s-tor-at-top-deny` | as stated |
| the lowest-numbered matching entry decides | `s-overlap-lower-denies`, `s-overlap-lower-allows` | as stated |
| that entry must match **all** bytes, or the access fails | `s-na4-span-first-deny`, `s-na4-span-last-deny`, `s-napot-span-allow` | as stated |
| no match: M succeeds | all "no-entry" rows | as stated |
| no match: S/U fails when at least one entry is implemented | `s-nomatch-load`, `s-nomatch-fetch`, `u-nomatch-load` | as stated |
| L=0 and the access is M → succeeds whatever R/W/X say | `m-unlocked-deny-load/store/fetch/amo` | as stated |
| L=1 (or any S/U access) → the access's own R/W/X bit is required | `lock-deny-m-load/fetch`, all S/U deny rows | as stated |
| a load/LR needs R; a store/SC/AMO needs W; a fetch needs X | `s-load-deny-r`, `s-lr-deny-r`, `s-store-deny-w`(see §7 D5), `s-sc-deny-w`, `s-amoadd-deny-r`, `s-fetch-deny-x` | as stated except the plain-store rows |
| a refusal raises cause 5 (load class) or 7 (store/SC/AMO class) or 1 (fetch) with `tval` = the address | every deny row | as stated except the plain-store rows |
| writes to `pmpcfg[i]` are ignored while `L=1` | `lock-cfg-write-ignored` | pmpcfg2 reads back 0x91909090 after a write of zero |
| writes to `pmpaddr[i]` are ignored while `L=1` | `lock-addr-write-ignored` | pmpaddr12 unchanged |
| a locked entry with A=TOR also freezes `pmpaddr[i-1]` | `lock-tor-prev-addr-ignored` | pmpaddr13 unchanged |
| ... and only while it is locked | `tor-unlocked-prev-addr-writable` | pmpaddr4 takes the written value |
| L locks even with A=OFF | `lock-off-matches-nothing` | as stated |
| reserved bits 6:5 read zero | `warl-cfg-reserved-bits` | as stated |
| `W=1,R=0` canonicalises to `W=1,R=1` | `warl-cfg-w-only` | low byte reads 0x13 |
| `pmpaddr[63:54]` read zero | `warl-addr-high-bits` | as stated |
| MPRV: an M-mode **data** access is checked as MPP's | `mprv-load-uses-mpp`, `mprv-store-uses-mpp` | cause 5 / refused (D5) |
| MPRV does not apply to instruction access | `mprv-does-not-apply-to-fetch` | the fetch is allowed |
| delegation: with `medeleg=0` a trap taken in S is handled in M | `deleg-off-s-trap-to-m` | cause 5 taken in M |

## 7. Defects found

Five, all in the privilege/PMP area. Four are fixed here; each fix is in a file
this lane owns. D5 is not fixed and is why the case is not delivered.

### D1 — the NAPOT comparison masked the wrong half (**fixed**)

`rtl/core/mosaic_pmp.sv`, `byte_match`: the region's fixed bits are everything
*above* bit n, but the comparison was written against the complement of the mask,
so it compared the *free* bits. A NAPOT entry then matched only an address whose
low bits happened to equal the entry's trailing-ones pattern — one word of the
region — and matched nothing else.

Evidence: `csr-pmpaddr0-read` / `csr-pmpcfg0-read` showed the code-region entry
installed as `pmpaddr0 = 0x20000fff`, `pmpcfg0 = 0x1d` (L=0, A=NAPOT, X=1, R=1,
32 KB at 0x80000000), and the very next S-mode row's *fetch* of 0x800079e8 — four
kilobytes inside that region — trapped with cause 1, with
`o_pmp_fetch_deny_ctr_o == 0` (so the query had found **no** matching entry).
Fix: `byte_match = ((y & mask) == (addr_q[i] & mask))`. The all-ones entry still
covers the whole space (`mask` is zero, nothing is compared), which is what the
module's own comment claims. The driver's independent model had the same
inversion and was fixed with it — the model agreeing with the machine is what
made the second failure visible.

### D2 — every supervisor CSR read as unimplemented (**fixed**)

`rtl/core/mosaic_csr.sv`: the M-mode `case` ends with `default: addr_impl = 0`,
and the `ifdef MOSAIC_CSR_HAS_S` arm that follows set only `wr_legal`. So no
S-mode CSR address was ever marked implemented and every one of them raised the
illegal-instruction exception in every mode.

Evidence: the program's own bootstrap — `csrw scounteren` from M-mode at
0x80000020 — trapped with cause 2 at cycle 35, before anything else ran, and the
machine then looped in the handler. Fix: each supervisor arm now raises
`addr_impl` as well.

### D3 — the read-only CSR encoding was applied to reads (**fixed**)

`rtl/core/mosaic_csr.sv`: `csr_priv_ok` required the *write* minimum privilege for
every access, and `csr[11:10] == 11` (read-only) was decoded as "min privilege M".
A U-mode read of the `cycle` counter — permitted by `mcounteren` and `scounteren`
— was therefore refused.

Evidence: `u-csr-cycle-allowed` trapped with cause 2 where the ISA's rule allows
the read (`u-csr-cycle-gated`, the same read with `mcounteren.CY = 0`, is refused
and must stay refused). Fix: the read gate and the write gate are separate
(`csr_priv_ok_r` for `csr_illegal_o`, `csr_priv_ok_r && csr_priv_ok_w` for
`csr_wr_illegal_o`), which is exactly the content of the read-only encoding.

### D4 — an illegal xRET executed before its exception was taken (**fixed**)

`rtl/core/mosaic_core.sv`: `csr_mret_valid = sys_wb_valid && sys_mret_q`. An MRET
executed below M-mode is an exception, and the exception is latched from the
macro's completion — but in that same cycle the completion also strobed the CSR
file's return, so the illegal MRET changed the privilege and redirected to `mepc`
*and* was recorded as excepting. The trap was then taken from the mode the illegal
return had just entered.

Evidence: `s-mret-illegal` (S-mode stub executing `mret`) trapped with cause 2 but
with `mstatus.MPP = U`, and the trap's PC was the same stub — the machine had
executed the MRET, landed in U at `mepc` and only refused the *second* attempt.
Fix: `csr_mret_valid`/`csr_sret_valid`/`wfi_valid_i` are gated with `!sys_exc`, so
an excepting system macro's *effect* is suppressed while its completion is still
offered (which is what latches the trap). `s-mret-illegal` then reports cause 2
taken from S, and `u-sret-illegal` is unchanged.

### D5 — a PMP-refused store does not raise the store access fault (**open**)

`rtl/core/mosaic_core.sv` + `rtl/core/mosaic_store_queue.sv`. A store is
*authorised by its retirement* and only then drained to the endpoint
(`drain_req_valid_o` requires `auth_cnt_q != 0`), so the endpoint's refusal
arrives after the store has architecturally retired. The store queue consumes the
faulting response and only *counts* it (`o_fault_ctr`, `o_last_fault_*`, the last
of which the core ties off); nothing routes it to the ROB. The machine's
compensating mechanism is a **dispatch-time** store check
(`store_fault_kind`/`disp_store_faults`), but it consults the PMA memory map only —
it has no PMP term.

Evidence, from the p1 run (`results/unit/privilege.permission_matrix.*`):

| row | entry | expected | observed |
|---|---|---|---|
| `s-store-deny-w` | NAPOT, R only | cause 7, no write | **no trap**, no write, `o_pmp_deny_ctr_o` incremented and `o_mem_lsu_access_fault_o` incremented at the same cycle |
| `u-store-deny-w` | NAPOT, R only | cause 7, no write | **no trap**, no write |
| `lock-deny-m-store` | locked NA4, no perms (M-mode) | cause 7, no write | **no trap**, no write |
| `mprv-store-uses-mpp` | NAPOT, X only, MPRV=1/MPP=S | cause 7, no write | **no trap**, no write |

The `ecall` that follows each S/U stub is what retires instead (cause 9 / 8), and
in the two M-mode rows nothing is reported at all. The same access shapes in the
**load** class (`s-load-deny-r`, `mprv-load-uses-mpp`, cause 5) and in the
**AMO/LR/SC** class (`s-amoadd-deny-r`, `s-sc-deny-w`, cause 7) are refused *and*
reported correctly, because those are serialised and presented to the endpoint
before retirement.

Why it is not fixed here: the correct fix is not local. A store must not be
authorised to retire until its PMP check has passed, which changes the store
queue's authorisation contract, and the check must be non-speculative (a CSR write
that changes the PMP entries can be older than a dispatched store, so a
dispatch-time query is not sufficient on its own). That is the memory path's
design — I-023/I-038/I-040 own it and four delivered cases
(`core.mem_program`, `amo.linearization`, `lrsc.reservation_progress`,
`mmio.exactly_once`) depend on the contract as it stands. I did not want to change
it under the same commit that is already reporting a failure.


## 8. The controls

`tools/run_priv_controls.py` rebuilds the case with one `-D` per fail mode the
card names, from a deleted build directory, with the `-D` on the command line and
offered to both halves (Verilator for the RTL switches, `-CFLAGS` for the driver
control), and runs it. `python3 tools/run_priv_controls.py --profile p1`:

| control | kind | sha256 (12) | exit | first failure |
|---|---|---|---|---|
| shipping | — | `809cde00e73b` | 1 | `s-store-deny-w`: mcause 9, expected 7 |
| `MOSAIC_PMP_MUTANT_LOCK_IGNORED` | RTL | `7a92df4c6c7f` | 1 | same as shipping |
| `MOSAIC_PMP_MUTANT_OVERLAP_INVERTED` | RTL | `af1bac0e573c` | 1 | same as shipping |
| `MOSAIC_PMP_MUTANT_M_MODE_ENFORCED` | RTL | `c8077e04464a` | 1 | *different*: max-cycles exhausted, 53 traps, head 0x80007a34 |
| `MOSAIC_PRIV_MUTANT_FAULT_WRITES` | driver | `59ade68f2c1f` | 1 | **`s-load-deny-r`: the destination register is untouched** |

`python3 tools/run_priv_controls.py --profile p0`:

| control | sha256 (12) | exit |
|---|---|---|
| shipping | `4a6721191c7d` | 0 |
| `LOCK_IGNORED` / `OVERLAP_INVERTED` / `M_MODE_ENFORCED` | `4a6721191c7d` (**identical** — the p0 elaboration compiles the PMP engine out entirely, so the mutant code does not exist in that build) | 0 |
| `FAULT_WRITES` | `2800e7146d82` (differs) | 0 (inert: p0 runs no refused access) |

**Honest reading of this table: the mutant evidence for I-044 is not
established.** Every control has a differing binary hash where the code it mutates
is in the build, and `M_MODE_ENFORCED` and `FAULT_WRITES` demonstrably change the
case's behaviour — but under p1 the *shipping* build already fails, so a control
that also fails proves nothing about the case's ability to detect it, and under p0
the PMP rows do not run at all. A mutant table is only evidence when the shipping
build passes; this one does not. The table is recorded because the controls were
built and run as required, not because it certifies anything.

The fourth control is a **driver-side** control on purpose: "a permission fault
still writes" cannot be expressed as an RTL switch, because the refusal is
structural (the endpoint never presents the refused access, so there is no write
to leak). Mutating the *checker* to expect the write is the only way to ask "would
this case notice if a refused access had taken effect?", and it is labelled as a
driver control rather than passed off as an RTL mutant.

## 9. The fourteen cases re-run (all from deleted build directories, `--profile p0`)

```
PASS core.corpus_sweep            task=I-023
PASS core.mem_program             task=I-023
PASS core.trap_csr_program        task=I-023
PASS core.corpus_branch           task=I-023
PASS fence.code_and_data_order    task=I-037
PASS mmio.exactly_once            task=I-038
PASS amo.linearization            task=I-039
PASS lrsc.reservation_progress    task=I-040
PASS compressed.cross_boundary    task=I-041
PASS csr.precise_trap_mret        task=I-019
PASS interrupt.boundary_replay    task=I-020
PASS irq.replay_timeline          task=V-015
PASS trap.precise_state           task=V-014
PASS rename.bank_bias_exhaustion  task=I-032
```

The gates: `python3 tools/gen_manifest.py --profile p0` and `--profile p1` both
exit 0; `python3 tools/check_records.py` is green; `python3 tools/lint_rtl.py
--profile p0` reports 43 source files clean; `slang-tidy --std 1800-2017
--single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)` is clean.

## 10. The geometry contract (the `o_geom_*` ports)

`sim/tb/mosaic_core_tb.sv` gained seven **output** ports
(`o_geom_pmp_entries_o`, `o_geom_pmp_g_o`, `o_geom_pmp_grain_bytes_o`,
`o_geom_pmp_cfg_count_o`, `o_geom_has_s_o`, `o_geom_has_u_o`,
`o_geom_priv_least_o`), all read straight from the generated packages by scope
reference. Every one of them is *consumed by a check*, not merely printed:

| port | what consumes it |
|---|---|
| `pmp_entries` | selects which rows are applicable, and is checked against the machine's own CSR decode: `csr-pmpaddr16-illegal` proves an address beyond the count is not implemented, `csr-pmpaddr0-read` proves one inside it is |
| `pmp_g`, `pmp_grain_bytes` | the grain is *derived from the machine* by `warl-addr-high-bits` (a write of all ones with A=OFF reads back with exactly G trailing zeros) and the driver requires `1 << g == grain_bytes` |
| `pmp_cfg_count` | `csr-pmpcfg0-read` (implemented) versus `csr-pmpcfg1-illegal` (the odd number is illegal at every count) |
| `has_s`, `has_u` | choose between the two expectations of the bootstrap rows, and `mstatus.MPP`'s read-back in `boot-mstatus-mpp-warl` is required to equal 1 exactly when `has_s` |
| `priv_least` | `boot-mstatus-mpp-warl` and the handler's return: after a trap and `mret`, `mstatus.MPP` reads back the profile's least-privileged supported mode (0 with U, 3 without) |

So the case *does* compare the RTL against the configured geometry rather than
printing it: the entry count and the grain are proven from the machine's own CSR
behaviour, and the mode support is proven by the mode the machine actually resumes
in.

## 11. Configuration provenance (what attempt 1 landed, and what this case depends on)

Attempt 1's configuration work is real and this case depends on all of it:

* `config/csr/mode_m.json` gained the writable `mstatus` fields (MPP/SPP/MPRV/SUM/
  MXR/TVM/TW/TSR) and supervisor bit 1 in `mie`/`mip`; without MPP and MPRV being
  writable, neither the bootstrap rows nor the MPRV rows could exist at all.
* `config/csr/mode_su.json` is what makes `MOSAIC_CSR_HAS_S`/`HAS_U` true for p1;
  the seven supervisor CSRs the CSR table rows exercise come from it.
* `config/csr/mode_pmp.json` plus the `pmp` block in `config/geometry/p{1,2,3}.json`
  give the entry count (16), the grain (4 bytes, G=0 so NA4 is selectable) and the
  CSR numbers — `pmpcfg0`/`pmpcfg2` and `pmpaddr0..15`. `config/geometry/p0.json`
  has **no** `pmp` block, which is why p0 implements zero entries.
* `tools/gen_manifest.py`'s merge of a register declared in two mode blocks is what
  makes `--profile p1` generate at all; `--profile p1` now exits 0 and its
  generated package carries the supervisor and PMP constants this case reads.
* **p0's generated constants are byte-identical to the pre-change copy**, as
  attempt 1 claimed — I re-checked `build/p0/rtl/mosaic_csr_pkg.svh` and
  `build/p0/sim/mosaic_csr_table.h` against the claim's content (no `MOSAIC_CSR_HAS_S`,
  `mstatus` wmask `0x66aa`, no PMP constants). That is exactly why the p0 run
  exercises the M-only path: with no `pmp` block and no supervisor file, every
  PMP-shaped and every S/U-shaped row is inapplicable there.

## 12. Not covered (honest list)

* **Translation does not exist.** Sv39/Sv48, the page-table walker, `satp`, `SUM`,
  `MXR`, A/D updates and page faults are I-045/I-046. `satp` is present as a CSR
  and is exercised only as a CSR (`TVM` is *not* asserted by a row of its own).
  Every address in this matrix is physical.
* **`satp.TVM` / `mstatus.TW` / `mstatus.TSR`** are writable and the RTL reads
  them, but only TSR's effect is indirectly exercised (SRET in S-mode is asserted
  illegal by construction of the profile; a `TSR=1` row is not written).
* **The "an access whose *last* byte matches an entry and whose *first* byte does
  not" case in M-mode is not asserted.** The specification's "the lowest-numbered
  entry that matches any byte of an access" makes such an access fail, while the
  RTL decides "no match" from the first byte alone and lets M-mode succeed. The
  S-mode form of the same shape (`s-na4-span-last-deny`) is asserted and both agree
  (deny); the M-mode form would be a *divergence*, and asserting it would be a
  claim about a rule the module's own comment justifies differently. It is
  recorded here rather than tested, so that a later reader can decide.
* **No interrupt is ever asserted**, so the interaction of PMP with an interrupt
  taken at a privilege boundary is not covered (I-020 covers interrupts; the
  privilege side of it is not).
* **`pmpcfg` writes are asserted per byte only through the read-back rows**; a
  multi-byte write where one byte is locked and its neighbour is not is asserted
  (that is what `lock-cfg-write-ignored` does) but not exhaustively.
* **Multi-hart, hypervisor and `Ss1p13`-style extensions** are out of scope for
  this profile.
* **The p0 profile's PMP and S/U rows are inapplicable**, as §4 says; they are not
  "not covered because nobody wrote them", they are covered under p1 only.

## 13. What would close this package

1. The store-fault path (D5): a store's PMP check must gate its retirement, with
   the fault routed to the ROB. That is the memory path's change, not this lane's.
2. A re-run of this case under p1 with the shipping build passing, which is what
   turns the control table in §8 into evidence.
