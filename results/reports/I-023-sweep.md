# I-023 — the whole corpus, on the out-of-order core, against the host oracle

`CASE=core.corpus_sweep`, driver `sim/unit/tb_core_sweep.cpp`, top
`mosaic_core_tb`, Verilator 5.052 (macOS arm64), profile p0.

The other corpus cases run a handful of programs each: `core.corpus_branch`
drives p02 through the control path, `core.mem_program` drives p03/p09 through
the memory path, `core.trap_csr_program` drives p08/p13 through the trap path.
This case closes the gap. It loads **every** (program, input) image the corpus
builds — `tests/programs/build/<program>.iN.elf`, one ELF per declared input
pattern, 13 programs × 3 inputs = 39 images — runs each from the profile's reset
vector (0x80000000, `_start`) through the integrated core, and compares the four
signature words the program's own stores left in memory with the value
`tools/host_oracle.py` computes on the host.

The case passes from a clean build: 39 runs, 39 PASS, 0 FAIL, 0 STOPPED, exit 0.

```
$ python3 tools/run_unit.py --case core.corpus_sweep
PASS core.corpus_sweep            task=I-023
```

Run by hand (23 s, the only build is the case's own):

```
$ build/p0/unit/core.corpus_sweep/core.corpus_sweep \
      --case core.corpus_sweep --seed 1 --max-cycles 4000000
RESULT PASS core.corpus_sweep programs=13 inputs=3 runs=39 pass=39 fail=0 stopped=0 \
       cycles=17484988 retires=2774478 oracle=tools/host_oracle.py
```

## 1. Where the expectation comes from

Three independent things, and none of them is the DUT:

1. **The host oracle, invoked live.** Every run of the driver starts by
   executing `python3 tools/host_oracle.py --all` as a subprocess and parsing its
   table (`RunOracle()` in the driver). The expectation is the oracle's own
   arithmetic for the exact (a, b, c) of each image, not a transcription that
   could have drifted from it. That invocation is also the oracle's own
   self-check: `--all` exits 0 only when the oracle agrees with the corpus
   declaration (`corpus.json` → `expect_sig`/`expect_traps`), with the recorded
   golden file (`golden.json`), and with itself about input sensitivity (every
   program's three inputs must give a distinct signature). A non-zero exit fails
   the case with the oracle's stderr attached, because then the thing being
   compared against is not the oracle. It exited 0, on 39 rows.

   The program list and the input patterns are *not* hardcoded in the driver:
   they come from the oracle's table, which is built from
   `tests/programs/corpus.json`, and the ELF path follows the corpus's own
   `<program>.iN.elf` naming. Adding a program to the corpus adds it to the
   sweep.

2. **An independent host interpretation of the same image**, run in-process on
   its own `mosaic::MemoryModel`: `sim/unit/trap_ref.h`, the RV64IM_Zicsr
   interpreter that models the p0 trap policy, which `core.trap_csr_program`
   already uses. The driver requires the interpreter's end-state signature to
   equal the oracle-derived expectation for every one of the 39 runs (`ref_matches`
   in the driver; each is also a `Reporter` check). The interpreter shares
   nothing with the RTL and nothing with the DUT's memory.

3. **The two program-derived relations** between a program's published signature
   and the oracle's row. For 11 of the 13 programs the program publishes exactly
   the oracle's row. Two do not, for reasons that are properties of the *program
   text* and are stated, applied and checked here rather than absorbed silently
   (the same two facts `core.trap_csr_program` records):
   * `p08_misaligned` folds **all eight** `MOSAIC_TRAPLOG_RECORDS` records rather
     than the five the oracle predicts, and its low two bits are its own canary
     verdict: `program = ((oracle & ~3) << 24) | bits`. The driver applies this
     and requires the independent interpretation to produce the same value.
   * `p13_romstore` folds with the written record count (which the oracle models)
     but XORs in a marker set on the instruction *after* the armed store, which
     `trap.S`'s `mepc + 4` recovery does execute: `program = oracle ^ 1`. Applied
     and checked the same way.

   For those two programs the driver prints both the oracle's raw row and the
   derived value, so the matrix below cannot be read as if every row were the
   same kind of number.

The comparison itself is word-by-word: the first word where the DUT's memory
differs from the derived expectation is named, with both values.

## 2. What runs

Each image is loaded, its `.text`/data words go into the instruction memory
(anything not in the image answers ECALL, so a runaway fetch reaches a system
instruction rather than a don't-care), and its loadable segments go into the
harness's sparse platform memory model. crt0 then executes its own boot
sequence — stack, `mscratch`, `mtvec`, five `zero_region` sweeps (~17 KiB of
stack zeroed a store at a time; this is where most of the ~450k cycles go), the
FROMHOST override, the TOHOST read probe, `call main`. Nothing about any program
is patched, skipped or pre-set; the case never writes to the instruction image
except under the `--control-illegal` control below.

A run is over when the program writes its own TOHOST (the frozen exit protocol,
handled in the memory model), when the machine refuses an instruction and stops,
when it stalls, or at the cycle bound. `finished` is checked before `stopped`, so
a program that traps and recovers (p08, p13) is judged on its exit, not on the
trap.

Every cycle of every run also holds the invariants the other core cases assert:
the divergent-recovery counters (`squash_not_committed`, `squash_underflow`,
`journal_overflow`) stay zero; ROB occupancy stays within the ROB; the load
queue's forwarding-query cross-check stays zero; the store queue's authorised
prefix never exceeds its occupancy. A run that trips one fails where it happened.

### The matrix

| program | in | verdict | cycles | retires | redirects | traps |
|---|---|---:|---:|---:|---:|---:|
| p01_addsub | 0 | PASS | 447925 | 71072 | 17764 | 0 |
| p01_addsub | 1 | PASS | 447937 | 71071 | 17765 | 0 |
| p01_addsub | 2 | PASS | 447925 | 71072 | 17764 | 0 |
| p02_branch | 0 | PASS | 448406 | 71123 | 17788 | 0 |
| p02_branch | 1 | PASS | 448548 | 71142 | 17794 | 0 |
| p02_branch | 2 | PASS | 448312 | 71120 | 17782 | 0 |
| p03_loadstore | 0 | PASS | 447983 | 71088 | 17764 | 0 |
| p03_loadstore | 1 | PASS | 447983 | 71088 | 17764 | 0 |
| p03_loadstore | 2 | PASS | 447990 | 71085 | 17765 | 0 |
| p04_mul | 0 | PASS | 448229 | 71064 | 17764 | 0 |
| p04_mul | 1 | PASS | 448229 | 71064 | 17764 | 0 |
| p04_mul | 2 | PASS | 448229 | 71064 | 17764 | 0 |
| p05_divrem | 0 | PASS | 448464 | 71072 | 17766 | 0 |
| p05_divrem | 1 | PASS | 448464 | 71072 | 17766 | 0 |
| p05_divrem | 2 | PASS | 448459 | 71081 | 17765 | 0 |
| p06_shiftlogic | 0 | PASS | 447917 | 71075 | 17764 | 0 |
| p06_shiftlogic | 1 | PASS | 447917 | 71075 | 17764 | 0 |
| p06_shiftlogic | 2 | PASS | 447917 | 71075 | 17764 | 0 |
| p07_byteops | 0 | PASS | 447945 | 71080 | 17764 | 0 |
| p07_byteops | 1 | PASS | 447945 | 71080 | 17764 | 0 |
| p07_byteops | 2 | PASS | 447945 | 71080 | 17764 | 0 |
| p08_misaligned | 0 | PASS | 450075 | 71592 | 17799 | 5 |
| p08_misaligned | 1 | PASS | 450080 | 71592 | 17799 | 5 |
| p08_misaligned | 2 | PASS | 450080 | 71592 | 17799 | 5 |
| p09_storeload | 0 | PASS | 447966 | 71076 | 17764 | 0 |
| p09_storeload | 1 | PASS | 447966 | 71076 | 17764 | 0 |
| p09_storeload | 2 | PASS | 447966 | 71076 | 17764 | 0 |
| p10_jalr_link | 0 | PASS | 448144 | 71098 | 17775 | 0 |
| p10_jalr_link | 1 | PASS | 448144 | 71098 | 17775 | 0 |
| p10_jalr_link | 2 | PASS | 448144 | 71098 | 17775 | 0 |
| p11_bigmuldiv | 0 | PASS | 448315 | 71069 | 17765 | 0 |
| p11_bigmuldiv | 1 | PASS | 448315 | 71069 | 17765 | 0 |
| p11_bigmuldiv | 2 | PASS | 448312 | 71078 | 17764 | 0 |
| p12_memwalk | 0 | PASS | 448637 | 71245 | 17789 | 0 |
| p12_memwalk | 1 | PASS | 448672 | 71253 | 17790 | 0 |
| p12_memwalk | 2 | PASS | 448603 | 71237 | 17788 | 0 |
| p13_romstore | 0 | PASS | 448300 | 71162 | 17769 | 1 |
| p13_romstore | 1 | PASS | 448300 | 71162 | 17769 | 1 |
| p13_romstore | 2 | PASS | 448300 | 71162 | 17769 | 1 |

**Summary: 39 runs (13 programs × 3 inputs): 39 PASS, 0 FAIL, 0 STOPPED;
cycles = 17 484 988, retires = 2 774 478.** The case fails if any run fails,
stops, stalls or times out; `stopped` is counted separately from `fail` so a
program the machine cannot run is never averaged into a signature mismatch.

The matrix prints the first divergent signature word on any failure. On this
revision there is none, so every row prints `-`.

### The signature is not compared alone

Under `SWEEP_DEBUG=1` the driver additionally diffs the DUT's memory model
against the independent interpretation's, byte for byte, over the signature
area, the 128-byte scratch buffer and the program-input table. All 39 runs
report **0 mismatching bytes**:

```
$ SWEEP_DEBUG=1 build/p0/unit/core.corpus_sweep/core.corpus_sweep ... --only p12_memwalk
  [debug] p12_memwalk in0 ref=0x438587e9cc0e086c 0x23456789abcdea0f 0x0000000000000008 0x01234567a39081b7 \
          oracle-derived=0x438587e9cc0e086c ... match=1
  [debug] p12_memwalk in0 memory mismatching bytes in signature+scratch+inputs: 0; dut_retires=71245
  p12_memwalk    in0  PASS     cycles=448637   retires=71245   redir=17789 traps=0
```

## 3. Programs that cannot run, and what stops them

**None.** Every one of the 39 runs reached its own TOHOST and its signature
equals the oracle-derived expectation. That is itself the stated result: the
sweep is not "sweep complete" over a subset — it is 13 programs × 3 declared
inputs, every image the corpus builds, all executed from the reset vector.

The driver still implements the diagnostic, so a program that *could* not run
would be named rather than skipped. It reports the program, the refused
instruction (word and mnemonic), the PC, the cycle, the refusal counters and the
ROB occupancy at the stop, for example the `--control-illegal` control:

```
STOP  p02_branch in0: the machine refused instruction word 0x0000007f at
      pc=0x0000000080000000 (cycle 8) and stopped (unsupported=1, illegal=1,
      rob_occupied=0). The refused instruction is the oldest the front end
      delivered whose PC never retired.
```

The refused instruction is reconstructed from evidence, not assumed: the driver
tracks the front end's delivered instruction stream (in order) and the set of
PCs that retired; a stop quiesces the machine with an empty ROB, everything
dispatched before the refusal retires, and the oldest delivery whose PC never
retired is the one the dispatch refused.

## 4. The two programs the sweep exists for

### p12_memwalk — what it needs beyond a byte-array memory

p12 is the corpus's small workload: it builds a 16-word data-dependent
recurrence in the scratch buffer with 16 back-to-back `sd`s, walks part of it
back with a load-only loop whose trip count (`n = (c & 3) + 8`) comes from an
input, and folds loop branches, ALU and memory into one signature. What it needs
beyond a byte-array memory is exactly what makes it worth running:

* **A memory served the way the endpoint serves it.** The core's data port does
  not read an address; it reads the aligned doubleword the access selects
  (`mosaic_lsu_endpoint.sv`'s lane convention) and applies byte strobes to that
  window. p12's 16 doubleword stores and its walk loop's 8–11 doubleword loads
  must round-trip through that path — the load/store queues, the forwarding
  query, and the store queue's retirement-authorised drain — not merely through
  a flat array.
  All three input variants produce the oracle's signature and the DUT's scratch
  matches the interpreter's byte for byte.
* **The FROMHOST device rule and the doubleword window to agree.** This is the
  one place where "a byte array" is genuinely not enough, and it is a real
  trap: the memory model serves FROMHOST as a *device* — a read at exactly
  0x80001008 returns the input word, whatever its size — while the data port
  reads that address as an eight-byte window. A harness that seeds only the
  device register truncates the word: crt0's `ld t4, 0(t3)` sees only the input's
  low byte, and (when that byte is non-zero) writes that byte back into
  `mosaic_prog_inputs`. **This sweep's first version had exactly that defect**
  (see §6) and p01 failed because of it. The driver now seeds FROMHOST the way
  `core.trap_csr_program` does: the device register through `SetInputWord`, plus
  the high seven bytes into RAM so the doubleword window agrees.
* **The branch/redirect path**, for its two loops (a backward `bne` in the fill
  and a data-dependent `bgeu` trip test), and **the CSR/trap path**, because
  crt0 installs `mscratch`/`mtvec` before `main` — every corpus program is
  executed through that boot sequence, which is why running from the reset
  vector rather than from `main` matters.

### p13_romstore — the trap case's path, confirmed here

`p13_romstore` is already run by `core.trap_csr_program`, unmodified from
`_start`. This sweep runs the same three images through the same mechanism
independently:

| | trap case (`I-023-trap.md` §2.1) | this sweep |
|---|---|---|
| p13 in0..2 retires | 71162 | 71162 |
| p13 in0..2 traps | 1 (cause 7) | 1 |
| p13 in0..2 cycles | 448300 | 448300 |
| p13 in0..2 signature word 3 | `0x…0106` | `0x…0106` |

**The two evidence paths agree**, exactly, on all three inputs — including word
3, where the program's published value (`oracle ^ 1` = `0x…0106`) differs from
the oracle's printed row (`0x…0107`) for the documented armed-trap-resume
reason.

`p08_misaligned` also agrees with the trap case on the parts that are comparable:

| | trap case | this sweep |
|---|---|---|
| p08 retires | 71592 | 71592 |
| p08 traps | 5 (4,4,6,6,7) | 5 |
| p08 signature word 3 | in0 `0x101018181c000003`, in1/in2 `0x101018181c000002` | same |
| p08 cycles | in0 450080, in1/in2 450080 | in0 450075, in1/in2 450080 |

The only difference is p08 in0's cycle count, by five cycles: the two cases stop
their run loops on different conditions (this one stops as soon as the TOHOST
store is performed; the trap case stops when its predicted retirement stream is
exhausted), which moves the cycle at which the settle window begins. The retired
instruction counts and the trap counts — the DUT-side facts — are identical.

## 5. Controls: the failure path, fired

A sweep whose failure path has never fired is not evidence that the programs
pass. Three controls, none of them part of the registered run, each make the
case fail and are reported here with their exact command and output.

**(a) A perturbed expectation** — `--control-expect <program> <input> <word>`
XORs one bit into the oracle-derived expectation for one case after the program
relation is applied:

```
$ ... --control-expect p07_byteops 2 1        # exit 1
  ** CONTROL --control-expect p07_byteops in2 word 1: the expectation is deliberately wrong
  p07_byteops    in2  FAIL     cycles=447945   retires=71080   redir=17764 traps=0   first_divergent_word=1
FAIL  p07_byteops in2: first divergent signature word 1 --
      oracle-derived expectation 0xfffffffffffffffe, DUT 0xffffffffffffffff
RESULT FAIL core.corpus_sweep programs=13 inputs=3 runs=39 pass=38 fail=1 stopped=0 ...
```

The right word is named (word 1, the perturbed one), with the DUT's real value
beside the perturbed expectation.

**(b) A mis-set input pattern** — `--control-input <program> <input>` boots the
DUT on a different input than the oracle's row describes, by seeding its FROMHOST
with `a ^ 1`. This does not touch the expectation; the DUT genuinely computes a
different signature:

```
$ ... --control-input p12_memwalk 0           # exit 1
  ** CONTROL --control-input p12_memwalk in0: the DUT is booted on a different input ...
  p12_memwalk    in0  FAIL     ...  first_divergent_word=0
FAIL  p12_memwalk in0: first divergent signature word 0 --
      oracle-derived expectation 0x438587e9cc0e086c, DUT 0x438587e9cc0e088b
RESULT FAIL core.corpus_sweep ... pass=38 fail=1 stopped=0
```

A one-bit change of `a` shifts the walk's seed word and is caught at word 0 —
which is the point: the sweep compares exactly, and the whole chain from crt0's
input read through the memory walk to the signature stores is live.

**(c) A refused instruction** — `--control-illegal <program> <input>` patches a
`0x0000007f` word (opcode 1111111, `insn[1:0] = 11`: a well-formed 32-bit
instruction the decoder has no arm for) over the image's reset vector in the
driver's copy only, so the STOP diagnosis path is shown to fire:

```
$ ... --control-illegal p02_branch 0          # exit 1
  ** CONTROL --control-illegal p02_branch in0: the illegal 32-bit word 0x0000007f was planted ...
  p02_branch     in0  STOP     cycles=72       retires=0       redir=0    traps=0
STOP  p02_branch in0: the machine refused instruction word 0x0000007f at
      pc=0x0000000080000000 (cycle 8) and stopped (unsupported=1, illegal=1, rob_occupied=0).
RESULT FAIL core.corpus_sweep ... pass=38 fail=0 stopped=1
```

All three controls exit 1 and name the right location; the registered run exits
0 with all 39 rows PASS.

## 6. A defect this work found in itself, and fixed

The first version of the driver seeded FROMHOST with `MemoryModel::SetInputWord`
alone. Because the data port reads the doubleword window and the memory model
serves the device register at an exact address, the window came back as the
input's low byte in seven zero bytes: crt0's FROMHOST read returned `a & 0xff`,
and (when non-zero) wrote it into `mosaic_prog_inputs`. p01 in0 and in1 failed
with signatures that fitted `a0 = a & 0xff` exactly (in1: sig = `0x100, 0xfe,
0x80, 0x0`, the values `a0 = 0xff, a1 = 1, a2 = 1` produce); in2 passed only
because `a = 0x8000000000000000` has a zero low byte, so crt0's `beqz` took the
"no override" branch and the compiled input survived.

That is a harness defect, not an RTL one, and it is worth recording because it
is precisely the class of self-deception this sweep exists to avoid: the case
would have reported two "DUT failures" that were the harness truncating its own
input. The fix is `SeedFromhost()` — the device register plus the seven high
bytes seeded into RAM so the device rule and the doubleword window agree — which
is the same function `core.trap_csr_program` has used since it was written. No
file outside `sim/unit/tb_core_sweep.cpp` was changed for it. After the fix all
39 runs pass and the DUT's memory matches the interpreter's byte for byte.

## 7. An observation outside the corpus (no RTL changed)

While building control (c), planting an **all-zero word** (`0x00000000`, an
illegal encoding whose `insn[1:0] = 00`) at the reset vector did *not* stop the
machine. The run retired 71122 instructions — exactly one fewer than the healthy
71123 — with `unsupported = 0`, `illegal = 0`, `stopped = 0`, produced the
program's correct signature, and the DUT's memory matched the reference's byte
for byte; the independent interpreter stopped at the illegal word
(`ref_exited = false`). So the word was neither executed, nor refused, nor
trapped: the front end dropped it and the machine continued.

Two comments in the RTL describe the other behaviour —
`rtl/core/mosaic_fetch.sv` ("reported illegal, not decoded and not delivered …
fetch recognises the encoding, reports it, and stops") and `rtl/core/mosaic_core.sv`
("carried as an invalid control word, which dispatch refuses and counts, and the
machine stops cleanly at it") — and the observation is that the machine does not
stop. This is **not a corpus finding**: `tests/programs/audit/check_p0_isa.py`
proves every instruction in every corpus ELF is in `rv64im_zicsr_zifencei`, so
every word has `insn[1:0] = 11`, and no run above can reach it. It is flagged for
the front-end / I-041 lane, which owns the compressed-encoding contract; I did
not change any RTL for it, because the fix is a contract decision (report-and-
stop versus drop-and-continue) in a lane this package does not own, and this
sweep's evidence does not depend on it. Reproduction:

```
$ build/p0/unit/core.corpus_sweep/core.corpus_sweep --case core.corpus_sweep \
      --max-cycles 4000000 --only p02_branch --control-illegal p02_branch 0
```
with the planted word changed from `0x0000007f` to `0x00000000`.

## 8. Revision

Run against working-tree revision `382baf4` ("I-037 FENCE/FENCE.I recorded;
register the corpus sweep and V-013/V-014"), Verilator 5.052, macOS arm64.

Every RTL file the case compiles (the registry's list), SHA-256:

| File | SHA-256 |
|---|---|
| `rtl/core/mosaic_pkg.sv` | `3d6ab3533a44953a7dc66d70e15c3845d9c6a63234e071c82f84ce6b666944fc` |
| `rtl/core/mosaic_uop_pkg.sv` | `ea83748fcf35184c907daed4ea0d1e477980df6689ab1cbf086c0027bd7b61a2` |
| `rtl/core/mosaic_alu.sv` | `0a92b98eb4c835dc7565b1ebd6cfa5af323c8e203177e0d000d3f23c18c5c38e` |
| `rtl/core/mosaic_branch_cmp.sv` | `cf996100038627df14c2e4db364eaf94944a4bd325b8bb7dac22ed901f7d9f4e` |
| `rtl/core/mosaic_branch_target.sv` | `0e5f8ca86a4f256aaff4564330d91a35ed65450498c46c45a2ebd55c1b569942` |
| `rtl/core/mosaic_predictor.sv` | `bad709b88af95cbccef34ab5c668a630b0fff5e8cbb9e700322fd1cc04b2ed53` |
| `rtl/core/mosaic_fetch.sv` | `a83330c5c160cc1b413273ea800944f337e675a22c25d708203f3f523206517c` |
| `rtl/core/mosaic_decoder.sv` | `f27cdcf1ee28433f0d2b76c396702638563d413e490f133c0952d68380cb09c4` |
| `rtl/core/mosaic_rename.sv` | `180a1e35c4cc0a5ae5f96eb3b76fad2bc9af650f51ba6f437126c45b5c2a5d6b` |
| `rtl/core/mosaic_rob.sv` | `afba8cf6f4f954d1dfbbc39796baa2d263c8bbd0c7082b5425c8dcf39dd0d2c1` |
| `rtl/core/mosaic_iq.sv` | `447bfabdc3dc5b8c6bbf44fd61c5d37934ea7541fe854c5be55f308ada464240` |
| `rtl/core/mosaic_prf.sv` | `2e695d234e70a62381270433624d98a6b5a48f4fcf0f4aedbb775f218f3c82a6` |
| `rtl/core/mosaic_wb_arbiter.sv` | `0f6f3ab7a3425ffb16ce22b084d6f195e3f8c1898310ecfb4c2ef1cbd1d2e748` |
| `rtl/core/mosaic_macro_desc.sv` | `eab7879e000c5a0be3ca437e96573fb80f7f7da36357f3b239838bfd455567b6` |
| `rtl/core/mosaic_dispatch.sv` | `a40ce867e76a7a5097f28976cf58f7d0779f89ba9b697e26fd686406ba023acf` |
| `rtl/core/mosaic_cluster.sv` | `be1b41436d82f669741b7151649257c883bcefa7af2d3c88fbec9130a35025c4` |
| `rtl/core/mosaic_redirect_arb.sv` | `658aeb6b7e35bc43748829ed0686e361a27068e43e8d8d5eca6167c7b52eba91` |
| `rtl/core/mosaic_muldiv.sv` | `dd12e9ba3df579593d125790742ed39c5b0b5935be6ef634d2579dbf1f4f2d0d` |
| `rtl/core/mosaic_retire.sv` | `2745da49c116ea3846e74ebc92ef8c54c5b38a41c9d107161e9a992906c66583` |
| `rtl/core/mosaic_core.sv` | `6f8a9154b45c47f1f1f4456b9b02c5202045dbe733f9dd15a5f4bbc01e122ca6` |
| `rtl/core/mosaic_csr.sv` | `a159f5f39a5202945b2008e63df401827f65b29d941adf7d92386e712292f32a` |
| `rtl/core/mosaic_interrupt.sv` | `fb4fbfe2378c868b2ad98284a2bfc028853feff4730037b5b633a798c0a4919a` |

The rest of what the case compiles or depends on:

| File | SHA-256 |
|---|---|
| `sim/tb/mosaic_core_tb.sv` | `55c543e138cca4bd528fbb0047563b54804e785fd792e69f5db5696cc2eb9efa` |
| `sim/unit/tb_core_sweep.cpp` | `3a98593d75121b358dff49db07dc4ea72e14da5a41b8a97a26c2e3c4f4ac0095` |
| `sim/unit/mem_ref.h` | `21e4fd4248156016ee0ae6b6a57136c413e34f947223a402752b8a60db2e66f4` |
| `sim/unit/trap_ref.h` | `b0ce3f2721a193bafedc0e76493e6531a749390268dc8e1b77bc2526e253f8d6` |
| `sim/common/elf_loader.cpp` | `87786a8c186d6a7a8883d591bb5fa625084b9fc10745aab81cc746f2ec5a862f` |
| `sim/common/memory_model.cpp` | `827e06b8efe8795a1ab3e3323b8a88fa796f4fc5e479f3df6f7061753df56cde` |
| `sim/common/event_tap.cpp` | `25163bfd50f1d87a4ca2adb39041d45573421d09e154ac526e5936c1554ca660` |
| `sim/common/sim_common.cpp` | `8902c767f7f0c63e4ed44f486962a40ee653332bad83ff89f18bae82c84c30b6` |
| `tests/unit/registry.json` | `20c7f3eacaba2357519b166264506aeac6611d9e5e35defc3c2e7cf841c64774` |

The full RTL list was hashed before the first run and again after the last: no
file changed, so the numbers above are the final ones for this revision. Another
lane is wiring MMIO into the same core; if `rtl/core/mosaic_core.sv` (or anything
else in the list) moves, the sweep must be re-run and this report's matrix
replaced — the driver's first line prints the sources it was built from, and the
oracle cross-check would fail loudly on a core that no longer runs the corpus.

## 9. What this sweep cannot see

An honest list, because "39 PASS" is only as strong as the comparison behind it.

1. **It compares four signature words per program, not an instruction stream.**
   The signature is a 256-bit function of the program's inputs and its execution;
   a defect that changes a register, a store or a PC on a path that does not feed
   those four words is invisible here. `core.corpus_branch`, `core.mem_program`
   and `core.trap_csr_program` compare per-instruction retirement streams (pc,
   destination, value) for p02, p03/p09 and p08/p13 respectively — this case does
   **not**, and a defect those cases do not cover for the other nine programs
   would not be caught by either. The per-cycle invariants (§2) and the byte-level
   memory diff under `SWEEP_DEBUG` narrow the gap but do not close it.
2. **The memory diff is not part of the registered run.** All 39 runs were
   verified to have zero mismatching bytes in signature/scratch/inputs, but that
   is a `SWEEP_DEBUG=1` observation, not a check the registered case asserts. The
   registered case asserts only the signature words (and the reference's agreement
   with the oracle).
3. **No timing or throughput is asserted.** Cycles and retires are printed, not
   compared. A correct but much slower machine passes. Conversely, nothing here
   would catch a machine that retires extra dead instructions.
4. **Fixed, corpus-declared inputs, no randomness.** One seed is irrelevant (the
   runs are deterministic) and there is no random instruction stream: this case
   is not a fuzzer.
5. **No interrupts, no WFI, no external events.** The harness holds
   `irq_soft/timer/ext` low and `mtime` at 0 for every run, and crt0 leaves
   `mstatus.MIE = 0`. The interrupt path is `core.trap_csr_program`'s.
6. **No MMIO.** The data port is the harness's sparse platform model, not the
   SoC; the UART/CLINT are modelled to the extent the memory model implements
   them and no corpus program touches them. Another lane owns MMIO.
7. **FENCE / FENCE.I are not exercised** — no corpus program contains one
   (they are `CASE=fence.code_and_data_order`).
8. **Traps only in the forms p08/p13 arm**: two misaligned loads, two misaligned
   stores and a read-only-region store, plus the ECALL/EBREAK/MRET machinery
   crt0's handler uses. Nested traps, `mstatus` WARL behaviour, unimplemented
   CSRs, WFI and trap-during-interrupt are not reached.
9. **`mtval` is not checked.** p08/p13 fold the trap *causes* (positionally, via
   the log fold) into the signature; the records' `mtval` words are never
   compared. A wrong `mtval` that leaves the cause sequence intact passes here
   (the trap case checks the payload separately).
10. **X-propagation is not modelled as X.** The build uses
    `--x-assign unique --x-initial unique`, so an undriven or uninitialised value
    appears as a definite (wrong) value rather than as X; a defect whose only
    symptom is X would be seen as a value mismatch, or might be masked.
11. **The instruction memory is the image, not the platform.** Fetches outside
    the image answer ECALL; there is no instruction-side fault path, no cache and
    no compressed-instruction fetch. A fetch beyond the image would be judged by
    the exit protocol timeout, not by a fetch-fault comparison.
12. **Store visibility is not observed live.** The case reads the final memory
    image. `core.mem_program` checks transaction counts and store-drain ordering
    against a reference; this case does not, except through the final signature.

## 10. Gates

```
$ python3 tools/lint_rtl.py --profile p0        # 33 source file(s) clean
$ slang-tidy --std 1800-2017 --single-unit -I build/p0/rtl $(find rtl -name '*.sv')
                                                # exit 0, no errors (pre-existing
                                                # STYLE-2 port-suffix warnings only)
$ c++ -std=c++17 -fsyntax-only -Wall -Wextra -Wshadow ... sim/unit/tb_core_sweep.cpp
                                                # clean
```

No RTL was modified by this package, so the lint and slang results are
unchanged; the C++ lint is the driver's own and is clean.

`python3 tools/check_records.py` still reports one paperwork inconsistency, and
it is not this report's to fix (the ledger is
`config/status/implementation_status.json`, which this package may not edit):
`registry: case core.corpus_sweep belongs to delivered package I-023, which does
not list it as evidence`. I-023 is marked delivered and does not yet list the
new case among the cases it claims as evidence; adding it is an edit for the
integration lead. The inconsistency predates this work (the case was registered
with the package already delivered) and does not affect the case's result.
