// CASE=fifo.backpressure -- work package I-005.
//
// Drives the four `mosaic_fifo` instances (DEPTH = 1, 2, 3, 8) and the
// `mosaic_skid_buffer` in sim/tb/mosaic_fifo_tb.sv through interleaved
// push/pop, full, empty, stall and mid-stall reset stimulus, and checks every
// one of them, every cycle, against an *independent* shadow model.
//
// Independence of the shadow model is the point of this file. The DUT is a ring
// of two pointers plus an occupancy counter; the shadow model below is a
// `std::deque` of payload values with its own full/empty predicate. Nothing here
// reuses the DUT's arithmetic, so a bug in the pointer wrap, the full/empty
// compare or the occupancy update cannot cancel out against the same bug in the
// checker.
//
// Checks run every cycle on every instance:
//   * `in_ready`    equals (items < depth); for the fall-through skid buffer,
//                   (empty || draining)
//   * `out_valid`   equals the model's prediction
//   * `count`       equals the number of items held
//   * `out_payload` equals the *first-in* item -- the packet sequence check, and
//     during a stall the "no stall may change the head item" check
//   * payload stability: if the previous cycle was out_valid && !out_ready,
//     this cycle must still be out_valid with an unchanged payload
//   * conservation: accepted == emitted + buffered + explicitly_cancelled
//   * a FIFO's presented outputs do not move when `out_ready` is perturbed,
//     which is what makes its input ready safe on a long combinational ready
//     path (the skid buffer deliberately does move, and is excluded)
//
// Coverage is counted and asserted at the end, so a stimulus change that stops
// reaching full / empty / stall / backpressure fails the case instead of
// quietly passing over a shorter path.

#include "sim_common.h"
#include "verilated.h"
#include "Vmosaic_fifo_tb.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Reset length. Fixed and documented here, per the sim/common convention: the
// DUT needs nothing beyond "at least one rising edge with rst high".
constexpr int kResetCycles = 4;

// Directed phase lengths. Sized so the whole case stays far below the
// registry's 20000-cycle budget.
constexpr int kFillCycles = 12;    // out_ready low: must reach full and stay full
constexpr int kDrainCycles = 12;   // out_ready high, no pushes: must reach empty
constexpr int kSteadyCycles = 24;  // push and pop every cycle
constexpr int kStallCycles = 10;   // output stalled hard while still pushing
constexpr int kRandomCycles = 3000;
constexpr int kStallBeforeReset = 4;  // cycles stalled immediately before a reset
constexpr int kResetBurst = 2;        // cycles with rst asserted
constexpr int kFreeCyclesPerReset = 500;

constexpr unsigned kMaxReports = 12;
constexpr uint32_t kInstanceCount = 5;  // four fifos + the skid buffer

struct Shadow {
  const char* name = "";
  unsigned depth = 1;
  // `mosaic_skid_buffer` is fall-through: it presents the input directly while
  // it holds nothing, and accepts whenever it is empty or being drained.
  bool fall_through = false;

  std::deque<uint32_t> items;
  uint64_t accepted = 0;
  uint64_t emitted = 0;
  uint64_t cancelled = 0;

  bool stalled = false;         // previous cycle was out_valid && !out_ready
  uint32_t stalled_payload = 0;

  // Per-instance coverage.
  uint64_t full_cycles = 0;
  uint64_t empty_cycles = 0;
  uint64_t stall_cycles = 0;
  uint64_t simultaneous_cycles = 0;
  uint64_t backpressure_cycles = 0;

  bool ExpectedReady(bool out_ready) const {
    if (fall_through) return items.empty() || out_ready;
    return items.size() < depth;
  }

  bool ExpectedOutValid(bool in_valid) const {
    if (fall_through) return !items.empty() || in_valid;
    return !items.empty();
  }

  // Reset drops everything held. Those items are the "explicitly cancelled"
  // term of the conservation invariant, counted once, here, and nowhere else.
  void Cancel() {
    cancelled += items.size();
    items.clear();
    stalled = false;
    stalled_payload = 0;
  }
};

// Verilator exposes every top-level port as a member; the driver reaches them
// through pointers so all five instances share one code path.
struct Pins {
  uint8_t* in_valid = nullptr;
  uint8_t* in_ready = nullptr;
  uint32_t* in_payload = nullptr;
  uint8_t* out_valid = nullptr;
  uint8_t* out_ready = nullptr;
  uint32_t* out_payload = nullptr;
  uint8_t* count = nullptr;  // null for the skid buffer
};

struct Instance {
  Shadow shadow;
  Pins pins;
  // The producer's next item. It advances only on a transfer that really
  // happened, which is what keeps the producer side of the protocol legal: the
  // payload cannot change while valid is high and ready is low.
  uint32_t next_payload = 1;
  mosaic::Rng rng{1};
};

class Bench {
 public:
  Bench(const mosaic::Options& options, mosaic::Reporter& reporter,
        Vmosaic_fifo_tb* top)
      : rep_(reporter), top_(top) {
    auto add = [&](const char* name, unsigned depth, bool fall_through,
                   uint64_t salt, Pins pins) {
      Instance inst;
      inst.shadow.name = name;
      inst.shadow.depth = depth;
      inst.shadow.fall_through = fall_through;
      inst.pins = pins;
      inst.next_payload = 1;
      inst.rng = mosaic::Rng(options.seed + salt);
      inst_.push_back(inst);
    };

    add("fifo_d1", 1, false, 0x9e37u,
        Pins{&top->d1_in_valid, &top->d1_in_ready, &top->d1_in_payload,
             &top->d1_out_valid, &top->d1_out_ready, &top->d1_out_payload,
             &top->d1_count});
    add("fifo_d2", 2, false, 0x9e37u * 2,
        Pins{&top->d2_in_valid, &top->d2_in_ready, &top->d2_in_payload,
             &top->d2_out_valid, &top->d2_out_ready, &top->d2_out_payload,
             &top->d2_count});
    add("fifo_d3", 3, false, 0x9e37u * 3,
        Pins{&top->d3_in_valid, &top->d3_in_ready, &top->d3_in_payload,
             &top->d3_out_valid, &top->d3_out_ready, &top->d3_out_payload,
             &top->d3_count});
    add("fifo_d8", 8, false, 0x9e37u * 4,
        Pins{&top->d8_in_valid, &top->d8_in_ready, &top->d8_in_payload,
             &top->d8_out_valid, &top->d8_out_ready, &top->d8_out_payload,
             &top->d8_count});
    add("skid", 1, true, 0x9e37u * 5,
        Pins{&top->sk_in_valid, &top->sk_in_ready, &top->sk_in_payload,
             &top->sk_out_valid, &top->sk_out_ready, &top->sk_out_payload,
             nullptr});
  }

  // ---- cycle engine ------------------------------------------------------

  uint64_t cycles() const { return cycles_; }
  uint64_t failures() const { return failures_; }
  int checks() const { return checks_; }
  uint64_t reset_cycles() const { return reset_cycles_; }
  bool Resetting() const { return reset_remaining_ > 0; }
  void PulseReset(int cycles) { reset_remaining_ = cycles; }

  // One cycle with the same stimulus applied to all four FIFOs; the skid buffer
  // gets its own so the two families can be exercised in different states.
  void Step(bool want_fifo, bool ready_fifo, bool want_skid, bool ready_skid) {
    bool want[kInstanceCount];
    bool ready[kInstanceCount];
    for (uint32_t i = 0; i < kInstanceCount - 1; ++i) {
      want[i] = want_fifo;
      ready[i] = ready_fifo;
    }
    want[kInstanceCount - 1] = want_skid;
    ready[kInstanceCount - 1] = ready_skid;
    RunCycle(want, ready);
  }

  // One cycle where every instance draws its own valid and ready, so a bug that
  // only appears at one depth under one pattern cannot hide behind the others.
  void StepRandomised() {
    bool want[kInstanceCount];
    bool ready[kInstanceCount];
    for (uint32_t i = 0; i < kInstanceCount; ++i) {
      want[i] = inst_[i].rng.Chance(62);
      ready[i] = inst_[i].rng.Chance(55);
    }
    RunCycle(want, ready);
  }

  // The value the skid buffer's producer will offer next; the directed skid
  // checks use it instead of hard-coding a payload, because the skid buffer has
  // been driven by every earlier phase too.
  uint32_t SkidNextPayload() const { return inst_.back().next_payload; }

  // ---- directed assertions ----------------------------------------------

  void CheckPostResetClean(const char* what) {
    for (Instance& inst : inst_) {
      Expect(inst.shadow.name, what, 0, *inst.pins.out_valid);
      Expect(inst.shadow.name, what, 1, *inst.pins.in_ready);
      if (inst.pins.count) {
        Expect(inst.shadow.name, what, 0, *inst.pins.count);
      }
    }
  }

  void CheckAllFull(const char* what) {
    for (Instance& inst : inst_) {
      Expect(inst.shadow.name, what, 0, *inst.pins.in_ready);
      if (inst.pins.count) {
        Expect(inst.shadow.name, what, inst.shadow.depth, *inst.pins.count);
      }
    }
  }

  void CheckAllEmpty(const char* what) {
    for (Instance& inst : inst_) {
      Expect(inst.shadow.name, what, 0, *inst.pins.out_valid);
      Expect(inst.shadow.name, what, 1, *inst.pins.in_ready);
      if (inst.pins.count) Expect(inst.shadow.name, what, 0, *inst.pins.count);
    }
  }

  // The skid buffer's reason for existing, checked as a property rather than
  // only described in prose: with the output stalled it absorbs one more item,
  // presents the first-in item, and refuses a second item while it is held.
  //
  // These read the skid buffer's *within-cycle* observation, captured during
  // the step just run. That is deliberate and is the whole point of a
  // fall-through buffer: `in_ready`, `out_valid` and `out_payload` are
  // functions of this cycle's inputs, so sampling them after the next clock
  // edge would describe a different cycle and assert nothing about this one.
  void SkidExpect(const char* what, bool ready, bool valid, uint32_t payload) {
    Expect("skid", what, ready ? 1 : 0, skid_ready_);
    Expect("skid", what, valid ? 1 : 0, skid_valid_);
    // out_payload is a don't-care when out_valid is 0, so it is not compared.
    if (valid) Expect("skid", what, payload, skid_payload_);
  }

  void ReportCoverage() {
    for (Instance& inst : inst_) {
      const Shadow& s = inst.shadow;
      Require(s.accepted > 0, s.name, "coverage: accepted nothing");
      Require(s.emitted > 0, s.name, "coverage: emitted nothing");
      Require(s.stall_cycles > 0, s.name, "coverage: never stalled the output");
      Require(s.backpressure_cycles > 0, s.name,
              "coverage: never back-pressured a held item");
      if (!s.fall_through) {
        Require(s.full_cycles > 0, s.name, "coverage: never reached full");
        Require(s.empty_cycles > 0, s.name, "coverage: never reached empty");
      }
      CheckConservation(s.name, s);
      std::fprintf(stderr,
                   "  %-8s depth=%u accepted=%llu emitted=%llu cancelled=%llu "
                   "buffered=%zu full=%llu empty=%llu stall=%llu "
                   "push_and_pop=%llu\n",
                   s.name, s.depth, (unsigned long long)s.accepted,
                   (unsigned long long)s.emitted,
                   (unsigned long long)s.cancelled, s.items.size(),
                   (unsigned long long)s.full_cycles,
                   (unsigned long long)s.empty_cycles,
                   (unsigned long long)s.stall_cycles,
                   (unsigned long long)s.simultaneous_cycles);
    }
  }

 private:
  void RunCycle(const bool want[], const bool ready[]) {
    const bool rst = Resetting();
    top_->rst = rst ? 1 : 0;
    top_->d1_in_valid = want[0] ? 1 : 0;
    top_->d1_out_ready = ready[0] ? 1 : 0;
    top_->d2_in_valid = want[1] ? 1 : 0;
    top_->d2_out_ready = ready[1] ? 1 : 0;
    top_->d3_in_valid = want[2] ? 1 : 0;
    top_->d3_out_ready = ready[2] ? 1 : 0;
    top_->d8_in_valid = want[3] ? 1 : 0;
    top_->d8_out_ready = ready[3] ? 1 : 0;
    top_->sk_in_valid = want[4] ? 1 : 0;
    top_->sk_out_ready = ready[4] ? 1 : 0;
    for (Instance& inst : inst_) *inst.pins.in_payload = inst.next_payload;

    // Low phase: settle this cycle's combinational outputs.
    top_->clk = 0;
    top_->eval();

    for (uint32_t i = 0; i < kInstanceCount; ++i) Observe(i, rst);

    // Rising edge, then back to the low phase.
    top_->clk = 1;
    top_->eval();
    top_->clk = 0;
    top_->eval();

    clock_.RisingEdge();
    ++cycles_;

    if (rst) {
      ++reset_cycles_;
      // Reset is synchronous: the edge just taken discarded whatever was held.
      // Those items are the conservation term "explicitly cancelled".
      for (Instance& inst : inst_) inst.shadow.Cancel();
      if (reset_remaining_ > 0) --reset_remaining_;
    }
  }

  void Observe(uint32_t i, bool rst) {
    Instance& inst = inst_[i];
    const Pins& p = inst.pins;
    Shadow& s = inst.shadow;

    const bool in_valid = *p.in_valid != 0;
    const bool in_ready = *p.in_ready != 0;
    const uint32_t in_payload = *p.in_payload;
    const bool out_valid = *p.out_valid != 0;
    const bool out_ready = *p.out_ready != 0;
    const uint32_t out_payload = *p.out_payload;
    const uint32_t count = p.count ? static_cast<uint32_t>(*p.count) : 0;

    // The skid buffer's combinational outputs are the within-cycle answer for
    // this edge, so keep a copy for the directed checks in phase 6.
    if (s.fall_through) {
      skid_ready_ = in_ready ? 1 : 0;
      skid_valid_ = out_valid ? 1 : 0;
      skid_payload_ = out_payload;
    }

    // A FIFO presents nothing that depends on the downstream ready. That is
    // what makes its input ready safe to feed a long combinational ready path.
    // Probe it by perturbing out_ready within the settled cycle: nothing the
    // FIFO presents may move. The skid buffer deliberately does move, so it is
    // excluded.
    if (!rst && !s.fall_through) {
      const uint8_t saved = *p.out_ready;
      *p.out_ready = saved ? 0 : 1;
      top_->eval();
      const bool moved = (*p.in_ready != 0) != in_ready ||
                         (*p.out_valid != 0) != out_valid ||
                         (out_valid && *p.out_payload != out_payload) ||
                         (p.count && static_cast<uint32_t>(*p.count) != count);
      *p.out_ready = saved;
      top_->eval();
      Require(!moved, s.name,
              "outputs depend combinationally on out_ready");
    }

    // --- model prediction ---------------------------------------------------
    Expect(s.name, "in_ready", s.ExpectedReady(out_ready) ? 1 : 0,
           in_ready ? 1 : 0);
    Expect(s.name, "out_valid", s.ExpectedOutValid(in_valid) ? 1 : 0,
           out_valid ? 1 : 0);
    if (p.count) Expect(s.name, "count", s.items.size(), count);

    // The wire must show the first-in item. This is the packet sequence check,
    // and during a stall it is the "no stall changes the head item" check.
    if (!s.items.empty()) {
      Expect(s.name, "out_payload", s.items.front(), out_payload);
    } else if (s.fall_through && in_valid) {
      // Nothing held: the skid buffer must present the producer's own bus.
      Expect(s.name, "out_payload falls through", in_payload, out_payload);
    }

    // --- payload stability across a stall ----------------------------------
    if (s.stalled) {
      Require(out_valid, s.name, "out_valid dropped while stalled");
      Expect(s.name, "out_payload held during stall", s.stalled_payload,
             out_payload);
    }

    // --- account for the transfers seen on the wires -----------------------
    const bool push = in_valid && in_ready;
    const bool pop = out_valid && out_ready;
    if (push) {
      s.items.push_back(in_payload);
      s.accepted++;
      inst.next_payload++;
    }
    if (pop) {
      s.emitted++;
      if (!s.items.empty()) s.items.pop_front();
    }

    // --- conservation, every cycle -----------------------------------------
    CheckConservation(s.name, s);

    if (!rst) {
      s.stalled = out_valid && !out_ready;
      s.stalled_payload = out_payload;
      if (!s.fall_through) {
        if (!in_ready) s.full_cycles++;
        if (!out_valid) s.empty_cycles++;
      }
      if (s.stalled) s.stall_cycles++;
      if (push && pop) s.simultaneous_cycles++;
      if (s.stalled && !in_ready) s.backpressure_cycles++;
    }
  }

  void CheckConservation(const char* who, const Shadow& s) {
    const uint64_t held = s.items.size();
    const uint64_t rhs = s.emitted + held + s.cancelled;
    Expect(who, "conservation accepted == emitted + buffered + cancelled",
           s.accepted, rhs);
  }

  // Every check funnels through here so one run reports every distinct failure
  // it saw, while `Mismatch` still records the first for result.json.
  void Expect(const char* inst, const char* what, uint64_t expect,
              uint64_t actual) {
    ++checks_;
    if (expect == actual) return;
    ++failures_;
    std::ostringstream where;
    where << inst << '.' << what << " (cycle " << cycles_ << ')';
    if (failures_ == 1) {
      rep_.Mismatch(where.str(), mosaic::Hex(expect), mosaic::Hex(actual));
    } else if (failures_ <= kMaxReports) {
      std::fprintf(stderr, "CHECK FAILED: %s: expected %s, got %s\n",
                   where.str().c_str(), mosaic::Hex(expect).c_str(),
                   mosaic::Hex(actual).c_str());
    }
  }

  void Require(bool ok, const char* inst, const char* what) {
    ++checks_;
    if (ok) return;
    ++failures_;
    std::ostringstream where;
    where << inst << '.' << what << " (cycle " << cycles_ << ')';
    if (failures_ == 1) {
      rep_.Mismatch(where.str(), "holds", "violated");
    } else if (failures_ <= kMaxReports) {
      std::fprintf(stderr, "CHECK FAILED: %s: invariant violated\n",
                   where.str().c_str());
    }
  }

  mosaic::Reporter& rep_;
  Vmosaic_fifo_tb* top_;
  std::vector<Instance> inst_;
  mosaic::ClockDriver clock_;
  uint64_t cycles_ = 0;
  uint64_t failures_ = 0;
  int checks_ = 0;
  uint64_t reset_cycles_ = 0;
  int reset_remaining_ = kResetCycles;
  // Within-cycle snapshot of the skid buffer, taken before the clock edge.
  uint64_t skid_ready_ = 0;
  uint64_t skid_valid_ = 0;
  uint64_t skid_payload_ = 0;
};

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
  Vmosaic_fifo_tb* top = new Vmosaic_fifo_tb;

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

  // ---- phase 1: power-on reset --------------------------------------------
  // kResetCycles rising edges with rst high and no stimulus. On the first cycle
  // afterwards the DUT must be clean: no stale out_valid, count back to zero,
  // ready again.
  while (bench.Resetting() && !out_of_cycles()) {
    bench.Step(false, false, false, false);
  }
  if (!aborted) {
    bench.Step(false, false, false, false);
    bench.CheckPostResetClean("after reset: no stale valid, count zero, ready");
  }

  // ---- phase 2: fill to full with the output stalled ------------------------
  for (int i = 0; i < kFillCycles && !out_of_cycles(); ++i) {
    bench.Step(true, false, true, false);
  }
  if (!aborted) {
    bench.CheckAllFull("filled: in_ready low, count == DEPTH");
    for (int i = 0; i < 3 && !out_of_cycles(); ++i) {
      bench.Step(true, false, true, false);
    }
    bench.CheckAllFull("still full after three further stalled cycles");
  }

  // ---- phase 3: drain -------------------------------------------------------
  for (int i = 0; i < kDrainCycles && !out_of_cycles(); ++i) {
    bench.Step(false, true, false, true);
  }
  if (!aborted) bench.CheckAllEmpty("drained: out_valid low, count zero");

  // ---- phase 4: push and pop on the same cycle, every cycle -----------------
  // Runs through the "full while draining" corner: a FIFO that is full must
  // refuse the push for that cycle and accept it on the next, and the check
  // that enforces this is the per-cycle count/in_ready comparison.
  for (int i = 0; i < kSteadyCycles && !out_of_cycles(); ++i) {
    bench.Step(true, true, true, true);
  }

  // ---- phase 5: hard output stall with the producer still offering ----------
  // The window in which a mutated FIFO would let the head item move.
  for (int i = 0; i < kStallCycles && !out_of_cycles(); ++i) {
    bench.Step(true, false, true, false);
  }

  // ---- phase 6: drain, then the skid buffer's directed properties ------------
  for (int i = 0; i < kDrainCycles && !out_of_cycles(); ++i) {
    bench.Step(false, true, false, true);
  }

  if (!aborted) {
    // Drain the skid buffer with the producer idle, so the directed properties
    // below start from a known empty state. Offering *and* draining at the same
    // time would not empty it: a fall-through buffer never holds an item when
    // both sides are moving every cycle.
    for (int i = 0; i < 3 && !out_of_cycles(); ++i) {
      bench.Step(false, true, false, true);
    }
    bench.SkidExpect("skid starts empty", true, false, 0);

    // Offer the first item with the output held down. The skid buffer must take
    // it anyway -- that is the slack the module exists to provide.
    const uint32_t first = bench.SkidNextPayload();
    bench.Step(false, true, true, false);
    bench.SkidExpect("absorbs one item while the output is stalled", true,
                     true, first);

    // Offer a second item while the first is held. It must be refused, and the
    // output must still be the first-in item.
    bench.Step(false, true, true, false);
    bench.SkidExpect("refuses a second item while one is held", false, true,
                     first);

    // A third cycle of the same offer changes nothing: still refused, still the
    // first item.
    bench.Step(false, true, true, false);
    bench.SkidExpect("still refuses a second item", false, true, first);

    // Release the output while the producer is still offering: the held item
    // leaves and the offered item is taken in the same cycle, with no bubble.
    bench.Step(false, true, true, true);
    bench.SkidExpect("drains and refills in one cycle", true, true, first);

    // The item offered in that cycle is now the held item, in order.
    bench.Step(false, true, false, true);
    bench.SkidExpect("the next item is now held", true, true, first + 1);

    bench.Step(false, true, false, true);
    bench.SkidExpect("empty again", true, false, 0);
  }

  // ---- phase 7: randomised backpressure with resets inside a stall ----------
  // Every kFreeCyclesPerReset the output is stalled for a few cycles with items
  // still buffered, and reset is then asserted in the middle of that stall.
  // This is the card's "reset with items held must not leak them" scenario, and
  // it is the positive-control half of negative control 4.
  //
  // The sequence is written out cycle by cycle instead of as a mode machine so
  // that the post-reset assertion is taken on the first settled cycle after the
  // final reset edge, before any further stimulus can refill the buffer.
  int stall_left = 0;
  int reset_left = 0;
  int since_reset = kFreeCyclesPerReset - 100;  // exercise a reset burst early

  for (int i = 0; i < kRandomCycles && !out_of_cycles(); ++i) {
    if (reset_left > 0) {
      bench.Step(false, false, false, false);  // rst asserted, no stimulus
      if (--reset_left == 0) {
        // First settled cycle after the last reset edge: nothing survived it.
        bench.Step(false, false, false, false);
        bench.CheckPostResetClean("no stale valid or payload escapes reset");
        stall_left = 0;
      }
      continue;
    }
    if (stall_left > 0) {
      // Keep offering while the output is held down, so the reset lands on a
      // stalled FIFO that still holds items.
      bench.Step(true, false, true, false);
      if (--stall_left == 0) {
        reset_left = kResetBurst;
        bench.PulseReset(kResetBurst);
      }
      continue;
    }
    if (++since_reset >= kFreeCyclesPerReset) {
      since_reset = 0;
      stall_left = kStallBeforeReset;
      bench.Step(true, false, true, false);
      continue;
    }
    bench.StepRandomised();
  }

  // ---- phase 8: final drain and conservation --------------------------------
  if (!aborted) {
    for (int i = 0; i < 8 * kDrainCycles && !out_of_cycles(); ++i) {
      bench.Step(false, true, false, true);
    }
    bench.CheckAllEmpty("final drain leaves every instance empty");
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
                       std::to_string(bench.reset_cycles()) + " reset cycles";
  return rep.Finish("PASS", detail);
}