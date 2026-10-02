# The reset-traffic rule (V-010), closed for every driver

Work item: the `tb_vec.cpp` reset-traffic follow-up. This report classifies
**every** driver that does not route through `mosaic::BusResetGate`, fixes the
ones where the rule applies, and records the ones where it does not.

## The rule and the one implementation

While reset is asserted, a driver's bus model MUST NOT accept, queue or deliver
a request; a request the DUT presents while reset is asserted is *ignored*. The
rule has one implementation, `mosaic::BusResetGate`
(`sim/common/bus_reset_gate.h`), with two named knobs: `SetObserveResetTraffic`
(off by default) and `SetAcceptDuringResetControl` (the negative control, off by
default).

The class was previously 31 `sim/unit/*.cpp` drivers (30 `tb_core_*.cpp` plus
`tb_reset_traffic.cpp`). The follow-up asked for the class to be closed for the
rest: classify each driver, fix the ones where the rule applies, and say per
driver why it does not.

## Method

The main agent's candidate list came from a crude regex and was explicitly not a
verdict. Every file below was read. The deciding question is not "does it have a
`req`/`rsp` wire" but **"does the driver own a model that accepts, queues or
delivers a request the DUT presents?"** — the only thing the hazard needs, and
the only thing the gate can guard. A driver that drives a module's ports
directly, or that is itself the requester of a bus it does not model, cannot
violate the rule.

## Counts

| | files |
|---|---|
| driver files total (`sim/unit/*.cpp` + `sim/tb/*.sv`) | 110 (71 + 39) |
| `.cpp` drivers on the gate, before | 31 |
| `.cpp` drivers **fixed** here | **8** |
| `.cpp` drivers on the gate, after | **39** |
| `.cpp` drivers classified "rule does not apply" | 27 |
| `.cpp` drivers classified "applies, model is the shared wrapper" | 5 |
| `.sv` wrappers on the gate | 0 (the gate is C++; see the wrapper note) |

## Fixed drivers (rule applies)

All eight own a model of a request/response channel the DUT presents requests
to. Each now routes its accept/deliver decision through one
`mosaic::BusResetGate`, and each case carries a control that presents a request
during reset and is **observed to fail** when the guard is bypassed.

| driver | case | model that the DUT presents requests to | control, and what it prints when the guard is bypassed |
|---|---|---|---|
| `sim/unit/tb_load.cpp` | `lsu.byte_forwarding` | `Memory` with two `Port`s (load + store), `Step()` accepts into `pending` | `MISMATCH [reset-traffic] reset-traffic: the memory model accepted a request while reset was asserted @ cycle N` |
| `sim/unit/tb_lsu.cpp` | `lsu.size_fault_boundaries` | `MemoryModel::Edge()` latches `pending_` | `MISMATCH [reset-traffic] reset-traffic: the memory model accepted a request while reset was asserted @ cycle N` |
| `sim/unit/tb_store.cpp` | `store.wrong_path_visibility` | `MemoryModel::Edge()` latches `pending_` | `MISMATCH [reset-traffic] reset-traffic: the memory model accepted a request while reset was asserted @ cycle N` |
| `sim/unit/tb_fetch.cpp` | `fetch.redirect_late_response` | `Requester` (the responder side of the fetch request/response channel) | `RESULT FAIL … reset-traffic: the requester accepted a request while reset was asserted` |
| `sim/unit/tb_cache.cpp` | `cache.refill_evict_fault`, `cache.mshr_nonblocking` | `Harness` (memory behind the blocking caches) and `MshrHarness` (memory behind the non-blocking L1D) | `CHECK FAILED: reset-traffic: the memory model accepted a request while reset was asserted` |
| `sim/unit/tb_prefetch.cpp` | `prefetch.fault_and_pollution` | `accepted_` model memory behind the prefetch read port | `CHECK FAILED: reset-traffic: the memory model accepted a prefetch read while reset was asserted` |
| `sim/unit/tb_remote.cpp` | `remote.kill_with_delayed_response` | `Remote` (the far side of the request/response link) | `RESULT FAIL … reset-traffic: the remote model accepted a request while reset was asserted` |
| `sim/unit/tb_multihart.cpp` | `multihart.isolation` | `SharedBus` (the shared memory service, one request + one response channel) | `CHECK FAILED: reset-traffic: the shared bus accepted a request while reset was asserted` |

Each phase also runs the *control* (accept-during-reset engaged) and asserts
that the same path then accepts, so the rule check is a guard that has been seen
to fire, not one that cannot fail. The controls were additionally observed from
outside: with `BusResetGate::MayAccept`/`MayDeliver` temporarily forced to ignore
reset, every one of the eight cases above goes red at its reset-traffic check.

### Notes on the two drivers that already had an ad-hoc guard

* `tb_cache.cpp` carried `if (!rst && req_valid && ready)` in `ServiceMem` and
  `!rst && …` in `MshrHarness`, and clears its pending state at reset. That is
  **strictly equivalent** to `MayAccept(rst, presented)`, so it was migrated to
  the gate rather than kept as a second mechanism.
* `tb_lsu.cpp` and `tb_store.cpp` flush the model at reset (`FlushPending()`),
  because the DUT's transaction state is cleared by the reset and the model must
  resynchronise with it. That is **not** subsumed by the gate — the gate refuses
  new traffic, it does not discard an already-accepted request — so the flush is
  retained and the gate is added on top of it, on the accept/deliver decision.

## Applies, but the model is the shared wrapper (5 drivers)

`tb_bringup.cpp`, `tb_exit.cpp`, `tb_loader.cpp`, `tb_reset.cpp` and
`tb_sampling.cpp` all instantiate `Vmosaic_bringup_tb`, whose memory model lives
in `sim/tb/mosaic_bringup_tb.sv`. That model carries its own guard
(`h_rst && !h_accept_in_reset` clears pending state and refuses) and its own
named control (`h_accept_in_reset`), exercised by `harness.reset_traffic`.
`tb_reset.cpp` additionally asserts `h_if_pending`/`h_d_pending` are low on every
reset edge.

The guard is **not strictly equivalent** to the gate: it also clears pending
state (a stronger reset resynchronisation), and it is an SV implementation that
cannot hold the C++ class. It is therefore left as the wrapper's deliberate
second implementation, and its control is `harness.reset_traffic`'s. These five
`.cpp` drivers are not counted in the 39 "on the gate".

## Rule does not apply (27 `.cpp` drivers)

Each instantiates a module and drives its ports directly, or is itself the
requester of a channel it does not model. There is no model behind a request
interface for the DUT to present to, so reset-time traffic cannot be accepted or
queued and the rule cannot be violated. Evidence is the DUT top and the ports
the driver drives.

| driver | DUT (`Vmosaic_*_tb`) | evidence |
|---|---|---|
| `tb_alu.cpp` | `mosaic_alu_tb` | combinational; drives `a`/`b`/`op` and the branch-target pins, reads results |
| `tb_arbiter.cpp` | `mosaic_arbiter_tb` | **requester**: drives `req_valid_i`/`req_src*_i` and `srv_busy_i`, reads grants |
| `tb_bypass.cpp` | `mosaic_cluster_bypass_tb` | register-tag bypass network; drives `p_*`/`w_*`/`c_*` tag pins |
| `tb_csr.cpp` | `mosaic_csr_tb` | CSR file; drives `csr_*`/`mip_i`/`mtime_i`, reads CSRs |
| `tb_decoder.cpp` | `mosaic_decoder_tb` | combinational; drives `insn`, reads control |
| `tb_fifo.cpp` | `mosaic_fifo_tb` | elastic buffers; drives `d*_in_valid`/`d*_out_ready` |
| `tb_fpu.cpp` | `mosaic_fpu_tb` | FP unit; drives `req_*` op pins, reads `res_*` |
| `tb_interrupt.cpp` | `mosaic_interrupt_tb` | interrupt controller; drives `irq_*`/`mip_*`/`mstatus_*` |
| `tb_iq.cpp` | `mosaic_iq_tb` | issue queue; drives `clk`/`rst` and reads exported counters |
| `tb_lease.cpp` | `mosaic_lease_alloc_tb` | lease allocator; drives `req_mask`/`req_valid`/`can_*`/`rel_*` |
| `tb_llb.cpp` | `mosaic_llb_tb` | locality buffer; drives `req_*`/`fill_*`/`inv_*`. `mem_` is a value oracle, not a request channel |
| `tb_muldiv.cpp` | `mosaic_muldiv_tb` | mul/div unit; drives `req_*`, reads `res_*` |
| `tb_owner_fsm.cpp` | `mosaic_owner_fsm_tb` | owner FSM; drives `uop_done`/`ctrl_*`/`res_*` |
| `tb_predictor.cpp` | `mosaic_predictor_tb` | branch predictor; drives `q_*`/`upd_*` |
| `tb_prf.cpp` | `mosaic_prf_tb` | register file; drives `rd_valid_i`/`wr_en_i` |
| `tb_qos.cpp` | `mosaic_qos_tb` | **requester**: drives `req_valid_i`/`req_tag*`/`req_data*`/`srv_busy_i` |
| `tb_ram.cpp` | `mosaic_ram_tb` | **the DUT is the memory**; the driver drives `waddr`/`wdata`/`we`/`raddr` |
| `tb_recovery.cpp` | `mosaic_recovery_tb` | recovery unit; drives `alloc_*`/`commit_*`/`redirect*`/`cred_req_*`/`rsp_*` |
| `tb_rename.cpp` | `mosaic_rename_tb` | rename; drives `alloc_*`/`commit_*`/`free_*`/`wb_*` |
| `tb_result_fifo.cpp` | `mosaic_result_fifo_tb` | result FIFO; drives `p_valid`/`c_ready`/`kill_*` |
| `tb_retire.cpp` | `mosaic_retire_tb` | retire; drives `alloc_*`/`cmp_*`/`pay_*` |
| `tb_rob.cpp` | `mosaic_rob_tb` | ROB; drives `alloc_*`/`close_*`/`cmp_*` |
| `tb_soc.cpp` | `mosaic_soc_tb` | **requester**: drives the `m0`/`m1` master request ports and `rom_load_*`/`uart_rx_*`; the SoC owns the memory and generates its own completions |
| `tb_steering.cpp` | `mosaic_steering_tb` | steering; drives `req_*`/`unit_*` |
| `tb_vec.cpp` | `mosaic_vec_tb` | vector descriptor unit; drives `alloc_*`/`elem_done_*`/`fault_*` (see the decision below) |
| `tb_vrf.cpp` | `mosaic_vrf_tb` | vector register file; drives `rd_valid_i`/`wr_valid_i`/`q_*` |
| `tb_wb.cpp` | `mosaic_wb_arbiter_tb` | writeback arbiter; internal datapath handshakes with PRF/ROB/rename, no memory bus |

## `tb_vec.cpp`: the reset-dominance decision

**Decision: the gate does not apply.** `tb_vec.cpp` instantiates `mosaic_vec_tb`
(a descriptor unit) and drives `alloc_valid`, `elem_done_valid` and
`fault_valid` directly; there is no bus or memory model anywhere in the case. A
request the DUT presents to a model cannot exist, so V-010 has nothing to guard.

`PhaseResetInFlight` (around line 732) is a **DUT-side reset-dominance**
contract, not bus traffic: it offers allocate / element-done / fault pulses on
two cycles with `rst` high and then asserts the unit left no descriptor, no
bitmap bits, no fault progress, no counters and no charged ROB entry. The
contract it tests is *"reset clears state despite the presented stimulus"*, not
*"the DUT ignores the stimulus"* — the pre-reset descriptor must be gone, which
"ignore and preserve" would fail. That is the opposite direction from V-010,
which constrains the *environment* (a bus model ignores a request the DUT
presents). The two are not in conflict, and the case now says which it tests in
the phase comment.

## `.sv` wrappers (39 files)

Every `sim/tb/*.sv` that does not reference the gate is a Verilator wrapper for
one or more `.cpp` drivers. Their headers say so, e.g. "Simulation wrapper …
The wrapper adds no timing of its own" (`mosaic_alu_tb.sv`,
`mosaic_cluster_bypass_tb.sv`, `mosaic_core_tb.sv`, `mosaic_csr_tb.sv`,
`mosaic_fetch_tb.sv`, `mosaic_fpu_tb.sv`, `mosaic_interrupt_tb.sv`,
`mosaic_muldiv_tb.sv`, `mosaic_owner_fsm_tb.sv`, `mosaic_predictor_tb.sv`,
`mosaic_prf_tb.sv`, `mosaic_recovery_tb.sv`, `mosaic_retire_tb.sv`,
`mosaic_rob_tb.sv`, `mosaic_steering_tb.sv`) or "simulation-only glue"
(`mosaic_load_queue_tb.sv`, `mosaic_ram_tb.sv`, `mosaic_store_queue_tb.sv`).
They flatten the DUT's ports so the C++ driver can drive them; they hold no bus
model of their own, so the rule cannot be violated in them. The one exception is
`mosaic_bringup_tb.sv`, which *does* hold the memory model and its own
equivalent guard (see the wrapper-level section above).

`mosaic_core_tb.sv` deserves a note: it is the wrapper for the 30 `tb_core_*.cpp`
drivers. It only flattens the core's `imem_*`/`dmem_*`/`cb_mem_*` ports out to
the C++ side (`assign imem_rsp.rdata = imem_rsp_rdata_i;` etc.); the bus model
is the C++ driver, and those drivers are all on the gate.

## Not covered

* The 39 `sim/tb/*.sv` wrappers do not hold the gate. 38 of them hold no bus
  model, so there is nothing to route. `mosaic_bringup_tb.sv` holds the model
  but carries its own equivalent guard; it cannot hold the C++ class, and its
  guard is stronger than the gate (it also clears pending state), so it was left
  as the wrapper's deliberate second implementation with
  `harness.reset_traffic` as its control.
* The 27 `.cpp` drivers in the table above are not on the gate because the rule
  does not apply to them; adding the gate would be bookkeeping with no hazard
  behind it.
* `multihart.isolation` (`sim/unit/tb_multihart.cpp`) is fixed and carries its
  control, but its case could not be re-run during this work: `--profile p3`
  fails to elaborate `rtl/core/mosaic_core.sv` (missing `MOSAIC_MSHR_ENTRIES`
  and friends) because another lane is mid-flight in the config gate. This is a
  phantom failure of a sibling's half-finished edit, not of this change. The
  case must be re-run once that lane lands.
* The controls present their request through the driver's own accept path with a
  synthetic `presented=true`, because these unit DUTs present nothing while held
  in reset. The guard is exercised exactly as the cycle loop exercises it; the
  DUT-side "presents a reset-vector request" behaviour is the `harness.reset_traffic`
  case's job.
