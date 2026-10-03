/* tests/programs/vec/src/v11_mul_e32.S
 *
 * Capability: MUL (family 2) -- vmul.vv, vmulh.vv, vmulhu.vv, vmulhsu.vx --
 * at SEW = 32, LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x10 (vsew = 2 -> SEW = 32, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = 4; `vsetvli t0, x0, 0x10` requests VL = 4.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a): A = { 0x00000002, 0xFFFFFFFF, 0x80000000, 0x00000004 }
 * vs1 (in_b): B = { 0x80000000, 0x00000002, 0x00000002, 0x40000000 }
 * scalar t1 = 0x80000000
 *
 * ------------------------------------------------------------- derivation
 *
 *   op0 vmul.vv    v4:i = (A[i] * B[i]) mod 2^32            (low half)
 *   op1 vmulh.vv   v4:i = (signed A[i] * signed B[i]) >> 32  (high half)
 *   op2 vmulhu.vv  v4:i = (unsigned A[i] * unsigned B[i]) >> 32
 *   op3 vmulhsu.vx v4:i = (signed A[i] * unsigned t1) >> 32
 *
 * With A = {2, -1, -2^31, 4}, B = {-2^31, 2, 2, 2^30}, t1 = 2^31:
 *   op0 { 0x00000000, 0xFFFFFFFE, 0x00000000, 0x00000000 }
 *   op1 { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000001 }
 *   op2 { 0x00000001, 0x00000001, 0x00000001, 0x00000001 }
 *   op3 { 0x00000001, 0xFFFFFFFF, 0xC0000000, 0x00000002 }
 *
 * (op3 element 2: signed A = -2^31 times unsigned 2^31 = -2^62, high 32 bits
 *  = -2^30 = 0xC0000000.)
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 15  op0      res + 32 .. 47  op2
 *   res + 16 .. 31  op1      res + 48 .. 63  op3
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

    vmul.vv    v4, v2, v1
    la      t0, res+0
    vse32.v v4, (t0)

    vmulh.vv   v4, v2, v1
    la      t0, res+16
    vse32.v v4, (t0)

    vmulhu.vv  v4, v2, v1
    la      t0, res+32
    vse32.v v4, (t0)

    li      t1, 0x80000000
    vmulhsu.vx v4, v2, t1
    la      t0, res+48
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 64, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0x00000002
    .word   0xFFFFFFFF
    .word   0x80000000
    .word   0x00000004
in_b:
    .word   0x80000000
    .word   0x00000002
    .word   0x00000002
    .word   0x40000000
exp:
    .word   0x00000000        /* op0[0] */
    .word   0xFFFFFFFE        /* op0[1] */
    .word   0x00000000        /* op0[2] */
    .word   0x00000000        /* op0[3] */
    .word   0xFFFFFFFF        /* op1[0] */
    .word   0xFFFFFFFF        /* op1[1] */
    .word   0xFFFFFFFF        /* op1[2] */
    .word   0x00000001        /* op1[3] */
    .word   0x00000001        /* op2[0] */
    .word   0x00000001        /* op2[1] */
    .word   0x00000001        /* op2[2] */
    .word   0x00000001        /* op2[3] */
    .word   0x00000001        /* op3[0] */
    .word   0xFFFFFFFF        /* op3[1] */
    .word   0xC0000000        /* op3[2] */
    .word   0x00000002        /* op3[3] */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  64, 0
