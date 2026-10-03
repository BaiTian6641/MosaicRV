/* tests/programs/vec/src/v18_slide_e32.S
 *
 * Capability: SLIDE (family 12) -- vslideup.vx, vslidedown.vx -- at SEW = 32,
 * LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x10 (vsew = 2 -> SEW = 32, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = 4; `vsetvli t0, x0, 0x10` requests VL = 4.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a): A = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 }
 * The destination v6 is cleared before the slideup, so the elements the slide
 * does not write stay 0.
 *
 * ------------------------------------------------------------- derivation
 *
 *   op0 vslideup.vx   v4,v2,2:  v4[i] = (i < 2) ? unchanged : vs2[i-2]
 *       { 0, 0, 0x11111111, 0x22222222 }
 *   op1 vslidedown.vx v4,v2,1:  v4[i] = (i+1 < vlmax) ? vs2[i+1] : 0
 *       { 0x22222222, 0x33333333, 0x44444444, 0 }
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 15  op0      res + 16 .. 31  op1
 *
 * One check over the 32-byte block; check index 1.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E32_M1      /* e32, m1 -> vl = VLMAX = 4 */

    la      t0, in_a
    vle32.v v2, (t0)                  /* v2 = A (vs2) */
    la      t0, zero32
    vle32.v v6, (t0)                  /* v6 = 0: the unwritten slideup elements */

    li      t1, 2
    vslideup.vx v6, v2, t1
    la      t0, res+0
    vse32.v v6, (t0)

    la      t0, zero32
    vle32.v v6, (t0)
    li      t1, 1
    vslidedown.vx v6, v2, t1
    la      t0, res+16
    vse32.v v6, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 32, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0x11111111
    .word   0x22222222
    .word   0x33333333
    .word   0x44444444
zero32:
    .space  16, 0
exp:
    .word   0x00000000        /* op0[0] */
    .word   0x00000000        /* op0[1] */
    .word   0x11111111        /* op0[2] */
    .word   0x22222222        /* op0[3] */
    .word   0x22222222        /* op1[0] */
    .word   0x33333333        /* op1[1] */
    .word   0x44444444        /* op1[2] */
    .word   0x00000000        /* op1[3] */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  32, 0
