# I-040 — the LR/SC reservation and its progress condition

Case: **`CASE=lrsc.reservation_progress`** (registered in `tests/unit/registry.json`,
task I-040, top `mosaic_core_tb`, driver `sim/unit/tb_core_lrsc.cpp`).

Status: the case builds from a deleted build directory and passes on the shipping
RTL; four `-D` controls fail with named first failures and different binary
hashes. The report's last section is the honest account of what is *not* covered.

---

## 1. The reservation contract

### What is reserved

One hart, one reservation: a validity bit and the tag of the granule it covers.
The **granule is the naturally aligned 64-byte block** that contains the LR's
address — `mosaic_reservation.GRANULE_BITS = 6` is the single place that number
is written. The plan's memory row asks for a reservation set of at most 64
contiguous bytes, and RVA23's `Za64rs` asks for at least a 64-byte reservation
set, so the declaration and the requirement are the same number.

### The encodings

`lr.w`/`lr.d` and `sc.w`/`sc.d` share opcode `0101111` with the nine AMO
operations and are recognised by `funct5` (`00010`/`00011`) in the **core's front
end** (`mosaic_core.sv`, the `dbuf_ctl_new` `always_comb`), exactly as I-039's
AMOs are. This is an ownership decision, not taste: `CASE=decode.rv64im_reserved`
enumerates every opcode outside RV64IM as illegal and opcode `0101111` is outside
RV64IM, so the decoder (`mosaic_decoder.sv`) is untouched and its case still
passes. A reserved word — a bad `funct3`, or a `funct5` that is none of the nine
AMO operations, `lr` or `sc` — leaves the decoder's fully-illegal control word
intact rather than half-decoding.

`funct3` `010`/`011` select the word/doubleword width. `lr` has no second source
(`uses_rs2 = 0`), so rename presents its reserved `rs2` field as a ready `x0`
rather than waiting on a register the instruction never reads. `aq`/`rl` are
`insn[26]`/`insn[25]` and travel with the transaction.

### What each instruction means

* **`lr`** reads the location and returns the old value (`lr.w` sign-extends it,
  exactly as `lw` does), and — only if the read did not fault — establishes the
  reservation on the granule containing the address. Its response is a load's
  response, one beat on the port.
* **`sc`** is decided **before the memory system is asked**. The reservation
  manager answers combinationally, and:
  * on a **miss** the endpoint never leaves `ST_IDLE` for `ST_REQ`: it builds the
    response (`rd = 1`) and moves to `ST_DONE`. **The memory system does not see
    the instruction at all**, which is the structural statement of "an SC with no
    reservation does not write".
  * on a **hit** it performs exactly **one write beat** — the same `ST_REQ` /
    `ST_WAIT` states an ordinary store uses, so there is no separate write path to
    keep in step — and returns `rd = 0`.
  * either way it **consumes** the reservation: an SC pairs with the most recent
    LR, so a second SC without an intervening LR fails.

A *failure* therefore means "no memory access was performed and `rd = 1`". It is
not an error, not a trap, and not a write of zero.

### The invalidation sources

Four, and each is a named input of `mosaic_reservation`:

| source | signal | what it is |
|---|---|---|
| this hart's own writes | `own_write_valid_i` | asserted on the offered **write beat** of the endpoint's port, so one strobe covers an ordinary store, an AMO's write beat and a successful SC's write beat — "a store, an AMO, or an SC" cannot be three rules that disagree |
| another agent's write | `ext_write_valid_i` | the coherence notification (below) |
| any SC this hart executes | `consume_valid_i` | success or failure; it consumes the pairing |
| an exception / context switch | `flush_valid_i` | wired from the core's redirect, which is what a trap or a context switch asserts |

A write whose byte range does **not** intersect the granule does not clear the
reservation. The ISA would permit an implementation to be more pessimistic (an SC
may fail for any reason), but here over-invalidation is an observable defect: it
turns a store to an unrelated address into a spurious SC failure, and the fourth
control below is exactly that mutation. The rule is stated once, in `touches()`,
and both write sources use it. The range test compares the granule of the first
*and* of the last byte, so a range that starts outside the granule and ends
inside it is caught rather than missed.

A clear wins over a set in the same cycle: the LR's read has already happened, so
a write arriving with it is ordered *after* the read.

### Where the reservation lives, and why there

`mosaic_reservation` is instantiated **inside `mosaic_lsu_endpoint`**, because the
endpoint is the only structure that can see every event the reservation is a
function of: it performs this hart's stores, AMOs and SCs, it is where another
agent's writes are notified to, and it is where the LR's read completes. The
reservation is therefore never a second copy of a fact some other module owns.

### Misalignment

The p0 policy traps it. The endpoint decides from the address alone, before the
memory is asked, and reports **store/AMO address misaligned (6)** for an SC and
**load address misaligned (4)** for an LR — an LR is a load and an SC is a
conditional store, and the privileged spec's cause 6/7 fold covers the atomic
whose effect is a write. The case's programs are aligned throughout; the trap
path itself is not exercised here (it is I-033's boundary).

### The load queue, and why it had to learn about the atomic class

An LR/SC (and an AMO) is issued through the load queue — that is what returns a
value to `rd` before retirement and what blocks a younger load behind it. But a
load queue can also answer a load **out of the store queue**, without touching
the endpoint. An LR answered that way never reaches memory and never establishes
a reservation; an SC answered that way never performs its write. I-039's report
recorded this as a stated gap ("the load queue is not taught to disable forwarding
for an AMO"). I-040 closes it: `mosaic_amo_unit` publishes the identity of the
atomic macro it holds, `mosaic_load_queue` compares it against its own head, and
an atomic head forwards no byte and is never replayed — it goes to memory and its
response completes it. The four AMO controls and `amo.linearization` still pass
with the gate in place.

---

## 2. The case

### The programs

Two hand-assembled RV64IM+A images; the expectations are written next to the
instructions that must produce them, from the ISA. There is no interpreter and no
comparison against a previously recorded run.

**Run A — the reservation semantics.** Straight-line, in order:

1. an SC with **no reservation at all** (`rd` must be 1 and memory must not
   change) — and note the seed store to the same address immediately precedes it,
   so this is also the case where the load queue must not answer an SC from a
   store;
2. an LR that returns the old value and establishes the reservation, then the SC
   that **succeeds exactly once and writes exactly one value**;
3. a second SC with no intervening LR — it must fail;
4. a store **inside** the granule (A+8) — the following SC must fail;
5. a store **outside** the granule (A+64) — the following SC must **succeed**;
6. an **AMO** to the reserved address — the following SC must fail;
7. an **ECALL** between the LR and the SC — the exception breaks the reservation
   and the following SC must fail.

**Run B — the constrained LR/SC loop.** `lr; nops; sc; bnez x2, loop`, the exact
shape the ISA permits to fail but requires to succeed eventually. The harness
plays the second agent: whenever the hart holds a reservation and the memory port
is free, it performs one ordinary 8-byte write to the granule in the shared
memory model **and** raises `ext_write_valid_i` — one event, delivered both
places. The loop must then succeed.

### The three views, and the checks

73 checks, all of them statements about the architecture rather than about the
DUT's internals:

* **the retirement stream** — each LR retires with `rd` = the value the memory
  held at that PC; each SC retires with `rd` = the status the ISA defines for that
  block. `x2` is written only by an SC in both programs, so the retirements of
  `x2` are counted and their values required to be 0 or 1;
* **the data-port history**, replayed against a shadow memory: every LR read
  returns what the history holds, every SC beat is a write, every ordinary store
  lands, and the AMO between block 6's LR and SC appears as **one atomic pair**
  whose write finds the location unchanged since its read;
* **the number of writes** — `sc write beats == the number of SCs the
  architecture says succeeded`, and `lr read beats == the number of LRs`. This is
  the check the card's Pass criterion turns on, and it is a cross-check between
  two independent accounts (the architectural status and the memory traffic);
* **the reservation state** — whenever the DUT reports a reservation standing,
  the granule it reports must be `address & ~63` (42 samples in run A, 0
  violations). The granule rule is checked as *state*, not only through its
  consequences.

Measured, shipping build (`results/unit/lrsc.reservation_progress/run.log`):

```
lrsc run A: 39 retires, 13 txns, 186 cycles, lr=5 sc_ok=2 sc_fail=5 sc_beats=2
            lr_beats=5 granule_samples=42
lrsc run B: 51 retires, 10 txns, 183 cycles, injected=3 attempts=4 lr=4 sc_ok=1
            sc_fail=3 sc_beats=1
RESULT PASS lrsc.reservation_progress checks=73 seed=1
```

Run A's 13 transactions are exactly: 3 ordinary stores (the seed, the in-granule
store, the out-of-granule store) + 1 exit store + 5 LR reads + 2 SC writes + 2
AMO beats. The two SC write beats are the two successful SCs; the five failures
produced no beat at all.

### The exit protocol

The program stores its exit code to TOHOST and then **halts with WFI**, and the
harness waits for the halt and for the store queue to drain. WFI rather than a
self-jump is deliberate and was found the hard way: a self-jump is a *redirect*,
the redirect is issued when the jump executes — which, out of order, can be before
the store that precedes it has retired — and a redirect flushes the whole store
queue, so the exit store was squashed before it drained. WFI is a system macro the
core stages and takes only at the ROB head, so the exit store has retired and
drained before the machine halts. (This is a property of the *exit pattern*, not
of the LR/SC path; it is recorded here because the first version of the case was
wrong for exactly this reason.)

---

## 3. The forward-progress condition

### The declaration

> **A constrained LR/SC loop — `lr`, then only instructions that are neither
> memory accesses nor control transfers, then `sc`, then a backward branch that
> repeats the loop only if the SC failed — succeeds within `N` attempts, provided
> that no other agent writes the reservation granule and no exception intervenes
> between the LR and the SC.**

`N = 16` is the declared bound, written down in `tb_core_lrsc.cpp` as
`kProgressBound` and **also compiled into the program**: the loop counts its
attempts and takes a *different* exit code (2, "attempts exceeded the bound") if
it ever needs more than `N`. An infinite retry loop therefore cannot pass — it
either halts on the bound with the failure code or trips the harness's stall
guard.

### The measurement

The bound is checked **from the run's own numbers**, not assumed:

* the program writes its attempt count to memory, and the harness requires
  `attempts == interfering writes + 1` — the loop needed exactly one attempt per
  write the second agent performed, plus the one that succeeded;
* `attempts <= N`;
* the endpoint's counters must agree: `sc_ok == 1`, `sc_fail == interfering
  writes`, `lr == interfering writes + 1`, `res_ext_inval == interfering writes`;
* **the stimulus is checked too**: if the harness itself injected `N` or more
  conflicting writes, the assumption the bound is stated under would be violated
  and the case fails loudly by name *before* anything else is measured.

Measured (run B): 3 interfering writes, 4 attempts, 1 success, 3 failures,
3 external invalidations. The observed bound on this implementation is therefore
one attempt per interfering write plus one — it never fails spuriously — and the
case holds it to `N = 16`.

---

## 4. Controls

Built by hand from a **deleted** `obj_dir`, `-D` inserted into the same Verilator
command line the shipping case uses, into a separate build directory so the
shipping binary is untouched. Reproducible with
`python3 tools/run_lrsc_controls.py`. Shipping binary
`sha256[:16] = a9da498369325d51`.

| `-D` control | what it breaks | first failure | exit | sha256[:16] |
|---|---|---|---|---|
| `MOSAIC_LRSC_MUTANT_SC_WRITES_ON_FAIL` | `rtl/core/mosaic_lsu_endpoint.sv`: an SC whose reservation is gone performs its write anyway (and reports success) | `the instruction at 0x000000008000001c retired rd=2 we=1 value=0x0000000000000000, the ISA requires rd=2 value=0x0000000000000001` | 1 | `62ecad756ccce237` |
| `MOSAIC_LRSC_MUTANT_SC_DOUBLE_WRITE` | `rtl/core/mosaic_lsu_endpoint.sv`: a successful SC is sent round for a second write beat | `exactly one write beat per successful SC -- the ISA says 2 succeed, the port saw 4 SC write beats` | 1 | `227e36b3836d3eaf` |
| `MOSAIC_LRSC_MUTANT_NO_EXT_INVAL` | `rtl/core/mosaic_reservation.sv`: another agent's write never clears the reservation | `3 external writes must clear the reservation, the endpoint counted 0` | 1 | `d124787a79b65ca2` |
| `MOSAIC_LRSC_MUTANT_GRANULE_OVERINVALIDATE` | `rtl/core/mosaic_reservation.sv`: any write clears the reservation, whatever its address | `the instruction at 0x0000000080000058 retired rd=2 we=1 value=0x0000000000000001, the ISA requires rd=2 value=0x0000000000000000` | 1 | `b3539851acf87d34` |

Each hash differs from the shipping hash and the failure is named before the run
reports PASS, so a silent pass is impossible. Two notes on the failures:

* the double-write control is caught by the **port's write count**, not by the
  final memory image — the second write stores the same data, so the end state is
  identical. That is exactly why the case counts beats per SC.
* the over-invalidation control is caught by the **retirement stream**: block 5's
  SC reports failure where the ISA requires success, because the out-of-granule
  store at A+64 wrongly cleared the reservation. The memory image alone would
  also differ, but the architectural status is the more precise statement.

---

## 5. Gates

* `python3 tools/lint_rtl.py --profile p0` — **40 of 40 source files clean**.
* `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)`
  — exits 0. The only warnings are the pre-existing `STYLE-2` port-suffix ones
  (in `mosaic_steering.sv`, and on `mosaic_lsu_endpoint.sv`'s long-standing `o_*`
  observability ports). The new `mosaic_reservation.sv` has none.
* `python3 tools/check_records.py` — green (46 delivered packages, 56 registered
  cases).
* RTL style: no assignment patterns, no interfaces, explicit widths, `logic`.

Cases re-run after the change, all **PASS**:

| case | task |
|---|---|
| `lrsc.reservation_progress` | I-040 (this package) |
| `lsu.byte_forwarding` | I-035 |
| `lsu.size_fault_boundaries` | I-033 |
| `store.wrong_path_visibility` | I-034 |
| `amo.linearization` | I-039 |
| `core.mem_program` | I-023 |
| `core.corpus_sweep` | I-023 |
| `mmio.exactly_once` | I-038 |
| `fence.code_and_data_order` | I-037 |

and, because the change touches interfaces several other cases compile against:
`decode.rv64im_reserved` (I-010, PASS), `iq.wakeup_insert_select` (I-022, PASS),
`fabric.fixed_two_cluster` (I-023, PASS), `compressed.cross_boundary` (I-041,
PASS), `core.trap_csr_program` (I-023, PASS), `trap.precise_state` (V-014, PASS),
`retire.width_and_order` (V-013, PASS), `core.bringup_vs_reference` (I-008, PASS).

### The changes to shared interfaces

* `rtl/core/mosaic_pkg.sv` — `mem_kind_e` gains `MEM_LR`/`MEM_SC`; `decode_ctl_t`
  gains `is_lr`/`is_sc`. The decoder's `CTL_ILLEGAL` covers them with its
  `default: '0`, so `mosaic_decoder.sv` is unchanged.
* `rtl/core/mosaic_uop_pkg.sv` — `uop_meta_t` and `lsu_req_t` gain `is_lr`/`is_sc`.
* `rtl/core/mosaic_dispatch.sv` — the two flags ride the memory insert bus; LR/SC
  are dispatched as `UOP_LOAD` class for the same reason an AMO is.
* `rtl/core/mosaic_amo_unit.sv` — the one atomic record now also holds LR/SC and
  publishes its identity for the load queue's atomic-head gate.
* `rtl/core/mosaic_load_queue.sv` — an atomic head forwards no byte and is never
  replayed (see §1).
* `rtl/core/mosaic_store_queue.sv` — drives the two new request fields low.
* **`rtl/core/mosaic_core.sv`** — the minimal change the card asks to be named:
  the front-end decode arm for `lr`/`sc`; the `disp_mem_is_lr`/`disp_mem_is_sc`
  wiring through dispatch and the atomic record; the class merge into `ep_req`;
  LR/SC added to the serializer's non-speculative set and to the atomic-taken
  strobe; the atomic-allocation gate; the load queue's atomic probe; three new
  inputs (`ext_write_valid`, `ext_write_addr`, `ext_write_bytes`) and seven new
  outputs (`o_mem_res_valid`, `o_mem_res_granule`, `o_mem_lr_ctr`,
  `o_mem_sc_ok_ctr`, `o_mem_sc_fail_ctr`, `o_mem_res_ext_inval_ctr`,
  `o_mem_dmem_kind`) passed straight to/from the endpoint. The four cases the card
  names for a `mosaic_core.sv` change — `core.mem_program`, `core.corpus_sweep`,
  `amo.linearization`, `mmio.exactly_once` — were re-run and all PASS.
* Testbenches whose port lists had to follow: `sim/tb/mosaic_core_tb.sv`,
  `sim/tb/mosaic_lsu_endpoint_tb.sv`, `sim/tb/mosaic_load_queue_tb.sv`,
  `sim/tb/mosaic_store_queue_tb.sv`, `sim/tb/mosaic_iq_tb.sv`, and
  `sim/tb/mosaic_decoder_tb.sv` + `sim/unit/tb_decoder.cpp` (the decoder case
  flattens the whole `decode_ctl_t`, whose width moved from 141 to 143 bits).

---

## 6. Not covered (honest list)

* **The second agent is not a second hart.** p0 has one hart and one memory port.
  What stands in for the other agent is the harness writing into the shared
  memory model *and* raising `ext_write_valid_i`; the memory model is the memory
  both use, so the write and the notification are one event by construction. This
  is enough to show that a conflicting write invalidates the reservation, but it
  is not a second hart, there is no arbitration, and — the sharper limitation —
  the case cannot distinguish a design that invalidates on the *notification*
  from one that invalidates on the *write*, because here they are the same event.
  A real multi-master test needs the coherent fabric (I-042/I-043, V-086).
* **The granule rule without a cache.** The reservation granule is observable
  directly (the DUT exports it and the case checks it against `address & ~63` in
  every cycle a reservation stands), and its *boundary* is observable through the
  SC's status — an in-granule store breaks the reservation and an out-of-granule
  store does not. What is **not** observable is why 64 bytes is the right size:
  with no cache and no coherence, any granule that contains the LR's address would
  behave identically for every access this machine can make, so the case tests the
  *declared* rule, not its necessity. A cache (I-042) is what would make a
  wrong-sized granule visible.
* **`aq`/`rl` ordering effects.** The path is conservatively ordered — the LR/SC
  is presented only at the ROB head, the store drain has priority at the port mux,
  and the load queue issues in order — so `aq`/`rl` demand nothing extra and a
  plain LR/SC and an `aq`/`rl` one produce the same order here. The case carries
  the bits (the AMO case's `MOSAIC_AMO_MUTANT_IGNORE_AQR` covers that mechanism)
  but does not demonstrate a relaxation they would forbid.
* **Speculative LR/SC.** Both are serialized to the ROB head, so an LR never
  establishes a reservation on a path that can be squashed and an SC never writes
  speculatively. The case therefore does not exercise "an LR squashed by a
  mispredict leaves no reservation"; the design makes it unreachable rather than
  tested.
* **The misaligned LR/SC trap.** The profile traps it and the endpoint decides it
  from the address alone, with LR taking cause 4 and SC cause 6, but the case's
  programs are aligned. That boundary is I-033's case.
* **LR/SC to a device region.** The serializer's atomic predicate does not include
  the PMA device classification, so an atomic to MMIO would be serialized but the
  profile claims no atomic MMIO. No case drives it.
* **The progress condition under real contention.** The measured bound is one
  attempt per interfering write because this implementation never fails
  spuriously. The case would catch a regression that introduced spurious failures
  (the attempt count would exceed `injections + 1`), but it cannot exhibit a
  *legitimate* long retry, because nothing in this machine can produce one.
* **A corpus program with LR/SC.** No assembled A-extension program exists in
  `tests/programs/**`; the case assembles its own images.
* **The `lrsc.reservation_progress` registry entry.** It still carries
  `"pending": true`, which the integration lead removes when the package is
  recorded. The new modules `mosaic_reservation.sv` and the driver
  `sim/unit/tb_core_lrsc.cpp` are resolved by Verilator from the entry's source
  list and `-I rtl/core`, so no registry edit was needed.
