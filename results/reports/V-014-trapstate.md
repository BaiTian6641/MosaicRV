# V-014 — synchronous traps and precise state

Work package V-014 (`docs/validation-plan.md` §5 V-014, "比较同步 trap 与精确状态").
Registered case **`trap.precise_state`**, top `mosaic_core_tb`, seed 1, budget
4,000,000 cycles.

| Artifact | Role |
|---|---|
| `sim/unit/tb_core_trapstate.cpp` | the case: the M-mode program, the trap handler that observes architectural state, the independent per-trap comparison, the placement coverage, the fail-mode controls' injection points |
| `sim/unit/trap_ref.h` (unchanged) | the independent RV64IM_Zicsr interpreter the per-trap expectation is folded from |
| `sim/common/memory_model.cpp` (unchanged) | the platform memory each side owns a *separate* instance of |
| `tools/run_trapstate_controls.py` | the controls: six mutants (two RTL, four driver), each rebuilt from a deleted directory |
| `results/unit/trap.precise_state/` | the observed run: `run.log`, `result.json` |
| `build/p0/trapstate_controls/` | the control builds: `build_command.txt` and the binary of each mutant, plus `out-*/` |
| `results/reports/V-014-trapstate.md` | this file |

## STATUS: PASS — the case passes from a clean build, all seven synchronous trap kinds are exercised at five placements, and six controls (one per card fail mode, plus a snapshot and a replay view of two of them) fail as required

| Check | Observed |
| --- | --- |
| the registered case | `PASS trap.precise_state` (exit 0) |
| | `checks=167810 comparisons=65917 cycles=12081 retires=4740 traps=35 seed=1` |
| shipping binary (clean build) | `sha256 2673f4bf6212960cb0fdfda0e5f0eb20e932026ecc872bf3d413de419c32bf34` |
| controls | **6 of 6 exit 1**, each naming the check it breaks, each binary differing from the shipping one, each rebuilt from a deleted directory (§5) |
| placements produced | `head=7 away=7 before-long=7 after-long=7 same-cycle=2` out of 35 scenarios; every placement observed, not merely intended (§4) |
| RTL gates | `tools/lint_rtl.py --profile p0` green (34 files); `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` exit 0, 0 errors, only pre-existing STYLE notices; **no RTL file was edited by this package** |
| `tools/check_event_contract.py --profile p0 --negative` | 33/33 negative controls rejected |
| `tools/check_records.py` | green: `ok records agree: 39 delivered package(s), 49 registered case(s), every claimed case exists and belongs to the package claiming it`. The case keeps its `"pending": true` marker; the integration lead clears it when the package is recorded. |
| existing cases re-run | `core.corpus_sweep`, `core.trap_csr_program`, `fabric.fixed_two_cluster`, `retire.width_and_order` all PASS (§9) |
| the driver under the project's C++ standard | `-Wall -Wextra -Wshadow` adds no warning of its own (verilated headers are not `-Wextra` clean; the `lint-cpp` target compiles our sources the same way) |

## 1. The rule the case exists to test

> A synchronous exception is not a retirement. When a load, a store, a system
> macro or an illegal encoding faults, the faulting instruction is dropped, an
> architectural event of its own is published in lane 0 carrying `mcause` and
> `mtval`, and **nothing younger** may have any architectural effect. Everything
> **older** is complete: `mepc` is the faulting PC, `mcause` the ISA's code,
> `mtval` the faulting address for a memory fault and zero for a system trap,
> `mstatus` saves `MIE` into `MPIE` with `MIE` cleared and `MPP` set to M, the
> handler runs at the installed vector, and `mret` restores `MIE` from `MPIE`
> and re-executes from `mepc`. The instruction the trap squashed is neither lost
> nor half-committed: after `mret` it re-executes and its effect appears exactly
> once.

The card's four fail modes are the ways that rule is easiest to get wrong: an
exception's writeback polluting `rd` first, a wrong `mtval` being masked
wholesale, a reference model that takes one instruction too many, and an
instruction lost on replay after the trap. Each is injected as a control (§5).

## 2. The program

One hand-assembled M-mode program at `0x80000000`; 2,768 words. It installs the
vector, sets `mstatus.MIE`, clears `mie`, spends sixteen instructions past the
front end's window, then runs **35 directed scenarios** (7 trap kinds × 5
placements), a final register dump, and the exit protocol.

### 2.1 The seven trap kinds

| Kind | Instruction | `mcause` | `mtval` |
|---|---|---|---|
| illegal instruction | `csrrs x6, 0x7b0, x0` (an unimplemented CSR read) | 2 | 0 |
| `ecall` | `ecall` | 11 | 0 |
| `ebreak` | `ebreak` | 3 | 0 |
| load misalignment | `ld x20, 1(x0)` | 4 | `0x1` |
| store misalignment | `sd x20, 1(x0)` | 6 | `0x1` |
| load access fault | `ld x20, 0(x27)`, `x27 = 0x90000000` (outside the frozen map) | 5 | `0x90000000` |
| store access fault | `sd x20, 0(x27)`, `x27 = 0x90000000` | 7 | `0x90000000` |

The illegal encoding is the unimplemented-CSR read the sibling trap case also
uses; the system trap's `mtval` is zero because `mosaic_core.sv`'s trap
controller publishes `{0}` for a system trap (`trap_tval = head_exc_trap ?
exc_tval_head : 0`), which the Privileged Spec permits and the independent
interpreter models the same way.

### 2.2 The five placements

All seven kinds are emitted at every placement (35 scenarios). The placement is
arranged by the program and **asserted from what the run produced** (§4):

| Placement | Program structure | Expected observable |
|---|---|---|
| `head` | the fault is the first instruction of the scenario, after only the setup | the fault is resolved within a few cycles of its completion; shallow ROB |
| `away` | 24 dependent older `addi` ahead of the fault | the fault completes while older work is still in flight; deep ROB |
| `before-long` | a younger `div` (65 cycles) behind the fault | the younger long-latency instruction has no effect at the trap and re-executes after `mret` |
| `after-long` | an older `div` (65 cycles) ahead of the fault | the older long-latency result is complete in the trap frame; the trap waits for it |
| `same-cycle` | one independent older instruction immediately ahead | an older retirement (or a completion collision) shares the cycle the fault completes in |

### 2.3 The trap handler — how architectural state is observed

The machine publishes no committed register file, so the only honest way to read
precise state is through the trap handler, before it touches anything:

```
handler:
  csrrw t0, mscratch, t0        # t0 <- the frame the program placed in mscratch,
                                # mscratch <- the old t0
  sd    x1..x4, x6..x31         # every register but the base and x0, unmodified
  csrr  t1, mscratch ; sd t1    # recover the old x5 into the frame
  csrr ... mepc/mcause/mtval/mstatus/mtvec/mie/mip ; sd
  ld    t2, PROBE ; sd t2       # the word a *younger* store of the scenario targets
  csrr  t1, mepc ; addi t1,4 ; csrw mepc, t1 ; mret
```

Each scenario seeds five older canary registers (`x10..x14`), two younger
registers (`x18`, `x19`), the faulting load's destination (`x20`), the probe word
and a `div` pair with constants; the younger register writes and the younger
store to the probe are emitted **after** the fault, so after `mret` resumes at
`mepc + 4` they execute for the first time and their effect can be checked
against the trap-time frame.

### 2.4 Two harness facts worth recording

* **The program's own text spans `MOSAIC_TOHOST` (`0x80001000`).** The platform's
  data model treats a *write* at that address as the exit protocol rather than as
  memory, so the instruction image is deliberately **not** copied into the data
  model: doing so reported a false exit with an instruction word as the exit
  code, which the first draft of this case hit. Nothing in the program loads from
  its own text, so the data model needs no code; the fetch port and the
  reference's own fetch both read the image directly.
* **The exit protocol completes when the store drains, not when it retires.**
  Retirement authorises a store; the store queue performs it. The reference's
  stream is exhausted when the exit store retires, and the run then continues
  (with the comparison disarmed) until the platform model reports the write.

## 3. The independent expectation

Nothing in the comparison is taken from the DUT.

* **The per-instruction stream** is `mosaic_trap::RunReference`
  (`sim/unit/trap_ref.h`), executed against its **own** `mosaic::MemoryModel`
  instance. Every retirement the machine publishes is compared with it, pc by pc,
  destination by destination, value by value; every trap entry is compared by
  cause, `mtval` and `mepc`.
* **The architectural register file at each trap** is the *fold* of that
  interpreter's retirements in program order: the register file at the instant
  the faulting instruction is at the head -- after every older instruction and
  before any younger one. That fold is what the frame must equal, field by field
  (32 registers, each reported individually with its value).
* **The trap CSRs** follow the ISA text, not the DUT. `mepc` is the faulting PC
  with its low two bits zero; `mcause` is the kind's code; `mtval` is the
  faulting address for a memory fault and zero for a system trap; `mstatus` at
  entry is `0x1880` (`MPIE <- MIE(1)`, `MIE <- 0`, `MPP <- M`); the handler PC is
  the installed vector. `mret` restores `MIE <- MPIE` and sets `MPIE`, so
  `mstatus` between traps is `0x1888` -- checked against the machine's own
  `o_csr_mstatus_o` between traps and against the next trap's entry value.
  `MIE` is deliberately **set** for the whole run (no source is ever asserted), so
  `MPIE <- MIE` saves a one and the restore is not the trivial zero-to-zero.
* **Precise state, stated against constants this file owns** as well as against
  the fold: the faulting instruction left `x20` at its pre-fault value; the
  younger writes to `x18`/`x19` are invisible; the older canaries `x10..x14`
  completed; the probe word is the scenario's pre-value; the older `div`'s result
  is complete in the `after-long` frames; and the *replayed* younger `div`'s
  result is in the final register dump.
* **The final state**: the machine's whole RAM is compared, word by word, against
  the independent model's -- which executed the same words, including every
  replayed store. Together with the 35 frames and the retire-stream comparison,
  that is the end-to-end statement that the replay neither lost nor duplicated
  an instruction.

The trap event stream is also checked directly: no ordinary instruction retires
in a trap cycle, the faulting PC appears in the lane-0 trap stream, no retirement
is ever published at a faulting PC, and the machine's own `mcause`/`mepc`/`mtval`
wires agree with the trap pulse one cycle later.

## 4. What the run produced

From `results/unit/trap.precise_state/run.log`. `cap_gap` is the cycles between
the exception's completion being captured and the trap being taken (memory faults
only); `occupied` is the ROB depth at the trap; `cap_commit`/`cap_coll` are the
retirements and writeback collisions in the capture cycle. (The same-cycle
retirement count at a *system* macro's execution cycle is also recorded -- it is
zero for every system trap, which is the structural fact §8 explains.)

| # | kind | place | cause | `mepc` | `mtval` | occupied | cap_gap | cap_commit | cap_coll |
|---|---|---|---|---|---|---|---|---|---|
| 0 | illegal | head | 2 | `0x80000124` | `0x0` | 10 | – | – | – |
| 1 | illegal | away | 2 | `0x80000298` | `0x0` | 26 | – | – | – |
| 2 | illegal | before-long | 2 | `0x800003ac` | `0x0` | 9 | – | – | – |
| 3 | illegal | after-long | 2 | `0x800004c8` | `0x0` | 26 | – | – | – |
| 4 | illegal | same-cycle | 2 | `0x800005e0` | `0x0` | 10 | – | – | – |
| 5 | ecall | head | 11 | `0x800006f4` | `0x0` | 10 | – | – | – |
| 6 | ecall | away | 11 | `0x80000868` | `0x0` | 26 | – | – | – |
| 7 | ecall | before-long | 11 | `0x8000097c` | `0x0` | 9 | – | – | – |
| 8 | ecall | after-long | 11 | `0x80000a98` | `0x0` | 26 | – | – | – |
| 9 | ecall | same-cycle | 11 | `0x80000bb0` | `0x0` | 10 | – | – | – |
| 10 | ebreak | head | 3 | `0x80000cc4` | `0x0` | 10 | – | – | – |
| 11 | ebreak | away | 3 | `0x80000e38` | `0x0` | 26 | – | – | – |
| 12 | ebreak | before-long | 3 | `0x80000f4c` | `0x0` | 9 | – | – | – |
| 13 | ebreak | after-long | 3 | `0x80001068` | `0x0` | 26 | – | – | – |
| 14 | ebreak | same-cycle | 3 | `0x80001180` | `0x0` | 10 | – | – | – |
| 15 | ld-misalign | head | 4 | `0x80001294` | `0x1` | 10 | 3 | 0 | 0 |
| 16 | ld-misalign | away | 4 | `0x80001408` | `0x1` | 25 | 30 | 0 | 1 |
| 17 | ld-misalign | before-long | 4 | `0x8000151c` | `0x1` | 9 | 2 | 0 | 0 |
| 18 | ld-misalign | after-long | 4 | `0x80001638` | `0x1` | 63 | 72 | 2 | 0 |
| 19 | ld-misalign | same-cycle | 4 | `0x80001750` | `0x1` | 10 | 3 | 1 | 0 |
| 20 | st-misalign | head | 6 | `0x80001864` | `0x1` | 10 | 6 | 1 | 0 |
| 21 | st-misalign | away | 6 | `0x800019d8` | `0x1` | 26 | 33 | 1 | 0 |
| 22 | st-misalign | before-long | 6 | `0x80001aec` | `0x1` | 9 | 5 | 1 | 0 |
| 23 | st-misalign | after-long | 6 | `0x80001c08` | `0x1` | 63 | 77 | 0 | 0 |
| 24 | st-misalign | same-cycle | 6 | `0x80001d20` | `0x1` | 9 | 5 | 0 | 0 |
| 25 | ld-access | head | 5 | `0x80001e44` | `0x90000000` | 10 | 1 | 0 | 0 |
| 26 | ld-access | away | 5 | `0x80001fc8` | `0x90000000` | 21 | 20 | 1 | 0 |
| 27 | ld-access | before-long | 5 | `0x800020ec` | `0x90000000` | 11 | 2 | 0 | 0 |
| 28 | ld-access | after-long | 5 | `0x80002218` | `0x90000000` | 61 | 71 | 2 | 0 |
| 29 | ld-access | same-cycle | 5 | `0x80002340` | `0x90000000` | 10 | 1 | 0 | 0 |
| 30 | st-access | head | 7 | `0x80002464` | `0x90000000` | 5 | 1 | 1 | 0 |
| 31 | st-access | away | 7 | `0x800025e8` | `0x90000000` | 21 | 26 | 0 | 0 |
| 32 | st-access | before-long | 7 | `0x8000270c` | `0x90000000` | 5 | 1 | 1 | 0 |
| 33 | st-access | after-long | 7 | `0x80002838` | `0x90000000` | 61 | 76 | 2 | 0 |
| 34 | st-access | same-cycle | 7 | `0x80002960` | `0x90000000` | 5 | 1 | 1 | 0 |

Counted coverage (each placement counted only for scenarios of that placement):

| Placement | Produced | Predicate |
|---|---|---|
| `head` | 7 | memory: `cap_gap <= 8`; system: `occupied <= 12` |
| `away` | 7 | memory: `cap_gap >= 16`; system: `occupied >= 20` |
| `before-long` | 7 | the final dump's `x24` is the replayed `div` result (`0x24924`) |
| `after-long` | 7 | an older long-latency result is in the frame and the trap waited (`away` observable) |
| `same-cycle` | 2 | a retirement/collision in the capture cycle (memory kinds only) |

The split is clean: a fault resolved at the head completes and traps within a few
cycles (`cap_gap` 1–6, ROB depth 5–10); one completed away from the head waits
for the older work (`cap_gap` 20–33, depth 21–26); one behind an older `div`
waits the whole 65-cycle unit (`cap_gap` 71–77) while the ROB fills to 61–63.
`same-cycle` is observed for the memory kinds where an older retirement genuinely
shares the cycle the fault completes in; for a **system** macro it is
structurally impossible and is reported as such in §8.

Squashed younger work: the run records 2 accumulator squashes
(`o_squash_acc_ctr`) and 21 operations at the mul/div unit -- the seven older
`div`s plus the seven younger ones issued **both** speculatively (and squashed)
and after replay -- and every scenario's frame shows the younger register and
probe values untouched.

## 5. Controls

`tools/run_trapstate_controls.py`. The card's four fail modes, each injected as
exactly one `-D`, each rebuilt from a deleted directory with its `-D` in that
build's own `build_command.txt`, each binary differing from the shipping one
(`2673f4bf…`), each exiting 1 with the named first failure:

| Define | Layer | Fail mode | Binary sha256 | First named failure |
|---|---|---|---|---|
| `MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL` | RTL (`mosaic_retire`) | 1. exception writeback polluting `rd` | `5133c29230350f84f6ccc1c5f7ee9721201d5173fc341e5e532dfd743b91deee` | cycle 108: **an ordinary instruction retired in the trap cycle** (`pc 80000124`) |
| `MOSAIC_TRAPSTATE_MUTANT_MTVAL_MASKED` | driver | 2. wrong `mtval` masked wholesale | `3aaf84c7e90e800d9d64fe77747adec39a8e5ea8ba7c1da2afe367dd0c60f332` | scenario 15 (ld-misalign/head): **mtval is the ISA's value** (got `0x1`, expected `0x0`) |
| `MOSAIC_TRAPSTATE_MUTANT_REF_EXTRA_INSN` | driver | 3. reference takes one instruction too many | `9b70d0e2e268caa7ec91e0a89c5704b77e439f5992fefaea4762376019834614` | cycle 134: **the retirement stream follows the reference: retire 73** (expected `80000124`, got `80002a54`) |
| `MOSAIC_CORE_MUTANT_MRET_PC_WRONG` | RTL (`mosaic_core`) | 4. instruction lost on replay after the trap | `d14ea7269e8250b0643e586d3d6880b3a0e61742e10b5a7f8820b884b0343a23` | cycle 335: **the retirement stream follows the reference: retire 132** (expected `80000128`, got `8000012c`) |
| `MOSAIC_TRAPSTATE_MUTANT_RD_POLLUTED` | driver | 1 (snapshot view) | `6b1ab4a0d07c3a1a6bef9a93bea255ca47a5b9ee6d2ac5dcb3b99a856dafd0e9` | scenario 15: **x20 at trap entry** (got `0x400f`, expected `0x0`) |
| `MOSAIC_TRAPSTATE_MUTANT_REPLAY_LOST` | driver | 4 (replay view) | `09aa2da143055e084e4de11be1854ed3048c165342377de7b8bfe6ce21ce0357` | cycle 12081: **the post-trap store reached memory** (got `0x6022`, expected `0x5022`) |

Fail modes 1 and 4 are properties of the machine and use the RTL controls the
project already documents. Fail modes 2 and 3 are properties of the **checker** --
"a wrong `mtval` being masked wholesale" is a statement about what the comparison
expects, and "the reference model taking one instruction too many" is a statement
about this case's own reference -- and no RTL mutant can express them; those two
are `-CFLAGS -D` mutations of this file's comparison, labelled DRIVER above and in
the tool. The two extra driver controls make fail modes 1 and 4 fail at the
**snapshot** and **replay** layers, which is where the case's substance is, rather
than only at the retirement stream.

`MOSAIC_RETIRE_MUTANT_TRAP_AS_NORMAL` also demonstrates the event-contract hazard
the card's first fail mode names: under it the trapping entry applies its
writeback, and the case reports it in the same cycle as "an ordinary instruction
retired in the trap cycle", i.e. the machine tried to both trap and retire the
same instruction.

## 6. Taxonomy: what this case drives that `core.trap_csr_program` does not

| Trap kind | `core.trap_csr_program` | `trap.precise_state` |
|---|---|---|
| illegal instruction (unimplemented CSR) | directed program | ✔, at 5 placements |
| `ecall` | directed program | ✔, at 5 placements |
| `ebreak` | directed program | ✔, at 5 placements |
| load misalignment | corpus `p08_misaligned`, directed | ✔, at 5 placements |
| store misalignment | corpus `p08` / `p13` | ✔, at 5 placements |
| store access fault | corpus `p13_romstore` (a store into the read-only boot ROM) | ✔, an **uncovered** store address (`0x90000000`) |
| **load access fault** | not driven | ✔ (an uncovered load address) |
| precise register state at the trap | not checked (traps are checked by cause/epc only) | ✔, the whole register file per trap, against the independent fold |
| placements (head/away/before/after long, same-cycle) | not driven | ✔, produced and asserted |
| `mret` restoration | checked by the program's own handler | ✔, from the machine's `mstatus` and the next trap's entry |

The other case's directed program and corpus are unchanged and still pass (§9);
this case adds the load-access path, the placement matrix, and the precise-state
observation layer.

## 7. Observation: the undo-journal overflow flag is set by this program

`o_journal_ovf_ctr` is **nonzero** (`journal_ovf_seen=1`) once the program runs a
branchless stretch longer than `MOSAIC_ROB_ENTRIES` (64) allocations. This is
recorded rather than asserted away, because it is a fact about the machine and
about this program:

* `mosaic_rename`'s undo window is emptied only by a branch checkpoint
  (`ckpt_valid`) or by the trap path's full restore (`flush_restore`); it is
  **not** advanced by retirement. A branchless run therefore grows the window
  without bound, and this program's handler plus scenario setup exceeds 64
  allocations between traps.
* The flag is **sticky**: once set it is never cleared, not even by the trap
  path's own `flush_restore` that empties the window.
* No architectural effect followed. The trap path restores from the *committed*
  map, not the journal, so the overflow does not touch any architectural result
  here -- and the case proves that independently: every one of the 35 frames and
  the machine's whole RAM match the reference bit for bit. A journal-based
  squash (a branch mispredict to a checkpoint) is bounded by the in-flight window,
  which is the ROB depth (64), so it cannot need to undo more than the journal
  holds.

This is reported, not fixed: two lanes have collided on this core this session,
no RTL file was edited by this package. The sticky flag is worth a maintainer's
look (it would mask a genuinely clamped restore later in a run), but it is not a
precise-state defect.

## 8. Not covered

* **Asynchronous interrupts.** No `irq_soft_i`/`irq_timer_i`/`irq_ext_i` is ever
  asserted; the interrupt path is V-015's. `mie = 0` for the whole run.
* **Delegation.** p0 is M-only and `medeleg`/`mideleg` canonicalise to zero; no
  delegation is driven or checked.
* **S/U mode, virtual memory, and privilege transitions** other than M;
  `mstatus.MPP` is exercised only as its M value.
* **`mtval` for a fetch/instruction-access fault.** All faults here are data-path
  or system traps; an instruction-fetch fault would need a fetch error response,
  which this harness does not drive.
* **The corpus signature protocol and the firmware's own trap handler**
  (`tests/programs/src/trap.S`) -- `core.trap_csr_program` covers those.
* **A `same-cycle-completion` situation for a system trap.** A system macro is
  resolved at the head, so nothing younger can complete and nothing older can
  retire in the cycle it executes: the situation is structurally impossible for
  `ecall`/`ebreak`/illegal, and the case reports `same-cycle` coverage from the
  memory kinds (2 of 7). The `sys_commit` field is recorded and is zero for every
  system trap, which is the positive statement of that fact.
* **Compressed instructions, F/D/V, multi-hart, MMIO/device traps, WFI, FENCE.I**
  -- other packages.
* **The undo-journal overflow** is an observation (§7), not a check.
* **A branch-mispredict squash interleaved with a trap** is not driven: the
  program is branchless except for the trap path and the park loop.

## 9. Gates, revisions and the run set

Gates (run with no RTL edited by this package):

* `python3 tools/lint_rtl.py --profile p0` → `lint: 34 source file(s) clean`.
* `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' | sort)` → exit 0, 0 errors (only pre-existing STYLE notices).
* `python3 tools/check_records.py` → green.
* `python3 tools/check_event_contract.py --profile p0 --negative` → 33/33 rejected.
* Cases re-run to confirm nothing shared broke: `core.corpus_sweep` PASS,
  `core.trap_csr_program` PASS, `fabric.fixed_two_cluster` PASS,
  `retire.width_and_order` PASS. (No shared file was edited, so these are a
  regression guard rather than a cutover.)

RTL revision: git `HEAD = 1ec0fadfccf4fb3465aee080656f684c4bb73121`, and the
sha256 of every RTL file the case compiles:

```
3d6ab3533a44953a7dc66d70e15c3845d9c6a63234e071c82f84ce6b666944fc  rtl/core/mosaic_pkg.sv
edbccd478582ead041ff3bfa7238b1160b7b84036da7124a87e94d71afeef0db  rtl/core/mosaic_uop_pkg.sv
0a92b98eb4c835dc7565b1ebd6cfa5af323c8e203177e0d000d3f23c18c5c38e  rtl/core/mosaic_alu.sv
cf996100038627df14c2e4db364eaf94944a4bd325b8bb7dac22ed901f7d9f4e  rtl/core/mosaic_branch_cmp.sv
0e5f8ca86a4f256aaff4564330d91a35ed65450498c46c45a2ebd55c1b569942  rtl/core/mosaic_branch_target.sv
bad709b88af95cbccef34ab5c668a630b0fff5e8cbb9e700322fd1cc04b2ed53  rtl/core/mosaic_predictor.sv
a83330c5c160cc1b413273ea800944f337e675a22c25d708203f3f523206517c  rtl/core/mosaic_fetch.sv
b5b03c7db72e5a371e24ee0597452716740a6075b5f03e22aafceb5098bade5a  rtl/core/mosaic_decoder.sv
180a1e35c4cc0a5ae5f96eb3b76fad2bc9af650f51ba6f437126c45b5c2a5d6b  rtl/core/mosaic_rename.sv
afba8cf6f4f954d1dfbbc39796baa2d263c8bbd0c7082b5425c8dcf39dd0d2c1  rtl/core/mosaic_rob.sv
447bfabdc3dc5b8c6bbf44fd61c5d37934ea7541fe854c5be55f308ada464240  rtl/core/mosaic_iq.sv
2e695d234e70a62381270433624d98a6b5a48f4fcf0f4aedbb775f218f3c82a6  rtl/core/mosaic_prf.sv
0f6f3ab7a3425ffb16ce22b084d6f195e3f8c1898310ecfb4c2ef1cbd1d2e748  rtl/core/mosaic_wb_arbiter.sv
eab7879e000c5a0be3ca437e96573fb80f7f7da36357f3b239838bfd455567b6  rtl/core/mosaic_macro_desc.sv
a40ce867e76a7a5097f28976cf58f7d0779f89ba9b697e26fd686406ba023acf  rtl/core/mosaic_dispatch.sv
be1b41436d82f669741b7151649257c883bcefa7af2d3c88fbec9130a35025c4  rtl/core/mosaic_cluster.sv
658aeb6b7e35bc43748829ed0686e361a27068e43e8d8d5eca6167c7b52eba91  rtl/core/mosaic_redirect_arb.sv
dd12e9ba3df579593d125790742ed39c5b0b5935be6ef634d2579dbf1f4f2d0d  rtl/core/mosaic_muldiv.sv
c907ce840427d827f4ce2516095a62c61dceaf738b3e3439a5f2ab30defa6571  rtl/core/mosaic_retire.sv
dec1561eabf645a2bd9aa1bfe890c38a5540259f8787024a5a371a6d9277067a  rtl/core/mosaic_core.sv
a159f5f39a5202945b2008e63df401827f65b29d941adf7d92386e712292f32a  rtl/core/mosaic_csr.sv
fb4fbfe2378c868b2ad98284a2bfc028853feff4730037b5b633a798c0a4919a  rtl/core/mosaic_interrupt.sv
```

Case sources at delivery:

```
8837ae3df4497ae9ee96e97f5df306ae0028584a64478154da901c94ec526efc  sim/unit/tb_core_trapstate.cpp
d4735a43bde309112f55eb51dca131153cd2491ab87b230aa160f6a8fd082363  tools/run_trapstate_controls.py
9596bd41dd5a344c05066b66ff968d7c36e56bd9f9f103593a156b9929a6763e  sim/tb/mosaic_core_tb.sv
```
