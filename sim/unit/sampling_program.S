/* sim/unit/sampling_program.S -- the V-010 sampling-calibration fixture.
 *
 * This is the program case harness.sampling_calibration runs.  Its bytes are
 * embedded in sim/unit/tb_sampling.cpp (the harness must not depend on a
 * toolchain at run time), and this file is the recipe that produces them:
 *
 *   riscv64-elf-gcc -march=rv64im_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
 *       -nostdlib -nostartfiles -ffreestanding -fno-builtin -fno-pic \
 *       -mno-relax -Wa,--fatal-warnings -c sampling_program.S -o sampling_program.o
 *   riscv64-elf-ld -Ttext=0x80000000 --no-relax -e _start \
 *       -o sampling_program.elf sampling_program.o
 *   riscv64-elf-objcopy -O binary sampling_program.elf sampling_program.bin
 *
 * The disassembly, the exact byte count and the SHA-256 are in
 * results/reports/V-010-sampling.md; every instruction is
 * rv64im_zicsr_zifencei and the program executes no CSR instruction and takes
 * no trap.
 *
 * WHY IT HAS THIS SHAPE
 * ---------------------
 * The case is about the harness's clock and sampling loop, so the program has
 * to have three properties and no others:
 *
 *   1. Real memory traffic.  Each loop iteration performs one store and one
 *      load, and the epilogue performs two more stores, so the data port
 *      handshakes many times and item-by-item backpressure on `h_d_gap` has
 *      something to hold off.
 *   2. Repeated PCs.  The loop body is fetched and retired four times, so the
 *      same `pc` legitimately appears in the retire stream with different
 *      sequence numbers.  That is what makes the repeated-sampling detector's
 *      identity `(retire_seq, pc)` and not just `pc`: a detector that keyed on
 *      `pc` alone would false-fire here, and the case would be measuring
 *      nothing.
 *   3. A non-zero TOHOST store as the last executed instruction, so the frozen
 *      end-of-run signal is the store whose own retire the harness must have
 *      recorded before it stops.  The trailing `j 1b` is never fetched: the
 *      run ends on the store's commit.
 *
 * The program reads only memory it wrote in the same run and writes only RAM
 * and TOHOST, so its final memory is a function of the program alone and the
 * reference model and the DUT have exactly the same thing to execute.
 */
    .equ TOHOST,   0x80001000
    .equ SIG_BASE, 0x80000400
    .equ BUF,      0x80002000

    .section .text
    .globl _start
_start:
    li   t0, BUF                   /* buffer base                            */
    li   t1, SIG_BASE              /* signature area                         */
    li   t2, TOHOST                /* end-of-run register                    */
    li   t3, 4                     /* loop trip count                        */
    li   a7, 0x1122334455667788    /* value to store and reload              */
    addi a6, zero, 0               /* accumulator                            */
loop:
    sd   a7, 0(t0)                 /* RAM store                              */
    ld   a0, 0(t0)                 /* RAM load of the same address           */
    add  a6, a6, a0                /* accumulate                             */
    addi t0, t0, 8                 /* next buffer word                       */
    addi t3, t3, -1                /* one trip fewer                         */
    bne  t3, zero, loop            /* fetch the body again                   */
    sd   a6, 0(t1)                 /* sig[0] = accumulated value             */
    sd   a6, 0(t2)                 /* TOHOST = non-zero: end of run          */
1:  j    1b                        /* never reached                          */

    .size _start, . - _start
