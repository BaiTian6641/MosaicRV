/* tests/programs/src/p01_addsub.S
 *
 * Integer add / sub / shift / logic with sign-extension and overflow edges.
 *
 * Covered instructions: add, sub, andi, sra, sltu, slli, srli, xori, or, xor.
 *
 * Computed signature (inputs a, b, c from the input area):
 *   sig0 = a + b                         (mod 2^64; overflow wraps, ADDI/ADD)
 *   sig1 = a - b                         (mod 2^64)
 *   sig2 = (a + b) >>arith (c & 63)      (SRA, sign-propagating)
 *   sig3 = sltu(a, b) ? (a + b) : (a - b)
 *
 * Input sensitivity: sig0/sig1/sig2 depend on all three inputs; sig3
 * switches its source operand on the unsigned comparison, so the two inputs
 * with equal sums but different orderings take different paths.
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS

    add     t0, a0, a1                 /* sig0 */
    sub     t1, a0, a1                 /* sig1 */

    andi    t2, a2, 63
    sra     t3, t0, t2                 /* sig2: arithmetic shift of the sum */

    sltu    t4, a0, a1
    sub     t5, a0, a1
    /* sig3 also folds a zero-extend + NOT path so that the byte-extension
     * instructions contribute an observable, input-dependent result instead
     * of being dead code. */
    slli    t2, a1, 56
    srli    t2, t2, 56                 /* zero-extend b[7:0] */
    xori    t2, t2, -1                 /* NOT                  */
    andi    t2, t2, 0xFF               /* NOT of b[7:0]        */
    xor     t5, t5, t2

    SIG0   t0
    SIG1   t1
    SIG2   t3
    SIG3   t5

    FINISH_PASS
    .size   main, . - main