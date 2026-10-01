/* tests/programs/src/p05_divrem.S
 *
 * RV64M integer division and remainder, including the two architecturally
 * defined corner cases:
 *
 *   divisor == 0      DIV  -> -1 (all ones)   DIVU -> 2^64-1
 *                     REM  -> dividend        REMU -> dividend
 *   dividend == INT64_MIN, divisor == -1
 *                     DIV  -> INT64_MIN (overflow, no trap)
 *                     REM  -> 0
 *
 * Covered instructions: div, divu, rem, remu.
 *
 * Computed signature:
 *   sig0 = DIV(a, b) ^ DIVU(a, b)
 *   sig1 = REM(a, b) ^ REMU(a, b)
 *   sig2 = DIV(a, c) ^ DIVU(a, c)
 *   sig3 = REM(a, c) ^ REMU(a, c)
 *
 * Because each word is an exclusive-OR of the signed and unsigned result,
 * an implementation that confuses the two fails, and an implementation that
 * mishandles divide-by-zero or the signed overflow case fails too.
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS

    /* DIV(a, 0) and DIVU(a, 0) are both all ones and REM(a, 0) and
     * REMU(a, 0) are both the dividend, so the exclusive-or of the signed
     * and unsigned forms is identically zero whenever the divisor is zero.
     * The marker below makes a divide-by-zero case observable instead of
     * vacuously correct. */
    div     t0, a0, a1
    divu    t1, a0, a1
    xor     s4, t0, t1                         /* sig0 */
    bnez    a1, 1f
    li      t2, MOSAIC_DIVZERO_MARK
    xor     s4, s4, t2
1:
    rem     t0, a0, a1
    remu    t1, a0, a1
    xor     s5, t0, t1                         /* sig1 */

    div     t0, a0, a2
    divu    t1, a0, a2
    xor     s6, t0, t1                         /* sig2 */
    bnez    a2, 2f
    li      t2, MOSAIC_DIVZERO_MARK
    xor     s6, s6, t2
2:
    rem     t0, a0, a2
    remu    t1, a0, a2
    xor     s7, t0, t1                         /* sig3 */

    SIG0    s4
    SIG1    s5
    SIG2    s6
    SIG3    s7

    FINISH_PASS
    .size   main, . - main

/* The three compiled-in inputs for this (program, input) pair. */
    MOSAIC_PROGRAM_INPUTS
