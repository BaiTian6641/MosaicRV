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
