// ============================================================================
// tb_qos.cpp -- CASE=memory.qos_no_starvation, work package I-062.
//
// The DUT is `mosaic_mem_qos`: the MEF criticality/QoS memory request
// scheduler. The driver is the environment it serves -- three request
// identities (a latency-critical SCALAR dependent load, a PREFETCH placeholder,
// and BULK streaming traffic) and a memory port that accepts one request per
// ready cycle and is then busy for a class-dependent number of cycles. The
// driver is never the DUT's oracle for the *choice* where the policy defines
// it: it checks the invariants the policy's bound rests on, it measures the
// bound from the run's own numbers, and it owns the fail modes the card names.
//
// ---------------------------------------------------------------- the case
//
// The card asks for a criticality/QoS memory scheduler, the service assumption
// written down, and a case that shows:
//
//   * a scalar *dependent chain* -- a load whose result feeds the next load's
//     address -- driven against sustained bulk streaming;
//   * every class's p50/p99 latency measured from the run;
//   * the bulk class starving the scalar chain with the policy off, so the
//     stimulus provably reaches a fail mode;
//   * the data result identical with the policy off and on (a QoS policy is a
//     scheduling choice, not a memory model);
//   * the age-based escape firing for an old request in the bulk class;
//   * every class exercised, and every measured wait within a declared bound
//     under a checked service assumption.
//
// ------------------------------------------------------------------ checks
//
// Every cycle, in every phase, the driver checks:
//
//   data          the payload delivered with a grant is the payload of the
//                 request it is delivered for -- a scheduling choice may change
//                 *when* a request is served, never *what* it returns.
//   conservation  the DUT's per-class occupancy equals the driver's own
//                 accepted-but-not-served queue; the served request is the
//                 *oldest* accepted request of its class with its own tag; the
//                 DUT's own loss counter stays zero; the grant/accept/refuse/
//                 stall counters agree with the driver's independent tallies.
//   bound         a normal grant never charges a class already at its ceiling,
//                 and an over-quota grant is always an age escape for a request
//                 that actually aged. The window -- the quota's W grants -- is
//                 the object every waiting bound is computed from.
//   escape        the age escape is taken only when no class with pending work
//                 is under its quota *and* the request it serves has aged; and
//                 the port never idles with an aged request pending.
//   starvation    at every window close, every class that had pending work at
//                 every grant of that window was granted at least its quota;
//                 and, earlier, no such class can already be unable to reach
//                 its quota in the rest of the window. This is the check that
//                 fails loudly when a class is starved indefinitely.
//   assumption    with work pending, the port is not ready for more than the
//                 declared QOS_D_MAX consecutive cycles, and the DUT never
//                 reports a violation the stimulus did not cause.
//
// --------------------------------------------------------- the bound, and
// ------------------------------------------- why it is computed from the rule
//
// The policy guarantees: a class with pending work at every grant of a window
// is granted at least Q[class] times in it. A request with n requests of its
// class ahead of it is therefore granted within ceil((n + 1) / Q[class]) + 1
// windows, and a window of W grants takes at most W * (D_MAX + 1) cycles. A
// request that instead reaches the age trigger is served once it ages. The
// declared bound is the sum of the two, computed from the module's own
// constants (read back from the DUT):
//
//     AGE_LIMIT + (D_MAX + 1) * ( N*DEPTH + W * (ceil(DEPTH / min Q) + 1) )
//
// and every measured wait is checked against it. The per-window reservation is
// checked against the run's own share_min as well.
//
// ---------------------------------------------------------------- the naive
// ---------------------------------------------------------------- control
//
// A stress case that no scheme could starve proves nothing, so the driver
// replays the *same offer stream* through a second model of the same machine --
// same queue depth, same service times, same downstream -- whose only
// difference is the selection rule: throughput-first (BULK, then PREFETCH, then
// SCALAR), no quota, no age. The run fails if that model starves no class, so
// "this stimulus reaches the card's fail mode" is measured rather than claimed.
// The same fail mode is demonstrated on the DUT itself with `qos_en = 0`: the
// dependent chain stops advancing while bulk streams.
//
// ------------------------------------------------------------------ phases
//
//   1. geometry              the policy's read-back is the policy this driver
//                            drives.
//   2. directed:fifo         three requests of one class leave in order, each
//                            carrying its own payload.
//   3. directed:credit       a full queue refuses and holds: backpressure, not
//                            loss.
//   4. directed:reservation  SCALAR and BULK continuously pending: SCALAR gets
//                            its floor and BULK no more than its ceiling,
//                            every wait within the declared bound.
//   5. directed:age_escape   BULK alone saturates its ceiling: the port idles
//                            until an old request ages, then the age escape
//                            serves it -- the mechanism the card names.
//   6. directed:assumption   the port held not-ready past D_MAX is reported as
//                            an assumption violation, not as a bound.
//   7. chain                 the scalar dependent chain against sustained bulk,
//                            policy on: p50/p99 per class, all waits bounded,
//                            every class exercised.
//   8. naive                 the same chain and bulk with qos_en = 0: bulk
//                            starves the scalar chain (the fail mode).
//   9. data_identity         one fixed mixed stream, policy off and on: the
//                            delivered data is identical both ways and equal to
//                            what was offered; the schedule differs.
//  10. saturation            the stress mix: every class a continuous stream,
//                            2400 cycles, the reservation and bound checks, the
//                            naive model control.
//  11. determinism           the same stimulus and seed reproduce the grant
//                            stream exactly.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_qos_tb.h"

namespace {

// ================================================================ constants
//
// The request classes and the machine the driver serves them into. These are
// the driver's environment: the policy's own geometry is read back from the DUT
// and checked against them.

constexpr int kN = 3;
enum { CLS_SCALAR = 0, CLS_PREFETCH = 1, CLS_BULK = 2 };

constexpr uint32_t kQuota[kN] = {4, 2, 2};
constexpr uint32_t kWindow = 8;
constexpr uint32_t kDepth = 4;
constexpr uint32_t kService[kN] = {1, 2, 2};
constexpr uint32_t kDMax = 4;
constexpr uint32_t kAgeLimit = 32;
constexpr uint32_t kMinQuota = 2;

const char* kName[kN] = {"SCALAR", "PREFETCH", "BULK"};
const char* ClassName(uint32_t c) { return c < static_cast<uint32_t>(kN) ? kName[c] : "?"; }

constexpr uint64_t CeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// The declared waiting bound, computed from the module's own constants (see the
// header). Every measured wait is checked against it.
constexpr uint64_t kBound =
    kAgeLimit + static_cast<uint64_t>(kDMax + 1) *
                    (kN * kDepth + kWindow * (CeilDiv(kDepth, kMinQuota) + 1));

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

// ============================================================ the stimulus

struct Stim {
  bool req_valid[kN] = {false, false, false};
  uint8_t req_tag[kN] = {0, 0, 0};
  uint32_t req_data[kN] = {0, 0, 0};
  // The directed assumption phase holds the port not-ready by hand; everywhere
  // else the driver's downstream model is the only source of `srv_busy`.
  bool force_busy = false;
};

// One accepted request, waiting for its class's queue to reach it.
struct Pending {
  uint8_t tag = 0;
  uint32_t data = 0;
  uint64_t arrive = 0;
  uint32_t ahead = 0;
};

struct WaitRec {
  uint8_t tag = 0;
  uint32_t ahead = 0;
  uint64_t wait = 0;
};

struct Grant {
  uint64_t cycle = 0;
  uint32_t cls = 0;
  uint8_t tag = 0;
  uint32_t data = 0;
  bool escape = false;
};

// The offer stream, recorded so the naive control can be replayed over exactly
// the stimulus the DUT saw.
struct Offer {
  uint8_t valid_mask = 0;
  uint8_t tag[kN] = {0, 0, 0};
  uint32_t data[kN] = {0, 0, 0};
};

// ================================================================= observed

struct Obs {
  bool srv_go = false;
  uint32_t srv_class = 0;
  uint8_t srv_tag = 0;
  uint32_t srv_data = 0;
  bool req_ready[kN] = {false, false, false};
  uint32_t used[kN] = {0, 0, 0};
  uint32_t pend[kN] = {0, 0, 0};
  uint32_t win_index = 0;
  uint32_t win_grants = 0;
  bool viol = false;
};

// ============================================================================
// the harness: the DUT plus the environment it is measured in
// ============================================================================

class Harness {
 public:
  Harness(Vmosaic_qos_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  Vmosaic_qos_tb* dut() { return dut_; }
  void Phase(const std::string& name) { phase_ = name; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }
  std::string Where() const { return phase_ + ": cycle " + Dec(cycles_); }

  // The policy selector is a phase property, not a per-cycle one.
  void SetPolicy(bool on) { qos_en_ = on; }

  // ------------------------------------------------------- measured numbers
  uint64_t granted_of(int c) const { return granted_[c]; }
  uint64_t accepted_of(int c) const { return accepted_[c]; }
  uint64_t grants() const { return grants_; }
  uint64_t accepts() const { return accepts_; }
  uint64_t refuses() const { return refused_; }
  uint64_t stalls() const { return stalls_; }
  uint64_t idles() const { return idle_ctr_seen_; }
  uint64_t escapes() const { return escape_ctr_seen_; }
  uint64_t busy_run_max() const { return busy_run_max_; }
  bool share_seen(int c) const { return share_seen_[c]; }
  uint64_t share_min(int c) const { return share_min_[c]; }
  const std::vector<WaitRec>& waits(int c) const { return waits_[c]; }
  bool wait_any(int c) const { return !waits_[c].empty(); }
  const std::vector<Offer>& offers() const { return offers_; }
  const std::vector<Grant>& stream() const { return stream_; }
  uint64_t windows_closed() const { return windows_closed_; }
  uint64_t pend_total() const { return PendTotal(); }

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
    dut_->qos_en_i = qos_en_ ? 1 : 0;
    uint8_t mask = 0;
    for (int c = 0; c < kN; c++) {
      if (s_in.req_valid[c]) mask = static_cast<uint8_t>(mask | (1u << c));
    }
    dut_->req_valid_i = mask;
    dut_->req_tag0_i = s_in.req_tag[0];
    dut_->req_tag1_i = s_in.req_tag[1];
    dut_->req_tag2_i = s_in.req_tag[2];
    dut_->req_data0_i = s_in.req_data[0];
    dut_->req_data1_i = s_in.req_data[1];
    dut_->req_data2_i = s_in.req_data[2];
    dut_->srv_busy_i = busy ? 1 : 0;
    dut_->eval();

    Obs o;
    if (!rst) {
      Capture(&o);
      Check(o, s_in, busy, now);
      Offer off;
      off.valid_mask = mask;
      for (int c = 0; c < kN; c++) {
        off.tag[c] = s_in.req_tag[c];
        off.data[c] = s_in.req_data[c];
      }
      offers_.push_back(off);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();

    if (!rst) Apply(o, s_in, busy, now);
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
    }
    busy_until_ = 0;
    refused_ = 0;
    accepts_ = 0;
    grants_ = 0;
    stalls_ = 0;
    busy_run_ = 0;
    busy_run_max_ = 0;
    idle_run_ = 0;
    escape_ctr_seen_ = 0;
    idle_ctr_seen_ = 0;
    win_index_ = 0;
    grants_in_window_ = 0;
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
    o->srv_tag = static_cast<uint8_t>(dut_->srv_tag_o & 0xffu);
    o->srv_data = dut_->srv_data_o;
    o->req_ready[0] = (dut_->req_ready_o & 1u) != 0;
    o->req_ready[1] = (dut_->req_ready_o & 2u) != 0;
    o->req_ready[2] = (dut_->req_ready_o & 4u) != 0;
    o->used[0] = dut_->o_used0_o;
    o->used[1] = dut_->o_used1_o;
    o->used[2] = dut_->o_used2_o;
    o->pend[0] = dut_->o_pend0_o;
    o->pend[1] = dut_->o_pend1_o;
    o->pend[2] = dut_->o_pend2_o;
    o->win_index = dut_->o_window_index_o;
    o->win_grants = dut_->o_window_grants_o;
    o->viol = dut_->o_assumption_violated_o != 0;
  }

  uint64_t PendTotal() const {
    uint64_t total = 0;
    for (int c = 0; c < kN; c++) total += q_[c].size();
    return total;
  }

  bool DriverEligAny() const {
    for (int c = 0; c < kN; c++) {
      if (!q_[c].empty() && win_grants_c_[c] < kQuota[c]) return true;
    }
    return false;
  }

  // ------------------------------------------------- the pre-edge contract
  void Check(const Obs& o, const Stim& s, bool busy, uint64_t now) {
    const std::string where = phase_ + ": cycle " + Dec(now);
    const uint64_t pend_total = PendTotal();

    // 1. The payload delivered with a grant is the payload of the request it is
    //    delivered for. This is checked first: a scheduling policy may change
    //    *when* a request is served, never *what* it returns.
    if (o.srv_go) {
      comparisons_++;
      Require(driver_has_head(o), where + " (data)",
              std::string("data: class ") + ClassName(o.srv_class) +
                  " was served with an empty driver queue");
      if (driver_has_head(o)) {
        Require(o.srv_data == q_[o.srv_class].front().data, where + " (data)",
                "data: class " + std::string(ClassName(o.srv_class)) + " delivered tag " +
                    Dec(o.srv_tag) + " with payload " + Dec(o.srv_data) +
                    ", the request with that tag was offered with payload " +
                    Dec(q_[o.srv_class].front().data) +
                    "; a scheduling choice changed a data result");
      }
    }

    if (qos_en_) {
      // 2. The window is the quota's W grants, and the quota is charged per
      //    grant: everything the bound's window and share are computed from.
      comparisons_++;
      Require(o.win_index == win_index_ && o.win_grants == grants_in_window_,
              where + " (bound)",
              "bound: the DUT's window is index " + Dec(o.win_index) + " with " + Dec(o.win_grants) +
                  " grant(s), the driver counted index " + Dec(win_index_) + " with " +
                  Dec(grants_in_window_) +
                  "; the window is not the quota's W grants, so no waiting bound can be computed "
                  "from it");
      for (int c = 0; c < kN; c++) {
        comparisons_++;
        Require(o.used[c] == win_grants_c_[c], where + " (bound)",
                "bound: class " + std::string(ClassName(c)) + " reports used=" + Dec(o.used[c]) +
                    " but the driver counted " + Dec(win_grants_c_[c]) +
                    " grant(s) to it in this window; the quota accounting is not per grant");
      }

      // 3. On a grant, the window's "work at every grant" bookkeeping is
      //    updated, and early starvation is checked: a class that had work at
      //    every grant so far and is still pending must still be able to reach
      //    its quota in what is left of the window. Under this policy the
      //    equality is a theorem (see mosaic_mem_qos.sv), so this can only fire
      //    when capacity has been taken from a class. The DUT's own escape flag
      //    is read after the edge (in Apply), where it is observable.
      if (o.srv_go) {
        const uint64_t after_grants = grants_in_window_ + 1;
        const uint32_t c = o.srv_class;
        last_elig_any_ = DriverEligAny();
        if (grants_in_window_ == 0) {
          for (int j = 0; j < kN; j++) had_work_[j] = true;
        }
        for (int j = 0; j < kN; j++) {
          had_work_[j] = had_work_[j] && (!q_[j].empty());
        }
        for (int j = 0; j < kN; j++) {
          if (!had_work_[j] || q_[j].empty()) continue;
          const uint64_t remaining = kWindow > after_grants ? kWindow - after_grants : 0;
          const uint64_t got = win_grants_c_[j] + (static_cast<uint32_t>(j) == c ? 1u : 0u);
          comparisons_++;
          Require(got + remaining >= kQuota[j], where + " (starvation)",
                  "starvation: class " + std::string(ClassName(j)) +
                      " had pending work at every grant so far this window and is still pending, "
                      "but " + Dec(got) + " grant(s) with " + Dec(remaining) +
                      " opportunity(ies) left cannot reach its reserved share " + Dec(kQuota[j]) +
                      "; a class with continuous work can be starved for ever");
        }
      }

      // 4. The port must not idle with an aged request pending: that is the age
      //    escape failing to fire.
      if (pend_total > 0 && !busy && !o.srv_go && !DriverEligAny()) {
        idle_run_++;
      } else {
        idle_run_ = 0;
      }
      if (!expect_violation_ && driver_oldest_age(now) >= kAgeLimit + 2) {
        comparisons_++;
        Require(idle_run_ <= kBound, where + " (escape)",
                "escape: the port has been idle for " + Dec(idle_run_) +
                    " consecutive cycles with a request that has aged past the trigger pending; "
                    "the age escape did not fire, so the request has no waiting bound");
      }
    }

    // 5. Conservation does not assume the rule. A port may be idle (the policy
    //    idles when every pending class is at its ceiling and nothing has aged),
    //    so "ready with work must transfer" is *not* asserted: liveness is
    //    checked by the age-escape bound above and by each phase's own end
    //    condition (every offered request is delivered). What is checked here is
    //    that no accepted request is lost: occupancy, FIFO order, counters.

    // 6. The DUT's occupancy is the driver's queue.
    for (int c = 0; c < kN; c++) {
      comparisons_++;
      Require(o.pend[c] == q_[c].size(), where + " (conservation)",
              std::string("conservation: class ") + ClassName(c) + " holds " + Dec(o.pend[c]) +
                  " request(s) at the DUT, the driver accepted " + Dec(q_[c].size()) +
                  "; a request was lost or delivered without a transfer");
    }

    // 7. The delivered request is the oldest of its class, with its own tag.
    if (o.srv_go) {
      comparisons_++;
      Require(o.srv_class < static_cast<uint32_t>(kN), where + " (conservation)",
              "conservation: srv_class " + Dec(o.srv_class) + " is out of range");
      comparisons_++;
      Require(!q_[o.srv_class].empty(), where + " (conservation)",
              std::string("conservation: class ") + ClassName(o.srv_class) +
                  " was served with an empty queue");
      comparisons_++;
      Require(o.srv_tag == q_[o.srv_class].front().tag, where + " (conservation)",
              std::string("conservation: class ") + ClassName(o.srv_class) + " delivered tag " +
                  Dec(o.srv_tag) + " but the oldest accepted request of that class has tag " +
                  Dec(q_[o.srv_class].front().tag) + "; the class queue is not FIFO");
    }

    // 8. The assumption, measured by the driver too.
    if (pend_total > 0 && busy) {
      busy_run_++;
    } else {
      busy_run_ = 0;
    }
    if (busy_run_ > busy_run_max_) busy_run_max_ = busy_run_;
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

    // 9. (The window's "work at every grant" bookkeeping is updated above, on
    //    the grant itself, before the early-starvation check.)
  }

  bool driver_has_head(const Obs& o) const {
    return o.srv_class < static_cast<uint32_t>(kN) && !q_[o.srv_class].empty();
  }

  uint64_t driver_oldest_age(uint64_t now) const {
    uint64_t oldest = 0;
    for (int c = 0; c < kN; c++) {
      if (q_[c].empty()) continue;
      const uint64_t age = now - q_[c].front().arrive;
      if (age > oldest) oldest = age;
    }
    return oldest;
  }

  // -------------------------------------------------- the post-edge model
  void Apply(const Obs& o, const Stim& s, bool busy, uint64_t now) {
    const std::string where = phase_ + ": cycle " + Dec(now);
    const uint64_t pend_total = PendTotal();

    // The DUT's own loss counter must stay at zero.
    comparisons_++;
    Require(dut_->o_drop_ctr_o == 0, where + " (conservation)",
            "conservation: the DUT reported " + Dec(dut_->o_drop_ctr_o) +
                " discarded request(s); a request was dropped instead of back-pressured");

    if (o.srv_go) {
      Pending p = q_[o.srv_class].front();
      q_[o.srv_class].pop_front();
      WaitRec w;
      w.tag = p.tag;
      w.ahead = p.ahead;
      w.wait = now - p.arrive;
      waits_[o.srv_class].push_back(w);
      granted_[o.srv_class]++;
      grants_++;
      const bool dut_escaped = dut_->o_escape_ctr_o != escape_ctr_seen_;
      Grant g;
      g.cycle = now;
      g.cls = o.srv_class;
      g.tag = o.srv_tag;
      g.data = o.srv_data;
      g.escape = dut_escaped;
      stream_.push_back(g);
      busy_until_ = now + kService[o.srv_class];

      if (qos_en_) {
        // A grant charges a class that was already at its ceiling only if the
        // DUT reports it as an age escape, and an age escape must serve a
        // request that actually aged, only when no class was eligible.
        comparisons_++;
        Require(dut_escaped || win_grants_c_[o.srv_class] < kQuota[o.srv_class], where + " (quota)",
                std::string("quota: class ") + ClassName(o.srv_class) +
                    " was granted while already at " + Dec(win_grants_c_[o.srv_class]) +
                    " grant(s) in this window, and the DUT did not report an age escape; the "
                    "class's ceiling was not enforced, so a streaming class can monopolise the "
                    "port");
        if (dut_escaped) {
          comparisons_++;
          Require(w.wait + 2 >= kAgeLimit, where + " (escape)",
                  "escape: class " + std::string(ClassName(o.srv_class)) +
                      " was served over quota but its request had only aged " + Dec(w.wait) +
                      " cycles, below the trigger " + Dec(kAgeLimit) +
                      "; the over-quota path was not an age escape");
          comparisons_++;
          Require(!last_elig_any_, where + " (escape)",
                  "escape: the DUT served over quota while a class with pending work was still "
                  "under its quota; the reservation's premise does not hold, so the bound does not "
                  "follow");
        }
        grants_in_window_++;
        win_grants_c_[o.srv_class]++;
        if (grants_in_window_ == kWindow) {
          for (int c = 0; c < kN; c++) {
            if (!had_work_[c]) continue;
            comparisons_++;
            Require(win_grants_c_[c] >= kQuota[c], where + " (starvation)",
                    std::string("starvation: class ") + ClassName(c) + " was granted " +
                        Dec(win_grants_c_[c]) + " time(s) in window " + Dec(win_index_) +
                        " although it had pending work at every grant of that window; its reserved "
                        "share is " + Dec(kQuota[c]) +
                        ", so a class with continuous work can be starved for ever");
            if (!share_seen_[c]) {
              share_seen_[c] = true;
              share_min_[c] = win_grants_c_[c];
            } else if (win_grants_c_[c] < share_min_[c]) {
              share_min_[c] = win_grants_c_[c];
            }
          }
          windows_closed_++;
          grants_in_window_ = 0;
          for (int c = 0; c < kN; c++) win_grants_c_[c] = 0;
          win_index_++;
        }
      }
    }

    // Admissions and refusals: a full queue refuses and the producer holds.
    for (int c = 0; c < kN; c++) {
      if (s.req_valid[c] && o.req_ready[c]) {
        Pending p;
        p.tag = s.req_tag[c];
        p.data = s.req_data[c];
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
    escape_ctr_seen_ = dut_->o_escape_ctr_o;
    idle_ctr_seen_ = dut_->o_idle_ctr_o;
  }

  Vmosaic_qos_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  std::string phase_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  bool qos_en_ = true;

  // the environment
  std::deque<Pending> q_[kN];
  uint64_t busy_until_ = 0;
  uint64_t accepted_[kN] = {0, 0, 0};
  uint64_t granted_[kN] = {0, 0, 0};
  uint64_t refused_ = 0;
  uint64_t accepts_ = 0;
  uint64_t grants_ = 0;
  uint64_t stalls_ = 0;
  uint64_t escape_ctr_seen_ = 0;
  uint64_t idle_ctr_seen_ = 0;
  uint32_t busy_run_ = 0;
  uint32_t busy_run_max_ = 0;
  uint64_t idle_run_ = 0;

  // the window, as the driver counts it
  uint64_t win_index_ = 0;
  uint64_t grants_in_window_ = 0;
  uint64_t win_grants_c_[kN] = {0, 0, 0};
  bool had_work_[kN] = {true, true, true};
  bool share_seen_[kN] = {false, false, false};
  uint64_t share_min_[kN] = {0, 0, 0};
  uint64_t windows_closed_ = 0;

  // records
  std::vector<WaitRec> waits_[kN];
  std::vector<Offer> offers_;
  std::vector<Grant> stream_;
  bool expect_violation_ = false;
  bool last_elig_any_ = false;
};

// ============================================================================
// the naive control: the same machine, throughput-first
// ============================================================================

struct NaiveResult {
  uint64_t grants[kN] = {0, 0, 0};
  uint64_t max_wait[kN] = {0, 0, 0};
  uint64_t stuck[kN] = {0, 0, 0};
  uint64_t total_grants = 0;
};

// The selection rule the driver forbids: the throughput classes win, SCALAR
// waits for a gap. Same queue depth, same service times, same downstream.
NaiveResult ReplayNaive(const std::vector<Offer>& offers) {
  NaiveResult r;
  std::deque<Pending> q[kN];
  uint64_t busy_until = 0;
  for (size_t t = 0; t < offers.size(); t++) {
    for (int c = 0; c < kN; c++) {
      if (((offers[t].valid_mask >> c) & 1u) == 0) continue;
      if (q[c].size() >= kDepth) continue;
      Pending p;
      p.tag = offers[t].tag[c];
      p.data = offers[t].data[c];
      p.arrive = t;
      p.ahead = static_cast<uint32_t>(q[c].size());
      q[c].push_back(p);
    }
    if (t < busy_until) continue;
    int pick = -1;
    for (int c = kN - 1; c >= 0; c--) {  // BULK (2) > PREFETCH (1) > SCALAR (0)
      if (!q[c].empty()) {
        pick = c;
        break;
      }
    }
    if (pick < 0) continue;
    const Pending p = q[pick].front();
    q[pick].pop_front();
    const uint64_t wait = t - p.arrive;
    if (wait > r.max_wait[pick]) r.max_wait[pick] = wait;
    r.grants[pick]++;
    r.total_grants++;
    busy_until = t + kService[pick];
  }
  for (int c = 0; c < kN; c++) r.stuck[c] = q[c].size();
  return r;
}

void ReportNaive(const NaiveResult& naive, Harness* h) {
  for (int c = 0; c < kN; c++) {
    std::printf("  [naive] %-8s grants=%llu max-wait=%llu stuck=%llu\n", ClassName(c),
                static_cast<unsigned long long>(naive.grants[c]),
                static_cast<unsigned long long>(naive.max_wait[c]),
                static_cast<unsigned long long>(naive.stuck[c]));
  }
  std::printf("  [dut]   grants=%llu/%llu/%llu escapes=%llu refuses=%llu idles=%llu\n",
              static_cast<unsigned long long>(h->granted_of(0)),
              static_cast<unsigned long long>(h->granted_of(1)),
              static_cast<unsigned long long>(h->granted_of(2)),
              static_cast<unsigned long long>(h->escapes()),
              static_cast<unsigned long long>(h->refuses()),
              static_cast<unsigned long long>(h->idles()));
}

// ============================================================== percentiles

void PrintLatency(const char* label, const std::vector<WaitRec>& waits) {
  std::vector<uint64_t> v;
  v.reserve(waits.size());
  for (const WaitRec& w : waits) v.push_back(w.wait);
  if (v.empty()) {
    std::printf("  [lat] %-8s n=0\n", label);
    return;
  }
  std::sort(v.begin(), v.end());
  const uint64_t p50 = v[(v.size() * 50) / 100];
  const uint64_t p99 = v[(v.size() * 99) / 100];
  std::printf("  [lat] %-8s n=%llu p50=%llu p99=%llu max=%llu\n", label,
              static_cast<unsigned long long>(v.size()),
              static_cast<unsigned long long>(p50),
              static_cast<unsigned long long>(p99),
              static_cast<unsigned long long>(v.back()));
}

// ============================================================================
// phases
// ============================================================================

// ------------------------------------------------------------ 1. geometry

void PhaseGeometry(Harness* h, mosaic::Reporter* rep) {
  h->Phase("geometry");
  h->SetPolicy(true);
  h->Reset(4);
  Vmosaic_qos_tb* d = h->dut();
  Require(d->o_classes_o == kN && d->o_window_o == kWindow && d->o_d_max_o == kDMax &&
              d->o_depth_o == kDepth && d->o_age_limit_o == kAgeLimit,
          "geometry",
          "the DUT reports " + Dec(d->o_classes_o) + " classes, window " + Dec(d->o_window_o) +
              ", d_max " + Dec(d->o_d_max_o) + ", depth " + Dec(d->o_depth_o) + ", age_limit " +
              Dec(d->o_age_limit_o) + "; this driver drives 3/" + Dec(kWindow) + "/" + Dec(kDMax) +
              "/" + Dec(kDepth) + "/" + Dec(kAgeLimit));
  const uint32_t quota_lane[3] = {d->o_quota0_o, d->o_quota1_o, d->o_quota2_o};
  for (int c = 0; c < kN; c++) {
    Require(quota_lane[c] == kQuota[c], "geometry",
            std::string("class ") + ClassName(c) + " reports quota " + Dec(quota_lane[c]) +
                ", this driver drives " + Dec(kQuota[c]));
  }
  Require(d->o_used0_o == 0 && d->o_used1_o == 0 && d->o_used2_o == 0 && d->o_pend0_o == 0 &&
              d->o_pend1_o == 0 && d->o_pend2_o == 0,
          "geometry", "a class is not empty after reset");
  Require(d->o_window_index_o == 0 && d->o_window_grants_o == 0 && !d->o_assumption_violated_o,
          "geometry", "the window or assumption state is not clear after reset");
  rep->Check(true,
             "geometry: 3 classes with quotas 4/2/2 summing to a window of " + Dec(kWindow) +
                 " grants, depth " + Dec(kDepth) + ", age trigger " + Dec(kAgeLimit) +
                 ", declared maximum not-ready run " + Dec(kDMax) + ", bound " + Dec(kBound));
}

// ------------------------------------------------------- 2. directed: fifo

void PhaseFifo(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:fifo");
  h->SetPolicy(true);
  h->Reset(4);

  const uint8_t tags[3] = {0x11, 0x22, 0x33};
  const uint32_t data[3] = {0xAAA00001u, 0xAAA00002u, 0xAAA00003u};
  std::vector<uint8_t> got_tag;
  std::vector<uint32_t> got_data;
  for (int i = 0; i < 3; i++) {
    Stim s;
    s.req_valid[CLS_BULK] = true;
    s.req_tag[CLS_BULK] = tags[i];
    s.req_data[CLS_BULK] = data[i];
    const Obs o = h->Cycle(s);
    Require(o.req_ready[CLS_BULK], h->Where(),
            "the BULK queue refused request " + Dec(i) + " while it had room");
    if (o.srv_go) {
      got_tag.push_back(o.srv_tag);
      got_data.push_back(o.srv_data);
    }
  }
  for (int i = 0; i < 400 && got_tag.size() < 3u; i++) {
    const Obs o = h->Cycle(Stim());
    if (!o.srv_go) continue;
    Require(o.srv_class == static_cast<uint32_t>(CLS_BULK), h->Where(),
            "a class with no pending request was served (class " + Dec(o.srv_class) + ")");
    got_tag.push_back(o.srv_tag);
    got_data.push_back(o.srv_data);
  }
  Require(got_tag.size() == 3, "directed:fifo",
          "starvation: only " + Dec(got_tag.size()) + " of 3 BULK requests were delivered while "
          "the class held pending work throughout; a class with continuous work can be starved "
          "for ever");
  for (size_t i = 0; i < got_tag.size(); i++) {
    Require(got_tag[i] == tags[i], "directed:fifo",
            "class order violated: position " + Dec(i) + " delivered tag " + Dec(got_tag[i]) +
                ", the queue was loaded oldest first");
    Require(got_data[i] == data[i], "directed:fifo",
            "data: position " + Dec(i) + " delivered payload " + Dec(got_data[i]) +
                ", expected " + Dec(data[i]));
  }
  rep->Check(true, "directed: three BULK requests left the queue oldest first, each carrying its "
                   "own payload");
}

// ----------------------------------------------------- 3. directed: credit

void PhaseCredits(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:credit-backpressure");
  h->SetPolicy(true);
  h->Reset(4);

  Stim s;
  s.req_valid[CLS_BULK] = true;
  s.req_tag[CLS_BULK] = 0x5A;
  s.req_data[CLS_BULK] = 0xDEADBEEFu;
  uint64_t refused = 0;
  uint64_t accepted = 0;
  for (int i = 0; i < 16; i++) {
    const Obs o = h->Cycle(s);
    if (!o.req_ready[CLS_BULK]) {
      refused++;
    } else {
      accepted++;
    }
  }
  Require(refused > 0, "directed:credit-backpressure",
          "the BULK queue never refused: no backpressure was exercised");
  Require(accepted > 0 && h->accepted_of(CLS_BULK) == accepted, "directed:credit-backpressure",
          "the queue admitted " + Dec(h->accepted_of(CLS_BULK)) + " request(s) while the driver "
          "saw " + Dec(accepted));
  std::printf("  [credit]  refused=%llu admitted=%llu\n",
              static_cast<unsigned long long>(refused),
              static_cast<unsigned long long>(accepted));
  rep->Check(true,
             "directed: a full class queue refuses (" + Dec(refused) +
                 " refused offers) and the producer's offer is held and admitted later (" +
                 Dec(accepted) + " admitted); nothing is lost");
}

// ------------------------------------------------ 4. directed: reservation

void PhaseReservation(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:reservation");
  h->SetPolicy(true);
  h->Reset(4);

  // SCALAR, the latency-critical class, holds a request in *every* cycle, and
  // BULK does too. Both queues are therefore never empty, which is what makes a
  // class's per-window share measurable from the run.
  for (int i = 0; i < 240; i++) {
    Stim s;
    s.req_valid[CLS_SCALAR] = true;
    s.req_tag[CLS_SCALAR] = static_cast<uint8_t>(0x10u + (i % 4));
    s.req_data[CLS_SCALAR] = 0x5000u + static_cast<uint32_t>(i);
    s.req_valid[CLS_BULK] = true;
    s.req_tag[CLS_BULK] = static_cast<uint8_t>(0x70u + ((i / 2) % 4));
    s.req_data[CLS_BULK] = 0x9000u + static_cast<uint32_t>(i);
    h->Cycle(s);
  }

  Require(h->wait_any(CLS_SCALAR) && h->wait_any(CLS_BULK), "directed:reservation",
          "starvation: a continuously pending class was never served under the reservation");
  Require(h->share_seen(CLS_SCALAR) && h->share_seen(CLS_BULK), "directed:reservation",
          "no window closed with both classes pending at every grant, so their shares are not "
          "measurable from this run");
  Require(h->share_min(CLS_SCALAR) >= kQuota[CLS_SCALAR], "directed:reservation",
          "starvation: SCALAR received as few as " + Dec(h->share_min(CLS_SCALAR)) +
              " grant(s) in a window it had work throughout, below its reserved share " +
              Dec(kQuota[CLS_SCALAR]));
  Require(h->share_min(CLS_BULK) >= kQuota[CLS_BULK], "directed:reservation",
          "starvation: BULK received as few as " + Dec(h->share_min(CLS_BULK)) +
              " grant(s) in a window it had work throughout, below its share " +
              Dec(kQuota[CLS_BULK]));

  uint64_t worst = 0;
  for (const WaitRec& w : h->waits(CLS_BULK)) {
    Require(w.wait <= kBound, "directed:reservation",
            "bound: a BULK request under the SCALAR flood waited " + Dec(w.wait) +
                " cycles, above the declared bound " + Dec(kBound));
    if (w.wait > worst) worst = w.wait;
  }
  std::printf("  [reserve] SCALAR share_min=%llu BULK share_min=%llu BULK max-wait=%llu "
              "bound=%llu windows=%llu\n",
              static_cast<unsigned long long>(h->share_min(CLS_SCALAR)),
              static_cast<unsigned long long>(h->share_min(CLS_BULK)),
              static_cast<unsigned long long>(worst),
              static_cast<unsigned long long>(kBound),
              static_cast<unsigned long long>(h->windows_closed()));
  rep->Check(true,
             "directed: under a continuous SCALAR+BULK flood, SCALAR's measured share_min is " +
                 Dec(h->share_min(CLS_SCALAR)) + " (reservation " + Dec(kQuota[CLS_SCALAR]) +
                 ") and BULK's is " + Dec(h->share_min(CLS_BULK)) + " (quota " +
                 Dec(kQuota[CLS_BULK]) + "); BULK's worst wait " + Dec(worst) +
                 " is within the declared bound " + Dec(kBound) + " over " +
                 Dec(h->windows_closed()) + " closed windows");
}

// -------------------------------------------------- 5. directed: age escape

void PhaseAgeEscape(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:age_escape");
  h->SetPolicy(true);
  h->Reset(4);

  // BULK alone: it fills its ceiling, every pending class is then at its
  // ceiling, and the port idles until the oldest request ages -- at which point
  // the age escape serves it. This is the mechanism the card names, on its own.
  uint64_t first_escape_cycle = 0;
  uint64_t escaped_wait = 0;
  bool saw_escape = false;
  for (int i = 0; i < 200; i++) {
    Stim s;
    s.req_valid[CLS_BULK] = true;
    s.req_tag[CLS_BULK] = static_cast<uint8_t>(0x40u + (i % 5));
    s.req_data[CLS_BULK] = 0xB000u + static_cast<uint32_t>(i);
    const Obs o = h->Cycle(s);
    if (o.srv_go && !saw_escape && h->escapes() > 0) {
      saw_escape = true;
      first_escape_cycle = h->cycles();
      escaped_wait = h->waits(CLS_BULK).back().wait;
    }
  }
  Require(h->escapes() > 0, "directed:age_escape",
          "escape: the port never took the age escape although a lone class saturated its ceiling "
          "and a request aged past the trigger; a request would wait without a bound");
  Require(escaped_wait + 2 >= kAgeLimit, "directed:age_escape",
          "escape: the first over-quota grant served a request that had waited only " +
              Dec(escaped_wait) + " cycles, below the age trigger " + Dec(kAgeLimit) +
              "; the over-quota path was not an age escape");
  std::printf("  [age]     first-escape-cycle=%llu escaped-wait=%llu escapes=%llu idles=%llu\n",
              static_cast<unsigned long long>(first_escape_cycle),
              static_cast<unsigned long long>(escaped_wait),
              static_cast<unsigned long long>(h->escapes()),
              static_cast<unsigned long long>(h->idles()));
  rep->Check(true,
             "directed: the age escape fired for an old BULK request (first at cycle " +
                 Dec(first_escape_cycle) + ", after a wait of " + Dec(escaped_wait) +
                 " cycles against a trigger of " + Dec(kAgeLimit) + "; " + Dec(h->escapes()) +
                 " over-quota grant(s)); until then the port idled with every pending class at "
                 "its ceiling (" + Dec(h->idles()) + " idle cycles)");
}

// -------------------------------------------------- 6. directed: assumption

void PhaseAssumption(Harness* h, mosaic::Reporter* rep) {
  h->Phase("directed:assumption");
  h->SetPolicy(true);
  h->Reset(4);
  h->ExpectViolation(true);

  Stim s;
  s.req_valid[CLS_PREFETCH] = true;
  s.req_tag[CLS_PREFETCH] = 0x31;
  s.req_data[CLS_PREFETCH] = 0xC0FFEEu;
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
              static_cast<int>(o.viol));
  rep->Check(true,
             "directed: holding the port not-ready past the declared maximum (" + Dec(kDMax + 3) +
                 " cycles, measured longest run " + Dec(run_max) + " against D_MAX " + Dec(kDMax) +
                 ") is reported as an assumption violation, not as a bound; the flag clears on "
                 "reset");
}

// ================================================================== the chain
//
// A scalar *dependent* load: each request's payload is the value the memory
// returns for its address, and the next request's address is that value. The
// chain advances only when a scalar load is served, so a policy that starves
// the scalar class stops the chain. BULK streams every cycle.

uint32_t LoadValue(uint32_t addr) { return (addr * 2654435761u) + 0x9E3779B9u; }
uint32_t NextAddr(uint32_t value) { return (value ^ 0x0000ABCDu) & 0x0000FFFFu; }

struct ChainResult {
  uint64_t scalar_links = 0;
  uint64_t bulk_served = 0;
};

ChainResult RunChain(Harness* h, int cycles, bool offer_prefetch) {
  ChainResult r;
  bool outstanding = false;
  uint32_t addr = 0x1000;
  uint8_t scalar_tag = 0;
  uint8_t bulk_tag = 0;
  int link = 0;
  for (int i = 0; i < cycles; i++) {
    Stim s;
    if (!outstanding) {
      scalar_tag = static_cast<uint8_t>(0x20u + (link % 8));
      s.req_valid[CLS_SCALAR] = true;
      s.req_tag[CLS_SCALAR] = scalar_tag;
      s.req_data[CLS_SCALAR] = LoadValue(addr);
    }
    s.req_valid[CLS_BULK] = true;
    s.req_tag[CLS_BULK] = bulk_tag++;
    s.req_data[CLS_BULK] = 0xB0000000u + static_cast<uint32_t>(i);
    if (offer_prefetch && (i % 7) == 0) {
      s.req_valid[CLS_PREFETCH] = true;
      s.req_tag[CLS_PREFETCH] = static_cast<uint8_t>(0x50u + (i % 4));
      s.req_data[CLS_PREFETCH] = 0x000F000u + static_cast<uint32_t>(i);
    }
    const Obs o = h->Cycle(s);
    if (s.req_valid[CLS_SCALAR] && o.req_ready[CLS_SCALAR]) {
      outstanding = true;
    }
    if (o.srv_go && o.srv_class == static_cast<uint32_t>(CLS_SCALAR)) {
      // The one outstanding scalar link was served: its result is the next
      // link's address.
      r.scalar_links++;
      outstanding = false;
      addr = NextAddr(o.srv_data);
      link++;
    }
  }
  r.bulk_served = h->granted_of(CLS_BULK);
  return r;
}

// -------------------------------------------------------- 7. the chain (on)

void PhaseChain(Harness* h, mosaic::Reporter* rep) {
  h->Phase("chain");
  h->SetPolicy(true);
  h->Reset(4);
  const uint64_t start = h->cycles();
  const ChainResult r = RunChain(h, 1200, /*offer_prefetch=*/true);

  Require(r.scalar_links > 20, "chain",
          "the scalar dependent chain advanced only " + Dec(r.scalar_links) +
              " link(s) under the policy; the chain must make progress");
  Require(r.bulk_served > 20, "chain",
          "the bulk stream was served only " + Dec(r.bulk_served) + " time(s) under the policy");

  for (int c = 0; c < kN; c++) {
    Require(h->accepted_of(c) > 0, "chain",
            "starvation: class " + std::string(ClassName(c)) +
                " accepted no request in the chain phase");
    Require(h->granted_of(c) > 0, "chain",
            "starvation: class " + std::string(ClassName(c)) +
                " was never served in the chain phase");
    for (const WaitRec& w : h->waits(c)) {
      Require(w.wait <= kBound, "chain",
              "bound: class " + std::string(ClassName(c)) + " request tag " + Dec(w.tag) +
                  " waited " + Dec(w.wait) + " cycles, above the declared bound " + Dec(kBound));
    }
  }

  std::printf("  [chain]   cycles=%llu scalar-links=%llu\n",
              static_cast<unsigned long long>(h->cycles() - start),
              static_cast<unsigned long long>(r.scalar_links));
  for (int c = 0; c < kN; c++) PrintLatency(ClassName(c), h->waits(c));
  rep->Check(true,
             "chain: a scalar dependent chain advanced " + Dec(r.scalar_links) +
                 " link(s) against " + Dec(r.bulk_served) + " bulk grant(s); every class was "
                 "exercised and every measured wait is within the declared bound " + Dec(kBound) +
                 " (p50/p99 printed above)");
}

// ------------------------------------------------------ 8. the chain (off)

void PhaseNaiveChain(Harness* h, mosaic::Reporter* rep) {
  h->Phase("naive");
  h->SetPolicy(false);
  h->Reset(4);
  const ChainResult r = RunChain(h, 800, /*offer_prefetch=*/false);

  Require(r.scalar_links <= 1, "naive",
          "with the policy off the scalar dependent chain advanced " + Dec(r.scalar_links) +
              " link(s); the naive throughput-first policy was expected to starve it, so this "
              "stimulus does not reach the card's fail mode");
  Require(r.bulk_served > 100, "naive",
          "with the policy off bulk was served only " + Dec(r.bulk_served) +
              " time(s); the throughput-first policy did not monopolise the port");
  std::printf("  [naive]   policy-off scalar-links=%llu bulk-served=%llu\n",
              static_cast<unsigned long long>(r.scalar_links),
              static_cast<unsigned long long>(r.bulk_served));
  rep->Check(true,
             "naive: with the QoS policy off, the throughput-first rule served " +
                 Dec(r.bulk_served) + " bulk request(s) and starved the scalar dependent chain "
                 "after " + Dec(r.scalar_links) + " link(s) -- the stimulus provably reaches the "
                 "card's fail mode");
}

// ------------------------------------------------------ 9. data identity

void PhaseDataIdentity(Harness* h, mosaic::Reporter* rep) {
  h->Phase("data_identity");

  // A fixed, non-reactive stream: 200 requests, one per name, each with a unique
  // tag (the port's tag is 8 bits) and a payload that must come back unchanged.
  // Each is offered and *held until accepted*, so the stream is exactly the
  // requests intended. It is replayed twice -- policy off, then policy on -- and
  // drained, so the comparison covers every offered request.
  struct ReqSpec {
    uint32_t cls;
    uint8_t tag;
    uint32_t data;
  };
  std::vector<ReqSpec> specs;
  mosaic::Rng rng(0xD1D0ull);
  const int kReqs = 200;
  for (int i = 0; i < kReqs; i++) {
    uint32_t cls;
    const int slot = i % 10;
    if (slot < 6) {
      cls = CLS_BULK;
    } else if (slot < 8) {
      cls = CLS_SCALAR;
    } else {
      cls = CLS_PREFETCH;
    }
    ReqSpec spec;
    spec.cls = cls;
    spec.tag = static_cast<uint8_t>(i + 1);
    spec.data = 0xC0DE0000u | (cls << 20) | rng.Below(1u << 16);
    specs.push_back(spec);
  }

  auto run = [&](bool qos_on, std::vector<std::pair<uint8_t, uint32_t>>* delivered,
                 std::vector<uint32_t>* order) {
    h->SetPolicy(qos_on);
    h->Phase(qos_on ? "data_identity:on" : "data_identity:off");
    h->Reset(4);
    auto record = [&](const Obs& o) {
      if (o.srv_go) {
        delivered->push_back({o.srv_tag, o.srv_data});
        order->push_back(o.srv_class);
      }
    };
    size_t idx = 0;
    while (idx < specs.size()) {
      const ReqSpec& spec = specs[idx];
      Stim s;
      s.req_valid[spec.cls] = true;
      s.req_tag[spec.cls] = spec.tag;
      s.req_data[spec.cls] = spec.data;
      const Obs o = h->Cycle(s);
      record(o);
      if (o.req_ready[spec.cls]) idx++;
    }
    // Drain: stop offering and let every accepted request be served, so the two
    // runs are compared over the same, complete set.
    uint64_t guard = 40000;
    while (h->pend_total() > 0) {
      Require(guard-- > 0, "data_identity", "a run did not drain its queues");
      record(h->Cycle(Stim()));
    }
  };

  std::vector<std::pair<uint8_t, uint32_t>> off_data, on_data;
  std::vector<uint32_t> off_order, on_order;
  run(false, &off_data, &off_order);
  run(true, &on_data, &on_order);

  std::map<uint8_t, uint32_t> off_map(off_data.begin(), off_data.end());
  std::map<uint8_t, uint32_t> on_map(on_data.begin(), on_data.end());
  Require(off_map.size() == static_cast<size_t>(kReqs), "data_identity",
          "policy-off delivered only " + Dec(off_map.size()) + " distinct request(s) of " +
              Dec(kReqs) + " offered after draining");
  Require(on_map.size() == off_map.size(), "data_identity",
          "data: the number of distinct requests delivered differs: policy-off " +
              Dec(off_map.size()) + ", policy-on " + Dec(on_map.size()));
  for (const auto& kv : on_map) {
    auto it = off_map.find(kv.first);
    Require(it != off_map.end(), "data_identity",
            "data: tag " + Dec(kv.first) + " was delivered with the policy on but not off");
    Require(it->second == kv.second, "data_identity",
            "data: tag " + Dec(kv.first) + " returned payload " + Dec(kv.second) +
                " with the policy on and " + Dec(it->second) +
                " with the policy off; priority changed a data result");
  }
  Require(on_order != off_order, "data_identity",
          "the policy on and off produced the same service order, so the comparison does not "
          "exercise a scheduling choice");
  std::printf("  [data]    delivered=%llu identical=yes order-differs=yes\n",
              static_cast<unsigned long long>(on_map.size()));
  rep->Check(true,
             "data_identity: the same " + Dec(kReqs) +
                 " offered request(s) delivered " + Dec(on_map.size()) +
                 " identical payload(s) with the policy off and on (every tag matches its offer), "
                 "while the service order changes -- priority is a scheduling choice, not a "
                 "memory model");
}

// -------------------------------------------------------- 10. saturation

void RunMix(Harness* h, uint64_t seed, int cycles) {
  mosaic::Rng rng(seed);
  bool offering[kN] = {true, false, true};
  uint8_t tag[kN] = {1, 1, 1};
  for (int i = 0; i < cycles; i++) {
    Stim s;
    for (int c = 0; c < kN; c++) {
      // SCALAR and BULK demand every cycle. PREFETCH is a burst: present for a
      // stretch, absent for a stretch. During the absent stretch only SCALAR and
      // BULK are pending, their quotas sum below the window, and the port must
      // idle until a request ages and the age escape fires -- so the stress mix
      // reaches the over-quota path, not just the quota round-robin.
      uint32_t rate;
      if (c == CLS_SCALAR) {
        rate = 100u;
      } else if (c == CLS_BULK) {
        rate = 100u;
      } else {
        rate = ((i / 100) % 2 == 0) ? 100u : 0u;
      }
      if (!offering[c] && rng.Chance(rate)) {
        offering[c] = true;
        tag[c] = static_cast<uint8_t>(tag[c] + 1);
      }
      s.req_valid[c] = offering[c];
      s.req_tag[c] = tag[c];
      s.req_data[c] = 0xE0000000u | (static_cast<uint32_t>(c) << 16) | tag[c];
    }
    const Obs o = h->Cycle(s);
    for (int c = 0; c < kN; c++) {
      if (s.req_valid[c] && o.req_ready[c]) offering[c] = false;
    }
  }
}

void PhaseSaturation(Harness* h, mosaic::Reporter* rep, uint64_t seed) {
  h->Phase("saturation");
  h->SetPolicy(true);
  h->Reset(4);
  const uint64_t start = h->cycles();
  RunMix(h, seed ^ 0x1062ull, 2400);

  // --- coverage: the stress must actually reach every class and every path.
  // SCALAR and BULK are offered every cycle, so their per-window share is
  // measurable; PREFETCH is intermittent and is covered by its grants and waits.
  for (int c = 0; c < kN; c++) {
    Require(h->accepted_of(c) > 0, "saturation",
            "class " + std::string(ClassName(c)) + " accepted no request in the stress mix");
    Require(h->granted_of(c) > 0, "saturation",
            "starvation: class " + std::string(ClassName(c)) +
                " was never served in the stress mix");
    const bool continuous = (c == CLS_SCALAR || c == CLS_BULK);
    if (continuous) {
      Require(h->share_seen(c), "saturation",
              "no window closed with class " + std::string(ClassName(c)) +
                  " pending at every grant, so its share is not measurable from this run");
      Require(h->share_min(c) >= kQuota[c], "saturation",
              "starvation: class " + std::string(ClassName(c)) + " received as few as " +
                  Dec(h->share_min(c)) + " grant(s) in a window it had work throughout, below its "
                  "share " + Dec(kQuota[c]));
    }
  }
  Require(h->refuses() > 0, "saturation",
          "no class queue ever refused, so backpressure was not exercised");
  Require(h->escapes() > 0, "saturation", "the age escape was never taken");
  Require(h->windows_closed() >= 4, "saturation",
          "only " + Dec(h->windows_closed()) + " window(s) closed");
  Require(!h->dut()->o_assumption_violated_o, "saturation",
          "assumption: the stress mix violated the downstream bound");
  Require(h->busy_run_max() <= kDMax, "saturation",
          "assumption: the driver measured a not-ready run of " + Dec(h->busy_run_max()) +
              " cycles, past the declared maximum " + Dec(kDMax));

  // --- the measured worst-case wait and the declared bound.
  uint64_t total_waits = 0;
  for (int c = 0; c < kN; c++) {
    uint64_t worst = 0;
    for (const WaitRec& w : h->waits(c)) {
      Require(w.wait <= kBound, "saturation",
              "bound: class " + std::string(ClassName(c)) + " request tag " + Dec(w.tag) +
                  " waited " + Dec(w.wait) + " cycles, above the declared bound " + Dec(kBound));
      if (w.wait > worst) worst = w.wait;
      total_waits++;
    }
    std::printf("  [bound] %-8s waits=%llu max-wait=%llu bound=%llu share_min=%llu quota=%llu\n",
                ClassName(c), static_cast<unsigned long long>(h->waits(c).size()),
                static_cast<unsigned long long>(worst),
                static_cast<unsigned long long>(kBound),
                static_cast<unsigned long long>(h->share_min(c)),
                static_cast<unsigned long long>(kQuota[c]));
  }

  // --- the naive control on the same stimulus.
  const NaiveResult naive = ReplayNaive(h->offers());
  bool naive_starved = false;
  for (int c = 0; c < kN; c++) {
    if (naive.stuck[c] > 0 || naive.max_wait[c] > kBound) naive_starved = true;
  }
  Require(naive_starved, "saturation",
          "the saturation stimulus starved no class under a naive throughput-first scheme, so the "
          "case does not reach the card's fail mode");
  ReportNaive(naive, h);

  std::printf("  [mix]   cycles=%llu grants=%llu accepts=%llu refuses=%llu escapes=%llu "
              "stalls=%llu idles=%llu windows=%llu\n",
              static_cast<unsigned long long>(h->cycles() - start),
              static_cast<unsigned long long>(h->grants()),
              static_cast<unsigned long long>(h->accepts()),
              static_cast<unsigned long long>(h->refuses()),
              static_cast<unsigned long long>(h->escapes()),
              static_cast<unsigned long long>(h->stalls()),
              static_cast<unsigned long long>(h->idles()),
              static_cast<unsigned long long>(h->windows_closed()));
  for (int c = 0; c < kN; c++) PrintLatency(ClassName(c), h->waits(c));

  rep->Check(true,
             "saturation: " + Dec(h->grants()) + " requests over " + Dec(h->cycles()) +
                 " cycles from " + Dec(total_waits) + " measured requests (SCALAR and BULK "
                 "continuous, PREFETCH 25%), " + Dec(h->windows_closed()) +
                 " windows closed with every class that had work throughout granted at least its "
                 "share, every measured wait within the declared bound " + Dec(kBound) +
                 ", the age escape taken " + Dec(h->escapes()) + " time(s), and the same stimulus "
                 "leaving a class unserved under a naive throughput-first scheme");
}

// -------------------------------------------------------- 11. determinism

void PhaseDeterminism(Harness* h, mosaic::Reporter* rep, uint64_t seed) {
  h->SetPolicy(true);
  h->Phase("determinism");
  h->Reset(4);
  const uint64_t start_a = h->cycles();
  RunMix(h, seed ^ 0xd00dull, 300);
  const std::vector<Grant> a = h->stream();

  h->Phase("determinism");
  h->Reset(4);
  const uint64_t start_b = h->cycles();
  RunMix(h, seed ^ 0xd00dull, 300);
  const std::vector<Grant> b = h->stream();

  Require(a.size() == b.size(), "determinism",
          "run A made " + Dec(a.size()) + " grants, run B " + Dec(b.size()));
  for (size_t i = 0; i < a.size(); i++) {
    Require(a[i].cycle - start_a == b[i].cycle - start_b && a[i].cls == b[i].cls &&
                a[i].tag == b[i].tag && a[i].data == b[i].data,
            "determinism",
            "grant " + Dec(i) + " differs: A(cycle " + Dec(a[i].cycle) + ", class " +
                Dec(a[i].cls) + ", tag " + Dec(a[i].tag) + "), B(cycle " + Dec(b[i].cycle) +
                ", class " + Dec(b[i].cls) + ", tag " + Dec(b[i].tag) + ")");
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
  Vmosaic_qos_tb dut;
  Harness harness(&dut, &clk, options.max_cycles);

  std::string detail;
  bool passed = true;
  try {
    PhaseGeometry(&harness, &reporter);
    PhaseFifo(&harness, &reporter);
    PhaseCredits(&harness, &reporter);
    PhaseReservation(&harness, &reporter);
    PhaseAgeEscape(&harness, &reporter);
    PhaseAssumption(&harness, &reporter);

    PhaseChain(&harness, &reporter);
    PhaseNaiveChain(&harness, &reporter);
    PhaseDataIdentity(&harness, &reporter);

    PhaseSaturation(&harness, &reporter, options.seed);
    PhaseDeterminism(&harness, &reporter, options.seed);

    detail = "QoS memory arbitration holds: " + Dec(harness.comparisons()) +
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
