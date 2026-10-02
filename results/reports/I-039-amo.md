# I-039 — the A-extension atomic path

Case: **`CASE=amo.linearization`** (registered in `tests/unit/registry.json`, task I-039,
top `mosaic_core_tb`, driver `sim/unit/tb_core_amo.cpp`).

Status: the case builds from a deleted build directory and passes on the shipping
RTL; three `-D` controls fail with named first failures and different binary hashes.
The report's last section is the honest account of what is *not* covered.

---

## 1. The rule

**Which encodings are decoded.** The A extension's opcode `0101111` is decoded in the
**core's front end** (`rtl/core/mosaic_core.sv`, the `dbuf_ctl_new` `always_comb`),
not in `mosaic_decoder.sv`. This is the same ownership split WFI already uses and it
is deliberate: `CASE=decode.rv64im_reserved` enumerates every opcode outside RV64IM as
illegal, and opcode `0101111` is outside RV64IM. Adding it to the decoder would falsify
that case's reference model. The decoder is therefore untouched by I-039 and its case
still passes.

A word is a legal AMO exactly when

```
insn[6:0]  == 0101111                      (opcode)
insn[14:12] ∈ {010 (AMO*.W), 011 (AMO*.D)} (width; every other funct3 is reserved)
insn[31:27] ∈ {00000 AMOADD, 00001 AMOSWAP, 00100 AMOXOR, 01100 AMOAND,
               01000 AMOOR, 10000 AMOMIN, 10100 AMOMAX, 11000 AMOMINU, 11100 AMOMAXU}
```

LR and SC (`funct5` `00010`/`00011`) share the opcode and are **left illegal** here;
they are I-040's, and a reserved funct5 that half-works is a reserved encoding that
becomes a bug. A reserved word leaves the decoder's fully-illegal control word intact
rather than half-decoding.

**What the instruction means.** `rd` receives the *old* value (sign-extended for
AMO*.W, exactly as `lw` sign-extends); the operand is `rs2`; the address is `rs1`
alone — an AMO has no immediate field, so the front end leaves `imm = 0` and the
dispatch entry carries `rs2` in its payload slot. The comparison operations'
signedness is a property of the *operation* (`AMOMIN`/`AMOMAX` signed vs
`AMOMINU`/`AMOMAXU` unsigned), never of the access. `aq`/`rl` are `insn[26]`/`insn[25]`.

**Where the atomicity is.** An AMO is issued through the **load queue**, and that is
load-bearing: the load queue returns a value to a destination register before
retirement, and — because it issues only its head — an AMO sitting at the head blocks
a younger load to the same address from overtaking it. It must *not* be a store class:
a store completes at allocation and drains after retirement, which would write memory
before the atomic read was performed.

The serialization point is `mosaic_lsu_endpoint`. It performs the read-modify-write as
**one indivisible operation**:

```
IDLE --accept--> REQ --mem ready--> WAIT --mem response--> [AMO] AMO_W --mem ready-->
                                           AMO_WAIT --mem response--> DONE
```

* `REQ` presents the **read beat** (the read of the aligned doubleword).
* `WAIT` latches the doubleword and moves straight to `AMO_W` — the endpoint is **not
  in `ST_IDLE` between the beats**, so it accepts no other request and offers the
  memory port to nothing else. The two beats are adjacent on the port with no other
  access between them.
* `AMO_W` presents the **write beat**: the new field is computed by
  `rtl/core/mosaic_amo_alu.sv` from the latched doubleword and the operand, and shifted
  into the window by lane so no neighbouring byte moves.
* `AMO_WAIT` completes the transaction and reports the **old** value.

The whole AMO is additionally routed through the **same non-speculative, exactly-once
serializer a device access uses** (`mosaic_core.sv`): it is presented only when it is
the ROB head and cannot be squashed, and it is a barrier for everything else while it
is held. That is why an AMO's side effect is never performed on a wrong path and never
performed twice.

The atomic attribute travels to the memory system on the port: `mem_req_t` carries
`amo`, `amo_op`, `aq` and `rl`, set on **both** beats, so the two beats are
attributable to one atomic operation and `aq`/`rl` are carried rather than dropped
after decode. `lsu_req_t` carries the same four fields plus the operand (in
`store_data`).

**The one record the load queue cannot carry.** The load queue's entry is a frozen
interface and has no room for the operation, the ordering bits or the operand.
`rtl/core/mosaic_amo_unit.sv` holds the one AMO the queue is currently carrying, keyed
by the macro's whole identity (hart + rob_index + rob_gen + uop_index); the integration
re-presents it when the queue offers that identity. At most one AMO is resident at a
time (`busy` refuses a second allocation at dispatch); a throughput choice, not a
correctness one — ordinary loads still queue behind the AMO.

---

## 2. What the case does

`sim/unit/tb_core_amo.cpp` builds a 30-binding RV64IM program by hand (no ELF, no
assembler) covering **every operation at both widths** with boundary operands:
AMOADD overflow and sign boundary, AMOSWAP, the three bitwise ops, and
AMOMIN/AMOMAX (signed) against AMOMINU/AMOMAXU (unsigned) with the seed's or the
operand's top bit set, at both widths; `aq` and `rl` set on several bindings,
including both at once.

* **Run A — atomic coverage.** The per-instruction retirement stream is compared,
  pc/rd/value, against an independent RV64IM+A interpreter (`sim/unit/mem_ref.h`)
  running on its own memory model seeded identically; the final atomic scratch area is
  compared word for word. Every AMO instruction is then required to appear at the data
  port as **exactly two beats, both marked atomic**, at the instruction's address, with
  its width, its operation and its `aq`/`rl`; the write's payload is independently
  required to equal the operation applied to the value the read returned.
* **Run B — the same history under a competing agent.** The harness performs an
  ordinary 8-byte store to the first atomic location on every cycle the core has
  *released* the memory port (`o_mem_lsu_busy` low and no request presented) — the
  honest model of a cooperative second master on a single-port memory. The whole
  data-port history (core transactions **and** the competitor's stores, in order) is
  then replayed against a shadow memory: every read must match the shadow, and every
  atomic write must (a) find the shadow unchanged since the atomic read it belongs to
  and (b) equal the operation applied to that read. A history that survives the replay
  **is a legal linearization**: the atomic steps are indivisible and the competing
  stores are ordered around them, never inside them.

Measured, shipping build (`results/unit/amo.linearization/run.log`):

```
amo.linearization: 30 AMO bindings, run A 158 retires / 150 txns / 770 cycles,
                   run B 158 retires / 191 txns / 770 cycles
RESULT PASS amo.linearization checks=699 amo_bindings=30 run_a_txns=150
            run_b_txns=191 cycles=1540 seed=1
```

The 150 transactions of run A are exactly `30 × (2 ordinary loads + 1 ordinary store +
2 atomic beats)`; run B adds 41 competing stores. Run A's 158 retires equal the
independent reference's trace length.

---

## 3. Controls

Built by hand from a deleted `obj_dir`, `-D` inserted into the same Verilator command
line the shipping case uses, into a separate build directory so the shipping binary is
untouched. Shipping binary `sha256[:16] = dfc1fd46f1a1952a`.

| `-D` control | what it breaks | first failure | exit | sha256[:16] |
|---|---|---|---|---|
| `MOSAIC_AMO_MUTANT_SPLIT` | `rtl/core/mosaic_lsu_endpoint.sv`: the endpoint completes the read as an ordinary load, returns to `ST_IDLE` (releasing the port) and issues the write a cycle later as an ordinary store — the "load+store decomposition" | `every AMO instruction is exactly two data-port beats (a read and a write): expected 60, saw 0` | 1 | `b450aa628193eb95` |
| `MOSAIC_AMO_MUTANT_IGNORE_AQR` | `rtl/core/mosaic_lsu_endpoint.sv`: `aq`/`rl` are dropped from the memory beat, so they are hints | `amoadd.d: both atomic beats carry the instruction's aq/rl` | 1 | `83c19da1d2fa7158` |
| `MOSAIC_AMO_MUTANT_MINMAX_SIGNED` | `rtl/core/mosaic_amo_alu.sv`: the signed and unsigned comparisons are exchanged (`AMOMIN`/`AMOMAX` compare unsigned, `AMOMINU`/`AMOMAXU` signed) | `the atomic scratch slot 6 (amomin.d) expected 0xffffffffffffffff, got 0x0000000000000000` | 1 | `60c711c3933b969c` |

Each mutant's hash differs from the shipping hash and the failure is named before the
run reports PASS, so a silent pass is impossible.

Why the SPLIT control fails on the *structure* first: dropping the atomic marking makes
the two beats ordinary accesses, and the case's first structural check needs 60 atomic
beats. Under contention the same mutation would also fail the replay — a competing
store lands in the released window and the write, computed from the stale read, does
not find the shadow unchanged. Run A aborts before run B, so the structural failure is
what is reported.

---

## 4. Gates

* `python3 tools/lint_rtl.py --profile p0` — **38 of 38 source files clean**.
* `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv')` —
  exits 0; the only warnings are the pre-existing `STYLE-2` port-suffix ones in
  `mosaic_steering.sv`.
* `python3 tools/check_records.py` — green (43 delivered packages, 53 registered cases).
* Cases re-run: `wb.same_bank_many_producers` **PASS**, `completion.fu_collision`
  **PASS**, `steering.capacity_locality` **PASS**, `iq.wakeup_insert_select` **PASS**,
  `fabric.fixed_two_cluster` **PASS**, `decode.rv64im_reserved` **PASS**.

### One red neighbour, not this change

`core.corpus_sweep` and `core.mem_program` FAIL on the shared tree at the time of
writing: the machine stops after a handful of retires on plain corpus programs
(`core.mem_program`: "stopped=1 ... retired=50"; `core.corpus_sweep`: 39 runs, 0 PASS,
0 FAIL, 39 STOPPED, every one refused at its first ordinary instruction). These are
**not** I-039's: the AMO decode arm triggers only for opcode `0101111`, and the refused
words are ordinary loads/stores (`0x00050023`) and ALU forms. A sibling lane is landing
I-041 (RV64C decompression) across `mosaic_decoder.sv` / `mosaic_fetch.sv` /
`mosaic_core.sv` concurrently; `fabric.fixed_two_cluster` was failing with the same
front-end signature earlier in the session and now passes, which is the same work
moving. `amo.linearization`, `core.mem_program` and `core.corpus_sweep` all share the
front end, so the two corpus cases cannot be attributed until that lane lands.

---

## 5. Not covered (honest list)

* **LR and SC.** `funct5` `00010`/`00011` are decoded as illegal, deliberately; the
  reservation manager and `lrsc.reservation_progress` are I-040. The case does not
  present them.
* **Concurrency between two harts.** p0 has one hart and one memory port. The
  "competing agent" is a cooperative second master the harness injects when the core
  has released the port; it is enough to show the read-modify-write is indivisible, but
  it is not a second hart and does not exercise multi-master arbitration.
* **`aq`/`rl` ordering *effects*.** Because the path is conservatively ordered — the
  AMO is presented only at the ROB head, the store drain has priority at the port mux,
  and the load queue issues in order — the ordering `aq`/`rl` demand holds structurally,
  so a plain AMO and an `aq`/`rl` AMO produce the same memory order in this profile.
  The case therefore verifies that each atomic transaction **carries** the
  instruction's `aq`/`rl` (`MOSAIC_AMO_MUTANT_IGNORE_AQR` moves exactly that), and that
  no younger access is observed between the beats; it does **not** demonstrate a
  relaxation that `aq`/`rl` would forbid. A design with a relaxing path would need that
  test.
* **Misaligned AMO.** The p0 policy traps it (`"atomic": "trap"`), the endpoint decides
  from the address alone before the memory is asked, and it reports *store/AMO*
  misaligned (cause 6), not load misaligned. The case's program is aligned
  throughout; the trap path itself is not exercised here.
* **AMO to a device region.** An AMO to MMIO would be serialized (the AMO predicate is
  OR-ed into the serializer's) and carry the PMA device attribute; no case drives it,
  and the plan does not claim atomic MMIO.
* **Speculative disambiguation (I-036).** The load queue never passes an older store
  whose address is unknown, and the AMO blocks younger loads at the head, so no
  memory-disambiguation machinery is involved. A blocking behaviour for a younger load
  behind an AMO is exercised implicitly, not measured.
* **Store-to-load forwarding into an AMO.** The case's AMOs target addresses whose only
  earlier writer has already drained (the AMO runs at the ROB head), so forwarding
  never sources an AMO byte. The load queue is not taught to disable forwarding for an
  AMO, because the frozen interface carries no atomic bit; that is stated here rather
  than asserted as safe.
* **The `amo.linearization` registry entry.** It lists the case's sources; the new
  modules `mosaic_amo_unit.sv` and `mosaic_amo_alu.sv` are resolved by Verilator from
  `-I rtl/core` and need no registry edit. The entry still carries `"pending": true`,
  which the integration lead removes when the package is recorded.
* **A corpus program with AMOs.** No assembled A-extension program exists in
  `tests/programs/**`; the case assembles its own image. A corpus binding is not part of
  I-039.