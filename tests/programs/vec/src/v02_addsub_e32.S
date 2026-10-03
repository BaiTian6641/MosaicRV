/* tests/programs/vec/src/v02_addsub_e32.S
 *
 * Capability: ADDSUB (family 0) -- vadd.vv, vsub.vv, vadd.vi, vsub.vx -- at
 * SEW = 32, LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x28 (this core's encoding: vsew = 5 -> SEW = 32,
 * vlmul = 0 -> m1, vta = vma = 0).  VLMAX = VLEN/SEW = 4; `vsetvli t0, x0,
 * 0x28` requests VL = VLMAX = 4.
 *
 * ------------------------------------------------------------------ inputs
 *
 * A (vs2), 4 x 32-bit: 0x00000001, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF
 * B (vs1), 4 x 32-bit: 0x00000002, 0x00000001, 0x80000000, 0x00000001
 *
 * ------------------------------------------------------------- derivation
 *
 * All results are modulo 2^32 (mask = 0xFFFFFFFF):
 *
 *   op0  vadd.vv v4, v2, v1        A + B
 *        [0x3, 0x00000000, 0x00000000, 0x80000000]
 *        (0xFFFFFFFF + 1 wraps to 0; 0x80000000 + 0x80000000 wraps to 0)
 *
 *   op1  vsub.vv v4, v2, v1        A - B
 *        [0xFFFFFFFF, 0xFFFFFFFE, 0x00000000, 0x7FFFFFFE]
 *
 *   op2  vadd.vi v4, v2, 5         A + 5   (imm field is 5-bit sign-extended)
 *        [0x00000006, 0x00000004, 0x80000005, 0x80000004]
 *
 *   op3  vsub.vx v4, v2, t1        t1 = 3; A - 3
 *        [0xFFFFFFFE, 0xFFFFFFFC, 0x7FFFFFFD, 0x7FFFFFFC]
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res + 0 .. 15  op0 (elements 0..3)
 *   res + 16 .. 31 op1
 *   res + 32 .. 47 op2
 *   res + 48 .. 63 op3
 *
 * One check over the 64-byte block; check index 1.
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
    la      t0, in_b
    vle32.v v1, (t0)                  /* v1 = B (vs1) */

    vadd.vv v4, v2, v1
    la      t0, res+0
    vse32.v v4, (t0)

    vsub.vv v4, v2, v1
    la      t0, res+16
    vse32.v v4, (t0)

    vadd.vi v4, v2, 5
    la      t0, res+32
    vse32.v v4, (t0)

    li      t1, 3
    vsub.vx v4, v2, t1
    la      t0, res+48
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 64, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0x00000001
    .word   0xFFFFFFFF
    .word   0x80000000
    .word   0x7FFFFFFF
in_b:
    .word   0x00000002
    .word   0x00000001
    .word   0x80000000
    .word   0x00000001
exp:
    .word   0x00000003, 0x00000000, 0x00000000, 0x80000000   /* op0 */
    .word   0xFFFFFFFF, 0xFFFFFFFE, 0x00000000, 0x7FFFFFFE   /* op1 */
    .word   0x00000006, 0x00000004, 0x80000005, 0x80000004   /* op2 */
    .word   0xFFFFFFFE, 0xFFFFFFFC, 0x7FFFFFFD, 0x7FFFFFFC   /* op3 */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  64, 0
