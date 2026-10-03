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
 * THIS CORE ENCODES THE `vsew` FIELD AS log2(SEW), NOT AS RVV 1.0's
 * SMALLEST_SEW CODE.  In RVV 1.0 the field bits [5:3] are 0/1/2/3 for
 * e8/e16/e32/e64; this core's mosaic_vec_cfg.sv accepts 3..6 and computes
 * `sew = 1 << vsew`, and its own unit case rvv.vtype_layout fixes exactly that
 * (VsewValid = 3..6, sew_log2 == vsew).  A `vsetvli rd, rs1, e32, m1` written
 * with the GAS mnemonic therefore encodes field 2, which this core treats as an
 * unsupported vtype (vill = 1, vl = 0), and every following vector instruction
 * is refused.  So these programs pass the raw 11-bit vtypei, with the field
 * value this core defines.  This is a machine assumption, not a derivation from
 * the spec, and it is called out again in README.md and in the report.
 *
 *   VTYPE_E8_M1  = (3 << 3) | 0  = 0x18   vsew=3 -> SEW=8,  vlmul=m1
 *   VTYPE_E16_M1 = (4 << 3) | 0  = 0x20   vsew=4 -> SEW=16, vlmul=m1
 *   VTYPE_E32_M1 = (5 << 3) | 0  = 0x28   vsew=5 -> SEW=32, vlmul=m1
 *   VTYPE_E64_M1 = (6 << 3) | 0  = 0x30   vsew=6 -> SEW=64, vlmul=m1
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

#define VTYPE_E8_M1            0x18
#define VTYPE_E16_M1           0x20
#define VTYPE_E32_M1           0x28
#define VTYPE_E64_M1           0x30

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
