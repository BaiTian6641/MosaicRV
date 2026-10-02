# I-053 — the banked VRF and its fixed lane mapping

Work package **I-053** of `docs/stage-4-vector-locality.md`, implementing

> 定义固定 logical-element→bank/lane 映射及冲突仲裁；测试 fractional/integer
> LMUL、group overlap、wide operands、端口不足。运行 `CASE=vrf.mapping_aliases`.
> Pass: 同一 logical vector 在不同 lane 数实现上读写一致；mask/source overlap
> 不覆盖尚未读取元素。
> Fail: lane resize 后 element 重排丢失或 VRF read latency 在平台间变化未处理。

## STATUS: COMPLETE

| Acceptance item | Evidence |
| --- | --- |
| The mechanism exists as RTL with its mapping, arbitration and lifetime rule in the header | `rtl/core/mosaic_vrf.sv` (new) |
| The same logical vector reads and writes identically at 2, 4 and 8 lanes | the whole file is primed once and read at 2, 4 and 8 lanes (128 words each), then rewritten at 2 and read at 8, at 4 and read at 2, at 8 and read at 4; every word equals the written pattern (phase `lane-count`) |
| Mask/source overlap never overwrites an unread element | a destination write aliasing a source (`vd == vs2`) and a destination aliasing the mask (`vd == v0`) are both refused while the read is in flight, the read returns the pre-write value, and the retried write then lands (phase `overlap`) |
| Integer and fractional LMUL, wide operands, port shortage | phases `mapping` (LMUL 1/2/4/8, 1/2, 1/4, 1/8; SEW 8/16/32/64), `wide` (SEW=64 over two banks), `conflicts` (bank port limits and broadcast) |
| Fail criterion — lane resize permutes/loses an element | `MOSAIC_VRF_MUTANT_LANE_PERMUTE`, exit 1 |
| Fail criterion — VRF read latency varies between platforms | `MOSAIC_VRF_MUTANT_PLATFORM_LATENCY`, exit 1 |
| Fail criterion — an overlapping write clobbers an unread element | `MOSAIC_VRF_MUTANT_OVERLAP_CLOBBER`, exit 1 |
| `python3 tools/run_unit.py --profile p0 --case vrf.mapping_aliases` | `RESULT PASS … 3806 per-cycle comparisons over 3846 cycles … seed 1`, exit 0, sha256 `319fb871d7983ecb…` |
| The same binary at seeds 2, 7 and 12345 | PASS, identical 3806 comparisons |
| `verilator --lint-only -Wall` on the new module, shipping and all four `-D` controls | clean under every definition |
| `slang-tidy --std 1800-2017 --single-unit` on the package, module and wrapper | exit 0 (only the tree-wide STYLE advisories) |
| This report | — |

The case passes on the shipping build; the four mutants below are what makes
that pass meaningful.

## The registry entry

`tests/unit/registry.json` already carried the case, naming

| field | value | state found |
| --- | --- | --- |
| `task` | `I-053` | — |
| `top` | `mosaic_vrf_tb` | — |
| `rtl` | `rtl/core/mosaic_pkg.sv`, `rtl/core/mosaic_vrf.sv` | **missing** |
| `sv` | `sim/tb/mosaic_vrf_tb.sv` | **missing** |
| `cpp` | `sim/unit/tb_vrf.cpp` | **missing** |
| `pending` | `true` | left as found |

The three missing sources were created at exactly the registered paths. The
registry was **not** edited — the `pending` marker is the integration lead's to
clear when the package is recorded, per the project's fixed-before-built order.

## The mechanism, as a rule

`mosaic_vrf` is the banked storage for the 32 architectural vector registers. It
owns three rules and nothing else.

### 1. The fixed logical-element-to-bank/row mapping

Geometry is the p2 vector block (`config/geometry/p2.json`): `VLEN = 128`,
`ELEN = 64`, `VREGS = 32`, `BANK_W = 32`, `BANKS = 32`. A 32-bit bank word holds
`VLEN/BANK_W = 4` words of a register; eight registers share the 32 banks
(`REGS_PER_ROW = 8`), so the file is `ROWS = 4` deep. Register `r`'s word `w`
lives at

```
row  = r / REGS_PER_ROW
bank = (r % REGS_PER_ROW) * WORDS_PER_REG + w
```

a modulo/quotient decode rather than a bit-field split, so a geometry whose bank
count does not divide its entry count cannot alias two registers onto one word.
Total storage is `BANKS * ROWS * BANK_W = 4096` bits, exactly 32 × 128.

A demand names `(base register, element, SEW, LMUL)` and the element index is the
**group** index, so one rule covers integer and fractional LMUL:

```
grp_bits = VLEN << LMUL   (LMUL >= 0)   |   VLEN >> -LMUL   (LMUL < 0)
bit_off  = elem * SEW     must satisfy bit_off + SEW <= grp_bits
phys     = (base & ~(grp_regs-1)) + bit_off / VLEN
```

with `grp_regs = 2^LMUL` for `LMUL >= 0` and 1 for fractional LMUL, and the base
register aligned down to the group size. An element whose bit range leaves the
group is **refused and counted** (`o_rd_bad_ctr` / `o_wr_bad_ctr`), never aliased
onto a live element. An SEW ≤ 32 element fits one bank word; an SEW = 64 element
spans two words of the same row — the wide-operand case. A read returns the
element right-justified with the bits above SEW cleared.

### 2. Conflict arbitration — port shortage

A bank read serves up to `RD_PORTS` distinct rows per cycle and broadcasts a
shared `(bank, row)` word to every consumer of it. A bank write serves `WR_PORTS`
rows and does **not** share a row (two writers of one word would be a race).
Extra demands are refused (`rd_gnt` / `wr_gnt` low), retried by the caller and
counted apart (`o_rd_conflict_ctr`, `o_wr_conflict_ctr`); a refused demand leaves
no trace. The lowest-numbered slot has priority; the priority is an index, not a
fairness scheme (fairness belongs to the operand collector, I-026/I-027).

### 3. Read lifetime — the mask/source-overlap rule

A write is refused when its bit range overlaps a read that has been granted but
not yet presented: a read granted this cycle (it presents at `+RD_LATENCY`), and
every pipeline stage that presents **after** this cycle. The stage presenting
*this* cycle is already visible to its consumer, so it does not block a write
offered in the same cycle — the write applies at the edge and becomes visible
next cycle, after the read's value was taken. There is no same-cycle
write-through, so a read and a write of one address presented together resolve as
read-before-write. Refused writes are counted on `o_wr_hazard_ctr`.

Latency is a property of the design (`RD_LATENCY = 1`), exposed on
`o_rd_latency_o`. It does not depend on the lane quota `lane_count_i` or on the
platform selector `plat_i`; the platform selector exists only so a control can
make it depend on one.

## Acceptance property 1 — the same logical vector across lane counts

`lane_count_i` is a **runtime** input (1..8; the case uses 2, 4 and 8). It
decides only how many slots are eligible each cycle — a throughput knob. The
mapping does not mention it.

The driver primes all 128 words (32 registers × 4 words, SEW=32) at 8 lanes and
reads the whole file back at 2, 4 and 8 lanes; all three readbacks equal the
primed pattern and each other. It then **rewrites** the file at 2 lanes and reads
at 8, rewrites at 4 and reads at 2, rewrites at 8 and reads at 4, so every
(write lanes, read lanes) pair is exercised. Finally a wide SEW=64 element is
written at 2 lanes and read at 8. A mapping that permuted or lost an element when
the lane quota changed would fail the first readback; `LANE_PERMUTE` does.

## Acceptance property 2 — overlap does not overwrite an unread element

Phase `overlap`, source case (`vd == vs2`): read `v1[1]` and, in the **same**
cycle, offer the write of `v1[1]` with a new value. The shipping build grants the
read, refuses the write (hazard counted), and the read's response carries the
**old** value; the retried write is then admitted and a later read sees the new
value. Mask case (`vd == v0`): the same sequence with the destination aliasing
the mask register. `OVERLAP_CLOBBER` drops the guard and the write passes in the
grant cycle, which is the first failure.

## Mutants

House rule, all four: the `-D` name is named in the table and every occurrence in
`rtl/core/mosaic_vrf.sv` is one branch of one rule (`LANE_PERMUTE` appears once
in each of the read path, the write path and the query, since all three resolve
one element the same way); the build directory is wiped when the command changes
(appending a `-D` does — `tools/run_unit.py` records the command in
`build_command.txt`); each mutant binary's sha256 differs from the shipping one;
the shipping binary was rebuilt afterwards and its hash reproduced. All four
definitions lint clean under `verilator --lint-only -Wall`.

Base: exit 0, sha256 `319fb871d7983ecbc1e9d0a0ab4c3fea98828c5fccb315afb21b952cdff50d9c`,
`RESULT PASS … 3806 per-cycle comparisons over 3846 cycles … seed 1`.

| mutant | exit | sha256 (16) | first failing check | injects |
| --- | --- | --- | --- | --- |
| `MOSAIC_VRF_MUTANT_LANE_PERMUTE` | 1 | `1669e60c6ee796ed` | `mapping: cycle 8: q_phys_reg: expected 0, got 2` | the physical register is offset by the runtime lane quota, so changing lanes permutes and aliases elements |
| `MOSAIC_VRF_MUTANT_LOSE_ELEMENT` | 1 | `a489ece74ed4a4fd` | `mapping: cycle 38: q_row: expected 3, got 1` | the row decode drops the top row bit, so registers 16-31 alias 0-15 |
| `MOSAIC_VRF_MUTANT_PLATFORM_LATENCY` | 1 | `9193d6d66d197838` | `latency: cycle 779: rd_rsp_valid[0]: expected 0, got 1` | the read latency is a function of `plat_i`, so the same request answers at a different cycle |
| `MOSAIC_VRF_MUTANT_OVERLAP_CLOBBER` | 1 | `bbab134430748e72` | `overlap: cycle 740: wr_gnt: expected 0, got 2` | the read-lifetime guard is dropped, so a destination write clobbers an unread source |

Each mutant's first failure is on the rule it injects, none of the four hashes
equals the shipping hash, and none passes identically to the shipping build, so
none is a redundancy probe.

What each control is evidence for, in the card's terms:

* **Lane permute** — "lane resize 后 element 重排丢失". The mapping query returns
  a different physical register for the same `(register, element, SEW, LMUL)` as
  soon as `lane_count_i` changes; the driver's independent model, which has no
  lane term, disagrees at the first queried tuple. The same defect would fail the
  `lane-count` readback that follows.
* **Lose an element** — "mapping loses an element". The two-row decode aliases
  registers 16-31 onto 0-15, so the mapping check disagrees on the row of the
  first register in the top half; a readback of the aliased half would show the
  overwritten data.
* **Platform latency** — "VRF read latency 在平台间变化未处理". The response
  appears one cycle late at `plat_i != 0`; the driver's model is fixed at the
  design's latency, so the response-valid field disagrees the first time a probe
  is issued at another platform.
* **Overlap clobber** — "source overwrite". The guard is removed, so the
  destination write is granted in the same cycle as the read of the same element
  and the read is corrupted.

## Coverage

Ten phases, each resetting and priming first; every cycle in every phase compares
the DUT's whole output surface — the two grant vectors, all eight response
valid/tag/data triples, the eight mapping-query fields, `busy`, and the seven
counters — against the independent model. The run is **3806 compared cycles over
3846 total cycles**.

| phase | what it witnesses |
| --- | --- |
| `geometry` | every width the driver drives is the DUT's own read-back |
| `mapping` | 14 tuples (integer LMUL 1/2/4/8, fractional 1/2, 1/4, 1/8; SEW 8/16/32/64; wide, unaligned and overflowing groups; out-of-range refused) agree with the model **and are identical at 2, 4 and 8 lanes** |
| `lane-count` | the whole file is identical under (write 8, read 2/4/8), (write 2, read 8), (write 4, read 2), (write 8, read 4), and a wide element written at 2 and read at 8 |
| `conflicts` | four demands on one bank grant exactly `RD_PORTS` and count the rest; a shared `(bank,row)` word is broadcast; a second writer of one bank is refused |
| `wide` | an SEW=64 element spans two bank words, and writing one element does not disturb the other |
| `fractional` | LMUL 1/2 and 1/8 elements round-trip; an out-of-group element is refused and counted |
| `overlap` | the lifetime guard on a source alias and on a mask alias, with the pre-write value observed |
| `latency` | the response arrives exactly one cycle after the grant at every `plat_i` in 0..3 and every lane count in 2/4/8 |
| `random` | 1500 seeded cycles over the whole address space (random SEW/LMUL/element, a quarter forced self-aliasing); coverage of conflicts, hazards and bad requests is **required** of the run, not hoped for |
| `determinism` | the same seed reproduces the run byte for byte; the counters are zero after a reset |

## Gates

| gate | result |
| --- | --- |
| `verilator --lint-only -Wall -Wno-DECLFILENAME rtl/core/mosaic_vrf.sv` | clean |
| the same with each of the four `-DMOSAIC_VRF_MUTANT_*` | clean |
| `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_pkg.sv rtl/core/mosaic_vrf.sv sim/tb/mosaic_vrf_tb.sv` | exit 0 (STYLE advisories only, the same class the existing wrappers raise) |
| `vrf.mapping_aliases`, seeds 1/2/7/12345 | PASS, exit 0 |

**Not run here, deliberately:** the project-wide `make check`, `lint_rtl`,
`lint-slang` and `check_records`, and the other registered cases. Those are the
integration run, executed once every lane has landed; at the time of writing
another lane holds uncommitted edits in `mosaic_core.sv`, `mosaic_cluster.sv`,
`mosaic_iq.sv` and `tests/unit/registry.json`, so a project-wide pass would
report their in-flight state rather than this package's. This case compiles only
`rtl/core/mosaic_vrf.sv`, `sim/tb/mosaic_vrf_tb.sv`, `sim/unit/tb_vrf.cpp` and
the shared harness support the runner adds to every case, so its numbers are a
property of this module and this geometry and cannot move with the core wiring.

## Commands

```sh
# shipping base, through the official runner
python3 tools/run_unit.py --profile p0 --case vrf.mapping_aliases

# build + run one control (the extra -D makes the runner wipe and rebuild)
python3 - <<'PY'
import sys; sys.path.insert(0, 'tools'); import run_unit
run_unit.VERILATOR_FLAGS.append('-DMOSAIC_VRF_MUTANT_OVERLAP_CLOBBER')
run_unit.build_case('p0', 'vrf.mapping_aliases',
                    run_unit.load_registry()['cases']['vrf.mapping_aliases'])
PY
build/p0/unit/vrf.mapping_aliases/vrf.mapping_aliases \
    --case vrf.mapping_aliases --out /tmp/vrf_mut --seed 1 --max-cycles 200000; echo $?

# the module's own lint, shipping and every control
verilator --lint-only -Wall -Wno-DECLFILENAME rtl/core/mosaic_vrf.sv
for m in LANE_PERMUTE LOSE_ELEMENT PLATFORM_LATENCY OVERLAP_CLOBBER; do
  verilator --lint-only -Wall -Wno-DECLFILENAME -DMOSAIC_VRF_MUTANT_$m rtl/core/mosaic_vrf.sv
done
```

## Files

| file | change |
| --- | --- |
| `rtl/core/mosaic_vrf.sv` | **new** — the banked storage, the fixed mapping, the conflict arbitration, the read-lifetime guard and the four controls |
| `sim/tb/mosaic_vrf_tb.sv` | **new** — the wrapper: the p2 geometry, per-slot control words repacked onto the module's buses, geometry read-back |
| `sim/unit/tb_vrf.cpp` | **new** — the driver: the independent model, ten phases, the mutant hooks |
| `results/reports/I-053-vrf.md` | this file |

No existing file was modified. `tests/unit/registry.json` and
`config/status/implementation_status.json` are untouched.

## The revision measured

| item | value |
| --- | --- |
| git HEAD | `b1966f5868659f8d65b21379015c21e4eb4609e8` |
| working tree | HEAD plus other lanes' uncommitted edits of the day (`mosaic_core.sv`, `mosaic_cluster.sv`, `mosaic_iq.sv`, their testbenches and `tests/unit/registry.json`) |
| shipping binary sha256 | `319fb871d7983ecbc1e9d0a0ab4c3fea98828c5fccb315afb21b952cdff50d9c` |
| re-run on that revision | the PASS line, the comparison count and the binary hash reproduced exactly |

## Not covered

* **Not wired into the live core.** This is a module-level package, like I-027,
  I-029 and I-030 before it: the registry names a standalone case, so the VRF is
  verified on its own. The integration — `mosaic_core`/`mosaic_dispatch` routing
  vector operands to it, and the vector ALU/LSU consuming its responses — is
  separate work (I-054 and the fabric integration). The module takes its lane
  quota, platform selector and demands at face value; nothing here checks that a
  caller offers a legal `(SEW, LMUL, element)` for the operation it is executing
  (that is `mosaic_vec_desc`'s matrix, I-051).
* **The geometry is the p2 numbers only.** The module's parameters are general,
  but the case fixes `VLEN=128, ELEN=64, VREGS=32, BANK_W=32, BANKS=32,
  LANES_MAX=8, RD_PORTS=2, WR_PORTS=1, RD_LATENCY=1`. A different bank width,
  entry count or port count is not exercised. The widths of two function
  arguments (`[1:0]` row, `[4:0]` bank) are written for this geometry; the
  geometry read-back is what a caller must check before reusing it elsewhere.
* **Storage is not reset**, following `rtl/common/mosaic_ram.sv` and
  `mosaic_prf.sv`: reset cost is control state, not 4096 bits of storage. The
  driver primes every word before reading it, so an unwritten read is never
  compared — and "the value of an unwritten register" is not a defined quantity
  in RVV either.
* **The read latency is fixed at one cycle.** The module is written to carry any
  `RD_LATENCY` (the pipeline has `RD_LATENCY` stages and the lifetime set spans
  them), but only a one-cycle latency is measured; a two-cycle build's lifetime
  window is present in the RTL but not covered by a case.
* **The lifetime guard is exact at element granularity, conservative by design
  across widths.** Two accesses overlap iff their bit ranges in the same physical
  register overlap, so an SEW=8 write inside an SEW=32 element an SEW=32 read has
  not consumed is also refused. That is stricter than the architectural rule
  needs and is the safe direction; a mixed-width throughput cost is not measured.
* **The lane quota is a runtime input, not a compile-time instantiation.** The
  card's "different lane counts" is read here as different values of
  `lane_count_i` on one machine, which is what I-059's broker changes. Three
  separately parametrised instances are not built.
* **One seed family, one clock.** Seeds 1, 2, 7 and 12345; no reset-in-flight
  test at the pipeline edges beyond the ordinary reset between phases; no
  same-cycle read+write to one address with write-through (there is none by
  design).
* **No timing, area, synthesis or Fmax claim.** The controller's arbitration is
  written for clarity, not for a critical path.
