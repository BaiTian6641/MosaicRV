# I-010 / I-011 — RV64I/M decode and the integer ALU

Work package **I-011** (integer ALU), run as `CASE=alu.boundaries` from
`tests/unit/registry.json`. The card is `### I-011 — 实现整数 ALU 与分支目标计算`
in `docs/stage-1-scalar-control.md`.

**This section covers the integer ALU only.** The I-011 card also carries branch
condition evaluation, JAL/JALR link values and target computation; those are a
separate unit with its own interface and are *not* implemented in the files
described here. What this unit provides for them is `ALU_PASSB`, the pass-through
op that address generation uses. See *Scope, and what this section does not claim*.

---

## What was built

| File | Contents |
| --- | --- |
| `rtl/core/mosaic_alu.sv` | `mosaic_alu`, all sixteen `alu_op_e` encodings, purely combinational |
| `sim/tb/mosaic_alu_tb.sv` | Port-for-port simulation wrapper (the DUT has no clock, so that is all the wrapper can be) |
| `sim/unit/tb_alu.cpp` | Independent reference model, boundary matrix, shift sweep, seeded random campaign, coverage assertions, five negative controls |

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

## Behaviour, in prose

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

## The testbench

`CASE=alu.boundaries` → `python3 tools/run_unit.py --case alu.boundaries`.

The DUT is combinational, so the top-level wrapper has no clock and no reset, and
the driver settles each vector with a single `eval()`. `--max-cycles` therefore
bounds stimulus vectors, not clock cycles. There is no reset schedule because there
is no state to reset.

### Stimulus

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

**Phases**

| Phase | Vectors | What it is |
| --- | --- | --- |
| operand matrix | 6 400 | all 20 × 20 pairs × all 16 ops |
| shift sweep | 2 160 | 6 shift ops × 20 boundary values × 9 amounts, in both operand orders |
| random campaign | 100 000 | `--seed`ed, biased onto the interesting shapes |
| **total applied** | **108 560** | seed 1 |
| hand-written invariants | 25 | checked against the RTL without going through the model; not counted as stimulus vectors |

Half the random vectors are biased onto shapes that uniform noise essentially never
produces: a boundary operand on one side; a small `b` (a realistic shift amount)
with a shift op; a `b` whose bits are all set above bit 5; operands differing only
above bit 32; low-entropy all-ones/all-zeros operands; and sign-crossing pairs equal
in magnitude but differing in sign.

### The reference model, and how each expected value is computed

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

Finally, the 25 hand-written invariants are checked against the RTL **without going
through the model at all**, so a shared mistake in `ReferenceAlu` cannot make the
model and the RTL agree for the wrong reason.

### Invariants asserted

The 25 hand-written invariants, one line each:

```
add(0,0) is zero and sets zero                         add(1,-1) is zero while a != 0
add(INT64_MAX,1) wraps to INT64_MIN                    sub(0,1) is -1
slt(-1,1) is 1, signed                                 sltu(-1,1) is 0, -1 is the largest unsigned value
slt(INT64_MIN,1) is 1                                  sltu(INT64_MIN,1) is 0
sra(INT64_MIN,1) replicates the sign bit               srl(INT64_MIN,1) shifts a zero in
sll by 64 is a shift by 0                              sll by the all-ones mask is a shift by 63
sll masks the shift amount to six bits, ignoring bit 31
addw sign-extends its 32-bit answer                    addw ignores the bits above bit 31 of its operands
addw of a sign-extended operand ignores the upper bits subw wraps at 32 bits and sign-extends
sllw by 32 is a shift by 0, not a shift to zero        sllw by the all-ones mask is a shift by 31
sraw replicates bit 31 of its operand                  srlw shifts a zero in above bit 31
or of complementary patterns                           and of complementary patterns is zero
xor of complementary patterns                          passb returns b unchanged
```

Each is two checks, one on `result` and one on `zero`, each named in the failure
output.

Plus, per vector: `result` against the model, and `zero` against `result == 0`.

Plus coverage, asserted at the end so that a stimulus edit which stops reaching
something fails the case instead of quietly passing over a shorter path:

* every one of the sixteen ops was exercised, produced a zero result, produced a
  non-zero result, and was seen at `a == b == 0`;
* `slt` and `sltu` each saw the `-1` against `1` pair;
* `sra` saw an operand with bit 63 set;
* `sllw` saw a shift amount that masks to 32, and `sll` saw one that masks to 0 but
  is not 0;
* `addw` produced a sign-extended negative word result, and `sraw` shifted an
  operand with bit 31 set;
* some op produced zero from a non-zero `a`.

Those last eight are exactly the operand shapes that separate the five mutants, so a
run that cannot distinguish signed from unsigned, 5-bit from 6-bit masking, or
arithmetic from logical shift fails rather than reporting a pass.

## Negative controls

Five deliberate defects, each behind a `-D` that is off in the shipping build, each
injected into `rtl/core/mosaic_alu.sv`, and each demonstrated below to fail the case
with exit 1. The wrapper and the C++ driver are identical in all six builds; only the
`-D` changes.

| Mutant | Injected defect | First mismatch reported | Exit |
| --- | --- | --- | --- |
| `MOSAIC_ALU_MUTANT_1` | `extend_word` zero-extends the W answer | `addw(0x7fffffff, 1)`: expected `0xffffffff80000000`, got `0x0000000080000000` | 1 |
| `MOSAIC_ALU_MUTANT_2` | `sra` implemented as a logical shift | `sra(0x8000000000000000, 1)`: expected `0xc000000000000000`, got `0x4000000000000000` | 1 |
| `MOSAIC_ALU_MUTANT_3` | `sllw` shifts by six bits, not five | `sllw(-1, 32)`: expected `0xffffffffffffffff`, got `0x0000000000000000` | 1 |
| `MOSAIC_ALU_MUTANT_4` | `slt` uses the unsigned comparison | `slt(-1, 1)`: expected `0x0000000000000001`, got `0x0000000000000000` | 1 |
| `MOSAIC_ALU_MUTANT_5` | `zero` computed from `a` instead of `result` | `add(1, -1)`: expected `zero=1`, got `zero=0` | 1 |

The run stops at eight failed checks so a mutant log stays readable; the coverage
assertions are skipped in that case, because after a deliberate halt they would be
reporting the truncation rather than anything about the stimulus.

Mutant 4 is worth calling out as a process result: the first time this table was
produced, **mutant 4 passed** — because the `ifdef` block for it had never been
written, so `-DMOSAIC_ALU_MUTANT_4` compiled to nothing. A negative control that is
never exercised proves nothing. The block now exists, and the defect is detected.

## Evidence

### Lint — `rtl/core/mosaic_alu.sv` and `sim/tb/mosaic_alu_tb.sv`

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_alu_tb \
    -Irtl/core rtl/core/mosaic_alu.sv sim/tb/mosaic_alu_tb.sv \
  | grep -oE '^%[A-Za-z-]+: [a-z/._A-Za-z0-9]+\.sv' | sort | uniq -c
     32 %Warning-UNUSEDPARAM: rtl/core/mosaic_pkg.sv
```

Every warning is `UNUSEDPARAM` inside `rtl/core/mosaic_pkg.sv`, which is read-only for
this package: they are the constants an ALU has no business referencing (`OP_LOAD`,
`EXC_MISALIGNED`, …). A `lint_off` region in `mosaic_alu.sv` cannot silence them,
because Verilator attributes the warning to the definition rather than to the use
site. With that one warning class suppressed:

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSEDPARAM --top-module mosaic_alu_tb \
    -Irtl/core rtl/core/mosaic_alu.sv sim/tb/mosaic_alu_tb.sv; echo $?
- V e r i l a t i o n   R e p o r t: Verilator 5.052 2026-09-05 rev vUNKNOWN-built20260905
- Verilator: Built from 0.061 MB sources in 4 modules, into 0.026 MB in 3 C++ files needing 0.000 MB
0
```

Zero warnings and zero errors from the two files this package owns. **Recommendation
for integration:** a `/* verilator lint_off UNUSEDPARAM */` region at the top of
`rtl/core/mosaic_pkg.sv`, or `-Wno-UNUSEDPARAM` in the Makefile's
`VERILATOR_LINT_FLAGS`, would make `make lint` clean repo-wide. Either is a one-line
change to a file this package does not own.

### The case, seed 1

```
$ python3 tools/run_unit.py --case alu.boundaries; echo $?
PASS alu.boundaries               task=I-011
0

$ cat results/unit/alu.boundaries/run.log
$ /Users/flare/MosaicRV/build/p0/unit/alu.boundaries/alu.boundaries --case alu.boundaries --out /Users/flare/MosaicRV/results/unit/alu.boundaries --seed 1 --max-cycles 200000
RESULT PASS alu.boundaries 108560 stimulus vectors (25 hand-written invariants, seed 1); add=4895 sub=4738 sll=9855 slt=4767 sltu=4744 xor=4871 srl=10105 sra=10252 or=4732 and=4894 addw=4813 subw=4753 sllw=10288 srlw=10144 sraw=9986 passb=4723

$ python3 -c "import json;d=json.load(open('results/unit/alu.boundaries/result.json'));print('checks',d['checks'],'failures',d['failures'],'seed',d['seed'])"
checks 217244 failures 0 seed 1
```

217 244 checks: 2 per stimulus vector (217 120), 2 per hand-written invariant (50), one
model self-consistency check, and 73 coverage assertions. Every op is exercised
between 4 700 and 10 300 times.

### Seed independence

The random campaign is reproducible from `--seed`; the case does not pass on seed 1
alone.

```
$ for s in 2 7 12345 4294967296; do build/p0/unit/alu.boundaries/alu.boundaries \
    --case alu.boundaries --out /tmp/alu_seed_$s --seed $s --max-cycles 200000; echo "exit=$?"; done
RESULT PASS alu.boundaries 108560 stimulus vectors (25 hand-written invariants, seed 2); add=4672 sub=4685 sll=10161 slt=4879 sltu=4832 xor=4693 srl=10238 sra=10329 or=4824 and=4664 addw=4717 subw=4756 sllw=10136 srlw=10144 sraw=10118 passb=4712
exit=0
RESULT PASS alu.boundaries 108560 stimulus vectors (25 hand-written invariants, seed 7); add=4770 sub=4787 sll=10255 slt=4825 sltu=4645 xor=4800 srl=10030 sra=10225 or=4876 and=4726 addw=4796 subw=4684 sllw=10059 srlw=10165 sraw=10132 passb=4785
exit=0
RESULT PASS alu.boundaries 108560 stimulus vectors (25 hand-written invariants, seed 12345); add=4688 sub=4891 sll=10123 slt=4863 sltu=4669 xor=4732 srl=10294 sra=10087 or=4760 and=4798 addw=4700 subw=4764 sllw=10149 srlw=10221 sraw=9991 passb=4830
exit=0
RESULT PASS alu.boundaries 108560 stimulus vectors (25 hand-written invariants, seed 4294967296); add=4710 sub=4742 sll=10130 slt=4830 sltu=4881 xor=4783 srl=10064 sra=9976 or=4789 and=4834 addw=4946 subw=4822 sllw=10249 srlw=10086 sraw=10015 passb=4703
exit=0
```

### Third cross-check of the hand-written constants

The 25 hand-written expected values were recomputed by an independent
arbitrary-precision Python model — a third implementation, in a third language, with
none of C++'s integer-conversion rules — and compared against the literals in
`sim/unit/tb_alu.cpp`:

```
$ python3 /tmp/xcheck_alu.py      # throwaway script, not committed
checked 25 hand-written invariants against the independent python model, 0 wrong
```

This is not decoration. Two of the 25 constants were wrong when first written — a
6-bit-versus-64-bit reading of one `sll` case, and the wrong extension on one `srlw`
case — and the RTL caught both on the first run.

### C++ lint

```
$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -Isim/common -Ibuild/p0/sim \
    -Ibuild/p0/unit/alu.boundaries/obj_dir -I$(verilator -getenv VERILATOR_ROOT)/include \
    sim/unit/tb_alu.cpp 2>&1 | grep 'tb_alu.cpp:[0-9]'
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
for n in 1 2 3 4 5; do
  d=$R/build/p0/unit/alu.mutant$n; rm -rf $d; mkdir -p $d
  verilator --cc --exe --build -j 0 -O2 -CFLAGS "-O2 -std=c++17 -Wall" \
    --x-assign unique --x-initial unique --top-module mosaic_alu_tb -Mdir $d/obj_dir \
    -I$R/build/p0/sim -I$R/rtl/core -I$R/rtl/common \
    -CFLAGS -I$R/sim/common -CFLAGS -I$R/build/p0/sim \
    -DMOSAIC_ALU_MUTANT_$n -o $d/alu.mutant$n \
    $R/sim/tb/mosaic_alu_tb.sv $R/rtl/core/mosaic_alu.sv $R/sim/unit/tb_alu.cpp \
    $R/sim/common/sim_common.cpp
  $d/alu.mutant$n --case alu.boundaries --out $d/out --seed 1 --max-cycles 200000
  echo "exit=$?"
done
```

Output:

```
=== MOSAIC_ALU_MUTANT_1 : build exit=0 ===
MISMATCH addw sign-extends its 32-bit answer: expected 0xffffffff80000000, got 0x0000000080000000
ABORT: 8 checks failed (last: matrix addw(a=0x0000000000000000, b=0xffffffffffffffff) result); stopping to keep the log readable
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 4005 vectors
--- MUTANT 1 : run exit=1 ---
=== MOSAIC_ALU_MUTANT_2 : build exit=0 ===
MISMATCH sra(INT64_MIN,1) replicates the sign bit: expected 0xc000000000000000, got 0x4000000000000000
ABORT: 8 checks failed (last: matrix sra(a=0xffffffffffffffff, b=0x000000007fffffff) result); stopping to keep the log readable
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 2889 vectors
--- MUTANT 2 : run exit=1 ---
=== MOSAIC_ALU_MUTANT_3 : build exit=0 ===
MISMATCH sllw by 32 is a shift by 0, not a shift to zero: expected 0xffffffffffffffff, got 0x0000000000000000
ABORT: 8 checks failed (last: matrix sllw(a=0x0000000000000001, b=0xfffffffffffffffe) zero flag); stopping to keep the log readable
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 4826 vectors
--- MUTANT 3 : run exit=1 ---
=== MOSAIC_ALU_MUTANT_4 : build exit=0 ===
MISMATCH slt(-1,1) is 1, signed: expected 0x0000000000000001, got 0x0000000000000000
ABORT: 8 checks failed (last: matrix slt(a=0x0000000000000000, b=0xfffffffffffffffe) zero flag); stopping to keep the log readable
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 1206 vectors
--- MUTANT 4 : run exit=1 ---
=== MOSAIC_ALU_MUTANT_5 : build exit=0 ===
MISMATCH add(1,-1) is zero while a != 0: expected zero=1, got zero=0
ABORT: 8 checks failed (last: matrix add(a=0x0000000000000000, b=0x0000000000000003) zero flag); stopping to keep the log readable
coverage: not asserted, the run halted at the failure limit
RESULT FAIL alu.boundaries 8 failed checks over 4 vectors
--- MUTANT 5 : run exit=1 ---
```

Every mutant exits 1 with a named first mismatch. Mutant 5 reports after only four
vectors because the hand-written invariants run before the matrix and `add(1, -1)` is
the second of them; it fails on the `zero` flag alone, with every `result` value
correct, which is what makes it a control for the flag rather than for the
datapath.

## Scope, and what this section does not claim

* **Branch condition evaluation, JAL/JALR link values and target computation** — the
  other half of the I-011 card — are **not** implemented here. They need their own
  interface (`funct3` condition, PC, immediate, JALR's bit-0 clear, 4-byte target
  alignment, and the misaligned-target policy) and their own case. `ALU_PASSB` is
  provided for them. No claim is made about that half of I-011.
* `XLEN` is a parameter, but the word forms are written for RV64 (32-bit W operands,
  6-bit and 5-bit shift masks). Only `XLEN = 64` is instantiated in this profile,
  which is what `mosaic_pkg::MOSAIC_XLEN` freezes.
* The `alu_op_e` encodings are transcribed by hand into `sim/unit/tb_alu.cpp`, since
  C++ cannot import a SystemVerilog package. I-010's `decode.rv64im_reserved` checks
  the same numbering from the RTL side; the two transcriptions must agree.
* `rtl/core/mosaic_pkg.sv` was not modified.