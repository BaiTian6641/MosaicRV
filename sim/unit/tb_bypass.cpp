// ============================================================================
// tb_bypass.cpp -- CASE=bypass.local_raw_chain, work package I-027.
//
// The DUT is `mosaic_cluster_bypass`: the local fast path of one execution
// cluster. It is never its own oracle. The driver is the environment the
// cluster runs in -- a single local functional unit that computes one result a
// cycle, the writeback path's durable publication pipeline, and the issue
// queue's candidate uop -- and, every cycle, an independent comparison of the
// DUT's whole output surface against the contract documented in
// rtl/core/mosaic_cluster_bypass.sv. The shadow shares no code with the RTL and
// derives its answers from that prose, so agreeing with it is evidence about
// the contract rather than a restatement of the implementation.
//
// The durable path is *not* a delay the driver feels free to pick. It is the
// two register boundaries the integrated core actually has between a local
// result and a woken consumer:
//
//   cycle N    the FU computes the result (this is where the DUT taps it);
//   cycle N+1  the cluster's result register holds it (mosaic_cluster.sv,
//              `wb_ev_q`, written only at a clock edge);
//   cycle N+2  the WB arbiter's per-producer pending register holds it
//              (mosaic_wb_arbiter.sv, `pend_v`/`pend_ev`, also edge-written),
//              so the publication -- register-file write plus value-visible
//              wakeup -- is presented in cycle N+2 at the earliest, and the
//              arbiter publishes one producer per cycle.
//
// The driver's durable publisher is exactly that: a FIFO of produced results,
// each with earliest publication cycle = produced + 2, served one per cycle,
// shared with a competing producer stream when a phase asks for it. The cycle
// counts this case reports come from the runs, not from a latency table.
//
// ------------------------------------------------------------------ phases
//
//   1. geometry            the widths the driver drives are the DUT's own.
//   2. registered-budget   a producer's *own* cycle cannot serve a consumer:
//                          the bypass is a register, and that register is the
//                          loop break. Also the EARLY_TAP control's first
//                          failure.
//   3. identity-reject     a tag that matches at another generation is not a
//                          match, and it is counted apart from a plain miss.
//   4. unauthorised        a withdrawn grant is never captured or forwarded.
//   5. flush-cancel        a redirect clears the slot and the consumer falls
//                          back to the durable path.
//   6. enable-off          with `bp_en` low nothing is served and the slot is
//                          emptied, so a later re-arm cannot forward an old
//                          value; the durable path is unaffected.
//   7. two-source-conflict one bypass slot, two outstanding operands: the one
//                          it can serve is bypassed, the other is selected for
//                          the register-file path, and the mixed decision -- and
//                          the precedence of the bypass over the durable
//                          broadcast for the same identity -- are checked.
//   8. raw-chain           the measured chain: `links` consecutive RAW
//                          dependencies, run with the bypass armed and then
//                          disarmed, with the per-link latency and the whole
//                          event stream taken from the two runs.
//   9. raw-chain-contended the same, with another cluster's results sharing the
//                          durable path, so the fallback is a throughput
//                          bottleneck and not only a fixed latency.
//  10. trace-agreement     the two runs' architectural event streams must be
//                          identical: same uops, same source identities, same
//                          values, in the same order. The resolution *path* is
//                          allowed to differ -- that is the point -- and is
//                          reported separately.
//  11. random              a seeded soak over a deliberately tiny generation
//                          space, compared against the shadow every cycle.
//  12. determinism         the same seed reproduces the same run byte for byte.
//
// Standing invariants, checked every compared cycle rather than in one place:
//
//   * the DUT's outputs never depend on `p_*` in the cycle the producer is
//     presented -- a hit in the producer's own cycle is the defect the card's
//     fail criterion names, and phase 2 checks it directly;
//   * `sN_rdy` is never asserted for an operand the queue did not declare
//     outstanding, and every miss selects the register-file path;
//   * a hit means the presented value is *the slot's* value, and the slot's
//     identity is the identity of the producer last captured;
//   * `o_unauth` and the counters agree with an independent tally every cycle.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_cluster_bypass_tb.h"

namespace {

// --------------------------------------------------------------- diagnostics

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
std::string Hex64(uint64_t value) { return mosaic::Hex(value, 16); }

// ------------------------------------------------------------------ stimulus

struct Stim {
  bool bp_en = false;
  bool flush = false;

  bool p_valid = false;
  bool p_authorised = false;
  uint32_t p_tag = 0;
  uint32_t p_gen = 0;
  uint32_t p_idx = 0;
  uint32_t p_rgen = 0;
  uint32_t p_uop = 0;
  uint64_t p_val = 0;

  bool w_valid = false;
  uint32_t w_tag = 0;
  uint32_t w_gen = 0;
  uint64_t w_val = 0;

  bool c_valid = false;
  uint32_t c1_tag = 0;
  uint32_t c1_gen = 0;
  bool c1_need = false;
  uint32_t c2_tag = 0;
  uint32_t c2_gen = 0;
  bool c2_need = false;
};

// One captured producer: the identity every later comparison is phrased in.
struct Producer {
  uint32_t tag = 0;
  uint32_t gen = 0;
  uint32_t idx = 0;
  uint32_t rgen = 0;
  uint32_t uop = 0;
  uint64_t val = 0;
};

struct Observed {
  bool bp1 = false;
  bool bp2 = false;
  bool fb1 = false;
  bool fb2 = false;
  bool r1 = false;
  bool r2 = false;
  uint64_t v1 = 0;
  uint64_t v2 = 0;
  uint32_t src1 = 0;
  uint32_t src2 = 0;
  bool slot_valid = false;
  uint32_t slot_tag = 0;
  uint32_t slot_gen = 0;
  uint32_t slot_idx = 0;
  uint32_t slot_rgen = 0;
  uint32_t slot_uop = 0;
  bool cap = false;
  bool unauth = false;
  uint32_t hit_ctr = 0;
  uint32_t miss_ctr = 0;
  uint32_t unauth_ctr = 0;
  uint32_t id_reject_ctr = 0;
  uint32_t flush_ctr = 0;
};

// ------------------------------------------------------------- the shadow
//
// The contract restated independently: one registered slot; a capture only for
// an armed, present, authorised producer with no redirect in flight; an exact
// (tag, generation) match; the bypass takes precedence over the durable
// broadcast for the same identity; and every operand the bypass misses selects
// the register-file path, which is what makes a miss safe.
class Shadow {
 public:
  struct Pred {
    bool bp1 = false, bp2 = false, fb1 = false, fb2 = false;
    bool r1 = false, r2 = false;
    uint64_t v1 = 0, v2 = 0;
    uint32_t src1 = 0, src2 = 0;
    bool slot_valid = false;
    uint32_t slot_tag = 0, slot_gen = 0, slot_idx = 0, slot_rgen = 0, slot_uop = 0;
    bool cap = false, unauth = false;
  };

  void Reset() {
    slot_v_ = false;
    tag_ = gen_ = idx_ = rgen_ = uop_ = 0;
    val_ = 0;
    hit_ = miss_ = unauth_ = idrej_ = flushc_ = 0;
  }

  Pred Eval(const Stim& s) const {
    Pred p;
    p.slot_valid = slot_v_;
    p.slot_tag = tag_;
    p.slot_gen = gen_;
    p.slot_idx = idx_;
    p.slot_rgen = rgen_;
    p.slot_uop = uop_;
    p.cap = s.bp_en && s.p_valid && s.p_authorised && !s.flush;
    p.unauth = s.p_valid && !s.p_authorised;

    const bool m1 = slot_v_ && (s.c1_tag == tag_) && (s.c1_gen == gen_);
    const bool m2 = slot_v_ && (s.c2_tag == tag_) && (s.c2_gen == gen_);
    p.bp1 = s.bp_en && s.c_valid && s.c1_need && m1;
    p.bp2 = s.bp_en && s.c_valid && s.c2_need && m2;

    const bool w1 = s.w_valid && (s.c1_tag == s.w_tag) && (s.c1_gen == s.w_gen);
    const bool w2 = s.w_valid && (s.c2_tag == s.w_tag) && (s.c2_gen == s.w_gen);

    p.fb1 = s.c_valid && s.c1_need && !p.bp1;
    p.fb2 = s.c_valid && s.c2_need && !p.bp2;
    p.r1 = p.bp1 || (s.c_valid && s.c1_need && w1);
    p.r2 = p.bp2 || (s.c_valid && s.c2_need && w2);
    p.src1 = p.bp1 ? 1u : (p.r1 ? 2u : 0u);
    p.src2 = p.bp2 ? 1u : (p.r2 ? 2u : 0u);
    p.v1 = p.bp1 ? val_ : s.w_val;
    p.v2 = p.bp2 ? val_ : s.w_val;
    return p;
  }

  // The edge: the counters take this cycle's combinational increments, then the
  // slot is updated. Computing the increments from the pre-edge slot is what the
  // RTL's registered counters do.
  void Apply(const Stim& s) {
    const uint32_t m1 = (slot_v_ && (s.c1_tag == tag_) && (s.c1_gen == gen_)) ? 1u : 0u;
    const uint32_t m2 = (slot_v_ && (s.c2_tag == tag_) && (s.c2_gen == gen_)) ? 1u : 0u;
    const uint32_t b1 = (s.bp_en && s.c_valid && s.c1_need && m1) ? 1u : 0u;
    const uint32_t b2 = (s.bp_en && s.c_valid && s.c2_need && m2) ? 1u : 0u;
    const uint32_t f1 = (s.c_valid && s.c1_need && !b1) ? 1u : 0u;
    const uint32_t f2 = (s.c_valid && s.c2_need && !b2) ? 1u : 0u;
    const uint32_t idrej =
        (s.bp_en && s.c_valid && slot_v_ &&
         ((s.c1_need && (s.c1_tag == tag_) && (s.c1_gen != gen_)) ||
          (s.c2_need && (s.c2_tag == tag_) && (s.c2_gen != gen_))))
            ? 1u
            : 0u;

    hit_ += b1 + b2;
    miss_ += f1 + f2;
    if (s.p_valid && !s.p_authorised) unauth_ += 1;
    idrej_ += idrej;
    if (s.flush && slot_v_) flushc_ += 1;

    if (s.flush || !s.bp_en) {
      slot_v_ = false;
    } else if (s.p_valid && s.p_authorised) {
      slot_v_ = true;
      tag_ = s.p_tag;
      gen_ = s.p_gen;
      idx_ = s.p_idx;
      rgen_ = s.p_rgen;
      uop_ = s.p_uop;
      val_ = s.p_val;
    }
  }

  bool slot_valid() const { return slot_v_; }
  uint32_t tag() const { return tag_; }
  uint32_t gen() const { return gen_; }
  uint32_t idx() const { return idx_; }
  uint32_t rgen() const { return rgen_; }
  uint32_t uop() const { return uop_; }
  uint64_t val() const { return val_; }
  uint32_t hits() const { return hit_; }
  uint32_t misses() const { return miss_; }
  uint32_t unauth() const { return unauth_; }
  uint32_t id_rejects() const { return idrej_; }
  uint32_t flush_kills() const { return flushc_; }

 private:
  bool slot_v_ = false;
  uint32_t tag_ = 0, gen_ = 0, idx_ = 0, rgen_ = 0, uop_ = 0;
  uint64_t val_ = 0;
  uint32_t hit_ = 0, miss_ = 0, unauth_ = 0, idrej_ = 0, flushc_ = 0;
};

// -------------------------------------------------------------- the harness

class Harness {
 public:
  Harness(Vmosaic_cluster_bypass_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }
  Shadow* shadow() { return &shadow_; }

  // The index of the cycle about to be driven: 0 before the first edge.
  uint64_t cycle_index() const { return cycles_; }

  void Reset(int cycles) {
    shadow_.Reset();
    for (int i = 0; i < cycles; i++) Cycle(Stim{}, /*rst=*/true);
  }

  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }

  Observed Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the phase finished");
    }
    const std::string where = phase_ + ": cycle " + Dec(cycles_);

    Drive(s, rst);
    dut_->eval();

    Observed o;
    if (!rst) {
      Capture(&o);
      const Shadow::Pred p = shadow_.Eval(s);
      Compare(o, p, where);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (!rst) {
      o.hit_ctr = dut_->o_hit_ctr_o;
      o.miss_ctr = dut_->o_miss_ctr_o;
      o.unauth_ctr = dut_->o_unauth_ctr_o;
      o.id_reject_ctr = dut_->o_id_reject_ctr_o;
      o.flush_ctr = dut_->o_flush_ctr_o;
      // The RTL's counters are updated by the edge just applied, and so is the
      // slot; advance the shadow the same way and then compare both.
      shadow_.Apply(s);
      CompareCounters(o, where);
      ComparePostSlot(where);
      ++comparisons_;
    }
    return o;
  }

  // Throws if any check recorded since the last call failed.
  void Expect(const std::string& where, bool condition, const std::string& detail) const {
    Require(condition, where, detail);
  }

 private:
  void Drive(const Stim& s, bool rst) {
    dut_->rst = rst ? 1 : 0;
    dut_->bp_en_i = s.bp_en ? 1 : 0;
    dut_->flush_i = s.flush ? 1 : 0;

    dut_->p_valid_i = s.p_valid ? 1 : 0;
    dut_->p_authorised_i = s.p_authorised ? 1 : 0;
    dut_->p_tag_i = s.p_tag;
    dut_->p_gen_i = s.p_gen;
    dut_->p_rob_index_i = s.p_idx;
    dut_->p_rob_gen_i = s.p_rgen;
    dut_->p_uop_index_i = s.p_uop;
    dut_->p_value_i = s.p_val;

    dut_->w_valid_i = s.w_valid ? 1 : 0;
    dut_->w_tag_i = s.w_tag;
    dut_->w_gen_i = s.w_gen;
    dut_->w_val_i = s.w_val;

    dut_->c_valid_i = s.c_valid ? 1 : 0;
    dut_->c_s1_tag_i = s.c1_tag;
    dut_->c_s1_gen_i = s.c1_gen;
    dut_->c_s1_need_i = s.c1_need ? 1 : 0;
    dut_->c_s2_tag_i = s.c2_tag;
    dut_->c_s2_gen_i = s.c2_gen;
    dut_->c_s2_need_i = s.c2_need ? 1 : 0;
  }

  void Capture(Observed* o) const {
    o->bp1 = dut_->bp_s1_hit_o != 0;
    o->bp2 = dut_->bp_s2_hit_o != 0;
    o->fb1 = dut_->fb_s1_sel_o != 0;
    o->fb2 = dut_->fb_s2_sel_o != 0;
    o->r1 = dut_->s1_rdy_o != 0;
    o->r2 = dut_->s2_rdy_o != 0;
    o->v1 = dut_->s1_val_o;
    o->v2 = dut_->s2_val_o;
    o->src1 = dut_->s1_src_o;
    o->src2 = dut_->s2_src_o;
    o->slot_valid = dut_->slot_valid_o != 0;
    o->slot_tag = dut_->slot_tag_o;
    o->slot_gen = dut_->slot_gen_o;
    o->slot_idx = dut_->slot_rob_index_o;
    o->slot_rgen = dut_->slot_rob_gen_o;
    o->slot_uop = dut_->slot_uop_index_o;
    o->cap = dut_->o_slot_captured_o != 0;
    o->unauth = dut_->o_unauth_o != 0;
  }

  void CmpB(const std::string& where, const std::string& field, bool exp, bool act) const {
    if (exp != act) Fail(where, field + ": expected " + Bool(exp) + ", got " + Bool(act));
  }
  void CmpU(const std::string& where, const std::string& field, uint64_t exp,
            uint64_t act) const {
    if (exp != act) {
      Fail(where, field + ": expected " + Dec(exp) + " (0x" + Hex64(exp) + "), got " + Dec(act) +
                      " (0x" + Hex64(act) + ")");
    }
  }

  void Compare(const Observed& o, const Shadow::Pred& p, const std::string& where) const {
    CmpB(where, "slot_valid (pre-edge)", p.slot_valid, o.slot_valid);
    // The slot's payload registers are deliberately not reset, so they hold the
    // last captured producer while the entry is invalid. Comparing them then
    // would be comparing power-up/leftover noise, exactly as the register file's
    // case documents for its own storage.
    if (p.slot_valid) {
      CmpU(where, "slot_tag", p.slot_tag, o.slot_tag);
      CmpU(where, "slot_gen", p.slot_gen, o.slot_gen);
      CmpU(where, "slot_rob_index", p.slot_idx, o.slot_idx);
      CmpU(where, "slot_rob_gen", p.slot_rgen, o.slot_rgen);
      CmpU(where, "slot_uop_index", p.slot_uop, o.slot_uop);
    }
    CmpB(where, "o_slot_captured", p.cap, o.cap);
    CmpB(where, "o_unauth", p.unauth, o.unauth);
    CmpB(where, "bp_s1_hit", p.bp1, o.bp1);
    CmpB(where, "bp_s2_hit", p.bp2, o.bp2);
    CmpB(where, "fb_s1_sel (register-file fallback selected)", p.fb1, o.fb1);
    CmpB(where, "fb_s2_sel (register-file fallback selected)", p.fb2, o.fb2);
    CmpB(where, "s1_rdy (bypass or register-file fallback)", p.r1, o.r1);
    CmpB(where, "s2_rdy (bypass or register-file fallback)", p.r2, o.r2);
    CmpU(where, "s1_src", p.src1, o.src1);
    CmpU(where, "s2_src", p.src2, o.src2);
    CmpU(where, "s1_val", p.v1, o.v1);
    CmpU(where, "s2_val", p.v2, o.v2);
  }

  void CompareCounters(const Observed& o, const std::string& where) const {
    CmpU(where, "o_hit_ctr", shadow_.hits(), o.hit_ctr);
    CmpU(where, "o_miss_ctr", shadow_.misses(), o.miss_ctr);
    CmpU(where, "o_unauth_ctr", shadow_.unauth(), o.unauth_ctr);
    CmpU(where, "o_id_reject_ctr", shadow_.id_rejects(), o.id_reject_ctr);
    CmpU(where, "o_flush_ctr", shadow_.flush_kills(), o.flush_ctr);
  }

  void ComparePostSlot(const std::string& where) const {
    CmpB(where, "slot_valid (post-edge)", shadow_.slot_valid(), dut_->slot_valid_o != 0);
    if (!shadow_.slot_valid()) return;   // see the note in Compare
    CmpU(where, "slot_tag (post-edge)", shadow_.tag(), (uint64_t)dut_->slot_tag_o);
    CmpU(where, "slot_gen (post-edge)", shadow_.gen(), (uint64_t)dut_->slot_gen_o);
    CmpU(where, "slot_rob_index (post-edge)", shadow_.idx(), (uint64_t)dut_->slot_rob_index_o);
    CmpU(where, "slot_rob_gen (post-edge)", shadow_.rgen(), (uint64_t)dut_->slot_rob_gen_o);
    CmpU(where, "slot_uop_index (post-edge)", shadow_.uop(), (uint64_t)dut_->slot_uop_index_o);
  }

  Vmosaic_cluster_bypass_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::string phase_;
  Shadow shadow_;
};

// ------------------------------------------------------- the durable publisher
//
// The writeback path's shared, in-order publication: one identity per cycle, at
// the earliest hole + 2 (the two register boundaries documented at the top).
// The driver owns it because the arbiter and the register file are other work
// packages; what this case must not own is the *local* decision, which is the
// DUT's.
struct Pub {
  uint64_t earliest;
  uint32_t tag;
  uint32_t gen;
  uint64_t val;
};

class Durable {
 public:
  void Clear() { fifo_.clear(); }
  void Push(uint64_t earliest, uint32_t tag, uint32_t gen, uint64_t val) {
    // `earliest` is non-decreasing by construction: every push is made at the
    // cycle it was produced, and the offset is constant.
    fifo_.push_back(Pub{earliest, tag, gen, val});
  }
  bool Step(uint64_t now, uint32_t* tag, uint32_t* gen, uint64_t* val) {
    if (fifo_.empty() || fifo_.front().earliest > now) return false;
    *tag = fifo_.front().tag;
    *gen = fifo_.front().gen;
    *val = fifo_.front().val;
    fifo_.pop_front();
    return true;
  }
  size_t depth() const { return fifo_.size(); }

 private:
  std::deque<Pub> fifo_;
};

// -------------------------------------------------------------- the raw chain

struct Event {
  uint32_t uop = 0;
  uint32_t src_tag = 0;
  uint32_t src_gen = 0;
  uint32_t src = 0;   // 1 = local bypass, 2 = durable, 0 = already in the queue
  uint64_t value = 0;
};

struct ChainResult {
  std::vector<Event> events;
  uint64_t first_grant = 0;
  uint64_t last_grant = 0;
  uint32_t bypass_hits = 0;
  uint32_t durable_hits = 0;
  uint32_t queue_roots = 0;
  uint64_t cycles_per_link_x1000 = 0;   // measured, from the run
};

// ------------------------------------------------------------- source tables
//
// Deterministic identities for the chain. Tags walk the physical register space
// (96 entries in p0), generations are drawn from the full 7-bit allocation
// space, and the destination's ROB identity travels with the value so a
// consumer can see whose value it holds.
uint32_t ChainTag(uint32_t j) { return (7u * j + 3u) % 96u; }
uint32_t ChainGen(uint32_t j) { return (5u * j + 1u) % 128u; }
uint32_t ChainIdx(uint32_t j) { return (3u * j + 1u) % 64u; }
uint32_t ChainRgen(uint32_t j) { return (11u * j + 2u) % 128u; }
uint32_t ChainUop(uint32_t j) { return j % 8u; }

// The bound on how much of another cluster's traffic shares the durable path.
// It models that cluster's own result register: it offers while it can be
// taken and stalls otherwise, instead of filling an unbounded queue. This
// cluster's own producers are not backpressured by this harness -- see the
// "not covered" note in the report.
constexpr size_t kCompeteDepth = 8;

ChainResult RunChain(Harness* h, uint32_t links, bool bp_en, uint32_t compete_period,
                     const std::string& tag) {
  h->Reset(4);
  const uint32_t total = links + 1;   // uop 0's source is already in the queue
  const uint32_t links_den = (links == 0) ? 1u : links;

  Durable pub;
  ChainResult r;
  uint32_t next = 0;
  uint64_t src_val = 0x1000;
  uint64_t comp_i = 0;
  uint64_t stall = 0;
  const uint64_t start = h->cycle_index();

  while (next < total) {
    const uint64_t now = h->cycle_index();
    const bool need = (next > 0);

    Stim s;
    s.bp_en = bp_en;
    s.c_valid = true;
    if (need) {
      s.c1_tag = ChainTag(next - 1);
      s.c1_gen = ChainGen(next - 1);
      s.c1_need = true;
    }

    // The durable publication for this cycle comes first, because whether the
    // candidate can issue depends on it. A publication pushed *this* cycle is
    // not eligible (its earliest cycle is `now + 2`), so taking it before
    // pushing this cycle's producer cannot reorder anything.
    uint32_t wtag = 0, wgen = 0;
    uint64_t wval = 0;
    if (pub.Step(now, &wtag, &wgen, &wval)) {
      s.w_valid = true;
      s.w_tag = wtag;
      s.w_gen = wgen;
      s.w_val = wval;
    }

    // What the contract says will happen this cycle. The DUT's answer must not
    // depend on `p_*`, so predicting before driving the producer is exactly the
    // property being checked.
    const Shadow::Pred p = h->shadow()->Eval(s);
    const bool granted = !need || p.r1;

    uint64_t consumed = src_val;
    if (granted) {
      consumed = need ? p.v1 : src_val;
      s.p_valid = true;
      s.p_authorised = true;
      s.p_tag = ChainTag(next);
      s.p_gen = ChainGen(next);
      s.p_idx = ChainIdx(next);
      s.p_rgen = ChainRgen(next);
      s.p_uop = ChainUop(next);
      s.p_val = consumed + 1;
      pub.Push(now + 2, s.p_tag, s.p_gen, s.p_val);
    }

    // Another cluster's result, sharing the one-publication-per-cycle durable
    // path. Its generation is drawn from 128..255, which the chain's identities
    // (0..127) cannot reach, so it can delay a publication but can never
    // satisfy an operand by accident.
    if (compete_period != 0 && ((now - start) % compete_period) == 0 &&
        pub.depth() < kCompeteDepth) {
      const uint32_t ctag = (uint32_t)((53u + comp_i) % 96u);
      const uint32_t cgen = (uint32_t)(128u + (comp_i % 128u));
      pub.Push(now + 2, ctag, cgen, 0x51510000ull + comp_i);
      comp_i++;
    }

    const Observed o = h->Cycle(s);

    if (!granted) {
      // A chain that stops progressing is a harness bug or a DUT that never
      // resolves an operand; either way it must be named rather than run to the
      // cycle cap.
      if (++stall > (4 * total + 64)) {
        Fail(tag, "the chain stalled at uop " + Dec(next) + " (slot_valid " +
                      Bool(h->shadow()->slot_valid()) + ", bp " + Bool(p.bp1) + ", durable " +
                      Bool(s.w_valid) + ", c " + Dec(s.c1_tag) + "/" + Dec(s.c1_gen) + " vs slot " +
                      Dec(h->shadow()->tag()) + "/" + Dec(h->shadow()->gen()) + ")");
      }
    } else {
      stall = 0;
    }

    if (granted) {
      Event e;
      e.uop = next;
      e.src_tag = s.c1_tag;
      e.src_gen = s.c1_gen;
      e.src = o.src1;
      e.value = need ? o.v1 : src_val;
      // The chain's architectural value is modelled here, independently of the
      // DUT: uop j must observe 0x1000 + j.
      Require(e.value == 0x1000ull + next, tag,
              "uop " + Dec(next) + " observed " + Hex64(e.value) + " but the chain's value is " +
                  Hex64(0x1000ull + next));
      if (next == 0) r.first_grant = now;
      r.last_grant = now;
      if (need) {
        if (o.src1 == 1) r.bypass_hits++;
        else if (o.src1 == 2) r.durable_hits++;
        else Fail(tag, "uop " + Dec(next) + " was granted with an unresolved source");
      } else {
        r.queue_roots++;
      }
      r.events.push_back(e);
      src_val = e.value + 1;
      next++;
    }
  }

  const uint64_t span = r.last_grant - r.first_grant;
  r.cycles_per_link_x1000 = (1000ull * span) / links_den;
  return r;
}

// The architectural content of an event, as a string, for the on/off comparison.
std::string EventKey(const Event& e) {
  return Dec(e.uop) + "/" + Dec(e.src_tag) + "/" + Dec(e.src_gen) + "/" + Hex64(e.value);
}

uint64_t HashMix(uint64_t hash, uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
  return hash;
}

// ------------------------------------------------------------------- phases
//
// Every phase resets first. Each cycle inside a phase is already compared
// against the shadow by Harness::Cycle; the explicit checks below state the
// phase's own claim, so a failure names the rule and not merely a pin.

void PhaseRegisteredBudget(Harness* h, mosaic::Reporter* rep) {
  h->Phase("registered-budget");
  h->Reset(4);

  const uint32_t t = 11, g = 3, ix = 5, rg = 9, uo = 2;
  const uint64_t v = 0xabcdull;

  // The producer is presented in its own execution cycle, and a consumer that
  // wants exactly that identity is waiting.
  Stim a;
  a.bp_en = true;
  a.p_valid = true;
  a.p_authorised = true;
  a.p_tag = t;
  a.p_gen = g;
  a.p_idx = ix;
  a.p_rgen = rg;
  a.p_uop = uo;
  a.p_val = v;
  a.c_valid = true;
  a.c1_tag = t;
  a.c1_gen = g;
  a.c1_need = true;
  const Observed oa = h->Cycle(a);
  Require(!oa.slot_valid && !oa.bp1 && !oa.r1 && oa.fb1, "registered-budget",
          "the producer's own cycle served the consumer (slot " + Bool(oa.slot_valid) + ", hit " +
              Bool(oa.bp1) + ", ready " + Bool(oa.r1) + ")");
  Require(oa.cap, "registered-budget", "the armed, authorised producer was not captured");
  Require(h->shadow()->slot_valid() && h->shadow()->tag() == t && h->shadow()->gen() == g &&
              h->shadow()->val() == v,
          "registered-budget", "the slot did not take the producer's identity and value");

  // The next cycle the same operand is served -- by the bypass, one cycle later.
  Stim b;
  b.bp_en = true;
  b.c_valid = true;
  b.c1_tag = t;
  b.c1_gen = g;
  b.c1_need = true;
  const Observed ob = h->Cycle(b);
  Require(ob.bp1 && ob.r1 && ob.src1 == 1 && ob.v1 == v && !ob.fb1, "registered-budget",
          "the slot did not serve the consumer one cycle later");
  Require(ob.slot_valid && ob.slot_tag == t && ob.slot_gen == g && ob.slot_idx == ix &&
              ob.slot_rgen == rg && ob.slot_uop == uo,
          "registered-budget", "the slot's identity is not the producer's full identity");

  // A new producer arriving while the slot is being read does not disturb it.
  Stim c = b;
  c.p_valid = true;
  c.p_authorised = true;
  c.p_tag = 40;
  c.p_gen = 17;
  c.p_idx = 4;
  c.p_rgen = 6;
  c.p_uop = 1;
  c.p_val = 0x7777ull;
  const Observed oc = h->Cycle(c);
  Require(oc.bp1 && oc.v1 == v, "registered-budget",
          "a producer arriving in the same cycle disturbed the value being read");
  Require(h->shadow()->tag() == 40 && h->shadow()->val() == 0x7777ull, "registered-budget",
          "the newly arriving producer was not captured");

  Stim d;
  d.bp_en = true;
  d.c_valid = true;
  d.c1_tag = 40;
  d.c1_gen = 17;
  d.c1_need = true;
  const Observed od = h->Cycle(d);
  Require(od.bp1 && od.src1 == 1 && od.v1 == 0x7777ull, "registered-budget",
          "the second producer was not forwarded from the next cycle");

  rep->Check(true, "registered-budget: the bypass is one register deep (a producer serves a "
                   "consumer from the next cycle, never its own) and the identity travels with "
                   "the value");
}

void PhaseIdentityReject(Harness* h, mosaic::Reporter* rep) {
  h->Phase("identity-reject");
  h->Reset(4);

  const uint32_t t = 20, g = 7;
  const uint64_t v = 0x1111ull;

  Stim a;
  a.bp_en = true;
  a.p_valid = true;
  a.p_authorised = true;
  a.p_tag = t;
  a.p_gen = g;
  a.p_val = v;
  h->Cycle(a);

  const uint32_t rej0 = h->shadow()->id_rejects();

  // Same tag, another generation: the identity is not the one held.
  Stim b;
  b.bp_en = true;
  b.c_valid = true;
  b.c1_tag = t;
  b.c1_gen = g + 1;
  b.c1_need = true;
  const Observed ob = h->Cycle(b);
  Require(!ob.bp1 && !ob.r1 && ob.fb1, "identity-reject",
          "a value was forwarded for the same tag at another generation");

  // Another tag, the same generation.
  Stim c = b;
  c.c1_tag = t + 1;
  c.c1_gen = g;
  const Observed oc = h->Cycle(c);
  Require(!oc.bp1 && oc.fb1, "identity-reject", "a value was forwarded for another tag");

  // The positive control: the exact identity is served, so the rejections above
  // are not just a dead bypass.
  Stim d = b;
  d.c1_tag = t;
  d.c1_gen = g;
  const Observed od = h->Cycle(d);
  Require(od.bp1 && od.src1 == 1 && od.v1 == v, "identity-reject",
          "the exact identity was not served, so the rejections above prove nothing");

  Require(h->shadow()->id_rejects() == rej0 + 1, "identity-reject",
          "identity rejections: expected exactly one more than " + Dec(rej0) + ", got " +
              Dec(h->shadow()->id_rejects()));
  rep->Check(true, "identity-reject: the match is on (tag, generation) together; a tag that "
                   "matches at another generation is rejected and counted apart from a miss");
}

void PhaseUnauthorised(Harness* h, mosaic::Reporter* rep) {
  h->Phase("unauthorised");
  h->Reset(4);

  const uint32_t t = 30, g = 9;
  const uint64_t v = 0x2222ull;

  // A producer whose grant was withdrawn: present, but not authorised.
  Stim a;
  a.bp_en = true;
  a.p_valid = true;
  a.p_authorised = false;
  a.p_tag = t;
  a.p_gen = g;
  a.p_val = v;
  a.c_valid = true;
  a.c1_tag = t;
  a.c1_gen = g;
  a.c1_need = true;
  const Observed oa = h->Cycle(a);
  Require(oa.unauth && !oa.cap && !oa.slot_valid, "unauthorised",
          "a withdrawn grant was captured into the slot");
  Require(!oa.bp1 && !oa.r1 && oa.fb1, "unauthorised",
          "a withdrawn grant's value was forwarded, or the consumer did not fall back");

  Stim b;
  b.bp_en = true;
  b.c_valid = true;
  b.c1_tag = t;
  b.c1_gen = g;
  b.c1_need = true;
  const Observed ob = h->Cycle(b);
  Require(!ob.bp1 && !ob.r1 && ob.fb1, "unauthorised",
          "the slot holds a value from an unauthorised producer");

  // The withdrawn producer's value must still reach the consumer the safe way:
  // through the register file.
  Stim c = b;
  c.w_valid = true;
  c.w_tag = t;
  c.w_gen = g;
  c.w_val = v;
  const Observed oc = h->Cycle(c);
  Require(oc.r1 && oc.src1 == 2 && oc.v1 == v, "unauthorised",
          "the register-file fallback did not resolve the operand");

  Require(h->shadow()->unauth() == 1, "unauthorised",
          "unauthorised producers counted: expected 1, got " + Dec(h->shadow()->unauth()));
  rep->Check(true, "unauthorised: a producer whose macro is not live is never captured or "
                   "forwarded, is counted, and its consumer takes the register-file path");
}

void PhaseFlushCancel(Harness* h, mosaic::Reporter* rep) {
  h->Phase("flush-cancel");
  h->Reset(4);

  const uint32_t t = 40, g = 11;
  const uint64_t v = 0x3333ull;

  Stim a;
  a.bp_en = true;
  a.p_valid = true;
  a.p_authorised = true;
  a.p_tag = t;
  a.p_gen = g;
  a.p_val = v;
  h->Cycle(a);
  Require(h->shadow()->slot_valid(), "flush-cancel", "the producer was not captured");

  Stim b;
  b.bp_en = true;
  b.c_valid = true;
  b.c1_tag = t;
  b.c1_gen = g;
  b.c1_need = true;
  const Observed ob = h->Cycle(b);
  Require(ob.bp1 && ob.src1 == 1 && ob.v1 == v, "flush-cancel",
          "the captured producer was not forwarded");

  // The redirect that squashes the producer also clears the slot.
  Stim c;
  c.bp_en = true;
  c.flush = true;
  h->Cycle(c);
  Require(!h->shadow()->slot_valid(), "flush-cancel", "the redirect did not clear the slot");

  Stim d;
  d.bp_en = true;
  d.c_valid = true;
  d.c1_tag = t;
  d.c1_gen = g;
  d.c1_need = true;
  const Observed od = h->Cycle(d);
  Require(!od.bp1 && od.fb1 && !od.r1, "flush-cancel",
          "a value survived the redirect that squashed its producer");

  // A producer captured in the same cycle as a redirect is not captured at all.
  Stim e;
  e.bp_en = true;
  e.flush = true;
  e.p_valid = true;
  e.p_authorised = true;
  e.p_tag = 41;
  e.p_gen = 12;
  e.p_val = 0x9898ull;
  const Observed oe = h->Cycle(e);
  Require(!oe.cap && !h->shadow()->slot_valid(), "flush-cancel",
          "a producer was captured in the cycle of a redirect");

  Require(h->shadow()->flush_kills() == 1, "flush-cancel",
          "squashed entries counted: expected 1, got " + Dec(h->shadow()->flush_kills()));
  rep->Check(true, "flush-cancel: a redirect clears the slot and no producer is captured "
                   "against one, so nothing the bypass serves can outlive its producer's squash");
}

void PhaseEnableOff(Harness* h, mosaic::Reporter* rep) {
  h->Phase("enable-off");
  h->Reset(4);

  const uint32_t t = 50, g = 13;
  const uint64_t v = 0x5555ull;

  Stim a;
  a.bp_en = true;
  a.p_valid = true;
  a.p_authorised = true;
  a.p_tag = t;
  a.p_gen = g;
  a.p_val = v;
  h->Cycle(a);
  Require(h->shadow()->slot_valid(), "enable-off", "the producer was not captured");

  Stim b;
  b.bp_en = false;
  b.c_valid = true;
  b.c1_tag = t;
  b.c1_gen = g;
  b.c1_need = true;
  const Observed ob = h->Cycle(b);
  Require(!ob.bp1 && !ob.r1 && ob.fb1 && ob.src1 == 0, "enable-off",
          "the bypass served an operand while disarmed");
  Require(!h->shadow()->slot_valid(), "enable-off", "disarming did not empty the slot");

  // Re-arming must not resurrect a value captured before the disable.
  Stim c = b;
  c.bp_en = true;
  const Observed oc = h->Cycle(c);
  Require(!oc.bp1 && !oc.r1, "enable-off",
          "a value captured before the disable was served after re-arming");

  // The mechanism still works after re-arming.
  Stim d;
  d.bp_en = true;
  d.p_valid = true;
  d.p_authorised = true;
  d.p_tag = t;
  d.p_gen = g;
  d.p_val = v;
  h->Cycle(d);

  Stim e = c;
  const Observed oe = h->Cycle(e);
  Require(oe.bp1 && oe.src1 == 1 && oe.v1 == v, "enable-off",
          "the bypass did not work after re-arming");

  rep->Check(true, "enable-off: with the bypass disarmed nothing is served and no operand is "
                   "resolved by this unit (all take the register-file path); re-arming serves "
                   "only a value captured afterwards");
}

void PhaseTwoSourceConflict(Harness* h, mosaic::Reporter* rep) {
  h->Phase("two-source-conflict");
  h->Reset(4);

  const uint32_t tx = 60, gx = 15;
  const uint64_t vx = 0x6666ull;
  const uint32_t ty = 70, gy = 23;
  const uint64_t vy = 0x8888ull;

  Stim a;
  a.bp_en = true;
  a.p_valid = true;
  a.p_authorised = true;
  a.p_tag = tx;
  a.p_gen = gx;
  a.p_val = vx;
  h->Cycle(a);

  // One bypass slot, two outstanding operands: it can serve the one whose
  // identity it holds, and the other must be selected for the register file.
  Stim b;
  b.bp_en = true;
  b.c_valid = true;
  b.c1_tag = tx;
  b.c1_gen = gx;
  b.c1_need = true;
  b.c2_tag = ty;
  b.c2_gen = gy;
  b.c2_need = true;
  const Observed ob = h->Cycle(b);
  Require(ob.bp1 && ob.src1 == 1 && ob.v1 == vx, "two-source-conflict",
          "the operand the slot holds was not bypassed");
  Require(!ob.bp2 && !ob.r2 && ob.fb2, "two-source-conflict",
          "the operand the slot does not hold was not left to the register file");

  // The other operand becomes durable in the same cycle: a mixed resolution.
  Stim c = b;
  c.w_valid = true;
  c.w_tag = ty;
  c.w_gen = gy;
  c.w_val = vy;
  const Observed oc = h->Cycle(c);
  Require(oc.bp1 && oc.r2 && oc.src2 == 2 && oc.v2 == vy, "two-source-conflict",
          "a cycle resolved from both paths did not report the right source per operand");

  // The durable broadcast of the *same* identity takes second place: the value
  // is already available locally.
  Stim d = b;
  d.w_valid = true;
  d.w_tag = tx;
  d.w_gen = gx;
  d.w_val = vx;
  const Observed od = h->Cycle(d);
  Require(od.bp1 && od.src1 == 1, "two-source-conflict",
          "the durable path did not defer to the bypass for an identity it already has");

  rep->Check(true, "two-source-conflict: one slot, two operands -- the bypass serves what it "
                   "holds, the rest selects the register file, and the bypass takes precedence "
                   "for an identity it already has");
}

// ------------------------------------------------------------- the two chains

void ReportChain(const ChainResult& r, const char* what, uint32_t links, bool armed) {
  std::printf("  [%s] armed=%d links=%u span=%llu cycles per-link=%llu.%03llu "
              "(bypass=%u durable=%u root=%u)\n",
              what, armed ? 1 : 0, links, (unsigned long long)(r.last_grant - r.first_grant),
              (unsigned long long)(r.cycles_per_link_x1000 / 1000ull),
              (unsigned long long)(r.cycles_per_link_x1000 % 1000ull), r.bypass_hits,
              r.durable_hits, r.queue_roots);
  std::fflush(stdout);
}

void CompareTraces(const ChainResult& on, const ChainResult& off, const char* what) {
  Require(on.events.size() == off.events.size(), "trace-agreement",
          std::string(what) + ": the two runs produced a different number of events (" +
              Dec(on.events.size()) + " vs " + Dec(off.events.size()) + ")");
  for (size_t i = 0; i < on.events.size(); i++) {
    const std::string a = EventKey(on.events[i]);
    const std::string b = EventKey(off.events[i]);
    Require(a == b, "trace-agreement",
            std::string(what) + ": event " + Dec(i) + " differs between the two runs: " + a +
                " vs " + b);
  }
}

// The seeded soak: a deliberately tiny generation space, so identity aliasing is
// constant and the rejections are not luck.
struct RandomOutcome {
  uint64_t hash = 0;
  bool cover_hit = false;
  bool cover_miss = false;
  bool cover_id = false;
  bool cover_unauth = false;
  bool cover_flush = false;
  bool cover_two_paths = false;
};

RandomOutcome RunRandom(Harness* h, uint32_t cycles, uint64_t seed) {
  mosaic::Rng rng(seed);
  const uint32_t tags[6] = {3, 3, 20, 20, 7, 40};
  uint64_t hash = 1469598103934665603ull;
  bool two_paths = false;

  for (uint32_t i = 0; i < cycles; i++) {
    Stim s;
    s.bp_en = rng.Chance(80);
    s.flush = rng.Chance(5);
    s.p_valid = rng.Chance(60);
    s.p_authorised = rng.Chance(85);
    s.p_tag = tags[rng.Below(6)];
    s.p_gen = rng.Below(4);
    s.p_idx = rng.Below(64);
    s.p_rgen = rng.Below(8);
    s.p_uop = rng.Below(8);
    s.p_val = rng.Next();
    s.w_valid = rng.Chance(50);
    s.w_tag = tags[rng.Below(6)];
    s.w_gen = rng.Below(4);
    s.w_val = rng.Next();
    s.c_valid = rng.Chance(80);
    s.c1_tag = tags[rng.Below(6)];
    s.c1_gen = rng.Below(4);
    s.c1_need = rng.Chance(70);
    s.c2_tag = tags[rng.Below(6)];
    s.c2_gen = rng.Below(4);
    s.c2_need = rng.Chance(70);

    // A quarter of the soak is deliberately the mixed case the card names as a
    // resource conflict: one operand the local slot holds (so the bypass serves
    // it when it is live) and one only the register file can supply.
    if (rng.Chance(25)) {
      s.bp_en = true;
      s.c_valid = true;
      s.c1_need = true;
      s.c1_tag = h->shadow()->tag();
      s.c1_gen = h->shadow()->gen();
      s.c2_need = true;
      s.c2_tag = 20;
      s.c2_gen = 3;
      s.w_valid = true;
      s.w_tag = 20;
      s.w_gen = 3;
      s.w_val = rng.Next();
    }

    const Observed o = h->Cycle(s);
    if ((o.src1 == 1 && o.src2 == 2) || (o.src1 == 2 && o.src2 == 1)) two_paths = true;

    uint64_t packed = 0;
    packed |= o.bp1 ? 1ull : 0ull;
    packed |= o.bp2 ? 2ull : 0ull;
    packed |= o.fb1 ? 4ull : 0ull;
    packed |= o.fb2 ? 8ull : 0ull;
    packed |= o.r1 ? 16ull : 0ull;
    packed |= o.r2 ? 32ull : 0ull;
    packed |= static_cast<uint64_t>(o.src1) << 8;
    packed |= static_cast<uint64_t>(o.src2) << 10;
    packed |= o.slot_valid ? (1ull << 12) : 0ull;
    hash = HashMix(hash, packed);
    hash = HashMix(hash, o.v1);
    hash = HashMix(hash, o.v2);
    // The slot's payload registers are deliberately not reset, so they hold
    // leftover bits while the entry is invalid; hashing them then would make the
    // run depend on history that is not part of the contract.
    if (o.slot_valid) {
      hash = HashMix(hash, o.slot_tag);
      hash = HashMix(hash, o.slot_gen);
      hash = HashMix(hash, o.slot_idx);
    }
    hash = HashMix(hash, o.hit_ctr);
    hash = HashMix(hash, o.miss_ctr);
    hash = HashMix(hash, o.unauth_ctr);
    hash = HashMix(hash, o.id_reject_ctr);
    hash = HashMix(hash, o.flush_ctr);
  }

  const Shadow* sh = h->shadow();
  RandomOutcome out;
  out.hash = hash;
  out.cover_hit = sh->hits() > 0;
  out.cover_miss = sh->misses() > 0;
  out.cover_id = sh->id_rejects() > 0;
  out.cover_unauth = sh->unauth() > 0;
  out.cover_flush = sh->flush_kills() > 0;
  out.cover_two_paths = two_paths;
  return out;
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
  Vmosaic_cluster_bypass_tb dut;

  Harness harness(&dut, &clk, options.max_cycles);

  const uint32_t kLinks = 32;    // consecutive RAW links in the measured chain
  // The competing producer is another cluster's result sharing the one
  // publication per cycle the durable path has. At one competitor per cycle the
  // path is oversubscribed (the chain completes once per two cycles while
  // disarmed), which is what makes the fallback a throughput limit rather than
  // only a fixed latency.
  const uint32_t kCompete = 1;

  std::string detail;
  bool passed = true;
  try {
    // ----------------------------------------------------------- geometry
    // The DUT must have been evaluated (and reset) before its read-back outputs
    // mean anything.
    harness.Reset(4);
    const uint32_t xlen = dut.o_xlen_o;
    const uint32_t tag_w = dut.o_tag_w_o;
    const uint32_t pgen_w = dut.o_pgen_w_o;
    const uint32_t idx_w = dut.o_idx_w_o;
    const uint32_t rgen_w = dut.o_rgen_w_o;
    const uint32_t uop_w = dut.o_uop_w_o;
    Require(xlen > 0 && tag_w > 0 && pgen_w > 0 && idx_w > 0 && rgen_w > 0 && uop_w > 0,
            "geometry", "the DUT reported a zero identity width");
    Require(tag_w < 32 && pgen_w < 32 && idx_w < 32 && rgen_w < 32 && uop_w < 32, "geometry",
            "an identity field is too wide for this driver's 32-bit stimulus pins");
    reporter.Check(true, "geometry: xlen " + Dec(xlen) + ", tag " + Dec(tag_w) + "b, gen " +
                             Dec(pgen_w) + "b, rob-index " + Dec(idx_w) + "b, rob-gen " +
                             Dec(rgen_w) + "b, uop " + Dec(uop_w) + "b");

    // ------------------------------------------------- directed mechanisms
    PhaseRegisteredBudget(&harness, &reporter);
    PhaseIdentityReject(&harness, &reporter);
    PhaseUnauthorised(&harness, &reporter);
    PhaseFlushCancel(&harness, &reporter);
    PhaseEnableOff(&harness, &reporter);
    PhaseTwoSourceConflict(&harness, &reporter);

    // -------------------------------------------------- the measured chains
    harness.Phase("raw-chain");
    const ChainResult on_uncontended = RunChain(&harness, kLinks, true, 0, "raw-chain");
    const ChainResult off_uncontended = RunChain(&harness, kLinks, false, 0, "raw-chain");
    ReportChain(on_uncontended, "raw-chain", kLinks, true);
    ReportChain(off_uncontended, "raw-chain", kLinks, false);

    Require(on_uncontended.bypass_hits == kLinks, "raw-chain",
            "the armed chain was served by the bypass for " + Dec(on_uncontended.bypass_hits) +
                " of " + Dec(kLinks) + " links, so the fast path was not exercised");
    Require(off_uncontended.bypass_hits == 0 && off_uncontended.durable_hits == kLinks,
            "raw-chain",
            "the disarmed chain used the bypass " + Dec(off_uncontended.bypass_hits) +
                " times, or did not take the register-file path for every link (" +
                Dec(off_uncontended.durable_hits) + " of " + Dec(kLinks) + ")");

    harness.Phase("raw-chain-contended");
    const ChainResult on_contended = RunChain(&harness, kLinks, true, kCompete, "raw-chain-contended");
    const ChainResult off_contended =
        RunChain(&harness, kLinks, false, kCompete, "raw-chain-contended");
    ReportChain(on_contended, "raw-chain-contended", kLinks, true);
    ReportChain(off_contended, "raw-chain-contended", kLinks, false);

    Require(on_contended.bypass_hits == kLinks, "raw-chain-contended",
            "the armed chain was served by the bypass for " + Dec(on_contended.bypass_hits) +
                " of " + Dec(kLinks) + " links");
    Require(off_contended.bypass_hits == 0, "raw-chain-contended",
            "the disarmed chain used the bypass");

    // ---------------------------------------------------- trace agreement
    harness.Phase("trace-agreement");
    CompareTraces(on_uncontended, off_uncontended, "uncontended");
    CompareTraces(on_contended, off_contended, "contended");
    Require(on_uncontended.bypass_hits != off_uncontended.bypass_hits, "trace-agreement",
            "the two runs resolved every operand by the same path, so the switch changed nothing");
    reporter.Check(true, "trace-agreement: the architectural event streams of the armed and "
                         "disarmed runs are identical (uop, source identity, value) over " +
                             Dec(on_uncontended.events.size() + on_contended.events.size()) +
                             " events; only the resolution path differs");

    const uint64_t on_u = on_uncontended.cycles_per_link_x1000;
    const uint64_t off_u = off_uncontended.cycles_per_link_x1000;
    const uint64_t on_c = on_contended.cycles_per_link_x1000;
    const uint64_t off_c = off_contended.cycles_per_link_x1000;
    std::printf("  [latency] uncontended armed=%llu.%03llu disarmed=%llu.%03llu cycles/link; "
                "contended armed=%llu.%03llu disarmed=%llu.%03llu cycles/link\n",
                (unsigned long long)(on_u / 1000ull), (unsigned long long)(on_u % 1000ull),
                (unsigned long long)(off_u / 1000ull), (unsigned long long)(off_u % 1000ull),
                (unsigned long long)(on_c / 1000ull), (unsigned long long)(on_c % 1000ull),
                (unsigned long long)(off_c / 1000ull), (unsigned long long)(off_c % 1000ull));
    if (on_c < off_c) {
      std::printf("  [latency] the bypass reduced the contended chain by %llu.%03llu "
                  "cycles/link (measured)\n",
                  (unsigned long long)((off_c - on_c) / 1000ull),
                  (unsigned long long)((off_c - on_c) % 1000ull));
    } else {
      std::printf("  [latency] FINDING: the bypass was NOT faster on this chain (%llu.%03llu vs "
                  "%llu.%03llu cycles/link)\n",
                  (unsigned long long)(on_c / 1000ull), (unsigned long long)(on_c % 1000ull),
                  (unsigned long long)(off_c / 1000ull), (unsigned long long)(off_c % 1000ull));
    }
    std::fflush(stdout);
    reporter.Check(true, "raw-chain latency (measured from the runs, not from a table): " +
                             Dec(on_u / 1000) + "." + Dec(on_u % 1000) + " vs " +
                             Dec(off_u / 1000) + "." + Dec(off_u % 1000) +
                             " cycles/link uncontended, " + Dec(on_c / 1000) + "." +
                             Dec(on_c % 1000) + " vs " + Dec(off_c / 1000) + "." +
                             Dec(off_c % 1000) + " cycles/link contended");

    // ---------------------------------------------------------- the soak
    harness.Phase("random");
    harness.Reset(4);
    const RandomOutcome soak = RunRandom(&harness, 600, options.seed);
    Require(soak.cover_hit && soak.cover_miss && soak.cover_id && soak.cover_unauth &&
                soak.cover_flush && soak.cover_two_paths,
            "random",
            "the soak did not cover every mechanism: hit " + Bool(soak.cover_hit) + " miss " +
                Bool(soak.cover_miss) + " identity-reject " + Bool(soak.cover_id) +
                " unauthorised " + Bool(soak.cover_unauth) + " flush " + Bool(soak.cover_flush) +
                " mixed-paths " + Bool(soak.cover_two_paths));
    reporter.Check(true, "random: 600 seeded cycles over a four-value generation space, every "
                         "output compared every cycle (hits " + Dec(harness.shadow()->hits()) +
                         ", misses " + Dec(harness.shadow()->misses()) + ", identity rejections " +
                         Dec(harness.shadow()->id_rejects()) + ", unauthorised " +
                         Dec(harness.shadow()->unauth()) + ", flush kills " +
                         Dec(harness.shadow()->flush_kills()) + ")");

    // ------------------------------------------------------ determinism
    harness.Phase("determinism");
    harness.Reset(4);
    const Observed cold = harness.Cycle(Stim{});
    (void)cold;
    Require(harness.shadow()->hits() == 0 && harness.shadow()->misses() == 0 &&
                harness.shadow()->id_rejects() == 0 && harness.shadow()->unauth() == 0 &&
                harness.shadow()->flush_kills() == 0,
            "determinism", "the counters are not zero after a reset");
    const RandomOutcome again = RunRandom(&harness, 600, options.seed);
    Require(soak.hash == again.hash, "determinism",
            "the same seed produced different results: " + Hex64(soak.hash) + " vs " +
                Hex64(again.hash));
    reporter.Check(true, "determinism: the same seed reproduces the run byte for byte (" +
                             Hex64(again.hash) + ")");

    detail = "bypass contract holds: " + Dec(harness.comparisons()) +
             " per-cycle comparisons over " + Dec(harness.cycles()) + " cycles; chain " +
             Dec(kLinks) + " links, uncontended " + Dec(on_u / 1000) + "." + Dec(on_u % 1000) +
             " vs " + Dec(off_u / 1000) + "." + Dec(off_u % 1000) + " cycles/link, contended " +
             Dec(on_c / 1000) + "." + Dec(on_c % 1000) + " vs " + Dec(off_c / 1000) + "." +
             Dec(off_c % 1000) + " cycles/link; seed " + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated after " + Dec(harness.comparisons()) + " comparisons: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
