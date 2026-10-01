# V-012 — fixed signature and termination decision

Work package V-012 (`docs/validation-plan.md` §5 V-012, "固定 signature 与终止判断";
`docs/implementation-plan.md` §3.1). Registered case
**`exit.protocol_termination`**, top `mosaic_bringup_tb`.

| Artifact | Role |
|---|---|
| `sim/unit/tb_exit.cpp` | the case: the termination state machine, the eight documented exit classes, the signature model, the six mutants |
| `tests/programs/termination/src/x01…x08*.S` | the eight scenario programs |
| `tests/programs/termination/exit.h` | the constants and the two provenance comments the programs rely on |
| `tests/programs/termination/Makefile` | builds them into `tests/programs/build/termination/`; `dis` and `audit` targets |
| `tests/programs/Makefile` | one added delegate target, `make -C tests/programs termination` |
| `results/unit/exit.protocol_termination/` | the observed run: `summary.txt`, `run.log`, `result.json`, one event-stream file per scenario |
| `results/reports/V-012-exit.md` | this file |

## STATUS: PASS — 11 of 11 rows produced their documented status; the checks are negative-controlled

| Check | Observed |
| --- | --- |
| the registered case | `PASS exit.protocol_termination task=V-012` (exit 0) |
| scenarios vs their documented status | `11/11 scenarios matched their documented status, 8 distinct exit classes, checks=28 cycles=40744 budget=20000 drain=32` |
| six harness mutants, each built from an empty build directory | **all six exit 1**, each naming the one scenario it affects (§5) |
| two image controls | both `IMAGE_REJECTED(7)` before anything runs (§5) |
| ISA audit of the seven audit-able programs | `7 ELF(s), 155 instructions, all in rv64im_zicsr_zifencei` (exit 0) |
| the firmware corpus is untouched | `tools/host_oracle.py --all` exit 0 (39 cases), `--check-golden-only` exit 0 |
| `tb_exit.cpp` under the project's C++ standard | `-Wall -Wextra -Wshadow` adds **no** warning of its own |
| RTL gates | no RTL file was touched by this package |

---

## 1. What the case drives

One run per scenario, of the DUT through the ports `mosaic_bringup_tb` already
publishes: reset, then tick until the memory model raises its TOHOST pulse, then
the drain, then read the signature back through the harness read-back port. The
decision is a small state machine with a **documented order of phases**, so a
failure is always attributable:

| Phase | Question | Class if it fails |
| --- | --- | --- |
| `IMAGE` | did the loader accept the ELF, and is its entry the profile reset vector? | `IMAGE_REJECTED(7)` |
| `RUN` | did the program reach TOHOST inside the budget? | `TIMEOUT(2)` / `WFI_STALL(3)` |
| `DRAIN` | was the exit store retired, was the signature complete at the exit, was the data port idle? | `UNDRAINED_STORE(6)` |
| `OVERRUN` | did any store write past the frozen four-word signature window? | `SIGNATURE_OVERRUN(5)` |
| `EXIT` | was the word written to TOHOST exactly `pass_code` (1)? | `PROGRAM_FAIL(1)` |
| `SIGNATURE` | do the four signature words equal the model? | `SIGNATURE_MISMATCH(4)` |
| `COMPLETE` | all of the above | `PASS(0)` |

Documented constants of the machine: reset 5 cycles (the I-008 core's contract),
per-scenario budget `min(--max-cycles, 20000)` cycles (the runner's 4,000,000 is
an upper bound, not the budget — a timeout must actually be reached, and the two
timeout scenarios would otherwise cost minutes), drain window **32 cycles** fixed
and documented, and the signature window `[0x80000400, 0x80000420)` with a guard
band `[0x80000420, 0x80000600)`.

The **process** exit code follows the project convention (`sim_common.h`): 0 only
when every scenario produced exactly its documented status, 1 when any did not
(naming scenario, phase and cycle), 2 for a usage error. So the case exits 0 for
a *correctly detected* timeout and 1 for a timeout reported as success — which is
the property the card asks for, expressed the way every other case in this tree
expresses it (`harness.timeout` does the same).

### 1.1 The drain

The pulse says the memory model accepted a non-zero store to TOHOST. It does not
say the hart retired that store, and it does not say the program finished writing
its signature. The drain keeps simulating 32 further cycles and requires:

* **(a) ordering** — the store that wrote TOHOST is observed to retire **at or
  after** the pulse cycle;
* **(b) completeness** — **no** store touches the signature window or its guard
  band at or after the pulse;
* **(c) idle** — the hart is out of the data-request states (`dbg_state` 3/4)
  when the window closes.

The signature is read only after the window. In this design the memory model
performs the TOHOST store and raises the pulse on the same edge and the in-order
core has no store buffer, so (a) and (c) hold on every run and the store retires
on the pulse cycle itself; they are reported in §6 as **redundancy probes**, not
as detections. (b) is the rule that fires, and it is the rule that a store buffer
would break: it is stated now, against a DUT that cannot violate it, so that the
harness's answer is fixed before a DUT that can.

## 2. Where the expectations come from — and what is not read

* **Addresses.** `reset_vector`, `tohost`, `fromhost`, `signature`,
  `signature_words`, `pass_code` are read from `config/profiles/p0.json`, and the
  RAM base/size from the `ram` region of `config/memory/p0.json` (region-scoped:
  a plain key search finds `boot_rom`'s base first and would make every PC look
  unmapped). All seven are cross-checked against the generated
  `build/p0/sim/mosaic_platform.h`, plus two containment checks (the guard band
  and the operand area lie inside RAM). A literal that stops matching its
  configuration fails by name before anything runs.
* **Operands.** `{a, b, c}` live in exactly one place, the scenario table in
  `tb_exit.cpp`. The harness writes them into the firmware program buffer
  (`MOSAIC_SCRATCH_BASE`, 0x80001080) through the image-loader port while reset
  is asserted. They are **not** compiled into the programs, so a program cannot
  disagree with the table about what it was asked to compute.
* **Expected signature.** The four expressions
  `{a+b, a-b, a^b, (a<<3)^c}` evaluated in C++ from those operands. It is a
  second implementation of the four instructions the programs execute (assembly
  vs C++), and it is never read from the DUT, from DUT memory, from the ELF, or
  from any file the DUT writes. The one thing read back from the DUT is the
  signature itself, which is what it is compared against.
* **Expected status.** Part of each scenario's documentation in this report and
  in the scenario table; not derived from the run.
* **Not read, deliberately:** the UART and any text anywhere — the run ends
  because a non-zero store to the frozen TOHOST address was observed by the
  memory model, and for no other reason (`h_uart_count` is checked to be 0 in
  every scenario, so "no UART text was involved" is a check and not a claim); the
  DUT's own report of success (the TOHOST word is *interpreted*, not trusted);
  the program's image for expected values.
* **Not this case's job:** comparing the DUT's retire stream against an
  independent reference (I-008's `core.bringup_vs_reference`), and anything about
  the out-of-order core (a different lane's `tb_core.cpp`).

## 3. The eight documented exit classes

| Code | Class | Meaning |
| --- | --- | --- |
| 0 | `PASS` | whole protocol completed, no mismatch, drain finished |
| 1 | `PROGRAM_FAIL` | TOHOST written, word not exactly 1; code = bits [63:1] |
| 2 | `TIMEOUT` | protocol never completed: budget expired, or the PC left RAM (`RUN/PC_ESCAPED`) |
| 3 | `WFI_STALL` | WFI executed, p0 has no wake source, so the hart can never reach TOHOST |
| 4 | `SIGNATURE_MISMATCH` | a signature word disagrees with the model (first word named) |
| 5 | `SIGNATURE_OVERRUN` | a store wrote past the frozen four-word window |
| 6 | `UNDRAINED_STORE` | exit signalled with a store still outstanding / exit store never retired / data port busy |
| 7 | `IMAGE_REJECTED` | the loader refused the image, or its entry is not the reset vector |

All eight are *observed* in the 11-row table below (the case asserts, as a
separate check, that each of the eight is produced by the scenario that
documents it — a single catch-all failure could not have produced this table).

## 4. The scenario programs and the observed run

`make -C tests/programs termination` builds them; each is its own reset path (no
`crt0.S`, no `trap.S`), so nothing here inherits the corpus's "main returned →
PASS" shortcut. `make -C tests/programs/termination dis` prints the disassembly
of each; the eight programs total 155 instructions in seven audit-able images
(x04 excluded, §7.3).

| Program | Drives | Clean path | Expected class |
| --- | --- | --- | --- |
| `x01_normal.S` | signature then TOHOST = 1 | 25 instr | `PASS(0)` |
| `x02_fail.S` | signature then TOHOST = (0x2A<<1)\|0 | 25 instr | `PROGRAM_FAIL(1)`, code 0x2A |
| `x03_spin.S` | signature then `j .` forever | 20 instr | `TIMEOUT(2)` |
| `x04_wfi.S` | mtvec ← own park loop, then `wfi` | 24 instr | `WFI_STALL(3)` |
| `x05_signature_mismatch.S` | `xori` makes word 2 one bit wrong, then TOHOST = 1 | 26 instr | `SIGNATURE_MISMATCH(4)` |
| `x06_signature_overrun.S` | four words, then a fifth at 0x80000420, then TOHOST = 1 | 31 instr | `SIGNATURE_OVERRUN(5)` |
| `x07_pending_store.S` | words 0..2, TOHOST = 1, **then** word 3 | 25 instr | `UNDRAINED_STORE(6)` |
| `x08_runaway.S` | `jalr x0, 0(x0)` into zeroed boot_rom | 3 instr | `TIMEOUT(2)`, `RUN/PC_ESCAPED` |

Verbatim observed run (`python3 tools/run_unit.py --profile p0 --case
exit.protocol_termination`, exit 0; also in
`results/unit/exit.protocol_termination/run.log`):

```
  x01_normal status=PASS(0) expected=PASS(0) cycles=120 phase=COMPLETE tohost=0x0000000000000001 sig=0x1,0x2468acf13579bdd,0xfffffffffffffffd,0x91a2b3c4d5e6f75 want=0x1,0x2468acf13579bdd,0xfffffffffffffffd,0x91a2b3c4d5e6f75 uart=0
  x02_fail status=PROGRAM_FAIL(1) expected=PROGRAM_FAIL(1) cycles=120 phase=EXIT/PROGRAM_FAIL tohost=0x0000000000000054 sig=0x8000000000000000,0x7ffffffffffffffe,0x7ffffffffffffffe,0xfffffffffffffff9 want=0x8000000000000000,0x7ffffffffffffffe,0x7ffffffffffffffe,0xfffffffffffffff9 uart=0
  x03_spin status=TIMEOUT(2) expected=TIMEOUT(2) cycles=20000 phase=RUN/TIMEOUT tohost=0x0000000000000000 sig=0x7ffffffffffffffe,0x8000000000000002,0x7ffffffffffffffe,0x3f want=0x7ffffffffffffffe,0x8000000000000002,0x7ffffffffffffffe,0x3f uart=0
  x04_wfi status=WFI_STALL(3) expected=WFI_STALL(3) cycles=20000 phase=RUN/WFI tohost=0x0000000000000000 sig=0xf0e215685baabdfc,0xcc7968773a53221e,0xcc99e8975a553de2,0xf56df77e57f78042 want=0xf0e215685baabdfc,0xcc7968773a53221e,0xcc99e8975a553de2,0xf56df77e57f78042 uart=0
  x05_signature_mismatch status=SIGNATURE_MISMATCH(4) expected=SIGNATURE_MISMATCH(4) cycles=123 phase=SIGNATURE tohost=0x0000000000000001 sig=0xffffffffffffffff,0x1e1e1e1e1e1e1e1f,0xfffffffffffffffe,0x787878787878787f want=0xffffffffffffffff,0x1e1e1e1e1e1e1e1f,0xffffffffffffffff,0x787878787878787f uart=0
  x06_signature_overrun status=SIGNATURE_OVERRUN(5) expected=SIGNATURE_OVERRUN(5) cycles=140 phase=OVERRUN tohost=0x0000000000000001 sig=0x3333333333333333,0xeeeeeeeeeeeeeeef,0x3333333333333333,0x88888888888888bb want=0x3333333333333333,0xeeeeeeeeeeeeeeef,0x3333333333333333,0x88888888888888bb uart=0
  x07_pending_store status=UNDRAINED_STORE(6) expected=UNDRAINED_STORE(6) cycles=115 phase=DRAIN/COMPLETENESS tohost=0x0000000000000001 sig=0xffffffffffffffff,0x5555555555555555,0xffffffffffffffff,0x555555555555554f want=0xffffffffffffffff,0x5555555555555555,0xffffffffffffffff,0x555555555555554f uart=0
  x08_runaway status=TIMEOUT(2) expected=TIMEOUT(2) cycles=6 phase=RUN/PC_ESCAPED tohost=0x0000000000000000 sig=0x0,0x0,0x0,0x0 want=0x3,0xffffffffffffffff,0x3,0xb uart=0
  x01_normal(second inputs) status=PASS(0) expected=PASS(0) cycles=120 phase=COMPLETE tohost=0x0000000000000001 sig=0xffffffffffffffff,0xfffe0001fffe0001,0xffffffffffffffff,0xfff80007fff80011 want=0xffffffffffffffff,0xfffe0001fffe0001,0xffffffffffffffff,0xfff80007fff80011
  control:corrupt magic status=IMAGE_REJECTED(7) expected=IMAGE_REJECTED(7) phase=IMAGE/LOADER (bad-magic: missing \x7fELF magic)
  control:wrong entry point status=IMAGE_REJECTED(7) expected=IMAGE_REJECTED(7) phase=IMAGE/ENTRY (entry point 0x0000000080000004 is not the profile reset vector 0x0000000080000000)
RESULT PASS exit.protocol_termination 11/11 scenarios matched their documented status, 8 distinct exit classes, checks=28 cycles=40744 budget=20000 drain=32
```

Evidence per row, from the saved event streams (one line per architectural
event, `<cycle> <pc> <insn> <trap> <cause> <store_addr> <bytes> <rd> <value>`):

* **x01** — the signature is complete before the exit; the TOHOST store retires
  at cycle 88, the same cycle as the pulse; drain closes at 120; the four words
  equal the model.
* **x03** — the whole signature is already published when the budget expires at
  cycle 20000 (`sig` equals the model), and TOHOST is 0: the program computed
  everything and still did not finish, which is exactly why a timeout is not a
  pass.
* **x04** — the WFI is visible in the DUT's own stream as a trap event
  (`83 0x0000000080000058 0x10500073 1 2`) and the run then parks at
  `0x8000005c` until the budget. The class is chosen from the observed encoding,
  not from the file name.
* **x05** — the program self-reports PASS (`tohost` = 1) and word 2 is
  `0xfffffffffffffffe` where the model says `0xffffffffffffffff`.
* **x06** — the run is clean up to the exit, and the overrun store is named:
  `91 0x0000000080000060 0x01eeb023` writes 8 bytes at `0x80000420`, one word past
  the frozen window (and the `li` that materialises the address is visible at
  cycle 80).
* **x07** — pulse at cycle 83, then a store to `0x80000418` (signature word 3) at
  cycle 88. The program signalled completion before the window was complete; the
  harness refuses the exit and does not compare the signature.
* **x08** — two events, then the PC is 0 and the `RUN/PC_ESCAPED` phase fires at
  cycle 6. An "empty" run is not a pass.
* **second inputs** — same program, different operands, different signature,
  both equal to the model: the comparison follows the operands and is not wired
  to a constant (asserted explicitly).

## 5. Control table (observed exit codes)

Mutants are one-line `#ifdef` blocks in `tb_exit.cpp` — the code under test for
this package is the termination state machine, so the mutants are C++ defines and
are passed as `-CFLAGS -D…` (a bare Verilator `-D` only reaches the Verilog
preprocessor). Each was built from an **empty** build directory with the same
Verilator flag list, source order and include paths `tools/run_unit.py` uses,
plus that single extra `-CFLAGS`. The build script is throwaway and not
committed; the exact commands are in §8.

| Control | Defect injected | Expected | Observed |
| --- | --- | --- | --- |
| `MOSAIC_EXIT_MUTANT_TIMEOUT_IS_PASS` | a budget expiry is returned as `PASS` | `x03_spin` wrong | **exit 1** — `expected TIMEOUT(2), got PASS(0)` |
| `MOSAIC_EXIT_MUTANT_WFI_AS_TIMEOUT` | the WFI class is dropped (timeout only) | `x04_wfi` wrong | **exit 1** — `expected WFI_STALL(3), got TIMEOUT(2)` |
| `MOSAIC_EXIT_MUTANT_FAIL_IS_PASS` | a non-1 TOHOST word is accepted | `x02_fail` wrong | **exit 1** — `expected PROGRAM_FAIL(1), got PASS(0)` |
| `MOSAIC_EXIT_MUTANT_NO_SIGNATURE_CHECK` | the signature comparison is skipped | `x05` wrong | **exit 1** — `expected SIGNATURE_MISMATCH(4), got PASS(0)` |
| `MOSAIC_EXIT_MUTANT_NO_OVERRUN_CHECK` | the guard-band rule is skipped | `x06` wrong | **exit 1** — `expected SIGNATURE_OVERRUN(5), got PASS(0)` |
| `MOSAIC_EXIT_MUTANT_NO_DRAIN` | the drain phase is skipped: the signature is read at the pulse | `x07` wrong | **exit 1** — `expected UNDRAINED_STORE(6), got SIGNATURE_MISMATCH(4)` |

Verbatim (`python3 /tmp/exit_mutants.py`, exit codes read without a pipe):

```
=============== MOSAIC_EXIT_MUTANT_TIMEOUT_IS_PASS
exit code: 1
MISMATCH x03_spin termination status: expected TIMEOUT(2), got PASS(0)
RESULT FAIL exit.protocol_termination x03_spin: expected TIMEOUT(2), observed PASS(0) -- phase RUN cycle 20000: the cycle budget expired with TOHOST never written [MOSAIC_EXIT_MUTANT_TIMEOUT_IS_PASS: a timeout was treated as success]

=============== MOSAIC_EXIT_MUTANT_WFI_AS_TIMEOUT
exit code: 1
MISMATCH x04_wfi termination status: expected WFI_STALL(3), got TIMEOUT(2)
RESULT FAIL exit.protocol_termination x04_wfi: expected WFI_STALL(3), observed TIMEOUT(2) -- phase RUN cycle 83: WFI executed and p0 has no wake source, so the hart can never reach TOHOST (budget 20000)

=============== MOSAIC_EXIT_MUTANT_FAIL_IS_PASS
exit code: 1
MISMATCH x02_fail termination status: expected PROGRAM_FAIL(1), got PASS(0)
RESULT FAIL exit.protocol_termination x02_fail: expected PROGRAM_FAIL(1), observed PASS(0) -- protocol complete at cycle 88, drained at 120, signature matches the model

=============== MOSAIC_EXIT_MUTANT_NO_SIGNATURE_CHECK
exit code: 1
MISMATCH x05_signature_mismatch termination status: expected SIGNATURE_MISMATCH(4), got PASS(0)
RESULT FAIL exit.protocol_termination x05_signature_mismatch: expected SIGNATURE_MISMATCH(4), observed PASS(0) -- protocol complete at cycle 91, drained at 123, signature matches the model

=============== MOSAIC_EXIT_MUTANT_NO_OVERRUN_CHECK
exit code: 1
MISMATCH x06_signature_overrun termination status: expected SIGNATURE_OVERRUN(5), got PASS(0)
RESULT FAIL exit.protocol_termination x06_signature_overrun: expected SIGNATURE_OVERRUN(5), observed PASS(0) -- protocol complete at cycle 108, drained at 140, signature matches the model

=============== MOSAIC_EXIT_MUTANT_NO_DRAIN
exit code: 1
MISMATCH x07_pending_store termination status: expected UNDRAINED_STORE(6), got SIGNATURE_MISMATCH(4)
RESULT FAIL exit.protocol_termination x07_pending_store: expected UNDRAINED_STORE(6), observed SIGNATURE_MISMATCH(4) -- phase SIGNATURE cycle 83: signature word 3 is 0x0000000000000000, the model says 0x555555555555554f
```

Two things to read carefully in that output:

* The **detector is the status comparison**, not the detail text. Three of the
  mutants short-circuit a check and fall through to the success path, so their
  `detail` still says "signature matches the model" — the mutant's own text, not
  an observation. The case fails on the *status*.
* `MOSAIC_EXIT_MUTANT_NO_DRAIN` shows the hazard directly: reading the signature
  at the pulse instead of after the drain turns `x07`'s undrained exit into a
  bogus `SIGNATURE_MISMATCH` on word 3 (0 vs the model's value). "Read the
  signature before the store drains" does not merely miss a violation; it
  fabricates a different one.

Image controls (both must be refused before any instruction runs; both are part
of the same 11-row table and use the same runner function as the rest):

| Control | Corruption | Observed |
| --- | --- | --- |
| corrupt magic | byte 0 of `x01_normal.elf` replaced with `X` | `IMAGE_REJECTED(7)` / `IMAGE/LOADER` — `bad-magic: missing \x7fELF magic` |
| wrong entry point | `e_entry` (offset 24) set to `0x80000004` — structurally valid, inside the executable segment | `IMAGE_REJECTED(7)` / `IMAGE/ENTRY` — `entry point 0x0000000080000004 is not the profile reset vector 0x0000000080000000` |

The second one is what exercises the `IMAGE/ENTRY` rule; without it that rule
would be unreachable.

## 6. Redundancy probes (stated, not counted)

* **Drain (a) and (c).** On every scenario the TOHOST store is observed to retire
  on the pulse cycle and the hart is out of `S_REQ`/`S_WAIT` well inside the
  window. `mosaic_bringup_core.sv` has no store buffer, so it cannot violate
  either rule, and no run in this campaign fails them: they are **redundancy
  probes**, recorded as holding, not as detections. The *phase* they belong to is
  load-bearing and demonstrated — dropping it changes `x07`'s class (mutant
  table). Both rules are stated anyway because the design that will be able to
  violate them (V-013/V-018's store drain) does not exist yet, and a rule written
  after the fact is worth less than one written before it.
* **`x03` vs `x08` statuses.** Both are `TIMEOUT(2)`, distinguished by the phase
  string (`RUN/TIMEOUT` vs `RUN/PC_ESCAPED`). The class is deliberately the same:
  neither run completed a protocol, and inventing a separate code for "the PC
  left RAM" would imply this harness can act on it. The phase names it.

## 7. Integration and gates

### 7.1 The corpus is untouched

The eight programs are **not** corpus programs: they are not in
`tests/programs/corpus.json`, they are not built by `make -C tests/programs all`,
they are their own reset path, and `tools/host_oracle.py` never sees them.
Neither `crt0.S` nor `mosaic_p0.ld` was modified, so no golden signature was
re-recorded — the card's re-record branch does not apply. Observed, after all
changes:

```
$ python3 tools/host_oracle.py --all            # exit 0
39 case(s) computed
oracle agrees with the declared expectations and the golden file for every case; all three inputs of every program give a distinct signature.

$ python3 tools/host_oracle.py --check-golden-only   # exit 0
oracle agrees with the golden file.
```

### 7.2 The directory is named `termination`, not `exit` — this is a workaround, and the reason is a defect

The programs were first written in `tests/programs/exit/`. The ISA audit refused
every image:

```
../build/exit/x03_spin.elf: dynamic symbol exit -- libc/libgcc leaked in
```

`tests/programs/audit/check_p0_isa.py:190` searches for each name in
`FORBIDDEN_SYMBOLS` as a substring of the whole `objdump -t`/`-T` output, which
includes objdump's own header line containing the **file path**: the path
`build/exit/x03_spin.elf` contains the forbidden string `exit`. It is a false
positive — any image whose path contains `exit`, `printf`, `memcpy`, `strlen` or
one of the libgcc helper names is rejected regardless of its contents. Renaming
the directory to `termination/` sidesteps it; the underlying substring match is
**not fixed here** (that file belongs to I-007's audit and fixing it deserves its
own control), and it is reported as a finding.

### 7.3 ISA audit

```
$ make -C tests/programs/termination audit
ok   ../build/termination/x01_normal.elf             25 instructions, entry=0x80000000
ok   ../build/termination/x02_fail.elf               25 instructions, entry=0x80000000
ok   ../build/termination/x03_spin.elf               20 instructions, entry=0x80000000
ok   ../build/termination/x05_signature_mismatch.elf    26 instructions, entry=0x80000000
ok   ../build/termination/x06_signature_overrun.elf    31 instructions, entry=0x80000000
ok   ../build/termination/x07_pending_store.elf      25 instructions, entry=0x80000000
ok   ../build/termination/x08_runaway.elf             3 instructions, entry=0x80000000

audit: 7 ELF(s), 155 instructions, all in rv64im_zicsr_zifencei; no libgcc, no libc, no relocations, entry 0x80000000
```

`x04_wfi` is excluded **by name, in the target's own output**, because the audit's
M-mode allow-list is currently `{"mret"}` (`check_p0_isa.py:63`) and the program
deliberately executes WFI. Excluding it is honest only if the exclusion is real,
so the audit was also run on it directly and does reject it:

```
$ python3 tests/programs/audit/check_p0_isa.py --objdump riscv64-elf-objdump \
    --readelf riscv64-elf-readelf --arch 'rv64i2p1_m2p0_zicsr2p0_zifencei2p0_zmmul1p0' \
    tests/programs/build/termination/x04_wfi.elf      # exit 1
=== p0 ISA audit failures ===
tests/programs/build/termination/x04_wfi.elf: 1 instruction(s) outside rv64im_zicsr_zifencei:
    wfi   <- 80000058:	10500073          	wfi
```

WFI is a machine-mode instruction from the privileged specification, not an
extension; a future audit that lists it (or that takes the profile's privileged
set from `config/`) would let `x04_wfi` be audited with the rest. Until then the
exclusion is printed on every run of the target.

### 7.4 C++ standard

```
$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -Ibuild/p0/sim -Isim/common \
      -I"$(verilator -getenv VERILATOR_ROOT)/include" \
      -Ibuild/p0/unit/exit.protocol_termination/obj_dir sim/unit/tb_exit.cpp
64 warnings generated.
```

All 64 warnings are from Verilator's own headers under `-Wextra` (which
`sim_common.h` documents as not `-Wextra`-clean); **none** is from
`tb_exit.cpp`. No RTL file was touched, so `tools/lint_rtl.py --profile p0` and
`slang-tidy` are unaffected by this package.

### 7.5 Files

Added: `sim/unit/tb_exit.cpp`, `tests/programs/termination/**`. Changed:
`tests/programs/Makefile` (one delegate target `termination`, not part of
`all`). Generated by running the case:
`results/unit/exit.protocol_termination/{summary.txt,run.log,result.json,*.events.txt}`
and two control ELFs.

```
3d15f4576fe7934bbdcee24c60760abfb61f291a0b9a0bf08326601a7d024aad  build/termination/x01_normal.elf
d54d1ac319f81c5489c7d8784371a3f1a34442a2e79f10234518976807ace9fa  build/termination/x02_fail.elf
bf034c26d438c6ba19ad0f1055705f3d7b9f3d732a659fef03fb90d7ebf0121c  build/termination/x03_spin.elf
09d8f81384b264d7cfdc97cbef894f5fb8bcdbfc9f6a7cd19f40f98111f1f9fc  build/termination/x04_wfi.elf
6e39ca2cd091d6aec601210762950266c7f1d50679303a2b6546600efc229666  build/termination/x05_signature_mismatch.elf
392c95a1edb0b2d65171e858819f6856f8fb25606ee071d461418fbfe8585ebd  build/termination/x06_signature_overrun.elf
8979fc701eeb098d097264069766fb92602e3e149dd48a496674578ac5178955  build/termination/x07_pending_store.elf
06e8332db08f5a12b5a0cb8e2e32a68740d268961234bbca9f407f6a4064b9b6  build/termination/x08_runaway.elf
ade40a8e341016ea3af885dec69859620f2ee2238acf80a17050e632e1bd2704  sim/unit/tb_exit.cpp
```

## 8. Commands

```sh
# programs (idempotent; also available as `make -C tests/programs termination`)
make -C tests/programs/termination            # build + entry/segment map
make -C tests/programs/termination dis        # disassemble every program
make -C tests/programs/termination audit      # ISA audit of the 7 non-WFI programs

# the case, from an empty build directory
rm -rf build/p0/unit/exit.protocol_termination
python3 tools/run_unit.py --profile p0 --case exit.protocol_termination

# iterate by hand
python3 -c "import sys;sys.path.insert(0,'tools');import run_unit;\
e=run_unit.load_registry()['cases']['exit.protocol_termination'];\
run_unit.build_case('p0','exit.protocol_termination',e)"
build/p0/unit/exit.protocol_termination/exit.protocol_termination \
    --case exit.protocol_termination --out results/unit/exit.protocol_termination \
    --seed 1 --max-cycles 4000000

# the corpus must be unaffected
python3 tools/host_oracle.py --all
python3 tools/host_oracle.py --check-golden-only
```

The mutant campaign is `/tmp/exit_mutants.py` (throwaway, not committed): for
each define it repeats `run_unit.VERILATOR_FLAGS`, the registry's source order
and include paths into a fresh `build/p0/unit/exit.protocol_termination-mutants/<mutant>/obj_dir`,
adds exactly `-CFLAGS -D<mutant>`, then runs the binary with
`--case exit.protocol_termination --out <that dir>/out --seed 1 --max-cycles 4000000`
and captures the exit code and the `RESULT`/`MISMATCH` lines. Build directory
state: `build/p0/unit/exit.protocol_termination` was deleted once before the
campaign (the runner wipes it anyway when the build command changes) and the
mutant directories are separate, so no mutant ever shared an object directory
with the shipping build; a clean build from an empty directory takes 2.5 s.

## 9. Not verified — honest list

* **The OoO core.** The DUT here is the I-008 single-issue in-order bring-up
  core. Nothing in this report says anything about the out-of-order fabric, and
  the drain rules are specified for a store buffer that does not exist yet.
* **Drain (a) and (c) as detectors** (§6): observed to hold, never observed to
  fail. Redundancy probes.
* **`UNDRAINED_STORE` sub-cases "the TOHOST store never retired" and "the data
  port busy at the window close"**: implemented, unreachable with this core, and
  therefore unobserved. Only the completeness sub-case (b) is demonstrated.
* **`RUN/PC_ESCAPED` is a harness convention.** p0 program space is taken to be
  RAM because the reset vector and every corpus/termination image are in RAM and
  boot_rom is a read/execute device the firmware never runs from. An ISA-legal
  program that executed out of boot_rom would be reported as a runaway; no such
  program exists in this corpus by construction.
* **The signature guard band is this case's rule**, not a line in `config/` and
  not an ISA rule; its justification (a bounded, aligned, otherwise-unassigned
  block) is in `exit.h`. A program that legitimately wants that RAM would be
  rejected here, and no corpus program is run through this case.
* **The harness mutants are C++ defines of the harness itself.** They show each
  classification is load-bearing (the case's own assertion is sensitive to each
  one); they are not RTL fault injection into the core, and the core's mutants
  remain I-008's.
* **No UART/MMIO/interrupt path** is exercised: p0 has none, and `x04_wfi`'s
  point is that no wake source exists.
* **The audit false positive in §7.2 is reported, not fixed.**
* **`x04_wfi` is outside the ISA audit** because the audit's M-mode allow-list is
  `{"mret"}`. Its disassembly is printed by `make -C tests/programs/termination
  dis` and quoted above.
