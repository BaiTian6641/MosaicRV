# I-023 — the memory path in the integrated core

CASE=core.mem_program (top `mosaic_core_tb`, driver `sim/unit/tb_core_mem.cpp`).

**Verdict: the package is delivered.** From a deleted build directory the case
builds and passes, and four injected defects — one per Fail mode the card names —
each mutate the binary, exit 1 and name the check they break.

```
PASS core.mem_program  checks=125 comparisons=9543 cycles=1017 retires=306
                       loads=63 stores=84 data_txns=147 fwd_bytes=96 mem_bytes=306
                       programs=2 inputs=6 seed=1
```

Shipping binary `sha256 9e4d8e6dd515ebc0335667c43454c5e512218a02b2ce4365c7878e478e60a7c0`
(`build/p0/unit/core.mem_program/core.mem_program`), from the recorded command in
`build/p0/unit/core.mem_program/build_command.txt`.

---

## 1. What was wired

```
dispatch ── alloc ──> mosaic_load_queue  ──┐
   │ (operands captured)                    ├──> mosaic_lsu_endpoint ──> dmem
   └─ alloc ──> mosaic_store_queue ─────────┘
                      ▲          ▲
   ROB retire ── commit(1,2) ─────┘   (authorisation)
   redirect ──── squash_all ──────┴─── (both queues)
```

* **`mosaic_dispatch.sv`** no longer refuses `UOP_LOAD`/`UOP_STORE` (only
  UOP_SYSTEM and FENCE/FENCE.I remain refused). A memory macro leaves through a
  new insert port instead of through a cluster: its base is slot 1's value, a
  store's payload is slot 2's, the offset is the decoded immediate, and the
  address is carried as `base + imm` because the LSU owns the one adder.
* **`mosaic_core.sv`** instantiates the load queue, the store queue and the LSU
  endpoint; arbitrates the endpoint's single upstream port (store drain first,
  then loads); routes the endpoint's response back to the queue that owns it; adds
  the memory path as a fourth completion producer of `mosaic_wb_arbiter`; and
  drives the store queue's authorisation from the two ROB retire lanes, the
  squash from `redirect_valid`, and the load queue's flush from the same pulse.
* **A store is execution-complete at allocation.** Its operands are captured when
  it is inserted, so the ROB's completion for it is offered in that same cycle (a
  store has no register value and nothing else to wait for). What remains is the
  *authorisation*, which the retire path supplies.
* **A load's completion** carries the value the load queue merged and extended,
  plus the destination the entry carried with it (`alloc_dst_*` /
  `result_dst_*` on the load queue — the identity had to live with the load, or a
  second table keyed on the ROB index could disagree with it after a recycle).
* The **data port** is now real: the endpoint drives `dmem_req_*`, and the
  harness's memory system answers it (byte-strobed, lane-aligned, fixed latency).

### Module changes outside the integration lane (owner gaps, reported)

| module | change | the case that would have caught it if it were wrong |
|---|---|---|
| `mosaic_store_queue.sv` | a **second commit port** (`commit2_*`), and its `o_commit_ctr` now increments by the *sum* of both ports | CASE=core.corpus_branch caught the counter bug (two non-blocking assignments to one signal made the sum lose the second port, and 14 stores reported 12 authorisations); CASE=core.mem_program exercises the port itself — 4 (p03) and 8 (p09) two-store retire cycles per run |
| `mosaic_load_queue.sv` | `alloc_dst_*` / `result_dst_*` carry-through, and a `flush_valid_i` port | CASE=core.mem_program's per-instruction value comparison (a wrong destination writes the value into the wrong physical register) |
| `mosaic_wb_arbiter.sv` | a **fourth producer port** (`wb_ev3`/`wb_valid3`/`wb_ready3`) | CASE=open (`wb.same_bank_many_producers`) still passes with the port held idle; CASE=core.mem_program's load results and store completions are the exercise |
| `mosaic_macro_desc` wiring | `wr_is_store` / `rd_is_store0/1` now carry "this macro is a store" | CASE=core.mem_program: without it no store is ever authorised, and nothing drains |
| `sim/common/elf_loader.{h,cpp}` | best-effort `.symtab` parsing (`Image::symbols`, `FindSymbol`) | CASE=loader.elf_boundaries still passes; the parse never fails a load |
| `sim/tb/mosaic_core_tb.sv` | 25 new `o_mem_*` observation outputs (additive) | — |
| `sim/unit/mem_ref.h` (new) | the DUT's data memory and the reference interpreter, shared by the two core cases | — |

### Why the second commit port

The ROB retires two instructions per cycle and stores are the instructions most
likely to be back to back. With one authorisation port a store can retire
**unauthorised**, which is the one state the store queue's squash rule cannot
handle: too old for the dead window to take back (it has retired), and
unauthorised, so `SQUASH_SPARES_AUTHORISED` does not spare it — a whole-queue
flush would silently delete a store the architecture already promised. Both lanes
authorising in their own retire cycle makes "retired implies authorised" exact.
p03's nine consecutive stores reach two-retire cycles on every run.

---

## 2. What the program does

Two real corpus programs run unmodified from their own `main`, each in its three
declared input variants; the only harness-written instruction is `jal x0, main`
at the reset vector (crt0's first instructions are the two `csrw`s this machine
refuses). Nothing else is skipped: `main` establishes its own bases and loads its
own inputs from its own `.rodata`.

| program | instructions | what it covers |
|---|---|---|
| `p03_loadstore` | 57 retired, 11 loads, 14 stores | `sd sw sh sb` then `ld lw lhu lh lb lbu`; stores feeding much later loads at the same address; one dead store (offset 24) that must not affect anything |
| `p09_storeload` | 45 retired, 10 loads, 14 stores | store-to-load at the **same address, back to back**, three times; `sw`/`lw`; a byte store into the low byte of a double then a load of the merged double; a half store likewise; a load immediately followed by a store that overwrites it |

Measured at the data port (one transaction per load and per store), the run set
covers **all eight access classes**: `ldouble lword lhalf lbyte` and
`sdouble sword shalf sbyte`. Every access is naturally aligned and inside RAM, so
there are no traps — and the case asserts that it saw none
(`lsu_misaligned == 0`, `lsu_access_fault == 0`, `lq_fault == 0`, `sq_fault == 0`).

### Where the expectation comes from

* **The four signature words** are what `python3 tools/host_oracle.py --program
  p03_loadstore` / `--program p09_storeload` prints; the values are transcribed in
  the driver with those commands. They are compared against the memory image the
  program's own `sd`s left behind — i.e. **through the store path** — and the
  independent interpreter's memory is compared against the same oracle, so an
  agreement between machine and interpreter cannot be vacuous.
* **Every retired instruction** (pc, destination, register-write, value) is
  compared with that independent RV64IM interpreter, which has its own memory
  model and decodes the same words (`sim/unit/mem_ref.h`). A load that returns the
  wrong bytes is named at the instruction that loaded them.
* **The exit protocol** is the frozen rule: the program writes `MOSAIC_TOHOST`,
  and a pass writes exactly `MOSAIC_PASS_CODE` (1).
* **The access count** is the reference's own: the data port must show exactly one
  transaction per load and per store — no more (a wrong-path access reached
  memory) and no fewer (an access was dropped).

---

## 3. Mutants

Built by `python3 tools/run_core_controls.py --case core.mem_program`, each from a
**deleted** build directory, with the command recorded in that build's
`build_command.txt` (the `-D` is in it). `cmp` against the shipping binary is what
"differs" below; the sha256 is the mutant's own.

| define | injects | differs | exit | first failure |
|---|---|---|---|---|
| `MOSAIC_CORE_MUTANT_STORE_PRECOMMIT` | a store is authorised when it is *allocated* (the store queue's watermark is advanced every cycle a resident store is unauthorised) instead of when it retires | `d0444be92c7179f1da22d70c57e4c832aaee3cf9dd76c230e94c70eb0890785b` | 1 | `no store reaches memory before its instruction retires: writes seen <= stores retired: store transactions=1, retired stores=0` (cycle 50, p03) |
| `MOSAIC_LQ_MUTANT_YOUNGER_FORWARDS` | the load queue forwards from the youngest covering store regardless of whether it is **older** than the load | `6ec73a99202d8570b7b7435f83c219461537a9e1a78436792eb4760d21dc25c9` | 1 | `the retirement stream follows the reference: retire 14 at 0x8000001234 value for x5 expected 0x0123456789abcdef, got 0xfedcba9876543210 (a load)` (p09) |
| `MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH` | dispatch lets a load or store through the branch barrier, so a wrong-path access is allocated and issued before the redirect discards it | `b209218c1ae9f1e5b6c8aebaee3a1c127ebaee6e1d72231c464963a51b65d185` | 1 | `exactly one data transaction per load and per store reached the data port: data transactions=26 (lsu=26), loads=11 stores=14` (p03) |
| `MOSAIC_CORE_MUTANT_STORE_SIZE_WORD` | the store queue is told every store is a word, so the strobes and the bytes written disagree with the instruction for byte/half/double stores | `0ef4ceb10f660b987bc09625cab0bfd32bdb7e0cf7d17ef6c0ae9aa55814f20f` | 1 | `the retirement stream follows the reference: retire 30 at 0x8000001274 value for x5 expected 0x0123456789abcdef, got 0x0000000089abcdef (a load)` (p03) |

Notes on two of them, because the interesting part is *how* they are caught:

* **Premature visibility is invisible in the final image.** The mutant produces
  the same signature and the same memory as the shipping build: the store it
  sends early is a store that would have drained anyway. An end-state comparison
  would call it correct. It is caught only because the case checks the *order*
  invariant every cycle — no store transaction before the instruction that owns
  it has retired.
* **The squashed access** is injected by placing `ld x0, 0(x0)` at the reset
  vector's **fall-through**, which is wrong path for the entry branch. In the
  shipping build the barrier holds dispatch until the branch resolves and the
  redirect purges the front end, so the access never happens and the transaction
  count is exact. With the barrier bypassed it is allocated into the load queue,
  issued to the endpoint, and counted as a 26th transaction against 25 expected.
  A *store* on the same fall-through is a weaker control — the watermark stops it
  from draining, so only its allocation shows; that is why the injection is a
  load.

### A "passing mutant" caught before the compiler

While building the controls, `MOSAIC_CORE_MUTANT_STORE_PRECOMMIT` was first
written as `sq_commit_valid = sq_alloc_valid` with the allocating store's id. It
compiled, exited 1 — and was **not** evidence: the id named an entry that was not
resident yet (`alloc` takes effect at the edge), so every such commit was refused
as stale and the machine simply deadlocked with `sq_auth=0`. The control was
rewritten to name the entry the store queue actually holds at its watermark
(`sq_entry_pay[auth_cnt]`), which is what makes the premature drain real. The
tool's `MISS` verdict is what surfaced it; a mutant table that only counted exit
codes would have called the first version a success.

---

## 4. Not covered (honest list)

* **Store-to-load forwarding is exercised through the core — but only by p09.**
  p09 forwards 32 bytes per run (two double loads served from the resident
  store); p03 forwards **0** bytes: its stores are far enough ahead of its loads
  that they have already drained, so those loads are served by memory. Both are
  correct; the case states which happened. A program that forced p03's loads to
  forward would need the store to be resident later, which the corpus program
  does not arrange.
* **The load queue's blocking on an unknown store address is never exercised
  here.** Every store's operands are read for real before it is allocated, so
  `alloc_addr_valid`/`alloc_data_valid` are always 1 and the store queue's
  `fwd_blocked` is never high (`o_mem_lq_blocked == 0`, `o_mem_lq_replay == 0`
  asserted). The module's own case covers the late-fill and blocked paths; the
  core never enters them, because this integration chose to hold the macro in
  dispatch instead of allocating it early. That is a throughput cost, not a
  correctness one, and it is the reason the fill port is tied off.
* **No wrong-path load or store reaches memory, and the case shows that it
  cannot**: the conservative recovery makes a branch a barrier, so nothing
  younger than an unresolved branch is ever dispatched. The control above proves
  the *check* works by making the barrier leak, not that the shipping machine
  leaks.
* **`mosaic_recovery`'s narrower squash window is not used.** The core drives the
  store queue with `squash_all_i = 1`; the region inputs are wired with the
  redirecting branch's own identity so the narrower rule is a one-line change the
  day the barrier is lifted. Whole-queue is correct here *because* both retire
  lanes authorise in their own cycle — an authorised store that has not drained
  is spared by the watermark, and there is no retired-but-unauthorised store for
  the flush to lose. The run shows both halves: `sq_squash == 0` (nothing
  unauthorised was taken) and 2–4 authorised stores **spared** per run (they were
  resident across a redirect and survived it).
* **No faults, no misalignment, no unaligned or overlapping-partial accesses, no
  FENCE ordering.** The two programs are naturally aligned and inside RAM; p13
  (`romstore`) is deliberately not run because its whole content is a store to
  the read-only boot ROM that must trap and be logged by the trap handler, and
  this machine has no trap path yet. Unaligned behaviour is the endpoint's own
  case (`lsu.size_fault_boundaries`), not this one.
* **`ev_store_addr`/`ev_store_data`/`ev_store` remain unwired** (they were before
  this package). A store's payload lives in the store queue, and getting it to the
  retire event needs a port the store queue does not have; the case classifies
  retired stores by decoding the image at the retired PC instead, and compares the
  memory image byte for byte against the reference.
* **What still stands between this machine and the whole corpus**: the CSR/trap
  path (p13, p08, and every program's `_start`), so `crt0`'s boot sequence and
  the trap log cannot run; FENCE/FENCE.I (I-037); and the non-blocking/late-alias
  LSU work (I-036, I-043) that the endpoint's one-outstanding-transaction model
  and the load queue's in-order issue deliberately exclude.

## 5. Regressions

All four cases that use this top, plus the three LSU module cases and the
writeback arbiter case, were rebuilt and re-run after the last edit:

```
PASS fabric.fixed_two_cluster   PASS core.corpus_branch   PASS core.unwritten_reg_read
PASS core.mem_program           PASS store.wrong_path_visibility
PASS lsu.byte_forwarding        PASS lsu.size_fault_boundaries
PASS wb.same_bank_many_producers
```

`CASE=core.corpus_branch` needed a real update, and it is stronger for it: its
reference now models loads and stores and its run ends at the program's **exit
protocol** instead of at the first refused macro (which used to be a load). It now
checks that the program wrote `TOHOST = PASS` and that the four signature words in
memory match the host oracle — through the store path — where it previously
compared four registers held at the point the machine stopped. It also found the
dispatch bug described below.

## 6. A defect this package found

A memory macro **captures** its operands at insert; there is no issue queue behind
it to deliver a later wakeup. `s1_value_ok` is not "this operand is ready" — it is
satisfied by `!rq_written`, precisely because the cluster path inserts a
not-ready uop and lets the issue queue fill it in. Driving the memory insert from
`s1_value_ok` alone therefore inserted a store whose base register had been
written the cycle before with the register file's **stale** content: the corpus
program's exit store went to address 0 instead of `MOSAIC_TOHOST`, silently. The
insert now requires `rq_written && value_ok`, and CASE=core.corpus_branch's
exit-protocol check is what named it.
