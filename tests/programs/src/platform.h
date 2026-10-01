/* tests/programs/src/platform.h
 *
 * MosaicRV p0 platform surface for the bare-metal test corpus.
 *
 * Every constant in this file is transcribed from frozen configuration, not
 * invented here:
 *
 *   - Region addresses/sizes : config/memory/p0.json          (profile p0)
 *   - Test protocol          : config/profiles/p0.json -> "test_protocol"
 *   - Misalignment policy    : config/profiles/p0.json -> "misalignment"
 *     (instruction_fetch = trap, load = trap, store = trap, atomic = trap)
 *
 * The linker script tests/programs/linker/mosaic_p0.ld places the signature
 * area at exactly MOSAIC_SIGNATURE_BASE, so these two cannot drift.
 */

#ifndef MOSAIC_PLATFORM_H
#define MOSAIC_PLATFORM_H

/* ------------------------------------------------------------------ */
/* Frozen memory map (config/memory/p0.json, profile p0)             */
/* ------------------------------------------------------------------ */
#define MOSAIC_BOOT_ROM_BASE      0x00000000UL   /* 4 KiB, r+x, NOT writable */
#define MOSAIC_BOOT_ROM_SIZE      0x00001000UL
#define MOSAIC_UART_BASE          0x00100000UL   /* 256 B  */
#define MOSAIC_UART_SIZE          0x00000100UL
#define MOSAIC_TEST_HARNESS_BASE  0x00102000UL   /* 16 B   */
#define MOSAIC_TEST_HARNESS_SIZE  0x00000010UL
#define MOSAIC_CLINT_BASE         0x02000000UL   /* 4 KiB  */
#define MOSAIC_CLINT_SIZE         0x00001000UL
#define MOSAIC_RAM_BASE           0x80000000UL   /* 2 MiB  */
#define MOSAIC_RAM_SIZE           0x00200000UL

/* config/profiles/p0.json -> reset.reset_vector */
#define MOSAIC_RESET_VECTOR       0x80000000UL

/* ------------------------------------------------------------------ */
/* Frozen test protocol (config/profiles/p0.json -> test_protocol)    */
/* ------------------------------------------------------------------ */
#define MOSAIC_TOHOST             0x00102000UL   /* u64, RW, write != 0 ends */
#define MOSAIC_FROMHOST           0x00102008UL   /* u64, R,  input word      */
#define MOSAIC_SIGNATURE_BASE     0x80000400UL   /* 4 x u64 in RAM           */
#define MOSAIC_SIGNATURE_WORDS    4
#define MOSAIC_PASS_CODE          1              /* bit 0 of tohost          */

/* Folded into a signature word when a divisor is zero, so that a
 * divide-by-zero case is observable.  DIV(a,0) and DIVU(a,0) are both all
 * ones and REM(a,0) and REMU(a,0) are both the dividend, so comparing the
 * signed and unsigned forms alone cannot distinguish a correct
 * implementation from one that returns anything else for a zero divisor. */
#define MOSAIC_DIVZERO_MARK       0x5555555555555555UL

/* ------------------------------------------------------------------ */
/* Firmware-owned RAM areas (see linker/mosaic_p0.ld)                 */
/*                                                                     */
/* These are firmware conventions, not platform guarantees.  They are  */
/* all NOLOAD output sections so they are provably present in the ELF  */
/* program headers while consuming no file bytes.                     */
/* ------------------------------------------------------------------ */
#define MOSAIC_TRAPLOG_BASE       0x80001000UL
#define MOSAIC_TRAPLOG_RECORDS    8
#define MOSAIC_TRAPLOG_REC_WORDS  2              /* { mcause, mtval }        */
#define MOSAIC_TRAPLOG_REC_BYTES  (MOSAIC_TRAPLOG_REC_WORDS * 8)

/* The three program inputs are compiled into .rodata as mosaic_prog_inputs,
 * which the linker places immediately after .text inside the loadable image.
 * A harness-supplied FROMHOST word overwrites word 0 in place. */
#define MOSAIC_INPUT_WORDS        3              /* a, b, c                  */

#define MOSAIC_SCRATCH_BASE       0x80001080UL
#define MOSAIC_SCRATCH_SIZE       128

/* Machine-mode CSR numbers (RISC-V Privileged Spec v1.12) */
#define MSTATUS   0x300
#define MISA      0x301
#define MIE       0x304
#define MTVEC     0x305
#define MSCRATCH  0x340
#define MEPC      0x341
#define MCAUSE    0x342
#define MTVAL     0x343
#define MIP       0x344

/* mstatus field masks used by the trap handler */
#define MSTATUS_MIE      0x0000000000000008UL
#define MSTATUS_MPIE     0x0000000000000080UL
#define MSTATUS_MPP_M    0x0000000000001800UL

/* mcause exception codes (exceptions only; bit 63 is clear) */
#define EXC_INSN_MISALIGNED   0
#define EXC_INSN_ACCESS       1
#define EXC_ILLEGAL_INSN      2
#define EXC_BREAKPOINT        3
#define EXC_LOAD_MISALIGNED   4
#define EXC_LOAD_ACCESS       5
#define EXC_STORE_MISALIGNED  6
#define EXC_STORE_ACCESS      7
#define EXC_ECALL_U           8
#define EXC_ECALL_M          11

/* UART register (config/memory/p0.json -> device "uart") */
#define MOSAIC_UART_THR        (MOSAIC_UART_BASE + 0)

/* CLINT register (config/memory/p0.json -> device "clint") */
#define MOSAIC_CLINT_MTIME     (MOSAIC_CLINT_BASE + 0xBFF8)

/* ------------------------------------------------------------------ */
/* C accessors                                                        */
/*                                                                     */
/* The p0 corpus is 100% assembly.  These accessors exist so the same
 * frozen map is available to any future C firmware without a second,
 * hand-maintained copy of the addresses.  They are compiled out of every
 * assembly translation unit. */
/* ------------------------------------------------------------------ */
#ifndef __ASSEMBLER__

/* tohost: writing a non-zero value ends the program.  Bit 0 == PASS. */
static inline volatile unsigned long *mosaic_tohost(void)
{
    return (volatile unsigned long *)(unsigned long)MOSAIC_TOHOST;
}

/* fromhost: read an input word supplied by the harness. */
static inline volatile unsigned long *mosaic_fromhost(void)
{
    return (volatile unsigned long *)(unsigned long)MOSAIC_FROMHOST;
}

static inline unsigned long mosaic_tohost_read(void)
{
    return *(volatile unsigned long *)(unsigned long)MOSAIC_TOHOST;
}

static inline void mosaic_tohost_write(unsigned long value)
{
    *(volatile unsigned long *)(unsigned long)MOSAIC_TOHOST = value;
}

static inline unsigned long mosaic_fromhost_read(void)
{
    return *(volatile unsigned long *)(unsigned long)MOSAIC_FROMHOST;
}

/* signature[i], i in [0, MOSAIC_SIGNATURE_WORDS) */
static inline volatile unsigned long *mosaic_signature(unsigned int index)
{
    return (volatile unsigned long *)
        ((unsigned long)MOSAIC_SIGNATURE_BASE + 8UL * index);
}

/* trap log record i: { mcause, mtval } */
static inline volatile unsigned long *mosaic_traplog(unsigned int index)
{
    return (volatile unsigned long *)
        ((unsigned long)MOSAIC_TRAPLOG_BASE
         + (unsigned long)MOSAIC_TRAPLOG_REC_BYTES * index);
}

/* program input words; word 0 is overwritten in place when the harness
 * supplies a non-zero FROMHOST.  The base address is a link-time symbol,
 * so this accessor is only usable from C linked against mosaic_p0.ld. */
extern unsigned long mosaic_prog_inputs[];

static inline volatile unsigned long *mosaic_input(unsigned int index)
{
    return &mosaic_prog_inputs[index];
}

static inline volatile unsigned char *mosaic_scratch(void)
{
    return (volatile unsigned char *)(unsigned long)MOSAIC_SCRATCH_BASE;
}

/* UART byte write. */
static inline void mosaic_uart_putc(unsigned char value)
{
    *(volatile unsigned char *)(unsigned long)MOSAIC_UART_THR = value;
}

/* M-mode CSR accessors. */
static inline unsigned long mosaic_read_mepc(void)
{
    unsigned long value;
    __asm__ volatile ("csrr %0, mepc" : "=r"(value));
    return value;
}

static inline void mosaic_write_mepc(unsigned long value)
{
    __asm__ volatile ("csrw mepc, %0" : : "r"(value));
}

#endif /* !__ASSEMBLER__ */

/* ------------------------------------------------------------------ */
/* Assembly surface                                                   */
/*                                                                     */
/* Register convention used by every corpus program:                  */
/*   s0 = signature area base (0x80000400)                            */
/*   s1 = scratch buffer base   (0x80000500)                          */
/*   s2 = program input base    (0x800004c0)                          */
/*   a0..a2 = inputs a, b, c (loaded from the input area by crt0)     */
/*   s3..s11 are free for program use; the trap handler never touches */
/*   them, and it fully saves/restores ra, a0-a7 and t0-t6.           */
/* ------------------------------------------------------------------ */
#ifdef __ASSEMBLER__

/* Load the three program inputs into a0/a1/a2 from the input area.
 *
 * Address materialisation uses `la` on linker-script symbols rather than
 * `li` on the numeric constants above: with -mcmodel=medany GAS cannot turn
 * a bare absolute constant into a PC-relative sequence, and using the
 * numeric value here would silently change the addressing mode.  The script
 * places these symbols at exactly MOSAIC_*_BASE. */
.macro MOSAIC_LOAD_INPUTS
    la      s2, mosaic_prog_inputs
    ld      a0, 0(s2)
    ld      a1, 8(s2)
    ld      a2, 16(s2)
.endm

/* Establish the three reserved base pointers. */
.macro MOSAIC_SETUP_BASES
    la      s0, __signature_start
    la      s1, __scratch_start
    la      s2, mosaic_prog_inputs
.endm

/* Compiled-in program inputs.  Each corpus program emits exactly one table,
 * so the table is part of that program's object file and is recompiled
 * whenever corpus.json changes.  Putting it in shared boot code instead would
 * make the boot object a function of every program's inputs at once. */
#ifndef PROG_INPUT0
#define PROG_INPUT0 0
#endif
#ifndef PROG_INPUT1
#define PROG_INPUT1 0
#endif
#ifndef PROG_INPUT2
#define PROG_INPUT2 0
#endif

.macro MOSAIC_PROGRAM_INPUTS
    .section .rodata, "a", @progbits
    .balign 8
    .globl  mosaic_prog_inputs
mosaic_prog_inputs:
    .dword  PROG_INPUT0
    .dword  PROG_INPUT1
    .dword  PROG_INPUT2
    .size   mosaic_prog_inputs, . - mosaic_prog_inputs
.endm

/* sig[0..3] = signature word 0..3 */
.macro SIG0 reg
    sd      \reg,  0(s0)
.endm
.macro SIG1 reg
    sd      \reg,  8(s0)
.endm
.macro SIG2 reg
    sd      \reg, 16(s0)
.endm
.macro SIG3 reg
    sd      \reg, 24(s0)
.endm

/* scratch byte offset */
.macro SCRATCH reg, off
    addi    \reg, s1, \off
.endm

/* End the run with PASS.  Does not return.
 *
 * The frozen rule is "tohost == 1 means PASS; any other non-zero value means
 * FAIL and bits [63:1] carry a program-defined code".  A pass therefore has
 * to write exactly MOSAIC_PASS_CODE, with no diagnostic code folded in: any
 * other value, including (n << 1) | 1 for n != 0, is a FAIL by that rule.
 * A program that wants to report a diagnostic on the pass path must put it
 * somewhere other than TOHOST.
 *
 * This encoding is also what Spike's HTIF expects: HTIF reads the exit code
 * as tohost >> 1, so tohost == 1 is exit code 0 (pass) and tohost == 2*code
 * is exit code `code` (fail), with no ambiguity in between. */
.macro FINISH_PASS
    li      a0, 0
    li      a1, 1
    call    tohost_finish
.endm

/* End the run with FAIL, code = \code in bits [63:1], PASS bit clear.
 * Does not return. */
.macro FINISH_FAIL code
    li      a0, \code
    li      a1, 0
    call    tohost_finish
.endm

#endif /* __ASSEMBLER__ */

#endif /* MOSAIC_PLATFORM_H */