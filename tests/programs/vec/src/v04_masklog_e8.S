/* tests/programs/vec/src/v04_masklog_e8.S
 *
 * Capability: MASKLOG (family 10) -- the eight mask-register logical
 * operations vmand, vmnand, vmor, vmnor, vmxor, vmxnor, vmandn, vmorn -- at
 * SEW = 8, LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x18 (this core's encoding: vsew = 3 -> SEW = 8,
 * vlmul = 0 -> m1, vta = vma = 0).  VLMAX = VLEN/SEW = 16; `vsetvli t0, x0,
 * 0x18` requests VL = VLMAX = 16.
 *
 * Mask-register logical instructions are always unmasked (vm = 1 is the only
 * legal encoding) and operate on the mask bits 0..vl-1.  A mask register is
 * stored to memory one bit per element packed 8 bits to a byte, LSB = the
 * lowest-numbered element, so a 16-bit mask is two little-endian bytes.
 *
 * ------------------------------------------------------------------ inputs
 *
 * pair 0: vs2 = A0 = 0xAAAA, vs1 = B0 = 0x00FF
 * pair 1: vs2 = A1 = 0x8001, vs1 = B1 = 0xFFFF
 *
 * ------------------------------------------------------------- derivation
 *
 * With a = vs2 bit and b = vs1 bit, all within 16 bits (the op order):
 *
 *   op0 vmand   a & b          op4 vmxor   a ^ b
 *   op1 vmnand  ~(a & b)       op5 vmxnor  ~(a ^ b)
 *   op2 vmor    a | b          op6 vmandn  a & ~b
 *   op3 vmnor   ~(a | b)       op7 vmorn   a | ~b
 *
 * pair 0 (A0 = 0xAAAA, B0 = 0x00FF):
 *   vmand  0x00AA   vmnand 0xFF55   vmor 0xAAFF   vmnor 0x5500
 *   vmxor  0xAA55   vmxnor 0x55AA   vmandn 0xAA00  vmorn 0xFFAA
 *
 * pair 1 (A1 = 0x8001, B1 = 0xFFFF):
 *   vmand  0x8001   vmnand 0x7FFE   vmor 0xFFFF   vmnor 0x0000
 *   vmxor  0x7FFE   vmxnor 0x8001   vmandn 0x0000  vmorn 0x8001
 *
 * ---------------------------------------------------- result layout (bytes)
 *
 *   res + 0..  15  pair0 vmand      bytes 0..1 = the 16-bit mask, 2..15 = 0
 *   res + 16.. 31  pair0 vmnand
 *   ...            one 16-byte block per op, pair0's eight ops then pair1's
 *   res + 240..255 pair1 vmorn
 *
 * Bytes 2..15 of every block are 0 because the destination register is zeroed
 * before the first op and a masked-operation destination under vma = 0 keeps
 * untouched tail bytes.  One check over the 256-byte block; check index 1.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E8_M1       /* e8, m1 -> vl = VLMAX = 16 */

    la      t0, zero16
    vle8.v v4, (t0)                   /* v4 = 0: tail bytes stay 0 */

    /* ---- pair 0 ---- */
    la      t0, a0
    vle8.v v2, (t0)
    la      t0, b0
    vle8.v v1, (t0)
    vmand.mm  v4, v2, v1
    la      t0, res+0
    vse8.v v4, (t0)
    vmnand.mm v4, v2, v1
    la      t0, res+16
    vse8.v v4, (t0)
    vmor.mm   v4, v2, v1
    la      t0, res+32
    vse8.v v4, (t0)
    vmnor.mm  v4, v2, v1
    la      t0, res+48
    vse8.v v4, (t0)
    vmxor.mm  v4, v2, v1
    la      t0, res+64
    vse8.v v4, (t0)
    vmxnor.mm v4, v2, v1
    la      t0, res+80
    vse8.v v4, (t0)
    vmandn.mm v4, v2, v1
    la      t0, res+96
    vse8.v v4, (t0)
    vmorn.mm  v4, v2, v1
    la      t0, res+112
    vse8.v v4, (t0)

    /* ---- pair 1 ---- */
    la      t0, a1
    vle8.v v2, (t0)
    la      t0, b1
    vle8.v v1, (t0)
    vmand.mm  v4, v2, v1
    la      t0, res+128
    vse8.v v4, (t0)
    vmnand.mm v4, v2, v1
    la      t0, res+144
    vse8.v v4, (t0)
    vmor.mm   v4, v2, v1
    la      t0, res+160
    vse8.v v4, (t0)
    vmnor.mm  v4, v2, v1
    la      t0, res+176
    vse8.v v4, (t0)
    vmxor.mm  v4, v2, v1
    la      t0, res+192
    vse8.v v4, (t0)
    vmxnor.mm v4, v2, v1
    la      t0, res+208
    vse8.v v4, (t0)
    vmandn.mm v4, v2, v1
    la      t0, res+224
    vse8.v v4, (t0)
    vmorn.mm  v4, v2, v1
    la      t0, res+240
    vse8.v v4, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 256, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
/* A mask operand is 2 bytes on the wire (16 elements) plus a zero tail. */
a0:     .half 0xAAAA
        .zero 14
b0:     .half 0x00FF
        .zero 14
a1:     .half 0x8001
        .zero 14
b1:     .half 0xFFFF
        .zero 14
zero16: .zero 16

exp:
    .half 0x00AA
    .zero 14                 /* pair0 vmand  */
    .half 0xFF55
    .zero 14                 /* pair0 vmnand */
    .half 0xAAFF
    .zero 14                 /* pair0 vmor   */
    .half 0x5500
    .zero 14                 /* pair0 vmnor  */
    .half 0xAA55
    .zero 14                 /* pair0 vmxor  */
    .half 0x55AA
    .zero 14                 /* pair0 vmxnor */
    .half 0xAA00
    .zero 14                 /* pair0 vmandn */
    .half 0xFFAA
    .zero 14                 /* pair0 vmorn  */
    .half 0x8001
    .zero 14                 /* pair1 vmand  */
    .half 0x7FFE
    .zero 14                 /* pair1 vmnand */
    .half 0xFFFF
    .zero 14                 /* pair1 vmor   */
    .half 0x0000
    .zero 14                 /* pair1 vmnor  */
    .half 0x7FFE
    .zero 14                 /* pair1 vmxor  */
    .half 0x8001
    .zero 14                 /* pair1 vmxnor */
    .half 0x0000
    .zero 14                 /* pair1 vmandn */
    .half 0x8001
    .zero 14                 /* pair1 vmorn  */

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  256, 0
