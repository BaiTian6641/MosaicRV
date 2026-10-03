/* tests/programs/vec/src/v08_masked_addsub_e32.S
 *
 * Capability: the mask-enable path of ADDSUB -- vadd.vv / vsub.vv with the
 * `v0.t` masked form -- at SEW = 32, LMUL = m1, VLEN = 128.
 *
 * Configuration: the mask register is loaded first under vtypei = 0x00
 * (SEW = 8, so a `vle8.v` matches the SEW), then the arithmetic runs under
 * vtypei = 0x10 (SEW = 32, LMUL = m1, VL = VLMAX = 4, vta = vma = 0).  vma = 0
 * means a masked-off destination element is *undisturbed*: it keeps whatever
 * the destination register held before the instruction, which this program
 * makes deterministic by seeding v4.
 *
 * ------------------------------------------------------------------ inputs
 *
 * A (vs2), 4 x 32-bit: 0x00000001, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF
 * B (vs1), 4 x 32-bit: 0x00000002, 0x00000001, 0x80000000, 0x00000001
 * v0 mask            : elements 0 and 2 active (mask value 0x5)
 * v4 seed            : 0xDEADBEEF in all four elements
 *
 * ------------------------------------------------------------- derivation
 *
 * The spec's masked policy: element i is written iff v0[i] = 1; otherwise, with
 * vma = 0, it is left undisturbed.  All arithmetic is modulo 2^32.
 *
 *   op0  vadd.vv v4, v2, v1, v0.t
 *        [0] = 0x00000001 + 0x00000002 = 0x00000003     (active)
 *        [1] = 0xDEADBEEF                               (masked off, undisturbed)
 *        [2] = 0x80000000 + 0x80000000 = 0x00000000     (active, wraps)
 *        [3] = 0xDEADBEEF                               (masked off, undisturbed)
 *
 *   op1  vsub.vv v4, v2, v1, v0.t
 *        [0] = 0x00000001 - 0x00000002 = 0xFFFFFFFF     (active)
 *        [1] = 0xDEADBEEF                               (masked off)
 *        [2] = 0x80000000 - 0x80000000 = 0x00000000     (active)
 *        [3] = 0xDEADBEEF                               (masked off)
 *
 * The destination is re-seeded with 0xDEADBEEF before op1 so that op1's
 * masked-off elements are undisturbed from the seed and not from op0's result
 * (the same seed makes both derivations independent).
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res +  0 .. 15  op0
 *   res + 16 .. 31  op1
 *
 * One check over the 32-byte block; check index 1.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    /* The mask register must be loaded while SEW = 8: a vector load's EEW has
     * to equal the current SEW (the core refuses a mismatch as an illegal
     * instruction), and a mask operand is always the packed 8-bit layout.  v0
     * keeps its contents across the following vsetvli. */
    vsetvli t0, x0, VTYPE_E8_M1       /* e8, m1 -> vl = VLMAX = 16 */
    la      t0, v0mask
    vle8.v v0, (t0)                   /* v0 = element mask (bits 0 and 2) */

    vsetvli t0, x0, VTYPE_E32_M1      /* e32, m1 -> vl = VLMAX = 4 */

    la      t0, in_a
    vle32.v v2, (t0)                  /* v2 = A (vs2) */
    la      t0, in_b
    vle32.v v1, (t0)                  /* v1 = B (vs1) */

    la      t0, seed
    vle32.v v4, (t0)                  /* v4 = 0xDEADBEEF seed */
    vadd.vv v4, v2, v1, v0.t
    la      t0, res+0
    vse32.v v4, (t0)

    la      t0, seed
    vle32.v v4, (t0)                  /* re-seed for op1's masked-off elements */
    vsub.vv v4, v2, v1, v0.t
    la      t0, res+16
    vse32.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 32, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0x00000001, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF
in_b:
    .word   0x00000002, 0x00000001, 0x80000000, 0x00000001
v0mask:
    .byte   0x05
    .zero   15
seed:
    .word   0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF
exp:
    .word   0x00000003, 0xDEADBEEF, 0x00000000, 0xDEADBEEF   /* op0 */
    .word   0xFFFFFFFF, 0xDEADBEEF, 0x00000000, 0xDEADBEEF   /* op1 */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  32, 0
