// ============================================================================
// tb_rob.cpp -- CASE=rob.out_of_order_children, work package I-016.
//
// The DUT is never its own oracle. Every output is compared, on every cycle of
// every phase, against an independent C++ shadow written from the *contract*
// documented in rtl/core/mosaic_rob.sv -- the descriptor/bitmap rule, the
// generation rule, the flush rule -- and not from the RTL's structure. The
// shadow shares no code with the RTL and never looks at Verilator internals, so
// agreeing with it is evidence about the contract rather than a restatement of
// the implementation.
//
// Geometry is read from the elaborated DUT (`o_rob_entries`, `o_index_w`,
// `o_max_uops`, `o_id_w`, `o_pc_w`, `o_num_uops_w`), which are constants
// derived from the instance parameters, so this file contains no depth of its
// own: a profile with a different ROB sizes the shadow on the next run, and
// there is no number here to forget to update.
//
// Phases, each of which can fail on its own:
//
//   1. reset-state       the documented cold state straight after reset.
//   2. out-of-order      a 3-child macro completing 2, 0, 1 and a max-child macro
//                        completing in strict reverse: not complete until the last
//                        child, and the done count never passes the child count.
//                        This is the card's "last-uop-arrives-is-complete" failure
//                        in the exact shape the card describes it.
//   3. duplicate         the same child three times: reported every time, counted
//                        once, descriptor progress unchanged.
//   4. wrap-generation   more than ROB_ENTRIES allocations with every slot
//                        demonstrably recycled, then a completion aimed at a
//                        recycled slot rejected. The wrap is *observed* -- the
//                        occupant's generation and tag are checked to have
//                        changed -- and never assumed.
//   5. full              allocation into a full ROB refused and reported, with
//                        every slot proven undisturbed, then a strictly in-order
//                        drain of the whole buffer.
//   6. flush             committed history untouched, free count restored exactly,
//                        generation counter not rewound, pre-flush completions
//                        still rejected afterwards.
//   7. exception         an exceptional macro blocks the head and is reported for
//                        replay, whether the exception arrived at dispatch or at a
//                        child completion.
//   8. close-gate        an open macro accumulates children but cannot retire
//                        until it is closed; a stale close is refused.
//   9. full-depth        all ROB_ENTRIES slots live at once, kept full while the
//                        buffer wraps repeatedly, every slot swept on every cycle
//                        and the retirement order checked against allocation
//                        order.
//  10. random-soak       random stimulus compared against the shadow on every
//                        cycle, including deliberate stale, duplicate and
//                        out-of-range completions.
//
// Standing invariants are checked on every cycle of every phase rather than in
// one place: the conservation identity, occupancy against the entry count, the
// generation counter against the allocation count, mutual exclusion of the four
// completion reports, and the progress bound `done count <= child count`.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_rob_tb.h"

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

uint32_t Mask(uint32_t value, uint32_t width) {
  return (width >= 32) ? value : (value & ((1u << width) - 1u));
}

uint64_t Mask64(uint64_t value, uint32_t width) {
  return (width >= 64) ? value : (value & ((1ull << width) - 1ull));
}

uint32_t PopCount(uint32_t value) {
  uint32_t count = 0;
  while (value) {
    count += value & 1u;
    value >>= 1;
  }
  return count;
}

uint32_t Clog2(uint32_t value) {
  uint32_t width = 1;
  while ((1u << width) < value) width++;
  return width;
}

// --------------------------------------------------------------- the stimulus
struct Stim {
  bool alloc_valid = false;
  uint32_t alloc_tag = 0;
  uint64_t alloc_pc = 0;
  uint32_t alloc_num_uops = 1;
  bool alloc_exc = false;
  bool alloc_open = false;

  bool close_valid = false;
  uint32_t close_index = 0;
  uint32_t close_gen = 0;

  bool cmp_valid = false;
  uint32_t cmp_index = 0;
  uint32_t cmp_gen = 0;
  uint32_t cmp_uop = 0;
  bool cmp_exc = false;

  bool retire_req = false;
  bool flush = false;
  uint32_t obs_index = 0;
};

std::string Describe(const Stim& s) {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "[alloc v=%d tag=%u pc=%s n=%u exc=%d open=%d | close v=%d "
                "i=%u g=0x%x | cmp v=%d i=%u g=0x%x uop=%u exc=%d | retire=%d "
                "flush=%d obs=%u]",
                s.alloc_valid, s.alloc_tag, mosaic::Hex(s.alloc_pc).c_str(),
                s.alloc_num_uops, s.alloc_exc, s.alloc_open, s.close_valid,
                s.close_index, s.close_gen, s.cmp_valid, s.cmp_index, s.cmp_gen,
                s.cmp_uop, s.cmp_exc, s.retire_req, s.flush, s.obs_index);
  return buf;
}

// ------------------------------------------------------------------ the shadow
//
// An independent model of the documented contract. Every field has a
// counterpart in the header prose; none of them was obtained by reading RTL.
//
// The one simplification is stated rather than hidden: the shadow assumes the
// entry count fills the slot-index space, i.e. it models the elaboration in
// which no index can name a slot that does not exist. That is true for every
// profile in config/geometry/ today, and the geometry check at start-up fails
// loudly rather than letting the shadow quietly model the wrong thing.
struct Slot {
  bool valid = false;
  uint32_t gen = 0;
  uint32_t tag = 0;
  uint64_t pc = 0;
  uint32_t num_uops = 0;
  uint32_t done_mask = 0;
  bool exc = false;
  bool closed = false;
};

class ShadowRob {
 public:
  // Everything the DUT presents combinationally: the documented rules applied to
  // the *pre-edge* state and this cycle's ports.
  struct View {
    bool alloc_ok = false;
    bool alloc_refused = false;
    bool alloc_full = false;
    bool alloc_bad_uops = false;
    uint32_t alloc_index = 0;
    uint32_t alloc_gen = 0;

    bool close_ok = false;
    bool close_stale = false;

    bool cmp_accepted = false;
    bool cmp_duplicate = false;
    bool cmp_stale = false;
    bool cmp_bad_uop = false;

    bool retire_ack = false;
    bool head_valid = false;
    bool head_ready = false;
    bool head_replay = false;
    bool head_complete = false;
    bool head_exc = false;
    bool head_closed = false;
    uint32_t head_index = 0;
    uint32_t head_gen = 0;
    uint32_t head_tag = 0;
    uint64_t head_pc = 0;
    uint32_t head_num_uops = 0;
    uint32_t head_done_mask = 0;
    uint32_t head_done_cnt = 0;

    bool obs_valid = false;
    uint32_t obs_gen = 0;
    uint32_t obs_tag = 0;
    uint64_t obs_pc = 0;
    uint32_t obs_num_uops = 0;
    uint32_t obs_done_mask = 0;
    uint32_t obs_done_cnt = 0;
    bool obs_exc = false;
    bool obs_closed = false;

    uint32_t head_ptr = 0;
    uint32_t alloc_ptr = 0;
    uint32_t occupied = 0;
    uint32_t free_slots = 0;
    uint32_t alloc_total = 0;
    uint32_t retired_total = 0;
    uint32_t squashed_total = 0;
    uint32_t gen_counter = 0;
  };

  ShadowRob(uint32_t entries, uint32_t max_uops, uint32_t index_w, uint32_t id_w,
            uint32_t pc_w, uint32_t num_uops_w)
      : entries_(entries),
        max_uops_(max_uops),
        index_w_(index_w),
        id_w_(id_w),
        pc_w_(pc_w),
        num_uops_w_(num_uops_w),
        uop_w_(Clog2(max_uops)),
        gen_mask_(Mask(~0u, id_w)),
        slots_(entries),
        slot_allocs_(entries, 0),
        slot_first_gen_(entries, 0) {}

  // --------------------------------------------------------------- accessors
  uint32_t entries() const { return entries_; }
  uint32_t max_uops() const { return max_uops_; }
  uint32_t occupied() const { return occupied_; }
  uint32_t alloc_ptr() const { return alloc_; }
  uint32_t retired_total() const { return retired_; }

  const Slot& At(uint32_t index) const { return slots_[index]; }

  // How many times this slot has been allocated to since reset.
  uint32_t SlotAllocations(uint32_t index) const { return slot_allocs_[index]; }

  // The generation the *first* allocation to this slot carried. A completion
  // naming it, for a slot that is live again, is exactly the stale arrival the
  // card names.
  uint32_t SlotFirstGen(uint32_t index) const { return slot_first_gen_[index]; }

  // Every live slot, oldest first.
  std::vector<uint32_t> LiveIndices() const {
    std::vector<uint32_t> live;
    for (uint32_t i = 0; i < entries_; i++) {
      if (slots_[i].valid) live.push_back(i);
    }
    std::sort(live.begin(), live.end(),
              [&](uint32_t a, uint32_t b) { return Age(a) < Age(b); });
    return live;
  }

  void Reset() {
    for (Slot& slot : slots_) slot = Slot();
    std::fill(slot_allocs_.begin(), slot_allocs_.end(), 0);
    std::fill(slot_first_gen_.begin(), slot_first_gen_.end(), 0);
    head_ = 0;
    alloc_ = 0;
    occupied_ = 0;
    alloc_total_ = 0;
    retired_ = 0;
    squashed_ = 0;
    gen_counter_ = 0;
  }

  // The children a descriptor with `n` children is waiting for: the low n bits.
  uint32_t ExpectedMask(uint32_t n) const {
    if (n == 0 || n > max_uops_) return 0;
    const uint32_t all = (max_uops_ >= 32) ? ~0u : ((1u << max_uops_) - 1u);
    return all >> (max_uops_ - n);
  }

  View Peek(const Stim& s) const {
    View v;
    Decode(s, &v);

    v.alloc_index = alloc_;
    v.alloc_gen = gen_counter_ & gen_mask_;

    const Slot& h = slots_[head_];
    const uint32_t raw = h.done_mask & ExpectedMask(h.num_uops);
    v.head_valid = h.valid;
    v.head_index = head_;
    v.head_gen = h.valid ? h.gen : 0;
    v.head_tag = h.valid ? h.tag : 0;
    v.head_pc = h.valid ? h.pc : 0;
    v.head_num_uops = h.valid ? h.num_uops : 0;
    v.head_done_mask = h.valid ? raw : 0;
    v.head_done_cnt = PopCount(v.head_done_mask);
    v.head_complete = h.valid && (raw == ExpectedMask(h.num_uops));
    v.head_exc = h.valid && h.exc;
    v.head_closed = h.valid && h.closed;
    v.head_replay = h.valid && h.exc;
    v.head_ready = h.valid && v.head_complete && !v.head_exc && v.head_closed;

    const Slot& o = slots_[Mask(s.obs_index, index_w_)];
    v.obs_valid = o.valid;
    v.obs_gen = o.valid ? o.gen : 0;
    v.obs_tag = o.valid ? o.tag : 0;
    v.obs_pc = o.valid ? o.pc : 0;
    v.obs_num_uops = o.valid ? o.num_uops : 0;
    v.obs_done_mask = o.valid ? (o.done_mask & ExpectedMask(o.num_uops)) : 0;
    v.obs_done_cnt = PopCount(v.obs_done_mask);
    v.obs_exc = o.valid && o.exc;
    v.obs_closed = o.valid && o.closed;

    v.head_ptr = head_;
    v.alloc_ptr = alloc_;
    v.occupied = occupied_;
    v.free_slots = entries_ - occupied_;
    v.alloc_total = alloc_total_;
    v.retired_total = retired_;
    v.squashed_total = squashed_;
    v.gen_counter = gen_counter_;
    return v;
  }

  // Apply the edge. `Decode` is recomputed from the same pre-edge state `Peek`
  // used, so the two can never disagree about which events occurred.
  void Apply(const Stim& s) {
    View v;
    Decode(s, &v);

    if (s.flush) {
      // A flush has priority over every other event offered in the same cycle:
      // the macro was speculative, and the flush decides its fate.
      squashed_ += occupied_;
      for (Slot& slot : slots_) slot.valid = false;
      head_ = alloc_;
      occupied_ = 0;
      return;
    }

    if (v.alloc_ok) {
      Slot& slot = slots_[alloc_];
      slot.valid = true;
      slot.gen = gen_counter_ & gen_mask_;
      slot.tag = Mask(s.alloc_tag, id_w_);
      slot.pc = Mask64(s.alloc_pc, pc_w_);
      slot.num_uops = Mask(s.alloc_num_uops, num_uops_w_);
      slot.done_mask = 0;
      slot.exc = s.alloc_exc;
      slot.closed = !s.alloc_open;
      if (slot_allocs_[alloc_] == 0) slot_first_gen_[alloc_] = slot.gen;
      slot_allocs_[alloc_]++;
      alloc_ = Next(alloc_);
      occupied_++;
      alloc_total_++;
      gen_counter_++;
    }

    if (v.close_ok) slots_[Mask(s.close_index, index_w_)].closed = true;

    if (v.cmp_accepted) {
      Slot& slot = slots_[Mask(s.cmp_index, index_w_)];
      slot.done_mask |= 1u << Mask(s.cmp_uop, uop_w_);
      if (s.cmp_exc) slot.exc = true;
    }

    if (v.retire_ack) {
      slots_[head_].valid = false;
      head_ = Next(head_);
      occupied_--;
      retired_++;
    }
  }

 private:
  // The allocation decision, the four completion outcomes and the head's
  // retirement eligibility, all from the pre-edge state.
  void Decode(const Stim& s, View* v) const {
    const bool full = (occupied_ == entries_);
    const uint32_t n = Mask(s.alloc_num_uops, num_uops_w_);
    const bool bad = (n == 0) || (n > max_uops_);

    v->alloc_full = full;
    v->alloc_bad_uops = bad;
    v->alloc_ok = s.alloc_valid && !full && !bad && !s.flush;
    v->alloc_refused = s.alloc_valid && !s.flush && (full || bad);

    const uint32_t ci = Mask(s.close_index, index_w_);
    const bool close_ident =
        slots_[ci].valid && slots_[ci].gen == Mask(s.close_gen, id_w_);
    v->close_ok = s.close_valid && !s.flush && close_ident;
    v->close_stale = s.close_valid && !s.flush && !close_ident;

    const uint32_t xi = Mask(s.cmp_index, index_w_);
    const Slot& slot = slots_[xi];
    const bool live = slot.valid;
    const bool ident = live && slot.gen == Mask(s.cmp_gen, id_w_);
    const uint32_t uop = Mask(s.cmp_uop, uop_w_);
    const bool in_range = (uop < slot.num_uops);
    const bool already = ((slot.done_mask >> uop) & 1u) != 0;

    v->cmp_accepted = s.cmp_valid && !s.flush && ident && in_range && !already;
    v->cmp_duplicate = s.cmp_valid && !s.flush && ident && in_range && already;
    v->cmp_stale = s.cmp_valid && !s.flush && !ident;
    v->cmp_bad_uop = s.cmp_valid && !s.flush && ident && !in_range;

    const Slot& h = slots_[head_];
    const uint32_t raw = h.done_mask & ExpectedMask(h.num_uops);
    const bool complete = h.valid && (raw == ExpectedMask(h.num_uops));
    const bool ready = h.valid && complete && !h.exc && h.closed;
    v->retire_ack = s.retire_req && ready && !s.flush;
  }

  uint32_t Next(uint32_t index) const {
    return (index + 1 == entries_) ? 0u : index + 1;
  }

  // How far `index` sits above the head, i.e. its age in the queue.
  uint32_t Age(uint32_t index) const { return (index + entries_ - head_) % entries_; }

  uint32_t entries_;
  uint32_t max_uops_;
  uint32_t index_w_;
  uint32_t id_w_;
  uint32_t pc_w_;
  uint32_t num_uops_w_;
  uint32_t uop_w_;
  uint32_t gen_mask_;
  std::vector<Slot> slots_;
  std::vector<uint32_t> slot_allocs_;
  std::vector<uint32_t> slot_first_gen_;
  uint32_t head_ = 0;
  uint32_t alloc_ = 0;
  uint32_t occupied_ = 0;
  uint32_t alloc_total_ = 0;
  uint32_t retired_ = 0;
  uint32_t squashed_ = 0;
  uint32_t gen_counter_ = 0;
};

// The state the DUT is in *after* the last rising edge.
//
// The live ports are combinational: during a cycle they describe the edge that
// has not happened yet, which is what `Compare` checks. A phase that wants to
// assert on what the ROB holds *now* -- "after this completion, is the macro
// complete?" -- has to read the settled state instead, or it would be asserting
// on the cycle before the one it just drove. The two are different questions and
// mixing them silently weakens a check, so they are read from different places:
// event reports (cmp_accepted, retire_ack) come from the live ports, state comes
// from this snapshot.
struct Settled {
  bool head_valid = false;
  bool head_ready = false;
  bool head_complete = false;
  bool head_exc = false;
  bool head_closed = false;
  bool head_replay = false;
  uint32_t head_tag = 0;
  uint32_t head_num_uops = 0;
  uint32_t head_done_mask = 0;
  uint32_t head_done_cnt = 0;
  uint32_t head_ptr = 0;
  uint32_t alloc_ptr = 0;
  uint32_t occupied = 0;
  uint32_t free_slots = 0;
  uint32_t alloc_total = 0;
  uint32_t retired_total = 0;
  uint32_t squashed_total = 0;
  uint32_t gen_counter = 0;
  uint32_t alloc_index = 0;
  uint32_t alloc_gen = 0;
};

// The outcome of the edge the DUT is about to take.
//
// These are combinational from the pre-edge state and describe the event in
// flight, so they are captured with the clock still low -- reading them after
// the edge would read the *next* cycle's answer to the next cycle's question,
// which is a different value (a retirement clears the head, so `retire_ack`
// falls again the moment it is taken).
struct Reports {
  bool alloc_ok = false;
  bool alloc_refused = false;
  bool alloc_full = false;
  bool alloc_bad_uops = false;
  bool close_ok = false;
  bool close_stale = false;
  bool cmp_accepted = false;
  bool cmp_duplicate = false;
  bool cmp_stale = false;
  bool cmp_bad_uop = false;
  bool retire_ack = false;
};

// ---------------------------------------------------------------- the harness
//
// Owns the clock, the reset schedule, the comparison against the shadow and the
// standing invariants.
class Harness {
 public:
  // The identity a completion has to name: the slot *and* the generation the ROB
  // handed out when the macro was allocated.
  struct Identity {
    uint32_t index = 0;
    uint32_t gen = 0;
    uint32_t tag = 0;
  };

  // What the soak actually exercised. A random campaign that never reached
  // capacity, never wrapped and never produced a duplicate would pass while
  // testing nothing, so the phase asserts these are non-zero.
  struct Counters {
    uint32_t flushes = 0;
    uint32_t accepted = 0;
    uint32_t duplicates = 0;
    uint32_t stale_completions = 0;
    uint32_t bad_uops = 0;
    uint32_t full_refusals = 0;
    uint32_t exceptional = 0;
  };

  Harness(Vmosaic_rob_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles,
          ShadowRob* shadow)
      : dut_(dut),
        clk_(clk),
        max_cycles_(max_cycles),
        shadow_(shadow),
        entries_(shadow->entries()),
        max_uops_(shadow->max_uops()) {}

  void Phase(const std::string& name) { phase_ = name; }

  // A freshly reset DUT *and* a freshly reset shadow, so no phase can pass on
  // state left behind by the one before it.
  void Fresh(int cycles = 4) {
    for (int i = 0; i < cycles; i++) Cycle(Stim{}, /*rst=*/true, /*sweep=*/false);
    shadow_->Reset();
    have_prev_ = false;
    counters_ = Counters();
  }

  // One full clock period. When `sweep` is set, every slot of the ROB is
  // compared through the observation port, not only the selected one.
  void Cycle(const Stim& s, bool rst = false, bool sweep = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + std::to_string(max_cycles_) +
                       ") exhausted before the phase finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->alloc_valid_i = s.alloc_valid;
    dut_->alloc_tag_i = s.alloc_tag;
    dut_->alloc_pc_i = s.alloc_pc;
    dut_->alloc_num_uops_i = s.alloc_num_uops;
    dut_->alloc_exc_i = s.alloc_exc;
    dut_->alloc_open_i = s.alloc_open;
    dut_->close_valid_i = s.close_valid;
    dut_->close_index_i = s.close_index;
    dut_->close_gen_i = s.close_gen;
    dut_->cmp_valid_i = s.cmp_valid;
    dut_->cmp_index_i = s.cmp_index;
    dut_->cmp_gen_i = s.cmp_gen;
    dut_->cmp_uop_i = s.cmp_uop;
    dut_->cmp_exc_i = s.cmp_exc;
    dut_->retire_req_i = s.retire_req;
    dut_->flush_valid_i = s.flush;
    dut_->obs_index_i = s.obs_index;
    dut_->eval();

    if (!rst) {
      Compare(s);
      CaptureReports();
      if (sweep) SweepSlots(s);
      shadow_->Apply(s);
      Tally(s);
    }

    dut_->clk = 1;
    dut_->eval();
    Capture();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  // ------------------------------------------------------- stimulus helpers
  Identity Alloc(uint32_t tag, uint32_t num_uops, bool exc = false, bool open = false) {
    // The identity this allocation will be given is the settled state *before*
    // the cycle, not after it: `alloc_index` and `alloc_gen` are combinational
    // from the allocation pointer, and that pointer has already moved by the
    // time the edge has been applied. Reading them afterwards would name the
    // next macro's slot, and every completion aimed at it would be stale.
    const uint32_t index = settled_.alloc_index;
    const uint32_t gen = settled_.alloc_gen;

    Stim s;
    s.alloc_valid = true;
    s.alloc_tag = tag;
    s.alloc_pc = 0x80000000ull + 4ull * tag;
    s.alloc_num_uops = num_uops;
    s.alloc_exc = exc;
    s.alloc_open = open;
    Cycle(s);

    Identity id;
    id.index = index;
    id.gen = gen;
    id.tag = tag;
    return id;
  }

  void Complete(uint32_t index, uint32_t gen, uint32_t uop, bool exc = false) {
    Stim s;
    s.cmp_valid = true;
    s.cmp_index = index;
    s.cmp_gen = gen;
    s.cmp_uop = uop;
    s.cmp_exc = exc;
    Cycle(s);
  }

  void Close(uint32_t index, uint32_t gen) {
    Stim s;
    s.close_valid = true;
    s.close_index = index;
    s.close_gen = gen;
    Cycle(s);
  }

  // One cycle asking to retire. Returns whether the ROB accepted.
  bool Retire() {
    Stim s;
    s.retire_req = true;
    Cycle(s);
    return report_.retire_ack;
  }

  void Flush() {
    Stim s;
    s.flush = true;
    Cycle(s);
  }

  // Read one slot back through the observation port without advancing time.
  void Observe(uint32_t index) {
    dut_->obs_index_i = index;
    dut_->eval();
    CompareObs(index, At(phase_, clk_->cycle()));
  }

  // ---------------------------------------------------------- observed state
  uint32_t ObservedOccupied() const { return settled_.occupied; }
  uint32_t ObservedFree() const { return settled_.free_slots; }
  uint32_t ObservedAllocTotal() const { return settled_.alloc_total; }
  uint32_t ObservedRetired() const { return settled_.retired_total; }
  uint32_t ObservedSquashed() const { return settled_.squashed_total; }
  uint32_t ObservedGenCounter() const { return settled_.gen_counter; }
  uint32_t ObservedAllocIndex() const { return settled_.alloc_index; }
  uint32_t ObservedAllocGen() const { return settled_.alloc_gen; }
  uint32_t ObservedHeadPtr() const { return settled_.head_ptr; }
  uint32_t ObservedAllocPtr() const { return settled_.alloc_ptr; }

  bool AllocOk() const { return report_.alloc_ok; }
  bool AllocRefused() const { return report_.alloc_refused; }
  bool AllocFull() const { return report_.alloc_full; }
  bool AllocBadUops() const { return report_.alloc_bad_uops; }
  bool CloseOk() const { return report_.close_ok; }
  bool CloseStale() const { return report_.close_stale; }
  bool CmpAccepted() const { return report_.cmp_accepted; }
  bool CmpDuplicate() const { return report_.cmp_duplicate; }
  bool CmpStale() const { return report_.cmp_stale; }
  bool CmpBadUop() const { return report_.cmp_bad_uop; }
  bool RetireAck() const { return report_.retire_ack; }
  bool HeadValid() const { return settled_.head_valid; }
  bool HeadReady() const { return settled_.head_ready; }
  bool HeadComplete() const { return settled_.head_complete; }
  bool HeadExc() const { return settled_.head_exc; }
  bool HeadClosed() const { return settled_.head_closed; }
  bool HeadReplay() const { return settled_.head_replay; }
  uint32_t HeadTag() const { return settled_.head_tag; }
  uint32_t HeadNumUops() const { return settled_.head_num_uops; }
  uint32_t HeadDoneMask() const { return settled_.head_done_mask; }
  uint32_t HeadDoneCnt() const { return settled_.head_done_cnt; }

  // -------------------------------------------------------------- the shadow
  const Slot& S(uint32_t index) const { return shadow_->At(index); }
  std::vector<uint32_t> LiveIndices() const { return shadow_->LiveIndices(); }
  ShadowRob& shadow() const { return *shadow_; }
  uint32_t Entries() const { return entries_; }
  uint32_t MaxUops() const { return max_uops_; }

  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }
  uint64_t allocations() const { return allocs_; }
  uint64_t sweep_checks() const { return sweep_checks_; }
  const Counters& counters() const { return counters_; }

 private:
  // The event reports, with the clock still low.
  void CaptureReports() {
    report_.alloc_ok = dut_->alloc_ok_o;
    report_.alloc_refused = dut_->alloc_refused_o;
    report_.alloc_full = dut_->alloc_full_o;
    report_.alloc_bad_uops = dut_->alloc_bad_uops_o;
    report_.close_ok = dut_->close_ok_o;
    report_.close_stale = dut_->close_stale_o;
    report_.cmp_accepted = dut_->cmp_accepted_o;
    report_.cmp_duplicate = dut_->cmp_duplicate_o;
    report_.cmp_stale = dut_->cmp_stale_o;
    report_.cmp_bad_uop = dut_->cmp_bad_uop_o;
    report_.retire_ack = dut_->retire_ack_o;
  }

  // The outputs as they stand on the settled post-edge state.
  void Capture() {
    settled_.head_valid = dut_->head_valid_o;
    settled_.head_ready = dut_->head_ready_o;
    settled_.head_complete = dut_->head_complete_o;
    settled_.head_exc = dut_->head_exc_o;
    settled_.head_closed = dut_->head_closed_o;
    settled_.head_replay = dut_->head_replay_o;
    settled_.head_tag = dut_->head_tag_o;
    settled_.head_num_uops = dut_->head_num_uops_o;
    settled_.head_done_mask = dut_->head_done_mask_o;
    settled_.head_done_cnt = dut_->head_done_cnt_o;
    settled_.head_ptr = dut_->o_head_ptr_o;
    settled_.alloc_ptr = dut_->o_alloc_ptr_o;
    settled_.occupied = dut_->o_occupied_o;
    settled_.free_slots = dut_->o_free_o;
    settled_.alloc_total = dut_->o_alloc_total_o;
    settled_.retired_total = dut_->o_retired_total_o;
    settled_.squashed_total = dut_->o_squashed_total_o;
    settled_.gen_counter = dut_->o_gen_counter_o;
    settled_.alloc_index = dut_->alloc_index_o;
    settled_.alloc_gen = dut_->alloc_gen_o;
  }

  // ------------------------------------------------------------- comparison
  void Compare(const Stim& s) {
    const std::string where = At(phase_, clk_->cycle());
    const std::string stim = Describe(s);
    const ShadowRob::View e = shadow_->Peek(s);

    auto bit = [&](const char* name, bool got, bool want) {
      Require(got == want, where, std::string(name) + ": expected " + Bool(want) +
                                ", got " + Bool(got) + stim);
      ++comparisons_;
    };
    auto field = [&](const char* name, uint64_t got, uint64_t want) {
      Require(got == want, where, std::string(name) + ": expected " +
                                std::to_string(want) + ", got " +
                                std::to_string(got) + stim);
      ++comparisons_;
    };

    bit("alloc_ok", dut_->alloc_ok_o, e.alloc_ok);
    bit("alloc_refused", dut_->alloc_refused_o, e.alloc_refused);
    bit("alloc_full", dut_->alloc_full_o, e.alloc_full);
    bit("alloc_bad_uops", dut_->alloc_bad_uops_o, e.alloc_bad_uops);
    field("alloc_index", dut_->alloc_index_o, e.alloc_index);
    field("alloc_gen", dut_->alloc_gen_o, e.alloc_gen);

    bit("close_ok", dut_->close_ok_o, e.close_ok);
    bit("close_stale", dut_->close_stale_o, e.close_stale);

    bit("cmp_accepted", dut_->cmp_accepted_o, e.cmp_accepted);
    bit("cmp_duplicate", dut_->cmp_duplicate_o, e.cmp_duplicate);
    bit("cmp_stale", dut_->cmp_stale_o, e.cmp_stale);
    bit("cmp_bad_uop", dut_->cmp_bad_uop_o, e.cmp_bad_uop);

    bit("retire_ack", dut_->retire_ack_o, e.retire_ack);
    bit("head_valid", dut_->head_valid_o, e.head_valid);
    bit("head_ready", dut_->head_ready_o, e.head_ready);
    bit("head_replay", dut_->head_replay_o, e.head_replay);
    bit("head_complete", dut_->head_complete_o, e.head_complete);
    bit("head_exc", dut_->head_exc_o, e.head_exc);
    bit("head_closed", dut_->head_closed_o, e.head_closed);
    field("head_index", dut_->head_index_o, e.head_index);
    field("head_gen", dut_->head_gen_o, e.head_gen);
    field("head_tag", dut_->head_tag_o, e.head_tag);
    Require(dut_->head_pc_o == e.head_pc, where,
            "head_pc: expected " + mosaic::Hex(e.head_pc) + ", got " +
                mosaic::Hex(dut_->head_pc_o) + stim);
    field("head_num_uops", dut_->head_num_uops_o, e.head_num_uops);
    field("head_done_mask", dut_->head_done_mask_o, e.head_done_mask);
    field("head_done_cnt", dut_->head_done_cnt_o, e.head_done_cnt);

    bit("obs_valid", dut_->obs_valid_o, e.obs_valid);
    field("obs_gen", dut_->obs_gen_o, e.obs_gen);
    field("obs_tag", dut_->obs_tag_o, e.obs_tag);
    Require(dut_->obs_pc_o == e.obs_pc, where,
            "obs_pc: expected " + mosaic::Hex(e.obs_pc) + ", got " +
                mosaic::Hex(dut_->obs_pc_o) + stim);
    field("obs_num_uops", dut_->obs_num_uops_o, e.obs_num_uops);
    field("obs_done_mask", dut_->obs_done_mask_o, e.obs_done_mask);
    field("obs_done_cnt", dut_->obs_done_cnt_o, e.obs_done_cnt);
    bit("obs_exc", dut_->obs_exc_o, e.obs_exc);
    bit("obs_closed", dut_->obs_closed_o, e.obs_closed);

    field("o_head_ptr", dut_->o_head_ptr_o, e.head_ptr);
    field("o_alloc_ptr", dut_->o_alloc_ptr_o, e.alloc_ptr);
    field("o_occupied", dut_->o_occupied_o, e.occupied);
    field("o_free", dut_->o_free_o, e.free_slots);
    field("o_alloc_total", dut_->o_alloc_total_o, e.alloc_total);
    field("o_retired_total", dut_->o_retired_total_o, e.retired_total);
    field("o_squashed_total", dut_->o_squashed_total_o, e.squashed_total);
    field("o_gen_counter", dut_->o_gen_counter_o, e.gen_counter);

    Invariants(where, stim, s);
  }

  // Compare every slot through the observation port. This is what makes the
  // checks "across the full depth" rather than "at the head": a descriptor
  // corrupted in a slot nothing is reading right now is still corrupt.
  void SweepSlots(const Stim& s) {
    const uint32_t selected = s.obs_index;
    const std::string where = At(phase_, clk_->cycle());
    for (uint32_t i = 0; i < entries_; i++) {
      if (i == selected) continue;
      dut_->obs_index_i = i;
      dut_->eval();
      CompareObs(i, where);
      ++sweep_checks_;
    }
    dut_->obs_index_i = selected;
    dut_->eval();
  }

  void CompareObs(uint32_t index, const std::string& where) {
    const Slot& slot = shadow_->At(index);
    const std::string at = "obs[" + std::to_string(index) + "].";
    auto bit = [&](const char* name, bool got, bool want) {
      Require(got == want, where,
              at + name + ": expected " + Bool(want) + ", got " + Bool(got));
      ++comparisons_;
    };
    auto field = [&](const char* name, uint64_t got, uint64_t want) {
      Require(got == want, where,
              at + name + ": expected " + std::to_string(want) + ", got " +
                  std::to_string(got));
      ++comparisons_;
    };

    bit("valid", dut_->obs_valid_o, slot.valid);
    field("gen", dut_->obs_gen_o, slot.valid ? slot.gen : 0);
    field("tag", dut_->obs_tag_o, slot.valid ? slot.tag : 0);
    Require(dut_->obs_pc_o == (slot.valid ? slot.pc : 0), where,
            at + "pc: expected " + mosaic::Hex(slot.valid ? slot.pc : 0) + ", got " +
                mosaic::Hex(dut_->obs_pc_o));
    field("num_uops", dut_->obs_num_uops_o, slot.valid ? slot.num_uops : 0);
    field("done_mask", dut_->obs_done_mask_o,
          slot.valid ? (slot.done_mask & shadow_->ExpectedMask(slot.num_uops)) : 0);
    field("done_cnt", dut_->obs_done_cnt_o, PopCount(dut_->obs_done_mask_o));
    bit("exc", dut_->obs_exc_o, slot.valid && slot.exc);
    bit("closed", dut_->obs_closed_o, slot.valid && slot.closed);
  }

  // --------------------------------------------------------- the invariants
  void Invariants(const std::string& where, const std::string& stim, const Stim& s) {
    const uint32_t occupied = dut_->o_occupied_o;

    // I1. Every allocated macro is accounted for exactly once: retired, squashed
    //     or still held. This is the ROB's conservation law, and it is what
    //     makes "the free count is restored exactly after a flush" a checkable
    //     claim rather than an assertion.
    Require(dut_->o_alloc_total_o ==
                dut_->o_retired_total_o + dut_->o_squashed_total_o + occupied,
            where, "conservation: alloc_total " + std::to_string(dut_->o_alloc_total_o) +
                       " != retired " + std::to_string(dut_->o_retired_total_o) +
                       " + squashed " + std::to_string(dut_->o_squashed_total_o) +
                       " + occupied " + std::to_string(occupied) + stim);

    // I2. Occupancy is bounded by the geometry, in both directions.
    Require(occupied <= entries_, where,
            "occupancy " + std::to_string(occupied) + " exceeds the " +
                std::to_string(entries_) + " entries the ROB has" + stim);
    Require(dut_->o_free_o == entries_ - occupied, where,
            "the free count does not complement occupancy" + stim);

    // I3. The generation counter advances on exactly the allocations and on
    //     nothing else -- not on a retirement, not on a flush. A flush that
    //     rewound it breaks this on the flush cycle itself, which is why the
    //     check is every cycle and not only in the flush phase.
    Require(dut_->o_gen_counter_o == dut_->o_alloc_total_o, where,
            "gen_counter " + std::to_string(dut_->o_gen_counter_o) +
                " != alloc_total " + std::to_string(dut_->o_alloc_total_o) + stim);

    // I4. A completion produces exactly one outcome, and only if one was
    //     offered. Four overlapping reports would make "which of them happened"
    //     unanswerable, which is the same as not reporting any of them.
    const int outcomes = dut_->cmp_accepted_o + dut_->cmp_duplicate_o +
                         dut_->cmp_stale_o + dut_->cmp_bad_uop_o;
    Require(outcomes <= 1, where,
            "one completion produced " + std::to_string(outcomes) +
                " mutually exclusive reports" + stim);
    Require(outcomes == (s.cmp_valid && !s.flush ? 1 : 0), where,
            "a completion was offered but " + std::to_string(outcomes) +
                " reports came back" + stim);

    // I5. Retirement never advances past a macro that is not ready.
    Require(!dut_->retire_ack_o || dut_->head_ready_o, where,
            "retire_ack asserted without head_ready" + stim);

    // I6. Progress never exceeds the macro's own child count, and the reported
    //     count is the population count of the reported bitmap. This is the
    //     invariant an arrival-count design breaks on a duplicate.
    Require(dut_->head_done_cnt_o <= max_uops_, where,
            "head_done_cnt " + std::to_string(dut_->head_done_cnt_o) +
                " exceeds the widest macro the geometry allows" + stim);
    if (dut_->head_valid_o) {
      Require(dut_->head_done_cnt_o <= dut_->head_num_uops_o, where,
              "head_done_cnt " + std::to_string(dut_->head_done_cnt_o) +
                  " exceeds the head's own child count " +
                  std::to_string(dut_->head_num_uops_o) + stim);
      Require(dut_->head_done_cnt_o == PopCount(dut_->head_done_mask_o), where,
              "head_done_cnt is not the population count of head_done_mask" + stim);
      Require(dut_->head_done_mask_o ==
                  (dut_->head_done_mask_o &
                   shadow_->ExpectedMask(dut_->head_num_uops_o)),
              where,
              "head_done_mask has bits outside the head's own child count" + stim);
    }

    // I7. A macro reported for replay is exceptional, and an exceptional macro
    //     is never ready. Together with I5, this is the head-blocking rule.
    Require(!dut_->head_replay_o || (dut_->head_exc_o && dut_->head_valid_o), where,
            "head_replay without an exceptional live head" + stim);
    Require(!(dut_->head_exc_o && dut_->head_ready_o), where,
            "an exceptional macro is ready to retire" + stim);

    // I8. An open macro is never ready: the set of children it waits for is not
    //     final, and retiring on a bitmap that is still growing is the same
    //     failure as retiring on an arrival count.
    Require(!(dut_->head_valid_o && !dut_->head_closed_o && dut_->head_ready_o), where,
            "an unclosed macro is ready to retire" + stim);

    // I9. The conservation counters are monotonic. A flush that cleared
    //     committed history breaks this on the flush cycle itself.
    if (have_prev_) {
      Require(dut_->o_retired_total_o >= prev_retired_, where,
              "retired_total went backwards: " + std::to_string(prev_retired_) +
                  " -> " + std::to_string(dut_->o_retired_total_o) + stim);
      Require(dut_->o_squashed_total_o >= prev_squashed_, where,
              "squashed_total went backwards: " + std::to_string(prev_squashed_) +
                  " -> " + std::to_string(dut_->o_squashed_total_o) + stim);
    }
  }

  // Count what the soak actually exercised, from the DUT's own reports.
  void Tally(const Stim& s) {
    if (s.flush) counters_.flushes++;
    if (dut_->cmp_accepted_o) counters_.accepted++;
    if (dut_->cmp_duplicate_o) counters_.duplicates++;
    if (dut_->cmp_stale_o) counters_.stale_completions++;
    if (dut_->cmp_bad_uop_o) counters_.bad_uops++;
    if (dut_->alloc_refused_o && dut_->alloc_full_o) counters_.full_refusals++;
    if (dut_->alloc_ok_o && s.alloc_exc) counters_.exceptional++;
    if (report_.alloc_ok) ++allocs_;

    prev_retired_ = dut_->o_retired_total_o;
    prev_squashed_ = dut_->o_squashed_total_o;
    have_prev_ = true;
  }

  Vmosaic_rob_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  ShadowRob* shadow_;
  uint32_t entries_;
  uint32_t max_uops_;
  std::string phase_ = "startup";
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  uint64_t sweep_checks_ = 0;
  uint64_t allocs_ = 0;      // across the whole run, never reset per phase
  Counters counters_;
  Settled settled_;
  Reports report_;
  uint32_t prev_retired_ = 0;
  uint32_t prev_squashed_ = 0;
  bool have_prev_ = false;
};

// ------------------------------------------------------------------ phase 1
// The cold state: an empty ROB, zeroed counters, nothing retirable, and every
// slot reporting itself invalid rather than whatever the silicon powered up with.
void PhaseResetState(Harness* h) {
  h->Phase("reset-state");

  Require(h->ObservedOccupied() == 0, "reset-state",
          "the ROB is not empty after reset: occupied = " +
              std::to_string(h->ObservedOccupied()));
  Require(h->ObservedFree() == h->Entries(), "reset-state",
          "the free count after reset is " + std::to_string(h->ObservedFree()) +
              ", expected " + std::to_string(h->Entries()));
  Require(h->ObservedAllocTotal() == 0, "reset-state", "alloc_total is not zero");
  Require(h->ObservedRetired() == 0, "reset-state", "retired_total is not zero");
  Require(h->ObservedSquashed() == 0, "reset-state", "squashed_total is not zero");
  Require(h->ObservedGenCounter() == 0, "reset-state", "gen_counter is not zero");
  Require(h->ObservedAllocGen() == 0, "reset-state", "alloc_gen is not zero");
  Require(h->ObservedAllocIndex() == 0, "reset-state", "alloc_ptr is not zero");
  Require(h->HeadValid() == 0, "reset-state", "a reset ROB reports a valid head");
  Require(h->HeadTag() == 0 && h->HeadNumUops() == 0 && h->HeadDoneMask() == 0,
          "reset-state",
          "a reset ROB reports a non-zero descriptor at the head");

  Require(!h->Retire(), "reset-state", "an empty ROB acknowledged a retirement");

  for (uint32_t i = 0; i < h->Entries(); i++) h->Observe(i);
}

// ------------------------------------------------------------------ phase 2
// The card's own failure case: children complete out of order, and the last one
// to arrive is not the last one to complete.
void PhaseOutOfOrder(Harness* h) {
  h->Phase("out-of-order-children");
  const uint32_t max_uops = h->MaxUops();

  // A three-child macro completing 2, 0, 1. Child 2 is the *last* child by
  // index, so a design that took the last arrival as the completion signal calls
  // this macro complete after one cycle, with two children outstanding.
  const Harness::Identity three = h->Alloc(/*tag=*/1, /*num_uops=*/3);

  h->Complete(three.index, three.gen, 2);
  Require(h->HeadComplete() == 0, "out-of-order-children",
          "a 3-child macro was called complete after its last-numbered child "
          "alone arrived -- exactly the 'last-uop-arrives-is-complete' failure");
  Require(h->HeadDoneCnt() == 1, "out-of-order-children",
          "expected 1 child complete, got " + std::to_string(h->HeadDoneCnt()));
  Require(h->HeadNumUops() == 3, "out-of-order-children",
          "the descriptor lost its child count");
  Require(!h->Retire(), "out-of-order-children",
          "the ROB retired a 3-child macro with two children outstanding");

  h->Complete(three.index, three.gen, 0);
  Require(h->HeadComplete() == 0, "out-of-order-children",
          "a 3-child macro was called complete with child 1 outstanding");
  Require(h->HeadDoneCnt() == 2, "out-of-order-children",
          "expected 2 children complete, got " + std::to_string(h->HeadDoneCnt()));

  h->Complete(three.index, three.gen, 1);
  Require(h->HeadComplete() == 1, "out-of-order-children",
          "a 3-child macro was not complete after all three children arrived");
  Require(h->HeadDoneCnt() == 3, "out-of-order-children",
          "expected 3 children complete, got " + std::to_string(h->HeadDoneCnt()));
  Require(h->Retire(), "out-of-order-children",
          "a fully complete 3-child macro did not retire");

  // The widest macro the geometry allows, completing in strict reverse order:
  // the last child first, on every step.
  const Harness::Identity wide = h->Alloc(/*tag=*/2, max_uops);
  for (uint32_t uop = max_uops; uop-- > 0;) {
    h->Complete(wide.index, wide.gen, uop);
    const bool last = (uop == 0);
    Require(h->HeadComplete() == last, "out-of-order-children",
            "after child " + std::to_string(uop) + " of " + std::to_string(max_uops) +
                " in reverse order: expected " + (last ? "complete" : "incomplete") +
                ", got " + (h->HeadComplete() ? "complete" : "incomplete"));
    Require(h->HeadDoneCnt() <= h->HeadNumUops(), "out-of-order-children",
            "the done count passed the child count mid-expansion");
  }
  Require(h->Retire(), "out-of-order-children",
          "the fully reversed " + std::to_string(max_uops) +
              "-child macro did not retire");
  Require(h->ObservedRetired() == 2, "out-of-order-children",
          "expected exactly 2 retirements, got " + std::to_string(h->ObservedRetired()));

  // A child index outside the macro's own expansion is a fourth report, not a
  // silent bit: a 3-child macro has no child 5, and setting that bit would make
  // the mask claim a child the expansion never had.
  const Harness::Identity narrow = h->Alloc(/*tag=*/3, /*num_uops=*/3);
  h->Complete(narrow.index, narrow.gen, 5);
  Require(h->CmpBadUop() == 1, "out-of-order-children",
          "a completion for child 5 of a 3-child macro was not reported out of range");
  Require(h->CmpAccepted() == 0, "out-of-order-children",
          "a completion outside the macro's child count was accepted");
  Require(h->HeadDoneMask() == 0, "out-of-order-children",
          "an out-of-range completion changed the completion bitmap");
}

// ------------------------------------------------------------------ phase 3
// The same child three times: reported every time, counted once, progress
// unchanged.
void PhaseDuplicate(Harness* h) {
  h->Phase("duplicate");

  const Harness::Identity m = h->Alloc(/*tag=*/7, /*num_uops=*/3);

  h->Complete(m.index, m.gen, 1);
  const uint32_t count_after_first = h->HeadDoneCnt();
  const uint32_t mask_after_first = h->HeadDoneMask();
  Require(count_after_first == 1, "duplicate",
          "expected 1 child complete, got " + std::to_string(count_after_first));

  Stim s;
  s.cmp_valid = true;
  s.cmp_index = m.index;
  s.cmp_gen = m.gen;
  s.cmp_uop = 1;

  // The identical completion again. It must be *reported*, and it must change
  // nothing: no second count, no second bit, no completion.
  h->Cycle(s);
  Require(h->CmpDuplicate() == 1, "duplicate",
          "the second arrival of child 1 was not reported as a duplicate");
  Require(h->CmpAccepted() == 0, "duplicate",
          "a duplicate arrival was accepted as a fresh completion");
  Require(h->HeadDoneCnt() == count_after_first, "duplicate",
          "a duplicate arrival moved the done count from " +
              std::to_string(count_after_first) + " to " +
              std::to_string(h->HeadDoneCnt()));
  Require(h->HeadDoneMask() == mask_after_first, "duplicate",
          "a duplicate arrival changed the completion bitmap");
  Require(h->HeadComplete() == 0, "duplicate",
          "a 3-child macro became complete after one child plus its duplicate");

  // And a third delivery, to prove the report is not a one-shot.
  h->Cycle(s);
  Require(h->CmpDuplicate() == 1, "duplicate",
          "the third arrival of child 1 stopped being reported");

  // Fill the macro in and retire it: the duplicates must not have advanced it.
  h->Complete(m.index, m.gen, 0);
  h->Complete(m.index, m.gen, 2);
  Require(h->HeadDoneCnt() == 3, "duplicate",
          "after every child plus three duplicates of child 1 the done count is " +
              std::to_string(h->HeadDoneCnt()) + ", expected 3");
  Require(h->HeadComplete() == 1, "duplicate",
          "the macro is not complete even though all three children arrived once");
  Require(h->Retire(), "duplicate", "the completed macro did not retire");
  Require(h->ObservedRetired() == 1, "duplicate",
          "the duplicate arrivals retired the macro early: retired_total = " +
              std::to_string(h->ObservedRetired()));
}

// ------------------------------------------------------------------ phase 4
// The wrap. Every slot recycled, then a completion aimed at a recycled slot
// rejected.
void PhaseWrapGeneration(Harness* h) {
  h->Phase("wrap-generation");
  const uint32_t entries = h->Entries();

  // -- the scenario: retire a macro, then allocate exactly far enough around the
  //    circle for the allocation pointer to land on the slot that macro just
  //    vacated, and prove the occupant has changed.
  const Harness::Identity victim = h->Alloc(/*tag=*/100, /*num_uops=*/1);
  h->Complete(victim.index, victim.gen, 0);
  Require(h->Retire(), "wrap-generation", "the victim macro did not retire");

  const uint32_t occ_after_retire = h->ObservedOccupied();
  const uint32_t victim_slot = victim.index;
  // An allocation writes the slot the pointer *names* and only then advances it,
  // so recycling slot S takes one more allocation than it takes for the pointer
  // to arrive at S. Getting this off by one would leave the victim's slot
  // untouched and the scenario below would prove nothing -- which is exactly the
  // "the wrap probably happened" failure this phase exists to rule out.
  const uint32_t distance =
      (victim_slot + entries - h->ObservedAllocPtr()) % entries + 1;
  Require(distance + occ_after_retire <= entries, "wrap-generation",
          "recycling slot " + std::to_string(victim_slot) + " needs " +
              std::to_string(distance) + " more allocations but only " +
              std::to_string(entries - occ_after_retire) + " fit");

  for (uint32_t i = 0; i < distance; i++) h->Alloc(/*tag=*/200 + i, /*num_uops=*/1);

  // The wrap has now provably happened, and it is checked rather than assumed:
  // the slot the victim vacated is live again, holding a different generation
  // and a different macro.
  const Slot& occupant = h->S(victim_slot);
  Require(occupant.valid, "wrap-generation",
          "slot " + std::to_string(victim_slot) +
              " was not reallocated, so the wrap did not happen");
  Require(occupant.gen != victim.gen, "wrap-generation",
          "slot " + std::to_string(victim_slot) + " was reallocated with the same " +
              "generation " + mosaic::Hex(victim.gen) +
              ", so the generations are not doing any work");
  Require(occupant.tag != victim.tag, "wrap-generation",
          "slot " + std::to_string(victim_slot) +
              " still holds the retired macro's tag");

  // The victim's late completion must be rejected. The slot is *live*, which is
  // what makes this sharp: a design that checked only liveness would accept it,
  // and the result would be written into the wrong instruction.
  const uint32_t mask_before = occupant.done_mask;
  Stim s;
  s.cmp_valid = true;
  s.cmp_index = victim.index;
  s.cmp_gen = victim.gen;
  s.cmp_uop = 0;
  h->Cycle(s);
  Require(h->CmpStale() == 1, "wrap-generation",
          "a completion aimed at a recycled slot (index " +
              std::to_string(victim.index) + ", generation " +
              mosaic::Hex(victim.gen) + ", but the slot now holds generation " +
              mosaic::Hex(occupant.gen) + ") was not rejected");
  Require(h->CmpAccepted() == 0, "wrap-generation",
          "a stale completion was accepted into the macro that recycled the slot");
  Require(h->S(victim_slot).done_mask == mask_before, "wrap-generation",
          "a stale completion changed the live macro's completion bitmap");

  // The converse, so this phase cannot pass on "reject everything": a completion
  // naming the slot's current generation is accepted.
  h->Complete(victim_slot, h->S(victim_slot).gen, 0);
  Require(h->CmpAccepted() == 1, "wrap-generation",
          "a completion naming the live macro's own generation was rejected, so "
          "the rejection above proves nothing");
  Require(h->S(victim_slot).done_mask != mask_before, "wrap-generation",
          "an accepted completion did not change the live macro's bitmap");

  // -- the sustained case: drive well past one full turn of the pointer. The
  //    allocation pointer is strictly round-robin here, so the per-slot
  //    allocation counts below are a lower bound on how many times each slot
  //    recycled, not an estimate.
  //
  // The buffer is at capacity after the scenario above, so the loop has to keep
  // it turning over or every allocation below would be refused and the phase
  // would prove nothing. Each round finishes whatever is at the head, retires
  // it, allocates one more macro and completes that macro's children in a
  // rotating order.
  const uint32_t rounds = 3 * entries + 17;
  for (uint32_t round = 0; round < rounds; round++) {
    for (uint32_t spin = 0; spin < h->MaxUops() + 2; spin++) {
      if (!h->HeadValid() || h->HeadComplete()) break;
      const uint32_t head_index = h->ObservedHeadPtr();
      const Slot& head_slot = h->S(head_index);
      uint32_t uop = 0;
      while (uop < head_slot.num_uops && ((head_slot.done_mask >> uop) & 1u) != 0) uop++;
      if (uop >= head_slot.num_uops) break;
      h->Complete(head_index, head_slot.gen, uop);
    }
    h->Retire();

    const uint32_t num_uops = 1 + (round % 3);
    const Harness::Identity id = h->Alloc(/*tag=*/1000 + round, num_uops);
    Require(h->AllocOk(), "wrap-generation",
            "allocation " + std::to_string(round) +
                " was refused with " + std::to_string(h->ObservedOccupied()) +
                " macros held; the buffer stopped turning over, so the rest of "
                "this phase would prove nothing");
    for (uint32_t k = 0; k < num_uops; k++) {
      h->Complete(id.index, id.gen, (k + round) % num_uops);
    }
  }

  Require(h->ObservedAllocTotal() >= 2 * entries, "wrap-generation",
          "only " + std::to_string(h->ObservedAllocTotal()) +
              " allocations happened; a " + std::to_string(entries) +
              "-entry ROB cannot have wrapped");

  uint32_t least_recycled = ~0u;
  for (uint32_t i = 0; i < entries; i++) {
    least_recycled = std::min(least_recycled, h->shadow().SlotAllocations(i));
  }
  Require(least_recycled >= 2, "wrap-generation",
          "the least-recycled slot was allocated only " +
              std::to_string(least_recycled) +
              " times, so this phase did not wrap the buffer");

  // Every live slot now has an older generation to probe with, and every one of
  // them must still be rejected: the aliasing property stated over the whole
  // phase rather than over one hand-built case.
  const std::vector<uint32_t> live = h->LiveIndices();
  Require(!live.empty(), "wrap-generation", "nothing is live to probe with");
  for (uint32_t idx : live) {
    const uint32_t first_gen = h->shadow().SlotFirstGen(idx);
    Require(first_gen != h->S(idx).gen, "wrap-generation",
            "slot " + std::to_string(idx) +
                " has no older generation, so it was never recycled");
    Stim probe;
    probe.cmp_valid = true;
    probe.cmp_index = idx;
    probe.cmp_gen = first_gen;
    probe.cmp_uop = 0;
    const uint32_t mask = h->S(idx).done_mask;
    h->Cycle(probe);
    Require(h->CmpStale() == 1, "wrap-generation",
            "a completion naming generation " + mosaic::Hex(first_gen) + " for slot " +
                std::to_string(idx) + " was not rejected; the slot now holds " +
                mosaic::Hex(h->S(idx).gen));
    Require(h->S(idx).done_mask == mask, "wrap-generation",
            "a stale completion changed slot " + std::to_string(idx));
  }

  // Drain, so the phase ends with the ROB empty and the conservation identity
  // holding over every allocation it made. No squash: a flush here would mean
  // the drain was not in order.
  for (uint32_t guard = 0; guard < 16 * entries + 64 && h->ObservedOccupied() > 0;
       guard++) {
    bool progressed = false;
    for (uint32_t idx : h->LiveIndices()) {
      const Slot& slot = h->S(idx);
      for (uint32_t uop = 0; uop < slot.num_uops; uop++) {
        if (((slot.done_mask >> uop) & 1u) == 0) {
          h->Complete(idx, slot.gen, uop);
          progressed = true;
          break;
        }
      }
      if (progressed) break;
    }
    h->Retire();
  }
  Require(h->ObservedOccupied() == 0, "wrap-generation",
          "the ROB did not drain: " + std::to_string(h->ObservedOccupied()) +
              " macros still held");
  Require(h->ObservedRetired() == h->ObservedAllocTotal(), "wrap-generation",
          "every macro allocated in this phase should have retired: " +
              std::to_string(h->ObservedRetired()) + " retired, " +
              std::to_string(h->ObservedAllocTotal()) + " allocated");
  Require(h->ObservedSquashed() == 0, "wrap-generation",
          "the wrap phase squashed a macro, but issued no flush");
}

// ------------------------------------------------------------------ phase 5
// Capacity: allocation into a full ROB is refused, reported, and changes
// nothing; then the whole buffer drains strictly in order.
void PhaseFull(Harness* h) {
  h->Phase("full");
  const uint32_t entries = h->Entries();

  std::vector<Harness::Identity> live;
  for (uint32_t i = 0; i < entries; i++) {
    live.push_back(h->Alloc(/*tag=*/300 + i, /*num_uops=*/1));
  }
  Require(h->ObservedOccupied() == entries, "full",
          "expected " + std::to_string(entries) + " macros held, got " +
              std::to_string(h->ObservedOccupied()));

  // Every macro went to a distinct slot: the circular pointer did not fold two
  // of them onto one another.
  {
    std::vector<uint32_t> indices;
    for (const Harness::Identity& id : live) indices.push_back(id.index);
    std::sort(indices.begin(), indices.end());
    for (size_t i = 1; i < indices.size(); i++) {
      Require(indices[i] != indices[i - 1], "full",
              "two live macros share slot " + std::to_string(indices[i]));
    }
  }

  // One more: refused, reported, and changing nothing.
  Stim s;
  s.alloc_valid = true;
  s.alloc_tag = 999;
  s.alloc_pc = 0xdeadbeefull;
  s.alloc_num_uops = 1;
  h->Cycle(s);
  Require(h->AllocOk() == 0, "full", "an allocation into a full ROB was accepted");
  Require(h->AllocRefused() == 1, "full",
          "an allocation into a full ROB was not reported as refused");
  Require(h->AllocFull() == 1, "full", "the ROB does not report itself full");
  Require(h->ObservedOccupied() == entries, "full",
          "the refused allocation changed the occupancy");
  Require(h->ObservedAllocTotal() == entries, "full",
          "the refused allocation was counted as an allocation");

  // Nothing was overwritten: every slot still holds the macro it was given.
  for (uint32_t i = 0; i < entries; i++) h->Observe(i);
  for (const Harness::Identity& id : live) {
    Require(h->S(id.index).valid && h->S(id.index).tag == id.tag, "full",
            "slot " + std::to_string(id.index) +
                " no longer holds the macro allocated to it");
  }

  // A malformed child count is refused too, through a different report, so the
  // producer can tell "the ROB is full" from "you sent nonsense".
  s.alloc_num_uops = 0;
  h->Cycle(s);
  Require(h->AllocOk() == 0, "full", "a zero-child macro was allocated");
  Require(h->AllocBadUops() == 1, "full",
          "a zero-child macro was not reported through alloc_bad_uops");
  Require(h->ObservedAllocTotal() == entries, "full",
          "the malformed allocation was counted as an allocation");

  // Drain the whole ROB strictly in order. This is the ordering invariant across
  // the full depth, checked at every position rather than only at the head.
  for (uint32_t i = 0; i < entries; i++) {
    const Harness::Identity& id = live[i];
    Require(h->HeadTag() == id.tag, "full",
            "at position " + std::to_string(i) + " the head holds tag " +
                std::to_string(h->HeadTag()) + ", expected " + std::to_string(id.tag) +
                ": retirement is not in allocation order");
    h->Complete(id.index, id.gen, 0);
    Require(h->HeadComplete() == 1, "full",
            "the head is not complete after its only child arrived");
    Require(h->Retire(), "full",
            "the head at position " + std::to_string(i) + " did not retire");
  }
  Require(h->ObservedOccupied() == 0, "full",
          "the ROB is not empty after draining every macro");
  Require(h->ObservedRetired() == entries, "full",
          "expected " + std::to_string(entries) + " retirements, got " +
              std::to_string(h->ObservedRetired()));

  // Having drained, the ROB accepts allocations again.
  h->Alloc(/*tag=*/400, /*num_uops=*/1);
  Require(h->ObservedOccupied() == 1, "full",
          "the ROB did not accept an allocation after being drained");
}

// ------------------------------------------------------------------ phase 6
// Flush: speculative state goes, committed history stays, the free count is
// restored exactly, and the generation counter does not rewind.
void PhaseFlush(Harness* h) {
  h->Phase("flush");
  const uint32_t entries = h->Entries();

  // Commit some history first.
  std::vector<Harness::Identity> committed;
  for (uint32_t i = 0; i < 6; i++) committed.push_back(h->Alloc(500 + i, 2));
  for (uint32_t i = 0; i < 6; i++) {
    h->Complete(committed[i].index, committed[i].gen, 1);
    h->Complete(committed[i].index, committed[i].gen, 0);
    Require(h->Retire(), "flush", "a committed macro did not retire");
  }
  Require(h->ObservedRetired() == 6, "flush",
          "expected 6 committed retirements, got " + std::to_string(h->ObservedRetired()));

  // Now fill speculatively, part-completed, with one exceptional macro inside.
  std::vector<Harness::Identity> speculative;
  for (uint32_t i = 0; i < 20; i++) {
    speculative.push_back(h->Alloc(600 + i, /*num_uops=*/3, /*exc=*/(i == 7)));
  }
  for (uint32_t i = 0; i < 10; i++) {
    h->Complete(speculative[i].index, speculative[i].gen, 0);
  }
  const uint32_t occupied_before = h->ObservedOccupied();
  Require(occupied_before == 20, "flush",
          "expected 20 speculative macros, got " + std::to_string(occupied_before));

  const uint32_t retired_before = h->ObservedRetired();
  const uint32_t squashed_before = h->ObservedSquashed();
  const uint32_t alloc_total_before = h->ObservedAllocTotal();
  const uint32_t gen_before = h->ObservedGenCounter();

  h->Flush();

  Require(h->ObservedOccupied() == 0, "flush",
          "the flush left " + std::to_string(h->ObservedOccupied()) +
              " macros in the ROB");
  Require(h->ObservedRetired() == retired_before, "flush",
          "the flush disturbed committed history: retired_total went from " +
              std::to_string(retired_before) + " to " +
              std::to_string(h->ObservedRetired()));
  Require(h->ObservedSquashed() == squashed_before + occupied_before, "flush",
          "the flush did not account for the " + std::to_string(occupied_before) +
              " macros it discarded: squashed_total is " +
              std::to_string(h->ObservedSquashed()) + ", expected " +
              std::to_string(squashed_before + occupied_before));
  Require(h->ObservedAllocTotal() == alloc_total_before, "flush",
          "the flush disturbed the allocation count");
  Require(h->ObservedGenCounter() == gen_before, "flush",
          "the flush rewound the generation counter from " +
              std::to_string(gen_before) + " to " +
              std::to_string(h->ObservedGenCounter()) +
              ": a completion in flight across the flush would then alias");
  Require(h->ObservedFree() == entries, "flush",
          "after the flush the free count is " + std::to_string(h->ObservedFree()) +
              ", expected the full " + std::to_string(entries));
  Require(h->HeadValid() == 0, "flush", "a flushed ROB still reports a valid head");
  Require(!h->Retire(), "flush", "a flushed ROB acknowledged a retirement");

  // A completion that was in flight when the flush arrived is still rejected.
  const Harness::Identity& late = speculative[3];
  Stim s;
  s.cmp_valid = true;
  s.cmp_index = late.index;
  s.cmp_gen = late.gen;
  s.cmp_uop = 2;
  h->Cycle(s);
  Require(h->CmpStale() == 1, "flush",
          "a completion that predates the flush was not rejected");
  Require(h->CmpAccepted() == 0, "flush",
          "a completion that predates the flush was accepted");

  // Reallocation after the flush continues the generation sequence rather than
  // restarting it, so nothing in flight can be mistaken for the new work.
  const Harness::Identity first_after = h->Alloc(/*tag=*/700, /*num_uops=*/1);
  Require(first_after.gen == gen_before, "flush",
          "the first allocation after the flush got generation " +
              mosaic::Hex(first_after.gen) +
              ", expected the counter to continue at " + mosaic::Hex(gen_before));
  Require(h->AllocRefused() == 0, "flush",
          "an allocation after a flush was reported as refused");

  // ... and the ROB is fully usable again, up to its capacity and no further.
  for (uint32_t i = 0; i < entries - 1; i++) h->Alloc(800 + i, /*num_uops=*/1);
  Require(h->ObservedOccupied() == entries, "flush",
          "the ROB did not refill to capacity after a flush");
  h->Alloc(/*tag=*/899, /*num_uops=*/1);
  Require(h->AllocOk() == 0, "flush",
          "the ROB accepted more macros than its capacity after a flush");
  Require(h->ObservedRetired() == retired_before, "flush",
          "the allocations after the flush changed the committed history");
}

// ------------------------------------------------------------------ phase 7
// An exceptional macro blocks the head and is reported for replay.
void PhaseException(Harness* h) {
  h->Phase("exception");

  // (a) the exception is known at dispatch.
  const Harness::Identity a = h->Alloc(/*tag=*/10, /*num_uops=*/1);
  const Harness::Identity b = h->Alloc(/*tag=*/11, /*num_uops=*/2, /*exc=*/true);
  const Harness::Identity c = h->Alloc(/*tag=*/12, /*num_uops=*/1);

  h->Complete(a.index, a.gen, 0);
  h->Complete(b.index, b.gen, 0);
  h->Complete(b.index, b.gen, 1);
  h->Complete(c.index, c.gen, 0);

  Require(h->HeadTag() == a.tag, "exception", "the wrong macro is at the head");
  Require(h->Retire(), "exception", "the clean head macro did not retire");

  Require(h->HeadTag() == b.tag, "exception",
          "the exceptional macro is not at the head after the clean one retired");
  Require(h->HeadComplete() == 1, "exception",
          "the exceptional macro is not complete, so this phase would be testing "
          "an ordinary completeness rule instead of the exception rule");
  Require(h->HeadExc() == 1, "exception",
          "the exceptional macro is not reported as exceptional");
  Require(h->HeadReplay() == 1, "exception",
          "the exceptional macro is not reported for replay");
  Require(h->HeadReady() == 0, "exception",
          "an exceptional macro is reported ready to retire");

  // Ask repeatedly. It must never advance, however long the request is held.
  for (uint32_t i = 0; i < 8; i++) {
    Require(!h->Retire(), "exception",
            "an exceptional macro retired after " + std::to_string(i + 1) +
                " retirement requests");
  }
  Require(h->ObservedOccupied() == 2, "exception",
          "retirement requests past an exceptional macro consumed entries: " +
              std::to_string(h->ObservedOccupied()) + " left, expected 2");
  Require(h->HeadTag() == b.tag, "exception",
          "the head advanced past the exceptional macro");

  // The macro behind it is complete and must not retire either: in-order
  // retirement means the head blocks the whole window, not just itself.
  Require(h->S(c.index).done_mask != 0, "exception",
          "the macro behind the exceptional one was not completed, so this phase "
          "is not testing head blocking");
  Require(h->S(c.index).done_mask == h->shadow().ExpectedMask(h->S(c.index).num_uops),
          "exception", "the macro behind the exceptional one is not complete");

  // (b) the exception arrives with a child completion instead.
  h->Fresh();
  h->Phase("exception");
  const Harness::Identity d = h->Alloc(/*tag=*/20, /*num_uops=*/2);
  h->Complete(d.index, d.gen, 0);
  Require(h->HeadExc() == 0, "exception",
          "the macro is exceptional before any child has raised one");
  h->Complete(d.index, d.gen, 1, /*exc=*/true);
  Require(h->HeadExc() == 1, "exception",
          "an exception raised by a child completion was lost");
  Require(h->HeadReplay() == 1, "exception",
          "an exception raised by a child completion is not reported for replay");
  Require(h->HeadComplete() == 1, "exception",
          "the macro is not complete after both children arrived");
  Require(h->HeadReady() == 0, "exception",
          "a macro whose child raised an exception is ready to retire");
  Require(!h->Retire(), "exception",
          "a macro whose child raised an exception retired");
}

// ------------------------------------------------------------------ phase 8
// An open macro accumulates children but cannot retire until it is closed.
void PhaseCloseGate(Harness* h) {
  h->Phase("close-gate");

  const Harness::Identity open =
      h->Alloc(/*tag=*/30, /*num_uops=*/2, /*exc=*/false, /*open=*/true);
  h->Complete(open.index, open.gen, 0);
  h->Complete(open.index, open.gen, 1);

  Require(h->HeadComplete() == 1, "close-gate",
          "the open macro is not complete even though both children arrived");
  Require(h->HeadClosed() == 0, "close-gate",
          "a macro allocated open reports itself closed");
  Require(h->HeadReady() == 0, "close-gate",
          "an open macro is ready to retire while its expansion is still growing");
  Require(!h->Retire(), "close-gate", "an open macro retired");

  // A close aimed at a stale generation is refused and reported.
  Stim s;
  s.close_valid = true;
  s.close_index = open.index;
  s.close_gen = open.gen ^ 1u;
  h->Cycle(s);
  Require(h->CloseStale() == 1, "close-gate",
          "a close naming the wrong generation was not reported stale");
  Require(h->CloseOk() == 0, "close-gate",
          "a close naming the wrong generation was applied");
  Require(h->HeadClosed() == 0, "close-gate", "a stale close closed the macro anyway");

  // A close aimed at a dead slot is refused too.
  s.close_index = (open.index + 1) % h->Entries();
  s.close_gen = 0;
  h->Cycle(s);
  Require(h->CloseStale() == 1, "close-gate",
          "a close aimed at a slot that is not live was not reported stale");
  Require(h->CloseOk() == 0, "close-gate", "a close into a dead slot was applied");

  // The real close.
  h->Close(open.index, open.gen);
  Require(h->CloseOk() == 1, "close-gate",
          "a close naming the right generation was refused");
  Require(h->HeadClosed() == 1, "close-gate", "the macro did not close");
  Require(h->Retire(), "close-gate", "the closed macro did not retire");
}

// ------------------------------------------------------------------ phase 9
// Every slot live at once, kept full while the buffer wraps repeatedly, every
// slot swept on every cycle, retirement order checked against allocation order.
void PhaseFullDepth(Harness* h, uint32_t retire_target) {
  h->Phase("full-depth");
  const uint32_t entries = h->Entries();

  std::deque<uint32_t> order;
  for (uint32_t i = 0; i < entries; i++) {
    h->Alloc(/*tag=*/1000 + i, /*num_uops=*/1 + (i % h->MaxUops()));
    order.push_back(1000 + i);
  }
  Require(h->ObservedOccupied() == entries, "full-depth",
          "the ROB is not full: " + std::to_string(h->ObservedOccupied()) + " of " +
              std::to_string(entries));

  uint32_t retired = 0;
  const uint32_t guard_limit = 32 * entries + 256;
  for (uint32_t guard = 0; guard < guard_limit && retired < retire_target; guard++) {
    // Always make progress: work on a macro that has no child completed yet if
    // there is one, otherwise finish the head, otherwise ask to retire. A phase
    // that only ever asked to retire would hang on a head that needs more
    // children, and one that only ever completed children would never drain.
    Stim s;
    bool worked = false;
    for (uint32_t idx : h->LiveIndices()) {
      if (h->S(idx).done_mask == 0) {
        s.cmp_valid = true;
        s.cmp_index = idx;
        s.cmp_gen = h->S(idx).gen;
        s.cmp_uop = 0;
        worked = true;
        break;
      }
    }
    if (!worked) {
      const Slot& head_slot = h->S(h->ObservedHeadPtr());
      const uint32_t expected = h->shadow().ExpectedMask(head_slot.num_uops);
      if (head_slot.valid && head_slot.done_mask != expected) {
        for (uint32_t uop = 0; uop < head_slot.num_uops; uop++) {
          if (((head_slot.done_mask >> uop) & 1u) == 0) {
            s.cmp_valid = true;
            s.cmp_index = h->ObservedHeadPtr();
            s.cmp_gen = head_slot.gen;
            s.cmp_uop = uop;
            worked = true;
            break;
          }
        }
      }
    }
    if (!worked) s.retire_req = true;

    const uint32_t head_before = h->HeadTag();
    h->Cycle(s, /*rst=*/false, /*sweep=*/true);

    if (s.retire_req && h->RetireAck()) {
      Require(!order.empty(), "full-depth",
              "a macro retired but the expected-order queue is empty");
      Require(head_before == order.front(), "full-depth",
              "retirement " + std::to_string(retired) + " took the macro with tag " +
                  std::to_string(head_before) + ", but the oldest live macro has tag " +
                  std::to_string(order.front()) +
                  ": retirement is not strictly in allocation order");
      order.pop_front();
      ++retired;

      // Refill, so the ROB stays at full occupancy and the allocation pointer
      // keeps wrapping while the phase runs.
      if (h->ObservedOccupied() < entries) {
        const uint32_t tag = 2000 + retired;
        h->Alloc(tag, /*num_uops=*/1 + (retired % h->MaxUops()));
        order.push_back(tag);
      }
      Require(order.size() == h->ObservedOccupied(), "full-depth",
              "the expected-order queue holds " + std::to_string(order.size()) +
                  " tags but the ROB holds " + std::to_string(h->ObservedOccupied()) +
                  " macros");
    }
  }

  Require(retired == retire_target, "full-depth",
          "only " + std::to_string(retired) + " of " + std::to_string(retire_target) +
              " macros retired while the ROB was held at full occupancy");
  Require(h->ObservedAllocTotal() > entries, "full-depth",
          "the full-depth phase allocated only " + std::to_string(h->ObservedAllocTotal()) +
              " macros, so the buffer never wrapped");
  Require(h->ObservedSquashed() == 0, "full-depth",
          "the full-depth phase squashed a macro without issuing a flush");
}

// ----------------------------------------------------------------- phase 10
// Random stimulus, compared against the shadow on every cycle, with deliberate
// stale, duplicate and out-of-range completions mixed in.
void PhaseRandom(Harness* h, uint64_t seed, uint32_t cycles) {
  h->Phase("random-soak");
  mosaic::Rng rng(seed ^ 0x9e3779b97f4a7c15ull);
  const uint32_t entries = h->Entries();
  const uint32_t max_uops = h->MaxUops();
  uint32_t next_tag = 1;

  for (uint32_t n = 0; n < cycles; n++) {
    const std::vector<uint32_t> live = h->LiveIndices();
    Stim s;

    // Allocation is attempted less often once the ROB is full, but it is still
    // attempted: a soak that never offers an allocation into a full ROB never
    // observes the refusal, and the refusal is a documented behaviour rather
    // than something the absence of traffic can stand in for.
    const bool try_alloc = (h->ObservedOccupied() < entries) ? rng.Chance(70)
                                                            : rng.Chance(25);
    if (try_alloc) {
      s.alloc_valid = true;
      s.alloc_tag = next_tag;
      s.alloc_pc = 0x80000000ull + 4ull * next_tag;
      s.alloc_num_uops = 1 + rng.Below(max_uops);
      s.alloc_exc = rng.Chance(8);
      s.alloc_open = rng.Chance(10);
      if (rng.Chance(4)) s.alloc_num_uops = 0;
      ++next_tag;
    }

    if (!live.empty() && rng.Chance(80)) {
      const uint32_t idx = live[rng.Below(static_cast<uint32_t>(live.size()))];
      const Slot& slot = h->S(idx);
      s.cmp_valid = true;
      s.cmp_index = idx;
      s.cmp_gen = slot.gen;
      s.cmp_uop = rng.Below(max_uops);
      const uint32_t roll = rng.Below(100);
      if (roll < 20) {
        s.cmp_gen = slot.gen ^ (1u + rng.Below(15));  // a generation nobody holds
      } else if (roll < 30) {
        s.cmp_index = rng.Below(entries);             // may be a dead slot
      }
      s.cmp_exc = rng.Chance(6);
    } else if (rng.Chance(10)) {
      s.cmp_valid = true;
      s.cmp_index = rng.Below(entries);
      s.cmp_gen = rng.Below(64);
      s.cmp_uop = rng.Below(max_uops);
    }

    if (!live.empty() && rng.Chance(25)) {
      const uint32_t idx = live[rng.Below(static_cast<uint32_t>(live.size()))];
      s.close_valid = true;
      s.close_index = idx;
      s.close_gen = h->S(idx).gen;
      if (rng.Chance(30)) s.close_gen ^= 1u;
    }

    s.retire_req = rng.Chance(40);
    s.obs_index = rng.Below(entries);
    if (rng.Chance(1)) s.flush = true;

    h->Cycle(s, /*rst=*/false, /*sweep=*/(n % 97) == 0);
  }

  // The soak must actually have exercised what it claims to. A random campaign
  // that never reached capacity, never wrapped and never produced a duplicate
  // would pass while testing nothing.
  const Harness::Counters& c = h->counters();
  Require(h->ObservedAllocTotal() > entries, "random-soak",
          "the soak allocated only " + std::to_string(h->ObservedAllocTotal()) +
              " macros, which cannot wrap a " + std::to_string(entries) +
              "-entry ROB");
  Require(c.flushes > 0, "random-soak", "the soak never flushed");
  Require(c.accepted > 0, "random-soak", "the soak never accepted a completion");
  Require(c.duplicates > 0, "random-soak", "the soak never produced a duplicate");
  Require(c.stale_completions > 0, "random-soak",
          "the soak never produced a rejected completion");
  Require(c.bad_uops > 0, "random-soak",
          "the soak never produced an out-of-range completion");
  Require(c.full_refusals > 0, "random-soak", "the soak never hit capacity");
  Require(c.exceptional > 0, "random-soak",
          "the soak never produced an exceptional macro");
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
  Vmosaic_rob_tb dut;

  std::string detail;
  bool passed = true;
  try {
    // The geometry outputs are constants derived from the instance parameters, so
    // one evaluation is enough to read them and no reset is needed first. The
    // shadow is sized from these numbers and from nothing else.
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    const uint32_t entries = dut.o_rob_entries_o;
    const uint32_t index_w = dut.o_index_w_o;
    const uint32_t max_uops = dut.o_max_uops_o;
    const uint32_t id_w = dut.o_id_w_o;
    const uint32_t pc_w = dut.o_pc_w_o;
    const uint32_t num_uops_w = dut.o_num_uops_w_o;

    Require(entries > 0 && max_uops > 0 && index_w > 0, "geometry",
            "the DUT reported a zero depth, so the shadow cannot be sized");
    Require(pc_w == 64, "geometry",
            "expected a 64-bit PC, the DUT reports " + std::to_string(pc_w));
    Require(entries == (1u << index_w), "geometry",
            "the shadow models the never-truncates elaboration only: the DUT "
            "reports " + std::to_string(entries) + " entries in a " +
                std::to_string(index_w) + "-bit index space");
    Require(id_w == 2 * index_w, "geometry",
            "the identity width is not twice the slot index width: id_w = " +
                std::to_string(id_w) + ", index_w = " + std::to_string(index_w));
    Require(id_w <= 32 && num_uops_w <= 32 && pc_w <= 64, "geometry",
            "a driver-facing field is wider than the testbench interface");

    ShadowRob shadow(entries, max_uops, index_w, id_w, pc_w, num_uops_w);
    Harness harness(&dut, &clk, options.max_cycles, &shadow);

    // Phase order is a deliberate choice. Each phase resets first and owns one
    // mechanism, and a run stops at the first failure, so the order decides
    // *which* phase reports a given defect. The directed phases therefore run
    // before the two that are sensitive to everything.
    harness.Phase("reset-state");
    harness.Fresh();
    PhaseResetState(&harness);

    harness.Phase("out-of-order-children");
    harness.Fresh();
    PhaseOutOfOrder(&harness);

    harness.Phase("duplicate");
    harness.Fresh();
    PhaseDuplicate(&harness);

    harness.Phase("wrap-generation");
    harness.Fresh();
    PhaseWrapGeneration(&harness);

    harness.Phase("full");
    harness.Fresh();
    PhaseFull(&harness);

    harness.Phase("flush");
    harness.Fresh();
    PhaseFlush(&harness);

    harness.Phase("exception");
    harness.Fresh();
    PhaseException(&harness);

    harness.Phase("close-gate");
    harness.Fresh();
    PhaseCloseGate(&harness);

    harness.Phase("full-depth");
    harness.Fresh();
    PhaseFullDepth(&harness, 3 * entries);

    harness.Phase("random-soak");
    harness.Fresh();
    PhaseRandom(&harness, options.seed, 12000);

    const Harness::Counters& c = harness.counters();
    detail = "rob contract holds: " + std::to_string(harness.comparisons()) +
             " shadow comparisons, " + std::to_string(harness.sweep_checks()) +
             " of them across every slot of the buffer, over " +
             std::to_string(harness.cycles()) + " cycles and " +
             std::to_string(harness.allocations()) + " accepted allocations; soak: " +
             std::to_string(c.accepted) + " accepted, " +
             std::to_string(c.duplicates) + " duplicate, " +
             std::to_string(c.stale_completions) + " stale, " +
             std::to_string(c.bad_uops) + " out-of-range, " +
             std::to_string(c.full_refusals) + " refused-at-capacity, " +
             std::to_string(c.exceptional) + " exceptional, " +
             std::to_string(c.flushes) + " flushes, seed " +
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
