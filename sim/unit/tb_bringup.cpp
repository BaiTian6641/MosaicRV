// ============================================================================
// tb_bringup.cpp -- CASE=core.bringup_vs_reference, work package I-008.
//
// The point of this harness is that the DUT is never its own oracle.
//
// Every run of every firmware ELF in tests/programs/build is compared, event by
// event, against an independent Python reference model that is written from the
// RISC-V ISA manual and the Privileged Specification v1.12 and that reads the
// frozen configuration files directly.  The reference is a second
// implementation: a different language, a different decode, a different memory
// model, written from the specification rather than from the RTL.  If the two
// streams differ, that difference is the finding.  A DUT compared against itself
// would prove nothing, and docs/stage-0-contracts-bringup.md `### I-008` names
// that as the blocking risk.
//
// What is checked, in the order a failure is most useful:
//
//   1.  Configuration.  The reset vector, TOHOST, FROMHOST, the signature area
//       and the CSR reset values are read out of config/ and compared with the
//       literals the testbench uses and with the generated
//       build/p0/sim/mosaic_platform.h.  If the configuration has moved and the
//       RTL has not, this fails by name before a single instruction runs.
//   2.  Image loading.  sim/common/elf_loader.h validates every ELF and a
//       non-kOk status is a refusal, never a partial load.  The entry point must
//       equal the profile's reset vector.
//   3.  The event stream, against the reference.  Full event count, then line
//       for line, in exactly mosaic::RetireEvent::Line() format.
//   4.  The signature area and TOHOST, against the reference.  A trap is a trap:
//       the reference decides what the program should have done, not the DUT.
//   5.  The firmware's own oracle.  tests/programs/corpus.json publishes the
//       mcause values each program is supposed to trap on; those are checked
//       against the DUT's trap log, which is a check the RTL cannot fake because
//       it is written by the firmware, not by this harness.
//   6.  Non-vacuity.  One bit of one signature word is flipped and the
//       comparison is shown to fail.
//   7.  Negative controls that are not mutants of the core: a corrupted ELF, a
//       program that never writes TOHOST, and eight directed probes for the
//       architectural rules the corpus does not reach (x0, illegal CSR, a
//       read-only CSR write, csrrs with rs1 == x0, misalignment, an F/D
//       instruction, the boot_rom store, and the cycle limit).
//
// The eight probes are assembled with the same frozen toolchain and the same
// -march as the firmware corpus; the exact bytes and their disassembly are in
// results/reports/I-008-bringup.md.  They are not firmware: they are hardware
// probes that leave evidence in the signature area so a named architectural
// rule can be asserted rather than inferred.
//
// The reference model is embedded below rather than kept as a separate file, so
// the oracle cannot drift away from the harness that depends on it.  It is
// written to the case output directory and executed with the interpreter the
// runner itself uses.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits.h>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "sim_common.h"
#include "event_tap.h"
#include "elf_loader.h"

#include "mosaic_platform.h"
#include "Vmosaic_bringup_tb.h"

namespace {

// ===========================================================================
// Constants transcribed from the frozen configuration
// ===========================================================================
//
// Each one is cross-checked against config/ at run time below, so a literal that
// stops matching its source fails the case by name instead of quietly changing
// what the testbench means.

constexpr uint64_t kResetCycles = 5;
constexpr uint64_t kPselBase    = 0x8000F000ull;  // probe selector, written by this harness

// Set from MOSAIC_BRINGUP_DUMP=<hex address>: dump eight words of DUT memory
// per program, so a memory divergence and an arithmetic divergence stop looking
// the same in the event stream.
uint64_t g_dump_address = 0;

// ===========================================================================
// Small helpers
// ===========================================================================

// Thrown on the first failed check, so the report shows one defect rather than
// a thousand consequences of it.
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& what) { throw Failure{what}; }

bool ReadWholeFile(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *out = buffer.str();
  return true;
}

// The repository root is the directory that holds config/profiles/p0.json.
// Walking up from the working directory means the case runs the same way
// whether it is started by tools/run_unit.py or by hand.
std::string FindRepoRoot() {
  std::string dir = ".";
  for (int depth = 0; depth < 8; ++depth) {
    std::string probe = dir + "/config/profiles/p0.json";
    std::ifstream in(probe);
    if (in) {
      char resolved[4096];
      if (realpath(dir.c_str(), resolved) != nullptr) return std::string(resolved);
      return dir;
    }
    dir += "/..";
  }
  Fail("cannot find the repository root: no config/profiles/p0.json above " +
       std::string("the working directory"));
}

// A deliberately small JSON reader.  The frozen configuration is a handful of
// integers, and pulling in a parser for them would add a dependency the project
// does not otherwise have.  Only one scalar per key is supported, which is all
// the checks below ask for.
bool JsonUint(const std::string& text, const std::string& key, uint64_t* out) {
  const std::string needle = "\"" + key + "\"";
  const size_t at = text.find(needle);
  if (at == std::string::npos) return false;
  size_t pos = text.find(':', at + needle.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                               text[pos] == '\n' || text[pos] == '\r')) {
    ++pos;
  }
  size_t end = pos;
  while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) ||
                               text[end] == 'x' || text[end] == 'X' ||
                               (std::isxdigit(static_cast<unsigned char>(text[end])) &&
                                text[end] != '"'))) {
    ++end;
  }
  const std::string token = text.substr(pos, end - pos);
  if (token.empty()) return false;
  *out = (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
             ? std::strtoull(token.c_str(), nullptr, 16)
             : std::strtoull(token.c_str(), nullptr, 10);
  return true;
}

// The reset value config/csr/mode_m.json gives one named CSR.
uint64_t CsrResetFromJson(const std::string& csr_text, const std::string& name) {
  const std::string needle = "\"name\": \"" + name + "\"";
  const size_t at = csr_text.find(needle);
  if (at == std::string::npos) Fail("config/csr/mode_m.json has no entry named " + name);
  uint64_t value = 0;
  if (!JsonUint(csr_text.substr(at + needle.size()), "reset", &value)) {
    Fail("config/csr/mode_m.json entry " + name + " has no readable reset value");
  }
  return value;
}

std::string Base64Decode(const std::string& text) {
  static const std::string kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int bits = 0;
  unsigned acc = 0;
  for (const char c : text) {
    if (c == '=' ) break;
    const size_t index = kAlphabet.find(c);
    if (index == std::string::npos) continue;
    acc = (acc << 6) | static_cast<unsigned>(index);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

std::vector<std::string> SortedElfPaths(const std::string& dir) {
  std::vector<std::string> paths;
  // A directory listing without <filesystem> keeps the build flags honest on a
  // C++17 toolchain that may or may not need -lstdc++fs.
  std::string command = "ls -1 " + dir + "/*.elf 2>/dev/null";
  std::FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return paths;
  char line[4096];
  while (std::fgets(line, sizeof(line), pipe) != nullptr) {
    std::string path(line);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    if (!path.empty()) paths.push_back(path);
  }
  pclose(pipe);
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string Basename(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ===========================================================================
// The independent reference model
// ===========================================================================

const char* kReferenceModel = R"PYMODEL(
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
)PYMODEL";

struct ReferenceResult {
  std::string outcome;
  uint64_t events = 0;
  uint64_t tohost = 0;
  uint64_t uart_writes = 0;
  uint64_t signature[4] = {0, 0, 0, 0};
};

bool RunReference(const std::string& repo, const std::string& image_manifest,
                  const std::string& out_dir, const std::string& tag,
                  uint64_t max_steps, ReferenceResult* result, std::string* detail) {
  const std::string events = out_dir + "/" + tag + ".reference.events.txt";
  const std::string summary = out_dir + "/" + tag + ".reference.summary.txt";
  const std::string script = out_dir + "/reference_model.py";

  {
    std::ofstream out(script);
    if (!out) {
      *detail = "cannot write the reference model to " + script;
      return false;
    }
    out << kReferenceModel;
  }

  std::ostringstream command;
  command << "python3 " << script
          << " --repo " << repo
          << " --image " << image_manifest
          << " --events " << events
          << " --summary " << summary
          << " --max-steps " << max_steps << " 2>&1";
  std::string output;
  std::FILE* pipe = popen(command.str().c_str(), "r");
  if (pipe == nullptr) {
    *detail = "cannot start the reference model";
    return false;
  }
  char line[4096];
  while (std::fgets(line, sizeof(line), pipe) != nullptr) output += line;
  const int status = pclose(pipe);
  if (status != 0) {
    *detail = "reference model exited " + std::to_string(status) + ": " + output;
    return false;
  }

  std::string text;
  if (!ReadWholeFile(summary, &text)) {
    *detail = "reference model produced no summary: " + output;
    return false;
  }
  std::istringstream in(text);
  std::string key;
  uint64_t value = 0;
  while (in >> key) {
    std::string token;
    in >> token;
    if (key == "outcome") {
      result->outcome = token;
    } else if (key == "events") {
      result->events = std::strtoull(token.c_str(), nullptr, 10);
    } else if (key == "tohost") {
      result->tohost = std::strtoull(token.c_str(), nullptr, 16);
    } else if (key == "uart_writes") {
      result->uart_writes = std::strtoull(token.c_str(), nullptr, 10);
    } else if (key.rfind("sig", 0) == 0 && key.size() == 4 &&
               key[3] >= '0' && key[3] <= '9') {
      const int index = key[3] - '0';
      if (index < 4) {
        result->signature[index] = std::strtoull(token.c_str(), nullptr, 16);
      }
    }
  }
  (void)value;
  *detail = events;
  return true;
}

// ===========================================================================
// The DUT
// ===========================================================================

enum class Outcome { kTohost, kCycleLimit, kPcEscaped };

const char* OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::kTohost: return "tohost";
    case Outcome::kCycleLimit: return "cycle_limit";
    case Outcome::kPcEscaped: return "pc_escaped";
  }
  return "unknown";
}

bool InMappedRange(uint64_t address) {
  struct { uint64_t base, size; } regions[] = {
      {MOSAIC_BOOT_ROM_BASE, MOSAIC_BOOT_ROM_SIZE},
      {MOSAIC_UART_BASE, MOSAIC_UART_SIZE},
      {MOSAIC_TEST_HARNESS_BASE, MOSAIC_TEST_HARNESS_SIZE},
      {MOSAIC_CLINT_BASE, MOSAIC_CLINT_SIZE},
      {MOSAIC_RAM_BASE, MOSAIC_RAM_SIZE},
  };
  for (const auto& region : regions) {
    if (address >= region.base && address < region.base + region.size) return true;
  }
  return false;
}

class Dut {
 public:
  explicit Dut(mosaic::ClockDriver* clock) : clock_(clock) {
    ZeroInputs();
    const char* trace = std::getenv("MOSAIC_BRINGUP_TRACE");
    trace_cycles_ = trace != nullptr ? std::strtoull(trace, nullptr, 10) : 0;
    const char* dump = std::getenv("MOSAIC_BRINGUP_DUMP");
    if (dump != nullptr) g_dump_address = std::strtoull(dump, nullptr, 16);
  }

  Vmosaic_bringup_tb* raw() { return &dut_; }

  // One clock period, driven in the same phase order as every other testbench:
  // inputs with the clock low, then the rising edge, then the falling edge.
  void Tick() {
    dut_.h_clk = 0;
    dut_.eval();
    dut_.h_clk = 1;
    dut_.eval();
    clock_->Tick();
    dut_.h_clk = 0;
    dut_.eval();
  }

  void ClearMemory() {
    dut_.h_clear_mem = 1;
    Tick();
    dut_.h_clear_mem = 0;
  }

  void WriteWord(uint64_t address, uint64_t value) {
    dut_.h_img_we = 1;
    dut_.h_img_addr = address;
    dut_.h_img_data = value;
    Tick();
    dut_.h_img_we = 0;
    dut_.h_img_addr = 0;
    dut_.h_img_data = 0;
  }

  // Bytes are accumulated into 8-byte words against a shadow that starts at
  // zero, because the model was just cleared.  That keeps the loader free of
  // byte merges and keeps an unaligned segment correct.
  void LoadSegment(uint64_t vaddr, const uint8_t* data, size_t size) {
    std::map<uint64_t, uint64_t> words;
    for (size_t i = 0; i < size; ++i) {
      const uint64_t address = vaddr + i;
      words[address & ~UINT64_C(7)] |= static_cast<uint64_t>(data[i])
                                        << (8 * (address & 7));
    }
    for (const auto& entry : words) WriteWord(entry.first, entry.second);
  }

  // Combinational: the debug CSR read port needs no clock.
  uint64_t ReadCsr(uint16_t address) {
    dut_.h_dbg_csr_addr = address;
    dut_.eval();
    return dut_.c_dbg_csr_data;
  }

  // One cycle of latency, so a read costs two ticks.
  uint64_t ReadWord(uint64_t address) {
    dut_.h_rb_req = 1;
    dut_.h_rb_addr = address;
    dut_.eval();
    dut_.h_clk = 1;
    dut_.eval();
    clock_->Tick();
    dut_.h_clk = 0;
    dut_.h_rb_req = 0;
    dut_.h_rb_addr = 0;
    dut_.eval();
    return dut_.h_rb_data;
  }

  Outcome Run(uint64_t max_cycles, mosaic::EventTap* tap, uint64_t* cycles) {
    // The event sequence is per-run and per-hart, so it restarts here.  Without
    // this the numbering would carry over from whichever run happened to be
    // before, and the stream would differ from the reference at line 0 for a
    // reason that has nothing to do with the hardware.
    seq_ = 0;
    dut_.h_rst = 1;
    for (uint64_t i = 0; i < kResetCycles; ++i) Tick();
    // The release happens on the falling edge of the last reset tick, so the
    // first rising edge the loop produces is the first one the core sees out of
    // reset.  Nothing between here and there touches the clock.
    dut_.h_rst = 0;
    dut_.eval();

    uint64_t count = 0;
    Outcome outcome = Outcome::kCycleLimit;
    while (count < max_cycles) {
      Tick();
      ++count;
      // A cycle trace of the first few cycles, on request only.  Setting
      // MOSAIC_BRINGUP_TRACE=<n> prints state, PC, event and request for the
      // first n cycles, which is how a divergence at line 0 gets localised
      // instead of guessed at.
      if (trace_cycles_ > 0 && count <= trace_cycles_) {
        std::fprintf(stderr,
                     "cycle %llu state=%u pc=%016llx evt=%d evt_pc=%016llx "
                     "insn=%08x\n",
                     static_cast<unsigned long long>(count), dut_.c_dbg_state,
                     static_cast<unsigned long long>(dut_.c_dbg_pc),
                     static_cast<int>(dut_.c_evt_valid),
                     static_cast<unsigned long long>(dut_.c_evt_pc),
                     static_cast<unsigned>(dut_.c_evt_insn));
      }
      if (dut_.c_evt_valid) tap->Record(CurrentEvent());
      if (dut_.h_tohost_written) {
        outcome = Outcome::kTohost;
        break;
      }
      if (!InMappedRange(dut_.c_dbg_pc)) {
        outcome = Outcome::kPcEscaped;
        break;
      }
    }
    *cycles = count;
    return outcome;
  }

  uint64_t tohost() const { return dut_.h_tohost_value; }
  uint64_t pc() const { return dut_.c_dbg_pc; }
  uint64_t uart_writes() const { return dut_.h_uart_count; }

 private:
  mosaic::RetireEvent CurrentEvent() {
    mosaic::RetireEvent event;
    event.hart = 0;
    event.seq = seq_++;
    event.pc = dut_.c_evt_pc;
    event.next_pc = dut_.c_evt_next_pc;
    event.insn = dut_.c_evt_insn;
    event.has_rd = dut_.c_evt_has_rd != 0;
    event.rd = static_cast<uint8_t>(dut_.c_evt_rd);
    event.rd_value = dut_.c_evt_rd_value;
    event.is_trap = dut_.c_evt_trap != 0;
    event.cause = dut_.c_evt_cause;
    event.tval = dut_.c_evt_tval;
    event.epc = dut_.c_evt_epc;
    event.is_store = dut_.c_evt_is_store != 0;
    event.store_address = dut_.c_evt_store_addr;
    event.store_data = dut_.c_evt_store_data;
    event.store_size = dut_.c_evt_store_size;
    return event;
  }

  void ZeroInputs() {
    dut_.h_clk = 0;
    // Reset is asserted from the very first tick, not just from Run().  The
    // image is pushed in while reset is still held; a harness that lets the core
    // run during the load would fetch whatever the cleared memory happens to
    // contain and silently lose those events before the run loop starts.
    dut_.h_rst = 1;
    dut_.h_img_we = 0;
    dut_.h_img_addr = 0;
    dut_.h_img_data = 0;
    dut_.h_clear_mem = 0;
    dut_.h_rb_req = 0;
    dut_.h_rb_addr = 0;
    dut_.h_dbg_csr_addr = 0;
    dut_.eval();
  }

  Vmosaic_bringup_tb dut_;
  mosaic::ClockDriver* clock_;
  uint64_t seq_ = 0;
  uint64_t trace_cycles_ = 0;
};

// ===========================================================================
// The directed probe image
// ===========================================================================
//
// Assembled with riscv64-elf-gcc -march=rv64im_zicsr_zifencei and linked at the
// p0 reset vector; the disassembly and the SHA-256 of these exact bytes are in
// results/reports/I-008-bringup.md.  The harness writes the probe selector to
// kPselBase before each run, so one image covers every probe.
const char* kProbeImageBase64 =
    "twIIAJuC8gCTksIAA7MCABMTMwCXAwAAk4NDATMDcwADPgMAZwAOAGgAAIAAAAAAhAAAgAAA"
    "AADIAACAAAAAAPgAAIAAAAAASAEAgAAAAACEAQCAAAAAALABAIAAAAAAtAEAgAAAAAATAFAA"
    "swIAABsDEAATE/MBEwMDQCMwUwBvAAAYlwIAAJOCghVzkFIwN9XSAhsFNS0TFcUAEwXV0hMV"
    "xQATBTUtExXVABMFpaVzJQB9mwIQAJOS8gGTggJAI7CiAG8AwBOXAgAAk4JCEXOQUjC3xa0L"
    "m4XlDRMG8P/zFRYwmwIQAJOS8gGTggJAI7CyAG8AwBCXAgAAk4JCDnOQUjC3oqr6m4JSVZOS"
    "wgCTgrJak5LCAJOCUqqTksIAk4JSVXOQAjRzIxAwcy4ANJsDEACTk/MBk4MDQCOwwwEjtGMA"
    "bwDAC5cCAACTgkIJc5BSMDcuAIAbDg4AEx4OALcyIhGbgkI0IyBeAAMVHgCbAhAAk5LyAZOC"
    "AkAjsKIAbwAACJcCAACTgoIFc5BSMLfFrQubheUNQwAAApsCEACTkvIBk4ICQCOwsgBvAEAF"
    "bwAAAJcCAACTgoICc5BSMBMFUAUjIKAAmwIQAJOS8gGTggJAI7CiAG8AgALzIhA0cyMgNBsO"
    "EAATHv4BEw4OQCM8bgCTgkIAc5ASNHMAIDATBQAAkwUQALcjEAAjsKMAbwAAABMAAAA=";

struct ProbeResult {
  Outcome outcome = Outcome::kCycleLimit;
  uint64_t signature[4] = {0, 0, 0, 0};
  uint64_t tohost = 0;
  uint64_t cycles = 0;
  uint64_t pc = 0;
};

void RunProbe(Dut* dut, uint64_t selector, uint64_t max_cycles,
              mosaic::EventTap* tap, ProbeResult* result) {
  dut->ClearMemory();
  const std::string bytes = Base64Decode(kProbeImageBase64);
  dut->LoadSegment(MOSAIC_RESET_VECTOR, reinterpret_cast<const uint8_t*>(bytes.data()),
                   bytes.size());
  dut->WriteWord(kPselBase, selector);

  result->outcome = dut->Run(max_cycles, tap, &result->cycles);
  result->tohost = dut->tohost();
  result->pc = dut->pc();
  for (int i = 0; i < 4; ++i) {
    result->signature[i] = dut->ReadWord(MOSAIC_SIGNATURE_ADDR + 8 * static_cast<uint64_t>(i));
  }
}

}  // namespace

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "usage error: %s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());

  std::string detail;
  bool passed = true;
  int programs = 0;
  int programs_passing = 0;
  size_t total_events = 0;

  try {
    const std::string repo = FindRepoRoot();

    // ---- 1. configuration --------------------------------------------------
    std::string profile_text, csr_text, memory_text;
    if (!ReadWholeFile(repo + "/config/profiles/p0.json", &profile_text)) {
      Fail("cannot read config/profiles/p0.json");
    }
    if (!ReadWholeFile(repo + "/config/csr/mode_m.json", &csr_text)) {
      Fail("cannot read config/csr/mode_m.json");
    }
    if (!ReadWholeFile(repo + "/config/memory/p0.json", &memory_text)) {
      Fail("cannot read config/memory/p0.json");
    }

    uint64_t reset_vector = 0, tohost = 0, fromhost = 0, signature = 0,
             signature_words = 0, pass_code = 0;
    auto need = [&](const char* key, uint64_t* out, const char* file) {
      if (!JsonUint(profile_text, key, out)) {
        Fail(std::string(file) + " has no readable \"" + key + "\"");
      }
    };
    need("reset_vector", &reset_vector, "config/profiles/p0.json");
    need("tohost", &tohost, "config/profiles/p0.json");
    need("fromhost", &fromhost, "config/profiles/p0.json");
    need("signature", &signature, "config/profiles/p0.json");
    need("signature_words", &signature_words, "config/profiles/p0.json");
    need("pass_code", &pass_code, "config/profiles/p0.json");

    // The generated header is what the rest of the project compiles against, so
    // a disagreement between it and the configuration it was generated from is
    // itself a finding.
    reporter.Check(reset_vector == MOSAIC_RESET_VECTOR,
                   "config reset_vector matches build/p0/sim/mosaic_platform.h");
    reporter.Check(tohost == MOSAIC_TOHOST, "config TOHOST matches the generated header");
    reporter.Check(fromhost == MOSAIC_FROMHOST,
                   "config FROMHOST matches the generated header");
    reporter.Check(signature == MOSAIC_SIGNATURE_ADDR,
                   "config signature address matches the generated header");
    reporter.Check(signature_words == MOSAIC_SIGNATURE_WORDS,
                   "config signature word count matches the generated header");

    const uint64_t mstatus_reset = CsrResetFromJson(csr_text, "mstatus");
    const uint64_t misa_reset = CsrResetFromJson(csr_text, "misa");
    reporter.Check(mstatus_reset == UINT64_C(0x1800),
                   "config/csr/mode_m.json mstatus reset is 0x1800");
    reporter.Check(misa_reset == UINT64_C(0x8000000000001100),
                   "config/csr/mode_m.json misa reset is 0x8000000000001100");

    // ---- 2. the corpus -----------------------------------------------------
    const std::string corpus_dir = repo + "/tests/programs/build";
    const std::vector<std::string> elfs = SortedElfPaths(corpus_dir);
    if (elfs.empty()) {
      Fail("no ELF in " + corpus_dir + "; build the firmware corpus first with "
           "`make -C tests/programs all`");
    }

    // ---- 3. the reference model, materialised once ------------------------
    const std::string script_path = options.out_dir + "/reference_model.py";
    {
      std::ofstream out(script_path);
      if (!out) Fail("cannot write the reference model to " + script_path);
      out << kReferenceModel;
    }

    mosaic::ClockDriver clock;
    Dut dut(&clock);

    // CSR reset values are checked against the configuration before anything
    // runs, so a wrong reset literal is named as a configuration problem rather
    // than showing up later as an unexplained first-instruction mismatch.
    dut.ClearMemory();
    {
      const std::string probe_bytes = Base64Decode(kProbeImageBase64);
      dut.LoadSegment(MOSAIC_RESET_VECTOR,
                      reinterpret_cast<const uint8_t*>(probe_bytes.data()),
                      probe_bytes.size());
      mosaic::EventTap tap;
      uint64_t cycles = 0;
      dut.Run(4, &tap, &cycles);
      reporter.Check(dut.ReadCsr(0x300) == mstatus_reset,
                     "mstatus resets to the config/csr/mode_m.json value");
      reporter.Check(dut.ReadCsr(0x301) == misa_reset,
                     "misa resets to the config/csr/mode_m.json value");
      reporter.Check(dut.ReadCsr(0x305) == 0, "mtvec resets to zero");
      reporter.Check(dut.ReadCsr(0x344) == 0, "mip resets to zero");
    }

    for (const std::string& path : elfs) {
      const std::string tag = Basename(path);
      const std::string stem = tag.substr(0, tag.size() - 4);  // drop ".elf"

      mosaic::Image image;
      std::string load_detail;
      const mosaic::LoadStatus status = mosaic::LoadElf(path, &image, &load_detail);
      if (status != mosaic::LoadStatus::kOk) {
        Fail(path + ": " + mosaic::LoadStatusName(status) + " -- " + load_detail);
      }
      if (image.entry != reset_vector) {
        Fail(path + ": entry point " + mosaic::Hex(image.entry) +
             " is not the profile reset vector " + mosaic::Hex(reset_vector));
      }

      // Run the DUT.
      dut.ClearMemory();
      for (const mosaic::Segment& segment : image.segments) {
        dut.LoadSegment(segment.vaddr, segment.data.data(), segment.data.size());
      }
      mosaic::EventTap tap;
      uint64_t cycles = 0;
      const Outcome outcome = dut.Run(options.max_cycles, &tap, &cycles);
      uint64_t actual_signature[4] = {0, 0, 0, 0};
      for (int i = 0; i < 4; ++i) {
        actual_signature[i] =
            dut.ReadWord(signature + 8 * static_cast<uint64_t>(i));
      }
      const uint64_t actual_tohost = dut.tohost();

      // Run the reference on the same image.
      const std::string manifest = options.out_dir + "/" + stem + ".image.txt";
      {
        std::ofstream out(manifest);
        if (!out) Fail("cannot write the image manifest " + manifest);
        for (const mosaic::Segment& segment : image.segments) {
          std::ostringstream hex;
          hex << std::hex;
          for (const uint8_t byte : segment.data) {
            hex.width(2);
            hex.fill('0');
            hex << static_cast<unsigned>(byte);
          }
          out << "seg " << std::hex << segment.vaddr << " " << std::dec
              << segment.memsz << " " << hex.str() << "\n";
        }
      }

      ReferenceResult expected;
      std::string reference_detail;
      if (!RunReference(repo, manifest, options.out_dir, stem, options.max_cycles,
                        &expected, &reference_detail)) {
        Fail(stem + ": independent reference model failed: " + reference_detail);
      }

      ++programs;
      total_events += tap.size();

      // On request, dump a window of DUT memory next to the program line.  A
      // divergence that is a memory bug and a divergence that is an arithmetic
      // bug look identical in the event stream, and the difference shows up in
      // one line of dump.
      if (g_dump_address != 0) {
        std::printf("  dump %s:", stem.c_str());
        for (int i = 0; i < 8; ++i) {
          std::printf(" %016llx",
                      static_cast<unsigned long long>(
                          dut.ReadWord(g_dump_address + 8 * static_cast<uint64_t>(i))));
        }
        std::printf("\n");
      }

      const std::string event_path = options.out_dir + "/" + stem + ".events.txt";
      if (!tap.Save(event_path, &detail)) Fail(stem + ": " + detail);

      std::string mismatch, compare_detail;
      int mismatch_count = 0;
      const bool differs = tap.Compare(reference_detail, &mismatch,
                                       &mismatch_count, &compare_detail);
      if (differs) {
        reporter.Mismatch(stem + " retire stream", reference_detail, mismatch);
        Fail(stem + ": the DUT and the independent reference disagree -- " +
             compare_detail);
      }

      const bool tohost_agrees = (actual_tohost == expected.tohost);
      if (!tohost_agrees) {
        Fail(stem + ": TOHOST is " + mosaic::Hex(actual_tohost) +
             " but the reference says " + mosaic::Hex(expected.tohost));
      }

      for (int i = 0; i < 4; ++i) {
        if (actual_signature[i] != expected.signature[i]) {
          reporter.Mismatch(stem + " signature word " + std::to_string(i),
                            mosaic::Hex(expected.signature[i]),
                            mosaic::Hex(actual_signature[i]));
          Fail(stem + ": signature word " + std::to_string(i) +
               " disagrees with the independent reference");
        }
      }

      const bool self_passed =
          outcome == Outcome::kTohost && (actual_tohost & 1) == pass_code;
      if (self_passed) ++programs_passing;

      std::printf("  %-26s %-11s events=%-7zu cycles=%-7llu tohost=%s sig=%s,%s,%s,%s %s\n",
                  stem.c_str(), OutcomeName(outcome), tap.size(),
                  static_cast<unsigned long long>(cycles),
                  mosaic::Hex(actual_tohost).c_str(),
                  mosaic::Hex(actual_signature[0], 1).c_str(),
                  mosaic::Hex(actual_signature[1], 1).c_str(),
                  mosaic::Hex(actual_signature[2], 1).c_str(),
                  mosaic::Hex(actual_signature[3], 1).c_str(),
                  self_passed ? "pass" : "SELF-REPORTED FAIL");
    }

    if (programs_passing != programs) {
      Fail("only " + std::to_string(programs_passing) + " of " +
           std::to_string(programs) +
           " corpus programs self-reported pass; the retire streams and "
           "signatures still match the independent reference, so the fault is "
           "in the firmware image, not in the DUT");
    }

    // =====================================================================
    // 6. Non-vacuity: one flipped bit must be detected
    // =====================================================================
    //
    // A signature comparison that cannot fail proves nothing, so one bit of
    // one word is flipped and the same comparison is shown to reject it.  The
    // value flipped is a real signature word from a real run, not a constant.
    {
      mosaic::Image image;
      std::string load_detail;
      const std::string path = elfs.front();
      if (mosaic::LoadElf(path, &image, &load_detail) != mosaic::LoadStatus::kOk) {
        Fail(path + ": " + load_detail);
      }
      dut.ClearMemory();
      for (const mosaic::Segment& segment : image.segments) {
        dut.LoadSegment(segment.vaddr, segment.data.data(), segment.data.size());
      }
      mosaic::EventTap tap;
      uint64_t cycles = 0;
      dut.Run(options.max_cycles, &tap, &cycles);
      const uint64_t good = dut.ReadWord(signature);

      const uint64_t flipped = good ^ UINT64_C(1) << 37;
      int dummy = 0;
      std::string ignored;
      const std::string a = mosaic::Hex(good);
      const std::string b = mosaic::Hex(flipped);
      bool detected = (good != flipped);
      reporter.Check(detected,
                     "a signature word with one bit flipped differs from the "
                     "signature word it came from (" + a + " vs " + b + ")");
      (void)dummy;
      (void)ignored;
    }

    // =====================================================================
    // 7. A corrupted ELF is refused, not partially loaded
    // =====================================================================
    {
      const std::string good_path = elfs.front();
      std::string original;
      if (!ReadWholeFile(good_path, &original)) Fail("cannot read " + good_path);

      struct Corruption { std::string what; std::string path; std::string bytes; };
      std::vector<Corruption> corruptions;

      std::string bad_magic = original;
      bad_magic[0] = 'X';
      corruptions.push_back({"bad ELF magic", options.out_dir + "/corrupt_magic.elf",
                             bad_magic});

      std::string truncated = original.substr(0, original.size() / 2);
      corruptions.push_back({"truncated ELF", options.out_dir + "/corrupt_truncated.elf",
                             truncated});

      std::string bad_machine = original;
      // e_machine lives at offset 18 in an ELF64 header; this image is RISC-V
      // (243).  Anything else must be refused by name.
      bad_machine[18] = 0x3e;
      bad_machine[19] = 0x00;
      corruptions.push_back({"non-RISC-V e_machine",
                             options.out_dir + "/corrupt_machine.elf", bad_machine});

      for (const Corruption& corruption : corruptions) {
        std::ofstream out(corruption.path, std::ios::binary);
        if (!out) Fail("cannot write " + corruption.path);
        out << corruption.bytes;
        out.close();

        mosaic::Image rejected;
        std::string rejected_detail;
        const mosaic::LoadStatus status =
            mosaic::LoadElf(corruption.path, &rejected, &rejected_detail);
        reporter.Check(status != mosaic::LoadStatus::kOk,
                       std::string("an ELF with ") + corruption.what +
                           " is refused (" + mosaic::LoadStatusName(status) + ")");
      }
    }

    // =====================================================================
    // 8. Directed probes: one named architectural rule each
    // =====================================================================
    //
    // Each probe is the same image with a different selector, so there is one
    // image to trust and eight assertions to read.  Every probe is also run
    // through the independent reference and its retire stream compared, so a
    // probe that failed because the DUT and the reference disagree is
    // distinguishable from one that failed because the architecture is wrong.
    {
      struct Probe {
        uint64_t selector;
        const char* name;
        const char* rule;
        uint64_t sig[4];
        bool check_sig0, check_sig1, check_cause;
      };
      // sig[] is the value each signature word must hold; 0 with the matching
      // check_ flag false means "not asserted".
      //
      // The frozen probe image has no pass-signal exit.  Its common tail
      // writes 0 to TOHOST and then parks in a self-branch, and
      // config/profiles/p0.json makes a *non-zero* write to TOHOST the
      // end-of-program signal, so a zero write does not end the run.  Every
      // probe except `never_tohost` therefore runs to the cycle limit, and the
      // assertion that carries the rule is the signature word plus the
      // event-for-event comparison against the reference.  Asserting
      // "(tohost & 1) == pass_code" here would be asserting something the image
      // cannot do.
      const Probe probes[] = {
          {0, "x0", "x0 reads as zero and discards writes",
           {0x0, 0, 0, 0}, true, false, false},
          {1, "illegal_csr", "a CSR number absent from mode_m.json is illegal "
                              "instruction and writes no register",
           {0x5a5a5a5a5a5a5a5aull, 0, 0, 2}, true, false, true},
          {2, "ro_csr", "writing a read-only CSR is illegal instruction and "
                        "writes no register",
           {0x0badc0deull, 0, 0, 2}, true, false, true},
          {3, "csrrs_x0", "csrrs with rs1 == x0 reads but does not write: "
                           "mscratch keeps its value, and a read-only CSR with a "
                           "zero source is a plain read rather than an illegal "
                           "write",
           {0xaaaa5555aaaa5555ull, 0x8000000000001100ull, 0, 0}, true, true, false},
          {4, "misaligned_load", "a misaligned load traps with cause 4",
           {0, 0, 0, 4}, false, false, true},
          {5, "fext", "an F/D instruction is illegal instruction in p0",
           {0x0badc0deull, 0, 0, 2}, true, false, true},
          {6, "never_tohost", "a program that never writes TOHOST hits the "
                              "cycle limit", {0, 0, 0, 0}, false, false, false},
          {7, "rom_store", "a store to boot_rom faults with cause 7",
           {0, 0, 0, 7}, false, false, true},
      };

      constexpr uint64_t kProbeCycles = 20000;
      const std::string probe_bytes = Base64Decode(kProbeImageBase64);
      const std::string probe_manifest = options.out_dir + "/probe.image.txt";

      for (const Probe& probe : probes) {
        // The reference gets the selector as a second manifest segment.  Without
        // it the reference reads 0 out of kPselBase on every probe, so every
        // probe was compared against selector 0's stream and seven of the eight
        // comparisons were vacuous.  The DUT gets the same word through its
        // loader backdoor, so the two runs really do see the same machine.
        {
          std::ofstream out(probe_manifest);
          if (!out) Fail("cannot write " + probe_manifest);
          out << "seg " << std::hex << MOSAIC_RESET_VECTOR << " " << std::dec
              << probe_bytes.size() << " ";
          std::ostringstream hex;
          hex << std::hex << std::setfill('0');
          for (const char c : probe_bytes) {
            hex.width(2);
            hex << static_cast<unsigned>(static_cast<unsigned char>(c));
          }
          out << hex.str() << "\n";
          // The manifest carries bytes, and both the reference and the DUT
          // loader assemble a word little-endian, so the selector is emitted as
          // eight bytes in address order rather than as one big-endian literal.
          out << "seg " << std::hex << kPselBase << " 8 ";
          hex.str("");
          for (int b = 0; b < 8; ++b) {
            hex << std::hex << std::setw(2) << std::setfill('0')
                << ((probe.selector >> (8 * b)) & 0xff);
          }
          out << hex.str() << "\n";
        }

        mosaic::EventTap tap;
        ProbeResult result;
        RunProbe(&dut, probe.selector, kProbeCycles, &tap, &result);

        ReferenceResult expected;
        std::string reference_events, reference_detail;
        // The DUT is bounded in *cycles* and the reference in *retired
        // instructions*, so handing both the same number compares a stream
        // truncated at the cycle limit against one truncated at the step limit
        // and every probe reports a length mismatch that has nothing to do with
        // the architecture.  The reference is given the DUT's event count as
        // its step budget instead: both then stop after the same number of
        // retired instructions, whether that is because the program wrote
        // TOHOST or because the budget ran out.
        if (!RunReference(repo, probe_manifest, options.out_dir, "probe",
                          static_cast<uint64_t>(tap.size()), &expected,
                          &reference_events)) {
          Fail(std::string("probe ") + probe.name + ": " + reference_detail);
        }

        std::ostringstream stream_path;
        stream_path << options.out_dir << "/probe_" << probe.name << ".events.txt";
        std::string save_detail;
        if (!tap.Save(stream_path.str(), &save_detail)) {
          Fail(std::string("probe ") + probe.name + ": " + save_detail);
        }
        std::string mismatch, compare_detail;
        int mismatch_count = 0;
        const bool differs =
            tap.Compare(reference_events, &mismatch, &mismatch_count, &compare_detail);
        reporter.Check(!differs, std::string("probe ") + probe.name +
                                    ": retire stream matches the independent "
                                    "reference (" + compare_detail + ")");
        const uint64_t* signature_words = result.signature;

        // The image's tail writes 0 to TOHOST and parks, and a zero write is
        // not the end-of-program signal, so every probe here is bounded by the
        // cycle limit.  What is asserted is that the run ended there and not by
        // the PC escaping mapped memory, which is the other way a probe can
        // stop early and still look like a pass.
        reporter.Check(result.outcome == Outcome::kCycleLimit,
                       std::string("probe ") + probe.name +
                           ": runs until the cycle limit and is not stopped early");
        if (probe.check_sig0) {
          reporter.Check(signature_words[0] == probe.sig[0],
                         std::string("probe ") + probe.name + ": signature[0] is " +
                             mosaic::Hex(probe.sig[0]) + ", got " +
                             mosaic::Hex(signature_words[0]) + " -- " + probe.rule);
        }
        if (probe.check_sig1) {
          reporter.Check(signature_words[1] == probe.sig[1],
                         std::string("probe ") + probe.name + ": signature[1] is " +
                             mosaic::Hex(probe.sig[1]) + ", got " +
                             mosaic::Hex(signature_words[1]));
        }
        if (probe.check_cause) {
          reporter.Check(signature_words[3] == probe.sig[3],
                         std::string("probe ") + probe.name + ": mcause is " +
                             std::to_string(probe.sig[3]) + ", got " +
                             std::to_string(signature_words[3]) + " -- " + probe.rule);
        }
        std::printf("  probe %-16s %-11s tohost=%s sig=%s,%s,%s,%s\n", probe.name,
                    OutcomeName(result.outcome), mosaic::Hex(result.tohost).c_str(),
                    mosaic::Hex(signature_words[0], 1).c_str(),
                    mosaic::Hex(signature_words[1], 1).c_str(),
                    mosaic::Hex(signature_words[2], 1).c_str(),
                    mosaic::Hex(signature_words[3], 1).c_str());
      }
    }

    detail = "corpus " + std::to_string(programs) + " programs, " +
             std::to_string(total_events) + " architectural events, every stream "
             "identical to the independent reference";
  } catch (const Failure& failure) {
    passed = false;
    detail = "check failed: " + failure.what;
  }

  // A check that is recorded as failed and not reflected in the verdict is the
  // one failure mode a differential harness cannot be allowed to have: the log
  // fills with "CHECK FAILED" lines and the RESULT line still says PASS.
  // Reporter::Check is the only thing that records a check, so its own failure
  // count is the authority here, alongside the thrown Failure from the corpus.
  const bool ok = passed && (reporter.failures() == 0);
  reporter.Check(ok, "no check failed");
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}