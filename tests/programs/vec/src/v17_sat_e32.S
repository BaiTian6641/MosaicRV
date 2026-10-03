/* tests/programs/vec/src/v17_sat_e32.S
 *
 * Capability: SAT (family 9) -- vsaddu.vv, vsadd.vv, vssubu.vv, vssub.vv,
 * vaaddu.vv, vaadd.vv, vasubu.vv, vasub.vv -- at SEW = 32, LMUL = m1,
 * VLEN = 128.
 *
 * Configuration: vtypei = 0x10 (vsew = 2 -> SEW = 32, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = 4; `vsetvli t0, x0, 0x10` requests VL = 4.  vxrm = 3
 * (rdn): the averaging forms use a truncating right shift, so no rounding term
 * appears in the constants.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a): A = { 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000, 0x00000001 }
 *              = { 2^31-1, -1, -2^31, 1 } signed
 * vs1 (in_b): B = { 0x00000001, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE }
 *              = { 1, -1, -1, -2 } signed
 *
 * ------------------------------------------------------------- derivation
 *
 *   op0 vsaddu  min(A + B, 2^32-1)             op4 vaaddu ((A + B) at 2*SEW) >> 1
 *   op1 vsadd   clamp(A + B, -2^31, 2^31-1)    op5 vaadd  ((A + B) at 2*SEW) >>> 1
 *   op2 vssubu  max(A - B, 0)                  op6 vasubu ((A - B) at 2*SEW) >> 1
 *   op3 vssub   clamp(A - B, -2^31, 2^31-1)    op7 vasub  ((A - B) at 2*SEW) >>> 1
 *
 * The averaging forms extend both operands to 2*SEW, compute there, shift right
 * by one, then truncate to SEW -- so vasubu's element 2 (0x80000000 - 0xFFFF
 * FFFF = 0xFFFFFFFF80000001 at 64 bits) truncates to 0xC0000000, not to the
 * 32-bit-modular 0x40000000.
 *
 *   op0 { 0x80000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF }
 *   op1 { 0x7FFFFFFF, 0xFFFFFFFE, 0x80000000, 0xFFFFFFFF }
 *   op2 { 0x7FFFFFFE, 0x00000000, 0x00000000, 0x00000000 }
 *   op3 { 0x7FFFFFFE, 0x00000000, 0x80000001, 0x00000003 }
 *   op4 { 0x40000000, 0xFFFFFFFF, 0xBFFFFFFF, 0x7FFFFFFF }
 *   op5 { 0x40000000, 0xFFFFFFFF, 0xBFFFFFFF, 0xFFFFFFFF }
 *   op6 { 0x3FFFFFFF, 0x00000000, 0xC0000000, 0x80000001 }
 *   op7 { 0x3FFFFFFF, 0x00000000, 0xC0000000, 0x00000001 }
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 15  op0      res + 64 ..  79  op4
 *   res + 16 .. 31  op1      res + 80 ..  95  op5
 *   res + 32 .. 47  op2      res + 96 .. 111  op6
 *   res + 48 .. 63  op3      res +112 .. 127  op7
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
    li      t0, 0x4
    csrw    0x00F, t0                 /* vcsr.vxrm = 2 (rdn) */

    la      t0, in_a
    vle32.v v2, (t0)                  /* v2 = A (vs2) */
    la      t0, in_b
    vle32.v v1, (t0)                  /* v1 = B (vs1) */

    vsaddu.vv v4, v2, v1
    la      t0, res+0
    vse32.v v4, (t0)

    vsadd.vv  v4, v2, v1
    la      t0, res+16
    vse32.v v4, (t0)

    vssubu.vv v4, v2, v1
    la      t0, res+32
    vse32.v v4, (t0)

    vssub.vv  v4, v2, v1
    la      t0, res+48
    vse32.v v4, (t0)

    vaaddu.vv v4, v2, v1
    la      t0, res+64
    vse32.v v4, (t0)

    vaadd.vv  v4, v2, v1
    la      t0, res+80
    vse32.v v4, (t0)

    vasubu.vv v4, v2, v1
    la      t0, res+96
    vse32.v v4, (t0)

    vasub.vv  v4, v2, v1
    la      t0, res+112
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 128, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0x7FFFFFFF
    .word   0xFFFFFFFF
    .word   0x80000000
    .word   0x00000001
in_b:
    .word   0x00000001
    .word   0xFFFFFFFF
    .word   0xFFFFFFFF
    .word   0xFFFFFFFE
exp:
    .word   0x80000000        /* op0 vsaddu */
    .word   0xFFFFFFFF
    .word   0xFFFFFFFF
    .word   0xFFFFFFFF
    .word   0x7FFFFFFF        /* op1 vsadd */
    .word   0xFFFFFFFE
    .word   0x80000000
    .word   0xFFFFFFFF
    .word   0x7FFFFFFE        /* op2 vssubu */
    .word   0x00000000
    .word   0x00000000
    .word   0x00000000
    .word   0x7FFFFFFE        /* op3 vssub */
    .word   0x00000000
    .word   0x80000001
    .word   0x00000003
    .word   0x40000000        /* op4 vaaddu */
    .word   0xFFFFFFFF
    .word   0xBFFFFFFF
    .word   0x7FFFFFFF
    .word   0x40000000        /* op5 vaadd */
    .word   0xFFFFFFFF
    .word   0xBFFFFFFF
    .word   0xFFFFFFFF
    .word   0x3FFFFFFF        /* op6 vasubu */
    .word   0x00000000
    .word   0xC0000000
    .word   0x80000001
    .word   0x3FFFFFFF        /* op7 vasub */
    .word   0x00000000
    .word   0xC0000000
    .word   0x00000001

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  128, 0
