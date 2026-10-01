/* tests/programs/src/crt0.S
 *
 * Reset entry for the MosaicRV p0 bare-metal corpus.
 *
 * The hart leaves reset with PC = MOSAIC_RESET_VECTOR = 0x80000000, which is
 * the first byte of RAM.  The linker script places .text.init at exactly that
 * address and ENTRY(_start), so the instruction executed out of reset is the
 * first instruction of _start below.
 *
 * Boot sequence:
 *   1. establish the stack (declared right here, not inherited)
 *   2. point mscratch at the dedicated trap stack
 *   3. install the machine trap vector (mtvec, direct mode)
 *   4. zero every NOLOAD region, one region at a time
 *   5. let the harness override input word 0 through FROMHOST
 *   6. read TOHOST once (spec: reads 0 while the program runs)
 *   7. call main(); if it returns, finish PASS
 */

#include "platform.h"

/* The stacks are declared here, explicitly, with an explicit size and
 * alignment.  The linker script places the containing output section
 * wherever it fits in RAM; nothing about them depends on a symbol this
 * package did not define itself. */
    .equ MOSAIC_STACK_SIZE,  16384
    .equ MOSAIC_STACK_ALIGN, 16

/* Per-input compiled-in defaults, overridden by the Makefile with
 * -DPROG_INPUT0/-DPROG_INPUT1/-DPROG_INPUT2 so that each (program, input)
 * pair gets its own ELF.  They live in .rodata, which is ordinary RAM as far
 * as the hart is concerned, so the FROMHOST override can be written straight
 * over word 0 without a copy. */
#ifndef PROG_INPUT0
#define PROG_INPUT0 0
#endif
#ifndef PROG_INPUT1
#define PROG_INPUT1 0
#endif
#ifndef PROG_INPUT2
#define PROG_INPUT2 0
#endif

/* ================================================================== */
/* .bss: firmware-private scalar state.  Zeroed by _start.             */
/* ================================================================== */
    .section .bss, "aw", @nobits
    .balign 8
    .globl  mosaic_tohost_probe
mosaic_tohost_probe:
    .space  8, 0                  /* value read back from TOHOST at boot */

/* ================================================================== */
/* .stack: the boot/program stack.                                    */
/* ================================================================== */
    .section .stack, "aw", @nobits
    .balign MOSAIC_STACK_ALIGN
    .globl  mosaic_stack_bottom
mosaic_stack_bottom:
    .space  MOSAIC_STACK_SIZE, 0
    .balign MOSAIC_STACK_ALIGN
    .globl  mosaic_stack_top
mosaic_stack_top:
    .size   mosaic_stack_top, 0

/* ================================================================== */
/* .text.init: the reset path.                                         */
/* ================================================================== */
    .section .text.init, "ax", @progbits
    .balign 4
    .globl  _start
    .type   _start, @function
_start:
    /* 1. stack */
    la      sp, mosaic_stack_top

    /* 2. trap stack (also defined explicitly in trap.S) */
    la      t0, mosaic_trapstack_top
    csrw    mscratch, t0

    /* 3. mtvec = trap_entry, direct mode (BASE[1:0] forced to 0) */
    la      t0, trap_entry
    csrw    mtvec, t0

    /* 4. zero every NOLOAD region, one region at a time */
    la      a0, __bss_start
    la      a1, __bss_end
    call    zero_region

    la      a0, __signature_start
    la      a1, __signature_end
    call    zero_region

    la      a0, __traplog_start
    la      a1, __traplog_end
    call    zero_region

    la      a0, __scratch_start
    la      a1, __scratch_end
    call    zero_region

    la      a0, __bss_stack_start
    la      a1, __bss_stack_end
    call    zero_region

    /* 5. FROMHOST override of input word 0.
     *    Per protocol a harness that supplies no input leaves FROMHOST at 0,
     *    which must be treated as "no override" rather than as an input. */
    li      t3, MOSAIC_FROMHOST
    ld      t4, 0(t3)
    beqz    t4, 7f
    la      t5, mosaic_prog_inputs
    sd      t4, 0(t5)
7:

    /* 6. TOHOST read probe.  Protocol: reads 0 while the program runs.
     *    FROMHOST and TOHOST are adjacent registers of the same device. */
    ld      t4, -8(t3)
    la      t5, mosaic_tohost_probe
    sd      t4, 0(t5)

    /* 7. run the program */
    call    main

    /* main returned without writing TOHOST itself: report PASS, code 0.
     * Programs that want to report failure call tohost_finish directly. */
    li      a0, 0
    li      a1, 1
    call    tohost_finish
    .size   _start, . - _start

/* ================================================================== */
/* Compiled-in input defaults (read-only by convention, lives in the   */
/* loadable image so the harness loader has nothing extra to map)       */
/* ================================================================== */
    .section .rodata, "a", @progbits
    .balign 8
    .globl  mosaic_prog_inputs
mosaic_prog_inputs:
    .dword  PROG_INPUT0
    .dword  PROG_INPUT1
    .dword  PROG_INPUT2
    .size   mosaic_prog_inputs, . - mosaic_prog_inputs

/* ================================================================== */
/* Shared runtime helpers                                              */
/* ================================================================== */
    .section .text, "ax", @progbits
    .balign 4

/* tohost_finish(a0 = code, a1 = pass_flag)
 *
 * Frozen protocol: writing a non-zero value to TOHOST ends the program.
 * Bit 0 of the written word is PASS; bits [63:1] are the program-defined
 * code the harness reports.  The park loop below is unreachable on a
 * conforming harness (the write ends the run) but guarantees the hart never
 * runs off into unprogrammed memory if it does not. */
    .globl  tohost_finish
    .type   tohost_finish, @function
tohost_finish:
    slli    t0, a0, 1
    or      t0, t0, a1
    li      t1, MOSAIC_TOHOST
    sd      t0, 0(t1)
1:
    j       1b
    .size   tohost_finish, . - tohost_finish

/* zero_region(a0 = start, a1 = end)
 *
 * Zero [start, end) with 8-byte stores once the cursor is 8-byte aligned,
 * then finish with a byte-at-a-time tail.  Used once per NOLOAD region so a
 * region never depends on the linker having produced a length that is a
 * multiple of 8. */
    .globl  zero_region
    .type   zero_region, @function
zero_region:
    beq     a0, a1, 9f              /* empty region: nothing to do */

    /* byte-align the cursor first */
1:
    andi    t0, a0, 7
    beqz    t0, 3f
    sb      zero, 0(a0)
    addi    a0, a0, 1
    bne     a0, a1, 1b
    j       9f

    /* 8-byte body */
3:
    addi    t2, a1, -8
    bltu    a0, t2, 5f
    sd      zero, 0(a0)
    addi    a0, a0, 8
    j       3b

    /* tail: fewer than 8 bytes left */
5:
    bgeu    a0, a1, 9f
    sb      zero, 0(a0)
    addi    a0, a0, 1
    j       5b

9:
    ret
    .size   zero_region, . - zero_region