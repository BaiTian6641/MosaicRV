/* tests/programs/vec/src/v20_compress_e8.S
 *
 * Capability: COMPRESS (family 14) -- vcompress.vm -- at SEW = 8, LMUL = m1,
 * VLEN = 128.
 *
 * Configuration: vtypei = 0x00 (vsew = 0 -> SEW = 8, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = VLEN/SEW = 16; `vsetvli t0, x0, 0x00` requests VL = 16.
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_src):  S = { 0x10, 0x11, ..., 0x1F }
 * vs1 (in_mask): the vcompress mask register, a vector register whose bit i is
 *                the mask bit for element i:
 *                byte0 = 0x0F (elements 0..3), byte1 = 0x0A (elements 9, 11)
 *
 * ------------------------------------------------------------- derivation
 *
 *   vcompress.vm v4, v2, v1 packs the elements whose mask bit is set, in
 *   element order, into the low elements of v4; the remaining elements keep
 *   their previous value (v4 is zeroed here, and vta = 0).
 *   Selected: 0, 1, 2, 3, 9, 11 -> S[0..3] = 0x10..0x13, S[9] = 0x19,
 *   S[11] = 0x1B:
 *   { 0x10, 0x11, 0x12, 0x13, 0x19, 0x1B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res + 0 .. 15  the compressed vector
 *
 * One check over the 16-byte block; check index 1.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E8_M1       /* e8, m1 -> vl = VLMAX = 16 */

    la      t0, in_src
    vle8.v v2, (t0)                   /* v2 = source (vs2) */
    la      t0, in_mask
    vle8.v v1, (t0)                   /* v1 = mask (vs1) */
    la      t0, zero16
    vle8.v v4, (t0)                   /* v4 = 0: the unwritten tail */

    vcompress.vm v4, v2, v1
    la      t0, res+0
    vse8.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 16, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_src:
    .byte   0x10
    .byte   0x11
    .byte   0x12
    .byte   0x13
    .byte   0x14
    .byte   0x15
    .byte   0x16
    .byte   0x17
    .byte   0x18
    .byte   0x19
    .byte   0x1A
    .byte   0x1B
    .byte   0x1C
    .byte   0x1D
    .byte   0x1E
    .byte   0x1F
in_mask:
    .byte   0x0F
    .byte   0x0A
    .space  14, 0
zero16:
    .space  16, 0
exp:
    .byte   0x10
    .byte   0x11
    .byte   0x12
    .byte   0x13
    .byte   0x19
    .byte   0x1B
    .space  10, 0

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  16, 0
