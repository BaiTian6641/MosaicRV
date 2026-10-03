/* tests/programs/vec/vec.h
 *
 * Platform surface for the self-checking RVV programs in tests/programs/vec/.
 *
 * The memory map, the TOHOST/FROMHOST protocol and the reset vector are
 * transcribed from the same frozen configuration the scalar corpus uses
 * (../src/platform.h, which transcribes config/memory/p0.json and
 * config/profiles/p0.json).  Nothing here invents an address.
 *
 * ------------------------------------------------------------------ VLEN
 *
 * Every program in this directory assumes VLEN = 128 bits, ELEN = 64 bits.
 * That is the value rtl/core/mosaic_core.sv hardcodes as CORE_VEC_VLEN and the
 * one config/geometry/p2.json declares ("vector": {"vlen": 128, "elen": 64}).
 * The programs state their own VLEN/SEW assumption in their header comment and
 * set `vsetvli` explicitly before every vector instruction.
 *
 * ----------------------------------------------------------- the vtype word
 *
 * The `vsew` field bits [5:3] use the RVV 1.0 SMALLEST_SEW encoding:
 * 0/1/2/3 for e8/e16/e32/e64.  (This core previously used a pre-ratification
 * log2(SEW) encoding in the same field; that was fixed to the ratified
 * encoding, and these programs carry the spec values.)  A plain GAS
 * `vsetvli rd, rs1, e32, m1` therefore encodes exactly the value below, and
 * the programs pass the raw 11-bit vtypei anyway so the assumption is explicit.
 *
 *   VTYPE_E8_M1  = (0 << 3) | 0  = 0x00   vsew=0 -> SEW=8,  vlmul=m1
 *   VTYPE_E16_M1 = (1 << 3) | 0  = 0x08   vsew=1 -> SEW=16, vlmul=m1
 *   VTYPE_E32_M1 = (2 << 3) | 0  = 0x10   vsew=2 -> SEW=32, vlmul=m1
 *   VTYPE_E64_M1 = (3 << 3) | 0  = 0x18   vsew=3 -> SEW=64, vlmul=m1
 *
 * vta = vma = 0 (tail and mask undisturbed), so a masked-off destination
 * element keeps its previous value and every expected value below is the one
 * the spec defines for an undisturbed policy.
 *
 * ------------------------------------------------------------------ status
 *
 * A program that reaches its pass trap writes the word 1 to TOHOST (the frozen
 * PASS encoding) and records 1 in `vec_status`.  A program that detects a
 * mismatch records a packed status in `vec_status` and writes 2 (a non-zero
 * word with bit 0 clear) to TOHOST:
 *
 *   bit 31 clear : (check_index << 16) | first_differing_byte_offset
 *   bit 31 set   : 0x80000000 | mcause   (an unexpected trap)
 *
 * The harness reads `vec_status` out of the program image's memory and names
 * the first failure from it; see sim/unit/tb_core_vecselfcheck.cpp.
 */

#ifndef MOSAIC_VEC_VEC_H
#define MOSAIC_VEC_VEC_H

#include "platform.h"

#define MOSAIC_VEC_VLEN        128
#define MOSAIC_VEC_VLENB       16

#define VTYPE_E8_M1            0x00
#define VTYPE_E16_M1           0x08
#define VTYPE_E32_M1           0x10
#define VTYPE_E64_M1           0x18

/* mstatus.VS (bits [10:9]) = 1 (Initial), so vector state is usable.  Reset
 * leaves VS = Off and a `vsetvli` before this write is refused. */
.macro VEC_ENABLE_VS
    li      t0, 0x200
    csrs    mstatus, t0
.endm

/* End with PASS: vec_status = 1, TOHOST = 1 (the frozen PASS encoding). */
.macro VEC_PASS
    la      t0, vec_status
    li      t1, 1
    sd      t1, 0(t0)
    li      t0, MOSAIC_TOHOST
    li      t1, 1
    sd      t1, 0(t0)
9:  j       9b
.endm

/* Compare [a0, a0+a2) with [a1, a1+a2) byte by byte.
 * Returns a0 = 0 if equal, else the first differing byte offset + 1. */
.macro VEC_CHECK got, want, nbytes, index
    la      a0, \got
    la      a1, \want
    li      a2, \nbytes
    call    check_region
    beqz    a0, 8f
    addi    a1, a0, -1          /* first differing byte offset */
    li      a0, \index          /* which result block failed      */
    j       vec_fail
8:
.endm

#endif /* MOSAIC_VEC_VEC_H */
