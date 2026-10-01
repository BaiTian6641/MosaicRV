# I-023 — the control path (CASE=`core.corpus_branch`)

Work package **I-023** of `docs/implementation-plan.md`, control-path wave. The
obligation this closes is

> 分支解析必须到达重定向仲裁器；胜者重定向前端、清除簇内更年轻的工作、并驱动
> rename/恢复的 squash。
>
> Pass: 一个真实语料程序跑通，逐指令的架构事件流与独立期望一致。
> Fail: 重定向不清除更年轻的已取指令（被 squashed 的指令 retire）、重定向 PC 差
> 一条指令、或分支解析没有被采纳（错误路径 retire）。

## STATUS: COMPLETE

`python3 tools/run_unit.py --profile p0 --case core.corpus_branch` →

```
RESULT PASS core.corpus_branch checks=28 comparisons=8907 cycles=1783
            retires=256 control=96 taken=74 branches=48(taken 26) jal=27 jalr=21
            back_edges=35 inputs=3 seed=1
```

It did not pass when it was written: the case found **four real defects** in the
integrated machine, three of them in the control path this wave is about. The
core's header said the recovery was "the barrier" and that recovery "is what the
case exercises"; nothing had ever run a program through a branch, and the
control path did not work at all. Details in *What the case found*.

| Acceptance item | Evidence |
| --- | --- |
| the branch result reaches the redirect arbiter, and the winner redirects fetch | the arbiter reads both clusters' resolution requests (`{c1,c0}_redir_*`), and the case checks **every** redirect's PC against the taken transfer's target node by node, observed off the core's redirect port (`o_redirect_valid`/`o_redirect_pc`), not inferred from the retire stream |
| the winner's redirect is applied to fetch | the cycle after each redirect the front end's PC must be that target; checked per redirect (`o_fetch_pc`), and the fetch unit's response classifier and output register are exposed (`o_dbg_fetch_state`, `o_dbg_redir_bundle`) |
| younger work is purged | the decode buffer is cleared on the redirect, the fetch unit cancels its slots and retires the instruction in its output register; `MOSAIC_FETCH_MUTANT_OUT_REG_NO_REDIRECT_FLUSH` and `MOSAIC_CORE_MUTANT_NO_PURGE` both fail the case, the first as *a squashed instruction retiring* |
| rename's squash is driven, and at a committed boundary | the core takes the checkpoint on the redirect pulse (the cycle the branch's own commit has landed) and squashes the cycle after; the case requires `ckpt == squash_accepted == taken transfers` and that `squash_not_committed` / `squash_underflow` / `journal_overflow` stay zero; `MOSAIC_CORE_MUTANT_EARLY_CKPT` (the checkpoint one cycle early) fails on exactly those counters |
| a real corpus program, with an expectation that is not the DUT | `tests/programs/src/p02_branch.S`, the corpus's branch program, loaded as an ELF; the expectation is an independent RV64IM interpreter over the same words plus the host oracle's four signature values; three declared inputs are run |
| taken and not-taken, JAL/JALR, back edges | 48 conditional branches (26 taken, 22 not taken), 27 JAL, 21 JALR, 35 back edges across the three runs |
| the machine retires the program's instructions and nothing else | every retirement is compared in the cycle it happens: pc, destination, and value (pc/rd/reg-we/value) against the reference, instruction by instruction |
| the machine stops at the first macro dispatch refuses, and quiesces | the run stops at the program's first `sd`, `o_stopped=1`, ROB empty, rename at a boundary, and the free list back at its post-reset occupancy |
| the four signature values are the host oracle's | the retired values of t0/s3/t4/t3 are compared against `tools/host_oracle.py --program p02_branch`'s sig0..sig3 for each input, and the driver's own interpreter must agree with the oracle on all four before the DUT's values count |
| `fabric.fixed_two_cluster` still passes | `RESULT PASS fabric.fixed_two_cluster checks=59 comparisons=678 cycles=129 …`; `python3 tools/run_core_controls.py` → all 3 of its mutants still caught |
| `python3 tools/lint_rtl.py --profile p0` | every file this wave touched is clean: `ok rtl/core/mosaic_core.sv`, `ok rtl/core/mosaic_dispatch.sv`, `ok rtl/core/mosaic_fetch.sv`, `ok rtl/core/mosaic_redirect_arb.sv`, `ok rtl/core/mosaic_cluster.sv`. The tool's own summary is `lint: 1 of 33 source file(s) failed` -- the one failure is `rtl/core/mosaic_load_queue.sv`, a file another lane added to the tree while this wave was in flight and which is in no case registered by this wave |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | **0 errors** |
| negative controls | `python3 tools/run_corpus_branch_controls.py` → 7 of 7 observable controls OK (binary differs from shipping, exit 1, named check), 1 documented as unobservable **in this case** |
| this report | — |

## Files

| File | Change |
| --- | --- |
| `rtl/core/mosaic_redirect_arb.sv` | **fixed**: the head gate looked only at retire lane 0 (section *the lane-1 head*); the arbiter now takes `head1_*` and acts on either retiring lane |
| `rtl/core/mosaic_fetch.sv` | **fixed**: the output register's lifecycle — a redirect now retires it, and a dropped (stale) response no longer suppresses its drain; `o_dbg_state` observation port; two further negative controls |
| `rtl/core/mosaic_dispatch.sv` | **fixed**: a bank conflict between the two sources put the *first* source's value into the second operand; the fetch is now two cycles with a held value; one negative control |
| `rtl/core/mosaic_core.sv` | the rename checkpoint/squash are now driven (section 12b); the arbiter's second retire lane is connected; observation ports `o_redirect_valid`, `o_redirect_pc`, `o_fetch_pc`, `o_squash_acc_ctr`, `o_ckpt_ctr`, `o_dbg_redir_bundle`; three negative controls |
| `rtl/core/mosaic_cluster.sv` | `MOSAIC_CLUSTER_MUTANT_IGNORE_TAKEN` negative control; no functional change |
| `sim/tb/mosaic_core_tb.sv` | the new core outputs are brought out; the standalone arbiter instance ties its second lane low (it is the single-lane directed test) |
| `sim/tb/mosaic_fetch_tb.sv` | brings `mosaic_fetch`'s new observation port out (a pin left dangling is a build failure in this tree) |
| `sim/unit/tb_fetch.cpp` | **fixed**: the fetch case's model had the *same* drain defect as the RTL -- a dropped response suppressed the output register's drain -- so it asserted the defective behaviour at cycle 166. Its model now says what the register does; `fetch.redirect_late_response` PASSes |
| `sim/unit/tb_core_corpus.cpp` | **new**: the driver |
| `tools/run_corpus_branch_controls.py` | **new**: the mutant campaign |
| `results/reports/I-023-branches.md` | this file |

One more case was affected: `fetch.redirect_late_response` (I-009) instantiates
`mosaic_fetch` directly, so the new observation port had to be brought out of
its wrapper, and its reference model -- which mirrored the output register's
defective drain -- failed once the RTL was correct. Both are fixed above; the
case passes.

The files this wave touched that are listed by another registered case:
`fabric.fixed_two_cluster` lists `mosaic_redirect_arb.sv`, `mosaic_fetch.sv`,
`mosaic_dispatch.sv`, `mosaic_cluster.sv`, `mosaic_core.sv` and
`sim/tb/mosaic_core_tb.sv`. All five RTL changes and the wrapper change are
behaviour-preserving for that case's program (it contains no branch, so no
request is ever raised, no redirect is ever issued and the fetch unit's output
register is never exposed to a redirect), and the case plus its three controls
were re-run: `fabric.fixed_two_cluster` PASS, all 3 controls OK.

## What the case drives

The DUT is the integrated p0 core. The program is the corpus's own
`p02_branch`, loaded from `tests/programs/build/p02_branch.i0.elf` through
`sim/common/elf_loader.cpp`. The arm it executes is `main`'s body from
`0x8000_122c` to `0x8000_12e8` — the six-way comparison cascade, the
data-dependent popcount loop, the JAL/JALR link chain with the JALR bit-0 rule,
and the input-dependent select — ending at the program's first `sd`, which
dispatch refuses and the machine stops at.

```
8000122c  li   t0,0             ; the six-way cascade: beq/blt/bge/bltu/bgeu
80001230  beq  a0,a1,80001238     each followed by an unconditional `j` that
80001234  j    80001240           skips the ori, so both arms are real code
   …      blt/bge/bltu/bgeu with the same shape
80001274  mv   t1,t0            ; popcount: andi/add/srli + `bnez t1,1b`
80001294  jal  ra,p02_stub      ; one JAL, whose `ret` is a JALR back
800012ac  beqz t5,2f            ; the JALR round-trip loop, t5 = c & 7
800012b0  jalr t1               ; target deliberately odd (ori 1): bit 0 must
                                ; be cleared
800012c0  …  bnez/bnez          ; input-dependent select of sig3
```

Two declarations about the harness, because the integrated core services only
ALU, branch and MUL/DIV macros (dispatch refuses loads, stores, CSRs and fences
*before* allocating, and stops the machine there):

1. The corpus program's first instruction from `_start` is a CSR write and its
   fourth in `main` is a load, so neither entry point can run. The driver finds
   the program's branch region by matching the `MOSAIC_LOAD_INPUTS` macro's
   five-word pattern in the loaded image (not by a hard-coded PC) and enters
   there.
2. The core has no load path, so the three declared input words are placed in
   a0/a1/a2 by a 6–17 word brick the harness writes at the reset vector (which
   is `crt0`'s text — dead code on this machine) and ends with a jump into the
   branch region. The driver asserts the brick does not reach the corpus
   program's own body, and that the program never writes the brick's scratch
   register (x31), which it checks against the reference trace. The program
   text itself is executed byte for byte from the entry point on.

Both are in the driver's header with the same wording.

## What the branch mix actually was

From the three runs' own stdout (`[run N]` lines), the reference's count of
executed control transfers:

| input | a | b | c | retires | control | taken | branches | jal | jalr | back edges | redirects |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | `0x0` | `0x1` | `0x3` | 74 | 31 | 24 | 15 | 9 | 7 | 11 | 24 |
| 1 | `0x8000000000000000` | `0x7fffffffffffffff` | `0x5` | 101 | 40 | 31 | 18 | 11 | 11 | 16 | 31 |
| 2 | `0xffffffffffffffff` | `0xffffffffffffffff` | `0x9` | 81 | 25 | 19 | 15 | 7 | 3 | 8 | 19 |
| **total** | | | | **256** | **96** | **74** | **48** | **27** | **21** | **35** | **74** |

* **Taken and not-taken are both exercised, by the same instructions.** The
  three inputs drive the same six conditional branches to different outcomes
  (input 0: `blt` and `bltu` taken; input 1: `blt` and `bgeu` taken; input 2:
  `beq`, `bge` and `bgeu` taken), so 26 of 48 branches are taken and 22 are
  not. There is no predictor in this package (`pred_valid` is tied low), so a
  taken branch is a redirect by construction and a not-taken branch is
  accounted for without one.
* **JAL and JALR both run.** 27 JAL/`ret`-shaped transfers and 21 JALR. The
  JALR whose target is deliberately odd (`ori t1,t1,1`) is executed 3, 5 and 1
  times by inputs 0, 1 and 2; if bit 0 were not cleared the machine would fetch
  at an odd address and the retire stream would diverge immediately.
* **35 back edges.** The popcount loop's `bnez t1,1b` and the JALR loop's
  `j 1b` are the backward transfers; the retire order and the redirect targets
  are checked on every one of them.
* **Mispredicts per hundred instructions**: with the predictor disabled, every
  taken control transfer redirects, so the redirect rate is 74/256 = **28.9
  redirects per 100 retired instructions** (74 of 96 control transfers), and
  the arbiter reported waiting (`o_redir_wait_ctr` = 64, 85 and 55 cycles) on
  every one of them — the head gate is exercised, not assumed.

## What the case found

Four integration defects, all in the traffic between units; none of them in a
unit's own registered case, which is why they survived. Three are in the
control path. Each has a negative control so it cannot come back silently.

### D3 — the arbiter's head gate ignored retire lane 0's neighbour

`mosaic_redirect_arb`'s rule is "the request may only act in the cycle its own
macro is the ROB head and that head is being retired". The core retires **two
entries per cycle**, and the arbiter's head view was wired to lane 0 only
(`rob_head_*`, `rob_retire_ack`). A branch that became the head in the cycle the
entry in front of it retired therefore left in *lane 1* and never matched
`off == 0`: its request stayed pending for ever, the branch barrier was never
released and dispatch never allocated again.

The first run of this driver deadlocked on the harness brick's own `jal`:

```
cycle 17  c1v=1 c1t=1 c1idx=5 c1gen=5 | hv=1 hr=1 hidx=4 hgen=4 occ=2 act=0 …
cycle 17  [trace] retire pc=0x80000014 rd= 0 we=0   (lane 1: the jal itself)
cycle 18  c1v=1 c1t=1 c1idx=5 c1gen=5 | hv=0 hr=0 hidx=6 hgen=0 occ=0 act=0 …
          … stalled, allocated=6 retired=6, for ever
```

The request names index 5 generation 5 — the jal — and it retires in lane 1 with
lane 0 (index 4) in the same cycle; the ROB is empty afterwards, so index 5 is
never the lane-0 head and the request can never act.

Fix: the arbiter takes a second head view (`head1_valid/index/gen/retire`) and
acts when the request's identity *is* either retiring entry, by identity
equality rather than by an offset. This keeps the rule's purpose — by the end of
that cycle every older instruction has committed, either this cycle or an
earlier one — which is what the squash below needs. A consumer with one retire
lane ties the second view low and gets the original rule unchanged, which is
exactly what the wrapper's standalone directed arbiter does.

### D4 — the fetch unit's output register survived a redirect, and re-delivered

Two defects in one register, both in `mosaic_fetch.sv`:

* the redirect retires the fetch unit's outstanding slots and bumps its epoch,
  but it did **not** clear the instruction already sitting in the output
  register. That instruction was fetched after the branch the redirect came
  from, so it is wrong-path by construction — and the next cycle it was
  delivered to the decoder as though it were correct.
* the register's drain sat in a `else` branch of the response handler, so a
  *dropped* (non-live) response in the same cycle suppressed the drain. The
  instruction then stayed in the register and was delivered again every cycle
  the consumer was ready.

Together they produced an unbounded stream of one wrong-path PC retiring:

```
cycle 18  out_v=1 out_pc=0x80000020 rsp_fire=0 redir=1        (the redirect)
cycle 19  out_v=1 out_pc=0x80000020 rsp_fire=1 live=0 stale=1 (pushed to the dbuf)
cycle 20  out_v=1 out_pc=0x80000020 rsp_fire=1 live=0 stale=1 (pushed again)
          … every cycle that the decode buffer had room
retire 6 pc=0x80000020  retire 7 pc=0x80000020  …  five times
```

Fix: the output register has one lifecycle, written once — a live response
installs, a redirect retires, otherwise it drains when the consumer takes it —
and a dropped response never holds it.

### D5 — two operands in one register-file bank, and the second became the first

`mosaic_dispatch`'s operand fetch read both sources through one read port per
bank. When the two source tags mapped to the same bank, slot 0 won and slot 1
was not read — but the "value ok" test only asked whether *that bank's* response
was valid, which it was, because it was slot 0's response. `s2_val_sel` then
read the same bank's data: **the second operand silently became the first.**
The comment said slot 1 "is re-offered next cycle", which could never happen:
slot 0 wins the conflict unconditionally, so slot 1 was never read at all.

Found as a wrong branch outcome, not as a stall:

```
ref  [96] pc=0x800012c4 rd=6 we=1 value=0x0000000000000023   (xor t1,t1,t0)
dut  [96] pc=0x800012c4 rd=6 we=1 value=0x000000000000002c
```

t0 (the branch mask) was 0x29 instead of 0x26, which is the mask for `a0 == a1`;
the machine had resolved `beq a0,a1` with `a0 = 0x8000000000000000` and
`a1 = 0x7fffffffffffffff` as **taken**.

Fix: the pair is fetched over two cycles. When the two sources share a bank,
slot 0 is read first into a holding register and slot 1 is read the cycle
after, with the held value standing in for slot 0; the value-ok test requires
the source to have actually owned the bank (or to be the held one), so the
conflict stalls the insert for one cycle instead of corrupting it.

### The control path as shipped

`mosaic_core` section 12b now drives rename's recovery:

* **the checkpoint** is taken on the redirect pulse. That is the cycle in which
  the redirecting branch's own commit has landed (`mosaic_retire` publishes the
  commit combinationally with the retire acknowledgement), and under the
  barrier nothing younger has allocated — so it is the one cycle in which
  `speculative map == committed map`, which is the precondition
  `mosaic_rename` documents for a checkpoint it will accept a squash to. The
  core samples it there rather than assuming it, and the case asserts the
  squash was *accepted*.
* **the squash** follows one cycle later, when the restore can no longer race
  the commit that funded the checkpoint. Under the barrier it restores a state
  that is already correct — that is the barrier's whole purpose — so this wave's
  contribution is that the path is wired, the precondition is checked and the
  counters are visible; the day I-018's saved-map controller lifts the barrier,
  a squash that is no longer a no-op lands on the same signals.

## Negative controls

`python3 tools/run_corpus_branch_controls.py`. Every mutant is built from an
empty directory with one `-D`, must differ from the shipping binary (`cmp`),
and must exit 1 naming the check it breaks:

| mutant | defect injected | first failure |
| --- | --- | --- |
| `MOSAIC_FETCH_MUTANT_OUT_REG_NO_REDIRECT_FLUSH` | a redirect does not retire the fetch unit's output register | `the retirement stream follows the reference: retire 6 pc expected 0x8000122c, got 0x80000020` — a squashed instruction retiring |
| `MOSAIC_CORE_MUTANT_REDIRECT_NEXT` | the front end resumes one instruction past the redirect target | `the front end fetches the redirect's target: after the redirect to 0x8000122c the fetch PC is 0x80001230` |
| `MOSAIC_CLUSTER_MUTANT_IGNORE_TAKEN` | the resolution is raised as not taken whatever the comparator said | `the arbiter issued 0 redirects, the reference executed 24 taken control transfers` |
| `MOSAIC_CORE_MUTANT_NO_PURGE` | the redirect does not clear the decode buffer | `the arbiter issued 1 redirects, the reference executed 24 taken control transfers` |
| `MOSAIC_CORE_MUTANT_EARLY_CKPT` | the checkpoint is taken in the act cycle, before the branch's commit | `the divergent-recovery counters stay zero: squash_nc=1 under=0 journal=0` |
| `MOSAIC_CORE_MUTANT_EARLY_BARRIER_RELEASE` | the barrier is released when the branch resolves, not when the arbiter acts | `an empty ROB is at a rename boundary: occupied=0, o_rename_boundary=0 at commit=6` |
| `MOSAIC_DISPATCH_MUTANT_BANK_CONFLICT_VALUE` | the second source is read from the first source's bank | `the redirect the arbiter issues names the taken transfer's target: redirect 1 pc expected 0x80001240, got 0x80001238` |

All seven build, all seven produce a binary that differs from the shipping one,
and all seven exit 1 on exactly the check named above. The three the card asks
for are the first three rows: *a redirect does not purge younger work (a
squashed instruction retires)*, *the redirect PC is off by one instruction*, and
*the branch's resolution is not taken into account (wrong-path work commits)*.

**One control is documented as NOT OBSERVED by this case**:
`MOSAIC_FETCH_MUTANT_OUT_REG_DRAG` rebuilds the "a dropped response suppresses
the drain" half of D4 and the case still passes. With the redirect flush in
place the register is empty whenever a stale response is dropped, so the
mutation has nothing to hold — the two halves of D4 are only jointly observable
in this program. The define is kept (it is the defect that was fixed, and the
assertion in the source says which failure it would produce), it is listed in
the runner as unobservable rather than counted, and it is in the *not covered*
list below. That is a coverage gap in the case, stated rather than hidden.

## Not covered, honestly

* **Younger uops inside a cluster.** The conservative recovery makes a branch a
  barrier: nothing is dispatched behind a branch until it has resolved and, if
  it redirected, until the redirect has been applied. So no younger uop ever
  reaches an issue queue, and the cluster's purge FSM only ever runs its
  "queue is empty" path in this case. The mechanism is wired (the arbiter's
  redirect pulses the cluster flush) and is what discards the front-end work
  D4 kept alive, but the *kill* arm has no reachable stimulus here. The
  closest reachable control is `MOSAIC_CORE_MUTANT_EARLY_BARRIER_RELEASE`,
  which lifts the barrier, puts younger work in flight and fails the case on
  rename's own boundary invariant — it shows the barrier is load-bearing, not
  that the cluster's purge is.
* **A read of an architectural register that has never been written since
  reset deadlocks the machine.** Found while characterising the mutants, not
  fixed here: it is not in the control path this wave owns, and the fix is a
  change to the readiness model that this case cannot verify. `p02_branch`
  writes every register it reads, so the case passes. Reproduction: run this
  case with the brick's two `EmitLi64` calls for a0/a1 removed (the branch then
  compares two reset mappings) and the machine stops making progress at
  `beq a0,a1`:
  `stalled: head_pc=0x80001230 head_valid=1 occupied=1 allocated=5 retired=4
  stopped=0 redirects=1`.
  The mechanism: at reset rename maps `x_a → tag a, generation 0`; the
  writeback arbiter's ready table starts empty, so `rq_written[tag a]` is false,
  dispatch marks the source not-ready and the issue queue waits for a wakeup
  that will never come. Reading such a mapping should read zero. It needs the
  reset mappings to be marked written *and* the PRF's "never written" response
  accepted for them — and the first allocation of a tag also carries generation
  0, so tag-and-generation alone does not identify a reset mapping. That is a
  readiness wave of its own; it is called out here rather than half-done.
* **Loads, stores, CSRs, traps.** Not in this package: dispatch refuses them and
  stops the machine, so the program's `SIG0..SIG3` stores and its `FINISH_PASS`
  never execute, and the comparison is per instruction over the architectural
  event stream rather than an end-state check. The signature *values* are still
  checked — they are in t0/s3/t4/t3 when the machine stops.
* **The predictor.** Disabled (`pred_valid` tied low), so every taken transfer
  redirects and the "mispredict rate" above is the redirect rate, not a
  predictor's error rate. The arbiter's oldest-wins rule between two
  simultaneous resolutions is therefore exercised only by the standalone
  directed test in `fabric.fixed_two_cluster`, not here: a branch is a barrier,
  so two resolutions cannot be outstanding at once.
* **Two-lane head arbitration standing alone.** The wrapper's standalone
  arbiter instance ties `head1_*` low, so the new lane-1 rule is covered only
  through the core (by this case). The directed single-lane properties are
  unchanged and still pass.
* **Areas of the recovery that the barrier makes unreachable**: the undo
  journal is emptied by every checkpoint and never walked (the squash restores
  a state that is already correct), so `journal_overflow`, the journal walk and
  the free-list restore of a non-empty window are exercised by
  `recovery.nested_branch_full_queues` (I-018), not here.
* **Timing, area, synthesis.** No claim.

## Commands

```
python3 tools/run_unit.py --profile p0 --case core.corpus_branch      # PASS
python3 tools/run_unit.py --profile p0 --case fabric.fixed_two_cluster  # PASS
python3 tools/run_corpus_branch_controls.py                          # 7/7 OK, 1 documented non-observation
python3 tools/run_core_controls.py                                   # fabric's 3 controls still OK
python3 tools/lint_rtl.py --profile p0                               # every file this wave touched: clean
                                                                    # (1 unrelated failure: mosaic_load_queue.sv, another lane's new file)
slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)  # 0 errors
python3 tools/host_oracle.py --program p02_branch                    # the signature expectations
```
