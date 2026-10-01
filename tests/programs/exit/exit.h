/* tests/programs/exit/exit.h
 *
 * Constants for the V-012 exit-protocol programs
 * (docs/validation-plan.md section 5 V-012, case exit.protocol_termination).
 *
 * These programs are NOT part of the p01..p13 firmware corpus and are NOT
 * listed in tests/programs/corpus.json.  They exist to drive the harness's
 * termination state machine: each one completes the frozen TOHOST protocol in
 * a different way -- correctly, with an explicit FAIL, not at all, by asking
 * the hart to wait for an interrupt that p0 can never deliver, with a
 * signature that disagrees with the harness's independent model, with a store
 * past the frozen signature window, or by signalling completion before its
 * last signature store has been issued.
 *
 * Because they are not corpus programs they are not built by
 * `make -C tests/programs all`, they are not counted by tools/host_oracle.py,
 * and the golden signatures in tests/programs/golden.json are untouched by
 * them.  They are built by `make -C tests/programs exit` into
 * tests/programs/build/exit/.
 *
 * Every address below is transcribed from frozen configuration via
 * src/platform.h, which is the corpus's single source of truth for the map and
 * the protocol; nothing here invents an address.
 */

#ifndef MOSAIC_EXIT_EXIT_H
#define MOSAIC_EXIT_EXIT_H

#include "platform.h"

/* Where the harness deposits the scenario operands {a, b, c}.
 *
 * MOSAIC_SCRATCH_BASE (0x80001080) is the firmware-owned 128-byte program
 * buffer declared in src/platform.h.  The harness writes the three 64-bit
 * operands there through the image-loader port while reset is still asserted,
 * so the operands have exactly one source of truth -- the harness's scenario
 * table -- and cannot drift from a copy compiled into a program.  No exit
 * program uses the buffer for anything else. */
#define MOSAIC_EXIT_INPUT          MOSAIC_SCRATCH_BASE

/* The four signature words every exit program computes:
 *
 *     sig[0] = a + b
 *     sig[1] = a - b
 *     sig[2] = a ^ b
 *     sig[3] = (a << 3) ^ c
 *
 * The harness evaluates the same four expressions in C++ from the same
 * operands and compares.  The assembly and the C++ are two independent
 * implementations written from this comment; a disagreement is the finding. */

/* The explicit-FAIL code written by x02_fail.S.  The frozen rule
 * (src/platform.h) is "TOHOST == 1 is PASS; any other non-zero value is FAIL
 * with bits [63:1] carrying a program-defined code", so the value stored is
 * (code << 1) | 0. */
#define MOSAIC_EXIT_FAIL_CODE      0x2A

/* The end of the signature area of interest.
 *
 * config/profiles/p0.json freezes the signature at 0x80000400 with exactly
 * four words, so the window is [0x80000400, 0x80000420).  The 512-byte page
 * that holds it is otherwise unassigned RAM in the frozen map (.scratch starts
 * at 0x80001080, .bss at 0x80001100), and the harness reserves the remainder
 * of that page as the signature guard band: a store that lands beyond the
 * frozen window but inside the page is a signature write that ran past the
 * region, and is refused rather than silently ignored.  x06_signature_overrun.S
 * stores at MOSAIC_EXIT_SIGNATURE_GUARD_WORD to produce exactly that. */
#define MOSAIC_EXIT_SIGNATURE_GUARD_END   0x80000500UL
#define MOSAIC_EXIT_SIGNATURE_GUARD_WORD  (MOSAIC_SIGNATURE_BASE + 8 * MOSAIC_SIGNATURE_WORDS)

/* The WFI encoding (SYSTEM, funct12 = 0x105, rd = rs1 = 0).  p0 has no
 * interrupt controller and no wake source, so a WFI that waits can never
 * retire; the harness recognises this instruction in the DUT's event stream
 * and reports a stall rather than a program FAIL.  x04_wfi.S executes it. */
#define MOSAIC_EXIT_WFI_INSN       0x10500073

#endif /* MOSAIC_EXIT_EXIT_H */
