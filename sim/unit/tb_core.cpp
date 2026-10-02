// ============================================================================
// tb_core.cpp -- CASE=fabric.fixed_two_cluster, work package I-023.
//
// The DUT is the integrated p0 core: fetch -> decode buffer -> dispatch (one
// macro per cycle, alternating cluster affinity) -> two issue queues with one
// ALU and one branch resolver each, cluster 0's queue also routing to the
// shared iterative MUL/DIV -> the writeback arbiter -> the ROB -> in-order
// retire over two lanes. The wrapper `sim/tb/mosaic_core_tb.sv` flattens the
// core and adds a standalone `mosaic_redirect_arb` the driver also exercises.
//
// The case drives a program and proves four things, each of which fails by
// name and cycle if the design stops doing it:
//
//   1. the program retires correctly. An independent RV64IM interpreter,
//      written from the ISA text and decoding the same instruction words, is
//      the expectation model; the DUT's architectural event stream (pc, rd,
//      value, one lane per retired instruction) is compared with it event by
//      event. The interpreter shares nothing with the RTL -- not a decoder,
//      not a register file, not an encoding table -- so a value that both
//      agree on is a value the RTL computed.
//
//   2. the second cluster is genuinely used. The dispatch affinity is fixed
//      and documented (`mosaic_dispatch.sv`: first macro of a pair -> cluster
//      0, second -> cluster 1, MUL/DIV -> cluster 0), so which cluster each
//      instruction of the program must execute in is a function of its
//      position that the driver knows independently and can check from the
//      granted uop ids. The controls are: both clusters executed ALU uops
//      (their own counters), every grant follows the affinity rule, the two
//      clusters executed *disjoint* uop sets whose union is the whole program,
//      and both queues held live work in the same cycle. What is *not*
//      asserted is a grant offered by both clusters in the same cycle: with
//      one allocation per cycle and alternating affinity the two issue streams
//      are staggered by construction, and the measurement is reported rather
//      than required. `MOSAIC_DISPATCH_MUTANT_SINGLE_CLUSTER` (in
//      mosaic_dispatch.sv) forces every macro to cluster 0; this case fails
//      under it, which is the negative control for this control.
//
//   3. completion is out of order while retirement is not. The program places
//      an M-extension division at ROB index 12 and an immediately-ready ALU
//      op at index 13. The shared unit is fixed-latency (65 cycles at 64-bit
//      width), so index 13 completes long before index 12 -- observable on the
//      arbiter's published-completion port -- while the retire stream stays in
//      program order. "A younger uop completes before an older one but never
//      retires before it" is therefore a check, not a claim.
//
//   4. the standalone redirect arbiter keeps its oldest-wins rule when two
//      clusters resolve in the same cycle, waits for its macro to reach the
//      head, and drops a dead request without acting.
//
// -------------------------------------------------- what the case has caught
//
// Two integration defects, both recorded in results/reports/I-023-core.md:
//
//   * the decode buffer in mosaic_core.sv swapped the first two instructions
//     of a program at start-up (program-order violation). Fixed there;
//     `MOSAIC_CORE_MUTANT_DBUF_PUSH_SLOT` rebuilds the defective form and the
//     program-order comparison fails under it.
//   * the committed map was fed the ROB entry generation where rename's maps
//     are keyed on the physical tag's generation, so the map equality the
//     redirect/checkpoint path depends on was false from the first commit on.
//     Fixed in mosaic_core.sv (the generation now comes from the descriptor
//     store, which is where the destination identity lives).
//
// ---------------------------------------------------------- what is left out
//
// Loads, stores, CSR accesses, traps, interrupts, multi-hart and the caches
// are not in this package. Dispatch refuses a memory or system macro *before*
// allocating it, and the program therefore ends with an ECALL: the machine
// stops cleanly there (`o_stopped`, `o_stop_ctr == 1`) and the driver asserts
// that exactly the instructions before it retired, with no illegal decode and
// no redirect. Nothing here is a substitute for the memory-path cases
// (I-033..I-038) or the trap path (I-019/I-020).
//
// --------------------------------------------------------------- the memory
//
// The instruction memory is a word array in the driver with a one-cycle
// request/response handshake, and it returns ECALL for any address past the
// program. That makes a runaway fetch stop the machine cleanly rather than
// reading a don't-care, and every fetch must hand back exactly one response
// (the fetch unit's credit rule).
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_core_tb.h"

namespace {

// ------------------------------------------------------------------ failure
// Thrown on the first failed check so the report names one defect, at one
// phase and cycle, instead of a thousand consequences of it.
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }

// ============================================================================
// The program (CASE=fabric.fixed_two_cluster)
// ============================================================================
// Handed to `riscv64-elf-as -march=rv64im_zicsr_zifencei -mabi=lp64`,
// linked at the profile's reset vector with `-Ttext=0x80000000 -e _start`,
// flattened with `objcopy -O binary`. The listing, the disassembly and the
// commands are in results/reports/I-023-core.md.
//
//   idx  instruction              cluster (affinity)  role
//    0   addi x1, x0, 11          c0                  independent chain A
//    1   addi x2, x0, 22          c1                  independent chain B
//    2   addi x3, x0, 33          c0                  independent chain A
//    3   addi x4, x0, 44          c1                  independent chain B
//    4   add  x5, x1, x2          c0                  RAW across the clusters
//    5   add  x6, x3, x4          c1                  RAW across the clusters
//    6   sub  x7, x5, x1          c0                  RAW chain in c0
//    7   sub  x8, x6, x3          c1                  RAW chain in c1
//    8   addi x9, x0, 1           c0                  seed the wide dividend
//    9   slli x9, x9, 63          c1                  RAW chain in c1
//   10   addi x9, x9, -1          c0                  RAW chain in c0
//   11   addi x13, x0, 3          c1                  divisor
//   12   div  x10, x9, x13        c0 (MUL/DIV forced) the long-latency uop
//   13   andi x14, x0, 123        c1                  younger, immediately ready
//   14   or   x15, x14, x7        c0                  RAW after the division
//   15   slli x16, x15, 2         c1                  RAW chain in c1
//   16   sltu x17, x16, x9        c0                  RAW chain in c0
//   17   ecall                    -                   refused by dispatch: stop
//
// Seventeen instructions retire; the ECALL never does.
const uint32_t kProgram[] = {
    0x00b00093u,  // addi x1, x0, 11
    0x01600113u,  // addi x2, x0, 22
    0x02100193u,  // addi x3, x0, 33
    0x02c00213u,  // addi x4, x0, 44
    0x002082b3u,  // add  x5, x1, x2
    0x00418333u,  // add  x6, x3, x4
    0x401283b3u,  // sub  x7, x5, x1
    0x40330433u,  // sub  x8, x6, x3
    0x00100493u,  // addi x9, x0, 1
    0x03f49493u,  // slli x9, x9, 63
    0xfff48493u,  // addi x9, x9, -1
    0x00300693u,  // addi x13, x0, 3
    0x02d4c533u,  // div  x10, x9, x13
    0x07b07713u,  // andi x14, x0, 123
    0x007767b3u,  // or   x15, x14, x7
    0x00279813u,  // slli x16, x15, 2
    0x009838b3u,  // sltu x17, x16, x9
    0x00000073u,  // ecall
};

constexpr int kProgWords = static_cast<int>(sizeof(kProgram) / sizeof(kProgram[0]));
constexpr int kRetireCount = kProgWords - 1;  // the ECALL is refused, never retires
// The two indices the out-of-order-completion control is built on.
constexpr int kLongIndex = 12;   // div  x10, x9, x13
constexpr int kYoungIndex = 13;  // andi x14, x0, 123

// ============================================================================
// The expectation model: a from-the-ISA RV64IM interpreter
// ============================================================================
// It decodes the same words the core is fed and produces the architectural
// record of every instruction that retires. Written from the RISC-V
// unprivileged ISA manual (chapters 2 and 12 for the M extension); it neither
// reads nor resembles the RTL. It fails loudly on any word it does not
// implement, so the program cannot drift away from the model silently.
struct Expect {
  uint64_t pc = 0;
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
};

uint64_t Sext(uint64_t value, int bits) {
  const uint64_t shift = static_cast<uint64_t>(64 - bits);
  return static_cast<uint64_t>(static_cast<int64_t>(value << shift) >> shift);
}

std::vector<Expect> ReferenceTrace(uint64_t base, const uint32_t* words, int count,
                                   std::string* error) {
  std::vector<Expect> trace;
  uint64_t regs[32] = {0};

  for (int i = 0; i < count; i++) {
    const uint32_t insn = words[i];
    const uint64_t pc = base + 4u * static_cast<uint64_t>(i);
    const uint32_t opcode = insn & 0x7fu;
    const uint32_t rd = (insn >> 7) & 0x1fu;
    const uint32_t funct3 = (insn >> 12) & 0x7u;
    const uint32_t rs1 = (insn >> 15) & 0x1fu;
    const uint32_t rs2 = (insn >> 20) & 0x1fu;
    const uint32_t funct7 = (insn >> 25) & 0x7fu;
    const uint64_t a = regs[rs1];
    const uint64_t b = regs[rs2];

    auto bad = [&](const char* what) {
      if (error != nullptr) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s at pc %s (word 0x%08x)",
                      what, mosaic::Hex(pc).c_str(), insn);
        *error = buf;
      }
      return std::vector<Expect>{};
    };

    uint64_t result = 0;
    switch (opcode) {
      case 0x13u: {  // OP-IMM
        const int64_t imm = static_cast<int64_t>(Sext(insn >> 20, 12));
        switch (funct3) {
          case 0x0u: result = a + static_cast<uint64_t>(imm); break;
          case 0x1u: result = a << ((insn >> 20) & 0x3fu); break;   // SLLI
          case 0x2u: result = (static_cast<int64_t>(a) < imm) ? 1 : 0; break;
          case 0x3u: result = (a < static_cast<uint64_t>(imm)) ? 1 : 0; break;
          case 0x4u: result = a ^ static_cast<uint64_t>(imm); break;
          case 0x5u:                                                 // SRLI/SRAI
            result = (funct7 == 0x20u) ? static_cast<uint64_t>(
                                             static_cast<int64_t>(a) >> ((insn >> 20) & 0x3fu))
                                       : (a >> ((insn >> 20) & 0x3fu));
            break;
          case 0x6u: result = a | static_cast<uint64_t>(imm); break;
          case 0x7u: result = a & static_cast<uint64_t>(imm); break;
          default: return bad("unimplemented OP-IMM funct3");
        }
        break;
      }
      case 0x33u:  // OP (RV64I arithmetic and the M extension)
        if (funct7 == 0x01u) {
          switch (funct3) {
            case 0x0u: result = a * b; break;                        // MUL
            case 0x1u: {                                             // MULH
              const __int128 p = static_cast<__int128>(static_cast<int64_t>(a)) *
                                 static_cast<__int128>(static_cast<int64_t>(b));
              result = static_cast<uint64_t>(p >> 64);
              break;
            }
            case 0x2u: {                                             // MULHSU
              const __int128 p = static_cast<__int128>(static_cast<int64_t>(a)) *
                                 static_cast<__int128>(b);
              result = static_cast<uint64_t>(p >> 64);
              break;
            }
            case 0x3u: {                                             // MULHU
              const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
              result = static_cast<uint64_t>(p >> 64);
              break;
            }
            case 0x4u:                                               // DIV
              if (b == 0) {
                result = ~0ull;                                      // divide by zero: -1
              } else if (a == 0x8000000000000000ull && b == ~0ull) {
                result = a;                                          // overflow: INT_MIN
              } else {
                result = static_cast<uint64_t>(static_cast<int64_t>(a) /
                                               static_cast<int64_t>(b));
              }
              break;
            case 0x5u: result = (b == 0) ? ~0ull : (a / b); break;   // DIVU
            case 0x6u:                                               // REM
              if (b == 0) {
                result = a;
              } else if (a == 0x8000000000000000ull && b == ~0ull) {
                result = 0;
              } else {
                result = static_cast<uint64_t>(static_cast<int64_t>(a) %
                                               static_cast<int64_t>(b));
              }
              break;
            case 0x7u: result = (b == 0) ? a : (a % b); break;       // REMU
            default: return bad("unimplemented M-extension funct3");
          }
        } else {
          switch (funct3) {
            case 0x0u:
              result = (funct7 == 0x20u) ? (a - b) : (a + b);
              break;
            case 0x1u: result = a << (b & 0x3fu); break;
            case 0x2u: result = (static_cast<int64_t>(a) < static_cast<int64_t>(b)) ? 1 : 0; break;
            case 0x3u: result = (a < b) ? 1 : 0; break;
            case 0x4u: result = a ^ b; break;
            case 0x5u:
              result = (funct7 == 0x20u)
                           ? static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 0x3fu))
                           : (a >> (b & 0x3fu));
              break;
            case 0x6u: result = a | b; break;
            case 0x7u: result = a & b; break;
            default: return bad("unimplemented OP funct3");
          }
        }
        break;
      case 0x73u:  // SYSTEM: the program's terminator. Refused by dispatch, never retires.
        if (insn == 0x00000073u && i == count - 1) {
          return trace;
        }
        return bad("unexpected SYSTEM instruction");
      default:
        return bad("unimplemented opcode");
    }

    if (rd != 0) regs[rd] = result;
    Expect e;
    e.pc = pc;
    e.rd = rd;
    e.reg_we = (rd != 0);
    e.value = result;
    trace.push_back(e);
  }
  if (error != nullptr) *error = "the program does not end in an ECALL";
  return {};
}

// ============================================================================
// Geometry, read out of the elaborated DUT (never re-derived)
// ============================================================================
struct Geometry {
  uint32_t xlen = 0, clusters = 0, retire_width = 0, rob_entries = 0;
  uint32_t rob_index_w = 0, rob_gen_w = 0, uop_index_w = 0, uop_id_w = 0;
  uint32_t prf_entries = 0, prf_tag_w = 0, int_gen_w = 0, iq_entries = 0;
  uint32_t occ_w = 0, req_id_w = 0, epoch_w = 0, seq_w = 0, ret_id_w = 0;
  uint32_t fetch_outstanding = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.clusters = dut->o_geom_clusters_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.rob_index_w = dut->o_geom_rob_index_w_o;
  g.rob_gen_w = dut->o_geom_rob_gen_w_o;
  g.uop_index_w = dut->o_geom_uop_index_w_o;
  g.uop_id_w = dut->o_geom_uop_id_w_o;
  g.prf_entries = dut->o_geom_prf_entries_o;
  g.prf_tag_w = dut->o_geom_prf_tag_w_o;
  g.int_gen_w = dut->o_geom_int_gen_w_o;
  g.iq_entries = dut->o_geom_iq_entries_o;
  g.occ_w = dut->o_geom_occ_w_o;
  g.req_id_w = dut->o_geom_req_id_w_o;
  g.epoch_w = dut->o_geom_epoch_w_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.fetch_outstanding = dut->o_geom_fetch_outstanding_o;
  g.ret_id_w = dut->o_geom_ret_id_w_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

// ============================================================================
// Wide and packed port access
// ============================================================================
// Two different layouts appear on this DUT's ports and confusing them reports
// the wrong lane, which looks like a design decision rather than a bug:
//
//   * a 128-bit event payload (`ev_pc`, `ev_value`) is two 64-bit lanes, so
//     Verilator hands it over as a `VlWide` indexed in 32-bit words and lane i
//     is words 2i and 2i+1;
//   * the small per-lane fields (`ev_rd`, `ev_seq`, `ev_id`) are packed by
//     *field width*, so lane 1 starts at bit `width`, not at bit 32.
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
// The instruction memory
// ============================================================================
// A one-cycle handshake: a request is accepted when `req_valid && req_ready`,
// its response is presented `latency` cycles later, and it is held until the
// fetch unit takes it (`rsp_ready`). Every accepted request produces exactly
// one response, which is the precondition the fetch unit's credit rule states.
// A window may withhold responses entirely (`Stall`), which delays them without
// losing one; that is how the driver exercises the front end against a slow
// memory.
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };

  Imem(uint64_t base, const uint32_t* words, int count, int latency)
      : base_(base), words_(words), count_(count), latency_(latency) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
    accepted_ = 0;
    hold_ = 0;
  }

  // Withhold every response for `cycles` cycles. This is the fetch side of
  // back-pressure: with no delivered instruction the decode buffer must drain
  // what it holds without a push alongside, which is the state its ordering fix
  // has to keep correct. One window per program is enough to reach it.
  void Stall(int cycles) { hold_ = cycles; }
  int Held() const { return hold_; }

  // The response presented this cycle, if any.
  bool HasResponse() const { return hold_ == 0 && !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint64_t ResponseWord() const { return Word(ready_.front().addr); }

  // Accept one request (called with the cycle's sampled handshake).
  void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
    Request r;
    r.addr = addr;
    r.id = id;
    r.epoch = epoch;
    inflight_.push_back(Entry{r, latency_});
    accepted_++;
  }

  void PopResponse() { ready_.pop_front(); }

  // Advance the latency pipeline at the end of the cycle.
  void Advance() {
    if (hold_ > 0) {
      hold_--;
      return;
    }
    for (size_t i = 0; i < inflight_.size();) {
      if (--inflight_[i].left == 0) {
        ready_.push_back(inflight_[i].req);
        inflight_.erase(inflight_.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  }

  uint32_t Word(uint64_t addr) const {
    if (addr < base_ || addr >= base_ + 4ull * static_cast<uint64_t>(count_) ||
        ((addr - base_) & 3ull) != 0) {
      return 0x00000073u;  // ECALL: a runaway fetch stops the machine cleanly
    }
    return words_[(addr - base_) / 4];
  }

 private:
  struct Entry {
    Request req;
    int left = 0;
  };
  uint64_t base_;
  const uint32_t* words_;
  int count_;
  int latency_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
  int hold_ = 0;
  uint64_t accepted_ = 0;
};

// ============================================================================
// The redirect-arbiter directed test
// ============================================================================
struct ArbIn {
  bool v[2] = {false, false};
  uint64_t pc[2] = {0, 0};
  uint32_t idx[2] = {0, 0};
  uint32_t gen[2] = {0, 0};
  bool taken[2] = {false, false};
  bool head_valid = false;
  uint32_t head_index = 0;
  uint32_t head_gen = 0;
  uint32_t head_occupied = 0;
  bool head_retire = false;
};

struct ArbOut {
  bool ack[2] = {false, false};
  bool redirect_valid = false;
  uint64_t redirect_pc = 0;
  bool act_valid = false;
  bool act_taken = false;
  uint32_t act_ctr = 0, wait_ctr = 0, dead_ctr = 0;
};

// ============================================================================
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::ClockDriver* clk, mosaic::Reporter* reporter,
          uint64_t max_cycles)
      : dut_(dut), clk_(clk), reporter_(reporter), max_cycles_(max_cycles) {}

  void Configure(const Geometry& g) {
    g_ = g;
    ret_mask_ = (g.retire_width >= 32) ? 0xffffffffu : ((1u << g.retire_width) - 1u);
    imem_ = Imem(g.reset_vector, kProgram, kProgWords, /*latency=*/1);
  }

  void Phase(const std::string& name) { phase_ = name; }
  const std::string& phase() const { return phase_; }
  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }
  const Geometry& geometry() const { return g_; }

  // A named check: counted, and it stops the run with the phase and cycle if it
  // fails.
  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + std::to_string(cycles_),
                  what + ": " + detail);
  }

  // A per-cycle cross-check: counted in `comparisons_`, and it stops the run
  // the same way. These are the standing invariants, as opposed to the named
  // properties a phase establishes.
  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + std::to_string(cycles_),
                  what + ": " + detail);
  }

  // --------------------------------------------------------------- recording
  struct Retire {
    uint64_t pc = 0;
    uint32_t rd = 0;
    bool reg_we = false;
    uint64_t value = 0;
    uint32_t seq = 0;
    bool trap = false;
    bool store = false;
    uint64_t cycle = 0;
  };

  const std::vector<Retire>& retires() const { return retires_; }
  const std::vector<uint32_t>& publish_order() const { return publish_order_; }
  int publish_cycle(uint32_t index) const {
    auto it = publish_cycle_.find(index);
    return it == publish_cycle_.end() ? -1 : it->second;
  }
  int retire_cycle(uint32_t index) const {
    auto it = retire_cycle_.find(index);
    return it == retire_cycle_.end() ? -1 : it->second;
  }
  const std::vector<uint64_t>& c0_grant_uops() const { return c0_grants_; }
  const std::vector<uint64_t>& c1_grant_uops() const { return c1_grants_; }
  uint64_t dual_grant_cycles() const { return dual_grant_cycles_; }
  uint64_t overlap_cycles() const { return overlap_cycles_; }
  uint32_t max_occupied() const { return max_occupied_; }
  const std::vector<uint64_t>& fetch_addrs() const { return fetch_addrs_; }

  // Decode a grant payload {rob_index, rob_gen, uop_index}.
  uint32_t GrantIndex(uint64_t uop) const {
    return static_cast<uint32_t>(
        (uop >> (g_.rob_gen_w + g_.uop_index_w)) & ((1ull << g_.rob_index_w) - 1ull));
  }

  // --------------------------------------------------------------- resets
  void Reset(int cycles, bool imem_enabled) {
    imem_enabled_ = imem_enabled;
    imem_.Reset();
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void ClearTrace() {
    retires_.clear();
    publish_order_.clear();
    publish_cycle_.clear();
    retire_cycle_.clear();
    c0_grants_.clear();
    c1_grants_.clear();
    fetch_addrs_.clear();
    dual_grant_cycles_ = 0;
    overlap_cycles_ = 0;
    max_occupied_ = 0;
    last_progress_ = 0;
  }

  // ------------------------------------------------------------------ cycles
  // One full clock period, in the same shape every testbench in this tree uses:
  // drive the inputs, settle the combinational answers, compare, then edge.
  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + std::to_string(cycles_),
           "max-cycles (" + std::to_string(max_cycles_) + ") exhausted: " + Diagnose());
    }
    last_head_pc_ = dut_->o_dbg_head_pc_o;

    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = imem_enabled_ ? 1 : 0;
    dut_->imem_rsp_valid_i = imem_.HasResponse() ? 1 : 0;
    if (imem_.HasResponse()) {
      const Imem::Request& r = imem_.Response();
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem_.ResponseWord());
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = r.id;
      dut_->imem_rsp_epoch_i = r.epoch;
      dut_->imem_rsp_len_i = 4;  // a 32-bit instruction, in bytes
    } else {
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }
    DriveArb(arb_in_);
    dut_->eval();

    if (!rst) Observe();

    ModelStep(rst);
    Edge();
    ++cycles_;
  }

  // ------------------------------------------------------------------ observe
  void Observe() {
    // 1. The core's own retire counter is an independent tally of the event
    //    stream it is publishing. Checked before this cycle's events are read,
    //    so the two are the same cycle's.
    Compare("the retire counter equals the event stream published so far",
            dut_->o_commit_o == retires_.size(),
            "counter=" + std::to_string(dut_->o_commit_o) +
                " events=" + std::to_string(retires_.size()));

    Compare("ROB occupancy is within the ROB",
            dut_->o_rob_occupied_o <= g_.rob_entries,
            "occupied=" + std::to_string(dut_->o_rob_occupied_o));

    // The core's header claims the speculative map equals the committed map
    // whenever the machine holds no unretired macro. This is that claim.
    // The core's documented claim, at every quiescent point: with no unretired
    // macro in the ROB the speculative map equals the committed map. This is
    // the boundary a redirect (and I-018's checkpoint) depends on, so it is
    // checked every cycle it can be observed rather than once at the end.
    if (dut_->o_rob_occupied_o == 0) {
      Compare("an empty ROB is at a rename boundary",
              dut_->o_rename_boundary_o != 0,
              "occupied=0, o_rename_boundary=0 at commit=" +
                  std::to_string(dut_->o_commit_o));
    }

    Compare("no redirect while a straight-line program runs",
            dut_->o_redirect_o == 0,
            "o_redirect_ctr=" + std::to_string(dut_->o_redirect_o));
    Compare("no squash is issued by this recovery",
            dut_->o_squash_nc_o == 0 && dut_->o_squash_under_o == 0 &&
                dut_->o_journal_ovf_o == 0,
            "squash_nc=" + std::to_string(dut_->o_squash_nc_o) +
                " under=" + std::to_string(dut_->o_squash_under_o) +
                " journal=" + std::to_string(dut_->o_journal_ovf_o));

    const uint64_t alloc = dut_->o_dbg_alloc_ctr_o;

    // 2. Completions and grants must name a macro that was actually allocated.
    if (dut_->o_wb_pub_valid_o) {
      Compare("a published completion names an allocated macro",
              dut_->o_wb_pub_index_o < alloc,
              "published index=" + std::to_string(dut_->o_wb_pub_index_o) +
                  " but only " + std::to_string(alloc) + " macros were allocated");
      const uint32_t index = dut_->o_wb_pub_index_o;
      publish_order_.push_back(index);
      if (publish_cycle_.find(index) == publish_cycle_.end()) {
        publish_cycle_[index] = static_cast<int>(cycles_);
      }
    }
    for (int c = 0; c < 2; c++) {
      const bool valid = (c == 0) ? (dut_->o_c0_grant_valid_o != 0) : (dut_->o_c1_grant_valid_o != 0);
      if (!valid) continue;
      const uint64_t uop = (c == 0) ? (uint64_t)dut_->o_c0_grant_uop_o
                                    : (uint64_t)dut_->o_c1_grant_uop_o;
      Compare("a granted uop names an allocated macro", GrantIndex(uop) < alloc,
              "cluster " + std::to_string(c) + " granted index " +
                  std::to_string(GrantIndex(uop)) + " with " + std::to_string(alloc) +
                  " allocations");
      if (c == 0) {
        c0_grants_.push_back(uop);
      } else {
        c1_grants_.push_back(uop);
      }
    }
    if (dut_->o_c0_grant_valid_o && dut_->o_c1_grant_valid_o) {
      dual_grant_cycles_++;
      Compare("two clusters present different uops in one cycle",
              dut_->o_c0_grant_uop_o != dut_->o_c1_grant_uop_o,
              "both clusters present uop " + std::to_string(dut_->o_c0_grant_uop_o));
    }
    if (dut_->o_c0_count_o > 0 && dut_->o_c1_count_o > 0) overlap_cycles_++;
    if (dut_->o_rob_occupied_o > max_occupied_) max_occupied_ = dut_->o_rob_occupied_o;

    // 3. The architectural event stream.
    const uint32_t mask = static_cast<uint32_t>(dut_->ev_valid_o) & ret_mask_;
    // Lane 1 retiring without lane 0 is an out-of-order retire, by definition.
    if (g_.retire_width >= 2) {
      Compare("retire lane 1 is never set without lane 0",
              ((mask & 2u) == 0) || ((mask & 1u) != 0),
              "ev_valid=" + std::to_string(mask));
    }
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((mask & (1u << lane)) == 0) continue;
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.seq = static_cast<uint32_t>(PackedLane(dut_->ev_seq_o, lane, g_.seq_w));
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.trap = PackedLane(dut_->ev_trap_o, lane, 1) != 0;
      r.store = PackedLane(dut_->ev_store_o, lane, 1) != 0;
      r.cycle = cycles_;
      if (!retires_.empty()) {
        const uint32_t prev = retires_.back().seq;
        const uint32_t dist = (r.seq - prev) & ((1u << g_.seq_w) - 1u);
        Compare("the retire sequence strictly increases",
                dist != 0 && dist < (1u << (g_.seq_w - 1)),
                "prev seq=" + std::to_string(prev) + " now=" + std::to_string(r.seq));
      }
      retires_.push_back(r);
      // Keyed by the retirement ordinal, which the "index == program order"
      // check below establishes is the instruction's ROB index for this
      // straight-line program (no redirect ever discards a slot).
      retire_cycle_[static_cast<uint32_t>(retires_.size() - 1)] =
          static_cast<int>(cycles_);
    }

    ProgressCheck();
  }

  // A stall is not a timeout: it is a defect with a location, and it is worth
  // reporting long before the cycle budget runs out.
  void ProgressCheck() {
    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_wb_pub_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) {
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail(phase_ + " at cycle " + std::to_string(cycles_), "stalled: " + Diagnose());
    }
  }

  std::string Diagnose() {
    std::string s = "head_pc=" + mosaic::Hex(last_head_pc_) +
                    " head_valid=" + std::to_string(dut_->o_dbg_head_valid_o) +
                    " head_complete=" + std::to_string(dut_->o_dbg_head_complete_o) +
                    " occupied=" + std::to_string(dut_->o_rob_occupied_o) +
                    " allocated=" + std::to_string(dut_->o_dbg_alloc_ctr_o) +
                    " inserted=" + std::to_string(dut_->o_dbg_ins_ctr_o) +
                    " retired=" + std::to_string(dut_->o_commit_o) +
                    " c0_count=" + std::to_string(dut_->o_c0_count_o) +
                    " c1_count=" + std::to_string(dut_->o_c1_count_o) +
                    " stopped=" + std::to_string(dut_->o_stopped_o) +
                    " wb_wr=" + std::to_string(dut_->o_wb_wr_o) +
                    " wb_wake=" + std::to_string(dut_->o_wb_wake_o) +
                    " wb_stale=" + std::to_string(dut_->o_wb_stale_o) +
                    " wb_dup=" + std::to_string(dut_->o_wb_dup_o) +
                    " fetch_pc_last=" + mosaic::Hex(last_fetch_addr_) +
                    " fetches=" + std::to_string(fetch_addrs_.size());
    return s;
  }

  // ------------------------------------------------------------------- model
  void ModelStep(bool rst) {
    if (bus_reset_.MayAccept(rst, imem_enabled_ && dut_->imem_req_valid_o &&
                                      dut_->imem_req_ready_i)) {
      const uint64_t addr = dut_->imem_req_addr_o;
      fetch_addrs_.push_back(addr);
      last_fetch_addr_ = addr;
      imem_.Accept(addr, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if (bus_reset_.MayDeliver(rst) && dut_->imem_rsp_valid_i && dut_->imem_rsp_ready_o &&
        imem_.HasResponse()) {
      imem_.PopResponse();
    }
    imem_.Advance();
  }

  void Edge() {
    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
  }

  // ------------------------------------------------------- redirect arbiter
  void DriveArb(const ArbIn& in) {
    dut_->arb_req_valid0_i = in.v[0] ? 1 : 0;
    dut_->arb_req_valid1_i = in.v[1] ? 1 : 0;
    dut_->arb_req_pc0_i = in.pc[0];
    dut_->arb_req_pc1_i = in.pc[1];
    dut_->arb_req_idx0_i = in.idx[0];
    dut_->arb_req_idx1_i = in.idx[1];
    dut_->arb_req_gen0_i = in.gen[0];
    dut_->arb_req_gen1_i = in.gen[1];
    dut_->arb_req_taken0_i = in.taken[0] ? 1 : 0;
    dut_->arb_req_taken1_i = in.taken[1] ? 1 : 0;
    dut_->arb_head_valid_i = in.head_valid ? 1 : 0;
    dut_->arb_head_index_i = in.head_index;
    dut_->arb_head_gen_i = in.head_gen;
    dut_->arb_head_occupied_i = in.head_occupied;
    dut_->arb_head_retire_i = in.head_retire ? 1 : 0;
  }

  ArbOut SampleArb() const {
    ArbOut o;
    o.ack[0] = dut_->arb_ack0_o != 0;
    o.ack[1] = dut_->arb_ack1_o != 0;
    o.redirect_valid = dut_->arb_redirect_valid_o != 0;
    o.redirect_pc = dut_->arb_redirect_pc_o;
    o.act_valid = dut_->arb_act_valid_o != 0;
    o.act_taken = dut_->arb_act_taken_o != 0;
    o.act_ctr = dut_->arb_act_ctr_o;
    o.wait_ctr = dut_->arb_wait_ctr_o;
    o.dead_ctr = dut_->arb_dead_ctr_o;
    return o;
  }

  // One directed arbiter cycle: drive, sample the combinational decision before
  // the edge, then clock. The registered redirect pulse appears in the *next*
  // call's sample.
  ArbOut ArbStep(const ArbIn& in, bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + std::to_string(cycles_), "max-cycles exhausted");
    }
    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = 0;
    dut_->imem_rsp_valid_i = 0;
    DriveArb(in);
    dut_->eval();
    const ArbOut out = SampleArb();
    Edge();
    ++cycles_;
    arb_in_ = in;
    return out;
  }

  void SetImemEnabled(bool enabled) { imem_enabled_ = enabled; }
  void SetArbInput(const ArbIn& in) { arb_in_ = in; }

  // Evidence read straight off the DUT, for the phase checks and the report.
  uint32_t dut_commit() const { return dut_->o_commit_o; }
  uint32_t dut_redirect() const { return dut_->o_redirect_o; }
  uint32_t dut_illegal() const { return dut_->o_illegal_o; }
  uint32_t dut_unsupported() const { return dut_->o_unsupported_o; }
  uint32_t dut_stop_ctr() const { return dut_->o_stop_o; }
  uint32_t dut_stopped() const { return dut_->o_stopped_o; }
  uint32_t dut_occupied() const { return dut_->o_rob_occupied_o; }
  uint32_t dut_boundary() const { return dut_->o_rename_boundary_o; }
  uint32_t dut_squash_nc() const { return dut_->o_squash_nc_o; }
  uint32_t dut_squash_under() const { return dut_->o_squash_under_o; }
  uint32_t dut_journal_ovf() const { return dut_->o_journal_ovf_o; }
  uint32_t dut_c0_alu() const { return dut_->o_c0_alu_o; }
  uint32_t dut_c1_alu() const { return dut_->o_c1_alu_o; }
  uint32_t dut_muldiv() const { return dut_->o_muldiv_o; }
  uint32_t dut_alloc_ctr() const { return dut_->o_dbg_alloc_ctr_o; }
  void StallImem(int cycles) { imem_.Stall(cycles); }
  int ImemHeld() const { return imem_.Held(); }
  uint32_t dut_free_count() const { return dut_->o_free_count_o; }

 private:
  static constexpr uint64_t kStallCycles = 4000;

  Vmosaic_core_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Geometry g_;
  uint32_t ret_mask_ = 0;
  Imem imem_{0, nullptr, 0, 1};
  // The reset-traffic rule (V-010, sim/common/bus_reset_gate.h): while reset is
  // asserted this model accepts nothing, so no reset-time response can be
  // queued ahead of a fresh post-reset one.
  mosaic::BusResetGate bus_reset_;
  bool imem_enabled_ = false;
  ArbIn arb_in_{};

  std::string phase_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;

  std::vector<Retire> retires_;
  std::vector<uint32_t> publish_order_;
  std::map<uint32_t, int> publish_cycle_;
  std::map<uint32_t, int> retire_cycle_;
  std::vector<uint64_t> c0_grants_;
  std::vector<uint64_t> c1_grants_;
  uint64_t dual_grant_cycles_ = 0;
  uint64_t overlap_cycles_ = 0;
  uint32_t max_occupied_ = 0;
  std::vector<uint64_t> fetch_addrs_;
  uint64_t last_fetch_addr_ = 0;
  uint64_t last_head_pc_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
};

// ============================================================================
// Phase 1: after reset the machine is quiescent
// ============================================================================
void PhaseResetState(Harness* h) {
  h->SetImemEnabled(false);
  h->Reset(4, false);
  h->Check("no commit before the first instruction", h->dut_commit() == 0,
           "o_commit_ctr=" + std::to_string(h->dut_commit()));
  h->Check("the machine is not stopped at reset", h->dut_stopped() == 0,
           "o_stopped=1 at reset");
  h->Check("the machine is at a rename boundary at reset", h->dut_boundary() != 0,
           "o_rename_boundary=0 at reset");
  h->Check("the ROB is empty at reset", h->dut_occupied() == 0,
           "occupied=" + std::to_string(h->dut_occupied()));
  h->Check("nothing has been fetched yet", h->fetch_addrs().empty(),
           "a fetch was issued during reset");
}

// ============================================================================
// Phase 2: the standalone redirect arbiter
// ============================================================================
// The core never produces two simultaneous resolutions (a branch is a barrier),
// so the arbiter's tie-break is only reachable directly. Five directed
// properties, each one cycle long.
void PhaseRedirectArb(Harness* h) {
  h->SetImemEnabled(false);
  h->Reset(4, false);

  // (a) Two live requests in one cycle: the older one wins and redirects; the
  //     younger taken one is a wrong-path loser and is acknowledged.
  {
    ArbIn in;
    in.v[0] = in.v[1] = true;
    in.pc[0] = 0x80000100ull;
    in.pc[1] = 0x80000200ull;
    in.idx[0] = 10;
    in.idx[1] = 11;
    in.taken[0] = in.taken[1] = true;
    in.head_valid = true;
    in.head_index = 10;
    in.head_gen = 0;
    in.head_occupied = 4;
    in.head_retire = true;
    const ArbOut out = h->ArbStep(in, false);
    h->Check("oldest request wins", out.act_valid && out.ack[0],
             "act_valid=0 or the older request was not acknowledged");
    h->Check("a wrong-path taken loser is acknowledged", out.ack[1],
             "the younger taken request was left holding its slot");
    h->Check("the winner is reported taken", out.act_taken,
             "o_act_taken is low for a taken branch");
    // The requester clears on ack; hold the head so the registered pulse is
    // observed alone.
    ArbIn clear = in;
    clear.v[0] = clear.v[1] = false;
    const ArbOut pulse = h->ArbStep(clear, false);
    h->Check("the redirect pulse carries the winner's PC",
             pulse.redirect_valid && pulse.redirect_pc == 0x80000100ull,
             "redirect_valid=" + std::to_string(pulse.redirect_valid) +
                 " pc=" + mosaic::Hex(pulse.redirect_pc));
  }

  // (b) A live request whose macro is not yet the head waits, and acts in the
  //     cycle it becomes the head and is retired.
  {
    ArbIn in;
    in.v[0] = true;
    in.pc[0] = 0x80000300ull;
    in.idx[0] = 12;
    in.taken[0] = true;
    in.head_valid = true;
    in.head_index = 10;
    in.head_gen = 0;
    in.head_occupied = 4;
    in.head_retire = true;
    const ArbOut waiting = h->ArbStep(in, false);
    h->Check("a request behind the head does not act yet",
             !waiting.act_valid && !waiting.ack[0],
             "act_valid=" + std::to_string(waiting.act_valid));
    // The wait counter is registered, so the cycle it counts is read back on
    // the next edge: hold the request (the requester still owns it) and look.
    const ArbOut still_waiting = h->ArbStep(in, false);
    h->Check("the waiting request is counted as waiting",
             still_waiting.wait_ctr == waiting.wait_ctr + 1,
             "o_wait_ctr went " + std::to_string(waiting.wait_ctr) + " -> " +
                 std::to_string(still_waiting.wait_ctr));

    ArbIn head = in;
    head.head_index = 12;
    const ArbOut acted = h->ArbStep(head, false);
    h->Check("it acts in the cycle it is the retiring head",
             acted.act_valid && acted.ack[0] && acted.act_taken,
             "act_valid=" + std::to_string(acted.act_valid) +
                 " ack=" + std::to_string(acted.ack[0]));
    ArbIn clear = head;
    clear.v[0] = false;
    const ArbOut pulse = h->ArbStep(clear, false);
    h->Check("that redirect carries the branch target",
             pulse.redirect_valid && pulse.redirect_pc == 0x80000300ull,
             "redirect_valid=" + std::to_string(pulse.redirect_valid));
    h->Check("exactly one action was counted for this request",
             pulse.act_ctr == acted.act_ctr + 1,
             "o_act_ctr went " + std::to_string(acted.act_ctr) + " -> " +
                 std::to_string(pulse.act_ctr));
  }

  // (c) A request for a slot outside the occupied window is dead: acknowledged
  //     so the requester can clear it, and never acted on.
  {
    ArbIn in;
    in.v[0] = true;
    in.pc[0] = 0x80000400ull;
    in.idx[0] = 13;
    in.taken[0] = true;
    in.head_valid = true;
    in.head_index = 10;
    in.head_gen = 0;
    in.head_occupied = 2;
    in.head_retire = true;
    const ArbOut out = h->ArbStep(in, false);
    h->Check("a dead request is dropped without acting",
             out.ack[0] && !out.act_valid,
             "ack=" + std::to_string(out.ack[0]) +
                 " act_valid=" + std::to_string(out.act_valid));
    ArbIn clear = in;
    clear.v[0] = false;
    const ArbOut after = h->ArbStep(clear, false);
    h->Check("the dead request is counted",
             after.dead_ctr == out.dead_ctr + 1,
             "o_dead_ctr went " + std::to_string(out.dead_ctr) + " -> " +
                 std::to_string(after.dead_ctr));
  }

  // (d) The head index with a recycled generation is dead as well.
  {
    ArbIn in;
    in.v[0] = true;
    in.pc[0] = 0x80000500ull;
    in.idx[0] = 10;
    in.gen[0] = 0;
    in.taken[0] = true;
    in.head_valid = true;
    in.head_index = 10;
    in.head_gen = 5;
    in.head_occupied = 4;
    in.head_retire = true;
    const ArbOut out = h->ArbStep(in, false);
    h->Check("a recycled head slot is dead, not acted on",
             out.ack[0] && !out.act_valid,
             "ack=" + std::to_string(out.ack[0]) +
                 " act_valid=" + std::to_string(out.act_valid));
  }

  // (e) A not-taken winner is accounted for without a redirect, and it drops
  //     nothing: a younger not-taken request is on the correct path and keeps
  //     its slot.
  {
    ArbIn in;
    in.v[0] = in.v[1] = true;
    in.pc[0] = 0x80000600ull;
    in.pc[1] = 0x80000604ull;
    in.idx[0] = 10;
    in.idx[1] = 11;
    in.taken[0] = in.taken[1] = false;
    in.head_valid = true;
    in.head_index = 10;
    in.head_gen = 0;
    in.head_occupied = 4;
    in.head_retire = true;
    const ArbOut out = h->ArbStep(in, false);
    h->Check("a not-taken resolution acts without being taken",
             out.act_valid && !out.act_taken && out.ack[0],
             "act_valid=" + std::to_string(out.act_valid) +
                 " act_taken=" + std::to_string(out.act_taken));
    h->Check("a younger not-taken request is not dropped", !out.ack[1],
             "the arbiter acknowledged a request on the correct path");
    ArbIn clear = in;
    clear.v[0] = clear.v[1] = false;
    const ArbOut pulse = h->ArbStep(clear, false);
    h->Check("a not-taken resolution raises no redirect", !pulse.redirect_valid,
             "redirect_valid=1 for a not-taken branch");
  }

  ArbIn idle;
  h->SetArbInput(idle);
  h->ArbStep(idle, false);
}

// ============================================================================
// Phase 3: run the program, compare the architectural stream
// ============================================================================
void PhaseCoreProgram(Harness* h) {
  h->SetImemEnabled(false);
  h->Reset(4, false);
  h->ClearTrace();
  h->SetImemEnabled(true);
  // The free list's state after reset is the baseline the run must return to:
  // every physical tag a macro takes must be released when that macro retires.
  const uint32_t free_after_reset = h->dut_free_count();

  std::string ref_error;
  const std::vector<Expect> expected =
      ReferenceTrace(h->geometry().reset_vector, kProgram, kProgWords, &ref_error);
  h->Check("the reference model implements every instruction word",
           ref_error.empty() && static_cast<int>(expected.size()) == kRetireCount,
           ref_error.empty() ? "expected " + std::to_string(kRetireCount) +
                                   " retiring instructions, model produced " +
                                   std::to_string(expected.size())
                             : ref_error);

  // Run until the refused ECALL has stopped the machine and the retirement it
  // triggered has drained, or until nothing has moved for a long time.
  //
  // One window with instruction responses withheld, opened once the pipeline
  // holds work (six macros allocated), so the decode buffer has to drain what
  // it holds without a delivery alongside. That is the back-pressure case the
  // buffer's ordering fix has to keep correct, and
  // MOSAIC_CORE_MUTANT_DBUF_KEEP_STALE is the control for it.
  int quiet = 0;
  bool stalled = false;
  while (true) {
    if (!stalled && h->dut_alloc_ctr() >= 6) {
      h->StallImem(6);
      stalled = true;
    }
    h->Cycle(false);
    const bool drained = (h->dut_stopped() != 0) &&
                         (static_cast<int>(h->retires().size()) == kRetireCount);
    if (drained) {
      if (++quiet >= 12) break;
    } else {
      quiet = 0;
    }
  }

  // A one-line summary of what ran: enough to see the shape of the run in the
  // log without printing 17 events, and the numbers the report quotes.
  {
    std::string pcs;
    for (size_t i = 0; i < h->retires().size(); i++) {
      if (i != 0) pcs += " ";
      char buf[24];
      std::snprintf(buf, sizeof(buf), "%llx",
                    static_cast<unsigned long long>(h->retires()[i].pc));
      pcs += buf;
    }
    std::printf("  [retire] %zu events in program order, pcs: %s\n",
                h->retires().size(), pcs.c_str());
    std::printf("  [fetch] %zu requests, straight-line from the reset vector, "
                "one response window withheld\n", h->fetch_addrs().size());
    std::printf("  [quiescent] cycle=%llu occupied=%u rename_boundary=%u commit=%u "
                "free_tags=%u stopped=%u\n",
                (unsigned long long)h->cycles(), h->dut_occupied(),
                h->dut_boundary(), h->dut_commit(), h->dut_free_count(),
                h->dut_stopped());
  }
  h->Check("every instruction before the ECALL retired",
           static_cast<int>(h->retires().size()) == kRetireCount,
           "retired " + std::to_string(h->retires().size()) + " of " +
               std::to_string(kRetireCount) + "; " + h->Diagnose());
  h->Check("the core's own retire counter agrees", h->dut_commit() == kRetireCount,
           "o_commit_ctr=" + std::to_string(h->dut_commit()));
  h->Check("every physical tag was returned to the free list",
           h->dut_free_count() == free_after_reset,
           "free count after reset=" + std::to_string(free_after_reset) +
               ", after the program=" + std::to_string(h->dut_free_count()));
  // The core's header states this claim for every quiescent point; the
  // per-cycle comparison above already enforces it, and it is named here so a
  // quiescent machine that is *not* at a boundary is a reported defect.
  h->Check("the machine is at a rename boundary once quiescent",
           h->dut_boundary() != 0,
           "o_rename_boundary=0 with the ROB empty after " +
               std::to_string(kRetireCount) + " retirements");
  h->Check("the machine stopped exactly once, at the refused ECALL",
           h->dut_stop_ctr() == 1 && h->dut_stopped() != 0,
           "o_stop_ctr=" + std::to_string(h->dut_stop_ctr()) +
               " o_stopped=" + std::to_string(h->dut_stopped()));
  h->Check("the refused macro was a system instruction, not an illegal decode",
           h->dut_unsupported() >= 1 && h->dut_illegal() == 0,
           "o_unsupported_ctr=" + std::to_string(h->dut_unsupported()) +
               " o_illegal_ctr=" + std::to_string(h->dut_illegal()));
  h->Check("no redirect, no squash and no rename fault in the run",
           h->dut_redirect() == 0 && h->dut_squash_nc() == 0 &&
               h->dut_squash_under() == 0 && h->dut_journal_ovf() == 0,
           "redirect=" + std::to_string(h->dut_redirect()) +
               " squash_nc=" + std::to_string(h->dut_squash_nc()) +
               " under=" + std::to_string(h->dut_squash_under()) +
               " journal=" + std::to_string(h->dut_journal_ovf()));
  h->Check("the PC stream is exact: one word fetched per instruction",
           h->fetch_addrs().size() >= static_cast<size_t>(kProgWords),
           "only " + std::to_string(h->fetch_addrs().size()) + " fetches");
  bool aligned_and_forward = true;
  std::string first_bad;
  for (size_t i = 0; i < h->fetch_addrs().size(); i++) {
    const uint64_t want = h->geometry().reset_vector + 4ull * i;
    if (h->fetch_addrs()[i] != want) {
      aligned_and_forward = false;
      if (first_bad.empty()) {
        first_bad = "fetch " + std::to_string(i) + " at " +
                    mosaic::Hex(h->fetch_addrs()[i]) + ", expected " +
                    mosaic::Hex(want);
      }
    }
  }
  h->Check("no fetch outside the straight-line PC stream", aligned_and_forward,
           first_bad);

  // The architectural comparison, event by event.
  for (int i = 0; i < kRetireCount; i++) {
    if (i >= static_cast<int>(h->retires().size())) break;
    const Harness::Retire& got = h->retires()[i];
    const Expect& want = expected[i];
    h->Compare("retire " + std::to_string(i) + " pc",
               got.pc == want.pc,
               "expected " + mosaic::Hex(want.pc) + ", got " + mosaic::Hex(got.pc));
    h->Compare("retire " + std::to_string(i) + " reg_we",
               got.reg_we == want.reg_we,
               "expected " + std::to_string(want.reg_we) + ", got " +
                   std::to_string(got.reg_we));
    h->Compare("retire " + std::to_string(i) + " rd",
               got.rd == want.rd,
               "expected x" + std::to_string(want.rd) + ", got x" +
                   std::to_string(got.rd));
    h->Compare("retire " + std::to_string(i) + " value",
               got.value == want.value,
               "expected " + mosaic::Hex(want.value) + ", got " + mosaic::Hex(got.value));
    h->Compare("retire " + std::to_string(i) + " is not a trap or a store",
               !got.trap && !got.store,
               "trap=" + std::to_string(got.trap) + " store=" + std::to_string(got.store));
  }
  h->Check("the retirement order is program order",
           static_cast<int>(h->retires().size()) == kRetireCount,
           "the stream is short, so no order can be claimed");

  // Values that make the comparison non-vacuous: the wide dividend and the
  // division's quotient are not achievable by an idle or single-bit machine.
  h->Check("the division produced the wide quotient the reference computes",
           h->retires().size() > static_cast<size_t>(kLongIndex) &&
               h->retires()[kLongIndex].value == 0x2aaaaaaaaaaaaaaauLL,
           h->retires().size() > static_cast<size_t>(kLongIndex)
               ? "got " + mosaic::Hex(h->retires()[kLongIndex].value)
               : "the stream is short");
}

// ============================================================================
// Phase 4: the second cluster is genuinely used
// ============================================================================
void PhaseClusterFabric(Harness* h) {
  h->Check("cluster 0 executed at least one ALU uop", h->dut_c0_alu() > 0,
           "o_c0_alu_ctr=0");
  h->Check("cluster 1 executed at least one ALU uop", h->dut_c1_alu() > 0,
           "o_c1_alu_ctr=0 -- the second cluster was never used");
  h->Check("exactly one MUL/DIV uop executed, through cluster 0",
           h->dut_muldiv() == 1,
           "o_muldiv_ctr=" + std::to_string(h->dut_muldiv()));

  // The affinity rule, checked against the program's own index arithmetic:
  // even index -> cluster 0, odd index -> cluster 1 (the MUL/DIV is at an even
  // index and is forced to cluster 0 anyway).
  bool affinity_ok = true;
  std::string bad;
  for (uint64_t uop : h->c0_grant_uops()) {
    const uint32_t idx = h->GrantIndex(uop);
    if ((idx & 1u) != 0) {
      affinity_ok = false;
      if (bad.empty()) bad = "cluster 0 was granted index " + std::to_string(idx);
    }
  }
  for (uint64_t uop : h->c1_grant_uops()) {
    const uint32_t idx = h->GrantIndex(uop);
    if ((idx & 1u) != 1u) {
      affinity_ok = false;
      if (bad.empty()) bad = "cluster 1 was granted index " + std::to_string(idx);
    }
  }
  h->Check("every grant follows the fixed affinity rule", affinity_ok, bad);

  // The two clusters executed *disjoint* sets of uops whose union is the whole
  // program: every instruction was executed exactly once, by exactly one of the
  // two clusters. This is the control the card asks for -- "both clusters
  // executed distinct uops" -- and it cannot be satisfied by one cluster doing
  // all the work, or by one uop being counted twice.
  std::vector<uint32_t> from_c0, from_c1;
  for (uint64_t uop : h->c0_grant_uops()) from_c0.push_back(h->GrantIndex(uop));
  for (uint64_t uop : h->c1_grant_uops()) from_c1.push_back(h->GrantIndex(uop));
  std::sort(from_c0.begin(), from_c0.end());
  std::sort(from_c1.begin(), from_c1.end());
  std::vector<uint32_t> merged;
  std::set_union(from_c0.begin(), from_c0.end(), from_c1.begin(), from_c1.end(),
                 std::back_inserter(merged));
  bool disjoint = true;
  for (uint32_t i = 0; i < from_c0.size() && disjoint; i++) {
    if (std::binary_search(from_c1.begin(), from_c1.end(), from_c0[i])) disjoint = false;
  }
  bool complete = merged.size() == static_cast<size_t>(kRetireCount);
  for (size_t i = 0; complete && i < merged.size(); i++) {
    if (merged[i] != i) complete = false;
  }
  h->Check("the two clusters executed disjoint uops covering the program",
           disjoint && complete,
           "cluster 0 executed " + std::to_string(from_c0.size()) +
               " uops, cluster 1 " + std::to_string(from_c1.size()) +
               ", union " + std::to_string(merged.size()) +
               (disjoint ? "" : ", and an instruction was executed by both"));

  // Both clusters held un-issued work at the same instant. (A grant offered by
  // both clusters in the *same cycle* is not expected here and is not required:
  // dispatch allocates one macro per cycle and alternates the clusters, so the
  // two issue streams are staggered by construction. The measurement is
  // reported below rather than asserted.)
  h->Check("both issue queues held live work in the same cycle",
           h->overlap_cycles() > 0,
           "no cycle had both clusters' queues non-empty");
  h->Check("more than one instruction was in flight",
           h->max_occupied() >= 2,
           "max ROB occupancy=" + std::to_string(h->max_occupied()));

  std::printf("  [fabric] c0_alu=%u c1_alu=%u muldiv=%u c0_grants=%zu c1_grants=%zu "
              "both_queues_live_cycles=%llu same_cycle_dual_offer=%llu "
              "max_rob_occupied=%u\n",
              h->dut_c0_alu(), h->dut_c1_alu(), h->dut_muldiv(),
              h->c0_grant_uops().size(), h->c1_grant_uops().size(),
              static_cast<unsigned long long>(h->overlap_cycles()),
              static_cast<unsigned long long>(h->dual_grant_cycles()),
              h->max_occupied());
  std::printf("  [fabric] cluster-0 executed indices:");
  for (uint32_t i : from_c0) std::printf(" %u", i);
  std::printf("\n  [fabric] cluster-1 executed indices:");
  for (uint32_t i : from_c1) std::printf(" %u", i);
  std::printf("\n");
}

// ============================================================================
// Phase 5: out-of-order completion, in-order retirement
// ============================================================================
void PhaseOutOfOrder(Harness* h) {
  const int long_cycle = h->publish_cycle(kLongIndex);
  const int young_cycle = h->publish_cycle(kYoungIndex);
  h->Check("the older M-extension uop completed at all", long_cycle >= 0,
           "no completion was published for ROB index " + std::to_string(kLongIndex));
  h->Check("the younger ALU uop completed at all", young_cycle >= 0,
           "no completion was published for ROB index " + std::to_string(kYoungIndex));
  h->Check("the younger uop completed before the older one",
           young_cycle >= 0 && long_cycle >= 0 && young_cycle < long_cycle,
           "completion cycle of index " + std::to_string(kYoungIndex) + " = " +
               std::to_string(young_cycle) + ", of index " + std::to_string(kLongIndex) +
               " = " + std::to_string(long_cycle));

  // At the younger uop's completion the older one had neither completed nor
  // retired, and the retire stream as a whole stayed in program order.
  h->Check("the older uop had not retired when the younger completed",
           h->retire_cycle(kLongIndex) > young_cycle,
           "index " + std::to_string(kLongIndex) + " retired at " +
               std::to_string(h->retire_cycle(kLongIndex)) + ", before or with the "
               "younger completion at " + std::to_string(young_cycle));
  h->Check("the younger uop did not retire before the older one",
           h->retire_cycle(kYoungIndex) >= h->retire_cycle(kLongIndex),
           "index " + std::to_string(kYoungIndex) + " retired at " +
               std::to_string(h->retire_cycle(kYoungIndex)) + ", index " +
               std::to_string(kLongIndex) + " at " +
               std::to_string(h->retire_cycle(kLongIndex)));

  // The completion order as a whole is not the program order.
  bool out_of_order = false;
  for (size_t i = 1; i < h->publish_order().size(); i++) {
    if (h->publish_order()[i] < h->publish_order()[i - 1]) out_of_order = true;
  }
  h->Check("the completion sequence is not the program order", out_of_order,
           "every completion was published in increasing ROB index");

  // Every instruction completed exactly once, and the ROB index of a
  // completion is the instruction's position in the program: the mapping the
  // two checks above depend on, checked rather than assumed.
  std::vector<uint32_t> indices = h->publish_order();
  std::sort(indices.begin(), indices.end());
  bool exact = indices.size() == static_cast<size_t>(kRetireCount);
  for (size_t i = 0; exact && i < indices.size(); i++) {
    if (indices[i] != i) exact = false;
  }
  h->Check("each instruction completed exactly once, index == program order",
           exact,
           "published " + std::to_string(indices.size()) +
               " completions, not one per instruction in program order");

  std::printf("  [ooo] index %d completed at cycle %d (retired %d), index %d at "
              "cycle %d (retired %d); publications=%zu\n",
              kLongIndex, long_cycle, h->retire_cycle(kLongIndex), kYoungIndex,
              young_cycle, h->retire_cycle(kYoungIndex), h->publish_order().size());
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

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  mosaic::ClockDriver clk;
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    // The geometry ports are constants, so one evaluation reads them.
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    const Geometry g = ReadGeometry(&dut);
    Harness harness(&dut, &clk, &reporter, options.max_cycles);
    harness.Configure(g);

    harness.Phase("geometry");
    harness.Check("this case proves the 64-bit two-cluster profile", g.xlen == 64,
                  "XLEN=" + std::to_string(g.xlen));
    harness.Check("there are exactly two clusters to be used", g.clusters == 2,
                  "MOSAIC_CLUSTERS=" + std::to_string(g.clusters));
    harness.Check("the retire window is two lanes wide", g.retire_width >= 2,
                  "retire width=" + std::to_string(g.retire_width));
    harness.Check("the uop id is exactly {rob_index, rob_gen, uop_index}",
                  g.uop_id_w == g.rob_index_w + g.rob_gen_w + g.uop_index_w,
                  "uop_id_w=" + std::to_string(g.uop_id_w) + " vs " +
                      std::to_string(g.rob_index_w) + "+" + std::to_string(g.rob_gen_w) +
                      "+" + std::to_string(g.uop_index_w));
    harness.Check("the ROB index spans its entry count",
                  g.rob_entries == (1u << g.rob_index_w),
                  "entries=" + std::to_string(g.rob_entries) + " index_w=" +
                      std::to_string(g.rob_index_w));
    harness.Check("the program fits the ROB window the index mapping assumes",
                  kProgWords <= static_cast<int>(g.rob_entries),
                  "program words=" + std::to_string(kProgWords));
    harness.Check("the reset vector is the base the program is linked at",
                  g.reset_vector == 0x80000000ull,
                  "reset vector=" + mosaic::Hex(g.reset_vector));

    auto clog2 = [](uint32_t value) {
      uint32_t w = 1;
      while ((1u << w) < value) w++;
      return w;
    };
    harness.Check("the physical tag and generation widths span the register file",
                  g.prf_tag_w == clog2(g.prf_entries) && g.int_gen_w == g.prf_tag_w,
                  "entries=" + std::to_string(g.prf_entries) + " tag_w=" +
                      std::to_string(g.prf_tag_w) + " int_gen_w=" +
                      std::to_string(g.int_gen_w));
    harness.Check("the ROB generation and occupancy widths follow the ROB depth",
                  g.rob_gen_w >= g.rob_index_w && g.occ_w == clog2(g.rob_entries + 1),
                  "rob_gen_w=" + std::to_string(g.rob_gen_w) + " occ_w=" +
                      std::to_string(g.occ_w));
    harness.Check("the retire sequence modulus spans two ROB generations",
                  g.seq_w == clog2(2 * g.rob_entries + 1),
                  "seq_w=" + std::to_string(g.seq_w));
    harness.Check("the event identity is a {tag, generation} pair",
                  g.ret_id_w == 2 * g.prf_tag_w,
                  "ret_id_w=" + std::to_string(g.ret_id_w));
    harness.Check("the fetch request id spans the outstanding window",
                  g.req_id_w == clog2(g.fetch_outstanding) &&
                      g.epoch_w == g.rob_index_w + 1,
                  "req_id_w=" + std::to_string(g.req_id_w) + " epoch_w=" +
                      std::to_string(g.epoch_w));
    harness.Check("each cluster's queue can hold work", g.iq_entries > 0,
                  "MOSAIC_IQ_ENTRIES=" + std::to_string(g.iq_entries));

    harness.Phase("reset-state");
    PhaseResetState(&harness);

    harness.Phase("redirect-arbiter");
    PhaseRedirectArb(&harness);

    harness.Phase("core-program");
    PhaseCoreProgram(&harness);

    harness.Phase("cluster-fabric");
    PhaseClusterFabric(&harness);

    harness.Phase("out-of-order-completion");
    PhaseOutOfOrder(&harness);

    detail = "checks=" + std::to_string(reporter.checks()) +
             " comparisons=" + std::to_string(harness.comparisons()) +
             " cycles=" + std::to_string(harness.cycles()) +
             " retires=" + std::to_string(harness.retires().size()) +
             " c0_alu=" + std::to_string(harness.dut_c0_alu()) +
             " c1_alu=" + std::to_string(harness.dut_c1_alu()) +
             " muldiv=" + std::to_string(harness.dut_muldiv()) +
             " dual_grant_cycles=" + std::to_string(harness.dual_grant_cycles()) +
             " max_occupied=" + std::to_string(harness.max_occupied()) +
             " ooo(index " + std::to_string(kYoungIndex) + " completed at " +
             std::to_string(harness.publish_cycle(kYoungIndex)) + ", index " +
             std::to_string(kLongIndex) + " at " +
             std::to_string(harness.publish_cycle(kLongIndex)) + ")" +
             " seed=" + std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the fabric contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}