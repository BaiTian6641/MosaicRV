// ============================================================================
// tb_muldiv.cpp -- CASE=muldiv.kill_and_edges, work package I-012.
//
// The DUT is never its own oracle. Two independent references are used, one for
// each half of the contract:
//
//   * the ARITHMETIC comes from the host's own signed and unsigned 128-bit
//     integers (`RefCalc` below). The RTL multiplies with a shift-add loop and
//     divides with a restoring loop; the reference does neither. If both were
//     written as shift-add, agreeing would only mean the two loops share a
//     misreading of the ISA.
//   * the TIMING AND CONTROL come from a small state machine (`ShadowMulDiv`)
//     written from the contract documented at the top of
//     rtl/core/mosaic_muldiv.sv: one operation in flight, ready only in idle,
//     result held until taken, flush cancels everything and is ready next
//     cycle. It shares no code with the RTL and holds no Verilator types.
//
// Every cycle is compared, so a control defect is caught on the cycle it
// happens rather than by a later consequence. The standing invariants are
// re-checked every cycle as well:
//
//   * `req_ready_o` is high only when the unit is idle and no flush is asserted;
//   * `res_valid_o` is high only after an operation actually completed;
//   * the returned identity always equals the accepted request's identity;
//   * accepted == completed + cancelled + in-flight, with in-flight = o_busy;
//   * `o_killed_res_ctr` never exceeds `o_cancelled_ctr`, and it counts only
//     the results that had already been computed when the flush arrived;
//   * no result ever appears after a flush except the next operation's own.
//
// Phases, each of which can fail on its own:
//
//   1. reset-state      the documented cold state.
//   2. directed-edges   all 8 operations x {64-bit, W} over a curated vector
//                       set: 0, +1, -1, MAX, MIN, div-by-zero, MIN/-1 signed
//                       overflow, mulhsu sign mixes, and W vectors with garbage
//                       in the upper 32 bits that must be ignored.
//   3. cancel-sweep     cancel at EVERY iteration position, for a 64-bit and a
//                       W operation, using o_iter/o_busy to find each position
//                       rather than guessing the latency; after each cancel a
//                       new operation with a different identity must complete
//                       correctly, and the cancelled identity must never appear.
//   4. backpressure     hold res_ready_i low for many cycles while reloading the
//                       request inputs, require the identical value and identity
//                       to stay offered, then accept it and prove a following
//                       operation still completes (no lost operation, no leak).
//   5. reset-cancel     an operation in flight when rst_i arrives: the unit must
//                       come back idle and ready with its counters cleared.
//   6. random           a seeded soak: random operations, random flush timing,
//                       random back-pressure, shadow-compared every cycle.
//
// Failure counting is deliberate: per-cycle comparisons record a failure and
// carry on rather than aborting, so a mutant's failure count is a delta against
// the shipping build rather than a single line. A phase that cannot make
// progress (the DUT never offers a result) aborts with a named reason instead of
// spinning.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_muldiv_tb.h"

namespace {

// Thrown for a structural failure: a phase that cannot continue, as opposed to a
// check that merely failed.
struct Abort {
  std::string what;
};

[[noreturn]] void Stop(const std::string& where, const std::string& detail) {
  throw Abort{where + ": " + detail};
}

std::string Bool(bool value) { return value ? "1" : "0"; }
std::string Dec(uint64_t value) { return std::to_string(value); }
std::string HexU(uint64_t value) { return mosaic::Hex(value); }

// ------------------------------------------------------------- op encodings
// Mirrors of mosaic_pkg::md_op_e. The numbering is part of the contract between
// the decoder and this unit, so the directed phase exercises every value by
// number: a renumbering of the package would fail here rather than silently
// agree.
enum : uint8_t {
  MD_MUL = 0, MD_MULH = 1, MD_MULHSU = 2, MD_MULHU = 3,
  MD_DIV = 4, MD_DIVU = 5, MD_REM = 6, MD_REMU = 7
};

const char* OpName(uint8_t op) {
  switch (op) {
    case MD_MUL: return "mul";
    case MD_MULH: return "mulh";
    case MD_MULHSU: return "mulhsu";
    case MD_MULHU: return "mulhu";
    case MD_DIV: return "div";
    case MD_DIVU: return "divu";
    case MD_REM: return "rem";
    default: return "remu";
  }
}

// ------------------------------------------- independent arithmetic reference
// Written from the RISC-V unprivileged ISA, M extension:
//
//   * a W form is the operation at 32-bit width, operands from the low 32 bits,
//     result sign-extended to 64;
//   * a signed high multiply is the high half of the signed product; mulhsu
//     mixes one signed and one unsigned operand;
//   * division by zero answers an all-ones quotient and a remainder equal to the
//     dividend;
//   * the signed overflow MIN / -1 answers MIN with remainder 0, which the host
//     arithmetic reproduces for free at the width the operation works at.
//
// The host's `__int128` arithmetic is the oracle; the RTL's loops are the thing
// under test.
uint64_t RefCalc(uint8_t op, bool w, uint64_t a, uint64_t b) {
  const unsigned width = w ? 32u : 64u;
  const uint64_t wmask = w ? 0xFFFFFFFFull : ~0ull;
  const uint64_t au = a & wmask;
  const uint64_t bu = b & wmask;
  const int64_t as = w ? static_cast<int64_t>(static_cast<int32_t>(au))
                       : static_cast<int64_t>(au);
  const int64_t bs = w ? static_cast<int64_t>(static_cast<int32_t>(bu))
                       : static_cast<int64_t>(bu);
  // A W answer is a 32-bit value in the 64-bit lane: sign-extend bit 31.
  auto finish = [w, wmask](uint64_t value) -> uint64_t {
    value &= wmask;
    if (!w) return value;
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(
        static_cast<uint32_t>(value))));
  };
  auto high = [width](const __int128 product) -> uint64_t {
    return static_cast<uint64_t>(static_cast<__uint128_t>(product) >> width);
  };
  switch (op) {
    case MD_MUL:
      return finish(static_cast<uint64_t>(
          static_cast<__uint128_t>(au) * static_cast<__uint128_t>(bu)));
    case MD_MULHU:
      return finish(static_cast<uint64_t>(
          (static_cast<__uint128_t>(au) * static_cast<__uint128_t>(bu)) >>
          width));
    case MD_MULH:
      return finish(high(static_cast<__int128>(as) * static_cast<__int128>(bs)));
    case MD_MULHSU:
      return finish(high(static_cast<__int128>(as) *
                         static_cast<__int128>(static_cast<__uint128_t>(bu))));
    case MD_DIV:
    case MD_REM: {
      uint64_t quotient = wmask;
      uint64_t remainder = au;
      if (bu != 0) {
        const __int128 q = static_cast<__int128>(as) / static_cast<__int128>(bs);
        const __int128 r = static_cast<__int128>(as) % static_cast<__int128>(bs);
        quotient = static_cast<uint64_t>(static_cast<__uint128_t>(q)) & wmask;
        remainder = static_cast<uint64_t>(static_cast<__uint128_t>(r)) & wmask;
      }
      return finish(op == MD_DIV ? quotient : remainder);
    }
    default: {   // MD_DIVU, MD_REMU
      const uint64_t quotient = (bu == 0) ? wmask : (au / bu);
      const uint64_t remainder = (bu == 0) ? au : (au % bu);
      return finish(op == MD_DIVU ? quotient : remainder);
    }
  }
}

// ------------------------------------------------------------------ identity
struct Ident {
  uint64_t rob_index = 0;
  uint64_t rob_gen = 0;
  uint64_t uop_index = 0;

  bool operator==(const Ident& other) const {
    return rob_index == other.rob_index && rob_gen == other.rob_gen &&
           uop_index == other.uop_index;
  }
  bool operator!=(const Ident& other) const { return !(*this == other); }
  std::string str() const {
    return "(rob=" + Dec(rob_index) + ", gen=" + Dec(rob_gen) +
           ", uop=" + Dec(uop_index) + ")";
  }
};

// ------------------------------------------------------------------ stimulus
struct Stim {
  bool req_valid = false;
  uint8_t op = MD_MUL;
  bool w = false;
  uint64_t a = 0;
  uint64_t b = 0;
  Ident id{};
  bool flush = false;
  bool res_ready = false;

  std::string str() const {
    return "[valid=" + Bool(req_valid) + " op=" + OpName(op) + (w ? "w" : "") +
           " a=" + HexU(a) + " b=" + HexU(b) + " id" + id.str() +
           " flush=" + Bool(flush) + " ready=" + Bool(res_ready) + "]";
  }
};

// ---------------------------------------------------------------- observation
struct Obs {
  // pre-edge: what the DUT presented in the cycle under test
  bool req_ready = false;
  bool res_valid = false;
  uint64_t res_data = 0;
  Ident res_id{};
  bool busy_pre = false;
  uint8_t iter_pre = 0;
  // post-edge: what is true now
  bool busy = false;
  uint8_t iter = 0;
  uint32_t accepted = 0;
  uint32_t completed = 0;
  uint32_t cancelled = 0;
  uint32_t killed = 0;
};

// --------------------------------------------------------------------- shadow
// The documented contract as a state machine. It never looks at the DUT.
class ShadowMulDiv {
 public:
  enum class St { Idle, Run, Done };

  void Reset() {
    st_ = St::Idle;
    iter_ = 0;
    width_ = 0;
    have_result_ = false;
    result_ = 0;
    id_ = Ident{};
    accepted_ = 0;
    completed_ = 0;
    cancelled_ = 0;
    killed_ = 0;
  }

  void SetMasks(uint64_t rob, uint64_t gen, uint64_t uop) {
    rob_mask_ = rob;
    gen_mask_ = gen;
    uop_mask_ = uop;
  }

  // The identity as the DUT will latch it: only the bits its ports carry.
  Ident Mask(const Ident& id) const {
    return Ident{id.rob_index & rob_mask_, id.rob_gen & gen_mask_,
                 id.uop_index & uop_mask_};
  }

  // Pre-edge expectations, from the state before this cycle's edge.
  bool ReadyExpect(bool flush) const { return st_ == St::Idle && !flush; }
  bool ValidExpect(bool flush) const { return st_ == St::Done && !flush; }
  bool BusyExpect() const { return st_ != St::Idle; }
  uint8_t IterExpect() const { return static_cast<uint8_t>(iter_); }
  uint64_t ResultData() const { return result_; }
  const Ident& ResultId() const { return id_; }

  uint32_t accepted() const { return accepted_; }
  uint32_t completed() const { return completed_; }
  uint32_t cancelled() const { return cancelled_; }
  uint32_t killed() const { return killed_; }
  bool idle() const { return st_ == St::Idle; }

  void Advance(const Stim& s) {
    const bool ready = (st_ == St::Idle) && !s.flush;
    const bool req_fire = s.req_valid && ready;
    const bool valid = (st_ == St::Done) && !s.flush;
    const bool res_fire = valid && s.res_ready;

    if (req_fire) ++accepted_;
    if (res_fire) ++completed_;
    if (s.flush && st_ == St::Run) ++cancelled_;
    if (s.flush && st_ == St::Done) {
      ++cancelled_;
      ++killed_;
    }

    if (s.flush) {
      st_ = St::Idle;
      iter_ = 0;
      have_result_ = false;
      return;
    }

    switch (st_) {
      case St::Idle:
        if (s.req_valid) {
          op_ = s.op;
          w_ = s.w;
          a_ = s.a;
          b_ = s.b;
          id_ = Mask(s.id);
          width_ = s.w ? 32u : 64u;
          iter_ = 0;
          have_result_ = false;
          st_ = St::Run;
        }
        break;
      case St::Run:
        ++iter_;
        if (iter_ == width_) {
          result_ = RefCalc(op_, w_, a_, b_);
          have_result_ = true;
          st_ = St::Done;
        }
        break;
      case St::Done:
        if (s.res_ready) {
          st_ = St::Idle;
          iter_ = 0;
          have_result_ = false;
        }
        break;
    }
  }

 private:
  St st_ = St::Idle;
  uint8_t op_ = 0;
  bool w_ = false;
  uint64_t a_ = 0, b_ = 0;
  Ident id_{};
  unsigned width_ = 0;
  uint64_t iter_ = 0;
  bool have_result_ = false;
  uint64_t result_ = 0;

  uint32_t accepted_ = 0;
  uint32_t completed_ = 0;
  uint32_t cancelled_ = 0;
  uint32_t killed_ = 0;

  uint64_t rob_mask_ = 0, gen_mask_ = 0, uop_mask_ = 0;
};

// -------------------------------------------------------------------- harness
class Harness {
 public:
  Harness(Vmosaic_muldiv_tb* dut, mosaic::ClockDriver* clk,
          mosaic::Reporter* reporter, uint64_t max_cycles)
      : dut_(dut), clk_(clk), reporter_(reporter), max_cycles_(max_cycles) {}

  void Phase(const std::string& name) { phase_ = name; }
  void BindShadow(ShadowMulDiv* shadow) { shadow_ = shadow; }
  void SetContext(const std::string& text) { context_ = text; }

  void SetMasks(uint64_t rob, uint64_t gen, uint64_t uop) {
    rob_mask_ = rob;
    gen_mask_ = gen;
    uop_mask_ = uop;
  }

  Ident MaskId(const Ident& id) const {
    return Ident{id.rob_index & rob_mask_, id.rob_gen & gen_mask_,
                 id.uop_index & uop_mask_};
  }

  // Assert reset for `cycles` rising edges with no stimulus, and reset the
  // shadow with it: a reset that cleared only the DUT would leave the two
  // models describing different machines.
  void Reset(int cycles) {
    for (int i = 0; i < cycles; ++i) {
      Cycle(Stim{}, /*rst=*/true);
    }
    if (shadow_ != nullptr) shadow_->Reset();
  }

  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }

  // One clock period, in the order sim/common/sim_common.h documents:
  //   1. drive with the clock low and compare the *combinational* outputs
  //      against the shadow computed from the same pre-edge state;
  //   2. advance the shadow over the edge;
  //   3. apply the edge, then compare the *registered* state and the standing
  //      invariants.
  // Values read before the edge are what the DUT presented in the cycle under
  // test; values read after it are what is true now. Mixing them compares
  // different cycles.
  Obs Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Stop(phase_, "max-cycles (" + Dec(max_cycles_) +
                       ") exhausted before the campaign finished");
    }

    dut_->clk = 0;
    dut_->rst = rst ? 1 : 0;
    dut_->req_valid = s.req_valid ? 1 : 0;
    dut_->req_op = s.op & 0x7;
    dut_->req_w = s.w ? 1 : 0;
    dut_->req_a = s.a;
    dut_->req_b = s.b;
    dut_->req_rob_index = s.id.rob_index & rob_mask_;
    dut_->req_rob_gen = s.id.rob_gen & gen_mask_;
    dut_->req_uop_index = s.id.uop_index & uop_mask_;
    dut_->flush = s.flush ? 1 : 0;
    dut_->res_ready = s.res_ready ? 1 : 0;
    dut_->eval();

    Obs o;
    o.req_ready = dut_->req_ready != 0;
    o.res_valid = dut_->res_valid != 0;
    o.res_data = dut_->res_data;
    o.res_id = Ident{dut_->res_rob_index, dut_->res_rob_gen, dut_->res_uop_index};
    o.busy_pre = dut_->o_busy != 0;
    o.iter_pre = static_cast<uint8_t>(dut_->o_iter);

    const std::string where = Where(s);

    if (!rst) {
      CompareCombinational(s, o, where);
      shadow_->Advance(s);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    o.busy = dut_->o_busy != 0;
    o.iter = static_cast<uint8_t>(dut_->o_iter);
    o.accepted = dut_->o_accepted_ctr;
    o.completed = dut_->o_completed_ctr;
    o.cancelled = dut_->o_cancelled_ctr;
    o.killed = dut_->o_killed_res_ctr;

    if (!rst) {
      CompareState(o, where);
      CheckInvariants(s, o, where);
      ++comparisons_;
    }
    return o;
  }

  // Run cycles with a fixed stimulus until the DUT offers a result, then return
  // that observation. Bounded, so a DUT that never completes stops the phase
  // with a reason rather than spinning.
  Obs RunToResult(const Stim& base, int bound, const std::string& what) {
    for (int i = 0; i < bound; ++i) {
      Obs o = Cycle(base);
      if (o.res_valid) return o;
    }
    Stop(phase_, what + ": no result within " +
                     Dec(static_cast<uint64_t>(bound)) + " cycles");
  }

 private:
  std::string Where(const Stim& s) const {
    std::string text = phase_ + ": cycle " + Dec(clk_->cycle());
    if (!context_.empty()) text += " [" + context_ + "]";
    return text + " " + s.str();
  }

  void CompareCombinational(const Stim& s, const Obs& o,
                            const std::string& where) {
    reporter_->Check(o.req_ready == shadow_->ReadyExpect(s.flush),
                     where + " req_ready_o=" + Bool(o.req_ready) + " expected " +
                         Bool(shadow_->ReadyExpect(s.flush)));
    reporter_->Check(o.res_valid == shadow_->ValidExpect(s.flush),
                     where + " res_valid_o=" + Bool(o.res_valid) + " expected " +
                         Bool(shadow_->ValidExpect(s.flush)));
    if (o.res_valid && shadow_->ValidExpect(s.flush)) {
      reporter_->Check(o.res_data == shadow_->ResultData(),
                       where + " result " + HexU(o.res_data) + " expected " +
                           HexU(shadow_->ResultData()));
      reporter_->Check(o.res_id == shadow_->ResultId(),
                       where + " identity " + o.res_id.str() + " expected " +
                           shadow_->ResultId().str());
    }
  }

  void CompareState(const Obs& o, const std::string& where) {
    reporter_->Check(o.busy == shadow_->BusyExpect(),
                     where + " o_busy=" + Bool(o.busy) + " expected " +
                         Bool(shadow_->BusyExpect()));
    reporter_->Check(o.iter == shadow_->IterExpect(), where + " o_iter=" +
                                                         Dec(o.iter) +
                                                         " expected " +
                                                         Dec(shadow_->IterExpect()));
    reporter_->Check(o.accepted == shadow_->accepted(),
                     where + " accepted_ctr=" + Dec(o.accepted) + " expected " +
                         Dec(shadow_->accepted()));
    reporter_->Check(o.completed == shadow_->completed(),
                     where + " completed_ctr=" + Dec(o.completed) + " expected " +
                         Dec(shadow_->completed()));
    reporter_->Check(o.cancelled == shadow_->cancelled(),
                     where + " cancelled_ctr=" + Dec(o.cancelled) + " expected " +
                         Dec(shadow_->cancelled()));
    reporter_->Check(o.killed == shadow_->killed(),
                     where + " killed_res_ctr=" + Dec(o.killed) + " expected " +
                         Dec(shadow_->killed()));
  }

  // The standing invariants. These are properties of the *contract*, not a
  // restatement of the shadow, so they are checked by name even where the
  // shadow would also catch the same defect.
  void CheckInvariants(const Stim& s, const Obs& o, const std::string& where) {
    // req_ready_o may only be high when the unit can take the request that
    // cycle: that means idle (no operation in flight, iteration counter at 0)
    // and no flush.
    reporter_->Check(!(o.req_ready && o.busy_pre),
                     where + " req_ready_o high while the unit was busy");
    reporter_->Check(!(o.req_ready && s.flush),
                     where + " req_ready_o high in a flush cycle");
    reporter_->Check(!(o.req_ready && o.iter_pre != 0),
                     where + " req_ready_o high with a non-zero iteration counter");

    // res_valid_o may only be high when an operation actually completed.
    reporter_->Check(!(o.res_valid && !o.busy_pre),
                     where + " res_valid_o high with no operation in flight");
    reporter_->Check(!(o.res_valid && s.flush),
                     where + " res_valid_o high in a flush cycle");

    // Counter conservation: accepted == completed + cancelled + in-flight.
    const uint32_t in_flight = o.busy ? 1u : 0u;
    const uint32_t expect = o.completed + o.cancelled + in_flight;
    reporter_->Check(o.accepted == expect,
                     where + " counter conservation: accepted=" +
                         Dec(o.accepted) + " completed=" + Dec(o.completed) +
                         " cancelled=" + Dec(o.cancelled) + " in-flight=" +
                         Dec(in_flight) + " (expected accepted=" + Dec(expect) +
                         ")");

    // A killed result is a cancellation, never its own separate operation.
    reporter_->Check(o.killed <= o.cancelled,
                     where + " killed_res_ctr exceeds cancelled_ctr");
  }

  Vmosaic_muldiv_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* reporter_;
  ShadowMulDiv* shadow_ = nullptr;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::string phase_ = "unset";
  std::string context_;

  uint64_t rob_mask_ = ~0ull;
  uint64_t gen_mask_ = ~0ull;
  uint64_t uop_mask_ = ~0ull;
};

// ------------------------------------------------------- directed vector table
// The values every operation is exercised with. Chosen so that the cases named
// in the card all appear: 0, +1, -1, MAX, MIN, the signed overflow MIN/-1, the
// divide-by-zero divisor, operands whose sign bits disagree (mulhsu), and word
// patterns whose upper 32 bits a W form must ignore.
std::vector<uint64_t> SpecialValues(bool w) {
  if (w) {
    return {0ull, 1ull, 2ull, 0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFull,
            0xFFFFFFFEull, 0x0000000100000000ull};
  }
  return {0ull, 1ull, 2ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
          0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull,
          0x0000000100000000ull};
}

std::vector<uint64_t> WordJunkValues() {
  // W forms: the low 32 bits are the operand and bits 63:32 are not; these
  // patterns disagree between the halves, so a unit that read the wrong half
  // (or failed to mask) would answer differently.
  return {0xDEADBEEF00000001ull, 0xFFFFFFFF00000002ull, 0x1234567800000000ull,
          0xCAFEBABE7FFFFFFFull, 0x0000000180000000ull};
}

// ------------------------------------------------------------------- phases
void PhaseResetState(Harness* h, mosaic::Reporter* reporter) {
  h->SetContext("cold state after reset");
  const Obs o = h->Cycle(Stim{});
  reporter->Check(o.req_ready, "reset-state: req_ready_o is high when idle");
  reporter->Check(!o.res_valid, "reset-state: res_valid_o is low");
  reporter->Check(!o.busy, "reset-state: o_busy is low");
  reporter->Check(o.iter == 0, "reset-state: o_iter is 0");
  reporter->Check(o.accepted == 0 && o.completed == 0 && o.cancelled == 0 &&
                      o.killed == 0,
                  "reset-state: all four counters are 0");
}

// One operation, request to result, with the result and identity checked, and
// the result taken immediately.
void RunOne(Harness* h, mosaic::Reporter* reporter, const std::string& what,
            uint8_t op, bool w, uint64_t a, uint64_t b, const Ident& id) {
  h->SetContext(what);
  const Ident want = h->MaskId(id);
  Stim s;
  s.req_valid = true;
  s.op = op;
  s.w = w;
  s.a = a;
  s.b = b;
  s.id = id;
  s.res_ready = true;
  const Obs acc = h->Cycle(s);
  reporter->Check(acc.req_ready, what + ": the request was not accepted");
  if (!acc.req_ready) Stop("directed", what + ": request refused while idle");
  reporter->Check(acc.busy, what + ": the accepted operation did not make the "
                                    "unit busy");
  const uint32_t completed_before = acc.completed;

  Stim take;
  take.res_ready = true;
  const Obs res = h->RunToResult(take, 200, what);
  const uint64_t expect = RefCalc(op, w, a, b);
  reporter->Check(res.res_data == expect, what + ": result " + HexU(res.res_data) +
                                              " expected " + HexU(expect));
  reporter->Check(res.res_id == want, what + ": identity " + res.res_id.str() +
                                          " expected " + want.str());
  reporter->Check(res.completed == completed_before + 1,
                  what + ": completed_ctr did not advance by exactly one");
  reporter->Check(!res.busy, what + ": the unit stayed busy after delivery");
}

void PhaseDirected(Harness* h, mosaic::Reporter* reporter, uint64_t* ops_done) {
  uint64_t ident_counter = 0;
  auto next_id = [&ident_counter](uint64_t salt) {
    // A different identity for every request, so a late result can never be
    // matched to the wrong request by accident.
    const uint64_t n = ident_counter++;
    return Ident{(n * 7 + salt) & 0x3F, (n * 11 + salt * 3) & 0x7F,
                 (n + salt) & 0x7};
  };

  for (uint8_t op = 0; op < 8; ++op) {
    for (int wi = 0; wi <= 1; ++wi) {
      const bool w = wi != 0;
      const std::vector<uint64_t> values = SpecialValues(w);
      for (uint64_t a : values) {
        for (uint64_t b : values) {
          RunOne(h, reporter,
                 std::string("directed ") + OpName(op) + (w ? "w" : "") + " " +
                     HexU(a) + " " + HexU(b),
                 op, w, a, b, next_id(op));
          ++*ops_done;
        }
      }
      if (w) {
        const std::vector<uint64_t> junk = WordJunkValues();
        for (uint64_t a : junk) {
          for (uint64_t b : junk) {
            RunOne(h, reporter,
                   std::string("directed ") + OpName(op) +
                       "w upper-half junk " + HexU(a) + " " + HexU(b),
                   op, w, a, b, next_id(op));
            ++*ops_done;
          }
        }
      }
    }
  }

  // The named corner cases, called out separately so the report can name them.
  struct Edge {
    const char* name;
    uint8_t op;
    bool w;
    uint64_t a;
    uint64_t b;
  };
  const Edge kEdges[] = {
      {"div by zero", MD_DIV, false, 0x123456789ABCDEF0ull, 0ull},
      {"divu by zero", MD_DIVU, false, 0x123456789ABCDEF0ull, 0ull},
      {"rem by zero", MD_REM, false, 0x8000000000000000ull, 0ull},
      {"remu by zero (0/0)", MD_REMU, false, 0ull, 0ull},
      {"signed overflow MIN/-1", MD_DIV, false, 0x8000000000000000ull,
       0xFFFFFFFFFFFFFFFFull},
      {"signed overflow MIN/-1 (rem)", MD_REM, false, 0x8000000000000000ull,
       0xFFFFFFFFFFFFFFFFull},
      {"W signed overflow MIN32/-1", MD_DIV, true, 0x80000000ull,
       0xFFFFFFFFull},
      {"W div by zero", MD_DIV, true, 0x80000000ull, 0ull},
      {"mulhsu negative x positive", MD_MULHSU, false, 0xFFFFFFFFFFFFFFFFull,
       0x0000000000000002ull},
      {"mulhsu positive x negative", MD_MULHSU, false, 0x0000000000000002ull,
       0xFFFFFFFFFFFFFFFFull},
      {"mulhu all ones", MD_MULHU, false, 0xFFFFFFFFFFFFFFFFull,
       0xFFFFFFFFFFFFFFFFull},
      {"mulw max x max", MD_MUL, true, 0x7FFFFFFFull, 0x7FFFFFFFull},
      {"divuw unsigned radix", MD_DIVU, true, 0xFFFFFFFFull, 2ull},
      {"remuw unsigned radix", MD_REMU, true, 0xFFFFFFFFull, 2ull},
  };
  for (const Edge& e : kEdges) {
    RunOne(h, reporter,
           std::string("edge: ") + e.name + " (" + OpName(e.op) +
               (e.w ? "w" : "") + ")",
           e.op, e.w, e.a, e.b, next_id(e.op));
    ++*ops_done;
  }

  // Nothing was lost and nothing was cancelled: every request in this phase was
  // accepted, computed and delivered.
  h->SetContext("directed phase totals");
  const Obs o = h->Cycle(Stim{});
  reporter->Check(o.accepted == *ops_done,
                  "directed: accepted_ctr=" + Dec(o.accepted) + ", expected " +
                      Dec(*ops_done));
  reporter->Check(o.completed == *ops_done,
                  "directed: completed_ctr=" + Dec(o.completed) + ", expected " +
                      Dec(*ops_done));
  reporter->Check(o.cancelled == 0 && o.killed == 0,
                  "directed: no operation was cancelled");
  reporter->Check(!o.busy, "directed: the unit is idle at the end of the phase");
}

void PhaseCancelSweep(Harness* h, mosaic::Reporter* reporter, uint64_t* cancels,
                      uint64_t* kills) {
  uint64_t ident_counter = 0x400;
  auto next_id = [&ident_counter]() {
    const uint64_t n = ident_counter++;
    return Ident{n & 0x3F, (n * 13) & 0x7F, (n * 5) & 0x7};
  };

  for (int wi = 0; wi <= 1; ++wi) {
    const bool w = wi != 0;
    const int max_iter = w ? 32 : 64;
    for (int k = 0; k <= max_iter; ++k) {
      const uint8_t op = static_cast<uint8_t>((k + wi) % 8);
      const Ident id_a = next_id();
      const uint64_t a = 0x9E3779B97F4A7C15ull;
      const uint64_t b = 0xBF58476D1CE4E5B9ull;
      h->SetContext("cancel " + std::string(OpName(op)) + (w ? "w" : "") +
                    " at iteration " + Dec(static_cast<uint64_t>(k)));

      Stim req;
      req.req_valid = true;
      req.op = op;
      req.w = w;
      req.a = a;
      req.b = b;
      req.id = id_a;
      req.res_ready = false;
      Obs cur = h->Cycle(req);
      reporter->Check(cur.req_ready, "cancel: the unit refused a request in idle");
      if (!cur.req_ready) Stop("cancel", "request refused in idle");
      reporter->Check(cur.busy,
                      "cancel: the accepted operation did not make the unit busy");

      int guard = 0;
      while (static_cast<int>(cur.iter) < k) {
        if (!cur.busy) {
          Stop("cancel", "the unit went idle before iteration " +
                             Dec(static_cast<uint64_t>(k)) + " (stopped at " +
                             Dec(cur.iter) + ")");
        }
        cur = h->Cycle(Stim{});
        if (++guard > 300) {
          Stop("cancel", "o_iter never reached " + Dec(static_cast<uint64_t>(k)));
        }
      }
      // At iteration k. For k == max_iter the operation has just completed, so
      // the result must be offered; below that no result exists yet.
      if (k == max_iter) {
        cur = h->Cycle(Stim{});
        reporter->Check(cur.res_valid,
                        "cancel: at iteration " + Dec(static_cast<uint64_t>(k)) +
                            " the result was not offered");
      } else {
        reporter->Check(!cur.res_valid,
                        "cancel: a result appeared at iteration " +
                            Dec(static_cast<uint64_t>(k)));
      }

      const uint32_t cancelled_before = cur.cancelled;
      const uint32_t killed_before = cur.killed;
      Stim flush;
      flush.flush = true;
      const Obs fl = h->Cycle(flush);
      reporter->Check(!fl.busy, "cancel: the unit stayed busy through a flush");
      reporter->Check(!fl.res_valid,
                      "cancel: a result was offered in a flush cycle");
      reporter->Check(fl.cancelled == cancelled_before + 1,
                      "cancel: the flush did not count exactly one cancellation");
      if (k == max_iter) {
        reporter->Check(fl.killed == killed_before + 1,
                        "cancel: a computed result destroyed by flush was not "
                        "counted in o_killed_res_ctr");
        ++*kills;
      } else {
        reporter->Check(fl.killed == killed_before,
                        "cancel: a partially computed operation was counted as a "
                        "killed result");
      }
      ++*cancels;

      // A new operation with a different identity must be accepted on the very
      // next cycle, must complete correctly, and must be the *only* result that
      // appears: the cancelled identity must never come back.
      const Ident id_b = next_id();
      const Ident want_b = h->MaskId(id_b);
      Stim next;
      next.req_valid = true;
      next.op = static_cast<uint8_t>((op + 3) % 8);
      next.w = w;
      next.a = 0x0123456789ABCDEFull;
      next.b = 0xFEDCBA9876543210ull;
      next.id = id_b;
      next.res_ready = true;
      const Obs nx = h->Cycle(next);
      reporter->Check(nx.req_ready,
                      "cancel: the unit did not accept a request on the cycle "
                      "after a flush");
      if (!nx.req_ready) Stop("cancel", "request refused after flush");
      const uint32_t completed_before = nx.completed;

      Stim take;
      take.res_ready = true;
      const Obs res = h->RunToResult(take, 200, "cancel follow-up");
      const uint64_t expect = RefCalc(next.op, w, next.a, next.b);
      reporter->Check(res.res_data == expect,
                      "cancel: the follow-up result was " + HexU(res.res_data) +
                          ", expected " + HexU(expect));
      reporter->Check(res.res_id == want_b,
                      "cancel: the follow-up result carried identity " +
                          res.res_id.str() + ", expected " + want_b.str());
      reporter->Check(res.res_id != h->MaskId(id_a),
                      "cancel: the cancelled operation's identity reappeared");
      reporter->Check(res.completed == completed_before + 1,
                      "cancel: the follow-up result was not delivered exactly "
                      "once");
      reporter->Check(!res.busy, "cancel: the unit stayed busy after the "
                                 "follow-up result");
    }
  }
}

void PhaseBackpressure(Harness* h, mosaic::Reporter* reporter,
                       uint64_t* held_cycles) {
  // 1. Hold the result for many cycles while the request inputs are reloaded
  //    with different values: the offered result and identity must not move.
  h->SetContext("back-pressure hold");
  const Ident id = Ident{17, 23, 5};
  Stim req;
  req.req_valid = true;
  req.op = MD_REMU;
  req.w = false;
  req.a = 0x123456789ABCDEF0ull;
  req.b = 0x000000000000000Full;
  req.id = id;
  req.res_ready = false;
  Obs cur = h->Cycle(req);
  reporter->Check(cur.req_ready, "backpressure: request not accepted");
  if (!cur.req_ready) Stop("backpressure", "request refused in idle");

  while (!cur.res_valid) {
    cur = h->Cycle(Stim{});
  }
  const uint64_t held_data = cur.res_data;
  const Ident held_id = cur.res_id;
  const uint64_t expect = RefCalc(MD_REMU, false, req.a, req.b);
  reporter->Check(held_data == expect, "backpressure: first offered value is " +
                                           HexU(held_data) + ", expected " +
                                           HexU(expect));
  reporter->Check(held_id == h->MaskId(id),
                  "backpressure: first offered identity is " + held_id.str());

  for (int i = 0; i < 100; ++i) {
    // Reload every request input differently on every stalled cycle. A unit
    // that lets an input reach the offered result, or that drops it, shows up
    // here rather than much later.
    Stim wiggle;
    wiggle.req_valid = (i % 3) == 0;
    wiggle.op = static_cast<uint8_t>(i % 8);
    wiggle.w = (i % 2) == 0;
    wiggle.a = 0xF0F0F0F0F0F0F0F0ull ^ static_cast<uint64_t>(i);
    wiggle.b = 0x0F0F0F0F0F0F0F0Full + static_cast<uint64_t>(i);
    wiggle.id = Ident{static_cast<uint64_t>(i), static_cast<uint64_t>(i * 3), 1};
    wiggle.res_ready = false;
    const Obs o = h->Cycle(wiggle);
    reporter->Check(o.res_valid,
                    "backpressure: the held result stopped being offered");
    reporter->Check(o.res_data == held_data,
                    "backpressure: the offered value changed while waiting (" +
                        HexU(o.res_data) + " vs " + HexU(held_data) + ")");
    reporter->Check(o.res_id == held_id,
                    "backpressure: the offered identity changed while waiting");
    ++*held_cycles;
  }

  // 2. Accept it, and require the counters and the following operation to be
  //    exactly right: no lost operation, no credit leak.
  Stim take;
  take.res_ready = true;
  const Obs took = h->Cycle(take);
  reporter->Check(took.completed == 1,
                  "backpressure: completing the held result did not advance "
                  "completed_ctr exactly once");
  reporter->Check(!took.busy,
                  "backpressure: the unit stayed busy after the held result was "
                  "accepted");

  h->SetContext("back-pressure follow-up");
  const Ident id2 = Ident{31, 63, 7};
  Stim after;
  after.req_valid = true;
  after.op = MD_MUL;
  after.w = false;
  after.a = 0x00000000000000FFull;
  after.b = 0x0000000000000100ull;
  after.id = id2;
  after.res_ready = true;
  const Obs acc = h->Cycle(after);
  reporter->Check(acc.req_ready,
                  "backpressure: the unit refused a request after the held "
                  "result was accepted");
  if (!acc.req_ready) Stop("backpressure", "request refused after delivery");
  const Obs res = h->RunToResult(after, 200, "back-pressure follow-up");
  reporter->Check(res.res_data == RefCalc(MD_MUL, false, after.a, after.b),
                  "backpressure: the follow-up operation's result was wrong");
  reporter->Check(res.res_id == h->MaskId(id2),
                  "backpressure: the follow-up result carried the wrong identity");
  reporter->Check(res.completed == 2,
                  "backpressure: the follow-up operation was not completed");

  // 3. The same hold, but killed by a flush instead of taken: the value must
  //    not be delivered and the kill must be visible as its own event.
  h->SetContext("back-pressure kill");
  Stim req2;
  req2.req_valid = true;
  req2.op = MD_DIV;
  req2.w = true;
  req2.a = 0x00000000FFFF0000ull;
  req2.b = 0x0000000000000003ull;
  req2.id = Ident{9, 9, 9};
  req2.res_ready = false;
  Obs cur2 = h->Cycle(req2);
  if (!cur2.req_ready) Stop("backpressure", "request refused in idle (kill)");
  while (!cur2.res_valid) cur2 = h->Cycle(Stim{});
  const uint32_t completed_before = cur2.completed;
  const uint32_t killed_before = cur2.killed;
  Stim kill;
  kill.flush = true;
  const Obs killed = h->Cycle(kill);
  reporter->Check(!killed.res_valid,
                  "backpressure: a killed result was offered in the flush cycle");
  reporter->Check(killed.completed == completed_before,
                  "backpressure: a killed result was counted as completed");
  reporter->Check(killed.killed == killed_before + 1,
                  "backpressure: the killed result was not counted exactly once");
  reporter->Check(!killed.busy, "backpressure: the unit stayed busy after the kill");
}

void PhaseResetCancel(Harness* h, mosaic::Reporter* reporter) {
  h->SetContext("reset during an operation");
  Stim req;
  req.req_valid = true;
  req.op = MD_MULH;
  req.w = false;
  req.a = 0xDEADBEEFDEADBEEFull;
  req.b = 0x0123456789ABCDEFull;
  req.id = Ident{42, 42, 3};
  Obs cur = h->Cycle(req);
  reporter->Check(cur.req_ready, "reset-cancel: request not accepted");
  if (!cur.req_ready) Stop("reset-cancel", "request refused in idle");
  for (int i = 0; i < 5; ++i) {
    cur = h->Cycle(Stim{});
  }
  reporter->Check(cur.busy, "reset-cancel: the operation was not in flight");

  // Reset with no other stimulus. The shadow is reset with it, so the counters
  // are compared from zero again: a reset is a new epoch, not an event in the
  // old one.
  h->Reset(2);
  const Obs o = h->Cycle(Stim{});
  reporter->Check(o.req_ready, "reset-cancel: not ready after reset");
  reporter->Check(!o.busy, "reset-cancel: still busy after reset");
  reporter->Check(!o.res_valid, "reset-cancel: a result survived the reset");
  reporter->Check(o.accepted == 0 && o.completed == 0 && o.cancelled == 0 &&
                      o.killed == 0,
                  "reset-cancel: the counters survived the reset");
  reporter->Check(o.iter == 0, "reset-cancel: o_iter survived the reset");

  // And the unit is usable straight afterwards.
  h->SetContext("reset-cancel follow-up");
  const Ident id = Ident{1, 2, 3};
  Stim after;
  after.req_valid = true;
  after.op = MD_MULHU;
  after.w = false;
  after.a = 0xFFFFFFFFFFFFFFFFull;
  after.b = 0xFFFFFFFFFFFFFFFFull;
  after.id = id;
  after.res_ready = true;
  const Obs acc = h->Cycle(after);
  reporter->Check(acc.req_ready, "reset-cancel: refused a request after reset");
  if (!acc.req_ready) Stop("reset-cancel", "request refused after reset");
  const Obs res = h->RunToResult(after, 200, "reset-cancel follow-up");
  reporter->Check(res.res_data == RefCalc(MD_MULHU, false, after.a, after.b),
                  "reset-cancel: the follow-up result was wrong");
  reporter->Check(res.res_id == h->MaskId(id),
                  "reset-cancel: the follow-up identity was wrong");
}

void PhaseRandom(Harness* h, uint32_t seed, uint32_t cycles, uint64_t* flushes) {
  mosaic::Rng rng(seed ^ 0x9E3779B9u);
  uint64_t ident_counter = 0x10000;
  for (uint32_t i = 0; i < cycles; ++i) {
    Stim s;
    s.res_ready = rng.Chance(60);
    const uint32_t roll = rng.Below(100);
    if (roll < 55) {
      s.req_valid = true;
      s.op = static_cast<uint8_t>(rng.Below(8));
      s.w = rng.Chance(40);
      s.a = rng.Next();
      s.b = rng.Next();
      if (s.w) {
        // Give the W case garbage in the upper half often enough that reading
        // the wrong half is not a rare event.
        s.a = (s.a & 0xFFFFFFFFull) | (rng.Next() << 32);
        s.b = (s.b & 0xFFFFFFFFull) | (rng.Next() << 32);
      }
      const uint64_t n = ident_counter++;
      s.id = Ident{n & 0x3F, (n * 3) & 0x7F, n & 0x7};
    }
    s.flush = roll >= 90;
    if (s.flush) ++*flushes;
    h->SetContext("random " + Dec(i));
    h->Cycle(s);
  }
}

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr,
                 "usage: %s --case <id> [--out <dir>] [--seed <n>] "
                 "[--max-cycles <n>] [--verbose]\n%s\n",
                 argv[0], error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options, std::string("Verilator ") +
                                        Verilated::productVersion());
  mosaic::ClockDriver clk;
  Vmosaic_muldiv_tb dut;

  Harness harness(&dut, &clk, &reporter, options.max_cycles);
  ShadowMulDiv shadow;

  std::string detail;
  bool aborted = false;
  try {
    // The geometry comes from the elaborated DUT, never from a literal here.
    harness.Reset(4);
    const uint32_t rob_w = dut.o_rob_index_w;
    const uint32_t gen_w = dut.o_rob_gen_w;
    const uint32_t uop_w = dut.o_uop_index_w;
    if (rob_w == 0 || rob_w > 32 || gen_w == 0 || gen_w > 32 || uop_w == 0 ||
        uop_w > 32) {
      Stop("geometry", "the DUT reported an unusable identity width");
    }
    const uint64_t rob_mask = (1ull << rob_w) - 1ull;
    const uint64_t gen_mask = (1ull << gen_w) - 1ull;
    const uint64_t uop_mask = (1ull << uop_w) - 1ull;

    auto fresh = [&]() {
      harness.Reset(4);
      shadow.Reset();
      shadow.SetMasks(rob_mask, gen_mask, uop_mask);
      harness.SetMasks(rob_mask, gen_mask, uop_mask);
      harness.BindShadow(&shadow);
      harness.SetContext("");
    };

    fresh();
    harness.Phase("reset-state");
    PhaseResetState(&harness, &reporter);

    fresh();
    harness.Phase("directed-edges");
    uint64_t ops_done = 0;
    PhaseDirected(&harness, &reporter, &ops_done);

    fresh();
    harness.Phase("cancel-sweep");
    uint64_t cancels = 0, kills = 0;
    PhaseCancelSweep(&harness, &reporter, &cancels, &kills);

    fresh();
    harness.Phase("backpressure");
    uint64_t held = 0;
    PhaseBackpressure(&harness, &reporter, &held);

    fresh();
    harness.Phase("reset-cancel");
    PhaseResetCancel(&harness, &reporter);

    fresh();
    harness.Phase("random");
    uint64_t flushes = 0;
    PhaseRandom(&harness, static_cast<uint32_t>(options.seed), 4000, &flushes);

    reporter.Check(ops_done > 700,
                   "coverage: directed operations exercised: " + Dec(ops_done));
    reporter.Check(cancels == 65 + 33,
                   "coverage: every cancel position was exercised (" +
                       Dec(cancels) + " cancellations)");
    reporter.Check(kills == 2,
                   "coverage: the killed-result path was exercised for both "
                   "widths (" + Dec(kills) + ")");
    reporter.Check(held == 100,
                   "coverage: back-pressure held a result for " + Dec(held) +
                       " cycles");
    reporter.Check(flushes > 100,
                   "coverage: random flushes exercised: " + Dec(flushes));

    detail = "muldiv contract holds: " + Dec(harness.comparisons()) +
             " shadow comparisons over " + Dec(harness.cycles()) + " cycles, " +
             Dec(ops_done) + " directed operations, " + Dec(cancels) +
             " cancellations (" + Dec(kills) +
             " with a computed result), seed " + Dec(options.seed);
  } catch (const Abort& a) {
    reporter.Mismatch(a.what, "the campaign to finish", "it stopped");
    detail = "aborted: " + a.what;
    aborted = true;
  }

  const bool passed = !aborted && reporter.failures() == 0;
  dut.final();
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
