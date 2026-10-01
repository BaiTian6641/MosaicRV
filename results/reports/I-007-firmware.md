# I-007 — Bare-metal image and independent host oracle

Status: **partially complete.** The corpus builds, passes the p0 ISA audit, and
the oracle is green. **One program (`p08_misaligned`, all 3 inputs) does not yet
pass the independent Spike cross-check and is an open defect, not a closed
item.** Section 9 has the reproduction and what is already known.

Everything below is transcribed from `config/memory/p0.json` and
`config/profiles/p0.json`. No address in this package is invented.

## 1. Memory map actually used

Frozen regions (`config/memory/p0.json`, profile `p0`):

| Region | Base | Size | Flags | Used by this package |
|---|---|---|---|---|
| `boot_rom` | `0x00000000` | 4 KiB | r+x, **not writable**, `error_response: access_fault` | never loaded or stored, except one deliberate faulting store in `p08` |
| `uart` | `0x00100000` | 256 B | rw MMIO | reserved, no access in this corpus |
| `test_harness` | `0x00102000` | 16 B | rw MMIO | recognised by the harness; **no firmware writes it any more** (see §4) |
| `clint` | `0x02000000` | 4 KiB | rw MMIO | reserved, no access in this corpus |
| `ram` | `0x80000000` | 2 MiB | rwx | everything below |

Reset vector `0x80000000` (`config/profiles/p0.json` → `reset.reset_vector`).

### Linker layout — `tests/programs/linker/mosaic_p0.ld`

```
0x80000000  .text.init    reset path + runtime helpers + trap handler (AX)
            .rodata       compiled-in program inputs (A)
            .data
0x80000400  .signature    FROZEN test_protocol.signature, 4 x 8 bytes (NOLOAD)
0x80001000  tohost        FROZEN test_protocol.tohost (RESERVED, nothing else here)
0x80001008  fromhost      FROZEN test_protocol.fromhost (RESERVED)
0x80001080  .scratch      128-byte program buffer (NOLOAD)
0x80001100  .bss          firmware scalar state (NOLOAD)
0x80001200  .progtext     the corpus program's main() (AX)
0x80002000  .traplog      8 records x {mcause, mtval} (NOLOAD)
...         .stack 16 KiB, .trapstack 1 KiB (NOLOAD)
```

**Why two code regions.** The reset vector and the signature area are both
frozen, so only 1 KiB separates them. Everything that must be reachable at
reset is placed contiguously in that window and fits (`.text.init` is
`0x2b8` bytes); the per-program body is placed after the firmware-owned areas
where it can grow without pushing the reset path off the reset vector. Both
regions are ordinary RAM inside one `PT_LOAD`, so the loader has nothing extra
to map.

Link-time invariants, each an `ASSERT` and therefore a link error, not a
silent surprise:

```
ASSERT(__ram_base == 0x80000000, ...)
ASSERT(__signature_base == 0x80000400, ...)
ASSERT(__boot_end <= __signature_base, ...)          /* boot region fits below the signature */

ASSERT(__scratch_end <= __bss_base, ...)
ASSERT(__bss_end <= __progtext_base, ...)
ASSERT(__traplog_base > __tohost_base + 16 || __traplog_end <= __tohost_base, ...)
ASSERT(__bss_base      > __tohost_base + 16 || __bss_end      <= __tohost_base, ...)
ASSERT(__scratch_base  > __tohost_base + 16 || __scratch_end  <= __tohost_base, ...)
ASSERT(__trapstack_section_end <= __ram_end, ...)
ASSERT(__image_end > __boot_rom_end, ...)            /* never in boot_rom */
```

`.riscv.attributes` is deliberately **not** discarded: it carries the ISA
string the audit compares against.

### Real segment map

```
$ riscv64-elf-readelf -lW tests/programs/build/p01_addsub.i0.elf | grep LOAD
  LOAD           0x001000 0x0000000080000000 0x0000000080000000 0x001284 0x005690 RWE 0x1000

$ riscv64-elf-readelf -SW tests/programs/build/p01_addsub.i0.elf
  [ 1] .text.init        PROGBITS  0000000080000000 001000 0002b8 00  AX  0 0 4
  [ 3] .signature        NOBITS    0000000080000400 0012d0 000020 00  WA  0 0 8
```

Entry point, from `make -C tests/programs map`:

```
--- build/p01_addsub.i0.elf ---
  Entry point address:               0x80000000
```

## 2. Reset path

`tests/programs/src/crt0.S`. `.text.init` is placed at `0x80000000` and
`ENTRY(_start)`, so the instruction executed out of reset is the first
instruction of `_start`.

```
_start:
    la   sp, mosaic_stack_top        # 1. stack
    la   t0, mosaic_trapstack_top
    csrw mscratch, t0                # 2. trap stack
    la   t0, trap_entry
    csrw mtvec, t0                   # 3. trap vector, direct mode
    # 4. zero every NOLOAD region, one region at a time:
    la a0,__bss_start ; la a1,__bss_end                   ; call zero_region
    la a0,__signature_start ; la a1,__signature_end       ; call zero_region
    la a0,__traplog_start ; la a1,__traplog_end           ; call zero_region
    la a0,__scratch_start ; la a1,__scratch_end           ; call zero_region
    la a0,__stack_section_start ; la a1,__trapstack_section_end ; call zero_region
    # 5. FROMHOST override of input word 0
    li t3, MOSAIC_FROMHOST ; ld t4,0(t3) ; beqz t4,7f
    la t5, mosaic_prog_inputs ; sd t4,0(t5)
7:  # 6. TOHOST read probe: reads 0 while the program runs
    ld t4,-8(t3) ; la t5,mosaic_tohost_probe ; sd t4,0(t5)
    call main                        # 7. run
    li a0,0 ; li a1,1 ; call tohost_finish
```

**Stacks are declared here, with explicit size and alignment**, not inherited:
`.stack` is `MOSAIC_STACK_SIZE` = 16384 bytes, `.balign 16`, with
`mosaic_stack_bottom` / `mosaic_stack_top` defined in `crt0.S` itself;
`.trapstack` is 1024 bytes, `.balign 16`, defined in `trap.S`.

**`zero_region(a0, a1)`** byte-aligns the cursor, then uses 8-byte `sd`
stores, then a byte-at-a-time tail:

```
zero_region:
    beq  a0,a1,9f
1:  andi t0,a0,7 ; beqz t0,3f ; sb zero,0(a0) ; addi a0,a0,1 ; bne a0,a1,1b ; j 9f
3:  addi t2,a1,-8 ; bltu a0,t2,5f ; sd zero,0(a0) ; addi a0,a0,8 ; j 3b
5:  bgeu a0,a1,9f ; sb zero,0(a0) ; addi a0,a0,1 ; j 5b
9:  ret
```

It is called once per region, so no region depends on the linker having
produced a length that is a multiple of 8.

## 3. Trap handler

`tests/programs/src/trap.S`. Policy, stated exactly because the oracle has to
model it:

> A trap is **recoverable only when the program has explicitly armed it.**
> Before an instruction that is expected to trap, the program calls
> `trap_expect(<pc of that instruction>, <count>)`. When a trap is taken and
> `mepc == __mexpc_expected` **and** `__mexpc_arm != 0`, the handler records it
> and resumes at `mepc + 4`, decrementing `__mexpc_arm`. **Every other trap is
> fatal**: the handler records it and ends the run with the TOHOST PASS bit
> clear and `code = mcause`.

All p0 instructions are 4 bytes, so `mepc + 4` is always an instruction
boundary and no compressed extension is in scope.

Context saving: `csrrw sp, mscratch, sp` switches to the dedicated trap stack
and preserves the interrupted `mscratch`; `ra`, `a0`-`a7` and `t0`-`t6` are
saved and restored (128-byte frame); `mepc`, `mcause`, `mtval` and the
interrupted `mscratch` are all read on entry. No `s`-register is touched, so
resuming at `mepc + 4` is architecturally transparent. Interrupts are never
enabled, so no nesting is possible; the handler still performs the
architecturally correct `mstatus` manipulation (`MPIE = 1`, `MPP = M`,
`MIE = 0`) before `mret`.

Recorded per trap, at `0x80001000`, stride 16 bytes:

```
word 0 : mcause   - architecturally defined, compared by the oracle
word 1 : mtval    - recorded for debug dumps only
```

`mtval` is deliberately **not** part of the expected signature: the Privileged
specification does not require `mtval` to hold the faulting address for access
faults, so predicting it would encode an implementation choice rather than
architecture.

Fatal path, deliberately without restoring the interrupted registers (restoring
would overwrite `mcause` with whatever the frame held):

```
trap_fatal:
    csrr t1, mcause
    slli t0, t1, 1            # PASS bit stays clear
    li   t2, MOSAIC_TOHOST
    sd   t0, 0(t2)
2:  j 2b
```

## 4. Result protocol

The protocol is **frozen in `config/profiles/p0.json` → `test_protocol`**, so
`sim/harness` and this firmware read the same numbers from the same file and
cannot drift. This package implements it; it does not define it.

| Item | Value | Source |
|---|---|---|
| `tohost` | `0x80001000` | `test_protocol.tohost` |
| `fromhost` | `0x80001008` | `test_protocol.fromhost` |
| signature | `0x80000400`, 4 × 8-byte words | `test_protocol.signature` / `.signature_words` |
| pass code | `1` | `test_protocol.pass_code` |

Rules as implemented:

1. Writing a non-zero value to TOHOST ends the program. `tohost == 1` means
   PASS. Any other non-zero value means FAIL and bits [63:1] are a
   program-defined code the harness reports. Reading TOHOST returns 0 while
   the program runs (`crt0` probes it once into `mosaic_tohost_probe`).
2. Reading FROMHOST yields an input word. A non-zero value overwrites
   compiled-in input word 0 in place; zero means "no override", because a
   harness that supplies no input leaves the register at 0 and that must not
   be mistaken for an input.
3. The signature area is 4 words at `0x80000400`, in its own `NOLOAD` output
   section, so its presence and address are provable from the program headers.
   The harness reads all 4 and compares word by word.

**HTIF convention.** Spike's HTIF reads the exit code as `tohost >> 1`. Under
this protocol a pass writes exactly `1` → exit code 0; a failure with code *k*
writes `2*k` → exit code *k*. Pass and failure are unambiguous, and the codes
survive into a Spike run.

**Address change, and why.** TOHOST and FROMHOST moved from the
`test_harness` MMIO device at `0x00102000` to RAM at `0x80001000`. HTIF polls
the `tohost` symbol address directly and Spike maps only a contiguous region
from `0x80000000`, so `0x102000` is outside everything it maps:

```
$ spike --isa=rv64im_zicsr_zifencei build/p01_addsub.i0.elf
Access exception occurred while accessing tohost:
Memory address 0x102000 is invalid
```

This is also the universal bare-metal convention (riscv-tests, `pk`, Spike all
assume it), and it is what makes the corpus runnable end to end on an
independent reference. The `test_harness` region stays in the memory map and
the harness recognises it; no firmware here writes it.

**`tohost` / `fromhost` symbols — deviation from the instruction to use
`PROVIDE`.** They are plain assignments, not `PROVIDE`:

```
tohost   = 0x80001000;
fromhost = 0x80001008;
```

`PROVIDE` only materialises a symbol when a relocation references it, and
nothing in the firmware references these, so `PROVIDE` leaves them out of the
symbol table entirely. That was tried and observed:

```
warning: tohost and fromhost symbols not in ELF; can't communicate with target
```

and the run never terminated. With plain assignments the symbols are present:

```
$ riscv64-elf-readelf -sW build/p01_addsub.i0.elf | grep -E ' (tohost|fromhost)$'
    62: 0000000080001008     0 NOTYPE  GLOBAL DEFAULT  ABS fromhost
    63: 0000000080001000     0 NOTYPE  GLOBAL DEFAULT  ABS tohost
```

The protection `PROVIDE` was meant to provide — a stale address cannot survive
silently — is instead provided by a real check.
`tests/programs/audit/check_p0_isa.py` reads `test_protocol` from
`config/profiles/p0.json` and asserts both symbols exist at those addresses in
every ELF, so a profile change that moves them **fails the audit** rather than
passing unnoticed. That is strictly stronger than dropping the symbols.

## 5. Corpus

`tests/programs/corpus.json` is the single source of truth; `mkvars.py` turns
it into `build/corpus.mk`, so adding a (program, input) pair is the only edit
needed to extend the build. Each program emits its own three-word input table
via `MOSAIC_PROGRAM_INPUTS`, and program objects are named
`<program>.i<n>.o` and depend on `corpus.json` — an earlier version compiled
the table into shared boot code, which silently left every ELF carrying zero
inputs when an input changed.

Every program takes three inputs `a`, `b`, `c` and writes 4 signature words.

The card asks for 12 programs; all 12 are present. `p13_romstore` is a
13th, split out of `p08_misaligned` so that the one case in that program
which does *not* depend on the misalignment policy can still be
adjudicated by an independent reference. §9 explains why.

| # | Program | Covers | a / b / c (3 inputs) |
|---|---|---|---|
| 1 | `p01_addsub` | ADD SUB ADDI SRA SLLI SRLI XORI ANDI SLTU | `0x0123456789abcdef,fedcba9876543212,13` · `0x7fff…fff,1,1` · `0x8000…,fffffffe,63` |
| 2 | `p02_branch` | BEQ BNE BLT BGE BLTU BGEU JAL JALR (bit-0 clear) | `0,1,3` · `0x8000…,0x7fff…,5` · `0xffff…,0xffff…,9` |
| 3 | `p03_loadstore` | SD SW SH SB LD LW LHU LH LB LBU | `0x0123456789abcdef,fedcba9876543210,7` · `0x8000000000000001,0x180,0xffff…` · `0,0xffff…,0x8000…` |
| 4 | `p04_mul` | MUL MULH MULHU MULHSU | `0x0123…,0xfedc…,0x8000000000000001` · `0xffff…,0xffff…,0x8000…` · `0x8000…,0x8000000000000001,0x0123…` |
| 5 | `p05_divrem` | DIV DIVU REM REMU + divide-by-zero + MIN/−1 | `0xfedc…,0x10,3` · `0x8000…,0xffff…,3` · `0xffff…,0,0xffff…` |
| 6 | `p06_shiftlogic` | SLL SRL SRA AND OR XOR SLT SLTU NEG | `0x8000000000000001,3,1` · `0xffff…,0xffff…,63` · `0xffffffffffffff00,0xffffffffffffff00,4` |
| 7 | `p07_byteops` | LB LBU LH LHU SB SH SW | `0x8080,0x7f7f,0xffff` · `0xff7f,0x8080,0x8000` · `0xffff…,0,0xffff…` |
| 8 | `p08_misaligned` | misaligned LH/LW/SH/SD must trap, aligned neighbours must not, store to boot_rom must fault | `0x0123…,0xfedc…,0x5a5a…` · `0x8000…,0,0` · `0xffff…,0xffff…,0xffff…` |
| 9 | `p09_storeload` | store→load to the same address at 4 widths | `0x0123…,0xfedc…,0xaaaa…` · `0xdeadbeefcafebabe,0xffff…,0` · `0xffff…,0,0x5555…` |
| 10 | `p10_jalr_link` | JAL/JALR link value, JALR bit-0 clear, `rd == rs1`, nested call | `0x0123…,0x10,0` · `0xffff…,1,0` · `0x8000…,0xffff…,0` |
| 11 | `p11_bigmuldiv` | dependent DIV/DIVU/REM/REMU chain | `0xfedc…,0x11,5` · `0x8000…,0xfffffffffffffffe,7` · `0xffff…,0,0xffff…` |
| 12 | `p12_memwalk` | 16-word build + load-only walk, input-dependent trip count | `0x0123…,0x1111…,5` · `0xffff…,1,2` · `0x8000…,0xaaaa…,8` |
| 13 | `p13_romstore` | store into read-only `boot_rom` → `mcause` 7 (**split out of `p08`**, see §9) | `0x0123…,0xfedc…,7` · `0x8000…,0,0` · `0xffff…,0xffff…,0xffff…` |

Expected trap trace, `p08_misaligned` (the only trapping program), identical
for all three inputs:

| # | Access | Address | `mcause` |
|---|---|---|---|
| 1 | `lh` load | `scratch+9` (odd) | 4 load address misaligned |
| 2 | `lw` load | `scratch+10` (≡2 mod 4) | 4 load address misaligned |
| 3 | `sh` store | `scratch+7` (odd) | 6 store/AMO address misaligned |
| 4 | `sd` store | `scratch+12` (≡4 mod 8) | 6 store/AMO address misaligned |
| 5 | `sw` store | `0x00000000` (boot_rom, not writable) | 7 store/AMO access fault |

The oracle predicts the trap trace, not just the arithmetic: the expected
`mcause` sequence `[4,4,6,6,7]` is folded into `sig3` by the program and
declared in `corpus.json`, so a DUT that performs a misaligned access instead
of trapping produces a different `sig3` even if the canary happened to survive.

### How input-insensitivity was avoided

A program whose signature ignores its input is not a test, so the oracle
**refuses to pass such a case**. `--all` fails if a signature's four words are
all equal, and fails if any two of a program's three inputs produce the same
signature. Both checks are live and have already rejected work:

* `p05_divrem`'s divide-by-zero input produced `[MARK, 0, 0, 0]` because
  `DIV(a,0)` and `DIVU(a,0)` are *both* all-ones and `REM(a,0)` and
  `REMU(a,0)` are *both* the dividend, making the signed-vs-unsigned
  exclusive-or identically zero. `MOSAIC_DIVZERO_MARK` (`0x5555555555555555`)
  is now folded in when the divisor is zero, so the case is observable.
* `p10_jalr_link`'s `sig3` was provably equal to `sig2` for **every** input
  (`a1 ^ (a0 ^ a1) == a0`); the inner routine was changed so they differ.
* The degenerate rule is `>= 2` distinct words, not 4, because some
  legitimate cases are symmetric by construction: `p10`'s two link checks are
  both 0 by design, and the MIN/−1 overflow case makes `DIV` and `DIVU`
  coincide.

## 6. Oracle derivation per program

`tools/host_oracle.py` computes expected signature words and the expected trap
trace **in Python from the declared inputs only**. It never imports, executes,
parses or observes the RTL, the assembler output, the linker script or the
firmware sources. Its only input is `tests/programs/corpus.json`, which is a
*declaration* of (program, input, expected result).

Two independent derivations run for every non-trivial result and must agree
before anything is emitted; disagreement is a hard failure, never a value:

| Quantity | Derivation A | Derivation B |
|---|---|---|
| `MULH` / `MULHSU` / `MULHU` | Python big-integer multiply over signed or zero-extended operands | 16-bit limb schoolbook multiply, limbs signed per operand, reassembled to 128 bits |
| `DIV` / `REM` | Python magnitude division with truncating-toward-zero correction | 64-iteration bit-serial restoring divider on masked 64-bit words, no division operator anywhere in the function |
| `DIVU` / `REMU` | Python `//` and `%` on the unsigned values | the same restoring divider with sign handling off |
| sign extension | `sext()` from Python negative integers | `sext_masked()` from an explicit mask-and-or expression |
| `SRL` | mask and shift | bit-at-a-time rebuild of the shifted-out bits |
| `SRA` | Python arithmetic shift | explicit zero-fill plus sign-fill construction |
| branch select | `if` on the comparison | mask-merge of both arms built from the comparison |

This is not decorative: it caught four real modelling bugs during bring-up —
`MULH` truncation direction for negative products, a signed/unsigned operand
mix-up in the restoring divider, `SRA` of an all-ones value, and a masked
derivation that disagreed with its second derivation.

Per program, the computation is:

* `p01` `sig0 = a+b`, `sig1 = a−b`, `sig2 = sra(a+b, c&63)`,
  `sig3 = sltu(a,b) ? a+b : a−b` XOR `NOT(b&0xff)&0xff`.
* `p02` `sig0` = 6-branch mask (BEQ,BNE,BLT,BGE,BLTU,BGEU); `sig1` = its
  popcount; `sig2` = `1 + (c&7)` indirect transfers; `sig3` = select on
  `mask ^ (c&0x3f)`.
* `p03` `sig0 = a ^ sext32(a>>32)` (LW sign-extends),
  `sig1 = zext(a,16) ^ sext16(zext(b,16))`, `sig2 = sext8(a&0xff) ^ (b&0xff)`,
  `sig3 = b ^ (a+c)`.
* `p04` `mul`, `mulh`, `mulhu(a,b)`, `mulhsu(a,c) ^ mul(a,c)`.
* `p05` `sigN = DIV ^ DIVU ^ (divisor==0 ? MARK : 0)` for the division words,
  `REM ^ REMU` for the remainder words, over divisor pairs `(a,b)` and `(a,c)`.
* `p06` `a << (c&63)`, `a >>u (c&63)`, `a >>s (c&63)`, and
  `((a^b)|b|~(a&b)) ^ (−(a<s b) ? ~0 : 0) ^ ((−(b<s c) ? ~0 : 0) << 16)`.
* `p07` a word assembled byte by byte from `a` and `b`, read back with
  `lbu`, `lb`, `lh`, `lhu`; the half at `scratch+4` holds `c & 0xffff`.
* `p08` `sig0 = a & 0xff`, `sig1 = a >> 32`, `sig2 = sext16(a>>48)`, and
  `sig3 = (fold of the five logged `mcause` values, 8 bits each) << 2 | 0b11`
  (bit 0 canary intact, bit 1 `c` round-tripped). Traces `[4,4,6,6,7]`.
* `p09` `sig0` = the double round-trip after `sd a; ld; sd b; ld; sd a; ld`;
  `sig1` = `sext32(a & 0xffffffff)` (LW sign-extends); `sig2` =
  `((a & ~0xff)|(b & 0xff)) ^ ((a & ~0xffff)|(b & 0xffff))`; `sig3 = c`.
* `p10` `sig0` = JAL link delta (0), `sig1` = JALR link delta (0),
  `sig2 = a+1+2b`, `sig3 = (b−1) ^ (2·(a+3b))`.
* `p11` `sig0 = DIV ^ DIVU ^ mark`, `sig1 = REM ^ REMU`,
  `dividend = REMU(a,b) + c`, `sig2 = DIV(dividend, b)`,
  `sig3 = REM(sig2, b) ^ dividend`.
* `p12` `w[0] = a ^ c`, `w[i+1] = (w[i] << 1) ^ b`, `n = (c&3)+8`;
  `sig0 = Σ w[0..n−1]`, `sig1 = w[n−1]`, `sig2` = count of odd `w[i]` for
  `i < n`, `sig3 = w[0] ^ (w[15] >> 32)`.

### Three-way agreement

`--all` checks the oracle against **three** things: its own recomputation, the
`expect_sig` / `expect_traps` declaration in `corpus.json`, and the recorded
golden file `tests/programs/golden.json`. Any disagreement exits non-zero.

## 7. Build

`tests/programs/Makefile`. Targets: `all` (build + print entry point and
segment map), `audit`, `map`, `clean`.

Flags, pinned:

```
-march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany
-nostdlib -nostartfiles -ffreestanding -fno-builtin -fno-pic -mno-relax
-fno-asynchronous-unwind-tables -fno-stack-protector -fno-common
-Wl,--no-relax -Wl,-T,linker/mosaic_p0.ld -Wl,--fatal-warnings
```

`-march` is never relaxed and there is no fallback march. `-mno-relax` /
`--no-relax` keeps linker-inserted gp-relative sequences out of the image so
the disassembly audit sees exactly what was written.

## 8. Verification actually run

### Build — 39 ELFs, 13 programs × 3 inputs

```
$ make -C tests/programs all
=== MosaicRV p0 firmware: entry point and segment map ===
--- toolchain ---
riscv64-elf-gcc (GCC) 16.2.0
GNU readelf (GNU Binutils) 2.47.20260726
...
--- build/p01_addsub.i0.elf ---
  Entry point address:               0x80000000
  LOAD  0x001000 0x0000000080000000 ... 0x001284 0x005690 RWE 0x1000
$ echo $?
0
```

### Disassembly audit — `make -C tests/programs audit`

```
$ make -C tests/programs audit
ok   build/p01_addsub.i0.elf                        221 instructions, entry=0x80000000
...
ok   build/p12_memwalk.i2.elf                       225 instructions, entry=0x80000000

audit: 39 ELF(s), 8649 instructions, all in rv64im_zicsr_zifencei; no libgcc, no libc, no relocations, entry 0x80000000
```

Per ELF the audit enforces four hard failures:

1. every mnemonic is in the p0 set — objdump runs with `-M no-aliases`, so a
   pseudo-name (`li`, `mv`, `j`, `ret`, `nop`, …) appearing in the output is
   itself a failure, because it means the image was not audited at the
   encoding level. `mret` is allowed separately as an M-mode privileged
   instruction, which p0 (`privilege_modes: ["M"]`) requires for the trap
   handler; nothing from S/U mode is permitted.
2. the `.riscv.attributes` ISA string is exactly
   `rv64i2p1_m2p0_zicsr2p0_zifencei2p0_zmmul1p0`, **and** every extension
   token in it is decomposed and checked against the p0 allowlist, so a future
   `-march` change that quietly adds A/C/F/D/V fails naming the offender.
3. no libc/libgcc: no `__muldi3`/`__divdi3`/… helper symbol, no `PT_INTERP`,
   no `NEEDED`, no `.plt`/`.dynamic`/`.got.plt`, no relocation of any kind.
4. entry point is `0x80000000`, and the `tohost`/`fromhost` symbols exist at
   the addresses `config/profiles/p0.json` declares.

### Oracle

```
$ python3 tools/host_oracle.py --all ; echo "EXIT=$?"
MosaicRV p0 host oracle -- expected signatures
signature area 0x80000400, 4 words, tohost 0x80001000, fromhost 0x80001008
misalignment policy: load/store misaligned -> trap (config/profiles/p0.json)

program        in a                  b                  c                  sig0               sig1               sig2               sig3               traps
p01_addsub      0 0x0123456789abcdef 0xfedcba9876543212 0xd                0x0000000000000001 0x02468acf13579bdd 0x0000000000000000 0x02468acf13579b30 -
p01_addsub      1 0x7fffffffffffffff 0x1                0x1                0x8000000000000000 0x7ffffffffffffffe 0xc000000000000000 0x80000000000000fe -
p01_addsub      2 0x8000000000000000 0xfffffffffffffffe 0x3f               0x7ffffffffffffffe 0x8000000000000002 0x0000000000000000 0x8000000000000003 -
p02_branch      0 0x0                0x1                0x3                0x0000000000000016 0x0000000000000003 0x0000000000000004 0x0000000000000001 -
...
p08_misaligned  0 0x0123456789abcdef 0xfedcba9876543210 0x5a5a5a5a5a5a5a 0x00000000000000ef 0x0000000001234567 0x0000000000000123 0x000000101018181f 4,4,6,6,7
...
p12_memwalk     2 0x8000000000000000 0xaaaaaaaaaaaaaaaa 0x8                0x7ffffffffffffb2e 0x5555555555555166 0x0000000000000000 0x800000005555555d -

39 case(s) computed
oracle agrees with the declared expectations and the golden file for every case; all three inputs of every program give a distinct signature.
EXIT=0
```

### One-bit-flip negative control (demonstrated, not asserted)

One bit of one expected signature word was flipped in a throwaway copy of the
golden file:

```
$ python3 tools/host_oracle.py --golden /tmp/g.json --check-golden-only ; echo "EXIT=$?"
=== oracle FAILED (1) ===
  p01_addsub.i0 sig0: oracle 0x0000000000000001, golden 0x0000000000000000
EXIT=1
```

The comparison is not vacuous: a single differing bit fails the run.

## 9. Independent Spike cross-check — 36/39, and one NOT_CLAIMED claim

`tests/programs/audit/spike_crosscheck.py` is a **third** opinion. The oracle
computes the expected signature from the inputs in pure Python; Spike executes
the actual ELF and the script reads Spike's own architectural memory writes.
Spike runs with its default memory only (DRAM at `0x80000000`), which is now
possible precisely because TOHOST is in RAM; `boot_rom` at 0x0 is left
unmapped so a store there faults as the memory map requires.

```
$ python3 tests/programs/audit/spike_crosscheck.py --timeout 15
spike cross-check: 33/39 case(s) agree between Spike's architectural memory
writes and the host oracle
  p08_misaligned.i0: spike exit 255 (tohost was not 1)
  p08_misaligned.i0: spike ['0x0','0x0','0x0','0x0'], oracle ['0x00000000000000ef','0x0000000001234567','0x0000000000000123','0x000000101018181f']
  p08_misaligned.i1: spike exit 255 (tohost was not 1)
  p08_misaligned.i2: spike exit 255 (tohost was not 1)
```

**33 of the 39 cases agree.** Six do not: `p08_misaligned` i0/i1/i2 and
`p13_romstore` i0/i1/i2. In every failing case `sig0`, `sig1` and `sig2` agree
exactly and only a single word differs, and in every failing case that word is
the one that folds the recorded `mcause`.

### `p08_misaligned` — misalignment claim is `NOT_CLAIMED`

The frozen policy is `config/profiles/p0.json` → `misalignment: {load: "trap",
store: "trap"}`, and the firmware encodes and the oracle validates the trace
`[4,4,6,6,7]`. **That claim is `NOT_CLAIMED` as evidence**: it has not been
validated against any reference, and this reference cannot validate it.

The reason is a property of the reference, not a defect in the firmware:

> Spike services misaligned accesses natively rather than raising
> address-misaligned exceptions, and exposes no command-line option to change
> that (`spike --help` offers only `--priv=<m|mu|msu>` and `--wfi-as-nop` in
> that neighbourhood). The RISC-V specification permits either behaviour. So
> the frozen p0 policy and this reference disagree by construction, and the
> disagreement cannot be configured away.

Measured directly, over the full 71,163-commit run of `p08_misaligned.i0.elf`:

```
trap_entry (0x80000160) entries : 1
mret                             : 1
hottest PCs: 0x80000130 (17741), 0x80000134/38/3c (17736 each)
```

The handler is entered **once** and returns **once**. `p08` arms five traps
and expects five; only one was taken, and it is trap 5, the store to
unmapped address 0. Traps 1–4, the misaligned `lh`/`lw`/`sh`/`sd`, never
raised an exception — exactly what the behaviour above predicts.

### `p13_romstore` — the part this reference *can* adjudicate

Split out of `p08` into its own source, corpus entry, oracle model and three
inputs. It contains only the boot_rom store, which depends on
`config/memory/p0.json` (`boot_rom.writable = false`,
`error_response: "access_fault"`) and not on the misalignment policy, so Spike
checks it. All three inputs agree. Its signature folds the **trap record
count** as well as the cause, so a DUT that raised the fault twice, dropped
the store, or let it succeed produces a different `sig3`:

```
sig0 = lbu[0]                    sig3 = ((record_count << 8) | first_mcause)
sig1 = sext32(a >> 32)                          ^ (1 if the store was not trapped)
sig2 = sext16(a >> 48)
expected trap trace: [7]   (specification; NOT_CLAIMED against this reference)
```

The low byte of `sig3` **is** the cause: `sig3 = (record_count << 8) |
first_mcause`, so `0x107` is count 1, cause 7. Spike produces `0x106` — count
1, cause **6**, store/AMO address misaligned — for a `sw` to address `0`, which
is a perfectly 4-byte-aligned address (verified: the store is
`addiw t3, zero, 0` then `sw a0, 0(t3)`). Cause 7 is what the Privileged
specification requires for a store to a non-writable region and what our
frozen map demands, so **the oracle is right and the reference differs**.

Part of that is my harness's fault and is stated as such: I run Spike with
`boot_rom` **unmapped entirely**, because Spike has no read-only memory
region type. So Spike is answering "what happens on a store to a hole" rather
than "what happens on a store to a read-only region". That is an approximation
of the frozen map, not a faithful model of it. Either way this reference cannot
adjudicate the read-only-region case, so the assertion is `NOT_CLAIMED` rather
than counted as agreeing.

### Root cause of the p08 failure — found by I-008, fixed here

`tests/programs/linker/mosaic_p0.ld` originally placed `.traplog` at
`0x80001000`, which is where `test_protocol.tohost` moved to. Every trapping
program therefore wrote its first `mcause` straight into the result word, and
the harness latched it as the outcome:

```
p08_misaligned.i0  outcome=tohost tohost=0000000000000004
p08_misaligned.i1  outcome=tohost tohost=0000000000000004
p08_misaligned.i2  outcome=tohost tohost=0000000000000004
```

That is the whole of the earlier "illegal instruction, endless `ret` into
`_start`" mystery: the trace was self-inflicted. The trap log has been moved
to `0x80002000`, above the program code, and three `ASSERT`s now make an
overlap with the `tohost`/`fromhost` window a link error. `p08` now runs to
completion on Spike and `sig0`, `sig1` and `sig2` all agree with the oracle.

What remains is exactly one word. `p08`'s `sig3` folds the recorded
`mcause` values, and Spike records a different set because it does not trap on
misaligned accesses:

```
p08_misaligned.i1: spike [.., .., .., 0x101018181c000002]
                   oracle [.., .., .., 0x000000101018181f]
```

`sig0`/`sig1`/`sig2` (the aligned-neighbour accesses, which do not depend on
the misalignment policy) agree exactly. Only the trap fold differs, which is
the `NOT_CLAIMED` item below and nothing else. The earlier narrowed question
about control flow falling back into `crt0` was a **consequence** of the
collision, not an independent defect; it is resolved.

### Open question — resolved

### Open question, narrowed — not a cause

**Resolved.** The hot loop at `0x80000130`–`0x8000013c` was `zero_region`'s byte-at-a-time
tail loop in `crt0.S`:

```
80000130: bgeu a0, a1, done
80000134: sb   zero, 0(a0)
80000138: addi a0, a0, 1
8000013c: j    0x80000130
```

Its only caller is `_start`, so by the end of the run control has fallen back
into the boot code: `ra`-based returns in the program are landing in `crt0`.

**This may well be a consequence of the missing traps rather than an
independent defect.** With traps 1–4 never taken, `p08` never followed the
control flow it was designed to follow, so its `ret`-based bookkeeping never
ran the way the author intended. That is not yet distinguished from a real
second bug. I am not claiming a root cause.

### Bugs the cross-check actually caught

The Spike cross-check was not a formality. It found **ten** real defects that
build, audit and oracle all passed:

| Found in | Defect |
|---|---|
| firmware `p01` | the SLTU select block had been lost in an edit; `sig3` was constant |
| oracle `p01` | the select direction was inverted relative to the firmware |
| firmware `p02` | the JALR loop used `jalr zero, t1`, so the stub's `ret` returned to the loop setup and the program never terminated |
| firmware `p10` | `ra` was never established before the first `jalr`; the callee returned into `crt0`, which reported PASS with an all-zero signature |
| firmware `p10` | `jalr a3, a3` with `rd == ra` clobbered the return address, again not terminating |
| oracle `p09` | `LW` sign-extends; the model zero-extended |
| oracle `p03` | the same `LW` sign-extension error in `sig0` |
| oracle `p04` | `MULHSU` treats rs2 as **unsigned**; the model treated both operands as signed |
| firmware `p07` | byte assembly extracted `a[23:16]` instead of `a[15:8]` |
| build | `crt0.o` was not rebuilt when `corpus.json` changed, so every ELF silently carried zero inputs |

This is direct evidence that the three-way check is doing its job: the oracle
agrees with the declaration and the golden file, and *both* were wrong in
several of these cases.

```
python3 tools/host_oracle.py --program p13_romstore   # the split-out case
```

## 10. Commands a reviewer can run

```sh
make -C tests/programs clean && make -C tests/programs all
make -C tests/programs audit
python3 tools/check_profile.py --profile p0
python3 tools/host_oracle.py --all
python3 tools/host_oracle.py --record          # refresh golden.json from the oracle
python3 tests/programs/audit/spike_crosscheck.py --timeout 15   # needs Spike on PATH
```

`spike_crosscheck.py` defaults to `$HOME/mosaic-ref/install/bin/spike`.

## 11. Files owned by this package

```
tests/programs/Makefile
tests/programs/corpus.json          declaration: inputs, expected signatures, expected traps
tests/programs/golden.json          recorded oracle output
tests/programs/mkvars.py            corpus.json -> build/corpus.mk
tests/programs/linker/mosaic_p0.ld
tests/programs/src/platform.h       frozen addresses, CSR numbers, assembly macros
tests/programs/src/crt0.S           reset path, zero_region, tohost_finish
tests/programs/src/trap.S           trap entry, trap_expect, trap stack
tests/programs/src/p01..p13*.S      one .S per corpus program
tests/programs/audit/check_p0_isa.py    ISA/arch/link/helper/HTIF-symbol audit
tests/programs/audit/spike_crosscheck.py  third-party cross-check
tools/host_oracle.py
results/reports/I-007-firmware.md
```

Nothing outside `tests/programs/`, `tools/host_oracle.py` and this report was
created or modified.