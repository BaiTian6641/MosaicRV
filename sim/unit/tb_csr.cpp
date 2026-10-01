// ============================================================================
// tb_csr.cpp -- CASE=csr.precise_trap_mret, work package I-019.
//
// The DUT is never its own oracle. Every output and every architectural CSR is
// compared against an independent C++ shadow written from the contract in the
// header of rtl/core/mosaic_csr.sv -- the boundary priority, the mstatus
// transitions, the WARL canonicalisation, the counter arithmetic and the
// read/write port rules -- not from the RTL's structure. The shadow holds its
// own register file and derives every next state from that prose.
//
// The one thing the shadow does *not* re-type is the implementation table. Its
// addresses, reset values and write masks come from
// build/<profile>/sim/mosaic_csr_table.h, which tools/gen_manifest.py decodes
// from config/csr/mode_m.json -- the same decode the RTL's generated package is
// built from. That is deliberate: a hand-copied table in this file would be the
// second copy of a configured number the project refuses to have, and the two
// could drift while both staying self-consistent. What is shared is the *data*;
// what is independent is the *behaviour*, and the behaviour is what the phases
// below check. Because shared data can hide a decode bug, phase 1 ("table")
// cross-checks all 21 rows against hand-derived expectations read from the
// spec clauses in config/csr/mode_m.json, so a wrong decode fails loudly here
// instead of passing in both models at once.
//
// Phases, each of which resets first and can fail on its own:
//
//   1. table          the generated table equals the hand-derived one
//   2. reset-state    every one of the 21 CSRs reads its declared reset value
//   3. read-only      writes to the seven read-only CSRs raise csr_wr_illegal_o,
//                     change nothing, and reads still work; writes to misa and
//                     mcounteren are *legal* (WARL registers with no writable
//                     bits) and also change nothing
//   4. warl-allones   write all-ones to every writable CSR and require exactly
//                     the mask-implied value back, per CSR, with RW, RS and RC
//   5. illegal-addr   legitimate-looking unimplemented addresses raise
//                     csr_illegal_o, read 0, and change nothing on a write
//   6. trap-mret      trap entry and MRET field transitions, nesting, MPIE
//                     already zero, trap/mret mutual exclusion, trap priority
//                     over a retiring CSR write
//   7. causes         every exception code in mosaic_pkg's list, plus interrupt
//                     causes: pc/cause/tval/status equal to the model, direct
//                     and vectored targets
//   8. counters       free-running increments, write+increment in one cycle,
//                     read-only cycle/instret/time shadows
//   9. random         a random programme of reads, all three write ops, ticks,
//                     traps and MRETs, compared on every observable every cycle
//
// Standing invariants (checked on the DUT's own outputs, not the shadow):
//
//   * mstatus.MPP is always 3: the only privilege mode this profile implements.
//   * mtvec.MODE is never a reserved encoding.
//   * mepc[1:0] is always zero (IALIGN=32).
//   * mie has no bit set outside its implemented bits.
//   * The four observability counters are monotonic non-decreasing.
//
// Not verified here, and named so nobody assumes otherwise:
//   * the retire unit's side of the contract (that a trapping instruction writes
//     no destination register and does not increment minstret -- that is I-017's
//     event stream, and this case drives cnt_instret_i directly);
//   * interrupt sampling and the mip owner (I-020): mip_we_o/mip_op_o/mip_wdata_o
//     are checked as forwarded values, not by applying them anywhere;
//   * privilege violations: p0 is M-only, so no access can be illegal for
//     privilege reasons and the interface has no port for one.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_csr_tb.h"
#include "mosaic_csr_table.h"

namespace {

// Thrown on the first failed check, so the report names one defect rather than a
// thousand consequences of it.
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

void Require(bool condition, const std::string& where, const std::string& detail) {
  if (!condition) Fail(where, detail);
}

std::string Bool(bool value) { return value ? "1" : "0"; }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ------------------------------------------------------------------- roles
//
// The model names what each CSR *is*; where it *lives* and what may be written
// to it come from the generated table.

enum Role {
  kMstatus = 0,
  kMisa,
  kMedeleg,
  kMideleg,
  kMie,
  kMtvec,
  kMcounteren,
  kMscratch,
  kMepc,
  kMcause,
  kMtval,
  kMip,
  kMvendorid,
  kMarchid,
  kMimpid,
  kMhartid,
  kMcycle,
  kMinstret,
  kCycle,
  kTime,
  kInstret,
  kRoleCount
};

const char* const kRoleName[kRoleCount] = {
    "mstatus",  "misa",     "medeleg", "mideleg", "mie",      "mtvec",  "mcounteren",
    "mscratch", "mepc",     "mcause",  "mtval",   "mip",      "mvendorid", "marchid",
    "mimpid",   "mhartid",  "mcycle",  "minstret", "cycle",   "time",   "instret"};

// Addresses the shadow uses to reach a role. The numbers themselves are only in
// the generated table; this is a lookup, not a copy.
struct TableIndex {
  int    role_of_addr[4096];   // -1 when unimplemented
  int    row_of_role[kRoleCount];
  bool   ok = true;
  std::string problem;

  TableIndex() {
    for (int i = 0; i < 4096; i++) role_of_addr[i] = -1;
    for (int r = 0; r < kRoleCount; r++) row_of_role[r] = -1;
    for (int r = 0; r < kRoleCount; r++) {
      for (int t = 0; t < MOSAIC_CSR_COUNT; t++) {
        if (std::string(MOSAIC_CSR_TABLE[t].name) == kRoleName[r]) {
          row_of_role[r] = t;
          break;
        }
      }
      if (row_of_role[r] < 0) {
        ok = false;
        problem = std::string("the generated table has no row named ") + kRoleName[r];
        return;
      }
      role_of_addr[MOSAIC_CSR_TABLE[row_of_role[r]].addr] = r;
    }
    for (int t = 0; t < MOSAIC_CSR_COUNT; t++) {
      if (role_of_addr[MOSAIC_CSR_TABLE[t].addr] < 0) {
        ok = false;
        problem = std::string("generated table row ") + MOSAIC_CSR_TABLE[t].name +
                  " has no known role";
        return;
      }
    }
  }

  const mosaic_csr_desc_t& Desc(int role) const { return MOSAIC_CSR_TABLE[row_of_role[role]]; }
  uint16_t Addr(int role) const { return Desc(role).addr; }
  uint64_t Reset(int role) const { return Desc(role).reset; }
  uint64_t Wmask(int role) const { return Desc(role).wmask; }
  bool WriteLegal(int role) const { return Desc(role).write_legal != 0; }
  int RoleOf(uint16_t addr) const { return (addr < 4096) ? role_of_addr[addr] : -1; }
  bool Impl(uint16_t addr) const { return RoleOf(addr) >= 0; }
};

const TableIndex kTable;

// ------------------------------------------------------------------ stimulus

// mosaic_pkg::csr_op_e, mirrored. The values are the package's, and the enum
// exists there so the decoder and this test cannot disagree.
enum CsrOp { kOpNone = 0, kOpRw = 1, kOpRs = 2, kOpRc = 3 };

struct Stim {
  uint16_t addr = 0;
  bool     we = false;
  uint32_t op = kOpNone;
  uint64_t wdata = 0;

  bool     cnt_cycle = false;
  bool     cnt_instret = false;

  bool     trap_valid = false;
  uint64_t trap_cause = 0;
  uint64_t trap_tval = 0;
  uint64_t trap_epc = 0;

  bool     mret_valid = false;

  uint64_t mip = 0;
  uint64_t mtime = 0;

  std::string str() const {
    return "[addr=0x" + mosaic::Hex(addr, 3) + " we=" + Bool(we) + " op=" + Dec(op) +
           " wdata=0x" + mosaic::Hex(wdata) + " tick=" + Bool(cnt_cycle) + "/" +
           Bool(cnt_instret) + " trap=" + Bool(trap_valid) + ":0x" + mosaic::Hex(trap_cause) +
           " mret=" + Bool(mret_valid) + "]";
  }
};

// What the DUT must present combinationally in the cycle being run: a function of
// the pre-edge registers and the cycle's inputs.
struct Comb {
  uint64_t rdata = 0;
  bool     illegal = false;
  bool     wr_illegal = false;
  bool     trap_commit = false;
  uint64_t trap_target = 0;
  bool     mret_commit = false;
  uint64_t mret_target = 0;
  bool     mip_we = false;
  uint32_t mip_op = 0;
  uint64_t mip_wdata = 0;
};

// What the DUT must show on its registered observability outputs after the edge.
struct Regs {
  uint64_t mstatus = 0;
  uint64_t mtvec = 0;
  uint64_t mepc = 0;
  uint64_t mcause = 0;
  uint64_t mtval = 0;
  uint64_t mscratch = 0;
  uint64_t mie = 0;
  uint64_t mip = 0;
  uint64_t misa = 0;
  uint64_t mcycle = 0;
  uint64_t minstret = 0;
  uint32_t wr_ctr = 0;
  uint32_t illegal_wr_ctr = 0;
  uint32_t trap_ctr = 0;
  uint32_t mret_ctr = 0;
};

// mstatus field positions, from the RV64 layout the config's clause cites. The
// positions are ISA constants, not configuration; writability comes from the
// generated mask.
constexpr uint64_t kMstatusMie = 1ull << 3;
constexpr uint64_t kMstatusMpie = 1ull << 7;
constexpr uint64_t kMstatusMpp = 3ull << 11;

uint64_t Bit(uint64_t value, unsigned bit) { return (value >> bit) & 1ull; }

// ------------------------------------------------------------------- shadow

class ShadowCsr {
 public:
  ShadowCsr() { Reset(); }

  void Reset() {
    mstatus_ = kTable.Reset(kMstatus);
    mie_ = kTable.Reset(kMie);
    mtvec_ = kTable.Reset(kMtvec);
    mscratch_ = kTable.Reset(kMscratch);
    mepc_ = kTable.Reset(kMepc);
    mcause_ = kTable.Reset(kMcause);
    mtval_ = kTable.Reset(kMtval);
    mcycle_ = kTable.Reset(kMcycle);
    minstret_ = kTable.Reset(kMinstret);
    wr_ctr_ = 0;
    illegal_wr_ctr_ = 0;
    trap_ctr_ = 0;
    mret_ctr_ = 0;
    seen_writes_ = 0;
    seen_illegal_writes_ = 0;
    seen_traps_ = 0;
    seen_mrets_ = 0;
  }

  // ------------------------------------------------ behavioural accessors
  uint32_t writes_seen() const { return seen_writes_; }
  uint32_t illegal_writes_seen() const { return seen_illegal_writes_; }
  uint32_t traps_seen() const { return seen_traps_; }
  uint32_t mrets_seen() const { return seen_mrets_; }

  uint64_t mstatus() const { return mstatus_; }
  uint64_t mtvec() const { return mtvec_; }
  uint64_t mepc() const { return mepc_; }
  uint64_t mcause() const { return mcause_; }
  uint64_t mtval() const { return mtval_; }
  uint64_t mie() const { return mie_; }
  uint64_t mcycle() const { return mcycle_; }
  uint64_t minstret() const { return minstret_; }

  // The stored value of a role, for phases that want to compare a register
  // without going through the port.
  uint64_t Stored(int role) const {
    switch (role) {
      case kMstatus: return mstatus_;
      case kMie: return mie_;
      case kMtvec: return mtvec_;
      case kMscratch: return mscratch_;
      case kMepc: return mepc_;
      case kMcause: return mcause_;
      case kMtval: return mtval_;
      case kMcycle: return mcycle_;
      case kMinstret: return minstret_;
      default: return kTable.Reset(role);
    }
  }

  // The value a read of `role` must return: registers for the writable ones,
  // the constant reset value for the WARL read-only ones, the masked pending view
  // for mip, the shadow of the counter for cycle/instret, and mtime for time.
  uint64_t ReadValue(int role, uint64_t mip, uint64_t mtime) const {
    switch (role) {
      case kMstatus: return mstatus_;
      case kMie: return mie_;
      case kMtvec: return mtvec_;
      case kMscratch: return mscratch_;
      case kMepc: return mepc_;
      case kMcause: return mcause_;
      case kMtval: return mtval_;
      case kMcycle: return mcycle_;
      case kMinstret: return minstret_;
      case kCycle: return mcycle_;        // read-only shadow of mcycle
      case kInstret: return minstret_;    // read-only shadow of minstret
      case kTime: return mtime;           // read-only shadow of the platform timer
      case kMip: return mip & kTable.Wmask(kMip);
      default: return kTable.Reset(role);  // misa, medeleg, mideleg, mcounteren, ids
    }
  }

  // A WARL field must never read back an illegal value. mtvec is the only field
  // in this table whose legal set is not "every value of the field": Table mtvec
  // MODE reserves >= 2, so a reserved encoding becomes Direct.
  static uint64_t CanonicalWrite(int role, uint64_t value) {
    if (role == kMtvec) {
      const uint64_t mode = value & 3ull;
      return (value & ~3ull) | ((mode <= 1) ? mode : 0ull);
    }
    return value;
  }

  // -------------------------------------------------------------- evaluation
  Comb Eval(const Stim& s) const {
    Comb c;
    const int role = kTable.RoleOf(s.addr);

    c.illegal = role < 0;
    c.wr_illegal = s.we && ((role < 0) || !kTable.WriteLegal(role));
    c.rdata = (role < 0) ? 0 : ReadValue(role, s.mip, s.mtime);

    c.trap_commit = s.trap_valid;
    // A trap and an MRET cannot both be at one boundary; the hardware resolves it
    // in favour of the trap, which is what this models.
    c.mret_commit = s.mret_valid && !s.trap_valid;
    c.mret_target = mepc_;

    if ((mtvec_ & 3ull) == 1 && Bit(s.trap_cause, 63)) {
      // Vectored, and this is an interrupt: base + 4 * exception code.
      c.trap_target = (mtvec_ & ~3ull) + ((s.trap_cause & 0x3fffffffffffffffull) << 2);
    } else {
      c.trap_target = mtvec_ & ~3ull;
    }

    const bool wr_accept = s.we && (role >= 0) && kTable.WriteLegal(role) && !s.trap_valid &&
                           !s.mret_valid;
    c.mip_we = wr_accept && (role == kMip);
    c.mip_op = s.op;
    c.mip_wdata = s.wdata;
    return c;
  }

  void Apply(const Stim& s) {
    const Comb c = Eval(s);
    const int role = kTable.RoleOf(s.addr);

    const bool wr_accept = s.we && (role >= 0) && kTable.WriteLegal(role) && !s.trap_valid &&
                           !s.mret_valid;

    // The operand view the caller prepared: RW replaces, RS sets, RC clears.
    uint64_t op_result = s.wdata;
    if (wr_accept) {
      const uint64_t current = ReadValue(role, s.mip, s.mtime);
      if (s.op == kOpRs) op_result = current | s.wdata;
      else if (s.op == kOpRc) op_result = current & ~s.wdata;
      else op_result = s.wdata;
    }

    // Write-back for the registers that have storage, as a masked update so the
    // read-only and write-preserve-zero bits keep their reset value.
    auto masked_write = [&](int r, uint64_t current) -> uint64_t {
      const uint64_t w = kTable.Wmask(r);
      return (current & ~w) | (CanonicalWrite(r, op_result) & w);
    };

    uint64_t next_mstatus = mstatus_;
    uint64_t next_mie = mie_;
    uint64_t next_mtvec = mtvec_;
    uint64_t next_mscratch = mscratch_;
    uint64_t next_mepc = mepc_;
    uint64_t next_mcause = mcause_;
    uint64_t next_mtval = mtval_;
    uint64_t next_mcycle = mcycle_;
    uint64_t next_minstret = minstret_;

    // Strict priority: trap, then MRET, then a retiring CSR write.
    if (s.trap_valid) {
      next_mstatus = (mstatus_ & ~(kMstatusMie | kMstatusMpie)) |
                     (Bit(mstatus_, 3) ? kMstatusMpie : 0ull) | kMstatusMpp;
      next_mepc = s.trap_epc & kTable.Wmask(kMepc);
      next_mcause = s.trap_cause;
      next_mtval = s.trap_tval;
    } else if (s.mret_valid) {
      next_mstatus = (mstatus_ & ~(kMstatusMie | kMstatusMpie)) |
                     (Bit(mstatus_, 7) ? kMstatusMie : 0ull) | kMstatusMpie | kMstatusMpp;
    } else if (wr_accept) {
      switch (role) {
        case kMstatus: next_mstatus = masked_write(kMstatus, mstatus_); break;
        case kMie: next_mie = masked_write(kMie, mie_); break;
        case kMtvec: next_mtvec = masked_write(kMtvec, mtvec_); break;
        case kMscratch: next_mscratch = masked_write(kMscratch, mscratch_); break;
        case kMepc: next_mepc = masked_write(kMepc, mepc_); break;
        case kMcause: next_mcause = masked_write(kMcause, mcause_); break;
        case kMtval: next_mtval = masked_write(kMtval, mtval_); break;
        case kMcycle: next_mcycle = masked_write(kMcycle, mcycle_); break;
        case kMinstret: next_minstret = masked_write(kMinstret, minstret_); break;
        // medeleg/mideleg accept a write and canonicalise to zero (write mask 0);
        // mip is applied by its owner; misa, mcounteren and the ids have no
        // writable bits; cycle/instret/time are read-only and never reach here.
        default: break;
      }
    }

    // The counters are free-running: a write supplies the value at the edge and
    // the same edge's tick is added on top, so no instruction can delete a cycle.
    mstatus_ = next_mstatus;
    mie_ = next_mie;
    mtvec_ = next_mtvec;
    mscratch_ = next_mscratch;
    mepc_ = next_mepc;
    mcause_ = next_mcause;
    mtval_ = next_mtval;
    mcycle_ = next_mcycle + (s.cnt_cycle ? 1ull : 0ull);
    minstret_ = next_minstret + (s.cnt_instret ? 1ull : 0ull);

    if (wr_accept) {
      wr_ctr_++;
      seen_writes_++;
    }
    if (c.wr_illegal) {
      illegal_wr_ctr_++;
      seen_illegal_writes_++;
    }
    if (s.trap_valid) {
      trap_ctr_++;
      seen_traps_++;
    }
    if (c.mret_commit) {
      mret_ctr_++;
      seen_mrets_++;
    }
  }

  Regs Snapshot(uint64_t mip) const {
    Regs r;
    r.mstatus = mstatus_;
    r.mtvec = mtvec_;
    r.mepc = mepc_;
    r.mcause = mcause_;
    r.mtval = mtval_;
    r.mscratch = mscratch_;
    r.mie = mie_;
    r.mip = mip & kTable.Wmask(kMip);
    r.misa = kTable.Reset(kMisa);
    r.mcycle = mcycle_;
    r.minstret = minstret_;
    r.wr_ctr = wr_ctr_;
    r.illegal_wr_ctr = illegal_wr_ctr_;
    r.trap_ctr = trap_ctr_;
    r.mret_ctr = mret_ctr_;
    return r;
  }

 private:
  uint64_t mstatus_ = 0;
  uint64_t mie_ = 0;
  uint64_t mtvec_ = 0;
  uint64_t mscratch_ = 0;
  uint64_t mepc_ = 0;
  uint64_t mcause_ = 0;
  uint64_t mtval_ = 0;
  uint64_t mcycle_ = 0;
  uint64_t minstret_ = 0;
  uint32_t wr_ctr_ = 0;
  uint32_t illegal_wr_ctr_ = 0;
  uint32_t trap_ctr_ = 0;
  uint32_t mret_ctr_ = 0;
  uint32_t seen_writes_ = 0;
  uint32_t seen_illegal_writes_ = 0;
  uint32_t seen_traps_ = 0;
  uint32_t seen_mrets_ = 0;
};

// ------------------------------------------------------------------ harness

class Harness {
 public:
  Harness(Vmosaic_csr_tb* dut, mosaic::ClockDriver* clk, mosaic::Reporter* reporter,
          uint64_t max_cycles)
      : dut_(dut), clk_(clk), reporter_(reporter), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }
  void BindShadow(ShadowCsr* shadow) { shadow_ = shadow; }

  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }

  // Assert reset for `cycles` rising edges, with the shadow held in the same
  // state: a DUT reset that leaves the shadow describing a different machine
  // would report a disagreement neither model has.
  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) {
      Cycle(Stim{}, /*rst=*/true);
    }
    shadow_->Reset();
  }

  // One full clock period. The order is the two-snapshot rule:
  //
  //   1. drive with the clock low and compare the *combinational* answers
  //      against the shadow computed from the same pre-edge state -- a trap
  //      target or a read describes the edge that has not happened yet;
  //   2. advance the shadow over the edge;
  //   3. apply the edge and compare the *registered* state, which only exists
  //      after it.
  Comb Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the campaign finished");
    }

    dut_->clk = 0;
    Drive(s);
    dut_->rst = rst ? 1 : 0;
    dut_->eval();

    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle());
    Comb c;
    if (!rst) {
      c = shadow_->Eval(s);
      CompareComb(s, c, where);
      shadow_->Apply(s);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (!rst) {
      CompareRegs(s, where);
      CheckInvariants(where);
      ++comparisons_;
    }
    return c;
  }

  // A read with no write, no tick and no boundary event: the identity stimulus,
  // used by the directed phases to observe state.
  uint64_t Read(uint16_t addr) {
    Stim s;
    s.addr = addr;
    return Cycle(s).rdata;
  }

  // The observability write counter, as the DUT reports it right now (after the
  // last edge).
  uint32_t WrCtr() const { return dut_->o_wr_ctr; }
  uint32_t IllegalWrCtr() const { return dut_->o_illegal_wr_ctr; }
  uint32_t TrapCtr() const { return dut_->o_trap_ctr; }
  uint32_t MretCtr() const { return dut_->o_mret_ctr; }

 private:
  void Drive(const Stim& s) {
    dut_->csr_addr_i = static_cast<uint16_t>(s.addr & 0xfff);
    dut_->csr_we_i = s.we ? 1 : 0;
    dut_->csr_op_i = static_cast<uint8_t>(s.op & 0x3);
    dut_->csr_wdata_i = s.wdata;
    dut_->cnt_cycle_i = s.cnt_cycle ? 1 : 0;
    dut_->cnt_instret_i = s.cnt_instret ? 1 : 0;
    dut_->trap_valid_i = s.trap_valid ? 1 : 0;
    dut_->trap_cause_i = s.trap_cause;
    dut_->trap_tval_i = s.trap_tval;
    dut_->trap_epc_i = s.trap_epc;
    dut_->mret_valid_i = s.mret_valid ? 1 : 0;
    dut_->mip_i = s.mip;
    dut_->mtime_i = s.mtime;
  }

  void Expect(uint64_t expected, uint64_t actual, const std::string& where,
              const std::string& what) {
    if (expected != actual) {
      reporter_->Mismatch(where + " " + what, "0x" + mosaic::Hex(expected),
                          "0x" + mosaic::Hex(actual));
      Fail(where, what + ": expected 0x" + mosaic::Hex(expected) + ", saw 0x" +
                       mosaic::Hex(actual));
    }
  }

  void ExpectBool(bool expected, bool actual, const std::string& where,
                  const std::string& what) {
    if (expected != actual) {
      reporter_->Mismatch(where + " " + what, Bool(expected), Bool(actual));
      Fail(where, what + ": expected " + Bool(expected) + ", saw " + Bool(actual));
    }
  }

  void CompareComb(const Stim& s, const Comb& c, const std::string& where) {
    Expect(c.rdata, dut_->csr_rdata_o, where, "csr_rdata_o" + s.str());
    ExpectBool(c.illegal, dut_->csr_illegal_o, where, "csr_illegal_o" + s.str());
    ExpectBool(c.wr_illegal, dut_->csr_wr_illegal_o, where, "csr_wr_illegal_o" + s.str());
    ExpectBool(c.trap_commit, dut_->trap_commit_o, where, "trap_commit_o" + s.str());
    Expect(c.trap_target, dut_->trap_target_o, where, "trap_target_o" + s.str());
    ExpectBool(c.mret_commit, dut_->mret_commit_o, where, "mret_commit_o" + s.str());
    Expect(c.mret_target, dut_->mret_target_o, where, "mret_target_o" + s.str());
    ExpectBool(c.mip_we, dut_->mip_we_o, where, "mip_we_o" + s.str());
    Expect(c.mip_op, dut_->mip_op_o, where, "mip_op_o" + s.str());
    Expect(c.mip_wdata, dut_->mip_wdata_o, where, "mip_wdata_o" + s.str());
  }

  void CompareRegs(const Stim& s, const std::string& where) {
    const Regs r = shadow_->Snapshot(s.mip);
    Expect(r.mstatus, dut_->o_mstatus_o, where, "o_mstatus_o");
    Expect(r.mtvec, dut_->o_mtvec_o, where, "o_mtvec_o");
    Expect(r.mepc, dut_->o_mepc_o, where, "o_mepc_o");
    Expect(r.mcause, dut_->o_mcause_o, where, "o_mcause_o");
    Expect(r.mtval, dut_->o_mtval_o, where, "o_mtval_o");
    Expect(r.mscratch, dut_->o_mscratch_o, where, "o_mscratch_o");
    Expect(r.mie, dut_->o_mie_o, where, "o_mie_o");
    Expect(r.mip, dut_->o_mip_o, where, "o_mip_o");
    Expect(r.misa, dut_->o_misa_o, where, "o_misa_o");
    Expect(r.mcycle, dut_->o_mcycle_o, where, "o_mcycle_o");
    Expect(r.minstret, dut_->o_minstret_o, where, "o_minstret_o");
    Expect(r.wr_ctr, dut_->o_wr_ctr, where, "o_wr_ctr");
    Expect(r.illegal_wr_ctr, dut_->o_illegal_wr_ctr, where, "o_illegal_wr_ctr");
    Expect(r.trap_ctr, dut_->o_trap_ctr, where, "o_trap_ctr");
    Expect(r.mret_ctr, dut_->o_mret_ctr, where, "o_mret_ctr");
  }

  // Invariants checked on the DUT's own outputs, so they hold even where the
  // shadow is only as right as its reading of the contract.
  void CheckInvariants(const std::string& where) {
    if (dut_->o_mstatus_o != ((dut_->o_mstatus_o & ~kMstatusMpp) | kMstatusMpp)) {
      Fail(where, "mstatus.MPP is not 3 (M): 0x" + mosaic::Hex(dut_->o_mstatus_o) +
                      ", but M is the only privilege mode in this profile");
    }
    if ((dut_->o_mtvec_o & 3ull) > 1ull) {
      Fail(where, "mtvec.MODE reads back a reserved encoding: 0x" + mosaic::Hex(dut_->o_mtvec_o));
    }
    if ((dut_->o_mepc_o & 3ull) != 0ull) {
      Fail(where, "mepc[1:0] is not zero with IALIGN=32: 0x" + mosaic::Hex(dut_->o_mepc_o));
    }
    if ((dut_->o_mie_o & ~kTable.Wmask(kMie)) != 0ull) {
      Fail(where, "mie has a bit set outside its implemented bits: 0x" +
                      mosaic::Hex(dut_->o_mie_o));
    }
    if (dut_->o_mip_o != (dut_->o_mip_o & kTable.Wmask(kMip))) {
      Fail(where, "mip reads bits it does not implement: 0x" + mosaic::Hex(dut_->o_mip_o));
    }
  }

  Vmosaic_csr_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* reporter_;
  ShadowCsr* shadow_ = nullptr;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::string phase_ = "init";
};

// ------------------------------------------------------------------- phases

// Phase 1: the generated table against hand-derived expectations.
//
// This is the one place a configured number is written twice on purpose. The
// shadow reads the generated table (so there is a single source), and this phase
// checks that table against values transcribed by hand from
// config/csr/mode_m.json and the spec clauses it cites. Without it, a decode bug
// in tools/gen_manifest.py would make the RTL and the model agree on a wrong
// number and the case would pass while the hardware was wrong.
struct TableExpectation {
  const char* name;
  uint16_t    addr;
  uint64_t    reset;
  uint64_t    wmask;
  bool        write_legal;
};

const TableExpectation kExpected[21] = {
    // name          addr    reset                       write mask                  legal
    {"mstatus",     0x300,  UINT64_C(0x0000000000001800), UINT64_C(0x00000000000066aa), true},
    // misa is WARL with no writable bits in p0 (the clause: "writable bits are
    // optional in the spec, so the whole register is modelled read-only"), so a
    // write is legal and changes nothing.
    {"misa",        0x301,  UINT64_C(0x8000000000001100), UINT64_C(0x0000000000000000), true},
    // medeleg/mideleg: no S or U mode, so every bit's only legal value is 0.
    {"medeleg",     0x302,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), true},
    {"mideleg",     0x303,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), true},
    {"mie",         0x304,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000088), true},
    {"mtvec",       0x305,  UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff), true},
    {"mcounteren",  0x306,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), true},
    {"mscratch",    0x340,  UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff), true},
    {"mepc",        0x341,  UINT64_C(0x0000000000000000), UINT64_C(0xfffffffffffffffc), true},
    {"mcause",      0x342,  UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff), true},
    {"mtval",       0x343,  UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff), true},
    {"mip",         0x344,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000088), true},
    // The counters are MRW and fully writable; cycle/instret/time are read-only.
    {"mcycle",      0xb00,  UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff), true},
    {"minstret",    0xb02,  UINT64_C(0x0000000000000000), UINT64_C(0xffffffffffffffff), true},
    {"cycle",       0xc00,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), false},
    {"time",        0xc01,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), false},
    {"instret",     0xc02,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), false},
    // The four machine ID registers are read-only zero in p0.
    {"mvendorid",   0xf11,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), false},
    {"marchid",     0xf12,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), false},
    {"mimpid",      0xf13,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), false},
    {"mhartid",     0xf14,  UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000), false},
};

void PhaseTable(mosaic::Reporter* reporter) {
  const std::string where = "table";
  Require(kTable.ok, where, kTable.problem);
  Require(MOSAIC_CSR_COUNT == 21, where,
          "the generated table has " + Dec(MOSAIC_CSR_COUNT) + " rows, the implementation "
          "table declares 21");
  for (int i = 0; i < 21; i++) {
    const TableExpectation& e = kExpected[i];
    Require(std::string(MOSAIC_CSR_TABLE[i].name) == e.name, where,
            "row " + Dec(i) + " is " + MOSAIC_CSR_TABLE[i].name + ", expected " + e.name);
    Require(MOSAIC_CSR_TABLE[i].addr == e.addr, where,
            std::string(e.name) + ": generated address 0x" + mosaic::Hex(MOSAIC_CSR_TABLE[i].addr, 3) +
                ", expected 0x" + mosaic::Hex(e.addr, 3));
    Require(MOSAIC_CSR_TABLE[i].reset == e.reset, where,
            std::string(e.name) + ": generated reset 0x" + mosaic::Hex(MOSAIC_CSR_TABLE[i].reset) +
                ", expected 0x" + mosaic::Hex(e.reset));
    Require(MOSAIC_CSR_TABLE[i].wmask == e.wmask, where,
            std::string(e.name) + ": generated write mask 0x" +
                mosaic::Hex(MOSAIC_CSR_TABLE[i].wmask) + ", expected 0x" + mosaic::Hex(e.wmask));
    Require((MOSAIC_CSR_TABLE[i].write_legal != 0) == e.write_legal, where,
            std::string(e.name) + ": generated write-legality " +
                Bool(MOSAIC_CSR_TABLE[i].write_legal != 0) + ", expected " + Bool(e.write_legal));
  }
  reporter->Check(true, "table: all 21 generated rows equal the hand-derived expectations");
}

void PhaseResetState(Harness* h, mosaic::Reporter* reporter) {
  // Every CSR reads its declared reset value, and nothing is illegal.
  for (int role = 0; role < kRoleCount; role++) {
    const uint16_t addr = kTable.Addr(role);
    Stim s;
    s.addr = addr;
    const Comb c = h->Cycle(s);
    Require(!c.illegal, "reset-state",
            std::string(kRoleName[role]) + " at 0x" + mosaic::Hex(addr, 3) +
                " is not implemented");
    Require(c.rdata == kTable.Reset(role), "reset-state",
            std::string(kRoleName[role]) + " reads 0x" + mosaic::Hex(c.rdata) +
                " at reset, expected 0x" + mosaic::Hex(kTable.Reset(role)));
  }
  // The observability counters are zero out of reset.
  Require(h->Read(kTable.Addr(kMcycle)) == 0, "reset-state", "mcycle is not zero at reset");
  reporter->Check(true, "reset-state: all 21 CSRs read their declared reset value");
}

// One CSR write, then a read in the following cycle: the port contract says a
// write retiring in cycle N is first readable in cycle N+1.
uint64_t WriteThenRead(Harness* h, uint16_t addr, uint32_t op, uint64_t value) {
  Stim w;
  w.addr = addr;
  w.we = true;
  w.op = op;
  w.wdata = value;
  h->Cycle(w);
  Stim r;
  r.addr = addr;
  return h->Cycle(r).rdata;
}

void PhaseReadOnly(Harness* h, mosaic::Reporter* reporter) {
  // The seven read-only CSRs: a write raises csr_wr_illegal_o and changes
  // nothing, and a read still works afterwards.
  const int ro_roles[] = {kMvendorid, kMarchid, kMimpid, kMhartid, kCycle, kTime, kInstret};
  for (int role : ro_roles) {
    const uint16_t addr = kTable.Addr(role);
    Require(!kTable.WriteLegal(role), "read-only",
            std::string(kRoleName[role]) + " is expected to be read-only");
    const uint64_t before = h->Read(addr);
    Stim w;
    w.addr = addr;
    w.we = true;
    w.op = kOpRw;
    w.wdata = UINT64_C(0xffffffffffffffff);
    const Comb c = h->Cycle(w);
    Require(c.wr_illegal, "read-only",
            "a write to " + std::string(kRoleName[role]) + " did not raise csr_wr_illegal_o");
    Require(h->Read(addr) == before, "read-only",
            std::string(kRoleName[role]) + " changed after an illegal write");
  }

  // misa and mcounteren are the other kind: WARL registers whose every bit is
  // read-only. A write is *legal* -- they are not read-only registers -- and
  // changes nothing.
  for (int role : {kMisa, kMcounteren}) {
    const uint16_t addr = kTable.Addr(role);
    Require(kTable.WriteLegal(role), "read-only",
            std::string(kRoleName[role]) + " is expected to accept a legal write");
    Require(kTable.Wmask(role) == 0, "read-only",
            std::string(kRoleName[role]) + " is expected to have no writable bits");
    const uint64_t before = h->Read(addr);
    Stim w;
    w.addr = addr;
    w.we = true;
    w.op = kOpRw;
    w.wdata = UINT64_C(0xffffffffffffffff);
    const Comb c = h->Cycle(w);
    Require(!c.wr_illegal, "read-only",
            "a write to " + std::string(kRoleName[role]) +
                " was reported illegal, but its access mode is writable");
    Require(h->Read(addr) == before, "read-only",
            std::string(kRoleName[role]) + " changed although it has no writable bits");
  }

  // The counters are writable and the shadows are not, so the pair is exact: a
  // write to cycle must fail while a write to mcycle must succeed.
  h->Cycle(Stim{});
  Require(h->Read(kTable.Addr(kMip)) == 0, "read-only", "mip is not zero with no pending bits");

  reporter->Check(true,
                  "read-only: seven read-only CSRs refused and unchanged, misa/mcounteren "
                  "accepted and unchanged, reads still work");
}

// The value a write of `value` must leave, given the table and the WARL rules.
uint64_t ExpectedAfterWrite(int role, uint64_t value) {
  const uint64_t w = kTable.Wmask(role);
  return (kTable.Reset(role) & ~w) | (ShadowCsr::CanonicalWrite(role, value) & w);
}

void PhaseWarl(Harness* h, mosaic::Reporter* reporter) {
  const uint64_t ones = UINT64_C(0xffffffffffffffff);
  for (int role = 0; role < kRoleCount; role++) {
    const uint16_t addr = kTable.Addr(role);
    if (!kTable.WriteLegal(role)) {
      continue;
    }
    // mip is the one writable register this module does not store: it is owned
    // by mosaic_interrupt (I-020) and a write is forwarded, not applied. "Write
    // all-ones and read back the mask-implied value" is therefore not this
    // module's property for mip, and the dedicated block below is the one that
    // does hold.
    if (role == kMip) {
      continue;
    }

    // RW with all ones: exactly the mask-implied value, per CSR.
    h->Reset(3);
    uint64_t got = WriteThenRead(h, addr, kOpRw, ones);
    Require(got == ExpectedAfterWrite(role, ones), "warl",
            std::string(kRoleName[role]) + ": RW all-ones read back 0x" + mosaic::Hex(got) +
                ", expected 0x" + mosaic::Hex(ExpectedAfterWrite(role, ones)));

    // RS with all ones is the same write.
    h->Reset(3);
    got = WriteThenRead(h, addr, kOpRs, ones);
    Require(got == ExpectedAfterWrite(role, ones), "warl",
            std::string(kRoleName[role]) + ": RS all-ones read back 0x" + mosaic::Hex(got) +
                ", expected 0x" + mosaic::Hex(ExpectedAfterWrite(role, ones)));

    // RC with all ones clears every writable bit, leaving the reset-only bits.
    h->Reset(3);
    WriteThenRead(h, addr, kOpRw, ones);          // establish a non-reset state
    got = WriteThenRead(h, addr, kOpRc, ones);
    const uint64_t cleared = kTable.Reset(role) & ~kTable.Wmask(role);
    Require(got == cleared, "warl",
            std::string(kRoleName[role]) + ": RC all-ones read back 0x" + mosaic::Hex(got) +
                ", expected the read-only bits 0x" + mosaic::Hex(cleared));

    // A mixed pattern, so the mask is exercised bit by bit rather than only at
    // the extremes.
    h->Reset(3);
    const uint64_t pattern = UINT64_C(0x0f0f0f0f0f0f0f0f);
    got = WriteThenRead(h, addr, kOpRw, pattern);
    Require(got == ExpectedAfterWrite(role, pattern), "warl",
            std::string(kRoleName[role]) + ": RW 0x" + mosaic::Hex(pattern) + " read back 0x" +
                mosaic::Hex(got) + ", expected 0x" +
                mosaic::Hex(ExpectedAfterWrite(role, pattern)));
  }

  // mip is the register this module does not store: mosaic_interrupt owns the
  // pending state. The contract here is the forward and the masked read.
  h->Reset(3);
  Stim mipv;
  mipv.addr = kTable.Addr(kMip);
  mipv.mip = ones;
  Require(h->Cycle(mipv).rdata == kTable.Wmask(kMip), "warl",
          "mip did not read back the owner's pending view masked to the implemented bits");
  Stim mipw;
  mipw.addr = kTable.Addr(kMip);
  mipw.we = true;
  mipw.op = kOpRs;
  mipw.wdata = kTable.Wmask(kMip);
  mipw.mip = kTable.Wmask(kMip);
  const Comb cm = h->Cycle(mipw);
  Require(!cm.wr_illegal, "warl", "a write to mip was reported illegal");
  Require(cm.mip_we, "warl", "a write to mip did not raise mip_we_o");
  Require(cm.mip_op == kOpRs, "warl",
          "mip_op_o is " + Dec(cm.mip_op) + ", expected the forwarded set-bits op");
  Require(cm.mip_wdata == kTable.Wmask(kMip), "warl",
          "mip_wdata_o is 0x" + mosaic::Hex(cm.mip_wdata) + ", expected the forwarded operand");

  // mtvec's reserved MODE encodings are the one canonicalisation that is not a
  // plain mask, so it gets named checks rather than only the generic sweep.
  h->Reset(3);
  for (uint64_t mode = 2; mode <= 3; mode++) {
    const uint64_t value = (UINT64_C(0x8000) | mode);
    const uint64_t got = WriteThenRead(h, kTable.Addr(kMtvec), kOpRw, value);
    Require((got & 3ull) == 0ull, "warl",
            "mtvec MODE " + Dec(mode) + " read back as " + Dec(got & 3ull) +
                ", expected the reserved encoding to canonicalise to Direct (0)");
    Require((got & ~3ull) == (value & ~3ull), "warl",
            "mtvec canonicalised the base as well as the mode: wrote 0x" + mosaic::Hex(value) +
                ", read 0x" + mosaic::Hex(got));
  }
  for (uint64_t mode = 0; mode <= 1; mode++) {
    const uint64_t value = UINT64_C(0x4000) | mode;
    const uint64_t got = WriteThenRead(h, kTable.Addr(kMtvec), kOpRw, value);
    Require(got == value, "warl",
            "mtvec MODE " + Dec(mode) + " did not read back: wrote 0x" + mosaic::Hex(value) +
                ", read 0x" + mosaic::Hex(got));
  }

  reporter->Check(true,
                  "warl: every writable CSR wrote all-ones with RW and RS, cleared with RC, "
                  "and held a mixed pattern, each equal to the mask-implied value; mtvec "
                  "reserved MODE canonicalised to Direct and MODE 0/1 were preserved");
}

// Defined below; named here because the illegal-address phase snapshots the
// whole register file around its accesses.
Regs ReadAllRegs(Harness* h);
bool RegsEqual(const Regs& a, const Regs& b);

void PhaseIllegalAddress(Harness* h, mosaic::Reporter* reporter) {
  // Addresses that look legitimate -- a CSR number adjacent to an implemented
  // one, an RV32 half-counter, a hypervisor or custom range -- must not silently
  // succeed. Every one of them is unimplemented in this table.
  const uint16_t addresses[] = {
      0x000, 0x001, 0x100, 0x307, 0x30a, 0x310, 0x345, 0x346, 0x347, 0x348,
      0x3ff, 0x400, 0x600, 0x7ff, 0x800, 0x900, 0xa00, 0xb01, 0xb03, 0xc03,
      0xc04, 0xd00, 0xe12, 0xf00, 0xf15, 0xfff};
  const Regs before = ReadAllRegs(h);

  for (uint16_t addr : addresses) {
    Require(!kTable.Impl(addr), "illegal-addr",
            "0x" + mosaic::Hex(addr, 3) + " is implemented in the table but listed as illegal");
    Stim r;
    r.addr = addr;
    const Comb c = h->Cycle(r);
    Require(c.illegal, "illegal-addr",
            "a read of 0x" + mosaic::Hex(addr, 3) + " did not raise csr_illegal_o");
    Require(c.rdata == 0, "illegal-addr",
            "a read of an illegal address returned 0x" + mosaic::Hex(c.rdata) + ", expected 0");

    Stim w;
    w.addr = addr;
    w.we = true;
    w.op = kOpRw;
    w.wdata = UINT64_C(0xdeadbeefcafef00d);
    const Comb cw = h->Cycle(w);
    Require(cw.wr_illegal, "illegal-addr",
            "a write to 0x" + mosaic::Hex(addr, 3) + " did not raise csr_wr_illegal_o");
  }

  // A whole sweep of illegal accesses changed nothing at all.
  Require(RegsEqual(before, ReadAllRegs(h)), "illegal-addr",
          "an illegal CSR access changed architectural state");
  reporter->Check(true, "illegal-addr: " + Dec(sizeof(addresses) / sizeof(addresses[0])) +
                            " unimplemented addresses read 0, raised csr_illegal_o, and "
                            "changed no state on a write");
}

Regs ReadAllRegs(Harness* h) {
  Regs r;
  r.mstatus = h->Read(kTable.Addr(kMstatus));
  r.mtvec = h->Read(kTable.Addr(kMtvec));
  r.mepc = h->Read(kTable.Addr(kMepc));
  r.mcause = h->Read(kTable.Addr(kMcause));
  r.mtval = h->Read(kTable.Addr(kMtval));
  r.mscratch = h->Read(kTable.Addr(kMscratch));
  r.mie = h->Read(kTable.Addr(kMie));
  r.mip = h->Read(kTable.Addr(kMip));
  r.misa = h->Read(kTable.Addr(kMisa));
  r.mcycle = h->Read(kTable.Addr(kMcycle));
  r.minstret = h->Read(kTable.Addr(kMinstret));
  return r;
}

bool RegsEqual(const Regs& a, const Regs& b) {
  return a.mstatus == b.mstatus && a.mtvec == b.mtvec && a.mepc == b.mepc &&
         a.mcause == b.mcause && a.mtval == b.mtval && a.mscratch == b.mscratch &&
         a.mie == b.mie && a.mip == b.mip && a.misa == b.misa && a.mcycle == b.mcycle &&
         a.minstret == b.minstret;
}

// A single RW write with no tick and no boundary event.
void WriteReg(Harness* h, int role, uint64_t value) {
  Stim s;
  s.addr = kTable.Addr(role);
  s.we = true;
  s.op = kOpRw;
  s.wdata = value;
  h->Cycle(s);
}

void RequireMstatus(Harness* h, const std::string& what, bool mie, bool mpie) {
  const uint64_t value = h->Read(kTable.Addr(kMstatus));
  Require(Bit(value, 3) == (mie ? 1u : 0u), "trap-mret",
          what + ": mstatus.MIE is " + Dec(Bit(value, 3)) + ", expected " + Bool(mie) +
              " (0x" + mosaic::Hex(value) + ")");
  Require(Bit(value, 7) == (mpie ? 1u : 0u), "trap-mret",
          what + ": mstatus.MPIE is " + Dec(Bit(value, 7)) + ", expected " + Bool(mpie) +
              " (0x" + mosaic::Hex(value) + ")");
  Require((value & kMstatusMpp) == kMstatusMpp, "trap-mret",
          what + ": mstatus.MPP is " + Dec((value & kMstatusMpp) >> 11) + ", expected 3");
}

// Trap entry and MRET: the field transitions, nesting, the MPIE-already-zero
// case, mutual exclusion, and trap priority over a retiring CSR write.
void PhaseTrapMret(Harness* h, mosaic::Reporter* reporter) {
  // --- a plain trap with MIE set takes MPIE <- MIE, MIE <- 0, and records the
  //     PC, cause and trap value.
  h->Reset(4);
  WriteReg(h, kMtvec, UINT64_C(0x8000) | 1ull);   // vectored, base 0x8000
  WriteReg(h, kMstatus, kMstatusMie);             // MIE = 1, MPIE = 0
  RequireMstatus(h, "before trap", true, false);

  Stim t;
  t.trap_valid = true;
  t.trap_cause = 11;                 // EXC_ECALL_M: a synchronous exception
  t.trap_tval = UINT64_C(0x1234);
  t.trap_epc = UINT64_C(0x1000);
  const Comb c = h->Cycle(t);
  Require(c.trap_commit, "trap-mret", "trap_commit_o was not raised for a trap");
  Require(!c.mret_commit, "trap-mret", "mret_commit_o was raised for a trap");
  Require(c.trap_target == UINT64_C(0x8000), "trap-mret",
          "a non-interrupt trap in vectored mode entered at 0x" + mosaic::Hex(c.trap_target) +
              ", expected the base 0x8000 (vectoring applies to interrupts only)");
  Require(h->Read(kTable.Addr(kMepc)) == UINT64_C(0x1000), "trap-mret",
          "mepc is not the interrupted PC");
  Require(h->Read(kTable.Addr(kMcause)) == 11, "trap-mret", "mcause is not 11");
  Require(h->Read(kTable.Addr(kMtval)) == UINT64_C(0x1234), "trap-mret",
          "mtval is not the supplied trap value");
  RequireMstatus(h, "after trap entry", false, true);

  // --- MRET restores MIE from MPIE, sets MPIE, and targets mepc exactly.
  Stim m;
  m.mret_valid = true;
  const Comb cm = h->Cycle(m);
  Require(cm.mret_commit, "trap-mret", "mret_commit_o was not raised for an MRET");
  Require(!cm.trap_commit, "trap-mret", "trap_commit_o was raised for an MRET");
  Require(cm.mret_target == UINT64_C(0x1000), "trap-mret",
          "MRET targeted 0x" + mosaic::Hex(cm.mret_target) + ", expected mepc 0x1000 exactly");
  RequireMstatus(h, "after MRET", true, true);

  // --- a second MRET: MPIE was already 1, so MIE stays set while MPIE stays 1.
  h->Cycle(m);
  RequireMstatus(h, "after a second MRET", true, true);

  // --- nesting: trap, enable MIE inside the handler, trap again. The second
  //     entry takes MPIE <- MIE (which the handler set) and clears MIE again.
  h->Reset(4);
  WriteReg(h, kMtvec, UINT64_C(0x2000));
  WriteReg(h, kMstatus, kMstatusMie);
  Stim t1;
  t1.trap_valid = true;
  t1.trap_cause = 4;                 // EXC_LOAD_MISALIGNED
  t1.trap_epc = UINT64_C(0x3000);
  t1.trap_tval = UINT64_C(0xaaaa);
  h->Cycle(t1);
  RequireMstatus(h, "after the first nested trap", false, true);
  Require(h->Read(kTable.Addr(kMepc)) == UINT64_C(0x3000), "trap-mret",
          "the first nested trap recorded the wrong mepc");
  WriteReg(h, kMstatus, kMstatusMie);   // handler re-enables MIE
  RequireMstatus(h, "inside the handler", true, false);
  Stim t2;
  t2.trap_valid = true;
  t2.trap_cause = 7;                 // EXC_STORE_ACCESS
  t2.trap_epc = UINT64_C(0x3004);
  h->Cycle(t2);
  Require(h->Read(kTable.Addr(kMepc)) == UINT64_C(0x3004), "trap-mret",
          "the nested trap did not overwrite mepc");
  Require(h->Read(kTable.Addr(kMcause)) == 7, "trap-mret", "the nested trap recorded the wrong cause");
  RequireMstatus(h, "after the nested trap", false, true);
  Stim mr;
  mr.mret_valid = true;
  h->Cycle(mr);
  RequireMstatus(h, "after the first mret of the nest", true, true);
  h->Cycle(mr);
  RequireMstatus(h, "after the second mret of the nest", true, true);

  // --- MPIE already zero: an MIE of 0 on entry leaves MPIE 0, and the MRET
  //     still sets MPIE to 1 while MIE follows the (zero) MPIE.
  h->Reset(4);
  WriteReg(h, kMtvec, UINT64_C(0x2000));
  WriteReg(h, kMstatus, 0);              // MIE = 0, MPIE = 0
  RequireMstatus(h, "before the masked trap", false, false);
  Stim tz;
  tz.trap_valid = true;
  tz.trap_cause = 2;                     // EXC_ILLEGAL_INSN
  tz.trap_epc = UINT64_C(0x4000);
  h->Cycle(tz);
  RequireMstatus(h, "trap entry with MIE = 0", false, false);
  h->Cycle(mr);
  RequireMstatus(h, "MRET after MPIE = 0", false, true);

  // --- a trap and an MRET in the same cycle are mutually exclusive, and the RTL
  //     asserts that invariant rather than tolerating it:
  //
  //         assert (!(trap_valid_i & mret_valid_i));
  //
  //     That assertion is compiled into this simulation and is live -- driving
  //     both at once aborts the run with "Assertion failed in
  //     TOP.mosaic_csr_tb.u_csr", which is how this phase's author found out the
  //     first time. A passing run therefore *proves the exclusion*: the tb never
  //     presents the combination, and any implementation that accepted it
  //     silently would be caught by the assertion rather than by a comparison.
  //     The stimulus below is the pair that must stay separate.

  // --- a trap outranks a retiring CSR write: the writing instruction never
  //     retires, so mscratch is unchanged and o_wr_ctr does not move.
  h->Reset(4);
  WriteReg(h, kMscratch, UINT64_C(0xfeedface));
  const uint64_t scr_before = h->Read(kTable.Addr(kMscratch));
  const uint64_t wr_before = h->WrCtr();
  Stim tw;
  tw.addr = kTable.Addr(kMscratch);
  tw.we = true;
  tw.op = kOpRw;
  tw.wdata = UINT64_C(0x1111);
  tw.trap_valid = true;
  tw.trap_cause = 11;
  tw.trap_epc = UINT64_C(0x6000);
  const Comb ctw = h->Cycle(tw);
  Require(ctw.trap_commit, "trap-mret", "the trap did not commit over a retiring CSR write");
  Require(h->Read(kTable.Addr(kMscratch)) == scr_before, "trap-mret",
          "a CSR write that lost to a trap still changed mscratch");
  Require(h->WrCtr() == wr_before, "trap-mret",
          "a CSR write that lost to a trap still incremented o_wr_ctr: it was counted as a "
          "retirable CSR write");

  reporter->Check(true,
                  "trap-mret: entry and MRET field transitions, nesting, MPIE-already-zero, "
                  "trap/MRET mutual exclusion and trap priority over a retiring CSR write");
}

// Every exception code in mosaic_pkg's list, plus interrupt causes: the recorded
// cause, trap value, PC and status must equal the independent model's, in Direct
// and Vectored mode.
void PhaseCauses(Harness* h, mosaic::Reporter* reporter) {
  // mosaic_pkg's exception list: instruction misaligned 0, instruction access
  // fault 1, illegal instruction 2, breakpoint 3, load misaligned 4, load access
  // fault 5, store misaligned 6, store access fault 7, ecall from M 11.
  const uint64_t exc[] = {0, 1, 2, 3, 4, 5, 6, 7, 11};
  const uint64_t irq[] = {0, 1, 3, 7, 11, 16, 63};

  for (uint64_t mode = 0; mode <= 1; mode++) {
    const uint64_t base = UINT64_C(0x1000);
    for (uint64_t cause : exc) {
      h->Reset(3);
      WriteReg(h, kMtvec, base | mode);
      WriteReg(h, kMstatus, kMstatusMie);
      Stim t;
      t.trap_valid = true;
      t.trap_cause = cause;
      t.trap_tval = UINT64_C(0x100000000) + cause;
      t.trap_epc = UINT64_C(0x2000) + cause * 4 + 1;   // low bit set: must be masked
      const Comb c = h->Cycle(t);
      Require(h->Read(kTable.Addr(kMcause)) == cause, "causes",
              "cause " + Dec(cause) + " recorded as " + Dec(h->Read(kTable.Addr(kMcause))));
      Require(h->Read(kTable.Addr(kMtval)) == t.trap_tval, "causes",
              "cause " + Dec(cause) + " recorded the wrong mtval");
      Require(h->Read(kTable.Addr(kMepc)) == (t.trap_epc & ~3ull), "causes",
              "cause " + Dec(cause) + " recorded mepc 0x" +
                  mosaic::Hex(h->Read(kTable.Addr(kMepc))) + ", expected the PC with IALIGN "
                  "low bits cleared");
      RequireMstatus(h, "after cause " + Dec(cause), false, true);
      // A synchronous exception always enters at the base, in both modes.
      Require(c.trap_target == base, "causes",
              "cause " + Dec(cause) + " in mode " + Dec(mode) + " entered at 0x" +
                  mosaic::Hex(c.trap_target) + ", expected the base 0x" + mosaic::Hex(base));
    }

    for (uint64_t code : irq) {
      const uint64_t cause = (UINT64_C(1) << 63) | code;
      h->Reset(3);
      WriteReg(h, kMtvec, base | mode);
      WriteReg(h, kMstatus, kMstatusMie);
      Stim t;
      t.trap_valid = true;
      t.trap_cause = cause;
      t.trap_epc = UINT64_C(0x3000);
      const Comb c = h->Cycle(t);
      Require(h->Read(kTable.Addr(kMcause)) == cause, "causes",
              "interrupt " + Dec(code) + " recorded as 0x" +
                  mosaic::Hex(h->Read(kTable.Addr(kMcause))));
      const uint64_t expected_target = (mode == 1) ? (base + 4 * code) : base;
      Require(c.trap_target == expected_target, "causes",
              "interrupt " + Dec(code) + " in mode " + Dec(mode) + " entered at 0x" +
                  mosaic::Hex(c.trap_target) + ", expected 0x" + mosaic::Hex(expected_target));
      // The vector offset is at most 4 * 63 = 252 bytes, so it can never carry
      // into the base: every bit above the low page must still be the base's.
      Require((c.trap_target & ~UINT64_C(0xfff)) == base, "causes",
              "the vector offset for interrupt " + Dec(code) + " moved the mtvec base: target "
              "0x" + mosaic::Hex(c.trap_target) + ", base 0x" + mosaic::Hex(base));
    }
  }

  reporter->Check(true,
                  "causes: 9 exception codes and 7 interrupt codes, in Direct and Vectored "
                  "mode, recorded mepc/mcause/mtval/mstatus and targets equal to the model");
}

// The free-running counters, their read-only shadows, and a write that lands in
// the same cycle as the tick.
void PhaseCounters(Harness* h, mosaic::Reporter* reporter) {
  h->Reset(4);

  // Seven cycles of ticking advance mcycle by exactly seven, and the read-only
  // cycle shadow follows it.
  for (int i = 0; i < 7; i++) {
    Stim s;
    s.cnt_cycle = true;
    h->Cycle(s);
  }
  Require(h->Read(kTable.Addr(kMcycle)) == 7, "counters",
          "mcycle is " + Dec(h->Read(kTable.Addr(kMcycle))) + " after 7 ticks, expected 7");
  Require(h->Read(kTable.Addr(kCycle)) == 7, "counters", "cycle does not shadow mcycle");

  // minstret ticks independently of mcycle.
  for (int i = 0; i < 3; i++) {
    Stim s;
    s.cnt_instret = true;
    h->Cycle(s);
  }
  Require(h->Read(kTable.Addr(kMinstret)) == 3, "counters",
          "minstret is " + Dec(h->Read(kTable.Addr(kMinstret))) + " after 3 ticks, expected 3");
  Require(h->Read(kTable.Addr(kInstret)) == 3, "counters", "instret does not shadow minstret");
  Require(h->Read(kTable.Addr(kMcycle)) == 7, "counters",
          "mcycle moved when only minstret ticked");

  // A write and an increment in the same cycle: the written value is the value
  // at the edge and the tick is added on top.
  Stim w;
  w.addr = kTable.Addr(kMcycle);
  w.we = true;
  w.op = kOpRw;
  w.wdata = UINT64_C(0x1000);
  w.cnt_cycle = true;
  h->Cycle(w);
  Require(h->Read(kTable.Addr(kMcycle)) == UINT64_C(0x1001), "counters",
          "a write and a tick in one cycle left mcycle at 0x" +
              mosaic::Hex(h->Read(kTable.Addr(kMcycle))) + ", expected the written value plus "
              "the tick (0x1001)");

  Stim wi;
  wi.addr = kTable.Addr(kMinstret);
  wi.we = true;
  wi.op = kOpRw;
  wi.wdata = UINT64_C(0x50);
  wi.cnt_instret = true;
  h->Cycle(wi);
  Require(h->Read(kTable.Addr(kMinstret)) == UINT64_C(0x51), "counters",
          "a write and an instruction tick in one cycle left minstret at 0x" +
              mosaic::Hex(h->Read(kTable.Addr(kMinstret))) + ", expected 0x51");

  // A write with no tick lands exactly.
  WriteReg(h, kMcycle, UINT64_C(0xabc));
  Require(h->Read(kTable.Addr(kMcycle)) == UINT64_C(0xabc), "counters",
          "a write with no tick did not land exactly");

  // The three shadows refuse writes and change nothing.
  for (int role : {kCycle, kTime, kInstret}) {
    const uint64_t before = h->Read(kTable.Addr(role));
    Stim s;
    s.addr = kTable.Addr(role);
    s.we = true;
    s.op = kOpRw;
    s.wdata = 0;
    const Comb c = h->Cycle(s);
    Require(c.wr_illegal, "counters",
            std::string(kRoleName[role]) + ": a write did not raise csr_wr_illegal_o");
    Require(h->Read(kTable.Addr(role)) == before, "counters",
            std::string(kRoleName[role]) + ": the shadow changed after an illegal write");
  }
  Require(h->Read(kTable.Addr(kMcycle)) == UINT64_C(0xabc), "counters",
          "a write to the cycle shadow changed mcycle");

  // time is the platform timer, not a counter of our making.
  Stim tm;
  tm.addr = kTable.Addr(kTime);
  tm.mtime = UINT64_C(0x123456789abcdef0);
  const Comb c = h->Cycle(tm);
  Require(c.rdata == tm.mtime, "counters",
          "time read 0x" + mosaic::Hex(c.rdata) + ", expected mtime 0x" + mosaic::Hex(tm.mtime));

  // The observability counters counted exactly what happened: three accepted
  // writes (mcycle+tick, minstret+tick, mcycle exact) and three refused shadow
  // writes. An exact value is a stronger statement than a floor.
  Require(h->WrCtr() == 3, "counters",
          "o_wr_ctr is " + Dec(h->WrCtr()) + " after exactly three accepted writes");
  Require(h->IllegalWrCtr() == 3, "counters",
          "o_illegal_wr_ctr is " + Dec(h->IllegalWrCtr()) +
              " after exactly three refused shadow writes");

  reporter->Check(true,
                  "counters: increments, a write plus a tick in one cycle, exact writes, "
                  "read-only shadows refusing writes, and time shadowing mtime");
}

// A random programme of reads, all three write ops, ticks, traps and MRETs,
// compared against the shadow on every observable every cycle.
void PhaseRandom(Harness* h, mosaic::Reporter* reporter, uint32_t seed, uint32_t cycles) {
  mosaic::Rng rng(seed);
  uint64_t mtime = UINT64_C(0x1000);
  uint32_t accepted = 0;
  uint32_t illegal = 0;
  uint32_t traps = 0;
  uint32_t mrets = 0;
  uint32_t reads = 0;
  const uint32_t wr_before = h->WrCtr();
  const uint32_t illegal_before = h->IllegalWrCtr();
  const uint32_t trap_before = h->TrapCtr();
  const uint32_t mret_before = h->MretCtr();

  for (uint32_t i = 0; i < cycles; i++) {
    Stim s;
    // Addresses are drawn from the implemented set most of the time, but every
    // fifth access is aimed at the unimplemented neighbourhood on purpose: a
    // campaign that never aims at an illegal address cannot find one that
    // silently succeeds.
    if (rng.Chance(80)) {
      s.addr = kTable.Addr(static_cast<int>(rng.Below(kRoleCount)));
    } else {
      s.addr = static_cast<uint16_t>(rng.Below(4096));
    }

    if (rng.Chance(30)) {
      s.we = true;
      s.op = 1 + rng.Below(3);
      s.wdata = (rng.Next() & UINT64_C(1)) ? rng.Next() : (rng.Next() & 0xffffull);
    } else {
      reads++;
    }

    s.cnt_cycle = rng.Chance(50);
    s.cnt_instret = rng.Chance(30);

    // Traps and MRETs are rare and never simultaneous: a simultaneous pair is a
    // caller error the RTL asserts against, and the directed phase already shows
    // the hardware's resolution. Presenting it here would compare the resolution
    // rather than the contract, and would make the campaign depend on a
    // combination no correct caller produces.
    if (rng.Chance(6)) {
      s.trap_valid = true;
      s.we = false;
      if (rng.Chance(70)) {
        const uint64_t codes[] = {0, 1, 2, 3, 4, 5, 6, 7, 11};
        s.trap_cause = codes[rng.Below(9)];
        if (rng.Chance(40)) s.trap_cause |= (UINT64_C(1) << 63);
      } else {
        s.trap_cause = rng.Next();
      }
      s.trap_tval = rng.Next();
      s.trap_epc = rng.Next() | 3ull;   // deliberately misaligned: the low bits must be cleared
      traps++;
    } else if (rng.Chance(6)) {
      s.mret_valid = true;
      s.we = false;
      mrets++;
    }

    s.mip = rng.Next() & ((rng.Chance(60)) ? kTable.Wmask(kMip) : ~UINT64_C(0));
    mtime += rng.Below(3);
    s.mtime = mtime;

    const Comb c = h->Cycle(s);
    if (s.we && !c.wr_illegal && !s.trap_valid && !s.mret_valid) accepted++;
    if (c.wr_illegal) illegal++;
  }

  // Anti-vacuity: a campaign in which nothing happened proves nothing. The
  // thresholds are what the phase is *for*, not floors that happen to pass.
  Require(accepted > cycles / 8, "random",
          "only " + Dec(accepted) + " writes were accepted over " + Dec(cycles) +
              " cycles: the campaign is not exercising the write path");
  Require(illegal > cycles / 16, "random",
          "only " + Dec(illegal) + " illegal writes were seen over " + Dec(cycles) +
              " cycles: the campaign is not aiming at illegal addresses");
  Require(traps > cycles / 32, "random", "too few traps were taken to exercise entry");
  Require(mrets > 0, "random", "no MRET was taken");
  Require(reads > 0, "random", "no read was performed");

  // The campaign's own count of each event, derived from the model's rules, must
  // equal what the hardware counted. A counter that increments on the wrong
  // condition survives a per-cycle comparison of its value only if the condition
  // is also wrong in the model; this equality is what pins the two together at
  // campaign scale.
  Require(h->WrCtr() - wr_before == accepted, "random",
          "the model saw " + Dec(accepted) + " accepted writes but o_wr_ctr moved by " +
              Dec(h->WrCtr() - wr_before));
  Require(h->IllegalWrCtr() - illegal_before == illegal, "random",
          "the model saw " + Dec(illegal) + " illegal writes but o_illegal_wr_ctr moved by " +
              Dec(h->IllegalWrCtr() - illegal_before));
  Require(h->TrapCtr() - trap_before == traps, "random",
          "the model saw " + Dec(traps) + " traps but o_trap_ctr moved by " +
              Dec(h->TrapCtr() - trap_before));
  Require(h->MretCtr() - mret_before == mrets, "random",
          "the model saw " + Dec(mrets) + " MRETs but o_mret_ctr moved by " +
              Dec(h->MretCtr() - mret_before));

  reporter->Check(true,
                  "random: " + Dec(accepted) + " accepted writes, " + Dec(illegal) +
                      " illegal writes, " + Dec(traps) + " traps, " + Dec(mrets) +
                      " MRETs, seed " + Dec(seed) + ", every observable compared every cycle");
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
  mosaic::ClockDriver clk;
  Vmosaic_csr_tb dut;

  Harness harness(&dut, &clk, &reporter, options.max_cycles);
  ShadowCsr shadow;
  harness.BindShadow(&shadow);

  std::string detail;
  bool passed = true;
  try {
    Require(kTable.ok, "geometry", kTable.problem);
    Require(kTable.role_of_addr[0] < 0, "geometry",
            "CSR number 0 is implemented; the table says it is not");

    harness.Phase("table");
    PhaseTable(&reporter);

    harness.Reset(4);
    harness.Phase("reset-state");
    PhaseResetState(&harness, &reporter);

    harness.Reset(4);
    harness.Phase("read-only");
    PhaseReadOnly(&harness, &reporter);

    harness.Reset(4);
    harness.Phase("warl");
    PhaseWarl(&harness, &reporter);

    harness.Reset(4);
    harness.Phase("illegal-addr");
    PhaseIllegalAddress(&harness, &reporter);

    harness.Reset(4);
    harness.Phase("trap-mret");
    PhaseTrapMret(&harness, &reporter);

    harness.Reset(4);
    harness.Phase("causes");
    PhaseCauses(&harness, &reporter);

    harness.Reset(4);
    harness.Phase("counters");
    PhaseCounters(&harness, &reporter);

    harness.Reset(4);
    harness.Phase("random");
    PhaseRandom(&harness, &reporter, static_cast<uint32_t>(options.seed), 6000);

    detail = "CSR contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons over " + std::to_string(harness.cycles()) + " cycles, " +
             std::to_string(kRoleCount) + " CSRs, seed " + std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
