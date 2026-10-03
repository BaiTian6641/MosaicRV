/* tests/programs/vec/src/v07_control_wrong_addsub.S
 *
 * NEGATIVE CONTROL for CASE=vec.selfcheck_corpus.
 *
 * This is v01_addsub_e64.S with exactly one hand-derived constant deliberately
 * wrong: expected op1 element 1 is written as 0xFFFFFFFFFFFFFFFD instead of
 * 0xFFFFFFFFFFFFFFFE.  The DUT computes the correct 0xFFFFFFFFFFFFFFFE (the
 * derivation in v01 is unchanged), so the program's own comparison must fail at
 * the first differing byte -- byte 24 of the result block -- and the program
 * must write FAIL to TOHOST with vec_status = (check 1, offset 24).
 *
 * That failure is what proves the self-checks bite rather than the programs
 * merely not trapping.  tools/run_vector_selfcheck_controls.py builds this file
 * from a deleted build directory and requires the case's own driver to exit 1
 * and name the failure.  It is NOT part of the registered run: it is not in the
 * Makefile's PROGRAMS list and not in the driver's default program list.
 *
 * The rest of this file is v01 verbatim (see v01_addsub_e64.S for the full
 * derivation); the one changed line is marked WRONG below.
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
    .dword  0xFFFFFFFFFFFFFFFD          /* WRONG: op1[1] is really ...FE */
    .dword  0x0000000000000011          /* op2[0] */
    .dword  0x000000000000000F          /* op2[1] */
    .dword  0xFFFFFFFFFFFFFFFE          /* op3[0] */
    .dword  0xFFFFFFFFFFFFFFFC          /* op3[1] */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  64, 0
