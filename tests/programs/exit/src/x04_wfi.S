/* tests/programs/exit/src/x04_wfi.S -- V-012 scenario "executes WFI and never wakes".
 *
 * p0 is M-mode only and has no interrupt controller and no wake source: a WFI
 * that waits can never be woken.  The scenario is therefore a stall, not a
 * program that finished and not a program that failed: the harness recognises
 * the WFI instruction in the DUT's event stream and reports the WFI-stall
 * class, which is a different documented code from the plain cycle-budget
 * timeout.
 *
 * The trap vector is pointed at the same park loop first.  This kernel build
 * has no WFI sleep state, so the WFI raises illegal instruction (cause 2) and
 * the hart vectors to mtvec; with mtvec aimed at a self-loop the run parks
 * instead of taking the corpus trap handler's fatal path (which would write
 * TOHOST and turn a stall into a program FAIL).  Setting mtvec is what makes
 * the scenario a stall rather than a firmware-reported failure.  MT-vec's
 * BASE field is 4-byte aligned, which is what direct mode requires.
 */

#include "exit.h"

    .section .text.init, "ax", @progbits
    .balign 4
    .globl  _start
    .type   _start, @function
_start:
    li      t0, MOSAIC_EXIT_INPUT
    ld      a0, 0(t0)
    ld      a1, 8(t0)
    ld      a2, 16(t0)

    add     t0, a0, a1
    sub     t1, a0, a1
    xor     t2, a0, a2
    slli    t3, a0, 3
    xor     t3, t3, a2

    li      s0, MOSAIC_SIGNATURE_BASE
    sd      t0, 0(s0)
    sd      t1, 8(s0)
    sd      t2, 16(s0)
    sd      t3, 24(s0)

    /* direct-mode mtvec = the park loop below */
    la      t0, 1f
    csrw    mtvec, t0

    /* wait for an interrupt that p0 can never deliver */
    wfi

1:  j       1b
    .size   _start, . - _start
