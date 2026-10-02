// ============================================================================
// tb_core_fabric.cpp -- CASE=fabric.integrated, work package I-090 (V-034).
//
// The card: wire the verified fabric modules into the live core one at a time
// and *measure* the fixed-versus-dynamic comparison on equal resources. This
// driver is the measurement.
//
// ------------------------------------------------------------- what it runs
//
// The same corpus ELF, from the same image, is run **twice** through the same
// elaborated core: once with the fabric off (`fab_dyn_i` low: dispatch's
// alternating cluster affinity, rename's allocator without the I-032 bank
// preference, no local bypass) and once with it on (`fab_dyn_i` high: the I-029
// steering policy routes every cluster-bound macro, the I-032 bank preference
// is driven by that routing decision, and the I-027 bypass offers its issue
// queue an earlier value-visible wakeup). One toggle, one program, one seed.
//
// Nothing else differs: the image, the reset, the memory model, the cycle
// budget. The two runs are therefore comparable, and everything the second one
// does is a *choice the first one could have made too*.
//
// ------------------------------------------------------------- what it checks
//
//   1. the two configurations compute the same architecture. The whole retire
//      event stream -- sequence, PC, instruction, length, identity, register
//      write, value, store address/data/size, CSR write, trap cause/tval -- is
//      compared event by event, and the four signature words the program
//      publishes are compared word by word. Dynamic scheduling may change
//      *when* and *where* work executes; it must never change *what* is
//      computed.
//   2. the dynamic configuration actually changed something measurable. The
//      per-unit issue counts or the cycle count must differ between the two
//      runs. A "dynamic" mode that never steers is the card's fail mode, and
//      this check is what makes it a failure instead of a passing no-op.
//   3. the fabric is actually doing the work it claims: the dynamic run's
//      trading router grants a cluster-bound macro and records which key
//      decided; the bypass taps producers and resolves operands early; the
//      fixed run's router is idle and its bypass is empty. A run that reports a
//      difference the toggle cannot have caused fails here.
//   4. the machine's invariants still hold with the fabric enabled: the
//      divergent-recovery counters stay zero, the ROB occupancy stays within
//      the ROB and drains, the load queue's forwarding-query cross-check stays
//      zero, the store queue's authorised prefix never exceeds its occupancy,
//      and the committed-instruction count equals the retire events the port
//      published.
//
// ---------------------------------------------------------------- the toggle
//
// `--control-fixed-is-dynamic` runs the *fixed* configuration through the
// dynamic path, which is the measurement control: the two runs are then
// identical by construction and check 2 must fail, proving the comparison is
// falsifiable rather than merely present. It is not part of the registered run.
//
// `--image <path>` selects another corpus image; the default is the branch /
// ALU program `p02_branch.i0.elf`, which both routes enough cluster-bound
// macros for a distribution to be measurable and carries dependent chains for
// the bypass to resolve.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"

#include "elf_loader.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic_ref::DataMem;

namespace {

// The reset length and settle window every other core case uses, so "cycle N"
// means the same thing here.
constexpr int kResetCycles = 4;
constexpr int kSettleCycles = 8;
// No commit and no allocation for this long is a hang with a location, not a
// timeout. A corpus program's crt0 makes visible progress almost every cycle.
constexpr uint64_t kStallCycles = 50000;

// The number of routable targets the steering's capability matrix describes
// (the two clusters, the shared MUL/DIV+FP route and the LSU).
constexpr uint32_t kFabUnits = 4;
// The number of `grant_reason` values the steering can report.
constexpr uint32_t kFabReasons = 6;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// The loaded program image
// ============================================================================
// The ELF's loadable words, plus a view of the raw bytes for the shared memory
// model. An address the image does not cover answers ECALL, so a runaway fetch
// reaches a system instruction instead of a don't-care.
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
    elf_ = image;
    entry_ = image.entry;
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
      for (uint64_t i = 0; i < seg.filesz; ++i) {
        bytes_[seg.vaddr + i] = seg.data[i];
      }
    }
    return !words_.empty();
  }

  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x00000073u : it->second;
  }

  uint64_t entry() const { return entry_; }
  const mosaic::Image& elf() const { return elf_; }
  const std::map<uint64_t, uint8_t>& bytes() const { return bytes_; }

 private:
  std::map<uint64_t, uint32_t> words_;
  std::map<uint64_t, uint8_t> bytes_;
  mosaic::Image elf_;
  uint64_t entry_ = 0;
};

// ============================================================================
// The instruction memory (one outstanding request, fixed latency)
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
  }

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

 private:
  struct Entry {
    Request req;
    int left = 0;
  };
  const ProgImage* img_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
};

// ============================================================================
// Geometry, read from the elaborated DUT
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint64_t reset_vector = 0;
  uint32_t seq_w = 0;       // per-lane width of the retire sequence field
  uint32_t ret_id_w = 0;    // per-lane width of the retire identity field
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.ret_id_w = dut->o_geom_ret_id_w_o;
  return g;
}

// A 64-bit lane of a wide port: Verilator lays a wide signal out as 32-bit
// words, two per 64-bit lane.
template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return static_cast<uint64_t>(wide[lane * 2]) |
         (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
}

// A packed scalar field: lane `lane` of a `width`-bit-per-lane vector.
uint64_t ScalarField(uint64_t value, uint32_t lane, uint32_t width) {
  if (width == 0) return 0;
  const uint64_t shift = static_cast<uint64_t>(lane) * width;
  const uint64_t mask = (width >= 64) ? ~UINT64_C(0) : ((UINT64_C(1) << width) - 1);
  return (value >> shift) & mask;
}

// The i-th 32-bit word of a wide (VlWide) observation port.
template <typename Wide>
uint32_t WideWord(const Wide& wide, uint32_t index) {
  return static_cast<uint32_t>(wide[index]);
}

std::string FindRepoRoot() {
  std::string dir = ".";
  for (int depth = 0; depth < 8; ++depth) {
    std::FILE* probe = std::fopen((dir + "/config/profiles/p0.json").c_str(), "r");
    if (probe != nullptr) {
      std::fclose(probe);
      return dir;
    }
    dir += "/..";
  }
  return std::string();
}

// ============================================================================
// One retired instruction, as the frozen architectural event port published it
// ============================================================================
struct RetireRec {
  uint64_t seq = 0;
  uint64_t pc = 0;
  uint64_t insn = 0;
  uint64_t len = 0;
  uint64_t id = 0;
  uint64_t reg_we = 0;
  uint64_t rd = 0;
  uint64_t value = 0;
  uint64_t store = 0;
  uint64_t store_addr = 0;
  uint64_t store_data = 0;
  uint64_t store_size = 0;
  uint64_t csr_we = 0;
  uint64_t csr_addr = 0;
  uint64_t csr_value = 0;
  uint64_t trap = 0;
  uint64_t trap_cause = 0;
  uint64_t trap_tval = 0;

  bool operator==(const RetireRec& o) const {
    // `id` is deliberately not compared. It is the *physical* destination
    // identity -- the {generation, tag} rename allocated -- and a change in the
    // allocator (the I-032 bank preference) legitimately moves it. It is
    // microarchitectural, not architectural: the program cannot observe it, and
    // requiring it to match would be requiring the fabric to allocate nothing.
    // Everything the ISA can observe is compared, field for field.
    return seq == o.seq && pc == o.pc && insn == o.insn && len == o.len &&
           reg_we == o.reg_we && rd == o.rd && value == o.value && store == o.store &&
           store_addr == o.store_addr && store_data == o.store_data &&
           store_size == o.store_size && csr_we == o.csr_we && csr_addr == o.csr_addr &&
           csr_value == o.csr_value && trap == o.trap && trap_cause == o.trap_cause &&
           trap_tval == o.trap_tval;
  }
};

std::string DumpRec(const RetireRec& r) {
  return "seq=" + Dec(r.seq) + " pc=" + U64(r.pc) + " insn=" + U64(r.insn) +
         " len=" + Dec(r.len) + " id=" + U64(r.id) + " rd=" + Dec(r.rd) +
         " we=" + Dec(r.reg_we) + " val=" + U64(r.value) +
         " store=" + Dec(r.store) + " sa=" + U64(r.store_addr) +
         " sd=" + U64(r.store_data) + " ss=" + Dec(r.store_size) +
         " csrwe=" + Dec(r.csr_we) + " csra=" + U64(r.csr_addr) +
         " csrv=" + U64(r.csr_value) + " trap=" + Dec(r.trap) +
         " cause=" + Dec(r.trap_cause) + " tval=" + U64(r.trap_tval);
}

// ============================================================================
// One run's record
// ============================================================================
struct RunRecord {
  bool fab_dyn = false;
  bool finished = false;
  bool passed = false;
  uint64_t cycles = 0;
  uint64_t retires_port = 0;    // o_commit_ctr
  uint64_t redirects = 0;
  uint64_t traps = 0;
  uint64_t unsupported = 0;
  uint64_t illegal = 0;
  uint64_t rob_occupied_end = 0;
  uint64_t desc_live_end = 0;
  uint64_t invariant_violations = 0;
  std::string invariant_first;
  bool signature_readable = false;
  uint64_t signature[4] = {0, 0, 0, 0};

  uint32_t unit_issues[kFabUnits] = {0, 0, 0, 0};
  uint32_t reason_ctr[kFabReasons] = {0, 0, 0, 0, 0, 0};
  uint32_t grant_ctr = 0;
  uint32_t stall_ctr = 0;
  uint32_t reject_ctr = 0;
  uint32_t fab_units = 0;
  uint32_t fab_classes = 0;
  uint32_t alloc_bank0 = 0;
  uint32_t alloc_bank1 = 0;
  uint32_t bp_captured = 0;
  uint32_t bp_hit = 0;
  uint32_t bp_unauth = 0;
  uint32_t bp_flush = 0;

  // The machine's own per-resource issue counts, observable in *both*
  // configurations (the router's `unit_issues` above exist only while the
  // fabric is on). These are what "the issue distribution changed" has to mean.
  uint32_t c0_alu = 0, c1_alu = 0, c0_br = 0, c1_br = 0, muldiv = 0;

  std::vector<RetireRec> retires;
};

// ============================================================================
// The runner: one configuration, from reset to the exit protocol
// ============================================================================
class Runner {
 public:
  Runner(Vmosaic_core_tb* dut, const Geometry& g) : dut_(dut), g_(g) {}

  RunRecord Run(const ProgImage& image, bool fab_dyn, uint64_t max_cycles) {
    RunRecord record;
    record.fab_dyn = fab_dyn;

    dut_mem_ = mosaic::MemoryModel();
    {
      std::string detail;
      if (!dut_mem_.LoadImage(image.elf(), &detail)) {
        Fail("setup", detail);
      }
    }

    Imem imem(&image);
    DataMem dmem(&dut_mem_);
    imem.Reset();
    dmem.Reset();

    cycles_ = 0;
    violations_ = 0;
    seq_width_ = g_.seq_w;
    ret_id_width_ = g_.ret_id_w;
    invariant_first_.clear();
    last_commit_ = 0;
    last_alloc_ = 0;
    last_progress_ = 0;
    retires_.clear();
    alloc_bank0_ = 0;
    alloc_bank1_ = 0;
    last_alloc_ctr_ = 0;
    fab_dyn_ = fab_dyn;

    dut_->clk = 0;
    dut_->rst = 1;
    dut_->fab_dyn_i = fab_dyn ? 1 : 0;
    dut_->eval();
    for (int i = 0; i < kResetCycles; i++) Cycle(true, &imem, &dmem);

    RunStatus status = RunStatus::kTimeout;
    while (true) {
      Cycle(false, &imem, &dmem);
      if (dut_mem_.finished()) {
        status = RunStatus::kPass;
        break;
      }
      if (dut_->o_stopped_o != 0) {
        status = RunStatus::kStopped;
        break;
      }
      if (cycles_ >= max_cycles) break;
      if (cycles_ - last_progress_ > kStallCycles) {
        status = RunStatus::kStalled;
        break;
      }
    }
    for (int i = 0; i < kSettleCycles; i++) Cycle(false, &imem, &dmem);

    record.finished = (status == RunStatus::kPass);
    record.passed = dut_mem_.passed();
    record.cycles = cycles_;
    record.retires_port = dut_->o_commit_o;
    record.redirects = dut_->o_redirect_o;
    record.traps = dut_->o_csr_trap_o;
    record.unsupported = dut_->o_unsupported_o;
    record.illegal = dut_->o_illegal_o;
    record.rob_occupied_end = dut_->o_rob_occupied_o;
    record.desc_live_end = dut_->o_desc_live_o;
    record.invariant_violations = violations_;
    record.invariant_first = invariant_first_;
    record.retires = retires_;

    {
      std::vector<uint64_t> words;
      if (dut_mem_.ReadSignature(&words) && words.size() == 4) {
        record.signature_readable = true;
        for (int k = 0; k < 4; k++) record.signature[k] = words[k];
      }
    }

    for (uint32_t u = 0; u < kFabUnits; u++) {
      record.unit_issues[u] = WideWord(dut_->o_fab_unit_issues_o, u);
    }
    for (uint32_t r = 0; r < kFabReasons; r++) {
      record.reason_ctr[r] = WideWord(dut_->o_fab_reason_ctr_o, r);
    }
    record.grant_ctr = dut_->o_fab_grant_ctr_o;
    record.stall_ctr = dut_->o_fab_stall_ctr_o;
    record.reject_ctr = dut_->o_fab_reject_ctr_o;
    record.fab_units = dut_->o_fab_units_o;
    record.fab_classes = dut_->o_fab_classes_o;
    record.alloc_bank0 = alloc_bank0_;
    record.alloc_bank1 = alloc_bank1_;
    record.bp_captured = dut_->o_fab_bp_captured_o;
    record.bp_hit = dut_->o_fab_bp_hit_o;
    record.bp_unauth = dut_->o_fab_bp_unauth_o;
    record.bp_flush = dut_->o_fab_bp_flush_o;
    record.c0_alu = dut_->o_c0_alu_o;
    record.c1_alu = dut_->o_c1_alu_o;
    record.c0_br = dut_->o_c0_br_o;
    record.c1_br = dut_->o_c1_br_o;
    record.muldiv = dut_->o_muldiv_o;

    if (!record.finished) {
      std::string why = "the run did not reach the program's exit protocol";
      if (status == RunStatus::kStopped) {
        why = "the machine stopped on an instruction it refuses (unsupported=" +
              Dec(record.unsupported) + ", illegal=" + Dec(record.illegal) +
              ", pc=" + U64(dut_->o_dbg_deliver_pc_o) + ")";
      } else if (status == RunStatus::kStalled) {
        why = "no commit and no allocation for " + Dec(kStallCycles) + " cycles";
      } else {
        why = "the cycle bound was reached";
      }
      Fail(fab_dyn ? "dynamic run" : "fixed run", why);
    }
    return record;
  }

 private:
  enum class RunStatus { kPass, kStopped, kStalled, kTimeout };

  void Note(const std::string& text) {
    violations_++;
    if (invariant_first_.empty()) {
      invariant_first_ = "cycle " + Dec(cycles_) + ": " + text;
    }
  }

  void Observe() {
    // The architectural event port: every lane whose valid bit is set is one
    // retired instruction, in program order.
    const uint32_t mask = (g_.retire_width >= 32)
                              ? 0xFFFFFFFFu
                              : ((1u << g_.retire_width) - 1u);
    const uint32_t valid = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((valid & (1u << lane)) == 0) continue;
      RetireRec r;
      r.seq = ScalarField(dut_->ev_seq_o, lane, seq_width_);
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.insn = ScalarField(dut_->ev_insn_o, lane, 32);
      r.len = ScalarField(dut_->ev_len_o, lane, 3);
      r.id = ScalarField(dut_->ev_id_o, lane, ret_id_width_);
      r.reg_we = ScalarField(dut_->ev_reg_we_o, lane, 1);
      r.rd = ScalarField(dut_->ev_rd_o, lane, 5);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.store = ScalarField(dut_->ev_store_o, lane, 1);
      r.store_addr = PayloadLane(dut_->ev_store_addr_o, lane);
      r.store_data = PayloadLane(dut_->ev_store_data_o, lane);
      r.store_size = ScalarField(dut_->ev_store_size_o, lane, 3);
      r.csr_we = ScalarField(dut_->ev_csr_we_o, lane, 1);
      r.csr_addr = ScalarField(dut_->ev_csr_addr_o, lane, 12);
      r.csr_value = PayloadLane(dut_->ev_csr_value_o, lane);
      r.trap = ScalarField(dut_->ev_trap_o, lane, 1);
      r.trap_cause = PayloadLane(dut_->ev_trap_cause_o, lane);
      r.trap_tval = PayloadLane(dut_->ev_trap_tval_o, lane);
      retires_.push_back(r);
    }

    // The standing invariants every core case asserts, kept here so a fabric
    // run that corrupts the machine's bookkeeping fails where it happened.
    if (dut_->o_squash_nc_o != 0 || dut_->o_squash_under_o != 0 ||
        dut_->o_journal_ovf_o != 0) {
      Note("recovery counters: squash_nc=" + Dec(dut_->o_squash_nc_o) + " under=" +
           Dec(dut_->o_squash_under_o) + " journal=" + Dec(dut_->o_journal_ovf_o));
    }
    if (dut_->o_rob_occupied_o > g_.rob_entries) {
      Note("ROB occupancy " + Dec(dut_->o_rob_occupied_o) + " exceeds " +
           Dec(g_.rob_entries));
    }
    if (dut_->o_mem_lq_query_mismatch_o != 0) {
      Note("the load queue's forwarding-query cross-check tripped: " +
           Dec(dut_->o_mem_lq_query_mismatch_o));
    }
    if (dut_->o_mem_sq_auth_o > dut_->o_mem_sq_occupied_o) {
      Note("store-queue authorised prefix " + Dec(dut_->o_mem_sq_auth_o) +
           " exceeds occupancy " + Dec(dut_->o_mem_sq_occupied_o));
    }

    // The bank the I-032 preference was driven with, sampled on the cycles the
    // allocator advanced. It proves both banks were exercised while the fabric
    // was on, not that the preference changed anything (its case owns that).
    if (dut_->o_dbg_alloc_ctr_o != last_alloc_ctr_) {
      last_alloc_ctr_ = dut_->o_dbg_alloc_ctr_o;
      if (fab_dyn_ && dut_->o_fab_alloc_bank_o == 0) alloc_bank0_++;
      if (fab_dyn_ && dut_->o_fab_alloc_bank_o == 1) alloc_bank1_++;
    }

    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_wb_pub_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) last_progress_ = cycles_;
  }

  void Cycle(bool rst, Imem* imem, DataMem* dmem) {
    dut_->rst = rst ? 1 : 0;
    dut_->fab_dyn_i = fab_dyn_ ? 1 : 0;

    dut_->imem_req_ready_i = 1;
    if (imem->HasResponse()) {
      const Imem::Request& r = imem->Response();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem->ResponseWord());
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
    if (dmem->HasResponse()) {
      const DataMem::Rsp& r = dmem->CurrentResponse();
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
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = 0;

    dut_->eval();

    if (!rst) Observe();

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      imem->Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem->PopResponse();
    }
    imem->Advance();

    if (bus_reset_.MayAccept(rst, (dut_->dmem_req_valid_o != 0) &&
                                      (dut_->dmem_req_ready_i != 0))) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem->Accept(r, cycles_);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
      dmem->PopResponse();
    }
    dmem->Advance();

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  Vmosaic_core_tb* dut_;
  // The reset-traffic rule (V-010, sim/common/bus_reset_gate.h): while reset
  // is asserted this model accepts nothing, so no reset-time response can be
  // queued ahead of a fresh post-reset one.
  mosaic::BusResetGate bus_reset_;
  Geometry g_;
  mosaic::MemoryModel dut_mem_;
  // Per-lane field widths of the event port, read from the labelled geometry
  // the wrapper publishes rather than re-derived here.
  uint32_t seq_width_ = 0;
  uint32_t ret_id_width_ = 0;
  uint64_t cycles_ = 0;
  uint64_t violations_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_alloc_ctr_ = 0;
  uint64_t last_progress_ = 0;
  bool fab_dyn_ = false;
  uint32_t alloc_bank0_ = 0;
  uint32_t alloc_bank1_ = 0;
  std::string invariant_first_;
  std::vector<RetireRec> retires_;
};

}  // namespace

int main(int argc, char** argv) {
  bool fixed_is_dynamic = false;
  std::vector<char*> filtered;
  filtered.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = (i + 1) < argc;
    if (flag == "--control-fixed-is-dynamic") {
      fixed_is_dynamic = true;
    } else {
      filtered.push_back(argv[i]);
      if (has_value && (flag == "--case" || flag == "--out" || flag == "--image" ||
                        flag == "--expect" || flag == "--seed" ||
                        flag == "--max-cycles")) {
        filtered.push_back(argv[++i]);
      }
    }
  }

  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(static_cast<int>(filtered.size()), filtered.data(),
                              &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    const std::string repo = FindRepoRoot();
    if (repo.empty()) {
      Fail("setup", "cannot find the repository root: no config/profiles/p0.json "
                    "above the working directory");
    }
    std::string elf_path = options.image.empty()
                               ? repo + "/tests/programs/build/p02_branch.i0.elf"
                               : options.image;
    ProgImage image;
    std::string load_detail;
    if (!image.Load(elf_path, &load_detail)) {
      Fail("setup", load_detail + " (build the corpus with `make -C tests/programs all`)");
    }
    // The wrapper's geometry read-back is a continuous assignment of the
    // generated parameters, but Verilator only settles outputs on `eval`.
    dut.clk = 0;
    dut.rst = 1;
    dut.fab_dyn_i = 0;
    dut.eval();
    const Geometry geom = ReadGeometry(&dut);
    if (image.entry() != geom.reset_vector) {
      Fail("setup", "the image's entry " + U64(image.entry()) +
                        " is not the profile's reset vector " + U64(geom.reset_vector));
    }
    std::printf("fabric.integrated: image=%s entry=%s retire_width=%u rob=%u "
                "artifact=%s\n",
                elf_path.c_str(), U64(image.entry()).c_str(), geom.retire_width,
                geom.rob_entries, kFabUnits == 4 ? "steering+bankbias+bypass" : "?");

    Runner runner(&dut, geom);
    RunRecord fixed = runner.Run(image, false, options.max_cycles);
    // A full reset between the two runs: the second run starts from the same
    // state as the first, which is what makes them comparable.
    RunRecord dyn = runner.Run(image, fixed_is_dynamic ? false : true, options.max_cycles);

    auto report_run = [](const char* label, const RunRecord& r) {
      std::printf(
          "  %-8s cycles=%-8llu retires=%-7llu redir=%-5llu traps=%-3llu "
          "unit_issues=[%u %u %u %u] grants=%u stalls=%u rejects=%u "
          "reasons=[%u %u %u %u %u %u] bp_captured=%u bp_hit=%u bp_unauth=%u "
          "bp_flush=%u banks=[%u %u] mt_issues=[alu %u/%u br %u/%u md %u]\n",
          label, static_cast<unsigned long long>(r.cycles),
          static_cast<unsigned long long>(r.retires_port),
          static_cast<unsigned long long>(r.redirects),
          static_cast<unsigned long long>(r.traps), r.unit_issues[0], r.unit_issues[1],
          r.unit_issues[2], r.unit_issues[3], r.grant_ctr, r.stall_ctr, r.reject_ctr,
          r.reason_ctr[0], r.reason_ctr[1], r.reason_ctr[2], r.reason_ctr[3],
          r.reason_ctr[4], r.reason_ctr[5], r.bp_captured, r.bp_hit, r.bp_unauth,
          r.bp_flush, r.alloc_bank0, r.alloc_bank1,
          r.c0_alu, r.c1_alu, r.c0_br, r.c1_br, r.muldiv);
    };
    std::printf("fixed  configuration:\n");
    report_run("fixed", fixed);
    std::printf("dynamic configuration:\n");
    report_run("dynamic", dyn);

    // ---------------------------------------------------------------- check 1
    // The architecture is identical, event by event and word by word.
    bool same_stream = (fixed.retires.size() == dyn.retires.size());
    int first_diff = -1;
    if (same_stream) {
      for (size_t i = 0; i < fixed.retires.size(); i++) {
        if (!(fixed.retires[i] == dyn.retires[i])) {
          same_stream = false;
          first_diff = static_cast<int>(i);
          break;
        }
      }
    }
    if (!same_stream) {
      const size_t at = (first_diff < 0) ? fixed.retires.size() : size_t(first_diff);
      std::printf("  ARCHITECTURE DIVERGES at retire %zu:\n", at);
      if (first_diff >= 0) {
        std::printf("    fixed:   %s\n", DumpRec(fixed.retires[at]).c_str());
        std::printf("    dynamic: %s\n", DumpRec(dyn.retires[at]).c_str());
      } else {
        std::printf("    fixed retired %zu, dynamic retired %zu\n",
                    fixed.retires.size(), dyn.retires.size());
      }
    }
    bool same_sig = fixed.signature_readable && dyn.signature_readable;
    for (int k = 0; k < 4 && same_sig; k++) {
      if (fixed.signature[k] != dyn.signature[k]) same_sig = false;
    }
    if (!same_sig) {
      std::printf("  SIGNATURE DIVERGES: fixed %s %s %s %s / dynamic %s %s %s %s\n",
                  U64(fixed.signature[0]).c_str(), U64(fixed.signature[1]).c_str(),
                  U64(fixed.signature[2]).c_str(), U64(fixed.signature[3]).c_str(),
                  U64(dyn.signature[0]).c_str(), U64(dyn.signature[1]).c_str(),
                  U64(dyn.signature[2]).c_str(), U64(dyn.signature[3]).c_str());
    }
    reporter.Check(same_stream && same_sig,
                   "the fixed and dynamic configurations retire the same architecture: "
                   "the whole retire event stream and the four signature words are "
                   "identical (" + Dec(fixed.retires.size()) + " events)");
    reporter.Check(fixed.retires_port == fixed.retires.size() &&
                       dyn.retires_port == dyn.retires.size(),
                   "the committed-instruction count equals the retire events the port "
                   "published in both configurations (" +
                       Dec(fixed.retires_port) + " / " + Dec(dyn.retires_port) + ")");

    // ---------------------------------------------------------------- check 2
    // The dynamic configuration changed something measurable.
    const bool dist_differs = (fixed.c0_alu != dyn.c0_alu) ||
                              (fixed.c1_alu != dyn.c1_alu) ||
                              (fixed.c0_br != dyn.c0_br) ||
                              (fixed.c1_br != dyn.c1_br) ||
                              (fixed.muldiv != dyn.muldiv);
    const bool cycles_differ = fixed.cycles != dyn.cycles;
    std::printf("  measurable difference: machine issue distribution differs=%d "
                "(c0 alu %u->%u, c1 alu %u->%u), cycles differ=%d (fixed %llu, "
                "dynamic %llu)\n",
                dist_differs ? 1 : 0, fixed.c0_alu, dyn.c0_alu, fixed.c1_alu,
                dyn.c1_alu, cycles_differ ? 1 : 0,
                static_cast<unsigned long long>(fixed.cycles),
                static_cast<unsigned long long>(dyn.cycles));
    reporter.Check(dist_differs || cycles_differ,
                   "the dynamic configuration changed something measurable: the "
                   "machine's per-resource issue distribution or the cycle count "
                   "differs from the fixed configuration (issues differ=" +
                       std::string(dist_differs ? "yes" : "no") + ", cycles differ=" +
                       std::string(cycles_differ ? "yes" : "no") + ")");

    // ---------------------------------------------------------------- check 3
    // The machinery that is supposed to be doing the work is.
    uint32_t reason_sum = 0;
    for (uint32_t r = 0; r < kFabReasons; r++) reason_sum += dyn.reason_ctr[r];
    reporter.Check(fixed.grant_ctr == 0 && fixed.stall_ctr == 0 &&
                       fixed.reject_ctr == 0,
                   "the fixed configuration's router is idle (its strategy is the "
                   "affinity toggle, not the steering): grants/stalls/rejects are 0");
    reporter.Check(dyn.grant_ctr > 0 && reason_sum == dyn.grant_ctr,
                   "the dynamic configuration's router granted " + Dec(dyn.grant_ctr) +
                       " macros and recorded a deciding key for every one of them "
                       "(reason histogram sums to " + Dec(reason_sum) + ")");
    reporter.Check(dyn.bp_captured > 0 && dyn.bp_hit > 0 && dyn.bp_unauth == 0,
                   "the cluster bypass tapped " + Dec(dyn.bp_captured) +
                       " producers and resolved " + Dec(dyn.bp_hit) +
                       " operands early, and forwarded nothing unauthorised");
    reporter.Check(fixed.bp_captured == 0 && fixed.bp_hit == 0,
                   "the fixed configuration's bypass is disarmed: no slot capture and "
                   "no early wakeup");
    reporter.Check(fixed.fab_units == kFabUnits && dyn.fab_units == kFabUnits &&
                       dyn.fab_classes >= 7,
                   "the steering's capability model is the core's (" + Dec(dyn.fab_units) +
                       " units, " + Dec(dyn.fab_classes) + " classes)");
    reporter.Check(!dyn.fab_dyn || (dyn.alloc_bank0 > 0 && dyn.alloc_bank1 > 0),
                   "the I-032 bank preference was driven from the routing decision: "
                   "both cluster banks appear in the allocation sample (" +
                       Dec(dyn.alloc_bank0) + " / " + Dec(dyn.alloc_bank1) + ")");

    // ---------------------------------------------------------------- check 4
    // The invariants hold with the fabric enabled (and off).
    for (const RunRecord* r : {&fixed, &dyn}) {
      const std::string label = r->fab_dyn ? "dynamic" : "fixed";
      reporter.Check(r->invariant_violations == 0,
                     label + " configuration: the machine's invariants hold every cycle "
                             "(recovery, ROB occupancy, load-queue cross-check, "
                             "store-queue authorisation)");
      reporter.Check(r->rob_occupied_end == 0 && r->desc_live_end == 0,
                     label + " configuration: the ROB and the descriptor store drain "
                             "to empty at the exit protocol (identity liveness)");
      reporter.Check(r->passed,
                     label + " configuration: the program's own exit protocol declares "
                             "success (signature readable=" +
                         std::string(r->signature_readable ? "yes" : "no") + ")");
    }
    if (fixed.invariant_violations != 0) {
      std::printf("  fixed invariant first: %s\n", fixed.invariant_first.c_str());
    }
    if (dyn.invariant_violations != 0) {
      std::printf("  dynamic invariant first: %s\n", dyn.invariant_first.c_str());
    }

    passed = (reporter.failures() == 0);
    detail = "image=" + std::string(elf_path.substr(elf_path.find_last_of('/') + 1)) +
             " fixed_cycles=" + Dec(fixed.cycles) + " dyn_cycles=" + Dec(dyn.cycles) +
             " fixed_retires=" + Dec(fixed.retires_port) +
             " dyn_retires=" + Dec(dyn.retires_port) +
             " fixed_issues=" + Dec(fixed.c0_alu) + "/" + Dec(fixed.c1_alu) + "/" +
             Dec(fixed.c0_br) + "/" + Dec(fixed.c1_br) + "/" + Dec(fixed.muldiv) +
             " dyn_issues=" + Dec(dyn.c0_alu) + "/" + Dec(dyn.c1_alu) + "/" +
             Dec(dyn.c0_br) + "/" + Dec(dyn.c1_br) + "/" + Dec(dyn.muldiv) +
             " dyn_unit_issues=" + Dec(dyn.unit_issues[0]) + "/" +
             Dec(dyn.unit_issues[1]) + "/" + Dec(dyn.unit_issues[2]) + "/" +
             Dec(dyn.unit_issues[3]) + " dyn_reasons=" + Dec(dyn.reason_ctr[0]) + "," +
             Dec(dyn.reason_ctr[1]) + "," + Dec(dyn.reason_ctr[2]) + "," +
             Dec(dyn.reason_ctr[3]) + "," + Dec(dyn.reason_ctr[4]) + "," +
             Dec(dyn.reason_ctr[5]) + " bp_captured=" + Dec(dyn.bp_captured) +
             " bp_hit=" + Dec(dyn.bp_hit) + " arch_identical=" +
             (same_stream && same_sig ? "yes" : "no") +
             (fixed_is_dynamic ? " CONTROL=--control-fixed-is-dynamic" : "");
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the fabric runs", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
