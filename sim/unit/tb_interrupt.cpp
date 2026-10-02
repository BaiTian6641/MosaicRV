// ============================================================================
// tb_interrupt.cpp -- CASE=interrupt.boundary_replay, work package I-020.
//
// The DUT is never its own oracle. Every output is compared, on every cycle of
// every phase, against an independent C++ shadow written from the *contract*
// documented in rtl/core/mosaic_interrupt.sv -- the two-flop sampling rule, the
// software-latch/OR rule for mip, the MEI > MSI > MTI priority, the delegation
// gate, the "enabled pending, regardless of mstatus.MIE" wake rule, and the
// saturating counters. The shadow shares no code with the RTL and never looks at
// Verilator internals, so agreeing with it is evidence about the contract rather
// than a restatement of the implementation.
//
// Two-snapshot discipline, from the project's standing rule: the combinational
// decision outputs (mip, irq_valid, irq_cause, the pending flags) are compared
// against the shadow computed from the *pre-edge* state -- they are what the DUT
// presented in the cycle under test. The registered state (halt and the four
// counters) is compared after the edge -- that is what is true now. Comparing a
// combinational offer after the edge, or a counter before it, would compare
// different cycles and produce a mismatch that names no real defect. Both
// snapshots of the combinational outputs are in fact checked (pre-edge and
// post-edge), which is strictly tighter than either alone.
//
// The metastability model the RTL documents is discrete: the synchroniser
// samples at rising edges, a pulse that does not span an edge is never observed,
// and a level becomes visible two edges after it is presented. This testbench
// therefore checks *cycles* -- the glitch phase asserts that a sub-cycle pulse
// produces no pending bit at all, and the latency phases assert the exact cycle
// at which a pulse first becomes visible. There is no analog metastability in a
// discrete simulation, and no statistical claim is made here.
//
// Phases, each of which can fail on its own:
//
//   1. reset-state        the documented cold state: no pending bits, no halt,
//                         counters at zero.
//   2. sync-and-glitch    a sub-cycle pulse is never observed; a one-cycle pulse
//                         appears exactly two edges later; a held level stays
//                         pending until two edges after it is dropped.
//   3. masking            all eight mie combinations and all eight source
//                         combinations, the priority order when several are
//                         pending, mstatus.MIE masking, and the mideleg gate
//                         (implemented, not assumed).
//   4. boundary-latency   an enabled pending interrupt with core_can_trap_i held
//                         low for a randomised interval: no offer in the window,
//                         pending preserved, and the trap offered at the first
//                         legal boundary whatever the latency was.
//   5. halt-resume        WFI enters the halt, a late enabled source wakes it
//                         exactly once, an already-pending enabled interrupt
//                         prevents the halt entirely, a pending-but-disabled
//                         source must not wake it, and mstatus.MIE does not
//                         affect the wake.
//   6. mip-software-write a software write raises MTIP with the platform idle
//                         and the trap follows; CSR_RC clears it; a write to a
//                         read-only bit is ignored; the platform request cannot
//                         be cleared by software.
//   7. replay             the card's central claim. A scripted event timeline is
//                         run twice with the same seed; the trap boundaries,
//                         wakes, halt entries, per-cycle trace hash and final
//                         counters are compared event by event, not just at the
//                         end.
//   8. random             a random soak of all stimulus kinds, compared against
//                         the shadow every cycle.
//
// Standing invariants, re-checked every cycle of every phase:
//
//   * irq_valid_o is never high unless `core_can_trap_i` is high, mstatus.MIE is
//     set, and an enabled, non-delegated pending bit exists.
//   * a pending interrupt is sticky for as long as its source or its software
//     latch holds it -- it is never dropped, and never taken while masked.
//   * the spurious-wake counter never moves.
//   * the halt is never entered while an enabled interrupt is pending.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "sim_common.h"
#include "mosaic_csr_table.h"

#include <cstring>
#include "Vmosaic_interrupt_tb.h"

namespace {

// Thrown on the first failed check, so the report names one defect rather than
// a thousand consequences of it.
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

// --------------------------------------------------------------- encodings
// Written from the contract, not read out of the DUT: the shadow has to be an
// independent statement of what these values mean.
constexpr uint32_t kBitMsip = 3;
constexpr uint32_t kBitStip = 5;
constexpr uint32_t kBitMtip = 7;
constexpr uint32_t kBitMeip = 11;

constexpr uint64_t kCauseMsi = 0x8000000000000003ull;
constexpr uint64_t kCauseSti = 0x8000000000000005ull;
constexpr uint64_t kCauseMti = 0x8000000000000007ull;
constexpr uint64_t kCauseMei = 0x800000000000000bull;

// The writable mip bits come from the profile's own generated CSR table, not
// from a literal. A p1 machine implements STIP (bit 5) because it has S-mode and
// a supervisor timer source, so a shadow that hardcoded p0's ["7","3"] was
// modelling a different machine than the one under test.
uint64_t MipWritableMaskFromProfile() {
  for (unsigned i = 0; i < MOSAIC_CSR_COUNT; i++) {
    if (std::strcmp(MOSAIC_CSR_TABLE[i].name, "mip") == 0) return MOSAIC_CSR_TABLE[i].wmask;
  }
  return 0;
}
const uint64_t kMipWritableMask = MipWritableMaskFromProfile();
// A supervisor timer source exists exactly when STIP is a writable bit.
const bool kHasStip = ((kMipWritableMask >> kBitStip) & 1ull) != 0;
// One timer source, two pending bits: a timer event raises MTIP and, in a
// profile with a supervisor timer, STIP as well.
const uint64_t kStipBit = kHasStip ? (1ull << kBitStip) : 0ull;
const uint64_t kTimerPendingBits = (1ull << kBitMtip) | kStipBit;

// mosaic_pkg::csr_op_e
constexpr uint32_t kCsrNone = 0;
constexpr uint32_t kCsrRw = 1;
constexpr uint32_t kCsrRs = 2;
constexpr uint32_t kCsrRc = 3;

constexpr uint64_t kAllMie = (1ull << kBitMsip) | (1ull << kBitMtip) | (1ull << kBitMeip);

// --------------------------------------------------------------- the stimulus
struct Stim {
  bool irq_soft = false;
  bool irq_timer = false;
  bool irq_ext = false;

  uint64_t mie = 0;
  uint64_t mideleg = 0;
  bool mstatus_mie = true;

  bool mip_we = false;
  uint32_t mip_op = kCsrNone;
  uint64_t mip_wdata = 0;

  bool core_can_trap = true;
  bool wfi_valid = false;
};

// The fields read back from the DUT. Combinational and registered outputs are
// kept in the same struct but are filled and compared in different snapshots.
struct Outputs {
  uint64_t mip = 0;
  bool irq_valid = false;
  uint64_t irq_cause = 0;
  bool irq_timer_pending = false;
  bool irq_soft_pending = false;
  bool irq_ext_pending = false;

  bool halt = false;
  uint8_t irq_ctr = 0;
  uint8_t halt_cycles = 0;
  uint8_t wake_ctr = 0;
  uint8_t spurious_wake_ctr = 0;
};

// ------------------------------------------------------------------ the shadow
class ShadowInterrupt {
 public:
  void Reset() {
    meta_ = 0;
    sync_ = 0;
    sw_msip_ = false;
    sw_stip_ = false;
    sw_mtip_ = false;
    halted_ = false;
    irq_ctr_ = 0;
    halt_cycles_ = 0;
    wake_ctr_ = 0;
    spurious_wake_ctr_ = 0;
  }

  // mip: platform pending OR software latch, on the bits the platform and the
  // config declare to exist. Everything else is read-only zero.
  uint64_t Pending() const {
    uint64_t mip = 0;
    if (sync_bit(0) || sw_msip_) mip |= (1ull << kBitMsip);
    if (sync_bit(1) || sw_mtip_) mip |= (1ull << kBitMtip);
    if (sync_bit(2)) mip |= (1ull << kBitMeip);
    // The platform timer raises STIP as well as MTIP in a profile that has a
    // supervisor timer (the one timer, and the delegation registers decide which
    // mode takes it). Software may also latch and clear it through mip[5].
    if (kHasStip && (sync_bit(1) || sw_stip_)) mip |= (1ull << kBitStip);
    return mip;
  }

  uint64_t Take(const Stim& s) const { return Pending() & s.mie & ~s.mideleg; }

  bool WakeLegal(const Stim& s) const { return (Pending() & s.mie) != 0; }

  void Comb(const Stim& s, Outputs* o) const {
    const uint64_t mip = Pending();
    const uint64_t take = mip & s.mie & ~s.mideleg;
    o->mip = mip;
    o->irq_timer_pending = ((mip >> kBitMtip) & 1) != 0;
    o->irq_soft_pending = ((mip >> kBitMsip) & 1) != 0;
    o->irq_ext_pending = ((mip >> kBitMeip) & 1) != 0;
    o->irq_valid = s.core_can_trap && s.mstatus_mie && (take != 0);
    if (((take >> kBitMeip) & 1) != 0) {
      o->irq_cause = kCauseMei;
    } else if (((take >> kBitMsip) & 1) != 0) {
      o->irq_cause = kCauseMsi;
    } else if (((take >> kBitMtip) & 1) != 0) {
      o->irq_cause = kCauseMti;
    } else if (((take >> kBitStip) & 1) != 0) {
      o->irq_cause = kCauseSti;
    } else {
      o->irq_cause = 0;
    }
    // The halt is registered, so its current value is state, not a function of
    // this cycle's inputs.
    o->halt = halted_;
  }

  // Advance over one rising edge. The combinational values that decide the edge
  // are computed from the pre-edge state, which is why nothing below updates the
  // synchroniser or the latch before the halt decision and the counters.
  void Apply(const Stim& s) {
    uint32_t raw = 0;
    if (s.irq_soft) raw |= 1u;
    if (s.irq_timer) raw |= 2u;
    if (s.irq_ext) raw |= 4u;
    const uint32_t meta_next = raw;
    const uint32_t sync_next = meta_;

    bool msip = sw_msip_;
    bool stip = sw_stip_;
    bool mtip = sw_mtip_;
    if (s.mip_we) {
      const uint64_t wd = s.mip_wdata & kMipWritableMask;
      const bool wd_msip = ((wd >> kBitMsip) & 1) != 0;
      const bool wd_stip = ((wd >> kBitStip) & 1) != 0;
      const bool wd_mtip = ((wd >> kBitMtip) & 1) != 0;
      if (s.mip_op == kCsrRw) {
        msip = wd_msip;
        stip = wd_stip;
        mtip = wd_mtip;
      } else if (s.mip_op == kCsrRs) {
        msip = msip || wd_msip;
        stip = stip || wd_stip;
        mtip = mtip || wd_mtip;
      } else if (s.mip_op == kCsrRc) {
        msip = msip && !wd_msip;
        stip = stip && !wd_stip;
        mtip = mtip && !wd_mtip;
      }
      // CSR_NONE with mip_we asserted writes nothing: the latch holds.
    }

    const bool wake_legal = (Pending() & s.mie) != 0;
    bool halted_d;
    if (s.wfi_valid && !wake_legal) {
      halted_d = true;
    } else if (wake_legal) {
      halted_d = false;
    } else {
      halted_d = halted_;
    }

    const bool halt_leave = halted_ && !halted_d;
    const bool legal_wake = halt_leave && wake_legal;
    const bool spurious_wake = halt_leave && !wake_legal;

    const bool irq_valid_now = s.core_can_trap && s.mstatus_mie && (Take(s) != 0);
    if (irq_valid_now) SatInc(&irq_ctr_);
    if (halted_) SatInc(&halt_cycles_);
    if (legal_wake) SatInc(&wake_ctr_);
    if (spurious_wake) SatInc(&spurious_wake_ctr_);

    meta_ = meta_next;
    sync_ = sync_next;
    sw_msip_ = msip;
    sw_stip_ = stip;
    sw_mtip_ = mtip;
    halted_ = halted_d;
  }

  bool halted() const { return halted_; }
  uint8_t irq_ctr() const { return irq_ctr_; }
  uint8_t halt_cycles() const { return halt_cycles_; }
  uint8_t wake_ctr() const { return wake_ctr_; }
  uint8_t spurious_wake_ctr() const { return spurious_wake_ctr_; }

 private:
  static void SatInc(uint8_t* value) {
    if (*value != 0xff) *value = static_cast<uint8_t>(*value + 1);
  }

  bool sync_bit(int index) const { return ((sync_ >> index) & 1u) != 0; }

  uint32_t meta_ = 0;
  uint32_t sync_ = 0;
  bool sw_msip_ = false;
  bool sw_stip_ = false;
  bool sw_mtip_ = false;
  bool halted_ = false;
  uint8_t irq_ctr_ = 0;
  uint8_t halt_cycles_ = 0;
  uint8_t wake_ctr_ = 0;
  uint8_t spurious_wake_ctr_ = 0;
};

// ------------------------------------------------------------------ the harness
//
// Owns the clock, the reset schedule, the comparison against the shadow, the
// standing invariants and the coverage counters. The measured quantities the
// phases assert on are the DUT's own outputs -- a phase assertion that compared
// shadow against shadow would pass no matter what the hardware did.
class Harness {
 public:
  Harness(Vmosaic_interrupt_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }
  void BindShadow(ShadowInterrupt* shadow) { shadow_ = shadow; }

  // Assert reset for `cycles` rising edges with the given idle stimulus, then
  // reset the shadow: a reset that touched only the DUT would leave the two
  // models describing different machines.
  void Reset(int cycles) {
    Stim idle;
    idle.core_can_trap = true;
    for (int i = 0; i < cycles; i++) Cycle(idle, /*rst=*/true);
    if (shadow_ != nullptr) shadow_->Reset();
  }

  Outputs Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the campaign finished");
    }
    Drive(s, rst);
    dut_->eval();
    pre_ = ReadNow();

    if (!rst) {
      CompareComb(s, pre_, "pre-edge");
      shadow_->Apply(s);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
    post_ = ReadNow();

    if (!rst) {
      CheckSpuriousWakeCounter();
      CompareComb(s, post_, "post-edge");
      CompareState();
      CheckInvariants(s);
      CountCoverage(s);
    }
    return post_;
  }

  // One clock period in which a source is pulsed high and low *inside the low
  // phase*, so that no rising edge ever sees it. This is the discrete model of a
  // sub-cycle glitch: the synchroniser has had no opportunity to sample it, and
  // the pending bit must not move. The DUT is checked while the pulse is
  // actually asserted, which is the moment a missing synchroniser becomes
  // visible combinationally.
  Outputs Glitch(const Stim& s, uint32_t bits) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles exhausted before the campaign finished");
    }
    Stim pulsed = s;
    pulsed.irq_soft = pulsed.irq_soft || ((bits & 1u) != 0);
    pulsed.irq_timer = pulsed.irq_timer || ((bits & 2u) != 0);
    pulsed.irq_ext = pulsed.irq_ext || ((bits & 4u) != 0);

    Drive(pulsed, false);
    dut_->eval();
    pre_ = ReadNow();
    CompareComb(s, pre_, "mid-cycle glitch");
    ++cov_glitches;

    // Drop the pulse and complete the cycle with the source as it was, so the
    // edge is taken with the glitch gone.
    Drive(s, false);
    dut_->eval();
    shadow_->Apply(s);

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
    post_ = ReadNow();

    CheckSpuriousWakeCounter();
    CompareComb(s, post_, "post-edge");
    CompareState();
    CheckInvariants(s);
    CountCoverage(s);
    return post_;
  }

  // Snapshots. `pre` is the cycle under test (combinational offers and the
  // register contents that produced them); `post` is after the edge.
  const Outputs& pre() const { return pre_; }
  const Outputs& post() const { return post_; }

  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }

  // Coverage counters, asserted by main so that a phase which silently stopped
  // exercising its mechanism is not reported as a pass.
  uint64_t cov_irq_cycles = 0;
  uint64_t cov_cause_mei = 0;
  uint64_t cov_cause_msi = 0;
  uint64_t cov_cause_mti = 0;
  uint64_t cov_masked_cycles = 0;
  uint64_t cov_boundary_low = 0;
  uint64_t cov_halt_entries = 0;
  uint64_t cov_wakes = 0;
  uint64_t cov_glitches = 0;
  uint64_t cov_sw_writes = 0;
  uint64_t cov_halt_while_disabled_pending = 0;

 private:
  void Drive(const Stim& s, bool rst) {
    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->irq_soft = s.irq_soft ? 1 : 0;
    dut_->irq_timer = s.irq_timer ? 1 : 0;
    dut_->irq_ext = s.irq_ext ? 1 : 0;
    dut_->mie = s.mie;
    dut_->mideleg = s.mideleg;
    dut_->mstatus_mie = s.mstatus_mie ? 1 : 0;
    // The shadow models a hart executing in M-mode: a non-delegated interrupt is
    // gated by mstatus.MIE and a delegated one is not taken at all, which is the
    // module's decision for priv=M. Driving priv=M (and SIE=0, unused from M)
    // keeps the model an honest statement of the machine under test instead of an
    // accident of an undriven input.
    dut_->priv = 3;
    dut_->mstatus_sie = 0;
    dut_->mip_we = s.mip_we ? 1 : 0;
    dut_->mip_op = s.mip_op;
    dut_->mip_wdata = s.mip_wdata;
    dut_->core_can_trap = s.core_can_trap ? 1 : 0;
    dut_->wfi_valid = s.wfi_valid ? 1 : 0;
  }

  void ReadInto(Outputs* o) const {
    o->mip = dut_->mip;
    o->irq_valid = dut_->irq_valid != 0;
    o->irq_cause = dut_->irq_cause;
    o->irq_timer_pending = dut_->irq_timer_pending != 0;
    o->irq_soft_pending = dut_->irq_soft_pending != 0;
    o->irq_ext_pending = dut_->irq_ext_pending != 0;
    o->halt = dut_->wfi_halt != 0;
    o->irq_ctr = static_cast<uint8_t>(dut_->o_irq_ctr);
    o->halt_cycles = static_cast<uint8_t>(dut_->o_halt_cycles);
    o->wake_ctr = static_cast<uint8_t>(dut_->o_wake_ctr);
    o->spurious_wake_ctr = static_cast<uint8_t>(dut_->o_spurious_wake_ctr);
  }

  Outputs ReadNow() const {
    Outputs o;
    ReadInto(&o);
    return o;
  }

  // The module's own spurious-wake counter, checked before anything else in the
  // cycle. It is a *detector*, not a statistic that is compared to the shadow
  // and then never used: a halt that ends without an enabled pending interrupt
  // is exactly the classic WFI bug, and this counter is the DUT's own statement
  // that it happened. Checking it first means the first failure of that class
  // names the counter rather than the halt output it also disturbed.
  void CheckSpuriousWakeCounter() const {
    Require(post_.spurious_wake_ctr == 0,
            phase_ + ": cycle " + Dec(clk_->cycle()) + " o_spurious_wake_ctr",
            "a halt ended without an enabled pending interrupt (DUT count " +
                Dec(post_.spurious_wake_ctr) + ")");
  }

  void CompareComb(const Stim& s, const Outputs& actual, const std::string& snapshot) {
    Outputs expected;
    shadow_->Comb(s, &expected);
    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle()) + " " + snapshot;
    Require(actual.mip == expected.mip, where + " mip",
            "expected " + mosaic::Hex(expected.mip) + ", got " + mosaic::Hex(actual.mip));
    Require(actual.irq_valid == expected.irq_valid, where + " irq_valid",
            "expected " + Bool(expected.irq_valid) + ", got " + Bool(actual.irq_valid));
    Require(actual.irq_cause == expected.irq_cause, where + " irq_cause",
            "expected " + mosaic::Hex(expected.irq_cause) + ", got " +
                mosaic::Hex(actual.irq_cause));
    Require(actual.irq_timer_pending == expected.irq_timer_pending,
            where + " irq_timer_pending",
            "expected " + Bool(expected.irq_timer_pending) + ", got " +
                Bool(actual.irq_timer_pending));
    Require(actual.irq_soft_pending == expected.irq_soft_pending, where + " o_irq_soft_pending",
            "expected " + Bool(expected.irq_soft_pending) + ", got " +
                Bool(actual.irq_soft_pending));
    Require(actual.irq_ext_pending == expected.irq_ext_pending, where + " o_irq_ext_pending",
            "expected " + Bool(expected.irq_ext_pending) + ", got " +
                Bool(actual.irq_ext_pending));
    Require(actual.halt == expected.halt, where + " wfi_halt",
            "expected " + Bool(expected.halt) + ", got " + Bool(actual.halt));
    ++comparisons_;
  }

  void CompareState() {
    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle()) + " post-edge";
    Require(post_.halt == shadow_->halted(), where + " wfi_halt",
            "expected " + Bool(shadow_->halted()) + ", got " + Bool(post_.halt));
    Require(post_.irq_ctr == shadow_->irq_ctr(), where + " o_irq_ctr",
            "expected " + Dec(shadow_->irq_ctr()) + ", got " + Dec(post_.irq_ctr));
    Require(post_.halt_cycles == shadow_->halt_cycles(), where + " o_halt_cycles",
            "expected " + Dec(shadow_->halt_cycles()) + ", got " + Dec(post_.halt_cycles));
    Require(post_.wake_ctr == shadow_->wake_ctr(), where + " o_wake_ctr",
            "expected " + Dec(shadow_->wake_ctr()) + ", got " + Dec(post_.wake_ctr));
    Require(post_.spurious_wake_ctr == shadow_->spurious_wake_ctr(),
            where + " o_spurious_wake_ctr",
            "expected " + Dec(shadow_->spurious_wake_ctr()) + ", got " +
                Dec(post_.spurious_wake_ctr));
    ++comparisons_;
  }

  void CheckInvariants(const Stim& s) {
    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle());
    // The boundary. This is the card's "irq_valid_o never rises outside
    // core_can_trap_i" as a check on the DUT's own output, not as a comment.
    Require(!pre_.irq_valid || s.core_can_trap,
            where + " boundary: irq_valid_o outside core_can_trap_i",
            "irq_valid_o=1 while core_can_trap_i=0");
    Require(!pre_.irq_valid || s.mstatus_mie, where + " boundary: irq_valid_o with mstatus.MIE=0",
            "irq_valid_o=1 while mstatus_mie_i=0");
    const uint64_t take = pre_.mip & s.mie & ~s.mideleg;
    Require(!pre_.irq_valid || take != 0, where + " boundary: irq_valid_o without a cause",
            "irq_valid_o=1 with no enabled, non-delegated pending bit");
    // A halt is never entered while an enabled interrupt is already pending.
    Require(!(post_.halt && !pre_.halt && (pre_.mip & s.mie) != 0),
            where + " WFI: halted with an enabled interrupt already pending",
            "wfi_halt_o rose while mie & mip was non-zero");
  }

  void CountCoverage(const Stim& s) {
    if (pre_.irq_valid) {
      ++cov_irq_cycles;
      if (pre_.irq_cause == kCauseMei) ++cov_cause_mei;
      if (pre_.irq_cause == kCauseMsi) ++cov_cause_msi;
      if (pre_.irq_cause == kCauseMti) ++cov_cause_mti;
    }
    if ((pre_.mip & s.mie) != 0 && !pre_.irq_valid) ++cov_masked_cycles;
    if (!s.core_can_trap) ++cov_boundary_low;
    if (post_.halt && !pre_.halt) ++cov_halt_entries;
    if (post_.wake_ctr != pre_.wake_ctr) ++cov_wakes;
    if (s.mip_we) ++cov_sw_writes;
    if (pre_.halt && (pre_.mip & s.mie) == 0 && pre_.mip != 0) ++cov_halt_while_disabled_pending;
  }

  Vmosaic_interrupt_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  ShadowInterrupt* shadow_ = nullptr;
  std::string phase_ = "startup";
  Outputs pre_;
  Outputs post_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
};

// ------------------------------------------------------------------ stimulus
Stim Base() {
  Stim s;
  s.core_can_trap = true;
  return s;
}

// Settle the two-flop synchroniser for one source change: two edges for the
// value to reach the second flop, plus one cycle for the combinational outputs
// to reflect it.
void Settle(Harness* h, const Stim& s, int cycles = 3) {
  for (int i = 0; i < cycles; i++) h->Cycle(s);
}

// -------------------------------------------------------------------- phases
void PhaseResetState(Harness* h) {
  h->Reset(4);
  Stim s = Base();
  for (int i = 0; i < 3; i++) h->Cycle(s);
  Require(h->post().mip == 0, "reset-state: mip after reset",
          "expected 0, got " + mosaic::Hex(h->post().mip));
  Require(!h->post().irq_valid, "reset-state: irq_valid after reset", "expected 0, got 1");
  Require(!h->post().halt, "reset-state: wfi_halt after reset", "expected 0, got 1");
  Require(h->post().irq_ctr == 0 && h->post().halt_cycles == 0 &&
              h->post().wake_ctr == 0 && h->post().spurious_wake_ctr == 0,
          "reset-state: counters after reset", "a counter was non-zero");
  // A source asserted *during* reset must not leave anything behind: the
  // synchroniser is control state and is cleared.
  Stim during = Base();
  during.irq_soft = true;
  during.irq_timer = true;
  during.irq_ext = true;
  for (int i = 0; i < 2; i++) h->Cycle(during, /*rst=*/true);
  h->Cycle(Base());
  Require(h->post().mip == 0, "reset-state: mip after a source held through reset",
          "expected 0 immediately after reset release, got " + mosaic::Hex(h->post().mip));
}

void PhaseSyncAndGlitch(Harness* h) {
  h->Reset(4);
  Stim s = Base();
  Settle(h, s);

  // 1. A sub-cycle pulse on each source: never observed, no counter moves.
  for (uint32_t bit = 1; bit <= 4; bit <<= 1) {
    const Outputs before = h->post();
    h->Glitch(s, bit);
    Require(h->post().mip == before.mip,
            "sync-and-glitch: sub-cycle glitch changed mip",
            "pulse bit " + Dec(bit) + " produced mip " + mosaic::Hex(h->post().mip));
    Require(h->post().irq_ctr == before.irq_ctr, "sync-and-glitch: glitch offered a trap",
            "o_irq_ctr moved on a sub-cycle pulse");
    Settle(h, s);
    Require(h->post().mip == 0, "sync-and-glitch: glitch latched a pending bit",
            "mip is " + mosaic::Hex(h->post().mip) + " after settling");
  }

  // 2. A pulse that spans exactly one rising edge is visible for exactly one
  //    cycle, two edges after it was presented. The card asks for the model to
  //    be stated; this is the model, asserted by cycle rather than by "eventually".
  {
    const uint64_t driven = h->cycles();
    Stim p = Base();
    p.irq_timer = true;
    h->Cycle(p);  // edge `driven + 1` samples the pulse into the first flop
    p.irq_timer = false;
    h->Cycle(p);  // edge `driven + 2` moves it to the second flop; still not
                  // visible to the decision logic before that edge
    Require(!h->pre().irq_timer_pending,
            "sync-and-glitch: a one-cycle pulse was visible too early",
            "post-edge cycle " + Dec(h->cycles() - 1) + " already showed MTIP");
    h->Cycle(p);
    Require(h->cycles() == driven + 3, "sync-and-glitch: cycle accounting",
            "unexpected cycle count");
    Require(h->pre().irq_timer_pending && h->pre().mip == kTimerPendingBits,
            "sync-and-glitch: a one-cycle pulse was not visible after two edges",
            "pre-edge cycle " + Dec(h->cycles()) + " mip=" + mosaic::Hex(h->pre().mip));
    // Exactly one cycle: the pre-edge snapshot of the next cycle is already
    // clear again, because the pulse itself was dropped.
    h->Cycle(p);
    Require(h->pre().mip == 0, "sync-and-glitch: a one-cycle pulse latched",
            "pre-edge cycle " + Dec(h->cycles()) + " mip=" + mosaic::Hex(h->pre().mip));
  }

  // 3. A held level stays pending, and clears exactly two edges after it drops.
  {
    Stim lvl = Base();
    lvl.irq_soft = true;
    lvl.irq_ext = true;
    Settle(h, lvl);
    Require(h->pre().mip == ((1ull << kBitMsip) | (1ull << kBitMeip)),
            "sync-and-glitch: held levels not pending",
            "mip=" + mosaic::Hex(h->pre().mip));
    lvl.irq_soft = false;
    lvl.irq_ext = false;
    h->Cycle(lvl);
    Require(h->pre().mip == ((1ull << kBitMsip) | (1ull << kBitMeip)),
            "sync-and-glitch: a level cleared too early",
            "mip=" + mosaic::Hex(h->pre().mip) + " on the first cycle after deassert");
    h->Cycle(lvl);
    Require(h->pre().mip == ((1ull << kBitMsip) | (1ull << kBitMeip)),
            "sync-and-glitch: a level cleared one edge too early",
            "mip=" + mosaic::Hex(h->pre().mip) + " on the second cycle after deassert");
    h->Cycle(lvl);
    Require(h->pre().mip == 0, "sync-and-glitch: a level never cleared",
            "mip=" + mosaic::Hex(h->pre().mip) + " on the third cycle after deassert");
  }

  // 4. The three sources are independent: each one alone produces its own bit.
  for (uint32_t bit = 1; bit <= 4; bit <<= 1) {
    Stim one = Base();
    one.irq_soft = (bit & 1u) != 0;
    one.irq_timer = (bit & 2u) != 0;
    one.irq_ext = (bit & 4u) != 0;
    Settle(h, one, 4);
    const uint64_t expected = (one.irq_soft ? (1ull << kBitMsip) : 0) |
                              (one.irq_timer ? kTimerPendingBits : 0) |
                              (one.irq_ext ? (1ull << kBitMeip) : 0);
    Require(h->pre().mip == expected, "sync-and-glitch: source bit " + Dec(bit),
            "expected " + mosaic::Hex(expected) + ", got " + mosaic::Hex(h->pre().mip));
    Require(h->pre().irq_soft_pending == one.irq_soft &&
                h->pre().irq_timer_pending == one.irq_timer &&
                h->pre().irq_ext_pending == one.irq_ext,
            "sync-and-glitch: pending flag for source bit " + Dec(bit),
            "the pending flags do not mirror mip");
  }
}

void PhaseMasking(Harness* h) {
  h->Reset(4);
  Stim s = Base();
  s.mie = kAllMie;
  s.mstatus_mie = true;
  s.irq_soft = true;
  s.irq_timer = true;
  s.irq_ext = true;
  Settle(h, s, 4);
  Require(h->pre().mip == (kAllMie | kStipBit), "masking: setup",
          "not all three sources are pending");

  // All eight mie combinations, all three sources pending.
  for (uint32_t mask = 0; mask < 8; mask++) {
    Stim m = s;
    m.mie = ((mask & 1u) ? (1ull << kBitMsip) : 0ull) |
            ((mask & 2u) ? (1ull << kBitMtip) : 0ull) |
            ((mask & 4u) ? (1ull << kBitMeip) : 0ull);
    h->Cycle(m);
    Require(h->pre().irq_valid == (m.mie != 0), "masking: mie=" + Dec(mask),
            "expected irq_valid " + Bool(m.mie != 0) + ", got " + Bool(h->pre().irq_valid));
    uint64_t expected_cause = 0;
    if (((m.mie >> kBitMeip) & 1) != 0) {
      expected_cause = kCauseMei;
    } else if (((m.mie >> kBitMsip) & 1) != 0) {
      expected_cause = kCauseMsi;
    } else if (((m.mie >> kBitMtip) & 1) != 0) {
      expected_cause = kCauseMti;
    }
    Require(h->pre().irq_cause == expected_cause, "masking: cause for mie=" + Dec(mask),
            "expected " + mosaic::Hex(expected_cause) + ", got " +
                mosaic::Hex(h->pre().irq_cause));
  }

  // All eight source combinations, everything enabled: the priority order.
  for (uint32_t combo = 0; combo < 8; combo++) {
    Stim c = Base();
    c.mie = kAllMie;
    c.mstatus_mie = true;
    c.irq_soft = (combo & 1u) != 0;
    c.irq_timer = (combo & 2u) != 0;
    c.irq_ext = (combo & 4u) != 0;
    Settle(h, c, 4);
    const bool want_ext = (combo & 4u) != 0;
    const bool want_soft = !want_ext && (combo & 1u) != 0;
    const bool want_timer = !want_ext && !want_soft && (combo & 2u) != 0;
    const bool want_any = want_ext || want_soft || want_timer;
    const uint64_t want_cause =
        want_ext ? kCauseMei : (want_soft ? kCauseMsi : (want_timer ? kCauseMti : 0ull));
    Require(h->pre().irq_valid == want_any, "masking: source combo " + Dec(combo),
            "expected irq_valid " + Bool(want_any) + ", got " + Bool(h->pre().irq_valid));
    Require(h->pre().irq_cause == want_cause, "masking: priority for source combo " + Dec(combo),
            "expected " + mosaic::Hex(want_cause) + " (MEI > MSI > MTI), got " +
                mosaic::Hex(h->pre().irq_cause));
  }

  // mstatus.MIE masks everything, and un-masking brings the same pending set
  // back with the same cause.
  {
    Stim m = Base();
    m.mie = kAllMie;
    m.irq_timer = true;
    Settle(h, m, 3);
    m.mstatus_mie = false;
    h->Cycle(m);
    Require(!h->pre().irq_valid, "masking: mstatus.MIE=0",
            "a trap was offered while the global enable was clear");
    // The cause is a candidate report, not an offer: it still names the winner
    // among the enabled, non-delegated pending bits while the global enable
    // blocks the trap, and it is the same cause the un-masked cycle uses below.
    Require(h->pre().irq_cause == kCauseMti, "masking: cause with mstatus.MIE=0",
            "expected the masked candidate " + mosaic::Hex(kCauseMti) + ", got " +
                mosaic::Hex(h->pre().irq_cause));
    Require(h->pre().mip == kTimerPendingBits, "masking: pending while masked",
            "the pending bit was dropped by masking, not delayed");
    h->Cycle(m);
    Require(!h->pre().irq_valid, "masking: mstatus.MIE=0 holds",
            "a trap was offered on the second masked cycle");
    m.mstatus_mie = true;
    h->Cycle(m);
    Require(h->pre().irq_valid && h->pre().irq_cause == kCauseMti,
            "masking: un-masking", "the trap did not reappear with the same cause");
    m.irq_timer = false;
    Settle(h, m, 4);
  }

  // The delegation gate: implemented, not assumed. A delegated interrupt is
  // still pending and still enabled -- it is simply not this module's trap.
  {
    Stim d = Base();
    d.mie = (1ull << kBitMeip);
    d.irq_ext = true;
    d.mstatus_mie = true;
    Settle(h, d, 4);
    Require(h->pre().irq_valid, "masking: undelegated MEI", "the trap was not offered");
    d.mideleg = (1ull << kBitMeip);
    h->Cycle(d);
    Require(!h->pre().irq_valid, "masking: delegated MEI",
            "a delegated interrupt was taken by the M-mode decision");
    Require(h->pre().irq_ext_pending, "masking: delegated MEI still pending",
            "mideleg must not change the pending bit");
    // A lower-priority undelegated bit must win while the higher one is
    // delegated: this is a gate on the taken condition, not a global mask.
    d.irq_timer = true;
    d.mie = (1ull << kBitMeip) | (1ull << kBitMtip);
    Settle(h, d, 4);
    Require(h->pre().irq_valid && h->pre().irq_cause == kCauseMti,
            "masking: delegated MEI with pending MTI",
            "expected the undelegated MTI to be taken, got cause " +
                mosaic::Hex(h->pre().irq_cause));
    d.mideleg = 0;
    h->Cycle(d);
    Require(h->pre().irq_cause == kCauseMei, "masking: undelegating MEI",
            "MEI did not win once it was undelegated");
    d.irq_timer = false;
    d.irq_ext = false;
    Settle(h, d, 4);
  }
}

void PhaseBoundaryLatency(Harness* h, mosaic::Rng* rng) {
  h->Reset(4);
  for (int round = 0; round < 10; round++) {
    Stim s = Base();
    s.mie = kAllMie;
    s.mstatus_mie = true;
    s.irq_timer = true;
    Settle(h, s, 4);
    Require(h->pre().irq_valid, "boundary-latency: setup", "the trap was not offered");

    // Hold the boundary low for a randomised interval. This models the outside
    // world -- a divide that has not finished, a macro that has not retired --
    // without pretending this module knows anything about it.
    const uint32_t low = 1 + rng->Below(64);
    s.core_can_trap = false;
    for (uint32_t i = 0; i < low; i++) {
      // A second source arrives while the boundary is blocked, on some rounds.
      if (round % 3 == 1 && i == low / 2) s.irq_soft = true;
      if (round % 3 == 2 && i == low / 2) s.mstatus_mie = false;
      h->Cycle(s);
      Require(!h->pre().irq_valid, "boundary-latency: offer inside the blocked window",
              "irq_valid_o rose while core_can_trap_i was low (round " + Dec(round) +
                  ", cycle " + Dec(i) + ")");
      Require(h->pre().irq_timer_pending, "boundary-latency: timer pending lost while blocked",
              "the timer pending bit was dropped during the blocked window");
    }
    const uint8_t irq_ctr_blocked = h->post().irq_ctr;

    // The interrupt is still there when the boundary opens, and the offer is a
    // function of the state and the inputs only -- not of how long we waited.
    s.core_can_trap = true;
    s.mstatus_mie = true;
    h->Cycle(s);
    Require(h->pre().irq_valid, "boundary-latency: no offer at the first legal boundary",
            "core_can_trap_i rose and nothing was offered (round " + Dec(round) + ")");
    const uint64_t want_cause = (h->pre().mip & (1ull << kBitMeip)) != 0 ? kCauseMei
                                : (h->pre().mip & (1ull << kBitMsip)) != 0 ? kCauseMsi
                                                                          : kCauseMti;
    Require(h->pre().irq_cause == want_cause, "boundary-latency: cause at the boundary",
            "expected " + mosaic::Hex(want_cause) + ", got " + mosaic::Hex(h->pre().irq_cause));
    Require(h->post().irq_ctr == static_cast<uint8_t>(irq_ctr_blocked + 1),
            "boundary-latency: offer count at the boundary",
            "expected exactly one new offer, o_irq_ctr went " + Dec(irq_ctr_blocked) + " -> " +
                Dec(h->post().irq_ctr));

    // Drop everything and let the synchroniser drain before the next round.
    s.irq_timer = false;
    s.irq_soft = false;
    s.irq_ext = false;
    Settle(h, s, 5);
    Require(h->pre().mip == 0, "boundary-latency: drain", "a pending bit survived the drain");
  }
}

void PhaseHaltResume(Harness* h) {
  h->Reset(4);

  // 1. Nothing pending: WFI halts, the halt is counted, and a late enabled
  //    source wakes it exactly once.
  {
    Stim s = Base();
    s.mie = (1ull << kBitMtip);
    s.mstatus_mie = true;
    Settle(h, s, 3);
    const uint8_t halt_cycles_before = h->post().halt_cycles;
    const uint8_t wakes_before = h->post().wake_ctr;

    s.wfi_valid = true;
    h->Cycle(s);
    Require(h->post().halt, "halt-resume: WFI did not halt", "wfi_halt_o stayed low");
    s.wfi_valid = false;

    for (int i = 0; i < 5; i++) {
      h->Cycle(s);
      Require(h->post().halt, "halt-resume: halt ended without a wake event",
              "wfi_halt_o dropped while nothing was pending");
    }
    Require(h->post().halt_cycles == static_cast<uint8_t>(halt_cycles_before + 5),
            "halt-resume: o_halt_cycles",
            "expected exactly 5 counted cycles, went " + Dec(halt_cycles_before) + " -> " +
                Dec(h->post().halt_cycles));

    // The late source: two edges to become visible, then the wake.
    s.irq_timer = true;
    h->Cycle(s);
    Require(h->post().halt, "halt-resume: woke before the source was synchronised",
            "the halt ended in the same cycle the source was asserted");
    h->Cycle(s);
    Require(h->post().halt, "halt-resume: woke one edge too early",
            "the halt ended before the synchroniser had produced the pending bit");
    h->Cycle(s);
    Require(!h->post().halt, "halt-resume: the late source did not wake the core",
            "wfi_halt_o is still high after the pending bit became visible");
    Require(h->post().wake_ctr == static_cast<uint8_t>(wakes_before + 1),
            "halt-resume: o_wake_ctr", "expected exactly one wake, got " +
                                            Dec(h->post().wake_ctr));
    for (int i = 0; i < 3; i++) {
      h->Cycle(s);
      Require(h->post().wake_ctr == static_cast<uint8_t>(wakes_before + 1),
              "halt-resume: a second wake", "the wake was counted more than once");
      Require(!h->post().halt, "halt-resume: the halt came back", "wfi_halt_o rose again");
    }
    Require(h->pre().irq_valid && h->pre().irq_cause == kCauseMti,
            "halt-resume: the trap after the wake",
            "an enabled pending interrupt did not produce a trap after the wake");
    s.irq_timer = false;
    Settle(h, s, 4);
  }

  // 2. An enabled interrupt is already pending when WFI arrives: it must not
  //    halt at all.
  {
    Stim s = Base();
    s.mie = (1ull << kBitMtip);
    s.mstatus_mie = true;
    s.irq_timer = true;
    Settle(h, s, 4);
    Require(h->pre().irq_valid, "halt-resume: setup 2", "no pending interrupt to test with");
    const uint8_t halt_cycles_before = h->post().halt_cycles;
    const uint8_t wakes_before = h->post().wake_ctr;
    s.wfi_valid = true;
    h->Cycle(s);
    Require(!h->post().halt, "halt-resume: halted with an enabled interrupt already pending",
            "wfi_halt_o rose although the interrupt should have completed the WFI");
    Require(h->post().halt_cycles == halt_cycles_before,
            "halt-resume: a halt cycle was counted for a WFI that did not halt",
            "o_halt_cycles moved");
    Require(h->post().wake_ctr == wakes_before,
            "halt-resume: a wake was counted for a halt that never happened",
            "o_wake_ctr moved");
    s.wfi_valid = false;
    Settle(h, s, 3);
  }

  // 3. The classic bug: a pending but *disabled* interrupt must not wake the
  //    core. The core halts, the source becomes pending while mie enables a
  //    different interrupt, and it stays halted until the timer's own enable
  //    arrives.
  {
    Stim s = Base();
    s.mie = (1ull << kBitMsip);  // enables software, not the timer
    s.mstatus_mie = true;
    Settle(h, s, 3);
    s.wfi_valid = true;
    h->Cycle(s);
    Require(h->post().halt, "halt-resume: setup 3", "the core did not halt");
    s.wfi_valid = false;

    s.irq_timer = true;  // pending, but mie does not enable it
    const uint8_t wakes_before = h->post().wake_ctr;
    for (int i = 0; i < 6; i++) {
      h->Cycle(s);
      Require(h->post().halt, "halt-resume: woken by a disabled interrupt",
              "wfi_halt_o dropped with mie & mip == 0 (cycle " + Dec(i) + ")");
    }
    Require(h->pre().irq_timer_pending, "halt-resume: disabled source was not pending",
            "the source did not become pending, so the test proves nothing");
    Require(h->post().spurious_wake_ctr == 0, "halt-resume: o_spurious_wake_ctr",
            "the spurious wake counter moved with mip & mie == 0");
    Require(h->post().wake_ctr == wakes_before, "halt-resume: a spurious wake was counted",
            "o_wake_ctr moved while the core should have stayed halted");

    // Enabling the identical source now wakes it, exactly once.
    s.mie = (1ull << kBitMtip);
    h->Cycle(s);
    Require(!h->post().halt, "halt-resume: enabling a pending interrupt did not wake the core",
            "wfi_halt_o stayed high after mie enabled the pending bit");
    Require(h->post().wake_ctr == static_cast<uint8_t>(wakes_before + 1),
            "halt-resume: enable-time wake count", "expected exactly one wake");
    Require(h->post().spurious_wake_ctr == 0, "halt-resume: o_spurious_wake_ctr after enabling",
            "a legal wake was counted as spurious");
    s.irq_timer = false;
    s.mie = 0;
    Settle(h, s, 4);
  }

  // 4. mstatus.MIE=0 does not stop the wake: the WFI rule uses mie & mip only.
  {
    Stim s = Base();
    s.mie = (1ull << kBitMtip);
    s.mstatus_mie = false;
    Settle(h, s, 3);
    s.wfi_valid = true;
    h->Cycle(s);
    Require(h->post().halt, "halt-resume: WFI with mstatus.MIE=0 did not halt",
            "wfi_halt_o stayed low");
    s.wfi_valid = false;
    h->Cycle(s);
    Require(!h->pre().irq_valid, "halt-resume: trap offered with mstatus.MIE=0",
            "irq_valid_o rose while the global enable was clear");
    s.irq_timer = true;
    const uint8_t wakes_before = h->post().wake_ctr;
    Settle(h, s, 3);
    Require(!h->post().halt, "halt-resume: mstatus.MIE=0 blocked the wake",
            "the halt persisted although an enabled interrupt became pending");
    Require(h->post().wake_ctr == static_cast<uint8_t>(wakes_before + 1),
            "halt-resume: wake count with mstatus.MIE=0", "expected exactly one wake");
    Require(!h->pre().irq_valid, "halt-resume: trap with mstatus.MIE=0 after waking",
            "the trap was offered although mstatus.MIE was clear");
    s.irq_timer = false;
    Settle(h, s, 4);
  }

  // 5. Repeated WFI pulses while halted are not extra wakes, and a new WFI after
  //    a completed one halts again.
  {
    Stim s = Base();
    s.mie = 0;
    s.mstatus_mie = true;
    Settle(h, s, 3);
    s.wfi_valid = true;
    h->Cycle(s);
    Require(h->post().halt, "halt-resume: setup 5", "the core did not halt");
    const uint8_t wakes_before = h->post().wake_ctr;
    for (int i = 0; i < 3; i++) h->Cycle(s);  // WFI held asserted while halted
    Require(h->post().halt, "halt-resume: repeated WFI dropped the halt", "wfi_halt_o went low");
    Require(h->post().wake_ctr == wakes_before, "halt-resume: repeated WFI counted a wake",
            "o_wake_ctr moved");
    s.wfi_valid = false;
    h->Cycle(s);
    Require(h->post().halt, "halt-resume: the halt ended with nothing pending",
            "wfi_halt_o dropped with mie & mip == 0");
  }
}

void PhaseMipSoftwareWrite(Harness* h) {
  h->Reset(4);

  // 1. Software raises MTIP with the platform timer idle, and the trap follows.
  {
    Stim s = Base();
    s.mie = (1ull << kBitMtip);
    s.mstatus_mie = true;
    Settle(h, s, 3);
    Require(!h->pre().irq_timer_pending, "mip-software-write: setup",
            "the platform timer was already pending");
    s.mip_we = true;
    s.mip_op = kCsrRw;
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    Require(h->post().mip == (1ull << kBitMtip), "mip-software-write: CSR_RW set bit 7",
            "mip=" + mosaic::Hex(h->post().mip));
    s.mip_we = false;
    s.mip_wdata = 0;
    h->Cycle(s);
    Require(h->pre().irq_timer_pending, "mip-software-write: pending flag",
            "o_irq_timer_pending did not follow the software latch");
    Require(h->pre().irq_valid && h->pre().irq_cause == kCauseMti,
            "mip-software-write: the software-raised interrupt was not taken",
            "irq_valid=" + Bool(h->pre().irq_valid) + " cause=" + mosaic::Hex(h->pre().irq_cause));

    // CSR_RC clears it, and the trap goes away.
    s.mip_we = true;
    s.mip_op = kCsrRc;
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    s.mip_we = false;
    s.mip_wdata = 0;
    h->Cycle(s);
    Require(h->post().mip == 0, "mip-software-write: CSR_RC cleared bit 7",
            "mip=" + mosaic::Hex(h->post().mip));
    Require(!h->pre().irq_valid, "mip-software-write: trap after clearing",
            "the trap was still offered after mip[7] was cleared");
    Require(h->pre().irq_cause == 0, "mip-software-write: cause after clearing",
            "irq_cause_o was non-zero with nothing pending");
  }

  // 2. CSR_RS sets, CSR_RW with zero clears, and both writable bits are
  //    independent.
  {
    Stim s = Base();
    Settle(h, s, 2);
    s.mip_we = true;
    s.mip_op = kCsrRs;
    s.mip_wdata = (1ull << kBitMsip);
    h->Cycle(s);
    Require(h->post().mip == (1ull << kBitMsip), "mip-software-write: CSR_RS set bit 3",
            "mip=" + mosaic::Hex(h->post().mip));
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    Require(h->post().mip == ((1ull << kBitMsip) | (1ull << kBitMtip)),
            "mip-software-write: CSR_RS set bit 7 as well",
            "mip=" + mosaic::Hex(h->post().mip));
    s.mip_op = kCsrRw;
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    Require(h->post().mip == (1ull << kBitMtip), "mip-software-write: CSR_RW replaced the latch",
            "mip=" + mosaic::Hex(h->post().mip));
    s.mip_wdata = 0;
    h->Cycle(s);
    Require(h->post().mip == 0, "mip-software-write: CSR_RW with zero cleared the latch",
            "mip=" + mosaic::Hex(h->post().mip));
    s.mip_we = false;
    s.mip_op = kCsrNone;
    Settle(h, s, 2);
  }

  // 3. A write to a read-only bit is ignored, mip_we_i=0 writes nothing, and
  //    CSR_NONE with mip_we_i asserted writes nothing.
  {
    Stim s = Base();
    Settle(h, s, 2);
    s.mip_we = true;
    s.mip_op = kCsrRs;
    // Bits 11 and 63 are read-only in every profile; bit 5 is read-only only
    // where the profile has no supervisor timer, so it is included only when it
    // really is read-only.
    s.mip_wdata = (1ull << kBitMeip) | (1ull << 63) | (kHasStip ? 0ull : (1ull << 5));
    h->Cycle(s);
    Require(h->post().mip == 0, "mip-software-write: write to a read-only bit",
            "mip=" + mosaic::Hex(h->post().mip) + " after writing bits 11, 5 and 63");
    s.mip_op = kCsrNone;
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    Require(h->post().mip == 0, "mip-software-write: CSR_NONE with mip_we_i",
            "mip=" + mosaic::Hex(h->post().mip) + " after a CSR_NONE write");
    s.mip_we = false;
    s.mip_op = kCsrRw;
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    Require(h->post().mip == 0, "mip-software-write: mip_we_i=0",
            "mip=" + mosaic::Hex(h->post().mip) + " after a write with mip_we_i low");
  }

  // 4. The software latch is ORed with the platform source: clearing mip cannot
  //    clear a platform request, and the platform request alone cannot be
  //    mistaken for a latch that software can drop.
  {
    Stim s = Base();
    s.mie = (1ull << kBitMtip);
    s.mstatus_mie = true;
    s.irq_timer = true;
    Settle(h, s, 4);
    Require(h->pre().irq_timer_pending, "mip-software-write: platform request",
            "the platform timer is not pending");
    s.mip_we = true;
    s.mip_op = kCsrRc;
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    s.mip_we = false;
    s.mip_wdata = 0;
    h->Cycle(s);
    Require(h->post().mip == kTimerPendingBits,
            "mip-software-write: a software clear dropped a platform request",
            "mip=" + mosaic::Hex(h->post().mip));
    Require(h->pre().irq_valid && h->pre().irq_cause == kCauseMti,
            "mip-software-write: the platform request stopped being taken",
            "the trap disappeared after a software clear of a platform-held bit");
    s.irq_timer = false;
    Settle(h, s, 4);
  }

  // 5. While the software latch holds MTIP, the core still halts only if the
  //    interrupt is disabled -- and then the enable wakes it. This closes the
  //    loop between the two mechanisms in this module.
  {
    Stim s = Base();
    s.mie = 0;
    s.mstatus_mie = true;
    Settle(h, s, 2);
    s.mip_we = true;
    s.mip_op = kCsrRw;
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    s.mip_we = false;
    s.mip_wdata = 0;
    s.wfi_valid = true;
    h->Cycle(s);
    Require(h->post().halt, "mip-software-write: WFI halted with a disabled pending latch",
            "the core did not halt");
    s.wfi_valid = false;
    const uint8_t wakes_before = h->post().wake_ctr;
    h->Cycle(s);
    Require(h->post().halt, "mip-software-write: a disabled latch woke the core",
            "wfi_halt_o dropped with mie & mip == 0");
    s.mie = (1ull << kBitMtip);
    h->Cycle(s);
    Require(!h->post().halt, "mip-software-write: enabling the latch did not wake the core",
            "wfi_halt_o stayed high");
    Require(h->post().wake_ctr == static_cast<uint8_t>(wakes_before + 1),
            "mip-software-write: wake count", "expected exactly one wake");
    s.mie = 0;
    s.mip_op = kCsrRc;
    s.mip_we = true;
    s.mip_wdata = (1ull << kBitMtip);
    h->Cycle(s);
    s.mip_we = false;
    s.mip_wdata = 0;
    Settle(h, s, 2);
  }
}

// ---------------------------------------------------------------- the replay
struct ReplayTrace {
  std::vector<std::pair<uint64_t, uint64_t>> traps;  // (cycle, cause) at 0 -> 1 of irq_valid
  std::vector<uint64_t> wakes;                       // cycles where o_wake_ctr moved
  std::vector<uint64_t> halts;                       // cycles where the halt was entered
  uint64_t hash = 0xcbf29ce484222325ull;
  uint64_t cycles_run = 0;
  uint8_t irq_ctr = 0;
  uint8_t halt_cycles = 0;
  uint8_t wake_ctr = 0;
  uint8_t spurious_wake_ctr = 0;
};

void HashMix(uint64_t* hash, uint64_t value) {
  // FNV-1a over the bytes of the value: a trace hash that is cheap and that
  // catches any single-cycle difference in the observable tuple.
  for (int i = 0; i < 8; i++) {
    *hash ^= (value >> (i * 8)) & 0xffull;
    *hash *= 0x100000001b3ull;
  }
}

// Build the scripted event timeline. Fixed given the seed, so "the same seed and
// the same events" is literally true: the same vector of stimuli is replayed.
std::vector<Stim> BuildScript(mosaic::Rng* rng, uint32_t cycles) {
  std::vector<Stim> script;
  Stim s;
  s.core_can_trap = true;
  s.mstatus_mie = true;
  s.mie = 0;
  static const uint64_t kMieChoices[4] = {
      0, (1ull << kBitMtip), (1ull << kBitMsip), kAllMie};
  static const uint64_t kMidelegChoices[3] = {0, (1ull << kBitMeip), (1ull << kBitMtip)};
  for (uint32_t i = 0; i < cycles; i++) {
    if (rng->Chance(10)) s.irq_soft = rng->Chance(50);
    if (rng->Chance(14)) s.irq_timer = rng->Chance(50);
    if (rng->Chance(8)) s.irq_ext = rng->Chance(50);
    if (rng->Chance(14)) s.mie = kMieChoices[rng->Below(4)];
    if (rng->Chance(8)) s.mideleg = kMidelegChoices[rng->Below(3)];
    if (rng->Chance(12)) s.mstatus_mie = rng->Chance(60);
    if (rng->Chance(20)) s.core_can_trap = rng->Chance(75);
    if (rng->Chance(6)) {
      s.mip_we = true;
      s.mip_op = kCsrRw + rng->Below(3);  // a real write op, never CSR_NONE
      s.mip_wdata = (rng->Chance(50) ? (1ull << kBitMtip) : 0ull) |
                    (rng->Chance(50) ? (1ull << kBitMsip) : 0ull) |
                    (rng->Chance(25) ? (1ull << kBitMeip) : 0ull);
    } else {
      s.mip_we = false;
    }
    if (rng->Chance(5)) s.wfi_valid = true;  // a one-cycle pulse
    script.push_back(s);
    s.wfi_valid = false;
  }
  return script;
}

ReplayTrace RunScript(Harness* h, ShadowInterrupt* shadow, const std::vector<Stim>& script) {
  h->Reset(4);
  shadow->Reset();
  h->BindShadow(shadow);
  ReplayTrace trace;
  // Cycle numbers are recorded *relative to the start of this run*. The two
  // replay runs happen at different absolute points in the campaign, and a
  // comparison of absolute cycles would fail for a reason that has nothing to
  // do with the hardware.
  const uint64_t base = h->cycles();
  bool irq_was_high = false;
  bool halt_was_high = false;
  uint8_t prev_wake = 0;
  for (const Stim& s : script) {
    h->Cycle(s);
    const Outputs& pre = h->pre();
    const Outputs& post = h->post();
    if (pre.irq_valid && !irq_was_high) {
      trace.traps.push_back(std::make_pair(h->cycles() - base, pre.irq_cause));
    }
    irq_was_high = pre.irq_valid;
    if (post.halt && !halt_was_high) trace.halts.push_back(h->cycles() - base);
    halt_was_high = post.halt;
    if (post.wake_ctr != prev_wake) trace.wakes.push_back(h->cycles() - base);
    prev_wake = post.wake_ctr;
    HashMix(&trace.hash, pre.mip);
    HashMix(&trace.hash, pre.irq_valid ? 1ull : 0ull);
    HashMix(&trace.hash, pre.irq_cause);
    HashMix(&trace.hash, post.halt ? 1ull : 0ull);
    HashMix(&trace.hash, post.irq_ctr);
    HashMix(&trace.hash, post.halt_cycles);
    HashMix(&trace.hash, post.wake_ctr);
    HashMix(&trace.hash, post.spurious_wake_ctr);
  }
  trace.cycles_run = h->cycles() - base;
  trace.irq_ctr = h->post().irq_ctr;
  trace.halt_cycles = h->post().halt_cycles;
  trace.wake_ctr = h->post().wake_ctr;
  trace.spurious_wake_ctr = h->post().spurious_wake_ctr;
  return trace;
}

void PhaseReplay(Harness* h, ShadowInterrupt* shadow, mosaic::Rng* rng) {
  const uint32_t kScriptCycles = 600;
  const std::vector<Stim> script = BuildScript(rng, kScriptCycles);

  const ReplayTrace a = RunScript(h, shadow, script);
  const ReplayTrace b = RunScript(h, shadow, script);

  Require(a.traps.size() >= 3, "replay: script coverage",
          "the scripted timeline produced only " + Dec(a.traps.size()) +
              " trap boundaries, so replay proves little");
  Require(!a.wakes.empty(), "replay: script coverage", "the scripted timeline never woke a halt");
  Require(!a.halts.empty(), "replay: script coverage", "the scripted timeline never halted");

  Require(a.traps.size() == b.traps.size(), "replay: trap boundary count",
          "run A had " + Dec(a.traps.size()) + " boundaries, run B had " + Dec(b.traps.size()));
  for (size_t i = 0; i < a.traps.size(); i++) {
    Require(a.traps[i].first == b.traps[i].first, "replay: trap boundary cycle " + Dec(i),
            "run A at cycle " + Dec(a.traps[i].first) + ", run B at cycle " +
                Dec(b.traps[i].first));
    Require(a.traps[i].second == b.traps[i].second, "replay: trap boundary cause " + Dec(i),
            "run A cause " + mosaic::Hex(a.traps[i].second) + ", run B cause " +
                mosaic::Hex(b.traps[i].second));
  }
  Require(a.wakes == b.wakes, "replay: wake cycles",
          "run A and run B woke on different cycles");
  Require(a.halts == b.halts, "replay: halt entries",
          "run A and run B entered the halt on different cycles");
  Require(a.hash == b.hash, "replay: per-cycle trace hash",
          "run A hash " + mosaic::Hex(a.hash) + ", run B hash " + mosaic::Hex(b.hash));
  Require(a.cycles_run == b.cycles_run, "replay: cycle count",
          "run A ran " + Dec(a.cycles_run) + " cycles, run B ran " + Dec(b.cycles_run) + ")");
  Require(a.irq_ctr == b.irq_ctr && a.halt_cycles == b.halt_cycles &&
              a.wake_ctr == b.wake_ctr && a.spurious_wake_ctr == b.spurious_wake_ctr,
          "replay: final counters",
          "run A (" + Dec(a.irq_ctr) + "," + Dec(a.halt_cycles) + "," + Dec(a.wake_ctr) + "," +
              Dec(a.spurious_wake_ctr) + ") vs run B (" + Dec(b.irq_ctr) + "," +
              Dec(b.halt_cycles) + "," + Dec(b.wake_ctr) + "," + Dec(b.spurious_wake_ctr) + ")");
  Require(a.spurious_wake_ctr == 0, "replay: spurious wakes",
          "the scripted timeline produced " + Dec(a.spurious_wake_ctr) + " spurious wakes");
}

void PhaseRandom(Harness* h, mosaic::Rng* rng) {
  h->Reset(4);
  const uint32_t kCycles = 4000;
  static const uint64_t kMieChoices[4] = {
      0, (1ull << kBitMtip), (1ull << kBitMsip), kAllMie};
  Stim s = Base();
  for (uint32_t i = 0; i < kCycles; i++) {
    if (rng->Chance(20)) s.irq_soft = rng->Chance(50);
    if (rng->Chance(25)) s.irq_timer = rng->Chance(50);
    if (rng->Chance(18)) s.irq_ext = rng->Chance(50);
    if (rng->Chance(25)) s.mie = kMieChoices[rng->Below(4)];
    if (rng->Chance(20)) s.mideleg = rng->Chance(20) ? (1ull << kBitMeip) : 0ull;
    if (rng->Chance(25)) s.mstatus_mie = rng->Chance(65);
    if (rng->Chance(35)) s.core_can_trap = rng->Chance(70);
    s.mip_we = rng->Chance(8);
    if (s.mip_we) {
      s.mip_op = kCsrRw + rng->Below(3);
      s.mip_wdata = (rng->Chance(50) ? (1ull << kBitMtip) : 0ull) |
                    (rng->Chance(50) ? (1ull << kBitMsip) : 0ull) |
                    (rng->Chance(20) ? (1ull << kBitMeip) : 0ull);
    }
    s.wfi_valid = rng->Chance(6);
    h->Cycle(s);
  }
  Require(h->post().spurious_wake_ctr == 0, "random: o_spurious_wake_ctr",
          "the random soak produced " + Dec(h->post().spurious_wake_ctr) + " spurious wakes");
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
  Vmosaic_interrupt_tb dut;

  Harness harness(&dut, &clk, options.max_cycles);
  ShadowInterrupt shadow;
  mosaic::Rng rng(options.seed);

  std::string detail;
  bool passed = true;
  try {
    // Phase order is deliberate: each phase resets first and owns one mechanism,
    // and the run stops at the first failure, so the order decides which phase
    // names a given defect. The replay phase runs the longest-tail mechanism
    // last, before the random soak, which can only say "some cycle".
    harness.BindShadow(&shadow);

    harness.Phase("reset-state");
    PhaseResetState(&harness);

    harness.Phase("sync-and-glitch");
    PhaseSyncAndGlitch(&harness);

    harness.Phase("masking");
    PhaseMasking(&harness);

    harness.Phase("boundary-latency");
    PhaseBoundaryLatency(&harness, &rng);

    harness.Phase("halt-resume");
    PhaseHaltResume(&harness);

    harness.Phase("mip-software-write");
    PhaseMipSoftwareWrite(&harness);

    harness.Phase("replay");
    PhaseReplay(&harness, &shadow, &rng);

    harness.Phase("random");
    PhaseRandom(&harness, &rng);

    // Coverage. A phase that quietly stopped exercising its mechanism must not
    // be able to report a pass.
    Require(harness.cov_irq_cycles > 0, "coverage: irq_valid_o",
            "no cycle offered a trap");
    Require(harness.cov_cause_mei > 0, "coverage: MEI (code 11)",
            "no machine external interrupt was ever taken");
    Require(harness.cov_cause_msi > 0, "coverage: MSI (code 3)",
            "no machine software interrupt was ever taken");
    Require(harness.cov_cause_mti > 0, "coverage: MTI (code 7)",
            "no machine timer interrupt was ever taken");
    Require(harness.cov_masked_cycles > 0, "coverage: masked pending",
            "no cycle had an enabled pending interrupt that was not offered");
    Require(harness.cov_boundary_low > 0, "coverage: boundary low",
            "no cycle held core_can_trap_i low with an enabled pending interrupt");
    Require(harness.cov_halt_entries > 0, "coverage: halt entries",
            "no WFI ever halted the core");
    Require(harness.cov_wakes > 0, "coverage: wakes", "no halt was ever ended by a wake");
    Require(harness.cov_glitches > 0, "coverage: glitches",
            "no sub-cycle glitch was exercised");
    Require(harness.cov_sw_writes > 0, "coverage: software mip writes",
            "no software write to mip was exercised");
    Require(harness.cov_halt_while_disabled_pending > 0, "coverage: halted over a disabled pending bit",
            "the core never sat halted with a pending but disabled interrupt");
    Require(dut.o_spurious_wake_ctr == 0, "coverage: spurious wakes",
            "the DUT reported " + std::to_string(dut.o_spurious_wake_ctr) + " spurious wakes");
    Require(dut.o_irq_ctr == shadow.irq_ctr() && dut.o_halt_cycles == shadow.halt_cycles() &&
                dut.o_wake_ctr == shadow.wake_ctr() &&
                dut.o_spurious_wake_ctr == shadow.spurious_wake_ctr(),
            "coverage: final counters against the shadow",
            "the DUT and the shadow counted different totals");

    detail = "interrupt contract holds: " + std::to_string(harness.comparisons()) +
             " comparisons over " + std::to_string(harness.cycles()) + " cycles, " +
             std::to_string(harness.cov_irq_cycles) + " offered traps (MEI " +
             std::to_string(harness.cov_cause_mei) + " / MSI " +
             std::to_string(harness.cov_cause_msi) + " / MTI " +
             std::to_string(harness.cov_cause_mti) + "), " +
             std::to_string(harness.cov_wakes) + " legal wakes, seed " +
             std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    // The comparison count is part of the evidence: a mutant that dies early
    // after a handful of comparisons has changed behaviour in a way this run
    // can quantify, not merely in a way it reports as a failure.
    detail = "contract violated after " + std::to_string(harness.comparisons()) +
             " comparisons: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
