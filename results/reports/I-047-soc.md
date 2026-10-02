# I-047 — SoC interconnect and error completion

Task card: `docs/implementation-plan.md` §I-047 (line 727) and
`docs/stage-3-memory-system.md` §I-047 (line 394). Case: `soc.bus_errors_and_ids`
(`tests/unit/registry.json`, still marked `"pending": true` — the integration
lead clears that when recording). This package is additive: it adds
`rtl/soc/mosaic_soc.sv` and instantiates no core module, so it cannot regress
any other registered case.

Files owned by this deliverable:

| File | What it is |
|---|---|
| `rtl/soc/mosaic_soc.sv` | the interconnect and its peripherals — one self-describing module |
| `sim/tb/mosaic_soc_tb.sv` | `mosaic_soc_tb`, the Verilator top: one `mosaic_soc` instance, all ports exposed |
| `sim/unit/tb_soc.cpp` | `CASE=soc.bus_errors_and_ids`, the campaign and the independent counters |
| `tools/run_soc_controls.py` | the four negative controls (mutants) of §8 |
| `results/reports/I-047-soc.md` | this file |

Toolchain actually used: **Verilator 5.052 2026-09-05**, macOS/arm64
(Darwin 25.6.0), Python 3.9.6. Working tree `git rev-parse HEAD` =
`56ed95c6e4a0e14fe60c763919c0544c058379ce`.

Compiled source revisions:

| File | SHA-256 |
|---|---|
| `rtl/soc/mosaic_soc.sv` | `6d6c1573e864e6fa5a564c9ef685f4b7d41625419c6f8b70b32ebe44243e372f` |
| `sim/tb/mosaic_soc_tb.sv` | `d90be3de5e9465b1dbe13928617784cae42329f826fe082ada2f59311e308afa` |
| `sim/unit/tb_soc.cpp` | `fe1a16c766fe40eacc1702c939c6d6cf6159f4fa72640c7ddfeeb9e78e1cdd3f` |
| `tools/run_soc_controls.py` | `0278f887b8004ceb0582872b25a238f53ce844f3c2756cd7e438db2462a38721` |

Shipping test binary `sha256 =
afc06da876826c0322070ac5c8e67cd2b6e9f396aa2c70103c52dbf2748e6d00`.

---

## 1. The interface

Two requester ports. Each is exactly the port `mosaic_lsu_endpoint.sv` presents
to the memory system, `mem_req_t`/`mem_rsp_t` field for field, plus the
transaction identity the endpoint publishes beside that port (`o_txn_id`):

```systemverilog
module mosaic_soc #(
  parameter int unsigned ID_W            = 4,
  parameter int unsigned MAX_OUTSTANDING = 4,
  parameter bit          HAS_MSIP        = 1'b0,
  parameter logic [63:0] MSIP_BASE       = 64'h00000000000C0000
) (
  input  clk, rst,
  // mX_req_we/size/wstrb/wdata/addr  == mem_req_t.we/size/wstrb/wdata/addr
  // mX_req_amo                        == mem_req_t.amo
  // mX_req_id                         == the endpoint's published o_txn_id
  // mX_rsp_rdata/fault                == mem_rsp_t.rdata/fault
  // mX_rsp_id                         == the same identity, returned
  input  m0_req_valid, output m0_req_ready, input m0_req_we,
  input  [63:0] m0_req_addr, input [2:0] m0_req_size, input [7:0] m0_req_wstrb,
  input  [63:0] m0_req_wdata, input m0_req_amo, input [ID_W-1:0] m0_req_id,
  output m0_rsp_valid, input m0_rsp_ready, output [63:0] m0_rsp_rdata,
  output m0_rsp_fault, output [1:0] m0_rsp_err, output [ID_W-1:0] m0_rsp_id,
  … m1_* identical …,
  input  stall,                       // the memory system's backpressure line
  input  rom_load_en, [8:0] rom_load_index, [63:0] rom_load_data,
  input  uart_rx_push, [7:0] uart_rx_data, output uart_rx_full,
  output uart_tx_valid, [7:0] uart_tx_data,
  output irq_timer, irq_soft, irq_ext, exit_valid, [31:0] exit_code,
  // observability
  output [31:0] o_accepted_ctr, o_completed_normal_ctr, o_completed_error_ctr,
                o_outstanding_ctr, o_uart_rx_pop_ctr, o_uart_tx_ctr,
                o_clint_exit_ctr, o_dec_err_ctr, o_slv_err_ctr,
  output o_conservation_ok, o_abort_pulse, o_id_mismatch
);
```

`mX_rsp_err` is the error class: `0` OK, `1` SLVERR, `2` DECERR. `mX_rsp_fault`
is `mX_rsp_err != 0`; the core's endpoint turns a fault into a **load access
fault (cause 5)** for a load/LR and a **store access fault (cause 7)** for a
store/AMO/SC, decided by its own `we` — so "an access fault of the right class"
is the endpoint's mapping of this one bit, not a second signal here.

**What is not carried.** `amo_op`, `aq` and `rl` are not ported. The endpoint
presents an atomic read-modify-write as *two beats* on this port (`ST_REQ` and
`ST_AMO_W`, both with `amo = 1`) and computes the arithmetic itself with
`mosaic_amo_alu`, so a memory system that receives the port never acts on
`amo_op`; and this fabric serves one requester per port in the order it is
offered, so there is no reordering for `aq`/`rl` to constrain. Both are
consequences of the one-requester-per-port structure, not omissions. An AMO beat
aimed at a device region is refused with SLVERR (atomic access to a device
register is not supported by this platform).

## 2. The rules

### 2.1 One completion per accepted request — the conservation identity

A request is **accepted** on an edge where `mX_req_valid && mX_req_ready`. It is
then owned by one transaction-table entry until its completion is **consumed** on
an edge where `mX_rsp_valid && mX_rsp_ready`. Four registered counters move with
those two events, and the identity

```
accepted == completed_normal + completed_error + outstanding
```

is exported combinationally as `o_conservation_ok` and required to hold **every
live cycle** by the case. The fabric's four counters are also exported so the
driver can compare them against its own independent count of the same two
handshakes every cycle (`counter agreement`). "Every accepted request receives
exactly one completion" is therefore checked three ways: the identity, the
independent counter comparison, and an end-of-phase drain that requires
`outstanding == 0` and every issued identity to have been consumed exactly once.
A counter leak is the card's fail mode ("a response error is dropped and the ROB
waits for ever"); the drain is what names it.

### 2.2 Identity (ID) preservation

The requester supplies `mX_req_id`. It is latched into the table entry, carried
to the slave port, returned on the slave response, and presented on the
**originating** requester's port with that same id. A slave that returns a
different id than it was given raises the sticky `o_id_mismatch` (a defect, not a
liberty). The delivery scan routes each completion by the entry's `master` field;
two requesters can complete in the same cycle, and the driver checks that no
completion is delivered to a requester that did not issue it.

### 2.3 Byte strobes preserved

`wstrb` and the lane-aligned `wdata` are carried unchanged from the requester to
the slave. The RAM merges only the strobed lanes; the device registers (UART
scratch, timer compare, MSIP, test-end) apply the same rule, with the register
byte index taken from the offset inside the register window and the data lane
from the access's address lane — the lane-aligned convention
`mosaic_lsu_endpoint.sv` documents ("the byte at `addr` is lane `addr[2:0]`").
The UART scratch and the timer compare are the proof: a byte store at scratch+1
changes one byte of a 32-bit register, and the value read back is positioned at
the access's lane.

### 2.4 Backpressure, and no combinational ready loop

`mX_req_ready` is low exactly when the transaction table is full; a slave's
request port is free only when that slave is not already answering (`stall_i`
holds every dispatch, which is how the memory system's own backpressure is
modelled). In both cases the offered payload is held unchanged and accepted
exactly once, later. **No `ready` is a function of the same port's `valid`** —
`free_c` is a function of `txn_valid`, the arbiter's choice is a function of the
round-robin bit and the *other* requester's valid, and a slave's availability is
a registered bit — so there is no path from an acceptance back to its own offer
and no combinational ready loop to exhibit. The case checks the consequence
either way: a request offered while the table is full is held and then accepted
once, and a completion held by a low `rsp_ready` is stable and consumed once.

### 2.5 Error mapping: unmapped, DECERR and SLVERR

| Situation | Completion |
|---|---|
| Address no region covers (`DECERR`) | `mX_rsp_err = 2`, `fault = 1`, `rdata = 0`, produced the cycle the request is accepted (no slave is involved, and none is waited for) |
| Write to the read-only boot ROM (`SLVERR`) | `mX_rsp_err = 1`, `fault = 1`, nothing written |
| Write to the read-only test-harness window (`SLVERR`) | `mX_rsp_err = 1`, `fault = 1` |
| Device access that is misaligned, crosses a register boundary, or names a register the device does not implement (`SLVERR`) | `mX_rsp_err = 1`, `fault = 1`, no side effect |
| AMO beat aimed at a device region (`SLVERR`) | `mX_rsp_err = 1`, `fault = 1` |

Neither class is ever dropped: an error is a completion. `o_dec_err_ctr` counts
DECERR completions as they are accepted; `o_slv_err_ctr` counts SLVERR
completions as they are consumed.

## 3. Reset with a transaction in flight — the specified behaviour

Reset is synchronous. It clears the transaction table, the per-slave busy
registers and all four counters **together**, so the conservation identity
survives the reset unchanged (accepted and outstanding fall by the same amount).
A transaction that was in flight when `rst` asserted is therefore **aborted**:

1. it is discarded — the table is empty after reset and no completion is
   delivered for it, ever (the case checks `m0_rsp_valid == 0` after the reset);
2. the loss is *reported*, not hidden — `o_abort_pulse` is high in the cycle
   reset is asserted while `outstanding != 0`;
3. the counters clear together, so the identity holds again from the first
   post-reset cycle, and the fabric is immediately usable (a fresh request
   completes normally).

The requester is expected to be reset with the fabric — a hart does not survive
the reset that resets its SoC — so no completion is owed across the boundary.

## 4. The platform map implemented

The decode is the union of `config/memory/p0.json` and `config/memory/p1.json`
(p1 is p0 plus the CLINT MSIP word), taken from the generated `mosaic_cfg_pkg`
constants so the RTL and the C++ harness read one source of truth:

| Region | Base | Size | Kind | Decode |
|---|---|---|---|---|
| `boot_rom` | `0x0000_0000` | 4 KiB | ROM | read-only; loaded through a load port; a write is SLVERR |
| `uart` | `0x0010_0000` | 256 B | device | RX/TX/STATUS/SCRATCH |
| `test_harness` | `0x0010_2000` | 16 B | device | identity register, reads only |
| `clint` | `0x0200_0000` | 4 KiB | device | mtime / mtimecmp / test-end |
| `ram` | `0x8000_0000` | 2 MiB | RAM | byte-strobed read/write |
| `clint_msip` | `0x000C_0000` | 4 B | device | p1 only; set by `HAS_MSIP` |

The one thing the RTL cannot read from the JSON is whether the profile declares
the MSIP word, so `mosaic_soc_tb` sets `HAS_MSIP` from
`mosaic_cfg_pkg::MOSAIC_PROFILE_INDEX` (0 for p0; 1 for p1; the constant was
checked against every file in `config/memory/`). A p0 build therefore decodes
`0x000C_0000` as unmapped — DECERR — exactly as p0's map says, and the case
checks whichever of the two maps the profile declares. The unmapped-address test
uses `0x0010_0100` (the gap between `uart` and `test_harness`) and
`0x4000_0000`, which are unmapped in both profiles.

## 5. The peripherals

The register-level contract is the one the V-019 model
(`sim/unit/tb_core_mmio_model.cpp`) was written to; this module is the first real
device implementation of it. All accesses must be aligned to their size and lie
inside one register window; anything else is SLVERR and performs nothing. A
read has a side effect only where the platform says so (the UART RX pop).

| Device | Offset | Width | Read | Write |
|---|---|---|---|---|
| UART RX | `+0x00` | 32 | pops one queued input word (one side effect) | SLVERR |
| UART TX | `+0x04` | 32 | SLVERR | transmits the strobed bytes; one side effect per accepted access |
| UART STATUS | `+0x08` | 32 | bit0 = RX not empty, bit8 = TX ready; no side effect | SLVERR |
| UART SCRATCH | `+0x0C` | 32 | the register, lane-positioned | byte-strobed register write |
| harness ID | `+0x00` | 32 | `0x54455354` ("TEST") | SLVERR |
| CLINT MTIME | `+0x00` | 64 | the free-running counter | SLVERR (read-only) |
| CLINT MTIMECMP | `+0x08` | 64 | the compare value | byte-strobed register write |
| CLINT EXIT | `+0x10` | 32 | SLVERR (write-only) | sets the test-end code; one side effect |
| MSIP | `+0x00` | 32 | the pending word | byte-strobed; bit 0 is the software-interrupt pending bit |

**Side effects are exactly-once by construction.** A table entry is dispatched to
a slave at most once (the dispatch sets `txn_inflight`, cleared only when the
slave's response is captured), and each slave's side effect happens in the cycle
it is dispatched; the fabric never re-issues an entry and never replays a
response.

**Interrupts** are the platform's device lines, driven directly:
`irq_timer = (mtime >= mtimecmp)`, `irq_soft = msip[0]` (p1+),
`irq_ext = (RX not empty)`.

**A disagreement recorded rather than hidden.** Three existing models disagree
about the device register offsets and about `test_harness` writability
(`config/memory/*.json` says writable, the core's own PMA store map refuses it,
and the models differ among themselves). This SoC implements V-019's
register-level map and the core's rule that the harness window is read-only (a
write is SLVERR). That choice is stated here rather than left implicit; see §10.

## 6. The case

`CASE=soc.bus_errors_and_ids`, top `mosaic_soc_tb`, driver `sim/unit/tb_soc.cpp`.
Each named requirement is a phase; each phase drives the DUT and asserts on its
results. The driver is not the DUT's oracle: it keeps its own issued/completed
records and its own counters, and every identity/data/error check compares
against what it issued.

| Phase | What it establishes |
|---|---|
| `reset` | after reset the counters are zero and no completion is presented |
| `id-preservation` | two requesters issue interleaved requests with distinct ids; every completion is delivered on the port that issued it carrying that id (2 then 3 interleaved requests) |
| `strobe-narrow` | a byte store at scratch+1 changes one byte of the 32-bit UART scratch, read back lane-positioned; the same on a wide RAM word (8-byte store then byte store) |
| `backpressure` | the table fills to exactly 4; a fifth offer is refused (`ready = 0`) and then accepted once when the stub stall releases; a completion held by a low `rsp_ready` is presented and consumed once |
| `unmapped` | a read and a write to addresses no region covers are DECERR completions with zero data; the DECERR counter records exactly two |
| `slverr` | ROM write, harness write, unmodelled UART register, and a misaligned device access are SLVERR; the harness id reads TEST; the refused ROM write changed nothing; the SLVERR counter records exactly four |
| `reset-in-flight` | a request accepted but not dispatched is aborted by reset; `o_abort_pulse` reports it; outstanding and the counters clear together; no completion is delivered later; a fresh request then completes |
| `mmio-exactly-once` | four RX reads pop four distinct words in order; a STATUS read pops nothing; three TX writes are three side effects and three pulses; the timer compare reads back what was written; the test-end write is one side effect with the written code |
| `interrupts` | the timer line follows `mtime >= mtimecmp`; the external line follows RX; the MSIP line (where declared) is asserted and cleared by the word |

Measured (recorded `results/unit/soc.bus_errors_and_ids/result.json`):

| Quantity | p0 | p1 |
|---|---|---|
| verdict | PASS | PASS |
| checks / failures | 48 / 0 | 49 / 0 |
| accepted / normal / error | 15 / 14 / 1 | 17 / 17 / 0 |
| DECERR / SLVERR (post-reset window) | 1 / 0 | 0 / 0 |
| UART rx pops / tx writes / test-end writes | 4 / 3 / 1 | 4 / 3 / 1 |
| cycles | 187 | 199 |

The counts are the *post-reset* window: the `reset-in-flight` phase resets the
counters (that is the behaviour under test), and the `unmapped` and `slverr`
phases check their own before/after deltas before that reset. The p0 `error = 1`
is the MSIP probe's DECERR (p0 declares no MSIP word); p1 has none.

## 7. Conservation-identity results

* `o_conservation_ok` was high in **every live cycle** of both runs
  (`conservation checks > 0`, `conservation_failed == 0`).
* The fabric's four counters equalled the driver's independent count in every
  live cycle (the `counter agreement` check).
* At the end of every phase and at the end of the campaign,
  `outstanding == 0` and every issued identity had been consumed exactly once
  (`duplicate completion` and `id routing` are the failures that catch the
  alternatives).

## 8. Negative controls — the mutants

`tools/run_soc_controls.py` rebuilds the case with exactly one defect injected
through a single `-DMOSAIC_SOC_MUTANT_*` define, from a deleted build directory,
with the define on the recorded command line
(`build/p0/soc_controls/<mutant>/build_command.txt`). Each binary differs from
the shipping one and exits 1 naming the check it breaks:

| # | Define | Injected defect (card's Fail mode) | First failure | Binary `sha256` |
|---|---|---|---|---|
| 0 | *(shipping)* | none | `RESULT PASS …` | `afc06da876826c0322070ac5c8e67cd2b6e9f396aa2c70103c52dbf2748e6d00` |
| 1 | `MOSAIC_SOC_MUTANT_DROP_ERROR` | a peripheral (SLVERR) completion is dropped instead of delivered — *"a response error is dropped and the ROB waits for ever"* | `FAIL slverr: completion for requester 0 id 0: expected the completion arrives (an accepted request is never dropped), got absent after 2000 cycles` | `9d1cc7ed4c4bf30f7c2c44730ca978671d34798156da6685a3fa7cb757a54c22` |
| 2 | `MOSAIC_SOC_MUTANT_ID_SWAP` | the completion is delivered on the other requester's port — *"a response delivered to the wrong requester"* | `FAIL id routing: expected a completion for an id issued by requester 0 is delivered to requester 1, got requester 1 was delivered id 0 instead` | `23d5d8923feaa7992a9ca1317ca284ef897f6cf1417beebf012340c394d5ace9` |
| 3 | `MOSAIC_SOC_MUTANT_STROBE_DROP` | a device register write ignores the access's byte position and takes its data from lane 0 — *"a byte strobe dropped on a narrow write"* | `FAIL strobe-narrow: UART scratch read-back: expected 0xaabb11dd00000000, got 0x0000000000000000` | `85317c6fa6e4b24286c0555ea25d4314cc4a00c76ba8896e962e77168d920f0d` |
| 4 | `MOSAIC_SOC_MUTANT_DUP_COMPLETE` | a consumed completion is not retired, so it is delivered again — *"a duplicated completion"* | `FAIL duplicate completion: expected every accepted request completes exactly once, got requester 0 was delivered id 0 a second time` | `cc14e00ca0564643d3b6be10d532feb00d42c0bbdfe683a0db9bccd4f2fd5094` |

Each expected string is chosen so it appears only in the failure message, never
in a phase banner; a mutant cannot pass by printing the phase name. Mutant 1 is
caught by the completion liveness check (the accepted request never completes);
mutant 2 by the routing check (a requester that did not issue the id); mutant 3
by the narrow-write read-back; mutant 4 by the exactly-once check.

## 9. Gates

| Gate | Result |
|---|---|
| `make check` | **PASS** (exit 0) |
| `python3 tools/lint_rtl.py --profile p0` | **PASS** — `lint: 49 source file(s) clean`, including `rtl/soc/mosaic_soc.sv` |
| `python3 tools/lint_rtl.py --profile p1` | **PASS** — `lint: 49 source file(s) clean` |
| `make lint-slang` | **PASS** (exit 0; only the project-wide `STYLE-2` port-suffix warnings every module emits) |
| `python3 tools/check_records.py` | **PASS** — `62 delivered package(s), 69 registered case(s)` |
| `python3 tools/check_exclusions.py` | **PASS** |
| `python3 tools/check_exclusions.py --negative` | **PASS** — `13/13 illegal ledgers rejected` |
| `CASE=soc.bus_errors_and_ids` (p0, clean build) | **PASS** — 48 checks, 0 failures, 187 cycles |
| `PROFILE=p1 CASE=soc.bus_errors_and_ids` | **PASS** — 49 checks, 0 failures, 199 cycles |
| `python3 tools/run_soc_controls.py` | **PASS** — all 4 mutants caught |
| `CASE=mmio.exactly_once` | **PASS** (I-038) |
| `CASE=core.mem_program` | **PASS** (I-023) |
| `CASE=core.corpus_sweep` | **PASS** (I-023) |
| `CASE=store.wrong_path_visibility` | **PASS** (I-034) |
| `CASE=lsu.byte_forwarding` | **PASS** (I-035) |
| `CASE=lsu.size_fault_boundaries` | **PASS** (I-033) |
| `python3 tools/run_act_dut.py --no-generate` (p1) | **PASS** — `ran=127 passed=127 expected=127` |
| `make lint-cpp` | **RED, not from this package** — 7 errors from `sim/unit/tb_core_fp.cpp` (`Vmosaic_core_tb` members the FP lane's wrapper does not declare). `sim/unit/tb_soc.cpp` compiles with zero diagnostics of its own. |

The case builds and passes from a deleted build directory (both the runner and
the controls script do exactly that), so the PASS is not a stale-object artifact.

## 10. Not covered (honest list)

* **The SoC is not wired into the core's endpoint.** This is a module-level
  package, like the caches and the fabric: `mosaic_soc` is instantiated only by
  its own testbench. Connecting `mosaic_lsu_endpoint` (and the fetch port) to it
  is separate integration work and does not exist yet. The interface here is the
  endpoint's, plus the identity the endpoint already publishes.
* **Not a full AXI implementation.** The card names "an AXI/local bus wrapper";
  what exists is a local bus with AXI's *properties* that the card requires
  (identity preserved, byte strobes preserved, backpressure honoured, error
  completions returned) and a strict one-outstanding-per-slave model. There are
  no bursts, no separate AW/W/B channels, no write-response ordering, no `len`/
  `size`/`burst` encodings, and no cross-requester reordering beyond the
  parallelism of the six slaves. `MAX_OUTSTANDING` is fixed at 4.
* **No PLIC / interrupt controller.** The interrupt outputs are the device lines
  (timer, software, external) directly, as the CLINT/PLIC boundary the core
  consumes; priority, enable and claim/complete are not implemented. The SoC is
  not wired to `mosaic_interrupt`.
* **The peripheral register map is newly specified here.** The three existing
  models disagree about device register offsets and about `test_harness`
  writability (§5). This module implements V-019's register-level map; that is a
  choice, not a derivation from a single frozen source, and the disagreement is
  recorded rather than resolved across the other packages.
* **The timer is a free-running counter, not a real clock.** `mtime` increments
  once per `clk`; there is no timebase calibration, no `mtime` write, and no
  cross-domain synchroniser.
* **The UART is a byte pipe, not a UART.** No baud generator, no framing, no
  overrun/parity, no FIFO-full interrupt; TX is a one-cycle pulse plus a byte.
* **Partial (non-contiguous) write strobes are not exercised from the core.**
  The endpoint always derives a contiguous mask from the size and address, so
  the case's narrow-write test is a *byte* store into a wide register (the
  offset/position is the load-bearing part). A partial mask on a multi-byte
  access is honoured by the fabric but is not driven by any core path.
* **No ECC/parity, no RAM collision matrix, no data-integrity check.** The RAM
  is a byte-strobed array with no reset, following `rtl/common/mosaic_ram.sv`'s
  reset contract; its collision behaviour is not exercised because the fabric
  serialises a slave to one access at a time.
* **No synthesis / area / timing evidence** (unlike I-006 and I-042). This is a
  functional simulation package only.
* **The case is deterministic and directed**; the `seed` option is accepted but
  unused. A randomised soak is left to a future suite.
* **`make lint-cpp` is red** on `sim/unit/tb_core_fp.cpp` (a different lane's
  file), not on anything this package owns.

## 11. How to reproduce

```sh
cd /Users/flare/MosaicRV
python3 tools/run_unit.py --case soc.bus_errors_and_ids              # PASS (p0)
python3 tools/run_unit.py --profile p1 --case soc.bus_errors_and_ids # PASS (p1)
python3 tools/run_soc_controls.py                                    # 4/4 mutants caught
python3 tools/lint_rtl.py --profile p0                               # clean
python3 tools/lint_rtl.py --profile p1                               # clean
make lint-slang                                                      # exit 0
python3 tools/check_records.py                                       # green
python3 tools/check_exclusions.py && python3 tools/check_exclusions.py --negative
python3 tools/run_act_dut.py --no-generate                           # 127/127
```
