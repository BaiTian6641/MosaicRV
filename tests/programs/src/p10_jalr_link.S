/* tests/programs/src/p10_jalr_link.S
 *
 * JAL and JALR link-register semantics, including the JALR bit-0 clear rule.
 *
 *   - JAL writes the address of the following instruction into rd.
 *   - JALR writes the address of the following instruction into rd, and
 *     clears bit 0 of the computed target.  Here the target is deliberately
 *     made odd (ori 1) before the jump, so an implementation that does not
 *     clear bit 0 lands on an address that is not an instruction boundary.
 *   - JALR reads rs1 before writing rd, so rd == rs1 is legal and must
 *     yield rd = link address.
 *
 * The link values are checked against "address of the instruction after the
 * jump", which is computed with la.  Both checks must produce exactly 0, and
 * a layout change cannot make them pass vacuously.
 *
 * Covered instructions: jal, jalr, ret, la (auipc+addi), add, sub, xor.
 *
 * Call sequence with inputs a, b:
 *   a0 = a
 *   jal  p10_jal_b     -> a0 = a + 1
 *   jalr p10_jalr_b    -> a0 = a + 1 + b
 *   jalr a3, a3        -> a0 = a + 1 + 2b        (rd == rs1)
 *   jal  p10_outer     -> a1 = b - 1, a2 = a0 ^ a1
 *
 * Computed signature:
 *   sig0 = JAL link delta        (0, link == next instruction)
 *   sig1 = JALR link delta       (0, link == next instruction)
 *   sig2 = a0 after the chain    = a + 1 + 2b
 *   sig3 = a1 ^ a2               = (b - 1) ^ (2 * (a + 3b))
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS

    /* ra is owned by crt0's `call main`; every routine below returns through
     * it, so save it before the first call and restore it on the way out. */
    addi    sp, sp, -16
    sd      ra, 0(sp)

    /* ---- JAL link ---- */
    jal     ra, p10_jal_b
p10_after_jal:
    la      t1, p10_after_jal
    sub     s4, ra, t1                         /* must be 0 */

    /* ---- JALR with bit 0 deliberately set ---- */
    la      t2, p10_jalr_b
    ori     t2, t2, 1                          /* JALR must clear bit 0 */
    jalr    ra, t2
p10_after_jalr:
    la      t4, p10_after_jalr
    sub     s5, ra, t4                         /* must be 0 */

    /* ---- JALR with rd == rs1 ----
     * ra must point at the continuation before this one, because `jalr` with
     * rd == ra would clobber the return address the callee needs. */
    la      a4, 2f
    mv      a3, t2
    mv      ra, a4
    jalr    a3, a3
2:

    /* ---- nested call / return through ra ---- */
    jal     ra, p10_outer

    xor     t0, a1, a2                         /* sig3 */

    ld      ra, 0(sp)
    addi    sp, sp, 16

    SIG0    s4
    SIG1    s5
    SIG2    a0
    SIG3    t0
    .size   main, . - main

    .balign 4
p10_jal_b:
    addi    a0, a0, 1
    ret
    .size   p10_jal_b, . - p10_jal_b

    .balign 4
p10_jalr_b:
    add     a0, a0, a1
    ret
    .size   p10_jalr_b, . - p10_jalr_b

    .balign 4
p10_outer:
    addi    sp, sp, -16
    sd      ra, 0(sp)
    addi    a1, a1, -1
    jal     ra, p10_inner
    ld      ra, 0(sp)
    addi    sp, sp, 16
    ret
    .size   p10_outer, . - p10_outer

    .balign 4
p10_inner:
    add     a2, a0, a1
    slli    a2, a2, 1
    ret
    .size   p10_inner, . - p10_inner

/* The three compiled-in inputs for this (program, input) pair. */
    MOSAIC_PROGRAM_INPUTS
