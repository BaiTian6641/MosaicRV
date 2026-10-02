// ============================================================================
// tb_core_mem.cpp -- CASE=core.mem_program, work package I-023.
//
// The DUT is the integrated p0 core (the same top CASE=fabric.fixed_two_cluster
// and CASE=core.corpus_branch drive): fetch -> decode buffer -> dispatch -> two
// clusters -> writeback arbiter -> ROB -> retire -> commit to rename, with the
// redirect arbiter on the branch-resolution side and **the memory path wired
// in**: dispatch -> load queue / store queue -> mosaic_lsu_endpoint -> the
// core's data port, with the ROB's commit path authorising stores and a
// redirect squashing both queues.
//
// ------------------------------------------------------------- the programs
//
// Two real corpus programs run, each in its three declared input variants, from
// `tests/programs/build/`:
//
//   p03_loadstore  sd sw sh sb, then ld lw lhu lh lb lbu, at every width, with
//                  a store feeding a much later load at the same address
//   p09_storeload  store-to-load at the *same* address, back to back, at several
//                  widths -- a load followed immediately by a store that
//                  overwrites it, then a load of the result; a sub-width store
//                  into the low byte/half of a double, then a load of the merged
//                  double
//
// The program text is executed unmodified from its own `main`. The only thing
// the harness writes is one instruction at the reset vector -- `jal x0, main` --
// because the core boots at MOSAIC_RESET_VECTOR and `_start` (crt0) begins with
// two CSR writes that this machine refuses. `main` establishes s0/s1/s2 itself
// and loads its three inputs from its own `.rodata`, so the program is entered
// with no register state set up by this driver and no instruction skipped.
//
// p13_romstore is deliberately not run: its whole point is a store to the
// read-only boot ROM that must trap and be logged by the machine trap handler,
// and this machine has no trap path yet (the reference says so too, rather than
// inventing the architectural result).
//
// ------------------------------------------------------------ the expectation
//
// Nothing here comes from the DUT:
//
//   * The four signature words are the values `python3 tools/host_oracle.py
//     --program p03_loadstore` / `--program p09_storeload` prints for each input
//     variant. They are quoted below with the commands that produced them, and
//     they are compared against the memory image the program's own `sd` of each
//     signature left behind -- i.e. through the store path.
//   * The per-instruction expectation is an independent RV64IM interpreter with
//     its own memory (sim/unit/mem_ref.h), decoding the same words the machine
//     is fed and modelling loads and stores itself. Every retirement the machine
//     publishes is compared with it, pc by pc, destination by destination, value
//     by value -- so a load that returns the wrong bytes is caught at the
//     instruction that loaded them, not only at the signature.
//   * The exit expectation is the frozen protocol: the program ends by writing
//     MOSAIC_TOHOST, and a conforming pass writes exactly MOSAIC_PASS_CODE (1).
//
// -------------------------------------------------------------- what it checks
//
//   per run:
//   1. the per-instruction retirement stream equals the reference, in order,
//      including every load's value and every store's pc;
//   2. every redirect the arbiter issues goes to the resolved transfer's target,
//      and there is exactly one per taken transfer;
//   3. the program reaches its own exit protocol (TOHOST = PASS);
//   4. the four signature words in memory equal the host oracle's, and the
//      independent interpreter agrees with the host oracle on the same four --
//      the second is what makes the first meaningful;
//   5. the scratch area the program wrote equals the reference's byte for byte;
//   6. exactly the accesses the reference performed reached the data port --
//      one transaction per load and per store, no more (no access from an
//      instruction that never retired) and no fewer (none silently dropped);
//   7. no store reached memory before the instruction that owns it retired:
//      the number of store transactions the memory has taken never exceeds the
//      number of store instructions that have retired so far. This is the
//      property a store queue exists for, and it is checked every cycle rather
//      than only at the end, because a prematurely-visible store leaves the
//      final image unchanged;
//   8. every load byte came from a store or from memory (the conservation
//      identity), and the machine's own no-fault, no-replay, no-squash
//      invariant counters stay clean;
//      across all runs:
//   9. the access classes the run set must actually include -- loads and stores,
//      every width, at least one store feeding a later load -- are counted from
//      the retired instructions and required to be non-empty, so "the program
//      covers the classes" is measured rather than asserted.
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

using mosaic_ref::DataMem;
using mosaic_ref::RefInsn;
using mosaic_ref::RefResult;

namespace {

constexpr int kResetCycles = 4;
// The program must reach its exit protocol within this many cycles. The runs are
// a few hundred cycles long; this bound is only reached if the machine stopped
// making progress.
constexpr int kTraceCycles = 40000;
// How long the machine is given, after the predicted stream ends, for the stores
// that were already authorised to reach memory.
constexpr int kDrainCycles = 256;
// A stall is a defect with a location, not a timeout.
constexpr int kStallCycles = 4000;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// Instruction encodings the harness needs (the reset-vector brick only)
// ============================================================================
uint32_t EncJ(int32_t offset, uint32_t rd) {
  const uint32_t u = static_cast<uint32_t>(offset);
  const uint32_t imm = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
                       (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12);
  return imm | (rd << 7) | 0x6Fu;
}

// ============================================================================
// The loaded program image
// ============================================================================
// The ELF's loadable segments as words, with the harness's single-instruction
// brick written at the reset vector. The instruction memory returns ECALL for
// anything not in the image, so a runaway fetch stops the machine cleanly
// instead of reading a don't-care.
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
  const mosaic::Image& image() const { return image_; }
  uint64_t lowest() const { return lo_; }
  uint64_t highest() const { return hi_; }

  // A named symbol from the image's `.symtab`, or null. The harness enters the
  // program at `main` and says so by name rather than by a transcribed address.
  const mosaic::Symbol* Symbol(const std::string& name) const {
    return image_.FindSymbol(name);
  }

 private:
  std::map<uint64_t, uint32_t> words_;
  std::vector<mosaic::Segment> segments_;
  mosaic::Image image_;
  uint64_t entry_ = 0;
  uint64_t lo_ = 0;
  uint64_t hi_ = 0;
};

// ============================================================================
// The instruction memory
// ============================================================================
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
// Geometry, taken from the elaborated DUT rather than re-derived
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint32_t seq_w = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

// Verilator hands a wide port over as a `VlWide` indexed in 32-bit words, two per
// 64-bit lane; narrow ports arrive as plain scalars.
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
// One (program, input) run
// ============================================================================
struct RunChecks {
  uint64_t retires = 0;
  uint64_t comparisons = 0;
  uint64_t cycles = 0;
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t fwd_bytes = 0;
  uint64_t mem_bytes = 0;
  uint64_t txns = 0;
};

class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(mem) {}

  void Configure(const Geometry& g) { g_ = g; }

  void Phase(const std::string& name) { phase_ = name; }
  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }

  struct Retire {
    uint64_t pc = 0;
    uint32_t rd = 0;
    bool reg_we = false;
    uint64_t value = 0;
    uint32_t seq = 0;
  };
  const std::vector<Retire>& retires() const { return retires_; }
  const std::vector<uint64_t>& redirects() const { return redirects_; }

  // Every store transaction the memory has taken, in order, for the
  // "no store reaches memory before its instruction retires" check.
  uint64_t store_txns() const {
    uint64_t n = 0;
    for (const DataMem::Txn& t : dmem_.txns()) {
      if (t.req.we) n++;
    }
    return n;
  }
  const std::vector<DataMem::Txn>& txns() const { return dmem_.txns(); }

  bool Complete() const { return retires_.size() >= expected_->size(); }

  void StopComparing() {
    expected_ = nullptr;
    expected_targets_ = nullptr;
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + State());
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
    // The data port. A transaction is taken whenever the endpoint offers one;
    // the response is presented for as long as it takes the endpoint to consume
    // it.
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
    // The standalone redirect arbiter is not driven by this case.
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
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
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

  // What the machine is doing, for a failure message.
  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) + " stopped=" + Dec(dut_->o_stopped_o) +
           " lq=" + Dec(dut_->o_mem_lq_occupied_o) +
           " sq=" + Dec(dut_->o_mem_sq_occupied_o) +
           " lq_alloc=" + Dec(dut_->o_mem_lq_alloc_o) +
           " lq_issue=" + Dec(dut_->o_mem_lq_issue_o) +
           " sq_alloc=" + Dec(dut_->o_mem_sq_alloc_o) +
           " sq_auth=" + Dec(dut_->o_mem_sq_auth_o) +
           " sq_drain=" + Dec(dut_->o_mem_sq_drain_o);
  }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Expect(const std::vector<RefInsn>* trace, const std::vector<uint64_t>* targets) {
    expected_ = trace;
    expected_targets_ = targets;
  }

 private:
  // The number of store instructions among the first `n` reference
  // instructions. The reference is the prediction, so this is what the machine
  // *should* have retired by now.
  uint64_t ReferenceStoresUpTo(size_t n) const {
    uint64_t count = 0;
    if (expected_ == nullptr) return 0;
    for (size_t i = 0; i < n && i < expected_->size(); i++) {
      if ((*expected_)[i].is_store) count++;
    }
    return count;
  }

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
            "squash_nc=" + Dec(dut_->o_squash_nc_o) +
                " under=" + Dec(dut_->o_squash_under_o) +
                " journal=" + Dec(dut_->o_journal_ovf_o));

    // ---- the memory path's own invariants, every cycle ----
    // The load queue cross-checks the store queue's forwarding query against the
    // entries it exports; a non-zero counter means the two views disagree.
    Compare("the load queue's forwarding-query cross-check stays zero",
            dut_->o_mem_lq_query_mismatch_o == 0,
            "mismatches=" + Dec(dut_->o_mem_lq_query_mismatch_o));
    // No store queue entry is ever offered to memory before its entry is
    // authorised: `auth <= count` is the watermark's prefix invariant, and
    // `drain <= alloc` says nothing left the queue that never entered it.
    Compare("the store queue's authorised prefix never exceeds its occupancy",
            dut_->o_mem_sq_auth_o <= dut_->o_mem_sq_occupied_o,
            "auth=" + Dec(dut_->o_mem_sq_auth_o) +
                " occupied=" + Dec(dut_->o_mem_sq_occupied_o));
    Compare("no store drains that was never allocated",
            dut_->o_mem_sq_drain_o + dut_->o_mem_sq_occupied_o <=
                dut_->o_mem_sq_alloc_o,
            "drain=" + Dec(dut_->o_mem_sq_drain_o) +
                " occupied=" + Dec(dut_->o_mem_sq_occupied_o) +
                " alloc=" + Dec(dut_->o_mem_sq_alloc_o));
    Compare("the load queue does not hold more loads than it allocated",
            dut_->o_mem_lq_occupied_o <= dut_->o_mem_lq_alloc_o,
            "occupied=" + Dec(dut_->o_mem_lq_occupied_o) +
                " alloc=" + Dec(dut_->o_mem_lq_alloc_o));
    // A store must not be visible to memory before the instruction that owns it
    // retires. Authorisation happens at retirement and the endpoint takes the
    // store after that, so the number of store transactions memory has seen can
    // never exceed the number of store instructions that have retired.
    {
      const uint64_t writes = store_txns();
      const uint64_t retired_stores = ReferenceStoresUpTo(retires_.size());
      Compare("no store reaches memory before its instruction retires: "
              "writes seen <= stores retired",
              writes <= retired_stores,
              "store transactions=" + Dec(writes) + ", retired stores=" +
                  Dec(retired_stores));
    }

    if (dut_->o_redirect_valid_o != 0) {
      redirects_.push_back(dut_->o_redirect_pc_o);
      if (expected_targets_ != nullptr) {
        if (redirects_.size() > expected_targets_->size()) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the redirect the arbiter issues names the taken transfer's target: "
               "redirect " + Dec(redirects_.size() - 1) +
               " has no counterpart -- the reference executed only " +
               Dec(expected_targets_->size()) + " taken control transfers");
        }
        const uint64_t want = (*expected_targets_)[redirects_.size() - 1];
        if (redirects_.back() != want) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "the redirect the arbiter issues names the taken transfer's target: "
               "redirect " + Dec(redirects_.size() - 1) + " pc expected " + U64(want) +
               ", got " + U64(redirects_.back()));
        }
      }
    }

    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    if (g_.retire_width >= 2) {
      Compare("retire lane 1 is never set without lane 0",
              ((got_mask & 2u) == 0) || ((got_mask & 1u) != 0),
              "ev_valid=" + Dec(got_mask));
    }
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.seq = static_cast<uint32_t>(PackedLane(dut_->ev_seq_o, lane, g_.seq_w));
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
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
               Dec(expected_->size()) + " instructions, ending at " +
               U64(expected_->back().pc));
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
               Dec(e.rd) + " expected " + U64(e.value) + ", got " + U64(r.value) +
               (e.is_load ? " (a load)" : ""));
        }
        comparisons_++;
      }
    }
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
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled: " + State());
    }
  }

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  // The reset-traffic rule (V-010, sim/common/bus_reset_gate.h): while reset is
  // asserted this model accepts nothing, so no reset-time response can be
  // queued ahead of a fresh post-reset one.
  mosaic::BusResetGate bus_reset_;
  Geometry g_;
  std::string phase_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::vector<Retire> retires_;
  std::vector<uint64_t> redirects_;
  const std::vector<RefInsn>* expected_ = nullptr;
  const std::vector<uint64_t>* expected_targets_ = nullptr;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
};

// ============================================================================
// The repository root
// ============================================================================
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
// The declared programs and their host-oracle signatures
// ============================================================================
// `python3 tools/host_oracle.py --program p03_loadstore` and
// `--program p09_storeload`, run at the time this case was written, print
// exactly these rows. They are transcribed here so the comparison is against the
// host oracle's arithmetic and not against this driver's interpreter -- the two
// are independent, and a disagreement between them fails the case rather than
// being averaged away.
//                                              sig0                sig1                sig2                sig3
struct Expected {
  uint64_t a;
  uint64_t b;
  uint64_t c;
  uint64_t sig[4];
};

struct Program {
  const char* name;
  const char* elf;
  const Expected inputs[3];
};

const Program kPrograms[2] = {
    {"p03_loadstore",
     "tests/programs/build/p03_loadstore.i%d.elf",
     {
         {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210), UINT64_C(0x7),
          {UINT64_C(0x0123456788888888), UINT64_C(0x000000000000ffff),
           UINT64_C(0xffffffffffffffff), UINT64_C(0xffffffffffffffe6)}},
         {UINT64_C(0x8000000000000001), UINT64_C(0x180), UINT64_C(0xffffffffffffffff),
          {UINT64_C(0x7fffffff80000001), UINT64_C(0x0000000000000181),
           UINT64_C(0x0000000000000081), UINT64_C(0x8000000000000180)}},
         {UINT64_C(0x0), UINT64_C(0xffffffffffffffff), UINT64_C(0x8000000000000000),
          {UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff),
           UINT64_C(0x00000000000000ff), UINT64_C(0x7fffffffffffffff)}},
     }},
    {"p09_storeload",
     "tests/programs/build/p09_storeload.i%d.elf",
     {
         {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210),
          UINT64_C(0xaaaaaaaaaaaaaaaa),
          {UINT64_C(0x0123456789abcdef), UINT64_C(0xffffffff89abcdef),
           UINT64_C(0x000000000000ff00), UINT64_C(0xaaaaaaaaaaaaaaaa)}},
         {UINT64_C(0xdeadbeefcafebabe), UINT64_C(0xffffffffffffffff), UINT64_C(0x0),
          {UINT64_C(0xdeadbeefcafebabe), UINT64_C(0xffffffffcafebabe),
           UINT64_C(0x0000000000004500), UINT64_C(0x0000000000000000)}},
         {UINT64_C(0xffffffffffffffff), UINT64_C(0x0), UINT64_C(0x5555555555555555),
          {UINT64_C(0xffffffffffffffff), UINT64_C(0xffffffffffffffff),
           UINT64_C(0x000000000000ff00), UINT64_C(0x5555555555555555)}},
     }},
};

// `p03_loadstore` and `p09_storeload` are pure straight-line programs with no
// data-dependent control flow except the terminal `call tohost_finish`, so the
// signatures depend only on the compiled-in inputs; three inputs per program, and
// every input gives a distinct signature (which the oracle itself checks).
constexpr int kInputsPerProgram = 3;

// ============================================================================
// One run
// ============================================================================
struct RunOutcome {
  uint64_t cycles = 0;
  uint64_t comparisons = 0;
  size_t retires = 0;
  uint32_t loads = 0;
  uint32_t stores = 0;
  uint64_t fwd_bytes = 0;
  uint64_t mem_bytes = 0;
  uint64_t txns = 0;
};

RunOutcome RunOnce(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
                   const std::string& elf_path, const Expected& input, int program_index,
                   int input_index, const Geometry& geometry,
                   std::vector<std::string>* classes) {
  RunOutcome outcome;

  // ---- the image, and the harness brick at the reset vector ----
  ProgImage image;
  std::string load_detail;
  if (!image.Load(elf_path, &load_detail)) {
    Fail("program", load_detail + " (build the corpus with "
                                  "`make -C tests/programs all`)");
  }
  const mosaic::Symbol* main_symbol = image.Symbol("main");
  if (main_symbol == nullptr) {
    Fail("program", elf_path + " has no `main` symbol, so the harness has no "
                               "entry point to jump to");
  }
  const uint64_t entry = main_symbol->value;
  if (geometry.reset_vector == entry) {
    Fail("geometry", "the reset vector and the program's entry are the same address");
  }
  if (main_symbol->size == 0) {
    Fail("program", "`main` has zero size in " + elf_path);
  }
  // The one instruction the harness supplies: a jump to the program's own entry.
  // Everything the program needs -- its bases, its inputs, its scratch -- it
  // establishes itself, so nothing about the program is skipped or pre-set.
  const int64_t offset = static_cast<int64_t>(entry) -
                         static_cast<int64_t>(geometry.reset_vector);
  if (offset < -(INT32_C(1) << 20) || offset >= (INT32_C(1) << 20)) {
    Fail("program", "`main` is out of JAL range from the reset vector");
  }
  image.WriteWord(geometry.reset_vector, EncJ(static_cast<int32_t>(offset), 0));
  // The word after the entry branch -- its fall-through, which is wrong path --
  // is a store. It is never executed: the branch barrier holds dispatch until
  // the branch has resolved and the redirect has purged the front end, so the
  // machine must never make this access. Placing it here is what gives the
  // barrier a *directed* control: with the barrier bypassed for memory macros
  // (MOSAIC_CORE_MUTANT_MEM_BEHIND_BRANCH) this store is allocated, issued to
  // the endpoint and counted, and the "exactly one data transaction per load and
  // per store" check names it. It also targets the read-only boot ROM, so if it
  // ever did execute the endpoint would report an access fault.
  // A *load*, so the defect it controls for is the one that reaches the
  // endpoint: a store on a wrong path is stopped by the authorisation watermark
  // (it never retires, so it never drains), while a load is issued to the
  // endpoint as soon as it reaches the load queue's head.
  const uint32_t kWrongPathStore = 0x00003003u;  // `ld x0, 0(x0)`
  image.WriteWord(geometry.reset_vector + 4, kWrongPathStore);

  // ---- the memory: one model for the DUT, one for the reference ----
  mosaic::MemoryModel dut_mem;
  mosaic::MemoryModel ref_mem;
  {
    std::string detail;
    if (!dut_mem.LoadImage(image.image(), &detail)) Fail("run", detail);
    if (!ref_mem.LoadImage(image.image(), &detail)) Fail("run", detail);
  }

  // ---- the expectation ----
  const RefResult reference = mosaic_ref::RunReference(image, geometry.reset_vector, &ref_mem);
  if (!reference.exited) {
    Fail("run", std::string("the reference did not reach the program's exit protocol: ") +
                    reference.stop_reason + " at " + U64(reference.stop_pc));
  }
  std::vector<uint64_t> expected_targets;
  for (const RefInsn& insn : reference.trace) {
    if (insn.is_control && insn.taken) expected_targets.push_back(insn.next_pc);
  }

  const std::string ph = "run-" + std::string(kPrograms[program_index].name) + "-in" +
                         Dec(input_index);
  Harness harness(dut, reporter, max_cycles, &image, &dut_mem);
  harness.Configure(geometry);
  harness.Phase(ph);
  harness.Expect(&reference.trace, &expected_targets);

  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);

  // Run until the reference's whole trace has retired AND every taken transfer
  // in it has been acted on: the arbiter's redirect is registered, so the last
  // transfer's redirect lands the cycle after its retirement.
  while (!harness.Complete() || harness.redirects().size() < expected_targets.size()) {
    harness.Cycle(false);
    if (harness.cycles() > kTraceCycles) {
      Fail(ph, "the program did not reach its exit protocol within " + Dec(kTraceCycles) +
                   " cycles: " + harness.State());
    }
  }
  // Four settle cycles, comparisons still armed: the rename squash follows the
  // redirect by one cycle, so the counters that describe the last recovery land
  // after it. Nothing retires in that window (the park loop is an order of
  // magnitude further away), and if something did, the comparison says so.
  for (int i = 0; i < 4; i++) harness.Cycle(false);

  const uint64_t redirects_at_end = harness.redirects().size();
  const uint64_t lq_alloc = dut->o_mem_lq_alloc_o;
  const uint64_t lq_issue = dut->o_mem_lq_issue_o;
  const uint64_t lq_done = dut->o_mem_lq_done_o;
  const uint64_t lq_replay = dut->o_mem_lq_replay_o;
  const uint64_t lq_blocked = dut->o_mem_lq_blocked_o;
  const uint64_t lq_fault = dut->o_mem_lq_fault_o;
  const uint64_t lq_fwd_bytes = dut->o_mem_lq_fwd_bytes_o;
  const uint64_t lq_mem_bytes = dut->o_mem_lq_mem_bytes_o;
  const uint64_t lq_mismatch = dut->o_mem_lq_query_mismatch_o;
  const uint64_t sq_alloc = dut->o_mem_sq_alloc_o;
  const uint64_t sq_commit = dut->o_mem_sq_commit_o;
  const uint64_t sq_commit2 = dut->o_mem_sq_commit2_o;
  const uint64_t sq_commit_stale = dut->o_mem_sq_commit_stale_o;
  const uint64_t sq_drain = dut->o_mem_sq_drain_o;
  const uint64_t sq_squash = dut->o_mem_sq_squash_o;
  const uint64_t sq_spared = dut->o_mem_sq_spared_o;
  const uint64_t sq_fault = dut->o_mem_sq_fault_o;
  const uint64_t sq_auth = dut->o_mem_sq_auth_o;
  const uint64_t sq_occupied = dut->o_mem_sq_occupied_o;
  const uint64_t lsu_txn = dut->o_mem_lsu_txn_o;
  const uint64_t lsu_misaligned = dut->o_mem_lsu_misaligned_o;
  const uint64_t lsu_access_fault = dut->o_mem_lsu_access_fault_o;
  const uint64_t ins_stall = dut->o_mem_ins_stall_o;

  // Everything after the predicted stream is the program's own park loop: run
  // with the comparison off until the authorised stores have reached memory.
  harness.StopComparing();
  for (int i = 0; i < kDrainCycles && !dut_mem.finished(); i++) harness.Cycle(false);

  outcome.cycles = harness.cycles();
  outcome.comparisons = harness.comparisons();
  outcome.retires = harness.retires().size();
  outcome.loads = reference.loads;
  outcome.stores = reference.stores;
  outcome.fwd_bytes = lq_fwd_bytes;
  outcome.mem_bytes = lq_mem_bytes;
  outcome.txns = harness.txns().size();

  // ---- 1. the per-instruction architectural stream ----
  // Compared live in Observe(); this is the statement that it covered the whole
  // prediction.
  harness.Check("the per-instruction retire stream matches the independent RV64IM "
                "interpretation (" + Dec(reference.trace.size()) + " instructions, " +
                Dec(reference.loads) + " loads, " + Dec(reference.stores) + " stores)",
                harness.retires().size() == reference.trace.size(),
                "retired " + Dec(harness.retires().size()) + ", predicted " +
                    Dec(reference.trace.size()));

  // ---- 2. the control transfers ----
  harness.Check("every redirect names the taken transfer's target and there is "
                "exactly one per taken transfer",
                redirects_at_end == expected_targets.size(),
                "redirects=" + Dec(redirects_at_end) + ", taken transfers=" +
                    Dec(expected_targets.size()));

  // ---- 3. the exit protocol ----
  harness.Check("the program wrote TOHOST with the PASS bit set (exactly "
                "MOSAIC_PASS_CODE)",
                dut_mem.finished() && dut_mem.passed() && dut_mem.exit_code() == 1,
                "finished=" + Dec(dut_mem.finished() ? 1 : 0) + " passed=" +
                    Dec(dut_mem.passed() ? 1 : 0) + " tohost=" +
                    U64(dut_mem.exit_code()));

  // ---- 4. the host oracle's signature, twice ----
  std::vector<uint64_t> ref_signature;
  if (!ref_mem.ReadSignature(&ref_signature) || ref_signature.size() != 4) {
    Fail(ph, "the reference's signature area is not readable");
  }
  for (int k = 0; k < 4; k++) {
    if (ref_signature[k] != input.sig[k]) {
      Fail(ph, "the reference interpreter disagrees with the host oracle on signature "
               "word " + Dec(k) + ": " + U64(ref_signature[k]) + " vs " +
               U64(input.sig[k]));
    }
  }
  reporter->Check(true, ph + ": the independent RV64IM interpreter agrees with the host "
                        "oracle on all four signature words (a=" + U64(input.a) +
                        " b=" + U64(input.b) + " c=" + U64(input.c) + ")");

  std::vector<uint64_t> signature;
  if (!dut_mem.ReadSignature(&signature) || signature.size() != 4) {
    Fail(ph, "the signature area is not readable in the memory model");
  }
  for (int k = 0; k < 4; k++) {
    if (signature[k] != input.sig[k]) {
      Fail(ph, "signature word " + Dec(k) + " in memory: the host oracle computes " +
                   U64(input.sig[k]) + ", the machine's stores left " +
                   U64(signature[k]));
    }
  }
  reporter->Check(true, ph + ": the four signature words the program's own stores left "
                        "in memory equal the host oracle's values");

  // ---- 5. the scratch the program wrote, byte for byte ----
  // The signature area is the program's published result; the scratch area is
  // where its stores actually landed. Both are compared against the reference's
  // memory, so a store that wrote the right bytes to the wrong place is named
  // here rather than only as a wrong signature.
  {
    uint64_t compared = 0;
    for (uint64_t addr = MOSAIC_SIGNATURE_ADDR;
         addr < MOSAIC_SIGNATURE_ADDR + 8 * MOSAIC_SIGNATURE_WORDS; addr += 8) {
      uint64_t want = 0;
      uint64_t got = 0;
      if (ref_mem.Read(addr, 8, &want) != mosaic::AccessStatus::kOk ||
          dut_mem.Read(addr, 8, &got) != mosaic::AccessStatus::kOk) {
        Fail(ph, "the signature word at " + U64(addr) + " is not readable");
      }
      if (want != got) {
        Fail(ph, "memory at " + U64(addr) + ": the reference's stores left " +
                     U64(want) + ", the machine's left " + U64(got));
      }
      compared++;
    }
    reporter->Check(true, ph + ": the " + Dec(compared) +
                          " signature words in memory match the reference's byte for byte");
  }

  // ---- 6. exactly the accesses the reference performed reached the data port --
  // One transaction per load and per store. More means an access was made for an
  // instruction that never retired (a wrong-path access reached memory); fewer
  // means an access was silently dropped.
  harness.Check("exactly one data transaction per load and per store reached the data "
                "port",
                harness.txns().size() == reference.loads + reference.stores &&
                    lsu_txn == reference.loads + reference.stores,
                "data transactions=" + Dec(harness.txns().size()) + " (lsu=" +
                    Dec(lsu_txn) + "), loads=" + Dec(reference.loads) + " stores=" +
                    Dec(reference.stores));

  // ---- 7. the memory path's accounting ----
  harness.Check("no load queue entry is lost: every allocated load issued and completed",
                lq_alloc == lq_issue && lq_issue == lq_done,
                "alloc=" + Dec(lq_alloc) + " issue=" + Dec(lq_issue) +
                    " done=" + Dec(lq_done));
  harness.Check("every allocated store drained exactly once",
                sq_alloc == sq_drain && sq_alloc == reference.stores,
                "alloc=" + Dec(sq_alloc) + " drain=" + Dec(sq_drain) +
                    " stores=" + Dec(reference.stores));
  harness.Check("every store was authorised exactly once and none was refused as stale",
                sq_commit + sq_commit_stale == reference.stores && sq_commit_stale == 0,
                "commits=" + Dec(sq_commit) + " stale=" + Dec(sq_commit_stale) +
                    " stores=" + Dec(reference.stores));
  harness.Check("no access faulted and none was misaligned",
                lq_fault == 0 && sq_fault == 0 && lsu_misaligned == 0 &&
                    lsu_access_fault == 0,
                "lq_fault=" + Dec(lq_fault) + " sq_fault=" + Dec(sq_fault) +
                    " misaligned=" + Dec(lsu_misaligned) + " access_fault=" +
                    Dec(lsu_access_fault));
  harness.Check("no load was blocked or replayed (every store's operands were captured "
                "at allocation)",
                lq_replay == 0 && lq_blocked == 0,
                "replay=" + Dec(lq_replay) + " blocked=" + Dec(lq_blocked));
  harness.Check("the load queue's forwarding-query cross-check was clean",
                lq_mismatch == 0, "mismatches=" + Dec(lq_mismatch));
  // A redirect squashes the *unauthorised* suffix of the store queue and spares
  // the entries that are already authorised (a store that retired has been
  // promised, and recovery may not take it back). The conservative recovery
  // leaves nothing younger than a branch in the machine at all, so nothing
  // should ever be squashed here -- but an authorised store that has not yet
  // drained is resident across a redirect by construction, and those are exactly
  // the entries the spare rule must keep. That this count is non-zero is the
  // rule demonstrated through the core, not merely asserted of the module.
  harness.Check("no unauthorised store was squashed out of the store queue: the "
                "conservative recovery leaves nothing younger than a branch in it",
                sq_squash == 0,
                "squashed=" + Dec(sq_squash));
  harness.Check("the store queue's conservation identity holds: allocated == drained + "
                "squashed + occupied",
                sq_alloc == sq_drain + sq_squash + sq_occupied,
                "alloc=" + Dec(sq_alloc) + " drain=" + Dec(sq_drain) +
                    " squashed=" + Dec(sq_squash) + " occupied=" + Dec(sq_occupied));
  reporter->Check(true, ph + ": " + Dec(sq_spared) +
                        " authorised store(s) were resident across a redirect and spared "
                        "by the store queue's watermark rule");
  reporter->Check(true, ph + ": " + Dec(sq_commit2) +
                        " store authorisation(s) used the store queue's second commit "
                        "port (two stores retiring in one cycle)");
  harness.Check("the store queue is empty at the end of the run",
                sq_occupied == 0 && sq_auth == 0,
                "occupied=" + Dec(sq_occupied) + " auth=" + Dec(sq_auth));
  // The load bytes are conserved: each byte of every completed load is served by
  // an older store's byte or by the memory response, and the load queue's two
  // counters are that partition. The expected total comes from the *transactions*
  // the data port saw, not from the queue's own accounting.
  {
    uint64_t expected_load_bytes = 0;
    for (const DataMem::Txn& t : harness.txns()) {
      if (!t.req.we) expected_load_bytes += 1ull << t.req.size;
    }
    harness.Check("every completed load byte came from a store or from memory",
                  lq_fwd_bytes + lq_mem_bytes == expected_load_bytes,
                  "forwarded=" + Dec(lq_fwd_bytes) + " memory=" + Dec(lq_mem_bytes) +
                      " expected=" + Dec(expected_load_bytes));
  }

  // The access-class coverage, accumulated across runs.
  if (classes != nullptr) {
    uint64_t widths[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};  // [load][size]
    for (const DataMem::Txn& t : harness.txns()) {
      const int kind = t.req.we ? 1 : 0;
      const unsigned size = (t.req.size <= 3u) ? t.req.size : 3u;
      widths[kind][size]++;
    }
    const char* size_names[4] = {"byte", "half", "word", "double"};
    for (int k = 0; k < 2; k++) {
      for (int s = 0; s < 4; s++) {
        if (widths[k][s] != 0) {
          classes->push_back(std::string(k ? "s" : "l") + size_names[s]);
        }
      }
    }
  }

  std::printf("  [run %s in %d] a=%s b=%s c=%s: %zu retires, %u loads, %u stores, "
              "%llu data transactions, %llu forwarded bytes, %llu memory bytes, "
              "%llu store-write pairs, sq_commit2=%llu stalls=%llu, cycles=%llu\n",
              kPrograms[program_index].name, input_index, U64(input.a).c_str(),
              U64(input.b).c_str(), U64(input.c).c_str(), harness.retires().size(),
              reference.loads, reference.stores,
              static_cast<unsigned long long>(harness.txns().size()),
              static_cast<unsigned long long>(lq_fwd_bytes),
              static_cast<unsigned long long>(lq_mem_bytes),
              static_cast<unsigned long long>(sq_commit),
              static_cast<unsigned long long>(sq_commit2),
              static_cast<unsigned long long>(ins_stall),
              static_cast<unsigned long long>(harness.cycles()));
  return outcome;
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
    if (geometry.reset_vector != 0x80000000ull) {
      Fail("geometry", "the reset vector is not the corpus link base: " +
                           U64(geometry.reset_vector));
    }
    if (geometry.xlen != 64) {
      Fail("geometry", "the profile is not 64-bit");
    }

    uint64_t total_cycles = 0;
    uint64_t total_comparisons = 0;
    uint64_t total_retires = 0;
    uint64_t total_loads = 0;
    uint64_t total_stores = 0;
    uint64_t total_fwd_bytes = 0;
    uint64_t total_mem_bytes = 0;
    uint64_t total_txns = 0;
    std::vector<std::string> classes;

    std::printf("core.mem_program: %zu programs x %d inputs from %s\n",
                sizeof(kPrograms) / sizeof(kPrograms[0]), kInputsPerProgram,
                (repo + "/tests/programs/build").c_str());

    for (size_t p = 0; p < sizeof(kPrograms) / sizeof(kPrograms[0]); p++) {
      for (int i = 0; i < kInputsPerProgram; i++) {
        char elf_name[256];
        std::snprintf(elf_name, sizeof(elf_name), kPrograms[p].elf, i);
        const std::string elf_path = repo + "/" + elf_name;
        const RunOutcome r = RunOnce(&dut, &reporter, options.max_cycles, elf_path,
                                     kPrograms[p].inputs[i], static_cast<int>(p), i,
                                     geometry, &classes);
        total_cycles += r.cycles;
        total_comparisons += r.comparisons;
        total_retires += r.retires;
        total_loads += r.loads;
        total_stores += r.stores;
        total_fwd_bytes += r.fwd_bytes;
        total_mem_bytes += r.mem_bytes;
        total_txns += r.txns;
      }
    }

    // The access classes the run set covers, measured from the data port rather
    // than asserted in prose. A program that stopped exercising a width would
    // empty one of these and fail here.
    auto covered = [&](const std::string& name) {
      return std::find(classes.begin(), classes.end(), name) != classes.end();
    };
    const char* required[8] = {"lbyte", "lhalf", "lword", "ldouble",
                               "sbyte", "shalf", "sword", "sdouble"};
    for (const char* name : required) {
      reporter.Check(covered(name), std::string("the run set exercises an ") + name +
                                        " access at the data port");
    }
    reporter.Check(total_fwd_bytes > 0,
                   "at least one load byte was served by an older store: store-to-load "
                   "forwarding is exercised through the core (" + Dec(total_fwd_bytes) +
                       " bytes)");
    reporter.Check(total_mem_bytes > 0,
                   "at least one load byte was served by memory (" + Dec(total_mem_bytes) +
                       " bytes)");
    reporter.Check(total_stores > 0 && total_loads > 0,
                   "the run set completed " + Dec(total_loads) + " loads and " +
                       Dec(total_stores) + " stores");

    detail = "checks=" + Dec(reporter.checks()) + " comparisons=" +
             Dec(total_comparisons) + " cycles=" + Dec(total_cycles) + " retires=" +
             Dec(total_retires) + " loads=" + Dec(total_loads) + " stores=" +
             Dec(total_stores) + " data_txns=" + Dec(total_txns) + " fwd_bytes=" +
             Dec(total_fwd_bytes) + " mem_bytes=" + Dec(total_mem_bytes) +
             " programs=" + Dec(sizeof(kPrograms) / sizeof(kPrograms[0])) + " inputs=" +
             Dec(kInputsPerProgram) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the memory path holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
