# RVV `vtype` field position — ratified layout (I-051 follow-up)

Case: `rvv.vtype_layout` (top `mosaic_vec_tb`, driver `sim/unit/tb_vec.cpp`).
Verdict: **PASS** on p0 and p2, with the three mutants below caught. The defect
was a specification-conformance bug: the whole delivered vector family placed
`vtype.vsew` at bits **7:5**, the pre-ratification position, and the I-052 lane
followed the recorded layout so its snapshot stayed consistent with the
descriptor it feeds. The two lanes agreed with each other and disagreed with the
specification; a program that reads `vtype` back and decodes `vsew` at the
ratified 5:3 would have computed the wrong SEW.

| artifact | sha256 |
|---|---|
| `rtl/core/mosaic_vec_desc.sv` | `d2949d785649ec1d9d9782adf1fd1072c96df6d0260fe2862c28679e9b3df462` |
| `rtl/core/mosaic_vec_cfg.sv` | `b01b294c7bbf5426907febf7d1e8573caf86b59a478011948c8be9b975818f36` |
| `config/csr/vector.json` | `919b4b371cbd6b9e9c86fc4af7275c963405042bf1f3d9cf475a8b69bd386469` |
| `sim/unit/tb_vec.cpp` | `1962a2e8924956a601e2a2a1c0b7e866b10818cf9272ff62615b510b1bad1f30` |
| `sim/tb/mosaic_vec_tb.sv` (unchanged) | `a490670f162b0a989b2475dddb43eb1179eb233b57bf6e07dfc522e7ea98b337` |
| `tools/run_vec_layout_controls.py` (new) | `372cac8465c994a5fc719da5c6817f35a6ffc8e411b4ccd9128e82e24268374c` |
| `tests/unit/registry.json` (untouched, `"pending": true`) | `748667d8277da309baf2c12d6739445747960bd79bd44660c8bf0fff78b17921` |
| shipping binary sha256, p0 == p2 (`build/p2/unit/rvv.vtype_layout/rvv.vtype_layout`) | `c44dd7a4475578d34ffec19eb42a1ecb1277c9211a1f37ada8a92dde2b7f73f4` |

`tests/unit/registry.json` and `config/status/implementation_status.json` were
not edited: the layout case was registered before this work and its
`"pending": true` marker is the integration lead's to clear. `sim/tb/mosaic_vec_tb.sv`
was **not** changed — the layout fields are port-mapped straight through, so the
wrapper carries no position of its own.

## 1. The specification's layout

RVV v1.0 (pinned tag `3570f998903f00352552b670f1f7b7334f0a144a`,
`riscv/riscv-v-spec`), `vtype-format.adoc` (included at L190 of `src/v-spec.adoc`;
the prose at L186-L188 names the five fields "vill, vma, vta, vsew[2:0], and
vlmul[2:0]"), for XLEN=64:

| bits | field | meaning |
|---|---|---|
| `63` | `vill` | previous `vset{i}vl{i}` attempted an unsupported value |
| `62:8` | reserved | must be written zero; reads zero (WARL) |
| `7` | `vma` | mask-agnostic policy |
| `6` | `vta` | tail-agnostic policy |
| `5:3` | `vsew[2:0]` | SEW = 2^vsew |
| `2:0` | `vlmul[2:0]` | LMUL encoding (100 reserved) |

The `vset{i}vl{i}` immediate `vtypei[10:0]` carries the same positions for bits
10:0, and bits 10:8 of it are reserved for the same reason bits 10:8 of a
`vsetvl` `rs2` word are.

## 2. Artifacts changed

* **`rtl/core/mosaic_vec_desc.sv`** — the descriptor's combinational legality
  decode read `sew_raw = vtype_i[7:5]`; now `vtype_i[5:3]`. A "the vtype layout"
  section was added to the header, and the two layout mutants added to the
  negative-control block of the file comment.
* **`rtl/core/mosaic_vec_cfg.sv`** — the configuration unit's decode points:
  `arg_vsew = arg[7:5]` → `arg[5:3]`; `vtype_supported` and `old_valid`
  (`vsew_ok`/`emul_ok` and `old_vlmax`) moved from `v[7:5]`/`vtype_q[7:5]` to
  `[5:3]`. The `MOSAIC_VEC_MUTANT_SILENT_M1` re-layout of the argument
  (`{arg_vsew, arg[4:3], arg_vlmul}`) became `{arg[7:6], arg_vsew, arg_vlmul}` so
  vta/vma stay at 6/7. The `vset` seam itself stores `arg[7:0]` verbatim (the
  field position is a decode property, not a storage property — see §5). Header
  comments updated: the layout section, the CSR-permissions note, the mutant list.
* **`config/csr/vector.json`** — the `vtype` `spec_clause` recorded the wrong bit
  map ("vma = bit 3, vta = bit 4, vsew[2:0] = bits 7:5"); now "vsew[2:0] = bits
  5:3, vta = bit 6, vma = bit 7". The same row's `access` was changed from `rwr`
  to `ro` (§7).
* **`sim/unit/tb_vec.cpp`** — the oracle encoders were **written from the
  specification** and are the case's contract with the machine:
  `Vtype(vsew,vlmul,vill)` (descriptor oracle, `vsew << 5` → `<< 3`),
  `Vtypei(vsew,vlmul,ta,ma)` (vset oracle; `ma` bit 7, `ta` bit 6, `vsew` bits
  5:3), and `WordSupported` (`(v >> 5)` → `(v >> 3)`). The new `rvv.vtype_layout`
  phase set was added, and `--case` dispatches to it.
* **`tools/run_vec_layout_controls.py`** (new) — the three controls, modelled on
  `tools/run_vec_cfg_controls.py`.

Nothing else encodes the constant; see §3.

## 3. How the copies were searched for

A field position is a constant that gets copied, and this project has paid three
times already for a case and its RTL agreeing about one. The search was
mechanical, not from memory of the four files named in the card:

* `grep -rn "vsew"` over `rtl/ sim/ config/ tools/ docs/` (`*.sv *.svh *.cpp *.h
  *.json *.py *.md`), excluding `build/`, `results/`, `.git/`, the research
  reports;
* `grep -rn "7:5|5:3|62:8|vlmul\[2:0\]|vsew\[2:0\]|vtype-format|vtype bit"` over
  the same tree, to catch the positions written without the word `vsew`;
* `grep -rn "vta|vma"` over `rtl/ sim/ config/ tools/ tests/` (filtering
  `sfence.vma`) to catch a decide that names the agnostic bits;
* `grep -rn "vtype"` over `config/csr/rule_ledger.json` (no `vtype` rule exists)
  and `tests/unit/registry.json` (the case entry), and `grep` over the generated
  `build/*/rtl/mosaic_csr_pkg.svh` for the `vtype` constants.

**Found and fixed (five independent copies of the same wrong constant):**

1. `rtl/core/mosaic_vec_desc.sv` — `sew_raw = vtype_i[7:5]`;
2. `rtl/core/mosaic_vec_cfg.sv` — `arg_vsew`, `vtype_supported`, `old_valid`,
   `old_vlmax`, and the `SILENT_M1` re-layout;
3. `sim/unit/tb_vec.cpp` — `Vtype`, `Vtypei` and `WordSupported` (the oracle
   agreed with the DUT by construction, which is why no existing check caught
   this);
4. `config/csr/vector.json` — the `vtype` `spec_clause` bit-map sentence;
5. comments in `mosaic_vec_cfg.sv` that stated the divergence as a deliberate
   family choice (now rewritten to the ratified positions).

**Checked and found clean:** `sim/tb/mosaic_vec_tb.sv` (pure port mapping, no
field position); `config/csr/rule_ledger.json` (no `vtype` rule);
`config/status/implementation_status.json` and `results/reports/I-051-*.md` /
`I-052-*.md` (historical records, not encoders — left as written);
`docs/` (the vector documents describe SEW/LMUL/vta/vma as concepts, never bit
positions); `build/*/rtl/mosaic_csr_pkg.svh` (the generated `vtype` constants are
an address, a reset, a write mask and a legality flag — no field position). The
only remaining `7:5` in the tree is inside the two mutant branches and the
comments that describe them.

## 4. The rest of the layout, field by field

| field | spec | implementation | verdict |
|---|---|---|---|
| `vlmul[2:0]` | bits 2:0 | `vtype_i[2:0]`, `arg[2:0]`, `Vtypei`/`Vtype` bit 2:0 | agrees |
| `vsew[5:3]` | bits 5:3 | fixed to `[5:3]` everywhere | agrees |
| `vta[6]` | bit 6 | stored verbatim (`arg[7:0]`); not decoded, because it does not affect legality | agrees (position not contradicted) |
| `vma[7]` | bit 7 | stored verbatim; not decoded for the same reason | agrees (position not contradicted) |
| `vill[63]` | bit XLEN-1 = 63 | `vtype_i[63]`, `v[63]`, `vtype_q[63]`, `VTYPE_VILL = 1<<63` | agrees |
| reserved `62:8` | written zero, reads zero | supported path stores `{56'b0, arg[7:0]}` (so 62:8 = 0); unsupported path stores `VTYPE_VILL` (62:0 = 0); reset stores `VTYPE_VILL`; a nonzero reserved bit in the `vset` argument makes the value unsupported (vill), per "all bits of the vtype argument must be considered" | agrees |

**No second wrong field position was found.** Two things are worth recording as
*not* position errors:

* `vta`/`vma` are never *consumed*. The tail-agnostic and mask-agnostic element
  policies are element-level behaviour owned by the later vector packages
  (I-056+); the descriptor and the configuration unit deliberately do not read
  the bits. Their positions in the stored word are nevertheless asserted by the
  case (bit 6 / bit 7).
* The generated `MOSAIC_CSR_MINPRIV_W_VTYPE` is `2'd3`, decoded from the CSR
  *address* bits [11:10] like every other CSR, not from the field layout. It is
  unchanged by this work and is not a `vtype` field.
* **One scope boundary, not a position error:** the descriptor's legality query
  itself does not test bits 62:8 — `vtype_legal_c` checks only `vill`,
  `vsew`/`vlmul` and the EMUL floor. The reserved-bit rule lives in the
  configuration unit, which is the only producer of a `vtype` word and sets
  `vill` for any reserved bit. A word with a reserved bit set and `vill` clear is
  therefore unreachable from `vset{i}vl{i}`, and the descriptor would accept such
  a word if handed one directly (the matrix case only sweeps `vsew`/`vlmul`/
  `vill`, and the layout case feeds only spec-formed words). Flagged rather than
  changed: the descriptor is a consumer of a formed configuration, and adding a
  second copy of the reserved-bit rule there would be the kind of duplicated
  constant this package exists to remove.

## 5. The case and the round-trip evidence

`rvv.vtype_layout` runs three phases (1253 checks, 88 encoded words, 193 cycles):

1. **`vtype-roundtrip`** — every legal SEW (8/16/32/64) × every legal LMUL × all
   four `vta`/`vma` combinations = 88 words. Each word is built by `Vtypei()` at
   the specification's positions, driven through `vsetvli`, and the unit must:
   decode the encoded SEW/LMUL (checked through `VLMAX = VLEN·LMUL/SEW` and the
   chosen `vl`), read the same word back, decode `vsew` at 5:3, `vlmul` at 2:0,
   `vta` at 6, `vma` at 7, read bits 62:8 as zero, and return the same word on a
   CSR read of 0xC21.
2. **`vtype-layout descriptor`** — the same 88 spec-encoded words are presented
   to the descriptor's legality query (opivv, disjoint registers). It must
   accept each legal configuration and report `sew_log2` and `lmul_exp` from the
   encoded fields and `elem_count` from the encoded SEW/LMUL.
3. **`vtype-layout vill` / `reserved` / `uro`** — an unsupported configuration
   (`vsew = 0`, SEW = 1) sets `vill`, zeroes `vtype[62:0]` and sets `vl = 0`; a
   reserved bit (8, 30, 62) in a `vsetvl` argument sets `vill`; a software CSR
   write to `vtype` (including one aimed at a reserved bit, or one trying to set
   or clear `vill`) is illegal and the register is unchanged and reads
   `vtype[62:8] = 0`. That last group is the URO rule the I-052 lane recorded.

Because the encoders are written from the manual and the expected decode is
computed from what was encoded, the case fails if any decode point disagrees with
the specification — which is exactly what the `LEGACY_75` mutant demonstrates.

## 6. Mutant table

`python3 tools/run_vec_layout_controls.py --profile p2`. Each mutant is rebuilt
from a **deleted** directory with its `-D` on that build's own recorded command
line (`build/p2/vec_layout_controls/<define>/build_command.txt`), must differ from
the shipping binary, must exit 1, and its first `CHECK FAILED` must name the
check.

| mutant | injects | exit | sha256 | first failure |
|---|---|---|---|---|
| `MOSAIC_VEC_MUTANT_VTYPE_LEGACY_75` | `vsew` back at 7:5 (with vta/vma at 4/3) in the descriptor **and** the configuration unit | 1 | `93b19c129ae2cf93a69c94e904e44275c159eed58ecebacf59a8b448de7a4348` | `vtype-roundtrip vsew=3 vlmul=0 ta=0 ma=0: a legal configuration was not accepted` |
| `MOSAIC_VEC_MUTANT_VTYPE_SEW_WRONG` | descriptor decodes SEW from bits 4:2, so a legal configuration is misread | 1 | `e264fedb088fb38a2fe0dfb0605a4271def0f471313580f3136facea4e8417d6` | `vtype-layout descriptor vsew=3 vlmul=0 ta=0 ma=0: sew_log2 decoded from the wrong bits` |
| `MOSAIC_VEC_MUTANT_VTYPE_SW_WRITE` | a software CSR write to `vtype` is accepted, so software can write `vill` | 1 | `90c5ccb58c9b6f0ff63ab53d2d4be722006159766b6dcf6f0e23c135c323f00f` | `vtype-layout uro: software set vill through a vtype CSR write` |

Shipping baseline: `RESULT PASS rvv.vtype_layout checks=1253 words=88
descriptors=88 cycles=193`, binary sha256
`c44dd7a4475578d34ffec19eb42a1ecb1277c9211a1f37ada8a92dde2b7f73f4`. All three
mutants mutate the binary, exit 1 and name the check they break.

The pre-existing controls were re-run because the descriptor and the
configuration unit changed: `tools/run_vec_controls.py --profile p2` catches all
three `rvv.descriptor_legality` mutants, and `tools/run_vec_cfg_controls.py
--profile p2` catches all four `rvv.vset_boundaries` mutants (their shipping
binaries have new hashes, as expected).

## 7. The two loose ends I-052 recorded

**`vtype` `access` vs the URO rule — the table was wrong, the implementation was
right.** `config/csr/vector.json` marked `vtype` `access: rwr` with writable
fields `7:0`/`63`; the configuration unit implements the specification's
read-only rule (a software CSR write raises illegal-instruction; only
`vset{i}vl{i}` updates it). The specification is unambiguous: L104 lists `0xC21`
as **URO** and L175-L177 says it "can only be updated by vset{i}vl{i}
instructions". `rwr` describes the *storage* `vset` writes, not software
permission, so the table row was the wrong half. It now reads `access: "ro"`,
`behavior: "fixed"`, `unmodifiable_bits: ["63:0"]` — the same shape as `vl`
(0xC20) and `vlenb` (0xC22), which are also hardware-updated and software
read-only. The generated `MOSAIC_CSR_WMASK_VTYPE` fell from
`0x80000000000000ff` to `0x0` and `MOSAIC_CSR_WRITE_LEGAL_VTYPE` from `1'b1` to
`1'b0`, which is the table and the RTL now saying the same thing. The
`spec_clause` was extended to say so.

**Empty `build/p2/rtl/mosaic_csr_pkg.svh`.** During I-052's run the p2 generated
CSR package was 0 bytes because a concurrent CSR change was mid-flight, so p2
conclusions from that tree were unreliable. Every profile was regenerated before
any p2 build here:

```
build/p0/rtl/mosaic_csr_pkg.svh  37233 bytes
build/p1/rtl/mosaic_csr_pkg.svh  53804 bytes
build/p2/rtl/mosaic_csr_pkg.svh  46976 bytes   (VTYPE wmask 0x0, write-legal 0)
build/p3/rtl/mosaic_csr_pkg.svh  46976 bytes
```

The p2 `vtype` row is `ADDR=0xc21, RESET=0x8000_0000_0000_0000, WMASK=0,
WRITE_LEGAL=0`. Nothing in `rtl/` names `MOSAIC_CSR_*_VTYPE` (the vector CSRs are
owned by `mosaic_vec_cfg`, not `mosaic_csr`), so the generated change is
checked-consistent table content, not a new RTL path. No generated file was
hand-edited.

## 8. Keep-passing evidence (re-run this wave)

| case | profile | result |
|---|---|---|
| `rvv.descriptor_legality` | p0, p2 | PASS `checks=336 combos=9284 cycles=28` |
| `rvv.vset_boundaries` | p0, p2 | PASS `checks=571 vtypes=64 avl_bands=36 cycles=276` |
| `rvv.vtype_layout` | p0, p2 | PASS `checks=1253 words=88 descriptors=88 cycles=193` |
| `core.act_dut` | p1 | **PASS `applicable=127 generated=127 run=127 passed=127 failed=0`**, `act_commit=96493a91448ca50780013fd892daec2c204487ba` |
| `core.corpus_sweep` | p0 | PASS `programs=13 inputs=3 runs=39 pass=39 fail=0 cycles=17484988 retires=2774478` |
| `privilege.permission_matrix` | p1 | PASS `checks=6411 comparisons=57 cycles=22954 retires=9427 traps=53` |
| `csr.rule_ledger` | p0 | PASS `rules=86 examples=172 checks=4 comparisons=7490` |

ACT4 is re-run and stays **127/127**. `csr.rule_ledger` is a p0 case (its ledger
covers p0); a p2 run reports "the rule ledger is empty", which is the case's
pre-existing profile contract, not a regression from this change. The vector
cases pass under both p0 and p2 from the same source.

## 9. Gates

* `make check`: **exit 0** (`records agree: 64 delivered package(s), 72
  registered case(s)`; `check_exclusions.py` and `--negative` green; negative
  mutations rejected as designed).
* `tools/lint_rtl.py --profile p0`: **50 of 50 clean**; `--profile p2`: **50 of
  50 clean** (both `mosaic_vec_desc` and `mosaic_vec_cfg` included).
* `slang-tidy --std 1800-2017 --single-unit`: **exit 0** for p0 and p2. The only
  lines naming the vector files are the same STYLE-13
  unnamed-`always_comb` warnings the existing vector/cache modules carry; no new
  category.
* RTL style: no assignment patterns, no interfaces, explicit widths, `logic`.

## 10. Not covered (honest list)

* **`vta`/`vma` are stored, not honoured.** Tail-agnostic and mask-agnostic
  element policy (what vta/vma *do* to elements) is element-level behaviour for
  the later vector packages (I-056+). The bits are round-tripped at 6/7 but no
  check exercises agnostic-tail or agnostic-mask fills.
* **No core integration.** `mosaic_vec_desc`/`mosaic_vec_cfg` are module-level
  packages; the case drives them through `mosaic_vec_tb`, not through a decoded
  `vsetvli` in `mosaic_core`. The instruction-encoding seam (decoder's `vtypei`
  immediate extraction) is separate work and was not touched.
* **`vsetivli`'s immediate width.** The unit takes an 11-bit `vset_vtypei_i` for
  all three forms; the ratified `vsetivli` carries only `vtypei[9:0]`. This is a
  pre-existing interface detail, independent of the field positions, and is not
  changed or asserted here.
* **`vstart` out of bounds** and the reserved-value traps remain as I-052
  recorded; unchanged.
* **The five deleted-build binaries are p2 only.** The mutant table is p2; the
  case itself is proven under p0 and p2. No p1/p3 mutant run was made.
* **`csr.precise_trap_mret` (p2) uses the regenerated table but was not re-run**
  in this wave; it does not exercise a vector CSR, and the p2 `vtype` row change
  only moves it from "software-writable" to "software-read-only", which matches
  its reset-only value.
