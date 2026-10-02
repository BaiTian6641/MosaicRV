// ============================================================================
// tb_fetch.cpp -- CASE=fetch.redirect_late_response, work package I-009.
//
// The DUT is never its own oracle. Every output is compared against an
// independent C++ shadow written from the *contract* documented in
// rtl/core/mosaic_fetch.sv and in config/contracts/interfaces.json, not from
// the RTL's structure. The shadow holds its own slot table, its own epoch, its
// own credit ledger and its own model of the advisory predictor, and it derives
// the classify-before-consume rule from the prose:
//
//   * a request occupies a slot until exactly one event returns the credit;
//   * a response is live, stale or squashed, and only a live one is delivered;
//   * a redirect retires every in-flight request but returns no credit itself.
//
// Sizes are read from the elaborated DUT (o_fetch_outstanding, o_epoch_w and
// friends), so this file contains no geometry of its own: a profile with a
// different fetch_outstanding or ROB depth needs no edit here.
//
// Two standing invariants are checked on every cycle of every phase, in
// addition to the full shadow comparison:
//
//   * `issued == accepted + credit_returned_by_drops + outstanding`, the credit
//     identity. It is the invariant that catches the whole class of
//     same-cycle bugs at once: a lost release and a double release both break it,
//     and neither shows up as a wrong output if the dropped response happens to
//     carry the same payload as the live one.
//   * `outstanding <= MOSAIC_FETCH_OUTSTANDING`. The bound is structural in the
//     hardware (there is nowhere else for a request to go), so this is checked
//     as an inequality on the published count rather than as a comparison.
//
// Phases, each of which can fail on its own:
//
//   1.  reset-state          the documented cold state straight after reset.
//   2.  redirect-late        the card's named case: issue, redirect, then answer
//                            some responses from before the redirect and some
//                            from after it.
//   3.  late-but-live        a response for a still-valid request is accepted
//                            however late it is; only stale ones are dropped.
//   4.  credit-exactly-once  counts, not "a drop happened": slots issued, credit
//                            returned by accepts, credit returned by drops.
//   5.  bound-enforced       the issue port hammered with a slow responder.
//   6.  same-cycle           every combination of issue / response / redirect /
//                            out_ready x four kinds of response.
//   7.  epoch-wrap           enough redirects to wrap the epoch counter, then a
//                            stale response after the wrap.
//   8.  illegal-16bit        a compressed encoding is reported illegal.
//   9.  fault-path           an instruction access fault, cause 1.
//  10.  predictor-advisory   the owned predictor: a trained target is used, a
//                            reported miss falls through to pc+4, a redirect
//                            squashes the hint.
//  11.  random               randomised traffic, out-of-order responses, random
//                            redirects and updates, shadow compared every cycle.
//
// The learned rule from this project -- "the most common real defect has been
// the model and the RTL disagreeing about a condition" -- is why the harness
// prints the full stimulus in every mismatch message. When the shadow and the
// DUT disagree, both sides' state has to be visible in the failure text, or the
// next step is a guess.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_fetch_tb.h"

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

std::string At(const std::string& where, uint64_t cycle) {
  return where + ": cycle " + std::to_string(cycle);
}

std::string Hex(uint32_t value) { return mosaic::Hex(value, 8); }
std::string Hex64(uint64_t value) { return mosaic::Hex(value, 16); }

std::string B(bool value) { return value ? "1" : "0"; }

// Exception causes, from rtl/core/mosaic_pkg.sv. The driver quotes them rather
// than including the package, because a testbench that shares the RTL's
// constants cannot tell a wrong constant from a wrong implementation.
const uint64_t kExcInsnAccess = 1;    // instruction access fault
const uint64_t kExcIllegalInsn = 2;   // illegal instruction

const uint8_t kKindInsn = 1;
const uint8_t kKindIllegal = 2;
const uint8_t kKindFault = 3;

// ---------------------------------------------------------------- stimulus
struct Stim {
  bool req_valid = false;
  uint64_t req_pc = 0;

  bool rsp_valid = false;
  uint32_t rsp_id = 0;
  uint32_t rsp_epoch = 0;
  uint32_t rsp_data = 0x00000013u;
  uint8_t rsp_len = 4;
  bool rsp_fault = false;

  bool redirect_valid = false;
  uint64_t redirect_pc = 0;

  bool pred_valid = false;
  uint64_t pred_pc = 0;
  bool pred_is_branch = false;
  bool pred_is_jump = false;
  bool pred_is_return = false;

  bool upd_valid = false;
  uint64_t upd_pc = 0;
  bool upd_is_branch = false;
  bool upd_is_jump = false;
  bool upd_is_call = false;
  bool upd_is_return = false;
  bool upd_is_taken = false;
  uint64_t upd_target = 0;
  bool ckpt_valid = false;
  bool flush = false;

  bool out_ready = true;
};

std::string Describe(const Stim& s) {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer),
                "req(v=%d pc=%s) rsp(v=%d id=%d ep=%d data=%s len=%d f=%d) "
                "redir(v=%d pc=%s) out_ready=%d | "
                "pred(v=%d pc=%s br=%d jmp=%d ret=%d) "
                "upd(v=%d pc=%s br=%d jmp=%d call=%d ret=%d taken=%d tgt=%s ckpt=%d flush=%d)",
                s.req_valid ? 1 : 0, Hex64(s.req_pc).c_str(),
                s.rsp_valid ? 1 : 0, s.rsp_id, s.rsp_epoch, Hex(s.rsp_data).c_str(),
                static_cast<int>(s.rsp_len), s.rsp_fault ? 1 : 0,
                s.redirect_valid ? 1 : 0, Hex64(s.redirect_pc).c_str(),
                s.out_ready ? 1 : 0,
                s.pred_valid ? 1 : 0, Hex64(s.pred_pc).c_str(),
                s.pred_is_branch ? 1 : 0, s.pred_is_jump ? 1 : 0,
                s.pred_is_return ? 1 : 0,
                s.upd_valid ? 1 : 0, Hex64(s.upd_pc).c_str(),
                s.upd_is_branch ? 1 : 0, s.upd_is_jump ? 1 : 0,
                s.upd_is_call ? 1 : 0, s.upd_is_return ? 1 : 0,
                s.upd_is_taken ? 1 : 0, Hex64(s.upd_target).c_str(),
                s.ckpt_valid ? 1 : 0, s.flush ? 1 : 0);
  return buffer;
}

// ------------------------------------------------------------ shadow: fetch
//
// Everything the RTL is documented to hold, re-derived from the prose. The
// credit ledger is kept as *three* separate counts rather than one running
// balance, because the whole point of the module is that they cannot drift:
// `issued` counts allocations, `accept` counts responses that delivered, and
// `credit_drop` counts responses that were discarded and still paid. A shadow
// that stored only a balance would agree with a DUT that returned credit twice.
struct Slot {
  bool busy = false;
  bool cancelled = false;
  uint32_t epoch = 0;
  uint64_t pc = 0;
};

struct Expect {
  // combinational, this cycle
  bool req_ready = false;
  bool req_fire = false;
  uint32_t req_id = 0;
  uint32_t req_epoch = 0;
  bool rsp_ready = false;
  bool rsp_fire = false;
  bool rsp_live = false;
  bool rsp_stale = false;
  bool rsp_squashed = false;
  bool rsp_retire = false;
  bool rsp_release = false;
  bool pred_next_valid = false;
  uint64_t pred_next_pc = 0;
  bool pred_squashed = false;
  bool pred_taken = false;
  bool pred_btb_hit = false;
  bool pred_btb_miss = false;
  bool pred_ras_valid = false;
  bool pred_ras_underflow = false;

  // registered, read back this cycle
  bool out_valid = false;
  bool out_illegal = false;
  bool out_fault = false;
  uint64_t out_pc = 0;
  uint32_t out_bits = 0;
  uint8_t out_len = 0;
  uint64_t out_cause = 0;
  uint32_t outstanding = 0;
  uint32_t cancel_pending = 0;
  uint32_t epoch = 0;
  uint32_t issued = 0;
  uint32_t accept = 0;
  uint32_t drop = 0;
  uint32_t stale = 0;
  uint32_t squashed = 0;
  uint32_t credit_drop = 0;
  uint32_t delivered = 0;
  uint32_t fault = 0;
  uint32_t illegal = 0;
  uint32_t deny = 0;
  uint32_t cancel = 0;
  uint64_t fetch_pc = 0;
};

class ShadowFetch {
 public:
  ShadowFetch(uint32_t outstanding, uint32_t epoch_w, uint32_t btb_entries,
              uint32_t bpu_entries, uint32_t ras_entries)
      : outstanding_(outstanding),
        epoch_mask_((1u << epoch_w) - 1u),
        btb_entries_(btb_entries),
        bpu_entries_(bpu_entries),
        ras_entries_(ras_entries),
        slots_(outstanding),
        btb_tag_(btb_entries, 0),
        btb_target_(btb_entries, 0),
        btb_kind_(btb_entries, 0),
        btb_valid_(btb_entries, 0),
        bpu_ctr_(bpu_entries, 0),
        bpu_valid_(bpu_entries, 0),
        ras_mem_(ras_entries, 0) {}

  uint32_t outstanding() const { return outstanding_; }

  void Reset() {
    for (auto& slot : slots_) slot = Slot{};
    epoch_ = 0;
    fetch_pc_ = 0;
    out_kind_ = 0;
    out_pc_ = 0;
    out_bits_ = 0;
    out_len_ = 0;
    out_cause_ = 0;
    issued_ = accept_ = drop_ = stale_ = squashed_ = 0;
    credit_drop_ = delivered_ = fault_ = illegal_ = deny_ = cancel_ = 0;
    for (size_t i = 0; i < btb_valid_.size(); i++) btb_valid_[i] = 0;
    for (size_t i = 0; i < bpu_valid_.size(); i++) bpu_valid_[i] = 0;
    ras_sp_ = 0;
    ras_ckpt_ = 0;
  }

  // The pure function: state + stimulus -> everything the module publishes.
  // Written first and used for both the comparison and the state update, so the
  // shadow can never compare against one rule and update by another.
  Expect Peek(const Stim& s) const {
    Expect e;

    e.epoch = epoch_;
    e.fetch_pc = fetch_pc_;

    // --- output register, read back -------------------------------------
    e.out_valid = (out_kind_ == kKindInsn);
    e.out_illegal = (out_kind_ == kKindIllegal);
    e.out_fault = (out_kind_ == kKindFault);
    e.out_pc = out_pc_;
    e.out_bits = out_bits_;
    e.out_len = out_len_;
    e.out_cause = out_cause_;

    // --- response intake ------------------------------------------------
    // The output register can take a response when it is empty or is being
    // drained this cycle. Nothing else gates readiness: a response whose id is
    // out of range is still *consumed*, classified squashed, and reported.
    const bool out_can_take = (out_kind_ == 0) || s.out_ready;
    e.rsp_ready = out_can_take;
    e.rsp_fire = s.rsp_valid && e.rsp_ready;

    const bool id_ok = s.rsp_id < outstanding_;
    // Ownership and liveness are two different questions. Ownership asks whose
    // credit this response settles; liveness asks whether the instruction it
    // carries still exists. A slot cancelled by a redirect still owns the
    // response that is on its way, and that response is what returns the credit.
    const bool slot_owns = id_ok && slots_[s.rsp_id].busy &&
                           slots_[s.rsp_id].epoch == s.rsp_epoch;
    // The redirect is part of the live test: a response that arrives on the
    // cycle the redirect retires its epoch is stale, even though its epoch is
    // the one in force right now. So is a slot a redirect retired earlier,
    // whatever the counter says -- which is what makes the classification
    // survive the epoch wrapping.
    e.rsp_live = slot_owns && !slots_[s.rsp_id].cancelled && !s.redirect_valid &&
                 (s.rsp_epoch == epoch_);
    // A response the slot owns ends that slot's occupancy, whether it delivered
    // or was dropped. `rsp_release` is the narrower case: credit that came back
    // through a *drop* rather than through an accept.
    const bool retire = e.rsp_fire && slot_owns;
    e.rsp_retire = retire;
    e.rsp_release = retire && !e.rsp_live;
    e.rsp_stale = !e.rsp_live && (slot_owns || (s.rsp_epoch != epoch_));
    e.rsp_squashed = !e.rsp_live && !slot_owns && (s.rsp_epoch == epoch_);

    // --- issue path -----------------------------------------------------
    bool free_found = false;
    uint32_t alloc_id = 0;
    for (uint32_t i = 0; i < outstanding_; i++) {
      if (!free_found && (!slots_[i].busy || (e.rsp_retire && s.rsp_id == i))) {
        free_found = true;
        alloc_id = i;
      }
    }
    // A redirect refuses the issue port: the requester is about to be told a
    // different PC, so a request taken this cycle would be issued under an
    // epoch that is being retired at this edge.
    e.req_ready = !s.redirect_valid && free_found;
    e.req_fire = s.req_valid && e.req_ready;
    e.req_id = alloc_id;
    e.req_epoch = epoch_;

    // --- the advisory predictor -----------------------------------------
    PeekPredictor(s, &e);

    // --- published counters and the slot census -------------------------
    uint32_t busy = 0;
    uint32_t cancelled = 0;
    for (const auto& slot : slots_) {
      if (slot.busy) {
        busy++;
        if (slot.cancelled) cancelled++;
      }
    }
    e.outstanding = busy;
    e.cancel_pending = cancelled;
    e.issued = issued_;
    e.accept = accept_;
    e.drop = drop_;
    e.stale = stale_;
    e.squashed = squashed_;
    e.credit_drop = credit_drop_;
    e.delivered = delivered_;
    e.fault = fault_;
    e.illegal = illegal_;
    e.deny = deny_;
    e.cancel = cancel_;
    return e;
  }

  // The state update for the edge, using the same Peek() result.
  void Step(const Stim& s, const Expect& e) {
    if (s.redirect_valid) {
      epoch_ = (epoch_ + 1u) & epoch_mask_;
      fetch_pc_ = s.redirect_pc;
    }
    if (s.req_valid && !e.req_ready) deny_++;
    if (e.req_fire) issued_++;

    if (e.rsp_fire) {
      if (e.rsp_live) {
        accept_++;
        // I-041's rule, in one place: the *encoding* decides the length. Low two
        // bits 11 is a 32-bit instruction and must be reported in four bytes;
        // anything else is a 16-bit compressed instruction and must be reported
        // in two. A 32-bit encoding in two bytes is a malformed response and is
        // refused as illegal; so is a 16-bit encoding with no length at all.
        const bool is_16bit = (s.rsp_data & 3u) != 3u;
        const bool is_32bit = !is_16bit && (s.rsp_len == 4);
        const bool is_insn = is_16bit || is_32bit;
#ifdef MOSAIC_FETCH_MUTANT_DELIVER_16BIT
        // The mutant reports a 16-bit instruction in four bytes and does not
        // mask its upper half, which is the "compressed length reported as
        // four" defect.
        const bool mut16len4 = true;
#else
        const bool mut16len4 = false;
#endif
        if (s.rsp_fault) {
          out_kind_ = kKindFault;
          out_cause_ = kExcInsnAccess;
          fault_++;
        } else if (is_insn) {
          out_kind_ = kKindInsn;
          out_cause_ = 0;
          delivered_++;
        } else {
          out_kind_ = kKindIllegal;
          out_cause_ = kExcIllegalInsn;
          illegal_++;
        }
        out_pc_ = slots_[s.rsp_id].pc;
        out_bits_ = (is_16bit && !mut16len4) ? (s.rsp_data & 0xffffu) : s.rsp_data;
        out_len_ = (is_16bit && !mut16len4) ? 2u : 4u;
      } else {
        drop_++;
        if (e.rsp_stale) {
          stale_++;
        } else {
          squashed_++;
        }
        if (e.rsp_release) credit_drop_++;
      }
    }
    // The output register's own lifecycle, independent of whether a response
    // fired this cycle, because the two are independent events: a live response
    // installs a new instruction (and wins over a drain in the same cycle), a
    // redirect retires whatever the register holds -- that instruction was
    // fetched after the branch the redirect came from, so it is wrong-path by
    // construction -- and otherwise the register drains when the consumer takes
    // it. A dropped (non-live) response must not hold it: it says nothing about
    // the instruction already in the register. Folding the two into one branch,
    // which is the form this model shipped with and mirrored the RTL's, makes
    // the register hold its instruction for ever and deliver it again every
    // cycle the consumer is ready.
    if (!(e.rsp_fire && e.rsp_live) &&
        (s.redirect_valid || (out_kind_ != 0 && s.out_ready))) {
      out_kind_ = 0;
    }

    // The issue wins over the release when the two name the same slot: the
    // credit the response returns is the credit the request spends.
    for (uint32_t i = 0; i < outstanding_; i++) {
      if (e.req_fire && e.req_id == i) {
        slots_[i].busy = true;
        slots_[i].cancelled = false;
        slots_[i].epoch = epoch_;
        slots_[i].pc = s.req_pc;
      } else if (e.rsp_retire && s.rsp_id == i) {
        slots_[i].busy = false;
        slots_[i].cancelled = false;
      } else if (s.redirect_valid) {
        // Everything still in flight is retired by the redirect and keeps its
        // credit until its late response is dropped.
        slots_[i].cancelled = slots_[i].busy;
      }
    }

    if (s.redirect_valid) cancel_ += e.outstanding;

    StepPredictor(s);
  }

  // The published ledger, for the phase-level assertions that read counts
  // rather than cycles.
  uint32_t issued() const { return issued_; }
  uint32_t accepted() const { return accept_; }
  uint32_t dropped() const { return drop_; }
  uint32_t stale() const { return stale_; }
  uint32_t squashed() const { return squashed_; }
  uint32_t credit_drops() const { return credit_drop_; }
  uint32_t delivered() const { return delivered_; }
  uint32_t faults() const { return fault_; }
  uint32_t illegals() const { return illegal_; }
  uint32_t denials() const { return deny_; }
  uint32_t cancels() const { return cancel_; }
  uint32_t epoch() const { return epoch_; }

 private:
  // ---------------------------------------------------- the shadow predictor
  //
  // `mosaic_fetch` owns a `mosaic_predictor`, so its advisory outputs are part
  // of what this module publishes and the shadow has to have an opinion about
  // them. What is modelled here is the predictor's *documented* contract --
  // a bimodal direction table, a tagged target buffer and a return-address
  // stack -- and only enough of it to make `pred_next_pc` non-trivial: a cold
  // table reports a miss, a trained entry supplies a target, and a return
  // prefers the stack. The predictor's own behaviour is I-021's tested contract
  // (CASE=predictor.btb_aliasing); what is tested here is that fetch does not
  // turn any of it into authority.
  static uint32_t IndexOf(uint64_t pc, uint32_t entries) {
    uint32_t width = 1;
    while ((1u << width) < entries) width++;
    return static_cast<uint32_t>((pc >> 1) & ((1ull << width) - 1ull));
  }

  void PeekPredictor(const Stim& s, Expect* e) const {
    const uint32_t btb_index = IndexOf(s.pred_pc, btb_entries_);
    const bool btb_index_ok = btb_index < btb_entries_;
    const uint8_t want_kind = s.pred_is_branch ? 1 : 2;
    const bool btb_hit = btb_index_ok && btb_valid_[btb_index] &&
                         btb_tag_[btb_index] == s.pred_pc &&
                         btb_kind_[btb_index] == want_kind;

    const uint32_t bpu_index = IndexOf(s.pred_pc, bpu_entries_);
    const bool bpu_index_ok = bpu_index < bpu_entries_;
    const bool entry_taken =
        (bpu_index_ok && bpu_valid_[bpu_index]) ? ((bpu_ctr_[bpu_index] & 2u) != 0)
                                                : false;

    const bool ras_top_valid = ras_sp_ != 0;
    const bool use_ras = s.pred_valid && s.pred_is_jump && s.pred_is_return && ras_top_valid;

    e->pred_taken = s.pred_valid && (s.pred_is_branch || s.pred_is_jump)
                        ? (s.pred_is_branch ? entry_taken : true)
                        : false;
    const bool redirect = s.pred_valid && (s.pred_is_branch || s.pred_is_jump) &&
                          (use_ras || (btb_hit && (s.pred_is_jump || entry_taken)));
    const bool btb_miss = s.pred_valid && (s.pred_is_branch || s.pred_is_jump) &&
                          !use_ras && !btb_hit;

    uint64_t target;
    if (use_ras) {
      target = ras_mem_[ras_sp_ - 1];
    } else if (btb_hit) {
      target = btb_target_[btb_index];
    } else {
      target = s.pred_pc + 4;
    }

    e->pred_btb_hit = btb_hit;
    e->pred_btb_miss = btb_miss;
    e->pred_ras_valid = use_ras;
    e->pred_ras_underflow =
        s.pred_valid && s.pred_is_jump && s.pred_is_return && !ras_top_valid;

    // Fetch's rule, re-derived: a reported miss falls through to pc + 4, and a
    // redirect in this cycle voids the hint entirely.
    e->pred_squashed = s.pred_valid && s.redirect_valid;
    e->pred_next_valid = s.pred_valid && !s.redirect_valid;
    e->pred_next_pc = btb_miss ? (s.pred_pc + 4) : (redirect ? target : (s.pred_pc + 4));
  }

  void StepPredictor(const Stim& s) {
    // The checkpoint written this edge is *not* the one a same-cycle flush
    // restores: the hardware restores the registered value, which is the
    // pointer as it was before this edge. Writing the checkpoint first and then
    // flushing would restore the current pointer and silently disable the
    // whole mechanism -- found by a return prediction that came back one entry
    // off, not by reading the code twice.
    const uint32_t restore_to = ras_ckpt_;
    if (s.ckpt_valid) ras_ckpt_ = ras_sp_;

    const bool ctrl = s.upd_valid && (s.upd_is_branch || s.upd_is_jump);
    if (!s.flush) {
      if (ctrl && s.upd_is_branch) {
        const uint32_t index = IndexOf(s.upd_pc, bpu_entries_);
        if (index < bpu_entries_) {
          if (!bpu_valid_[index]) {
            bpu_ctr_[index] = s.upd_is_taken ? 2 : 1;
          } else if (s.upd_is_taken) {
            bpu_ctr_[index] = bpu_ctr_[index] == 3 ? 3 : bpu_ctr_[index] + 1;
          } else {
            bpu_ctr_[index] = bpu_ctr_[index] == 0 ? 0 : bpu_ctr_[index] - 1;
          }
          bpu_valid_[index] = 1;
        }
      }
      if (ctrl && (s.upd_is_jump || s.upd_is_taken)) {
        const uint32_t index = IndexOf(s.upd_pc, btb_entries_);
        if (index < btb_entries_) {
          btb_tag_[index] = s.upd_pc;
          btb_target_[index] = s.upd_target;
          btb_kind_[index] = s.upd_is_branch ? 1 : 2;
          btb_valid_[index] = 1;
        }
      }
    }

    if (s.flush) {
      ras_sp_ = restore_to;
    } else if (ctrl && s.upd_is_call && ras_sp_ < ras_entries_) {
      ras_mem_[ras_sp_] = s.upd_pc + 4;
      ras_sp_++;
    } else if (ctrl && s.upd_is_return && ras_sp_ != 0) {
      ras_sp_--;
    }
  }

  uint32_t outstanding_;
  uint32_t epoch_mask_;
  uint32_t btb_entries_;
  uint32_t bpu_entries_;
  uint32_t ras_entries_;

  std::vector<Slot> slots_;
  uint32_t epoch_ = 0;
  uint64_t fetch_pc_ = 0;

  uint8_t out_kind_ = 0;   // 0 means the output register is empty
  uint64_t out_pc_ = 0;
  uint32_t out_bits_ = 0;
  uint8_t out_len_ = 0;
  uint64_t out_cause_ = 0;

  uint32_t issued_ = 0;
  uint32_t accept_ = 0;
  uint32_t drop_ = 0;
  uint32_t stale_ = 0;
  uint32_t squashed_ = 0;
  uint32_t credit_drop_ = 0;
  uint32_t delivered_ = 0;
  uint32_t fault_ = 0;
  uint32_t illegal_ = 0;
  uint32_t deny_ = 0;
  uint32_t cancel_ = 0;

  std::vector<uint64_t> btb_tag_;
  std::vector<uint64_t> btb_target_;
  std::vector<uint8_t> btb_kind_;
  std::vector<uint8_t> btb_valid_;
  std::vector<uint8_t> bpu_ctr_;
  std::vector<uint8_t> bpu_valid_;
  std::vector<uint64_t> ras_mem_;
  uint32_t ras_sp_ = 0;
  uint32_t ras_ckpt_ = 0;
};

// ------------------------------------------------------------------ harness
class Harness {
 public:
  Harness(Vmosaic_fetch_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles,
          ShadowFetch* shadow)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles), shadow_(shadow) {}

  void Phase(const std::string& name) { phase_ = name; }

  // The shadow is re-created once the elaborated geometry is known, so it is
  // bound rather than owned.
  void BindShadow(ShadowFetch* shadow) { shadow_ = shadow; }

  // What the module publishes right now, with no stimulus applied. Phases read
  // this instead of poking the DUT, so every number they assert on has already
  // been compared against the shadow on the cycle it changed.
  Expect PeekFinal() const { return shadow_->Peek(Stim{}); }

  // Assert reset for `cycles` rising edges with no other stimulus. The shadow is
  // reset with the DUT so no phase can pass on state left by the one before it.
  void Reset(int cycles) {
    shadow_->Reset();
    for (int i = 0; i < cycles; i++) {
      Cycle(Stim{}, /*rst=*/true);
    }
  }

  // One clock period. Everything is compared while the clock is low: the
  // combinational outputs describe the cycle that is about to be sampled, and
  // the registered outputs are the state that will be visible afterwards.
  Expect Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + std::to_string(max_cycles_) +
                       ") exhausted before the phase finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->req_valid = s.req_valid ? 1 : 0;
    dut_->req_pc = s.req_pc;
    dut_->rsp_valid = s.rsp_valid ? 1 : 0;
    dut_->rsp_id = s.rsp_id;
    dut_->rsp_epoch = s.rsp_epoch;
    dut_->rsp_data = s.rsp_data;
    dut_->rsp_len = s.rsp_len;
    dut_->rsp_fault = s.rsp_fault ? 1 : 0;
    dut_->redirect_valid = s.redirect_valid ? 1 : 0;
    dut_->redirect_pc = s.redirect_pc;
    dut_->pred_valid = s.pred_valid ? 1 : 0;
    dut_->pred_pc = s.pred_pc;
    dut_->pred_is_branch = s.pred_is_branch ? 1 : 0;
    dut_->pred_is_jump = s.pred_is_jump ? 1 : 0;
    dut_->pred_is_return = s.pred_is_return ? 1 : 0;
    dut_->upd_valid = s.upd_valid ? 1 : 0;
    dut_->upd_pc = s.upd_pc;
    dut_->upd_is_branch = s.upd_is_branch ? 1 : 0;
    dut_->upd_is_jump = s.upd_is_jump ? 1 : 0;
    dut_->upd_is_call = s.upd_is_call ? 1 : 0;
    dut_->upd_is_return = s.upd_is_return ? 1 : 0;
    dut_->upd_is_taken = s.upd_is_taken ? 1 : 0;
    dut_->upd_target = s.upd_target;
    dut_->ckpt_valid = s.ckpt_valid ? 1 : 0;
    dut_->flush = s.flush ? 1 : 0;
    dut_->out_ready = s.out_ready ? 1 : 0;
    dut_->eval();

    const Expect e = shadow_->Peek(s);
    const std::string where = At(phase_, clk_->cycle());

    // During reset the outputs are the pre-reset state by contract, so only the
    // post-reset cycles are compared against the shadow.
    if (!rst) {
      Compare(s, e, where);
      CheckInvariants(e, where);
    }

    shadow_->Step(s, e);

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    // After the edge the registered outputs must show exactly what the shadow
    // now holds. This is the check that a register which updates wrongly is
    // caught on the cycle it happens rather than on the cycle someone notices.
    if (!rst) {
      const Expect after = shadow_->Peek(Stim{});
      CheckRegistered(s, after, where);
    }
    return e;
  }

  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }
  const std::string& phase() const { return phase_; }

 private:
  void Mismatch(const std::string& field, const std::string& expected,
                const std::string& actual, const Stim& s, const std::string& where) {
    Fail(where, field + ": expected " + expected + ", got " + actual + " [" +
                    Describe(s) + "]");
  }

  // The narrow fields (request id, epoch, the slot census) are compared whole.
  // They are the fields where an extra bit would alias two values onto one, so
  // they are compared without a mask: masking a width bug out of the comparison
  // is how an id that wraps early survives a test that "checks the id".
  void CheckIdField(const char* name, uint32_t actual, uint32_t expected,
                    uint32_t width, const Stim& s, const std::string& where) {
    ++comparisons_;
    if (actual != expected) {
      Mismatch(name, Hex(expected) + " (" + std::to_string(width) + " bits)", Hex(actual),
               s, where);
    }
  }

  void Compare(const Stim& s, const Expect& e, const std::string& where) {
    const auto bit = [&](const char* name, int actual, bool expected) {
      ++comparisons_;
      if ((actual != 0) != expected) {
        Mismatch(name, B(expected), actual ? "1" : "0", s, where);
      }
    };

    bit("req_ready", dut_->req_ready, e.req_ready);
    CheckIdField("req_id", dut_->req_id, e.req_id, id_w_, s, where);
    CheckIdField("req_epoch", dut_->req_epoch, e.req_epoch, epoch_w_, s, where);

    bit("rsp_ready", dut_->rsp_ready, e.rsp_ready);
    bit("rsp_squashed", dut_->rsp_squashed, e.rsp_squashed);

    CheckIdField("outstanding_count", dut_->outstanding_count, e.outstanding,
                 cnt_w_, s, where);
    CheckIdField("cancel_pending", dut_->cancel_pending, e.cancel_pending, cnt_w_,
                 s, where);
    CheckIdField("epoch_now", dut_->epoch_now, e.epoch, epoch_w_, s, where);

    ++comparisons_;
    if (dut_->fetch_pc != e.fetch_pc) {
      Mismatch("fetch_pc", Hex64(e.fetch_pc), Hex64(dut_->fetch_pc), s, where);
    }

    bit("pred_next_valid", dut_->pred_next_valid, e.pred_next_valid);
    bit("pred_squashed", dut_->pred_squashed, e.pred_squashed);
    ++comparisons_;
    if (dut_->pred_next_pc != e.pred_next_pc) {
      Mismatch("pred_next_pc", Hex64(e.pred_next_pc), Hex64(dut_->pred_next_pc), s,
               where);
    }
    bit("pred_taken", dut_->pred_taken, e.pred_taken);
    bit("pred_btb_hit", dut_->pred_btb_hit, e.pred_btb_hit);
    bit("pred_btb_miss", dut_->pred_btb_miss, e.pred_btb_miss);
    bit("pred_ras_valid", dut_->pred_ras_valid, e.pred_ras_valid);
    bit("pred_ras_underflow", dut_->pred_ras_underflow, e.pred_ras_underflow);
  }

  void CheckRegistered(const Stim& s, const Expect& e, const std::string& where) {
    const auto bit = [&](const char* name, int actual, bool expected) {
      ++comparisons_;
      if ((actual != 0) != expected) {
        Mismatch(name, B(expected), actual ? "1" : "0", s, where);
      }
    };
    const auto word = [&](const char* name, uint64_t actual, uint64_t expected) {
      ++comparisons_;
      if (actual != expected) {
        Mismatch(name, Hex64(expected), Hex64(actual), s, where);
      }
    };
    const auto counter = [&](const char* name, uint32_t actual, uint32_t expected) {
      ++comparisons_;
      if (actual != expected) {
        Mismatch(name, std::to_string(expected), std::to_string(actual), s, where);
      }
    };

    bit("out_valid(registered)", dut_->out_valid, e.out_valid);
    bit("out_illegal(registered)", dut_->out_illegal, e.out_illegal);
    bit("out_fault(registered)", dut_->out_fault, e.out_fault);
    word("out_pc", dut_->out_pc, e.out_pc);
    ++comparisons_;
    if (dut_->out_bits != e.out_bits) {
      Mismatch("out_bits", Hex(e.out_bits), Hex(dut_->out_bits), s, where);
    }
    counter("out_len", dut_->out_len, e.out_len);
    word("out_cause", dut_->out_cause, e.out_cause);
    counter("issued_count", dut_->issued_count, e.issued);
    counter("accept_count", dut_->accept_count, e.accept);
    counter("drop_count", dut_->drop_count, e.drop);
    counter("stale_drop_count", dut_->stale_drop_count, e.stale);
    counter("squashed_drop_count", dut_->squashed_drop_count, e.squashed);
    counter("credit_drop_count", dut_->credit_drop_count, e.credit_drop);
    counter("delivered_count", dut_->delivered_count, e.delivered);
    counter("fault_count", dut_->fault_count, e.fault);
    counter("illegal_count", dut_->illegal_count, e.illegal);
    counter("deny_count", dut_->deny_count, e.deny);
    counter("cancel_count", dut_->cancel_count, e.cancel);
  }

  // The two standing invariants, checked on every cycle of every phase.
  void CheckInvariants(const Expect& e, const std::string& where) {
    ++comparisons_;
    const uint32_t accounted = e.accept + e.credit_drop + e.outstanding;
    if (e.issued != accounted) {
      Fail(where, "credit identity broken: issued " + std::to_string(e.issued) +
                      " != accepted " + std::to_string(e.accept) + " + credit from drops " +
                      std::to_string(e.credit_drop) + " + outstanding " +
                      std::to_string(e.outstanding));
    }
    ++comparisons_;
    if (e.drop != e.stale + e.squashed) {
      Fail(where, "every dropped response must be classified: drop " +
                      std::to_string(e.drop) + " != stale " + std::to_string(e.stale) +
                      " + squashed " + std::to_string(e.squashed));
    }
    ++comparisons_;
    if (e.credit_drop > e.stale) {
      Fail(where, "more drops returned credit than were stale: " +
                      std::to_string(e.credit_drop) + " > " + std::to_string(e.stale));
    }
    ++comparisons_;
    if (e.outstanding > outstanding_) {
      Fail(where, "outstanding " + std::to_string(e.outstanding) +
                      " exceeds MOSAIC_FETCH_OUTSTANDING " + std::to_string(outstanding_));
    }
    ++comparisons_;
    if (e.cancel_pending > e.outstanding) {
      Fail(where, "more cancelled slots than occupied slots");
    }
  }

  Vmosaic_fetch_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  ShadowFetch* shadow_;
  std::string phase_;
  uint64_t comparisons_ = 0;
  uint64_t cycles_ = 0;

 public:
  uint32_t outstanding_ = 4;
  uint32_t epoch_w_ = 7;
  uint32_t id_w_ = 2;
  uint32_t cnt_w_ = 3;
};

// ------------------------------------------------------- the requester side
//
// The C++ side is both the requester and the responder, which is the only way to
// construct the case this test exists for: a self-driven fetch unit cannot be
// made to answer a redirect in a chosen order.
class Requester {
 public:
  struct Pending {
    uint32_t id;
    uint32_t epoch;
    uint64_t pc;
    uint32_t data;
    uint8_t len;
    bool fault;
    uint64_t issued_cycle;
  };

  std::deque<Pending> pending;

  // A response is a transfer with backpressure, so the responder has to hold it
  // until it is taken. Offering one for a single cycle and moving on is the
  // classic testbench bug: the request never gets its answer, its credit never
  // comes back, and the failure shows up as a "leak" hundreds of cycles later
  // with nothing to connect it to the cycle that caused it.
  bool holding = false;
  Stim held;

  void Hold(const Pending& p) {
    holding = true;
    held = Respond(p);
  }

  // ---- the reset-traffic rule (V-010) -------------------------------------
  // The requester is the driver's bus model: it accepts the requests the fetch
  // unit issues and holds the responses. `Accept` is the one place a request
  // enters it, so the reset-traffic control below exercises the real path. A
  // request presented while reset is asserted is refused.
  bool Accept(bool rst, const Pending& p) {
    if (!gate_.MayAccept(rst, /*request_presented=*/true)) return false;
    pending.push_back(p);
    return true;
  }

  // Presents one request with reset asserted and returns true iff the model
  // accepted it. The shipping configuration must refuse; the control must
  // accept, which is what proves the rule check can fail.
  bool PresentDuringReset() {
    const size_t before = pending.size();
    Accept(/*rst=*/true, Make(0, 0, 0x80000000ull, 0, 0));
    const bool grew = pending.size() != before;
    if (grew) pending.pop_back();
    return grew;
  }
  void SetAcceptDuringResetControl(bool on) { gate_.SetAcceptDuringResetControl(on); }

  // Put the response being held on this cycle's stimulus, if there is one.
  void Apply(Stim* s) const {
    if (!holding) return;
    s->rsp_valid = true;
    s->rsp_id = held.rsp_id;
    s->rsp_epoch = held.rsp_epoch;
    s->rsp_data = held.rsp_data;
    s->rsp_len = held.rsp_len;
    s->rsp_fault = held.rsp_fault;
  }

  // Called after the edge: a held response that was taken is finished.
  void Retire(const Expect& e) {
    if (holding && e.rsp_fire) holding = false;
  }

  // Build the response for a pending request, with the payload the requester
  // decided on when it issued.
  Stim Respond(const Pending& p, bool out_ready = true) {
    Stim s;
    s.rsp_valid = true;
    s.rsp_id = p.id;
    s.rsp_epoch = p.epoch;
    s.rsp_data = p.data;
    s.rsp_len = p.len;
    s.rsp_fault = p.fault;
    s.out_ready = out_ready;
    return s;
  }

  // A deterministic payload for a PC, so a replay with the same seed gives the
  // same bits. `form` selects 32-bit, 16-bit or fault.
  static Pending Make(uint32_t id, uint32_t epoch, uint64_t pc, int form,
                      uint64_t cycle) {
    Pending p;
    p.id = id;
    p.epoch = epoch;
    p.pc = pc;
    p.data = static_cast<uint32_t>((pc >> 2) ^ 0x13579bdfu) | 3u;
    p.len = 4;
    p.fault = false;
    if (form == 1) {
      // A 16-bit compressed encoding: low two bits are not 11, and the
      // reported length is 2 bytes.
      p.data = static_cast<uint32_t>((pc >> 2) ^ 0x2468ace0u) & ~3u;
      p.data |= (pc & 1u);          // 00 or 01: never 11
      p.len = 2;
    } else if (form == 2) {
      p.fault = true;
    } else if (form == 3) {
      // Four bytes returned for an instruction whose encoding is still
      // compressed: the low two bits are not 11. The encoding decides, so this
      // is a 16-bit instruction and must be reported as two bytes.
      p.data = static_cast<uint32_t>((pc >> 2) ^ 0x2468ace0u) & ~3u;
      p.data |= 1u;
      p.len = 4;
    } else if (form == 4) {
      // A 32-bit encoding (low two bits 11) returned in only two bytes: the
      // instruction cannot be assembled from what arrived, and fetch refuses it
      // rather than delivering half an instruction.
      p.data = static_cast<uint32_t>((pc >> 2) ^ 0x13579bdfu) | 3u;
      p.len = 2;
    }
    p.issued_cycle = cycle;
    return p;
  }

 private:
  // V-010: the one reset-traffic gate this bus model routes through.
  mosaic::BusResetGate gate_;
};

}  // namespace

// =========================================================== the phases
namespace {

const uint64_t kBasePc = 0x0000000080000000ull;

void PhaseResetState(Harness* h, mosaic::Reporter* reporter) {
  h->Phase("reset-state");
  h->Reset(4);

  // A fresh machine accepts a request, has nothing outstanding, and has an
  // epoch of zero. These are read from the DUT directly rather than from the
  // shadow, so a reset that leaves the hardware dirty fails here even if the
  // shadow were wrong about what clean is.
  Stim idle;
  idle.out_ready = true;
  const Expect e = h->Cycle(idle);
  Require(e.outstanding == 0, h->phase(), "outstanding is not zero after reset");
  Require(e.epoch == 0, h->phase(), "the epoch does not start at zero");
  Require(e.issued == 0 && e.accept == 0 && e.drop == 0, h->phase(),
          "a counter is non-zero straight out of reset");
  Require(e.req_ready, h->phase(),
          "a machine with an empty request table must accept a request");

  reporter->Check(true, "reset-state: the documented cold state");
}

// V-010: the reset-traffic rule. The requester is the driver's bus model: a
// request presented while reset is asserted is refused, and the guard has been
// seen to fire.
void PhaseResetTraffic(Harness* h, mosaic::Reporter* reporter, Requester* req) {
  h->Phase("reset-traffic");
  h->Reset(4);
  Require(!req->PresentDuringReset(), h->phase(),
          "the requester accepted a request while reset was asserted");
  req->SetAcceptDuringResetControl(true);
  const bool control_accepted = req->PresentDuringReset();
  req->SetAcceptDuringResetControl(false);
  Require(control_accepted, h->phase(),
          "the accept-during-reset control did not fire: the guard is untested");
  Require(!req->PresentDuringReset(), h->phase(),
          "the accept-during-reset control was left engaged");
  reporter->Check(true, "reset-traffic: the rule holds and the guard fires");
}

void PhaseRedirectLateResponse(Harness* h, mosaic::Reporter* reporter,
                               Requester* req) {
  h->Phase("redirect-late-response");
  h->Reset(4);

  const uint32_t bound = h->outstanding_;
  const uint32_t n = std::min<uint32_t>(bound, 3);
  Require(n >= 2, h->phase(), "the geometry leaves no room for this case");

  // --- before the redirect: fill the table --------------------------------
  for (uint32_t i = 0; i < n; i++) {
    Stim s;
    s.req_valid = true;
    s.req_pc = kBasePc + 4 * i;
    const Expect e = h->Cycle(s);
    Require(e.req_fire, h->phase(),
            "request " + std::to_string(i) + " was refused while the table was empty");
    req->Accept(/*rst=*/false, Requester::Make(e.req_id, e.req_epoch, s.req_pc, 0, 0));
  }

  // --- the redirect -------------------------------------------------------
  {
    Stim s;
    s.redirect_valid = true;
    s.redirect_pc = kBasePc + 0x400;
    s.req_valid = true;          // an issue offered on the redirect cycle
    s.req_pc = kBasePc + 0x800;
    // `e` is the state *before* the edge this cycle applies; `post` is the state
    // after it. Phase assertions about what a cycle did are made on `post`, and
    // the harness has already compared both against the shadow.
    const Expect e = h->Cycle(s);
    const Expect post = h->PeekFinal();
    Require(!e.req_ready, h->phase(),
            "the issue port accepted a request on the cycle of a redirect");
    Require(!e.req_fire, h->phase(),
            "a request was issued under an epoch being retired at this edge");
    Require(post.epoch == 1, h->phase(),
            "the redirect did not advance the epoch (epoch=" +
                std::to_string(post.epoch) + ")");
    Require(post.cancel_pending == n, h->phase(),
            "the redirect did not retire every in-flight request (cancelled " +
                std::to_string(post.cancel_pending) + " of " + std::to_string(n) + ")");
    Require(post.outstanding == n, h->phase(),
            "the redirect returned credit instead of waiting for the late "
            "responses to be dropped");
    Require(!post.out_valid && !post.out_illegal && !post.out_fault, h->phase(),
            "something was delivered on the redirect cycle");
  }

  // --- after the redirect: answer the old requests, out of order ---------
  // Delivered last-to-first on purpose: a table indexed by arrival order would
  // pass this and fail the randomised one.
  uint32_t delivered_before = 0;
  for (uint32_t k = 0; k < n; k++) {
    const Requester::Pending p = req->pending.back();
    req->pending.pop_back();
    const Expect e = h->Cycle(req->Respond(p));
    const Expect post = h->PeekFinal();
    Require(!post.out_valid && !post.out_illegal && !post.out_fault, h->phase(),
            "a response from before the redirect was delivered");
    Require(e.rsp_stale, h->phase(), "a pre-redirect response was not classified stale");
    Require(e.rsp_release, h->phase(),
            "a pre-redirect response did not return its slot's credit");
    Require(post.outstanding == n - 1 - k, h->phase(),
            "outstanding did not fall by exactly one for the drop (expected " +
                std::to_string(n - 1 - k) + ", got " +
                std::to_string(post.outstanding) + ")");
    Require(post.credit_drop == k + 1, h->phase(),
            "the drop returned " + std::to_string(post.credit_drop) +
                " credits in total, expected " + std::to_string(k + 1));
    delivered_before++;
  }
  Require(delivered_before == n, h->phase(), "not every late response was delivered");

  // --- and now the requests of the new epoch ------------------------------
  uint32_t new_epoch_requests = 0;
  for (uint32_t i = 0; i < 2; i++) {
    Stim s;
    s.req_valid = true;
    s.req_pc = kBasePc + 0x400 + 4 * i;
    const Expect e = h->Cycle(s);
    Require(e.req_fire, h->phase(),
            "the table did not accept a request after the late responses drained");
    Require(e.req_epoch == 1, h->phase(),
            "a request was issued under the pre-redirect epoch");
    req->Accept(/*rst=*/false, Requester::Make(e.req_id, e.req_epoch, s.req_pc, 0, 0));
    new_epoch_requests++;
  }
  for (uint32_t i = 0; i < new_epoch_requests; i++) {
    const Requester::Pending p = req->pending.front();
    req->pending.pop_front();
    const Expect e = h->Cycle(req->Respond(p));
    const Expect post = h->PeekFinal();
    Require(e.rsp_live, h->phase(), "a post-redirect response was not classified live");
    Require(post.out_valid, h->phase(), "a post-redirect instruction was not delivered");
    Require(post.out_pc == p.pc, h->phase(),
            "the delivered PC " + Hex64(post.out_pc) +
                " is not the PC of the request " + Hex64(p.pc));
  }

  // The counts, not merely "a drop happened".
  const Expect now = h->PeekFinal();
  Require(now.drop == n, h->phase(),
          "expected exactly " + std::to_string(n) + " drops, got " +
              std::to_string(now.drop));
  Require(now.stale == n, h->phase(),
          "expected exactly " + std::to_string(n) + " stale drops, got " +
              std::to_string(now.stale));
  Require(now.squashed == 0, h->phase(),
          "a well-behaved responder produced " + std::to_string(now.squashed) +
              " squashed responses");
  Require(now.credit_drop == now.drop, h->phase(),
          "the credit returned by drops (" + std::to_string(now.credit_drop) +
              ") does not equal the number of drops (" + std::to_string(now.drop) + ")");
  Require(now.issued == now.accept + now.credit_drop + now.outstanding, h->phase(),
          "the credit identity does not hold at the end of the phase");
  Require(now.outstanding == 0, h->phase(), "requests leaked at the end of the phase");
  Require(now.accept == new_epoch_requests, h->phase(),
          "expected " + std::to_string(new_epoch_requests) + " accepted responses, got " +
              std::to_string(now.accept));

  reporter->Check(true, "redirect-late-response: pre-redirect responses dropped, "
                        "post-redirect delivered, credit balanced");
}

void PhaseLateButLive(Harness* h, mosaic::Reporter* reporter, Requester* req) {
  h->Phase("late-but-live");
  h->Reset(4);

  Stim issue;
  issue.req_valid = true;
  issue.req_pc = kBasePc;
  const Expect issued = h->Cycle(issue);
  Require(issued.req_fire, h->phase(), "the first request was refused");
  req->Accept(/*rst=*/false,
              Requester::Make(issued.req_id, issued.req_epoch, issue.req_pc, 0, 0));

  // Sixteen idle cycles. Latency is not a timeout: a response for a request that
  // is still outstanding is live however late it is. Only a *stale epoch* is
  // dropped, and the distinction has to survive an arbitrary delay.
  Stim idle;
  idle.out_ready = true;
  for (int i = 0; i < 16; i++) {
    const Expect e = h->Cycle(idle);
    Require(e.outstanding == 1, h->phase(),
            "a request expired on its own after " + std::to_string(i) +
                " idle cycles");
  }

  const Requester::Pending p = req->pending.front();
  req->pending.pop_front();
  const Expect e = h->Cycle(req->Respond(p));
  const Expect post = h->PeekFinal();
  Require(e.rsp_live, h->phase(),
          "a response for a still-outstanding request was not accepted after 16 cycles");
  Require(post.out_valid, h->phase(), "a late but live response delivered nothing");
  Require(post.out_pc == p.pc, h->phase(),
          "a late but live response was delivered at the wrong PC");
  Require(post.outstanding == 0, h->phase(), "the credit was not returned by the accept");

  reporter->Check(true, "late-but-live: latency alone never drops a response");
}

void PhaseCreditExactlyOnce(Harness* h, mosaic::Reporter* reporter,
                            mosaic::Rng* rng, Requester* req) {
  h->Phase("credit-exactly-once");
  h->Reset(4);

  // Three redirects over a table that is kept busy, so every drop is a drop of a
  // request that really did take a credit, and every cancel really did retire a
  // live one. The invariant has been checked every cycle by the harness; what
  // this phase adds is the end-of-phase count, where a leak or a double return
  // shows up as a number rather than as a rate.
  std::deque<Requester::Pending> pending;
  uint64_t pc = kBasePc;
  for (int step = 0; step < 120; step++) {
    Stim s;
    s.out_ready = true;
    s.req_valid = (rng->Below(4) != 0);
    s.req_pc = pc;

    const bool redirect = (step % 37) == 36;
    if (redirect) {
      s.redirect_valid = true;
      s.redirect_pc = kBasePc + 0x1000 * static_cast<uint64_t>(step);
    }

    // Answer something if there is anything to answer, preferring the oldest --
    // and hold it until it is taken.
    if (!req->holding && !pending.empty() && (rng->Below(2) == 0)) {
      const size_t which = rng->Below(static_cast<uint32_t>(pending.size()));
      req->Hold(pending[which]);
      pending.erase(pending.begin() + static_cast<long>(which));
    }
    req->Apply(&s);

    const Expect e = h->Cycle(s);
    req->Retire(e);
    if (e.req_fire) {
      pending.push_back(Requester::Make(e.req_id, e.req_epoch, s.req_pc, 0, 0));
      pc += 4;
    }
  }

  // Drain: every request that is still outstanding gets its response.
  while (!pending.empty() || req->holding) {
    if (!req->holding && !pending.empty()) {
      req->Hold(pending.front());
      pending.pop_front();
    }
    Stim s;
    s.out_ready = true;
    req->Apply(&s);
    req->Retire(h->Cycle(s));
  }
  // And then let the last deliveries drain out of the output register.
  Stim drain;
  drain.out_ready = true;
  for (int i = 0; i < 4; i++) h->Cycle(drain);

  const Expect e = h->PeekFinal();
  Require(e.outstanding == 0, h->phase(),
          "requests leaked: outstanding=" + std::to_string(e.outstanding) +
              " after every request was answered");
  Require(e.issued == e.accept + e.credit_drop + e.outstanding, h->phase(),
          "the credit identity does not hold at the end of the phase");
  Require(e.credit_drop == e.drop, h->phase(),
          "the credit returned by drops (" + std::to_string(e.credit_drop) +
              ") does not equal the number of drops (" + std::to_string(e.drop) + ")");
  Require(e.drop > 0, h->phase(), "the phase never dropped anything");
  Require(e.accept > 0, h->phase(), "the phase never accepted anything");
  Require(e.cancel > 0, h->phase(), "the phase never redirected");

  reporter->Check(true, "credit-exactly-once: every credit returned exactly once");
}

void PhaseBoundEnforced(Harness* h, mosaic::Reporter* reporter) {
  h->Phase("bound-enforced");
  h->Reset(4);

  const uint32_t bound = h->outstanding_;
  uint32_t saw_full = 0;
  uint32_t saw_denial = 0;

  // The issue port is hammered and the responder never answers: the table fills
  // and then must refuse, visibly.
  Stim s;
  s.req_valid = true;
  s.out_ready = true;
  for (uint32_t i = 0; i < 8 * bound + 8; i++) {
    s.req_pc = kBasePc + 4 * static_cast<uint64_t>(i);
    const Expect e = h->Cycle(s);
    if (e.outstanding == bound) saw_full++;
    if (s.req_valid && !e.req_ready) saw_denial++;
    Require(e.outstanding <= bound, h->phase(),
            "outstanding reached " + std::to_string(e.outstanding) +
                ", past MOSAIC_FETCH_OUTSTANDING " + std::to_string(bound));
  }
  Require(saw_full > 0, h->phase(), "the table never reached the bound");
  Require(saw_denial > 0, h->phase(), "the table never had to refuse an issue");
  Require(h->PeekFinal().deny == saw_denial, h->phase(),
          "refusals were not all reported: deny_count=" +
              std::to_string(h->PeekFinal().deny) + " observed=" +
              std::to_string(saw_denial));

  reporter->Check(true, "bound-enforced: outstanding never passed the bound, every "
                        "refusal was reported");
}

void PhaseSameCycle(Harness* h, mosaic::Reporter* reporter) {
  h->Phase("same-cycle");
  h->Reset(4);

  // Every combination of (a request offered, a response offered, a redirect, a
  // ready sink) against every kind of response, exhaustively rather than by
  // sampling. Each case asserts the credit identity -- which Cycle() has
  // already checked on the combination cycle and the one after -- and then the
  // specific consequence of that combination, so that a change which keeps the
  // ledger balanced while delivering the wrong thing is still caught.
  enum Kind { kNone, kLive, kStaleOwn, kStaleRecycled, kSquashed, kKindCount };
  const char* kind_name[kKindCount] = {"none", "live", "stale-own-slot",
                                       "stale-recycled-slot", "squashed"};

  uint32_t combinations = 0;
  for (int kind = 0; kind < kKindCount; kind++) {
    for (int redirect = 0; redirect < 2; redirect++) {
      for (int issue = 0; issue < 2; issue++) {
        for (int out_ready = 0; out_ready < 2; out_ready++) {
          h->Reset(4);

          // --- the preconditions ------------------------------------------
          // Two requests in one epoch, so that both slots are occupied and a
          // redirect has something to retire:
          //
          //   slot_a: still outstanding under the first epoch
          //   slot_b: answered and released, then re-issued after a redirect,
          //           so it holds the second epoch -- which is what makes a
          //           response stamped with the first epoch a response for a
          //           *recycled* slot, the card's first ABA counterexample.
          uint32_t slot_a = 0;
          uint32_t slot_b = 0;
          uint32_t epoch0 = 0;
          {
            Stim s;
            s.req_valid = true;
            s.req_pc = kBasePc;
            const Expect e = h->Cycle(s);
            slot_a = e.req_id;
            epoch0 = e.req_epoch;
          }
          {
            Stim s;
            s.req_valid = true;
            s.req_pc = kBasePc + 4;
            const Expect e = h->Cycle(s);
            slot_b = e.req_id;
            Require(slot_b != slot_a, h->phase(),
                    "the two setup requests did not take different slots");
          }
          {
            Stim s;
            s.rsp_valid = true;
            s.rsp_id = slot_b;
            s.rsp_epoch = epoch0;
            s.rsp_data = 0x00000013u;
            s.rsp_len = 4;
            const Expect e = h->Cycle(s);
            Require(e.rsp_live, h->phase(), "the setup response was not live");
          }
          {
            // Retire slot_a's request, then reuse slot_b under the new epoch.
            Stim s;
            s.redirect_valid = true;
            s.redirect_pc = kBasePc + 0x8000;
            h->Cycle(s);
          }
          uint32_t epoch1 = 0;
          {
            Stim s;
            s.req_valid = true;
            s.req_pc = kBasePc + 8;
            const Expect e = h->Cycle(s);
            Require(e.req_fire && e.req_id == slot_b, h->phase(),
                    "the setup did not reuse the slot it should have");
            epoch1 = e.req_epoch;
          }

          // A `squashed` response needs an id that nothing owns. Answer slot_a
          // first, so its slot is free while the epoch is unchanged: the
          // response below is then a duplicate of a request already retired,
          // which is the card's second counterexample.
          if (kind == kSquashed) {
            Stim s;
            s.rsp_valid = true;
            s.rsp_id = slot_a;
            s.rsp_epoch = epoch0;
            s.rsp_data = 0x00000013u;
            s.rsp_len = 4;
            const Expect e = h->Cycle(s);
            Require(e.rsp_stale && e.rsp_release, h->phase(),
                    "the setup drop for slot_a was not a credit-returning stale drop");
          }

          // --- the combination cycle ---------------------------------------
          Stim s;
          s.out_ready = out_ready != 0;
          s.req_valid = issue != 0;
          s.req_pc = kBasePc + 0x100;
          s.redirect_valid = redirect != 0;
          s.redirect_pc = kBasePc + 0x9000;

          const std::string label = std::string("rsp=") + kind_name[kind] +
                                    " redirect=" + (redirect ? "1" : "0") +
                                    " issue=" + (issue ? "1" : "0") + " out_ready=" +
                                    (out_ready ? "1" : "0");

          switch (kind) {
            case kNone:
              break;
            case kLive:
              // slot_b's request, current epoch, slot not retired.
              s.rsp_valid = true;
              s.rsp_id = slot_b;
              s.rsp_epoch = epoch1;
              s.rsp_data = 0x00000013u;
              s.rsp_len = 4;
              break;
            case kStaleOwn:
              // slot_a, retired by the redirect above. When this case also
              // redirects, the response lands on the redirect cycle itself,
              // which is the combination that must drop it.
              s.rsp_valid = true;
              s.rsp_id = slot_a;
              s.rsp_epoch = epoch0;
              s.rsp_data = 0x00000013u;
              s.rsp_len = 4;
              break;
            case kStaleRecycled:
              // slot_b's *first* occupant: the slot now holds a different
              // request, so this response owns nothing.
              s.rsp_valid = true;
              s.rsp_id = slot_b;
              s.rsp_epoch = epoch0;
              s.rsp_data = 0x00000013u;
              s.rsp_len = 4;
              break;
            case kSquashed:
              // A duplicate response for a request already retired, stamped
              // with the epoch in force. Nothing owns it.
              s.rsp_valid = true;
              s.rsp_id = slot_a;
              s.rsp_epoch = epoch1;
              s.rsp_data = 0x00000013u;
              s.rsp_len = 4;
              break;
            default:
              break;
          }
          ++combinations;

          const Expect before = h->PeekFinal();
          const Expect e = h->Cycle(s);

          // The conservation law, checked once for every combination rather than
          // per kind: a cycle that retires one slot and issues one request must
          // leave the census exactly where it started, and a cycle that only
          // does one of the two must move it by exactly one.
          {
            const uint32_t after = h->PeekFinal().outstanding;
            const uint32_t retired = e.rsp_retire ? 1u : 0u;
            const uint32_t issued = e.req_fire ? 1u : 0u;
            Require(after + retired == before.outstanding + issued, h->phase(),
                    label + ": credits did not balance across the cycle (outstanding "
                    "went from " + std::to_string(before.outstanding) + " to " +
                    std::to_string(after) + " with " + std::to_string(retired) +
                    " retired and " + std::to_string(issued) + " issued)");
          }

          if (kind == kLive && !redirect) {
            Require(e.rsp_live, h->phase(), label + ": a live response was not accepted");
            Require(h->PeekFinal().accept == before.accept + 1, h->phase(),
                    label + ": the accept counter did not move by one");
          }
          if (redirect) {
            Require(!e.req_fire, h->phase(),
                    label + ": a request was issued on a redirect cycle");
            Require(h->PeekFinal().epoch != before.epoch, h->phase(),
                    label + ": the redirect did not advance the epoch");
            const Expect post = h->PeekFinal();
            Require(!post.out_valid && !post.out_illegal && !post.out_fault, h->phase(),
                    label + ": something was delivered on a redirect cycle");
          }
          if (kind == kStaleOwn) {
            Require(!e.rsp_live, h->phase(),
                    label + ": a response from a retired request was accepted");
            Require(e.rsp_release, h->phase(),
                    label + ": a stale response did not return its credit");
            if (!e.req_fire) {
              Require(h->PeekFinal().outstanding + 1 == before.outstanding, h->phase(),
                      label + ": the drop did not free exactly one slot");
            }
          }
          if (kind == kStaleRecycled) {
            Require(!e.rsp_live, h->phase(),
                    label + ": a response for a recycled slot was accepted");
            Require(!e.rsp_release, h->phase(),
                    label + ": a response for a recycled slot returned a credit "
                            "that had already been spent");
          }
          if (kind == kSquashed) {
            Require(!e.rsp_live, h->phase(),
                    label + ": a response for an id that nothing owns was accepted");
            Require(!e.rsp_release, h->phase(),
                    label + ": a response that owns nothing returned a credit");
            Require(h->PeekFinal().squashed == before.squashed + 1, h->phase(),
                    label + ": the protocol error was not counted as squashed");
          }

          // Settle the machine and re-check the ledger: a release that was
          // merely deferred has to have happened by now.
          Stim drain;
          drain.out_ready = true;
          for (int i = 0; i < 3; i++) h->Cycle(drain);
          const Expect after = h->PeekFinal();
          Require(after.issued == after.accept + after.credit_drop + after.outstanding,
                  h->phase(), label + ": the credit identity broke after settling");
        }
      }
    }
  }
  Require(combinations == 5 * 2 * 2 * 2, h->phase(),
          "the combination sweep did not run every case");

  reporter->Check(true, "same-cycle: " + std::to_string(combinations) +
                            " issue/response/redirect/out_ready combinations keep the "
                            "credit identity");
}

void PhaseEpochWrap(Harness* h, mosaic::Reporter* reporter, Requester* req) {
  h->Phase("epoch-wrap");
  h->Reset(4);

  const uint32_t period = 1u << h->epoch_w_;

  // Enough redirects to go round the counter twice, so a stale response that
  // was stamped before the wrap is still around after it. Every redirect is
  // separated by a request and its response, because the interesting case is
  // the epoch arithmetic and not a stalled table.
  std::deque<Requester::Pending> pending;
  uint32_t wraps = 0;
  uint32_t redirects = 0;
  // The epoch in force after the previous redirect, which is what a wrap has to
  // be measured against: `Expect` from a cycle is the state *before* its edge.
  uint32_t last_epoch = h->PeekFinal().epoch;

  // Two requests are kept outstanding across a redirect, so there is always a
  // stale response in flight while the epoch moves.
  std::vector<Requester::Pending> stale_carried;

  const uint32_t target_redirects = 2 * period + 4;
  const uint32_t cycle_cap = 8 * target_redirects + 64;

  for (uint32_t i = 0; redirects < target_redirects && i < cycle_cap; i++) {
    Stim s;
    s.out_ready = true;

    // Offer a request on most cycles, and answer the oldest outstanding one on
    // the others.
    if (pending.empty() || (i % 3) != 0) {
      s.req_valid = true;
      s.req_pc = kBasePc + 4 * static_cast<uint64_t>(i);
    } else {
      const Requester::Pending p = pending.front();
      pending.pop_front();
      s.rsp_valid = true;
      s.rsp_id = p.id;
      s.rsp_epoch = p.epoch;
      s.rsp_data = p.data;
      s.rsp_len = p.len;
      s.rsp_fault = p.fault;
    }

    // One redirect in five, and never two cycles running: a redirect never gets
    // to answer a response in the same cycle here, which the same-cycle phase
    // covers exhaustively instead.
    const bool redirect = (i % 5) == 4;
    if (redirect) {
      s.redirect_valid = true;
      s.redirect_pc = kBasePc + 0x20000 + 4 * static_cast<uint64_t>(i);
      redirects++;
    }

    const Expect e = h->Cycle(s);
    if (e.req_fire) {
      pending.push_back(Requester::Make(e.req_id, e.req_epoch, s.req_pc, 0, 0));
    }
    if (redirect) {
      // Whatever was outstanding is now stale. Hold on to two of them and
      // answer them much later, well after the counter has wrapped.
      while (stale_carried.size() < 2 && !pending.empty()) {
        stale_carried.push_back(pending.front());
        pending.pop_front();
      }
      const uint32_t after = h->PeekFinal().epoch;
      if (after < last_epoch) wraps++;
      last_epoch = after;
    }
  }

  Require(redirects == target_redirects, h->phase(),
          "the sweep stopped early: " + std::to_string(redirects) + " of " +
              std::to_string(target_redirects) + " redirects");
  Require(wraps >= 2, h->phase(),
          "the epoch counter never wrapped (only " + std::to_string(wraps) +
              " wraps in " + std::to_string(redirects) + " redirects)");
  Require(!stale_carried.empty(), h->phase(),
          "no request survived long enough to be answered after the wrap");
  Require(last_epoch < period, h->phase(),
          "the epoch counter exceeded its declared width (epoch " +
              std::to_string(last_epoch) + ", modulus " + std::to_string(period) + ")");

  // The point of the phase: responses stamped before the wrap are still
  // rejected, because the slot that owns them is retired rather than recycled.
  for (const Requester::Pending& p : stale_carried) {
    const Expect e = h->Cycle(req->Respond(p));
    Require(!e.rsp_live, h->phase(),
            "a response stamped before the epoch wrapped was accepted (epoch " +
                std::to_string(p.epoch) + ", now " + std::to_string(e.epoch) + ")");
    Require(e.rsp_release, h->phase(),
            "a stale response after the wrap did not return its credit");
    Require(e.outstanding == 0 || h->PeekFinal().outstanding < h->outstanding_,
            h->phase(), "the drop did not free the slot it owned");
  }

  reporter->Check(true, "epoch-wrap: the counter wrapped and stale responses were "
                        "still rejected");
}

void PhaseCompressed16Bit(Harness* h, mosaic::Reporter* reporter, Requester* req) {
  h->Phase("compressed-16bit");
  h->Reset(4);

  // A 16-bit encoding returned in two bytes is an *instruction*: it is
  // delivered, with its own length (two) and its own bits (the low half only --
  // the upper half of the fetch window is the next instruction's encoding).
  // Before I-041 this was reported illegal; that refusal is the contract this
  // phase replaces, and this is what it is replaced with.
  Stim issue;
  issue.req_valid = true;
  issue.req_pc = kBasePc;
  const Expect issued = h->Cycle(issue);
  Require(issued.req_fire, h->phase(), "the request was refused");

  const Requester::Pending p =
      Requester::Make(issued.req_id, issued.req_epoch, issue.req_pc, 1, 0);
  const Expect e = h->Cycle(req->Respond(p));
  const Expect post = h->PeekFinal();

  Require(e.rsp_live, h->phase(), "the 16-bit response was not classified live");
  Require(post.out_valid, h->phase(),
          "a 16-bit instruction was not delivered as a decodable instruction");
  Require(!post.out_illegal, h->phase(),
          "a 16-bit instruction was reported illegal");
  Require(post.out_len == 2, h->phase(),
          "a 16-bit instruction was reported with length " +
              std::to_string(post.out_len) + ", expected 2");
  Require(post.out_bits == (p.data & 0xffffu), h->phase(),
          "a 16-bit instruction's bits were not preserved unmasked");
  Require((post.out_bits >> 16) == 0, h->phase(),
          "a 16-bit instruction carried its neighbour's bytes in the upper half");
  Require(post.out_cause == 0, h->phase(),
          "a delivered instruction carried a cause");
  Require(post.out_pc == issue.req_pc, h->phase(),
          "the delivery lost the PC the instruction belongs to");
  Require(post.delivered == 1 && post.illegal == 0, h->phase(),
          "the 16-bit instruction was not counted as delivered exactly once");
  Require(post.outstanding == 0, h->phase(),
          "a delivered instruction did not return its slot's credit");

  // The same encoding returned in a four-byte window. The *encoding* still says
  // 16 bits, so the instruction is still two bytes long and its upper half is
  // still not part of it: a rule that trusted the byte count would report four
  // and hand the decoder the next instruction's bytes.
  {
    Stim again;
    again.req_valid = true;
    again.req_pc = kBasePc + 4;
    const Expect accepted = h->Cycle(again);
    Require(accepted.req_fire, h->phase(),
            "the table did not accept a request after a delivery");

    const Requester::Pending q =
        Requester::Make(accepted.req_id, accepted.req_epoch, again.req_pc, 3, 0);
    h->Cycle(req->Respond(q));
    const Expect second = h->PeekFinal();
    Require(second.out_valid, h->phase(),
            "a compressed encoding in a four-byte window was not delivered");
    Require(second.out_len == 2, h->phase(),
            "the byte count decided the length instead of the encoding (len=" +
                std::to_string(second.out_len) + ")");
    Require((second.out_bits >> 16) == 0, h->phase(),
            "the four-byte window's upper half was carried into a 16-bit "
            "instruction's bits");
    Require(second.delivered == 2, h->phase(),
            "both 16-bit encodings should be counted as delivered (delivered=" +
                std::to_string(second.delivered) + ")");
    Require(second.outstanding == 0, h->phase(),
            "a delivered instruction did not return its slot's credit");
  }

  // A 32-bit *encoding* returned in two bytes is malformed: the instruction
  // cannot be assembled from what arrived, so it is refused rather than
  // delivered half-decoded.
  {
    Stim third;
    third.req_valid = true;
    third.req_pc = kBasePc + 8;
    const Expect accepted = h->Cycle(third);
    Require(accepted.req_fire, h->phase(),
            "the table did not accept a request after a delivery");

    const Requester::Pending q =
        Requester::Make(accepted.req_id, accepted.req_epoch, third.req_pc, 4, 0);
    h->Cycle(req->Respond(q));
    const Expect last = h->PeekFinal();
    Require(last.out_illegal, h->phase(),
            "a 32-bit encoding returned in two bytes was delivered anyway");
    Require(last.out_len == 4, h->phase(),
            "the malformed response's report did not carry a length");
    Require(last.illegal == 1, h->phase(),
            "the malformed response was not counted as illegal");
  }

  reporter->Check(true, "compressed-16bit: a 16-bit encoding is delivered with its own "
                        "length and its own bits, and a truncated 32-bit one is refused");
}

void PhaseFaultPath(Harness* h, mosaic::Reporter* reporter, Requester* req) {
  h->Phase("fault-path");
  h->Reset(4);

  Stim issue;
  issue.req_valid = true;
  issue.req_pc = kBasePc;
  const Expect issued = h->Cycle(issue);
  Require(issued.req_fire, h->phase(), "the request was refused");

  const Requester::Pending p =
      Requester::Make(issued.req_id, issued.req_epoch, issue.req_pc, 2, 0);
  const Expect e = h->Cycle(req->Respond(p));
  const Expect post = h->PeekFinal();

  Require(e.rsp_live, h->phase(), "the faulting response was not classified live");
  Require(!post.out_valid, h->phase(),
          "an instruction access fault was delivered as an instruction");
  Require(!post.out_illegal, h->phase(),
          "an instruction access fault was reported as an illegal encoding");
  Require(post.out_fault, h->phase(), "an instruction access fault was not reported");
  Require(post.out_cause == kExcInsnAccess, h->phase(),
          "the fault reported cause " + std::to_string(post.out_cause) +
              ", expected instruction access fault (" +
              std::to_string(kExcInsnAccess) + ")");
  Require(post.fault == 1, h->phase(),
          "the fault was not counted (fault=" + std::to_string(post.fault) + ")");
  Require(post.outstanding == 0, h->phase(),
          "an instruction access fault did not return its slot's credit");
  Require(post.accept == 1, h->phase(),
          "a faulting response did not consume its request exactly once");
  Require(post.delivered == 0, h->phase(),
          "a faulting response was counted as a delivered instruction");

  // And the machine keeps working afterwards: a fault is an event, not a halt.
  Stim again;
  again.req_valid = true;
  again.req_pc = kBasePc + 4;
  const Expect next = h->Cycle(again);
  Require(next.req_fire, h->phase(),
          "the fetch unit stopped issuing requests after an instruction fault");

  reporter->Check(true, "fault-path: cause 1 reported, credit returned, machine alive");
}

void PhasePredictorAdvisory(Harness* h, mosaic::Reporter* reporter) {
  h->Phase("predictor-advisory");
  h->Reset(4);

  // A cold table has no target for a jump, so the query must *report* a miss and
  // fetch must fall through to pc + 4 rather than treating the fall-through
  // address it gets back as a prediction.
  {
    Stim s;
    s.pred_valid = true;
    s.pred_pc = kBasePc;
    s.pred_is_jump = true;
    const Expect e = h->Cycle(s);
    Require(e.pred_btb_miss, h->phase(),
            "a cold jump query did not report that no target was available");
    Require(e.pred_next_pc == s.pred_pc + 4, h->phase(),
            "a reported miss did not fall through to pc + 4 (got " +
                Hex64(e.pred_next_pc) + ")");
    Require(e.pred_next_valid, h->phase(), "a valid query produced no next PC");
  }

  // Train an unconditional jump and query it again.
  const uint64_t jump_pc = kBasePc + 0x80;
  const uint64_t jump_target = kBasePc + 0x4000;
  {
    Stim s;
    s.upd_valid = true;
    s.upd_pc = jump_pc;
    s.upd_is_jump = true;
    s.upd_target = jump_target;
    h->Cycle(s);
  }
  {
    Stim s;
    s.pred_valid = true;
    s.pred_pc = jump_pc;
    s.pred_is_jump = true;
    const Expect e = h->Cycle(s);
    Require(e.pred_btb_hit, h->phase(), "a trained jump did not hit");
    Require(!e.pred_btb_miss, h->phase(), "a trained jump still reported a miss");
    Require(e.pred_next_pc == jump_target, h->phase(),
            "the predicted target was not used (got " + Hex64(e.pred_next_pc) +
                ", expected " + Hex64(jump_target) + ")");
  }

  // A redirect in the same cycle voids the hint rather than letting it be
  // applied and then contradicted.
  {
    Stim s;
    s.pred_valid = true;
    s.pred_pc = jump_pc;
    s.pred_is_jump = true;
    s.redirect_valid = true;
    s.redirect_pc = kBasePc + 0x90000;
    const Expect e = h->Cycle(s);
    Require(e.pred_squashed, h->phase(),
            "a redirect did not squash the prediction in the same cycle");
    Require(!e.pred_next_valid, h->phase(),
            "a squashed prediction was still published as valid");
  }

  // A return with an empty stack is reported, not answered with a garbage
  // address: this is the report the predictor header says fetch cannot
  // reconstruct for itself.
  {
    Stim s;
    s.pred_valid = true;
    s.pred_pc = kBasePc + 0x100;
    s.pred_is_jump = true;
    s.pred_is_return = true;
    const Expect e = h->Cycle(s);
    Require(e.pred_ras_underflow, h->phase(),
            "a return predicted on an empty return-address stack was not reported");
    Require(e.pred_next_pc == s.pred_pc + 4, h->phase(),
            "a return on an empty stack was answered with something other than "
            "the fall-through address");
  }

  reporter->Check(true, "predictor-advisory: a miss falls through, a hit redirects, a "
                        "redirect squashes, an empty stack is reported");
}

void PhaseRandom(Harness* h, mosaic::Reporter* reporter, mosaic::Rng* rng,
                 Requester* req) {
  h->Phase("random");
  h->Reset(4);

  std::deque<Requester::Pending> pending;
  uint64_t next_pc = kBasePc;
  uint32_t redirects = 0;

  // Enough cycles for the campaign to reach the geometry from several angles.
  const uint32_t cycles = 4000;
  for (uint32_t i = 0; i < cycles; i++) {
    Stim s;
    s.out_ready = (rng->Below(8) != 0);

    s.req_valid = (rng->Below(3) != 0);
    s.req_pc = next_pc;

    // A query about a PC drawn from a small set, so the predictor is asked about
    // the same addresses often enough for the BTB to matter.
    if (rng->Below(2) != 0) {
      s.pred_valid = true;
      s.pred_pc = kBasePc + 4 * static_cast<uint64_t>(rng->Below(16));
      s.pred_is_branch = (rng->Below(2) != 0);
      s.pred_is_jump = !s.pred_is_branch;
      s.pred_is_return = s.pred_is_jump && (rng->Below(4) == 0);
    }

    if (rng->Below(16) == 0) {
      s.upd_valid = true;
      s.upd_pc = kBasePc + 4 * static_cast<uint64_t>(rng->Below(16));
      s.upd_is_branch = (rng->Below(2) != 0);
      s.upd_is_jump = !s.upd_is_branch;
      s.upd_is_call = s.upd_is_jump && (rng->Below(3) == 0);
      s.upd_is_return = s.upd_is_jump && !s.upd_is_call && (rng->Below(3) == 0);
      s.upd_is_taken = (rng->Below(2) != 0);
      s.upd_target = kBasePc + 4 * static_cast<uint64_t>(rng->Below(16)) + 0x40;
      s.ckpt_valid = (rng->Below(8) == 0);
      s.flush = (rng->Below(32) == 0);
    }

    if (rng->Below(20) == 0) {
      s.redirect_valid = true;
      s.redirect_pc = kBasePc + 0x400 * static_cast<uint64_t>(rng->Below(64));
      redirects++;
    }

    // Answer a random outstanding request, so responses come back out of order
    // and some of them are for requests a redirect has already retired. A
    // response is held until it is taken.
    if (!req->holding && !pending.empty() && (rng->Below(2) == 0)) {
      const size_t which = rng->Below(static_cast<uint32_t>(pending.size()));
      req->Hold(pending[which]);
      pending.erase(pending.begin() + static_cast<long>(which));
    }
    req->Apply(&s);

    const Expect e = h->Cycle(s);
    req->Retire(e);
    if (e.req_fire) {
      // I-041: the random traffic now includes the compressed encodings (form 1:
      // 16-bit in two bytes; form 3: 16-bit in four) and the malformed one (form 4:
      // a 32-bit encoding in two bytes), as well as faults (form 2).
      const int form = static_cast<int>(rng->Below(16) == 0 ? rng->Below(5) : 0);
      pending.push_back(Requester::Make(e.req_id, e.req_epoch, s.req_pc, form, i));
      next_pc += 4;
    }
  }

  // Drain everything still outstanding, then let the output register empty.
  while (!pending.empty() || req->holding) {
    if (!req->holding && !pending.empty()) {
      req->Hold(pending.front());
      pending.pop_front();
    }
    Stim s;
    s.out_ready = true;
    req->Apply(&s);
    req->Retire(h->Cycle(s));
  }
  Stim drain;
  drain.out_ready = true;
  for (int i = 0; i < 8; i++) h->Cycle(drain);

  const Expect e = h->PeekFinal();
  // The campaign has to have done something. A run in which nothing was ever
  // dropped or never accepted would pass a shadow comparison vacuously.
  Require(redirects > 0, h->phase(), "the campaign never redirected");
  Require(e.drop > 0, h->phase(), "the campaign never dropped a late response");
  Require(e.accept > 0, h->phase(), "the campaign never accepted a response");
  Require(e.fault > 0, h->phase(), "the campaign never produced a fetch fault");
  Require(e.illegal > 0, h->phase(), "the campaign never produced a rejected encoding");
  Require(e.deny > 0, h->phase(), "the campaign never reached the bound");
  Require(e.outstanding == 0, h->phase(),
          "requests leaked over the campaign: outstanding=" +
              std::to_string(e.outstanding));
  Require(e.issued == e.accept + e.credit_drop + e.outstanding, h->phase(),
          "the credit identity broke over the campaign");

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
  Vmosaic_fetch_tb dut;

  // A placeholder geometry until the elaborated instance is read back below. The
  // shadow is re-created with the real numbers before any check runs.
  ShadowFetch shadow(4, 7, 64, 512, 16);
  Harness harness(&dut, &clk, options.max_cycles, &shadow);

  std::string detail;
  bool passed = true;
  try {
    clk.BeginReset(4);
    for (int i = 0; i < 4; i++) harness.Cycle(Stim{}, /*rst=*/true);

    // The geometry comes from the elaborated DUT, and from nowhere else, so a
    // profile change moves the hardware and the shadow together.
    const uint32_t outstanding = dut.o_fetch_outstanding;
    const uint32_t epoch_w = dut.o_epoch_w;
    const uint32_t id_w = dut.o_id_w;
    const uint32_t cnt_w = dut.o_cnt_w;
    Require(outstanding > 0, "geometry", "the DUT reported zero fetch_outstanding");
    Require(epoch_w >= 2 && epoch_w < 32, "geometry",
            "implausible epoch width " + std::to_string(epoch_w));
    Require(id_w >= 1 && id_w < 32, "geometry",
            "implausible request-id width " + std::to_string(id_w));
    Require(cnt_w >= 1 && cnt_w < 32, "geometry",
            "implausible count width " + std::to_string(cnt_w));
    Require(dut.o_xlen == 64, "geometry", "expected a 64-bit XLEN");
    // The declared widths have to be able to do their jobs. These are real
    // checks and not restatements of the RTL: a request id that cannot name the
    // last slot, or a count that cannot say "full", is a geometry bug that only
    // shows up once the table is busy.
    Require((1u << id_w) >= outstanding, "geometry",
            "a " + std::to_string(id_w) + "-bit request id cannot name " +
                std::to_string(outstanding) + " slots");
    Require((1u << cnt_w) > outstanding, "geometry",
            "a " + std::to_string(cnt_w) + "-bit count cannot express \"full\" at " +
                std::to_string(outstanding) + " outstanding");
    Require((1u << epoch_w) > outstanding + 1, "geometry",
            "an epoch modulus of " + std::to_string(1u << epoch_w) +
                " does not exceed the " + std::to_string(outstanding + 1) +
                " epochs that can be live at once");

    // The geometry only reaches the wrapper through the readback ports, so the
    // shadow is rebuilt and rebound once the numbers are known.
    shadow = ShadowFetch(outstanding, epoch_w, 64, 512, 16);
    harness.BindShadow(&shadow);
    harness.outstanding_ = outstanding;
    harness.epoch_w_ = epoch_w;
    harness.id_w_ = id_w;
    harness.cnt_w_ = cnt_w;

    Requester requester;
    mosaic::Rng rng(options.seed);

    PhaseResetState(&harness, &reporter);
    PhaseResetTraffic(&harness, &reporter, &requester);
    PhaseRedirectLateResponse(&harness, &reporter, &requester);
    PhaseLateButLive(&harness, &reporter, &requester);
    PhaseCreditExactlyOnce(&harness, &reporter, &rng, &requester);
    PhaseBoundEnforced(&harness, &reporter);
    PhaseSameCycle(&harness, &reporter);
    PhaseEpochWrap(&harness, &reporter, &requester);
    PhaseCompressed16Bit(&harness, &reporter, &requester);
    PhaseFaultPath(&harness, &reporter, &requester);
    PhasePredictorAdvisory(&harness, &reporter);
    PhaseRandom(&harness, &reporter, &rng, &requester);

    detail = "fetch contract holds: " + std::to_string(harness.comparisons()) +
             " comparisons over " + std::to_string(harness.cycles()) + " cycles, seed " +
             std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    // The count of comparisons completed before the failure is part of the
    // diagnosis: it is the delta a mutation is measured by. A run that passed
    // every comparison reports the same number in the success line, so "how far
    // did it get" is answerable without re-running anything.
    detail = "contract violated after " + std::to_string(harness.comparisons()) +
             " comparisons in " + std::to_string(harness.cycles()) + " cycles: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}