/* tests/programs/exit/src/x01_normal.S -- V-012 scenario "completes normally".
 *
 * The reference behaviour of the frozen exit protocol: compute the four
 * signature words from the harness-supplied operands, publish them in the
 * frozen signature window, write exactly 1 to TOHOST (PASS bit set, code 0),
 * and park.  Nothing else.  The harness must read a complete signature, see
 * the store retire after the TOHOST pulse, and report exit status 0.
 *
 * Self-contained: it is its own reset path (no crt0.S, no trap.S), so this
 * program cannot inherit the corpus's "main returned -> PASS" shortcut and
 * cannot be confused with a corpus case.
 */

#include "exit.h"

    .section .text.init, "ax", @progbits
    .balign 4
    .globl  _start
    .type   _start, @function
_start:
    /* operands {a, b, c}: written by the harness into the program buffer
     * before reset is released */
    li      t0, MOSAIC_EXIT_INPUT
    ld      a0, 0(t0)
    ld      a1, 8(t0)
    ld      a2, 16(t0)

    /* signature = { a+b, a-b, a^b, (a<<3)^c } */
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

    /* protocol PASS: the word written to TOHOST must be exactly 1 */
    li      t0, MOSAIC_TOHOST
    li      t1, 1
    sd      t1, 0(t0)

    /* park: the frozen protocol never expects another memory operation after
     * the TOHOST store, and the harness requires that */
1:  j       1b
    .size   _start, . - _start
