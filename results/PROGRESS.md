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

**Not yet recorded as agreeing:** on a clean rebuild the Spike cross-check
reports **four** disagreements, not three, and `p13_romstore` disagrees on all
three inputs by exactly one in the low byte (`...107` against `...106`). The
package report claims all three of its inputs agree. That claim is contradicted
by the tool's own output and is being resolved before anything is recorded
either way. A report that says "36 of 39" when the tool says 37 is worse than no
number, so the discrepancy is being carried as an open item rather than smoothed
away.
