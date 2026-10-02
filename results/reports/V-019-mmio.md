# V-019 — independent MMIO side-effect model

Work package V-019 (`docs/validation-plan.md` §5 V-019, "实现独立 MMIO 副作用模型").
Registered case **`mmio.side_effect_model`**, top `mosaic_core_tb`, driver
`sim/unit/tb_core_mmio_model.cpp`, seed 1, budget 4,000,000 cycles.

| Artifact | Role |
|---|---|
| `sim/unit/tb_core_mmio_model.cpp` (new) | the case: the locked event script, the register-level device model, the independent RV64IM_Zicsr reference, the eight runs, the checks |
| `tools/run_mmio_model_controls.py` (new) | the controls: fourteen `-D` rebuilds from a deleted directory, twelve observable RTL/harness mutations and two weakening controls |
| `results/unit/mmio.side_effect_model/` | the observed run: `run.log`, `result.json` |
| `build/p0/mmio_model_controls/` | the control builds: one directory per mutant, its binary and `out-*` |
| `results/reports/V-019-mmio.md` | this file |

## STATUS: PASS — the case passes from a clean build, the card's coverage is measured, and all observable controls fail as required

| Check | Observed |
| --- | --- |
| the registered case, at the revision of §8 | `PASS mmio.side_effect_model` (exit 0), `checks=261` |
| runs | 8: the correct-path device contract, the cancelled-access scene, and six trap scenes |
| device side effects in the contract run | 8: two destructive-read pops, two transmit appends, three strobed scratch writes, one test-end write |
| data-port transactions in the contract run | 16 device + 10 ordinary, every one compared field by field against the reference |
| shipping binary | `sha256 f25b39cdb4dd4d4709007c9d56969ee1a2944892760f3b824218f4cc48cd4eb5` |
| controls | **14 of 14 as required**: 12 exit 1 naming the check they break (each binary differing from the shipping one, each rebuilt from a deleted directory with its `-D` in that build's command), 2 weakening controls exit 0 |
| `python3 tools/lint_rtl.py --profile p0` | `lint: 45 source file(s) clean` (**no RTL file was edited by this lane**) |
| `python3 tools/check_records.py` | `ok records agree: 55 delivered package(s), 64 registered case(s)` |
| strict C++ lint of the driver | clean under `-Wall -Wextra -Wshadow` (`make lint-cpp` also compiles the other lanes' drivers; its failure is the I-045 lane's stale `tb_core_sv39.cpp` header, not this file — §9) |

## 1. The rule the case exists to test

The card's acceptance is a statement about *side effects*, not about data:

> Every access the program makes to a device happens **exactly once**, in
> **program order**, carrying its own **address**, **width** and **byte enables**;
> a performed access's **side effect** happens exactly once and its value is the
> reference's; an access the device rejects has **no side effect** and the core
> **traps** on it with the architectural cause and tval; an access the wrong path
> **cancels** performs **no side effect at all**; and an **unmodelled device
> address is refused, never read as zero**.

The device lives on the memory system's side of the data port, so the case is
only interesting if it proves the *memory system* was used as the architecture
says — which is why the check that carries the case is the per-transaction
comparison of `(address, read/write, size, byte enables, data)`, not the values
the program publishes. A device whose read data happens to match is still a
different access if its address differs, and the case says so by name (§7).

## 2. The device model

The model is `Device` in `sim/unit/tb_core_mmio_model.cpp`. It is addressed by
*transaction*: one accepted data-port access is one device access, whatever its
width. Its whole input state is one `const LockedEvents` object — the UART
receive stream, the status word, the identity word, the scratch and timer reset
values, the exit value — and the DUT-side device and the independent reference's
device are two instances of the *same class* built from that one object. "The
same locked event is given to both sides" is therefore a property of the
construction, not a promise.

### 2.1 The register map

The p0 map's three non-idempotent regions (`config/memory/p0.json`: `uart`,
`test_harness`, `clint`) carry these registers:

| Region | Offset | Register | Access | Width | Behaviour |
|---|---|---|---|---|---|
| uart | +0x00 | RX | read | 4 | **destructive**: pops one word of the locked stream; two reads are two side effects and two values |
| uart | +0x04 | TX | write | 4 | appends the byte lanes the strobes select, in order |
| uart | +0x08 | STATUS | read | 4 | **idempotent**: two reads, one word, no state change |
| uart | +0x0C | SCRATCH | r/w | 4 | byte-addressable: a byte or halfword store changes only the bytes its strobes select |
| test_harness | +0x00 | ID | read | 4 | idempotent identity word; the window is **not writable** (§2.4) |
| clint | +0x00 | MTIME | read | 8 | idempotent locked timer value |
| clint | +0x08 | MTIMECMP | write | 8 | compare register, strobed writes |
| clint | +0x10 | EXIT | write | 4 | the test-end register: records the exit code and ends the run |

Every other address in a device region is **not modelled** and is answered with a
fault, never with a silent zero. A fault is also the answer to: an access whose
width does not fit its register (a doubleword of a four-byte register), an
unaligned access, and an address no region covers.

### 2.2 What is a "side effect"

Two logs are kept, and the case compares both against the reference:

* `accesses_` — every device access the program made, `performed` or not, with
  its address, direction, size, byte enables and data. This is the order/address/
  width/byte-enable log.
* `effects_` — only the accesses that **changed device state**: an RX pop, a TX
  append, a SCRATCH write, a MTIMECMP write, an EXIT write, each with the value
  it produced. The contract run has eight.

A read of an idempotent register is a *performed* access with **no** effect;
that separation is what lets the case require "an idempotent read changes
nothing" and "a destructive read changes something" at the same granularity.

### 2.3 The locked events

```
rx[4]      = { 0x41, 0x42, 0x43, 0x44 }   the RX stream, in order
status     = 0x0000005A
scratch    = 0x0000005A                   equal to STATUS on purpose (§7)
harness_id = 0x54455354                   "TEST"
mtime      = 0x1000                       also driven on the core's `mtime_i`
mtimecmp   = 0x2000
exit_value = 1
```

No interrupt is asserted and no random input is used: the case is deterministic
and the seed does not change its behaviour. A device with a readiness handshake
of its own is out of scope (§8); the memory's four-cycle response latency makes
the endpoint a slave that does not answer in the cycle it is asked, so the
serializer's hold is exercised under backpressure.

### 2.4 The core's store map, and a discrepancy found while building the case

A store's *address* is checked before it is allocated: `mosaic_core.sv`'s
`store_fault_kind` refuses a store whose whole extent is not inside one writable
region, so the memory never sees it. The reference implements that map because a
reference that disagreed with the platform about which stores fault would be
measuring the wrong machine.

That map says the **test_harness** window is **not writable** ("mapped and
defines no writable register; the frozen protocol's TOHOST and FROMHOST live in
RAM"). `config/memory/p0.json` says the same region is `"writable": true` with
`"error_response": "none"`, and the platform model
(`sim/common/memory_model.cpp`) faults every access there. Three statements, one
region:

| Source | test_harness writes | test_harness reads |
|---|---|---|
| `config/memory/p0.json` | allowed (`writable: true`) | allowed |
| `rtl/core/mosaic_core.sv` `store_fault_kind` | store/AMO access fault at dispatch | (no dispatch check) |
| `sim/common/memory_model.cpp` | access fault | access fault |

The case drives the RTL's behaviour: a read of `harness+0x00` is a modelled
register, a write to `harness+0x04` traps with cause 7 and no side effect. This
is reported so the integration lead can reconcile the config with the RTL; it is
not changed here because this lane is read-only on RTL. It is also why the
card's "test-end register" is modelled at `clint+0x10`: a device register in
test_harness could never be written.

## 3. The runs

Eight runs, each a full reset of the same DUT with its own memory image and its
own pair of device instances from the same locked events. Measured numbers are
from the `RESULT` line, not stated from intent:

| Run | What it covers | retires | dev txns | ordinary txns | effects | traps | cycles |
|---|---|---|---|---|---|---|---|
| `contract` | the device contract on the correct path | 51 | 16 | 10 | 8 | 0 | 255 |
| `cancelled` | a wrong-path access cancelled by a trap | 41 | 2 | 3 | 2 | 1 | 158 |
| `illegal_width` | a `ld` of a four-byte register | 25 | 2 | 3 | 1 | 1 | 95 |
| `unmapped_read` | a load no region covers | 25 | 1 | 4 | 1 | 1 | 90 |
| `unmodelled_reg` | a read of `uart+0x40` | 25 | 2 | 3 | 1 | 1 | 95 |
| `misaligned` | a `lw` at `scratch+2` | 25 | 1 | 3 | 1 | 1 | 88 |
| `unmapped_write` | a store no region covers | 25 | 1 | 3 | 1 | 1 | 87 |
| `harness_write` | a store into the readable device window | 25 | 1 | 3 | 1 | 1 | 87 |

**The contract run** does every side effect the card names on the path that must
perform it: two destructive RX reads, two idempotent STATUS reads, two
idempotent ID reads, two timer reads, a byte and a halfword strobed write to
SCRATCH, a full-word overwrite of it, two transmit bytes and the test-end write.
Every read is published, so a duplicated or dropped side effect moves the
numbers. Its checks: the two pops are the two locked values and differ; the
idempotent registers return the same word twice; the byte write at offset 3 and
the halfword write at offset 0 leave every byte their strobes do not select
alone (from the `0x0000005A` reset, `sb 0x77` at +3 then `sh 0x1234` at +0 read
back as `0x77001234`), while the word write replaces all four (`0xBEEF`); the
UART received `"AB"` in order; every published word equals the reference's; and
the ordinary traffic equals the reference's presented accesses exactly.

**The cancelled run** is a trap-squashed wrong path (the idiom
`CASE=core.trap_csr_program` documents): an iterative `div` keeps the ROB head
busy while a trapping `ecall` sits behind it, and behind the ecall are a
wrong-path ordinary load, a wrong-path device read, a wrong-path device **write**
and a second wrong-path pop. The ordinary load *does* reach memory — the check
records its read of `0x80002000`, so the window is real — while the device
accesses must not be presented, because a device access is presented only when
it cannot be squashed. The device popped once, transmitted nothing, and the
serializer refused device offers for the wrong path (`o_mem_dev_wait > 0`).

## 4. The memory port, field by field

The evidence that the device was used as the architecture says is the data-port
transaction log. The contract run's device accesses, from
`MOSAIC_MMIO_MODEL_TRACE=1` (abridged to the two strobed writes and the reads
around them):

```
[txn] cycle=154 read  addr=0x0000000002000000 size=3 wstrb=ff wdata=0x0000000000000000 dev=1
[txn] cycle=167 write addr=0x0000000080001438 size=3 wstrb=ff wdata=0x0000000000001000 dev=0   (publish)
[txn] cycle=175 write addr=0x000000000010000f size=0 wstrb=80 wdata=0x7700000000000000 dev=1   (sb +3)
[txn] cycle=183 write addr=0x000000000010000c size=1 wstrb=30 wdata=0x0000123400000000 dev=1   (sh +0)
[txn] cycle=191 read  addr=0x000000000010000c size=2 wstrb=f0 wdata=0x0000000000000000 dev=1   (lw)
[txn] cycle=203 write addr=0x0000000080001440 size=3 wstrb=ff wdata=0x0000000077001234 dev=0   (publish)
[txn] cycle=210 write addr=0x000000000010000c size=2 wstrb=f0 wdata=0x0000beef00000000 dev=1   (sw +0)
[txn] cycle=218 read  addr=0x000000000010000c size=2 wstrb=f0 wdata=0x0000000000000000 dev=1   (lw)
[txn] cycle=230 write addr=0x0000000080001448 size=3 wstrb=ff wdata=0x000000000000beef dev=0   (publish)
[txn] cycle=238 write addr=0x0000000000100004 size=0 wstrb=10 wdata=0x0000004100000000 dev=1   (TX 'A')
[txn] cycle=246 write addr=0x0000000000100004 size=0 wstrb=10 wdata=0x0000004200000000 dev=1   (TX 'B')
[txn] cycle=254 write addr=0x0000000002000010 size=2 wstrb=0f wdata=0x0000000000000001 dev=1   (test end)
```

Reading it: `size` is the port's size code (0 = byte, 1 = halfword, 2 = word,
3 = doubleword); `wstrb` is laid in the addressed doubleword window, so the `sb`
at `0x10000F` carries strobe bit 7 (= byte 3 of the register) and the `sh` at
`0x10000C` carries bits 4–5 (= bytes 0–1). The two transmits are a byte each
(strobe `0x10` = window lane 4), in program order, and the test-end write is last.
Every one of these is compared, in order, against the reference's own device log
by `CheckDeviceLog`, and the eight side effects by `CheckEffects`.

## 5. Traps and the absence of a side effect

Each trap scene resolves its access against the reference before the DUT runs,
and then requires exactly one trap event at the faulting PC and the published
`mepc`/`mcause`/`mtval` to be the reference's:

| Scene | Faulting access | Cause | Effect on the device |
|---|---|---|---|
| `illegal_width` | `ld` at `uart+0x08` (8 bytes of a 4-byte register) | 5 | the access is presented and **refused**: no side effect |
| `unmapped_read` | `lw` at `0x00100100` (no region) | 5 | the memory is consulted and faults: no device access |
| `unmodelled_reg` | `lw` at `uart+0x40` | 5 | presented and refused: **not read as zero** |
| `misaligned` | `lw` at `uart+0x0C+2` | 4 | refused before memory: the device never sees it |
| `unmapped_write` | `sw` at `0x00100100` | 7 | refused at dispatch by the store map: no device access |
| `harness_write` | `sw` at `test_harness+0x04` | 7 | refused at dispatch by the store map: no device access |

In every scene the device's effect log has exactly one entry — the test-end
write — so the faulting access performed no side effect. The `misaligned` scene
is also where the core's counters and the memory's diverge by one, and the case
states it rather than hiding it: the endpoint *accepts* the misaligned access
and then refuses it before the memory sees it, so the core's device counter is
one above the memory's address-based count. `CheckCounters` takes that expected
difference as a parameter, and it is one only in that scene.

## 6. Mutants

All built from a **deleted** build directory with the `-D` on that build's
Verilator command line, by `python3 tools/run_mmio_model_controls.py`. Shipping
binary `sha256 f25b39cdb4dd4d47…`. "differs" is the mutant binary's hash against
the shipping one.

| Mutant (`-D`) | Defect | sha256 (16) | Exit | First failure |
|---|---|---|---|---|
| `MOSAIC_CORE_MUTANT_DEV_SPECULATIVE` | the non-speculation gate is removed | `78809cde0fcffccd` | 1 | the scratch read-back is served before the store it must observe drains (`retire 38 … x28 expected 0x…beef, got 0x…1234`) |
| `MOSAIC_CORE_MUTANT_STORE_PRECOMMIT` | a store is authorised at allocation | `648f7c48de6a7bc7` | 1 | the wrong-path device store reached the device: `device 3, reference 2` |
| `MOSAIC_CORE_MUTANT_DEV_RETRY` | the held device access is not released | `7dbf2e9b0b97ec43` | 1 | `identity 4128 appears at device transactions 0 and 1` |
| `MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE` | a drain does not remove the entry | `2bdbc0e143273915` | 1 | the wedged queue is caught by the progress watchdog: `stalled` |
| `MOSAIC_SQ_MUTANT_DRAIN_OUT_OF_ORDER` | the drain takes the youngest ready entry | `f2864dcea474b049` | 1 | `max-cycles exhausted` |
| `MOSAIC_CORE_MUTANT_STORE_SIZE_WORD` | every store's allocation size is a word | `b3a2e23a1ac53265` | 1 | the strobed write changes different bytes (`retire 33 … x28 expected 0x…77001234, got 0x…1234`) |
| `MOSAIC_LSU_MUTANT_NO_STORE_SHIFT` | store data at lane zero, not the addressed lane | `df6280ef30f39f62` | 1 | the byte store writes the wrong byte (`retire 33 … expected 0x…77001234, got 0`) |
| `MOSAIC_CORE_MUTANT_DEV_AS_RAM` | the device attribute is cleared | `82910a37f81770b9` | 1 | `access to 0x…100000 (a device region) presented with device attribute 0` |
| `MOSAIC_LSU_MUTANT_DEV_ERR_OK` | a device fault is reported as a read of zero | `73509c1cc5315ce2` | 1 | `the access did not trap: the instruction at 0x…30 retires instead of trapping` |
| `MOSAIC_LSU_MUTANT_FAULT_AS_ZERO` | any fault is reported as a read of zero | `709f0d6c23993ec3` | 1 | `the access did not trap: the instruction at 0x…30 retires instead of trapping` |
| `MOSAIC_LSU_MUTANT_NO_MISALIGN_CHECK` | the misalignment check never fires | `d03346e99c9dd232` | 1 | the misaligned access is refused by the device with cause 5, not 4 (`retire 13 … x7 expected 4, got 5`) |
| `MOSAIC_MMIO_MODEL_MUTANT_STATUS_AS_SCRATCH` (driver) | a STATUS read is reported at SCRATCH's address | `8d4675df14b379cc` | 1 | `access 2 address expected 0x…100008, got 0x…10000c` |
| `MOSAIC_MMIO_MODEL_RD_ONLY` (driver) | weakening control: only the published data is compared | `e75d3ba663532012` | 0 | passes, as required |
| `MOSAIC_MMIO_MODEL_RD_ONLY` + `…STATUS_AS_SCRATCH` | the data-only comparison on the wrong-address injection | `e74be1914cb7123c` | 0 | passes, as required |

Each of the twelve observable mutants differs from the shipping binary, exits 1,
and names its check. The two weakening controls exist to show the address and
attribute comparisons are load-bearing: with them, the STATUS→SCRATCH injection
— identical read data, identical published values, only the address wrong —
**passes**, and without them it fails on the address (§7).

Two honest notes on the control set. `MOSAIC_SQ_MUTANT_DRAIN_DUPLICATE` and
`MOSAIC_SQ_MUTANT_DRAIN_OUT_OF_ORDER` break the memory path so badly that the
machine wedges (a stall and a runaway trap loop) before a second device side
effect can be observed; they are caught by the harness's no-progress watchdog and
the cycle budget rather than by the side-effect comparison. That is still a
named failure, and it is reported as such rather than dressed up.
`MOSAIC_CORE_MUTANT_DEV_SPECULATIVE`'s first failure lands in the contract run,
not the cancelled one: with the gate removed a device *load* is issued as soon as
the load queue offers it, so the contract's read-back of SCRATCH is served before
the store that must precede it has drained. The cancelled-run checks are behind
it; both are consequences of the same removed gate.

## 7. "Identical read data must not mask a wrong address"

`STATUS` and `SCRATCH` both start at `0x0000005A` from the locked events. The
driver-side control `MOSAIC_MMIO_MODEL_MUTANT_STATUS_AS_SCRATCH` *reports* each
STATUS read at SCRATCH's address while still performing the real read, so:

* the value the program reads is identical (`0x5A`), and every published word is
  identical — a data-only comparison cannot see anything wrong;
* the device access log differs at address `0x…100008` vs `0x…10000c`, and the
  full case fails with exactly that address named.

`MOSAIC_MMIO_MODEL_RD_ONLY` is the paired weakening control: with it the driver
drops the access-log, attribute and counter comparisons and keeps only the
published-data comparison, and the injection then **passes**. The two rows
together are the evidence that the address comparison — not read data — is what
holds the case up.

## 8. Not covered

* **The test_harness region's writability is inconsistent across the config, the
  RTL and the platform model** (§2.4). This case drives the RTL's behaviour and
  reports the discrepancy; reconciling `config/memory/p0.json` with
  `mosaic_core.sv` is the integration lead's call, not this lane's.
* **No interrupt is asserted.** The card's "flush 中 speculative MMIO" is
  exercised by a trap redirect, not by an asynchronous interrupt. I-038's report
  names the interrupt path around an in-flight device access as untested and
  says V-019 is where it should be measured; this case does not assert
  `irq_soft/timer/ext`, so that gate remains argued from structure. It is the
  obvious next addition to this file (the locked event script is already the
  place for it).
* **No device with its own readiness handshake.** The model answers within the
  memory's fixed latency; a real UART that stalls until it is ready is out of
  scope. The four-cycle latency does exercise the serializer's hold.
* **The `test_harness` write is refused before memory by the core's map, so the
  device model cannot itself confirm the refusal** — the trap and the absence of
  a transaction are the evidence.
* **`clint` mtime is a single locked value, not a counting timer.** The timer's
  *read* is idempotent by construction; a timer whose value advances between two
  reads is not modelled, because the reference could not reproduce cycle-varying
  time deterministically.
* **Instruction fetch is not routed through this device model** (the core's
  fetch port is separate). A device used as an instruction source is not covered.
* **Only the p0 map is covered.** The three regions are p0's; p1/p2/p3 have their
  own maps.
* **The device model is a harness model, not a UART/CLINT adapter.** The
  registers live in `tb_core_mmio_model.cpp`; a real device, a real transport and
  a timeout/watchdog for a non-responding device are not part of p0.

## 9. Gates

| Command | Result |
|---|---|
| `python3 tools/run_unit.py --profile p0 --case mmio.side_effect_model` | PASS, exit 0, `checks=261` |
| `python3 tools/run_mmio_model_controls.py` | all 14 controls as required (12 exit 1, 2 exit 0) |
| `python3 tools/lint_rtl.py --profile p0` | `lint: 45 source file(s) clean` |
| `python3 tools/check_records.py` | `ok records agree: 55 delivered package(s), 64 registered case(s)` |
| strict C++ lint of `sim/unit/tb_core_mmio_model.cpp` (`-Wall -Wextra -Wshadow`) | clean |
| `make lint-cpp` | **fails on another lane's file**: `sim/unit/tb_core_sv39.cpp` does not match the current `Vmosaic_core_tb.h` (it names `ptw_xl_valid_i` etc.; the wrapper was re-shaped by the I-045 lane). This file is not implicated. |

The tree was unbuildable twice while this package was in flight — first an
unconnected `xl_attr_o` on `mosaic_ptw`, then an unconnected `sys_ins_src2_val`
on `mosaic_dispatch` (with an implicit 1-bit net at `mosaic_dispatch.sv:998`) —
both from sibling lanes' mid-landing edits. The case was developed and first
verified against the pinned revision of §10 (`git archive HEAD`); the final run
recorded here is on the live tree after those lanes connected their ports, and
the controls were re-run there.

## 10. RTL and wrapper revisions this run was measured against

The tree carries sibling lanes' uncommitted work, so these are the *live* file
hashes, not commit ids:

| File | sha256 |
|---|---|
| `rtl/core/mosaic_core.sv` | `4f4c3b9eef95b29c63f074655278caccb51eb2da8e6cfaee3a21272d21c58ef4` |
| `rtl/core/mosaic_dispatch.sv` | `b34a034144d7b0b437ae0341d45b98dd1cb4d46b372a15b49a58991e62293f0b` |
| `rtl/core/mosaic_lsu_endpoint.sv` | `1f65126344e31ef6d7c3c8c0012393fbb9fbc91125f63b1d55679741e091b889` |
| `rtl/core/mosaic_load_queue.sv` | `f84636263e0ac20f59fa48f5c6b0857ac8c0970b6666c3cdc9e88e8f1a16cc88` |
| `rtl/core/mosaic_store_queue.sv` | `1c3cc4d6e3751d78ff13d1bf968b18bb288a049cea786d91286f79be86b0661b` |
| `rtl/core/mosaic_pkg.sv` | `215588f22cde73b8f47123dd294548adf274ba92908829b871ff58e27fbf1c3e` |
| `rtl/core/mosaic_uop_pkg.sv` | `9fcc53339f6d276eabbfe6e6fc82591dd4bae94c35aca9f2ce152f31ba82d4a7` |
| `rtl/core/mosaic_ptw.sv` | `4cc2995a3eef659d3a059ec201d5d04d2c7a5d86975615ce9d86b2d5c43b6130` |
| `sim/tb/mosaic_core_tb.sv` | `2b5991e40e39e5eb1942df1fd3d72cfa692f0cd6b45ccf908fbd9d966f3a480d` |

The pinned revision first used for verification is `git` HEAD
`7cb1b5d` (the commit that registered this case).

## 11. How to reproduce

```sh
# the case (builds all eight runs, prints one RESULT line)
python3 tools/run_unit.py --profile p0 --case mmio.side_effect_model

# shipping and the fourteen controls, each from a deleted build directory
python3 tools/run_mmio_model_controls.py
python3 tools/run_mmio_model_controls.py --only DEV_SPECULATIVE

# the device accesses, side effects and traps of every run
MOSAIC_MMIO_MODEL_TRACE=1 \
  ./build/p0/unit/mmio.side_effect_model/mmio.side_effect_model \
  --case mmio.side_effect_model --out /tmp/mmio --seed 1 --max-cycles 4000000

# the gates this lane is responsible for
python3 tools/lint_rtl.py --profile p0
python3 tools/check_records.py
```

## 12. Files

New: `sim/unit/tb_core_mmio_model.cpp`,
`tools/run_mmio_model_controls.py`, `results/reports/V-019-mmio.md`.
No RTL file was edited; `tests/unit/registry.json` and
`config/status/implementation_status.json` are the integration lead's and were
not touched.
