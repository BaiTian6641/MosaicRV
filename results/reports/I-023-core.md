# I-023 — 实现静态双 cluster 对照 (CASE=`fabric.fixed_two_cluster`)

Work package **I-023** of `docs/implementation-plan.md`, implementing the
obligation

> 把程序接到两个 cluster 上（固定亲和）；执行相互独立的 ALU 链与 RAW 链；证明可有多条
> in-flight 且可乱序完成。
>
> Pass: retire 对照正确，trace 显示两个 cluster 在跑不同的 live uOP，且 younger uop 可以
> 先于 older 完成，但绝不能先于它 retire。
> Fail: 第二个 cluster 从未被使用，或机器实际上是单发射解释执行却声称 OoO。

## STATUS: COMPLETE

| Acceptance item | Evidence |
| --- | --- |
| The card's pass criterion: retire 对照正确 | the retirement stream (`ev_pc`, `ev_rd`, `ev_reg_we`, `ev_value`, 2 lanes) is compared event by event against an RV64IM interpreter written from the ISA text: 17 of 17 events, program order, exit 0 — see *The expectation model* |
| The card's pass criterion: two clusters on different live uOPs | cluster 0 executed ROB indices `0 2 4 6 8 10 12 14 16`, cluster 1 `1 3 5 7 9 11 13 15`: disjoint sets whose union is the whole program, checked from the granted uop ids; both queues non-empty in the same cycle (`both_queues_live_cycles=1`) — see *Both clusters are live* |
| The card's pass criterion: younger completes first, retires later | index 13 (cluster 1 ALU) completed at cycle 49, index 12 (cluster 0 M-extension) at cycle 114; retirement order is program order, index 12 retired at cycle 115, index 13 at 115 — see *Out-of-order completion* |
| The card's fail criterion: the second cluster is never used | `MOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER` forces every macro to cluster 0; the case exits 1 with `o_c1_alu_ctr=0 -- the second cluster was never used` as the first failure — see *Mutants* |
| The card's fail criterion: single-issue interpreted execution claiming OoO | the completion order is checked not to be program order (index 13's completion is published 65 cycles before index 12's), and multiple in-flight is checked (`max ROB occupancy = 6`) |
| Stronger than the card: program order in the decode buffer | a real RTL defect was found (two instructions swapped at start-up), fixed, and left with a negative control — see *Two integration defects* |
| `python3 tools/run_unit.py --profile p0 --case fabric.fixed_two_cluster` | `RESULT PASS fabric.fixed_two_cluster checks=59 comparisons=678 cycles=129 retires=17 …`, exit 0 |
| `python3 tools/run_core_controls.py` | 3 mutants, each mutating the binary, each exit 1 with the named check — see *Mutants* |
| `python3 tools/lint_rtl.py --profile p0` | `lint: 32 source file(s) clean`, including `ok rtl/core/mosaic_core.sv: clean as mosaic_core`, `ok rtl/core/mosaic_dispatch.sv: clean as mosaic_dispatch` |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | **0 errors** (STYLE-2/STYLE-13 warnings are project-wide and pre-existing) |
| `python3 tools/run_unit.py --profile p0 --all` | my case PASS; the remaining failures are other lanes' (five cases whose registered sources do not exist yet and one V-012 case) and share no source with this work — see *The full suite* |
| `tests/unit/registry.json` untouched | the case was registered already and is used unchanged (it is the lead's file; `config/status/implementation_status.json` and `results/PROGRESS.md` are too, and were not edited here. The two `--all` runs rewrote `results/unit/*/result.json` and `run.log`, which is what the runner does by design.) |
| This report | — |

## Files

| File | Change |
| --- | --- |
| `sim/unit/tb_core.cpp` | **the driver** (the source the case was blocked on: it did not exist on disk when this work started) — memory model, independent RV64IM reference, the architectural comparison, the fabric and out-of-order controls, and the directed redirect-arbiter test |
| `sim/tb/mosaic_core_tb.sv` | geometry outputs added (`o_geom_*`) so the driver decodes identities and builds the PC sequence from the DUT's own widths and reset vector instead of re-deriving them; nothing else changed |
| `rtl/core/mosaic_core.sv` | **decode-buffer ordering defect fixed** (section 2) and **committed-map generation domain fixed** (section 14); two `MOSAIC_CORE_MUTANT_*` negative controls added |
| `rtl/core/mosaic_dispatch.sv` | `MOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER` negative control added; no functional change |
| `tools/run_core_controls.py` | **new**: rebuilds the case from an empty directory with one defect injected, requires the binary to differ from the shipping one and the run to exit 1 naming the check it breaks (modelled on `tools/run_loader_controls.py`) |
| `results/reports/I-023-core.md` | this file |

`rtl/core/mosaic_core.sv`, `rtl/core/mosaic_dispatch.sv` and
`sim/tb/mosaic_core_tb.sv` are listed by **no other case** in
`tests/unit/registry.json` (checked), so nothing in the other lanes' elaboration
changed.

## What the case drives

The DUT is the integrated p0 core:

```
fetch -> 2-entry decode buffer -> dispatch (one macro per cycle, alternating
cluster affinity) -> two clusters (1 ALU + 1 branch resolver each; cluster 0's
issue queue also routes to the shared iterative MUL/DIV) -> writeback arbiter
-> PRF -> ROB (64) -> retire (2 lanes) -> commit to rename
```

The wrapper also instantiates a standalone `mosaic_redirect_arb`, which the core
itself cannot exercise with two simultaneous resolutions (a branch is a barrier
in this package), so the driver drives that DUT directly as well.

Six phases, each failing by name and cycle:

1. `geometry` — the widths the driver decodes with, read out of the elaborated
   DUT: XLEN 64, two clusters, 2-lane retire, `rob_entries == 1 << rob_index_w`,
   `prf_tag_w == int_gen_w == clog2(prf_entries)`, `occ_w == clog2(rob_entries+1)`,
   `seq_w == clog2(2*rob_entries+1)`, `ret_id_w == 2*prf_tag_w`,
   `uop_id_w == rob_index_w + rob_gen_w + uop_index_w`, `req_id_w ==
   clog2(fetch_outstanding)`, `epoch_w == rob_index_w + 1`.
2. `reset-state` — nothing committed, not stopped, ROB empty, and at a rename
   boundary before the first instruction.
3. `redirect-arbiter` — five directed properties of the standalone arbiter:
   oldest wins with two live requests and the younger taken loser is
   acknowledged; a request behind the head waits (and the wait is counted) and
   acts in the cycle it becomes the retiring head; a request whose slot is
   outside the occupied window is dropped and counted; a request at the head
   index with a recycled generation is dropped; a not-taken winner raises no
   redirect and drops nothing younger.
4. `core-program` — the program runs to the refused ECALL and the architectural
   event stream is compared against the reference.
5. `cluster-fabric` — both clusters executed, affinity, disjointness, queue
   overlap, in-flight depth.
6. `out-of-order-completion` — the completion order is not the program order,
   and the retirement order is.

Standing per-cycle comparisons (678 of them) run alongside: the core's own
`o_commit_ctr` against the event stream it is publishing; a published completion
and a granted uop naming a macro that was actually allocated; rename map
equality whenever the ROB is empty; no redirect, no squash and no illegal decode
in a straight-line program; ROB occupancy within the ROB; retire lane 1 never
set without lane 0; the retire sequence strictly increasing.

## The program

17 instructions plus a terminator, assembled with the project's pinned
toolchain (`riscv64-elf-*`), pure RV64IM — no load, store, CSR, fence or trap,
because dispatch refuses those *before* allocating and stops the machine. `MUL`/
`DIV` is the only long-latency resource in the package.

| idx | pc | instruction | cluster (affinity) | role |
| --- | --- | --- | --- | --- |
| 0 | …00 | `addi x1, x0, 11` | c0 | independent chain A |
| 1 | …04 | `addi x2, x0, 22` | c1 | independent chain B |
| 2 | …08 | `addi x3, x0, 33` | c0 | independent chain A |
| 3 | …0c | `addi x4, x0, 44` | c1 | independent chain B |
| 4 | …10 | `add x5, x1, x2` | c0 | RAW across clusters |
| 5 | …14 | `add x6, x3, x4` | c1 | RAW across clusters |
| 6 | …18 | `sub x7, x5, x1` | c0 | RAW chain in c0 |
| 7 | …1c | `sub x8, x6, x3` | c1 | RAW chain in c1 |
| 8 | …20 | `addi x9, x0, 1` | c0 | seed the wide dividend |
| 9 | …24 | `slli x9, x9, 63` | c1 | RAW chain in c1 |
| 10 | …28 | `addi x9, x9, -1` | c0 | RAW chain in c0 |
| 11 | …2c | `addi x13, x0, 3` | c1 | divisor |
| 12 | …30 | `div x10, x9, x13` | c0 (MUL/DIV is forced to c0) | the long-latency uop |
| 13 | …34 | `andi x14, x0, 123` | c1 | younger, immediately ready |
| 14 | …38 | `or x15, x14, x7` | c0 | RAW after the division |
| 15 | …3c | `slli x16, x15, 2` | c1 | RAW chain in c1 |
| 16 | …40 | `sltu x17, x16, x9` | c0 | RAW chain in c0 |
| 17 | …44 | `ecall` | — | refused by dispatch: the machine stops |

Values the comparison is non-vacuous about: `x9 = 0x7fffffffffffffff`,
`x13 = 3`, and the reference's quotient for index 12 is
`0x2aaaaaaaaaaaaaaa` — a value an idle or single-bit machine cannot produce.
The driver additionally checks that value by name.

```sh
# the program is embedded in sim/unit/tb_core.cpp; this is how the words were
# produced (and the .S listing is a comment there, line for line)
riscv64-elf-as -march=rv64im_zicsr_zifencei -mabi=lp64 -o prog.o prog.S
riscv64-elf-ld -Ttext=0x80000000 -e _start -nostdlib -o prog.elf prog.o
riscv64-elf-objcopy -O binary prog.elf prog.bin      # 72 bytes, 18 words
riscv64-elf-objdump -d prog.elf
```

```
0000000080000000 <_start>:
    80000000: 00b00093   li   ra,11
    80000004: 01600113   li   sp,22
    80000008: 02100193   li   gp,33
    8000000c: 02c00213   li   tp,44
    80000010: 002082b3   add  t0,ra,sp
    80000014: 00418333   add  t1,gp,tp
    80000018: 401283b3   sub  t2,t0,ra
    8000001c: 40330433   sub  s0,t1,gp
    80000020: 00100493   li   s1,1
    80000024: 03f49493   slli s1,s1,0x3f
    80000028: fff48493   addi s1,s1,-1
    8000002c: 00300693   li   a3,3
    80000030: 02d4c533   div  a0,s1,a3
    80000034: 07b07713   andi a4,zero,123
    80000038: 007767b3   or   a5,a4,t2
    8000003c: 00279813   slli a6,a5,0x2
    80000040: 009838b3   sltu a7,a6,s1
    80000044: 00000073   ecall
```

SHA-256 of the 72-byte image: `9820ba4cbc761ff1462ce3fef7bfe5260bcd830b4610aeee47e9134641d09e8f`.

The instruction memory is a word array in the driver with a one-cycle
request/response handshake (`imem_req_id`/`imem_req_epoch` echoed back,
`imem_rsp_len = 4`), and it returns `ECALL` for any address past the program, so a
runaway fetch stops the machine cleanly instead of reading a don't-care. Every
accepted request produces exactly one response — the precondition the fetch
unit's credit rule states. One window of six withheld responses is opened once
the pipeline holds six macros, so the front end is also exercised under
back-pressure (the machine drains the decode buffer and resumes; the run is 129
cycles instead of 123).

## The expectation model, and why it is independent

`ReferenceTrace()` in `sim/unit/tb_core.cpp` is a small RV64IM interpreter:
decode by opcode/funct3/funct7, a 32-entry architectural register file, the
M-extension rules from the ISA text including division by zero (`-1`) and the
`INT_MIN / -1` overflow case, 64-bit wrapping, and `SLLI`'s shamt masking. It
decodes **the same 18 words** the core is fed and produces the expected
retirement record — `pc`, `rd`, `reg_we`, `value` — for each instruction.

It shares nothing with the RTL: not a decoder, not an encoding table, not a
register file, and not the assembly listing (it reads the encoded words). A word
it does not implement fails loudly rather than defaulting, so the program cannot
drift away from the model. The values are additionally anchored to a hard-coded
quotient (`0x2aaaaaaaaaaaaaaa`) so a program edit cannot silently weaken the
comparison.

## Both clusters are live — the evidence

From the run's own stdout (DUT counters and the driver's grant trace):

```
[fabric] c0_alu=8 c1_alu=8 muldiv=1 c0_grants=9 c1_grants=8
         both_queues_live_cycles=1 same_cycle_dual_offer=0 max_rob_occupied=6
[fabric] cluster-0 executed indices: 0 2 4 6 8 10 12 14 16
[fabric] cluster-1 executed indices: 1 3 5 7 9 11 13 15
```

* **Both clusters executed.** `o_c0_alu_ctr=8` and `o_c1_alu_ctr=8`, plus
  `o_muldiv_ctr=1` through cluster 0: every instruction that retires (17 of
  them) had its uop complete, and a macro can only complete in the cluster its
  queue was inserted into.
* **The affinity rule holds, checked against the program's own index
  arithmetic.** Every uop cluster 0 was granted carries an even ROB index and
  every uop cluster 1 was granted an odd one; the raw index lists above are the
  trace the card asks for, and they are disjoint and cover `0..16`.
* **The two clusters executed disjoint uop sets whose union is the program** —
  the control that fails if either cluster does nothing or if one uop is counted
  twice.
* **Both queues held live work at the same instant** (`both_queues_live_cycles
  = 1`), and six instructions were in flight at once
  (`max ROB occupancy = 6`), so this is not single-issue interpreted execution.
* The driver's recorded grant pulses equal the DUT's counters exactly (9 and 8 —
  every offer was accepted immediately, nothing was refused or re-offered), so
  the trace and the counters are the same event stream seen twice.

**What is not asserted, and why.** A grant *offered by both clusters in the same
cycle* never happens in this design and is not required: dispatch allocates one
macro per cycle and alternates the clusters, so the two issue streams are
staggered by construction and each cycle presents exactly one cluster to the
writeback arbiter. The measurement (`same_cycle_dual_offer=0`) is reported rather
than asserted; asserting it would be a check that cannot pass, and dropping it
is recorded here rather than hidden.

## Out-of-order completion with in-order retirement

```
[ooo] index 12 completed at cycle 114 (retired 115), index 13 at cycle 49 (retired 115)
[quiescent] cycle=129 occupied=0 rename_boundary=1 commit=17 free_tags=64 stopped=1
```

* Index 12 is the `div` and index 13 the `andi` immediately behind it. The
  shared unit is fixed-latency (65 cycles at 64-bit width), the `andi` is ready
  the cycle it is inserted: **index 13 completed 65 cycles before index 12**
  (`publish_cycle(13) = 49 < publish_cycle(12) = 114`), observed on the arbiter's
  published-completion port.
* At index 13's completion, index 12 had **neither completed nor retired** —
  cluster 0 was still iterating while cluster 1's uop completed.
* Index 13 **did not retire before index 12**: the retirement stream is program
  order, index 12 in lane 0 of cycle 115 and index 13 in lane 1 of the same
  cycle.
* The completion order as a whole is not the program order (`publications=17` is
  not ascending in ROB index: `… 10, 11, 13, 14, 15, 16, 12`), and each
  instruction completed exactly once with `rob_index == program position`, which
  is the mapping those two checks rely on.

## Two integration defects found and fixed

Both are in `rtl/core/mosaic_core.sv`, which this work package owns; both were
found by this case (there was no driver for the case before, so nothing had run
the integrated core); both now have a negative control so they cannot come back
unnoticed. **These are the honest substance of this report: the case does not
pass because a check was weakened, it passes because two defects were removed.**

### D1 — the decode buffer swapped two instructions (program-order violation)

The first run of the new driver failed at the program-order comparison:

```
CHECK FAILED retire 1 pc: expected 0x0000000080000004, got 0x0000000080000008
```

The trace showed the allocation order `0, 2, 1, 3, …`: the macro at `…08` was
allocated *before* the one at `…04`, so the machine retired two instructions out
of program order. The decode buffer's push wrote slot 0 whenever nothing was
popped (`dbuf_pc[dbuf_take ? 1 : 0] <= …`), which overwrites a live entry
when the buffer holds exactly one entry in **slot 1** — the state a pop leaves
behind, because the shift moves the (invalid) slot 1 down and the push replaces
it at slot 1. The following push then landed in front of an older instruction.

Fix (section 2): the next state is computed per slot, the push lands at the tail
after the pop (`dbuf_cnt - dbuf_take`, one bit here), and the slot a pop vacates
is invalidated — so the live entries are always exactly `[0 .. dbuf_cnt-1]`.

Evidence after the fix: the retirement PCs are
`…00 04 08 0c 10 14 18 1c 20 24 28 2c 30 34 38 3c 40`, and
`MOSAIC_CORE_MUTANT_DBUF_PUSH_SLOT` rebuilds the defective push slot and the same
comparison fails again (exit 1, `retire 1 pc`).

### D2 — the committed map was keyed on the ROB generation, not the tag's

The rename-boundary comparison failed after every run:

```
CHECK FAILED core-program: the machine is at a rename boundary once quiescent
             -- o_rename_boundary=0 with the ROB empty after 17 retirements
```

`mosaic_rename` stores `spec_gen[a] = alloc_new_gen` (the *physical tag's*
generation, `gen[tag] + 1`) and installs the committed map from the commit port;
`ckpt_committed` (exposed as `o_rename_boundary`) is `speculative map ==
committed map`, which the core's header documents as holding at every quiescent
point and which the redirect/checkpoint path depends on. The core drove that
commit generation from `ret_commit_gen`, which `mosaic_retire` derives from
`rob_id` — and the core feeds `rob_id` the ROB's own entry generation. The two
are different counters that happen to be the same width (`MOSAIC_ID_W_ROB_GEN` =
`MOSAIC_INT_PRF_TAG_W` = 7), so nothing in the build objected; the map equality
was false from the first commit on.

Fix (section 14): the commit generation comes from the descriptor store
(`rd_gen0`/`rd_gen1`, the destination identity the ROB does not carry, read
combinationally at the same head index the retire lane commits), and the retire
module's own `commit_gen` output is deliberately left unconnected with the
reason stated in the source. `o_rename_boundary` is now 1 at every quiescent
point, and `MOSAIC_CORE_MUTANT_ROB_GEN_COMMIT` restores the old wiring and fails
the boundary comparison.

A related, incidental observation worth recording: while editing D2 I dropped
`ren_commit2_valid/rd/tag` (the second retire lane's commit wires) for one build,
and this same boundary comparison caught it at cycle 112 (`commit=17`). A check
that catches a broken second commit lane is a check with teeth.

## Mutants

`tools/run_core_controls.py` builds the shipping case from an empty directory,
runs it (it must pass), then rebuilds it with one define, requires the binary to
differ from the shipping one (`cmp`), and requires exit 1 with the named check as
the first failure. Observed output:

```
building the shipping case from an empty directory...
  baseline exit=0 RESULT PASS fabric.fixed_two_cluster checks=59 comparisons=678
              cycles=129 retires=17 c0_alu=8 c1_alu=8 muldiv=1
              dual_grant_cycles=0 max_occupied=6 ooo(index 13 completed at 49,
              index 12 at 114) seed=1

mutant                                         exit  result
MOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER          1     OK
    RESULT FAIL … cluster-fabric at cycle 129: cluster 1 executed at least one
         ALU uop: o_c1_alu_ctr=0 -- the second cluster was never used
MOSAIC_CORE_MUTANT_DBUF_PUSH_SLOT              1     OK
    RESULT FAIL … core-program at cycle 129: retire 1 pc: expected
         0x0000000080000004, got 0x0000000080000008
MOSAIC_CORE_MUTANT_ROB_GEN_COMMIT              1     OK
    RESULT FAIL … core-program at cycle 39: an empty ROB is at a rename boundary:
         occupied=0, o_rename_boundary=0 at commit=8
all 3 mutants mutate the binary, exit 1 and name the check they break
```

`MOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER` is the card's Fail criterion stated as a
defect (the second cluster is never used) and is the control the acceptance
criterion asks for. The other two are the controls for D1 and D2.

## Commands

```sh
# shipping base, through the official runner (exits 0)
python3 tools/run_unit.py --profile p0 --case fabric.fixed_two_cluster

# all three negative controls, each rebuilt from an empty directory
python3 tools/run_core_controls.py

# gates
python3 tools/lint_rtl.py --profile p0
slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)

# the whole registered suite (my case is one line of it)
python3 tools/run_unit.py --profile p0 --all
```

Observed `run.log` of the shipping case:

```
[retire] 17 events in program order, pcs: 80000000 80000004 80000008 8000000c
         80000010 80000014 80000018 8000001c 80000020 80000024 80000028
         8000002c 80000030 80000034 80000038 8000003c 80000040
[fetch] 24 requests, straight-line from the reset vector, one response window withheld
[quiescent] cycle=129 occupied=0 rename_boundary=1 commit=17 free_tags=64 stopped=1
[fabric] c0_alu=8 c1_alu=8 muldiv=1 c0_grants=9 c1_grants=8
         both_queues_live_cycles=1 same_cycle_dual_offer=0 max_rob_occupied=6
[fabric] cluster-0 executed indices: 0 2 4 6 8 10 12 14 16
[fabric] cluster-1 executed indices: 1 3 5 7 9 11 13 15
[ooo] index 12 completed at cycle 114 (retired 115), index 13 at cycle 49 (retired 115)
      publications=17
RESULT PASS fabric.fixed_two_cluster checks=59 comparisons=678 cycles=129
       retires=17 c0_alu=8 c1_alu=8 muldiv=1 dual_grant_cycles=0 max_occupied=6
       ooo(index 13 completed at 49, index 12 at 114) seed=1
```

## The full suite

`python3 tools/run_unit.py --profile p0 --all` was run after the work landed.
`fabric.fixed_two_cluster` reports `PASS`. The failures it reports are other
lanes' and share no source with this work:

* five cases whose registered sources do not exist yet
  (`arbiter.forward_progress`, `bypass.local_raw_chain`,
  `reconfigure.drain_and_generation`, `steering.capacity_locality`,
  `store.wrong_path_visibility`) — the registry entries are ahead of the RTL;
* `exit.protocol_termination` (task V-012), which builds
  `mosaic_bringup_core.sv` + `tb_exit.cpp`. Its failure mode changed between two
  runs minutes apart (first a `RESULT FAIL … UNDRAINED_STORE`, then a *build*
  failure with `sim/unit/tb_exit.cpp:566:47: error: unknown type name 'A'`),
  which is what a lane mid-edit looks like. None of those files is touched here.

Since `mosaic_core.sv`, `mosaic_dispatch.sv` and `mosaic_core_tb.sv` are
compiled by this case only, no other case's elaboration can have changed.

## Not covered (honest list)

The card is about the cluster fabric. Everything below is *outside* what this
case proves, and is named so the green line is not read as more than it is.

* **Branches, and therefore the whole recovery path.** The program contains no
  branch, JAL or JALR: the branch barrier, the redirect arbiter's use *inside*
  the core, the cluster purge, fetch redirect and the epoch/cancel rules are
  never exercised through the core. The configurable *not-taken* arm of the
  arbiter is covered only in the directed phase, where the driver drives it. The
  branch resolver of the second cluster is never used.
* **The data path.** No load, store, fence, `fence.i`, atomics, MMIO, CLINT, UART
  or boot ROM: dispatch refuses all of them, so `dmem_req_valid` is never
  asserted and the data port's protocol is not checked at all (I-033..I-038).
* **CSRs, traps, interrupts, `mret`, `ecall` as a trap.** The `ECALL` here is
  *refused* by dispatch and stops the machine; the trap path (I-019/I-020) is
  not reached, no exception payload is ever retired, and `ev_trap` is only
  checked to be 0. Multi-hart is not modelled (`hart` is a constant 0 in this
  profile).
* **Caches, coherence and the remote link** — nothing in this package.
* **A second taken resolution at the arbiter with both arms through the core.**
  Only the directed standalone phase presents two live requests.
* **MUL/DIV depth.** Exactly one M-extension op (`div`, non-zero divisor, no
  overflow) executes; the other seven encodings, the W forms, the
  cancel/flush paths and the FIFO-less result hold are I-012's.
* **The writeback arbiter's collateral paths.** One collision-free ALU stream and
  one M-extension completion; `o_wb_collision_ctr`, `o_wb_stale_ctr`,
  `o_wb_dup_ctr` are only required to be consistent with the run, not stressed
  (I-026 owns them).
* **The decode-buffer stale-slot line is not exercised.** The fix in D1 has two
  halves: the push slot (covered by `MOSAIC_CORE_MUTANT_DBUF_PUSH_SLOT`) and the
  invalidation of the slot a pop vacates. Reaching the second needs the buffer
  *full* and then drained with no delivery alongside — dispatch and fetch
  stalled together. A mutant that removes that line
  was built and **passed** (`RESULT PASS … checks=59`), i.e. this program never
  reaches the state; the define was then removed rather than left in the tree,
  because a mutant nothing catches is not evidence. The line is kept because it
  is what makes the live entries exactly `[0 .. dbuf_cnt-1]`, and the gap is
  recorded here instead of being claimed as covered.
* **`same_cycle_dual_offer=0`.** See *Both clusters are live*: the control that
  fails if the second cluster is idle is the disjoint-union/affinity/counter one,
  not a same-cycle offer.
* **One seed, one profile.** `seed 1` as registered, p0 only; the driver asserts
  the geometry it decodes (64-bit, two clusters, 2-lane retire, 4 queued banks of
  identity widths) and fails loudly on a change rather than testing a different
  claim silently.
* **`ev_id`, `ev_store_addr/data`, `ev_trap_cause`** are not compared; the
  architectural comparison uses `ev_pc`, `ev_rd`, `ev_reg_we`, `ev_value` and the
  `ev_valid`/`ev_trap`/`ev_store` lanes. `ev_id` now carries the ROB entry
  generation (unchanged by the D2 fix — the fix moved only what *rename's
  committed map* is keyed on, deliberately not the event identity the retire
  lane's own case documents).
* **Timing, area and synthesis.** No claim.