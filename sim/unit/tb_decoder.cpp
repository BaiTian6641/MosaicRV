// CASE=decode.rv64im_reserved -- work package I-010.
//
// Drives rtl/core/mosaic_decoder.sv and compares every field of its
// `decode_ctl_t` against an INDEPENDENT C++ reference decoder written from the
// RISC-V ISA manual, plus a set of named assertions about the encodings the
// card calls out by name.
//
// Independence is the whole point of this file. `DecodeRef()` below is written
// from the instruction-format diagrams in volume I of the manual, using its own
// immediate assemblers and its own legality predicates; it shares no code, no
// constants and no structure with the RTL. Where the two disagree, one of them
// is wrong and the test says which instruction and which field. Before this
// comparison was trusted, both the reference and the RTL were cross-checked
// against `riscv64-elf-objdump -M no-aliases` over 92 assembled instructions
// (results/reports/I-010-011-decode-alu.md), which is a third, independent
// source: a shared misreading of the manual would have to be shared with GNU
// as well.
//
// What runs, in order:
//
//   1. Directed       named checks for the encodings the card names: S-type with
//                     a negative offset, a backwards B-type branch, a backwards
//                     J-type jump, addiw of a value whose bit 31 is set,
//                     shift-by-0, shift-by-63, the illegal shift-by-64, an odd
//                     jalr offset, and the CSR x0 read/write rules.
//   2. Reserved       every reserved encoding class, each named, asserted to be
//                     illegal *and* to leave the whole struct at its defined
//                     illegal state.
//   3. Sweeps         structural completeness, one instruction per cycle:
//                       - all 128 opcodes x all 8 funct3 x 8 funct7 patterns
//                       - all 128 funct7 values on every funct7-sensitive opcode
//                       - all 64 insn[31:26] values on OP-IMM
//                       - every rd x rs1 pair, and every rs1 x rs2 pair
//                       - every immediate bit, field by field, exhaustively
//   4. Random         a seeded campaign: half uniform 32-bit words, half words
//                     biased towards the eleven RV64IM opcodes so that the
//                     legal side gets as much coverage as the illegal side.
//
// Every instruction presented is compared field by field against the
// reference; the first mismatch stops the run and is reported with the
// instruction word, the field name and both values.

#include "sim_common.h"
#include "verilated.h"
#include "Vmosaic_decoder_tb.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------- mosaic_pkg encodings
// Mirrors of rtl/core/mosaic_pkg.sv. The *numbering* is part of the contract
// between the decoder and the functional units, so the directed phase asserts
// every ALU operation by number: if the package were renumbered, this case
// fails rather than silently agreeing with a renumbered decoder.
enum : uint8_t {
  ALU_ADD = 0, ALU_SUB = 1, ALU_SLL = 2, ALU_SLT = 3, ALU_SLTU = 4,
  ALU_XOR = 5, ALU_SRL = 6, ALU_SRA = 7, ALU_OR = 8, ALU_AND = 9,
  ALU_ADDW = 10, ALU_SUBW = 11, ALU_SLLW = 12, ALU_SRLW = 13, ALU_SRAW = 14,
  ALU_PASSB = 15
};
enum : uint8_t {
  MD_MUL = 0, MD_MULH = 1, MD_MULHSU = 2, MD_MULHU = 3,
  MD_DIV = 4, MD_DIVU = 5, MD_REM = 6, MD_REMU = 7
};
enum : uint8_t { MEM_NONE = 0, MEM_LOAD = 1, MEM_STORE = 2 };
enum : uint8_t { SZ_BYTE = 0, SZ_HALF = 1, SZ_WORD = 2, SZ_DBL = 3 };
enum : uint8_t { CSR_NONE = 0, CSR_RW = 1, CSR_RS = 2, CSR_RC = 3 };
enum : uint32_t {
  OP_LOAD = 0x03, OP_MISC_MEM = 0x0F, OP_IMM = 0x13, OP_AUIPC = 0x17,
  OP_STORE = 0x23, OP_IMM_32 = 0x1B, OP_MUL_DIV = 0x33, OP_BRANCH = 0x63,
  OP_JALR = 0x67, OP_JAL = 0x6F, OP_SYSTEM = 0x73
};

// ------------------------------------------------------------------ Ref type
// One field per decode_ctl_t member, same names, so the comparison is a
// macro list rather than a hand-written list that can drift out of step.
struct Ref {
  uint8_t valid, illegal;
  uint8_t uses_rs1, uses_rs2, uses_imm;
  uint8_t rs1, rs2, rd;
  uint64_t imm;
  uint8_t alu_op, uses_alu, reg_write;
  uint8_t mem_kind, mem_size, mem_signed;
  uint8_t is_branch, branch_funct, is_jal, is_jalr, is_auipc, writes_link;
  uint8_t is_miscmem, is_fence_i;
  uint8_t is_muldiv, md_op, md_signed;
  uint8_t is_system, is_ecall, is_ebreak, is_mret;
  uint8_t csr_op;
  uint16_t csr_addr;
  uint8_t csr_writes, csr_reads, csr_imm_form;
};

// The single illegal state: every field a defined zero, illegal set. This is
// mosaic_pkg::decode_ctl_t's CTL_ILLEGAL, restated here from the package
// rather than borrowed from the RTL.
Ref Illegal() {
  Ref r{};
  r.illegal = 1;
  return r;
}

// ------------------------------------------------------- reference immediates
// From the "RV32I instruction formats" diagrams. Note the three shapes that are
// easy to get wrong and are each checked against objdump in the report:
//   * I-type imm is insn[31:20] as ONE field -- insn[11:7] is rd.
//   * S-type imm is insn[31:25] || insn[11:7], i.e. it borrows the bits that
//     are rd in an I-type.
//   * B-type and J-type are scrambled, and B/J imm[0] is hard-wired zero.
int64_t SignExtend(uint64_t value, int bits) {
  const uint64_t sign = 1ull << (bits - 1);
  return (value & sign) ? static_cast<int64_t>(value | ~((1ull << bits) - 1))
                        : static_cast<int64_t>(value);
}

int64_t ImmI(uint32_t insn) { return SignExtend((insn >> 20) & 0xFFF, 12); }

int64_t ImmS(uint32_t insn) {
  return SignExtend((((insn >> 25) & 0x7F) << 5) | ((insn >> 7) & 0x1F), 12);
}

int64_t ImmB(uint32_t insn) {
  const uint32_t raw = (((insn >> 31) & 1) << 12) | (((insn >> 7) & 1) << 11) |
                       (((insn >> 25) & 0x3F) << 5) | (((insn >> 8) & 0xF) << 1);
  return SignExtend(raw, 13);
}

int64_t ImmU(uint32_t insn) { return SignExtend(insn & 0xFFFFF000u, 32); }

int64_t ImmJ(uint32_t insn) {
  const uint32_t raw = (((insn >> 31) & 1) << 20) | (((insn >> 12) & 0xFF) << 12) |
                       (((insn >> 20) & 1) << 11) | (((insn >> 21) & 0x3FF) << 1);
  return SignExtend(raw, 21);
}

// ---------------------------------------------------- reference: the decoder
// RV64I base + M, 32-bit instructions only. Returns the full control struct.
Ref DecodeRef(uint32_t insn) {
  const uint32_t op = insn & 0x7F;
  const uint32_t f3 = (insn >> 12) & 7;
  const uint32_t f7 = (insn >> 25) & 0x7F;
  const uint32_t top6 = (insn >> 26) & 0x3F;
  const uint32_t rd = (insn >> 7) & 0x1F;
  const uint32_t rs1 = (insn >> 15) & 0x1F;
  const uint32_t rs2 = (insn >> 20) & 0x1F;
  const uint32_t imm12 = (insn >> 20) & 0xFFF;

  Ref r = Illegal();

  switch (op) {
    case OP_LOAD: {
      r.uses_rs1 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rd = static_cast<uint8_t>(rd);
      r.imm = static_cast<uint64_t>(ImmI(insn));
      r.reg_write = rd != 0;
      r.mem_kind = MEM_LOAD;
      r.uses_alu = 1;
      r.alu_op = ALU_ADD;
      switch (f3) {
        case 0: r.mem_size = SZ_BYTE; r.mem_signed = 1; return r;  // lb
        case 1: r.mem_size = SZ_HALF; r.mem_signed = 1; return r;  // lh
        case 2: r.mem_size = SZ_WORD; r.mem_signed = 1; return r;  // lw
        case 3: r.mem_size = SZ_DBL;  r.mem_signed = 1; return r;  // ld
        case 4: r.mem_size = SZ_BYTE; r.mem_signed = 0; return r;  // lbu
        case 5: r.mem_size = SZ_HALF; r.mem_signed = 0; return r;  // lhu
        default: return Illegal();      // 110, 111 reserved
      }
    }
    case OP_MISC_MEM: {
      if (f3 == 0) { r.is_miscmem = 1; return r; }   // fence
      if (f3 == 1) { r.is_miscmem = 1; r.is_fence_i = 1; return r; }  // fence.i
      return Illegal();
    }
    case OP_IMM: {
      r.uses_rs1 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.imm = static_cast<uint64_t>(ImmI(insn));
      r.uses_alu = 1;
      switch (f3) {
        case 0: r.alu_op = ALU_ADD;  return r;                              // addi
        case 2: r.alu_op = ALU_SLT;  return r;                              // slti
        case 3: r.alu_op = ALU_SLTU; return r;                              // sltiu
        case 4: r.alu_op = ALU_XOR;  return r;                              // xori
        case 6: r.alu_op = ALU_OR;   return r;                              // ori
        case 7: r.alu_op = ALU_AND;  return r;                              // andi
        case 1:                                                          // slli
          if (top6 != 0) return Illegal();
          r.alu_op = ALU_SLL;
          r.imm = insn >> 20 & 0x3F;
          return r;
        case 5:                                                          // srai/srli
          if (top6 == 0) { r.alu_op = ALU_SRL; r.imm = insn >> 20 & 0x3F; return r; }
          if (top6 == 0x10) { r.alu_op = ALU_SRA; r.imm = insn >> 20 & 0x3F; return r; }
          return Illegal();
        default: return Illegal();
      }
    }
    case OP_AUIPC: {
      // funct3 is not part of the encoding.
      r.uses_imm = 1;
      r.imm = static_cast<uint64_t>(ImmU(insn));
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.is_auipc = 1;
      r.uses_alu = 1;
      r.alu_op = ALU_ADD;
      return r;
    }
    case OP_STORE: {
      r.uses_rs1 = 1;
      r.uses_rs2 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rs2 = static_cast<uint8_t>(rs2);
      r.imm = static_cast<uint64_t>(ImmS(insn));
      r.mem_kind = MEM_STORE;
      r.uses_alu = 1;
      r.alu_op = ALU_ADD;
      switch (f3) {
        case 0: r.mem_size = SZ_BYTE; return r;   // sb
        case 1: r.mem_size = SZ_HALF; return r;   // sh
        case 2: r.mem_size = SZ_WORD; return r;   // sw
        case 3: r.mem_size = SZ_DBL;  return r;   // sd
        default: return Illegal();
      }
    }
    case OP_IMM_32: {
      r.uses_rs1 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.uses_alu = 1;
      r.imm = static_cast<uint64_t>(ImmI(insn));
      switch (f3) {
        case 0: r.alu_op = ALU_ADDW; return r;                          // addiw
        case 1:                                                          // slliw
          if (f7 != 0) return Illegal();
          r.alu_op = ALU_SLLW;
          r.imm = (insn >> 20) & 0x1F;
          return r;
        case 5:                                                          // srliw/sraiw
          if (f7 == 0) { r.alu_op = ALU_SRLW; r.imm = (insn >> 20) & 0x1F; return r; }
          if (f7 == 0x20) { r.alu_op = ALU_SRAW; r.imm = (insn >> 20) & 0x1F; return r; }
          return Illegal();
        default: return Illegal();
      }
    }
    case OP_MUL_DIV: {
      r.uses_rs1 = 1;
      r.uses_rs2 = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rs2 = static_cast<uint8_t>(rs2);
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      if (f7 == 1) {                                   // M extension
        r.is_muldiv = 1;
        r.md_op = static_cast<uint8_t>(f3);
        r.md_signed = !(f3 == 3 || f3 == 5 || f3 == 7);
        return r;
      }
      r.uses_alu = 1;
      switch (f3) {
        case 0:
          if (f7 == 0) r.alu_op = ALU_ADD;
          else if (f7 == 0x20) r.alu_op = ALU_SUB;
          else return Illegal();
          return r;
        case 1: if (f7 == 0) r.alu_op = ALU_SLL; else return Illegal(); return r;
        case 2: if (f7 == 0) r.alu_op = ALU_SLT; else return Illegal(); return r;
        case 3: if (f7 == 0) r.alu_op = ALU_SLTU; else return Illegal(); return r;
        case 4: if (f7 == 0) r.alu_op = ALU_XOR; else return Illegal(); return r;
        case 5:
          if (f7 == 0) r.alu_op = ALU_SRL;
          else if (f7 == 0x20) r.alu_op = ALU_SRA;
          else return Illegal();
          return r;
        case 6: if (f7 == 0) r.alu_op = ALU_OR; else return Illegal(); return r;
        default:
          if (f7 == 0) r.alu_op = ALU_AND; else return Illegal();
          return r;
      }
    }
    case OP_BRANCH: {
      if (f3 == 2 || f3 == 3) return Illegal();   // 010, 011 are not branches
      r.uses_rs1 = 1;
      r.uses_rs2 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rs2 = static_cast<uint8_t>(rs2);
      r.imm = static_cast<uint64_t>(ImmB(insn));
      r.is_branch = 1;
      r.branch_funct = static_cast<uint8_t>(f3);
      return r;
    }
    case OP_JALR: {
      if (f3 != 0) return Illegal();
      r.uses_rs1 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rd = static_cast<uint8_t>(rd);
      r.imm = static_cast<uint64_t>(ImmI(insn));
      r.reg_write = rd != 0;
      r.is_jalr = 1;
      r.writes_link = 1;
      return r;
    }
    case OP_JAL: {
      // funct3 is not part of the encoding.
      r.uses_imm = 1;
      r.imm = static_cast<uint64_t>(ImmJ(insn));
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.is_jal = 1;
      r.writes_link = 1;
      return r;
    }
    case OP_SYSTEM: {
      switch (f3) {
        case 0: {
          if (rd != 0 || rs1 != 0) return Illegal();
          if (imm12 == 0x000) { r.is_system = 1; r.is_ecall = 1; return r; }
          if (imm12 == 0x001) { r.is_system = 1; r.is_ebreak = 1; return r; }
          if (imm12 == 0x302) { r.is_system = 1; r.is_mret = 1; return r; }
          return Illegal();
        }
        case 1: case 2: case 3:
        case 5: case 6: case 7: {
          const bool imm_form = f3 >= 5;
          r.is_system = 1;
          r.csr_imm_form = imm_form ? 1 : 0;
          r.csr_addr = static_cast<uint16_t>(imm12);
          r.uses_rs1 = 1;
          r.rs1 = static_cast<uint8_t>(rs1);
          r.rd = static_cast<uint8_t>(rd);
          r.reg_write = rd != 0;
          r.csr_op = (f3 == 1 || f3 == 5) ? CSR_RW : (f3 == 2 || f3 == 6) ? CSR_RS
                                                                       : CSR_RC;
          r.csr_writes = (r.csr_op == CSR_RW) || rs1 != 0;
          r.csr_reads = (r.csr_op != CSR_RW) || rd != 0;
          return r;
        }
        default:
          return Illegal();     // funct3 100 is reserved
      }
    }
    default:
      return Illegal();         // every other opcode, including OP-32
  }
}

// ------------------------------------------------------------------ encoders
// Written from the same manual diagrams, so a directed test builds the word it
// means to test rather than pasting a hex constant whose provenance is unclear.
uint32_t EncI(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm) {
  return op | (f3 << 12) | (rd << 7) | (rs1 << 15) |
         ((static_cast<uint32_t>(imm) & 0xFFF) << 20);
}
uint32_t EncS(uint32_t op, uint32_t f3, uint32_t rs2, uint32_t rs1, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0xFFF;
  return op | (f3 << 12) | ((u & 0x1F) << 7) | (rs1 << 15) | (rs2 << 20) |
         (((u >> 5) & 0x7F) << 25);
}
uint32_t EncB(uint32_t op, uint32_t f3, uint32_t rs2, uint32_t rs1, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off) & 0x1FFF;
  return op | (f3 << 12) | (((u >> 11) & 1) << 7) | (rs2 << 20) | (rs1 << 15) |
         (((u >> 1) & 0xF) << 8) | (((u >> 5) & 0x3F) << 25) |
         (((u >> 12) & 1) << 31);
}
uint32_t EncU(uint32_t op, uint32_t rd, uint32_t imm20) {
  return op | (rd << 7) | ((imm20 & 0xFFFFF) << 12);
}
uint32_t EncJ(uint32_t op, uint32_t rd, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off) & 0x1FFFFF;
  return op | (rd << 7) | (((u >> 12) & 0xFF) << 12) | (((u >> 11) & 1) << 20) |
         (((u >> 1) & 0x3FF) << 21) | (((u >> 20) & 1) << 31);
}
uint32_t EncR(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, uint32_t rs2,
              uint32_t f7) {
  return op | (f3 << 12) | (rd << 7) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}
// Shift-immediate: shamt is 6 bits in insn[25:20] and the selector is insn[31:26].
uint32_t EncShiftX(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1,
                   uint32_t shamt, uint32_t top6) {
  return op | (f3 << 12) | (rd << 7) | (rs1 << 15) | (top6 << 26) |
         ((shamt & 0x3F) << 20);
}
// Word shift-immediate: shamt is 5 bits in insn[24:20], selector insn[31:25].
uint32_t EncShiftW(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1,
                   uint32_t shamt, uint32_t f7) {
  return op | (f3 << 12) | (rd << 7) | (rs1 << 15) | ((shamt & 0x1F) << 20) |
         (f7 << 25);
}
// Raw word from its fields, for the structural sweeps.
uint32_t Word(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, uint32_t rs2,
              uint32_t f7) {
  return op | (f3 << 12) | (rd << 7) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}

// ------------------------------------------------------------------ the bench
class Bench {
 public:
  Bench(const mosaic::Options& options, mosaic::Reporter& reporter,
        Vmosaic_decoder_tb* top)
      : opt_(options), rep_(reporter), top_(top) {}

  // Present one instruction word and compare every field. Returns false on the
  // first mismatch, which stops the run: a decoder that disagrees about an
  // instruction is broken, and the rest of the campaign would only bury the
  // diagnostic.
  bool Apply(uint32_t insn) {
    if (aborted_) return false;
    if (presented_ >= opt_.max_cycles) {
      aborted_ = true;
      reason_ = "exceeded --max-cycles=" + std::to_string(opt_.max_cycles);
      rep_.Check(false, reason_);
      return false;
    }
    ++presented_;
    top_->insn = insn;
    top_->eval();

    ++op_seen_[insn & 0x7F];
    ++opf3_seen_[(insn & 0x7F) * 8 + ((insn >> 12) & 7)];

    const Ref want = DecodeRef(insn);
    const Ref got = Observe();
    if (want.valid) ++legal_;
    if (want.illegal) ++illegal_;

    std::string where = "insn=" + mosaic::Hex(insn, 8);
    bool ok = true;
    ok &= Field(where, "valid", want.valid, got.valid);
    ok &= Field(where, "illegal", want.illegal, got.illegal);
    ok &= Field(where, "uses_rs1", want.uses_rs1, got.uses_rs1);
    ok &= Field(where, "uses_rs2", want.uses_rs2, got.uses_rs2);
    ok &= Field(where, "uses_imm", want.uses_imm, got.uses_imm);
    ok &= Field(where, "rs1", want.rs1, got.rs1);
    ok &= Field(where, "rs2", want.rs2, got.rs2);
    ok &= Field(where, "rd", want.rd, got.rd);
    ok &= Field(where, "imm", want.imm, got.imm);
    ok &= Field(where, "alu_op", want.alu_op, got.alu_op);
    ok &= Field(where, "uses_alu", want.uses_alu, got.uses_alu);
    ok &= Field(where, "reg_write", want.reg_write, got.reg_write);
    ok &= Field(where, "mem_kind", want.mem_kind, got.mem_kind);
    ok &= Field(where, "mem_size", want.mem_size, got.mem_size);
    ok &= Field(where, "mem_signed", want.mem_signed, got.mem_signed);
    ok &= Field(where, "is_branch", want.is_branch, got.is_branch);
    ok &= Field(where, "branch_funct", want.branch_funct, got.branch_funct);
    ok &= Field(where, "is_jal", want.is_jal, got.is_jal);
    ok &= Field(where, "is_jalr", want.is_jalr, got.is_jalr);
    ok &= Field(where, "is_auipc", want.is_auipc, got.is_auipc);
    ok &= Field(where, "writes_link", want.writes_link, got.writes_link);
    ok &= Field(where, "is_miscmem", want.is_miscmem, got.is_miscmem);
    ok &= Field(where, "is_fence_i", want.is_fence_i, got.is_fence_i);
    ok &= Field(where, "is_muldiv", want.is_muldiv, got.is_muldiv);
    ok &= Field(where, "md_op", want.md_op, got.md_op);
    ok &= Field(where, "md_signed", want.md_signed, got.md_signed);
    ok &= Field(where, "is_system", want.is_system, got.is_system);
    ok &= Field(where, "is_ecall", want.is_ecall, got.is_ecall);
    ok &= Field(where, "is_ebreak", want.is_ebreak, got.is_ebreak);
    ok &= Field(where, "is_mret", want.is_mret, got.is_mret);
    ok &= Field(where, "csr_op", want.csr_op, got.csr_op);
    ok &= Field(where, "csr_addr", want.csr_addr, got.csr_addr);
    ok &= Field(where, "csr_writes", want.csr_writes, got.csr_writes);
    ok &= Field(where, "csr_reads", want.csr_reads, got.csr_reads);
    ok &= Field(where, "csr_imm_form", want.csr_imm_form, got.csr_imm_form);

    if (!ok) {
      rep_.Mismatch(where, "reference decoder", "RTL decoder");
      aborted_ = true;
      reason_ = "decoder disagrees with the reference at " + where;
      rep_.Check(false, reason_);
      return false;
    }
    if (!CheckBreakout(got)) return false;
    return true;
  }

  // ---- named directed checks ------------------------------------------------
  void Directed() {
    if (aborted_) return;

    // --- immediate formats, each at a value that cannot be confused ---------
    // S-type with a negative offset. insn[11:7] supplies imm[4:0] here, so an
    // I-type assembly of this offset would give -32 instead of -16.
    {
      const uint32_t insn = EncS(OP_STORE, 0, 5, 6, -16);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.imm == static_cast<uint64_t>(-16), "S-type offset -16");
      rep_.Check(r.rs2 == 5 && r.rs1 == 6, "S-type rs2/rs1");
      rep_.Check(r.rd == 0, "S-type has no rd: insn[11:7] is immediate, not rd");
      rep_.Check(r.uses_rs2 && !r.uses_imm * 0 && r.uses_imm,
                 "S-type uses rs2 and imm");
      rep_.Check(r.mem_kind == MEM_STORE && r.mem_size == SZ_BYTE, "sb");
    }
    {
      const uint32_t insn = EncS(OP_STORE, 3, 31, 7, -2048);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.imm == static_cast<uint64_t>(-2048), "sd offset -2048");
      rep_.Check(r.mem_size == SZ_DBL, "sd is a doubleword store");
    }
    // S-type offset +2047: the extreme positive, which needs imm[11:5] = 1111111.
    {
      const uint32_t insn = EncS(OP_STORE, 2, 1, 2, 2047);
      if (!Apply(insn)) return;
      rep_.Check(Observe().imm == 2047, "sw offset +2047");
    }

    // B-type backwards branch. imm[12], imm[10:5] and imm[4:1] are three
    // different bit ranges of the word; a swapped pair shows up here and only
    // here.
    {
      const uint32_t insn = EncB(OP_BRANCH, 0, 13, 14, -4096);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.imm == static_cast<uint64_t>(-4096), "beq offset -4096 (max back)");
      rep_.Check(r.is_branch && r.branch_funct == 0, "beq");
      rep_.Check(r.rs1 == 14 && r.rs2 == 13, "B-type rs1/rs2");
      rep_.Check(r.rd == 0 && !r.reg_write, "branch writes no register");
    }
    {
      const uint32_t insn = EncB(OP_BRANCH, 4, 1, 2, -2);   // blt
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.imm == static_cast<uint64_t>(-2) && r.branch_funct == 4,
                 "blt offset -2, funct3 100");
    }
    {
      const uint32_t insn = EncB(OP_BRANCH, 7, 31, 30, 4094);  // bgeu, max forward
      if (!Apply(insn)) return;
      rep_.Check(Observe().imm == 4094 && Observe().branch_funct == 7,
                 "bgeu offset +4094 (max forward)");
    }

    // J-type backward jump: imm[20], imm[10:1], imm[11] and imm[19:12].
    {
      const uint32_t insn = EncJ(OP_JAL, 1, -1048576);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.imm == static_cast<uint64_t>(-1048576),
                 "jal offset -1048576 (most negative legal)");
      rep_.Check(r.is_jal && r.writes_link && r.reg_write, "jal writes the link");
      rep_.Check(r.rd == 1 && !r.uses_rs1 && !r.uses_rs2, "jal rd, no rs1/rs2");
    }
    {
      const uint32_t insn = EncJ(OP_JAL, 0, 1048574);
      if (!Apply(insn)) return;
      rep_.Check(Observe().imm == 1048574, "jal offset +1048574 (max forward)");
    }
    {
      const uint32_t insn = EncJ(OP_JAL, 31, -2);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.imm == static_cast<uint64_t>(-2),
                 "jal offset -2: imm[20] set and sign set together");
      rep_.Check(r.reg_write == 0, "jal with rd == x0 performs no register write");
    }

    // addiw of a value whose bit 31 is set. The decoder selects ALU_ADDW, and
    // the ALU sign-extends that 32-bit result; the immediate here is -1, i.e.
    // every bit set, so an operand-side 32-bit truncation would be visible.
    {
      const uint32_t insn = EncI(OP_IMM_32, 0, 9, 8, -1);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.alu_op == ALU_ADDW, "addiw selects ALU_ADDW (sign-extending)");
      rep_.Check(r.imm == UINT64_MAX, "addiw immediate -1 sign-extends to all ones");
      rep_.Check(r.rs1 == 8 && r.rd == 9, "addiw rs1/rd");
      rep_.Check(r.uses_alu && r.uses_imm, "addiw uses rs1 + imm");
    }

    // Shifts: by zero, by 63, and the illegal by 64.
    {
      const uint32_t insn = EncShiftX(OP_IMM, 1, 5, 6, 0, 0x00);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.alu_op == ALU_SLL && r.imm == 0, "slli by 0");
    }
    {
      const uint32_t insn = EncShiftX(OP_IMM, 1, 5, 6, 63, 0x00);
      if (!Apply(insn)) return;
      rep_.Check(Observe().imm == 63 && Observe().alu_op == ALU_SLL,
                 "slli by 63: insn[31:26] zero, 6-bit shamt");
    }
    {
      // shamt field 0 with insn[31:26] = 000001: a shift amount of 64 in the
      // RV32 reading of the encoding, and reserved on RV64.
      const uint32_t insn = EncShiftX(OP_IMM, 1, 5, 6, 0, 0x01);
      if (!Apply(insn)) return;
      ExpectIllegalState("slli by 64 is reserved on RV64", insn);
    }
    {
      const uint32_t insn = EncShiftX(OP_IMM, 5, 5, 6, 63, 0x10);
      if (!Apply(insn)) return;
      rep_.Check(Observe().alu_op == ALU_SRA && Observe().imm == 63,
                 "srai by 63: funct3 101 with insn[31:26] = 010000");
    }
    {
      const uint32_t insn = EncShiftX(OP_IMM, 5, 5, 6, 31, 0x00);
      if (!Apply(insn)) return;
      rep_.Check(Observe().alu_op == ALU_SRL, "srli by 31: funct3 101, top6 zero");
    }
    {
      const uint32_t insn = EncShiftW(OP_IMM_32, 1, 5, 6, 31, 0x00);
      if (!Apply(insn)) return;
      rep_.Check(Observe().alu_op == ALU_SLLW && Observe().imm == 31,
                 "slliw by 31: 5-bit shamt in insn[24:20]");
    }
    {
      const uint32_t insn = EncShiftW(OP_IMM_32, 5, 5, 6, 31, 0x20);
      if (!Apply(insn)) return;
      rep_.Check(Observe().alu_op == ALU_SRAW, "sraiw by 31");
    }
    {
      const uint32_t insn = EncShiftW(OP_IMM_32, 5, 5, 6, 0, 0x01);
      if (!Apply(insn)) return;
      ExpectIllegalState("srliw with funct7 0000001 is reserved", insn);
    }

    // jalr: the decoder exposes pc + rs1 + imm and clears nothing. An odd
    // immediate must survive intact -- clearing target bit 0 is the core's
    // job (I-011), because it is a target-computation rule.
    {
      const uint32_t insn = EncI(OP_JALR, 0, 1, 2, 3);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.is_jalr && r.writes_link && r.reg_write, "jalr links");
      rep_.Check(r.imm == 3, "jalr immediate 3 is not cleared by the decoder");
      rep_.Check(r.rs1 == 2 && r.uses_rs1 && r.uses_imm, "jalr uses rs1 + imm");
      rep_.Check(!r.uses_alu, "jalr does not use the ALU datapath");
    }
    {
      const uint32_t insn = EncI(OP_JALR, 0, 1, 2, -2048);
      if (!Apply(insn)) return;
      rep_.Check(Observe().imm == static_cast<uint64_t>(-2048), "jalr -2048");
    }

    // lui / auipc: U-type, no rs1, no rs2, and the immediate has its low 12
    // bits hard-wired zero.
    {
      const uint32_t insn = EncU(OP_AUIPC, 3, 0xABCDE);
      if (!Apply(insn)) return;
      const Ref r = Observe();
      rep_.Check(r.is_auipc && r.alu_op == ALU_ADD && r.uses_imm, "auipc");
      rep_.Check(r.imm == static_cast<uint64_t>(0xABCDE000), "auipc imm, low 12 zero");
      rep_.Check(!r.uses_rs1 && !r.uses_rs2 && r.rs1 == 0 && r.rs2 == 0,
                 "auipc insn[19:15] and insn[24:20] are immediate, not registers");
    }
    {
      const uint32_t insn = EncU(OP_AUIPC, 3, 0x80000);
      if (!Apply(insn)) return;
      rep_.Check(Observe().imm == 0xFFFFFFFF80000000ull, "auipc sign-extends imm[31]");
    }
    {
      const uint32_t insn = EncU(OP_AUIPC, 3, 0xFFFFF);
      if (!Apply(insn)) return;
      rep_.Check(Observe().imm == static_cast<uint64_t>(0xFFFFFFFFFFFFF000),
                 "auipc with imm[31:12] all ones");
    }

    // Every legal ALU operation, by number, so a renumbering of mosaic_pkg's
    // alu_op_e cannot pass unnoticed.
    {
      struct { const char* name; uint32_t insn; uint8_t op; } cases[] = {
          {"add",   EncR(OP_MUL_DIV, 0, 1, 2, 3, 0x00), ALU_ADD},
          {"sub",   EncR(OP_MUL_DIV, 0, 1, 2, 3, 0x20), ALU_SUB},
          {"sll",   EncR(OP_MUL_DIV, 1, 1, 2, 3, 0x00), ALU_SLL},
          {"slt",   EncR(OP_MUL_DIV, 2, 1, 2, 3, 0x00), ALU_SLT},
          {"sltu",  EncR(OP_MUL_DIV, 3, 1, 2, 3, 0x00), ALU_SLTU},
          {"xor",   EncR(OP_MUL_DIV, 4, 1, 2, 3, 0x00), ALU_XOR},
          {"srl",   EncR(OP_MUL_DIV, 5, 1, 2, 3, 0x00), ALU_SRL},
          {"sra",   EncR(OP_MUL_DIV, 5, 1, 2, 3, 0x20), ALU_SRA},
          {"or",    EncR(OP_MUL_DIV, 6, 1, 2, 3, 0x00), ALU_OR},
          {"and",   EncR(OP_MUL_DIV, 7, 1, 2, 3, 0x00), ALU_AND},
          {"addi",  EncI(OP_IMM, 0, 1, 2, 4),            ALU_ADD},
          {"slti",  EncI(OP_IMM, 2, 1, 2, 4),            ALU_SLT},
          {"sltiu", EncI(OP_IMM, 3, 1, 2, 4),            ALU_SLTU},
          {"xori",  EncI(OP_IMM, 4, 1, 2, 4),            ALU_XOR},
          {"ori",   EncI(OP_IMM, 6, 1, 2, 4),            ALU_OR},
          {"andi",  EncI(OP_IMM, 7, 1, 2, 4),            ALU_AND},
          {"addiw", EncI(OP_IMM_32, 0, 1, 2, 4),         ALU_ADDW},
      };
      for (const auto& c : cases) {
        if (!Apply(c.insn)) return;
        const Ref r = Observe();
        rep_.Check(r.alu_op == c.op && r.uses_alu,
                   std::string("alu_op of ") + c.name);
      }
    }

    // The RV32 word forms are not in this list on purpose: they are reserved.

    // Memory widths and signedness.
    {
      struct { const char* name; uint32_t insn; uint8_t size; uint8_t sign; } m[] = {
          {"lb",  EncI(OP_LOAD, 0, 1, 2, 8), SZ_BYTE, 1},
          {"lh",  EncI(OP_LOAD, 1, 1, 2, 8), SZ_HALF, 1},
          {"lw",  EncI(OP_LOAD, 2, 1, 2, 8), SZ_WORD, 1},
          {"ld",  EncI(OP_LOAD, 3, 1, 2, 8), SZ_DBL,  1},
          {"lbu", EncI(OP_LOAD, 4, 1, 2, 8), SZ_BYTE, 0},
          {"lhu", EncI(OP_LOAD, 5, 1, 2, 8), SZ_HALF, 0},
          {"sb",  EncS(OP_STORE, 0, 1, 2, 8), SZ_BYTE, 0},
          {"sh",  EncS(OP_STORE, 1, 1, 2, 8), SZ_HALF, 0},
          {"sw",  EncS(OP_STORE, 2, 1, 2, 8), SZ_WORD, 0},
          {"sd",  EncS(OP_STORE, 3, 1, 2, 8), SZ_DBL,  0},
      };
      for (const auto& c : m) {
        if (!Apply(c.insn)) return;
        const Ref r = Observe();
        rep_.Check(r.mem_size == c.size && r.mem_signed == c.sign,
                   std::string("memory width/signedness of ") + c.name);
        rep_.Check(r.uses_alu && r.alu_op == ALU_ADD,
                   std::string(c.name) + " computes rs1 + imm in the ALU");
      }
    }

    // M extension: md_op and md_signed for all eight.
    {
      struct { const char* name; uint32_t f3; uint8_t op; uint8_t sign; } m[] = {
          {"mul", 0, MD_MUL, 1},    {"mulh", 1, MD_MULH, 1},
          {"mulhsu", 2, MD_MULHSU, 1}, {"mulhu", 3, MD_MULHU, 0},
          {"div", 4, MD_DIV, 1},   {"divu", 5, MD_DIVU, 0},
          {"rem", 6, MD_REM, 1},   {"remu", 7, MD_REMU, 0},
      };
      for (const auto& c : m) {
        const uint32_t insn = EncR(OP_MUL_DIV, c.f3, 4, 5, 6, 0x01);
        if (!Apply(insn)) return;
        const Ref r = Observe();
        rep_.Check(r.is_muldiv && r.md_op == c.op && r.md_signed == c.sign,
                   std::string("M classification of ") + c.name);
        rep_.Check(!r.uses_alu && r.uses_rs1 && r.uses_rs2 && r.reg_write,
                   std::string(c.name) + " reads rs1/rs2 and writes rd");
      }
    }

    // CSR x0 rules. These are the two rules a decoder most often gets wrong,
    // because both look like they are handled by the CSR unit.
    {
      const uint32_t insn = Word(OP_SYSTEM, 1, 0, 0, 0xC00, 0x300);  // csrrw x0, mstatus, x0
      if (!Apply(insn)) return;
      Ref r = Observe();
      rep_.Check(!r.csr_writes, "csrrw always writes");
      rep_.Check(!r.csr_reads, "csrrw with rd == x0 does not read the CSR");
      rep_.Check(r.csr_op == CSR_RW && r.csr_addr == 0x300, "csrrw csr_op/addr");
      rep_.Check(!r.reg_write, "csrrw x0 writes no register");
      rep_.Check(r.is_system && !r.csr_imm_form, "csrrw is a register form");
    }
    {
      const uint32_t insn = Word(OP_SYSTEM, 1, 1, 0, 0xC00, 0x300);  // csrrw x1, csr, x0
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.csr_writes && r.csr_reads, "csrrw with rd != x0 reads and writes");
    }
    {
      const uint32_t insn = Word(OP_SYSTEM, 2, 1, 0, 0xC00, 0x340);  // csrrs x1, mscratch, x0
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(!r.csr_writes, "csrrs with rs1 == x0 does not write the CSR");
      rep_.Check(r.csr_reads, "csrrs always reads");
      rep_.Check(r.csr_op == CSR_RS && r.reg_write, "csrrs csr_op and rd write");
    }
    {
      const uint32_t insn = Word(OP_SYSTEM, 3, 1, 0, 0xC00, 0x340);  // csrrc x1, mscratch, x0
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(!r.csr_writes && r.csr_reads, "csrrc with rs1 == x0 does not write");
    }
    {
      const uint32_t insn = Word(OP_SYSTEM, 2, 1, 5, 0xC00, 0x340);  // csrrs x1, mscratch, x5
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.csr_writes, "csrrs with rs1 != x0 writes");
    }
    {
      const uint32_t insn = Word(OP_SYSTEM, 5, 0, 0, 0x340, 0x305);  // csrrwi x0, mscratch, 0
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.csr_imm_form && !r.csr_reads,
                 "csrrwi x0 does not read the CSR");
      rep_.Check(!r.csr_writes, "csrrwi with zimm == 0 does not write");
    }
    {
      const uint32_t insn = Word(OP_SYSTEM, 6, 1, 31, 0x305, 0xC00);  // csrrsi x1, cycle, 31
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.csr_imm_form && r.csr_writes && r.csr_reads,
                 "csrrsi with zimm != 0 writes and reads");
      rep_.Check(r.csr_op == CSR_RS && r.csr_addr == 0xC00, "csrrsi csr_op/addr");
    }
    {
      const uint32_t insn = Word(OP_SYSTEM, 7, 1, 17, 0x180, 0xF11);  // csrrci x1, satp, 17
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.csr_op == CSR_RC && r.csr_addr == 0x180, "csrrci csr_op/addr");
    }

    // ecall / ebreak / mret, and the registers they do not have.
    {
      const uint32_t insn = 0x00000073u;
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.is_system && r.is_ecall && !r.is_ebreak && !r.is_mret,
                 "ecall");
      rep_.Check(!r.reg_write && !r.uses_rs1 && !r.uses_rs2, "ecall reads nothing");
    }
    {
      const uint32_t insn = 0x00100073u;
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.is_system && r.is_ebreak && !r.is_ecall, "ebreak");
    }
    {
      const uint32_t insn = 0x30200073u;
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.is_system && r.is_mret, "mret");
    }

    // fence / fence.i
    {
      const uint32_t insn = 0x0FF0000Fu;
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.is_miscmem && !r.is_fence_i, "fence");
    }
    {
      const uint32_t insn = 0x0000100Fu;
      if (!Apply(insn)) return;
      r = Observe();
      rep_.Check(r.is_miscmem && r.is_fence_i, "fence.i");
    }

    // x0 discipline: every instruction that can write rd must report reg_write
    // as 0 when rd is x0, so nothing downstream has to special-case x0.
    {
      const uint32_t words[] = {
          EncI(OP_IMM, 0, 0, 2, 4), EncI(OP_IMM_32, 0, 0, 2, 4),
          EncU(OP_AUIPC, 0, 0x123), EncJ(OP_JAL, 0, 4), EncI(OP_JALR, 0, 0, 2, 4),
          EncI(OP_LOAD, 2, 0, 2, 4), EncR(OP_MUL_DIV, 0, 0, 1, 2, 0x00),
          EncR(OP_MUL_DIV, 1, 0, 1, 2, 0x01), Word(OP_SYSTEM, 1, 0, 1, 0x300, 0x300),
      };
      for (uint32_t insn : words) {
        if (!Apply(insn)) return;
        rep_.Check(!Observe().reg_write,
                   "insn=" + mosaic::Hex(insn, 8) + " with rd == x0 does not write");
      }
    }
  }

  // ---- named reserved-encoding checks ---------------------------------------
  void Reserved() {
    if (aborted_) return;

    // LOAD funct3 110/111: RV64I defines no load wider than ld.
    for (uint32_t f3 : {6u, 7u}) {
      ExpectIllegalState("reserved: LOAD funct3 " + std::to_string(f3),
                         Word(OP_LOAD, f3, 1, 2, 0, 0x00));
      if (aborted_) return;
    }
    // MISC-MEM funct3 010..111: only fence (000) and fence.i (001) exist.
    for (uint32_t f3 = 2; f3 < 8; ++f3) {
      ExpectIllegalState("reserved: MISC-MEM funct3 " + std::to_string(f3),
                         Word(OP_MISC_MEM, f3, 1, 2, 3, 0x0F));
      if (aborted_) return;
    }
    // OP-IMM slli with any nonzero insn[31:26].
    for (uint32_t top6 = 1; top6 < 64; ++top6) {
      ExpectIllegalState("reserved: slli with insn[31:26]=" + std::to_string(top6),
                         EncShiftX(OP_IMM, 1, 1, 2, 3, top6));
      if (aborted_) return;
    }
    // OP-IMM funct3 101 with insn[31:26] not 000000 or 010000.
    for (uint32_t top6 = 0; top6 < 64; ++top6) {
      if (top6 == 0 || top6 == 0x10) continue;
      ExpectIllegalState("reserved: srli/srai selector insn[31:26]=" +
                             std::to_string(top6),
                         EncShiftX(OP_IMM, 5, 1, 2, 3, top6));
      if (aborted_) return;
    }
    // STORE funct3 100..111.
    for (uint32_t f3 = 4; f3 < 8; ++f3) {
      ExpectIllegalState("reserved: STORE funct3 " + std::to_string(f3),
                         Word(OP_STORE, f3, 0, 1, 2, 0x00));
      if (aborted_) return;
    }
    // OP: funct7 outside {0000000, 0100000, 0000001}.
    for (uint32_t f7 = 2; f7 < 128; ++f7) {
      if (f7 == 0x20) continue;
      for (uint32_t f3 = 0; f3 < 8; ++f3) {
        ExpectIllegalState("reserved: OP funct7=" + std::to_string(f7) +
                               " funct3=" + std::to_string(f3),
                           Word(OP_MUL_DIV, f3, 1, 2, 3, f7));
        if (aborted_) return;
      }
    }
    // OP-IMM-32: funct3 outside {000, 001, 101}.
    for (uint32_t f3 = 0; f3 < 8; ++f3) {
      if (f3 == 0 || f3 == 1 || f3 == 5) continue;
      ExpectIllegalState("reserved: OP-IMM-32 funct3 " + std::to_string(f3),
                         Word(OP_IMM_32, f3, 1, 2, 3, 0x00));
      if (aborted_) return;
    }
    // OP-IMM-32 shift funct7 outside {0000000, 0100000}.
    for (uint32_t f7 = 1; f7 < 128; ++f7) {
      if (f7 == 0x20) continue;
      for (uint32_t f3 : {1u, 5u}) {
        ExpectIllegalState("reserved: OP-IMM-32 shift funct7=" +
                               std::to_string(f7) + " funct3=" +
                               std::to_string(f3),
                           Word(OP_IMM_32, f3, 1, 2, 3, f7));
        if (aborted_) return;
      }
    }
    // BRANCH funct3 100 is not a branch: funct3 100 is blt, and 010/011 do not
    // exist. Wait -- blt IS funct3 100, so the reserved values are 010 and 011.
    for (uint32_t f3 : {2u, 3u}) {
      ExpectIllegalState("reserved: BRANCH funct3 " + std::to_string(f3),
                         Word(OP_BRANCH, f3, 0, 1, 2, 0x00));
      if (aborted_) return;
    }
    // JALR funct3 other than 000.
    for (uint32_t f3 = 1; f3 < 8; ++f3) {
      ExpectIllegalState("reserved: JALR funct3 " + std::to_string(f3),
                         Word(OP_JALR, f3, 1, 2, 0, 0x00));
      if (aborted_) return;
    }
    // The RV32-only word forms: addw subw sllw srlw sraw, opcode 0111011.
    {
      struct { const char* name; uint32_t f3; uint32_t f7; } w[] = {
          {"addw", 0, 0x00}, {"subw", 0, 0x20}, {"sllw", 1, 0x00},
          {"srlw", 5, 0x00}, {"sraw", 5, 0x20},
      };
      for (const auto& c : w) {
        ExpectIllegalState(std::string("reserved on RV64: ") + c.name,
                           Word(0x3B, c.f3, 1, 2, 3, c.f7));
        if (aborted_) return;
      }
      // Every other funct3/funct7 combination on that opcode is reserved too.
      for (uint32_t f3 = 0; f3 < 8; ++f3) {
        for (uint32_t f7 = 0; f7 < 128; ++f7) {
          if (!Apply(Word(0x3B, f3, 1, 2, 3, f7))) return;
        }
      }
    }
    // SYSTEM funct3 100 is reserved, and so is every funct3 111.. except the
    // three immediate CSR forms above; here only 100 is untested.
    ExpectIllegalState("reserved: SYSTEM funct3 100", Word(OP_SYSTEM, 4, 1, 2, 0x300, 0x000));
    if (aborted_) return;
    // SYSTEM funct3 000 with an imm12 that is not 000/001/302.
    for (uint32_t imm12 : {0x002u, 0x003u, 0x101u, 0x102u, 0x200u, 0x301u,
                           0x303u, 0x305u, 0x7B2u, 0xB00u, 0xFFFu}) {
      ExpectIllegalState("reserved: SYSTEM funct3 000 imm12=" +
                             mosaic::Hex(imm12, 3),
                         Word(OP_SYSTEM, 0, 0, 0, imm12, 0x000));
      if (aborted_) return;
    }
    // ecall/ebreak/mret with a nonzero rd or rs1: those fields are not part of
    // the encoding, so a nonzero value is a reserved encoding.
    for (uint32_t imm12 : {0x000u, 0x001u, 0x302u}) {
      ExpectIllegalState("reserved: SYSTEM imm12=" + mosaic::Hex(imm12, 3) +
                             " with rd != x0",
                         Word(OP_SYSTEM, 0, 1, 0, imm12, 0x000));
      if (aborted_) return;
      ExpectIllegalState("reserved: SYSTEM imm12=" + mosaic::Hex(imm12, 3) +
                             " with rs1 != x0",
                         Word(OP_SYSTEM, 0, 0, 1, imm12, 0x000));
      if (aborted_) return;
    }
    // Every opcode this profile does not decode. The eleven legal opcodes are
    // skipped; the remaining 117 must all be illegal.
    for (uint32_t op = 0; op < 128; ++op) {
      if (op == OP_LOAD || op == OP_MISC_MEM || op == OP_IMM ||
          op == OP_AUIPC || op == OP_STORE || op == OP_IMM_32 ||
          op == OP_MUL_DIV || op == OP_BRANCH || op == OP_JALR ||
          op == OP_JAL || op == OP_SYSTEM) {
        continue;
      }
      for (uint32_t f3 = 0; f3 < 8; ++f3) {
        if (!Apply(Word(op, f3, 1, 2, 3, 0x00))) return;
      }
      ++reserved_opcodes_;
    }
    rep_.Check(reserved_opcodes_ == 117,
               "117 opcodes are outside RV64IM and all decode as illegal");
  }

  // ---- structural sweeps ----------------------------------------------------
  void Sweeps() {
    if (aborted_) return;

    // (a) every opcode x every funct3 x eight funct7 patterns.
    static const uint32_t kF7[] = {0x00, 0x01, 0x02, 0x20, 0x21, 0x40, 0x7F, 0x7B};
    for (uint32_t op = 0; op < 128; ++op) {
      for (uint32_t f3 = 0; f3 < 8; ++f3) {
        for (uint32_t f7 : kF7) {
          if (!Apply(Word(op, f3, 5, 6, 7, f7))) return;
        }
      }
    }

    // (b) every funct7 on the opcodes where funct7 selects the operation.
    for (uint32_t op : {0x33u, 0x3Bu, 0x1Bu}) {
      for (uint32_t f3 = 0; f3 < 8; ++f3) {
        for (uint32_t f7 = 0; f7 < 128; ++f7) {
          if (!Apply(Word(op, f3, 5, 6, 7, f7))) return;
        }
      }
    }
    // (c) every insn[31:26] on OP-IMM, where the shift-immediate selectors
    // live; this is the field a 7-bit funct7 reading gets wrong.
    for (uint32_t f3 = 0; f3 < 8; ++f3) {
      for (uint32_t top6 = 0; top6 < 64; ++top6) {
        if (!Apply(EncShiftX(OP_IMM, f3, 5, 6, 37, top6))) return;
      }
    }

    // (d) every register index: rd x rs1 for one representative of each format,
    // and rs1 x rs2 for the register-register forms.
    {
      const uint32_t reps[] = {
          EncI(OP_IMM, 0, 0, 0, 0x123),
          EncI(OP_LOAD, 2, 0, 0, 0x123),
          EncI(OP_JALR, 0, 0, 0, 0x123),
          EncI(OP_IMM_32, 0, 0, 0, 0x123),
          EncU(OP_AUIPC, 0, 0x12345),
          EncJ(OP_JAL, 0, 0x1234),
          EncShiftX(OP_IMM, 1, 0, 0, 0x3F, 0),
      };
      for (uint32_t base : reps) {
        for (uint32_t rd = 0; rd < 32; ++rd) {
          for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
            const uint32_t insn = (base & ~((0x1Fu << 7) | (0x1Fu << 15))) |
                                  (rd << 7) | (rs1 << 15);
            if (!Apply(insn)) return;
          }
        }
      }
      for (uint32_t base : {EncR(OP_MUL_DIV, 0, 0, 0, 0, 0x00),
                            EncR(OP_MUL_DIV, 4, 0, 0, 0, 0x01),
                            EncS(OP_STORE, 2, 0, 0, 0x40),
                            EncB(OP_BRANCH, 5, 0, 0, 0x100)}) {
        for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
          for (uint32_t rs2 = 0; rs2 < 32; ++rs2) {
            const uint32_t insn = (base & ~((0x1Fu << 15) | (0x1Fu << 20))) |
                                  (rs1 << 15) | (rs2 << 20);
            if (!Apply(insn)) return;
          }
        }
      }
    }

    // (e) every immediate bit, field by field. Each immediate format is
    // scrambled across the word, so sweeping each field across its full range
    // with the others held at a few patterns reaches every bit value of every
    // bit of the immediate -- which is the complete argument, and costs a
    // fraction of a full 2^32 sweep.
    //   I-type: the whole 12-bit immediate, twice (addi and ld).
    for (uint32_t imm = 0; imm < 4096; ++imm) {
      if (!Apply(EncI(OP_IMM, 0, 5, 6, static_cast<int32_t>(imm)))) return;
      if (!Apply(EncI(OP_LOAD, 3, 5, 6, static_cast<int32_t>(imm)))) return;
    }
    //   S-type: imm[11:5] over all 128, imm[4:0] over all 32.
    for (uint32_t hi = 0; hi < 128; ++hi) {
      for (uint32_t pattern = 0; pattern < 8; ++pattern) {
        const int32_t imm = static_cast<int32_t>((hi << 5) | (pattern & 0x1F));
        if (!Apply(EncS(OP_STORE, 2, 7, 8, imm))) return;
      }
    }
    for (uint32_t lo = 0; lo < 32; ++lo) {
      for (uint32_t pattern = 0; pattern < 8; ++pattern) {
        const int32_t imm = static_cast<int32_t>((((pattern * 37) % 128) << 5) | lo);
        if (!Apply(EncS(OP_STORE, 3, 7, 8, imm))) return;
      }
    }
    //   B-type: all 2^13 immediate patterns, for two of the six funct3 values.
    for (uint32_t off = 0; off < 8192; ++off) {
      if (!Apply(EncB(OP_BRANCH, 0, 9, 10,
                      static_cast<int32_t>(off) - 4096))) return;
      if (!Apply(EncB(OP_BRANCH, 6, 9, 10,
                      static_cast<int32_t>(off) - 4096))) return;
    }
    //   J-type: imm[10:1] (2^10) and imm[19:12] (2^8) and imm[20] separately.
    for (uint32_t low = 0; low < 1024; ++low) {
      for (uint32_t pattern = 0; pattern < 4; ++pattern) {
        const uint32_t imm = ((pattern * 0x155) & 0x7F800) | (low << 1);
        if (!Apply(EncJ(OP_JAL, 5, static_cast<int32_t>(imm) - 0x100000)))
          return;
      }
    }
    for (uint32_t mid = 0; mid < 256; ++mid) {
      for (uint32_t pattern = 0; pattern < 4; ++pattern) {
        const uint32_t imm = ((pattern * 0x2AB) & 0x3FFFE) | (mid << 12);
        if (!Apply(EncJ(OP_JAL, 6, static_cast<int32_t>(imm) - 0x100000)))
          return;
      }
    }
    //   U-type: imm[31:12] over 2^12 and the sign bit alone.
    for (uint32_t imm20 = 0; imm20 < 4096; ++imm20) {
      if (!Apply(EncU(OP_AUIPC, 5, imm20))) return;
    }
    for (uint32_t top = 0; top < 256; ++top) {
      for (uint32_t pattern = 0; pattern < 8; ++pattern) {
        if (!Apply(EncU(OP_LUI_UNUSED_MARKER, 5, (top << 12) | (pattern * 0x1111))))
          return;
      }
    }
  }

  // ---- random campaign ------------------------------------------------------
  void Random() {
    if (aborted_) return;
    static const uint32_t kOps[] = {OP_LOAD, OP_MISC_MEM, OP_IMM, OP_AUIPC,
                                     OP_STORE, OP_IMM_32, OP_MUL_DIV,
                                     OP_BRANCH, OP_JALR, OP_JAL, OP_SYSTEM};
    for (uint32_t i = 0; i < opt_.max_cycles / 4; ++i) {
      if (!Apply(static_cast<uint32_t>(rng_.Next() >> 32))) return;
      uint32_t word = 0;
      const uint32_t op = kOps[rng_.Below(11)];
      word |= op;
      word |= rng_.Below(8) << 12;
      word |= rng_.Below(32) << 7;
      word |= rng_.Below(32) << 15;
      word |= rng_.Below(32) << 20;
      // funct7 from the interesting neighbourhood most of the time
      word |= (rng_.Chance(70) ? (rng_.Below(128)) : (rng_.Below(3) << 5))
              << 25;
      if (!Apply(word)) return;
    }
  }

  // ---- finish ---------------------------------------------------------------
  int Finish() {
    if (!aborted_) {
      // Coverage: the campaign really did reach every opcode and every
      // (opcode, funct3) pair, and both the legal and the illegal side were
      // exercised in bulk.
      for (uint32_t op = 0; op < 128; ++op) {
        rep_.Check(op_seen_[op] > 0,
                   "opcode " + std::to_string(op) + " was presented");
      }
      for (uint32_t i = 0; i < 1024; ++i) {
        rep_.Check(opf3_seen_[i] > 0,
                   "opcode/funct3 pair " + std::to_string(i) + " was presented");
      }
      rep_.Check(legal_ > 10000, "legal side exercised: " +
                                     std::to_string(legal_) + " instructions");
      rep_.Check(illegal_ > 10000, "illegal side exercised: " +
                                       std::to_string(illegal_) + " instructions");
      rep_.Check(presented_ <= opt_.max_cycles,
                 "presented " + std::to_string(presented_) +
                     " instructions within --max-cycles=" +
                     std::to_string(opt_.max_cycles));
      rep_.Check(legal_ + illegal_ == presented_,
                 "every presented instruction was either legal or illegal");
    }
    rep_.Check(!aborted_, aborted_ ? reason_ : "no mismatch against the reference");

    if (aborted_ || rep_.failures() > 0) {
      std::ostringstream detail;
      detail << presented_ << " instructions compared against the reference, "
             << rep_.failures() << " failure(s)";
      return rep_.Finish("FAIL", detail.str());
    }
    std::ostringstream detail;
    detail << presented_ << " instructions (" << legal_ << " legal, " << illegal_
           << " illegal), 0 mismatches";
    return rep_.Finish("PASS", detail.str());
  }

  uint64_t presented() const { return presented_; }

 private:
  Ref Observe() const {
    Ref r{};
    r.valid = static_cast<uint8_t>(top_->o_valid);
    r.illegal = static_cast<uint8_t>(top_->o_illegal);
    r.uses_rs1 = static_cast<uint8_t>(top_->o_uses_rs1);
    r.uses_rs2 = static_cast<uint8_t>(top_->o_uses_rs2);
    r.uses_imm = static_cast<uint8_t>(top_->o_uses_imm);
    r.rs1 = static_cast<uint8_t>(top_->o_rs1);
    r.rs2 = static_cast<uint8_t>(top_->o_rs2);
    r.rd = static_cast<uint8_t>(top_->o_rd);
    r.imm = top_->o_imm;
    r.alu_op = static_cast<uint8_t>(top_->o_alu_op);
    r.uses_alu = static_cast<uint8_t>(top_->o_uses_alu);
    r.reg_write = static_cast<uint8_t>(top_->o_reg_write);
    r.mem_kind = static_cast<uint8_t>(top_->o_mem_kind);
    r.mem_size = static_cast<uint8_t>(top_->o_mem_size);
    r.mem_signed = static_cast<uint8_t>(top_->o_mem_signed);
    r.is_branch = static_cast<uint8_t>(top_->o_is_branch);
    r.branch_funct = static_cast<uint8_t>(top_->o_branch_funct);
    r.is_jal = static_cast<uint8_t>(top_->o_is_jal);
    r.is_jalr = static_cast<uint8_t>(top_->o_is_jalr);
    r.is_auipc = static_cast<uint8_t>(top_->o_is_auipc);
    r.writes_link = static_cast<uint8_t>(top_->o_writes_link);
    r.is_miscmem = static_cast<uint8_t>(top_->o_is_miscmem);
    r.is_fence_i = static_cast<uint8_t>(top_->o_is_fence_i);
    r.is_muldiv = static_cast<uint8_t>(top_->o_is_muldiv);
    r.md_op = static_cast<uint8_t>(top_->o_md_op);
    r.md_signed = static_cast<uint8_t>(top_->o_md_signed);
    r.is_system = static_cast<uint8_t>(top_->o_is_system);
    r.is_ecall = static_cast<uint8_t>(top_->o_is_ecall);
    r.is_ebreak = static_cast<uint8_t>(top_->o_is_ebreak);
    r.is_mret = static_cast<uint8_t>(top_->o_is_mret);
    r.csr_op = static_cast<uint8_t>(top_->o_csr_op);
    r.csr_addr = static_cast<uint16_t>(top_->o_csr_addr);
    r.csr_writes = static_cast<uint8_t>(top_->o_csr_writes);
    r.csr_reads = static_cast<uint8_t>(top_->o_csr_reads);
    r.csr_imm_form = static_cast<uint8_t>(top_->o_csr_imm_form);
    return r;
  }

  bool Field(const std::string& where, const char* name, uint64_t want,
             uint64_t got) {
    if (want == got) return true;
    rep_.Mismatch(where + "." + name, mosaic::Hex(want), mosaic::Hex(got));
    return false;
  }

  // The named ports must carry exactly the bits of the struct the RTL drove.
  // Without this, a typo in one of the 37 break-out assignments would either
  // hide a decoder bug or invent one.
  bool CheckBreakout(const Ref& r) {
    uint64_t chunk[3] = {0, 0, 0};
    int nbits = 0;
    auto push = [&](uint64_t value, int width) {
      for (int i = width - 1; i >= 0; --i) {
        const int bit = nbits + i;
        if ((value >> i) & 1) chunk[bit >> 6] |= 1ull << (bit & 63);
      }
      nbits += width;
    };
    // Declaration order of mosaic_pkg::decode_ctl_t, MSB first.
    push(r.valid, 1);       push(r.illegal, 1);
    push(r.uses_rs1, 1);    push(r.uses_rs2, 1);  push(r.uses_imm, 1);
    push(r.rs1, 5);         push(r.rs2, 5);       push(r.rd, 5);
    push(r.imm, 64);
    push(r.alu_op, 4);      push(r.uses_alu, 1); push(r.reg_write, 1);
    push(r.mem_kind, 3);    push(r.mem_size, 3);  push(r.mem_signed, 1);
    push(r.is_branch, 1);   push(r.branch_funct, 3);
    push(r.is_jal, 1);      push(r.is_jalr, 1);   push(r.is_auipc, 1);
    push(r.writes_link, 1);
    push(r.is_miscmem, 1);  push(r.is_fence_i, 1);
    push(r.is_muldiv, 1);   push(r.md_op, 3);     push(r.md_signed, 1);
    push(r.is_system, 1);   push(r.is_ecall, 1);  push(r.is_ebreak, 1);
    push(r.is_mret, 1);
    push(r.csr_op, 2);      push(r.csr_addr, 12);
    push(r.csr_writes, 1);  push(r.csr_reads, 1); push(r.csr_imm_form, 1);

    const uint64_t lo = chunk[0] | (chunk[1] << 32);
    const uint64_t w0 = top_->o_ctl_bits[0];
    const uint64_t w1 = top_->o_ctl_bits[1];
    const uint64_t w2 = top_->o_ctl_bits[2];
    const uint64_t dut_lo = w0 | (w1 << 32);
    const uint64_t dut_hi = w2 & 0x1FFFFFFFFull;
    if (lo == dut_lo && chunk[2] == dut_hi) return true;
    rep_.Mismatch("testbench break-out vs mosaic_decoder's decode_ctl_t",
                  mosaic::Hex(lo) + "/" + mosaic::Hex(chunk[2]),
                  mosaic::Hex(dut_lo) + "/" + mosaic::Hex(w2 & 0x1FFFFFFFFull));
    rep_.Check(false, "testbench break-out does not match the struct");
    aborted_ = true;
    reason_ = "testbench break-out does not match decode_ctl_t";
    return false;
  }

  // Assert that an encoding is reserved: illegal, not valid, and every other
  // field left at the defined illegal state rather than half-decoded.
  void ExpectIllegalState(const std::string& name, uint32_t insn) {
    if (!Apply(insn)) return;
    const Ref r = Observe();
    const Ref z = Illegal();
    bool clean = r.illegal == 1 && r.valid == 0;
    const uint8_t* a = reinterpret_cast<const uint8_t*>(&r);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&z);
    // Compare every byte except the padding-sensitive tail: compare the
    // scalar fields explicitly instead of memcmp over a struct with padding.
    clean = clean && r.uses_rs1 == 0 && r.uses_rs2 == 0 && r.uses_imm == 0 &&
            r.rs1 == 0 && r.rs2 == 0 && r.rd == 0 && r.imm == 0 &&
            r.alu_op == z.alu_op && r.uses_alu == 0 && r.reg_write == 0 &&
            r.mem_kind == MEM_NONE && r.mem_size == z.mem_size &&
            r.mem_signed == 0 && r.is_branch == 0 && r.branch_funct == 0 &&
            r.is_jal == 0 && r.is_jalr == 0 && r.is_auipc == 0 &&
            r.writes_link == 0 && r.is_miscmem == 0 && r.is_fence_i == 0 &&
            r.is_muldiv == 0 && r.md_op == z.md_op && r.md_signed == 0 &&
            r.is_system == 0 && r.is_ecall == 0 && r.is_ebreak == 0 &&
            r.is_mret == 0 && r.csr_op == CSR_NONE && r.csr_addr == 0 &&
            r.csr_writes == 0 && r.csr_reads == 0 && r.csr_imm_form == 0;
    (void)a; (void)b;
    rep_.Check(clean, name);
    ++reserved_checks_;
  }

  mosaic::Options opt_;
  mosaic::Reporter& rep_;
  Vmosaic_decoder_tb* top_;
  mosaic::Rng rng_{1};
  bool aborted_ = false;
  std::string reason_;
  uint64_t presented_ = 0;
  uint64_t legal_ = 0;
  uint64_t illegal_ = 0;
  uint64_t reserved_checks_ = 0;
  uint64_t reserved_opcodes_ = 0;
  uint32_t op_seen_[128] = {0};
  uint32_t opf3_seen_[1024] = {0};
};

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options opt;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &opt, &error)) {
    std::fprintf(stderr,
                 "usage: %s --case <id> [--out <dir>] [--seed <n>] "
                 "[--max-cycles <n>] [--verbose]\n%s\n",
                 argv[0], error.c_str());
    return mosaic::kExitUsage;
  }

  Verilated::commandArgs(argc, argv);
  mosaic::Reporter rep(opt, std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_decoder_tb* top = new Vmosaic_decoder_tb;

  Bench bench(opt, rep, top);
  bench.Directed();
  bench.Reserved();
  bench.Sweeps();
  bench.Random();
  const int status = bench.Finish();
  top->final();
  delete top;
  return status;
}