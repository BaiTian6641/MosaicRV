// ============================================================================
// tb_predictor.cpp -- CASE=predictor.btb_aliasing, work package I-021.
//
// The DUT is never its own oracle. Every output is compared against an
// independent C++ shadow written from the *contract* documented in
// rtl/core/mosaic_predictor.sv, not from the RTL's structure. The shadow holds
// its own bimodal table, BTB and stack and derives the index, the tag check,
// the saturating counter and the saturating RAS push from that prose. It shares
// no code with the RTL and never looks at Verilator internals, so agreeing with
// it is evidence about the contract rather than a restatement of the
// implementation.
//
// Sizes are read from the elaborated DUT (o_bpu_entries and friends), so this
// file contains no geometry of its own: a profile with a different BTB depth
// needs no edit here, and the shadow cannot disagree with the hardware about
// how big it is.
//
// Phases, each of which can fail on its own:
//
//   1. reset-state         the documented cold state straight after reset.
//   2. btb-aliasing        two PCs colliding in the index are distinguished.
//   3. btb-no-false-miss   the converse: trained aliased PCs must NOT report a
//                          miss, so a predictor that reported everything as a
//                          miss cannot pass phase 2.
//   4. ras                 push/pop, overflow past capacity, return on empty.
//   5. ras-mispredict      a squashed call's push is corrected on flush.
//   6. training            a PC whose behaviour changes changes its prediction,
//                          in both directions.
//   7. mispredict-report   every wrong prediction is accounted for; and the
//                          phase fails if it never manages to produce one.
//   8. reset-determinism   the same program replayed after a reset gives
//                          byte-identical output.
//   9. random              a random program compared against the shadow on
//                          every cycle.
//
// Two standing invariants are checked on every cycle of every phase rather than
// in one place:
//
//   * `pred_redirect` and `pred_btb_miss` are never both high. They are
//     contradictory: one says "take this target", the other says "I have no
//     target".
//   * `pred_redirect` never rises without a reported source, i.e. without
//     either `pred_btb_hit` or `pred_ras_valid`. A redirect with no reported
//     source is a fetch that cannot be attributed and therefore cannot be
//     accounted for by the core's mispredict counter.
//
// The second invariant is the "wrong predictions are recoverable" criterion in
// mechanical form: a predictor that is wrong without saying so is worse than
// one that never predicts, so nothing here may redirect on an unreported guess.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_predictor_tb.h"

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

std::string At(const std::string& where, uint64_t cycle) {
  return where + ": cycle " + std::to_string(cycle);
}

// --------------------------------------------------------------- the shadow
//
// An independent model of the documented contract. Every field has a
// counterpart in the header prose; none of them was obtained by reading RTL.
class ShadowPredictor {
 public:
  struct EdgeReports {
    bool overflow = false;
    bool underflow = false;
  };

  ShadowPredictor(uint32_t bpu_entries, uint32_t btb_entries, uint32_t ras_entries)
      : bpu_entries_(bpu_entries),
        btb_entries_(btb_entries),
        ras_entries_(ras_entries),
        bpu_valid_(bpu_entries, 0),
        bpu_ctr_(bpu_entries, 0),
        btb_valid_(btb_entries, 0),
        btb_tag_(btb_entries, 0),
        btb_target_(btb_entries, 0),
        btb_kind_(btb_entries, 0),
        ras_mem_(ras_entries, 0),
        ras_sp_(0),
        ras_ckpt_(0) {}

  uint32_t bpu_entries() const { return bpu_entries_; }
  uint32_t btb_entries() const { return btb_entries_; }
  uint32_t ras_entries() const { return ras_entries_; }

  // The index policy from the header: bits [IDX_W:1], where bit 0 is dropped
  // because an instruction address is always 4-byte aligned. An index at or past
  // the depth is a guaranteed miss whose updates are dropped, never wrapped.
  static uint32_t IndexWidth(uint32_t entries) {
    uint32_t width = 1;
    while ((1u << width) < entries) width++;
    return width;
  }

  static uint32_t Index(uint64_t pc, uint32_t entries) {
    const uint32_t width = IndexWidth(entries);
    return static_cast<uint32_t>((pc >> 1) & ((1ull << width) - 1ull));
  }

  static bool IndexInRange(uint64_t pc, uint32_t entries) {
    return Index(pc, entries) < entries;
  }

  struct QueryResult {
    bool taken = false;
    uint64_t target = 0;
    bool redirect = false;
    bool btb_hit = false;
    bool btb_miss = false;
    bool ras_valid = false;
    bool ras_underflow = false;
  };

  // The combinational query, exactly as the contract describes it.
  QueryResult Query(uint64_t pc, bool valid, bool is_branch, bool is_jump,
                    bool is_return) const {
    QueryResult r;

    const uint32_t bpu_index = Index(pc, bpu_entries_);
    const uint32_t btb_index = Index(pc, btb_entries_);

    // An untrained entry is not-taken, so a cold table is never optimistic.
    const bool entry_taken =
        (IndexInRange(pc, bpu_entries_) && bpu_valid_[bpu_index])
            ? (bpu_ctr_[bpu_index] & 0x2u) != 0
            : false;

    // Both the tag and the kind must match: a call's target may not be served
    // for a conditional branch that happens to share the index.
    const uint8_t want_kind = is_branch ? 1 : 2;
    r.btb_hit = IndexInRange(pc, btb_entries_) && btb_valid_[btb_index] &&
                btb_tag_[btb_index] == pc && btb_kind_[btb_index] == want_kind;

    const bool ras_top_valid = ras_sp_ != 0;
    r.ras_valid = valid && is_jump && is_return && ras_top_valid;
    r.ras_underflow = valid && is_jump && is_return && !ras_top_valid;

    r.taken =
        (valid && (is_branch || is_jump)) ? (is_branch ? entry_taken : true) : false;

    r.redirect = valid && (is_branch || is_jump) &&
                 (r.ras_valid || (r.btb_hit && (is_jump || entry_taken)));

    if (r.ras_valid) {
      r.target = ras_mem_[ras_sp_ - 1];
    } else if (r.btb_hit) {
      r.target = btb_target_[btb_index];
    } else {
      r.target = pc + 4;
    }

    r.btb_miss = valid && (is_branch || is_jump) && !r.ras_valid && !r.btb_hit;
    return r;
  }

  // One resolved transfer, applied at the rising edge.
  EdgeReports Update(uint64_t pc, bool valid, bool is_branch, bool is_jump, bool is_call,
                     bool is_return, bool taken, uint64_t target, bool ckpt_valid,
                     bool flush) {
    const uint32_t bpu_index = Index(pc, bpu_entries_);
    const uint32_t btb_index = Index(pc, btb_entries_);

    const bool is_ctrl = valid && (is_branch || is_jump);
    const bool trains_direction = is_ctrl && is_branch;
    const bool writes_btb = is_ctrl && (is_jump || taken);
    const bool ras_push = is_ctrl && is_call;
    const bool ras_pop = is_ctrl && is_return;

    const bool ras_full = ras_sp_ == ras_entries_;
    const bool ras_empty = ras_sp_ == 0;

    EdgeReports reports;
    reports.overflow = ras_push && !flush && ras_full;
    reports.underflow = ras_pop && !flush && ras_empty;

    // Both the checkpoint save, the pointer update and the pushed word all read
    // the *pre-edge* pointer, because in the RTL they are non-blocking
    // assignments. That matters twice over:
    //
    //   * when `flush` and `ckpt_valid` arrive together, the restore uses the
    //     checkpoint as it was before this edge, not the one written this edge;
    //   * a push stores at the entry the pointer named *before* it advanced, so
    //     the stored word and the pointer can never disagree.
    //
    // Both are modelled here from one pre-edge copy, which is the only way to
    // keep the shadow honest about non-blocking semantics.
    const uint32_t sp_pre = ras_sp_;

    uint32_t sp_next = sp_pre;
    if (flush) {
      sp_next = ras_ckpt_;
    } else if (ras_push && !ras_full) {
      sp_next = sp_pre + 1;
    } else if (ras_pop && !ras_empty) {
      sp_next = sp_pre - 1;
    }

    if (ckpt_valid) ras_ckpt_ = sp_pre;

    if (!flush && ras_push && !ras_full) {
      ras_mem_[sp_pre] = pc + 4;
    }

    ras_sp_ = sp_next;

    if (trains_direction && IndexInRange(pc, bpu_entries_) && !flush) {
      // The first observation writes an absolute value, because the counter
      // array is never reset and an increment from power-up contents would make
      // the first trained state depend on the silicon rather than on the
      // branch. Every later update saturates from a value written here.
      uint32_t ctr = bpu_ctr_[bpu_index];
      if (!bpu_valid_[bpu_index]) {
        ctr = taken ? 2 : 1;
      } else {
        ctr = taken ? ((ctr == 3) ? 3 : ctr + 1) : ((ctr == 0) ? 0 : ctr - 1);
      }
      bpu_ctr_[bpu_index] = static_cast<uint8_t>(ctr);
      bpu_valid_[bpu_index] = 1;
    }

    if (writes_btb && IndexInRange(pc, btb_entries_) && !flush) {
      btb_tag_[btb_index] = pc;
      btb_target_[btb_index] = target;
      btb_kind_[btb_index] = is_branch ? 1 : 2;
      btb_valid_[btb_index] = 1;
    }

    return reports;
  }

  void Reset() {
    std::fill(bpu_valid_.begin(), bpu_valid_.end(), 0);
    std::fill(bpu_ctr_.begin(), bpu_ctr_.end(), 0);
    std::fill(btb_valid_.begin(), btb_valid_.end(), 0);
    std::fill(btb_tag_.begin(), btb_tag_.end(), 0);
    std::fill(btb_target_.begin(), btb_target_.end(), 0);
    std::fill(btb_kind_.begin(), btb_kind_.end(), 0);
    std::fill(ras_mem_.begin(), ras_mem_.end(), 0);
    ras_sp_ = 0;
    ras_ckpt_ = 0;
  }

  // Current stack depth, so a phase can assert the RAS came back to a known
  // state after a flush instead of only inferring it from a prediction.
  uint32_t ras_depth() const { return ras_sp_; }

 private:
  uint32_t bpu_entries_;
  uint32_t btb_entries_;
  uint32_t ras_entries_;
  std::vector<uint8_t> bpu_valid_;
  std::vector<uint8_t> bpu_ctr_;
  std::vector<uint8_t> btb_valid_;
  std::vector<uint64_t> btb_tag_;
  std::vector<uint64_t> btb_target_;
  std::vector<uint8_t> btb_kind_;
  std::vector<uint64_t> ras_mem_;
  uint32_t ras_sp_;
  uint32_t ras_ckpt_;
};

// ------------------------------------------------------------- the stimulus
struct Stim {
  uint64_t q_pc = 0;
  bool q_valid = false;
  bool q_is_branch = false;
  bool q_is_jump = false;
  bool q_is_return = false;

  bool upd_valid = false;
  uint64_t upd_pc = 0;
  bool upd_is_branch = false;
  bool upd_is_jump = false;
  bool upd_is_call = false;
  bool upd_is_return = false;
  bool upd_taken = false;
  uint64_t upd_target = 0;

  bool ckpt_valid = false;
  bool flush = false;
};

// What the DUT actually presented in the cycle just run, kept so a phase can
// make a decision about the prediction rather than only comparing it.
struct Observed {
  bool pred_taken = false;
  uint64_t pred_target = 0;
  bool pred_redirect = false;
  bool pred_btb_hit = false;
  bool pred_btb_miss = false;
  bool pred_ras_valid = false;
  bool pred_ras_underflow = false;
  bool ras_overflow = false;
  bool ras_underflow = false;
};

// ------------------------------------------------------------------ harness
//
// Owns the clock, the reset schedule, the comparison against the shadow, and
// the two standing invariants.
class Harness {
 public:
  Harness(Vmosaic_predictor_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }

  // The shadow this harness advances. Must be bound after every Reset().
  void BindShadow(ShadowPredictor* shadow) { shadow_ = shadow; }

  // Assert reset for `cycles` rising edges, with no other stimulus. After this
  // returns the DUT is in its documented cold state.
  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) {
      Cycle(Stim{}, /*rst=*/true);
    }
  }

  // One full clock period. The query and update ports are evaluated while the
  // clock is low -- the query port is combinational, and the update reports
  // describe the edge that has not happened yet -- then the shadow is advanced,
  // then the edge is applied.
  const Observed& Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + std::to_string(max_cycles_) +
                       ") exhausted before the campaign finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->q_pc = s.q_pc;
    dut_->q_valid = s.q_valid;
    dut_->q_is_branch = s.q_is_branch;
    dut_->q_is_jump = s.q_is_jump;
    dut_->q_is_return = s.q_is_return;
    dut_->upd_valid = s.upd_valid;
    dut_->upd_pc = s.upd_pc;
    dut_->upd_is_branch = s.upd_is_branch;
    dut_->upd_is_jump = s.upd_is_jump;
    dut_->upd_is_call = s.upd_is_call;
    dut_->upd_is_return = s.upd_is_return;
    dut_->upd_taken = s.upd_taken;
    dut_->upd_target = s.upd_target;
    dut_->ckpt_valid = s.ckpt_valid;
    dut_->flush = s.flush;
    dut_->eval();

    const std::string where = At(phase_, clk_->cycle());

    // During reset the outputs are the pre-reset state by contract, so only the
    // post-reset cycles are compared against the shadow.
    if (!rst) {
      CheckQuery(s, where);
      CheckUpdateReports(s, where);
      shadow_->Update(s.upd_pc, s.upd_valid, s.upd_is_branch, s.upd_is_jump,
                      s.upd_is_call, s.upd_is_return, s.upd_taken, s.upd_target,
                      s.ckpt_valid, s.flush);
    }

    Capture();

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
    return observed_;
  }

  const Observed& observed() const { return observed_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }

 private:
  void CheckQuery(const Stim& s, const std::string& where) {
    const auto expect =
        shadow_->Query(s.q_pc, s.q_valid, s.q_is_branch, s.q_is_jump, s.q_is_return);

    // The stimulus goes into every mismatch message. Without it a failure in a
    // random campaign names a cycle number and nothing a reader can act on.
    const std::string stim =
        " [q_pc=" + mosaic::Hex(s.q_pc) + " qv=" + Bool(s.q_valid) + " qbr=" +
        Bool(s.q_is_branch) + " qjmp=" + Bool(s.q_is_jump) + " qret=" +
        Bool(s.q_is_return) + " | upd_pc=" + mosaic::Hex(s.upd_pc) + " uv=" +
        Bool(s.upd_valid) + " ubr=" + Bool(s.upd_is_branch) + " ujmp=" +
        Bool(s.upd_is_jump) + " ucall=" + Bool(s.upd_is_call) + " uret=" +
        Bool(s.upd_is_return) + " utaken=" + Bool(s.upd_taken) + " ckpt=" +
        Bool(s.ckpt_valid) + " flush=" + Bool(s.flush) + "]";

    Require(dut_->pred_taken == expect.taken, where,
            "pred_taken: expected " + Bool(expect.taken) + ", got " +
                Bool(dut_->pred_taken) + stim);
    Require(dut_->pred_target == expect.target, where,
            "pred_target: expected " + mosaic::Hex(expect.target) + ", got " +
                mosaic::Hex(dut_->pred_target) + stim);
    Require(dut_->pred_redirect == expect.redirect, where,
            "pred_redirect: expected " + Bool(expect.redirect) + ", got " +
                Bool(dut_->pred_redirect) + stim);
    Require(dut_->pred_btb_hit == expect.btb_hit, where,
            "pred_btb_hit: expected " + Bool(expect.btb_hit) + ", got " +
                Bool(dut_->pred_btb_hit) + stim);
    Require(dut_->pred_btb_miss == expect.btb_miss, where,
            "pred_btb_miss: expected " + Bool(expect.btb_miss) + ", got " +
                Bool(dut_->pred_btb_miss) + stim);
    Require(dut_->pred_ras_valid == expect.ras_valid, where,
            "pred_ras_valid: expected " + Bool(expect.ras_valid) + ", got " +
                Bool(dut_->pred_ras_valid) + stim);
    Require(dut_->pred_ras_underflow == expect.ras_underflow, where,
            "pred_ras_underflow: expected " + Bool(expect.ras_underflow) + ", got " +
                Bool(dut_->pred_ras_underflow) + stim);

    // Standing invariant 1: the two reports are contradictory and must never
    // both be asserted.
    Require(!(dut_->pred_redirect && dut_->pred_btb_miss), where,
            "pred_redirect and pred_btb_miss are both high: the predictor claims a "
            "target and no target at the same time");

    // Standing invariant 2: a redirect must always name where its target came
    // from, so the core can attribute the prediction and count a mispredict
    // against the structure that produced it.
    Require(!(dut_->pred_redirect && !dut_->pred_btb_hit && !dut_->pred_ras_valid),
            where,
            "pred_redirect is high with neither pred_btb_hit nor pred_ras_valid: a "
            "silent redirect that cannot be accounted for");

    ++comparisons_;
  }

  void CheckUpdateReports(const Stim& s, const std::string& where) {
    // Recomputed from the shadow's pre-edge state, because the reports describe
    // the edge that has not happened yet.
    const bool expect_overflow = s.upd_valid && s.upd_is_call && !s.flush &&
                                 shadow_->ras_depth() == shadow_->ras_entries();
    const bool expect_underflow = s.upd_valid && s.upd_is_return && !s.flush &&
                                  shadow_->ras_depth() == 0;

    Require(dut_->ras_overflow == expect_overflow, where,
            "ras_overflow: expected " + Bool(expect_overflow) + ", got " +
                Bool(dut_->ras_overflow));
    Require(dut_->ras_underflow == expect_underflow, where,
            "ras_underflow: expected " + Bool(expect_underflow) + ", got " +
                Bool(dut_->ras_underflow));
    ++comparisons_;
  }

  void Capture() {
    observed_.pred_taken = dut_->pred_taken != 0;
    observed_.pred_target = dut_->pred_target;
    observed_.pred_redirect = dut_->pred_redirect != 0;
    observed_.pred_btb_hit = dut_->pred_btb_hit != 0;
    observed_.pred_btb_miss = dut_->pred_btb_miss != 0;
    observed_.pred_ras_valid = dut_->pred_ras_valid != 0;
    observed_.pred_ras_underflow = dut_->pred_ras_underflow != 0;
    observed_.ras_overflow = dut_->ras_overflow != 0;
    observed_.ras_underflow = dut_->ras_underflow != 0;
  }

  Vmosaic_predictor_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::string phase_ = "init";
  ShadowPredictor* shadow_ = nullptr;
  Observed observed_;
};

// ------------------------------------------------------------------- phases

// Stimulus helpers, so each phase reads as the sequence it is testing.
Stim Query(uint64_t pc, bool is_branch, bool is_jump, bool is_return) {
  Stim s;
  s.q_pc = pc;
  s.q_valid = true;
  s.q_is_branch = is_branch;
  s.q_is_jump = is_jump;
  s.q_is_return = is_return;
  return s;
}

Stim Resolve(uint64_t pc, bool is_branch, bool is_jump, bool is_call, bool is_return,
             bool taken, uint64_t target, bool ckpt_valid = false, bool flush = false) {
  Stim s;
  s.upd_valid = true;
  s.upd_pc = pc;
  s.upd_is_branch = is_branch;
  s.upd_is_jump = is_jump;
  s.upd_is_call = is_call;
  s.upd_is_return = is_return;
  s.upd_taken = taken;
  s.upd_target = target;
  s.ckpt_valid = ckpt_valid;
  s.flush = flush;
  return s;
}

// Phase 1: the documented cold state.
void PhaseResetState(Harness* harness, mosaic::Reporter* reporter) {
  harness->Phase("reset-state");
  // A cold predictor answers every branch with not-taken, a reported miss, and
  // the sequential target. Anything else would be a prediction with no evidence
  // behind it, which is the optimistic bias the contract forbids.
  const Observed& obs = harness->Cycle(Query(0x8000'0100, true, false, false));
  Require(!obs.pred_taken, "reset-state", "a cold entry predicted taken");
  Require(obs.pred_btb_miss, "reset-state", "a cold entry did not report a BTB miss");
  Require(obs.pred_target == 0x8000'0104, "reset-state",
          "a cold entry did not return the sequential target");
  Require(!obs.pred_redirect, "reset-state", "a cold entry asked for a redirect");
  reporter->Check(true, "reset-state: cold entries are not-taken and report a miss");
}

// Phase 2 and 3: aliasing must be distinguished, and a trained pair must not be
// reported as a miss.
void PhaseBtbAliasing(Harness* harness, mosaic::Reporter* reporter, uint32_t btb_entries) {
  harness->Phase("btb-aliasing");

  const uint64_t pc_a = 0x8000'0200;
  const uint32_t width = ShadowPredictor::IndexWidth(btb_entries);
  // One full index period up keeps the index identical and makes the tag differ,
  // which is exactly the aliasing this card names. Derived from the depth, so the
  // phase still collides if the geometry changes.
  const uint64_t pc_b = pc_a + (1ull << (width + 1));
  Require(ShadowPredictor::Index(pc_a, btb_entries) ==
              ShadowPredictor::Index(pc_b, btb_entries),
          "btb-aliasing",
          "the two chosen PCs do not collide in the index, so this phase would "
          "prove nothing");
  Require(ShadowPredictor::IndexInRange(pc_b, btb_entries), "btb-aliasing",
          "the aliasing partner falls outside the indexable range, so it could never "
          "be allocated");

  const uint64_t target_a = 0x8000'0300;
  const uint64_t target_b = 0x8000'0400;

  // Before either is trained, both must report a miss.
  harness->Cycle(Query(pc_a, true, false, false));
  Require(harness->observed().pred_btb_miss, "btb-aliasing",
          "an untrained branch did not report a miss");
  harness->Cycle(Query(pc_b, true, false, false));
  Require(harness->observed().pred_btb_miss, "btb-aliasing",
          "an untrained branch did not report a miss");

  // Train A taken and B not-taken, several times each, so A's entry holds
  // target_a and B's counter is pushed down.
  for (int i = 0; i < 4; i++) {
    harness->Cycle(Resolve(pc_a, true, false, false, false, true, target_a));
    harness->Cycle(Resolve(pc_b, true, false, false, false, false, 0));
  }

  // A now hits and returns its own target. The direction is deliberately *not*
  // asserted here: the bimodal table is PhaseTraining's subject, and each phase
  // owns exactly one mechanism so that a failure names the thing that broke
  // rather than the first thing that happened to notice.
  const Observed& a = harness->Cycle(Query(pc_a, true, false, false));
  Require(a.pred_btb_hit, "btb-aliasing", "the trained branch missed its own entry");
  Require(a.pred_target == target_a, "btb-aliasing",
          "the trained branch returned " + mosaic::Hex(a.pred_target) + ", expected " +
              mosaic::Hex(target_a));

  // The discriminating case: B shares A's index, its tag differs, so it must be
  // reported as a miss and must not return A's target. A predictor that ignores
  // the tag returns target_a here with no miss at all.
  const Observed& b = harness->Cycle(Query(pc_b, true, false, false));
  Require(b.pred_btb_miss, "btb-aliasing",
          "the aliasing partner was NOT reported as a miss: a missing tag comparison "
          "would produce exactly this");
  Require(!b.pred_btb_hit, "btb-aliasing", "the aliasing partner claimed a hit");
  Require(b.pred_target != target_a, "btb-aliasing",
          "the aliasing partner was handed the other branch's target " +
              mosaic::Hex(target_a));
  Require(b.pred_target == pc_b + 4, "btb-aliasing",
          "a missed branch did not return the sequential target");

  // The converse, and the reason phase 2 alone is not enough: once retrained, B
  // has its own entry and must hit. A predictor that reported a miss for every
  // query would have passed the check above.
  harness->Cycle(Resolve(pc_b, true, false, false, false, true, target_b));
  const Observed& b_hit = harness->Cycle(Query(pc_b, true, false, false));
  Require(b_hit.pred_btb_hit, "btb-no-false-miss",
          "a branch with a trained entry reported a miss: this phase would pass on a "
          "predictor that reports every query as a miss");
  Require(!b_hit.pred_btb_miss, "btb-no-false-miss",
          "a branch with a trained entry reported a miss");
  Require(b_hit.pred_target == target_b, "btb-no-false-miss",
          "the retrained branch returned " + mosaic::Hex(b_hit.pred_target) +
              ", expected " + mosaic::Hex(target_b));

  // A third PC two index periods above A -- same index as both A and B, but a
  // tag matching neither -- must still miss. This is the check that a stale
  // entry is not simply left readable. The step is a whole index period rather
  // than +4, because bit 1 of the address is part of the index.
  const uint64_t pc_c = pc_a + 2 * (1ull << (width + 1));
  const Observed& c = harness->Cycle(Query(pc_c, true, false, false));
  Require(ShadowPredictor::Index(pc_c, btb_entries) ==
              ShadowPredictor::Index(pc_b, btb_entries),
          "btb-no-false-miss", "the untrained probe does not share the index");
  Require(c.pred_btb_miss, "btb-no-false-miss",
          "an untrained PC sharing an occupied index did not report a miss");

  reporter->Check(true, "btb-aliasing: colliding PCs are distinguished, trained ones hit");
}

// Phase 4: RAS push/pop, overflow past capacity, and a return on an empty stack.
void PhaseRas(Harness* harness, mosaic::Reporter* reporter, uint32_t ras_entries) {
  harness->Phase("ras");

  // Push well past capacity. The surplus pushes must be reported as overflow and
  // must leave the pointer alone, so the stack saturates rather than wrapping
  // into a plausible but wrong return address.
  const uint64_t overflow_count = 4;
  int overflows_seen = 0;
  for (uint64_t i = 0; i < ras_entries + overflow_count; i++) {
    const Observed& obs =
        harness->Cycle(Resolve(0x8000'1000 + i * 4, false, true, true, false, true, 0));
    if (obs.ras_overflow) ++overflows_seen;
  }
  Require(overflows_seen == static_cast<int>(overflow_count), "ras",
          "expected " + std::to_string(overflow_count) + " reported overflows, saw " +
              std::to_string(overflows_seen));

  // Pop the whole stack through query-time returns: each must hand back the
  // address pushed for that call, deepest last.
  for (uint64_t i = ras_entries; i > 0; i--) {
    const uint64_t call_pc = 0x8000'1000 + (i - 1) * 4;
    const uint64_t expected_return = call_pc + 4;
    const Observed& obs = harness->Cycle(Query(0x8000'9000, false, true, true));
    Require(obs.pred_ras_valid, "ras",
            "a return query on a non-empty stack did not report the RAS as the source");
    Require(obs.pred_target == expected_return, "ras",
            "return " + std::to_string(i) + " produced " + mosaic::Hex(obs.pred_target) +
                ", expected the pushed address " + mosaic::Hex(expected_return));
    harness->Cycle(Resolve(0x8000'9000, false, true, false, true, true, 0));
  }

  // The stack is now empty. A resolve-time return must be reported and must not
  // move the pointer below zero.
  const Observed& empty_resolve =
      harness->Cycle(Resolve(0x8000'9100, false, true, false, true, true, 0));
  Require(empty_resolve.ras_underflow, "ras",
          "a return on an empty stack was not reported as an underflow");

  // And a query-time return on an empty stack must report the underflow and fall
  // back rather than inventing an address.
  const Observed& empty_query = harness->Cycle(Query(0x8000'9200, false, true, true));
  Require(empty_query.pred_ras_underflow, "ras",
          "a return query on an empty stack did not report the underflow");
  Require(!empty_query.pred_ras_valid, "ras",
          "a return query on an empty stack claimed a RAS target");

  reporter->Check(true, "ras: push/pop, saturating overflow, empty-stack return");
}

// Phase 5: a squashed call's speculative push must be corrected by a flush.
void PhaseRasMispredict(Harness* harness, mosaic::Reporter* reporter, uint32_t ras_entries) {
  harness->Phase("ras-mispredict");

  // One committed call leaves the stack at depth 1.
  harness->Cycle(Resolve(0x8000'5000, false, true, true, false, true, 0x8000'6000));

  // A second call is offered with a checkpoint taken in the same cycle, so its
  // RAS effect is undoable.
  harness->Cycle(Resolve(0x8000'5010, false, true, true, false, true, 0x8000'6100,
                         /*ckpt_valid=*/true));

  // A flush cycle: recovery only, no stimulus of its own.
  Stim flush;
  flush.flush = true;
  harness->Cycle(flush);

  // After the flush the stack must be back to depth 1. The next return reads the
  // top entry, which is the *committed* call's return address. A predictor that
  // ignores flush keeps the squashed entry and returns the wrong address here,
  // which is the silently-wrong-fetch this phase exists to catch.
  const uint64_t committed_return = 0x8000'5004;
  const Observed& obs = harness->Cycle(Query(0x8000'7000, false, true, true));
  Require(obs.pred_ras_valid, "ras-mispredict", "the return after a flush did not come "
                                               "from the RAS");
  Require(obs.pred_target == committed_return, "ras-mispredict",
          "after a flush the RAS returned " + mosaic::Hex(obs.pred_target) +
              ", expected the committed call's address " + mosaic::Hex(committed_return));

  // Drain the stack so nothing leaks into a later phase.
  for (uint32_t i = 0; i < ras_entries + 2; i++) {
    harness->Cycle(Resolve(0x8000'7000, false, true, false, true, true, 0));
  }

  reporter->Check(true, "ras-mispredict: a squashed push is corrected by flush");
}

// Phase 6: the prediction must follow a branch whose behaviour changes, in both
// directions.
void PhaseTraining(Harness* harness, mosaic::Reporter* reporter) {
  harness->Phase("training");
  const uint64_t pc = 0x8000'8000;
  const uint64_t taken_target = 0x8000'9000;

  const Observed& cold = harness->Cycle(Query(pc, true, false, false));
  Require(!cold.pred_taken, "training", "a cold branch predicted taken");

  // Train taken. A static predictor never becomes taken and fails here.
  bool became_taken = false;
  for (int i = 0; i < 4; i++) {
    harness->Cycle(Resolve(pc, true, false, false, false, true, taken_target));
    if (harness->Cycle(Query(pc, true, false, false)).pred_taken) became_taken = true;
  }
  Require(became_taken, "training",
          "four consecutive taken resolves never produced a taken prediction: the "
          "direction table is not training");

  // Train not-taken. The counter has to move *down* as well, which is the half a
  // one-sided training test would miss.
  bool became_not_taken = false;
  for (int i = 0; i < 4; i++) {
    harness->Cycle(Resolve(pc, true, false, false, false, false, 0));
    if (!harness->Cycle(Query(pc, true, false, false)).pred_taken) became_not_taken = true;
  }
  Require(became_not_taken, "training",
          "four consecutive not-taken resolves never produced a not-taken prediction: "
          "the counter never moves down");

  reporter->Check(true, "training: the prediction follows the branch both ways");
}

// Phase 7: wrong predictions must be reported, and the phase must fail if it
// never manages to produce one.
void PhaseMispredictReport(Harness* harness, mosaic::Reporter* reporter) {
  harness->Phase("mispredict-report");
  const uint64_t pc = 0x8000'a000;
  const uint64_t taken_target = 0x8000'b000;

  // The stimulus has to be one the table provably *cannot* follow. A plain
  // alternation is followed exactly by a 2-bit counter -- every not-taken step
  // takes the counter down to 01 and every taken step back up to 10 -- so a
  // perfect-alternation phase produces zero mispredicts and would pass a
  // vacuous check. Driving three taken resolves to saturate the counter at 11
  // and then a single not-taken is the shortest sequence that forces a wrong
  // prediction: the counter steps 11 -> 10, still weakly taken.
  auto settle = [&](int taken_runs, bool final_taken) {
    for (int i = 0; i < taken_runs; i++) {
      harness->Cycle(Resolve(pc, true, false, false, false, true, taken_target));
      harness->Cycle(Query(pc, true, false, false));
    }
    harness->Cycle(Resolve(pc, true, false, false, false, final_taken,
                           final_taken ? taken_target : 0));
    return harness->Cycle(Query(pc, true, false, false));
  };

  int wrong = 0;
  int silent = 0;
  int redirects = 0;

  // Three such episodes, so a single coincidence cannot carry the phase.
  for (int episode = 0; episode < 3; episode++) {
    const Observed& obs = settle(3, /*final_taken=*/false);
    const bool outcome = false;
    if (obs.pred_redirect) ++redirects;
    if (obs.pred_taken != outcome) {
      ++wrong;
      // A wrong prediction issued as a redirect must still name its source, so
      // the core can count a mispredict against the structure that produced it.
      if (!obs.pred_btb_hit && !obs.pred_ras_valid) ++silent;
    }
  }

  Require(wrong > 0, "mispredict-report",
          "the phase produced no wrong prediction at all, so it proves nothing: the "
          "stimulus has to make the predictor wrong");
  Require(silent == 0, "mispredict-report",
          std::to_string(silent) +
              " wrong prediction(s) were issued as a redirect with no reported source, "
              "so the core could not account for them");
  Require(redirects > 0, "mispredict-report",
          "no wrong prediction was issued as a redirect at all, so the reporting path "
          "for an actual mispredict was never exercised");

  reporter->Check(true, "mispredict-report: wrong predictions are reported and counted");
}

// Phase 8: reset determinism. The same program replayed after a reset must
// produce byte-identical output.
void PhaseResetDeterminism(Harness* harness, mosaic::Reporter* reporter,
                           uint32_t bpu_entries, uint32_t btb_entries,
                           uint32_t ras_entries, uint64_t seed) {
  harness->Phase("reset-determinism");

  // The stimulus comes from a generator seeded identically for both runs, so the
  // replay is the same program and not merely a similar one.
  auto run_program = [&](ShadowPredictor* shadow, std::string* transcript) {
    mosaic::Rng rng(seed);
    harness->BindShadow(shadow);
    for (int step = 0; step < 250; step++) {
      const uint64_t pc = 0x9000'0000ull + (rng.Next() % 64) * 4;
      const bool is_branch = rng.Chance(60);
      const bool is_jump = !is_branch;
      const bool is_call = is_jump && rng.Chance(40);
      const bool is_return = is_jump && !is_call && rng.Chance(50);
      const bool taken = rng.Chance(70);
      const uint64_t target = pc + 4 + (rng.Next() % 16) * 4;
      const bool ckpt = rng.Chance(20);
      const bool flush = rng.Chance(20);

      if (rng.Chance(50)) {
        const Observed& obs = harness->Cycle(Query(pc, is_branch, is_jump, is_return));
        *transcript += "Q " + mosaic::Hex(pc) + " " + Bool(is_branch) + Bool(is_jump) +
                       Bool(is_return) + " -> " + Bool(obs.pred_taken) + " " +
                       mosaic::Hex(obs.pred_target) + " hit=" + Bool(obs.pred_btb_hit) +
                       " miss=" + Bool(obs.pred_btb_miss) + " ras=" +
                       Bool(obs.pred_ras_valid) + " redir=" + Bool(obs.pred_redirect) +
                       "\n";
      } else {
        const Observed& obs = harness->Cycle(
            Resolve(pc, is_branch, is_jump, is_call, is_return, taken, target, ckpt, flush));
        *transcript += "U " + mosaic::Hex(pc) + " " + Bool(taken) + " " +
                       mosaic::Hex(target) + " ov=" + Bool(obs.ras_overflow) + " un=" +
                       Bool(obs.ras_underflow) + "\n";
      }
    }
  };

  ShadowPredictor first(bpu_entries, btb_entries, ras_entries);
  harness->Reset(4);
  harness->BindShadow(&first);
  std::string first_transcript;
  run_program(&first, &first_transcript);

  // Reset the DUT and rebuild the model, then replay the identical program.
  harness->Reset(4);
  ShadowPredictor second(bpu_entries, btb_entries, ras_entries);
  harness->BindShadow(&second);
  std::string second_transcript;
  run_program(&second, &second_transcript);

  Require(!first_transcript.empty(), "reset-determinism", "the program produced no output");
  Require(first_transcript == second_transcript, "reset-determinism",
          "replaying the same program after a reset produced different output: the "
          "predictor is not in a defined state after reset");

  reporter->Check(true, "reset-determinism: the replay is byte-identical");
}

// Phase 9: a random program, compared against the shadow on every cycle.
void PhaseRandom(Harness* harness, mosaic::Reporter* reporter, uint32_t bpu_entries,
                 uint32_t btb_entries, uint32_t ras_entries, uint64_t seed) {
  harness->Phase("random");

  mosaic::Rng rng(seed ^ 0x5bf0'3635ull);
  ShadowPredictor shadow(bpu_entries, btb_entries, ras_entries);
  harness->BindShadow(&shadow);

  int misses = 0;
  int redirects = 0;
  for (int step = 0; step < 4000; step++) {
    const uint64_t pc = 0x8000'0000ull + (rng.Next() % 256) * 4;
    const bool is_branch = rng.Chance(55);
    const bool is_jump = !is_branch;
    const bool is_call = is_jump && rng.Chance(35);
    const bool is_return = is_jump && !is_call && rng.Chance(45);
    const bool taken = rng.Chance(65);
    const uint64_t target = pc + 4 + (rng.Next() % 32) * 4;

    if (rng.Chance(55)) {
      const Observed& obs = harness->Cycle(Query(pc, is_branch, is_jump, is_return));
      if (obs.pred_btb_miss) ++misses;
      if (obs.pred_redirect) ++redirects;
    } else {
      harness->Cycle(Resolve(pc, is_branch, is_jump, is_call, is_return, taken, target,
                             rng.Chance(15), rng.Chance(15)));
    }
  }

  // The campaign has to be doing something: a run in which nothing ever hit and
  // nothing ever missed would pass a shadow comparison vacuously.
  Require(redirects > 0, "random",
          "no redirect was ever predicted: the campaign never exercised a hit");
  Require(misses > 0, "random",
          "no miss was ever reported: the campaign never exercised the cold path");

  reporter->Check(true, "random: the shadow matched the DUT on every cycle");
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
  Vmosaic_predictor_tb dut;

  Harness harness(&dut, &clk, options.max_cycles);

  std::string detail;
  bool passed = true;
  try {
    // Bring the DUT out of reset, then read the geometry back off the elaborated
    // instance. The shadow is sized from these numbers and from nothing else.
    harness.Reset(4);

    const uint32_t bpu_entries = dut.o_bpu_entries;
    const uint32_t btb_entries = dut.o_btb_entries;
    const uint32_t ras_entries = dut.o_ras_entries;
    Require(bpu_entries > 0 && btb_entries > 0 && ras_entries > 0, "geometry",
            "the DUT reported a zero depth, so the shadow cannot be sized");
    Require(dut.o_xlen == 64, "geometry", "expected a 64-bit XLEN");

    // Each phase gets a freshly reset DUT and a freshly reset shadow, so no phase
    // can pass on state left behind by the one before it.
    ShadowPredictor shadow(bpu_entries, btb_entries, ras_entries);
    auto fresh = [&]() {
      harness.Reset(4);
      shadow.Reset();
      harness.BindShadow(&shadow);
    };

    // Phase order is a deliberate choice, not an accident. Each phase resets
    // first and owns exactly one mechanism, and a run stops at the first
    // failure, so the order decides *which* phase reports a given defect. The
    // directed phases therefore run before the two that are sensitive to
    // everything:
    //
    //   * reset-determinism replays a whole program and compares transcripts, so
    //     it fails on *any* state divergence -- it would mask every other
    //     phase's diagnosis if it ran first. It runs after the phases that can
    //     name the specific structure at fault.
    //   * the random soak is last for the same reason, and because it is the
    //     phase whose job is breadth rather than attribution.
    //
    // The consequence is stated plainly in the report: a predictor whose reset
    // leaves stale state behind is caught by the first phase that depends on
    // cleared state, which is btb-aliasing, not by reset-determinism. That is
    // the same root cause under a different name, not a missed detection.
    fresh();
    PhaseResetState(&harness, &reporter);

    fresh();
    PhaseBtbAliasing(&harness, &reporter, btb_entries);

    fresh();
    PhaseRas(&harness, &reporter, ras_entries);

    fresh();
    PhaseRasMispredict(&harness, &reporter, ras_entries);

    fresh();
    PhaseTraining(&harness, &reporter);

    fresh();
    PhaseMispredictReport(&harness, &reporter);

    PhaseResetDeterminism(&harness, &reporter, bpu_entries, btb_entries, ras_entries,
                          options.seed);

    fresh();
    PhaseRandom(&harness, &reporter, bpu_entries, btb_entries, ras_entries, options.seed);

    detail = "predictor contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons over " + std::to_string(harness.cycles()) +
             " cycles, seed " + std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
