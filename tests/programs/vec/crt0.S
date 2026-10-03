/* tests/programs/vec/crt0.S
 *
 * Reset path shared by the self-checking RVV programs in tests/programs/vec/.
 *
 * This is deliberately NOT the scalar corpus's crt0.S.  These programs are
 * their own reset path, so they cannot inherit the corpus's "main returned ->
 * PASS" shortcut, and they add the one thing a vector program needs that a
 * scalar one does not: `mstatus.VS` is enabled before the first `vsetvli`
 * (reset leaves it Off, and a `vsetvli` while VS = Off is an illegal
 * instruction).
 *
 * Boot sequence:
 *   1. establish the stack
 *   2. install a fatal trap handler (mtvec, direct mode)
 *   3. enable vector state (mstatus.VS = Initial)
 *   4. call main(); if it returns without ending the run, report PASS
 *
 * A trap is always FATAL here: these programs are expected to execute only
 * delivered, advertised operations, so any trap is a finding the run must
 * report, not something to absorb.  The handler records the raw mcause in
 * `vec_status` (bit 31 set) and writes a non-zero word with bit 0 clear to
 * TOHOST, which the frozen protocol reports as FAIL.
 */

#include "vec.h"

/* ================================================================== */
/* .stack: a small stack.  The programs nest at most two calls.        */
/* ================================================================== */
    .section .stack, "aw", @nobits
    .balign 16
    .globl  vec_stack_bottom
vec_stack_bottom:
    .space  1024, 0
    .balign 16
    .globl  vec_stack_top
vec_stack_top:
    .size   vec_stack_top, 0

/* ================================================================== */
/* .bss: the packed status word the harness reads back.               */
/* ================================================================== */
    .section .bss, "aw", @nobits
    .balign 8
    .globl  vec_status
vec_status:
    .space  8, 0

/* ================================================================== */
/* .text.init: the reset path.                                         */
/* ================================================================== */
    .section .text.init, "ax", @progbits
    .balign 4
    .globl  _start
    .type   _start, @function
_start:
    la      sp, vec_stack_top
    la      t0, vec_trap_fatal
    csrw    mtvec, t0
    VEC_ENABLE_VS
    call    main
    VEC_PASS
    .size   _start, . - _start

/* ================================================================== */
/* Shared helpers.                                                     */
/* ================================================================== */
    .section .text, "ax", @progbits
    .balign 4

/* vec_fail(a0 = check index, a1 = first differing byte offset)
 *
 * Record (check_index << 16) | (offset & 0xFFFF) in vec_status, write 2 to
 * TOHOST (FAIL: non-zero, bit 0 clear), and park.  Does not return. */
    .globl  vec_fail
    .type   vec_fail, @function
vec_fail:
    la      t0, vec_status
    slli    t1, a0, 16
    slli    a1, a1, 48          /* mask the offset to its low 16 bits */
    srli    a1, a1, 48
    or      t1, t1, a1
    sd      t1, 0(t0)
    li      t0, MOSAIC_TOHOST
    li      t1, 2
    sd      t1, 0(t0)
1:  j       1b
    .size   vec_fail, . - vec_fail

/* vec_trap_fatal -- any unexpected trap ends the run with FAIL.
 *
 * vec_status = 0x80000000 | mcause, TOHOST = 2 (FAIL).  Does not return. */
    .globl  vec_trap_fatal
    .type   vec_trap_fatal, @function
vec_trap_fatal:
    csrr    t0, mcause
    li      t1, 1
    slli    t1, t1, 31
    or      t0, t0, t1
    la      t1, vec_status
    sd      t0, 0(t1)
    li      t0, MOSAIC_TOHOST
    li      t1, 2
    sd      t1, 0(t0)
1:  j       1b
    .size   vec_trap_fatal, . - vec_trap_fatal

/* check_region(a0 = got, a1 = want, a2 = nbytes) -> a0
 *
 * Byte-by-byte compare.  Returns 0 when equal, else the first differing byte
 * offset + 1.  Leaf function; clobbers t0-t4 and a0 only. */
    .globl  check_region
    .type   check_region, @function
check_region:
    li      t0, 0
1:  bgeu    t0, a2, 3f
    add     t1, a0, t0
    add     t2, a1, t0
    lbu     t3, 0(t1)
    lbu     t4, 0(t2)
    bne     t3, t4, 2f
    addi    t0, t0, 1
    j       1b
2:  addi    a0, t0, 1
    ret
3:  li      a0, 0
    ret
    .size   check_region, . - check_region
