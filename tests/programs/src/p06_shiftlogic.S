/* tests/programs/src/p06_shiftlogic.S
 *
 * Shifts, bitwise logic and the signed/unsigned comparison pair.
 *
 * Covered instructions: slli, srli, srai, sll, srl, sra, andi, ori, xori,
 * and, or, xor, slt, sltu.
 *
 * Computed signature (inputs a, b, c):
 *   sig0 = a <<  (c & 63)
 *   sig1 = a >>u (c & 63)
 *   sig2 = a >>s (c & 63)
 *   sig3 = ((a ^ b) | (a & b) | ~a)
 *           ^ (-(a <s b) ? ~0 : 0)
 *           ^ ((-(b <s c) ? ~0 : 0) << 16)
 *
 * sig2 differs from sig1 for a negative a, which is the only thing that
 * separates SRA from SRL.  The two comparison results are widened to all
 * ones / all zeros so that SLT and SLTU are observable as data, not just as
 * branch conditions.
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS

    andi    t0, a2, 63
    slli    s4, a0, t0                         /* sig0 */
    srl     s5, a0, t0                         /* sig1 */
    sra     s6, a0, t0                         /* sig2 */

    /* ---- bitwise mix ---- */
    xor     t1, a0, a1
    or      t1, t1, a1
    and     t2, a0, a1
    not     t2, t2
    or      t1, t1, t2

    /* ---- comparison terms ---- */
    slt     t3, a0, a1
    neg     t3, t3                             /* 0 or all ones */
    xor     t1, t1, t3

    slt     t3, a1, a2
    neg     t3, t3
    slli    t3, t3, 16
    xor     s7, t1, t3

    SIG0    s4
    SIG1    s5
    SIG2    s6
    SIG3    s7

    FINISH_PASS 6
    .size   main, . - main