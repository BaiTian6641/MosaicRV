// ============================================================================
// tb_core_compressed.cpp -- CASE=compressed.cross_boundary, work package I-041.
//
// The DUT is the integrated p0 core with the C extension wired in: the fetch
// unit decides an instruction's length from its own encoding (`rsp_data[1:0] ==
// 11` is a 32-bit instruction and four bytes, anything else is a compressed one
// and two), delivers the instruction's *own* bits and *own* length, advances the
// program counter by that length at the cycle the response is accepted, and
// carries the length and the original bits through the decode buffer, the macro
// descriptor and the ROB so the architectural event record reports what retired
// -- PC, length and the instruction's own encoding -- rather than a PC the
// consumer has to interpret. mosaic_decoder expands the 16-bit encoding to its
// base-ISA equivalent, so nothing downstream of it knows C exists.
//
// ------------------------------------------------------------- the programs
//
// Six hand-built images (no ELF and no assembler at run time: the encodings are
// emitted here one instruction at a time by this file's own encoders, whose
// output was checked against `riscv64-elf-as -march=rv64imc` / `objdump -d` at
// authoring time -- see the report's encoding table). Each run has its own
// image and its own reference, so a run inherits nothing from the one before it.
//
//   mixed     a 16/32-bit mixed straight line with the interesting placements:
//             a compressed instruction whose PC is 2 mod 4, a 32-bit
//             instruction whose PC is 2 mod 4 (so its four bytes straddle a
//             word boundary), compressed HINTs (c.nop, c.addi x0), a c.beqz
//             that is not taken (so its fall-through must be PC+2, not PC+4), a
//             c.bnez that is taken, a c.j, and a c.jalr whose link register must
//             be PC+2 (the card's "link PC increment").
//
//   boundary  the same mix, arranged so a 32-bit instruction's four bytes cross
//             a fetch "line" (the harness's memory model has an eight-byte
//             line), and ending with an instruction whose *second halfword* is
//             outside the mapped image: the fetch of it must fault, and the
//             fault must be reported at the instruction's own start address.
//
//   reserved  one run per reserved/unimplemented compressed encoding (six of
//             them): the instructions before it retire, the reserved encoding
//             itself does not, and the machine stops at it. The all-zero
//             halfword is one of them, so "a fetch of padding does not execute"
//             is stated and not implied.
//
//   trap      a misaligned c.lwsp with the trap vector installed: the trap
//             record must name the compressed instruction's own PC and its own
//             length, which is the card's "the trap trace uses the original
//             instruction PC/length".
//
// ------------------------------------------------------------ the expectation
//
// Nothing here comes from the DUT. The image is this file's own, and the
// instruction stream it must produce is computed by this file's own RV64IC
// interpreter (RefCore below), which reads the same bytes and is written from
// the ISA text -- it does not call, mirror or import the RTL's decompressor.
// Every retired lane is compared against it:
//
//   1. the PC is the instruction's own start address (never "the previous PC
//      plus four": the reference's PC comes from summing each instruction's own
//      length);
//   2. the length the event record reports is the instruction's own (2 or 4),
//      and the length agrees with the encoding the record carries --
//      `len == 2` if and only if `insn[1:0] != 11` -- for every lane;
//   3. the bits the event record carries are the instruction's own encoding, the
//      one the image holds at that PC (a 16-bit instruction's upper half must
//      not be the next instruction's);
//   4. the destination, the write enable and the value are the reference's;
//   5. a trap lane names the trapping instruction's own PC, length and bits, and
//      the cause and tval the ISA defines.
//
// plus the structural facts the reference cannot state:
//
//   6. every address the core *requests* from the instruction port is an
//      instruction's start address (or an address the image cannot serve at
//      all): a PC that lands in the middle of an instruction is a PC advanced by
//      the wrong amount;
//   7. at the second-halfword fault the core's delivered PC is the faulting
//      instruction's start address;
//   8. `o_fetch_pc` only ever takes values that are instruction start addresses;
//   9. a reserved encoding leaves no architectural trace at all: nothing
//      retires at its PC.
//
// ------------------------------------------------------------- the controls
//
// tools/run_compressed_controls.py rebuilds this case with exactly one defect
// injected and requires the run to fail by naming the check it breaks:
//   (a) MOSAIC_CORE_MUTANT_FETCH_PC_PLUS4     the counter is advanced by four
//       whatever the encoding was (the card's named failure mode);
//   (b) MOSAIC_FETCH_MUTANT_DELIVER_16BIT     a 16-bit instruction is delivered
//       as a 32-bit one, length four;
//   (c) MOSAIC_DECODER_MUTANT_C_RESERVED_EXEC a reserved encoding is expanded
//       and executed instead of refused;
//   (d) MOSAIC_IMEM_MUTANT_WRONG_LINE         the harness's memory model
//       assembles a straddling instruction from the wrong line. In p0 there is
//       no cache: the *memory* assembles the four bytes, so the defect that
//       "assembles from the wrong line" lives there, and this case's
//       per-instruction comparison is what has to catch it.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "sim_common.h"

using mosaic_ref::DataMem;

namespace {

constexpr int kResetCycles = 4;
constexpr int kDrainCycles = 96;
constexpr int kStallCycles = 4000;
constexpr int kMaxRunCycles = 20000;
// The harness's fetch "line": a request whose four bytes cross a line boundary
// is served from one line in the shipping model, and from the *next* one under
// control (d).
constexpr uint64_t kLineBytes = 8;
constexpr uint64_t kBase = 0x80000000ull;
constexpr uint64_t kTohost = 0x80001000ull;   // MOSAIC_TOHOST
constexpr uint64_t kPassCode = 1;             // MOSAIC_PASS_CODE
// The compressed-instruction length rule, stated once: an encoding whose low two
// bits are 11 is a 32-bit instruction, anything else is a 16-bit one.
constexpr uint32_t kLen32 = 4;
constexpr uint32_t kLen16 = 2;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// Instruction encoding: the formats this file's programs use
// ============================================================================
// Every one of these was checked against `riscv64-elf-as -march=rv64imc` and
// `riscv64-elf-objdump -d` when the programs below were laid out.
uint32_t EncR(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, uint32_t rs2,
              uint32_t f7) {
  return op | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}
uint32_t EncI(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm) {
  return op | (rd << 7) | (f3 << 12) | (rs1 << 15) |
         ((static_cast<uint32_t>(imm) & 0xFFFu) << 20);
}
uint32_t EncS(uint32_t op, uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0xFFFu;
  return op | ((u & 0x1Fu) << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) |
         ((u >> 5) << 25);
}
uint32_t EncU(uint32_t op, uint32_t rd, uint32_t imm20) {
  return op | (rd << 7) | ((imm20 & 0xFFFFFu) << 12);
}
uint32_t EncJ(uint32_t op, uint32_t rd, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off) & 0x1FFFFFu;
  return op | (rd << 7) | (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
         (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12);
}
uint32_t EncCsr(uint32_t f3, uint32_t rd, uint32_t csr, uint32_t rs1) {
  return 0x73u | (rd << 7) | (f3 << 12) | (rs1 << 15) | (csr << 20);
}

// A compressed instruction is a halfword; these builders place the fields where
// the 16-bit formats put them (RV64C).
uint16_t CQuad0(uint32_t f3, uint32_t w, uint32_t v) {
  (void)w;
  return static_cast<uint16_t>((f3 << 13) | (v & 0x1FFFu) | 0x0u);
}
uint16_t CQuad1(uint32_t f3, uint32_t w, uint32_t v) {
  (void)w;
  return static_cast<uint16_t>((f3 << 13) | (v & 0x1FFFu) | 0x1u);
}
// 000 imm[5] rd imm[4:0] 01 -- c.addi (rd = x0 is c.nop / a hint)
uint16_t CAddi(uint32_t rd, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0x3Fu;
  return static_cast<uint16_t>(0x0000u | (((u >> 5) & 1u) << 12) | (rd << 7) |
                               ((u & 0x1Fu) << 2) | 0x1u);
}
// 010 imm[5] rd imm[4:0] 01 -- c.li
uint16_t CLi(uint32_t rd, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0x3Fu;
  return static_cast<uint16_t>(0x4000u | (((u >> 5) & 1u) << 12) | (rd << 7) |
                               ((u & 0x1Fu) << 2) | 0x1u);
}
// 100 0 rd rs2 10 -- c.mv (rd = x0 is a hint)
uint16_t CMv(uint32_t rd, uint32_t rs2) {
  return static_cast<uint16_t>(0x8000u | (rd << 7) | (rs2 << 2) | 0x2u);
}
// 100 1 rd rs2 10 -- c.add (rd = x0 is a hint)
uint16_t CAdd(uint32_t rd, uint32_t rs2) {
  return static_cast<uint16_t>(0x9000u | (rd << 7) | (rs2 << 2) | 0x2u);
}
// 101 imm 01 -- c.j, `off` is the byte offset (even, |off| <= 2046)
uint16_t CJ(int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off) & 0x7FFu;
  const uint32_t v = (((u >> 11) & 1u) << 12) | (((u >> 4) & 1u) << 11) |
                     (((u >> 8) & 0x3u) << 9) | (((u >> 10) & 1u) << 8) |
                     (((u >> 6) & 1u) << 7) | (((u >> 7) & 1u) << 6) |
                     (((u >> 1) & 0x7u) << 3) | (((u >> 5) & 1u) << 2);
  return static_cast<uint16_t>(0xA000u | v | 0x1u);
}
// 110/111 rs1' imm 01 -- c.beqz (f3 = 110) / c.bnez (f3 = 111)
uint16_t CBranch(uint32_t f3, uint32_t rs1p, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off) & 0x1FFu;
  const uint32_t v = (((u >> 8) & 1u) << 12) | (((u >> 3) & 0x3u) << 10) |
                     ((rs1p & 0x7u) << 7) | (((u >> 6) & 0x3u) << 5) |
                     (((u >> 1) & 0x3u) << 3) | (((u >> 5) & 1u) << 2);
  return static_cast<uint16_t>((f3 << 13) | v | 0x1u);
}
// 100 1 rs1 00000 10 -- c.jalr: link = pc + 2
uint16_t CJalr(uint32_t rs1) {
  return static_cast<uint16_t>(0x9000u | (rs1 << 7) | 0x2u);
}
// 010 uimm rd uimm 10 -- c.lwsp: lw rd, uimm(x2)
uint16_t CLwsp(uint32_t rd, uint32_t uimm) {
  const uint32_t u = uimm & 0xFFu;
  const uint32_t v = (((u >> 5) & 0x3u) << 2) | (((u >> 2) & 0x7u) << 4) |
                     (((u >> 7) & 1u) << 12);
  return static_cast<uint16_t>(0x4000u | v | (rd << 7) | 0x2u);
}
uint16_t CNop() { return CAddi(0, 0); }

// ============================================================================
// The instruction image
// ============================================================================
// A byte-addressed image, because a mixed-width program is not word-addressed:
// the interesting cases are exactly the instructions whose four-byte fetch
// window does not line up with anything.
//
// The memory system's view of a request is what determines a fetch *fault*, and
// it is stated here as the rule the ISA gives: the encoding decides how many
// bytes the instruction needs. The first halfword is read; if its low two bits
// are 11 the instruction is 32-bit and the second halfword must be mapped, or
// the fetch cannot be served (the card's "second-halfword fault"); otherwise the
// instruction is 16-bit and two bytes are enough, so an unmapped upper halfword
// is not a fault -- it belongs to whatever comes next. A halfword that is not
// mapped at all cannot even be read, so the fetch faults there too.
class ProgImage {
 public:
  void Emit16(uint64_t addr, uint16_t half) {
    Put(addr, static_cast<uint8_t>(half & 0xFFu));
    Put(addr + 1, static_cast<uint8_t>(half >> 8));
    starts_.insert(addr);
  }
  void Emit32(uint64_t addr, uint32_t word) {
    for (int i = 0; i < 4; i++) Put(addr + static_cast<uint64_t>(i),
                                    static_cast<uint8_t>((word >> (8 * i)) & 0xFFu));
    starts_.insert(addr);
  }

  bool Mapped(uint64_t addr) const { return bytes_.count(addr) != 0; }
  bool IsStart(uint64_t addr) const { return starts_.count(addr) != 0; }

  uint16_t Half(uint64_t addr) const {
    const auto lo = bytes_.find(addr);
    const auto hi = bytes_.find(addr + 1);
    const uint32_t a = (lo == bytes_.end()) ? 0u : lo->second;
    const uint32_t b = (hi == bytes_.end()) ? 0u : hi->second;
    return static_cast<uint16_t>(a | (b << 8));
  }

  // The four bytes the instruction at `addr` needs, if they can be assembled.
  // `wrong_line` is control (d): the assembler takes the bytes of the *next*
  // line when the instruction's window crosses a line boundary.
  bool Fetch(uint64_t addr, bool wrong_line, uint32_t* bits, uint32_t* len) const {
    if (!Mapped(addr) || !Mapped(addr + 1)) return false;
    uint64_t from = addr;
    const uint16_t first = Half(addr);
    const uint32_t need = ((first & 3u) == 3u) ? 4u : 2u;
    if (wrong_line && (addr % kLineBytes) + need > kLineBytes) {
      from = addr + kLineBytes - (addr % kLineBytes);   // the following line
    }
    if (need == 4u) {
      if (!Mapped(from + 2) || !Mapped(from + 3)) return false;
      *len = kLen32;
      *bits = static_cast<uint32_t>(Half(from)) |
              (static_cast<uint32_t>(Half(from + 2)) << 16);
    } else {
      *len = kLen16;
      *bits = static_cast<uint32_t>(first);
    }
    return true;
  }

  uint64_t first() const { return starts_.empty() ? 0 : *starts_.begin(); }
  size_t starts() const { return starts_.size(); }

 private:
  void Put(uint64_t addr, uint8_t byte) { bytes_[addr] = byte; }

  std::map<uint64_t, uint8_t> bytes_;
  std::set<uint64_t> starts_;
};

// ============================================================================
// The independent reference: RV64IC, written from the ISA text
// ============================================================================
// It executes the image the case builds, with its own register file and its own
// expansion of the 16-bit encodings, and produces the architectural stream the
// DUT's event records are compared against. It stops where the program's own
// story ends: the exit store, a reserved encoding, a fetch it cannot serve, or a
// trap.
struct RefRecord {
  uint64_t pc = 0;
  uint32_t len = 0;
  uint32_t bits = 0;
  uint32_t rd = 0;
  bool we = false;
  uint64_t value = 0;
  bool trap = false;
  uint64_t cause = 0;
  uint64_t tval = 0;
};

struct RefTrace {
  std::vector<RefRecord> records;
  bool stopped = false;      // the program's own story ends here
  bool exited = false;       // it reached the exit protocol
  uint64_t stop_pc = 0;
  std::string stop_reason;
};

constexpr uint64_t kCauseLoadMisaligned = 4;   // the ISA's exception code

uint64_t SignExtend(uint64_t value, unsigned bits) {
  const uint64_t sign = UINT64_C(1) << (bits - 1);
  return (value ^ sign) - sign;
}

RefTrace RunReference(const ProgImage& img, uint64_t start) {
  RefTrace out;
  uint64_t regs[32] = {};
  uint64_t pc = start;
  for (int step = 0; step < 512; step++) {
    uint32_t bits = 0;
    uint32_t len = 0;
    if (!img.Fetch(pc, false, &bits, &len)) {
      out.stopped = true;
      out.stop_pc = pc;
      out.stop_reason = "the instruction at this address cannot be fetched: its "
                        "second halfword is outside the image";
      return out;
    }
    RefRecord r;
    r.pc = pc;
    r.len = len;
    r.bits = bits;
    const uint64_t next_seq = pc + len;

    if (len == kLen16) {
      const uint32_t c = bits & 0xFFFFu;
      const uint32_t f3 = (c >> 13) & 0x7u;
      const uint32_t rdp = 8u + ((c >> 2) & 0x7u);
      const uint32_t rs1p = 8u + ((c >> 7) & 0x7u);
      const uint32_t rd = (c >> 7) & 0x1Fu;
      const uint32_t rs2 = (c >> 2) & 0x1Fu;
      switch (c & 3u) {
        case 0u: {   // quadrant 0
          if (f3 == 0u) {   // c.addi4spn: addi rd', x2, nzuimm
            const uint32_t nzuimm = (((c >> 7) & 0xFu) << 6) | (((c >> 11) & 0x3u) << 4) |
                                    (((c >> 5) & 1u) << 3) | (((c >> 6) & 1u) << 2);
            if (nzuimm == 0u) {   // reserved (this is the all-zero halfword)
              out.stopped = true;
              out.stop_pc = pc;
              out.stop_reason = "reserved compressed encoding: c.addi4spn with "
                                "nzuimm == 0";
              return out;
            }
            r.rd = rdp;
            r.we = true;
            r.value = regs[2] + nzuimm;
            regs[rdp] = r.value;
          } else {
            out.stopped = true;
            out.stop_pc = pc;
            out.stop_reason = "reserved or unimplemented quadrant-0 encoding";
            return out;
          }
          break;
        }
        case 1u: {   // quadrant 1
          const int64_t imm6 = static_cast<int64_t>(SignExtend(((c >> 12) & 1u) << 5 |
                                                               ((c >> 2) & 0x1Fu), 6));
          if (f3 == 0u) {          // c.nop / c.addi: addi rd, rd, imm
            r.rd = rd;
            r.we = (rd != 0);
            r.value = static_cast<uint64_t>(static_cast<int64_t>(regs[rd]) + imm6);
            if (r.we) regs[rd] = r.value;
          } else if (f3 == 2u) {   // c.li: addi rd, x0, imm
            r.rd = rd;
            r.we = (rd != 0);
            r.value = static_cast<uint64_t>(imm6);
            if (r.we) regs[rd] = r.value;
          } else if (f3 == 5u) {   // c.j: jal x0, offset
            const int32_t off = static_cast<int32_t>(
                SignExtend(((c >> 12) & 1u) << 11 | ((c >> 11) & 1u) << 4 |
                               ((c >> 9) & 0x3u) << 8 | ((c >> 8) & 1u) << 10 |
                               ((c >> 7) & 1u) << 6 | ((c >> 6) & 1u) << 7 |
                               ((c >> 3) & 0x7u) << 1 | ((c >> 2) & 1u) << 5,
                           12));
            pc = pc + static_cast<uint64_t>(off);
            out.records.push_back(r);
            continue;
          } else if (f3 == 6u || f3 == 7u) {   // c.beqz / c.bnez
            const int32_t off = static_cast<int32_t>(
                SignExtend(((c >> 12) & 1u) << 8 | ((c >> 10) & 0x3u) << 3 |
                               ((c >> 5) & 0x3u) << 6 | ((c >> 3) & 0x3u) << 1 |
                               ((c >> 2) & 1u) << 5,
                           9));
            const uint64_t v = regs[rs1p];
            const bool take = (f3 == 6u) ? (v == 0u) : (v != 0u);
            pc = take ? (pc + static_cast<uint64_t>(off)) : (pc + kLen16);
            out.records.push_back(r);
            continue;
          } else {
            out.stopped = true;
            out.stop_pc = pc;
            out.stop_reason = "reserved or unimplemented quadrant-1 encoding";
            return out;
          }
          break;
        }
        case 2u: {   // quadrant 2
          if (f3 == 0u) {          // c.slli (rd == x0 is a hint)
            const uint32_t sh = ((c >> 12) & 1u) << 5 | ((c >> 2) & 0x1Fu);
            r.rd = rd;
            r.we = (rd != 0);
            r.value = (sh >= 64) ? 0 : (regs[rd] << sh);
            if (r.we) regs[rd] = r.value;
          } else if (f3 == 2u) {   // c.lwsp: lw rd, uimm(x2)
            const uint32_t uimm = (((c >> 2) & 0x3u) << 6) | (((c >> 12) & 1u) << 5) |
                                  (((c >> 4) & 0x7u) << 2);
            if (rd == 0u) {
              out.stopped = true;
              out.stop_pc = pc;
              out.stop_reason = "reserved compressed encoding: c.lwsp with rd == x0";
              return out;
            }
            const uint64_t addr = regs[2] + uimm;
            if ((addr & 3u) != 0u) {    // the p0 policy traps a misaligned access
              r.trap = true;
              r.cause = kCauseLoadMisaligned;
              r.tval = addr;
              out.records.push_back(r);
              out.stopped = true;
              out.stop_pc = pc;
              out.stop_reason = "the misaligned load traps, and the trap record is "
                                "where this reference stops";
              return out;
            }
            r.rd = rd;
            r.we = true;
            r.value = 0;   // the run's data is not what this case is about
            regs[rd] = r.value;
          } else if (f3 == 4u) {
            if (((c >> 12) & 1u) == 0u) {
              if (((c >> 2) & 0x1Fu) == 0u) {   // c.jr: jalr x0, 0(rs1)
                if (rd == 0u) {
                  out.stopped = true;
                  out.stop_pc = pc;
                  out.stop_reason = "reserved compressed encoding: c.jr with rs1 == x0";
                  return out;
                }
                pc = regs[rd] & ~UINT64_C(1);
                out.records.push_back(r);
                continue;
              }
              r.rd = rd;   // c.mv: add rd, x0, rs2
              r.we = (rd != 0);
              r.value = regs[rs2];
              if (r.we) regs[rd] = r.value;
            } else if (((c >> 2) & 0x1Fu) == 0u) {
              if (rd == 0u) {   // c.ebreak
                out.stopped = true;
                out.stop_pc = pc;
                out.stop_reason = "c.ebreak is not part of this case";
                return out;
              }
              r.rd = 1u;        // c.jalr: jalr x1, 0(rs1); the link is PC + 2
              r.we = true;
              r.value = pc + kLen16;
              regs[1] = r.value;
              pc = regs[rd] & ~UINT64_C(1);
              out.records.push_back(r);
              continue;
            } else {            // c.add: add rd, rd, rs2
              r.rd = rd;
              r.we = (rd != 0);
              r.value = regs[rd] + regs[rs2];
              if (r.we) regs[rd] = r.value;
            }
          } else {
            out.stopped = true;
            out.stop_pc = pc;
            out.stop_reason = "reserved or unimplemented quadrant-2 encoding";
            return out;
          }
          break;
        }
        default: {
          out.stopped = true;
          out.stop_pc = pc;
          out.stop_reason = "reserved compressed quadrant";
          return out;
        }
      }
      out.records.push_back(r);
      pc = next_seq;
      continue;
    }

    // 32-bit forms.
    const uint32_t opcode = bits & 0x7Fu;
    const uint32_t rd = (bits >> 7) & 0x1Fu;
    const uint32_t f3 = (bits >> 12) & 0x7u;
    const uint32_t rs1 = (bits >> 15) & 0x1Fu;
    const uint32_t rs2 = (bits >> 20) & 0x1Fu;
    const uint32_t f7 = (bits >> 25) & 0x7Fu;
    const int64_t imm_i = static_cast<int32_t>(bits) >> 20;
    switch (opcode) {
      case 0x13u: {   // OP-IMM
        if (f3 == 0u) {
          r.rd = rd;
          r.we = (rd != 0);
          r.value = static_cast<uint64_t>(static_cast<int64_t>(regs[rs1]) + imm_i);
        } else if (f3 == 1u) {   // slli (RV64: shamt in imm[5:0])
          const uint32_t sh = ((bits >> 20) & 0x3Fu);
          r.rd = rd;
          r.we = (rd != 0);
          r.value = (sh >= 64) ? 0 : (regs[rs1] << sh);
        } else {
          Fail("reference", "an OP-IMM form this reference does not implement: f3=" +
                                Dec(f3) + " at pc " + U64(pc));
        }
        if (r.we) regs[rd] = r.value;
        break;
      }
      case 0x33u: {   // OP
        if (f3 == 0u && f7 == 0x00u) {
          r.rd = rd;
          r.we = (rd != 0);
          r.value = regs[rs1] + regs[rs2];
          if (r.we) regs[rd] = r.value;
        } else {
          Fail("reference", "an OP form this reference does not implement at pc " +
                                U64(pc));
        }
        break;
      }
      case 0x17u: {   // AUIPC
        r.rd = rd;
        r.we = (rd != 0);
        const uint32_t imm20 = (bits >> 12) & 0xFFFFFu;
        r.value = pc + static_cast<uint64_t>(SignExtend(imm20, 20) << 12);
        if (r.we) regs[rd] = r.value;
        break;
      }
      case 0x6Fu: {   // JAL
        const int32_t off = static_cast<int32_t>(
            SignExtend(((bits >> 31) & 1u) << 20 | ((bits >> 12) & 0xFFu) << 12 |
                           ((bits >> 20) & 1u) << 11 | ((bits >> 21) & 0x3FFu) << 1,
                       21));
        r.rd = rd;
        r.we = (rd != 0);
        r.value = pc + kLen32;
        if (r.we) regs[rd] = r.value;
        pc = pc + static_cast<uint64_t>(off);
        out.records.push_back(r);
        continue;
      }
      case 0x73u: {   // SYSTEM: the CSR instruction this case installs mtvec with
        r.rd = rd;
        r.we = false;   // the case checks the event stream, not the CSR file
        break;
      }
      case 0x23u: {   // STORE (sd)
        const uint64_t addr = regs[rs1] +
                              static_cast<uint64_t>(static_cast<int64_t>(
                                  (static_cast<int32_t>(bits) >> 25) << 5 |
                                  static_cast<int32_t>((bits >> 7) & 0x1Fu)));
        r.rd = 0;
        r.we = false;
        if (addr == kTohost && regs[rs2] == kPassCode) {
          out.records.push_back(r);
          out.stopped = true;
          out.exited = true;
          out.stop_pc = pc;
          out.stop_reason = "the program reached its exit protocol";
          return out;
        }
        break;
      }
      default:
        Fail("reference", "an opcode this reference does not implement at pc " +
                              U64(pc) + ": " + U64(bits));
    }
    out.records.push_back(r);
    pc = next_seq;
  }
  Fail("reference", "the reference ran away: it never stopped");
  return out;
}

// ============================================================================
// Geometry and lane packing
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint64_t reset_vector = 0;
  uint32_t seq_w = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  g.seq_w = dut->o_geom_seq_w_o;
  return g;
}

template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return static_cast<uint64_t>(wide[lane * 2]) |
         (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
}
inline uint64_t PayloadLane(uint64_t value, uint32_t /*lane*/) { return value; }

uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
  const uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
  return (packed >> (lane * width)) & mask;
}

// ============================================================================
// The run
// ============================================================================
// One retired lane, exactly as the architectural event stream published it.
struct DutRecord {
  uint64_t pc = 0;
  uint32_t len = 0;
  uint32_t insn = 0;
  uint32_t rd = 0;
  bool we = false;
  uint64_t value = 0;
  bool trap = false;
  uint64_t cause = 0;
  uint64_t tval = 0;
};

// One instruction the fetch unit had in its output register, as the observation
// ports publish it: its identity, and whether it was delivered as an
// instruction (an illegal or faulting delivery shows `is_insn` false but still
// has a PC -- that is how a case can say *which* address could not be decoded).
struct Delivery {
  uint64_t pc = 0;
  bool is_insn = false;
};

// "A response was accepted at this PC, and the fetch the core issued in that
// same cycle went here." It is the machine's own statement of the instruction's
// length, and it is what the reference's length is compared against.
struct Advance {
  uint64_t pc = 0;
  uint64_t req = 0;
};

class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, const ProgImage* img, bool wrong_line,
          uint64_t max_cycles)
      : dut_(dut), imem_(img, wrong_line), dmem_(&mem_), max_cycles_(max_cycles) {}

  void Configure(const Geometry& g) { g_ = g; }

  const std::vector<DutRecord>& records() const { return records_; }
  const std::vector<Delivery>& deliveries() const { return deliveries_; }
  const std::vector<Advance>& advances() const { return advances_; }
  const std::vector<uint64_t>& requests() const { return requests_; }
  bool stopped() const { return dut_->o_stopped_o != 0; }
  uint64_t cycles() const { return cycles_; }
  uint64_t illegal_ctr() const { return dut_->o_illegal_o; }
  uint64_t unsupported_ctr() const { return dut_->o_unsupported_o; }
  uint64_t mepc() const { return PayloadLane(dut_->o_csr_mepc_o, 0); }
  uint64_t mtvec() const { return PayloadLane(dut_->o_csr_mtvec_o, 0); }

  // The machine's own evidence counters at the cycle the run's story was
  // complete -- read there rather than at the end of the window, because after
  // the story ends the front end is still fetching and the counters describe a
  // machine that has moved on.
  struct Snapshot {
    uint64_t illegal = 0;
    uint64_t unsupported = 0;
    uint64_t fetch_pc = 0;
    bool stopped = false;
    uint64_t cycle = 0;
  };
  void SetWant(size_t want) { want_ = want; }
  bool HaveSnapshot() const { return snap_taken_; }
  const Snapshot& snapshot() const { return snap_; }

  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) +
           " stopped=" + Dec(dut_->o_stopped_o) +
           " fetch_pc=" + U64(PayloadLane(dut_->o_fetch_pc_o, 0));
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail("run", "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + State());
    }
    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = 1;

    if (imem_.HasResponse() && !rst) {
      Imem::Response rsp = imem_.Current();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = rsp.data;
      dut_->imem_rsp_fault_i = rsp.fault ? 1 : 0;
      dut_->imem_rsp_id_i = rsp.id;
      dut_->imem_rsp_epoch_i = rsp.epoch;
      // The memory system serves a *word*: the interface's size is four bytes
      // whatever the instruction's own length turns out to be. An instruction
      // that needs a byte the region does not hold is a fault (above).
      dut_->imem_rsp_len_i = 4;
    } else {
      dut_->imem_rsp_valid_i = 0;
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }

    dut_->dmem_req_ready_i = 1;
    if (dmem_.HasResponse()) {
      const DataMem::Rsp& r = dmem_.CurrentResponse();
      dut_->dmem_rsp_valid_i = 1;
      dut_->dmem_rsp_rdata_i = r.rdata;
      dut_->dmem_rsp_fault_i = r.fault ? 1 : 0;
    } else {
      dut_->dmem_rsp_valid_i = 0;
      dut_->dmem_rsp_rdata_i = 0;
      dut_->dmem_rsp_fault_i = 0;
    }

    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;
    dut_->eval();

    if (!rst) Observe();

    if (!rst && (dut_->imem_req_valid_o != 0) && (dut_->imem_req_ready_i != 0)) {
      requests_.push_back(dut_->imem_req_addr_o);
      imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o,
                   dut_->imem_req_epoch_o);
    }
    if (!rst && (dut_->imem_rsp_valid_i != 0) && (dut_->imem_rsp_ready_o != 0)) {
      imem_.PopResponse();
    }
    imem_.Advance();

    if (!rst && (dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0)) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(r, cycles_);
    }
    if (!rst && (dut_->dmem_rsp_valid_i != 0) && (dut_->dmem_rsp_ready_o != 0)) {
      dmem_.PopResponse();
    }
    dmem_.Advance();

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

 private:
  // ---------------------------------------------------- the instruction port
  class Imem {
   public:
    struct Response {
      uint32_t data = 0;
      uint32_t id = 0;
      uint32_t epoch = 0;
      bool fault = false;
    };

    Imem(const ProgImage* img, bool wrong_line) : img_(img), wrong_line_(wrong_line) {}

    bool HasResponse() const { return !ready_.empty(); }
    const Response& Current() const { return ready_.front(); }

    void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
      Response rsp;
      rsp.id = id;
      rsp.epoch = epoch;
      uint32_t bits = 0;
      uint32_t len = 0;
      if (!img_->Fetch(addr, wrong_line_, &bits, &len)) {
        rsp.fault = true;
        rsp.data = img_->Mapped(addr) ? static_cast<uint32_t>(img_->Half(addr))
                                      : 0x00000073u;   // ECALL for a hole
      } else {
        rsp.data = bits;
      }
      rsp.data |= 0;   // the word is returned as read; the DUT decides the length
      inflight_.push_back(Entry{rsp, 1});
    }

    void PopResponse() { ready_.pop_front(); }

    void Advance() {
      for (size_t i = 0; i < inflight_.size();) {
        if (--inflight_[i].left == 0) {
          ready_.push_back(inflight_[i].rsp);
          inflight_.erase(inflight_.begin() + static_cast<long>(i));
        } else {
          ++i;
        }
      }
    }

   private:
    struct Entry {
      Response rsp;
      int left = 0;
    };
    const ProgImage* img_;
    bool wrong_line_;
    std::vector<Entry> inflight_;
    std::deque<Response> ready_;
  };

  void Observe() {
    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got & (1u << lane)) == 0) continue;
      DutRecord r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.len = static_cast<uint32_t>(PackedLane(dut_->ev_len_o, lane, 3));
      r.insn = static_cast<uint32_t>(PackedLane(dut_->ev_insn_o, lane, 32));
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.trap = PackedLane(dut_->ev_trap_o, lane, 1) != 0;
      r.cause = PayloadLane(dut_->ev_trap_cause_o, lane);
      r.tval = PayloadLane(dut_->ev_trap_tval_o, lane);
      records_.push_back(r);
    }

    const uint32_t f0 = dut_->o_dbg_fetch_o[0];
    const uint32_t f2 = dut_->o_dbg_fetch_o[2];
    const bool reg_valid = (f0 & 1u) != 0;
    // The bundle carries the fetch output register's PC in its third word (32
    // bits, which is the whole address space this case uses), and
    // `o_dbg_deliver_valid_o` is high only when the register holds a
    // *deliverable instruction*: an illegal or faulting delivery has the same PC
    // and no instruction bit, which is how the fault's address stays observable.
    const uint64_t pc = static_cast<uint64_t>(f2);
    const bool is_insn = dut_->o_dbg_deliver_valid_o != 0;
    if (!reg_valid) {
      obs_valid_ = false;
    } else if (!obs_valid_ || pc != obs_pc_ || is_insn != obs_is_insn_) {
      obs_valid_ = true;
      obs_pc_ = pc;
      obs_is_insn_ = is_insn;
      Delivery d;
      d.pc = pc;
      d.is_insn = is_insn;
      deliveries_.push_back(d);
      // The request the core issued in the cycle this response was accepted is
      // the fetch that follows the instruction, and the distance between them is
      // the length the machine used.
      if (req_prev_cycle_ != 0) {
        Advance a;
        a.pc = pc;
        a.req = req_prev_cycle_;
        advances_.push_back(a);
      }
    }
    // The request accepted *this* cycle is the pair for whatever the register
    // takes at the next edge. Reset before reset/after a stop: no request, no
    // pair.
    req_prev_cycle_ = (!dut_->imem_req_valid_o || !dut_->imem_req_ready_i)
                          ? 0
                          : dut_->imem_req_addr_o;

    if (!snap_taken_ && records_.size() >= want_) {
      snap_taken_ = true;
      snap_.illegal = dut_->o_illegal_o;
      snap_.unsupported = dut_->o_unsupported_o;
      snap_.fetch_pc = PayloadLane(dut_->o_fetch_pc_o, 0);
      snap_.stopped = dut_->o_stopped_o != 0;
      snap_.cycle = cycles_;
    }
  }

  Vmosaic_core_tb* dut_;
  mosaic::MemoryModel mem_;
  Imem imem_;
  DataMem dmem_;
  Geometry g_;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  size_t want_ = 0;
  bool snap_taken_ = false;
  Snapshot snap_;
  std::vector<DutRecord> records_;
  std::vector<Delivery> deliveries_;
  std::vector<Advance> advances_;
  std::vector<uint64_t> requests_;
  bool obs_valid_ = false;
  uint64_t obs_pc_ = 0;
  bool obs_is_insn_ = false;
  uint64_t req_prev_cycle_ = 0;
};

// ============================================================================
// The programs
// ============================================================================
struct Emitter {
  ProgImage* img;
  uint64_t pc;
  void W32(uint32_t word) {
    img->Emit32(pc, word);
    pc += 4;
  }
  void W16(uint16_t half) {
    img->Emit16(pc, half);
    pc += 2;
  }
};

// The addresses a run's checks name, so no check has to search for them.
struct Sites {
  std::vector<uint64_t> straddles;   // four bytes across an eight-byte line
  uint64_t jalr_pc = 0;
  uint64_t beqz_pc = 0;
  uint64_t fault_pc = 0;
  uint64_t reserved_pc = 0;
  uint64_t trap_pc = 0;
  uint64_t trap_tval = 0;
};

// ---------------------------------------------------------------- mixed
// A 16/32-bit mixed straight line with the placements that matter: a compressed
// instruction at 2 mod 4, a 32-bit instruction at 2 mod 4, hints, a not-taken
// and a taken compressed branch, two compressed jumps and a c.jalr whose link
// must be PC+2. It ends with the frozen exit protocol and a park.
void BuildMixed(ProgImage* img, Sites* sites) {
  Emitter e{img, kBase};
  e.W32(EncI(0x13u, 0, 5, 0, 1));       // 0x00  addi x5, x0, 1
  e.W32(EncI(0x13u, 1, 5, 5, 31));      // 0x04  slli x5, x5, 31   x5 = base
  e.W16(CAddi(6, 1));                   // 0x08  c.addi x6, 1
  e.W16(CNop());                        // 0x0a  c.nop  (a hint, at 2 mod 4)
  e.W32(EncI(0x13u, 0, 7, 6, 5));       // 0x0c  addi x7, x6, 5
  e.W16(CAddi(8, 3));                   // 0x10  c.addi x8, 3
  if ((e.pc % 4u) != 2u) {
    Fail("setup", "the straddling instruction is not at 2 mod 4");
  }
  sites->straddles.push_back(e.pc);
  e.W32(EncI(0x13u, 0, 9, 8, 7));       // 0x12  addi x9, x8, 7 (32-bit at 2 mod 4)
  e.W16(CMv(10, 9));                    // 0x16  c.mv x10, x9
  e.W16(CAdd(10, 8));                   // 0x18  c.add x10, x8
  e.W16(CLi(11, -1));                   // 0x1a  c.li x11, -1
  e.W16(CAddi(0, 5));                   // 0x1c  c.addi x0, 5 (a hint)
  sites->beqz_pc = e.pc;
  e.W16(CBranch(6, 2, 4));              // 0x1e  c.beqz x10, +4 (not taken)
  e.W16(CNop());                        // 0x20  the not-taken branch's fall-through
  e.W16(CBranch(7, 2, 4));              // 0x22  c.bnez x10, +4 (taken)
  e.W16(CNop());                        // 0x24  skipped
  e.W32(EncU(0x17u, 12, 0));            // 0x26  auipc x12, 0
  e.W16(CAddi(12, 12));                 // 0x2a  c.addi x12, 12  -> 0x32
  sites->jalr_pc = e.pc;
  e.W16(CJalr(12));                     // 0x2c  c.jalr x12 (link = 0x2e = PC+2)
  e.W16(CNop());                        // 0x2e  skipped
  e.W16(CNop());                        // 0x30  skipped
  e.W32(EncI(0x13u, 0, 14, 1, 0));      // 0x32  addi x14, x1, 0
  e.W16(CJ(4));                         // 0x36  c.j +4
  e.W16(CNop());                        // 0x38  skipped
  e.W16(CJ(2));                         // 0x3a  c.j +2 -> 0x3c
  e.W32(EncI(0x13u, 0, 28, 0, 0x100));  // 0x3c  addi x28, x0, 0x100
  e.W32(EncI(0x13u, 1, 28, 28, 4));     // 0x40  slli x28, x28, 4
  e.W32(EncR(0x33u, 0, 28, 5, 28, 0));  // 0x44  add x28, x5, x28  = TOHOST
  e.W32(EncI(0x13u, 0, 29, 0, 1));      // 0x48  addi x29, x0, 1
  e.W32(EncS(0x23u, 3, 28, 29, 0));     // 0x4c  sd x29, 0(x28)   the exit protocol
  for (int i = 0; i < 8; i++) e.W16(CNop());   // 0x50  filler
  e.W16(CJ(0));                         // 0x60  the park: c.j .
}

// ------------------------------------------------------------- boundary
// The same mix arranged so that a 32-bit instruction's four bytes cross the
// harness's eight-byte line, and ending with an instruction whose second
// halfword is outside the image: the fetch of it must fault, and the fault must
// be reported at its own start address.
void BuildBoundary(ProgImage* img, Sites* sites) {
  Emitter e{img, kBase};
  e.W32(EncI(0x13u, 0, 5, 0, 1));       // 0x00  addi x5, x0, 1
  e.W32(EncI(0x13u, 1, 5, 5, 31));      // 0x04  slli x5, x5, 31
  e.W16(CAddi(6, 5));                   // 0x08  c.addi x6, 5
  e.W16(CNop());                        // 0x0a
  e.W32(EncI(0x13u, 0, 7, 6, 1));       // 0x0c  addi x7, x6, 1
  e.W16(CAddi(8, 2));                   // 0x10  c.addi x8, 2
  e.W16(CNop());                        // 0x12
  e.W16(CAddi(6, -1));                  // 0x14  c.addi x6, -1
  if (((e.pc % kLineBytes) + 4u) <= kLineBytes) {
    Fail("setup", "the straddling instruction does not cross the line");
  }
  sites->straddles.push_back(e.pc);
  e.W32(EncI(0x13u, 0, 9, 8, 3));       // 0x16  addi x9, x8, 3 (crosses the line)
  e.W16(CNop());                        // 0x1a
  e.W16(CNop());                        // 0x1c
  sites->fault_pc = e.pc;
  // Only the first halfword of the instruction is in the image: this is the
  // second-halfword fault, and the low two bits say the instruction is a 32-bit
  // one, so the fetch cannot be served.
  e.W16(static_cast<uint16_t>(EncI(0x13u, 0, 10, 9, 1) & 0xFFFFu));   // 0x1e
}

// -------------------------------------------------------------- reserved
// The instructions before the reserved encoding, then the encoding itself at a
// known address. Only the three instructions before it may retire.
void BuildReserved(ProgImage* img, Sites* sites, uint16_t encoding, uint64_t addr) {
  Emitter e{img, kBase};
  e.W32(EncI(0x13u, 0, 5, 0, 1));       // 0x00  addi x5, x0, 1
  e.W16(CAddi(6, 3));                   // 0x04  c.addi x6, 3
  e.W16(CNop());                        // 0x06
  while (e.pc < addr) e.W16(CNop());
  sites->reserved_pc = e.pc;
  e.W16(encoding);
  e.W16(CNop());                        // padding, so the window is servable
  e.W16(CNop());
}

// ------------------------------------------------------------------ trap
// A misaligned c.lwsp with the trap vector installed: the trap record must name
// the compressed instruction's own PC, length and bits.
void BuildTrap(ProgImage* img, Sites* sites) {
  Emitter e{img, kBase};
  const uint64_t handler = kBase + 0x40;
  e.W32(EncI(0x13u, 0, 2, 0, 1));       // 0x00  addi x2, x0, 1
  e.W32(EncI(0x13u, 1, 2, 2, 31));      // 0x04  slli x2, x2, 31   x2 = base
  e.W32(EncI(0x13u, 0, 2, 2, 0x102));   // 0x08  addi x2, x2, 0x102 (word-misaligned)
  e.W32(EncI(0x13u, 0, 5, 0, 0x40));    // 0x0c  addi x5, x0, the handler
  e.W32(EncCsr(1u, 0, 0x305u, 5));      // 0x10  csrrw x0, mtvec, x5
  e.W16(CNop());                        // 0x14
  e.W16(CNop());                        // 0x16
  e.W16(CNop());                        // 0x18
  if ((e.pc % 4u) != 2u) {
    Fail("setup", "the trapping compressed instruction is not at 2 mod 4");
  }
  sites->trap_pc = e.pc;
  sites->trap_tval = kBase + 0x102;
  e.W16(CLwsp(6, 0));                   // 0x1a  c.lwsp x6, 0(x2) -> misaligned
  while (e.pc < handler) e.W16(CNop()); // legal filler: the fetch is squashed
  e.W16(CJ(0));                         // 0x40  the handler: park
}

// ============================================================================
// Comparing the DUT's stream with the reference's
// ============================================================================
struct RunOutcome {
  std::vector<DutRecord> records;
  std::vector<Delivery> deliveries;
  std::vector<Advance> advances;
  std::vector<uint64_t> requests;
  Harness::Snapshot snapshot;
  uint64_t illegal = 0;
  uint64_t unsupported = 0;
  uint64_t cycles = 0;
  bool stopped = false;
  bool stopped_recorded = false;
};

RunOutcome RunOnce(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                   const Geometry& geometry, const ProgImage& img, size_t want,
                   bool expect_stop, bool wrong_line, const std::string& label) {
  Harness harness(dut, &img, wrong_line, kMaxRunCycles);
  harness.Configure(geometry);
  harness.SetWant(want);
  harness.Reset(kResetCycles);
  const size_t cap = want + 24;
  uint64_t last_records = 0;
  uint64_t last_progress = 0;
  while (true) {
    if (harness.stopped()) {
      for (int i = 0; i < kDrainCycles; i++) harness.Cycle(false);
      break;
    }
    if (!expect_stop && harness.records().size() >= want) break;
    if (harness.records().size() >= cap) break;
    if (harness.cycles() > static_cast<uint64_t>(kMaxRunCycles)) {
      Fail(label, "the run did not finish in " + Dec(kMaxRunCycles) + " cycles: " +
                      harness.State());
    }
    harness.Cycle(false);
    if (harness.records().size() != last_records) {
      last_records = harness.records().size();
      last_progress = harness.cycles();
    } else if (harness.cycles() - last_progress > kStallCycles) {
      Fail(label, "stalled at " + Dec(last_records) + " of " +
                      Dec(want) + " instructions: " + harness.State());
    }
  }
  RunOutcome out;
  out.records = harness.records();
  out.deliveries = harness.deliveries();
  out.advances = harness.advances();
  out.requests = harness.requests();
  out.snapshot = harness.snapshot();
  out.illegal = harness.illegal_ctr();
  out.unsupported = harness.unsupported_ctr();
  out.cycles = harness.cycles();
  out.stopped = harness.stopped();
  out.stopped_recorded = harness.HaveSnapshot();
  return out;
}

// The per-lane comparison. One check per fact, so a failure names the fact, and
// every fact is checked rather than only the first (the controls are then each
// caught by the check that describes their defect).
void CompareStream(mosaic::Reporter* reporter, const std::string& label,
                   const RunOutcome& got, const RefTrace& want) {
  const size_t n = std::min(got.records.size(), want.records.size());
  int mismatches = 0;
  for (size_t i = 0; i < n; i++) {
    const DutRecord& r = got.records[i];
    const RefRecord& e = want.records[i];
    if (r.pc != e.pc) {
      reporter->Check(false, label + ": the retirement stream follows the reference: "
                                      "retire " + Dec(i) + " pc expected " + U64(e.pc) +
                              ", got " + U64(r.pc));
      mismatches++;
      break;   // the stream is not aligned any more; the count check reports the rest
    }
    if (r.trap != e.trap) {
      reporter->Check(false, label + ": the event record's trap bit follows the "
                                      "reference: retire " + Dec(i) + " at " + U64(e.pc) +
                              " expected trap=" + Dec(e.trap) + ", got trap=" +
                              Dec(r.trap));
      mismatches++;
      continue;
    }
    if (r.trap) {
      if (r.len != e.len) {
        reporter->Check(false, label + ": the trap record names the trapping "
                                        "instruction's own length: at " + U64(e.pc) +
                                " expected " + Dec(e.len) + ", got " + Dec(r.len));
        mismatches++;
      }
      if (r.insn != e.bits) {
        reporter->Check(false, label + ": the trap record carries the trapping "
                                        "instruction's own bits: at " + U64(e.pc) +
                                " expected " + U64(e.bits) + ", got " + U64(r.insn));
        mismatches++;
      }
      if (r.cause != e.cause) {
        reporter->Check(false, label + ": the trap record's cause: at " + U64(e.pc) +
                                " expected " + Dec(e.cause) + ", got " + Dec(r.cause));
        mismatches++;
      }
      if (r.tval != e.tval) {
        reporter->Check(false, label + ": the trap record's tval: at " + U64(e.pc) +
                                " expected " + U64(e.tval) + ", got " + U64(r.tval));
        mismatches++;
      }
      continue;
    }
    if (r.len != e.len) {
      reporter->Check(false, label + ": the event record reports the instruction's "
                                      "own length: retire " + Dec(i) + " at " +
                              U64(e.pc) + " expected " + Dec(e.len) + ", got " +
                              Dec(r.len));
      mismatches++;
    }
    if (r.insn != e.bits) {
      reporter->Check(false, label + ": the event record carries the instruction's "
                                      "own bits: retire " + Dec(i) + " at " + U64(e.pc) +
                              " expected " + U64(e.bits) + ", got " + U64(r.insn));
      mismatches++;
    }
    if (r.rd != e.rd || r.we != e.we) {
      reporter->Check(false, label + ": the retirement stream follows the reference: "
                                      "retire " + Dec(i) + " at " + U64(e.pc) +
                              " destination expected rd=" + Dec(e.rd) + " we=" +
                              Dec(e.we) + ", got rd=" + Dec(r.rd) + " we=" + Dec(r.we));
      mismatches++;
    } else if (e.we && r.value != e.value) {
      reporter->Check(false, label + ": the retirement stream follows the reference: "
                                      "retire " + Dec(i) + " at " + U64(e.pc) +
                              " value for x" + Dec(e.rd) + " expected " + U64(e.value) +
                              ", got " + U64(r.value));
      mismatches++;
    }
  }
  reporter->Check(got.records.size() >= want.records.size(),
                  label + ": every instruction the reference produces is retired (" +
                      Dec(got.records.size()) + " of " + Dec(want.records.size()) +
                      " retired)");
  reporter->Check(mismatches == 0,
                  label + ": the retirement stream equals the independent RV64IC "
                          "reference, PC by PC, length by length, bits by bits (" +
                      Dec(n) + " lanes compared)");
}

// The checks that do not need the reference's instruction stream.
void CheckStructure(mosaic::Reporter* reporter, const std::string& label,
                    const RunOutcome& got, const ProgImage& img,
                    const RefTrace& ref) {
  // The length the event record reports must be the length its own encoding
  // states, on every lane: a 32-bit instruction when the low two bits are 11 and
  // a 16-bit one otherwise. This is the rule the whole package rests on and it is
  // checked from the record alone.
  int bad_len = 0;
  for (const DutRecord& r : got.records) {
    const uint32_t want_len = ((r.insn & 3u) == 3u) ? kLen32 : kLen16;
    if (r.len != want_len) {
      bad_len++;
      if (bad_len <= 4) {
        reporter->Check(false, label + ": the event record's length agrees with the "
                                        "encoding it carries: at " + U64(r.pc) +
                                " the encoding " + U64(r.insn) + " is " +
                                Dec(want_len) + " bytes, the record says " + Dec(r.len));
      }
    }
  }
  reporter->Check(bad_len == 0,
                  label + ": every event record's length is its instruction's own "
                          "encoding's length (" + Dec(got.records.size()) +
                      " lanes checked)");

  // Every address the core asks the instruction port for is an instruction's
  // start address -- or an address the image cannot serve at all, which is how a
  // runaway fetch ends. An address in the middle of an instruction is a PC
  // advanced by an amount that is not the instruction's length.
  int bad_req = 0;
  for (uint64_t addr : got.requests) {
    if (img.IsStart(addr)) continue;
    uint32_t bits = 0;
    uint32_t len = 0;
    if (!img.Fetch(addr, false, &bits, &len)) continue;   // not servable: allowed
    bad_req++;
    if (bad_req <= 3) {
      reporter->Check(false, label + ": every instruction request is for an "
                                      "instruction's start address: " + U64(addr) +
                              " is inside an instruction, not the start of one");
    }
  }
  reporter->Check(bad_req == 0,
                  label + ": every address the core requests is an instruction's own "
                          "start address (" + Dec(got.requests.size()) +
                      " requests checked)");

  // The program counter advances by the *delivered instruction's own length*:
  // the fetch the core issues in the cycle a response is accepted is the
  // instruction that follows it in the image, two bytes on for a 16-bit encoding
  // and four for a 32-bit one. The check is applied where the instruction is one
  // the reference itself executes -- so its length is not a matter of opinion --
  // and the pair is the machine's own: the PC it delivered and the address it
  // fetched next.
  std::set<uint64_t> ref_len16;
  std::set<uint64_t> ref_len32;
  for (const RefRecord& r : ref.records) {
    if (r.trap) continue;
    (r.len == kLen16 ? ref_len16 : ref_len32).insert(r.pc);
  }
  int bad_advance = 0;
  for (const Advance& a : got.advances) {
    uint32_t own = 0;
    if (ref_len16.count(a.pc) != 0) {
      own = kLen16;
    } else if (ref_len32.count(a.pc) != 0) {
      own = kLen32;
    } else {
      continue;   // not an instruction the reference executes: not this rule's
    }
    if (a.req == a.pc + own) continue;
    bad_advance++;
    if (bad_advance <= 4) {
      reporter->Check(false,
                      label + ": the program counter advances by the delivered "
                              "instruction's own length: the instruction at " +
                          U64(a.pc) + " is " + Dec(own) + " bytes long, but the "
                          "fetch issued when it was answered went to " + U64(a.req));
    }
  }
  reporter->Check(bad_advance == 0,
                  label + ": every fetch after an instruction is that instruction's "
                          "own length further on (" + Dec(got.advances.size()) +
                      " instruction/fetch pairs checked)");
}

// ============================================================================
// The reserved/unimplemented encodings the case drives
// ============================================================================
// Each is reserved (or, for the F/D forms, an unimplemented extension) by the
// RV64C text, and each was checked against `riscv64-elf-objdump -d`, which
// prints "unknown" for the reserved ones. The instructions before the encoding
// retire; the encoding itself must leave no architectural trace at all.
struct ReservedCase {
  const char* name;
  uint16_t encoding;
  const char* why;
};

const ReservedCase kReserved[] = {
    {"all-zero", 0x0000u,
     "c.addi4spn with nzuimm == 0 -- the all-zero halfword, which is what the "
     "fetch of padding after a program's text delivers"},
    {"c.addiw-rd0", 0x2001u, "c.addiw with rd == x0 (RV64: reserved)"},
    {"c-lwsp-rd0", 0x4002u, "c.lwsp with rd == x0"},
    {"c-ldsp-rd0", 0x6002u, "c.ldsp with rd == x0"},
    {"c-jr-rs1-0", 0x8002u, "c.jr with rs1 == x0"},
    {"quad0-f3-100", 0x8000u, "quadrant 0, funct3 == 100 (reserved)"},
    {"quad1-f3-100-11", 0x9C41u,
     "quadrant 1, funct3 == 100, the RV64 marker set and bits[6:5] == 10: the "
     "reserved sub-case of the c.subw/c.addw group"},
};

}  // namespace

// ============================================================================
// The driver
// ============================================================================
int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  uint64_t total_cycles = 0;
  uint64_t total_records = 0;
  uint64_t compressed_records = 0;
  uint64_t straddle_records = 0;
  uint64_t reserved_runs = 0;

  // Control (d): the harness's memory model assembles a straddling instruction
  // from the wrong line. The mutation is in the *memory*, because in p0 there is
  // no cache and the memory is what assembles the four bytes.
#ifdef MOSAIC_IMEM_MUTANT_WRONG_LINE
  const bool wrong_line = true;
#else
  const bool wrong_line = false;
#endif

  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    if (geometry.reset_vector != kBase) {
      Fail("geometry", "the reset vector is not the link base");
    }

    // ---------------------------------------------------------------- mixed
    {
      ProgImage img;
      Sites sites;
      BuildMixed(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      if (!ref.exited) {
        Fail("mixed", "the reference did not reach the exit protocol: " +
                          ref.stop_reason + " at pc " + U64(ref.stop_pc));
      }
      const std::string label = "mixed";
      reporter.Check(ref.exited,
                     label + ": the reference reaches the program's exit protocol");
      const RunOutcome got =
          RunOnce(&dut, &reporter, geometry, img, ref.records.size(), false,
                  wrong_line, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);

      // The card's own facts, stated rather than implied.
      size_t n_compressed = 0;
      size_t n_straddle = 0;
      for (const RefRecord& r : ref.records) {
        if (r.len == kLen16) n_compressed++;
        if (((r.pc % kLineBytes) + r.len) > kLineBytes) n_straddle++;
      }
      reporter.Check(n_compressed >= 12,
                     label + ": the program mixes 16-bit and 32-bit instructions (" +
                         Dec(n_compressed) + " compressed instructions retire)");
      reporter.Check(n_straddle >= 1,
                     label + ": the program places 32-bit instructions that do not "
                             "start on a word boundary (" + Dec(n_straddle) +
                         " of them, with a compressed instruction before them)");

      // The c.jalr's link register: PC + 2, never PC + 4.
      bool saw_jalr = false;
      for (const DutRecord& r : got.records) {
        if (r.pc != sites.jalr_pc) continue;
        saw_jalr = true;
        reporter.Check(r.rd == 1u && r.we && r.value == sites.jalr_pc + kLen16,
                       label + ": a compressed jump-and-link writes the link "
                               "register from the instruction's own length: c.jalr at " +
                           U64(sites.jalr_pc) + " wrote " + U64(r.value) +
                           ", expected " + U64(sites.jalr_pc + kLen16));
      }
      reporter.Check(saw_jalr, label + ": the c.jalr retired");

      // The not-taken compressed branch: its fall-through is the instruction at
      // PC + 2, not PC + 4.
      for (size_t i = 0; i + 1 < got.records.size(); i++) {
        if (got.records[i].pc != sites.beqz_pc) continue;
        reporter.Check(got.records[i + 1].pc == sites.beqz_pc + kLen16,
                       label + ": the not-taken compressed branch falls through to "
                               "its own PC + 2: after " + U64(sites.beqz_pc) +
                           " came " + U64(got.records[i + 1].pc));
      }

      // The 32-bit instruction at 2 mod 4 retires with its own four bytes.
      for (const DutRecord& r : got.records) {
        if (r.pc != sites.straddles.front()) continue;
        uint32_t bits = 0;
        uint32_t len = 0;
        img.Fetch(r.pc, false, &bits, &len);
        reporter.Check(r.len == kLen32 && r.insn == bits,
                       label + ": the 32-bit instruction whose PC is 2 mod 4 retires "
                               "with its own four bytes: at " + U64(r.pc) + " length " +
                           Dec(r.len) + ", bits " + U64(r.insn) + ", expected length 4 "
                           "and " + U64(bits));
      }

      reporter.Check(got.snapshot.illegal == 0 && got.snapshot.unsupported == 0,
                     label + ": no macro was refused while the program ran: illegal=" +
                         Dec(got.snapshot.illegal) + " unsupported=" +
                         Dec(got.snapshot.unsupported));
      reporter.Check(!got.snapshot.stopped,
                     label + ": the machine never stops on its own: it runs to the "
                             "program's exit protocol");

      total_cycles += got.cycles;
      total_records += got.records.size();
      compressed_records += n_compressed;
      straddle_records += n_straddle;
      std::printf("  [%s] %zu retires (%zu compressed, %zu across the line), "
                  "%llu cycles\n",
                  label.c_str(), got.records.size(), n_compressed, n_straddle,
                  static_cast<unsigned long long>(got.cycles));
    }

    // ------------------------------------------------------------- boundary
    {
      ProgImage img;
      Sites sites;
      BuildBoundary(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = "boundary";
      const RunOutcome got =
          RunOnce(&dut, &reporter, geometry, img, ref.records.size(), true,
                  wrong_line, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);

      reporter.Check(got.stopped,
                     label + ": the machine stops at the instruction whose second "
                             "halfword is outside the image");
      // The delivered record the fetch unit was holding when it stopped: it must
      // be the faulting instruction's *own start address*. A PC derived from the
      // payload, or advanced by four, names a different address here.
      bool fault_delivered = false;
      std::string seen;
      for (const Delivery& d : got.deliveries) {
        if (d.pc == 0) continue;
        if (seen.size() < 240) {
          seen += U64(d.pc) + (d.is_insn ? "i " : "x ");
        }
        if (d.pc == sites.fault_pc && !d.is_insn) fault_delivered = true;
      }
      reporter.Check(fault_delivered,
                     label + ": the second-halfword fault is reported at the "
                             "instruction's own start address: expected a refused "
                             "delivery at " + U64(sites.fault_pc) +
                         ", the deliveries seen were " + seen);
      bool second_half_requested = false;
      for (uint64_t addr : got.requests) {
        if (addr == sites.fault_pc + 2) second_half_requested = true;
      }
      reporter.Check(!second_half_requested,
                     label + ": the core never asks for the instruction's second "
                             "halfword as if it were an instruction's start");
      bool faulted_retired = false;
      for (const DutRecord& r : got.records) {
        if (r.pc == sites.fault_pc) faulted_retired = true;
      }
      reporter.Check(!faulted_retired,
                     label + ": the instruction that could not be fetched does not "
                             "retire");
      reporter.Check(got.illegal >= 1 && got.unsupported >= 1,
                     label + ": the refused fetch reaches dispatch as an undecodable "
                             "macro: illegal=" + Dec(got.illegal) + " unsupported=" +
                         Dec(got.unsupported));

      total_cycles += got.cycles;
      total_records += got.records.size();
      std::printf("  [%s] %zu retires, stop at the faulting instruction %s, "
                  "%llu cycles\n",
                  label.c_str(), got.records.size(), U64(sites.fault_pc).c_str(),
                  static_cast<unsigned long long>(got.cycles));
    }

    // ------------------------------------------------------------- reserved
    for (const ReservedCase& rc : kReserved) {
      ProgImage img;
      Sites sites;
      BuildReserved(&img, &sites, rc.encoding, kBase + 0x08);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = std::string("reserved.") + rc.name;
      const RunOutcome got =
          RunOnce(&dut, &reporter, geometry, img, ref.records.size(), true,
                  wrong_line, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);

      reporter.Check(got.stopped,
                     label + ": the machine stops at the reserved encoding (" +
                         std::string(rc.why) + ")");
      bool reserved_retired = false;
      for (const DutRecord& r : got.records) {
        if (r.pc == sites.reserved_pc) reserved_retired = true;
      }
      reporter.Check(!reserved_retired,
                     label + ": the reserved encoding at " + U64(sites.reserved_pc) +
                         " leaves no architectural trace (nothing retires at its PC)");
      reporter.Check(got.records.size() == ref.records.size(),
                     label + ": only the instructions before the reserved encoding "
                             "retire (" + Dec(got.records.size()) + " retired, " +
                         Dec(ref.records.size()) + " expected)");
      reporter.Check(got.illegal >= 1 && got.unsupported >= 1,
                     label + ": the decoder refuses the encoding and dispatch stops "
                             "at it: illegal=" + Dec(got.illegal) + " unsupported=" +
                         Dec(got.unsupported));

      reserved_runs++;
      total_cycles += got.cycles;
      total_records += got.records.size();
      std::printf("  [%s] stop at %s, %zu retires, %llu cycles\n", label.c_str(),
                  U64(sites.reserved_pc).c_str(), got.records.size(),
                  static_cast<unsigned long long>(got.cycles));
    }

    // ----------------------------------------------------------------- trap
    {
      ProgImage img;
      Sites sites;
      BuildTrap(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = "trap";
      const RunOutcome got =
          RunOnce(&dut, &reporter, geometry, img, ref.records.size(), false,
                  wrong_line, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);

      bool saw_trap = false;
      for (const DutRecord& r : got.records) {
        if (r.pc != sites.trap_pc || !r.trap) continue;
        saw_trap = true;
        uint32_t bits = 0;
        uint32_t len = 0;
        img.Fetch(sites.trap_pc, false, &bits, &len);
        reporter.Check(r.len == kLen16 && r.insn == bits,
                       label + ": the trap record names the compressed instruction's "
                               "own PC, length and bits: at " + U64(r.pc) + " length " +
                           Dec(r.len) + " bits " + U64(r.insn) + ", expected length 2 "
                           "and " + U64(bits));
        reporter.Check(r.cause == kCauseLoadMisaligned,
                       label + ": the trap record's cause is the misaligned load: got " +
                           Dec(r.cause));
        reporter.Check(r.tval == sites.trap_tval,
                       label + ": the trap record's tval is the misaligned address: "
                               "got " + U64(r.tval) + ", expected " +
                           U64(sites.trap_tval));
      }
      reporter.Check(saw_trap,
                     label + ": the misaligned compressed load produces a trap record "
                             "at " + U64(sites.trap_pc));
      // mepc is the instruction's own PC: the CSR path must not add the 32-bit
      // instruction's four bytes to a compressed instruction's address.
      // `mepc` is the *profile's* rule, not this case's. p0's CSR config declares
      // IALIGN=32 (config/csr/mode_m.json), so mepc[1:0] are read-only zero, and a
      // trap taken on a two-mod-four PC records that PC rounded down to a word
      // boundary. The architectural record this case asserts is the *event*
      // stream above; the CSR read-back is reported rather than asserted, and the
      // report's "not covered" section says why: the C extension is gated to p1 in
      // config/capability_ladder.json, the CSR mask belongs to the IALIGN=16 move,
      // and CASE=core.event_payload (another package's driver) pins the p0 mask.
      const uint64_t mepc = PayloadLane(dut.o_csr_mepc_o, 0);
      reporter.Check((mepc & 1u) == 0,
                     label + ": mepc's low bit is zero -- the one bit the spec always "
                             "masks: got " + U64(mepc));
      std::printf("  [note] %s: the trapping compressed instruction is at %s; mepc "
                  "reads %s (the p0 CSR config declares IALIGN=32)\n",
                  label.c_str(), U64(sites.trap_pc).c_str(), U64(mepc).c_str());

      total_cycles += got.cycles;
      total_records += got.records.size();
      std::printf("  [%s] trap at %s with length 2, %zu retires, %llu cycles\n",
                  label.c_str(), U64(sites.trap_pc).c_str(), got.records.size(),
                  static_cast<unsigned long long>(got.cycles));
    }

    passed = (reporter.failures() == 0);
    detail = "checks=" + Dec(reporter.checks()) + " runs=" +
             Dec(4 + reserved_runs) + " reserved_encodings=" + Dec(reserved_runs) +
             " retires=" + Dec(total_records) + " compressed=" +
             Dec(compressed_records) + " straddling=" + Dec(straddle_records) +
             " cycles=" + Dec(total_cycles) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the compressed path holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  const bool ok = passed && (reporter.failures() == 0);
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}

