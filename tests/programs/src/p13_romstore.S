/* tests/programs/src/p13_romstore.S
 *
 * A store into the read-only boot_rom region.
 *
 * This program was split out of p08_misaligned.  p08's expected trap trace is
 * [4,4,6,6,7], the first four entries of which depend on the frozen
 * misalignment policy (config/profiles/p0.json -> misalignment.load/store =
 * "trap").  Spike services misaligned accesses natively and exposes no option
 * to make it trap, so p08's misalignment claim can only ever be adjudicated by
 * a reference that implements the frozen policy.  This program contains only
 * the part that does NOT depend on that policy -- the store to boot_rom -- so
 * that part is checkable against the reference that is actually available.
 *
 * Covered instructions: sd, sd, sw, lbu, lw, lh, slli, or, xor.
 *
 * Scratch layout (MOSAIC_SCRATCH_BASE = 0x80001080):
 *    0 : sd a
 *    8 : sd b
 *   16 : sd c
 *
 * Expected trap trace, in order:
 *   1  sw at 0x00000000  (boot_rom is readable+executable but NOT writable,
 *                         error_response = access_fault) -> mcause 7
 *
 * No other access in this program can fault: every other access is naturally
 * aligned and inside RAM, which also makes the trap count itself part of the
 * signature.  A DUT that silently dropped the store, performed it, or took an
 * extra trap produces a different sig3.
 *
 * Computed signature:
 *   sig0 = lbu[0] = a & 0xff
 *   sig1 = lw[4]  = sext32(a >> 32)          (LW sign-extends)
 *   sig2 = lh[6]  = sext16(a >> 48)
 *   sig3 = ((trap_record_count << 8) | first_mcause) ^ a
 */

#include "platform.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    MOSAIC_SETUP_BASES
    MOSAIC_LOAD_INPUTS
    mv      s3, s1                             /* scratch */

    sd      a0, 0(s3)
    sd      a1, 8(s3)
    sd      a2, 16(s3)

    /* aligned neighbours first: these must not fault */
    lbu     s4, 0(s3)                          /* sig0 */
    lw      s5, 4(s3)                          /* sig1 */
    lh      s6, 6(s3)                          /* sig2 */

    /* the one armed trap: a store into the read-only boot_rom */
    la      t3, MOSAIC_BOOT_ROM_BASE
    la      a0, p13_t1
    li      a1, 1
    call    trap_expect
p13_t1:
    sw      a0, 0(t3)                          /* must raise mcause 7 */
    addi    s7, x0, 1                          /* only reached if it did NOT */

    /* fold the trap log into the signature */
    la      t0, __traplog_start
    ld      t1, 0(t0)                          /* mcause of record 0   */
    la      t2, mosaic_traplog_count
    ld      t3, 0(t2)                          /* records written      */
    slli    t4, t3, 8
    or      t6, t1, t4                         /* (count << 8) | mcause */
    xor     t6, t6, s7                         /* s7 is 0 unless the
                                                 * store was not trapped, so
                                                 * a silently-dropped store is
                                                 * visible in the signature */

    SIG0    s4
    SIG1    s5
    SIG2    s6
    SIG3    t6

    FINISH_PASS
    .size   main, . - main

/* The three compiled-in inputs for this (program, input) pair. */
    MOSAIC_PROGRAM_INPUTS