/* tests/programs/termination/src/x08_runaway.S -- V-012 scenario
 * "runs into unprogrammed memory" (the card's "an empty program is not a
 * pass" fail mode).
 *
 * The program jumps to address 0, which in the frozen map is boot_rom: mapped,
 * readable and executable, and entirely zero.  It therefore runs off into
 * unprogrammed memory and never writes TOHOST.  Nothing about the run may be
 * reported as success, and the harness's RUN phase says so with a PC-ESCAPED
 * detail rather than waiting out the whole cycle budget.
 *
 * p0 program space in this profile is RAM: the reset vector (0x80000000) is in
 * RAM and every corpus and termination program is linked there, while boot_rom
 * is a device the firmware never executes from.  A PC outside RAM is therefore
 * a runaway, and the harness reports it as the timeout class with a named
 * phase -- a runaway is not a completion, and it is certainly not a pass.
 */

#include "exit.h"

    .section .text.init, "ax", @progbits
    .balign 4
    .globl  _start
    .type   _start, @function
_start:
    /* jalr x0, 0(x0): pc <- 0, the first byte of boot_rom */
    li      t0, 0
    jalr    x0, t0, 0

    /* unreachable: kept so the section has a defined end */
1:  j       1b
    .size   _start, . - _start
