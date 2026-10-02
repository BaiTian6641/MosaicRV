# `rvv.mask_prefix_masked` — the masked forms of `vmsbf`/`vmsif`/`vmsof`

## STATUS: COMPLETE (unit)

| Requirement | Evidence |
| --- | --- |
| The masked rules are established from the specification, not the RTL | §"The rules, as rules"; anchored to the spec's own printed masked examples |
| The search is over the **active** elements only, and a masked-off source element is ignored | `MaskedPrefixExpectedBits` (host oracle) and 3 spec-anchor cells; `rvv.mask_prefix_masked` PASS |
| The RTL already implements the rule, so no value-path fix was needed | §"What the RTL does"; the running prefix is gated on `sel_active` (`mosaic_vec_alu.sv:1506`) |
| First *active* set bit at every position, all three ops | `masked-position` sweep: 3 ops × 16 positions = 48 cells, plus a coverage assertion; PASS |
| A source whose only set bit is masked off (active search finds none) | `masked-off-only` sweep: 3 ops × 16 = 48 cells; PASS |
| A set bit masked off *before* an active one (the two searches disagree about `k`) | `masked-off-before-active` sweep: 3 ops × 15 = 45 cells; PASS |
| `vma = 0` and `vma = 1` on a masked mask destination | 2 patterns × 2 `vma` × 3 ops = 12 cells; PASS |
| `vta` past `vl` | 2 patterns × 2 `vta` × 3 ops = 12 cells; PASS |
| Expectation host-computed, not read from the RTL | `MaskedPrefixExpectedBits` + the spec's printed results (3 anchors); neither reads the RTL |
| Deterministic | PASS with identical `checks=2870` at seeds 1, 2, 7, 12345 |
| Fail mode — search over all elements | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_SEARCH_ALL`, exit 1 first failure `masked-off-before-active k=2 vmsbf bit1: 0 expected 1` |
| Fail mode — a masked-off element contributes a set bit | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_MASKED_SRC`, exit 1 first failure `masked-position k=1 vmsif bit1: 0 expected 1` |
| Fail mode — `vma` wrong on a masked destination | `MOSAIC_VEC_ALU_MUTANT_MASKPFX_VMA`, exit 1 first failure `vma=1 pattern=0 vmsbf bit4: 0 expected 1` |
| Registered case runs and passes | `RESULT PASS rvv.mask_prefix_masked checks=2870 ops=3 positions=16 masked_before=45 masked_only=48 vma=12 vta=12 anchors=3 cycles=32365`, exit 0 |
| Sibling vector cases unchanged | `rvv.mask_prefix_semantics`, `rvv.mask_prefix_vstart`, `rvv.integer_mask_permute`, `rvv.memory_modes`, `rvv.partial_fault_restart`, `rvv.stop_path_inflight`, `rvv.fp_flags_reduction`, `rvv.chaining_hazards`, `vec.integrated`, `rvv.lane_resize_boundary`, `coalesce.element_faults` all PASS |
| Core regression | `core.act_dut` (p1) `applicable=127 generated=127 run=127 passed=127 failed=0` |
| Gates | `make check` exit 0; `lint_rtl` p0/p1 63/63 clean; `slang-tidy` exit 0; `check_records` exit 0; `check_exclusions` and `--negative` exit 0 |

---

## 1. The rules, as rules

Source: the pinned V spec, `~/mosaic-ref/riscv-v-spec/v-spec.adoc`, the three
subsections `vmsbf.m` set-before-first (line 4220), `vmsif.m`
set-including-first (4264) and `vmsof.m` set-only-first (4300). For a masked
instruction `vd, vs2, v0.t`, an element `i` is **active** iff `v0.mask[i]` is set
(within `[0, vl)`; `vstart` is separately constrained, §5). Let the **first set
bit over the active elements** be at position `k` — `k = min{ i active :
vs2[i] = 1 }` — and let "no `k`" mean every active source bit is zero. Then for
every active element `i`:

* **`vmsbf.m` — before the first.** `vd[i] = 1` iff `i < k`. The spec: *"writes
  a 1 to all active mask elements before the first active source element that is
  a 1, then writes a 0 to that element and all following active elements. If
  there is no set bit in the active elements of the source vector, then all
  active elements in the destination are written with a 1."*
* **`vmsif.m` — through the first.** `vd[i] = 1` iff `i <= k`. The spec:
  *"similar to set-before-first, except it also includes the element with a set
  bit."* With no such element there is nothing to include, so it coincides with
  set-before-first: **all active elements set**.
* **`vmsof.m` — only the first.** `vd[i] = 1` iff `i == k`. The spec: *"similar
  to set-before-first, except it only sets the first element with a bit set, if
  any."* With no such element, **no active element is set**.

**The masked-off source contributes nothing.** The search is over the active
elements: a masked-off source element is not an element of the search at all.
Treating it as a zero and ignoring it agree on `k`; what the specification
forbids is letting it contribute. This is exactly what the spec's own masked
example pins down:

```
    1 1 0 0 0 0 1 1   v0 (mask)
    1 0 0 1 0 1 0 0   v3 (source)
                      vmsbf.m v2, v3, v0.t
    0 1 x x x x 1 1   v2
```

Elements 2 and 4 have set source bits but are masked off; they are not "the
first". The active elements are `{0,1,6,7}`, the first active set bit is
element 7, so elements 0, 1 and 6 get 1 and element 7 gets 0 — the printed
result. (For `vmsof` the printed example uses `v3 = 11010100`, whose first
active set bit is element 6.)

**Destination policy when the destination is a mask register.** A masked-off
element (`i < vl`, `v0.mask[i] = 0`) and a tail element (`i >= vl`) follow the
standard `vma` / `vta` policy. The mask destination's agnostic value is
**all-ones** (v-spec.adoc 452–456: for `vmsbf.m`/`vmsif.m`/`vmsof.m` *"any
element in the tail of the result can be written with the value the
mask-producing operation would have calculated"*, i.e. all bits of the mask
register set):

| element | `vma`/`vta` = 0 | `vma`/`vta` = 1 |
| --- | --- | --- |
| masked-off (`i < vl`, mask 0) | unchanged | 1 |
| tail (`i >= vl`) | unchanged | 1 |
| active | the rule above | the rule above |

The spec's printed examples show `x` for the masked-off elements, i.e. `vma = 0`.

## 2. What the RTL does — already correct, so no value-path fix

The reading says the RTL is **already right**, and the deliverable is the
coverage that proves it. The mechanism, in `rtl/core/mosaic_vec_alu.sv`:

* The per-element prefix is `sel_pfx = ef_pfx | bbit` (line 838), but the
  packet engine updates the running prefix **only for an active element**:
  `if (sel_active) pfx_q <= sel_pfx;` (line 1506). A masked-off element takes
  the `E_PLAN → E_EXEC` path with no source access, so its source bit never
  reaches `pfx_q`; the search is over the active elements by construction.
* The write policy is `wr = mm ? 1 : vma` inside `[vstart, vl)` and `wr = vta`
  at/after `vl` (lines 582–586), and an inactive element with `wr = 1` is
  written with all-ones: `if (!acc && wr) begin sel_result = zz; sel_mres = 1; end`
  (lines 920–923). So `vma = 0` leaves a masked-off mask destination undisturbed
  and `vma = 1` writes it with 1; the tail behaves the same under `vta`.

A masked-off element performs no source access, so it cannot trap and cannot
change the search. The registered case passes on the shipping build.

*Observation, deliberately not changed here.* The single-element lane's prefix
output `e_pfx_o = sel_pfx & e_valid_i` does **not** gate on the element being
active, so for a masked-off element it still carries `ef_pfx | bbit`. The packet
engine does not use `e_pfx_o` (it uses `sel_pfx` and gates on `sel_active`), so
this is unobservable in the unit and was left alone rather than changed without
a case that can see it. It is recorded in §6.

## 3. The case `rvv.mask_prefix_masked`

`RunMaskPrefixMaskedCase` in `sim/unit/tb_vec.cpp` (registered, `top`
`mosaic_vec_tb`, `"pending": true`; the integration lead clears the marker).
Each cell drives one packet with the mask operand (`mask_en = true`, `v0`), SEW=8
/ LMUL=1 (a mask register), `vstart = 0`, reads the destination mask back from
the VRF and compares every bit.

### The host oracle

`MaskedPrefixExpectedBits(op, src, mask, old, vl, vma, vta)`, a host function in
`tb_vec.cpp`, is the rule set of §1 stated directly and computed from the source
and mask patterns; it never reads the RTL. Three cells are checked **not**
through it but against the specification's own printed masked result
(`spec-anchor`), so a shared misreading of the rules cannot make both sides
agree: `v0 = 0xC3` with `vs2 = 0x94` for `vmsbf`/`vmsif` (active bits
`0x43`/`0xC3`) and `vs2 = 0xD4` for `vmsof` (active bit `0x40`), with the `x`
elements left at the destination seed.

### Coverage table (168 cells; `checks = 168×17 + 6 coverage + 8 harness = 2870`)

| Phase | operation × setting | cells | what it proves |
| --- | --- | --- | --- |
| `masked-position k=0..15` | 3 ops × 16 positions | 48 | first *active* set bit at every addressable position; the mask turns off exactly the elements below `k` |
| `masked-off-before-active k=1..15` | 3 ops × 15 | 45 | a set bit masked off before an active set bit — the two searches disagree about `k` (over-all gives `k=0`) |
| `masked-off-only k=0..15` | 3 ops × 16 | 48 | the only set bit is masked off, so the active search finds none; active slice all-zero (`vmsbf`/`vmsif` all-ones, `vmsof` all-zeros) |
| `vma=0/1 pattern=0/1` | 2 patterns × 2 `vma` × 3 ops | 12 | masked-off mask destination: undisturbed under `vma=0`, all-ones under `vma=1`; patterns place masked-off elements before and after `k`, and include a masked-off set bit |
| `vta=0/1 tail-below-set / tail-above-set` | 2 patterns × 2 `vta` × 3 ops | 12 | `vl=8`, tail `[8,16)` undisturbed under `vta=0`, all-ones under `vta=1`; a source bit above `vl` is not a first set bit |
| `spec-anchor` | 3 ops | 3 | the specification's printed masked results (oracle-independence) |

Six coverage assertions require the cell counts above; all pass. Every cell also
asserts `!alu_illegal && !alu_trap`.

## 4. Negative controls

`tools/run_vec_maskpfx_controls.py --case rvv.mask_prefix_masked` (the tool now
carries all three mask-prefix cases). Each mutant is rebuilt **from a deleted
build directory**, its `-D` is on that build's recorded `build_command.txt`, its
binary hash differs from the shipping one, and it exits 1 with a named first
failure:

| build | sha256 | exit | first failure |
| --- | --- | --- | --- |
| *(shipping)* | `fad6238c4b38d4c40fe5b6933cb8dec065dda227041b557dd267a52443aedea2` | 0 | `RESULT PASS` |
| `MOSAIC_VEC_ALU_MUTANT_MASKPFX_SEARCH_ALL` | `9975b63ea8d12b28190d208054823868d8f1e487e82bca953bfb4378bf677c1e` | 1 | `masked-off-before-active k=2 vmsbf bit1: 0 expected 1` |
| `MOSAIC_VEC_ALU_MUTANT_MASKPFX_MASKED_SRC` | `2d2a0d5a9a86b93c539464c356243dbf5149e2bb52b607c9fc5dc37a0ad7d7db` | 1 | `masked-position k=1 vmsif bit1: 0 expected 1` |
| `MOSAIC_VEC_ALU_MUTANT_MASKPFX_VMA` | `7abd6ebe337f85fdb1237844b044eea65749ee345ada9c44f8c554dcb349e8c5` | 1 | `vma=1 pattern=0 vmsbf bit4: 0 expected 1` |

* `SEARCH_ALL` — the search is over all elements, not the active ones: a
  masked-off element of a mask-prefix instruction takes the source-access path
  (`E_PLAN → E_VS1`) and its own source bit enters the running prefix. This is
  the defect class the package exists for; it is caught by the
  `masked-off-before-active` sweep.
* `MASKED_SRC` — a masked-off source element contributes a **1** to the search
  instead of being ignored, so a masked-off element with a *zero* source bit can
  become "the first". This is the "reverse" error §1 forbids; it is caught by
  the `masked-position` sweep (which is the cell the search-over-all defect
  passes, so the two mutants fail different phases and name different checks).
* `VMA` — a masked-off destination element of a mask-prefix instruction under
  `vma = 1` is written with 0 instead of the mask-agnostic all-ones.

The pre-existing controls still hold: `rvv.mask_prefix_semantics` (3 mutants)
and `rvv.mask_prefix_vstart` (3 mutants) both report *"all 3 mutants mutate the
binary, exit 1 and name the check they break"*.

## 5. `vstart` interaction

The `vstart` rule is a separate package (`rvv.mask_prefix_vstart`, I-057): a
non-zero `vstart` is an illegal-instruction exception for all three operations.
The gate is `ef_family == F_MASKPFX && cfg_vstart_i != 0` and does **not**
inspect the mask operand, so a *masked* prefix instruction with a non-zero
`vstart` is refused before the mask is consulted, exactly as the unmasked form
is. The masked case therefore runs only at `vstart = 0`; the composition
(masked + non-zero `vstart`) is not separately exercised here, because the
`vstart` package already decides it at whole-instruction granularity and the
mask cannot change the answer.

## 6. Not covered

* **Core-level routing.** The unit is still not wired for a masked prefix test
  at core level. `mosaic_core.sv:1494` gives its ALU lane
  `VEC_ALU_CAPS = 17'b…0100_0001` — `ADDSUB` and `LOGIC` only — so the `MASKPFX`
  family bit (11) is **not advertised**: an `vmsbf`/`vmsif`/`vmsof` routed to the
  core lane is refused as an illegal instruction by the capability gate
  (`!caps_i[ef_family]`). The core cannot execute these operations at all today,
  masked or unmasked, and no core-level test exercises them. (The descriptor,
  `mosaic_vec_desc.sv`, does classify `VOP_VMASKMV`.)
* **The element lane's prefix output for a masked-off element.** `e_pfx_o` is not
  gated on activity (§2); no unit case can observe it, and the packet engine is
  correct, so it was not changed.
* **`vl < 16` with masked-off elements inside `[0, vl)`.** The `vta` cells use
  `vl = 8` but with an all-active mask; the masked sweeps use `vl = 16`. A
  masked-off element below a short `vl` is not separately exercised (the rules
  compose, but the cell is not present).
* **Multiple destination mask registers / overlap.** The destination register
  cannot overlap the source or (masked) `v0` per the spec; the descriptor's
  overlap rule is exercised by other cases, not here.
* **The interaction of the masked forms with a non-zero `vstart`** — see §5.
