/* tests/programs/termination/src/x06_signature_overrun.S -- V-012 scenario
 * "the signature write runs past the signature region".
 *
 * Writes the four frozen signature words correctly, then writes one more word
 * at MOSAIC_EXIT_SIGNATURE_GUARD_WORD = 0x80000420, the word immediately after
 * the frozen window.  It is ordinary mapped RAM, so nothing in the ISA stops
 * the store: the defect is a protocol one -- the frozen signature is four
 * words, and a program that extends it past that width is writing outside the
 * region the harness will read.
 *
 * The harness reserves the rest of the signature page as the guard band and
 * refuses the run, so the extra word cannot be silently ignored and cannot
 * turn into a pass.  Without that rule the fifth word would be invisible to
 * the comparison and the program would look perfect.
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
    xor     t2, a0, a1
    slli    t3, a0, 3
    xor     t3, t3, a2

    li      s0, MOSAIC_SIGNATURE_BASE
    sd      t0, 0(s0)
    sd      t1, 8(s0)
    sd      t2, 16(s0)
    sd      t3, 24(s0)

    /* the overrun: a fifth "signature" word, past the frozen four */
    li      t4, MOSAIC_EXIT_SIGNATURE_GUARD_WORD
    li      t5, 0x5EED
    sd      t5, 0(t4)

    li      t0, MOSAIC_TOHOST
    li      t1, 1
    sd      t1, 0(t0)

1:  j       1b
    .size   _start, . - _start
