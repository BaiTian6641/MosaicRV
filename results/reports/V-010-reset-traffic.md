# V-010 — the reset-traffic rule, harness-wide

Case: `harness.reset_traffic` (driver `sim/unit/tb_reset_traffic.cpp`, top
`mosaic_bringup_tb`, registry `"pending": true`). Shared mechanism:
`sim/common/bus_reset_gate.h` (`mosaic::BusResetGate`).

## 1. The rule, and what happens to a request presented during reset

**While reset is asserted, a driver's bus model must not accept, queue or
deliver a request. A request the DUT presents while reset is asserted is
ignored: the model accepts nothing and answers nothing.**

That is the defined behaviour. It does not depend on what the memory model
happens to do. The alternative considered — asserting a protocol error — was
rejected because reset is a legal environment state in which the fetch unit's
PC register is *held* at the reset vector, so the DUT presenting a request is
not a DUT error; it is the harness's job not to answer it.

Why it matters: the OoO core's fetch unit matches a response to a fetch slot by
`{id, epoch}`, and only a redirect advances the epoch. During reset the fetch
unit presents the reset-vector request every reset cycle. A model that accepts
those requests queues responses the core discards at reset; they then sit in
front of the post-reset responses with the *same* `id` and `epoch`, so fetch
pairs a stale response with a fresh request and the instruction stream shifts
by one — which from outside looks like `ev_rd`/`ev_value` lagging `ev_pc`.

## 2. The one shared mechanism

`sim/common/bus_reset_gate.h` defines `mosaic::BusResetGate`, included from
`sim/common/sim_common.h` so every driver that already includes the common
header inherits it without a second include.

```cpp
bool MayAccept(bool reset_asserted, bool request_presented); // req_valid && req_ready
bool MayDeliver(bool reset_asserted) const;                  // gate the response pop
void SetObserveResetTraffic(bool);      // named observation option, OFF by default
void SetAcceptDuringResetControl(bool); // negative-control hook, OFF by default
```

`MayAccept` is the single decision point: it returns true only when a request is
actually presented (`request_presented`) and either reset is not asserted or the
negative control is engaged. A driver's cycle therefore reads:

```cpp
if (bus_reset_.MayAccept(rst, (req_valid && req_ready))) { bus.Accept(...); }
if (bus_reset_.MayDeliver(rst) && (rsp_valid && rsp_ready)) { bus.Pop(); }
```

Nothing else touches the bus. The two knobs are explicit and named; the default
is to ignore reset-time traffic silently, with no bookkeeping.

*Observation option* (`SetObserveResetTraffic`): for a driver that must *see*
reset-time traffic (a case asserting the DUT presents nothing during reset).
Observation records what was presented and never changes what the bus does.
*Control* (`SetAcceptDuringResetControl`): restores the pre-rule behaviour so a
case can prove it is what catches the class.

## 3. Where it is enforced, and what was already compliant

Every core-level driver under `sim/unit/` that models a bus or memory now routes
every accept and every response pop through the gate. Enumerated from the tree,
not from the brief's list (the brief named `tb_core_branch.cpp` and
`tb_core_perf.cpp`, which do not exist).

**Changed (22 drivers, no guard at all).** Each gained one
`mosaic::BusResetGate bus_reset_;` member and gate-guarded imem/dmem accepts and
pops:

`tb_core.cpp`, `tb_core_act.cpp`, `tb_core_boot.cpp`, `tb_core_corpus.cpp`,
`tb_core_csr_rules.cpp`, `tb_core_event.cpp`, `tb_core_fabric.cpp`,
`tb_core_fence.cpp`, `tb_core_fp.cpp`, `tb_core_irq.cpp`, `tb_core_mem.cpp`,
`tb_core_mmio.cpp`, `tb_core_mmio_model.cpp`, `tb_core_priv.cpp`,
`tb_core_ready.cpp`, `tb_core_retire.cpp`, `tb_core_sv39.cpp`,
`tb_core_sweep.cpp`, `tb_core_tlb.cpp`, `tb_core_trap.cpp`,
`tb_core_trapstate.cpp`, `tb_core_visibility.cpp`.

`tb_core_sv39.cpp` and `tb_core_tlb.cpp` compute `imem_accept`/`dmem_accept` as
booleans rather than calling `Accept` directly; those booleans now go through
the gate. `tb_core.cpp` and `tb_core_ready.cpp` route through their `ModelStep`,
which now takes `rst`.

**Partial ad-hoc guards, replaced by the gate (4 drivers).** Evidence is the
code, not a claim:

| driver | what it already had |
|---|---|
| `tb_core_amo.cpp` | `if (!rst) { … }` around the imem accept + pop; the **dmem** accept was unguarded |
| `tb_core_compressed.cpp` | `if (!rst && req_valid && req_ready)` on the imem accept; the **dmem** accept was unguarded |
| `tb_core_lrsc.cpp` | `if (!rst) { … }` around the imem accept + pop; the **dmem** accept was unguarded |
| `tb_core_pc.cpp` | `if (!rst && req_valid && req_ready)` on the imem accept; the **dmem** accept was unguarded |

For these four the ad-hoc `!rst` was *replaced* by the gate, so there is one copy
of the rule, not two.

**Already fully compliant (1 driver).** `tb_core_cache_path.cpp` is the one
driver the lane fixed ad hoc (`!rst` on the imem/dmem accept and pop). It was
refactored onto the shared gate the same way (both `CoreRun` and `Directed`), so
it no longer carries its own copy of the rule.

**The bringup wrapper** (`sim/tb/mosaic_bringup_tb.sv`) already refused
reset-time traffic: its fetch/data accept logic lives in the non-reset `else`
branch and the reset branch clears `m_if_pending`/`m_d_pending`. It is a second,
independent implementation of the rule (SystemVerilog, not C++). It gained a
`h_accept_in_reset` control input and `h_if_reset_accepts`/`h_d_reset_accepts`
counters, so the case can *assert* the rule rather than infer it.

## 4. The case, and its control

`harness.reset_traffic` runs a hand-built 16-instruction RV64IM program on
`mosaic_bringup_core` through `mosaic_bringup_tb`. **The first post-reset
instruction is `addi`, and no redirect is executed before the run ends** — the
condition under which the hazard bites. (A program starting with `JAL` would
pass even with the bug present, which is why every other driver's corpus masks
it.)

The rule run (`h_accept_in_reset = 0`):

* the core was in `S_FETCH` — the state that presents the fetch request — on all
  **8** reset cycles, so reset-time traffic was really offered;
* `h_if_reset_accepts == h_d_reset_accepts == 0`: the bus model accepted nothing
  during reset;
* all **15** retired instruction words equal the byte at their own program
  counter in the image (a one-instruction shift is caught by name);
* the four signature words are the program's arithmetic (`0x123, 0x456, 0x579,
  0x7ff`) and TOHOST is `1` — the program's own values, not the DUT's stream.

The control (`h_accept_in_reset = 1`) restores accept-during-reset: the model
accepts **4** reset-time fetch requests, and the case's rule check
(`reset_accepts == 0`) becomes false — i.e. the case **fails** under the control.
The case asserts exactly that, so it is the case that catches the class.

The bringup core is in-order and re-syncs its handshake, so the *stream shift*
cannot be observed on this DUT; the counter is the detector there. The shift
itself is demonstrated deterministically in the case with the shared gate: a
queueing model that accepts the reset-vector request leaves a stale response
that the post-reset request (same `id`, same `epoch`, because nothing
redirected) matches, and the wrong instruction word is delivered. Under the rule
the queue is empty and the post-reset request gets its own word.

RESULT line:

```
RESULT PASS harness.reset_traffic first=nonredirect reset_request_cycles=8
  reset_accepts=0 stream_events=15 stream_matches=1 control_accepts=4
  control_completed=1 stale_response_class=demonstrated 39 checks
```

## 5. Every affected driver's own case → still passing

Re-run after the change, each with a forced clean rebuild. 32/33 pass.

| case | profile | verdict |
|---|---|---|
| amo.linearization | p0 | PASS |
| boot.p1_contract | p0 | PASS |
| compressed.cross_boundary | p0 | PASS |
| core.corpus_branch | p0 | PASS |
| core.corpus_sweep | p0 | PASS |
| core.event_payload | p0 | PASS |
| core.mem_program | p0 | PASS |
| core.trap_csr_program | p0 | PASS |
| core.unwritten_reg_read | p0 | PASS |
| csr.rule_ledger | p0 | PASS |
| fabric.fixed_two_cluster | p0 | PASS |
| fabric.integrated | p0 | PASS |
| fence.code_and_data_order | p0 | PASS |
| fp.precise_flags_and_boxing | p0 | PASS |
| harness.reset_traffic | p0 | PASS |
| irq.replay_timeline | p0 | PASS |
| lrsc.reservation_progress | p0 | PASS |
| mem.visibility_provenance | p0 | PASS |
| mmio.exactly_once | p0 | PASS |
| mmio.side_effect_model | p0 | PASS |
| pc.branch_and_fetch_visibility | p0 | PASS |
| privilege.permission_matrix | p0 | PASS |
| retire.width_and_order | p0 | PASS |
| sv39.walk_and_faults | p0 | PASS |
| tlb.sfence_vma | p0 | PASS |
| trap.precise_state | p0 | PASS |
| cache.integrated_path | p1 | PASS |
| core.act_dut | p1 | PASS (127/127) |
| core.bringup_vs_reference | p0 | PASS |
| reset.replay_determinism | p0 | PASS |
| loader.elf_boundaries | p0 | PASS |
| exit.protocol_termination | p0 | PASS |
| harness.sampling_calibration | p0 | PASS |
| core.act_dut | p0 | **FAIL — pre-existing, not this change** |

`core.act_dut` under **p0** fails 0/127. Rebuilding `core.act_dut` with
`git show HEAD:sim/unit/tb_core_act.cpp` (nothing else changed) fails identically,
so the p0 failure predates this change; the case drives the p1 ACT4 configuration
(`config=tests/act4/mosaic-p1/test_config.yaml`) and is a p1 case. Under p1 it is
127/127 both at HEAD and with this change.

## 6. Gates

* `make lint-cpp` — fails **only** on `sim/unit/tb_vec.cpp`, which this lane was
  told not to touch (it belongs to the `VecChaining` lane, whose edits are in
  flight and whose generated `Vmosaic_vec_tb.h` is out of date). Every one of
  this lane's 28 C++ files compiles with **no errors** under the `lint-cpp`
  flags (`-std=c++17 -fsyntax-only -Wall -Wextra -Wshadow`); the remaining
  diagnostics are pre-existing warnings, which `lint-cpp` does not treat as
  failures.
* `make check`, `tools/lint_rtl.py`, `slang-tidy`, `check_records.py`,
  `check_exclusions.py` are the integration lead's project-wide gates; no RTL or
  config file in this lane's change set, so they are unaffected by it.

## 7. Not covered

* **`sim/unit/tb_vec.cpp`** (vector) — not touched, per the lane split. Its
  harness drives `mosaic_vec_tb` directly with a stimulus/expect model; it needs
  the same rule if it models a bus across a reset. Reported for the `VecChaining`
  lane to apply (or for me to apply after it lands).
* **Non-core drivers that model a bus.** Surveyed, and already compliant in
  their own way — each refuses reset-time traffic with an ad-hoc `!rst`, which is
  the same behaviour as the gate but not routed through the shared mechanism:
  * `tb_cache.cpp` — `ServiceMem` accepts only under `if (!rst && req_valid && req_ready)`.
  * `tb_soc.cpp` — `Step` samples/accepts only when `!rst`; the reset branch
    clears its counters.
  * `tb_remote.cpp` — `remote_.Accept` is under `if (!rst)`.
  * `tb_fetch.cpp` — its response path is under `if (!rst)`.
  They are not core-level fetch-path drivers, so the epoch hazard cannot bite
  them the way it bites the core drivers, and they are left as they are; the
  report records them rather than silently converting them.
* **Directed PTW/TLB benches** inside `tb_core_sv39.cpp`/`tb_core_tlb.cpp`
  (`PtwBench`, `TlbBench`) model the *translation* memory port and drive reset
  separately from their `Cycle()`. They are sub-module fixtures, not the core
  fetch path, and are not gated. Stated here rather than implied.
* **`perf.equal_resource_compare`** lists `sim/unit/tb_core_perf.cpp` in the
  registry, but that file does not exist in the tree (registered `pending`), so
  there is no driver to change.
* The control demonstrates the rule's consequence on the in-order bringup core
  via the counter and the deterministic stale-response model; it does **not**
  reproduce the full epoch-shift on the OoO core, which would need a
  `mosaic_core_tb` case. The shared mechanism is exercised by every core driver's
  own passing case in the table above.

## 8. Note on the shared mechanism's semantics

The first cut of `MayAccept` returned true whenever reset was deasserted,
ignoring `request_presented`; a driver using the prescribed
`if (MayAccept(rst, req_valid && req_ready))` then accepted on *every* live
cycle, which made `core.act_dut` fail 0/127. The gate now requires a presented
request as well as "not in reset", so the prescribed usage is a correct gate.
This is why the table above is a re-run and not a claim.
