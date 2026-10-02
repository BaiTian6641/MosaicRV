# V-017 — the CSR legality-relationship comparator and its rule ledger

Case: **`CASE=csr.rule_ledger`** (registered in `tests/unit/registry.json`, task
V-017, `pending: true` until the integration lead records it). Driver:
`sim/unit/tb_core_csr_rules.cpp`, top `mosaic_core_tb`. Card:
`docs/validation-plan.md` §5 V-017 — "build a CSR legality-relationship
comparator".

## STATUS: PASS — 54 rules, 108 example pairs, six controls that fail

```
RESULT PASS csr.rule_ledger rules=54 examples=108 checks=4 comparisons=6493
            cycles=3064 retires=769 traps=22 seed=1
```

Run with the official runner from a deleted build directory:

```
python3 tools/run_unit.py --profile p0 --case csr.rule_ledger
```

The case drives one hand-assembled M-mode program through the integrated p0
core: 54 ledger rules × a positive and a negative example = 108 example
sequences, 6493 comparisons, 769 retirements, 22 illegal-instruction
exceptions. Every one of the 22 traps is required by exactly one rule and no
unexpected trap is taken.

---

## 1. What the case is, and where every expectation comes from

The ledger is the program. `config/csr/rule_ledger.json` states, per rule, the
CSR, the bit field, the rule kind, the specification clause, the applicability
preconditions, and the positive and negative example the rule is checked by.
`tools/gen_manifest.py` transcribes it into
`build/<profile>/sim/mosaic_csr_rules.h`, and `sim/unit/tb_core_csr_rules.cpp`
walks that table:

* for each example it emits one instruction sequence — load the operand from a
  RAM literal pool, perform the CSR operation (`csrrw`/`csrrs`/`csrrc`, or the
  `csrr` read itself), read the CSR back, and store the read-back to a RAM slot;
* it collects the set of illegal-instruction trap PCs the machine raises and
  requires that set to equal, exactly, the set the ledger's examples demand;
* after the run it reads the slots and compares each read-back against the
  ledger's expectation — never against an observed value.

Nothing in the comparison is taken from the DUT. The expected read, the adjacent
illegal value, and whether the access must trap are all ledger data, generated
from `config/csr/mode_m.json` (through the same `effective_csr_wmask` decode the
RTL's write mask comes from) and the specification clauses recorded per CSR.

The machine is the oracle for "was this access permitted": an illegal CSR access
raises the illegal-instruction exception (cause 2) and does not retire, so the
faulting PC appears in the trap set and nowhere in the retirement stream. A
denied write must also leave the destination register unchanged, so an
unimplemented CSR accepted as zero is caught twice: by the missing trap and by
the read-back still holding the canary the driver pre-loaded.

### Coverage is asserted, not assumed

At the end the driver requires every rule index in `MOSAIC_CSR_RULE_IDS` to have
been visited by both a positive and a negative example, and the number of
executed examples to equal `MOSAIC_CSR_EXAMPLE_COUNT`. A rule that no stimulus
reaches fails the case by name; it is never skipped.

---

## 2. The ledger's shape

`config/schema/csr_rules.schema.json` validates it, and
`tools/mosaic/config_check.py::_check_csr_rules` cross-checks it against the
profile's implementation table (every rule names an implemented CSR — or is
explicitly about an address the table does not name — every implemented CSR
carries at least one rule, the example target addresses exist and are writable
where a write is expected, and every rule names the adjacent illegal result it
rejects). A malformed or stale ledger fails `python3 tools/check_profile.py`.

Top level:

| key | meaning |
|---|---|
| `schema_version` | 1 |
| `profile` | `p0` — the implementation table the addresses and preconditions are written against |
| `spec` | Privileged Spec v1.12, tag `Priv-v1.12`, commit `98964261c931d51884f733664b22efe43de93033` |
| `kind_semantics` | what each rule kind means and how the comparator decides accept/reject |
| `rules[]` | one entry per rule |

Each rule: `id`, `csr`, `address`, `field`, `bits`, `kind`, `clause`,
`preconditions[]`, `positive`, `negative`. Each example: `op`
(`csrrw`/`csrrs`/`csrrc`/`csrr`), `write`, `traps` (0/1/2 illegal-instruction
exceptions required), `target` (the CSR read), `write_target` (the CSR written,
which differs from `target` only for an alias rule), `expect`
(`read` exact / `range` inclusive / `advance` min-delta over a gap / `canary`),
`forbid` (the adjacent illegal result the read must never equal), `pre_read`.

### Counts

54 rules, 108 examples. By kind:

| kind | rules | what it constrains |
|---|---:|---|
| `strict` | 22 | a determined value: read-back must equal the expectation exactly |
| `read_only` | 14 | software can never change the field |
| `permission` | 10 | the address/encoding denies the access; it must trap |
| `warl_allowed` | 5 | a WARL field with a stated legal set; accept a legal value, reject the adjacent illegal one |
| `alias` | 3 | two addresses read the same state |

By CSR (the "per-CSR coverage counts" the card asks for):

| CSR | rules | CSR | rules | CSR | rules |
|---|---:|---|---:|---|---:|
| mstatus | 11 | mip | 5 | cycle | 2 |
| mtvec | 3 | mie | 3 | instret | 2 |
| mcycle | 3 | minstret | 3 | time | 2 |
| mvendorid | 2 | marchid | 2 | mimpid | 2 |
| mhartid | 2 | mepc | 2 | misa | 1 |
| medeleg | 1 | mideleg | 1 | mcounteren | 1 |
| mscratch | 1 | mcause | 1 | mtval | 1 |
| unimplemented (3 addresses) | 3 | | | | |

Every one of the 21 CSRs p0 implements carries at least one rule;
`_check_csr_rules` makes that a hard requirement, so the ledger cannot quietly
stop covering a register.

---

## 3. The rule kinds, and how the comparator decides accept/reject

The `kind_semantics` block in the ledger states this; the examples that pin the
boundary are quoted verbatim below.

* **`strict`** — the field's value is fully determined by the write and the
  register's reset state. The comparator requires the read-back to equal the
  expectation exactly and rejects any other value. The positive and negative
  examples drive the field to its `1` pattern and its `0` pattern, so a
  stuck-at-1 or stuck-at-0 bit fails one of them. Example — `mstatus.MIE`:

  ```json
  "positive": { "op": "csrrw", "write": "0x0000000000000008", "traps": 0,
                "expect": { "read": "0x0000000000001808" } },
  "negative": { "op": "csrrw", "write": "0x0000000000000000", "traps": 0,
                "expect": { "read": "0x0000000000001800" },
                "forbid": "0x0000000000001808" }
  ```

* **`warl_allowed`** — the field is WARL with a stated legal set. The comparator
  accepts a legal differing result and rejects the *adjacent illegal* one: the
  read must never equal the value the rule marks with `forbid`. Example —
  `mtvec.MODE`, whose legal set is `{0 Direct, 1 Vectored}` and whose reserved
  encodings `{2,3}` must not be read back:

  ```json
  "positive": { "op": "csrrw", "write": "0x0000000000000001", "traps": 0,
                "expect": { "read": "0x0000000000000001" } },   // legal differing result accepted
  "negative": { "op": "csrrw", "write": "0x0000000000000002", "traps": 0,
                "expect": { "read": "0x0000000000000000" },
                "forbid": "0x0000000000000002" }                // adjacent illegal value rejected
  ```

  A second rule pins the other reserved encoding (`0x3` → `0x0`, forbid `0x3`),
  and `medeleg`/`mideleg`/`mcounteren` are WARL whose only legal value is 0 in
  an M-only profile: a write is accepted and canonicalises.

* **`read_only`** — software can never change the field. The comparator requires
  the read-back to equal the field's fixed value after a write attempt and
  rejects the written value appearing. Example — `mstatus.MPP`, which is
  read-only 3 in an M-only profile:

  ```json
  "positive": { "op": "csrrw", "write": "0x0000000000000000", "traps": 0,
                "expect": { "read": "0x0000000000001800" } },
  "negative": { "op": "csrrw", "write": "0x0000000000001000", "traps": 0,
                "expect": { "read": "0x0000000000001800" },
                "forbid": "0x0000000000001000" }
  ```

* **`alias`** — two addresses read the same state. The comparator requires the
  alias read to track the canonical register across two different writes and
  rejects a result that does not. Example — `cycle`, the read-only shadow of
  `mcycle`:

  ```json
  "positive": { "op": "csrrw", "write": "0x0000000000100000", "traps": 0,
                "target": "0xc00", "write_target": "0xb00",
                "expect": { "range": ["0x0000000000100000", "0x0000000000101000"] } },
  "negative": { "op": "csrrw", "write": "0x0000000000200000", "traps": 0,
                "target": "0xc00", "write_target": "0xb00",
                "expect": { "range": ["0x0000000000200000", "0x0000000000201000"] },
                "forbid": "0x0000000000100000" }
  ```

  The canonical register is written and the alias is read; a shadow that read a
  constant (e.g. zero) fails the positive, and a shadow that ignored the second
  write fails the negative's `forbid`.

* **`permission`** — the access is permitted only when the address is implemented
  and its encoding allows the operation. The comparator requires an illegal
  access to raise the illegal-instruction exception and to leave the destination
  and state unchanged, and rejects an access accepted as zero. Example —
  `mvendorid.write_denied` (the address is read-only) and
  `unimplemented.read_0x7ff` (the address is not implemented at all):

  `pre_read` reads the register before the operation and requires the same value
  after it, so the denied write is proved to have left the state unchanged:

  ```json
  "positive": { "op": "csrrw", "write": "0xffffffffffffffff", "traps": 1,
                "expect": { "read": "0x0000000000000000" },
                "forbid": "0xffffffffffffffff", "pre_read": true },
  // unimplemented:
  "positive": { "op": "csrr", "traps": 1, "target": "0x7ff",
                "expect": { "canary": "0x00000000cafebabe" },
                "forbid": "0x0000000000000000" }
  ```

### The counters, independently

The card asks for counters "read/write/halt/permission" as separate rules, and
they are:

* **read** (`mcycle.read`, `minstret.read`) — the counter is running: the read
  is in an inclusive range and the reset value is forbidden;
* **write** (`mcycle.write`, `minstret.write`) — a write of `V` is read back in
  `[V, V+0x1000]` (the write supplies the value at that edge and the same edge's
  tick is added), and the *other* write's value is forbidden so a stuck counter
  fails;
* **halt** (`mcycle.halt`, `minstret.halt`) — after a write the counter keeps
  counting: two reads `64` instructions apart must differ by at least 32
  (positive) and at least 1 (negative), i.e. a write must not stop it;
* **permission** (`cycle.write_denied`, `instret.write_denied`,
  `time.write_denied`) — the read-only shadows deny the write and trap.

`time` is an alias of the platform's `mtime`: the harness drives `mtime_i =
0x1_0000_0000 + cycle`, so `time` must read the platform register (a shadow that
read zero fails the positive) and must never read the forbidden zero.

---

## 4. Full rule table

`w=` is the write operand (hex); `->` is the required read-back; `[trap xN]` is
the number of illegal-instruction exceptions the example must raise; `forbid` is
the adjacent illegal result.

| rule | kind | bits | positive | negative |
|---|---|---|---|---|
| `mstatus.SIE` | strict | 1 | csrrw w=2 -> 0x1802 | csrrw w=0 -> 0x1800 forbid 0x1802 |
| `mstatus.MIE` | strict | 3 | csrrw w=8 -> 0x1808 | csrrw w=0 -> 0x1800 forbid 0x1808 |
| `mstatus.SPIE` | strict | 5 | csrrw w=20 -> 0x1820 | csrrw w=0 -> 0x1800 forbid 0x1820 |
| `mstatus.MPIE` | strict | 7 | csrrw w=80 -> 0x1880 | csrrw w=0 -> 0x1800 forbid 0x1880 |
| `mstatus.VS` | strict | 10:9 | csrrw w=600 -> 0x1e00 | csrrw w=0 -> 0x1800 forbid 0x1e00 |
| `mstatus.FS` | strict | 14:13 | csrrw w=6000 -> 0x7800 | csrrw w=0 -> 0x1800 forbid 0x7800 |
| `mstatus.MPP` | read_only | 12:11 | csrrw w=0 -> 0x1800 | csrrw w=1000 -> 0x1800 forbid 0x1000 |
| `mstatus.SPP` | read_only | 8 | csrrw w=100 -> 0x1800 forbid 0x1900 | csrrw w=0 -> 0x1800 |
| `mstatus.MPRV_SUM_MXR_TVM_TW_TSR` | read_only | 22:17 | csrrw w=7e0000 -> 0x1800 forbid 0x7e1800 | csrrw w=0 -> 0x1800 |
| `mstatus.SD_XS_UXL_SXL` | read_only | 63,35:32,16:15 | csrrw w=8000000f00018000 -> 0x1800 forbid 0x8000000f00018000 | csrrw w=0 -> 0x1800 |
| `mstatus.WPRI` | read_only | 62:38,31:23,4,2,0 | csrrw w=7fffffc0ff800015 -> 0x1800 forbid 0x7fffffc0ff801815 | csrrw w=0 -> 0x1800 |
| `misa.value` | strict | 63:0 | csrrw w=0 -> 0x8000000000001100 | csrrw w=all-ones -> 0x8000000000001100 forbid all-ones |
| `medeleg.value` | warl_allowed | 9:0 | csrrw w=0 -> 0 | csrrw w=3ff -> 0 forbid 3ff |
| `mideleg.value` | warl_allowed | 1:0 | csrrw w=0 -> 0 | csrrw w=3ff -> 0 forbid 3ff |
| `mie.MTIE` | strict | 7 | csrrw w=80 -> 0x80 | csrrw w=0 -> 0 forbid 0x80 |
| `mie.MSIE` | strict | 3 | csrrw w=8 -> 0x8 | csrrw w=0 -> 0 forbid 0x8 |
| `mie.SSIE` | read_only | 1 | csrrw w=2 -> 0 forbid 2 | csrrw w=0 -> 0 |
| `mtvec.BASE` | strict | 63:2 | csrrw w=80000100 -> 0x80000100 | csrrw w=80000200 -> 0x80000200 forbid 0x80000100 |
| `mtvec.MODE` | warl_allowed | 1:0 | csrrw w=1 -> 1 | csrrw w=2 -> 0 forbid 2 |
| `mtvec.MODE.reserved3` | warl_allowed | 1:0 | csrrw w=0 -> 0 | csrrw w=3 -> 0 forbid 3 |
| `mcounteren.value` | warl_allowed | 2:0 | csrrw w=0 -> 0 | csrrw w=7 -> 0 forbid 7 |
| `mscratch.value` | strict | 63:0 | csrrw w=0123456789abcdef -> same | csrrw w=0 -> 0 forbid 0123456789abcdef |
| `mepc.BASE` | strict | 63:2 | csrrw w=80001234 -> 0x80001234 | csrrw w=80001235 -> 0x80001234 forbid 0x80001235 |
| `mepc.low` | read_only | 1:0 | csrrw w=3 -> 0 forbid 3 | csrrw w=0 -> 0 |
| `mcause.value` | strict | 63:0 | csrrw w=8000000000000007 -> same | csrrw w=0 -> 0 forbid 8000000000000007 |
| `mtval.value` | strict | 63:0 | csrrw w=deadbeef -> 0xdeadbeef | csrrw w=0 -> 0 forbid deadbeef |
| `mip.MSIP` | strict | 3 | csrrw w=8 -> 0x8 | csrrw w=0 -> 0 forbid 0x8 |
| `mip.MTIP` | strict | 7 | csrrw w=80 -> 0x80 | csrrw w=0 -> 0 forbid 0x80 |
| `mip.SSIP` | read_only | 1 | csrrw w=2 -> 0 forbid 2 | csrrw w=0 -> 0 |
| `mip.MEIP` | read_only | 11 | csrrw w=800 -> 0 forbid 800 | csrrw w=0 -> 0 |
| `mip.unimplemented_bits` | read_only | 63:16,15:12,10:8,6:4,2,0 | csrrw w=fffffffffffff775 -> 0 forbid same | csrrw w=0 -> 0 |
| `mvendorid.value` | read_only | 31:0 | csrr -> 0 forbid 1 | csrr -> 0 forbid 1 |
| `mvendorid.write_denied` | permission | 31:0 | csrrw w=all-ones -> 0 [trap x1, pre-read] forbid all-ones | csrrw w=1 -> 0 [trap x1, pre-read] |
| `marchid.value` | read_only | 63:0 | csrr -> 0 forbid 1 | csrr -> 0 forbid 1 |
| `marchid.write_denied` | permission | 63:0 | csrrw w=all-ones -> 0 [trap x1, pre-read] forbid all-ones | csrrw w=1 -> 0 [trap x1, pre-read] |
| `mimpid.value` | read_only | 63:0 | csrr -> 0 forbid 1 | csrr -> 0 forbid 1 |
| `mimpid.write_denied` | permission | 63:0 | csrrw w=all-ones -> 0 [trap x1, pre-read] forbid all-ones | csrrw w=1 -> 0 [trap x1, pre-read] |
| `mhartid.value` | read_only | 63:0 | csrr -> 0 forbid 1 | csrr -> 0 forbid 1 |
| `mhartid.write_denied` | permission | 63:0 | csrrw w=all-ones -> 0 [trap x1, pre-read] forbid all-ones | csrrw w=1 -> 0 [trap x1, pre-read] |
| `mcycle.read` | strict | 63:0 | csrr -> 1..0x1000000 | csrr -> 1..0x1000000 forbid 0 |
| `mcycle.write` | strict | 63:0 | csrrw w=1000 -> 0x1000..0x2000 | csrrw w=2000 -> 0x2000..0x3000 forbid 0x1000 |
| `mcycle.halt` | strict | 63:0 | csrrw w=100 -> advance>=32 gap 64 forbid 100 | csrrw w=100 -> advance>=1 gap 64 forbid 100 |
| `minstret.read` | strict | 63:0 | csrr -> 1..0x1000000 | csrr -> 1..0x1000000 forbid 0 |
| `minstret.write` | strict | 63:0 | csrrw w=1000 -> 0x1000..0x2000 | csrrw w=2000 -> 0x2000..0x3000 forbid 0x1000 |
| `minstret.halt` | strict | 63:0 | csrrw w=100 -> advance>=32 gap 64 forbid 100 | csrrw w=100 -> advance>=1 gap 64 forbid 100 |
| `cycle.alias` | alias | 63:0 | csrrw mcycle=100000 -> cycle 0x100000..0x101000 | csrrw mcycle=200000 -> cycle 0x200000..0x201000 forbid 0x100000 |
| `instret.alias` | alias | 63:0 | csrrw minstret=300000 -> instret 0x300000..0x301000 | csrrw minstret=400000 -> instret 0x400000..0x401000 forbid 0x300000 |
| `time.alias` | alias | 63:0 | csrr -> 0x100000000..0x100100000 | csrr -> 0x100000000..0x100100000 forbid 0 |
| `cycle.write_denied` | permission | 63:0 | csrrw w=ffffffff -> 1..0x1000000 [trap x1] forbid ffffffff | csrrw w=1 -> 1..0x1000000 [trap x1] |
| `instret.write_denied` | permission | 63:0 | csrrw w=ffffffff -> 1..0x1000000 [trap x1] forbid ffffffff | csrrw w=1 -> 1..0x1000000 [trap x1] |
| `time.write_denied` | permission | 63:0 | csrrw w=ffffffff -> mtime range [trap x1] forbid ffffffff | csrrw w=1 -> mtime range [trap x1] |
| `unimplemented.read_0x7ff` | permission | 11:0 | csrr 0x7ff -> canary [trap x1] forbid 0 | csrr 0x000 -> canary [trap x1] forbid 0 |
| `unimplemented.read_0xb03` | permission | 11:0 | csrr 0xb03 -> canary [trap x1] forbid 0 | csrr 0xb03 -> canary [trap x1] forbid 0 |
| `unimplemented.write_0x7ff` | permission | 11:0 | csrrw 0x7ff -> canary [trap x2] forbid 0 | csrrw 0xb03 -> canary [trap x2] forbid 0 |

---

## 5. Negative controls

`python3 tools/run_csrrules_controls.py` rebuilds the case with exactly one
defect, from an empty build directory, with its `-D` in that build's own command
line (`build_command.txt` next to each binary). Every mutant has a SHA-256
different from the shipping binary's
`66e1f5305e54d35d10959b005c65f703f5e2ec954b1c232e436972324fbfb86a`, exits 1, and
names the check it breaks as the first failure.

The card's three fail modes are properties of the *checker*: the ledger, not the
RTL, is the one place that says what this case expects, so no RTL `-D` can
express "this difference was waived" or "this unimplemented CSR is accepted as
zero". They are therefore `-CFLAGS -D…` mutations of the case's own comparison
and are labelled **DRIVER** below. Three further controls reuse the CSR file's
own documented RTL mutants (`rtl/core/mosaic_csr.sv`) to show the same ledger
catches a real machine defect.

| # | define | kind | fail mode / defect | exit | first failure |
|---|---|---|---|---|---|
| 1 | `MOSAIC_CSRRULES_MUTANT_SKIP_RULE` | DRIVER | **a rule silently skipped** — the driver drops one rule's stimulus while the ledger still requires it | 1 | `coverage: rule mstatus.SIE was never visited by its positive example` |
| 2 | `MOSAIC_CSRRULES_MUTANT_AUTO_WAIVER` | DRIVER | **a waiver generated from a difference** — the comparator accepts the adjacent illegal value `forbid` marks; the machine's correct canonicalisation then fails | 1 | `medeleg.value negative: the read-back: read 0x0, want 0x3ff` |
| 3 | `MOSAIC_CSRRULES_MUTANT_UNIMPL_ZERO` | DRIVER | **an unimplemented CSR accepted as zero** — the comparator expects a read of `0x7ff` to return zero without trapping | 1 | `unimplemented.read_0x7ff positive: the read's illegal-instruction trap: expected 0 trap, observed 1` |
| 4 | `MOSAIC_CSR_MUTANT_CYCLE_WRITABLE` | RTL | a read-only counter shadow becomes a writable alias | 1 | `every expected illegal access raised the illegal-instruction exception: traps=18 expected=22` |
| 5 | `MOSAIC_CSR_MUTANT_RO_WRITE_ACCEPTED` | RTL | every implemented CSR is treated as writable | 1 | `every expected illegal access raised the illegal-instruction exception: traps=8 expected=22` |
| 6 | `MOSAIC_CSR_MUTANT_NO_FIELD_MASK` | RTL | the `mstatus` write no longer applies the generated field mask | 1 | `mstatus.SIE positive: the read-back: read 0x2, want 0x1802` |

Mutant 6 is the most direct evidence that the "read-only bits cannot be changed"
rule bites: without the field mask, `csrrw mstatus, 0x2` replaces the whole
register with `0x2` and loses `MPP=3` (reset `0x1800`), so the very first
`mstatus` rule fails. Mutants 4 and 5 show the trap requirement is not
vacuous — 4 and 14 of the 22 required traps disappear.

SHA-256 of each mutant binary (shipping `147ea3a6…c4a5`):

```
MOSAIC_CSRRULES_MUTANT_SKIP_RULE      44fd9994de4e8c8257cec6a0fb873c33954580cf38af5733045b78160f8cc9f8
MOSAIC_CSRRULES_MUTANT_AUTO_WAIVER    b17c716af949b1feebad7ea04da1ff58619bfd132fc805c49c68666612ea15a1
MOSAIC_CSRRULES_MUTANT_UNIMPL_ZERO    8b44c7d3c88fcdee8c095f686290eab5156df50b4f9ecf9575c676d7a80f27a4
MOSAIC_CSR_MUTANT_CYCLE_WRITABLE      790496ad4433bf30d6f859de5df06d1755af5f355a4fb09dec0cca10cdfdd798
MOSAIC_CSR_MUTANT_RO_WRITE_ACCEPTED   89cd0acfa73f68217056e03a3ef7f1174c218316cb76db75c5ebafae3676c5aa
MOSAIC_CSR_MUTANT_NO_FIELD_MASK       4e6105283754ca245ac54c1221fbe0a39d101fce72f4b3f6a5a23c5ec3a49db3
```

---

## 6. RTL revision compiled

SHA-256 (first 16 hex digits) of the sources the registry lists for the case,
plus the configuration the ledger is checked against:

```
45c8d75196e48a32  rtl/core/mosaic_pkg.sv            2a4c5c422e6bbeeb  rtl/core/mosaic_core.sv
9fcc53339f6d276e  rtl/core/mosaic_uop_pkg.sv        3c7874610f0172e4  rtl/core/mosaic_csr.sv
0a92b98eb4c835dc  rtl/core/mosaic_alu.sv            801a1f9b274d1c08  rtl/core/mosaic_interrupt.sv
cf996100038627df  rtl/core/mosaic_branch_cmp.sv     f28610e78f4db7e7  sim/tb/mosaic_core_tb.sv
73c4168202d7af9f  rtl/core/mosaic_branch_target.sv  5d65f69fd45db50a  sim/unit/tb_core_csr_rules.cpp
bad709b88af95cbc  rtl/core/mosaic_predictor.sv      38c1027f9e48535d  config/csr/rule_ledger.json
c1592a67872989c9  rtl/core/mosaic_fetch.sv          2d7f2b054815dadf  config/csr/mode_m.json
a90500e438dfebc4  rtl/core/mosaic_decoder.sv        e0a2304e281a6a92  config/schema/csr_rules.schema.json
95226d0c45dbebf3  rtl/core/mosaic_rename.sv         e92ff3c347a80014  tools/gen_manifest.py
afba8cf6f4f954d1  rtl/core/mosaic_rob.sv            3a52189d6db0b487  build/p0/sim/mosaic_csr_rules.h
447bfabdc3dc5b8c  rtl/core/mosaic_iq.sv
2e695d234e70a623  rtl/core/mosaic_prf.sv
0f6f3ab7a3425ffb  rtl/core/mosaic_wb_arbiter.sv
a4ad9fe88307c7fc  rtl/core/mosaic_macro_desc.sv
533938f60a4717e9  rtl/core/mosaic_dispatch.sv
4ab36113a219b763  rtl/core/mosaic_cluster.sv
658aeb6b7e35bc43  rtl/core/mosaic_redirect_arb.sv
dd12e9ba3df57959  rtl/core/mosaic_muldiv.sv
e2c69c1892925b3d  rtl/core/mosaic_retire.sv
```

The RTL is unchanged by this package; `rtl/core/mosaic_csr.sv` was read, not
edited, so the I-044 lane's concurrent work there is untouched.

---

## 7. Gates re-run

| gate | result |
|---|---|
| `python3 tools/run_unit.py --profile p0 --case csr.rule_ledger` | **PASS** (54 rules, 108 examples, 6407 comparisons, 22 traps) |
| `python3 tools/run_unit.py --profile p0 --case csr.precise_trap_mret` | **PASS** (I-019) |
| `python3 tools/run_unit.py --profile p0 --case core.trap_csr_program` | **PASS** (I-023) |
| `python3 tools/run_unit.py --profile p0 --case trap.precise_state` | **PASS** (V-014) |
| `python3 tools/run_unit.py --profile p0 --case core.corpus_sweep` | **PASS** (I-023) |
| `python3 tools/run_unit.py --profile p0 --case irq.replay_timeline` | **PASS** (V-015) |
| `python3 tools/lint_rtl.py --profile p0` | clean, 43 source files; self-test rejects a latching module |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl <rtl>` | exit 0, all checks PASS |
| `python3 tools/check_records.py` | green: 51 delivered packages, 60 registered cases |
| `python3 tools/check_profile.py --all` | green, p0..p3 (the ledger is validated against p0) |
| `make check` | exit 0 |
| `make lint-cpp` | 57 files clean (the driver is `-Wall -Wextra -Wshadow` clean) |
| `python3 tools/run_csrrules_controls.py` | all 6 mutants differ, exit 1, name the check |

---

## 8. Not covered — honestly

* **Which CSRs are unimplemented in p0.** p0 is M-only
  (`config/profiles/p0.json` privilege_modes `["M"]`); the 21 CSRs above are the
  whole implementation table. Unimplemented are every S/U register (`sstatus`,
  `sie`, `sip`, `stvec`, `sscratch`, `sepc`, `scause`, `stval`, `satp`,
  `senvcfg`, `scounteren`), every `hpmcounter*`/`hpmcounter*h` and `*h`
  RV32-only counter, `mstatush` (RV32-only), `mconfigptr`, `menvcfg`/`mseccfg`,
  and the whole U CSR space (`0x000–0x0FF`, and `0xC00–0xCFF` outside
  `cycle`/`time`/`instret`). The ledger's `unimplemented.*` rules check three
  representative addresses (`0x7FF`, `0xB03`, `0x000`) trap and are not read as
  zero; the case does **not** enumerate every unimplemented address — that is
  the CSR decode's job, and `config_check`'s `_check_csr_rules` covers the other
  direction (every *implemented* CSR must carry a rule).

* **Permission errors in the wrong mode.** p0 implements no S or U mode, so
  "an M-mode CSR accessed from S/U" is unreachable — there is no wrong mode to
  be in. What *is* reachable and checked: (a) at runtime, the write-permission
  rules (a write to a read-only address raises illegal instruction and leaves the
  state unchanged — `mvendorid`…`mhartid`, `cycle`/`instret`/`time`); and
  (b) structurally, in `config_check._check_csr_rules`, the address-encoded
  permission of every implemented CSR (`csr[9:8]` is the minimum privilege,
  `csr[11:10] == 3` forces M for a write) is required to be at or below the
  least mode the profile implements. When the S/U profiles land, the runtime
  mode check becomes reachable and a `permission` rule must be added to the
  ledger for it.

* **The `mstatus.FS`/`VS` early-dirty rules.** p0 implements neither F/D nor V
  (`config/profiles/p0.json` extensions are `I, M, Zicsr, Zifencei, Zicntr,
  Zihpm`; the FPU I-049 built is selected by a later profile, not p0), so there
  is no floating-point or vector state for `FS`/`VS` to describe and the
  early-dirty transition has nothing to control. What was checked instead: the
  WARL *encoding* of both fields (write `0x600` → read `0x1e00` for VS, write
  `0x6000` → read `0x7800` for FS; the opposite pattern must not appear), and
  the ledger records the precondition explicitly. When I-050 lands and a profile
  implements FP state, the "an FP instruction may set FS to Dirty" rule becomes
  exercisable and must be added to the ledger — it cannot be added before then,
  because the coverage assertion would fail a rule no stimulus reaches.

* **`mip.MEIP`.** p0 has no PLIC, and the CSR file reads `mip & 0x88`, so bit 11
  always reads zero; the platform `irq_ext_i` is never asserted. The rule checks
  it is read-only zero. A profile that routes external interrupts into `mip`
  needs a rule for the platform-driven pending bit.

* **Counter values are ranges, not exact values.** `mcycle`/`minstret` are
  free-running, so a read-back is only pinned to an inclusive range plus a
  forbidden adjacent value (the reset value, or the other write's value). The
  counter *tick* is not pinned cycle-by-cycle here — the exact per-cycle
  arithmetic is I-019's `csr.precise_trap_mret`; this case pins the relationship
  ("a write does not halt it", "the write's value is the base of the range").

* **`mtvec` vectored entry arithmetic.** The case pins `mtvec.MODE`'s encoding
  (legal `{0,1}`, reserved `{2,3}` canonicalised to 0). The *use* of a vectored
  mode to compute a trap target is I-020/V-014's, not re-derived here.

* **`misa` WARL extension bits.** p0's `misa` has a write mask of 0, so the rule
  is "strict: the value is determined". The WARL behaviour of writing an
  unimplemented extension bit is exercised only through the all-ones negative;
  a profile that makes `misa` writable needs a `warl_allowed` rule with the
  implemented-extension set as its legal set.
