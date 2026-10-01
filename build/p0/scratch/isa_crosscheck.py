#!/usr/bin/env python3
"""Independent cross-check of the immediate formats against the GNU assembler.

Every instruction word of isa_check.dis is decoded here with the bit layouts
taken from the RISC-V ISA manual (volume I, "RV32I instruction formats"):

  I  imm[11:0]    = inst[31:25] inst[11:7]
  S  imm[11:0]    = inst[31:25] inst[11:7]        (same bits, other meaning)
  B  imm[12|10:5|4:1|11:0] = inst[31] inst[7] inst[30:25] inst[11:8] 0
  U  imm[31:12]   = inst[31:12], inst[11:0] = 0
  J  imm[20|10:1|11|19:12] = inst[31] inst[19:12] inst[20] inst[30:21] 0

and the reconstructed instruction text is compared with what objdump printed.
objdump is not this project's code, so a wrong formula here cannot agree with
the assembler by accident.

Usage: cd build/p0/scratch && python3 isa_crosscheck.py
"""

import re
import sys

ABI = ["zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
       "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
       "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
       "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"]

# The six CSRs used by isa_check.s, so that objdump's symbolic CSR operand can be
# turned back into the 12-bit address the decoder has to route.
CSR_NAMES = {"mstatus": 0x300, "cycle": 0xC00, "mvendorid": 0xF11,
             "mscratch": 0x340, "mtvec": 0x305, "satp": 0x180}

F3_R = {0: "add", 1: "sll", 2: "slt", 3: "sltu", 4: "xor", 5: "srl",
        6: "or", 7: "and"}
F3_I = {0: "addi", 2: "slti", 3: "sltiu", 4: "xori",
        6: "ori", 7: "andi"}
F3_LOAD = {0: "lb", 1: "lh", 2: "lw", 3: "ld", 4: "lbu", 5: "lhu"}
F3_STORE = {0: "sb", 1: "sh", 2: "sw", 3: "sd"}
F3_BRANCH = {0: "beq", 1: "bne", 4: "blt", 5: "bge", 6: "bltu", 7: "bgeu"}
F3_MD = {0: "mul", 1: "mulh", 2: "mulhsu", 3: "mulhu",
         4: "div", 5: "divu", 6: "rem", 7: "remu"}
F3_CSR = {1: "csrrw", 2: "csrrs", 3: "csrrc",
          5: "csrrwi", 6: "csrrsi", 7: "csrrci"}
PRED_SUCC = ((8, "i"), (4, "o"), (2, "r"), (1, "w"))


def sext(value, bits):
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


def imm_i(w):
    # I-type: imm[11:0] = inst[31:20] as one field. inst[11:7] is rd, which is
    # where the S-type layout takes its low half from -- the two must not be
    # confused, and the first version of this script did exactly that.
    return sext((w >> 20) & 0xFFF, 12)


def imm_s(w):
    return sext(((((w >> 25) & 0x7F) << 5) | ((w >> 7) & 0x1F)), 12)


def imm_b(w):
    return sext(((((w >> 31) & 1) << 12) | (((w >> 7) & 1) << 11) |
                 (((w >> 25) & 0x3F) << 5) | (((w >> 8) & 0xF) << 1)), 13)


def imm_j(w):
    return sext(((((w >> 31) & 1) << 20) | (((w >> 12) & 0xFF) << 12) |
                 (((w >> 20) & 1) << 11) | (((w >> 21) & 0x3FF) << 1)), 21)


def decode(word, addr):
    """The text objdump should have printed for `word` at `addr`."""
    op, rd, f3 = word & 0x7F, (word >> 7) & 0x1F, (word >> 12) & 7
    rs1, rs2, f7 = (word >> 15) & 0x1F, (word >> 20) & 0x1F, (word >> 25) & 0x7F
    r = ABI.__getitem__

    if op == 0x37:
        return "lui,%s,0x%x" % (r(rd), (word >> 12) & 0xFFFFF)
    if op == 0x17:
        return "auipc,%s,0x%x" % (r(rd), (word >> 12) & 0xFFFFF)
    if op == 0x6F:
        return "jal,%s,%x" % (r(rd), (addr + imm_j(word)) & (2 ** 64 - 1))
    if op == 0x67:
        assert f3 == 0, "jalr funct3=%d" % f3
        return "jalr,%s,%d(%s)" % (r(rd), imm_i(word), r(rs1))
    if op == 0x63:
        return "%s,%s,%s,%x" % (F3_BRANCH[f3], r(rs1), r(rs2),
                                (addr + imm_b(word)) & (2 ** 64 - 1))
    if op == 0x03:
        return "%s,%s,%d(%s)" % (F3_LOAD[f3], r(rd), imm_i(word), r(rs1))
    if op == 0x23:
        # S-type: insn[11:7] is imm[4:0], the data register is insn[24:20]
        return "%s,%s,%d(%s)" % (F3_STORE[f3], r(rs2), imm_s(word), r(rs1))
    if op == 0x0F:
        if f3 == 1:
            return "fence.i"
        assert f3 == 0, "misc-mem funct3=%d" % f3
        fm, pred, succ = (word >> 28) & 0xF, (word >> 24) & 0xF, (word >> 20) & 0xF
        return "fence," + "".join(c for m, c in PRED_SUCC if pred & m) + "," + \
            "".join(c for m, c in PRED_SUCC if succ & m)
    if op == 0x13:
        if f3 in F3_I:
            return "%s,%s,%s,%d" % (F3_I[f3], r(rd), r(rs1), imm_i(word))
        if f3 in (1, 5):
            top6 = (word >> 26) & 0x3F  # RV64: shamt is 6 bits, so the
            # legality field is insn[31:26], NOT insn[31:25]
            if f3 == 1:
                assert top6 == 0, "slli insn[31:26]=%02x" % top6
                name = "slli"
            else:
                assert top6 in (0, 0x10), "srli/srai insn[31:26]=%02x" % top6
                name = "srli" if top6 == 0 else "srai"
            return "%s,%s,%s,0x%x" % (name, r(rd), r(rs1), (word >> 20) & 0x3F)
        raise AssertionError("op-imm funct3=%d" % f3)
    if op == 0x1B:
        if f3 == 0:
            return "addiw,%s,%s,%d" % (r(rd), r(rs1), imm_i(word))
        if f3 in (1, 5):
            top5 = (word >> 25) & 0x7F  # RV64 word shifts: shamt is 5 bits,
            # so the legality field really is insn[31:25] here
            if f3 == 1:
                assert top5 == 0, "slliw insn[31:25]=%02x" % top5
                name = "slliw"
            else:
                assert top5 in (0, 0x20), "srliw/sraiw insn[31:25]=%02x" % top5
                name = "srliw" if top5 == 0 else "sraiw"
            return "%s,%s,%s,0x%x" % (name, r(rd), r(rs1), (word >> 20) & 0x1F)
        raise AssertionError("op-imm-32 funct3=%d" % f3)
    if op == 0x33:
        if f7 == 1:
            return "%s,%s,%s,%s" % (F3_MD[f3], r(rd), r(rs1), r(rs2))
        assert f7 in (0, 0x20), "op funct7=%02x" % f7
        name = F3_R[f3]
        if f3 == 0 and f7 == 0x20:
            name = "sub"
        if f3 == 5 and f7 == 0x20:
            name = "sra"
        return "%s,%s,%s,%s" % (name, r(rd), r(rs1), r(rs2))
    if op == 0x73:
        if f3 == 0:
            name = {0: "ecall", 1: "ebreak", 0x302: "mret"}[(word >> 20) & 0xFFF]
            return name
        src = str(rs1) if f3 >= 5 else r(rs1)
        return "%s,%s,0x%x,%s" % (F3_CSR[f3], r(rd), (word >> 20) & 0xFFF, src)
    raise AssertionError("opcode %02x" % op)


def normalise(printed):
    """Turn objdump's symbolic CSR operand back into its 12-bit address."""
    body = printed.split("#")[0].split("<")[0]
    tokens = body.split()          # objdump separates mnemonic and operands by
    parts = [tokens[0]] + [t.strip() for t in ",".join(tokens[1:]).split(",")]
    if len(parts) == 4 and parts[2] in CSR_NAMES:
        parts[2] = "0x%x" % CSR_NAMES[parts[2]]
    return ",".join(parts).rstrip(",")


def main():
    checked = mismatch = 0
    for line in open("isa_check.dis"):
        m = re.match(r"\s+([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)$", line)
        if not m:
            continue
        addr, word = int(m.group(1), 16), int(m.group(2), 16)
        printed, mine = normalise(m.group(3)), decode(word, addr)
        checked += 1
        if mine != printed:
            mismatch += 1
            print("MISMATCH %08x objdump=%-26s manual=%s" % (word, printed, mine))
    print("cross-checked %d instructions against objdump, %d mismatches"
          % (checked, mismatch))
    return 1 if mismatch else 0


if __name__ == "__main__":
    sys.exit(main())