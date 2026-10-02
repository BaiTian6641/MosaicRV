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

---

## 2026-09-30 — V-001 and V-003 DONE; V-002, V-004, V-005 BLOCKED

**V-001 — upstream closure and source ledger.** `tools/check_upstream.py` probes
the live environment rather than reading a stored string: source checkouts by HEAD
commit and tree cleanliness, tools by executing them and capturing their own
version output. Everything present carries an immutable identity, a source and a
licence.

```
$ python3 tools/check_upstream.py
source checkouts
  ok      riscv-isa-sim    0bff12123b1f  clean=True  Merge pull request #2450 ...
  ok      riscv-v-spec     2f68ef7256d6  clean=True  Merge pull request #939 ...
tools
  ok      verilator            Verilator 5.052 2026-09-05
  ok      yosys                Yosys 0.69+post
  ok      slang-tidy           slang-tidy version 11.0.0+0
  ok      sby                  SBY v0.69
  ok      riscv64-elf-gcc      riscv64-elf-gcc (GCC) 16.2.0
  ok      riscv64-elf-objdump  GNU objdump (GNU Binutils) 2.47.20260726
absent references, each with what would unblock it
  BLOCKED XiangShan    gate=V-005  clone and build OpenXiangShan/XiangShan (sbt/Chisel)
  BLOCKED NEMU         gate=V-004  clone and build OpenXiangShan/NEMU
  BLOCKED Sail         gate=V-002  install sail-riscv; optional next to Spike for p0
  BLOCKED ACT4         gate=V-002  obtain the ACT4 distribution and licence
PASS every present input has an immutable identity, a source and a licence
```

`--require <name>` makes an absent input a hard failure, which is how a gate that
depends on one refuses to run rather than proceeding with a reference that is not
there.

**V-003 — isolated environment and failure exit.** `tools/check_isolation.py` runs
the three scenarios the plan names, each paired with a **healthy control** so that a
checker which always reported "unsafe" could not pass: a missing simulator, an
unknown profile with no fallback, a foreign-architecture binary, a non-executable
file, and an output path that cannot be a directory. Eleven checks, all holding.

The binary probe understands **both** ELF and Mach-O. My first version assumed ELF
and the healthy control failed — a macOS binary is Mach-O, so an ELF-only probe
rejects every native binary on this platform. That is the third time this session a
checker of mine was wrong in a way that looked like the code under test.

**V-002, V-004 and V-005 are BLOCKED, not done.** The capability-intersection
matrix needs XiangShan, NEMU, Sail and ACT4; the NEMU ABI work needs NEMU; the
XiangShan positive control needs XiangShan. None of them is on this machine. Their
absence is named with the specific thing that would unblock each, and no package
depending on them is marked deferred-and-passed.

**A bookkeeping error I made twice in a row.** I marked the I-008 item complete
while it is demonstrably not, and then marked `V-001..V-005` complete while three
of the five are BLOCKED. Both were caught and reopened in the same turn. The habit
of marking a tracker item "done" because the agent returned is the precise habit
this project exists to break; the tracker is only worth keeping if it can be
*less* optimistic than the person filling it in.

---

## 2026-09-30 — I-008 — delivered, with one mutation control that cannot fail

`python3 tools/run_unit.py --case core.bringup_vs_reference` prints **PASS**, exit 0,
across all 39 corpus ELFs. Verified here, not taken on report: the case passes, all
nine RTL files lint clean, and **39 of 39 event streams are byte-identical** to the
independent reference when diffed directly (`diff` reports no difference on any ELF).
The one stream that differs is `probe`, which is a negative-control fixture and is
*meant* to differ.

### The root cause behind the whole divergence

The SYSTEM funct3 decode table was selected with the OP/OP-IMM `F3_*` names from
`mosaic_pkg`, but the Zicsr funct3 space is offset by one from them. So `csrrw`
(funct3 `001`) matched `F3_SLL` and decoded as **`csrrs`**. Every CSR instruction in
the firmware trap handler therefore did `csr_wdata = csr_rdata | csr_operand`,
which for `csrrw sp, mscratch, sp` is `mscratch | sp` — a **self-write**. `mscratch`
never changed, so the handler's exit `csrrw` restored `sp` to the trap-stack top
instead of the interrupted value, all seventeen caller-saved registers came back
zero, and `main` resumed with a register that made its misaligned word load read
aligned memory at address 0.

That one decode error produced *every* observable symptom at once: the missing
`csrrw` write, the wrong `mscratch`, and the missing cause-7 trap. The missing
cause-7 was **not** a PMA defect — the testbench modelled `boot_rom` correctly
throughout. Fixing the decode fixed all of it, and `p08_misaligned.i0` now produces
exactly the trap trace its own source documents.

The discriminator that settled it was reading `mscratch` out of band immediately
after the first trap's entry: still holding the crt0 value proved the write never
landed at all, rather than landing with a wrong operand. Inspection had already
shown `csr_operand = rs1_val` as a pre-edge register read and the commit path as
correct — so the defect was not where reading pointed.

### Defects fixed in this pass

- **DUT**: the Zicsr funct3 table offset, as above.
- **Reference**: `mret` did not update `mstatus`. Privileged Specification v1.12
  §2.1.6.1 requires `xIE ← xPIE`, `xPIE ← 1`, `xPP ← least-privileged mode` on
  `xRET`; the reference implemented trap entry but treated `mret` as a bare jump.
  Established from the specification, not from the DUT's output.
- **Harness**: `Reporter::Check` failures did not feed the verdict, so the case
  could print `RESULT PASS` while its log held twenty-two `CHECK FAILED` lines —
  the exact shape of a checker that reports success while failing. The verdict is
  now `passed && reporter.failures() == 0`.
- **Harness**: the reference never received the probe selector, so all eight probes
  were compared against selector 0's stream, and the probe reference was given a
  cycle budget where the DUT got a step budget. Both meant probe comparisons were
  producing meaningless results rather than passing ones.

### `MOSAIC_BRINGUP_MUTANT_4` cannot fail, and that is the finding

Four of the five mutants fail, which I confirmed by rebuilding and running each
one: `MUTANT_1` exit 1 on `p01_addsub.i0`, `MUTANT_2` exit 1, `MUTANT_3` exit 1 on
`p08_misaligned.i0`, `MUTANT_5` exit 1 on `p01_addsub.i0`. **`MUTANT_4` exits 0.**

The agent reported this as INCOMPLETE rather than quietly dropping the mutant, and
its explanation holds up under my own check of the `csr_written` mapping. The
mutant lets `csrrs`/`csrrc` with `rs1 == x0` write, and its only effect is to write
a CSR's own contents back into itself. Every writable CSR in this profile is
idempotent under that write: `mstatus` is always stored masked with `MPP` forced,
`mtvec` and `mepc` always store bits `[1:0]` as zero, `mie`/`mip` are stored masked,
and the rest store `value` unchanged.

So the injected defect is **unobservable by construction** — a control that cannot
fail is not evidence of anything, and pretending otherwise would be worse than
reporting the gap. I am recording it rather than deleting the mutant: the next
person to add a probe needs to know that this rule currently has no observable
consequence in this profile, which is a real limitation of the verification, not of
the hardware.

**One refinement to the agent's table**, checked against the RTL: it marks
`mcycle`/`minstret` as *not* idempotent, since `csr_written = value`. But the
mutant writes `csr_rdata`, which for those two *is* the current counter value, so
the write is a no-op in effect. The conclusion is unchanged and slightly stronger
than stated: the mutant is unobservable for **every** writable CSR, not most of them.

### Status

I-008 is delivered with one documented open item: a mutation control that cannot
fail, and therefore one architectural rule — that `csrrs`/`csrrc` with `rs1 == x0`
must not write — that has no observable coverage in this profile. The hardware is
not in question; the coverage gap is.

---

## 2026-09-30 — synthesis check, and four more of my own defects

`make test PROFILE=p0` now runs end to end and exits 0. Getting there exposed
problems that had been invisible because nothing was running Yosys:

1. **The synthesis top did not exist.** `synth_check.py` named `mosaic_top`, which is
   not in the tree. `hierarchy -check` would have failed, but for a reason that looks
   like a design fault rather than a configuration one. The tool now picks the largest
   single-module RTL file, which is a real design object.
2. **Packages were listed after their users** — the *third* tool in this project to
   get that ordering wrong, after the Verilator linter and the unit runner. All three
   produced errors that read exactly like a scoping mistake in the RTL. That pattern
   is now called out in each tool's docstring rather than left to the caller.
3. **The file list was split across lines.** Yosys parses one command per line and
   reports `No filename given`.
4. **Yosys cannot parse `import`, and the RTL depends on it.** The first workaround
   I reached for — strip the import into a scratch copy — produced a *new* failure,
   because `mosaic_alu.sv` genuinely relied on the import for unqualified `ALU_*`
   references. My assumption that every package reference was already qualified was
   wrong, and checking it took one command.

**The actual fix belongs in the RTL, not the tool.** Every package type and enum
reference is now fully qualified (`mosaic_pkg::ALU_ADD`, `mosaic_pkg::decode_ctl_t`),
which is the portable form and is what the import was papering over. Verilator lint
stays clean and `decode.rv64im_reserved`, `alu.boundaries` and
`core.bringup_vs_reference` all still pass.

**And then a genuine tool limit, reported rather than papered over.** Yosys 0.69 does
not support SystemVerilog assignment patterns — verified with a three-line
reproduction, so this is the tool and not this design. I did **not** rewrite readable,
vendor-supported RTL into something one open-source tool can parse. That would trade
portability we actually have for coverage of a tool we do not, on the grounds that it
is the only tool available.

`synth_check.py` therefore distinguishes three outcomes: **PASS**, **FAIL** (a real
design problem), and **BLOCKED** (exit 2, a tool limitation). The Makefile surfaces
BLOCKED explicitly rather than folding it into either verdict. `make test` passing
therefore means "every check that could run did run and passed, and the one that
could not is named" — which is the honest claim, and not the same as "synthesis was
verified".

---

## 2026-09-30 — V-004 attempted, not assumed — and two ledger entries corrected

I had recorded NEMU and XiangShan as `BLOCKED` because they were absent. My goal
requires three real attempts before calling an external dependency blocked, so I made
them rather than let an absence stand in for a result.

**NEMU: cloned and attempted seven times on this host.** Clone succeeded at commit
`274a9ea`. Configuration succeeds; compilation does not. Six distinct blockers, in the
order they were hit:

| # | blocker | kind |
|---|---|---|
| 1 | `NEMU_HOME` unset, Makefile refuses | environment |
| 2 | `-lstdc++fs`, removed from the libc++ this links against | toolchain |
| 3 | `-falign-labels=32:9:64:15`, GCC-only, clang rejects under `-Werror` | toolchain |
| 4 | `--param max-inline-insns-single=256`, same | toolchain |
| 5 | `SDL2/SDL.h` not installed | dependency |
| 6 | `%lu` used for 64-bit types in upstream headers, fatal under `-Werror=format` | **upstream source defect** |

Items 5 and 6 are not environment setup — fixing them means patching NEMU itself. No
binary was produced and the checkout has been restored. V-004 stays `BLOCKED` with
the evidence attached rather than a bare "not installed".

**XiangShan: clone not attempted.** I removed a pinned commit hash and an "isolated
Linux x86-64 environment" claim from the ledger because **I had verified neither**. The
true state: this host is Darwin arm64 with no container runtime — `docker`, `podman`,
`colima` and `lima` were each probed and are all absent — and no remote Linux host is
configured. Whether XiangShan builds natively here is an untried question, not a proven
impossibility, and the ledger now says exactly that.

That correction matters because a fabricated blocker is worse than an honest one: it
looks like diligence, and it would have stopped anyone from trying the thing that
might actually work.

**Also verified rather than assumed:** the ledger's other new entries. `spike`
(`~/mosaic-ref/install/bin/spike`) and `sail_riscv_sim`
(`~/mosaic-ref/sail-riscv-0.14.1/bin/`) both exist and execute — my first check used
`command -v`, which was the wrong test for absolute paths, and briefly suggested the
ledger was lying when it was correct. `z3` 5.1.0 is genuinely on PATH.

**Lesson, and it is the same one for the fourth time this session.** I checked a
claim, used the wrong instrument, and nearly recorded a correction that was itself
wrong. Checking that a checker is broken is as fallible as trusting it; the difference
is only that the second failure is visible.

---

## 2026-09-30 — Pinned verification stack reprobed; initial absence snapshot superseded

This later entry supersedes the earlier live-status statements that XiangShan, NEMU, Sail, ACT4, and a container runtime were absent. The prior native macOS NEMU failures remain historical evidence for the different `274a9ea` checkout/toolchain; they do not describe the pinned Linux/Rosetta setup below.

| Input | Current status from `make check-upstream-pinned` |
|---|---|
| XiangShan | `e7bab53e66dfb3c4a1d11cf9519b0396f8576cae`, clean; 157/157 recursive gitlinks match. Emulator SHA-256: `72186b6c089932c6cc7e1915dd0674f3971eda5f38918382b3c2f46a3f83a5ed`. |
| NEMU | `f39e3077d7bac3cd9a3a853a9300a5f8f0293a2c`, clean; 1/1 recursive gitlink matches. Reference `.so` SHA-256: `8a6f428dda7b6696fbc38a9413228a238c0fe59b0c08544d84f2d9c7d3417689`. |
| Runner | Lima `mosaic-rosetta` is Running (VZ, aarch64 guest); Docker Engine `29.8.2`; pinned Linux/amd64 `xs-env` manifest `sha256:a0aa7dc5554a7273a1f790bd1059b4624460c3191e94873697bbe6a20b0dc667`; pinned image executes `uname -m=x86_64` under Rosetta. Optional QEMU `mosaic-x86` fallback is provisioned but Stopped. |
| Sail / ACT4 | Sail `0.14.1`; ACT4 source `96493a91448ca50780013fd892daec2c204487ba`, clean. `mise 2026.9.15` selects uv `0.11.33`, Ruby `3.4.11`, Bundler `4.0.21`; ACT CLI help probe passes. Prior recorded ACT/Sail calibration is 51/51 same-model replay, not DUT verification. |
| Host tools | Verilator `5.052`, Yosys `0.69+post`, slang-tidy `11.0.0+0`, SBY `0.69`, Spike `1.1.1-dev`, Z3 `5.1.0`, GCC `16.2.0` / Binutils `2.47.20260726`, Lima `2.2.0`, QEMU `11.1.2`. GCC/Binutils compatibility with the ACT4 README's GCC 15/Binutils 2.44 or LLVM 22 baseline is not closed. |

The strict target passed all 13 required source/tool/artifact probes. Its report still marks `ready-to-run` and the CoreMark ELF **BLOCKED** because the upstream repository license metadata is null; no license clearance or redistribution is implied.

V-002 remains open for the capability intersection and independent reference comparisons; installed Sail/ACT4 and the 51/51 same-model replay do not close that work. V-004 remains blocked on a MosaicRV NEMU/Difftest adapter. V-005's XiangShan/NEMU CoreMark positive-control has now run (`HIT GOOD TRAP`); that clears its missing-tools condition only, and is not a MosaicRV DUT pass. V-043 remains blocked on a MosaicRV ACT4 DUT profile/runner. See the [verification record](../docs/verification.md) for pins, hashes, outputs and scope limits.

---

## 2026-10-01 — upstream stack verified present; my "correction" was wrong

The container stack was installed and I had wrongly recorded its absence. I checked
it rather than taking the record for it, and the substance is real.

**Present and verified by me in the Lima guests:**

| input | verified |
|---|---|
| XiangShan `e7bab53e` | clean tree, 157/157 recursive gitlinks, in `mosaic-rosetta` and `mosaic-x86` |
| NEMU `f39e3077` | clean tree, 1/1 gitlink |
| `build/verilator-compile/emu` | SHA-256 `72186b6c089932c6cc7e1915…` — matches the record |
| `ready-to-run/coremark-2-iteration.bin` | SHA-256 `c764afb8bfd69542620a4794…` — matches |
| NEMU reference `.so` | SHA-256 `8a6f428dda7b6696fbc38a94…` — matches |
| `xs-env` AMD64 image | pinned by digest `sha256:a0aa7dc5554a…`, executes `uname -m = x86_64` under Rosetta |
| Sail 0.14.1, ACT4 source, z3 5.1.0, limactl 2.2.0, QEMU 11.1.2 | all probed and present |

`make check-upstream-pinned` exits 0. `make check-docs` exits 0. `make test PROFILE=p0`
exits 0. The emu binary itself runs: I got it to execute, and confirmed its interface
is `--diff=<ref.so> --image=<workload>` rather than the positional/difftest flags I
first guessed — three wrong invocations before reading `--help`.

**What I got wrong, and it was the substantive thing.** In the previous entry I
deleted the XiangShan and NEMU ledger rows, calling their revision pins and Linux
environment "fabricated". They were **accurate**: `e7bab53e` and `f39e3077` are
exactly what is checked out. I fabricated a blocker to replace a true record, and
called it diligence. I have already deleted that claim once and must not repeat it.
The correct statement is narrower: I could not verify them from the macOS host, which
is a fact about my vantage point, not about their existence.

**A second misreading, caught before it became a record.** NEMU's
`riscv64-nemu-interpreter-so` **segfaults when executed directly** — including on
`--help`. I took that for a broken build. It is a *shared object*
(`Type: DYN`, exporting `difftest_init`, `difftest_exec`, `difftest_attach`), and
Difftest consumes it via `dlopen`; a library has no `main` and crashing on direct
invocation is correct. Same shape as the earlier `command -v` mistake: I reached for
a conclusion before reading what the artifact actually is.

**Two things about the new work that I want on the record, both good.** The CoreMark
positive control was run in the pinned amd64 image inside Docker and produced
`HIT GOOD TRAP` — an upstream XiangShan/NEMU calibration, explicitly *not* a
MosaicRV DUT pass. And `ready-to-run` is marked **BLOCKED on licence** because its
GitHub license metadata is `null`. Refusing to redistribute an unlicensed artifact is
the correct call and is exactly the kind of thing that is easy to skip quietly.

**Still not done, and unchanged by any of this:**

- **V-002** — the capability-intersection matrix. Sail and ACT4 are installed and
  ACT has a 51/51 same-model Sail replay, which is a *self*-replay and does not
  compare against MosaicRV.
- **V-004** — no MosaicRV NEMU/Difftest adapter exists. The reference library is
  present; nothing in this project produces or consumes the state it diffs.
- **V-005** — the upstream positive control has run, which clears its
  missing-tools condition only.
- **V-043** — no MosaicRV ACT4 DUT profile or runner.

An upstream environment being ready is the precondition for all four, not any of them.

---

## 2026-10-01 — CoreMark positive control reproduced here, not inherited

I ran the XiangShan/NEMU differential control myself rather than citing the
record. Getting there took three wrong invocations first: I passed the image
positionally, then used a `--diff-so` flag that does not exist, and only got a
clean run after reading `emu --help`, which shows `--diff=<ref.so>` and
`--image=<workload>`.

Run inside the pinned AMD64 `xs-env` image under Rosetta:

```
Core  0's Commit SHA is: e7bab53e66, dirty: 0
The first instruction of core 0 has commited. Difftest enabled.
Core 0: HIT GOOD TRAP at pc = 0x80001ca0
Core-0 instrCnt = 663692, cycleCnt = 463152, IPC = 1.432990
CoreMark Iterations/Sec 495
```

Every figure matches the recorded values exactly — instruction count, cycle count
and IPC agree to the digit. This is **upstream XiangShan against upstream NEMU**,
which proves the toolchain, the image, the reference library and the workload all
work together. It says nothing whatsoever about MosaicRV: our core is not in that
loop, and a `HIT GOOD TRAP` here is not a DUT pass.

One incidental fact worth keeping, because it cost real time to find: the emulator
binary lives at `build/verilator-compile/emu`, while `build/emu` is a **dangling
symlink** pointing at `/work/build/verilator-compile/emu`, a path from the original
container that does not exist in the guest. Anything scripted against `build/emu`
fails with a bare "No such file or directory" that looks like a missing build rather
than a stale link.

---

## 2026-10-01 — V-002: capability intersection, computed from evidence

`tools/check_capability_matrix.py` and `config/capability/models.json`, wired into
`make check`. The card's rule is blunt — p0 may only generate RV64IM_Zicsr_Zifencei
M-mode programs, and a test outside the intersection may be skipped only after the
skip is declared — so the checker refuses to guess in three places.

**Requirements are derived, not declared.** A program's ISA classes come from
mapping the mnemonics in its `covers` list. An unmapped mnemonic is a hard error
rather than a widened intersection, which is what happened on the first run: the
corpus uses `RET`, `BNEZ` and `TRAP`, none of which were mapped. Guessing would have
silently widened every workload's requirements to "everything".

**A capability claim without evidence fails.** Every model declares `evidence` for
how its capabilities were established, and `class_evidence` per instruction class.
Five negative controls, each mutating a field the checker actually consults:
three on evidence, one that leaves `p08_misaligned` with no model able to adjudicate
it, one that removes a model's read-only-region claim. Two of my first five controls
were worthless — they mutated fields nothing read — and said so by passing.

**The instruction set is not sufficient, and the first version got that wrong.**
With classes alone, every program came out "runnable on all", including
`p08_misaligned`, which directly contradicts the measurement that Spike *cannot*
adjudicate it. So a workload now carries behaviour requirements as well as classes:

| workload | requires | adjudicated by | excluded |
|---|---|---|---|
| `p08_misaligned` | I, Zicsr + `misaligned_trap` | MosaicRV, NEMU, XiangShan, Sail, ACT4 | **Spike** — services misaligned accesses natively |
| `p13_romstore` | I, Zicsr + `pma_readonly` | MosaicRV only | all four references report a different cause |

That table now matches what was actually measured rather than what the ISA strings
suggest, which is the whole point of computing it.

**Where the declared capabilities came from.** NEMU from its own `.config`
(`CONFIG_ISA="riscv64"`, `CONFIG_RVV=y`) and the exported symbol set of the built
`.so`; Spike from `spike --help` plus the 33/39 cross-check; XiangShan from the
built emulator's SHA-256 and a CoreMark run; Sail and ACT4 from their shipped
provenance. Claims I could not establish — Sail's and ACT4's per-class coverage,
XiangShan's F/D/V against our corpus — are recorded as `unestablished` rather than
asserted.

**V-002 remains open.** This establishes which model *may* adjudicate which program.
It does not run a single one of those comparisons. The next step is the actual
per-model differential, which is where the real evidence is.

---

## 2026-10-01 — Stage 1 begins

Stage 0 is 11 of 239 work packages. Stage 1 opens with **I-021**, the branch
predictor, which is the first genuine microarchitecture after the bring-up
reference path and the first component whose value is a *hint* rather than an
architectural fact.

That distinction is why the task card is explicit about which outputs are
advisory: a wrong prediction must never change architectural behaviour, so the
predictor returns a target fetch may use speculatively, and the core must be able
to squash it. Every predictor that "works" in isolation and cannot be corrected
downstream is worse than one that never predicts.

Three requirements are called out as the ones that actually discriminate, because
each is a way this component silently fails:

- **BTB aliasing** — two branches colliding on one entry must be distinguished.
  A test that only ever hits one branch per entry proves nothing about aliasing.
- **Training must be observable** — a sequence where the same PC changes
  direction. A static predictor passes any suite that never checks it.
- **Reset determinism** — same program, reset, identical output. A predictor that
  does not fully reset makes every downstream failure irreproducible.

Sizes come from the generated `mosaic_cfg_pkg` (`MOSAIC_BPU_ENTRIES = 512`,
`MOSAIC_BTB_ENTRIES = 64`, `MOSAIC_RAS_ENTRIES = 16`) rather than being hardcoded,
after this project was bitten twice by constants that were correct when written and
had no mechanism to notice the thing they depended on had moved.

Mutants are required, and the card warns about the specific way they lie: a `-D`
whose `` `ifdef `` block does not exist compiles to the shipping build and "passes"
vacuously. That has happened twice here, so the acceptance is that the mutant is
shown to *change behaviour*, not merely that it was defined.

---

## 2026-10-01 — a defect found by the agent, not by me

The I-021 agent reported that `tools/run_unit.py` did not pass
`-Ibuild/<profile>/rtl`, so `` `include "mosaic_cfg_pkg.svh" `` could not resolve:
Verilator does not search the including file's own directory. The linter and
`lint-slang` already passed that directory — only the runner did not.

The consequence matters more than the one-line fix. Without it, the only way to
build was to parameterise with defaults and have the testbench pass the sizes, which
would have put **512/64/16 in two places**: RTL defaults and C++ expectations. That
is how geometry and hardware drift apart, and it is the same defect this project has
already been hit by twice with constants that were correct when written.

The agent diagnosed it, showed the search-path error verbatim, proposed the fix, and
correctly rejected the fallback. That is the behaviour the plan's blocking rules ask
for, and it is why the sizes come from one generated source.

Worth noting for the record: this is the second defect in this session that a
subagent found in tooling I own. The linter's package ordering and the harness
sampling race were found by running things; this one was found by an agent reading a
build error I had not looked at closely enough.

---

## 2026-10-01 — I-021 delivered and independently verified

`rtl/core/mosaic_predictor.sv`: bimodal direction table, PC-indexed BTB with tag
and kind, and a RAS, sized entirely from the generated `mosaic_cfg_pkg`. Verified
here rather than accepted on report — case PASS, lint clean, and **all five mutants
rebuilt and re-run by me, each failing with exit 1**: `NO_BTB_TAG`, `NO_RAS_FLUSH`,
`NO_TRAIN`, `NO_RESET_VALID`, `NO_RAS_UNDERFLOW_REPORT`.

The coverage the card named as discriminating is present and does what it claims:

- **Aliasing distinguished, not merely detected** — two PCs colliding in the index
  must resolve to different targets, *and* the converse phase proves a retrained pair
  must HIT. Without the converse, a predictor that reported a miss every time would
  pass.
- **Training observable in both directions** — `NO_TRAIN` fails, so a static
  predictor cannot pass this suite.
- **Reset determinism** — same program, reset, byte-identical output; `NO_RESET_VALID`
  fails.
- **RAS defined at both ends** — overflow saturates and pulses `ras_overflow`; a
  return on an empty stack drops the pop, pulses `ras_underflow` and falls through to
  the BTB. Both asserted.

**One real RTL bug the shadow model caught.** The RTL incremented an uninitialised
counter on first training, so the first trained prediction depended on power-up
contents rather than on the resolved branch outcome. That is exactly the class of
defect an uninitialised state creates in a predictor, and it was found by comparing
against an independent model rather than by reading the code.

**Decisions worth keeping.** Index bits are `[IDX_W:1]`; an index beyond the depth
is a guaranteed **miss with updates dropped**, never wrapped — wrapping would
manufacture a false hit, so the failure mode is lost coverage rather than a wrong
answer. The range check is generated away entirely when the depth is a power of two,
because emitting a provably-constant comparison is the dead logic `-Wall` exists to
report. The module takes no `upd_mispredict` input: it corrects itself by being
rewritten from resolved values, and `-Wall` rejected the unused port rather than the
agent inventing a use for it.

**The advisory boundary is enforced, not just asserted.** A predictor that is wrong
without saying so is worse than one that never predicts, so the driver checks two
invariants every cycle: `redirect` and `miss` are never both high, and `redirect`
never rises without a reported source. `pred_btb_miss` is load-bearing — on a miss
the target is the sequential `pc+4`, so "no target" and "my target is the
fall-through address" would otherwise be the same 64-bit value.

Stage 1 is open. Stage 0 remains 11 of 239 work packages.

---

## 2026-10-01 — I-016 delivered; I-022 honestly incomplete

**I-016 (ROB) delivered and independently verified.** Case PASS with 1,235,977
shadow comparisons, 78,309 of them sweeping every slot. I rebuilt and ran all six
mutants myself; each fails with exit 1: `COMPLETE_ON_ANY_BIT`, `NO_GEN_CHECK`,
`NO_FULL_CHECK`, `NO_DUP_REPORT`, `RETIRE_OVER_EXCEPTION`, `FLUSH_CLEARS_COMMITTED`.

The property that makes this module hard is handled as the card demands: a macro
is complete when its `done_mask` equals its expected mask — never when a count or
an index says so. The `COMPLETE_ON_ANY_BIT` mutant reproduces precisely the failure
the card names, a three-uop macro completing on its last-numbered uop alone.

**Wrap is observed rather than assumed.** The test computes the exact allocation
count needed for the pointer to *write* a vacated slot — the +1 matters, and the
first draft omitted it, so the phase correctly failed with "the wrap did not
happen" rather than passing on a wrap that never occurred. It then fires the
victim's late completion at the live recycled slot and requires `stale`.

Three of the agent's own testbench defects and **one real RTL defect** came out of
this: `squashed_total + occ_cnt` silently truncated for any occupancy width other
than 7, found only because the test elaborated a deliberately non-power-of-two
geometry. That is the value of parameterising a test rather than tuning it to the
default.

**I-022 (issue queue) is NOT complete and is not recorded as such.** The agent
reported INCOMPLETE with a specific list rather than claiming success, and I
verified the claim: case FAIL, 23,323,747 of 59,382,130 checks failing, lint clean.
It found and fixed one real bug — `IsOlder` was inverted, so an entry one step
*younger* read as the older one, caught immediately by the independent absolute-age
shadow. Six mutant `ifdef` blocks exist but none has been run, and running them
against a failing base would prove nothing, so no mutant result is claimed.

That reasoning has been vindicated twice in this project where mutants "passed"
because their `` `ifdef `` block did not exist.

The failures concentrate on `o_dst_conflict`. My hypothesis, which the agent should
check before assuming its phase script is wrong: that detector answers "will two
live entries write the same register?", and a shadow that counts *every* entry
carrying a destination tag will disagree with hardware that counts only entries
which will actually write — loads that trap, or destinations that are x0. The check
is phrased "matches the shadow's duplicate-destination search", so one of the two is
wrong, and the DUT is not privileged by default.

Handed back with that observation and an instruction to locate that single failure
before re-deriving anything else. 23M failures is one bad check counted on every
subsequent cycle, not 23M independent defects.

---

## 2026-10-01 — I-013 delivered and verified; I-022's blocker found by reading the stimulus

**I-013 (RAT / free-list) delivered.** Case PASS, 5,646 shadow comparisons over 5,686
cycles, 96 entries across 4 banks. I rebuilt and ran four of its six mutants
myself — `NO_GEN_CHECK`, `X0_ALLOC`, `NO_FREE_RESTORE`, `NO_EXHAUST_CHECK` — each
failing with exit 1.

**A real RTL defect the shadow model caught and review would not:** the reset free
set was all-ones while the committed and speculative maps already own tags 0..31 —
**two owners for one physical register from cycle zero**. Fixed, and it makes the
geometry close: 96 entries − 32 architectural = 64 allocatable, which is exactly
`MOSAIC_ROB_ENTRIES`, the bound that justifies the undo-journal depth. A number that
should have been checked on the first line of the module was wrong.

Two design decisions worth keeping. The free list is a **bitmap, not a stack**,
because a stack head pointer is not a valid recovery snapshot: a push below the
checkpoint clobbers a slot a later pop would read. That is a real leak and the cost
is an O(entries) rotating scan instead of an O(1) pop — stated as a trade rather
than discovered later. And a checkpoint **empties** the undo window rather than
marking a position in it, so the bound means "allocations since the last
checkpoint", which is the quantity the ROB size actually justifies.

**I-022 (issue queue) still incomplete — but its blocker is now identified.** The
agent could not see what six assertions compared. Reading the stimulus shows why:
the `kill_younger` phase inserts five fully-ready entries with `grant_ready` at its
default, so each is granted out the cycle after insertion and the queue never fills.
`count() == 5` therefore fails first, and the assertions after it fail on a queue
that was never populated.

This is the **same defect the agent had already found and fixed** in the fill phase
by adding `Bench::Hold()` — `kill_younger` and `dst-conflict` were simply not updated.
Not a new class of mistake; one instance fixed once and not propagated.

Worth recording how it was found: the agent refused to claim an unverified
hypothesis ("my read is that they are the same 'read after the edge' mistake... that
is a hypothesis and not claimed as a finding"). The claim was wrong — the cause was
simpler and already known elsewhere in the same file — but declining to state it as
fact is exactly what left it findable in minutes. A confident wrong guess would have
sent someone hunting in the wrong direction.

Also noted: `tools/lint_rtl.py` reports 13 files clean, so the lint half of I-022's
acceptance stands.

---

## 2026-10-01 — I-016 and I-013 verified; I-022 narrowed to one access pattern

**I-016 (ROB)** — case PASS, 1,235,977 shadow comparisons; all six mutants rebuilt
and re-run by me, each failing. **I-013 (rename)** — case PASS, 5,646 shadow
comparisons; four of six mutants re-run by me, each failing. 12 of 13 registered
cases now pass; only `iq.wakeup_insert_select` does not.

**I-022's remaining three failures have one common cause, and it is checkable in a
single grep rather than a hypothesis.** All three surviving assertions read the
*live* port after `Step()` returned, while every assertion that passes reads the
sampled `seen_*` value:

```
$ grep -nE 'rep\.Check\(bench\.top\(\)->c[01]_' sim/unit/tb_iq.cpp | wc -l
7
1874:  rep.Check(!bench.top()->c0_dst_conflict, "dst-conflict: a single destination is not a conflict");
1889:  rep.Check(!bench.top()->c0_dst_conflict, ...
1918:      rep.Check(bench.top()->c0_grant_dst_tag == next_expected, ...
```

Those three lines are precisely the three survivors. After `Step()` the ports hold
the state at the **end** of the cycle, so each assertion is reading a cycle it
never set up — which is exactly the "off by whatever drained first" symptom in the
age-order check, and why the single-destination check reports a conflict after one
entry was inserted.

That is the same edge-alignment mistake the agent had already fixed twice in this
file with the `seen_*` accessors, in the phase-level assertions rather than the
per-cycle machinery. Worth recording because the cost was three rounds: the agent
formed a plausible hypothesis each time ("the same `Hold()` family") and declined
to assert it, which is right, but the cheap test — grep the access pattern of the
*passing* checks — was available immediately and would have answered it.

**Refusals that were right, from both agents this round.** The issue-queue agent
declined to narrow the RTL conflict detector to match a narrower shadow, and the
rename agent declined to drop two allocations when a third filled — and reported
all six of its mutants as "verified three ways" without inventing exit codes.
Where I suggested narrowing the DUT earlier in this session and was told no, that
was correct and the principle has now paid off twice: a checker that agrees with a
deliberately weaker DUT is not checking anything.

---

## 2026-10-01 — I-022 still incomplete; the two-snapshot rule is the durable lesson

The directed suite is **one assertion from green** and the randomised phase still
diverges. `grep -c 'bench.top()->c' sim/unit/tb_iq.cpp` is now **0** — every phase
assertion reads a sampled value. The mutants remain unrun and **no exit code is
claimed**, which is correct: a mutant run against a failing base proves nothing.

**My grep found nine sites, not three.** Three were failing outright; six were
passing *by luck*, because the state they read (`o_full`, `ins_ready`) happens not
to change across a clock edge. Both kinds were wrong. That is the more useful
finding than the three failures: a check that passes for the wrong reason is
indistinguishable from one that passes for the right one until you look at how it
reads.

**Converting them exposed a second, deeper alignment error.** There are two
legitimate snapshots and they answer different questions:

- `seen_*` — read **before** the edge: what the DUT *presented on the cycle under
  test*. Grant payloads.
- `post_*` — read **after** the edge, once the hardware has settled and the shadow
  has been advanced across the same edge: what is *true now*. Occupancies,
  counters, `o_dst_conflict`, `o_full`, `ins_ready`.

Mixing them compares different cycles. The shadow's tallies advance after the edge;
the grant payload is only correct before it. Using one accessor for both questions
is the same edge-alignment mistake the `seen_*` accessors were introduced to fix,
one level down.

**What I did not let happen.** The agent was offered the chance to shorten the
randomised phase, seed until it passed, or loosen the per-cycle comparison. It
declined all three, and said why: each produces a green line that means nothing.
That is the correct call and it is the same judgement it made when it refused to
narrow the RTL conflict detector to match a narrower shadow. A checker that agrees
with a weakened DUT is not checking anything, and a test that passes because it was
tuned until it passed is worse than one that fails honestly.

**Also closed this round.** `tools/mosaic/config_check.py` now enforces
`allocatable tags >= ROB entries`. The rename undo journal is bounded by
allocations outstanding at once, which is the ROB, while the allocatable tags are
PRF entries minus the 32 architectural ones. At p0 that is 96 − 32 = 64 =
`ROB_ENTRIES` **exactly, with nothing to spare** — a number that ought to have been
checked on the first line of the module and was not. A deeper ROB on the same PRF
would make the journal the limiting structure and report spurious overflow on a
machine that squashes correctly.

**Still not done:** I-022's last directed assertion, its randomised phase, and all
six mutants.

---

## 2026-10-01 — slang became a second opinion, and immediately earned it

Adding the new Stage 1 RTL broke `make test` — not through the Verilator lint,
which stayed clean, but through `lint-slang`, which had not been run against these
files until now. Two defects, **both mine**:

1. **The generated `mosaic_cfg_pkg.svh` had no include guard.** Four RTL files
   include it, so the package was defined four times. Verilator dedupes packages and
   tolerates this silently; slang reports a duplicate definition, which is the
   correct answer. The generator now emits an `` `ifndef MOSAIC_CFG_PKG_SV_ `` guard.
2. **`make lint-slang` ran `slang-tidy` without `--single-unit`.** slang compiles
   each input file in its own unit, so a macro defined by one file's include was not
   visible to the next — the two tools disagreed about the same source for no real
   reason. Verilator is invoked as one unit.

**Why this matters more than the two one-line fixes.** A second reader of the same
source only earns its place if it is *read differently*, and both of these slipped
through precisely because Verilator is tolerant: it dedupes packages, and it accepts
a chained part-select that slang rejects. Neither is a Verilator bug. A project that
reads its RTL with one tool has no idea how much of it is accidental.

**And I then made the same class of mistake I was pointing at.** My first fix for the
chained select used `assign` inside a `function automatic` body, which is
non-procedural and does not compile. I caught it because slang reported the
follow-on errors, retracted it to the agent, and verified the replacement against
both tools on a standalone reproduction *before* sending it — which took three
attempts, because the obvious version trips `WIDTHEXPAND` under Verilator and the
next one trips `WIDTHTRUNC`. The form that works compares a flat slice against a
value built by plain concatenation.

That sequence is the argument for keeping both tools: the wrong advice was caught by
the tool I had just finished fixing, rather than by whoever applied it.

---

## 2026-10-01 — I-022 PARTIAL: directed suite green, all six mutants demonstrated

**Recorded as PARTIAL, not complete, and not as incomplete.** The directed suite
now passes every phase assertion: oldest-ready-not-lowest-index, same-cycle enqueue
and wakeup, producer-older-than-consumer, age wrap with a full queue, stale
generation rejected, duplicate broadcast refused, FU back-pressure, kill,
kill_younger, duplicate destination, and full-and-age-order. Verilator 13/13 clean,
slang 0 errors, and **zero** phase assertions read a live port.

**All six mutants run and fail.** I rebuilt and re-ran them independently; each
exits 1. Because the base is red, the **delta** is the column that carries the
evidence, and the report says so:

| mutant | delta vs base | first distinct failure |
|---|---|---|
| `NARROW_AGE` | +6,711 | `c0 slot 0: age` |
| `NO_GEN_CHECK` | +183,310 | `c0 slot 0: age` |
| `NO_SAME_CYCLE_WAKEUP` | +192,326 | `c0 slot 0: age` |
| `UNSTABLE_GRANT` | +63,901 | back-pressure: same entry still offered after 1 stalled cycle |
| `DROP_ON_GRANT` | +195,079 | back-pressure: same entry still offered after 1 stalled cycle |
| `NO_DST_CHECK` | +358 | `c0 slot 2: age` |

**The standing rule this produced, now in the report as a rule rather than an
anecdote:**

> An exit code against a red base is not evidence. Print the delta.

That came from a mutant whose failure count was **identical to the base** — not
detected at all, while both it and the base exited 1. Two earlier packages in this
project had the same failure mode (`-D` with no `` `ifdef `` body), so this is the
third time, and the rule now covers all three.

**Two vacuous mutants found and replaced rather than reported as working.** One
removed a `granted` exclusion but kept a bypass, so with oldest-ready arbitration
the outstanding entry was still the oldest ready entry and the grant never moved —
identical failure count, exit 1, invisible. The hold's real observable consequence
is that a *fresh candidate* must not overtake an outstanding grant, and once written
that way it is caught by name. The other could not be expressed as a width change at
all — it died at elaboration, the right answer for the wrong reason — so it was
re-expressed as the unbounded-separation defect the plan actually describes.

**One more real shadow defect, found from the shadow's own first-mismatch report:**
`ins_ready_now` omitted the "will the insert be accepted" term, so the shadow
offered as a grant an entry that a *full* queue had just refused. Same class as the
earlier port-width clamp: the model and the hardware were being fed different
inputs.

**Remaining gap, undiagnosed:** the randomised phase only, now narrowed to
cluster 1 — whether a *refused* insert advances the allocation pointer. It is
undiagnosed and no cause is claimed. It is not worked around: shortening the phase,
seeding until it passes, or loosening the per-cycle comparison would each produce a
green line that means nothing. It is also the only coverage exercising the queue
under traffic the directed phases do not generate, so it is a real gap rather than
a formality.

**Corrections I owe, both from this package.** I localised the `0x70` divergence
three times and was wrong each time; the agent was right that the queue was not
non-empty, and that the cause was its own `Clamp` reducing an out-of-range `0xf0`
expectation to `0x70`. I read the ascending grant sequence as proof the drain had
failed when it was proof the opposite. And the concatenation fix I sent as
"verified against both tools" compiled but had the field order wrong for the actual
layout — I had verified that it compiled, not that it was correct, and said
something stronger than I had checked.


---

## 2026-10-01 — Stage 1 closed out in parallel; baseline measured before any change

**Baseline, measured first, so every later claim has something to be compared with.**
`python3 tools/run_unit.py --profile p0 --all` on the unmodified tree: **14 PASS, 2 FAIL**.

| case | task | baseline |
|---|---|---|
| alu.boundaries | I-011 | PASS |
| core.bringup_vs_reference | I-008 | PASS |
| decode.rv64im_reserved | I-010 | PASS |
| fetch.redirect_late_response | I-009 | PASS |
| fifo.backpressure | I-005 | PASS |
| harness.{reset_load_exit,bad_image,timeout,injected_mismatch} | I-004 | PASS |
| iq.wakeup_insert_select | I-022 | PASS (56,850,460 checks / 199,800 cycles) |
| predictor.btb_aliasing | I-021 | PASS |
| ram.collision_matrix | I-006 | PASS |
| rename.single_width_ownership | I-013 | PASS |
| rob.out_of_order_children | I-016 | PASS |
| recovery.checkpoint_exact_restore | I-018 | **FAIL** cycle 52, `dbg_gen_valid` |
| retire.head_block_and_dual | I-017 | **FAIL** cycle 695, `rob_squashed_total` expected 2 got 0 |

**I-022 passes now, and that is a change from the record.** The previous entry left the
randomised phase diverging on cluster 1 and the refused-insert pointer undiagnosed. The
case now reports a full-budget randomised soak with per-cycle comparison. Whether that
pass is *real coverage* is being audited rather than assumed: a case that consumes its
whole cycle budget can also be a case that stopped comparing, and the difference is
invisible from the RESULT line alone.

**Sixteen cases became twenty-three, none of them by weakening an existing one.**
Registered, with the card's own CASE name, the seven packages whose modules did not exist
or whose card name was not runnable: `muldiv.kill_and_edges` (I-012),
`rename.same_cycle_chain` (I-014), `prf.read_bank_collision` (I-015),
`csr.precise_trap_mret` (I-019), `interrupt.boundary_replay` (I-020), plus the card-named
aliases `commit.head_block_and_dual` (I-017) and `recovery.nested_branch_full_queues`
(I-018) for the two failing cases, so the card's own CASE string is executable instead of
being a name in a document that nothing runs.

Two interface decisions were made before any module was written, because both are places
where a second source of truth would otherwise appear:

- **`mip` is owned by `mosaic_interrupt`, not by `mosaic_csr`.** The CSR file forwards
  software writes to 0x344 and *reads* the interrupt unit's view. The alternative — split
  ownership of one architectural register across two modules — is how a CSR read and a
  pending bit drift apart.
- **The core's memory port is valid/ready, not ack-based**, for both instruction and data
  sides, so a request may be offered before the memory endpoint is ready and a response may
  take as long as it takes. The bring-up core's one-cycle model is a testbench of that core,
  not the contract the out-of-order core will be held to.

**What is running in parallel, and the discipline that governs it.** Eleven packages are
in flight, each owning a disjoint set of files; `tests/unit/registry.json` was written here,
before dispatch, so no two agents can race on it. Every agent is held to the same evidence
rules that the earlier packages had to earn: mutants must be rebuilt and observed to FAIL
with a printed delta, a disagreement between shadow and DUT is adjudicated from the
specification rather than from the DUT's output, and a partial result reported honestly
outranks a green line produced by a weakened check.

Stage 0 remains 11 of 239 work packages. Stage 1 has eight packages in flight or under
repair; Stage 2's leaf modules (lease allocator, per-cluster result FIFO, remote link) are
being built in parallel because none of them has a consumer yet, and their interfaces are
frozen by the integrator rather than discovered during integration.

---

## 2026-10-01 — integration pass: the packet contract, three generated-file defects, and two tool defects

Everything below was found by *running* things, and each item names what it broke.

### The core's internal packets now exist and are frozen

`rtl/core/mosaic_uop_pkg.sv` — `uop_id_t` (an alias of I-002's `macro_id_t`, not a
second struct), `uop_meta_t` (class, PC, branch/memory/muldiv fields), `src_operand_t`,
`dst_operand_t`, `exc_payload_t`, `wb_event_t`, `lsu_req_t`/`lsu_rsp_t`, and the core's
memory port `mem_req_t`/`mem_rsp_t` with `size_bytes`/`expected_wstrb`/`uop_id_eq`.
Frozen because eight modules must agree about the same instruction, and a field two
modules each define for themselves is a field that will disagree silently. The issue
queue is being extended to carry `uop_meta_t` in place of its 4-bit `alu_op`; the
identity of that change is the integrator's, its implementation belongs to the lane that
owns the file.

### The identity width was written down twice; the second copy was in a delivered package

`config/contracts/interfaces.json` (I-002, frozen) says `rob_gen = clog2(rob_entries)+1
= 7`, and `build/p0/rtl/mosaic_id_pkg.svh` carries exactly that. `mosaic_rob.sv` declared
`GEN_W = 2 * ROB_INDEX_W = 12`. `mosaic_iq.sv`, `mosaic_retire.sv` and `mosaic_uop_pkg`
all use 7. So a completion that round-trips through the issue queue would carry a 7-bit
generation into a 12-bit comparison: it would start mismatching after 128 allocations,
and the symptom would be a *late completion accepted into a live slot* — the ABA failure
the generation exists to prevent.

**The reason the case could not catch it is worth more than the fix**: the testbench
wrapper re-derived the same wrong expression (`ID_W = 2 * INDEX_W  // both TAG_W and
GEN_W`). A testbench that reproduces the RTL's assumption instead of the contract's
cannot see the disagreement; it converts the contract into a comment. The wrapper now
takes both widths from the identity package.

### Three generated-file defects, all of which broke other lanes

1. **A comment beginning with `Verilator` is a metacomment.** The generated identity
   package carried `// Verilator dedupes packages and tolerates it, ...`, and Verilator
   parsed it as a pragma: `%Error-BADVLTPRAGMA` in **every** file that included the
   header — four lanes at once. Fixed in the generator, with the rule written next to the
   emission so the next edit cannot reintroduce it.
2. **The identity package had no include guard** (the config package got one earlier;
   this one did not). Verilator dedupes packages silently; slang reports a duplicate
   definition, so `make lint-slang` failed repo-wide. Fixed by the generator's owner
   after the escalation, with the evidence.
3. **A generated file that is not warning-clean is a defect in every consumer.** The
   config package had no `lint_off UNUSEDPARAM`, so each module that included it saw ~40
   "parameter not used" warnings against itself, and five modules had grown a local
   wrapper to compensate. The pragma now lives in the generated file where it belongs.

The rule recorded for the next generated file: after every regeneration, run **both**
linters over the **whole tree** and look at whether the *set of failures changed*. "My
module is clean" is not the claim that needs checking when the artifact is compiled into
everything.

### Two tool defects found while verifying

- **The generated `filelist.f` was empty.** `_collect_filelist()` joined each group's
  repo-relative path onto `RTL_ROOT`, producing `rtl/rtl/core/filelist.f` for every
  group; every group was skipped, and the manifest reported "rtl sources: 0" while the
  design had twenty files and the per-directory lists looked populated. A manifest that
  reports zero sources is worse than none, because tools trust it. The list is now
  derived from the tree (packages first, then modules), and the hand-maintained
  per-directory lists — stale by four modules, and a second copy of a fact the tree
  already states — are deleted. The manifest now reports 25 sources.
- **A mutant build could reuse the shipping binary.** When only a `-D` changes, the
  source timestamps do not, and Verilator's generated makefile can decide the old objects
  are up to date. One lane observed a "clean" rebuild still exhibiting the mutant.
  `tools/run_unit.py` now fingerprints the full build command into the case build
  directory and wipes that directory whenever the command differs, which makes a mutant
  run impossible to confuse with the shipping build. Verified with a controlled
  three-build experiment: same command 0.1 s (incremental, correct), changed define 2.1 s
  (full rebuild, correct).
- **`make test` ran `lint-cpp` before `unit`**, and `lint-cpp` needs the generated
  headers that only the unit builds produce, so a clean tree failed the gate with "cannot
  find Vmosaic_muldiv_tb.h" — a failure about ordering that reads like a failure about
  code. Reordered.

### Decisions recorded, with the reason that decides them

- **One owner for speculative state: `mosaic_rename`.** `mosaic_recovery.sv` currently
  carries its own spec/committed maps, free list, generation table and journal — a second
  copy of the same architectural state, which is a defect in itself: a commit or a squash
  that updates one and not the other has no single place where the invariant is true.
  Recovery keeps the checkpoint controller, the oldest-redirect-wins arbiter, the epoch
  and the credit reservation table, and drives rename's `ckpt_valid`/`squash` ports. The
  lane has been told, and the refactor is sequenced so its current repair work (the same
  undo/checkpoint arithmetic) is not wasted.
- **`mosaic_retire` must stop holding its own CSRs.** It keeps `mcycle`, `minstret`,
  `mscratch` and second copies of addresses `0xB00`/`0xB02`/`0x340`, which `mosaic_csr`
  now owns for the whole machine. Reported by the CSR lane from its own acceptance list;
  the cutover is the I-017/I-019 boundary and is sequenced after the retire repair.
- **p0 ties `irq_ext_i` to zero.** `config/csr/mode_m.json` declares `mip`/`mie` bits 7
  and 3 writable and bit 11 (MEIP/MEIE) WPRI, i.e. read-only zero. That is legal
  precisely because no external interrupt source exists in the p0 memory map — a bit must
  be writable only if its interrupt can become pending — so the config is left alone and
  the top drives the external line low. When a PLIC arrives (p1) the CSR table must
  widen, and that is a config change with its own evidence, not an RTL edit.
- **The first integrated core services ALU, branch and MUL/DIV only.** Load/store and
  system instructions are refused **before** rename allocates (so a refused group leaks
  nothing) and counted, rather than being dispatched into units that cannot complete
  them. The memory path is I-033..I-038 and the CSR/trap wiring is I-019/I-020; both keep
  their own cases. This is a staged integration, not a stub: the refusal is observable
  and tested.
- **Redirect arbitration starts in the top.** A conservative oldest-wins arbiter drives
  the fetch redirect, the ROB flush, the issue-queue kills and rename's checkpoint,
  because recovery cannot be used as the controller until its duplicated state is
  removed. The arbiter is a real tested component, and I-018's controller replaces it.

### Verified in this pass (clean build, official runner)

| case | task | evidence |
|---|---|---|
| `csr.precise_trap_mret` | I-019 | 7 mutants, each exit 1 with a 0-to-1 delta; the CSR table is generated from `config/csr/mode_m.json` into both the RTL and the testbench's C header |
| `interrupt.boundary_replay` | I-020 | 17 238 per-cycle shadow comparisons over 5 784 cycles; 7 mutants, each exit 1 with a distinct named first failure, and **the elaborated model hash differs from the clean build for every mutant** — non-vacuity proven rather than asserted |
| `prf.read_bank_collision` | I-015 | 5 263 shadow comparisons; 6 mutants, each exit 1 with a delta; gaps stated plainly (4 banks is a power of two, so modulo-versus-bit-slice is indistinguishable at p0) |
| `muldiv.kill_and_edges` | I-012 | case PASS; report pending |
| `iq.wakeup_insert_select` | I-022 | budget confirmed real, refused-insert directed phase added, live-port reads zeroed |

### A mistake of my own, recorded because the file it damaged is this one

I appended the entry above with a tool that **writes** rather than appends, and truncated
this log from 59 539 bytes to 8 639. Recovered from the git index (the entry that was
lost was the one I had just written, and the text was still in hand), then re-appended
with an append. The lesson is the project's own: a tool used for the wrong operation
destroys data without failing, so the recovery path — and the fact that the log is
version-controlled — is what limits the damage. Nothing else was lost.

Stage 0 remains 11 of 239 work packages. In flight at the time of writing: the retire and
recovery repairs, two-wide rename, the MUL/DIV report, the issue-queue meta extension and
its permanent mutant, the lease allocator, the result FIFO, the remote link, the ROB
identity-width correction, the LSU testbench, and the first integrated core.

---

## 2026-10-01 — two gate defects fixed, both of the same shape

**A capability could be advertised with no verification behind it.** `check_coverage.py`
collected each capability's `verify_tasks` and then never consulted them:
`advertisable = all(impl_tasks delivered)`. `config_check.advertised_capabilities()` — the
function that actually decides which bits go into the generated `misa` — had the same
rule. So the moment I-019 and I-020 were recorded, `Zicsr` and `Zihpm` became advertisable
while V-008/V-012 were untouched: the manifest would have claimed an extension whose
independent cases do not exist. Both now require **both** halves (implementation *and*
verification) and print what each capability is waiting on
(`python3 tools/check_coverage.py --verbose`). No capability is advertisable today, which
is the correct answer: nothing in this tree has an independent positive/negative case yet
beyond the packages' own unit cases, and the ladder's verify tasks are the plan's own
statement of what that evidence must be. A capability that names *no* verify task is also
not advertisable — that is a gap in the ladder, not a licence to publish.

**`make lint-cpp` failed with an error that was about ordering, not code.** Each C++
driver includes the Verilator-generated header for its own testbench, and that header
exists only after the case has been built; on a partially-built tree the lint printed
"fatal error: 'Vmosaic_decoder_tb.h' file not found", which reads like a defect in the
C++. The target now skips such a file, *names* it, and prints the count — and `make test`
runs `unit` before `lint-cpp`, so in the gate nothing is skipped. Current output on a
tree with one unbuilt case: `lint-cpp: 23 file(s) clean` plus
`SKIPPED (run 'make unit' first): sim/unit/tb_fifo.cpp`.

Both fixes are the same failure mode the project keeps meeting: a checker that reads part
of its input and reports success, and a failure message that points at the wrong thing. The
first is now impossible to have silently, because the skipped and the waiting-on lists are
printed rather than implied.

State at this point: `make check` green, Verilator lint 25/25 clean, `slang-tidy` 0 errors,
`lint-cpp` clean, manifest generated with 25 sources, 33 registered cases. Ten lanes are
running (retire and recovery repairs, two-wide rename, MUL/DIV, the IQ meta extension and
its eight-mutant sweep, lease allocator, remote link, the ROB identity-width correction,
the LSU testbench, the first integrated core, and the mechanical mutant re-verification of
the three packages recorded above).

---

## 2026-10-01 — case sweep: 22 pass, 11 fail, and every failure is named

`python3 tools/run_unit.py --profile p0 --all`, 33 registered cases:

| state | cases |
|---|---|
| PASS (22) | alu, bringup-vs-reference, commit.head_block_and_dual, completion.fu_collision, csr, decode, fetch, fifo, four harness cases, interrupt, iq, muldiv, predictor, prf, ram, remote.kill_with_delayed_response, rename x2, retire.head_block_and_dual, rob |
| FAIL — module not written yet (7) | arbiter, bypass, fabric.fixed_two_cluster, lsu (testbench), owner_fsm (I-031), steering, wb — all "lists missing sources", i.e. lanes mid-flight, not defects |
| FAIL — real (3) | recovery x2 (free-list-exact phase, cycle 420: the checkpoint bundle's epoch/alloc_ptr diverge from the model's while a commit is in flight), lease.conflict_and_cancel |

The two Stage-1 blockers are now green: **`retire.head_block_and_dual` and
`commit.head_block_and_dual` pass**, and the two-wide rename case passes. So the
Stage-1 list is down to the recovery lane.

**Independent mutant re-verification (results/reports/mutant-reverification-2026-10-01.md).**
A separate lane re-ran every mutant in the three packages recorded above: **20 of 20 exit 1**
with the exact first-mismatch cycle, signal and values their own reports claimed, 26 builds,
0 build failures, 0 mutants exiting 0, and the base case re-run after each campaign is
byte-identical to before it. The interrupt package's model hash (`b54cf9434d46c64e…`) was
reproduced independently. That is the first time in this project that another lane's mutation
evidence has been re-derived end to end rather than taken on report, and it is the standard
the remaining packages' reports should be held to.

---

## 2026-10-01 — the decoder traps five mandatory RV64I instructions

Found by the MUL/DIV lane while extending the decoder for the M word forms, and confirmed
here by reading both sides:

* `rtl/core/mosaic_decoder.sv` decodes OP-32 (`0111011`) as the M word forms when
  `funct7 == 0000001`, and as **illegal** for every other funct7 — unless a mutant flag is
  defined.
* `rtl/core/mosaic_pkg.sv` states OP-32 is "on RV64 only mulw divw divuw remw remuw".
* `sim/unit/tb_decoder.cpp` asserts the same, calling `addw subw sllw srlw sraw` "the
  RV32-only I word forms ... reserved on RV64".

RV64I includes ADDW, SUBW, SLLW, SRLW and SRAW — opcode `0111011`, funct7 `0000000` and
`0100000`, funct3 `000`/`001`/`101`. They are not reserved and they are not "RV32 forms";
on RV32 they do not exist at all. So the shipping build traps five mandatory instructions
as illegal, and the testbench *asserts that behaviour as correct*, which is why the case
has been green since I-010 and why nothing else noticed: the corpus never emits them.

This is the third instance of the same failure shape in this project — a check that
encodes an assumption instead of the specification (the testbench that re-derived the
ROB's own width expression, the capability gate that collected `verify_tasks` and never
read them, and now a reserved-encoding table built from the wrong ISA width). The fix is
assigned to the lane that found it: decode the five forms, correct the package comment and
the testbench model, **delete the now-vacuous `MutRv32Word` mutant and add one that
restores the defect** so the case can catch it by name, re-run decode/muldiv/bringup, and
append a correction to I-010's report rather than rewriting it.

Also recorded, because it will be asked about: the two retire cases currently do not
*link* — `mosaic_rename` gained two outputs (`squash_not_committed`, `ckpt_committed`) for
the checkpoint precondition enforcement, and the retire wrapper does not connect them, so
Verilator stops with PINMISSING. The rename lane has been authorised to make that minimal
tie-off. Until it lands, the I-017 PASS that was observed earlier came from a binary built
before those ports existed, and it is therefore **not** recorded as delivered yet: it will
be re-run once the tree builds.

---

## 2026-10-01 — handoff: what the ladder says the next wave is

Sixteen packages are recorded as delivered. `python3 tools/check_coverage.py --verbose` now
computes, from the plan itself, exactly what each capability still waits on — and it says
something worth acting on:

| capability | waiting on | reading |
|---|---|---|
| RV64I (`I`) | I-008, I-011, I-013, I-021, **V-008..V-012** | the four implementation tasks all have green cases already (their mutant evidence is being independently re-derived in round 2 of `results/reports/mutant-reverification-2026-10-01.md`); the **five V tasks are the whole remaining gap** |
| RV64M (`M`) | **V-008, V-011** | I-012 is recorded; two V tasks stand between it and an advertisable M |
| Zicsr | **V-008, V-012** | same two V tasks |
| Zihpm | **V-012** | one V task |
| Zifencei | I-037, V-014 | I-037 (FENCE/FENCE.I) is Stage 3 |
| A / Zaamo / Zalrsc | I-039, I-040, V-016, V-020 | Stage 3+ |

So the cheapest real progress towards an *advertisable* p0 profile is not more RTL: it is
the V-series harness work, and in particular **V-008 (freeze the canonical architectural
event interface)**, which V-009/V-010/V-011/V-012 and the whole differential path build on.
Much of its machinery already exists and is exercised: `sim/common/event_tap.*`, the ELF
loader, the cycle-limit and signature protocol in the bring-up harness, and
`tools/host_oracle.py`. The remaining work is to *freeze* the interface (fields, semantics,
what is architectural versus observed) and calibrate the harness against it, then re-run
the corpus on the frozen definition. That is a documentation-plus-calibration package with
a re-run as its evidence, not a new RTL design.

Order for the next wave, by dependency and cost:

1. **V-008 → V-010 → V-011 → V-012** (event interface, clock/sampling calibration, ELF and
   memory bounds, signature and termination). These unblock `I`, `M`, `Zicsr`, `Zihpm`.
2. **V-009** (reset and initial state replayable) and **V-013/V-014/V-017** once their I-side
   counterparts land.
3. The recovery lane's completion, then the **rename/recovery cutover** (delete recovery's
   duplicated maps and journal, drive rename's `ckpt_valid`/`squash`, enforce the
   "squash only at the ROB head" precondition in hardware) — this is the integration task
   that turns two half-machines into one.
4. The integrated core (I-023) once the fabric lanes land, then the memory path
   (I-033 testbench → I-034 → I-035) so real programs can run, then the corpus differential
   on the OoO core with the frozen event interface.
5. **I-092** (the RVA23 mandatory matrix) before any RVA23 claim; it depends only on I-001
   and is not started. Everything in §3.2.1 of the implementation plan hangs off it.

Nothing in this list changes the plan's gates; it is the plan's own dependency graph read
in the order that unblocks the most capabilities per unit of work.

---

## 2026-10-01 — I-024 and I-028 recorded; an unexplained helper behaviour reported rather than buried

Both fabric leaves are delivered and independently re-verified here from clean builds:

* **I-024 (resource leases)** — `lease.conflict_and_cancel` PASS, six mutants each failing
  with a named first failure and a delta. The seventh mutant is reported as a **redundancy
  probe that passes identically and is therefore not evidence**; the lane then wrote the
  falsifiable version of the same defect (a dropped FU class) and that one is caught. That is
  the correct handling of a mutant that cannot fail, and it is the fourth time this project
  has met that situation — the first three were reported as gaps, this one was replaced with a
  control that can actually fail.
* **I-028 (remote link)** — `remote.kill_with_delayed_response` PASS with six mutants failing,
  and the header states the recycling rule the card demands: a killed request's entry may be
  reused, so a late response is matched against the pairing table and dropped as stale and
  counted, never delivered into the new occupant.

**One thing I am recording because it is not diagnosed.** The lease lane reports that a
two-argument helper (`lowest_free(occupied, width)`) returned "not found" for an empty pool at
runtime, while reading the generated C++ showed the expected behaviour; it replaced the helper
with a one-argument scan and explicitly does **not** claim a root cause. That is the honest
form of the report, and the reason it matters is that "the tool generated something other than
what the source says" is a claim about Verilator, not about this design — if it recurs, it
needs a minimal reproduction before anything else is built on top of the observation. No
defect is currently open from it: the replacement is tested and the module is green.

---

## 2026-10-01 — I-033 verified by an independent lane, and the two defects were in the testbench

`lsu.size_fault_boundaries` PASS from a clean build: 83 805 checks over 4 118 cycles, 336
loads, 181 stores, 332 memory transactions, 185 misaligned traps, 50 access faults, 517
responses; all four mutants exit 1 with a named first failure and a delta against that base.

The RTL was written here (the integration lead's own file) and verified by a *different* lane,
which is the arrangement the plan wants for exactly this reason: the module was found correct
and was not edited, and both defects it did find were in the testbench — the memory model wrote
the presented payload's low byte instead of the byte in the lane the address selects, and a
hand-written sign-extension expectation used `0x...0080` where `0x...ff80` is correct. Both
were decided from the specification and the module's documented lane convention, not from what
the DUT happened to output, and the report records them as testbench defects rather than
quietly fixing the numbers.

What the case does **not** cover is stated in its own report and matters for the next stage: no
physical memory, one outstanding transaction, and the don't-care fields (write strobes on a
load, read data on a store or a fault) hide any defect confined to them. Those are the seams
I-034 (store queue and commit authorisation), I-035 (load queue and byte forwarding) and I-038
(non-speculative MMIO) close, and each of them changes what "don't care" is allowed to mean.

**Delivered so far: 19 packages** — I-001..I-006, I-010, I-012, I-014, I-015, I-016, I-017,
I-019, I-020, I-022, I-024, I-025, I-028, I-033. Nothing is advertisable yet and that is the
correct answer: the capability ladder requires the independent verification packages
(V-008..V-012 for the scalar base) which are the next wave; V-008 is running now.

---

## 2026-10-01 — I-018 delivered, and the shifting symptom has a cause

The recovery package is green: both cases PASS from clean builds, 419 470 per-cycle
comparisons over 6 810 cycles, nine mutants each failing by name with a delta.

Two things in it are worth more than the green line:

**The changing first divergence was a memory bug, not a model that kept moving.** For several
rounds the integration side saw the recovery failure appear at cycle 52, then 420, then at a
different field, and the natural read was that the shadow model and the DUT were being fed
different inputs. The actual cause: an allocation at the undo bound was accepted while the
journal index wrapped, so the array write went out of bounds and corrupted the heap. It was
found with AddressSanitizer, not by reading, and its signature — a first divergence that moves
when unrelated code changes — is exactly what heap corruption looks like. That is now on the
record as the diagnosis path for this class: when the first divergence moves without the
stimulus changing, stop comparing models and run the simulator under a memory checker.

**Two previously delivered mutants never elaborated.** They had been counted as evidence while
they could not build at all — the same failure the project has met with `-D` blocks that do not
exist, one level earlier: the mutant does not compile, so no one notices it was never run.
They are re-expressed and now fail on the checks written for them, and three new mutants were
added so that every RTL defect fixed in this package has a control that fails on its account.
The rule to carry forward: a mutant's evidence is its build log *and* its exit code.

Also: the credit-kill rule was changed, not just the code — killing by epoch alone discards
older work the squash does not own, which the plan and the architecture review both forbid; the
module's header was rewritten so the documented rule and the implemented one are the same
statement again.

**Still ahead from this package's own analysis**: `mosaic_recovery` continues to own a second
copy of the speculative maps, free list, generation table and journal. The cutover to a single
owner (`mosaic_rename`), with recovery driving `ckpt_valid`/`squash` and enforcing the
"squash only at the ROB head" precondition, remains the integration task it was recorded as —
the green case does not imply it.

---

## 2026-10-01 — session interruption, recovery, and the first commits

The session running this integration was interrupted by a network fault while four lanes were
in flight. Recovery, in order, and what it found:

**What survived.** The working tree was intact: every module, testbench and report a lane had
written was on disk, including the work of a *second* session that had been editing the same
repository. That second session had also configured the git identity and made commits
(`f1dab89` .. `aeadd70`), so the repository had a history rather than one enormous untracked
tree. Both sessions were doing the same jobs — the same integration, the same event contract —
which is worth stating plainly because it means some artifacts were written twice and the ones
on disk are the survivors, not necessarily the ones from the lane whose report describes them.

**What was lost.** Four lanes: the core-integration driver, the event-contract finish, the
rename mutant round, and (dispatched later) the WB hardening. Nothing they had *written* was
lost; what was lost was their running state.

**State at the recovery commit (`6dc7c9a`, "Checkpoint recovery")**, all re-observed here:

| check | result |
|---|---|
| `python3 tools/lint_rtl.py --profile p0` | 31 of 31 source files clean |
| `slang-tidy --std 1800-2017 --single-unit` over every `.sv` | 0 errors (the `mosaic_dispatch.sv` use-before-declaration errors are gone) |
| `python3 tools/check_event_contract.py --profile p0` | OK — 21 fields, 100 octets per record, kinds RETIRE/TRAP/MEM_VISIBLE |
| `python3 tools/check_event_contract.py --negative` | 33 of 33 illegal interfaces rejected |
| `python3 tools/run_unit.py --profile p0 --case wb.same_bank_many_producers` | PASS — but `checks: 1`, 52 cycle comparisons, one mutant define that has never been run, and no report |
| `python3 tools/run_unit.py --profile p0 --case fabric.fixed_two_cluster` | FAIL — the case lists `sim/unit/tb_core.cpp`, which does not exist |

So of 33 registered cases, 32 pass and one cannot build. That one is I-023, and its failure is a
missing file, not a failing design: the RTL for the integrated core (core, cluster, dispatch,
macro_desc, redirect_arb, wb_arbiter) and its SV wrapper `sim/tb/mosaic_core_tb.sv` are in
place and lint clean, and the driver plus the bring-up iterations were what the interrupted lane
had not reached.

**Three things this recovery is carrying forward as rules**, because a session boundary is
where evidence decays:

1. **A commit is not a verdict.** The checkpoint commit says in its own message which case
   cannot build; `config/status/implementation_status.json` remains the only place a package is
   called delivered, and it is updated only after the case has been re-run from a deleted build
   directory in the session that records it.
2. **A case whose mutant has never been built is an untested test.** I-026 is the current
   example: the case is well structured (per-cycle comparison, conservation identities, an
   expectation model that reads rename's documented rule rather than compiling it) but its one
   mutant has no build log and no report, so the package is being hardened before it is
   recorded.
3. **Two sessions on one tree is worse than one session twice.** The duplicate work is
   recoverable; what is not recoverable is knowing which version of a file was the one whose
   evidence was collected. The lanes now running are told explicitly which files they own.

**Re-dispatched after the commit**: the I-023 driver (`sim/unit/tb_core.cpp` + the two-cluster
positive control), the V-008 finish (wire the checker into `make check`, write the derivation
report), the rename mutant round 3, and the I-026 hardening described above.

---

## 2026-10-01 — I-026 hardened, I-013 recorded, and round 3 caught its own vacuous pass

**I-026 (banked WB / value-visible wakeup)** grew from the thinnest case in the tree to a real
one: 52 comparisons over 54 cycles became 956 over 522, eleven phases, 194 completions, 23
same-bank collisions, a full 0..127 generation sweep including the 127->0 wrap, and a phase
where a consumer reads the woken `(tag, generation)` out of the elaborated register file on the
cycle *after* its wakeup — the value-visibility claim observed instead of argued combinationally.
Eight mutants, all rebuilt from a wiped directory, all exiting 1 with distinct named first
failures; independently re-checked here (`SAMEBANK_DROP` exits 1 at cycle 75 with
`offered != published`). No RTL defect was found by the new coverage, and the one limitation is
recorded rather than smoothed: the ready table keys on the numeric generation with no
reallocation input, so a recycled generation number on a live tag answers `written` until the
new producer writes. That is correct under the table's literal contract and is a cross-module
width property, not something this module can fix from its ports.

**I-013 (single-width rename ownership)** is recorded on round 3's evidence: 6 of 6 claimed
mutant rows exit 1 at the claimed cycle and signal, spot-checked here (`NO_GEN_CHECK` at cycle
212, `free_stale: expected 1, got 0`), and the six defines that pass are *expected-inert* in
this case because they target the two-wide path — they are caught by `rename.same_cycle_chain`.
I-014's note now carries two things: round 3 independently confirmed all six of its mutant rows
including the per-row check totals, and two mutants exist in the live RTL that **no package
report claims** (`CKPT_ALLOC_LEAK`, `NO_BOUNDARY_CHECK`) — caught by the cases, documented by
nobody. They are attributed now. Its report's section 5 base figures are stale by two phases
(9750/9818/16 against the live 9767/9843/18) and its recorded sha256 prefixes no longer
reproduce; that is drift to be noted, not re-pinned, and the mutant claims it rests on are the
ones that were checked.

**The process finding worth keeping.** Round 3's first pass appended short mutant tags
(`-DX0_ALLOC`) instead of the full macro names, so every row silently ran the *shipping* build
and passed. The `build_command.txt` stamp — written precisely so a mutant cannot masquerade as a
rebuild — showed no `-DMOSAIC` token, the pass was discarded and the campaign re-run with the
full names, and the whole episode is in the report. This is the third distinct way this project
has seen a "passing" mutant mean nothing (a define that does not exist in the RTL, a define that
does not elaborate, and now a define that never reached the compiler), and the stamp caught all
three. The rule stands: a mutant's evidence is its build command, its binary hash, **and** its
exit code.

---

## 2026-10-01 — the core runs: I-023, I-034, I-035-in-flight, and the four defects that were waiting

The integrated machine executes programs now. `fabric.fixed_two_cluster` (17 retires, 8 ALU uops
per cluster, uop 13 completing 65 cycles before uop 12 while retire order holds) and
`core.corpus_branch` (8 907 comparisons over 1 783 cycles: 96 control transfers, 48 branches of
which 26 taken, 27 JAL, 21 JALR, 35 back edges) both pass from clean builds. Four defects were
found by that work, every one of them of the kind that hides behind a passing directed test:

1. **The decode buffer swapped the first two instructions of every program.** Its push wrote
   slot 0 whenever nothing was popped, overwriting a live entry the pop had left in slot 1; the
   observed allocation order was 0,2,1 and the machine retired two instructions out of program
   order on its first real run.
2. **Rename's committed map was fed the ROB entry generation instead of the physical tag's
   generation.** Same width, different counter, so `speculative map == committed map` — the
   invariant every redirect and the whole I-018 checkpoint path rest on — was false from the
   first commit onward. It had never been observed because nothing had ever compared the two.
3. **The redirect arbiter only looked at retire lane 0.** With a two-wide retire a branch that
   leaves in lane 1 never matches the head, its request stays pending for ever, the branch
   barrier is never released and the core deadlocks after the *first* taken branch of the
   program. This is why "make the second cluster real" and "retire two per cycle" cannot be
   tested separately.
4. **Dispatch read both operands through one port per bank.** When the two source tags shared a
   bank the second operand silently became the first and the comparison's value-ok test accepted
   it: `beq a0,a1` with `a0 = 0x8000000000000000` and `a1 = 0x7fffffffffffffff` resolved
   *taken*. A same-bank operand conflict is not a performance detail; it is a wrong answer.

The fetch output register was wrong in two ways that produced an unbounded stream of one
wrong-path PC retiring — and the bring-up case's *own reference model* had the same defect
(it asserted the defective behaviour at cycle 166), which is the reminder that an independent
model is only independent if someone checks it against the specification rather than against the
DUT.

**I-034 closed the first half of the memory path**: the speculative store queue passes with
299 262 checks over 3 331 cycles, 112 stores allocated, 82 squashed, 24 drained, and seven
mutants covering both of the card's fail modes. The wrong-path requirement is checked against the
memory side, not the queue: a squashed store produces zero memory transactions. One contract
change is on the record — the authorisation watermark is now derived from the cycle's survivors
rather than adjusted arithmetically, because the soak produced a commit and a squash of the same
entry in one cycle, which the arithmetic form cannot express. Two testbench defects of the same
class were found by the soak and both were the *testbench's* fault, not the RTL's: the lane
convention describes the bus, not the memory, and every directed phase had started its stores at
lane 0.

**And one open defect, reported rather than fixed**, because it needs its own case and its own
reproduction: reading an architectural register that has not been written since reset **deadlocks
the machine**. Rename maps it to a tag/generation the writeback ready table never marks written,
so the issue queue waits for a wakeup that cannot arrive. `p02_branch` writes every register it
reads, which is exactly why the case passes. That is the next lane, with a reproducer that must
fail before the fix and a mutant that reintroduces it afterwards.

**Status: 32 of 239 work packages delivered** (I-001..I-009, I-010..I-026, I-028, I-033, I-034,
V-008, V-009, V-011), with V-012 and I-035 running and the readiness fix dispatched. Nothing is
advertisable yet: `M` waits on V-011's sibling V-010, `I` on I-013-adjacent verification, and the
ladder is deliberately the last thing to move.

---

## 2026-10-01 — the first advertisable capabilities, and what is still withheld

`python3 tools/gen_manifest.py --profile p0` now writes `isa_string: rv64m_zicsr_zihpm` with
`advertised: M, Zicsr, Zihpm`. Until today the ladder reported `advertisable now: (none yet)`
for the whole project, so this is the first time a capability has been allowed into misa, the
compiler's `-march` and the reference model — and it happened the way the plan intended: not
because someone decided the machine was good enough, but because every implementation task and
every verification task the ladder names for those three capabilities appears in
`config/status/implementation_status.json` with evidence behind it.

The withholding that remains is the interesting part. The base integer set `I` — the capability
without which the other three are meaningless — is still **not** advertised, because V-010 has
not been delivered. Everything else for `I` is done. That is the ladder working exactly as
designed: an ISA string that reads `rv64m_zicsr_zihpm` without an `i` looks wrong, and it is
right to look wrong, because the harness's one-handshake-one-record discipline has not been
demonstrated at the time of writing. A claim about `I` is a claim that the machine's event
counts are exact, and that claim belongs to the sampling loop, not to the RTL. V-010 was
dispatched with that sentence in its brief.

Two more notes for whoever reads this next:

* **`I` waits only on V-010; `Zifencei` waits on I-037 and V-014; `Zicntr` on I-076; the A/S/C
  families on tasks that have not started.** So the p0 scalar base is one package away from being
  fully advertisable, and the next layer (the ordered memory path — I-035 running, I-037,
  I-038) is what stands between this machine and running the corpus end to end on the
  out-of-order core rather than on the bring-up core.
* **The manifest is generated, not authored.** `build/p0/manifest.json` is the single file the
  DUT build, the compiler flags and the reference model all quote, and it is derived from the
  ledger plus the ladder. That is why "record the package" and "advertise the capability" are
  the same act in this project, and why the records in `implementation_status.json` are written
  the way they are — with the gaps stated in the same breath as the passes.

---

## 2026-10-01 — the scalar base is advertisable: rv64im_zicsr_zihpm

`python3 tools/gen_manifest.py --profile p0` now writes `advertised: I, M, Zicsr, Zihpm` and
`misa reset: 0x8000000000001100`. V-010 was the gate, and it earned it: the case runs a program
three ways (zero latency and two item-by-item latency plans) and requires **three independent
tallies to agree exactly** — the reference model's events, the event tap's records and the fetch
accepts — with a built-in detector for a record emitted twice for the same `(retire_seq, pc)`
and for a gap in the sequence numbers. Four harness mutants, including the double-eval one the
detector exists for, exit 1 with their own named first check.

Two findings from its audit belong in the record because they are about the whole tree, not
about the case:

* **`dut.final()` executes no design code anywhere.** Nine of thirty drivers never call it, and
  no `sim/tb/*.sv` contains a `final` block, a `timescale`, a delay, an `initial` block or
  `$finish`. So the card's "missing final before the end" fail mode has no instance to catch —
  which is a statement about this harness, not a pass: the end-of-run assertion discipline holds
  for the cases that implement it themselves and for no others.
* **`--timing` is enabled nowhere in the tree**, so "enabled without advancing pending events"
  has no instance either; the case asserts Verilator's `time() == 0` and `eventsPending() ==
  false` instead of pretending to test a mode the build never enters.

The deviations it found in other drivers (`tb_harness.cpp` evaluating twice in the high phase,
`tb_wb.cpp`'s extra low-phase eval, the combinational alu/decoder cases with no clock at all)
were reported rather than rewritten, and each is argued to be incapable of double-counting —
idempotent eval with no input change and no sample taken between the duplicates.

**Where the project stands: 35 of 239 work packages** (25 implementation, 10 verification), the
p0 scalar base advertised, and the next layer in flight: the memory path
(`mosaic_lsu_endpoint`, `mosaic_store_queue`, `mosaic_load_queue`) is verified module by module
and is now being wired into the out-of-order core so that real corpus programs — loads, stores,
byte accesses, misaligned faults — run on the machine itself rather than on the bring-up core.
That integration is what stands between "an out-of-order core that executes arithmetic and
branches" and "a core that can run the corpus", and after it come FENCE (I-037), MMIO (I-038),
CSR/trap integration, and only then the privileged and vector stages.

---

## 2026-10-01 — the out-of-order core runs the corpus: memory path integrated

`core.mem_program` passes from a clean build: two corpus programs (`p03_loadstore`,
`p09_storeload`) under three input patterns, 9 543 comparisons over 1 017 cycles, 306 retires,
63 loads, 84 stores, 147 data transactions, 96 bytes forwarded from the store queue, and each
program's signature matched **against `tools/host_oracle.py`'s independent host computation**
rather than against the DUT. The four core cases and the three memory-module cases all pass
together, which is the first time the machine has been exercised as a whole.

Two defects were found by that work, and both are worth naming because of where they hid:

* **Dispatch captured stale operands for memory macros.** The cluster path's operand-ready test
  is satisfied by the issue queue filling a not-ready uop later, but a *memory* macro captures
  its operands at insert. A store whose base register had just been written therefore took the
  register file's stale content and stored to address 0 instead of TOHOST — the program ran, the
  store executed, and it wrote the wrong place. The check that caught it was the corpus case's
  exit-protocol condition, not a directed memory test.
* **The store queue's commit counter incremented twice in one cycle.** Two separate non-blocking
  assignments to the same signal when both commit ports fired meant the second overwrote the
  first: 14 authorisations counted as 12. A counter that is *nearly* right is worse than one that
  is obviously wrong, and only a conservation identity checks the difference.

The controls that make the integration's claims mean something are the two that would otherwise
be argued in prose: `MOSAIC_CORE_MUTANT_STORE_PRECOMMIT` (a store authorised at allocation
instead of retirement, so it reaches memory before it retires) and
`MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH` (a memory macro bypassing the branch barrier, so the
wrong-path load at the entry branch's fall-through is issued) — both fail by name, both are the
precise behaviours the plan forbids.

**What is still missing from the core, in the lane's own words**: the CSR/trap path (which is
what `crt0`'s boot sequence, `p08_misaligned` and `p13_romstore` need — `p13` is simply not
runnable without it), store-to-load forwarding is exercised through the core only by `p09` (in
`p03` the stores have already drained when the loads complete), the load queue's
blocked-on-unknown-address and replay paths are never entered because dispatch reads every store
operand before allocating, and no faults, misalignment or FENCE ordering are exercised at all.
That list is the next wave's specification, and the CSR/trap integration is running now.

---

## 2026-10-01 — the out-of-order core runs the trap-dependent corpus

`core.trap_csr_program` passes from a clean build: **428 642 retires, 26 traps, 17 144 009
per-cycle comparisons**, and the two corpus programs that genuinely need traps — `p08_misaligned`
and `p13_romstore` — run from the reset vector through crt0 on the out-of-order machine, each
under three input patterns, with every signature matched against `tools/host_oracle.py`'s
independent host computation. The trap taxonomy is real rather than directed-only (ECALL,
EBREAK, an illegal CSR read, a read-only CSR write, a misaligned load, csrrwi, `mstatus` WARL),
and five interrupt scenarios are driven, including WFI halt-then-wake and a masked-by-MIE case.

The controls include the two that a trap implementation most often gets wrong, and they are
mirror images deliberately: `TRAP_EPC_NEXT` (a *fault*'s `mepc` pointing at the instruction after
the faulting one) and `IRQ_EPC_NEXT` (an *interrupt*'s `mepc` pointing at the interrupted
instruction instead of the next one). A machine that writes one rule for both passes every
directed test that only checks "a trap happened"; these two mutants fail by name, and the
distinction is the reason the case drives both classes.

Two expectation relationships are stated rather than claimed as raw oracle equality, and both are
checked rather than assumed: p08's third signature is the oracle's with the eight-record fold that
`trap.S`'s restore order produces, and p13's differs by one bit because the armed-trap recovery
resumes at `mepc+4`, so the marker instruction after the store executes. Those are the honest
forms of "the machine matches"; the dishonest form would have been to relax the comparison.

**Where the machine is now.** It executes arithmetic, control flow, loads, stores, CSRs, traps
and interrupts on real corpus programs, verified end to end against an independent host oracle,
with every capability claim gated by a ledger that names what is *not* covered in the same breath
as what is. The scalar base is advertised (`rv64im_zicsr_zihpm`). What is missing is ordering —
there is no `FENCE` or `FENCE.I` anywhere in the machine — and that package (I-037) is running,
followed by MMIO (I-038), the fabric remainder (I-029..I-032), and the widening of the corpus
sweep from the handful of programs each case runs to all of them.

---

## 2026-10-01 — the whole p0 corpus on the out-of-order core, and what that does and does not prove

`core.corpus_sweep` passes from a clean build: **13 programs x 3 input patterns = 39 runs, 39 PASS, 0
FAIL, 0 STOPPED, 17 484 988 cycles, 2 774 478 retires**, every signature compared against
`tools/host_oracle.py`'s host computation. The sweep prints the full matrix and states the SHA-256
of every RTL file it compiled. Its failure path was exercised, not assumed: a deliberately
perturbed expectation makes it report a mismatch at the right first divergent word. Two
independent evidence paths now agree on the two trap-dependent programs (`p08_misaligned`,
`p13_romstore`): the sweep and the trap case.

The machine this runs on is worth stating plainly, because three hours ago it could not run a
branch: fetch, decode, rename, dispatch, two clusters, banked register file, scoreboarded
writeback, reorder buffer, two-wide retire, recovery, LSU endpoint, speculative store queue, load
queue with byte forwarding, CSR file, traps, interrupts, `FENCE` and `FENCE.I` — with the corpus as
the oracle and a per-slot retire comparison (V-013) watching the parts a final signature cannot
see.

**What the sweep cannot see, in the lane's own words and mine**: it compares signatures, so a
defect that corrupts intermediate architectural state and is overwritten before the end would pass
it. That is exactly why the project has V-013's per-slot delta comparison (1195 checks, five card
situations produced with exact cycles, x0 asserted across 56 directed instructions, trace-gap
detector, five failing controls and three invisibility probes documented with reasons) and the
V-008 event contract (frozen schema, three producers, 33 negative controls). Neither substitutes
for the other, and a project that had only the sweep would be able to say "39 of 39 pass" while
being wrong about the machine.

**V-013's three findings are the proof that the verification is doing work.** It reported, and
deliberately did not fix: `LWU` is refused by the decoder although RV64I requires it (a real ISA
compliance gap, with the source comment that justifies the refusal being wrong); the core ties the
retire instance's store and CSR payload to constant zero, so the event stream cannot carry the
fields the frozen schema declares — the F-4 gap named at freeze time, now measured; and a
system-instruction trap publishes no event at all, so a consumer of the stream sees a PC jump with
no cause. A verification package that finds nothing on a machine this young is usually not
looking; this one found three things, named them, and left them to a package that owns the fix.
That package is running now, together with the corpus sweep's re-run after it lands.

---

## 2026-10-01 — state of the machine, for whoever picks this up next

Recorded after a session that started by recovering from an interruption and ended with the
out-of-order core running the whole p0 corpus. Everything below is re-derivable from the repository
(`config/status/implementation_status.json` is the ledger; `tests/unit/registry.json` is the case
registry; `results/reports/` holds one report per package; `results/PROGRESS.md` is this file).

**Delivered: 39 work packages; 49 registered cases; advertised ISA: rv64im_zicsr_zihpm (misa reset 0x8000000000001100).**

| Area | What is true now |
|---|---|
| Scalar base | `rv64im_zicsr_zihpm` advertised. Every implementation and verification task the ladder names for those four capabilities is recorded with evidence. |
| The core | Fetch, decode, rename, dispatch, two clusters, banked register file, scoreboarded writeback, ROB, two-wide retire, recovery, LSU endpoint, speculative store queue, load queue with byte-perfect forwarding, CSR file, traps, interrupts, `FENCE`/`FENCE.I`, non-speculative MMIO. |
| End-to-end | `core.corpus_sweep`: 13 programs x 3 inputs = 39 runs, all matching `tools/host_oracle.py`; 2.77 M retires. `core.trap_csr_program` runs `p08_misaligned` and `p13_romstore` from the reset vector through crt0. |
| Precision | V-013 compares per-slot retirement against an independent program-order model (1195 checks, five card situations, x0 across 56 directed instructions, trace-gap detector). V-014 (precise trap state) is running. |
| Interface | `config/contracts/event_v1.json` frozen; three producers agree; 33 negative controls. |
| Gates | `make check` green (config, contracts, event contract, records, coverage, capability matrix, upstream, isolation); lint 34/34 clean; `slang-tidy` clean; `tools/check_records.py` green. |

**What is NOT true, and must not be read into the table above.** No capability beyond
`I/M/Zicsr/Zihpm` may be advertised: `Zifencei` waits on V-014 (running), `Zicntr` on I-076, the A
and C families on packages not started, S-mode on I-044/I-047/I-048. There is no cache, no
multi-hart, no FP, no vector, no hypervisor, no lockstep, no ASIC/FPGA evidence. The
differentiated part of this project — dynamic ownership, aggregation, cohort execution — has
modules and cases (lease, remote link, result FIFO, WB arbiter, steering not started) but no
integrated demonstration, and the plan is explicit that the dynamic/fixed comparison has to be
measured on equal resources rather than inherited from a paper. The fabric remainder (I-029
steering, I-030 quota and forward-progress, I-031 ownership FSM, I-032 bank-aware allocation) and
the bypass's integration into the live cluster are the next implementation work after V-014;
Stage 3 continues with AMO (I-039), LR/SC (I-040), the C extension (I-041) and then S-mode and the
p1/Linux contract (I-044..I-048).

**Three process facts that this session earned the hard way**, for the next agent:

1. **Two lanes in one core file costs more than it saves.** Two collisions this session: one lane's
   in-flight core edit left the wrapper's port list unfinished, which made six *passing* cases fail
   at elaboration, and one commit of mine swept another lane's in-flight RTL into a commit labelled
   for a different package. Both were caught and both are recorded, but the rule is now: name the
   file owners in the brief, and commit only what you have verified.
2. **Every number in the ledger has a command behind it, and the number in the *record* is the one
   observed.** Two packages this session reported a check count that differed by one from the
   RESULT line, and both times the observed figure went into the ledger with the discrepancy noted.
   A record that quotes a lane's summary instead of the tool's output is how a project starts
   believing its own prose.
3. **The most valuable packages this session were the ones that found something.** LWU refused by
   the decoder, the event payload tied to zero, ECALL publishing no event, the response owner
   recorded at the wrong moment, a reset latch surviving a restart, a decode buffer swapping the
   first two instructions, rename's committed map fed the wrong generation counter. None of those
   were caught by a program passing; all of them were caught by a case that compared a *rule*
   against a model written from the specification.

---

## 2026-10-01 — the I-041 regression: how the tree went red, how it was contained, and what to do differently

**What happened.** Two lanes worked the same tree (I-039 adding the A extension's atomic path, I-041
adding RV64C decompression and length plumbing). I-039 delivered; I-041's first attempt did not —
it landed RTL that made `core.mem_program`, `core.corpus_branch`, `core.corpus_sweep` (all 39 runs)
and `core.trap_csr_program` stop with `illegal=1 unsup=1`. To protect the baseline I stashed I-041's
edits — and the stash caught `mosaic_pkg.sv`, `mosaic_uop_pkg.sv` and `mosaic_decoder.sv`, which
**both** lanes had edited, so the AMO path stopped building too. Two hand-restores (`OP_AMO`,
`MEM_AMO`, `amo_op_e`, the `lsu_req_t` AMO fields, plus the AMO lane's two-line `amo_op:` arm in
`CTL_ILLEGAL`) brought the tree back to a state where everything except I-041 passed, and the second
attempt restored the stashed work, fixed the defect, and delivered.

**What the defect actually was**, because it is a good one: a *speculatively fetched reserved
encoding* — the zero padding behind the corpus text's final `jal` — reached dispatch and stopped the
machine before the still-in-flight jump could redirect and squash it, and the redirect itself was
gated on `!stopped`, so the deadlock was complete. The fix is the right shape rather than a patch:
dispatch now *holds* an undecodable macro while a transfer is in flight, so the redirect can purge
it, and a transfer that turns out not to be taken still stops the machine. No check was weakened:
the reserved-encoding cases still stop at their encodings, and `core.ready_*` still stops at its
architectural ECALL.

**Two further defects fell out of finishing the package** — the decoder's CI-format immediate read
the destination register as the immediate (`c.addi x6,1` added 6), and a stale device attribute in
the serializer broke `mmio.exactly_once`, inherited from the AMO lane. Both were found because the
package had to make real programs work, not because a directed test looked for them.

**Three process lessons, now rules.**

1. **A stash is not a lane-private thing.** `git stash push -- <paths>` takes the *current* content
   of those paths, whoever wrote it. When two lanes share a file — and `mosaic_pkg.sv`,
   `mosaic_uop_pkg.sv` and `mosaic_decoder.sv` are shared by construction, since they are the type
   and encoding dictionaries — there is no pathspec that separates them. The correct containment is
   to commit the *good* lane's work first, then stash or revert the failing lane's hunks; that is
   the opposite order from the one I used.
2. **A failed lane owes the tree a green baseline, not a diagnosis.** I-041's first attempt left the
   tree non-elaborating and four cases red while it wrote an honest report. The report was worth
   having, but the ordering was wrong: revert first, report second, and only then re-attempt.
3. **The two-strike lesson from CompressedRetry.** The second attempt succeeded *because* the first
   attempt's work was preserved intact and its hypotheses were written down (the dbuf push, the
   PC-advance/response-model interaction). Both hypotheses were wrong, and the instrumentation the
   first lane recommended is what found the real cause. An honest, detailed failure is a reusable
   asset; a tidy failure with no hypotheses would have cost that attempt's 34 minutes twice.

**State after this stretch**: 45 packages delivered, advertised `I, M, Zicsr, Zifencei, Zihpm, C`,
38 RTL sources clean under both tools, `make check` and `check_records.py` green, and the whole p0
corpus passing on the out-of-order core. In flight: I-040 (LR/SC, which completes the A extension's
implementation) and I-031 (the ownership-change FSM). The A extension then waits only on its
verification packages V-016 and V-020.

---

## 2026-10-01 — state after the fabric and atomics waves (49 packages)

Advertised ISA: **rv64im_zicsr_zifencei_zihpm** (misa reset 0x8000000000001100). 58 registered cases, 49 delivered packages,
`make check` and `check_records.py` green, lint clean over every RTL source under both tools.

**Since the last summary**: the fabric chain is complete at module level (I-027 bypass, I-029
steering, I-030 bounded-service arbiter, I-031 ownership FSM, I-032 bank-aware allocation), the A
extension is complete (I-039 AMO, I-040 LR/SC — `Zaamo` and `Zalrsc` are advertisable and `A`
waits only on V-020), C is advertised (I-041, delivered on its second attempt after the regression
described above), and V-016 verified PC/fetch/FENCE.I. Non-hardware progress is now **49 of 188**
I+V packages.

**What is structurally important about the fabric modules**: all five are verified *in isolation* —
each has its own case and its own mutants, and none is instantiated by the core yet. The integration
that wires them in (and the arbitration/steering policy the core actually uses) is the plan's
I-090/V-034 territory and is explicitly **not** claimed by their records. Saying that plainly
matters more than the five green lines, because a reader could otherwise conclude the machine
schedules dynamically today; it schedules with an alternating toggle and a fixed baseline, and the
plan requires the dynamic/fixed comparison to be measured on equal resources before anything is
claimed about it.

**In flight**: I-044 privilege/PMP (attempt 2 — its RTL and config landed on attempt 1, and the
config work is real: `mstatus` gained its writable fields, `medeleg`/`mideleg` were narrowed, a
PMP config block and a `config/csr/mode_pmp.json` were added, and `gen_manifest` learned to merge a
register declared in two mode blocks, which is what had made `--profile p1` fail outright) and
I-049 (the F/D datapath, dispatchable because the card permits a declared subset as long as nothing
silently falls back to software).

**What the next wave is**: finish I-044, then I-050 (FP state and precise `fflags`), I-045/I-046
(Sv39 walker and TLB/SFENCE.VMA, which need I-042's caches first), the cache and MSHR work
(I-042/I-043), then the SoC and the p1 Linux contract (I-047/I-048). Stage 4+ (vector, multi-hart,
cohort, the measured dynamic/fixed comparison, the RVA23S64 matrix) is untouched: it is the larger
half of the plan and the half that justifies the project's name.

---

## 2026-10-01 — the p1 machine: privilege, virtual memory, atomics, and a ledger of what it does not prove (58 packages)

The p1 profile now declares **M/S/U privilege, PMP and Sv39 translation**, and the manifest advertises
`rv64acim_zaamo_zalrsc_zicsr_zifencei_zihpm`. That is a Linux-shaped machine: it can enter S-mode with `mret`, enforce PMP with real access
checks, walk a three-level page table with an explicit A/D policy, cache translations in a TLB with
exact `SFENCE.VMA` invalidation, take precise traps from all of it, and run the whole p0 corpus while
doing so.

**The package that matters most in this stretch is V-020**, because it is the one that says what the
rest does *not* prove. It records 52 exclusions (20 of them open, by the checker's own count), each with a reason, a specification basis,
an alternative verification that must exist **and pass**, and an end condition — and its checker
rejects any difference that no ledger entry permits. Its own list of the largest open exclusions is
the most useful paragraph this project has produced, and it is now machine-readable:

1. **No ACT4 ELF runs for any advertised p1 extension** (A, C, Zicond, Zmmul, CMO). Every
   "independent" expectation in this repository is a model written *here*. That is the deepest
   limitation on all 58 green lines, and V-043 is the package that addresses it (running now).
2. **`mepc[1:0]` was masked while p1 claimed C** — a contradiction inside an advertised capability,
   found by two different lanes and finally tracked by the ledger. The fix is in flight.
3. **No external reference** (NEMU, Sail, XiangShan) has ever been compared against this core.
4. Four holes in the event interface (no MEM_VISIBLE producer, no interrupt event, three fields
   absent, store payload unwired).
5. `p13`'s store-to-read-only trap has no independent oracle because only the DUT models the ROM as
   read-only.
6. One profile, one seed, no device watchdog.

**What the last stretch delivered**, in the order it landed: the A extension (AMO + LR/SC, then its
verification), C (delivered on the second attempt after a regression that is written up in this file),
the fabric modules (bypass, steering, quota/arbiter, ownership FSM, bank-aware allocation — each
verified in isolation and **none yet instantiated by the core**, which their records say explicitly),
the blocking L1 caches (module-level for the same reason), privilege/PMP (third attempt, after a real
defect where a PMP-refused store was counted rather than trapped), the CSR rule ledger, the memory
visibility provenance case (740 bytes each traced to a named committed producer), the Sv39 walker, the
TLB, the MMIO side-effect model, the FPU (self-implemented, `fdiv` correctly rounded in five modes,
`fsqrt` declared absent rather than faked) and the exclusion ledger itself.

**The next wave** is ACT4 against the integrated core (V-043), the IALIGN fix, FP state and precise
`fflags` (I-050), the caches in the path (I-043), the SoC and the Linux contract (I-047/I-048), and
then Stage 4+: vector, multi-hart, cohort, the measured dynamic/fixed comparison and the RVA23S64
matrix. Those last items are the larger half of the plan and the half that justifies the project's
name; nothing in this file should be read as claiming them.

---

## 2026-10-01 — an external suite runs against the real core, and finds five real bugs

The architectural test suite (ACT4, at a pinned revision, with a MosaicRV p1 configuration
checked in under `tests/act4/mosaic-p1/`) now runs **against the integrated out-of-order core**
rather than the bring-up model. N is closed against its own generation manifest — 127 applicable,
127 generated, 127 run — and 47 excluded suites are each named with a reason rather than dropped.
The PASS macro is calibrated: a corrupted expected-signature byte in `I-add-00` makes the case exit
1, and an ALU mutant fails `I-slt-00`, so the suite can fail and its passes therefore mean
something. Three *harness* defects were found and fixed on the way — a fetch image that was
4-byte-aligned-only (which had been hiding every Zca test), a static fetch image invisible to
`fence.i`, and fetch reads shadowing `MOSAIC_TOHOST` inside the ELF text.

**122 of 127 pass. The five that fail are real divergences in advertised extensions**, located by
diffing the signature-mode ELF against Sail's `.results` — the expectation is Sail's, not ours:

* **`c.lui` places its 6-bit immediate 16 bits too high** (Zca, 51 signature words).
* **SC returns failure where the reference expects success**, at both widths (Zalrsc, 23 words).
* **HPM `csrrc`/`csrrs` read-modify-write diverges** (Zihpm, 357 words).

This is the most important event in this file's recent history, and it is worth saying why plainly.
Every previous green line in this project rested on a model written **here**: the host oracle, the
reference interpreters, the expected-event tables. V-020's ledger said so in as many words ("no
independent reference has ever been compared against this core"), and the first external suite to
run found three defects in three different advertised extensions within minutes of being pointed at
the machine. The conclusion is not that the project's own cases were worthless — they found dozens
of real defects, including several that would have made the machine deadlock — but that
self-consistency is a weaker property than correctness, and the difference is measurable.

Two consequences are already recorded rather than promised: `core.act_dut` is a **registered case
that reports FAIL** until the five divergences are fixed (that is the honest state, and the suite is
not to be narrowed or skipped to make it green), and each fix owes a paragraph on *what this
project's own cases were missing* that let the bug through — for `SC` in particular, `lrsc.
reservation_progress` passes today, so the ACT4 failure is telling us that case's coverage has a
hole rather than that the machine is broken in a way we already knew about.

---

## 2026-10-01 — two hygiene defects, one durable fix, and a record that stops lying

The p1-only lint failure and the "sstatus has no known role" crash are both fixed, and one of them
produced the kind of fix this project wants:

* **The unused `store_q0`/`store_q1` in `mosaic_pmp.sv` were dead weight**, not a missing driver —
  the store-commit port exposes only its two allow outputs, so the shared query struct's other
  fields were computed and never read. They are deleted, with a mutant
  (`STORE_COMMIT_UNGATED`) that exits 1 with `mcause=9 where 7 is required`, and the lane verified
  the fix by temporarily restoring the pre-fix shape and watching lint fail again.
* **Every CSR now declares a `role` from a closed set**, the role is emitted into the generated
  descriptor, and `check_profile` *rejects* a configuration whose CSR has no role or an unknown one
  (with negative controls observed for p0 at 50/50 and p1 at 37/37). That is the durable form: a
  configuration that cannot be ambiguous, rather than a case that catches the ambiguity later.

**Two registry-level changes came out of it**, both of which fix problems this file had recorded
but not solved:

1. **Cases can declare the profiles they belong to.** `csr.precise_trap_mret` is machine-mode-only
   by construction (a hand-derived 21-row oracle, `mstatus.MPP==3` and `IALIGN=32` as standing
   invariants, an M-only shadow) while p1's machine has 50 CSR rows and real S/U privilege — so
   running it under p1 tests a model that does not exist. It is now `"profiles": ["p0"]`, as is
   `core.trap_csr_program`; the runner skips such a case under `--all` and refuses it explicitly
   when asked for directly, and the records checker validates the field. Writing the supervisor-mode
   CSR model is a verification work package, not hygiene, and the lane correctly refused to smuggle
   it in.
2. **Results are per profile.** Until today `results/unit/<case>/result.json` was overwritten by
   whichever profile ran last — the lane's own first p1 run clobbered the p0 PASS that EX-009 and
   EX-025 cite as their evidence. p0 keeps the historical path (so every existing citation stays
   true) and every other profile now writes `results/unit/<case>/<profile>/`. Verified by running
   p1 and checking the p0 verdict is untouched.

The second one is worth a sentence of its own, because it is the failure mode this whole file exists
to prevent: a *passing* run silently destroyed the record another package relied on. No test failed,
no gate went red, and the evidence would have been gone. The fix is small; noticing it required a
lane that read the ledger instead of only running its own case.

---

## 2026-10-01 — three fixes from ACT4, and the reason our own cases could not have found them

The external suite's five divergences are now three: `c.lui`'s immediate shift and the SC
reservation disagreement are fixed (ACT4 125/127), and the HPM `csrrs`/`csrrc` divergence is in
flight. The fixes are the smaller half of what this stretch produced. The larger half is the
answer to "why did none of our cases catch these?", and it is uncomfortable in a way worth keeping:

**`c.lui`: our case was written around the field that had already failed.** `compressed.cross_boundary`
was built after the CI-format immediate bug (`c.addi` reading the destination as the immediate) and
it pinned *that* field position. The same encoding format has other field positions, and `c.lui`'s
`nzimm` at `rd[17:12]` was never compared against an independent expectation. A case written in
response to a bug tends to test the bug, not the format.

**SC: our case and our RTL agreed with each other, and both were wrong.** `lrsc.reservation_progress`
derived its in-granule and out-of-granule addresses from the RTL's own `GRANULE_BITS` constant — so
the case could not disagree with the design about the granule, which is the one thing a reservation
case exists to check. It also never issued an SC at an address *different* from its LR, which is the
only observable the declared reservation size controls. The platform declares
`reservation_set_size_exp = 3` (8 bytes); the machine used 64. Every check passed, because every
check was derived from the thing it was checking.

**Zihpm: the ledger said a behaviour was verified that no case mentions.** EX-009 claimed HPM
read-only-zero behaviour was "verified by the named CSR cases", and no case or table mentions
`hpmcounter` at all — while `csr.precise_trap_mret` explicitly asserted that `0xb03`/`0xc03`/`0xc04`
raise illegal instruction. So the project's own case asserted the *absence* of the CSRs its
advertised `Zihpm` requires. This is the single most useful thing the exclusion ledger has caught so
far, and it is exactly what V-020 was written for: not a failing test, but a *claim* that had drifted
away from anything real.

**The generalisation, which is now a rule**: an expectation derived from the design under test is
not an expectation. This project has met the same failure in four guises now — a reference model
that had copied the DUT's drain bug, a mutant whose define never reached the compiler, a granule
constant read from the RTL, and a ledger entry naming coverage that did not exist. The countermeasure
that actually works is an *external* oracle: Sail, in this case, found in minutes what four hundred
thousand of our own checks could not, precisely because it does not share a single constant with the
design.

Two consequences are recorded rather than promised: the mutants for the two fixes are still owed
(the lane left none, and the next lane is adding them), and the HPM fix will either make `Zihpm`
true or cause it to be un-advertised — an advertised extension the architectural suite fails is worse
than one that is honestly withheld.

---

## 2026-10-01 — I-050's second attempt: two real defects, and a rule that had been passing vacuously

The FP state package is not delivered yet (attempt 3 is running), but its second attempt found
things worth more than the green line it did not produce:

* **`csrw frm` silently did nothing.** The CSR write took the operand's bits `[7:5]` — `frm`'s place
  *inside* `fcsr` — instead of `[2:0]`, so a program that set the rounding mode by writing `frm`
  directly kept whatever mode was already there. A wrong rounding mode is a wrong answer in the last
  bit of every result, and nothing in this repository would have noticed.
* **`fadd.s f3,f1,f2` computed `f1+x2`.** `fp_s2_is_fp_c` was never set for OP-FP, so dispatch
  resolved the *second* FP source through the integer map. The first FP operation with two register
  operands was reading an integer register for one of them. Both defects are fixed and verified
  (f1+f2 = 3.0, f2+f1 = 3.0, f1−f1 = 2.0, f2+f2 = 4.0, including from load-produced operands).
* **The `csr.rule_ledger` failure was a vacuous stimulus, not a broken rule.** The V-017 driver used
  `ld`/`sd` displacements of `8*index`, which overflow the signed 12-bit immediate for indices ≥ 256:
  the `fflags` example loaded from unwritten RAM and stored its read-back where the harness never
  looked. The driver is fixed with two 256-entry windows and a permanent assembler displacement
  guard — **and the same bug had made the `hpmcounter` rules pass vacuously**, which is the second
  time this project has found a rule whose stimulus never reached the DUT.

That last one is the lesson of the day and it belongs next to the ACT4 one. An expectation that
shares a constant with the design is not an expectation (the reservation granule); a rule whose
stimulus overflows an immediate is not a test (the CSR ledger); a case written around the field that
already failed does not test the format (`c.lui`). Three different mechanisms, one failure mode: the
check and the thing checked agreeing about something neither of them verified.

The attempt also left the case **honestly failing**: 105 of 106 checks pass and the one failure is
its own anti-vacuity check, which exists so that a phase with no evidence cannot pass. That is the
check doing its job — the wrong-path phase had no squashed-but-completed FP operation to observe —
and the third attempt is constructing the stimulus rather than relaxing the check.

---

## 2026-10-01 — state of the machine, final entry for this session (61 packages)

**Advertised capabilities: I, M, Zicsr, Zifencei, Zihpm, A, C, S, U, PMP, Sv39, Zicsr_Zifencei_pairing, Zaamo, Zalrsc, Zicbom, Zicbop.** `python3 tools/check_coverage.py --verbose` computes that list from
the ledger; `python3 tools/gen_manifest.py --profile p1` prints the ISA string and privilege modes
the build manifest is allowed to publish. Nothing in this file is claimed beyond it.

**What the machine is now.** A two-wide out-of-order RV64 core with: a banked register file and a
scoreboarded writeback path; a reorder buffer, precise traps, M/S/U privilege and PMP; a serial
Sv39 walker with a TLB and exact `SFENCE.VMA`; blocking L1 caches and an MSHR (both module-level,
neither wired into the core); an FP rename namespace with NaN-boxing and precise `fflags`; AMO and
LR/SC; the C extension; `FENCE`/`FENCE.I`; non-speculative MMIO; and a fabric of five modules
(bypass, steering, quota/arbiter, ownership FSM, bank-aware allocation) that are **verified in
isolation and not instantiated by the core** — the plan's I-090/V-034 integration is what would make
them real, and the fixed-versus-dynamic comparison the project's name rests on has not been measured.

**The strongest evidence in the tree** is the external one: **ACT4 passes 127/127 against the
integrated core**, with N closed against its generation manifest and its PASS macro calibrated. It
is strong precisely because it is not ours — it found five real defects in advertised extensions
within minutes (a `c.lui` immediate, an SC reservation granule that our own case agreed with the RTL
about, an HPM read-modify-write, and two ledger/case expectations that asserted the absence of the
CSRs the machine was advertising).

**The honest ledger** (`config/validation/exclusion_ledger.json`, checked by
`tools/check_exclusions.py` in `make check`) records 52 exclusions with reasons, spec bases,
alternative verifications that must exist **and pass**, and end conditions. Its largest open items
remain: no external reference (NEMU/Sail/XiangShan) is compared against this core in the normal
loop; no external suite covers FP, caches or the fabric (the ACT4 config excludes F/D and caches);
one profile and one seed for most cases; `fsqrt` and the fmadd family are declared absent; `frm` is
not renamed; and no device watchdog exists.

**Next wave, in dependency order**: V-060/V-061 (the FP verification packages that are now F's only
remaining gates), I-047/I-048 (the SoC and the p1 Linux contract), the fabric integration and the
measured fixed-versus-dynamic comparison (I-084), then Stage 4+: vector (I-051+), multi-hart and
cohort (I-064+), the measurement gates (I-076+) and the RVA23S64 mandatory matrix (I-092..I-098).
The plan is 239 packages; 61 are delivered, and the remainder is the larger half.

**For whoever continues**, the three habits that produced this state, in order of value: (1) an
expectation derived from the design under test is not an expectation — use the host, Sail, or a
specification-derived model, and prefer the external one; (2) a package is delivered when its case
passes **from a deleted build directory** with controls that fail, and the record carries the number
the tool printed, not the number the lane's summary remembered; (3) when a lane fails, its
hypotheses are the asset — two of the three retries in this session succeeded because the failed
attempt had written down what it suspected and why.

---

## 2026-10-01 — the fabric is in the machine, and the dynamic-versus-fixed comparison is measured (67 packages)

The goal's central requirement — *the dynamic-versus-fixed comparison measured on equal resources, never
inherited from documents* — is now met by measurement. Three of the five fabric modules are part of
the live core: the **bank-aware allocation** (I-032) driven from the routing decision, the **local
bypass** (I-027) instantiated in the cluster with its registered slot exposed as an earlier
value-visible wakeup, and the **steering policy** (I-029) replacing the alternating toggle for every
cluster-bound macro, with the fixed baseline selectable through one input bit.

**The measurement, on equal resources** — same ELF (`p02_branch.i0`), same seed, one toggle:

| | fixed | dynamic |
|---|---|---|
| cycles | 448 406 | 448 468 (**+62, +0.014%**) |
| retired events | 71 123 | 71 123 (**identical**) |
| integer-ALU issues per cluster | 214 / 17 624 | 8 498 / 9 340 |
| branch issues per cluster | 17 767 / 17 769 | 18 790 / 16 746 |
| router grants | 0 | 57 281 (age 57 267, load 14) |
| bypass captures | 0 | 17 849 (33 early-resolved) |

The **architectural result is identical field for field** — the whole retire stream and all four
signature words — and the dynamic configuration is **0.014% slower** on this program. That is
reported as a finding, not hidden, and the per-step table (`tools/run_fabric_steps.py`) isolates
each contribution: steering alone is cycle-neutral while balancing the clusters, the bank preference
costs about 80 cycles, the bypass recovers about 18. A steering policy that helps on one program and
hurts on another is exactly what the plan's "measured, not inherited" requirement exists to expose —
and a project that had reported only the cluster balance would have called this a win.

**Two modules are deliberately not wired in**, with reasons rather than silence: the bounded-service
arbiter (I-030), because its request/queue model does not compose with the writeback arbiter's
per-producer pending registers and its write/rename/ROB completion decision — inserting it is a
redesign of the durable completion path, not a wiring change — and the ownership FSM (I-031), because
it exists for reconfiguration, which p0 and p1 never perform, so wiring it in would be dead logic in
a machine whose counters and valid bits are the evidence.

**One control deserves naming**: `NO_DELTA` holds the strategy input low, so the "dynamic" run *is*
the fixed machine and no measurable difference exists. It is a control on the *measurement*, which is
what makes the comparison falsifiable — the same discipline the project applies to mutants, applied
to the number the project most wants to believe.

**State**: 67 packages delivered, 16 capabilities advertised, ACT4 127/127, `make check` and the
records/coverage/exclusion gates green, lint 51/51 on both profiles. Remaining for the goal: the
vector datapath (I-054..I-063), locality and the LLB, multi-hart/cohort/pod (I-064..I-075), the
measurement and release gates (I-076..I-086), the RVA23S64 mandatory matrix (I-092..I-098), F/D's
advertisement (gated on V-060/V-061, which are vector and LLB verification), wiring the caches into
the fetch and memory paths, and the integrated full comparison suite of I-084.

---

## 2026-10-01 — the caches are in the path, and the vector chain has six packages (69 delivered)

**The caches are real parts of the machine now.** `cache.integrated_path` passes at p1: the instruction
cache sits in the fetch path and the data cache in the memory path, cacheability comes from the
generated platform map rather than a hand-written predicate, MMIO and atomics bypass, and an `FENCE.I`
micro-FSM flushes the data side, invalidates the instruction side and holds the front end off. The
evidence that the caches are *used* is a memory-port beat count falling from **4186 to 107** with the
architectural results identical on and off — the number that rules out the silent failure of a cache
that never hits. Four mutants fail by name, including stale instruction bytes executed after `FENCE.I`
and a cached device access.

**A harness defect was found and closed, and it is worth reading.** The first attempt reported that
the retire payload (`ev_rd`/`ev_value`) appeared to *lag* `ev_pc` by one retirement for one program
while the same decode was exact for the corpus. The verdict, proven by an order-swap experiment rather
than by reading: the **driver's bus model queued fetch responses during the eight reset cycles** (the
fetch unit re-offers the reset-vector request while reset is asserted), and because fetch matches a
response to a slot by `{id, epoch}` — and only a redirect advances the epoch — the post-reset requests
consumed those stale words. From outside that looks exactly like a payload lagging its program
counter. Fixed in the driver, no RTL line changed, reduced to a two-instruction reproduction. The p0
corpus is immune only by the luck of its stimulus (its entry is a `JAL`, which bumps the epoch), and
**the same hazard is latent in the other core drivers** — recorded for a follow-up, because "it does
not happen to fail today" is not the same as "it cannot".

**The vector chain now has six delivered packages**, and two of them are large:

| package | what it delivered |
|---|---|
| I-051 descriptor | 9284 EEW/EMUL/SEW/overlap combinations, 6588 illegal rejected; a later conformance fix moved `vtype.vsew` from the recorded bits 7:5 to the ratified 5:3 across five copies of the constant |
| I-052 configuration | `vsetvl`/`vsetvli`/`vsetivli`, 36 AVL bands, `vill` blocking execution |
| I-053 VRF | 2/4/8 lane counts read the same logical vector, with a lifetime rule that refuses a write overlapping an unread read |
| I-054 integer datapath | 99 952 checks, **17 families implemented and 17 declared absent with names** — so the classic "implement `vadd`/`vmul` and advertise V" shortcut is structurally impossible, since an absent family has no capability bit |
| I-056 memory packetizer | **every mode the card names**, with element-granular fault ownership (`o_trap_elem_o` is the value a `vstart` restart needs) and a rejected whole-macro trap |

Two of those findings came from one lane reading another's work rather than from a test: the `vsew`
field position (a specification conformance bug that only an outside reader would question) and the
harness reset-queue hazard (which looked like a DUT defect until the order-swap experiment). Both are
worth keeping as a habit: the reader who trusts the existing convention is the one who cannot find the
convention's mistakes.

**State**: 69 packages delivered, 16 capabilities advertised, ACT4 127/127, `make check` and every
gate green, lint 55/55 on both profiles. Remaining for the goal: the rest of the vector chain
(I-055 vector FP, I-057 partial-trap restart, I-058 chaining, and the rest through I-063), locality and
the LLB, multi-hart/cohort/pod (I-064..I-075), the measurement and release gates (I-076..I-086), the
RVA23S64 matrix (I-092..I-098), wiring the vector unit into the core, and the harness-wide
reset-traffic rule.

---

## 2026-10-01 — the vector chain reaches seven packages, and two lanes corrected the lead (71 delivered)

Seven of the vector block's packages are delivered and green: descriptor (I-051), configuration
(I-052), VRF (I-053), integer datapath (I-054), FP datapath (I-055), memory packetizer (I-056) and
the partial-trap restart (I-057). Two of this stretch's most valuable outputs were **corrections to
the instructions I gave**:

* **Fault-only-first does not shorten `vl` for a first-element fault.** My brief said it does, twice.
  The lane checked the specification, found that element 0 *takes a trap with `vl` unmodified* while
  an element k>0 is absorbed with `vl` reduced to k, cited the pinned tag and line numbers, confirmed
  it against the ratified manual, implemented the specification, and said in its report that the brief
  was wrong. That is the behaviour the project wants from a lane: the integration lead is not a
  specification.
* **The `vtype.vsew` field position** was found by one vector lane reading another's descriptor and
  noticing that the layout did not match the ratified format — a conformance bug that no test in the
  repository would have raised, because every test agreed with the code.

**The two newest packages are the strongest evidence the vector work is real rather than nominal.**
I-057 proves the store side-effect rule by **counting memory writes per byte** across a fault, the
restart and the completion, requiring every byte written exactly once and the request stream to be
exactly `0..k` then `k..vl-1` — the card's "a restart must not duplicate an irreversible side effect"
made into a number rather than an argument. I-055 implements a **determinism table** instead of a
tolerance: results are compared with `==` for everything deterministic including the *ordered*
reductions, and by membership in an enumerated permitted set for the unordered ones, with the ordered
test vector `1.0, 2^24, 1.0, -2^24` chosen so that a left fold gives 0 and a reassociated fold gives 1
— a machine that reassociates an ordered reduction fails instead of passing within an epsilon.

Two rules are now recorded rather than lost, both reported independently by two lanes: the
**mask-prefix operations with a non-zero `vstart` must raise illegal-instruction** and neither I-054
nor I-057 implements that; and the **harness reset-traffic hazard** (every core driver currently
accepts bus requests while reset is asserted, and the corpus is immune only because its entry
instruction happens to redirect). Neither is a failing test today; both are the kind of thing that
becomes a mystery later.

**State**: 71 packages delivered, 16 capabilities advertised, ACT4 127/127, `make check` and every
gate green, lint 57/57 on both profiles. Remaining for the goal: the rest of the vector block
(I-058..I-063: chaining, lane quotas, and the arithmetic restart), **wiring the vector unit into the
core** (no decoded vector instruction reaches any of these seven units yet), the LLB and locality
work, multi-hart/cohort/pod (I-064..I-075), the measurement and release gates (I-076..I-086) and the
RVA23S64 mandatory matrix (I-092..I-098).

---

## 2026-10-01 — chaining measured, and a harness hazard closed class-wide (72 delivered)

**I-058 (vector chaining)** passes with the benefit *measured* rather than asserted: 10 cycles with chaining
against 18 without, 8 forwarded elements against 0, 1 stall against 9, and the architectural state identical
on and off. Both of the card's named hazards are controlled by mutants — a macro-level ready bit letting a
consumer read an unwritten element, and a cancelled instruction's packet being accepted by a new descriptor
— and the scoreboard design is the sharpest distinction the vector work has produced: the descriptor's
element bitmap is **architectural progress bound to the restart point** and must never record a speculative
packet, while chaining readiness is a **microarchitectural forwarding fact** that carries element data and
the producing generation. They cannot disagree, because when the binding input is set the descriptor's
element-done pulse *is* the network's accepted-packet pulse.

**The reset-traffic hazard is closed class-wide.** The rule — while reset is asserted, no bus model accepts,
queues or delivers a request, and a request the DUT presents is ignored, because holding the reset-vector
request is a legal state and not a DUT error — now lives in **one shared mechanism** (`mosaic::BusResetGate`),
which **27 core drivers** route through. The survey is the valuable part: six non-core drivers already carried
their own ad-hoc guard, and several core drivers guarded the *instruction* path but **not the data** path, so
the hazard was half-closed rather than absent. The case's RESULT line states the condition that mattered
(`first=nonredirect` — a program starting with a `JAL` would pass even with the bug present), and its control
run accepts 4 reset-time requests and *makes the case fail*, which is what proves the case catches the class.
It also demonstrates the stale-response mechanism deterministically.

**Three process rules were earned this stretch and are worth more than either package.** (1) A lane that
suspects its change is being blamed for a red gate should **exonerate itself by experiment** — rebuilding the
same case from `git show HEAD:` — which is exactly how the red `core.act_dut` was traced to another lane's
in-flight edits rather than to chaining. (2) **A `git stash` must always carry a pathspec**: a bare one takes
every modified tracked file in the repository, and this time it swept a sibling's tracked edits into a stash
that had to be restored hash-verified. (3) **A gate that is red because another lane is mid-edit is still a
gate**: I told the offending lane to land the change in one piece and re-verify the 127-ELF suite rather than
treat "someone else's edit" as an excuse to look away.

**State**: 72 packages delivered, 16 capabilities advertised, ACT4 127/127, `make check` and every gate green,
lint 58/58 on both profiles. Remaining for the goal: I-059..I-063 (lane quotas and the arithmetic restart),
**wiring the vector unit into the core** — seven delivered vector units still have zero references from
`mosaic_core.sv`, which is the single largest gap between what is verified and what the machine does — the
LLB and locality work, multi-hart/cohort/pod (I-064..I-075), the measurement and release gates
(I-076..I-086), the RVA23S64 mandatory matrix (I-092..I-098), and the mask-prefix `vstart` rule that two
lanes have now reported.

---

## 2026-10-01 — the vector engine is in the machine, and a lying diagnostic (74 delivered)

**`vec.integrated` passes: a decoded vector instruction now executes on the integrated out-of-order
core.** The engine (configuration, descriptor, VRF, ALU, LSU, restart, chain) is instantiated in
`mosaic_core.sv`; OP-V decodes with **one macro per instruction**; dispatch routes through the system
insert port with an allocation barrier; the vector CSRs are on the core's CSR path with `mstatus.VS`
dirtying per ROB slot; vector macros resolve at the ROB head and complete on writeback port 3; a fault
is precise with `vstart` and a restart that keeps the committed prefix; and the packetizer's memory
port is a **fourth owner** on the single data arbiter. **ACT4 held 127/127 through every step**, and
forty-one cases were re-run green afterwards. On this program the vector path is *slower* (203 cycles
against 103) — reported rather than hidden, and expected while vector registers are not renamed and
macros are serialised by the barrier.

**The defect found behind the failure is the story of the day.** The first attempt reported that the
handler's *first* `csrr mtval` returned 0 while a later read returned the right value, and
hypothesised a trap-entry staging latency — a defect any real handler would hit. The retry disproved
that with evidence (one write, one cycle; the first read was correct; the core's own observation ports
agreed) and found the actual cause of the wrong number: **the check's own failure message printed
`run.Slot(16)` while the condition tested `run.Slot(2)`** — the testbench was describing a different
word from the one it examined, and the word it described is always zero. Behind that artifact was a
genuine ISA defect: the vector fault's `mtval` was computed as `base + (vstart << eew_sew)` where
`eew_sew` is log2(EEW in **bits**) while the packetizer addresses in **bytes**, so the byte offset
should be `vstart << (eew_sew - 3)`. For an e32 access at `vstart=2` the machine named `base+64` where
the faulting element sits at `base+8` — wrong by the factor of eight that shifting bits instead of
bytes produces, and **any handler computing from `mtval` for a vector memory fault was handed an
address eight elements further on.** One term, fixed.

The generalisation belongs next to the project's other rules: **a diagnostic that prints a plausible
value and names a plausible mechanism can send an investigation in the wrong direction for its whole
duration.** The first attempt spent its time on a hypothesised staging latency because its own message
said "read 0" with a number attached; the value was real and the label was false. That is the same
failure mode as the mutant that never reached the compiler and the case whose stimulus never arrived —
a check whose *report* is not about the thing it *tested*.

**I-060 (the LLB) is also delivered**, and its shape is the plan's argument made concrete: the
structure is legal **only because its invalidation sources can be listed**, and eight are — this
hart's store or AMO, an L1 refill, a snoop, a shootdown, `FENCE`, `FENCE.I`, `SFENCE.VMA` (exactly
the named ASID and/or page, never treated as a data fence) and a context change. The key is the
physical line plus the permission context with **no virtual address in it** (the mutant that keys on
the virtual address is what forces that), data-line invalidations ignore the context so a store
removes every context's copy, and the in-flight rule is stated and checked: an invalidating cycle
grants no hit, the refusal is combinational with priority over the tag match, and it is *specific* —
a lookup on another line still hits in the same cycle. The case asserts the buffer actually **hit**
before each invalidation it tests, which is what stops it passing by accident.

**State**: 74 ledger entries delivered, 16 capabilities advertised, ACT4 127/127, `make check` and
every gate green, lint 59/59 on both profiles, and the exclusion ledger at 18 open / 36 covered.
Remaining for the goal: I-059 (the lane broker, deliberately kept open — the integration is recorded
as I-059a rather than being allowed to mark it delivered), I-061..I-063, wiring the LLB and the
caches' locality into the core, vector register renaming, multi-hart/cohort/pod (I-064..I-075), the
measurement and release gates (I-076..I-086), the RVA23S64 mandatory matrix (I-092..I-098), the
mask-prefix `vstart` rule, and the `tb_vec.cpp` reset-traffic follow-up.
