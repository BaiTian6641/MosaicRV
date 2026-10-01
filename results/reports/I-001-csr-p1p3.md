# I-001 — S/U, Vector and Hypervisor CSR tables for `p1`, `p2`, `p3`

Deliverables:

| File | Profiles | Status |
| --- | --- | --- |
| [`config/csr/mode_su.json`](../../config/csr/mode_su.json) | `p1`, `p2`, `p3` | shipped |
| [`config/csr/vector.json`](../../config/csr/vector.json) | `p2`, `p3` | shipped |
| [`config/csr/hypervisor.json`](../../config/csr/hypervisor.json) | `p3` | shipped: zero CSRs defined, four absent ranges declared — see [§3](#3-hypervisor-an-extension-that-is-absent) |

Model: the frozen M-mode table
[`config/csr/mode_m.json`](../../config/csr/mode_m.json) and its report
[`results/reports/I-001-csr-m.md`](I-001-csr-m.md). Schema:
[`config/schema/csr.schema.json`](../../config/schema/csr.schema.json).

Current gate state, all observed with the commands below:

```
$ python3 tools/check_profile.py --all
profile p0: configuration OK
profile p1: configuration OK
profile p2: configuration OK
profile p3: configuration OK            exit 0

$ python3 tools/check_profile.py --profile p3 --negative
negative controls: 22/22 illegal configurations rejected   exit 0
```

`p0` 47/47, `p1` 34/34, `p2` 30/30, `p3` 22/22 on the negative controls.

---

## 1. Ground-truth sources, and which revision each row was read from

Three different documents are involved, at three different pinned revisions.
Every row's `spec_clause` names the document, the file and the line range, so
each claim is checkable directly against the pinned revision.

| Used for | Document | Pinned revision | Files |
| --- | --- | --- | --- |
| `mode_su.json` | **RISC-V Privileged Specification v1.12** (ratified) | tag `Priv-v1.12` = commit [`98964261c931d51884f733664b22efe43de93033`](https://github.com/riscv/riscv-isa-manual/commit/98964261c931d51884f733664b22efe43de93033) | `src/priv-csrs.tex`, `src/machine.tex`, `src/supervisor.tex`, `src/c.tex` |
| `vector.json` | **RISC-V Vector Extension Specification v1.0** (ratified) | tag `v1.0` of `riscv/riscv-v-spec` = commit [`3570f998903f00352552b670f1f7b7334f0a144a`](https://github.com/riscvarchive/riscv-v-spec/commit/3570f998903f00352552b670f1f7b7334f0a144a) | `v-spec.adoc` |
| `senvcfg` CBIE/CBCFE only | riscv-isa-manual **main** | commit [`51c1291fc8168bf36530de3386d3f452069ce327`](https://github.com/riscv/riscv-isa-manual/commit/51c1291fc8168bf36530de3386d3f452069ce327) (2026-09-29) | `src/priv/machine.adoc` |

Fetched with `curl` on 2026-09-30; all resolved HTTP 200.

Three path facts that matter and are easy to get wrong, all verified here:

* **`v1.12` ships LaTeX, not AsciiDoc.** `src/priv/csrs.adoc`, `src/priv/machine.adoc`
  and `src/priv/supervisor.adoc` return 404 at the `Priv-v1.12` tag; the whole
  `src/` tree at that commit contains no `.adoc` file. The ratified paths are
  `src/priv-csrs.tex`, `src/machine.tex`, `src/supervisor.tex`,
  `src/hypervisor.tex`. Citing the `.adoc` paths at v1.12 would be wrong.
* **The vector chapter is not in the privileged repository at v1.12.** At that
  tag `src/v.tex` is a 15-line stub that says only "The current working group
  draft is hosted at https://github.com/riscv/riscv-v-spec", and the tag
  contains no vector source at all. The ratified vector text is the
  `riscv-v-spec` repository at tag `v1.0`, which is what `vector.json` cites.
  (`docs/references.md` VR-013 points at the *main*-branch
  `src/unpriv/vector-common.adoc`, which is a **draft**; it was not used as a
  normative source here.)
* **Why main was used at all, and only for one question.** Privileged spec
  v1.12 L919-L922 says the definitions of the `senvcfg` `CBCFE` and `CBIE`
  fields "will be furnished by the forthcoming Zicbom extension" — v1.12 has no
  normative sentence for them at all. The ratified Zicbom text does, and at the
  pinned main commit it is in `src/priv/machine.adoc` L2240-L2261. That is the
  only place this report leaves v1.12, and the reason is stated in the row.

---

## 2. `config/csr/mode_su.json` — S and U

Two mode blocks, `S` (11 rows) then `U` (3 rows).

### 2.1 Address conversions, re-checked arithmetically

JSON addresses are **decimal integers equal to the hex CSR number**.

| CSR | hex | decimal | check |
| --- | --- | --- | --- |
| `sstatus` | `0x100` | 256 | 1·256 + 0·16 + 0 |
| `sie` | `0x104` | 260 | 256 + 4 |
| `stvec` | `0x105` | 261 | 256 + 5 |
| `scounteren` | `0x106` | 262 | 256 + 6 |
| `senvcfg` | `0x10A` | 266 | 256 + 10 |
| `sscratch` | `0x140` | 320 | 1·256 + 4·16 + 0 = 256 + 64 |
| `sepc` | `0x141` | 321 | 320 + 1 |
| `scause` | `0x142` | 322 | 320 + 2 |
| `stval` | `0x143` | 323 | 320 + 3 |
| `sip` | `0x144` | 324 | 320 + 4 |
| `satp` | `0x180` | 384 | 1·256 + 8·16 + 0 = 256 + 128 |
| `cycle` | `0xC00` | 3072 | 12·256 |
| `time` | `0xC01` | 3073 | 3072 + 1 |
| `instret` | `0xC02` | 3074 | 3072 + 2 |

All ≤ 4095, so all satisfy the schema's 12-bit bound.

**Independent cross-check with the toolchain.** A file was generated straight
from the two JSON tables (`csrr x1, 0x<addr>` for all 21 rows) and assembled
with `riscv64-elf-gcc -march=rv64im_zicsr -mabi=lp64`; `riscv64-elf-objdump`
disassembles each encoding back to the symbolic name GNU derives from the CSR
number, which is an authority independent of the RISC-V manual:

```
008020f3  csrr ra,vstart     100020f3  csrr ra,sstatus    c00020f3  rdcycle  ra
009020f3  csrr ra,vxsat      104020f3  csrr ra,sie        c01020f3  rdtime   ra
00a020f3  csrr ra,vxrm       105020f3  csrr ra,stvec      c02020f3  rdinstret ra
00f020f3  csrr ra,vcsr       106020f3  csrr ra,scounteren c20020f3  csrr ra,vl
10a020f3  csrr ra,0x10a      140020f3  csrr ra,sscratch   c21020f3  csrr ra,vtype
141020f3  csrr ra,sepc       180020f3  csrr ra,satp       c22020f3  csrr ra,vlenb
142020f3  csrr ra,scause     144020f3  csrr ra,sip
143020f3  csrr ra,stval
```

20 of 21 round-trip to the expected name. The twenty-first is `senvcfg`
(`0x10A` = 266): the installed GNU assembler prints the raw number because its
CSR name table predates the `senvcfg` allocation, not because the address is
wrong. The encoding `0x10a020f3` decodes to `csr[11:0] = 0x10A`, and
`priv-csrs.tex` L202 is unambiguous: `0x10A & SRW & senvcfg`.

### 2.2 `sstatus` — the one register whose reset value is not zero

Layout (SXLEN = 64, Fig. `sstatusreg`, `supervisor.tex` L104-L165):

| Bits | Field | Disposition here | Basis |
| --- | --- | --- | --- |
| 63 | `SD` | read-only | machine.tex L961-L964 "The SD bit is a read-only bit that summarizes …" |
| 62:34 | WPRI | WPRI | figure L120 / L125 |
| 33:32 | `UXL[1:0]` | **read-only `0b10`** | supervisor.tex L200-L203; see below |
| 31:20 | WPRI | WPRI | figure L122 / L125 |
| 19 | `MXR` | writable | supervisor.tex L217-L221; the read-only-zero condition (machine.tex L697) needs S-mode absent |
| 18 | `SUM` | writable | supervisor.tex L223-L229; L231 "SUM is read-only 0 if `satp`.MODE is read-only 0" — `satp`.MODE is writable here |
| 17 | WPRI | WPRI | figure L125 |
| 16:15 | `XS[1:0]` | read-only 0 | machine.tex L949-L950 |
| 14:13 | `FS[1:0]` | **writable** | machine.tex L925-L926, L930-L931 |
| 12:11 | WPRI | WPRI | figure L147 |
| 10:9 | `VS[1:0]` | **writable** | machine.tex L941-L942, L946-L947 |
| 8 | `SPP` | writable | supervisor.tex L167-L172 |
| 7 | WPRI | WPRI | figure L151 |
| 6 | `UBE` | read-only 0 | supervisor.tex L257-L261; every profile is little-endian |
| 5 | `SPIE` | writable | supervisor.tex L180-L183 |
| 4:2 | WPRI | WPRI | figure L154 |
| 1 | `SIE` | writable | supervisor.tex L174-L178 |
| 0 | WPRI | WPRI | figure L156 |

The three bit lists are exhaustive and pairwise disjoint, so no bit is declared
both writable and read-only.

**`UXL`, and therefore `reset = 0x2_0000_0000` (8589934592).** `UXL` is a WARL
field encoding UXLEN, and the spec explicitly allows it to be read-only
(L200-L203). UXLEN is 64, so `UXL = 0b10`. The reset value of the *register* has
to contain it: Table `misabase` (machine.tex L57-L71) allocates only MXL 1/2/3
(= XLEN 32/64/128) and leaves 0 unallocated, so a `UXL` of 0 would be an illegal
WARL value, and L2857 says "No WARL field contains an illegal value". Reset is
therefore `0b10 << 32 = 8589934592` with every other field 0. This follows the
same convention as the frozen `mstatus` row, whose `reset = 6144` likewise
carries a read-only field value (`MPP = 0b11`).

**`FS`/`VS` must be writable — and that contradicts the frozen M-mode table.**
`machine.tex` L925-L926: "If the F extension is implemented, the FS field shall
not be read-only zero", and L941-L942 says the same for `VS`. `p2` and `p3`
claim `F`, `D` and `V`, so read-only zero would be a spec violation there. For
`p1`, which claims neither, L930-L931 and L946-L947 permit but do not require
read-only zero, and a writable field is the choice that keeps one table valid
for all three profiles. `supervisor.tex` L187-L191 states that reading or
writing any `sstatus` field is equivalent to the homonymous `mstatus` field, so
`sstatus.FS`/`VS` and `mstatus.FS`/`VS` are the same storage.
`config/csr/mode_m.json` declares `mstatus.FS`/`VS` read-only zero for
`p0`–`p3`; **the two tables therefore currently contradict each other, and
`mode_m.json` has to change.** That file is not mine to edit; it is reported
here and to the integrator.

### 2.3 `sie` and `sip` — what `medeleg`/`mideleg` do and do not gate

These are not redefined here: `medeleg` (0x302) and `mideleg` (0x303) are M-mode
CSRs and already exist in `config/csr/mode_m.json`, which this table neither
duplicates nor contradicts.

**Writability does *not* depend on `mideleg`.** The rule that fixes `sie` is
`supervisor.tex` L409-L410: "A bit in `sie` must be writable if the corresponding
interrupt can ever become pending" — a statement about the interrupt source, not
about delegation. `sip` is governed by L400-L403: "Each individual bit in
register `sip` may be writable or may be read-only."

**`mideleg` gates only delivery to S-mode.** `machine.tex` L1404-L1416 makes an
interrupt trap to S-mode only when bit *i* is set in both `sip` and `sie` *and*
bit *i* is clear in `mideleg`. So a `sie` bit can be fully writable and still
never fire if M-mode software leaves the matching `mideleg` bit set.

`medeleg` is the same mechanism for synchronous exceptions: `machine.tex`
L1326-L1329 allocates a `medeleg` bit per `mcause` value and L1354-L1356 makes
`medeleg[11]` (ECALL from M-mode) read-only zero because that cause can never
occur below M.

Per-field disposition, from the platform rather than the delegation bits:

| Bit | Register | `sie` | `sip` | Why |
| --- | --- | --- | --- | --- |
| 1 `SSIP`/`SSIE` | supervisor software | writable | writable | `clint_msip` is mapped in every memory map (L487-L491) |
| 5 `STIP`/`STIE` | supervisor timer | writable | **read-only, not zero** | `clint` is mapped; L485-L486 "If implemented, STIP is read-only in `sip`, and is set and cleared by the execution environment" |
| 9 `SEIP`/`SEIE` | supervisor external | WPRI | WPRI | no interrupt controller exists — `KNOWN_DEVICES` is `uart, test_harness, clint, msip` and no memory map names anything else, so SEIP can never become pending and L499-L501 makes the pair read-only zeros |
| 63:16 | platform/custom | WPRI | WPRI | L346-L347; MosaicRV designates none |
| 4:2, 0, 8:6 | standard reserved | WPRI | WPRI | drawn as literal `0` in Figs. `siereg-standard` / `sipreg-standard` |

`STIP` is the one row that is read-only but *not* write-preserve-zero, because
it can legitimately read 1 at run time. Declaring it in `unmodifiable_bits`
("software can never change it") rather than in `wpri_fields` ("reads always
return zero") is what distinguishes it from `SEIP`.

### 2.4 The rest of the S block, briefly

* `stvec` (0x105) — `BASE[63:2]` and `MODE[1:0]` both WARL (`supervisor.tex`
  L288-L305); MODE 0 Direct and MODE 1 Vectored are accepted, MODE ≥ 2 is
  Reserved (L320) and canonicalises, exactly as the frozen `mtvec` row does.
  Reset 0 = BASE 0, Direct, a legal WARL value.
* `scounteren` (0x106) — 32-bit, `CY`/`TM`/`IR` writable, `HPM31..HPM3`
  write-preserve-zero because MosaicRV implements no `hpmcounter_n` CSR at all.
  L582-L585 makes the fields "effectively WARL"; L576-L580 is the illegal-
  instruction rule a clear bit enables. **Caveat reported in §5.2:** the frozen
  `mcounteren` row is read-only zero, and L590-L591 requires *both* bits set, so
  these three gates are currently inert.
* `senvcfg` (0x10A) — `CBZE` read-only 0 (Zicboz is claimed by no profile);
  `CBCFE` (6) and `CBIE` (5:4) writable because all three profiles claim Zicbom
  and the ratified Zicbom rules make them read-only zero only when Zicbom is
  *not* implemented; `CBIE` is a genuine 2-bit WARL field because `0b10` is
  reserved; `FIOM` (0) writable because the only sentence permitting a read-only
  `FIOM` is conditioned on `satp`.MODE being read-only zero, i.e. always Bare
  (L853), which is not the case here. See §5.1 for the v1.12-deferral issue.
* `sscratch` (0x140) — unconstrained SXLEN-bit read/write; `warl` because the
  schema has no plain read/write behaviour token, same reasoning as the frozen
  `mscratch` row.
* `sepc` (0x141) — **only `sepc[0]` is read-only zero here, not `sepc[1:0]`.**
  The frozen `mepc` row used IALIGN=32 because `p0` is RV64IM without C. Every
  profile that loads this table claims C, and `c.tex` L28-L31: "The C extension
  allows 16-bit instructions to be freely intermixed with 32-bit instructions,
  with the latter now able to start on any 16-bit boundary, i.e., IALIGN=16", so
  the two-low-bits rule of `supervisor.tex` L624-L625 does not apply and the
  `sepc[1]` read-masking rule of L627-L631 never fires.
* `scause` (0x142) — Interrupt = bit 63, Exception Code = bits 62:0, WLRL
  (L674-L676), a strict subset of WARL, so `behavior: "warl"`, matching the
  frozen `mcause` row. Reset 0, mirroring the spec's own "the value 0 should be
  returned on implementations that do not distinguish different reset
  conditions" (machine.tex L2860-L2864).
* `stval` (0x143) — full-width WARL (L799-L801 "must be able to hold all valid
  virtual addresses and the value 0"). The illegal-instruction-bit-return
  feature of L783-L806 is **not** claimed, so the `2^N` requirement does not
  apply. Which traps set `stval` informatively is a platform policy outside this
  table and is recorded as open in §5.4.
* `satp` (0x180) — see §2.6.

### 2.5 The U block: why it contains exactly three rows

`tools/mosaic/config_check.py` L269-L278 fails any profile whose
`privilege_modes` contains a mode no CSR table defines, and `csr.schema.json`
sets `csrs` to `minItems: 1`, so a `U` block with no rows is inexpressible. The
question is therefore not "may it be empty" but "what is the smallest correct
U-mode inventory".

**There is no U-only CSR in this profile.** Every U-mode-accessible CSR other
than the floating-point and vector ones is declared `URO` in `priv-csrs.tex`
(L166-L168 for `cycle`/`time`/`instret`) and is therefore already defined by the
frozen M-mode table, which records what M-mode can reach. For `p1` the floating
point CSRs (`fflags`/`frm`/`fcsr`, 0x001-0x003) must **not** be defined because
`p1` claims neither `F` nor `D`; the vector CSRs belong to `vector.json` and
exist only for `p2`/`p3`. So the only honest content a `U` block can have in a
profile without F/D/V is the three unprivileged counters.

The block therefore re-declares `cycle`, `time` and `instret`, and each row's
`spec_clause` says so explicitly and points at the M-mode row that owns the
register's definition. This is a real expressiveness gap in the schema, which
has no notion of "the lowest privilege at which this CSR may be accessed" — the
concept the *architectural* address encoding actually uses
(`priv-csrs.tex` L36-L37: "The next two bits (csr[9:8]) encode the lowest
privilege level that can access the CSR"). Recommended schema addition:
a per-row `min_privilege` (or `access_modes`) field, after which the U block
could hold only what is genuinely U-visible and the counters would live in one
place.

**U-mode reachability is gated, not free.** A `U`-mode read of any of the three
raises an illegal instruction unless *both* `mcounteren` and `scounteren` have
the corresponding bit set (`supervisor.tex` L576-L580 and the commentary
L590-L591). That is the reason the U block exists as a record rather than as a
declaration of accessibility.

### 2.6 `satp` — the honest WARL value set

`MODE` is a 4-bit WARL field (Fig. `rv64satp`, L972-L994). Table `satp-mode`
(L1043-L1071) allocates, for SXLEN=64: `0` Bare, `1-7` reserved for standard
use, `8` Sv39, `9` Sv48, `10` Sv57, `11` Sv64 (reserved), `12-13` reserved,
`14-15` designated for custom use.

MosaicRV implements **Sv39 only**. `p1`, `p2` and `p3` all claim `Sv39`
(`config/capability_ladder.json`, `Sv39`, `min_profile: p1`) and **none of them
claims `Sv48` or `Sv57`** — `p3`'s own `derived_notes` says "Sv48 … stay
NOT_CLAIMED" even though the ladder lists `Sv48` with `min_profile: p3`. So the
legal `MODE` set recorded here is exactly `{0b0000 Bare, 0b1000 Sv39}`. A write
carrying any other MODE leaves the whole register unmodified
(L1039-L1041: "if `satp` is written with an unsupported MODE, the entire write
has no effect; no fields in `satp` are modified") rather than silently selecting
an unimplemented translation scheme.

`ASID` (bits 59:44) is declared **read-only zero, i.e. ASIDLEN = 0**. L1073
says "The number of ASID bits is *unspecified* and may be zero", so 0 is inside
the specified range and is the conservative end of it; it is also the only
choice consistent with L1009-L1010 ("To select MODE=Bare, software must write
zero to the remaining fields of `satp` (bits 30-0 when SXLEN=32, or bits 59-0
when SXLEN=64)"). `ASIDMAX` is 16 for Sv39 (L1079) and stays available if the
S-mode implementation tasks later need per-address-space fences.
`PPN` (bits 43:0) is the writable WARL root pointer.

Access from S-mode is gated by `mstatus.TVM`: `machine.tex` L816-L820 — "When
TVM=1, attempts to read or write the `satp` CSR or execute an SFENCE.VMA or
SINVAL.VMA instruction while executing in S-mode will raise an illegal
instruction exception".

---

## 3. Hypervisor: an extension that is absent

**`config/csr/hypervisor.json` defines no CSRs at all.** `p3` claims neither the
`H` extension nor a second translation stage, so the architecturally correct
table is the empty one, and the table says so in a form the checker can verify:
`"modes": []` plus four `absent_csr_ranges` entries covering every CSR number
band that the privileged specification allocates to the hypervisor privilege
level. A table full of plausible-looking hypervisor CSRs would advertise
two-stage translation that MosaicRV does not implement, which is the exact
failure this configuration layer exists to prevent.

This section records how that conclusion was reached, because the obvious ways
to get a file that validates all produce a table that lies. Each was written
into a throwaway copy of `config/` and fed to
`config_check.load("p3", config_root=...)`, not assumed.

### 3.1 What the architecture says must happen

`priv-csrs.tex` L114-L118, verbatim:

> Attempts to access a non-existent CSR raise an illegal instruction
> exception.  Attempts to access a CSR without appropriate privilege
> level or to write a read-only register also raise illegal instruction
> exceptions.

and L36-L37: "The top two bits (csr[11:10]) indicate whether the register is
read/write or read-only. The next two bits (csr[9:8]) encode the lowest privilege
level that can access the CSR."

Every hypervisor and VS CSR is allocated in a range whose `csr[9:8]` is `0b10`,
the hypervisor level (Table `csrrwpriv`, L80-L91: `0x200-0x2FF`, `0x600-0x67F`,
`0x680-0x6BF`, `0xA00-0xA7F`, `0xA80-0xABF`, `0xAC0-0xAFF`, `0xE00-0xE7F`,
`0xE80-0xEBF`, `0xEC0-0xEFF`). `p3`'s lowest mode above U is S, so the
hypervisor level does not exist and *every* access to those addresses must
raise an illegal instruction. There is no third possibility: not
read-as-zero, not read-only-zero, not writable-ignored.

### 3.2 Both candidate designs were tested, not assumed

Each candidate was written into a throwaway copy of `config/` and fed to
`config_check.load("p3", config_root=...)`. Results verbatim:

| Design | Candidate | Verdict |
| --- | --- | --- |
| (a) no rows | `{"modes": []}` | `$.modes :: array has 0 items, minItems is 1` |
| (a) no rows | `{"modes": [{"mode": "S", "csrs": []}]}` | `$.modes[0].csrs :: array has 0 items, minItems is 1` |
| (b) H block | `{"modes": [{"mode": "H", "csrs": [hstatus 0x600]}]}` | `$.modes[0].mode :: value 'H' not in enum ['M', 'S', 'U']` **and** `csr table mode H :: profile 'p3' does not claim privilege mode H, so this table may not define it` |
| (b') `reserved` | `{"modes": [{"mode": "S", "csrs": [hstatus 0x600, access "reserved"]}]}` | `csr S.hstatus :: a published CSR table may not declare a reserved address` |
| (c) read-as-zero | `{"modes": [{"mode": "S", "csrs": [hstatus 0x600, access "ro", behavior "read_only_zero"]}]}` | **passes** — and is false, see below |

Design (b) is blocked twice over: `csr.schema.json` L24 fixes `mode` to the
enum `["M", "S", "U"]`, and `config_check.py` L419-L426 rejects any mode block
whose mode is not in the profile's `privilege_modes`. Adding `"H"` to the enum
alone would still fail the checker, and making it pass would require an `H` row
in `config/capability_ladder.json` and `"H"` in `p3`'s `privilege_modes` — that
is, it would make the table claim a privilege level `p3` does not have, which
is the exact "advertise ahead of implementation" failure the configuration
layer exists to prevent.

Design (c) is the one variant that would let `--all` pass, and it is the trap
the task card warned about: it declares `hstatus` (0x600) a readable CSR
returning zero. L114-L115 requires an illegal instruction there. Shipping (c)
would put a false statement into the frozen contract, and a downstream
config→RTL generator would have no way to tell it apart from a real CSR.

### 3.3 What the schema needed, and what was adopted

The table as originally specified could not say "this range is deliberately
unimplemented", and the fix was not in this task's file set. The integrator
adopted the recommended option: `csr.schema.json` now sets `modes` to
`minItems: 0` and adds a top-level `absent_csr_ranges` array of
`{first, last, reason, spec_clause}` objects, and `config_check.py` enforces
three rules on it — `first` must not exceed `last`; a table with no `modes` must
declare at least one absent range; and no CSR defined by *any* table of the
profile may fall inside a declared range. The third rule is the valuable one:
it means "define `hstatus` while also declaring the hypervisor range
unimplemented" is now a hard configuration error rather than a contradiction
for a human to notice. Verified in a throwaway copy of `config/`:

| Injected fault | Verdict |
| --- | --- |
| define `hstatus` (0x600) in an `S` block | `address 0x600 lies inside a range this profile declares unimplemented` |
| define `hgatp` (0x680) in an `S` block | `address 0x680 lies inside a range this profile declares unimplemented` |
| empty table, no `absent_csr_ranges` | `defines no CSRs and declares no absent ranges; an empty table must say what it is deliberately leaving out and why` |
| range with `first` > `last` | `range 0x2ff..0x200 has first > last` |

The rejected option remains option 3 above: adding `"H"` to the mode enum. It
would require a ladder row and a `privilege_modes` change for a mode `p3` does
not implement, and it would turn "the extension is absent" into "the extension
is present but empty" — the opposite of the truth.

### 3.4 The ranges actually declared, and why

`priv-csrs.tex` L80-L91 allocates ten CSR number bands to `csr[9:8] = 0b10`, the
hypervisor privilege level, and L36-L37 says that field "encode[s] the lowest
privilege level that can access the CSR". `p3`'s lowest mode above U is S, so
that level does not exist and no address in those bands can be accessed from any
implemented mode. The shipped table declares four contiguous mergers of those
ten bands:

| Declared range | Merged bands | Contents |
| --- | --- | --- |
| `0x200-0x2FF` (512-767) | L82 | `vsstatus`, `vsie`, `vstvec`, `vsscratch`, `vsepc`, `vscause`, `vstval`, `vsip`, `vsatp` |
| `0x600-0x6FF` (1536-1791) | L83, L84, L85 | `hstatus`, `hedeleg`, `hideleg`, `hie`, `htimedelta`, `hcounteren`, `hgeie`, `henvcfg`, `henvcfgh`, `htimedeltah`, `htval`, `hip`, `hvip`, `htinst`, `hgatp`, `hcontext`, plus the custom RW band |
| `0xA00-0xAFF` (2560-2815) | L86, L87, L88 | no CSR named in the ratified table; standard and custom RW bands at hypervisor level |
| `0xE00-0xEFF` (3584-3839) | L89, L90, L91 | `hgeip` (0xE12), plus the custom RO bands |

Coverage of all ten bands and the absence of any conflict with the 42 CSRs
`p3` defines across its four tables were both checked programmatically.

**Corrections applied to the draft of this file.** The version of
`hypervisor.json` that appeared in the working tree when this section was
written carried three factual errors, all corrected here: it labelled
`0x600-0x67F` as "Virtual Supervisor CSRs" (those are the *Hypervisor* CSRs; the
VS CSRs are `0x200-0x2FF`), it labelled `0x200-0x23F` as "Debug/Trace and
trigger CSRs" (that band is the VS CSR band, truncated mid-range at 0x240), and
it omitted the hypervisor-level bands `0xA00-0xAFF` and `0xE00-0xEFF` entirely,
which is where `hgeip` lives. Correcting them matters: an absent range that is
misnamed or truncated under-reports the illegal-instruction surface the CSR unit
has to implement.

---

## 4. `config/csr/vector.json` — V, VLEN = 128, ELEN = 64

`p2` and `p3` both freeze `isa_target.vlen: 128`, `elen: 64`. One `U` block, seven
rows, all of them U-mode or unprivileged per the v1.0 address table (L99-L105).

| CSR | hex | decimal | access | behaviour | reset |
| --- | --- | --- | --- | --- | --- |
| `vstart` | `0x008` | 8 | `rwr` | `warl_wpri` | 0 |
| `vxsat` | `0x009` | 9 | `rwr` | `warl_wpri` | 0 |
| `vxrm` | `0x00A` | 10 | `rwr` | `warl_wpri` | 0 |
| `vcsr` | `0x00F` | 15 | `rwr` | `warl_wpri` | 0 |
| `vl` | `0xC20` | 3104 | `ro` | `fixed` | 0 |
| `vtype` | `0xC21` | 3105 | `rwr` | `warl_wpri` | 9223372036854775808 |
| `vlenb` | `0xC22` | 3106 | `ro` | `fixed` | **16** |

Addresses cross-checked with the toolchain in §2.1 (all seven round-trip:
`vstart`, `vxsat`, `vxrm`, `vcsr`, `vl`, `vtype`, `vlenb`).

### 4.1 `vlenb` = 16, not 0

`v-spec.adoc` L533-L534: "The XLEN-bit-wide read-only CSR `vlenb` holds the
value VLEN/8, i.e., the vector register length in bytes", and the note at
L536-L537: "The value in `vlenb` is a design-time constant in any
implementation." VLEN is frozen for the life of the hart at 128, so
`vlenb = 128/8 = 16`. This is the one row where the "every counter resets to
zero" instinct is wrong: a reset value of 0 would tell software the vector
register is zero bytes wide, and the value is not a choice the implementation
has — it is a consequence of `isa_target.vlen`.

### 4.2 `vtype` — the reset value the task card got wrong

The card asked for "`vtype` 0 (`vill` 0)". **That value is architecturally
impossible and was not used.** A `vtype` of 0 decodes to `vsew = 0b000`, i.e.
`SEW = 2^0 = 1`, and `SEW` must be at least `SEW_MIN = 8` (L309-L311: "In the
standard extensions, SEW_MIN=8"). L326-L327: "An attempt to set an unsupported
SEW and LMUL configuration sets the `vill` bit in `vtype`." A `vtype` reading 0
with `vill` clear would report an unsupported configuration as legal.

The only self-consistent reset is `vill = 1` with all other bits zero, which is
also what the spec recommends — L680-L681: "It is recommended that at reset,
`vtype.vill` is set, the remaining bits in `vtype` are zero, and `vl` is set to
zero" — and it satisfies the mandatory rule at L676-L678 that "`vtype` and `vl`
must have values that can be read and then restored with a single `vsetvl`
instruction".

```
vill = bit 63 = 1, bits 62:0 = 0   →   reset = 1 << 63 = 9223372036854775808
```

**Which fields are writable when `vill` is 1 versus when it is 0** — the
difference the card flagged, and it is easy to get backwards:

* `vill = 0` (L491-L492: "`vill` … encode[s] that a previous `vset{i}vl{i}`
  instruction attempted to write an unsupported value to `vtype`"): the eight
  configuration bits `vtype[7:0]` — `vlmul[2:0]` = 2:0, `vma` = 3, `vta` = 4,
  `vsew[2:0]` = 7:5 — hold the encoding written by the last *successful*
  `vset{i}vl{i}` and are readable.
* `vill = 1`: L512-L513 "When the `vill` bit is set, the other XLEN-1 bits in
  `vtype` shall be zero." The whole configuration field reads as zero, so the
  offending `vsew`/`vlmul` values are **not** recoverable by reading `vtype`;
  the only way to find out what was rejected is to re-execute the
  `vset{i}vl{i}` and test `vill` again.
* Independently of `vill`, L506-L507: "If the `vill` bit is set, then any attempt
  to execute a vector instruction that depends upon `vtype` will raise an
  illegal-instruction exception" — with the note at L509-L510 that
  `vset{i}vl{i}` and whole-register loads/stores/moves are *not* such an
  instruction, which is what makes the recovery path above possible.

Legal value set recorded in the row. The `vlmul` encoding table (L348-L361)
draws the 3-bit field as three separate columns and is easy to misread:
`0b000` → LMUL 1, `0b001` → 2, `0b010` → 4, `0b011` → 8, `0b100` → the single
"reserved" row (L353), `0b101` → 1/8, `0b110` → 1/4, `0b111` → 1/2. The legal
MosaicRV set is `vsew` ∈ {3,4,5,6} (SEW 8/16/32/64) crossed with `vlmul` ∈
{`0b000`, `0b001`, `0b010`, `0b011`, `0b101`, `0b110`, `0b111`} (LMUL 1, 2, 4, 8,
1/8, 1/4, 1/2), subject to `SEW ≤ LMUL*ELEN` (L323-L324) and to the mandatory
fractional-LMUL floor of L305-L314 for ELEN=64, which needs 1/8, 1/4 and 1/2 —
all three present. Every other `vtype` argument sets `vill` and leaves `vtype`
unchanged apart from `vill`.

### 4.3 The rest of the vector block

* `vstart` — L566-L567: "The `vstart` CSR is defined to have only enough
  writable bits to hold the largest element index (one less than the maximum
  VLMAX)", with the note L569-L572 giving `VLMAX_max = VLEN` and the worked
  example "for VLEN=256, `vstart` would have 8 bits to represent indices from 0
  through 255". VLEN = 128 → largest index 127 → **7 writable bits**; the other
  57 are write-preserve-zero, the only value they can hold given that the upper
  bits are reserved (L574-L575) and the spec explicitly refuses to assign them a
  meaning (L577-L579). Reset 0: L683 says the value is arbitrary at reset, and
  0 is the value every vector instruction itself establishes at the end of
  execution (L555-L558), so it is legal under every `SEW`/`LMUL`.
* `vxsat` — L651-L654: "a single read-write least-significant bit
  (`vxsat[0]`) … Bits `vxsat[XLEN-1:1]` should be written as zeros." Writable
  bit 0, WPRI 63:1, reset 0. "Read-write" here means hardware sets it and
  software clears it; the schema has no hardware-set flag token (§5.5).
* `vxrm` — L612-L615: "a two-bit read-write rounding-mode field in the
  least-significant bits (`vxrm[1:0]`). The upper bits, `vxrm[XLEN-1:2]`, should
  be written as zeros." Writable 1:0, WPRI 63:2, reset 0 = `rnu`. See §5.6 for
  the conservative-mode observation.
* `vcsr` — L663-L672 allocates bits 2:1 to `vxrm[1:0]` and bit 0 to `vxsat`, and
  L617-L619 / L656 say it is the *same storage* as the two dedicated CSRs, so
  both are read-write here. Writable 2:1 and 0, WPRI 63:3, reset 0. See §5.7.
* `vl` — L517-L519: "The XLEN-bit-wide read-only `vl` CSR can only be updated by
  the `vset{i}vl{i}` instructions, and the fault-only-first vector load
  instruction variants." No software-writable state and no encoding constraint,
  so `ro`/`fixed` with the whole register read-only — the same modelling the
  frozen M-mode table uses for the read-only counter shadows. Reset 0 per L680-L681,
  and consistent with the `vill = 1` reset because no `vtype`-dependent
  instruction may execute until the first successful `vset{i}vl{i}`.

---

## 5. Ambiguities found, and how each was resolved

Nothing below was silently guessed. Where the task card and the ratified
specification disagreed, the specification won and the deviation is recorded.

### 5.1 `senvcfg.CBIE` / `CBCFE` — v1.12 has no normative text

`supervisor.tex` L919-L922: "The definitions of the CBCFE and CBIE fields will
be furnished by the forthcoming Zicbom extension." At the ratified v1.12 tag
there is no writability sentence for these bits at all, so v1.12 alone cannot
justify any access model.
**Resolution:** read the ratified Zicbom rules at the pinned main commit
(`src/priv/machine.adoc` L2240-L2261) — "When the Zicbom extension is not
implemented, CBCFE is read-only zero" and "The Zicbom extension adds the CBIE …
WARL field … When the Zicbom extension is not implemented, CBIE is read-only
zero. The encoding `0b10` is reserved." `p1`, `p2` and `p3` all claim Zicbom
(`config/capability_ladder.json`, `Zicbom`, `min_profile: p1`), so both fields
are writable, and `CBIE` is recorded as a two-bit WARL field whose legal values
are `0b00`, `0b01`, `0b11`.

### 5.2 `scounteren` is currently inert because of the frozen `mcounteren` row

`supervisor.tex` L590-L591: "U-mode may only access a counter if the
corresponding bits in `scounteren` and `mcounteren` are both set."
`config/csr/mode_m.json` declares `mcounteren` read-only zero for **all four**
profiles, on the grounds that `p0` is M-only. For `p1`–`p3` that makes the
`scounteren` bits this table declares writable unobservable: setting
`scounteren.CY` still leaves U-mode reads of `cycle` trapping.
**Resolution:** `scounteren` is declared as the spec requires (CY/TM/IR
writable, HPM31:3 write-preserve-zero because no `hpmcounter_n` CSR exists).
The `mcounteren` row is not mine and the spec does permit read-only zero even
with U-mode (`machine.tex` L1820-L1823), so this is reported, not patched: the
U-mode counter path stays unreachable until `mcounteren` is made writable for
`p1`–`p3`. An implementation task must not treat `scounteren` alone as enabling
a counter.

### 5.3 `sstatus.FS`/`VS` versus `mstatus.FS`/`VS` — found, reported, resolved

Described in §2.2. The specification is unambiguous (`machine.tex` L925-L926,
L941-L942) and the M-mode table as this delivery found it was not. This table
follows the specification: `sstatus.FS` (14:13) and `sstatus.VS` (10:9) are
writable, which is mandatory for `p2`/`p3` and permitted for `p1` by
L930-L931/L946-L947. The integrator has since corrected `mode_m.json` to the
same disposition, and `config_check.py` now enforces the direction that
matters — a profile that claims `F` (resp. `V`) may not declare bits 14:13
(resp. 10:9) read-only zero — with two dedicated negative controls. The
opposite direction is deliberately not enforced, because `mode_su.json` is
shared by `p1`, `p2` and `p3` and a writable field that reads zero in a profile
without `F` is the correct single description of "no floating-point state
exists", not a violation. See also §7.

### 5.4 Which traps set `stval` informatively — open

`supervisor.tex` L746-L748: "The hardware platform will specify which exceptions
must set `stval` informatively and which may unconditionally set it to zero."
That platform policy is outside a CSR table. The frozen `mtval` row records the
same open question; this row records it for `stval` too. The illegal-instruction
bit-return feature (L783-L806) is deliberately **not** claimed, which keeps the
`2^N` width requirement off the table.

### 5.5 No behaviour token for "hardware-maintained, software-read-only"

`vl` and `vtype` are read-only to software but are not constants: `vtype` is
WARL-constrained and `vl` changes with every `vset{i}vl{i}`. The schema's
`behavior` enum has no such token, and `config_check.py` L476-L477 rejects
`warl`/`warl_wpri` combined with `access: "ro"`, so `vtype` cannot be both
URO and WARL. Resolution: `vl` uses `ro`/`fixed` to mean "no software-writable
state" (the same convention the frozen M-mode table uses for `cycle`/`time`/
`instret`), and `vtype` uses `rwr`/`warl_wpri` to record the genuine WARL field
structure, with each row's `spec_clause` stating in full that no `CSRRW` path
exists and the only writers are `vset{i}vl{i}`. This is a schema limitation, not
a claim that software may write `vtype`; the RTL must implement it read-only.

### 5.6 `vxrm` reset value: 0 (`rnu`) is legal but not the most conservative

L683 permits any value at reset. `0b00` is `rnu` (round-to-nearest-up,
L636), which can round a value *up*. `0b10` is `rdn` (round-down/truncate,
L638) and is the conservative choice for a fixed-point datapod, since it can
only lose precision, never invent it. The task card named 0 and the card is
followed, so `reset = 0`; the datapath owner should revisit this before the
fixed-point instructions are implemented, and whichever value is chosen the RTL
and this table must agree.

### 5.7 `vcsr[0]` writability is not stated

The `.vcsr` layout table (L663-L672) names the fields but gives no writability
statement, and in practice writing 1 to `vcsr[0]` does not set the accrued
saturation flag, because the flag is set only by executing a saturating
fixed-point instruction. The row follows the normative text of L651-L654, which
declares `vxsat[0]` read-write, and treats `vcsr[0]` as the same bit. The
difference is documented in the row; a future schema should gain a
"hardware-set, write-1-ignored" access class.

### 5.8 `satp.ASIDLEN` = 0, and `satp.MODE` = {Bare, Sv39} only

Both are recorded in §2.6. The `Sv48` case is worth calling out: the capability
ladder has an `Sv48` row with `min_profile: "p3"`, so `Sv48` is *one rung away*
from being claimable, but no profile claims it and `p3`'s `derived_notes` say so
explicitly. `satp` therefore does **not** accept MODE 9.

### 5.9 The card's premise about `sstatush` is wrong

The card says `sstatush`, `sieh` and `siph` "are RV32-only in the ratified
table". At the ratified v1.12 tag, **none of the three exists at all**:
`grep` over `priv-csrs.tex`, `machine.tex`, `supervisor.tex` and
`hypervisor.tex` returns no occurrence of any of them. On main
(`51c1291f`) two of them exist and are indeed marked RV32-only —
`src/priv/csrs.adoc` L208 `0x114 SRW sieh "Upper 32 bits of sie, RV32 only"`
and L225 `0x154 SRW siph` — while address `0x120` is **not** `sstatush` at all
but `scountinhibit` (L216). All three are therefore absent from this table; see
§6.

### 5.10 Defects in the negative-control harness — found, reported, resolved

While this delivery was in progress, `python3 tools/check_profile.py --profile
p2 --negative` was found to report **5 of 47 wrongly accepted**: "claims PMP
one rung too early", "claims S one rung too early", "claims Sv39 one rung too
early", "claims U one rung too early", and "VLEN declared without claiming V".
`--profile p3 --negative` had the identical five, while `p0` was 47/47 and
`p1` was 40/40.

Cause, in the then-current `tools/check_profile.py` L135-L138: `future` was
built as `cap["min_profile"] != self.profile`, which for any profile above
`p0` also includes capabilities *below* that profile. For `p2`/`p3` those five
names are the ones the profile did not already list, and
`_check_capabilities` found nothing wrong with them. The remaining cases were
rejected only for the wrong reason — "array items are not unique" — which is a
false pass. The VLEN control set `vlen: 128`, which `p2`/`p3` already had, so
the mutation was a no-op. Before this delivery those two profiles reported a
clean count only because their base configuration failed to load, which made
every control trivially "rejected" — a vacuous pass, not a working gate.

**This was never caused by the tables in this delivery, and no CSR table could
cause it:** the mutators write only `profiles/<p>.json`; the failure was
entirely in the ladder/profile logic. The integrator has fixed the `future`
computation and the VLEN control, and added two new controls covering the
`mstatus.FS`/`mstatus.VS` rule. Current state, all observed:

```
$ python3 tools/check_profile.py --profile p0 --negative   47/47 rejected   exit 0
$ python3 tools/check_profile.py --profile p1 --negative   34/34 rejected   exit 0
$ python3 tools/check_profile.py --profile p2 --negative   30/30 rejected   exit 0
$ python3 tools/check_profile.py --profile p3 --negative   22/22 rejected   exit 0
```

The `p3` run is now meaningful rather than vacuous: its base configuration
loads, so every control is exercised against a valid configuration and rejected
on its own merits. One gap remains — see §7.

---

## 6. Deliberate omissions, and the illegal-instruction case each one enables

The negative case for every omission is the same architectural rule,
`priv-csrs.tex` L114-L115: *"Attempts to access a non-existent CSR raise an
illegal instruction exception."* Each row below names a concrete instruction and
what must happen.

### 6.1 `mode_su.json` (p1, p2, p3)

| Omitted | Address | Why | Negative case it enables |
| --- | --- | --- | --- |
| `sstatush` | `0x120` = 288 | not allocated at the ratified v1.12 tag at all; on main this address is `scountinhibit` (§5.9) | `csrr x1, 0x120` → illegal instruction in every mode |
| `sieh` | `0x114` = 276 | allocated on main as "RV32 only" (`priv/csrs.adoc` L208); RV64 here | `csrr x1, 0x114` → illegal instruction |
| `siph` | `0x154` = 340 | allocated on main as "RV32 only" (`priv/csrs.adoc` L225); RV64 here | `csrr x1, 0x154` → illegal instruction |
| `scontext` | `0x5A8` = 1448 | Debug/Trace register (`priv-csrs.tex` L221); no profile claims the Debug specification | `csrr x1, 0x5A8` → illegal instruction |
| `fflags`, `frm`, `fcsr` | `0x001`–`0x003` = 1–3 | URW (`priv-csrs.tex` L159-L162) but **`p1` claims neither `F` nor `D`**, so they must not be defined in a table `p1` loads | on `p1`: `csrr x1, 0x001` → illegal instruction. **This omission is forced for `p1` and is a gap for `p2`/`p3` — see §7** |
| `hpmcounter3..31` | `0xB03..0xB1F`, `0xC03..0xC1F` | no hardware performance counters exist in MosaicRV; the `HPM` bits of `scounteren` are write-preserve-zero | `rdinstret`-shaped access to any of these → illegal instruction |
| `cycleh`, `timeh`, `instreth` | `0xC80`–`0xC82` | "RV32 only" (`priv-csrs.tex` L173-L175) | `csrr x1, 0xC80` → illegal instruction |

### 6.2 `vector.json` (p2, p3)

| Omitted | Address | Why | Negative case it enables |
| --- | --- | --- | --- |
| CSR numbers 11–14 | `0x00B`–`0x00E` | "tentatively reserved for future vector CSRs" (`v-spec.adoc` L108-L109) | `csrr x1, 0x00B` → illegal instruction |
| `0xC23`–`0xC7F` | 3107–3199 | unallocated in the vector chapter | `csrr x1, 0xC23` → illegal instruction |
| **every vector CSR, conditionally** | all seven | `v-spec.adoc` L124-L126: "Attempts to execute any vector instruction, or to access the vector CSRs, raise an illegal-instruction exception when `mstatus.VS` is set to Off" | with `mstatus.VS` = Off, `csrr x1, 0xC21` → illegal instruction even though the row exists |

### 6.3 Hypervisor and VS (`p3`) — the whole extension is the omission

`p3` claims neither the `H` extension nor a second translation stage, so every
address in the hypervisor and VS ranges must raise an illegal instruction.
From `priv-csrs.tex` L226-L284 (caption "Currently allocated RISC-V hypervisor
and VS CSR addresses", L282):

| CSR | Address (hex / dec) | CSR | Address (hex / dec) |
| --- | --- | --- | --- |
| `vsstatus` | `0x200` / 512 | `hcounteren` | `0x606` / 1542 |
| `vsie` | `0x204` / 516 | `hgeie` | `0x607` / 1543 |
| `vstvec` | `0x205` / 517 | `htval` | `0x643` / 1603 |
| `vsscratch` | `0x240` / 576 | `hip` | `0x644` / 1604 |
| `vsepc` | `0x241` / 577 | `hvip` | `0x645` / 1605 |
| `vscause` | `0x242` / 578 | `htinst` | `0x64A` / 1610 |
| `vstval` | `0x243` / 579 | `hgatp` | `0x680` / 1664 |
| `vsip` | `0x244` / 580 | `hcontext` | `0x6A8` / 1704 |
| `vsatp` | `0x280` / 640 | `hgeip` | `0xE12` / 3602 |
| `hstatus` | `0x600` / 1536 | `henvcfg` | `0x60A` / 1546 |
| `hedeleg` | `0x602` / 1538 | `henvcfgh` | `0x61A` / 1562 (RV32/HSXLEN=32 only) |
| `hideleg` | `0x603` / 1539 | `htimedelta` | `0x605` / 1541 |
| `hie` | `0x604` / 1540 | `htimedeltah` | `0x615` / 1557 (HSXLEN=32 only) |

Negative case: `csrr x1, 0x600` (`hstatus`) in any mode ≤ S → illegal
instruction, and the same for the whole of `0x200-0x2FF`, `0x600-0x67F`,
`0x680-0x6BF`, `0x6C0-0x6FF`, `0xA00-0xA7F`, `0xA80-0xABF`, `0xAC0-0xAFF`,
`0xE00-0xE7F`, `0xE80-0xEBF` and `0xEC0-0xEFF` — the ten bands Table `csrrwpriv`
(L80-L91) allocates to `csr[9:8] = 0b10`, a privilege level `p3` does not have.
`config/csr/hypervisor.json` declares exactly those ten bands, merged into four
ranges (§3.4), so the illegal-instruction surface is now machine-readable, and
`config_check.py` fails the configuration if any table defines a CSR inside one
of them. That is the check that turns "advertise ahead of implementation" from
a review comment into a build error.

---

## 7. Gaps outside this delivery's file ownership

Reported, not patched, because these files belong to other owners. Items 1 and
5 were raised by this delivery and have since been fixed by the integrator; they
are kept here so the reasoning stays on record.

1. **RESOLVED — `mstatus.FS`/`VS` were read-only zero for `p2`/`p3`**, which
   claim `F` and `V`. `machine.tex` L925-L926 and L941-L942 make that illegal,
   and `supervisor.tex` L187-L191 makes them the same storage as the `sstatus`
   fields this delivery declares writable. `mode_m.json` now declares both
   writable and `config_check.py` enforces it with two negative controls. See
   §2.2 and §5.3.
2. **OPEN — `mcounteren` is still read-only zero for `p1`–`p3`**, so no U-mode
   counter access is possible and the `scounteren` gates this delivery declares
   writable are inert. Legal per `machine.tex` L1820-L1823, but it contradicts
   the platform obligations in the profiles' own `derived_notes`, and an
   implementation task must not treat `scounteren` alone as enabling a counter.
   See §5.2.
3. **OPEN — `config/csr/mode_m.json` omits `mcountinhibit` (0x320 = 800)**, which
   the ratified v1.12 tag does allocate (`priv-csrs.tex` L378, `src/machine.tex`
   L1826-L1864). The checker does not require it, so it is an omission rather
   than a failure.
4. **OPEN — `p2` and `p3` claim `F` and `D` but no CSR table defines `fflags`,
   `frm` or `fcsr` (0x001-0x003).** They cannot go in `mode_su.json`, which
   `p1` also loads and which must not define a CSR belonging to an extension
   `p1` does not claim. They need a `config/csr/fpu.json` listed by `p2`/`p3`,
   or the F/D claim is unbacked at the CSR layer. The checker does not catch
   this: nothing cross-references `isa_target.extensions` against the CSRs a
   table defines.
5. **RESOLVED — the negative controls were only sound for `p0`.** The `future`
   set and the VLEN control are fixed and two `mstatus.FS`/`VS` controls were
   added; all four profiles now reject every control. See §5.10.
6. **OPEN — the `absent_csr_ranges` rule has no negative control.** The three new
   checks it enables were each verified by hand in a throwaway copy of `config/`
   (§3.3), but none of them is in `NegativeControls`, so a future refactor could
   silently drop the "no defined CSR inside a declared absent range" rule — the
   one that matters most — and the suite would stay green. A control that
   inserts an `hstatus` row at 0x600 into a sandboxed copy of `config/csr/`
   would close it.
7. **OPEN — `csr.schema.json` still has no notion of a CSR's lowest accessible
   privilege**, which is the concept the architectural CSR address encoding
   actually uses (`priv-csrs.tex` L36-L37). The `U` block in
   `config/csr/mode_su.json` has to re-declare the three URO counters that the
   M-mode table already owns (§2.5). A per-row `min_privilege` field would let
   the counters live in one place.
