// ============================================================================
// tb_result_fifo.cpp -- CASE=completion.fu_collision, work package I-025.
//
// The DUT is never its own oracle. Every output and every piece of state is
// compared against an independent C++ shadow written from the *contract*
// documented in rtl/core/mosaic_result_fifo.sv -- not from the RTL's structure.
// The shadow is an ordinary ordered queue with its own counters; it derives the
// kill predicate, the fixed-priority producer arbitration, the refusal of a push
// with no free slot, the compaction on kill and the conservation identity from
// that prose. It shares no code with the RTL and never looks at Verilator
// internals, so agreeing with it is evidence about the contract rather than a
// restatement of the implementation.
//
// Geometry is read out of the elaborated DUT (`o_dut_*`), so this file contains
// no depth, no producer count and no field width: the shadow sizes itself from
// the hardware that exists. The wrapper's own re-expression of the same numbers
// (`o_tb_*`) is required to agree with the DUT's localparams field by field, so a
// profile change is a named failure here rather than a silently narrowed port.
//
// Phases, each of which resets first and fails on its own:
//
//   1. reset-state  the documented cold state: empty, no head, every producer
//                   refused, counters zero.
//   2. collision    every producer completes in the same cycle with room for
//                   exactly one: one acceptance, the rest still offered next
//                   cycle with byte-identical payloads, and the exception-bearing
//                   result survives the collision intact. Every result then
//                   enters, and the delivery order is the acceptance order.
//   3. fill-drain   fill, keep pushing (all refused, nothing lost), hold the
//                   consumer stalled (valid and payload stable), then drain and
//                   compare every element and its order, exceptions interleaved.
//   4. kill         a killing match drops buffered entries and counts them, an
//                   offering producer whose result matches is absorbed and
//                   counted, a non-matching producer is refused normally and
//                   taken once there is room, and no killed result is delivered.
//   5. pop-push     a full queue's pop does not admit a same-cycle push, and the
//                   refused result is taken the cycle after: the documented
//                   consequence of `*_ready` never consulting `c_ready`.
//   6. valid-pulse  the card's Fail criterion, first half: a result is presented
//                   and *held* across a full queue for several cycles and is
//                   accepted, unchanged, once there is room. The second half --
//                   multiply and load returning together -- is phase 2.
//   7. random       random producer timing, random consumer back-pressure and
//                   random kills, shadow-compared every cycle, with the coverage
//                   counters asserted at the end so a campaign that did nothing
//                   cannot pass.
//
// Standing invariants, checked on every cycle of every phase rather than in one
// place:
//
//   * `o_push_ctr == o_pop_ctr + o_kill_ctr + o_count`, from the DUT's own
//     ports: every accepted result leaves exactly once. A pop that was not
//     pushed, a duplicate pop and a lost push each break it, and every phase
//     assertion that counts accepted results independently of the DUT's counters
//     is a second check on the same identity.
//   * `o_kill_total == o_kill_ctr + o_kill_offer_ctr`.
//   * The occupancy bitmap is exactly `position < o_count`, and every position at
//     or above `o_count` of the observation vector is zero.
//   * No identity that is currently under a kill is delivered.
// ============================================================================

#include <verilated.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_result_fifo_tb.h"

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
std::string Dec(uint64_t value) { return std::to_string(value); }

// ------------------------------------------------------------------ the result
// One completion: the uop identity, the value, and the exception payload that
// must travel with them.
struct Entry {
  uint32_t rob_index = 0;
  uint32_t rob_gen = 0;
  uint32_t uop_index = 0;
  uint64_t data = 0;
  bool exc = false;
  uint64_t cause = 0;
  uint64_t tval = 0;

  bool operator==(const Entry& o) const {
    return rob_index == o.rob_index && rob_gen == o.rob_gen && uop_index == o.uop_index &&
           data == o.data && exc == o.exc && cause == o.cause && tval == o.tval;
  }
  bool operator!=(const Entry& o) const { return !(*this == o); }
  std::string str() const {
    return "{i" + Dec(rob_index) + ",g" + Dec(rob_gen) + ",u" + Dec(uop_index) + ",d0x" +
           mosaic::Hex(data, 16) + ",exc" + Bool(exc) + ",cause" + Dec(cause) + ",tval0x" +
           mosaic::Hex(tval, 16) + "}";
  }
};

std::string DescribeSeq(const std::vector<Entry>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); i++) {
    if (i != 0) out += ", ";
    out += v[i].str();
  }
  return out + "]";
}

// ---------------------------------------------------------------- the geometry
// Everything the shadow and the codec need, learned from the elaborated DUT.
struct Geom {
  uint32_t entries = 0;
  uint32_t producers = 0;
  uint32_t entry_w = 0;
  uint32_t cnt_w = 0;
  uint32_t xlen = 0;
  uint32_t rob_index_w = 0;
  uint32_t rob_gen_w = 0;
  uint32_t uop_w = 0;
  uint32_t kill_index_mask = 0;
  uint32_t kill_gen_mask = 0;
  uint32_t uop_mask = 0;

  // Field offsets, least significant first, derived from the widths above with
  // the layout the RTL header documents. `entry_w` is checked against the sum
  // once, at startup, so a layout drift is a named failure rather than an
  // off-by-one in every payload comparison.
  int off_tval = 0;
  int off_cause = 0;
  int off_exc = 0;
  int off_data = 0;
  int off_uop = 0;
  int off_gen = 0;
  int off_index = 0;
};

void DeriveOffsets(Geom* g) {
  g->off_tval = 0;
  g->off_cause = (int)g->xlen;
  g->off_exc = (int)(2 * g->xlen);
  g->off_data = (int)(2 * g->xlen) + 1;
  g->off_uop = (int)(3 * g->xlen) + 1;
  g->off_gen = g->off_uop + (int)g->uop_w;
  g->off_index = g->off_gen + (int)g->rob_gen_w;
}

// ----------------------------------------------------------- packing/unpacking
// Payloads cross the boundary as flat bit vectors, exactly as the RTL declares
// them. These helpers convert between a word array (what Verilator exposes) and
// the `Entry` the shadow reasons about.
class Codec {
 public:
  explicit Codec(const Geom& g) : g_(g) {}

  uint64_t Bits(const std::vector<uint32_t>& w, int off, int width) const {
    uint64_t value = 0;
    for (int b = 0; b < width; b++) {
      const int bit = off + b;
      value |= (uint64_t)((w[bit / 32] >> (bit % 32)) & 1u) << b;
    }
    return value;
  }

  void PutBits(std::vector<uint32_t>* w, int off, int width, uint64_t value) const {
    for (int b = 0; b < width; b++) {
      const int bit = off + b;
      const uint32_t mask = 1u << (bit % 32);
      if ((value >> b) & 1ull) {
        (*w)[bit / 32] |= mask;
      } else {
        (*w)[bit / 32] &= ~mask;
      }
    }
  }

  Entry Get(const std::vector<uint32_t>& w, int base) const {
    Entry e;
    e.rob_index = (uint32_t)Bits(w, base + g_.off_index, (int)g_.rob_index_w);
    e.rob_gen = (uint32_t)Bits(w, base + g_.off_gen, (int)g_.rob_gen_w);
    e.uop_index = (uint32_t)Bits(w, base + g_.off_uop, (int)g_.uop_w);
    e.data = Bits(w, base + g_.off_data, (int)g_.xlen);
    e.exc = Bits(w, base + g_.off_exc, 1) != 0;
    e.cause = Bits(w, base + g_.off_cause, (int)g_.xlen);
    e.tval = Bits(w, base + g_.off_tval, (int)g_.xlen);
    return e;
  }

  void Put(std::vector<uint32_t>* w, int base, const Entry& e) const {
    PutBits(w, base + g_.off_index, (int)g_.rob_index_w, e.rob_index);
    PutBits(w, base + g_.off_gen, (int)g_.rob_gen_w, e.rob_gen);
    PutBits(w, base + g_.off_uop, (int)g_.uop_w, e.uop_index);
    PutBits(w, base + g_.off_data, (int)g_.xlen, e.data);
    PutBits(w, base + g_.off_exc, 1, e.exc ? 1 : 0);
    PutBits(w, base + g_.off_cause, (int)g_.xlen, e.cause);
    PutBits(w, base + g_.off_tval, (int)g_.xlen, e.tval);
  }

  std::vector<uint32_t> Wide(int bits) const {
    return std::vector<uint32_t>((bits + 31) / 32, 0u);
  }

  const Geom& geom() const { return g_; }

 private:
  Geom g_;
};

template <std::size_t N>
std::vector<uint32_t> WordsOf(const VlWide<N>& value) {
  std::vector<uint32_t> out(N);
  for (std::size_t i = 0; i < N; i++) out[i] = value[i];
  return out;
}

template <std::size_t N>
void StoreWide(VlWide<N>* dst, const std::vector<uint32_t>& src) {
  for (std::size_t i = 0; i < N; i++) (*dst)[i] = (i < src.size()) ? src[i] : 0u;
}

// ------------------------------------------------------------------- stimulus
struct Stim {
  std::vector<bool> p_valid;
  std::vector<Entry> p_pay;
  bool c_ready = false;
  bool kill_valid = false;
  bool kill_all = false;
  uint32_t kill_index = 0;
  uint32_t kill_gen = 0;
};

Stim Idle(const Geom& g) {
  Stim s;
  s.p_valid.assign(g.producers, false);
  s.p_pay.assign(g.producers, Entry{});
  return s;
}

// Unique identities, so "this exact result was killed and must never appear" is
// decidable: the campaign never reuses an identity after a kill.
class Entries {
 public:
  explicit Entries(const Geom& g) : g_(g) {}

  Entry Make(bool exc) {
    Entry e;
    const uint64_t c = counter_++;
    e.rob_index = (uint32_t)((c >> 0) & g_.kill_index_mask);
    e.rob_gen = (uint32_t)((c >> 6) & g_.kill_gen_mask);
    e.uop_index = (uint32_t)((c >> 13) & g_.uop_mask);
    e.data = 0x1111000000000000ull + c;
    e.exc = exc;
    e.cause = exc ? (0x0d + (c & 0xf)) : 0;
    e.tval = exc ? (0xdeadbeef00000000ull | c) : 0;
    return e;
  }

 private:
  Geom g_;
  uint64_t counter_ = 0;
};

// ---------------------------------------------------------------------- shadow
struct Prediction {
  bool c_valid = false;
  Entry c_pay{};
  std::vector<bool> p_ready;
  std::vector<bool> granted;
  std::vector<bool> absorbed;
  bool pop = false;
  std::vector<Entry> next_q;
  std::vector<Entry> killed_buffered;   // buffered entries the kill removed
  std::vector<Entry> absorbed_pay;      // in-flight offers the kill consumed
  uint32_t next_push = 0;
  uint32_t next_pop = 0;
  uint32_t next_kill = 0;
  uint32_t next_kill_offer = 0;
};

class Shadow {
 public:
  explicit Shadow(const Geom& g) : g_(g) {}

  void Reset() {
    q_.clear();
    push_ = pop_ = kill_ = kill_offer_ = 0;
    killed_ids_.clear();
  }

  bool Hits(const Entry& e, const Stim& s) const {
    return s.kill_valid &&
           (s.kill_all || (e.rob_index == s.kill_index && e.rob_gen == s.kill_gen));
  }

  Prediction Predict(const Stim& s) const {
    Prediction r;
    r.p_ready.assign(g_.producers, false);
    r.granted.assign(g_.producers, false);
    r.absorbed.assign(g_.producers, false);

    const bool head_hit = !q_.empty() && Hits(q_[0], s);
    r.c_valid = !q_.empty() && !head_hit;
    if (!q_.empty()) r.c_pay = q_[0];
    r.pop = r.c_valid && s.c_ready;

    const uint32_t free_slots = g_.entries - (uint32_t)q_.size();
    uint32_t taken = 0;
    for (uint32_t p = 0; p < g_.producers; p++) {
      const bool absorb = s.p_valid[p] && Hits(s.p_pay[p], s);
      r.absorbed[p] = absorb;
      const bool offer = s.p_valid[p] && !absorb;
      if (absorb) {
        r.p_ready[p] = true;
        r.absorbed_pay.push_back(s.p_pay[p]);
      } else if (offer && taken < free_slots) {
        r.granted[p] = true;
        r.p_ready[p] = true;
        taken++;
      }
    }

    for (size_t j = 0; j < q_.size(); j++) {
      if (Hits(q_[j], s)) {
        r.killed_buffered.push_back(q_[j]);
        continue;
      }
      if (j == 0 && r.pop) continue;
      r.next_q.push_back(q_[j]);
    }
    for (uint32_t p = 0; p < g_.producers; p++) {
      if (r.granted[p]) {
        r.next_q.push_back(s.p_pay[p]);
        r.next_push++;
      }
    }
    r.next_kill = (uint32_t)r.killed_buffered.size();
    r.next_kill_offer = (uint32_t)r.absorbed_pay.size();
    r.next_pop = r.pop ? 1 : 0;
    return r;
  }

  void Apply(const Prediction& r, const Stim& s) {
    // A killed identity stays killed until the same identity is legitimately
    // pushed again, which re-arms it. Nothing in the campaign reuses an identity,
    // so the set is exact rather than approximate.
    for (const Entry& e : r.killed_buffered) killed_ids_.insert(Id(e));
    for (const Entry& e : r.absorbed_pay) killed_ids_.insert(Id(e));
    for (uint32_t p = 0; p < g_.producers; p++) {
      if (r.granted[p]) killed_ids_.erase(Id(s.p_pay[p]));
    }
    q_ = r.next_q;
    push_ += r.next_push;
    pop_ += r.next_pop;
    kill_ += r.next_kill;
    kill_offer_ += r.next_kill_offer;
  }

  static uint64_t Id(const Entry& e) {
    return ((uint64_t)e.rob_index << 32) | ((uint64_t)e.rob_gen << 16) | e.uop_index;
  }

  bool IsKilled(const Entry& e) const { return killed_ids_.count(Id(e)) != 0; }

  const std::vector<Entry>& q() const { return q_; }
  uint32_t count() const { return (uint32_t)q_.size(); }
  uint32_t push() const { return push_; }
  uint32_t pop() const { return pop_; }
  uint32_t kill() const { return kill_; }
  uint32_t kill_offer() const { return kill_offer_; }
  uint64_t killed_count() const { return killed_ids_.size(); }

 private:
  Geom g_;
  std::vector<Entry> q_;
  uint32_t push_ = 0;
  uint32_t pop_ = 0;
  uint32_t kill_ = 0;
  uint32_t kill_offer_ = 0;
  std::set<uint64_t> killed_ids_;
};

// --------------------------------------------------------------------- harness
// A pre-edge reading of the DUT's combinational outputs: what the hardware
// presented in the cycle under test. Post-edge state is read through the
// accessors below, which is the two-snapshot rule this project's reports insist
// on -- the two are never mixed into one comparison.
struct Observed {
  bool c_valid = false;
  Entry c_pay{};
  std::vector<bool> p_ready;
};

class Harness {
 public:
  Harness(Vmosaic_result_fifo_tb* dut, mosaic::ClockDriver* clk, const Codec& codec,
          uint64_t max_cycles)
      : dut_(dut), clk_(clk), codec_(codec), max_cycles_(max_cycles) {
    observed_.p_ready.assign(codec.geom().producers, false);
  }

  void BindShadow(Shadow* shadow) { shadow_ = shadow; }

  void Phase(const std::string& name) {
    phase_ = name;
    pushes_ = pops_ = kills_ = kill_offers_ = refusals_ = collisions_ = 0;
    exc_pushes_ = exc_pops_ = 0;
    phase_comparisons_ = 0;
  }

  // Assert reset for `cycles` rising edges with no stimulus. The shadow is reset
  // here rather than by the caller, because a reset that moved only the DUT would
  // leave the two models describing different machines.
  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(Idle(codec_.geom()), true);
    if (shadow_ != nullptr) shadow_->Reset();
  }

  void Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the campaign finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->c_ready = s.c_ready ? 1 : 0;
    dut_->kill_valid = s.kill_valid ? 1 : 0;
    dut_->kill_all = s.kill_all ? 1 : 0;
    dut_->kill_rob_index = (uint8_t)(s.kill_index & codec_.geom().kill_index_mask);
    dut_->kill_rob_gen = (uint8_t)(s.kill_gen & codec_.geom().kill_gen_mask);
    uint8_t valid_bits = 0;
    std::vector<uint32_t> pay =
        codec_.Wide((int)(codec_.geom().producers * codec_.geom().entry_w));
    for (uint32_t p = 0; p < codec_.geom().producers; p++) {
      if (s.p_valid[p]) valid_bits |= (uint8_t)(1u << p);
      codec_.Put(&pay, (int)(p * codec_.geom().entry_w), s.p_pay[p]);
    }
    dut_->p_valid = valid_bits;
    StoreWide(&dut_->p_pay, pay);
    dut_->eval();

    const std::string where = phase_ + ": cycle " + Dec(cycles_);

    if (!rst) {
      const Prediction p = shadow_->Predict(s);
      CaptureObserved();
      CompareCombinational(s, p, where);
      shadow_->Apply(p, s);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
    ++phase_cycles_;

    if (!rst) {
      CompareState(where);
      CheckConservation(where);
      UpdateCoverage(s);
      ++comparisons_;
      ++phase_comparisons_;
    }
  }

  const Observed& prev() const { return observed_; }

  // Post-edge state, read from the DUT.
  uint32_t count() const { return dut_->o_count; }
  uint32_t push_ctr() const { return dut_->o_push_ctr; }
  uint32_t pop_ctr() const { return dut_->o_pop_ctr; }
  uint32_t kill_ctr() const { return dut_->o_kill_ctr; }
  uint32_t kill_offer_ctr() const { return dut_->o_kill_offer_ctr; }
  uint32_t kill_total() const { return dut_->o_kill_total; }

  std::vector<Entry> entries() const {
    const std::vector<uint32_t> w = WordsOf(dut_->o_entry_pay);
    std::vector<Entry> out;
    for (uint32_t j = 0; j < count(); j++) {
      out.push_back(codec_.Get(w, (int)(j * codec_.geom().entry_w)));
    }
    return out;
  }

  std::vector<bool> occ() const { return Mask(dut_->o_occ); }
  std::vector<bool> exc_occ() const { return Mask(dut_->o_exc_occ); }

  uint64_t comparisons() const { return comparisons_; }
  uint64_t phase_comparisons() const { return phase_comparisons_; }
  uint64_t cycles() const { return cycles_; }
  uint64_t pushes() const { return pushes_; }
  uint64_t pops() const { return pops_; }
  uint64_t kills() const { return kills_; }
  uint64_t kill_offers() const { return kill_offers_; }
  uint64_t refusals() const { return refusals_; }
  uint64_t collisions() const { return collisions_; }
  uint64_t exc_pushes() const { return exc_pushes_; }
  uint64_t exc_pops() const { return exc_pops_; }
  const Shadow& shadow() const { return *shadow_; }

 private:
  std::vector<bool> Mask(uint32_t bits) const {
    std::vector<bool> out(codec_.geom().entries, false);
    for (uint32_t j = 0; j < codec_.geom().entries; j++) out[j] = ((bits >> j) & 1u) != 0;
    return out;
  }

  void CaptureObserved() {
    observed_.c_valid = dut_->c_valid != 0;
    observed_.c_pay = codec_.Get(WordsOf(dut_->c_pay), 0);
    for (uint32_t p = 0; p < codec_.geom().producers; p++) {
      observed_.p_ready[p] = ((dut_->p_ready >> p) & 1u) != 0;
    }
  }

  void CompareCombinational(const Stim& s, const Prediction& e, const std::string& where) {
    Require(observed_.c_valid == e.c_valid, where,
            "c_valid: expected " + Bool(e.c_valid) + ", got " + Bool(observed_.c_valid) + " " +
                DescribeStim(s));
    if (e.c_valid) {
      Require(observed_.c_pay == e.c_pay, where,
              "c_pay: expected " + e.c_pay.str() + ", got " + observed_.c_pay.str());
    }
    for (uint32_t p = 0; p < codec_.geom().producers; p++) {
      Require(observed_.p_ready[p] == e.p_ready[p], where,
              "p_ready[" + Dec(p) + "]: expected " + Bool(e.p_ready[p]) + ", got " +
                  Bool(observed_.p_ready[p]) + " " + DescribeStim(s));
    }
  }

  // Compare the whole state, not a projection of it.
  void CompareState(const std::string& where) {
    const Shadow& sh = *shadow_;
    Require(count() == sh.count(), where,
            "o_count: expected " + Dec(sh.count()) + ", got " + Dec(count()));

    const std::vector<bool> want_occ = RunLength(sh.count());
    const std::vector<bool> got_occ = occ();
    Require(got_occ == want_occ, where,
            "o_occ differs from the shadow at position " + Dec(FirstDiff(got_occ, want_occ)));

    std::vector<bool> want_exc(codec_.geom().entries, false);
    for (uint32_t j = 0; j < sh.count(); j++) want_exc[j] = sh.q()[j].exc;
    const std::vector<bool> got_exc = exc_occ();
    Require(got_exc == want_exc, where,
            "o_exc_occ differs from the shadow at position " + Dec(FirstDiff(got_exc, want_exc)));

    // Every position, zero-padded by the DUT above the occupancy, so the whole
    // vector is comparable and a leaked entry is visible as a nonzero position
    // with no occupancy bit.
    const std::vector<uint32_t> w = WordsOf(dut_->o_entry_pay);
    for (uint32_t j = 0; j < codec_.geom().entries; j++) {
      const Entry got = codec_.Get(w, (int)(j * codec_.geom().entry_w));
      const Entry want = (j < sh.count()) ? sh.q()[j] : Entry{};
      Require(got == want, where,
              "entry[" + Dec(j) + "]: expected " + want.str() + ", got " + got.str());
    }

    Require(push_ctr() == sh.push(), where,
            "o_push_ctr: expected " + Dec(sh.push()) + ", got " + Dec(push_ctr()));
    Require(pop_ctr() == sh.pop(), where,
            "o_pop_ctr: expected " + Dec(sh.pop()) + ", got " + Dec(pop_ctr()));
    Require(kill_ctr() == sh.kill(), where,
            "o_kill_ctr: expected " + Dec(sh.kill()) + ", got " + Dec(kill_ctr()));
    Require(kill_offer_ctr() == sh.kill_offer(), where,
            "o_kill_offer_ctr: expected " + Dec(sh.kill_offer()) + ", got " +
                Dec(kill_offer_ctr()));
    Require(kill_total() == sh.kill() + sh.kill_offer(), where,
            "o_kill_total: expected " + Dec(sh.kill() + sh.kill_offer()) + ", got " +
                Dec(kill_total()));

    // No identity that is currently killed may be presented.
    if (observed_.c_valid) {
      Require(!sh.IsKilled(observed_.c_pay), where,
              "a killed result was presented to the consumer: " + observed_.c_pay.str());
    }
  }

  // Conservation, recomputed from the DUT's own ports.
  void CheckConservation(const std::string& where) {
    Require(push_ctr() == pop_ctr() + kill_ctr() + count(), where,
            "conservation broken: o_push_ctr " + Dec(push_ctr()) + " != o_pop_ctr " +
                Dec(pop_ctr()) + " + o_kill_ctr " + Dec(kill_ctr()) + " + o_count " +
                Dec(count()) + " (a lost push, a duplicate pop or a pop that was never pushed "
                "is exactly this gap)");
    Require(kill_total() == kill_ctr() + kill_offer_ctr(), where,
            "o_kill_total " + Dec(kill_total()) + " != o_kill_ctr " + Dec(kill_ctr()) +
                " + o_kill_offer_ctr " + Dec(kill_offer_ctr()));
  }

  void UpdateCoverage(const Stim& s) {
    const uint32_t push_now = push_ctr();
    const uint32_t pop_now = pop_ctr();
    const uint32_t kill_now = kill_ctr();
    const uint32_t off_now = kill_offer_ctr();
    if (push_now >= last_push_) pushes_ += push_now - last_push_;
    if (pop_now >= last_pop_) pops_ += pop_now - last_pop_;
    if (kill_now >= last_kill_) kills_ += kill_now - last_kill_;
    if (off_now >= last_off_) kill_offers_ += off_now - last_off_;
    last_push_ = push_now;
    last_pop_ = pop_now;
    last_kill_ = kill_now;
    last_off_ = off_now;

    uint32_t refused = 0;
    uint32_t served = 0;
    for (uint32_t p = 0; p < codec_.geom().producers; p++) {
      if (!s.p_valid[p]) continue;
      if (observed_.p_ready[p]) {
        served++;
        if (s.p_pay[p].exc) exc_pushes_++;
      } else {
        refused++;
      }
    }
    refusals_ += refused;
    if (served > 0 && refused > 0) collisions_++;
    if (observed_.c_valid && s.c_ready && observed_.c_pay.exc) exc_pops_++;
  }

  std::vector<bool> RunLength(uint32_t live) const {
    std::vector<bool> out(codec_.geom().entries, false);
    for (uint32_t j = 0; j < live && j < codec_.geom().entries; j++) out[j] = true;
    return out;
  }

  static uint32_t FirstDiff(const std::vector<bool>& a, const std::vector<bool>& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; i++) {
      if (a[i] != b[i]) return (uint32_t)i;
    }
    return (uint32_t)n;
  }

  std::string DescribeStim(const Stim& s) const {
    std::string out = "[";
    for (uint32_t p = 0; p < codec_.geom().producers; p++) {
      out += "p" + Dec(p) + "=" + Bool(s.p_valid[p]);
      if (s.p_valid[p]) out += s.p_pay[p].str();
      out += " ";
    }
    out += "c_ready=" + Bool(s.c_ready) + " kill=" + Bool(s.kill_valid) +
           (s.kill_all ? " all" : (" " + Dec(s.kill_index) + "/" + Dec(s.kill_gen))) + "]";
    return out;
  }

  Vmosaic_result_fifo_tb* dut_;
  mosaic::ClockDriver* clk_;
  Codec codec_;
  uint64_t max_cycles_;
  Shadow* shadow_ = nullptr;
  std::string phase_;
  Observed observed_;
  uint64_t comparisons_ = 0;
  uint64_t phase_comparisons_ = 0;
  uint64_t cycles_ = 0;
  uint64_t phase_cycles_ = 0;
  // Coverage, all of it derived from the DUT's own counters and outputs.
  uint64_t pushes_ = 0, pops_ = 0, kills_ = 0, kill_offers_ = 0;
  uint64_t refusals_ = 0, collisions_ = 0, exc_pushes_ = 0, exc_pops_ = 0;
  uint32_t last_push_ = 0, last_pop_ = 0, last_kill_ = 0, last_off_ = 0;
};

// ---------------------------------------------------------------------- phases

void PhaseResetState(Harness* h, mosaic::Reporter* rep, const Geom& g) {
  h->Reset(4);
  h->Phase("reset-state");

  h->Cycle(Idle(g));

  Require(h->count() == 0, "reset-state", "the queue is not empty at reset");
  Require(!h->prev().c_valid, "reset-state", "c_valid is high with an empty queue");
  for (uint32_t p = 0; p < g.producers; p++) {
    Require(!h->prev().p_ready[p], "reset-state",
            "p_ready[" + Dec(p) + "] is high at reset: a producer would think an accepted "
            "result was stored");
  }
  Require(h->push_ctr() == 0 && h->pop_ctr() == 0 && h->kill_ctr() == 0 &&
              h->kill_offer_ctr() == 0,
          "reset-state", "a counter is nonzero at reset");
  for (uint32_t j = 0; j < g.entries; j++) {
    Require(!h->occ()[j], "reset-state", "occupancy bit " + Dec(j) + " is set at reset");
    Require(!h->exc_occ()[j], "reset-state",
            "exception occupancy bit " + Dec(j) + " is set at reset");
  }
  rep->Check(true, "reset-state: empty, no head, every producer refused, counters zero");
}

// Every producer completes in the same cycle with room for exactly one.
void PhaseCollision(Harness* h, mosaic::Reporter* rep, const Geom& g) {
  h->Reset(4);
  h->Phase("collision");
  Entries ent(g);

  std::vector<Entry> accepted;
  std::vector<Entry> delivered;

  // Fill to ENTRIES-1 so exactly one slot is free, through producer 0.
  for (uint32_t i = 0; i + 1 < g.entries; i++) {
    Stim s = Idle(g);
    s.p_valid[0] = true;
    s.p_pay[0] = ent.Make(false);
    h->Cycle(s);
    Require(h->prev().p_ready[0], "collision", "a pre-fill push into a free slot was refused");
    accepted.push_back(s.p_pay[0]);
  }
  Require(h->count() == g.entries - 1, "collision",
          "the pre-fill did not fill to ENTRIES-1: count " + Dec(h->count()));

  // All producers complete at once, the last of them exceptionally.
  std::vector<Entry> offered;
  for (uint32_t p = 0; p < g.producers; p++) offered.push_back(ent.Make(p == g.producers - 1));
  Stim s = Idle(g);
  for (uint32_t p = 0; p < g.producers; p++) {
    s.p_valid[p] = true;
    s.p_pay[p] = offered[p];
  }
  h->Cycle(s);

  uint32_t served = 0;
  for (uint32_t p = 0; p < g.producers; p++) {
    if (h->prev().p_ready[p]) served++;
  }
  Require(served == 1, "collision",
          "with one free slot and " + Dec(g.producers) +
              " simultaneous completions, expected exactly one acceptance, got " + Dec(served));
  Require(h->prev().p_ready[0], "collision",
          "fixed priority by producer index must accept producer 0 first");
  Require(!h->prev().p_ready[g.producers - 1], "collision",
          "the exception-bearing producer must have lost the collision");
  Require(h->count() == g.entries, "collision",
          "the queue is not full after the collision: count " + Dec(h->count()));
  accepted.push_back(offered[0]);

  // Every loser is still offering the same payload. Re-present it unchanged and
  // require it to enter unchanged, draining when the queue is full.
  std::vector<bool> still(g.producers, true);
  still[0] = false;
  for (uint32_t guard = 0; guard < 64; guard++) {
    bool pending = false;
    for (uint32_t p = 0; p < g.producers; p++) pending = pending || still[p];
    if (!pending) break;

    Stim t = Idle(g);
    for (uint32_t p = 0; p < g.producers; p++) {
      if (still[p]) {
        t.p_valid[p] = true;
        t.p_pay[p] = offered[p];  // unchanged, exactly as held across the collision
      }
    }
    h->Cycle(t);
    bool progressed = false;
    for (uint32_t p = 0; p < g.producers; p++) {
      if (still[p] && h->prev().p_ready[p]) {
        still[p] = false;
        accepted.push_back(offered[p]);
        progressed = true;
      }
    }
    if (!progressed) {
      Require(h->count() == g.entries, "collision",
              "a producer was refused with a free slot: nothing was accepted and the queue is "
              "not full");
      Stim d = Idle(g);
      d.c_ready = true;
      h->Cycle(d);
      Require(h->prev().c_valid, "collision", "a drain of a full queue presented nothing");
      delivered.push_back(h->prev().c_pay);
    }
    Require(guard != 63, "collision", "the collision losers never entered the queue");
  }

  // Drain and compare the whole delivery order and content.
  while (h->count() != 0) {
    Stim d = Idle(g);
    d.c_ready = true;
    h->Cycle(d);
    Require(h->prev().c_valid, "collision", "a drain presented nothing while count != 0");
    delivered.push_back(h->prev().c_pay);
  }
  Require(delivered == accepted, "collision",
          "the delivery sequence differs from the acceptance sequence: expected " +
              DescribeSeq(accepted) + ", got " + DescribeSeq(delivered));

  bool exc_survived = false;
  for (const Entry& e : delivered) {
    if (e == offered[g.producers - 1]) exc_survived = true;
  }
  Require(exc_survived, "collision",
          "the exception-bearing result did not survive the collision intact: expected " +
              offered[g.producers - 1].str() + " somewhere in " + DescribeSeq(delivered));
  Require(offered[g.producers - 1].exc, "collision",
          "the collision phase's last producer is supposed to be exceptional");

  rep->Check(true, "collision: one slot, " + Dec(g.producers) +
                       " simultaneous completions, one accepted, losers held and delivered "
                       "unchanged in priority order, exception intact");
}

void PhaseFillDrain(Harness* h, mosaic::Reporter* rep, const Geom& g) {
  h->Reset(4);
  h->Phase("fill-drain");
  Entries ent(g);

  std::vector<Entry> expected;
  // Fill the queue through a rotating producer, interleaving exceptions.
  for (uint32_t j = 0; j < g.entries; j++) {
    const uint32_t p = j % g.producers;
    Stim s = Idle(g);
    s.p_valid[p] = true;
    s.p_pay[p] = ent.Make((j % 2) == 1);
    h->Cycle(s);
    Require(h->prev().p_ready[p], "fill-drain", "a fill push was refused with free slots");
    expected.push_back(s.p_pay[p]);
  }
  Require(h->count() == g.entries, "fill-drain", "the fill did not fill the queue");

  // Keep pushing while full: everything is refused, nothing is lost and nothing
  // is counted as stored.
  const uint32_t before_push = h->push_ctr();
  for (uint32_t k = 0; k < 4 * g.producers; k++) {
    const uint32_t p = k % g.producers;
    Stim s = Idle(g);
    s.p_valid[p] = true;
    s.p_pay[p] = ent.Make(false);
    h->Cycle(s);
    Require(!h->prev().p_ready[p], "fill-drain",
            "a push into a full queue was accepted: the queue would have to overflow");
  }
  Require(h->count() == g.entries, "fill-drain", "the queue changed size while full");
  Require(h->push_ctr() == before_push, "fill-drain",
          "a refused push advanced o_push_ctr: a refused result was counted as stored");

  // Back-pressure: the consumer stalls, so valid and payload must not move.
  const Entry stable = h->entries().front();
  for (int k = 0; k < 3; k++) {
    Stim s = Idle(g);
    s.c_ready = false;
    h->Cycle(s);
    Require(h->prev().c_valid, "fill-drain", "c_valid dropped under back-pressure");
    Require(h->prev().c_pay == stable, "fill-drain",
            "c_pay moved under back-pressure: expected " + stable.str() + ", got " +
                h->prev().c_pay.str());
    Require(h->count() == g.entries, "fill-drain", "the queue drained with c_ready low");
  }

  // Drain everything and compare order and content.
  std::vector<Entry> delivered;
  while (h->count() != 0) {
    Stim s = Idle(g);
    s.c_ready = true;
    h->Cycle(s);
    Require(h->prev().c_valid, "fill-drain", "a drain presented nothing");
    delivered.push_back(h->prev().c_pay);
  }
  Require(delivered == expected, "fill-drain",
          "the drained sequence differs: expected " + DescribeSeq(expected) + ", got " +
              DescribeSeq(delivered));

  rep->Check(true, "fill-drain: filled, " + Dec(4 * g.producers) +
                       " refused pushes with nothing lost, 3 stalled cycles with a stable "
                       "head, drained in order with exceptions interleaved");
}

void PhaseKill(Harness* h, mosaic::Reporter* rep, const Geom& g) {
  h->Reset(4);
  h->Phase("kill");
  Entries ent(g);
  std::vector<Entry> delivered;

  // Two buffered entries: the head matches the kill, the tail (exceptional) does
  // not.
  Entry head = ent.Make(false);
  Entry tail = ent.Make(true);
  {
    Stim s = Idle(g);
    s.p_valid[0] = true;
    s.p_pay[0] = head;
    h->Cycle(s);
    Require(h->prev().p_ready[0], "kill", "the first buffered push was refused");
  }
  {
    const uint32_t p = 1 % g.producers;
    Stim s = Idle(g);
    s.p_valid[p] = true;
    s.p_pay[p] = tail;
    h->Cycle(s);
    Require(h->prev().p_ready[p], "kill", "the second buffered push was refused");
  }
  Require(h->count() == 2, "kill", "expected two buffered entries, got " + Dec(h->count()));

  // A kill naming the head's macro, while one producer offers a *different uop of
  // the same macro* (also killed, so absorbed) and another offers an unrelated
  // result (refused this cycle, because the kill frees nothing until the edge,
  // and taken the cycle after).
  Entry same_macro = head;
  same_macro.uop_index = (head.uop_index + 1) & g.uop_mask;
  same_macro.data = head.data ^ 0x5a5a5a5aull;  // a different value, same macro
  Entry other = ent.Make(false);

  const uint32_t p_killed_offer = (g.producers >= 3) ? 2 : 1;
  const uint32_t p_other_offer = 0;
  Stim s = Idle(g);
  s.kill_valid = true;
  s.kill_index = head.rob_index;
  s.kill_gen = head.rob_gen;
  s.p_valid[p_killed_offer] = true;
  s.p_pay[p_killed_offer] = same_macro;
  s.p_valid[p_other_offer] = true;
  s.p_pay[p_other_offer] = other;
  h->Cycle(s);

  Require(h->prev().p_ready[p_killed_offer], "kill",
          "an offering result matching the kill must be absorbed and ready, not held: a held "
          "dead result is a producer that can never be released");
  Require(!h->prev().p_ready[p_other_offer], "kill",
          "a push was accepted in the cycle of a kill with a full queue: the kill must not "
          "free a slot for a same-cycle push");
  Require(h->kill_ctr() == 1, "kill",
          "expected exactly one buffered entry dropped by the kill, got " + Dec(h->kill_ctr()));
  Require(h->kill_offer_ctr() == 1, "kill",
          "expected exactly one absorbed in-flight offer, got " + Dec(h->kill_offer_ctr()));
  Require(h->count() == 1, "kill", "expected 1 entry after the kill, got " + Dec(h->count()));
  Require(h->entries()[0] == tail, "kill",
          "the surviving entry is not the exceptional tail: got " + h->entries()[0].str());

  // The unrelated offer is taken now that the kill's freed slot exists.
  {
    Stim t = Idle(g);
    t.p_valid[p_other_offer] = true;
    t.p_pay[p_other_offer] = other;
    h->Cycle(t);
    Require(h->prev().p_ready[p_other_offer], "kill",
            "the unrelated offer was still refused with a free slot after the kill");
  }
  Require(h->count() == 2 && h->entries()[1] == other, "kill",
          "the unrelated offer did not land behind the survivor");

  // Drain and require the exceptional survivor and the unrelated offer, in that
  // order, with no killed identity among them.
  while (h->count() != 0) {
    Stim d = Idle(g);
    d.c_ready = true;
    h->Cycle(d);
    Require(h->prev().c_valid, "kill", "a drain after the kill presented nothing");
    Require(!h->shadow().IsKilled(h->prev().c_pay), "kill",
            "a killed result was delivered: " + h->prev().c_pay.str());
    delivered.push_back(h->prev().c_pay);
  }
  Require(delivered.size() == 2 && delivered[0] == tail && delivered[1] == other, "kill",
          "the post-kill delivery was wrong: got " + DescribeSeq(delivered));

  // kill_all: two buffered entries, one offering producer, everything dies.
  h->Reset(4);
  h->Phase("kill-all");
  Entries ent2(g);
  const uint32_t buffered = (g.entries < 2) ? g.entries : 2;
  for (uint32_t j = 0; j < buffered; j++) {
    const uint32_t p = j % g.producers;
    Stim f = Idle(g);
    f.p_valid[p] = true;
    f.p_pay[p] = ent2.Make(j == 1);
    h->Cycle(f);
  }
  Stim ka = Idle(g);
  ka.kill_valid = true;
  ka.kill_all = true;
  ka.p_valid[0] = true;
  ka.p_pay[0] = ent2.Make(true);
  h->Cycle(ka);
  Require(h->count() == 0, "kill-all", "kill_all left " + Dec(h->count()) + " entries");
  Require(h->kill_ctr() == buffered, "kill-all",
          "kill_all did not count every buffered drop: " + Dec(h->kill_ctr()) + " of " +
              Dec(buffered));
  Require(h->kill_offer_ctr() == 1, "kill-all", "the absorbed offer was not counted");
  Require(!h->prev().c_valid, "kill-all", "c_valid is high after kill_all emptied the queue");

  // A kill that matches nothing leaves the queue alone.
  h->Reset(4);
  h->Phase("kill-miss");
  Entries ent3(g);
  Entry keep = ent3.Make(false);
  {
    Stim f = Idle(g);
    f.p_valid[0] = true;
    f.p_pay[0] = keep;
    h->Cycle(f);
  }
  const Entry stranger = ent3.Make(false);
  Stim km = Idle(g);
  km.kill_valid = true;
  km.kill_index = (stranger.rob_index + 1) & g.kill_index_mask;
  km.kill_gen = (stranger.rob_gen + 1) & g.kill_gen_mask;
  h->Cycle(km);
  Require(h->kill_ctr() == 0, "kill-miss", "a kill that matched nothing dropped an entry");
  Require(h->count() == 1 && h->entries()[0] == keep, "kill-miss",
          "a kill that matched nothing disturbed the queue");

  rep->Check(true, "kill: matching buffered drop counted, matching in-flight offer absorbed "
                   "and counted, unrelated result refused then taken, near-miss kill inert, no "
                   "killed result delivered");
}

// The other half of the back-pressure contract: a same-cycle pop does not free a
// slot for a same-cycle push, because `*_ready` never consults `c_ready`. The
// shadow encodes that rule, so this phase makes it a named, directed assertion
// rather than something only the soak would catch.
void PhaseSameCyclePopPush(Harness* h, mosaic::Reporter* rep, const Geom& g) {
  h->Reset(4);
  h->Phase("pop-push-same-cycle");
  Entries ent(g);

  std::vector<Entry> expected;
  std::vector<Entry> delivered;
  for (uint32_t j = 0; j < g.entries; j++) {
    Stim s = Idle(g);
    s.p_valid[0] = true;
    s.p_pay[0] = ent.Make(j % 2 == 1);
    h->Cycle(s);
    Require(h->prev().p_ready[0], "pop-push-same-cycle", "a fill push was refused");
    expected.push_back(s.p_pay[0]);
  }

  const Entry next = ent.Make(true);
  Stim s = Idle(g);
  s.c_ready = true;
  s.p_valid[0] = true;
  s.p_pay[0] = next;
  h->Cycle(s);
  Require(h->prev().c_valid, "pop-push-same-cycle", "the consumer was ready but nothing was "
                                                    "presented by a non-empty queue");
  Require(!h->prev().p_ready[0], "pop-push-same-cycle",
          "a push was accepted in the same cycle the consumer emptied a slot: *_ready depends "
          "on the consumer, which the contract forbids (the same rule that keeps the writeback "
          "arbiter off the execution units' critical path)");
  Require(h->count() == g.entries - 1, "pop-push-same-cycle",
          "expected the pop only, count " + Dec(h->count()));
  delivered.push_back(h->prev().c_pay);
  expected.push_back(next);

  // The same stimulus one cycle later is accepted (and, because the consumer is
  // still ready, the entry behind the first is delivered in the same cycle the
  // push now finds the pre-existing free slot).
  h->Cycle(s);
  Require(h->prev().p_ready[0], "pop-push-same-cycle",
          "the push refused while the pop happened was not accepted the cycle after; the "
          "producer would be stuck holding a result the queue has room for");
  if (h->prev().c_valid && s.c_ready) delivered.push_back(h->prev().c_pay);

  while (h->count() != 0) {
    Stim d = Idle(g);
    d.c_ready = true;
    h->Cycle(d);
    Require(h->prev().c_valid, "pop-push-same-cycle", "a drain presented nothing");
    delivered.push_back(h->prev().c_pay);
  }
  Require(delivered == expected, "pop-push-same-cycle",
          "delivery order wrong: expected " + DescribeSeq(expected) + ", got " +
              DescribeSeq(delivered));

  rep->Check(true, "pop-push-same-cycle: a full queue's pop did not admit a same-cycle push, "
                   "and the refused result was taken the next cycle");
}

// The card's Fail criterion, first half: a producer may have to hold its result.
void PhaseValidPulse(Harness* h, mosaic::Reporter* rep, const Geom& g) {
  h->Reset(4);
  h->Phase("valid-pulse");
  Entries ent(g);

  for (uint32_t j = 0; j < g.entries; j++) {
    Stim s = Idle(g);
    s.p_valid[0] = true;
    s.p_pay[0] = ent.Make(false);
    h->Cycle(s);
  }
  Require(h->count() == g.entries, "valid-pulse", "the fill did not fill the queue");

  // Present a result and hold it, with the consumer stalled: it must be refused
  // every cycle and must not be sampled, re-sampled or lost.
  const uint32_t held_producer = (g.producers >= 2) ? 1 : 0;
  const Entry held = ent.Make(true);
  const uint32_t push_before = h->push_ctr();
  for (int k = 0; k < 5; k++) {
    Stim s = Idle(g);
    s.p_valid[held_producer] = true;
    s.p_pay[held_producer] = held;
    h->Cycle(s);
    Require(!h->prev().p_ready[held_producer], "valid-pulse",
            "a held result was accepted while the queue was full: the queue would have to "
            "overflow, or the producer was told yes and the result was dropped");
    Require(h->count() == g.entries, "valid-pulse", "the full queue changed size");
    const std::vector<Entry> now = h->entries();
    bool present = false;
    for (const Entry& e : now) present = present || (e == held);
    Require(!present, "valid-pulse",
            "a held, refused result appeared in the queue: it was sampled without a transfer");
  }
  Require(h->push_ctr() == push_before, "valid-pulse",
          "a refused, held result advanced the push counter");

  // Make room; the held result must now be accepted, byte-identical.
  {
    Stim d = Idle(g);
    d.c_ready = true;
    h->Cycle(d);
    Require(h->prev().c_valid, "valid-pulse", "the drain presented nothing");
  }
  {
    Stim s = Idle(g);
    s.p_valid[held_producer] = true;
    s.p_pay[held_producer] = held;
    h->Cycle(s);
    Require(h->prev().p_ready[held_producer], "valid-pulse",
            "the held result was still refused after room appeared");
  }
  bool present = false;
  for (const Entry& e : h->entries()) present = present || (e == held);
  Require(present, "valid-pulse", "the held result did not enter the queue intact: " + held.str());

  rep->Check(true, "valid-pulse: a result held across 5 refused cycles was neither sampled nor "
                   "lost, and entered unchanged once there was room (multiply and load "
                   "returning together is the collision phase)");
}

void PhaseRandom(Harness* h, mosaic::Reporter* rep, const Geom& g, uint64_t seed, int cycles) {
  h->Reset(4);
  h->Phase("random");
  mosaic::Rng rng(seed);
  Entries ent(g);

  std::vector<bool> busy(g.producers, false);
  std::vector<Entry> pending(g.producers);

  for (int i = 0; i < cycles; i++) {
    Stim s = Idle(g);
    for (uint32_t p = 0; p < g.producers; p++) {
      if (!busy[p] && rng.Chance(45)) {
        busy[p] = true;
        pending[p] = ent.Make(rng.Chance(20));
      }
      if (busy[p]) {
        s.p_valid[p] = true;
        s.p_pay[p] = pending[p];
      }
    }
    s.c_ready = rng.Chance(60);
    s.kill_valid = rng.Chance(12);
    if (s.kill_valid) {
      s.kill_all = rng.Chance(25);
      if (!s.kill_all) {
        // Aim at something real often enough to exercise the matcher: a buffered
        // entry, an in-flight producer's macro, or a random identity.
        const uint32_t pick = rng.Below(10);
        if (pick < 5 && h->count() > 0) {
          const Entry e = h->entries()[rng.Below(h->count())];
          s.kill_index = e.rob_index;
          s.kill_gen = e.rob_gen;
        } else if (pick < 8) {
          uint32_t live = 0;
          for (uint32_t p = 0; p < g.producers; p++) live += busy[p] ? 1 : 0;
          if (live > 0) {
            uint32_t want = rng.Below(live);
            for (uint32_t p = 0; p < g.producers; p++) {
              if (!busy[p]) continue;
              if (want == 0) {
                s.kill_index = pending[p].rob_index;
                s.kill_gen = pending[p].rob_gen;
                break;
              }
              want--;
            }
          } else {
            s.kill_index = rng.Below(g.kill_index_mask + 1);
            s.kill_gen = rng.Below(g.kill_gen_mask + 1);
          }
        } else {
          s.kill_index = rng.Below(g.kill_index_mask + 1);
          s.kill_gen = rng.Below(g.kill_gen_mask + 1);
        }
      }
    }

    h->Cycle(s);

    for (uint32_t p = 0; p < g.producers; p++) {
      if (busy[p] && h->prev().p_ready[p]) busy[p] = false;
    }
  }

  // Drain whatever is left, so the campaign ends at a defined state.
  while (h->count() != 0) {
    Stim d = Idle(g);
    d.c_ready = true;
    h->Cycle(d);
    Require(h->prev().c_valid, "random", "a final drain presented nothing");
    Require(!h->shadow().IsKilled(h->prev().c_pay), "random",
            "a killed result was delivered: " + h->prev().c_pay.str());
  }

  // Anti-vacuity: a campaign in which nothing happened proves nothing. The
  // thresholds are what the phase is *for*, not floors that happen to pass.
  Require(h->pushes() > 0, "random", "no result was ever pushed");
  Require(h->pops() > 0, "random", "no result was ever delivered");
  Require(h->kills() > 0, "random", "no buffered entry was ever killed");
  Require(h->kill_offers() > 0, "random", "no in-flight offer was ever killed");
  Require(h->refusals() > (uint64_t)cycles / 8, "random",
          "only " + Dec(h->refusals()) + " refused offers over " + Dec(cycles) +
              " cycles: the campaign was not exercising back-pressure");
  Require(h->collisions() > (uint64_t)cycles / 20, "random",
          "only " + Dec(h->collisions()) + " contended cycles over " + Dec(cycles) +
              ": the campaign was not producing collisions");
  Require(h->exc_pushes() > 0, "random", "no exceptional result was ever pushed");
  Require(h->exc_pops() > 0, "random", "no exceptional result was ever delivered");
  Require(h->shadow().killed_count() > 0, "random",
          "no identity was ever brought under a kill");

  rep->Check(true, "random: " + Dec(h->pushes()) + " pushes, " + Dec(h->pops()) + " pops, " +
                       Dec(h->kills()) + " buffered kills, " + Dec(h->kill_offers()) +
                       " absorbed offers, " + Dec(h->refusals()) + " refused offers, " +
                       Dec(h->collisions()) + " contended cycles, " + Dec(h->exc_pushes()) +
                       "/" + Dec(h->exc_pops()) + " exceptional pushes/pops over " +
                       Dec(cycles) + " cycles");
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
  Vmosaic_result_fifo_tb dut;

  std::string detail;
  bool passed = true;

  try {
    dut.eval();  // settle the geometry read-back before sizing anything

    // The geometry comes from the elaborated DUT, never from a literal here.
    Geom g;
    g.entries = dut.o_dut_entries;
    g.producers = dut.o_dut_producers;
    g.entry_w = dut.o_dut_entry_w;
    g.cnt_w = dut.o_dut_cnt_w;
    g.xlen = dut.o_dut_xlen;
    g.rob_index_w = dut.o_dut_rob_index_w;
    g.rob_gen_w = dut.o_dut_rob_gen_w;
    g.uop_w = dut.o_dut_uop_w;
    g.kill_index_mask = (1u << g.rob_index_w) - 1u;
    g.kill_gen_mask = (1u << g.rob_gen_w) - 1u;
    g.uop_mask = (1u << g.uop_w) - 1u;
    DeriveOffsets(&g);

    // The wrapper's re-expression of the geometry must agree with the DUT's own
    // localparams field by field: a disagreement here is how a profile change
    // would otherwise narrow a port silently.
    Require(dut.o_tb_entries == g.entries, "geometry",
            "wrapper entries (" + Dec(dut.o_tb_entries) + ") != DUT entries (" + Dec(g.entries) +
                ")");
    Require(dut.o_tb_producers == g.producers, "geometry",
            "wrapper producers (" + Dec(dut.o_tb_producers) + ") != DUT producers (" +
                Dec(g.producers) + ")");
    Require(dut.o_tb_entry_w == g.entry_w, "geometry",
            "wrapper entry width (" + Dec(dut.o_tb_entry_w) + ") != DUT entry width (" +
                Dec(g.entry_w) + ")");
    Require(dut.o_tb_cnt_w == g.cnt_w, "geometry",
            "wrapper count width (" + Dec(dut.o_tb_cnt_w) + ") != DUT count width (" +
                Dec(g.cnt_w) + ")");
    Require(dut.o_tb_xlen == g.xlen, "geometry",
            "wrapper XLEN (" + Dec(dut.o_tb_xlen) + ") != DUT XLEN (" + Dec(g.xlen) + ")");
    Require(dut.o_tb_rob_index_w == g.rob_index_w, "geometry",
            "wrapper rob_index width != DUT rob_index width");
    Require(dut.o_tb_rob_gen_w == g.rob_gen_w, "geometry",
            "wrapper rob_gen width != DUT rob_gen width");
    Require(dut.o_tb_uop_w == g.uop_w, "geometry", "wrapper uop width != DUT uop width");

    Require(g.entry_w == g.rob_index_w + g.rob_gen_w + g.uop_w + 3 * g.xlen + 1, "geometry",
            "the payload width " + Dec(g.entry_w) +
                " is not the sum of its fields: a field would be silently shared with its "
                "neighbour");
    Require(g.entries >= 1, "geometry", "the result FIFO has no entries");
    Require(g.producers >= 2, "geometry",
            "fewer than two producers makes the collision case vacuous");
    Require(g.cnt_w == (uint32_t)(32 - __builtin_clz((unsigned)g.entries)), "geometry",
            "the occupancy counter width " + Dec(g.cnt_w) + " is not the width of " +
                Dec(g.entries) + " entries");

    Codec codec(g);
    Shadow shadow(g);
    Harness harness(&dut, &clk, codec, options.max_cycles);

    auto fresh = [&]() {
      harness.Reset(4);
      shadow.Reset();
      harness.BindShadow(&shadow);
    };

    fresh();
    PhaseResetState(&harness, &reporter, g);

    fresh();
    PhaseCollision(&harness, &reporter, g);

    fresh();
    PhaseFillDrain(&harness, &reporter, g);

    fresh();
    PhaseKill(&harness, &reporter, g);

    fresh();
    PhaseSameCyclePopPush(&harness, &reporter, g);

    fresh();
    PhaseValidPulse(&harness, &reporter, g);

    fresh();
    PhaseRandom(&harness, &reporter, g, options.seed, 30000);

    detail = "result fifo contract holds: " + std::to_string(harness.comparisons()) +
             " per-cycle whole-state comparisons over " + std::to_string(harness.cycles()) +
             " cycles, " + std::to_string(g.entries) + " entries / " +
             std::to_string(g.producers) + " producers / " + std::to_string(g.entry_w) +
             "-bit payload, seed " + std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
