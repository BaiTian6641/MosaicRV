/* tests/programs/vec/src/v05_maskpfx_e8.S
 *
 * Capability: MASKPFX (family 11) -- vmsbf.m, vmsif.m, vmsof.m -- at SEW = 8,
 * LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x00 (RVV 1.0 SMALLEST_SEW: vsew = 0 -> SEW = 8,
 * vlmul = 0 -> m1, vta = vma = 0).  VLMAX = 16; `vsetvli t0, x0, 0x00`
 * requests VL = VLMAX = 16.
 * vstart is 0 (vsetvli clears it), which the mask-prefix family requires.
 *
 * A mask register is stored one bit per element packed 8 bits to a byte, LSB =
 * the lowest-numbered element, so a 16-bit mask is two little-endian bytes.
 *
 * ------------------------------------------------------------- derivation
 *
 * For a source mask whose first set bit over the active elements is at position
 * k, with the running prefix pfx = OR of the source bits strictly before the
 * current element (V spec, "Vector Mask Instructions"):
 *
 *   vmsbf.m  vd[i] = 1 iff no source bit at or before i is set   (bits i < k)
 *   vmsif.m  vd[i] = 1 iff no source bit strictly before i is set (bits i <= k)
 *   vmsof.m  vd[i] = 1 iff source bit i is the first set bit     (bit i == k)
 *
 * and when no active source bit is set (the all-zero row, which is where the
 * three are NOT symmetric): vmsbf and vmsif set every active bit, vmsof sets
 * none.
 *
 * Patterns (0x0001 first at 0, ..., 0x8000 first at 15, 0x0000 none) and the
 * expected 16-bit results in program order (vmsbf, vmsif, vmsof):
 *
 *   src 0x0001 -> 0x0000, 0x0001, 0x0001
 *   src 0x0002 -> 0x0001, 0x0003, 0x0002
 *   src 0x0080 -> 0x007F, 0x00FF, 0x0080       (first set at bit 7)
 *   src 0x0100 -> 0x00FF, 0x01FF, 0x0100       (first set at bit 8, across a byte)
 *   src 0x8000 -> 0x7FFF, 0xFFFF, 0x8000       (first set at bit 15)
 *   src 0x0000 -> 0xFFFF, 0xFFFF, 0x0000       (all-zero asymmetry)
 *   src 0x00FF -> 0x0000, 0x0001, 0x0001
 *   src 0xAAAA -> 0x0001, 0x0003, 0x0002
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 * Three 16-byte blocks per pattern (vmsbf, vmsif, vmsof), patterns in the order
 * above, so pattern p's vmsbf block starts at res + 48*p.  Each block's bytes
 * 2..15 are 0 (the destination is zeroed first and vma = 0 leaves the tail
 * undisturbed).  One check over the 384-byte block; check index 1.
 */

#include "vec.h"

/* Load source \src, run the three prefix ops into v4, store each to res+\off. */
.macro PFXBLK src, off
    la      t0, \src
    vle8.v v2, (t0)
    vmsbf.m v4, v2
    la      t0, res+\off
    vse8.v v4, (t0)
    vmsif.m v4, v2
    la      t0, res+\off+16
    vse8.v v4, (t0)
    vmsof.m v4, v2
    la      t0, res+\off+32
    vse8.v v4, (t0)
.endm

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E8_M1        /* e8, m1 -> vl = VLMAX = 16 */

    la      t0, zero16
    vle8.v v4, (t0)                    /* v4 = 0: tail bytes stay 0 */

    PFXBLK  src0, 0
    PFXBLK  src1, 48
    PFXBLK  src2, 96
    PFXBLK  src3, 144
    PFXBLK  src4, 192
    PFXBLK  src5, 240
    PFXBLK  src6, 288
    PFXBLK  src7, 336

    fence   rw, rw
    VEC_CHECK res, exp, 384, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
src0:   .half 0x0001
        .zero 14
src1:   .half 0x0002
        .zero 14
src2:   .half 0x0080
        .zero 14
src3:   .half 0x0100
        .zero 14
src4:   .half 0x8000
        .zero 14
src5:   .half 0x0000
        .zero 14
src6:   .half 0x00FF
        .zero 14
src7:   .half 0xAAAA
        .zero 14
zero16: .zero 16

exp:
    /* pattern 0x0001 */
    .half 0x0000
    .zero 14
    .half 0x0001
    .zero 14
    .half 0x0001
    .zero 14
    /* pattern 0x0002 */
    .half 0x0001
    .zero 14
    .half 0x0003
    .zero 14
    .half 0x0002
    .zero 14
    /* pattern 0x0080 */
    .half 0x007F
    .zero 14
    .half 0x00FF
    .zero 14
    .half 0x0080
    .zero 14
    /* pattern 0x0100 */
    .half 0x00FF
    .zero 14
    .half 0x01FF
    .zero 14
    .half 0x0100
    .zero 14
    /* pattern 0x8000 */
    .half 0x7FFF
    .zero 14
    .half 0xFFFF
    .zero 14
    .half 0x8000
    .zero 14
    /* pattern 0x0000 (all-zero: vmsbf/vmsif all-ones, vmsof none) */
    .half 0xFFFF
    .zero 14
    .half 0xFFFF
    .zero 14
    .half 0x0000
    .zero 14
    /* pattern 0x00FF */
    .half 0x0000
    .zero 14
    .half 0x0001
    .zero 14
    .half 0x0001
    .zero 14
    /* pattern 0xAAAA */
    .half 0x0001
    .zero 14
    .half 0x0003
    .zero 14
    .half 0x0002
    .zero 14

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  384, 0
