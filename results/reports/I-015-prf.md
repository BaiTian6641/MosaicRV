# I-015 — banked physical register file (PRF)

Work package I-015, card `### I-015` in `docs/implementation-plan.md` (§4-§10).
Case `CASE=prf.read_bank_collision`.

Files delivered:

| File | Role |
|---|---|
| `rtl/core/mosaic_prf.sv` | the register file: MOSAIC_INT_PRF_ENTRIES x MOSAIC_XLEN storage in MOSAIC_PRF_BANKS banks, one write port per bank, one read demand per bank with a zero-latency combinational response, validity/generation machinery, status counters |
| `sim/tb/mosaic_prf_tb.sv` | simulation wrapper: pass-through ports, whole-storage observation, geometry read-back |
| `sim/unit/tb_prf.cpp` | C++ driver + independent shadow model + nine phases |
| `results/reports/I-015-prf.md` | this file |

Nothing outside that list was edited. In particular `tests/unit/registry.json`
(the case was already registered), `config/status/implementation_status.json`,
`results/PROGRESS.md`, `tools/gen_manifest.py`, `rtl/core/filelist.f` and other
packages' files were not touched; §9 lists what I would change there instead.

---

## 1. The interface, and its cycle semantics

`rst` is synchronous and active high. Everything else in the read path is
**combinational**: a demand offered in cycle N is answered in cycle N. The only
registered state is the storage, the validity bits and the five counters.

```
rd_valid_i[b], rd_tag_i[b], rd_gen_i[b]
  -> rd_ready_o[b], rsp_valid_o[b], rsp_tag_o[b], rsp_gen_o[b], rsp_data_o[b],
     rsp_gen_mismatch_o[b], rsp_never_written_o[b]
```

| Signal | Meaning |
|---|---|
| `rd_ready_o[b]` | **the grant for demand slot `b` in this cycle.** The demand is taken and its response is on `rsp_*` in the same cycle. |
| `rsp_valid_o[b]` | equal to `rd_ready_o[b]`. A refused demand produces no response at all. |
| `rsp_tag_o[b]` | always the tag the demand named, granted or not. |
| `rsp_gen_o[b]` | the **stored** generation of the entry (or zero when the entry has no value). |
| `rsp_gen_mismatch_o[b]` | the entry holds a value and its stored generation is not the requested one. |
| `rsp_never_written_o[b]` | the entry has held no admitted value since reset (or the tag has no home entry). |

Three decisions the frozen interface does not fix, and that this implementation
therefore *pins* (a consumer written against a different choice would be wrong,
so the choice is stated rather than implied):

1. **Zero-latency read, no response register.** The port comment says a demand
   "is taken only where its bank's single read port is free this cycle", and the
   card says "the consumer re-offers the refused one". With a zero-latency port
   the response slot *is* the demand's own slot and is free exactly when the
   demand is granted, so `rd_ready_o[b]` is a grant, not a "can accept next
   cycle" hint. A design with a one-cycle response register would fail this
   test; §10 says so.
2. **One demand per bank per cycle, lowest-numbered slot wins.** The priority is
   fixed index order, not round-robin and not age: fairness between consumers is
   the operand collector's job (I-026/I-027), and a register file that invented
   its own would be wrong twice.
3. **A refused demand leaves no trace.** No queue, no retry state, no reorder
   buffer inside the PRF. The collector re-offers; the PRF answers each offer
   from scratch. This is what keeps the collection rules in one place.

## 2. Geometry and the bank decode

`MOSAIC_INT_PRF_ENTRIES = 96` over `MOSAIC_PRF_BANKS = 4`:

```
bank = tag % MOSAIC_PRF_BANKS        home bank, 0..BANKS-1
row  = tag / MOSAIC_PRF_BANKS        row inside that bank
rows = ceil(ENTRIES / BANKS)         storage depth per bank (24)
```

This is a real modulo, not a field split, for the reason the header gives at
length: the two agree only when the bank count is a power of two *and* the entry
count is a multiple of the bank count squared, and the geometry file explicitly
says the bank count need not divide the entry count evenly. Rows are `ceil`, so
a profile whose entry count does not divide evenly leaves the surplus rows of the
last bank unreachable rather than aliasing them onto real entries; the
reachability rule is `tag < ENTRIES`, and an out-of-range tag is refused (a write
is dropped, a read is answered `rsp_never_written_o`) rather than wrapped —
wrapping an out-of-range index onto a valid one manufactures an alias onto
somebody else's register.

**What p0 can and cannot distinguish.** With `BANKS = 4 = 2²` the *bank*
selection `tag % 4` is the same function as the low two bits of the tag, so no
test can tell a modulo from a slice there — §10 records that as unverified. What
p0 *can* distinguish is the **row** decode, and the test does: the
`SLICE_ROW` mutant takes the row from `tag`'s low bits and is caught on the first
write above tag 3 (§8). The entry count (96) is not a power of two, so the
`ceil`/quotient arithmetic is exercised on a real non-power-of-two geometry:
tag 95 lands in bank 3 row 23, and the shadow's independent `tag / banks`
arithmetic agrees with the hardware on every tag in the random campaign.

### File-scope width names, and the one lint collision integration produced

`mosaic_prf.sv` declares its eight width constants **at file scope** (a module's
ANSI port list cannot see declarations inside its own body), which puts them in
the compilation-unit scope shared by every file elaborated with it. They are
therefore named `MPRF_*`, not `PRF_*`: `mosaic_uop_pkg.sv` declares its own
`PRF_ENTRIES` for the same register file in package scope, and when the first
module that both instantiates `mosaic_prf` and mentions that package was
elaborated (`rtl/core/mosaic_core.sv`, I-023), Verilator reported the package's
declaration as hiding the file-scope one:

```
%Warning-VARHIDDEN: rtl/core/mosaic_uop_pkg.sv:105:27: Declaration of signal hides
declaration in upper scope: 'PRF_ENTRIES'
  rtl/core/mosaic_prf.sv:188:25: ... Location of original declaration
```

`VARHIDDEN` is an error under the project's `-Wall` lint gate, and the warning is
emitted while the *package* is processed, so a `lint_off` in either file does not
reach it. The prefix is the fix: the two scopes no longer meet. Verified after the
rename — `rtl/core/mosaic_core.sv: clean as mosaic_core` in the project lint, and
a scoped `verilator --lint-only -Wall --top-module mosaic_core` of that file exits
0 with zero warnings. Behaviour is unchanged: the base case, its comparison
counts and all six mutant failure points are identical before and after the
rename (§8).

## 3. The rules the card is about

### Write-through (§ "Same-cycle write and read to the same bank")

A read of an entry that a write port writes in the same cycle returns the **new**
value and the **new** generation, and its generation comparison is against the
new generation. The comparison is against the same *entry* (`row_of(wr_tag) ==
row_of(rd_tag)` within the same bank), not merely the same bank: a write to
another row of the same bank must not leak into the response. Both halves are
directed tests (§7, phases `write-through`), and both are covered by mutants
(`NO_WRITE_THROUGH`, `SLICE_ROW`).

### Never-written (§ "仅 simulation 读出初始化零掩盖未定义读")

Validity is a separate reset bit per entry, outside the storage arrays, because
generation 0 is a *real* generation and therefore cannot express "no value". A
read of an entry whose bit is clear raises `rsp_never_written_o`, returns zero
data and zero generation (**deterministic values, not storage contents**), raises
no generation mismatch, and increments `o_invalid_ctr`. That is a check, not a
comment: it is asserted for every one of the 96 entries right after reset (phase
`reset-state`), again in the `never-written` phase, and the
`NEVER_WRITTEN_VALID` mutant fails on the first comparison of the whole run.

### Generation mismatch

A read naming generation g of an entry holding generation h ≠ g raises
`rsp_gen_mismatch_o` and reports **h** on `rsp_gen_o`, so the consumer compares
rather than trusts (and may report the difference). A mismatched read of an entry
that *has* a value raises no `rsp_never_written_o` and does not move
`o_invalid_ctr`; generation 0 is treated as a real generation. Phase
`generation`; mutant `IGNORE_STORED_GEN`.

### Write admission

A write is admitted exactly when `wr_en_i[b] && wr_gen_valid_i[b] && the tag's
home bank == b && tag < ENTRIES`. Each condition has a purpose:

* `wr_gen_valid_i[b]` is the producer's statement that the identity it delivers
  has a **live** generation (`mosaic_rename.sv`'s `gen_valid`). A writeback that
  does not carry one is refused rather than stored, so a producer cannot
  manufacture validity out of a generation number that was never assigned; the
  entry stays unwritten and a read of it reports `rsp_never_written_o` instead of
  a plausible zero.
* the home-bank check refuses a **mis-routed** write instead of letting it land
  in another bank's row space, where a later read for the tag it claimed would
  find unrelated data.
* the range check refuses a tag with no home entry.

A refused write changes nothing and is not counted; `o_wr_ctr` counts admitted
writes. Phase `write-admission`, which ends with a correctly routed write that
*is* admitted and readable, so the refusals are not "writes never work".

## 4. State, reset and counters

| State | Reset? | Why |
|---|---|---|
| `data_q[BANKS][ROWS]`, `gen_q[BANKS][ROWS]` | **no** | reset cost is control state, not DEPTH x WIDTH of storage (`rtl/common/mosaic_ram.sv`, `mosaic_rename.sv`) |
| `valid_q[BANKS]` | yes, to 0 | validity *is* control state, and it is what keeps undefined storage out of a response |
| five 32-bit counters | yes, to 0 | status only |

`o_wr_ctr` admitted writes, `o_rd_ctr` granted demands, `o_conflict_ctr` demands
refused because another slot took their bank in the same cycle (with a
zero-latency port that is the only possible refusal reason), `o_mismatch_ctr`
responses whose stored generation did not match, `o_invalid_ctr` responses for
entries with no value since reset. `o_busy` is high in a cycle in which some
offered demand was refused — this register file's contribution to back-pressure;
the collector is what remembers the demand.

## 5. What this module deliberately does not own

The operand collector: retrying a refused demand, collecting operands across
cycles, arbitrating between consumers (I-026/I-027). There is no queue, no
collection state and no fairness policy in `mosaic_prf.sv`. The only thing here
that resembles collection is the *test's* retry queue, and that lives in the
driver, where it is checked against the shadow's model of the same arbitration.

## 6. The test: shadow, phases, invariants

The DUT is never its own oracle. `ShadowPrf` holds per-bank rows of
`{valid, gen, data}`, its own counters, and derives the decode, the arbitration,
the write-through rule, the admission rule and the never-written rule from the
contract prose. It is written differently on purpose: storage as a vector of
bank vectors (a bank/row swap cannot cancel), arbitration as a per-bank "taken"
flag walked in slot order (no shared priority encoder).

Geometry is read from the elaborated DUT (`o_entries`, `o_banks`, `o_rows`,
`o_bank_w`, `o_row_w`, ...), and the first checks after reset assert the numbers
are coherent: `rows == ceil(entries/banks)`, the decode widths equal `$clog2` of
the counts, `banks <= entries`, and the tag field can name every entry. The
driver contains no depth, no bank count and no tag width.

Phases (each resets first, so the first failure names one mechanism):

| # | Phase | What it pins down |
|---|---|---|
| 1 | `reset-state` | counters zero, all validity bits clear, a read of all 96 entries reports never-written, no conflicts/mismatches |
| 2 | `bank-collision` | two demands to one bank in one cycle → exactly one grant (slot 0), the other refused with no response; demands to other banks granted in the same cycle; `o_busy` and `o_conflict_ctr` move; the refused demand is served when re-offered; a three-way conflict is the same story with two refusals recovered |
| 3 | `write-through` | preload, read-back, same-cycle write+read returns new value/new generation (and not the old value), the value reaches storage next cycle, a write to another row of the same bank does not leak, a write with no live generation neither bypasses nor is admitted; the read is offered in a slot that is *not* the bank index |
| 4 | `generation` | matching read clean; `g-1` → mismatch with the stored generation reported; generation 0 of a written entry is not "unwritten"; the three counters move by exactly one where they should |
| 5 | `never-written` | unwritten entry reports the flag, no mismatch, zero data/generation, `o_invalid_ctr` and `o_rd_ctr` move; a tag with no home entry (96..127 exist in the 7-bit field) is answered, not wrapped |
| 6 | `write-admission` | mis-routed, dead-generation and out-of-range writes all refused with `o_wr_ctr` unmoved; a correct write is admitted and readable |
| 7 | `reset-validity` | writes are readable, then reset makes every one of them never-written again and clears all validity bits |
| 8 | `random` | 4000 cycles of random writes (with dead generations, mis-routing, out-of-range tags) and demanded reads retried out of a queue, compared against the shadow every cycle; coverage guards require the campaign to have actually conflicted, mismatched, read unwritten entries and written through |
| 9 | `determinism` | the same seeded programme twice, hash-identical, both drained |

Standing invariants, checked on every compared cycle of every phase (from the
DUT's own outputs, never from the shadow's prediction):

* no two granted demands in one cycle target the same bank — this is the card's
  per-cycle property, asserted from the hardware's grants;
* `rsp_valid_o[b] == rd_ready_o[b]`: a refusal carries no response, a grant
  carries one;
* `o_busy` is high exactly when some offered demand was refused;
* granted + refused == offered, so no demand is invented and none disappears;
* a never-written response carries no data and no mismatch;
* the validity bits match the shadow for **all** 96 entries; the generation and
  data words match wherever the validity bit is set. The words of an *invalid*
  entry are deliberately not compared: the arrays are not reset, so their
  contents are undefined until first written.

**Two-snapshot discipline.** Grants, responses and `o_busy` are read *before* the
clock edge (that is what the DUT presented in the cycle under test); the counters
and the storage are read *after* the edge (that is when the registers are true).
The driver's retry queue is updated from the pre-edge grants for the same reason.

## 7. Commands run, with output

All commands from `/Users/flare/MosaicRV`, profile `p0`, Verilator 5.052.

### 7.1 The registered case

```
$ python3 tools/run_unit.py --profile p0 --case prf.read_bank_collision
PASS prf.read_bank_collision      task=I-015

$ make unit CASE=prf.read_bank_collision PROFILE=p0
PASS prf.read_bank_collision      task=I-015
```

Verbatim result line (`--seed 1`, the registry's seed):

```
RESULT PASS prf.read_bank_collision prf contract holds: 5263 shadow comparisons over 5311 cycles,
96 entries / 4 banks of 24 rows, seed 1; 822 writes, 1309 granted reads, 568 conflicts,
1101 mismatches, 205 never-written reads
```

10 `Reporter` checks (nine phases + the final "no contract violation in any
phase"), 0 failures, exit 0.

### 7.2 Lint

```
$ python3 tools/lint_rtl.py --profile p0
ok   rtl/core/mosaic_prf.sv: clean as mosaic_prf
lint: 31 source file(s) clean                       # exit 0

$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_prf.sv
# exit 0, 0 errors. mosaic_prf.sv contributes 4 x STYLE-13 (unnamed always_comb)
# and 6 x STYLE-2 (ports named "o_wr_ctr"... which the frozen interface fixes, so
# there is no "_o"-suffix convention to follow for these status outputs).
# Scoped to this module because the whole-tree invocation does not complete
# today: it stops on a sibling's in-flight file, rtl/core/mosaic_dispatch.sv
# ("identifier 'head_fire' used before its declaration", six such errors). An
# earlier whole-tree run, with that file absent, exited 0 and reported the style
# counts 100 STYLE-13 / 701 STYLE-2 / 10 STYLE-16 / 1 STYLE-6 / 1 STYLE-7 across
# the tree -- the same classes, from other modules, none an error.
```

```
$ g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -Ibuild/p0/sim -Isim/common \
      -I$(verilator --getenv VERILATOR_ROOT)/include \
      -Ibuild/p0/unit/prf.read_bank_collision/obj_dir sim/unit/tb_prf.cpp
# exit 0; the only warnings name Verilator's own headers (verilated_types.h,
# verilated_funcs.h), none name tb_prf.cpp. This is the same command
# `make lint-cpp` runs for every file, scoped to mine because the project-wide
# target currently stops earlier on a sibling's in-flight file (§9.4).
```

The wrapper `sim/tb/mosaic_prf_tb.sv` is not linted by either RTL gate (they walk
`rtl/`); it is compiled by the case build above.

Integration check for the scope collision of §2 (the first module that both
instantiates this file and mentions `mosaic_uop_pkg`):

```
$ grep -E "mosaic_core" /tmp/lint_after_rename.log
ok   rtl/core/mosaic_core.sv: clean as mosaic_core

$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_core \
      -Ibuild/p0/rtl -Irtl/common -Irtl/core rtl/core/mosaic_core.sv
# exit 0, 0 warnings
```

### 7.3 Not seed-1-fragile

```
$ for s in 1 2 3 7 12345; do
    build/p0/unit/prf.read_bank_collision/prf.read_bank_collision \
      --case prf.read_bank_collision --out /tmp/prf-seed-$s --seed $s --max-cycles 200000; done
seed 1      RESULT PASS ... 5263 shadow comparisons over 5311 cycles ...
seed 2      RESULT PASS ... 5262 shadow comparisons over 5310 cycles ...
seed 3      RESULT PASS ... 5265 shadow comparisons over 5313 cycles ...
seed 7      RESULT PASS ... 5269 shadow comparisons over 5317 cycles ...
seed 12345  RESULT PASS ... 5263 shadow comparisons over 5311 cycles ...
```

Determinism: the same seed run twice produces byte-identical `RESULT` lines
(`diff` clean), which is also checked internally by phase 9 (a rolling hash over
every response, `o_busy` and all five counters of every cycle of the programme,
run twice from a fresh reset).

## 8. Mutation testing

Recipe (the card's, via the project's own runner — no test is edited, no check is
loosened; each mutant is a `define` appended to the runner's flags, and the base
is rebuilt the same way with no define extra):

```python
import sys; sys.path.insert(0, 'tools'); import run_unit
run_unit.VERILATOR_FLAGS.append('-DMOSAIC_PRF_MUTANT_<name>')
run_unit.build_case('p0', CASE, run_unit.load_registry()['cases'][CASE])
# then: build/p0/unit/<CASE>/<CASE> --case <CASE> --out /tmp/mut --seed 1 --max-cycles 200000
```

Every `ifdef` body exists in `rtl/core/mosaic_prf.sv` (line numbers below), so
no mutant silently compiles the shipping build.

| Mutant | Line | Defect injected | Verdict | First mismatch | Comparisons before failure (base: 5263, 0 failures) |
|---|---|---|---|---|---|
| `NO_BANK_CONFLICT` | 381 | a bank conflict is ignored: two slots are granted to one bank in one cycle | FAIL exit 1 | `bank-collision: cycle 37: slot 1: rd_ready expected 0, got 1 [rd=s0:(t0,g0) s1:(t4,g0) s2:(t1,g0) s3:(t2,g0)]` | 25 |
| `DROP_BANK0_WRITE` | 336 | bank 0's write port never applies | FAIL exit 1 | `write-through: cycle 46: o_wr_ctr expected 1, got 0` | 30 |
| `IGNORE_STORED_GEN` | 406, 416 | a read echoes the requested generation and never reports a mismatch (data still stored data) | FAIL exit 1 | `generation: cycle 58: slot 0: rsp_gen_mismatch expected 1, got 0 [rd=s0:(t1,g6)]` | 38 |
| `NEVER_WRITTEN_VALID` | 432 | an entry with no value since reset is reported as an ordinary valid read of zero | FAIL exit 1 | `reset-state: cycle 9: slot 0: rsp_never_written expected 1, got 0` | 1 |
| `NO_WRITE_THROUGH` | 394 | a same-cycle write to the read entry is not visible to the read | FAIL exit 1 | `write-through: cycle 48: slot 1: rsp_gen_mismatch expected 0, got 1 [wr=b0:(t0,g4,v)=0xfedcba9876543210 rd=s1:(t0,g4)]` | 32 |
| `SLICE_ROW` | 308 | the row is `tag`'s low bits instead of `tag/BANKS` (masked into range so it stays a wrong-entry defect, not an out-of-bounds read) | FAIL exit 1 | `write-through: cycle 50: the validity bit of bank 0 row 1 expected 1, got 0` | 34 |

Delta statement: the unmodified base completes all 5263 comparisons with 0
failures and exit 0; each mutant above exits 1 with exactly 1 failure and dies at
the listed comparison, i.e. on the specific behaviour it broke. Note that
`DROP_BANK0_WRITE` and `SLICE_ROW` are caught by the *stored-state* comparison
(validity/generation/data of a row nothing ever read), not only by the response
path — which is the reason the wrapper exposes the storage at all.

## 9. Findings outside this package's ownership (reported, not edited)

1. **`rtl/core/filelist.f` does not list `mosaic_prf.sv`.** It also lacks
   `mosaic_rob.sv` and the other post-I-016 modules, so this is the existing gap
   the I-016 report already recorded, not a new one. I did not edit it: it is a
   shared ordered list and other lanes are landing files into it concurrently.
   Whoever owns it should add `mosaic_prf.sv` after `mosaic_iq.sv`.
2. **The generated identity package is not Yosys-parsable.** `mosaic_prf.sv` is
   the first RTL file to include `build/p0/rtl/mosaic_id_pkg.svh`, and Yosys 0.69
   rejects its `return`-style functions:
   ```
   $ yosys -q -p "read_verilog -sv -Ibuild/p0/rtl -Irtl/common -Irtl/core rtl/core/mosaic_prf.sv; hierarchy -check -top mosaic_prf"
   build/p0/rtl/mosaic_id_pkg.svh:78: ERROR: syntax error, unexpected OP_LAND, expecting ';'
   ```
   Line 78 is `macro_id_eq`'s `return (a.hart == b.hart) && (a.rob_gen == b.rob_gen) && ...`.
   Minimal probes (`/tmp/ysprobe`): `return (a.x == b.x) && (a.y == b.y);` fails,
   `return ((a.x == b.x) && (a.y == b.y));` parses, `fn = a && b;` parses. So
   wrapping the whole expression in parentheses (or using the assignment form
   `seq_older` already uses) fixes it in `tools/gen_manifest.py`. It is **latent
   today**: `make synth-generic` is already `BLOCKED` earlier, on
   `mosaic_decoder.sv:240`'s assignment patterns, so `read_verilog` never reaches
   the id package and the gate still exits 0. It will surface the moment the
   assignment-pattern blocker is lifted.
3. **A transient generator defect blocked every Verilator build for ~1 minute.**
   Mid-run, the working-tree edit adding include guards to `mosaic_id_pkg.svh`
   emitted a comment line starting with `Verilator`, which Verilator lexes as a
   metacomment: `%Error-BADVLTPRAGMA: mosaic_id_pkg.svh:8:1: Unknown verilator
   comment`. It affected every consumer of that header (`mosaic_muldiv.sv`,
   `mosaic_result_fifo.sv`, `mosaic_uop_pkg.sv`, this module). Reported to Main;
   the generator owner reworded the line and the official gates now pass with no
   workaround. Kept here because the same wording exists in the `cfg` package
   where it is harmless only because the word is mid-line.
4. **`make lint-cpp` currently fails on `sim/unit/tb_muldiv.cpp`** with
   `fatal error: 'Vmosaic_muldiv_tb.h' file not found` — a sibling's case whose
   generated header is not built yet, not this package's file. My file's compile
   is verified scoped in §7.2.
5. **The whole-tree `slang-tidy` invocation does not complete today.** It stops
   on a sibling's in-flight file, `rtl/core/mosaic_dispatch.sv`, with six
   "identifier used before its declaration" errors. Not this module's file; the
   scoped run in §7.2 is exit 0.

## 10. What is NOT verified

* **That the bank field is a modulo rather than a bit slice.** With
  `BANKS = 4 = 2²`, `tag % 4` and `tag[1:0]` are the same function on the 7-bit
  tag, so no test on this profile can separate them. What is separated is the
  *row* decode (`SLICE_ROW` mutant) and the `ceil`/quotient arithmetic on 96 =
  4 x 24 entries. The divisor form is what the RTL uses so that a profile with a
  non-power-of-two bank count stays correct; that claim is verified by geometry
  checks and by inspection, not by an executed profile.
* **Profiles other than p0.** Only `p0` exists; the driver sizes itself from the
  DUT and asserts the geometry is coherent, but a second profile has never been
  run. The C++ additionally requires `banks <= 8` (checked at runtime, so a
  larger geometry fails loudly) and assumes the standard Verilator port types.
* **A pipelined/registered read response.** This implementation pins zero-latency
  combinational reads (§1). A design that registers the response would fail this
  case, and the case does not attempt to detect which of the two the frozen
  interface "meant" — the interface does not say.
* **A distinct response-slot resource.** With a zero-latency port a response slot
  can never be occupied, so "the response slot is free" is the demand's own slot
  and there is no separate state to test. If the intended design has one, this
  test would have to be rewritten with it.
* **X-propagation on the inputs.** No phase drives X on any port. Undefined
  *storage* is exercised implicitly: `--x-initial unique` fills the unreset
  arrays, and the reason the response for an invalid entry is forced to zero
  rather than read from storage is exactly to keep that garbage out of a
  comparison. The array contents of invalid entries are not compared (they are
  undefined by design), so a bug that corrupted only an unwritten row would be
  invisible until that row was written and read.
* **Power-up contents of the storage arrays**, for the same reason.
* **Collector behaviour.** Retry policy, multi-cycle collection and consumer
  arbitration are I-026/I-027 and are not modelled anywhere in this package
  beyond the driver's own retry queue.
* **Longer soaks.** The random phase is 4000 cycles and the determinism
  programme 600 cycles twice; `max_cycles = 200000` is a bound the run does not
  approach (5311 cycles used). No claim is made about behaviour beyond that.
* **`o_*` counter wrap.** The counters are 32-bit and wrap by construction; no
  test runs long enough to wrap them (822 writes/admitted on seed 1).
