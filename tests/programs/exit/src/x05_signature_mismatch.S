/* tests/programs/exit/src/x05_signature_mismatch.S -- V-012 scenario
 * "writes PASS and then produces a mismatching signature".
 *
 * The one instruction that differs from x01_normal.S is the `xori` below: word
 * 2 holds (a ^ b) ^ 1 instead of (a ^ b).  Everything else -- including the
 * TOHOST word, which is exactly 1 -- is protocol-correct, so the program's own
 * self-report says PASS.  The harness must not accept it: the signature is
 * compared against the four expressions evaluated independently in C++ from
 * the same operands, and one differing bit in one word is the finding.  This
 * is the scenario that proves a PASS word cannot buy a pass.
 *
 * The corrupted word is computed by the program, not patched into the image
 * after the fact, so the mismatch is a real architectural result.
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

    /* deliberate: one bit of signature word 2 is wrong */
    xori    t2, t2, 1

    li      s0, MOSAIC_SIGNATURE_BASE
    sd      t0, 0(s0)
    sd      t1, 8(s0)
    sd      t2, 16(s0)
    sd      t3, 24(s0)

    /* the program claims PASS anyway */
    li      t0, MOSAIC_TOHOST
    li      t1, 1
    sd      t1, 0(t0)

1:  j       1b
    .size   _start, . - _start
