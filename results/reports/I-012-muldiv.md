# I-012 — the shared iterative MUL/DIV unit, its decoder extension, and its unit case

Work package **I-012** (`docs/implementation-plan.md`), run as
`CASE=muldiv.kill_and_edges` from `tests/unit/registry.json`. Validation-plan
card V-026 ("M 的长延迟与边界测试") names the same mechanisms: high-multiply
signed combinations, W results, divide-by-zero, MIN/−1, quotient/remainder
signs, and a flush inserted *during* the operation.

```
python3 tools/run_unit.py --profile p0 --case muldiv.kill_and_edges
  RESULT PASS muldiv.kill_and_edges  muldiv contract holds: 76192 shadow comparisons
  over 76228 cycles, 1238 directed operations, 98 cancellations (2 with a computed
  result), latency 65/33 cycles (64/W), seed 1
  checks 1155304, failures 0, exit 0

python3 tools/run_unit.py --profile p0 --case decode.rv64im_reserved
  RESULT PASS decode.rv64im_reserved  166733 instructions (82384 legal, 84349 illegal),
  0 mismatches, 1443 named reserved checks
  checks 169534, failures 0, exit 0

python3 tools/run_unit.py --profile p0 --case core.bringup_vs_reference
  RESULT PASS core.bringup_vs_reference  corpus 39 programs, 2774346 architectural
  events, every stream identical to the independent reference
```

## What was built

| File | Contents |
| --- | --- |
| `rtl/core/mosaic_muldiv.sv` (new) | `mosaic_muldiv`, the frozen interface: one operation in flight, shift-add multiplier, restoring divider, one shared iteration counter, result held until `res_ready_i`, absolute-priority flush, four counters, six mutation hooks |
| `sim/tb/mosaic_muldiv_tb.sv` (new) | Port-for-port wrapper; identity widths derived from the generated identity package and read back out as `o_rob_index_w`/`o_rob_gen_w`/`o_uop_index_w` so the driver sizes its shadow from the DUT |
| `sim/unit/tb_muldiv.cpp` (new) | Host-arithmetic reference (`__int128`), an independent control shadow, per-cycle comparison, standing invariants, seven phases |
| `rtl/core/mosaic_pkg.sv` | `logic md_w` in `decode_ctl_t`; `localparam OP_32 = 7'b0111011`, whose comment was corrected to the ISA's content (RV64I word arithmetic + RV64M word forms, not "RV32 forms") |
| `rtl/core/mosaic_decoder.sv` | The OP-32 arm, both classes: funct7 0000000/0100000 → the five RV64I word forms `addw subw sllw srlw sraw` (`ALU_ADDW`/`ALU_SUBW`/`ALU_SLLW`/`ALU_SRLW`/`ALU_SRAW`), funct7 0000001 → the five M word forms with `md_w` set and funct3 001/010/011 reserved. `RV32_WORD_LEGAL` deleted, `W_FORMS_ILLEGAL` added (see the correction section) |
| `sim/tb/mosaic_decoder_tb.sv` | `o_md_w` break-out; `o_ctl_bits` widened 133 → 134 **\*** |
| `sim/unit/tb_decoder.cpp` | `md_w` in the reference struct, the OP-32 reference decode for both classes, five positive RV64I word-form rows, ten binutils-encoding rows, the eight M word forms under funct7 0000001, the reserved funct7 and funct3 classes, `md_w` in the illegal-state and break-out checks |

**\* File-ownership note.** `sim/tb/mosaic_decoder_tb.sv` is not in this card's
file list. It had to change: adding `md_w` to `decode_ctl_t` changes the width
of the struct the wrapper exposes as `o_ctl_bits` (133 → 134 bits) and the driver
needs a port to read the new field through. Without the three-line edit (port,
one `assign`, one width) the card's own requirement — "extend `tb_decoder.cpp`
with the 8 W forms" — cannot be met at all. The change is reported here rather
than left silent for that reason. No other lane's files were touched.

## The interface, as frozen

Every port name, direction and width is exactly the frozen list. The identity
widths are declared at file scope from the generated package
(`mosaic_id_pkg::MOSAIC_ID_W_ROB_INDEX` / `_ROB_GEN` / `_UOP_INDEX`) rather than
written into the port list as literals, following the convention
`rtl/core/mosaic_retire.sv` established (a port list cannot see a declaration
inside the module body). At p0 those are 6 / 7 / 3 bits. The unit is otherwise
independent of the geometry: it has no depth, no bank count and no array.

`mosaic_pkg.sv` and the generated identity header are both **included**, not
assumed present on the command line. Verilator does not search the include path
for an unresolved package (only for an unresolved `import`, which is why
`mosaic_decoder.sv` works with no package listed); `rtl/core/mosaic_fetch.sv`
documents the same conclusion. The include is idempotent — `mosaic_pkg.sv`
carries its own guard, and the identity header is wrapped in a scoped
`lint_off MODDUP`.

## Design decisions worth stating

**One operation in flight, no result FIFO.** The card's documented failure is a
long-latency unit that drops a result when its result FIFO is full. A unit with
no FIFO cannot have that failure. What it has instead is one result register set
that does not move until `res_ready_i`: the value and the identity payload
(`rob_index`, `rob_gen`, `uop_index`) are latched with the request and read out
unchanged while the unit waits. The test's back-pressure phase reloads every
*request* input differently on 100 consecutive stalled cycles and requires the
offered value and identity not to move — an input leak into the result is not
theoretical, the `MOSAIC_MULDIV_MUTANT_UNSTABLE_RESULT` hook is exactly that
defect and the phase catches it.

**Latency depends on control only.** 64 iterations for a 64-bit operation, 32
for a W form, `width + 1` cycles from the accepting edge to `res_valid_o`. Never
fewer for a zero operand, a power-of-two divisor or an early dividend term. The
`latency-equal` phase runs each operation at each width with six operand vectors
chosen to tempt a value-dependent early-out (0, 1, all-ones, MIN/−1, a power of
two, an alternating pattern) and requires the same cycle count every time; the
observed latency is 65/33 cycles (64-bit/W).

**The width rule.** A W form is the ISA's 32-bit operation, not a 64-bit
operation truncated: `divuw 0xffffffff, 2` is `0x7fffffff`, and computing it at
64 bits with sign-extended operands gives `0xffffffff`. The unit therefore
carries a working width (32/64), stores the operands as raw working-width
patterns, reads the sign bits at bit `width - 1`, runs `width` iterations and
sign-extends a 32-bit answer exactly once. The decoder produces the five word
forms the ISA defines; the three high-half combinations that no instruction can
reach (`md_op` ∈ {MULH, MULHSU, MULHU} with `md_w`) are still *defined* — the
same operation at the working width — because the port is the full three-bit
encoding and a defined answer on an unreachable input is a unit a later package
can reason about.

**Signedness is a property of the operation, not of the data.** `mulhsu` mixes
one signed and one unsigned operand, which is the whole reason it is its own
encoding; `mulhu`/`divu`/`remu` are unsigned on both sides.

**Flush has absolute priority.** It wins over an iteration step, over a result
handshake and over a request: `req_ready_o` is gated by it, so a request that
arrives in a flush cycle is refused rather than accepted and immediately
cancelled. `res_valid_o` is masked by it, so no result is offered in the flush
cycle either — a consumer that is being squashed cannot be relied on to ignore
one. After the flush edge the unit is in IDLE and accepts on the very next
cycle, which the cancel sweep checks on every one of its 98 cancellations.

**Two cancellation events, not one.** `o_cancelled_ctr` counts every operation a
flush destroyed; `o_killed_res_ctr` counts the subset whose result had already
been *computed*. The difference is the difference between "the pipeline threw
away some work it was doing" and "the consumer was too slow and a finished
result was destroyed at the interface", and lumping them together hides the
second. The standing invariant `accepted == completed + cancelled + o_busy` is
checked on every cycle, so a leak in either direction is caught the cycle it
happens.

**Reset clears control state only.** The state register, the iteration counter
and the four counters. The datapath registers (128-bit accumulator and friends)
are **not** reset: every one is written in full at acceptance before any step
reads it, so no result can depend on a stale value, and resetting 128 bits per
flag is cost that buys nothing — the rule `rtl/common/mosaic_ram.sv` states.

## Decode extension

OP-32 (`7'b0111011`) carries two legal classes, and the decoder now produces
both. **funct7 `0000000`/`0100000`** are the RV64I word arithmetic forms — R-type,
rs1 and rs2 are registers, the shift amount is `rs2[4:0]`, no immediate:

| funct7 | funct3 | instruction | `alu_op` |
| --- | --- | --- | --- |
| 0000000 | 000 | `addw` | ALU_ADDW |
| 0000000 | 001 | `sllw` | ALU_SLLW |
| 0000000 | 101 | `srlw` | ALU_SRLW |
| 0100000 | 000 | `subw` | ALU_SUBW |
| 0100000 | 101 | `sraw` | ALU_SRAW |

**funct7 `0000001`** is the M extension's word forms:

| funct3 | instruction | `md_op` | `md_signed` | `md_w` |
| --- | --- | --- | --- | --- |
| 000 | `mulw` | MD_MUL | 1 | 1 |
| 001/010/011 | — reserved (no `mulhw`/`mulhsuw`/`mulhuw`) | illegal | | |
| 100 | `divw` | MD_DIV | 1 | 1 |
| 101 | `divuw` | MD_DIVU | 0 | 1 |
| 110 | `remw` | MD_REM | 1 | 1 |
| 111 | `remuw` | MD_REMU | 0 | 1 |

Every 64-bit form leaves `md_w` 0. `tb_decoder.cpp` walks all eight funct3 values
under funct7 `0000001`, so the three reserved ones are pinned as reserved beside
the five legal ones; it pins the RV64I word forms positively and names the funct3
patterns each of their funct7 values does *not* define; it pins the funct7 values
outside {`0000000`, `0100000`, `0000001`} as reserved (the card's "OP-32 funct7
not F7_M/F7_BASE/F7_ALT stays illegal"); and it counts the legal encodings over
all 1024 as exactly 5 + 5. `MOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL` rejects the
five RV64I word forms again, and the case catches it by name.

**Third-source cross-check.** The encodings were confirmed against the GNU
toolchain as well, so a shared misreading of the manual would have to be shared
with binutils too:

```
$ riscv64-elf-as -march=rv64im -o mdcheck.o mdcheck.s ; riscv64-elf-objdump -d -M no-aliases mdcheck.o
 003100bb addw   403100bb subw   003110bb sllw   003150bb srlw   403150bb sraw
 023100bb mulw   023140bb divw   023150bb divuw  023160bb remw   023170bb remuw
$ objdump -D -b binary -m riscv:rv64 /tmp/raw.bin     # 023110bb 023120bb 023130bb
 023110bb .insn  023120bb .insn  023130bb .insn
```

All ten legal encodings and the three reserved funct3 values match the decoder
exactly, and the ten legal words are asserted one by one in the case.

## What the case checks, and what it counts

Two independent references, one per half of the contract:

* the **arithmetic** is the host's own `__int128` signed and unsigned
  arithmetic. The RTL multiplies with a shift-add loop and divides with a
  restoring loop; the reference does neither, so agreement is evidence about the
  ISA rather than a restatement of the algorithm under test.
* the **control** is a small C++ state machine written from the header prose of
  `rtl/core/mosaic_muldiv.sv`. It shares no code with the RTL.

| Phase | Cycles | What it pins |
| --- | --- | --- |
| reset-state | 5 | the cold state: ready, not busy, `o_iter` 0, all counters 0 |
| directed-edges | ~59 000 | 1 238 operations: all 8 ops × {64-bit, W} over 8×8 curated vectors, 25 W vectors with garbage in bits 63:32, and 14 named corners (`div`/`divu`/`rem`/`remu` by zero, `MIN/−1` for div and rem, W `MIN32/−1`, W divide-by-zero, `mulhsu` both sign orders, `mulhu` all-ones, `mulw` MAX×MAX, `divuw`/`remuw` of `0xffffffff/N`) |
| latency-equal | ~4 800 | equal latency across operand values, and `width + 1` cycles |
| cancel-sweep | ~8 200 | cancel at **every** iteration position: k = 0..64 for a 64-bit operation, k = 0..32 for a W one (98 cancellations, of which 2 destroy a computed result); after each flush a new operation with a different identity must complete correctly, and the cancelled identity must never reappear |
| backpressure | ~230 | 100 stalled cycles with reloaded request inputs; value and identity stable; then accepted, and a following operation still completes (no lost operation, no credit leak); then the same hold killed by a flush |
| reset-cancel | ~150 | reset during the iterations, and reset while a *completed* result waits — both must leave the unit idle, ready and with the counters cleared |
| random | 4 000 | random ops, widths, operands (W with garbage upper halves), flush timing and back-pressure; shadow-compared every cycle |

Totals: 76 192 shadow comparisons over 76 228 cycles, **1 155 304 named checks,
0 failures**. The standing invariants are re-checked on every cycle: ready only
when idle and unflushed; `res_valid_o` only after an operation completed;
identity always the accepted request's; `accepted == completed + cancelled +
o_busy`; `killed <= cancelled`; nothing offered in a flush cycle.

## Mutants

Each was rebuilt with the define appended to the runner's flags and run through
the binary directly, and the column that carries the evidence is the **delta**
against the base, which is green (0 failures):

| Mutant | Define | exit | checks | failures | delta | first failure |
| --- | --- | --- | --- | --- | --- | --- |
| flush ignored | `MOSAIC_MULDIV_MUTANT_FLUSH_IGNORED` | 1 | 963 936 | 10 | **+10** | `cancel-sweep … flush=1 … o_busy=1 expected 0` |
| result dropped when `res_ready_i` low | `MOSAIC_MULDIV_MUTANT_DROP_UNREADY` | 1 | 1 091 459 | 6 050 | **+6 050** | `cancel-sweep … at iteration 64 … o_busy=0 expected 1` |
| delivered result never released | `MOSAIC_MULDIV_MUTANT_RES_STUCK` | 1 | 1 034 | 11 | **+11** | `directed-edges … o_busy=1 expected 0` (the next request is refused) |
| divide-by-zero quotient 0 | `MOSAIC_MULDIV_MUTANT_DIV0_ZERO` | 1 | 1 155 304 | 130 | **+130** | `directed div 0x0 0x0: result 0x0 expected 0xffffffffffffffff` |
| `md_w` ignored (64-bit op for a W form) | `MOSAIC_MULDIV_MUTANT_IGNORE_W` | 1 | 1 537 212 | 113 726 | **+113 726** | `directed-edges cycle 4270 [directed mulw …] res_valid_o=0 expected 1, o_iter=33 expected 0` |
| `res_data_o` changes while waiting | `MOSAIC_MULDIV_MUTANT_UNSTABLE_RESULT` | 1 | 1 155 304 | 209 | **+209** | `backpressure: the offered value changed while waiting` |

Every `ifdef` has a body that changes behaviour in the shipping build; none of
the six compiles the unmodified design. The base is green, so the deltas are
meaningful in the strong sense (an exit code against a red base would not be).

The decoder's own negative control after the OP-32 correction:

| Mutant | exit | failures | delta vs base | first mismatch |
| --- | --- | --- | --- | --- |
| `MOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL` | 1 | 2 | **+2** | `insn=0x003100bb.valid: expected 0x1, got 0x0` |

(`0x003100bb` is `addw`.) This mutant was added by the correction below: it
rejects the five RV64I word forms, which is exactly the shipped behaviour the
case now catches by name. The `MOSAIC_DECODER_MUTANT_RV32_WORD_LEGAL` hook it
replaces was deleted — it mutated the shipping behaviour into what the corrected
shipping behaviour now is, which made it vacuous.

## Discrepancy found and fixed: OP-32's I word forms

**Status: resolved.** The integration lead ruled the defect real and put the fix
in this card's scope; the correction is in `mosaic_decoder.sv`, `mosaic_pkg.sv`
and `tb_decoder.cpp`, and
[results/reports/I-010-011-decode-alu.md](I-010-011-decode-alu.md) carries an
appended correction section (its body is unchanged).

`addw`, `subw`, `sllw`, `srlw`, `sraw` are **RV64I** instructions on OP-32
(`0111011`) with funct7 `0000000`/`0100000`. The three independent sources agree:

1. the ISA manual (RV64I adds ADDW/SUBW/SLLW/SRLW/SRAW on OP-32; RV32I defines
   no OP-32 at all);
2. `riscv64-elf-as -march=rv64im` assembles all five and objdump disassembles
   them by name (encodings above);
3. the project's own reference path, `rtl/core/mosaic_bringup_core.sv`, decodes
   all five as legal `ALU_*W` operations, and `sim/unit/tb_bringup.cpp` — an
   independent model — does the same.

The previous shipping decoder rejected all five as reserved, on the stated (and
backwards) belief that the W forms are RV32 forms. The decode is now corrected,
the reserved classes are re-derived properly (the funct3 patterns each I-word
funct7 does *not* define, the high-half M funct3 values, and every other funct7),
and the vacuous `RV32_WORD_LEGAL` mutant — which mutated the shipping behaviour
into what is now correctly the shipping behaviour — was deleted and replaced by
`MOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL`, which restores the defect. That mutant
is the thing that keeps this from coming back.

```
python3 tools/run_unit.py --profile p0 --case decode.rv64im_reserved
  RESULT PASS decode.rv64im_reserved  166733 instructions (82384 legal, 84349 illegal),
  0 mismatches, 1443 named reserved checks              (checks 169534, failures 0)

-DMOSAIC_DECODER_MUTANT_W_FORMS_ILLEGAL
  exit 1, 2 failures, delta +2 against the green base
  first mismatch: insn=0x003100bb.valid: expected 0x1, got 0x0        (that word is addw)

python3 tools/run_unit.py --profile p0 --case core.bringup_vs_reference
  RESULT PASS corpus 39 programs, 2774346 architectural events, every stream
  identical to the independent reference                              (no second defect)
```

The bring-up case executes through a decoder and an ALU, so it is the check for a
routing hole (`uses_alu`/`is_muldiv`/`uses_imm`). It stays green: **no second
defect in the same package was found**.

## Lint

Both readers, on the final sources (scoped to the files this card owns; the
project-wide gate is the integration lead's to run over the whole tree):

```
$ verilator --lint-only -Wall -Wno-DECLFILENAME --top-module mosaic_muldiv \
    -Ibuild/p0/rtl -Irtl/common -Irtl/core rtl/core/mosaic_pkg.sv rtl/core/mosaic_muldiv.sv
  (no diagnostics)                                            exit 0

$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl rtl/core/mosaic_muldiv.sv
  6 x [STYLE-2] port 'o_busy'/'o_iter'/'o_*_ctr' is not correctly suffixed with "_o"
                                                              exit 0

$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -Ibuild/p0/sim -Isim/common \
    -I$(verilator --getenv VERILATOR_ROOT)/include \
    -Ibuild/p0/unit/muldiv.kill_and_edges/obj_dir sim/unit/tb_muldiv.cpp
  (no warnings from any file this card wrote)                 exit 0
```

The six `slang-tidy` warnings are the frozen interface's names: the project
convention is that testbench-visible status outputs are called `o_*`, and the
frozen port list mandates exactly these six. They are style warnings, not errors,
and `make lint-slang` (which requires exit 0) is satisfied. The only warnings
from `lint-cpp`'s command line come out of Verilator's own runtime headers, which
the Makefile documents as not `-Wextra` clean.

## What is NOT verified

* **No integration.** The unit is not instantiated by any top-level in this
  case; nothing exercises it against a real ROB/IQ consumer, a late writeback
  path, or the result credit that will carry the identity. Only the boundary
  contract (accept/hold/flush/counters) is verified.
* **One profile.** p0 only (identity widths 6/7/3). The RTL derives them from the
  generated package, and the driver reads them back from the DUT, so a different
  profile should size itself — but no second profile was run.
* **The three unreachable W high-half combinations** (`mulh`/`mulhsu`/`mulhu`
  with `md_w`) are verified against *my* definition of "the same operation at
  the working width". The ISA defines no such instruction, so there is no
  external oracle for them; they are unit-level coverage of a total function,
  not evidence about an instruction.
* **Zkt / constant-time.** The design has no value-dependent latency, which the
  latency phase checks, but no formal or statistical constant-time property is
  claimed; RVA23 Zkt is a later package.
* **Throughput.** One operation in flight is the contract, so there is no
  back-to-back acceptance test; the case checks only that a new request is
  accepted on the cycle after a delivery or a flush.
* **The decoder's other classes** were not re-verified beyond re-running
  `decode.rv64im_reserved`, which passes with its own reference decoder and its
  1 443 named reserved checks. The ten legal OP-32 encodings are now additionally
  cross-checked against binutils word by word, and the I-010 correction above
  went with it; the rest of the decoder still rests on I-010's own objdump
  cross-check.
* **No core built from the shared decoder exists yet**, so the five RV64I word
  forms are verified at the decoder boundary (fields, `alu_op`, `uses_alu`) and
  through the bring-up case's *own* decoder and ALU — not end-to-end through the
  shared decoder into a real ALU instance.
* **No formal property checking**, no synthesis run of this module (I-012 does
  not own a synthesis target).

## Defects found in this round, and which side was wrong

1. **The DUT was wrong about `o_iter` in IDLE.** The first run failed on the
   shadow comparing `o_iter=64 expected 0` after a result had been taken. The
   contract did not say what `o_iter` is when idle; the useful answer for a
   testbench that cancels "at every position" is 0, so the *RTL* was changed to
   clear the iteration counter when an operation leaves, and the header now
   states it. The alternative — teaching the shadow to expect a stale 64 — would
   have pinned a value that means "the last operation, which is gone".
2. **A live-input leak into the held result, caught before the first run.** The
   divide-by-zero quotient was originally selected with `acc_div_zero ||
   div_zero_r`, where `acc_div_zero` is combinational in the *current* request
   inputs. In DONE that would have made the offered value change whenever the
   requester reloaded `req_b_i` — the exact defect
   `MOSAIC_MULDIV_MUTANT_UNSTABLE_RESULT` injects. Caught by re-reading the
   result datapath against the "unchanged while waiting" requirement, fixed to
   the latched flag only, and the back-pressure phase now exercises the leak
   directly.
3. **A test defect, twice.** Two waits were written with a default stimulus
   (`res_ready = 0`) after asserting the result was offered, so the result was
   never taken and the *next* operation was refused. The failure read like a DUT
   bug ("request refused while idle") and was the driver's. Fixed by taking the
   result in the same stimulus.

No check was weakened, no phase shortened, and no seed tuned to reach green in
any of this.
