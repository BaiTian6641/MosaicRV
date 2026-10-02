# I-010 / I-011 — RV64I/M decode, the integer ALU, and branch targets

Work package **I-011** (integer ALU and branch target), run as
`CASE=alu.boundaries` from `tests/unit/registry.json`. The card is
`### I-011 — 实现整数 ALU 与分支目标计算` in `docs/stage-1-scalar-control.md`.

The card has two halves and this document covers both: the **integer ALU**
(`mosaic_alu`, the sixteen `alu_op_e` encodings) and the **branch/target half**
(`mosaic_branch_target` plus its comparator `mosaic_branch_cmp`). Both are covered
by the one case the card names.

## What was built

| File | Contents |
| --- | --- |
| `rtl/core/mosaic_alu.sv` | `mosaic_alu`, all sixteen `alu_op_e` encodings, purely combinational |
| `rtl/core/mosaic_branch_target.sv` | `mosaic_branch_target`: link value, taken/not-taken target, transfer predicate |
| `rtl/core/mosaic_branch_cmp.sv` | `mosaic_branch_cmp`: the BEQ/BNE/BLT/BGE/BLTU/BGEU comparison |
| `sim/tb/mosaic_alu_tb.sv` | Port-for-port simulation wrapper for all three units |
| `sim/unit/tb_alu.cpp` | Independent reference models, boundary matrix, shift sweep, branch sweep, closed comparator-to-target chain, seeded random campaigns, coverage assertions, nine negative controls |

`tests/unit/registry.json` was edited once, to add `mosaic_branch_target.sv` and
`mosaic_branch_cmp.sv` to the `rtl` list of `alu.boundaries`; without it the case
cannot build the new units. Nothing else outside this file list was touched.

---

# Part I — the integer ALU

### Interface

`mosaic_alu #(.XLEN(int))`, XLEN = 64 in every build of this profile.

| Port | Dir | Width | Meaning |
| --- | --- | --- | --- |
| `a` | in | `XLEN` | first operand |
| `b` | in | `XLEN` | second operand, or the shift amount for the six shift ops |
| `op` | in | 4 | `mosaic_pkg::alu_op_e` encoding |
| `result` | out | `XLEN` | the RV64 answer, fully defined for every input |
| `zero` | out | 1 | 1 exactly when `result == 0` |

No clock, no reset, no state. `op` is exactly as wide as the enum, so there is no
illegal `op`: all sixteen encodings are decoded, and the `default` arm is
unreachable.

### Behaviour, in prose

`result` is the RV64I definition of `(a, b, op)` at `XLEN` bits. Three rules carry
almost all of the risk, and each is implemented in exactly one place:

1. **The `*W` forms sign-extend.** `addw`, `subw`, `sllw`, `srlw` and `sraw` read
   `a[31:0]` and `b[31:0]` only, compute a 32-bit answer, and that answer is then
   sign-extended into the 64-bit result lane. They do not zero-extend, and bits
   63:32 of the operands never reach the result. `addw(0x7fffffff, 1)` is
   `0xffffffff80000000`. The rule lives in one expression, `extend_word()`.
2. **Shift amounts are masked to the destination width.** `sll`/`srl`/`sra` use
   `b[5:0]`, so `sll(x, 64) == x`. `sllw`/`srlw`/`sraw` use `b[4:0]`, so
   `sllw(x, 32) == x` and `sraw(x, 0x80000000) == x` — taking six bits there is the
   classic RV64 defect, and is negative control 3 below.
3. **Comparisons are full-width 0/1 with a per-op signedness.** `slt` reads its two
   operands as `int64_t`, so `slt(-1, 1) == 1`; `sltu` reads them as `uint64_t`, so
   the same pair gives 0, because `0xffff…` is the largest unsigned value there is.
   `zero` is derived from `result`, never from the operands: `add(1, -1)` is zero
   with `a != 0`, and a word form whose 32-bit answer is zero sign-extends to zero
   and still reports `zero == 1`.

`ALU_PASSB` returns `b` unchanged, bit for bit, and exists so address-generation
paths can route an operand through the ALU instead of around it.

### Instruction table implemented

| Encoding | `alu_op_e` | Result | RV64 instructions |
| --- | --- | --- | --- |
| `4'd0` | `ALU_ADD` | `a + b`, modulo 2^64 | `add`, `addi` |
| `4'd1` | `ALU_SUB` | `a - b`, modulo 2^64 | `sub` |
| `4'd2` | `ALU_SLL` | `a << b[5:0]` | `sll`, `slli` |
| `4'd3` | `ALU_SLT` | `($signed(a) < $signed(b)) ? 1 : 0` | `slt`, `slti` |
| `4'd4` | `ALU_SLTU` | `($unsigned(a) < $unsigned(b)) ? 1 : 0` | `sltu`, `sltiu` |
| `4'd5` | `ALU_XOR` | `a ^ b` | `xor`, `xori` |
| `4'd6` | `ALU_SRL` | `a >> b[5:0]` | `srl`, `srli` |
| `4'd7` | `ALU_SRA` | `$signed(a) >>> b[5:0]` | `sra`, `srai` |
| `4'd8` | `ALU_OR` | `a \| b` | `or`, `ori` |
| `4'd9` | `ALU_AND` | `a & b` | `and`, `andi` |
| `4'd10` | `ALU_ADDW` | `sext32(a[31:0] + b[31:0])` | `addw`, `addiw` |
| `4'd11` | `ALU_SUBW` | `sext32(a[31:0] - b[31:0])` | `subw` |
| `4'd12` | `ALU_SLLW` | `sext32(a[31:0] << b[4:0])` | `sllw`, `slliw` |
| `4'd13` | `ALU_SRLW` | `sext32(a[31:0] >> b[4:0])` | `srlw`, `srliw` |
| `4'd14` | `ALU_SRAW` | `sext32($signed(a[31:0]) >>> b[4:0])` | `sraw`, `sraiw` |
| `4'd15` | `ALU_PASSB` | `b` | address generation |

`mul`, `div` and `rem` are deliberately **not** here: they belong to the M unit
(I-012), which consumes `a` and `b` directly, and a partial version here would be a
second source of truth for the same instructions.

### Carry / overflow flags

There is no carry output, and none is planned. RV64I defines `add`/`sub` to wrap
modulo 2^64 and `addw`/`subw` to wrap modulo 2^32, so the adder's carry-out carries
no architectural state in this profile. The only consumer of a carry in base RV64I
would be a carry-propagating shift, and there is none: M has its own multiply unit
and A is not in the p0 profile. So for `sub` with `a < 0` — the case the assignment
calls out — nothing is dropped: there is no carry flag to drop and no downstream
user in this design wants one. The three questions RV64I *does* give an
architectural answer to — `slt`, `sltu` and the zero flag — are answered inside
this block.

### ALU stimulus

**Operand set (20 values)** — `0`, `1`, `2`, `3`, `-1`, `-2`, `INT64_MIN`,
`INT64_MAX`, `0x7fffffff`, `0xffffffff7fffffff`, `0x80000000`,
`0xffffffff80000000`, `0xffffffff`, `0xffffffff00000000`, `0xffff`, `0xffff0000`,
`0xaaaa…`, `0x5555…`, `0x100000000`, `0x100000001`. Each is present because it is a
point where an answer changes: all three extensions of the 32-bit boundary values
are there, so a word form that zero-extends and a word form that sign-extends
disagree on most of them; and the two values differing only above bit 32 separate
the W forms from the 64-bit forms.

**Shift amounts (9)** — `0, 1, 31, 32, 63, 64, 65, 127, 0xffff…ff`. `31`/`32`
straddle the W boundary, `63` is the largest legal 64-bit shift, `64`/`65`/`127`
exercise the wrap, and the all-ones mask masks to 63 for the 64-bit forms and to 31
for the W forms, separating the two masking rules by construction.

| Phase | Vectors | What it is |
| --- | --- | --- |
| operand matrix | 6 400 | all 20 × 20 pairs × all 16 ops |
| shift sweep | 2 160 | 6 shift ops × 20 boundary values × 9 amounts, in both operand orders |
| random campaign | 100 000 | `--seed`ed, biased onto the interesting shapes |
| **total applied** | **108 560** | seed 1 |

Half the random vectors are biased onto shapes that uniform noise essentially never
produces: a boundary operand on one side; a small `b` (a realistic shift amount)
with a shift op; a `b` whose bits are all set above bit 5; operands differing only
above bit 32; low-entropy all-ones/all-zeros operands; and sign-crossing pairs equal
in magnitude but differing in sign.

### The ALU reference model, and how each expected value is computed

`ReferenceAlu()` is written from the RV64I pseudo-code in host integer arithmetic —
`uint64_t`/`int64_t` for the 64-bit forms, `uint32_t` for the W forms — and shares
no expression with the RTL. Per op:

* `add`/`sub`: `ua + ub`, `ua - ub` on `uint64_t`. Unsigned arithmetic is defined to
  wrap, which is exactly RV64's rule.
* `sll`/`srl`: `ua << (ub & 63)`, `ua >> (ub & 63)`. The mask is written out rather
  than left to a shift that would be undefined or saturating.
* `sra`: `ArithmeticShiftRight64(ua, ub & 63)`, which builds the sign fill from
  **unsigned** operations (`fill = ~0 << (64-n)`) instead of shifting a signed type.
* `slt`: `static_cast<int64_t>(a) < static_cast<int64_t>(b)`; `sltu`: `a < b` as
  `uint64_t`.
* `xor`/`or`/`and`: the obvious bitwise operators.
* `addw`/`subw`/`sllw`/`srlw`/`sraw`: truncate to `uint32_t` **first**
  (`aw = (uint32_t)a`, `bw = (uint32_t)b`), compute the 32-bit answer, then apply
  `SignExtend32()`, which fills bits 63:32 from bit 31 by hand.
* `passb`: `ub`.

**The three spellings that silently compute the wrong thing**, all of them live traps
in this file, and all three avoided by the model:

* `(int64_t)(uint32_t)(a + b)` is not `addw` — `a + b` is evaluated in 64 bits
  *before* the truncation, so `addw(0xffffffff80000000, 1)` comes out as `0` in that
  spelling and `0x0000000080000000` in `addw`'s real definition. The model truncates
  first.
* `(uint64_t)(uint32_t)x` is not the W rule either; that is zero-extension, which is
  precisely what negative control 1 injects into the RTL.
* `a >> n` on a signed type is `sra` in intent but implementation-defined before
  C++20. There is no signed right shift anywhere in the reference.

The reference's one remaining implementation-defined step — the `uint64_t → int64_t`
conversion in `slt` — is not left load bearing. `CheckModelSelfConsistency()` runs it
over all 400 boundary pairs against `SignedLessByBias()`, a second formulation that
never leaves unsigned arithmetic (flipping bit 63 of both operands turns the signed
order into the unsigned order), and fails the case if the two ever disagree. It also
proves all sixteen ops are reachable through the switch rather than falling through
to the unreachable sentinel.

The 25 hand-written ALU invariants are checked against the RTL **without going
through the model at all**, so a shared mistake in `ReferenceAlu` cannot make the
model and the RTL agree for the wrong reason.

---

# Part II — branch condition, link value and jump target

### Why the comparator is a second module

The port list fixed for `mosaic_branch_target` takes `branch_taken` as an *input*:
the comparison result arrives from outside. The card nevertheless requires the branch
comparison to be signed for BLT/BGE and unsigned for BLTU/BGEU, and requires a
negative control for a BLTU that compares signed. A unit with no operand inputs
cannot host either, so `mosaic_branch_cmp` is delivered as its own module in the file
next to it: one implementation of the six conditions, one place the signed/unsigned
rule lives, one place a mutant can break it. The core wires the comparator's `taken`
into the target unit's `branch_taken`, and the case checks that closed chain
explicitly as well as checking each unit against its own model.

### Interfaces

`mosaic_branch_target #(.XLEN(int))`

| Port | Dir | Width | Meaning |
| --- | --- | --- | --- |
| `pc` | in | `XLEN` | address of the instruction |
| `imm` | in | `XLEN` | branch/jump offset, already sign-extended by the decoder |
| `is_branch` | in | 1 | a conditional branch: BEQ…BGEU |
| `is_jal` | in | 1 | unconditional jump with its own immediate |
| `is_jalr` | in | 1 | unconditional jump whose target has bit 0 forced to 0 |
| `branch_taken` | in | 1 | the comparison result from `mosaic_branch_cmp`, or 0 |
| `link` | out | `XLEN` | `pc + 4`, the value JAL/JALR write to `rd` |
| `target` | out | `XLEN` | the address control transfers to, or `pc + 4` when it does not |
| `is_taken` | out | 1 | does control actually transfer |

`mosaic_branch_cmp #(.XLEN(int))`

| Port | Dir | Width | Meaning |
| --- | --- | --- | --- |
| `rs1_value` | in | `XLEN` | architectural value of rs1; x0 presents 0 |
| `rs2_value` | in | `XLEN` | architectural value of rs2; x0 presents 0 |
| `branch_funct` | in | 3 | the condition |
| `taken` | out | 1 | the comparison result |

### What decides control transfer

```
is_taken = is_jal | is_jalr | (is_branch & branch_taken)
target   = is_taken ? (is_jalr ? ((pc + imm) & ~64'd1) : (pc + imm)) : (pc + 4)
link     = pc + 4
```

**Nothing in this unit sniffs an instruction encoding.** That is the part of the
design that had to change, and the reason is worth recording because the first
version of this section was wrong in kind rather than in degree.

The port list this package was first given carried `is_jalr` and `branch_funct` but
no `is_branch` and no `is_jal`. There was then no way to tell an unconditional JAL
from a branch that did not fire, so the first implementation inferred it:
`is_taken = is_jalr | ~names_condition | branch_taken`, where "names no condition"
meant `branch_funct` was `3'b010` or `3'b011`, the two funct3 values no RV64I branch
uses (BGEU is `3'b111`). That was inventing semantics for two reserved encodings.
Today it happens to be harmless; the failure mode is that the day an instruction
whose funct3 collides with that pattern is routed through this unit, `is_taken`
disagrees with real control flow — and disagrees only for instructions nobody was
thinking about. The port list was under-specified, and the honest description is that
it was specified badly, not that the first implementation was implemented badly.

The fix is the interface itself: `decode_ctl_t` already carries `is_branch`, `is_jal`
and `is_jalr` as separate fields, so the decoder states what the instruction is and
this unit does arithmetic on the statement. Two properties are worth having:

* the unit is total — every combination of the three flags and `branch_taken` has a
  defined answer, and the sweep exercises all sixteen combinations including the ones
  no decoder emits;
* a mis-wired caller fails loudly. `is_branch & branch_taken` means an instruction
  that named no branch cannot be redirected by a stray comparator answer, so a
  comparator left driving the wrong instruction shows up as a wrong branch decision
  rather than as a silent redirect of a JAL.

### Behaviour, in prose

`link` is `pc + 4`, **always** — for JAL, for JALR, for a branch, for an instruction
whose pc is odd, and for a pc at the top of the address space where it wraps to zero.
In IALIGN=32 a jump target that is not 4-byte aligned traps before fetch, so there is
no "aligned link" rule to implement and none is invented.

`target` is the address control transfers to when `is_taken`, and `pc + 4` when it is
not. A not-taken instruction therefore still produces a correct sequential next PC on
the same port, so nothing else in the core needs a second "fall-through" path.

`JALR` clears **bit 0 and nothing else** of `pc + imm`. Bit 1 survives. That is the
whole point of the rule: a target that is 2 mod 4 has to stay 2 mod 4 so that the core
can raise `EXC_INSN_MISALIGNED` with the original PC and instruction available for
`tval`. A target unit that quietly rounded bit 1 away would hide a trap from the
checker, and the misaligned-target *report* is deliberately not this unit's job. When
`is_jal` and `is_jalr` are both high, `is_jalr` wins for the bit-0 clear, because it
is the stricter of the two rules.

The immediate arrives already sign-extended and is never re-sign-extended here, so
there is no second place where a W-form immediate could be widened.

### Comparison table

| funct3 | Condition | Reading | Answer |
| --- | --- | --- | --- |
| `3'b000` | BEQ | — | `rs1 == rs2` |
| `3'b001` | BNE | — | `rs1 != rs2` |
| `3'b100` | BLT | **signed** | `int64(rs1) < int64(rs2)` |
| `3'b101` | BGE | **signed** | `int64(rs1) >= int64(rs2)` |
| `3'b110` | BLTU | **unsigned** | `uint64(rs1) < uint64(rs2)` |
| `3'b111` | BGEU | **unsigned** | `uint64(rs1) >= uint64(rs2)` |
| `3'b010`, `3'b011` | none | — | 0; the decoder reports these illegal |

**x0.** The comparator's inputs are register *values*, not register numbers: x0 is
the register file's responsibility and it presents 0 here, so a comparison against
x0 reads as a comparison against zero. This unit cannot distinguish x0 from any
other register that happens to hold 0 and does not try. The testbench carries x0
cases because they are where a signed/unsigned mix-up is most visible:
`blt(0, -1)` is false while `bltu(0, -1)` is true.

### Branch stimulus

**PCs (11)** — `0`, `2`, `4`, `8`, `0x1000`, `0xfffffffe`…, `0x100000000`,
`0x7ffffffffffffffc`, `0xffffffff00000000`, `0xfffffffffffffff8`,
`0xfffffffffffffffc`. The last one makes `pc + 4` wrap to zero; the odd value makes
an odd pc explicit.

**Immediates (16)** — `0, 4, 8, -4, -8, 1, 2, 6, -1, 0x7f8, -0x800, 0x800,
0x7ffffffc, 0xffffffff80000004, 3, 5`. The set deliberately contains offsets whose
low bits are 1 and 2, so the difference between "JALR clears bit 0" and "JALR clears
bits 1 and 0" is visible, and offsets that make `pc + imm` misaligned in bit 0, in
bit 1, and in both.

| Phase | Vectors | What it is |
| --- | --- | --- |
| branch-target sweep | 2 816 | 11 pcs × 16 imms × all 16 combinations of `is_branch` × `is_jal` × `is_jalr` × `branch_taken` |
| comparator sweep | 3 200 | all 8 funct3 × all 400 pairs of the 20-value boundary operand set |
| closed chain | 3 520 | the DUT comparator's answer driving the DUT target unit: 8 funct3 × 20 operand values × 11 pcs × both `is_jalr` |
| branch random | 40 000 | `--seed`ed; a realistic instruction kind 95 % of the time, 70 % aligned pcs and small immediates, 30 % sign-crossing or x0 comparator operands |
| **total applied** | **49 536** | seed 1 |

The sweep includes flag combinations no decoder emits — all three flags low, and
`is_jal` together with `is_jalr` — on purpose: those are the vectors that prove the
unit fails loudly rather than silently on a mis-wired input.

### Branch reference models

`ReferenceBranchCmp(funct, rs1, rs2)` switches on the funct3 and compares in
`int64_t` for BLT/BGE and in `uint64_t` for BLTU/BGEU, returning false for the two
funct3 values that name no condition.

`ReferenceBranchTarget(pc, imm, is_branch, is_jal, is_jalr, branch_taken)` computes
`link = pc + 4`, `sum = pc + imm`, `aligned = is_jalr ? (sum & ~1) : sum`,
`is_taken = is_jal || is_jalr || (is_branch && branch_taken)`, and
`target = is_taken ? aligned : link` — the same rules as the RTL, written
independently in host integers.

The two units are checked separately: the target-unit sweep drives `branch_taken`
from the stimulus rather than from the DUT, so a wrong condition cannot hide behind
a wrong address or the other way round. The closed chain is checked on top of that,
with the DUT comparator's answer driving the DUT target unit and the pair compared
against the model chain — which is the configuration the core will actually wire.

### Branch invariants asserted

34 hand-written branch invariants, checked against the RTL without going through the
models:

```
JAL at pc=0 links to pc+4 and targets pc+imm          link wraps to zero at the top of the address space
a JALR at the top of the address space targets zero    an odd pc links to pc+4, unaligned
JALR clears bit 0 of the target                        JALR keeps bit 1: a target 2 bytes past pc
JALR keeps bit 1 with a 6-byte offset                  JALR clears bit 0 of pc+imm, not of imm
JALR keeps bit 1 of a large misaligned target          is_jalr wins over is_jal, and still clears bit 0
JAL leaves a 2-byte-aligned target for the trap logic
a taken forward branch yields pc+imm                   an untaken forward branch yields pc+4
an untaken backward branch yields pc+4                 an untaken branch with bit 1 set in imm yields pc+4
branch_taken high with no branch named transfers nothing
an ordinary ALU instruction falls through to pc+4      an unconditional jump ignores branch_taken
beq(7,7) is true                                       beq(7,8) is false
bne(7,7) is false                                      blt(-1,1) is true, signed
blt(1,-1) is false                                     bge(-1,1) is false
bge(1,-1) is true                                      bltu(-1,1) is false, -1 is the largest unsigned
bgeu(-1,1) is true                                     blt(x0,-1) is false: x0 reads as zero
bltu(x0,-1) is true: x0 reads as zero                  beq(x0,x0) is true
blt(INT64_MIN,INT64_MAX) is true                       bltu(INT64_MIN,INT64_MAX) is false
funct3 010 compares as no condition                    funct3 011 compares as no condition
```

Plus per-vector checks of `link`, `target`, `is_taken` and the comparator's `taken`,
plus these coverage assertions:

* JAL, JALR and a conditional branch were each exercised;
* a taken transfer and a not-taken instruction were both seen;
* **every** not-taken instruction produced `target == link`;
* `branch_taken` was seen high with no branch named — the mis-wired-caller case;
* a JALR cleared a bit 0 that was set;
* a JALR left a misaligned bit 1 in the target — i.e. "clears bit 0 only" was seen
  happening, not merely asserted;
* all eight funct3 values were compared, both outcomes of every condition were seen,
  and a comparison against a zero (x0) operand was made;
* the comparator-to-target chain was exercised.

---

# Negative controls

Nine deliberate defects, each behind a `-D` that is off in the shipping build, and
each demonstrated below to fail the case with exit 1. The wrapper and the C++ driver
are identical in all ten builds; only the `-D` changes.

## ALU mutants

| Mutant | Injected defect | First mismatch reported | Exit |
| --- | --- | --- | --- |
| `MOSAIC_ALU_MUTANT_1` | `extend_word` zero-extends the W answer | `addw(0x7fffffff, 1)`: expected `0xffffffff80000000`, got `0x0000000080000000` | 1 |
| `MOSAIC_ALU_MUTANT_2` | `sra` implemented as a logical shift | `sra(0x8000000000000000, 1)`: expected `0xc000000000000000`, got `0x4000000000000000` | 1 |
| `MOSAIC_ALU_MUTANT_3` | `sllw` shifts by six bits, not five | `sllw(-1, 32)`: expected `0xffffffffffffffff`, got `0x0000000000000000` | 1 |
| `MOSAIC_ALU_MUTANT_4` | `slt` uses the unsigned comparison | `slt(-1, 1)`: expected `0x0000000000000001`, got `0x0000000000000000` | 1 |
| `MOSAIC_ALU_MUTANT_5` | `zero` computed from `a` instead of `result` | `add(1, -1)`: expected `zero=1`, got `zero=0` | 1 |

## Branch mutants

| Mutant | Injected defect | First mismatch reported | Exit |
| --- | --- | --- | --- |
| `MOSAIC_BRANCH_TARGET_MUTANT_1` | JALR forgets to clear bit 0 | `jalr(pc=0, imm=1)`: expected target `0x0000000000000000`, got `0x0000000000000001` | 1 |
| `MOSAIC_BRANCH_TARGET_MUTANT_2` | `link` is `pc + 2` | `JAL at pc=0`: expected link `0x0000000000000004`, got `0x0000000000000002` | 1 |
| `MOSAIC_BRANCH_TARGET_MUTANT_3` | a not-taken branch leaks the branch target | `untaken branch pc=0x1000 imm=0x7f8`: expected target `0x0000000000001004`, got `0x00000000000017f8` | 1 |
| `MOSAIC_BRANCH_CMP_MUTANT_1` | BLTU/BGEU compare with the signed relation | `bltu(-1, 1)`: expected `0`, got `1` | 1 |

The run stops at eight failed checks so a mutant log stays readable; the coverage
assertions are skipped in that case, because after a deliberate halt they would be
reporting the truncation rather than anything about the stimulus.

## Things that went wrong on the way, kept on the record

**Mutant 4 passed the first time.** The first time the mutant table was produced,
**mutant 4 passed** — because the `ifdef` block for it had never been written, so
`-DMOSAIC_ALU_MUTANT_4` compiled to nothing and the run was bit-identical to the
shipping build. A define that selects nothing builds cleanly and a case that only
exercises the good path reports a pass, which is exactly the failure mode that makes
mutation evidence worthless. The block now exists, the defect is detected, and this
is the same reason `tools/lint_rtl.py --self-test` requires a deliberately latching
module to actually produce a LATCH warning: a gate that has never been seen to fail is
not a gate.

**Two of the hand-written ALU constants were wrong when first written.** A
6-bit-versus-64-bit reading of one `sll` case (the expectation for `sll(x, 0x80000000)`
was written as if the shift amount were not masked to six bits) and the wrong sign
extension on one `srlw` case. Both were caught by the RTL on the first run and are the
reason the boundary set is worth its size: a reference model that is wrong in the same
direction as the DUT is worse than no model, and these two were wrong in exactly the
direction the boundary values are chosen to expose.

**Two more branch constants were wrong the same way on the first run of Part II** — a
target written as `link` where the answer was `pc + imm`, and `blt(-1,1)` written as
false where it is true — again caught immediately by the RTL.

**The first version of the branch interface was specified badly**, as set out in
*What decides control transfer* above. That one was not caught by a failing test,
because the heuristic it encoded happened to agree with the real answer everywhere
the stimulus reached; it was caught by review. It is in this report because the class
of defect is the one this configuration-and-contract layer exists to prevent.

---

# Evidence

### Lint — every RTL file this package owns, plus the wrapper

The project linter is `tools/lint_rtl.py`, which elaborates each source on its own
with the packages it imports:

```
$ python3 tools/lint_rtl.py | grep -E 'mosaic_alu|mosaic_branch'
ok   rtl/core/mosaic_alu.sv: clean as mosaic_alu
ok   rtl/core/mosaic_branch_cmp.sv: clean as mosaic_branch_cmp
ok   rtl/core/mosaic_branch_target.sv: clean as mosaic_branch_target
```

and, with the plain `-Wall` the Makefile uses and no warning suppressions at all:

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_alu_tb \
    -Irtl/core -Ibuild/p0/sim rtl/core/mosaic_alu.sv rtl/core/mosaic_branch_target.sv \
    rtl/core/mosaic_branch_cmp.sv sim/tb/mosaic_alu_tb.sv; echo $?
- V e r i l a t i o n   R e p o r t: Verilator 5.052 2026-09-05 rev vUNKNOWN-built20260905
- Verilator: Built from 0.109 MB sources in 6 modules, into 0.040 MB in 3 C++ files needing 0.000 MB
0
```

Zero warnings and zero errors from the five files this package owns.

**History worth keeping.** An earlier version of this section was written before
`mosaic_pkg.sv` carried its scoped `/* verilator lint_off UNUSEDPARAM */` region, and
plain `-Wall` then produced **32 `UNUSEDPARAM` warnings, every one of them attributed
to `rtl/core/mosaic_pkg.sv`** — the opcode and exception constants an ALU has no
business referencing. A `lint_off` region in the *using* module cannot suppress them,
because Verilator attributes the warning to the definition, not to the use site. Two
import forms were also measured on the way to the present one: a `$unit`-scope
`import mosaic_pkg::*` raises `IMPORTSTAR`, and Verilator 5.052 rejects the narrower
`import mosaic_pkg::alu_op_e::*` as a syntax error, so `mosaic_alu` keeps a plain
`import mosaic_pkg::*;` **inside the module body**, which is sufficient because its
port list uses only plain vectors.

All three import forms were re-measured directly against Verilator 5.052 with the
package listed *first* on the command line, so none of those claims rests on the
linter's file ordering. No file in this package was changed in response to an
earlier, incorrect scoping diagnosis of this area: the import placement above is the
one the code started with and the one that lints clean.

$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module <each probe> \
    -Irtl/core rtl/core/mosaic_pkg.sv <probe>.sv
in-body import                 -> 0 warnings, 0 errors
file-scope import mosaic_pkg::* -> %Warning-IMPORTSTAR: 'import::*' in $unit scope
import mosaic_pkg::alu_op_e::*  -> %Error: syntax error, unexpected IDENTIFIER-::

### The case, seed 1

```
$ python3 tools/run_unit.py --case alu.boundaries; echo $?
PASS alu.boundaries               task=I-011
0

$ cat results/unit/alu.boundaries/run.log
$ /Users/flare/MosaicRV/build/p0/unit/alu.boundaries/alu.boundaries --case alu.boundaries --out /Users/flare/MosaicRV/results/unit/alu.boundaries --seed 1 --max-cycles 200000
RESULT PASS alu.boundaries 108560 ALU and 49536 branch stimulus vectors (59 hand-written invariants, seed 1); alu_ops=add=4895 sub=4738 sll=9855 slt=4767 sltu=4744 xor=4871 srl=10105 sra=10252 or=4732 and=4894 addw=4813 subw=4753 sllw=10288 srlw=10144 sraw=9986 passb=4723; branch_target=22816 compare=23200 chain=3520

$ python3 -c "import json;d=json.load(open('results/unit/alu.boundaries/result.json'));print('checks',d['checks'],'failures',d['failures'],'seed',d['seed'])"
checks 316022 failures 0 seed 1
```

316 022 checks: two or three per stimulus vector, two or three per hand-written
invariant, one model self-consistency check, and 90 coverage assertions. Every ALU op
is exercised between 4 700 and 10 300 times.

### Seed independence

The random campaigns are reproducible from `--seed`; the case does not pass on seed 1
alone.

```
$ for s in 2 7 4294967296; do build/p0/unit/alu.boundaries/alu.boundaries \
    --case alu.boundaries --out /tmp/as_$s --seed $s --max-cycles 200000; echo "exit=$?"; done
RESULT PASS alu.boundaries 108560 ALU and 49536 branch stimulus vectors (59 hand-written invariants, seed 2); alu_ops=add=4672 sub=4685 sll=10161 slt=4879 sltu=4832 xor=4693 srl=10238 sra=10329 or=4824 and=4664 addw=4717 subw=4756 sllw=10136 srlw=10144 sraw=10118 passb=4712; branch_target=22816 compare=23200 chain=3520
exit=0
RESULT PASS alu.boundaries 108560 ALU and 49536 branch stimulus vectors (59 hand-written invariants, seed 7); alu_ops=add=4770 sub=4787 sll=10255 slt=4825 sltu=4645 xor=4800 srl=10030 sra=10225 or=4876 and=4726 addw=4796 subw=4684 sllw=10059 srlw=10165 sraw=10132 passb=4785; branch_target=22816 compare=23200 chain=3520
exit=0
RESULT PASS alu.boundaries 108560 ALU and 49536 branch stimulus vectors (59 hand-written invariants, seed 4294967296); alu_ops=add=4710 sub=4742 sll=10130 slt=4830 sltu=4881 xor=4783 srl=10064 sra=9976 or=4789 and=4834 addw=4946 subw=4822 sllw=10249 srlw=10086 sraw=10015 passb=4703; branch_target=22816 compare=23200 chain=3520
exit=0
```

### Third cross-check of the hand-written ALU constants

The 25 hand-written ALU expected values were recomputed by an independent
arbitrary-precision Python model — a third implementation, in a third language, with
none of C++'s integer-conversion rules — and compared against the literals in
`sim/unit/tb_alu.cpp`:

```
$ python3 /tmp/xcheck_alu.py      # throwaway script, not committed
checked 25 hand-written invariants against the independent python model, 0 wrong
```

This is not decoration. Two of the 25 constants were wrong when first written — the
6-bit-versus-64-bit reading of one `sll` case and the wrong extension on one `srlw`
case — and the RTL caught both on the first run.

### C++ lint

```
$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -Isim/common -Ibuild/p0/sim \
    -Ibuild/p0/unit/alu.boundaries/obj_dir -I$(verilator -getenv VERILATOR_ROOT)/include \
    sim/unit/tb_alu.cpp 2>&1 | grep -E 'tb_alu\.cpp:[0-9]+:[0-9]+: (error|warning)'
(no output)
```

Clean under the flags `make lint-cpp` uses. The warnings still present in the full
output all come from Verilator's own runtime headers, which the Makefile already
documents as not being held to that standard.

### Mutation runs

Each mutant was built with the runner's own flags plus its `-D`, into its own build
directory:

```
R=$(pwd)
SRC="$R/sim/tb/mosaic_alu_tb.sv $R/rtl/core/mosaic_alu.sv \
     $R/rtl/core/mosaic_branch_target.sv $R/rtl/core/mosaic_branch_cmp.sv \
     $R/sim/unit/tb_alu.cpp $R/sim/common/sim_common.cpp"
verilator --cc --exe --build -j 0 -O2 -CFLAGS "-O2 -std=c++17 -Wall" \
  --x-assign unique --x-initial unique --top-module mosaic_alu_tb -Mdir $d/obj_dir \
  -I$R/build/p0/sim -I$R/rtl/core -I$R/rtl/common \
  -CFLAGS -I$R/sim/common -CFLAGS -I$R/build/p0/sim \
  -D<the mutant> -o $d/m $SRC
```

Output, all nine:

```
=== MOSAIC_ALU_MUTANT_1 build=0 ===
MISMATCH addw sign-extends its 32-bit answer: result: expected 0xffffffff80000000, got 0x0000000080000000
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 4005 ALU and 0 branch vectors
exit=1
=== MOSAIC_ALU_MUTANT_2 build=0 ===
MISMATCH sra(INT64_MIN,1) replicates the sign bit: result: expected 0xc000000000000000, got 0x4000000000000000
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 2889 ALU and 0 branch vectors
exit=1
=== MOSAIC_ALU_MUTANT_3 build=0 ===
MISMATCH sllw by 32 is a shift by 0, not a shift to zero: result: expected 0xffffffffffffffff, got 0x0000000000000000
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 4826 ALU and 0 branch vectors
exit=1
=== MOSAIC_ALU_MUTANT_4 build=0 ===
MISMATCH slt(-1,1) is 1, signed: result: expected 0x0000000000000001, got 0x0000000000000000
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 1206 ALU and 0 branch vectors
exit=1
=== MOSAIC_ALU_MUTANT_5 build=0 ===
MISMATCH add(1,-1) is zero while a != 0: zero flag: expected 1, got 0
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 4 ALU and 0 branch vectors
exit=1
=== MOSAIC_BRANCH_TARGET_MUTANT_1 build=0 ===
MISMATCH JALR clears bit 0 of the target: target: expected 0x0000000000000000, got 0x0000000000000001
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 92 branch vectors
exit=1
=== MOSAIC_BRANCH_TARGET_MUTANT_2 build=0 ===
MISMATCH JAL at pc=0 links to pc+4 and targets pc+imm: link: expected 0x0000000000000004, got 0x0000000000000002
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 23 failed checks over 0 ALU and 0 branch vectors
exit=1
=== MOSAIC_BRANCH_TARGET_MUTANT_3 build=0 ===
MISMATCH an untaken forward branch yields pc+4: target: expected 0x0000000000001004, got 0x00000000000017f8
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 33 branch vectors
exit=1
=== MOSAIC_BRANCH_CMP_MUTANT_1 build=0 ===
MISMATCH bltu(-1,1) is false, -1 is the largest unsigned: expected 0, got 1
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 5226 branch vectors
exit=1
```

`MOSAIC_ALU_MUTANT_5` reports after four vectors because the hand-written invariants
run before the matrix; it fails on the `zero` flag alone, with every `result` value
correct, which is what makes it a control for the flag rather than for the datapath.
`MOSAIC_BRANCH_TARGET_MUTANT_2` fails every link on its very first vector, so the run
stops before any stimulus is counted.

---

# Scope, and what this section does not claim

* **Misaligned-target reporting is not here, by design.** `target` preserves bit 1 so
  the core can raise `EXC_INSN_MISALIGNED` (or `EXC_INSN_ACCESS` under a stricter PMA)
  at the fetch boundary with the original PC and instruction to build `tval`.
* **x0 handling is not here, by design.** `mosaic_branch_cmp` takes values; presenting
  0 for `x0` is the register file's job, and the case exercises the consequences.
* **Immediate construction is not here.** The branch and jump immediate is decoded,
  sign-extended and delivered as a `XLEN` value; these units never re-derive it.
* **The decoder does not exist yet for these units to be wired into.** The interface
  assumes the `decode_ctl_t` fields `is_branch`, `is_jal`, `is_jalr` and `imm`; if I-010
  lands them under different names, this port list is where the mismatch will show.
* `XLEN` is a parameter on all three modules, but the word forms and the 6-bit/5-bit
  shift masks are written for RV64. Only `XLEN = 64` is instantiated in this profile.
* The `alu_op_e` encodings are transcribed by hand into `sim/unit/tb_alu.cpp`, since
  C++ cannot import a SystemVerilog package. I-010's `decode.rv64im_reserved` checks
  the same numbering from the RTL side; the two transcriptions must agree. The branch
  funct3 encodings here and in `mosaic_branch_cmp` are the ISA literals (BEQ=0,
  BNE=1, BLT=4, BGE=5, BLTU=6, BGEU=7; 2 and 3 name no condition), not a new set of
  constants — and they are now confined to the comparator, which is the only unit that
  needs to know them.
* `rtl/core/mosaic_pkg.sv` was not modified.

---

# Part III — I-010, RV64I/M decode

Work package **I-010**, run as `CASE=decode.rv64im_reserved` from
`tests/unit/registry.json`. The card is `### I-010 — 实现 RV64I/M decode 与非法指令`
in `docs/stage-1-scalar-control.md`.

This part covers the decoder and its case only. The ALU, the branch comparator and
the jump-target unit are Parts I and II above.

---

## What was built

| File | Contents |
| --- | --- |
| `rtl/core/mosaic_decoder.sv` | `mosaic_decoder`, RV64I + M, purely combinational, 652 lines |
| `sim/tb/mosaic_decoder_tb.sv` | Simulation wrapper: the struct broken out into 37 named ports plus the raw 133-bit vector, 129 lines |
| `sim/unit/tb_decoder.cpp` | Independent reference decoder, encoders, directed set, reserved enumeration, structural sweeps, random campaign, 1318 lines |

### Interface

```systemverilog
module mosaic_decoder (
    input  wire  [31:0]             insn,
    output mosaic_pkg::decode_ctl_t ctl
);
```

No clock, no reset, no state. Every field of `ctl` is a function of `insn` alone,
so the decoder can sit anywhere in the front end and its output can be sampled
whenever the instruction word is stable. `rtl/core/mosaic_pkg.sv` is the only other
file needed; the port is written `mosaic_pkg::decode_ctl_t` so its origin is
unambiguous to a reader who has not opened the package.

### Field contract

These are the decisions a consumer of `decode_ctl_t` needs and cannot infer from
the field names. Each one is enforced by a named check in the case.

| Field | Contract |
| --- | --- |
| `valid`, `illegal` | `illegal` implies `!valid`, always. No partial decode: an instruction is fully legal or `ctl` is exactly `CTL_ILLEGAL` with nothing stale left over |
| `reg_write` | already qualified by `rd != 0`. A write to x0 is not a register write, so nothing downstream special-cases x0 again (I-013 relies on this) |
| `rd`, `rs1`, `rs2` | the raw instruction fields **wherever those bits are register fields**; `x0` where the bits are an immediate fragment — `rs1`/`rs2` for U-type (`lui`/`auipc`), `rd` for S-type and B-type |
| `imm` | sign-extended to 64 bits, and the *only* immediate the instruction has. `uses_imm` means "imm participates": the address add for loads and stores, the shift amount for the six shift-immediates (zero-extended, never sign-extended), the control-transfer offset |
| `alu_op`, `uses_alu` | driven only when `uses_alu` is 1. Control transfers leave them at the `CTL_ILLEGAL` values `ALU_PASSB` / 0 and are driven by `branch_funct` / `writes_link` instead: the link value is `PC + 4`, produced by the branch unit |
| `is_auipc` | ALU operand A is the instruction's PC |
| `uses_rs1 == 0` | ALU operand A is hard-wired zero (`lui`) |
| `md_signed` | 1 for `mul`, `mulh`, `mulhsu`, `div`, `rem`; 0 for `mulhu`, `divu`, `remu`. `mulhsu`'s mixed signedness is carried by `MD_MULHSU` itself, not by this bit |
| `csr_reads`, `csr_writes` | the architectural intent *after* the x0 rules: `csrrw` with `rd == x0` does not read, `csrrs`/`csrrc` with `rs1 == x0` do not write |
| `uses_rs1` (CSR) | 1 for every CSR form — in the immediate forms `insn[19:15]` is a zero-extended 5-bit zimm, not a register index, and `csr_imm_form` says which |
| `mem_kind`, `mem_size`, `mem_signed` | valid whenever `mem_kind != MEM_NONE` |
| JALR target | **not** computed here. `is_jalr` + `uses_imm` + `imm` is what the core needs: `target = pc + rs1 + imm`, after which **the core clears target bit 0**. Clearing bit 0 is a target-computation rule owned by I-011, not an encoding rule, so it is deliberately not folded into `imm` |

The last row is worth stating plainly because it is a boundary decision: the
decoder is responsible for the *encoding* of the offset and the target unit
(I-011, Part II) is responsible for the *arithmetic* of the target, including the
alignment clear. The case asserts the boundary from both sides: `tb_decoder.cpp`
checks that a `jalr` with immediate 3 delivers `imm == 3`, unmodified, and
`tb_alu.cpp` checks that the target unit clears bit 0 of the sum.

## Instruction table implemented

Twelve opcodes decode. `funct3` values not in the table are reserved and set
`illegal`; so is any `funct7` outside those listed.

| Opcode | `funct3` | Instructions | Other constraints |
| --- | --- | --- | --- |
| `0000011` LOAD | 000 001 010 011 100 101 | `lb lh lw ld lbu lhu` | 110, 111 reserved |
| `0001111` MISC-MEM | 000 / 001 | `fence` / `fence.i` | 010–111 reserved |
| `0010011` OP-IMM | 000 010 011 100 110 111 | `addi slti sltiu xori ori andi` | — |
| | 001 | `slli` | `insn[31:26] == 000000` |
| | 101 | `srli` / `srai` | `insn[31:26] == 000000` / `010000` |
| `0010111` AUIPC | any | `auipc` | `funct3` is not part of the encoding |
| `0100011` STORE | 000 001 010 011 | `sb sh sw sd` | 100–111 reserved |
| `0011011` OP-IMM-32 | 000 / 001 / 101 | `addiw` / `slliw` / `srliw`,`sraiw` | `slliw` needs `funct7 == 0`; the shifts need `funct7` 0 or `0100000` |
| `0110011` OP+M | `funct7 == 0000001` | `mul mulh mulhsu mulhu div divu rem remu` | `funct3` is the operation |
| | `funct7 == 0000000` / `0100000` | `add sub sll slt sltu xor srl sra or and` | `sub` and `sra` need `0100000` |
| `1100011` BRANCH | 000 001 100 101 110 111 | `beq bne blt bge bltu bgeu` | 010, 011 reserved |
| `1100111` JALR | 000 | `jalr` | every other `funct3` reserved |
| `1101111` JAL | any | `jal` | `funct3` is not part of the encoding |
| `1110011` SYSTEM | 000 | `ecall` (imm12 `000`), `ebreak` (`001`), `mret` (`302`) | `rd == rs1 == x0` required; every other imm12 reserved |
| | 001 010 011 | `csrrw csrrs csrrc` | — |
| | 101 110 111 | `csrrwi csrrsi csrrci` | `funct3` 100 reserved |

Two entries in that table are worth calling out because getting them wrong is
invisible until something breaks:

**`lbu` and `lhu` are decoded.** They are RV64I, not reserved. A decoder that
treats `funct3` 100/101 on LOAD as reserved is silently dropping two legal
instructions; the case asserts both by name.

**The shift-immediate selector is six bits, not seven.** On RV64 the 64-bit
shift-immediates carry a **6-bit** `shamt` in `insn[25:20]`, so they are selected
by `insn[31:26]`, not by the 7-bit `funct7` field. The `*W` shift-immediates
carry a 5-bit `shamt` in `insn[24:20]` and really are selected by `insn[31:25]`.
Reading `funct7` for the 64-bit forms rejects every `shamt` whose `insn[31:26]` is
not zero — that is, `shamt` 32 through 63, all of which are legal. The case
sweeps all 64 values of `insn[31:26]` against all 8 `funct3`, and has a named
check for `slli` by 63.

### Immediate formats

Taken from the volume I instruction-format diagrams, then cross-checked against
the assembler (see *Third check: the assembler*):

| Format | Immediate | Sign-extended to |
| --- | --- | --- |
| I-type | `imm[11:0] = insn[31:20]` — **one field**; `insn[11:7]` is `rd` | 12 → 64 |
| S-type | `imm[11:5] = insn[31:25]`, `imm[4:0] = insn[11:7]` | 12 → 64 |
| B-type | `imm[12] = insn[31]`, `imm[11] = insn[7]`, `imm[10:5] = insn[30:25]`, `imm[4:1] = insn[11:8]`, `imm[0] = 0` | 13 → 64 |
| U-type | `imm[31:12] = insn[31:12]`, `imm[11:0] = 0` | 32 → 64 |
| J-type | `imm[20] = insn[31]`, `imm[10:1] = insn[30:21]`, `imm[11] = insn[20]`, `imm[19:12] = insn[19:12]`, `imm[0] = 0` | 21 → 64 |

The I-type line is the one that is easy to get wrong, because the manual's
instruction-format *diagram* draws the immediate as `inst[31:25] inst[11:7]` for
both I and S — the same two bit ranges under two names. In an I-type instruction
`insn[11:7]` is `rd`. Using the S layout for a load or a `jalr` corrupts every
address and every jalr offset whose low five bits are non-zero. The reference
decoder and the RTL each assembled it their own way, and the case compares them
field by field, so this could not have survived.

## Reserved encodings, and why each is reserved

The named enumeration the card asks for. Every row below is asserted by name in
`Reserved()`, and each assertion checks *both* that `illegal` is set and that
every other field is at the defined illegal state — a reserved encoding that
half-decodes is as much a bug as one that decodes wrongly.

| Reserved encoding | Count swept | Why it is reserved |
| --- | --- | --- |
| LOAD `funct3` 110, 111 | 2 | RV64I defines no load wider than `ld`; 32-bit load/store assume RV32 |
| MISC-MEM `funct3` 010–111 | 6 | only `fence` (000) and `fence.i` (001) exist in the base |
| OP-IMM `slli` with `insn[31:26] != 0` | 63 | RV64 reserves the whole `insn[31:26]` field: a nonzero value is not a shift amount, it is an unallocated extension slot |
| OP-IMM `funct3` 101, selector not `000000`/`010000` | 62 | the selector chooses `srli` vs `srai`; any other value names no shift |
| STORE `funct3` 100–111 | 4 | RV64I defines no store width beyond `sd` |
| OP `funct7` outside `{0000000, 0100000, 0000001}` × all 8 `funct3` | 750 | `funct7` selects the operation; `0000000` and `0100000` are fully allocated across the eight `funct3`, and `0000001` is the M extension |
| OP-IMM-32 `funct3` outside `{000, 001, 101}` | 5 | only `addiw`, `slliw` and `srliw`/`sraiw` exist |
| OP-IMM-32 shift `funct7` outside `{0000000, 0100000}` | 250 | same argument as the OP `funct7` |
| BRANCH `funct3` 010, 011 | 2 | the branch `funct3` space is `000 001 100 101 110 111`; 010/011 are the ALU's `slt`/`sltu` encodings, which name no condition. **`funct3` 100 is `blt`, not a reserved value** — the ALU mapping is not the branch mapping |
| JALR `funct3` 001–111 | 7 | `jalr` has exactly one `funct3` |
| RV32-only `addw subw sllw srlw sraw` (opcode `0111011`) | 5 named, plus all 1024 encodings of that opcode | these are RV32-only; on RV64 they are reserved and must raise an illegal instruction |
| SYSTEM `funct3` 100 | 1 | there is no fourth CSR form and no form with neither a register nor an immediate |
| SYSTEM `funct3` 000, imm12 ∉ {`000`,`001`,`302`} | 11 sampled | the other nine system instructions in that slot (`uret`, `sret`, `wfi`, `sfence.vma`, `hfence`, `sb`, …) are not part of this profile's M-mode set |
| SYSTEM `funct3` 000 with `rd != x0` or `rs1 != x0` | 6 | `rd` and `rs1` are not part of the `ecall`/`ebreak`/`mret` encoding; a nonzero value there is a reserved encoding, not "ecall with a don't-care destination" |
| Every opcode outside the twelve above | 116 × 8 | the custom-0/custom-1 spaces, the A extension, `OP-32`, the F/D/Q opcodes, the float `OP-32` space, the other privileged instructions, and everything unassigned. A 16-bit compressed instruction arrives with `insn[1:0] != 11` and lands on one of these, which is why I-041 decompresses *before* here rather than after |

**Total named reserved checks in the case: 1428.**

### The two RV32-only rules, stated separately

The card calls out the RV32 word forms explicitly, so, to be unambiguous: on
RV64, `addw`, `subw`, `sllw`, `srlw` and `sraw` — opcode `0111011` — are
**illegal**. All 1024 encodings of that opcode are swept, not just the five named
ones, and `reserved_opcodes_` is asserted to be exactly 116, which fails if a
future edit adds a thirteenth legal opcode without updating the enumeration.

## The testbench

`CASE=decode.rv64im_reserved` → `python3 tools/run_unit.py --case decode.rv64im_reserved`.

The DUT is combinational, so the wrapper has no clock and no reset and the driver
settles each vector with a single `eval()`. `--max-cycles` therefore bounds
instruction words presented, not clock cycles.

### Why the wrapper has no clock, and why it exposes 38 ports

The decoder has no state, so a clock would have nothing to advance; adding one
would only create unused-signal warnings and suggest a false sequentiality.
Instead the struct is broken out into **one named port per field**, so the driver
compares field by field and can name the field that mismatched.

That break-out is 37 assignments, and a typo in any of them would either hide a
decoder bug or invent one. So the wrapper also exports the struct unchanged as
`o_ctl_bits`, and the driver re-packs the 37 observed fields in the declaration
order of `decode_ctl_t` and compares the two. That check is not decoration: it
caught two real bugs during development — a bit-order error in the driver's
own packing (MSB-first vs LSB-first), and a five-word-vs-three-word
misunderstanding of how Verilator widens a 133-bit port. Both would have been
attributed to the decoder.

### The reference decoder, and why it is independent

`DecodeRef()` is written from the ISA manual's instruction-format diagrams, with
its own immediate assemblers (`ImmI`/`ImmS`/`ImmB`/`ImmU`/`ImmJ`), its own
encoders for building test vectors (`EncI`/`EncS`/`EncB`/`EncU`/`EncJ`/`EncR`/
`EncShiftX`/`EncShiftW`/`EncCsr`), and its own legality predicates. It shares no
expression and no control structure with the RTL. Where they disagree, the case
stops at the first difference and reports the instruction word, the field name
and both values.

Two design points in the reference are worth naming because they were the source
of its own bugs:

* `DecodeFields()` returns a `Decoded{Ref ctl; bool legal;}` pair, and `DecodeRef()`
  turns that into `valid`/`illegal`. Splitting it this way means **no arm inside
  the switch can forget to set `valid`** — which is exactly the mistake the first
  version made, and it made every instruction in the case look illegal.
* `Illegal()` restates `CTL_ILLEGAL` from the package rather than borrowing the
  RTL's constant, including `alu_op = ALU_PASSB` — the package's defined "no ALU
  operation" encoding, which is *not* zero.

### Third check: the assembler

Neither the RTL nor the reference was trusted on the immediate formats alone. A
third implementation — a Python script that decodes `objdump -M no-aliases`
output and re-renders the instruction text — was compared against
`riscv64-elf-gcc`/`objdump` over 92 assembled instructions covering every
immediate format, every shift boundary, every memory width and every CSR form:

```
$ cd build/p0/scratch
$ riscv64-elf-gcc -march=rv64im_zicsr_zifencei -mabi=lp64 -c isa_check.s -o isa_check.o
$ riscv64-elf-objdump -d -M no-aliases isa_check.o > isa_check.dis
$ python3 isa_crosscheck.py
cross-checked 92 instructions against objdump, 0 mismatches
```

Representative lines from that disassembly, the ones that pin the formats down:

```
  3c:	8000bf83          	ld	t6,-2048(ra)
  40:	fff1c103          	lbu	sp,-1(gp)
  44:	7ff2d203          	lhu	tp,2047(t0)
  48:	80038367          	jalr	t1,-2048(t2) # ffffffff7ffff800
  54:	00069613          	slli	a2,a3,0x0
  58:	03f79713          	slli	a4,a5,0x3f          <- shamt 63, insn[31:26] == 0
  68:	43fbdb13          	srai	s6,s7,0x3f
  6c:	000c9c1b          	slliw	s8,s9,0x0
  74:	01fede1b          	srliw	t3,t4,0x1f
  78:	405fdf1b          	sraiw	t5,t6,0x5
  9c:	7e208f63          	beq	ra,sp,89a
  a0:	804180e3          	beq	gp,tp,fffffffffffff8a0
  e4:	80000d6f          	jal	s10,fffffffffff000e4
  f8:	029423b3          	mulhsu	t2,s0,s1
 160:	34002573          	csrrs	a0,mscratch,zero
 164:	0ff0000f          	fence	iorw,iorw
```

The Python cross-checker earned its place three times before the RTL existed. It
found: the I-type immediate assembled with the S-type layout (`insn[31:20]` vs
`insn[31:25]||insn[11:7]`); the 64-bit shift-immediate selector read as a 7-bit
`funct7` instead of a 6-bit `insn[31:26]`; and `blt` transcribed as `funct3` 010
instead of 100. **All three were then found independently in the RTL by the case
itself**, which is the point of having three sources.

### Stimulus

| Phase | Instructions | What it is |
| --- | --- | --- |
| directed | ~180 | the card's named cases, each with its own assertions |
| reserved | 1428 named + 936 + 1024 | the enumeration above, each with a name |
| sweep (a) | 8192 | all 128 opcodes × all 8 `funct3` × 8 `funct7` patterns |
| sweep (b) | 3072 | all 128 `funct7` values × all 8 `funct3` on `0110011`, `0111011`, `0011011` |
| sweep (c) | 512 | all 64 `insn[31:26]` values × all 8 `funct3` on OP-IMM |
| sweep (d) | 11264 | every `rd` × `rs1` pair for 8 formats, every `rs1` × `rs2` pair for 6 |
| sweep (e) | ~37 000 | every immediate bit, field by field, exhaustively |
| random | 100 000 | `--seed`ed: half uniform 32-bit words, half biased onto the twelve legal opcodes |
| **total applied** | **166 701** | 84 469 legal, 82 232 illegal, seed 1 |

**Sweep (e) is the complete argument for immediates**, and it is worth spelling
out because it is cheaper than a 2^32 campaign by five orders of magnitude. Each
immediate is scrambled across the word, so sweeping *each field* across its full
range while the others are held at a few patterns reaches every value of every
bit of the immediate:

* I-type: all 4096 values of `imm[11:0]`, twice — once through `addi`, once
  through `ld`, so a decoder that only got it right for ALU forms is caught.
* S-type: all 128 values of `imm[11:5]` × 8 patterns, then all 32 values of
  `imm[4:0]` × 8 patterns. Split in two because that is exactly where the I/S
  confusion lives.
* B-type: **all 8192** immediate patterns, for `funct3` 000 and 110.
* J-type: all 1024 values of `imm[10:1]`, all 256 of `imm[19:12]`, and `imm[20]`
  separately — the three scrambled fields plus the sign, each swept on its own.
* U-type: all 4096 values of `imm[31:12]`, then all 256 values of `imm[31:24]`
  to reach the sign bit independently.

### Invariants asserted

Named checks against the RTL, written without reference to the model, so a shared
mistake in `DecodeRef` cannot make model and DUT agree for the wrong reason. The
card's required cases are marked ★.

**Immediate formats**

```
★ S-type offset -16                     S-type has no rd: insn[11:7] is immediate, not rd
sd offset -2048                          sw offset +2047 (needs imm[11:5] all ones)
★ beq offset -4096 (max back)           bgeu offset +4094 (max forward)
blt offset -2: branch funct3 100         ★ jal offset -1048576 (most negative legal)
jal offset +1048574 (max forward)        jal offset -2: imm[20] and the sign bit set together
lui imm, low 12 bits zero and imm[31] sign-extended
auipc sign-extends imm[31]               auipc with imm[31:12] all ones
lui passes the U-type immediate through the ALU
lui/auipc insn[19:15] and insn[24:20] are immediate, not registers
```

**Shifts**

```
★ slli by 0                              ★ slli by 63: insn[31:26] zero, 6-bit shamt
★ slli by 64 is reserved on RV64         ★ srai by 63: funct3 101 with insn[31:26] = 010000
srli by 31: funct3 101, top6 zero        srai by 0 is not the same as srli by 0 in op
slliw by 31: 5-bit shamt in insn[24:20]  sraiw by 31
srliw with funct7 0000001 is reserved
```

**ALU selection — every legal operation asserted by number**

```
add sub sll slt sltu xor srl sra or and addi slti sltiu xori ori andi addiw
```

so a renumbering of `mosaic_pkg::alu_op_e` fails the case instead of silently
agreeing with a renumbered decoder. The `alu_op_e` values are transcribed by hand
into `tb_decoder.cpp` (C++ cannot import a SystemVerilog package), and this is the
decoder-side check of that transcription; `tb_alu.cpp` holds the other copy. The
two must agree, and the RV32 `ALU_SUBW`/`ALU_SLLW`/`ALU_SRLW`/`ALU_SRAW`
encodings are checked from the ALU side only, because no RV64 instruction reaches
them through the decoder — which is the point.

**Memory**

```
lb lh lw ld lbu lhu sb sh sw sd: width, signedness, and rs1 + imm in the ALU
```

**M extension**

```
mul mulh mulhsu mulhu div divu rem remu: md_op, md_signed, and rd write
```

**CSR x0 rules — the two rules a decoder most often gets wrong, because both look
like they belong to the CSR unit**

```
csrrw always writes the CSR                csrrw with rd == x0 does not read the CSR
csrrw with rd != x0 reads and writes       csrrs with rs1 == x0 does not write the CSR
csrrs always reads the CSR                 csrrs with rs1 != x0 writes the CSR
csrrc with rs1 == x0 does not write        csrrwi with rd == x0 does not read the CSR
csrrwi always writes the CSR, whatever the zimm
csrrsi with zimm != 0 writes and reads     csr_op and csr_addr for all six forms
```

**Control transfer and x0 discipline**

```
★ jalr immediate 3 survives: the core clears the target
jalr links, uses rs1 + imm, does not use the ALU datapath
branch writes no register, does not use the ALU datapath
jal with rd == x0 performs no register write; with rd == x31 it writes the link
9 instructions with rd == x0 all report reg_write == 0
decode_ctl_t is 133 bits wide and every one is accounted for
the 37 named ports carry exactly the bits of the struct
```

**Coverage assertions**, so a stimulus edit that stops reaching something fails
the case instead of quietly passing over a shorter path:

* every one of the 128 opcodes was presented;
* every one of the 1024 (opcode, `funct3`) pairs was presented;
* the legal side was exercised at least 10 000 times and the illegal side at
  least 10 000 times;
* the 116 non-RV64IM opcodes were all enumerated and all decoded as illegal;
* 1428 named reserved checks ran;
* every presented instruction was legal or illegal, never both;
* the run stayed within `--max-cycles`.

## Negative controls

Seven deliberate defects, each behind a `-D` that is **off in the shipping build**,
each confined to an `` `ifdef `` block in `rtl/core/mosaic_decoder.sv`. The
wrapper and the C++ driver are identical in all eight builds; only the `-D`
changes. All seven are detected, each with exit 1.

| Mutant | Injected defect | First mismatch reported | Exit |
| --- | --- | --- | --- |
| `MOSAIC_DECODER_MUTANT_SRAI_FUNCT3` | `srai` accepted with `funct3` 000 | `0x40730293.imm`: expected `0x0000000000000407`, got `0x0000000000000007` | 1 |
| `MOSAIC_DECODER_MUTANT_S_IMM_AS_I` | S-type immediate built with the I-type layout | `0xfe530823.imm`: expected `0xfffffffffffffff0`, got `0xffffffffffffffe5` | 1 |
| `MOSAIC_DECODER_MUTANT_RV32_WORD_LEGAL` | RV32-only `addw`/`subw`/… decode as legal | `0x003100bb.valid`: expected `0x0000000000000000`, got `0x0000000000000001` | 1 |
| `MOSAIC_DECODER_MUTANT_RESERVED_F3_LEGAL` | reserved LOAD `funct3` 111 accepted as a load | `0x00017083.valid`: expected `0x0000000000000000`, got `0x0000000000000001` | 1 |
| `MOSAIC_DECODER_MUTANT_SHIFT_UPPER_IGNORED` | `slli` ignores `insn[31:26]`, so shift-by-64 is legal | `0x04031293.valid`: expected `0x0000000000000000`, got `0x0000000000000001` | 1 |
| `MOSAIC_DECODER_MUTANT_B_IMM_SWAPPED` | B immediate's `[10:5]` and `[4:1]` halves exchanged | `0x007302e3.imm`: expected `0x0000000000000804`, got `0x0000000000000900` | 1 |
| `MOSAIC_DECODER_MUTANT_CSR_INTENT` | CSR read/write intent ignores the x0 rules | `0x30001073.csr_reads`: expected `0x0000000000000000`, got `0x0000000000000001` | 1 |

The first two rows are the card's suggested mutants and the last four were added
because the first two are caught in the *first* instruction of the campaign, which
means they prove the comparison works but not that the later phases do. With all
seven, the earliest a defect is detected ranges from instruction 1 to instruction
9798, so the reserved enumeration, the `funct7` sweep, the register sweep and the
B-immediate sweep are each independently shown to have teeth.

`MOSAIC_DECODER_MUTANT_SRAI_FUNCT3` is worth a note: the first implementation of it
put the defect *inside* the `funct3 == 101` arm, where it was unreachable — the
condition it weakened (`funct3 == F3_SRL_SRA`) was already true. The `-D`
compiled, the run was bit-identical to the shipping build, and **the case passed**.
That is the same failure mode as `MOSAIC_ALU_MUTANT_4` in Part I, and it is why the
defect was moved out to the `funct3 == 000` arm where it changes the decode of a
real instruction. A negative control that is never exercised proves nothing.

## Evidence

### Lint

The project linter, which elaborates each source on its own with the packages it
imports:

```
$ python3 tools/lint_rtl.py | tail -3
ok   rtl/core/mosaic_bringup_core.sv: clean as mosaic_bringup_core
ok   rtl/core/mosaic_decoder.sv: clean as mosaic_decoder
lint: 9 source file(s) clean
```

and the decoder plus its wrapper under the plain `-Wall` the Makefile uses, with
no warning suppressions at all — in **both** file orders, since the package is
named in the command line:

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_decoder_tb \
    -Irtl/core rtl/core/mosaic_pkg.sv rtl/core/mosaic_decoder.sv \
    sim/tb/mosaic_decoder_tb.sv; echo $?
- V e r i l a t i o n   R e p o r t: Verilator 5.052 2026-09-05 rev vUNKNOWN-built20260905
- Verilator: Built from 0.113 MB sources in 4 modules, into 0.070 MB in 3 C++ files needing 0.000 MB
0
```

Zero warnings and zero errors from the three files this package owns.

**A scoping error that was not an error, and the import form that survives.** An
earlier iteration of this file put `import mosaic_pkg::*;` at `$unit` (file)
scope, above the module. `tools/lint_rtl.py` at that time appended the package
*after* the module on the Verilator command line, and Verilator — which processes
files in order — reported

```
%Error: rtl/core/mosaic_decoder.sv:220:14: Reference to 'decode_ctl_t' before
declaration (IEEE 1800-2023 6.18)
```

which reads exactly like a real scoping mistake in the module. It was a
file-ordering bug in the linter. The two import forms were then measured directly
against Verilator 5.052, with the package listed first, and the result is:

```
in-body import, qualified port            -> 0 warnings, 0 errors
file-scope (IMPORTSTAR)                   -> %Warning-IMPORTSTAR: 'import::*' in $unit scope
```

The file-scope form is therefore wrong on its own terms regardless of the linter
bug: `-Wall` promotes `IMPORTSTAR` to an error, and it is not even needed here,
because the port is written `mosaic_pkg::decode_ctl_t`. The shipped file uses an
in-body `import` and fully qualifies the two package *type* references
(`mosaic_pkg::decode_ctl_t` on the port and on `CTL_ILLEGAL`,
`mosaic_pkg::md_op_e` on the enum cast), which resolves regardless of file order.

The general lesson is worth recording next to the evidence: **a diagnostic that
names a specific standard-mandated construct and points at a plausible line is very
persuasive and can be completely fictional.** "Reference before declaration" is
exactly the shape an ordering bug imitates. It is the same class of defect as the
`mosaic_bringup_core.sv` and `mosaic_alu.sv` import questions, and it is the reason
no import form in this package was changed on the strength of a message about
scoping — all of them were re-measured against the tool instead.

### The case, seed 1

```
$ python3 tools/run_unit.py --case decode.rv64im_reserved; echo $?
PASS decode.rv64im_reserved       task=I-010
0

$ cat results/unit/decode.rv64im_reserved/run.log
$ /Users/flare/MosaicRV/build/p0/unit/decode.rv64im_reserved/decode.rv64im_reserved \
    --case decode.rv64im_reserved --out /Users/flare/MosaicRV/results/unit/decode.rv64im_reserved \
    --seed 1 --max-cycles 200000
RESULT PASS decode.rv64im_reserved 166701 instructions (84469 legal, 82232 illegal), 0 mismatches, 1428 named reserved checks

$ python3 -c "import json;d=json.load(open('results/unit/decode.rv64im_reserved/result.json'));print('checks',d['checks'],'failures',d['failures'],'seed',d['seed'])"
169428 0 1
```

169 428 checks: 37 field comparisons plus one break-out comparison per presented
instruction, the named directed and reserved checks, and 128 + 1024 + 7 coverage
assertions.

### Seed independence

```
$ for s in 2 7 4294967296; do build/p0/unit/decode.rv64im_reserved/decode.rv64im_reserved \
    --case decode.rv64im_reserved --out /tmp/ds_$s --seed $s --max-cycles 200000; echo "exit=$?"; done
RESULT PASS decode.rv64im_reserved 166701 instructions (84355 legal, 82346 illegal), 0 mismatches, 1428 named reserved checks
exit=0
RESULT PASS decode.rv64im_reserved 166701 instructions (84426 legal, 82275 illegal), 0 mismatches, 1428 named reserved checks
exit=0
RESULT PASS decode.rv64im_reserved 166701 instructions (84473 legal, 82228 illegal), 0 mismatches, 1428 named reserved checks
exit=0
```

### C++ lint

```
$ incs="-Ibuild/p0/sim -Isim/common -I$(verilator -getenv VERILATOR_ROOT)/include"
$ for d in build/p0/unit/*/obj_dir; do [ -d "$d" ] && incs="$incs -I$d"; done
$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow $incs sim/unit/tb_decoder.cpp 2>&1 \
    | grep -E 'tb_decoder\.cpp:[0-9]+:[0-9]+: (error|warning)'
(no output)
```

Zero from `sim/unit/tb_decoder.cpp`. The 64 warnings still in the full output all
come from Verilator's own runtime headers (55 in `verilated_funcs.h`, 9 in
`verilated_types.h`), which the Makefile already documents as not being held to
that standard.

### Mutation runs

Each mutant was built with the runner's own flags plus its `-D`, into its own
build directory. `tools/run_unit.py` does not expose a `-D` flag, so the Verilator
command below is the runner's, reproduced verbatim with the define and the paths
made absolute (Verilator's generated `make` runs in `-Mdir`, so relative
`-CFLAGS -I` paths do not resolve):

```sh
R=$(pwd)
verilator --cc --exe --build -j 0 -O2 -CFLAGS "-O2 -std=c++17 -Wall" \
  --x-assign unique --x-initial unique --top-module mosaic_decoder_tb \
  -Mdir $R/build/p0/unit/mutants/$M/obj_dir \
  -I$R/build/p0/sim -I$R/rtl/core -I$R/rtl/common \
  -CFLAGS -I$R/sim/common -CFLAGS -I$R/build/p0/sim \
  -D$M -o $R/build/p0/unit/mutants/$M/decode.rv64im_reserved \
  $R/sim/tb/mosaic_decoder_tb.sv $R/rtl/core/mosaic_decoder.sv \
  $R/sim/unit/tb_decoder.cpp $R/sim/common/sim_common.cpp
```

Output, all seven, with the exit status of each run:

```
=== -DMOSAIC_DECODER_MUTANT_SRAI_FUNCT3 (exit 1) ===
MISMATCH insn=0x40730293.imm: expected 0x0000000000000407, got 0x0000000000000007
CHECK FAILED: decoder disagrees with the reference at insn=0x40730293
RESULT FAIL decode.rv64im_reserved 4681 instructions compared, 2 failure(s)
=== -DMOSAIC_DECODER_MUTANT_S_IMM_AS_I (exit 1) ===
MISMATCH insn=0xfe530823.imm: expected 0xfffffffffffffff0, got 0xffffffffffffffe5
CHECK FAILED: decoder disagrees with the reference at insn=0xfe530823
RESULT FAIL decode.rv64im_reserved 1 instructions compared, 2 failure(s)
=== -DMOSAIC_DECODER_MUTANT_RV32_WORD_LEGAL (exit 1) ===
MISMATCH insn=0x003100bb.valid: expected 0x0000000000000000, got 0x0000000000000001
RESULT FAIL decode.rv64im_reserved 1487 instructions compared, 2 failure(s)
=== -DMOSAIC_DECODER_MUTANT_RESERVED_F3_LEGAL (exit 1) ===
MISMATCH insn=0x00017083.valid: expected 0x0000000000000000, got 0x0000000000000001
RESULT FAIL decode.rv64im_reserved 85 instructions compared, 2 failure(s)
=== -DMOSAIC_DECODER_MUTANT_SHIFT_UPPER_IGNORED (exit 1) ===
MISMATCH insn=0x04031293.valid: expected 0x0000000000000000, got 0x0000000000000001
RESULT FAIL decode.rv64im_reserved 14 instructions compared, 2 failure(s)
=== -DMOSAIC_DECODER_MUTANT_B_IMM_SWAPPED (exit 1) ===
MISMATCH insn=0x007302e3.imm: expected 0x0000000000000804, got 0x0000000000000900
RESULT FAIL decode.rv64im_reserved 9798 instructions compared, 2 failure(s)
=== -DMOSAIC_DECODER_MUTANT_CSR_INTENT (exit 1) ===
MISMATCH insn=0x30001073.csr_reads: expected 0x0000000000000000, got 0x0000000000000001
RESULT FAIL decode.rv64im_reserved 62 instructions compared, 2 failure(s)
```

## Things that went wrong on the way, kept on the record

**The reference decoder never set `valid`.** The first version returned a `Ref`
whose `valid`/`illegal` pair was whatever `Illegal()` had left in it, so every
instruction decoded "illegal" and the very first comparison failed. The run did
its job — it stopped at instruction 1 and named the field — but the cause was in
the *checker*, not the DUT, which is worth naming because it is the failure mode a
model-based test is supposed to protect against and here it protected the DUT from
the model instead. Fixed by splitting `DecodeFields()` (which cannot forget
`valid`, because it returns a separate `legal` flag) from `DecodeRef()` (which
sets the pair).

**Three transcription errors survived the hand-decoding and were caught by the
case**, all on the first full run and all in the same family — reading a format's
field as another format's field with the same bit positions:

* `blt` transcribed as `funct3` 010 (the ALU's `slt`) instead of 100, so the
  branch arm rejected `blt` and accepted a value that names no condition. The
  Python cross-checker had already found this one independently.
* the M-extension signedness predicate written as "unsigned means all three
  `funct3` bits set", which is 111 only and classifies `mulhu` (011) and `divu`
  (101) as signed. Written out as an explicit exclusion of `011`/`101`/`111` now.
* `fence` and `fence.i` decoded but with `legal` never set, so both reported
  illegal. Caught by the very next directed instruction.

**Three of my own named checks were wrong**, all caught immediately: a `lui`
expectation written as `0xABCDE000` where the sign-extended answer is
`0xFFFFFFFFABCDE000`; `csrrwi with zimm == 0` expected *not* to write, where
`csrrwi` always writes; and a "jal with rd == x0 performs no register write"
assertion attached to an instruction built with `rd = x31`. All three were
expectation errors, not decoder errors, and all three were found because the check
was written from the ISA manual rather than from the model's output.

**A `tb_decoder.cpp` self-check caught two bugs in `tb_decoder.cpp`.** The
break-out comparison first packed the 37 fields LSB-first, then assumed Verilator
widened the 133-bit port into three 64-bit words rather than five 32-bit ones. Both
produced a "MISMATCH … (testbench break-out vs decode_ctl_t)" that looked like a
decoder defect and was not. This is the argument for keeping the self-check: an
unchecked 37-line break-out is a place where a testbench bug masquerades as a DUT
bug.

**One mutant was inert on the first run**, as described under Negative controls.

---

# Scope, and what this section does not claim

* **CSR *execution* is not here.** The decoder classifies `csr_op`, `csr_addr`,
  `csr_writes`, `csr_reads` and `csr_imm_form`; I-019 implements the registers.
  Writing a read-only CSR is an illegal instruction, but that is an address-decode
  property and needs the project's CSR table, so it is left to the module that owns
  the table rather than hardcoded here.
* **`mret` is classified, not executed.** `is_mret` is driven; the trap unit (I-019)
  acts on it.
* **The M datapath is not here.** `is_muldiv`, `md_op` and `md_signed` are driven;
  I-012 owns the multiplier and divider. The case tests the classification without
  that unit, which is what makes it testable now.
* **Compressed instructions are not here.** I-041 owns them; a 16-bit instruction
  arrives with `insn[1:0] != 11` and lands on one of the 116 reserved opcodes.
* **Alignment and trap generation are not here.** `fence` and `fence.i` are
  classified; the memory system and the trap unit act on them.
* **`lbu`/`lhu` are decoded** even though the card's instruction list omits them.
  They are RV64I, not reserved, and a decoder that rejects them silently drops two
  legal instructions. Flagging the omission rather than following it.
* **`rtl/core/mosaic_pkg.sv` was not modified.**
* `tests/unit/registry.json` was not modified by this package.
---

# Correction (I-012 lane, appended — the body above is unchanged)

**What this corrects.** The body of this report lists OP-32 (`0111011`) as reserved
and calls `addw subw sllw srlw sraw` "the RV32-only word forms … reserved on
RV64". That is wrong. They are **RV64I** instructions and the shipping decoder
trapped all five as illegal, together with the per-encoding rationale in
`sim/unit/tb_decoder.cpp` and the comment on `OP_32` in `mosaic_pkg.sv`. A core
that traps five legal instructions breaks any compiler that emits them; the
corpus only passed because it never emitted one.

The correction was found while adding the M-extension word forms to OP-32 for
work package **I-012** and was authorised by the integration lead. Three
independent sources agree, and the first two are not the project's own code:

**1. The ISA.** RV32I defines no OP-32 at all, so the W forms cannot be RV32
forms; RV64I adds ADDW/SUBW/SLLW/SRLW/SRAW on OP-32 with funct7
`0000000`/`0100000`, exactly as it adds ADD/SLL/SRL/SUB/SRA on OP.

**2. binutils** (`riscv64-elf-as -march=rv64im` + `riscv64-elf-objdump -d -M
no-aliases`), the same third source this report already used:

```
   0:	003100bb          	addw	ra,sp,gp
   4:	403100bb          	subw	ra,sp,gp
   8:	003110bb          	sllw	ra,sp,gp
   c:	003150bb          	srlw	ra,sp,gp
  10:	403150bb          	sraw	ra,sp,gp
  14:	023100bb          	mulw	ra,sp,gp
  18:	023140bb          	divw	ra,sp,gp
  1c:	023150bb          	divuw	ra,sp,gp
  20:	023160bb          	remw	ra,sp,gp
  24:	023170bb          	remuw	ra,sp,gp
  28:	0051009b          	addiw	ra,sp,5

$ objdump -D -b binary -m riscv:rv64 /tmp/raw.bin          # 023110bb 023120bb 023130bb
   0:	023110bb          	.insn	4, 0x023110bb
   4:	023120bb          	.insn	4, 0x023120bb
   8:	023130bb          	.insn	4, 0x023130bb
```

Ten encodings on OP-32 are legal: the five I word forms with funct7 `0000000`
(addw `funct3` 000, sllw 001, srlw 101) and `0100000` (subw 000, sraw 101), and
the five M word forms with funct7 `0000001` (mulw 000, divw 100, divuw 101, remw
110, remuw 111). Both classes are R-type: rs1 and rs2 are register indices, the
shift amount is `rs2[4:0]`, and **there is no immediate** in either — the
shamt-immediate forms are `slliw`/`srliw`/`sraiw` on OP-IMM-32, which were
already decoded correctly and are untouched. The `023110bb`/`023120bb`/`023130bb`
attempts disassemble as `.insn`, which is the check that funct3 001/010/011 under
funct7 `0000001` really are reserved.

**3. This project's own reference path.** `rtl/core/mosaic_bringup_core.sv`
(OP-32 arm) and its independent C++ model `sim/unit/tb_bringup.cpp` (line ~712)
already decoded all five as legal `ALU_*W` operations. `mosaic_alu.sv` implements
them, and before this correction `ALU_SUBW` was produced by **no** decode path in
the shared decoder.

**What changed.**

| File | Change |
| --- | --- |
| `rtl/core/mosaic_decoder.sv` | OP-32 funct7 `0000000`/`0100000` now decode to `ALU_ADDW`/`ALU_SLLW`/`ALU_SRLW`/`ALU_SUBW`/`ALU_SRAW`, legal, `uses_alu` set, `uses_imm` clear; the `md_w` M word forms are unchanged; every genuinely reserved funct7/funct3 stays illegal, and the header prose was corrected |
| `rtl/core/mosaic_pkg.sv` | the `OP_32` comment corrected (it is no longer this report's "pkg was not modified") |
| `sim/unit/tb_decoder.cpp` | the reference model decodes both classes; five positive W-form rows and ten binutils-encoding rows were added; the reserved-class block now pins the funct3 patterns each I-word funct7 does *not* define and counts 5 + 5 legal encodings over all 1024; the "reserved on RV64" rationale was deleted |
| `MOSAIC_DECODER_MUTANT_RV32_WORD_LEGAL` | **deleted** — it mutated the shipping behaviour into what is now, correctly, the shipping behaviour, which made it vacuous |
| `MOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL` | **added** — rejects the five RV64I word forms again, i.e. restores exactly this defect so it cannot come back unnoticed |

**Verification after the change.**

```
python3 tools/run_unit.py --profile p0 --case decode.rv64im_reserved
  RESULT PASS decode.rv64im_reserved  166733 instructions (82384 legal, 84349 illegal),
  0 mismatches, 1443 named reserved checks            (checks 169534, failures 0, exit 0)

-DMOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL
  exit 1, 2 failures, delta +2 against the green base
  first mismatch: insn=0x003100bb.valid: expected 0x0000000000000001, got 0x0000000000000000

python3 tools/run_unit.py --profile p0 --case muldiv.kill_and_edges
  RESULT PASS muldiv.kill_and_edges   (unchanged)

python3 tools/run_unit.py --profile p0 --case core.bringup_vs_reference
  RESULT PASS core.bringup_vs_reference  corpus 39 programs, 2774346 architectural events,
  every stream identical to the independent reference
```

The bring-up case is the one that executes through a decoder and an ALU, and it
stays green: no second defect in the routing (`uses_alu`/`is_muldiv`/`uses_imm`
are all as the ALU expects) was found. The legal-instruction count in this report
rises accordingly (the opcode sweep now finds ten legal OP-32 encodings), and the
"116 reserved opcodes" figure elsewhere in the body was already superseded when
I-012 made OP-32 a decoded opcode at all (it is 115 in the current case).

## Addendum (I-017): `LWU`, and the wrapper width that had gone stale

This section is an addendum to the report above, not a rewrite of it. Work
package I-017 (`results/reports/I-017-event-payload.md`) fixed a defect V-013
found in this decoder and, because this case owns "which encodings are
reserved", extended it to cover the fix.

**`LWU` was refused.** The `OP_LOAD` arm decoded funct3 `0,1,2,3,4,5` and
treated everything else as illegal, with a comment claiming `110` and `111` are
both reserved. That is wrong for `110`: RV64I defines **LWU**, the
zero-extending word load, the project's own reference model implements it
(`sim/unit/trap_ref.h`, `case 0x6u: size = 4; sign = false`), and a program
containing `lwu` stopped the machine
(`unsupported=1 illegal=1 stopped=1` — V-013's observation). funct3 `110` now
decodes as `SZ_WORD` with `mem_signed = 0`; funct3 `111` remains the only
reserved load, and the comment says which is which.

**What this case now does about it.**

* the independent C++ reference decoder decodes funct3 `110` as legal;
* the named `LWU` check runs *first* in the directed phase, so a regression
  fails with the instruction named rather than with the first field the sweep
  happens to disagree about;
* the memory-width table pins `LWU`'s size and signedness beside `lb`/`lh`/`lw`/
  `ld`/`lbu`/`lhu`;
* only funct3 `111` is left in the reserved class.

**Verification after the change.**

```
python3 tools/run_unit.py --profile p0 --case decode.rv64im_reserved
  RESULT PASS decode.rv64im_reserved  166734 instructions (82915 legal, 83819 illegal),
  0 mismatches, 1442 named reserved checks            (exit 0)

-DMOSAIC_DECODER_MUTANT_LWU_ILLEGAL
  exit 1, 5 failures
  first: LWU (LOAD funct3 110) is a legal encoding
  field mismatch: insn=0x00816083.valid: expected 0x0000000000000001, got 0x0000000000000000
```

The legal population rises by 531 instructions and the named reserved checks
fall by one: funct3 `110` moved from the reserved class to the legal one, and
the reserved class for LOAD is now the single encoding `111`.

**A pre-existing defect this addendum also records.** The case did not *build*
on the tree I-017 started from:

```
%Warning-WIDTHTRUNC: sim/tb/mosaic_decoder_tb.sv:85:21: Operator ASSIGNW expects
134 bits on the Assign RHS, but Assign RHS's VARREF 'ctl' generates 135 bits.
%Error: Exiting due to 1 warning(s)
```

`mosaic_pkg::decode_ctl_t` grew `is_wfi` (the core integration recognises WFI in
the front end and left the decoder's illegal set alone) and neither the
wrapper's flattened `o_ctl_bits` nor this driver's bit accounting was updated.
It reproduces with the unmodified decoder, so it is not a consequence of the
`LWU` change. The accounting was completed — `is_wfi` is exported and pushed, the
total is 135 bits and the top word's mask widened from `0x3F` to `0x7F` — because
the case has to build to be extended at all. Whoever owns the decoder wrapper
should confirm the fix; a case that cannot build is not a case that passes.
