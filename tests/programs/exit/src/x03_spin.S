/* tests/programs/exit/src/x03_spin.S -- V-012 scenario "never exits".
 *
 * Writes a complete signature and then spins forever without ever writing
 * TOHOST.  The protocol has not completed, so the run must be stopped by the
 * cycle budget and reported as the timeout class.  A timeout is not a pass:
 * the case asserts that this scenario comes back with the timeout status and
 * not with exit status 0.
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

    /* no TOHOST write, ever */
1:  j       1b
    .size   _start, . - _start
