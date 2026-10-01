# I-020 — M-mode interrupt decision and the WFI boundary

Work package **I-020** of `docs/implementation-plan.md` (§5), implementing the
`CASE=interrupt.boundary_replay` obligation: sample interrupts at a legal
architectural boundary, define the WFI behaviour, and make the interrupt event
timeline **replayable to the same trap boundary**.

## STATUS: COMPLETE

| Acceptance item | Evidence |
| --- | --- |
| 1. Verilator `-Wall` clean | `verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_interrupt rtl/core/mosaic_pkg.sv rtl/core/mosaic_interrupt.sv` → exit 0 (this is the command `tools/lint_rtl.py` builds for this file: the same flags, the same `-I` set, the imported package listed first) |
| 2. slang clean | `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_interrupt.sv rtl/core/mosaic_pkg.sv` → exit 0 (2 style `WARN`s, the same `AlwaysCombBlockNamed`/`EnforcePortSuffix` categories every existing core module emits; the gate is exit status) |
| 3. C++ clean | `c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow … sim/unit/tb_interrupt.cpp` → exit 0 (no warning from this file; the only warnings come from Verilator's own `verilated_*.h`, which `make lint-cpp` also includes) |
| 4. `python3 tools/run_unit.py --profile p0 --case interrupt.boundary_replay` | `PASS interrupt.boundary_replay task=I-020`, exit 0, **17238 comparisons over 5784 cycles**, `result.json` `checks=1 failures=0` |
| 5. Discriminating coverage | every coverage counter asserted at the end of the run — see *Coverage* |
| 6. Mutants fail with exit 1, with a delta | all seven, each with a distinct named first failure and a comparison-count delta against the base — see *Mutants* |
| 7. This report | — |

The lint commands above are the **scoped** form of the project gates: Verilator
is invoked exactly as `tools/lint_rtl.py` invokes it for this file and slang
exactly as `make lint-slang` does for the two files involved. The project-wide
`make lint` / `make lint-slang` runs are the integration lead's, because sibling
lanes were editing other RTL files concurrently.

## Files

| File | Lines | Contents |
| --- | --- | --- |
| `rtl/core/mosaic_interrupt.sv` | 518 | `mosaic_interrupt`: source synchronisers, mip composition with the software latch, the priority + delegation decision, the WFI halt/resume state, the four counters |
| `sim/tb/mosaic_interrupt_tb.sv` | 98 | straight pass-through wrapper; no clock, no reset, no `$display` (the C++ driver owns all of them) |
| `sim/unit/tb_interrupt.cpp` | 1458 | independent C++ shadow, per-cycle two-snapshot comparison, eight phases, coverage |
| `results/reports/I-020-interrupt.md` | this file | — |

## The interface decision the card left to me

The card froze the source, CSR-view, decision and WFI ports, and said the
boundary state is mine to name ("the cleanest is an input `core_can_trap_i`").
This module adds exactly that one port:

```systemverilog
input  logic core_can_trap_i,   // the architectural boundary; integrator wires it
```

`irq_valid_o` is gated by it, and the testbench asserts as a **standing
invariant of every cycle of every phase** that

```
irq_valid_o  ==>  core_can_trap_i & mstatus.MIE & (mip_o & mie_i & ~mideleg_i) != 0
```

so "never outside the boundary" is a check on the DUT's own output rather than a
comment. The integrator's half of the contract — raising `core_can_trap_i` only
at a legal instruction boundary, and low while a trap is already being taken —
is *not* verified here (there is no core in this case); it is named in *Not
verified*.

Other decisions, and why:

* **Priority MEI (11) > MSI (3) > MTI (7)**, the privileged spec's "decreasing
  priority order: MEI, MSI, MTI, SEI, SSI, STI" (Machine-Level ISA, *Machine
  Interrupt Registers*). It is implemented as a priority chain, not as the
  numerically highest cause, and the source-combination phase checks all eight
  source combinations against that order.
* **`mideleg_i` gates the taken condition, not the pending bit.** A delegated
  interrupt is still pending; it is simply not this module's trap to take. The
  gate is implemented even though `config_check` pins mideleg to 0 for p0, and
  the test drives it directly to prove the gate is real.
* **`irq_cause_o` is the winning candidate, independent of `mstatus.MIE` and of
  `core_can_trap_i`.** Those two gates decide whether a trap may be taken *now*
  (`irq_valid_o`); the cause stays stable across a blocked window, so a core that
  latches the cause when the boundary opens cannot latch a value the wait
  invalidated. It is zero only when no enabled, non-delegated pending bit exists.
* **`mip_o` = synchronised platform request OR software latch**, on precisely the
  bits `config/csr/mode_m.json` declares writable (`mip.writable_fields = 7, 3`);
  everything else reads zero. The mask is a documented `localparam`
  (`MIP_WRITABLE_MASK`, `0x…0088`) because the manifest generator emits geometry,
  not CSR field legality — the coupling between that mask and the CSR field table
  is flagged in *Reported, not fixed*.
* **WFI wake condition is `mie_i & mip != 0`** — no `mstatus.MIE` term (the
  privileged spec's rule), and no delegation term (delegation decides which mode
  takes the resulting trap, not whether the wait ends). A WFI whose interrupt is
  already enabled and pending does not halt: `wfi_halt_o` never rises.
* **Four saturating 8-bit counters.** A wrapping byte counter turns a large count
  into a plausible small one; saturation is stated and the shadow saturates
  identically. `o_irq_ctr` reaches 1471 offered cycles in the shipping run, so
  the counters cross 255 and the saturation rule is compared on every one of
  those cycles.
* **Metastability model (stated because the card asks).** Discrete, not analog:
  the two-flop synchroniser samples at rising edges; a pulse that spans no edge
  is never observed, a pulse that spans exactly one edge appears two edges later,
  and a level clears two edges after it drops. The glitch phase drives a
  sub-cycle pulse *while the clock is low* and checks the pending bit does not
  move at that instant — the moment a missing synchroniser is combinationally
  visible — and the latency phases then assert the exact cycle at which a real
  pulse becomes visible.

`mosaic_pkg` is pulled in with an explicit `` `include "mosaic_pkg.sv" `` inside
the module, because the frozen interface uses `mosaic_pkg::csr_op_e` while
`CASE=interrupt.boundary_replay` lists only `rtl/core/mosaic_interrupt.sv` as a
source. The include is guard-safe (`mosaic_pkg.sv` has `` `ifndef MOSAIC_PKG_SV_ ``)
and verified on all three paths: the unit runner (package pulled in by the
include), Verilator lint with the package listed first *and* included (no
duplicate definition), and `slang --single-unit`, which sorts
`mosaic_interrupt.sv` before `mosaic_pkg.sv` — the include makes file order
irrelevant. The cleaner fix is a one-line registry change; see *Reported, not
fixed*.

## What the testbench checks

The DUT is never its own oracle. `ShadowInterrupt` re-implements the contract in
C++ — the two-flop sampling rule, the OR with the software latch, the priority
chain, the delegation gate, the wake rule, the halt FSM and the saturating
counters — and is compared every cycle. Two-snapshot discipline is observed
strictly: combinational offers (`mip`, `irq_valid`, `irq_cause`, the pending
flags) are compared against the shadow computed from the pre-edge state, and the
registered state (halt, four counters) after the edge; the combinational outputs
are additionally compared post-edge against the post-edge shadow, which is
strictly tighter than either snapshot alone. A reset touched the shadow too, so
the two models never describe different machines.

Eight phases, each of which can fail on its own:

| Phase | What it pins down |
| --- | --- |
| `reset-state` | the cold state; a source held through reset leaves nothing behind |
| `sync-and-glitch` | sub-cycle pulses are never observed; a one-cycle pulse is visible for exactly one cycle, exactly two edges later; a held level clears exactly two edges after it drops; each source bit is independent |
| `masking` | all 8 `mie` combinations, all 8 source combinations against the priority order, `mstatus.MIE` masking and un-masking with the same pending set, and the `mideleg` gate (delegated MEI not taken while undelegated MTI is) |
| `boundary-latency` | `core_can_trap_i` low for a randomised 1–64 cycle interval: no offer inside the window, pending preserved, then exactly one offer with the right cause at the first legal boundary; a second source arriving mid-window changes the winner as priority requires |
| `halt-resume` | WFI halts and `o_halt_cycles` counts exactly the completed halted cycles; a late enabled source wakes exactly once, in the cycle the pending bit becomes visible; an already-pending enabled interrupt prevents the halt entirely; a pending-but-disabled interrupt does **not** wake the core, and enabling that same bit then does, exactly once; `mstatus.MIE=0` does not block the wake; repeated WFI pulses are not extra wakes |
| `mip-software-write` | CSR_RW/RS/RC semantics on bits 7 and 3, a write to a read-only bit ignored, `mip_we_i=0` and `CSR_NONE` writing nothing, platform request not clearable by software, and WFI over a disabled software-raised MTIP woken by the enable |
| `replay` | the card's central claim — see below |
| `random` | 4000 cycles of randomised sources, enables, delegation, global enable, boundary, WFI pulses and mip writes, compared every cycle |

**Replay.** A 600-cycle scripted timeline is built from the seed and run twice.
For each run the test records, relative to the run start, every *rising edge* of
`irq_valid_o` as `(cycle, cause)`, every halt entry, every wake cycle, a FNV-1a
hash of the full per-cycle observable tuple, and the final counters. The two runs
are then compared **event by event** — boundary count, then each boundary's cycle
and each boundary's cause by index, then the wake and halt cycle lists, then the
trace hash and the counters. The script is required to produce at least three trap
boundaries, at least one halt and at least one wake, so a script that stopped
driving the mechanisms cannot pass by producing two empty traces that agree.

## Coverage

Asserted at the end of the run, so a phase that quietly stopped exercising its
mechanism fails the case: offered traps 1471 (MEI 242 / MSI 703 / MTI 526 — all
three causes taken), masked pending cycles, boundary-low cycles, halt entries,
wakes, glitches, software mip writes, "halted while a pending interrupt was
disabled" cycles, `o_spurious_wake_ctr == 0` on the DUT, and the DUT's four
counters equal to the shadow's.

Sample robustness (the registered seed is 1; these were run as a check that the
case is not seed-tuned, not as alternative acceptance evidence): seeds
2, 7, 12345, 999983 all `PASS`, exit 0, 1377–1626 offered traps each.

## Mutants

`-DMOSAIC_INTERRUPT_MUTANT_<n>` selects one defect; the shipping build defines
none. Every row below was produced with the case's build directory **deleted
first**, so no object from a previous build could be involved, and the SHA-256 of
the generated model (`obj_dir/Vmosaic_interrupt_tb*.cpp`) was compared with the
clean build's — **every mutant elaborates different hardware**, which is the
direct answer to "a mutant whose `ifdef` body does not exist compiles the
shipping build and proves nothing".

Base: `PASS`, exit 0, **failure count 0**, model `b54cf9434d46c64e`, 17238
comparisons. (The base was rebuilt and re-run after the campaign and produced the
same model hash and the same PASS, so the table is not standing on a stale
binary. Two comment-only edits to the RTL were made after the campaign — the
header's cause wording and the WFI monitor's wording — and the rebuilt model hash
is still `b54cf9434d46c64e`, i.e. the elaborated hardware is unchanged and this
table describes exactly the shipping RTL.)

| Mutant | Δ failures | Δ comparisons vs base | exit | First failure (cycle) |
| --- | --- | --- | --- | --- |
| `NO_SYNC` (second flop bypassed) | +1 | 17238 → **58** | 1 | `sync-and-glitch: cycle 30 post-edge mip: expected 0x0, got 0x80` |
| `PRIORITY_REVERSED` (MTI first) | +1 | 17238 → **127** | 1 | `masking: cycle 57 post-edge irq_cause: expected 0x8000…0b, got 0x8000…07` |
| `IGNORE_MIDELEG` (gate dropped) | +1 | 17238 → **297** | 1 | `masking: cycle 113 pre-edge irq_valid: expected 0, got 1` |
| `MIE_IGNORED` (global enable dropped) | +1 | 17238 → **264** | 1 | `masking: cycle 102 pre-edge irq_valid: expected 0, got 1` |
| `IRQ_OUTSIDE_BOUNDARY` (`core_can_trap_i` dropped) | +1 | 17238 → **339** | 1 | `boundary-latency: cycle 131 pre-edge irq_valid: expected 0, got 1` |
| `WFI_WAKE_DISABLED` (halt cleared on any pending bit) | +1 | 17238 → **1435** | 1 | `halt-resume: cycle 501 o_spurious_wake_ctr: a halt ended without an enabled pending interrupt (DUT count 1)` |
| `IGNORE_SW_WRITE` (software latch never updates) | +1 | 17238 → **1531** | 1 | `mip-software-write: cycle 537 post-edge mip: expected 0x80, got 0x0` |

The base's failure count is 0 rather than red, so the failure delta is 0 → 1 in
every row; the comparison-count column is the quantitative part of the delta and
is the column to read, per the standing rule that an exit code alone is not
evidence.

Two notes on the mutants that are evidence about the *checks*, not just the DUT:

* `WFI_WAKE_DISABLED` fails on the **DUT's own `o_spurious_wake_ctr`**, which the
  harness checks before anything else in the cycle. That counter is therefore a
  detector with a demonstrated trigger, not a statistic that is compared and
  never used. In the shipping build the counter is structurally unreachable — the
  only path that clears the halt is the architectural wake rule, and the monitor
  re-derives that rule rather than reading the clear path's own condition.
* `NO_SYNC` fails in the glitch phase, at the mid-cycle assertion, i.e. at the
  instant the raw asynchronous source becomes combinationally visible — which is
  the discrete model of "the sampler was removed", not an incidental timing shift.

## Honest record of the two disagreements hit while writing this

1. The harness compared the **stale** pre-edge snapshot after the edge inside
   `Glitch()`, so the shadow (advanced) was compared against a snapshot taken
   before the glitch was dropped. The **testbench** was wrong — it mixed the two
   snapshots the project's rule warns about. Fixed by making the comparison take
   the snapshot it is given, and by re-reading both snapshots explicitly.
2. The masking phase asserted `irq_cause_o == 0` whenever no trap was offered,
   including while `mstatus.MIE=0`. The **testbench** was wrong: the contract is
   that the cause is the winning candidate and the gates are `irq_valid_o`. The
   RTL header was ambiguous about this, so the ambiguity was fixed in the header
   (the cause is a candidate report, stable across a blocked window) and the test
   now asserts the candidate's value while masked and again when it is offered.

No case was made green by weakening a comparison, shortening a phase, or tuning a
seed. The registered seed 1 passes, and so do four other seeds.

## What is NOT verified

* **The integrator's half of the boundary contract.** This case proves the module
  never offers a trap outside `core_can_trap_i`; it cannot prove the core raises
  it only at a legal boundary or that it is low while a trap is being taken.
* **The CSR wiring.** `mip_we_i`/`mip_op_i`/`mip_wdata_i` and `mie_i`/`mideleg_i`
  are driven by the testbench. That the CSR file forwards the mip write on the
  same edge, and that `mip_o` is what the mip CSR read returns, are integration
  obligations not exercised here.
* **Delegation beyond bits 3/7/11** and any S-mode behaviour: p0 is M-only and
  `mideleg` above those bits is never driven.
* **Metastability in the analog sense.** No claim is made or tested; the test
  asserts the discrete sampling model the RTL documents.
* **Synthesis.** No Yosys run includes this file (`rtl/core/filelist.f` is a
  hand-maintained list that already omits several newer modules). The RTL uses
  only `always_ff`/`always_comb`, static bounds and no `initial`, but that is read
  from the source, not observed from a synthesis run.
* **The counter saturation path as a directed phase.** Saturation is exercised
  incidentally (1471 offered cycles > 255) and compared against the shadow, but no
  phase drives a counter to 255 and back.
* **A formal claim.** There is no SVA or property file for this module.

## Reported, not fixed

1. **Registry entry is incomplete for the frozen interface.**
   `tests/unit/registry.json` lists `rtl/core/mosaic_interrupt.sv` alone, but the
   frozen interface uses `mosaic_pkg::csr_op_e`. `tools/run_unit.py` compiles
   exactly the listed sources, so the module must pull the package in itself (it
   does, with a guard-safe `` `include ``). The cleaner change — one line, owned
   by the integrator — is to add `rtl/core/mosaic_pkg.sv` to the entry, as
   `core.bringup_vs_reference` and `retire.head_block_and_dual` do.
2. **`config/csr/mode_m.json` makes the external interrupt unreachable through
   the CSR view.** Both `mip` and `mie` there have `writable_fields: ["7","3"]`
   and `wpri_fields` including `"15:8"`, which covers bit 11 — MEIP/MEIE. So a
   CSR read of mip can never show MEIP and a CSR write to mie can never enable
   MEIE, while this module's frozen interface carries both and the case drives
   them at the port. Either the config should declare bits 7/3/11 writable
   (`wpri_fields` split so that 11 is not WPRI), or the interface's `irq_ext_i`
   path is dead in p0 and should say so. Not edited: `config/` is not mine.
3. **`rtl/core/filelist.f` is stale** — it lists 10 of the existing modules. It is
   not mine to police, and adding only my file would leave it just as
   inconsistent, so this module is not added to it.
4. **A stale-object build hazard was observed once.** After a sequence of mutant
   builds, a clean rebuild of this case produced a binary that still exhibited the
   mutant behaviour while the generated model file on disk was the clean one;
   only deleting `build/p0/unit/interrupt.boundary_replay/` cleared it. A later
   four-step controlled reproduction did **not** reproduce it, so it is
   intermittent (most plausibly Verilator's `--skip-identical` / make timestamp
   interaction when the only change is a `-D` flag). The mutant table above was
   therefore produced with the build directory deleted before every build, and
   the base was re-verified after the campaign. Anyone comparing a mutant against
   a base should do the same rather than trusting an incremental rebuild.
