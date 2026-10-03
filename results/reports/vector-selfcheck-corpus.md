# `vec.selfcheck_corpus` — self-checking RVV programs on the out-of-order core

The registered case **`vec.selfcheck_corpus`** (top `mosaic_core_tb`, driver
`sim/unit/tb_core_vecselfcheck.cpp`, profile **p2**). It runs the seven
self-checking RVV programs under `tests/programs/vec/` on the integrated core
and requires every one to reach its PASS trap.

**The corpus had no vector program at all** (`tests/programs/src/` is scalar,
`p01..p13`), and it could not get one the usual way: `tools/host_oracle.py`, the
only sanctioned producer of golden signatures, is a scalar RV64IM reference with
zero vector support (`grep -cE "vadd|vsetvli|VECTOR|OP_V" tools/host_oracle.py`
returns 0). These programs are the honest substitute: their expected values are
**hand-derived from the RVV specification** and carried as constants in the
program, which computes the result on the DUT and compares. **The human
derivation is the oracle here**, and that is a weaker oracle than a reference
model — see §"Not covered" for exactly what that does and does not prove.

## STATUS: COMPLETE (programs + driver + control)

| Acceptance item | Evidence |
| --- | --- |
| A set of self-checking RVV programs in their own directory, corpus untouched | `tests/programs/vec/` (7 programs + a control); `tests/programs/src/`, `corpus.json`, `golden.json`, `tools/host_oracle.py` unchanged |
| Cover the delivered mask-prefix ops | `v05_maskpfx_e8`: `vmsbf`/`vmsif`/`vmsof` over 8 patterns incl. the all-zero row |
| Cover the mask-logical family | `v04_masklog_e8`: all eight of `vmand/vmnand/vmor/vmnor/vmxor/vmxnor/vmandn/vmorn` |
| Cover the advertised integer families | `v01`/`v02`/`v08` (ADDSUB, incl. `.vx`/`.vi`/masked) and `v03` (LOGIC) — the two the core advertises |
| Cover a vector load/store | `v06_loadstore`: unit-stride round trip at EEW 8/16/32/64 plus a masked store |
| No program for an unadvertised family | `VEC_ALU_CAPS` and `VEC_LSU_CAPS` read from `rtl/core/mosaic_core.sv`; only ADDSUB/LOGIC/MASKLOG/MASKPFX + unit-stride are targeted |
| Hand derivation auditable in the program | a header block per program: inputs, per-element expected result, the rule, and the expected `vl`/`vtype` |
| Deterministic | directed; identical PASS at seeds 1 and 7 |
| Registered case runs them, names how many passed | `RESULT PASS vec.selfcheck_corpus 7/7 self-checking RVV programs reached the pass trap` |
| Profile with the vector engine enabled | **p2** (the lowest profile whose geometry declares the vector engine); also PASS 7/7 on p0, p1 and p3 |
| A control observed to FAIL, from a deleted build | `v07_control_wrong_addsub`; driver exit 1, first failure `check 1 failed at byte offset 24` |
| Report | this file |

## The machine these programs target

`rtl/core/mosaic_core.sv` advertises:

```
VEC_ALU_CAPS = 17'b0001100_0100_0001   // ADDSUB(0) + LOGIC(6) + MASKLOG(10) + MASKPFX(11)
VEC_LSU_CAPS = 8'b0000_0001            // unit-stride only
```

The ALU *implements* seventeen families, but a family whose capability bit is
clear is refused at execution (`o_illegal`, no VRF transaction), and the core
decodes only the four above. A program for any other family would trap, and a
trapping program is not evidence, so none was written. The core also refuses a
vector load/store whose instruction EEW differs from `vtype.SEW` (§"Finding 2").

## The programs

| program | capability | SEW / VL / LMUL | check |
| --- | --- | --- | --- |
| `v01_addsub_e64` | ADDSUB `vadd.vv`, `vsub.vv`, `vadd.vx`, `vsub.vx` | e64 / 2 / m1 | 64-bit add/sub, wraparound at 2^64 |
| `v02_addsub_e32` | ADDSUB `vadd.vv`, `vsub.vv`, `vadd.vi`, `vsub.vx` | e32 / 4 / m1 | 32-bit wrap at `0xFFFFFFFF`/`0x80000000`, immediate + scalar |
| `v03_logic_e16` | LOGIC `vand.vv`, `vor.vv`, `vxor.vv`, `vand.vx`, `vor.vi`, `vxor.vi` | e16 / 8 / m1 | all three bitwise ops, three forms |
| `v04_masklog_e8` | MASKLOG all eight `.mm` ops | e8 / 16 / m1 | two source-mask pairs |
| `v05_maskpfx_e8` | MASKPFX `vmsbf.m`, `vmsif.m`, `vmsof.m` | e8 / 16 / m1 | 8 patterns incl. first set at 0/7/8/15 and all-zero |
| `v06_loadstore` | unit-stride load/store | e8/e16/e32/e64 | byte-exact round trip per width + a masked store |
| `v08_masked_addsub_e32` | ADDSUB masked `v0.t` | e32 / 4 / m1 | masked-off elements undisturbed (`vma = 0`) |
| `v07_control_wrong_addsub` | **control** | e64 / 2 / m1 | copy of `v01` with one wrong constant; not in the registered run |

Each program configures `vsetvli` explicitly and states its VLEN/SEW assumption.
The `vsew` field uses the RVV 1.0 `SMALLEST_SEW` encoding (`0x00`/`0x08`/`0x10`/
`0x18` for e8/e16/e32/e64); see §"Finding 1" for the history of that encoding.

## The hand derivations (the oracle)

Full derivations are in each program's header. The rules and the load-bearing
constants:

**ADDSUB** — modulo 2^SEW; `vadd.vv vd,vs2,vs1` = `vs2+vs1`, `vsub.*` = `vs2-src`,
`.vx`/`.vi` use a scalar/5-bit-sign-extended immediate as the second operand.
`v01` (e64): `A={1, 0xFFFFFFFFFFFFFFFF}`, `B={2,1}` →
`vadd.vv={3,0}`, `vsub.vv={0xFFFFFFFFFFFFFFFF,0xFFFFFFFFFFFFFFFE}`,
`vadd.vx(+0x10)={0x11,0xF}`, `vsub.vx(-3)={0xFFFFFFFFFFFFFFFE,0xFFFFFFFFFFFFFFFC}`.
`v02` (e32) and `v08` (masked, `vma=0`) are derived the same way in their headers.

**LOGIC** — `vand/vor/vxor` bitwise, within SEW. `v03` derives all six blocks
from `A={0xFFFF,0x0F0F,...}` and `B={0x0F0F,0x00FF,...}`; e.g. `vand.vx &0x0FF0`
= `{0x0FF0,0x0F00,0x0AA0,0x0230,0,0,0x00F0,0x0550}`.

**MASKLOG** — with `a = vs2 bit`, `b = vs1 bit`:
`vmand=a&b`, `vmnand=~(a&b)`, `vmor=a|b`, `vmnor=~(a|b)`, `vmxor=a^b`,
`vmxnor=~(a^b)`, `vmandn=a&~b`, `vmorn=a|~b`. `v04` checks both pairs, e.g.
`(0xAAAA,0x00FF)`: `vmand=0x00AA … vmorn=0xFFAA`.

**MASKPFX** — for first set bit `k` over `[0,vl)`: `vmsbf` sets `i<k`, `vmsif`
sets `i<=k`, `vmsof` sets `i==k`; with no set bit, `vmsbf`/`vmsif` set every
active bit and `vmsof` sets none. `v05` checks all 8 patterns, including the
all-zero asymmetry (`0x0000 → 0xFFFF, 0xFFFF, 0x0000`) and a first set bit that
crosses a mask byte (`0x0100 → 0x00FF, 0x01FF, 0x0100`).

A mask register is stored one bit per element packed 8 bits to a byte (LSB =
lowest element), so a 16-bit mask is two little-endian bytes; the programs store
the destination with `vse8.v` and compare those bytes.

## The case's RESULT line

Built from a deleted directory and run on p2 (7 programs):

```
CASE vec.selfcheck_corpus -- self-checking RVV programs on the out-of-order core
  reset vector 0x0000000080000000, TOHOST 0x0000000080001000, image dir …/tests/programs/vec/build
  programs: 7; each carries its expected values as constants derived by hand from the RVV specification
  PASS v01_addsub_e64         vec_retire=11  elems=2    vlenb=16 cycles=3225
  PASS v02_addsub_e32         vec_retire=11  elems=4    vlenb=16 cycles=3360
  PASS v03_logic_e16          vec_retire=15  elems=8    vlenb=16 cycles=5283
  PASS v04_masklog_e8         vec_retire=38  elems=16   vlenb=16 cycles=16623
  PASS v05_maskpfx_e8         vec_retire=58  elems=16   vlenb=16 cycles=24623
  PASS v06_loadstore          vec_retire=16  elems=0    vlenb=16 cycles=4507
  PASS v08_masked_addsub_e32  vec_retire=11  elems=2    vlenb=16 cycles=1985
RESULT PASS vec.selfcheck_corpus 7/7 self-checking RVV programs reached the pass trap
```

exit 0. The same 7/7 PASS was re-observed with the spec `vsew` encoding against
the fixed RTL on **p0**, **p1**, **p2** and **p3** (manual builds; `run_unit.py`
runs the case only on its registered profile). The vector engine is present in
every profile because `CORE_VEC_VLEN = 128` is hardcoded in `mosaic_core.sv`;
p2/p3 are the profiles whose *geometry* declares it. **p2 is the registered
profile**: it is the lowest profile whose `config/geometry/p2.json` carries the
`"vector"` block (`vlen 128`, `elen 64`, 32 VRF banks), so the case is registered
where the vector configuration is declared rather than merely present.

The driver also requires, per program: at least one vector macro retired (so a
run that silently took a scalar path cannot pass), zero vector traps, zero
vector faults, `vlenb == 16`, `vill` clear, and the program's `vec_status` word
== 1.

## The control (observed to FAIL)

`v07_control_wrong_addsub.S` is `v01_addsub_e64.S` with one expected constant
deliberately wrong (`op1[1] = 0xFFFFFFFFFFFFFFFD` instead of `…FE`). The DUT
computes the correct `…FE`, so the program's own comparison must fail at the
first differing byte.

Built from deleted directories by `tools/run_vector_selfcheck_controls.py`
(which also rebuilds the driver from a deleted directory and re-runs the
shipping case first):

```
baseline exit=0 RESULT PASS vec.selfcheck_corpus 7/7 self-checking RVV programs reached the pass trap
control program sha256:  b38db7e3f77f23e064d2f2c04dddf46c24d4a555d2cbfb0793cc9e89e802fca3
  exit=1 RESULT FAIL vec.selfcheck_corpus 0/1 self-checking RVV programs reached the pass trap
  first failure: CHECK FAILED: v07_control_wrong_addsub: wrote TOHOST=0x0000000000000002 (FAIL); check 1 failed at byte offset 24
```

The named first failure (`check 1`, byte offset 24 = `op1` element 1) is exactly
the perturbed constant, and the driver exits 1. That is what proves the
self-checks bite rather than the programs merely not trapping. The control
program's ELF sha256 is stable across rebuilds (`b38db7e3…02fca3`); the driver
binary's hash is not (Verilator's generated sources make it build-dependent), so
only the ELF hash is quoted as a stable identifier.

## Findings

### Finding 1 — the `vtype` SEW encoding was pre-ratification; fixed to RVV 1.0

The delivered engine originally treated the `vsew` field as **log2(SEW)**
(`vsew_ok = 3..6`, `sew = 1 << vsew`), which is the pre-ratification encoding:
RVV 1.0 encodes `vsew` as 0/1/2/3 for e8/e16/e32/e64. Under that encoding a
GAS `vsetvli rd, rs1, e32, m1` (field 2) was read as an unsupported vtype
(`vill=1`, `vl=0`) and every following vector instruction was refused, so the
first revision of these programs passed raw `vtypei` with the field values this
core defined (0x18/0x20/0x28/0x30).

That divergence is **fixed** (lane `FixVsewEncoding`): `mosaic_vec_cfg.sv` now
accepts `vsew <= 3` and derives the width exponent as `vsew + 3`, and the
load/store EEW comparison is adjusted so a spec-legal `e32` load still matches
the decoder's `e32` EEW. **These programs were updated to the spec encoding** — `vsetvli` with
`vtypei = 0x00/0x08/0x10/0x18` for e8/e16/e32/e64 — and the GAS mnemonics in
`vec.h` now encode exactly those values. A negative-control mutant
(`MOSAIC_VEC_MUTANT_VTYPE_SEW_UNSHIFTED`) preserves the old interpretation.
**Re-verified against the fixed RTL: 7/7 PASS and the control still fails as
below.** The arithmetic the programs check is unchanged by the encoding.

### Finding 2 — a vector load's EEW must equal `vtype.SEW` (documented limitation)

The first `v08` draft loaded its mask with `vle8.v v0, (t0)` while `vtype.SEW`
was 32. The core refused it:

```
instruction: vle8.v v0, (t0)      vtypei = 0x10 (SEW=32)
observed:    illegal instruction, mcause = 2 (vec_status = 0x80000002)
```

`rtl/core/mosaic_core.sv` states this deliberately: *"The packetizer derives an
element's width from vtype.SEW, not from the instruction's width suffix. A suffix
that disagrees is therefore refused rather than mis-addressed."* RVV 1.0 permits
EEW ≠ SEW (loading a mask with `vle8.v` under any SEW is the standard idiom), so
this is a divergence from the spec's memory model — a documented limitation of
the delivered memory path, not an unnoticed defect. The program was fixed the
conformant-with-this-machine way (load the mask under SEW=8, then `vsetvli` to
SEW=32), and the behavior is recorded here rather than hidden.

## Not covered

**Advertised families with no program: none.** Every family the core advertises
(ADDSUB, LOGIC, MASKLOG, MASKPFX) and the unit-stride load/store path has at
least one program. What is *not* covered:

* **Within advertised families:**
  * the **masked** forms of MASKPFX (`vmsbf/vmsif/vmsof` with `v0.t`) — the
    plain forms are covered, and the masked forms are covered at unit level by
    `rvv.mask_prefix_masked`; and
  * `vrsub` and `vnot`, which the ALU's family table lists but the core decoder
    does not decode (so they are not advertised at the core and cannot be run).
* **Twelve of the thirteen families this report listed as unreachable were
  advertised afterwards** (WIDE, MUL, MULW, SHIFT, MINMAX, CMP, SAT, SLIDE,
  GATHER, COMPRESS, REDUCE, REDWIDE) together with their decoder arms and the
  twelve programs now in this corpus; `NARROW` remains unadvertised because the
  delivered ALU and its unit model both truncate its `2*SEW` source. See
  `results/reports/vector-family-advertisement.md`.
* **Everything the ALU/decoder does not implement at all**: integer divide and
  remainder, multiply-accumulate, scaling shifts (`vssrl/vssra`), `vsmul`,
  integer extension (`vzext/vsext`), `viota`/`vid`, `vrgatherei16`, the
  `vmerge`/`vmv*` moves and scalar-vector moves, vector FP, and every vector
  memory class other than unit-stride (strided, indexed, whole-register,
  fault-only-first, segment).
* **The oracle's nature**: these programs verify against a **human derivation,
  not a reference model**. A wrong derivation would pass or fail for the wrong
  reason, and nothing here cross-checks the derivation against an independent
  model of RVV (the only in-repo RVV model, `sim/unit/mask_prefix_ref.h`, is a
  model of the mask families and was *not* used as the expected-value source —
  the constants were derived from the specification text and then compared with
  the DUT). **They are evidence of execution and arithmetic on the delivered
  engine; they are not evidence of RVV conformance.**

## Gates

| gate | result |
| --- | --- |
| `python3 tools/lint_rtl.py --profile p0` | exit 0, 65 source files clean (no RTL was changed by this work) |
| `python3 tools/lint_rtl.py --profile p1` | exit 0, 65 source files clean |
| `python3 tools/check_records.py` | exit 0 |
| new driver under the `lint-cpp` flags (`-std=c++17 -fsyntax-only -Wall -Wextra -Wshadow`) | clean. (`make lint-cpp`'s obj_dir search can pick a stale `build/p2/unit/csr.rule_ledger/obj_dir/Vmosaic_core_tb.h` that predates the vector ports; that stale artifact would break any driver using those ports and is not this work.) |
| `python3 tools/check_exclusions.py` | one failure, `pmu.trace_accounting` has no recorded PASS — a **sibling lane's** case, not this work; this work registers no case in `registry.json` |
| `python3 tools/host_oracle.py --all` | exit 0, 39 cases, oracle agrees with declaration, golden file and itself |
| `python3 tools/host_oracle.py --check-golden-only` | exit 0 |
| `vec.mask_prefix_at_core` (p0) | PASS (the directly related core-level case still passes) |
| `core.corpus_sweep`, `core.act_dut`, `rvv.descriptor_legality` | share no source with this work (no RTL, no corpus, no registry edit); this work adds only `sim/unit/tb_core_vecselfcheck.cpp`, `tests/programs/vec/`, `tools/run_vector_selfcheck_controls.py` and this report |
| `tests/programs/src/`, `corpus.json`, `golden.json`, `tools/host_oracle.py` | unchanged |

## The registry entry

The case id is **`vec.selfcheck_corpus`**, the profile is **`p2`**. It is now
registered in `tests/unit/registry.json` as **task `I-054`**, `profiles: ["p2"]`,
`max_cycles: 500000`, with the driver `sim/unit/tb_core_vecselfcheck.cpp` and the
`vec.mask_prefix_at_core` RTL/SV list. `python3 tools/check_records.py` reports
101 registered cases and agrees. No suite entry is needed (`run_unit.py --all`
runs every registered case). The entry, for reference:

```json
"vec.selfcheck_corpus": {
  "task": "I-054",
  "top": "mosaic_core_tb",
  "rtl": [
    "rtl/core/mosaic_pkg.sv", "rtl/core/mosaic_uop_pkg.sv", "rtl/core/mosaic_alu.sv",
    "rtl/core/mosaic_branch_cmp.sv", "rtl/core/mosaic_branch_target.sv",
    "rtl/core/mosaic_predictor.sv", "rtl/core/mosaic_fetch.sv", "rtl/core/mosaic_decoder.sv",
    "rtl/core/mosaic_rename.sv", "rtl/core/mosaic_rob.sv", "rtl/core/mosaic_iq.sv",
    "rtl/core/mosaic_prf.sv", "rtl/core/mosaic_wb_arbiter.sv", "rtl/core/mosaic_macro_desc.sv",
    "rtl/core/mosaic_dispatch.sv", "rtl/core/mosaic_steering.sv", "rtl/core/mosaic_cluster.sv",
    "rtl/core/mosaic_cluster_bypass.sv", "rtl/core/mosaic_redirect_arb.sv",
    "rtl/core/mosaic_muldiv.sv", "rtl/core/mosaic_retire.sv", "rtl/core/mosaic_core.sv",
    "rtl/core/mosaic_owner_fsm.sv", "rtl/core/mosaic_lane_broker.sv", "rtl/core/mosaic_fpu.sv",
    "rtl/core/mosaic_fp_unit.sv", "rtl/core/mosaic_vec_desc.sv", "rtl/core/mosaic_vec_cfg.sv",
    "rtl/core/mosaic_vrf.sv", "rtl/core/mosaic_vec_alu.sv", "rtl/core/mosaic_vec_lsu.sv",
    "rtl/core/mosaic_vec_restart.sv", "rtl/core/mosaic_vec_chain.sv", "rtl/core/mosaic_vec_fp.sv"
  ],
  "sv": ["sim/tb/mosaic_core_tb.sv"],
  "cpp": [
    "sim/unit/tb_core_vecselfcheck.cpp",
    "sim/common/elf_loader.cpp",
    "sim/common/memory_model.cpp",
    "sim/common/event_tap.cpp"
  ],
  "max_cycles": 500000,
  "seed": 1,
  "profiles": ["p2"]
}
```

## Files

* `tests/programs/vec/README.md` — why this directory exists.
* `tests/programs/vec/vec.h` — platform surface, the `vtype` assumption, the pass/fail status word.
* `tests/programs/vec/crt0.S` — its own reset path: stack, fatal trap handler, `mstatus.VS`, helpers.
* `tests/programs/vec/vec_selfcheck.ld` — linker script (image stays below the TOHOST window).
* `tests/programs/vec/src/v01..v08*.S` — the programs and the control.
* `tests/programs/vec/Makefile` — builds `build/*.elf`.
* `sim/unit/tb_core_vecselfcheck.cpp` — the case driver.
* `tools/run_vector_selfcheck_controls.py` — the control.
