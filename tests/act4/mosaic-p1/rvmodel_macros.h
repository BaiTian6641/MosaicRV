# rvmodel_macros.h
# DUT-specific macro definitions for the MosaicRV p1 profile (work package V-043).
# Derived from riscv-arch-test config/sail/sail-RVA20S64/rvmodel_macros.h (BSD-3-Clause).
#
# What differs from the Sail sample and why:
#
#   * RVMODEL_MTIMECMP_ADDRESS / RVMODEL_MTIME_ADDRESS are deliberately NOT
#     defined. The Sail sample defines them at 0x02004000 / 0x0200BFF8, which
#     lie outside the p1 profile's CLINT region (0x02000000, 0x1000 bytes). The
#     ACT4 trap prologue writes mtimecmp unconditionally when the macro is
#     defined, so the sample's value makes the very first instructions of every
#     ELF take a store access fault before mtvec is installed. With the macro
#     undefined the prologue emits five nops instead and the test runs.
#   * The interrupt-controller macros are left undefined for the same reason: the
#     p1 memory map has no simple-interrupt generator device, and no suite this
#     configuration generates drives an interrupt.
#
# The HTIF tohost/fromhost protocol is kept exactly as ACT4 expects: the
# specification of the exit protocol is `RVMODEL_HALT_PASS` writing 1 and
# `RVMODEL_HALT_FAIL` writing 3 to the `tohost` symbol. The DUT's testbench
# (sim/unit/tb_core_act.cpp) watches the retirement stream for a committed store
# to that symbol and reads the value.

#ifndef _RVMODEL_MACROS_H
#define _RVMODEL_MACROS_H

#define CLINT_BASE_ADDRESS 0x02000000

#define RVMODEL_DATA_SECTION \
        .pushsection .tohost,"aw",@progbits;                \
        .balign 8; .global tohost; tohost: .dword 0;         \
        .balign 8; .global fromhost; fromhost: .dword 0;     \
        .popsection

#define STANDARD_SM_SUPPORTED

##### STARTUP #####

// #define RVMODEL_BOOT

//#define RVMODEL_BOOT_TO_MMODE

##### TERMINATION #####

# Terminate test with a pass indication.
#define RVMODEL_HALT_PASS  \
  li x1, 1                ;\
  la t0, tohost           ;\
  write_tohost_pass:      ;\
    sw x1, 0(t0)          ;\
    sw x0, 4(t0)          ;\
    j write_tohost_pass   ;\


# Terminate test with a fail indication.
#define RVMODEL_HALT_FAIL \
  li x1, 3                ;\
  la t0, tohost           ;\
  write_tohost_fail:      ;\
    sw x1, 0(t0)          ;\
    sw x0, 4(t0)          ;\
    j write_tohost_fail   ;\


##### IO #####

// #define RVMODEL_IO_INIT(_R1, _R2, _R3)

# Prints a null-terminated string over HTIF (device 1). Used only by the
# failure diagnostics; a passing test never writes here.
#define RVMODEL_IO_WRITE_STR(_R1, _R2, _R3, _STR_PTR)               \
1:                           ;                       \
  lbu _R1, 0(_STR_PTR)        ;/* Load byte */        \
  beqz _R1, 3f                ;/* Exit if null */     \
2: /* htif_putc */           ;                      \
  la _R2, tohost       ;   \
  sw _R1, 0(_R2)     ; \
  /* device=1 (terminal), cmd=1 (output) */ \
  li _R1, 0x01010000 ;\
  sw _R1, 4(_R2)   ;\
  addi _STR_PTR, _STR_PTR, 1 ;/* Next char */        \
  j 1b                       ;/* Loop */             \
3:

##### Interrupt timing (required by check_defines.h even with no interrupt source) #####
#define RVMODEL_MAX_CYCLES_PER_TIMER_TICK 1
#define RVMODEL_INTERRUPT_LATENCY 1
#define RVMODEL_TIMER_INT_SOON_DELAY 5000

##### Access Fault #####

#define RVMODEL_ACCESS_FAULT_ADDRESS 0x00000000

#endif // _RVMODEL_MACROS_H
