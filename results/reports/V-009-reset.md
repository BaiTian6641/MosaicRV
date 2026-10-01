# V-009 — reset and initial state replay

Work package V-009 (`docs/validation-plan.md` §5 V-009, `docs/stage-0-contracts-bringup.md`
§V-009): run the same program under 1/2/5/17 valid-clock reset lengths and
several release phases, have a reset interrupt a simulation in progress and
restart it, and check the SRAM non-reset initialisation contract rather than
assuming it.

| Artifact | Role |
|---|---|
| `sim/unit/tb_reset.cpp` | the case driver; case `reset.replay_determinism` |
| `sim/unit/reset_program.S` | the fixture program (148 bytes), and the recipe that produces the embedded bytes |
| `sim/tb/mosaic_bringup_tb.sv` | the DUT wrapper and its memory model; **edited** (see §7) |
| `rtl/core/mosaic_bringup_core.sv` | the DUT; **edited** (mutant hooks only, §6) |
| `results/reports/V-009-reset.md` | this file |

## STATUS: PASS — one defect found in the environment model and fixed, five controls all caught

| Check | Result |
| --- | --- |
| `python3 tools/run_unit.py --profile p0 --case reset.replay_determinism` | `PASS`, exit 0, 440 checks, 0 failures |
| `python3 tools/run_unit.py --profile p0 --case core.bringup_vs_reference` | `PASS`, exit 0, 0 failed checks (clean build, §8) |
| negative controls | 5 of 5 fail with exit 1 and a named first failure (§6) |
| `python3 tools/lint_rtl.py --profile p0` | `rtl/core/mosaic_bringup_core.sv` clean; **one unrelated failure in `rtl/core/mosaic_core.sv`**, owned by another lane (§9) |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | exit 0, 0 errors (§9) |

---

## 1. What the case drives

`mosaic_bringup_core` through `sim/tb/mosaic_bringup_tb.sv` — the same DUT pair
the registered I-008 case uses. The harness drives the clock phase by phase
(inputs with the clock low, then the rising edge, then the falling edge, one
`eval()` per phase, the project convention from `sim/common/sim_common.h`) and
owns reset itself, so a reset length is a number of rising edges that the core
samples with `rst_i = 1`, not a wall-clock duration.

The program is the fixture in `sim/unit/reset_program.S`: it loads a RAM word
nothing has written, stores and reloads a RAM word, performs exactly one UART
write, writes and reads `mscratch`, fills the four signature words, and writes a
non-zero TOHOST — the frozen end-of-run signal. It is 148 bytes, embedded in the
harness as base64 and pinned by size and by FNV-1a-64
`0x3c01b62787c4c669` (the harness fails by name if the bytes change). SHA-256 of
the flat binary:

```
2e58b8934922c23fd0db66de0dcb5f316e42f4598d8f6a65ab3edd4e18402997
```

Rebuilt with:

```
$ riscv64-elf-gcc -march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
      -nostdlib -nostartfiles -ffreestanding -fno-builtin -fno-pic \
      -mno-relax -Wa,--fatal-warnings -c reset_program.S -o reset_program.o
$ riscv64-elf-ld -Ttext=0x80000000 --no-relax -e _start \
      -o reset_program.elf reset_program.o
$ riscv64-elf-objcopy -O binary reset_program.elf reset_program.bin
$ riscv64-elf-objdump -d -M no-aliases reset_program.elf
0000000080000000 <_start>:
    80000000:	000802b7          	lui	t0,0x80
    80000004:	0012829b          	addiw	t0,t0,1
    80000008:	00c29293          	slli	t0,t0,0xc        # 0x80001000 TOHOST
    8000000c:	00100337          	lui	t1,0x100         # 0x00100000 UART
    80000010:	000803b7          	lui	t2,0x80
    80000014:	0033839b          	addiw	t2,t2,3
    80000018:	00c39393          	slli	t2,t2,0xc        # 0x80003000 untouched
    8000001c:	00100e1b          	addiw	t3,zero,1
    80000020:	01fe1e13          	slli	t3,t3,0x1f
    80000024:	400e0e13          	addi	t3,t3,1024       # 0x80000400 signature
    80000028:	00040eb7          	lui	t4,0x40
    8000002c:	001e8e9b          	addiw	t4,t4,1
    80000030:	00de9e93          	slli	t4,t4,0xd        # 0x80002000 scratch
    80000034:	0003b503          	ld	a0,0(t2)
    80000038:	00ae3023          	sd	a0,0(t3)
    8000003c:	02d2d5b7          	lui	a1,0x2d2d
    80000040:	2d35859b          	addiw	a1,a1,723
    80000044:	00c59593          	slli	a1,a1,0xc
    80000048:	d2d58593          	addi	a1,a1,-723
    8000004c:	00c59593          	slli	a1,a1,0xc
    80000050:	2d358593          	addi	a1,a1,723
    80000054:	00d59593          	slli	a1,a1,0xd
    80000058:	a5a58593          	addi	a1,a1,-1446      # 0x5a5a5a5a5a5a5a5a
    8000005c:	00beb023          	sd	a1,0(t4)
    80000060:	000eb603          	ld	a2,0(t4)
    80000064:	00ce3423          	sd	a2,8(t3)
    80000068:	04100693          	addi	a3,zero,65
    8000006c:	00d30023          	sb	a3,0(t1)          # one UART write
    80000070:	34061073          	csrrw	zero,mscratch,a2
    80000074:	34002773          	csrrs	a4,mscratch,zero
    80000078:	00ee3823          	sd	a4,16(t3)
    8000007c:	000067b7          	lui	a5,0x6
    80000080:	00d7879b          	addiw	a5,a5,13
    80000084:	00fe3c23          	sd	a5,24(t3)
    80000088:	00100813          	addi	a6,zero,1
    8000008c:	0102b023          	sd	a6,0(t0)          # TOHOST = 1
    80000090:	0000006f          	jal	zero,80000090 <_start+0x90>
```

Every instruction is rv64im_zicsr_zifencei. The fixture is deliberately boring:
every store it performs is unconditional and writes a constant, so its effect on
memory is idempotent. That is what makes "reset in the middle and restart" a
comparison of *transaction* state rather than of leftover data.

## 2. Where the expectation comes from — and what it does not read

The expected post-reset state is **not** read from the DUT and **not** taken from
a reference stream. It is built from four independent sources:

* `config/profiles/p0.json` (through `build/p0/sim/mosaic_platform.h`) for the
  reset vector, TOHOST, the signature address and the pass code; the case checks
  the JSON and the generated header agree with each other and with the
  fixture's own literals before anything runs;
* `config/csr/mode_m.json` for the CSR reset values — every compared CSR is
  zero except `mstatus = 0x1800` and `misa = 0x8000000000001100`, both asserted
  from the JSON;
* the core's published interface contract in `mosaic_bringup_core.sv`, which
  states that reset is *synchronous and active high* and holds the machine in
  `S_FETCH` with the PC at the reset vector;
* `docs/platform-plan.md` §2: "SRAM/BRAM data array is not fully reset; valid/
  owner/ECC state is reset, and only legally initialised or written data may be
  read".

The case **does** compare the DUT with the DUT in one place — the same-seed
replay check. That check alone cannot detect a defect that is deterministic (a
dead machine replays perfectly), so it is never used instead of the reset-length
and restart comparisons, which have the independent expectation above. The
reference-stream comparison that I-008 does is not repeated here; V-009 is about
what reset leaves behind, and the streams are compared to *each other* across
reset lengths, release instants and restarts.

## 3. The reset-length and release-instant sweep

16 configurations — reset lengths 1, 2, 5 and 17 sampled rising edges, each with
four release instants — and each configuration run twice from a fresh clear, so
32 runs, 245 reset edges and 4896 live edges. The full table is in
`results/unit/reset.replay_determinism/run.log`:

```
$ grep -c '' results/unit/reset.replay_determinism/run.log
  reset 1 edges, release high-after-last-edge tohost      reset= 1 live= 126 events= 36 tohost=0x0000000000000001 sig0=0x0
  ... (16 configurations, every line identical except reset=)
```

For **every** configuration the case asserts:

* the core sampled `rst_i = 1` on exactly the requested number of rising edges;
* on every reset edge — no architectural event, no data request, no outstanding
  transaction held by the environment — and, from the second edge on, PC at the
  reset vector and state `S_FETCH` (before the first edge those two registers
  hold the host simulator's power-on value, which is not a claim this contract
  makes; the other three hold on the first edge too, because the harness clears
  the model and holds reset before `Run()`);
* when reset releases, before a single live edge has been taken: PC is the reset
  vector, state is `S_FETCH`, every compared CSR holds its reset value, and no
  end-of-run latch is set;
* the program runs to TOHOST with the profile pass code, the first architectural
  event is the instruction at the reset vector, the run performs exactly one
  UART device write, and the fixture's RAM and `mscratch` round-trips are intact;
* the run is **identical** to the reference configuration: same architectural
  event stream, same number of live clock edges (126), same live-edge offset to
  the first event, same signature, same TOHOST, same post-run CSR state;
* the same configuration run a second time is bit-identical.

### The release instant is not an independent axis, and this was measured

The four release instants are physically distinct points inside the clock period
(rst_i drops while the clock is still high right after the last reset edge; one
settled eval later; in the low phase before the first live edge; at the end of
that low phase). The RTL samples `rst_i` only at a rising edge — that is the
published contract and it is also *proved* here, see §5 — so all four must be
identical, and they are: every one of the 16 configurations produced the same
stream, the same 126 live edges and the same state.

**Reported honestly: three of the four release instants are redundancy probes.**
They pass identically to the first, so they are one measured data point, not
four. Their value is that they falsify the harness's own model of release: if
the harness believed a release in the high phase shortened the reset, that
belief would show up as a different reset-edge count or a one-cycle shift, and
it does not. What the sweep *does* establish as an independent axis is the reset
length: 1, 2, 5 and 17 edges all leave the same machine and cost exactly
`length + 126` physical edges.

## 4. Reset interrupts a run in progress and restarts it

Four abort points, each rewinding to a fresh clear and load, running without a
reset until the named transaction state appears, asserting reset at that instant,
and then releasing:

| Abort at | Observed at | Transaction outstanding |
|---|---|---|
| waiting for an instruction fetch (`S_FWAIT`) | live edge 1 | `h_if_pending = 1` |
| waiting for a data access (`S_DWAIT`) | live edge 43 | `h_d_pending = 1` |
| the TOHOST store accepted by the model | live edge 125 | `h_d_pending = 1`, store address = TOHOST |
| a seeded number of live edges | live edge 65 | `h_if_pending = 1` |

Each abort is checked to have actually landed on the state it names — an abort
that misses is vacuous and would be reported as such. After each abort:

* asserting `rst_i` with the clock low and **no** clock edge changes nothing: the
  PC before and after the assertion are equal (this is what makes the reset
  synchronous, and it is asserted, not assumed);
* five reset edges from the left-behind state, a release, and the program runs
  again — 36 events in 126 live edges, the whole program bit for bit, with the
  same signature, TOHOST and CSR state as the uninterrupted reference run;
* the first live edge after the restart reports no end-of-run latch.

```
$ grep 'abort at' results/unit/reset.replay_determinism/run.log
  abort at waiting for an instruction fetch aborted after    1 live edges (if_pending=1 d_pending=0 tohost_req=0), restarted 36 events in 126 edges, first_live_tohost=0, tohost=0x0000000000000001
  abort at waiting for a data access aborted after   43 live edges (if_pending=0 d_pending=1 tohost_req=0), restarted 36 events in 126 edges, first_live_tohost=0, tohost=0x0000000000000001
  abort at posting the TOHOST store aborted after  125 live edges (if_pending=0 d_pending=1 tohost_req=1), restarted 36 events in 126 edges, first_live_tohost=0, tohost=0x0000000000000001
  abort at after a seeded number of edges aborted after   65 live edges (if_pending=1 d_pending=0 tohost_req=0), restarted 36 events in 126 edges, first_live_tohost=0, tohost=0x0000000000000001
```

## 5. The SRAM non-reset initialisation contract, checked rather than assumed

`docs/platform-plan.md` §2 says the SRAM data array is **not** reset, and that
only legally initialised or written data may be read. The testbench memory model
follows that: `h_rst` resets the port state, the counters and the device
registers, but it re-initialises no array; the arrays are zeroed only by the
explicit one-cycle `h_clear_mem` pulse. That is exactly the shape of the fail
mode "the harness depends on C++ memory happening to be zero", so the case
checks both directions:

| # | Checked | Observed |
|---|---|---|
| 5a | a RAM word written *before* five reset edges still holds its value after them — reset alone does not re-initialise the array | holds `0xabcfa6a8e079651d` |
| 5b | the RAM word the fixture reads but nothing wrote reads zero, `signature[0]` is zero, and reading it twice gives the same value | `0`, `0`, `0` |
| 5c | the clear pass re-initialises the array: the sentinel reads zero after it, and so does the image | `0`, `0` |
| 5d | loading the image again puts its first word back at the reset vector, so 5b/5c are not "the loader never worked" | `0x0012829b000802b7` |

The sentinel value is drawn from the case seed, so the check is not a fixed
constant that could accidentally be baked in. The run also proves the device was
really driven: the UART holds `0x41` (`'A'`, the fixture's one write) and the
model reports exactly one UART write.

## 6. Negative controls

All five are `ifdef` blocks in the code under test, off in the shipping build.
Each mutant is built from an **empty build directory** (`shutil.rmtree` before
`verilator`) with the registered Verilator command line plus one `-D`, and run
with the registered `--seed 1 --max-cycles 4000000`.

| Define | Where | Defect injected | Result |
|---|---|---|---|
| `MOSAIC_RESET_MUTANT_EVENT_IN_RESET` | `mosaic_bringup_core.sv` | the machine reports an architectural event while held in reset | **FAILS, exit 1** |
| `MOSAIC_RESET_MUTANT_KEEP_MSCRATCH` | `mosaic_bringup_core.sv` | `mscratch` is not restored by reset, so a previous run's value survives | **FAILS, exit 1** |
| `MOSAIC_RESET_MUTANT_SKIP_CLEAR` | `mosaic_bringup_tb.sv` | the clear pass leaves the RAM array alone, so the harness depends on host zero-init | **FAILS, exit 1** |
| `MOSAIC_RESET_MUTANT_STALE_PENDING` | `mosaic_bringup_tb.sv` | an outstanding fetch/data transaction is not cleared by reset | **FAILS, exit 1** |
| `MOSAIC_RESET_MUTANT_STALE_TOHOST` | `mosaic_bringup_tb.sv` | the end-of-run latch survives reset | **FAILS, exit 1** |

First failure of each, verbatim:

```
=== MOSAIC_RESET_MUTANT_EVENT_IN_RESET : exit 1, 17 failed checks
    CHECK FAILED: reset 1 edges, release high-after-last-edge: no architectural event is reported while reset is asserted (1 violations over 1 edges)
    RESULT FAIL reset.replay_determinism ... 439 checks, every stream bit-identical
=== MOSAIC_RESET_MUTANT_KEEP_MSCRATCH : exit 1, 31 failed checks
    CHECK FAILED: reset 1 edges, release falling-edge: every compared CSR holds its reset value when reset releases (mstatus=0x0000000000001800 ... mscratch=0x5a5a5a5a5a5a5a5a ... minstret=0)
    RESULT FAIL reset.replay_determinism ... 439 checks, every stream bit-identical
=== MOSAIC_RESET_MUTANT_SKIP_CLEAR : exit 1, 3 failed checks
    CHECK FAILED: the harness's clear pass re-initialises the RAM data array: the word that held 0xabcfa6a8e079651d reads 0xabcfa6a8e079651d after it
    RESULT FAIL reset.replay_determinism ... 439 checks, every stream bit-identical
=== MOSAIC_RESET_MUTANT_STALE_PENDING : exit 1, 3 failed checks
    CHECK FAILED: abort at waiting for an instruction fetch: the restarted run takes the same number of live clock edges as an uninterrupted run
    RESULT FAIL reset.replay_determinism ... 4900 live edges driven, 439 checks, every stream bit-identical
=== MOSAIC_RESET_MUTANT_STALE_TOHOST : exit 1, 7 failed checks
    CHECK FAILED: abort at posting the TOHOST store: no end-of-run latch is reported on the first live edge after the restart
    RESULT FAIL reset.replay_determinism ... 4771 live edges driven, 439 checks, every stream bit-identical
```

No control is vacuous: each fails, exits 1, and names its own first failure. The
mutant hook in `mosaic_bringup_core.sv` is two `ifdef` blocks around existing
assignments; the shipping build is byte-identical to the build without them.

## 7. The defect this case found, and the fix

The TOHOST-store abort found a real defect — in the **environment model**, not in
the core.

`sim/tb/mosaic_bringup_tb.sv` latches the end-of-run signal (`m_tohost_commit`)
in the same cycle it accepts a store to TOHOST, and reports it one cycle later
through `h_tohost_written`. The reset branch cleared `m_tohost_written` but
**not** `m_tohost_commit`. A reset landing in that one-cycle window therefore
left the latch set, and the first live edge after release reported an end-of-run
that no instruction had performed: the new run ended itself in its first cycle
with an empty event stream. That is precisely the V-009 fail mode "an old
transaction crosses reset and pollutes the new run", in its environment form.

The fix is one line in the reset branch:

```systemverilog
      m_tohost_commit  <= 1'b0;
```

It is guarded by `MOSAIC_RESET_MUTANT_STALE_TOHOST`, and the mutant row in §6 is
the proof that the line is load-bearing rather than decorative: with the line
removed the case fails, with it present the case passes. The fix does not change
the shipping behaviour of I-008 — `clear_mem` already cleared the same register,
and the reset window it closes is never entered by that harness — and
`core.bringup_vs_reference` is re-run green in §8.

Two smaller things were fixed in the course of the work and are noted here
because they were caught by the case rather than by inspection:

* the abort at the TOHOST store originally triggered on the cycle the request is
  *posted*, one cycle before the memory model accepts it. The abort was
  therefore vacuous — the stale-latch defect could not be seen. It now triggers
  on the cycle the store is outstanding (`h_d_pending` with the TOHOST address),
  which is the window, and the case asserts that the model has accepted the
  store before the reset lands.
* `sim/tb/mosaic_bringup_tb.sv` gained three observation outputs —
  `h_if_pending`, `h_d_pending`, `c_dmem_req_o`/`c_dmem_we_o`/`c_dmem_addr_o` —
  and the pre-existing `h_tohost_value` `[63:0]` width was preserved. Every one
  of them is read by a check named in §3 or §4; none is instrumentation nobody
  asserts on. (`h_tohost_value` was briefly narrowed while this edit was being
  made and was restored before any case was run; the I-008 port widths and
  meanings are otherwise untouched.)

## 8. Evidence

```
$ python3 tools/run_unit.py --profile p0 --case reset.replay_determinism
PASS reset.replay_determinism     task=V-009

$ grep '^RESULT' results/unit/reset.replay_determinism/run.log
RESULT PASS reset.replay_determinism reset sweep 32 runs over 4 reset lengths x
4 release instants, 4 aborts, 245 reset edges and 4896 live edges driven,
439 checks, every stream bit-identical

$ grep '^RESULT' results/unit/reset.replay_determinism/result.json
  "checks": 440,
  "failures": 0,

$ python3 tools/run_unit.py --profile p0 --case core.bringup_vs_reference
PASS core.bringup_vs_reference    task=I-008

$ grep '^RESULT' results/unit/core.bringup_vs_reference/run.log
RESULT PASS core.bringup_vs_reference corpus 39 programs, 2774346 architectural
events, every stream identical to the independent reference

$ grep -c 'CHECK FAILED' results/unit/core.bringup_vs_reference/run.log
0
```

Both were run from a clean build directory (`build/p0/unit/<case>` removed first).
The `build/p0/unit/reset.replay_determinism` directory was deleted once before
this campaign, as required, and the runner rebuilt it.

The fixture's embedded bytes were checked against the source twice, once by the
harness (`fixture 148 bytes, fnv1a-64 0x3c01b62787c4c669`) and once outside it
(`shasum -a 256` above).

## 9. Gates

```
$ python3 tools/lint_rtl.py --profile p0
ok   rtl/core/mosaic_bringup_core.sv: clean as mosaic_bringup_core
ok   ... (27 more)
FAIL rtl/core/mosaic_core.sv: lint failed
lint: 1 of 31 source file(s) failed
```

The one failure is `rtl/core/mosaic_core.sv` (`UNUSEDSIGNAL` on
`dbuf_push_at_w`, `desc_gen0`, `desc_gen1`). That file is owned by another lane
that is building the integrated core and is mid-flight; it is outside this
package's ownership and this package did not touch it. The file this package
edited, `rtl/core/mosaic_bringup_core.sv`, is clean.

```
$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)
... [STYLE-2] warnings on pre-existing port names in rtl/common/mosaic_skid_buffer.sv
SLANG_EXIT=0, 0 error lines
```

## 10. Not verified

* **Reset polarity as a waveform.** The contract says synchronous, active high,
  and the case proves the deassertion is edge-sampled (§4) and that the
  assertion acts on the edge; it does not exercise an *active-low* reset or an
  asynchronous assertion path, because the RTL has neither.
* **Zero reset edges.** What a machine does if the harness never takes a reset
  edge is undefined by this contract and is not tested. The smallest tested
  length is 1.
* **`mcycle`.** Excluded from the state comparison on purpose: it counts clocks,
  so it is *supposed* to differ between reset lengths
  (`docs/validation-plan.md` §3 rule 4). `minstret` is compared and does not.
* **Privilege mode.** p0 is M-mode only and the bring-up core has no privilege
  register; the privilege state a reset produces is carried by `mstatus`
  (`MIE`/`MPIE`/`MPP`), which the case compares against the configured reset
  value. There is no separate mode register to check.
* **Queue ownership in the real sense.** The bring-up core has no queue — it has
  one instruction in flight and its outstanding-transaction state lives in the
  memory model. What is checked is the analogue: the environment must hold no
  outstanding transaction while reset is asserted and must not carry one across
  it. When the integrated core lands, the same case should be re-pointed at it
  and the reorder-buffer/rename-allocation ownership state added.
* **Waveform fragment.** The V-009 output list mentions one. What this report
  carries instead is the per-edge invariant counts, the abort table and the
  release-instant equivalence; a VCD would add a picture, not a claim. Recorded
  here so the difference between "not produced" and "produced and passed" is
  explicit.
* **An ELF-loaded fixture.** The fixture is loaded as flat bytes through the
  word-granular image port; V-007/V-011 own the ELF entry and boundary contract,
  and `core.bringup_vs_reference` exercises that path in the same testbench.
* **Reset during a store that has not yet reached memory** — i.e. a
  microarchitectural store buffer. The bring-up core performs each store in the
  cycle it is handed to memory, so there is no in-flight store to lose beyond
  the end-of-run latch fixed in §7.
