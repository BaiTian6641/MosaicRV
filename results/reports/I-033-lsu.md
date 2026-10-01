# I-033 — the ordered memory endpoint and the access-fault boundary

Work package I-033. Case `CASE=lsu.size_fault_boundaries`.

This is the **independent verification** of `rtl/core/mosaic_lsu_endpoint.sv`, a
module written by the integration lead. The DUT is therefore a candidate under
test, not a specification: every expected value below is derived from the RISC-V
load/store rules and from the module's own documented contract (the "policy",
"byte lanes" and "state machine" sections of its header), and the driver contains
no expression copied from the RTL.

Files delivered:

| File | Role |
|---|---|
| `sim/tb/mosaic_lsu_endpoint_tb.sv` | simulation wrapper; flat driver-facing ports, packet assembly/flattening, geometry read back |
| `sim/unit/tb_lsu.cpp` | C++ driver: independent memory model, spec-side value rules, eight phases |
| `results/reports/I-033-lsu.md` | this file |

`rtl/core/mosaic_lsu_endpoint.sv` was **not edited**. No RTL defect was found
(§6). `tests/unit/registry.json`, `config/**` and `results/PROGRESS.md` were not
touched.

---

## 1. The harness and the two-snapshot rule

`Bench::Cycle()` is the only place the DUT is read. It drives the inputs with the
clock low, evaluates, and reads **one** snapshot of every DUT output. That
snapshot — and nothing read after the rising edge — is used by `CheckCycle()` and
by the state update in `Commit()`. A value presented in cycle *N* is never mixed
with the state that exists after edge *N*.

Cycle indices are exact and shared between the driver, the model and the RTL's
documented state machine. For a memory access accepted upstream in cycle *N*:
the memory request is offered from *N+1*, the memory may take it from *N+1*, the
endpoint consumes the memory response and composes its own response at edge *M*,
and the response is offered from *M+1* and held until accepted. A misaligned
access composes its trap response at the acceptance edge and offers it from *N+1*
without ever entering the request state.

## 2. The memory model

`MemoryModel` is a byte-addressed 4 KiB region at `0x8000_0000`; an access whose
bytes leave it is answered with `fault`.

* **Lane convention.** A response is the aligned doubleword whose byte at `addr`
  is lane `addr[2:0]`. A store writes the byte at address `a` from lane `a[2:0]`
  of the presented payload, for exactly the bytes the access owns.
* **No alignment check.** An unaligned access is assembled and served. This is
  deliberate: the profile's trap policy lives in the endpoint, and the model must
  not be able to stand in for it.
* Programmable latency, programmable back-pressure on the request side, and
  injectable faults.
* It keeps its **own** accepted-request counter and request log, and the
  misalignment and ordering checks assert on *those*, never on a DUT counter.

The store data written into the model is the value the specification demands
(`store_data << 8*lane`); the DUT's own `wdata` and `wstrb` are compared against
that value in the same cycle, so the model's contents never depend on a DUT
output.

## 3. What is checked on every cycle

`CheckCycle()` runs in every cycle of every phase (idle cycles included):

1. `req_ready_o` is asserted **exactly** when the endpoint holds no outstanding
   request, and `o_busy` is exactly "one is outstanding".
2. The response the driver is owed is offered, and `id`, `fault`, `cause`, `tval`
   and (for a successful load) `data` match the model. A response that starts
   being offered is held, unchanged, until accepted; no response appears when
   none is owed.
3. A misaligned access never reaches the memory — checked on the model's counter.
4. The memory request carries the right address, size and kind; for a store, the
   `wstrb` and shifted `wdata` the specification demands; and the payload does
   not change while `valid && !ready`.
5. `o_inflight_addr`/`o_inflight_size` name the access being served.
6. All six counters equal the driver's own tally of what it observed.
7. Conservation, on the endpoint's own outputs: an accepted request is either
   still inside (`pending`) or has had its response composed; a composed response
   is either being offered or has been delivered; and the driver's in-flight
   count equals `offered + pending` — the endpoint holds at most one of each.
8. `o_last_fault_cause`/`o_last_fault_tval` equal the driver's record.

## 4. Phases, and the check count of each

Each phase resets first and ends by comparing every counter against the driver's
tally and by idling the endpoint. Counts are the harness's own check calls
(printed per phase in the run log). Base run, seed 1:

| Phase | Checks | Cycles | Coverage in the phase |
|---|---|---|---|
| `reset-state` | 86 | 6 | idle, `rsp_valid_o` low, six counters zero, no memory request |
| `size-sign-matrix` | 5091 | 252 | 4 sizes × 2 signs × every aligned lane (15 lanes): 45 loads, 15 stores, 15 readback loads, 60 memory transactions |
| `access-fault` | 771 | 290 | 8 faulted accesses (4 sizes × load/store) + the out-of-region case: cause 5/7, `tval` = address |
| `misalignment` | 2785 | 440 | 17 misaligned offsets × load/store = 34 traps, **plus** the precedence pair (aligned unmapped → 5/7, unaligned unmapped → 4/6); model saw 1 transaction (the aligned unmapped access) and 0 for every misaligned one |
| `backpressure` | 1701 | 525 | 25 cycles of consumer refusal, 20 cycles of memory refusal, a 12-cycle memory latency |
| `ordering-identity` | 969 | 575 | two requests in acceptance order with the second offered while the first was in flight; ids differing only in the generation field; 0/all-ones/field-corner ids; a request offered while a response was held |
| `reset-in-flight` | 269 | 592 | reset while the transaction was inside the memory system, then a first post-reset transaction |
| `random-soak` | 72128 | 4118 | 400 random transactions: 256 loads, 144 stores, 250 memory transactions, 150 misaligned traps, 41 access faults, random latency/back-pressure on both sides |
| **total** | **83805** | **4118** | 517 responses, 336 loads, 181 stores, 332 transactions, 185 misaligned traps, 50 access faults |

First mismatch: **none** — the base run is PASS, exit 0.

```
$ python3 tools/run_unit.py --profile p0 --case lsu.size_fault_boundaries
PASS lsu.size_fault_boundaries    task=I-033
```

Seed robustness (the soak's stimulus comes from `--seed`; no seed was tuned):

| seed | 1 | 2 | 3 | 7 | 99 | 12345 | 424242 |
|---|---|---|---|---|---|---|---|
| verdict | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| checks | 83805 | 86861 | 87091 | 83487 | 84402 | 87955 | 86461 |

## 5. Mutants

Each mutant was rebuilt with `run_unit.VERILATOR_FLAGS.append('-D<name>')` and
the case's own sources, run directly, and required to exit 1. The failing
message is included because it shows the mutant's own symptom, i.e. that the
`ifdef` body was really compiled and the shipping build was not what ran.

| Mutant | Exit | Failures (base 0) | Checks before the abort (base 83805) | First mismatch |
|---|---|---|---|---|
| `NO_MISALIGN_CHECK` | 1 | 1 | 6019 | `[misalignment] response-offered: no response presented while one is owed @ cycle 295` — the access went to memory instead of trapping |
| `NO_SIGN_EXTEND` | 1 | 1 | 281 | `[size-sign-matrix] response-data: expected 0xffffffffffffff80, got 0x0000000000000080 @ cycle 17` — a signed byte load is zero-extended |
| `NO_STORE_SHIFT` | 1 | 1 | 660 | `[size-sign-matrix] memory-wdata: expected 0x3456789abcdef000, got 0x123456789abcdef0 @ cycle 35` — the store data is not shifted to the addressed lane |
| `FAULT_AS_ZERO` | 1 | 1 | 5289 | `[access-fault] response-fault: expected fault=1, got 0 @ cycle 259` — an access fault is reported as a successful access |

Every mutant fails in the phase that owns the behaviour, and no mutant needed a
check that did not already exist.

Note on `NO_MISALIGN_CHECK`: the first check to fire is the missing trap response
(the cycle after the acceptance), so the run aborts before reaching the
model-counter assertion that follows each misaligned transaction. That assertion
— "the memory model saw zero transactions for a misaligned access" — is still
live on every cycle of every misaligned transaction (check 3 above) and is the
line of defence against the distinct defect of trapping *and* leaking the access
to memory.

`Reporter` records one check per run by project convention (as `tb_rob.cpp`
does), so `result.json` shows `"checks": 1, "failures": 0` for the base and
`"failures": 1` for a mutant; the harness's own 83805 comparisons are printed
per phase in `results/unit/lsu.size_fault_boundaries/run.log`.

## 6. Defects found

**In the RTL: none.** The module passed every phase above and every mutant
distinguishes a documented behaviour. In particular the two places where the
RTL's own choices are visible, and which the case therefore treats as
don't-care rather than as checked behaviour:

* `mem_req_o.wstrb` and `mem_req_o.wdata` are driven with the size mask and the
  shifted store operand even for a **load**. A memory must ignore both when `we`
  is low, so the driver checks them only for stores and states so here.
* `rsp.data` is driven with the extracted value on a **faulted load** and with
  zero for a **store**. The module's response carries a separate `fault` bit and
  the contract calls `data` "the loaded, extended value", so the driver checks
  `data` only for a successful load; on a fault (and for a store) it is
  explicitly don't-care and the fault bit / strobes are authoritative. This is
  the consistent choice the case makes, stated here as required.

**In the testbench: two, both mine, both fixed.**

1. The memory model first wrote the presented payload's *low* byte to `addr`
   instead of the byte in the lane the address selects. Decided by the module's
   documented lane convention ("the byte at `addr` is lane `addr[2:0]`"); the
   model now writes through the lane. The DUT was right.
2. A directed expectation in the back-pressure phase was written by hand as
   `0xffffffffffff0080` for a signed halfword `0xff80`. The correct
   sign-extension of `0xff80` is `0xffffffffffffff80` (bit 15 is the sign bit,
   so bits 63:16 become ones). The DUT produced the correct value and the
   spec-side `SpecExtract` agreed with it; the hand-written constant was the
   error and was corrected. This is recorded because it is exactly the kind of
   "tune the test to the model" step that must not be taken silently — here the
   model and the DUT were right and the constant was wrong.

## 7. NOT verified

* **No physical memory.** The region is a `std::vector<uint8_t>`; there is no
  SRAM, no cache, no MMIO, no coherence, no error-correction, and no protocol
  behaviour beyond the single-outstanding valid/ready pair.
* **No multi-outstanding.** The endpoint holds one transaction; I-043's MSHR and
  nonblocking responses are out of scope, and nothing here constrains them
  beyond the acceptance-order statement for one outstanding request.
* **The memory model is flushed on reset.** A real memory would not be, so the
  reset-in-flight phase verifies the endpoint's own state and the fact that the
  half-answered access never completes, not that a memory resynchronises.
* **Don't-care fields**, as listed in §6: `wstrb`/`wdata` on a load, `rsp.data`
  on a store or a fault. A defect confined to those fields is invisible here.
* **Access-fault data.** Because `rsp.data` on a fault is don't-care, the case
  cannot distinguish "data is zeroed on a fault" from "data is the extracted
  value"; only the fault bit, cause and tval are pinned.
* **Only size codes 0..3** (byte, half, word, doubleword) are driven. Codes 4..7
  are not legal encodings and the case does not pretend to define them.
* **One hart.** `macro_id_t.hart` is 0 in p0; the identity check covers
  rob_index/rob_gen/uop_index and their *layout* (the wrapper exports each width
  and the driver checks the sum), not a second hart.
* **The response latency is an interface assumption.** "The response is built in
  the cycle the memory answers and offered from the next one" is checked against
  the module's documented state machine, not against the ISA; a deliberate change
  to that beat would be a change to the expectation.
* **Random, not exhaustive.** The soak is 400 seeded transactions (over 500
  responses per run); it is not a proof, and the directed phases, not the soak,
  pin each documented behaviour.
* **Simulation only.** No synthesis or Yosys run covers the testbench files. They
  obey the no-assignment-pattern rule, so they can be parsed by Yosys, but that
  was not exercised.
* **No formal property checking**, and no negative test of the memory model
  itself (e.g. a memory that answers with the wrong latency is not modelled).

## 8. How to reproduce

```
# the case (builds, runs, prints one RESULT line)
python3 tools/run_unit.py --profile p0 --case lsu.size_fault_boundaries

# a different soak seed
build/p0/unit/lsu.size_fault_boundaries/lsu.size_fault_boundaries \
  --case lsu.size_fault_boundaries --out /tmp/lsu --seed 7 --max-cycles 200000

# one mutant
python3 - <<'PY'
import sys; sys.path.insert(0, 'tools'); import run_unit
C = 'lsu.size_fault_boundaries'
e = run_unit.load_registry()['cases'][C]
run_unit.VERILATOR_FLAGS.append('-DMOSAIC_LSU_MUTANT_NO_SIGN_EXTEND')
run_unit.build_case('p0', C, e)
PY
build/p0/unit/lsu.size_fault_boundaries/lsu.size_fault_boundaries \
  --case lsu.size_fault_boundaries --out /tmp/lsu-mut --seed 1 --max-cycles 200000; echo "exit $?"

# lint, scoped to the new sources
verilator --lint-only -Wall --top-module mosaic_lsu_endpoint_tb \
  -Ibuild/p0/sim -Irtl/core -Irtl/common -Ibuild/p0/rtl \
  rtl/core/mosaic_pkg.sv rtl/core/mosaic_uop_pkg.sv rtl/core/mosaic_lsu_endpoint.sv \
  sim/tb/mosaic_lsu_endpoint_tb.sv

c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow \
  -Ibuild/p0/sim -Isim/common -I$(verilator --getenv VERILATOR_ROOT)/include \
  -Ibuild/p0/unit/lsu.size_fault_boundaries/obj_dir sim/unit/tb_lsu.cpp
```

The Verilator lint above reports no warnings and exits 0. The `c++` line is
`lint-cpp`'s own command scoped to this case's driver: it reports only warnings
from Verilator's runtime headers, none from `tb_lsu.cpp`. (`make lint-cpp` over
the whole tree reported `25 file(s) clean` when it was run for this work; it was
failing afterwards on `sim/unit/tb_rename.cpp`, a concurrently edited sibling
file that is not part of this case.) `slang-tidy` reports only the project-wide
style warnings (`STYLE-2` port suffixes, `STYLE-7` `i_` instance prefix) that the
RTL and the other wrappers already trigger, and which the project's `lint-slang`
gate does not cover because it walks `rtl/` only.
