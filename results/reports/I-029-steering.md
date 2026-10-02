# I-029 — dynamic steering, the baseline policy

Work package **I-029** of `docs/implementation-plan.md` (section 4, and the
integration obligations of section 3.1: `V-028` pairs a fixed run with a dynamic
run on the same program).

> 实现动态 steering 基础策略 — Inputs: capability matrix、queue occupancy、
> producer locality、fixed baseline. Action: 先 deterministic locality-first +
> least-loaded + age tie-break，不引入不可解释 predictor；同 ELF/seed 固定与
> 动态模式对照。
> Pass: incompatible FU 永不接收；资源饱和只 stall/retry，不改变架构结果。
> Fail: load balancing 无视 dependency lifetime 或为凑性能改变物理资源数。

## STATUS: COMPLETE (module + case + controls), with one integration item left open

| Acceptance item | Evidence |
| --- | --- |
| The policy exists as RTL, with each key stated as a rule | `rtl/core/mosaic_steering.sv` (new) |
| The capability matrix is consulted and never bypassed | `unit_cap` is a port; every grant is checked against the mask the DUT was given, per cycle, in every phase (`incompatible unit`) |
| A class no unit implements is never granted | SYSTEM offers are `reject`ed in the directed phase and in the soak |
| Same program, same seed, fixed vs dynamic: identical architectural results | retire streams and signatures compared element by element; both also compared to the program's own reference (no DUT, no policy) |
| The measured difference is reported, not assumed | `[fixed]`/`[dynamic]`/`[delta]` lines from the run: cycles 572 vs 379, stalls 311 vs 115, issues 168/0/21/50 vs 88/80/21/50 |
| Saturation stalls and retries, and changes no result | `saturation` phase: the MUL/DIV queue reaches its capacity (2 of 2), 20 stall cycles, and every macro retires once with its reference value in **both** modes |
| Fail criterion — an incompatible function unit receives work | `MOSAIC_STEER_MUTANT_IGNORE_CAPABILITY`, exit 1, first failure named `incompatible unit` |
| Fail criterion — saturation resolved by dropping a macro rather than retrying it | `MOSAIC_STEER_MUTANT_SATURATION_ADMIT`, exit 1, first failure named `saturation` |
| Fail criterion — a policy that is not a function of the program's state | `MOSAIC_STEER_MUTANT_NONDET_AGE`, exit 1, first failure named `age tie-break` |
| `python3 tools/run_unit.py --profile p0 --case steering.capacity_locality` | `RESULT PASS … 3030 per-cycle comparisons over 3118 cycles … seed 1`, exit 0, binary sha256 `84e33f391c47a3963d03094cab87bfa657eb4f3e042349bccaa469c6c3e6057a` (rebuilt from two independent empty directories, same hash) |
| The ten cases the brief names | all re-run on this revision and PASS (listed under *Regression*) |
| `python3 tools/lint_rtl.py --profile p0` | green — `lint: 35 source file(s) clean`, including `ok rtl/core/mosaic_steering.sv: clean as mosaic_steering` |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | green — exit 0, no errors |
| `python3 tools/check_records.py` | green — `records agree: 40 delivered package(s), 52 registered case(s), every claimed case exists and belongs to the package claiming it` |
| `tests/unit/registry.json` untouched by this lane | the entry named three sources that did not exist; all three were created at exactly the registered paths (see *The registry entry*) |
| This report | — |

## The registry entry

`tests/unit/registry.json` already carried the case, naming

| field | value | state found |
| --- | --- | --- |
| `top` | `mosaic_steering_tb` | — |
| `rtl` | `rtl/core/mosaic_steering.sv` | **missing** |
| `sv` | `sim/tb/mosaic_steering_tb.sv` | **missing** |
| `cpp` | `sim/unit/tb_steering.cpp` | **missing** |

The three sources were missing and were created at exactly those paths. The
registry was **not** edited by this lane; the working-tree change to that file
is another lane's (`amo.linearization`, I-039) and does not touch this entry.
The card allows the policy to live "in `mosaic_dispatch.sv` **and/or**
`mosaic_cluster.sv` **and/or** a new module"; the registered topology picks the
third, so the policy is a new module and the integrated wiring is described, not
performed, under *Not covered*.

## Files

| file | state | what it is |
| --- | --- | --- |
| `rtl/core/mosaic_steering.sv` | new | the policy: capability filter, locality, load, age, and the fixed baseline, plus three observation counters |
| `sim/tb/mosaic_steering_tb.sv` | new | the wrapper: flat 32-bit pins, narrowing to the module's port widths, geometry read-back (the module's widths are its own parameters; the wrapper fixes them as literals and the driver checks the DUT's read-back against the same numbers, so a drift is a runtime failure) |
| `sim/unit/tb_steering.cpp` | new | the driver: the machine both modes steer into, the program model, the shadow contract, and the seven phases |
| `tools/run_steering_controls.py` | new | the mutant campaign: rebuild from an empty directory per mutant, `-D` recorded, hash compared, first failure required |
| `rtl/core/mosaic_core.sv` | **unchanged** | zero lines changed |
| `rtl/core/mosaic_dispatch.sv` | **unchanged** | zero lines changed |
| `rtl/core/mosaic_cluster.sv` | **unchanged** | zero lines changed |

Nothing in the live core was touched, so the core cases were re-run to confirm
that rather than to repair anything: they pass unchanged.

## The policy, as a rule

One macro is offered per cycle -- the macro dispatch is about to insert -- with
its class, its program-order age, and the locality of the unit that produced its
operand. The answer is exactly one of **grant** (with a unit), **stall** or
**reject**. Four keys are applied in this order, and all four are a function of
the offer, the capability matrix and the machine's occupancy: nothing here reads
a free-running counter, a random source, a temperature or a performance
estimate.

1. **Capability.** Unit `u` is eligible for class `c` iff bit `u*6 + c` of
   `unit_cap` is set. A unit that cannot execute a class is never a candidate,
   in either strategy, under any load. A class no unit claims is `reject`ed and
   never granted anywhere: in p0 that is SYSTEM (a CSR access, ECALL/EBREAK/
   MRET, WFI, FENCE/FENCE.I), which the core resolves at the architectural
   boundary, not in a functional unit.
2. **Locality.** Among the eligible units with room, the preferred pool is
   those that are shared between clusters or whose `unit_locality` equals the
   macro's `req_locality` -- and only when the macro *states* a preference
   (`req_locality_en` high: its producer ran in a cluster). With no preference,
   the preferred pool is every candidate and the key discriminates nothing. If
   the preferred pool is empty the whole candidate set is used, so locality is a
   preference and never a filter that could starve a macro: an operand produced
   in the other cluster is a longer wire, not an impossibility.
3. **Load.** Among the pool, the least occupied unit wins (`unit_occ` is an
   input, not a guess).
4. **Age.** Ties on occupancy are broken by waiting time, not by index: the unit
   whose last grant is oldest wins. `last_age[u]` is written with the age of the
   macro `u` was granted, so the tie-break state is derived from the grants the
   program actually caused; a unit that was just fed goes behind one that has
   been idle since before the oldest macro in the machine. With no history at
   all (all ages equal) the lowest index wins, which is the only remaining
   deterministic, history-free choice.

`grant_reason` reports which key decided -- `1` fixed affinity, `2` capability
alone, `3` locality, `4` load, `5` age -- so the decision is observable rather
than implied, and the soak *requires* all five to occur.

Mode 0 (`mode_dyn` low) is the fixed baseline the card requires as a control:
each class goes to the unit the fixed table names, load and locality are not
consulted, and a full pinned unit is a **stall**, not a spill. The two modes are
the same machine with one strategy changed, which is what makes their comparison
an experiment rather than two designs.

### The age key's modulus

`last_age` is compared unsigned, which is exact only while the distance between
two live ages is below half the modulus (the plan's section 1.3 rule). The
driver's programs are 256 macros and the driver states the bound as 4096; with
`AGE_W = 16` the modulus 65536 is more than eight times the largest comparable
distance, so wrap is unreachable in this profile. That is a bound on the
*driver's* programs, not a proof about a machine with an unbounded stream: see
*Not covered*.

## The capability matrix it consults

The matrix is an input; the module owns none of it. The environment the case
models is the geometry the plan fixes for p0 (section 1.2: two clusters each
with one integer ALU, one shared iterative MUL/DIV, one shared LSU):

| unit | class it may execute | locality | shared | capacity | latency |
| --- | --- | --- | --- | --- | --- |
| 0 `cluster0-alu` | ALU, BRANCH | cluster 0 | no | 2 | 3 |
| 1 `cluster1-alu` | ALU, BRANCH | cluster 1 | no | 2 | 3 |
| 2 `muldiv` | MUL/DIV | — | yes | 2 | 8 |
| 3 `lsu` | LOAD, STORE | — | yes | 4 | 3 |

The fixed table pins ALU and BRANCH to unit 0, MUL/DIV to unit 2, LOAD and STORE
to unit 3. The soak does not use this matrix: it randomises the matrix, the
occupancy, the room pins, the fixed table and the strategy every cycle, so the
DUT is exercised as a policy and not as a hardcoded p0 table (with bit 5, SYSTEM,
left clear so the reject path is reached).

## The fixed-vs-dynamic comparison

One generated program of 256 macros (52% ALU, 12% BRANCH, 10% MUL/DIV, 14% LOAD,
5% STORE, 7% SYSTEM; 62% of macros consume the immediately preceding one, so
dependency lifetime -- not only dispatch rate -- decides how long an entry holds
its unit's queue), one seed, the same machine, run twice. Every cycle of both
runs is compared against the shadow.

```
  [streams] fixed and dynamic retired identical streams
  [fixed]   cycles=572 stalls=311 rejects=17 issues=168/0/21/50 peak-occ=2/0/2/4
  [dynamic] cycles=379 stalls=115 rejects=17 issues=88/80/21/50 peak-occ=2/2/2/4
  [delta]   cycles=-193 stalls=-196 cluster0=-80 cluster1=+80
```

| quantity | fixed baseline | dynamic steering | difference |
| --- | --- | --- | --- |
| cycles to retire 256 macros | 572 | 379 | **−193 (−34%)** |
| stall cycles | 311 | 115 | −196 |
| macros issued to cluster 0's ALU | 168 | 88 | −80 |
| macros issued to cluster 1's ALU | 0 | 80 | +80 |
| macros issued to MUL/DIV | 21 | 21 | 0 |
| macros issued to LSU | 50 | 50 | 0 |
| macros rejected (SYSTEM, boundary) | 17 | 17 | 0 |
| peak occupancy per unit | 2/0/2/4 | 2/2/2/4 | — |
| retire stream, signature | identical | identical | — |

The two modes route *differently* (the driver fails the run if every unit's
issue count is identical, so the comparison cannot pass vacuously), and they
retire *identically*: the same 256 retire events, in program order, with the
same classes, the same value-validity and the same values, and the same
signature over that stream. Both streams also equal the reference computed from
the program alone, with no DUT and no policy.

Reproduced at two further seeds (the case is registered at seed 1):

| seed | fixed cycles/stalls | dynamic cycles/stalls | fixed issues | dynamic issues |
| --- | --- | --- | --- | --- |
| 1 | 572 / 311 | 379 / 115 | 168/0/21/50 | 88/80/21/50 |
| 7 | 588 / 323 | 401 / 134 | 173/0/26/45 | 87/86/26/45 |
| 12345 | 627 / 365 | 417 / 154 | 175/0/16/52 | 83/92/16/52 |

**What these numbers are a measurement of.** Each figure is *cycles, stalls and
issues per unit for this one program, at this geometry (four units with the
capacities and latencies tabled above), with dispatch at one macro per cycle and
in-order retire*. They are not a claim about clock frequency, about another
program, about another geometry, or about a machine with a real cluster
occupancy. The direction of the difference is a property of this policy on this
program: the fixed baseline pins 65% of the macros onto one ALU while the other
sits idle, and the dynamic policy spills to it. A policy that changed nothing
would have been reported as a finding here instead; it does not.

## Saturation: stalls and retries, not a changed result

The directed saturation phase is five MUL/DIV macros (independent) followed by
four ALU macros that all wait for the last of them, run in both modes:

```
  [saturation] macros=9 stalls=20 cycles=47 peak-occ(muldiv)=2
```

MUL/DIV is the only capable unit for its class, has two queue entries and one
execution stage of eight cycles, so the third offer must stall until a
completion frees a slot. The run **requires** the stall count to be non-zero,
the MUL/DIV queue to actually reach its capacity (2 of 2 -- so the stalls are
the policy holding macros back, not an idle machine), and all five MUL/DIV
macros to be issued. Every macro of the nine retires exactly once with its
reference value in both modes, and the two modes' streams are identical.

A stall is a retry of the *same* macro: the policy answers `stall` without
advancing anything, the driver re-offers the identical offer (same class, same
age, same locality) next cycle, and the macro is admitted later with its
identity intact. The run's own accounting is checked: grants + rejects = 256 for
every program, and the retire stream is the program's.

## Mutants

House rule, all three: the `-D` is asserted to occur in `rtl/core/mosaic_steering.sv`
before the campaign; the build directory is deleted and rebuilt per mutant, so
no shipping object can be reused; the `-D` is on the recorded command line in
that build's `build_command.txt`; every mutant binary's sha256 differs from the
shipping binary's; and the shipping build is rebuilt and run first.

Base: exit 0, sha256 `84e33f391c47a3963d03094cab87bfa657eb4f3e042349bccaa469c6c3e6057a`,
`RESULT PASS … 3030 per-cycle comparisons over 3118 cycles … seed 1`.

| mutant | exit | sha256 (16) | first failing check | injects |
| --- | --- | --- | --- | --- |
| `MOSAIC_STEER_MUTANT_IGNORE_CAPABILITY` | 1 | `89cb334b9ff230d8` | `directed:incompatible-unit-never-receives-work: cycle 9 (incompatible unit): incompatible unit: a macro of class 0 was granted to unit 2 whose capability mask is 0x0000000000000004` | the capability filter is dropped |
| `MOSAIC_STEER_MUTANT_SATURATION_ADMIT` | 1 | `438f14be90965d3a` | `directed:incompatible-unit-never-receives-work: cycle 9 (saturation): saturation: a macro was admitted to unit 0 which reported no room` | room is not consulted |
| `MOSAIC_STEER_MUTANT_NONDET_AGE` | 1 | `f19eec2dd1e301cb` | `directed:least-loaded-tie-age-first: cycle 69 (age tie-break): the least-loaded pool {mask 0x0000000000000003} was broken differently from the contract: the grant history says unit 0, the DUT chose unit 1, so the decision is not the function of the program's state the contract names` | the age tie-break reads a counter `rst` does not clear |

What each control is evidence for, in the card's terms:

* **An incompatible function unit receives work.** The mutant removes the
  capability filter, so a macro's class is no longer a condition on its
  destination. The driver offers an ALU macro with both ALUs full and the
  MUL/DIV and LSU idle; the shipping build stalls (the card's "saturation only
  stalls or retries"), and the mutant grants unit 2, which cannot execute an ALU
  macro. The first failure is the capability check itself, named in the card's
  words. Independently, the machine model would have computed the poisoned value
  of an incompatible unit into the retire stream, so the same defect is also an
  architectural difference -- the routing check merely names it first.
* **Saturation resolved by dropping a macro.** The mutant removes the room
  condition, so a full unit is handed the macro anyway. It is caught on the very
  first offer, at the invariant that a grant may only go to a unit that reported
  room. Had the invariant not been checked, the consequence the card names
  follows directly: the unit has no entry to put the macro in, so the macro is
  either overwritten or lost, and the retire stream then differs from the
  program's. The check reports the admission, which is the first observable
  event of that failure rather than its aftermath.
* **A policy that is not a function of the program's state.** The mutant's
  tie-break reads a counter that `rst` does not clear. The driver compares the
  decision against the contract *every cycle*, so the divergence is caught at
  the first tie whose outcome differs -- reported under `age tie-break`, with
  the tied pool and both choices. This is the mutant that bears on the
  fixed/dynamic comparison, and it is worth being precise about what it does and
  does not do:

  > Any tie-break among units that are *eligible and have room* is
  > architecturally inert: they implement the same class with the same
  > semantics, so no choice among them can change a value. A mutant that
  > changed the architectural result while still respecting capability and room
  > is therefore impossible -- and that impossibility is exactly the property
  > the case exists to demonstrate. What a non-deterministic tie-break destroys
  > is the *premise* of the fixed/dynamic comparison: that the dynamic run is a
  > function of the program and the seed, so that "same program, same seed,
  > same result" is a statement about the policy and not about when the
  > simulation happened to start. The driver catches it there, at the first
  > decision that the program's own state does not explain, and the run's exit
  > status and named first failure are identical in kind to the other two.

## Coverage

Every cycle of every phase is compared against the shadow: the five
combinational decision fields (`grant_valid`, `grant_unit`, `grant_reason`,
`stall`, `reject`), the four per-unit issue counters, and the three refusal
counters -- **3030 compared cycles over 3118 total** for the registered run.

| phase | what it witnesses |
| --- | --- |
| `geometry` | the geometry the driver drives (4 units, 6 classes, 16-bit age, 2-bit unit, 4-bit occupancy) is the DUT's own read-back |
| `directed` | capability first (an ALU offer with both ALUs full stalls rather than spilling into MUL/DIV), locality preference and its spill when the preferred cluster is full, stall when every capable unit is full, SYSTEM rejected and never granted, the shared units taking their classes, MUL/DIV stalling when its only unit is full, the fixed baseline pinning classes and stalling instead of spilling, and the age key alternating between two equally loaded units |
| `matrix` | a legal matrix with a third capable unit, where the age key rotates through three units and wraps; and a matrix whose row for a class is empty, where both strategies reject |
| `fixed` / `dynamic` | the experiment above: 256 macros, both modes, streams and signatures compared, routing required to differ |
| `saturation-fixed` / `saturation-dynamic` | the directed MUL/DIV burst in both modes |
| `random` | 1200 seeded cycles with the matrix, occupancy, room pins, fixed table and strategy randomised, every output compared every cycle; the run **requires** grants, stalls and rejects to occur and all five `grant_reason` values to be used (witnessed: reasons 1..5 all non-zero, both strategies used) |
| `determinism` | the same program and seed reproduce 256 decisions (macro, unit, reason, cycle) and the signature exactly |

## Gates

| gate | result |
| --- | --- |
| `python3 tools/lint_rtl.py --profile p0` | green — `lint: 35 source file(s) clean` |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv' \| sort)` | green — exit 0, no errors |
| `python3 tools/check_records.py` | green — 40 delivered packages, 52 registered cases, ownership agrees |
| `steering.capacity_locality` | PASS, exit 0, sha256 `84e33f391c47a3963d03094cab87bfa657eb4f3e042349bccaa469c6c3e6057a` |

## Regression

Every case the brief names was re-run on this revision, all PASS, all unchanged
by this lane (no core file was edited):

`fabric.fixed_two_cluster`, `core.corpus_branch`, `core.unwritten_reg_read`,
`core.mem_program`, `core.trap_csr_program`, `core.corpus_sweep`,
`fence.code_and_data_order`, `mmio.exactly_once`, `iq.wakeup_insert_select`,
`bypass.local_raw_chain`.

## Not covered

* **The policy is not wired into the live core, and the live core does not use
  it.** `mosaic_dispatch.sv` still routes by the alternating toggle
  (`target_cluster = dec_ctl0.is_muldiv ? 1'b0 : aff_toggle;`) and
  `fabric.fixed_two_cluster` still measures that. `mosaic_core.sv`,
  `mosaic_dispatch.sv` and `mosaic_cluster.sv` have **zero lines changed** by
  this lane. What an integration would need, precisely:
  1. a strategy select (fixed / dynamic) reaching dispatch -- a new input, and a
     decision about what the shipping core's default is;
  2. producer locality at dispatch: which cluster produced the macro's operand.
     Dispatch today has tags and generations, not a producer cluster, so this is
     a new field from rename/the PRF, not a wire that exists;
  3. per-unit room and occupancy for the four targets, which dispatch does not
     have today (it has the two clusters' `ins_ready`, not the shared units'
     occupancy);
  4. the capability matrix and the fixed table as configuration, from the same
     place the rest of the geometry comes from.
  Note that making the shipping core use the *fixed* baseline would contradict
  `fabric.fixed_two_cluster`, whose claim is that cluster 1 executes its share
  of a program; that case would have to be re-stated as part of the integration,
  not silently kept.
* **The occupancy the policy reads is modelled, not measured from real
  clusters.** The driver owns the machine table; nothing here measures a real
  issue queue's occupancy or a real cluster's insert latency.
* **The policy's timing is not characterised.** It is combinational over four
  units; whether it fits the dispatch insert path's cycle in the integrated core
  is not measured, and no synthesis, area or Fmax claim is made.
* **The age modulus bound is a driver bound.** The 4096-macro bound that makes
  the unsigned age comparison exact is stated by the driver. A machine with an
  unbounded stream would need the modular comparison the plan's section 1.3
  describes; the RTL's own comment says so rather than implying the width is a
  proof.
* **Whether the policy is actually exercised at saturation: yes, in part.**
  Measured: the MUL/DIV queue fills to its capacity (2 of 2) in both modes and
  stalls (20 stall cycles in the directed burst); in the fixed baseline the
  cluster-0 ALU queue also fills to capacity (peak 2 of 2, 311 stalls in the
  256-macro program); in the dynamic run *both* cluster queues reach capacity
  (peak 2/2) while the run's 115 stalls occur. So saturation of a single unit is
  exercised, and the policy's spill/least-loaded behaviour at that boundary is
  exercised. What is **not** exercised is a stall caused by *both* clusters
  being full in the same cycle while the macro's class has no shared fallback
  -- i.e. an ALU macro that could go to either cluster and finds neither with
  room. That state is reachable in principle (both peak at capacity) and was not
  separately forced by a directed case.
* **Two clusters contending for one unit in the same cycle is not exercised.**
  This core dispatches one macro per cycle, so the shared MUL/DIV and LSU are
  contended by macros from both localities *across* cycles (the soak and the
  program do that), but there is no case in which two clusters' dispatchers
  offer to one shared unit in the same cycle. That is a property of the
  dispatch width, not of the policy, and it would need the second ROB
  allocation port the plan reports as missing.
* **SYSTEM is never steered because no unit claims it.** The case's claim "an
  incompatible unit never receives work" is checked against the matrix the DUT
  is handed. If a configuration claimed SYSTEM on a unit, this policy would
  route SYSTEM macros to it; the soak deliberately leaves that bit clear, so
  that behaviour is untested by this case.
* **No claim about a different geometry.** The capacities, latencies and the
  unit count are the case's environment. A different unit count changes the
  parameter widths and the wrapper's pins; the policy is written generically
  (loops over `ST_N_UNITS`), but only 4 units / 6 classes is built and run.
* **No performance claim beyond this program.** The 34% cycle reduction is for
  this one generated program at this geometry; the negative direction is
  reachable (a program whose macros are all shared-unit work would show no
  difference, and one whose locality preference is always satisfiable would show
  a smaller one).
