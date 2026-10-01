---

## 2026-09-30 — I-011 corrected, and why the correction is on record

`mosaic_branch_target` now takes `is_branch`, `is_jal`, `is_jalr` and
`branch_taken`, and computes

```
is_taken = is_jal | is_jalr | (is_branch & branch_taken)
```

with `funct3` appearing only inside `mosaic_branch_cmp`, which is the one unit
that needs to know the encoding exists. `CASE=alu.boundaries` passes with 108560
ALU and 49536 branch vectors, 316022 checks, zero failures; all nine mutants still
fail.

The property that makes this worth having is tested rather than asserted: a
caller that drives `branch_taken` high while naming no branch gets
`is_taken = 0`, and the testbench both checks that invariant and asserts the
stimulus actually reached the case. A design that fails loudly is only useful if
something is watching for the failure.

**What happened here, recorded because the process matters more than the
outcome.** My first port list omitted `is_jal`, which made an unconditional `JAL`
indistinguishable from a not-taken branch. The agent solved that by treating
funct3 `010`/`011` — two reserved encodings no RV64I branch uses — as "names no
condition, therefore taken". I rejected it. It is harmless today and silently
wrong the day an instruction collides with the pattern, and the failure would
land only on instructions nobody was looking at. The honest description is that
**my port list was under-specified, not that the agent implemented it wrongly**,
and that is what the report says.
---

## 2026-09-30 — I-007 closed — the p08 root cause was self-inflicted

The `.traplog` section was placed at `0x80001000` — the address **I** moved
TOHOST to earlier in the same session. Every trapping program wrote its first
`mcause` straight into the result word, so the harness latched `mcause=4` as the
outcome and each run self-reported FAIL. The "endless re-entry" and "control
falling back into crt0" symptoms were both consequences of that collision.

Found by `BringupCore`, working on a different package, and reported rather than
worked around locally. Fixed by moving `.traplog` to `0x80002000`, and — more
valuably — by adding three linker `ASSERT`s that make any overlap between
`.traplog`, `.bss` or `.scratch` and the `0x80001000` TOHOST/FROMHOST window a
link error. The bug cannot recur silently, which matters more than the fix.

This is also on the record as **my** error: the collision existed only because I
moved the test protocol into RAM to make Spike able to poll TOHOST at all. The
decision was right and the consequence was not checked.

The store-to-`boot_rom` case was split out as `p13_romstore` so the reference
that is actually available can adjudicate it. Corpus is now 13 programs × 3
inputs = 39 ELFs, 8649 instructions, all within `rv64im_zicsr_zifencei`.

**Corrected final status: 33 of 39 reference-validated, two assertions marked
`NOT_CLAIMED` with stated reasons.**

The package report had claimed "36 of 39 agree". That number came from a `grep`
truncated with `head`, not from the tool's own count; the tool says **33 of 39**
and six cases disagree. The false figure was caught here and corrected rather
than carried forward.

In **every** failing case `sig0`, `sig1` and `sig2` agree with Spike exactly.
Only the trap-fold word differs, which is a precise statement of what is and is
not validated:

| claim | status | reason |
|---|---|---|
| arithmetic, branches, loads/stores, aligned-neighbour behaviour | validated, 33/39 | agrees with Spike word for word |
| `p08` misaligned-access traps | `NOT_CLAIMED` | Spike services misaligned accesses natively and exposes no option to trap |
| `p13` store to non-writable `boot_rom` | `NOT_CLAIMED` | Spike reports cause 6 where the specification requires cause 7, and the harness models `boot_rom` as unmapped rather than read-only because Spike has no read-only region type |

The oracle was **not** edited to match Spike in either case. Cause 7 is
architecturally correct for a store to a non-writable region, and writing 6 into
the expectation to turn a tool green is exactly the drift this package exists to
prevent.

Closing both would need a reference that implements the frozen misalignment
policy *and* models a non-writable region. Neither is configurable in this
Spike build.

---

## 2026-09-30 — I-004 regression, caught and fixed at the root

Moving the test protocol into RAM to make Spike able to poll TOHOST broke my own
harness, which is exactly what a shared constant invites. Four separate defects,
each found by running rather than reading:

1. **The micro-program hardcoded `0x00102000`.** It is now *derived* from the
   generated platform header: the address is split into a `lui` and, where the
   low bits need it, an `addi`, all encoded in C++ from `MOSAIC_SIGNATURE_ADDR`
   and `MOSAIC_TOHOST`. A constant that was correct once cannot go stale again.
2. **`EncSd` put `rs2` in the `rd` field.** The result was `sd x0, 0(t1)` — a
   valid-looking encoding that stores zero. Checked against
   `riscv64-elf-objdump -d -M no-aliases`, which reports `00533023` for
   `sd t0, 0(t1)` and the corrected encoder produces the same.
3. **A sampling race in the testbench.** `rd_value_out` is combinational from the
   register file, so reading it after the clock edge samples the register the
   write *just produced* — one instruction ahead. The wrong value still looked
   like plausible register content, which is what made it hard to see. It is now
   sampled in the pre-edge phase, where the source operands still hold their
   original values.
4. **The memory model only recognised TOHOST inside the device region.** Now that
   the protocol is an ordinary memory word, it is handled before the region-kind
   switch. Reads and writes to the now-undefined `test_harness` device region
   raise an access fault rather than returning zero, so firmware written against
   the old placement fails loudly instead of appearing to work.

All four harness cases pass again, and `make lint-cpp` is clean across ten C++
files.

**The lesson worth keeping**: three of these four are the same mistake wearing
different clothes — a value that was correct when written and had no mechanism to
notice when the thing it depended on changed. The fix was never "fix the
constant"; it was to derive the value, assert the encoding against the
assembler, and sample at the point where the signal means what the comment says.

---

## 2026-09-30 — I-008 — INCOMPLETE, diagnosed to two defects, handed on

The bring-up core exists, lints clean, and produces a retire stream **byte-identical**
to the independent Python reference on every program that does not trap.
`p01_addsub.i0` matches line for line across 71068 events. The package is **not**
delivered, and the report says so.

Two distinct defects remain, both localised by diffing the two event streams rather
than by reading the RTL:

**1. A `csrrw` whose `rd` and `rs1` are the same register.** The first differing
line is `pc=0x80000250 insn=34011173`, which is `csrrw sp, mscratch, sp` — it appears
at both ends of the firmware trap handler. The *read* is correct (the DUT returns the
old `mscratch`, `0x80006480`); the *written* value is not, leaving `mscratch` holding
what it would hold if no `csrrw` ever wrote it. By inspection `csr_operand = rs1_val`,
`rs1_val` is a pre-edge register read, and the write sits inside `if (csr_writes)` inside
`if (commit_now)` with an unconditional `mscratch_q <= mscratch_n`. I could not find the
defect by reading, which usually means a wrong assumption rather than obviously wrong
code — so the discriminator is an out-of-band `ReadCsr(0x340)` after the first trap's
entry, which separates "the write never lands" from "it lands with the wrong operand".

**2. A missing store/AMO access fault.** Trap counts are `4,4,6,6,7` in the reference
and `4,6` in the DUT. The missing cause-7 is the store to `boot_rom` at address 0, which
`config/memory/p0.json` declares `writable: false`. The core only observes
`dmem_fault_i`, so whether this is a core bug or a missing PMA model in
`sim/tb/mosaic_bringup_tb.sv` has to be established, not assumed. I hit exactly this
class of bug in my own harness when a region moved: the model silently accepted writes it
should have refused.

Also unresolved and worth naming: the DUT traps an odd-address `lh` and an odd-address
`sh` correctly but not a `lw` at address ≡2 (mod 4), while the alignment check,
`size_bytes` and the decoder all read correctly. That combination points at something
subtle rather than at an obvious fault, and it is the reason the next agent is told to
instrument rather than re-read.

**Correction I owe the previous agent:** I read `34011173` as `csrr x2, mscratch` in my
first message. It is `csrrw sp, mscratch, sp`; funct3 is `001`, not `000`. The agent
corrected me and was right.

**Corrections I owe myself**, all of the same shape: a cycle budget that was too small,
and three separate cases where I named a specific standard-mandated construct as the
fault and was wrong — the lint tool's file ordering, once in the linter and once in the
test runner. Each read as a plausible defect in someone else's code. The general
lesson, which is going into the next agent's card: *a diagnostic that names a real
construct and points at a plausible line can be completely fictional, and the more
specific it sounds the more it deserves to be checked against the tool's raw output.*

The five existing mutants have still never been run, so they remain an unmet
acceptance criterion. I-008 stays open.
