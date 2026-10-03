/* tests/programs/vec/src/v22_redwide_e16.S
 *
 * Capability: REDWIDE (family 16) -- vwredsumu.vs, vwredsum.vs -- at SEW = 16,
 * LMUL = m1, VLEN = 128.  The accumulator is 2*SEW = 32 bits.
 *
 * Configuration: vtypei = 0x08 (vsew = 1 -> SEW = 16, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = VLEN/SEW = 8; `vsetvli t0, x0, 0x08` requests VL = 8.
 * The result is stored by switching to e32 and writing the destination's low
 * word.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs1 (in_acc): the initial accumulator is vs1[0] = 0x8123.
 * vs2 (in_src): S = { 0x1000, 0x2000, 0x8000, 0xFFFF,
 *                     0x0001, 0x0002, 0x4000, 0x0003 }
 *               = { 4096, 8192, -32768, -1, 1, 2, 16384, 3 } signed
 *
 * ------------------------------------------------------------- derivation
 *
 *   op0 vwredsumu.vs: acc = zext(vs1[0]); acc += zext(S[i]) for i = 0..7
 *       acc = 0x8123 + (0x1000+0x2000+0x8000+0xFFFF+1+2+0x4000+3) = 0x00027128
 *   op1 vwredsum.vs:  acc = sext(vs1[0]); acc += sext(S[i]) for i = 0..7
 *       acc = -0x7EDD + (4096+8192-32768-1+1+2+16384+3) = 0xFFFF7128
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res + 0 ..  3  op0 result word      res + 16 .. 19  op1 result word
 *
 * Two checks of 4 bytes each; check indices 1 and 2.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E16_M1      /* e16, m1 -> vl = VLMAX = 8 */

    la      t0, in_acc
    vle16.v v1, (t0)                  /* v1 = initial accumulator (vs1[0]) */
    la      t0, in_src
    vle16.v v2, (t0)                  /* v2 = source (vs2) */

    vwredsumu.vs v4, v2, v1
    vsetvli t0, x0, VTYPE_E32_M1      /* e32, m1 to store the 2*SEW result */
    la      t0, res+0
    vse32.v v4, (t0)

    vsetvli t0, x0, VTYPE_E16_M1      /* back to e16 for the second reduction */
    vwredsum.vs v4, v2, v1
    vsetvli t0, x0, VTYPE_E32_M1
    la      t0, res+16
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res+0,  exp+0, 4, 1
    VEC_CHECK res+16, exp+4, 4, 2
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_acc:
    .hword  0x8123
    .space  14, 0
in_src:
    .hword  0x1000
    .hword  0x2000
    .hword  0x8000
    .hword  0xFFFF
    .hword  0x0001
    .hword  0x0002
    .hword  0x4000
    .hword  0x0003
exp:
    .word   0x00027128        /* vwredsumu */
    .word   0xFFFF7128        /* vwredsum */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  32, 0
