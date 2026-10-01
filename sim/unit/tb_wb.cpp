// ============================================================================
// tb_wb.cpp -- CASE=wb.same_bank_many_producers, work package I-026.
//
// The DUT is a *pair*: `mosaic_wb_arbiter` decides what may become durable and
// `mosaic_prf` is where "durable" has to mean something. Neither is its own
// oracle. The driver is the environment the arbiter runs in -- rename's
// writeback rule and the ROB's identity-liveness rule -- and, every cycle, an
// independent comparison of the arbiter's presentation against what the
// contract says it must be:
//
//   * rename is modelled from its documented rule (accept iff the (tag,
//     generation) is the current owner and has not been written; otherwise
//     stale, or duplicate if the current owner has already written). It is *not*
//     read out of mosaic_rename.sv, which this case does not compile.
//   * the ROB is modelled from its documented rule (an identity is live until
//     its macro is discarded; a second completion of a live identity is a
//     duplicate).
//   * the register file is not modelled at all: the value is read back out of
//     the elaborated `mosaic_prf` through its read ports with the matching
//     (tag, generation), which is the only evidence that "the value is durable"
//     that is not a restatement of the arbiter's own write port.
//
// ------------------------------------------------------------------ the proof
//
//   1. same-bank serialisation without loss   two completions offered in one
//      cycle, one per cluster, whose destinations share a bank. Both are
//      captured (`wb_ready` high, nothing refused), both are written to the PRF,
//      `o_wr_ctr` advances by exactly two, `o_collision_ctr` by at least one,
//      and each offered value is read back out of the register file with its own
//      (tag, generation). Every offered completion appears exactly once.
//
//   2. never woken before durable   combinationally, in every cycle: a wakeup
//      is only ever published in the same cycle the register file's write port
//      presents that exact (tag, generation, value) with `wr_en` and
//      `wr_gen_valid` set for the tag's home bank, *and* the completion was
//      accepted by the rename port. `wu_valid` without `prf_wr_en` for the
//      value's bank, or without `ren_wb_accepted`, is the failure.
//
//   3. a stale result changes nothing and is counted   a completion whose
//      destination generation is not the tag's current one raises
//      `o_stale_ctr`, does not raise `o_wr_ctr`, presents no PRF write and no
//      wakeup, and leaves the register file's contents bit-for-bit unchanged.
//
//   4. a duplicate producer writes once   the same (tag, generation) offered a
//      second time raises `o_dup_ctr`, does not write, and the register file
//      still holds the *first* value.
//
// plus the ready-table query: `q_written` is 1 for the written (tag, generation)
// and 0 for the same tag at another generation and for an unwritten tag.
//
// Standing invariants, checked on every cycle rather than in one phase: the
// arbiter's counters against an independent tally; the conservation identity
// offered == published at the end of every phase; and "the thing the arbiter
// says it is publishing is a completion a producer actually offered".
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_wb_arbiter_tb.h"

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

std::string At(const std::string& where, uint64_t cycle) {
  return where + ": cycle " + std::to_string(cycle);
}

uint32_t Mask(uint32_t value, uint32_t width) {
  return (width >= 32) ? value : (value & ((1u << width) - 1u));
}

// ------------------------------------------------------------------ an event
// One completion, as a producer presents it. This mirrors mosaic_uop_pkg::
// wb_event_t, but only the fields the completion path acts on -- the exception
// payload's cause/tval are carried as zeros by the wrapper and are not checked
// here because the arbiter does not act on them.
struct Event {
  uint32_t rob_index = 0;
  uint32_t rob_gen = 0;
  uint32_t uop_index = 0;
  uint32_t tag = 0;
  uint32_t gen = 0;  // the 8-bit PRF generation
  bool x0 = false;
  bool value_valid = false;
  uint64_t value = 0;
  bool exc_valid = false;
  bool is_store = false;
  bool is_load = false;
};

std::string Describe(const Event& e) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "[id=%u.0x%x.%u tag=%u gen=%u x0=%d vv=%d val=0x%llx exc=%d "
                "st=%d ld=%d]",
                e.rob_index, e.rob_gen, e.uop_index, e.tag, e.gen, e.x0,
                e.value_valid, static_cast<unsigned long long>(e.value),
                e.exc_valid, e.is_store, e.is_load);
  return buf;
}

// ------------------------------------------------------------ rename's rule
//
// The writeback rule as mosaic_rename.sv documents it, nothing more: a tag owns
// one generation at a time, and a generation of a tag is written at most once.
// The arbiter offers the low bits of the destination generation (the width
// rename's allocation counter carries), so the model keys on exactly what is
// offered.
class RenameModel {
 public:
  struct Answer {
    bool accepted = false;
    bool stale = false;
    bool duplicate = false;
  };

  // Declare `tag`'s current assigned generation. A tag that has not been
  // declared owns nothing, and any offer naming it is stale.
  void Assign(uint32_t tag, uint32_t gen) {
    State& s = state_[tag];
    s.assigned = true;
    s.gen = gen;
  }

  Answer Query(bool valid, uint32_t tag, uint32_t gen) const {
    Answer a;
    if (!valid) return a;
    auto it = state_.find(tag);
    if (it == state_.end() || !it->second.assigned || it->second.gen != gen) {
      a.stale = true;
      return a;
    }
    if (it->second.written.count(gen) != 0) {
      a.duplicate = true;
      return a;
    }
    a.accepted = true;
    return a;
  }

  // A writeback the arbiter offered *and* this model accepted has happened.
  void Commit(uint32_t tag, uint32_t gen) { state_[tag].written.insert(gen); }

  bool Written(uint32_t tag, uint32_t gen) const {
    auto it = state_.find(tag);
    return it != state_.end() && it->second.written.count(gen) != 0;
  }

 private:
  struct State {
    bool assigned = false;
    uint32_t gen = 0;
    std::set<uint32_t> written;
  };
  std::map<uint32_t, State> state_;
};

// ---------------------------------------------------------------- ROB's rule
//
// The identity of a live macro. The arbiter only needs "is this identity still
// live": a completion of a discarded macro must not reach a register file.
class RobModel {
 public:
  struct Answer {
    bool accepted = false;
    bool duplicate = false;
    bool stale = false;
    bool bad_uop = false;
  };

  struct Key {
    uint32_t index, gen, uop;
    bool operator<(const Key& o) const {
      if (index != o.index) return index < o.index;
      if (gen != o.gen) return gen < o.gen;
      return uop < o.uop;
    }
  };

  // Insert if absent. A producer re-offering an identity the ROB has already
  // completed must stay "done": the identity of a macro is unique, so a second
  // completion of it is a duplicate, not a fresh arrival.
  void DeclareLive(const Event& e) {
    live_.emplace(Key{e.rob_index, e.rob_gen, e.uop_index}, false);
  }

  void DeclareDead(const Event& e) { live_.erase(Key{e.rob_index, e.rob_gen, e.uop_index}); }

  Answer Query(bool valid, uint32_t index, uint32_t gen, uint32_t uop) const {
    Answer a;
    if (!valid) return a;
    auto it = live_.find(Key{index, gen, uop});
    if (it == live_.end()) {
      a.stale = true;
      return a;
    }
    if (it->second) {
      a.duplicate = true;
      return a;
    }
    a.accepted = true;
    return a;
  }

  // The arbiter published this identity and this model accepted it: the macro
  // has now completed once.
  void Commit(bool valid, uint32_t index, uint32_t gen, uint32_t uop, bool accepted) {
    if (!valid || !accepted) return;
    auto it = live_.find(Key{index, gen, uop});
    if (it != live_.end()) it->second = true;
  }

 private:
  std::map<Key, bool> live_;  // key -> already completed
};

// ---------------------------------------------------------- durable contents
// What the register file is expected to hold, per physical tag: the (generation,
// value) of the last accepted write. Used to compare against the register file
// actually read back, not to replace the read-back.
class PrfExpect {
 public:
  void Write(uint32_t tag, uint32_t gen, uint64_t value) {
    Entry& e = entries_[tag];
    e.written = true;
    e.gen = gen;
    e.value = value;
  }
  bool Written(uint32_t tag) const {
    auto it = entries_.find(tag);
    return it != entries_.end() && it->second.written;
  }
  uint32_t Gen(uint32_t tag) const { return entries_.at(tag).gen; }
  uint64_t Value(uint32_t tag) const { return entries_.at(tag).value; }

 private:
  struct Entry {
    bool written = false;
    uint32_t gen = 0;
    uint64_t value = 0;
  };
  std::map<uint32_t, Entry> entries_;
};

// ---------------------------------------------------------------- the tally
struct Tally {
  uint64_t offered = 0;
  uint64_t published = 0;
  uint64_t written = 0;
  uint64_t wakeups = 0;
  uint64_t stale = 0;
  uint64_t duplicate = 0;
  uint64_t rob_stale = 0;
  uint64_t rob_duplicate = 0;
  uint64_t rob_ok = 0;
  uint64_t rob_bad = 0;
  uint64_t drops = 0;
  uint64_t wide_gen = 0;
  uint64_t stores = 0;
  uint64_t loads = 0;
  uint64_t collisions = 0;  // observed from the DUT, monotone
};

class Harness {
 public:
  Harness(Vmosaic_wb_arbiter_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  // Only the widths the comparison actually folds into a mask are kept: the tag
  // and generation widths the driver does not need to re-derive are checked in
  // main() against their contract relationship instead of being stored.
  void Configure(uint32_t banks, uint32_t pgen_w, uint32_t igen_w) {
    banks_ = banks;
    pgen_w_ = pgen_w;
    igen_w_ = igen_w;
  }

  void Phase(const std::string& name) { phase_ = name; }

  // Reset the DUT and every model, so no phase can pass on state the previous
  // one left behind. `totals_` is deliberately *not* reset: it is the run's
  // running count.
  void Fresh(int cycles = 4) {
    for (int i = 0; i < cycles; i++) Cycle(/*rst=*/true);
    rename_ = RenameModel();
    rob_ = RobModel();
    prf_expect_ = PrfExpect();
    for (Held& h : held_) h = Held{};
    inflight_.clear();
    exp_ = Tally();
    prev_collision_ = 0;
  }

  RenameModel& Rename() { return rename_; }
  RobModel& Rob() { return rob_; }
  const Tally& Totals() const { return totals_; }
  const Tally& Expected() const { return exp_; }
  const PrfExpect& Prf() const { return prf_expect_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }

  // Offer `e` on producer `port` (0 = cluster 0, 1 = cluster 1, 2 = MUL/DIV). The
  // producer holds it until the arbiter takes it, exactly as the interface
  // contract requires.
  void Offer(int port, const Event& e) {
    Require(port >= 0 && port < 3, phase_, "bad producer port");
    Require(!held_[port].valid, phase_, "producer " + std::to_string(port) +
                                            " already holds a completion");
    // Every identity a phase offers must be one the ROB considers live until the
    // phase says otherwise.
    rob_.DeclareLive(e);
    held_[port].valid = true;
    held_[port].ev = e;
  }

  // Run until the arbiter has consumed every offered completion *and* one more
  // quiet cycle proves nothing is left to publish.
  void Drain(uint64_t limit = 64) {
    uint64_t guard = 0;
    while (AnyHeld() || !inflight_.empty()) {
      Cycle(/*rst=*/false);
      Require(++guard <= limit, At(phase_, cycles_),
              "the arbiter did not publish the pending completions within " +
                  std::to_string(limit) + " cycles");
    }
    Cycle(/*rst=*/false);  // quiet: must publish nothing
  }

  bool AnyHeld() const {
    return held_[0].valid || held_[1].valid || held_[2].valid;
  }

  // --- one cycle: drive, settle the combinational answers, compare, edge -----
  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + std::to_string(max_cycles_) +
                       ") exhausted before the phase finished");
    }

    // Stage 1: everything the arbiter reads that is *not* an answer.
    DriveInputs(rst);
    dut_->eval();

    // Stage 2: the answers, computed from what the arbiter is presenting. Both
    // are combinational functions of the arbiter's own outputs, exactly like the
    // real rename and ROB, so there is no cycle here: no answer can depend on
    // another answer.
    const Answers ans = ComputeAnswers();
    DriveAnswers(ans);
    dut_->eval();

    if (!rst) {
      Check(ans);
      Apply(ans);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  // ------------------------------------------------------------- read probes
  struct PrfRead {
    bool valid = false;
    bool mismatch = false;
    bool never_written = false;
    uint32_t tag = 0;
    uint32_t gen = 0;
    uint64_t data = 0;
  };

  // Read `tag`/`gen` back out of the register file through slot 0. One slot is
  // enough: with a single demand there is no bank conflict, and the response is
  // combinational in the cycle it is offered.
  PrfRead PrfProbe(uint32_t tag, uint32_t gen) {
    SetReadPort(0, tag, gen);
    dut_->eval();
    PrfRead r;
    r.valid = (dut_->prf_rsp_valid_o & 1) != 0;
    r.mismatch = (dut_->prf_rsp_gen_mismatch_o & 1) != 0;
    r.never_written = (dut_->prf_rsp_never_written_o & 1) != 0;
    r.tag = dut_->prf_rsp_tag0_o;
    r.gen = dut_->prf_rsp_gen0_o;
    r.data = dut_->prf_rsp_data0_o;
    ClearReadPorts();
    dut_->eval();
    return r;
  }

  struct StashRead {
    bool valid = false;
    uint64_t value = 0;
  };

  StashRead StashProbe(uint32_t index) {
    dut_->stash_rd0_i = index;
    dut_->eval();
    StashRead r;
    r.valid = dut_->stash_valid0_o;
    r.value = dut_->stash_value0_o;
    dut_->stash_rd0_i = 0;
    dut_->eval();
    return r;
  }

  // Ask the ready-table querys its slots at once. `q_written[s]` is bit s.
  uint32_t QProbe(uint32_t tag0, uint32_t gen0, bool valid0, uint32_t tag1,
                  uint32_t gen1, bool valid1) {
    dut_->q_valid_i = (valid0 ? 1u : 0u) | (valid1 ? 2u : 0u);
    dut_->q_tag0_i = tag0;
    dut_->q_tag1_i = tag1;
    dut_->q_gen0_i = gen0;
    dut_->q_gen1_i = gen1;
    dut_->eval();
    const uint32_t written = dut_->q_written_o;
    dut_->q_valid_i = 0;
    dut_->q_tag0_i = dut_->q_tag1_i = 0;
    dut_->q_gen0_i = dut_->q_gen1_i = 0;
    dut_->eval();
    return written;
  }

 private:
  struct Held {
    bool valid = false;
    Event ev;
  };

  struct Answers {
    RenameModel::Answer ren;
    RobModel::Answer rob;
  };

  // --------------------------------------------------------------- driving
  void SetReadPort(int slot, uint32_t tag, uint32_t gen) {
    ClearReadPorts();
    dut_->prf_rd_valid_i = static_cast<uint8_t>(1u << slot);
    switch (slot) {
      case 0: dut_->prf_rd_tag0_i = tag; dut_->prf_rd_gen0_i = gen; break;
      case 1: dut_->prf_rd_tag1_i = tag; dut_->prf_rd_gen1_i = gen; break;
      case 2: dut_->prf_rd_tag2_i = tag; dut_->prf_rd_gen2_i = gen; break;
      default: dut_->prf_rd_tag3_i = tag; dut_->prf_rd_gen3_i = gen; break;
    }
  }

  void ClearReadPorts() {
    dut_->prf_rd_valid_i = 0;
    dut_->prf_rd_tag0_i = dut_->prf_rd_tag1_i = dut_->prf_rd_tag2_i = dut_->prf_rd_tag3_i = 0;
    dut_->prf_rd_gen0_i = dut_->prf_rd_gen1_i = dut_->prf_rd_gen2_i = dut_->prf_rd_gen3_i = 0;
  }

  void DrivePort(int port, bool valid, const Event& e) {
    switch (port) {
      case 0:
        dut_->wb_valid0_i = valid; dut_->wb0_rob_index_i = e.rob_index;
        dut_->wb0_rob_gen_i = e.rob_gen; dut_->wb0_uop_index_i = e.uop_index;
        dut_->wb0_tag_i = e.tag; dut_->wb0_gen_i = e.gen; dut_->wb0_x0_i = e.x0;
        dut_->wb0_value_valid_i = e.value_valid; dut_->wb0_value_i = e.value;
        dut_->wb0_exc_valid_i = e.exc_valid; dut_->wb0_is_store_i = e.is_store;
        dut_->wb0_is_load_i = e.is_load;
        break;
      case 1:
        dut_->wb_valid1_i = valid; dut_->wb1_rob_index_i = e.rob_index;
        dut_->wb1_rob_gen_i = e.rob_gen; dut_->wb1_uop_index_i = e.uop_index;
        dut_->wb1_tag_i = e.tag; dut_->wb1_gen_i = e.gen; dut_->wb1_x0_i = e.x0;
        dut_->wb1_value_valid_i = e.value_valid; dut_->wb1_value_i = e.value;
        dut_->wb1_exc_valid_i = e.exc_valid; dut_->wb1_is_store_i = e.is_store;
        dut_->wb1_is_load_i = e.is_load;
        break;
      default:
        dut_->wb_valid2_i = valid; dut_->wb2_rob_index_i = e.rob_index;
        dut_->wb2_rob_gen_i = e.rob_gen; dut_->wb2_uop_index_i = e.uop_index;
        dut_->wb2_tag_i = e.tag; dut_->wb2_gen_i = e.gen; dut_->wb2_x0_i = e.x0;
        dut_->wb2_value_valid_i = e.value_valid; dut_->wb2_value_i = e.value;
        dut_->wb2_exc_valid_i = e.exc_valid; dut_->wb2_is_store_i = e.is_store;
        dut_->wb2_is_load_i = e.is_load;
        break;
    }
  }

  void DriveInputs(bool rst) {
    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;

    DrivePort(0, held_[0].valid, held_[0].ev);
    DrivePort(1, held_[1].valid, held_[1].ev);
    DrivePort(2, held_[2].valid, held_[2].ev);

    dut_->ren_wb_accepted_i = 0;
    dut_->ren_wb_stale_i = 0;
    dut_->ren_wb_duplicate_i = 0;
    dut_->rob_cmp_accepted_i = 0;
    dut_->rob_cmp_duplicate_i = 0;
    dut_->rob_cmp_stale_i = 0;
    dut_->rob_cmp_bad_uop_i = 0;

    dut_->q_valid_i = 0;
    dut_->q_tag0_i = dut_->q_tag1_i = 0;
    dut_->q_gen0_i = dut_->q_gen1_i = 0;
    dut_->stash_rd0_i = 0;
    dut_->stash_rd1_i = 0;
    ClearReadPorts();
  }

  void DriveAnswers(const Answers& a) {
    dut_->ren_wb_accepted_i = a.ren.accepted ? 1 : 0;
    dut_->ren_wb_stale_i = a.ren.stale ? 1 : 0;
    dut_->ren_wb_duplicate_i = a.ren.duplicate ? 1 : 0;
    dut_->rob_cmp_accepted_i = a.rob.accepted ? 1 : 0;
    dut_->rob_cmp_duplicate_i = a.rob.duplicate ? 1 : 0;
    dut_->rob_cmp_stale_i = a.rob.stale ? 1 : 0;
    dut_->rob_cmp_bad_uop_i = a.rob.bad_uop ? 1 : 0;
  }

  Answers ComputeAnswers() const {
    Answers a;
    a.ren = rename_.Query(dut_->ren_wb_valid_o != 0, dut_->ren_wb_tag_o, dut_->ren_wb_gen_o);
    a.rob = rob_.Query(dut_->rob_cmp_valid_o != 0, dut_->rob_cmp_index_o,
                       dut_->rob_cmp_gen_o, dut_->rob_cmp_uop_o);
    return a;
  }

  // --------------------------------------------------------------- matching
  const Event* FindInflight(uint32_t index, uint32_t gen, uint32_t uop) const {
    for (const Event& e : inflight_) {
      if (e.rob_index == index && e.rob_gen == gen && e.uop_index == uop) return &e;
    }
    return nullptr;
  }

  bool Ready(int port) const {
    if (port == 0) return dut_->wb_ready0_o != 0;
    if (port == 1) return dut_->wb_ready1_o != 0;
    return dut_->wb_ready2_o != 0;
  }

  // ---------------------------------------------------------------- compare
  void Check(const Answers& ans) {
    const std::string where = At(phase_, clk_->cycle());
    const bool pub = dut_->rob_cmp_valid_o != 0;

    const Event* ev = nullptr;
    if (pub) {
      ev = FindInflight(dut_->rob_cmp_index_o, dut_->rob_cmp_gen_o, dut_->rob_cmp_uop_o);
      Require(ev != nullptr, where,
              "the arbiter published a completion no producer offered: id=" +
                  std::to_string(dut_->rob_cmp_index_o) + ".0x" +
                  mosaic::Hex(dut_->rob_cmp_gen_o, 1) + "." +
                  std::to_string(dut_->rob_cmp_uop_o));
      Require(dut_->rob_cmp_exc_o == (ev->exc_valid ? 1 : 0), where,
              "rob_cmp_exc does not match the offered completion " + Describe(*ev));
    }

    const bool dst_ok = pub && ev->value_valid && !ev->x0;
    const bool rob_live = !ans.rob.stale && !ans.rob.bad_uop;
    const bool write_ok = dst_ok && rob_live && ans.ren.accepted;

    // --- 1. the rename offer -------------------------------------------------
    Require(dut_->ren_wb_valid_o == (dst_ok ? 1 : 0), where,
            "ren_wb_valid: expected " + Bool(dst_ok) + ", got " +
                Bool(dut_->ren_wb_valid_o != 0) + (pub ? " " + Describe(*ev) : ""));
    if (dst_ok) {
      Require(dut_->ren_wb_tag_o == ev->tag, where,
              "ren_wb_tag: expected " + std::to_string(ev->tag) + ", got " +
                  std::to_string(dut_->ren_wb_tag_o) + " " + Describe(*ev));
      Require(dut_->ren_wb_gen_o == Mask(ev->gen, igen_w_), where,
              "ren_wb_gen: expected " + std::to_string(Mask(ev->gen, igen_w_)) +
                  ", got " + std::to_string(dut_->ren_wb_gen_o) + " " + Describe(*ev));
    }
    ++comparisons_;

    // --- 2. never woken before durable --------------------------------------
    // The write port must present exactly the published value, on the tag's home
    // bank, in the very cycle the wakeup is advertised -- and only when rename
    // accepted the completion.
    const uint32_t bank = ev ? (ev->tag % banks_) : 0;
    const uint32_t prf_en = (dut_->prf_wr_en0_o ? 1u : 0u) |
                            (dut_->prf_wr_en1_o ? 2u : 0u) |
                            (dut_->prf_wr_en2_o ? 4u : 0u) |
                            (dut_->prf_wr_en3_o ? 8u : 0u);
    const uint32_t prf_gv = (dut_->prf_wr_gen_valid0_o ? 1u : 0u) |
                            (dut_->prf_wr_gen_valid1_o ? 2u : 0u) |
                            (dut_->prf_wr_gen_valid2_o ? 4u : 0u) |
                            (dut_->prf_wr_gen_valid3_o ? 8u : 0u);
    const uint32_t want_en = write_ok ? (1u << bank) : 0u;

    Require(prf_en == want_en, where,
            "PRF write port: expected mask " + std::to_string(want_en) + ", got " +
                std::to_string(prf_en) + " (write_ok=" + Bool(write_ok) +
                ", accepted=" + Bool(ans.ren.accepted) + ", rob_live=" + Bool(rob_live) +
                ")" + (pub ? " " + Describe(*ev) : ""));
    Require(prf_gv == want_en, where,
            "PRF wr_gen_valid: expected mask " + std::to_string(want_en) + ", got " +
                std::to_string(prf_gv));
    if (write_ok) {
      const uint32_t tag_b = PrfTag(bank), gen_b = PrfGen(bank);
      const uint64_t data_b = PrfData(bank);
      Require(tag_b == ev->tag, where,
              "prf_wr_tag[bank " + std::to_string(bank) + "]: expected " +
                  std::to_string(ev->tag) + ", got " + std::to_string(tag_b) + " " +
                  Describe(*ev));
      Require(gen_b == ev->gen, where,
              "prf_wr_gen[bank " + std::to_string(bank) + "]: expected " +
                  std::to_string(ev->gen) + ", got " + std::to_string(gen_b) + " " +
                  Describe(*ev));
      Require(data_b == ev->value, where,
              "prf_wr_data[bank " + std::to_string(bank) + "]: expected " +
                  mosaic::Hex(ev->value) + ", got " + mosaic::Hex(data_b) + " " +
                  Describe(*ev));
    }

    Require(dut_->wu_valid_o == (write_ok ? 1 : 0), where,
            "wu_valid: expected " + Bool(write_ok) + ", got " +
                Bool(dut_->wu_valid_o != 0) + " (accepted=" + Bool(ans.ren.accepted) +
                ", rob_live=" + Bool(rob_live) + ")" + (pub ? " " + Describe(*ev) : ""));
    if (write_ok) {
      Require(dut_->wu_tag_o == ev->tag && dut_->wu_gen_o == Mask(ev->gen, igen_w_) &&
                  dut_->wu_val_o == ev->value,
              where, "wakeup payload does not match the write: " + Describe(*ev));
    }
    // A wakeup for a completion rename did not accept is the failure this case
    // exists for; state it directly rather than only through write_ok.
    Require(!dut_->wu_valid_o || dut_->ren_wb_accepted_i, where,
            "a wakeup was published for a completion the rename port did not accept" +
                (pub ? " " + Describe(*ev) : ""));
    Require(!dut_->wu_valid_o || dut_->ren_wb_valid_o, where,
            "a wakeup was published without a rename writeback offer");

    // No publication means no holding slot is occupied, so every producer must
    // see `wb_ready`. A producer refused while the arbiter has nothing to
    // publish would deadlock the completion path.
    if (!pub) {
      Require(dut_->wb_ready0_o && dut_->wb_ready1_o && dut_->wb_ready2_o, where,
              "a producer was refused while nothing was published");
    }

    // --- 3. publication and counters ----------------------------------------
    Require(dut_->o_pub_valid_o == (write_ok ? 1 : 0), where,
            "o_pub_valid: expected " + Bool(write_ok) + ", got " +
                Bool(dut_->o_pub_valid_o != 0) + (pub ? " " + Describe(*ev) : ""));
    if (dut_->o_pub_valid_o) {
      Require(dut_->o_pub_index_o == ev->rob_index && dut_->o_pub_gen_o == ev->rob_gen &&
                  dut_->o_pub_value_o == ev->value,
              where, "o_pub_* does not describe the published completion " + Describe(*ev));
      // The ROB is told exactly the identity that is being published.
      Require(dut_->rob_cmp_index_o == ev->rob_index &&
                  dut_->rob_cmp_gen_o == ev->rob_gen &&
                  dut_->rob_cmp_uop_o == ev->uop_index,
              where, "rob_cmp_* does not describe the published completion " + Describe(*ev));
    }

    CheckCounters(where);
  }

  void CheckCounters(const std::string& where) {
    auto eq = [&](const char* name, uint64_t got, uint64_t want) {
      Require(got == want, where, std::string(name) + ": expected " +
                                      std::to_string(want) + ", got " +
                                      std::to_string(got));
    };
    eq("o_wr_ctr", dut_->o_wr_ctr_o, exp_.written);
    eq("o_wake_ctr", dut_->o_wake_ctr_o, exp_.wakeups);
    eq("o_stale_ctr", dut_->o_stale_ctr_o, exp_.stale);
    eq("o_dup_ctr", dut_->o_dup_ctr_o, exp_.duplicate);
    eq("o_rob_stale_ctr", dut_->o_rob_stale_ctr_o, exp_.rob_stale);
    eq("o_rob_dup_ctr", dut_->o_rob_dup_ctr_o, exp_.rob_duplicate);
    eq("o_rob_ok_ctr", dut_->o_rob_ok_ctr_o, exp_.rob_ok);
    eq("o_rob_bad_ctr", dut_->o_rob_bad_ctr_o, exp_.rob_bad);
    eq("o_drop_ctr", dut_->o_drop_ctr_o, exp_.drops);
    eq("o_pub_ctr", dut_->o_pub_ctr_o, exp_.published);
    eq("o_wide_gen_ctr", dut_->o_wide_gen_ctr_o, exp_.wide_gen);
    eq("o_store_ctr", dut_->o_store_ctr_o, exp_.stores);
    eq("o_load_ctr", dut_->o_load_ctr_o, exp_.loads);
    Require(dut_->o_collision_ctr_o >= prev_collision_, where,
            "o_collision_ctr went backwards: " + std::to_string(prev_collision_) +
                " -> " + std::to_string(dut_->o_collision_ctr_o));
    // The collision counter is the one DUT count this driver does not model from
    // its own state -- its value depends on how the arbiter chose to hold the
    // pending set, which is the implementation, not the contract. It is tracked
    // monotonically and its per-phase delta is asserted where the contract names
    // it (two same-bank completions must collide at least once).
    totals_.collisions += dut_->o_collision_ctr_o - prev_collision_;
    prev_collision_ = dut_->o_collision_ctr_o;
    ++comparisons_;
  }

  // ------------------------------------------------------------------ apply
  void Apply(const Answers& ans) {
    const bool pub = dut_->rob_cmp_valid_o != 0;
    const uint32_t pidx = dut_->rob_cmp_index_o;
    const uint32_t pgen = dut_->rob_cmp_gen_o;
    const uint32_t puop = dut_->rob_cmp_uop_o;

    const Event* ev = nullptr;
    if (pub) {
      ev = FindInflight(pidx, pgen, puop);
      if (ev != nullptr) {
        const bool dst_ok = ev->value_valid && !ev->x0;
        const bool rob_live = !ans.rob.stale && !ans.rob.bad_uop;
        const bool write_ok = dst_ok && rob_live && ans.ren.accepted;

        exp_.published++;
        totals_.published++;
        if (ans.rob.stale) { exp_.rob_stale++; totals_.rob_stale++; }
        if (ans.rob.bad_uop) { exp_.rob_bad++; totals_.rob_bad++; }
        if (ans.rob.accepted) { exp_.rob_ok++; totals_.rob_ok++; }
        if (ans.rob.duplicate) { exp_.rob_duplicate++; totals_.rob_duplicate++; }
        if (ev->is_store) { exp_.stores++; totals_.stores++; }
        if (ev->is_load) { exp_.loads++; totals_.loads++; }
        if (dst_ok) {
          if (ans.ren.stale) { exp_.stale++; totals_.stale++; }
          if (ans.ren.duplicate) { exp_.duplicate++; totals_.duplicate++; }
          if (Mask(ev->gen, pgen_w_) >= (1u << igen_w_)) {
            exp_.wide_gen++;
            totals_.wide_gen++;
          }
          if (write_ok) {
            exp_.written++;
            exp_.wakeups++;
            totals_.written++;
            totals_.wakeups++;
            rename_.Commit(ev->tag, Mask(ev->gen, igen_w_));
            prf_expect_.Write(ev->tag, ev->gen, ev->value);
          } else if (!ans.ren.accepted) {
            exp_.drops++;
            totals_.drops++;
          }
        }
        // Remove the published completion from the in-flight set.
        for (auto it = inflight_.begin(); it != inflight_.end(); ++it) {
          if (it->rob_index == pidx && it->rob_gen == pgen && it->uop_index == puop) {
            inflight_.erase(it);
            break;
          }
        }
      }
    }

    // The ROB learns that the published identity is done, if it accepted it.
    rob_.Commit(pub, pidx, pgen, puop, ans.rob.accepted);

    // A producer whose completion the arbiter took no longer holds it.
    for (int i = 0; i < 3; i++) {
      if (held_[i].valid && Ready(i)) {
        inflight_.push_back(held_[i].ev);
        totals_.offered++;
        held_[i] = Held{};
      }
    }
  }

  // ------------------------------------------------------- register-file bits
  uint32_t PrfTag(uint32_t bank) const {
    switch (bank) {
      case 0: return dut_->prf_wr_tag0_o;
      case 1: return dut_->prf_wr_tag1_o;
      case 2: return dut_->prf_wr_tag2_o;
      default: return dut_->prf_wr_tag3_o;
    }
  }
  uint32_t PrfGen(uint32_t bank) const {
    switch (bank) {
      case 0: return dut_->prf_wr_gen0_o;
      case 1: return dut_->prf_wr_gen1_o;
      case 2: return dut_->prf_wr_gen2_o;
      default: return dut_->prf_wr_gen3_o;
    }
  }
  uint64_t PrfData(uint32_t bank) const {
    switch (bank) {
      case 0: return dut_->prf_wr_data0_o;
      case 1: return dut_->prf_wr_data1_o;
      case 2: return dut_->prf_wr_data2_o;
      default: return dut_->prf_wr_data3_o;
    }
  }

  Vmosaic_wb_arbiter_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;

  uint32_t banks_ = 4;
  uint32_t pgen_w_ = 8;
  uint32_t igen_w_ = 7;

  std::string phase_ = "startup";
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;

  Held held_[3];
  std::vector<Event> inflight_;

  RenameModel rename_;
  RobModel rob_;
  PrfExpect prf_expect_;

  Tally exp_;      // expected DUT counter values at the start of this cycle
  Tally totals_;   // accumulated over the whole run
  uint64_t prev_collision_ = 0;
};

// ============================================================================
// the phases
// ============================================================================

// A completion with a live value.
Event ValueEvent(uint32_t index, uint32_t rob_gen, uint32_t uop, uint32_t tag,
                 uint32_t gen, uint64_t value, bool is_load) {
  Event e;
  e.rob_index = index;
  e.rob_gen = rob_gen;
  e.uop_index = uop;
  e.tag = tag;
  e.gen = gen;
  e.x0 = false;
  e.value_valid = true;
  e.value = value;
  e.is_load = is_load;
  return e;
}

// --------------------------------------------------------- reset-state phase
void PhaseResetState(Harness* h) {
  h->Phase("reset-state");
  h->Fresh();

  // One quiet cycle checks the cold presentation directly.
  h->Cycle(/*rst=*/false);

  Require(h->Expected().written == 0 && h->Expected().stale == 0 &&
              h->Expected().duplicate == 0,
          "reset-state", "the tallies are not zero at reset");
}

// ------------------------------------------ 1. same-bank serialisation
void PhaseSameBank(Harness* h) {
  h->Phase("same-bank-many-producers");
  h->Fresh();

  const Tally before = h->Totals();
  const uint64_t coll_before = h->Totals().collisions;

  // Two destinations in the same bank: the home bank is tag % banks and, for p0,
  // 5 % 4 == 9 % 4 == 1.
  const uint32_t tag_a = 5, tag_b = 9;
  const uint32_t gen_a = 1, gen_b = 2;
  h->Rename().Assign(tag_a, gen_a);
  h->Rename().Assign(tag_b, gen_b);
  Require(tag_a % 4 == tag_b % 4, "same-bank", "the probe tags are not in one bank");

  const Event ea = ValueEvent(/*index=*/0, /*rob_gen=*/0, /*uop=*/0, tag_a, gen_a,
                              0xaaaa0000aaaall, /*is_load=*/true);
  const Event eb = ValueEvent(/*index=*/1, /*rob_gen=*/0, /*uop=*/0, tag_b, gen_b,
                              0xbbbb1111bbbbll, /*is_load=*/true);

  h->Offer(0, ea);  // cluster 0
  h->Offer(1, eb);  // cluster 1

  // One cycle accepts both into the arbiter's holding slots, then Drain publishes
  // one per cycle.
  h->Cycle(/*rst=*/false);
  Require(h->Totals().offered - before.offered == 2, "same-bank",
          "both same-cycle completions were not taken: " +
              std::to_string(h->Totals().offered - before.offered) + " of 2");
  h->Drain();

  const Tally after = h->Totals();
  const uint64_t writes = after.written - before.written;
  const uint64_t wakes = after.wakeups - before.wakeups;
  const uint64_t pubs = after.published - before.published;
  const uint64_t stale = after.stale - before.stale;
  const uint64_t dup = after.duplicate - before.duplicate;

  Require(writes == 2, "same-bank",
          "both completions must reach the PRF, saw " + std::to_string(writes));
  Require(after.offered - before.offered == pubs, "same-bank",
          "offered != published: a completion was lost");
  Require(wakes == 2, "same-bank", "expected two value-visible wakeups");
  Require(stale == 0 && dup == 0, "same-bank",
          "the same-bank phase raised stale/duplicate counts");

  // The collision counter must show the delay that the single rename writeback
  // port forces when two completions share a bank.
  const uint64_t collisions = h->Totals().collisions - coll_before;
  std::printf("  [same-bank] offered=%llu writes=%llu wakeups=%llu collisions=%llu\n",
              static_cast<unsigned long long>(2),
              static_cast<unsigned long long>(writes),
              static_cast<unsigned long long>(wakes),
              static_cast<unsigned long long>(collisions));
  Require(collisions >= 1, "same-bank",
          "two same-bank completions produced no collision: o_collision_ctr delta " +
              std::to_string(collisions));

  // Both values must be durable: read them back out of the register file with
  // their own (tag, generation).
  for (const Event& e : {ea, eb}) {
    const Harness::PrfRead r = h->PrfProbe(e.tag, e.gen);
    Require(r.valid && !r.never_written, "same-bank",
            "reading back tag " + std::to_string(e.tag) + " gen " +
                std::to_string(e.gen) + " reports no value");
    Require(!r.mismatch, "same-bank",
            "reading back tag " + std::to_string(e.tag) + " reports a generation "
            "mismatch");
    Require(r.data == e.value, "same-bank",
            "tag " + std::to_string(e.tag) + ": PRF holds " + mosaic::Hex(r.data) +
                ", expected " + mosaic::Hex(e.value));
  }
}

// ------------------------------------------------------------- 3. stale result
void PhaseStale(Harness* h) {
  h->Phase("stale-result");
  h->Fresh();

  const Tally before = h->Totals();
  const uint32_t tag = 20;
  const uint32_t cur_gen = 3;
  h->Rename().Assign(tag, cur_gen);

  // A legitimate write first, so "the PRF content is unchanged" has content to
  // be unchanged from.
  const Event good = ValueEvent(0, 0, 0, tag, cur_gen, 0x1111222233334444ull, true);
  h->Offer(0, good);
  h->Drain();

  const Harness::PrfRead r_good = h->PrfProbe(tag, cur_gen);
  Require(r_good.valid && !r_good.mismatch && r_good.data == good.value, "stale-result",
          "the reference write did not reach the PRF");

  const uint64_t wr_before = h->Totals().written;
  const uint64_t stale_before = h->Totals().stale;
  const uint64_t pubs_before = h->Totals().published;

  // Now a completion for the same tag at a *previous* generation. Rename answers
  // stale; nothing may change.
  const Event stale = ValueEvent(1, 0, 0, tag, cur_gen - 1, 0xdeadbeefdeadbeefull, true);
  h->Offer(0, stale);
  h->Drain();

  Require(h->Totals().stale - stale_before == 1, "stale-result",
          "o_stale_ctr did not advance by exactly one: " +
              std::to_string(h->Totals().stale - stale_before));
  Require(h->Totals().written - wr_before == 0, "stale-result",
          "a stale completion incremented o_wr_ctr");
  Require(h->Totals().published - pubs_before == 1, "stale-result",
          "a stale completion was not offered to the rename/ROB ports");

  // The register file is bit-for-bit what it was.
  const Harness::PrfRead r_after = h->PrfProbe(tag, cur_gen);
  Require(r_after.valid && !r_after.mismatch && r_after.data == good.value &&
              r_after.gen == cur_gen,
          "stale-result", "a stale completion changed the PRF: now holds " +
                              mosaic::Hex(r_after.data));
  // And the stale generation itself was never stored.
  const Harness::PrfRead r_stale = h->PrfProbe(tag, cur_gen - 1);
  Require(r_stale.mismatch || r_stale.never_written, "stale-result",
          "the stale generation appears as a live PRF entry");

  std::printf("  [stale] stale=%llu writes=%llu\n",
              static_cast<unsigned long long>(h->Totals().stale - before.stale),
              static_cast<unsigned long long>(h->Totals().written - before.written));
}

// ------------------------------------------------------------ 4. duplicate
void PhaseDuplicate(Harness* h) {
  h->Phase("duplicate-producer");
  h->Fresh();

  const Tally before = h->Totals();
  const uint32_t tag = 40;
  const uint32_t gen = 5;
  h->Rename().Assign(tag, gen);

  const Event first = ValueEvent(0, 0, 0, tag, gen, 0x0f0f0f0f0f0f0f0full, true);
  h->Offer(0, first);
  h->Drain();

  const uint64_t wr_after_first = h->Totals().written;
  Require(wr_after_first - before.written == 1, "duplicate-producer",
          "the first producer did not write exactly once");

  // The same (tag, generation) arrives a second time, on the other cluster, with
  // a different value. Rename answers duplicate.
  Event second = first;
  second.value = 0xf0f0f0f0f0f0f0f0ull;
  h->Offer(1, second);
  h->Drain();

  Require(h->Totals().duplicate - before.duplicate == 1, "duplicate-producer",
          "o_dup_ctr did not advance by exactly one: " +
              std::to_string(h->Totals().duplicate - before.duplicate));
  Require(h->Totals().written - before.written == 1, "duplicate-producer",
          "the duplicate incremented o_wr_ctr: writes=" +
              std::to_string(h->Totals().written - before.written));

  // The register file still holds the first value.
  const Harness::PrfRead r = h->PrfProbe(tag, gen);
  Require(r.valid && !r.mismatch && r.data == first.value, "duplicate-producer",
          "the duplicate overwrote the first value: holds " + mosaic::Hex(r.data));

  std::printf("  [duplicate] dup=%llu writes=%llu\n",
              static_cast<unsigned long long>(h->Totals().duplicate - before.duplicate),
              static_cast<unsigned long long>(h->Totals().written - before.written));
}

// --------------------------------------------------- corroboration: dead macro
void PhaseRobStale(Harness* h) {
  h->Phase("rob-dead-identity");
  h->Fresh();

  const Tally before = h->Totals();
  const uint32_t tag = 7, gen = 2;
  h->Rename().Assign(tag, gen);

  const Event e = ValueEvent(3, 1, 0, tag, gen, 0xcafebabecafebabeull, true);
  h->Offer(0, e);
  h->Cycle(/*rst=*/false);  // the arbiter takes it
  Require(h->Totals().offered - before.offered == 1, "rob-dead-identity",
          "the completion was not taken");
  // The macro is discarded before its value would be published.
  h->Rob().DeclareDead(e);
  h->Drain();

  Require(h->Totals().rob_stale - before.rob_stale == 1, "rob-dead-identity",
          "o_rob_stale_ctr did not advance: " +
              std::to_string(h->Totals().rob_stale - before.rob_stale));
  Require(h->Totals().written - before.written == 0, "rob-dead-identity",
          "a completion of a discarded macro reached the register file");
  const Harness::PrfRead r = h->PrfProbe(tag, gen);
  Require(!r.valid || r.never_written, "rob-dead-identity",
          "a discarded macro's completion is in the register file");

  std::printf("  [rob-dead] rob_stale=%llu writes=%llu\n",
              static_cast<unsigned long long>(h->Totals().rob_stale - before.rob_stale),
              static_cast<unsigned long long>(h->Totals().written - before.written));
}

// ------------------------------------------- corroboration: no destination
void PhaseNoDestination(Harness* h) {
  h->Phase("no-destination-completion");
  h->Fresh();

  const Tally before = h->Totals();

  // A store: it owes the ROB a completion but carries no register value. The
  // rename port must never be offered it, and no wakeup may be published.
  Event e;
  e.rob_index = 9;
  e.rob_gen = 2;
  e.uop_index = 0;
  e.tag = 0;
  e.gen = 0;
  e.x0 = false;
  e.value_valid = false;
  e.value = 0;
  e.is_store = true;
  h->Offer(2, e);  // the shared MUL/DIV producer is unused otherwise
  h->Drain();

  Require(h->Totals().published - before.published == 1, "no-destination",
          "a store completion was not published to the ROB");
  Require(h->Totals().stores - before.stores == 1, "no-destination",
          "o_store_ctr did not advance");
  Require(h->Totals().written - before.written == 0, "no-destination",
          "a completion with no destination wrote the register file");
  Require(h->Totals().wakeups - before.wakeups == 0, "no-destination",
          "a completion with no destination published a wakeup");
}

// ------------------------------------------------------- ready-table query
void PhaseReadyTable(Harness* h) {
  h->Phase("ready-table");
  h->Fresh();

  const Tally before = h->Totals();
  const uint32_t tag = 60, gen = 4;
  h->Rename().Assign(tag, gen);
  const Event e = ValueEvent(0, 0, 0, tag, gen, 0x123456789abcdef0ull, true);
  h->Offer(0, e);
  h->Drain();

  Require(h->Totals().written - before.written == 1, "ready-table",
          "the write did not happen");

  // Slot 0 asks the written (tag, generation), slot 1 the same tag at the next
  // generation -- which the tag has never held a value for.
  const uint32_t written_a = h->QProbe(tag, gen, true, tag, gen + 1, true);
  Require((written_a & 1u) != 0, "ready-table",
          "q_written is 0 for the written (tag, generation)");
  Require((written_a & 2u) == 0, "ready-table",
          "q_written is 1 for the same tag at a different generation");

  // Slot 0 asks an unwritten tag, slot 1 is not offered at all.
  const uint32_t written_b = h->QProbe(63, gen, true, tag, gen, false);
  Require((written_b & 1u) == 0, "ready-table", "q_written is 1 for an unwritten tag");
  Require((written_b & 2u) == 0, "ready-table",
          "q_written is 1 for a query slot that was not valid");

  std::printf("  [ready-table] q_written=%u%u / %u%u\n", (written_a >> 1) & 1,
              written_a & 1, (written_b >> 1) & 1, written_b & 1);
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
  Vmosaic_wb_arbiter_tb dut;

  std::string detail;
  bool passed = true;
  try {
    // The geometry outputs are constants derived from the instance parameters, so
    // one evaluation is enough to read them and no reset is needed first.
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    const uint32_t banks = dut.o_banks_o;
    const uint32_t prf_entries = dut.o_prf_entries_o;
    const uint32_t tag_w = dut.o_prf_tag_w_o;
    const uint32_t pgen_w = dut.o_prf_gen_w_o;
    const uint32_t igen_w = dut.o_igen_w_o;
    const uint32_t xlen = dut.o_xlen_o;
    const uint32_t rob_entries = dut.o_rob_entries_o;
    const uint32_t rob_index_w = dut.o_rob_index_w_o;
    const uint32_t rob_gen_w = dut.o_rob_gen_w_o;
    const uint32_t uop_w = dut.o_uop_w_o;

    // The same-bank mechanism only exists with at least two banks, and the case's
    // probe tags assume four; a profile change must fail loudly here rather than
    // quietly test a different claim.
    Require(banks == 4, "geometry",
            "this case proves same-bank serialisation for the 4-bank profile, the "
            "DUT reports " + std::to_string(banks) + " banks");
    Require(xlen == 64, "geometry",
            "expected a 64-bit datapath, the DUT reports " + std::to_string(xlen));
    Require(tag_w > 0 && tag_w <= 32 && pgen_w > 0 && pgen_w <= 32 && igen_w > 0 &&
                igen_w <= 32,
            "geometry", "a driver-facing identity field is wider than the interface");
    Require(prf_entries > 0 && prf_entries <= (1u << tag_w), "geometry",
            "the PRF entry count does not fit the tag width");
    // The rename/ready-table generation is the tag-allocation generation, i.e.
    // clog2(entries). Asserting the relationship keeps this honest under a
    // geometry change.
    uint32_t clog = 1;
    while ((1u << clog) < prf_entries) clog++;
    Require(igen_w == clog, "geometry",
            "the rename generation must be clog2(int_prf_entries) = " +
                std::to_string(clog) + ", the DUT reports " + std::to_string(igen_w));
    Require(rob_entries == (1u << rob_index_w), "geometry",
            "the ROB index does not span the entry count");
    Require(rob_gen_w >= rob_index_w && uop_w > 0, "geometry",
            "a ROB identity field is too narrow");

    Harness harness(&dut, &clk, options.max_cycles);
    harness.Configure(banks, pgen_w, igen_w);

    harness.Phase("reset-state");
    PhaseResetState(&harness);

    harness.Phase("same-bank-many-producers");
    PhaseSameBank(&harness);

    harness.Phase("stale-result");
    PhaseStale(&harness);

    harness.Phase("duplicate-producer");
    PhaseDuplicate(&harness);

    harness.Phase("rob-dead-identity");
    PhaseRobStale(&harness);

    harness.Phase("no-destination-completion");
    PhaseNoDestination(&harness);

    harness.Phase("ready-table");
    PhaseReadyTable(&harness);

    const Tally& t = harness.Totals();
    detail = "completion path holds: " + std::to_string(harness.comparisons()) +
             " cycle comparisons over " + std::to_string(harness.cycles()) +
             " cycles; offered=" + std::to_string(t.offered) +
             " published=" + std::to_string(t.published) +
             " writes=" + std::to_string(t.written) +
             " wakeups=" + std::to_string(t.wakeups) +
             " stale=" + std::to_string(t.stale) +
             " duplicate=" + std::to_string(t.duplicate) +
             " rob_stale=" + std::to_string(t.rob_stale) +
             " collisions=" + std::to_string(t.collisions) + " seed " +
             std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
