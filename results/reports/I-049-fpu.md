# I-049 — the F/D implementation decision, `mosaic_fpu`, and CASE=fp.operation_matrix

Status: **delivered as a unit**. The registered case passes from a clean build and
four injected defects each fail it with a named first mismatch. The machine's
advertised ISA is unchanged: nothing here decodes, renames, commits or advertises
F/D. This package delivers the *datapath* behind a tagged handshake; the decode,
the FP register file, `fcsr` and precise-commit `fflags` are I-050's and
V-050's, and the capability set is closed by I-082.

Everything below is measured, not hoped for. Commands are quoted where they were
run, and every number in the tables came out of the run in this session.

---

## 1. The decision, and its licence reasoning

**The F/D implementation is self-implemented in SystemVerilog. No third-party
FPU is vendored, and nothing was copied in.**

The reasons, in the order they decided it:

1. **Licence.** This workstation has no network access, so no candidate FPU
   could be fetched to vendor it. The project may not take material whose
   licence has not been reviewed, and a licence cannot be reviewed for source
   that is not present. The one widely used reference implementation in this
   space, Berkeley SoftFloat, is BSD-3-Clause (reviewable in principle) but is a
   **C++ software library**: it is an oracle, never a datapath, and the card
   forbids software as a substitute for a hardware datapath. A vendored RTL FPU
   could not have its provenance audited here. Having no candidate whose licence
   could be reviewed *and* whose semantics could be checked against the
   specification in this environment, the honest answer is to write the
   datapath here.
2. **Semantics.** The behaviour that matters for this project is the RISC-V
   rounding, NaN, subnormal and `fflags` contract, and the case exists to test
   it. A vendored core would have to be accepted as-is (a semantic import nobody
   reviewed) or modified (at which point it is ours anyway, with the licence
   question still open).
3. **Boundary.** Writing it here makes the boundary explicit and checkable: the
   operations that are implemented are listed and tested; the ones that are not
   are *declared absent* in the RTL header, in §3 below and in the capability
   matrix, and a request for one returns a named marker rather than a plausible
   number. There is no software fallback anywhere in the unit — see §7, which
   says how that is verified: a fallback would need somewhere to live, and this
   module has no memory interface, no instruction store and no software-visible
   state at all.

What the decision costs is stated plainly rather than hidden: **`fsqrt.s/d` is
not implemented**. The card accepts `fdiv.s/d` as the alternative and `fdiv` is
implemented, is exercised in every rounding mode, and is checked against the
host FPU. `fsqrt` remains a declared-absent line until I-082/I-093 take it.

---

## 2. What was built

| Artifact | Path | Lines |
|---|---|---|
| The datapath | `rtl/core/mosaic_fpu.sv` | 1560 |
| The simulation wrapper | `sim/tb/mosaic_fpu_tb.sv` | 136 |
| The case driver and oracles | `sim/unit/tb_fpu.cpp` | 1789 |
| The mutant runner | `tools/run_fpu_controls.py` | 235 |
| The added package enumeration | `rtl/core/mosaic_pkg.sv` (additive: `fp_op_e`, `fp_rm_e`) | +50 |

`mosaic_fpu` is one module behind a tagged handshake:

* **request** — `req_valid_i`/`req_ready_o`, `req_op_i` (`mosaic_pkg::fp_op_e`),
  `req_fmt_i` (1 = single, 0 = double), `req_rm_i` (resolved rounding mode),
  `req_iw_i`/`req_is_i` (the integer side's width and signedness for the
  conversions), `req_a_i`/`req_b_i`, and the project identity
  (`rob_index`, `rob_gen`, `uop_index`);
* **response** — `res_valid_o`/`res_ready_i`, `res_data_o`, `res_fflags_o`
  (`{NV,DZ,OF,UF,NX}`, the CSR bit order), and the **same identity** back, so the
  completion fabric can consume it exactly as it consumes an ALU or MUL/DIV
  result and a late result is attributable;
* **status** — `o_busy`, `o_latency_o` (the declared latency of the operation in
  flight), `o_iter`, and `accepted`/`completed`/`cancelled`/`killed-results`
  counters that conserve.

The unit is single-issue with one operation in flight. A rejected request is
refused, never accepted and thrown away: `req_ready_o` is high only in IDLE with
no flush in that cycle.

---

## 3. The operation capability matrix

Implemented means "has a hardware path in `mosaic_fpu.sv`, is exercised by the
case, and its result and flags are compared against an independent oracle".
Declared absent means "named here so that a reader cannot infer it from
silence", and a request for it returns the canonical quiet NaN with NV rather
than a number.

| Operation | Formats | Status | Notes |
|---|---|---|---|
| `fadd` | S, D | implemented | alignment with guard/round/sticky; one rounding |
| `fsub` | S, D | implemented | same path; the subtract-direction sticky fix is §6 |
| `fmul` | S, D | implemented | 128-bit exact product, one rounding |
| `fdiv` | S, D | implemented | 65-step restoring division, quotient truncated + sticky, one rounding |
| `fsqrt` | S, D | **declared absent** | returns canonical qNaN + NV; no plausible number is produced |
| `fmadd`/`fmsub`/`fnmadd`/`fnmsub` | S, D | **declared absent** | not on this card's list either; named so the list is not assumed complete by omission |
| `fsgnj`, `fsgnjn`, `fsgnjx` | S, D | implemented | bit sign manipulation; no flags; NaN payload passes through |
| `fmin`, `fmax` | S, D | implemented | NaN operand returns the other operand; ±0 rules per the ISA |
| `feq`, `flt`, `fle` | S, D | implemented | feq is quiet; flt/fle signal on any NaN |
| `fclass` | S, D | implemented | all ten classes |
| `fmv.x.w`, `fmv.w.x`, `fmv.x.d`, `fmv.d.x` | S/D | implemented | `fmv.x.w` sign-extends bit 31 |
| `fcvt.w.s`, `fcvt.wu.s`, `fcvt.l.s`, `fcvt.lu.s`, and the `.d` forms | S, D | implemented | RISC-V saturation on overflow/NaN with NV; NX when inexact in range |
| `fcvt.s.w`, `fcvt.s.wu`, `fcvt.s.l`, `fcvt.s.lu`, and the `.d` forms | S, D | implemented | host conversion instruction as the oracle |
| `fcvt.s.d`, `fcvt.d.s` | S↔D | implemented | rounds / exact respectively |

The enumeration in `mosaic_pkg.sv` (`fp_op_e`, 20 values, plus `fp_rm_e`) is the
F/D list the decoder will consume when I-050 wires it. It is **additive**: no
existing encoding changed, the decoder still rejects every encoding outside
RV64IM (CASE=decode.rv64im_reserved is unaffected), and `misa` is untouched, so
nothing in the advertised capability set moved.

---

## 4. Rounding and NaN policy, and the `fflags` boundary

**Rounding modes.** All five, in the instruction's own encoding, in every
arithmetic path (one rounding per operation, never a double rounding):

| `rm` | Mode | Evidence |
|---|---|---|
| 000 | RNE (ties to even) | host oracle |
| 001 | RTZ | host oracle |
| 010 | RDN | host oracle |
| 011 | RUP | host oracle |
| 100 | RMM (ties to maximum magnitude) | directed tie table, expectations derived from the host (§5) |
| 101/110 | reserved | treated as RNE, stated and tested (`misc.rm-reserved.*`) |
| 111 | dynamic | **not resolved here**: resolving `frm`/`fcsr` is I-050's; the unit receives a resolved mode and treats 111 as RNE, which the case checks rather than leaving undefined |

**NaN policy.** One canonical quiet NaN, `0x7FC0_0000` / `0x7FF8_0000_0000_0000`.
No payload is propagated into a NaN *result* (the ISA allows either and requires
consistency); a signalling-NaN operand sets NV on every operation that examines
its operands' NaN-ness; `fsgnj*`, `fmv.*` and `fclass` never set NV for a NaN
input, and `fsgnj`/`fmv` copy the bits through unchanged. `feq` sets NV only for
an sNaN (it is the quiet comparison); `flt`/`fle` set NV for any NaN (they are
the signalling comparisons). `fmin`/`fmax` return the non-NaN operand and set NV
only for an sNaN operand.

**Subnormals.** Gradual underflow, with no flush-to-zero on any path and no
flush flag anywhere in the unit: operands are unpacked into a normalised
(exponent, significand) pair whose leading one is at bit 63, so a subnormal
*operand* is an ordinary small value to the arithmetic, and a subnormal *result*
falls out of the same single rounding step at the subnormal precision. Tininess
is detected **before** rounding (the exact result's own exponent below the
minimum normal exponent). IEEE 754-2019 makes that an implementation choice and
requires only consistency; the host FPU used as the oracle does the same, which
was checked on exactly the boundary vector (`subnormal.d.div.rounds-to-normal`:
the exact midpoint below the smallest normal is delivered as the smallest normal
with UF|NX, not with NX alone).

**`fflags` — and the boundary with I-050.** `res_fflags_o` is the five flags in
CSR bit order (`bit4 NV, bit3 DZ, bit2 OF, bit1 UF, bit0 NX`). **This unit only
*produces* the flags of the operation it has just executed.** Making them
precise — merging speculative flags and committing them only with the
instruction that raised them — is I-050's, and the boundary is structural: the
unit holds no architectural FP state at all (no `f0`-`f31`, no `fcsr`, no
`frm`), so it cannot get that boundary wrong. Flag-by-operation, each checked on
its own operation rather than on a combined signal:

| Operation | NV | DZ | OF | UF | NX |
|---|---|---|---|---|---|
| fadd/fsub | inf−inf, sNaN | — | yes | yes | yes |
| fmul | 0×inf, sNaN | — | yes | yes | yes |
| fdiv | 0/0, inf/inf, sNaN | finite nonzero / 0 | yes | yes | yes |
| fmin/fmax | sNaN only | — | — | — | — |
| feq | sNaN only | — | — | — | — |
| flt/fle | any NaN | — | — | — | — |
| fclass, fsgnj*, fmv* | — | — | — | — | — |
| fcvt int→fp | — | — | — | — | inexact |
| fcvt fp→int | NaN or out of range (saturating, no NX alongside) | — | — | — | inexact in range |

Overflow always sets OF and NX together, and the delivered value follows the
mode (infinity for RNE/RMM; the largest finite for RZ; the mode-directed
infinity for RUP/RDN). `inf/0` is an infinity with **no** DZ, as IEEE states and
as the host was checked to do.

---

## 5. The oracle: what computes the expectation

The DUT is never its own oracle. Each operation class names its:

* **Arithmetic** (`fadd`/`fsub`/`fmul`/`fdiv`, both formats, every mode the host
  supports) — the **host's own floating-point unit**, through C's `float`/
  `double` operators with `fesetround`, with the exception flags from
  `fetestexcept` mapped to `{NV,DZ,OF,UF,NX}`. That is a second, independent
  IEEE 754 implementation on this machine (the ARM FPU), not a model written
  here. Operands are `volatile` so no operation is folded away. NaN results are
  compared by *policy* (the unit must return the canonical NaN) rather than by
  bit equality with the host's payload, which is not part of the contract.
* **Conversions** — `int→fp` uses the host's own conversion instruction under
  the current mode; `fp→int` has no C equivalent for the saturation rule (C's
  cast is undefined out of range), so it uses a model written here from the
  specification, and **its in-range signed answers are cross-checked against the
  host's `llrint`/`lrint`**, which round identically and report NX the same way.
  Both checks are in the run.
* **Comparisons, `fmin`/`fmax`, `fclass`, `fsgnj*`, `fmv*`** — bit-level
  predicates over the operand encodings written here from the ISA; there is no
  arithmetic in them to have a second implementation of.
* **RMM, the one mode this host cannot do**: macOS's `<fenv.h>` does not define
  `FE_TONEARESTFROMZERO` (checked, not assumed), so there is no host RMM. RMM is
  measured on a directed tie table whose expectation is **derived from the
  host**: at an exact tie, ties-to-maximum-magnitude is the away-from-zero
  neighbour, which is the host's RUP result for a positive result and its RDN
  result for a negative one. The driver additionally **proves each vector is a
  genuine tie with exact dyadic arithmetic** (`Dya*` in the driver — no rounding
  anywhere, sharing no code with either the RTL or the host): the exact result
  must equal the midpoint of the two neighbours the host produces at RDN and RUP.
  A run that cannot show a tie is a failure, not a skip.
  The same phase checks the complementary fact that **division can never produce
  an exact tie** in a binary format: a tie needs an odd factor of 2^p in the
  divisor's significand, and a normal significand in [2^(p−1), 2^p) has at most
  p−1 factors of two. 400 random divisions are required to fail the tie test.

The RTL is not consulted for any expectation, and no expectation in the driver
was produced by running the RTL.

---

## 6. Latency, backpressure, and what the case checks

**Latency contract**, fixed per operation class and independent of operand
*values* (the property the RVA23 Zkt work will need, and the reason division
always runs its full iteration count):

| Class | Declared | Measured |
|---|---|---|
| every operation except `fdiv` | 1 cycle | 1 cycle (`lat.*` phase and every issue) |
| `fdiv.s/d` | 66 cycles (65 restoring steps + pack) | 66 cycles, including for 0/2, 3/3, a subnormal divisor and a divide-by-zero special case |

"Latency" is the number of cycles from the cycle in which
`req_valid_i && req_ready_o` accepts to the cycle `res_valid_o` is first high.
`o_latency_o` states the declaration for the operation in flight, and the driver
compares the measurement against that declaration rather than against a literal,
so a latency change cannot pass unnoticed.

**Backpressure.** A result that is not taken keeps its value, its five flag bits
and its identity for as long as the consumer stalls: the case holds a division's
result for the remaining 34 of 100 stalled cycles (100 − 66) and compares value,
flags and tag on every one of them, then takes it and proves the unit accepts
again. **Flush** cancels an operation in flight and a *completed* result waiting
to be taken (counted separately, as MUL/DIV does), masks `res_valid_o` in the
flush cycle, and leaves the unit ready; the case checks both, and that the
cancelled operation's identity never appears later.

The conservation invariant `accepted == completed + cancelled + in-flight` is
checked on every completed operation from the DUT's own counters, and once more
at the end of the campaign.

**Phases and volume, seed 1:** 19904 checks, 0 failures, 4735 operations
(4735 accepted, 4733 completed, 2 cancelled) over 56524 cycles. Seeds 7, 12345
and 99991 were run with the same result (54639, 56459, 53209 cycles).

```
$ python3 tools/run_unit.py --profile p0 --case fp.operation_matrix
PASS fp.operation_matrix          task=I-049
RESULT PASS fp.operation_matrix operation matrix holds: 4735 operations
(4735 accepted, 4733 completed, 2 cancelled), 56524 cycles, seed 1
```

---

## 7. What is not covered, and what is declared rather than faked

* **`fsqrt.s/d` is not implemented.** It is a declared-absent line in §3 and in
  the RTL header. A request for it (or for any unnamed operation) returns the
  canonical quiet NaN with NV — a marker a later integration can trap on — and
  the case checks that it does so rather than returning a number. No part of
  `fsqrt` is approximated.
* **`fmadd`/`fmsub`/`fnmadd`/`fnmsub` are not implemented** and are named so
  that the operation list is not assumed complete by omission.
* **Partial coverage of the double-precision subnormal space.** The case reaches
  subnormals from both directions (subnormal operands and subnormal results),
  the boundary between subnormal and normal, the smallest subnormal, the largest
  subnormal, underflow to zero with and without rounding back up, and the
  before-rounding tininess boundary. It does not enumerate all 2^52 double
  subnormals; the random soak covers a spread. This is coverage of the *rules*,
  not of the space.
* **Rounding-mode coverage is asymmetric.** Four modes have a host oracle on
  every operation; RMM has a directed tie table plus the property that RMM
  agrees with RNE whenever the result is exact. There is no RMM host oracle on
  this platform and the report does not pretend otherwise.
* **The comparison/`fclass` model and the `fp→int` model are written here**, not
  by an independent implementation; the `fp→int` model is cross-checked against
  the host's `llrint` in range, and the comparison predicates are three-line
  orderings with no arithmetic to disagree about. This is the weakest link in
  the oracle story and is stated as such.
* **No cycle-level formal proof, no synthesis/PnR evidence, no claim about
  area or Fmax.** The unit is written to the project's RTL rules (no latches, no
  interfaces, no assignment patterns, explicit widths) and passes Verilator
  `--lint-only -Wall` and slang-tidy, which is what this card's gates ask for.
  H4/H-series claims are not made.
* **No integration evidence.** Nothing here proves that the core can decode,
  rename or commit an FP instruction: it cannot yet, by design (I-050/V-050).
  The case drives the unit through its own wrapper, which is the boundary the
  card defines.
* **`fdiv` accuracy is declared, not faked**: it is correctly rounded in all
  five modes and checked against the host on every vector class above
  (including subnormal operands and results, division by zero, 0/0 and
  inf/inf); the restoring loop's quotient is truncated with a sticky bit and
  rounded exactly once. There is no "approximately correct" path.

**No RTL in this project falls back to software, and here is how that is
verified.** It is not verified by grepping for a forbidden word: it is
structural. `mosaic_fpu` has no memory interface, no instruction store, no
program counter, no way to transfer control, and no architectural FP state; its
entire state is the identity, control and datapath registers listed in the file,
and its entire input is one request's operands and control bits. There is
nowhere for a software path to live, and a request for a declared-absent
operation returns the marker of §7 rather than an answer computed any other way.
The case's behavioural half of the same claim is the `unimplemented` phase: the
unit must answer "not implemented", never a plausible number.

---

## 8. Mutants

House rule, applied by `tools/run_fpu_controls.py`: the shipping build is built
and run first **from an empty build directory**; then each mutant is rebuilt
from its own empty directory with its `-D` on the recorded command line, must
produce a binary that differs from the shipping one, must exit 1, and must name
the check it breaks as the *first* mismatch.

Shipping binary sha256 `efa239081c6b81b30f2091e9c369825a835a42a392f8d2beed9af61cfbebf08c`.

| Define | Injects | Exit | Binary sha256 (first 16) | First mismatch |
|---|---|---|---|---|
| `MOSAIC_FPU_MUTANT_RM_TIE_AWAY` | RNE rounds an exact tie away from zero | 1 | `ae550462f01778ff` | `ties.d.add.pos.rne.bits: expected 0x3ff0000000000000, got 0x3ff0000000000001` |
| `MOSAIC_FPU_MUTANT_DROP_NX` | NX is never set on an inexact result | 1 | `5cb2459e68d190ae` | `ties.d.add.pos.rne.fflags: expected NX, got -` |
| `MOSAIC_FPU_MUTANT_NONCANONICAL_NAN` | a NaN result carries a payload bit | 1 | `680403f861421298` | `nan.d.add.qnan+qnan.rne.bits: expected 0x7ff8000000000000, got 0x7ff8000000000001` |
| `MOSAIC_FPU_MUTANT_FLUSH_SUBNORMAL` | an inexact subnormal result is delivered as zero | 1 | `d5f2c06c168ca8b3` | `subnormal.d.mul.minsub*minsub.rup.bits: expected 0x0000000000000001, got 0x0000000000000000` |

```
$ python3 tools/run_fpu_controls.py
  baseline exit=0 RESULT PASS fp.operation_matrix ...
all 4 mutants mutate the binary, exit 1 and name the check they break
```

Each define appears in `rtl/core/mosaic_fpu.sv` only under `ifdef`, and the
runner refuses to run if a define it claims to build does not occur in the
source.

---

## 9. Gates

* `python3 tools/lint_rtl.py --profile p0` → **42 source files clean** (41 before
  this package, plus `mosaic_fpu.sv`), and `--self-test` still reports that a
  latching module is rejected.
* `slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name
  '*.sv' | sort)` → **exit 0**. `mosaic_fpu.sv` adds no warning class that the
  existing units do not already produce (the `EnforcePortSuffix` style rule,
  which `mosaic_muldiv.sv` also trips for its `o_*` status ports).
* `python3 tools/check_records.py` → `records agree: 49 delivered package(s), 58
  registered case(s)`. The registry entry for `fp.operation_matrix` is unchanged
  and still marked `"pending": true`; clearing that marker is the integration
  lead's act, not this package's.
* `sim/unit/tb_fpu.cpp` compiles clean under the `lint-cpp` flags
  (`-std=c++17 -fsyntax-only -Wall -Wextra -Wshadow`): **0 warnings attributable
  to the file**.
* `sim/tb/mosaic_fpu_tb.sv` and `rtl/core/mosaic_pkg.sv` are covered by the two
  RTL gates above.

`tools/synth_check.py --profile p0` is **already blocked before it reaches this
file**, by two things that are not this package's: Yosys 0.69 cannot parse the
generated `build/p0/rtl/mosaic_id_pkg.svh` (the `return (…) && (…)` expression
at line 78), and it cannot parse `rtl/core/mosaic_amo_unit.sv` (`::` inside a
cast, line 58). The first is the same limitation that makes the shipped
`rtl/core/mosaic_muldiv.sv` fail an identical scratch run — the check was run
with that file in place of this one and failed on the same line of the same
generated header — and the second is a sibling's file, untouched here. Nothing
in this package makes the synth gate worse, and nothing here presents the lint
and AST gates as synthesis evidence in its place.

---

## 10. Defects this work found, and which side was wrong

Recorded because the same shape will recur in the F/D follow-on work.

1. **Subnormal unpack shifted by the wrong distance** (RTL). A denormal's
   leading one was moved to `63 − (63 − i)` instead of `63 − i`, so every
   subnormal operand was decoded as a different, tiny value. Found by the very
   first directed subnormal vector (`minsub + minsub` returned zero).
2. **The restoring divider's initial remainder violated its own invariant**
   (RTL). `rem < divisor` must hold before the loop; the significands are both
   in [1,2), so the dividend can exceed the divisor. One pre-subtraction at
   accept fixes it and makes the quotient's leading one known up front. Found by
   a decimal-simple vector (`1.0/2.0` was right by luck; `(1+2^-52)/2` was one
   ulp low, with the whole low half of the quotient wrong).
3. **The subtract path's sticky had the wrong sign** (RTL). The shifted-out bits
   of the smaller operand reduce the difference's magnitude, so the truncated
   difference is an *upper* bound of the exact result; the directed modes then
   round the wrong way. The fix subtracts the sticky from the difference,
   restoring the "value = retained + fraction" convention. Found by the random
   soak as one-ulp errors under RUP/RDN/RTZ only.
4. **Unpacked zeros had exponent 0** (RTL), which is inside the range of real
   exponents, so `2^-1074 < +0` compared true. Fixed by giving a zero an
   exponent below every real one, with the ±0 rules kept where they belong (in
   the callers that know the operation).
5. **`fp→int` rounding skipped its increment for very tiny values** (driver
   model), and the `int→fp` leading-one index was inverted (RTL). Both were
   caught because the model is cross-checked against the host's `llrint`: the
   cross-check exists precisely so a model bug cannot hide behind a shared
   mistake.
6. **Three driver harness bugs** (default `res_ready = true` in a stall loop,
   absolute instead of relative counter expectations). Recorded because they
   show the value of the conservation invariant: the counters were right and the
   test was wrong.

In every case the oracle (host FPU, or the exact dyadic tie proof) decided which
side was wrong; the RTL was never taken as the reference.
