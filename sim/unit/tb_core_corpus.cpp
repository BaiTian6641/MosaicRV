// ============================================================================
// tb_core_corpus.cpp -- CASE=core.corpus_branch, work package I-023.
//
// The DUT is the integrated p0 core (the same top CASE=fabric.fixed_two_cluster
// drives): fetch -> decode buffer -> dispatch -> two clusters -> writeback
// arbiter -> ROB -> retire -> commit to rename, with the redirect arbiter on the
// branch-resolution side.
//
// The program is a **real corpus program**, loaded from the ELF that
// `make -C tests/programs all` produces for `p02_branch` (the corpus program
// whose whole purpose is the six branch conditions, JAL/JALR and the JALR
// bit-0 clear rule). The case is about the control path -- branch resolution ->
// redirect arbiter -> fetch redirect, cluster purge, rename squash -- and this
// is the program that exercises it.
//
// ------------------------------------------------- what the DUT can execute
//
// The integrated core services ALU, branch and MUL/DIV macros only: dispatch
// refuses any macro whose decode names a memory or system operation *before*
// allocating it, and stops the machine there (mosaic_dispatch.sv). So the
// corpus program cannot run from `_start`: crt0's second instruction is a CSR
// write. It cannot run from `main` either: main's fourth instruction is a load.
//
// The harness therefore does two declared things, neither of them a change to
// the corpus program:
//
//   1. It writes a brick of instructions at the reset vector -- the address the
//      core's fixed reset vector names -- that builds the three declared input
//      words in a0/a1/a2 with immediates (the core has no load path) and jumps
//      to the program's branch region. crt0 lives there and would not run on
//      this machine anyway; nothing else the case executes is overwritten, and
//      the driver asserts that.
//   2. It enters the corpus program at the first instruction after
//      `MOSAIC_LOAD_INPUTS` -- the address the driver finds in the loaded image
//      by matching that macro's five-word pattern, not a hard-coded PC. That
//      macro is `la s2, mosaic_prog_inputs` followed by three `ld`s, which is
//      exactly the harness-input read this machine cannot perform; the program
//      text itself is executed unmodified from there.
//
// Everything the DUT retires after that entry point is the corpus program's own
// instructions, byte for byte as the assembler and linker produced them.
//
// ------------------------------------------------------------ the expectation
//
// Two expectations, neither of them from the DUT:
//
//   * `ReferenceTrace()` in this file is an RV64IM interpreter written from the
//     ISA text. It decodes the same words the machine is fed and produces the
//     expected retirement record -- pc, rd, reg-we, value -- for every
//     instruction, plus the branch statistics. It shares nothing with the RTL:
//     not a decoder, not an encoding table, not a register file. It stops where
//     the machine is documented to stop (the first macro dispatch refuses), so
//     the length of the trace is also a prediction.
//
//   * The four signature values the program computes are the values
//     `tools/host_oracle.py --program p02_branch` computes on the host, an
//     independent Python model of the ISA over the same declared inputs. They
//     are quoted in this file with the command that produced them, and the
//     driver checks the DUT's retired values for the four registers the
//     program's SIG0..SIG3 macros store against them. That is an expectation
//     that comes from neither the RTL nor this driver's interpreter.
//
// ------------------------------------------------------------- what it checks
//
//   1. the per-instruction retirement stream equals the reference, in order;
//   2. every redirect the arbiter issues goes to the resolved branch's target
//      (observed on the redirect port, not inferred from the retire stream);
//   3. the redirect count equals the number of taken control transfers the
//      reference executed, and every control transfer is acted on exactly once;
//   4. the arbiter waited for its macro to reach the ROB head (the wait counter
//      is non-zero) -- the head gate is exercised, not assumed;
//   5. the rename checkpoint is at a committed boundary and the squash is
//      accepted: `squash_not_committed` and `squash_underflow` stay zero;
//   6. the machine stops cleanly at the first refused macro and quiesces with an
//      empty ROB at a rename boundary;
//   7. the four signature values equal the host oracle's.
//
// ---------------------------------------------------------- what is not here
//
// Loads, stores, CSR accesses, traps and interrupts are not in this package, so
// the program's SIG0..SIG3 stores and its FINISH_PASS never execute: the machine
// stops just before them. The comparison is therefore per instruction over the
// architectural event stream, not an end-state check, which is the same form
// CASE=core.bringup_vs_reference uses. Nothing here is a substitute for the
// memory path (I-033..I-038) or the trap path (I-019/I-020).
//
// The conservative recovery in mosaic_core.sv makes a branch a barrier: nothing
// is dispatched behind a branch until it has resolved and, if it redirected, its
// redirect has been applied. The control path this case proves is the one that
// design has -- redirect arbitration at the ROB head, fetch redirect, front-end
// purge, ROB flush and the rename squash -- and the report states what the
// barrier still leaves unexercised (younger uops inside a cluster, which this
// design can never create).
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "elf_loader.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "sim_common.h"

// The reference interpreter and the DUT's data memory live in mem_ref.h so that
// this case and CASE=core.mem_program cannot disagree about either.
using mosaic_ref::DataMem;
using mosaic_ref::RefInsn;
using mosaic_ref::RefResult;

namespace {

constexpr int kResetCycles = 4;
// The program must reach its exit protocol within this many cycles. The run is a
// few hundred cycles long; this bound is only reached if the machine stopped
// making progress, and reporting it as a timeout is more useful than exhausting
// --max-cycles.
constexpr int kTraceCycles = 20000;
// How long the machine is given, after the predicted stream ends, for the stores
// that were already authorised to reach memory.
constexpr int kDrainCycles = 256;
// A stall is a defect with a location, not a timeout. The whole run is a few
// hundred cycles; this bound is only reached if the machine stopped making
// progress, and reporting it as a stall is more useful than exhausting
// --max-cycles.
constexpr int kStallCycles = 4000;
constexpr int kMaxModelSteps = 100000;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// Instruction encodings the harness needs (the brick at the reset vector only)
// ============================================================================
uint32_t EncR(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd,
              uint32_t op) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}

uint32_t EncI(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) | (f3 << 12) |
         (rd << 7) | op;
}

uint32_t EncU(uint32_t imm20, uint32_t rd, uint32_t op) {
  return ((imm20 & 0xFFFFFu) << 12) | (rd << 7) | op;
}

uint32_t EncJ(int32_t offset, uint32_t rd) {
  const uint32_t u = static_cast<uint32_t>(offset);
  const uint32_t imm = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
                       (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12);
  return imm | (rd << 7) | 0x6Fu;
}

uint32_t EncAddi(uint32_t rd, uint32_t rs1, int32_t imm) {
  return EncI(imm, rs1, 0x0u, rd, 0x13u);
}
uint32_t EncLui(uint32_t rd, uint32_t imm20) { return EncU(imm20, rd, 0x37u); }
uint32_t EncSlli(uint32_t rd, uint32_t rs1, uint32_t shamt) {
  return EncI(static_cast<int32_t>(shamt), rs1, 0x1u, rd, 0x13u);
}
uint32_t EncSrli(uint32_t rd, uint32_t rs1, uint32_t shamt) {
  return EncI(static_cast<int32_t>(shamt), rs1, 0x5u, rd, 0x13u);
}
uint32_t EncOr(uint32_t rd, uint32_t rs1, uint32_t rs2) {
  return EncR(0x00u, rs2, rs1, 0x6u, rd, 0x33u);
}

// ============================================================================
// The loaded program image
// ============================================================================
// The ELF's loadable segments, as words, with the harness's reset-vector brick
// written on top. The instruction memory returns ECALL for anything not in the
// image, so a runaway fetch stops the machine cleanly instead of reading a
// don't-care.
class ProgImage {
 public:
  bool Load(const std::string& path, std::string* detail) {
    mosaic::Image image;
    const mosaic::LoadStatus status = mosaic::LoadElf(path, &image, detail);
    if (status != mosaic::LoadStatus::kOk) {
      *detail = std::string("ELF refused: ") + mosaic::LoadStatusName(status) + ": " +
                *detail;
      return false;
    }
    entry_ = image.entry;
    segments_ = image.segments;
    image_ = image;
    for (const mosaic::Segment& seg : image.segments) {
      for (uint64_t off = 0; off + 4 <= seg.memsz; off += 4) {
        uint32_t word = 0;
        for (uint64_t b = 0; b < 4; b++) {
          if (off + b < seg.filesz) {
            word |= static_cast<uint32_t>(seg.data[off + b]) << (8 * b);
          }
        }
        words_[seg.vaddr + off] = word;
      }
    }
    if (words_.empty()) {
      *detail = "the image has no words";
      return false;
    }
    lo_ = words_.begin()->first;
    hi_ = words_.rbegin()->first;
    return true;
  }

  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    if (it == words_.end()) return 0x00000073u;  // ECALL
    return it->second;
  }

  bool Has(uint64_t addr) const { return words_.find(addr) != words_.end(); }

  void WriteWord(uint64_t addr, uint32_t word) { words_[addr] = word; }

  uint64_t entry() const { return entry_; }
  const std::vector<mosaic::Segment>& segments() const { return segments_; }
  const mosaic::Image& image() const { return image_; }
  uint64_t lowest() const { return lo_; }
  uint64_t highest() const { return hi_; }

 private:
  std::map<uint64_t, uint32_t> words_;
  std::vector<mosaic::Segment> segments_;
  mosaic::Image image_;
  uint64_t entry_ = 0;
  uint64_t lo_ = 0;
  uint64_t hi_ = 0;
};

// ============================================================================
// The independent reference: an RV64IM interpreter with memory
// ============================================================================
// Decoded from the same words the machine is fed, from the ISA text. It stops
// where dispatch refuses a macro (an illegal encoding, a CSR/system instruction
// or FENCE/FENCE.I) and after the program reaches its exit protocol, so the
// length of its trace is a prediction too: a machine that executes more, or
// stops somewhere else, disagrees.
//
// It lives in sim/unit/mem_ref.h with the DUT's data memory, because
// CASE=core.mem_program needs exactly the same two things, and one copy is one
// place for them to be right. Its memory is its own `mosaic::MemoryModel`
// instance: a load reads what the reference's own stores wrote, and the two
// byte-granular rules the machine's memory system depends on (which byte a
// byte store owns, and what a faulting access leaves behind) are stated once.
RefResult ReferenceRun(const ProgImage& img, uint64_t start,
                       mosaic::MemoryModel* mem) {
  return mosaic_ref::RunReference(img, start, mem);
}

// ============================================================================
// The instruction memory
// ============================================================================
// A one-cycle handshake: a request is accepted when `req_valid && req_ready`, its
// response is presented one cycle later, and it is held until the fetch unit
// takes it. Every accepted request produces exactly one response, which is the
// precondition the fetch unit's credit rule states.
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };

  explicit Imem(const ProgImage* img) : img_(img) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
    accepted_ = 0;
  }

  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint64_t ResponseWord() const { return img_->Word(ready_.front().addr); }

  void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
    Request r;
    r.addr = addr;
    r.id = id;
    r.epoch = epoch;
    inflight_.push_back(Entry{r, 1});
    accepted_++;
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

  uint64_t accepted() const { return accepted_; }

 private:
  struct Entry {
    Request req;
    int left = 0;
  };
  const ProgImage* img_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
  uint64_t accepted_ = 0;
};

// ============================================================================
// The harness
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t clusters = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint32_t rob_index_w = 0;
  uint32_t rob_gen_w = 0;
  uint32_t uop_index_w = 0;
  uint32_t uop_id_w = 0;
  uint32_t prf_entries = 0;
  uint32_t prf_tag_w = 0;
  uint32_t int_gen_w = 0;
  uint32_t iq_entries = 0;
  uint32_t occ_w = 0;
  uint32_t req_id_w = 0;
  uint32_t epoch_w = 0;
  uint32_t fetch_outstanding = 0;
  uint32_t seq_w = 0;
  uint32_t ret_id_w = 0;
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
  g.fetch_outstanding = dut->o_geom_fetch_outstanding_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.ret_id_w = dut->o_geom_ret_id_w_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

// Verilator hands a wide port over as a `VlWide` indexed in 32-bit words,
// two per 64-bit lane; narrow ports arrive as plain scalars.
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

class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(mem) {}

  void Configure(const Geometry& g) {
    g_ = g;
    ret_mask_ = (g.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g.retire_width) - 1u);
  }

  // The expectation, supplied before the run so every retirement and every
  // redirect can be compared *in the cycle it happens* rather than only at the
  // end. A divergence is then reported at the instruction it happens on, which
  // is the difference between "a check failed" and "this instruction was
  // wrong".
  void Expect(const std::vector<RefInsn>* trace,
              const std::vector<uint64_t>* targets) {
    expected_ = trace;
    expected_targets_ = targets;
  }

  void Phase(const std::string& name) { phase_ = name; }
  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }
  const std::string& phase() const { return phase_; }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  // --------------------------------------------------------------- recording
  struct Retire {
    uint64_t pc = 0;
    uint32_t rd = 0;
    bool reg_we = false;
    uint64_t value = 0;
    uint32_t seq = 0;
    uint64_t cycle = 0;
  };
  const std::vector<Retire>& retires() const { return retires_; }

  // Redirect observations, off the core's redirect port.
  struct Redirect {
    uint64_t pc = 0;
    uint64_t cycle = 0;
  };
  const std::vector<Redirect>& redirects() const { return redirects_; }

  void ClearTrace() {
    retires_.clear();
    redirects_.clear();
    last_progress_ = 0;
    last_commit_ = 0;
    imem_.Reset();
    dmem_.Reset();
  }

  // The reference's whole trace has been retired. The run stops there: the
  // program has reached its exit protocol and everything after it is the park
  // loop, which is not part of the prediction (see mem_ref.h).
  bool Complete() const {
    return expected_ != nullptr && retires_.size() >= expected_->size();
  }

  // From here on nothing is compared against the reference: the run continues
  // only so the stores that were already authorised can reach memory, which is
  // what makes the exit protocol observable. The redirect-target comparison
  // must stop too -- the program is in its park loop and redirects on every
  // iteration -- while the redirects already observed, and the counters read
  // before this call, are what the case checks.
  void StopComparing() {
    expected_ = nullptr;
    expected_targets_ = nullptr;
  }

  const DataMem& dmem() const { return dmem_; }
  size_t dbg_txns_ = 0;

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + Diagnose());
    }
    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = 1;
    dut_->imem_rsp_valid_i = imem_.HasResponse() ? 1 : 0;
    if (imem_.HasResponse()) {
      const Imem::Request& r = imem_.Response();
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem_.ResponseWord());
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = r.id;
      dut_->imem_rsp_epoch_i = r.epoch;
      dut_->imem_rsp_len_i = 4;
    } else {
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;

    // ------------------------------------------------------------- data port
    // The memory system's side of the LSU endpoint's protocol. A request is
    // taken whenever the endpoint offers one; the response is presented for as
    // many cycles as it takes the endpoint to consume it, which is a real
    // back-pressure case rather than the trivial always-ready one.
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
    dut_->eval();

    if (!rst) Observe();

    // The handshake is sampled after eval, and the memory advances at the edge.
    if ((dut_->imem_req_valid_o != 0) && (dut_->imem_req_ready_i != 0)) {
      imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if ((dut_->imem_rsp_valid_i != 0) && (dut_->imem_rsp_ready_o != 0)) {
      imem_.PopResponse();
    }
    imem_.Advance();

    if ((dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0)) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(r, cycles_);
    }
    if ((dut_->dmem_rsp_valid_i != 0) && (dut_->dmem_rsp_ready_o != 0)) {
      dmem_.PopResponse();
    }
    dmem_.Advance();
    if (std::getenv("MOSAIC_CORPUS_MEM") != nullptr && dmem_.txns().size() != dbg_txns_) {
      dbg_txns_ = dmem_.txns().size();
      const DataMem::Txn& t = dmem_.txns().back();
      std::printf("  [dmem] cycle=%llu #%zu we=%d addr=%s size=%u wstrb=%02x wdata=%s\n",
                  static_cast<unsigned long long>(cycles_), dmem_.txns().size(),
                  t.req.we ? 1 : 0, U64(t.req.addr).c_str(), t.req.size, t.req.wstrb,
                  U64(t.req.wdata).c_str());
    }

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  bool Stopped() const { return stopped_; }
  bool Occupied() const { return occupied_ != 0; }

  // What the machine is doing, for a failure message.
  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) +
           " stopped=" + Dec(dut_->o_stopped_o) +
           " redirects=" + Dec(dut_->o_redirect_o) +
           " recovering=" + Dec(dut_->o_recovering_o);
  }

 private:
  void Observe() {
    Compare("the retire counter equals the event stream published so far",
            dut_->o_commit_o == retires_.size(),
            "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(retires_.size()));
    Compare("ROB occupancy is within the ROB",
            dut_->o_rob_occupied_o <= g_.rob_entries,
            "occupied=" + Dec(dut_->o_rob_occupied_o));
    Compare("the divergent-recovery counters stay zero",
            dut_->o_squash_nc_o == 0 && dut_->o_squash_under_o == 0 &&
                dut_->o_journal_ovf_o == 0,
            "squash_nc=" + Dec(dut_->o_squash_nc_o) + " under=" + Dec(dut_->o_squash_under_o) +
                " journal=" + Dec(dut_->o_journal_ovf_o));
    if (dut_->o_rob_occupied_o == 0) {
      Compare("an empty ROB is at a rename boundary",
              dut_->o_rename_boundary_o != 0,
              "occupied=0, o_rename_boundary=0 at commit=" + Dec(dut_->o_commit_o));
    }

    // The cycle after a redirect the front end must be fetching the redirect's
    // target. Checked here rather than left to the retirement stream, because
    // this is the redirect's own contract: "the winner redirects fetch".
    if (pending_redirect_cycle_ >= 0 && cycles_ == pending_redirect_cycle_ + 1) {
      if (dut_->o_fetch_pc_o != pending_redirect_pc_) {
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "the front end fetches the redirect's target: after the redirect to " +
             U64(pending_redirect_pc_) + " the fetch PC is " +
             U64(dut_->o_fetch_pc_o));
      }
      comparisons_++;
    }
    if (dut_->o_redirect_valid_o != 0) {
      Redirect r;
      r.pc = dut_->o_redirect_pc_o;
      r.cycle = cycles_;
      redirects_.push_back(r);
      pending_redirect_pc_ = r.pc;
      pending_redirect_cycle_ = static_cast<int>(cycles_);
      if (expected_targets_ != nullptr) {
        if (redirects_.size() > expected_targets_->size()) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the redirect the arbiter issues names the taken transfer's target: "
               "redirect " + Dec(redirects_.size() - 1) + " has no counterpart -- the "
               "reference executed only " + Dec(expected_targets_->size()) +
               " taken control transfers");
        }
        const uint64_t want = (*expected_targets_)[redirects_.size() - 1];
        if (r.pc != want) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the redirect the arbiter issues names the taken transfer's target: "
               "redirect " + Dec(redirects_.size() - 1) + " pc expected " + U64(want) +
               ", got " + U64(r.pc));
        }
        comparisons_++;
      }
      if (std::getenv("MOSAIC_CORPUS_TRACE") != nullptr) {
        std::printf("  [trace] cycle=%5llu REDIRECT to %s\n",
                    static_cast<unsigned long long>(cycles_), U64(r.pc).c_str());
      }
    }

    const uint32_t mask = static_cast<uint32_t>(dut_->ev_valid_o) & ret_mask_;
    if (std::getenv("MOSAIC_CORPUS_TRACE") != nullptr) {
      static uint64_t last = ~0ull;
      const uint64_t b = dut_->o_dbg_redir_o;
      if (b != last) {
        last = b;
        std::printf("  [redir] cycle=%5llu c0v=%llu c0t=%llu c0idx=%llu c0gen=%llu "
                    "c1v=%llu c1t=%llu c1idx=%llu c1gen=%llu | hv=%llu hr=%llu "
                    "hidx=%llu hgen=%llu occ=%llu act=%llu tak=%llu bar=%llu rec=%llu\n",
                    static_cast<unsigned long long>(cycles_),
                    (unsigned long long)(b >> 0 & 1), (unsigned long long)(b >> 2 & 1),
                    (unsigned long long)(b >> 7 & 0x3F), (unsigned long long)(b >> 13 & 0x7F),
                    (unsigned long long)(b >> 1 & 1), (unsigned long long)(b >> 3 & 1),
                    (unsigned long long)(b >> 22 & 0x3F), (unsigned long long)(b >> 28 & 0x7F),
                    (unsigned long long)(b >> 35 & 1), (unsigned long long)(b >> 36 & 1),
                    (unsigned long long)(b >> 37 & 0x3F), (unsigned long long)(b >> 43 & 0x7F),
                    (unsigned long long)(b >> 54 & 0xFF), (unsigned long long)(b >> 50 & 1),
                    (unsigned long long)(b >> 51 & 1), (unsigned long long)(b >> 52 & 1),
                    (unsigned long long)(b >> 53 & 1));
      }
    }
    if (std::getenv("MOSAIC_CORPUS_FETCH") != nullptr) {
      const auto& f = dut_->o_dbg_fetch_o;
      auto fbit = [&](int n) -> int { return (f[n / 32] >> (n % 32)) & 1u; };
      auto fbits = [&](int lo, int hi) -> uint64_t {
        uint64_t v = 0;
        for (int n = lo; n <= hi; n++) v |= static_cast<uint64_t>(fbit(n)) << (n - lo);
        return v;
      };
      std::printf("  [fetch] cycle=%4llu out_v=%d out_pc=%s rsp_v=%d rsp_rdy=%d "
                  "rsp_fire=%d live=%d stale=%d owns=%d redir=%d rsp_id=%llu "
                  "rsp_epoch=%llu epoch=%llu req_rdy=%d req_v=%d req_fire=%d "
                  "busy=%llx canc=%llx slot0_epoch=%llu bits=%08x\n",
                  static_cast<unsigned long long>(cycles_), fbit(0),
                  U64(fbits(64, 95)).c_str(), fbit(1), fbit(2), fbit(3), fbit(4), fbit(5),
                  fbit(6), fbit(7),
                  static_cast<unsigned long long>(fbits(8, 11)),
                  static_cast<unsigned long long>(fbits(12, 18)),
                  static_cast<unsigned long long>(fbits(19, 25)), fbit(26), fbit(27),
                  fbit(28),
                  static_cast<unsigned long long>(fbits(29, 32)),
                  static_cast<unsigned long long>(fbits(33, 36)),
                  static_cast<unsigned long long>(fbits(37, 43)),
                  static_cast<unsigned>(fbits(96, 127)));
    }
    if (g_.retire_width >= 2) {
      Compare("retire lane 1 is never set without lane 0",
              ((mask & 2u) == 0) || ((mask & 1u) != 0), "ev_valid=" + Dec(mask));
    }
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((mask & (1u << lane)) == 0) continue;
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.seq = static_cast<uint32_t>(PackedLane(dut_->ev_seq_o, lane, g_.seq_w));
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.cycle = cycles_;
      if (!retires_.empty()) {
        const uint32_t prev = retires_.back().seq;
        const uint32_t dist = (r.seq - prev) & ((1u << g_.seq_w) - 1u);
        Compare("the retire sequence strictly increases",
                dist != 0 && dist < (1u << (g_.seq_w - 1)),
                "prev seq=" + Dec(prev) + " now=" + Dec(r.seq));
      }
      retires_.push_back(r);
      if (expected_ != nullptr) {
        if (retires_.size() > expected_->size()) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the retirement stream follows the reference: retire " +
               Dec(retires_.size() - 1) + " at pc " + U64(r.pc) +
               " has no counterpart -- the reference retires only " +
               Dec(expected_->size()) + " instructions before the refused macro at " +
               U64((*expected_).empty() ? 0 : expected_->back().next_pc) + "");
        }
        const RefInsn& e = (*expected_)[retires_.size() - 1];
        if (r.pc != e.pc) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the retirement stream follows the reference: retire " +
               Dec(retires_.size() - 1) + " pc expected " + U64(e.pc) + ", got " +
               U64(r.pc));
        }
        if (r.rd != e.rd || r.reg_we != e.reg_we) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the retirement stream follows the reference: retire " +
               Dec(retires_.size() - 1) + " at " + U64(e.pc) + " destination expected rd=" +
               Dec(e.rd) + " we=" + Dec(e.reg_we) + ", got rd=" + Dec(r.rd) +
               " we=" + Dec(r.reg_we));
        }
        if (e.reg_we && r.value != e.value) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the retirement stream follows the reference: retire " +
               Dec(retires_.size() - 1) + " at " + U64(e.pc) + " value for x" +
               Dec(e.rd) + " expected " + U64(e.value) + ", got " + U64(r.value));
        }
        comparisons_++;
      }
      if (std::getenv("MOSAIC_CORPUS_TRACE") != nullptr) {
        std::printf("  [trace] cycle=%5llu retire pc=%s rd=%2u we=%u value=%s "
                    "c0br=%s c1br=%s act=%s wait=%s\n",
                    static_cast<unsigned long long>(cycles_), U64(r.pc).c_str(), r.rd,
                    r.reg_we ? 1 : 0, U64(r.value).c_str(),
                    Dec(dut_->o_c0_br_o).c_str(), Dec(dut_->o_c1_br_o).c_str(),
                    Dec(dut_->o_redir_act_o).c_str(), Dec(dut_->o_redir_wait_o).c_str());
      }
    }

    stopped_ = dut_->o_stopped_o != 0;
    occupied_ = dut_->o_rob_occupied_o;
    ProgressCheck();
  }

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
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled: " + Diagnose());
    }
  }

  std::string Diagnose() {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) +
           " stopped=" + Dec(dut_->o_stopped_o) +
           " redirects=" + Dec(dut_->o_redirect_o) +
           " recovering=" + Dec(dut_->o_recovering_o);
  }

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  Geometry g_;
  uint32_t ret_mask_ = 0;
  std::string phase_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::vector<Retire> retires_;
  std::vector<Redirect> redirects_;
  uint64_t pending_redirect_pc_ = 0;
  int pending_redirect_cycle_ = -1;
  const std::vector<RefInsn>* expected_ = nullptr;
  const std::vector<uint64_t>* expected_targets_ = nullptr;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  bool stopped_ = false;
  uint64_t occupied_ = 0;
};

// ============================================================================
// The harness brick at the reset vector
// ============================================================================
// rd = v, using `scratch` as a temporary. Everything here is a base RV64I
// instruction the p0 core services.
void EmitLi32(uint32_t rd, uint32_t v32, std::vector<uint32_t>* out) {
  // sign_extend_64(v32): lui carries the top 20 bits, addi the low 12.
  const uint32_t hi20 = (v32 + 0x800u) >> 12;
  const int32_t lo12 = static_cast<int32_t>(v32 << 20) >> 20;
  out->push_back(EncLui(rd, hi20 & 0xFFFFFu));
  if (lo12 != 0) out->push_back(EncAddi(rd, rd, lo12));
}

void EmitLi64Full(uint32_t rd, uint32_t scratch, uint64_t v, std::vector<uint32_t>* out);

void EmitLi64(uint32_t rd, uint32_t scratch, uint64_t v, std::vector<uint32_t>* out) {
  if ((v >> 32) == 0) {
    EmitLi32(rd, static_cast<uint32_t>(v), out);
    return;
  }
  EmitLi64Full(rd, scratch, v, out);
}

// The same, without the "it fits in 32 bits" shortcut. `EmitLi32` sign-extends
// its immediate, so it cannot materialise a value whose bit 31 is set -- the
// corpus's own RAM base (0x80000000) is one -- and the shortcut would give
// 0xffffffff80000000 for it.
void EmitLi64Full(uint32_t rd, uint32_t scratch, uint64_t v, std::vector<uint32_t>* out) {
  EmitLi32(rd, static_cast<uint32_t>(v >> 32), out);
  out->push_back(EncSlli(rd, rd, 32));            // the sign extension shifts out
  EmitLi32(scratch, static_cast<uint32_t>(v), out);
  out->push_back(EncSlli(scratch, scratch, 32));  // zero-extend the low half
  out->push_back(EncSrli(scratch, scratch, 32));
  out->push_back(EncOr(rd, rd, scratch));
}

// Register names used by the brick. a0/a1/a2 are the corpus's three inputs
// (platform.h); x31 is the scratch -- p02_branch never writes it, which the
// driver checks against the reference trace.
constexpr uint32_t kRegA0 = 10;
constexpr uint32_t kRegA1 = 11;
constexpr uint32_t kRegA2 = 12;
constexpr uint32_t kRegS0 = 8;    // the corpus programs' signature base (platform.h)
constexpr uint32_t kRegScratch = 31;

// The MOSAIC_LOAD_INPUTS pattern (platform.h): `la s2, mosaic_prog_inputs`
// followed by the three loads from it. The driver finds the program's branch
// region by matching it rather than by hard-coding a PC.
struct InputPrologue {
  bool found = false;
  uint64_t at = 0;      // address of the `auipc s2, ...`
  uint64_t after = 0;   // the first instruction of the program body
};

InputPrologue FindInputPrologue(const ProgImage& img, uint64_t from) {
  InputPrologue out;
  for (uint64_t pc = from; pc + 20 <= img.highest() + 4; pc += 4) {
    const uint32_t w0 = img.Word(pc);
    const uint32_t w1 = img.Word(pc + 4);
    const uint32_t w2 = img.Word(pc + 8);
    const uint32_t w3 = img.Word(pc + 12);
    const uint32_t w4 = img.Word(pc + 16);
    // auipc s2, ... ; addi s2, s2, ... ; ld a0,0(s2) ; ld a1,8(s2) ; ld a2,16(s2)
    const bool is_auipc_s2 = ((w0 & 0x7Fu) == 0x17u) && (((w0 >> 7) & 0x1Fu) == 18u);
    const bool is_addi_s2 = ((w1 & 0x7Fu) == 0x13u) && (((w1 >> 7) & 0x1Fu) == 18u) &&
                            (((w1 >> 15) & 0x1Fu) == 18u) && (((w1 >> 12) & 0x7u) == 0u);
    const bool is_ld_a0 = ((w2 & 0x7Fu) == 0x03u) && (((w2 >> 7) & 0x1Fu) == 10u) &&
                          (((w2 >> 15) & 0x1Fu) == 18u) && (((w2 >> 12) & 0x7u) == 0x3u) &&
                          (((w2 >> 20) & 0xFFFu) == 0u);
    const bool is_ld_a1 = ((w3 & 0x7Fu) == 0x03u) && (((w3 >> 7) & 0x1Fu) == 11u) &&
                          (((w3 >> 15) & 0x1Fu) == 18u) && (((w3 >> 12) & 0x7u) == 0x3u) &&
                          (((w3 >> 20) & 0xFFFu) == 8u);
    const bool is_ld_a2 = ((w4 & 0x7Fu) == 0x03u) && (((w4 >> 7) & 0x1Fu) == 12u) &&
                          (((w4 >> 15) & 0x1Fu) == 18u) && (((w4 >> 12) & 0x7u) == 0x3u) &&
                          (((w4 >> 20) & 0xFFFu) == 16u);
    if (is_auipc_s2 && is_addi_s2 && is_ld_a0 && is_ld_a1 && is_ld_a2) {
      out.found = true;
      out.at = pc;
      out.after = pc + 20;
      return out;
    }
  }
  return out;
}

// ============================================================================
// One (program, input) run
// ============================================================================
struct RunResult {
  RefResult reference;
  uint64_t entry = 0;
  uint64_t brick_words = 0;
  uint64_t brick_end = 0;
  uint64_t redirect_ctr = 0;
  uint64_t act_ctr = 0;
  uint64_t wait_ctr = 0;
  uint64_t dead_ctr = 0;
  uint64_t squash_acc = 0;
  uint64_t ckpt = 0;
  uint64_t unsupported_ctr = 0;
  uint64_t stop_ctr = 0;
  uint64_t cycles = 0;
  uint64_t comparison_count = 0;
};

// ============================================================================
// The repository root
// ============================================================================
// The directory that holds config/profiles/p0.json, found by walking up from the
// working directory, so the case runs the same way whether it is started by
// tools/run_unit.py or by hand.
std::string FindRepoRoot() {
  std::string dir = ".";
  for (int depth = 0; depth < 8; ++depth) {
    std::ifstream probe(dir + "/config/profiles/p0.json");
    if (probe) {
      char resolved[4096];
      if (realpath(dir.c_str(), resolved) != nullptr) return std::string(resolved);
      return dir;
    }
    dir += "/..";
  }
  Fail("setup", "cannot find the repository root: no config/profiles/p0.json above the "
                "working directory");
}

// ============================================================================
// The declared corpus inputs and their host-oracle signatures
// ============================================================================
// `python3 tools/host_oracle.py --program p02_branch`, run at the time this case
// was written, prints exactly these rows (sig0..sig3 are the four words the
// program's SIG0..SIG3 macros publish, and the registers that hold them at the
// point the machine stops are t0, s3, t4 and t3). They are transcribed here so
// the comparison is against the host oracle's arithmetic and not against this
// driver's interpreter -- the two are independent, and a disagreement between
// them fails the case rather than being averaged away.
struct CorpusInput {
  uint64_t a;
  uint64_t b;
  uint64_t c;
  uint64_t sig0;  // t0  = x5
  uint64_t sig1;  // s3  = x19
  uint64_t sig2;  // t4  = x29
  uint64_t sig3;  // t3  = x28
};

const CorpusInput kInputs[3] = {
    {UINT64_C(0x0), UINT64_C(0x1), UINT64_C(0x3), UINT64_C(0x16), UINT64_C(0x3),
     UINT64_C(0x4), UINT64_C(0x1)},
    {UINT64_C(0x8000000000000000), UINT64_C(0x7fffffffffffffff), UINT64_C(0x5),
     UINT64_C(0x26), UINT64_C(0x3), UINT64_C(0x6), UINT64_C(0x7fffffffffffffff)},
    {UINT64_C(0xffffffffffffffff), UINT64_C(0xffffffffffffffff), UINT64_C(0x9),
     UINT64_C(0x29), UINT64_C(0x3), UINT64_C(0x2), UINT64_C(0xffffffffffffffff)},
};

// ============================================================================
// One (program, input) run
// ============================================================================
RunResult RunOnce(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
                  const ProgImage& program, const InputPrologue& prologue,
                  const Geometry& geometry, const CorpusInput& input, int index) {
  RunResult result;
  result.entry = prologue.after;

  // ---- the harness brick: the three inputs, then a jump into the program ----
  // The brick also establishes the signature base register, which main's own
  // MOSAIC_SETUP_BASES would have set had the run started there. It is needed
  // now that the program's SIG0..SIG3 stores actually execute: without it they
  // would write through s0 = 0, which is the read-only boot ROM.
  ProgImage image = program;
  std::vector<uint32_t> brick;
  EmitLi64(kRegA0, kRegScratch, input.a, &brick);
  EmitLi64(kRegA1, kRegScratch, input.b, &brick);
  EmitLi64(kRegA2, kRegScratch, input.c, &brick);
  EmitLi64Full(kRegS0, kRegScratch, MOSAIC_SIGNATURE_ADDR, &brick);
  brick.push_back(EncJ(static_cast<int32_t>(prologue.after -
                                             (geometry.reset_vector + 4 * brick.size())),
                       0));
  result.brick_words = brick.size();
  for (size_t i = 0; i < brick.size(); i++) {
    image.WriteWord(geometry.reset_vector + 4 * i, brick[i]);
  }
  result.brick_end = geometry.reset_vector + 4 * brick.size();

  // The brick must not reach anything the program executes. main and the stub
  // are thousands of bytes up; the corpus input table is the nearest thing below
  // it, and the brick must stay under both.
  if (result.brick_end > prologue.at) {
    Fail("run", "the harness brick reaches the corpus program's own body");
  }

  // ---- memory: one model for the DUT's data port, one for the reference ----
  // Two instances, loaded from the same image, so a store the DUT performs can
  // never be the value the reference loads: the reference's own stores are the
  // only thing that reaches the reference's memory.
  mosaic::MemoryModel dut_mem;
  mosaic::MemoryModel ref_mem;
  {
    std::string mem_detail;
    if (!dut_mem.LoadImage(program.image(), &mem_detail)) Fail("run", mem_detail);
    if (!ref_mem.LoadImage(program.image(), &mem_detail)) Fail("run", mem_detail);
  }

  // ---- the expectation: an independent RV64IM interpreter on the same words --
  result.reference = ReferenceRun(image, geometry.reset_vector, &ref_mem);
  if (std::getenv("MOSAIC_CORPUS_TRACE") != nullptr) {
    std::printf("  [ref] size=%zu stopped=%d reason=%s stop_pc=%s exited=%d exit_pc=%s\n",
                result.reference.trace.size(), result.reference.stopped ? 1 : 0,
                result.reference.stop_reason.c_str(), U64(result.reference.stop_pc).c_str(),
                result.reference.exited ? 1 : 0, U64(result.reference.exit_pc).c_str());
  }

  // The brick builds the inputs with a scratch register. Nothing in the corpus
  // program may write it, or the brick's own arithmetic would be visible as a
  // program result; the reference trace is where that is checked.
  for (const RefInsn& insn : result.reference.trace) {
    if (insn.pc >= prologue.after && insn.reg_we && insn.rd == kRegScratch) {
      Fail("run", "the corpus program writes x31 at " + U64(insn.pc) +
                      ", which is the harness brick's scratch register");
    }
  }

  // ---- run ----
  Harness harness(dut, reporter, max_cycles, &image, &dut_mem);
  harness.Configure(geometry);
  harness.Phase("run-input" + Dec(index));

  // The expected redirect targets, in the order the reference takes them: the
  // arbiter's output is compared against them as each one is issued.
  std::vector<uint64_t> expected_targets;
  for (const RefInsn& insn : result.reference.trace) {
    if (insn.is_control && insn.taken) expected_targets.push_back(insn.next_pc);
  }
  harness.Expect(&result.reference.trace, &expected_targets);

  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);
  harness.ClearTrace();

  // Run until the reference's whole trace has retired AND every taken transfer
  // in it has been acted on. The second half matters because the arbiter's
  // redirect is registered: the last transfer of the prediction is the park
  // loop's own branch, which retires in cycle T and redirects in T+1, so a loop
  // that stopped at the retirement would read one redirect short. Waiting for
  // that redirect is safe -- the next park-loop iteration retires some twenty
  // cycles later -- and the retire comparison stays armed the whole time, so an
  // extra retirement is still named rather than silently accepted.
  while (!harness.Complete() ||
         harness.redirects().size() < expected_targets.size()) {
    harness.Cycle(false);
    if (harness.cycles() > kTraceCycles) {
      Fail("run-input" + Dec(index),
           "the program did not reach its exit protocol within " + Dec(kTraceCycles) +
               " cycles: " + harness.State());
    }
  }

  // The counters are read at the instant the predicted stream ends, after a
  // short settle window: the rename squash is issued the cycle *after* the
  // redirect (`ren_squash = redirect_delay_q` in mosaic_core.sv), so the last
  // recovery's counters land one cycle later. The comparison stays armed for
  // these four cycles, so a machine that retired or redirected again in them is
  // still named rather than tolerated; the park loop's next iteration is an
  // order of magnitude further away, so they are quiet.
  for (int i = 0; i < 4; i++) harness.Cycle(false);

  result.cycles = harness.cycles();
  result.comparison_count = harness.comparisons();
  result.redirect_ctr = dut->o_redirect_o;
  result.act_ctr = dut->o_redir_act_o;
  result.wait_ctr = dut->o_redir_wait_o;
  result.dead_ctr = dut->o_redir_dead_o;
  result.squash_acc = dut->o_squash_acc_o;
  result.ckpt = dut->o_ckpt_o;
  result.unsupported_ctr = dut->o_unsupported_o;
  result.stop_ctr = dut->o_stop_o;
  const uint64_t redirects_at_end = harness.redirects().size();

  // Let the authorised stores drain. Nothing is compared from here on; the
  // machine is executing the program's own park loop, which the reference does
  // not predict because the program has already ended.
  harness.StopComparing();
  for (int i = 0; i < kDrainCycles && !dut_mem.finished(); i++) harness.Cycle(false);

  const std::string ph = "run-input" + Dec(index);
  const RefResult& ref = result.reference;
  const std::vector<Harness::Retire>& got = harness.retires();

  if (std::getenv("MOSAIC_CORPUS_TRACE") != nullptr) {
    std::printf("  [trace] %zu retires, brick=%zu words at %s..%s, entry=%s, ref=%zu "
                "instructions (control=%u taken=%u), act=%s wait=%s dead=%s redirects=%s "
                "ckpt=%s squash=%s tohost=%s\n",
                got.size(), brick.size(), U64(geometry.reset_vector).c_str(),
                U64(result.brick_end).c_str(), U64(prologue.after).c_str(),
                ref.trace.size(), ref.control_total, ref.taken_total,
                Dec(result.act_ctr).c_str(), Dec(result.wait_ctr).c_str(),
                Dec(result.dead_ctr).c_str(), Dec(redirects_at_end).c_str(),
                Dec(result.ckpt).c_str(), Dec(result.squash_acc).c_str(),
                U64(dut_mem.exit_code()).c_str());
    for (size_t i = 0; i < got.size(); i++) {
      std::printf("  [trace] retire %3zu pc=%s rd=%2u we=%u value=%s\n", i,
                  U64(got[i].pc).c_str(), got[i].rd, got[i].reg_we ? 1 : 0,
                  U64(got[i].value).c_str());
    }
    for (size_t i = 0; i < ref.trace.size(); i++) {
      std::printf("  [ref  ] insn   %3zu pc=%s rd=%2u we=%u value=%s%s%s\n", i,
                  U64(ref.trace[i].pc).c_str(), ref.trace[i].rd,
                  ref.trace[i].reg_we ? 1 : 0, U64(ref.trace[i].value).c_str(),
                  ref.trace[i].is_control ? " control" : "",
                  ref.trace[i].taken ? " taken" : "");
    }
  }

  // ---- 1. every redirect goes to the resolved transfer's target ----
  if (redirects_at_end != expected_targets.size()) {
    Fail(ph, "the arbiter issued " + Dec(redirects_at_end) +
                 " redirects, the reference executed " + Dec(expected_targets.size()) +
                 " taken control transfers");
  }
  for (size_t i = 0; i < expected_targets.size(); i++) {
    if (harness.redirects()[i].pc != expected_targets[i]) {
      Fail(ph, "redirect " + Dec(i) + " pc: expected the taken transfer's target " +
                   U64(expected_targets[i]) + ", got " + U64(harness.redirects()[i].pc));
    }
  }
  reporter->Check(true, ph + ": every redirect names the taken transfer's target (" +
                        Dec(expected_targets.size()) + " redirects)");

  // ---- 2. the per-instruction architectural stream ----
  if (!ref.stopped) {
    Fail(ph, "the reference model never reached a refused macro");
  }
  if (got.size() != ref.trace.size()) {
    std::string ctx = "\n";
    const size_t n = std::min<size_t>(ref.trace.size(), 8);
    for (size_t i = ref.trace.size() - n; i < ref.trace.size(); i++) {
      ctx += "      ref  [" + Dec(i) + "] pc=" + U64(ref.trace[i].pc) + " rd=" +
             Dec(ref.trace[i].rd) + " we=" + Dec(ref.trace[i].reg_we) + " value=" +
             U64(ref.trace[i].value) + (ref.trace[i].is_control ? " control" : "") +
             (ref.trace[i].taken ? " taken -> " + U64(ref.trace[i].next_pc) : "") + "\n";
      if (i < got.size()) {
        ctx += "      dut  [" + Dec(i) + "] pc=" + U64(got[i].pc) + " rd=" +
               Dec(got[i].rd) + " we=" + Dec(got[i].reg_we) + " value=" +
               U64(got[i].value) + "\n";
      }
    }
    ctx += "      ref stops at " + U64(ref.stop_pc) + " word=" +
           mosaic::Hex(ref.stop_word, 8) + "\n";
    for (size_t i = ref.trace.size(); i < got.size() && i < ref.trace.size() + 4; i++) {
      ctx += "      dut  [" + Dec(i) + "] pc=" + U64(got[i].pc) + " rd=" + Dec(got[i].rd) +
             " we=" + Dec(got[i].reg_we) + " value=" + U64(got[i].value) + "\n";
    }
    Fail(ph, "retired " + Dec(got.size()) + " instructions, the reference predicts " +
                 Dec(ref.trace.size()) + ctx);
  }
  for (size_t i = 0; i < ref.trace.size(); i++) {
    const RefInsn& e = ref.trace[i];
    const Harness::Retire& r = got[i];
    if (r.pc != e.pc) {
      Fail(ph, "retire " + Dec(i) + " pc: expected " + U64(e.pc) + ", got " + U64(r.pc) +
                   " -- the machine retired " + Dec(got.size()) +
                   " instructions, the reference predicts " + Dec(ref.trace.size()));
    }
    if (r.rd != e.rd || r.reg_we != e.reg_we) {
      Fail(ph, "retire " + Dec(i) + " at " + U64(e.pc) + " destination: expected rd=" +
                   Dec(e.rd) + " we=" + Dec(e.reg_we) + ", got rd=" + Dec(r.rd) +
                   " we=" + Dec(r.reg_we));
    }
    if (e.reg_we && r.value != e.value) {
      Fail(ph, "retire " + Dec(i) + " at " + U64(e.pc) + " value for x" + Dec(e.rd) +
                   ": expected " + U64(e.value) + ", got " + U64(r.value));
    }
  }
  reporter->Check(true, ph + ": the per-instruction retire stream matches the "
                             "independent RV64IM interpretation (" +
                        Dec(ref.trace.size()) + " instructions)");

  // ---- 3. the arbiter accounted for every resolution exactly once ----
  if (result.act_ctr != ref.control_total) {
    Fail(ph, "the arbiter acted on " + Dec(result.act_ctr) + " resolutions; the "
                 "reference executed " + Dec(ref.control_total) + " control transfers");
  }
  if (result.redirect_ctr != ref.taken_total) {
    Fail(ph, "the redirect counter is " + Dec(result.redirect_ctr) + ", the reference "
                 "executed " + Dec(ref.taken_total) + " taken transfers");
  }
  reporter->Check(true, ph + ": all " + Dec(ref.control_total) +
                        " control transfers were acted on, " + Dec(ref.taken_total) +
                        " of them taken");

  // ---- 4. the head gate was exercised ----
  if (result.wait_ctr == 0) {
    Fail(ph, "the redirect arbiter never waited for a branch to reach the ROB head "
                 "(o_redir_wait_ctr=0), so the head gate was never exercised");
  }
  if (result.dead_ctr != 0) {
    Fail(ph, "the arbiter dropped " + Dec(result.dead_ctr) +
                 " requests as dead: a resolution named a slot the ROB no longer owns");
  }
  reporter->Check(true, ph + ": the arbiter waited " + Dec(result.wait_ctr) +
                        " cycles for its macro to become the retiring head, and dropped "
                        "no request as dead");

  // ---- 5. the rename squash was taken at a committed boundary ----
  if (result.ckpt != ref.taken_total) {
    Fail(ph, "the core took " + Dec(result.ckpt) + " rename checkpoints for " +
                 Dec(ref.taken_total) + " redirects");
  }
  if (result.squash_acc != ref.taken_total) {
    Fail(ph, "rename accepted " + Dec(result.squash_acc) + " squashes for " +
                 Dec(ref.taken_total) + " redirects (refused = " +
                 Dec(dut->o_squash_nc_o) + ", underflow = " + Dec(dut->o_squash_under_o) + ")");
  }
  reporter->Check(true, ph + ": rename took " + Dec(result.ckpt) +
                        " checkpoints and accepted " + Dec(result.squash_acc) +
                        " squashes, all at a committed boundary");

  // ---- 6. the program reached its exit protocol ----
  // The frames this replaces asserted that the machine *stopped* at the first
  // macro dispatch refuses. That was true of a machine with no memory path:
  // every corpus program's first refused macro used to be a load or a store.
  // Now they execute, the program runs on to the frozen exit protocol, and the
  // stronger statement is available: it wrote TOHOST with the PASS bit set, and
  // the four signature words it published are in memory where the protocol says
  // they are -- through the store path, not through a register read.
  if (!dut_mem.finished()) {
    Fail(ph, "the program never wrote TOHOST, so it never reached its exit protocol "
             "(the reference stops with: " + ref.stop_reason + ")");
  }
  if (!dut_mem.passed()) {
    Fail(ph, "the program wrote TOHOST = " + U64(dut_mem.exit_code()) +
                 ", whose bit 0 is not set: the frozen protocol reads that as FAIL");
  }
  if (dut_mem.exit_code() != 1) {
    Fail(ph, "the program wrote TOHOST = " + U64(dut_mem.exit_code()) +
                 "; a conforming pass writes exactly MOSAIC_PASS_CODE (1)");
  }
  if (ref.stop_reason != "the program reached its exit protocol") {
    Fail(ph, "the reference stopped for another reason (" + ref.stop_reason +
                 " at " + U64(ref.stop_pc) +
                 "), so the machine reaching the exit protocol is not the same "
                 "prediction");
  }
  reporter->Check(true, ph + ": the program reached its exit protocol (TOHOST = PASS at " +
                        U64(ref.exit_pc) + ")");

  // ---- 7. the host oracle's signature values ----
  // The expectation is the host oracle's four words. It is compared twice and
  // through two different paths:
  //
  //   * against the memory the DUT's own SIG stores wrote -- the end-to-end
  //     check, because those four stores are ordinary `sd`s that went through
  //     the store queue, the endpoint and the memory system;
  //   * against the memory the *reference interpreter* wrote while executing
  //     the same program. That second comparison is what makes the first one
  //     meaningful: if this driver's interpreter disagreed with the oracle, an
  //     agreement between the machine and the interpreter would prove nothing,
  //     and this fails instead.
  const uint64_t oracle[4] = {input.sig0, input.sig1, input.sig2, input.sig3};

  std::vector<uint64_t> ref_signature;
  if (!ref_mem.ReadSignature(&ref_signature) || ref_signature.size() != 4) {
    Fail(ph, "the reference's signature area is not readable");
  }
  for (int k = 0; k < 4; k++) {
    if (ref_signature[k] != oracle[k]) {
      Fail(ph, "the reference interpreter disagrees with the host oracle on signature "
               "word " + Dec(k) + ": " + U64(ref_signature[k]) + " vs " +
               U64(oracle[k]));
    }
  }
  reporter->Check(true, ph + ": the independent RV64IM interpreter agrees with the host "
                        "oracle on all four signature words (sig0=" + U64(oracle[0]) +
                        " sig1=" + U64(oracle[1]) + " sig2=" + U64(oracle[2]) +
                        " sig3=" + U64(oracle[3]) + ")");

  std::vector<uint64_t> signature;
  if (!dut_mem.ReadSignature(&signature) || signature.size() != 4) {
    Fail(ph, "the signature area is not readable in the memory model");
  }
  for (int k = 0; k < 4; k++) {
    if (signature[k] != oracle[k]) {
      Fail(ph, "signature word " + Dec(k) + " in memory: the host oracle computes " +
                   U64(oracle[k]) + ", the machine's stores left " + U64(signature[k]));
    }
  }
  reporter->Check(true, ph + ": the four signature words in memory equal the host "
                        "oracle's values, written by the program's own SIG stores");

  std::printf("  [run %d] a=%s b=%s c=%s: %zu retires, %u control transfers (%u taken, "
              "%u branches, %u jal, %u jalr, %u back edges), %u loads, %u stores, "
              "%s redirects, act=%s wait=%s, cycles=%s\n",
              index, U64(input.a).c_str(), U64(input.b).c_str(), U64(input.c).c_str(),
              got.size(), ref.control_total, ref.taken_total, ref.branches, ref.jal,
              ref.jalr, ref.back_edges, ref.loads, ref.stores, Dec(redirects_at_end).c_str(),
              Dec(result.act_ctr).c_str(), Dec(result.wait_ctr).c_str(),
              Dec(result.cycles).c_str());
  return result;
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

    const std::string repo = FindRepoRoot();
    const std::string elf = repo + "/tests/programs/build/p02_branch.i0.elf";

    ProgImage program;
    std::string load_detail;
    if (!program.Load(elf, &load_detail)) {
      Fail("program", load_detail + " (build the corpus with "
                                    "`make -C tests/programs all`)");
    }

    // The wrapper's reset vector is a constant of the elaborated core, and the
    // image is linked at it. Everything below assumes the two agree.
    if (geometry.reset_vector != program.entry()) {
      Fail("geometry", "the core resets to " + U64(geometry.reset_vector) +
                           ", the ELF entry is " + U64(program.entry()));
    }

    const InputPrologue prologue = FindInputPrologue(program, program.entry());
    if (!prologue.found) {
      Fail("program", "the MOSAIC_LOAD_INPUTS prologue was not found in " + elf);
    }

    // The first phase's checks go through the same named-check path as the rest.
    reporter.Check(geometry.xlen == 64, "geometry: the profile is 64-bit");
    reporter.Check(geometry.reset_vector == 0x80000000ull,
                   "geometry: the reset vector is the corpus link base");
    reporter.Check(prologue.after > prologue.at && prologue.after == prologue.at + 20,
                   "program: the branch region follows the input prologue");
    reporter.Check(program.Has(prologue.at + 16),
                   "program: the input prologue's third load is in the image");

    std::printf("core.corpus_branch: %s, entry=%s, branch region starts at %s, "
                "reset-vector brick at %s\n",
                elf.c_str(), U64(program.entry()).c_str(),
                U64(prologue.after).c_str(), U64(geometry.reset_vector).c_str());

    uint64_t total_cycles = 0;
    uint64_t total_comparisons = 0;
    uint32_t total_control = 0;
    uint32_t total_taken = 0;
    uint32_t total_branches = 0;
    uint32_t total_branch_taken = 0;
    uint32_t total_jal = 0;
    uint32_t total_jalr = 0;
    uint32_t total_back_edges = 0;
    size_t total_retires = 0;
    for (int i = 0; i < 3; i++) {
      const RunResult r = RunOnce(&dut, &reporter, options.max_cycles, program,
                                  prologue, geometry, kInputs[i], i);
      total_cycles += r.cycles;
      total_comparisons += r.comparison_count;
      total_control += r.reference.control_total;
      total_taken += r.reference.taken_total;
      total_branches += r.reference.branches;
      total_branch_taken += r.reference.branch_taken;
      total_jal += r.reference.jal;
      total_jalr += r.reference.jalr;
      total_back_edges += r.reference.back_edges;
      total_retires += r.reference.trace.size();
    }

    // The branch mix has to be a mix: a program that only ever branches one way
    // would not be evidence about the control path.
    reporter.Check(total_branch_taken > 0 &&
                       total_branches > total_branch_taken,
                   "the corpus program exercises taken and not-taken branches: " +
                       Dec(total_branch_taken) + " of " + Dec(total_branches) + " taken");
    reporter.Check(total_jal > 0 && total_jalr > 0,
                   "the corpus program exercises JAL and JALR: " + Dec(total_jal) +
                       " JAL, " + Dec(total_jalr) + " JALR");
    reporter.Check(total_back_edges > 0,
                   "the corpus program exercises back edges: " + Dec(total_back_edges));

    detail = "checks=" + Dec(reporter.checks()) + " comparisons=" +
             Dec(total_comparisons) + " cycles=" + Dec(total_cycles) + " retires=" +
             Dec(total_retires) + " control=" + Dec(total_control) + " taken=" +
             Dec(total_taken) + " branches=" + Dec(total_branches) + "(taken " +
             Dec(total_branch_taken) + ") jal=" + Dec(total_jal) + " jalr=" +
             Dec(total_jalr) + " back_edges=" + Dec(total_back_edges) + " inputs=" +
             Dec(3) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the control path holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
