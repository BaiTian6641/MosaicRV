/* tests/programs/src/p04_mul.S
 *
 * RV64M low- and high-multiply, including the signed-high cases that a
 * naive implementation gets wrong.
 *
 * Covered instructions: mul, mulh, mulhsu, mulhu.
 *
 * Computed signature:
 *   sig0 = MUL(a, b)         low 64 bits of a*b
 *   sig1 = MULH(a, b)        high 64 bits, signed x signed
 *   sig2 = MULHU(a, b)       high 64 bits, unsigned x unsigned
 *   sig3 = MULHSU(a, c) ^ MUL(a, c)
 *
 * Inputs are chosen so that at least one of the three products has a
 * different result under signed and unsigned interpretation of the high
 * half, and so that MULHSU(a, c) differs from both MULH and MULHU.
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS

    mul     t0, a0, a1
    mulh    t1, a0, a1
    mulhu   t2, a0, a1
    mulhsu  t3, a0, a2
    mul     t4, a0, a2
    xor     t3, t3, t4                         /* sig3 */

    SIG0    t0
    SIG1    t1
    SIG2    t2
    SIG3    t3

    FINISH_PASS
    .size   main, . - main