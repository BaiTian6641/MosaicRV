# MosaicRV — mid-point revision

**Written 2026-10-02 for the project's first mid-point revision.** Every number below was produced by
running the tool named next to it, in this tree, today. Nothing is inherited from an earlier summary, and
where something is unknown it says so.

```
python3 tools/check_coverage.py --verbose   -> 239 packages (98 I / 90 V / 51 H), 84 delivered,
                                               34 ladder capabilities, 18 advertisable
python3 tools/check_records.py              -> 84 delivered packages, 102 registered cases,
                                               every claimed case exists and belongs to its package
python3 tools/lint_rtl.py --profile p0|p1   -> 65/65 source files clean, both profiles
make check                                  -> exit 0 (records, exclusions, closure, capability
                                               matrix, isolation, docs)
```

## 1. What this repository is, in one paragraph

A scalable RISC-V out-of-order core in SystemVerilog, verified with Verilator 5.052 on macOS arm64,
whose research claim is that several independent execution tiles can dynamically partition, lend and
aggregate **while every hart keeps exact architectural semantics**. The claim is only interesting if the
semantics are exact, so the project's method is: every claim has a command behind it, every check has a
control that has been *seen to fail*, and a package is delivered only when its own registered case passes
from a deleted build directory and its negative control fails with a named first failure.

**84 of 239 packages are delivered** (all 84 are I/V; the 51 H hardware packages are out of scope here —
no boards, PDKs or foundry access exist on this machine, and they are recorded as excluded, never as done).

## 2. Capability state — the honest matrix

**18 advertisable**: I, M, Zicsr, Zifencei, **Zicntr**, Zihpm, A, C, S, U, PMP, Sv39, Zicsr_Zifencei_pairing,
Zaamo, Zalrsc, Zicbom, Zicbop, **Zve64d**.

**16 not advertisable, each with its named blocker**:

| capability | waiting on | what that actually means |
|---|---|---|
| **F, D** | **V-060** | the lane-partition verification; its declared inputs do not exist (§5) |
| **V** | **V-063, V-064** | V-064 is **delivered**; V-063 is blocked by a design decision (§5) |
| Sv48 | I-097, V-090 | not implemented |
| Zkt, Zvkt, Sha | I-093, V-085 | not implemented |
| Zicfilp, Zicfiss | I-095, V-088 | not implemented |
| Zimop, Zcmop, Zicond, Zmmul | I-093, V-086 | not implemented |
| PMLEN0, PMLEN7 | I-094, V-087 | not implemented |
| CMO | V-085 | implementation exists, verification package open |

Two things this table does **not** say, and they matter for the revision: the ladder's dependency edges
are the ladder's, not necessarily a design statement — F and D wait on V-060/V-061 because the ladder says
so, and V-061 is now delivered, leaving V-060 alone. And **V-060 being the sole blocker for F and D means
the shortest path to four capabilities runs through one package's prerequisites**, not through four
separate implementations.

## 3. What the machine can do today (and what it cannot)

**Can**: RV64IMC + Zicsr/Zifencei + A (Zaamo/Zalrsc) + Zicbom/Zicbop, M and S and U modes, Sv39
translation with a TLB, PMP, Zicntr/Zihpm counters, a two-hart configuration with independent per-hart
state and correct `mhartid`, a 2-cluster out-of-order fabric with dynamic ownership, steering and a
measured equal-resource comparison, an 8 KB config-driven non-blocking L1 per side with a coalescing
MSHR, a low-latency buffer with a documented invalidation set, and a vector engine that **dispatches 16 of
its 17 implemented operation families** (ADDSUB, WIDE, MUL, MULW, SHIFT, LOGIC, MINMAX, CMP, SAT, MASKLOG,
MASKPFX, SLIDE, GATHER, COMPRESS, REDUCE, REDWIDE).

**Cannot, and the report must not pretend otherwise**: no FPU execution in the advertised set (F/D wait on
verification), no RVV conformance claim (no reference model exists that can execute RVV — §5), no L2/L3,
one outstanding memory transaction per port, no SMT, no interrupt controller topology beyond a self-raised
software interrupt, no 4-hart configuration, no cross-hart coherence verification, and no synthesis,
timing or area numbers of any kind (no synthesis flow on this machine).

## 4. Defects found and fixed — and the shape they share

Each of these was **architecturally visible** and each was found by something other than reading the RTL
it lived in:

1. **`minstret` under-counted every dual-retire cycle.** The core drove the CSR increment with
   `rob_retire_ack | rob_retire_ack_next` — a one-bit OR where a count was needed. Found by reconciling
   the PMU counters against a hand-computable trace (I-076). Fixed to a 2-bit sum.
2. **Both harts read `mhartid` = 0.** `mosaic_csr` used a single generated constant and the core never
   wired `HART_ID` in. Found by a verification package (V-064). Fixed with a per-instance parameter whose
   default keeps single-hart builds bit-identical.
3. **`vtype.vsew` was encoded as log2(SEW) where RVV 1.0 defines 0..3.** `vtype` is architecturally
   visible, so a toolchain-encoded `vsetvli e32,m1` set `vill` and refused every following vector
   instruction, and a spec `e64` was silently sized as SEW=8. Found by **writing the project's first real
   RVV programs** and assembling them with a real assembler. Fixed at the source and through all eight
   readers, with the test oracles corrected — an oracle that keeps the DUT's encoding agrees with the DUT
   about the bug.
4. **A coherence defect the larger L1 exposed**: a bypassed AMO masked by a stale clean copy. Fixed with a
   targeted line-invalidate.
5. **An unarmed ECALL/EBREAK was refused against `mtvec`**, which is written at retirement and only valid
   when the macro is architecturally next. The old 2-entry queue made "not yet next" an accident of
   depth; deepening it to 8 turned the latent violation into a trap. Fixed with the rule the module's own
   header already stated.

**The shared shape, which is the most valuable thing this stretch produced**: every one of these was a
rule that was **true for the wrong reason** — correct only because nothing exercised the case that would
break it. Two of them were additionally **agreed about by the test that should have caught them** (the
`vsew` oracle and the NARROW stimulus truncate exactly as the DUT does). That is the argument for the
self-checking corpus and for controls that are *seen* to fail, and it is the reason the next revision
should treat "the test passes" as a weaker statement than it looks.

## 5. Open defects, findings and blockers

**Open defects (not fixed, with reproductions):**

* **NARROW (vector family 5) is not advertised and must not be.** `mosaic_vec_alu.sv:542` masks every
  family's `vs2` to SEW and re-widens it, and the unit stimulus and its oracle truncate identically, so
  `vnsra` is always a logical shift and `vnclip`/`vnclipu` never consume the wide bits. A probe program
  with a genuine 2*SEW negative source fails at byte offset 0. **The unit test and the DUT agree about a
  wrong behaviour** — the same shape as defect 3.
* **A vector load's EEW must equal `vtype.SEW`**, which RVV 1.0 does not require, so a legal mask load
  (`vle8.v` under SEW=32) traps `mcause=2`.
* **A latent defect in the retire/ROB lane-1 path (I-017's territory).** Found by the same-cycle second
  insert; with the insert kept and only the retire-side lane-1 request forced to zero, the program-order
  comparison passes and the wrong-PC failure disappears, so the defect is in the lane the insert exposes,
  not in the insert's identity. **The insert is held** (112 lines deleted) because it is a
  retirement-order correctness defect and cannot ship; the machine ships allocation-only, and V-013 is
  PASS in that configuration.
* **`Zihpm` is advertised while its counters read zero.** The HPM CSRs exist and are readable, but nothing
  drives them, so the advertisement currently rests on CSR existence rather than on a working mapping.
  I-076 produced the schema and the events; the wiring is not done.
* **`decode.rv64im_reserved` did not build** after the vector widening took `decode_ctl_t` to 299 bits
  while the decoder testbench still broke out 143 (being fixed at the time of writing).

**Blockers that are decisions, not unfinished work:**

* **V-063 (MEF QoS no-starvation) cannot be delivered as written.** `rtl/core/mosaic_mem_qos.sv` exists
  and is unit-verified with four mutants, but it is **not instantiated in the core** and the memory path
  has no criticality or age ports. The record says that omission was deliberate: composing the QoS window
  model with the hand-written single-outstanding ownership mux "would replace the ownership model … and
  re-derive response routing and PTW priority — a second service model, which the card forbids". **The
  choice is the architect's**: replace the ownership model, or rewrite the card to verify the scheduler as
  a unit plus the ownership model as it exists.
* **V-060 (lane partition) cannot be delivered as written** because its three declared inputs are absent:
  (a) **no lane-count axis** in any geometry profile — lanes are an attribution the broker hands out, not
  parallel datapaths; (b) **no vector program** in the oracle-recorded corpus; (c) **no reference that can
  model RVV at all** — `tools/host_oracle.py` is an 806-line scalar reference with zero vector support, so
  a vector program's golden signature cannot be recorded. (c) is the deep one and it is an **adapter**
  problem, not a research problem: NEMU is already pinned, built and RVV-capable, and `EX-041` records
  that no MosaicRV adapter produces or consumes its Difftest state layout. The one thing that must not
  happen is a vector program admitted with a DUT-produced signature, which would make the corpus an echo.

## 6. The scalability requirement (recorded 2026-10-02)

`docs/scalability-plan.md` records the user's requirement that fetch/decode queue depth, ROB depth,
allocation/rename width, decode width, INT/FP/VEC unit counts and load/store counts be **configuration
inputs** whose change preserves the architectural contract, with illegal combinations rejected by the
config gate by name.

**Config-driven today**: rename/allocation width, ROB, PRF entries/banks/ports, clusters, INT ALU per
cluster, MUL/DIV count, LSU count, LQ/SQ depth, L1 line/sets/ways, MSHR entries, PMP, fetch outstanding,
predictor/BTB/RAS sizes. **Still literals (S-1…S-3)**: decode width, decoded-queue depth, commit width,
FP unit count, VEC ALU/lane count.

**S-4 is delivered**: 15 constraint rules with 15 negative controls, and the five missing keys declared
*not yet consumed* — with two findings recorded rather than smoothed: the shipping profiles' effective
decode width (1) is **below** `rename_width` (2), and the plan's fabric disjunction is not independently
satisfiable under the schema's per-key minimums.

**The Kunminghu V2 comparison rule**, which is a precondition for publishing anything: absolute-IPC claims
only at **matched configuration** (same workload, toolchain and measurement definition); at unmatched
configurations only normalised metrics (per ROB entry, per KB of L1, retired per issued request); nothing
in the document authorises a claim about XiangShan. Reference values are tabled in the document (decode 6,
rename 6, commit 8, ROB 160, LQ/SQ 72/56, L1 64–128 KB, L2/L3, 4 ALU + 4 BJU + 5 FP + 5 vector pipes)
against ours. "Beat" is defined as better IPC at equal width/ROB/cache, better performance *per
configuration*, or the properties Kunminghu is not built for — never as "we are faster" from a 2-wide,
64-entry, cache-less machine.

## 7. What the next revision should decide

1. **V-063**: replace the memory ownership model, or rewrite the card. Nothing else unblocks **V**.
2. **V-060's prerequisites**: build the lane-count axis (also S-3's vector-lane row) and, above all,
   decide the **RVV reference** — NEMU plus an adapter is the recorded path. F and D hang on this package.
3. **The held insert**: whether to fix the retire/ROB lane-1 defect (which would let the same-cycle insert
   return) or to leave the machine allocation-only. The held code is salvageable if the experiment's
   verdict holds.
4. **The measurement packages** (I-077…I-086): I-077's card asks for cycles/IPC/actual clock/useful
   work/LUT/BRAM, and **the clock/LUT/BRAM columns need a synthesis flow this machine does not have**
   (`tools/synth_check.py` is blocked by Yosys parsing, and synthesis is H-package territory). Decide
   whether those packages are delivered with the measurable columns and the rest named as not measurable
   here, or are deferred to the H track.
5. **The RVA23 matrix** (I-092…I-098): I-092 is the entry point and is a matrix/contract task, not RTL —
   it classifies every mandatory, Sha's sub-items and every ratified option and binds owners and spec
   clauses. It is the cheapest of the seven and it gates the rest.

## 8. Process rules this stretch earned

1. **A rule that is true for the wrong reason is a defect waiting for an input.** Five instances are
   listed in §4; two of them had a *test* agreeing with the DUT about the wrong behaviour.
2. **Write software against the specification, not against the RTL.** The first real RVV programs found
   two divergences on day one that no unit case could have found, because every unit case drove the
   module with the encoding the module already used.
3. **A control that dies in the compiler, or that is inert because the path is never exercised, is not a
   control.** The control suite now refuses to run twice concurrently (`flock`) and prints how many
   controls were *inert* — two runs sharing a build directory had produced contradictory measurements
   from the same tree.
4. **A ledger entry must be backed by tracked files.** A commit of mine recorded a delivered package
   while its RTL stayed uncommitted (`git show HEAD:… | grep -c idec_queue` was 0); the rule is now to
   check `git status --porcelain` on the package's own RTL, not just the files staged.
5. **A held feature with a named failing case is honest; a shipped feature with a failing case is a broken
   machine with a green dashboard.**
