/* tests/programs/vec/src/v10_wide_e32.S
 *
 * Capability: WIDE (family 1) -- vwaddu.vv, vwadd.vv, vwsubu.vx, vwsub.vx --
 * at SEW = 32, LMUL = m1, VLEN = 128.  The destination is 2*SEW = 64 bits with
 * EMUL = 2, so the result group is the two registers v4:v5 (m2).
 *
 * Configuration: vtypei = 0x10 (RVV 1.0 SMALLEST_SEW: vsew = 2 -> SEW = 32,
 * vlmul = 0 -> m1).  VLMAX = VLEN/SEW = 4; `vsetvli t0, x0, 0x10` requests
 * VL = 4.  The destination is stored by writing its two m1 halves (v4, v5)
 * with e32 stores, which reads back the same 32 bytes.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a): A = { 0xFFFFFFFF, 0x00000001, 0x80000000, 0x00000000 }
 * vs1 (in_b): B = { 0x00000001, 0xFFFFFFFF, 0x00000002, 0xFFFFFFFF }
 * scalar t1 = 1
 *
 * ------------------------------------------------------------- derivation
 *
 * Widening: the source elements are extended to 2*SEW before the operation.
 *
 *   op0 vwaddu.vv v4, v2, v1   v4:i = zext(A[i]) + zext(B[i])       (64-bit)
 *   op1 vwadd.vv  v4, v2, v1   v4:i = sext(A[i]) + sext(B[i])
 *   op2 vwsubu.vx v4, v2, t1   v4:i = zext(A[i]) - zext(t1)
 *   op3 vwsub.vx  v4, v2, t1   v4:i = sext(A[i]) - sext(t1)
 *
 * With A = {-1, 1, -2^31, 0}, B = {1, -1, 2, -1}, t1 = 1:
 *   op0 { 0x0000000100000000, 0x0000000100000000,
 *         0x0000000080000002, 0x00000000FFFFFFFF }
 *   op1 { 0x0000000000000000, 0x0000000000000000,
 *         0xFFFFFFFF80000002, 0xFFFFFFFFFFFFFFFF }
 *   op2 { 0x00000000FFFFFFFE, 0x0000000000000000,
 *         0x000000007FFFFFFF, 0xFFFFFFFFFFFFFFFF }
 *   op3 { 0xFFFFFFFFFFFFFFFE, 0x0000000000000000,
 *         0xFFFFFFFF7FFFFFFF, 0xFFFFFFFFFFFFFFFF }
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 31  op0 (four 64-bit elements, little-endian)
 *   res + 32 .. 63  op1
 *   res + 64 .. 95  op2
 *   res + 96 ..127  op3
 *
 * One check over the 128-byte block; check index 1.
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

    vwaddu.vv v4, v2, v1
    vwadd.vv  v8, v2, v1
    li        t1, 1
    vwsubu.vx v12, v2, t1
    vwsub.vx  v16, v2, t1

    la      t0, res+0
    vse32.v v4, (t0)                  /* dst m2 halves, low then high */
    la      t0, res+16
    vse32.v v5, (t0)
    la      t0, res+32
    vse32.v v8, (t0)
    la      t0, res+48
    vse32.v v9, (t0)
    la      t0, res+64
    vse32.v v12, (t0)
    la      t0, res+80
    vse32.v v13, (t0)
    la      t0, res+96
    vse32.v v16, (t0)
    la      t0, res+112
    vse32.v v17, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 128, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0xFFFFFFFF
    .word   0x00000001
    .word   0x80000000
    .word   0x00000000
in_b:
    .word   0x00000001
    .word   0xFFFFFFFF
    .word   0x00000002
    .word   0xFFFFFFFF
exp:
    .dword  0x0000000100000000        /* op0[0] */
    .dword  0x0000000100000000        /* op0[1] */
    .dword  0x0000000080000002        /* op0[2] */
    .dword  0x00000000FFFFFFFF        /* op0[3] */
    .dword  0x0000000000000000        /* op1[0] */
    .dword  0x0000000000000000        /* op1[1] */
    .dword  0xFFFFFFFF80000002        /* op1[2] */
    .dword  0xFFFFFFFFFFFFFFFF        /* op1[3] */
    .dword  0x00000000FFFFFFFE        /* op2[0] */
    .dword  0x0000000000000000        /* op2[1] */
    .dword  0x000000007FFFFFFF        /* op2[2] */
    .dword  0xFFFFFFFFFFFFFFFF        /* op2[3] */
    .dword  0xFFFFFFFFFFFFFFFE        /* op3[0] */
    .dword  0x0000000000000000        /* op3[1] */
    .dword  0xFFFFFFFF7FFFFFFF        /* op3[2] */
    .dword  0xFFFFFFFFFFFFFFFF        /* op3[3] */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  128, 0
