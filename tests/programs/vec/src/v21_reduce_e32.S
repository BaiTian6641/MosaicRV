/* tests/programs/vec/src/v21_reduce_e32.S
 *
 * Capability: REDUCE (family 15) -- vredsum, vredand, vredor, vredxor,
 * vredminu, vredmin, vredmaxu, vredmax (all .vs) -- at SEW = 32, LMUL = m1,
 * VLEN = 128.
 *
 * Configuration: vtypei = 0x10 (vsew = 2 -> SEW = 32, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = 4; `vsetvli t0, x0, 0x10` requests VL = 4.  Only vd[0] is
 * written: the reduction's result is a scalar and the rest of the destination
 * group is undisturbed, so each op's block is compared in its first word.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs1 (in_acc): the initial accumulator is vs1[0] = 0; the rest of vs1 is
 *               unused by the fold.
 * vs2 (in_src): S = { 0x00000003, 0x00000005, 0x80000001, 0x00000007 }
 *               = { 3, 5, -2^31 + 1, 7 } signed
 *
 * ------------------------------------------------------------- derivation
 *
 *   vd[0] = vs1[0] op vs2[0] op vs2[1] op vs2[2] op vs2[3], ascending.
 *
 *   op0 vredsum   0 + 3 + 5 + 0x80000001 + 7 = 0x80000010
 *   op1 vredmaxu  max unsigned        = 0x80000001
 *   op2 vredmax   max signed          = 7
 *   op3 vredminu  min unsigned        = 0
 *   op4 vredmin   min signed          = 0x80000001 (-2^31 + 1)
 *   op5 vredand   0 & ...             = 0
 *   op6 vredor    3 | 5 | 0x80000001 | 7 = 0x80000007
 *   op7 vredxor   3 ^ 5 ^ 0x80000001 ^ 7 = 0x80000000
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res + 16*op .. 16*op + 3  op's result word (the rest of the 16-byte block
 *   is the untouched part of the destination group and is not compared)
 *
 * Eight checks of 4 bytes each; check index op+1.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E32_M1      /* e32, m1 -> vl = VLMAX = 4 */

    la      t0, in_acc
    vle32.v v1, (t0)                  /* v1 = initial accumulator (vs1[0]) */
    la      t0, in_src
    vle32.v v2, (t0)                  /* v2 = source (vs2) */

    vredsum.vs   v4, v2, v1
    la      t0, res+0
    vse32.v v4, (t0)
    vredmaxu.vs  v4, v2, v1
    la      t0, res+16
    vse32.v v4, (t0)
    vredmax.vs   v4, v2, v1
    la      t0, res+32
    vse32.v v4, (t0)
    vredminu.vs  v4, v2, v1
    la      t0, res+48
    vse32.v v4, (t0)
    vredmin.vs   v4, v2, v1
    la      t0, res+64
    vse32.v v4, (t0)
    vredand.vs   v4, v2, v1
    la      t0, res+80
    vse32.v v4, (t0)
    vredor.vs    v4, v2, v1
    la      t0, res+96
    vse32.v v4, (t0)
    vredxor.vs   v4, v2, v1
    la      t0, res+112
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res+0,   exp+0,  4, 1
    VEC_CHECK res+16,  exp+4,  4, 2
    VEC_CHECK res+32,  exp+8,  4, 3
    VEC_CHECK res+48,  exp+12, 4, 4
    VEC_CHECK res+64,  exp+16, 4, 5
    VEC_CHECK res+80,  exp+20, 4, 6
    VEC_CHECK res+96,  exp+24, 4, 7
    VEC_CHECK res+112, exp+28, 4, 8
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_acc:
    .word   0x00000000
    .word   0x0000000F
    .word   0xFFFFFFF0
    .word   0x00000001
in_src:
    .word   0x00000003
    .word   0x00000005
    .word   0x80000001
    .word   0x00000007
exp:
    .word   0x80000010        /* vredsum */
    .word   0x80000001        /* vredmaxu */
    .word   0x00000007        /* vredmax */
    .word   0x00000000        /* vredminu */
    .word   0x80000001        /* vredmin */
    .word   0x00000000        /* vredand */
    .word   0x80000007        /* vredor */
    .word   0x80000000        /* vredxor */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  128, 0
