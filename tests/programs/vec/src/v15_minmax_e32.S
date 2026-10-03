/* tests/programs/vec/src/v15_minmax_e32.S
 *
 * Capability: MINMAX (family 7) -- vminu.vv, vmin.vv, vmaxu.vv, vmax.vv -- at
 * SEW = 32, LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x10 (vsew = 2 -> SEW = 32, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = 4; `vsetvli t0, x0, 0x10` requests VL = 4.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a): A = { 0xFFFFFFFF, 0x00000005, 0x80000000, 0x7FFFFFFF }
 *              = { -1, 5, -2^31, 2^31 - 1 } signed
 * vs1 (in_b): B = { 0x00000001, 0xFFFFFFFB, 0x00000002, 0x80000001 }
 *              = { 1, -5, 2, -2^31 + 1 } signed
 *
 * ------------------------------------------------------------- derivation
 *
 *   op0 vminu.vv  v4:i = min(unsigned A[i], unsigned B[i])
 *   op1 vmin.vv   v4:i = min(signed   A[i], signed   B[i])
 *   op2 vmaxu.vv  v4:i = max(unsigned A[i], unsigned B[i])
 *   op3 vmax.vv   v4:i = max(signed   A[i], signed   B[i])
 *
 *   op0 { 0x00000001, 0x00000005, 0x00000002, 0x7FFFFFFF }
 *   op1 { 0xFFFFFFFF, 0xFFFFFFFB, 0x80000000, 0x80000001 }
 *   op2 { 0xFFFFFFFF, 0xFFFFFFFB, 0x80000000, 0x80000001 }
 *   op3 { 0x00000001, 0x00000005, 0x00000002, 0x7FFFFFFF }
 *
 * The unsigned and signed forms disagree at every element, so a wrong
 * signedness in the decoder or the ALU changes the bytes.
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

    vminu.vv v4, v2, v1
    la      t0, res+0
    vse32.v v4, (t0)

    vmin.vv  v4, v2, v1
    la      t0, res+16
    vse32.v v4, (t0)

    vmaxu.vv v4, v2, v1
    la      t0, res+32
    vse32.v v4, (t0)

    vmax.vv  v4, v2, v1
    la      t0, res+48
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 64, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0xFFFFFFFF
    .word   0x00000005
    .word   0x80000000
    .word   0x7FFFFFFF
in_b:
    .word   0x00000001
    .word   0xFFFFFFFB
    .word   0x00000002
    .word   0x80000001
exp:
    .word   0x00000001        /* op0 */
    .word   0x00000005
    .word   0x00000002
    .word   0x7FFFFFFF
    .word   0xFFFFFFFF        /* op1 */
    .word   0xFFFFFFFB
    .word   0x80000000
    .word   0x80000001
    .word   0xFFFFFFFF        /* op2 */
    .word   0xFFFFFFFB
    .word   0x80000000
    .word   0x80000001
    .word   0x00000001        /* op3 */
    .word   0x00000005
    .word   0x00000002
    .word   0x7FFFFFFF

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  64, 0
