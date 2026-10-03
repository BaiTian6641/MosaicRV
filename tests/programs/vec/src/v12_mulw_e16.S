/* tests/programs/vec/src/v12_mulw_e16.S
 *
 * Capability: MULW (family 3) -- vwmulu.vv, vwmulsu.vv, vwmul.vv -- at
 * SEW = 16, LMUL = m1, VLEN = 128.  The destination is 2*SEW = 32 bits with
 * EMUL = 2, so each result group is two registers (m2).
 *
 * Configuration: vtypei = 0x08 (vsew = 1 -> SEW = 16, vlmul = 0 -> m1).
 * VLMAX = VLEN/SEW = 8; `vsetvli t0, x0, 0x08` requests VL = 8.  Each result
 * group is stored by writing its two m1 halves with e32 stores.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a): A = { 0xFFFF, 0x8000, 0x0002, 0x0001, 0x7FFF, 0xFFFF, 0x0003, 0 }
 * vs1 (in_b): B = { 0x0002, 0x0002, 0x8000, 0xFFFF, 0x0002, 0xFFFF, 0x7FFF,
 *                   0x0010 }
 *
 * ------------------------------------------------------------- derivation
 *
 *   op0 vwmulu.vv  v4:i = zext(A[i]) * zext(B[i])           (32-bit)
 *   op1 vwmulsu.vv v4:i = sext(A[i]) * zext(B[i])
 *   op2 vwmul.vv   v4:i = sext(A[i]) * sext(B[i])
 *
 * With A = {-1, -32768, 2, 1, 32767, -1, 3, 0} and B = {2, 2, -32768, -1, 2,
 * -1, 32767, 16}:
 *   op0 { 0x0001FFFE, 0x00010000, 0x00010000, 0x0000FFFF,
 *         0x0000FFFE, 0xFFFE0001, 0x00017FFD, 0x00000000 }
 *   op1 { 0xFFFFFFFE, 0xFFFF0000, 0x00010000, 0x0000FFFF,
 *         0x0000FFFE, 0xFFFF0001, 0x00017FFD, 0x00000000 }
 *   op2 { 0xFFFFFFFE, 0xFFFF0000, 0xFFFF0000, 0xFFFFFFFF,
 *         0x0000FFFE, 0x00000001, 0x00017FFD, 0x00000000 }
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 31  op0      res + 64 .. 95  op2
 *   res + 32 .. 63  op1
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

    vwmulu.vv  v4, v2, v1
    vwmulsu.vv v8, v2, v1
    vwmul.vv   v12, v2, v1

    vsetvli t0, x0, VTYPE_E32_M1      /* e32, m1 -> vl = 4, for the stores */

    la      t0, res+0
    vse32.v v4, (t0)
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

    fence   rw, rw
    VEC_CHECK res, exp, 96, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .hword  0xFFFF
    .hword  0x8000
    .hword  0x0002
    .hword  0x0001
    .hword  0x7FFF
    .hword  0xFFFF
    .hword  0x0003
    .hword  0x0000
in_b:
    .hword  0x0002
    .hword  0x0002
    .hword  0x8000
    .hword  0xFFFF
    .hword  0x0002
    .hword  0xFFFF
    .hword  0x7FFF
    .hword  0x0010
exp:
    .word   0x0001FFFE        /* op0[0] */
    .word   0x00010000        /* op0[1] */
    .word   0x00010000        /* op0[2] */
    .word   0x0000FFFF        /* op0[3] */
    .word   0x0000FFFE        /* op0[4] */
    .word   0xFFFE0001        /* op0[5] */
    .word   0x00017FFD        /* op0[6] */
    .word   0x00000000        /* op0[7] */
    .word   0xFFFFFFFE        /* op1[0] */
    .word   0xFFFF0000        /* op1[1] */
    .word   0x00010000        /* op1[2] */
    .word   0x0000FFFF        /* op1[3] */
    .word   0x0000FFFE        /* op1[4] */
    .word   0xFFFF0001        /* op1[5] */
    .word   0x00017FFD        /* op1[6] */
    .word   0x00000000        /* op1[7] */
    .word   0xFFFFFFFE        /* op2[0] */
    .word   0xFFFF0000        /* op2[1] */
    .word   0xFFFF0000        /* op2[2] */
    .word   0xFFFFFFFF        /* op2[3] */
    .word   0x0000FFFE        /* op2[4] */
    .word   0x00000001        /* op2[5] */
    .word   0x00017FFD        /* op2[6] */
    .word   0x00000000        /* op2[7] */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  96, 0
