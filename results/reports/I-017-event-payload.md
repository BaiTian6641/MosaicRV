# I-017 — the event payload: `LWU`, the store/CSR identity, and the system-trap event

Work package I-017 (`docs/implementation-plan.md`, the retirement boundary and the
CSR side effects; `docs/validation-plan.md` §5). This package fixes the three
defects V-013 found and reported without fixing
(`results/reports/V-013-retire.md` §6.1–§6.3) and adds the registered case that
makes them checkable:

| Artifact | Role |
|---|---|
| `rtl/core/mosaic_decoder.sv` | defect 1: `LWU` (LOAD funct3 `110`) implemented; `MOSAIC_DECODER_MUTANT_LWU_ILLEGAL` restores the refusal |
| `rtl/core/mosaic_retire.sv` | defect 3: a trap the system unit resolved publishes its lane-0 event; `MOSAIC_RETIRE_MUTANT_SYS_TRAP_NO_EVENT` removes it |
| `rtl/core/mosaic_core.sv` | defect 2: the retire payload bus wired from the store queue and the CSR file; `MOSAIC_CORE_MUTANT_NO_STORE_PAYLOAD`, `MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG`, `MOSAIC_CORE_MUTANT_NO_CSR_PAYLOAD` |
| `sim/tb/mosaic_core_tb.sv` | the wrapper's event ports for the CSR and trap groups, and the store address/data that were left unconnected |
| `sim/tb/mosaic_retire_tb.sv` | the retire unit's three new trap inputs, exposed and left at zero by the unit case |
| `sim/unit/tb_core_event.cpp` | **CASE=`core.event_payload`**: the program, the assembler that evaluates what it emits, and the field-for-field comparison |
| `sim/unit/tb_decoder.cpp` | **CASE=`decode.rv64im_reserved`** extended: `LWU` is legal, funct3 `111` is the only reserved load |
| `tools/run_event_controls.py` | the six controls, each rebuilt from a deleted directory |
| `config/contracts/event_v1.json` | reconciliation F-6: the finding, and the producer the integrated core now has |
| `results/unit/core.event_payload/` | the observed run: `run.log`, `result.json`, `controls.log` |
| `build/p0/event_controls/` | the control builds: `build_command.txt` and the binary of each mutant, plus `out-*/` |

## STATUS: PASS — the case passes from a clean build, six controls fail as required, every affected case re-run green

| Check | Observed |
| --- | --- |
| the registered case | `PASS core.event_payload` (exit 0) |
| | `checks=27 comparisons=484 seed=1`, from `events=64 retires=63 stores=9 csr=8 traps=1 dual_store=1 cycles=232` |
| shipping binary | `sha256 9de5e2fe5e07a21d65e1bcbae4a1c6539d177a3d5b0b9eb9eeb161e5e8ed32d2` |
| the extended decoder case | `PASS decode.rv64im_reserved` — `166734 instructions (82915 legal, 83819 illegal), 0 mismatches, 1442 named reserved checks` |
| controls | **6 of 6 exit 1**, each naming the check it breaks, each binary differing from the shipping one, each rebuilt from a deleted directory (§5) |
| RTL gates | `tools/lint_rtl.py --profile p0` green (33 files); `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` 0 errors |
| `tools/check_event_contract.py --profile p0` | green — `21 fields, 100 octets/record`; the pending list is still `ev_insn, ev_pc_after, ev_trap_epc` (F-3) |
| `tools/check_event_contract.py --profile p0 --negative` | green — **33/33 illegal interfaces rejected**, unchanged by F-6 |
| `python3 tools/check_records.py` | green (37 packages, 49 cases) |
| the cases the edits could affect | re-run and PASS: `fabric.fixed_two_cluster`, `core.corpus_branch`, `core.unwritten_reg_read`, `core.mem_program`, `core.trap_csr_program`, `fence.code_and_data_order`, `retire.head_block_and_dual`, `retire.width_and_order`, `decode.rv64im_reserved`, `alu.boundaries`, `lsu.byte_forwarding`, `lsu.size_fault_boundaries`, `store.wrong_path_visibility` (§7) |

## 1. `LWU` (LOAD funct3 `110`) was refused as a reserved encoding

**Reproduction.** V-013's program contained `lwu x0, 0(t3)` and the machine
stopped on it: `unsupported=1 illegal=1 stopped=1`. `rtl/core/mosaic_decoder.sv`
decoded funct3 `0,1,2,3,4,5` in the `OP_LOAD` arm and treated everything else as
illegal, with a comment claiming `110` and `111` are both reserved. That is wrong
for `110`: RV64I defines `LWU`, the project's own reference model implements it
(`sim/unit/trap_ref.h`, `case 0x6u: size = 4; sign = false`), and so does the ISA.

**Fix.** funct3 `110` decodes as `mem_size = SZ_WORD`, `mem_signed = 0` — the
same 32-bit access as `lw` with no sign extension into the upper half. funct3
`111` remains illegal, and the comment now says which of the two is reserved and
why. `MOSAIC_DECODER_MUTANT_LWU_ILLEGAL` puts the refusal back.

**Before/after through the integrated core** (`MOSAIC_DECODER_MUTANT_LWU_ILLEGAL`
on the `core.event_payload` program, which contains an `lwu`):

```
before:  cycle 85: unsupported=1 illegal=1 stopped=1 (head_pc=0x80000078 ... retired=30)
after:   the same program retires the lwu and reaches its exit protocol
```

**Coverage.** `decode.rv64im_reserved` owns "which encodings are reserved", so it
carries the fix: the reference decoder in `sim/unit/tb_decoder.cpp` decodes funct3
`110` as legal, the named check for `LWU` is the *first* thing the case does (so a
regression fails with the instruction named rather than with the first field the
sweep disagrees about), the memory-width table pins `LWU`'s size and signedness,
and only funct3 `111` is left in the reserved class. The legal population moved
from 82384 to 82915 instructions and the reserved checks from 1443 to 1442 (one
class of two encodings became one encoding, plus four named `LWU` checks).

**End to end.** A decoder that accepts the encoding is not enough: a load path
that sign-extended it would still be wrong. The `core.event_payload` program
stores `0xDEADBEEF` (bit 31 set) and loads it back with `lwu`, and the case
asserts the retiring value is `0x00000000DEADBEEF` — the independent interpreter
(`sim/unit/trap_ref.h`) computes the same value, so the per-instruction comparison
covers it twice.

## 2. The event stream carried no store or CSR identity

**Reproduction.** `mosaic_core.sv` tied the retire instance's `pay_is_store`,
`pay_store_addr`, `pay_store_data`, `pay_store_size`, `pay_csr_we`,
`pay_csr_addr` and `pay_csr_value` to constant zero, so `ev_store`,
`ev_store_size` and `ev_csr_*` were always zero even though
`config/contracts/event_v1.json` declares them. V-013 measured it: 0 of its 11
retiring stores carried the flag while the store queue authorised exactly those
11 (its §6.2). This is the F-4-adjacent gap the freeze recorded — with the
important distinction that F-4 is about `MEM_VISIBLE` and this is about the
retirement record's own memory group (§6).

**Fix — the store group.** The store queue exports its entries (`o_entry_pay`)
and authorises them in order, so the committing store is the entry at the
authorisation watermark (`sq_auth_cnt`) and the second lane's is the entry after
it. The core reads that entry's `base`, `imm`, `data` and `size` and presents
`address = base + imm` — the sum the memory endpoint's single adder forms, so the
event cannot describe a different store from the one that reached memory. The
validity bits are the queue's own (`addr_valid`, `data_valid`) plus its
`commit_ok`/`commit2_ok`, and every field of the group is **zero unless that lane
really is the store being authorised**, so the schema's validity rule holds as a
property of the bus rather than as a rule a consumer has to remember to apply.

**Fix — the CSR group.** A CSR macro is staged at the ROB head and its staging
entry is freed at the end of the very cycle it retires, so in that cycle the
entry still names it and the CSR file still answers for its address. The value
presented is the CSR file's own read-back, one cycle after the write strobe: the
write landed at the completion edge, so the register already holds what the
instruction wrote. That is the architectural value — WARL canonicalisation
included — taken from the file that owns the register, not a second copy of the
write-operation rule in the core.

**Before/after.** `retire.width_and_order` (V-013's own case, whose driver was
*not* edited) prints its observation on every run:

```
before:  [reported defect] the retire event's store-identity field carried 0 bit(s) (11 retiring stores decoded from the image)
after:   [reported defect] the retire event's store-identity field carried 11 bit(s) (11 retiring stores decoded from the image)
```

and `core.event_payload` now compares each of the 9 store and 8 CSR payloads
field by field (§3).

## 3. The case: `core.event_payload`

`sim/unit/tb_core_event.cpp`, top `mosaic_core_tb`, seed 1, budget 4,000,000
cycles (the run takes 232). One hand-assembled program produces every shape the
schema distinguishes, and no file under `tests/programs/` is touched.

| # | What the program does | What the case requires |
| --- | --- | --- |
| 1 | installs the handler in `mtvec` with a `csrrw` | the trap vector's own write is a CSR event, with the handler address as its value |
| 2 | seven CSR writes on `mscratch`: `csrrw`, `csrrs`, `csrrc`, `csrrwi`, `csrrsi`, `csrrci` | `ev_csr_we = 1`, `csr_addr = 0x340`, and `csr_value` equal to the register's post-write value (0x1234 → 0x12F4 → 0x12C4 → 7 → 0x1F → 0x1E), computed by the driver's own Zicsr model |
| 3 | two CSR instructions that do **not** write (`csrrs rd, csr, x0`) | `ev_csr_we = 0` and `csr_addr`/`csr_value` **zero** |
| 4 | stores of all four sizes (`sd`/`sw`/`sh`/`sb`) at distinguishable addresses with distinguishable data | `ev_store = 1`, `mem_addr`, `mem_size`, and `mem_data` byte-exact over exactly `mem_size` bytes |
| 5 | a 64-bit datum built by a shift (`0x7FFFFFFFFFFFFFFF`) | a payload truncated to 32 bits cannot pass |
| 6 | a load between the stores, and an `lwu` | a load retirement carries **no** store payload (`ev_store = 0` and all three fields zero) and the `lwu`'s value is zero-extended |
| 7 | a `div` holding the head for ~64 cycles, one filler, then `sh` and `sb` | **one cycle retires a store in lane 0 and a store in lane 1** with *distinct* payloads — the per-lane rule, which a single snapshot cannot satisfy |
| 8 | `ecall` | a lane-0 TRAP record whose `cause`/`tval` equal the trap entry's own ports **in the same cycle** and whose `pc_before` is the entry's `epc`; and no register, CSR or store payload on it |
| 9 | the exit protocol (`sd` of the pass code to `MOSAIC_TOHOST`) | the exit store is itself a store event, and the run ends with `RESULT PASS` |

The expectation is not transcribed. The assembler *evaluates* every instruction
as it emits it — register values, `rs1 + imm`, the stored datum, the size, and
the CSR file's post-write value under the profile's WARL rules — so the
comparison tables are a second implementation of the ISA's addressing and Zicsr
rules computed from the program's own text. The trap entry applies its own rule
to that model (`mepc = the trapping PC`) before the handler is assembled, so the
handler's CSR write is evaluated as the program will execute it.

Two further opinions close the loop:

* `sim/unit/trap_ref.h`, an independent RV64IM_Zicsr interpreter with its own
  memory, predicts every retirement (`pc`, `rd`, `reg_we`, `value`) and every
  trap entry (`cause`, `epc`); the DUT's stream is compared with it in order, and
  the case requires every predicted retirement to have been published;
* the end state: `mscratch`, `mepc` and `mtvec` read out of the core must equal
  the values the driver's model computed, and the data region of the platform
  memory must equal the reference's **byte for byte**.

Observed run (`results/unit/core.event_payload/run.log`):

```
  [store] cycle=65  lane=0 pc=...068 addr=0x80000800 data=0x00000000deadbeef size=3
  [store] cycle=67  lane=0 pc=...06c addr=0x80000808 data=0x00000000deadbeef size=2
  [store] cycle=69  lane=0 pc=...070 addr=0x80000810 data=0x00000000deadbeef size=1
  [store] cycle=71  lane=0 pc=...074 addr=0x80000818 data=0x00000000deadbeef size=0
  [store] cycle=90  lane=0 pc=...080 addr=0x80000828 data=0x00000000000000ff size=3
  [store] cycle=92  lane=0 pc=...090 addr=0x80000840 data=0x7fffffffffffffff size=3
  [store] cycle=162 lane=0 pc=...0c4 addr=0x80000830 data=0x000000000000abcd size=1
  [store] cycle=162 lane=1 pc=...0c8 addr=0x80000838 data=0x0000000000001234 size=0   <- the dual-retire pair
  [store] cycle=219 lane=0 pc=...0e4 addr=0x80001000 data=0x0000000000000001 size=3   <- the exit store
  [csr]   cycle=16  lane=0 pc=...010 addr=0x305 value=0x80000200
  [csr]   cycle=26  lane=0 pc=...024 addr=0x340 value=0x1234
  [csr]   cycle=30  lane=0 pc=...02c addr=0x340 value=0x12f4
  [csr]   cycle=34  lane=0 pc=...034 addr=0x340 value=0x12c4
  [csr]   cycle=38  lane=0 pc=...038 addr=0x340 value=0x0007
  [csr]   cycle=42  lane=0 pc=...03c addr=0x340 value=0x001f
  [csr]   cycle=46  lane=0 pc=...040 addr=0x340 value=0x001e
  [csr]   cycle=183 lane=0 pc=...208 addr=0x341 value=0x800000d0  <- the handler's mepc write
  [trap]  cycle=164 lane=0 pc=0x800000cc cause=0x000000000000000b tval=0x0
core.event_payload: events=64 retires=63 stores=9 csr=8 traps=1 dual_store=1 post_exit=0 cycles=232
RESULT PASS core.event_payload checks=27 comparisons=484 seed=1
```

The dual-retire shape is produced deliberately (a divide holds the head while
the two stores behind it complete, and one filler takes the divide's second
lane), so its absence would be a fact about the program rather than a silent gap
— and the case asserts it happened.

## 4. The system-trap event

**Reproduction.** With `ecall`, the DUT asserts the trap port but emits no lane-0
event, so a consumer sees the PC jump to the handler with nothing explaining it —
contradicting `mosaic_retire.sv`'s own header that a trap is an event in its own
right. ROB-recorded exceptions (a misaligned load) publish one; the system path
did not.

**Fix.** `mosaic_retire` takes the trap the system unit resolved as a *statement*
from the core (`sys_trap_valid`/`cause`/`tval`) in the cycle it is taken, merges
it with the buffer's own exception bit into one `trap_now`, and publishes the same
lane-0 TRAP record either way — the same `ev_valid`/`ev_trap`/`ev_seq` path, the
same `pc_before` from the ROB head, and the cause/tval the trap entry publishes.
Nothing here recomputes whether a trap should happen: the core decides, the
module reports. `trap_flush` and the trap handoff ports follow the merged signal,
so the module's own contract ("a trapping instruction at the head is
architecturally final") holds for both sources; the integrated core does not
consume either, and its trap controller is unchanged.

The record carries no register, CSR or store payload: the trap lane is not a
retired lane, and the retire unit gates every payload field on that, which is the
schema's `trap_rule` enforced by construction rather than by review.

**Identity.** The event's `pc_before` is the ROB head's PC, which for a
synchronous trap *is* the trap's `epc` — the schema's own `epc_is_not_pc` rule
says so, and `ev_trap_epc` is still a declared-but-absent port (F-3), so the case
checks the event's `pc_before` against the entry's `o_trap_epc_o` rather than
growing the frozen schema. The event's `cause`/`tval` are checked against
`o_trap_cause_o`/`o_trap_tval_o` **in the same cycle**.

## 5. Controls

`python3 tools/run_event_controls.py` (exit 0), output kept as
`results/unit/core.event_payload/controls.log`. Each row's build directory was
deleted before the build, its `-D` is in that build's own `build_command.txt`,
its binary differs from the shipping one, and it exits 1 naming the check.

| Mutant | Case | Defect it injects | First failure it produced | Binary sha256 |
|---|---|---|---|---|
| `MOSAIC_CORE_MUTANT_NO_STORE_PAYLOAD` | `core.event_payload` | the store payload tied off again (the shipped wiring, V-013 §6.2) | `the store payload is present exactly for a store instruction: pc=0x80000068 ev_store=0` (cycle 65) | `6d4a7000a6514f405d45da44097b5ae8f82e9990d5b96f3c175356138c1d7a63` |
| `MOSAIC_CORE_MUTANT_STORE_SIZE_WRONG` | `core.event_payload` | every store reported as a word | `store event 0 carries mem_size: pc=0x80000068 size=2 expected 3` (cycle 65) | `defe443695e4d98b3ac918fbaf6787b2878cdb40e18b47f0a13559fa2a098e4f` |
| `MOSAIC_CORE_MUTANT_NO_CSR_PAYLOAD` | `core.event_payload` | the CSR payload tied off again | `the CSR payload is present exactly for a CSR instruction that writes: pc=0x80000010 ev_csr_we=0` (cycle 16) | `6fe8012425f708f38887cadd8b73e4b14182c0d7351518abc26ed2fbe297ea76` |
| `MOSAIC_RETIRE_MUTANT_SYS_TRAP_NO_EVENT` | `core.event_payload` | a system trap publishes no event (V-013 §6.3) | `exactly one trap event was published, for the ecall: trap events=0` (cycle 232) | `6446f1b9aa960dc8680b78fb1fb876291b32dcafb6c59077e1c02a73a3af6b99` |
| `MOSAIC_DECODER_MUTANT_LWU_ILLEGAL` | `core.event_payload` | `LWU` refused again, seen from the integrated core | `the machine never stopped on a refused macro: unsupported=1 illegal=1 stopped=1` (cycle 85 — V-013's own observation) | `3d538e5772a6b3ec3a2bf0259c6191c84c35c980293337b6e5298df2da41b9e5` |
| `MOSAIC_DECODER_MUTANT_LWU_ILLEGAL` | `decode.rv64im_reserved` | the same defect, at the unit that owns the encoding | `LWU (LOAD funct3 110) is a legal encoding` and `insn=0x00816083.valid: expected 0x1, got 0x0` | `e54804dbbb480d229ffd8ad15909da66f17deba11f382247796f1b306fe3be11` |

Shipping binaries: `core.event_payload`
`9de5e2fe5e07a21d65e1bcbae4a1c6539d177a3d5b0b9eb9eeb161e5e8ed32d2`,
`decode.rv64im_reserved`
`3b988139166b571e665ba548c32887414f4d88c90e153522e8a99c7f1e0cd297`.

The `LWU` control is built twice on purpose: the encoding is owned by
`mosaic_decoder.sv`, so the case that owns "which encodings are reserved" must
catch it, and the integrated core must also be shown to stop on the instruction
the way V-013 observed.

## 6. What each fix changed about the machine, and the schema's F-4 note

* The decoder accepts one more encoding (LOAD funct3 `110`) and the machine
  executes it; `decode.rv64im_reserved`'s legal population grew accordingly.
* The retire event now carries a store's and a CSR write's own identity through
  the integrated core. `ev_store`, `ev_store_size`, `ev_csr_we`, `ev_csr_addr`
  and `ev_csr_value` are real, and the store address/data were wired out of the
  core for the first time (`ev_store_addr`, `ev_store_data` were unconnected in
  the wrapper).
* A trap the system unit resolves now publishes a lane-0 TRAP record with the
  same cause/tval/identity as the trap entry.
* **`MEM_VISIBLE` still has no DUT-side producer.** The fix produces the
  *retirement* record's memory group (kind RETIRE, `mem_is_store` 1). The schema's
  `memory_rule` and `store_drain_is_separate` require the store becoming visible
  to the memory system to be a **second** record of kind `MEM_VISIBLE`, and
  nothing in the implemented p0 path emits one: the memory endpoint still has no
  event tap, which is exactly F-4, and F-4 is unchanged. `config/contracts/event_v1.json`
  gained **F-6** recording the finding and the producer the integrated core now
  has; F-4's verdict ("a DUT-side producer is a declared gap") stands.

## 7. Cases re-run

Every case the edits could affect was re-run after the final RTL state, each from
its own build:

| Case | Result |
| --- | --- |
| `core.event_payload` | PASS (checks=27 comparisons=484) |
| `decode.rv64im_reserved` | PASS (166734 instructions, 0 mismatches) |
| `retire.head_block_and_dual` | PASS — the retire unit's new inputs are driven to zero by its own wrapper, so its behaviour is unchanged |
| `retire.width_and_order` | PASS — and its store-identity observation moved from 0 to 11 bits (§2) |
| `fabric.fixed_two_cluster` | PASS — its program has no stores and no CSR writes, so its `!is_trap && !is_store` assertion is untouched |
| `core.unwritten_reg_read` | PASS |
| `core.mem_program` | PASS |
| `core.trap_csr_program` | PASS — the corpus traps are memory faults, and its directed program's ECALL/EBREAK/illegal-CSR traps now also publish a lane-0 record, which its stream comparison skips as it always skipped trap events |
| `fence.code_and_data_order` | PASS |
| `core.corpus_branch` | PASS |
| `alu.boundaries` | PASS |
| `lsu.byte_forwarding`, `lsu.size_fault_boundaries`, `store.wrong_path_visibility` | PASS |

`core.corpus_sweep` (another lane's driver and report) was **not** run and not
edited: this package changed RTL, so that lane's revision is invalidated by
design and it re-runs on its own.

## 8. A pre-existing defect found on the way (and fixed, because it blocked this package's case)

`decode.rv64im_reserved` did **not build** on the tree this package started from:

```
%Warning-WIDTHTRUNC: sim/tb/mosaic_decoder_tb.sv:85:21: Operator ASSIGNW expects
134 bits on the Assign RHS, but Assign RHS's VARREF 'ctl' generates 135 bits.
%Error: Exiting due to 1 warning(s)
```

`mosaic_pkg::decode_ctl_t` grew a field (`is_wfi`, added by the core integration,
which recognises WFI in the front end) and neither the wrapper's flattened
`o_ctl_bits` nor the driver's bit accounting was updated. Verified to be
pre-existing: the same failure occurs with `git show HEAD:rtl/core/mosaic_decoder.sv`
in place of the edited file. Since this package owns that driver for the `LWU`
fix and the case has to build to be extended, the accounting was completed:
`is_wfi` is now exported and pushed, and the total is 135 bits (the top word's
mask widened from `0x3F` to `0x7F`). This is a *reported* defect that was fixed
rather than hidden, and it is worth a second look by whoever owns the decoder
wrapper: a case that cannot build is not a case that passes.

## 9. Not covered

* **`MEM_VISIBLE` has no DUT-side producer** (§6). The store's *visibility* is
  still only observable as final memory; the schema's F-4 gap is unchanged.
* **An asynchronous interrupt still publishes no lane-0 event.** The hook this
  package added to `mosaic_retire` takes the trap the *system unit* resolved, as
  the card asked; an interrupt is a third source of `trap_decision` and was left
  alone, because the card named the system path and this package did not want to
  change the interrupt path another lane's case (`core.trap_csr_program`) covers.
  The same one-line hook would cover it, and the gap is stated rather than
  implied: a consumer watching only the event stream still sees a PC jump with no
  record when an interrupt is taken.
* **`ev_trap_epc` is still a declared-but-absent port (F-3).** The trap event's
  `epc` is expressed through `pc_before`, which the schema says equals it for a
  synchronous trap; an *asynchronous* trap's `epc` is not expressible at all
  until the port exists. `ev_insn` and `ev_pc_after` remain absent too, and the
  pending list is unchanged.
* **`mtval` for a system illegal-instruction trap is 0.** The core's
  `trap_tval` is `head_exc_trap ? exc_tval_head : 0`, so an illegal CSR access
  (cause 2) publishes `tval = 0` where the ISA asks for the instruction's
  encoding. The event now reports the same value the trap entry uses — the two
  agree, which is what this case checks — but the value itself is a neighbouring
  defect this package did not take on.
* **CSR values are read back, not recomputed.** The event's `csr_value` is the
  CSR file's read-back in the retirement cycle. For the registers this program
  uses (mtvec, mscratch, mepc) that is the architectural value; a CSR whose write
  is forwarded to another unit rather than stored in the file (`mip`) would
  report that unit's current value, and no case here exercises it.
* **`mem_data` above `mem_size` is not constrained.** The case compares exactly
  the bytes `mem_size` says are the effect, which is the schema's meaning; the
  bytes above it are carried through from the store queue and are not asserted.
* **One profile, one program, one hart, seed 1, no interrupts, no waveform, no
  formal proof.** The claim is about the finite stimulus of this run on this
  revision of the RTL.

## 10. Revision

| Item | Value |
| --- | --- |
| files this package edited | `rtl/core/mosaic_decoder.sv`, `rtl/core/mosaic_retire.sv`, `rtl/core/mosaic_core.sv`, `sim/tb/mosaic_core_tb.sv`, `sim/tb/mosaic_retire_tb.sv`, `sim/unit/tb_decoder.cpp`, `config/contracts/event_v1.json` |
| files this package created | `sim/unit/tb_core_event.cpp`, `tools/run_event_controls.py`, this report |
| not touched | `tests/unit/registry.json` (`core.event_payload` keeps its `"pending": true`), `config/status/implementation_status.json`, `results/PROGRESS.md`, `tests/programs/**`, any other lane's case driver or report |
| tool | Verilator 5.052, `--x-assign unique --x-initial unique`, seed 1 |
| reproduction | `python3 tools/run_unit.py --case core.event_payload`, `--case decode.rv64im_reserved`, `python3 tools/run_event_controls.py` |
