/* sim/unit/reset_program.S -- the V-009 reset fixture.
 *
 * This is the program case reset.replay_determinism runs.  Its bytes are
 * embedded in sim/unit/tb_reset.cpp (the harness must not depend on a
 * toolchain at run time), and this file is the recipe that produces them:
 *
 *   riscv64-elf-gcc -march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
 *       -nostdlib -nostartfiles -ffreestanding -fno-builtin -fno-pic \
 *       -mno-relax -Wa,--fatal-warnings -c reset_program.S -o reset_program.o
 *   riscv64-elf-ld -Ttext=0x80000000 --no-relax -e _start \
 *       -o reset_program.elf reset_program.o
 *   riscv64-elf-objcopy -O binary reset_program.elf reset_program.bin
 *
 * 148 bytes, SHA-256
 *   2e58b8934922c23fd0db66de0dcb5f316e42f4598d8f6a65ab3edd4e18402997
 *
 * The disassembly is printed in results/reports/V-009-reset.md; every
 * instruction is rv64im_zicsr_zifencei.
 *
 * Deliberately short, so that a reset can be placed at any instruction
 * boundary, and deliberately boring: the only things it does that a reset case
 * cares about are, in order:
 *
 *   1. load a RAM word nothing has written -- the SRAM initialisation
 *      contract, because the value the program reads is whatever the
 *      harness's clear pass left there;
 *   2. a RAM store followed by a load of the same address;
 *   3. exactly one UART device write, so "no device write during reset" has
 *      something to be about;
 *   4. a CSR write and read of mscratch, so post-reset CSR state is observable;
 *   5. four signature words;
 *   6. a non-zero write to TOHOST, the frozen end-of-run signal.
 *
 * Every store it performs is unconditional and writes a constant, so the
 * program's effect on memory is idempotent: re-running it from reset after an
 * abort at any instruction boundary produces the same final memory.  That is
 * what makes "reset in the middle and restart" a comparison of transaction
 * state rather than of leftover data.
 */
    .equ UART_BASE,    0x00100000
    .equ TOHOST,       0x80001000
    .equ SIG_BASE,     0x80000400
    .equ SCRATCH,      0x80002000
    .equ UNTOUCHED,    0x80003000

    .section .text
    .globl _start
_start:
    li   t0, TOHOST
    li   t1, UART_BASE
    li   t2, UNTOUCHED
    li   t3, SIG_BASE
    li   t4, SCRATCH

    ld   a0, 0(t2)                 /* a0 = RAM word nothing wrote            */
    sd   a0, 0(t3)                 /* sig[0] = that word                     */

    li   a1, 0x5a5a5a5a5a5a5a5a
    sd   a1, 0(t4)                 /* RAM store                              */
    ld   a2, 0(t4)                 /* RAM load of the same address           */
    sd   a2, 8(t3)                 /* sig[1] = value read back               */

    li   a3, 0x41                  /* 'A'                                    */
    sb   a3, 0(t1)                 /* one UART device write                  */

    csrw mscratch, a2              /* CSR write                              */
    csrr a4, mscratch              /* CSR read                               */
    sd   a4, 16(t3)                /* sig[2] = mscratch round-trip           */

    li   a5, 0x600d
    sd   a5, 24(t3)                /* sig[3] = magic                         */

    li   a6, 1
    sd   a6, 0(t0)                 /* TOHOST = pass code: end of run         */
1:  j    1b

    .size _start, . - _start
