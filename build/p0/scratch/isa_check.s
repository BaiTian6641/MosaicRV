# Cross-check set: every immediate format and every shift/mask boundary the
# decoder has to reproduce. Values are chosen to be unmistakable in objdump
# output: large negatives, all-ones, alternating patterns, and the exact
# shift-amount boundaries.

    .option norelax
    .text

# ---- U-type ---------------------------------------------------------------
    lui   x5, 0x12345           # imm[31:12] = 0x12345, low 12 zero
    lui   x6, 0xfedcb          # sign-extends to 0xfffffffffffedcb000
    lui   x7, 0x80000          # sign-extends to 0xffffffff80000000
    auipc x8, 0xabcde

# ---- I-type ---------------------------------------------------------------
    addi  x9,  x10, 2047        # +2047, positive extreme
    addi  x11, x12, -2048       # -2048, negative extreme
    addi  x13, x14, -1
    slti  x15, x16, 1
    sltiu x17, x18, -1          # -1 sign-extended, compared unsigned
    xori  x19, x20, 0x7ff
    ori   x21, x22, -2048
    andi  x23, x24, -1
    lb    x25, 1234(x26)
    lh    x27, -1234(x28)
    lw    x29, 2047(x30)
    ld    x31, -2048(x1)
    lbu   x2,  -1(x3)
    lhu   x4,  2047(x5)
    jalr  x6, -2048(x7)
    jalr  x8, 2047(x9)
    addiw x10, x11, -1
    slli  x12, x13, 0
    slli  x14, x15, 63
    srli  x16, x17, 0
    srli  x18, x19, 63
    srai  x20, x21, 1
    srai  x22, x23, 63
    slliw x24, x25, 0
    slliw x26, x27, 31
    srliw x28, x29, 31
    sraiw x30, x31, 5

# ---- S-type ---------------------------------------------------------------
    sb    x1, 2047(x2)
    sb    x3, -2048(x4)
    sh    x5, 1234(x6)
    sh    x7, -1234(x8)
    sw    x9, 64(x10)
    sd    x11, -2048(x12)
    sb    x13, -1(x14)
    sd    x15, 2047(x16)

# ---- B-type
    beq   x1, x2, .+2046
    beq   x3, x4, .-2048
    bne   x5, x6, .+2
    blt   x7, x8, .-2
    bge   x9, x10, .+4094
    bltu  x11, x12, .-4094
    bgeu  x13, x14, .+16
    beq   x15, x16, .-16
    bne   x17, x18, .+2
    bgeu  x19, x20, .+64
    blt   x21, x22, .-64

# ---- J-type
    jal   x19, .+1048574
    jal   x20, .-1048576
    jal   x21, .+2
    jal   x22, .-2
    jal   x23, .+262142
    jal   x24, .-262144
    jal   x25, .-2
    jal   x26, .-1048576
    jal   x0, .+0
    jal   ra, .+131070

# ---- M extension ----------------------------------------------------------
    mul   x1, x2, x3
    mulh  x4, x5, x6
    mulhsu x7, x8, x9
    mulhu x10, x11, x12
    div   x13, x14, x15
    divu  x16, x17, x18
    rem   x19, x20, x21
    remu  x22, x23, x24

# ---- OP / OP-IMM ----------------------------------------------------------
    add   x25, x26, x27
    sub   x28, x29, x30
    sll   x31, x1, x2
    slt   x3, x4, x5
    sltu  x6, x7, x8
    xor   x9, x10, x11
    srl   x12, x13, x14
    sra   x15, x16, x17
    or    x18, x19, x20
    and   x21, x22, x23

# ---- system ---------------------------------------------------------------
    ecall
    ebreak
    mret
    csrrw  x1, 0x300, x2
    csrrs  x3, 0xc00, x4
    csrrc  x5, 0xf11, x6
    csrrwi x7, 0x340, 31
    csrrsi x8, 0x305, 0
    csrrci x9, 0x180, 17
    csrrw  x0, 0x340, x0        # rd == rs1 == x0: no read, no write
    csrrs  x10, 0x340, x0       # rs1 == x0: read only, no write

# ---- misc-mem -------------------------------------------------------------
    fence
    fence rw, rw
    fence.i
