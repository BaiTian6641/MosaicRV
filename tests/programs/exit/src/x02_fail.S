/* tests/programs/exit/src/x02_fail.S -- V-012 scenario "reports an explicit FAIL".
 *
 * Identical to x01 up to the exit word, except that the value written to
 * TOHOST has its PASS bit clear: bits [63:1] carry MOSAIC_EXIT_FAIL_CODE.
 * The frozen rule is "TOHOST == 1 is PASS; any other non-zero value is FAIL",
 * so this program is a well-formed FAIL, not a malformed run: it must be
 * reported as the program-fail class with that code, and must never be
 * accepted as success.
 *
 * The signature is written before the FAIL word on purpose, so the run also
 * carries evidence that the program executed correctly up to the point where
 * it chose to report failure.  The harness checks that signature.
 */

#include "exit.h"

    .section .text.init, "ax", @progbits
    .balign 4
    .globl  _start
    .type   _start, @function
_start:
    li      t0, MOSAIC_EXIT_INPUT
    ld      a0, 0(t0)
    ld      a1, 8(t0)
    ld      a2, 16(t0)

    add     t0, a0, a1
    sub     t1, a0, a1
    xor     t2, a0, a1
    slli    t3, a0, 3
    xor     t3, t3, a2

    li      s0, MOSAIC_SIGNATURE_BASE
    sd      t0, 0(s0)
    sd      t1, 8(s0)
    sd      t2, 16(s0)
    sd      t3, 24(s0)

    /* explicit FAIL: (code << 1) | 0, PASS bit clear */
    li      t0, MOSAIC_TOHOST
    li      t1, (MOSAIC_EXIT_FAIL_CODE << 1)
    sd      t1, 0(t0)

1:  j       1b
    .size   _start, . - _start
