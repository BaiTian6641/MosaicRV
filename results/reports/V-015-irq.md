# V-015 — the interrupt replay timeline

Work package V-015 (`docs/validation-plan.md` §5 V-015, "中断重放"). Registered
case **`irq.replay_timeline`**, top `mosaic_core_tb`, seed 1, budget 4,000,000
cycles.

| Artifact | Role |
|---|---|
| `sim/unit/tb_core_irq.cpp` | the case: the M-mode program, the handler that logs `{mcause, mepc}` from inside the trap, the retirement-boundary stimulus timeline, the per-cycle checks, the replay comparison, the three controls' injection points |
| `tools/run_irq_controls.py` | the controls: three driver mutants, each rebuilt from a deleted directory with its `-D` in that build's own `build_command.txt` |
| `results/unit/irq.replay_timeline/` | the observed run: `run.log`, `result.json` |
| `build/p0/irq_controls/` | the control builds: `build_command.txt` and the binary of each mutant, plus `out-*/` |
| `results/reports/V-015-irq.md` | this file |

**No RTL file was edited by this package.** `rtl/core/mosaic_interrupt.sv`,
`mosaic_core.sv`, `mosaic_csr.sv` and `mosaic_core_tb.sv` (the wrapper listed in
the registry) are unchanged; the case is entirely a new driver. Nothing in the
tree had to be re-elaborated for a new port.

## STATUS: PASS — the case passes from a clean build, the same stimulus replays to the same asserted and accepted boundaries on a second run, an interrupt asserted while masked is taken after its enable, and all three controls fail as required

| Check | Observed |
| --- | --- |
| the registered case | `PASS irq.replay_timeline` (exit 0) |
| | `checks=70 cycles=549 retires=308 traps=3 trace=0x5e5cbc99d04548e0 seed=1` |
| other seeds (not the registered one) | `PASS` at seeds 2, 3, 7, 42 — 70 checks and exactly 3 traps each, with seed-dependent window lengths and traces (`552/0xe998dd42…`, `550/0x3c453fb1…`, `564/0x2b6e309f…`, `536/0x4a1b9979…`) |
| shipping binary (clean build) | `sha256 d1d45936f6a50c0f608234b6f5601a20d05e00f110b7f4a665dd1c472e4bdfed` |
| controls | **3 of 3 exit 1**, each naming the check it breaks, each binary differing from the shipping one, each rebuilt from a deleted directory (§7) |
| replay | run A and run B: identical 4 asserted boundaries, identical 3 accepted boundaries, identical per-cycle trace `0x5e5cbc99d04548e0`, identical 549 cycles / 308 retires |
| the case still keeps its `"pending": true` registry marker | the integration lead clears it when the package is recorded; this package did not edit `tests/unit/registry.json` |
| `tools/check_records.py` | green: `ok records agree: 41 delivered package(s), 52 registered case(s), every claimed case exists and belongs to the package claiming it` |
| the sibling interrupt case | `interrupt.boundary_replay` (I-020) still `PASS` |
| the driver under the project's C++ standard | `-Wall -Wextra -Wshadow` adds no warning of its own (verilated headers are not `-Wextra` clean; the `lint-cpp` target compiles our sources the same way) |

## 1. The rule the case exists to test

> An interrupt is not an instruction. It has no PC and no decode; it is offered
> only at a boundary the core chooses (`core_can_trap_i`), and where it is
> accepted is a function of the program's own state and the platform's level
> inputs — never of when the simulator's host happened to run. Once taken, `mepc`
> names the instruction to **re-execute** (the interrupted instruction has not
> executed), the enable CSRs take effect at the boundary their write retires at,
> a pending interrupt that is not yet enabled is not dropped, and a WFI ends only
> on an *enabled* pending interrupt.

The three ways that rule is easiest to lose — and the three controls (§7) — are:
a stimulus whose timing comes from somewhere other than the machine, an interrupt
treated as an ordinary retiring instruction, and a pending interrupt lost at a
boundary.

## 2. The program

One hand-assembled M-mode program at `0x80000000`, 326 words, built by the
driver from three seeded parameters (the window lengths) so that "same seed"
means something. It installs `mtvec`, clears `mstatus` and `mie`, and then runs
five windows:

| Window | Content | Purpose |
|---|---|---|
| P1 (53 nops) | the timer and external lines are asserted here while `mie = 0` **and** `mstatus.MIE = 0` | pending-but-disabled |
| — | `csrw mstatus, 8` (MIE=1, nothing enabled) then P2 (45 nops, `mie = 0`) | MIE set, no source enabled |
| — | `csrw mie, 0x80` (MTIE) at `0x800001a8`, immediately followed by one `nop` | **the enable CSR written in the immediately preceding instruction** |
| P3 (16 nops) | the software line is asserted here while `mie` holds no software bit | an unaccepted interrupt that must not be lost |
| — | `csrw mstatus, 0`; `csrw mie, 0x8` (MSIE); `csrw mstatus, 0x8`; `ecall` at `0x80000204` | mid-course: the software interrupt becomes enabled and unmasked exactly at the ecall |
| P4 (39 nops) | the external line is released here | a numbered-boundary deassert |
| — | `csrw mie, 0x80`; `csrw mstatus, 0`; `wfi` at `0x800002bc` | WFI with an enabled-but-unmasked timer |
| P5 (61 nops) | straight to the exit store at `0x800003b4` | nothing after the WFI touches the enables (§6) |

The handler is the architectural rule written out: it logs `{mcause, mepc}` from
inside the trap, and resumes by `mcause[63]` — `mepc + 4` for an exception,
`mepc` for an interrupt, because the interrupted instruction has not executed.
It is reached only through `mtvec`; the program is branchless, so the branch
predictor is never trained and holds no state across the replay.

## 3. The stimulus: asserted boundaries, recorded separately

Every stimulus is keyed on a **numbered retirement boundary**: the driver watches
the architectural retirement stream and fires an event when a *named instruction
of this program* retires. The boundary's number is the instruction's PC, and the
commit count at the instant the pins were applied is recorded with it. The one
exception is the WFI wake, and it is structural rather than a convenience: a hart
halted by WFI retires nothing, so no retirement boundary exists to key on; that
event is keyed on the halt's own duration, which is still a number produced by
the machine's state.

```
	asserted boundaries
	assert timer + external (masked)   retire:0x8000007c  boundary=0x020 cycle=52  -> soft=0 timer=1 ext=1
	assert software (masked)           retire:0x800001c0  boundary=0x084 cycle=219 -> soft=1 timer=0 ext=1
	release external                   retire:0x80000260  boundary=0x0d1 cycle=432 -> soft=0 timer=0 ext=0
	assert timer + external (WFI wake) halt>=3            boundary=0x0e9 cycle=464 -> soft=0 timer=1 ext=1
```

Timer and software are also released by the machine's own behaviour — the driver
drops a source at the cycle after the interrupt it drives is accepted, which is
what a level-sensitive CLINT request does in a program that clears the source.

## 4. What the run produced: accepted boundaries

```
	accepted boundaries
	boundary=0x06b cycle=131 cause=MTI(7)    mepc=0x800001ac head_pc=0x800001ac pre_retire=0x800001a8 retired_in_cycle=0 irq=1
	boundary=0x094 cycle=241 cause=MSI(3)    mepc=0x80000204 head_pc=0x80000204 pre_retire=0x80000200 retired_in_cycle=0 irq=1
	boundary=0x0a7 cycle=322 cause=ECALL(11) mepc=0x80000204 head_pc=0x80000204 pre_retire=0x80000514 retired_in_cycle=0 irq=0
```

* **timer (MTI)** — asserted at boundary `0x020` while `mie` and `mstatus.MIE`
  were both clear, accepted at boundary `0x06b`, and the retirement immediately
  before that boundary is `0x800001a8`, the `csrw mie`; its `mepc` is
  `0x800001ac`, the very next instruction. That is the card's *"an enable CSR
  written in the immediately preceding instruction"*, and the *"an unaccepted
  interrupt is not lost"*: the timer was pending with `mie[7] = 0` for 75 cycles
  and with `mip & mie ≠ 0` but `mstatus.MIE = 0` for 85 cycles, and was then
  taken at the first boundary the enable opened.
* **software (MSI)** — asserted at boundary `0x084` while no software bit was
  enabled, accepted at boundary `0x094`. Its `mepc` is the ecall's PC, and the
  ecall's own trap follows at boundary `0x0a7`: the two competed and *both* were
  delivered (see §5).
* **ecall** — the synchronous exception, cause 11, `mepc` = the ecall itself.
* Every interrupt trap cycle has `retired_in_cycle = 0`: nothing retires in the
  cycle an interrupt is accepted, because the interrupted instruction has not
  executed and retirement is suppressed for the trap decision.

Checked every cycle, on both runs: `o_commit_o` equals the number of retirement
events published; `o_irq_valid_o` is never high without `mstatus.MIE` and an
enabled pending bit; the interrupt unit's spurious-wake counter never moves; the
exception-payload generation never mismatches.

## 5. The competition, and one honest observation

The software interrupt is enabled (`mie = MSIE`) and unmasked (`mstatus.MIE = 1`)
in the instructions immediately before the `ecall`, and it is already pending.
The machine takes the **interrupt first** — at boundary `0x094`, with the ecall
as the head (`head_pc = 0x80000204`) — and then, because an interrupt's `mepc`
names the instruction to re-execute, the ecall runs, traps at boundary `0x0a7`
with `mepc` = the same address, and the handler resumes past it. Both are
delivered exactly once; the case asserts that (exactly one ecall trap, with the
interrupt's `mepc` naming it).

The mechanism is worth stating rather than hiding: `core_can_trap` excludes a
head that is a *staged* system macro (`sys_head`), and `sys_head` is
`sys_valid_q && rob_head_index == sys_index_q`. Only one macro is staged at a
time, so when the CSR macro in front of the ecall leaves staging on the cycle it
retires, there is one cycle in which the ecall is the head but its staging entry
has not been refilled: in that cycle `sys_head` is low, the boundary is open, and
a pending enabled interrupt is taken before the ecall. The architectural result
is unaffected — the same two traps with the same two `mepc` values are delivered,
in the other order — but the *order* is a property of the boundary rule, and the
case records it as it is rather than requiring a tidier one.

This also means the strict priority case (`trap_is_irq = !head_exc_trap && …`,
an exception already recorded on the head beating a pending interrupt) is **not
exercised** here; see §8.

## 6. The WFI wake, and why nothing follows it that touches the enables

The WFI at `0x800002bc` halts (an enabled pending interrupt would have kept it
running), the external line is asserted while it is halted and does **not** wake
it (its enable bit is read-only zero), and the timer asserted at `halt >= 3`
does: 6 completed halted cycles, no spurious wake, and no trap taken on the wake
because `mstatus.MIE` is clear.

Nothing after the WFI touches `mstatus` or `mie`. That is deliberate and is a
fact about this machine that the case had to accommodate: the halt gates *fetch*
and new allocation, not the instructions already in flight, so the instructions
behind the WFI retire while the hart is halted (the run observed four of them).
A program that cleared the timer's enable in the first instructions after its
WFI would clear it before the platform's level arrived and the wake would be lost
with the source still asserted. Leaving the enables alone is safe because
`mstatus.MIE` is clear, so no interrupt can be taken, and it is what a real WFI
sequence does.

## 7. Controls

`tools/run_irq_controls.py`. The card's three fail modes, each injected as exactly
one `-D`, each rebuilt from a deleted directory with its `-D` in that build's own
`build_command.txt`, each binary differing from the shipping one
(`d1d45936…`), each exiting 1 with the named first failure:

| Define | Layer | Fail mode | Binary sha256 | First named failure |
|---|---|---|---|---|
| `MOSAIC_IRQ_MUTANT_WALLCLOCK` | driver | 1. a stimulus timed by host wall time | `7009d3ecf83e778f0fbd5a2979d35451e895246cb6769bd2aede0c82703b9adf` | **the same stimulus replays to the same asserted boundary 0** (A boundary=39 cycle=59 host_tag=`0x…9efc3f`, B boundary=33 cycle=53 host_tag=`0x…b37dc1`) |
| `MOSAIC_IRQ_MUTANT_ORDINARY_RETIRE` | driver | 2. an interrupt treated as an ordinary retiring instruction | `e35e0e3d8b9066da355711d6e4cb2e852f481a06aaf61950c2b62cd87d34f961` | cycle 131: **an interrupt is not an instruction: no ordinary instruction retires in the interrupt's trap cycle** (cause MTI(7), retired_in_cycle=0) |
| `MOSAIC_IRQ_MUTANT_LOST_PENDING` | driver | 3. a lost unaccepted interrupt | `db7d579129679bc15296464ba4f9209f6f75bdec3cee5ef2f5542d66ac3c34a0` | **an unaccepted interrupt is not lost** (accept mepc=`0x800001ac`, pre-retire pc=`0x800001a8`) |

All three fail modes are statements about what *this case's comparison* expects,
so all three are `-CFLAGS -D` mutations of the driver, exactly as V-014's
checker-layer controls are:

* `WALLCLOCK` delays each event by a host-clock-derived number of cycles and
  records the host clock with each event and in the per-cycle trace. The two runs
  then reach different asserted boundaries (here 39 vs 33) with different host
  tags, and the replay comparison fails on the first asserted boundary. In the
  shipping build the tag is zero and the delay is zero, so nothing below changes.
* `ORDINARY_RETIRE` makes the checker require the interrupted instruction to
  appear in the retirement stream in the interrupt's trap cycle. A machine that
  correctly suppresses retirement in the trap cycle fails it immediately; the
  diagnostic prints `retired_in_cycle=0`, which is the machine's own evidence
  that it did *not* retire the interrupted instruction.
* `LOST_PENDING` makes the checker assert the fail mode's belief — that an
  interrupt asserted while masked is dropped at the boundary, so no acceptance
  follows its enable. The machine takes it, so the check fails while printing the
  exact `mepc` and preceding retirement PC it observed.

## 8. Not covered

* **The external interrupt is never taken** — and cannot be. `config/csr/mode_m.json`
  lists `mie`'s writable set as bits 7 and 3 and `mip`'s 15:8 as WPRI; the
  generated `MOSAIC_CSR_WMASK_MIP`/`MOSAIC_CSR_WMASK_MIE` are `0x88`, so the CSR
  file reads bit 11 as zero in **both** registers. The case drives `irq_ext_i`,
  asserts and releases it at numbered boundaries, and pins the consequence: no
  MEI trap, no wake, and `mip[11]`/`mie[11]` never set in the architectural view.
  The interrupt unit's internal `platform_meip` pending bit (`mosaic_interrupt.sv`)
  is therefore not architecturally reachable in p0 — worth recording, since the
  module's own comment describes bit 11 as a real pending source.
* **The exception-over-interrupt priority rule is not exercised.** In every run
  the interrupt is offered and taken as soon as the boundary opens, before a
  synchronous fault can be recorded on the head; §5 explains why (the head is
  excluded only once its exception is captured or it is a *staged* macro).
  Exercising `trap_is_irq = !head_exc_trap && …` would need a fault captured on
  the head before the first offerable boundary. The corresponding behaviour — an
  ecall interrupted before it executed, then taken once — *is* exercised.
* **Two interrupts pending and enabled at once is not exercised**, so the
  MEI > MSI > MTI priority order is not re-derived here; I-020
  (`interrupt.boundary_replay`) covers all eight `mie` and all eight source
  combinations and the priority order directly.
* **Software writes to `mip`** (a software-raised MTIP) are I-020's; this case
  drives only the three platform lines.
* **`mideleg` is zero in p0**, so the delegation gate is not exercised here.
* **Multi-hart / two CLINT contexts** is out of scope for p0 (one hart).
* **Saturation**: not a property of this package (it is the steering card's term).
  The nearest thing here is the interrupt unit's saturating counters, which I-020
  checks; this case keeps every count below the ceiling.
* **The WFI wake is keyed on the halt's duration, not on a retirement boundary**,
  for the structural reason given in §3 — stated so the deviation is not mistaken
  for an oversight.
