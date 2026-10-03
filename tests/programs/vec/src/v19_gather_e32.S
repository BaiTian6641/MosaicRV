/* tests/programs/vec/src/v19_gather_e32.S
 *
 * Capability: GATHER (family 13) -- vrgather.vv -- at SEW = 32, LMUL = m1,
 * VLEN = 128.
 *
 * Configuration: vtypei = 0x10 (vsew = 2 -> SEW = 32, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = 4; `vsetvli t0, x0, 0x10` requests VL = 4.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a, the table):   A = { 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD }
 * vs1 (in_idx, indices):  I = { 3, 0, 1, 7 }
 *
 * ------------------------------------------------------------- derivation
 *
 *   vrgather.vv v4, v2, v1:  v4[i] = (I[i] < vlmax) ? vs2[I[i]] : 0
 *   { 0xDDDDDDDD, 0xAAAAAAAA, 0xBBBBBBBB, 0 }
 *
 * Element 3's index 7 is out of range (>= VLMAX = 4) and must yield 0, not an
 * access: the delivered unit evidence names that as the gather's boundary.
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res + 0 .. 15  the gathered vector
 *
 * One check over the 16-byte block; check index 1.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E32_M1      /* e32, m1 -> vl = VLMAX = 4 */

    la      t0, in_a
    vle32.v v2, (t0)                  /* v2 = table (vs2) */
    la      t0, in_idx
    vle32.v v1, (t0)                  /* v1 = indices (vs1) */

    vrgather.vv v4, v2, v1
    la      t0, res+0
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 16, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0xAAAAAAAA
    .word   0xBBBBBBBB
    .word   0xCCCCCCCC
    .word   0xDDDDDDDD
in_idx:
    .word   3
    .word   0
    .word   1
    .word   7
exp:
    .word   0xDDDDDDDD        /* op[0] = table[3] */
    .word   0xAAAAAAAA        /* op[1] = table[0] */
    .word   0xBBBBBBBB        /* op[2] = table[1] */
    .word   0x00000000        /* op[3]: index 7 is out of range */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  16, 0
