# MosaicRV scalability plan — configurable width, depth and unit counts

Status: **requirement recorded 2026-10-01 at the user's direction**; implementation is work in
progress. This document is an addition to the specification set, not a change to it:
`docs/implementation-plan.md`, `docs/validation-plan.md`, `docs/architecture-review.md` and the three
root research reports are frozen and are not edited here.

## 1. The requirement, in the user's words

> "our RISC-V processor must be configurable on scalable aspect … I'm allowed to change Fetch/decode
> queue, ROB, allocations/renaming, width of decode, number of INT ALU, FP ALU, VEC ALU, memory
> Load/Store, those components in largely configurable ways. Right now, for validation, we will
> consider … proof of concept compare to XiangShan Kunminghu v2. The ultimate goal is scalable for
> partial frontend and scalable dynamic backend, and try to beat KunMingHu v2 processor."

So three levels of ambition, and they must not be confused with one another:

1. **Configurable** — every listed dimension is a *configuration input*, not a source edit, and an
   illegal combination is rejected by the config gate rather than discovered in simulation.
2. **Scalable** — a parameter change must preserve the architectural contract: the same program
   produces the same retirement stream and the same signatures at every legal configuration. That is
   the property `V-060`'s card already demands for lanes ("a lane-count change must not alter
   `VLEN`/architectural state") generalised to width, depth and unit counts.
3. **Better than Kunminghu V2** — a *research* goal, and it is only meaningful at matched
   configuration. See §4, which is the part of this document that keeps the project honest.

## 2. What is configurable today, and where

`config/geometry/<profile>.json` is the single source, and `tools/gen_manifest.py` emits
`build/<profile>/rtl/mosaic_cfg_pkg.svh` and `mosaic_id_pkg.svh` from it. A parameter that reaches the
RTL through those generated packages is a real knob; a parameter that reaches the RTL as a literal is
a source edit wearing a configuration's clothing.

| Dimension the user named | Where it lives today | State |
|---|---|---|
| Fetch outstanding requests | `frontend/fetch_outstanding` | **config-driven** (4) |
| Branch predictor / BTB / RAS sizes | `frontend/bpu_entries`, `btb_entries`, `ras_entries` | **config-driven** |
| Decode width | — | **hard-coded** (the decoder produces one control word per cycle) |
| Fetch/decode queue depth | — | **hard-coded** (8 entries, `mosaic_idec_queue.sv`, added this week) |
| Rename / allocation width | `rename/rename_width`, `rename/dispatch_width` | **config-driven** (2, widened this week) |
| ROB depth | `rob/entries` | **config-driven** (64) |
| Commit width | `MOSAIC_RETIRE_WIDTH` | generated, but **not yet a geometry key** |
| Physical registers (int) | `int_prf/entries`, `banks`, `ports_per_bank` | **config-driven** (96 / 4 / 1) |
| Clusters | `fabric/clusters` | **config-driven** (2) |
| Issue queue depth | `fabric/iq_entries_per_cluster` | **config-driven** (8) |
| INT ALU count | `fabric/alu_per_cluster` | **config-driven** (1) |
| MUL/DIV count | `fabric/mul_div_units` | **config-driven** (1, shared) |
| FP ALU count | — | **hard-coded** (one `mosaic_fp_unit`, one FP pipe) |
| Vector ALU / lane count | — | **hard-coded** (one vector engine; the lane broker treats lanes as an attribution, not parallel datapaths) |
| Load/store units | `lsu/units` | **config-driven** (1) |
| Load / store queue depth | `lsu/lq_entries`, `lsu/sq_entries` | **config-driven** (8 / 8) |
| L1 geometry | `caches/line_bytes`, `l1i_sets`, `l1i_ways`, `l1d_sets`, `l1d_ways` | **config-driven**, enlarged to 8 KB per side this week |
| MSHR entries | `caches/mshrs` | **config-driven** (4) |
| PMP entries / granularity | `pmp/entries`, `pmp/granularity_bytes` | **config-driven** |

**Acceptance for level 1**: for every row above, changing the value in `config/geometry/*.json` and
regenerating must elaborate, pass lint (both tools) and run the corpus without any other source edit;
and `tools/check_profile.py`'s negative controls must reject a geometry that is internally impossible
(an ROB smaller than one macro, a rename width larger than the decode width, zero load units with a
nonzero load queue, a line size smaller than the access width, and so on).

## 3. Constraint rules the generator must enforce

A parameter set is not free: some combinations are illegal, and the project's rule is that an illegal
configuration is *rejected by the config gate*, not discovered as a simulation symptom. At minimum:

* `rename_width >= 1`, `decode_width >= rename_width` (you cannot rename what you cannot decode);
* `rob/entries >= max_uops_per_macro`, and `>= 2 * rename_width` for any configuration claiming
  two-wide allocation to be useful;
* `commit_width >= 1` and `<= 2 * max_uops_per_macro`;
* every `int_prf/entries >= arch_int_regs + rob/entries` (the free list must be able to cover the
  whole window — the project has already hit this rule once, in the bank-bias work);
* `lsu/lq_entries >= lsu/units`, `lsu/sq_entries >= lsu/units`;
* at least one of `fabric/alu_per_cluster` or `fabric/mul_div_units` non-zero, and one cluster;
* `caches/line_bytes >= 8` and a power of two, with `caches/l1i_ways <= caches/l1i_sets`.

## 4. The comparison with Kunminghu V2 — and the rule that keeps it honest

Reference: the XiangShan user guide's *Typical Configurations* for Kunminghu V2
(<https://docs.xiangshan.cc/projects/user-guide/en/kunminghu-v2/typical-configuration>), read
2026-10-01. Its published typical configuration:

| Kunminghu V2 | value | MosaicRV today (p1) |
|---|---|---|
| Pipeline stages | 13 | ~8 (not yet characterised) |
| Decode width | 6 | 1 |
| Rename width | 6 | 2 |
| Commit width | 8 | 2 |
| ROB | 160 | 64 |
| RAB (branch buffer) | 256 | 16 (RAS) / predictor BHT 1024 |
| Physical registers (Int / FP / Vector) | 224 / 192 / 128 | 96 / — / — |
| Load queue / store queue | 72 / 56 | 8 / 8 |
| Issue queues (Int / FP / Mem) | 24×4 / 18×3 / 16 | 8×2 (INT) / — / — |
| L1I / L1D | 64 KB / 128 KB (configurable) | 8 KB / 8 KB (this week) |
| L2 / L3 | 512 KB–1 MB 8-way / 2–16 MB | none |
| INT execution | 4 ALU (2 with MUL/BKU) + 4 BJU | 1 ALU × 2 clusters + 1 shared MUL/DIV |
| FP execution | 5 pipes (2 FDIV) | 1 |
| Memory execution | 3 LDU + 2 STA + 2 STD | 1 load/store port, 1 outstanding transaction |
| Vector execution | 5 pipes | 1 engine, lanes are an attribution |
| Load latency (to use) | 4 | 1-cycle L1 hit, ~8 cycles per blocking miss |
| Integer MUL / DIV | 3 / 4–19 | 1-cycle MUL/DIV with iterative divide |

**The honest rule, which is a precondition for publishing any comparison at all:**

1. **Absolute-IPC claims are only valid at matched configuration.** Compare our generator instantiated
   at Kunminghu-like widths (decode 6, rename 6, commit 8, ROB 160, L1 64 KB, ≥ 4 ALU) against
   Kunminghu's published numbers, on the same workload, the same toolchain and the same measurement
   definition. A 2-wide, 64-entry, 256-byte-cache machine compared against a 6-wide, 160-entry,
   64 KB-cache machine measures the parameters, not the design.
2. **At unmatched configurations the only defensible metrics are normalised ones**: IPC per ROB
   entry, IPC per kilobyte of L1, instructions retired per issued request, or work per cycle at equal
   *area* once an area model exists (it does not yet). Those are the numbers this project may publish
   before it can run a matched comparison.
3. **Nothing in this document authorises a claim about Kunminghu.** The comparison target is a
   *configuration to instantiate and measure against*, and the deliverable is a table with the exact
   commands, the workload, the toolchain version and the measurement definition — the same standard
   `I-084` already set ("explicit latency/backpressure and licence provenance", "pure IPC is not a
   signoff").
4. **What "beat" could honestly mean**: not absolute IPC on this process and timeline. It could mean
   (a) better IPC at the *same* width/ROB/cache on a specified workload class, (b) better
   performance-per-configuration — the same architectural result with fewer ROB entries or kilobytes
   of cache, which is the plan's own "equal resources" idea, or (c) the properties Kunminghu is not
   built for: dynamic per-hart resource ownership with unchanged architectural state (I-031/I-059)
   and lane-count-independent vector state. Those are research contributions; they are testable;
   none of them is "we are faster".

## 5. Work items

| id | item | acceptance |
|---|---|---|
| S-1 | Make decode width a geometry key and generate the decoder's control-width from it | a 2-wide and a 6-wide decode elaborate and pass the corpus with identical retirement streams |
| S-2 | Make the decoded-instruction queue depth a geometry key | depth 8 → 32 with no source edit; the wide-allocation path stays correct at both |
| S-3 | Make commit width and FP/VEC unit counts geometry keys | a configuration with two FP pipes and two vector pipes elaborates and passes the FP/vector suites; the FP/VEC issue ports are generated, not literal |
| S-4 | Add the generator's constraint rules of §3 with negative controls | `check_profile --negative` rejects each listed impossible combination by name |
| S-5 | Characterise a *Kunminghu-like* configuration of this generator and publish the table | the workloads, the toolchain pin, the measurement definition and the numbers, with the honest-normalisation rule of §4 stated on the same page |
| S-6 | Pipeline-stage and Fmax characterisation is explicitly **not** claimed here | recorded as not measured, with the reason (no synthesis/PnR flow on this machine; `tools/synth_check.py` is blocked by Yosys 0.9x parsing, and H-package territory) |
| S-7 | Keep every configuration lint-clean and style-conformant | `lint_rtl` and `slang-tidy` clean at each configuration the generator can emit; no assignment patterns, no interfaces, explicit widths, `logic` |

## 6. Why this document says "not yet" rather than a number

The measured facts as of this writing, all from cases in the tree: the machine is **fetch-bound**
(the fetch unit answers on 22–25 % of cycles, so allocation runs at 0.2/cycle and the two-wide path
never engages); the memory path was **blocking and single-outstanding** with a 256-byte L1 while the
configuration already declared far more, which this week's work corrected to 8 KB per side plus a
wired MSHR; and there is **no L2, no L3, no SMT, one outstanding memory transaction and no
non-blocking I-cache** — the four things Kunminghu's numbers rest on. Any comparison published
before those exist would be a comparison of specifications, and this project's whole method is that
a claim without a command behind it is not a claim.
