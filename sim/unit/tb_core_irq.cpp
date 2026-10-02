// ============================================================================
// tb_core_irq.cpp -- CASE=irq.replay_timeline, work package V-015.
//
// The DUT is the integrated p0 core with the interrupt decision (I-020), the
// CSR file (I-019), the system-instruction boundary and the trap controller all
// wired in. The case drives the three CLINT-style level sources from the
// outside -- a timer, a software and an external line -- and asks one question:
//
//     is the boundary at which an interrupt is *accepted* a function of the
//     program and the stimulus alone, so that the same stimulus reaches the
//     same acceptance boundary on a second run?
//
// ------------------------------------------------------------- why this case
//
// An interrupt is not an instruction. It has no PC, no decode, no retirement,
// and it is taken only where the core chooses to offer it (`core_can_trap_i`).
// The card's fail modes are the three ways that rule is easiest to lose:
//
//   1. a stimulus whose timing comes from somewhere other than the machine
//      (host wall time) -- the same program then reaches a different boundary
//      on the second run;
//   2. an interrupt treated as an ordinary retiring instruction -- it appears
//      in the retirement stream, or the instruction it interrupts retires in
//      the trap cycle;
//   3. a pending interrupt dropped at a boundary instead of staying pending --
//      an interrupt asserted while masked is "lost" rather than taken once it
//      becomes enabled.
//
// Each is injected as a control (see the `MOSAIC_IRQ_MUTANT_*` blocks and
// results/reports/V-015-irq.md).
//
// ------------------------------------------------- what is driven, and where
//
// Every stimulus is keyed on a **numbered retirement boundary**, never on a
// cycle count and never on the host clock: the driver watches the architectural
// retirement stream and fires an event when a *named instruction of this
// program* retires (its PC is the boundary's number, and the commit count at
// that instant is recorded with it). The one exception is the WFI wake, and the
// exception is structural rather than a convenience: a hart halted by WFI
// retires nothing, so no retirement boundary exists to key on. That event is
// keyed on the halt's own duration (the number of completed halted cycles),
// which is still a number produced by the machine's state; it is recorded as
// such and called out in the report.
//
// The stimulus covers the card's named cases:
//
//   * pending-but-disabled -- the timer is asserted while `mie` is 0 and while
//     `mstatus.MIE` is 0, and the run counts the cycles in each state;
//   * an enable CSR written in the immediately preceding instruction -- the
//     timer is enabled by `csrw mie, 0x80`, and the interrupt must be accepted
//     at the very next boundary, with `mepc` naming the instruction after the
//     write and the *preceding* retirement being the write itself;
//   * a synchronous trap competing with an interrupt -- a software interrupt is
//     pending, enabled and unmasked when an `ecall` reaches the head; the
//     exception is taken (cause 11) and the interrupt is taken at the next
//     boundary instead of being lost;
//   * WFI wake -- the WFI halts with an enabled-but-unmasked timer (so no trap
//     is taken), an external source that is pending but can never be enabled
//     does *not* wake it, and the timer's assertion does;
//   * the external line throughout: it is pending for long stretches and is
//     never accepted, because `config/csr/mode_m.json` makes `mie` bits 7 and 3
//     the only writable ones and bit 11 (MEIE) read-only zero.
//
// Asserted boundaries and accepted boundaries are recorded *separately* and
// printed separately, so "the interrupt was asserted at boundary N and accepted
// at boundary M" is evidence rather than an inference from a single number.
//
// ------------------------------------------------------- where the facts come from
//
// Nothing here is taken from the DUT as its own oracle:
//
//   * the program's own handler logs `{mcause, mepc}` from inside the trap, so
//     the architectural record is written by the machine, not by the driver;
//   * the expected causes are the ISA's (3/7/11 and the interrupt bit) and the
//     expected `mepc` values are PCs this file assembled;
//   * the replay comparison runs the *same* program and the *same* seeded
//     stimulus twice through a reset, from a fresh memory model each time, and
//     compares the two runs' stimulus records, acceptance records, per-cycle
//     trace hash and final architectural state;
//   * the standing invariants below are the contract's own rules, checked every
//     cycle rather than at the end:
//       - `irq_valid_o` is never high unless `mstatus.MIE` is set and an
//         enabled, non-delegated pending bit exists;
//       - the interrupt unit's spurious-wake counter never moves;
//       - the retirement counter equals the retirement events published;
//       - an interrupt is never accepted in a cycle that retires anything.
//
// ---------------------------------------------------------------- the mutant hooks
//
// The shipping build defines none of these; the table with real output is in
// results/reports/V-015-irq.md and the runner is tools/run_irq_controls.py.
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
#include <vector>

#ifdef MOSAIC_IRQ_MUTANT_WALLCLOCK
#include <chrono>
#endif

#include "Vmosaic_core_tb.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic_ref::DataMem;

namespace {

// ---------------------------------------------------------------------------
// Parameters of the run
// ---------------------------------------------------------------------------
constexpr int kResetCycles = 4;
// The run is a few hundred instructions and three traps; 4,000,000 is the
// registry's budget and is only reached if the machine stops making progress.
constexpr uint64_t kRunCycles = 4000000;
// A stall is a defect with a location, not a timeout.
constexpr uint64_t kStallCycles = 4000;

// The handler's log: a 64-bit count at +0, then 16-byte `{mcause, mepc}`
// records at +16. Placed above the text and the protocol areas so nothing the
// program does can be confused with it.
constexpr uint64_t kLogAddr  = 0x80010000ull;
constexpr int kLogMax = 16;

// Interrupt cause codes (mcause bit 63 set + the ISA's code), and the
// synchronous exception this program arms.
constexpr uint64_t kCauseMsi  = 0x8000000000000003ull;  // machine software
constexpr uint64_t kCauseMti  = 0x8000000000000007ull;  // machine timer
constexpr uint64_t kCauseMei  = 0x800000000000000bull;  // machine external
constexpr uint64_t kCauseEcall = 11ull;                 // M-mode environment call

// mie/mip bit positions, from the ISA's cause table. MEIP/MEIE (11) is
// implemented as read-only zero by config/csr/mode_m.json, so the external line
// can be pending and can never be enabled or taken.
constexpr uint64_t kBitMtie = 0x80ull;
constexpr uint64_t kBitMsie = 0x08ull;
constexpr uint64_t kBitMeie = 0x800ull;
constexpr uint64_t kBitMie  = 0x8ull;   // mstatus.MIE

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ---------------------------------------------------------------------------
// The host wall clock. The shipping build returns zero and the runner applies
// nothing; under the control it returns a value derived from the host's steady
// clock. See MOSAIC_IRQ_MUTANT_WALLCLOCK below.
// ---------------------------------------------------------------------------
uint64_t HostWallTag() {
#ifdef MOSAIC_IRQ_MUTANT_WALLCLOCK
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const uint64_t ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  return ns | 1ull;
#else
  return 0;
#endif
}

// ============================================================================
// A very small assembler, the same linear emitter the sibling core cases use
// ============================================================================
class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}

  uint64_t pc() const { return base_ + 4ull * words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }
  void Emit(uint32_t w) { words_.push_back(w); }

  static uint32_t R(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3,
                    uint32_t rd, uint32_t op) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
  }
  static uint32_t I(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd,
                    uint32_t op) {
    return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) |
           (f3 << 12) | (rd << 7) | op;
  }
  static uint32_t S(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3,
                    uint32_t op) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 5) & 0x7Fu) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           ((u & 0x1Fu) << 7) | op;
  }
  static uint32_t U(uint32_t imm20, uint32_t rd, uint32_t op) {
    return (imm20 << 12) | (rd << 7) | op;
  }

  void Addi(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x13)); }
  void Add(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0, rs2, rs1, 0, rd, 0x33)); }
  void Xori(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 4, rd, 0x13)); }
  void Slli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(I(static_cast<int32_t>(sh), rs1, 1, rd, 0x13));
  }
  void Srli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(I(static_cast<int32_t>(sh), rs1, 5, rd, 0x13));
  }
  void Lui(uint32_t rd, uint32_t imm20) { Emit(U(imm20, rd, 0x37)); }
  void Ld(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 3, rd, 0x03)); }
  void Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { Emit(S(imm, rs2, rs1, 3, 0x23)); }
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 1, rd, 0x73));
  }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 2, rd, 0x73));
  }
  void Ecall() { Emit(0x00000073u); }
  void Mret() { Emit(0x30200073u); }
  void Wfi() { Emit(0x10500073u); }

  // `li rd, value`, with the 32-bit zero-extension the p0 addresses need.
  void LiAbs(uint32_t rd, uint64_t value) {
    const int64_t v = static_cast<int64_t>(value);
    if (v >= -2048 && v <= 2047) {
      Addi(rd, 0, static_cast<int32_t>(v));
      return;
    }
    const uint32_t lo = static_cast<uint32_t>(value) & 0xFFFu;
    const uint32_t hi = (static_cast<uint32_t>(value) + 0x800u) >> 12;
    Lui(rd, hi);
    Addi(rd, rd, static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0));
    if (value > 0xFFFFFFFFull) {
      Fail("assembler", "li of a value wider than 32 bits is not implemented");
    }
    Slli(rd, rd, 32);
    Srli(rd, rd, 32);
  }

  void La(uint32_t rd, const std::string& label) {
    LaFixup fix;
    fix.at = words_.size();
    fix.rd = rd;
    fix.label = label;
    fixups_.push_back(fix);
    words_.push_back(0);
    words_.push_back(0);
  }

  void Mark(const std::string& label) { labels_[label] = pc(); }
  uint64_t Label(const std::string& name) const {
    auto it = labels_.find(name);
    return (it == labels_.end()) ? 0 : it->second;
  }

  void Resolve() {
    for (const LaFixup& fix : fixups_) {
      auto it = labels_.find(fix.label);
      if (it == labels_.end()) continue;
      const int64_t here = static_cast<int64_t>(base_ + 4ull * fix.at);
      const int64_t delta = static_cast<int64_t>(it->second) - here;
      const uint32_t lo = static_cast<uint32_t>(delta) & 0xFFFu;
      const uint32_t hi = static_cast<uint32_t>((delta + 0x800) >> 12) & 0xFFFFFu;
      words_[fix.at] = U(hi, fix.rd, 0x17);   // auipc rd, hi20
      words_[fix.at + 1] = I(static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0),
                             fix.rd, 0, fix.rd, 0x13);
    }
  }

 private:
  struct LaFixup {
    size_t at;
    uint32_t rd;
    std::string label;
  };
  uint64_t base_;
  std::vector<uint32_t> words_;
  std::vector<LaFixup> fixups_;
  std::map<std::string, uint64_t> labels_;
};

// ============================================================================
// The instruction image and the instruction port
// ============================================================================
class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x00000073u : it->second;   // ECALL off-image
  }
  const std::map<uint64_t, uint32_t>& words() const { return words_; }

 private:
  std::map<uint64_t, uint32_t> words_;
};

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
  uint32_t ResponseWord() const { return img_->Word(ready_.front().addr); }

  void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
    Request r;
    r.addr = addr;
    r.id = id;
    r.epoch = epoch;
    inflight_.push_back(Entry{r, 1});
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
// Geometry, from the elaborated DUT
// ============================================================================
struct Geometry {
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return static_cast<uint64_t>(wide[lane * 2]) |
         (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
}
uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
  const uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
  return (packed >> (lane * width)) & mask;
}

// ============================================================================
// The observed records
// ============================================================================
struct Retire {
  uint64_t pc = 0;
  uint64_t cycle = 0;
  uint64_t commit = 0;   // the commit count after this retirement
};

struct TrapRecord {
  uint64_t cause = 0;
  uint64_t mepc = 0;
  uint64_t tval = 0;
  uint64_t target = 0;
  bool is_irq = false;
  uint64_t cycle = 0;
  uint64_t boundary = 0;      // the commit count when the trap was decided
  uint64_t head_pc = 0;       // the head the trap was decided on
  bool head_valid = false;
  bool retired_in_cycle = false;   // any lane of the retire stream was populated
  uint64_t pre_retire_pc = 0;      // the last ordinary retirement before the trap
  bool has_pre_retire = false;
  uint64_t mip = 0, mie = 0, mstatus = 0, irq_valid = 0;
};

struct StimulusRecord {
  std::string label;
  std::string trigger;        // "retire:<pc>" or "halt>=N"
  uint64_t host_tag = 0;      // the host clock value this schedule was derived from
  uint64_t trigger_cycle = 0;
  uint64_t assert_boundary = 0;   // the commit count when the pins were applied
  uint64_t assert_cycle = 0;
  uint64_t soft = 0, timer = 0, ext = 0;
};

struct RunResult {
  std::vector<StimulusRecord> stimuli;
  std::vector<Retire> retires;
  std::vector<TrapRecord> traps;
  uint64_t trace_hash = 0;
  uint64_t cycles = 0;
  uint64_t retired = 0;

  // Evidence the scenario checks read.
  uint64_t cycles_timer_pending_disabled = 0;   // mip[7]=1, mie[7]=0
  uint64_t cycles_enabled_unmasked_no_mie = 0;  // (mip&mie)!=0, mstatus.MIE=0
  uint64_t cycles_ext_high = 0;                 // cycles the external line was driven
  bool mip11_seen = false;                      // the CSR-visible mip[11] ever set
  bool mie11_seen = false;                      // the CSR-visible mie[11] ever set
  uint64_t halt_cycles = 0;
  bool wfi_halted = false;
  uint64_t wake_cycle = 0;
  uint64_t wake_assert_cycle = 0;
  uint64_t halt_cycles_at_wake_assert = 0;
  bool ext_asserted_during_halt = false;
  uint64_t log_count = 0;
  std::vector<std::pair<uint64_t, uint64_t>> log;   // {mcause, mepc}
};

// ============================================================================
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(mem, 1) {}

  void Configure(const Geometry& g) { g_ = g; }
  void Phase(const std::string& name) { phase_ = name; }
  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t retired() const { return retires_.size(); }
  const std::vector<Retire>& retires() const { return retires_; }
  const std::vector<TrapRecord>& traps() const { return traps_; }
  RunResult* result() { return result_; }
  void SetResult(RunResult* r) { result_ = r; }
  uint64_t halt_streak() const { return halt_streak_; }
  bool halt_fell() const { return halt_fell_; }
  void ClearHaltFell() { halt_fell_ = false; }

  void SetPins(bool soft, bool timer, bool ext) {
    next_soft_ = soft;
    next_timer_ = timer;
    next_ext_ = ext;
  }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " retired=" + Dec(dut_->o_commit_o) + " traps=" + Dec(traps_.size()) +
           " mcause=" + U64(dut_->o_csr_mcause_o) + " mepc=" + U64(dut_->o_csr_mepc_o) +
           " mstatus=" + U64(dut_->o_csr_mstatus_o) + " mip=" + U64(dut_->o_csr_mip_o);
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + State());
    }
    mtime_++;
    dut_->rst = rst ? 1 : 0;
    dut_->irq_soft_i = next_soft_ ? 1 : 0;
    dut_->irq_timer_i = next_timer_ ? 1 : 0;
    dut_->irq_ext_i = next_ext_ ? 1 : 0;
    dut_->mtime_i = mtime_;
    dut_->imem_req_ready_i = 1;
    dut_->imem_rsp_valid_i = imem_.HasResponse() ? 1 : 0;
    if (imem_.HasResponse()) {
      const Imem::Request& r = imem_.Response();
      dut_->imem_rsp_rdata_i = imem_.ResponseWord();
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

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

 private:
  void Mix(uint64_t value) {
    if (result_ == nullptr) return;
    result_->trace_hash = (result_->trace_hash ^ value) * 1099511628211ull;
  }

  void Observe() {
    RunResult* rr = result_;
    Compare("the retire counter equals the retirement events published",
            dut_->o_commit_o == retires_.size(),
            "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(retires_.size()));
    Compare("ROB occupancy is within the ROB",
            dut_->o_rob_occupied_o <= g_.rob_entries,
            "occupied=" + Dec(dut_->o_rob_occupied_o));
    Compare("the rename recovery counters report no invalid squash",
            dut_->o_squash_nc_o == 0 && dut_->o_squash_under_o == 0,
            "squash_nc=" + Dec(dut_->o_squash_nc_o) +
                " under=" + Dec(dut_->o_squash_under_o));
    Compare("every exception payload was matched to its slot's generation",
            dut_->o_exc_gen_mismatch_o == 0,
            "mismatches=" + Dec(dut_->o_exc_gen_mismatch_o));
    Compare("no spurious WFI wake",
            dut_->o_spurious_wake_o == 0,
            "spurious=" + Dec(dut_->o_spurious_wake_o));
    // The interrupt unit's own rule, checked every cycle and stated exactly:
    // an offer requires the global enable *and* an enabled, non-delegated
    // pending bit. (`mideleg` is zero in p0, so `mip & mie` is the whole of it.)
    Compare("an interrupt is only offered with MIE set and an enabled pending bit",
            (dut_->o_irq_valid_o == 0) ||
                (((dut_->o_csr_mstatus_o & kBitMie) != 0) &&
                 ((dut_->o_csr_mip_o & dut_->o_csr_mie_o) != 0)),
            "irq_valid=1 mstatus=" + U64(dut_->o_csr_mstatus_o) +
                " mie=" + U64(dut_->o_csr_mie_o) + " mip=" + U64(dut_->o_csr_mip_o));

    if (rr != nullptr) {
      if (((dut_->o_csr_mip_o & kBitMtie) != 0) &&
          ((dut_->o_csr_mie_o & kBitMtie) == 0)) {
        rr->cycles_timer_pending_disabled++;
      }
      if (((dut_->o_csr_mip_o & dut_->o_csr_mie_o) != 0) &&
          ((dut_->o_csr_mstatus_o & kBitMie) == 0)) {
        rr->cycles_enabled_unmasked_no_mie++;
      }
      if (next_ext_) rr->cycles_ext_high++;
      if ((dut_->o_csr_mip_o & kBitMeie) != 0) rr->mip11_seen = true;
      if ((dut_->o_csr_mie_o & kBitMeie) != 0) rr->mie11_seen = true;
    }

    // The halt: count completed halted cycles and the transition out of it.
    if (dut_->o_wfi_halt_o != 0) {
      halt_streak_++;
      if (rr != nullptr) {
        rr->halt_cycles++;
        rr->wfi_halted = true;
      }
    } else {
      if (halt_streak_ > 0) halt_fell_ = true;
      halt_streak_ = 0;
    }

    // The retirement stream. Lane order is program order.
    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      if (PackedLane(dut_->ev_trap_o, lane, 1) != 0) continue;   // a trap event
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.cycle = cycles_;
      r.commit = dut_->o_commit_o;
      retires_.push_back(r);
    }

    // The trap decision. A trap pulse is one cycle wide.
    if (dut_->o_trap_valid_o != 0) {
      TrapRecord t;
      t.cause = dut_->o_trap_cause_o;
      t.mepc = dut_->o_trap_epc_o;
      t.tval = dut_->o_trap_tval_o;
      t.target = dut_->o_trap_target_o;
      t.is_irq = dut_->o_trap_is_irq_o != 0;
      t.cycle = cycles_;
      t.boundary = dut_->o_commit_o;
      t.head_pc = dut_->o_dbg_head_pc_o;
      t.head_valid = dut_->o_dbg_head_valid_o != 0;
      t.retired_in_cycle = retires_.size() != 0 &&
                           retires_.back().cycle == cycles_;
      t.has_pre_retire = !retires_.empty();
      if (t.has_pre_retire) t.pre_retire_pc = retires_.back().pc;
      t.mip = dut_->o_csr_mip_o;
      t.mie = dut_->o_csr_mie_o;
      t.mstatus = dut_->o_csr_mstatus_o;
      t.irq_valid = dut_->o_irq_valid_o;
      traps_.push_back(t);
    }

    if (rr != nullptr) {
      Mix(cycles_);
      Mix(dut_->o_commit_o);
      Mix(dut_->o_csr_mip_o);
      Mix(dut_->o_csr_mie_o);
      Mix(dut_->o_csr_mstatus_o);
      Mix(dut_->o_irq_valid_o);
      Mix(dut_->o_trap_valid_o);
      Mix(dut_->o_trap_is_irq_o);
      Mix(dut_->o_trap_cause_o);
      Mix(dut_->o_trap_epc_o);
      Mix(dut_->o_wfi_halt_o);
      Mix((next_soft_ ? 1u : 0u) | (next_timer_ ? 2u : 0u) | (next_ext_ ? 4u : 0u));
    }

    // The progress guard: a run that stops retiring, trapping or allocating for
    // thousands of cycles is a defect, and it must be named rather than timed out.
    if (std::getenv("IRQ_DEBUG") != nullptr) {
      std::printf("    [cyc] %llu commit=%llu head=%s mip=%s mie=%s mstatus=%s "
                  "irq=%d trap=%d wfi=%d pins=%d%d%d\n",
                  static_cast<unsigned long long>(cycles_),
                  static_cast<unsigned long long>(dut_->o_commit_o),
                  U64(dut_->o_dbg_head_pc_o).c_str(), U64(dut_->o_csr_mip_o).c_str(),
                  U64(dut_->o_csr_mie_o).c_str(), U64(dut_->o_csr_mstatus_o).c_str(),
                  dut_->o_irq_valid_o, dut_->o_trap_valid_o, dut_->o_wfi_halt_o,
                  next_soft_ ? 1 : 0, next_timer_ ? 1 : 0, next_ext_ ? 1 : 0);
    }
    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_trap_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) {
      last_progress_ = cycles_;
    } else if (cycles_ - last_progress_ > kStallCycles) {
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled: " + State());
    }
  }

 public:
  uint64_t mtime() const { return mtime_; }

 private:
  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  Geometry g_;
  std::string phase_;
  RunResult* result_ = nullptr;
  std::vector<Retire> retires_;
  std::vector<TrapRecord> traps_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  uint64_t mtime_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  uint64_t halt_streak_ = 0;
  bool halt_fell_ = false;
  bool next_soft_ = false, next_timer_ = false, next_ext_ = false;
};

// ============================================================================
// Repository root
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
// The program
// ============================================================================
struct ScenarioParams {
  int pad1 = 0, pad2 = 0, pad3 = 0, pad4 = 0, pad5 = 0;
  uint64_t wake_halt = 0;     // completed halted cycles before the wake source
};

// The seeded parameters: the pad lengths (how long each window is) and the
// halt duration before the wake. The named boundaries are instruction PCs, not
// these numbers, but the numbers are what makes "the same seed" mean something.
// The host wall clock is deliberately *not* an input here -- it enters in
// RunOnce, per run, because that is what the first control injects.
ScenarioParams DeriveParams(uint64_t seed) {
  ScenarioParams p;
  mosaic::Rng rng(seed);
  p.pad1 = 40 + static_cast<int>(rng.Below(24));
  p.pad2 = 40 + static_cast<int>(rng.Below(24));
  p.pad3 = 16;
  p.pad4 = 32 + static_cast<int>(rng.Below(16));
  p.pad5 = 48 + static_cast<int>(rng.Below(16));
  p.wake_halt = 3 + rng.Below(4);
  return p;
}

struct Program {
  Asm asm_{MOSAIC_RESET_VECTOR};
  uint64_t handler_pc = 0;
  uint64_t t1_pc = 0, t2_pc = 0, t3_pc = 0;
  uint64_t mie_enable_pc = 0, mie_enable_next_pc = 0;
  uint64_t ecall_pc = 0, after_ecall_pc = 0;
  uint64_t wfi_pc = 0, after_wfi_pc = 0;
  uint64_t exit_pc = 0;
};

// The handler. It logs `{mcause, mepc}` from inside the trap and then resumes
// by the architectural rule: after the faulting instruction for an exception
// (skip 4 bytes) and *at* the interrupted instruction for an interrupt (skip
// nothing, because the instruction has not executed yet). The distinction is
// `mcause[63]`, read from the machine rather than assumed.
void EmitHandler(Asm* a, uint64_t log_addr) {
  a->Mark("handler");
  a->Csrrs(5, 0x342, 0);        // t0 = mcause
  a->Csrrs(6, 0x341, 0);        // t1 = mepc
  a->LiAbs(28, log_addr);       // t3 = &log
  a->Ld(29, 28, 0);             // t4 = count
  a->Slli(30, 29, 4);           // t5 = count * 16
  a->Add(30, 30, 28);
  a->Sd(5, 30, 16);             // record[count].mcause
  a->Sd(6, 30, 24);             // record[count].mepc
  a->Addi(29, 29, 1);
  a->Sd(29, 28, 0);             // count++
  a->Srli(7, 5, 63);            // t2 = mcause >> 63  (1 for an interrupt)
  a->Xori(7, 7, 1);             // 0 for an interrupt, 1 for an exception
  a->Slli(7, 7, 2);             // 4 for an exception, 0 for an interrupt
  a->Add(6, 6, 7);
  a->Csrrw(0, 0x341, 6);        // mepc = the resume PC
  a->Mret();
}

Program BuildProgram(const ScenarioParams& p) {
  Program prog{Asm(MOSAIC_RESET_VECTOR)};
  Asm& a = prog.asm_;
  a.La(5, "handler");
  a.Csrrw(0, 0x305, 5);         // mtvec = handler
  a.Csrrw(0, 0x300, 0);         // mstatus = 0 (MIE clear, MPP already M)
  a.Csrrw(0, 0x304, 0);         // mie = 0

  // Window P1: the timer and external sources are asserted while everything is
  // disabled, and the timer must stay pending without ever being offered.
  for (int i = 0; i < p.pad1; i++) {
    if (i == p.pad1 / 2) a.Mark("t1");
    a.Addi(0, 0, 0);
  }
  prog.t1_pc = a.Label("t1");
  a.LiAbs(5, kBitMie);
  a.Csrrw(0, 0x300, 5);         // mstatus.MIE = 1, but mie is still 0

  // Window P2: MIE set, no source enabled. The interrupt is armed at the end.
  for (int i = 0; i < p.pad2; i++) a.Addi(0, 0, 0);
  a.LiAbs(5, kBitMtie);
  a.Mark("mie_enable");
  a.Csrrw(0, 0x304, 5);         // the enable CSR, immediately before the boundary
  prog.mie_enable_pc = a.Label("mie_enable");
  a.Addi(0, 0, 0);              // the boundary the timer must be accepted at
  prog.mie_enable_next_pc = prog.mie_enable_pc + 4;

  // Window P3: the software interrupt is asserted here, while MIE is still 1
  // and no software bit is enabled, so nothing is taken and nothing is lost.
  for (int i = 0; i < p.pad3; i++) {
    if (i == 4) a.Mark("t2");
    a.Addi(0, 0, 0);
  }
  prog.t2_pc = a.Label("t2");
  a.Csrrw(0, 0x300, 0);         // mstatus.MIE = 0
  a.LiAbs(5, kBitMsie);
  a.Csrrw(0, 0x304, 5);         // MSIE = 1, MTIE = 0
  a.LiAbs(5, kBitMie);
  a.Csrrw(0, 0x300, 5);         // MIE = 1, immediately before the ecall
  a.Mark("ecall");
  a.Ecall();
  prog.ecall_pc = a.Label("ecall");
  a.Addi(0, 0, 0);              // the boundary the software interrupt must be taken at
  prog.after_ecall_pc = prog.ecall_pc + 4;
  a.Csrrw(0, 0x300, 0);
  a.Csrrw(0, 0x304, 0);

  // Window P4: the external source is released at a numbered retirement
  // boundary, after the two traps that could have been confused with it.
  for (int i = 0; i < p.pad4; i++) {
    if (i == p.pad4 / 2) a.Mark("t3");
    a.Addi(0, 0, 0);
  }
  prog.t3_pc = a.Label("t3");
  a.LiAbs(5, kBitMtie);
  a.Csrrw(0, 0x304, 5);         // MTIE = 1 (enabled)
  a.Csrrw(0, 0x300, 0);         // MIE = 0, so the WFI halts and the wake takes no trap
  a.Mark("wfi");
  a.Wfi();
  prog.wfi_pc = a.Label("wfi");
  prog.after_wfi_pc = prog.wfi_pc + 4;
  // Nothing after the WFI touches mstatus or mie. The halt gates *fetch*, not
  // the instructions already in flight, so the instructions behind the WFI
  // retire while the hart is halted; if one of them cleared the timer's enable
  // the wake would be lost with the source still asserted. Leaving the enables
  // alone is safe because mstatus.MIE is clear, so no interrupt can be taken.

  // Window P5: straight line to the exit protocol, long enough that the
  // instructions in flight behind the WFI cannot reach the exit store before
  // the wake has been delivered.
  for (int i = 0; i < p.pad5; i++) a.Addi(0, 0, 0);
  a.Mark("exit");
  prog.exit_pc = a.Label("exit");
  a.LiAbs(5, 1);
  a.LiAbs(6, MOSAIC_TOHOST);
  a.Sd(5, 6, 0);
  // Straight-line filler: a run past the exit store must not reach the handler
  // before the store drains. No branch is executed anywhere in this program, so
  // the branch predictor is never trained and holds no state across a replay.
  for (int i = 0; i < 64; i++) a.Addi(0, 0, 0);
  EmitHandler(&a, kLogAddr);
  a.Resolve();
  prog.handler_pc = a.Label("handler");
  return prog;
}

// ============================================================================
// The stimulus timeline
// ============================================================================
enum class TriggerKind { kRetire, kHalt };

constexpr int kPinSoft = 1;
constexpr int kPinTimer = 2;
constexpr int kPinExt = 4;

struct TimelineEvent {
  const char* label;
  TriggerKind kind;
  uint64_t pc;      // for kRetire: the landmark instruction's PC
  uint64_t halt;    // for kHalt: the completed halted cycles
  int set_mask;
  int clr_mask;
};

// ============================================================================
// One run of the program
// ============================================================================
RunResult RunOnce(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                  const Geometry& g, const Program& prog,
                  const ScenarioParams& params, const std::string& phase) {
  RunResult rr;
  rr.trace_hash = 1469598103934665603ull;
  // The host wall clock enters *here*, once per run, and nowhere else. In the
  // shipping build it is zero and nothing below changes; under the first control
  // it is a per-run value derived from the host's steady clock, so the two runs
  // of an identical program and seed are no longer identical.
  const uint64_t wall_tag = HostWallTag();
  const int wall_shift = (wall_tag == 0) ? 0 : static_cast<int>(wall_tag % 8);
  rr.trace_hash = (rr.trace_hash ^ wall_tag) * 1099511628211ull;

  ProgImage image;
  for (size_t i = 0; i < prog.asm_.words().size(); i++) {
    image.Put(g.reset_vector + 4ull * i, prog.asm_.words()[i]);
  }
  // Off-image fetches answer ECALL; the filler is a self-loop so the fetch that
  // runs past the program can neither fault nor stop the machine.
  for (uint64_t addr = g.reset_vector + 4ull * prog.asm_.words().size();
       addr < g.reset_vector + 0x1000ull; addr += 4) {
    image.Put(addr, 0x0000006fu);
  }

  mosaic::MemoryModel dut_mem;
  // The program's text is not copied into the data model: a store to
  // MOSAIC_TOHOST lives inside the text span, and copying the words there would
  // make the exit protocol indistinguishable from an instruction write.
  dut_mem.SetInputWord(0);

  Harness harness(dut, reporter, kRunCycles, &image, &dut_mem);
  harness.Configure(g);
  harness.Phase(phase);
  harness.SetResult(&rr);
  harness.SetPins(false, false, false);
  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);

  const TimelineEvent events[] = {
      {"assert timer + external (masked)",
       TriggerKind::kRetire, prog.t1_pc, 0, kPinTimer | kPinExt, 0},
      {"assert software (masked)", TriggerKind::kRetire, prog.t2_pc, 0, kPinSoft, 0},
      {"release external", TriggerKind::kRetire, prog.t3_pc, 0, 0, kPinExt},
      {"assert timer + external (WFI wake)", TriggerKind::kHalt, 0, params.wake_halt,
       kPinTimer | kPinExt, 0},
  };
  const int kEvents = static_cast<int>(sizeof(events) / sizeof(events[0]));
  bool scheduled[4] = {false, false, false, false};
  bool applied[4] = {false, false, false, false};
  uint64_t fire_at[4] = {0, 0, 0, 0};

  int pins = 0;
  size_t traps_before = 0;
  while (true) {
    const uint64_t now = harness.cycles();
    // (1) Fire the events whose trigger has been satisfied by the state seen so
    // far. A retirement boundary is named by the PC of the instruction that
    // retired; the halt trigger is the halt's own duration.
    for (int k = 0; k < kEvents; k++) {
      if (scheduled[k]) continue;
      bool ready = false;
      if (events[k].kind == TriggerKind::kRetire) {
        for (const Retire& r : harness.retires()) {
          if (r.pc == events[k].pc) {
            ready = true;
            break;
          }
        }
      } else {
        ready = harness.halt_streak() >= events[k].halt;
      }
      if (!ready) continue;
      scheduled[k] = true;
      fire_at[k] = now + static_cast<uint64_t>(wall_shift);
    }
    // (2) Apply the scheduled events, recording the boundary they were applied at.
    for (int k = 0; k < kEvents; k++) {
      if (!scheduled[k] || applied[k] || now < fire_at[k]) continue;
      applied[k] = true;
      pins = (pins | events[k].set_mask) & ~events[k].clr_mask;
      StimulusRecord s;
      s.label = events[k].label;
      s.host_tag = wall_tag;
      if (events[k].kind == TriggerKind::kRetire) {
        s.trigger = "retire:" + U64(events[k].pc);
      } else {
        s.trigger = "halt>=" + Dec(events[k].halt);
      }
      s.trigger_cycle = now;
      s.assert_boundary = dut->o_commit_o;
      s.assert_cycle = now;
      s.soft = (pins & kPinSoft) ? 1 : 0;
      s.timer = (pins & kPinTimer) ? 1 : 0;
      s.ext = (pins & kPinExt) ? 1 : 0;
      rr.stimuli.push_back(s);
      if ((pins & kPinExt) != 0 && harness.halt_streak() > 0) {
        rr.ext_asserted_during_halt = true;
      }
      if (events[k].kind == TriggerKind::kHalt) {
        rr.wake_assert_cycle = now;
        rr.halt_cycles_at_wake_assert = harness.halt_streak();
      }
    }

    harness.SetPins((pins & kPinSoft) != 0, (pins & kPinTimer) != 0,
                    (pins & kPinExt) != 0);
    traps_before = harness.traps().size();
    harness.Cycle(false);

    // (3) The automatic release for the *first* timer window: the source is
    // dropped because the machine took it. The wake source is deliberately
    // *not* released here: a CLINT-style level stays asserted until software
    // clears it, and releasing it in the cycle the halt falls would race the
    // WFI macro's own completion -- the halt would be re-armed by a WFI whose
    // writeback is still presented while the source has gone. Holding it is
    // both the physical model and the race-free one.
    if (harness.traps().size() > traps_before) {
      const TrapRecord& t = harness.traps().back();
      if (t.is_irq && t.cause == kCauseMti) pins &= ~kPinTimer;
      if (t.is_irq && t.cause == kCauseMsi) pins &= ~kPinSoft;
    }
    if (harness.halt_fell()) {
      if (rr.wake_cycle == 0) rr.wake_cycle = harness.cycles();
      harness.ClearHaltFell();
    }

    if (dut_mem.finished()) break;
    if (harness.cycles() >= kRunCycles) {
      Fail(phase, "the program did not reach its exit protocol: " + harness.State());
    }
  }
  // Let the store queue and the last events settle, with the sources released.
  harness.SetPins(false, false, false);
  for (int i = 0; i < 8; i++) harness.Cycle(false);

  rr.traps = harness.traps();
  rr.retires = harness.retires();
  rr.cycles = harness.cycles();
  rr.retired = harness.retires().size();

  // The handler's log, read from the memory model.
  uint64_t count = 0;
  if (dut_mem.Read(kLogAddr, 8, &count) != mosaic::AccessStatus::kOk) {
    Fail(phase, "the handler log count is not readable");
  }
  rr.log_count = count;
  for (uint64_t i = 0; i < count && i < kLogMax; i++) {
    uint64_t cause = 0, epc = 0;
    if (dut_mem.Read(kLogAddr + 16 + 16 * i, 8, &cause) != mosaic::AccessStatus::kOk ||
        dut_mem.Read(kLogAddr + 24 + 16 * i, 8, &epc) != mosaic::AccessStatus::kOk) {
      Fail(phase, "the handler log record " + Dec(i) + " is not readable");
    }
    rr.log.emplace_back(cause, epc);
  }
  if (!dut_mem.finished() || !dut_mem.passed()) {
    Fail(phase, "the program did not reach its exit protocol with the pass code");
  }
  return rr;
}

// ============================================================================
// The checks
// ============================================================================
std::string CauseName(uint64_t cause) {
  if (cause == kCauseMsi) return "MSI(3)";
  if (cause == kCauseMti) return "MTI(7)";
  if (cause == kCauseMei) return "MEI(11)";
  if (cause == kCauseEcall) return "ECALL(11)";
  return U64(cause);
}

const TrapRecord* FindTrap(const RunResult& rr, bool is_irq, uint64_t cause) {
  for (const TrapRecord& t : rr.traps) {
    if (t.is_irq == is_irq && t.cause == cause) return &t;
  }
  return nullptr;
}

void CheckRun(mosaic::Reporter* reporter, const std::string& phase,
              const Program& prog, const ScenarioParams& params,
              const RunResult& rr) {
  auto Check = [&](const std::string& what, bool ok, const std::string& detail) {
    reporter->Check(ok, phase + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase, what + ": " + detail);
  };

  std::printf("  [%s] %llu cycles, %llu retires, %zu traps, trace=%s\n",
              phase.c_str(), static_cast<unsigned long long>(rr.cycles),
              static_cast<unsigned long long>(rr.retired), rr.traps.size(),
              U64(rr.trace_hash).c_str());
  std::printf("  [%s] evidence: timer-pending-disabled=%llu enabled-but-MIE-clear=%llu "
              "ext-driven=%llu halt_cycles=%llu ext_during_halt=%d log=%llu\n",
              phase.c_str(),
              static_cast<unsigned long long>(rr.cycles_timer_pending_disabled),
              static_cast<unsigned long long>(rr.cycles_enabled_unmasked_no_mie),
              static_cast<unsigned long long>(rr.cycles_ext_high),
              static_cast<unsigned long long>(rr.halt_cycles),
              rr.ext_asserted_during_halt ? 1 : 0,
              static_cast<unsigned long long>(rr.log_count));
  std::printf("  [%s] asserted boundaries:\n", phase.c_str());
  for (const StimulusRecord& s : rr.stimuli) {
    std::printf("      %-38s %-22s boundary=%s cycle=%llu -> soft=%llu timer=%llu ext=%llu\n",
                s.label.c_str(), s.trigger.c_str(), U64(s.assert_boundary).c_str(),
                static_cast<unsigned long long>(s.assert_cycle),
                static_cast<unsigned long long>(s.soft),
                static_cast<unsigned long long>(s.timer),
                static_cast<unsigned long long>(s.ext));
  }
  std::printf("  [%s] accepted boundaries:\n", phase.c_str());
  for (const TrapRecord& t : rr.traps) {
    std::printf("      boundary=%-5s cycle=%-5llu cause=%-10s mepc=%s head_pc=%s "
                "pre_retire=%s retired_in_cycle=%d irq=%d\n",
                U64(t.boundary).c_str(), static_cast<unsigned long long>(t.cycle),
                CauseName(t.cause).c_str(), U64(t.mepc).c_str(), U64(t.head_pc).c_str(),
                t.has_pre_retire ? U64(t.pre_retire_pc).c_str() : "(none)",
                t.retired_in_cycle ? 1 : 0, t.is_irq ? 1 : 0);
  }

  // ---------------------------------------------------------------- the shape
  Check("exactly three traps were taken (the timer interrupt, the software "
        "interrupt and the ecall)",
        rr.traps.size() == 3, "traps=" + Dec(rr.traps.size()));
  if (rr.traps.size() != 3) return;

  // The card's second fail mode: an interrupt is not an instruction and must
  // not appear in the retirement stream -- nothing retires in its trap cycle.
  for (const TrapRecord& t : rr.traps) {
    if (!t.is_irq) continue;
    bool ok = !t.retired_in_cycle;
#ifdef MOSAIC_IRQ_MUTANT_ORDINARY_RETIRE
    // NEGATIVE CONTROL 2: the checker treats the interrupt as an ordinary
    // retiring instruction -- it requires the interrupted instruction to retire
    // in the trap cycle. A machine that correctly suppresses retirement in the
    // trap cycle then fails this check, which is exactly what it is for.
    ok = t.retired_in_cycle;
#endif
    Check("an interrupt is not an instruction: no ordinary instruction retires in "
          "the interrupt's trap cycle",
          ok,
          "cause=" + CauseName(t.cause) + " cycle=" + Dec(t.cycle) +
              " retired_in_cycle=" + Dec(t.retired_in_cycle ? 1 : 0));
  }
  for (const TrapRecord& t : rr.traps) {
    if (!t.is_irq) continue;
    Check("an interrupt's cause is an enabled source, never the external line "
          "whose enable bit is read-only zero",
          t.cause == kCauseMti || t.cause == kCauseMsi,
          "cause=" + CauseName(t.cause));
    Check("an interrupt's mepc names the instruction it is taken before",
          t.mepc == t.head_pc,
          "mepc=" + U64(t.mepc) + " head=" + U64(t.head_pc));
    Check("an interrupt's target is the installed trap vector",
          t.target == prog.handler_pc,
          "target=" + U64(t.target) + " handler=" + U64(prog.handler_pc));
  }
  Check("exactly one timer and one software interrupt were accepted",
        FindTrap(rr, true, kCauseMti) != nullptr &&
            FindTrap(rr, true, kCauseMsi) != nullptr,
        "an expected interrupt is missing");

  const TrapRecord* ecall = FindTrap(rr, false, kCauseEcall);
  const TrapRecord* mti = FindTrap(rr, true, kCauseMti);
  const TrapRecord* msi = FindTrap(rr, true, kCauseMsi);
  Check("the synchronous trap is the ecall with the ISA's cause",
        ecall != nullptr, "the ecall trap is missing");
  if (ecall != nullptr) {
    Check("the exception's mepc is the ecall itself",
          ecall->mepc == prog.ecall_pc,
          "mepc=" + U64(ecall->mepc) + " expected " + U64(prog.ecall_pc));
  }

  // ------------------------------------- pending-but-disabled, and not lost
  Check("the timer interrupt was accepted", mti != nullptr,
        "no MTI interrupt was taken");
  Check("the timer was pending while its enable bit was clear, before anything "
        "was enabled",
        rr.cycles_timer_pending_disabled > 0,
        "cycles=" + Dec(rr.cycles_timer_pending_disabled));
  Check("an enabled interrupt was not offered while mstatus.MIE was clear",
        rr.cycles_enabled_unmasked_no_mie > 0,
        "cycles=" + Dec(rr.cycles_enabled_unmasked_no_mie));
  if (mti != nullptr) {
    const bool enable_was_preceding =
        mti->has_pre_retire && mti->pre_retire_pc == prog.mie_enable_pc &&
        mti->mepc == prog.mie_enable_next_pc;
    bool not_lost = enable_was_preceding;
#ifdef MOSAIC_IRQ_MUTANT_LOST_PENDING
    // NEGATIVE CONTROL 3: the checker is mutated to the fail mode's belief --
    // that an interrupt asserted while masked is dropped at the boundary, so no
    // acceptance follows its enable. The machine takes it, so this fails.
    not_lost = !enable_was_preceding;
#endif
    Check("an unaccepted interrupt is not lost: the timer asserted while masked is "
          "accepted at the boundary immediately after its enable CSR",
          not_lost,
          "accept mepc=" + U64(mti->mepc) + " expected " +
              U64(prog.mie_enable_next_pc) + " pre-retire pc=" +
              U64(mti->pre_retire_pc) + " expected " + U64(prog.mie_enable_pc));
  }

  // ------------------------------- a synchronous trap competing with an interrupt
  //
  // This machine's boundary rule (`core_can_trap` in mosaic_core.sv) excludes a
  // head whose exception is already recorded and a head that is a *staged*
  // system macro, but it does not wait for a system macro's staging to be
  // established: for one cycle the head is an ecall whose staging entry is
  // still occupied by the CSR access in front of it. In that window an enabled,
  // unmasked pending interrupt is offered and taken, and the ecall is then
  // reached again because an interrupt's `mepc` names the instruction the
  // handler must resume *at*. Both are delivered exactly once; the case states
  // what the machine does rather than what would be tidier.
  Check("a synchronous trap competed with a pending, enabled, unmasked interrupt "
        "from the same boundary",
        msi != nullptr && ecall != nullptr && msi->head_pc == prog.ecall_pc &&
            msi->cycle < ecall->cycle,
        msi == nullptr || ecall == nullptr
            ? "the competition was not produced"
            : "msi head=" + U64(msi->head_pc) + " expected " + U64(prog.ecall_pc) +
                  ", msi cycle=" + Dec(msi->cycle) + " ecall cycle=" +
                  Dec(ecall->cycle));
  if (msi != nullptr) {
    Check("the competing interrupt was legitimately offered: an enabled pending "
          "bit and mstatus.MIE",
          ((msi->mip & msi->mie) != 0) && ((msi->mstatus & kBitMie) != 0) &&
              msi->irq_valid != 0,
          "mip=" + U64(msi->mip) + " mie=" + U64(msi->mie) + " mstatus=" +
              U64(msi->mstatus) + " irq_valid=" + Dec(msi->irq_valid));
  }
  Check("the ecall the interrupt preempted was neither lost nor double-executed: "
        "exactly one ecall trap, with the interrupt's mepc naming it",
        ecall != nullptr && msi != nullptr && ecall->cycle > msi->cycle &&
            ecall->mepc == prog.ecall_pc && msi->mepc == prog.ecall_pc,
        ecall == nullptr || msi == nullptr
            ? "the ecall or the interrupt is missing"
            : "ecall mepc=" + U64(ecall->mepc) + " msi mepc=" + U64(msi->mepc) +
                  " expected " + U64(prog.ecall_pc));

  // ------------------------------------------------------------------- WFI
  Check("the WFI really halted (an enabled pending interrupt would have kept it "
        "running)",
        rr.wfi_halted && rr.halt_cycles > 0,
        "halted=" + Dec(rr.wfi_halted ? 1 : 0) + " halt_cycles=" + Dec(rr.halt_cycles));
  Check("the wake came from the timer asserted during the halt, after the halt had "
        "already lasted the stimulated number of cycles",
        rr.wake_assert_cycle > 0 && rr.wake_cycle > rr.wake_assert_cycle &&
            rr.halt_cycles_at_wake_assert >= params.wake_halt,
        "wake_assert=" + Dec(rr.wake_assert_cycle) + " wake=" + Dec(rr.wake_cycle) +
            " halted_at_assert=" + Dec(rr.halt_cycles_at_wake_assert) + " expected >=" +
            Dec(params.wake_halt));
  Check("the external source, pending and never enable-able, was asserted during "
        "the halt and did not wake it",
        rr.ext_asserted_during_halt,
        "the external line was never asserted while the halt was asserted");
  {
    bool no_trap_after_wake = true;
    for (const TrapRecord& t : rr.traps) {
      if (t.cycle >= rr.wake_assert_cycle) no_trap_after_wake = false;
    }
    Check("the wake took no trap (mstatus.MIE was clear)",
          no_trap_after_wake && rr.wake_assert_cycle > 0,
          "a trap was taken at or after the wake assertion");
  }

  // -------------------------------------------------------------- external
  //
  // The external line is driven here, and the p0 contract makes it inert: mip
  // bit 11 (MEIP) and mie bit 11 (MEIE) are both outside the profile's
  // implemented set -- config/csr/mode_m.json lists bits 15:8 as WPRI, and the
  // generated `MOSAIC_CSR_WMASK_MIP`/`MOSAIC_CSR_WMASK_MIE` are 0x88, so both
  // read as zero and neither can be written. The case therefore states what
  // "assert the external interrupt" means in this profile: it is asserted and
  // released at numbered retirement boundaries, it produces no trap, it does not
  // wake the halt, and it does not appear in the architectural mip or mie at
  // all.
  bool ext_asserted = false, ext_released = false;
  for (const StimulusRecord& s : rr.stimuli) {
    if (s.ext) ext_asserted = true;
    if (!s.ext && ext_asserted) ext_released = true;
  }
  Check("the external line was asserted and released at numbered boundaries",
        ext_asserted && ext_released && rr.cycles_ext_high > 0,
        "asserted=" + Dec(ext_asserted ? 1 : 0) + " released=" +
            Dec(ext_released ? 1 : 0) + " cycles=" + Dec(rr.cycles_ext_high));
  Check("the external interrupt was never accepted",
        FindTrap(rr, true, kCauseMei) == nullptr,
        "an MEI interrupt was taken");
  Check("the external line does not appear in the architectural mip or mie: bit 11 "
        "is WPRI and reads zero in this profile",
        !rr.mip11_seen && !rr.mie11_seen,
        "mip[11]_seen=" + Dec(rr.mip11_seen ? 1 : 0) + " mie[11]_seen=" +
            Dec(rr.mie11_seen ? 1 : 0));

  // ------------------------------------------------------- the handler's log
  Check("the handler logged every trap the machine took",
        rr.log_count == rr.traps.size() && rr.log.size() == rr.log_count,
        "log_count=" + Dec(rr.log_count) + " traps=" + Dec(rr.traps.size()));
  for (size_t i = 0; i < rr.log.size() && i < rr.traps.size(); i++) {
    Check("the handler's log record " + Dec(i) + " matches the trap the machine took",
          rr.log[i].first == rr.traps[i].cause && rr.log[i].second == rr.traps[i].mepc,
          "logged {" + U64(rr.log[i].first) + "," + U64(rr.log[i].second) +
              "} machine {" + U64(rr.traps[i].cause) + "," + U64(rr.traps[i].mepc) + "}");
  }

  (void)params;
}

void CheckReplay(mosaic::Reporter* reporter, const std::string& phase,
                 const RunResult& a, const RunResult& b) {
  auto Check = [&](const std::string& what, bool ok, const std::string& detail) {
    reporter->Check(ok, phase + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase, what + ": " + detail);
  };

  Check("the same stimulus replays to the same asserted boundaries",
        a.stimuli.size() == b.stimuli.size(),
        "run A has " + Dec(a.stimuli.size()) + ", run B " + Dec(b.stimuli.size()));
  if (a.stimuli.size() == b.stimuli.size()) {
    for (size_t i = 0; i < a.stimuli.size(); i++) {
      Check("the same stimulus replays to the same asserted boundary " + Dec(i) +
                " (" + a.stimuli[i].label + ")",
            a.stimuli[i].assert_boundary == b.stimuli[i].assert_boundary &&
                a.stimuli[i].assert_cycle == b.stimuli[i].assert_cycle &&
                a.stimuli[i].host_tag == b.stimuli[i].host_tag &&
                a.stimuli[i].trigger == b.stimuli[i].trigger,
            "A boundary=" + Dec(a.stimuli[i].assert_boundary) + " cycle=" +
                Dec(a.stimuli[i].assert_cycle) + " host_tag=" +
                U64(a.stimuli[i].host_tag) + ", B boundary=" +
                Dec(b.stimuli[i].assert_boundary) + " cycle=" +
                Dec(b.stimuli[i].assert_cycle) + " host_tag=" +
                U64(b.stimuli[i].host_tag));
    }
  }

  Check("the same stimulus replays to the same accepted boundaries",
        a.traps.size() == b.traps.size(),
        "run A has " + Dec(a.traps.size()) + " traps, run B " + Dec(b.traps.size()));
  if (a.traps.size() == b.traps.size()) {
    for (size_t i = 0; i < a.traps.size(); i++) {
      Check("the same stimulus replays to the same accepted boundary " + Dec(i) +
                " (" + CauseName(a.traps[i].cause) + ")",
            a.traps[i].boundary == b.traps[i].boundary &&
                a.traps[i].cause == b.traps[i].cause &&
                a.traps[i].mepc == b.traps[i].mepc &&
                a.traps[i].is_irq == b.traps[i].is_irq,
            "A boundary=" + Dec(a.traps[i].boundary) + " cause=" +
                CauseName(a.traps[i].cause) + " mepc=" + U64(a.traps[i].mepc) +
                ", B boundary=" + Dec(b.traps[i].boundary) + " cause=" +
                CauseName(b.traps[i].cause) + " mepc=" + U64(b.traps[i].mepc));
    }
  }

  Check("the same stimulus replays to the same per-cycle trace",
        a.trace_hash == b.trace_hash && a.cycles == b.cycles &&
            a.retired == b.retired,
        "A hash=" + U64(a.trace_hash) + " cycles=" + Dec(a.cycles) + " retires=" +
            Dec(a.retired) + ", B hash=" + U64(b.trace_hash) + " cycles=" +
            Dec(b.cycles) + " retires=" + Dec(b.retired));
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
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    const std::string repo = FindRepoRoot();
    (void)repo;
    if (geometry.reset_vector != MOSAIC_RESET_VECTOR) {
      Fail("geometry", "the reset vector is not the profile's: " +
                           U64(geometry.reset_vector));
    }

    const ScenarioParams params = DeriveParams(options.seed);
    const Program program = BuildProgram(params);

    std::printf("irq.replay_timeline: seed=%llu pad1=%d pad2=%d pad3=%d pad4=%d pad5=%d "
                "wake_halt=%llu\n",
                static_cast<unsigned long long>(options.seed), params.pad1, params.pad2,
                params.pad3, params.pad4, params.pad5,
                static_cast<unsigned long long>(params.wake_halt));
    std::printf("  program: %zu words, handler=%s mie_enable=%s ecall=%s wfi=%s exit=%s\n",
                program.asm_.words().size(), U64(program.handler_pc).c_str(),
                U64(program.mie_enable_pc).c_str(), U64(program.ecall_pc).c_str(),
                U64(program.wfi_pc).c_str(), U64(program.exit_pc).c_str());

    // Run 1: the stimulus, from a cold reset.
    const RunResult run_a = RunOnce(&dut, &reporter, geometry, program, params,
                                    "irq-timeline-a");
    // Run 2: identical program, identical seed, through a second reset and a
    // fresh memory model. The determinism claim is about the machine, so the
    // two runs share nothing but the code.
    const RunResult run_b = RunOnce(&dut, &reporter, geometry, program, params,
                                    "irq-timeline-b");

    CheckRun(&reporter, "irq-timeline", program, params, run_a);
    CheckRun(&reporter, "irq-timeline-replay", program, params, run_b);
    CheckReplay(&reporter, "irq-timeline", run_a, run_b);

    detail = "checks=" + Dec(reporter.checks()) + " cycles=" + Dec(run_a.cycles) +
             " retires=" + Dec(run_a.retired) + " traps=" + Dec(run_a.traps.size()) +
             " trace=" + U64(run_a.trace_hash) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the interrupt boundary holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
