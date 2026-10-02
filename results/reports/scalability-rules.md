# S-4 — the configuration constraint rules, and the keys the schema now declares

Package: **S-4** of `docs/scalability-plan.md` (plus the schema work §2 needs).
Scope: `tools/check_profile.py`, `tools/mosaic/config_check.py`,
`config/schema/geometry.schema.json`, this report. **No RTL is changed by this
package.** It changes no profile's effective geometry either: the new keys are
declared *optional* in the schema and are absent from `p0..p3`, which is the
point (see "Absent means the RTL's literal").

Commands behind every claim below:

```
python3 tools/check_profile.py --all          # all four profiles accepted
python3 tools/check_profile.py --negative     # every control rejected, named
```

## 1. The rules, and why each impossible combination is impossible

The rules are implemented in `_check_geometry` (`tools/mosaic/config_check.py`),
under the heading *"the scalable-axis constraint rules"*. Each message names the
key or keys it is about. The rationale is the reason the combination cannot be
made to work, not merely the fact that it is refused.

| # | Rule | Why the combination is impossible |
|---|---|---|
| R1 | `rename.rename_width >= 1` | A rename stage of width zero allocates no physical register on any cycle; it is not a pipeline stage, and every downstream structure sized from `rename_width` would be zero-width. |
| R2 | `frontend.decode_width >= 1` and `>= rename.rename_width` | You cannot rename more uops than the decoder delivers: a rename wider than the decode has ports that are starved on every cycle, so the extra width is nominal hardware, not throughput. (Decided only when `decode_width` is declared — see §4.) |
| R3 | `rob.entries >= rob.max_uops_per_macro` | This machine allocates one ROB entry per *macro* (a 128-bit vector operation is one macro with a progress bitmap, not one entry per element). If the ROB were smaller than the largest macro, that macro could never be buffered whole and its allocation could never complete. |
| R4 | `rob.entries >= 2 * rename.rename_width` | The plan's two-wide allocation only pays off if the window holds at least two full allocation cycles. A ROB smaller than that is filled by the allocator in fewer than two cycles, so the second allocation port never has room and the width claim is nominal — exactly the failure §6 of the plan documents ("the two-wide path never engages"). |
| R5 | `rob.commit_width >= 1` | A machine that cannot retire an instruction on any cycle makes no forward progress; retirement is the only visor of committed state. |
| R6 | `rob.commit_width <= 2 * rob.max_uops_per_macro` | Retire bandwidth beyond twice the largest macro exceeds what the front end can ever present in a window, so the extra retire lanes are unexercisable — dead alignment logic on the critical architectural boundary. |
| R7 | `int_prf.entries >= ARCH_INT_REGS + rob.entries`, i.e. `int_prf.entries - 32 >= rob.entries` | The PRF is allocated from a free list; the rename undo journal holds one mapping per in-flight allocation. If there are fewer allocatable tags than ROB entries, the journal overflows on a correctly-squashing machine. This is the rule the bank-bias work already hit once; it existed before S-4 and gains its first negative control here. |
| R8 | `lsu.lq_entries >= lsu.units` and `lsu.sq_entries >= lsu.units` | A load or store unit with no queue entry can never be occupied: every unit needs at least one slot to be in flight. The extra unit is otherwise dead hardware. (Independent of the pre-existing "at least two entries" rule.) |
| R9 | `fabric.clusters >= 1`; and `fabric.alu_per_cluster >= 1` **or** `fabric.mul_div_units >= 1` | Zero clusters is no issue fabric to schedule onto. Both integer unit counts zero is a fabric that can never issue or complete a uop. The plan states this as a disjunction; today the schema's own per-key minimum of 1 makes it equivalent to "both positive" (see §4, finding F2). |
| R10 | `caches.line_bytes >= 8` | Eight bytes is the widest naturally aligned RISC-V access; a line narrower than one access cannot hold it, so every access would straddle a line with no way to place it. |
| R11 | `caches.line_bytes` is a power of two | Set indexing and refill beating assume a bit-slice of the address; a non-power-of-two line needs a divide or an explicit mask, and the mask would drop bytes. |
| R12 | `caches.l1i_ways <= caches.l1i_sets` (data side likewise) | More ways than sets leaves part of the tag array unreachable by any index: the set index has fewer values than there are ways, so some way can never be selected. |

`power_of_two()` and the geometry docstring already existed; R1/R3–R12 are new,
R7 is the pre-existing rule now covered by a control.

## 2. Negative controls, with the observed messages

`tools/check_profile.py --negative` mutates a throwaway copy of the real config
one key at a time and requires the gate to reject it. Each control also declares
the key names (and, where the schema *also* rejects the value, a distinctive
phrase from the semantic message) that must appear in the problems; a rejection
for some *other* reason is itself a failure. The controls now run for **every
profile** (`p0` alone declares no caches, so the cache rules would otherwise have
no control on the default run).

Result: `61/61` (p0), `52/52` (p1), `48/48` (p2), `43/43` (p3) rejected.

| Rule | Control | Observed rejection (semantic message; the schema's own message may also presence — both are required to name the key) |
|---|---|---|
| R1 | rename width below one | `geometry: rename.rename_width=0 is not at least 1; a rename stage of width zero allocates no physical register and is not a pipeline stage` |
| R2 | decode width narrower than the rename width | `geometry: frontend.decode_width=1 is narrower than rename.rename_width=2; you cannot rename more uops than the decoder delivers, and the extra rename ports would be starved every cycle` |
| R3 | ROB smaller than one macro | `geometry: rob.entries=8 cannot hold one macro of rob.max_uops_per_macro=9 uops; the largest macro the front end can present would never fit, so allocation of it could never complete` |
| R4 | ROB smaller than two allocation cycles | `geometry: rob.entries=2 is less than two allocation cycles (2 * rename.rename_width=4); …` (schema `geometry$.rob.entries: value 2 < minimum 8` also fires) |
| R5 | commit width below one | `geometry: rob.commit_width=0 is not at least 1; a machine that can never retire an instruction makes no forward progress` |
| R6 | commit width beyond twice the macro width | `geometry: rob.commit_width=17 exceeds 2 * rob.max_uops_per_macro=16; …` |
| R7 | PRF cannot cover the ROB window | `geometry: int_prf.entries=95 leaves 63 allocatable tags after 32 architectural registers, fewer than rob.entries=64; …` |
| R8 | load queue smaller than the load units | `geometry: lsu.lq_entries=2 is smaller than lsu.units=4; a load unit with no queue entry can never be occupied, so it is dead hardware` |
| R8 | store queue smaller than the store units | `geometry: lsu.sq_entries=2 is smaller than lsu.units=4; a store unit with no queue entry can never be occupied, so it is dead hardware` |
| R9 | zero clusters | `geometry: fabric.clusters=0 is zero; there is no issue fabric to schedule onto` |
| R9 | no integer execution unit in the fabric | `geometry: fabric.alu_per_cluster=0 and fabric.mul_div_units=0 are both zero; a fabric with no integer execution unit can never issue or complete a uop` |
| R10 | cache line narrower than eight bytes | `geometry: caches.line_bytes=4 is smaller than 8, the narrowest RISC-V access width a line must hold` |
| R11 | cache line is not a power of two | `geometry: caches.line_bytes=24 is not a power of two; set indexing and refill beating would need a divide or an explicit mask` |
| R12 | L1I has more ways than sets | `geometry: caches.l1i_ways=8 exceeds caches.l1i_sets=4; more ways than sets leaves part of the tag array unreachable by any index` |
| R12 | L1D has more ways than sets | `geometry: caches.l1d_ways=8 exceeds caches.l1d_sets=4; …` |

**Positive controls.** Declaring the new keys at legal values must still be
accepted: a copy of p0 with `decode_width=2`, `idec_queue_entries=8`,
`commit_width=2`, `fp_units=1`, `vec_units=1` loads `ok`; and the boundary
`commit_width=16 == 2 * max_uops_per_macro` with `decode_width == rename_width`
loads `ok`. So the rules reject the impossible and admit the legal.

## 3. Keys newly declared in the schema (declared, not yet consumed)

Added as **optional** properties in `config/schema/geometry.schema.json`; the
`description` of each says in full that it is *declared, not yet consumed by the
RTL*. None is in a profile yet, and none is in a schema `required` list.

| Key | RTL literal when absent | Consumer package |
|---|---|---|
| `frontend.decode_width` | 1 — `mosaic_decoder` produces one control word per cycle | **S-1** |
| `frontend.idec_queue_entries` | 8 — `CORE_DBUF_DEPTH` in `mosaic_core.sv` | **S-2** |
| `rob.commit_width` | `rename.dispatch_width` — the value `gen_manifest.py` gives `MOSAIC_RETIRE_WIDTH` | **S-3** |
| `fabric.fp_units` | 1 — one `mosaic_fp_unit`, one FP pipe | **S-3** |
| `fabric.vec_units` | 1 — one vector engine; the lane broker treats lanes as attribution, not parallel datapaths | **S-3** |

### Absent means the RTL's literal, not zero

The plan's intent is preserved literally: `rob.get("commit_width",
rename["dispatch_width"])` is exactly how `gen_manifest.py` derives
`MOSAIC_RETIRE_WIDTH` today, so an absent `commit_width` is the RTL's literal,
not a zero that would fail R5. For the axes whose literal the gate cannot model
from the document (`decode_width`, unit counts) the rule is decided only once the
axis is declared, because a *declared* value is the only thing a configuration
gate can be asked about. **Finding F1** is the direct consequence of the honest
reading of that literal:

* **F1 — the shipping profiles' effective decode width is below their rename
  width.** §2 records decode width as hard-coded at one control word per cycle,
  while `rename_width` is 2. If `decode_width` were declared as that literal (1),
  R2 would reject `p0..p3`. R2 is **not** weakened: any configuration that
  declares `decode_width < rename_width` is rejected (the control above proves
  it), and by declaring the axis S-1 inherits a rule that is already correct. The
  measured consequence of the current literal — "the two-wide path never
  engages" — is §6 of the plan. **This is a finding about the configuration, not
  a reason to weaken the rule.** It cannot be closed by editing a profile without
  contradicting "p0..p3 keep their current effective geometry"; it closes when
  S-1 makes `decode_width` a consumed key and the profiles declare their real
  width.

* **F2 — the plan's unit-count disjunction is not independently satisfiable
  today.** R9 is stated as "at least one of `alu_per_cluster` / `mul_div_units`
  non-zero", but the schema requires each to be at least 1, so the both-zero case
  is rejected by the schema as well as by R9. S-4 does not relax the per-key
  minimums (that would admit a zero-ALU cluster the RTL cannot build); it records
  the disjunction explicitly so a future change that relaxes one minimum keeps
  the other non-zero.

## 4. Not covered / not claimed

* **This package changes no RTL.** The width and unit-count keys are *declared
  only*: `decode_width`, `idec_queue_entries`, `commit_width`, `fp_units` and
  `vec_units` do not reach a generated package or a module parameter yet.
  Making the **RTL-side width keys real — decode width (S-1), decoded-queue
  depth (S-2), commit width and FP/VEC unit counts (S-3) — is out of scope here**
  and is owned by those follow-up packages. Until they land, an absent key means
  the RTL's literal documented in §3, and R2 is decided only for a declared
  `decode_width`.
* **The vector-lane-count axis is not a rule here.** The lane counts
  (`lane_groups_*`, `max_eu64_per_group`) already have their own consistency
  checks in `_check_geometry`; §3 names no cross-key constraint for them, and
  none is invented.
* **R4 and R11 are currently entailed by the schema bounds** (`rob.entries >= 8`,
  `rename_width <= 2`; `line_bytes ∈ {32, 64}`). They are written as rules because
  they are distinct properties that the bounds will be widened past in S-1..S-3,
  and the controls prove the rule fires (their distinctive messages appear among
  the problems) even where the schema also rejects the value.
* **The schema's `line_bytes` enum `{32, 64}` is narrower than R10/R11.** Values
  the plan's "≥ 8 and a power of two" would admit (16, 128) are still rejected by
  the enum, deliberately: the cache RTL supports only 32- and 64-byte lines
  today. This is strictness, not a weakened rule; it is the same reason the
  renamed profile must not declare a line the RTL cannot build.
* **No generator consumption.** `tools/gen_manifest.py` is unchanged; it still
  derives `MOSAIC_RETIRE_WIDTH` from `rename.dispatch_width`. Wiring
  `rob.commit_width` into it is S-3. The constraint gate derives the same
  literal so the rule is checked against what the generator emits.
* **Gate status as observed at this revision.** `check_profile --all` and
  `--negative` are green; `check_records` is green; `check_contracts` and
  `check_event_contract` pass (they ran inside `make check`). Two project gates
  were **already red from sibling work in flight, not from this package**:
  `check_exclusions` reports `frontend.width_buffering` and
  `locality.integrated_path` as unregistered exclusions, and `lint_rtl --profile
  p0` fails on `mosaic_multihart.sv` because `mosaic_core.sv` gained debug ports
  the multihart wrapper does not connect. Neither file is touched here, and both
  belong to the lanes that own those edits.
