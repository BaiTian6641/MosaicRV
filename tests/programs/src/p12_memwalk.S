/* tests/programs/src/p12_memwalk.S
 *
 * A longer mixed program: build a 16-word pattern in scratch with a data
 * dependent recurrence, then walk it back with a load-only loop whose trip
 * count comes from an input.  This is the closest thing in the corpus to a
 * small workload: it exercises the memory endpoint, the loop branches and the
 * ALU at the same time, and the result is a function of all three inputs.
 *
 * Covered instructions: sd, ld, slli, xor, add, andi, bnez, blt.
 *
 * Recurrence (all arithmetic mod 2^64):
 *   w[0] = a ^ c
 *   w[i+1] = (w[i] << 1) ^ b          for i in 0..14
 *
 * The sum pass covers the first n = (c & 3) + 8 words, so the trip count --
 * and therefore how much of the pattern is accumulated -- changes with the
 * input.
 *
 * Computed signature:
 *   sig0 = sum of w[0..n-1]
 *   sig1 = w[n-1]
 *   sig2 = number of i < n with (w[i] & 1) == 1
 *   sig3 = w[0] ^ (w[15] >> 32)
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS
    mv      s3, s1

    /* ---- fill 16 words ---- */
    xor     t0, a0, a2                          /* w[0] = a ^ c */
    mv      s8, t0                              /* remember w[0] */
    mv      t1, s3                              /* cursor */
    li      t2, 16
1:
    sd      t0, 0(t1)
    slli    t0, t0, 1
    xor     t0, t0, a1                          /* w[i+1] */
    addi    t1, t1, 8
    addi    t2, t2, -1
    bnez    t2, 1b

    /* ---- w[15] is the last word the fill loop wrote ---- */
    addi    t1, t1, -8
    ld      s10, 0(t1)                          /* w[15] */

    /* ---- walk back over n words ---- */
    andi    t3, a2, 3
    addi    t3, t3, 8                           /* n = (c & 3) + 8 */
    mv      t1, s3
    li      t4, 0                              /* running sum */
    li      t5, 0                              /* odd counter */
    li      t6, 0                              /* i */
    mv      s11, t1
2:
    bgeu    t6, t3, 3f
    ld      t0, 0(t1)
    add     t4, t4, t0
    andi    t2, t0, 1
    add     t5, t5, t2
    addi    t1, t1, 8
    addi    t6, t6, 1
    j       2b
3:
    /* t1 now points at w[n], so the last word summed is at -8 */
    addi    t1, t1, -8
    ld      s11, 0(t1)                         /* w[n-1] */

    srli    t0, s10, 32
    xor     t0, t0, s8                          /* sig3 */

    SIG0    t4
    SIG1    s11
    SIG2    t5
    SIG3    t0

    FINISH_PASS
    .size   main, . - main