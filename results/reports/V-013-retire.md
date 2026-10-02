# V-013 — multi-retire width and order

Work package V-013 (`docs/validation-plan.md` §5 V-013, "比较多退休宽度与顺序").
Registered case **`retire.width_and_order`**, top `mosaic_core_tb`, seed 1,
budget 4,000,000 cycles.

| Artifact | Role |
|---|---|
| `sim/unit/tb_core_retire.cpp` | the case: the program, the independent per-slot model wiring, the comparison, the trace gap detector, the coverage assertions, four checker mutants |
| `sim/unit/trap_ref.h` (unchanged) | the independent RV64IM_Zicsr interpreter the per-slot model is built from |
| `sim/common/memory_model.cpp` (unchanged) | the platform memory each side owns a *separate* instance of |
| `tools/run_retire_controls.py` | the controls: five mutants, each rebuilt from a deleted directory; three invisibility probes |
| `results/unit/retire.width_and_order/` | the observed run: `run.log`, `result.json` |
| `build/p0/retire_controls/` | the control builds: `build_command.txt` and the binary of each mutant, plus `out-*/` |
| `results/reports/V-013-retire.md` | this file |

## STATUS: PASS — the case passes from a clean build, the five situations are produced and counted, five controls fail as required

| Check | Observed |
| --- | --- |
| the registered case | `PASS retire.width_and_order` (exit 0) |
| | `checks=1187 events=119 retires=118 comparisons=973 dual=28 single=63 same_rd=1 trap_blocked=1 store_load=1 cycles=573 seed=1` |
| shipping binary | `sha256 ed82804c6f0ec41c422cb6399724f61b28f1666fc889474c2b09b4b5388d19f8` |
| controls | **5 of 5 exit 1**, each naming the check it breaks, each binary differing from the shipping one, each rebuilt from a deleted directory (§5) |
| invisibility probes | 3 module mutants under the core change nothing at the top and are recorded as such, with the reason (§5.2) |
| RTL gates | `tools/lint_rtl.py --profile p0` green (33 files); `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv')` 0 errors, exit 0; **no RTL file was edited by this package** |
| the driver under the project's C++ standard | `-Wall -Wextra -Wshadow` adds no warning of its own |
| `tools/check_records.py` | **RED**, for a record defect that is not this package's: `registry: case core.corpus_sweep belongs to delivered package I-023, which does not list it as evidence` (§6.4) |

## 1. The rule the case exists to test

> For every cycle, the events the core publishes are the retiring instructions in
> **program order**: slot 0 is the oldest, slot 1 the next oldest, a slot retires
> only if the slot in front of it did, and no instruction is published twice or
> skipped. `retire_order` advances by exactly one per published event. A trapping
> instruction in slot 0 retires **nothing younger** in that cycle: the trap is an
> event of its own and the entries behind it are gone. `x0` is read as zero and a
> write to it is discarded -- it never appears as a destination write in the
> stream.

The card's five situations are the shapes in which that rule is easiest to get
wrong. All five are *directed* in one program, and the case counts each one from
the observed stream (§4) rather than asserting that the program intends to.

## 2. The program

121 words at `0x80000000`; the trap handler is the first four instructions with a
jump over them at word 0, so `mtvec` is installed with an address known before
the program that writes it is assembled. The unwritten space after the program is
filled with `jal x0, 0` so a fetch that runs past the end neither faults nor
feeds a don't-care into the decoder.

| # | Situation | Where | What the case requires |
| --- | --- | --- | --- |
| 1 | two writes to one `rd` in one cycle | `addi x8, x0, 0x111` at `0x80000108`, `addi x8, x0, 0x222` at `0x8000010c` | both retire in one cycle, slot 0's delta is `0x111` and slot 1's is `0x222` (the two disagree, so a delta taken from the cycle's end state is wrong), and `x8` afterwards is `0x222` |
| 2 | a write to `x0` | 56 directed instructions, one per writeback arm the decoder implements: `op-reg`, `mul-div`, `op-imm`, `op-32`, `op-imm-32`, `lui`, `auipc`, `load`, `csr`, `csr-imm`, `jal`, `jalr` | each retires **in a lane** (40 distinct Pcs in lane 0, 16 in lane 1), no event ever reports `reg_we` with `rd = 0`, and the two instructions that read `x0` back (`add x29, x0, x0`, `add x30, x0, x6`) publish the model's values |
| 3 | the instruction after a branch | taken `beq x0, x0, +8` at `0x80000120` (its shadow word at `0x80000124` must never retire); not-taken `bne x0, x0, +8` at `0x80000130` (its fall-through must retire); taken `jal x0, +8` at `0x800000d0` (shadow at `0x800000d4` must never retire); `jalr x0, t6, 8` at `0x800000dc` | a taken branch whose next event is its target, a branch whose next event is its own fall-through, and neither shadow word ever published |
| 4 | a trapping instruction in slot 0 with an ordinary instruction in slot 1 | misaligned load `ld x9, 1(x0)` at `0x800001bc`, with `addi x9, x0, 0x7777` behind it | in that cycle lane 1 is not valid (checked), the ROB holds at least two entries, the trap event carries cause 4 and vectors to the model's next PC, and the ordinary instruction retires after `mret` resumes at `0x800001c0` |
| 5 | an adjacent store/load | `sd x6, 0(x5)` at `0x8000015c`, `ld x7, 0(x5)` at `0x80000160`, same address | both retire in one cycle, store in slot 0 and load in slot 1, with the load's value the one the store forwarded (`0x5a5`) |

Two `div` instructions are placed so that situations 1 and 5 *can* occur: a
divide holds the reorder-buffer head for ~64 cycles, during which the pair behind
it completes, and one filler instruction between the divide and the pair takes
the divide's second retire slot, so the next cycle presents the pair as slots 0
and 1 rather than splitting it across the width. Without them the two macros are
issued a cycle apart and retire a cycle apart -- a property of the issue rate,
not of the retirement path -- and the pair would be counted zero times. That is
stated here because it is a fact about how the situations were produced, not
because the case asserts a particular cycle count.

## 3. The independent per-slot model

`mosaic_trap::RunReference` (sim/unit/trap_ref.h) executes the same words in
program order from its own `mosaic::MemoryModel` -- a second instance the DUT can
never touch -- through the misaligned-load trap and the handler's `mret`, and
returns one ordered event per architectural action. This case derives the event's
**new PC** as the PC of the next event in program order, classifies a store by
decoding the word at the retired PC (the event's own store field is inert -- see
§6.2), and otherwise compares the DUT's stream against the model's, slot by slot:

| Field | Compared | Notes |
| --- | --- | --- |
| identity | event PC | the whole stream must be the model's, in order (the model's events, including the trap entry, are consumed one per published slot) |
| class | trap flag | a trap entry is an event, not a retirement |
| destination | `rd`, `reg_we` | for a write to `x0` the model says "no write", and so must the machine |
| value | `value`, when the model says the instruction wrote | never derived from the DUT's own ports |
| new PC | for a trap: `o_trap_target_o` must equal the model's next PC; for branches: the next event's PC *is* the target | the switch of control |
| trap identity | `o_trap_cause_o`, `o_trap_epc_o`, synchronous (`o_trap_is_irq_o == 0`) | |
| `retire_order` | `ev_seq`, dense and strictly increasing **modulo its own width** | the counter is `$clog2(2*ROB+1) = 7` bits, so it wraps every 128 events; the check is "advances by exactly one per event, modulo the width", and it reports the first gap or duplicate with the cycle and **both identities** (`pc`, `id`, `seq`, `lane`, `cycle`) |
| per cycle | lane order is program order, a slot retires only behind the slot in front of it, and two slots in one cycle carry distinct event identities | |
| second opinion | `o_commit_o == retirements published`, and finally `retire_order` covers the model | |

Nothing in the comparison reads the DUT's retire ports to *build* the
expectation; those ports are only what is compared.

## 4. Coverage: what the run produced, counted rather than asserted

From the observed run (`results/unit/retire.width_and_order/run.log`):

| Measured | Count | Requirement |
| --- | --- | --- |
| published events / retirements / traps | 119 / 118 / 1 | all model events consumed, compared once each |
| cycles retiring two slots | 28 | `> 0` |
| cycles retiring one slot | 63 | `> 0` -- the comparison is exercised at the width-one shape as well as width two |
| cycles with two writes to the same `rd` | 1 | `> 0`, and the two values disagree (`0x111` vs `0x222`), so a cycle-end delta cannot pass |
| trap cycles with an ordinary instruction present behind the trap | 1 | `> 0`; `o_rob_occupied_o = 10` at the trap cycle, lane 1 not valid |
| cycles with a store in slot 0 and a load in slot 1 | 1 | `> 0` (`0x8000015c`, `0x80000160`) |
| taken conditional branches followed by their target | 1 | `> 0` (`beq` at `0x80000120`) |
| conditional branches followed by their fall-through | 1 | `> 0` (`bne` at `0x80000130`) |
| shadow words that retired | 0 | the two words a taken transfer skips must never appear |
| `x0`-destination instructions retired, lane 0 / lane 1 | 40 / 16 distinct Pcs | every one the program emitted appears; none of them reports a destination write |
| stores the store queue authorised | 11 | equals the number of retiring stores decoded from the image |
| requests refused as unsupported / illegal | 0 / 0 | the machine never stopped on a macro |

The architectural end state the program publishes is compared with the model's
memory byte for byte: `x0` read back through `add` is `0` and `x0 + 3` is `3`,
the two-writes-to-one-`rd` result is `0x222`, the not-taken branch's fall-through
wrote `0x7777`, the store/load pair both read `0x5a5`, and the instruction behind
the trap wrote `0x7777` after the handler returned.

## 5. Controls

`python3 tools/run_retire_controls.py` (exit 0). Each row's build directory was
deleted before the build, its `-D` is in that build's own `build_command.txt`,
its binary differs from the shipping one, and it exits 1 naming the check.

| Mutant | Kind | Defect it injects | First failure it produced | Binary sha256 |
| --- | --- | --- | --- | --- |
| `MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL` | RTL | the trapping instruction takes the ordinary retirement path: its payload reaches the stream as a retirement and its event carries no trap flag | `the trap event occupies lane 0 of the stream: the trap pulse and the event stream disagree` (cycle 505) | `b2fbb3160493ade860f0e126fe0d320bb7ae0853ac07b7bc89821bac4c129c9a` |
| `MOSAIC_RETIRE_CHECKER_CYCLE_END` | driver | the older slot's expectation is taken from the cycle's end state instead of from the model's per-slot step | `the retirement stream follows the program-order model: event 61 value: at 0x0000000080000108 x8: expected 0x0000000000000222, got 0x0000000000000111` (cycle 388 — the two-writes-to-one-rd cycle) | `188f7408a3d1c09693bb049dfc06b184463d03fc68859c777e0893c82ea24038` |
| `MOSAIC_RETIRE_CHECKER_LANE_REVERSED` | driver | the model is consumed in the order the slots were read rather than in lane order — the card's "slot order established by callback accident" | `the retirement stream follows the program-order model: event 24 pc: expected pc 0x0000000080000070, got 0x000000008000006c` (cycle 59) | `9a65865199912a1042b8c22c9245b894589f710b7672aac8c091127683733c91` |
| `MOSAIC_RETIRE_CHECKER_X0_WRITE` | driver | the expectation describes a write to `x0` as a real destination write | `the retirement stream follows the program-order model: event 0 destination: at 0x0000000080000000: expected rd x0 we=1, got x0 we=0` (cycle 11) | `6d2a8324cda950e853b196ab8b4528f5ffd388f8876213b8f6c15957309cfd08` |
| `MOSAIC_RETIRE_CHECKER_DROP_EVENT` | driver | the checker loses one event on the way to the comparison | `retire_order is dense and strictly increasing: cycle 59: expected seq 23 after (pc 0x000000008000006c id 0x0000000000000b80 seq 23 lane 0 at cycle 59), saw seq 24 (pc 0x0000000080000070 id 0x0000000000000c00 seq 24 lane 1 at cycle 59)` | `a20e2ab280274454a366b23078e71ad9af7514c40c60ea65b07e09dd56b701e9` |

Mapping to the card's fail modes: "a cycle's end state contaminating an earlier
slot" → `CYCLE_END`; "slot order established by callback accident" →
`LANE_REVERSED`; "`x0` modified" → `X0_WRITE`; "an unreported trace gap" →
`DROP_EVENT`. The fifth control, the only RTL mutant, covers the pass criterion
"a slot-0 exception preventing illegal younger retirement" from the trap-identity
side: emitted as an ordinary retire, the trap stops being a trap event and the
stream disagrees with the trap handoff in the same cycle.

### 5.1 Why four of the five are driver-level, stated explicitly

Three of the card's four fail modes are properties of the *checker* (an
expectation taken from the wrong snapshot, an order taken from the wrong place, a
detector that does not fire), and the fourth ("`x0` modified") cannot be made
observable by any mutant this project already has, because the core is built so
that it cannot happen: the decoder qualifies `reg_write` with `rd != 0`
(`mosaic_decoder.sv`, every `ctl.reg_write` arm), dispatch qualifies the
descriptor with `!alloc_is_x0` (`mosaic_dispatch.sv:543`), the source-read path
hardwires `x0` to zero (`mosaic_rename.sv`, `rs*_is_x0`), and the commit path
excludes `rd 0` (`mosaic_retire.sv`, `commit_valid`). So the `x0` control injects
the defect into the *expectation* -- a write to `x0` described as a real
destination write, which is exactly what a machine that modified `x0` would
publish -- and the case must reject it at the per-slot destination check. The
case also asserts the machine's own events never carry such a write.

That is weaker evidence than an RTL mutant that really writes `x0` would be, and
it is labelled as such here rather than presented as one. An RTL mutant that
reports a destination write for `rd 0` could be added to `mosaic_retire.sv` in
the project's `ifdef` style, and was not added: this package changed no RTL, so
the revision another lane is running the corpus against stays valid. If that
trade is ever revisited, the mutant is a one-line guarded variant of
`ev_reg_we = lane_effect & pay_reg_we`.

### 5.2 Invisibility probes: module mutants that change nothing at the top

Run and reported because "this defect is invisible here" is a fact about the
machine, and because a probe that changes behaviour would mean the core changed.
All three exit 0 with results identical to the shipping build.

| Probe | Why it is invisible through the integrated core | Binary sha256 |
| --- | --- | --- |
| `MOSAIC_RETIRE_MUTANT_SECOND_LANE_UNORDERED` | the order rule is removed, but the acknowledgement filter below it drops an ack for lane 1 with no ack for lane 0, so no out-of-order event is published; the fault is reported on `o_order_fault`, which the integrated core does not expose | `846fb77e921871cf9f08ef460b031c6bb8ff9b509bb6cd562dbb0f6b4eed6c8e` |
| `MOSAIC_ROB_MUTANT_RETIRE_OVER_EXCEPTION` | an exceptional macro becomes retirable in the buffer, but the retire unit decides the trap from the entry's own exception flag, so the trap is still taken and the entry still does not retire | `29600b7a2c0b15b90d595cbc97013f081555bd4685779aafe0a2413645b2942e` |
| `MOSAIC_RENAME_MUTANT_X0_ALLOC` | a write to `x0` allocates a physical tag, but nothing architectural reads it: the source path hardwires `x0` to zero and the commit path excludes `rd 0`. The defect is a tag leak, visible only as exhaustion after enough writes to `x0` | `84a5de5f0a2f0fff0b5f25ddc256d598b56b956d4a12eed420e3613aa23c0d21` |

## 6. Reported defects (found, not fixed)

This package changed **no RTL** and **no other case's driver**; each item below is
reported so the owner of the affected package can act, and each is reachable from
this case's own run.

### 6.1 `LWU` (LOAD, funct3 `110`) is refused as illegal, but RV64I requires it

`mosaic_decoder.sv:278-296` (the `OP_LOAD` arm) decodes funct3 `0,1,2,3,4,5` and treats
everything else as illegal, with the comment at line 289, *"funct3 110 and 111 are reserved:
RV64I has no load wider than ld"*. That is wrong for `110`: RV64I defines
**LWU**, an unsigned word load. `sim/unit/trap_ref.h` (the project's own
reference) implements it (`case 0x6u`), and so does the ISA.

Observed while this case was being written, with `lwu x0, 0(t3)` in the program:
the machine stopped at that instruction --
`cycle 6266: stalled: head_pc=0x0 head_valid=0 occupied=0 allocated=45 retired=45
unsupported=1 illegal=1 stopped=1 traps=0 mret=0 redirects=1`. The `lwu` was
removed from the program, so the x0 write forms the case enumerates are the arms
the decoder accepts today; this paragraph is here so the omission is not mistaken
for coverage.

### 6.2 The retire event's store and CSR identity fields are tied off

`mosaic_core.sv:2536-2543` wires the retire instance's `pay_is_store`,
`pay_store_addr`, `pay_store_data`, `pay_store_size`, `pay_csr_we`,
`pay_csr_addr` and `pay_csr_value` to constant zero. The event contract
(config/contracts/interfaces.json, and the module header) has the retire event
carrying these; through the integrated core `ev_store`, `ev_store_size` and
`ev_csr_*` are therefore always zero. The run confirms it: 0 of the 11 retiring
stores carried the flag, while the store queue authorised exactly those 11 (the
case checks that cross-identity separately, from the store-queue counter and its
own decode of the retired PC). The per-slot delta this case compares therefore
carries the retired PC as the store's identity, not the event's own field.

### 6.3 A system-instruction trap publishes no event at all

`mosaic_retire.sv` documents that "the trap is an event in its own right, and it
occupies lane 0's slot". That holds for an exception recorded on the reorder-buffer
entry -- the misaligned load in this program publishes exactly that event -- but
not for a trap the system unit resolves (`mosaic_core.sv`, `sys_trap_q`). Observed
while this case was being written, with `ecall` as the trapping instruction:

```
[ev] cycle=404 pulse=1 occupied=7      <- the trap pulse, no event in this cycle
     allocated=110 retired=103 unsupported=0 illegal=0 stopped=0 traps=0
```

The last event published before the trap was the previous instruction; the next
was the handler's first instruction. The DUT asserted `o_trap_valid_o` and
`o_trap_epc_o` named the `ecall`, but `ev_*` carried **no event at all** for it,
so a checker reading only the event stream sees a PC discontinuity with nothing
to explain it. The program uses a misaligned load instead (that path does publish
the lane-0 trap event); whoever owns the trap path should decide whether a system
trap is meant to be an event.

### 6.4 `tools/check_records.py` is red on the committed tree (not this package)

`registry: case core.corpus_sweep belongs to delivered package I-023, which does
not list it as evidence`. The registry names `core.corpus_sweep` (task I-023) and
the delivery ledger does not list it; the ledger is outside this package's write
scope. Every other record check passes.

## 7. Gates

| Gate | Result |
| --- | --- |
| `python3 tools/lint_rtl.py --profile p0` | `lint: 33 source file(s) clean` (exit 0) |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | 0 errors (exit 0; 5 pre-existing `STYLE-16` warnings about generate blocks, none in a file this package touched) |
| `python3 tools/check_records.py` | red for §6.4 (a record defect outside this package); no red line names this package |
| the driver, `-Wall -Wextra -Wshadow -fsyntax-only` | 0 warnings of its own |
| RTL revision | unmodified by this package |

The five existing core cases (`fabric.fixed_two_cluster`, `core.corpus_branch`,
`core.unwritten_reg_read`, `core.mem_program`, `core.trap_csr_program`) were **not
re-run**, because this package compiled and ran the same RTL they do, byte for
byte: no RTL file, no wrapper and no shared header used by them was touched. If
that is not wanted, the honest statement is that they were not re-run rather than
that they passed.

## 8. Not covered

* **Interrupt-driven retirement.** No interrupt input is driven. An asynchronous
  trap retires the instruction at the head of the boundary, which is a different
  gate from the synchronous path this case exercises, and this case says nothing
  about it.
* **A 1-wide configuration is not exercised, only a width-one event shape.** The
  registered profile p0 builds `MOSAIC_RETIRE_WIDTH = 2`; the width comes from the
  generated package (`tools/gen_manifest.py` writes it from the geometry file's
  `dispatch_width`), so a 1-wide *netlist* would need a different profile, and
  every profile in `config/geometry/` is 2-wide. What the case does exercise is 63
  cycles in which exactly one slot retired, so the comparison, the ordering rule
  and the gap detector are all exercised at the width-one shape of the stream.
  Running the case in a 1-wide build is a configuration change the case cannot
  make by itself, and that limit is stated rather than papered over.
* **Store and CSR identity in the delta** (§6.2): the event's own store/CSR
  fields are inert, so the case identifies a store by its retired PC and its own
  decode, and CSR writes are not compared at all.
* **No CSR architectural state is compared.** This case reconstructs the GPR
  delta; `mepc`/`mcause` are only read where the trap event needs them, and no
  CSR snapshot sequence is rebuilt. That is V-014's subject.
* **The `retire_order` counter is 7 bits wide**, so it wraps every 128 events and
  a gap of exactly 128 would look dense to the density check alone. The model
  comparison is what covers the same ground independently: it requires every
  predicted event to be published exactly once, in order, so a dropped event is
  also a PC/identity mismatch. No gap of that size is produced or injected.
* **A misaligned load is the only trapping instruction exercised**; a misaligned
  store, an instruction access fault and an illegal instruction are not, and §6.1
  is the reason one of them cannot be.
* **Single hart, M-mode, no A/F/D/V/C** was exercised; nothing about multi-hart
  retirement order, and the "no cross-hart global order from host callback order"
  rule of the plan, is tested here.
* **No waveform, no formal proof.** The claim is about the finite stimulus of this
  run, on this revision of the RTL.

## 9. Revision and reproduction

| Item | Value |
| --- | --- |
| git revision | `382baf44a321556d6a0dbd36b01312dde547fdd8` (working tree: this package's files are new and untracked) |
| RTL tree hash | `sha256(find rtl -name '*.sv' \| sort \| xargs cat) = d6461dd95eda01ca2c2772f9356c9edd9a25658dd3c76a07d8909b7a371741f7` |
| `rtl/core/mosaic_core.sv` | `6f8a9154b45c47f1f1f4456b9b02c5202045dbe733f9dd15a5f4bbc01e122ca6` |
| `rtl/core/mosaic_retire.sv` | `2745da49c116ea3846e74ebc92ef8c54c5b38a41c9d107161e9a992906c66583` |
| `rtl/core/mosaic_dispatch.sv` | `a40ce867e76a7a5097f28976cf58f7d0779f89ba9b697e26fd686406ba023acf` |
| `rtl/core/mosaic_rename.sv` | `180a1e35c4cc0a5ae5f96eb3b76fad2bc9af650f51ba6f437126c45b5c2a5d6b` |
| `rtl/core/mosaic_rob.sv` | `afba8cf6f4f954d1dfbbc39796baa2d263c8bbd0c7082b5425c8dcf39dd0d2c1` |
| `sim/tb/mosaic_core_tb.sv` | `55c543e138cca4bd528fbb0047563b54804e785fd792e69f5db5696cc2eb9efa` |
| tool | Verilator 5.052, `--x-assign unique --x-initial unique`, seed 1 |

```sh
# the case, from a clean build directory
rm -rf build/p0/unit/retire.width_and_order
python3 tools/run_unit.py --profile p0 --case retire.width_and_order

# the controls: shipping first, then one deleted-directory build per mutant, then the probes
python3 tools/run_retire_controls.py

# a single per-slot trace of any run (env-gated, off by default)
MOSAIC_RETIRE_TRACE=1 MOSAIC_RETIRE_DUMP=1 \
  build/p0/unit/retire.width_and_order/retire.width_and_order \
  --case retire.width_and_order --out /tmp/v013 --seed 1 --max-cycles 100000
```
