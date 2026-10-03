/* tests/programs/vec/src/v16_cmp_e32.S
 *
 * Capability: CMP (family 8) -- vmseq, vmsne, vmsltu, vmslt, vmsleu, vmsle,
 * vmsgtu, vmsgt (all .vv) -- at SEW = 32, LMUL = m1, VLEN = 128.
 *
 * Configuration: vtypei = 0x10 (vsew = 2 -> SEW = 32, vlmul = 0 -> m1, vta =
 * vma = 0).  VLMAX = 4; `vsetvli t0, x0, 0x10` requests VL = 4, so each
 * comparison produces a 4-bit mask (bits 0..3).
 *
 * ------------------------------------------------------------------ inputs
 *
 * vs2 (in_a): A = { 0x00000005, 0xFFFFFFFF, 0x80000000, 0x00000000 }
 *              = { 5, -1, -2^31, 0 } signed
 * vs1 (in_b): B = { 0x00000005, 0x00000001, 0x7FFFFFFF, 0x00000000 }
 *              = { 5, 1, 2^31 - 1, 0 } signed
 *
 * ------------------------------------------------------------- derivation
 *
 * Each result is a mask bit per element; stored with vse8.v under e8 the mask
 * occupies bit i of byte i/8.
 *
 *   op0 vmseq   0x09     op4 vmsleu  0x09
 *   op1 vmsne   0x06     op5 vmsle   0x0F
 *   op2 vmsltu  0x00     op6 vmsgtu  0x06
 *   op3 vmslt   0x06     op7 vmsgt   0x00
 *
 * (vmseq: elements 0 and 3 equal -> 0b1001; vmsle: all four true -> 0b1111.)
 *
 * The destination masks are stored one per 16-byte block with vse8.v; the
 * mask registers are zeroed first, so bytes 1..15 of every block are 0.
 * Under vta = 0 the destination tail is undisturbed.
 *
 * One check over the 128-byte block; check index 1.
 */

#include "vec.h"

    .section .text, "ax", @progbits
    .balign 4
    .globl  main
    .type   main, @function
main:
    vsetvli t0, x0, VTYPE_E8_M1       /* e8, m1: zero the mask registers */
    la      t0, zero16
    vle8.v v4, (t0)
    vle8.v v5, (t0)
    vle8.v v6, (t0)
    vle8.v v7, (t0)
    vle8.v v8, (t0)
    vle8.v v9, (t0)
    vle8.v v10, (t0)
    vle8.v v11, (t0)

    vsetvli t0, x0, VTYPE_E32_M1      /* e32, m1 -> vl = VLMAX = 4 */
    la      t0, in_a
    vle32.v v2, (t0)                  /* v2 = A (vs2) */
    la      t0, in_b
    vle32.v v1, (t0)                  /* v1 = B (vs1) */

    vmseq.vv  v4,  v2, v1
    vmsne.vv  v5,  v2, v1
    vmsltu.vv v6,  v2, v1
    vmslt.vv  v7,  v2, v1
    vmsleu.vv v8,  v2, v1
    vmsle.vv  v9,  v2, v1
    vmsgtu.vv v10, v2, v1
    vmsgt.vv  v11, v2, v1

    vsetvli t0, x0, VTYPE_E8_M1       /* e8, m1 -> vl = 16 for the stores */
    la      t0, res+0
    vse8.v v4, (t0)
    la      t0, res+16
    vse8.v v5, (t0)
    la      t0, res+32
    vse8.v v6, (t0)
    la      t0, res+48
    vse8.v v7, (t0)
    la      t0, res+64
    vse8.v v8, (t0)
    la      t0, res+80
    vse8.v v9, (t0)
    la      t0, res+96
    vse8.v v10, (t0)
    la      t0, res+112
    vse8.v v11, (t0)

    fence   rw, rw
    VEC_CHECK res, exp, 128, 1
    VEC_PASS
    .size   main, . - main

    .section .rodata, "a", @progbits
    .balign 8
in_a:
    .word   0x00000005
    .word   0xFFFFFFFF
    .word   0x80000000
    .word   0x00000000
in_b:
    .word   0x00000005
    .word   0x00000001
    .word   0x7FFFFFFF
    .word   0x00000000
zero16:
    .space  16, 0
exp:
    .byte   0x09                    /* vmseq */
    .space  15, 0
    .byte   0x06                    /* vmsne */
    .space  15, 0
    .byte   0x00                    /* vmsltu */
    .space  15, 0
    .byte   0x06                    /* vmslt */
    .space  15, 0
    .byte   0x09                    /* vmsleu */
    .space  15, 0
    .byte   0x0F                    /* vmsle */
    .space  15, 0
    .byte   0x06                    /* vmsgtu */
    .space  15, 0
    .byte   0x00                    /* vmsgt */
    .space  15, 0

    .section .bss, "aw", @nobits
    .balign 8
res:
    .space  128, 0
