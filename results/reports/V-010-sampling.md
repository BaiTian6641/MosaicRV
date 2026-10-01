# V-010 — clock and sampling-loop calibration

Work package V-010 (`docs/validation-plan.md` §5 V-010, `docs/stage-0-contracts-bringup.md`
§V-010): fix the unit of integer time, drive in the low phase, `eval()` in the high
phase, sample after the state has settled, use `eval()`/`final()` deliberately, enable
`--timing` only where a delay is genuinely needed and advance its pending events, apply
backpressure item by item, and check that one handshake is recorded exactly once.

| Artifact | Role |
|---|---|
| `sim/unit/tb_sampling.cpp` | the case driver; case `harness.sampling_calibration` (**new**) |
| `sim/unit/sampling_program.S` | the fixture program (112 bytes), and the recipe that produces the embedded bytes (**new**) |
| `sim/tb/mosaic_bringup_tb.sv` | the DUT wrapper; **edited** — two harness inputs for response latency, one `-D`-gated mutant (§7) |
| `rtl/core/mosaic_bringup_core.sv` | the DUT; **edited** — the wait-state defect this case found, one `else` per wait state (§8) |
| `tools/run_sampling_controls.py` | the mutant runner (**new**) |
| `results/reports/V-010-sampling.md` | this file |

## STATUS: PASS — one defect found in the DUT and fixed, four controls all caught

| Check | Result |
| --- | --- |
| `python3 tools/run_unit.py --profile p0 --case harness.sampling_calibration` | `PASS`, exit 0, 110 checks, 3 runs, 630 cycles |
| `python3 tools/run_unit.py --profile p0 --case core.bringup_vs_reference` | `PASS`, exit 0 (re-run after the DUT and wrapper edits) |
| `python3 tools/run_unit.py --profile p0 --case reset.replay_determinism` | `PASS`, exit 0 (re-run) |
| `python3 tools/run_unit.py --profile p0 --case loader.elf_boundaries` | `PASS`, exit 0 (re-run) |
| `python3 tools/run_unit.py --profile p0 --case exit.protocol_termination` | `PASS`, exit 0 (re-run) |
| `python3 tools/run_unit.py --profile p0 --case harness.bad_image` | `PASS`, exit 0 (re-run) |
| negative controls, this package: `python3 tools/run_sampling_controls.py` | 4 of 4 mutants exit 1 and name the check they break (§6) |
| V-009's five reset controls, re-run after the DUT edit | 5 of 5 still exit 1 and name a check (§8) |
| `verilator --lint-only -Wall` on `mosaic_bringup_tb.sv` and `mosaic_bringup_core.sv` | exit 0, no warnings |

---

## 1. The timeline convention, written out

**The unit of integer time is one rising edge of the harness's own clock.** There is no
other notion of time in this case: `--timing` is not enabled anywhere in the build,
Verilator's `VerilatedContext::time()` is asserted to still be 0 at the end of the run,
and `eventsPending()` is asserted false, so no delayed event exists that anything could
have been left to advance. That is the checkable form of "enable `--timing` only where a
delay is genuinely needed": no delay is genuinely needed, because the harness owns the
clock, so the scheduler is never switched on.

One cycle, in order, exactly three `eval()` calls and no more, in the shipping build:

| Phase | What happens | `eval()` |
| --- | --- | --- |
| low | `h_clk = 0`; the harness drives reset, the image port, and the two backpressure-latency inputs | yes |
| rising edge | `h_clk = 1`; all sequential state updates here | yes |
| high (settled) | the harness samples `c_evt_valid`, `h_if_pending`, `h_d_pending`, `c_dmem_req_o`/`we_o`/`addr_o`, `h_tohost_written` and `c_dbg_state` — **once**, and only here | none |
| falling edge | `h_clk = 0`; the model is left low for the next cycle | yes |

Because every flip-flop in the design is `posedge`-triggered, a repeated `eval()` with
the clock unchanged cannot re-trigger one; the hazard is not the extra evaluation but an
extra *sample* of the settled state, which is what the detectors in §5 exist for.

Run cycle *N* means the *N*-th rising edge after reset release; the reset edges are
before cycle 1 and are not counted. Every check that names a cycle uses that numbering.

The convention is not merely described, it is checked cycle-exactly. The exit protocol
is the sharpest instrument available, because it has three observable cycle numbers:
the TOHOST store is posted on the data port in cycle *req*; the memory model accepts it
at the end of that cycle, which is when it takes effect and sets the end-of-run latch;
`h_tohost_written` is that latch registered once more, so it is seen in cycle
*req + 2*; the ack is delivered in cycle *req + 1 + gap*; the core commits on that edge
and the event pulse is registered, so the store retires in cycle *req + 2 + gap*.

```
zero-latency  tohost store req=153 watch=155 retire=155
plan B        tohost store req=231 watch=233 retire=235   (this item's gap = 2)
plan C        tohost store req=235 watch=237 retire=240   (this item's gap = 3)
```

All three relations (`watch = req + 2`, `retire = watch + gap`, `retire >= watch`) are
asserted for every plan. A harness with the phase order wrong, or with `eval()` treated
as a clock, does not reproduce them.

This also exposes something the exit protocol has to survive, and a harness that stops
on the end-of-run pulse alone does not: **under backpressure the pulse precedes the
store's retire by exactly the item's memory latency.** A harvester that ends the run the
moment `h_tohost_written` is seen would cut the final architectural event off the stream
— a missed retire, produced by the environment rather than by the core. This case
therefore ends on the store's *retire* and asserts that the pulse was seen at or before
it. V-012 owns the full termination state machine; this is the one fact about it V-010
needs, and it is measured rather than assumed.

## 2. What the case drives

`mosaic_bringup_core` through `sim/tb/mosaic_bringup_tb.sv` — the same DUT pair the
registered I-008, V-009, V-011 and V-012 cases use, so the calibration is about the
harness those cases actually share.

The wrapper gained two inputs for this package, `h_if_gap` and `h_d_gap` (`[7:0]`), which
add that many extra cycles of response latency to the fetch and data ports. They are
zero in every other driver — Verilator zero-initialises inputs, verified directly before
the ports were added — and **the zero case is the original model statement for
statement**, so no other driver's behaviour can change. This case is the only one that
drives them, and it picks the latency *per item*: the *N*-th handshake on a port is
served with the *N*-th entry of that port's published pattern, so successive requests
get deliberately different latencies.

| Plan | fetch gaps | data gaps |
|---|---|---|
| `zero-latency` | — (all 0) | — (all 0) |
| `plan B` | 0, 1, 2, 3 | 0, 2, 1, 3 |
| `plan C` | 3, 1, 0, 2 | 1, 3, 2, 0 |

The fixture is `sim/unit/sampling_program.S`, 112 bytes, embedded base64 and pinned by
size and FNV-1a-64 `0x23a629ada6b49194`. SHA-256 of the flat binary:

```
62adcb3c2543fdcb516ba03bb4a3eb33e34efb170b42faac4eacccae5959edb8
```

Rebuilt with:

```
$ riscv64-elf-gcc -march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
      -nostdlib -nostartfiles -ffreestanding -fno-builtin -fno-pic \
      -mno-relax -Wa,--fatal-warnings -c sampling_program.S -o sampling_program.o
$ riscv64-elf-ld -Ttext=0x80000000 --no-relax -e _start -o sampling_program.elf sampling_program.o
$ riscv64-elf-objcopy -O binary sampling_program.elf sampling_program.bin
$ riscv64-elf-objdump -d -M no-aliases sampling_program.elf
0000000080000000 <_start>:
    80000000:	000402b7          	lui	t0,0x40
    80000004:	0012829b          	addiw	t0,t0,1
    80000008:	00d29293          	slli	t0,t0,0xd        # 0x80002000 BUF
    8000000c:	0010031b          	addiw	t1,zero,1
    80000010:	01f31313          	slli	t1,t1,0x1f
    80000014:	40030313          	addi	t1,t1,1024       # 0x80000400 SIG
    80000018:	000803b7          	lui	t2,0x80
    8000001c:	0013839b          	addiw	t2,t2,1
    80000020:	00c39393          	slli	t2,t2,0xc        # 0x80001000 TOHOST
    80000024:	00400e13          	addi	t3,zero,4         # trip count
    80000028:	004498b7          	lui	a7,0x449
    8000002c:	8cd8889b          	addiw	a7,a7,-1843
    80000030:	00e89893          	slli	a7,a7,0xe
    80000034:	45588893          	addi	a7,a7,1109
    80000038:	00c89893          	slli	a7,a7,0xc
    8000003c:	66788893          	addi	a7,a7,1639
    80000040:	00c89893          	slli	a7,a7,0xc
    80000044:	78888893          	addi	a7,a7,1928       # 0x1122334455667788
    80000048:	00000813          	addi	a6,zero,0
000000008000004c <loop>:
    8000004c:	0112b023          	sd	a7,0(t0)          # store
    80000050:	0002b503          	ld	a0,0(t0)          # load
    80000054:	00a80833          	add	a6,a6,a0
    80000058:	00828293          	addi	t0,t0,8
    8000005c:	fffe0e13          	addi	t3,t3,-1
    80000060:	fe0e16e3          	bne	t3,zero,8000004c <loop>
    80000064:	01033023          	sd	a6,0(t1)          # sig[0]
    80000068:	0103b023          	sd	a6,0(t2)          # TOHOST = non-zero
    8000006c:	0000006f          	jal	zero,8000006c <loop+0x20>
```

Every instruction is rv64im_zicsr_zifencei; the driver re-checks two structural
properties of those bytes rather than assuming them — that there is **no SYSTEM
instruction** anywhere in the image (so the fixture traps nowhere and touches no CSR,
which is what makes "one fetch per instruction" hold for this program) and that the last
word is a **self-loop** the run never fetches, so the last executed instruction is the
TOHOST store. The loop body is deliberately fetched and retired four times: the same
`pc` legitimately appears several times in the stream, which is what keeps §5's identity
detector honest.

## 3. Where each of the three tallies comes from

The card asks for three tallies and for each to be named with its source. They are:

**(a) The DUT's own RTL event stream.** `c_evt_valid` sampled once per cycle in the
settled high phase, one `mosaic::RetireEvent` record per pulse, sequence number = the
harness's record index. 45 records.

**(b) The harness's own handshake tally.** Accumulated in the same sampling loop from
the port levels, and *not* derived from (a): the fetch ack level (`h_if_pending`), the
data ack level (`h_d_pending`) split into reads and writes by the request the ack
answers, and the retire pulse. Leading edges and cycles-high are counted separately, so
a level held for two cycles is visible. 45 fetch accepts, 10 data accepts (6 write, 4
read), 45 retire leading edges.

**(c) The architectural expectation.** Not read from the DUT and not written from the
RTL. The independent RV64IM reference model that `core.bringup_vs_reference` uses is
**extracted at run time** from the `R"PYMODEL(...)"` literal in
`sim/unit/tb_bringup.cpp`, validated against sentinels, written out and executed on this
case's own fixture image. Its summary gives `events = 45` (also `steps` and `minstret =
45`, checked to agree with each other, and `outcome = tohost`, so a truncated reference
run cannot pass quietly). Its *own emitted instruction stream* is then decoded with the
two RISC-V opcodes for LOAD (`0x03`) and STORE (`0x23`) to get 4 loads and 6 stores.

Extracting rather than copying is deliberate: a copy would be a second oracle that could
silently disagree with the one I-008 trusts, and this case's job is to be comparable
with I-008, not to be a third opinion. The cost of that choice is stated in §10.

**And one relation that is a contract, not an observation.** "One fetch per instruction"
holds here because `mosaic_bringup_core.sv` publishes "one outstanding request per port,
one instruction in flight, the core accepts nothing new until the matching ack", and
because the fixture contains no trap and no re-fetch. It is asserted as such and named
as such in the check text ("per the core's one-outstanding-request contract"); it is not
an independent measurement of the core.

## 4. The tallies, and their agreement

```
  reference (I-008 oracle, extracted from tb_bringup.cpp): 45 instructions, 4 loads, 6 stores, tohost=0x4488cd115599de20
  zero-latency   retired_tohost  cycles=155    events=45   fetch_acks=45   data_acks=10   (w=6 r=4) wait=55    tohost store req=153 watch=155 retire=155
  plan B         retired_tohost  cycles=235    events=45   fetch_acks=45   data_acks=10   (w=6 r=4) wait=135   tohost store req=231 watch=233 retire=235
  plan C         retired_tohost  cycles=240    events=45   fetch_acks=45   data_acks=10   (w=6 r=4) wait=140   tohost store req=235 watch=237 retire=240
```

Per run, asserted for all three plans:

| Relation | Value |
| --- | --- |
| tap records == reference instructions | 45 == 45 |
| tap records == the harness's own retire tally | 45 == 45 |
| fetch accepts (leading edges == cycles high) == reference instructions | 45 == 45 == 45 |
| data accepts == reference loads + reference stores | 10 == 4 + 6 |
| write accepts == reference stores; DUT stream's `is_store` count == reference stores | 6 == 6 == 6 |
| read accepts == reference loads | 4 == 4 |
| TOHOST and `signature[0]` == the reference's | equal |

And the two cross-run statements that make the backpressure meaningful:

* **plan B and plan C retire exactly the same stream as the zero-latency run, line for
  line.** Extra latency changes timing and nothing architectural.
* **the backpressured runs are genuinely slower**: 155 cycles and 55 wait-cycles at zero
  latency (exactly 45 fetches + 4 loads + 6 stores, i.e. the one-cycle-per-handshake
  contract), against 235/135 and 240/140. The extra cycles are the core sitting in
  `S_FWAIT`/`S_DWAIT`, not a change in the work done.

## 5. The repeated-sampling detector

Three properties of the sampling loop, each checked over every record of every run:

1. **Identity `(retire_seq, pc)` is never recorded twice.** The sequence number is the
   harness's own record index, so this is a statement about the loop, not about the DUT.
   Because the fixture's loop body retires from the same `pc` four times, a detector
   keyed on `pc` alone would false-fire here; that the check passes is itself evidence
   that it is keyed on an identity.
2. **The sequence numbers are dense with no gap** (`events[i].seq == i`).
3. **No cycle is sampled twice for one handshake**, and no level is held across two
   cycles: for each of the three handshakes the leading-edge count must equal the
   cycles-high count, and the first cycle at which a second sample saw the same level,
   or a level persisted, is recorded and named.

Plus the request/ack pairing from the same loop: no port posts a second request while
one is in flight, and no ack arrives without a request to answer — the latter named with
the first cycle it happened.

All six checks pass in the shipping build; §6 is the evidence that they can fail.

## 6. Negative controls

Four mutants, each `ifdef`-gated and off in the shipping build. Three live in the driver
(`-D` reaches the C++ half), one in the DUT wrapper (`-D` reaches the SystemVerilog
half); `tools/run_sampling_controls.py` passes each define to both halves, builds from an
empty directory, checks the binary differs from the shipping one (`cmp`), and requires
exit 1 *with its own named check as the first failure*.

| Define | Where | Defect injected | First failure |
|---|---|---|---|
| `MOSAIC_SAMPLING_MUTANT_DOUBLE_EVAL` | driver | the high phase is evaluated twice and the sampler re-entered, so `eval()` is used as if it were a clock and one retire is recorded twice | **FAILS, exit 1** |
| `MOSAIC_SAMPLING_MUTANT_REEMIT_RECORD` | driver | the record callback fires twice for one architectural event and re-emits the identical `(retire_seq, pc)` | **FAILS, exit 1** |
| `MOSAIC_SAMPLING_MUTANT_SKIP_SAMPLE` | driver | the timeline advances without re-sampling: the edge is taken but the settled state is not looked at every other cycle | **FAILS, exit 1** |
| `MOSAIC_SAMPLING_MUTANT_STRETCH_ACK` | wrapper | the memory model holds the fetch ack high for two cycles instead of one | **FAILS, exit 1** |

Verbatim first failures:

```
=== MOSAIC_SAMPLING_MUTANT_DOUBLE_EVAL : exit 1
    CHECK FAILED: zero-latency: no cycle was sampled twice for one handshake (fetch=1, data=61, retire=3)
    RESULT FAIL harness.sampling_calibration 3 runs ... 630 cycles, 45 instructions per run, 110 checks
=== MOSAIC_SAMPLING_MUTANT_REEMIT_RECORD : exit 1
    CHECK FAILED: zero-latency: no retire_seq is emitted twice (first duplicate seq 2)
    RESULT FAIL harness.sampling_calibration 3 runs ... 630 cycles, 45 instructions per run, 110 checks
=== MOSAIC_SAMPLING_MUTANT_SKIP_SAMPLE : exit 1
    CHECK FAILED: zero-latency: the tap recorded one event per instruction the reference says the program retires (23 records vs 45 reference instructions)
    RESULT FAIL harness.sampling_calibration 3 runs ... 40155 cycles, 45 instructions per run, 110 checks
=== MOSAIC_SAMPLING_MUTANT_STRETCH_ACK : exit 1
    CHECK FAILED: zero-latency: the fetch ack is a one-cycle pulse (45 leading edges vs 90 cycles high)
    RESULT FAIL harness.sampling_calibration 3 runs ... 630 cycles, 45 instructions per run, 110 checks
```

Each mutant is caught by the check that describes it, and each also trips later checks
(the double sample trips the tallies as well; the re-emit trips the tally as well; the
stretched ack trips the request/ack pairing as well). None of them exits 0, so no check
here is vacuous.

The stretched-ack mutant is worth one sentence on its own: holding the ack one cycle
too long changes **nothing architectural** — the core is in `S_EXEC` on the second cycle
and ignores the port — so only the harness's own handshake tally can see it. That is
exactly the class of defect "one handshake recorded exactly once" is about.

## 7. The wrapper edit, and what it does not change

`sim/tb/mosaic_bringup_tb.sv` gained `h_if_gap`/`h_d_gap` and, per port, a small wait
counter. With both gaps zero — every driver except this one — the generated logic is the
original: request in cycle *N*, ack in cycle *N+1*, ack high for exactly one cycle, and
the store effect and the TOHOST latch at the accept, unchanged. `harness.bad_image`,
`core.bringup_vs_reference`, `reset.replay_determinism`, `loader.elf_boundaries` and
`exit.protocol_termination` were all re-run green afterwards (§8), and the V-009 controls
were re-run as well because the wrapper is shared.

Two properties of the model are worth recording because the case now depends on them:
`h_tohost_written` is a **one-cycle pulse**, not a latch, so a harness must watch it every
cycle or miss it; and the store's memory effect happens when the model *accepts* the
request, not when it acks, so under backpressure the effect lands earlier than the store's
commit. Neither is new — both are the pre-existing model semantics — but neither had ever
been observable before, because nothing had ever delayed a response.

## 8. The defect this case found, and the fix

`mosaic_bringup_core.sv`'s next-state logic set `state_n = S_FETCH` as the case-statement
default and then gave `S_FWAIT` and `S_DWAIT` no self-loop:

```systemverilog
    state_n = S_FETCH;
    case (state_q)
      S_FWAIT:  if (ifetch_ack_i && !ifetch_fault_i) state_n = S_EXEC;
      S_DWAIT:  if (dmem_ack_i && !dmem_fault_i) state_n = S_FETCH;
```

With no ack in a wait state, the state fell through to the default, the core went back to
`S_FETCH`/`S_DREQ` and **re-posted the request every cycle it was waiting** — while the
module's own published memory-interface contract says the ack "may be returned in any
later cycle; the core holds state and ignores every port signal until it arrives". The
defect was invisible for the whole life of the package because every environment in the
tree answers on the very next cycle; it becomes fatal the moment a response is delayed,
which is precisely what V-010's card asks for. Applying backpressure produced 143 fetch
accepts and 37 data accepts for a program with 45 instructions and 10 memory operations,
under a contract that promises one of each.

The fix is one `else` per wait state, so "no ack yet" holds the state:

```systemverilog
      S_FWAIT:  if (ifetch_ack_i && !ifetch_fault_i) state_n = S_EXEC;
                else state_n = S_FWAIT;
      S_DWAIT:  if (dmem_ack_i && !dmem_fault_i) state_n = S_FETCH;
                else state_n = S_DWAIT;
```

Every other driver produces a one-cycle ack, so the `else` is never taken in any of them:
the change is behaviour-preserving for all of them, and that is confirmed rather than
argued — see the re-runs in the status table. The five controls in `results/reports/V-009-reset.md`
were also re-run after the edit: each was built one at a time with
`tools/run_core_controls.py`'s `build(entry, "reset.replay_determinism", [define], dir)`
(that helper passes the define to Verilator, where those mutants live) and executed with
the registered arguments. All five still exit 1. One of them,
`MOSAIC_RESET_MUTANT_STALE_PENDING`, is now first named by an *earlier* check in V-009's
list ("the restarted run retires the instruction at the reset vector first") rather than
by its edge-count check; the edge-count check still fires for that mutant too (11 failed
checks in all), so no V-009 evidence was weakened, but the change of *first* named check
is recorded here because V-009's report quotes the old one.

## 9. Audit: every other driver's clocking discipline

Every `sim/unit/tb_*.cpp` and `sim/tb/*.sv` pair in `tests/unit/registry.json` was read
for three things: whether it follows the low-phase-drive / high-phase-eval / settled-sample
convention, whether it calls `eval()` more than once per cycle, and whether it runs an
end-of-run final check. **No other driver was rewritten** — they are owned by their cases.

Reading key for the table: *evals/cycle* is the number of `eval()` calls the driver makes
per clock period in its shipped form; *final()* is whether it calls Verilator's
`dut.final()`; *end check* is whether it has an end-of-run assertion at all (every one
does, via `Reporter::Finish` after a failures-count check).

| Case(s) | Driver | Clock shape | evals/cycle | More than one eval per cycle? | final() | End check | Verdict |
|---|---|---|---|---|---|---|---|
| `harness.reset_load_exit`, `harness.bad_image`, `harness.timeout`, `harness.injected_mismatch` | `sim/harness/tb_harness.cpp` | harness-driven `clk`; low/eval, then **two** high-phase evals (the falling edge is folded into the next cycle's low phase) | 3 | **yes** — a second high-phase eval after the host-side store write | no | yes | deviates in eval count; the second eval is idempotent (clock unchanged, no DUT input changed), taken *before* the `retire` sample, so no handshake is counted twice |
| `core.bringup_vs_reference` | `tb_bringup.cpp` | low/eval, rising/eval, falling/eval | 3 | no | no | yes | conforms |
| `reset.replay_determinism` | `tb_reset.cpp` | `LowPhase`/`RisingEdge`/`FallingEdge` | 3 | **documented extras only**: two evals with no edge in the release-instant sweep, and one no-edge eval in the synchronous-reset probe | no | yes | conforms; both extras are deliberate probes of "release/eval point does not matter", not sampling |
| `loader.elf_boundaries` | `tb_loader.cpp` | low/eval, rising/eval, falling/eval | 3 | no | no | yes | conforms |
| `exit.protocol_termination` | `tb_exit.cpp` | low/eval, rising/eval, falling/eval | 3 | no | no | yes | conforms |
| `harness.sampling_calibration` | `tb_sampling.cpp` (this package) | low/eval, rising/eval, **sample**, falling/eval | 3 | no (the mutants add one) | **yes** | yes | conforms; the sample is between the rising-edge eval and the falling-edge eval |
| `wb.same_bank_many_producers` | `tb_wb.cpp` | drive, eval, drive answers, eval, rising/eval, falling/eval | 4 | **yes** — stage 1 settles the arbiter, stage 2 settles the answers computed from its outputs | yes | yes | deviates in eval count, by design and commented; the interpolated step is an *input change*, so it is not a re-sample of one settled state |
| `iq.wakeup_insert_select` | `tb_iq.cpp` | `Settle` (low/eval), `Edge` (rising/eval, falling/eval), then a further low-phase eval before reading post-edge state | 4 | **yes** — the extra low-phase eval is redundant (clock already low, no input changed) | no | yes | benign; no sample is taken between the duplicate pair, and the reset loop omits an explicit falling phase (folded into the next cycle's low phase) |
| the remaining 21 unit cases: `fifo`, `ram`, `predictor`, `rename`×2, `rob`, `retire`×2, `recovery`×2, `fetch`, `muldiv`, `prf`, `csr`, `interrupt`, `lease`, `result_fifo`, `remote`, `core`×3, `lsu`, `load`, `store` | `tb_core*.cpp`, `tb_fetch/csr/interrupt/lease/prf/rename/retire/rob/recovery/remote/muldiv/result_fifo/store/load/lsu/predictor/fifo/ram.cpp` | drive inputs with the clock low, low-phase eval for the pre-edge snapshot, rising/eval, falling/eval | 3 | no — `tb_recovery` and `tb_rob` sample (`Capture()`) *between* the rising and falling evals, which is the convention rather than a deviation | 19 of 21 (`fifo` and `ram` do not) | yes | conform |
| `decode.rv64im_reserved` | `tb_decoder.cpp` | none — combinational DUT, no clock, no reset | 1 eval per stimulus word | no | no | yes | does not exercise the cycle convention at all (by design, documented in its header) |
| `alu.boundaries` | `tb_alu.cpp` | none — combinational DUT | 1 eval per vector | no | no | yes | as above |
| `arbiter.forward_progress`, `bypass.local_raw_chain`, `core.mem_program`, `reconfigure.drain_and_generation`, `steering.capacity_locality` | *(no source on disk)* | — | — | — | — | — | registered with drivers that do not exist yet; nothing to audit |

**Global findings.**

* `--timing` is enabled **nowhere** — not in `VERILATOR_FLAGS`, not in any per-case build,
  not in any driver. There is therefore no build in the tree with a pending Verilator
  event to advance, and the card's "--timing without advancing its events" fail mode has
  no instance to point at. Only this case *checks* that, and only for itself.
* **No `sim/tb/*.sv` file contains a delay, a `timescale`, an `initial` block, a
  `$finish`/`$stop`, or a `final` block.** `dut.final()` therefore executes no design
  code anywhere in this project; the 21 drivers that call it are calling an empty
  function, and the 9 that do not are not skipping design behaviour. "The end-of-run
  final assertion actually runs" is consequently a statement about the *harness's*
  end-of-run assertion, which is what §5/§6 make checkable in this case and what no
  other driver states.
* Every driver samples after an edge and never samples one settled level twice in a
  cycle in its shipped form. The two drivers that take more than one `eval()` per cycle
  (`tb_wb`, `tb_harness`) and the one that takes a redundant extra eval (`tb_iq`) all do
  so before their sample point, with the intervening action being an input change or a
  host-side memory write; none of them can double-count a handshake as written.
* The two combinational drivers (`tb_alu`, `tb_decoder`) have no timeline at all, so
  nothing in them is calibrated by this package and nothing in them needs to be.
* One incidental observation, **not** a clocking deviation and not acted on:
  `sim/tb/mosaic_rob_tb.sv` comments that "the runner builds with `--x-initial unique`,
  so an undriven input would be a random value every run". A direct experiment with the
  registered flags (`--x-assign unique --x-initial unique`, 2-state) shows an undriven
  top-level input and an undriven internal net both reading 0 after three clock periods
  in Verilator 5.052, and the five dependent cases pass with the two new input ports
  left undriven by their drivers, which is the same property. The wrapper's constant
  assignment is therefore defensive rather than load-bearing; no file was changed.

## 10. Not covered

Honest limits of this package, in the order they matter:

* **The reference model is extracted from `sim/unit/tb_bringup.cpp` at run time, not
  re-implemented here.** That is independent of the RTL (it is a second implementation in
  another language, written from the spec) and it cannot drift from I-008's oracle, but
  it *is* a dependency on that file's structure: if the `PYMODEL` literal moves or loses
  its sentinels, this case fails by name rather than silently. It also means this case is
  not an independent third opinion on the ISA; it deliberately is not one.
* **This case does not diff the DUT's event stream against the reference field by field.**
  That is I-008's award. Here the streams are compared by count, by the store flag, and
  by the fixture's own TOHOST and signature. Backpressure-invariance is checked by
  comparing the three runs against *each other* line for line, which is a real check but
  is a self-comparison of the DUT under different timing, not an oracle comparison.
* **Only the bring-up core is calibrated.** The out-of-order core's drivers were audited
  (§9) and not exercised; whether `mosaic_core_tb`'s handshake accounting survives
  backpressure is unknown, and its drivers are owned by other cases.
* **`--timing` is not exercised, only excluded.** No case in the tree needs a Verilator
  scheduled delay: the harness owns the clock, so a delay is never needed. Providing a
  `--timing`-using example would put Verilator's scheduler (and its integer time) inside
  the loop being calibrated, which is the card's own fail mode. The assertion that the
  case makes is "Verilator's time never advanced and no event is pending", which is the
  checkable half; the other half of the card's sentence has no instance to demonstrate.
* **The latency patterns are two fixed published sequences, not a randomized campaign.**
  The point of the case is that the *counts* are invariant and the *timeline* is
  predictable for a known latency, so a fixed plan is what makes the cycle-exact
  assertions possible; a randomized latency campaign would be a different case.
* **The `S_FWAIT`/`S_DWAIT` fix is verified by re-running the five dependent cases and the
  five V-009 controls, not by a formal argument.** With a one-cycle ack the new `else` is
  unreachable, and that is the argument; the re-runs are the evidence.
* Five registered cases (`arbiter.forward_progress`, `bypass.local_raw_chain`,
  `core.mem_program`, `reconfigure.drain_and_generation`, `steering.capacity_locality`)
  have no driver on disk yet, so they appear in §9 as unaudited rather than as conforming.
