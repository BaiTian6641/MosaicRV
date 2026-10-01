// CASE=decode.rv64im_reserved -- work package I-010.
//
// Drives rtl/core/mosaic_decoder.sv and compares every field of its
// `decode_ctl_t` against an INDEPENDENT C++ reference decoder written from the
// RISC-V ISA manual, plus named assertions about the encodings the card calls
// out by name.
//
// Independence is the whole point of this file. `DecodeRef()` below is written
// from the instruction-format diagrams in volume I of the manual, with its own
// immediate assemblers and its own legality predicates; it shares no code, no
// expressions and no control structure with the RTL. Where the two disagree,
// one of them is wrong and the run says which instruction, which field and both
// values. Before the comparison was trusted, both the reference and the RTL
// were cross-checked against `riscv64-elf-objdump -M no-aliases` over 92
// assembled instructions (results/reports/I-010-011-decode-alu.md) -- a third,
// independent source, so a shared misreading of the manual would also have to
// be shared with GNU.
//
// What runs, in order:
//
//   1. Directed  named checks for the encodings the card names: S-type with a
//                negative offset, a backwards B-type branch, a backwards
//                J-type jump, addiw of a value whose bit 31 is set, shift by 0,
//                shift by 63, the reserved shift by 64, an odd jalr offset, and
//                the CSR x0 read/write rules.
//   2. Reserved  every reserved encoding class, each named, asserted illegal
//                *and* asserted to leave the whole struct at its defined
//                illegal state rather than half-decoded.
//   3. Sweeps    structural completeness, one instruction per step:
//                  - all 128 opcodes x all 8 funct3 x 8 funct7 patterns
//                  - all 128 funct7 values on every funct7-sensitive opcode
//                  - all 64 insn[31:26] values on OP-IMM
//                  - every rd x rs1 pair and every rs1 x rs2 pair
//                  - every immediate bit, field by field, exhaustively
//   4. Random    a seeded campaign: half uniform 32-bit words, half words
//                biased onto the twelve RV64IM opcodes so the legal side gets
//                as much coverage as the illegal side.
//
// Every instruction presented is compared field by field. The first mismatch
// stops the run: a decoder that disagrees about one instruction is broken, and
// the rest of the campaign would only bury the diagnostic.
//
// The DUT is combinational, so there is no clock, no reset and no reset
// schedule, and `--max-cycles` bounds instruction words presented rather than
// clock cycles. One "step" is one `eval()`.

#include "sim_common.h"
#include "verilated.h"
#include "Vmosaic_decoder_tb.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <string>

namespace {

// ------------------------------------------------------- mosaic_pkg encodings
// Mirrors of rtl/core/mosaic_pkg.sv. The numbering is part of the contract
// between the decoder and the functional units, so the directed phase asserts
// every ALU operation by number: if the package were renumbered this case fails
// instead of silently agreeing with a renumbered decoder.
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
  OP_LOAD = 0x03, OP_MISC_MEM = 0x0F, OP_IMM = 0x13, OP_LUI = 0x37,
  OP_AUIPC = 0x17, OP_STORE = 0x23, OP_IMM_32 = 0x1B, OP_MUL_DIV = 0x33,
  OP_OP32 = 0x3B, OP_BRANCH = 0x63, OP_JALR = 0x67, OP_JAL = 0x6F,
  OP_SYSTEM = 0x73
};

// ------------------------------------------------------------------ Ref type
// One member per decode_ctl_t field, same names, so the comparison is a list of
// one-liners rather than a hand-maintained mapping that can drift.
struct Ref {
  uint8_t valid, illegal;
  uint8_t uses_rs1, uses_rs2, uses_imm;
  uint8_t rs1, rs2, rd;
  uint64_t imm;
  uint8_t alu_op, uses_alu, reg_write;
  uint8_t mem_kind, mem_size, mem_signed;
  uint8_t is_branch, branch_funct, is_jal, is_jalr, is_auipc, writes_link;
  uint8_t is_miscmem, is_fence_i;
  uint8_t is_muldiv, md_op, md_signed, md_w;
  uint8_t is_system, is_ecall, is_ebreak, is_mret;
  uint8_t csr_op;
  uint16_t csr_addr;
  uint8_t csr_writes, csr_reads, csr_imm_form;
};

// The single illegal state: every field a defined zero, illegal set. This is
// mosaic_decoder's CTL_ILLEGAL, restated here from the package rather than
// borrowed from the RTL.
Ref Illegal() {
  Ref r{};
  r.illegal = 1;
  r.alu_op = ALU_PASSB;   // the package's defined "no ALU operation" encoding
  return r;
}

// ------------------------------------------------------- reference immediates
// From the "RV32I instruction formats" diagrams. The three shapes that are easy
// to get wrong, each cross-checked against objdump in the report:
//   * I-type imm is insn[31:20] as ONE field -- insn[11:7] is rd.
//   * S-type imm is insn[31:25] || insn[11:7]: it borrows the bits that are rd
//     in an I-type, and rd in a store is therefore always x0.
//   * B-type and J-type are scrambled, and B/J imm[0] is hard-wired zero.
int64_t SignExtend(uint64_t value, int bits) {
  const uint64_t sign = 1ull << (bits - 1);
  return (value & sign)
             ? static_cast<int64_t>(value | ~((1ull << bits) - 1))
             : static_cast<int64_t>(value);
}

int64_t ImmI(uint32_t insn) { return SignExtend((insn >> 20) & 0xFFF, 12); }

int64_t ImmS(uint32_t insn) {
  return SignExtend(((((insn >> 25) & 0x7F) << 5) | ((insn >> 7) & 0x1F)), 12);
}

int64_t ImmB(uint32_t insn) {
  const uint32_t raw = (((insn >> 31) & 1) << 12) | (((insn >> 7) & 1) << 11) |
                       (((insn >> 25) & 0x3F) << 5) | (((insn >> 8) & 0xF) << 1);
  return SignExtend(raw, 13);
}

int64_t ImmU(uint32_t insn) { return SignExtend(insn & 0xFFFFF000u, 32); }

int64_t ImmJ(uint32_t insn) {
  const uint32_t raw = (((insn >> 31) & 1) << 20) |
                       (((insn >> 12) & 0xFF) << 12) |
                       (((insn >> 20) & 1) << 11) |
                       (((insn >> 21) & 0x3FF) << 1);
  return SignExtend(raw, 21);
}

// ---------------------------------------------------- reference: the decoder
// `DecodeFields` fills in the control fields for a legal instruction and leaves
// `illegal` set as the marker; `DecodeRef` turns that into the pair
// valid/illegal the DUT must report. Splitting it this way means no arm inside
// the switch can forget to set valid, which is exactly the mistake the first
// version of this file made.
struct Decoded {
  Ref ctl;
  bool legal;
};

Decoded DecodeFields(uint32_t insn) {
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
        case 0: r.mem_size = SZ_BYTE; r.mem_signed = 1; return {r, true};  // lb
        case 1: r.mem_size = SZ_HALF; r.mem_signed = 1; return {r, true};  // lh
        case 2: r.mem_size = SZ_WORD; r.mem_signed = 1; return {r, true};  // lw
        case 3: r.mem_size = SZ_DBL;  r.mem_signed = 1; return {r, true};  // ld
        case 4: r.mem_size = SZ_BYTE; r.mem_signed = 0; return {r, true};  // lbu
        case 5: r.mem_size = SZ_HALF; r.mem_signed = 0; return {r, true};  // lhu
        default: return {Illegal(), false};      // 110, 111 reserved
      }
    }
    case OP_MISC_MEM: {
      if (f3 == 0) { r.is_miscmem = 1; return {r, true}; }                    // fence
      if (f3 == 1) { r.is_miscmem = 1; r.is_fence_i = 1; return {r, true}; }  // fence.i
      return {Illegal(), false};
    }
    case OP_IMM: {
      r.uses_rs1 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.uses_alu = 1;
      r.imm = static_cast<uint64_t>(ImmI(insn));
      switch (f3) {
        case 0: r.alu_op = ALU_ADD;  return {r, true};   // addi
        case 2: r.alu_op = ALU_SLT;  return {r, true};   // slti
        case 3: r.alu_op = ALU_SLTU; return {r, true};   // sltiu
        case 4: r.alu_op = ALU_XOR;  return {r, true};   // xori
        case 6: r.alu_op = ALU_OR;   return {r, true};   // ori
        case 7: r.alu_op = ALU_AND;  return {r, true};   // andi
        case 1:                                    // slli
          if (top6 != 0) return {Illegal(), false};
          r.alu_op = ALU_SLL;
          r.imm = (insn >> 20) & 0x3F;
          return {r, true};
        case 5:                                    // srli / srai
          if (top6 == 0) {
            r.alu_op = ALU_SRL;
            r.imm = (insn >> 20) & 0x3F;
            return {r, true};
          }
          if (top6 == 0x10) {
            r.alu_op = ALU_SRA;
            r.imm = (insn >> 20) & 0x3F;
            return {r, true};
          }
          return {Illegal(), false};
        default: return {Illegal(), false};
      }
    }
    case OP_LUI: {
      // funct3 is not part of the encoding. No rs1, no rs2: in a U-type those
      // bits are imm[19:15] and imm[9:5].
      r.uses_imm = 1;
      r.imm = static_cast<uint64_t>(ImmU(insn));
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.alu_op = ALU_PASSB;   // the answer is the immediate itself
      r.uses_alu = 1;
      return {r, true};
    }
    case OP_AUIPC: {
      r.uses_imm = 1;
      r.imm = static_cast<uint64_t>(ImmU(insn));
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.is_auipc = 1;
      r.uses_alu = 1;
      r.alu_op = ALU_ADD;
      return {r, true};
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
        case 0: r.mem_size = SZ_BYTE; return {r, true};   // sb
        case 1: r.mem_size = SZ_HALF; return {r, true};   // sh
        case 2: r.mem_size = SZ_WORD; return {r, true};   // sw
        case 3: r.mem_size = SZ_DBL;  return {r, true};   // sd
        default: return {Illegal(), false};
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
        case 0: r.alu_op = ALU_ADDW; return {r, true};                          // addiw
        case 1:                                                          // slliw
          if (f7 != 0) return {Illegal(), false};
          r.alu_op = ALU_SLLW;
          r.imm = (insn >> 20) & 0x1F;
          return {r, true};
        case 5:                                                          // srliw/sraiw
          if (f7 == 0) {
            r.alu_op = ALU_SRLW;
            r.imm = (insn >> 20) & 0x1F;
            return {r, true};
          }
          if (f7 == 0x20) {
            r.alu_op = ALU_SRAW;
            r.imm = (insn >> 20) & 0x1F;
            return {r, true};
          }
          return {Illegal(), false};
        default: return {Illegal(), false};
      }
    }
    case OP_MUL_DIV: {
      r.uses_rs1 = 1;
      r.uses_rs2 = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rs2 = static_cast<uint8_t>(rs2);
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      if (f7 == 1) {                                     // M extension
        r.is_muldiv = 1;
        r.md_op = static_cast<uint8_t>(f3);
        r.md_signed = !(f3 == 3 || f3 == 5 || f3 == 7);
        return {r, true};
      }
      r.uses_alu = 1;
      switch (f3) {
        case 0:
          if (f7 == 0) r.alu_op = ALU_ADD;
          else if (f7 == 0x20) r.alu_op = ALU_SUB;
          else return {Illegal(), false};
          return {r, true};
        case 1:
          if (f7 != 0) return {Illegal(), false};
          r.alu_op = ALU_SLL;
          return {r, true};
        case 2:
          if (f7 != 0) return {Illegal(), false};
          r.alu_op = ALU_SLT;
          return {r, true};
        case 3:
          if (f7 != 0) return {Illegal(), false};
          r.alu_op = ALU_SLTU;
          return {r, true};
        case 4:
          if (f7 != 0) return {Illegal(), false};
          r.alu_op = ALU_XOR;
          return {r, true};
        case 5:
          if (f7 == 0) r.alu_op = ALU_SRL;
          else if (f7 == 0x20) r.alu_op = ALU_SRA;
          else return {Illegal(), false};
          return {r, true};
        case 6:
          if (f7 != 0) return {Illegal(), false};
          r.alu_op = ALU_OR;
          return {r, true};
        default:
          if (f7 != 0) return {Illegal(), false};
          r.alu_op = ALU_AND;
          return {r, true};
      }
    }
    case OP_OP32: {
      // The M extension's word forms. On RV64 only funct7 0000001 is defined,
      // and only funct3 000/100/101/110/111 are: the high-half multiplies have
      // no word form, so 001/010/011 are reserved. Every other funct7 on this
      // opcode is the RV32-only I word forms (addw subw sllw srlw sraw) and is
      // reserved on RV64 -- which is exactly what this core is.
      if (f7 != 1) return {Illegal(), false};
      if (f3 == 1 || f3 == 2 || f3 == 3) return {Illegal(), false};
      r.uses_rs1 = 1;
      r.uses_rs2 = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rs2 = static_cast<uint8_t>(rs2);
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.is_muldiv = 1;
      r.md_op = static_cast<uint8_t>(f3);
      r.md_signed = !(f3 == 3 || f3 == 5 || f3 == 7);
      r.md_w = 1;
      return {r, true};
    }
    case OP_BRANCH: {
      // beq 000, bne 001, blt 100, bge 101, bltu 110, bgeu 111; 010 and 011
      // are the reserved pair.
      if (f3 == 2 || f3 == 3) return {Illegal(), false};
      r.uses_rs1 = 1;
      r.uses_rs2 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rs2 = static_cast<uint8_t>(rs2);
      r.imm = static_cast<uint64_t>(ImmB(insn));
      r.is_branch = 1;
      r.branch_funct = static_cast<uint8_t>(f3);
      return {r, true};
    }
    case OP_JALR: {
      if (f3 != 0) return {Illegal(), false};
      r.uses_rs1 = 1;
      r.uses_imm = 1;
      r.rs1 = static_cast<uint8_t>(rs1);
      r.rd = static_cast<uint8_t>(rd);
      r.imm = static_cast<uint64_t>(ImmI(insn));
      r.reg_write = rd != 0;
      r.is_jalr = 1;
      r.writes_link = 1;
      return {r, true};
    }
    case OP_JAL: {
      // funct3 is not part of the encoding.
      r.uses_imm = 1;
      r.imm = static_cast<uint64_t>(ImmJ(insn));
      r.rd = static_cast<uint8_t>(rd);
      r.reg_write = rd != 0;
      r.is_jal = 1;
      r.writes_link = 1;
      return {r, true};
    }
    case OP_SYSTEM: {
      switch (f3) {
        case 0: {
          // funct3 000: rd and rs1 are not part of ecall/ebreak/mret.
          if (rd != 0 || rs1 != 0) return {Illegal(), false};
          if (imm12 == 0x000) { r.is_system = 1; r.is_ecall = 1; return {r, true}; }
          if (imm12 == 0x001) { r.is_system = 1; r.is_ebreak = 1; return {r, true}; }
          if (imm12 == 0x302) { r.is_system = 1; r.is_mret = 1; return {r, true}; }
          return {Illegal(), false};
        }
        case 1: case 2: case 3:                    // csrrw csrrs csrrc
        case 5: case 6: case 7: {                  // csrrwi csrrsi csrrci
          const bool imm_form = f3 >= 5;
          r.is_system = 1;
          r.csr_imm_form = imm_form ? 1 : 0;
          r.csr_addr = static_cast<uint16_t>(imm12);
          r.uses_rs1 = 1;
          r.rs1 = static_cast<uint8_t>(rs1);
          r.rd = static_cast<uint8_t>(rd);
          r.reg_write = rd != 0;
          r.csr_op = (f3 == 1 || f3 == 5) ? CSR_RW
                    : (f3 == 2 || f3 == 6) ? CSR_RS : CSR_RC;
          r.csr_writes = (r.csr_op == CSR_RW) || rs1 != 0;
          r.csr_reads = (r.csr_op != CSR_RW) || rd != 0;
          return {r, true};
        }
        default:
          return {Illegal(), false};      // funct3 100 is reserved
      }
    }
    default:
      return {Illegal(), false};          // every opcode this profile does not decode
  }
}

Ref DecodeRef(uint32_t insn) {
  const Decoded d = DecodeFields(insn);
  if (!d.legal) return d.ctl;
  Ref r = d.ctl;
  r.valid = 1;
  r.illegal = 0;
  return r;
}

// ------------------------------------------------------------------ encoders
// Written from the same manual diagrams, so a directed test builds the word it
// means to test rather than pasting a hex constant of unclear provenance.
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
uint32_t EncCsr(uint32_t f3, uint32_t rd, uint32_t src, uint32_t csr) {
  return OP_SYSTEM | (f3 << 12) | (rd << 7) | (src << 15) |
         ((csr & 0xFFF) << 20);
}
// Shift-immediate: shamt is 6 bits in insn[25:20], selector insn[31:26].
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
// Raw word from its fields, for the structural sweeps. All arguments must fit
// their fields: rs2 and f7 share bits 24:20 / 31:25.
uint32_t Word(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, uint32_t rs2,
              uint32_t f7) {
  return op | (f3 << 12) | (rd << 7) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}

// ------------------------------------------------------------------ the bench
class Bench {
 public:
  Bench(const mosaic::Options& options, mosaic::Reporter& reporter,
        Vmosaic_decoder_tb* top)
      : opt_(options), rep_(reporter), top_(top), rng_(options.seed) {}

  // Present one instruction word and compare every field. Returns false on the
  // first mismatch, which stops the run.
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
    if (want.valid) ++legal_;
    if (want.illegal) ++illegal_;
    const Ref got = Observe();

    const std::string where = "insn=" + mosaic::Hex(insn, 8);
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
    ok &= Field(where, "md_w", want.md_w, got.md_w);
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
      rep_.Mismatch(where, "the reference decoder", "the RTL decoder");
      aborted_ = true;
      reason_ = "decoder disagrees with the reference at " + where;
      rep_.Check(false, reason_);
      return false;
    }
    return CheckBreakout(got, where);
  }

  // ---- named directed checks ------------------------------------------------
  void Directed() {
    if (aborted_) return;
    Ref r = Illegal();

    // --- immediate formats, each at a value that cannot be confused ---------
    // S-type with a negative offset: insn[11:7] supplies imm[4:0] here, so an
    // I-type assembly of the same offset would read -32 instead of -16.
    if (!Apply(EncS(OP_STORE, 0, 5, 6, -16))) return;
    r = Observe();
    rep_.Check(r.imm == static_cast<uint64_t>(-16), "S-type offset -16");
    rep_.Check(r.rs2 == 5 && r.rs1 == 6, "S-type rs2/rs1");
    rep_.Check(r.rd == 0, "S-type has no rd: insn[11:7] is immediate, not rd");
    rep_.Check(r.uses_rs1 && r.uses_rs2 && r.uses_imm,
               "S-type uses rs1, rs2 and imm");
    rep_.Check(r.mem_kind == MEM_STORE && r.mem_size == SZ_BYTE, "sb");

    if (!Apply(EncS(OP_STORE, 3, 31, 7, -2048))) return;
    r = Observe();
    rep_.Check(r.imm == static_cast<uint64_t>(-2048), "sd offset -2048");
    rep_.Check(r.mem_size == SZ_DBL, "sd is a doubleword store");

    if (!Apply(EncS(OP_STORE, 2, 1, 2, 2047))) return;
    rep_.Check(Observe().imm == 2047, "sw offset +2047 (needs imm[11:5] all ones)");

    // B-type backwards branch. imm[12], imm[10:5] and imm[4:1] are three
    // different bit ranges of the word, so a swapped pair shows up here.
    if (!Apply(EncB(OP_BRANCH, 0, 13, 14, -4096))) return;
    r = Observe();
    rep_.Check(r.imm == static_cast<uint64_t>(-4096), "beq offset -4096 (max back)");
    rep_.Check(r.is_branch && r.branch_funct == 0, "beq");
    rep_.Check(r.rs1 == 14 && r.rs2 == 13, "B-type rs1/rs2");
    rep_.Check(r.rd == 0 && !r.reg_write, "branch writes no register");
    rep_.Check(!r.uses_alu, "branch does not use the ALU datapath");

    if (!Apply(EncB(OP_BRANCH, 4, 1, 2, -2))) return;
    r = Observe();
    rep_.Check(r.imm == static_cast<uint64_t>(-2) && r.branch_funct == 4,
               "blt offset -2: branch funct3 100");
    if (!Apply(EncB(OP_BRANCH, 7, 31, 30, 4094))) return;
    r = Observe();
    rep_.Check(r.imm == 4094 && r.branch_funct == 7,
               "bgeu offset +4094 (max forward)");

    // J-type backward jump: imm[20], imm[10:1], imm[11] and imm[19:12].
    if (!Apply(EncJ(OP_JAL, 1, -1048576))) return;
    r = Observe();
    rep_.Check(r.imm == static_cast<uint64_t>(-1048576),
               "jal offset -1048576 (most negative legal)");
    rep_.Check(r.is_jal && r.writes_link && r.reg_write, "jal writes the link");
    rep_.Check(r.rd == 1 && !r.uses_rs1 && !r.uses_rs2, "jal rd, no rs1/rs2");
    if (!Apply(EncJ(OP_JAL, 0, 1048574))) return;
    rep_.Check(Observe().imm == 1048574, "jal offset +1048574 (max forward)");
    if (!Apply(EncJ(OP_JAL, 31, -2))) return;
    r = Observe();
    rep_.Check(r.imm == static_cast<uint64_t>(-2),
               "jal offset -2: imm[20] and the sign bit set together");
    rep_.Check(r.reg_write && r.rd == 31, "jal with rd == x31 writes the link");
    if (!Apply(EncJ(OP_JAL, 0, -2))) return;
    rep_.Check(!Observe().reg_write,
               "jal with rd == x0 performs no register write");

    // addiw of a value whose bit 31 is set. The decoder selects ALU_ADDW, which
    // the ALU sign-extends; the immediate here is -1, i.e. every bit set, so an
    // operand-side 32-bit truncation would be visible at execution.
    if (!Apply(EncI(OP_IMM_32, 0, 9, 8, -1))) return;
    r = Observe();
    rep_.Check(r.alu_op == ALU_ADDW, "addiw selects ALU_ADDW, which sign-extends");
    rep_.Check(r.imm == UINT64_MAX, "addiw immediate -1 sign-extends to all ones");
    rep_.Check(r.rs1 == 8 && r.rd == 9, "addiw rs1/rd");
    rep_.Check(r.uses_alu && r.uses_imm, "addiw uses rs1 + imm");

    // Shifts: by zero, by 63, and the reserved shift by 64.
    if (!Apply(EncShiftX(OP_IMM, 1, 5, 6, 0, 0x00))) return;
    r = Observe();
    rep_.Check(r.alu_op == ALU_SLL && r.imm == 0, "slli by 0");
    if (!Apply(EncShiftX(OP_IMM, 1, 5, 6, 63, 0x00))) return;
    r = Observe();
    rep_.Check(r.alu_op == ALU_SLL && r.imm == 63,
               "slli by 63: insn[31:26] zero, 6-bit shamt");
    if (!Apply(EncShiftX(OP_IMM, 1, 5, 6, 0, 0x01))) return;
    ExpectIllegalState("slli by 64 is reserved on RV64");
    if (!Apply(EncShiftX(OP_IMM, 5, 5, 6, 63, 0x10))) return;
    r = Observe();
    rep_.Check(r.alu_op == ALU_SRA && r.imm == 63,
               "srai by 63: funct3 101 with insn[31:26] = 010000");
    if (!Apply(EncShiftX(OP_IMM, 5, 5, 6, 31, 0x00))) return;
    rep_.Check(Observe().alu_op == ALU_SRL, "srli by 31: funct3 101, top6 zero");
    if (!Apply(EncShiftX(OP_IMM, 5, 5, 6, 0, 0x10))) return;
    rep_.Check(Observe().imm == 0, "srai by 0 is not the same as srli by 0 in op");
    if (!Apply(EncShiftW(OP_IMM_32, 1, 5, 6, 31, 0x00))) return;
    r = Observe();
    rep_.Check(r.alu_op == ALU_SLLW && r.imm == 31,
               "slliw by 31: 5-bit shamt in insn[24:20]");
    if (!Apply(EncShiftW(OP_IMM_32, 5, 5, 6, 31, 0x20))) return;
    rep_.Check(Observe().alu_op == ALU_SRAW, "sraiw by 31");
    if (!Apply(EncShiftW(OP_IMM_32, 5, 5, 6, 0, 0x01))) return;
    ExpectIllegalState("srliw with funct7 0000001 is reserved");

    // jalr: the decoder exposes pc + rs1 + imm and clears nothing. An odd
    // immediate must survive intact -- clearing target bit 0 is the core's job
    // (I-011), because that is a target-computation rule.
    if (!Apply(EncI(OP_JALR, 0, 1, 2, 3))) return;
    r = Observe();
    rep_.Check(r.is_jalr && r.writes_link && r.reg_write, "jalr links");
    rep_.Check(r.imm == 3, "jalr immediate 3 survives: the core clears the target");
    rep_.Check(r.rs1 == 2 && r.uses_rs1 && r.uses_imm, "jalr uses rs1 + imm");
    rep_.Check(!r.uses_alu, "jalr does not use the ALU datapath");
    if (!Apply(EncI(OP_JALR, 0, 1, 2, -2048))) return;
    rep_.Check(Observe().imm == static_cast<uint64_t>(-2048), "jalr -2048");

    // lui / auipc: U-type, no rs1, no rs2, immediate low 12 bits hard-wired zero.
    if (!Apply(EncU(OP_LUI, 3, 0xABCDE))) return;
    r = Observe();
    rep_.Check(r.alu_op == ALU_PASSB && r.uses_alu && r.uses_imm,
               "lui passes the U-type immediate through the ALU");
    rep_.Check(r.imm == 0xFFFFFFFFABCDE000ull,
               "lui imm, low 12 bits zero and imm[31] sign-extended");
    rep_.Check(!r.uses_rs1 && !r.uses_rs2 && r.rd == 3,
               "lui insn[19:15] and insn[24:20] are immediate, not registers");
    if (!Apply(EncU(OP_AUIPC, 3, 0xABCDE))) return;
    r = Observe();
    rep_.Check(r.is_auipc && r.alu_op == ALU_ADD && r.uses_imm, "auipc");
    rep_.Check(r.imm == 0xFFFFFFFFABCDE000ull, "auipc imm");
    if (!Apply(EncU(OP_AUIPC, 3, 0x80000))) return;
    rep_.Check(Observe().imm == 0xFFFFFFFF80000000ull, "auipc sign-extends imm[31]");
    if (!Apply(EncU(OP_AUIPC, 3, 0xFFFFF))) return;
    rep_.Check(Observe().imm == 0xFFFFFFFFFFFFF000ull,
               "auipc with imm[31:12] all ones");

    // Every legal ALU operation, by number, so a renumbering of mosaic_pkg's
    // alu_op_e cannot pass unnoticed. The RV32 word forms are absent on purpose.
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
        rep_.Check(Observe().alu_op == c.op && Observe().uses_alu,
                   std::string("alu_op of ") + c.name);
      }
    }

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
        r = Observe();
        rep_.Check(r.mem_size == c.size && r.mem_signed == c.sign,
                   std::string("memory width and signedness of ") + c.name);
        rep_.Check(r.uses_alu && r.alu_op == ALU_ADD,
                   std::string(c.name) + " computes rs1 + imm in the ALU");
        rep_.Check(r.uses_rs1 && r.uses_imm,
                   std::string(c.name) + " uses the address immediate");
      }
    }

    // M extension: md_op and md_signed for all eight.
    {
      struct { const char* name; uint32_t f3; uint8_t op; uint8_t sign; } m[] = {
          {"mul", 0, MD_MUL, 1},         {"mulh", 1, MD_MULH, 1},
          {"mulhsu", 2, MD_MULHSU, 1},  {"mulhu", 3, MD_MULHU, 0},
          {"div", 4, MD_DIV, 1},         {"divu", 5, MD_DIVU, 0},
          {"rem", 6, MD_REM, 1},         {"remu", 7, MD_REMU, 0},
      };
      for (const auto& c : m) {
        if (!Apply(EncR(OP_MUL_DIV, c.f3, 4, 5, 6, 0x01))) return;
        r = Observe();
        rep_.Check(r.is_muldiv && r.md_op == c.op && r.md_signed == c.sign,
                   std::string("M classification of ") + c.name);
        rep_.Check(!r.uses_alu && r.uses_rs1 && r.uses_rs2 && r.reg_write,
                   std::string(c.name) + " reads rs1 and rs2 and writes rd");
        rep_.Check(!r.md_w, std::string(c.name) + " (64-bit) leaves md_w clear");
      }
    }

    // M extension WORD forms: OP-32 with funct7 0000001. The five encodings the
    // ISA defines decode with md_w set and the same md_op/md_signed as their
    // 64-bit counterparts. All eight funct3 values are walked here, so the
    // three that are reserved (001/010/011 -- there is no mulhw/mulhsuw/mulhuw)
    // are pinned as reserved next to the five that are legal, and the RV32-only
    // I word forms on the same opcode are the negative neighbour.
    {
      struct { const char* name; uint32_t f3; uint8_t op; uint8_t sign; } w[] = {
          {"mulw", 0, MD_MUL, 1},   {"divw", 4, MD_DIV, 1},
          {"divuw", 5, MD_DIVU, 0}, {"remw", 6, MD_REM, 1},
          {"remuw", 7, MD_REMU, 0},
      };
      for (const auto& c : w) {
        if (!Apply(EncR(OP_OP32, c.f3, 4, 5, 6, 0x01))) return;
        r = Observe();
        rep_.Check(r.is_muldiv && r.md_w && r.md_op == c.op &&
                       r.md_signed == c.sign,
                   std::string("W classification of ") + c.name);
        rep_.Check(!r.uses_alu && r.uses_rs1 && r.uses_rs2 && r.reg_write,
                   std::string(c.name) + " reads rs1 and rs2 and writes rd");
      }
      // The high-half word encodings the ISA does not define.
      for (uint32_t f3 : {1u, 2u, 3u}) {
        if (!Apply(EncR(OP_OP32, f3, 4, 5, 6, 0x01))) return;
        ExpectIllegalState("reserved: no MULH*W form (OP-32 funct3 " +
                           std::to_string(f3) + " with funct7 0000001)");
      }
      // The negative neighbour: addw and subw are the RV32-only I word forms
      // and are reserved on RV64, on the very opcode the W forms live on.
      if (!Apply(EncR(OP_OP32, 0, 4, 5, 6, 0x00))) return;
      ExpectIllegalState("reserved on RV64: addw");
      if (!Apply(EncR(OP_OP32, 0, 4, 5, 6, 0x20))) return;
      ExpectIllegalState("reserved on RV64: subw");
    }

    // CSR x0 rules: the two rules a decoder most often gets wrong, because both
    // look like they belong to the CSR unit rather than here.
    if (!Apply(EncCsr(1, 0, 0, 0x300))) return;              // csrrw x0, mstatus, x0
    r = Observe();
    rep_.Check(r.csr_writes, "csrrw always writes the CSR");
    rep_.Check(!r.csr_reads, "csrrw with rd == x0 does not read the CSR");
    rep_.Check(r.csr_op == CSR_RW && r.csr_addr == 0x300, "csrrw csr_op/csr_addr");
    rep_.Check(!r.reg_write, "csrrw x0 writes no register");
    rep_.Check(r.is_system && !r.csr_imm_form, "csrrw is a register form");
    rep_.Check(r.uses_rs1 && r.rs1 == 0, "csrrw uses rs1, which is x0 here");
    if (!Apply(EncCsr(1, 1, 0, 0x300))) return;              // csrrw x1, mstatus, x0
    r = Observe();
    rep_.Check(r.csr_writes && r.csr_reads, "csrrw with rd != x0 reads and writes");
    if (!Apply(EncCsr(2, 1, 0, 0x340))) return;              // csrrs x1, mscratch, x0
    r = Observe();
    rep_.Check(!r.csr_writes, "csrrs with rs1 == x0 does not write the CSR");
    rep_.Check(r.csr_reads, "csrrs always reads the CSR");
    rep_.Check(r.csr_op == CSR_RS && r.reg_write, "csrrs csr_op, and it writes rd");
    if (!Apply(EncCsr(3, 1, 0, 0x340))) return;              // csrrc x1, mscratch, x0
    r = Observe();
    rep_.Check(!r.csr_writes && r.csr_reads,
               "csrrc with rs1 == x0 does not write the CSR");
    if (!Apply(EncCsr(2, 1, 5, 0x340))) return;              // csrrs x1, mscratch, x5
    rep_.Check(Observe().csr_writes, "csrrs with rs1 != x0 writes the CSR");
    if (!Apply(EncCsr(5, 0, 0, 0x340))) return;              // csrrwi x0, mscratch, 0
    r = Observe();
    rep_.Check(r.csr_imm_form && !r.csr_reads,
               "csrrwi with rd == x0 does not read the CSR");
    rep_.Check(r.csr_writes, "csrrwi always writes the CSR, whatever the zimm");
    if (!Apply(EncCsr(6, 1, 31, 0xC00))) return;             // csrrsi x1, cycle, 31
    r = Observe();
    rep_.Check(r.csr_imm_form && r.csr_writes && r.csr_reads,
               "csrrsi with zimm != 0 writes and reads");
    rep_.Check(r.csr_op == CSR_RS && r.csr_addr == 0xC00, "csrrsi csr_op/csr_addr");
    if (!Apply(EncCsr(7, 1, 17, 0x180))) return;             // csrrci x1, satp, 17
    r = Observe();
    rep_.Check(r.csr_op == CSR_RC && r.csr_addr == 0x180, "csrrci csr_op/csr_addr");

    // ecall / ebreak / mret, and the registers they do not have.
    if (!Apply(0x00000073u)) return;
    r = Observe();
    rep_.Check(r.is_system && r.is_ecall && !r.is_ebreak && !r.is_mret, "ecall");
    rep_.Check(!r.reg_write && !r.uses_rs1 && !r.uses_rs2, "ecall reads nothing");
    if (!Apply(0x00100073u)) return;
    rep_.Check(Observe().is_system && Observe().is_ebreak && !Observe().is_ecall,
               "ebreak");
    if (!Apply(0x30200073u)) return;
    rep_.Check(Observe().is_system && Observe().is_mret, "mret");

    // fence / fence.i
    if (!Apply(0x0FF0000Fu)) return;
    r = Observe();
    rep_.Check(r.is_miscmem && !r.is_fence_i, "fence");
    if (!Apply(0x0000100Fu)) return;
    r = Observe();
    rep_.Check(r.is_miscmem && r.is_fence_i, "fence.i");

    // x0 discipline: anything that can write rd must report reg_write 0 when rd
    {
      const uint32_t words[] = {
          EncI(OP_IMM, 0, 0, 2, 4),      EncI(OP_IMM_32, 0, 0, 2, 4),
          EncU(OP_AUIPC, 0, 0x123),      EncJ(OP_JAL, 0, 4),
          EncI(OP_JALR, 0, 0, 2, 4),     EncI(OP_LOAD, 2, 0, 2, 4),
          EncR(OP_MUL_DIV, 0, 0, 1, 2, 0x00), EncR(OP_MUL_DIV, 1, 0, 1, 2, 0x01),
          EncCsr(1, 0, 1, 0x300),
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
      if (!Apply(Word(OP_LOAD, f3, 1, 2, 0, 0x00))) return;
      ExpectIllegalState("reserved: LOAD funct3 " + std::to_string(f3));
    }
    // MISC-MEM funct3 010..111: only fence (000) and fence.i (001) exist.
    for (uint32_t f3 = 2; f3 < 8; ++f3) {
      if (!Apply(Word(OP_MISC_MEM, f3, 1, 2, 3, 0x0F))) return;
      ExpectIllegalState("reserved: MISC-MEM funct3 " + std::to_string(f3));
    }
    // OP-IMM slli with any nonzero insn[31:26] -- this is where "shift by 64"
    // lives on RV64.
    for (uint32_t top6 = 1; top6 < 64; ++top6) {
      if (!Apply(EncShiftX(OP_IMM, 1, 1, 2, 3, top6))) return;
      ExpectIllegalState("reserved: slli with insn[31:26]=" + std::to_string(top6));
    }
    // OP-IMM funct3 101 with insn[31:26] neither 000000 nor 010000.
    for (uint32_t top6 = 0; top6 < 64; ++top6) {
      if (top6 == 0 || top6 == 0x10) continue;
      if (!Apply(EncShiftX(OP_IMM, 5, 1, 2, 3, top6))) return;
      ExpectIllegalState("reserved: srli/srai selector insn[31:26]=" +
                         std::to_string(top6));
    }
    // STORE funct3 100..111: RV64I has no other store width.
    for (uint32_t f3 = 4; f3 < 8; ++f3) {
      if (!Apply(Word(OP_STORE, f3, 0, 1, 2, 0x00))) return;
      ExpectIllegalState("reserved: STORE funct3 " + std::to_string(f3));
    }
    // OP: every funct7 outside {0000000, 0100000, 0000001}, for all funct3.
    for (uint32_t f7 = 2; f7 < 128; ++f7) {
      if (f7 == 0x20) continue;
      for (uint32_t f3 = 0; f3 < 8; ++f3) {
        if (!Apply(Word(OP_MUL_DIV, f3, 1, 2, 3, f7))) return;
        ExpectIllegalState("reserved: OP funct7=" + std::to_string(f7) +
                           " funct3=" + std::to_string(f3));
      }
    }
    // OP-IMM-32: funct3 outside {000, 001, 101}.
    for (uint32_t f3 = 0; f3 < 8; ++f3) {
      if (f3 == 0 || f3 == 1 || f3 == 5) continue;
      if (!Apply(Word(OP_IMM_32, f3, 1, 2, 3, 0x00))) return;
      ExpectIllegalState("reserved: OP-IMM-32 funct3 " + std::to_string(f3));
    }
    // OP-IMM-32 shift: funct7 outside {0000000, 0100000}.
    for (uint32_t f7 = 1; f7 < 128; ++f7) {
      if (f7 == 0x20) continue;
      for (uint32_t f3 : {1u, 5u}) {
        if (!Apply(Word(OP_IMM_32, f3, 1, 2, 3, f7))) return;
        ExpectIllegalState("reserved: OP-IMM-32 shift funct7=" +
                           std::to_string(f7) + " funct3=" +
                           std::to_string(f3));
      }
    }
    // BRANCH: the reserved pair is funct3 010/011. funct3 100 is blt.
    for (uint32_t f3 : {2u, 3u}) {
      if (!Apply(Word(OP_BRANCH, f3, 0, 1, 2, 0x00))) return;
      ExpectIllegalState("reserved: BRANCH funct3 " + std::to_string(f3));
    }
    // JALR: every funct3 other than 000.
    for (uint32_t f3 = 1; f3 < 8; ++f3) {
      if (!Apply(Word(OP_JALR, f3, 1, 2, 0, 0x00))) return;
      ExpectIllegalState("reserved: JALR funct3 " + std::to_string(f3));
    }
    // OP-32. Three reserved classes live on this opcode and each is pinned by
    // name; the five legal encodings are the M word forms.
    {
      // (a) The RV32-only I word forms: reserved on RV64, on the very opcode
      // the M word forms live on. This is the negative neighbour for the
      // mulw/divw/... family.
      struct { const char* name; uint32_t f3; uint32_t f7; } w[] = {
          {"addw", 0, 0x00}, {"subw", 0, 0x20}, {"sllw", 1, 0x00},
          {"srlw", 5, 0x00}, {"sraw", 5, 0x20},
      };
      for (const auto& c : w) {
        if (!Apply(Word(OP_OP32, c.f3, 1, 2, 3, c.f7))) return;
        ExpectIllegalState(std::string("reserved on RV64: ") + c.name);
      }
      // (b) The high-half word multiplies: there is no MULHW/MULHSUW/MULHUW, so
      // these three funct3 values under funct7 0000001 are reserved even though
      // the opcode and funct7 are otherwise the M word forms.
      for (uint32_t f3 : {1u, 2u, 3u}) {
        if (!Apply(Word(OP_OP32, f3, 1, 2, 3, 0x01))) return;
        ExpectIllegalState("reserved: no MULH*W (OP-32 funct3 " +
                           std::to_string(f3) + " with funct7 0000001)");
      }
      // (c) The completeness sweep over all 1024 encodings, counting the legal
      // ones rather than assuming how many there are.
      uint32_t legal_op32 = 0;
      for (uint32_t f3 = 0; f3 < 8; ++f3) {
        for (uint32_t f7 = 0; f7 < 128; ++f7) {
          if (!Apply(Word(OP_OP32, f3, 1, 2, 3, f7))) return;
          if (Observe().valid) ++legal_op32;
        }
      }
      rep_.Check(legal_op32 == 5,
                 "exactly five OP-32 encodings are legal: the five M word forms");
    }
    // SYSTEM funct3 100 is reserved.
    if (!Apply(Word(OP_SYSTEM, 4, 1, 2, 3, 0x300))) return;
    ExpectIllegalState("reserved: SYSTEM funct3 100");
    // SYSTEM funct3 000 with an imm12 that is not 000 / 001 / 302.
    for (uint32_t imm12 : {0x002u, 0x003u, 0x101u, 0x102u, 0x200u, 0x301u,
                           0x303u, 0x305u, 0x7B2u, 0xB00u, 0xFFFu}) {
      if (!Apply(Word(OP_SYSTEM, 0, 0, 0, imm12 >> 5, imm12 & 0x1F))) return;
      ExpectIllegalState("reserved: SYSTEM funct3 000 imm12=" +
                         mosaic::Hex(imm12, 3));
    }
    // ecall/ebreak/mret with a nonzero rd or rs1: those bits are not part of
    // the encoding, so a nonzero value is a reserved encoding.
    for (uint32_t imm12 : {0x000u, 0x001u, 0x302u}) {
      if (!Apply(Word(OP_SYSTEM, 0, 1, 0, imm12 >> 5, imm12 & 0x1F))) return;
      ExpectIllegalState("reserved: SYSTEM imm12=" + mosaic::Hex(imm12, 3) +
                         " with rd != x0");
      if (!Apply(Word(OP_SYSTEM, 0, 0, 1, imm12 >> 5, imm12 & 0x1F))) return;
      ExpectIllegalState("reserved: SYSTEM imm12=" + mosaic::Hex(imm12, 3) +
                         " with rs1 != x0");
    }
    // Every opcode this profile does not decode. The twelve it does decode are
    // skipped -- OP-32 is one of them now (the M word forms), and all 1024 of
    // its encodings were swept above -- so the remaining 115 must be illegal.
    for (uint32_t op = 0; op < 128; ++op) {
      if (op == OP_LOAD || op == OP_MISC_MEM || op == OP_IMM ||
          op == OP_LUI || op == OP_AUIPC || op == OP_STORE ||
          op == OP_IMM_32 || op == OP_MUL_DIV || op == OP_OP32 ||
          op == OP_BRANCH || op == OP_JALR || op == OP_JAL ||
          op == OP_SYSTEM) {
        continue;
      }
      for (uint32_t f3 = 0; f3 < 8; ++f3) {
        if (!Apply(Word(op, f3, 1, 2, 3, 0x00))) return;
      }
      ++reserved_opcodes_;
    }
    rep_.Check(reserved_opcodes_ == 115,
               "115 opcodes are outside RV64IM and all decode as illegal");
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
    // live: the field a 7-bit funct7 reading gets wrong.
    for (uint32_t f3 = 0; f3 < 8; ++f3) {
      for (uint32_t top6 = 0; top6 < 64; ++top6) {
        if (!Apply(EncShiftX(OP_IMM, f3, 5, 6, 37, top6))) return;
      }
    }

    // (d) every register index: rd x rs1 for one representative of each format,
    // and rs1 x rs2 for the register-register formats.
    {
      const uint32_t rd_rs1[] = {
          EncI(OP_IMM, 0, 0, 0, 0x123),     EncI(OP_LOAD, 2, 0, 0, 0x123),
          EncI(OP_JALR, 0, 0, 0, 0x123),    EncI(OP_IMM_32, 0, 0, 0, 0x123),
          EncU(OP_LUI, 0, 0x12345),         EncU(OP_AUIPC, 0, 0x12345),
          EncJ(OP_JAL, 0, 0x1234),          EncShiftX(OP_IMM, 1, 0, 0, 0x3F, 0),
      };
      for (uint32_t base : rd_rs1) {
        for (uint32_t rd = 0; rd < 32; ++rd) {
          for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
            if (!Apply((base & ~((0x1Fu << 7) | (0x1Fu << 15))) | (rd << 7) |
                       (rs1 << 15))) {
              return;
            }
          }
        }
      }
      const uint32_t rs1_rs2[] = {
          EncR(OP_MUL_DIV, 0, 0, 0, 0, 0x00), EncR(OP_MUL_DIV, 4, 0, 0, 0, 0x01),
          EncR(OP_MUL_DIV, 7, 0, 0, 0, 0x20), EncS(OP_STORE, 2, 0, 0, 0x40),
          EncB(OP_BRANCH, 5, 0, 0, 0x100),   EncCsr(1, 0, 0, 0x300),
      };
      for (uint32_t base : rs1_rs2) {
        for (uint32_t rs1 = 0; rs1 < 32; ++rs1) {
          for (uint32_t rs2 = 0; rs2 < 32; ++rs2) {
            if (!Apply((base & ~((0x1Fu << 15) | (0x1Fu << 20))) | (rs1 << 15) |
                       (rs2 << 20))) {
              return;
            }
          }
        }
      }
    }

    // (e) every immediate bit, field by field. Each immediate is scrambled
    // across the word, so sweeping each field over its full range with the
    // others held at a few patterns reaches every value of every bit of the
    // immediate -- a complete argument, for a fraction of a 2^32 sweep.
    //   I-type: the whole 12-bit immediate, for an ALU form and a load.
    for (uint32_t imm = 0; imm < 4096; ++imm) {
      if (!Apply(EncI(OP_IMM, 0, 5, 6, static_cast<int32_t>(imm)))) return;
      if (!Apply(EncI(OP_LOAD, 3, 5, 6, static_cast<int32_t>(imm)))) return;
    }
    //   S-type: imm[11:5] over all 128, then imm[4:0] over all 32.
    for (uint32_t hi = 0; hi < 128; ++hi) {
      for (uint32_t pattern = 0; pattern < 8; ++pattern) {
        const int32_t imm = static_cast<int32_t>((hi << 5) | (pattern & 0x1F));
        if (!Apply(EncS(OP_STORE, 2, 7, 8, imm))) return;
      }
    }
    for (uint32_t lo = 0; lo < 32; ++lo) {
      for (uint32_t pattern = 0; pattern < 8; ++pattern) {
        const int32_t imm =
            static_cast<int32_t>((((pattern * 37) % 128) << 5) | lo);
        if (!Apply(EncS(OP_STORE, 3, 7, 8, imm))) return;
      }
    }
    //   B-type: all 2^13 immediate patterns, for two of the six funct3 values.
    for (uint32_t off = 0; off < 8192; ++off) {
      const int32_t imm = static_cast<int32_t>(off) - 4096;
      if (!Apply(EncB(OP_BRANCH, 0, 9, 10, imm))) return;
      if (!Apply(EncB(OP_BRANCH, 6, 9, 10, imm))) return;
    }
    //   J-type: imm[10:1] over 2^10, imm[19:12] over 2^8, imm[20] separately.
    for (uint32_t low = 0; low < 1024; ++low) {
      for (uint32_t pattern = 0; pattern < 4; ++pattern) {
        const uint32_t imm = ((pattern * 0x155) & 0x7F800) | (low << 1);
        if (!Apply(EncJ(OP_JAL, 5, static_cast<int32_t>(imm) - 0x100000))) return;
      }
    }
    for (uint32_t mid = 0; mid < 256; ++mid) {
      for (uint32_t pattern = 0; pattern < 4; ++pattern) {
        const uint32_t imm = ((pattern * 0x2AB) & 0x3FFFE) | (mid << 12);
        if (!Apply(EncJ(OP_JAL, 6, static_cast<int32_t>(imm) - 0x100000))) return;
      }
    }
    for (uint32_t sign = 0; sign < 2; ++sign) {
      for (uint32_t pattern = 0; pattern < 4; ++pattern) {
        const uint32_t imm = (sign << 20) | ((pattern * 0x2AB3) & 0x1FFFE);
        if (!Apply(EncJ(OP_JAL, 7, static_cast<int32_t>(imm) - 0x100000))) return;
      }
    }
    //   U-type: imm[31:12] over 2^12, then imm[31:24] over 2^8.
    for (uint32_t imm20 = 0; imm20 < 4096; ++imm20) {
      if (!Apply(EncU(OP_LUI, 5, imm20))) return;
    }
    for (uint32_t top = 0; top < 256; ++top) {
      for (uint32_t pattern = 0; pattern < 8; ++pattern) {
        if (!Apply(EncU(OP_AUIPC, 5, (top << 12) | (pattern * 0x1111)))) return;
      }
    }
  }

  // ---- random campaign ------------------------------------------------------
  void Random() {
    if (aborted_) return;
    static const uint32_t kOps[] = {
        OP_LOAD, OP_MISC_MEM, OP_IMM, OP_LUI, OP_AUIPC, OP_STORE,
        OP_IMM_32, OP_MUL_DIV, OP_OP32, OP_BRANCH, OP_JALR, OP_JAL, OP_SYSTEM};
    const uint32_t iterations = opt_.max_cycles / 4;
    for (uint32_t i = 0; i < iterations; ++i) {
      if (!Apply(static_cast<uint32_t>(rng_.Next() >> 32))) return;
      uint32_t word = kOps[rng_.Below(13)];
      word |= rng_.Below(8) << 12;
      word |= rng_.Below(32) << 7;
      word |= rng_.Below(32) << 15;
      word |= rng_.Below(32) << 20;
      word |= (rng_.Chance(70) ? rng_.Below(128) : (rng_.Below(3) << 5)) << 25;
      if (!Apply(word)) return;
    }
  }

  // ---- finish ---------------------------------------------------------------
  int Finish() {
    if (!aborted_) {
      // Coverage: the campaign really did reach every opcode and every
      // (opcode, funct3) pair, and both sides were exercised in bulk.
      for (uint32_t op = 0; op < 128; ++op) {
        rep_.Check(op_seen_[op] > 0,
                   "opcode " + std::to_string(op) + " was presented");
      }
      for (uint32_t i = 0; i < 1024; ++i) {
        rep_.Check(opf3_seen_[i] > 0,
                   "opcode/funct3 pair " + std::to_string(i) + " was presented");
      }
      rep_.Check(legal_ > 10000,
                 "legal side exercised: " + std::to_string(legal_) + " instructions");
      rep_.Check(illegal_ > 10000, "illegal side exercised: " +
                                       std::to_string(illegal_) + " instructions");
      rep_.Check(presented_ <= opt_.max_cycles,
                 "presented " + std::to_string(presented_) +
                     " instructions within --max-cycles=" +
                     std::to_string(opt_.max_cycles));
      rep_.Check(legal_ + illegal_ == presented_,
                 "every presented instruction was legal or illegal, never both");
      rep_.Check(reserved_checks_ > 400,
                 "named reserved checks ran: " + std::to_string(reserved_checks_));
    }
    rep_.Check(!aborted_, aborted_ ? reason_ : "no mismatch against the reference");

    std::ostringstream detail;
    if (aborted_ || rep_.failures() > 0) {
      detail << presented_ << " instructions compared, " << rep_.failures()
             << " failure(s)";
      return rep_.Finish("FAIL", detail.str());
    }
    detail << presented_ << " instructions (" << legal_ << " legal, " << illegal_
           << " illegal), 0 mismatches, " << reserved_checks_
           << " named reserved checks";
    return rep_.Finish("PASS", detail.str());
  }

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
    r.md_w = static_cast<uint8_t>(top_->o_md_w);
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
  // Without this, a typo in one of the break-out assignments would either hide
  // a decoder bug or invent one.
  bool CheckBreakout(const Ref& r, const std::string& where) {
    // 134 bits: 2 + 3 + 15 + 64 + 6 + 7 + 8 + 2 + 5 + 4 + 12 + 3. The first
    // field pushed lands at the most significant end, because that is how a
    // packed struct is laid out.
    const int kTotalBits = 134;
    uint64_t chunk[3] = {0, 0, 0};
    int nbits = 0;
    // Fields are laid out MSB first, and inside a field the value's bit 0 sits
    // at the field's lowest struct bit.
    auto push = [&](uint64_t value, int width) {
      const int base = kTotalBits - (nbits + width);
      for (int i = 0; i < width; ++i) {
        if ((value >> i) & 1) chunk[(base + i) >> 6] |= 1ull << ((base + i) & 63);
      }
      nbits += width;
    };
    // Declaration order of mosaic_pkg::decode_ctl_t, MSB first.
    push(r.valid, 1);       push(r.illegal, 1);
    push(r.uses_rs1, 1);    push(r.uses_rs2, 1);   push(r.uses_imm, 1);
    push(r.rs1, 5);         push(r.rs2, 5);        push(r.rd, 5);
    push(r.imm, 64);
    push(r.alu_op, 4);      push(r.uses_alu, 1);  push(r.reg_write, 1);
    push(r.mem_kind, 3);    push(r.mem_size, 3);   push(r.mem_signed, 1);
    push(r.is_branch, 1);   push(r.branch_funct, 3);
    push(r.is_jal, 1);      push(r.is_jalr, 1);    push(r.is_auipc, 1);
    push(r.writes_link, 1);
    push(r.is_miscmem, 1);  push(r.is_fence_i, 1);
    push(r.is_muldiv, 1);   push(r.md_op, 3);      push(r.md_signed, 1);
    push(r.md_w, 1);
    push(r.is_system, 1);   push(r.is_ecall, 1);   push(r.is_ebreak, 1);
    push(r.is_mret, 1);
    push(r.csr_op, 2);      push(r.csr_addr, 12);
    push(r.csr_writes, 1);  push(r.csr_reads, 1); push(r.csr_imm_form, 1);
    rep_.Check(nbits == kTotalBits,
               "decode_ctl_t is " + std::to_string(kTotalBits) +
                   " bits wide and every one is accounted for");

    // 134 bits is five 32-bit Verilator words, not three 64-bit ones.
    const uint64_t w0 = top_->o_ctl_bits[0], w1 = top_->o_ctl_bits[1];
    const uint64_t w2 = top_->o_ctl_bits[2], w3 = top_->o_ctl_bits[3];
    const uint64_t w4 = top_->o_ctl_bits[4];
    const uint64_t dut_lo = w0 | (w1 << 32);
    const uint64_t dut_mid = w2 | (w3 << 32);
    const uint64_t dut_top = w4 & 0x3Full;
    const uint64_t my_lo = chunk[0];
    const uint64_t my_mid = chunk[1];
    const uint64_t my_top = chunk[2] & 0x3Full;
    if (my_lo == dut_lo && my_mid == dut_mid && my_top == dut_top) return true;
    rep_.Mismatch(where + " (testbench break-out vs decode_ctl_t)",
                  mosaic::Hex(my_lo) + "|" + mosaic::Hex(my_mid) + "|" +
                      mosaic::Hex(my_top),
                  mosaic::Hex(dut_lo) + "|" + mosaic::Hex(dut_mid) + "|" +
                      mosaic::Hex(dut_top));
    rep_.Check(false, "testbench break-out does not match decode_ctl_t");
    aborted_ = true;
    reason_ = "testbench break-out does not match decode_ctl_t at " + where;
    return false;
  }

  // Assert that the instruction just applied is reserved: illegal, not valid,
  // and every other field left at the defined illegal state rather than
  // half-decoded.
  void ExpectIllegalState(const std::string& name) {
    const Ref r = Observe();
    const Ref z = Illegal();
    const bool clean =
        r.illegal == 1 && r.valid == 0 && r.uses_rs1 == 0 && r.uses_rs2 == 0 &&
        r.uses_imm == 0 && r.rs1 == 0 && r.rs2 == 0 && r.rd == 0 && r.imm == 0 &&
        r.alu_op == z.alu_op && r.uses_alu == 0 && r.reg_write == 0 &&
        r.mem_kind == MEM_NONE && r.mem_size == z.mem_size && r.mem_signed == 0 &&
        r.is_branch == 0 && r.branch_funct == 0 && r.is_jal == 0 &&
        r.is_jalr == 0 && r.is_auipc == 0 && r.writes_link == 0 &&
        r.is_miscmem == 0 && r.is_fence_i == 0 && r.is_muldiv == 0 &&
        r.md_op == z.md_op && r.md_signed == 0 && r.md_w == 0 && r.is_system == 0 &&
        r.is_ecall == 0 && r.is_ebreak == 0 && r.is_mret == 0 &&
        r.csr_op == CSR_NONE && r.csr_addr == 0 && r.csr_writes == 0 &&
        r.csr_reads == 0 && r.csr_imm_form == 0;
    rep_.Check(clean, name);
    ++reserved_checks_;
  }

  mosaic::Options opt_;
  mosaic::Reporter& rep_;
  Vmosaic_decoder_tb* top_;
  mosaic::Rng rng_;
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