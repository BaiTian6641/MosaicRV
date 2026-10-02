// ============================================================================
// tb_owner_fsm.cpp -- CASE=reconfigure.drain_and_generation, work package I-031.
//
// The DUT is `mosaic_owner_fsm`: the resource-ownership change FSM, the state
// machine frozen in config/contracts/interfaces.json as
// STOP_ADMIT -> DRAIN -> ACK -> PUBLISH -> RESUME. The DUT is never its own
// oracle. Every combinational answer, every registered state bit and every
// counter is compared against an independent C++ shadow written from the
// *contract* documented in rtl/core/mosaic_owner_fsm.sv -- not from the RTL's
// structure.
//
// The shadow's independent representation is deliberate, and it is where most of
// this file's value is. Where the RTL keeps three saturating counters, the
// shadow keeps three *stacks of outstanding items*: an admit pushes a token and
// a settle pops one, and the count it is compared against is the stack depth.
// The RTL's "count + clamp" and the shadow's "push/pop" can only agree if both
// are doing the same accounting, so a lost settle (a stack that pops nothing
// because the RTL forgot to decrement) or a double charge (the RTL incremented
// twice) shows up as a divergence on the very cycle it happens rather than as a
// wrong publish three hundred cycles later.
//
// ---------------------------------------------------------------- the case
//
// The card (docs/implementation-plan.md section 6, I-031) asks for a
// reconfiguration FSM with owner generations and idempotent control messages,
// and names the failure modes:
//
//   * a new owner must be published only after every outstanding uop, result
//     and credit of the old owner is settled -- "the issue queue is empty" is
//     *not* a drain;
//   * a repeated control message must be idempotent;
//   * a reconfiguration must not lose architectural state.
//
// The phases below exercise exactly those, and the checks are split so that a
// defect is named by the property it breaks rather than by the first byte that
// happens to differ:
//
//   message     the control answer (`ctrl_ready/ok/dup/reject`) is what the
//               contract says for this message and this state;
//   barrier     `ack_req` rises in ACK, an ack is accepted only there, and an
//               ack elsewhere is counted and inert;
//   counters    the three outstanding counts match the shadow's stacks, every
//               cycle, in every state;
//   accounting  the event counters (accepts, duplicates, rejects, acks,
//               admits-after-stop, unmatched settles, publishes, aborts) match
//               the shadow's own tallies;
//   drain       the DUT never leaves DRAIN while any class is outstanding (this
//               is the check that fails loudly on "publish before the drain");
//   barrier     the DUT never enters PUBLISH without a pre-edge `ack_ok`;
//   idempotent  a message whose sequence repeats the last accepted one is never
//               accepted, however long it is held;
//   bound       the measured latency from accepted message to publish is within
//               the bound computed from the run's own numbers, and the drain
//               watchdog fires at exactly the declared limit;
//   cold        a reset taken from any non-IDLE state restores the documented
//               cold ledger, publishes nothing and bumps no generation.
//
// ------------------------------------------------------------------ progress
//
// The progress assumption, written down and checked rather than assumed: **each
// outstanding item is matched by exactly one settle, and each settle arrives
// within DRAIN_LIMIT cycles of the admit** (the participants' obligation). Under
// it, a reconfiguration accepted while N items are outstanding publishes within
// `1 (STOP) + N (settles, one class per cycle each) + 1 (ack) + 1 (publish)`
// cycles. The case measures the real latency from the run and checks it against
// that bound; the watchdog is what makes a *violation* of the assumption visible
// as a named abort instead of an infinite wait. The stall phase drives exactly
// that violation and requires the abort, not a hang and not a wrong publish.
//
// Two-snapshot rule, applied everywhere below: values read *before* the clock
// edge describe the cycle under test and are what the DUT *presents*; values
// read *after* the edge are registered state and are what is true *now*. They
// are never mixed in one comparison. The two semantic post-checks (`drain`,
// `barrier`) are the one place the two are deliberately combined, because the
// property is about a transition: the post-edge state is read against the
// pre-edge counters and ack.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_owner_fsm_tb.h"

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

std::string Dec(uint64_t value) { return std::to_string(value); }

// The state encoding, documented in the module's port list. Named once here so a
// phase that means "the drain state" cannot silently name a different number.
enum { S_IDLE = 0, S_STOP = 1, S_DRAIN = 2, S_ACK = 3, S_PUBLISH = 4, S_COUNT = 5 };

const char* StateName(uint32_t state) {
  switch (state) {
    case S_IDLE: return "IDLE";
    case S_STOP: return "STOP";
    case S_DRAIN: return "DRAIN";
    case S_ACK: return "ACK";
    case S_PUBLISH: return "PUBLISH";
    default: return "?";
  }
}

// The geometry this driver was written for (p0). The readback checks in `main`
// fail if the elaborated DUT disagrees, so a profile change cannot leave the
// driver comparing against the wrong widths.
const uint32_t kGenW = 7;          // clog2(64) + 1
const uint32_t kGenMod = 1u << kGenW;  // the generation is a wrapping 7-bit counter
const uint32_t kCntW = 10;         // clog2(64 * 8 + 1)
const uint32_t kSeqW = 8;
const uint32_t kDrainLimit = 256;  // 4 * 64 ROB entries

// ============================================================================
// The stimulus
// ============================================================================
struct Stim {
  bool ctrl_valid = false;
  uint32_t ctrl_seq = 0;
  bool ack_valid = false;
  bool uop_new = false, uop_done = false;
  bool res_new = false, res_done = false;
  bool crd_new = false, crd_done = false;
};

std::string Str(const Stim& s) {
  std::string text = "[";
  text += s.ctrl_valid ? ("ctrl# " + Dec(s.ctrl_seq)) : "ctrl-";
  text += s.ack_valid ? " ack" : " ---";
  text += s.uop_new ? " u+" : " ..";
  text += s.uop_done ? " u-" : " ..";
  text += s.res_new ? " r+" : " ..";
  text += s.res_done ? " r-" : " ..";
  text += s.crd_new ? " c+" : " ..";
  text += s.crd_done ? " c-" : " ..";
  text += "]";
  return text;
}

// ============================================================================
// The shadow
//
// An independent model of the documented contract. Every rule below is a
// sentence from the module's header, not a step of the RTL.
// ============================================================================
class Shadow {
 public:
  Shadow(uint32_t drain_limit, uint32_t gen_w)
      : drain_limit_(drain_limit), gen_mask_((gen_w >= 32) ? 0xffffffffu : ((1u << gen_w) - 1u)) {
    Reset();
  }

  void Reset() {
    state_ = S_IDLE;
    owner_gen_ = 0;
    old_gen_ = 0;
    last_seq_ = 0;
    seq_seen_ = false;
    out_uop_.clear();
    out_res_.clear();
    out_crd_.clear();
    drain_ctr_ = 0;
    stall_ = false;
    ctrl_ok_count_ = 0;
    ctrl_dup_count_ = 0;
    ctrl_reject_count_ = 0;
    ack_ok_count_ = 0;
    ack_unexpected_count_ = 0;
    admit_after_stop_count_ = 0;
    settle_unmatched_count_ = 0;
    publish_count_ = 0;
    abort_count_ = 0;
  }

  // ---------------------------------------------------- what the DUT presents
  struct Out {
    bool ctrl_ready = false, ctrl_ok = false, ctrl_dup = false, ctrl_reject = false;
    bool ack_req = false, ack_ok = false, ack_unexpected = false;
    bool stop_admit = false, busy = false, publish = false;
    uint32_t owner_gen = 0, old_gen = 0, new_gen = 0;
    uint32_t cnt_uop = 0, cnt_res = 0, cnt_crd = 0;
  };

  // Pure: reads the pre-edge ledger and this cycle's stimulus and mutates
  // nothing.
  Out Eval(const Stim& s) const {
    const bool is_dup = seq_seen_ && (s.ctrl_seq == last_seq_);
    Out o;
    o.ctrl_ready = (state_ == S_IDLE);
    o.ctrl_ok = s.ctrl_valid && (state_ == S_IDLE) && !is_dup;
    o.ctrl_dup = s.ctrl_valid && is_dup;
    o.ctrl_reject = s.ctrl_valid && (state_ != S_IDLE) && !is_dup;
    o.ack_req = (state_ == S_ACK);
    o.ack_ok = s.ack_valid && (state_ == S_ACK);
    o.ack_unexpected = s.ack_valid && (state_ != S_ACK);
    o.stop_admit = (state_ == S_STOP) || (state_ == S_DRAIN) || (state_ == S_ACK);
    o.busy = (state_ != S_IDLE);
    o.publish = (state_ == S_PUBLISH);
    o.owner_gen = owner_gen_;
    o.old_gen = old_gen_;
    o.new_gen = (old_gen_ + 1) & gen_mask_;
    o.cnt_uop = static_cast<uint32_t>(out_uop_.size());
    o.cnt_res = static_cast<uint32_t>(out_res_.size());
    o.cnt_crd = static_cast<uint32_t>(out_crd_.size());
    return o;
  }

  // Applies one clock edge.
  void Apply(const Stim& s) {
    const uint32_t pre_state = state_;
    const bool pre_stop_admit = (pre_state == S_STOP) || (pre_state == S_DRAIN) ||
                                (pre_state == S_ACK);
    const bool pre_drained = out_uop_.empty() && out_res_.empty() && out_crd_.empty();
    const bool pre_expired = (drain_ctr_ >= (drain_limit_ - 1));
    const bool is_dup = seq_seen_ && (s.ctrl_seq == last_seq_);

    // Outstanding work: admit pushes, settle pops. An admit while admitting is
    // stopped is still counted (never lost) and flagged; a settle on an empty
    // stack is unmatched and applies nothing.
    if (s.uop_new) out_uop_.push_back(1);
    if (s.res_new) out_res_.push_back(1);
    if (s.crd_new) out_crd_.push_back(1);
    if (s.uop_done) {
      if (!out_uop_.empty()) out_uop_.pop_back();
      else settle_unmatched_count_++;
    }
    if (s.res_done) {
      if (!out_res_.empty()) out_res_.pop_back();
      else settle_unmatched_count_++;
    }
    if (s.crd_done) {
      if (!out_crd_.empty()) out_crd_.pop_back();
      else settle_unmatched_count_++;
    }
    if (pre_stop_admit) {
      if (s.uop_new) admit_after_stop_count_++;
      if (s.res_new) admit_after_stop_count_++;
      if (s.crd_new) admit_after_stop_count_++;
    }

    const bool ctrl_ok = s.ctrl_valid && (state_ == S_IDLE) && !is_dup;
    const bool ctrl_dup = s.ctrl_valid && is_dup;
    const bool ctrl_reject = s.ctrl_valid && (state_ != S_IDLE) && !is_dup;
    if (ctrl_ok) {
      last_seq_ = s.ctrl_seq;
      seq_seen_ = true;
      old_gen_ = owner_gen_;
      ctrl_ok_count_++;
    }
    if (ctrl_dup) ctrl_dup_count_++;
    if (ctrl_reject) ctrl_reject_count_++;
    if (s.ack_valid && (state_ == S_ACK)) ack_ok_count_++;
    if (s.ack_valid && (state_ != S_ACK)) ack_unexpected_count_++;

    switch (state_) {
      case S_IDLE:
        if (ctrl_ok) state_ = S_STOP;
        break;
      case S_STOP:
        state_ = S_DRAIN;
        break;
      case S_DRAIN:
        if (pre_drained) {
          state_ = S_ACK;
        } else if (pre_expired) {
          state_ = S_IDLE;
          stall_ = true;
          abort_count_++;
        }
        break;
      case S_ACK:
        if (s.ack_valid) {
          state_ = S_PUBLISH;
          owner_gen_ = (old_gen_ + 1) & gen_mask_;
          publish_count_++;
        }
        break;
      case S_PUBLISH:
        state_ = S_IDLE;
        break;
      default:
        state_ = S_IDLE;
        break;
    }

    if (pre_state == S_DRAIN && !pre_drained && !pre_expired) drain_ctr_++;
    else if (pre_state != S_DRAIN) drain_ctr_ = 0;
    // (A drain that aborts this edge keeps its final count; the RTL does too.)
  }

  // ---------------------------------------------------------- registered state
  uint32_t state() const { return state_; }
  uint32_t owner_gen() const { return owner_gen_; }
  uint32_t old_gen() const { return old_gen_; }
  uint32_t last_seq() const { return last_seq_; }
  bool seq_seen() const { return seq_seen_; }
  uint32_t cnt_uop() const { return static_cast<uint32_t>(out_uop_.size()); }
  uint32_t cnt_res() const { return static_cast<uint32_t>(out_res_.size()); }
  uint32_t cnt_crd() const { return static_cast<uint32_t>(out_crd_.size()); }
  uint32_t drain_ctr() const { return drain_ctr_; }
  bool stall() const { return stall_; }
  uint32_t ctrl_ok_count() const { return ctrl_ok_count_; }
  uint32_t ctrl_dup_count() const { return ctrl_dup_count_; }
  uint32_t ctrl_reject_count() const { return ctrl_reject_count_; }
  uint32_t ack_ok_count() const { return ack_ok_count_; }
  uint32_t ack_unexpected_count() const { return ack_unexpected_count_; }
  uint32_t admit_after_stop_count() const { return admit_after_stop_count_; }
  uint32_t settle_unmatched_count() const { return settle_unmatched_count_; }
  uint32_t publish_count() const { return publish_count_; }
  uint32_t abort_count() const { return abort_count_; }

 private:
  uint32_t drain_limit_;
  uint32_t gen_mask_;
  uint32_t state_ = S_IDLE;
  uint32_t owner_gen_ = 0;
  uint32_t old_gen_ = 0;
  uint32_t last_seq_ = 0;
  bool seq_seen_ = false;
  std::vector<uint32_t> out_uop_;
  std::vector<uint32_t> out_res_;
  std::vector<uint32_t> out_crd_;
  uint32_t drain_ctr_ = 0;
  bool stall_ = false;
  uint32_t ctrl_ok_count_ = 0;
  uint32_t ctrl_dup_count_ = 0;
  uint32_t ctrl_reject_count_ = 0;
  uint32_t ack_ok_count_ = 0;
  uint32_t ack_unexpected_count_ = 0;
  uint32_t admit_after_stop_count_ = 0;
  uint32_t settle_unmatched_count_ = 0;
  uint32_t publish_count_ = 0;
  uint32_t abort_count_ = 0;
};

// ============================================================================
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_owner_fsm_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }
  void Bind(Shadow* shadow) { shadow_ = shadow; }
  Shadow* shadow() const { return shadow_; }
  Vmosaic_owner_fsm_tb* dut() const { return dut_; }
  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) {
      Cycle(Stim(), /*rst=*/true);
    }
    if (shadow_ != nullptr) shadow_->Reset();
  }

  // One clock period, in the documented three steps: drive and settle, compare
  // what the DUT presents and let the shadow take its edge, then clock the DUT,
  // compare the registered state, and run the standing invariants.
  void Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the campaign finished");
    }
    Drive(s, rst);
    dut_->eval();

    const uint32_t pre_state = dut_->o_state;
    const uint32_t pre_uop = dut_->o_cnt_uop;
    const uint32_t pre_res = dut_->o_cnt_res;
    const uint32_t pre_crd = dut_->o_cnt_crd;
    const bool pre_ack_ok = dut_->ack_ok;

    const std::string where = phase_ + ": cycle " + Dec(clk_->cycle());
    at_ = where;

    if (!rst) {
      const Shadow::Out exp = shadow_->Eval(s);

      // The idempotency property, checked against the driver's own record of
      // what it last sent and what the DUT accepted -- before the general
      // combinational comparison, so a repeated message is named as such.
      if (dut_->ctrl_ok && shadow_->seq_seen() && (s.ctrl_seq == shadow_->last_seq())) {
        Fail("idempotent",
             "ctrl_ok rose for sequence " + Dec(s.ctrl_seq) + " at " + where +
                 "; that sequence is the last accepted one, so a repeated control "
                 "message was not ignored");
      }

      Need("message", "ctrl_ready", exp.ctrl_ready, dut_->ctrl_ready, s);
      Need("message", "ctrl_ok", exp.ctrl_ok, dut_->ctrl_ok, s);
      Need("message", "ctrl_dup", exp.ctrl_dup, dut_->ctrl_dup, s);
      Need("message", "ctrl_reject", exp.ctrl_reject, dut_->ctrl_reject, s);

      Need("barrier", "ack_req", exp.ack_req, dut_->ack_req, s);
      Need("barrier", "ack_ok", exp.ack_ok, dut_->ack_ok, s);
      Need("barrier", "ack_unexpected", exp.ack_unexpected, dut_->ack_unexpected, s);

      Need("state", "o_stop_admit", exp.stop_admit, dut_->o_stop_admit, s);
      Need("state", "o_busy", exp.busy, dut_->o_busy, s);
      Need("state", "o_publish", exp.publish, dut_->o_publish, s);

      Need("generation", "o_owner_gen", exp.owner_gen, dut_->o_owner_gen, s);
      Need("generation", "o_old_gen", exp.old_gen, dut_->o_old_gen, s);
      Need("generation", "o_new_gen", exp.new_gen, dut_->o_new_gen, s);

      Need("counters", "o_cnt_uop", exp.cnt_uop, dut_->o_cnt_uop, s);
      Need("counters", "o_cnt_res", exp.cnt_res, dut_->o_cnt_res, s);
      Need("counters", "o_cnt_crd", exp.cnt_crd, dut_->o_cnt_crd, s);

      shadow_->Apply(s);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (!rst) {
      const uint32_t post_state = dut_->o_state;

      // A DUT that leaves DRAIN with work still outstanding is publishing before
      // the drain completes -- the card's fail mode, named here.
      if (pre_state == S_DRAIN && post_state != S_DRAIN && post_state != S_IDLE) {
        const bool drained_pre = (pre_uop == 0) && (pre_res == 0) && (pre_crd == 0);
        if (!drained_pre) {
          Fail("drain",
               "left DRAIN for " + std::string(StateName(post_state)) + " at " + where +
                   " with uop=" + Dec(pre_uop) + " res=" + Dec(pre_res) + " crd=" + Dec(pre_crd) +
                   " still outstanding; the new owner must not be published before every "
                   "class of the old owner is settled");
        }
      }
      // Entering PUBLISH without an acknowledgement is the barrier defect.
      if (post_state == S_PUBLISH && pre_state != S_PUBLISH && !pre_ack_ok) {
        Fail("barrier",
             "entered PUBLISH at " + where + " without an acknowledged barrier (pre-edge "
             "state " + std::string(StateName(pre_state)) + ", ack_ok=0)");
      }

      Need("state", "o_state", shadow_->state(), dut_->o_state, s);
      Need("generation", "o_owner_gen", shadow_->owner_gen(), dut_->o_owner_gen, s);
      Need("counters", "o_cnt_uop", shadow_->cnt_uop(), dut_->o_cnt_uop, s);
      Need("counters", "o_cnt_res", shadow_->cnt_res(), dut_->o_cnt_res, s);
      Need("counters", "o_cnt_crd", shadow_->cnt_crd(), dut_->o_cnt_crd, s);
      Need("counters", "o_drain_cycles", shadow_->drain_ctr(), dut_->o_drain_cycles, s);
      Need("stall", "o_drain_stall", shadow_->stall() ? 1u : 0u, dut_->o_drain_stall, s);

      Need("accounting", "o_ctrl_ok_count", shadow_->ctrl_ok_count(), dut_->o_ctrl_ok_count,
           s);
      Need("accounting", "o_ctrl_dup_count", shadow_->ctrl_dup_count(), dut_->o_ctrl_dup_count,
           s);
      Need("accounting", "o_ctrl_reject_count", shadow_->ctrl_reject_count(),
           dut_->o_ctrl_reject_count, s);
      Need("accounting", "o_ack_ok_count", shadow_->ack_ok_count(), dut_->o_ack_ok_count, s);
      Need("accounting", "o_ack_unexpected_count", shadow_->ack_unexpected_count(),
           dut_->o_ack_unexpected_count, s);
      Need("accounting", "o_admit_after_stop_count", shadow_->admit_after_stop_count(),
           dut_->o_admit_after_stop_count, s);
      Need("accounting", "o_settle_unmatched_count", shadow_->settle_unmatched_count(),
           dut_->o_settle_unmatched_count, s);
      Need("accounting", "o_publish_count", shadow_->publish_count(), dut_->o_publish_count,
           s);
      Need("accounting", "o_abort_count", shadow_->abort_count(), dut_->o_abort_count, s);

      // Drain cycles are only meaningful in DRAIN; the RTL holds its last value
      // on the abort edge, and the shadow does the same, but an independent
      // statement of the *bound* is worth having: a live drain must never have
      // run longer than the watchdog.
      if (dut_->o_state == S_DRAIN) {
        Require(dut_->o_drain_cycles < kDrainLimit, "bound",
                "a drain has run " + Dec(dut_->o_drain_cycles) + " cycles, at or past the "
                "declared limit of " + Dec(kDrainLimit));
      }
      CheckStandingInvariants(where);
      ++comparisons_;
    }
  }

 private:
  void Drive(const Stim& s, bool rst) {
    dut_->rst = rst ? 1 : 0;
    dut_->ctrl_valid = s.ctrl_valid ? 1 : 0;
    dut_->ctrl_seq = s.ctrl_seq;
    dut_->ack_valid = s.ack_valid ? 1 : 0;
    dut_->uop_new = s.uop_new ? 1 : 0;
    dut_->uop_done = s.uop_done ? 1 : 0;
    dut_->res_new = s.res_new ? 1 : 0;
    dut_->res_done = s.res_done ? 1 : 0;
    dut_->crd_new = s.crd_new ? 1 : 0;
    dut_->crd_done = s.crd_done ? 1 : 0;
  }

  void Need(const std::string& where, const std::string& what, uint64_t expected,
            uint64_t actual, const Stim& s) {
    if (expected != actual) {
      Fail(where, what + " is " + Dec(actual) + ", expected " + Dec(expected) + " at " + at_ +
                      " (stimulus " + Str(s) + ")");
    }
  }

  // Invariants that need no shadow: they are properties of the contract itself
  // and are checked every cycle of every phase. The combinational one-liners
  // (`o_stop_admit`, `o_busy`) are not repeated here -- they are compared against
  // the shadow, which is a real oracle, rather than against a restatement.
  void CheckStandingInvariants(const std::string& where) {
    // Every accepted reconfiguration either publishes, aborts, or is the one
    // currently in flight; none vanishes and none happens twice.
    const uint64_t resolved = static_cast<uint64_t>(dut_->o_publish_count) +
                              static_cast<uint64_t>(dut_->o_abort_count);
    const uint64_t accepted = static_cast<uint64_t>(dut_->o_ctrl_ok_count);
    if (resolved > accepted) {
      Fail("accounting",
           "at " + where + " publishes+aborts = " + Dec(resolved) +
               " exceeds accepted messages = " + Dec(accepted));
    }
    const uint64_t in_flight = accepted - resolved;
    const bool mid_flight = (dut_->o_state == S_STOP) || (dut_->o_state == S_DRAIN) ||
                            (dut_->o_state == S_ACK);
    if (in_flight > 1) {
      Fail("accounting",
           "at " + where + " " + Dec(in_flight) +
               " reconfigurations are unresolved at once; at most one may be in flight");
    }
    if ((in_flight == 1) != mid_flight) {
      Fail("accounting",
           "at " + where + " state " + StateName(dut_->o_state) + " and " + Dec(in_flight) +
               " in-flight reconfiguration(s) disagree");
    }
    if (dut_->o_drain_stall && (dut_->o_abort_count == 0)) {
      Fail("stall", "at " + where + " the drain stall latched without an abort");
    }
  }

  Vmosaic_owner_fsm_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::string phase_;
  std::string at_;
  Shadow* shadow_ = nullptr;
};

// ============================================================================
// phases
// ============================================================================

// ---------------------------------------------------------------- geometry
void PhaseGeometry(Harness* h, mosaic::Reporter* rep) {
  h->Phase("geometry");
  h->Reset(4);

  const Vmosaic_owner_fsm_tb* d = h->dut();
  Require(d->o_gen_w == kGenW, "geometry",
          "the DUT reports generation width " + Dec(d->o_gen_w) + ", this driver drives " +
              Dec(kGenW));
  Require(d->o_cnt_w == kCntW, "geometry",
          "the DUT reports drain-count width " + Dec(d->o_cnt_w) + ", this driver drives " +
              Dec(kCntW));
  Require(d->o_seq_w == kSeqW, "geometry",
          "the DUT reports sequence width " + Dec(d->o_seq_w) + ", this driver drives " +
              Dec(kSeqW));
  Require(d->o_drain_limit == kDrainLimit, "geometry",
          "the DUT reports drain limit " + Dec(d->o_drain_limit) + ", this driver drives " +
              Dec(kDrainLimit));
  Require(d->o_states == S_COUNT, "geometry",
          "the DUT reports " + Dec(d->o_states) + " states, this driver drives " + Dec(S_COUNT));

  Require(d->o_state == S_IDLE && d->o_owner_gen == 0 && d->o_busy == 0, "geometry",
          "the FSM is not idle with generation 0 after reset");
  Require(d->o_cnt_uop == 0 && d->o_cnt_res == 0 && d->o_cnt_crd == 0 && d->o_drain_stall == 0,
          "geometry",
          "the outstanding counts or the stall flag are not clear after reset");
  Require(d->o_ctrl_ok_count == 0 && d->o_publish_count == 0 && d->o_abort_count == 0 &&
              d->o_ctrl_dup_count == 0,
          "geometry",
          "the event counters are not clear after reset");

  rep->Check(true,
             "geometry: generation " + Dec(kGenW) + " bits, drain count " + Dec(kCntW) +
                 " bits, drain watchdog " + Dec(kDrainLimit) +
                 " cycles, cold ledger after reset");
}

// --------------------------------------------------- busy-idle accounting
// Outstanding work can be admitted and settled freely while idle, and a settle
// with nothing outstanding is unmatched, not an underflow.
void PhaseIdleAccounting(Harness* h, mosaic::Reporter* rep) {
  h->Phase("idle");
  h->Reset(4);

  // Admit one of each class, then settle them.
  for (int i = 0; i < 4; i++) {
    Stim s;
    s.uop_new = true;
    h->Cycle(s);
  }
  for (int i = 0; i < 4; i++) {
    Stim s;
    s.uop_done = true;
    s.res_new = (i < 2);
    h->Cycle(s);
  }
  for (int i = 0; i < 2; i++) {
    Stim s;
    s.res_done = true;
    h->Cycle(s);
  }
  // Three settles with nothing outstanding: unmatched.
  for (int i = 0; i < 3; i++) {
    Stim s;
    s.crd_done = true;
    h->Cycle(s);
  }
  Require(h->dut()->o_settle_unmatched_count == 3, "counters",
          "three settles with an empty class produced " + Dec(h->dut()->o_settle_unmatched_count) +
              " unmatched-settle reports, expected 3");
  Require(h->dut()->o_cnt_uop == 0 && h->dut()->o_cnt_res == 0 && h->dut()->o_cnt_crd == 0,
          "counters", "the counts are not back to zero after settling every admit");
  rep->Check(true, "idle: admits and settles track per class and an unmatched settle is "
                   "counted without underflow");
}

// ------------------------------------------------- the one reconfiguration
// The card's central property: a new generation is published only after every
// class is settled, and the measured latency is within the bound computed from
// this run's own numbers.
void PhaseHappyPath(Harness* h, mosaic::Reporter* rep) {
  h->Phase("happy");
  h->Reset(4);

  // Build a mixed outstanding population: 3 uops, 2 results, 2 credits.
  for (int i = 0; i < 3; i++) {
    Stim s;
    s.uop_new = true;
    h->Cycle(s);
  }
  for (int i = 0; i < 2; i++) {
    Stim s;
    s.res_new = true;
    h->Cycle(s);
  }
  for (int i = 0; i < 2; i++) {
    Stim s;
    s.crd_new = true;
    h->Cycle(s);
  }
  Require(h->dut()->o_cnt_uop == 3 && h->dut()->o_cnt_res == 2 && h->dut()->o_cnt_crd == 2,
          "counters", "the mixed population did not accumulate as driven");

  const uint32_t n_start = 3 + 2 + 2;
  const uint64_t accept_cycle = h->cycles();
  uint64_t publish_cycle = 0;
  uint32_t max_drain = 0;

  // Raise the message and hold it for the whole reconfiguration: the same
  // message repeated every cycle must be one reconfiguration, not many.
  {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 5;
    h->Cycle(s);
  }
  Require(h->dut()->o_ctrl_ok_count == 1 && h->dut()->o_state == S_STOP, "message",
          "the control message was not accepted exactly once into STOP");

  bool saw_ack_state = false;
  for (int guard = 0; guard < 64 && publish_cycle == 0; guard++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 5;  // held: a duplicate every cycle
    const uint32_t st = h->dut()->o_state;
    if (st == S_DRAIN) {
      if (h->dut()->o_cnt_uop > 0) s.uop_done = true;
      else if (h->dut()->o_cnt_res > 0) s.res_done = true;
      else if (h->dut()->o_cnt_crd > 0) s.crd_done = true;
      if (h->dut()->o_drain_cycles > max_drain) max_drain = h->dut()->o_drain_cycles;
    } else if (st == S_ACK) {
      saw_ack_state = true;
      s.ack_valid = true;
    }
    h->Cycle(s);
    if (h->dut()->o_state == S_PUBLISH && h->dut()->o_publish) {
      publish_cycle = h->cycles();  // the cycle in which the publish was presented
    }
  }

  Require(publish_cycle != 0, "drain", "the reconfiguration never published");
  Require(saw_ack_state, "barrier", "the FSM never entered ACK");
  Require(h->dut()->o_owner_gen == 1, "generation",
          "the published generation is " + Dec(h->dut()->o_owner_gen) + ", expected 1");
  Require(h->dut()->o_publish_count == 1 && h->dut()->o_abort_count == 0, "accounting",
          "expected exactly one publish and no abort, got publish=" +
              Dec(h->dut()->o_publish_count) + " abort=" + Dec(h->dut()->o_abort_count) +
              " owner_gen=" + Dec(h->dut()->o_owner_gen) + " state=" +
              StateName(h->dut()->o_state));
  Require(h->dut()->o_ctrl_dup_count >= 3, "idempotent",
          "the held message was only reported as a duplicate " +
              Dec(h->dut()->o_ctrl_dup_count) + " time(s)");
  Require(h->dut()->o_cnt_uop == 0 && h->dut()->o_cnt_res == 0 && h->dut()->o_cnt_crd == 0,
          "counters", "the counts are not zero after publish");

  // Hold the same message past the publish: the contract says the *same*
  // message is one reconfiguration however long it is held, so nothing may
  // start a second one. This is the check the non-idempotent control mutant
  // breaks.
  for (int i = 0; i < 6; i++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 5;
    h->Cycle(s);
  }
  Require(h->dut()->o_ctrl_ok_count == 1 && h->dut()->o_publish_count == 1 &&
              h->dut()->o_owner_gen == 1,
          "idempotent", "a message held past the publish started another reconfiguration "
          "(accepted=" + Dec(h->dut()->o_ctrl_ok_count) + " publishes=" +
          Dec(h->dut()->o_publish_count) + " generation=" + Dec(h->dut()->o_owner_gen) + ")");
  Require(h->dut()->o_ctrl_dup_count >= 6, "idempotent",
          "the held duplicate was reported only " + Dec(h->dut()->o_ctrl_dup_count) +
              " time(s) for six held cycles");

  const uint64_t latency = publish_cycle - accept_cycle;
  // The bound from the run's own numbers, term by term: one cycle for the
  // accepted message to take effect and one for STOP to hand over to DRAIN, one
  // settle cycle per outstanding item (an item can settle at most once per
  // cycle), one cycle for the FSM to observe that the last count reached zero,
  // and one cycle from the ack to the publish.
  const uint64_t bound = n_start + 4;
  Require(latency <= bound, "bound",
          "reconfiguration accepted with " + Dec(n_start) + " outstanding items took " +
              Dec(latency) + " cycles to publish, above the run's own bound of " + Dec(bound) +
              " (1 accept + 1 STOP + " + Dec(n_start) + " settle + 1 observe + 1 publish)");
  Require(latency <= kDrainLimit, "bound",
          "a drain of " + Dec(latency) + " cycles exceeded the declared watchdog limit " +
              Dec(kDrainLimit));
  Require(max_drain > 0, "drain", "no drain cycle was ever observed");

  std::printf("  [drain] outstanding=%u latency=%llu bound=%llu max-drain-cycles=%u\n", n_start,
              static_cast<unsigned long long>(latency), static_cast<unsigned long long>(bound),
              max_drain);
  rep->Check(true, "happy: a mixed population of 3 uops, 2 results and 2 credits held the new "
                   "generation back until every class was settled, then published generation 1 "
                   "in " + Dec(latency) + " cycles (bound " + Dec(bound) + ")");
}

// -------------------------------------------------------- ack barrier
void PhaseBarrier(Harness* h, mosaic::Reporter* rep) {
  h->Phase("barrier");
  h->Reset(4);

  {
    Stim s;
    s.uop_new = true;
    h->Cycle(s);
  }
  {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 3;
    h->Cycle(s);
  }
  // Drain it.
  for (int i = 0; i < 4 && h->dut()->o_state != S_ACK; i++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 3;
    if (h->dut()->o_state == S_DRAIN && h->dut()->o_cnt_uop > 0) s.uop_done = true;
    h->Cycle(s);
  }
  Require(h->dut()->o_state == S_ACK && h->dut()->ack_req, "barrier",
          "the drain completed but ACK was not entered with ack_req raised");

  // Withhold the ack for a while: no publish may happen.
  const uint32_t gen_before = h->dut()->o_owner_gen;
  for (int i = 0; i < 10; i++) {
    Stim s;
    s.ack_valid = false;
    h->Cycle(s);
    Require(h->dut()->o_publish == 0 && h->dut()->o_owner_gen == gen_before, "barrier",
            "a generation was published before the barrier was acknowledged");
  }
  // An ack that arrives in a state other than ACK (here, after the wait) is
  // fine; but a late ack after publish must be inert -- exercised in the soak.
  {
    Stim s;
    s.ack_valid = true;
    h->Cycle(s);
  }
  Require(h->dut()->o_owner_gen == gen_before + 1 && h->dut()->o_ack_ok_count == 1, "barrier",
          "the acknowledged barrier did not publish exactly one new generation");

  // A second ack after the publish is unexpected and inert.
  const uint32_t pub_before = h->dut()->o_publish_count;
  for (int i = 0; i < 3; i++) {
    Stim s;
    s.ack_valid = true;
    h->Cycle(s);
  }
  Require(h->dut()->o_publish_count == pub_before && h->dut()->o_ack_unexpected_count >= 3,
          "barrier", "a repeated ack was not counted as unexpected and inert");

  rep->Check(true, "barrier: a drain that completed while the ack was withheld published nothing, "
                   "and repeated acks were unexpected and inert");
}

// ------------------------------------------ admit during stop is not lost
void PhaseAdmitAfterStop(Harness* h, mosaic::Reporter* rep) {
  h->Phase("admit-stop");
  h->Reset(4);

  {
    Stim s;
    s.uop_new = true;
    h->Cycle(s);
  }
  {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 9;
    h->Cycle(s);
  }
  // Now in STOP/DRAIN with stopping in force. Admit one of each class anyway.
  const uint32_t before = h->dut()->o_admit_after_stop_count;
  for (int i = 0; i < 3; i++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 9;
    s.res_new = (i == 0);
    s.crd_new = (i == 1);
    s.uop_new = (i == 2);
    h->Cycle(s);
  }
  Require(h->dut()->o_admit_after_stop_count == before + 3, "accounting",
          "three admits during stop produced " +
              Dec(h->dut()->o_admit_after_stop_count - before) + " violation reports, expected 3");
  Require(h->dut()->o_cnt_uop == 2 && h->dut()->o_cnt_res == 1 && h->dut()->o_cnt_crd == 1,
          "counters",
          "an admit during stop was refused rather than counted: uop=" +
              Dec(h->dut()->o_cnt_uop) + " res=" + Dec(h->dut()->o_cnt_res) + " crd=" +
              Dec(h->dut()->o_cnt_crd) + ", expected 2/1/1");

  // Settle everything and confirm the publish waited for the late work.
  const uint32_t gen_before = h->dut()->o_owner_gen;
  for (int guard = 0; guard < 32 && h->dut()->o_owner_gen == gen_before; guard++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 9;
    if (h->dut()->o_state == S_DRAIN) {
      if (h->dut()->o_cnt_uop > 0) s.uop_done = true;
      else if (h->dut()->o_cnt_res > 0) s.res_done = true;
      else if (h->dut()->o_cnt_crd > 0) s.crd_done = true;
    } else if (h->dut()->o_state == S_ACK) {
      s.ack_valid = true;
    }
    h->Cycle(s);
  }
  Require(h->dut()->o_owner_gen == gen_before + 1, "drain",
          "the late-admitted work did not hold the publish back until it settled");

  rep->Check(true, "admit-stop: work admitted after the stop is counted and flagged, and the "
                   "publish waits for it rather than losing it");
}

// -------------------------------------- the watchdog turns a lost credit
// -------------------------------------- into a named abort, not a hang
void PhaseStallAbort(Harness* h, mosaic::Reporter* rep) {
  h->Phase("stall");
  h->Reset(4);

  {
    Stim s;
    s.crd_new = true;
    h->Cycle(s);
  }
  {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 11;
    h->Cycle(s);
  }
  Require(h->dut()->o_state == S_STOP, "message", "the stall test did not enter STOP");

  const uint32_t gen_before = h->dut()->o_owner_gen;
  uint32_t observed_stall_cycle = 0;
  for (uint32_t i = 0; i < kDrainLimit + 8; i++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 11;
    h->Cycle(s);
    if (h->dut()->o_drain_stall && observed_stall_cycle == 0) observed_stall_cycle = i;
  }
  Require(h->dut()->o_drain_stall == 1, "stall",
          "a credit left outstanding for ever did not trip the drain watchdog");
  Require(h->dut()->o_abort_count == 1 && h->dut()->o_publish_count == 0, "stall",
          "the stuck drain published or failed to abort exactly once");
  Require(h->dut()->o_owner_gen == gen_before, "generation",
          "the aborted reconfiguration changed the owner generation");
  Require(h->dut()->o_state == S_IDLE, "state", "the aborted reconfiguration did not return to "
                                                "IDLE");
  // The outstanding credit is *not* lost: the old owner still holds it.
  Require(h->dut()->o_cnt_crd == 1, "counters",
          "the abort lost the outstanding credit (count " + Dec(h->dut()->o_cnt_crd) +
              ", expected 1)");

  // Recovery: settle it and reconfigure again with a fresh sequence.
  {
    Stim s;
    s.crd_done = true;
    h->Cycle(s);
  }
  bool published = false;
  for (int guard = 0; guard < 16 && !published; guard++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 12;
    if (h->dut()->o_state == S_ACK) s.ack_valid = true;
    h->Cycle(s);
    if (h->dut()->o_publish) published = true;
  }
  Require(published && h->dut()->o_owner_gen == gen_before + 1, "drain",
          "the reconfiguration after a recovered abort did not publish");

  std::printf("  [stall] abort at drain-cycle %u (limit %u), generation held at %u, credit "
              "retained\n", observed_stall_cycle, kDrainLimit, gen_before);
  rep->Check(true, "stall: an unsettled credit tripped the watchdog into a named abort with no "
                   "generation change and no lost work, and the next reconfiguration published");
}

// ------------------------------------------ reset from every busy state
void CheckCold(Harness* h, const std::string& where) {
  const Vmosaic_owner_fsm_tb* d = h->dut();
  Require(d->o_state == S_IDLE && d->o_owner_gen == 0 && d->o_busy == 0 && d->o_publish == 0,
          where, "reset did not restore the idle state at generation 0");
  Require(d->o_stop_admit == 0 && d->o_cnt_uop == 0 && d->o_cnt_res == 0 && d->o_cnt_crd == 0,
          where, "reset did not clear the stop, drain and outstanding state");
  Require(d->o_drain_stall == 0 && d->o_drain_cycles == 0, where,
          "reset did not clear the drain watchdog");
  Require(d->o_ctrl_ok_count == 0 && d->o_ctrl_dup_count == 0 && d->o_ctrl_reject_count == 0 &&
              d->o_ack_ok_count == 0 && d->o_ack_unexpected_count == 0 &&
              d->o_admit_after_stop_count == 0 && d->o_settle_unmatched_count == 0 &&
              d->o_publish_count == 0 && d->o_abort_count == 0,
          where, "reset did not clear the event counters");
}

void PhaseResetTraversal(Harness* h, mosaic::Reporter* rep) {
  h->Phase("reset");

  // STOP: accept a message and reset in the same window.
  h->Reset(4);
  {
    Stim s;
    s.uop_new = true;
    h->Cycle(s);
    Stim c;
    c.ctrl_valid = true;
    c.ctrl_seq = 21;
    h->Cycle(c);
  }
  Require(h->dut()->o_state == S_STOP, "reset", "could not reach STOP");
  h->Reset(2);
  CheckCold(h, "reset");

  // DRAIN: accept, drain, and reset while still draining.
  h->Reset(4);
  {
    Stim s;
    s.uop_new = true;
    h->Cycle(s);
    Stim c;
    c.ctrl_valid = true;
    c.ctrl_seq = 22;
    h->Cycle(c);
  }
  for (int i = 0; i < 4 && h->dut()->o_state != S_DRAIN; i++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 22;
    h->Cycle(s);
  }
  Require(h->dut()->o_state == S_DRAIN, "reset", "could not reach DRAIN");
  h->Reset(2);
  CheckCold(h, "reset");

  // ACK: reach ACK and reset there.
  h->Reset(4);
  {
    Stim s;
    s.uop_new = true;
    h->Cycle(s);
    Stim c;
    c.ctrl_valid = true;
    c.ctrl_seq = 23;
    h->Cycle(c);
  }
  for (int i = 0; i < 8 && h->dut()->o_state != S_ACK; i++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 23;
    if (h->dut()->o_state == S_DRAIN && h->dut()->o_cnt_uop > 0) s.uop_done = true;
    h->Cycle(s);
  }
  Require(h->dut()->o_state == S_ACK, "reset", "could not reach ACK");
  h->Reset(2);
  CheckCold(h, "reset");

  // PUBLISH: reset on the publish cycle itself -- the generation must not advance.
  h->Reset(4);
  {
    Stim s;
    s.uop_new = true;
    h->Cycle(s);
    Stim c;
    c.ctrl_valid = true;
    c.ctrl_seq = 24;
    h->Cycle(c);
  }
  for (int i = 0; i < 8 && h->dut()->o_state != S_ACK; i++) {
    Stim s;
    s.ctrl_valid = true;
    s.ctrl_seq = 24;
    if (h->dut()->o_state == S_DRAIN && h->dut()->o_cnt_uop > 0) s.uop_done = true;
    h->Cycle(s);
  }
  Require(h->dut()->o_state == S_ACK, "reset", "could not reach ACK before PUBLISH");
  {
    Stim s;
    s.ack_valid = true;
    h->Cycle(s);
  }
  Require(h->dut()->o_state == S_PUBLISH && h->dut()->o_owner_gen == 1, "reset",
          "the ack did not publish generation 1");
  h->Reset(2);
  CheckCold(h, "reset");

  rep->Check(true, "reset: a reset taken from STOP, DRAIN, ACK and PUBLISH restores the cold "
                   "ledger and never advances the generation");
}

// ------------------------------------------------------------- soak
void PhaseSoak(Harness* h, mosaic::Reporter* rep, uint64_t seed) {
  h->Phase("soak");
  h->Reset(4);

  mosaic::Rng rng(seed ^ 0x1234abcdull);
  const uint32_t start_gen = h->dut()->o_owner_gen;
  for (int cycle = 0; cycle < 4000; cycle++) {
    Stim s;
    const uint32_t st = h->dut()->o_state;
    const bool admitting_stopped = (st == S_STOP) || (st == S_DRAIN) || (st == S_ACK);

    // New work: almost never once admitting is stopped (the odd violation
    // exercises the flagged-but-counted path), otherwise often enough to keep
    // the outstanding population small.
    const uint32_t new_pct = admitting_stopped ? 3 : 25;
    s.uop_new = rng.Chance(new_pct);
    s.res_new = rng.Chance(new_pct);
    s.crd_new = rng.Chance(new_pct);

    // Settlement: mostly when the class actually has something outstanding (so
    // drains are real work, not spurious settles), with the occasional spurious
    // settle to exercise the unmatched path under load too.
    s.uop_done = rng.Chance(55) && ((h->dut()->o_cnt_uop > 0) || s.uop_new || rng.Chance(4));
    s.res_done = rng.Chance(55) && ((h->dut()->o_cnt_res > 0) || s.res_new || rng.Chance(4));
    s.crd_done = rng.Chance(55) && ((h->dut()->o_cnt_crd > 0) || s.crd_new || rng.Chance(4));

    // Control messages: sometimes a fresh sequence, sometimes a repeat.
    if (rng.Chance(12)) {
      s.ctrl_valid = true;
      s.ctrl_seq = rng.Below(6);
    }
    // Acks: mostly in ACK, occasionally elsewhere.
    if (st == S_ACK) {
      s.ack_valid = rng.Chance(40);
    } else {
      s.ack_valid = rng.Chance(3);
    }
    h->Cycle(s);
  }

  const uint32_t publishes = h->dut()->o_publish_count;
  Require(publishes >= 3, "soak",
          "the soak produced only " + Dec(publishes) +
              " published generations, so it does not exercise the transition");
  Require(h->dut()->o_abort_count == 0, "soak",
          "the soak tripped the stall watchdog " + Dec(h->dut()->o_abort_count) +
              " time(s), so its stimulus did not obey the progress assumption");
  Require(h->dut()->o_owner_gen == ((start_gen + publishes) & (kGenMod - 1)), "generation",
          "the owner generation did not advance once per publish");
  std::printf("  [soak] cycles=4000 publishes=%u dup=%u reject=%u admit-after-stop=%u "
              "unmatched=%u\n", publishes, h->dut()->o_ctrl_dup_count,
              h->dut()->o_ctrl_reject_count, h->dut()->o_admit_after_stop_count,
              h->dut()->o_settle_unmatched_count);
  rep->Check(true, "soak: a 4000-cycle mixed soak of control messages, acks, admits and settles "
                   "stayed shadow-exact and published " + Dec(publishes) + " generations");
}

}  // namespace

// ============================================================================
// main
// ============================================================================

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
  Vmosaic_owner_fsm_tb dut;
  Harness harness(&dut, &clk, options.max_cycles);
  Shadow shadow(kDrainLimit, kGenW);
  harness.Bind(&shadow);

  std::string detail;
  bool passed = true;
  try {
    PhaseGeometry(&harness, &reporter);
    PhaseIdleAccounting(&harness, &reporter);
    PhaseHappyPath(&harness, &reporter);
    PhaseBarrier(&harness, &reporter);
    PhaseAdmitAfterStop(&harness, &reporter);
    PhaseStallAbort(&harness, &reporter);
    PhaseResetTraversal(&harness, &reporter);
    PhaseSoak(&harness, &reporter, options.seed);

    detail = "ownership change holds: " + Dec(harness.comparisons()) +
             " shadow-exact cycle comparisons over " + Dec(harness.cycles()) + " cycles";
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated after " + Dec(harness.comparisons()) + " checks: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
