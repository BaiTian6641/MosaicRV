# I-051 — freeze the RVV descriptor and profile

Case: `rvv.descriptor_legality` (top `mosaic_vec_tb`, driver `sim/unit/tb_vec.cpp`).
Verdict: **PASS** on p0 and p2, from a deleted build directory with the three
mutants below caught.

| item | value |
|---|---|
| `rtl/core/mosaic_vec_desc.sv` | `b5a450c0f8e8d87738511488b9298e35522804f5c3ba8eff0d959e2d68c14ccc` |
| `sim/tb/mosaic_vec_tb.sv` | `8cf080f2a677d0fa1270de25887208d942c5643531099421b557e1fc62197362` |
| `sim/unit/tb_vec.cpp` | `deb3abc035145716cc0777f8b68931ee8fc2eebab89afeb2d8adaf4875e53c3b` |
| `tools/run_vec_controls.py` | `46bb929a0b3d66193c5fd063db9d00b1820e1c5becd6b6605a26c0fea7a5b835` |
| shipping binary sha256 (p0 == p2) | `5522fbfb4fcfcee62b30b4cd8deac23cb514b3bfec04b2d9fe110a32790ffb19` |
| case result (p0, p2) | `RESULT PASS rvv.descriptor_legality checks=336 combos=9284 cycles=28` |

`tests/unit/registry.json` and `config/status/implementation_status.json` were
not edited: the registry entry was registered before this package and its
`"pending": true` marker is the integration lead's to clear.

## 1. The profile that is frozen

* **VLEN = 128 bits, ELEN = 64, vlenb = VLEN/8 = 16.** VLEN is fixed for the
  life of the hart (config/profiles/p2.json derived_notes: "runtime lane quota
  changes (I-059) must never change architectural VLEN"). The `lane_count_i`
  input is an *echo* only; the shipping build's `o_vlen_o` is the constant 128
  and `o_vlenb_o` is the constant 16, proven unchanged across a lane-count sweep
  of 2/4/8 by `lane-invariance`. p2/p3 declare the same 128/64
  (`isa_target.vlen`/`elen`).
* **The module does not read `mosaic_cfg_pkg::MOSAIC_VLEN`.** That constant is
  emitted only for profiles that declare a vector geometry, and `rtl/**` lints
  and `make unit` runs under every profile; the descriptor is a module-level
  package (like the caches and the fabric) that must elaborate everywhere. The
  freeze is therefore a design constant here, and the case asserts the two
  read-backs (`vlenb = 16`, and `VLEN = 128` across lanes). The binary is
  consequently identical for p0 and p2.

## 2. The declared vector operation list

One class per legality *shape* — source EEW, destination EEW, EMUL multiplier,
and the register-overlap rule. `o_class_count_o` exposes the count (18) and the
case requires every class to be visited.

| class | RVV 1.0 groups it shapes | dst EEW | src EEW | dst EMUL | src EMUL | overlap |
|---|---|---|---|---|---|---|
| `VOP_IVV` | vadd/vsub/vrsub/vmin[u]/vmax[u]/vand/vor/vxor/vsll/vsrl/vsra (OPIVV/X/I), vadc/vsbc | SEW | SEW | LMUL | LMUL | any |
| `VOP_VMUL` | vmul, vmulh[u/su], vmacc/vnmsac/vmadd/vnmsub | SEW | SEW | LMUL | LMUL | any |
| `VOP_VDIV` | vdiv[u], vrem[u] | SEW | SEW | LMUL | LMUL | any |
| `VOP_VWIDE` | vwadd[u]/vwsub[u]/vwmulu/vwmulsu/vwmacc[u], vwredsum[u] | 2·SEW | SEW | 2·LMUL | LMUL | partial no |
| `VOP_VNARROW` | vnsrl/vnsra/vnclip[u] | SEW | 2·SEW | LMUL | 2·LMUL | partial no |
| `VOP_VEXT` | vzext.vf2/vsext.vf2 | 2·SEW | SEW | LMUL | LMUL | partial no |
| `VOP_VRED` | vredsum/vredand/…/vredxor, vwredsum | SEW | SEW | LMUL | LMUL | any no |
| `VOP_VMASK` | vmseq/vmsne/vmslt[u]/vmsle[u]/vmsgt[u], vmadc/vmsbc | mask | SEW | LMUL | LMUL | any |
| `VOP_VMASKMV` | vmsbf/vmsif/vmsof | mask | mask | LMUL | LMUL | any no |
| `VOP_VSLIDE` | vslideup/down/1, vrgather, vrgatherei16, vcompress | SEW | SEW | LMUL | LMUL | any |
| `VOP_VFP` | vfadd/vfsub/vfmul/vfdiv/vfrsub/vfmacc/…/vfsqrt/…/vfsgnj/… (SEW ≥ 16) | SEW | SEW | LMUL | LMUL | any |
| `VOP_VFPWIDE` | vfwadd/vfwsub/vfwmul/vfwmacc, vfwcvt/vfncvt (SEW ≥ 16) | 2·SEW | SEW | 2·LMUL | LMUL | partial no |
| `VOP_VFPRED` | vfredsum/vfredosum/vfredmax/vfredmin (SEW ≥ 16) | SEW | SEW | LMUL | LMUL | any no |
| `VOP_VLOAD` | vle/vlse/vlre (unit-stride, strided, whole-register-encoded) | SEW | SEW | LMUL | LMUL | any |
| `VOP_VSTORE` | vse/vsse (no destination group) | — | SEW | — | LMUL | — |
| `VOP_VWHOLE` | vl<n>re<e>/vs<n>r, vmv<n>r.v — **vtype-free** | 8 | 8 | LMUL | LMUL | any |
| `VOP_VSET` | vsetvli/vsetivli/vsetvl — **vtype-free** | — | — | — | — | — |
| `VOP_VSPECIAL` | vmv.x.s/vmv.s.x/vmv.v.v/vmerge/vmvr | SEW | SEW | LMUL | LMUL | any |

## 3. The legality rules

**vtype rule** (raw `vtype_i`, independent of the class):

* `vill` set → unsupported (`VTYPE_UNSUPPORTED`).
* `vsew` ∉ {3,4,5,6} (SEW 8/16/32/64) → `RESERVED_VSEW` (V spec L309-L314).
* `vlmul = 100` → `RESERVED_VLMUL` (V spec L348-L361).
* SEW > LMUL·ELEN, i.e. effective EMUL < 1 → `EMUL_RANGE` (V spec L323-L331).
  Legal vtypes enumerate to **22** of the 64 `vsew × vlmul` pairs (SEW=8 admits
  all seven LMULs, SEW=16 excludes 1/8, SEW=32 excludes 1/8,1/4, SEW=64 excludes
  1/8,1/4,1/2).

**operation rule** (first rule broken names the reason, in this order):
`EEW_RANGE` (family minimum SEW, or destination EEW > ELEN / < 8) →
`EMUL_RANGE` (source or destination effective LMUL outside [1/8, 8]) →
`MASK_DST_OVERLAP` (masked instruction whose destination group includes v0) →
`OVERLAP_SRC` (destination vs source group, per the class's rule).

**register-group rule.** A group with LMUL exponent `e < 0` is one register; with
`e ≥ 0` it is `2^e` registers aligned to that size. Overlap is `any`
(vd may alias a source), `partial-no` (vd may equal a source group exactly but
may not partially overlap it), or `any-no` (disjoint). The mask rule is checked
against v0 as the aligned group base 0.

## 4. The descriptor contract

One descriptor, one macro identity, one ROB entry:

* identity `{rob_index, rob_gen, uop_index}` and the configuration snapshot
  `{vtype, vl, vstart, vd, mask_version}` captured at allocate;
* a **128-bit element bitmap** (one bit per element; bit *i* = element *i*
  produced its architectural update), with an out-of-order completion set;
* a **contiguous committed prefix** (`o_prefix_o`) computed separately from the
  bitmap's population count — the restart point, not the completion count;
* **fault progress**: the earliest faulting element and its class, frozen on
  first fault (`o_accepting_elems_o` drops, so younger elements cannot commit
  past the fault);
* `o_rob_entries_used_o` ∈ {0,1}: it must not scale with the element count.

**Reset (transaction in flight).** Reset clears the descriptor, the bitmap, the
fault record and the counters. An allocate or element-done pulse presented while
reset is asserted is ignored: the in-flight macro is squashed, publishes no
architectural effect, and leaves no partial bitmap. The descriptor is reusable on
the cycle reset deasserts. This is the package's defined answer to "what happens
to an outstanding vector macro at reset".

## 5. Conservation identities and coverage (the case's checks)

The case's standing invariant, checked on every clocked cycle, is
`rob_entries_used ≤ 1` **and** `o_elems_done_ctr_o == popcount(bitmap)`. A
descriptor that leaks an entry per element, or loses a completion, breaks one of
these.

The matrix is **enumerated, not sampled**:

* `vtype-matrix`: all 64 `vsew × vlmul` pairs → 22 legal / 42 illegal asserted
  against the spec oracle;
* `op-matrix`: 18 classes × 64 vtypes × 4 register cases × {masked, unmasked} =
  **9216** combinations, each asserted for legality, reason, and the
  `o_illegal` complement → 2628 legal / 6588 illegal, matching the oracle count;
* coverage checks: experience count (9216 + 64 + 4 = 9284), legal count, the set
  of classes visited (18/18) and the set of reasons observed (9/9) are asserted,
  so a rule that is never reached fails even if the visited combinations pass.

Reason histogram over the enumerated **op-matrix** (9216 combinations): `OK`
2628, `EMUL_RANGE` 864, `EEW_RANGE` 264, `OVERLAP_SRC` 417,
`MASK_DST_OVERLAP` 435, `RESERVED_VLMUL` 512, `RESERVED_VSEW` 4096. The
**vtype-matrix** adds 22 `OK`, 6 `EMUL_RANGE`, 4 `RESERVED_VLMUL` and 32
`RESERVED_VSEW`; the two directed probes add one `VTYPE_UNSUPPORTED` (`vill`)
and one `CLASS_INVALID` (unknown family). The case asserts all nine reasons are
observed at least once.

Descriptor phases: out-of-order completions and prefix closure, a refused second
allocate that must not overwrite the identity, release-and-reuse with a new
identity, fault after a partial prefix (younger completions blocked, later faults
do not overwrite the earliest), and reset with a macro and its completions in
flight.

## 6. Mutant table

`python3 tools/run_vec_controls.py --profile p2`. Each mutant is rebuilt from a
**deleted** directory with its `-D` on that build's own recorded command line
(`build/p2/vec_controls/<define>/build_command.txt`), must differ from the
shipping binary, must exit 1, and its first `CHECK FAILED` must name the check.

| mutant | defect | exit | sha256 | first failure |
|---|---|---|---|---|
| `MOSAIC_VEC_MUTANT_ILLEGAL_ACCEPTED` | widening with dst EMUL > 8 accepted | 1 | `e188f10a5492ca9f2ce5df34ea1fa348cf5b3fa786ea81ac30af5c61e50ae91f` | `op-matrix VWIDE vsew=3 vlmul=3 reg=disjoint mask=0: expected EMUL_RANGE got OK` |
| `MOSAIC_VEC_MUTANT_LOSE_PROGRESS` | every element completion dropped from the bitmap | 1 | `898b7dca330777b767806cf773bf6b12b03410b65c4a8608723fdd899755eecc` | `descriptor-progress: elements 0 and 1 not recorded` |
| `MOSAIC_VEC_MUTANT_LANE_VLEN` | the lane quota leaks into `o_vlen_o` | 1 | `6baf6758c191e1c2571f2f026739d8212d11eca6302b027a5fc7e30776fe71ac` | `lane-invariance: lane count 2 changed VLEN to 130, expected 128` |

All three differ from the shipping binary and name their check. Shipping
baseline: `RESULT PASS rvv.descriptor_legality checks=336 combos=9284 cycles=28`.

## 7. Keep-passing evidence (re-run this wave)

| case | profile | result |
|---|---|---|
| `rvv.descriptor_legality` | p0, p2 | PASS, checks=336 combos=9284 cycles=28 |
| `mmio.exactly_once` | p0 | PASS |
| `core.mem_program` | p0 | PASS |
| `core.corpus_sweep` | p0 | PASS |
| `store.wrong_path_visibility` | p0 | PASS |
| `lsu.byte_forwarding` | p0 | PASS |
| `lsu.size_fault_boundaries` | p0 | PASS |
| `core.act_dut` | p1 | **PASS `applicable=127 generated=127 run=127 passed=127 failed=0`**, `act_commit=96493a91448ca50780013fd892daec2c204487ba` |

The core was not edited (the descriptor is a new file), so ACT4 is unchanged at
127/127.

## 8. Gates

* `rtl/core/mosaic_vec_desc.sv` lints clean with Verilator `-Wall` and passes
  `slang-tidy`; `sim/tb/mosaic_vec_tb.sv` elaborates and builds under p0 and p2.
* `tools/check_records.py`: **green** (`61 delivered, 69 registered, records
  agree`).
* `make check` currently stops at `check_exclusions` with
  `soc.bus_errors_and_ids has no recorded PASS and no ledger entry names it`.
  That case belongs to **I-047** (the SocInterconnect lane), which is still in
  flight; my case has a recorded PASS and is not named. Not caused by this
  package.
* `make lint PROFILE=p0|p1` reports exactly one failing file,
  `rtl/soc/mosaic_soc.sv`, which is **I-047's** new module (still being written).
  `rvv.descriptor_legality`'s RTL lints clean in the same run. Not caused by this
  package.

## 9. Not covered (honest list)

* **Datapaths are not implemented.** This package freezes the descriptor and the
  legality contract only. vset/CSRs (I-052), the VRF and lane mapping (I-053),
  integer/mask/permute/reduction execution (I-054), vector FP (I-055), the memory
  packetizer (I-056) and restart (I-057) remain unimplemented; the 18 classes are
  legality *shapes*, not execution units.
* **The descriptor is not wired into the core.** `mosaic_vec_desc` and its
  testbench are a module-level package; `mosaic_core` / `mosaic_lsu_endpoint`
  were deliberately not touched (the core is frozen this wave). The integration
  is separate work.
* **Deferred matrix rows**, documented in the RTL header and not advertised:
  4:1/8:1 extension ratios (`vzext/vsext.vf4/vf8`), segment accesses
  (`vlseg/vsseg`, which need `nf`), indexed accesses (`vluxei/vsuxei`, which need
  the index group named), and fractional-LMUL element/segment field progress.
  Each has a definite outcome — *deferred, not in the matrix, must not be
  claimed* — but it is not a legal/illegal verdict yet.
* **`vma`/`vta`** (tail and mask policy) are not legality inputs and are ignored
  by the query; they select element write policy at execution (I-054).
* **One descriptor only.** Multi-macro chaining is I-058.
* **Element/segment granularity.** The bitmap is element-granular up to
  VLEN = 128 elements; the per-segment/per-field granularity the spec also has
  (vstart units, mask/segment effective length) is not modelled here.
* **The case runs under every profile** because the pre-existing registry entry
  carries no `profiles` restriction and may not be edited here. It is
  profile-independent (it reads no generated profile constant), so running it
  under p0 is harmless but does not exercise p0's absence of V.
