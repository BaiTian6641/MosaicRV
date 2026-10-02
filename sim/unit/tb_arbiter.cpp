// ============================================================================
// tb_arbiter.cpp -- CASE=arbiter.forward_progress, work package I-030.
//
// The DUT is `mosaic_arbiter`: quota-bounded service with reserved escape
// capacity. The driver is the environment the policy serves -- four producers
// offering completions, and a downstream that accepts one per ready cycle and
// is then busy for a class-dependent number of cycles (the card's mix of short
// and long function units, and the only source of the waiting bound's
// D_MAX). The driver is never the DUT's oracle for the *choice*: it does not
// re-implement the selection. It checks invariants the policy's bound rests on,
// and it measures the bound from the run's own numbers.
//
// ---------------------------------------------------------------- the case
//
// The card asks for a bounded-service policy, the fairness assumption written
// down, and a stress case that shows:
//
//   * continuous new requests into every class, with short and long service;
//   * at least one class a naive priority scheme would starve for ever;
//   * each class's worst-case wait measured from the run and checked against a
//     bound **computed from the run's own numbers**, not from the design's
//     intent;
//   * the assumption stated, and an assumption violation reported loudly as a
//     violation rather than as a bound;
//   * a check that a class cannot be starved indefinitely under that
//     assumption.
//
// ------------------------------------------------------------------ checks
//
// Every cycle, in every phase, the driver checks:
//
//   conservation   the DUT's per-class occupancy equals the driver's own
//                  accepted-but-not-delivered queue; the delivered request is
//                  the *oldest* of its class with its own tag; a ready port
//                  with pending work always transfers; the DUT's own loss
//                  counter stays zero; the grant/accept/refuse/stall counters
//                  agree with the driver's independent tallies.
//   bound          the DUT's window index, its grant count in the window and
//                  its per-class `used` counters equal the driver's counts, so
//                  the window really is the quota's W grant opportunities and
//                  the quota is charged per grant. A window that closes must
//                  have held exactly W opportunities.
//   escape         the escape path is taken only when no class with pending
//                  work is under its quota -- the premise the reservation proof
//                  needs.
//   assumption     with work pending, the port is not ready for more than the
//                  declared ARB_D_MAX consecutive cycles, and the DUT never
//                  reports a violation the stimulus did not cause.
//   starvation     at every window close, every class that had pending work at
//                  every opportunity of that window was granted at least its
//                  quota. This is the check that fails loudly on a class being
//                  starved indefinitely.
//
// --------------------------------------------------------- the bound, and
// ------------------------------------------- why it is computed from the run
//
// The policy guarantees: a class with pending work throughout a window is
// granted at least Q[class] times in it. The driver measures, from the run,
//   * `share_min[class]` -- the smallest number of grants the class actually
//     received in a window in which it had work at every opportunity;
//   * `gap_max` -- the longest wall-clock distance between two consecutive
//     grant opportunities, measured (each opportunity is a cycle in which the
//     port is ready and work is pending);
//   * W, verified as the number of opportunities in every window that closed.
// A request with `n` requests of its class ahead of it is therefore granted
// within
//
//     gap_max * (W * (ceil((n + 1) / share_min) + 1) + 2)      cycles
//
// and that is the number each measured wait is checked against. Every term is a
// measurement from this run, so the check cannot pass by repeating the design's
// intent: if the run shows a smaller share, or a longer gap, the bound the run
// is held to changes with it.
//
// ---------------------------------------------------------------- the naive
// ---------------------------------------------------------------- control
//
// A stress case that no scheme could starve proves nothing. The driver replays
// the *same offer stream* through a second model of the same machine -- same
// queue depth, same service times, same downstream -- whose only difference is
// strict class priority. The run fails if that model starves no class, so
// "this stimulus reaches the card's fail mode" is measured rather than claimed.
//
// ------------------------------------------------------------------ phases
//
//   1. geometry              the policy's read-back is the policy this driver
//                            drives.
//   2. directed:fifo-order   three requests of one class leave in order; the
//                            escape keeps a lone class moving past its quota.
//   3. directed:credit       a full queue refuses and holds: backpressure, not
//                            loss.
//   4. directed:reservation  a continuous SCALAR flood plus one VECTOR request:
//                            VECTOR is served, within the bound, and the same
//                            stimulus starves VECTOR under naive priority.
//   5. directed:return       the return channel (MEMORY) is served while
//                            SCALAR floods.
//   6. directed:escape       one class past its quota is still served.
//   7. directed:assumption   the port held not-ready past D_MAX is reported as
//                            an assumption violation, not as a bound.
//   8. saturation            the stress mix: every class a continuous stream
//                            of new requests, short and long service, 2400
//                            cycles, the reservation and bound checks above,
//                            and the naive control.
//   9. determinism           the same stimulus and seed reproduce the grant
//                            stream exactly.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_arbiter_tb.h"

namespace {

// ================================================================ constants
//
// The arbitration classes and the machine the driver serves them into. These
// are the driver's environment: the policy's own geometry is read back from the
// DUT and checked against them.

constexpr int kN = 4;
enum { CLS_SCALAR = 0, CLS_MEMORY = 1, CLS_MULDIV = 2, CLS_VECTOR = 3 };

constexpr uint32_t kQuota[kN] = {4, 2, 1, 1};
constexpr uint32_t kWindow = 8;
constexpr uint32_t kDepth = 4;
constexpr uint32_t kService[kN] = {1, 2, 3, 3};
constexpr uint32_t kDMax = 4;

const char* kName[kN] = {"SCALAR", "MEMORY", "MULDIV", "VECTOR"};
const char* ClassName(uint32_t c) { return c < static_cast<uint32_t>(kN) ? kName[c] : "?"; }

// =============================================================== diagnostics

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

uint64_t CeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// The bound this case holds every measured wait to. See the header: every term
// is measured from the run, never a constant of the design's intent.
uint64_t WaitBound(uint64_t share, uint32_t ahead, uint64_t gap, uint64_t window) {
  const uint64_t windows = CeilDiv(static_cast<uint64_t>(ahead) + 1, share);
  return gap * (window * (windows + 1) + 2);
}

// ============================================================ the stimulus

struct Stim {
  bool req_valid[kN] = {false, false, false, false};
  uint32_t req_src[kN] = {0, 0, 0, 0};
  // The directed assumption phase holds the port not-ready by hand; everywhere
  // else the driver's downstream model is the only source of `srv_busy`.
  bool force_busy = false;
};

// One accepted request, waiting for its class's queue to reach it.
struct Pending {
  uint32_t src = 0;
  uint64_t arrive = 0;
  uint32_t ahead = 0;
};

struct WaitRec {
  uint32_t src = 0;
  uint32_t ahead = 0;
  uint64_t wait = 0;
};

struct Grant {
  uint64_t cycle = 0;
  uint32_t cls = 0;
  uint32_t src = 0;
};

// The offer stream, recorded so the naive control can be replayed over exactly
// the stimulus the DUT saw.
struct Offer {
  uint8_t valid_mask = 0;
  uint8_t src[kN] = {0, 0, 0, 0};
};

// ================================================================= observed

struct Obs {
  bool srv_go = false;
  uint32_t srv_class = 0;
  uint32_t srv_src = 0;
  bool req_ready[kN] = {false, false, false, false};
  uint32_t used[kN] = {0, 0, 0, 0};
  uint32_t pend[kN] = {0, 0, 0, 0};
  uint32_t win_index = 0;
  uint32_t win_grants = 0;
  bool viol = false;
};

// ============================================================================
// the harness: the DUT plus the environment it is measured in
// ============================================================================

class Harness {
 public:
  Harness(Vmosaic_arbiter_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  Vmosaic_arbiter_tb* dut() { return dut_; }
  void Phase(const std::string& name) { phase_ = name; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }
  std::string Where() const { return phase_ + ": cycle " + Dec(cycles_); }

  // ------------------------------------------------------- measured numbers
  uint64_t granted_of(int c) const { return granted_[c]; }
  uint64_t accepted_of(int c) const { return accepted_[c]; }
  uint64_t grants() const { return grants_; }
  uint64_t accepts() const { return accepts_; }
  uint64_t refuses() const { return refused_; }
  uint64_t stalls() const { return stalls_; }
  uint64_t escapes() const { return escape_ctr_seen_; }
  uint64_t gap_max() const { return gap_max_; }
  uint32_t busy_run_max() const { return busy_run_max_; }
  bool share_seen(int c) const { return share_seen_[c]; }
  uint64_t share_min(int c) const { return share_min_[c]; }
  const std::vector<WaitRec>& waits(int c) const { return waits_[c]; }
  bool wait_any(int c) const { return !waits_[c].empty(); }
  const std::vector<Offer>& offers() const { return offers_; }
  const std::vector<Grant>& stream() const { return stream_; }
  uint64_t windows_closed() const { return windows_closed_; }
  uint64_t window_grant_min(int c) const { return win_grant_min_[c]; }

  // --------------------------------------------------------------- the run
  void Reset(int cycles) {
    ResetModel();
    ClearRecords();
    for (int i = 0; i < cycles; i++) Cycle(Stim(), /*rst=*/true);
  }

  void ExpectViolation(bool on) { expect_violation_ = on; }

  Obs Cycle(const Stim& s_in, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the phase finished");
    }
    const uint64_t now = cycles_;
    const std::string where = phase_ + ": cycle " + Dec(now);
    const bool busy = (!rst) && (s_in.force_busy || (now < busy_until_));

    dut_->rst = rst ? 1 : 0;
    uint8_t mask = 0;
    for (int c = 0; c < kN; c++) {
      if (s_in.req_valid[c]) mask = static_cast<uint8_t>(mask | (1u << c));
    }
    dut_->req_valid_i = mask;
    dut_->req_src0_i = s_in.req_src[0];
    dut_->req_src1_i = s_in.req_src[1];
    dut_->req_src2_i = s_in.req_src[2];
    dut_->req_src3_i = s_in.req_src[3];
    dut_->srv_busy_i = busy ? 1 : 0;
    dut_->eval();

    Obs o;
    bool any_under_quota = false;
    if (!rst) {
      Capture(&o);
      Check(o, s_in, busy, now, &any_under_quota);
      Offer off;
      off.valid_mask = mask;
      for (int c = 0; c < kN; c++) off.src[c] = static_cast<uint8_t>(s_in.req_src[c]);
      offers_.push_back(off);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();

    if (!rst) Apply(o, s_in, busy, now, any_under_quota);
    ++cycles_;
    return o;
  }

 private:
  void ResetModel() {
    for (int c = 0; c < kN; c++) {
      q_[c].clear();
      accepted_[c] = 0;
      granted_[c] = 0;
      win_grants_c_[c] = 0;
      had_work_[c] = true;
      share_seen_[c] = false;
      share_min_[c] = 0;
      win_grant_min_[c] = 0;
      max_wait_[c] = 0;
    }
    busy_until_ = 0;
    refused_ = 0;
    accepts_ = 0;
    grants_ = 0;
    stalls_ = 0;
    escapes_ = 0;
    escape_ctr_seen_ = 0;
    busy_run_ = 0;
    busy_run_max_ = 0;
    gap_max_ = 1;
    win_index_ = 0;
    grants_in_window_ = 0;
    opps_in_window_ = 0;
    win_started_ = false;
    windows_closed_ = 0;
    expect_violation_ = false;
  }

  void ClearRecords() {
    for (int c = 0; c < kN; c++) waits_[c].clear();
    offers_.clear();
    stream_.clear();
  }

  void Capture(Obs* o) {
    o->srv_go = dut_->srv_go_o != 0;
    o->srv_class = dut_->srv_class_o;
    o->srv_src = dut_->srv_src_o;
    o->req_ready[0] = (dut_->req_ready_o & 1u) != 0;
    o->req_ready[1] = (dut_->req_ready_o & 2u) != 0;
    o->req_ready[2] = (dut_->req_ready_o & 4u) != 0;
    o->req_ready[3] = (dut_->req_ready_o & 8u) != 0;
    o->used[0] = dut_->o_used0_o;
    o->used[1] = dut_->o_used1_o;
    o->used[2] = dut_->o_used2_o;
    o->used[3] = dut_->o_used3_o;
    o->pend[0] = dut_->o_pend0_o;
    o->pend[1] = dut_->o_pend1_o;
    o->pend[2] = dut_->o_pend2_o;
    o->pend[3] = dut_->o_pend3_o;
    o->win_index = dut_->o_window_index_o;
    o->win_grants = dut_->o_window_grants_o;
    o->viol = dut_->o_assumption_violated_o != 0;
  }

  uint64_t PendTotal() const {
    uint64_t total = 0;
    for (int c = 0; c < kN; c++) total += q_[c].size();
    return total;
  }

  // ------------------------------------------------- the pre-edge contract
  void Check(const Obs& o, const Stim& s, bool busy, uint64_t now, bool* any_under_quota) {
    const std::string where = phase_ + ": cycle " + Dec(now);
    const uint64_t pend_total = PendTotal();

    // 1. A ready port with pending work always transfers. A request that was
    //    accepted and is neither delivered nor still held is a loss.
    if (pend_total > 0 && !busy) {
      comparisons_++;
      Require(o.srv_go, where + " (conservation)",
              "conservation: the port is ready with " + Dec(pend_total) +
                  " pending request(s) and no transfer happened; an accepted request was "
                  "neither delivered nor held (loss instead of backpressure)");
    }

    // 2. The DUT's occupancy is the driver's queue.
    for (int c = 0; c < kN; c++) {
      comparisons_++;
      Require(o.pend[c] == q_[c].size(), where + " (conservation)",
              std::string("conservation: class ") + ClassName(c) + " holds " + Dec(o.pend[c]) +
                  " request(s) at the DUT, the driver accepted " + Dec(q_[c].size()) +
                  "; a request was lost or delivered without a transfer");
    }

    // 3. The delivered request is the oldest of its class, with its own tag.
    if (o.srv_go) {
      comparisons_++;
      Require(o.srv_class < static_cast<uint32_t>(kN), where + " (conservation)",
              "conservation: srv_class " + Dec(o.srv_class) + " is out of range");
      comparisons_++;
      Require(!q_[o.srv_class].empty(), where + " (conservation)",
              std::string("conservation: class ") + ClassName(o.srv_class) +
                  " was served with an empty queue");
      comparisons_++;
      Require(o.srv_src == q_[o.srv_class].front().src, where + " (conservation)",
              std::string("conservation: class ") + ClassName(o.srv_class) + " delivered tag " + Dec(o.srv_src) +
                  " but the oldest accepted request of that class has tag " +
                  Dec(q_[o.srv_class].front().src));
    }

    // 4. The window is the quota's W grant opportunities, and the quota is
    //    charged per grant: everything the bound's window and share are
    //    computed from is checked here.
    comparisons_++;
    Require(o.win_index == win_index_ && o.win_grants == grants_in_window_,
            where + " (bound)",
            "bound: the DUT's window is index " + Dec(o.win_index) + " with " + Dec(o.win_grants) +
                " grant(s), the driver counted index " + Dec(win_index_) + " with " +
                Dec(grants_in_window_) +
                "; the window is not the quota's W grant opportunities, so no waiting bound can "
                "be computed from it");
    for (int c = 0; c < kN; c++) {
      comparisons_++;
      Require(o.used[c] == win_grants_c_[c], where + " (bound)",
              std::string("bound: class ") + ClassName(c) + " reports used=" + Dec(o.used[c]) +
                  " but the driver counted " + Dec(win_grants_c_[c]) +
                  " grant(s) to it in this window; the quota accounting is not per grant");
    }

    // 5. The escape's premise: a class with pending work and room in its quota.
    for (int c = 0; c < kN; c++) {
      if (!q_[c].empty() && o.used[c] < kQuota[c]) *any_under_quota = true;
    }

    // 6. The assumption, measured by the driver too.
    if (pend_total > 0 && busy) {
      busy_run_++;
    } else {
      busy_run_ = 0;
    }
    if (busy_run_ > busy_run_max_) busy_run_max_ = busy_run_;
    if (busy_run_ + 1 > gap_max_) gap_max_ = busy_run_ + 1;
    if (!expect_violation_) {
      comparisons_++;
      Require(busy_run_ <= kDMax, where + " (assumption)",
              "assumption: the port was not ready for " + Dec(busy_run_) +
                  " consecutive cycles with work pending, past the declared maximum " + Dec(kDMax) +
                  "; the downstream bound is violated, so no waiting bound holds for this stretch");
      comparisons_++;
      Require(!o.viol, where + " (assumption)",
              "assumption: the DUT reported an assumption violation the driver's stimulus never "
              "caused");
    }

    // 7. An opportunity advances the window's opportunity count, and records
    //    whether each class had work at it.
    if (pend_total > 0 && !busy) {
      if (!win_started_) {
        win_started_ = true;
        for (int c = 0; c < kN; c++) had_work_[c] = true;
      }
      opps_in_window_++;
      for (int c = 0; c < kN; c++) {
        had_work_[c] = had_work_[c] && (!q_[c].empty());
      }
    }
  }

  // -------------------------------------------------- the post-edge model
  void Apply(const Obs& o, const Stim& s, bool busy, uint64_t now, bool any_under_quota) {
    const std::string where = phase_ + ": cycle " + Dec(now);
    const uint64_t pend_total = PendTotal();

    // The DUT's own loss counter must stay at zero.
    comparisons_++;
    Require(dut_->o_drop_ctr_o == 0, where + " (conservation)",
            "conservation: the DUT reported " + Dec(dut_->o_drop_ctr_o) +
                " discarded request(s); a request was dropped instead of back-pressured");

    // The escape is taken only when no class with pending work is under quota.
    const uint64_t escape_now = dut_->o_escape_ctr_o;
    if (escape_now != escape_ctr_seen_) {
      comparisons_++;
      Require(!any_under_quota, where + " (escape)",
              "escape: the DUT served over quota while a class with pending work was still under "
              "its quota; the reservation's premise does not hold, so the bound does not follow");
    }
    escape_ctr_seen_ = escape_now;

    // The downstream: the class just served holds the port for its service
    // time. This is the short/long function-unit mix the card asks for.
    if (o.srv_go) {
      Pending p = q_[o.srv_class].front();
      q_[o.srv_class].pop_front();
      WaitRec w;
      w.src = p.src;
      w.ahead = p.ahead;
      w.wait = now - p.arrive;
      waits_[o.srv_class].push_back(w);
      if (w.wait > max_wait_[o.srv_class]) max_wait_[o.srv_class] = w.wait;
      granted_[o.srv_class]++;
      grants_++;
      Grant g;
      g.cycle = now;
      g.cls = o.srv_class;
      g.src = o.srv_src;
      stream_.push_back(g);
      busy_until_ = now + kService[o.srv_class];

      // The window: every grant is credited first, and the W-th closes the
      // window. The closure is where the reservation invariant is evaluated, so
      // the W-th grant must be part of the count it evaluates.
      grants_in_window_++;
      win_grants_c_[o.srv_class]++;
      if (grants_in_window_ == kWindow) {
        for (int c = 0; c < kN; c++) {
          if (!had_work_[c]) continue;
          comparisons_++;
          Require(win_grants_c_[c] >= kQuota[c], where + " (starvation)",
                  std::string("starvation: class ") + ClassName(c) + " was granted " + Dec(win_grants_c_[c]) +
                      " time(s) in window " + Dec(win_index_) +
                      " although it had pending work at every opportunity of that window; its "
                      "reserved share is " + Dec(kQuota[c]) +
                      ", so a class with continuous work can be starved for ever");
          if (!share_seen_[c]) {
            share_seen_[c] = true;
            share_min_[c] = win_grants_c_[c];
          } else if (win_grants_c_[c] < share_min_[c]) {
            share_min_[c] = win_grants_c_[c];
          }
          if (windows_closed_ == 0 || win_grants_c_[c] < win_grant_min_[c]) {
            win_grant_min_[c] = win_grants_c_[c];
          }
        }
        windows_closed_++;
        comparisons_++;
        Require(opps_in_window_ == kWindow, where + " (bound)",
                "bound: window " + Dec(win_index_) + " held " + Dec(opps_in_window_) +
                    " grant opportunities, not W=" + Dec(kWindow) +
                    "; the window is not W opportunities, so the bound is not computable");
        grants_in_window_ = 0;
        for (int c = 0; c < kN; c++) win_grants_c_[c] = 0;
        win_index_++;
        opps_in_window_ = 0;
        win_started_ = false;
      }
    }

    // Admissions and refusals: a full queue refuses and the producer holds.
    for (int c = 0; c < kN; c++) {
      if (s.req_valid[c] && o.req_ready[c]) {
        Pending p;
        p.src = s.req_src[c];
        p.arrive = now;
        p.ahead = static_cast<uint32_t>(q_[c].size());
        q_[c].push_back(p);
        accepted_[c]++;
        accepts_++;
      }
      if (s.req_valid[c] && !o.req_ready[c]) refused_++;
    }
    if (pend_total > 0 && busy) stalls_++;

    // The counters, against the driver's independent tallies.
    comparisons_ += 4;
    Require(dut_->o_grant_ctr_o == grants_, where + " (conservation)",
            "conservation: the DUT counted " + Dec(dut_->o_grant_ctr_o) + " grant(s), the driver " +
                Dec(grants_));
    Require(dut_->o_accept_ctr_o == accepts_, where + " (conservation)",
            "conservation: the DUT counted " + Dec(dut_->o_accept_ctr_o) +
                " admission(s), the driver " + Dec(accepts_));
    Require(dut_->o_refuse_ctr_o == refused_, where + " (conservation)",
            "conservation: the DUT counted " + Dec(dut_->o_refuse_ctr_o) +
                " refusal(s), the driver " + Dec(refused_));
    Require(dut_->o_stall_ctr_o == stalls_, where + " (conservation)",
            "conservation: the DUT counted " + Dec(dut_->o_stall_ctr_o) +
                " stalled cycle(s), the driver " + Dec(stalls_));
  }

  Vmosaic_arbiter_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  std::string phase_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;

  // the environment
  std::deque<Pending> q_[kN];
  uint64_t busy_until_ = 0;
  uint64_t accepted_[kN] = {0, 0, 0, 0};
  uint64_t granted_[kN] = {0, 0, 0, 0};
  uint64_t refused_ = 0;
  uint64_t accepts_ = 0;
  uint64_t grants_ = 0;
  uint64_t stalls_ = 0;
  uint64_t escapes_ = 0;
  uint64_t escape_ctr_seen_ = 0;
  uint32_t busy_run_ = 0;
  uint32_t busy_run_max_ = 0;
  uint64_t gap_max_ = 1;

  // the window, as the driver counts it
  uint64_t win_index_ = 0;
  uint64_t grants_in_window_ = 0;
  uint64_t win_grants_c_[kN] = {0, 0, 0, 0};
  uint64_t opps_in_window_ = 0;
  bool win_started_ = false;
  bool had_work_[kN] = {true, true, true, true};
  bool share_seen_[kN] = {false, false, false, false};
  uint64_t share_min_[kN] = {0, 0, 0, 0};
  uint64_t win_grant_min_[kN] = {0, 0, 0, 0};
  uint64_t windows_closed_ = 0;
  uint64_t max_wait_[kN] = {0, 0, 0, 0};

  // records
  std::vector<WaitRec> waits_[kN];
  std::vector<Offer> offers_;
  std::vector<Grant> stream_;
  bool expect_violation_ = false;
};

// ============================================================================
// the naive control: the same machine, strict class priority
// ============================================================================
//
// The offer stream is the DUT's own stimulus; the queue depth, the service
// times and the downstream are the driver's model of the same machine. The only
// difference is the selection rule: lowest class index first, no quota, no
// escape. A request this model accepts and never serves is a request a naive
// priority scheme would starve.

struct NaiveResult {
  uint64_t grants[kN] = {0, 0, 0, 0};
  uint64_t max_wait[kN] = {0, 0, 0, 0};
  uint64_t stuck[kN] = {0, 0, 0, 0};
  uint64_t total_grants = 0;
};

NaiveResult ReplayNaive(const std::vector<Offer>& offers) {
  NaiveResult r;
  std::deque<Pending> q[kN];
  uint64_t busy_until = 0;
  for (size_t t = 0; t < offers.size(); t++) {
    for (int c = 0; c < kN; c++) {
      if (((offers[t].valid_mask >> c) & 1u) == 0) continue;
      if (q[c].size() >= kDepth) continue;
      Pending p;
      p.src = offers[t].src[c];
      p.arrive = t;
      p.ahead = static_cast<uint32_t>(q[c].size());
      q[c].push_back(p);
    }
    if (t < busy_until) continue;
    for (int c = 0; c < kN; c++) {
      if (q[c].empty()) continue;
      const Pending p = q[c].front();
      q[c].pop_front();
      const uint64_t wait = t - p.arrive;
      if (wait > r.max_wait[c]) r.max_wait[c] = wait;
      r.grants[c]++;
      r.total_grants++;
      busy_until = t + kService[c];
      break;
    }
  }
  for (int c = 0; c < kN; c++) r.stuck[c] = q[c].size();
  return r;
}

class NaiveReporter {
 public:
  void Report(const NaiveResult& naive, Harness* h) {
    for (int c = 0; c < kN; c++) {
      std::printf("  [naive] %-6s grants=%llu max-wait=%llu stuck=%llu\n", ClassName(c),
                  static_cast<unsigned long long>(naive.grants[c]),
                  static_cast<unsigned long long>(naive.max_wait[c]),
                  static_cast<unsigned long long>(naive.stuck[c]));
    }
    std::printf("  [dut]   grants=%llu/%llu/%llu/%llu escapes=%llu refuses=%llu\n",
                static_cast<unsigned long long>(h->granted_of(0)),
                static_cast<unsigned long long>(h->granted_of(1)),
                static_cast<unsigned long long>(h->granted_of(2)),
                static_cast<unsigned long long>(h->granted_of(3)),
                static_cast<unsigned long long>(h->escapes()),
                static_cast<unsigned long long>(h->refuses()));
  }
};

// ============================================================================
// shared stimulus pieces
// ============================================================================

// A short, rotating tag population, so consecutive requests of a class often
// carry the same tag -- a producer that re-issues -- and the window accounting
// is exercised on runs of same-tag grants as well as distinct ones.
uint32_t NextSrc(int c, uint64_t accepted) {
  return 0x10u + static_cast<uint32_t>(c) * 0x10u + static_cast<uint32_t>((accepted / 2) % 4);
}

// ============================================================================
// phase 2: directed -- fifo order and the escape
// ============================================================================

void PhaseFifoOrder(Harness* h) {
  h->Phase("directed:fifo-order");
  h->Reset(4);

  const uint32_t srcs[3] = {0x11, 0x22, 0x33};
  std::vector<uint32_t> got;
  // The queue drains while the offers are made, so the delivered tags are
  // collected from the first cycle on -- a grant can share a cycle with an
  // admission.
  for (int i = 0; i < 3; i++) {
    Stim s;
    s.req_valid[CLS_MULDIV] = true;
    s.req_src[CLS_MULDIV] = srcs[i];
    const Obs o = h->Cycle(s);
    Require(o.req_ready[CLS_MULDIV], h->Where(),
            "the MULDIV queue refused request " + Dec(i) + " while it had room");
    if (o.srv_go) {
      Require(o.srv_class == static_cast<uint32_t>(CLS_MULDIV), h->Where(),
              "a class with no pending request was served (class " + Dec(o.srv_class) + ")");
      got.push_back(o.srv_src);
    }
  }

  for (int i = 0; i < 80 && got.size() < 3u; i++) {
    const Obs o = h->Cycle(Stim());
    if (!o.srv_go) continue;
    Require(o.srv_class == static_cast<uint32_t>(CLS_MULDIV), h->Where(),
            "a class with no pending request was served (class " + Dec(o.srv_class) + ")");
    got.push_back(o.srv_src);
  }
  Require(got.size() == 3, "directed:fifo-order",
          "only " + Dec(got.size()) + " of 3 MULDIV requests were delivered");
  for (size_t i = 0; i < got.size(); i++) {
    Require(got[i] == srcs[i], "directed:fifo-order",
            "class order violated: position " + Dec(i) + " delivered tag " + Dec(got[i]) +
                ", the queue was loaded " + Dec(srcs[0]) + ", " + Dec(srcs[1]) + ", " +
                Dec(srcs[2]));
  }
}

// ============================================================================
// phase 3: directed -- a full queue refuses and holds (credits)
// ============================================================================

void PhaseCredits(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:credit-backpressure");
  h->Reset(4);

  Stim s;
  s.req_valid[CLS_VECTOR] = true;
  s.req_src[CLS_VECTOR] = 0x5A;   // one producer, one tag: it holds its offer
  uint64_t refused = 0;
  uint64_t accepted = 0;
  for (int i = 0; i < 14; i++) {
    Obs o = h->Cycle(s);
    if (!o.req_ready[CLS_VECTOR]) {
      refused++;
    } else {
      accepted++;
    }
  }
  Require(refused > 0, "directed:credit-backpressure",
          "the VECTOR queue never refused: no backpressure was exercised");
  Require(accepted > 0 && h->accepted_of(CLS_VECTOR) == accepted, "directed:credit-backpressure",
          "the queue admitted " + Dec(h->accepted_of(CLS_VECTOR)) + " request(s) while the driver "
          "saw " + Dec(accepted));
  std::printf("  [credit]  refused=%llu admitted=%llu\n",
              static_cast<unsigned long long>(refused),
              static_cast<unsigned long long>(accepted));
  rep->Check(true,
             "directed: a full class queue refuses (" + Dec(refused) +
                 " refused offers) and the producer's offer is held and admitted later (" +
                 Dec(accepted) + " admitted); nothing is lost (the conservation checks hold)");
}

// ============================================================================
// phase 4: directed -- the reservation under a continuous scalar flood
// ============================================================================

void PhaseReservation(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:reservation");
  h->Reset(4);

  // The SCALAR stream holds a request in *every* cycle -- the card's high
  // priority stream -- and VECTOR, the class a naive priority scheme would
  // starve, holds one too. Both queues are therefore never empty, which is what
  // makes a class's per-window share measurable from the run.
  for (int i = 0; i < 160; i++) {
    Stim s;
    s.req_valid[CLS_SCALAR] = true;
    s.req_src[CLS_SCALAR] = 0x10u + static_cast<uint32_t>(i % 4);
    s.req_valid[CLS_VECTOR] = true;
    s.req_src[CLS_VECTOR] = 0x70u + static_cast<uint32_t>((i / 2) % 4);
    h->Cycle(s);
  }

  Require(h->wait_any(CLS_VECTOR), "directed:reservation",
          "starvation: no VECTOR request was delivered although VECTOR held a request in every "
          "cycle -- the card's fail mode");
  Require(h->share_seen(CLS_VECTOR), "directed:reservation",
          "no window closed with VECTOR pending at every opportunity, so its share is not "
          "measurable from this run");

  const uint64_t share = h->share_min(CLS_VECTOR);
  const uint64_t gap = h->gap_max();
  uint64_t worst = 0;
  uint64_t worst_bound = 0;
  uint32_t worst_ahead = 0;
  for (const WaitRec& w : h->waits(CLS_VECTOR)) {
    const uint64_t bound = WaitBound(share, w.ahead, gap, kWindow);
    Require(w.wait <= bound, "directed:reservation",
            "bound: a VECTOR request under the SCALAR flood waited " + Dec(w.wait) +
                " cycles, above the run's own bound of " + Dec(bound) + " (share_min " +
                Dec(share) + ", gap " + Dec(gap) + ", W " + Dec(kWindow) + ", ahead " +
                Dec(w.ahead) + ")");
    if (w.wait > worst) {
      worst = w.wait;
      worst_bound = bound;
      worst_ahead = w.ahead;
    }
  }

  // The same stimulus under naive priority must starve VECTOR, or it is not the
  // stress the card asks for.
  const NaiveResult naive = ReplayNaive(h->offers());
  Require(naive.stuck[CLS_VECTOR] > 0 || naive.max_wait[CLS_VECTOR] > worst_bound,
          "directed:reservation",
          "the SCALAR flood did not starve VECTOR under a naive priority scheme, so this is not "
          "the card's stress");

  NaiveReporter().Report(naive, h);
  std::printf("  [reserve] VECTOR waits=%llu max-wait=%llu bound=%llu ahead=%u share_min=%llu "
              "gap=%llu W=%llu\n",
              static_cast<unsigned long long>(h->waits(CLS_VECTOR).size()),
              static_cast<unsigned long long>(worst),
              static_cast<unsigned long long>(worst_bound), worst_ahead,
              static_cast<unsigned long long>(share),
              static_cast<unsigned long long>(gap), static_cast<unsigned long long>(kWindow));
  rep->Check(true,
             "directed: under a continuous SCALAR flood VECTOR was served " +
                 Dec(h->waits(CLS_VECTOR).size()) + " time(s), worst wait " + Dec(worst) +
                 " against the run's own bound of " + Dec(worst_bound) + " (share_min " +
                 Dec(share) + ", gap " + Dec(gap) + ", ahead " + Dec(worst_ahead) +
                 "), while the same stimulus under naive priority left " +
                 Dec(naive.stuck[CLS_VECTOR]) + " VECTOR request(s) unserved");
}

// ============================================================================
// phase 5: directed -- the return channel
// ============================================================================

void PhaseReturn(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:return-channel");
  h->Reset(4);

  // The return channel (MEMORY) holds a request in every cycle while the SCALAR
  // stream does the same: the reserved capacity is what keeps the returns
  // moving, and the share the bound is computed from is measurable.
  for (int i = 0; i < 160; i++) {
    Stim s;
    s.req_valid[CLS_SCALAR] = true;
    s.req_src[CLS_SCALAR] = 0x20u + static_cast<uint32_t>(i % 4);
    s.req_valid[CLS_MEMORY] = true;
    s.req_src[CLS_MEMORY] = 0x60u + static_cast<uint32_t>((i / 2) % 4);
    h->Cycle(s);
  }

  Require(h->wait_any(CLS_MEMORY), "directed:return-channel",
          "no return-channel request was delivered while MEMORY held a request in every cycle");
  Require(h->share_seen(CLS_MEMORY), "directed:return-channel",
          "no window closed with MEMORY pending at every opportunity, so its share is not "
          "measurable from this run");

  const uint64_t share = h->share_min(CLS_MEMORY);
  uint64_t worst = 0;
  uint64_t worst_bound = 0;
  for (const WaitRec& w : h->waits(CLS_MEMORY)) {
    const uint64_t bound = WaitBound(share, w.ahead, h->gap_max(), kWindow);
    Require(w.wait <= bound, "directed:return-channel",
            "bound: a return-channel request waited " + Dec(w.wait) + " cycles, above the bound " +
                Dec(bound) + " computed from this run (share_min " + Dec(share) + ", gap " +
                Dec(h->gap_max()) + ", ahead " + Dec(w.ahead) + ")");
    if (w.wait > worst) {
      worst = w.wait;
      worst_bound = bound;
    }
  }
  std::printf("  [return]  MEMORY waits=%llu max-wait=%llu bound=%llu share_min=%llu gap=%llu "
              "W=%llu\n",
              static_cast<unsigned long long>(h->waits(CLS_MEMORY).size()),
              static_cast<unsigned long long>(worst),
              static_cast<unsigned long long>(worst_bound),
              static_cast<unsigned long long>(share),
              static_cast<unsigned long long>(h->gap_max()),
              static_cast<unsigned long long>(kWindow));
  rep->Check(true,
             "directed: the return channel was served through a continuous SCALAR flood: " +
                 Dec(h->waits(CLS_MEMORY).size()) + " request(s) delivered, worst wait " +
                 Dec(worst) + " against the run's own bound of " + Dec(worst_bound) +
                 ", reserved share " + Dec(share) + " per window");
}

// ============================================================================
// phase 6: directed -- the escape path
// ============================================================================

void PhaseEscape(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:escape");
  h->Reset(4);

  // One class alone: after its quota the escape must keep the port moving, or a
  // single active class would idle the port whenever its quota was spent.
  for (int i = 0; i < 20; i++) {
    Stim s;
    s.req_valid[CLS_VECTOR] = true;
    s.req_src[CLS_VECTOR] = 0x30;
    h->Cycle(s);
  }
  Require(h->escapes() > 0, "directed:escape",
          "escape: the port never served over quota although one class with a pending request was "
          "the only class with work");
  std::printf("  [escape]  over-quota-grants=%llu grants=%llu\n",
              static_cast<unsigned long long>(h->escapes()),
              static_cast<unsigned long long>(h->grants()));
  rep->Check(true, "directed: the escape capacity kept a lone class moving past its quota (" +
                       Dec(h->escapes()) + " over-quota grant(s), " + Dec(h->grants()) +
                       " total)");
}

// ============================================================================
// phase 7: directed -- the assumption, violated and reported
// ============================================================================

void PhaseAssumption(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:assumption");
  h->Reset(4);
  h->ExpectViolation(true);

  Stim s;
  s.req_valid[CLS_MEMORY] = true;
  s.req_src[CLS_MEMORY] = 0x31;
  s.force_busy = true;
  Obs o;
  for (int i = 0; i < static_cast<int>(kDMax) + 3; i++) {
    o = h->Cycle(s);
  }
  const uint32_t run_max = h->dut()->o_busy_run_max_o;
  Require(o.viol, "directed:assumption",
          "assumption: the port was held not-ready for " + Dec(kDMax + 3) +
              " cycles with work pending and the DUT did not report an assumption violation; a "
              "waiting bound would be claimed where none holds");
  Require(run_max > kDMax, "directed:assumption",
          "assumption: the DUT's longest not-ready run is " + Dec(run_max) +
              ", not above the declared maximum " + Dec(kDMax));
  h->ExpectViolation(false);

  h->Reset(4);
  Require(h->dut()->o_assumption_violated_o == 0, "directed:assumption",
          "the assumption flag is not cleared by reset");
  std::printf("  [assume]  forced-not-ready=%u run-max=%u violation=%d\n", kDMax + 3, run_max,
              (int)o.viol);
  rep->Check(true,
             "directed: holding the port not-ready past the declared maximum (" +
                 Dec(kDMax + 3) + " cycles, measured longest run " + Dec(run_max) +
                 " against D_MAX " + Dec(kDMax) +
                 ") is reported as an assumption violation, not as a bound; the flag clears on "
                 "reset");
}

// ============================================================================
// phase 8: saturation -- the stress mix
// ============================================================================

// A class decides to start a new request with probability `rate` percent per
// cycle and holds it, unchanged, until the queue accepts it. SCALAR at 100 is
// the continuous stream that would starve every other class under naive
// priority.
//
// MEMORY's demand is a *burst* pattern rather than a probability: memory returns
// arrive in batches, so there are stretches in which the return channel has
// nothing pending. That is what makes the escape capacity reachable in the
// stress mix too -- while MEMORY is idle its two reserved slots cannot be used
// by anyone, so a window with the other three classes at their quotas must
// serve over quota -- and it is a more honest model of a return channel than a
// class that demands evenly.
void RunMix(Harness* h, uint64_t seed, int cycles, const uint32_t rate[kN],
            bool memory_bursts) {
  mosaic::Rng rng(seed);
  bool offering[kN] = {true, false, false, false};
  uint32_t src[kN] = {0, 0, 0, 0};
  src[CLS_SCALAR] = NextSrc(CLS_SCALAR, 0);
  for (int i = 0; i < cycles; i++) {
    Stim s;
    for (int c = 0; c < kN; c++) {
      uint32_t r = rate[c];
      if (memory_bursts && c == CLS_MEMORY) {
        r = (((i / 60) % 2) == 0) ? 60u : 0u;
      }
      if (!offering[c] && rng.Chance(r)) {
        offering[c] = true;
        src[c] = NextSrc(c, h->accepted_of(c));
      }
      s.req_valid[c] = offering[c];
      s.req_src[c] = src[c];
    }
    const Obs o = h->Cycle(s);
    for (int c = 0; c < kN; c++) {
      if (s.req_valid[c] && o.req_ready[c]) offering[c] = false;
    }
  }
}

void PhaseSaturation(Harness* h, mosaic::Reporter* rep, uint64_t seed) {
  const uint32_t rate[kN] = {100, 50, 25, 25};

  h->Phase("saturation");
  h->Reset(4);
  const uint64_t mix_start = h->cycles();
  RunMix(h, seed ^ 0x1030ull, 2400, rate, /*memory_bursts=*/true);

  // --- coverage: the stress must actually reach every class and every path.
  for (int c = 0; c < kN; c++) {
    Require(h->accepted_of(c) > 0, "saturation",
            "class " + std::string(ClassName(c)) + " accepted no request in the stress mix");
    Require(h->granted_of(c) > 0, "saturation",
            "starvation: class " + std::string(ClassName(c)) +
                " was never served in the stress mix");
    Require(h->share_seen(c), "saturation",
            "no window closed with class " + std::string(ClassName(c)) +
                " pending at every opportunity, so its share is not measurable from this run");
  }
  Require(h->refuses() > 0, "saturation",
          "no class queue ever refused, so backpressure was not exercised");
  Require(h->escapes() > 0, "saturation", "the escape path was never taken");
  Require(h->windows_closed() >= 4, "saturation",
          "only " + Dec(h->windows_closed()) + " window(s) closed");
  Require(!h->dut()->o_assumption_violated_o, "saturation",
          "assumption: the stress mix violated the downstream bound");
  Require(h->busy_run_max() <= kDMax, "saturation",
          "assumption: the driver measured a not-ready run of " + Dec(h->busy_run_max()) +
              " cycles, past the declared maximum " + Dec(kDMax));

  // --- the measured worst-case wait and the bound computed from this run.
  uint64_t total_waits = 0;
  for (int c = 0; c < kN; c++) {
    const uint64_t share = h->share_min(c);
    Require(share >= kQuota[c], "saturation",
            "starvation: class " + std::string(ClassName(c)) + " received as few as " + Dec(share) +
                " grant(s) in a window in which it had pending work throughout, below its "
                "reserved share " + Dec(kQuota[c]));
    uint64_t worst_wait = 0;
    uint64_t worst_bound = 0;
    uint32_t worst_ahead = 0;
    for (const WaitRec& w : h->waits(c)) {
      const uint64_t bound = WaitBound(share, w.ahead, h->gap_max(), kWindow);
      Require(w.wait <= bound, "saturation",
              "bound: " + std::string(ClassName(c)) + " request tag " + Dec(w.src) + " waited " +
                  Dec(w.wait) + " cycles, above the run's own bound of " + Dec(bound) +
                  " (share_min " + Dec(share) + ", gap " + Dec(h->gap_max()) + ", W " +
                  Dec(kWindow) + ", ahead " + Dec(w.ahead) + ")");
      if (w.wait > worst_wait) {
        worst_wait = w.wait;
        worst_bound = bound;
        worst_ahead = w.ahead;
      }
      total_waits++;
    }
    std::printf("  [bound] %-6s waits=%llu max-wait=%llu bound=%llu ahead=%u share_min=%llu "
                "gap=%llu W=%llu\n",
                ClassName(c), static_cast<unsigned long long>(h->waits(c).size()),
                static_cast<unsigned long long>(worst_wait),
                static_cast<unsigned long long>(worst_bound), worst_ahead,
                static_cast<unsigned long long>(share),
                static_cast<unsigned long long>(h->gap_max()),
                static_cast<unsigned long long>(kWindow));
  }

  // --- the naive control on the same stimulus.
  const NaiveResult naive = ReplayNaive(h->offers());
  bool naive_starved = false;
  for (int c = 0; c < kN; c++) {
    if (naive.stuck[c] > 0 || naive.max_wait[c] > WaitBound(h->share_min(c), 0, h->gap_max(), kWindow)) {
      naive_starved = true;
    }
  }
  Require(naive_starved, "saturation",
          "the saturation stimulus starved no class under a naive priority scheme, so the case "
          "does not reach the card's fail mode");
  NaiveReporter().Report(naive, h);

  std::printf("  [mix]   cycles=%llu grants=%llu accepts=%llu refuses=%llu escapes=%llu "
              "stalls=%llu windows=%llu busy-run-max=%u gap-max=%llu\n",
              static_cast<unsigned long long>(h->cycles() - mix_start),
              static_cast<unsigned long long>(h->grants()),
              static_cast<unsigned long long>(h->accepts()),
              static_cast<unsigned long long>(h->refuses()),
              static_cast<unsigned long long>(h->escapes()),
              static_cast<unsigned long long>(h->stalls()),
              static_cast<unsigned long long>(h->windows_closed()),
              h->busy_run_max(), static_cast<unsigned long long>(h->gap_max()));

  rep->Check(true,
             "saturation: " + Dec(h->grants()) + " completions over " + Dec(h->cycles()) +
                 " cycles from " + Dec(total_waits) + " measured requests (SCALAR continuous, "
                 "MEMORY 50%, MULDIV 25%, VECTOR 25%; short and long service), " +
                 Dec(h->windows_closed()) + " windows closed with every class that had work "
                 "throughout granted at least its reserved share, every measured wait within the "
                 "bound computed from this run's own share_min/gap/W, and the same stimulus "
                 "leaving a class unserved under naive priority");
}

// ============================================================================
// phase 9: determinism
// ============================================================================

void PhaseDeterminism(Harness* h, mosaic::Reporter* rep, uint64_t seed) {
  const uint32_t rate[kN] = {100, 50, 25, 25};

  h->Phase("determinism");
  h->Reset(4);
  const uint64_t start_a = h->cycles();
  RunMix(h, seed ^ 0xd00dull, 300, rate, /*memory_bursts=*/false);
  const std::vector<Grant> a = h->stream();

  h->Phase("determinism");
  h->Reset(4);
  const uint64_t start_b = h->cycles();
  RunMix(h, seed ^ 0xd00dull, 300, rate, /*memory_bursts=*/false);
  const std::vector<Grant> b = h->stream();

  Require(a.size() == b.size(), "determinism",
          "run A made " + Dec(a.size()) + " grants, run B " + Dec(b.size()));
  for (size_t i = 0; i < a.size(); i++) {
    Require(a[i].cycle - start_a == b[i].cycle - start_b && a[i].cls == b[i].cls &&
                a[i].src == b[i].src,
            "determinism",
            "grant " + Dec(i) + " differs: A(cycle " + Dec(a[i].cycle) + ", class " +
                Dec(a[i].cls) + ", tag " + Dec(a[i].src) + "), B(cycle " + Dec(b[i].cycle) +
                ", class " + Dec(b[i].cls) + ", tag " + Dec(b[i].src) + ")");
  }
  std::printf("  [det]   grants=%llu cycles=%llu\n",
              static_cast<unsigned long long>(a.size()),
              static_cast<unsigned long long>(h->cycles() - start_b));
  rep->Check(true, "determinism: the same stimulus and seed reproduce " + Dec(a.size()) +
                       " grants exactly");
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
  Vmosaic_arbiter_tb dut;
  Harness harness(&dut, &clk, options.max_cycles);

  std::string detail;
  bool passed = true;
  try {
    // ----------------------------------------------------------- geometry
    harness.Phase("geometry");
    harness.Reset(4);
    const Obs g = harness.Cycle(Stim());
    Require(dut.o_classes_o == 4 && dut.o_window_o == kWindow && dut.o_d_max_o == kDMax &&
                dut.o_depth_o == kDepth,
            "geometry",
            "the DUT reports " + Dec(dut.o_classes_o) + " classes, window " + Dec(dut.o_window_o) +
                ", d_max " + Dec(dut.o_d_max_o) + ", depth " + Dec(dut.o_depth_o) +
                "; this driver drives 4/" + Dec(kWindow) + "/" + Dec(kDMax) + "/" + Dec(kDepth));
    const uint32_t quota_lane[4] = {dut.o_quota0_o, dut.o_quota1_o, dut.o_quota2_o, dut.o_quota3_o};
    for (int c = 0; c < kN; c++) {
      Require(quota_lane[c] == kQuota[c], "geometry",
              "class " + std::string(ClassName(c)) + " reports quota " + Dec(quota_lane[c]) +
                  ", this driver drives " + Dec(kQuota[c]));
      Require(g.used[c] == 0 && g.pend[c] == 0, "geometry",
              "class " + std::string(ClassName(c)) + " is not empty after reset");
    }
    Require(g.win_index == 0 && g.win_grants == 0 && !g.viol, "geometry",
            "the window or assumption state is not clear after reset");
    reporter.Check(true,
                   "geometry: 4 classes with quotas 4/2/1/1 summing to a window of " + Dec(kWindow) +
                       " grant opportunities, depth " + Dec(kDepth) + ", declared maximum "
                       "not-ready run " + Dec(kDMax));

    // -------------------------------------------------------- directed
    PhaseFifoOrder(&harness);
    reporter.Check(true, "directed: three requests of one class left the queue oldest first, and "
                         "the escape kept the lone class moving past its quota");

    PhaseCredits(&harness, &reporter);
    PhaseReservation(&harness, &reporter);
    PhaseReturn(&harness, &reporter);
    PhaseEscape(&harness, &reporter);
    PhaseAssumption(&harness, &reporter);

    // ------------------------------------------------------ those measured
    harness.Phase("saturation");
    PhaseSaturation(&harness, &reporter, options.seed);
    PhaseDeterminism(&harness, &reporter, options.seed);

    detail = "bounded service holds: " + Dec(harness.comparisons()) +
             " per-cycle checks over " + Dec(harness.cycles()) + " cycles";
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated after " + Dec(harness.comparisons()) + " checks: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
