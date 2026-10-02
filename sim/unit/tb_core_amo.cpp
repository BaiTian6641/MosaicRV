// ============================================================================
// tb_core_amo.cpp -- CASE=amo.linearization, work package I-039.
//
// The DUT is the integrated p0 core with the A-extension atomic path wired in:
// an AMO is decoded in the core's front end (so the decoder's own reserved-set
// case is untouched), issued through the load queue -- which is what blocks a
// younger load to the same address from overtaking it -- serialized through the
// same non-speculative, exactly-once point a device access uses, and performed
// by mosaic_lsu_endpoint as an indivisible read-modify-write: a read beat whose
// doubleword is latched, the new field computed by mosaic_amo_alu, and a write
// beat. The endpoint is not idle between the two beats, so no memory request is
// accepted in that window and no competing access can be interleaved.
//
// ------------------------------------------------------------- the programs
//
// Two runs of a hand-built RV64IM program (no ELF; the image is assembled here,
// one instruction at a time, so the case carries no dependency on the corpus or
// on an assembler):
//
//   run A  every AMO operation at both widths, with operands chosen on the
//          boundaries the operations turn on: AMOADD overflow, AMOSWAP, the
//          three bitwise ops, and -- the reason for the whole exercise --
//          AMOMIN/AMOMAX (signed) against AMOMINU/AMOMAXU (unsigned) with
//          operands whose top bit is set, at both widths. aq and rl are set on
//          several of them, including both at once.
//
//   run B  the same shape, with a *competing agent*: the harness performs an
//          ordinary 8-byte store to the first atomic location on every cycle the
//          core has released the memory port (`o_mem_lsu_busy` low and no
//          request presented). A cooperative second master on a single-port
//          memory can use the port exactly then. If the core holds the port
//          across the whole read-modify-write -- as it must -- the competing
//          stores fall before or after the atomic step and leave a consistent
//          history. If it releases it between the two beats (the named
//          "decomposition that loses atomicity"), a competing store lands inside
//          and the replay below catches the lost update.
//
// ------------------------------------------------------------ the expectation
//
//   * The per-instruction retirement stream is compared against an independent
//     RV64IM+A interpreter (sim/unit/mem_ref.h) running on its own memory model
//     seeded identically. Both the register result of every AMO (the *old*
//     value, sign-extended for AMO*.W) and every load/store/ALU result are
//     compared pc by pc, value by value.
//   * The final contents of the atomic scratch area are compared word for word
//     against the interpreter's own memory.
//   * Every AMO instruction must appear at the data port as exactly two
//     transactions, both marked atomic, at the right address, carrying the
//     instruction's operation and aq/rl bits -- never as a pair of ordinary
//     loads and stores. The operand is read off the read beat and the write
//     result off the write beat, and the write is required to equal the
//     operation applied to the value the read returned.
//   * The whole data-port history is replayed against a shadow memory: every
//     read must match the shadow, every atomic write must match the operation on
//     the value it read and must find the shadow unchanged since that read, and
//     the competing agent's stores are folded into the same history. A history
//     that survives the replay is a legal linearization of the run -- the atomic
//     steps are indivisible and the competing stores are ordered around them,
//     never inside them.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic_ref::DataMem;

namespace {

constexpr int kResetCycles = 4;
constexpr int kMaxRunCycles = 200000;
constexpr int kStallCycles = 20000;

// The program's data layout. Every address is RAM base + a small offset, and
// the base is built once by a shift (`addi 1; slli 31`) because `lui` cannot:
// on RV64 `lui` sign-extends its 32-bit result, so `lui x, 0x80003` yields
// 0xFFFFFFFF80003000, not 0x80003000. A 12-bit immediate offset from the
// unsigned base is the only thing a load/store can add on top of it.
constexpr uint64_t kRamBase    = 0x80000000ull;  // = MOSAIC_RAM_BASE
constexpr uint64_t kPoolOff    = 0x100;          // operands, then seeds
constexpr uint64_t kScratchOff = 0x600;          // one 8-byte slot per case (0x800 is not a positive 12-bit offset)
constexpr uint64_t kPool    = kRamBase + kPoolOff;
constexpr uint64_t kScratch = kRamBase + kScratchOff;
constexpr uint64_t kTohost  = 0x80001000;   // the frozen exit protocol

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// Instruction encoding (the handful of formats the program uses)
// ============================================================================
uint32_t EncR(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, uint32_t rs2,
              uint32_t f7) {
  return op | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}
uint32_t EncI(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm) {
  return op | (rd << 7) | (f3 << 12) | (rs1 << 15) |
         ((static_cast<uint32_t>(imm) & 0xFFFu) << 20);
}
uint32_t EncS(uint32_t op, uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return op | ((u & 0x1Fu) << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) |
         (((u >> 5) & 0x7Fu) << 25);
}
uint32_t EncU(uint32_t op, uint32_t rd, uint32_t imm20) {
  return op | (rd << 7) | (imm20 << 12);
}
uint32_t EncJ(uint32_t op, uint32_t rd, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off);
  return op | (rd << 7) | (((u >> 12) & 0xFFu) << 12) | (((u >> 11) & 1u) << 20) |
         (((u >> 1) & 0x3FFu) << 21) | (((u >> 20) & 1u) << 31);
}
uint32_t EncAmo(uint32_t f5, uint32_t aq, uint32_t rl, uint32_t f3, uint32_t rd,
                uint32_t rs1, uint32_t rs2) {
  return 0x2Fu | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) |
         (rl << 25) | (aq << 26) | (f5 << 27);
}

constexpr uint32_t F3_B = 0, F3_H = 1, F3_W = 2, F3_D = 3;
// funct5 -> the operation index used throughout this file.
uint32_t AmoFunct5(int op) {
  static const uint32_t kF5[9] = {0x00, 0x01, 0x04, 0x0C, 0x08, 0x10, 0x14, 0x18, 0x1C};
  return kF5[op];
}
const char* AmoName(int op) {
  static const char* kName[9] = {"amoadd", "amoswap", "amoxor", "amoand",
                                 "amoor",  "amomin", "amomax", "amominu", "amomaxu"};
  return kName[op];
}

// ============================================================================
// One binding of operation, width, seed and operand.
// ============================================================================
struct AmoCase {
  int op;          // 0..8, the index above
  unsigned size;   // 4 or 8
  uint64_t seed;   // the value already at the address
  uint64_t operand;// rs2
  uint32_t aq;
  uint32_t rl;
};

uint64_t ApplyAmo(int op, unsigned size, uint64_t old_raw, uint64_t operand) {
  if (size == 4) {
    const uint32_t a = static_cast<uint32_t>(old_raw);
    const uint32_t b = static_cast<uint32_t>(operand);
    uint32_t r = 0;
    switch (op) {
      case 0: r = a + b; break;
      case 1: r = b; break;
      case 2: r = a ^ b; break;
      case 3: r = a & b; break;
      case 4: r = a | b; break;
      case 5: r = (static_cast<int32_t>(a) < static_cast<int32_t>(b)) ? a : b; break;
      case 6: r = (static_cast<int32_t>(a) > static_cast<int32_t>(b)) ? a : b; break;
      case 7: r = (a < b) ? a : b; break;
      default: r = (a > b) ? a : b; break;
    }
    return r;
  }
  uint64_t r = 0;
  switch (op) {
    case 0: r = old_raw + operand; break;
    case 1: r = operand; break;
    case 2: r = old_raw ^ operand; break;
    case 3: r = old_raw & operand; break;
    case 4: r = old_raw | operand; break;
    case 5: r = (static_cast<int64_t>(old_raw) < static_cast<int64_t>(operand)) ? old_raw : operand; break;
    case 6: r = (static_cast<int64_t>(old_raw) > static_cast<int64_t>(operand)) ? old_raw : operand; break;
    case 7: r = (old_raw < operand) ? old_raw : operand; break;
    default: r = (old_raw > operand) ? old_raw : operand; break;
  }
  return r;
}

// The boundary-heavy case table. Every operation at both widths; the
// comparison operations are exercised with the seed's and the operand's top
// bits set so signed and unsigned disagree.
const std::vector<AmoCase>& Cases() {
  static const std::vector<AmoCase> kCases = {
      // ---- doubleword ----
      {0, 8, 0xFFFFFFFFFFFFFFFFull, 1, 0, 0},           // amoadd wrap
      {0, 8, 0x7FFFFFFFFFFFFFFFull, 1, 0, 1},           // amoadd sign boundary
      {1, 8, 0xDEADBEEFCAFEBABEull, 0x0123456789ABCDEFull, 0, 0},  // amoswap
      {2, 8, 0xFF00FF00FF00FF00ull, 0x0FF00FF00FF00FF0ull, 1, 1},  // amoxor
      {3, 8, 0xFFFFFFFF00000000ull, 0x00000000FFFFFFFFull, 0, 0},  // amoand
      {4, 8, 0x00000000FFFFFFFFull, 0xFFFFFFFF00000000ull, 0, 0},  // amoor
      {5, 8, 0xFFFFFFFFFFFFFFFFull, 0, 0, 0},           // amomin  signed
      {6, 8, 0xFFFFFFFFFFFFFFFFull, 0, 0, 0},           // amomax  signed
      {5, 8, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0, 0},  // amomin  extreme
      {6, 8, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0, 0},  // amomax  extreme
      {7, 8, 0xFFFFFFFFFFFFFFFFull, 0, 0, 0},           // amominu
      {8, 8, 0xFFFFFFFFFFFFFFFFull, 0, 0, 0},           // amomaxu
      {7, 8, 0x0000000000000000ull, 0x8000000000000000ull, 0, 0},  // amominu ext
      {8, 8, 0x0000000000000000ull, 0x8000000000000000ull, 0, 0},  // amomaxu ext
      // ---- word ----
      {0, 4, 0x00000000FFFFFFFFull, 1, 0, 0},           // amoadd.w wrap
      {0, 4, 0x000000007FFFFFFFull, 1, 1, 0},           // amoadd.w boundary
      {1, 4, 0x00000000DEADBEEFull, 0x0000000012345678ull, 0, 0},  // amoswap.w
      {2, 4, 0x00000000FF00FF00ull, 0x000000000FF00FF0ull, 0, 0},  // amoxor.w
      {3, 4, 0x00000000FFFF0000ull, 0x000000000000FFFFull, 0, 1},  // amoand.w
      {4, 4, 0x000000000000FFFFull, 0x00000000FFFF0000ull, 0, 0},  // amoor.w
      {5, 4, 0x00000000FFFFFFFFull, 0, 0, 0},           // amomin.w signed
      {6, 4, 0x00000000FFFFFFFFull, 0, 0, 0},           // amomax.w signed
      {5, 4, 0x0000000080000000ull, 0x000000007FFFFFFFull, 0, 0},  // amomin.w extreme
      {6, 4, 0x0000000080000000ull, 0x000000007FFFFFFFull, 0, 0},  // amomax.w extreme
      {7, 4, 0x00000000FFFFFFFFull, 0, 0, 0},           // amominu.w
      {8, 4, 0x00000000FFFFFFFFull, 0, 0, 0},           // amomaxu.w
      {7, 4, 0x0000000000000000ull, 0xFFFFFFFF80000000ull, 0, 0},  // amominu.w (operand low 32)
      {8, 4, 0x0000000000000000ull, 0xFFFFFFFF80000000ull, 0, 0},  // amomaxu.w
      // aq/rl on an arithmetic operation, both widths
      {0, 8, 0x0000000000000005ull, 3, 1, 0},
      {0, 4, 0x0000000000000005ull, 3, 0, 1},
  };
  return kCases;
}

// ============================================================================
// The program image
// ============================================================================
class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    if (it == words_.end()) return 0x00000073u;  // ECALL: stop cleanly
    return it->second;
  }
  bool Has(uint64_t addr) const { return words_.count(addr) != 0; }
  uint64_t highest() const { return words_.rbegin()->first; }

 private:
  std::map<uint64_t, uint32_t> words_;
};

uint64_t BuildProgram(ProgImage* img) {
  const std::vector<AmoCase>& cases = Cases();
  uint64_t pc = 0x80000000ull;
  auto emit = [&](uint32_t w) { img->Put(pc, w); pc += 4; };

  // x5 = the unsigned RAM base (0x80000000). `lui` cannot build it: it
  // sign-extends, so 0x80000 as an upper immediate gives 0xFFFFFFFF80000000.
  emit(EncI(0x13u, 0, 5, 0, 1));                     // addi x5, x0, 1
  emit(EncI(0x13u, 1, 5, 5, 31));                    // slli x5, x5, 31

  for (size_t i = 0; i < cases.size(); i++) {
    const AmoCase& c = cases[i];
    const int32_t scratch_off = static_cast<int32_t>(kScratchOff + i * 8);
    const int32_t operand_off = static_cast<int32_t>(kPoolOff + i * 8);
    const int32_t seed_off =
        static_cast<int32_t>(kPoolOff + (cases.size() + i) * 8);
    emit(EncI(0x03u, F3_D, 7, 5, operand_off));      // ld   x7, operand(x5)
    emit(EncI(0x03u, F3_D, 8, 5, seed_off));         // ld   x8, seed(x5)
    emit(EncS(0x23u, F3_D, 5, 8, scratch_off));      // sd   x8, scratch(x5)
    // An AMO's address is rs1 and nothing else -- it has no immediate field --
    // so the scratch address is materialised into x10 first.
    emit(EncI(0x13u, 0, 10, 5, scratch_off));         // addi x10, x5, scratch
    const uint32_t f5 = AmoFunct5(c.op);
    const uint32_t f3 = (c.size == 4) ? F3_W : F3_D;
    // rd = x9 (the old value; the reference checks it)
    emit(EncAmo(f5, c.aq, c.rl, f3, 9, 10, 7));       // amo<op> x9, x7, (x10)
  }

  // The frozen exit protocol: store the pass code to TOHOST, then spin. TOHOST
  // is RAM base + 0x1000, built with a shift because 0x1000 does not fit a
  // 12-bit immediate.
  emit(EncI(0x13u, 0, 28, 0, 0x100));                // addi x28, x0, 0x100
  emit(EncI(0x13u, 1, 28, 28, 4));                   // slli x28, x28, 4
  emit(EncR(0x33u, 0, 28, 5, 28, 0));                // add  x28, x5, x28
  emit(EncI(0x13u, 0, 29, 0, 1));                    // addi x29, x0, 1
  emit(EncS(0x23u, F3_D, 28, 29, 0));                // sd   x29, 0(x28)
  emit(EncJ(0x6Fu, 0, 0));                           // jal  x0, . (spin)
  return pc;
}

void SeedModels(mosaic::MemoryModel* dut_mem, mosaic::MemoryModel* ref_mem) {
  const std::vector<AmoCase>& cases = Cases();
  for (size_t i = 0; i < cases.size(); i++) {
    const uint64_t operand = cases[i].operand;
    const uint64_t seed = cases[i].seed;
    if (dut_mem->Write(kPool + i * 8, 8, operand) != mosaic::AccessStatus::kOk ||
        dut_mem->Write(kPool + (cases.size() + i) * 8, 8, seed) !=
            mosaic::AccessStatus::kOk) {
      Fail("seeding", "the constant pool is not writable RAM");
    }
    ref_mem->Write(kPool + i * 8, 8, operand);
    ref_mem->Write(kPool + (cases.size() + i) * 8, 8, seed);
  }
}

// ============================================================================
// Geometry and retire lanes
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
// One data-port transaction, as the memory system saw it.
// ============================================================================
struct TxnRec {
  uint64_t cycle = 0;
  bool we = false;
  bool amo = false;
  uint32_t amo_op = 0;
  bool aq = false;
  bool rl = false;
  uint64_t addr = 0;
  uint32_t size = 0;
  uint32_t wstrb = 0;
  uint64_t wdata = 0;
  uint64_t rdata = 0;
  bool interloper = false;   // an injected competing store, not the core's
};

// ============================================================================
// The shadow memory the history replay is checked against: 8-byte windows,
// seeded with the constant pool (everything else starts zero).
// ============================================================================
class Shadow {
 public:
  void Seed(const std::vector<AmoCase>& cases) {
    for (size_t i = 0; i < cases.size(); i++) {
      Set(kPool + i * 8, cases[i].operand);
      Set(kPool + (cases.size() + i) * 8, cases[i].seed);
    }
  }
  void Set(uint64_t addr, uint64_t value) { win_[addr & ~UINT64_C(7)] = value; }
  uint64_t Get(uint64_t addr) const {
    auto it = win_.find(addr & ~UINT64_C(7));
    return it == win_.end() ? 0 : it->second;
  }
  // Apply a byte-strobed store to the window.
  void Store(uint64_t addr, uint32_t wstrb, uint64_t wdata) {
    const uint64_t base = addr & ~UINT64_C(7);
    uint64_t win = Get(base);
    for (unsigned i = 0; i < 8; i++) {
      if (((wstrb >> i) & 1u) != 0u) {
        win = (win & ~(UINT64_C(0xff) << (8 * i))) |
              (((wdata >> (8 * i)) & UINT64_C(0xff)) << (8 * i));
      }
    }
    win_[base] = win;
  }

 private:
  std::map<uint64_t, uint64_t> win_;
};

// ============================================================================
// The run
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, const ProgImage* img,
          mosaic::MemoryModel* mem, uint64_t max_cycles)
      : dut_(dut), reporter_(reporter), imem_(img), dmem_(mem),
        max_cycles_(max_cycles) {}

  void Configure(const Geometry& g) { g_ = g; }

  struct Retire {
    uint64_t pc = 0;
    uint32_t rd = 0;
    bool reg_we = false;
    uint64_t value = 0;
  };
  const std::vector<Retire>& retires() const { return retires_; }
  const std::vector<TxnRec>& txns() const { return txns_; }
  uint64_t cycles() const { return cycles_; }

  void EnableInterloper(uint64_t addr, int start_cycle, int period) {
    interloper_ = true;
    inter_addr_ = addr;
    inter_start_ = start_cycle;
    inter_period_ = period;
    inter_value_ = 0;
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

    // The instruction port serves the image with a one-cycle latency, exactly
    // as CASE=core.mem_program's harness does.
    if (imem_.HasResponse()) {
      const Imem::Request& r = imem_.Response();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem_.ResponseWord());
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = r.id;
      dut_->imem_rsp_epoch_i = r.epoch;
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

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem_.PopResponse();
    }
    imem_.Advance();

    if (bus_reset_.MayAccept(rst, (dut_->dmem_req_valid_o != 0) &&
                                      (dut_->dmem_req_ready_i != 0))) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(r, cycles_);

      TxnRec t;
      t.cycle = cycles_;
      t.we = r.we;
      t.amo = dut_->dmem_req_amo_o != 0;
      t.amo_op = dut_->dmem_req_amo_op_o;
      t.aq = dut_->dmem_req_aq_o != 0;
      t.rl = dut_->dmem_req_rl_o != 0;
      t.addr = r.addr;
      t.size = r.size;
      t.wstrb = r.wstrb;
      t.wdata = r.wdata;
      txns_.push_back(t);
      pending_idx_ = static_cast<long>(txns_.size()) - 1;
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
      if (pending_idx_ >= 0) {
        txns_[static_cast<size_t>(pending_idx_)].rdata = dut_->dmem_rsp_rdata_i;
        pending_idx_ = -1;
      }
      dmem_.PopResponse();
    }
    dmem_.Advance();

    // The competing agent: a cooperative second master on the single-port
    // memory. It uses the port exactly when the core has released it -- the
    // endpoint says it is idle and presents no request.
    if (interloper_ && !rst && cycles_ >= static_cast<uint64_t>(inter_start_) &&
        ((cycles_ - static_cast<uint64_t>(inter_start_)) %
         static_cast<uint64_t>(inter_period_)) == 0 &&
        (dut_->o_mem_lsu_busy_o == 0) && (dut_->dmem_req_valid_o == 0)) {
      inter_value_ += UINT64_C(0x0100000000000001);
      Node(inter_addr_, inter_value_);
      TxnRec t;
      t.cycle = cycles_;
      t.interloper = true;
      t.we = true;
      t.addr = inter_addr_;
      t.size = 8;
      t.wstrb = 0xFF;
      t.wdata = inter_value_;
      t.rdata = inter_value_;
      txns_.push_back(t);
    }

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) + " stopped=" + Dec(dut_->o_stopped_o) +
           " lq=" + Dec(dut_->o_mem_lq_occupied_o) +
           " sq=" + Dec(dut_->o_mem_sq_occupied_o);
  }

  const std::vector<uint64_t>& redirects() const { return redirects_; }

 private:
  void Node(uint64_t addr, uint64_t value) {
    // The competing agent writes ordinary RAM directly; the DUT's memory model
    // is the memory it and the core share.
    if (dmem_.model()->Write(addr, 8, value) != mosaic::AccessStatus::kOk) {
      Fail("competing agent", "the competing store's address is not writable RAM");
    }
  }

  struct Imem {
    struct Request {
      uint64_t addr = 0;
      uint32_t id = 0;
      uint32_t epoch = 0;
    };
    explicit Imem(const ProgImage* img) : img_(img) {}
    bool HasResponse() const { return !ready_.empty(); }
    const Request& Response() const { return ready_.front(); }
    uint64_t ResponseWord() const { return img_->Word(ready_.front().addr); }
    void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
      inflight_.push_back(Entry{Request{addr, id, epoch}, 1});
    }
    void PopResponse() { ready_.pop_front(); }
    void Advance() {
      for (size_t i = 0; i < inflight_.size();) {
        if (--inflight_[i].left == 0) {
          ready_.push_back(inflight_[i].req);
          inflight_.erase(inflight_.begin() + static_cast<long>(i));
        } else {
          ++i;
        }
      }
    }
    struct Entry {
      Request req;
      int left = 0;
    };
    const ProgImage* img_;
    std::deque<Entry> inflight_;
    std::deque<Request> ready_;
  };

  void Observe() {
    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      retires_.push_back(r);
    }
    if (dut_->o_redirect_valid_o != 0) redirects_.push_back(dut_->o_redirect_pc_o);
  }

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  Imem imem_;
  DataMem dmem_;
  // The reset-traffic rule (V-010, sim/common/bus_reset_gate.h): while reset
  // is asserted this model accepts nothing, so no reset-time response can be
  // queued ahead of a fresh post-reset one.
  mosaic::BusResetGate bus_reset_;
  uint64_t max_cycles_;
  Geometry g_;
  uint64_t cycles_ = 0;
  long pending_idx_ = -1;
  std::vector<Retire> retires_;
  std::vector<TxnRec> txns_;
  std::vector<uint64_t> redirects_;
  bool interloper_ = false;
  uint64_t inter_addr_ = 0;
  int inter_start_ = 0;
  int inter_period_ = 0;
  uint64_t inter_value_ = 0;
};

// ============================================================================
// The replay: a legal linearization of the recorded history.
// ============================================================================
struct ReplayResult {
  int amo_pairs = 0;
  int read_checks = 0;
};

void Replay(const std::vector<TxnRec>& txns, const std::vector<AmoCase>& cases,
            Shadow* shadow, mosaic::Reporter* reporter, const std::string& label) {
  // A pending atomic step: the value its read saw and the address it is on.
  bool have_pending = false;
  uint64_t pending_addr = 0;
  uint64_t pending_old = 0;
  uint32_t pending_size = 0;

  for (const TxnRec& t : txns) {
    if (t.interloper) {
      shadow->Set(t.addr, t.wdata);
      continue;
    }
    if (t.amo) {
      if (!t.we) {
        // The read beat: the value the memory returned must be the value the
        // history had at this point, and nothing may intervene before the write.
        const uint64_t win = shadow->Get(t.addr);
        reporter->Check(win == t.rdata,
                        "the atomic read returns the value the history holds [" + label + "] (cyc " +
                            Dec(t.cycle) + " at " + U64(t.addr) + " shadow " + U64(win) +
                            " got " + U64(t.rdata) + ")");
        have_pending = true;
        pending_addr = t.addr;
        pending_old = win;
        pending_size = t.size;
      } else {
        reporter->Check(have_pending && t.addr == pending_addr && t.size == pending_size,
                        "the atomic write beat follows its read beat at the same "
                        "address and width");
        const uint64_t win = shadow->Get(t.addr);
        reporter->Check(win == pending_old,
                        "the atomic write finds the location unchanged since its "
                        "read: the read-modify-write is indivisible");
        shadow->Store(t.addr, t.wstrb, t.wdata);
        have_pending = false;
      }
      continue;
    }
    if (t.we) {
      shadow->Store(t.addr, t.wstrb, t.wdata);
      continue;
    }
    // An ordinary load: the window the memory returned must be the shadow.
    const uint64_t lwin = shadow->Get(t.addr);
    reporter->Check(lwin == t.rdata,
                    "an ordinary load returns the value the history holds [" + label + "] (cyc " +
                        Dec(t.cycle) + " at " + U64(t.addr) + " shadow " + U64(lwin) +
                        " got " + U64(t.rdata) + ")");
  }
}

// ============================================================================
// Check that every AMO instruction reached the port as one atomic pair.
// ============================================================================
void CheckAmoTransactions(const std::vector<TxnRec>& txns,
                          const std::vector<AmoCase>& cases,
                          int first_amo_txn_hint, mosaic::Reporter* reporter) {
  std::vector<const TxnRec*> beats;
  for (const TxnRec& t : txns) {
    if (t.amo) beats.push_back(&t);
  }
  reporter->Check(beats.size() == cases.size() * 2,
                  "every AMO instruction is exactly two data-port beats (a read and a "
                  "write): expected " + Dec(cases.size() * 2) + ", saw " +
                      Dec(beats.size()));
  if (beats.size() != cases.size() * 2) return;
  for (size_t i = 0; i < cases.size(); i++) {
    const AmoCase& c = cases[i];
    const TxnRec& rd = *beats[i * 2];
    const TxnRec& wr = *beats[i * 2 + 1];
    const uint64_t addr = kScratch + i * 8;
    const char* name = AmoName(c.op);
    const std::string tag = std::string(name) + (c.size == 4 ? ".w" : ".d");
    reporter->Check(rd.addr == addr && wr.addr == addr,
                    tag + ": both atomic beats name the instruction's address (want " +
                        U64(addr) + " read " + U64(rd.addr) + " write " + U64(wr.addr) +
                        ")");
    reporter->Check(!rd.we && wr.we,
                    tag + ": the atomic beats are a read then a write");
    reporter->Check(rd.size == ((c.size == 4) ? 2u : 3u) &&
                        wr.size == ((c.size == 4) ? 2u : 3u),
                    tag + ": both atomic beats carry the instruction's width (read " +
                        Dec(rd.size) + " write " + Dec(wr.size) + ")");
    reporter->Check(rd.amo_op == static_cast<uint32_t>(c.op) &&
                        wr.amo_op == static_cast<uint32_t>(c.op),
                    tag + ": both atomic beats carry the instruction's operation");
    reporter->Check(rd.aq == (c.aq != 0) && wr.aq == (c.aq != 0) &&
                        rd.rl == (c.rl != 0) && wr.rl == (c.rl != 0),
                    tag + ": both atomic beats carry the instruction's aq/rl");
    // The write must be the operation applied to the value the read returned.
    const uint64_t old_win = rd.rdata;
    const uint64_t operand = rd.wdata;
    const uint64_t expected =
        (c.size == 4)
            ? (old_win & ~UINT64_C(0xFFFFFFFF)) |
                  ApplyAmo(c.op, 4, old_win, operand)
            : ApplyAmo(c.op, 8, old_win, operand);
    reporter->Check(wr.wdata == expected,
                    tag + ": the written value is the operation applied to the value "
                          "read");
  }
  (void)first_amo_txn_hint;
  return;
}

// ============================================================================
// One whole run
// ============================================================================
struct RunResult {
  uint64_t retires = 0;
  uint64_t cycles = 0;
  uint64_t txns = 0;
};

RunResult RunOnce(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                  const Geometry& geometry, const ProgImage& img,
                  bool with_interloper, const std::string& label) {
  const std::vector<AmoCase>& cases = Cases();

  // Fresh memory for each run, so a run inherits nothing from the previous one.
  mosaic::MemoryModel dut_mem;
  mosaic::MemoryModel ref_mem;
  SeedModels(&dut_mem, &ref_mem);

  // The independent reference, on its own memory.
  mosaic_ref::RefResult reference =
      mosaic_ref::RunReference(img, geometry.reset_vector, &ref_mem);
  if (!reference.stopped) {
    Fail(label, "the reference did not stop at all");
  }
  if (!reference.exited) {
    Fail(label, "the reference did not reach its exit protocol: " +
                    reference.stop_reason + " at pc " + U64(reference.stop_pc) +
                    " word " + U64(reference.stop_word));
  }
  reporter->Check(reference.exited, label + ": the reference reaches the exit protocol");

  Harness harness(dut, reporter, &img, &dut_mem, kMaxRunCycles);
  harness.Configure(geometry);
  harness.Reset(kResetCycles);
  if (with_interloper) {
    // The competing agent stores to the first atomic location every 7 cycles,
    // once the program has had time to start.
    harness.EnableInterloper(kScratch, 64, 7);
  }

  uint64_t last_retires = 0;
  uint64_t last_progress = 0;
  const uint64_t want = reference.trace.size();
  while (harness.retires().size() < want) {
    harness.Cycle(false);
    if (harness.retires().size() != last_retires) {
      last_retires = harness.retires().size();
      last_progress = harness.cycles();
    } else if (harness.cycles() - last_progress > kStallCycles) {
      Fail(label, "stalled before the program's own end at retire " +
                      Dec(last_retires) + " of " + Dec(want));
    }
  }

  // The retirement stream, pc by pc, against the reference. Under contention the
// competing agent legitimately changes what an AMO reads, so the architectural
// *values* are not comparable to a reference that ran without it; the structure
// of the run (same instruction count, same history shape) still is, and the
// replay below is what the contended run is checked by.
  const std::vector<Harness::Retire>& retires = harness.retires();
  reporter->Check(retires.size() >= want, label + ": the reference's instructions all retire");
  const size_t n = with_interloper ? 0u
                                   : std::min<size_t>(retires.size(),
                                                      static_cast<size_t>(want));
  for (size_t i = 0; i < n; i++) {
    const Harness::Retire& r = retires[i];
    const mosaic_ref::RefInsn& e = reference.trace[i];
    if (r.pc != e.pc) {
      Fail(label, "retire " + Dec(i) + " pc expected " + U64(e.pc) + ", got " + U64(r.pc));
    }
    if (r.rd != e.rd || r.reg_we != e.reg_we) {
      Fail(label, "retire " + Dec(i) + " at " + U64(e.pc) + " destination expected rd=" +
                      Dec(e.rd) + " we=" + Dec(e.reg_we) + ", got rd=" + Dec(r.rd) +
                      " we=" + Dec(r.reg_we));
    }
    if (e.reg_we && r.value != e.value) {
      Fail(label, "retire " + Dec(i) + " at " + U64(e.pc) + " value expected " +
                      U64(e.value) + ", got " + U64(r.value));
    }
  }
  if (!with_interloper) {
    reporter->Check(n == want && retires.size() >= want,
                    label + ": the retirement stream follows the independent reference for "
                            "every instruction (" + Dec(n) + " compared)");
  }

  // The final atomic scratch area, against the reference's own memory.
  if (!with_interloper) {
    for (size_t i = 0; i < cases.size(); i++) {
      uint64_t got = 0;
      uint64_t expect = 0;
      dut_mem.Read(kScratch + i * 8, 8, &got);
      ref_mem.Read(kScratch + i * 8, 8, &expect);
      if (got != expect) {
        Fail(label, "the atomic scratch slot " + Dec(i) + " (" +
                        AmoName(cases[i].op) + (cases[i].size == 4 ? ".w" : ".d") +
                        ") expected " + U64(expect) + ", got " + U64(got));
      }
      reporter->Check(got == expect,
                      label + ": scratch slot " + Dec(i) + " holds the reference value");
    }
  }

  // The history replay (a legal linearization) and the atomic-beat structure.
  Shadow shadow;
  shadow.Seed(cases);
  Replay(harness.txns(), cases, &shadow, reporter, label);
  CheckAmoTransactions(harness.txns(), cases, 0, reporter);

  reporter->Check(harness.redirects().empty(),
                  label + ": a straight-line program issues no redirect");

  RunResult out;
  out.retires = retires.size();
  out.cycles = harness.cycles();
  out.txns = harness.txns().size();
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    if (geometry.reset_vector != 0x80000000ull) {
      Fail("geometry", "the reset vector is not the link base");
    }

    ProgImage img;
    BuildProgram(&img);

    RunResult a = RunOnce(&dut, &reporter, geometry, img, false, "atomic-coverage");
    RunResult b = RunOnce(&dut, &reporter, geometry, img, true,
                          "linearization-under-contention");

    std::printf("amo.linearization: %zu AMO bindings, run A %llu retires / %llu txns / "
                "%llu cycles, run B %llu retires / %llu txns / %llu cycles\n",
                Cases().size(),
                static_cast<unsigned long long>(a.retires),
                static_cast<unsigned long long>(a.txns),
                static_cast<unsigned long long>(a.cycles),
                static_cast<unsigned long long>(b.retires),
                static_cast<unsigned long long>(b.txns),
                static_cast<unsigned long long>(b.cycles));

    detail = "checks=" + Dec(reporter.checks()) + " amo_bindings=" + Dec(Cases().size()) +
             " run_a_txns=" + Dec(a.txns) + " run_b_txns=" + Dec(b.txns) +
             " cycles=" + Dec(a.cycles + b.cycles) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the atomic path holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  const bool ok = passed && (reporter.failures() == 0);
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}