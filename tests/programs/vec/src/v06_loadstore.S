/* tests/programs/vec/src/v06_loadstore.S
 *
 * Capability: the vector load/store path (VEC_LSU_CAPS advertises unit-stride
 * only) at every element width it wires: EEW = 8/16/32/64, LMUL = m1,
 * VLEN = 128.
 *
 * Configuration: one `vsetvli` per width, each with the matching vtypei
 * (0x18/0x20/0x28/0x30 for e8/e16/e32/e64, m1), because the core requires the
 * load/store EEW to equal the current SEW.  VL is VLMAX at each width:
 * 16 / 8 / 4 / 2 elements.
 *
 * ------------------------------------------------------------- derivation
 *
 * A load followed by a store of the same register with no arithmetic must
 * reproduce the source bytes exactly.  There is no arithmetic to derive; the
 * expectation is the source data itself, and the check is that the round trip
 * is byte-exact at every width (so a wrong element width, a wrong stride or a
 * truncated/duplicated element is observable).
 *
 * Then a masked store: with v0 = 0x00F0 (elements 4..7 active) and the source
 * holding bytes 0x00..0x0F, only bytes 4..7 of the destination change; the
 * destination was pre-filled with 0xAA, so the expected destination is
 *   0xAA 0xAA 0xAA 0xAA  0x04 0x05 0x06 0x07  0xAA x 8
 *
 * ------------------------------------------------------------ result layout
 *
 *   res  +  0 .. 15  e8  round trip
 *   res  + 16 .. 31  e16 round trip
 *   res  + 32 .. 47  e32 round trip
 *   res  + 48 .. 63  e64 round trip
 *   mres +  0 .. 15  masked-store destination (pre-filled 0xAA)
 *
 * Check index 1 covers the 64-byte round-trip block; check index 2 covers the
 * 16-byte masked-store block.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    /* ---- e8 / vl = 16 ---- */
    vsetvli t0, x0, VTYPE_E8_M1
    la      t0, src8
    vle8.v v2, (t0)
    la      t0, res+0
    vse8.v v2, (t0)

    /* ---- e16 / vl = 8 ---- */
    vsetvli t0, x0, VTYPE_E16_M1
    la      t0, src16
    vle16.v v2, (t0)
    la      t0, res+16
    vse16.v v2, (t0)

    /* ---- e32 / vl = 4 ---- */
    vsetvli t0, x0, VTYPE_E32_M1
    la      t0, src32
    vle32.v v2, (t0)
    la      t0, res+32
    vse32.v v2, (t0)

    /* ---- e64 / vl = 2 ---- */
    vsetvli t0, x0, VTYPE_E64_M1
    la      t0, src64
    vle64.v v2, (t0)
    la      t0, res+48
    vse64.v v2, (t0)

    /* ---- masked store: e8, v0 = 0x00F0, destination pre-filled 0xAA ---- */
    vsetvli t0, x0, VTYPE_E8_M1
    la      t0, mres
    li      t1, 0xAAAAAAAAAAAAAAAA
    sd      t1, 0(t0)
    sd      t1, 8(t0)
    la      t0, maskf0
    vle8.v v0, (t0)
    la      t0, src8
    vle8.v v2, (t0)
    la      t0, mres
    vse8.v v2, (t0), v0.t

    fence   rw, rw
    VEC_CHECK res, exp, 64, 1
    VEC_CHECK mres, mexp, 16, 2
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
src8:
    .byte   0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    .byte   0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
src16:
    .half   0x1234, 0x5678, 0x9ABC, 0xDEF0, 0x0F1E, 0x2D3C, 0x4B5A, 0x6978
src32:
    .word   0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00
src64:
    .dword  0x0123456789ABCDEF, 0xFEDCBA9876543210
maskf0:
    .half   0x00F0
    .zero   14

exp:
    .byte   0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    .byte   0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    .half   0x1234, 0x5678, 0x9ABC, 0xDEF0, 0x0F1E, 0x2D3C, 0x4B5A, 0x6978
    .word   0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00
    .dword  0x0123456789ABCDEF, 0xFEDCBA9876543210
mexp:
    .byte   0xAA, 0xAA, 0xAA, 0xAA, 0x04, 0x05, 0x06, 0x07
    .byte   0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  64, 0
mres:
    .space  16, 0
