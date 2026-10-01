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
