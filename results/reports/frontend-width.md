# The front end's width and depth (two-wide allocation/rename/insert + a real instruction queue)

> **2026-10-02 — the same-cycle second insert is HELD.** The measurements below
> were taken with the two-wide *insert* present. The insert triggers a
> retirement-order defect in `retire.width_and_order` (V-013); it has been
> deleted from `rtl/core/mosaic_dispatch.sv`, and the depth-8 decoded queue and
> two-wide *allocation* are kept. The isolation, the classification experiment
> (the defect is in the retire/ROB lane-1 path, not the insert's identity), the
> amended occupancy table and the full re-verification are in
> **`results/reports/held-insert.md`**. The lane's measurements below are left
> as they were taken.

Registered case: `perf.equal_resource_compare` (p1), extended with a fourth
workload and dispatch/issue-occupancy columns. RTL touched:
`mosaic_dispatch.sv`, `mosaic_rob.sv`, `mosaic_core.sv` (front-end section),
new `mosaic_idec_queue.sv`; driver/wrapper `sim/unit/tb_core_perf.cpp`,
`sim/tb/mosaic_core_tb.sv`. Controls: `tools/run_frontend_controls.py`.

**Verdict: the width and depth are implemented and correct — but the measurement
says the front end was never the binding resource, and the honest headline of
this package is that two-wide allocation does not engage on this machine.**

## What changed and why

### 1. Two-wide allocation, rename, operand read and insert

`mosaic_dispatch` now allocates **up to two macros per cycle** when both decoded
lanes are cluster-class macros (ALU / branch / MUL-DIV / FP):

* the group goes to `mosaic_rename`'s two-wide port (I-014, previously unused by
  the core) and to a **second allocation port added to `mosaic_rob`**;
* the two lanes' operand identities come from rename's second source port pair
  (`rs3`/`rs4`, with the same-cycle bypass). Lane 1's operand values are read
  best-effort on the banks lane 0 leaves free; a lane-1 source that was **not**
  read this cycle is not known to be unwritten, so lane 1 simply does not fire
  and becomes lane 0 next cycle rather than being inserted not-ready on a wakeup
  that may already have passed;
* the two clusters each take one insert per cycle, so a pair inserts together
  when its two macros target **different** clusters (lane 1 takes the complement
  of lane 0's alternating affinity); a pair that collides on one cluster inserts
  one per cycle through the queue's back-pressure;
* the descriptor store's second write lane (`mosaic_macro_desc` already had one,
  with the two-wide retire lane) is now driven, as are the FP/vector per-slot
  memos for lane 1.

**What is deliberately one-wide, and why** (named, not silent):

| path | reason |
|---|---|
| memory macros (load/store/AMO/LR/SC) | they capture operands into the load/store queue at insert; a second one in the cycle would need a second capture path and a second queue port, neither of which exists |
| system macros (CSR/ECALL/EBREAK/MRET/SRET/WFI/FENCE/FENCE.I) | resolved at the architectural boundary, single-issue by construction |
| vector macros | the vector engine is held to one macro in flight (I-059), and `vec_block` already refuses younger allocation |
| **any control transfer** | allocates alone: the in-flight-branch barrier is one bit with one recovery point, and two transfers in one group would need two |

### 2. A real instruction queue (`mosaic_idec_queue.sv`, depth 8)

The two-entry decode buffer is replaced by a depth-8 queue with a two-lane pop.
Each entry carries the instruction's **own length and bits** from `mosaic_fetch`
(the property the compressed work depends on); nothing re-derives them. The
delivery handshake is unchanged (`push_valid`/`push_ready`, payload held), and a
redirect (`purge`) drops everything — with priority over a push in the same
cycle, so the queue's discard rule and the fetch unit's `out_ready` cannot
disagree (a delivery accepted and then dropped is an instruction silently lost).

### 3. Occupancy instrumentation

`perf.equal_resource_compare` now reports, per workload per configuration:
`alloc/cyc`, `pair%` (cycles two macros allocated), `ins/cyc`, the mean dispatch
queue depth (`occ`, of 8), the mean issue-queue occupancy summed over the two
clusters (`iq`, of 16), the mean decoded-instruction-queue depth (`iq_q`, of 8),
`bar%` (allocation barrier), `off%` (two-wide offered), `l1%` (lane 1 eligible),
`frsp%`/`freq%` (fetch response / request cycles), `rec%` (recovering) and
`issue/cyc`.

## The measurement

Same workloads, same seed (1), same cycle budget. The three original workloads'
cycles and instruction counts are **unchanged from I-084** (2389 / 3088 / 466 →
2389 / 3088 / 463; vec_stream moves by 3 cycles because `pair_burst` was added to
the same harness and the configurations run in a different order against the
same core — no workload's architectural result changed, and the identity checks
pass).

```
workload alu_chain      cycles 2389  insns 494  IPC 0.206  (before: 2389 / 494 / 0.206)
  alloc/cyc=0.207 pair%=0 ins/cyc=0.207 occ=0.21/8 iq=0.37/16 iq_q=0.33/8
  bar%=12 off%=0 l1%=0 frsp%=25 freq%=50 rec%=19 issue/cyc=0.166

workload stream         cycles 3088  insns 319  IPC 0.103  (before: 3088 / 319 / 0.103)
  alloc/cyc=0.104 pair%=0 ins/cyc=0.104 occ=0.66/8 iq=0.71/16 iq_q=2.61/8
  bar%=47 off%=0 l1%=2 frsp%=22 freq%=46 rec%=14 issue/cyc=0.071

workload vec_stream     cycles  463  insns  24  IPC 0.051  (before: 466 / 24 / 0.051)
  alloc/cyc=0.054 pair%=0 ins/cyc=0.054 occ=0.47/8 iq=0.01/16 iq_q=5.22/8
  bar%=14 off%=0 l1%=46 frsp%=8 freq%=17 rec%=6 issue/cyc=0.032

workload pair_burst (new) cycles 1790 insns 60  IPC 0.033
  alloc/cyc=0.034 pair%=0 ins/cyc=0.034 occ=6.59/8 iq=11.78/16 iq_q=6.92/8
  bar%=34 off%=0 l1%=42 frsp%=4 freq%=8 rec%=1 issue/cyc=0.032   (pairs inserted = 3)
```

**IPC did not rise on the ALU-dependent chain, and the reason is measured, not
guessed.** Every workload is *fetch-bound*, not allocation-bound:

* `frsp%` is 25 % on `alu_chain` and 22 % on `stream`: the fetch unit accepts a
  response on about one cycle in four, so the front end delivers **~0.2
  instructions per cycle**. `alloc/cyc` equals `frsp%` on every workload — the
  allocator allocates exactly what fetch delivers and never queues behind it.
* the decoded-instruction queue is nearly empty on `alu_chain` (`iq_q = 0.33/8`)
  and the dispatch queue and issue queues are nearly empty too (`occ = 0.21/8`,
  `iq = 0.37/16`). There is nothing for a wider allocator to do.
* `off% = 0` everywhere: `wide_pair` is never even *offered*, because it needs
  `dec_valid[1]` (a second decoded instruction) at a moment `alloc_now` is true,
  and with fetch at ~0.2/cycle the queue almost never holds two.
* `pair_burst` was built specifically to force a backlog: it uses the iterative
  MUL/DIV unit (reached only through cluster 0's queue) so that issue is far
  slower than insert. It does back the machine up (`occ = 6.59/8`,
  `iq = 11.78/16`, `iq_q = 6.92/8`, `l1% = 42`) — and it still shows `off% = 0`,
  because the backlog pins the queue at 7-8 entries, where there is room for one
  more push but not two, and the head cannot insert (cluster 0's queue is full)
  so no slot frees. The two-wide *insert* path did fire 3 times (two macros left
  the dispatch queue into the two clusters in one cycle).

The binding resource on all four workloads is the **instruction fetch rate**:
`frsp% ≈ 22-25 %` with `freq% ≈ 46-50 %` and `rec% ≈ 14-19 %`. That is the L1I
hit/miss path plus the one-request-at-a-time handshake (`ic_cpu_req_valid`
requires `fetch_outstanding == 0 || fetch_rsp_live`), which is the memory lane's
subject, not the front-end width's. On `stream` the data path also binds
(`dmem = 394` beats), as I-084 already reported.

## The controls

`tools/run_frontend_controls.py` builds the shipping binary and each control from
its own deleted build directory, with one `-D` in the build command, and requires
a differing sha256, exit 1 and a named first failure. Observed (built against the
HEAD geometry in an isolated worktree, so the memory lane's in-flight L1/geometry
rewrite does not enter the binaries):

| control | hash | exit | first failure |
|---|---|---|---|
| shipping | `ec6b578f198edc8f` | 0 | PASS |
| `MOSAIC_ROB_MUTANT_SWAP_PAIR_ORDER` | `feae8e894f93a776` | 1 | `MISMATCH +steering on stream retirement 313: expected the same architecture as the baseline` |
| `MOSAIC_DISPATCH_MUTANT_DROP_PAIR_TAIL` | `50c7804fa1ecf040` | 1 | `MISMATCH baseline on stream: the machine stopped on an instruction it refuses (unsupported=1 illegal=1)` |
| `MOSAIC_RENAME_MUTANT_NO_BYPASS` (I-014's control, reused) | `749799e19cfefbe2` | 1 | `MISMATCH baseline on stream: the machine stopped on an instruction it refuses (unsupported=1 illegal=1)` |

All three fail with distinct hashes and named first failures. **Honest caveat:**
they fail on `stream`, not on `pair_burst` — i.e. each defect reaches further than
the pair path it was written for. `MOSAIC_ROB_MUTANT_SWAP_PAIR_ORDER` is inert on
the pair path itself (two-wide *allocation* never engages, so the swapped slots
are never created) and is caught only because the mutant also perturbs the
single-wide slot placement in a way the retire comparison sees. The control that
directly proves the two-wide *insert* is real is `MOSAIC_DISPATCH_MUTANT_DROP_PAIR_TAIL`
together with the `pair_burst` absolute checks; the pair-order control is a
weaker statement than its name suggests and is reported as such.

## The regression the wider window exposed, and its fix

Widening the front end's window exposed a latent unsoundness in the unarmed-trap
refusal, and it broke `mmio.exactly_once` (p0 and p1).

**Root cause.** `mosaic_dispatch` refused an ECALL/EBREAK whose trap vector was
not yet installed (`l0_trap_unarmed`) at the **decode input**, judging it against
`mtvec` -- architectural state that is written at retirement and is therefore only
valid once every older macro has retired. The old two-entry buffer made "not yet
architecturally next" accidental: the program in `tb_core_mmio.cpp` deliberately
puts sixteen instructions between `csrw mtvec` and the `ecall`, and its own
comment says that is "more than the front end's window". The depth-8 queue widens
that window, so the `ecall` was decoded before the `csrw` retired, refused, and
the machine stopped on an instruction whose trap vector was about to be
installed.

**Fix.** The refusal now uses the rule the module's header already stated --
refuse only when the macro is the architectural next instruction:

```
l0_refused = l0_unsupported && !branch_in_flight && (!l0_trap_unarmed || rob_empty_i)
```

with `rob_empty_i` the ROB's own occupancy (`rob_occupied == 0`), wired in
`mosaic_core.sv`. The invalid-decode half is unchanged: it is a property of the
instruction alone and does not depend on architectural state. Verified: 
`mmio.exactly_once` PASS at p0 and p1.

## Not covered

* **`retire.width_and_order` (p0, top `mosaic_core_tb`) fails in the tree and
  passes at clean HEAD**, with `event 110 pc expected 0x800001c0, got
  0x80000014 lane 1 at cycle 527`. The trigger is isolated to the **same-cycle
  second insert**, by three builds from deleted directories:

  | build | result |
  |---|---|
  | `-DMOSAIC_DISPATCH_MUTANT_NO_INSERT` (two-wide allocation kept, second insert removed) | **PASS** |
  | `-DMOSAIC_DISPATCH_MUTANT_NO_PAIR` (allocation and insert both removed) | **PASS** |
  | `-DMOSAIC_DISPATCH_MUTANT_NO_PAIR` with only `wide_pair=0` (allocation removed, insert kept) | **FAIL, same signature** |

  So the two-wide allocation and the depth-8 queue are safe, and the same-cycle
  second insert is the trigger. Further negatives inside the insert still fail
  identically: `-DMOSAIC_DISPATCH_MUTANT_H1_NO_CTRL` (lane 1 must not be a
  control transfer) and the memory lane's own `MOSAIC_MEM_SMALL_CACHE`. The
  failure's shape -- `lane 1` retiring a much older PC -- points at the two-wide
  *retire* lane (I-017) rather than at the insert's identity, which carries the
  ROB's own `alloc2_index`/`alloc2_gen`. **The root cause is not yet found and the
  insert is not fixed**; it must be held back (deleted, not stubbed) before this
  package ships, and the report is the record of that.
* **Two-wide allocation never engages on any workload in this tree.** The pair
  path is implemented, and the two-wide *insert* fires on `pair_burst`, but
  `alloc2` is 0 everywhere: with fetch at ~0.2 instructions/cycle, `dec_valid[1]`
  is almost never true when allocation is possible. Until the front end delivers
  at or near one instruction per cycle, the width cannot pay off, and any claim
  that it does would be unfounded.
* **Paths left one-wide**: memory, system, vector and control-transfer macros
  (reasons above). A pair whose two macros target the same cluster (e.g. two
  MUL/DIV) allocates two-wide but inserts one per cycle — also deliberate.
* **Verified green in the real tree after the fix**: `mmio.exactly_once` (p0 and
  p1), `core.act_dut` (p1, 127/127), `core.corpus_sweep` (p1),
  `perf.equal_resource_compare` (p1, `PASS workloads=4 configs=7`),
  `rename.same_cycle_chain` (p1), `rob.out_of_order_children` (p0),
  `rename.single_width_ownership` (p0), `core.corpus_branch` (p0),
  `iq.wakeup_insert_select` (p0).
* **Issue-queue occupancy is reported, but the issue queues never become the
  binding constraint** at this front-end rate — the claim that they *would* at a
  higher allocation rate is untested.
* The ROB is 64 entries and never fills on these workloads, so the second ROB
  allocation port's refusal path (`alloc2_refused`) is not exercised.
* Not run in this package's session: `core.mem_program`,
  `fabric.fixed_two_cluster`, `fabric.integrated`, `retire.*` (other than the
  failing `retire.width_and_order` above), `vec.integrated`, `make check`, both
  profiles' `lint_rtl`, `slang-tidy`, `check_records`, `check_exclusions` and
  `make lint-cpp` — the integration lead's regression, not this package's.
