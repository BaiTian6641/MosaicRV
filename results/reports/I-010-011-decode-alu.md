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
### Why the comparator is a second module

The port list fixed for `mosaic_branch_target` takes `branch_taken` as an *input*:
the comparison result arrives from outside. The card nevertheless requires the
branch comparison to be signed for BLT/BGE and unsigned for BLTU/BGEU, and requires
a negative control for a BLTU that compares signed. A unit with no operand inputs
cannot host either, so `mosaic_branch_cmp` is delivered as its own module in the
file next to it: one implementation of the six conditions, one place the
signed/unsigned rule lives, one place a mutant can break it. The core wires the
comparator's `taken` into the target unit's `branch_taken`, and the case checks that
closed chain explicitly as well as checking each unit against its own model.

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
thinking about. The port list was under-specified, and the honest description is
that it was specified badly, not that the first implementation was implemented
badly.

The fix is the interface itself: `decode_ctl_t` already carries `is_branch`, `is_jal`
and `is_jalr` as separate fields, so the decoder states what the instruction is and
this unit does arithmetic on the statement. Two properties are worth having:

* the unit is total — every combination of the three flags and `branch_taken` has a
  defined answer, and the sweep exercises all sixteen combinations including the ones
  no decoder emits;
* a mis-wired caller fails loudly. `is_branch & branch_taken` means an instruction
  that named no branch cannot be redirected by a stray comparator answer, so a
  comparator left driving the wrong instruction is visible as a wrong branch
  decision rather than as a silent redirect of a JAL.

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
0x7ffffffc, 0xffffffff80000004, 3, 5`. The set deliberately contains offsets whose low
bits are 1 and 2, so the difference between "JALR clears bit 0" and "JALR clears bits
1 and 0" is visible, and offsets that make `pc + imm` misaligned in bit 0, in bit 1,
and in both.

| Phase | Vectors | What it is |
| --- | --- | --- |
| branch-target sweep | 2 816 | 11 pcs × 16 imms × all 16 combinations of `is_branch` × `is_jal` × `is_jalr` × `branch_taken` |
| comparator sweep | 3 200 | all 8 funct3 × all 400 pairs of the 20-value boundary operand set |
| closed chain | 3 520 | the DUT comparator's answer driving the DUT target unit, 8 funct3 × 20 operand values × 11 pcs × both `is_jalr` |
| branch random | 40 000 | `--seed`ed; a realistic instruction kind 95 % of the time, 70 % aligned pcs and small immediates, 30 % sign-crossing or x0 operands |
| **total applied** | **49 536** | seed 1 |

The sweep includes flag combinations no decoder emits — all three flags low, and
`is_jal` with `is_jalr` — on purpose: those are the vectors that prove the unit fails
loudly rather than silently on a mis-wired input.

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
JAL at pc=0 links to pc+4 and targets pc+imm    link wraps to zero at the top of the address space
a JALR at the top of the address space targets zero  an odd pc links to pc+4, unaligned
JALR clears bit 0 of the target                  JALR keeps bit 1: a target 2 bytes past pc
JALR keeps bit 1 with a 6-byte offset            JALR clears bit 0 of pc+imm, not of imm
JALR keeps bit 1 of a large misaligned target    is_jalr wins over is_jal, and still clears bit 0
JAL leaves a 2-byte-aligned target for the trap logic
a taken forward branch yields pc+imm             an untaken forward branch yields pc+4
an untaken backward branch yields pc+4           an untaken branch with bit 1 set in imm yields pc+4
branch_taken high with no branch named transfers nothing
an ordinary ALU instruction falls through to pc+4  an unconditional jump ignores branch_taken
beq(7,7) is true                                 beq(7,8) is false
bne(7,7) is false                                blt(-1,1) is true, signed
blt(1,-1) is false                               bge(-1,1) is false
bge(1,-1) is true                                bltu(-1,1) is false, -1 is the largest unsigned
bgeu(-1,1) is true                               blt(x0,-1) is false: x0 reads as zero
bltu(x0,-1) is true: x0 reads as zero            beq(x0,x0) is true
blt(INT64_MIN,INT64_MAX) is true                 bltu(INT64_MIN,INT64_MAX) is false
funct3 010 compares as no condition              funct3 011 compares as no condition
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
0x7ffffffc, 0xffffffff80000004, 3, 5`. The set deliberately contains offsets whose low
bits are 1 and 2, so the difference between "JALR clears bit 0" and "JALR clears bits
1 and 0" is visible, and offsets that make `pc + imm` misaligned in bit 0, in bit 1,
and in both.

| Phase | Vectors | What it is |
| --- | --- | --- |
| branch sweep | 5 632 | 8 funct3 × 11 pcs × 16 imms × both `is_jalr` × both `branch_taken` |
| comparator sweep | 3 200 | all 8 funct3 × all 400 pairs of the 20-value boundary operand set |
| closed chain | 3 520 | the DUT comparator's answer driving the DUT target unit, 8 funct3 × 20 operand values × 11 pcs × both `is_jalr` |
| branch random | 20 000 | `--seed`ed; 70 % aligned pcs and small immediates, 30 % sign-crossing or x0 operands |
| **total applied** | **32 352** | seed 1 |

### Branch reference models

`ReferenceBranchCmp(funct, rs1, rs2)` switches on the funct3 and compares in
`int64_t` for BLT/BGE and in `uint64_t` for BLTU/BGEU, returning false for the two
funct3 values that name no condition.

`ReferenceBranchTarget(pc, imm, is_jalr, funct, branch_taken)` computes
`link = pc + 4`, `sum = pc + imm`, `aligned = is_jalr ? (sum & ~1) : sum`,
`is_taken = is_jalr || !names_condition || branch_taken`, and
`target = is_taken ? aligned : link` — the same four rules as the RTL, written
independently in host integers.

The two units are checked separately, with `branch_taken` driven from the stimulus
rather than from the DUT, so a wrong condition cannot hide behind a wrong address or
the other way round. The closed chain is checked on top of that, with the DUT
comparator's answer driving the DUT target unit and the pair compared against the
model chain — which is the configuration the core will actually wire.

### Branch invariants asserted

32 hand-written branch invariants, checked against the RTL without going through the
models:

```
link at pc=0 is pc+4 and the target is pc+imm        link wraps to zero at the top of the address space
a JALR at the top of the address space links to zero an odd pc links to pc+4, unaligned
JALR clears bit 0 of the target                      JALR keeps bit 1: a target 2 bytes past pc
JALR keeps bit 1 with a 6-byte offset                JALR clears bit 0 of pc+imm, not of imm
JALR keeps bit 1 of a large misaligned target        JAL leaves a 2-byte-aligned target for the trap logic
a branch with bit 1 set in imm falls through to pc+4  an untaken forward branch yields pc+4
an untaken backward branch yields pc+4               a taken forward branch yields pc+imm
funct3 010 names no condition and still transfers    funct3 011 names no condition and still transfers
beq(7,7) is true                                     beq(7,8) is false
bne(7,7) is false                                    blt(-1,1) is true, signed
blt(1,-1) is false                                   bge(-1,1) is false
bge(1,-1) is true                                    bltu(-1,1) is false, -1 is the largest unsigned
bgeu(-1,1) is true                                   blt(x0,-1) is false: x0 reads as zero
bltu(x0,-1) is true: x0 reads as zero                beq(x0,x0) is true
blt(INT64_MIN,INT64_MAX) is true                     bltu(INT64_MIN,INT64_MAX) is false
funct3 010 compares as no condition                  funct3 011 compares as no condition
```

Plus per-vector checks of `link`, `target`, `is_taken` and the comparator's `taken`,
plus these coverage assertions:

* every one of the eight funct3 values was exercised;
* a taken transfer and a not-taken branch were both seen;
* **every** not-taken branch produced `target == link`;
* JALR and the non-JALR path were both exercised;
* a JALR cleared a bit 0 that was set;
* a JALR left a misaligned bit 1 in the target — i.e. "clears bit 0 only" was seen
  happening, not merely asserted;
* both outcomes of every condition were seen, and a comparison against a zero (x0)
  operand was made;
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
| `MOSAIC_BRANCH_TARGET_MUTANT_2` | `link` is `pc + 2` | `link at pc=0`: expected `0x0000000000000004`, got `0x0000000000000002` | 1 |
| `MOSAIC_BRANCH_TARGET_MUTANT_3` | a not-taken branch leaks the branch target | an untaken branch with `imm=2`: expected `0x0000000000000004`, got `0x0000000000000002` | 1 |
| `MOSAIC_BRANCH_CMP_MUTANT_1` | BLTU/BGEU compare with the signed relation | `bltu(-1, 1)`: expected `0`, got `1` | 1 |

The run stops at eight failed checks so a mutant log stays readable; the coverage
assertions are skipped in that case, because after a deliberate halt they would be
reporting the truncation rather than anything about the stimulus.

## Two things that went wrong on the way, kept on the record

**Mutant 4 passed the first time.** The first time the mutant table was produced,
**mutant 4 passed** — because the `ifdef` block for it had never been written, so
`-DMOSAIC_ALU_MUTANT_4` compiled to nothing and the run was bit-identical to the
shipping build. A define that selects nothing builds cleanly and a case that only
exercises the good path reports a pass, which is exactly the failure mode that makes
mutation evidence worthless. The block now exists, the defect is detected, and this
is the same reason `tools/lint_rtl.py --self-test` requires a deliberately latching
module to actually produce a LATCH warning: a gate that has never been seen to fail is
not a gate.

**Two of the hand-written constants were wrong when first written.** A 6-bit-versus-
64-bit reading of one `sll` case (the model of `sll(x, 0x80000000)` was written as if
the shift amount were not masked to six bits) and the wrong sign extension on one
`srlw` case. Both were caught by the RTL on the first run and are the reason the
boundary set is worth its size: a reference model that is wrong in the same direction
as the DUT is worse than no model, and these two were wrong in exactly the direction
the boundary values are chosen to expose. Two more branch constants were wrong the
same way on the first run of Part II (a target written as `link` where the answer was
`pc + imm`, and `blt(-1,1)` written as false where it is true), again caught
immediately by the RTL.

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
- Verilator: Built from 0.109 MB sources in 6 modules, into 0.039 MB in 3 C++ files needing 0.000 MB
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

### The case, seed 1

```
$ python3 tools/run_unit.py --case alu.boundaries; echo $?
PASS alu.boundaries               task=I-011
0

$ cat results/unit/alu.boundaries/run.log
$ /Users/flare/MosaicRV/build/p0/unit/alu.boundaries/alu.boundaries --case alu.boundaries --out /Users/flare/MosaicRV/results/unit/alu.boundaries --seed 1 --max-cycles 200000
RESULT PASS alu.boundaries 108560 ALU and 32352 branch stimulus vectors (57 hand-written invariants, seed 1); alu_ops=add=4895 sub=4738 sll=9855 slt=4767 sltu=4744 xor=4871 srl=10105 sra=10252 or=4732 and=4894 addw=4813 subw=4753 sllw=10288 srlw=10144 sraw=9986 passb=4723; branch=28832 vectors, chain=3520

$ python3 -c "import json;d=json.load(open('results/unit/alu.boundaries/result.json'));print('checks',d['checks'],'failures',d['failures'],'seed',d['seed'])"
checks 339694 failures 0 seed 1
```

339 694 checks: two or three per stimulus vector, two or three per hand-written
invariant, one model self-consistency check, and 90 coverage assertions. Every ALU op
is exercised between 4 700 and 10 300 times.

### Seed independence

The random campaigns are reproducible from `--seed`; the case does not pass on seed 1
alone.

```
$ for s in 2 7 4294967296; do build/p0/unit/alu.boundaries/alu.boundaries \
    --case alu.boundaries --out /tmp/as_$s --seed $s --max-cycles 200000; echo "exit=$?"; done
RESULT PASS alu.boundaries 108560 ALU and 32352 branch stimulus vectors (57 hand-written invariants, seed 2); alu_ops=add=4672 sub=4685 sll=10161 slt=4879 sltu=4832 xor=4693 srl=10238 sra=10329 or=4824 and=4664 addw=4717 subw=4756 sllw=10136 srlw=10144 sraw=10118 passb=4712; branch=28832 vectors, chain=3520
exit=0
RESULT PASS alu.boundaries 108560 ALU and 32352 branch stimulus vectors (57 hand-written invariants, seed 7); alu_ops=add=4770 sub=4787 sll=10255 slt=4825 sltu=4645 xor=4800 srl=10030 sra=10225 or=4876 and=4726 addw=4796 subw=4684 sllw=10059 srlw=10165 sraw=10132 passb=4785; branch=28832 vectors, chain=3520
exit=0
RESULT PASS alu.boundaries 108560 ALU and 32352 branch stimulus vectors (57 hand-written invariants, seed 4294967296); alu_ops=add=4710 sub=4742 sll=10130 slt=4830 sltu=4881 xor=4783 srl=10064 sra=9976 or=4789 and=4834 addw=4946 subw=4822 sllw=10249 srlw=10086 sraw=10015 passb=4703; branch=28832 vectors, chain=3520
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
RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 63 branch vectors
exit=1
=== MOSAIC_BRANCH_TARGET_MUTANT_2 build=0 ===
MISMATCH link at pc=0 is pc+4 and the target is pc+imm: link: expected 0x0000000000000004, got 0x0000000000000002
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 19 failed checks over 0 ALU and 0 branch vectors
exit=1
=== MOSAIC_BRANCH_TARGET_MUTANT_3 build=0 ===
MISMATCH a branch with bit 1 set in imm falls through to pc+4: target: expected 0x0000000000000004, got 0x0000000000000002
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 21 branch vectors
exit=1
=== MOSAIC_BRANCH_CMP_MUTANT_1 build=0 ===
MISMATCH bltu(-1,1) is false, -1 is the largest unsigned: expected 0, got 1
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 8560 ALU and 4236 branch vectors
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
* `XLEN` is a parameter on all three modules, but the word forms and the 6-bit/5-bit
  shift masks are written for RV64. Only `XLEN = 64` is instantiated in this profile.
* The `alu_op_e` encodings are transcribed by hand into `sim/unit/tb_alu.cpp`, since
  C++ cannot import a SystemVerilog package. I-010's `decode.rv64im_reserved` checks
  the same numbering from the RTL side; the two transcriptions must agree. The branch
  funct3 encodings in this report and in `mosaic_branch_cmp` are the ISA literals
  (BEQ=0, BNE=1, BLT=4, BGE=5, BLTU=6, BGEU=7; 2 and 3 name no condition), not a
  new set of constants.
* `rtl/core/mosaic_pkg.sv` was not modified.