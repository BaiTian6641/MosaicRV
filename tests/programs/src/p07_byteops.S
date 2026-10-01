/* tests/programs/src/p07_byteops.S
 *
 * Signed versus unsigned byte and half loads.  This is the test that
 * separates LB from LBU and LH from LHU: for a value with the top bit set
 * the signed form produces 0xffff...fXX and the unsigned form 0x0000...00XX,
 * and the two are folded into different signature words so a swap is caught.
 *
 * Covered instructions: sb, sh, sw, lb, lbu, lh, lhu, slli, srli, andi, or.
 *
 * A 32-bit word is assembled byte by byte from a and b:
 *   byte 0 = a[7:0]      byte 1 = a[15:8]
 *   byte 2 = b[7:0]      byte 3 = b[15:8]
 * and a half at scratch+4 holds c & 0xffff.
 *
 * Computed signature:
 *   sig0 = lbu[0]  = a & 0xff
 *   sig1 = lb[1]   = sext8(a[15:8])
 *   sig2 = lh[2]   = sext16((b[7:0] << 8) | b[15:8])
 *   sig3 = lhu[4]  = c & 0xffff
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

    andi    t0, a0, 0x000000FF
    slli    t0, t0, 8
    andi    t1, a0, 0x0000FF00
    or      t0, t0, t1                          /* bytes 0 and 1 from a */
    slli    t0, t0, 16
    andi    t1, a1, 0x000000FF
    or      t0, t0, t1                          /* byte 2 from b */
    slli    t0, t0, 8
    andi    t1, a1, 0x0000FF00
    or      t0, t0, t1                          /* byte 3 from b */
    sw      t0, 0(s3)

    andi    t1, a2, 0xFFFF
    sh      t1, 4(s3)

    lbu     s4, 0(s3)
    lb      s5, 1(s3)
    lh      s6, 2(s3)
    lhu     s7, 4(s3)

    SIG0    s4
    SIG1    s5
    SIG2    s6
    SIG3    s7

    FINISH_PASS 7
    .size   main, . - main