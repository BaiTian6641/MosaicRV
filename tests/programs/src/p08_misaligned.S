/* tests/programs/src/p08_misaligned.S
 *
 * The frozen misalignment policy is config/profiles/p0.json:
 *
 *     "misalignment": { "instruction_fetch": "trap", "load": "trap",
 *                       "store": "trap", "atomic": "trap" }
 *
 * so every misaligned access below MUST raise an address-misaligned
 * exception.  The neighbouring accesses at the same alignment class MUST NOT
 * trap, and a canary must survive the whole sequence so a DUT that performs
 * the access instead of trapping is caught even if the trap count matches.
 *
 * Scratch layout (MOSAIC_SCRATCH_BASE = 0x80000500):
 *    0  : canary low   = 0x5a5a5a5a5a5a5a5a XOR c
 *    8  : canary high  = same value
 *   16  : sd a
 *   24  : sd b
 *   32  : sd c
 *
 * Expected trap trace, in order:
 *   1  lh  at scratch+9    (odd address)      -> mcause 4  load misaligned
 *   2  lw  at scratch+10   (addr % 4 == 2)    -> mcause 4  load misaligned
 *   3  sh  at scratch+7    (odd address)      -> mcause 6  store misaligned
 *   4  sd  at scratch+12   (addr % 8 == 4)    -> mcause 6  store misaligned
 *   5  sw  at 0x00000000   (boot_rom, ro)     -> mcause 7  store access fault
 *
 * All three faulting stores land inside the canary, so "the store happened"
 * and "the store trapped" are distinguishable from the signature alone.
 *
 * Computed signature:
 *   sig0 = lbu[16] = a & 0xff                  (aligned, must not trap)
 *   sig1 = lw[20]  = a >> 32                  (aligned, must not trap)
 *   sig2 = lh[22]  = sext16(a[63:48])         (aligned, must not trap)
 *   sig3 = (fold of every logged mcause, 8 bits each, most recent last)
 *          << 2 | (canary intact ? 1 : 0) | (c round-trip ok ? 2 : 0)
 */

#include "platform.h"

    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS
    mv      s3, s1                             /* scratch; t0-t6 stay free */

    /* ---- canary = 0x5a5a5a5a5a5a5a5a XOR c ---- */
    li      t0, 0x5A5A5A5A
    slli    t0, t0, 32
    li      t1, 0x5A5A5A5A
    or      t0, t0, t1                          /* t0 = canary constant */
    xor     t2, t0, a2                          /* t2 = canary value */
    sd      t2, 0(s3)
    sd      t2, 8(s3)

    sd      a0, 16(s3)
    sd      a1, 24(s3)
    sd      a2, 32(s3)

    /* ---- aligned neighbours: these must NOT trap ---- */
    lbu     s4, 16(s3)                          /* sig0 */
    lw      s5, 20(s3)                          /* sig1 */
    lh      s6, 22(s3)                          /* sig2 */

    /* ---- expected trap 1: misaligned half load ---- */
    addi    t3, s3, 9
    la      a0, p08_t1
    li      a1, 1
    call    trap_expect
p08_t1:
    lh      t4, 0(t3)

    /* ---- expected trap 2: misaligned word load ---- */
    addi    t3, s3, 10
    la      a0, p08_t2
    li      a1, 1
    call    trap_expect
p08_t2:
    lw      t4, 0(t3)

    /* ---- expected trap 3: misaligned half store (odd, inside canary) ---- */
    addi    t3, s3, 7
    la      a0, p08_t3
    li      a1, 1
    call    trap_expect
p08_t3:
    sh      t0, 0(t3)

    /* ---- expected trap 4: misaligned double store (inside canary) ---- */
    addi    t3, s3, 12
    la      a0, p08_t4
    li      a1, 1
    call    trap_expect
p08_t4:
    sd      t0, 0(t3)

    /* ---- expected trap 5: store to boot_rom, which is not writable ---- */
    li      t3, MOSAIC_BOOT_ROM_BASE
    la      a0, p08_t5
    li      a1, 1
    call    trap_expect
p08_t5:
    sw      zero, 0(t3)

    /* ---- the canary must be untouched ---- */
    li      t5, 0                              /* verdict bit mask */
    ld      t0, 0(s3)
    ld      t1, 8(s3)
    or      t0, t0, t1
    xor     t0, t0, t2
    beqz    t0, 1f
    j       2f
1:
    ori     t5, t5, 1                           /* bit 0: canary intact */
2:

    /* ---- an aligned double round-trip of c, as a control ---- */
    sd      a2, 32(s3)
    ld      t0, 32(s3)
    bne     t0, a2, 3f
    ori     t5, t5, 2                           /* bit 1: c round-tripped */
3:

    /* ---- fold the recorded trap trace, 8 bits per record ---- */
    la      t0, __traplog_start
    li      t1, 0                              /* record index */
    li      t2, MOSAIC_TRAPLOG_RECORDS
    li      t6, 0                              /* accumulator */
4:
    bgeu    t1, t2, 5f
    slli    t3, t1, 4
    add     t3, t0, t3
    ld      t4, 0(t3)                          /* mcause */
    slli    t6, t6, 8
    or      t6, t6, t4
    addi    t1, t1, 1
    j       4b
5:
    slli    t6, t6, 2
    or      t6, t6, t5

    SIG0    s4
    SIG1    s5
    SIG2    s6
    SIG3    t6

    FINISH_PASS 8
    .size   main, . - main
