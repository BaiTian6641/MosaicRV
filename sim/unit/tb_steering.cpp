// ============================================================================
// tb_steering.cpp -- CASE=steering.capacity_locality, work package I-029.
//
// The DUT is `mosaic_steering`: the dynamic steering baseline policy. It is
// never its own oracle. The driver is the environment the policy steers into --
// the two clusters' integer ALUs, the shared MUL/DIV and the shared LSU, each
// with a capability mask, an occupancy, a capacity and an execution latency --
// and, every cycle of every phase, an independent comparison of the DUT's whole
// output surface against the contract written in rtl/core/mosaic_steering.sv.
// The shadow shares no code with the RTL and derives its answers from that
// prose, so agreeing with it is evidence about the contract rather than a
// restatement of the implementation.
//
// ---------------------------------------------------------------- the case
//
// The card asks for one thing to be shown as an experiment: the *same program,
// with the same seed*, run through the *same machine* twice -- once with the
// fixed-affinity baseline and once with dynamic steering -- must produce
// identical architectural results, the dynamic run must never hand a macro to a
// unit that cannot execute it, and saturation must produce stalls and retries
// rather than a different answer. The measured difference between the two runs
// (cycles, stalls, issues per unit) is reported whatever it is: a policy that
// changed nothing would be a finding, not a failure.
//
// The driver therefore does not merely observe the DUT. It runs a program:
//
//   * one macro is offered to the policy per cycle, in program order, exactly
//     as dispatch would offer it, with its program-order age and the locality
//     of the unit that produced its operand;
//   * a grant admits the macro into that unit's queue, where it waits for its
//     operands and then occupies the unit for that unit's latency;
//   * a stall re-offers the *same* macro next cycle (the retry the card names),
//     and a reject resolves the macro at the architectural boundary with no
//     unit at all (p0's SYSTEM class);
//   * a macro retires only when it and every older macro have completed, the
//     ROB's in-order rule, so the retire stream is in program order and
//     independent of the routing;
//   * the value of a macro is computed by the unit that executes it, with the
//     class's semantics when the unit implements that class and with a poisoned
//     value when it does not. A routing defect is therefore visible in the
//     architectural results and not only in a routing check.
//
// The architectural result of a run is its retire stream -- (macro index,
// class, whether it wrote a value, the value) in program order -- plus its
// signature. Both modes are compared to each other and to the program's own
// reference, which is computed without the DUT and without the policy.
//
// The policy's generality is exercised with randomised capability matrices and
// fixed tables in the soak, not only with the p0 instance the case ships with:
// the DUT must be correct for the matrix it is handed, which is what makes it a
// policy rather than a hardcoded table.
//
// ---------------------------------------------------- why this is not circular
//
// The shadow is a second implementation of the four keys (capability, locality,
// load, age) and of nothing else; it never runs the program and never computes
// a value. The program model contains no policy. A bug in either would have to
// be mirrored by the other to pass, and the mutants in
// tools/run_steering_controls.py show that each key can fail alone.
//
// ------------------------------------------------------------------ phases
//
//   1. geometry         the widths the driver drives are the DUT's own.
//   2. directed         the four keys and the fallbacks, one stimulus at a time,
//                       each checked against the shadow *and* against an
//                       expectation written here: the capability filter, SYSTEM
//                       rejection, locality preference and its spill at
//                       saturation, least-loaded, the age tie-break, the fixed
//                       baseline, and stalling when the only capable unit is
//                       full.
//   3. fixed-vs-dynamic the same generated program and seed in both modes: the
//                       architectural streams and signatures must agree, the
//                       routing must not, and cycles/stalls/issues are reported.
//   4. saturation       a directed MUL/DIV burst whose only capable unit is
//                       full: stalls and retries are required, the retried
//                       macro is the same macro, and every macro still retires
//                       once with its reference value.
//   5. matrix           a directed matrix in which two units tie, so the age key
//                       decides, and a matrix in which a class has no unit.
//   6. random           a seeded soak with a randomised capability matrix,
//                       occupancy and strategy, every output compared against
//                       the shadow every cycle, with every key's decisions
//                       covered.
//   7. determinism      the same program and seed reproduce the run exactly.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_steering_tb.h"

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

uint64_t HashMix(uint64_t hash, uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
  return hash;
}

// ============================================================================
// the one machine both modes steer into
// ============================================================================
//
// These are the resources the plan fixes for p0 (section 1.2): two clusters each
// with one integer ALU, one shared iterative MUL/DIV, one shared LSU. The policy
// sees them only through the capability matrix, the occupancy and the room
// pins, so this table is the *environment*; its numbers are the scope of the
// measured case, not a claim about another geometry.

enum Cls { CLS_ALU = 0, CLS_BRANCH = 1, CLS_MULDIV = 2, CLS_LOAD = 3, CLS_STORE = 4, CLS_SYSTEM = 5 };
constexpr int kN = 4;
constexpr int kC = 6;

struct UnitSpec {
  const char* name;
  uint32_t cap;       // bitmask over Cls
  uint32_t locality;  // the cluster it belongs to
  bool shared;        // serves both clusters
  uint32_t capacity;  // queue entries, queue and execution together
  uint32_t latency;   // cycles from start to completion
};

const UnitSpec kUnits[kN] = {
    {"cluster0-alu", (1u << CLS_ALU) | (1u << CLS_BRANCH), 0, false, 2, 3},
    {"cluster1-alu", (1u << CLS_ALU) | (1u << CLS_BRANCH), 1, false, 2, 3},
    {"muldiv",       (1u << CLS_MULDIV),                   0, true,  2, 8},
    {"lsu",          (1u << CLS_LOAD) | (1u << CLS_STORE), 0, true,  4, 3},
};

// The fixed-affinity baseline the card names as a control: every class is
// pinned to one unit. SYSTEM has no unit; the capability filter rejects it
// before the table is ever read, so its entry is a filler.
const uint32_t kFixed[kC] = {0, 0, 2, 3, 3, 0};

bool UnitCapable(int unit, int cls) { return ((kUnits[unit].cap >> cls) & 1u) != 0; }

// The professional answer of a unit: the class's semantics when the unit
// implements the class, and a poisoned value when it does not. The second half
// is what makes "an incompatible unit received work" an architectural
// difference and not merely a routing observation.
uint64_t ExecOnUnit(int unit, int cls, uint64_t operand, uint32_t index) {
  if (!UnitCapable(unit, cls)) {
    return 0x0bad000000000000ull | static_cast<uint64_t>(index);
  }
  switch (cls) {
    case CLS_ALU:    return operand + static_cast<uint64_t>(index) * 3ull + 1ull;
    case CLS_BRANCH: return operand ^ (static_cast<uint64_t>(index) * 0x9e3779b97f4a7c15ull);
    case CLS_MULDIV: return operand * static_cast<uint64_t>(index | 1u) + 7ull;
    case CLS_LOAD:   return operand + static_cast<uint64_t>(index) * 8ull;
    default:         return 0ull;  // STORE, SYSTEM: no architectural value
  }
}

bool ClassWritesValue(int cls) { return cls != CLS_STORE && cls != CLS_SYSTEM; }

// ------------------------------------------------------------------ stimulus

struct Stim {
  bool mode_dyn = false;
  bool req_valid = false;
  uint32_t req_class = 0;
  bool req_locality = false;
  bool req_locality_en = false;
  uint32_t req_age = 0;
  bool unit_room[kN] = {true, true, true, true};
  uint32_t unit_occ[kN] = {0, 0, 0, 0};
  uint32_t unit_cap[kN] = {0, 0, 0, 0};
  uint32_t unit_locality = 0;
  uint32_t unit_shared = 0;
  uint32_t fixed_unit[kC] = {0, 0, 0, 0, 0, 0};
};

// The environment's default stimulus: the machine table above, with the
// occupancy the caller supplies and room derived from the capacity.
Stim BaseStim(bool mode_dyn, const uint32_t occ[kN]) {
  Stim s;
  s.mode_dyn = mode_dyn;
  s.unit_locality = 0;
  s.unit_shared = 0;
  for (int u = 0; u < kN; u++) {
    s.unit_cap[u] = kUnits[u].cap;
    s.unit_occ[u] = occ[u];
    s.unit_room[u] = occ[u] < kUnits[u].capacity;
    if (kUnits[u].shared) s.unit_shared |= (1u << u);
    s.unit_locality |= (kUnits[u].locality << u);
  }
  for (int c = 0; c < kC; c++) s.fixed_unit[c] = kFixed[c];
  return s;
}

struct Observed {
  bool grant_valid = false;
  uint32_t grant_unit = 0;
  uint32_t grant_reason = 0;
  bool stall = false;
  bool reject = false;
  uint32_t issues[kN] = {0, 0, 0, 0};
  uint32_t grant_ctr = 0;
  uint32_t stall_ctr = 0;
  uint32_t reject_ctr = 0;
};

// ============================================================================
// the shadow: the contract restated independently
// ============================================================================

class Shadow {
 public:
  struct Pred {
    bool grant_valid = false;
    uint32_t grant_unit = 0;
    uint32_t grant_reason = 0;
    bool stall = false;
    bool reject = false;
    uint32_t tie_mask = 0;  // the least-loaded pool the age key had to break
  };

  void Reset() {
    for (int u = 0; u < kN; u++) last_age_[u] = 0;
    grants_ = stalls_ = rejects_ = 0;
    for (int u = 0; u < kN; u++) issues_[u] = 0;
  }

  Pred Eval(const Stim& s) const {
    Pred p;
    bool cap_ok[kN];
    bool cand[kN];
    bool pref[kN];
    bool use[kN];
    bool min_set[kN];
    int cnt_cand = 0, cnt_use = 0, cnt_min = 0;
    const uint32_t cls = s.req_class;

    for (int u = 0; u < kN; u++) {
      const bool shared = ((s.unit_shared >> u) & 1u) != 0;
      const bool local = (((s.unit_locality >> u) & 1u) != 0) == s.req_locality;
      cap_ok[u] = ((s.unit_cap[u] >> cls) & 1u) != 0;
      cand[u] = cap_ok[u] && s.unit_room[u];
      pref[u] = cand[u] && (!s.req_locality_en || shared || local);
    }
    bool any_cap = false, any_cand = false, any_pref = false;
    for (int u = 0; u < kN; u++) {
      any_cap = any_cap || cap_ok[u];
      any_cand = any_cand || cand[u];
      any_pref = any_pref || pref[u];
      if (cand[u]) cnt_cand++;
    }
    for (int u = 0; u < kN; u++) {
      use[u] = any_pref ? pref[u] : cand[u];
      if (use[u]) cnt_use++;
    }
    uint32_t min_occ = 0xffffffffu;
    for (int u = 0; u < kN; u++) {
      if (use[u] && s.unit_occ[u] < min_occ) min_occ = s.unit_occ[u];
    }
    for (int u = 0; u < kN; u++) {
      min_set[u] = use[u] && (s.unit_occ[u] == min_occ);
      if (min_set[u]) cnt_min++;
    }
    uint32_t min_age = 0xffffffffu;
    for (int u = 0; u < kN; u++) {
      if (min_set[u] && last_age_[u] < min_age) min_age = last_age_[u];
    }
    int sel = -1;
    for (int u = 0; u < kN; u++) {
      if (min_set[u] && last_age_[u] == min_age && sel < 0) sel = u;
    }

    p.reject = s.req_valid && !any_cap;
    if (s.mode_dyn) {
      p.grant_valid = s.req_valid && any_cand;
      p.grant_unit = (sel < 0) ? 0u : static_cast<uint32_t>(sel);
      if (cnt_use == 0) {
        p.grant_reason = 0;
      } else if (cnt_use == 1 && cnt_cand > 1) {
        p.grant_reason = 3;
      } else if (cnt_use == 1) {
        p.grant_reason = 2;
      } else if (cnt_min == 1) {
        p.grant_reason = 4;
      } else {
        p.grant_reason = 5;
      }
    } else {
      const uint32_t fx = s.fixed_unit[cls];
      p.grant_valid = s.req_valid && cap_ok[fx] && s.unit_room[fx];
      p.grant_unit = fx;
      p.grant_reason = 1;
    }
    p.stall = s.req_valid && !p.grant_valid && !p.reject;
    for (int u = 0; u < kN; u++) {
      if (min_set[u]) p.tie_mask |= (1u << u);
    }
    return p;
  }

  void Apply(const Stim& s, const Pred& p) {
    if (p.grant_valid) {
      last_age_[p.grant_unit] = s.req_age;
      issues_[p.grant_unit]++;
      grants_++;
    }
    if (p.stall) stalls_++;
    if (p.reject) rejects_++;
  }

  uint32_t issues(int u) const { return issues_[u]; }
  uint32_t grants() const { return grants_; }
  uint32_t stalls() const { return stalls_; }
  uint32_t rejects() const { return rejects_; }

 private:
  uint32_t last_age_[kN] = {0, 0, 0, 0};
  uint32_t issues_[kN] = {0, 0, 0, 0};
  uint32_t grants_ = 0, stalls_ = 0, rejects_ = 0;
};

// ----------------------------------------------------------------- the harness

class Harness {
 public:
  Harness(Vmosaic_steering_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  Vmosaic_steering_tb* dut() { return dut_; }
  void Phase(const std::string& name) { phase_ = name; }
  const Shadow& shadow() const { return shadow_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t cycles() const { return cycles_; }

  void Reset(int cycles) {
    shadow_.Reset();
    for (int i = 0; i < cycles; i++) Cycle(Stim{}, /*rst=*/true);
  }

  Observed Cycle(const Stim& s, bool rst = false) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_, "max-cycles (" + Dec(max_cycles_) + ") exhausted before the phase finished");
    }
    const std::string where = phase_ + ": cycle " + Dec(cycles_);
    Drive(s, rst);
    dut_->eval();

    Observed o;
    Shadow::Pred p;
    if (!rst) {
      Capture(&o);
      // The card's two fail modes are checked against the pins the DUT was
      // given, before the contract comparison, so a violation of either is
      // reported in the card's own terms rather than as a field difference.
      if (o.grant_valid) {
        Require(static_cast<int>(o.grant_unit) < kN, where + " (grant range)",
                "grant_unit " + Dec(o.grant_unit) + " is out of range");
        Require(((s.unit_cap[o.grant_unit] >> s.req_class) & 1u) != 0, where + " (incompatible unit)",
                "incompatible unit: a macro of class " + Dec(s.req_class) +
                    " was granted to unit " + Dec(o.grant_unit) +
                    " whose capability mask is " + Hex64(s.unit_cap[o.grant_unit]));
        Require(s.unit_room[o.grant_unit], where + " (saturation)",
                "saturation: a macro was admitted to unit " + Dec(o.grant_unit) +
                    " which reported no room");
      }
      p = shadow_.Eval(s);
      Compare(o, p, where);
    }

    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (!rst) {
      o.issues[0] = dut_->o_unit_issues0_o;
      o.issues[1] = dut_->o_unit_issues1_o;
      o.issues[2] = dut_->o_unit_issues2_o;
      o.issues[3] = dut_->o_unit_issues3_o;
      o.grant_ctr = dut_->o_grant_ctr_o;
      o.stall_ctr = dut_->o_stall_ctr_o;
      o.reject_ctr = dut_->o_reject_ctr_o;
      shadow_.Apply(s, p);
      Require(o.issues[0] == shadow_.issues(0) && o.issues[1] == shadow_.issues(1) &&
                  o.issues[2] == shadow_.issues(2) && o.issues[3] == shadow_.issues(3),
              where + " (per-unit issues)",
              "issues " + Dec(o.issues[0]) + "/" + Dec(o.issues[1]) + "/" + Dec(o.issues[2]) +
                  "/" + Dec(o.issues[3]) + ", contract " + Dec(shadow_.issues(0)) + "/" +
                  Dec(shadow_.issues(1)) + "/" + Dec(shadow_.issues(2)) + "/" +
                  Dec(shadow_.issues(3)));
      Require(o.grant_ctr == shadow_.grants() && o.stall_ctr == shadow_.stalls() &&
                  o.reject_ctr == shadow_.rejects(),
              where + " (counters)",
              "grant/stall/reject " + Dec(o.grant_ctr) + "/" + Dec(o.stall_ctr) + "/" +
                  Dec(o.reject_ctr) + ", contract " + Dec(shadow_.grants()) + "/" +
                  Dec(shadow_.stalls()) + "/" + Dec(shadow_.rejects()));
    }
    return o;
  }

 private:
  void Drive(const Stim& s, bool rst) {
    dut_->rst = rst;
    dut_->mode_dyn_i = s.mode_dyn;
    dut_->req_valid_i = s.req_valid;
    dut_->req_class_i = s.req_class;
    dut_->req_locality_i = s.req_locality;
    dut_->req_locality_en_i = s.req_locality_en;
    dut_->req_age_i = s.req_age;
    dut_->unit_room0_i = s.unit_room[0];
    dut_->unit_room1_i = s.unit_room[1];
    dut_->unit_room2_i = s.unit_room[2];
    dut_->unit_room3_i = s.unit_room[3];
    dut_->unit_occ0_i = s.unit_occ[0];
    dut_->unit_occ1_i = s.unit_occ[1];
    dut_->unit_occ2_i = s.unit_occ[2];
    dut_->unit_occ3_i = s.unit_occ[3];
    dut_->unit_cap0_i = s.unit_cap[0];
    dut_->unit_cap1_i = s.unit_cap[1];
    dut_->unit_cap2_i = s.unit_cap[2];
    dut_->unit_cap3_i = s.unit_cap[3];
    dut_->unit_locality_i = s.unit_locality;
    dut_->unit_shared_i = s.unit_shared;
    uint64_t fixed = 0;
    for (int c = 0; c < kC; c++) fixed |= static_cast<uint64_t>(s.fixed_unit[c] & 0x3u) << (c * 8);
    dut_->fixed_unit_i = fixed;
  }

  void Capture(Observed* o) {
    o->grant_valid = dut_->grant_valid_o;
    o->grant_unit = dut_->grant_unit_o;
    o->grant_reason = dut_->grant_reason_o;
    o->stall = dut_->stall_o;
    o->reject = dut_->reject_o;
  }

  void Compare(const Observed& o, const Shadow::Pred& p, const std::string& where) {
    comparisons_++;
    std::string field;
    if (o.grant_valid != p.grant_valid) field = "grant_valid";
    else if (o.grant_valid && o.grant_unit != p.grant_unit) field = "grant_unit";
    else if (o.grant_valid && o.grant_reason != p.grant_reason) field = "grant_reason";
    else if (o.stall != p.stall) field = "stall";
    else if (o.reject != p.reject) field = "reject";
    if (field.empty()) return;
    if (field == "grant_unit" && p.grant_valid && p.grant_reason == 5 &&
        (p.tie_mask & (1u << o.grant_unit)) != 0 &&
        (p.tie_mask & (1u << p.grant_unit)) != 0) {
      Fail(where + " (age tie-break)",
           "the least-loaded pool {mask " + Hex64(p.tie_mask) +
               "} was broken differently from the contract: the grant history says unit " +
               Dec(p.grant_unit) + ", the DUT chose unit " + Dec(o.grant_unit) +
               ", so the decision is not the function of the program's state the contract "
               "names");
    }
    const uint64_t expected = field == "grant_valid" ? (p.grant_valid ? 1 : 0)
                             : field == "grant_unit" ? p.grant_unit
                             : field == "grant_reason" ? p.grant_reason
                             : field == "stall" ? (p.stall ? 1 : 0)
                                                : (p.reject ? 1 : 0);
    const uint64_t actual = field == "grant_valid" ? (o.grant_valid ? 1 : 0)
                           : field == "grant_unit" ? o.grant_unit
                           : field == "grant_reason" ? o.grant_reason
                           : field == "stall" ? (o.stall ? 1 : 0)
                                              : (o.reject ? 1 : 0);
    Fail(where + " (" + field + ")",
         field + ": expected " + Dec(expected) + ", got " + Dec(actual));
  }

  Vmosaic_steering_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  std::string phase_ = "boot";
  Shadow shadow_;
};

// ============================================================================
// the program model
// ============================================================================

struct Uop {
  int cls = CLS_ALU;
  int dep = -1;  // index of the producer, or -1
};

uint64_t SeedOperand(uint64_t seed) { return seed * 0x9e3779b97f4a7c15ull + 0x1234567ull; }

// A deterministic program. The class mix makes the two ALUs the contended
// resource and MUL/DIV the small, easily saturated one, and most macros consume
// the immediately preceding one, so dependency lifetime -- not only dispatch
// rate -- decides how long an entry holds its unit's queue.
std::vector<Uop> GenerateProgram(mosaic::Rng* rng, int n) {
  std::vector<Uop> program;
  program.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    Uop u;
    const uint32_t draw = rng->Below(100);
    if (draw < 52) u.cls = CLS_ALU;
    else if (draw < 64) u.cls = CLS_BRANCH;
    else if (draw < 74) u.cls = CLS_MULDIV;
    else if (draw < 88) u.cls = CLS_LOAD;
    else if (draw < 93) u.cls = CLS_STORE;
    else u.cls = CLS_SYSTEM;
    if (i > 0) {
      const uint32_t d = rng->Below(100);
      if (d < 62) u.dep = i - 1;
      else if (d < 86) u.dep = static_cast<int>(rng->Below(static_cast<uint32_t>(i)));
      else u.dep = -1;
    }
    program.push_back(u);
  }
  return program;
}

struct RetireEvent {
  int index = 0;
  int cls = 0;
  bool has_value = false;
  uint64_t value = 0;
};

struct Decision {
  int index = 0;
  int unit = -1;
  uint32_t reason = 0;
  uint64_t cycle = 0;
};

struct RunResult {
  std::vector<RetireEvent> retire;
  std::vector<Decision> decisions;
  uint64_t signature = 0;
  uint64_t cycles = 0;
  uint64_t stalls = 0;
  uint64_t rejects = 0;
  uint64_t grants = 0;
  uint64_t issue[kN] = {0, 0, 0, 0};
  uint64_t peak_occ[kN] = {0, 0, 0, 0};
};

// The reference retire stream: what the program's own model says happened,
// computed with no DUT and no policy.
std::vector<RetireEvent> ReferenceStream(const std::vector<Uop>& program, uint64_t seed) {
  std::vector<RetireEvent> out;
  std::vector<uint64_t> v(program.size(), 0);
  for (size_t i = 0; i < program.size(); i++) {
    const uint64_t operand = program[i].dep >= 0 ? v[static_cast<size_t>(program[i].dep)]
                                                 : SeedOperand(seed);
    // The reference is computed with no policy: any unit that implements the
    // class produces the same value, so the reference names the first one.
    int ref_unit = -1;
    for (int u = 0; u < kN && ref_unit < 0; u++) {
      if (UnitCapable(u, program[i].cls)) ref_unit = u;
    }
    v[i] = ClassWritesValue(program[i].cls)
               ? ExecOnUnit(ref_unit < 0 ? 0 : ref_unit, program[i].cls, operand,
                            static_cast<uint32_t>(i))
               : 0;
    RetireEvent e;
    e.index = static_cast<int>(i);
    e.cls = program[i].cls;
    e.has_value = ClassWritesValue(program[i].cls);
    e.value = v[i];
    out.push_back(e);
  }
  return out;
}

uint64_t StreamSignature(const std::vector<RetireEvent>& stream) {
  uint64_t hash = 1469598103934665603ull;
  for (const RetireEvent& e : stream) {
    hash = HashMix(hash, static_cast<uint64_t>(e.index));
    hash = HashMix(hash, static_cast<uint64_t>(e.cls));
    hash = HashMix(hash, e.has_value ? 1ull : 0ull);
    hash = HashMix(hash, e.value);
  }
  return hash;
}

// ============================================================================
// the machine: dispatch -> steer -> queue -> execute -> retire
// ============================================================================

struct RunOptions {
  bool require_stalls = false;
  bool trace_decisions = false;
};

RunResult RunProgram(Harness* harness, const std::vector<Uop>& program, uint64_t seed,
                     bool mode_dyn, const std::string& label, const RunOptions& opts) {
  const int n = static_cast<int>(program.size());
  RunResult r;
  std::vector<uint64_t> value(static_cast<size_t>(n), 0);

  harness->Phase(label);
  harness->Reset(4);

  std::deque<int> queue[kN];
  uint32_t occ[kN] = {0, 0, 0, 0};
  uint64_t busy_until[kN] = {0, 0, 0, 0};
  std::deque<int> inflight[kN];
  std::vector<uint64_t> complete_at(static_cast<size_t>(n), 0);
  std::vector<bool> complete(static_cast<size_t>(n), false);
  std::vector<int> unit_of(static_cast<size_t>(n), -1);

  int head = 0;
  int retire_ptr = 0;
  const uint64_t budget = static_cast<uint64_t>(n) * 64ull + 4096ull;

  while (retire_ptr < n) {
    r.cycles++;
    if (r.cycles > budget) {
      Fail(label, "no progress: " + Dec(retire_ptr) + " of " + Dec(n) + " macros retired after " +
                      Dec(r.cycles) + " cycles");
    }

    // Completions scheduled for this cycle free their unit.
    for (int u = 0; u < kN; u++) {
      std::deque<int> keep;
      while (!inflight[u].empty()) {
        const int idx = inflight[u].front();
        inflight[u].pop_front();
        if (complete_at[static_cast<size_t>(idx)] == r.cycles) {
          complete[static_cast<size_t>(idx)] = true;
          occ[u]--;
        } else {
          keep.push_back(idx);
        }
      }
      inflight[u].swap(keep);
    }

    // The offer: the macro dispatch would insert this cycle, in program order.
    uint32_t occ_pin[kN] = {occ[0], occ[1], occ[2], occ[3]};
    Stim s = BaseStim(mode_dyn, occ_pin);
    if (head < n) {
      s.req_valid = true;
      s.req_class = static_cast<uint32_t>(program[head].cls);
      s.req_age = static_cast<uint32_t>(head);
      const int dep = program[head].dep;
      s.req_locality_en = (dep >= 0 && unit_of[static_cast<size_t>(dep)] >= 0 &&
                           unit_of[static_cast<size_t>(dep)] < 2);
      s.req_locality = (dep >= 0 && unit_of[static_cast<size_t>(dep)] == 1);
    }
    const Observed o = harness->Cycle(s);

    if (s.req_valid) {
      const int cls = program[head].cls;
      if (o.grant_valid) {
        const int u = static_cast<int>(o.grant_unit);
        Require(u >= 0 && u < kN, label, "grant_unit " + Dec(o.grant_unit) + " is out of range");
        Require(UnitCapable(u, cls), label,
                "incompatible unit: macro " + Dec(head) + " of class " + Dec(cls) +
                    " was granted to " + kUnits[u].name + " whose capability mask is " +
                    Hex64(kUnits[u].cap));
        Require(static_cast<uint32_t>(occ[u]) < kUnits[u].capacity, label,
                "saturation: macro " + Dec(head) + " was admitted to " + kUnits[u].name +
                    " which has no room (occupancy " + Dec(occ[u]) + " of capacity " +
                    Dec(kUnits[u].capacity) + ")");
        queue[u].push_back(head);
        occ[u]++;
        if (occ[u] > r.peak_occ[u]) r.peak_occ[u] = occ[u];
        unit_of[static_cast<size_t>(head)] = u;
        r.grants++;
        r.issue[u]++;
        if (opts.trace_decisions) {
          Decision d;
          d.index = head;
          d.unit = u;
          d.reason = o.grant_reason;
          d.cycle = r.cycles;
          r.decisions.push_back(d);
        }
        head++;
      } else if (o.stall) {
        r.stalls++;
      } else if (o.reject) {
        r.rejects++;
        complete[static_cast<size_t>(head)] = true;  // resolved at the boundary
        unit_of[static_cast<size_t>(head)] = -1;
        if (opts.trace_decisions) {
          Decision d;
          d.index = head;
          d.unit = -1;
          d.reason = 0;
          d.cycle = r.cycles;
          r.decisions.push_back(d);
        }
        head++;
      } else {
        Fail(label, "macro " + Dec(head) +
                        " was offered and the policy neither granted, stalled nor rejected it");
      }
    }

    // Execution: each unit starts its oldest macro whose operands are ready.
    for (int u = 0; u < kN; u++) {
      if (queue[u].empty()) continue;
      if (busy_until[u] > r.cycles) continue;
      int pick = -1;
      for (size_t k = 0; k < queue[u].size(); k++) {
        const int idx = queue[u][k];
        const int dep = program[static_cast<size_t>(idx)].dep;
        if (dep < 0 || complete[static_cast<size_t>(dep)]) {
          pick = static_cast<int>(k);
          break;
        }
      }
      if (pick < 0) continue;
      const int idx = queue[u][static_cast<size_t>(pick)];
      queue[u].erase(queue[u].begin() + pick);
      const int dep = program[static_cast<size_t>(idx)].dep;
      const uint64_t operand = dep >= 0 ? value[static_cast<size_t>(dep)] : SeedOperand(seed);
      value[static_cast<size_t>(idx)] =
          ClassWritesValue(program[static_cast<size_t>(idx)].cls)
              ? ExecOnUnit(u, program[static_cast<size_t>(idx)].cls, operand,
                           static_cast<uint32_t>(idx))
              : 0;
      busy_until[u] = r.cycles + kUnits[u].latency;
      complete_at[static_cast<size_t>(idx)] = busy_until[u];
      inflight[u].push_back(idx);
    }

    // Retirement is in program order: a macro retires when it and every older
    // macro have completed, whichever units they ran on.
    while (retire_ptr < head && complete[static_cast<size_t>(retire_ptr)]) {
      RetireEvent e;
      e.index = retire_ptr;
      e.cls = program[static_cast<size_t>(retire_ptr)].cls;
      e.has_value = ClassWritesValue(program[static_cast<size_t>(retire_ptr)].cls);
      e.value = value[static_cast<size_t>(retire_ptr)];
      r.retire.push_back(e);
      retire_ptr++;
    }
  }

  Require(r.grants + r.rejects == static_cast<uint64_t>(n), label,
          "the run accounted for " + Dec(r.grants) + " grants and " + Dec(r.rejects) +
              " rejects for a program of " + Dec(n) + " macros");
  if (opts.require_stalls) {
    Require(r.stalls > 0, label, "the run never stalled, so saturation was not exercised");
  }
  r.signature = StreamSignature(r.retire);
  return r;
}

// ------------------------------------------------------------ comparisons

void CompareStreams(const RunResult& a, const RunResult& b, const std::string& where,
                    const std::string& label_a, const std::string& label_b) {
  Require(a.retire.size() == b.retire.size(), where,
          label_a + " retired " + Dec(a.retire.size()) + " macros, " + label_b + " " +
              Dec(b.retire.size()));
  for (size_t i = 0; i < a.retire.size(); i++) {
    const RetireEvent& x = a.retire[i];
    const RetireEvent& y = b.retire[i];
    Require(x.index == y.index && x.cls == y.cls && x.has_value == y.has_value &&
                x.value == y.value,
            where,
            "retire " + Dec(i) + ": " + label_a + " has (index " + Dec(x.index) + ", class " +
                Dec(x.cls) + ", value " + Hex64(x.value) + "), " + label_b + " has (index " +
                Dec(y.index) + ", class " + Dec(y.cls) + ", value " + Hex64(y.value) + ")");
  }
  Require(a.signature == b.signature, where,
          "the signatures differ: " + Hex64(a.signature) + " vs " + Hex64(b.signature));
}

void CompareToReference(const RunResult& r, const std::vector<RetireEvent>& ref,
                        const std::string& where, const std::string& label) {
  Require(r.retire.size() == ref.size(), where,
          label + " retired " + Dec(r.retire.size()) + " macros, the program has " +
              Dec(ref.size()));
  for (size_t i = 0; i < ref.size(); i++) {
    Require(r.retire[i].index == ref[i].index && r.retire[i].cls == ref[i].cls &&
                r.retire[i].has_value == ref[i].has_value && r.retire[i].value == ref[i].value,
            where,
            "retire " + Dec(i) + " of " + label + ": got (index " + Dec(r.retire[i].index) +
                ", class " + Dec(r.retire[i].cls) + ", value " + Hex64(r.retire[i].value) +
                "), the reference is (index " + Dec(ref[i].index) + ", class " +
                Dec(ref[i].cls) + ", value " + Hex64(ref[i].value) + ")");
  }
}

// ============================================================================
// phases
// ============================================================================

// One directed stimulus, driven once, with the unit and reason the rule
// predicts. The shadow has already checked the whole output surface; this
// checks that the expectation written here -- derived from the prose, not from
// the RTL -- is the one the policy met.
void Directed(Harness* harness, const std::string& name, const Stim& s, bool expect_grant,
              int expect_unit, uint32_t expect_reason, bool expect_stall, bool expect_reject) {
  harness->Reset(4);
  harness->Phase("directed:" + name);
  const Observed o = harness->Cycle(s);
  Require(o.grant_valid == expect_grant, "directed:" + name,
          "grant_valid expected " + Bool(expect_grant) + ", got " + Bool(o.grant_valid));
  if (expect_grant) {
    Require(static_cast<int>(o.grant_unit) == expect_unit, "directed:" + name,
            "grant_unit expected " + Dec(expect_unit) + ", got " + Dec(o.grant_unit));
    Require(o.grant_reason == expect_reason, "directed:" + name,
            "grant_reason expected " + Dec(expect_reason) + ", got " + Dec(o.grant_reason));
  }
  Require(o.stall == expect_stall, "directed:" + name,
          "stall expected " + Bool(expect_stall) + ", got " + Bool(o.stall));
  Require(o.reject == expect_reject, "directed:" + name,
          "reject expected " + Bool(expect_reject) + ", got " + Bool(o.reject));
}

void PhaseDirected(Harness* harness, mosaic::Reporter* reporter) {
  const uint32_t none[kN] = {0, 0, 0, 0};
  const uint32_t full_alu[kN] = {2, 2, 0, 0};
  const uint32_t alu0_full[kN] = {2, 0, 0, 0};

  // A macro image: class, locality preference, age.
  Stim s = BaseStim(true, none);
  s.req_valid = true;

  // 0. the capability filter first, because it is the card's first fail mode:
  //    an ALU macro with both ALUs full and the MUL/DIV and LSU idle must
  //    *stall*, not spill into a unit that cannot execute it.
  s = BaseStim(true, full_alu);
  s.req_valid = true;
  s.req_class = CLS_ALU;
  s.req_locality_en = false;
  s.req_age = 1;
  Directed(harness, "incompatible-unit-never-receives-work", s, false, 0, 0, true, false);

  s = BaseStim(true, none);
  s.req_valid = true;
  s.req_class = CLS_ALU;
  s.req_locality_en = true;
  s.req_locality = true;
  s.req_age = 1;
  Directed(harness, "locality-cluster1", s, true, 1, 3, false, false);

  s.req_locality = false;
  Directed(harness, "locality-cluster0", s, true, 0, 3, false, false);

  // 2. the preferred cluster is saturated: the other cluster takes it, and the
  //    locality key had nothing to decide between, so capability alone did.
  s = BaseStim(true, alu0_full);
  s.req_valid = true;
  s.req_class = CLS_ALU;
  s.req_locality_en = true;
  s.req_locality = false;
  s.req_age = 1;
  Directed(harness, "spill-when-local-full", s, true, 1, 2, false, false);

  // 3. every capable unit is full: stall, and nothing is granted.
  s = BaseStim(true, full_alu);
  s.req_valid = true;
  s.req_class = CLS_ALU;
  s.req_locality_en = false;
  s.req_age = 1;
  Directed(harness, "saturated-stalls", s, false, 0, 0, true, false);

  // 4. no unit implements SYSTEM: reject, never a grant.
  s = BaseStim(true, none);
  s.req_valid = true;
  s.req_class = CLS_SYSTEM;
  s.req_locality_en = false;
  s.req_age = 1;
  Directed(harness, "system-is-rejected", s, false, 0, 0, false, true);

  // 5. the shared units take their classes and are never a locality question.
  s = BaseStim(true, none);
  s.req_valid = true;
  s.req_class = CLS_MULDIV;
  s.req_locality_en = false;
  s.req_age = 1;
  Directed(harness, "muldiv-to-shared", s, true, 2, 2, false, false);

  s.req_class = CLS_LOAD;
  Directed(harness, "load-to-lsu", s, true, 3, 2, false, false);

  // 6. MUL/DIV is the only capable unit for its class, so a full one stalls.
  const uint32_t md_full[kN] = {0, 0, 2, 0};
  s = BaseStim(true, md_full);
  s.req_valid = true;
  s.req_class = CLS_MULDIV;
  s.req_locality_en = false;
  s.req_age = 1;
  Directed(harness, "muldiv-full-stalls", s, false, 0, 0, true, false);

  // 7. the fixed baseline ignores locality and load and pins each class.
  const uint32_t busy[kN] = {1, 0, 0, 0};
  s = BaseStim(false, busy);
  s.req_valid = true;
  s.req_class = CLS_ALU;
  s.req_locality_en = true;
  s.req_locality = true;
  s.req_age = 1;
  Directed(harness, "fixed-pins-alu-to-cluster0", s, true, 0, 1, false, false);

  s = BaseStim(false, none);
  s.req_valid = true;
  s.req_class = CLS_MULDIV;
  s.req_locality_en = false;
  s.req_age = 1;
  Directed(harness, "fixed-pins-muldiv", s, true, 2, 1, false, false);

  // 8. the fixed baseline stalls rather than spilling: its unit is full and it
  //    has no second choice.
  s = BaseStim(false, full_alu);
  s.req_valid = true;
  s.req_class = CLS_ALU;
  s.req_locality_en = false;
  s.req_age = 1;
  Directed(harness, "fixed-stalls-not-spills", s, false, 0, 0, true, false);

  // 9. load: with no locality preference and equal occupancy, the least loaded
  //    wins; when they are equally loaded the age key decides, and it is the
  //    grant history -- not the index -- that decides which is "older".
  s = BaseStim(true, none);
  s.req_valid = true;
  s.req_class = CLS_ALU;
  s.req_locality_en = false;
  s.req_age = 1;
  Directed(harness, "least-loaded-tie-age-first", s, true, 0, 5, false, false);

  // Drive two more offers from the same reset-free history: after U0 was fed at
  // age 1 it goes behind U1, and then behind itself again.
  harness->Phase("directed:tie-alternates");
  s.req_age = 2;
  Observed o = harness->Cycle(s);
  Require(o.grant_valid && o.grant_unit == 1 && o.grant_reason == 5, "directed:tie-alternates",
          "the age key must hand the second equal offer to the unit fed longest ago; got unit " +
              Dec(o.grant_unit) + " reason " + Dec(o.grant_reason));
  s.req_age = 3;
  o = harness->Cycle(s);
  Require(o.grant_valid && o.grant_unit == 0 && o.grant_reason == 5, "directed:tie-alternates",
          "the age key must alternate; got unit " + Dec(o.grant_unit) + " reason " +
              Dec(o.grant_reason));

  reporter->Check(true, "directed: capability filter, SYSTEM rejection, locality preference "
                        "and its spill, least-loaded, the age tie-break, the fixed baseline, "
                        "and stalling at saturation all answered as the rule states");
}

void PhaseFixedVsDynamic(Harness* harness, mosaic::Reporter* reporter, uint64_t seed) {
  mosaic::Rng rng(seed ^ 0x5eedull);
  const std::vector<Uop> program = GenerateProgram(&rng, 256);
  const std::vector<RetireEvent> reference = ReferenceStream(program, seed);

  RunOptions opts;
  opts.require_stalls = true;
  const RunResult fixed = RunProgram(harness, program, seed, false, "fixed", opts);
  const RunResult dynamic = RunProgram(harness, program, seed, true, "dynamic", opts);

  // The architectural result is the same and is the program's own.
  CompareStreams(fixed, dynamic, "fixed-vs-dynamic",
                 "the fixed-affinity run", "the dynamic-steering run");
  CompareToReference(fixed, reference, "fixed-vs-dynamic", "the fixed-affinity run");
  CompareToReference(dynamic, reference, "fixed-vs-dynamic", "the dynamic-steering run");

  // The routing must not be the same, or the comparison would be vacuous.
  bool routing_differs = false;
  for (int u = 0; u < kN; u++) routing_differs = routing_differs || fixed.issue[u] != dynamic.issue[u];
  Require(routing_differs, "fixed-vs-dynamic",
          "the two modes issued the same number of macros to every unit, so the switch changed "
          "nothing");

  std::printf("  [streams] %s\n", "fixed and dynamic retired identical streams");
  std::printf("  [fixed]   cycles=%llu stalls=%llu rejects=%llu issues=%llu/%llu/%llu/%llu "
              "peak-occ=%llu/%llu/%llu/%llu\n",
              (unsigned long long)fixed.cycles, (unsigned long long)fixed.stalls,
              (unsigned long long)fixed.rejects, (unsigned long long)fixed.issue[0],
              (unsigned long long)fixed.issue[1], (unsigned long long)fixed.issue[2],
              (unsigned long long)fixed.issue[3], (unsigned long long)fixed.peak_occ[0],
              (unsigned long long)fixed.peak_occ[1], (unsigned long long)fixed.peak_occ[2],
              (unsigned long long)fixed.peak_occ[3]);
  std::printf("  [dynamic] cycles=%llu stalls=%llu rejects=%llu issues=%llu/%llu/%llu/%llu "
              "peak-occ=%llu/%llu/%llu/%llu\n",
              (unsigned long long)dynamic.cycles, (unsigned long long)dynamic.stalls,
              (unsigned long long)dynamic.rejects, (unsigned long long)dynamic.issue[0],
              (unsigned long long)dynamic.issue[1], (unsigned long long)dynamic.issue[2],
              (unsigned long long)dynamic.issue[3], (unsigned long long)dynamic.peak_occ[0],
              (unsigned long long)dynamic.peak_occ[1], (unsigned long long)dynamic.peak_occ[2],
              (unsigned long long)dynamic.peak_occ[3]);
  std::printf("  [delta]   cycles=%+lld stalls=%+lld cluster0=%+lld cluster1=%+lld\n",
              (long long)dynamic.cycles - (long long)fixed.cycles,
              (long long)dynamic.stalls - (long long)fixed.stalls,
              (long long)dynamic.issue[0] - (long long)fixed.issue[0],
              (long long)dynamic.issue[1] - (long long)fixed.issue[1]);
  std::fflush(stdout);

  reporter->Check(true,
                  "fixed-vs-dynamic (same program, seed " + Dec(seed) + "): identical retire "
                  "streams and signatures, " + Hex64(dynamic.signature) + "; cycles " +
                      Dec(fixed.cycles) + " fixed vs " + Dec(dynamic.cycles) + " dynamic, "
                      "stalls " + Dec(fixed.stalls) + " vs " + Dec(dynamic.stalls) +
                      ", issues per unit fixed " + Dec(fixed.issue[0]) + "/" +
                      Dec(fixed.issue[1]) + "/" + Dec(fixed.issue[2]) + "/" +
                      Dec(fixed.issue[3]) + " vs dynamic " + Dec(dynamic.issue[0]) + "/" +
                      Dec(dynamic.issue[1]) + "/" + Dec(dynamic.issue[2]) + "/" +
                      Dec(dynamic.issue[3]));
}

void PhaseSaturation(Harness* harness, mosaic::Reporter* reporter, uint64_t seed) {
  // Five MUL/DIV macros with two queue entries and one execution stage each is
  // saturation by construction: the shared unit is the only one that can take
  // them, so the third offer must stall until a completion frees a slot.
  std::vector<Uop> program;
  for (int i = 0; i < 5; i++) {
    Uop u;
    u.cls = CLS_MULDIV;
    u.dep = -1;
    program.push_back(u);
  }
  for (int i = 0; i < 4; i++) {
    Uop u;
    u.cls = CLS_ALU;
    u.dep = 4;  // every ALU waits for the last MUL/DIV
    program.push_back(u);
  }
  const std::vector<RetireEvent> reference = ReferenceStream(program, seed);

  RunOptions opts;
  opts.require_stalls = true;
  const RunResult fixed = RunProgram(harness, program, seed, false, "saturation-fixed", opts);
  const RunResult dynamic = RunProgram(harness, program, seed, true, "saturation-dynamic", opts);

  CompareToReference(fixed, reference, "saturation", "the saturated fixed-affinity run");
  CompareToReference(dynamic, reference, "saturation", "the saturated dynamic run");
  CompareStreams(fixed, dynamic, "saturation", "the saturated fixed run", "the saturated dynamic run");
  Require(dynamic.peak_occ[2] <= kUnits[2].capacity, "saturation",
          "the MUL/DIV occupancy reached " + Dec(dynamic.peak_occ[2]) + " of a capacity of " +
              Dec(kUnits[2].capacity));
  // Saturation was actually reached: the queue filled to its capacity, so the
  // stalls above are the policy holding macros back, not an idle machine.
  Require(dynamic.peak_occ[2] == kUnits[2].capacity, "saturation",
          "the MUL/DIV queue never filled (" + Dec(dynamic.peak_occ[2]) + " of capacity " +
              Dec(kUnits[2].capacity) + "), so this run does not show saturation");
  Require(dynamic.issue[2] == 5, "saturation",
          "the MUL/DIV unit issued " + Dec(dynamic.issue[2]) + " of the 5 MUL/DIV macros");

  std::printf("  [saturation] macros=%d stalls=%llu cycles=%llu peak-occ(muldiv)=%llu\n",
              static_cast<int>(program.size()), (unsigned long long)dynamic.stalls,
              (unsigned long long)dynamic.cycles, (unsigned long long)dynamic.peak_occ[2]);
  std::fflush(stdout);

  reporter->Check(true,
                  "saturation: " + Dec(dynamic.stalls) + " stall(s) and retries on a full "
                  "MUL/DIV queue, peak occupancy " + Dec(dynamic.peak_occ[2]) + " of capacity " +
                      Dec(kUnits[2].capacity) + ", every macro retired once with its reference "
                      "value in both modes");
}

void PhaseMatrix(Harness* harness, mosaic::Reporter* reporter) {
  // A matrix in which two units are equally capable and equally loaded: the age
  // key is the only one left, and it must alternate between them. This is the
  // p0 matrix with the LSU widened to take ALU macros as well, which is a
  // legal matrix the policy has to be correct for.
  const uint32_t none[kN] = {0, 0, 0, 0};
  Stim s = BaseStim(true, none);
  s.unit_cap[3] = (1u << CLS_ALU) | (1u << CLS_LOAD) | (1u << CLS_STORE);
  s.req_valid = true;
  s.req_class = CLS_ALU;
  s.req_locality_en = false;
  s.req_age = 1;

  harness->Reset(4);
  harness->Phase("matrix:tie-across-shared-units");
  Observed o = harness->Cycle(s);
  Require(o.grant_valid && o.grant_unit == 0 && o.grant_reason == 5, "matrix:tie-across-shared-units",
          "the three equally loaded ALU-capable units must be decided by age; got unit " +
              Dec(o.grant_unit) + " reason " + Dec(o.grant_reason));
  s.req_age = 2;
  o = harness->Cycle(s);
  Require(o.grant_valid && o.grant_unit == 1 && o.grant_reason == 5, "matrix:tie-across-shared-units",
          "the age key must move to the next unit; got unit " + Dec(o.grant_unit));
  s.req_age = 3;
  o = harness->Cycle(s);
  Require(o.grant_valid && o.grant_unit == 3 && o.grant_reason == 5, "matrix:tie-across-shared-units",
          "the age key must reach the third unit; got unit " + Dec(o.grant_unit));
  s.req_age = 4;
  o = harness->Cycle(s);
  Require(o.grant_valid && o.grant_unit == 0, "matrix:tie-across-shared-units",
          "the age key must wrap back to the first unit; got unit " + Dec(o.grant_unit));

  // A matrix with an empty row: the class has no unit, and the answer is reject
  // in *both* strategies -- a fixed table cannot conjure one either.
  const uint32_t lsu_full[kN] = {0, 0, 0, 4};
  s = BaseStim(false, lsu_full);
  s.unit_cap[3] = 0;
  s.req_valid = true;
  s.req_class = CLS_LOAD;
  s.req_locality_en = false;
  s.req_age = 1;
  s.fixed_unit[CLS_LOAD] = 3;
  harness->Phase("matrix:empty-row-rejects");
  o = harness->Cycle(s);
  Require(!o.grant_valid && o.reject && !o.stall, "matrix:empty-row-rejects",
          "a class with no capable unit must be rejected, not stalled or granted");

  reporter->Check(true, "matrix: with a third capable unit the age key decides and rotates; "
                        "a class whose row is empty is rejected in both strategies");
}

void PhaseRandom(Harness* harness, mosaic::Reporter* reporter, uint64_t seed, int cycles) {
  harness->Reset(4);
  harness->Phase("random");
  mosaic::Rng rng(seed);
  uint64_t grants = 0, stalls = 0, rejects = 0;
  uint32_t reason_seen[6] = {0, 0, 0, 0, 0, 0};
  uint64_t fixed_grants = 0, dynamic_grants = 0;
  uint32_t age = 0;
  for (int i = 0; i < cycles; i++) {
    if ((i & 1u) == 0) age += 1 + rng.Below(2);

    // The matrix is randomised every cycle, so the policy is exercised as a
    // policy and not only as the p0 table. SYSTEM is left unassigned in this
    // soak so that the reject path is reached.
    uint32_t cap[kN];
    for (int u = 0; u < kN; u++) {
      cap[u] = rng.Next() & 0x1fu;  // classes 0..4; bit 5 (SYSTEM) stays clear
    }
    uint32_t occ[kN];
    uint32_t room[kN];
    for (int u = 0; u < kN; u++) {
      occ[u] = rng.Below(kUnits[u].capacity + 2);
      room[u] = (occ[u] < kUnits[u].capacity);
      if (rng.Chance(10)) room[u] = !room[u];  // the module must use the pin it was given
    }
    Stim s = BaseStim(rng.Chance(50), occ);
    for (int u = 0; u < kN; u++) {
      s.unit_cap[u] = cap[u];
      s.unit_room[u] = room[u];
    }
    for (int c = 0; c < kC; c++) s.fixed_unit[c] = rng.Below(kN);
    s.req_valid = rng.Chance(85);
    s.req_class = rng.Below(kC);
    s.req_locality_en = rng.Chance(60);
    s.req_locality = rng.Chance(50);
    s.req_age = age;

    const Observed o = harness->Cycle(s);
    if (!s.req_valid) continue;
    if (o.grant_valid) {
      grants++;
      reason_seen[o.grant_reason & 7u]++;
      if (o.grant_unit < kN) {
        Require(((cap[o.grant_unit] >> s.req_class) & 1u) != 0, "random",
                "cycle " + Dec(i) + ": class " + Dec(s.req_class) + " was granted to unit " +
                    Dec(o.grant_unit) + " whose capability mask is " + Hex64(cap[o.grant_unit]));
        Require(s.unit_room[o.grant_unit], "random",
                "cycle " + Dec(i) + ": a macro was admitted to unit " + Dec(o.grant_unit) +
                    " which reported no room");
      }
      if (s.mode_dyn) dynamic_grants++; else fixed_grants++;
    } else if (o.stall) {
      stalls++;
    } else if (o.reject) {
      rejects++;
    }
  }

  // Every key's decisions must actually have been reached, or the soak is
  // covering less than it claims.
  Require(grants > 0 && stalls > 0 && rejects > 0, "random",
          "the soak did not reach grant, stall and reject: " + Dec(grants) + "/" + Dec(stalls) +
              "/" + Dec(rejects));
  Require(fixed_grants > 0 && dynamic_grants > 0, "random",
          "the soak did not exercise both strategies");
  for (uint32_t reason = 1; reason <= 5; reason++) {
    Require(reason_seen[reason] > 0, "random",
            "the soak never used grant_reason " + Dec(reason));
  }

  reporter->Check(true, "random: " + Dec(cycles) + " seeded cycles with a randomised matrix, "
                        "occupancy and strategy, every output compared every cycle (grants " +
                        Dec(grants) + ", stalls " + Dec(stalls) + ", rejects " + Dec(rejects) +
                        ", reasons 1..5 " + Dec(reason_seen[1]) + "/" + Dec(reason_seen[2]) + "/" +
                        Dec(reason_seen[3]) + "/" + Dec(reason_seen[4]) + "/" +
                        Dec(reason_seen[5]) + ")");
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
  Vmosaic_steering_tb dut;
  Harness harness(&dut, &clk, options.max_cycles);

  std::string detail;
  bool passed = true;
  try {
    // ----------------------------------------------------------- geometry
    harness.Phase("geometry");
    harness.Reset(4);
    harness.Cycle(Stim{});
    const uint32_t units = dut.o_units_o;
    const uint32_t classes = dut.o_classes_o;
    const uint32_t age_w = dut.o_age_w_o;
    const uint32_t unit_w = dut.o_unit_w_o;
    const uint32_t occ_w = dut.o_occ_w_o;
    Require(units == kN && classes == kC && age_w == 16 && unit_w == 2 && occ_w == 4, "geometry",
            "the DUT's geometry is " + Dec(units) + " units, " + Dec(classes) + " classes, age " +
                Dec(age_w) + "b, unit " + Dec(unit_w) + "b, occupancy " + Dec(occ_w) +
                "b; this driver drives 4/6/16/2/4");
    reporter.Check(true, "geometry: the DUT reports the geometry this driver drives (" +
                             Dec(units) + " units, " + Dec(classes) + " classes, age " +
                             Dec(age_w) + "b, occupancy " + Dec(occ_w) + "b)");

    // ------------------------------------------------- directed mechanisms
    PhaseDirected(&harness, &reporter);

    // -------------------------------------------------- the experiment
    PhaseFixedVsDynamic(&harness, &reporter, options.seed);

    // --------------------------------------------------------- saturation
    PhaseSaturation(&harness, &reporter, options.seed);

    // ------------------------------------------------------------ matrices
    PhaseMatrix(&harness, &reporter);

    // -------------------------------------------------------------- soak
    PhaseRandom(&harness, &reporter, options.seed, 1200);

    // -------------------------------------------------------- determinism
    mosaic::Rng rng(options.seed ^ 0x5eedull);
    const std::vector<Uop> program = GenerateProgram(&rng, 256);
    RunOptions opts;
    opts.trace_decisions = true;
    const RunResult first = RunProgram(&harness, program, options.seed, true, "determinism-a", opts);
    const RunResult second = RunProgram(&harness, program, options.seed, true, "determinism-b", opts);
    CompareStreams(first, second, "determinism", "run A", "run B");
    Require(first.decisions.size() == second.decisions.size(), "determinism",
            "the two runs made a different number of decisions");
    for (size_t i = 0; i < first.decisions.size(); i++) {
      Require(first.decisions[i].index == second.decisions[i].index &&
                  first.decisions[i].unit == second.decisions[i].unit &&
                  first.decisions[i].reason == second.decisions[i].reason &&
                  first.decisions[i].cycle == second.decisions[i].cycle,
              "determinism",
              "decision " + Dec(i) + " differs: run A (macro " + Dec(first.decisions[i].index) +
                  ", unit " + Dec(first.decisions[i].unit) + ", reason " +
                  Dec(first.decisions[i].reason) + ", cycle " + Dec(first.decisions[i].cycle) +
                  "), run B (macro " + Dec(second.decisions[i].index) + ", unit " +
                  Dec(second.decisions[i].unit) + ", reason " + Dec(second.decisions[i].reason) +
                  ", cycle " + Dec(second.decisions[i].cycle) + ")");
    }
    reporter.Check(true, "determinism: the same program and seed reproduce " +
                             Dec(first.decisions.size()) + " decisions and the signature " +
                             Hex64(first.signature) + " exactly");

    detail = "steering contract holds: " + Dec(harness.comparisons()) +
             " per-cycle comparisons over " + Dec(harness.cycles()) +
             " cycles; fixed and dynamic agree on the architectural result; seed " +
             Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated after " + Dec(harness.comparisons()) +
             " comparisons: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
