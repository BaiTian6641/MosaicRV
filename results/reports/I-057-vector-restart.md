# I-057 — vector partial trap, `vstart` and fault-only-first

Work package **I-057** of `docs/implementation-plan.md` §3.1 (`V-057`, `V-058`),
implementing

> 在每个合法 element boundary 注入 page/access fault 与 interrupt；保存允许的
> 部分状态，恢复重执行；覆盖 FOF 首元素 fault 与后续 fault/vl 缩短。
> Outputs: vector restart controller、`CASE=rvv.partial_fault_restart`.
> Pass: 已发生的合法 store/element 更新不被伪回滚，重启不重复不可逆副作用，
> trap/vstart/vl 与 spec 一致。
> Fail: 把整个 vector instruction 当标量原子事务，或仅最后 packet 到达便退休。

## STATUS: COMPLETE (unit); NOT WIRED INTO THE CORE

| Acceptance item | Evidence |
| --- | --- |
| The restart controller exists as RTL | `rtl/core/mosaic_vec_restart.sv` (new); `sim/tb/mosaic_vec_tb.sv` instantiates it beside the I-056 packetizer and the I-051 descriptor |
| The descriptor's element bitmap and fault progress are bound, not duplicated | the controller drives `elem_done_valid_i`/`elem_done_index_i` with a one-per-cycle walk over the committed elements and `fault_valid_i`/`fault_elem_i`/`fault_code_i` with the earliest fault; it reads back `o_elem_bitmap_o`/`o_prefix_o`, and its restart point is the bitmap's first clear bit (`o_restart_vstart_o`); `o_prefix_agree_o` states the bitmap and the contiguous prefix name the same element |
| A fault at **every legal element boundary**, both classes, for each claimed form | phase `fault-boundaries`: unit-load, unit-store, strided-load, strided-store × elements 0..5 × {page, access} = 48 cells; the per-form "all six boundaries seen" count is itself a check |
| A precise interrupt at an element boundary | phase `interrupt`: boundaries 0..5 of a unit-stride load; the packetizer stops at the boundary (`o_stopped_o`/`o_stop_elem_o`) and the controller traps with `vstart` = the first unperformed element, `vl` untouched |
| Fault-only-first: the first-element fault and a later-element fault | phase `fof`: element 0 traps with `vl` unmodified; elements 1..5 are absorbed with `vl` = the faulting index; a strided form and a store are **not** absorbed |
| A restart does not duplicate an irreversible side effect | phase `restart`: a unit store faults at element 0, 2 and 5; the case counts the memory writes **per byte** across the fault, the restart and the completion, and requires every byte written exactly once |
| A restart from `vstart` performs exactly the remaining elements | the request stream across the two runs is `0..k` then `k..vl-1` (the faulting element is re-issued because its store was withheld; the committed ones are not), and the restart issues exactly `vl-k` requests |
| Fail criterion — a whole-macro trap where a partial one is required | `MOSAIC_VEC_RESTART_MUTANT_WHOLE_TRAP`, exit 1 on `fault-boundary unit-load at 1 code page: controller trap=1 vstart=0 expected 1/1` |
| Fail criterion — a restart that re-performs a committed element | `MOSAIC_VEC_RESTART_MUTANT_REDO_COMMITTED`, exit 1 on `restart store fault at 2: restart point 0 expected 2` |
| Fail criterion — a FOF load that raises a fault instead of shortening `vl` | `MOSAIC_VEC_RESTART_MUTANT_FOF_TRAPS`, exit 1 on `fof later fault at 1 code page: trap=1 vl_write=0 vl_new=0 expected no trap and vl=1` |
| Fail criterion — a `vstart` off by one | `MOSAIC_VEC_RESTART_MUTANT_VSTART_OFF_BY_ONE`, exit 1 on `fault-boundary unit-load at 0 code page: controller trap=1 vstart=1 expected 1/0` |
| Fail criterion — retiring when only the last packet arrived | `MOSAIC_VEC_RESTART_MUTANT_EARLY_RETIRE`, exit 1 on `retire-gate: o_complete_o stood while the packetizer was busy …` |
| `python3 tools/run_unit.py --profile p2 --case rvv.partial_fault_restart` | `RESULT PASS rvv.partial_fault_restart checks=613 cells=48 interrupts=6 fof=12 restarts=3 cycles=2859`, exit 0 |
| The same binary at seeds 2, 7 and 12345 | PASS, identical 613 checks and coverage |
| `verilator --lint-only -Wall` on the module, shipping and all five `-D` controls | clean under every definition |
| `slang-tidy --std 1800-2017` on the module and the packetizer/descriptor | exit 0 (only the tree-wide STYLE-13 advisories the other vector files carry) |
| `python3 tools/run_vec_restart_controls.py --profile p0` | 5 of 5 mutants mutate the binary, exit 1 and name the check they break |
| Sibling cases unchanged | `rvv.integer_mask_permute`, `rvv.memory_modes`, `rvv.descriptor_legality`, `rvv.vset_boundaries`, `rvv.vtype_layout`, `vrf.mapping_aliases`, `core.corpus_sweep` PASS |
| ACT4 | `RESULT PASS core.act_dut applicable=127 generated=127 run=127 passed=127 failed=0` (p1) |
| This report | — |

## The files, and what each change is

* **new** `rtl/core/mosaic_vec_restart.sv` — the controller. It owns the walk
  over the elements that took effect, the fault-only-first decision, the
  architectural `vstart`/`vl`/trap outputs and the retire gate.
* **modified** `rtl/core/mosaic_vec_lsu.sv` — additive only: a boundary stop
  (`stop_i` → `stopped_o`/`stop_elem_o`) that a precise interrupt needs, and a
  4-bit fault class carried with the response (`mem_rsp_fault_code_i` →
  `trap_code_o`). The fault path, the request stream and every existing mode are
  unchanged; `rvv.memory_modes` and its four mutants still pass.
* **modified** `rtl/core/mosaic_vec_desc.sv` — additive only: `fault_clear_i`
  consumes the recorded fault (so a restart re-executes) without touching the
  element bitmap (so the committed prefix survives).
* **modified** `sim/tb/mosaic_vec_tb.sv` — the new controller instance, the
  packetizer pins, and a `rst_bind_i` mux that lets the controller own the
  descriptor's progress ports when the restart case runs.
* **modified** `sim/unit/tb_vec.cpp` — the `Lsu` harness gains the restart
  fields, the per-byte write counter and the five phases of
  `rvv.partial_fault_restart`; every addition is additive to the existing cases.
* **new** `tools/run_vec_restart_controls.py` — the five controls.

`tests/unit/registry.json` and `config/status/implementation_status.json` were
**not** edited. The case stays marked `"pending": true`; the integration lead
clears that marker when the package is recorded. (The entry's `rtl` list still
does not name `mosaic_vec_restart.sv`; Verilator resolves it from `-I rtl/core`,
which is how the sibling vector modules are found for cases whose lists omit
them, but the lead should complete the list when recording.)

## The restart rule, as a rule

A vector instruction is one architectural instruction and **not** an atomic
scalar transaction. The controller implements, from the pinned V spec
(tag `3570f998`, `v-spec.adoc`):

* **R1 — partial trap.** A synchronous fault at element `k` commits the elements
  strictly before `k` (their stores were performed and their loads written back
  by the packetizer), latches `vstart = k`, and leaves element `k` and every
  later element unperformed. *A whole-macro trap reporting `vstart` = 0 for a
  fault at `k` is the card's first fail mode* — `MOSAIC_VEC_RESTART_MUTANT_
  WHOLE_TRAP`.
* **R2 — no duplicate side effect.** The restart point is the first element *not*
  yet performed: `vstart` = `k`. The re-execution begins there, so a store that
  already wrote elements `0..k-1` is not written again. *Beginning the restart at
  0 duplicates an irreversible side effect* — `MOSAIC_VEC_RESTART_MUTANT_REDO_
  COMMITTED`.
* **R3 — fault-only-first.** Only a unit-stride load launched with `exec_fof_i`
  trims, and only for a fault it may absorb (a page or access fault). *See the
  boundary below.*
* **R4 — retire gate.** The macro is complete only after the packetizer reports
  `done_o` — every response drained — not when the last request has been offered.
  *Retiring on "only the last packet arrived" is the card's second fail mode* —
  `MOSAIC_VEC_RESTART_MUTANT_EARLY_RETIRE`.

## The `vstart` / `vl` semantics implemented

* `vstart` is the index of the first element to be executed. Every macro begins
  at the configuration snapshot's `vstart` (so a re-execution resumes where the
  trap left off) and a clean macro resets it to zero (the controller publishes
  `o_vstart_o` = 0 for a clean completion, `k` for a trap).
* On a trap the controller writes `vstart` = the faulting element's index and
  `o_trap_code_o` = the fault class. The elements before it are committed to the
  descriptor's bitmap; the faulting one and later are not.
* The restart point `o_restart_vstart_o` is the **first clear bit of the
  descriptor's element bitmap** — the first element not performed — and is only
  published (`o_restart_ready_o`) after the walk over the committed elements
  finished. `o_prefix_agree_o` cross-checks it against the descriptor's
  contiguous `o_prefix_o`.
* On a fault-only-first trim the controller writes `vl` = the faulting index
  (`o_vl_write_o`/`o_vl_new_o`); the destination element at the faulting index
  and later are left untouched, so re-executing with the new `vl` re-loads
  nothing that already succeeded. (The specification *permits* spurious updates
  past the trim point — "Load instructions may overwrite active destination
  vector register group elements past the element index at which the trap is
  reported", L1710-L1713 — and this case uses `vma = vta = 0`, so the expectation
  is "untouched".)

## The fault-only-first boundary

The pinned specification is exact (L1702-L1707):

> These instructions execute as a regular load except that they will only take a
> trap caused by a synchronous exception on element 0.  If element 0 raises an
> exception, `vl` is not modified, and the trap is taken.  If an element > 0
> raises an exception, the corresponding trap is not taken, and the vector length
> `vl` is reduced to the index of the element that would have raised an
> exception.

So the boundary this package implements is:

| element that faults | trap | `vl` | destination |
| --- | --- | --- | --- |
| element 0 | **taken**, `vstart` = 0 | **unmodified** | untouched |
| element `k` > 0 (page/access) | not taken | reduced to `k` | `[0,k)` loaded, `[k,vl)` untouched |
| element `k` > 0, any other class | taken, `vstart` = `k` | unmodified | untouched |
| a strided form, or a store | taken, `vstart` = `k` | unmodified | untouched |

An interrupt is never absorbed (L1753-L1755: "implementations should not reduce
`vl` and should instead set a `vstart` value"); the controller always raises the
trap for it.

**A deliberate divergence from the assignment's prose.** The task text said
"fault-only-first with the first element faulting and with a later element
faulting (`vl` shortening in both)". The specification — the pinned tag, the
current ratified manual, and the card's own "FOF 首元素 fault 与后续 fault/vl
缩短" — says the **first** element fault is *not* a shortening case: element 0
raises the trap and `vl` is unmodified. This package implements and tests the
specification, and the first-element case is covered as the trap it is. If the
intent was the draft behaviour (element 0 trims `vl` to 0), that is a different
rule and would need the card changed first.

## The coverage table (form × fault position)

48 boundary cells: 4 forms × elements 0..5 × {page fault, access fault}. The
coverage counters are asserted at the end of the case, so a form or a boundary
that never ran fails rather than being implied.

| phase | form | fault positions covered | classes |
| --- | --- | --- | --- |
| fault-boundaries | unit load | 0, 1, 2, 3, 4, 5 | page, access |
| fault-boundaries | unit store | 0, 1, 2, 3, 4, 5 | page, access |
| fault-boundaries | strided load | 0, 1, 2, 3, 4, 5 | page, access |
| fault-boundaries | strided store | 0, 1, 2, 3, 4, 5 | page, access |
| interrupt | unit load | boundaries 0, 1, 2, 3, 4, 5 | interrupt |
| fof | unit load, FOF | element 0 (trap) | page, access |
| fof | unit load, FOF | elements 1, 2, 3, 4, 5 (trim) | page, access |
| fof | strided load, FOF; unit store, FOF | element 3 (must trap) | page |
| restart | unit store | fault at 0, 2, 5 then a restart | page |

For every boundary cell the case checks `o_trap_o`/`o_trap_elem_o`/
`o_trap_code_o` from the packetizer, `o_trap_o`/`o_rst_vstart_o`/
`o_rst_trap_code_o`/`o_rst_vl_write_o`/`o_rst_fof_trim_o`/`o_rst_complete_o`/
`o_rst_retire_ok_o`/`o_rst_elems_committed_o` from the controller, the
descriptor's `o_prefix_o`/`o_fault_valid_o`/`o_fault_elem_o`/`o_fault_code_o`, and
the memory (for a store) or the destination (for a load) element by element.

## The host oracle's identity

The oracle is written in `sim/unit/tb_vec.cpp` from the specification's rules,
not read from the DUT. `HostRst(mode, we, fof, vl, vstart, fault_elem,
fault_code)` returns the expected `trap`, `vstart`, `trap_code`, `vl_write`,
`vl_new` and the committed element count, applying the sentences quoted above;
the case never derives an expectation from `o_elem_bitmap_o` or `o_prefix_o` — it
only *compares* the descriptor against the oracle's numbers. The store side
effect is measured by the harness's own byte array: `LsuMem::Apply` increments a
per-byte write counter for every byte a non-faulting store enables, so "written
exactly once across the fault, the restart and the completion" is observed on the
memory side rather than inferred from the request stream. The destination and
memory state are the host VRF and the byte array the case primed, indexed by the
specification's address rules (`base + e` unit, `base + e*stride` strided).

## The mutant table

`python3 tools/run_vec_restart_controls.py --profile p0` — the shipping build is
built first from an empty directory and required to pass, then each mutant is
built from its own empty directory with its `-D` on the recorded command line.

| mutant | injects | sha256 | first failing check |
| --- | --- | --- | --- |
| shipping | — | `5ec17d380486ca78e985bd45bfa4c64128056fea679da1d066944ec0bda20418` | — (PASS) |
| `MOSAIC_VEC_RESTART_MUTANT_WHOLE_TRAP` | a fault at element k is reported with `vstart` = 0, so the restart cannot resume (the card's first fail mode) | `acc1f39b348943e46519857b907e2d92b39c7a3243563b7ba76f8b12ce6758d2` | `fault-boundary unit-load at 1 code page: controller trap=1 vstart=0 expected 1/1` |
| `MOSAIC_VEC_RESTART_MUTANT_REDO_COMMITTED` | the restart point is forced to 0, so a re-execution re-performs an element that already took effect and duplicates a store side effect | `dda95c7ddcc1144460bf02f176d10243695d9dc71c7686709b01f52389e29e2c` | `restart store fault at 2: restart point 0 expected 2` |
| `MOSAIC_VEC_RESTART_MUTANT_FOF_TRAPS` | a fault-only-first load raises the fault on a later element instead of shortening `vl` | `a182dfed74d22265159f33a5df00846e0f00c9f52f4056105c5248d008b085c2` | `fof later fault at 1 code page: trap=1 vl_write=0 vl_new=0 expected no trap and vl=1` |
| `MOSAIC_VEC_RESTART_MUTANT_VSTART_OFF_BY_ONE` | `vstart` is the element after the faulting one, so the restart skips an element | `e4566777a475a9148a5a8a4b55b37107bab77348c99daa7857949722bc166414` | `fault-boundary unit-load at 0 code page: controller trap=1 vstart=1 expected 1/0` |
| `MOSAIC_VEC_RESTART_MUTANT_EARLY_RETIRE` | the macro is called complete when the last request has been offered, before the responses drained (the card's second fail mode) | `1bb9a5e00404920c5a229be16e676da3e1f7ab36ed0a8d6b7fde9bad6d72f71c` | `retire-gate: o_complete_o stood while the packetizer was busy -- the last packet had arrived, the responses had not drained` |

Each mutant's binary differs from the shipping one (`cmp`), each exits 1, and
each names the check its defect breaks. The `REDO_COMMITTED` mutant is caught by
the restart-point assertion *before* the write-count assertion it would also
break; the report states the actual first failure rather than the intended one.
Verilator's generated model embeds a source-line mapping, so a comment-only edit
to any file in the case's compilation changes the binary hash; the hashes above
are from the tree state at the time of writing, and the *shipping-vs-mutant*
inequality is the property the control checks.

## Not covered — stated deliberately

* **The vector unit is not wired into the core.** `mosaic_core.sv` and
  `mosaic_dispatch.sv` are untouched (zero `mosaic_vec` references in
  `mosaic_core.sv`): no decoded vector instruction reaches this controller, and
  the descriptor is not driven from a real decode. Until that integration exists
  the vector capability is not an advertisement.
* **Interrupts at an element boundary *are* implemented, but only as a unit
  handshake.** There is no core interrupt controller here: the case asserts a
  request (`rst_intr_i`), the controller asks the packetizer to stop at the next
  item boundary, and the packetizer reports the first element not performed. The
  core-level interrupt delivery, the `epc`/`mcause` write and the trap return are
  not part of this package.
* **`vstart` restart for the arithmetic path is not exercised here.** The
  controller restarts the *memory* macros; I-054's report notes that the
  arithmetic engine implements the `vstart`-gated active range but no phase
  drives a non-zero `vstart` into it. This package does not add one.
* **`vmsof`/`vmsbf`/`vmsif` with `vstart != 0`** must raise illegal-instruction
  per the specification. I-054 states the unit does not raise; this package does
  not implement that rule either — it belongs to the mask-prefix instruction
  path, not to the memory restart controller, and it is still open.
* **Only the unit and strided forms are swept for boundaries.** The packetizer
  implements every mode, but the boundary sweep claims the two addressing modes
  that the partial-trap rule is written for; indexed, segment, whole-register and
  mask forms are not swept here. Their `vstart` handling is inherited from the
  packetizer (which begins at `vstart`) and is covered by `rvv.memory_modes`
  without a fault.
* **The fault class is carried, not decoded.** The packetizer forwards four bits
  and the controller decodes only "page", "access" (absorbable by FOF),
  "interrupt" and "other". Translation, PMP and PMA — which produce the class in
  a real machine — are not in this unit.
* **Misaligned elements, vector AMO and throughput** remain as I-056/I-054 left
  them.
* **Spurious destination updates past a FOF trim** are permitted by the
  specification and are not produced here (`vma = vta = 0`); a case that accepts
  the permitted set would be a different check, not this one.

## Tree state at the time of writing

This package's files are `rtl/core/mosaic_vec_restart.sv` (new),
`rtl/core/mosaic_vec_lsu.sv`, `rtl/core/mosaic_vec_desc.sv`,
`sim/tb/mosaic_vec_tb.sv`, `sim/unit/tb_vec.cpp` and
`tools/run_vec_restart_controls.py` (new). `make check` (p2), `tools/lint_rtl.py`
(p0 and p2, 57 files clean), `python3 tools/check_records.py`,
`python3 tools/check_exclusions.py` and `--negative` (13/13 illegal ledgers
rejected), `rvv.integer_mask_permute`, `rvv.memory_modes`, `rvv.descriptor_
legality`, `rvv.vset_boundaries`, `rvv.vtype_layout`, `vrf.mapping_aliases`,
`core.corpus_sweep` and `core.act_dut` (p1, 127/127) are green.

`make lint-slang` is red at the time of writing on the **other** lane's file:
`rtl/core/mosaic_vec_fp.sv:542` (`plan_c` used before its declaration) and
`:678` (a `logic[4:0]` → `fp_op_e` implicit conversion). It is reported rather
than worked around; this package owns neither file. On this package's files
`slang-tidy` exits 0 with only the tree-wide STYLE-13 advisories.
