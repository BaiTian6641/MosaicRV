/* tests/programs/exit/src/x07_pending_store.S -- V-012 scenario
 * "ends with a store still pending (not drained)".
 *
 * The program signals completion and *then* performs the last signature store:
 *
 *     sig[0..2]
 *     TOHOST = 1          <- the exit protocol is signalled here
 *     sig[3]              <- a store that has not even been issued yet
 *     park
 *
 * At the instant the exit is signalled this store is outstanding, so a harness
 * that accepted the exit there and read the signature would be reading a
 * window the program had not finished writing.  The harness's drain rule is
 * the one under test: no store to the signature window (or its guard band) may
 * be observed after the exit pulse, the store that wrote TOHOST must be
 * observed to retire after it, and the hart must be out of the data-request
 * states when the drain window closes.  If any of those is violated the run is
 * reported as the undrained-store class and the signature is not compared.
 *
 * This is the program-visible form of the hazard the validation plan names as
 * "未完成 store 就读 signature": in a design with a store buffer the same
 * ordering violation is possible without any program trying to cause it.
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

    /* signal completion before the signature is complete */
    li      t0, MOSAIC_TOHOST
    li      t1, 1
    sd      t1, 0(t0)

    /* the still-outstanding signature store */
    sd      t3, 24(s0)

1:  j       1b
    .size   _start, . - _start
