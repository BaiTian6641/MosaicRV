/* tests/programs/src/p02_branch.S
 *
 * Every branch condition plus JAL/JALR with the JALR bit-0 clear rule.
 *
 * Covered instructions: beq, bne, blt, bge, bltu, bgeu, jal, jalr, ret, and
 * the conditional-select shapes built from them.
 *
 * Computed signature (inputs a, b, c):
 *   sig0 = branch mask: bit0 BEQ, bit1 BNE, bit2 BLT, bit3 BGE, bit4 BLTU,
 *                       bit5 BGEU
 *   sig1 = popcount(sig0)                      (loop trip count varies)
 *   sig2 = number of indirect control transfers executed:
 *          1 jal + (c & 7) jalr round-trips
 *   sig3 = let m = sig0 ^ (c & 0x3f);
 *          (m & 1) ? b : ((m & 4) ? -a : a)
 *
 * Input sensitivity: sig0/sig3 vary the six-way comparison cascade,
 * sig1 varies with the mask, and sig2 varies the JALR loop trip count, so
 * all three inputs change both the executed path and the signature.
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS

    /* ---- six-way branch cascade ---- */
    li      t0, 0                              /* mask */
    beq     a0, a1, 1f
    j       2f
1:
    ori     t0, t0, 0x01
    j       3f
2:
    ori     t0, t0, 0x02
3:
    blt     a0, a1, 4f
    j       5f
4:
    ori     t0, t0, 0x04
5:
    bge     a0, a1, 6f
    j       7f
6:
    ori     t0, t0, 0x08
7:
    bltu    a0, a1, 8f
    j       9f
8:
    ori     t0, t0, 0x10
9:
    bgeu    a0, a1, 10f
    j       11f
10:
    ori     t0, t0, 0x20
11:

    /* ---- popcount; iteration count is data-dependent ---- */
    mv      t1, t0
    li      t2, 0
1:
    andi    t3, t1, 1
    add     t2, t2, t3
    srli    t1, t1, 1
    bnez    t1, 1b
    mv      s3, t2                            /* keep the popcount; t2 is reused below */

    /* ---- sig2: one jal plus (c & 7) jalr round-trips ---- */
    li      t4, 0
    jal     ra, p02_stub                    /* 1 jal  */
    addi    t4, t4, 1

    andi    t5, a2, 7                        /* trip count from input c */
    la      t1, p02_stub
    ori     t1, t1, 1                         /* bit 0 set on purpose: JALR
                                               * must clear bit 0 and land
                                               * on the even instruction
                                               * address */
1:
    beqz    t5, 2f
    jalr    zero, t1
    addi    t4, t4, 1
    addi    t5, t5, -1
    j       1b
2:

    /* ---- sig3: input-dependent select ---- */
    andi    t1, a2, 0x3F
    xor     t1, t1, t0                       /* m = mask ^ (c & 0x3f) */
    andi    t2, t1, 0x01
    bnez    t2, 3f
    andi    t2, t1, 0x04
    bnez    t2, 4f
    mv      t3, a0
    j       5f
3:
    mv      t3, a1
    j       5f
4:
    sub     t3, x0, a0
5:

    SIG0    t0
    SIG1    s3
    SIG2    t4
    SIG3    t3

    FINISH_PASS
    .size   main, . - main

    .balign 4
p02_stub:
    ret
    .size   p02_stub, . - p02_stub

/* The three compiled-in inputs for this (program, input) pair. */
    MOSAIC_PROGRAM_INPUTS
