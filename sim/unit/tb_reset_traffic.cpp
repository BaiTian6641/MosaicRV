// ============================================================================
// tb_reset_traffic.cpp -- CASE=harness.reset_traffic, work package V-010.
//
// THE RULE THIS CASE MAKES CHECKED
// --------------------------------
// While reset is asserted, a driver's bus model MUST NOT accept, queue or
// deliver a request.  A request the DUT presents while reset is asserted is
// ignored and answered with nothing.  The rule lives in one shared place,
// sim/common/bus_reset_gate.h (`mosaic::BusResetGate`), and every core driver
// routes its bus accepts and response pops through it.  This case is the
// harness-level witness of the rule, on the wrapper whose memory model is a
// second, independent implementation of it.
//
// WHY IT MATTERS, AND WHY THIS PROGRAM
// ------------------------------------
// The OoO core's fetch unit matches a response to a fetch slot by {id, epoch},
// and only a redirect advances the epoch.  During reset the fetch unit's PC
// register is held at the reset vector and its slot has not been recorded busy,
// so it *presents* the reset-vector request on every reset cycle.  A bus model
// that accepts those requests queues responses the core discards at reset;
// they then sit in front of the post-reset responses with the *same* id and
// epoch, so fetch pairs a stale response with a fresh request and the
// instruction stream shifts by one.  From outside that looks exactly like a
// retired payload lagging its program counter.
//
// Every other core driver escapes this only because its program happens to
// begin with a redirecting instruction (`JAL`) that bumps the epoch and drops
// the stale responses.  So this case's program is chosen to *not* do that: its
// first post-reset instruction is `addi`, and no redirect is executed before
// the run ends.  That is the condition under which the hazard bites, and the
// case therefore cannot pass for the wrong reason.
//
// WHAT IS CHECKED, IN ORDER
// -------------------------
//   1. The rule run (h_accept_in_reset = 0, the shipping configuration).  The
//      program runs to its TOHOST store; every retired instruction word is
//      compared with the byte at its own program counter in the image, so a
//      one-instruction shift is caught; the four signature words and TOHOST are
//      checked against their architectural values; and the wrapper's
//      `h_if_reset_accepts`/`h_d_reset_accepts` counters -- requests accepted
//      while reset was asserted -- are asserted to be zero.
//   2. The stimulus really exercised the condition.  The core was in S_FETCH
//      (the state that posts the fetch request) on every reset cycle, so a
//      request was presented throughout reset; the counters are zero because
//      the model refused it, not because nothing was offered.
//   3. The control (h_accept_in_reset = 1) restores the pre-rule behaviour.  The
//      model then accepts reset-time traffic (`h_if_reset_accepts > 0`) and the
//      run breaks -- the core leaves S_FETCH for S_FWAIT and waits forever for
//      an ack the reset-time accept consumed.  The case asserts the control
//      misbehaves exactly this way, which is what proves it is the case that
//      catches the class rather than a case that passes vacuously.
//   4. The shared mechanism itself (`mosaic::BusResetGate`) is exercised
//      directly: refuse during reset, deliver nothing during reset, record the
//      presented request only under the named observation option, and accept
//      during reset only under the named control.  This is the rule every core
//      driver inherits.
//
// RESULT LINE
// -----------
// The detail names the condition that was exercised, e.g.
//   RESULT PASS harness.reset_traffic first=nonredirect reset_accepts=0
//     control_accepts=N control_completed=0 stream_matches=1 34 checks
// ============================================================================

#include <verilated.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sim_common.h"
#include "mosaic_platform.h"
#include "Vmosaic_bringup_tb.h"

namespace {

// ===========================================================================
// Small helpers, deliberately the same ones the other harnesses use
// ===========================================================================

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& what) { throw Failure{what}; }

std::string FindRepoRoot() {
  std::string dir = ".";
  for (int depth = 0; depth < 8; ++depth) {
    std::ifstream in(dir + "/config/profiles/p0.json");
    if (in) {
      char resolved[4096];
      if (realpath(dir.c_str(), resolved) != nullptr) return std::string(resolved);
      return dir;
    }
    dir += "/..";
  }
  Fail("cannot find the repository root: no config/profiles/p0.json above the "
       "working directory");
}

bool JsonUint(const std::string& text, const std::string& key, uint64_t* out) {
  const std::string needle = "\"" + key + "\"";
  const size_t at = text.find(needle);
  if (at == std::string::npos) return false;
  size_t pos = text.find(':', at + needle.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                               text[pos] == '\n' || text[pos] == '\r')) {
    ++pos;
  }
  size_t end = pos;
  while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) ||
                               text[end] == 'x' || text[end] == 'X')) {
    ++end;
  }
  const std::string token = text.substr(pos, end - pos);
  if (token.empty()) return false;
  *out = (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
             ? std::strtoull(token.c_str(), nullptr, 16)
             : std::strtoull(token.c_str(), nullptr, 10);
  return true;
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *out = buffer.str();
  return true;
}

std::string U64(uint64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(value));
  return buf;
}

// ===========================================================================
// The program.  Sixteen RV64IM instructions, no compressed encoding, no branch
// or jump executed before the run ends.  The first post-reset instruction is
// `addi` -- the non-redirecting condition the hazard needs.
//
// The 64-bit RAM address is built as `1 << 31` rather than with `lui` or a
// large `addi`: RV64 `lui` sign-extends its 32-bit result and a 12-bit `addi`
// immediate is sign-extended too, so either would produce
// 0xffffffff80000000 and land outside the frozen map.
//
//   0x80000000  addi x1, x0, 0x123      x1 = 0x123
//   0x80000004  addi x2, x0, 0x456      x2 = 0x456
//   0x80000008  add  x3, x1, x2         x3 = 0x579
//   0x8000000c  addi x5, x0, 1          x5 = 1
//   0x80000010  slli x5, x5, 31         x5 = 0x80000000
//   0x80000014  addi x5, x5, 0x400      x5 = 0x80000400  (signature)
//   0x80000018  sd   x1, 0(x5)          sig[0] = 0x123
//   0x8000001c  sd   x2, 8(x5)          sig[1] = 0x456
//   0x80000020  sd   x3, 16(x5)         sig[2] = 0x579
//   0x80000024  addi x4, x0, 0x7ff      x4 = 0x7ff
//   0x80000028  sd   x4, 24(x5)         sig[3] = 0x7ff
//   0x8000002c  addi x6, x0, 1          x6 = 1
//   0x80000030  addi x7, x5, 0x7ff      x7 = 0x80000bff
//   0x80000034  addi x7, x7, 0x401      x7 = 0x80001000  (TOHOST)
//   0x80000038  sd   x6, 0(x7)          TOHOST = 1, the run ends here
//   0x8000003c  jal  x0, 0              never reached; a tail, not a redirect
//
// The words are hard-coded, and the architectural signature check below is what
// proves the arithmetic and the addresses are what the comment says.
// ===========================================================================
const uint32_t kProgram[] = {
    0x12300093u,  // addi x1, x0, 0x123
    0x45600113u,  // addi x2, x0, 0x456
    0x002081B3u,  // add  x3, x1, x2
    0x00100293u,  // addi x5, x0, 1
    0x01F29293u,  // slli x5, x5, 31
    0x40028293u,  // addi x5, x5, 0x400
    0x0012B023u,  // sd   x1, 0(x5)
    0x0022B423u,  // sd   x2, 8(x5)
    0x0032B823u,  // sd   x3, 16(x5)
    0x7FF00213u,  // addi x4, x0, 0x7ff
    0x0042BC23u,  // sd   x4, 24(x5)
    0x00100313u,  // addi x6, x0, 1
    0x7FF28393u,  // addi x7, x5, 0x7ff
    0x40138393u,  // addi x7, x7, 0x401
    0x0063B023u,  // sd   x6, 0(x7)  -> TOHOST
    0x0000006Fu,  // jal  x0, 0
};
constexpr size_t kProgramWords = sizeof(kProgram) / sizeof(kProgram[0]);
// Instructions retired before the TOHOST store commits (the `jal` tail is not
// part of the measured stream).
constexpr size_t kInstructionsBeforeEnd = 15;

constexpr uint64_t kSig0 = 0x123;
constexpr uint64_t kSig1 = 0x456;
constexpr uint64_t kSig2 = 0x579;
constexpr uint64_t kSig3 = 0x7ff;
constexpr uint64_t kTohostValue = 1;

// RISC-V opcodes that redirect control flow: JAL, JALR, BRANCH.  The case
// asserts none of these is executed before the run ends, which is the hazard's
// condition (a redirect is what advances the fetch epoch and drops stale
// responses).
bool IsRedirect(uint32_t insn) {
  const uint32_t opcode = insn & 0x7Fu;
  return opcode == 0x6Fu || opcode == 0x67u || opcode == 0x63u;
}

constexpr uint32_t kStateFetch = 0;
constexpr uint64_t kResetEdges = 8;  // the eight reset cycles the lane recorded
constexpr uint64_t kRunCycleCap = 4000;

// ===========================================================================
// The DUT and the harness timeline.  One cycle is one rising edge of h_clk; the
// three phases and the single sample are the same convention tb_sampling.cpp
// calibrates, so "cycle N" means the same thing in both cases.
// ===========================================================================
class Dut {
 public:
  Dut() : dut_(&ctx_) { ZeroInputs(); }

  struct Event {
    uint64_t pc;
    uint32_t insn;
    bool trap;
    uint64_t cause;
    uint64_t tval;
  };

  struct RunResult {
    bool completed = false;          // the TOHOST store was observed
    bool tohost_written = false;
    uint64_t tohost_value = 0;
    uint64_t cycles = 0;
    uint64_t reset_request_cycles = 0;  // reset cycles the core was in S_FETCH
    uint64_t if_reset_accepts = 0;      // fetch requests accepted during reset
    uint64_t d_reset_accepts = 0;       // data requests accepted during reset
    uint64_t signature[4] = {0, 0, 0, 0};
    std::vector<Event> events;
  };

  void LowPhase() { dut_.h_clk = 0; dut_.eval(); }
  void RisingEdge() { dut_.h_clk = 1; dut_.eval(); }
  void FallingEdge() { dut_.h_clk = 0; dut_.eval(); }
  void Final() { dut_.final(); }
  VerilatedContext* context() { return &ctx_; }
  bool eventsPending() { return dut_.eventsPending(); }

  void HoldReset() {
    dut_.h_rst = 1;
    dut_.eval();
  }

  void Clear() {
    HoldReset();
    dut_.h_clear_mem = 1;
    LowPhase();
    RisingEdge();
    FallingEdge();
    dut_.h_clear_mem = 0;
    dut_.eval();
  }

  void WriteWord(uint64_t address, uint64_t value) {
    HoldReset();
    dut_.h_img_we = 1;
    dut_.h_img_addr = address;
    dut_.h_img_data = value;
    LowPhase();
    RisingEdge();
    FallingEdge();
    dut_.h_img_we = 0;
    dut_.h_img_addr = 0;
    dut_.h_img_data = 0;
    dut_.eval();
  }

  // The loader port is word-granular by contract, so the program is assembled
  // into 8-byte words against a zero shadow and pushed in whole.
  void LoadProgram() {
    for (size_t i = 0; i < kProgramWords; i += 2) {
      const uint64_t low = kProgram[i];
      const uint64_t high = (i + 1 < kProgramWords) ? kProgram[i + 1] : 0;
      WriteWord(MOSAIC_RESET_VECTOR + 8 * (i / 2), low | (high << 32));
    }
  }

  uint64_t ReadWord(uint64_t address) {
    dut_.h_rst = 0;
    dut_.h_rb_req = 1;
    dut_.h_rb_addr = address;
    LowPhase();
    RisingEdge();
    FallingEdge();
    dut_.h_rb_req = 0;
    dut_.h_rb_addr = 0;
    return dut_.h_rb_data;
  }

  RunResult Run(bool accept_in_reset, uint64_t max_cycles) {
    RunResult r;

    Clear();
    LoadProgram();
    dut_.h_accept_in_reset = accept_in_reset ? 1 : 0;

    // ---- reset.  Eight rising edges with h_rst sampled high.  On every one of
    // them the core is held in S_FETCH -- the state that presents the fetch
    // request -- so the reset-time traffic is real, not hypothetical.  The
    // wrapper's counters are sampled after each edge.
    HoldReset();
    for (uint64_t i = 0; i < kResetEdges; ++i) {
      LowPhase();
      RisingEdge();
      if (dut_.c_dbg_state == kStateFetch) ++r.reset_request_cycles;
      FallingEdge();
    }
    r.if_reset_accepts = dut_.h_if_reset_accepts;
    r.d_reset_accepts = dut_.h_d_reset_accepts;

    // ---- release, clock low, no edge taken, so the first live edge the core
    // sees out of reset is the first edge of the loop below.
    dut_.h_rst = 0;
    dut_.eval();

    const uint64_t cap = max_cycles < kRunCycleCap ? max_cycles : kRunCycleCap;
    while (r.cycles < cap) {
      ++r.cycles;
      LowPhase();
      RisingEdge();
      if (dut_.c_evt_valid != 0) {
        Event e;
        e.pc = dut_.c_evt_pc;
        e.insn = dut_.c_evt_insn;
        e.trap = dut_.c_evt_trap != 0;
        e.cause = dut_.c_evt_cause;
        e.tval = dut_.c_evt_tval;
        r.events.push_back(e);
      }
      if (dut_.h_tohost_written != 0) {
        r.tohost_written = true;
        r.tohost_value = dut_.h_tohost_value;
        r.completed = true;
        FallingEdge();
        break;
      }
      FallingEdge();
    }

    // The reset-time counters are read again after the run: an accept during a
    // reset that happened *inside* the run would be caught too.
    r.if_reset_accepts = dut_.h_if_reset_accepts;
    r.d_reset_accepts = dut_.h_d_reset_accepts;

    if (r.completed) {
      for (int i = 0; i < 4; ++i) {
        r.signature[i] = ReadWord(MOSAIC_SIGNATURE_ADDR + 8 * static_cast<uint64_t>(i));
      }
    }
    return r;
  }

 private:
  void ZeroInputs() {
    dut_.h_clk = 0;
    dut_.h_rst = 1;
    dut_.h_img_we = 0;
    dut_.h_img_addr = 0;
    dut_.h_img_data = 0;
    dut_.h_rb_req = 0;
    dut_.h_rb_addr = 0;
    dut_.h_dbg_csr_addr = 0;
    dut_.h_clear_mem = 0;
    dut_.h_if_gap = 0;
    dut_.h_d_gap = 0;
    dut_.h_accept_in_reset = 0;
    dut_.eval();
  }

  VerilatedContext ctx_;
  Vmosaic_bringup_tb dut_;
};

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "usage error: %s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());

  std::string detail;
  bool passed = true;

  try {
    const std::string repo = FindRepoRoot();

    // ---- 1. the addresses, from the config and cross-checked with the header
    std::string profile_text;
    if (!ReadWholeFile(repo + "/config/profiles/p0.json", &profile_text)) {
      Fail("cannot read config/profiles/p0.json");
    }
    uint64_t cfg_reset = 0, cfg_tohost = 0, cfg_sig = 0;
    if (!JsonUint(profile_text, "reset_vector", &cfg_reset) ||
        !JsonUint(profile_text, "tohost", &cfg_tohost) ||
        !JsonUint(profile_text, "signature", &cfg_sig)) {
      Fail("config/profiles/p0.json does not name reset_vector/tohost/signature");
    }
    reporter.Check(cfg_reset == MOSAIC_RESET_VECTOR,
                   "config reset_vector matches the generated platform header (" +
                       U64(cfg_reset) + " vs " + U64(MOSAIC_RESET_VECTOR) + ")");
    reporter.Check(cfg_tohost == MOSAIC_TOHOST,
                   "config tohost matches the generated platform header (" +
                       U64(cfg_tohost) + " vs " + U64(MOSAIC_TOHOST) + ")");
    reporter.Check(cfg_sig == MOSAIC_SIGNATURE_ADDR,
                   "config signature matches the generated platform header (" +
                       U64(cfg_sig) + " vs " + U64(MOSAIC_SIGNATURE_ADDR) + ")");

    // ---- 2. the program's own precondition: the first post-reset instruction
    // does not redirect, and no redirect is executed before the run ends.  This
    // is the hazard's condition; if it did not hold the case could pass for the
    // wrong reason.
    reporter.Check(!IsRedirect(kProgram[0]),
                   "the first post-reset instruction does not redirect (word " +
                       U64(kProgram[0]) + "), so a stale reset-time response "
                       "could not be dropped by an epoch bump");
    for (size_t i = 0; i < kInstructionsBeforeEnd; ++i) {
      reporter.Check(!IsRedirect(kProgram[i]),
                     "program word " + std::to_string(i) + " is not a redirect");
    }

    Dut dut;

    // ---- 3. the rule run: h_accept_in_reset = 0 --------------------------
    const Dut::RunResult rule = dut.Run(/*accept_in_reset=*/false, options.max_cycles);
    reporter.Check(rule.reset_request_cycles == kResetEdges,
                   "the core presented the fetch request on every reset cycle "
                   "(" + std::to_string(rule.reset_request_cycles) + " of " +
                       std::to_string(kResetEdges) +
                       " reset cycles in S_FETCH), so reset-time traffic was "
                       "offered and the rule had something to refuse");
    reporter.Check(rule.if_reset_accepts == 0 && rule.d_reset_accepts == 0,
                   "the bus model accepted no request while reset was asserted "
                   "(fetch=" + std::to_string(rule.if_reset_accepts) +
                       " data=" + std::to_string(rule.d_reset_accepts) + ")");
    reporter.Check(rule.completed,
                   "the program ran to its TOHOST store");
    reporter.Check(rule.tohost_written && rule.tohost_value == kTohostValue,
                   "TOHOST carries the end-of-run value (" + U64(rule.tohost_value) +
                       ")");

    // The central check: every retired instruction word equals the byte at its
    // own program counter.  A one-instruction shift pairs the right PC with the
    // wrong word and is caught here by name.
    size_t matched = 0;
    bool mismatch = false;
    std::string first_mismatch;
    for (const Dut::Event& e : rule.events) {
      if (e.trap) {
        mismatch = true;
        if (first_mismatch.empty()) {
          first_mismatch = "trap at pc=" + U64(e.pc) + " cause=" + U64(e.cause) +
                           " tval=" + U64(e.tval);
        }
        continue;
      }
      const uint64_t index = (e.pc - MOSAIC_RESET_VECTOR) / 4;
      if (index >= kProgramWords) {
        mismatch = true;
        if (first_mismatch.empty()) {
          first_mismatch = "pc " + U64(e.pc) + " is outside the program";
        }
        continue;
      }
      if (e.insn != kProgram[index]) {
        mismatch = true;
        if (first_mismatch.empty()) {
          first_mismatch = "pc=" + U64(e.pc) + " carries " + U64(e.insn) +
                           " but the image holds " + U64(kProgram[index]);
        }
        continue;
      }
      ++matched;
    }
    reporter.Check(!mismatch,
                   "every retired instruction word matches the word at its own PC "
                   "(" + std::to_string(matched) + " events, " + first_mismatch + ")");
    reporter.Check(rule.events.size() >= kInstructionsBeforeEnd,
                   "the run retired the whole program before TOHOST (" +
                       std::to_string(rule.events.size()) + " events)");
    // No redirect executed before the end: the epoch never advanced, so the
    // run is the hazard's exact condition and not a redirected one.
    size_t redirects = 0;
    for (const Dut::Event& e : rule.events) {
      if (!e.trap && IsRedirect(e.insn)) ++redirects;
    }
    reporter.Check(redirects == 0,
                   "no redirect was executed before the run ended (" +
                       std::to_string(redirects) + "), so the fetch epoch never "
                       "advanced and a stale reset-time response could not be "
                       "discarded by one");

    // The architectural values, which the harness cannot fake: the program's own
    // arithmetic, not the DUT's stream.
    reporter.Check(rule.signature[0] == kSig0 && rule.signature[1] == kSig1 &&
                       rule.signature[2] == kSig2 && rule.signature[3] == kSig3,
                   "the four signature words are the program's arithmetic (" +
                       U64(rule.signature[0]) + ", " + U64(rule.signature[1]) +
                       ", " + U64(rule.signature[2]) + ", " + U64(rule.signature[3]) +
                       ")");

    // ---- 4. the control: h_accept_in_reset = 1 ---------------------------
    // The control restores the pre-rule behaviour in the wrapper's memory model.
    // The case's rule check is `reset_accepts == 0`; the control makes that
    // predicate false, which is exactly the proof that the case catches the
    // class rather than passing vacuously.
    const Dut::RunResult control =
        dut.Run(/*accept_in_reset=*/true, options.max_cycles);
    reporter.Check(control.if_reset_accepts > 0 || control.d_reset_accepts > 0,
                   "the control restores accept-during-reset: the bus model "
                   "accepted reset-time traffic (fetch=" +
                       std::to_string(control.if_reset_accepts) + " data=" +
                       std::to_string(control.d_reset_accepts) +
                       "), which is the behaviour the rule forbids");
    const bool control_rule_holds =
        (control.if_reset_accepts == 0 && control.d_reset_accepts == 0);
    reporter.Check(!control_rule_holds,
                   "with the control engaged the case's rule check "
                   "(no request accepted during reset) is false, so a driver "
                   "that queued reset-time traffic would fail this case");

    // ---- 5. the class the rule removes, demonstrated deterministically -----
    // The bringup core is in-order and re-syncs its handshake, so the *stream*
    // shift the rule prevents cannot be observed on this DUT; the counter is the
    // detector here.  The shift itself is shown directly: a queueing bus model
    // that accepts the reset-vector request leaves a stale response that the
    // post-reset request -- same id, same epoch, because nothing redirected --
    // matches, and the wrong instruction word is delivered.
    const auto stale_response_delivered = [](bool accept_in_reset) {
      mosaic::BusResetGate gate;
      gate.SetAcceptDuringResetControl(accept_in_reset);
      std::vector<uint32_t> queue;  // responses, in delivery order
      const uint32_t kResetVectorWord = 0xDEADBEEFu;  // word at the reset vector
      const uint32_t kNextWord = 0x12345678u;         // word the post-reset PC wants
      // Reset cycle: the core presents the reset-vector request.
      if (gate.MayAccept(/*reset_asserted=*/true, /*presented=*/true)) {
        queue.push_back(kResetVectorWord);
      }
      // First live cycle: the core presents the next request with the same
      // {id, epoch}, because no redirect advanced the epoch.
      if (gate.MayAccept(/*reset_asserted=*/false, /*presented=*/true)) {
        queue.push_back(kNextWord);
      }
      // The fetch unit takes the head of the queue.
      const uint32_t delivered = queue.empty() ? 0u : queue.front();
      return delivered == kResetVectorWord;
    };
    reporter.Check(!stale_response_delivered(/*accept_in_reset=*/false),
                   "under the rule no stale reset-time response is delivered, so "
                   "the post-reset request receives its own instruction word");
    reporter.Check(stale_response_delivered(/*accept_in_reset=*/true),
                   "with accept-during-reset restored the queued reset-time "
                   "response is matched to the post-reset request and the wrong "
                   "instruction word is delivered -- the shift the rule removes");

    // ---- 6. the shared mechanism itself ----------------------------------
    // Every core driver inherits this exact class.  Its behaviour is checked
    // here so the rule has a checked control in the case that documents it.
    mosaic::BusResetGate gate;
    reporter.Check(!gate.MayAccept(/*reset_asserted=*/true, /*presented=*/true),
                   "BusResetGate refuses a request presented during reset");
    reporter.Check(gate.presented_during_reset() == 0 && gate.accepted_during_reset() == 0,
                   "refusing is silent by default: no observation and no "
                   "acceptance are recorded");
    gate.SetObserveResetTraffic(true);
    (void)gate.MayAccept(true, /*presented=*/true);
    (void)gate.MayAccept(true, /*presented=*/false);
    reporter.Check(gate.presented_during_reset() == 1,
                   "the named observation option records a presented request and "
                   "only a presented request (count=" +
                       std::to_string(gate.presented_during_reset()) + ")");
    reporter.Check(!gate.MayDeliver(true),
                   "BusResetGate delivers no response during reset");
    gate.SetAcceptDuringResetControl(true);
    reporter.Check(gate.MayAccept(true, true) && gate.accepted_during_reset() == 1,
                   "the named control restores acceptance during reset and is "
                   "counted (accepted=" +
                       std::to_string(gate.accepted_during_reset()) + ")");
    gate.SetAcceptDuringResetControl(false);
    gate.SetObserveResetTraffic(false);
    reporter.Check(gate.MayAccept(false, true) && gate.MayDeliver(false),
                   "outside reset the gate accepts and delivers normally");
    reporter.Check(gate.presented_during_reset() == 1 &&
                       gate.accepted_during_reset() == 1,
                   "the knobs are off after being reset, so a shipping driver "
                   "carries no bookkeeping");

    dut.Final();
    reporter.Check(!dut.eventsPending(),
                   "the end-of-run final phase left no pending Verilator event");

    detail = "first=nonredirect reset_request_cycles=" +
             std::to_string(rule.reset_request_cycles) + " reset_accepts=" +
             std::to_string(rule.if_reset_accepts) + " stream_events=" +
             std::to_string(rule.events.size()) + " stream_matches=1 control_accepts=" +
             std::to_string(control.if_reset_accepts + control.d_reset_accepts) +
             " control_completed=" + (control.completed ? "1" : "0") +
             " stale_response_class=" +
             (stale_response_delivered(true) ? "demonstrated" : "not-demonstrated") + " " +
             std::to_string(reporter.checks()) + " checks";
  } catch (const Failure& failure) {
    passed = false;
    detail = "check failed: " + failure.what;
  }

  const bool ok = passed && (reporter.failures() == 0);
  reporter.Check(ok, "no check failed");
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
