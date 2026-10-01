# I-024 — atomic resource lease (`CASE=lease.conflict_and_cancel`)

Work package **I-024** of `docs/stage-2-execution-fabric.md`, implementing the
`CASE=lease.conflict_and_cancel` obligation in `docs/implementation-plan.md` §I-024.

## STATUS: COMPLETE

| Acceptance item | Evidence |
| --- | --- |
| Every grant occupies its whole set at once | occupancy is *derived* from the lease ledger — a partial reservation is not representable — and the shadow rebuilds the same derivation from its own ledger every cycle |
| A refusal has no side effect | directed phase + a **replay experiment**: the same stimulus with and without the refused request is bit-identical from the refusal cycle on |
| Release/cancel exactly once | the three rejection classes are counted by name, and a second terminal moves no resource; conservation `grants == releases + cancels + live` is checked on the DUT every cycle |
| Card's Fail criterion "只预留 FU 再无限等待 WB" | the partial phase's refusal leaves the FU pool occupied by nobody, and the same request is served the moment a credit returns; the `REFUSAL_CONSUMES` mutant shows the case would catch a reservation without a grant |
| Card's Fail criterion "flush 重复归还 credit" | the cancel phase, the `DOUBLE_RELEASE` mutant (release twice) and the `CANCEL_LEAK` mutant |
| `python3 tools/run_unit.py --profile p0 --case lease.conflict_and_cancel` | `PASS`, exit 0, 10 checks / 0 failures, 4366 shadow comparisons over 4462 cycles |
| Verilator `--lint-only -Wall` clean | 0 warnings, 0 errors on the module; `python3 tools/lint_rtl.py --profile p0` → `ok rtl/core/mosaic_lease_alloc.sv: clean` (that run reports one *sibling* file, `rtl/core/mosaic_macro_desc.sv`, failing — not this lane's) |
| `slang-tidy` clean | 0 errors for this module, singly and inside `--single-unit` over all of `rtl/`; the tree-wide run currently reports 5 errors in a sibling file (`rtl/core/mosaic_dispatch.sv`, use-before-declaration), none of them here. 53 style warnings: 52 × `STYLE-2` port-suffix noise that every module in this tree emits — `mosaic_iq` emits 67 — and one `STYLE-16` generate-block style warning, also present in `mosaic_iq`) |
| C++ held to the stricter standard | `c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow` → no diagnostic from `sim/unit/tb_lease.cpp` (only from Verilator's own headers) |
| Mutants shown failing | six of them, each with a distinct named first failure and a check-count delta; a seventh is documented as a **redundancy probe**, see *Mutants* |
| This report | — |

## Files

| File | Contents |
| --- | --- |
| `rtl/core/mosaic_lease_alloc.sv` | `mosaic_lease_alloc`: the atomic all-or-none resource lease |
| `sim/tb/mosaic_lease_alloc_tb.sv` | `mosaic_lease_alloc_tb`: straight pass-through, widths from the generated package, geometry read back out of the instance |
| `sim/unit/tb_lease.cpp` | independent C++ shadow, per-cycle comparison, 9 directed/random phases, coverage in the recorded result |
| `results/reports/I-024-lease.md` | this file |

No file outside this list was edited. `tests/unit/registry.json`,
`config/status/implementation_status.json`, `results/PROGRESS.md`,
`tools/gen_manifest.py`, `rtl/core/mosaic_pkg.sv` and every module owned by
another package are untouched. `rtl/core/filelist.f` was **not** edited either:
`tools/gen_manifest.py` generates the synthesis filelist from the tree, so the
module is picked up there without a hand edit (the same is true of the other
modules that are absent from `rtl/core/filelist.f`).

## Interface

`mosaic_lease_alloc` takes **no** parameters a caller can override: the geometry
is `localparam` in the parameter port list, read from `mosaic_cfg_pkg.svh`. The
lease generation width is `$clog2(MOSAIC_ROB_ENTRIES) + 1` — the same idiom and
the same value as `mosaic_iq`'s wakeup generation, derived from the same fact (a
terminal event is produced while its own uop is in flight, and a uop in flight
holds at most one lease, so the population is bounded by the ROB).

`mosaic_id_pkg.svh` is **not** included: this module needs no identity *type*
from it, only the width of an owner generation, and that width is derived above
from `MOSAIC_ROB_ENTRIES` with the same bound written out in the header.
`MOSAIC_ID_W_OWNER_GEN` is the semantically named field for the same thing and
holds the same value (7). When this module was written the generated identity
header had no include guard, so a second includer broke
`slang-tidy --single-unit` with `duplicate definition of 'mosaic_id_pkg'`; that
was fixed in the generator during this session (`build/p0/rtl/mosaic_id_pkg.svh`
now carries a guard), so avoiding the include is no longer a requirement — it is
a preference, because the identity contract belongs to the modules that pass
macro and physical-register ids around.

| Field | Source | p0 |
| --- | --- | --- |
| `REQ_COUNT` | `MOSAIC_CLUSTERS + MOSAIC_MULDIV_UNITS` | 3 issue ports |
| `SIZE_ALU` | `MOSAIC_CLUSTERS * MOSAIC_ALU_PER_CLUSTER` | 2 |
| `SIZE_MD` | `MOSAIC_MULDIV_UNITS` | 1 |
| `SIZE_WB` | `MOSAIC_CLUSTERS * MOSAIC_RESULT_FIFO` | 4 |
| `SIZE_NET` | `MOSAIC_CLUSTERS` | 2 |
| `LEASES` | `= SIZE_WB` | 4 |
| `GEN_W` | `$clog2(MOSAIC_ROB_ENTRIES) + 1` | 7 |
| `MAX_SZ`, `SLOT_W` | max pool, `$clog2` of it | 4, 2 |
| `ROW_W`, `IDX_W`, `LEASE_W`, `RR_W` | derived; `IDX_W = ROW_W + 1` | 2, 3, 10, 2 |

`LEASES = SIZE_WB` is a derivation, not a guess: every live lease holds exactly
one result credit for its whole life (reserved at grant, returned at the
terminal), so the number of simultaneously live leases is bounded by the credit
pool.

The index field of a lease id is **one bit wider than the ledger** on purpose.
Without the spare encoding a range check would be a tautology for a power-of-two
ledger and the "unknown lease id" report would be unreachable — a report nothing
could prove works. For p0 (4 records, 3-bit field) indices 4..7 are rejected.

## Cycle semantics

* **Set, not classes-per-port.** `req_mask[i*POOLS + p]` names the classes port
  `i` needs. Nothing ties a port to a class: a cluster port may ask for the
  shared MUL/DIV, which is exactly where the contention this module exists for
  comes from. The mask is the whole request.
* **All or none.** The grant condition is "every named class has a free slot
  *and* a record is free". One grant per requester per cycle, chosen in
  round-robin order starting at the pointer; the pointer follows the last grant
  of the cycle, so a requester that lost a contended slot is reached first on a
  later cycle.
* **Grant output.** `req_ready[i]` is combinational in this cycle's requests and
  the pre-edge ledger, and it is a commitment. `grant_id[i]` hands back
  `{record index, generation}` laid out exactly like a terminal id, and
  `grant_slot` hands back the slot reserved in each named class — the requester
  does not infer its slot, it is told.
* **Terminals free first.** A release or cancel accepted on an edge stops
  holding its slots on that edge, so the credit it returns can fund a grant on
  the *same* edge. A grant may therefore reuse a record terminated by the same
  cycle's terminal; the new epoch's generation is the old one plus one, so the
  two ids stay distinguishable.
* **Release wins.** The release is classified against the pre-edge ledger and
  applied; the cancel is then classified against what the release left behind.
  One lease presented on both terminal ports in one cycle is therefore not a
  don't-care: the release is accepted and the cancel is reported as
  `can_after_release`. `mosaic_fetch` states the same rule for a credit returned
  and spent on one edge.
* **Three-way rejection.** *stale* = the id names a record that was never
  granted, a generation that was never issued, or a superseded epoch (the caller
  has the wrong identity); *repeat* = the id is the record's current epoch and it
  is already terminated by the same event (the caller's lifetime accounting
  released it twice); *crossed* = the current epoch was terminated by the *other*
  event (the flush-versus-completion race). All three change nothing and are
  counted apart, because collapsing them into "not the owner" loses exactly the
  distinction a caller needs.
* **The ledger is the only copy of occupancy.** There is no second free-slot
  bitmap. Occupancy is derived from the live leases, so a slot is occupied
  exactly while a live lease holds it: a reservation belonging to no lease — the
  "FU reserved, waiting forever for a credit" state — is *not representable*.
  The price is a `LEASES`-term OR per pool, unrolled; a design with hundreds of
  leases would need the bitmaps back and would have to earn the invariant by
  testing it.
* **Reset is cold.** Synchronous, active high: the whole ledger returns to its
  initial state (every pool free, every record never-granted, every counter
  zero). It does not drain live leases one at a time — that is the FREEZE/DRAIN/
  REVOKED/INSTALL reconfiguration protocol of I-090, and a module that pretended
  to implement it here would need a state machine no test in this case could
  distinguish from a cold reset. What a reset must guarantee *here*, and what the
  case checks, is that the post-reset ledger equals the cold ledger whatever the
  pre-reset occupancy was, and that no pre-reset lease id can free anything
  afterwards (the `used` bits are cleared with the generations, so every old id
  is stale).
* **Everything combinational is in one `always_comb`.** See *Findings* 1: the
  occupancy derivation, the terminal classification, the arbitration and the
  packing of the grant ids share one block, because a separate block reading the
  arbitration's output was evaluated from a stale copy.

### State that is not reset, and why

`lease_gen`, `lease_mask` and `lease_slot` are not cleared, by the rule
`rtl/common/mosaic_ram.sv` states (reset cost is control state, not storage):
they are meaningless without `live`/`used`, which are reset. Two consequences
are load-bearing in the testbench: the *cold* state is compared through a ledger
projection that excludes those arrays, and the generation a grant hands out is
gated on `used` (`used ? gen + 1 : 0`, exactly as `mosaic_rename` gates on
`gen_valid`) so the power-up content of the array is never observable.

## Invariants checked every cycle

```
o_grant_count == o_rel_ok_count + o_can_ok_count + o_live_count   (in the DUT)
o_live_count  == population(o_lease_live)                          (in the DUT)
o_occ_count[p] == population(o_occ real slots of pool p)           (in the DUT)
o_occ       == union of the live leases' reservations              (shadow vs DUT)
o_lease_slot/mask/gen, o_rr, all ten counters, every cycle         (shadow vs DUT)
```

The third is checked against the DUT alone, so it holds in every cycle of every
phase with no reference to the shadow. The fourth is the shadow's *independent*
rebuilding of the derivation, which is why a partial reservation cannot hide.

## Testbench

Phases, each of which resets first, in order:

| Phase | What it establishes |
| --- | --- |
| `reset-state` | the cold ledger; a reset taken with every pool occupied restores it; no pre-reset id can free anything after it |
| `conflict` | two requesters needing the one shared MUL/DIV slot in one cycle → exactly one grant (port 0, the pointer's own port, wins), the refusal changes nothing at all, and the loser is granted on the edge the winner's release returns the slot |
| `partial` | a request naming {FU, credit} with every credit held is refused **and the FU slot stays free**, and the same request is served on the edge a credit returns; the mirror image (FU pool full, credit free) is refused without consuming the credit. At this geometry the request is refused because the ledger's records are exhausted, which — by the redundancy above — is the same moment the credit pool empties; what the phase establishes is the *consequence*: no class of the set was reserved |
| `cancel` | a flush returns both classes exactly once; the same requester is served again on a new epoch of the same record; the old id is rejected as *stale* rather than freeing the new lease |
| `terminal-errors` | double release, never-granted id, out-of-range record, never-issued generation aimed at a *live* lease, cancel-after-release, release-after-cancel: each counted by name, none moving a resource |
| `same-cycle` | a credit returned on an edge funds a grant on that edge, on the slot the release returned |
| `refusal` | the replay experiment: identical state from the refusal cycle on, bit for bit |
| `wrap` | one record's generation counted through its whole modulus (144 grants, wrap observed) |
| `random` | 4000 cycles of random request sets, random terminal timing aimed at live/dead/never-granted/superseded/out-of-range ids, shadow-compared every cycle |

**Two-snapshot rule.** `Cycle()` compares the *pre-edge* combinational answer
(`req_ready`, `grant_id`, `grant_slot`, the eight terminal pulses) against the
shadow's expectation computed from the same pre-edge state, then advances the
shadow, then applies the edge, then compares the *registered* state (occupancy,
ledger, pointer, counters). The pre-edge answer is returned **by value**, so a
phase that holds it across another `Cycle()` cannot silently read the next
cycle's outputs.

**The shadow is not the RTL.** It keeps its own ledger *and* separately a
per-pool bitmap and per-pool count, and asserts they agree with each other before
judging the hardware; it walks the candidates as a rotated *list* where the DUT
walks a modular index; its terminal classification is a sequence of named rules,
not the RTL's if-chain. It sizes itself from the DUT's readback ports and shares
no constant with the RTL.

**The replay experiment** (phase `refusal`) is what makes "a refusal has no side
effect" testable rather than asserted: a scripted stimulus is run once with a
request that cannot be served and once with that request never offered, and the
two state sequences are required to be identical from the refusal cycle onward.

## Real command output

### Base

```
$ python3 tools/run_unit.py --profile p0 --case lease.conflict_and_cancel
PASS lease.conflict_and_cancel    task=I-024
```

and the recorded result line, verbatim:

```
RESULT PASS lease.conflict_and_cancel lease contract holds: 4366 shadow comparisons over 4462 cycles, 3 ports / 4 records, seed 1; random: 1231 grants, 635 releases, 592 cancels, 1246 stale ids, 102 repeats, 116 cross-path, 1559 contended cycles, 639 same-cycle terminal+grant, 26 all-pools-full cycles
```

`result.json`: `"verdict": "PASS"`, `"checks": 10`, `"failures": 0`, exit 0.
The coverage numbers are in the recorded result rather than only in an assertion
that would have to fail before anyone could see them.

### Mutants

Each mutant is a `ifdef` block in the shipping source, off in the shipping build,
built with its `-D` appended to the runner's flags and run at the full
200000-cycle budget against this **green** base.

| Mutant | Defect injected | exit | verdict | checks (Δ) | first named failure |
| --- | --- | --- | --- | --- | --- |
| *(base)* | none | 0 | **PASS** | 10 | — |
| `PARTIAL_GRANT` | the FU class is dropped from the all-or-none test: the request is granted while the FU it named is unavailable | 1 | FAIL | 3 (−7) | `partial: cycle 62: req_ready[1] is 1, the contract says 0` |
| `PARTIAL_GRANT_WB` | *redundancy probe*, not a defect — the credit class is dropped: passes **identically** to the base | 0 | PASS | 10 (±0) | — (see below) |
| `REFUSAL_CONSUMES` | a refused request still takes a lease record, and with it the slots picked for it | 1 | FAIL | 3 (−7) | `partial: cycle 54: o_occ slot 0 of pool alu is 1, the ledger says 0` |
| `GRANT_BEFORE_TERMINAL` | this edge's arbitration does not see the credit this edge's terminal returns | 1 | FAIL | 2 (−8) | `conflict: cycle 41: req_ready[1] is 0, the contract says 1` |
| `DOUBLE_RELEASE` | the second release of one lease is accepted as if it were the first | 1 | FAIL | 5 (−5) | `terminal-errors: cycle 92: rel_ok is 1, the contract says 0` |
| `STALE_ACCEPT` | the generation is dropped from the release test | 1 | FAIL | 4 (−6) | `cancel: cycle 77: rel_ok is 1, the contract says 0` |
| `CANCEL_LEAK` | a cancelled lease keeps its reservation | 1 | FAIL | 4 (−6) | `cancel: cycle 75: o_occ slot 0 of pool alu is 1, the ledger says 0` |

**Delta.** The base is green, so `failures` can only go 0 → 1 and carries no
information; the informative delta is *where the run stops* — the check count and
the phase/cycle in the first mismatch. With a green base a vacuous mutant is
impossible to miss, because a mutant that does not change behaviour **passes**
(the row above proves it: `PARTIAL_GRANT_WB` passes with an identical count).
That is the stronger form of the project's rule that an exit code against a red
base is not evidence.

### The mutant that was vacuous, and why it is in the table anyway

`PARTIAL_GRANT` in its first form dropped the **credit** term, and it passed with
exactly the base's numbers. The cause is a property of the design, not of the
test: the ledger is sized by the credit pool and every live lease holds exactly
one credit, so "no record free" and "no credit free" are the same fact at every
geometry this derivation can produce. The credit term in the test is therefore
**implied**, and no mutant can distinguish it. Rather than delete the row or
pretend it was evidence, the probe is kept under its own name and the redundancy
is stated in the module header. The credit term stays in the shipping build
because it *is* the contract statement for that class — a request that names a
credit must find one, and the slot is reserved and returned under that name — and
because it is the line that would have to be right if a later profile let one
lease hold a different number of credits. What is not claimed is that it is
independently falsifiable today.

The falsifiable version of the same defect (the FU class dropped) was written
instead and is caught in the mirrored half of the partial phase.

## Findings

Four defects were found while building this package, and in three of them the
*testbench* was wrong rather than the DUT. They are recorded because the next
lane pays for them again otherwise.

**1. A combinational block reading another block's output was scheduled stale
(RTL structure).** The occupancy derivation, the terminal classification and the
arbitration were originally three `always_comb` blocks, with a fourth packing
`grant_id` out of the arbitration's `grant_rec_c`. In that build the arbitration
selected record 1, the ledger agreed, `o_occ` agreed — and the `grant_id` port
still reported record 0, one cycle behind. The evidence was the case's own
per-cycle comparison, not inspection: the internal signals were correct and the
port was not. Merging every combinational function of the ledger into one
`always_comb` removed the dependency and the mis-scheduling with it. Nothing in
the RTL's *logic* changed; only which block it lives in.

**2. A helper that took its width as an argument returned "not found" for an
empty pool (RTL).** `lowest_free(occupied, width)` reported `found = 0` with
`occupied = 4'b0000`, at three consecutive `eval()`s, while the generated C++ for
that function read correctly on inspection and the structurally identical
one-argument `lowest_free_record` worked. The shipping build no longer uses a
width argument at all: a slot a pool does not have is marked *not free* in the
free bitmap, so the scan is a plain lowest-free-bit search that knows nothing
about pool sizes — which is also the better contract (the pool's size is a
property of the data, not a parameter the scan must be told). **I did not
establish the root cause** and I am not claiming a tool bug; I can only report
that the two-argument form produced a wrong answer at runtime under Verilator
5.052 and that the one-argument form does not.

**3. The shadow was wrong; the DUT was right (testbench).** The randomised phase
found, at cycle 633, a cycle with a release and a cancel for the *same* id: the
shadow reported `can_repeat`, the DUT reported `can_after_release`. The
classification in `Eval` consulted the *registered* end-kind array after an
accepted release rather than the working view, so it saw how the record's
*previous* epoch had ended. The contract is explicit ("release wins, the cancel
is reported as `can_after_release`"), the DUT implements it, and the shadow was
fixed to carry the same working view the DUT's `end_w` carries.

**4. The harness decoded every id as record 0 (testbench).** The geometry was
read from the DUT after an `eval()`, but two derived widths (`row_w`, `idx_w`)
were computed *after* the harness and the shadow had taken their copies. A zero
index width made `Field(raw, gen_w, 0)` return 0, so the driver reported
`grant_id[1] is (0,g0)` while the port held `0x20000`. Fixed by computing the
whole geometry before either object is constructed.

Two smaller testbench corrections in the same family, recorded because each cost
a run: a rejection is *supposed* to increment its report counter, so "the state
must not change" compares the ledger and not the report counters; and the
deliberately-unreset arrays must not be compared across a reset.

## Not verified

* **Only the p0 geometry.** The driver asserts the p0 numbers (3 ports, 4
  classes, 4 records, sizes 2/1/4/2), so another profile fails loudly instead of
  silently testing something narrower — but a different geometry is untested.
* **The generation ABA bound.** A terminal delayed across `2**GEN_W` = 128 grants
  *to the same record* would alias onto the current epoch and be accepted. The
  wrap phase exercises the wrap and shows the DUT and the ledger agree across it,
  but the design does not claim to reject that case, and the test does not
  construct a terminal that old.
* **No real requester exists yet.** The module is exercised standalone against its
  observation ports. "A requester raises exactly one terminal per grant" is a
  caller contract this module detects violations of but cannot enforce; the
  operand collector, result FIFO and network credit plumbing that will drive the
  request and terminal ports are later packages (I-025/I-026/I-028).
* **Reconfiguration.** FREEZE/DRAIN/REVOKED/INSTALL is not implemented; reset is
  cold. A reset that preserves in-flight work is not verified (and is not
  claimed).
* **Fairness is observed, not proven.** The conflict and random phases show a
  loser being served and contention being widespread (1559 contended cycles), but
  no starvation-freedom bound and no formal liveness proof is offered.
* **One terminal per kind per cycle.** The module takes one release and one
  cancel per cycle and defines what happens when both name the same lease.
  Arbitrating several simultaneous terminals from different requesters is the
  caller's business and is not tested.
* **Synthesis and timing.** The module is not in a hand-maintained synthesis
  filelist (the generated one picks it up), and `make synth-generic` — a
  project-wide target — was not run by this lane. No area or Fmax is claimed.
* **Nothing about `mosaic_id_pkg.svh`.** The module does not include it and so
  neither exercises nor contributes to the identity contract; that the header is
  now guarded was observed, not tested here.

## Corrections owed

The `PARTIAL_GRANT` mutant was written, built and run before the matrix showed
it passing with the base's exact numbers — the same trap this project has paid
for three times, and the first matrix run is what caught it here. It is in the
table as a *redundancy* row rather than as evidence, and the reason it cannot be
evidence is now a statement in the module header rather than a note in a report.
The falsifiable version of the same defect (the FU class dropped) was written
afterwards, and the case catches it by name.
