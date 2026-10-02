# I-056 — vector memory packetizer

Work package **I-056** of `docs/implementation-plan.md` §3.1 (`V-056`), implementing

> 每 element 保留 logical index、地址、byte mask、fault ownership；先无 coalescing
> 的正确路径，再生成 line requests；测试跨 page/line/权限边界。
> Outputs: vector LSU descriptors、`CASE=rvv.memory_modes`.
> Pass: 每活跃 element 的地址/数据/异常属于正确 macro，ordered-indexed 的顺序要求
> 不被网络破坏。
> Fail: 一次 line merge 跨越不等价 permissions/MMIO，或漏实现 segment/whole-register
> 模式仍通告 V。

## STATUS: COMPLETE (unit); NOT WIRED INTO THE CORE

| Acceptance item | Evidence |
| --- | --- |
| The packetizer exists as RTL, driving the I-053 VRF from the I-052 snapshot | `rtl/core/mosaic_vec_lsu.sv` (new); `sim/tb/mosaic_vec_tb.sv` instantiates it beside `mosaic_vrf` + `mosaic_vec_alu` and wires its configuration to `cfg_snap_*` |
| Every item carries logical index, address, byte mask, fault ownership | `mem_req_elem_o`/`mem_req_field_o`, the beat address + `mem_req_wmask_o`, and the same pair echoed on `mem_rsp_elem_i`/`mem_rsp_field_i`; the case compares all of them against the host oracle |
| Faults are element-granular with `vstart` = the faulting index | phase `fault-position`: a fault at element 0, 1, 3, 5 of 6; `o_trap_elem_o` is the faulting index, the elements before it took effect and the ones at/after it did not |
| No merge crosses unequal permissions or MMIO | phase `device-straddle`: a byte-granular unit-stride run crosses a boundary at 0x105 inside one 8-byte beat; every request's enabled bytes lie in one region, ram=5 device=3, and the request count is the item count |
| Ordered-indexed ordering is enforced and observable | phase `ordered-order`: the ordered form keeps at most one request outstanding against a 6-cycle response; the accepted `(elem, field)` sequence is ascending on the memory port |
| Coverage asserted, not implied | 61 cells, every one of the 8 modes run and asserted at the end (`coverage: mode <m> did not run`); the cell count is itself a check |
| Fail criterion — segment/whole-register unimplemented while V advertised | both are implemented and run in both directions; whole-register covers NFIELDS 1, 2, 4 and 8 (the spec's supported set); the capability gate refuses any mode whose bit is clear with no memory transaction |
| Fail criterion — a merge across permissions/MMIO | `MOSAIC_VEC_LSU_MUTANT_MERGE_CROSS_REGION`, exit 1 on `a request spans two regions` |
| Fail criterion — a whole-macro fault, or wrong `vstart` | `MOSAIC_VEC_LSU_MUTANT_WHOLE_FAULT`, exit 1 on `vstart 0 expected 1` |
| Fail criterion — a wrong byte mask for width/position | `MOSAIC_VEC_LSU_MUTANT_MASK_WRONG`, exit 1 |
| Fail criterion — an ordered access reordered | `MOSAIC_VEC_LSU_MUTANT_ORDERED_REORDER`, exit 1 on `a second ordered request was issued before the first completed` |
| `python3 tools/run_unit.py --profile p0 --case rvv.memory_modes` | `RESULT PASS rvv.memory_modes checks=2350 modes=8 cells=61 cycles=3893`, exit 0, sha256 `c9718a193a5af084b12cc3b68a20da1ccd45b4d498b3b0d68849dcb07e9c944f` |
| The same binary at seeds 2, 7 and 12345 | PASS, identical 2350 checks and 61 cells |
| `verilator --lint-only -Wall` on the module, shipping and all four `-D` controls | clean under every definition |
| `slang-tidy --std 1800-2017` on the module and the case's file set | exit 0 (only the tree-wide STYLE advisories, the same ones `mosaic_vec_alu`/`mosaic_vrf` carry) |
| `python3 tools/run_vec_lsu_controls.py` | 4 of 4 mutants mutate the binary, exit 1 and name the check they break |
| Sibling cases unchanged | `rvv.integer_mask_permute`, `rvv.descriptor_legality`, `rvv.vset_boundaries`, `rvv.vtype_layout`, `vrf.mapping_aliases`, `core.corpus_sweep` PASS |
| ACT4 | `RESULT PASS core.act_dut applicable=127 generated=127 run=127 passed=127 failed=0` (p1) |
| This report | — |

## The registry entry

`tests/unit/registry.json` already carried the case, naming `mosaic_vec_lsu.sv`,
`sim/tb/mosaic_vec_tb.sv` and `sim/unit/tb_vec.cpp`. The module was created at
exactly the registered path; the wrapper and the driver were extended. The
registry was **not** edited — the `"pending": true` marker is the integration
lead's to clear when the package is recorded.

## The mechanism, as a rule

One vector memory macro is a sequence of **items**: one element for every mode
except the segmented ones, and one (element, field) pair for those. The unit
walks the items in ascending order, reads the vector operands it needs from the
banked VRF through one read slot, writes a load's destination through the write
port, and offers exactly **one memory request per item**. It owns no storage but
a small load write-back queue, a lane FIFO and a loaded bitmap.

* **configuration** — `cfg_vtype_i` / `cfg_vl_i` / `cfg_vstart_i` are the
  snapshot `mosaic_vec_cfg` hands out. SEW/LMUL/vta/vma are decoded from `vtype`
  at the ratified positions (vsew 5:3, vlmul 2:0, vta 6, vma 7) and from nowhere
  else. `vl` is used as the config unit clamped it (to VLMAX).
* **the memory port** — one request per item. `mem_req_addr_o` is the
  8-byte-aligned beat holding the element's lowest byte, `mem_req_wmask_o` the
  byte enables relative to it (`(1<<EEW/8 - 1) << lane`), `mem_req_wdata_o` the
  store data shifted into its lane, and `mem_req_elem_o`/`mem_req_field_o` the
  logical index the request belongs to. A response carries the same index/field
  pair back, which is how a fault is attributed.
* **element addressing**, one rule per mode (EEW in bytes):
  * unit stride `base + elem*EEW/8`
  * constant stride `base + elem*stride`
  * indexed `base + offset[elem]`, the offset zero-extended to XLEN, its EEW the
    instruction's `eiN` suffix and its EMUL `(EEW/SEW)*LMUL`
  * segment unit `base + (elem*NFIELDS + field)*EEW/8`; segment strided
    `base + elem*stride + field*EEW/8`; segment indexed
    `base + offset[elem] + field*EEW/8`
  * whole register `base + j*EEW/8`, `j` in `[0, NFIELDS*VLEN/EEW)`; the group is
    addressed with EMUL = log2(NFIELDS) (NFIELDS is 1, 2, 4 or 8, the spec's
    supported set)
  * mask `base + b`, `b` in `[0, EVL)`, `EVL = ceil(vl/8)`, EEW=8, EMUL=1,
    `vstart` in bytes
* **no coalescing**. The first correct version asks for exactly the bytes of one
  element, so the request count is the item count. That is what makes a merge
  observable, and it is why a line merge crossing a boundary cannot happen by
  construction.
* **tail policy** — a load's unloaded destination elements follow vta/vma
  (masked-off uses vma, `index >= vl` uses vta, below `vstart` is unchanged),
  applied in a post-pass over a 128-bit loaded bitmap after every response has
  been drained. Whole-register loads fill their group; the mask load is
  tail-agnostic (`0xFF` above EVL).

## The fault-ownership rule

Faults are **element-granular**, and the rule is stated once in the module
header:

> The packetizer issues items in ascending logical order. When a request
> completes with `mem_rsp_fault_i`, the macro stops: `o_trap_o` asserts with
> `o_trap_elem_o` = the faulting element index (the value the restart writes to
> `vstart`); no item at or after the faulting one is issued; a store at or after
> the faulting element is not performed (the response is a fault, so the memory
> side withholds the write); a load's destination elements at or after the
> faulting element are left untouched; and the elements strictly before it have
> already taken effect -- their stores were performed or their loads written
> back -- with any of their responses still queued drained before the unit
> retires the fault.

A whole-macro trap that reports no element index, or reports `vstart` = 0 for a
fault at element k, is not acceptable for a unit-stride or strided load. Phase
`fault-position` drives a fault at element 0, 1, 3 and 5 of a six-element macro,
in both directions, and checks `o_trap_elem_o`, that exactly the requests before
the fault were issued, that the stores before it reached memory while the
faulting one and later did not, and that the load's destination before the fault
was written while the faulting element and later were left as they were.

## Ordering and the network

The ordered forms (`vluxei`/`vsuxei`, `vluxseg`/`vsuxseg`) must not issue element
i+1 before element i completes. This unit implements:

* **ordered forms strictly** — at most one request outstanding; the next item is
  not offered until the previous response has arrived;
* **unordered forms pipelined** — up to `OUT_MAX = 2` outstanding loads, because
  a load has no memory side effect and the ordering freedom is real;
* **every store strictly**, ordered or not, because a store that faults must not
  have a later store already committed.

Phase `ordered-order` runs the plain indexed form in both directions with a
6-cycle memory response, records the accepted `(elem, field)` sequence on the
memory port and requires it ascending, and for the ordered form requires that no
second ordered request was accepted while one was outstanding.

## The device-straddling access

Phase `device-straddle` places a permission/MMIO boundary at `0x105` — *inside
the 8-byte beat `0x100`* — and runs an 8-element byte-granular unit-stride
access from `0x100` in both directions. The host memory model holds the region
map, so the check does not trust the packetizer's own notion of a region:
every request's enabled bytes must lie wholly in one region, elements `0x100`
through `0x104` must reach RAM and `0x105` through `0x107` the device, and the
request count must be the item count (no merge). With one request per element
the property holds by construction; the case proves it, and the
`MERGE_CROSS_REGION` control is caught by the explicit "a request spans two
regions" assertion.

## The mode coverage table

61 cells ran, every one comparing the whole request stream and (for a load) the
whole destination against the host oracle; the 8 modes are asserted present at
the end. `vl` is clamped to the configured VLMAX, exactly as the I-052 unit
clamps it.

| phase | mode | direction | configurations |
| --- | --- | --- | --- |
| device-straddle | unit | load + store | byte boundary inside beat 0x100 |
| byte-mask | unit | load | sew 8/16/32/64 at lanes 0,2,4,0 |
| byte-mask | strided | load | sew 16 stride 6, sew 32 stride 12 |
| byte-mask | indexed | load | sew 32, offsets at odd lanes |
| fault-position | unit | load + store | fault at element 0, 1, 3, 5 of 6 |
| ordered-order | indexed | load | ordered and unordered, latency 6 |
| capability-gate | all 8 | load | the mode's bit cleared: refused, no request |
| coverage | unit | load + store | sew 8/16/32/64, LMUL 2 and 1/2, vma/vta 0/1 with a mask, page-cross (0x0FF8, sew 64), line-cross (0x103C, sew 8) |
| coverage | strided | load + store | sew 16 stride 6, sew 32 stride 12 |
| coverage | indexed | load + store | `ei16`, `ei32` |
| coverage | seg-unit | load + store | NFIELDS 2 (sew 8), NFIELDS 4 (sew 32) |
| coverage | seg-strided | load + store | NFIELDS 2 (sew 16, stride 8) |
| coverage | seg-indexed | load + store | NFIELDS 2 (sew 32) |
| coverage | whole | load + store | NFIELDS 1, 2, 4, 8 at sew 64 |
| coverage | mask | load + store | `vl` 16 and 8 (EVL 2 and 1) |

The `masked-tail-agnostic`, `masked-vma1` and `masked-tail-undisturbed` rows
check that a masked-off or tail destination element is left as it was (vma/vta
0) or written all-ones (vma/vta 1), and that no request is issued for it.

## The host oracle's identity

`PlanLsu` in `sim/unit/tb_vec.cpp` computes each item's `(elem, field, address,
byte mask)` from the V specification's address rules — the formulas above,
taken from §7.2–7.9 of the pinned tag `~/mosaic-ref/riscv-v-spec`, the same
revision the descriptor cites. The port presents the beat address and the
lane-relative byte mask, so the comparison is `g.addr == want.addr & ~7` and
`g.mask == ((1<<EEW/8)-1) << (want.addr & 7)`. The register file is modelled as
32 × 128-bit values addressed with the same `(base, element, SEW, LMUL)` rule the
VRF implements, so a store's payload and a load's destination are checked as
element sequences, not merely as values. The memory model is a byte array with a
region map, a per-request response latency and a fault injector keyed by
`(element, field)`.

## The mutant table

`python3 tools/run_vec_lsu_controls.py --profile p0` — the shipping build is
built first from an empty directory and required to pass, then each mutant is
built from its own empty directory with its `-D` on the recorded command line.

| mutant | injects | sha256 | first failing check |
| --- | --- | --- | --- |
| shipping | — | `c9718a193a5af084b12cc3b68a20da1ccd45b4d498b3b0d68849dcb07e9c944f` | — (PASS) |
| `MOSAIC_VEC_LSU_MUTANT_MERGE_CROSS_REGION` | a request's byte mask absorbs the bytes of the earlier items that fell in the same beat, so a request names bytes on both sides of a boundary the elements themselves do not cross | `5504740c7f12571e70c466b445ca35100fefeba4aec31dc44c027ba28115b153` | `lsu unit load device-straddle req5: a request spans two regions` |
| `MOSAIC_VEC_LSU_MUTANT_WHOLE_FAULT` | a fault is reported with `vstart` reset to zero instead of the faulting element's index | `5b2f99851636a0ed85729442f032b704e6672fcfb8243b705215de490a3db416` | `fault-position load at 1 of 6: vstart 0 expected 1 (whole-macro trap or wrong element)` |
| `MOSAIC_VEC_LSU_MUTANT_MASK_WRONG` | the byte mask names the first byte of the beat and the whole width, not the element's bytes at its own width and position | `3371b5fedae1baf9c02dfdee22f5ec6f31141ed39c1b97ed4554fac2b691f3b9` | `lsu unit load device-straddle req0: a request spans two regions` (and 24 `byte-mask` failures in its own phase) |
| `MOSAIC_VEC_LSU_MUTANT_ORDERED_REORDER` | an ordered indexed access takes the unordered ordering gate, so the next element is issued before the previous one completed | `02580ce1ca9c23f7c1b561f9d4f43d4b0302095037c4614ef96b18f595136b4f` | `ordered-order ordered: a second ordered request was issued before the first completed` |

Each mutant's binary differs from the shipping one (`cmp`), and each exits 1.
The mask mutant is first caught by the device phase because a full-width mask
names bytes on both sides of the boundary — the same region property that
catches the merge — and its own `byte-mask` phase also fails; the report states
the actual first failure rather than the intended one.

## Not covered — stated deliberately

A package that claims more than it implements is the card's own fail mode, so:

* **The unit is not wired into the core.** `mosaic_core.sv`,
  `mosaic_dispatch.sv` and the cache files are untouched: no decoded vector
  memory instruction reaches this unit, and the descriptor is not driven from a
  real decode. Until that integration exists the vector capability is not an
  advertisement.
* **Translation and PMA are not performed here.** The packetizer drives a byte
  address on a memory port and takes the fault from the response. It has no TLB,
  no PMP and no PMA table; binding it to the I-046/I-043 translation path is
  separate work.
* **Misaligned elements are not implemented.** The port carries one 8-byte beat
  per request, and every element the case uses is naturally aligned. An element
  whose bytes leave its beat (`addr % EEW/8 != 0`) would need a two-beat split;
  that is not implemented and is not covered.
* **Vector AMO (`vamo*`) is not implemented**, and there is no capability bit for
  it, so it cannot be advertised.
* **Fault-only-first (`vleff`/`vsse`-style reduction of `vl`) is not
  implemented**; it belongs with I-057.
* **`vstart` restart is not exercised as a restart.** A non-zero `vstart` is
  implemented (the walk begins at it and the prestart elements are unchanged),
  but no phase re-executes from a saved `vstart`; the restart controller is
  I-057.
* **Segment NFIELDS 3, 5, 6, 7 and 8** are implemented (the per-field register
  addressing handles any count with EMUL*NFIELDS <= 8) but the case covers only
  NFIELDS 2 and 4 for the segment forms. Whole-register is restricted to
  NFIELDS 1, 2, 4, 8 by the specification and all four are covered.
* **Ordered indexed-segment (`vluxseg`/`vsuxseg`)** is implemented through the
  same `exec_ordered_i` path but the case only exercises the ordered property on
  the plain indexed form.
* **The unordered store forms are issued strictly.** This is a design choice,
  stated and implemented, not an accident: a store that faults must not have a
  later store already committed, so the partial-completion guarantee needs issue
  order. A future coalescer (I-061) may relax it with the same guarantee.
* **Sample number/`how`/prestart of the mask transfer** is modelled as EVL =
  ceil(vl/8) with `0xFF` tail; the tail-agnostic value is a legal choice, not the
  only one, and the case accepts the ones value it implements.
* **Throughput is out of scope.** The unit processes one item at a time with a
  two-deep load pipeline; the lane quota and the operand collector are I-059 /
  I-026.

## Tree state at the time of writing

`rtl/core/mosaic_vec_lsu.sv` and `sim/tb/mosaic_vec_tb.sv` are this package's
files. The other lane's `mosaic_cache*.sv`, `mosaic_l1_cache_path.sv` and the
cache wiring in `mosaic_core.sv` were not touched; `make lint-slang`,
`tools/lint_rtl.py` (p0 and p2), `python3 tools/check_records.py`,
`python3 tools/check_exclusions.py` and `--negative` (13/13 illegal ledgers
rejected) and `make check` are all green.
