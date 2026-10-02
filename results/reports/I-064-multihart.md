# I-064 / `multihart.isolation` — two architectural domains on one physical machine — **PASS**

Work package I-064 (`docs/implementation-plan.md` §3.1, the "two-hart architectural
domains" card). Case `CASE=multihart.isolation`, top `mosaic_multihart_tb`,
driver `sim/unit/tb_multihart.cpp`.

All commands are from `/Users/flare/MosaicRV`, Verilator 5.052 (macOS arm64),
profile **p3** (the two-hart profile: `config/profiles/p3.json`, `"harts": 2`),
measured on this revision.

> **The card.** *Inputs:* independent PC, rename tables, ROB, CSRs, load/store
> queues and interrupt state, plus a shared-resource contract. *Action:* start
> with statically partitioned execution resources, run different programs and
> ASIDs on the two harts, and make every packet carry its hart ownership; run
> `CASE=multihart.isolation`. *Outputs:* p3 two-hart control planes and
> independent retirement streams. *Pass:* one hart's fault or flush does not lose
> the other hart's requests or state, and each hart's stream compares against its
> own reference. *Fail:* a global redirect clearing the other hart, or a hart-ID
> reuse confusing a response.

## 0. Status, in one paragraph

Two `mosaic_core` instances run as two architectural domains. Each owns its PC,
rename tables, ROB, PRF, issue queues, execution lanes, load/store queues, CSRs,
interrupt state, TLB/PTW and predictor — the **static partition** the card starts
with. They **share the memory service**: one hart-tagged request/response bus,
one arbiter, one physical memory (the SoC the driver models). Every request that
leaves a hart carries the hart field of `mosaic_id_pkg::macro_id_t`; the response
is routed back by that identity and a response that names a slot which did not
request it is **dropped and counted**, never delivered to the wrong hart. The
registered case runs two different programs with **different ASIDs and different
Sv39 page tables that map the same virtual address to different physical pages**,
compares each hart's concurrent retirement stream to its own solo reference field
for field, drives a load page fault and branch redirects in hart 0 while hart 1 is
mid-program, and shows both harts really use the one shared service. Result:
**both streams equal their references, zero ownership mismatches, one trap
(cause 13, tval 0x4000_2000), 305 shared requests, 217 contention cycles.** Four
RTL mutants and one driver-side control each exit 1 naming the check they break.

## 1. Files

| File | State |
|---|---|
| `rtl/core/mosaic_multihart.sv` | **new**: the two-hart design — two `mosaic_core`, the hart-tagged shared memory bus, the round-robin arbiter, the ownership router with per-slot response buffering, the four `MOSAIC_MH_MUTANT_*` controls, and the shared-service evidence counters |
| `rtl/core/mosaic_core.sv` | **changed**: `HART_ID` and `RESET_PC` parameters (both defaulted so every existing instantiation is bit-for-bit); the hart field is placed into every `macro_id_t` producer (the five `.hart` sites, `rob_head_id`, `disp_mem_full_id`, the four `sq_commit*_id` sites); `RESET_PC` replaces the hard-coded reset vector; `HART_ID` is passed to the clusters and the FP unit |
| `rtl/core/mosaic_cluster.sv` | **changed**: `HART_ID` parameter, placed into the cluster's completion identity |
| `rtl/core/mosaic_fp_unit.sv` | **changed**: `HART_ID` parameter, placed into the FP completion identity |
| `sim/tb/mosaic_multihart_tb.sv` | **new**: the wrapper — flattens the design's ports, assembles the two bus packets from flat driver ports, and fixes the two harts' reset PCs |
| `sim/unit/tb_multihart.cpp` | **new**: the driver — the two programs, the two page tables, the shared memory model, the three phases (solo 0 / solo 1 / both), the per-hart reference comparison, the isolation checks, the sharing checks and the `--control-swap-ref` control |
| `tools/run_multihart_controls.py` | **new**: the reproducible control runner |
| `tests/unit/registry.json` | **not edited** — the `multihart.isolation` entry already existed (added by the integration lead); its `rtl` list names `rtl/core/mosaic_multihart.sv`, and the design's other dependencies are found through the runner's `-I rtl/core`. No registry change is needed. |

Not touched: `config/status/implementation_status.json`, `tests/programs/**`,
`results/PROGRESS.md`, every existing case's driver.

**No configuration change.** The hart count comes from the p3 profile
(`"harts": 2`); the module is a two-hart construction regardless of profile, and
the single-hart profiles build only `mosaic_core` as they always did.

## 2. The design: what is replicated, what is shared

This table *is* the design. "Replicated" means each hart has its own instance;
"shared" means one instance both harts contend for.

| Structure | Replicated (per hart) | Shared (one instance) |
|---|---|---|
| PC / front end (fetch, decode buffer, predictor) | yes | — |
| Rename tables, free list, undo journal | yes | — |
| ROB, macro descriptor, retirement | yes | — |
| PRF, issue queues, ALU/branch lanes, MUL/DIV | yes | — |
| FP unit, vector engine, lane broker | yes | — |
| Load queue, store queue, LSU endpoint | yes | — |
| CSRs, trap state, interrupt state (per-hart `mcsr`/`satp`/PMP) | yes | — |
| TLB and PTW | yes | — |
| L1 instruction and data cache path | yes (instantiated per core) | — |
| **Memory service** (request/response bus, arbiter, physical memory, SoC/MMIO) | — | **yes** |
| Clock and reset domain | — | **yes** (one `clk`, one `rst`; `hart_en_i` holds a hart in reset) |

The L1 cache path is replicated and **disabled** in this configuration
(`cache_en_i` low), so every access — including a page-table walk, which the core
sends through its data port — reaches the shared memory service. That is what
makes the ownership discipline visible on *every* packet. A **shared** cache is
I-065's coherence subject; this package shares the memory service, which is what
the two harts actually contend for. The report's *Not covered* says so again.

### 2.1 The shared memory service contract

One request channel, one response channel:

```
request   mem_req_valid, mem_req_ready, mem_req_hart[1:0], mem_req_src,
          mem_req (we/addr/size/wstrb/wdata/amo/amoq/rl), mem_req_id, mem_req_epoch
response  mem_rsp_valid, mem_rsp_ready, mem_rsp_hart[1:0], mem_rsp_src,
          mem_rsp (rdata/fault), mem_rsp_id, mem_rsp_epoch, mem_rsp_len
```

`src` is 0 for data and 1 for instruction; `hart` is the hart field. Four slots
indexed `{hart, src}` each hold at most one outstanding request (every core port
is single-outstanding), so a response's owner is decidable. The arbiter rotates
priority across the four slots, so neither hart can starve the other. A matched
response is captured into that slot's buffer and freed from the bus *in the same
cycle* (`mem_rsp_ready` is always high), so a core that is not ready for its
response can never hold the one shared channel and with it the other hart — this
was a real deadlock found while building the case and is fixed here.

## 3. Hart ownership: the rule and where the identity lives

The identity is the `hart` field of `mosaic_id_pkg::macro_id_t`
(`{hart, rob_index, rob_gen, uop_index}`) — the field the store queue, the TLB,
the MSHR and the chaining network already match on (`uop_id_eq` compares it). It
is the *same* idea reused, not a second one invented.

* **On the request:** the arbiter tags every request with `mem_req_hart`, and
  each core places its own `HART_ID` into the `macro_id_t` of every packet it
  emits (completions, memory uops, the endpoint's transaction id
  `o_mem_dmem_id`). The case checks the core's own identity: on every accepted
  data request, `o_mem_dmem_id`'s hart field must name the hart.
* **On the response:** the router computes `rslot = {mem_rsp_hart, mem_rsp_src}`
  and requires `slot_busy[rslot]` **and** the instruction id/epoch to match. A
  response that names a slot which did not request it — or a hart the machine
  does not have (the full hart field indexes the slot space, so hart 2/3 index a
  slot that is never busy) — is consumed, dropped, and counted in
  `o_owner_mismatch_ctr`. It is a detected error, never a wrong answer.
* **`HART_ID` default 0.** In the single-hart profiles the field is zero, which
  is exactly what the five producers hard-coded before; the multi-hart build
  changes one field's *meaning*, which is what `mosaic_uop_pkg`'s header
  anticipated ("removing it would make the multi-hart profile a change of every
  interface in the core rather than a change of one field's meaning").

## 4. The case: two programs, two ASIDs, two references

```
$ python3 tools/run_unit.py --profile p3 --case multihart.isolation
PASS multihart.isolation          task=I-064
```

**Two different programs.** Hart 0 and hart 1 run hand-encoded RV64IM programs
with different instruction sequences and different constants:

| | hart 0 | hart 1 |
|---|---|---|
| ASID (`satp[59:44]`) | 1 | 2 |
| Sv39 root | `0x8000_6000` | `0x8000_9000` |
| same VA `0x4000_0000` maps to | `0x8000_C000` | `0x8000_D000` |
| S-mode body | translated load, 8-trip add loop, translated signature store, translated load from an **unmapped** VA, post-handler marker, park | translated load, 12-trip `addi`+`xori` loop, translated signature store, park |
| M-mode handler | records `mcause`/`mtval`, advances `mepc`, `mret` | present but never taken |

Both harts use the **same virtual address** `0x4000_0000` and their own page
tables translate it to a different physical page, so an address space that leaked
between the harts would be visible as a wrong value. The case requires that hart
0's data port reached `0x8000_C000` and hart 1's reached `0x8000_D000`.

**Three phases, each its own reference.** The driver runs the same binary three
times from a clean reset with identically seeded memory:

```
phase A   hart_en = 01   hart 0 alone      -> hart 0's reference
phase B   hart_en = 10   hart 1 alone      -> hart 1's reference
phase C   hart_en = 11   both together     -> compared to A and B
```

**Result.**

| | solo 0 | solo 1 | both |
|---|---:|---:|---:|
| cycles | 858 | 654 | 855 |
| retires | 102 | 99 | 102 / 99 |
| signature | `0x1111_2222_3333_444c` | `0x5555_6666_7777_88a0` | same |
| trap cause / tval | 13 / `0x4000_2000` | — | 13 / `0x4000_2000` |
| redirects | 12 | 24 | 12 / 24 |
| satp (ASID) | — | — | `…0001…` / `…0002…` |

Both signatures equal an independently computed value (`seed+8` and
`seed ⊕`-folded 12 times), so the comparison is not merely self-consistent. Each
hart's concurrent stream equals its solo stream **field for field** (sequence,
PC, instruction, length, register write, value, store address/data/size, CSR
address/value, trap cause/tval). The only field excluded is the value port of a
non-register-writing instruction (a store's completion stash is not architectural
and is timing-dependent), stated here rather than silently dropped.

## 5. The isolation scenarios, with their evidence

**(a) A fault/flush in one hart does not disturb the other.** Hart 0's S-mode
body performs a translated load from the unmapped VA `0x4000_2000`; its M-mode
handler records the trap and returns. In the concurrent run hart 0 takes exactly
one trap — cause 13 (load page fault), tval `0x4000_2000` — while hart 1, whose
12-trip loop is longer, is mid-program. Hart 1 takes **no** trap, and hart 1's
concurrent stream equals its solo reference (99 retires, field for field). The
check is the stream equality; the trap and redirect counts are the evidence that
the scenario was actually driven.

**(b) No global redirect.** Hart 0's loop back-edges redirect 12 times. At the
cycle hart 0 redirected, hart 1 had in-flight ROB work (max occupancy 1 observed
at a redirect) and continued committing afterwards; hart 1's own redirect count
(24) is its own. Because the redirect arbiters are per-hart instances, a global
redirect is not a wiring accident here — which is why the fail mode is injected
as a control (§6): the `MOSAIC_MH_MUTANT_GLOBAL_REDIRECT` mutant makes hart 0's
redirect reset hart 1, and the stream-equality check catches it.

**(c) Hart-ID reuse must not confuse a response.** `o_owner_mismatch_ctr` is 0
in the shipping run, and every packet's own identity named its hart
(`id_hart_bad = 0`). The two controls that inject this fail mode —
`MOSAIC_MH_MUTANT_IGNORE_HART` (the matcher ignores the hart) and
`MOSAIC_MH_MUTANT_REUSE_TAG` (every request tagged hart 0) — both exit 1.

**(d) The shared structures are actually shared.** In the concurrent run both
harts' requests reached the one service (h0 = 136, h1 = 169; instruction 283,
data 22) and both had a request outstanding at the same time for 217 cycles
(`o_contend_ctr`), which is the direct statement that the service is shared
rather than replicated.

## 6. Controls

```
$ python3 tools/run_multihart_controls.py
```

Each mutant is built from an empty directory with exactly one `-D`, must differ
from the shipping binary, and must exit 1 naming the check it breaks. The
shipping binary is built and run first from an empty directory
(`RESULT PASS … shared_requests=305 contend=217`, sha256
`43d404f42b134a8144518f1815e486236355ad1146922bd8d49acefebef562d9`).

| control | injects | exit | sha256 | the check it breaks |
|---|---|---|---|---|
| `MOSAIC_MH_MUTANT_IGNORE_HART` | the response matcher ignores the hart and routes to hart 0's slot of that source | 1 | `c17708b6…` | `equals its own reference` — hart 1's responses are misrouted, hart 1 never completes (0 retires). The card's "hart-ID reuse confuses a response" fail mode |
| `MOSAIC_MH_MUTANT_REUSE_TAG` | every request is tagged hart 0 | 1 | `b824f17f…` | `retired its program and parked` — hart 1's response names hart 0's slot and is dropped; hart 1 never completes |
| `MOSAIC_MH_MUTANT_GLOBAL_REDIRECT` | hart 0's redirect resets hart 1 | 1 | `698fe1e8…` | `equals its own reference` — hart 1 restarts on every hart 0 redirect. The card's "global redirect clears the other hart" fail mode |
| `MOSAIC_MH_MUTANT_ONE_HART_BUS` | the arbiter never grants hart 1 | 1 | `638ebbba…` | `both harts used the one shared memory service` — the sharing claim is false and hart 1 makes no progress |
| `--control-swap-ref` (driver) | each hart is compared against the other hart's reference | 1 | shipping binary | `equals its own reference` — the two programs differ, so the comparison is falsifiable |

The driver-side control is the falsifiability check on the per-hart reference
comparison itself: if the two streams were compared against a single shared
reference and passed, the "each hart against its own reference" claim would be
vacuous.

## 7. Regression

Every case the brief names was re-run on this revision:

| case | result | | case | result |
|---|---|---|---|---|
| `core.act_dut` (ACT4) | PASS 127/127 | | `boot.p1_contract` | PASS |
| `core.corpus_sweep` | PASS | | `privilege.permission_matrix` | PASS |
| `core.mem_program` | PASS | | `sv39.walk_and_faults` | PASS |
| `perf.equal_resource_compare` | PASS | | `tlb.sfence_vma` | PASS |
| `locality.integrated_path` | PASS | | `amo.linearization` | PASS |
| `cache.integrated_path` | PASS | | `lrsc.reservation_progress` | PASS |
| `vec.integrated` | PASS | | `fabric.integrated` | PASS |

Gates: `make check`, both profiles' `lint_rtl` (`--profile p0 … p3`, each with
its `--self-test`), `slang-tidy`, `check_records`, `check_exclusions` (+
`--negative`) and `make lint-cpp` are green on this revision (see the run log for
the exact commands). The single-hart profiles are bit-for-bit unchanged: the two
new `mosaic_core` parameters default to the values the code hard-coded before
(`HART_ID = 0`, `RESET_PC = MOSAIC_RESET_VECTOR`), and no existing case passes
either parameter.

## 8. Not covered

* **This is static partitioning.** Each hart has its own issue queues and
  execution lanes; the plan's dynamic sharing, borrowing and reconfiguration are
  later packages. **I-031's ownership FSM is compiled but not instantiated** (it
  exists for reconfiguration, and this machine never reconfigures), exactly as
  the I-090 report leaves it.
* **The two harts share one clock and one reset domain.** `hart_en_i` holds a
  hart in reset, which is how the solo references are taken; the two cores share
  `clk` and `rst`. The isolation claims are about *architectural state*, not about
  independent clock domains — a fault or redirect in one hart does not disturb
  the other's state, but the two harts are not asynchronous.
* **No coherence and no cross-hart ordering claim.** The harts run on disjoint
  physical regions in this case; a store by one hart is not made visible to the
  other by any invalidation or ownership state, and the memory service is a
  single blocking bus, not a coherent fabric. The L1 cache path is replicated and
  disabled. **I-065 owns shared-memory serialization and coherence.**
* **No SMT claim.** The execution resources are statically partitioned; nothing
  here shares an issue queue, a lane or a rename table between the harts.
* **A single blocking memory bus.** One request and one response channel, so the
  two harts' accesses serialize. This is the honest cheapest construction the
  card permits ("statically partitioned execution resources first"); a banked,
  multi-outstanding service is a later package.
* **No cross-hart invalidation of the LR/SC reservation.** The core's
  `ext_write_*` coherence-notification port is tied low on both harts, so an LR
  on one hart is not broken by a store on the other. That is I-040/I-065.
* **Two programs, one seed.** Every number above is seed 1 and the two
  hand-encoded programs. The case measures those programs; a wider sweep is not
  claimed.
* **No timing, area or Fmax claim.** The arbiter is combinational over four
  slots and the two cores are two full copies of the single-hart machine; the
  cost of the second domain is not measured here.
