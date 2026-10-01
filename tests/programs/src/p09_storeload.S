/* tests/programs/src/p09_store_load_same.S
 *
 * Store-to-load at the SAME address, back to back, at several widths.
 *
 * This is the test that a store-to-load forwarding path, a non-blocking
 * cache, or a memory endpoint that reorders a store behind a later load
 * breaks: every load here reads an address the immediately preceding store
 * wrote, with no intervening instruction and no address distance to hide a
 * bug behind.
 *
 * Covered instructions: sd, sw, sh, sb, ld, lw, xor.
 *
 * Computed signature:
 *   sig0 = the double round-trip result, which must be a again after
 *          "sd a; ld t0; sd b; ld t1; sd t0; ld t2"
 *   sig1 = lw of the word written by "sw a, 4"   = a & 0xffffffff
 *   sig2 = ld[8] after "sd a, 8; sb b, 8"      = (a & ~0xff) | (b & 0xff)
 *          XOR ld[24] after "sd a, 24; sh b, 24"
 *                        = ((a & ~0xffff) | (b & 0xffff))
 *   sig3 = the double round-trip of c           = c
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

    /* ---- double width, same address, three times ---- */
    sd      a0, 0(s3)
    ld      t0, 0(s3)
    sd      a1, 0(s3)
    ld      t1, 0(s3)
    sd      t0, 0(s3)
    ld      t2, 0(s3)                          /* sig0 = a0 */

    /* ---- word width, same address ---- */
    sw      a0, 4(s3)
    lw      t3, 4(s3)                          /* sig1 = a & 0xffffffff */

    /* ---- byte store into the low byte of a double, same address ---- */
    sd      a0, 8(s3)
    sb      a1, 8(s3)
    ld      t4, 8(s3)

    /* ---- half store into the low half of a double, same address ---- */
    sd      a0, 24(s3)
    sh      a1, 24(s3)
    ld      t5, 24(s3)
    xor     s6, t4, t5                         /* sig2 */

    /* ---- plain double round-trip of the third input ---- */
    sd      a2, 16(s3)
    ld      t4, 16(s3)                         /* sig3 = a2 */

    SIG0    t2
    SIG1    t3
    SIG2    s6
    SIG3    t4

    FINISH_PASS
    .size   main, . - main