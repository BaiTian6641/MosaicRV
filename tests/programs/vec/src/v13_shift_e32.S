/* tests/programs/vec/src/v13_shift_e32.S
 *
 * Capability: SHIFT (family 4) -- vsll.vv, vsrl.vv, vsra.vv, vsll.vx -- at
 * SEW = 32, LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x10 (RVV 1.0 SMALLEST_SEW: vsew = 2 -> SEW = 32,
 * vlmul = 0 -> m1, vta = vma = 0).  VLMAX = VLEN/SEW = 4; `vsetvli t0, x0,
 * 0x10` requests VL = VLMAX = 4.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a), elements 0..3 (little-endian, 4 bytes each):
 *   A = { 0x80000001, 0x0000000F, 0xFFFFFFFF, 0x12345678 }
 * vs1 (in_b):
 *   B = { 0x00000001, 0x00000004, 0x0000001F, 0x00000000 }
 *
 * ------------------------------------------------------------- derivation
 *
 * The shift amount is `src & (SEW-1)` = `& 31`.  vsll is logical left, vsrl
 * logical right, vsra arithmetic right (sign-extending).  All results are the
 * low 32 bits.
 *
 *   op0 vsll.vv v4, v2, v1   v4[i] = (A[i] << (B[i]&31)) mod 2^32
 *       [0] 0x80000001 << 1  = 0x00000002
 *       [1] 0x0000000F << 4  = 0x000000F0
 *       [2] 0xFFFFFFFF << 31 = 0x80000000
 *       [3] 0x12345678 << 0  = 0x12345678
 *
 *   op1 vsrl.vv v4, v2, v1   v4[i] = A[i] >> (B[i]&31)   (logical)
 *       [0] 0x80000001 >> 1  = 0x40000000
 *       [1] 0x0000000F >> 4  = 0x00000000
 *       [2] 0xFFFFFFFF >> 31 = 0x00000001
 *       [3] 0x12345678 >> 0  = 0x12345678
 *
 *   op2 vsra.vv v4, v2, v1   v4[i] = A[i] >>> (B[i]&31)  (arithmetic)
 *       [0] 0x80000001 >>> 1  = 0xC0000000
 *       [1] 0x0000000F >>> 4  = 0x00000000
 *       [2] 0xFFFFFFFF >>> 31 = 0xFFFFFFFF
 *       [3] 0x12345678 >>> 0  = 0x12345678
 *
 *   op3 vsll.vx v4, v2, t1    t1 = 8; v4[i] = (A[i] << 8) mod 2^32
 *       [0] 0x00000100
 *       [1] 0x00000F00
 *       [2] 0xFFFFFF00
 *       [3] 0x34567800
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 15  op0 (elements 0..3)
 *   res + 16 .. 31  op1
 *   res + 32 .. 47  op2
 *   res + 48 .. 63  op3
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

    vsll.vv v4, v2, v1
    la      t0, res+0
    vse32.v v4, (t0)

    vsrl.vv v4, v2, v1
    la      t0, res+16
    vse32.v v4, (t0)

    vsra.vv v4, v2, v1
    la      t0, res+32
    vse32.v v4, (t0)

    li      t1, 8
    vsll.vx v4, v2, t1
    la      t0, res+48
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 64, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 4
in_a:
    .word   0x80000001
    .word   0x0000000F
    .word   0xFFFFFFFF
    .word   0x12345678
in_b:
    .word   0x00000001
    .word   0x00000004
    .word   0x0000001F
    .word   0x00000000
exp:
    .word   0x00000002        /* op0[0] */
    .word   0x000000F0        /* op0[1] */
    .word   0x80000000        /* op0[2] */
    .word   0x12345678        /* op0[3] */
    .word   0x40000000        /* op1[0] */
    .word   0x00000000        /* op1[1] */
    .word   0x00000001        /* op1[2] */
    .word   0x12345678        /* op1[3] */
    .word   0xC0000000        /* op2[0] */
    .word   0x00000000        /* op2[1] */
    .word   0xFFFFFFFF        /* op2[2] */
    .word   0x12345678        /* op2[3] */
    .word   0x00000100        /* op3[0] */
    .word   0x00000F00        /* op3[1] */
    .word   0xFFFFFF00        /* op3[2] */
    .word   0x34567800        /* op3[3] */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  64, 0
