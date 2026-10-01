
"""Independent RV64IM_Zicsr_Zifencei machine-mode reference for MosaicRV I-008.

THIS IS THE ORACLE.  A second implementation, written from the RISC-V ISA manual
and the Privileged Specification v1.12, reading the frozen configuration files
directly (config/memory/p0.json, config/csr/mode_m.json,
config/profiles/p0.json).  It never sees the RTL, the simulator or the DUT's
outputs.

It emits one line per architectural event in exactly the format of
sim/common/event_tap.cpp `mosaic::RetireEvent::Line()`, so a disagreement is a
one-line text diff that a person can read.
"""

import json
import os
import sys

M64 = (1 << 64) - 1

# Opcodes, written out: this model shares no code with the design under test,
# including its package.
OP_LOAD, OP_MISC_MEM, OP_IMM, OP_AUIPC, OP_IMM_32 = 0x03, 0x0F, 0x13, 0x17, 0x1B
OP_STORE, OP_LUI, OP_MUL_DIV, OP_BRANCH = 0x23, 0x37, 0x33, 0x63
OP_OP_32, OP_JALR, OP_JAL, OP_SYSTEM = 0x3B, 0x67, 0x6F, 0x73

ALU_ADD, ALU_SUB, ALU_SLL, ALU_SLT, ALU_SLTU = 0, 1, 2, 3, 4
ALU_XOR, ALU_SRL, ALU_SRA, ALU_OR, ALU_AND = 5, 6, 7, 8, 9
ALU_ADDW, ALU_SUBW, ALU_SLLW, ALU_SRLW, ALU_SRAW = 10, 11, 12, 13, 14
ALU_PASSB = 15

MD_MUL, MD_MULH, MD_MULHSU, MD_MULHU = 0, 1, 2, 3
MD_DIV, MD_DIVU, MD_REM, MD_REMU = 4, 5, 6, 7

CSR_RW, CSR_RS, CSR_RC = 1, 2, 3

# Privileged specification v1.12 exception codes.
EXC_INSN_MISALIGNED, EXC_INSN_ACCESS, EXC_ILLEGAL_INSN, EXC_BREAKPOINT = 0, 1, 2, 3
EXC_LOAD_MISALIGNED, EXC_LOAD_ACCESS = 4, 5
EXC_STORE_MISALIGNED, EXC_STORE_ACCESS = 6, 7
EXC_ECALL_M = 11


def u64(v):
    return v & M64


def sext(v, bits):
    """Sign-extend the low `bits` of v to 64 bits, returned two's complement."""
    v &= (1 << bits) - 1
    if (v >> (bits - 1)) & 1:
        v -= (1 << bits)
    return v & M64


def alu_eval(a, b, op):
    a, b = u64(a), u64(b)
    if op == ALU_ADD:
        return u64(a + b)
    if op == ALU_SUB:
        return u64(a - b)
    if op == ALU_SLL:
        return u64(a << (b & 63))
    if op == ALU_SLT:
        return 1 if (a - (1 << 64) if a >> 63 else a) < (b - (1 << 64) if b >> 63 else b) else 0
    if op == ALU_SLTU:
        return 1 if a < b else 0
    if op == ALU_XOR:
        return a ^ b
    if op == ALU_SRL:
        return a >> (b & 63)
    if op == ALU_SRA:
        return u64((a - (1 << 64) if a >> 63 else a) >> (b & 63))
    if op == ALU_OR:
        return a | b
    if op == ALU_AND:
        return a & b
    if op == ALU_ADDW:
        return u64(sext(a & 0xFFFFFFFF, 32) + sext(b & 0xFFFFFFFF, 32))
    if op == ALU_SUBW:
        return u64(sext(a & 0xFFFFFFFF, 32) - sext(b & 0xFFFFFFFF, 32))
    if op == ALU_SLLW:
        return sext(u64((a & 0xFFFFFFFF) << (b & 31)), 32)
    if op == ALU_SRLW:
        return sext((a & 0xFFFFFFFF) >> (b & 31), 32)
    if op == ALU_SRAW:
        return sext((a - (1 << 32) if a & 0x80000000 else a) >> (b & 31), 32)
    return b


def muldiv(a, b, op):
    a, b = u64(a), u64(b)
    sa = a - (1 << 64) if a >> 63 else a
    sb = b - (1 << 64) if b >> 63 else b
    if op == MD_MUL:
        return u64(a * b)
    if op == MD_MULH:
        return u64((sa * sb) >> 64)
    if op == MD_MULHSU:
        return u64((sa * b) >> 64)
    if op == MD_MULHU:
        return u64((a * b) >> 64)
    if op == MD_DIV:
        if b == 0:
            return M64
        if sa == -(1 << 63) and sb == -1:
            return a
        q = abs(sa) // abs(sb)
        return u64(-q if (sa < 0) != (sb < 0) else q)
    if op == MD_DIVU:
        return M64 if b == 0 else a // b
    if op == MD_REM:
        if b == 0:
            return a
        if sa == -(1 << 63) and sb == -1:
            return 0
        r = abs(sa) % abs(sb)
        return u64(-r if sa < 0 else r)
    if op == MD_REMU:
        return a if b == 0 else a % b
    return 0


class Memory(object):
    """config/memory/p0.json plus the TOHOST watch of the test protocol.

    Byte-granular and sparse.  Nothing is allocated until it is written, and an
    address no region covers is an access fault rather than a silent zero.  The
    profile's misalignment policy is the hart's, so this model does not policed
    it: an unaligned access is assembled byte by byte from whatever it covers.
    """

    def __init__(self, regions, tohost_addr):
        self.regions = regions
        self.tohost_addr = tohost_addr
        self.bytes = {}
        self.tohost = 0
        self.uart_writes = 0

    def region(self, addr):
        for r in self.regions:
            if r["base"] <= addr < r["base"] + r["size"]:
                return r
        return None

    def access_ok(self, addr, nbytes, is_write, is_exec):
        first = self.region(addr)
        if first is None:
            return False
        last = self.region(addr + nbytes - 1)
        if last is None or last["name"] != first["name"]:
            return False
        if is_exec:
            return bool(first["executable"])
        if is_write:
            return bool(first["writable"])
        return bool(first["readable"])

    def read(self, addr, n):
        value = 0
        for i in range(n):
            value |= self.bytes.get(addr + i, 0) << (8 * i)
        return value

    def write(self, addr, n, value):
        if (addr & ~7) == self.tohost_addr:
            self.tohost = value
        for i in range(n):
            self.bytes[addr + i] = (value >> (8 * i)) & 0xFF
        region = self.region(addr)
        if region is not None and region["kind"] == "mmio":
            self.uart_writes += 1


class CsrFile(object):
    """config/csr/mode_m.json.

    The legal set is exactly the table's keys, and a register is writable exactly
    when its entry has at least one writable field and is not read-only.  Nothing
    here is hard-coded that the configuration already states.
    """

    def __init__(self, table):
        self.table = table
        self.writable = {}
        for addr, entry in table.items():
            fields = entry.get("writable_fields") or []
            self.writable[addr] = bool(fields) and entry.get("access") != "ro"

    def is_legal(self, addr):
        return addr in self.table

    def is_writable(self, addr):
        return bool(self.writable.get(addr, False))

    def reset_of(self, addr):
        return u64(self.table[addr]["reset"])


class Machine(object):
    def __init__(self, mem, csrs, reset_vector, tohost_addr):
        self.mem = mem
        self.csrs = csrs
        self.tohost_addr = tohost_addr
        self.regs = [0] * 32
        self.pc = reset_vector
        self.mtvec = 0
        self.mscratch = 0
        self.mepc = 0
        self.mcause = 0
        self.mtval = 0
        self.mstatus = csrs.reset_of(0x300)
        self.medeleg = 0
        self.mideleg = 0
        self.mie = 0
        self.mip = 0
        self.cycles = 0
        self.minstret = 0
        self.lines = []

    def csr_read(self, addr):
        if addr == 0x300:
            return self.mstatus | 0x1800
        if addr == 0x301:
            return self.csrs.reset_of(0x301)
        if addr == 0x302:
            return self.medeleg
        if addr == 0x303:
            return self.mideleg
        if addr == 0x304:
            return self.mie
        if addr == 0x305:
            return self.mtvec
        if addr == 0x306:
            return 0
        if addr == 0x340:
            return self.mscratch
        if addr == 0x341:
            return self.mepc
        if addr == 0x342:
            return self.mcause
        if addr == 0x343:
            return self.mtval
        if addr == 0x344:
            return self.mip
        if addr in (0xF11, 0xF12, 0xF13, 0xF14, 0xC01):
            return 0
        if addr in (0xB00, 0xC00):
            return self.cycles
        if addr in (0xB02, 0xC02):
            return self.minstret
        return 0

    def csr_write(self, addr, value):
        value = u64(value)
        if addr == 0x300:
            self.mstatus = (value & 0x88) | 0x1800
        elif addr == 0x302:
            self.medeleg = value
        elif addr == 0x303:
            self.mideleg = value
        elif addr == 0x304:
            self.mie = value & 0x88
        elif addr == 0x305:
            self.mtvec = value & ~3
        elif addr == 0x340:
            self.mscratch = value
        elif addr == 0x341:
            self.mepc = value & ~3
        elif addr == 0x342:
            self.mcause = value
        elif addr == 0x343:
            self.mtval = value
        elif addr == 0x344:
            self.mip = value & 0x88
        elif addr == 0xB00:
            self.cycles = value
        elif addr == 0xB02:
            self.minstret = value

    def emit(self, seq, pc, next_pc, insn, has_rd, rd, rd_value,
             is_trap, cause, tval, epc, is_store, s_addr, s_data, s_size):
        line = "hart=0 seq=%016x pc=%016x next_pc=%016x insn=%08x" % (
            seq, pc, next_pc, insn)
        if has_rd:
            line += " rd=x%d val=%016x" % (rd, rd_value)
        if is_trap:
            line += " TRAP cause=%x tval=%016x epc=%016x" % (cause, tval, epc)
        if is_store:
            line += " STORE addr=%016x data=%016x size=%d" % (s_addr, s_data, s_size)
        self.lines.append(line)

    def take_trap(self, seq, cause, tval, insn):
        old = self.mstatus
        self.mstatus = ((old & ~0x88) |
                        (0x80 if (old & 0x08) else 0) |
                        (0x08 if (old & 0x80) else 0)) | 0x1800
        self.mepc = self.pc
        self.mcause = cause
        self.mtval = tval
        self.emit(seq, self.pc, self.mtvec, insn, False, 0, 0, True,
                  cause, tval, self.pc, False, 0, 0, 0)
        self.pc = self.mtvec
        self.cycles += 3

    def step(self, seq):
        pc = self.pc

        # IALIGN is 32 in this build, and this is the one trap that needs no
        # instruction, so it is taken before the fetch.
        if pc & 3:
            self.pc = pc
            self.take_trap(seq, EXC_INSN_MISALIGNED, pc, 0)
            return

        if not self.mem.access_ok(pc, 4, False, True):
            self.pc = pc
            self.take_trap(seq, EXC_INSN_ACCESS, pc, 0)
            return
        ir = self.mem.read(pc, 4)
        self.pc = pc

        opcode = ir & 0x7F
        rd = (ir >> 7) & 0x1F
        rs1 = (ir >> 15) & 0x1F
        rs2 = (ir >> 20) & 0x1F
        funct3 = (ir >> 12) & 7
        funct7 = (ir >> 25) & 0x7F

        a = self.regs[rs1] if rs1 else 0
        b = self.regs[rs2] if rs2 else 0

        illegal = False
        has_trap = False
        trap_cause = 0
        trap_tval = 0
        imm = 0
        mem_kind = 0
        mem_size = 0
        mem_signed = False
        mem_addr = 0
        store_value = 0
        is_store = False
        rd_value = 0
        has_rd = False
        next_pc = u64(pc + 4)
        uses_alu = False
        alu_a, alu_b, alu_op = 0, 0, ALU_PASSB
        is_mret = False
        is_ecall = False
        is_ebreak = False
        is_branch = False
        branch_taken = False

        if (ir & 3) != 3:
            illegal = True
        elif opcode == OP_LOAD:
            imm = sext(ir >> 20, 12)
            sizes = {0: (1, True), 1: (2, True), 2: (4, True), 3: (8, True),
                     4: (1, False), 5: (2, False), 6: (4, False)}
            if funct3 not in sizes:
                illegal = True
            else:
                mem_kind, mem_signed = 1, sizes[funct3][1]
                mem_size = sizes[funct3][0]
                mem_addr = u64(a + imm)
                has_rd = rd != 0
                uses_alu = True
                alu_a, alu_b, alu_op = a, imm, ALU_ADD
        elif opcode == OP_STORE:
            imm = sext((((ir >> 25) & 0x7F) << 5) | rd, 12)
            if funct3 == 0:
                mem_size = 1
            elif funct3 == 1:
                mem_size = 2
            elif funct3 == 2:
                mem_size = 4
            elif funct3 == 3:
                mem_size = 8
            else:
                illegal = True
            if not illegal:
                mem_kind, is_store = 2, True
                mem_addr = u64(a + imm)
                store_value = b & ((1 << (8 * mem_size)) - 1)
                uses_alu = True
                alu_a, alu_b, alu_op = a, imm, ALU_ADD
        elif opcode == OP_IMM:
            imm = sext(ir >> 20, 12)
            if ((ir >> 26) & 0x3F) != 0 and funct3 in (1, 5):
                # RV64 shifts the XLEN register by 6 bits; bit 25 belongs to the
                # immediate in RV32 and is reserved here.
                illegal = True
            else:
                alu_op = {0: ALU_ADD, 1: ALU_SLL, 2: ALU_SLT, 3: ALU_SLTU,
                          4: ALU_XOR,
                          5: ALU_SRA if (ir >> 25) & 1 else ALU_SRL,
                          6: ALU_OR, 7: ALU_AND}[funct3]
                alu_a, alu_b, uses_alu, has_rd = a, imm, True, rd != 0
        elif opcode == OP_IMM_32:
            imm = sext(ir >> 20, 12)
            if funct3 == 0:
                alu_op = ALU_ADDW
            elif funct3 == 1 and not ((ir >> 25) & 1) and ((ir >> 26) & 0x3F) == 0:
                alu_op = ALU_SLLW
            elif funct3 == 5 and ((ir >> 26) & 0x3F) == 0:
                alu_op = ALU_SRAW if (ir >> 25) & 1 else ALU_SRLW
            else:
                illegal = True
            if not illegal:
                alu_a, alu_b, uses_alu, has_rd = a, imm, True, rd != 0
        elif opcode == OP_LUI:
            # inst[31:12] is the immediate before the shift; LUI's immediate is
            # that value with the low 12 bits zero, sign-extended from bit 31.
            imm = sext(((ir >> 12) & 0xFFFFF) << 12, 32)
            alu_a, alu_b, alu_op = 0, imm, ALU_ADD
            uses_alu, has_rd = True, rd != 0
        elif opcode == OP_AUIPC:
            imm = sext(((ir >> 12) & 0xFFFFF) << 12, 32)
            alu_a, alu_b, alu_op = pc, imm, ALU_ADD
            uses_alu, has_rd = True, rd != 0
        elif opcode == OP_BRANCH:
            imm = sext((((ir >> 31) & 1) << 12) | (((ir >> 7) & 1) << 11) |
                       (((ir >> 25) & 0x3F) << 5) | (((ir >> 8) & 0xF) << 1), 13)
            is_branch = True
            if funct3 in (2, 3):
                illegal = True
            else:
                if funct3 == 0:
                    branch_taken = (a == b)
                elif funct3 == 1:
                    branch_taken = (a != b)
                elif funct3 == 4:
                    branch_taken = (s64(a) < s64(b))
                elif funct3 == 5:
                    branch_taken = not s64(a) < s64(b)
                elif funct3 == 6:
                    branch_taken = (a < b)
                else:
                    branch_taken = not a < b
                alu_op = {0: ALU_XOR, 1: ALU_XOR, 4: ALU_SLT, 5: ALU_SLT,
                          6: ALU_SLTU, 7: ALU_SLTU}[funct3]
                alu_a, alu_b, uses_alu = a, b, True
        elif opcode == OP_JAL:
            imm = sext((((ir >> 31) & 1) << 20) | (((ir >> 12) & 0xFF) << 12) |
                       (((ir >> 20) & 1) << 11) | (((ir >> 21) & 0x3FF) << 1), 21)
            next_pc = u64(pc + imm)
            has_rd = rd != 0
        elif opcode == OP_JALR:
            if funct3 != 0:
                illegal = True
            else:
                imm = sext(ir >> 20, 12)
                next_pc = u64((a + imm) & ~1)
                has_rd = rd != 0
        elif opcode == OP_MISC_MEM:
            # fence and fence.i are architectural no-ops in this cacheless,
            # single-issue machine: there is nothing for either to order.
            if funct3 == 0:
                pass
            elif funct3 == 1 and ((ir >> 28) & 0xF) == 0:
                pass
            else:
                illegal = True
            if (ir >> 15) & 0x1F or (ir >> 7) & 0x1F:
                illegal = True
        elif opcode == OP_MUL_DIV:
            if funct7 == 1:
                rd_value = muldiv(a, b, {0: MD_MUL, 1: MD_MULH, 2: MD_MULHSU,
                                         3: MD_MULHU, 4: MD_DIV, 5: MD_DIVU,
                                         6: MD_REM, 7: MD_REMU}[funct3])
                has_rd = rd != 0
            elif funct7 == 0:
                alu_op = {0: ALU_ADD, 1: ALU_SLL, 2: ALU_SLT, 3: ALU_SLTU,
                          4: ALU_XOR, 5: ALU_SRL, 6: ALU_OR, 7: ALU_AND}[funct3]
                alu_a, alu_b, uses_alu, has_rd = a, b, True, rd != 0
            elif funct7 == 0x20:
                # On this opcode funct7 = 0100000 selects exactly sub and sra.
                # The W forms are on OP-32 and srawi is an immediate form.
                if funct3 == 0:
                    alu_op = ALU_SUB
                elif funct3 == 5:
                    alu_op = ALU_SRA
                else:
                    illegal = True
                alu_a, alu_b, uses_alu, has_rd = a, b, True, rd != 0
            else:
                illegal = True
        elif opcode == OP_OP_32:
            alu_op = {(0x00, 0): ALU_ADDW, (0x00, 1): ALU_SLLW,
                      (0x00, 5): ALU_SRLW, (0x20, 0): ALU_SUBW,
                      (0x20, 5): ALU_SRAW}.get((funct7, funct3))
            if alu_op is None:
                illegal = True
            alu_a, alu_b, uses_alu, has_rd = a, b, True, rd != 0
        elif opcode == OP_SYSTEM:
            if funct3 == 0:
                if ir == 0x00000073:
                    is_ecall = True
                elif ir == 0x00100073:
                    is_ebreak = True
                elif ir == 0x30200073:
                    is_mret = True
                    next_pc = self.mepc
                    # MRET is not just "pc = mepc".  Privileged Specification
                    # v1.12, section 2.1.6.1: "When executing an xRET
                    # instruction, supposing xPP holds the value y, xIE is set
                    # to xPIE; the privilege mode is changed to y; xPIE is set
                    # to 1; and xPP is set to the least-privileged supported
                    # mode."  p0 implements no mode below M (see
                    # config/profiles/p0.json), so MPP stays 0b11 and only MIE
                    # (bit 3) and MPIE (bit 7) move.  This was the one place the
                    # reference left mstatus untouched, which made it disagree
                    # with the specification; see results/reports/I-008-bringup.md.
                    old_mstatus = self.mstatus
                    self.mstatus = ((old_mstatus & ~0x88) |
                                    (0x08 if (old_mstatus & 0x80) else 0) |
                                    0x80) | 0x1800
                else:
                    illegal = True
            elif funct3 == 4:
                illegal = True
            else:
                # funct3 1/2/3 are the register forms and 5/6/7 the immediate
                # forms; both encode the same three operations in bits 1:0.
                csr_addr = (ir >> 20) & 0xFFF
                csr_imm = funct3 >= 5
                csr_op = {1: CSR_RW, 2: CSR_RS, 3: CSR_RC}[funct3 & 3]
                operand = ((ir >> 15) & 0x1F) if csr_imm else a
                reads = (csr_op in (CSR_RS, CSR_RC)) or (csr_op == CSR_RW and rd != 0)
                writes = (csr_op == CSR_RW) or (csr_op in (CSR_RS, CSR_RC) and operand != 0)
                if not self.csrs.is_legal(csr_addr):
                    has_trap, trap_cause, trap_tval = True, EXC_ILLEGAL_INSN, ir
                elif writes and not self.csrs.is_writable(csr_addr):
                    has_trap, trap_cause, trap_tval = True, EXC_ILLEGAL_INSN, ir
                elif csr_op == CSR_RW:
                    rd_value = self.csr_read(csr_addr) if reads else 0
                    self.csr_write(csr_addr, operand)
                    has_rd = reads
                elif csr_op == CSR_RS:
                    rd_value = self.csr_read(csr_addr)
                    self.csr_write(csr_addr, rd_value | operand)
                    has_rd = reads
                else:
                    rd_value = self.csr_read(csr_addr)
                    self.csr_write(csr_addr, rd_value & ~operand)
                    has_rd = reads
        else:
            illegal = True

        # Trap priority, in the order the Privileged Specification resolves them.
        if illegal:
            has_trap, trap_cause, trap_tval = True, EXC_ILLEGAL_INSN, ir
        elif is_ecall:
            has_trap, trap_cause, trap_tval = True, EXC_ECALL_M, 0
        elif is_ebreak:
            has_trap, trap_cause, trap_tval = True, EXC_BREAKPOINT, pc
        elif mem_kind == 1 and (mem_addr % mem_size) != 0:
            has_trap, trap_cause, trap_tval = True, EXC_LOAD_MISALIGNED, mem_addr
        elif mem_kind == 2 and (mem_addr % mem_size) != 0:
            has_trap, trap_cause, trap_tval = True, EXC_STORE_MISALIGNED, mem_addr

        if has_trap:
            self.take_trap(seq, trap_cause, trap_tval, ir)
            return

        if mem_kind == 1:
            if not self.mem.access_ok(mem_addr, mem_size, False, False):
                self.take_trap(seq, EXC_LOAD_ACCESS, mem_addr, ir)
                return
            raw = self.mem.read(mem_addr, mem_size)
            if mem_size == 1:
                rd_value = sext(raw, 8) if mem_signed else raw
            elif mem_size == 2:
                rd_value = sext(raw, 16) if mem_signed else raw
            elif mem_size == 4:
                rd_value = sext(raw, 32) if mem_signed else raw
            else:
                rd_value = raw
            self.cycles += 5
        elif mem_kind == 2:
            if not self.mem.access_ok(mem_addr, mem_size, True, False):
                self.take_trap(seq, EXC_STORE_ACCESS, mem_addr, ir)
                return
            self.mem.write(mem_addr, mem_size, store_value)
            self.cycles += 5
        else:
            self.cycles += 3

        if not is_mret:
            # A load's or store's address add goes through the ALU too, but its
            # result is the address, not the value written to rd.
            if uses_alu and mem_kind == 0:
                rd_value = alu_eval(alu_a, alu_b, alu_op)
            if is_branch and branch_taken:
                next_pc = u64(pc + imm)
            if opcode == OP_JAL or opcode == OP_JALR:
                rd_value = u64(pc + 4)

        if has_rd:
            self.regs[rd] = u64(rd_value)
        self.minstret += 1
        self.emit(seq, pc, next_pc, ir, has_rd, rd, u64(rd_value), False,
                  0, 0, 0, is_store, mem_addr, store_value, mem_size)
        self.pc = next_pc


def s64(v):
    v &= M64
    return v - (1 << 64) if (v >> 63) else v


def main(argv):
    args = {}
    i = 1
    while i + 1 < len(argv):
        args[argv[i].lstrip("-")] = argv[i + 1]
        i += 2

    repo = args["repo"]
    profile = json.load(open(os.path.join(repo, "config", "profiles", "p0.json")))
    memcfg = json.load(open(os.path.join(repo, "config", "memory", "p0.json")))
    csrcfg = json.load(open(os.path.join(repo, "config", "csr", "mode_m.json")))

    protocol = profile["test_protocol"]
    reset_vector = profile["reset"]["reset_vector"]
    tohost_addr = protocol["tohost"]
    sig_addr = protocol["signature"]
    sig_words = protocol["signature_words"]

    table = {}
    for mode in csrcfg["modes"]:
        for entry in mode["csrs"]:
            table[entry["address"]] = entry
    csrs = CsrFile(table)
    mem = Memory(memcfg["regions"], tohost_addr)

    for line in open(args["image"]):
        parts = line.split()
        if len(parts) < 3 or parts[0] != "seg":
            continue
        vaddr = int(parts[1], 16)
        memsz = int(parts[2], 16)
        data = bytes.fromhex(parts[3]) if len(parts) > 3 else b""
        for k, byte in enumerate(data):
            mem.bytes[vaddr + k] = byte
        for k in range(len(data), memsz):
            mem.bytes[vaddr + k] = 0

    machine = Machine(mem, csrs, reset_vector, tohost_addr)

    limit = int(args["max-steps"])
    seq = 0
    outcome = "step_limit"
    while seq < limit:
        machine.step(seq)
        seq += 1
        if mem.tohost != 0:
            outcome = "tohost"
            break
        if not mem.access_ok(machine.pc, 4, False, True):
            outcome = "pc_escaped"
            break

    with open(args["events"], "w") as handle:
        for line in machine.lines:
            handle.write(line + "\n")

    with open(args["summary"], "w") as handle:
        handle.write("outcome %s\n" % outcome)
        handle.write("events %d\n" % len(machine.lines))
        handle.write("steps %d\n" % seq)
        handle.write("cycles %d\n" % machine.cycles)
        handle.write("minstret %d\n" % machine.minstret)
        handle.write("tohost %016x\n" % mem.tohost)
        handle.write("uart_writes %d\n" % mem.uart_writes)
        for w in range(sig_words):
            handle.write("sig%d %016x\n" % (w, mem.read(sig_addr + 8 * w, 8)))


main(sys.argv)
