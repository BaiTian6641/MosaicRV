# V-018 — separate the architectural memory from visible stores

Work package V-018 (`docs/validation-plan.md` §5 V-018, "分离架构内存与可见存储").
Registered case **`mem.visibility_provenance`**, top `mosaic_core_tb`, seed 1,
budget 4,000,000 cycles.

| Artifact | Role |
|---|---|
| `sim/unit/tb_core_visibility.cpp` | the case: the three memories, the per-byte provenance rule, the scene, the coverage assertions, the rd-only and zero-extend driver controls |
| `sim/unit/mem_ref.h` (unchanged) | the independent RV64IM interpreter and the DUT's data-port memory (`DataMem`) |
| `sim/common/memory_model.cpp` (unchanged) | the platform model; each of the three memories is a **separate instance** |
| `sim/common/event_codec.cpp` (newly linked, unchanged) | the frozen event serialiser — this case is its first live use (§3) |
| `tools/run_visibility_controls.py` | the controls: nine observable mutants and two documented-unobservable ones, each rebuilt from a deleted directory |
| `results/unit/mem.visibility_provenance/` | the observed run: `run.log`, `result.json`, `visibility.events.txt` (the pinned revision's) |
| `build/p0/visibility_controls/` | the control builds: one directory per mutant, its binary and `out-*` |
| `results/reports/V-018-visibility.md` | this file |

## STATUS: PASS — the case passes from a clean build, the card's coverage is measured, and nine controls fail as required

| Check | Observed |
| --- | --- |
| the registered case, at the pinned revision (§8) | `PASS mem.visibility_provenance` (exit 0) |
| | `checks=1626 retires=450 loads=88 stores=116 drains=116 seed=1` |
| the run set | 8 runs: `p03_loadstore` and `p09_storeload` at 3 inputs each, plus a harness-authored **scene** over each of the two programs |
| data-port transactions | 204 (88 load reads + 116 store writes); every write attributed to exactly one committed store |
| shipping binary | `sha256 67f87996d814450fd66f1b31b5d33ec77fb5768d9f948f2b15309bbd0990578a`, built from the pinned revision (§8) |
| controls | **9 of 9 observable controls exit as required** (8 exit 1 naming the check they break, 1 — the rd-only weakening control — exits 0), each binary differing from the shipping one, each rebuilt from a deleted directory with its `-D` in the build command (§7) |
| unobservable mutants | 2 module mutants change nothing through the integrated core and are recorded as such with the reason (§7.1) |
| RTL gates, on the verification tree | `python3 tools/lint_rtl.py --profile p0` green (43 source files); `make lint-slang` exit 0; **no RTL file was edited by this package** |
| RTL gates, live tree at hand-off | `lint_rtl` reports `1 of 44 source file(s) failed`: `rtl/core/mosaic_core.sv` has an unconnected `req_tval_i` at :3923, from the I-045 (SV39) lane's uncommitted edits to `mosaic_core.sv`/`mosaic_lsu_endpoint.sv`/`mosaic_pkg.sv`. Not this package's file; the gates are green again once that lane commits. |
| `python3 tools/check_records.py` | green: `ok records agree: 53 delivered package(s), 62 registered case(s), ...`. The registry entry gained one `cpp` source (`sim/common/event_codec.cpp`) from the integration lead, not from this lane |
| `make check` | exit 0 (it does not include `lint_rtl`/`lint-slang`; both were green on the verification tree) |
| `python3 tools/check_event_contract.py --profile p0 --negative` | `negative controls: 36/36 illegal interfaces rejected` |

## 1. The rule the case exists to test

The card separates the architectural memory from the memory that became
*visible*. The rule this case asserts, per byte:

> Every byte that changes in memory has **one** legal producer: a store that
> retired, in the architectural retire order, **before** the byte appeared. The
> bytes that become visible are the committed stores' bytes **in commit order**,
> one drain each, with the store's own size and byte mask; a load's returned
> value equals an allowed source (the image byte, or the most recent committed
> store's bytes for that address); and a store that was cancelled -- never
> committed -- contributes no visible byte at all.

`config/contracts/event_v1.json` states the same separation as its
`store_drain_is_separate` rule: "A committed store's retirement and its
visibility are two records of different kinds, both carrying the store's
`retire_seq`". The case emits both records and joins them on that id.

**Identity, not content.** The producer of a byte is identified by its
**position in the commit order** -- the store queue is an ordered compacted
sequence whose drain takes position 0, so the k-th visible store must be the
k-th committed store -- and is *named* by the machine's own identity: the
architectural `retire_seq` (the schema's causal id) plus the core's per-lane
`{generation, tag}` presented on `ev_id`. The pc is printed in every message but
the rule never keys on it: the scene contains two stores from the **same pc** to
the **same address** with different data (a two-iteration loop), and they are
told apart by `retire_seq = 09 / lane_id = 0x0480` and `retire_seq = 0d /
lane_id = 0x0680` (§5). A rule that identified producers by payload or by pc
would be ambiguous -- p09's `sd a0, 0(s3); ... sd t0, 0(s3)` stores **the same
data** twice to one address -- and this was a real failure during bring-up: the
first version matched by payload and reported a duplicate drain that was not one.

## 2. Three memories, one of which the DUT touches

Nothing in the case is derived from the DUT's own view of what it wrote.

| Model | Owned by | Role |
| --- | --- | --- |
| `ref_mem` | `mosaic::MemoryModel` inside `mosaic_ref::RunReference` | the **expectation**: it decodes the same instruction words from the same image, models loads and stores itself, and yields the per-instruction pc/rd/value and the final byte image |
| `dut_mem` | the DUT's data port (`DataMem`) | the **subject**: every accepted write transaction is a byte becoming visible, and it is instrumented byte by byte |
| `arch_mem` | the case (`ArchMem`) | the **ledger**: it starts from the same immutable image bytes (read from a private seed model) and is advanced only by *committed* stores, in retire order, from the payload the event stream carries |

`dut_mem == ref_mem` (byte for byte, over every address either wrote) and
`arch_mem == ref_mem` are checked at the end of every run, so the ledger cannot
quietly agree with the DUT. The card's forbidden shortcut -- overwriting the
reference memory with the DUT's and calling it agreement -- is structurally
impossible here: the three models are separate objects and `ref_mem` is mutated
only by the interpreter.

## 3. The event schema, and the codec's first live use

The trace `results/unit/mem.visibility_provenance/visibility.events.txt` is
frozen schema **v2** (`config/contracts/event_v1.json`, 22 fields, 101 octets per
record) produced through `sim/common/event_codec.h` — the codec had no consumer
before this case, and no case had ever linked it. The driver takes the version
and the record size from `MOSAIC_EVENT_SCHEMA_VERSION` and
`mosaic::kEventRecordBytes`, never from a literal.

* Every store's retirement is a **RETIRE** record with `mem_is_store = 1`,
  `mem_addr`, `mem_data`, `mem_size` and the store's `retire_seq`.
* Every store's visibility on the data port is a **MEM_VISIBLE** record with the
  same `retire_seq`, the same `mem_addr`/`mem_data`/`mem_size`, and the cycle it
  appeared.
* **232 records** were encoded to the 101-octet layout and decoded back, field by
  field, before any comparison used them: **232 round trips**, 22 fields each
  (5,104 field comparisons), zero disagreements. The encoder also refuses a
  version mismatch, a short buffer and a field overflow, so a malformed record is
  a harness failure rather than a silent byte pattern.

Example (the scene's loop, both passes; every field is the canonical text form):

```
store-commit retire_seq=09 ... cycle=0000000000000039 kind=01 pc_before=0000000080010028
             insn_bits=0059b423 insn_len=04 mem_is_store=01 mem_addr=0000000080010208
             mem_data=0000000000000011 mem_size=03 ...        lane_id=0x0480
mem-visible  retire_seq=09 ... cycle=000000000000003b kind=03 pc_before=0000000080010028
             mem_is_store=01 mem_addr=0000000080010208 mem_data=0000000000000011
             mem_size=03 ...                                  lane_id=0x0480
store-commit retire_seq=0d ... cycle=000000000000004d kind=01 pc_before=0000000080010028
             mem_data=0000000000000012 mem_size=03 ...        lane_id=0x0680
```

Field by field, and the kind each is valid in (`valid_kinds` in the schema):

| field | valid kinds | what this trace carries |
| --- | --- | --- |
| `retire_seq` | RETIRE, TRAP, MEM_VISIBLE | the store's position in the architectural order; the run fails if a run needs more than the field's 256 values |
| `hart_id` | RETIRE, TRAP, MEM_VISIBLE | 0 (the profile is single-hart) |
| `cycle` | RETIRE, TRAP, MEM_VISIBLE | the commit cycle on a store-commit line, the visibility cycle on a mem-visible line |
| `kind` | RETIRE, TRAP, MEM_VISIBLE | 1 (RETIRE) for a store's commit, 3 (MEM_VISIBLE) for its visibility |
| `pc_before` | RETIRE, TRAP | the committed store's pc; 0 on MEM_VISIBLE |
| `pc_after` | RETIRE, TRAP | 0 — no RTL producer (reconciliation F-3) |
| `insn_bits` | RETIRE, TRAP | the instruction word from the retire lane's own `ev_insn`, not re-read from the image; 0 on MEM_VISIBLE |
| `insn_len` | RETIRE, TRAP | from the lane's `ev_len` (4 here; the corpus is rv64im with no C); 0 on MEM_VISIBLE |
| `rob_id` | RETIRE, TRAP | 0 — the field is 14 bits and the core's per-lane `{gen, tag}` is 16, so the value travels as a `lane_id=` token instead (§9) |
| `gpr_write_valid`, `rd`, `rd_value` | RETIRE | 0 — a store writes no GPR |
| `csr_write_valid`, `csr_addr`, `csr_value` | RETIRE | 0 — a store writes no CSR |
| `mem_is_store` | RETIRE, MEM_VISIBLE | 1 |
| `mem_addr`, `mem_data`, `mem_size` | RETIRE, MEM_VISIBLE | the store's `base + imm`, its 64-bit operand, its 3-bit size code — the same three values on the commit record and on the visibility record, which is what joins them |
| `trap_cause`, `trap_tval`, `trap_epc` | TRAP | 0 — the run set takes no trap |

The MEM_VISIBLE record is emitted **from the harness's memory side effect**, not
from the DUT: reconciliation F-4 in the schema file records that "the memory path
has no producer ... a DUT-side producer is a declared gap (NOT verified)", and
that is still true after this package. What the case proves is the *rule* over
the DUT's data port, not that the DUT emits the record (§9).

## 4. The programs and the scene

The corpus programs carry the width/sign/mask/forwarding coverage:
`p03_loadstore` (sb/sh/sw/sd, ld/lw/lhu/lh/lb/lbu at every width, sign extension,
partial masks, read-back), `p09_storeload` (store-to-load at the same address
back to back, sub-width store into a doubleword then a load of the merged
doubleword). Three compiled inputs per program, entered unmodified at their own
`main`.

A harness-authored **scene** is written into RAM past the image (never into
`tests/programs/**`) and is entered by the reset brick; it hands over to the
corpus `main` with `jal`. It adds the three coverages the corpus cannot carry:

| # | Situation | Where | What the case requires |
|---|---|---|---|
| 1 | a wrong-path store at **writable RAM** | reset+8 `sd x10, 0(x10)` = `0x80010000`, fall-through of the entry jump; and scene+16 `sd t1, 0(s3)` = `0x80010200`, fall-through of a taken `beq` | no visible byte, no data-port transaction at those addresses, and no visible byte in the scene's scratch whose producer is not the loop's own store |
| 2 | an access **crossing the line boundary** | reset+12 `sd x10, 4(x10)`; scene+20 `sd t1, 4(s3)` = `0x80010204`, a doubleword at a non-multiple of 8 | zero data-port transactions at an address that is not a multiple of its size, and the target bytes equal the reference's |
| 3 | two stores from the **same pc to the same address** | scene+40 `sd t0, 8(s3)` executed twice (a two-iteration `bne` loop), data `0x11` then `0x12` | both commit; they carry different `retire_seq` **and** different `lane_id`; the byte holds the second pass's payload, produced by the second pass's store |

The base register is formed with `auipc`, not `lui`: `lui 0x80010` sign-extends
the 32-bit `0x80010000` and produces `0xFFFFFFFF80010000`, and the harness must
build the address the architecture does.

## 5. Commit versus drain, measured

Recorded per store: the **commit** cycle (the cycle its RETIRE record is
observed) and the **drain** cycle (the cycle its write transaction is accepted),
plus its position in the commit order.

| Quantity | Observed |
| --- | --- |
| committed stores that are stores | 116 |
| **bytes that became visible, each with a named committed producer** | **740** (84 doubleword + 8 word + 12 half + 12 byte stores) |
| drains checked, one per committed store, in commit order | 116 |
| drains at the same cycle as their commit | 0 |
| smallest / largest commit-to-drain gap | 2 / 13 cycles |
| byte-writes at an address that already had a producer (a superseding write) | 108 |
| drains whose byte was already superseded by a *newer committed* store (the in-order queue writes the older producer's byte first) | 24 |
| visible bytes with no committed producer | 0 |
| duplicate drains / out-of-order drains | 0 / 0 |
| bytes left holding a superseded producer's value (ledger vs reference) | 0 |

**Answer to the card's question, "was any byte in your programs ever written by two
producers?": yes, deliberately and legally.** 108 byte-writes went to an address
that already had a producer (the corpus reuses its scratch, and the scene's loop
writes one address twice). None of them is a defect: each was attributed to its
own committed store, in commit order, and each byte's *final* value is the newest
committed store's byte, checked against both the ledger and the reference. The
illegal forms -- the same producer twice (a duplicate drain) and a producer with
no commit -- are the two the controls inject, and both are caught.

The 24 "superseded" drains are the interesting number: the newer committed store
has retired but not yet drained, so the in-order drain legitimately writes the
older producer's byte, which the newer store's own drain then overwrites. That is
why the ledger comparison is a **final** per-byte comparison and not a per-drain
one: `arch_mem` holds the newest committed store's byte for an address, while the
drain sequence must still apply the older bytes in order. The case checks both
statements -- (a) each drain carries its producer's own bytes with its producer's
own mask, (b) after every store has drained, each byte equals the newest
committed store's byte and the reference's.

## 6. The coverage the card names, measured rather than asserted

| Coverage | Measured | How |
| --- | --- | --- |
| every load width (byte/half/word/double) | 8 / 8 / 8 / 64 retiring loads | each load decoded from its image word; width counted at retirement |
| every store width | 12 / 12 / 8 / 84 visible stores | counted from the accepted write transactions |
| sign extension on loads | 8 retirements where the sign-extended value differs from the raw bytes | the ledger value is sign-extended by the load's own size and signedness; a zero-extending machine fails the source comparison (a driver control shows the check is sensitive to it, §7) |
| partial byte masks on stores | 32 drains whose strobe mask does not cover the whole 8-byte window | the transaction's strobes are required to be exactly `((1<<size)-1) << (addr&7)`, so a masked byte store is checked, not assumed |
| same-hart store-to-load forwarding | 128 bytes | `o_mem_lq_fwd_bytes_o`, and every retiring load's value is compared against the ledger (which holds the forwarded store's bytes) |
| an access crossing a line boundary | 2 directed crossings (one per scene run) | the scene's `sd` at `0x80010204` is statically asserted to be a crossing access; zero data-port transactions are misaligned; `o_mem_lsu_misaligned_o == 0` in the shipping build |
| a wrong-path store that must leave no byte | 22 directed wrong-path stores (2 per run at the reset brick, 3 more per scene run), at 4 distinct target addresses | none of the 4 addresses received a write transaction or a visible byte, in any of the 8 runs; the only bytes in the scene's scratch have the loop's store as their producer |
| a cancelled store that leaks nothing | 0 stores squashed out of the queue over the whole run set | `o_mem_sq_squash_o` summed over the 8 runs (measured, not assumed) |
| a load whose value matches an allowed source | 88 loads | every retiring load's address is formed from the instruction word and the architectural register file, and its value compared with the ledger |

## 7. The controls

`tools/run_visibility_controls.py` builds the case once per mutant from a
**deleted** build directory, with the define in the **Verilator command line**
(and, for the two driver-side controls, in `-CFLAGS` as well), requires the
binary to differ from the shipping one, and requires the run's verdict. The
shipping binary is unchanged from the registered build.

```
verilator --cc --exe --build -j 0 -O2 -CFLAGS "-O2 -std=c++17 -Wall" \
  --x-assign unique --x-initial unique --top-module mosaic_core_tb \
  -Mdir build/p0/visibility_controls/<label>/obj_dir -Ibuild/p0/sim \
  -Irtl/core -Irtl/common -Ibuild/p0/rtl -CFLAGS -Isim/common \
  -CFLAGS -Ibuild/p0/sim [-D<mutant> ...] -o build/p0/visibility_controls/<label>/mem.visibility_provenance \
  <the 20 RTL files + sim/tb/mosaic_core_tb.sv + the 5 cpp files + sim_common.cpp>
```

| # | Mutant (`-D`) | Defect it injects | exit | binary sha256 | first failure |
|---|---|---|---|---|---|
| 1 | `MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH` + `MOSAIC_SQ_MUTANT_VISIBLE_BEFORE_COMMIT` | the barrier stops holding memory macros *and* the drain stops needing authorisation: a **wrong-path store's bytes become visible** | 1 | `b2bec02f…` | cycle 35, scene: "every visible byte has a legal, committed producer: a write to `0x80010200` size=8 reached memory and does not match the committed store (none outstanding)" |
| 2 | `MOSAIC_SQ_MUTANT_VISIBLE_BEFORE_COMMIT` | the drain offer ignores the authorisation watermark: a store's bytes are visible before it commits | 1 | `ddbc6394…` | cycle 55: "every visible byte has a legal, committed producer: a write to `0x80010208` size=8 reached memory … (no committed store is outstanding)" |
| 3 | `MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE` | an accepted drain does not remove the entry: **one store writes twice** | 1 | `6ca2f0a7…` | cycle 63: "a byte written twice by two producers: the store retire_seq=9 rob_id=0x0480 pc=0x80010028 was already drained at cycle 59 and its bytes are visible again at 0x80010208" |
| 4 | `MOSAIC_LQ_MUTANT_MEMORY_OVER_STORE` | a load that must forward reads memory instead: **stale bytes no legal source produced** | 1 | `728cb8cf…` | cycle 140: "a load's returned value matches an allowed source: retire 32 at 0x80001234 … returned 0x0, but the most recent committed store retire_seq=31 pc=0x80001230 holds 0x0123456789abcdef" |
| 5 | `MOSAIC_VISIBILITY_RD_ONLY` + `MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG` | **weakening control**: the driver drops every memory/provenance comparison and keeps only `rd` | **0** | `80206156…` | none — `PASS` with `checks=1535 retires=0 stores=0 drains=0`, i.e. the rd comparison alone does **not** catch the corrupt store payload that control 8 catches |
| 6 | `MOSAIC_VISIBILITY_ZERO_EXTEND` | the ledger zero-extends instead of sign-extending: shows the source check is sensitive to the sign bit | 1 | `9d7d48c6…` | cycle 199: "…retire 54 at 0x8000128c size=1 signed … returned 0xffffffffffffffef, but … holds 0x00000000000000ef" |
| 7 | `MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH` | the barrier stops holding memory macros: a wrong-path access reaches the data port before the redirect discards it | 1 | `a0c2e471…` | cycle 256: "exactly one data transaction per load and per store reached the data port: data transactions=30 (lsu=30), loads=13 stores=16" |
| 8 | `MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG` | the retiring store's payload reports every size as a word | 1 | `11591dfb…` | cycle 59: "the drain's byte mask is the store's own size: store … size=4 expected strobes 0x000000000000000f, the data port carried strobes 0x00000000000000ff size=8" |
| 9 | `MOSAIC_SQ_MUTANT_DRAIN_OUT_OF_ORDER` | the drain takes any ready entry instead of position 0 | 1 | `a60245e2…` | cycle 138: "the visible store is the oldest committed store not yet drained: a store at 0x80001088 drained while the older committed store retire_seq=31 pc=0x80001230 had not" |

Control 3 is the card's "duplicates a drain"; control 1 is its "makes a
wrong-path store's bytes visible"; control 4 is its "a load returns a value no
legal source could have produced"; control 5 is its rd-only weakening. Control 5
is deliberately the *inverse* of the others: it passes on a defect the real
comparison catches (control 8), which is what shows the byte comparison is
load-bearing rather than decorative. Control 6 is an *expectation-inverting*
driver control and is labelled as such: it does not break the machine, it shows
the case distinguishes the sign bit from the low bytes.

### 7.1 Mutants documented as unobservable through the integrated core

| Mutant | Why the case cannot see it |
| --- | --- |
| `MOSAIC_SQ_MUTANT_COMMIT_OUT_OF_ORDER` | the watermark is *derived from the leading survivors* (`mosaic_store_queue.sv`), so a commit naming a younger entry cannot advance it past an unauthorised older entry, and no store ever becomes visible out of order. `store.wrong_path_visibility` sees it on the queue's own commit port; a core-level case cannot. |
| `MOSAIC_LSU_MUTANT_NO_SIGN_EXTEND` | the integrated load value is extracted by `mosaic_load_queue.sv` (`extended_c` / `final_ext_c`) from the endpoint's lane-aligned `rsp.data`; the endpoint's own `extracted_c`, where this mutant lives, is not on the value path. This is a finding for the ledger: the endpoint's extraction is dead when the LQ drives the load. |

## 8. RTL revision hashes compiled

The 20 RTL sources of the **pinned revision** the case and every control were
built against: the committed tree at `HEAD` = `3174a95`. The working tree was
mid-edit by another lane (I-045, SV39) at hand-off -- `mosaic_core.sv`,
`mosaic_lsu_endpoint.sv` and `mosaic_pkg.sv` had uncommitted changes and the
core did not elaborate (an unconnected `req_tval_i`) -- so this case's evidence
was produced from a `git archive HEAD` export with the driver under test copied
in, which is why the revision is stated by hash rather than by "the tree". Every
hash below is the hash of the file that was *actually compiled*, from the
build's own file record:

* `mosaic_lsu_endpoint.sv` = `094ae245…` (part of the elaboration closure, §8.1).

The case must be re-run once the sibling lanes land; the registry entry's
`"pending": true` stays until the integration lead records it.

| file | sha256 |

| file | sha256 |
|---|---|
| `rtl/core/mosaic_pkg.sv` | `45c8d751…` |
| `rtl/core/mosaic_uop_pkg.sv` | `9fcc5333…` |
| `rtl/core/mosaic_alu.sv` | `0a92b98e…` |
| `rtl/core/mosaic_branch_cmp.sv` | `cf996100…` |
| `rtl/core/mosaic_branch_target.sv` | `73c41682…` |
| `rtl/core/mosaic_predictor.sv` | `bad709b8…` |
| `rtl/core/mosaic_fetch.sv` | `c1592a67…` |
| `rtl/core/mosaic_decoder.sv` | `a90500e4…` |
| `rtl/core/mosaic_rename.sv` | `95226d0c…` |
| `rtl/core/mosaic_rob.sv` | `afba8cf6…` |
| `rtl/core/mosaic_iq.sv` | `447bfabd…` |
| `rtl/core/mosaic_prf.sv` | `2e695d23…` |
| `rtl/core/mosaic_wb_arbiter.sv` | `0f6f3ab7…` |
| `rtl/core/mosaic_macro_desc.sv` | `a4ad9fe8…` |
| `rtl/core/mosaic_dispatch.sv` | `533938f6…` |
| `rtl/core/mosaic_cluster.sv` | `4ab36113…` |
| `rtl/core/mosaic_redirect_arb.sv` | `658aeb6b…` |
| `rtl/core/mosaic_muldiv.sv` | `dd12e9ba…` |
| `rtl/core/mosaic_retire.sv` | `e2c69c18…` |
| `rtl/core/mosaic_core.sv` | `22f00f8b…` |

### 8.1 Effective elaboration closure

The registry lists 20 RTL files, but the case's **effective elaboration closure
is 29**: Verilator resolves the modules the core instantiates from the same
directory, so `mosaic_lsu_endpoint.sv`, `mosaic_store_queue.sv`,
`mosaic_load_queue.sv`, `mosaic_amo_unit.sv`, `mosaic_amo_alu.sv`,
`mosaic_csr.sv`, `mosaic_interrupt.sv`, `mosaic_pmp.sv` and
`mosaic_reservation.sv` are compiled into this case as well (verified from the
build's own file record). That is why the store-queue and load-queue mutants the
controls use are applicable here: their files are part of the build even though
the registry does not name them. No RTL file was edited by this package.

## 9. Not covered

* **I-036 (speculative load replay / late alias) is not enabled.** The card asks
  for replay/rollback evidence in that case and it is **not claimed here**: the
  machine never has a load whose forwarded source is cancelled after it retires,
  because the conservative recovery leaves nothing younger than a branch in the
  machine at all: `o_mem_sq_squash_o` summed over this case's whole run set is
  **0** (reported by every run), and `core.mem_program` asserts the same. A "load
  replayed after its source was squashed" scenario is
  therefore not producible at the core today, and neither the run set nor the
  mutants exercise one.
* **The MEM_VISIBLE record has no DUT-side producer.** The record in the trace is
  emitted by this harness from the accepted data-port transaction. The schema's
  reconciliation F-4 records the gap; this case *proves the rule over the DUT's
  data port*, not that the DUT can emit the record itself.
* **`rob_id`, `pc_after`, `trap_epc`.** The schema's 14-bit `rob_id` is written
  zero in the trace, because the core's per-lane identity (`o_geom_ret_id_w_o` =
  16 bits) does not fit the frozen field; it is carried as an extra `lane_id=`
  token on each line, which a consumer must use instead. `pc_after` and
  `trap_epc` have no RTL producer (reconciliation F-3) and are zero.
* **No trap is taken.** The run set retires wholly on the architectural path, so
  no TRAP record is emitted and a store that faults (a PMP- or misalignment-
  refused store) is not covered here; the case asserts `o_mem_sq_fault_o == 0`,
  `o_mem_lsu_misaligned_o == 0` and `o_pmp_deny_ctr_o == 0` instead. The
  PMP-refused-store trap is another lane's defect.
* **The line-crossing coverage is a refusal, not a split.** p0's frozen policy
  traps a misaligned access before memory (`config/profiles/p0.json`,
  `mosaic_lsu_endpoint.sv`), so the crossing access is covered as "no byte, no
  transaction, `o_mem_lsu_misaligned_o` counts it when it is offered"; a
  naturally-handled boundary split (a cache line refill, or a two-beat access) is
  not implemented in p0 and is not claimed.
* **No cache is in the core path.** The card's Fail criterion "treating a cache
  refill as a load effect" cannot arise here: the DUT's data port in this case is
  the flat memory model, and the cache (I-042) is a separate top.
* **Protocol words.** A write to `MOSAIC_TOHOST`/`MOSAIC_FROMHOST` is a protocol
  register in the frozen memory model: the write does not store a byte, so those
  bytes are compared on the data port only (8 such bytes in the run set) and are
  excluded from the memory and ledger comparisons.
* **Wrong-path visibility needs the mutant.** In the shipping machine the
  wrong-path words are never dispatched (the branch barrier holds them), so the
  "a cancelled store leaks nothing" property is asserted as the absence of any
  byte and any transaction at those addresses; the *positive* evidence that the
  rule catches a leak is control 1.
