/* tests/programs/src/p11_bigmuldiv.S
 *
 * A DEPENDENT division chain: the result of one RV64M operation is the
 * dividend of the next, with no independent work in between.  This catches a
 * memory/issue path that serialises on the destination register incorrectly,
 * and it drives the divide-by-zero and signed-overflow corner cases a second
 * time in a data-dependent context.
 *
 * Covered instructions: div, divu, rem, remu, add, xor.
 *
 * With inputs a, b, c:
 *   t0 = DIV(a, b)           t1 = DIVU(a, b)
 *   t2 = REM(a, b)           t3 = REMU(a, b)
 *   t5 = REMU(a, b) + c      <- dividend depends on the previous REMU
 *   t6 = DIV(t5, b)          <- and on the previous result again
 *   t7 = REM(t6, b)   [held in t4]
 *
 * Computed signature:
 *   sig0 = DIV(a, b) ^ DIVU(a, b)
 *   sig1 = REM(a, b) ^ REMU(a, b)
 *   sig2 = DIV(t5, b)
 *   sig3 = REM(t6, b) ^ t5
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS

    div     t0, a0, a1
    divu    t1, a0, a1
    xor     s4, t0, t1                         /* sig0 */

    rem     t0, a0, a1
    remu    t1, a0, a1
    xor     s5, t0, t1                         /* sig1 */

    add     t5, t1, a2                         /* REMU result feeds the next */
    div     t6, t5, a1
    rem     t4, t6, a1

    xor     s7, t4, t5

    SIG0    s4
    SIG1    s5
    SIG2    t6
    SIG3    s7

    FINISH_PASS
    .size   main, . - main