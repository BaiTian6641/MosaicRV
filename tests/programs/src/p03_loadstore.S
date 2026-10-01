/* tests/programs/src/p03_loadstore.S
 *
 * Byte / half / word / double STORES at every width, and reads back with the
 * matching load widths.
 *
 * Covered instructions: sd, sw, sh, sb, ld, lw, lhu, lh, lb, lbu.
 *
 * Scratch layout (MOSAIC_SCRATCH_BASE = 0x80000500):
 *    0  : sd a
 *    8  : sd b
 *   16  : sw a >> 32
 *   20  : sh a & 0xffff
 *   22  : sb b & 0xff
 *   23  : sb a & 0xff
 *   24  : sd a
 *   32  : sd (a + c)
 *   40  : sh b & 0xffff
 *
 * Computed signature:
 *   sig0 = ld[0] ^ lw[16]        = a ^ (a >> 32)
 *   sig1 = lhu[20] ^ lh[40]      = (a & 0xffff) ^ sext16(b & 0xffff)
 *   sig2 = lb[23] ^ lbu[22]      = sext8(a & 0xff) ^ (b & 0xff)
 *   sig3 = ld[8] ^ ld[32]        = b ^ (a + c)
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS
    mv      s3, s1                             /* working scratch pointer */

    sd      a0, 0(s3)
    sd      a1, 8(s3)
    srli    t0, a0, 32
    sw      t0, 16(s3)
    slli    t1, a0, 48
    srli    t1, t1, 48                          /* t1 = a & 0xffff */
    sh      t1, 20(s3)
    andi    t2, a1, 0xFF
    sb      t2, 22(s3)
    andi    t3, a0, 0xFF
    sb      t3, 23(s3)
    sd      a0, 24(s3)
    add     t4, a0, a2
    sd      t4, 32(s3)
    slli    t5, a1, 48
    srli    t5, t5, 48                          /* t5 = b & 0xffff */
    sh      t5, 40(s3)

    /* ---- reads ---- */
    ld      t0, 0(s3)
    lw      t1, 16(s3)
    xor     s4, t0, t1                         /* sig0 */

    lhu     t0, 20(s3)
    lh      t1, 40(s3)
    xor     s5, t0, t1                         /* sig1 */

    lb      t0, 23(s3)
    lbu     t1, 22(s3)
    xor     s6, t0, t1                         /* sig2 */

    ld      t0, 8(s3)
    ld      t1, 32(s3)
    xor     s7, t0, t1                         /* sig3 */

    SIG0    s4
    SIG1    s5
    SIG2    s6
    SIG3    s7

    FINISH_PASS
    .size   main, . - main

/* The three compiled-in inputs for this (program, input) pair. */
    MOSAIC_PROGRAM_INPUTS
