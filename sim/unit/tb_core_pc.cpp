// ============================================================================
// tb_core_pc.cpp -- CASE=pc.branch_and_fetch_visibility, work package V-016.
//
// The DUT is the integrated p0 core the other `mosaic_core_tb` cases drive. This
// case is about the program counter and about what a fetch sees:
//
//   * the PC of every retired instruction, instruction by instruction, against an
//     independent RV64IM reference written in this file from the ISA text;
//   * the link value JAL and JALR write (always `pc + the instruction's own
//     length`, four bytes here);
//   * the address a taken transfer reaches, and the fact that JALR clears bit 0
//     of its computed target and *nothing else*;
//   * the fact that the program counter advances by the instruction's own length,
//     not by a constant, and never by a masked address;
//   * what the front end sees after FENCE.I: the bytes the store published.
//
// ------------------------------------------------------------- the five runs
//
// Every run has its own image and its own reference, so nothing is inherited:
//
//   transfers  a JAL forward, three conditional branches (two not taken, one
//              taken) and a JALR whose computed target has bit 0 SET. The link
//              values and the taken target are checked instruction by
//              instruction, and the JALR's target must be the cleared one.
//
//   pages      a taken branch forward across a 4 KiB page front and a taken
//              branch backward across it. The displacement is the interesting
//              number, and both signs are produced.
//
//   line       a JALR whose target is 0x80000FFE -- NOT 4-byte aligned, and the
//              four bytes of the instruction there cross both the harness's
//              eight-byte line at 0x80001000 and the 4 KiB page front. This is
//              the card's "fetch crossing a line/page front boundary" and its
//              "alignment fault on a jump target that is not 4-byte aligned".
//
//   fencei     a self-modifying program: a store patches the word immediately
//              after FENCE.I, FENCE.I executes, and the *fetch* must see the new
//              bytes. The front end fetched the stale word before the store took
//              effect, so a FENCE.I that does not invalidate the delivered view
//              executes the stale bytes and the retirement stream diverges.
//
//   wrongpath  a taken branch and a JAL whose skipped regions are not mapped at
//              all: every speculative fetch there is answered with an
//              instruction-access fault. The faults must leave no architectural
//              trace -- no event, no record, no trap, no stop.
//
//   fetchfault a JALR to an address the image cannot serve, on the *correct*
//              path. p0 has no instruction-access trap: the fault is delivered
//              as an undecodable macro and the machine stops at it with nothing
//              retired at the faulting address. This run states that, so the
//              report can say whether the fetch-fault path is a trap or a stop.
//
// ------------------------------------------------------------ the expectation
//
// Nothing here comes from the DUT. The images are this file's own (the encoders
// below, checked against `riscv64-elf-as -march=rv64im` / `objdump -d` at
// authoring time), and the architectural stream is computed by this file's own
// RV64IM interpreter, written from the ISA text. It reads the same bytes, models
// its own register file and its own memory, and treats FENCE / FENCE.I as
// ordering no-ops (it always reads current code, which is what "after FENCE.I the
// new code is observed" means). Per retired instruction the case compares:
//
//   1. `pc_before` -- the record's own `ev_pc` -- with the reference's PC;
//   2. `pc_after` -- the PC the instruction hands control to -- with the *next*
//      record's `pc_before`, which is the only place the p0 event port carries
//      it (V-008 rules F-3/F-4: the OoO retire port publishes `pc_before` and the
//      instruction's own length, and `ev_pc_after` has no RTL producer yet). The
//      reference states the PC after every instruction, so this is an ISA-derived
//      statement, not a re-reading of the DUT;
//   3. the instruction's own length (four bytes: every program here is RV64IM)
//      and its own bits, from the image;
//   4. the destination, the write enable and the value -- which is where the JAL
//      and JALR link values are checked, since the link is an ordinary register
//      write;
//   5. the trap bit: no run may produce a trap lane.
//
// plus the structural facts the reference cannot state:
//
//   6. every fetch request the core makes is an instruction's own start address;
//   7. the fetch issued in the cycle an answer is accepted is the instruction's
//      own length further on (the machine's own statement of the PC advance);
//   8. a wrong-path fetch leaves nothing: no record at any skipped address, and
//      the machine keeps running to its exit protocol;
//   9. a correct-path fetch fault stops the machine at the faulted instruction's
//      own start address, with no trap and nothing retired there.
//
// ------------------------------------------------------------- the controls
//
// tools/run_pc_controls.py rebuilds this case with exactly one defect injected,
// from a deleted build directory, and requires the mutant to exit 1 with a named
// check. Two are RTL defects (`-D`), one is a core defect, and one has to be a
// harness defect (`-CFLAGS -D`) because in p0 there is no cache and the *memory*
// is what assembles the four bytes at a PC:
//
//   (a) MOSAIC_BRANCH_TARGET_MUTANT_1        JALR forgets to clear bit 0;
//   (b) MOSAIC_CORE_MUTANT_FENCEI_NO_INVALIDATE  FENCE.I does not invalidate the
//       delivered fetch view (the core already has this mutant; this case reuses
//       it rather than inventing a second one);
//   (c) MOSAIC_CORE_MUTANT_NO_PURGE          a redirect does not purge the
//       undecodable macro the wrong-path fetch left in the decode buffer, so the
//       wrong-path fault leaks into the architectural stream;
//   (d) MOSAIC_IMEM_MUTANT_MASK_PC           the harness's instruction memory
//       serves the fetch of a PC that is not on a line front from the line front
//       -- the PC masked at a boundary -- so the machine executes a word that is
//       not the one at its own PC.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "sim_common.h"

namespace {

// The probe driver must reach --case before the schema can be checked.
constexpr int kResetCycles = 4;
// After the program's own story ends the pipeline is drained: enough cycles for
// the park loop to retire at least once, so `pc_after` has a next record.
constexpr int kDrainCycles = 64;
// A stall is "no retirement for this many cycles", which is more useful than a
// raw timeout: the failure names the machine's state.
constexpr int kStallCycles = 3000;
constexpr uint64_t kMaxRunCycles = 20000;

// ------------------------------------------------------------------ addresses
constexpr uint64_t kBase       = 0x80000000ull;   // the reset vector
constexpr uint64_t kTohost     = 0x80001000ull;   // MOSAIC_TOHOST
constexpr uint64_t kPageBytes  = 0x1000ull;       // a 4 KiB front
constexpr uint32_t kPassCode   = 1;               // MOSAIC_PASS_CODE
constexpr uint32_t kNop        = 0x00000013u;     // addi x0, x0, 0
// The harness's fetch "line". p0 has no cache and no MMU: the core asks for four
// bytes at the instruction's own PC and this file's memory model assembles them.
// The line is therefore *this harness's* model -- eight bytes, the same model
// I-041's cross-boundary case uses -- and it is the visible boundary at which a
// 32-bit instruction can straddle. See the report's boundary section.
constexpr uint64_t kLineBytes  = 8ull;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// Instruction encoding (RV64IM; every program here is 32-bit)
// ============================================================================
// Checked against `riscv64-elf-as -march=rv64im` / `objdump -d` when the
// programs below were laid out.
uint32_t EncR(uint32_t op, uint32_t f3, uint32_t f7, uint32_t rd, uint32_t rs1,
              uint32_t rs2) {
  return op | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}
uint32_t EncI(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm) {
  return op | (rd << 7) | (f3 << 12) | (rs1 << 15) |
         ((static_cast<uint32_t>(imm) & 0xFFFu) << 20);
}
uint32_t EncS(uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0xFFFu;
  return 0x23u | ((u & 0x1Fu) << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) |
         ((u >> 5) << 25);
}
uint32_t EncB(uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off) & 0x1FFFu;
  return 0x63u | (((u >> 11) & 1u) << 7) | (((u >> 1) & 0xFu) << 8) | (f3 << 12) |
         (rs1 << 15) | (rs2 << 20) | (((u >> 5) & 0x3Fu) << 25) |
         (((u >> 12) & 1u) << 31);
}
uint32_t EncU(uint32_t op, uint32_t rd, uint32_t imm20) {
  return op | (rd << 7) | ((imm20 & 0xFFFFFu) << 12);
}
uint32_t EncJ(uint32_t rd, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off) & 0x1FFFFFu;
  return 0x6Fu | (rd << 7) | (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
         (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12);
}
uint32_t Jalr(uint32_t rd, uint32_t rs1, int32_t imm) {
  return EncI(0x67u, 0x0u, rd, rs1, imm);
}

// Register names, for readability of the programs below.
enum {
  X0 = 0, RA = 1, T0 = 5, T1 = 6, T2 = 7, S0 = 8, S1 = 9, A0 = 10, A1 = 11,
  A2 = 12, A3 = 13, A4 = 14, A5 = 15, S2 = 18, S3 = 19, S4 = 20, S5 = 21,
  S6 = 22, S7 = 23, S8 = 24, S11 = 27, T3 = 28, T4 = 29, T5 = 30
};

// ============================================================================
// The instruction and data image: one byte-addressed memory
// ============================================================================
// Code and data are the *same* map, because the FENCE.I run's store patches an
// instruction and the next fetch must see it. The map is sparse: an address that
// was never written cannot be fetched, and that is what makes the wrong-path
// regions of the `wrongpath` run unservable.
class Img {
 public:
  void Emit32(uint64_t addr, uint32_t word) {
    for (unsigned i = 0; i < 4; i++) {
      Put(addr + i, static_cast<uint8_t>((word >> (8 * i)) & 0xFFu));
    }
    starts_.insert(addr);
  }

  bool Mapped(uint64_t addr) const { return bytes_.count(addr) != 0; }
  bool IsStart(uint64_t addr) const { return starts_.count(addr) != 0; }

  uint8_t Byte(uint64_t addr) const {
    const auto it = bytes_.find(addr);
    return (it == bytes_.end()) ? 0u : it->second;
  }
  uint16_t Half(uint64_t addr) const {
    return static_cast<uint16_t>(Byte(addr) | (Byte(addr + 1) << 8));
  }

  // The four bytes the instruction at `addr` needs, if the memory can assemble
  // them. As in I-041's model, the encoding decides the length: the first
  // halfword's low two bits are 11 for a 32-bit instruction (which then needs
  // its second halfword mapped, or the fetch cannot be served) and anything else
  // is a 16-bit instruction, where two bytes are enough. `mask_pc` is control
  // (d): the memory answers a PC that is not on a line front with the line front
  // -- the PC masked at a boundary.
  bool Fetch(uint64_t addr, bool mask_pc, uint32_t* bits, uint32_t* len) const {
    const uint64_t from = mask_pc ? (addr & ~(kLineBytes - 1u)) : addr;
    if (!Mapped(from) || !Mapped(from + 1)) return false;
    const uint16_t first = Half(from);
    if ((first & 3u) == 3u) {
      if (!Mapped(from + 2) || !Mapped(from + 3)) return false;
      *len = 4u;
      *bits = static_cast<uint32_t>(Half(from)) |
              (static_cast<uint32_t>(Half(from + 2)) << 16);
    } else {
      *len = 2u;
      *bits = static_cast<uint32_t>(first);
    }
    return true;
  }

  // The memory system's side of a store: `wstrb` bit i is byte (addr & ~7) + i.
  void Store(uint64_t addr, uint32_t wstrb, uint64_t wdata) {
    const uint64_t base = addr & ~7ull;
    for (unsigned i = 0; i < 8; i++) {
      if (((wstrb >> i) & 1u) == 0) continue;
      Put(base + i, static_cast<uint8_t>((wdata >> (8 * i)) & 0xFFu));
    }
  }

  void StoreWord(uint64_t addr, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
      Put(addr + i, static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }
  void StoreDouble(uint64_t addr, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) {
      Put(addr + i, static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }

  uint64_t Window(uint64_t addr) const {
    const uint64_t base = addr & ~7ull;
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) {
      v |= static_cast<uint64_t>(Byte(base + i)) << (8 * i);
    }
    return v;
  }

  // A block of NOPs, so a speculative fetch into an address the program never
  // executes is servable rather than a fault. The `line` run deliberately does
  // *not* fill the straddled address; that is the point of that run.
  void FillNops(uint64_t from, uint64_t to) {
    for (uint64_t a = from; a + 4 <= to; a += 4) Emit32(a, kNop);
  }

 private:
  void Put(uint64_t addr, uint8_t byte) { bytes_[addr] = byte; }

  std::map<uint64_t, uint8_t> bytes_;
  std::set<uint64_t> starts_;
};

// The exit protocol, at an arbitrary PC: materialise TOHOST PC-relatively, then
// store the pass code. `auipc` adds the upper 20 bits of (target - pc) and the
// `addi` the signed low 12 -- the only way to reach 0x8000_1000 on RV64 with two
// instructions. The park that follows is `jal x0, 0`.
void EmitExit(Img* img, uint64_t pc) {
  const int64_t delta = static_cast<int64_t>(kTohost) - static_cast<int64_t>(pc);
  const int64_t hi = (delta + 0x800) >> 12;
  const int64_t lo = delta - (hi << 12);
  img->Emit32(pc, EncU(0x17u, T5, static_cast<uint32_t>(hi) & 0xFFFFFu));
  img->Emit32(pc + 4, EncI(0x13u, 0x0u, T5, T5, static_cast<int32_t>(lo)));
  img->Emit32(pc + 8, EncI(0x13u, 0x0u, A0, X0, static_cast<int32_t>(kPassCode)));
  img->Emit32(pc + 12, EncS(0x3u, T5, A0, 0));
  img->Emit32(pc + 16, EncJ(X0, 0));   // the park
}

// ============================================================================
// The independent reference: an RV64IM interpreter for the programs above
// ============================================================================
// Written from the ISA text. It executes the image it is given (its own copy, so
// a store in the FENCE.I run patches the reference's code and not the DUT's) and
// stops where the program's own story ends: the exit store, or a PC it cannot
// fetch. FENCE and FENCE.I are ordering no-ops here, because the reference always
// reads current code -- which is exactly the behaviour FENCE.I must produce.
struct RefRec {
  uint64_t pc = 0;
  uint64_t next_pc = 0;
  uint32_t bits = 0;
  uint32_t len = 4;
  uint32_t rd = 0;
  bool we = false;
  uint64_t value = 0;
  bool is_jalr = false;
  uint64_t jalr_raw = 0;    // the target before bit 0 is cleared
  bool is_branch = false;
  bool taken = false;
};

struct RefTrace {
  std::vector<RefRec> recs;
  bool exited = false;
  uint64_t exit_pc = 0;
  bool stopped = false;
  uint64_t stop_pc = 0;
  std::string stop_reason;
};

uint64_t SignExtend(uint64_t value, unsigned bits) {
  const uint64_t sign = UINT64_C(1) << (bits - 1);
  return (value ^ sign) - sign;
}

RefTrace RunReference(Img img, uint64_t start) {
  RefTrace out;
  uint64_t regs[32] = {};
  uint64_t pc = start;
  for (int step = 0; step < 4096; step++) {
    uint32_t bits = 0;
    uint32_t len = 0;
    if (!img.Fetch(pc, false, &bits, &len)) {
      out.stopped = true;
      out.stop_pc = pc;
      out.stop_reason = "the instruction at this address cannot be fetched";
      return out;
    }
    const uint32_t op = bits & 0x7Fu;
    const uint32_t f3 = (bits >> 12) & 0x7u;
    const uint32_t rd = (bits >> 7) & 0x1Fu;
    const uint32_t rs1 = (bits >> 15) & 0x1Fu;
    const uint32_t rs2 = (bits >> 20) & 0x1Fu;
    const uint32_t f7 = (bits >> 25) & 0x7Fu;
    const int64_t imm_i = static_cast<int32_t>(bits) >> 20;
    const int64_t imm_s =
        (static_cast<int32_t>(bits) >> 25 << 5) |
        static_cast<int32_t>((bits >> 7) & 0x1Fu);
    const int64_t imm_b = static_cast<int32_t>(
        SignExtend(((bits >> 31) & 1u) << 12 | ((bits >> 7) & 1u) << 11 |
                       ((bits >> 25) & 0x3Fu) << 5 | ((bits >> 8) & 0xFu) << 1,
                   13));
    const int64_t imm_u = static_cast<int32_t>(bits & 0xFFFFF000u);
    const int64_t imm_j = static_cast<int32_t>(
        SignExtend(((bits >> 31) & 1u) << 20 | ((bits >> 12) & 0xFFu) << 12 |
                       ((bits >> 20) & 1u) << 11 | ((bits >> 21) & 0x3FFu) << 1,
                   21));

    RefRec r;
    r.pc = pc;
    r.bits = bits;
    r.len = len;
    r.next_pc = pc + len;
    uint64_t value = 0;
    bool we = false;

    if (op == 0x37u) {                       // LUI
      value = static_cast<uint64_t>(imm_u);
      we = true;
    } else if (op == 0x17u) {                // AUIPC
      value = pc + static_cast<uint64_t>(imm_u);
      we = true;
    } else if (op == 0x13u && (f3 == 0x0u || f3 == 0x6u || f3 == 0x7u)) {
      switch (f3) {                          // ADDI / ORI / ANDI
        case 0x0u: value = regs[rs1] + static_cast<uint64_t>(imm_i); break;
        case 0x6u: value = regs[rs1] | static_cast<uint64_t>(imm_i); break;
        default:   value = regs[rs1] & static_cast<uint64_t>(imm_i); break;
      }
      we = true;
    } else if (op == 0x33u && f3 == 0x0u && (f7 == 0x00u || f7 == 0x20u)) {
      value = (f7 == 0x00u) ? (regs[rs1] + regs[rs2]) : (regs[rs1] - regs[rs2]);
      we = true;
    } else if (op == 0x33u && f3 == 0x4u && f7 == 0x01u) {   // DIV
      const int64_t a = static_cast<int64_t>(regs[rs1]);
      const int64_t b = static_cast<int64_t>(regs[rs2]);
      int64_t q = 0;
      if (b != 0) {
        q = (a == INT64_MIN && b == -1) ? INT64_MIN : (a / b);
      }
      value = static_cast<uint64_t>(q);
      we = true;
    } else if (op == 0x23u && f3 == 0x3u) {  // SD
      const uint64_t addr =
          regs[rs1] + static_cast<uint64_t>(imm_s);
      if (addr == kTohost) {
        out.exited = true;
        out.exit_pc = pc;
        r.next_pc = pc + 4;
        out.recs.push_back(r);
        return out;
      }
      img.StoreDouble(addr, regs[rs2]);
      r.next_pc = pc + 4;
    } else if (op == 0x23u && f3 == 0x2u) {  // SW
      const uint64_t addr =
          regs[rs1] + static_cast<uint64_t>(imm_s);
      if (addr == kTohost) {
        out.exited = true;
        out.exit_pc = pc;
        r.next_pc = pc + 4;
        out.recs.push_back(r);
        return out;
      }
      img.StoreWord(addr, static_cast<uint32_t>(regs[rs2]));
      r.next_pc = pc + 4;
    } else if (op == 0x6Fu) {                // JAL
      r.next_pc = pc + static_cast<uint64_t>(imm_j);
      if (rd != 0) { value = pc + len; we = true; }
    } else if (op == 0x67u && f3 == 0x0u) {  // JALR
      r.is_jalr = true;
      r.jalr_raw = regs[rs1] + static_cast<uint64_t>(imm_i);
      r.next_pc = r.jalr_raw & ~UINT64_C(1);
      if (rd != 0) { value = pc + len; we = true; }
    } else if (op == 0x63u) {                // the conditional branches
      r.is_branch = true;
      bool take = false;
      switch (f3) {
        case 0x0u: take = (regs[rs1] == regs[rs2]); break;
        case 0x1u: take = (regs[rs1] != regs[rs2]); break;
        case 0x4u: take = (static_cast<int64_t>(regs[rs1]) <
                           static_cast<int64_t>(regs[rs2])); break;
        case 0x5u: take = (static_cast<int64_t>(regs[rs1]) >=
                           static_cast<int64_t>(regs[rs2])); break;
        default:
          Fail("reference", "a branch form this reference does not implement at pc " +
                                U64(pc));
      }
      r.taken = take;
      r.next_pc = take ? (pc + static_cast<uint64_t>(imm_b)) : (pc + len);
    } else if (op == 0x0Fu) {                // FENCE / FENCE.I: ordering no-ops
      r.next_pc = pc + 4;
    } else {
      Fail("reference", "an opcode this reference does not implement at pc " +
                            U64(pc) + ": " + U64(bits));
    }

    r.rd = we && rd != 0 ? rd : 0;
    r.we = we && rd != 0;
    r.value = value;
    if (r.we) regs[rd] = value;
    out.recs.push_back(r);
    pc = r.next_pc;
  }
  Fail("reference", "the reference ran away: it never reached a stop");
  return out;
}

// ============================================================================
// The run: one retired lane, one delivered fetch, one PC-advance pair
// ============================================================================
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

struct Delivery {
  uint64_t pc = 0;
  bool is_insn = false;
};

struct Advance {
  uint64_t pc = 0;
  uint64_t req = 0;
};

struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
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
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, Img* img, bool mask_pc, uint64_t max_cycles)
      : dut_(dut), img_(img), mask_pc_(mask_pc), max_cycles_(max_cycles) {}

  const std::vector<DutRecord>& records() const { return records_; }
  const std::vector<Delivery>& deliveries() const { return deliveries_; }
  const std::vector<Advance>& advances() const { return advances_; }
  const std::vector<uint64_t>& requests() const { return requests_; }
  uint64_t cycles() const { return cycles_; }
  bool exited() const { return exited_; }
  uint64_t exit_code() const { return exit_code_; }
  bool stopped() const { return dut_->o_stopped_o != 0; }

  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) +
           " stopped=" + Dec(dut_->o_stopped_o) +
           " illegal=" + Dec(dut_->o_illegal_o) +
           " unsupported=" + Dec(dut_->o_unsupported_o);
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
    for (unsigned i = 0; i < 4; i++) Cycle(false);   // let the first fetch start
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail("run", "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + State());
    }
    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = 1;

    // ------------------------------------------------------------ imem
    dut_->imem_rsp_valid_i = 0;
    dut_->imem_rsp_rdata_i = 0;
    dut_->imem_rsp_fault_i = 0;
    dut_->imem_rsp_id_i = 0;
    dut_->imem_rsp_epoch_i = 0;
    dut_->imem_rsp_len_i = 0;
    if (imem_.HasResponse() && !rst) {
      const Imem::Response& rsp = imem_.Current();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = rsp.data;
      dut_->imem_rsp_fault_i = rsp.fault ? 1 : 0;
      dut_->imem_rsp_id_i = rsp.id;
      dut_->imem_rsp_epoch_i = rsp.epoch;
      dut_->imem_rsp_len_i = 4;   // the memory serves a word; the DUT reads the
                                  // instruction's own length from its encoding
    }

    // ------------------------------------------------------------ dmem
    dut_->dmem_req_ready_i = 1;
    if (dmem_.HasResponse()) {
      const Dmem::Rsp& r = dmem_.CurrentResponse();
      dut_->dmem_rsp_valid_i = 1;
      dut_->dmem_rsp_rdata_i = r.rdata;
      dut_->dmem_rsp_fault_i = r.fault ? 1 : 0;
    } else {
      dut_->dmem_rsp_valid_i = 0;
      dut_->dmem_rsp_rdata_i = 0;
      dut_->dmem_rsp_fault_i = 0;
    }

    // No interrupts, no second agent, and the standalone redirect arbiter is not
    // the subject of this case: drive every input so nothing is left floating.
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = 0;
    dut_->ext_write_valid_i = 0;
    dut_->ext_write_addr_i = 0;
    dut_->ext_write_bytes_i = 0;
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_req_pc0_i = 0;
    dut_->arb_req_pc1_i = 0;
    dut_->arb_req_idx0_i = 0;
    dut_->arb_req_idx1_i = 0;
    dut_->arb_req_gen0_i = 0;
    dut_->arb_req_gen1_i = 0;
    dut_->arb_req_taken0_i = 0;
    dut_->arb_req_taken1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_index_i = 0;
    dut_->arb_head_gen_i = 0;
    dut_->arb_head_occupied_i = 0;
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
      Dmem::Txn t;
      t.we = dut_->dmem_req_we_o != 0;
      t.addr = dut_->dmem_req_addr_o;
      t.size = dut_->dmem_req_size_o;
      t.wstrb = dut_->dmem_req_wstrb_o;
      t.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(t);
      if (t.we && (t.addr & ~7ull) == (kTohost & ~7ull) && !exited_) {
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; i++) {
          if ((t.wstrb >> i) & 1u) {
            value |= ((t.wdata >> (8 * i)) & 0xFFu) << (8 * i);
          }
        }
        exited_ = true;
        exit_code_ = value;
      }
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

    Imem(Img* img, bool mask_pc) : img_(img), mask_pc_(mask_pc) {}

    bool HasResponse() const { return !ready_.empty(); }
    const Response& Current() const { return ready_.front(); }

    void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
      Response rsp;
      rsp.id = id;
      rsp.epoch = epoch;
      uint32_t bits = 0;
      uint32_t len = 0;
      if (!img_->Fetch(addr, mask_pc_, &bits, &len)) {
        rsp.fault = true;
        rsp.data = img_->Mapped(addr) ? static_cast<uint32_t>(img_->Half(addr))
                                      : 0x00000073u;   // ECALL for a hole
        faulted_.push_back(addr);
      } else {
        rsp.data = bits;
      }
      inflight_.push_back(Entry{rsp, 1});
    }

    const std::vector<uint64_t>& faulted() const { return faulted_; }
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
    Img* img_;
    bool mask_pc_;
    std::vector<Entry> inflight_;
    std::deque<Response> ready_;
    std::vector<uint64_t> faulted_;
  };

  // ---------------------------------------------------- the data port
  class Dmem {
   public:
    struct Rsp {
      uint64_t rdata = 0;
      bool fault = false;
    };
    struct Txn {
      bool we = false;
      uint64_t addr = 0;
      uint32_t size = 0;
      uint32_t wstrb = 0;
      uint64_t wdata = 0;
    };

    explicit Dmem(Img* img) : img_(img) {}

    void Reset() { inflight_.clear(); ready_.clear(); }
    bool HasResponse() const { return !ready_.empty(); }
    const Rsp& CurrentResponse() const { return ready_.front(); }

    void Accept(const Txn& t) {
      Rsp rsp;
      if (t.we) {
        if ((t.addr & ~7ull) != (kTohost & ~7ull)) {
          img_->Store(t.addr, t.wstrb, t.wdata);
        }
      } else {
        rsp.rdata = img_->Window(t.addr);
      }
      inflight_.push_back(Entry{rsp, 1});
    }
    void PopResponse() { ready_.pop_front(); }
    void Advance() {
      for (size_t i = 0; i < inflight_.size();) {
        if (--inflight_[i].left <= 0) {
          ready_.push_back(inflight_[i].rsp);
          inflight_.erase(inflight_.begin() + static_cast<long>(i));
        } else {
          ++i;
        }
      }
    }

   private:
    struct Entry {
      Rsp rsp;
      int left = 0;
    };
    Img* img_;
    std::vector<Entry> inflight_;
    std::deque<Rsp> ready_;
  };

  void Observe() {
    const uint32_t mask =
        (retire_width_ >= 32) ? 0xFFFFFFFFu : ((1u << retire_width_) - 1u);
    const uint32_t got = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < retire_width_ && lane < 32; lane++) {
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
      if (std::getenv("MOSAIC_PC_TRACE") != nullptr) {
        std::printf("  [retire] cycle=%llu pc=%s len=%u insn=%s rd=%u we=%u val=%s\n",
                    static_cast<unsigned long long>(cycles_), U64(r.pc).c_str(),
                    r.len, U64(r.insn).c_str(), r.rd, r.we ? 1u : 0u,
                    U64(r.value).c_str());
      }
      records_.push_back(r);
    }

    // The fetch unit's output register, as the debug bundle publishes it: word 0
    // bit 0 is "the register holds a delivery", word 2 is that delivery's PC, and
    // `o_dbg_deliver_valid_o` says whether it was a *deliverable instruction* (a
    // refused/dropped delivery has the same PC and the bit low, which is how the
    // faulting address stays observable).
    const uint32_t f0 = dut_->o_dbg_fetch_o[0];
    const uint64_t pc = static_cast<uint64_t>(dut_->o_dbg_fetch_o[2]);
    const bool reg_valid = (f0 & 1u) != 0;
    const bool is_insn = dut_->o_dbg_deliver_valid_o != 0;
    if (!reg_valid) {
      obs_valid_ = false;
    } else if (!obs_valid_ || pc != obs_pc_ || is_insn != obs_is_insn_) {
      obs_valid_ = true;
      obs_pc_ = pc;
      obs_is_insn_ = is_insn;
      deliveries_.push_back(Delivery{pc, is_insn});
      if (req_prev_cycle_ != 0) {
        advances_.push_back(Advance{pc, req_prev_cycle_});
      }
    }
    req_prev_cycle_ = (!dut_->imem_req_valid_o || !dut_->imem_req_ready_i)
                          ? 0
                          : dut_->imem_req_addr_o;
  }

  Vmosaic_core_tb* dut_;
  Img* img_;
  bool mask_pc_;
  uint64_t max_cycles_;
  Imem imem_{img_, mask_pc_};
  Dmem dmem_{img_};
  uint32_t retire_width_ = 2;
  uint64_t cycles_ = 0;
  std::vector<DutRecord> records_;
  std::vector<Delivery> deliveries_;
  std::vector<Advance> advances_;
  std::vector<uint64_t> requests_;
  bool obs_valid_ = false;
  uint64_t obs_pc_ = 0;
  bool obs_is_insn_ = false;
  uint64_t req_prev_cycle_ = 0;
  bool exited_ = false;
  uint64_t exit_code_ = 0;

 public:
  void SetGeometry(const Geometry& g) { retire_width_ = g.retire_width; }
  const std::vector<uint64_t>& imem_faults() const { return imem_.faulted(); }
};

// ============================================================================
// Running one program and comparing it with the reference
// ============================================================================
struct RunOut {
  std::vector<DutRecord> records;
  std::vector<Delivery> deliveries;
  std::vector<Advance> advances;
  std::vector<uint64_t> requests;
  std::vector<uint64_t> faults;
  uint64_t cycles = 0;
  bool exited = false;
  uint64_t exit_code = 0;
  bool stopped = false;
  uint64_t illegal = 0;
  uint64_t unsupported = 0;
  uint64_t traps = 0;
};

RunOut RunOnce(Vmosaic_core_tb* dut, const Geometry& geometry, Img* img,
               size_t want, bool expect_stop, bool mask_pc,
               const std::string& label) {
  Harness harness(dut, img, mask_pc, kMaxRunCycles);
  harness.SetGeometry(geometry);
  harness.Reset(kResetCycles);
  const size_t cap = want + 16;
  uint64_t last_retired = 0;
  uint64_t last_progress = 0;
  while (true) {
    if (harness.stopped()) {
      for (int i = 0; i < kDrainCycles; i++) harness.Cycle(false);
      break;
    }
    if (harness.exited()) {
      for (int i = 0; i < kDrainCycles; i++) harness.Cycle(false);
      break;
    }
    if (harness.records().size() >= cap) break;
    harness.Cycle(false);
    const uint64_t retired = harness.records().size();
    if (retired != last_retired) {
      last_retired = retired;
      last_progress = harness.cycles();
    } else if (harness.cycles() - last_progress > kStallCycles) {
      Fail(label, "stalled at " + Dec(retired) + " of " + Dec(want) +
                      " instructions: " + harness.State());
    }
  }
  if (!expect_stop && !harness.exited() && !harness.stopped()) {
    Fail(label, "the run did not reach its exit protocol in " +
                    Dec(harness.cycles()) + " cycles: " + harness.State());
  }
  RunOut out;
  out.records = harness.records();
  out.deliveries = harness.deliveries();
  out.advances = harness.advances();
  out.requests = harness.requests();
  out.faults = harness.imem_faults();
  out.cycles = harness.cycles();
  out.exited = harness.exited();
  out.exit_code = harness.exit_code();
  out.stopped = harness.stopped();
  out.illegal = dut->o_illegal_o;
  out.unsupported = dut->o_unsupported_o;
  out.traps = dut->o_csr_trap_o;
  return out;
}

// The per-instruction comparison. One check per fact, every fact checked rather
// than only the first, so each control is caught by the check that describes the
// defect it injects.
void CompareStream(mosaic::Reporter* reporter, const std::string& label,
                   const RunOut& got, const RefTrace& ref) {
  const size_t n = std::min(got.records.size(), ref.recs.size());
  int mismatches = 0;
  for (size_t i = 0; i < n; i++) {
    const DutRecord& r = got.records[i];
    const RefRec& e = ref.recs[i];

    // 1. pc_before.
    if (r.pc != e.pc) {
      reporter->Check(false, label + ": the retirement stream follows the "
                                      "reference: retire " + Dec(i) +
                              " pc_before expected " + U64(e.pc) + ", got " +
                              U64(r.pc));
      mismatches++;
      break;   // the stream is not aligned any more; the count check reports the rest
    }
    // 5. no trap lane.
    if (r.trap) {
      reporter->Check(false, label + ": no instruction produces a trap lane: retire " +
                                      Dec(i) + " at " + U64(r.pc) + " traps with cause " +
                              Dec(r.cause));
      mismatches++;
      continue;
    }
    // 3. the instruction's own length and bits.
    if (r.len != e.len) {
      reporter->Check(false, label + ": the event record reports the instruction's "
                                      "own length: retire " + Dec(i) + " at " +
                              U64(e.pc) + " expected " + Dec(e.len) + ", got " +
                              Dec(r.len));
      mismatches++;
    }
    if (r.insn != e.bits) {
      reporter->Check(false, label + ": the event record carries the instruction's "
                                      "own bits: retire " + Dec(i) + " at " +
                              U64(e.pc) + " expected " + U64(e.bits) + ", got " +
                              U64(r.insn));
      mismatches++;
    }
    // 4. the destination and value -- where the JAL/JALR link is checked.
    if (r.rd != e.rd || r.we != e.we) {
      reporter->Check(false, label + ": the retirement stream follows the "
                                      "reference: retire " + Dec(i) + " at " +
                              U64(e.pc) + " destination expected rd=" + Dec(e.rd) +
                              " we=" + Dec(e.we) + ", got rd=" + Dec(r.rd) +
                              " we=" + Dec(r.we));
      mismatches++;
    } else if (e.we && r.value != e.value) {
      reporter->Check(false, label + ": the retirement stream follows the "
                                      "reference: retire " + Dec(i) + " at " +
                              U64(e.pc) + " value for x" + Dec(e.rd) +
                              " expected " + U64(e.value) + ", got " + U64(r.value));
      mismatches++;
    }
    // 2. pc_after: the PC the instruction hands control to is the next record's
    // pc_before, and the reference states what it must be.
    if (i + 1 < got.records.size() && got.records[i + 1].pc != e.next_pc) {
      reporter->Check(false, label + ": pc_after is the next record's pc_before: "
                                      "after " + U64(e.pc) + " the reference "
                                      "continues at " + U64(e.next_pc) + ", the "
                                      "next record retires at " +
                                      U64(got.records[i + 1].pc));
      mismatches++;
    }
    // The link value, stated directly: pc + the instruction's own length. Only
    // for a jump that writes a link (`jal x0` writes nothing).
    if (((e.bits & 0x7Fu) == 0x6Fu || e.is_jalr) && e.we) {
      if (r.rd != e.rd || !r.we || r.value != e.pc + e.len) {
        reporter->Check(false, label + ": a jump-and-link writes pc + the "
                                        "instruction's own length: at " +
                                U64(e.pc) + " the link is " + U64(e.pc + e.len) +
                                ", the record wrote " + U64(r.value));
        mismatches++;
      }
    }
    // The JALR target: bit 0 cleared, and *nothing else* changed.
    if (e.is_jalr) {
      if (i + 1 < got.records.size() &&
          got.records[i + 1].pc != (e.jalr_raw & ~UINT64_C(1))) {
        reporter->Check(false, label + ": the JALR at " + U64(e.pc) +
                                        " clears bit 0 of its computed target and "
                                        "nothing else: the computed target is " +
                                U64(e.jalr_raw) + ", the instruction after it "
                                "retires at " + U64(got.records[i + 1].pc) +
                                ", expected " + U64(e.jalr_raw & ~UINT64_C(1)));
        mismatches++;
      }
    }
  }
  reporter->Check(got.records.size() >= ref.recs.size(),
                  label + ": every instruction the reference produces is retired (" +
                      Dec(got.records.size()) + " recorded, " + Dec(ref.recs.size()) +
                      " expected)");
  reporter->Check(mismatches == 0,
                  label + ": the retirement stream equals the independent RV64IM "
                          "reference, pc_before and pc_after instruction by "
                          "instruction (" + Dec(n) + " lanes compared)");
}

// The structural facts the reference cannot state.
void CheckStructure(mosaic::Reporter* reporter, const std::string& label,
                    const RunOut& got, const Img& img, const RefTrace& ref,
                    bool expect_refusal = false) {
  // Every record's length agrees with the encoding it carries: this case's
  // programs are RV64IM, so every retired instruction must be four bytes and its
  // low two bits 11. A machine that privately executed a compressed instruction
  // would report length two here.
  int bad_len = 0;
  for (const DutRecord& r : got.records) {
    const uint32_t want_len = ((r.insn & 3u) == 3u) ? 4u : 2u;
    if (r.len != want_len || r.len != 4u) {
      bad_len++;
      if (bad_len <= 4) {
        reporter->Check(false, label + ": every retired instruction is a four-byte "
                                        "RV64IM instruction: at " + U64(r.pc) +
                                " the record says length " + Dec(r.len) + " with "
                                "encoding " + U64(r.insn));
      }
    }
  }
  reporter->Check(bad_len == 0,
                  label + ": every event record's length is four bytes and agrees "
                          "with its own encoding (" + Dec(got.records.size()) +
                      " lanes checked)");

  // Every fetch request is an instruction's own start address -- or an address
  // the image cannot serve at all, which is how a wrong-path fetch and a runaway
  // both end. An address inside an instruction is a PC advanced by an amount that
  // is not the instruction's length.
  int bad_req = 0;
  for (uint64_t addr : got.requests) {
    if (img.IsStart(addr)) continue;
    uint32_t bits = 0;
    uint32_t len = 0;
    if (!img.Fetch(addr, false, &bits, &len)) continue;   // not servable: allowed
    bad_req++;
    if (bad_req <= 3) {
      reporter->Check(false, label + ": every instruction request is for an "
                                      "instruction's own start address: " +
                              U64(addr) + " is inside an instruction");
    }
  }
  reporter->Check(bad_req == 0,
                  label + ": every address the core requests is an instruction's "
                          "own start address (" + Dec(got.requests.size()) +
                      " requests checked)");

  // The machine's own statement of the PC advance: the fetch issued in the cycle
  // an answer was accepted is the instruction's own length further on. Checked
  // only where the reference itself says the instruction is one it executes.
  std::set<uint64_t> starts;
  for (const RefRec& r : ref.recs) starts.insert(r.pc);
  int bad_advance = 0;
  for (const Advance& a : got.advances) {
    if (starts.count(a.pc) == 0) continue;
    if (a.req == a.pc + 4u) continue;
    bad_advance++;
    if (bad_advance <= 3) {
      reporter->Check(false, label + ": the program counter advances by the "
                                      "delivered instruction's own length: the "
                                      "four-byte instruction at " + U64(a.pc) +
                              " was answered by a fetch to " + U64(a.req));
    }
  }
  reporter->Check(bad_advance == 0,
                  label + ": every fetch after an instruction is that instruction's "
                          "own length further on (" + Dec(got.advances.size()) +
                      " instruction/fetch pairs checked)");

  // Nothing the machine did may be refused as undecodable or unsupported, and no
  // trap may be taken, while a program is running its own story. The one
  // exception is the correct-path fetch fault: that *is* refused, and it is what
  // stops the machine, so that run asserts the refusal instead.
  if (expect_refusal) {
    reporter->Check(got.illegal >= 1 && got.unsupported >= 1,
                    label + ": the fetch fault reaches dispatch as an undecodable "
                            "macro and stops the machine: illegal=" +
                        Dec(got.illegal) + " unsupported=" + Dec(got.unsupported));
  } else {
    reporter->Check(got.illegal == 0 && got.unsupported == 0,
                    label + ": no macro is refused while the program runs: illegal=" +
                        Dec(got.illegal) + " unsupported=" + Dec(got.unsupported));
  }
  reporter->Check(got.traps == 0,
                  label + ": the machine takes no trap: o_csr_trap=" + Dec(got.traps));
}

// ============================================================================
// The programs
// ============================================================================
struct Sites {
  uint64_t jalr_raw = 0;          // a JALR's computed target, bit 0 set
  uint64_t jalr_cleared = 0;      // what the JALR must actually reach
  uint64_t unaligned_pc = 0;      // the JALR whose target is 2 mod 4
  uint64_t unaligned_target = 0;
  uint64_t fix_pc = 0;            // the word after FENCE.I
  uint64_t fence_i_pc = 0;
  uint64_t fault_pc = 0;          // the correct-path unfetchable target
  uint64_t pos_cross_pc = 0;      // a branch whose target crosses the page front
  uint64_t pos_cross_target = 0;
  uint64_t neg_cross_pc = 0;
  uint64_t neg_cross_target = 0;
};

// ---------------------------------------------------------------- transfers
void BuildTransfers(Img* img, Sites* sites) {
  img->FillNops(kBase, kBase + 0x100);
  img->Emit32(kBase + 0x00, EncI(0x13u, 0x0u, S0, X0, 1));        // addi s0, x0, 1
  img->Emit32(kBase + 0x04, EncJ(RA, 0x0C));                      // jal ra, +12
  img->Emit32(kBase + 0x08, EncI(0x13u, 0x0u, S1, X0, 0x111));    // skipped
  img->Emit32(kBase + 0x0C, EncI(0x13u, 0x0u, S2, X0, 0x222));    // skipped
  img->Emit32(kBase + 0x10, EncI(0x13u, 0x0u, S3, RA, 0));        // s3 = ra
  img->Emit32(kBase + 0x14, EncB(0x1u, X0, X0, 0x08));            // bne x0,x0 (not taken)
  img->Emit32(kBase + 0x18, EncB(0x4u, X0, X0, 0x08));            // blt x0,x0 (not taken)
  img->Emit32(kBase + 0x1C, EncB(0x0u, X0, X0, 0x08));            // beq x0,x0 (taken)
  img->Emit32(kBase + 0x20, EncI(0x13u, 0x0u, S4, X0, 0x333));    // skipped
  img->Emit32(kBase + 0x24, EncU(0x17u, A1, 0));                  // auipc a1, 0
  img->Emit32(kBase + 0x28, EncI(0x13u, 0x0u, A1, A1, 0x18));     // a1 += 0x18
  img->Emit32(kBase + 0x2C, EncI(0x13u, 0x6u, A1, A1, 1));        // ori a1, a1, 1
  sites->jalr_raw = kBase + 0x3D;
  sites->jalr_cleared = kBase + 0x3C;
  sites->unaligned_pc = kBase + 0x30;
  img->Emit32(kBase + 0x30, Jalr(2u, A1, 0));                     // jalr x2, a1, 0
  img->Emit32(kBase + 0x34, EncI(0x13u, 0x0u, S5, X0, 0x555));    // skipped
  img->Emit32(kBase + 0x38, EncI(0x13u, 0x0u, S6, X0, 0x666));    // skipped
  img->Emit32(kBase + 0x3C, EncI(0x13u, 0x0u, S7, 2u, 0));        // s7 = x2 (the link)
  EmitExit(img, kBase + 0x40);
}

// -------------------------------------------------------------------- pages
// The B-type immediate is 13 bits with bit 0 implied zero, so the positive
// displacement that reaches 0x1004 is +0xFFC from a branch at 0x008; +0x1000 is
// one step out of range and would be a different program.
void BuildPages(Img* img, Sites* sites) {
  img->FillNops(kBase, kBase + 0x1100);
  img->Emit32(kBase + 0x00, EncI(0x13u, 0x0u, S0, X0, 1));        // addi s0, x0, 1
  img->Emit32(kBase + 0x04, EncI(0x13u, 0x0u, S0, S0, 1));        // addi s0, s0, 1
  img->Emit32(kBase + 0x08, EncB(0x0u, X0, X0, 0xFFC));           // beq x0,x0 -> 0x1004
  img->Emit32(kBase + 0x1004, EncI(0x13u, 0x0u, S1, X0, 0x444));  // landed: positive cross
  img->Emit32(kBase + 0x1008, EncB(0x0u, X0, X0, -0x38));         // beq x0,x0 -> 0x0FD0
  img->Emit32(kBase + 0x0FD0, EncI(0x13u, 0x0u, S2, X0, 0x777));  // landed: negative cross
  EmitExit(img, kBase + 0x0FD4);
  sites->pos_cross_pc = kBase + 0x08;
  sites->pos_cross_target = kBase + 0x1004;
  sites->neg_cross_pc = kBase + 0x1008;
  sites->neg_cross_target = kBase + 0x0FD0;
}

// --------------------------------------------------------------------- line
void BuildLine(Img* img, Sites* sites) {
  img->FillNops(kBase, kBase + 0x0060);
  img->Emit32(kBase + 0x00, EncI(0x13u, 0x0u, S0, X0, 1));        // addi s0, x0, 1
  img->Emit32(kBase + 0x04, EncU(0x17u, A1, 0));                  // auipc a1, 0
  img->Emit32(kBase + 0x08, EncI(0x13u, 0x0u, A1, A1, 0x7FF));    // a1 += 0x7FF
  img->Emit32(kBase + 0x0C, EncI(0x13u, 0x0u, A1, A1, 0x7FB));    // a1 += 0x7FB -> 0x0FFE
  sites->unaligned_pc = kBase + 0x10;
  sites->unaligned_target = kBase + 0x0FFE;
  img->Emit32(kBase + 0x10, Jalr(2u, A1, 0));                     // jalr x2, a1, 0
  img->Emit32(kBase + 0x14, EncI(0x13u, 0x0u, S1, X0, 0x111));    // skipped
  img->Emit32(kBase + 0x18, EncI(0x13u, 0x0u, S2, X0, 0x222));    // skipped
  img->Emit32(kBase + 0x1C, EncI(0x13u, 0x0u, S3, 2u, 0));        // s3 = x2 (the link)
  img->Emit32(kBase + 0x20, EncJ(X0, 0x2C));                      // jal x0 -> 0x4C
  EmitExit(img, kBase + 0x4C);
  // The line-front word: only control (d) ever reads it, and it is what
  // "the PC masked to the line front" would execute in place of the word the
  // program put at 0x0FFE.
  img->Emit32(kBase + 0x0FF8, EncI(0x13u, 0x0u, S11, X0, 0xBAD));
  // The straddling instruction: PC 0x80000FFE (2 mod 4), four bytes crossing the
  // eight-byte line at 0x80001000 and the 4 KiB page front there.
  img->Emit32(kBase + 0x0FFE, EncI(0x13u, 0x0u, A4, X0, 0x777));
  img->Emit32(kBase + 0x1002, EncI(0x13u, 0x0u, A5, A4, 1));
  img->Emit32(kBase + 0x1006, EncJ(X0, -0xFBA));                  // jal x0 -> 0x4C
}

// ------------------------------------------------------------------- fencei
void BuildFenceI(Img* img, Sites* sites) {
  img->FillNops(kBase, kBase + 0x0080);
  img->Emit32(kBase + 0x00, EncU(0x37u, T1, 0x02200u));           // lui t1, 0x02200
  img->Emit32(kBase + 0x04, EncI(0x13u, 0x0u, T1, T1, 0x513));    // t1 = 0x02200513
  img->Emit32(kBase + 0x08, EncI(0x13u, 0x0u, T3, X0, 7));        // addi t3, x0, 7
  img->Emit32(kBase + 0x0C, EncR(0x33u, 0x4u, 0x01u, S0, T1, T3));  // div s0, t1, t3
  img->Emit32(kBase + 0x10, EncU(0x17u, T0, 0));                  // auipc t0, 0
  img->Emit32(kBase + 0x14, EncI(0x13u, 0x0u, T0, T0, 0x14));     // t0 = 0x24
  img->Emit32(kBase + 0x18, EncS(0x2u, T0, T1, 0));               // sw t1, 0(t0)
  img->Emit32(kBase + 0x1C, EncI(0x13u, 0x0u, S2, X0, 0x111));    // independent
  sites->fence_i_pc = kBase + 0x20;
  img->Emit32(kBase + 0x20, 0x0000100Fu);                         // fence.i
  sites->fix_pc = kBase + 0x24;
  img->Emit32(kBase + 0x24, EncI(0x13u, 0x0u, T2, X0, 0x11));     // addi t2, x0, 0x11 (old)
  img->Emit32(kBase + 0x28, EncI(0x13u, 0x0u, S3, T2, 0));        // s3 = t2
  EmitExit(img, kBase + 0x2C);
}

// ---------------------------------------------------------------- wrongpath
void BuildWrongPath(Img* img, Sites* sites) {
  img->Emit32(kBase + 0x00, EncI(0x13u, 0x0u, S0, X0, 1));        // addi s0, x0, 1
  img->Emit32(kBase + 0x04, EncB(0x0u, X0, X0, 0x100));           // beq x0,x0 -> 0x104
  // 0x008 .. 0x103 deliberately not mapped: every speculative fetch is a fault.
  img->Emit32(kBase + 0x104, EncI(0x13u, 0x0u, S1, X0, 0x444));   // landed
  img->Emit32(kBase + 0x108, EncJ(3u, 0x100));                    // jal x3 -> 0x208
  // 0x10C .. 0x207 deliberately not mapped.
  img->Emit32(kBase + 0x208, EncI(0x13u, 0x0u, S2, X0, 0x777));   // landed
  EmitExit(img, kBase + 0x20C);
}

// --------------------------------------------------------------- fetchfault
void BuildFetchFault(Img* img, Sites* sites) {
  img->Emit32(kBase + 0x00, EncI(0x13u, 0x0u, S0, X0, 1));        // addi s0, x0, 1
  img->Emit32(kBase + 0x04, EncU(0x17u, A1, 0));                  // auipc a1, 0
  img->Emit32(kBase + 0x08, EncI(0x13u, 0x0u, A1, A1, 0xFC));     // a1 = 0x100
  img->Emit32(kBase + 0x0C, Jalr(2u, A1, 0));                     // jalr -> 0x100
  img->Emit32(kBase + 0x10, EncI(0x13u, 0x0u, S1, X0, 0x111));    // never retires
  sites->fault_pc = kBase + 0x100;
}

// ============================================================================
// A small coverage ledger, so the case asserts its own coverage
// ============================================================================
struct Coverage {
  int jalr_bit0_set = 0;        // a JALR whose computed target had bit 0 set
  int jalr_2mod4 = 0;           // a JALR whose cleared target is 2 mod 4
  int pos_page_cross = 0;       // a taken transfer upward across a page front
  int neg_page_cross = 0;       // a taken transfer downward across a page front
  int line_straddle = 0;        // a retired instruction whose bytes cross the line
  int page_straddle = 0;        // a retired instruction whose bytes cross the page front
  int wrongpath_faults = 0;     // instruction-fetch faults answered off the path
};

void AccountCoverage(const RefTrace& ref, Coverage* cov) {
  for (size_t i = 0; i < ref.recs.size(); i++) {
    const RefRec& r = ref.recs[i];
    if ((r.pc % kLineBytes) + r.len > kLineBytes) cov->line_straddle++;
    if ((r.pc % kPageBytes) + r.len > kPageBytes) cov->page_straddle++;
    if (r.is_jalr) {
      if ((r.jalr_raw & 1u) != 0) cov->jalr_bit0_set++;
      if ((r.next_pc & 3u) != 0) cov->jalr_2mod4++;
    }
    const bool transfer = r.is_branch || (r.bits & 0x7Fu) == 0x6Fu || r.is_jalr;
    if (transfer && r.next_pc != r.pc + r.len) {
      if (r.next_pc / kPageBytes > r.pc / kPageBytes) cov->pos_page_cross++;
      if (r.next_pc / kPageBytes < r.pc / kPageBytes) cov->neg_page_cross++;
    }
  }
}

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

#ifdef MOSAIC_IMEM_MUTANT_MASK_PC
  const bool mask_pc = true;
#else
  const bool mask_pc = false;
#endif

  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    if (geometry.reset_vector != kBase) {
      Fail("geometry", "the reset vector is not the image base");
    }

    Coverage cov;
    uint64_t total_records = 0;
    uint64_t total_cycles = 0;
    int runs = 0;

    // ------------------------------------------------------------ transfers
    {
      Img img;
      Sites sites;
      BuildTransfers(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = "transfers";
      reporter.Check(ref.exited, label + ": the reference reaches the exit protocol");
      const RunOut got = RunOnce(&dut, geometry, &img, ref.recs.size(), false,
                                 mask_pc, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);
      AccountCoverage(ref, &cov);

      reporter.Check(got.exited && got.exit_code == kPassCode,
                     label + ": the machine reaches the exit protocol (exited=" +
                         Dec(got.exited) + " code=" + Dec(got.exit_code) + ")");
      // The JAL link, stated as a value: `jal ra, +12` at 0x004 writes 0x008,
      // and the program carries it to s3 through a register move.
      reporter.Check(sites.jalr_raw == sites.jalr_cleared + 1,
                     label + ": the JALR's computed target has bit 0 set (the "
                             "situation the case must produce)");
      bool saw_link = false;
      for (size_t i = 0; i < got.records.size(); i++) {
        if (got.records[i].pc != kBase + 0x04) continue;
        saw_link = true;
        reporter.Check(got.records[i].rd == RA && got.records[i].we &&
                           got.records[i].value == kBase + 0x08,
                       label + ": the JAL at " + U64(kBase + 0x04) +
                           " writes its link value 0x80000008: got " +
                           U64(got.records[i].value));
      }
      reporter.Check(saw_link, label + ": the JAL at 0x80000004 retired");
      // The JALR's link and its cleared target, stated directly.
      bool saw_jalr = false;
      for (size_t i = 0; i + 1 < got.records.size(); i++) {
        if (got.records[i].pc != kBase + 0x30) continue;
        saw_jalr = true;
        reporter.Check(got.records[i].rd == 2u && got.records[i].we &&
                           got.records[i].value == kBase + 0x34,
                       label + ": the JALR at " + U64(kBase + 0x30) +
                           " writes pc + 4 = 0x80000034: got " +
                           U64(got.records[i].value));
        reporter.Check(got.records[i + 1].pc == sites.jalr_cleared,
                       label + ": the JALR at " + U64(kBase + 0x30) +
                           " clears bit 0 of its computed target and nothing "
                           "else: the instruction after it is at " +
                           U64(got.records[i + 1].pc) + ", expected " +
                           U64(sites.jalr_cleared));
      }
      reporter.Check(saw_jalr, label + ": the JALR at 0x80000030 retired");
      // The taken branch's target and the two not-taken branches' fall-through,
      // stated as the PC after each one.
      bool saw_taken = false;
      bool saw_not_taken = false;
      for (size_t i = 0; i + 1 < got.records.size(); i++) {
        if (got.records[i].pc == kBase + 0x1C && got.records[i + 1].pc == kBase + 0x24) {
          saw_taken = true;
        }
        if (got.records[i].pc == kBase + 0x14 && got.records[i + 1].pc == kBase + 0x18) {
          saw_not_taken = true;
        }
        if (got.records[i].pc == kBase + 0x18 && got.records[i + 1].pc == kBase + 0x1C) {
          saw_not_taken = saw_not_taken && true;
        }
      }
      reporter.Check(saw_taken,
                     label + ": the taken branch reaches its target (0x8000001C -> "
                             "0x80000024)");
      reporter.Check(saw_not_taken,
                     label + ": a not-taken branch falls through to pc + 4 "
                             "(0x80000014 -> 0x80000018)");
      total_records += got.records.size();
      total_cycles += got.cycles;
      runs++;
      std::printf("  [%s] %zu retires, %llu cycles\n", label.c_str(),
                  got.records.size(),
                  static_cast<unsigned long long>(got.cycles));
    }

    // ---------------------------------------------------------------- pages
    {
      Img img;
      Sites sites;
      BuildPages(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = "pages";
      reporter.Check(ref.exited, label + ": the reference reaches the exit protocol");
      const RunOut got = RunOnce(&dut, geometry, &img, ref.recs.size(), false,
                                 mask_pc, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);
      AccountCoverage(ref, &cov);

      reporter.Check(got.exited && got.exit_code == kPassCode,
                     label + ": the machine reaches the exit protocol (exited=" +
                         Dec(got.exited) + " code=" + Dec(got.exit_code) + ")");
      // The two displacements, stated: a forward branch whose target is on the
      // far side of the page front, and a backward one whose target is on the
      // near side.
      std::set<uint64_t> pcs;
      for (const DutRecord& r : got.records) pcs.insert(r.pc);
      reporter.Check(pcs.count(sites.pos_cross_target) != 0 &&
                         pcs.count(sites.neg_cross_target) != 0,
                     label + ": both page-front crossings execute (positive target " +
                         U64(sites.pos_cross_target) + " and negative target " +
                         U64(sites.neg_cross_target) + " both retire)");
      bool saw_pos = false;
      bool saw_neg = false;
      for (size_t i = 0; i + 1 < got.records.size(); i++) {
        if (got.records[i].pc == sites.pos_cross_pc &&
            got.records[i + 1].pc == sites.pos_cross_target) {
          saw_pos = true;
        }
        if (got.records[i].pc == sites.neg_cross_pc &&
            got.records[i + 1].pc == sites.neg_cross_target) {
          saw_neg = true;
        }
      }
      reporter.Check(saw_pos,
                     label + ": a positive branch displacement crosses the 4 KiB "
                             "page front (" + U64(sites.pos_cross_pc) + " -> " +
                         U64(sites.pos_cross_target) + ")");
      reporter.Check(saw_neg,
                     label + ": a negative branch displacement crosses the 4 KiB "
                             "page front (" + U64(sites.neg_cross_pc) + " -> " +
                         U64(sites.neg_cross_target) + ")");
      total_records += got.records.size();
      total_cycles += got.cycles;
      runs++;
      std::printf("  [%s] %zu retires, %llu cycles\n", label.c_str(),
                  got.records.size(),
                  static_cast<unsigned long long>(got.cycles));
    }

    // ----------------------------------------------------------------- line
    {
      Img img;
      Sites sites;
      BuildLine(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = "line";
      reporter.Check(ref.exited, label + ": the reference reaches the exit protocol");
      const RunOut got = RunOnce(&dut, geometry, &img, ref.recs.size(), false,
                                 mask_pc, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);
      AccountCoverage(ref, &cov);

      reporter.Check(got.exited && got.exit_code == kPassCode,
                     label + ": the machine reaches the exit protocol (exited=" +
                         Dec(got.exited) + " code=" + Dec(got.exit_code) + ")");
      // The unaligned target: produced, and p0's behaviour stated. The C
      // extension makes a 2-mod-4 PC legal, so there is no instruction-address
      // fault here; what the case can state is that the machine goes there, that
      // the four bytes are assembled from the line the instruction starts in,
      // and that nothing traps.
      bool saw_target = false;
      for (size_t i = 0; i + 1 < got.records.size(); i++) {
        if (got.records[i].pc != sites.unaligned_target) continue;
        saw_target = true;
        uint32_t bits = 0;
        uint32_t len = 0;
        img.Fetch(sites.unaligned_target, false, &bits, &len);
        reporter.Check(got.records[i].len == 4u && got.records[i].insn == bits,
                       label + ": the instruction at the 2-mod-4 target " +
                           U64(sites.unaligned_target) + " retires with its own "
                           "four bytes (length " + Dec(got.records[i].len) +
                           ", bits " + U64(got.records[i].insn) + ")");
        reporter.Check(got.records[i + 1].pc == sites.unaligned_target + 4u,
                       label + ": the PC after a 2-mod-4 instruction is that "
                               "instruction plus four: next record at " +
                           U64(got.records[i + 1].pc));
      }
      reporter.Check(saw_target,
                     label + ": the JALR reaches its 2-mod-4 target " +
                         U64(sites.unaligned_target));
      reporter.Check((sites.unaligned_target % 4u) != 0,
                     label + ": the jump target the case produces is not 4-byte "
                             "aligned (" + U64(sites.unaligned_target) + ")");
      reporter.Check(got.traps == 0,
                     label + ": p0 takes no alignment fault on the 2-mod-4 target: "
                             "o_csr_trap=" + Dec(got.traps));
      total_records += got.records.size();
      total_cycles += got.cycles;
      runs++;
      std::printf("  [%s] %zu retires, target %s (2 mod 4), %llu cycles\n",
                  label.c_str(), got.records.size(),
                  U64(sites.unaligned_target).c_str(),
                  static_cast<unsigned long long>(got.cycles));
    }

    // --------------------------------------------------------------- fencei
    {
      Img img;
      Sites sites;
      BuildFenceI(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = "fencei";
      reporter.Check(ref.exited, label + ": the reference reaches the exit protocol");
      const RunOut got = RunOnce(&dut, geometry, &img, ref.recs.size(), false,
                                 mask_pc, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);
      AccountCoverage(ref, &cov);

      reporter.Check(got.exited && got.exit_code == kPassCode,
                     label + ": the machine reaches the exit protocol (exited=" +
                         Dec(got.exited) + " code=" + Dec(got.exit_code) + ")");
      // The fetch side: after FENCE.I the record at FIXPC carries the patched
      // instruction's bits and the patched value, not the stale ones.
      const uint32_t patched = 0x02200513u;   // addi t2, x0, 0x22
      bool saw_fix = false;
      for (size_t i = 0; i < got.records.size(); i++) {
        if (got.records[i].pc != sites.fix_pc) continue;
        saw_fix = true;
        reporter.Check(got.records[i].insn == patched,
                       label + ": after FENCE.I the new bytes are the ones "
                               "executed: the record at " + U64(sites.fix_pc) +
                           " carries " + U64(got.records[i].insn) +
                           ", expected the patched word " + U64(patched));
        reporter.Check(got.records[i].value == 0x22,
                       label + ": the patched instruction's effect is the "
                               "patched one: t2 = " + U64(got.records[i].value) +
                           ", expected 0x22");
      }
      reporter.Check(saw_fix,
                     label + ": the word immediately after FENCE.I " +
                         U64(sites.fix_pc) + " retires");
      total_records += got.records.size();
      total_cycles += got.cycles;
      runs++;
      std::printf("  [%s] %zu retires, FIXPC %s patched to %s, %llu cycles\n",
                  label.c_str(), got.records.size(), U64(sites.fix_pc).c_str(),
                  U64(patched).c_str(),
                  static_cast<unsigned long long>(got.cycles));
    }

    // ------------------------------------------------------------ wrongpath
    {
      Img img;
      Sites sites;
      BuildWrongPath(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = "wrongpath";
      reporter.Check(ref.exited, label + ": the reference reaches the exit protocol");
      const RunOut got = RunOnce(&dut, geometry, &img, ref.recs.size(), false,
                                 mask_pc, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref);
      AccountCoverage(ref, &cov);

      reporter.Check(got.exited && got.exit_code == kPassCode,
                     label + ": the machine reaches the exit protocol (exited=" +
                         Dec(got.exited) + " code=" + Dec(got.exit_code) + ")");
      // The situation produced: the front end really did fetch into both skipped
      // regions and was really answered with a fault there.
      int branch_gap_faults = 0;
      int jal_gap_faults = 0;
      for (uint64_t addr : got.faults) {
        if (addr >= kBase + 0x008 && addr < kBase + 0x104) branch_gap_faults++;
        if (addr >= kBase + 0x10C && addr < kBase + 0x208) jal_gap_faults++;
      }
      cov.wrongpath_faults += branch_gap_faults + jal_gap_faults;
      reporter.Check(branch_gap_faults >= 1,
                     label + ": the branch's skipped region is fetched and faults (" +
                         Dec(branch_gap_faults) + " faulting fetches at 0x80000008.."
                         "0x80000103)");
      reporter.Check(jal_gap_faults >= 1,
                     label + ": the JAL's skipped region is fetched and faults (" +
                         Dec(jal_gap_faults) + " faulting fetches at 0x8000010C.."
                         "0x80000207)");
      // The leak: no architectural trace at all.
      int trace_at_gap = 0;
      for (const DutRecord& r : got.records) {
        const bool in_branch_gap = (r.pc >= kBase + 0x008 && r.pc < kBase + 0x104);
        const bool in_jal_gap = (r.pc >= kBase + 0x10C && r.pc < kBase + 0x208);
        if (in_branch_gap || in_jal_gap) {
          trace_at_gap++;
          if (trace_at_gap <= 3) {
            reporter.Check(false, label + ": a wrong-path fetch leaves no "
                                            "architectural trace: an instruction "
                                            "retires at " + U64(r.pc) +
                                    ", an address no transfer reaches");
          }
        }
      }
      reporter.Check(trace_at_gap == 0,
                     label + ": a wrong-path fetch leaves no architectural trace "
                             "(no instruction retires at any address a transfer "
                             "skipped)");
      reporter.Check(!got.stopped,
                     label + ": the machine never stops on a wrong-path fetch "
                             "fault");
      reporter.Check(got.illegal == 0 && got.unsupported == 0,
                     label + ": the wrong-path fault never reaches dispatch as a "
                             "refused macro: illegal=" + Dec(got.illegal) +
                         " unsupported=" + Dec(got.unsupported));
      reporter.Check(got.traps == 0,
                     label + ": a wrong-path fetch fault takes no trap: "
                             "o_csr_trap=" + Dec(got.traps));
      total_records += got.records.size();
      total_cycles += got.cycles;
      runs++;
      std::printf("  [%s] %zu retires, %d wrong-path faults squashed, %llu cycles\n",
                  label.c_str(), got.records.size(),
                  branch_gap_faults + jal_gap_faults,
                  static_cast<unsigned long long>(got.cycles));
    }

    // ----------------------------------------------------------- fetchfault
    {
      Img img;
      Sites sites;
      BuildFetchFault(&img, &sites);
      const RefTrace ref = RunReference(img, kBase);
      const std::string label = "fetchfault";
      reporter.Check(ref.stopped,
                     label + ": the reference cannot fetch the JALR's target");
      const RunOut got = RunOnce(&dut, geometry, &img, ref.recs.size(), true,
                                 mask_pc, label);
      CompareStream(&reporter, label, got, ref);
      CheckStructure(&reporter, label, got, img, ref, /*expect_refusal=*/true);

      reporter.Check(got.stopped,
                     label + ": p0 stops at a correct-path instruction-access "
                             "fault (it does not take a trap for it)");
      bool retired_at_fault = false;
      for (const DutRecord& r : got.records) {
        if (r.pc == sites.fault_pc) retired_at_fault = true;
      }
      reporter.Check(!retired_at_fault,
                     label + ": nothing retires at the unfetchable address " +
                         U64(sites.fault_pc));
      reporter.Check(got.traps == 0,
                     label + ": the fetch fault takes no trap: o_csr_trap=" +
                         Dec(got.traps));
      bool refused_delivery = false;
      std::string seen;
      for (const Delivery& d : got.deliveries) {
        if (d.pc == 0) continue;
        if (seen.size() < 200) seen += U64(d.pc) + (d.is_insn ? "i " : "x ");
        if (d.pc == sites.fault_pc && !d.is_insn) refused_delivery = true;
      }
      reporter.Check(refused_delivery,
                     label + ": the fault is reported at the instruction's own "
                             "start address: expected a refused delivery at " +
                         U64(sites.fault_pc) + ", the deliveries seen were " + seen);
      total_records += got.records.size();
      total_cycles += got.cycles;
      runs++;
      std::printf("  [%s] stop at %s, %zu retires, %llu cycles\n", label.c_str(),
                  U64(sites.fault_pc).c_str(), got.records.size(),
                  static_cast<unsigned long long>(got.cycles));
    }

    // ----------------------------------------------- the case's own coverage
    reporter.Check(cov.jalr_bit0_set >= 1,
                   "coverage: a JALR whose computed target has bit 0 set is "
                   "produced (" + Dec(cov.jalr_bit0_set) + ")");
    reporter.Check(cov.jalr_2mod4 >= 1,
                   "coverage: a JALR whose cleared target is not 4-byte aligned is "
                   "produced (" + Dec(cov.jalr_2mod4) + ")");
    reporter.Check(cov.pos_page_cross >= 1,
                   "coverage: a taken transfer crosses a 4 KiB page front upward (" +
                       Dec(cov.pos_page_cross) + ")");
    reporter.Check(cov.neg_page_cross >= 1,
                   "coverage: a taken transfer crosses a 4 KiB page front downward (" +
                       Dec(cov.neg_page_cross) + ")");
    reporter.Check(cov.line_straddle >= 1,
                   "coverage: an instruction whose four bytes cross the harness's "
                   "eight-byte line retires (" + Dec(cov.line_straddle) + ")");
    reporter.Check(cov.page_straddle >= 1,
                   "coverage: an instruction whose four bytes cross a 4 KiB page "
                   "front retires (" + Dec(cov.page_straddle) + ")");
    reporter.Check(cov.wrongpath_faults >= 2,
                   "coverage: wrong-path instruction-fetch faults are produced (" +
                       Dec(cov.wrongpath_faults) + ")");

    passed = (reporter.failures() == 0);
    detail = "checks=" + Dec(reporter.checks()) + " runs=" + Dec(runs) +
             " retires=" + Dec(total_records) + " line_straddles=" +
             Dec(cov.line_straddle) + " page_straddles=" + Dec(cov.page_straddle) +
             " wrongpath_faults=" + Dec(cov.wrongpath_faults) + " cycles=" +
             Dec(total_cycles) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the PC and fetch contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  const bool ok = passed && (reporter.failures() == 0);
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
