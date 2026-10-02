// ============================================================================
// tb_core_csr_rules.cpp -- CASE=csr.rule_ledger, work package V-017.
//
// The DUT is the integrated p0 core with the CSR file (I-019), the interrupt
// decision (I-020) and the trap controller wired in. The case is the CSR
// legality-relationship comparator the validation plan asks for: for every rule
// in config/csr/rule_ledger.json it drives a legal access and its *adjacent
// illegal* neighbour and requires the machine to answer as the rule says.
//
// -------------------------------------------------------------- the card
//
// docs/validation-plan.md section 5, V-017. Inputs: the CSR implementation
// table, the specification version, a comparison-rule schema. Action: for every
// implemented CSR, cover legal writes, reserved/WARL values, read-only bits,
// permission errors and aliases; and test counters and the FS/VS early-dirty
// rules independently. Outputs: a per-bit rule ledger and, for every rule, a
// positive and a negative example. Pass: fields that are determined are compared
// strictly; where a relationship is merely *allowed*, the comparator accepts at
// least one legal differing result and rejects the adjacent illegal one; every
// rule carries its clause and its applicability preconditions.
//
// Fail: not comparing all of mstatus/mip; generating waivers automatically from
// a difference; accepting an unimplemented CSR unconditionally as zero.
//
// ------------------------------------------------------------- how it works
//
// The ledger is the program. `build/<profile>/sim/mosaic_csr_rules.h` is
// generated from config/csr/rule_ledger.json, and this driver walks it: every
// example becomes one instruction sequence (load the operand, perform the CSR
// operation, read the CSR back, store the read-back to a RAM slot) and every
// rule must be visited by both its examples or the coverage assertion at the end
// fails the case. A rule that is silently skipped therefore cannot pass -- it
// leaves the coverage count short.
//
// The machine is the oracle for "did this access trap": an illegal CSR access
// raises the illegal-instruction exception, and the driver matches the set of
// observed trap PCs against the set the ledger requires, exactly. A denied
// access must trap and must leave the destination register unchanged (the
// read-back is the *canary* it held before the access), so an unimplemented CSR
// accepted as zero fails the case rather than passing it.
//
// The expectations are the ledger's, not the machine's: the driver never derives
// an expected read from an observed one. The one place a range is allowed is
// where the specification makes the value genuinely non-deterministic -- a
// free-running counter that ticks between two reads -- and even there the rule
// names the adjacent illegal result (a value the counter may never hold).
//
// -------------------------------------------------------------- controls
//
// The card names three fail modes and each has a `-D` control, rebuilt by
// tools/run_csrrules_controls.py from a deleted build directory:
//
//   MOSAIC_CSRRULES_MUTANT_SKIP_RULE   the driver silently drops one rule's
//                                      stimulus; the coverage assertion must
//                                      fail ("was never visited").
//   MOSAIC_CSRRULES_MUTANT_AUTO_WAIVER the comparator is mutated to the
//                                      auto-waiver the fail mode describes: it
//                                      accepts the adjacent illegal value, so
//                                      the correct canonicalisation fails.
//   MOSAIC_CSRRULES_MUTANT_UNIMPL_ZERO the comparator is mutated to expect an
//                                      unimplemented CSR to read zero without
//                                      trapping; the correct trap fails.
//
// All three are properties of the checker and cannot be expressed as an RTL
// mutant, because no RTL module decides what this case expects -- the ledger
// does. They are labelled DRIVER in results/reports/V-017-csr.md.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_csr_rules.h"
#include "mosaic_csr_table.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic_ref::DataMem;

namespace {

constexpr int kResetCycles = 4;
constexpr uint64_t kProgramCycles = 2000000;
constexpr uint64_t kStallCycles = 20000;

// The program's data layout. The literal pool and the read-back slots live in
// RAM, far above the program text (which is under 12 KiB), so neither can collide
// with the code or with MOSAIC_TOHOST.
constexpr uint64_t kLiteralBase = 0x80008000ull;
constexpr uint64_t kSlotBase = 0x80010000ull;
// The platform's real-time counter, driven by the harness. `time` is a read-only
// alias of this register, so a value with a distinctive high word makes a broken
// alias (one that reads zero) fail the rule.
constexpr uint64_t kMtimeBase = 0x100000000ull;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }
bool Debug() { return std::getenv("CSRRULES_DEBUG") != nullptr; }

// ============================================================================
// The instruction image and a very small assembler
// ============================================================================
class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x0000006fu : it->second;   // jal x0, 0
  }

 private:
  std::map<uint64_t, uint32_t> words_;
};

class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}

  uint64_t pc() const { return base_ + 4ull * words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }

  static uint32_t I(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
    return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) |
           (f3 << 12) | (rd << 7) | op;
  }
  static uint32_t S(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 5) & 0x7Fu) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           ((u & 0x1Fu) << 7) | op;
  }
  static uint32_t U(uint32_t imm20, uint32_t rd, uint32_t op) {
    return (imm20 << 12) | (rd << 7) | op;
  }

  void Addi(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x13)); }
  void Lui(uint32_t rd, uint32_t imm20) { Emit(U(imm20, rd, 0x37)); }
  void Ld(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 3, rd, 0x03)); }
  void Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { Emit(S(imm, rs2, rs1, 3, 0x23)); }
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(csr, rs1, 1, rd, 0x73)); }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(csr, rs1, 2, rd, 0x73)); }
  void Csrrc(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(csr, rs1, 3, rd, 0x73)); }
  void Csrr(uint32_t rd, uint32_t csr) { Csrrs(rd, csr, 0); }
  void Mret() { Emit(0x30200073u); }
  void Jalr(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x67)); }
  void JalSelf() { Emit(0x0000006fu); }

  void Mark(const std::string& label) { labels_[label] = pc(); }

  // Load an absolute 64-bit address PC-relative. `lui` is unusable for the p0
  // addresses: 0x80008000 has bit 31 set, so LUI's sign extension produces
  // 0xFFFFFFFF80008000 and every access faults with tval naming the wrapped
  // address. auipc+addi keeps the whole 64-bit result.
  void LaAbs(uint32_t rd, uint64_t target) {
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(pc());
    const uint32_t lo = static_cast<uint32_t>(delta) & 0xFFFu;
    const uint32_t hi = static_cast<uint32_t>((delta + 0x800) >> 12) & 0xFFFFFu;
    Emit(U(hi, rd, 0x17));
    Emit(I(static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0), rd, 0, rd, 0x13));
  }

  void La(uint32_t rd, const std::string& label) {
    Fixup fix;
    fix.at = words_.size();
    fix.rd = rd;
    fix.label = label;
    fixups_.push_back(fix);
    words_.push_back(0);
    words_.push_back(0);
  }

  void Resolve() {
    for (const Fixup& fix : fixups_) {
      auto it = labels_.find(fix.label);
      if (it == labels_.end()) continue;
      const int64_t here = static_cast<int64_t>(base_ + 4ull * fix.at);
      const int64_t delta = static_cast<int64_t>(it->second) - here;
      const uint32_t lo = static_cast<uint32_t>(delta) & 0xFFFu;
      const uint32_t hi = static_cast<uint32_t>((delta + 0x800) >> 12) & 0xFFFFFu;
      words_[fix.at] = U(hi, fix.rd, 0x17);
      words_[fix.at + 1] = I(static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0),
                             fix.rd, 0, fix.rd, 0x13);
    }
  }

 private:
  void Emit(uint32_t w) { words_.push_back(w); }
  struct Fixup {
    size_t at;
    uint32_t rd;
    std::string label;
  };
  uint64_t base_;
  std::vector<uint32_t> words_;
  std::vector<Fixup> fixups_;
  std::map<std::string, uint64_t> labels_;
};

// ============================================================================
// The instruction port (one outstanding response, fixed latency)
// ============================================================================
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };
  explicit Imem(const ProgImage* img) : img_(img) {}
  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint32_t ResponseWord() const { return img_->Word(ready_.front().addr); }
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

struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint64_t reset_vector = 0;
  uint32_t has_s = 0;
  uint32_t has_u = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  g.has_s = dut->o_geom_has_s_o;
  g.has_u = dut->o_geom_has_u_o;
  return g;
}

// ============================================================================
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img), dmem_(mem) {}

  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t retires() const { return retires_; }
  uint64_t illegal_writes() const { return dut_->o_csr_illegal_wr_o; }
  const std::vector<uint64_t>& trap_pcs() const { return trap_pcs_; }
  uint64_t unexpected_traps() const { return unexpected_; }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "max-cycles (" + Dec(max_cycles_) + ") exhausted");
    }
    mtime_ = kMtimeBase + cycles_;
    dut_->rst = rst ? 1 : 0;
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
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
  void Observe() {
    Compare("the retire counter equals the retirement events published",
            dut_->o_commit_o == retires_,
            "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(retires_));
    Compare("ROB occupancy is within the ROB",
            dut_->o_rob_occupied_o <= g_.rob_entries,
            "occupied=" + Dec(dut_->o_rob_occupied_o));

    if (dut_->o_trap_valid_o != 0) {
      trap_pcs_.push_back(dut_->o_trap_epc_o);
      if (Debug() && trap_pcs_.size() <= 12) {
        std::printf("    [trap %zu] cycle=%llu epc=%s cause=%s tval=%s target=%s "
                    "pmp_deny=%u lsu_fault=%u\n",
                    trap_pcs_.size(), static_cast<unsigned long long>(cycles_),
                    U64(dut_->o_trap_epc_o).c_str(), U64(dut_->o_trap_cause_o).c_str(),
                    U64(dut_->o_trap_tval_o).c_str(), U64(dut_->o_trap_target_o).c_str(),
                    dut_->o_pmp_deny_ctr_o, dut_->o_mem_lsu_access_fault_o);
      }
      if (dut_->o_trap_is_irq_o != 0) {
        unexpected_++;
        Fail(phase_ + " at cycle " + Dec(cycles_),
             "an interrupt was taken; the case drives none");
      }
    }

    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      if (PackedLane(dut_->ev_trap_o, lane, 1) != 0) continue;   // lane-0 trap event
      retires_++;
    }
    ProgressCheck();
    if (Debug() && (cycles_ % 100000 == 0)) {
      std::printf("    [cycle %llu] commit=%u traps=%zu head_pc=%s occupied=%u\n",
                  static_cast<unsigned long long>(cycles_), dut_->o_commit_o,
                  trap_pcs_.size(), U64(dut_->o_dbg_head_pc_o).c_str(),
                  dut_->o_rob_occupied_o);
    }
  }

  void ProgressCheck() {
    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_trap_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) {
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled");
    }
  }

  static uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
    const uint64_t m = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
    return (packed >> (lane * width)) & m;
  }

 public:
  void Configure(const Geometry& g) { g_ = g; }
  void Phase(const std::string& name) { phase_ = name; }

 private:
  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  Geometry g_;
  std::string phase_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  uint64_t retires_ = 0;
  uint64_t mtime_ = 0;
  uint32_t last_commit_ = 0, last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  std::vector<uint64_t> trap_pcs_;
  uint64_t unexpected_ = 0;
};

// ============================================================================
// The generated program
// ============================================================================
struct ExamplePlan {
  int rule = 0;
  int negative = 0;
  bool emitted = false;
  bool op_trap = false;
  bool read_trap = false;
  uint64_t op_pc = 0, read_pc = 0, read2_pc = 0;
  int slot_a = 0, slot_b = 0;
  int lit_write = -1, lit_canary = -1;
};

struct Program {
  ProgImage image;
  std::vector<ExamplePlan> plans;      // one per ledger example
  std::vector<uint64_t> literals;
  uint64_t handler_pc = 0;
  uint64_t tohost = 0;
};

uint64_t SlotAddr(int slot) { return kSlotBase + 8ull * slot; }
uint64_t LitAddr(int lit) { return kLiteralBase + 8ull * lit; }

int RuleKind(int rule) { return MOSAIC_CSR_RULES[rule].kind; }
bool RuleIsMtvec(int rule) { return MOSAIC_CSR_RULES[rule].address == 0x305; }

#ifdef MOSAIC_CSRRULES_MUTANT_SKIP_RULE
// NEGATIVE CONTROL (fail mode 1): the driver silently drops the stimulus for one
// rule while the ledger still requires it. The coverage assertion must fail.
bool SkipExample(int rule) { return rule == 0; }
#else
bool SkipExample(int) { return false; }
#endif

Program BuildProgram(const Geometry& g) {
  Program program;
  program.tohost = MOSAIC_TOHOST;
  program.plans.resize(MOSAIC_CSR_EXAMPLE_COUNT);
  program.literals.assign(2 * MOSAIC_CSR_EXAMPLE_COUNT, 0);

  Asm asm_(g.reset_vector);

  // --- prologue: base registers, the trap vector, then the handler ----------
  asm_.LaAbs(31, kLiteralBase);   // x31 = literal pool
  asm_.LaAbs(30, kSlotBase);      // x30 = read-back slots
  asm_.La(5, "handler");
  asm_.Csrrw(0, 0x305, 5);                                   // mtvec <- handler (Direct)
  asm_.La(5, "main");
  asm_.Jalr(0, 5, 0);                                        // jump over the handler
  asm_.Mark("handler");
  // Skip the faulting instruction and return. Only x28 is touched, so the
  // examples' x5/x6/x7/x8 survive the trap.
  asm_.Csrr(28, 0x341);                                      // mepc
  asm_.Addi(28, 28, 4);
  asm_.Csrrw(0, 0x341, 28);                                  // mepc <- mepc + 4
  asm_.Mret();
  asm_.Mark("main");

  // --- emission --------------------------------------------------------------
  // Order: every example that must not trap first, then the ones that must. A
  // trap is a precise redirect, so doing all of the non-trapping rules first
  // keeps the trapping group's mstatus/mie/mip side effects away from the rules
  // that read those registers. The mtvec rules install a reserved mode and a
  // fresh base, so the trap vector is saved before them and restored before the
  // trapping group runs.
  std::vector<int> safe;
  std::vector<int> trapping;
  for (int i = 0; i < MOSAIC_CSR_EXAMPLE_COUNT; i++) {
    (MOSAIC_CSR_EXAMPLES[i].traps == 0 ? safe : trapping).push_back(i);
  }

  auto emit = [&](int i) {
    const mosaic_csr_example_t& e = MOSAIC_CSR_EXAMPLES[i];
    ExamplePlan& plan = program.plans[i];
    plan.rule = e.rule;
    plan.negative = e.negative;
    plan.slot_a = 2 * i;
    plan.slot_b = 2 * i + 1;
    if (SkipExample(e.rule)) return;   // control only; the ledger still requires it

    const uint64_t target = e.target;
    const uint64_t write_target = e.write_target;
    if (e.op != MOSAIC_EX_CSRR) {
      plan.lit_write = 2 * i;
      program.literals[plan.lit_write] = e.write;
      asm_.Ld(5, 31, static_cast<int32_t>(8 * plan.lit_write));
    }
    if (e.expect_mode == MOSAIC_EXP_CANARY) {
      plan.lit_canary = 2 * i + 1;
      program.literals[plan.lit_canary] = e.expect_lo;
      asm_.Ld(7, 31, static_cast<int32_t>(8 * plan.lit_canary));
    }
    if (e.op == MOSAIC_EX_CSRR) {
      plan.read_pc = asm_.pc();
      asm_.Csrr(7, static_cast<uint32_t>(target));
      plan.read_trap = (e.traps >= 1);
    } else {
      plan.op_pc = asm_.pc();
      switch (e.op) {
        case MOSAIC_EX_CSRRW: asm_.Csrrw(0, static_cast<uint32_t>(write_target), 5); break;
        case MOSAIC_EX_CSRRS: asm_.Csrrs(0, static_cast<uint32_t>(write_target), 5); break;
        default:              asm_.Csrrc(0, static_cast<uint32_t>(write_target), 5); break;
      }
      plan.op_trap = (e.traps >= 1);
      plan.read_pc = asm_.pc();
      asm_.Csrr(7, static_cast<uint32_t>(target));
      plan.read_trap = (e.traps >= 2);
    }
    asm_.Sd(7, 30, static_cast<int32_t>(8 * plan.slot_a));
    if (e.expect_mode == MOSAIC_EXP_ADVANCE) {
      for (int k = 0; k < e.advance_gap; k++) asm_.Addi(9, 9, 1);
      plan.read2_pc = asm_.pc();
      asm_.Csrr(8, static_cast<uint32_t>(target));
      asm_.Sd(8, 30, static_cast<int32_t>(8 * plan.slot_b));
    }
    plan.emitted = true;
  };

  bool mtvec_saved = false;
  for (int i : safe) {
    if (RuleIsMtvec(MOSAIC_CSR_EXAMPLES[i].rule) && !mtvec_saved) {
      asm_.Csrr(6, 0x305);
      mtvec_saved = true;
    }
    emit(i);
  }
  if (mtvec_saved) asm_.Csrrw(0, 0x305, 6);

  bool traps_cleared = false;
  for (int i : trapping) {
    if (!traps_cleared) {
      asm_.Addi(5, 0, 0);
      asm_.Csrrw(0, 0x300, 5);   // mstatus <- 0 (MIE clear)
      asm_.Csrrw(0, 0x304, 5);   // mie <- 0
      asm_.Csrrw(0, 0x344, 5);   // mip <- 0
      traps_cleared = true;
    }
    emit(i);
  }

  // --- exit -----------------------------------------------------------------
  asm_.Addi(5, 0, 1);
  asm_.LaAbs(6, program.tohost);
  asm_.Sd(5, 6, 0);
  asm_.JalSelf();

  asm_.Resolve();
  program.handler_pc = 0;
  if (Debug()) {
    for (size_t i = 0; i < asm_.words().size() && i < 60; i++) {
      std::printf("  [word %3zu] %s %08x\n", i,
                  U64(g.reset_vector + 4ull * i).c_str(), asm_.words()[i]);
    }
  }
  for (size_t i = 0; i < asm_.words().size(); i++) {
    program.image.Put(g.reset_vector + 4ull * i, asm_.words()[i]);
  }
  const uint64_t end = g.reset_vector + 4ull * asm_.words().size();
  for (uint64_t addr = end; addr < end + 0x800ull; addr += 4) {
    program.image.Put(addr, 0x0000006fu);
  }
  return program;
}

// ============================================================================
// The run and the comparison
// ============================================================================
void RunCase(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, const Geometry& g,
             uint64_t* totals) {
  const std::string phase = "csr-rule-ledger";

  if (MOSAIC_CSR_RULE_COUNT == 0) {
    Fail(phase, "the rule ledger is empty; config/csr/rule_ledger.json does not "
                "cover profile p0");
  }

  Program program = BuildProgram(g);

  mosaic::MemoryModel dut_mem;
  // The literal pool is data the program loads; the slots are the read-back
  // memory the driver reads after the run. Both live in RAM.
  for (size_t i = 0; i < program.literals.size(); i++) {
    if (dut_mem.Write(LitAddr(static_cast<int>(i)), 8, program.literals[i]) !=
        mosaic::AccessStatus::kOk) {
      Fail(phase, "the literal pool at " + U64(LitAddr(static_cast<int>(i))) +
                      " is not writable RAM");
    }
  }

  Harness harness(dut, reporter, kProgramCycles, &program.image, &dut_mem);
  harness.Configure(g);
  harness.Phase(phase);
  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);
  while (!dut_mem.finished() && harness.cycles() < kProgramCycles) {
    harness.Cycle(false);
  }
  for (int i = 0; i < 8; i++) harness.Cycle(false);
  if (!dut_mem.finished()) {
    Fail(phase, "the program never reached the exit protocol");
  }

  totals[0] = harness.cycles();
  totals[1] = harness.comparisons();
  totals[2] = harness.retires();
  totals[3] = harness.trap_pcs().size();

  // ---- the observed trap set ------------------------------------------------
  std::set<uint64_t> observed(harness.trap_pcs().begin(), harness.trap_pcs().end());
  std::set<uint64_t> expected_traps;
  uint64_t expected_illegal_writes = 0;
  for (const ExamplePlan& plan : program.plans) {
    if (!plan.emitted) continue;
    if (plan.op_trap) expected_traps.insert(plan.op_pc);
    if (plan.read_trap) expected_traps.insert(plan.read_pc);
    if (plan.op_trap) expected_illegal_writes++;
  }
  for (uint64_t pc : observed) {
    if (expected_traps.count(pc) == 0) {
      Fail(phase, "an illegal-instruction trap was taken at " + U64(pc) +
                      ", which no ledger example expects");
    }
  }
  harness.Check("every expected illegal access raised the illegal-instruction exception",
                observed.size() == expected_traps.size(),
                "traps=" + Dec(observed.size()) + " expected=" +
                    Dec(expected_traps.size()));
  harness.Check("the CSR file's illegal-write counter counts every denied write",
                harness.illegal_writes() == expected_illegal_writes,
                "counter=" + Dec(harness.illegal_writes()) + " expected=" +
                    Dec(expected_illegal_writes));

  // ---- the per-example comparison ------------------------------------------
  std::vector<int> positive_seen(MOSAIC_CSR_RULE_COUNT, 0);
  std::vector<int> negative_seen(MOSAIC_CSR_RULE_COUNT, 0);
  uint64_t examples_checked = 0;
  for (int i = 0; i < MOSAIC_CSR_EXAMPLE_COUNT; i++) {
    ExamplePlan& plan = program.plans[i];
    if (!plan.emitted) continue;
    const mosaic_csr_example_t& e = MOSAIC_CSR_EXAMPLES[i];
    const std::string tag = std::string(MOSAIC_CSR_RULE_IDS[e.rule]) + " " +
                            (e.negative ? "negative" : "positive");

    bool op_trap = plan.op_trap;
    bool read_trap = plan.read_trap;
    uint8_t expect_mode = e.expect_mode;
    uint64_t expect_lo = e.expect_lo;
    uint64_t expect_hi = e.expect_hi;
    bool has_forbid = e.has_forbid != 0;

#ifdef MOSAIC_CSRRULES_MUTANT_AUTO_WAIVER
    // NEGATIVE CONTROL (fail mode 2): the comparator accepts the adjacent
    // illegal value the ledger marks with `forbid`, i.e. it waives the
    // difference instead of rejecting it. The machine's correct canonicalisation
    // then fails this check.
    if (RuleKind(e.rule) == MOSAIC_RULE_WARL_ALLOWED && has_forbid) {
      expect_mode = MOSAIC_EXP_READ;
      expect_lo = e.forbid;
    }
#endif
#ifdef MOSAIC_CSRRULES_MUTANT_UNIMPL_ZERO
    // NEGATIVE CONTROL (fail mode 3): the comparator expects an unimplemented CSR
    // to be accepted as zero without trapping. The machine's correct trap fails
    // the trap check first.
    if (e.rule < MOSAIC_CSR_RULE_COUNT &&
        std::string(MOSAIC_CSR_RULE_IDS[e.rule]).rfind("unimplemented.", 0) == 0) {
      op_trap = false;
      read_trap = false;
    }
#endif

    const bool want_op_trap = op_trap;
    const bool want_read_trap = read_trap;
    const bool got_op_trap = observed.count(plan.op_pc) != 0;
    const bool got_read_trap = observed.count(plan.read_pc) != 0;
    if (e.op != MOSAIC_EX_CSRR) {
      harness.Compare(tag + ": the write's illegal-instruction trap",
                      got_op_trap == want_op_trap,
                      std::string("expected ") + (want_op_trap ? "1" : "0") +
                          " trap, observed " + (got_op_trap ? "1" : "0"));
    }
    harness.Compare(tag + ": the read's illegal-instruction trap",
                    got_read_trap == want_read_trap,
                    std::string("expected ") + (want_read_trap ? "1" : "0") +
                        " trap, observed " + (got_read_trap ? "1" : "0"));

    uint64_t value = 0;
    if (dut_mem.Read(SlotAddr(plan.slot_a), 8, &value) != mosaic::AccessStatus::kOk) {
      Fail(phase, tag + ": the read-back slot is not readable");
    }
    bool ok = true;
    std::string want;
    if (expect_mode == MOSAIC_EXP_READ || expect_mode == MOSAIC_EXP_CANARY) {
      ok = (value == expect_lo);
      want = U64(expect_lo);
    } else if (expect_mode == MOSAIC_EXP_RANGE) {
      ok = (value >= expect_lo && value <= expect_hi);
      want = U64(expect_lo) + ".." + U64(expect_hi);
    } else {   // ADVANCE
      uint64_t second = 0;
      if (dut_mem.Read(SlotAddr(plan.slot_b), 8, &second) != mosaic::AccessStatus::kOk) {
        Fail(phase, tag + ": the second read-back slot is not readable");
      }
      ok = (second - value) >= expect_lo;
      want = "advance >= " + Dec(expect_lo);
      if (has_forbid) ok = ok && (second != e.forbid) && (value != e.forbid);
    }
    harness.Compare(tag + ": the read-back", ok,
                    "read " + U64(value) + ", want " + want);
    if (has_forbid && expect_mode != MOSAIC_EXP_ADVANCE) {
      harness.Compare(tag + ": the adjacent illegal result is rejected",
                      value != e.forbid,
                      "read " + U64(value) + " equals the illegal value " +
                          U64(e.forbid));
    }

    if (e.negative) {
      negative_seen[e.rule]++;
    } else {
      positive_seen[e.rule]++;
    }
    examples_checked++;
  }

  totals[1] = harness.comparisons();

  // ---- coverage: a rule with no stimulus fails, it is not skipped -----------
  uint64_t rules_covered = 0;
  for (int r = 0; r < MOSAIC_CSR_RULE_COUNT; r++) {
    if (positive_seen[r] == 0) {
      Fail(phase, std::string("coverage: rule ") + MOSAIC_CSR_RULE_IDS[r] +
                      " was never visited by its positive example");
    }
    if (negative_seen[r] == 0) {
      Fail(phase, std::string("coverage: rule ") + MOSAIC_CSR_RULE_IDS[r] +
                      " was never visited by its negative example");
    }
    rules_covered++;
  }
  harness.Check("every ledger rule was visited",
                rules_covered == static_cast<uint64_t>(MOSAIC_CSR_RULE_COUNT),
                "covered=" + Dec(rules_covered) + " of " +
                    Dec(MOSAIC_CSR_RULE_COUNT));
  harness.Check("every ledger example was driven",
                examples_checked == static_cast<uint64_t>(MOSAIC_CSR_EXAMPLE_COUNT),
                "checked=" + Dec(examples_checked) + " of " +
                    Dec(MOSAIC_CSR_EXAMPLE_COUNT));

  totals[4] = rules_covered;
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
    if (geometry.reset_vector != MOSAIC_RESET_VECTOR) {
      Fail("geometry", "the reset vector is not the profile's: " + U64(geometry.reset_vector));
    }
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");

    uint64_t totals[5] = {0, 0, 0, 0, 0};
    RunCase(&dut, &reporter, geometry, totals);
    detail = "rules=" + Dec(totals[4]) + " examples=" + Dec(MOSAIC_CSR_EXAMPLE_COUNT) +
             " checks=" + Dec(reporter.checks()) + " comparisons=" + Dec(totals[1]) +
             " cycles=" + Dec(totals[0]) + " retires=" + Dec(totals[2]) +
             " traps=" + Dec(totals[3]) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "every ledger rule holds on this machine",
                      "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
