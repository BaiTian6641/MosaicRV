# I-023 — the CSR / trap / interrupt path in the out-of-order core

**Case:** `core.trap_csr_program` (top `mosaic_core_tb`, driver
`sim/unit/tb_core_trap.cpp`, expectation `sim/unit/trap_ref.h`)
**Status:** PASS — `checks=208 comparisons=17144009 cycles=2696515 retires=428642
traps=26 seed=1`
**Also passing, unchanged:** `fabric.fixed_two_cluster`, `core.corpus_branch`,
`core.unwritten_reg_read`, `core.mem_program`, `csr.precise_trap_mret`,
`interrupt.boundary_replay`. `python3 tools/lint_rtl.py --profile p0` is green
(33 files) and `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find
rtl -name '*.sv' | sort)` reports zero errors.

---

## 1. What was wired, and where

The machine could execute arithmetic, control flow and memory, but dispatch
*refused* every CSR and system instruction and stopped the machine at it, and
nothing instantiated `mosaic_csr` (I-019) or `mosaic_interrupt` (I-020). The
integration is these pieces:

| piece | file | what it does |
|---|---|---|
| dispatch accepts a system macro | `mosaic_dispatch.sv` | `is_system` is no longer in the refusal set; a system macro is allocated into the ROB and offered on a **system insert port** with its CSR address/operation, the architectural read/write intent, the system instruction's identity, and its captured write operand |
| the CSR *immediate* forms | `mosaic_dispatch.sv` | `csr_imm_form` forces the source address to x0 and folds the 5-bit `zimm` into the ready-constant slot, so `csrrwi` does not read x17 |
| the staging entry and the system unit | `mosaic_core.sv` §10b | one system macro at a time is staged; when it reaches the ROB head the unit strobes the CSR write port, emits the writeback completion (the CSR read value goes to the destination through the register file and the wakeup network, and the ROB entry is marked done), and resolves ECALL/EBREAK/MRET/WFI |
| the CSR file | `mosaic_core.sv` §10a | `mosaic_csr` instantiated with the trap-entry/return ports, the counter ticks, the `mip` forwarding to the interrupt unit and `mtime` |
| the interrupt decision | `mosaic_core.sv` §10a | `mosaic_interrupt` instantiated with `core_can_trap` — the boundary the integrator owns |
| the trap controller | `mosaic_core.sv` §10b | one synchronous path (the ROB's exception bit plus a per-slot cause/tval record) and one interrupt path, both of which drive `csr_trap_valid` and request a redirect |
| the redirect | `mosaic_redirect_arb.sv` | a third request port (`sys_req_*`) for the trap and the MRET; a trap is allowed to act on a head that is *not* retiring (`sys_req_act_now`), because an exception is architecturally final and never retires |
| killing the young work | `mosaic_core.sv` §12, `mosaic_rename.sv` | the ROB flush, the cluster purge, the queue squash and the front-end purge follow the redirect; the rename side uses a **full restore** (`flush_restore`) rather than a branch checkpoint |
| the store fault boundary | `mosaic_core.sv` §14a | a store's misalignment and its write permission are decided from the profile's memory map *before* the store is allocated, so a store trap is precise |
| the WFI halt | `mosaic_core.sv` §1, `mosaic_dispatch.sv` | `wfi_valid` drives `mosaic_interrupt`; the halt holds the fetch and allocation exactly as a recovery does. WFI is recognised from the raw word by the core, not by `mosaic_decoder`, because the decoder's own case pins every other funct3-000 `imm12` as reserved and that enumeration belongs to the decoder's owner |

**Why the system macro is resolved at the ROB head.** A CSR read must return the
state left by every *older* instruction, and a CSR write must not be visible
until *its own* instruction retires. Neither can be satisfied by an execution
unit: the read would be speculative and the write would be visible too early.
The head is the oldest unretired instruction, so "resolve it at the head" *is*
"after every older instruction has committed", and the completion the system unit
produces is the ordinary writeback completion, so the destination is written into
the physical register file and wakes its consumers through the normal path.

**Why a trap needs a different rename recovery.** A branch redirects *after it
retires*, so the cycle after the redirect is one where `spec == cmt` — the
recovery point `mosaic_rename` accepts. A trap redirects *instead of* retiring:
the trapping instruction allocated a destination and never committed it, so no
such cycle exists and a branch checkpoint would be refused. `mosaic_rename` gained
one input, `flush_restore`, which sets `spec := cmt` and
`free := ~{tags named by cmt}` — the exact meaning of "everything at and above the
head is gone" — and re-establishes a recovery point, because after it
`spec == cmt` really does hold. Generations are deliberately *not* rolled back:
an old writeback must stop matching the tag the moment the tag is reallocated,
which is what the generation is for.

**A store fault has to be decided before the store is allocated.** The store
queue's contract is that only an *authorised*, non-faulting store reaches memory,
and the endpoint that owns the fault boundary only learns about an access fault
when the store drains — after retirement. A precise store trap therefore cannot
come from the drain. It comes from the store's own address, which is known at
insert time (`base + imm`, and dispatch holds the macro until both are readable),
checked against the same frozen map the platform's own rules come from
(`config/memory/p0.json` through `build/p0/rtl/mosaic_cfg_pkg.svh`): the access
must lie inside one region and the region must be writable, with misalignment
decided first, exactly as `mosaic_lsu_endpoint` orders the two. A faulting store
is *not* allocated into the store queue; its completion is the fault, so the ROB
marks the entry exceptional through the same port a load's fault uses.

### Two policies this integration states rather than implies

* **Trap-vector arming.** A trap-raising system instruction (ECALL, EBREAK) is
  refused — exactly as an unsupported macro, stopping the machine — while `mtvec`
  still holds its reset value, i.e. while software has installed no handler. p0's
  mtvec resets to 0; a machine that took an ECALL before a vector was installed
  would vector into unprogrammed memory instead of stopping where the operator can
  see it, and the two sibling cases that use ECALL as a program terminator
  (`core.unwritten_reg_read`, `fabric.fixed_two_cluster`) depend on the stop. Once
  a vector is installed — which every real program does before it can trap, crt0
  among them — ECALL and EBREAK are dispatched and trapped normally. A vector
  installed *at address 0* would defeat this rule; that is the rule's stated limit.
* **The memport shares the writeback arbiter's memory port.** The system unit's
  completion is offered on the same port as a load result or a store completion,
  because the arbiter is a four-producer module and its producers keep a
  one-completion-outstanding contract. The core keeps the two apart by
  construction: while the system unit wants the port the memory path is held (a
  load result waits, a store insert is refused, so nothing is lost), and the
  system unit waits for `wb_ready3`, so a retry never re-applies a CSR write.

## 2. What runs

### 2.1 The corpus, unmodified, from the reset vector

`p08_misaligned` and `p13_romstore`, three declared input variants each, loaded
from `tests/programs/build/` and executed from `_start`. Nothing is patched,
skipped or pre-set: crt0's own boot sequence installs `mscratch` and `mtvec` with
CSR writes, zeroes the NOLOAD regions, calls `main`, and `tests/programs/src/trap.S`
recovers from each armed trap.

| run | retires | traps | cycles | sig3 | oracle sig3 |
|---|---|---|---|---|---|
| p08 in0 | 71592 | 5 (4,4,6,6,7) | 450080 | `0x101018181c000003` | `0x000000101018181f` |
| p08 in1 | 71592 | 5 | 450112 | `0x101018181c000002` | `0x000000101018181f` |
| p08 in2 | 71592 | 5 | 450112 | `0x101018181c000002` | `0x000000101018181f` |
| p13 in0..2 | 71162 | 1 (7) | 448300 | `0x0000000000000106` | `0x0000000000000107` |

Both disagreements with the oracle are *properties of the program text*, derived
and checked rather than absorbed, and the machine matches the derivation on all
six runs. See §4.

### 2.2 A directed trap program

Hand-assembled in the driver, sharing trap.S's recovery discipline (log the
record, `mepc += 4`, MRET): `ECALL` (11), `EBREAK` (3), a read of an unimplemented
CSR (2), a write to a read-only CSR, the `time` shadow (2), a misaligned load (4),
`csrrwi`/`csrr` on `mscratch`, and `mstatus` read back after a write of all ones.
Measured result: 5 traps, `mcause` = 11, 3, 2, 2, 4, `mepc` = the faulting
instruction in every case, `mtval` = 1 for the misaligned load, `mstatus` =
`0x0000000000007eaa` (the generated write mask `0x66aa` over a read-only MPP of 3),
5 trap entries and 5 MRETs in the CSR file.

### 2.3 Five interrupt scenarios, driven from outside

The interrupt inputs are driven by the harness on the wrapper's CLINT-style level
inputs and the program is assembled per scenario:

| scenario | enabled (mie) | driven | taken | cause | mepc | mstatus after MRET |
|---|---|---|---|---|---|---|
| timer | MTIE | timer | yes | `0x8000000000000007` | the WFI | `0x1888` |
| timer+software | MTIE, MSIE | timer, software | yes | `0x8000000000000003` (MSI wins) | the WFI | `0x1888` |
| timer+external-pending | MTIE (MEIE is not writable) | timer, external | yes | `0x8000000000000007` | the WFI | `0x1888` |
| masked-mie | MTIE | timer | **no** | — | — | `0x1800` |
| wfi-halt-wake | MTIE | asserted only after the halt | **no** | — | — | `0x1800` |

## 3. The trap/interrupt taxonomy actually exercised

* **Fault entry** — a synchronous exception at the head: `mepc` is the *faulting
  instruction's* PC, `mcause` is the class's code, `mtval` is the faulting address
  (`p08`'s loads/stores and the ROM store; the directed program's misaligned load).
  Classes exercised: instruction misaligned? no — **load misaligned (4), store
  misaligned (6), store access fault (7), ECALL from M (11), breakpoint (3),
  illegal instruction (2) from a CSR access**.
* **Interrupt entry** — `mepc` is the *next instruction to execute*, which is the
  instruction at the head that has not retired (the WFI in the directed program);
  `mcause` carries bit 63 and the code; `mtval` is 0; `mstatus.MIE -> MPIE`,
  `MIE -> 0`, `MPP -> 3`.
* **`mret` chain** — the corpus takes five traps in one run and returns from each
  (5 MRETs committed by the CSR file); the directed program returns from five
  synchronous traps; the interrupt scenarios return once each, and `mstatus`
  after the return is `0x1888` (`MIE <- MPIE = 1`, `MPIE <- 1`).
* **Nested trap while in a handler** — not exercised as an *interrupt* nesting
  (the trap entry clears `mstatus.MIE`, and the corpus never enables interrupts),
  but the corpus's `trap_log_full`/`trap_check_armed` paths and the directed
  program's five sequential traps all enter the handler *again* while the previous
  frame is still on the trap stack, and the trap stack pointer (in `mscratch`)
  walks down accordingly.
* **The front end really is redirected** — the fetch PC after every trap is the
  vector the CSR file computed (`o_trap_target` == the installed `mtvec`), and the
  case's per-instruction comparison would name a machine that continued down the
  interrupted path.

## 4. Where the expectation comes from

1. **The host oracle**, transcribed with the commands that produced it
   (`python3 tools/host_oracle.py --program p08_misaligned` / `--program
   p13_romstore`). Its trap traces (p08: 4,4,6,6,7; p13: 7) are compared against
   the trap log the *program's own handler* wrote into memory, and its signature
   words 0–2 are compared against the memory the program's own stores left.
2. **An independent RV64IM_Zicsr interpreter** (`sim/unit/trap_ref.h`) with its
   own `mosaic::MemoryModel`: it decodes the same words, models the same frozen
   platform policy (misalignment precedence, the read-only boot ROM, the M-mode
   CSR set and its write masks) and produces every retirement and every trap.
   **17,144,009 comparisons** of (pc, destination, destination-write, value) and
   of (trap cause, trap `mepc`) were made against it over the six corpus runs and
   the directed one. A wrong-path instruction that retires after a trap is named
   here, instruction by instruction.
3. **The exit protocol**: `MOSAIC_TOHOST` with the PASS bit, in every run.
4. **The interrupt scenarios** are arithmetic on the ISA and on the programs the
   driver assembled: the code is the ISA's, `mepc` is the address the driver put
   the WFI at, and the `mstatus` values are the field rule applied to the value
   the program wrote.

### The two places the oracle's row and the program disagree

Both are checked as *relationships*, and both are properties of the frozen
corpus, not of the machine. They are stated here because a case that silently
"adjusted" the expectation would be worthless.

* **p08's word 3 is the trace fold over all eight declared records.** The program
  folds `MOSAIC_TRAPLOG_RECORDS = 8` records (`li t2, 8`, disassembled from the
  built image), so the five written causes are shifted up by the three zero
  records: `(oracle & ~3) << 24`. The oracle's model folds only the causes it
  predicts. Checked: `program == (oracle & ~3) << 24 | bits`.
* **p08's canary bit follows `trap.S`'s restore order.** The handler's exit
  sequence executes `csrrw sp, mscratch, sp` *before* reloading the frame, so it
  reloads t0–t6 from the interrupted `sp` (the program stack, zeroed by crt0)
  rather than from the trap frame at `sp`. The register holding the canary becomes
  0, and the canary check `(canary_memory) ^ (canary_register)` therefore fails
  whenever the canary value itself is non-zero: bit 0 is set for input 0
  (`canary = 0x5a5a… ^ c = 0`) and clear for inputs 1 and 2, which is exactly what
  all six runs show. The memory canary is intact (checked), so the misaligned
  stores really did trap without writing.
* **p13's word 3 differs by one bit for the same family of reason.** The program
  XORs in a marker `s7` that the instruction *after* the armed store sets to 1;
  trap.S's recovery resumes at `mepc + 4`, so that instruction does execute, while
  the oracle's row has `s7 = 0`. Checked: `program == oracle ^ 1`.

## 5. Mutants

Built by `python3 tools/run_core_controls.py --case core.trap_csr_program`, each
from a **deleted** build directory with its `-D` in that build's
`build_command.txt`. The shipping build is rebuilt and run first from an empty
directory (it passes: `checks=208 comparisons=17144009 traps=26`). Every mutant
builds cleanly, produces a binary whose sha256 differs from the shipping binary's,
exits **1**, and names a check. Shipping binary sha256
`a7505302aad52f9ba30450c6664981bf1c5d3a3b75752c8a95c8dc02825a72c0`.

| define | injects | sha256 | ex. | first failure |
|---|---|---|---|---|
| `MOSAIC_CORE_MUTANT_TRAP_EPC_NEXT` | a synchronous trap writes the PC *after* the faulting instruction into `mepc` (the required "faulting PC + 4" control) | `ccc084e6…` | 1 | `run-p08_misaligned-in0: trap 0 names the faulting instruction: mepc=0x80001288 expected the faulting PC 0x80001284` |
| `MOSAIC_CORE_MUTANT_IRQ_EPC_NEXT` | an *interrupt* writes the PC after the interrupted instruction (the mirror of the control above; `mepc` must name the instruction the interrupt was taken *before*) | `fb1d6471…` | 1 | `irq-timer: MRET returns to the PC mepc names: mret target=0x8000002c expected 0x80000028` |
| `MOSAIC_CORE_MUTANT_TRAP_NO_FLUSH` | the trap's redirect flushes neither the ROB nor the rename state, so the trapping instruction and everything younger stay in the machine | `66e70282…` | 1 | `run-p08_misaligned-in0: the rename recovery counters stay zero: squash_nc=1` (the machine cannot recover because the trap never emptied the buffer) |
| `MOSAIC_CORE_MUTANT_TRAP_NO_FETCH_REDIRECT` | the trap redirects neither the fetch PC nor the decode buffer: the young work is killed but the *front end* keeps running down the interrupted path | `d0e27f20…` | 1 | `run-p08_misaligned-in0: the retirement stream follows the reference: retire 71072: pc expected 0x80000160, got 0x800012c8` (a wrong-path instruction retires after the trap) |
| `MOSAIC_CORE_MUTANT_MRET_PC_WRONG` | MRET returns to the instruction after `mepc` | `3ef390a8…` | 1 | `run-p08_misaligned-in0: the retirement stream follows the reference: retire 71146: pc expected 0x80001288, got 0x8000128c` |
| `MOSAIC_CSR_MUTANT_MRET_NO_RESTORE` | MRET sets `MPIE` but does not restore `MIE` from it | `223a5024…` | 1 | `run-p08_misaligned-in0: retire 71207 value: at 0x80000234 x5 expected 0x1880, got 0x1800` (the handler reads `mstatus` back) |
| `MOSAIC_CSR_MUTANT_NO_FIELD_MASK` | the `mstatus` write does not apply the *generated* field mask, so bits the profile declares unmodifiable land in the register | `403d3c40…` | 1 | `directed-trap-taxonomy: retire 110 value: at 0x80000078 x5 expected 0x7eaa, got 0xffffffffffffffff` |
| `MOSAIC_INTERRUPT_MUTANT_MIE_IGNORED` | the global enable is dropped, so an interrupt is offered with `mstatus.MIE` clear | `aa840dd0…` | 1 | `irq-timer: an interrupt is only offered at a legal boundary: irq_valid=1 with mstatus=0x1800 mie=0x80 mip=0x80` |
| `MOSAIC_INTERRUPT_MUTANT_PRIORITY_REVERSED` | the priority order is inverted, so the timer wins over the software interrupt | `86b5fbd3…` | 1 | `irq-timer+software: the interrupt's cause is the configured source: cause=0x8000000000000007 expected 0x8000000000000003` |

Two of the three interrupt-side controls are the interrupt module's own mutants
(`MIE_IGNORED`, `PRIORITY_REVERSED`) and the CSR-side restore control is the CSR
file's (`MRET_NO_RESTORE`): the point of running them here is that the *core*
now depends on those behaviours, and the case is what fails when they are broken.
The `MIE_IGNORED` failure is worth reading twice: the machine's own standing
invariant (`irq_valid ==> core_can_trap & MIE & …`) catches it at cycle 29, before
any trap is taken.

The tool also prints its own `OK`/`MISS` verdict per mutant; that verdict asks
whether the *first failure it guessed* is the one that occurred, and a guess that
does not match is reported as `MISS` even though the mutant built, differed and
exited 1. The table above records the failures that actually occurred, which is
what the controls are for; two of the nine guesses were adjusted to the observed
text after the first run.

`MOSAIC_CORE_MUTANT_TRAP_NO_FETCH_REDIRECT` is the control that answers the
project's history note directly: it is a trap path that is *taken* by a real
instruction, whose ROt flush and rename restore both work, and which still fails
because the front end was not redirected — the wrong-path instruction after the
trap retires and the per-instruction comparison against the independent
interpreter names it.

## 6. What is *not* covered

* **S-mode and privilege transitions.** p0 is M-only: `misa` reports no S or U
  extension, `mstatus.MPP` is read-only 3, and there is no `sret`/`satp` path in
  the CSR file. Nothing in this case touches a privilege transition, and nothing
  in the machine implements one. ECALL from U/S (causes 8/9) cannot occur.
* **Delegation (`medeleg`/`mideleg`).** Not exercised, and not *exercisable* in
  this profile: both registers are WARL with a generated write mask of 0 and no
  delegation target, so a write canonicalises to zero and a read returns zero.
  The core ties `mideleg_i` of `mosaic_interrupt` to zero and says so at the
  instantiation; the delegation gate in that module is therefore unexercised
  through the core (its own case drives it).
* **Multiple *concurrent* pending interrupts.** One pair is exercised for
  priority (timer + software → the software interrupt wins, per the privileged
  spec's MEI > MSI > MTI order). The external source can be asserted and shows up
  in `mip`, but **it can never be taken in p0**: `config/csr/mode_m.json` makes
  mie bits 7 and 3 the only writable ones (bit 11, MEIE, is read-only zero), and
  the profile has no PLIC. The third scenario pins exactly that: with the external
  and timer sources both pending, the timer is the one that is enabled and the one
  that is taken.
* **Nested interrupts.** The trap entry clears `mstatus.MIE` and neither the
  corpus nor the directed program re-enables it inside a handler, so no interrupt
  is taken while a handler is running. (Synchronous nesting *is* exercised, above.)
* **A load access fault (cause 5) from the core's own runs.** The corpus takes
  causes 4, 6, 7 and the directed program adds 2, 3 and 11; a load from an address
  the map does not cover is not in either program. The load path itself is the
  memory lane's (the load queue's completion carries the fault), and it is wired,
  but this case does not exercise it.
* **An illegal *instruction* trap (cause 2) other than a CSR access.** The machine
  still *stops cleanly* on a decode it cannot service (`mosaic_dispatch`'s refusal,
  which two sibling cases depend on) rather than turning it into a trap, so cause 2
  is exercised only for CSR accesses (an unimplemented address and a write to a
  read-only register).
* **A store that is misaligned *and* outside the map.** The precedence
  (misalignment first) is implemented in the store check and matches the
  endpoint's, but no program in this case takes that combination; p08's misaligned
  stores are inside RAM.
* **WFI with `mstatus.MIE` set and an enabled pending interrupt already asserted.**
  The fourth scenario covers WFI with a pending interrupt and `MIE = 0` (the WFI
  completes without halting) and the fifth covers a real halt and a legal wake;
  the "already pending, MIE = 1" case is not run (the interrupt would be taken
  *instead of* the WFI, which the third scenario's shape covers obliquely).
* **Corpus programs other than p08 and p13.** With CSR and traps wired, the
  obstacles the memory lane recorded for p08/p13 are gone, and crt0's boot
  (mscratch, mtvec, the module-zeroing loops) now runs to completion in this case.
  The other programs (p01–p07, p09–p12) were **not** run here and are not claimed;
  the one requirement this case adds to them is the arming rule — a program that
  raises a trap before installing `mtvec` stops instead of trapping, and any
  program using FENCE/FENCE.I is still refused (I-037).

## 7. Defects this work found in itself (and fixed)

Recorded because each was found *by the case*, and each would have been invisible
to a coarser check:

* **the exception payload was sampled from the live writeback bus** instead of
  from the completion the arbiter accepted, so a store's fault was reported with
  the cause and `tval` of whatever event happened to be offered one or two cycles
  later (`p08`'s third trap arrived as cause 5 with the *trap stack* address). The
  capture is now taken in the cycle the arbiter accepts the event.
* **the system macro was re-executed while its completion was in flight**, so
  every MRET committed twice (not visible in the stream because the repeat is
  idempotent, but wrong). The system unit now keeps the arbiter's
  one-completion-outstanding rule.
* **a held system trap was taken a second time** in the cycle the redirect was in
  flight, writing `mepc`/`mcause` again from a stale head. Every trap source is
  now gated the same way.
* **the dispatch queue held macros whose ROB entries the flush had dropped.** For
  a cluster or a memory macro that is harmless (the completion is stale and
  rejected); for the system path the staged macro could never retire, so the
  staging entry was occupied for ever and the handler stalled. The queue is now
  emptied by the same recovery that empties the ROB — which is exactly what it
  holds entries of.
* **the CSR immediate form read a register**: `s1_x0` (true, because the address
  is x0) short-circuited the folded `zimm`, so `csrrwi` wrote zero.
