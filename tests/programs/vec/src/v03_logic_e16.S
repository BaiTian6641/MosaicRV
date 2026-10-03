/* tests/programs/vec/src/v03_logic_e16.S
 *
 * Capability: LOGIC (family 6) -- vand.vv, vor.vv, vxor.vv, vand.vx, vor.vi,
 * vxor.vi -- at SEW = 16, LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x20 (this core's encoding: vsew = 4 -> SEW = 16,
 * vlmul = 0 -> m1, vta = vma = 0).  VLMAX = VLEN/SEW = 8; `vsetvli t0, x0,
 * 0x20` requests VL = VLMAX = 8.
 *
 * ------------------------------------------------------------------ inputs
 *
 * A (vs2), 8 x 16-bit: 0xFFFF, 0x0F0F, 0xAAAA, 0x1234, 0x8000, 0x0001,
 *                      0x00FF, 0x5555
 * B (vs1), 8 x 16-bit: 0x0F0F, 0x00FF, 0x5555, 0xFFFF, 0x0000, 0x8001,
 *                      0xF0F0, 0xAAAA
 *
 * ------------------------------------------------------------- derivation
 *
 * Bitwise, within 16 bits:
 *
 *   op0  vand.vv A & B
 *        [0x0F0F, 0x000F, 0x0000, 0x1234, 0x0000, 0x0001, 0x00F0, 0x0000]
 *   op1  vor.vv  A | B
 *        [0xFFFF, 0x0FFF, 0xFFFF, 0xFFFF, 0x8000, 0x8001, 0xF0FF, 0xFFFF]
 *   op2  vxor.vv A ^ B
 *        [0xF0F0, 0x0FF0, 0xFFFF, 0xEDCB, 0x8000, 0x8000, 0xF00F, 0xFFFF]
 *   op3  vand.vx A & 0x0FF0
 *        [0x0FF0, 0x0F00, 0x0AA0, 0x0230, 0x0000, 0x0000, 0x00F0, 0x0550]
 *   op4  vor.vi  A | 0x000F   (imm = 15)
 *        [0xFFFF, 0x0F0F, 0xAAAF, 0x123F, 0x800F, 0x000F, 0x00FF, 0x555F]
 *   op5  vxor.vi A ^ 0xFFFF   (imm = -1, sign-extended to 0xFFFF)
 *        [0x0000, 0xF0F0, 0x5555, 0xEDCB, 0x7FFF, 0xFFFE, 0xFF00, 0xAAAA]
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 15  op0
 *   res + 16 .. 31  op1
 *   ...             one 16-byte block per op, in program order
 *   res + 80 .. 95  op5
 *
 * One check over the 96-byte block; check index 1.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E16_M1      /* e16, m1 -> vl = VLMAX = 8 */

    la      t0, in_a
    vle16.v v2, (t0)                  /* v2 = A (vs2) */
    la      t0, in_b
    vle16.v v1, (t0)                  /* v1 = B (vs1) */

    vand.vv v4, v2, v1
    la      t0, res+0
    vse16.v v4, (t0)

    vor.vv v4, v2, v1
    la      t0, res+16
    vse16.v v4, (t0)

    vxor.vv v4, v2, v1
    la      t0, res+32
    vse16.v v4, (t0)

    li      t1, 0x0FF0
    vand.vx v4, v2, t1
    la      t0, res+48
    vse16.v v4, (t0)

    vor.vi v4, v2, 15
    la      t0, res+64
    vse16.v v4, (t0)

    vxor.vi v4, v2, -1
    la      t0, res+80
    vse16.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 96, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .half   0xFFFF, 0x0F0F, 0xAAAA, 0x1234, 0x8000, 0x0001, 0x00FF, 0x5555
in_b:
    .half   0x0F0F, 0x00FF, 0x5555, 0xFFFF, 0x0000, 0x8001, 0xF0F0, 0xAAAA
exp:
    .half   0x0F0F, 0x000F, 0x0000, 0x1234, 0x0000, 0x0001, 0x00F0, 0x0000
    .half   0xFFFF, 0x0FFF, 0xFFFF, 0xFFFF, 0x8000, 0x8001, 0xF0FF, 0xFFFF
    .half   0xF0F0, 0x0FF0, 0xFFFF, 0xEDCB, 0x8000, 0x8000, 0xF00F, 0xFFFF
    .half   0x0FF0, 0x0F00, 0x0AA0, 0x0230, 0x0000, 0x0000, 0x00F0, 0x0550
    .half   0xFFFF, 0x0F0F, 0xAAAF, 0x123F, 0x800F, 0x000F, 0x00FF, 0x555F
    .half   0x0000, 0xF0F0, 0x5555, 0xEDCB, 0x7FFF, 0xFFFE, 0xFF00, 0xAAAA

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  96, 0
