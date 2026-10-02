# I-061 — same-hart line coalescing

Work package **I-061** of `docs/implementation-plan.md` §3.1 (`V-062`), implementing

> 实现同 hart line coalescing。
> Inputs: per-element permissions/order/fault metadata、line transaction IDs。
> Action: 仅合并语义等价 normal-memory requests；返回保留各 element masks/faults；
> store 合并必须处理 same-address ordering；运行 CASE=coalesce.element_faults。
> Outputs: coalescer/return distributor、transaction reduction counters。
> Pass: coalesced/uncoalesced 下所有 defined values/faults 一致，重复地址 store 的
> 规则来自指令类型而非 lane 编号臆断。
> Fail: 隐藏每个 element 必需的访问检查或把 atomic/MMIO 合并。

## STATUS: DELIVERED (unit); ENABLED IN THE CORE

| Acceptance item | Evidence |
| --- | --- |
| The coalescer exists in `rtl/core/mosaic_vec_lsu.sv` and merges same-beat items into one request with a merged byte mask | `ST_CUR` accumulates a run of consecutive active items of one macro that share one 8-byte beat; `mem_req_wmask_o` is the union of the members' masks and `mem_req_addr_o` the shared beat |
| The return path distributes the beat back per element | `ST_DIST` hands a coalesced load's beat to each member one per cycle, straight to the VRF write port, so every element keeps its own mask and destination slot; a store's merged mask/data is applied per byte by the memory side |
| Only **normal memory** is merged; the device predicate comes from the *generated* platform map | `mosaic_cfg_pkg::mosaic_pa_normal` (emitted by `tools/gen_manifest.py` from each region's `kind`/`device`/`idempotent`); an MMIO/non-idempotent/uncovered byte is refused. Phase `device` drives a device macro whose elements share one beat and requires **no** merge, with the same shape in RAM merging as the contrast |
| Only semantically equivalent items share a request | the mergeability rule is a conjunction (not atomic, not ordered, unit/constant stride, every byte normal, store bytes disjoint from the group's); the *test* of equivalence is the case's identity check — the same macro run coalesced and uncoalesced defines the same values and faults |
| Per-element fault preservation (`vstart` restart correct) | a fault on a merged group **de-coalesces**: the group is replayed element by element in element order, so the elements before the faulting one are performed and the faulting one reports its own index. Phase `coalesce-fault` drives a fault at element 0/1/3/7 inside a merged group, both directions, and compares against the uncoalesced run |
| Repeated-address store order comes from the instruction type, not the lane | overlapping-byte stores are **never merged**, so the memory side sees them one at a time in element order. Phase `store-order` drives four stores to one address and requires the last element's value; the same rule is asserted in the `same-address` load row |
| Transaction-reduction counters | `req_ctr_o` (requests offered) and `merge_ctr_o` (elements a merge removed). Over the equivalence phase: **req_on=28, req_off=94** for the same 14 macros |
| `python3 tools/run_unit.py --profile p0 --case coalesce.element_faults` | `RESULT PASS coalesce.element_faults checks=675 cells=44 merged=12 req_on=28 req_off=94 cycles=4830`, exit 0, sha256 `c8fb7bfe558f2f6e897bf4bb73df302caaa906d0f44c686e466149cd1b0229cc` |
| The same binary at seeds 2, 7 and 12345 | PASS, identical `checks=675 cells=44 merged=12 req_on=28 req_off=94 cycles=4830` |
| The same case under profile p1 (a different generated map) | `RESULT PASS coalesce.element_faults` |
| `python3 tools/run_vec_coalesce_controls.py --profile p0` | 4 of 4 mutants mutate the binary, exit 1 and name the check they break (table below) |
| The I-056 controls are unaffected | `python3 tools/run_vec_lsu_controls.py --profile p0`: 4 of 4 still caught |
| `verilator --lint-only -Wall` on the module, shipping and all eight `-D` controls | clean under every definition (p0 include path) |
| `slang-tidy --std 1800-2017 --single-unit` on the tree | exit 0 (only the tree-wide STYLE advisories `mosaic_vec_lsu` already carried: its `always_comb` blocks are unnamed) |
| `make lint-cpp PROFILE=p0` | clean (72 files) |
| `make check`, `make check-records`, `make check-exclusions` (and `--negative`) | green |
| Sibling cases unchanged | `rvv.memory_modes`, `rvv.partial_fault_restart`, `rvv.integer_mask_permute`, `rvv.fp_flags_reduction`, `rvv.chaining_hazards`, `rvv.descriptor_legality`, `rvv.vset_boundaries`, `rvv.vtype_layout`, `vrf.mapping_aliases`, `rvv.lane_resize_boundary`, `rvv.mask_prefix_vstart`, `rvv.mask_prefix_semantics`, `vec.integrated` PASS |
| ACT4 (p1) with the coalescer enabled in the core | `RESULT PASS core.act_dut applicable=127 generated=127 run=127 passed=127 failed=0` |
| This report | — |

## The registry entry

`tests/unit/registry.json` already carried the case (`task: I-061`, top
`mosaic_vec_tb`, driver `sim/unit/tb_vec.cpp`, `pending: true`). Neither the
registry nor `config/status/implementation_status.json` was edited; the
`pending` marker is the integration lead's to clear when the package is
recorded.

## The mechanism, as a rule

The memory port's transaction is one **8-byte beat**. Coalescing merges a run of
consecutive items of one macro that fall in the same beat into one request with
the union of their byte masks, and distributes the response back to the members.
Because the port names at most 8 bytes, the beat is the largest unit one request
can cover; a 32-byte cache line is not reachable through this port and is not
claimed.

**Mergeability.** An item joins the pending group only when all of these hold,
and each clause is why the merged result equals the uncoalesced one:

* the macro is coalescing (`exec_coalesce_i`) and is **not** an atomic class
  access (`exec_atomic_i`) — an AMO is a serialization point;
* it is **not** one of the ordered indexed forms, whose per-element ordering
  contract a merged request would break;
* it is a **unit-stride or constant-stride** item (the indexed forms read a
  per-element operand and are left uncoalesced);
* every byte it enables is **normal memory** as the *generated platform map*
  defines normal (`mosaic_cfg_pkg::mosaic_pa_normal`, i.e. idempotent and not a
  device). An MMIO or non-idempotent byte, or an uncovered address, can never be
  merged. The predicate is the map's, never a hand-written list — the same
  discipline the cache (I-042) and the LLB (I-060) use;
* for a **store**, its bytes are **disjoint** from the group's.

The equivalence conditions the card names as "permission context and fault
class" are the macro's access class (load/store, size, ordered/unordered) plus
the map's per-address device/idempotency answer; items of different classes are
never in one macro, so they can never share a group. What makes this testable is
that "equivalent" is defined *operationally*: the case runs the same macros both
ways and requires identical defined values and faults.

**Return distribution.** The group descriptor — its members' logical index,
field and lane — rides an in-order FIFO beside the responses. A coalesced load's
beat is written to each member's destination slot one per cycle, straight
through the VRF write port; the write-back queue therefore only ever holds
size-1 responses and always has room, so a response can never be dropped. A
store's merged mask and lane-positioned data are applied per byte by the memory
side, exactly as the members' separate stores would have been.

**Fault preservation.** A merged request cannot say which of its members
faulted, so a fault on a group **de-coalesces**: the group is replayed element by
element, in element order, at most one request outstanding. The elements before
the faulting one are then performed individually (a store applied, a load written
back) and the faulting member reports its own index, so `vstart` is the element
that faulted and only the elements before it have taken effect. Hiding a
per-element access check is exactly what a group-level fault would do, and that
is the mutant `COALESCE_GROUP_FAULT`. A group that faults therefore removes
nothing from the request stream — its members are re-issued — which is why
`merge_ctr_o` counts a group only once it completes without a fault. (A fault
that were not a property of the addressed bytes would not reproduce element by
element; the fallback reports it at the group's first element rather than
dropping it. With the case's byte-granular memory the fallback is unreachable.)

**Repeated-address stores.** Two stores to the same address inside one macro
apply in the order the **instruction type** defines: element order. The ordered
forms require it; the unordered forms are implemented in element order too,
because that is the only order under which the coalesced and uncoalesced results
are identical. The order comes from the item sequence and the access class —
never from a lane number or from the order requests happen to complete — and the
coalescer never has to pick a winner, because overlapping-byte stores are simply
not merged. A disjoint store pair in the same beat *does* merge, which is what
makes the rule about the bytes rather than about the beat.

## The platform map

`tools/gen_manifest.py` now emits, per region, `MOSAIC_<NAME>_IS_DEVICE` and
`MOSAIC_<NAME>_IDEMPOTENT` (from the region's own `kind`/`device`/`idempotent`)
and three predicates: `mosaic_pa_device`, `mosaic_pa_idempotent` and
`mosaic_pa_normal = idempotent && !device`. The coalescer asks
`mosaic_pa_normal`; `mosaic_pa_cacheable` (I-042/I-060) is deliberately **not**
part of the predicate: the baseline profile advertises its RAM as uncacheable
(there is no cache in p0), yet that RAM is exactly the normal, idempotent memory
a coalesced line request is legal for, so gating on cacheability would refuse
the normal memory the card requires and coalesce nothing in p0. The map's
device/idempotency flags are what the card's "MMIO / non-idempotent" clause
means, and they come from the map, not from the RTL.

## The counters, coalesced against uncoalesced

`req_ctr_o` counts requests offered (a replay counts too); `merge_ctr_o` counts
the elements a merge removed. Over the case's equivalence phase, the same 14
macros (unit and constant stride, e8/e16/e32/e64, both directions, including
same-address and mid-beat runs):

| run | requests |
| --- | --- |
| coalescing off | **94** |
| coalescing on | **28** |
| reduction (`merge_ctr_o`) | **66** |

The case asserts, per row, that the reduction counter equals the fall in the
request count and that the uncoalesced run is exactly one request per item. Rows
where a merge is illegal (e64, stride 8) must show *no* reduction and a zero
counter, so a "coalescer" that merges indiscriminately fails as loudly as one
that never merges.

## The mutants

Each `-D` is rebuilt from a deleted build directory with its define on that
build's command line (recorded in the mutant's `build_command.txt`), the binary
hash must differ from the shipping one, the run must exit 1, and the first
failure must name the check it breaks. `python3 tools/run_vec_coalesce_controls.py
--profile p0` reports:

| mutant | injects | first failure | sha256 |
| --- | --- | --- | --- |
| `MOSAIC_VEC_LSU_MUTANT_COALESCE_MASK_LOSS` | a joining element's byte mask is dropped from the merged group | `coalesce unit e8 load: the coalesced byte mask is not the union of the elements' masks` | `751fdf1ce4b50b0029110e1dd5b8b3f989d3418fa5d772a18127bd6edd223d6f` |
| `MOSAIC_VEC_LSU_MUTANT_COALESCE_GROUP_FAULT` | a coalesced fault is reported for the group instead of being replayed element by element | `coalesce-fault unit e8 load at 1: vstart 0 expected 1 (a whole-group fault destroys element granularity)` | `6b499780c7151e16d7e0f57fbab6df161948c5ee76927a138681e9767cff18ec` |
| `MOSAIC_VEC_LSU_MUTANT_COALESCE_DEVICE` | the platform map is not consulted, so a device element is merged with its beat-mates | `device: 1 requests for 8 device elements sharing one beat (a device access was coalesced)` | `46f39d4030bf678f5061bc0d3348eb279ac0ae4ab9cd777864c2696584e9d794` |
| `MOSAIC_VEC_LSU_MUTANT_COALESCE_STORE_ORDER` | overlapping-byte stores are merged, so a repeated-address pair applies out of element order | `store-order same-address: 1 requests for 4 overlapping stores (an overlapping pair was merged)` | `6d87bbd4c39fa90be6f0a6eed1c5ae30338a76a67280a66ec4cf731b13b4651c` |

Shipping binary: sha256 `c8fb7bfe558f2f6e897bf4bb73df302caaa906d0f44c686e466149cd1b0229cc`.

The four I-056 mutants (`MERGE_CROSS_REGION`, `WHOLE_FAULT`, `MASK_WRONG`,
`ORDERED_REORDER`) are unchanged and still caught by `CASE=rvv.memory_modes`
(`python3 tools/run_vec_lsu_controls.py --profile p0`: 4 of 4).

## The case's phases

* **equivalence** — 14 macros, coalesced and uncoalesced, compared element by
  element against the uncoalesced run and against an oracle computed from the
  V-spec address rules; the merged byte mask must be the union of the members'
  masks; the reduction counter must equal the fall in requests.
* **coalesce-fault** — a fault at element 0/1/3/7 of a merged group (unit e8,
  unit e16, strided e32), both directions: `trap_elem` is the faulting element
  and matches the uncoalesced run, the elements before it took effect and none at
  or after it did.
* **device** — a device macro whose elements share one beat must not merge, with
  the same shape in RAM merging as the contrast; an atomic-class macro must not
  merge, with the non-atomic contrast merging; a device element among normal ones
  is its own request.
* **store-order** — four stores to one address apply in element order (last
  wins) and are not merged; a disjoint pair in one beat does merge.
* **coalesce-stop** — a precise-interrupt boundary stop taken while a group is
  pending: the boundary is inviolable (nothing at or after `stop_elem` took
  effect) and the coalesced run performs its pending group, which lies strictly
  below the boundary.
* **coverage** — 44 cells; both directions and both coalescable modes asserted.

## What this package found and did not fix

* **The coalescer is enabled in the core.** `mosaic_core.sv` instantiates
  `mosaic_vec_lsu` with `exec_coalesce_i = 1'b1` and `exec_atomic_i = 1'b0`, so
  `vec.integrated` and ACT4 run through it (both PASS). The vector memory port is
  not translated (no Sv39 in this path), so the addresses the map predicate sees
  are the ones the vector unit computes; that is the same physical-address
  assumption the I-056 path already made.
* **A probe on the I-057 stop path.** With the memory kept *ready* while the
  boundary stop is requested (not the pacing `rvv.partial_fault_restart` uses,
  which holds the memory un-ready so the outstanding response drains first), an
  **uncoalesced** load whose response is still in flight when the stop is taken
  is discarded rather than written back: `wf_push_c` is gated by `!abort_q`. The
  coalesced run performs its pending group. The case therefore asserts the
  inviolable boundary for both runs and the pre-boundary prefix only for the
  coalesced run. This is the I-057 stop path's behaviour, not the coalescer's,
  and is recorded here rather than silently fixed — it is out of this package's
  scope.
* **A first implementation of the return distributor pushed a coalesced load's
  members through the existing write-back queue.** Under back-pressure (the queue
  full and the VRF not draining in the same cycle) a size-1 response could then
  be refused and its data lost. The distributor was changed to write members
  straight to the VRF, so the queue only ever holds size-1 responses and always
  has room. This was caught by the case's `coalesce-stop` load row, and the fix
  is in the shipped revision.

## Not covered

* **Not coalesced, deliberately:** the indexed forms (ordered and unordered), the
  segmented forms, whole-register transfers and the mask transfer. Each is
  refused by the mergeability rule and the case does not extend to them; the
  card's coverage requirement is unit-stride and constant-stride, which are
  covered in both directions. The indexed forms keep one request per element.
* **Not a 32-byte line merge.** The memory port's transaction is one 8-byte beat,
  so the maximum merge is per beat. A cache-line (32-byte) coalescer would need a
  wider port and is not implemented or claimed.
* **No same-beat mixed normal/device case is constructible from the shipped
  maps.** Every region boundary in `config/memory/*.json` is 8-byte aligned, so a
  beat is wholly inside one region; the sharp same-beat device test (a whole
  device beat that would otherwise merge) is used instead, plus a strided macro
  with a device element among normal ones.
* **The boundary stop under coalescing is exercised by the case's
  `coalesce-stop` phase only at one pacing and one macro shape**; the I-057 stop
  semantics themselves are `rvv.partial_fault_restart`'s, and the divergence
  noted above is not addressed here.
* **No interaction with the LLB (I-060).** The LLB exists as a module but is not
  in the core's memory path, so the coalescer neither fills nor consults it. A
  future integration that puts the LLB in front of the vector port must decide
  whether a coalesced request is a single lookup; nothing here claims it.
* **Single hart only.** The card is same-hart coalescing; no cross-hart,
  coherence or QoS behaviour is implemented (that is I-062/I-069's scope).
* **No performance claim.** The case measures requests, not cycles or energy; the
  coalescer keeps one request in flight while it is enabled (a merged load's beat
  is distributed one member per cycle), which trades some concurrency for the
  distribution.
* **The case does not drive a masked (`v0`-masked) coalesced access.** The
  mergeability rule requires an item to be active, and the I-056 case covers the
  mask semantics; a masked coalesced run is not in this case's matrix.

## How to reproduce

```
python3 tools/run_unit.py --profile p0 --case coalesce.element_faults
python3 tools/run_unit.py --profile p1 --case coalesce.element_faults
python3 tools/run_vec_coalesce_controls.py --profile p0
python3 tools/run_vec_lsu_controls.py --profile p0          # I-056, must stay green
python3 tools/run_unit.py --profile p1 --case core.act_dut  # 127/127
make check PROFILE=p0
make lint-slang PROFILE=p0
make lint-cpp PROFILE=p0
python3 tools/lint_rtl.py --profile p0
python3 tools/lint_rtl.py --profile p1
```
