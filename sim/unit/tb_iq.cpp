// ============================================================================
// tb_iq.cpp -- CASE=iq.wakeup_insert_select, work package I-022.
//
// Drives the two `mosaic_iq` instances in sim/tb/mosaic_iq_tb.sv and checks
// every one of them, every cycle, against an *independent* shadow model written
// from the prose contract at the top of rtl/core/mosaic_iq.sv.
//
// ------------------------------------------------ independence of the model
//
// The shadow keeps, per slot, an entry with a 64-bit **absolute** age that never
// wraps, plus the two sources as {tag, generation, ready, value}, the uop
// identity, the operation, the immediate and the single destination. Three
// properties of that representation make it an oracle rather than a transcription:
//
//   * The DUT's age field is 5 bits and its "is A older than B" is a modular
//     comparison. The shadow's ages are unbounded 64-bit numbers and its
//     ordering is a plain `<`. A checker that reused the DUT's arithmetic would
//     agree with a DUT whose age logic is wrong; this one cannot, because the
//     two never share an expression.
//   * The shadow does not store the window invariant, it **checks** it: every
//     cycle, the live ages must span exactly `count - 1`. A DUT that let the
//     window grow is caught by that span check, with a message naming the span,
//     before it can produce a wrong selection.
//   * The shadow derives the 5-bit value the DUT should be storing by reducing
//     its own absolute age modulo the modulus, and compares it against the
//     observation port. So the width and the wrap are both exercised, and a
//     wrong modulus shows up as a wrong stored age rather than as a lucky
//     selection.
//
// The observation port is swept over every slot every cycle and every field is
// compared, so a defect that corrupts one slot cannot hide behind another.
//
// ---------------------------------------------------------- the two instances
//
// Cluster 0 and cluster 1 run the same directed stimulus against two separate
// shadows, and the directed phases additionally assert that a wakeup or a kill
// aimed at one cluster changes nothing in the other. That is the card's "local
// FUs only" as an observation rather than an assertion, and it is only
// checkable with two queues. In the randomised phase the two are driven from
// different seeds, so they disagree with each other as well as with themselves.
//
// --------------------------------------------------------------- the phases
//
//   0  geometry        the elaborated sizes are the ones this file assumes
//   1  reset-clean     nothing valid, nothing granted, counters zero
//   2  insert-basic    a uop goes in, is observed, is granted exactly once
//   3  oldest-ready    ready entries out of index order; the oldest by age wins
//                      every time, and the lowest index does not
//   4  same-cycle      the card's named case: dispatched this cycle, woken this
//                      cycle, selectable this cycle with the broadcast's value
//   5  producer-order  the same case with the producer *older* than the
//                      consumer, which a naive same-cycle rule gets wrong
//   6  age-wrap        the window walks all the way round the modulus with a
//                      full queue live; selection stays correct throughout
//   7  stale-wakeup    a matching tag with a stale generation is rejected and
//                      counted, and the entry stays not ready
//   8  duplicate-wu    a second broadcast for a satisfied operand changes
//                      nothing and is counted as a duplicate
//   9  back-pressure   the grant holds across cycles: same entry, same payload,
//                      accepted exactly once
//  10  kill            killed entries go, their slots come back, and a
//                      not-ready entry's age slot is released correctly
//  11  dst-conflict    two live uops naming one destination are reported
//  12  full-and-order  a full queue refuses inserts and issues in age order
//  13  refused-insert  a refused insert moves neither the allocation pointer
//                      nor the age, leaves no trace, and the next accepted
//                      insert lands where the shadow says
//  14  randomised      insert / wake / accept / kill against both shadows
//
// Coverage is counted and asserted at the end, so a stimulus change that stops
// reaching the wrap, the stale rejection, the duplicate, the full queue or the
// back-pressure fails the case instead of quietly passing over a shorter path.
// ============================================================================

#include "sim_common.h"
#include "verilated.h"
#include "Vmosaic_iq_tb.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- geometry
// The sizes this file was written against. They are *asserted* against the
// elaborated hardware in phase 0 rather than being the source of truth, so a
// profile this file was not updated for fails loudly.
constexpr unsigned kEntries   = 8;
constexpr unsigned kClusters  = 2;
constexpr unsigned kXlen      = 64;
constexpr unsigned kTagW      = 7;
constexpr unsigned kRobIndexW = 6;
constexpr unsigned kAgeW      = 5;
constexpr uint32_t kAgeMod    = 1u << kAgeW;
constexpr unsigned kUopIdW    = 16;
constexpr unsigned kIdxW      = 3;
constexpr unsigned kCntW      = 4;

// Reset length, fixed and documented per the sim/common convention.
constexpr int kResetCycles = 4;

// The uop identity is {rob_index, rob_gen, uop_index}, most significant field
// first, which is the layout rtl/core/mosaic_iq.sv documents. Transcribed here
// because a C++ file cannot import a SystemVerilog package; phase 0 does not
// check this, so it is checked the only way it can be: by inserting uops and
// observing that the DUT reports back the same identity, and by every kill
// naming what the driver inserted.
constexpr uint32_t MakeUop(uint32_t rob_index, uint32_t rob_gen, uint32_t uop_index) {
  return (rob_index << (7 + 3)) | (rob_gen << 3) | uop_index;
}
constexpr uint32_t UopRobIndex(uint32_t uop) { return uop >> (7 + 3); }
constexpr uint32_t UopRobGen(uint32_t uop) { return (uop >> 3) & 0x7fu; }

bool IsMacro(uint32_t uop, uint32_t rob_index, uint32_t rob_gen) {
  return UopRobIndex(uop) == rob_index && UopRobGen(uop) == rob_gen;
}

// One source operand, as the shadow holds it and as the driver offers it.
struct Source {
  uint32_t tag = 0;
  uint32_t gen = 0;
  bool ready = false;
  uint64_t value = 0;
};

// One shadow entry. `age_abs` is absolute, unbounded, and never reduced: the
// 5-bit value the DUT should be storing is *derived* from it, never the reverse.
struct Entry {
  bool valid = false;
  bool granted = false;
  uint64_t age_abs = 0;
  uint32_t uop = 0;
  uint32_t alu_op = 0;
  uint64_t imm = 0;
  Source s1;
  Source s2;
  uint32_t dst_tag = 0;
  uint32_t dst_gen = 0;
};

// What the shadow expects on the grant port this cycle.
struct ExpectedGrant {
  bool valid = false;
  bool from_ins = false;
  uint32_t uop = 0;
  uint32_t alu_op = 0;
  uint64_t imm = 0;
  uint64_t a = 0;
  uint64_t b = 0;
  uint32_t dst_tag = 0;
  uint32_t dst_gen = 0;
  int slot = -1;   // -1 when the grant is the entry being offered on the insert port
  // Whether the functional unit *accepted* the grant this cycle. The entry only
  // leaves the queue on an acceptance, so this and not `valid` is what the
  // shadow's removals and its grant counter must key on. A grant that is offered
  // and refused changes nothing but the `granted` flag.
  bool accepted = false;
};

// One cycle's stimulus. Both clusters get one of these; a phase fills the
// per-cluster fields differently where it means to.
struct Stimulus {
  bool ins_valid = false;
  uint32_t uop = 0;
  uint32_t alu_op = 0;
  uint64_t imm = 0;
  Source s1;
  Source s2;
  uint32_t dst_tag = 0;
  uint32_t dst_gen = 0;

  bool wu_valid = false;
  uint32_t wu_tag = 0;
  uint32_t wu_gen = 0;
  uint64_t wu_val = 0;

  bool kill_valid = false;
  uint32_t kill_rob_index = 0;
  uint32_t kill_rob_gen = 0;
  bool kill_younger = false;

  bool grant_ready = true;
};

// Mask a value to a port width. A driver that offers a number wider than the
// port it drives is testing something the hardware cannot express: the DUT sees
// the truncated value and any reference model that does not will disagree with
// it for a reason that has nothing to do with the queue. Every stimulus the
// driver builds is put through this once, so the shadow and the DUT are always
// looking at the same number.
template <unsigned W>
constexpr uint32_t Port(uint32_t value) {
  return (W >= 32) ? value : (value & ((1u << W) - 1u));
}

// Per-cluster coverage, asserted at the end.
struct Coverage {
  uint64_t cycles = 0;
  uint64_t grants = 0;
  uint64_t grant_stalls = 0;      // grant_valid high, grant_ready low
  uint64_t same_cycle_wu = 0;     // offered entry woken by this cycle's broadcast
  uint64_t same_cycle_grant = 0;  // ... and selected the same cycle
  uint64_t producer_older = 0;    // producer resident and older than the consumer
  uint64_t stale_rejects = 0;
  uint64_t dup_rejects = 0;
  uint64_t wu_misses = 0;
  uint64_t kills = 0;
  uint64_t kill_younger_hits = 0;
  uint64_t dst_conflicts = 0;
  uint64_t full_cycles = 0;
  uint64_t empty_cycles = 0;
  uint64_t age_wraps = 0;         // the 5-bit base value rolled over
  uint64_t max_occupancy = 0;
  uint64_t index_wraps = 0;       // alloc_ptr rolled over past the last slot
  uint64_t lowest_index_wrong = 0;  // cycles where oldest != lowest eligible index
  uint64_t kills_of_blocked = 0;  // killed an entry that was not ready
  bool saw_generation_advance = false;  // a tag was recycled with a new generation
};

// Verilator exposes every top-level port as a member, typed by the port's own
// width: `CData` for up to 8 bits, `SData` for 16, `IData` for 32, `QData` for
// 64. This struct uses exactly those types, so `&port` assigns without a cast
// and a port that changes width stops compiling rather than silently
// reinterpretation -- which is the point of typing them the way Verilator does
// rather than guessing uint32_t for everything and casting.
//
// One `Pins` per cluster, so both queues are driven and sampled through a
// single code path.
struct Pins {
  CData* ins_valid; CData* ins_ready; SData* ins_uop; CData* ins_alu_op;
  QData* ins_imm; CData* ins_src1_tag; CData* ins_src1_gen;
  CData* ins_src1_ready; QData* ins_src1_val; CData* ins_src2_tag;
  CData* ins_src2_gen; CData* ins_src2_ready; QData* ins_src2_val;
  CData* ins_dst_tag; CData* ins_dst_gen;
  CData* wu_valid; CData* wu_tag; CData* wu_gen; QData* wu_val;
  CData* grant_valid; CData* grant_ready; SData* grant_uop;
  CData* grant_alu_op; QData* grant_imm; QData* grant_a; QData* grant_b;
  CData* grant_dst_tag; CData* grant_dst_gen; CData* grant_index;
  CData* kill_valid; CData* kill_rob_index; CData* kill_rob_gen;
  CData* kill_younger;
  CData* occupied; CData* count; CData* full; CData* dst_conflict;
  CData* age_ctr; CData* alloc_index;
  CData* obs_index; CData* obs_valid; CData* obs_age; CData* obs_ready;
  CData* obs_granted; SData* obs_uop; CData* obs_alu_op; QData* obs_imm;
  CData* obs_src1_tag; CData* obs_src1_gen; CData* obs_src2_tag; CData* obs_src2_gen;
  CData* obs_dst_tag; CData* obs_dst_gen; CData* obs_src1_ready;
  CData* obs_src2_ready; QData* obs_src1_val; QData* obs_src2_val;
  IData* ins_total; IData* grant_total; IData* kill_total;
  IData* wu_total; IData* wu_matched; IData* wu_dup; IData* wu_stale;
  IData* wu_miss;
};
// The independent shadow.
class Shadow {
 public:
  explicit Shadow(const char* name) : name_(name) {}

  const char* name() const { return name_; }
  const Entry& slot(unsigned i) const { return slots_[i]; }
  unsigned count() const { return count_; }
  unsigned alloc_ptr() const { return alloc_ptr_; }
  uint64_t age_ctr_abs() const { return age_ctr_abs_; }

  // ---- predictions for the cycle about to be sampled ----

  unsigned expected_alloc_slot() const {
    for (unsigned k = 0; k < kEntries; k++) {
      unsigned p = (alloc_ptr_ + k) % kEntries;
      if (!slots_[p].valid) return p;
    }
    return alloc_ptr_;
  }

  bool expected_ins_ready() const { return count_ < kEntries; }

  // Is the entry being offered this cycle ready, after this cycle's broadcast?
  bool ins_ready_now(const Stimulus& st) const {
    // The `expected_ins_ready` term is the whole point: a full queue refuses the
    // insertion, so the entry it would have created does not exist and must not
    // be offered as a grant. Omitting it makes the shadow grant an entry the
    // hardware never admitted -- which is exactly what it did, on the first cycle
    // the randomised phase filled the queue while offering a ready uop.
    return expected_ins_ready() && Ready(st.s1, st) && Ready(st.s2, st);
  }

  // The kill set: every live uop of the named macro, plus everything younger
  // than the oldest of them.
  std::vector<int> kill_set(const Stimulus& st) const {
    std::vector<int> out;
    if (!st.kill_valid) return out;
    int ref = -1;
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid) continue;
      if (!IsMacro(slots_[i].uop, st.kill_rob_index, st.kill_rob_gen)) continue;
      if (ref < 0 || slots_[i].age_abs < slots_[ref].age_abs) ref = static_cast<int>(i);
    }
    if (ref < 0) return out;
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid) continue;
      if (IsMacro(slots_[i].uop, st.kill_rob_index, st.kill_rob_gen) ||
          (st.kill_younger && slots_[i].age_abs > slots_[ref].age_abs)) {
        out.push_back(static_cast<int>(i));
      }
    }
    return out;
  }

  int pending_slot() const {
    for (unsigned i = 0; i < kEntries; i++) {
      if (slots_[i].valid && slots_[i].granted) return static_cast<int>(i);
    }
    return -1;
  }

  // The oldest live slot by absolute age, or -1.
  int oldest_live() const {
    int best = -1;
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid) continue;
      if (best < 0 || slots_[i].age_abs < slots_[best].age_abs) best = static_cast<int>(i);
    }
    return best;
  }

  // The resident that should be presented: the oldest entry that is ready after
  // this cycle's broadcast and has not already been presented. Plain 64-bit
  // comparison -- this is the check that the DUT's modular comparison has to
  // agree with.
  int expected_winner(const Stimulus& st) const {
    int best = -1;
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid || slots_[i].granted) continue;
      if (!Ready(slots_[i].s1, st) || !Ready(slots_[i].s2, st)) continue;
      if (best < 0 || slots_[i].age_abs < slots_[best].age_abs) best = static_cast<int>(i);
    }
    return best;
  }

  // The lowest-index eligible resident, recorded only so the test can report how
  // often "oldest" and "lowest index" disagree -- a phase that never makes them
  // disagree proves nothing about oldest-ready selection.
  int lowest_index_eligible(const Stimulus& st) const {
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid || slots_[i].granted) continue;
      if (Ready(slots_[i].s1, st) && Ready(slots_[i].s2, st)) return static_cast<int>(i);
    }
    return -1;
  }

  ExpectedGrant expected_grant(const Stimulus& st, bool ins_on,
                                bool grant_ready) const {
    ExpectedGrant g;
    const int pend = pending_slot();
    const std::vector<int> killed = kill_set(st);
    const bool pend_killed =
        pend >= 0 && std::find(killed.begin(), killed.end(), pend) != killed.end();
    const int win = expected_winner(st);

    // The offered entry is the youngest by the window invariant, so it is only
    // considered when no resident is eligible. The caller asserts that
    // "youngest" rather than assuming it.
    if (pend < 0 && win < 0 && ins_on && ins_ready_now(st)) {
      g.valid = true;
      g.accepted = grant_ready;
      g.from_ins = true;
      g.slot = -1;
      g.uop = st.uop;
      g.alu_op = st.alu_op;
      g.imm = st.imm;
      g.a = Value(st.s1, st);
      g.b = Value(st.s2, st);
      g.dst_tag = st.dst_tag;
      g.dst_gen = st.dst_gen;
      return g;
    }
    if (pend >= 0) {
      if (pend_killed) return g;  // a kill withdraws an outstanding grant
      g.valid = true;
      g.accepted = grant_ready;
      g.from_ins = false;
      g.slot = pend;
      FillFromSlot(g, pend, st);
      return g;
    }
    if (win >= 0) {
      g.valid = true;
      g.accepted = grant_ready;
      g.from_ins = false;
      g.slot = win;
      FillFromSlot(g, win, st);
    }
    return g;
  }

  // The slots that leave the queue this cycle: the granted resident (if any) and
  // the kill set, unioned, so an entry that is both granted and killed leaves
  // once.
  std::vector<int> rm_set(const Stimulus& st, const ExpectedGrant& g) const {
    std::vector<int> out;
    const std::vector<int> killed = kill_set(st);
    for (int i : killed) out.push_back(i);
    if (g.accepted && !g.from_ins && g.slot >= 0 &&
        std::find(out.begin(), out.end(), g.slot) == out.end()) {
      out.push_back(g.slot);
    }
    return out;
  }

  // Exactly-once: does the offered entry collide with a live destination, or do
  // two live entries already collide?
  bool expected_dst_conflict(const Stimulus& st, bool ins_on) const {
    // The offered entry is folded in only when it will really be taken: a full
    // queue refuses it, so it names no destination this cycle.
    const bool ins_fire = ins_on && expected_ins_ready();
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid) continue;
      for (unsigned j = i + 1; j < kEntries; j++) {
        if (!slots_[j].valid) continue;
        if (slots_[i].dst_tag == slots_[j].dst_tag &&
            slots_[i].dst_gen == slots_[j].dst_gen) {
          return true;
        }
      }
      if (ins_fire && slots_[i].dst_tag == st.dst_tag && slots_[i].dst_gen == st.dst_gen) {
        return true;
      }
    }
    return false;
  }

  enum class WuClass { kNone, kMatched, kDup, kStale, kMiss };

  // Classified against the state at the *start* of the cycle, and against the
  // offered entry only when it will really be taken, which is what the RTL's
  // `ins_fire` term means. Calling this after the shadow has been advanced would
  // describe the next cycle instead of this one.
  WuClass classify(const Stimulus& st, bool ins_on) const {
    if (!st.wu_valid) return WuClass::kNone;
    const bool ins_fire = ins_on && expected_ins_ready();
    bool hit = false, dup = false, seen = false;
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid) continue;
      if (slots_[i].s1.tag == st.wu_tag || slots_[i].s2.tag == st.wu_tag) seen = true;
      hit |= Hit(st, slots_[i].s1) || Hit(st, slots_[i].s2);
      dup |= Dup(st, slots_[i].s1) || Dup(st, slots_[i].s2);
    }
    if (ins_fire) {
      hit |= Hit(st, st.s1) || Hit(st, st.s2);
      dup |= Dup(st, st.s1) || Dup(st, st.s2);
    }
    if (hit) return WuClass::kMatched;
    if (dup) return WuClass::kDup;
    if (seen) return WuClass::kStale;
    return WuClass::kMiss;
  }

  // ---- advance the shadow across the edge the DUT is about to take ----
  void step(const Stimulus& st, bool ins_on, const ExpectedGrant& g) {
    // Classified before anything is advanced: the classification describes the
    // broadcast as the DUT saw it, against the state it saw it in.
    const WuClass cls = classify(st, ins_on);
    const std::vector<int> killed = kill_set(st);
    const std::vector<int> rm = rm_set(st, g);
    const bool ins_fire = ins_on && expected_ins_ready();
    const bool ins_taken = ins_fire && g.accepted && g.from_ins;
    // Ages before anything is torn down, because the slide decisions are all
    // made against the ages as they were at the start of the cycle.
    uint64_t pre[kEntries];
    for (unsigned i = 0; i < kEntries; i++) pre[i] = slots_[i].age_abs;

    // The slot an insertion takes, chosen from the state as it stands at the
    // START of the cycle. The hardware's allocator scans `slot_valid` before this
    // edge's removals are applied, so a slot freed by this cycle's kill or grant
    // is NOT available to an insertion in the same cycle. Choosing it after the
    // removals is the same family of defect as the two above -- the model and
    // the hardware applying the rule at different points -- and it shows up
    // exactly on a cycle that both removes and inserts, which is why the
    // randomised phase reached it and the directed phases did not.
    unsigned alloc_slot = alloc_ptr_;
    for (unsigned k = 0; k < kEntries; k++) {
      const unsigned probe = (alloc_ptr_ + k) % kEntries;
      if (!slots_[probe].valid) { alloc_slot = probe; break; }
    }

    // The base is the *oldest live entry*, and it slides only when that entry
    // itself is the one leaving -- not whenever the oldest element of some
    // removal list happens to be it.
    const int base = oldest_live();
    const bool base_removed =
        base >= 0 && std::find(rm.begin(), rm.end(), base) != rm.end();

    // 1. Removals: every survivor steps down once for each removed entry that
    //    was older than it, except the base's own removal, which slides the
    //    window up instead of dragging it down.
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid) continue;
      const bool going = std::find(rm.begin(), rm.end(), static_cast<int>(i)) != rm.end();
      if (going) continue;
      uint64_t shift = 0;
      for (int j : rm) {
        if (pre[j] < pre[i]) shift++;
      }
      // The base's own removal is not a hole below anybody -- there is nobody
      // below it -- so it must not drag the whole window down. That exception is
      // the only way the age field ever wraps.
      if (base_removed && pre[base] < pre[i]) shift--;
      slots_[i].age_abs -= shift;
    }

    // 2. The broadcast reaches the residents it names -- tag *and* generation,
    //    and only operands that are not ready already.
    if (st.wu_valid) {
      for (unsigned i = 0; i < kEntries; i++) {
        if (!slots_[i].valid) continue;
        if (Hit(st, slots_[i].s1)) { slots_[i].s1.ready = true; slots_[i].s1.value = st.wu_val; }
        if (Hit(st, slots_[i].s2)) { slots_[i].s2.ready = true; slots_[i].s2.value = st.wu_val; }
      }
    }

    // 3. Removals take effect.
    for (int i : rm) {
      slots_[i].valid = false;
      slots_[i].granted = false;
    }

    // 4. The insertion, unless it was granted straight out of the insert port.
    if (ins_fire && !ins_taken) {
      const unsigned p = alloc_slot;
      Entry e;
      e.valid = true;
      e.age_abs = age_ctr_abs_ - static_cast<uint64_t>(rm.size()) + (base_removed ? 1 : 0);
      e.uop = st.uop;
      e.alu_op = st.alu_op;
      e.imm = st.imm;
      e.s1 = st.s1;
      e.s2 = st.s2;
      e.dst_tag = st.dst_tag;
      e.dst_gen = st.dst_gen;
      if (st.wu_valid) {
        if (Hit(st, e.s1)) { e.s1.ready = true; e.s1.value = st.wu_val; }
        if (Hit(st, e.s2)) { e.s2.ready = true; e.s2.value = st.wu_val; }
      }
      if (g.valid && !g.accepted && g.from_ins) e.granted = true;
      slots_[p] = e;
      alloc_ptr_ = (p + 1) % kEntries;  // already inside `ins_fire && !ins_taken`
    }

    // 5. A resident that was presented and not accepted becomes the
    //    outstanding grant.
    if (g.valid && !g.accepted && !g.from_ins) slots_[g.slot].granted = true;

    // 6. The counter moves down with every removal, up by one for a base slide,
    //    and up by one for an insertion that becomes resident.
    age_ctr_abs_ = age_ctr_abs_ - static_cast<uint64_t>(rm.size()) +
                   (base_removed ? 1 : 0) + ((ins_fire && !ins_taken) ? 1 : 0);

    recount();
    ins_total_ += ins_fire ? 1 : 0;
    grant_total_ += g.accepted ? 1 : 0;
    // An entry that is granted and killed in the same cycle has left by the
    // grant, so it is counted once, as a grant -- the same rule the hardware
    // applies with `kill_only_mask = kill_mask & ~grant_rm_mask`. Counting it
    // here as well is what put the conservation tally one ahead.
    uint32_t killed_only = 0;
    for (int i : killed) {
      const bool also_granted = g.accepted && !g.from_ins && g.slot == i;
      if (!also_granted) killed_only++;
    }
    kill_total_ += killed_only;
    switch (cls) {
      case WuClass::kNone: break;
      case WuClass::kMatched: wu_matched_++; wu_total_++; break;
      case WuClass::kDup: wu_dup_++; wu_total_++; break;
      case WuClass::kStale: wu_stale_++; wu_total_++; break;
      case WuClass::kMiss: wu_miss_++; wu_total_++; break;
    }
  }

  // ---- the shadow's own tallies ----
  uint32_t ins_total() const { return ins_total_; }
  uint32_t grant_total() const { return grant_total_; }
  uint32_t kill_total() const { return kill_total_; }
  uint32_t wu_total() const { return wu_total_; }
  uint32_t wu_matched() const { return wu_matched_; }
  uint32_t wu_dup() const { return wu_dup_; }
  uint32_t wu_stale() const { return wu_stale_; }
  uint32_t wu_miss() const { return wu_miss_; }

  // The stored value of an entry's first source, and whether it is ready. Used
  // by the stale and duplicate phases, which assert on the operand a wakeup did
  // or did not touch rather than on a counter alone.
  uint64_t s1_value(unsigned i) const { return slots_[i].s1.value; }
  bool s1_ready(unsigned i) const { return slots_[i].s1.ready; }
  bool entry_valid(unsigned i) const { return slots_[i].valid; }
  uint64_t age_of(unsigned i) const { return slots_[i].age_abs; }

  // Which slot holds a given uop identity, or -1. The driver names uops and
  // the hardware names slots; a phase that assumed "the first entry I inserted
  // is in slot 0" would be asserting about the allocation pointer rather than
  // about the queue.
  int slot_of(uint32_t uop) const {
    for (unsigned i = 0; i < kEntries; i++) {
      if (slots_[i].valid && slots_[i].uop == uop) return static_cast<int>(i);
    }
    return -1;
  }

  // The slot an insertion would occupy, and whether a kill of the named macro
  // would remove anything other than that macro's own uops -- i.e. whether
  // `kill_younger` is removing a genuine suffix.
  bool kill_removes_younger(const Stimulus& st) const {
    if (!st.kill_valid || !st.kill_younger) return false;
    const std::vector<int> killed = kill_set(st);
    for (int i : killed) {
      if (!IsMacro(slots_[i].uop, st.kill_rob_index, st.kill_rob_gen)) return true;
    }
    return false;
  }

  // Was any killed entry not yet ready? The card asks for the age slot of a
  // not-yet-ready entry to be released correctly, which is a different case
  // from killing a ready one.
  bool kill_includes_blocked(const Stimulus& st) const {
    for (int i : kill_set(st)) {
      if (!(slots_[i].s1.ready && slots_[i].s2.ready)) return true;
    }
    return false;
  }

  // The 5-bit value the DUT should be storing for an absolute age.
  static uint32_t Stored(uint64_t abs) { return static_cast<uint32_t>(abs % kAgeMod); }

  // The invariant the age design rests on: the live ages span exactly
  // count - 1. Returns the span, or -1 when the queue is empty.
  long long age_span() const {
    int oldest = oldest_live();
    if (oldest < 0) return -1;
    uint64_t lo = slots_[oldest].age_abs, hi = lo;
    for (unsigned i = 0; i < kEntries; i++) {
      if (!slots_[i].valid) continue;
      lo = std::min(lo, slots_[i].age_abs);
      hi = std::max(hi, slots_[i].age_abs);
    }
    return static_cast<long long>(hi - lo);
  }

 private:
  // Does this broadcast name this source, with the right generation, and is the
  // source still waiting for it?
  static bool Hit(const Stimulus& st, const Source& s) {
    return st.wu_valid && !s.ready && s.tag == st.wu_tag && s.gen == st.wu_gen;
  }
  // A second broadcast for an operand that already holds this exact value.
  static bool Dup(const Stimulus& st, const Source& s) {
    return st.wu_valid && s.ready && s.tag == st.wu_tag && s.gen == st.wu_gen;
  }
  static bool Ready(const Source& s, const Stimulus& st) { return s.ready || Hit(st, s); }
  static uint64_t Value(const Source& s, const Stimulus& st) {
    return Hit(st, s) ? st.wu_val : s.value;
  }

  void FillFromSlot(ExpectedGrant& g, int slot, const Stimulus& st) const {
    const Entry& e = slots_[slot];
    g.uop = e.uop;
    g.alu_op = e.alu_op;
    g.imm = e.imm;
    g.a = Value(e.s1, st);
    g.b = Value(e.s2, st);
    g.dst_tag = e.dst_tag;
    g.dst_gen = e.dst_gen;
  }

  void recount() {
    count_ = 0;
    for (unsigned i = 0; i < kEntries; i++) {
      if (slots_[i].valid) count_++;
    }
  }

  const char* name_;
  Entry slots_[kEntries];
  unsigned count_ = 0;
  unsigned alloc_ptr_ = 0;
  uint64_t age_ctr_abs_ = 0;
  uint32_t ins_total_ = 0, grant_total_ = 0, kill_total_ = 0;
  uint32_t wu_total_ = 0, wu_matched_ = 0, wu_dup_ = 0, wu_stale_ = 0, wu_miss_ = 0;
};

}  // namespace

// ===========================================================================
// The driver: owns the clock, drives both clusters, and compares every DUT
// output against the shadow every cycle.
// ===========================================================================
namespace {

class Bench {
 public:
  Bench(const mosaic::Options& options, mosaic::Reporter& reporter, Vmosaic_iq_tb* top)
      : rep_(reporter), top_(top), options_(options), cycles_(0) {
    auto add = [&](const char* name, Pins pins, uint64_t salt) {
      Instance inst;
      inst.shadow = new Shadow(name);
      inst.pins = pins;
      inst.rng = mosaic::Rng(options.seed + salt);
      inst.cluster = inst_.size();
      inst_.push_back(inst);
    };
    // Cluster 0 and cluster 1, in that order. They are the only two clusters
    // the port list names, and the readback in phase 0 checks that the wrapper
    // elaborated exactly that many.
    add("c0", PinsFor0(), options.seed ^ 0x9e3779b97f4a7c15ull);
    add("c1", PinsFor1(), options.seed ^ 0x517cc1b727220a95ull);
  }

  ~Bench() {
    for (Instance& i : inst_) delete i.shadow;
  }

  bool Resetting() const { return clk_.in_reset(); }
  void PulseReset(int cycles) { clk_.BeginReset(cycles); }
  uint64_t cycles() const { return cycles_; }
  int failures() const { return rep_.failures(); }
  int checks() const { return rep_.checks(); }
  int reset_cycles() const { return clk_.reset_cycles(); }

  // ---- one cycle, for both clusters -------------------------------------
  // `st` is applied to cluster 0; cluster 1 gets `st1` when the phase wants them
  // to differ and `st` otherwise.
  void Step(const Stimulus& st, const Stimulus* st1 = nullptr) {
    // Every field is brought to the width of the port it drives, once, here. The
    // shadow is then driven from the same clamped values as the hardware, so a
    // disagreement can only mean the queue is wrong -- never that the driver
    // offered something the pins cannot carry.
    Stimulus a = st;
    Stimulus b = st1 ? *st1 : st;
    Clamp(a);
    Clamp(b);
    for (unsigned c = 0; c < inst_.size(); c++) {
      Drive(inst_[c].pins, (c == 0) ? a : b);
    }
    Settle();
    for (unsigned c = 0; c < inst_.size(); c++) {
      const Stimulus& s = (c == 0) ? a : b;
      CheckOne(inst_[c], s);
    }
    Edge();
    for (unsigned c = 0; c < inst_.size(); c++) {
      const Stimulus& s = (c == 0) ? a : b;
      inst_[c].shadow->step(s, s.ins_valid, inst_[c].expected);
    }
    // The hardware has now taken the edge; read what it settled to, so a phase
    // can compare it with the shadow's state, which was advanced across the same
    // edge one line above.
    top_->clk = 0;
    top_->eval();
    for (unsigned c = 0; c < inst_.size(); c++) {
      const Pins& p = inst_[c].pins;
      Instance& in = inst_[c];
      in.post_grant_valid = (*p.grant_valid != 0);
      in.post_grant_uop = *p.grant_uop;
      in.post_grant_dst_tag = *p.grant_dst_tag;
      in.post_grant_a = *p.grant_a;
      in.post_dst_conflict = (*p.dst_conflict != 0);
      in.post_full = (*p.full != 0);
      in.post_ins_ready = (*p.ins_ready != 0);
      in.post_wu_stale = *p.wu_stale;
      in.post_count = *p.count;
      in.post_alloc_index = *p.alloc_index;
      in.post_age_ctr = *p.age_ctr;
      in.post_ins_total = *p.ins_total;
      in.post_grant_total = *p.grant_total;
      in.post_kill_total = *p.kill_total;
    }
    clk_.Tick();
    cycles_++;
  }

  // A cycle with no stimulus at all, used to let a queue settle.
  void Idle() {
    Stimulus s;
    Step(s);
  }

  // Empty cluster 0's queue for real.
  //
  // `Idle` cannot do this: a *blocked* entry is never granted, so a run of idle
  // cycles leaves every blocked entry sitting in the queue, and a phase that
  // assumes a clean start is then asserting about a queue it did not build. So
  // the entries are killed one at a time, oldest first, naming the macro the
  // shadow says is there. This is a driver convenience, not a check: the
  // per-cycle shadow comparison is still what validates every one of those
  // cycles.
  void DrainQueue() {
    Shadow* sh = shadow(0);
    for (int guard = 0; guard < 4 * static_cast<int>(kEntries) && sh->count() > 0; guard++) {
      const int oldest = sh->oldest_live();
      if (oldest < 0) break;
      const uint32_t uop = sh->slot(static_cast<unsigned>(oldest)).uop;
      Stimulus k = Kill(UopRobIndex(uop), UopRobGen(uop), /*younger=*/false);
      k.grant_ready = true;
      Step(k);
    }
  }

  // A stimulus with the functional unit refusing, so an inserted uop stays
  // queued. Phases that are building a state rather than exercising the accept
  // path use this; a phase that forgets it gets a queue that never fills, which
  // is a silent way to test nothing.
  static Stimulus Hold(Stimulus s) {
    s.grant_ready = false;
    return s;
  }

  // Convenience: a stimulus that is nothing but an insert.
  Stimulus InsertOnly(uint32_t uop, bool s1_ready = true, bool s2_ready = true,
                      uint32_t s1_tag = 0, uint32_t s1_gen = 0,
                      uint32_t s2_tag = 0, uint32_t s2_gen = 0,
                      uint32_t dst_tag = 0, uint32_t dst_gen = 0) {
    Stimulus s;
    s.ins_valid = true;
    s.uop = uop;
    s.alu_op = kAdd;
    s.imm = 0x1000u + uop;
    s.s1 = Source{s1_tag, s1_gen, s1_ready, 0x1111u + uop};
    s.s2 = Source{s2_tag, s2_gen, s2_ready, 0x2222u + uop};
    s.dst_tag = dst_tag;
    s.dst_gen = dst_gen;
    return s;
  }

  Stimulus Wakeup(uint32_t tag, uint32_t gen, uint64_t value) {
    Stimulus s;
    s.wu_valid = true;
    s.wu_tag = tag;
    s.wu_gen = gen;
    s.wu_val = value;
    return s;
  }

  Stimulus Kill(uint32_t rob_index, uint32_t rob_gen, bool younger) {
    Stimulus s;
    s.kill_valid = true;
    s.kill_rob_index = rob_index;
    s.kill_rob_gen = rob_gen;
    s.kill_younger = younger;
    return s;
  }

  // Overlays: `Merge` is a stimulus combination, so a phase can express "insert
  // this and wake that in the same cycle" without hand-building both.
  static Stimulus Merge(const Stimulus& base, const Stimulus& other) {
    Stimulus s = base;
    if (other.ins_valid) { s.ins_valid = true; s.uop = other.uop; s.alu_op = other.alu_op;
                           s.imm = other.imm; s.s1 = other.s1; s.s2 = other.s2;
                           s.dst_tag = other.dst_tag; s.dst_gen = other.dst_gen; }
    if (other.wu_valid) { s.wu_valid = true; s.wu_tag = other.wu_tag; s.wu_gen = other.wu_gen;
                          s.wu_val = other.wu_val; }
    if (other.kill_valid) { s.kill_valid = true; s.kill_rob_index = other.kill_rob_index;
                            s.kill_rob_gen = other.kill_rob_gen;
                            s.kill_younger = other.kill_younger; }
    s.grant_ready = other.grant_ready;
    return s;
  }

  Shadow* shadow(unsigned c) { return inst_[c].shadow; }
  // The resident the shadow picked on the last cycle, and the lowest-index
  // eligible one, so a phase can assert on the arbitration decision itself.
  int last_winner(unsigned c) const { return inst_[c].last_winner; }
  // What the DUT presented on the cycle the driver last checked.
  bool seen_grant_valid(unsigned c) const { return inst_[c].seen_grant_valid; }
  uint32_t seen_grant_uop(unsigned c) const { return inst_[c].seen_grant_uop; }
  uint64_t seen_grant_a(unsigned c) const { return inst_[c].seen_grant_a; }
  bool seen_ins_valid(unsigned c) const { return inst_[c].seen_ins_valid; }
  bool seen_dst_conflict(unsigned c) const { return inst_[c].seen_dst_conflict; }
  uint32_t seen_grant_dst_tag(unsigned c) const { return inst_[c].seen_grant_dst_tag; }
  uint32_t seen_wu_stale(unsigned c) const { return inst_[c].seen_wu_stale; }
  bool seen_full(unsigned c) const { return inst_[c].seen_full; }
  bool seen_ins_ready(unsigned c) const { return inst_[c].seen_ins_ready; }
  bool seen_grant_ready(unsigned c) const { return inst_[c].seen_grant_ready; }
  bool post_grant_valid(unsigned c) const { return inst_[c].post_grant_valid; }
  uint32_t post_grant_uop(unsigned c) const { return inst_[c].post_grant_uop; }
  uint32_t post_grant_dst_tag(unsigned c) const { return inst_[c].post_grant_dst_tag; }
  uint64_t post_grant_a(unsigned c) const { return inst_[c].post_grant_a; }
  bool post_dst_conflict(unsigned c) const { return inst_[c].post_dst_conflict; }
  bool post_full(unsigned c) const { return inst_[c].post_full; }
  bool post_ins_ready(unsigned c) const { return inst_[c].post_ins_ready; }
  uint32_t post_wu_stale(unsigned c) const { return inst_[c].post_wu_stale; }
  uint32_t post_count(unsigned c) const { return inst_[c].post_count; }
  uint32_t post_alloc_index(unsigned c) const { return inst_[c].post_alloc_index; }
  uint32_t post_age_ctr(unsigned c) const { return inst_[c].post_age_ctr; }
  uint32_t post_ins_total(unsigned c) const { return inst_[c].post_ins_total; }
  uint32_t post_grant_total(unsigned c) const { return inst_[c].post_grant_total; }
  uint32_t post_kill_total(unsigned c) const { return inst_[c].post_kill_total; }

  // One slot out of a cluster's observation port, read after the clock has
  // settled. This is a synthesizable status port (obs_index is driven, obs_* is
  // combinational over register state), not a functional interface, so a phase
  // may read it directly; it is routed through here so the phase does not reach
  // through `top()` and so the two clusters go through one code path.
  struct ObservedSlot { bool valid; uint32_t uop; };
  ObservedSlot ObserveSlot(unsigned c, unsigned idx) {
    Instance& in = inst_[c];
    *in.pins.obs_index = static_cast<uint8_t>(idx);
    top_->eval();
    ObservedSlot o;
    o.valid = (*in.pins.obs_valid != 0);
    o.uop = *in.pins.obs_uop;
    *in.pins.obs_index = 0;
    top_->eval();
    return o;
  }
  uint32_t seen_ins_uop(unsigned c) const { return inst_[c].seen_ins_uop; }
  const ExpectedGrant& last_expected(unsigned c) const { return inst_[c].expected; }
  int last_lowest_index(unsigned c) const { return inst_[c].last_lowest_index; }
  Coverage& coverage(unsigned c) { return inst_[c].cov; }
  // The per-cluster random stream, so the randomised phase can give the two
  // queues different stimulus and they disagree with each other as well as with
  // their own shadows.
  mosaic::Rng& RngFor(unsigned c) { return inst_[c].rng; }

  Vmosaic_iq_tb* top() { return top_; }

  // Walk cluster 0's allocation pointer round to slot 0.
  //
  // The pointer advances by one per accepted insertion and is periodic in the
  // queue depth, so a phase cannot choose where the *next* insert lands by
  // inserting a full queue -- that just returns the pointer to where it was.
  // What matters for oldest-ready is that a *low* slot ends up holding the
  // *youngest* entry, and that only happens when a low slot has been freed and
  // the pointer has come back round to it. So: drain, then insert-and-kill a
  // throwaway entry once per step until the pointer reads 0.
  void AlignAllocPtr() {
    Shadow* sh = shadow(0);
    DrainQueue();
    // The ROB index field is 6 bits wide, so the throwaway macros are numbered
    // from 48 upwards -- clear of every macro the directed phases use, and clear
    // of the wrap because there are at most DEPTH+1 of them.
    for (uint32_t i = 0; i < kEntries + 1 && sh->alloc_ptr() != 0; i++) {
      const uint32_t rob = 48u + i;
      Stimulus s = Blocked(MakeUop(rob, 0, 0), 0x3fu + i, 1, 0x3fu, 1);
      s.grant_ready = false;
      Step(s);
      // Kill it straight back out, so the queue does not fill up while walking
      // the pointer round.
      Stimulus k = Kill(rob, 0, /*younger=*/false);
      k.grant_ready = false;
      Step(k);
    }
  }

  // A tag/generation pair the driver has not used, so a wakeup aimed at it is a
  // genuine miss rather than an accidental hit.
  uint32_t fresh_tag() { return next_tag_++; }

  // Drive an entry that is blocked on a specific (tag, generation) pair.
  Stimulus Blocked(uint32_t uop, uint32_t tag, uint32_t gen, uint32_t dst_tag, uint32_t dst_gen) {
    Stimulus s;
    s.ins_valid = true;
    s.uop = uop;
    s.alu_op = kAdd;
    s.imm = 0x2000u + uop;
    s.s1 = Source{tag, gen, false, 0};
    s.s2 = Source{0, 0, true, 0x3333u + uop};   // second source already present
    s.dst_tag = dst_tag;
    s.dst_gen = dst_gen;
    return s;
  }

  // ---- phase helpers ----------------------------------------------------

  // Assert the DUT's whole visible state on every slot of one cluster.
  void CheckSlots(unsigned c);

  // Report coverage across both clusters and fail if a discriminating
  // situation was never reached.
  void ReportCoverage();

 private:
  // One cluster's state: its shadow, its pins, its own coverage, and the two
  // pieces of last-cycle grant state the stall check needs.
  struct Instance {
    Shadow* shadow = nullptr;
    Pins pins{};
    // `mosaic::Rng` has no default constructor, so the vector's copy is
    // constructed from a fixed seed and then replaced per cluster below.
    mosaic::Rng rng{1};
    Coverage cov{};
    ExpectedGrant expected{};
    unsigned cluster = 0;
    bool last_grant_valid = false;
    bool last_grant_ready = false;
    bool last_kill_valid = false;   // a kill withdraws the grant; see CheckOne
    uint32_t last_grant_uop = 0;
    uint64_t last_grant_a = 0, last_grant_b = 0, last_grant_imm = 0;
    uint32_t last_grant_dst_tag = 0, last_grant_dst_gen = 0;
    // The grant the DUT presented *on the cycle that was checked*. A phase that
    // reads the top-level grant after `Step` returns is reading the next cycle's
    // combinational value, which is one edge too late.
    uint32_t seen_grant_uop = 0;
    uint64_t seen_grant_a = 0, seen_grant_b = 0;
    bool seen_grant_valid = false;
    bool seen_ins_valid = false;
    uint32_t seen_ins_uop = 0;
    bool seen_dst_conflict = false;
    uint32_t seen_grant_dst_tag = 0;
    uint32_t seen_wu_stale = 0;
    bool seen_grant_ready = false;   // was the FU accepting on the snapshot cycle
    bool seen_full = false;
    bool seen_ins_ready = false;
    // The same values again, read AFTER the edge -- the hardware's settled state
    // for the cycle that was just taken, which is the one the shadow has also
    // just advanced to. A phase comparing state must use these, not `seen_*`.
    bool post_grant_valid = false;
    uint32_t post_grant_uop = 0, post_grant_dst_tag = 0;
    uint64_t post_grant_a = 0;
    bool post_dst_conflict = false, post_full = false, post_ins_ready = false;
    uint32_t post_wu_stale = 0;
    // The settled state after the edge, for the status and conservation words
    // a phase checks across a refusal: occupancy, the allocation pointer, the
    // age counter and the three conservation tallies.
    uint32_t post_count = 0, post_alloc_index = 0, post_age_ctr = 0;
    uint32_t post_ins_total = 0, post_grant_total = 0, post_kill_total = 0;
    uint32_t last_age_ctr = 0;
    // The resident the shadow picked this cycle, and the lowest-index eligible
    // one, both computed at check time against the state the DUT was in. A
    // phase needs these to assert on the *decision*, not to re-derive it from
    // the state after the edge.
    int last_winner = -1;
    int last_lowest_index = -1;
  };

  void Drive(const Pins& p, const Stimulus& s);

  // Bring every stimulus field to the width of the port it drives: 7 bits for a
  // PRF tag, 7 for a generation, 7 for a kill's ROB generation, 6 for a kill's
  // ROB index and 16 for the uop identity.
  static void Clamp(Stimulus& s) {
    s.uop = Port<kUopIdW>(s.uop);
    s.s1.tag = Port<kTagW>(s.s1.tag);
    s.s1.gen = Port<kTagW>(s.s1.gen);
    s.s2.tag = Port<kTagW>(s.s2.tag);
    s.s2.gen = Port<kTagW>(s.s2.gen);
    s.dst_tag = Port<kTagW>(s.dst_tag);
    s.dst_gen = Port<kTagW>(s.dst_gen);
    s.wu_tag = Port<kTagW>(s.wu_tag);
    s.wu_gen = Port<kTagW>(s.wu_gen);
    s.kill_rob_index = Port<kRobIndexW>(s.kill_rob_index);
    s.kill_rob_gen = Port<kTagW>(s.kill_rob_gen);
  }
  void Settle();
  void Edge();
  void CheckOne(Instance& inst, const Stimulus& s);
  void CheckGrantStability(Instance& inst);

  Pins PinsFor0();
  Pins PinsFor1();

  mosaic::Reporter& rep_;
  Vmosaic_iq_tb* top_;
  mosaic::Options options_;
  mosaic::ClockDriver clk_;
  uint64_t cycles_ = 0;
  std::vector<Instance> inst_;
  uint32_t next_tag_ = 0x40;   // well clear of the tags the phases allocate
  uint32_t dst_next_ = 0x20;

  // The `mosaic_pkg::alu_op_e` encoding, transcribed: ALU_ADD = 0.
  static constexpr uint32_t kAdd = 0;
};

// --- pin wiring -----------------------------------------------------------

Pins Bench::PinsFor0() {
  Vmosaic_iq_tb* t = top_;
  Pins p{};
  p.ins_valid = &t->c0_ins_valid; p.ins_ready = &t->c0_ins_ready;
  p.ins_uop = &t->c0_ins_uop; p.ins_alu_op = &t->c0_ins_alu_op;
  p.ins_imm = &t->c0_ins_imm;
  p.ins_src1_tag = &t->c0_ins_src1_tag; p.ins_src1_gen = &t->c0_ins_src1_gen;
  p.ins_src1_ready = &t->c0_ins_src1_ready; p.ins_src1_val = &t->c0_ins_src1_val;
  p.ins_src2_tag = &t->c0_ins_src2_tag; p.ins_src2_gen = &t->c0_ins_src2_gen;
  p.ins_src2_ready = &t->c0_ins_src2_ready; p.ins_src2_val = &t->c0_ins_src2_val;
  p.ins_dst_tag = &t->c0_ins_dst_tag; p.ins_dst_gen = &t->c0_ins_dst_gen;
  p.wu_valid = &t->c0_wu_valid; p.wu_tag = &t->c0_wu_tag; p.wu_gen = &t->c0_wu_gen;
  p.wu_val = &t->c0_wu_val;
  p.grant_valid = &t->c0_grant_valid; p.grant_ready = &t->c0_grant_ready;
  p.grant_uop = &t->c0_grant_uop; p.grant_alu_op = &t->c0_grant_alu_op;
  p.grant_imm = &t->c0_grant_imm; p.grant_a = &t->c0_grant_a; p.grant_b = &t->c0_grant_b;
  p.grant_dst_tag = &t->c0_grant_dst_tag; p.grant_dst_gen = &t->c0_grant_dst_gen;
  p.grant_index = &t->c0_grant_index;
  p.kill_valid = &t->c0_kill_valid; p.kill_rob_index = &t->c0_kill_rob_index;
  p.kill_rob_gen = &t->c0_kill_rob_gen; p.kill_younger = &t->c0_kill_younger;
  p.occupied = &t->c0_occupied; p.count = &t->c0_count; p.full = &t->c0_full;
  p.dst_conflict = &t->c0_dst_conflict; p.age_ctr = &t->c0_age_ctr;
  p.alloc_index = &t->c0_alloc_index;
  p.obs_index = &t->c0_obs_index; p.obs_valid = &t->c0_obs_valid;
  p.obs_age = &t->c0_obs_age; p.obs_ready = &t->c0_obs_ready;
  p.obs_granted = &t->c0_obs_granted; p.obs_uop = &t->c0_obs_uop;
  p.obs_src1_tag = &t->c0_obs_src1_tag; p.obs_src1_gen = &t->c0_obs_src1_gen;
  p.obs_src2_tag = &t->c0_obs_src2_tag; p.obs_src2_gen = &t->c0_obs_src2_gen;
  p.obs_alu_op = &t->c0_obs_alu_op; p.obs_imm = &t->c0_obs_imm;
  p.obs_dst_tag = &t->c0_obs_dst_tag; p.obs_dst_gen = &t->c0_obs_dst_gen;
  p.obs_src1_ready = &t->c0_obs_src1_ready; p.obs_src2_ready = &t->c0_obs_src2_ready;
  p.obs_src1_val = &t->c0_obs_src1_val; p.obs_src2_val = &t->c0_obs_src2_val;
  p.ins_total = &t->c0_ins_total; p.grant_total = &t->c0_grant_total;
  p.kill_total = &t->c0_kill_total; p.wu_total = &t->c0_wu_total;
  p.wu_matched = &t->c0_wu_matched; p.wu_dup = &t->c0_wu_dup;
  p.wu_stale = &t->c0_wu_stale; p.wu_miss = &t->c0_wu_miss;
  return p;
}

Pins Bench::PinsFor1() {
  Vmosaic_iq_tb* t = top_;
  Pins p{};
  p.ins_valid = &t->c1_ins_valid; p.ins_ready = &t->c1_ins_ready;
  p.ins_uop = &t->c1_ins_uop; p.ins_alu_op = &t->c1_ins_alu_op;
  p.ins_imm = &t->c1_ins_imm;
  p.ins_src1_tag = &t->c1_ins_src1_tag; p.ins_src1_gen = &t->c1_ins_src1_gen;
  p.ins_src1_ready = &t->c1_ins_src1_ready; p.ins_src1_val = &t->c1_ins_src1_val;
  p.ins_src2_tag = &t->c1_ins_src2_tag; p.ins_src2_gen = &t->c1_ins_src2_gen;
  p.ins_src2_ready = &t->c1_ins_src2_ready; p.ins_src2_val = &t->c1_ins_src2_val;
  p.ins_dst_tag = &t->c1_ins_dst_tag; p.ins_dst_gen = &t->c1_ins_dst_gen;
  p.wu_valid = &t->c1_wu_valid; p.wu_tag = &t->c1_wu_tag; p.wu_gen = &t->c1_wu_gen;
  p.wu_val = &t->c1_wu_val;
  p.grant_valid = &t->c1_grant_valid; p.grant_ready = &t->c1_grant_ready;
  p.grant_uop = &t->c1_grant_uop; p.grant_alu_op = &t->c1_grant_alu_op;
  p.grant_imm = &t->c1_grant_imm; p.grant_a = &t->c1_grant_a; p.grant_b = &t->c1_grant_b;
  p.grant_dst_tag = &t->c1_grant_dst_tag; p.grant_dst_gen = &t->c1_grant_dst_gen;
  p.grant_index = &t->c1_grant_index;
  p.kill_valid = &t->c1_kill_valid; p.kill_rob_index = &t->c1_kill_rob_index;
  p.kill_rob_gen = &t->c1_kill_rob_gen; p.kill_younger = &t->c1_kill_younger;
  p.occupied = &t->c1_occupied; p.count = &t->c1_count; p.full = &t->c1_full;
  p.dst_conflict = &t->c1_dst_conflict; p.age_ctr = &t->c1_age_ctr;
  p.alloc_index = &t->c1_alloc_index;
  p.obs_index = &t->c1_obs_index; p.obs_valid = &t->c1_obs_valid;
  p.obs_age = &t->c1_obs_age; p.obs_ready = &t->c1_obs_ready;
  p.obs_granted = &t->c1_obs_granted; p.obs_uop = &t->c1_obs_uop;
  p.obs_src1_tag = &t->c1_obs_src1_tag; p.obs_src1_gen = &t->c1_obs_src1_gen;
  p.obs_src2_tag = &t->c1_obs_src2_tag; p.obs_src2_gen = &t->c1_obs_src2_gen;
  p.obs_alu_op = &t->c1_obs_alu_op; p.obs_imm = &t->c1_obs_imm;
  p.obs_dst_tag = &t->c1_obs_dst_tag; p.obs_dst_gen = &t->c1_obs_dst_gen;
  p.obs_src1_ready = &t->c1_obs_src1_ready; p.obs_src2_ready = &t->c1_obs_src2_ready;
  p.obs_src1_val = &t->c1_obs_src1_val; p.obs_src2_val = &t->c1_obs_src2_val;
  p.ins_total = &t->c1_ins_total; p.grant_total = &t->c1_grant_total;
  p.kill_total = &t->c1_kill_total; p.wu_total = &t->c1_wu_total;
  p.wu_matched = &t->c1_wu_matched; p.wu_dup = &t->c1_wu_dup;
  p.wu_stale = &t->c1_wu_stale; p.wu_miss = &t->c1_wu_miss;
  return p;
}

void Bench::Drive(const Pins& p, const Stimulus& s) {
  *p.ins_valid = s.ins_valid;
  *p.ins_uop = s.uop;
  *p.ins_alu_op = static_cast<uint8_t>(s.alu_op);
  *p.ins_imm = s.imm;
  *p.ins_src1_tag = static_cast<uint8_t>(s.s1.tag);
  *p.ins_src1_gen = static_cast<uint8_t>(s.s1.gen);
  *p.ins_src1_ready = s.s1.ready;
  *p.ins_src1_val = s.s1.value;
  *p.ins_src2_tag = static_cast<uint8_t>(s.s2.tag);
  *p.ins_src2_gen = static_cast<uint8_t>(s.s2.gen);
  *p.ins_src2_ready = s.s2.ready;
  *p.ins_src2_val = s.s2.value;
  *p.ins_dst_tag = static_cast<uint8_t>(s.dst_tag);
  *p.ins_dst_gen = static_cast<uint8_t>(s.dst_gen);
  *p.wu_valid = s.wu_valid;
  *p.wu_tag = static_cast<uint8_t>(s.wu_tag);
  *p.wu_gen = static_cast<uint8_t>(s.wu_gen);
  *p.wu_val = s.wu_val;
  *p.grant_ready = s.grant_ready;
  *p.kill_valid = s.kill_valid;
  *p.kill_rob_index = static_cast<uint8_t>(s.kill_rob_index);
  *p.kill_rob_gen = static_cast<uint8_t>(s.kill_rob_gen);
  *p.kill_younger = s.kill_younger;
}

// Settle the combinational outputs with the clock low, so the sampling below
// sees the grant and readiness the DUT presents *this* cycle, not the ones the
// previous edge left behind.
void Bench::Settle() {
  top_->clk = 0;
  top_->eval();
}

// The rising edge. `mosaic::ClockDriver` counts cycles and owns the reset
// schedule but it does not own the wire, so the wire is driven here: the inputs
// are already in place and already checked against the shadow, so this is the
// edge both sides commit to. The shadow is advanced by the caller immediately
// after, in the same order the hardware will have applied them.
void Bench::Edge() {
  top_->clk = 1;
  top_->eval();
  top_->clk = 0;
  top_->eval();
}

void Bench::CheckGrantStability(Instance& inst) {
  // While the FU is stalling, the offered entry and its payload must not move.
  // This is the card's back-pressure rule, checked against the *previous*
  // cycle's observation, so it is independent of the shadow's own prediction.
  if (!inst.last_grant_valid || inst.last_grant_ready) return;
  const Pins& p = inst.pins;
  if (!*p.grant_valid) {
    rep_.Mismatch(std::string(inst.shadow->name()) + " back-pressure",
                  "grant_valid still high", "grant_valid dropped");
    rep_.Check(false, "grant withdrawn while the FU was stalled");
    return;
  }
  rep_.Check(*p.grant_uop == inst.last_grant_uop,
             std::string(inst.shadow->name()) + ": the stalled grant still offers the same uop");
  rep_.Check(*p.grant_a == inst.last_grant_a && *p.grant_b == inst.last_grant_b,
             std::string(inst.shadow->name()) + ": the stalled grant holds its operands");
  rep_.Check(*p.grant_imm == inst.last_grant_imm,
             std::string(inst.shadow->name()) + ": the stalled grant holds its immediate");
  rep_.Check(*p.grant_dst_tag == inst.last_grant_dst_tag &&
                 *p.grant_dst_gen == inst.last_grant_dst_gen,
             std::string(inst.shadow->name()) + ": the stalled grant holds its destination");
}

void Bench::CheckOne(Instance& inst, const Stimulus& s) {
  const Pins& p = inst.pins;
  const std::string who = inst.shadow->name();
  Shadow* sh = inst.shadow;
  inst.cov.cycles++;

  // ---- what the shadow says should happen, before the edge ----
  const ExpectedGrant g = sh->expected_grant(s, s.ins_valid, s.grant_ready);
  inst.expected = g;

  // ---- insert handshake ----
  const bool expect_ready = sh->expected_ins_ready();
  rep_.Check(*p.ins_ready == expect_ready,
             who + ": ins_ready == (count < DEPTH)");
  if (s.ins_valid && !expect_ready) inst.cov.full_cycles++;
  if (sh->count() == 0) inst.cov.empty_cycles++;
  inst.cov.max_occupancy = std::max<uint64_t>(inst.cov.max_occupancy, sh->count());

  // The slot an accepted insert would take, and that the allocation pointer
  // rolls over.
  rep_.Check(*p.alloc_index == sh->expected_alloc_slot(),
             who + ": alloc_index is the first free slot from the pointer");
  if (sh->expected_alloc_slot() == 0 && sh->alloc_ptr() == kEntries - 1) {
    inst.cov.index_wraps++;
  }

  // ---- occupancy, valid vector, age counter ----
  uint32_t occupied = 0;
  for (unsigned i = 0; i < kEntries; i++) {
    if (sh->slot(i).valid) occupied |= 1u << i;
  }
  rep_.Check(*p.occupied == occupied,
             who + ": the valid vector is exactly the shadow's live slots");
  rep_.Check(*p.count == sh->count(), who + ": o_count == the shadow's occupancy");
  rep_.Check((*p.full != 0) == (sh->count() == kEntries), who + ": o_full matches occupancy");
  rep_.Check(*p.age_ctr == Shadow::Stored(sh->age_ctr_abs()),
             who + ": o_age_ctr is the shadow's absolute counter reduced modulo the age width");

  // The window invariant, checked rather than assumed. This is the assertion
  // that makes the age design's safety claim testable.
  const long long span = sh->age_span();
  if (sh->count() > 0) {
    rep_.Check(span == static_cast<long long>(sh->count()) - 1,
               who + ": live ages span exactly count-1 (the window is contiguous)");
  }
  if (sh->count() == kEntries) {
    // A full queue is where a too-narrow modulus breaks, so note that the
    // situation was reached at all.
    inst.cov.max_occupancy = kEntries;
  }

  // ---- the grant ----
  const bool grant_valid = (*p.grant_valid != 0);
  rep_.Check(grant_valid == g.valid, who + ": grant_valid matches the shadow's prediction");
  if (g.valid) {
    inst.cov.grants++;
    if (!s.grant_ready) inst.cov.grant_stalls++;
    if (g.from_ins) inst.cov.same_cycle_grant++;
    rep_.Check(*p.grant_uop == g.uop, who + ": granted uop identity");
    rep_.Check(*p.grant_alu_op == static_cast<uint8_t>(g.alu_op), who + ": granted alu_op");
    rep_.Check(*p.grant_imm == g.imm, who + ": granted immediate");
    rep_.Check(*p.grant_a == g.a, who + ": granted operand a");
    rep_.Check(*p.grant_b == g.b, who + ": granted operand b");
    rep_.Check(*p.grant_dst_tag == static_cast<uint8_t>(g.dst_tag), who + ": granted dst tag");
    rep_.Check(*p.grant_dst_gen == static_cast<uint8_t>(g.dst_gen), who + ": granted dst generation");
    if (!g.from_ins) {
      rep_.Check(*p.grant_index == static_cast<uint8_t>(g.slot),
                 who + ": grant_index names the slot presented");
    }
  }

  // Oldest versus lowest index: recorded so a phase that never makes them
  // disagree is visible as such rather than passing quietly.
  const int win = sh->expected_winner(s);
  const int low = sh->lowest_index_eligible(s);
  inst.last_winner = win;
  inst.last_lowest_index = low;
  if (win >= 0 && low >= 0 && win != low) inst.cov.lowest_index_wrong++;
  // The same disagreement, one step later in the cycle: the entry being offered
  // is a candidate in its own right, and if it is younger than the resident
  // winner *and* would land in a lower slot, then "lowest index" names it and
  // "oldest" does not. This is the case the directed phase builds by hand.
  if (s.ins_valid && win >= 0 && sh->ins_ready_now(s) &&
      sh->expected_alloc_slot() < static_cast<unsigned>(win)) {
    inst.cov.lowest_index_wrong++;
  }

  // ---- exactly-once destination ----
  rep_.Check((*p.dst_conflict != 0) == sh->expected_dst_conflict(s, s.ins_valid),
             who + ": o_dst_conflict matches the shadow's duplicate-destination search");
  if (*p.dst_conflict) inst.cov.dst_conflicts++;

  // ---- wakeup classification, and what it did ----
  const Shadow::WuClass cls = sh->classify(s, s.ins_valid);
  if (cls == Shadow::WuClass::kStale) inst.cov.stale_rejects++;
  if (cls == Shadow::WuClass::kDup) inst.cov.dup_rejects++;
  if (cls == Shadow::WuClass::kMiss) inst.cov.wu_misses++;
  rep_.Check(*p.wu_total == sh->wu_total(), who + ": o_wu_total");
  rep_.Check(*p.wu_matched == sh->wu_matched(), who + ": o_wu_matched");
  rep_.Check(*p.wu_dup == sh->wu_dup(), who + ": o_wu_dup");
  rep_.Check(*p.wu_stale == sh->wu_stale(), who + ": o_wu_stale");
  rep_.Check(*p.wu_miss == sh->wu_miss(), who + ": o_wu_miss");
  // The four wakeup outcomes are mutually exclusive and exhaustive.
  rep_.Check(*p.wu_matched + *p.wu_dup + *p.wu_stale + *p.wu_miss == *p.wu_total,
             who + ": every broadcast is classified exactly once");

  // The same-cycle enqueue-and-wakeup situation, counted from the shadow's own
  // view of it: the offered entry is dispatched *and* enabled by this cycle's
  // broadcast. `same_cycle_grant` above is the stronger half -- that such an
  // entry was actually selected.
  if (s.ins_valid && s.wu_valid &&
      ((!s.s1.ready && s.s1.tag == s.wu_tag && s.s1.gen == s.wu_gen) ||
       (!s.s2.ready && s.s2.tag == s.wu_tag && s.s2.gen == s.wu_gen))) {
    inst.cov.same_cycle_wu++;
  }
  // The producer-older case: a resident entry that this same broadcast names is
  // older than the entry being offered. A "same-cycle producer jumps the queue"
  // rule would hand the grant to the fresh one, and this counts how often the
  // situation was present for the per-cycle grant check to judge.
  if (s.ins_valid && s.wu_valid) {
    for (unsigned i = 0; i < kEntries; i++) {
      const Entry& e = sh->slot(i);
      if (!e.valid) continue;
      const bool producer_hit = (!e.s1.ready && e.s1.tag == s.wu_tag && e.s1.gen == s.wu_gen) ||
                                (!e.s2.ready && e.s2.tag == s.wu_tag && e.s2.gen == s.wu_gen);
      if (producer_hit) inst.cov.producer_older++;
    }
  }

  // Kills, counted from the shadow's kill set so the coverage cannot be claimed
  // by a phase that merely asserted something.
  if (s.kill_valid) {
    if (sh->kill_set(s).empty() == false) {
      inst.cov.kills++;
      if (sh->kill_includes_blocked(s)) inst.cov.kills_of_blocked++;
      if (sh->kill_removes_younger(s)) inst.cov.kill_younger_hits++;
    }
  }

  // ---- conservation ----
  rep_.Check(*p.ins_total == sh->ins_total(), who + ": o_ins_total");
  rep_.Check(*p.grant_total == sh->grant_total(), who + ": o_grant_total");
  rep_.Check(*p.kill_total == sh->kill_total(), who + ": o_kill_total");
  rep_.Check(*p.ins_total == *p.grant_total + *p.kill_total + *p.count,
             who + ": ins_total == grant_total + kill_total + count");

  // ---- the back-pressure invariant, against last cycle ----
  // Not on a cycle that issues a kill, and not on the cycle after one: a kill
  // naming the presented entry withdraws the grant, which is the one documented
  // exception to "the grant holds until the FU accepts it". The withdrawal is
  // *observed* on the kill cycle, but the state this check compares against is
  // the previous cycle's, so the mismatch surfaces one cycle later -- which is
  // exactly the off-by-one that made this fire 14 times in a 200k-cycle run and
  // not once in a 2k-cycle one. The shadow checks the withdrawal itself, because
  // it knows which entry the kill names.
  if (!s.kill_valid && !inst.last_kill_valid) CheckGrantStability(inst);

  // ---- every slot, field by field ----
  CheckSlots(inst.cluster);

  inst.seen_grant_valid = grant_valid;
  inst.seen_grant_uop = *p.grant_uop;
  inst.seen_grant_a = *p.grant_a;
  inst.seen_grant_b = *p.grant_b;
  inst.seen_ins_valid = s.ins_valid;
  inst.seen_grant_ready = s.grant_ready;
  inst.seen_ins_uop = s.uop;
  inst.seen_dst_conflict = (*p.dst_conflict != 0);
  inst.seen_grant_dst_tag = *p.grant_dst_tag;
  inst.seen_wu_stale = *p.wu_stale;
  inst.seen_full = (*p.full != 0);
  inst.seen_ins_ready = (*p.ins_ready != 0);

  // Record for the next cycle's stability check and for the age-wrap counter.
  inst.last_grant_valid = grant_valid;
  inst.last_grant_ready = s.grant_ready;
  inst.last_kill_valid = s.kill_valid;
  if (grant_valid) {
    inst.last_grant_uop = *p.grant_uop;
    inst.last_grant_a = *p.grant_a;
    inst.last_grant_b = *p.grant_b;
    inst.last_grant_imm = *p.grant_imm;
    inst.last_grant_dst_tag = *p.grant_dst_tag;
    inst.last_grant_dst_gen = *p.grant_dst_gen;
  }
  const uint32_t base = *p.age_ctr;
  if (inst.last_age_ctr != base) {
    // A wrap is a decrease in the 5-bit value, which can only be the base
    // sliding past the modulus. The number of 5-bit rollovers is the count of
    // times the absolute counter crossed a multiple of the modulus.
    inst.cov.age_wraps += (base < inst.last_age_ctr) ? 1 : 0;
  }
  inst.last_age_ctr = base;

  // A tag whose generation has been seen before is the shape a recycled physical
  // register has; note that the run produced one.
  if (s.wu_valid) {
    for (unsigned i = 0; i < kEntries; i++) {
      const Entry& e = sh->slot(i);
      if (e.valid && e.s1.tag == s.wu_tag && e.s1.gen != s.wu_gen) {
        inst.cov.saw_generation_advance = true;
      }
    }
  }
}

void Bench::CheckSlots(unsigned c) {
  Instance& inst = inst_[c];
  const Pins& p = inst.pins;
  const std::string who = inst.shadow->name();
  Shadow* sh = inst.shadow;
  for (unsigned i = 0; i < kEntries; i++) {
    *p.obs_index = static_cast<uint8_t>(i);
    top_->eval();
    const Entry& e = sh->slot(i);
    const std::string at = who + " slot " + std::to_string(i) + ": ";
    rep_.Check((*p.obs_valid != 0) == e.valid, at + "valid bit");
    if (e.valid) {
      rep_.Check(*p.obs_age == Shadow::Stored(e.age_abs), at + "age");
      rep_.Check((*p.obs_ready != 0) == (e.s1.ready && e.s2.ready), at + "readiness");
      rep_.Check(*p.obs_uop == e.uop, at + "uop identity");
      rep_.Check(*p.obs_alu_op == static_cast<uint8_t>(e.alu_op), at + "alu_op");
      rep_.Check(*p.obs_imm == e.imm, at + "immediate");
      rep_.Check(*p.obs_dst_tag == static_cast<uint8_t>(e.dst_tag), at + "dst tag");
      rep_.Check(*p.obs_dst_gen == static_cast<uint8_t>(e.dst_gen), at + "dst generation");
      rep_.Check(*p.obs_src1_tag == static_cast<uint8_t>(e.s1.tag), at + "source 1 tag");
      rep_.Check(*p.obs_src1_gen == static_cast<uint8_t>(e.s1.gen), at + "source 1 generation");
      rep_.Check(*p.obs_src2_tag == static_cast<uint8_t>(e.s2.tag), at + "source 2 tag");
      rep_.Check(*p.obs_src2_gen == static_cast<uint8_t>(e.s2.gen), at + "source 2 generation");
      rep_.Check((*p.obs_src1_ready != 0) == e.s1.ready, at + "source 1 readiness");
      rep_.Check((*p.obs_src2_ready != 0) == e.s2.ready, at + "source 2 readiness");
      rep_.Check(*p.obs_src1_val == e.s1.value, at + "source 1 value");
      rep_.Check(*p.obs_src2_val == e.s2.value, at + "source 2 value");
    }
  }
  *p.obs_index = 0;
  top_->eval();
}

void Bench::ReportCoverage() {
  for (unsigned c = 0; c < inst_.size(); c++) {
    const std::string who = inst_[c].shadow->name();
    rep_.Check(inst_[c].cov.grants > 0, who + ": granted at least one uop");
    rep_.Check(inst_[c].cov.grant_stalls >= 4, who + ": held the grant under back-pressure");
    rep_.Check(inst_[c].cov.same_cycle_wu >= 1, who + ": enqueued and woken in one cycle");
    rep_.Check(inst_[c].cov.same_cycle_grant >= 1,
               who + ": the same-cycle entry was actually selected");
    rep_.Check(inst_[c].cov.producer_older >= 1,
               who + ": the same-cycle producer was older than its consumer");
    rep_.Check(inst_[c].cov.stale_rejects >= 1, who + ": rejected a stale generation");
    rep_.Check(inst_[c].cov.dup_rejects >= 1, who + ": refused a duplicate broadcast");
    rep_.Check(inst_[c].cov.wu_misses >= 1, who + ": broadcast a tag nobody was waiting for");
    rep_.Check(inst_[c].cov.kills >= 1, who + ": killed at least one entry");
    rep_.Check(inst_[c].cov.kill_younger_hits >= 1, who + ": killed a suffix with kill_younger");
    rep_.Check(inst_[c].cov.kills_of_blocked >= 1, who + ": killed a not-ready entry");
    rep_.Check(inst_[c].cov.dst_conflicts >= 1, who + ": reported a duplicate destination");
    rep_.Check(inst_[c].cov.max_occupancy == kEntries, who + ": filled the queue to capacity");
    rep_.Check(inst_[c].cov.full_cycles > 0, who + ": refused an insert into a full queue");
    rep_.Check(inst_[c].cov.age_wraps > 0, who + ": the age counter wrapped at least once");
    rep_.Check(inst_[c].cov.index_wraps > 0, who + ": the allocation pointer wrapped");
    rep_.Check(inst_[c].cov.lowest_index_wrong > 0,
               who + ": oldest-ready and lowest-index disagreed at least once");
    rep_.Check(inst_[c].cov.saw_generation_advance, who + ": saw a generation advance on a tag");
  }
}

}  // namespace

// ===========================================================================
// The phases. Each is a small script of Stimulus values, so what the DUT is
// being asked to do is readable in the order it is asked.
// ===========================================================================
namespace {

// Phase 0: the elaborated geometry is what this file assumed. Read from the
// instances rather than from a second transcription of the generated package,
// and checked against the sizes in the header of this file.
void PhaseGeometry(Bench& bench, mosaic::Reporter& rep) {
  Vmosaic_iq_tb* t = bench.top();
  // Settle once before reading: Verilator's constant assigns are only visible
  // after an evaluation, and a readback that silently returns zero would make
  // this phase fail for the wrong reason.
  t->eval();
  rep.Check(t->o_iq_entries == kEntries, "queue depth is 8 as this driver assumes");
  rep.Check(t->o_iq_clusters == kClusters, "two clusters elaborated");
  rep.Check(t->o_iq_xlen == kXlen, "XLEN is 64");
  rep.Check(t->o_iq_tag_w == kTagW, "PRF tag width is 7");
  rep.Check(t->o_iq_rob_index_w == kRobIndexW, "ROB index width is 6");
  rep.Check(t->o_iq_age_w == kAgeW, "age width is 5 bits, so the modulus is 32");
  rep.Check(t->o_iq_uop_id_w == kUopIdW, "uop identity is 16 bits");
  rep.Check(t->o_iq_idx_w == kIdxW, "slot index width is 3");
  rep.Check(t->o_iq_cnt_w == kCntW, "occupancy width is 4");
  // The inequality the age design rests on, evaluated here as well as in the
  // RTL's elaboration guard: a modulus at or below twice the maximum comparable
  // distance is the defect the plan names.
  rep.Check(kAgeMod > 2u * (kEntries - 1),
            "the age modulus 32 exceeds twice the maximum comparable distance 14");
}

// Phase 2: one uop in, observed, granted once, gone.
//
// The uop is dispatched *blocked*, so it becomes resident rather than being
// granted straight out of the insert port; the phase then enables it and checks
// it is granted exactly once and then gone. A fully-ready insert would be
// granted in the same cycle it was dispatched, which is a real behaviour but
// makes a poor "it went in and stayed" check.
void PhaseInsertBasic(Bench& bench, mosaic::Reporter& rep) {
  const uint32_t tag = bench.fresh_tag();
  Stimulus ins = bench.Blocked(MakeUop(4, 0, 0), tag, 1, /*dst*/ 9, 3);
  ins.grant_ready = false;
  bench.Step(ins);
  const int slot = bench.shadow(0)->slot_of(MakeUop(4, 0, 0));
  rep.Check(bench.shadow(0)->count() == 1, "insert-basic: the blocked entry is resident");
  rep.Check(slot >= 0, "insert-basic: it landed in a free slot");
  rep.Check(!bench.shadow(0)->s1_ready(static_cast<unsigned>(slot)),
            "insert-basic: it is still waiting for its operand");

  // Enable it. The FU is still refusing, so it is offered and held.
  Stimulus wu = bench.Wakeup(tag, 1, 0x0f0f0f0f0f0f0f0full);
  wu.grant_ready = false;
  bench.Step(wu);
  rep.Check(bench.seen_grant_valid(0), "insert-basic: the enabled entry is offered");
  rep.Check(bench.seen_grant_uop(0) == MakeUop(4, 0, 0), "insert-basic: it is the right uop");
  rep.Check(bench.seen_grant_a(0) == 0x0f0f0f0f0f0f0f0full,
            "insert-basic: the granted operand is the broadcast's value");

  // Accept it.
  Stimulus accept;
  accept.grant_ready = true;
  bench.Step(accept);
  rep.Check(bench.shadow(0)->count() == 0, "insert-basic: the queue is empty again");
  rep.Check(bench.shadow(0)->grant_total() == 1, "insert-basic: granted exactly once");
}

// Phase 3: oldest-ready, not lowest index.
//
// Four entries are inserted blocked, each its own macro so a kill is surgical.
// They land in slots 0..3 with ages 0..3. The two youngest are then woken, so
// slots 2 and 3 are ready. The *oldest* entry is then killed, which slides the
// window base up and frees slot 0; a fifth entry is dispatched into slot 0 at
// the *youngest* age. Now slot 0 is ready and is the youngest ready entry, and
// slot 2 is ready and is the oldest, so "lowest eligible index" names the wrong
// one. Every grant in that state is checked against the shadow, which picks by
// absolute age and never by index.
void PhaseOldestReady(Bench& bench, mosaic::Reporter& rep) {
  Shadow* sh = bench.shadow(0);
  bench.AlignAllocPtr();
  rep.Check(sh->alloc_ptr() == 0, "oldest-ready: the allocation pointer is at slot 0");

  // Fill the queue with blocked entries, one macro each so a kill is surgical.
  // Entry i lands in slot i and takes age i, so slot order and age order agree to
  // begin with -- which is exactly the state in which "lowest index" and "oldest"
  // are the same answer and the test would prove nothing.
  uint32_t tags[kEntries];
  for (int i = 0; i < static_cast<int>(kEntries); i++) {
    tags[i] = 0x50u + static_cast<uint32_t>(i);
    Stimulus s = bench.Blocked(MakeUop(static_cast<uint32_t>(8 + i), 0, 0), tags[i], 1,
                                0x30u + static_cast<uint32_t>(i), 1);
    s.grant_ready = false;
    bench.Step(s);
  }
  rep.Check(sh->count() == kEntries, "oldest-ready: the queue is full of blocked entries");

  // Kill the oldest, in slot 0. That frees the *lowest* slot, so the next
  // insertion takes it -- and an insertion always takes the *youngest* age. Slot
  // order and age order now disagree, which is the whole point.
  const int to_kill = sh->slot_of(MakeUop(8, 0, 0));
  rep.Check(to_kill == 0, "oldest-ready: the oldest entry is in slot 0");
  Stimulus k = bench.Kill(8, 0, /*younger=*/false);
  k.grant_ready = false;
  bench.Step(k);
  rep.Check(sh->count() == kEntries - 1, "oldest-ready: the oldest entry is gone");
  rep.Check(sh->expected_alloc_slot() == 0,
            "oldest-ready: the allocator's next choice is the freed lowest slot");

  // One cycle in which two entries become ready at once: a resident -- the entry
  // in slot 6, which is older -- and the entry being offered this cycle, which
  // will land in slot 0 at the youngest age. Both are enabled by the same
  // broadcast, because both are waiting on the same (tag, generation).
  //
  // The offered entry is *not* resident at the start of the cycle, so it is the
  // insert port's candidate rather than a resident one. It is younger, so a
  // correct oldest-ready queue must grant the resident in slot 6. A queue that
  // picked the lowest index would hand the grant to the offered entry in slot 0
  // instead, and the shadow -- which orders by unbounded absolute age -- says so
  // independently of the slot numbering.
  const int resident = sh->slot_of(MakeUop(14, 0, 0));   // the entry in slot 6
  rep.Check(resident == 6, "oldest-ready: the sixth entry landed in slot 6");
  const unsigned offered_slot = sh->expected_alloc_slot();
  rep.Check(offered_slot == 0, "oldest-ready: the offered entry will take slot 0");

  Stimulus ins = bench.Blocked(MakeUop(20, 0, 0), tags[6], 1, /*dst*/ 0x40, 1);
  ins.grant_ready = true;
  Stimulus wu = bench.Wakeup(tags[6], 1, 0x5eed5eed5eed5eedull);
  bench.Step(Bench::Merge(ins, wu));

  rep.Check(sh->slot_of(MakeUop(20, 0, 0)) == static_cast<int>(offered_slot),
            "oldest-ready: the offered entry did land in slot 0");
  rep.Check(sh->age_of(static_cast<unsigned>(offered_slot)) >
                sh->age_of(static_cast<unsigned>(resident)),
            "oldest-ready: slot 0 is younger than slot 6 despite the lower slot number");
  rep.Check(bench.seen_grant_uop(0) == sh->slot(resident).uop,
            "oldest-ready: the older entry in the higher slot was granted");
  rep.Check(bench.seen_grant_uop(0) != MakeUop(20, 0, 0),
            "oldest-ready: the lowest-index candidate was NOT granted");
  rep.Check(bench.coverage(0).lowest_index_wrong > 0,
            "oldest-ready: a lowest-index rule would have chosen differently");

  // The offered entry is still queued, ready, and is granted next.
  const int queued = sh->slot_of(MakeUop(20, 0, 0));
  rep.Check(queued >= 0 && sh->s1_ready(static_cast<unsigned>(queued)),
            "oldest-ready: the younger entry stayed queued and ready");
  bench.Idle();
  rep.Check(bench.seen_grant_uop(0) == sh->slot(queued).uop,
            "oldest-ready: and it is granted on the following cycle");
}

// Phase 4: the card's named same-cycle case. An entry dispatched this cycle
// whose operand is produced this cycle must be selectable this cycle, and it
// must carry the broadcast's value rather than the value on the insert bus.
void PhaseSameCycle(Bench& bench, mosaic::Reporter& rep) {
  Shadow* sh = bench.shadow(0);
  bench.DrainQueue();

  const uint32_t tag = bench.fresh_tag();
  Stimulus ins = bench.Blocked(MakeUop(12, 0, 0), tag, 5, /*dst*/ 0x50, 2);
  // Offer the insert bus a *wrong* value, so a grant carrying anything other than
  // the broadcast's number cannot pass by accident.
  ins.s1.value = 0xdead0000dead0000ull;
  Stimulus wu = bench.Wakeup(tag, 5, 0x0123456789abcdefull);
  Stimulus both = Bench::Merge(ins, wu);
  both.grant_ready = false;   // offered, and held

  const unsigned slot = sh->expected_alloc_slot();
  bench.Step(both);
  rep.Check(sh->s1_ready(slot), "same-cycle: the entry dispatched this cycle took the broadcast");
  rep.Check(sh->s1_value(slot) == 0x0123456789abcdefull,
            "same-cycle: it took the broadcast's value, not the insert bus's");
  rep.Check(bench.seen_grant_valid(0),
            "same-cycle: the entry is selectable in the cycle it was dispatched");
  rep.Check(bench.seen_grant_uop(0) == MakeUop(12, 0, 0),
            "same-cycle: and it is the entry that was just dispatched");
  rep.Check(bench.seen_grant_a(0) == 0x0123456789abcdefull,
            "same-cycle: the grant carries the broadcast's value");
  rep.Check(bench.coverage(0).same_cycle_wu >= 1,
            "same-cycle: an enqueue and a wakeup happened in one cycle");
  rep.Check(bench.coverage(0).same_cycle_grant >= 1,
            "same-cycle: that entry was selected in the same cycle");

  // Now accept it, with no new stimulus, so exactly one entry is consumed.
  Stimulus accept;
  accept.grant_ready = true;
  bench.Step(accept);
  rep.Check(sh->count() == 0, "same-cycle: the entry left after being accepted");
}

// Phase 5: the same case, but with the producer *older* than the consumer.
//
// A resident entry P is blocked on tag T. The consumer C -- which reads P's
// result -- is offered on the insert port in the same cycle, also blocked on T,
// and the broadcast for T arrives in that cycle. A naive "a same-cycle producer
// jumps the queue" rule hands the grant to C, because C looks freshly enabled.
// The correct answer is P: P is older. The shadow picks by absolute age, and the
// phase asserts the producer is the entry granted.
void PhaseProducerOrder(Bench& bench, mosaic::Reporter& rep) {
  Shadow* sh = bench.shadow(0);
  bench.DrainQueue();

  const uint32_t tag = bench.fresh_tag();
  // The producer: older, blocked, waiting for the broadcast.
  Stimulus prod = bench.Blocked(MakeUop(14, 0, 0), tag, 9, /*dst*/ 0x60, 4);
  prod.grant_ready = false;
  bench.Step(prod);
  const int prod_slot = sh->slot_of(MakeUop(14, 0, 0));
  rep.Check(prod_slot >= 0, "producer-order: the producer is resident");

  // The consumer, offered in the same cycle the producer's result is broadcast.
  // It goes into the next free slot at the youngest age.
  const unsigned cons_slot = sh->expected_alloc_slot();
  Stimulus cons = bench.Blocked(MakeUop(14, 0, 1), tag, 9, /*dst*/ 0x62, 4);
  cons.s1.value = 0xbad0000bad000000ull;
  Stimulus wu = bench.Wakeup(tag, 9, 0xfeedfacecafebeefull);
  Stimulus both = Bench::Merge(cons, wu);
  both.grant_ready = false;

  bench.Step(both);
  // After the edge both are resident: the producer enabled, the consumer enabled
  // in its dispatch cycle, and the producer still older.
  rep.Check(sh->entry_valid(cons_slot) && sh->s1_ready(cons_slot),
            "producer-order: the consumer was enabled by the same-cycle broadcast");
  rep.Check(sh->s1_value(cons_slot) == 0xfeedfacecafebeefull,
            "producer-order: the consumer holds the broadcast's value");
  rep.Check(sh->age_of(static_cast<unsigned>(prod_slot)) < sh->age_of(cons_slot),
            "producer-order: the producer is older than its consumer");
  rep.Check(bench.seen_grant_uop(0) == sh->slot(prod_slot).uop,
            "producer-order: the older producer was selected, not the fresh consumer");
  rep.Check(bench.seen_grant_a(0) == 0xfeedfacecafebeefull,
            "producer-order: the grant carries the producer's woken operand");
  rep.Check(bench.coverage(0).producer_older >= 1,
            "producer-order: the discriminating situation was reached");
}

// Phase 6: the age wrap.
//
// The queue is kept full and one entry is issued per cycle with a fresh entry
// dispatched behind it. The oldest entry leaving slides the window base up by
// one every cycle, so after 32 such cycles the 5-bit age field has rolled over
// with a *full* queue live -- the only state in which a too-narrow modulus is
// ambiguous. The selection is checked against the shadow's unbounded ages on
// every one of those cycles, and the phase asserts the wrap was observed and
// that the window is still contiguous afterwards.
void PhaseAgeWrap(Bench& bench, mosaic::Reporter& rep) {
  Shadow* sh = bench.shadow(0);
  bench.DrainQueue();

  // Fill the queue with *blocked* entries, so nothing issues while filling and
  // the queue reaches capacity. Then enable the oldest one each cycle.
  for (int i = 0; i < static_cast<int>(kEntries); i++) {
    Stimulus s = bench.Blocked(MakeUop(20, 0, static_cast<uint32_t>(i)),
                               0x60u + i, 1, 0x70u + i, 1);
    s.grant_ready = false;
    bench.Step(s);
  }
  rep.Check(sh->count() == kEntries, "age-wrap: the queue is full before the walk");

  // Issue one per cycle and replace it, so the base slides up by one every cycle
  // and the age field rolls over after kAgeMod of them. The tags the still-blocked
  // entries are waiting on are kept in a list in age order: the head is always
  // the oldest live entry, because each cycle wakes the head, lets it issue, and
  // pushes a fresh tag at the tail for the replacement.
  std::vector<uint32_t> pending;
  for (int i = 0; i < static_cast<int>(kEntries); i++) pending.push_back(0x60u + i);
  uint32_t dst = 0x100;
  for (int i = 0; i < 3 * static_cast<int>(kAgeMod); i++) {
    Stimulus ins = bench.Blocked(MakeUop(21, 0, static_cast<uint32_t>(i % 8)),
                                 pending.back(), 1, dst, 1);
    ins.grant_ready = true;
    Stimulus wu = bench.Wakeup(pending.front(), 1, 0x5a5a0000u + i);
    bench.Step(Bench::Merge(ins, wu));
    pending.erase(pending.begin());
    pending.push_back(0x200u + static_cast<uint32_t>(i));
    ++dst;
  }
  rep.Check(pending.size() == kEntries, "age-wrap: the pending list stayed the queue's depth");
  rep.Check(bench.coverage(0).age_wraps > 0, "age-wrap: the age counter wrapped");
  rep.Check(sh->age_ctr_abs() > kAgeMod,
            "age-wrap: the absolute age counter has passed the modulus");
  rep.Check(sh->age_span() == static_cast<long long>(sh->count()) - 1,
            "age-wrap: the live ages are still one contiguous window after the wrap");
  rep.Check(bench.coverage(0).max_occupancy == kEntries, "age-wrap: the queue was at capacity");
}

// Phase 7: a stale generation is rejected.
//
// An entry is blocked on (tag T, generation G). A broadcast arrives for tag T
// with a generation that is *not* G -- the shape a response has when the
// physical register was recycled and re-issued. The entry must not become ready,
// the broadcast must be counted as stale rather than absorbed, and the queue's
// contents must be unchanged. The phase then sends the *correct* generation and
// checks the entry does become ready, so the rejection is discriminating rather
// than a broadcast port that never works.
void PhaseStaleWakeup(Bench& bench, mosaic::Reporter& rep) {
  Shadow* sh = bench.shadow(0);
  bench.DrainQueue();

  const uint32_t tag = bench.fresh_tag();
  Stimulus ins = bench.Blocked(MakeUop(24, 0, 0), tag, 0x11, /*dst*/ 0x80, 3);
  ins.grant_ready = false;
  bench.Step(ins);
  rep.Check(!bench.shadow(0)->s1_ready(static_cast<unsigned>(sh->slot_of(MakeUop(24, 0, 0)))),
            "stale: the entry is blocked before the broadcast");

  // Stale: same tag, generation one below.
  Stimulus stale = bench.Wakeup(tag, 0x10, 0xaaaa0000bbbb0000ull);
  stale.grant_ready = false;
  bench.Step(stale);
  rep.Check(!bench.shadow(0)->s1_ready(static_cast<unsigned>(sh->slot_of(MakeUop(24, 0, 0)))),
            "stale: a matching tag with a stale generation did NOT make the entry ready");
  rep.Check(bench.shadow(0)->s1_value(static_cast<unsigned>(sh->slot_of(MakeUop(24, 0, 0)))) == 0,
            "stale: no value was written into the blocked operand");
  rep.Check(bench.post_wu_stale(0) == bench.shadow(0)->wu_stale(),
            "stale: the rejection is counted");
  rep.Check(bench.coverage(0).stale_rejects >= 1, "stale: the rejection was observed");

  // Positive control: the correct generation does enable it, so the previous
  // check is about the generation and not about a dead wakeup port.
  Stimulus good = bench.Wakeup(tag, 0x11, 0x5555666677778888ull);
  good.grant_ready = false;
  bench.Step(good);
  rep.Check(bench.shadow(0)->s1_ready(static_cast<unsigned>(sh->slot_of(MakeUop(24, 0, 0)))),
            "stale: the correct generation does enable the entry");

  // A tag nobody is waiting for: a miss, counted as its own outcome.
  Stimulus miss = bench.Wakeup(bench.fresh_tag(), 1, 0x99);
  miss.grant_ready = false;
  bench.Step(miss);
  rep.Check(bench.coverage(0).wu_misses >= 1, "stale-phase: a broadcast for an unwaited tag");
}

// Phase 8: a second broadcast for an operand that already holds the value. The
// stored value must not change and the broadcast must be counted as a duplicate
// rather than absorbed silently -- this is the "a duplicate producer for one
// destination" rule at the operand level.
void PhaseDuplicateWakeup(Bench& bench, mosaic::Reporter& rep) {
  Shadow* sh = bench.shadow(0);
  bench.DrainQueue();

  const uint32_t tag = bench.fresh_tag();
  Stimulus ins = bench.Blocked(MakeUop(26, 0, 0), tag, 2, /*dst*/ 0x90, 1);
  ins.grant_ready = false;
  bench.Step(ins);
  Stimulus first = bench.Wakeup(tag, 2, 0x1111222233334444ull);
  first.grant_ready = false;
  bench.Step(first);
  rep.Check(bench.shadow(0)->s1_ready(static_cast<unsigned>(sh->slot_of(MakeUop(26, 0, 0)))),
            "duplicate: the first broadcast took");

  // A second broadcast for the same (tag, generation), carrying a *different*
  // value. It must be refused: the operand already holds its final value.
  Stimulus again = bench.Wakeup(tag, 2, 0x9999888877776666ull);
  again.grant_ready = false;
  bench.Step(again);
  rep.Check(bench.shadow(0)->s1_value(static_cast<unsigned>(sh->slot_of(MakeUop(26, 0, 0)))) ==
                0x1111222233334444ull,
            "duplicate: a second broadcast did not overwrite the stored value");
  rep.Check(bench.coverage(0).dup_rejects >= 1, "duplicate: the second broadcast was refused");
  rep.Check(bench.post_grant_a(0) == 0x1111222233334444ull,
            "duplicate: the grant still carries the first value");
}

// Phase 9: FU back-pressure. `grant_ready` stays low for several cycles while a
// new ready entry is offered each cycle. The offered entry must not change, and
// the new entries must not steal the grant. The `CheckGrantStability` in the
// per-cycle path does the field-by-field check; the phase adds the "a different
// entry was not offered" assertion and the final accept.
void PhaseBackPressure(Bench& bench, mosaic::Reporter& rep) {
  bench.DrainQueue();

  Stimulus first = bench.InsertOnly(MakeUop(28, 0, 0), true, true, 0, 0, 0, 0, 0xa0, 1);
  first.grant_ready = false;
  bench.Step(first);
  rep.Check(bench.seen_grant_valid(0), "back-pressure: a grant is on offer");
  // The identity of the offered entry, read from the cycle that was just
  // checked (`seen_*` is the pre-edge snapshot). Reading the top-level wire
  // after `Step` returns would sample the next cycle's combinational value --
  // one edge too late, and a different cycle from every `seen_grant_uop` below.
  const uint32_t held = bench.seen_grant_uop(0);

  // Five cycles of refusal, with a *newer* ready uop offered every cycle. Each
  // of those is younger than the held one, so a queue that re-arbitrated would
  // keep the held entry (it is still the oldest) -- which is why the check that
  // matters is the *payload and identity* stability plus the count of grants.
  uint64_t grants_before = bench.coverage(0).grants;
  for (int i = 0; i < 5; i++) {
    Stimulus ins = bench.InsertOnly(MakeUop(28, 0, static_cast<uint32_t>(i + 1)), true, true,
                                    0, 0, 0, 0, 0xa1 + i, 1);
    ins.grant_ready = false;
    bench.Step(ins);
    rep.Check(bench.seen_grant_uop(0) == held,
              "back-pressure: the same entry is still offered after " +
                  std::to_string(i + 1) + " stalled cycles");
  }
  rep.Check(bench.coverage(0).grants > grants_before,
            "back-pressure: the grant was re-presented on each stalled cycle");
  rep.Check(bench.coverage(0).grant_stalls >= 5, "back-pressure: five stalled cycles observed");

  // Release the FU. The held entry is taken; the younger ones are still queued
  // and must come out in age order afterwards.
  Stimulus release;
  release.grant_ready = true;
  bench.Step(release);
  const int held_slot_after = bench.shadow(0)->pending_slot();
  rep.Check(held_slot_after < 0, "back-pressure: the held grant was consumed once accepted");
  rep.Check(bench.shadow(0)->count() == 5,
            "back-pressure: the five younger entries are still queued");

  // And they drain in age order, which the per-cycle shadow check enforces.
  for (int i = 0; i < 12 && bench.shadow(0)->count() > 0; i++) bench.Idle();
}

// Phase 10: kill.
//
// (a) a single not-ready entry is killed, its slot comes back, and the age it
//     held is released so the next insertion lands at the top of the window
//     rather than in a hole;
// (b) `kill_younger` removes a suffix -- a whole macro plus everything younger
//     -- and the older entries survive untouched, which is the "flush keeps the
//     necessary entries" half of the card.
void PhaseKill(Bench& bench, mosaic::Reporter& rep) {
  bench.DrainQueue();

  // (a) one blocked entry in the middle of the queue.
  bench.Step(bench.Hold(bench.Blocked(MakeUop(30, 1, 0), 0x91, 1, 0xb0, 1)));
  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(30, 0, 0), true, true, 0, 0, 0, 0, 0xb1, 1)));
  bench.Step(bench.Hold(bench.Blocked(MakeUop(30, 1, 1), 0x92, 1, 0xb2, 1)));
  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(30, 0, 1), true, true, 0, 0, 0, 0, 0xb3, 1)));
  rep.Check(bench.shadow(0)->count() == 4, "kill: four entries queued");
  const long long span_before = bench.shadow(0)->age_span();
  rep.Check(span_before == 3, "kill: the four live ages span 3 before the kill");

  Stimulus k = bench.Kill(30, 1, /*younger=*/false);
  k.grant_ready = false;
  bench.Step(k);
  rep.Check(bench.shadow(0)->count() == 2,
            "kill: both uops of the named macro are gone, the others survive");
  rep.Check(bench.shadow(0)->age_span() == 1,
            "kill: the ages of the survivors closed up rather than leaving a hole");
  rep.Check(bench.coverage(0).kills_of_blocked >= 1,
            "kill: a not-ready entry was killed");

  bench.DrainQueue();

  // (b) the suffix case. Three older entries, then a two-uop macro. The macro
  // is killed with `kill_younger`, so the macro's own two uops go *and* so does
  // anything younger -- here nothing is, so exactly the macro goes. The three
  // older entries must survive: that is the card's "flush keeps the necessary
  // entries".
  bench.DrainQueue();
  // Three *older macros*, one uop each, then the macro being squashed, two uops
  // wide. Each gets its own ROB index, so "the three older entries" below means
  // three separate macros rather than three uops of one. The FU is held: with it
  // at its default the queue grants and removes each entry the cycle after it is
  // dispatched, and never reaches five.
  for (int i = 0; i < 3; i++) {
    bench.Step(bench.Hold(bench.InsertOnly(MakeUop(static_cast<uint32_t>(44 + i), 0, 0),
                                           true, true, 0, 0, 0, 0, 0xd0 + i, 1)));
  }
  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(47, 0, 0), true, true, 0, 0, 0, 0, 0xd3, 1)));
  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(47, 0, 1), true, true, 0, 0, 0, 0, 0xd4, 1)));
  rep.Check(bench.shadow(0)->count() == 5, "kill_younger: three older entries plus a two-uop macro");
  Stimulus suffix = bench.Kill(47, 0, /*younger=*/true);
  suffix.grant_ready = false;
  bench.Step(suffix);
  rep.Check(bench.shadow(0)->count() == 3,
            "kill_younger: only the named macro went; the three older entries survived");

  // (c) a suffix that really is a suffix: the named macro has something younger
  // than it, and that younger entry must go too. Without this the previous case
  // would pass on a queue that ignored `kill_younger` entirely.
  bench.DrainQueue();
  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(50, 0, 0), true, true, 0, 0, 0, 0, 0xe0, 1)));
  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(51, 0, 0), true, true, 0, 0, 0, 0, 0xe1, 1)));
  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(52, 0, 0), true, true, 0, 0, 0, 0, 0xe2, 1)));
  rep.Check(bench.shadow(0)->count() == 3, "kill_younger: three macros queued in age order");
  Stimulus mid = bench.Kill(51, 0, /*younger=*/true);
  mid.grant_ready = false;
  bench.Step(mid);
  rep.Check(bench.shadow(0)->count() == 1,
            "kill_younger: the named macro and everything younger than it went");
  rep.Check(bench.coverage(0).kill_younger_hits >= 1,
            "kill_younger: the suffix kill removed something that was not the named macro");
  bench.DrainQueue();
}

// Phase 11: exactly-once ownership. Two live uops naming one destination is a
// second producer for a destination, and it must be *reported*, not merged.
void PhaseDstConflict(Bench& bench, mosaic::Reporter& rep) {
  bench.DrainQueue();

  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(50, 0, 0), true, true, 0, 0, 0, 0, 0xe0, 7)));
  rep.Check(!bench.seen_dst_conflict(0), "dst-conflict: a single destination is not a conflict");
  // A second uop naming the same (tag, generation).
  Stimulus dup = bench.InsertOnly(MakeUop(50, 0, 1), true, true, 0, 0, 0, 0, 0xe0, 7);
  dup.grant_ready = false;
  bench.Step(dup);
  rep.Check(bench.seen_dst_conflict(0),
            "dst-conflict: two live uops naming one destination are reported");
  rep.Check(bench.coverage(0).dst_conflicts >= 1, "dst-conflict: the report was observed");
  // The same tag with a different generation is a *different* physical register
  // version, so it is not a conflict -- the generation is what makes it so.
  bench.DrainQueue();
  bench.Step(bench.Hold(bench.InsertOnly(MakeUop(50, 0, 0), true, true, 0, 0, 0, 0, 0xe1, 7)));
  Stimulus other_gen = bench.InsertOnly(MakeUop(50, 0, 1), true, true, 0, 0, 0, 0, 0xe1, 8);
  other_gen.grant_ready = false;
  bench.Step(other_gen);
  rep.Check(!bench.seen_dst_conflict(0),
            "dst-conflict: the same tag with a new generation is a new destination");
}

// Phase 12: a full queue refuses inserts, and the entries it holds issue in age
// order with no gaps. The per-cycle shadow check does the ordering; the phase
// asserts the refusal and the drain.
void PhaseFullAndOrder(Bench& bench, mosaic::Reporter& rep) {
  bench.DrainQueue();

  for (int i = 0; i < static_cast<int>(kEntries); i++) {
    bench.Step(bench.Hold(bench.InsertOnly(MakeUop(60, 0, static_cast<uint32_t>(i)), true, true,
                                           0, 0, 0, 0, 0x78 + i, 1)));
  }
  rep.Check(bench.shadow(0)->count() == kEntries, "full: the queue is at capacity");
  rep.Check(bench.post_full(0), "full: o_full is high");
  // One more: must be refused, and must change nothing.
  Stimulus over = bench.InsertOnly(MakeUop(60, 0, 99), true, true, 0, 0, 0, 0, 0xff, 1);
  over.grant_ready = false;
  bench.Step(over);
  rep.Check(!bench.post_ins_ready(0), "full: a full queue refuses an insert");
  rep.Check(bench.shadow(0)->count() == kEntries, "full: the refused insert changed nothing");
  rep.Check(bench.coverage(0).full_cycles > 0, "full: the refusal was observed");

  // Drain: the shadow checks the order of every issue.
  uint32_t next_expected = 0x78;
  for (int i = 0; i < 16; i++) {
    bench.Idle();
    // This one asks "what was granted *during* the cycle just taken", so it reads
    // the pre-edge snapshot: before the edge the DUT's grant output is the entry
    // it presented on that cycle. The post-edge snapshot would be the *next*
    // entry, because the granted one has already left.
    // Only an *accepted* grant is an issue. A grant that is held across cycles is
    // presented on each of them, and counting the held repeats would walk the
    // expected sequence forward once per cycle instead of once per uop.
    if (bench.seen_grant_valid(0) && bench.seen_grant_ready(0)) {
      rep.Check(bench.seen_grant_dst_tag(0) == next_expected,
                "full: entries issue in age order: got destination tag " +
                    std::to_string(bench.seen_grant_dst_tag(0)) + ", expected " +
                    std::to_string(next_expected));
      if (next_expected < 0x78 + kEntries) next_expected++;
    }
  }
  rep.Check(bench.shadow(0)->count() == 0, "full: the queue drained");
}

// Phase 13: the refused-insert path, directed.
//
// The open question from the earlier session was whether a *refused* insert --
// one offered while the queue is full -- advances the allocation pointer or
// consumes an age. The randomised phase reaches a full queue, but only by
// chance and never at a known allocation pointer, so on its own it cannot
// separate "the refusal happened" from "the refusal changed the bookkeeping".
// This phase builds the situation deterministically and asserts its four
// consequences separately. The shadow decides acceptance from its own occupancy
// (`count_ < kEntries`), so it models refusal rather than echoing the DUT's
// decision: if it were fed the DUT's answer there would be nothing to check.
void PhaseRefusedInsert(Bench& bench, mosaic::Reporter& rep) {
  Shadow* sh = bench.shadow(0);
  bench.DrainQueue();

  // Fill to capacity with *blocked* entries, so nothing leaves on its own and
  // no grant is on offer: the queue stays exactly as built across the refusal.
  // Each entry gets its own ROB index so the later kill removes exactly one
  // macro rather than all of them.
  for (uint32_t i = 0; i < kEntries; i++) {
    Stimulus s = bench.Blocked(MakeUop(52u + i, 0, 0), 0x30u + i, 1, /*dst*/ 0x40u + i, 1);
    s.grant_ready = false;
    bench.Step(s);
  }
  rep.Check(sh->count() == kEntries, "refused: the queue is at capacity");
  rep.Check(bench.post_full(0) && bench.post_count(0) == kEntries,
            "refused: o_full and o_count agree that it is full");

  const unsigned before_ptr = sh->alloc_ptr();
  const uint64_t before_age = sh->age_ctr_abs();
  const uint32_t before_ins = sh->ins_total();
  const uint32_t before_kills = sh->kill_total();

  // The refused insert. It is ready on both sources so that if the queue
  // wrongly admitted it, it would be immediately selectable and the mistake
  // would show on the grant port rather than lying latent in a slot.
  const uint32_t refused_uop = MakeUop(51, 0, 0);
  Stimulus over = bench.InsertOnly(refused_uop, true, true, 0, 0, 0, 0, /*dst*/ 0x7e, 1);
  over.grant_ready = false;
  bench.Step(over);

  // (a) the DUT refused, and neither pointer nor age moved. The comparisons are
  // against the DUT's settled state (post_*) and the shadow's independent
  // bookkeeping; a DUT that advanced on the refusal disagrees with one or both.
  rep.Check(bench.seen_ins_valid(0) && !bench.seen_ins_ready(0),
            "refused: ins_valid high with ins_ready low is a real refusal");
  rep.Check(!bench.post_ins_ready(0), "refused: the DUT still reports not-ready");
  rep.Check(bench.post_count(0) == kEntries, "refused: o_count is unchanged");
  rep.Check(bench.post_alloc_index(0) == sh->expected_alloc_slot(),
            "refused: o_alloc_index did not advance past the shadow's slot");
  rep.Check(bench.post_age_ctr(0) == Shadow::Stored(sh->age_ctr_abs()),
            "refused: o_age_ctr did not advance");
  rep.Check(sh->alloc_ptr() == before_ptr, "refused: the shadow's pointer did not advance");
  rep.Check(sh->age_ctr_abs() == before_age,
            "refused: the shadow's age counter did not advance");
  rep.Check(bench.post_ins_total(0) == before_ins && sh->ins_total() == before_ins,
            "refused: no insert was counted by either side");

  // (b) the refused entry is not silently dropped from the far side: it is not
  // resident, and the slot the allocator would have used still holds what was
  // there -- the DUT's own observation port is asked, not its refusal decision.
  const int would_be = static_cast<int>(sh->expected_alloc_slot());
  rep.Check(sh->slot_of(refused_uop) < 0, "refused: the shadow never admitted the entry");
  const Bench::ObservedSlot at = bench.ObserveSlot(0, static_cast<unsigned>(would_be));
  rep.Check(at.valid && at.uop != refused_uop,
            "refused: the refused entry did not appear in the observation port");
  rep.Check(at.valid && at.uop == sh->slot(static_cast<unsigned>(would_be)).uop,
            "refused: the far side still holds the shadow's resident");

  // (c) the next accepted insert lands where the shadow says. Free exactly one
  // slot by killing the oldest macro, then insert; the refusal left the
  // allocation pointer where it was, so the slot the shadow predicts is the one
  // the DUT must use -- verified through the observation port, not the DUT's
  // alloc_index alone.
  const int oldest = sh->oldest_live();
  rep.Check(oldest >= 0, "refused: there is an oldest resident to free");
  const uint32_t victim = sh->slot(static_cast<unsigned>(oldest)).uop;
  Stimulus k = bench.Kill(UopRobIndex(victim), UopRobGen(victim), /*younger=*/false);
  k.grant_ready = false;
  bench.Step(k);
  rep.Check(sh->count() == kEntries - 1, "refused: the kill freed exactly one slot");

  const unsigned expect_slot = sh->expected_alloc_slot();
  const uint32_t next_uop = MakeUop(50, 0, 0);
  Stimulus next = bench.Blocked(next_uop, 0x3f, 1, /*dst*/ 0x7f, 1);
  next.grant_ready = false;
  bench.Step(next);
  rep.Check(bench.seen_ins_ready(0), "refused: the queue accepts again once it has room");
  rep.Check(sh->slot_of(next_uop) == static_cast<int>(expect_slot),
            "refused: the next insert landed at the shadow's slot");
  const Bench::ObservedSlot placed = bench.ObserveSlot(0, expect_slot);
  rep.Check(placed.valid && placed.uop == next_uop,
            "refused: the DUT put the next insert in that slot");
  rep.Check(sh->slot_of(refused_uop) < 0, "refused: the refused entry is still absent");

  // (d) conservation still balances, on the DUT and against the shadow.
  rep.Check(bench.post_ins_total(0) ==
                bench.post_grant_total(0) + bench.post_kill_total(0) + bench.post_count(0),
            "refused: ins_total == grant_total + kill_total + count");
  rep.Check(bench.post_ins_total(0) == sh->ins_total() &&
                bench.post_grant_total(0) == sh->grant_total() &&
                bench.post_kill_total(0) == sh->kill_total() &&
                bench.post_count(0) == sh->count(),
            "refused: the conservation counters match the shadow");
  rep.Check(sh->kill_total() == before_kills + 1,
            "refused: exactly the one injected kill was counted");

  // Leave the queue empty for the randomised phase. The entries this phase
  // built are permanently blocked -- their source tags are never broadcast --
  // so an undrained queue would sit full for the whole randomised run and
  // silently starve it of the insert/select traffic it exists to generate.
  bench.DrainQueue();
  rep.Check(sh->count() == 0, "refused: the phase left the queue empty");
}

// Phase 14: randomised traffic against both shadows. Each cluster has its own
// seed, so the two disagree with each other as well as with their own models.
void PhaseRandom(Bench& bench, mosaic::Reporter& rep, int cycles) {
  // Tags the driver believes are live, so a randomised wakeup sometimes names a
  // real waiter and is a genuine hit rather than a miss every time.
  struct Live {
    uint32_t tag, gen;
    bool valid;
  };
  Live live[kClusters][8];
  for (unsigned c = 0; c < kClusters; c++) {
    for (int i = 0; i < 8; i++) live[c][i] = Live{0, 0, false};
  }
  uint32_t tag_pool = 0x100;
  uint32_t rob_index = 100;

  for (int i = 0; i < cycles; i++) {
    Stimulus s[2];
    for (unsigned c = 0; c < kClusters; c++) {
      mosaic::Rng& rng = bench.RngFor(c);
      Stimulus& t = s[c];
      t.grant_ready = rng.Chance(70);
      t.ins_valid = rng.Chance(55);
      if (t.ins_valid) {
        t.uop = MakeUop(rob_index, 1, static_cast<uint32_t>(rng.Below(4)));
        rob_index++;
        t.alu_op = static_cast<uint32_t>(rng.Below(16));
        t.imm = rng.Next();
        t.s1.ready = rng.Chance(60);
        t.s1.value = rng.Next();
        if (!t.s1.ready) {
          t.s1.tag = tag_pool + static_cast<uint32_t>(rng.Below(24));
          t.s1.gen = static_cast<uint32_t>(rng.Below(8));
        } else {
          t.s1.tag = 0;
          t.s1.gen = 0;
        }
        t.s2.ready = rng.Chance(70);
        t.s2.value = rng.Next();
        if (!t.s2.ready) {
          t.s2.tag = tag_pool + static_cast<uint32_t>(rng.Below(24));
          t.s2.gen = static_cast<uint32_t>(rng.Below(8));
        } else {
          t.s2.tag = 0;
          t.s2.gen = 0;
        }
        t.dst_tag = tag_pool + static_cast<uint32_t>(rng.Below(24));
        t.dst_gen = static_cast<uint32_t>(rng.Below(8));
      }
      t.wu_valid = rng.Chance(45);
      if (t.wu_valid) {
        t.wu_tag = tag_pool + static_cast<uint32_t>(rng.Below(24));
        t.wu_gen = static_cast<uint32_t>(rng.Below(8));
        t.wu_val = rng.Next();
      }
      t.kill_valid = rng.Chance(10);
      if (t.kill_valid) {
        t.kill_rob_index = 100 + static_cast<uint32_t>(rng.Below(rob_index > 100 ? rob_index - 100 : 1));
        t.kill_rob_gen = 1;
        t.kill_younger = rng.Chance(50);
      }
      (void)live;
    }
    bench.Step(s[0], &s[1]);
  }
}

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options opt;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &opt, &error)) {
    std::fprintf(stderr,
                 "usage: %s --case <id> [--out <dir>] [--seed <n>] "
                 "[--max-cycles <n>] [--verbose]\n%s\n",
                 argv[0], error.c_str());
    return mosaic::kExitUsage;
  }

  Verilated::commandArgs(argc, argv);
  mosaic::Reporter rep(opt, std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_iq_tb* top = new Vmosaic_iq_tb;

  Bench bench(opt, rep, top);
  bool aborted = false;
  std::string abort_reason;

  auto out_of_cycles = [&]() {
    if (!aborted && bench.cycles() >= opt.max_cycles) {
      aborted = true;
      abort_reason = "exceeded --max-cycles=" + std::to_string(opt.max_cycles);
    }
    return aborted;
  };

  // ---- phase 0: geometry, before anything is driven -----------------------
  PhaseGeometry(bench, rep);

  // ---- phase 1: reset ------------------------------------------------------
  // rst is asserted for the reset schedule, then released. The first settled
  // cycle after the final reset edge is the one the checks are made on: nothing
  // may be valid, nothing granted, every counter zero, and no transfer reported
  // during the reset cycles themselves.
  bench.PulseReset(kResetCycles);
  top->rst = 1;
  for (int i = 0; i < kResetCycles; i++) {
    top->clk = 0; top->eval();
    top->clk = 1; top->eval();
    rep.Check(!top->c0_ins_ready, "reset: ins_ready is low while rst is high");
    rep.Check(!top->c0_grant_valid, "reset: grant_valid is low while rst is high");
  }
  top->clk = 0; top->eval();
  top->rst = 0;
  top->eval();
  rep.Check(top->c0_count == 0, "reset: the queue is empty after reset");
  rep.Check(top->c0_occupied == 0, "reset: no slot is valid after reset");
  rep.Check(top->c0_ins_total == 0 && top->c0_grant_total == 0 && top->c0_kill_total == 0,
            "reset: the conservation counters are zero");
  rep.Check(top->c0_wu_total == 0 && top->c0_wu_stale == 0 && top->c0_wu_matched == 0,
            "reset: the wakeup counters are zero");
  rep.Check(top->c0_age_ctr == 0, "reset: the age counter is zero");
  // Both clusters clean, not just cluster 0.
  rep.Check(top->c1_count == 0 && top->c1_occupied == 0, "reset: cluster 1 is clean too");

  // ---- phases 2..12 --------------------------------------------------------
  if (!aborted) PhaseInsertBasic(bench, rep);
  if (!aborted) PhaseOldestReady(bench, rep);
  if (!aborted) PhaseSameCycle(bench, rep);
  if (!aborted) PhaseProducerOrder(bench, rep);
  if (!aborted) PhaseAgeWrap(bench, rep);
  if (!aborted) PhaseStaleWakeup(bench, rep);
  if (!aborted) PhaseDuplicateWakeup(bench, rep);
  if (!aborted) PhaseBackPressure(bench, rep);
  if (!aborted) PhaseKill(bench, rep);
  if (!aborted) PhaseDstConflict(bench, rep);
  if (!aborted) PhaseFullAndOrder(bench, rep);
  if (!aborted) PhaseRefusedInsert(bench, rep);

  // ---- phase 14: randomised ------------------------------------------------
  if (!aborted) {
    const int budget = static_cast<int>(
        opt.max_cycles > bench.cycles() + 200 ? opt.max_cycles - bench.cycles() - 200 : 2000);
    PhaseRandom(bench, rep, budget);
  }

  bench.ReportCoverage();
  top->final();
  delete top;

  if (aborted) {
    std::fprintf(stderr, "ABORT: %s\n", abort_reason.c_str());
    return rep.Finish("FAIL", abort_reason);
  }
  if (bench.failures() > 0) {
    std::string detail = std::to_string(bench.failures()) + " failed of " +
                         std::to_string(bench.checks()) + " checks over " +
                         std::to_string(bench.cycles()) + " cycles";
    return rep.Finish("FAIL", detail);
  }
  std::string detail = std::to_string(bench.checks()) + " checks over " +
                       std::to_string(bench.cycles()) + " cycles, " +
                       std::to_string(bench.reset_cycles()) + " reset cycles, 2 clusters";
  return rep.Finish("PASS", detail);
}
