/* tests/programs/vec/src/v01_addsub_e64.S
 *
 * Capability: ADDSUB (family 0) -- vadd.vv, vsub.vv, vadd.vx, vsub.vx --
 * at SEW = 64, LMUL = m1, VLEN = 128.
 *
 * Configuration: vsetvli with vtypei = 0x18 (RVV 1.0 SMALLEST_SEW: vsew = 3 ->
 * SEW = 64, vlmul = 0 -> m1, vta = vma = 0).  VLMAX = VLEN/SEW = 2, and the
 * program requests VL = VLMAX with `vsetvli t0, x0, 0x18`.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a), element 0..1 (little-endian, 8 bytes each):
 *   A[0] = 0x0000000000000001
 *   A[1] = 0xFFFFFFFFFFFFFFFF
 * vs1 (in_b):
 *   B[0] = 0x0000000000000002
 *   B[1] = 0x0000000000000001
 *
 * ------------------------------------------------------------- derivation
 *
 * RVV integer arithmetic is modulo 2^SEW.  With SEW = 64, mask = 2^64 - 1:
 *
 *   op0  vadd.vv v4, v2, v1      v4[i] = (A[i] + B[i]) mod 2^64
 *        [0] = 3
 *        [1] = 0xFFFFFFFFFFFFFFFF + 1 = 0x10000000000000000 mod 2^64 = 0
 *
 *   op1  vsub.vv v4, v2, v1      v4[i] = (A[i] - B[i]) mod 2^64
 *        [0] = 1 - 2 = 0xFFFFFFFFFFFFFFFF
 *        [1] = 0xFFFFFFFFFFFFFFFF - 1 = 0xFFFFFFFFFFFFFFFE
 *
 *   op2  vadd.vx v4, v2, t1      t1 = 0x10; v4[i] = (A[i] + 0x10) mod 2^64
 *        [0] = 0x11
 *        [1] = 0xFFFFFFFFFFFFFFFF + 0x10 = 0x0F mod 2^64
 *
 *   op3  vsub.vx v4, v2, t1      t1 = 3; v4[i] = (A[i] - 3) mod 2^64
 *        [0] = 1 - 3 = 0xFFFFFFFFFFFFFFFE
 *        [1] = 0xFFFFFFFFFFFFFFFF - 3 = 0xFFFFFFFFFFFFFFFC
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 15  op0 (element 0, element 1)
 *   res + 16 .. 31  op1
 *   res + 32 .. 47  op2
 *   res + 48 .. 63  op3
 *
 * One check over the 64-byte block; check index 1.  A failure names the byte
 * offset, and the offsets above map it back to (op, element).
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E64_M1      /* e64, m1 -> vl = VLMAX = 2 */

    la      t0, in_a
    vle64.v v2, (t0)                  /* v2 = A (vs2) */
    la      t0, in_b
    vle64.v v1, (t0)                  /* v1 = B (vs1) */

    vadd.vv v4, v2, v1
    la      t0, res+0
    vse64.v v4, (t0)

    vsub.vv v4, v2, v1
    la      t0, res+16
    vse64.v v4, (t0)

    li      t1, 0x10
    vadd.vx v4, v2, t1
    la      t0, res+32
    vse64.v v4, (t0)

    li      t1, 3
    vsub.vx v4, v2, t1
    la      t0, res+48
    vse64.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 64, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .dword  0x0000000000000001
    .dword  0xFFFFFFFFFFFFFFFF
in_b:
    .dword  0x0000000000000002
    .dword  0x0000000000000001
exp:
    .dword  0x0000000000000003          /* op0[0] */
    .dword  0x0000000000000000          /* op0[1] */
    .dword  0xFFFFFFFFFFFFFFFF          /* op1[0] */
    .dword  0xFFFFFFFFFFFFFFFE          /* op1[1] */
    .dword  0x0000000000000011          /* op2[0] */
    .dword  0x000000000000000F          /* op2[1] */
    .dword  0xFFFFFFFFFFFFFFFE          /* op3[0] */
    .dword  0xFFFFFFFFFFFFFFFC          /* op3[1] */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  64, 0
