# I-001 — M-mode CSR table for profile `p0`

Deliverable: [`config/csr/mode_m.json`](../../config/csr/mode_m.json)
Schema: [`config/schema/csr.schema.json`](../../config/schema/csr.schema.json)
Scope: every CSR MosaicRV `p0` implements in M-mode, on RV64, single hart, no
hypervisor extension, no second translation stage, no PMP.

---

## 1. Ground-truth sources

The authoritative source is the **ratified RISC-V Privileged Specification
v1.12**, taken from the release tag, not from the moving `main` branch:

| What | URL |
| --- | --- |
| Tag | `Priv-v1.12` → commit [`98964261c931d51884f733664b22efe43de93033`](https://github.com/riscv/riscv-isa-manual/commit/98964261c931d51884f733664b22efe43de93033) |
| CSR address / privilege / name table | `https://raw.githubusercontent.com/riscv/riscv-isa-manual/Priv-v1.12/src/priv-csrs.tex` |
| Per-CSR behaviour, WARL/WPRI rules, reset section | `https://raw.githubusercontent.com/riscv/riscv-isa-manual/Priv-v1.12/src/machine.tex` |
| Field layouts | the byte-field/tabular figures inside `src/machine.tex` (v1.12 is LaTeX; the AsciiDoc `.edn` byte-field files exist only on `main`) |

Fetched with `curl` on 2026-09-30. Both files resolved with HTTP 200 at that
tag. Note for future maintainers: v1.12 ships `src/*.tex`, not
`src/priv/*.adoc` — the `main`-branch paths
`src/priv/csrs.adoc` and `src/priv/machine.adoc` return 404 at the `Priv-v1.12`
tag, so citing them would be wrong. `main` (commit
`51c1291fc8168bf36530de3386d3f452069ce327`, 2026-09-29) was read only for
cross-checking the Reset section; it agrees with v1.12 on every statement used
here except that it has since split the normative reset sentences into
labelled norms (`#norm:mstatus_mie_mprv_rst`, `#norm:misa_extensions_reset_ref`,
`#norm:rst_other_state_unspec`) without changing their content.

Every row's `spec_clause` names the file and the line range inside it, so each
claim is checkable against the tag directly.

---

## 2. Address conversions — re-checked arithmetically

Addresses in the JSON are **decimal integers equal to the hex CSR number**.
Each one was converted independently (hex → decimal) and then cross-checked
against the `0x…` literal in `priv-csrs.tex`:

| CSR | hex | decimal | check |
| --- | --- | --- | --- |
| `mstatus` | `0x300` | 768 | 3·16² + 0·16 + 0 = 768 |
| `misa` | `0x301` | 769 | 768 + 1 = 769 |
| `medeleg` | `0x302` | 770 | 768 + 2 = 770 |
| `mideleg` | `0x303` | 771 | 768 + 3 = 771 |
| `mie` | `0x304` | 772 | 768 + 4 = 772 |
| `mtvec` | `0x305` | 773 | 768 + 5 = 773 |
| `mcounteren` | `0x306` | 774 | 768 + 6 = 774 |
| `mscratch` | `0x340` | 832 | 3·256 + 4·16 + 0 = 768 + 64 = 832 |
| `mepc` | `0x341` | 833 | 832 + 1 |
| `mcause` | `0x342` | 834 | 832 + 2 |
| `mtval` | `0x343` | 835 | 832 + 3 |
| `mip` | `0x344` | 836 | 832 + 4 |
| `mvendorid` | `0xF11` | 3857 | 15·256 + 1·16 + 1 = 3840 + 16 + 1 = 3857 |
| `marchid` | `0xF12` | 3858 | 3857 + 1 |
| `mimpid` | `0xF13` | 3859 | 3857 + 2 |
| `mhartid` | `0xF14` | 3860 | 3857 + 3 |
| `mcycle` | `0xB00` | 2816 | 11·256 = 2816 |
| `minstret` | `0xB02` | 2818 | 2816 + 2 |
| `cycle` | `0xC00` | 3072 | 12·256 = 3072 |
| `time` | `0xC01` | 3073 | 3072 + 1 |
| `instret` | `0xC02` | 3074 | 3072 + 2 |

All 21 are ≤ 4095, so all satisfy the schema's 12-bit CSR-number bound.

Independent cross-check with the toolchain: a file was generated straight from
the JSON (`csrr xN, 0x<addr>` for every row) and assembled with
`riscv64-elf-gcc -march=rv64im_zicsr -mabi=lp64`; `riscv64-elf-objdump`
disassembles each encoding back to the *symbolic name GNU expects*, which is
derived independently from the CSR number:

```
300020f3  csrr ra,mstatus      30502373  csrr t1,mtvec
30102173  csrr sp,misa         306023f3  csrr t2,mcounteren
302021f3  csrr gp,medeleg      34002473  csrr s0,mscratch
30302273  csrr tp,mideleg      341024f3  csrr s1,mepc
304022f3  csrr t0,mie          34202573  csrr a0,mcause
343025f3  csrr a1,mtval        f1402873  csrr a6,mhartid
34402673  csrr a2,mip          b00028f3  csrr a7,mcycle
f11026f3  csrr a3,mvendorid    b0202973  csrr s2,minstret
f1202773  csrr a4,marchid      c00029f3  rdcycle s3
f13027f3  csrr a5,mimpid       c0102a73  rdtime s4
                               c0202af3  rdinstret s5
```

21/21 round-trip to the expected register names.

---

## 3. `misa` reset value — arithmetic

`misa` is MXLEN bits wide. For RV64, Fig. `misareg` (v1.12 `machine.tex`
L29–L47) splits it into `MXL[1:0]` = bits 63:62, a WARL-zero field at bits
61:26, and `Extensions[25:0]` at bits 25:0. "Bit 0 encodes presence of extension
'A', … through to bit 25 which encodes 'Z'" (L91–L93), i.e. **one bit per
letter, not per extension name**.

Spec requirements used:

* `machine.tex` L54–L55: "The MXL field is always set to the widest supported
  ISA variant at reset."
* `machine.tex` L98–L99: "At reset, the Extensions field shall contain the
  maximal set of supported extensions, and I shall be selected over E if both
  are available."
* `machine.tex` L2846–L2847 (Reset): "The `misa` register is reset to enable
  the maximal set of supported extensions and widest MXLEN."

`config/profiles/p0.json` freezes `xlen: 64` and
`isa_target.extensions: ["I", "M", "Zicsr", "Zifencei", "Zicntr", "Zihpm"]`.

Step by step:

1. **MXL.** Widest supported ISA is RV64 → `MXL = 2` (Table `misabase`
   L58–L71: `1 → XLEN 32`, `2 → XLEN 64`, `3 → XLEN 128`). `MXL` occupies
   bits 63:62, so `2 << 62 = 0x8000_0000_0000_0000`.
2. **I.** The base integer ISA is present, so the letter `I` bit is set.
   `I` is the 9th letter → bit 8. `1 << 8 = 0x0000_0000_0000_0100`.
3. **M.** The integer multiply/divide extension is present, so the letter `M`
   bit is set. `M` is the 13th letter → bit 12. `1 << 12 =
   0x0000_0000_0000_1000`.
   Bit numbering is not guesswork: the spec's own table `misa`l`etters`
   (L113–L149) gives bit 8 = `I` "RV32I/64I/128I base ISA" (L126) and bit 12 =
   `M` "Integer Multiply/Divide extension" (L130).
4. **Everything else is clear.** Per table `misa`l`etters`: `A`(0), `C`(2),
   `F`(5), `V`(21), `U`(20) and `S`(18) are **not** implemented in `p0`, and
   the remaining letters `B`, `D`, `E`, `G`–`R`, `T`–`Z` are unimplemented too,
   so bits 25:0 = `0x1100` and bits 61:26 are WARL zero.
5. **Sum.** `0x8000_0000_0000_0000 | 0x0000_0000_0000_0100 |
   0x0000_0000_0000_1000 = 0x8000_0000_0000_1100`
   = `9223372036854780160` decimal — the value stored in `reset`.

**`Zicsr`, `Zifencei`, `Zicntr` and `Zihpm` are deliberately absent from
`misa`.** They occupy no letter bit: they are *subsets/mandatory regroupings*
of functionality covered by other letters (`Zicsr`/`Zifencei` are part of base
`I`; `Zicntr` is the base `I`/`M` counters; `Zihpm` is base `M`). Advertising
them would mean inventing bits the Extensions field does not have — Table
`misa`l`etters` (L113–L149) says "All bits that are reserved for future use
must return zero when read."

`misa` is modelled entirely read-only (`access: "rwr"`, `behavior: "warl"`,
`unmodifiable_bits: ["63:0"]`). The spec makes writability *permissive*, not
mandatory: "The MXL field **may** be writable in implementations that support
multiple base ISAs" (L51–L52) and the Extensions field "**can** contain
writable bits **where the implementation allows** the supported ISA to be
modified" (L96–L97). `p0` supports exactly one base ISA and freezes its
extension set, so there is nothing to switch to.

---

## 4. `mstatus` reset value

The spec pins exactly three `mstatus` fields at reset. Verbatim from
`src/machine.tex` §"Reset", lines 2842–2845:

> Upon reset, a hart's privilege mode is set to M.  The `mstatus` fields MIE
> and MPRV are reset to 0.
> If little-endian memory accesses are supported, the `mstatus`/`mstatush`
> field MBE is reset to 0.

and line 2858:

> All other hart state is unspecified.

Two consequences:

* MIE = 0, MPRV = 0, MBE = 0 all agree with the fact that every one of those
  fields is **read-only zero** in an M-only, little-endian build anyway.
* Everything else is *unspecified*, not *zero* — except that line 2857 adds
  "No *WARL* field contains an illegal value." `MPP` is a WARL field
  (L621–L623: "`x`PP fields are *WARL* fields that can hold only privilege mode
  `x` and any implemented privilege mode lower than `x`. If privilege mode
  `x` is not implemented, then `x`PP must be read-only 0"), and with only M
  implemented the *only* legal `MPP` value is `0b11`. So MosaicRV resets
  `MPP = 0b11`.

```
MPP = 0b11 occupies bits 12:11  →  0b11 << 11 = 0x1800 = 6144
MIE (bit 3) = 0, MPRV (bit 17) = 0, MBE (bit 37) = 0, all other fields 0
reset = 0x0000_0000_0000_1800 = 6144
```

### `mstatus` field map (RV64, from Fig. `mstatusreg`, L441–L520)

| Bits | Field | `p0` disposition | Spec basis |
| --- | --- | --- | --- |
| 63 | `SD` | read-only 0 | derived: no FP/vector/context state to dirty |
| 62:38 | WPRI | write-preserve-zero | figure L449 (`\instbitrange{62}{38}`), field row L463 (`\wpri`) |
| 37 | `MBE` | read-only 0 | little-endian-only build ⇒ all of MBE/SBE/UBE read-only 0 (L767–L768) |
| 36 | `SBE` | read-only 0 | "If S-mode is not supported, SBE is read-only 0" L736 |
| 35:34 | `SXL` | read-only 0 | "if S-mode is not supported, then SXL is read-only zero" L646–L647 |
| 33:32 | `UXL` | read-only 0 | "if U-mode is not supported, then UXL is read-only zero" L651–L652 |
| 31:23 | WPRI | write-preserve-zero | figure L454, field row L468 |
| 22 | `TSR` | read-only 0 | "TSR is read-only 0 when S-mode is not supported" L860–L861 |
| 21 | `TW` | read-only 0 | "TW is read-only 0 when there are no modes less privileged than M" L841–L842 |
| 20 | `TVM` | read-only 0 | "TVM is read-only 0 when S-mode is not supported" L821 |
| 19 | `MXR` | read-only 0 | "MXR is read-only 0 if S-mode is not supported" L697–L698 |
| 18 | `SUM` | read-only 0 | "SUM is read-only 0 if S-mode is not supported" L718–L719 |
| 17 | `MPRV` | read-only 0 | "MPRV is read-only 0 if U-mode is not supported" L688 |
| 16:15 | `XS` | read-only 0 | "XS field is read-only zero" L950 |
| 14:13 | `FS` | read-only 0 | "If neither the F extension nor S-mode is implemented, then FS is read-only zero" L928 |
| 12:11 | `MPP` | read-only `0b11` | WARL, only M implemented (L621–L623) |
| 10:9 | `VS` | read-only 0 | "If neither the v registers nor S-mode is implemented, then VS is read-only zero" L944 |
| 8 | `SPP` | read-only 0 | "If privilege mode S is not implemented, then SPP must be read-only 0" L622–L623 |
| 7 | `MPIE` | **writable** | interrupt-enable stack, L504 figure row |
| 6 | `UBE` | read-only 0 | "If U-mode is not supported, UBE is read-only 0" L740 |
| 5 | `SPIE` | **writable** | interrupt-enable stack, L506 figure row |
| 4 | WPRI | write-preserve-zero | figure L507 |
| 3 | `MIE` | **writable** | reset to 0 (L2842–L2843) |
| 2 | WPRI | write-preserve-zero | figure L509 |
| 1 | `SIE` | **writable** | global interrupt-enable stack, L510 figure row |
| 0 | WPRI | write-preserve-zero | figure L511 |

`MPIE`/`MIE` are unambiguously writable. `SPIE`/`SIE` are stored writable
because the spec nowhere makes them read-only zero (contrast the explicit
"must be read-only 0" sentences for `SPP` and `SBE`); they simply have no
effect in an M-only hart. `SD` is read-only because it is a *derived* summary of
FS/VS/XS, all of which are zero.

The three bit lists are exhaustive and pairwise disjoint — verified
programmatically, together with the "no reset value may set a WPRI bit" and
"no reset value may set a bit declared writable" invariants.

---

## 5. Reset values, row by row, with the sentence each comes from

| CSR | reset | source |
| --- | --- | --- |
| `mstatus` | `0x1800` | MIE/MPRV = 0 and MBE = 0 (L2842–L2845) + MPP = `0b11` forced by "No *WARL* field contains an illegal value" (L2857) |
| `misa` | `0x8000000000001100` | "reset to enable the maximal set of supported extensions and widest MXLEN" (L2846–L2847); computed in §3 |
| `medeleg` | `0` | WARL, all bits read-only zero without S/U (L1354–L1356) |
| `mideleg` | `0` | WARL, no delegation target without S/U |
| `mie` | `0` | "The `mstatus` fields MIE and MPRV are reset to 0" ⇒ nothing enabled at reset; no *WARL* field holds an illegal value (L2857) ⇒ all-zero legal |
| `mtvec` | `0` | unspecified (L2858); 0 = Direct mode, base 0, a legal WARL value (L1198, L2857) |
| `mcounteren` | `0` | unspecified; WARL read-only zero without U-mode (L1820–L1824) |
| `mscratch` | `0` | unspecified (L2858) |
| `mepc` | `0` | unspecified (L2858); 0 is a legal WARL value |
| `mcause` | `0` | "the value 0 should be returned on implementations that do not distinguish different reset conditions" (L2860–L2862); p0 does not distinguish |
| `mtval` | `0` | unspecified (L2858); "must be able to hold all valid virtual addresses **and the value zero**" (L2259–L2260) |
| `mip` | `0` | nothing pending at reset; all-zero is legal (L2857) |
| `mvendorid` | `0` | "a value of 0 can be returned to indicate … this is a non-commercial implementation" (L194–L196) |
| `marchid` | `0` | "a value of 0 can be returned to indicate the field is not implemented" (L243–L245) |
| `mimpid` | `0` | "a value of 0 can be returned to indicate that the field is not implemented" (L296–L298) |
| `mhartid` | `0` | `harts: 1`; "at least one hart must have a hart ID of zero" (L332–L333) |
| `mcycle` | `0` | "The counter registers have an arbitrary value after the hart is reset" (L1657) — 0 chosen |
| `minstret` | `0` | same sentence (L1657) — 0 chosen |
| `cycle` | `0` | read-only shadow of `mcycle` (L1805–L1806) |
| `time` | `0` | read-only shadow of memory-mapped `mtime` (L1807–L1808); `mtime` reset unspecified, shadow reset 0 |
| `instret` | `0` | read-only shadow of `minstret` (L1805–L1806) |

---

## 6. `behavior` / `access` rationale

| CSR | access | behavior | why |
| --- | --- | --- | --- |
| `mstatus` | `rwr` | `warl` | MXLEN-bit read/write register (L365); every field either WARL or WPRI |
| `misa` | `rwr` | `warl` | "a WARL read-write register" (L22); fully read-only in this build |
| `medeleg`, `mideleg` | `rw` | `warl` | whole register drawn as one WARL field (L1310–L1347) |
| `mtvec` | `rw` | `warl` | "an MXLEN-bit WARL read/write register" (L1156) |
| `mcounteren` | `rwr` | `warl` | "all fields are *WARL* and may be read-only zero" (L1820–L1822) |
| `mscratch` | `rw` | `warl` | unconstrained MXLEN-bit RW register (L1893); `warl` is the only honest token the schema offers — see §7 |
| `mepc` | `rwr` | `warl` | "mepc is a *WARL* register" (L1945); `mepc[1:0]` read-only zero |
| `mcause` | `rw` | `warl` | read/write (L1984); Exception Code is WLRL, a strict subset of WARL |
| `mtval` | `rw` | `warl` | "If mtval is not read-only zero, it is a *WARL* register" (L2259) |
| `mie`, `mip` | `rwr` | `warl_wpri` | implemented bits MTIP/MTIE/MSIP/MSIE writable; "Bits of mie that are not writable must be read-only zero" (L1434) |
| `mvendorid` | `ro` | `fixed` | read-only identity CSR; MRO in the address table |
| `marchid`, `mimpid`, `mhartid` | `ro` | `fixed` | read-only identity CSRs; MRO |
| `mcycle`, `minstret` | `rw` | `warl` | see the recorded deviation in §7 |
| `cycle`, `time`, `instret` | `ro` | `fixed` | read-only shadows of `mcycle`/`minstret`/`mtime` (L1805–L1808); URO in the address table |

`mie`/`mip` implement exactly bits 7 (`MTIP`/`MTIE`) and 3 (`MSIP`/`MSIE`).
`p0` maps a CLINT (`config/memory/p0.json`) and **no PLIC**, so `MEIP`(11) can
never become pending and is write-preserve-zero, as are all supervisor/user
bits and bits 16+ ("bits 16 and above are designated for platform or custom
use", L1366–L1367 — `p0` designates none). Bits 15:12 are drawn as a literal
`0` in Figs. `mipreg-standard` / `miereg-standard` (L1459, L1501).

---

## 7. Recorded deviations and open ambiguities

Everything below is a place where the spec is permissive, silent, or mildly at
odds with the task card. None is guessed silently.

1. **`mcycle`/`minstret` are `rw`/`warl`, not `ro`/`fixed`.** The task card
   said "counters: `fixed` behavior with `access: "ro"`". The spec's address
   table marks both **MRW** (`priv-csrs.tex` L363–L364) and the text says
   "The counter registers have an arbitrary value after the hart is reset, and
   **can be written with a given value**" (L1657). Declaring them read-only
   would be a factual error in the frozen contract, so the spec wins. The three
   genuinely read-only counters (`cycle`, `time`, `instret`, URO in the address
   table) do get `ro`/`fixed`. *If the architecture owner wants the card's
   version instead, that is a deliberate deviation to be recorded, not a
   default.*
2. **`mcounteren` exists although the spec says it should not.** L1824: "In
   systems without U-mode, the `mcounteren` register should not exist." `p0` is
   M-only. Keeping it as an all-zero WARL register means firmware reading
   `0x306` gets a defined value instead of an illegal-instruction trap, and the
   alternative (omitting it) would make the register's absence indistinguishable
   from a spec violation. Flagged here as a conscious choice.
3. **Which exceptions set `mtval` informatively is undefined in this table.**
   L2175–L2176 assigns that to "the hardware platform". MosaicRV declares
   `mtval` *not* read-only zero (so it is a full-width WARL register), but the
   per-exception policy belongs to the trap RTL, not to this table. Open item.
4. **`marchid = 0` means "unallocated".** A non-zero architecture ID is
   required before any conformance claim (L266–L270). Tracked as a gap, not
   silently accepted as final.
5. **`time` reset = 0 is a shadow decision.** `mtime` is memory-mapped
   platform state and the Privileged spec gives it no reset value; 0 is used for
   the `time` shadow too.
6. **`medeleg`/`mideleg` are listed as fully writable.** They are one WARL
   field each; every bit's only legal value is `0`, so writes are accepted and
   every bit canonicalises to 0. This is the honest description of "WARL with
   a one-value set" and is why they are not listed under
   `unmodifiable_bits`: the *storage* is writable even though the *value* never
   changes. Software-visible behaviour: `medeleg`/`mideleg` always read 0.
7. **`mscratch` uses `behavior: "warl"`.** The spec calls it a plain
   MXLEN-bit read/write register with no field constraints (L1893); the schema
   has no plain-RW token, and an unconstrained register is precisely a WARL
   field whose legal set is all 2⁶⁴ values. Noted so nobody reads `warl` as
   "some bits are special".

---

## 8. Deliberate omissions (the negative case)

The table has 21 rows. Everything below is **absent on purpose**, so
`V-0xx` can assert that a CSR access raises an *illegal instruction* rather
than silently returning 0.

**RV32-only, absent because `p0` is RV64** — each is flagged "RV32 only" in
`priv-csrs.tex` or is defined as an RV32 alias:

| CSR | hex | decimal | Why absent |
| --- | --- | --- | --- |
| `mstatush` | `0x310` | 784 | `priv-csrs.tex` L310: "Additional machine status register, RV32 only." `machine.tex` L522: "For RV32 only, `mstatush` is a 32-bit read/write register". |
| `mcycleh` | `0xB80` | 2944 | `priv-csrs.tex` L369: "Upper 32 bits of `mcycle`, RV32 only." `machine.tex` L1725 caption: "Upper 32 bits of hardware performance monitor counters, RV32 only." |
| `minstreth` | `0xB82` | 2946 | `priv-csrs.tex` L370, same wording |
| `cycleh` | `0xC80` | 3200 | `priv-csrs.tex` L173: "Upper 32 bits of `cycle`, RV32 only"; `machine.tex` L1808–L1810: "Analogously, **on RV32I** the `cycleh`, `instreth` … are read-only shadows of `mcycleh`, `minstreth`" |
| `timeh` | `0xC81` | 3201 | `priv-csrs.tex` L174; `machine.tex` L1811–L1812: "**On RV32I** the `timeh` CSR is a read-only shadow of the upper 32 bits" |
| `instreth` | `0xC82` | 3202 | `priv-csrs.tex` L175; L1808–L1810, same sentence as `cycleh` |
| `medelegh`, `midelegh`, `mieh`, `miph` | `0x312`, `0x313`, `0x314`, `0x354` | 786, 787, 788, 852 | **Not present in the ratified v1.12 machine CSR address table at all** (verified: no `0x312`/`0x313`/`0x314`/`0x354` rows in `priv-csrs.tex` at tag `Priv-v1.12`). They appear only on the `main` development branch as RV32-only upper halves. Absent from `p0` on both counts. |
 [results/reports/I-001-csr-m.md#7C77]

Note `cycle`/`time`/`instret` themselves *are* present and are 64 bits wide on
RV64, because they shadow the 64-bit `mcycle`/`minstret` and the full 64-bit
`mtime` — only their `*h` aliases are RV32-only.

**M-mode CSRs `p0` does not implement** (each is a legitimate M-mode CSR that
this profile deliberately does not provide):

| CSR | hex | decimal | Why absent |
| --- | --- | --- | --- |
| `mconfigptr` | `0xF15` | 3861 | optional configuration-structure pointer; `p0` has no config structure |
| `mcountinhibit` | `0x320` | 800 | counters always run in `p0`; the spec allows absence: "If the `mcountinhibit` register is not implemented, the implementation behaves as though the register were set to zero" (L1878–L1879) |
| `mhpmcounter3`…`31` | `0xB03`…`0xB1F` | 2819–2847 | no hardware performance-monitoring events in `p0`; the spec permits "a legal implementation is to make both the counter and its corresponding event selector be read-only 0" (L1670–L1671) |
| `mhpmevent3`…`31` | `0x323`…`0x33F` | 803–831 | same |
| `menvcfg` | `0x30A` | 778 | no pointer-caching / cache-management features in `p0` |
| `mseccfg` | `0x747` | 1863 | no `Zicfilp`/`Svnapot`/seed-extension support in `p0` |
| `pmpcfg0`…`pmpcfg15`, `pmpaddr0`…`pmpaddr63` | `0x3A0`…, `0x3B0`… | 928…, 944… | PMP entries are read-only zero in `p0`; `pmpaddr0` = 944, `pmpcfg0` = 928 |
| `mtinst`, `mtval2` | `0x34A`, `0x34B` | 842, 843 | instruction-transformation and second-trap-value support; not implemented |
| `mvien`, `mvip` | `0x308`, `0x309` | 776, 777 | not in the ratified v1.12 machine CSR table at all; they exist only on the `main` development branch |
| `mtopei`, `mtopi` | `0x35C`, `0xFB0` | 860, 4016 | not in the ratified v1.12 machine CSR table; `mtopi` is the top-external-interrupt register and `mtopei` its predecessor — neither exists in `p0` |
| `mnscratch`, `mnepc`, `mncause`, `mnstatus` | `0x740`…`0x744` | 1856, 1857, 1858, 1860 | Smrnmi not implemented in `p0` |

These last three rows (`mvien`/`mvip`, `mtopei`/`mtopi`, the Smrnmi set) are
absent for the stronger reason that **the ratified v1.12 table does not define
them**; they are development-branch additions. Note that `mtopi` at `0xFB0` =
4016 *is* inside the schema's 0…4095 range, so its absence is a profile
choice, not a schema limit.

**Explicitly not claimed at all in `p0`** (they are *not* M-mode CSRs, but the
omission matters because a reviewer may look for them): `sstatus`, `sie`,
`stvec`, `scounteren`, `senvcfg`, `satp`, `sscratch`, `sepc`, `scause`,
`stval`, `sip`, `senvcfg`, `sstateen*`, `hstatus`, `hedeleg`, `hideleg`,
`hie`, `hip`, `htval`, `hgei*`, `vsstatus`, … — all of S-mode, U-mode and the
hypervisor extension, none of which `p0` has.

**Negative case this enables:** `csrr x1, 0x310` (`mstatush`), `csrr x1, 0xF15`
(`mconfigptr`), `csrr x1, 0x320` (`mcountinhibit`), `csrr x1, 0x3A0`
(`pmpcfg0`) and `csrr x1, 0xB80` (`mcycleh`) must all raise
**illegal instruction** (mcause = 2) on `p0`. They are the cheapest possible
canaries because the addresses are architecturally assigned — a trap is
unambiguously correct, whereas an unassigned address would be a
platform choice.

---

## 9. Verification performed

* `config/csr/mode_m.json` validates against `config/schema/csr.schema.json`
  with `tools/mosaic/jsonschema_mini.py` — prints `OK`, exits 0.
* Programmatic cross-check of all 21 rows: address equals the upstream hex
  literal; address in 0…4095; no duplicate names; `unmodifiable_bits`,
  `writable_fields` and `wpri_fields` are each internally well-formed, lie
  inside the declared width, and are pairwise disjoint; every `warl_wpri` row
  has a non-empty `writable_fields` *and* a non-empty `wpri_fields`; no
  `reset` value sets a WPRI bit; no `reset` value sets a bit declared writable
  on a non-WARL row; every `spec_clause` names the tag, the commit, the address
  table and the behaviour file. All pass.
* Every decimal address was derived twice (by hand from the hex, and by the
  script) and a third time by assembling `csrr` with each decimal value and
  disassembling: all 21 come back with the expected symbolic CSR name (§2).
