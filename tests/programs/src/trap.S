/* tests/programs/src/trap.S
 *
 * Machine-mode trap entry for the MosaicRV p0 bare-metal corpus.
 *
 * POLICY (this is the documented choice, not an accident):
 *
 *   A trap is RECOVERABLE only when the program has explicitly armed it.
 *   Before executing an instruction that is expected to trap, a program calls
 *   trap_expect(<pc of that instruction>, <number of traps allowed>), which
 *   writes __mexpc_expected / __mexpc_arm.  When a trap is taken and
 *
 *       mepc == __mexpc_expected   AND   __mexpc_arm != 0
 *
 *   the handler records the trap and resumes at mepc + 4 (all p0 instructions
 *   are 4 bytes and no compressed extension is in scope), decrementing
 *   __mexpc_arm.  Every other trap is FATAL: the handler records it, restores
 *   the hart, and ends the run with tohost bit 0 clear and code = mcause, so
 *   the harness reports a FAIL with the raw cause.
 *
 *   This is the only policy the host oracle has to model, which is why an
 *   unexpected trap can never be silently absorbed by the firmware and hide a
 *   DUT defect.
 *
 * Recorded per trap (MOSAIC_TRAPLOG_REC_WORDS == 2, at MOSAIC_TRAPLOG_BASE):
 *
 *   word 0 : mcause   - architecturally defined, compared by the oracle
 *   word 1 : mtval    - recorded for debug dumps only.  The Privileged spec
 *                       does not require mtval to hold the faulting address
 *                       for access faults, so it is deliberately NOT part of
 *                       the expected signature.
 *
 * mepc, mcause, mtval and the interrupted mscratch are all read out of the
 * CSRs on entry; the interrupted mscratch is preserved because mscratch is
 * reused as the trap-stack pointer.
 *
 * CONTEXT SAVING: the handler saves and restores ra, a0-a7 and t0-t6 on the
 * trap stack (128 bytes).  It never touches s0-s11, sp-based frame state, or
 * any memory a program is using, so resuming at mepc+4 is architecturally
 * transparent to the interrupted program.  Interrupts are never enabled
 * (mstatus.MIE is never set anywhere in this package), so no nesting is
 * possible; the handler still does the architecturally correct mstatus
 * MPIE/MPP manipulation before mret.
 */

#include "platform.h"

/* The trap stack is declared here, explicitly, with an explicit size and
 * alignment, exactly like the boot stack in crt0.S. */
    .equ MOSAIC_TRAPSTACK_SIZE,  1024
    .equ MOSAIC_TRAPSTACK_ALIGN, 16

#define TRAP_FRAME_BYTES 128

/* ================================================================== */
/* .trapstack                                                         */
/* ================================================================== */
    .section .trapstack, "aw", @nobits
    .balign MOSAIC_TRAPSTACK_ALIGN
    .globl  mosaic_trapstack_bottom
mosaic_trapstack_bottom:
    .space  MOSAIC_TRAPSTACK_SIZE, 0
    .balign MOSAIC_TRAPSTACK_ALIGN
    .globl  mosaic_trapstack_top
mosaic_trapstack_top:
    .size   mosaic_trapstack_top, 0

/* ================================================================== */
/* .bss: trap bookkeeping, zeroed by crt0 before main() runs.         */
/* ================================================================== */
    .section .bss, "aw", @nobits
    .balign 8
    .globl  mosaic_traplog_count
mosaic_traplog_count:
    .space  8, 0                  /* records written so far */
    .globl  mosaic_mexpc_expected
mosaic_mexpc_expected:
    .space  8, 0                  /* mepc that must match for recovery, else 0 */
    .globl  mosaic_mexpc_arm
mosaic_mexpc_arm:
    .space  8, 0                  /* remaining armed traps */

    .globl  mosaic_traplog_overflow
mosaic_traplog_overflow:
    .space  8, 0                  /* 1 if a trap arrived with the log full */

/* ================================================================== */
/* trap_expect(a0 = pc, a1 = count)                                    */
/*                                                                     */
/* Arm the trap handler for \count traps at exactly \pc.  Programs must   */
/* place the label immediately before the faulting instruction and pass */
/* its address, because mepc on a synchronous exception is the address  */
/* of the faulting instruction.                                        */
/* ================================================================== */
    .section .text.boot, "ax", @progbits
    .balign 4
    .globl  trap_expect
    .type   trap_expect, @function
trap_expect:
    la      t0, mosaic_mexpc_expected
    sd      a0, 0(t0)
    la      t0, mosaic_mexpc_arm
    sd      a1, 0(t0)
    ret
    .size   trap_expect, . - trap_expect

/* ================================================================== */
/* trap_entry                                                          */
/* ================================================================== */
    .balign 4
    .globl  trap_entry
    .type   trap_entry, @function
trap_entry:
    /* Switch to the trap stack.  mscratch holds the interrupted sp, which
     * is what "save the interrupted mscratch" means here. */
    csrrw   sp, mscratch, sp

    addi    sp, sp, -TRAP_FRAME_BYTES
    sd      ra,   0(sp)
    sd      a0,   8(sp)
    sd      a1,  16(sp)
    sd      a2,  24(sp)
    sd      a3,  32(sp)
    sd      a4,  40(sp)
    sd      a5,  48(sp)
    sd      a6,  56(sp)
    sd      a7,  64(sp)
    sd      t0,  72(sp)
    sd      t1,  80(sp)
    sd      t2,  88(sp)
    sd      t3,  96(sp)
    sd      t4, 104(sp)
    sd      t5, 112(sp)
    sd      t6, 120(sp)

    csrr    t0, mepc                /* t0 = faulting pc            */
    csrr    t1, mcause              /* t1 = cause                  */
    csrr    t2, mtval               /* t2 = faulting value/address */

    /* ---- append { mcause, mtval } to the trap log ---- */
    la      t3, mosaic_traplog_count
    ld      t4, 0(t3)
    li      t5, MOSAIC_TRAPLOG_RECORDS
    bgeu    t4, t5, trap_log_full

    li      t5, MOSAIC_TRAPLOG_BASE
    slli    t6, t4, 4               /* record stride = 16 bytes */
    add     t5, t5, t6
    sd      t1, 0(t5)
    sd      t2, 8(t5)
    addi    t4, t4, 1
    sd      t4, 0(t3)
    j       trap_check_armed

trap_log_full:
    la      t3, mosaic_traplog_overflow
    li      t4, 1
    sd      t4, 0(t3)
    j       trap_fatal              /* an unrecordable trap is never recoverable */


trap_check_armed:
    la      t3, mosaic_mexpc_expected
    ld      t4, 0(t3)
    bne     t0, t4, trap_fatal      /* wrong pc: fatal */

    la      t3, mosaic_mexpc_arm
    ld      t4, 0(t3)
    beqz    t4, trap_fatal         /* not armed: fatal */

    addi    t4, t4, -1              /* consume one armed trap */
    sd      t4, 0(t3)

    /* ---- architecturally correct return state ----
     * All p0 instructions are 4 bytes wide; resume after the faulting one. */
    addi    t0, t0, 4
    csrw    mepc, t0
    csrr    t0, mstatus
    or      t0, t0, MSTATUS_MPIE            /* MPIE = 1: enable on mret */
    li      t1, MSTATUS_MPP_M
    or      t0, t0, t1                     /* MPP = M; 0x1800 does not fit
                                             a 12-bit signed immediate */
    andi    t0, t0, -9                     /* clear MIE (bit 3)        */
    csrw    mstatus, t0

    csrrw   sp, mscratch, sp        /* sp = interrupted sp; mscratch = trap sp */
    ld      ra,   0(sp)
    ld      a0,   8(sp)
    ld      a1,  16(sp)
    ld      a2,  24(sp)
    ld      a3,  32(sp)
    ld      a4,  40(sp)
    ld      a5,  48(sp)
    ld      a6,  56(sp)
    ld      a7,  64(sp)
    ld      t0,  72(sp)
    ld      t1,  80(sp)
    ld      t2,  88(sp)
    ld      t3,  96(sp)
    ld      t4, 104(sp)
    ld      t5, 112(sp)
    ld      t6, 120(sp)
    addi    sp, sp, TRAP_FRAME_BYTES
    mret

/* Fatal trap: end the run immediately, without restoring the interrupted
 * registers.  Restoring first would overwrite mcause with whatever the
 * saved frame happened to hold, so the code is written to TOHOST here.
 * TOHOST bit 0 clear == FAIL; bits [63:1] carry the raw mcause, which is
 * what the harness reports. */
trap_fatal:
    csrr    t1, mcause
    slli    t0, t1, 1
    li      t2, MOSAIC_TOHOST
    sd      t0, 0(t2)
2:
    j       2b
    .size   trap_entry, . - trap_entry