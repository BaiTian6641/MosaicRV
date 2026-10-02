# I-038 — non-speculative MMIO

Status: **registered case passes; four controls fail as required; gates green.**

    CASE=mmio.exactly_once  ->  RESULT PASS mmio.exactly_once
        checks=69 runA_retires=34 runA_dev_txn=5 runA_ram_txn=6 runA_dev_hold=8
        runB_retires=39 runB_dev_txn=1 runB_ram_txn=4 runB_dev_hold=1 runB_dev_wait=60
        runA_cycles=137 runB_cycles=157 seed=1   (exit 0)

Shipping binary `sha256 = 3aaea510933903ce76ea101fabee2f83140d5d26a8aa8d1c2cc667acdb588046`.

---

## 1. The rule, stated as a rule

A **device address** is an address in a region `config/memory/p0.json` marks
*not idempotent*: `uart`, `test_harness` and `clint`. Those three regions are
exactly what `mosaic_uop_pkg::is_device_addr` names, through the generated bases
and sizes of `mosaic_cfg_pkg`; `boot_rom` and `ram` are idempotent and are
ordinary memory. One function, called by the module that orders an access and by
the two queues that decide whether a store may feed a load, so "which addresses
are devices" cannot differ between them.

For every device access, the device serializer between the queues and
`mosaic_lsu_endpoint` applies one rule with three clauses:

> **1. Non-speculative.** A device access is presented to the memory system only
> when it is *irreversible-safe*. For a **load** that means the access is the ROB
> head at a legal boundary — no redirect in flight, not recovering, not stopped —
> so every older instruction has already committed and nothing can squash it. For
> a **store** it is the store queue's own contract: a store is offered for drain
> only after the instruction that owns it has retired.
>
> **2. Exactly once.** The transaction is *owned* by the serializer from the
> cycle its requester is told "accepted" until its response is consumed: it is
> taken into one register, presented to the endpoint once, and released when the
> endpoint takes it. The requester cannot re-offer it (it has been accepted), so
> a replay cannot repeat the side effect — the store queue's and the load queue's
> own exactly-once rules are extended, not weakened. A device transaction is
> never replayed by the load queue either, because a replay re-issues the request
> and for a device that *is* a second side effect.
>
> **3. Not coalesced with the ordinary path.** A device access is never merged
> with ordinary traffic, and it is presented with an attribute that says so. The
> store queue's forwarding query skips device entries (they are neither a source
> for a load nor a blocker for one — their bytes are in a region no ordinary load
> can alias), the load queue's byte search skips them, and a device load's bytes
> are forced to come from the device response. The attribute the memory system
> sees (`o_mem_dmem_dev`) is the same predicate, so a coalescer or cache that
> keys on it cannot merge an MMIO access with RAM.

A device access is also a barrier for the ordinary path while it is held: the
endpoint port is not offered to anything else, so no younger access can overtake
it. It is never the other way round — the queue mux gives the store drain
priority, and every store older than the head has already drained — so the order
the memory system sees is program order.

### 1.1 Why a register, and what it buys

The endpoint already accepts one transaction at a time, so a purely combinational
gate would have been shorter. The register is what makes clause 2 a property of
the *transaction* rather than of a cycle: the classification, the identity and
the owner travel with the held request, so none of them can change between the
decision and the access. It also costs exactly one cycle per device access, which
is the only latency this work package adds (see §3.3).

### 1.2 What the rule deliberately does not do

* It does not serialize *ordinary* accesses. An idempotent load passes straight
  through and keeps speculative issue, which is what the two-run comparison in §3
  shows.
* It does not re-derive authorisation. A store reaches the serializer only
  through the store queue's drain port, whose contract is "authorised and
  material only"; the serializer relies on that rather than duplicating the
  watermark.
* It does not wait for the *fence* drain. A device access is ordered against
  older access by the queue mux and the store-drain priority, not by a drain of
  the whole memory path.

---

## 2. How it is wired in

* `mosaic_uop_pkg::is_device_addr` — the PMA predicate (§1), beside
  `size_bytes` and `expected_wstrb` in the same package, so the queues and the
  serializer share one definition.
* `mosaic_core.sv`, section 14 — **the serializer** is the stage between the
  queue request mux and `mosaic_lsu_endpoint`. It computes `is_device_addr(base +
  imm)`, takes a device offer into `ser_hold_q` when the clause-1 predicate holds
  (`ser_nonspec_c`), presents the held transaction to the endpoint, and forwards
  an ordinary offer combinationally. `ser_out_dev_c`, the owner and the identity
  are latched with the transaction and drive the endpoint's `req_dev_i`.
* `mosaic_lsu_endpoint.sv` — carries the attribute beside the frozen `lsu_req_t`
  as the scalar `req_dev_i` (the packed interface is not changed), latches it
  into its transaction and exports `o_txn_dev`/`o_txn_id`, so a testbench reads
  the attribute and the identity of the access the data port is *carrying*, in
  the cycle it accepts it.
* `mosaic_load_queue.sv` — the store-queue view is classified with the same
  predicate; a device store is excluded from the byte search, a device load's
  bytes are forced to come from the device, and a device load is completed by its
  one response and never replayed.
* `mosaic_store_queue.sv` — the forwarding query skips device entries, as neither
  source nor blocker.
* `mosaic_core.sv` — the FENCE/FENCE.I drain rule (`mem_path_idle`) and the
  reported `o_mem_lsu_busy` now include the serializer, so a fence cannot retire
  with a device access taken but not yet accepted by the endpoint; and
  `core_can_trap` is gated on the serializer being idle, so an interrupt cannot be
  taken while a device access is in flight (see §6).

Observability added for the case: `o_mem_dev_txn`, `o_mem_ram_txn`,
`o_mem_dev_wait`, `o_mem_dev_hold`, `o_mem_dmem_dev`, `o_mem_dmem_id`, and a
debug bundle `o_dbg_mmio` (renamed `o_dbg_mmio_o` in the wrapper).

**Where the serializer lives.** In `mosaic_core.sv` rather than in a new module
file, because the case's registered source list (`tests/unit/registry.json`,
`mmio.exactly_once`) names the RTL it compiles and must not be edited by a
package's author. Section 14 already hosts the memory path's integration logic;
the serializer is one more stage of it, with its block comment stating the rule.

---

## 3. The case

`CASE=mmio.exactly_once`, driver `sim/unit/tb_core_mmio.cpp`, top
`mosaic_core_tb`. Two hand-encoded programs run from the reset vector, each after
a full reset, each with its own memory image and its own device model. Instruction
fetch and data access read and write **one shared memory**, so a trap handler
reached through `mtvec` is assembled by the same program.

**The device model is this file's, on the memory system's side of the data
port**, and it is addressed by *transaction*, not by byte: one accepted access is
one side effect whatever its width. That is the granularity the rule is stated
at, and why a duplicated transaction shows up as a second pop rather than as
eight byte reads.

* `kFifo = 0x100000` (uart region) — a read pops a queue and returns the value;
  the four values are distinct, so a duplicated or dropped pop moves the numbers
  the program publishes.
* `kUart = 0x100008` (uart region) — a write appends a byte and is counted.
* `kErr = 0x2000000` (clint region) — any access is answered with `fault`: the
  error response the core must trap on.

The memory answers with a **four-cycle latency**, so the endpoint is a slave that
does not respond in the cycle it is asked and the serializer's hold is exercised
across that window instead of always being accepted the cycle after the take.

### 3.1 Run A — the correct path, and the error device's trap

Two FIFO reads (each one pop) and two UART writes, every popped value published;
then `mtvec` is installed and a load from the error device must raise a load
access fault with the faulting PC in `mepc`, cause 5 in `mcause` and the address
in `mtval`. The handler publishes all three and ends the run. The fall-through —
taken only if the access does *not* trap — publishes a marker instead, so "the
error was swallowed" is a named failure and not a hang. Because nothing in this
run speculates past a completed instruction, its ordinary RAM traffic must be
exactly the reference's; the case checks that too, which is the "device rule, not
a global freeze" half of run B's comparison.

### 3.2 Run B — the wrong path

An iterative `div` keeps the ROB head busy for sixty-odd cycles while a trapping
`ecall` sits behind it, and two loads are dispatched behind the `ecall`: an
ordinary RAM load first, then a FIFO read. **Nothing here waits on a branch** —
this profile's dispatch barrier stops allocation behind an unresolved branch
(`mosaic_core.sv`: `br_inflight`), so a branch cannot create this window at all,
while a trap is not a branch. The RAM load is the load queue's head, so it is
issued and performed before the `div` retires, the `ecall` traps and both are
flushed; the FIFO read behind it is the wrong-path *device* access and must not
be presented, because a device access is presented only at the ROB head.

**Expectations, none from the DUT:** (i) the per-instruction retirement stream is
compared against an independent RV64IM interpreter in the same file, which
decodes the same words from the ISA text, executes loads and stores against its
own memory, models the FIFO, UART and error device itself, and models both traps
(load access fault, ecall from M) rather than reading anything from the core;
(ii) the device classification is the profile's map written out in this file;
(iii) the published mepc/mcause/mtval are compared against the *model's* trap.

**What is checked:** the retirement stream in order including every value; that
every access carries the attribute its address's region demands; that every
device transaction has a distinct identity; that the device performed exactly the
retired pops/writes/errors and the memory system saw exactly that many device
accesses; that the core's device/RAM counters partition the transactions and
agree with the address-based counts; that the trap is precise (one trap event, the
right PC, the right published mepc/mcause/mtval); and, in run B, that the
wrong-path ordinary load reached memory while the wrong-path device load did not,
with the serializer refusing the offer until the redirect.

### 3.3 Measured numbers (not stated from intent)

| Quantity | Run A | Run B | Where it comes from |
|---|---|---|---|
| retirements | 34 | 39 | the retire event stream, compared with the reference |
| device transactions | 5 | 1 | the memory's transaction log **and** `o_mem_dev_txn` |
| ordinary transactions | 6 | 4 | the memory's transaction log **and** `o_mem_ram_txn` |
| cycles device transactions spent held | 8 | 1 | `o_mem_dev_hold` |
| cycles a device offer was refused (not the ROB head) | 0 | 60 | `o_mem_dev_wait` |
| run cycles | 137 | 157 | the harness's cycle counter |

Run A: 5 device accesses cost 8 hold cycles, so the serializer's register
boundary contributes **at least one cycle per access** (the minimum is one: a
device offer taken in cycle *T* is presented from *T+1*), and the surplus is the
endpoint being busy with an older access — the hold under backpressure. Run B:
the serializer refused device offers for 60 cycles. The dominant contributor is
the wrong-path load, which is offered from the moment it is the load queue's head
and refused until the `ecall` flushes it; a handful of those cycles belong to the
one legitimate pop while it was still behind the two `auipc`/`addi` that precede
it, so the counter is "refused device offers", not "refused wrong-path offers" by
construction. The only device transaction is that legitimate pop, and the
ordinary traffic is one transaction above the reference's retired RAM accesses —
the wrong-path RAM load, which did run.

Identity values observed on the port (from `MOSAIC_MMIO_TRACE=1`), all distinct:

    run A:  [dev] cycle= 16 addr=0x1000000 we=0 id=2064
            [dev] cycle= 29 addr=0x1000008 we=1 id=5160
            [dev] cycle= 43 addr=0x1000000 we=0 id=11352
            [dev] cycle= 62 addr=0x1000008 we=1 id=15480
            [dev] cycle= 72 addr=0x2000000 we=0 id=21672
            [trap] cycle=81 pc=0x80000054 cause=0x5
    run B:  [dev] cycle= 16 addr=0x1000000 we=0 id=2064   (the one legitimate pop)
            [trap] cycle=113 pc=0x80000070 cause=0xb      (the ecall, not the device)

---

## 4. Mutants

All built from a **deleted** build directory with the `-D` on that build's
Verilator command line, by `python3 tools/run_mmio_controls.py`. Shipping binary
`sha256 = 3aaea510933903ce…`.

| Mutant (`-D`) | Defect | sha256 (first 16) | Exit | First failure |
|---|---|---|---|---|
| `MOSAIC_CORE_MUTANT_DEV_SPECULATIVE` | the non-speculation gate is removed, so a device load is issued as soon as the load queue offers it; the FIFO read the `ecall` discards is performed for an instruction that never retires | `2bc22ee61e31ccfe` | 1 | `the device performed exactly the retired FIFO pops: model 2, reference 1` |
| `MOSAIC_CORE_MUTANT_DEV_RETRY` | the serializer keeps the device transaction after the endpoint has taken it, so the same access is offered — and performed — again | `efbf3dd1935092be` | 1 | `every device transaction has a distinct identity: identity 2064 appears at device transactions 0 and 1` |
| `MOSAIC_LSU_MUTANT_DEV_ERR_OK` | a fault response to a device access is reported as a successful read of zero, so the error response never traps | `c5acb7e7d00c192f` | 1 | `the error device's response did not trap: the instruction at 0x0000000080000054 retires instead of trapping` |
| `MOSAIC_CORE_MUTANT_DEV_AS_RAM` | the attribute presented to the memory system is cleared: the access is still serialized correctly, but it is presented as an ordinary access — the coalescing attribute | `3109f1235fdf671c` | 1 | `every access carries the device attribute its region demands: access to 0x0000000000100000 (a device region) presented with device attribute 0` |

Each binary differs from the shipping one, each exits 1 on its own named first
failure, and each mutation is a single defect in the shipping expression it
replaces.

**A control that is not claimed.** A *combinational ready loop* between the
serializer's gate and the queues. The serializer's upstream `ready` is a function
of its own register, its own classification and the **endpoint's** ready — and
the endpoint's ready is a function of its own state machine
(`req_ready_o = (state_q == ST_IDLE)`), never of `req_valid_i`. There is
therefore no path from a request back to its own acceptance to loop, and a mutant
cannot exhibit a loop the structure does not contain. The property is structural,
not tested: it is stated here instead of being claimed as a control.

---

## 5. Defect found while building the case

**The response owner was recorded at the wrong handshake.** `mosaic_core` records
which queue an endpoint response belongs to in `ep_owner_q`, and it did so on the
queues' offer being *taken* (`ep_req_valid && ep_req_ready`). That was correct
while `ep_req_ready` was the endpoint's own ready, because the endpoint only
accepts when it is idle — that is, after the previous response has been consumed.
The serializer broke that invariant for device requests: it tells the requester
"accepted" as soon as the access is irreversible-safe, *without* consulting the
endpoint. With the endpoint still serving an older transaction, the owner bit
flipped early and the older transaction's response was delivered to the new
owner.

It is a real defect at any memory latency above one cycle, and the case's
four-cycle latency is what exposed it: run A's second FIFO read completed with
the *previous store's* response and returned 0 instead of the queue's second
value (`retire 11 at 0x8000002c value for x28 expected 0x22222222…, got 0`). The
debug bundle printed `lq_rsp_valid=1 ep_rsp_valid=1 ep_owner=0` on the cycle the
store's response arrived, with the store's owner bit already cleared.

Fixed by recording the owner when the **endpoint** accepts the transaction it is
presenting (`ser_accept_c`), with the owner latched with the held transaction
(`ser_owner_q`) so it travels with the request. This is a device-path integration
defect, recorded here rather than in I-033/I-034/I-035 because this case found it.

---

## 6. NOT verified

* **The interrupt path around a device access is implemented but not
  exercised.** `core_can_trap` is gated on the serializer being idle, because an
  interrupt taken at the head while a device access is in flight would retire
  *nothing* for that access — the side effect would have happened for an
  instruction that never commits, and the response would be discarded. This case
  drives no interrupts (`irq_soft/timer/ext` are held low), so the gate is
  argued from the structure and from the code, not measured. An interrupt-driven
  MMIO case (V-019's destructive-read and speculative-MMIO items) is the place to
  measure it.
* **A device access that is coalesced with RAM by store-to-load forwarding is
  unreachable in this profile**, so the exclusion in the load and store queues is
  a guard rather than a measured behaviour. The reason is the rule itself: a
  device load is issued only at the ROB head, at which point every older store
  has retired and drained, so there is no older covering store to forward from;
  regions are disjoint, so no RAM access can alias a device access either. The
  mutant that would exhibit a forwarding merge therefore cannot be observed here,
  and the observable control for clause 3 is the attribute mutant above. The
  exclusions become load-bearing the day a coalescer or a cache exists (V-061,
  V-062), which must key on the attribute this case now checks.
* **`o_mem_dmem_id` is checked for uniqueness and counted, not attributed
  instruction by instruction.** The retire event carries a PRF identity
  (`{tag, gen}`), not the ROB identity a device transaction carries, so "no
  device transaction belongs to an instruction that never retires" is checked via
  the *count* (a wrong-path access shows up as an extra pop) rather than by
  mapping each transaction to a retiring instruction. Adding that mapping needs a
  retire-side identity probe, which this package does not add.
* **A device that never responds hangs the memory path.** The serializer owns a
  device transaction until its response is consumed and holds the ordinary path
  while it does; there is no timeout or watchdog in p0. The case's memory always
  responds, so this is stated rather than tested.
* **The wrong-path window is created by a trap, not by a branch**, because the
  dispatch barrier makes a wrong-path memory access behind an unresolved branch
  impossible in this profile. That is a property of the conservative recovery
  (I-018's saved-map controller is what lifts the barrier); when it is lifted,
  the device gate is the second line of defence and this case's run B is the
  shape to re-run for a branch-squashed access.
* **Only the p0 map is covered.** `is_device_addr` names the three non-idempotent
  regions of `config/memory/p0.json` through the generated constants; p1/p2/p3
  have their own maps and are not checked here.
* **The device is a harness model, not a real UART/CLINT adapter.** The
  FIFO/UART/error registers live in `sim/unit/tb_core_mmio.cpp`; the card's
  "UART/timer adapter" is modelled on the memory system's side, and a real
  device with its own readiness handshake is not part of this profile.

---

## 7. Gates

| Command | Result |
|---|---|
| `python3 tools/lint_rtl.py --profile p0` | 34 source files clean |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | exit 0; only the pre-existing STYLE notices (`u_sq` prefix in `mosaic_core.sv`, one blocking assignment in `mosaic_bringup_core.sv`) |
| `python3 tools/check_records.py` | records agree: 38 delivered packages, 49 registered cases |
| `python3 tools/run_unit.py --profile p0 --case mmio.exactly_once` | PASS, exit 0 |
| `python3 tools/run_mmio_controls.py` | all four observable controls OK |

Regression cases re-run from this tree and passing: `fabric.fixed_two_cluster`,
`core.corpus_branch`, `core.unwritten_reg_read`, `core.mem_program`,
`core.trap_csr_program`, `core.corpus_sweep`, `fence.code_and_data_order`,
`lsu.byte_forwarding`, `lsu.size_fault_boundaries`, `store.wrong_path_visibility`
(and, in the same batch, `iq.wakeup_insert_select`, `wb.same_bank_many_producers`).

---

## 8. How to reproduce

```sh
# the case (builds both runs, prints one RESULT line)
python3 tools/run_unit.py --profile p0 --case mmio.exactly_once

# shipping and the four mutants, each from a deleted build directory
python3 tools/run_mmio_controls.py
python3 tools/run_mmio_controls.py --only DEV_SPECULATIVE

# the device transactions and traps of both runs
MOSAIC_MMIO_TRACE=1 ./build/p0/unit/mmio.exactly_once/mmio.exactly_once \
    --case mmio.exactly_once --out /tmp/mmio --seed 1 --max-cycles 4000000
# ... and the memory-path state every cycle (what the hold/owner debug used)
MOSAIC_MMIO_CYCLES=1 ./build/p0/unit/mmio.exactly_once/mmio.exactly_once \
    --case mmio.exactly_once --out /tmp/mmio --seed 1 --max-cycles 4000000

# the gates
python3 tools/lint_rtl.py --profile p0
slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)
python3 tools/check_records.py
```

Files touched: `rtl/core/mosaic_uop_pkg.sv`, `rtl/core/mosaic_core.sv`,
`rtl/core/mosaic_lsu_endpoint.sv`, `rtl/core/mosaic_load_queue.sv`,
`rtl/core/mosaic_store_queue.sv`, `sim/tb/mosaic_core_tb.sv`,
`sim/tb/mosaic_lsu_endpoint_tb.sv`, `sim/tb/mosaic_load_queue_tb.sv`,
`sim/tb/mosaic_store_queue_tb.sv`, `sim/unit/tb_lsu.cpp`,
`sim/unit/tb_core_mmio.cpp` (new), `tools/run_mmio_controls.py` (new).
