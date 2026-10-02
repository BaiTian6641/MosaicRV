# The same-cycle second insert is held (V-013)

**Verdict: the two-wide *allocation* half of the front-end width package ships;
the same-cycle second *insert* is held — deleted, not ifdef'd — because it
triggers a retirement-order defect in `retire.width_and_order` (V-013). The
classification experiment says the defect lives in the **retire/ROB lane-1 path**
(I-017's territory) that the insert exposes, not in the insert's own identity.**
A held feature with a named failing case is honest; a shipped feature with a
failing case is a broken machine with a green dashboard.

RTL touched: `rtl/core/mosaic_dispatch.sv` (the deletion). Driver touched:
`sim/unit/tb_core_perf.cpp` (one check relaxed to a report, with the reason in
the comment). `rtl/core/mosaic_retire.sv` was edited **temporarily** for the
classification experiment and reverted — `git diff` for it is empty.

## 1. The isolation, reproduced

All builds are from a deleted directory, using `tools/run_frontend_controls.py`'s
own builder over the `retire.width_and_order` registry entry (so the source list
is the registry's own), then run as
`<binary> --case retire.width_and_order --out <dir> --seed 1 --max-cycles 4000000`.

| # | build | binary sha256[:16] | exit | RESULT |
|---|---|---|---|---|
| 0 | shipping (insert present) | `6cc93743ff927039` | 1 | `FAIL … cycle 527: event 110 pc: expected 0x800001c0, got 0x80000014 (lane 1)` |
| 1 | `-DMOSAIC_DISPATCH_MUTANT_NO_INSERT` (allocation kept, second insert removed) | `9823609441081ae4` | 0 | `PASS checks=1195 events=119 retires=118 comparisons=973 dual=28 single=63 cycles=573` |
| 2 | `-DMOSAIC_DISPATCH_MUTANT_NO_PAIR` (allocation and insert both removed) | `c44c62b8820cafc4` | 0 | `PASS checks=1195 events=119 retires=118 comparisons=973 dual=28 single=63 cycles=573` |
| 3 | allocation removed, **insert kept** | `e665bdd72b3d2b7d` | 1 | `FAIL … cycle 528: event 110 pc: expected 0x800001c0, got 0x80000014 (lane 1)` |

Build 3 needed a one-line temporary edit, because the stock
`MOSAIC_DISPATCH_MUTANT_NO_PAIR` removes the insert *and* the allocation: the
`elsif MOSAIC_DISPATCH_MUTANT_NO_PAIR` arm of the `head1_fire` chain was
temporarily renamed so the define zeroed `wide_pair` alone. The edit was reverted
before the deletion and is not in the delivered tree.

**Conclusion, unchanged from the author's record and now reproduced on this
tree:** the depth-8 decoded queue and two-wide *allocation* do not break V-013;
the **same-cycle second insert is the trigger**. Build 3's failure is the same
identity as build 0's (`pc 0x80000014 id 0x3bcc seq 110 lane 1`), one cycle
later because allocation is one-wide.

## 2. The classification experiment (the decisive result)

**Question:** with the insert kept, does removing the retire-side two-wide lane
make V-013 pass? The answer decides whether the defect is in the insert's
identity or in the retire lane it exposes.

**Constructing the 1-wide retire build.** The intended route — a throwaway
profile whose generated `MOSAIC_RETIRE_WIDTH` is 1 — is **not constructible**.
`tools/gen_manifest.py` was run over profile `p0` with only
`geometry.rename.dispatch_width` patched to 1 (in memory; nothing under `config/`
was modified), which writes `MOSAIC_RETIRE_WIDTH = 1` correctly, but the build
fails:

```
%Warning-WIDTHTRUNC: rtl/core/mosaic_core.sv:6476:8: Input port connection 'pay_valid'
    expects 1 bits on the pin connection, but pin connection's REPLICATE generates 2 bits.
%Warning-SELRANGE:  rtl/core/mosaic_core.sv:6975:30: Selection index out of range: 1:1 outside 0:0
%Error: Exiting due to 26 warning(s)
```

`mosaic_core.sv` hardwires the two-wide retire connections (`{rob_head1_valid,
rob_head_valid}`, `ret_req[1]`, …), so a `RET_WIDTH` of 1 fails to elaborate
whether it comes from the generated package or from a local edit to
`mosaic_retire.sv` — the same 26 warnings. `mosaic_core.sv` is outside this
task's write scope, so the width-1 route was not taken.

**The constructible equivalent.** The retire-side two-wide lane was held inside
`rtl/core/mosaic_retire.sv` with a temporary local parameter that forces the
lane-1 request to zero, keeping the 2-wide interface (so `mosaic_core.sv`
elaborates):

```systemverilog
// TEMPORARY EXPERIMENT (reverted before delivery)
localparam bit RET_EXPERIMENT_LANE1_HELD = 1'b0;
...
req_q[i] = order_ok[i] && rob_valid[i] && gate_ready[i] &&
           pay_valid[i] && gate_flush_ok[i] &&
           (i == 0 || RET_EXPERIMENT_LANE1_HELD);
```

This is "remove the retire-side two-wide lane" stated at the one place that
decides it, and it was reverted (`git diff rtl/core/mosaic_retire.sv` is empty).

**Outcome** (binary sha256[:16] `86c526da628a61a3`, exit 1):

```
CHECK FAILED: the run retires two slots in one cycle (0 cycle(s))
RESULT FAIL retire.width_and_order contract violated: check: the run retires two slots in one cycle (0 cycle(s))
```

The **only** failure is the coverage requirement that the run produce a
dual-slot cycle — which a 1-wide retire lane cannot produce. **The
program-order comparison passed**: no event carried a wrong PC, and every model
event was published and compared. The failure this case was written to catch —
`event 110 pc expected 0x800001c0, got 0x80000014 lane 1` — is gone.

**Verdict: the defect is in the retire/ROB lane-1 path** (I-017's territory),
which the insert merely exposes. The insert's own identity — it carries the
ROB's `alloc2_index`/`alloc2_gen` — is **likely sound**; the classification does
not prove it byte-for-byte, but it removes the retire lane from the failing
signature and leaves a clean stream behind.

## 3. What was deleted, and its extent

Deleted from `rtl/core/mosaic_dispatch.sv` (a real deletion, no `ifdef`, no
stub):

* `head1_fire` — its declaration, the pop term, the two-slot queue shift, the
  insert counter term and the `pair_ctr` increment;
* the `c0_from_h1`/`c1_from_h1` operand muxing of the two cluster insert buses
  (both buses now carry the head, one of them driven by the head's affinity);
* the lane-1 PRF read block (the `h1_rd1`/`h1_rd2` writes into `prf_rd_*`);
* the lane-1 operand block (`bank_of_src1`, `h1_need_s1/s2`, `h1_b1/b2_free`,
  `h1_rd1/2`, `h1_s1/s2_ok`, `h1_s1/s2_rdy`, `h1_s1/s2_val`, `h1_ins_ok`,
  `ins_ready1`, `head1`/`head1_valid`, `head1_is_*`).

`grep -c "head1_fire\|c0_from_h1\|c1_from_h1"` goes **39 → 0**. The file goes
**2137 → 2025 lines** (−112 net; the deleted machinery is ~150 lines and the
header grew by ~38 lines of "held" text). The module header's opening, its two-wide
allocation section, the two-cluster-affinity paragraph, the depth rationale and
the stale "lane 1 cannot be allocated until mosaic_rob offers a second allocation
port" note were rewritten to describe the held state; a stale "Depth 4" claim
(depth is 8) was corrected.

**Kept, deliberately:** the depth-8 decoded queue (`DSP_DEPTH = 8`, the two-lane
push at `push_at`/`push_at1`), two-wide allocation (`wide_pair`, the `alloc2_*`
ports, `e1_new`), the lane-1 rename and ROB allocation ports, and every
`MOSAIC_DISPATCH_MUTANT_*` diagnostic define. The three defines whose only
subject was the insert (`NO_INSERT`, `H1_NO_CTRL`, `H1_ALWAYS_NOTREADY`) are kept
as documented **inert** guards at the point where the insert lived, so the
isolation builds in §1 remain nameable; a build that names one is byte-identical
to shipping. The other defines (`DROP_PAIR_TAIL`, `NO_PAIR`, `NO_INIT_CONST`,
`ALL_INIT_CONST`, `BANK_CONFLICT_VALUE`, `SINGLE_CLUSTER`) are untouched.

## 4. The amended occupancy/IPC table (allocation-only machine)

Same workloads, seed 1, `perf.equal_resource_compare` (p1), baseline
configuration. The full 7-configuration table is in
`results/unit/perf.equal_resource_compare/p1/run.log`.

```
workload alu_chain  cycles 3373  insns 538  IPC 0.159
  alloc/cyc=0.160 pair%=0 ins/cyc=0.160 occ=0.16/8 iq=0.31/16 iq_q=0.36/8 bar%=16 off%=0 l1%=1 frsp%=22 freq%=44 rec%=25 issue/cyc=0.130
workload stream     cycles 3458  insns 363  IPC 0.104
  alloc/cyc=0.105 pair%=0 ins/cyc=0.105 occ=0.59/8 iq=0.32/16 iq_q=1.50/8 bar%=32 off%=0 l1%=2 frsp%=19 freq%=39 rec%=24 issue/cyc=0.076
workload vec_stream cycles 1436  insns  68  IPC 0.047
  alloc/cyc=0.048 pair%=0 ins/cyc=0.048 occ=0.19/8 iq=0.00/16 iq_q=1.86/8 bar%=21 off%=0 l1%=15 frsp%=12 freq%=24 rec%=29 issue/cyc=0.041
workload pair_burst cycles 2777  insns 104  IPC 0.037
  alloc/cyc=0.038 pair%=0 ins/cyc=0.038 occ=4.30/8 iq=7.64/16 iq_q=4.59/8 bar%=30 off%=0 l1%=28 frsp%=7 freq%=15 rec%=15 issue/cyc=0.037
  same-cycle two-wide insert: pairs=0 alloc2=0 (reported, not required)
```

**Did the numbers move?** Yes, against the numbers printed in
`frontend-width.md` — and **not because of the hold.** The tree moved under this
measurement: a sibling lane's p1 change is in the working tree
(`config/geometry/p1.json`, L1I/L1D `256 → 32` sets and `2 → 4` MSHRs,
non-blocking), which is why `alu_chain` is 3373 cycles here against 2389 there.
The clean isolation is the same-tree comparison, insert present vs held (both
built from this tree's sources):

| workload | cycles insns IPC | insert present | insert held |
|---|---|---|---|
| alu_chain | 3373 / 538 / 0.159 | pairs=0 alloc2=0, occ=0.16 | pairs=0 alloc2=0, occ=0.16 |
| stream | 3458 / 363 / 0.104 | pairs=0, occ=0.58 iq_q=1.49 | pairs=0, occ=0.59 iq_q=1.50 |
| vec_stream | 1436 / 68 / 0.047 | pairs=0, occ=0.18 | pairs=0, occ=0.19 |
| pair_burst | 2777 / 104 / 0.037 | **pairs=3**, alloc2=0, occ=4.30 | **pairs=0**, alloc2=0, occ=4.30 |

So the hold's entire measured effect on the perf case is the loss of the **3**
same-cycle pair inserts on `pair_burst` (and a ±0.01 occupancy wobble). The
architectural result is identical (`arch_identical=yes`), and the binding
resource is still the instruction fetch path (`frsp% 7–22`).

## 5. The exact state the package now claims

* **Two-wide allocation and rename: implemented.** The pair is presented to
  rename and the ROB atomically when both decoded lanes are cluster-class. On the
  measured workloads it engages only when the front end hands dispatch two such
  macros in one cycle — with the insert held, it never engaged in the perf run
  (`alloc2=0`); with the insert present it engaged only on `stream`.
* **Depth-8 decoded queue: implemented and kept.**
* **Same-cycle second insert: HELD** (deleted). Not shipped.
* **The width half therefore ships as *allocation-only*.** The performance claim
  is unchanged and unfavourable: the front end is fetch-bound, and the width was
  never the binding resource.
* **V-013 passes** from a clean build on this tree.

## 6. Re-verification (all run after the deletion)

Every case was built from a deleted directory
(`rm -rf build/<profile>/unit/<case>` then `python3 tools/run_unit.py …`), so no
verdict below is a stale binary.

| Item | Command | Verdict |
|---|---|---|
| `retire.width_and_order` p0 | `run_unit.py --profile p0 --case retire.width_and_order` | **PASS** `checks=1195 events=119 retires=118 comparisons=973 dual=28 single=63 cycles=573` |
| `mmio.exactly_once` p0 | `run_unit.py --profile p0 --case mmio.exactly_once` | **PASS** `checks=69 runA_retires=34 runB_retires=39` |
| `mmio.exactly_once` p1 | `run_unit.py --profile p1 --case mmio.exactly_once` | **PASS** (same counters) |
| `core.act_dut` p1 | `run_unit.py --profile p1 --case core.act_dut` | **PASS** `applicable=127 generated=127 run=127 passed=127 failed=0` |
| `core.corpus_sweep` p1 | `run_unit.py --profile p1 --case core.corpus_sweep` | **PASS** `programs=13 inputs=3 runs=39 pass=39 fail=0` |
| `perf.equal_resource_compare` p1 | `run_unit.py --profile p1 --case perf.equal_resource_compare` | **PASS** `workloads=4 configs=7 arch_identical=yes` |
| `rename.same_cycle_chain` p1 | `run_unit.py --profile p1 --case rename.same_cycle_chain` | **PASS** `9767 shadow comparisons over 9843 cycles` |
| `frontend.width_buffering` p1 | `run_unit.py --profile p1 --case frontend.width_buffering` | **PASS** `workloads=4 configs=7 arch_identical=yes` |
| `rob.out_of_order_children` p0 | `run_unit.py --profile p0 --case rob.out_of_order_children` | **PASS** `1235977 shadow comparisons` |
| `core.corpus_branch` p0 | `run_unit.py --profile p0 --case core.corpus_branch` | **PASS** `checks=31 comparisons=9264 cycles=1846` |
| `iq.wakeup_insert_select` p0 | `run_unit.py --profile p0 --case iq.wakeup_insert_select` | **PASS** `93878203 checks over 199800 cycles` |
| `tools/run_frontend_controls.py` | as-is (p1) | **exit 1 — one control NOT CAUGHT, see §7** |
| `tools/lint_rtl.py --profile p0` | as-is | **clean: 65 source file(s)** |
| `tools/lint_rtl.py --profile p1` | as-is | **clean: 65 source file(s)** |
| `tools/check_records.py` | as-is | **ok: 80 delivered package(s), 96 registered case(s)** |
| `tools/check_exclusions.py` | as-is | **ok: 54 registered, 18 open, 36 covered** |

## 7. `run_frontend_controls.py` is red: `MOSAIC_ROB_MUTANT_SWAP_PAIR_ORDER` is no longer caught

This is a consequence of the hold, and it is reported rather than smoothed over.

```
shipping  hash=5d188ccffc81928f exit=0 PASS
NOT CAUGHT MOSAIC_ROB_MUTANT_SWAP_PAIR_ORDER  hash=d509d5b7570b65b1 exit=0
CAUGHT     MOSAIC_DISPATCH_MUTANT_DROP_PAIR_TAIL  hash=b8d1fc67332e7392 exit=1
CAUGHT     MOSAIC_RENAME_MUTANT_NO_BYPASS         hash=f54835e8e68b1f3d exit=1
controls exit=1
```

The mutant places a two-wide group's two macros in the wrong ROB slots, and it
fires **only when the ROB accepts a two-wide group** (`alloc2_ok`). With the
insert present the dispatch queue drains two macros in a cycle, which is what
made two-wide allocation engage at all on `stream` — and the mutant was caught
there (`MISMATCH +steering on stream retirement 313: expected the same
architecture as the baseline`). With the insert held the queue drains one per
cycle, allocation never engages on any perf workload (`alloc2=0`), and the mutant
is inert.

**Attribution, measured, not argued.** The same perf case was built twice from
this tree's sources, differing only in `mosaic_dispatch.sv`:

| dispatch | SWAP control |
|---|---|
| pre-deletion (insert present, byte-exact reconstruction, sha `6cc93743ff927039` on V-013) | `swap_pre` sha `3673fe440858f891`, exit 1 — **CAUGHT** |
| delivered (insert held) | sha `d509d5b7570b65b1`, exit 0 — **NOT CAUGHT** |

The mutant and its check are untouched; the control's *subject* disappeared with
the insert. `MOSAIC_DISPATCH_MUTANT_DROP_PAIR_TAIL` still catches (it is about
allocation), and the other two controls are unaffected. **The honest statement is
that holding the insert costs this control its coverage on the current
workloads**; restoring it needs either the insert or a workload on which
allocation engages while the insert is held, neither of which this task can
provide.

## 8. Not covered

* **The root cause of the retirement-order defect is not found.** The
  classification localises it to the retire/ROB lane-1 path (I-017's territory);
  it does not name the line. Fixing it is not this task.
* **The insert is not re-testable end-to-end.** With the insert held, no
  registered case exercises it; the only evidence for its behaviour is the
  isolation builds in §1 and the classification in §2.
* **The insert's identity is "likely sound", not proven.** The classification
  removes the retire lane from the failing signature; it does not prove the
  `(alloc2_index, alloc2_gen)` identity correct on its own.
* **The width never engages in the measured perf run** with the insert held, so
  the allocation-only machine's width behaviour is unexercised by
  `perf.equal_resource_compare`; the pair-order control is inert (§7).
* **A 1-wide retire *netlist* is still not constructible** (§2): `mosaic_core.sv`
  hardwires the two-wide retire connections.
* **No synthesis, no formal proof, no waveform.** The claims are about the finite
  stimuli above, on this revision.
