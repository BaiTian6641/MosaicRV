// ============================================================================
// tb_sampling.cpp -- CASE=harness.sampling_calibration, work package V-010
// (docs/validation-plan.md section 5 V-010, "校准 Verilator 时钟与采样循环").
//
// WHAT THIS CASE IS
// -----------------
// It is a test of the *harness*, not of the core.  Every number this project
// records about a run -- retire counts, event counts, per-instruction
// comparisons, coverage bins -- is produced by a C++ loop that drives a
// Verilator model one phase at a time and decides, once per cycle, whether an
// architectural event happened.  If that loop can sample the same event twice,
// or miss one, then every count the project has recorded is suspect.  The
// calibration card names exactly that: "the unit of integer time is fixed;
// every accept and every retire corresponds one-to-one with an RTL event; the
// end-of-run final assertion actually runs."
//
// So the case does three things against the real DUT:
//
//   1. It fixes and exercises the timeline convention.  One cycle is one rising
//      edge of h_clk.  Inputs are driven in the low phase and evaluated, the
//      rising edge is evaluated, the settled state is sampled exactly once, and
//      the falling edge is evaluated.  There is no second eval() per phase in
//      the shipping build and `--timing` is not enabled, so Verilator's own
//      integer time never advances and the harness's integer cycle count is the
//      only notion of time in the run.
//
//   2. It applies *item-by-item backpressure* to the memory interface and
//      checks, cycle by cycle, that every handshake is observed exactly once.
//      The DUT wrapper (sim/tb/mosaic_bringup_tb.sv) gained two harness inputs,
//      `h_if_gap` and `h_d_gap`, which add that many cycles of response
//      latency to the fetch and data ports.  They are zero in every other
//      driver and the zero case is the original model statement for statement;
//      this case is the only one that drives them, and it picks the latency for
//      the N-th handshake individually from a published pattern, so stalls are
//      real and vary item by item.
//
//   3. It compares three tallies that come from three different places:
//
//        (a) the DUT's own RTL event stream, recorded by the event tap, one
//            record per acknowledged retire -- this is the number the project
//            uses everywhere;
//        (b) the harness's own count of handshakes, accumulated from the
//            settled sample of the port signals: the fetch ack level, the data
//            ack level (split into reads and writes by the request the ack
//            answers), and the retire pulse;
//        (c) the architectural expectation, which is *not* read from the DUT:
//            the independent RV64IM reference model that core.bringup_vs_reference
//            uses is extracted from sim/unit/tb_bringup.cpp at run time (so the
//            two cases cannot drift apart) and executed on this case's own
//            fixture image.  Its `events` count is the number of instructions
//            the program must retire; its own instruction stream is then
//            decoded with the two RISC-V opcodes for LOAD and STORE to get the
//            number of memory operations the program must perform.
//
//      The three must agree exactly: records == reference instructions ==
//      fetch accepts, and data accepts == reference loads + reference stores.
//
// WHERE EACH EXPECTATION COMES FROM, AND WHAT IS NOT READ
// ------------------------------------------------------
//   * Addresses (reset vector, TOHOST, signature) come from
//     config/profiles/p0.json through the generated build/p0/sim/mosaic_platform.h,
//     and the JSON is re-read and cross-checked against that header at run time.
//   * The fixture's bytes are embedded and pinned by size and by an FNV-1a-64
//     hash, the same way sim/unit/tb_reset.cpp pins its fixture.  Changing the
//     program without changing the hash fails the case by name.
//   * The instruction count, the load count and the store count come from the
//     independent reference model (see above), never from the DUT's own stream.
//     The DUT's stream is the thing under test; the reference is what it is
//     compared against.
//   * The handshake counts come from the harness's sampling loop alone.  They
//     are the subject of the calibration, so nothing in this file derives them
//     from the DUT's event stream.
//   * The relation "one fetch per instruction" is the core's *published*
//     interface contract (mosaic_bringup_core.sv: one outstanding request per
//     port, one instruction in flight, the core accepts nothing new until the
//     matching ack).  It is a contract, not an observation, and it is stated
//     here so the report can say so.
//
// WHAT WOULD MAKE THIS CASE PASS VACUOUSLY, AND WHY IT CANNOT
// ---------------------------------------------------------
//   * A dead machine that retires nothing would make "records == fetches" true
//     by both being zero, so the counts are checked against the reference's
//     non-zero instruction count and against the fixture's own TOHOST write.
//   * Backpressure that is not real would make "one ack per request" trivially
//     true, so the case asserts that the backpressured run takes strictly more
//     cycles and strictly more wait-cycles than the zero-latency run, and that
//     the architectural stream is unchanged by the extra latency.
//   * A detector that never fires is not a detector, so four `-D`-gated
//     mutants are shipped (three in this driver, one in the DUT wrapper) and
//     tools/run_sampling_controls.py builds each from an empty directory and
//     requires it to exit 1 with its named check.  See
//     results/reports/V-010-sampling.md.
// ============================================================================

#include <verilated.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sim_common.h"
#include "event_tap.h"
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

std::string Base64Decode(const std::string& text) {
  static const std::string kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int bits = 0;
  unsigned acc = 0;
  for (const char c : text) {
    if (c == '=') break;
    const size_t index = kAlphabet.find(c);
    if (index == std::string::npos) continue;
    acc = (acc << 6) | static_cast<unsigned>(index);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

// FNV-1a over the exact fixture bytes.  A fixture that changes without this
// constant changing fails the case by name, the same way the other harnesses
// pin their literals.
uint64_t Fnv1a64(const std::string& bytes) {
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= UINT64_C(0x100000001b3);
  }
  return hash;
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *out = buffer.str();
  return true;
}

bool InMappedRange(uint64_t address) {
  struct { uint64_t base, size; } regions[] = {
      {MOSAIC_BOOT_ROM_BASE, MOSAIC_BOOT_ROM_SIZE},
      {MOSAIC_UART_BASE, MOSAIC_UART_SIZE},
      {MOSAIC_TEST_HARNESS_BASE, MOSAIC_TEST_HARNESS_SIZE},
      {MOSAIC_CLINT_BASE, MOSAIC_CLINT_SIZE},
      {MOSAIC_RAM_BASE, MOSAIC_RAM_SIZE},
  };
  for (const auto& region : regions) {
    if (address >= region.base && address < region.base + region.size) return true;
  }
  return false;
}

// ===========================================================================
// The fixture
// ===========================================================================

// sim/unit/sampling_program.S, assembled and objcopy'd to a flat binary.  The
// recipe, the disassembly and the SHA-256 are in
// results/reports/V-010-sampling.md.
const char* kFixtureBase64 =
    "twIEAJuCEgCTktIAGwMQABMT8wETAwNAtwMIAJuDEwCTk8MAEw5AALeYRACbiNiMk5joAJOIWEWT"
    "mMgAk4h4ZpOYyACTiIh4EwgAACOwEgEDtQIAMwioAJOCggATDv7/4xYO/iMwAwEjsAMBbwAAAA==";
constexpr size_t kFixtureBytes = 112;
constexpr uint64_t kFixtureFnv1a = UINT64_C(0x23a629ada6b49194);

// The two RISC-V opcodes the reference model's instruction stream is decoded
// with.  They come from the ISA manual's instruction format table, not from the
// design under test.
constexpr uint32_t kOpcodeLoad  = 0x03;
constexpr uint32_t kOpcodeStore = 0x23;
constexpr uint32_t kOpcodeSystem = 0x73;  // ECALL/EBREAK/CSR, all of them traps or
                                          // CSR accesses the fixture must not use
constexpr uint32_t kOpcodeJal   = 0x6F;

// The harness bounds a run at this many cycles regardless of the registered
// budget: the fixture is a few tens of instructions, so a run that needs more
// than this has stopped making progress and is a failure to be reported, not a
// run to be truncated silently.  Hitting the bound is an explicit check
// failure.
constexpr uint64_t kRunCycleCap = 20000;

// The core's state encoding, from mosaic_bringup_core.sv `state_e`.
constexpr uint32_t kStateFetch = 0;
constexpr uint32_t kStateFwait = 1;
constexpr uint32_t kStateDwait = 4;

// ===========================================================================
// Item-by-item backpressure
// ===========================================================================
//
// The N-th handshake on a port is served with the N-th gap in that port's
// pattern.  Indexing by the handshake ordinal (not by the cycle) is what makes
// the stall *item by item*: successive requests deliberately get different
// latencies, and the pattern is fixed before the run, so the run is
// reproducible from the seed-free plan alone.
struct BackpressurePlan {
  const char* name = "none";
  const uint8_t* fetch_gaps = nullptr;
  const uint8_t* data_gaps = nullptr;
  size_t fetch_len = 0;
  size_t data_len = 0;

  uint8_t fetch_gap(uint64_t handshake) const {
    if (fetch_gaps == nullptr || fetch_len == 0) return 0;
    return fetch_gaps[handshake % fetch_len];
  }
  uint8_t data_gap(uint64_t handshake) const {
    if (data_gaps == nullptr || data_len == 0) return 0;
    return data_gaps[handshake % data_len];
  }
};

const uint8_t kPatternBFetch[] = {0, 1, 2, 3};
const uint8_t kPatternBData[] = {0, 2, 1, 3};
const uint8_t kPatternCFetch[] = {3, 1, 0, 2};
const uint8_t kPatternCData[] = {1, 3, 2, 0};

// ===========================================================================
// The DUT
// ===========================================================================

struct Annotation {
  // The cycle each record was observed in, kept beside it so a failure can name
  // the cycle as well as the instruction.
  uint64_t cycle = 0;
};

struct RunResult {
  std::string outcome;  // "retired_tohost", "cycle_limit", "pc_escaped"
  const char* plan = "none";

  uint64_t cycles = 0;       // rising edges driven in the live loop
  uint64_t wait_cycles = 0;  // cycles the core sat in S_FWAIT or S_DWAIT

  // The harness's own handshake tallies.  "edges" counts 0->1 transitions of
  // the level; "cycles" counts cycles the level was high.  A one-cycle
  // handshake has edges == cycles.
  uint64_t if_request_cycles = 0;
  uint64_t if_accept_edges = 0;
  uint64_t if_accept_cycles = 0;
  uint64_t d_request_edges = 0;
  uint64_t d_accept_edges = 0;
  uint64_t d_accept_cycles = 0;
  uint64_t d_write_accept_edges = 0;
  uint64_t d_write_accept_cycles = 0;
  uint64_t d_read_accept_edges = 0;
  uint64_t d_read_accept_cycles = 0;
  uint64_t retire_edges = 0;
  uint64_t retire_cycles = 0;

  // Violations, each named with the cycle it first happened.
  uint64_t first_double_retire_cycle = 0;   // two samples saw one retire pulse
  uint64_t first_double_if_cycle = 0;       // two samples saw one fetch ack
  uint64_t first_double_d_cycle = 0;        // two samples saw one data ack
  uint64_t first_if_stretch_cycle = 0;      // the fetch ack was high two cycles
  uint64_t first_d_stretch_cycle = 0;       // the data ack was high two cycles
  uint64_t first_ack_without_request_cycle = 0;
  uint64_t first_request_overrun_cycle = 0; // a request while one was in flight

  uint64_t if_requests_overrun = 0;
  uint64_t d_requests_overrun = 0;
  uint64_t d_acks_without_request = 0;
  uint64_t if_acks_without_request = 0;

  std::vector<mosaic::RetireEvent> events;
  std::vector<Annotation> annotations;

  bool tohost_retire_seen = false;
  uint64_t tohost_retire_cycle = 0;
  // The three cycle numbers of the exit protocol, so the timeline can be
  // checked cycle-exactly rather than described.
  uint64_t tohost_request_cycle = 0;  // the store is posted on the data port
  uint64_t tohost_watch_cycle = 0;    // the model's end-of-run pulse is seen
  bool tohost_written = false;
  // The core posts its first fetch request while reset is still asserted (it
  // holds S_FETCH with the reset vector), so a request is already outstanding
  // at the first live edge.  Recorded, and checked as a fact, rather than
  // assumed by silently pre-setting the harness's in-flight flag.
  uint64_t reset_release_state = 0;
  uint64_t tohost_value = 0;
  uint64_t signature[4] = {0, 0, 0, 0};

  bool final_check_ran = false;

  std::string Stream() const {
    std::string text;
    for (const mosaic::RetireEvent& event : events) text += event.Line() + "\n";
    return text;
  }
};

class Dut {
 public:
  Dut() : dut_(&ctx_) { ZeroInputs(); }

  // ---- the timeline convention -------------------------------------------
  //
  // One cycle is one rising edge of h_clk.  A cycle has exactly three phases
  // and the harness takes exactly one eval() in each:
  //
  //   low phase    h_clk = 0; eval(); harness drives inputs (reset, image,
  //                backpressure latency)
  //   rising edge  h_clk = 1; eval(); the sequential state updates here
  //   high phase   sample the settled outputs -- exactly once, from the state
  //                the rising edge just produced
  //   falling edge h_clk = 0; eval(); the model is left low for the next cycle
  //
  // The sample is taken after the rising edge and before the falling edge, and
  // only there, so "the state has settled" is structural rather than a promise.
  void LowPhase()    { dut_.h_clk = 0; dut_.eval(); }
  void RisingEdge()  { dut_.h_clk = 1; dut_.eval(); }
  void FallingEdge() { dut_.h_clk = 0; dut_.eval(); }

  VerilatedContext* context() { return &ctx_; }

  // ---- harness actions ----------------------------------------------------
  void HoldReset() { dut_.h_rst = 1; dut_.eval(); }

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

  // The whole fixture, assembled into 8-byte words against a zero shadow.  The
  // loader port is word-granular by contract, so a partial tail word is written
  // as a whole word and the words the image does not cover stay zero.
  void LoadFixture(const std::string& bytes) {
    std::map<uint64_t, uint64_t> words;
    for (size_t i = 0; i < bytes.size(); ++i) {
      const uint64_t address = MOSAIC_RESET_VECTOR + i;
      const uint64_t byte = static_cast<unsigned char>(bytes[i]);
      words[address & ~UINT64_C(7)] |= byte << (8 * (address & 7));
    }
    for (const auto& word : words) WriteWord(word.first, word.second);
  }

  // Backdoor readback: request in one cycle, answer registered at that cycle's
  // end.  rst_i must be low, because the model's reset branch clears the answer
  // register; every caller has finished the run it is inspecting.
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

  void DriveBackpressure(const BackpressurePlan& plan, uint64_t if_handshakes,
                         uint64_t d_handshakes) {
    dut_.h_if_gap = plan.fetch_gap(if_handshakes);
    dut_.h_d_gap = plan.data_gap(d_handshakes);
  }

  void ClearBackpressure() {
    dut_.h_if_gap = 0;
    dut_.h_d_gap = 0;
  }

  void Final() { dut_.final(); }

  // True only if Verilator still holds a scheduled event.  With no --timing in
  // the build this is always false, which is exactly the statement the case
  // makes: nothing in this run depends on an unadvanced delay.
  bool eventsPending() { return dut_.eventsPending(); }

  RunResult Run(const BackpressurePlan& plan, uint64_t max_cycles) {
    RunResult r;
    r.plan = plan.name;

    // ---- reset: no backpressure, so reset length stays a property of the
    // reset alone and not of the plan under test.
    ClearBackpressure();
    HoldReset();
    for (uint64_t i = 0; i < kResetEdges; ++i) {
      LowPhase();
      RisingEdge();
      FallingEdge();
    }
    // The release happens with the clock low and without taking an edge, so the
    // first rising edge the live loop produces is the first one the core sees
    // out of reset.
    dut_.h_rst = 0;
    dut_.eval();
    r.reset_release_state = dut_.c_dbg_state;

    // Cycle numbering restarts here: run cycle 1 is the first rising edge after
    // release.  This is the harness's integer time unit, and the only one.
    cycle_ = 0;
    prev_if_ = false;
    prev_d_ = false;
    prev_evt_ = false;
    last_if_sample_cycle_ = 0;
    last_d_sample_cycle_ = 0;
    last_evt_sample_cycle_ = 0;
    prev_if_cycle_ = 0;
    prev_d_cycle_ = 0;
    prev_evt_cycle_ = 0;
    // The core is held in S_FETCH while reset is asserted, and S_FETCH is the
    // state that posts the fetch request, so a request is already outstanding
    // when the first live edge is taken.  Pre-setting the flag is therefore the
    // truth about the interface, not a workaround; run_once checks the state it
    // is derived from.
    if_in_flight_ = (r.reset_release_state == kStateFetch);
    if (if_in_flight_) ++r.if_request_cycles;
    d_in_flight_ = false;
    d_in_flight_we_ = false;

    const uint64_t cap = max_cycles < kRunCycleCap ? max_cycles : kRunCycleCap;
    while (cycle_ < cap) {
      ++cycle_;

      LowPhase();
      DriveBackpressure(plan, r.if_accept_edges, r.d_accept_edges);
      RisingEdge();
#ifdef MOSAIC_SAMPLING_MUTANT_SKIP_SAMPLE
      // MUTANT (V-010 control): the timeline is advanced without re-sampling.
      // The rising edge is taken but the settled state is not looked at on
      // every other cycle, so an architectural retire that happened in a
      // skipped cycle is never recorded.  This is the "missed accept" the
      // tally comparison exists for.
      if ((cycle_ % 2) == 1) Sample(&r);
#else
      Sample(&r);
#endif

#ifdef MOSAIC_SAMPLING_MUTANT_DOUBLE_EVAL
      // MUTANT (V-010 control): eval() used as if it were a second clock.  The
      // high phase is evaluated a second time and the sampler is re-entered, so
      // the same settled state -- including the same architectural retire --
      // is recorded twice.  Nothing about the DUT changes; only the harness's
      // count does.
      dut_.eval();
      Sample(&r);
#endif

      FallingEdge();

      if (r.tohost_retire_seen) {
        r.outcome = "retired_tohost";
        break;
      }
      if (!InMappedRange(dut_.c_dbg_pc)) {
        r.outcome = "pc_escaped";
        break;
      }
    }
    if (r.outcome.empty()) r.outcome = "cycle_limit";
    r.cycles = cycle_;

    ClearBackpressure();
    r.tohost_written = dut_.h_tohost_written != 0;
    r.tohost_value = dut_.h_tohost_value;
    r.signature[0] = ReadWord(MOSAIC_SIGNATURE_ADDR + 0);
    r.signature[1] = ReadWord(MOSAIC_SIGNATURE_ADDR + 8);
    r.signature[2] = ReadWord(MOSAIC_SIGNATURE_ADDR + 16);
    r.signature[3] = ReadWord(MOSAIC_SIGNATURE_ADDR + 24);
    return r;
  }

  static constexpr uint64_t kResetEdges = 4;

 private:
  // =========================================================================
  // The sampling loop.  Called exactly once per cycle in the shipping build,
  // from the settled high phase.
  // =========================================================================
  void Sample(RunResult* r) {
    const bool evt = dut_.c_evt_valid != 0;
    const bool ifa = dut_.h_if_pending != 0;
    const bool da = dut_.h_d_pending != 0;
    const bool dreq = dut_.c_dmem_req_o != 0;
    const bool dwe = dut_.c_dmem_we_o != 0;
    const uint32_t state = dut_.c_dbg_state;

    // ---- the core's waiting states, as a measure of real stalls ------------
    if (state == kStateFwait || state == kStateDwait) ++r->wait_cycles;

    // ---- the fetch port ----------------------------------------------------
    // The request is observable through the core's published debug state:
    // mosaic_bringup_core.sv asserts ifetch_req_o in S_FETCH and nowhere else.
    if (state == kStateFetch) {
      ++r->if_request_cycles;
      if (if_in_flight_) {
        ++r->if_requests_overrun;
        if (r->first_request_overrun_cycle == 0) r->first_request_overrun_cycle = cycle_;
      }
      if_in_flight_ = true;
    }
    if (ifa) {
      ++r->if_accept_cycles;
      if (!prev_if_) ++r->if_accept_edges;
      if (last_if_sample_cycle_ == cycle_) {
        if (r->first_double_if_cycle == 0) r->first_double_if_cycle = cycle_;
      }
      // A one-cycle handshake cannot be high in two consecutive cycles.  If it
      // is, the environment held the ack and an edge-counting harness would
      // undercount it while a level-counting one would overcount it.
      if (prev_if_ && prev_if_cycle_ + 1 == cycle_) {
        if (r->first_if_stretch_cycle == 0) r->first_if_stretch_cycle = cycle_;
      }
      if (!if_in_flight_) {
        ++r->if_acks_without_request;
        if (r->first_ack_without_request_cycle == 0) {
          r->first_ack_without_request_cycle = cycle_;
        }
      }
      if_in_flight_ = false;
      last_if_sample_cycle_ = cycle_;
    }

    // ---- the data port -----------------------------------------------------
    if (dreq) {
      ++r->d_request_edges;
      if (dwe && ((dut_.c_dmem_addr_o & ~UINT64_C(7)) == MOSAIC_TOHOST) &&
          r->tohost_request_cycle == 0) {
        r->tohost_request_cycle = cycle_;
      }
      if (d_in_flight_) {
        ++r->d_requests_overrun;
        if (r->first_request_overrun_cycle == 0) r->first_request_overrun_cycle = cycle_;
      }
      d_in_flight_ = true;
      d_in_flight_we_ = dwe;
    }
    if (da) {
      ++r->d_accept_cycles;
      if (!prev_d_) ++r->d_accept_edges;
      // The ack answers the request that is in flight; dmem_we_o is only valid
      // with the request, so the accept is classified by what the request was.
      if (d_in_flight_we_) {
        ++r->d_write_accept_cycles;
        if (!prev_d_) ++r->d_write_accept_edges;
      } else {
        ++r->d_read_accept_cycles;
        if (!prev_d_) ++r->d_read_accept_edges;
      }
      if (last_d_sample_cycle_ == cycle_) {
        if (r->first_double_d_cycle == 0) r->first_double_d_cycle = cycle_;
      }
      if (prev_d_ && prev_d_cycle_ + 1 == cycle_) {
        if (r->first_d_stretch_cycle == 0) r->first_d_stretch_cycle = cycle_;
      }
      if (!d_in_flight_) {
        ++r->d_acks_without_request;
        if (r->first_ack_without_request_cycle == 0) {
          r->first_ack_without_request_cycle = cycle_;
        }
      }
      d_in_flight_ = false;
      last_d_sample_cycle_ = cycle_;
    }

    // ---- the end-of-run watch ----------------------------------------------
    // The model raises h_tohost_written for one cycle, one cycle after it
    // accepts a non-zero store to TOHOST.  Watching it every cycle (rather
    // than only after the retire) is what makes the ordering between the
    // environment's exit signal and the store's architectural retire
    // observable: with a one-cycle memory they coincide, and under
    // backpressure the pulse arrives while the store is still outstanding.
    if (dut_.h_tohost_written != 0 && r->tohost_watch_cycle == 0) {
      r->tohost_watch_cycle = cycle_;
    }

    // ---- the retire port ---------------------------------------------------
    if (evt) {
      ++r->retire_cycles;
      if (!prev_evt_) ++r->retire_edges;
      if (last_evt_sample_cycle_ == cycle_) {
        if (r->first_double_retire_cycle == 0) r->first_double_retire_cycle = cycle_;
      }
      if (prev_evt_ && prev_evt_cycle_ + 1 == cycle_) {
        // The core's own contract is a one-cycle pulse; two consecutive cycles
        // of evt_valid would be a different (and wrong) core, and the sampler
        // must not silently turn it into two records either way.
        if (r->first_double_retire_cycle == 0) r->first_double_retire_cycle = cycle_;
      }

      Annotation note;
      note.cycle = cycle_;
      mosaic::RetireEvent event = CurrentEvent(r->events.size());
      r->events.push_back(event);
      r->annotations.push_back(note);
#ifdef MOSAIC_SAMPLING_MUTANT_REEMIT_RECORD
      // MUTANT (V-010 control): the record callback fires twice for the same
      // architectural event and re-emits the identical (retire_seq, pc)
      // identity.  This is the double count the identity detector exists for;
      // the record count is wrong too, but the identity is what names it.
      r->events.push_back(r->events.back());
      r->annotations.push_back(note);
#endif

      if (event.is_store &&
          ((event.store_address & ~UINT64_C(7)) == MOSAIC_TOHOST)) {
        r->tohost_retire_seen = true;
        r->tohost_retire_cycle = cycle_;
      }
      last_evt_sample_cycle_ = cycle_;
    }

    // ---- carry the sample forward ------------------------------------------
    prev_if_ = ifa;
    prev_d_ = da;
    prev_evt_ = evt;
    if (ifa) prev_if_cycle_ = cycle_;
    if (da) prev_d_cycle_ = cycle_;
    if (evt) prev_evt_cycle_ = cycle_;
  }

  // The sequence number is the harness's own record index, so "dense with no
  // gap" is a statement about the sampling loop and not about the DUT.
  mosaic::RetireEvent CurrentEvent(uint64_t seq) {
    mosaic::RetireEvent event;
    event.hart = 0;
    event.seq = seq;
    event.pc = dut_.c_evt_pc;
    event.next_pc = dut_.c_evt_next_pc;
    event.insn = dut_.c_evt_insn;
    event.has_rd = dut_.c_evt_has_rd != 0;
    event.rd = static_cast<uint8_t>(dut_.c_evt_rd);
    event.rd_value = dut_.c_evt_rd_value;
    event.is_trap = dut_.c_evt_trap != 0;
    event.cause = dut_.c_evt_cause;
    event.tval = dut_.c_evt_tval;
    event.epc = dut_.c_evt_epc;
    event.is_store = dut_.c_evt_is_store != 0;
    event.store_address = dut_.c_evt_store_addr;
    event.store_data = dut_.c_evt_store_data;
    event.store_size = static_cast<unsigned>(dut_.c_evt_store_size);
    return event;
  }

  void ZeroInputs() {
    dut_.h_clk = 0;
    dut_.h_rst = 1;
    dut_.h_img_we = 0;
    dut_.h_img_addr = 0;
    dut_.h_img_data = 0;
    dut_.h_clear_mem = 0;
    dut_.h_rb_req = 0;
    dut_.h_rb_addr = 0;
    dut_.h_dbg_csr_addr = 0;
    dut_.h_if_gap = 0;
    dut_.h_d_gap = 0;
    dut_.eval();
  }

  VerilatedContext ctx_;
  Vmosaic_bringup_tb dut_;

  uint64_t cycle_ = 0;
  bool prev_if_ = false, prev_d_ = false, prev_evt_ = false;
  uint64_t last_if_sample_cycle_ = 0, last_d_sample_cycle_ = 0,
           last_evt_sample_cycle_ = 0;
  uint64_t prev_if_cycle_ = 0, prev_d_cycle_ = 0, prev_evt_cycle_ = 0;
  bool if_in_flight_ = false;
  bool d_in_flight_ = false;
  bool d_in_flight_we_ = false;
};

// ===========================================================================
// The independent reference model
// ===========================================================================
//
// The oracle is not written here and not written from the RTL: it is the very
// text core.bringup_vs_reference uses (work package I-008), extracted from
// sim/unit/tb_bringup.cpp at run time.  Extracting it rather than copying it is
// the point -- a copy would be a second oracle that could silently disagree
// with the one I-008 trusts, and this case's job is to be comparable with I-008,
// not to be a third opinion.  The extraction verifies the sentinels it needs,
// so a moved or restructured model fails here by name instead of producing a
// silently different count.
struct ReferenceResult {
  std::string outcome;
  uint64_t events = 0;   // instructions retired, per the reference
  uint64_t steps = 0;
  uint64_t minstret = 0;
  uint64_t loads = 0;    // decoded from the reference's own instruction stream
  uint64_t stores = 0;
  uint64_t tohost = 0;
  uint64_t signature[4] = {0, 0, 0, 0};
  std::string first_event;
};

std::string ExtractReferenceModel(const std::string& path, std::string* detail) {
  std::string text;
  if (!ReadWholeFile(path, &text)) {
    *detail = "cannot read " + path;
    return "";
  }
  const std::string begin = "R\"PYMODEL(";
  const std::string end = ")PYMODEL\"";
  const size_t from = text.find(begin);
  if (from == std::string::npos) {
    *detail = path + " no longer contains the PYMODEL raw string: the I-008 oracle "
                     "cannot be extracted";
    return "";
  }
  const size_t to = text.find(end, from + begin.size());
  if (to == std::string::npos) {
    *detail = path + " has an unterminated PYMODEL raw string";
    return "";
  }
  const std::string model = text.substr(from + begin.size(),
                                        to - (from + begin.size()));
  for (const char* sentinel : {"class Machine", "class Memory", "class CsrFile",
                               "def main(argv):", "config\", \"memory\""}) {
    if (model.find(sentinel) == std::string::npos) {
      *detail = std::string(path) + ": the extracted I-008 oracle does not contain " +
                sentinel;
      return "";
    }
  }
  return model;
}

bool RunReference(const std::string& repo, const std::string& out_dir,
                  const std::string& segment_hex, ReferenceResult* result,
                  std::string* detail) {
  const std::string script = out_dir + "/sampling_reference_model.py";
  const std::string manifest = out_dir + "/sampling.image.txt";
  const std::string events = out_dir + "/sampling.reference.events.txt";
  const std::string summary = out_dir + "/sampling_reference.summary.txt";

  std::string model = ExtractReferenceModel(repo + "/sim/unit/tb_bringup.cpp", detail);
  if (model.empty()) return false;
  {
    std::ofstream out(script);
    if (!out) { *detail = "cannot write " + script; return false; }
    out << model;
  }
  {
    std::ofstream out(manifest);
    if (!out) { *detail = "cannot write " + manifest; return false; }
    out << "seg " << std::hex << MOSAIC_RESET_VECTOR << " "
        << std::dec << kFixtureBytes << " " << segment_hex << "\n";
  }

  std::ostringstream command;
  command << "python3 " << script
          << " --repo " << repo
          << " --image " << manifest
          << " --events " << events
          << " --summary " << summary
          << " --max-steps " << kRunCycleCap << " 2>&1";
  std::string output;
  std::FILE* pipe = popen(command.str().c_str(), "r");
  if (pipe == nullptr) { *detail = "cannot start the reference model"; return false; }
  char line[4096];
  while (std::fgets(line, sizeof(line), pipe) != nullptr) output += line;
  if (pclose(pipe) != 0) {
    *detail = "reference model exited non-zero: " + output;
    return false;
  }

  std::string text;
  if (!ReadWholeFile(summary, &text)) {
    *detail = "reference model produced no summary: " + output;
    return false;
  }
  std::istringstream in(text);
  std::string key, token;
  while (in >> key >> token) {
    if (key == "outcome") result->outcome = token;
    else if (key == "events") result->events = std::strtoull(token.c_str(), nullptr, 10);
    else if (key == "steps") result->steps = std::strtoull(token.c_str(), nullptr, 10);
    else if (key == "minstret") result->minstret = std::strtoull(token.c_str(), nullptr, 10);
    else if (key == "tohost") result->tohost = std::strtoull(token.c_str(), nullptr, 16);
    else if (key.rfind("sig", 0) == 0 && key.size() == 4 &&
             key[3] >= '0' && key[3] <= '9') {
      result->signature[key[3] - '0'] = std::strtoull(token.c_str(), nullptr, 16);
    }
  }

  // The instruction counts come from the reference's own emitted stream, not
  // from its summary: the opcode is read out of the `insn=` field of every
  // line and decoded against the ISA manual's LOAD and STORE opcodes.  A
  // truncated stream would be caught by the events/steps check below.
  std::string stream_text;
  if (!ReadWholeFile(events, &stream_text)) {
    *detail = "reference model produced no event stream";
    return false;
  }
  std::istringstream stream(stream_text);
  uint64_t lines = 0;
  std::string text_line;
  while (std::getline(stream, text_line)) {
    if (text_line.empty()) continue;
    if (lines == 0) result->first_event = text_line;
    ++lines;
    const size_t at = text_line.find(" insn=");
    if (at == std::string::npos || at + 13 > text_line.size()) {
      *detail = "reference event line has no insn field: " + text_line;
      return false;
    }
    const uint32_t insn = static_cast<uint32_t>(
        std::strtoul(text_line.substr(at + 6, 8).c_str(), nullptr, 16));
    if ((insn & 0x7F) == kOpcodeLoad) ++result->loads;
    if ((insn & 0x7F) == kOpcodeStore) ++result->stores;
  }
  if (lines != result->events) {
    *detail = "the reference summary says " + std::to_string(result->events) +
              " events but the stream has " + std::to_string(lines) + " lines";
    return false;
  }
  detail->clear();
  return true;
}

}  // namespace

// ===========================================================================
// main
// ===========================================================================

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
  uint64_t runs = 0;
  uint64_t final_checks = 0;
  uint64_t total_cycles = 0;

  try {
    const std::string repo = FindRepoRoot();

    // ---- 1. configuration --------------------------------------------------
    std::string profile_text;
    if (!ReadWholeFile(repo + "/config/profiles/p0.json", &profile_text)) {
      Fail("cannot read config/profiles/p0.json");
    }
    uint64_t reset_vector = 0, tohost = 0, signature = 0;
    auto need = [&](const char* key, uint64_t* out) {
      if (!JsonUint(profile_text, key, out)) {
        Fail(std::string("config/profiles/p0.json has no readable \"") + key + "\"");
      }
    };
    need("reset_vector", &reset_vector);
    need("tohost", &tohost);
    need("signature", &signature);
    reporter.Check(reset_vector == MOSAIC_RESET_VECTOR,
                   "config reset_vector matches build/p0/sim/mosaic_platform.h");
    reporter.Check(tohost == MOSAIC_TOHOST,
                   "config TOHOST matches the generated header");
    reporter.Check(signature == MOSAIC_SIGNATURE_ADDR,
                   "config signature address matches the generated header");

    // ---- 2. the fixture ----------------------------------------------------
    const std::string fixture = Base64Decode(kFixtureBase64);
    reporter.Check(fixture.size() == kFixtureBytes,
                   "the embedded fixture is " + std::to_string(kFixtureBytes) +
                       " bytes (got " + std::to_string(fixture.size()) + ")");
    const uint64_t fixture_hash = Fnv1a64(fixture);
    reporter.Check(fixture_hash == kFixtureFnv1a,
                   "the embedded fixture bytes hash to " +
                       mosaic::Hex(kFixtureFnv1a, 1).substr(2) + " (got " +
                       mosaic::Hex(fixture_hash, 1).substr(2) + ")");
    reporter.Check(fixture.size() % 4 == 0,
                   "the fixture is a whole number of 32-bit instructions");

    // The fixture's own properties, read from its bytes and not assumed: no
    // SYSTEM instruction (so the program takes no trap, which is what makes
    // "one fetch per instruction" hold for this program), and a trailing
    // self-loop that is never fetched because the run ends on the TOHOST store.
    uint64_t system_words = 0, load_words = 0, store_words = 0;
    for (size_t i = 0; i + 4 <= fixture.size(); i += 4) {
      const uint32_t word = static_cast<uint32_t>(static_cast<unsigned char>(fixture[i])) |
                            (static_cast<uint32_t>(static_cast<unsigned char>(fixture[i + 1])) << 8) |
                            (static_cast<uint32_t>(static_cast<unsigned char>(fixture[i + 2])) << 16) |
                            (static_cast<uint32_t>(static_cast<unsigned char>(fixture[i + 3])) << 24);
      if ((word & 0x7F) == kOpcodeSystem) ++system_words;
      if ((word & 0x7F) == kOpcodeLoad) ++load_words;
      if ((word & 0x7F) == kOpcodeStore) ++store_words;
    }
    const size_t last = fixture.size() - 4;
    const uint32_t tail =
        static_cast<uint32_t>(static_cast<unsigned char>(fixture[last])) |
        (static_cast<uint32_t>(static_cast<unsigned char>(fixture[last + 1])) << 8) |
        (static_cast<uint32_t>(static_cast<unsigned char>(fixture[last + 2])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(fixture[last + 3])) << 24);
    reporter.Check(system_words == 0,
                   "the fixture executes no SYSTEM instruction, so one fetch per "
                   "instruction holds: it traps nowhere and touches no CSR");
    reporter.Check((tail & 0x7F) == kOpcodeJal && (tail & 0xF8000) == 0 &&
                       (tail >> 7) == 0,
                   "the fixture ends in a self-loop that the run never fetches, so "
                   "the last executed instruction is the TOHOST store (" +
                       mosaic::Hex(tail, 8).substr(2) + ")");
    reporter.Check(load_words >= 1 && store_words >= 3,
                   "the fixture really has memory traffic (" +
                       std::to_string(load_words) + " static loads, " +
                       std::to_string(store_words) + " static stores per pass, and "
                       "the loop body executes four times)");

    // ---- 3. the architectural expectation, from the I-008 oracle ----------
    ReferenceResult expected;
    std::string reference_detail;
    std::stringstream hex;
    hex << std::hex;
    for (size_t i = 0; i < fixture.size(); ++i) {
      hex.width(2);
      hex.fill('0');
      hex << static_cast<unsigned>(static_cast<unsigned char>(fixture[i]));
    }
    if (!RunReference(repo, options.out_dir, hex.str(), &expected, &reference_detail)) {
      Fail("the independent I-008 reference model could not be run: " + reference_detail);
    }
    reporter.Check(expected.outcome == "tohost",
                   "the independent reference model runs the fixture to the TOHOST "
                   "store and not to its step limit (outcome \"" + expected.outcome +
                       "\", " + std::to_string(expected.events) + " events)");
    reporter.Check(expected.events == expected.steps &&
                       expected.events == expected.minstret,
                   "the reference model's own count of instructions is internally "
                   "consistent (events=" + std::to_string(expected.events) +
                       ", steps=" + std::to_string(expected.steps) +
                       ", minstret=" + std::to_string(expected.minstret) + ")");
    reporter.Check(expected.loads > 0 && expected.stores > 0,
                   "the reference's instruction stream contains memory operations (" +
                       std::to_string(expected.loads) + " loads, " +
                       std::to_string(expected.stores) + " stores)");

    std::printf("  reference (I-008 oracle, extracted from tb_bringup.cpp): "
                "%llu instructions, %llu loads, %llu stores, tohost=%s\n",
                static_cast<unsigned long long>(expected.events),
                static_cast<unsigned long long>(expected.loads),
                static_cast<unsigned long long>(expected.stores),
                mosaic::Hex(expected.tohost).c_str());

    // ---- 4. the three runs -------------------------------------------------
    Dut dut;
    const BackpressurePlan plan_a{"zero-latency", nullptr, nullptr, 0, 0};
    const BackpressurePlan plan_b{"plan B", kPatternBFetch, kPatternBData,
                                  sizeof(kPatternBFetch), sizeof(kPatternBData)};
    const BackpressurePlan plan_c{"plan C", kPatternCFetch, kPatternCData,
                                  sizeof(kPatternCFetch), sizeof(kPatternCData)};

    auto run_once = [&](const BackpressurePlan& plan, const char* tag) {
      dut.Clear();
      dut.LoadFixture(fixture);
      RunResult r = dut.Run(plan, options.max_cycles);
      ++runs;
      total_cycles += r.cycles;

      const uint64_t expected_mem = expected.loads + expected.stores;

      std::printf("  %-14s %-15s cycles=%-6llu events=%-4zu fetch_acks=%-4llu "
                  "data_acks=%-4llu (w=%llu r=%llu) wait=%-5llu "
                  "tohost store req=%llu watch=%llu retire=%llu\n",
                  tag, r.outcome.c_str(),
                  static_cast<unsigned long long>(r.cycles), r.events.size(),
                  static_cast<unsigned long long>(r.if_accept_edges),
                  static_cast<unsigned long long>(r.d_accept_cycles),
                  static_cast<unsigned long long>(r.d_write_accept_cycles),
                  static_cast<unsigned long long>(r.d_read_accept_cycles),
                  static_cast<unsigned long long>(r.wait_cycles),
                  static_cast<unsigned long long>(r.tohost_request_cycle),
                  static_cast<unsigned long long>(r.tohost_watch_cycle),
                  static_cast<unsigned long long>(r.tohost_retire_cycle));

      // ---- (a) the run ended the way the protocol says --------------------
      reporter.Check(r.outcome == "retired_tohost",
                     std::string(tag) + ": the run ends on the retire of the TOHOST "
                     "store (outcome \"" + r.outcome + "\" after " +
                         std::to_string(r.cycles) + " cycles)");
      reporter.Check(r.reset_release_state == kStateFetch,
                     std::string(tag) + ": the core is held in S_FETCH at reset "
                     "release, so its first fetch request is already outstanding at "
                     "the first live edge (state " +
                         std::to_string(r.reset_release_state) + ")");
      reporter.Check(r.tohost_request_cycle != 0 && r.tohost_watch_cycle != 0,
                     std::string(tag) + ": the exit protocol actually happened: the "
                     "TOHOST store was posted on the data port at cycle " +
                         std::to_string(r.tohost_request_cycle) +
                         " and the model's end-of-run pulse was seen at cycle " +
                         std::to_string(r.tohost_watch_cycle));
      reporter.Check(r.tohost_written || r.tohost_watch_cycle != 0,
                     std::string(tag) + ": the end-of-run watch fired for the TOHOST "
                     "store (h_tohost_written was high at cycle " +
                         std::to_string(r.tohost_watch_cycle) + ")");
      reporter.Check(r.tohost_value != 0,
                     std::string(tag) + ": TOHOST is non-zero, which is the frozen "
                     "end-of-run signal (" + mosaic::Hex(r.tohost_value) + ")");
      reporter.Check(r.tohost_value == expected.tohost,
                     std::string(tag) + ": TOHOST agrees with the independent "
                     "reference (" + mosaic::Hex(expected.tohost) + ")");
      reporter.Check(r.signature[0] == expected.signature[0],
                     std::string(tag) + ": signature[0] agrees with the independent "
                     "reference (" + mosaic::Hex(expected.signature[0]) + ", got " +
                         mosaic::Hex(r.signature[0]) + ")");

      // ---- the timeline, checked cycle-exactly ----------------------------
      //
      // The data-port protocol is: the core posts the request in cycle N; the
      // model accepts it at the end of N, which is when the store takes effect
      // and the end-of-run latch is set; h_tohost_written is that latch
      // registered once more, so it is seen in cycle N+2; the ack is delivered
      // in cycle N+1+gap; the core commits on that edge and the event pulse is
      // registered, so the retire is seen in cycle N+2+gap.  Three relations
      // follow and are asserted here, which is what fixes the meaning of one
      // cycle: watch = request + 2, and retire = watch + the gap this item was
      // given.  A harness that had the phase order wrong, or that had confused
      // eval() with a clock, would not reproduce them.
      const uint64_t last_data_gap = plan.data_gap(expected_mem - 1);
      reporter.Check(r.tohost_watch_cycle == r.tohost_request_cycle + 2,
                     std::string(tag) + ": the end-of-run pulse is the store's accept "
                     "registered twice (watch cycle " +
                         std::to_string(r.tohost_watch_cycle) + " == request cycle " +
                         std::to_string(r.tohost_request_cycle) + " + 2)");
      reporter.Check(r.tohost_retire_cycle == r.tohost_watch_cycle + last_data_gap,
                     std::string(tag) + ": the store's retire follows its end-of-run "
                     "pulse by exactly the latency this item was given (retire cycle " +
                         std::to_string(r.tohost_retire_cycle) + " == watch cycle " +
                         std::to_string(r.tohost_watch_cycle) + " + gap " +
                         std::to_string(last_data_gap) + ")");
      reporter.Check(r.tohost_retire_cycle >= r.tohost_watch_cycle,
                     std::string(tag) + ": the exit signal is never seen after the "
                     "store it belongs to has retired");

      // ---- (b) the repeated-sampling detector -----------------------------
      // Identity is (retire_seq, pc).  The sequence numbers are the harness's
      // own record indices, so this asserts two properties of the sampling
      // loop: it never emits a record twice for one identity, and it never
      // skips a number.  A loop that keyed on `pc` alone would fail here on
      // purpose, because the fixture's loop body retires from the same pc four
      // times -- the detector checks an identity, not a pc.
      std::set<uint64_t> seen_seq;
      uint64_t first_duplicate_seq = 0, first_dense_gap = 0;
      for (size_t i = 0; i < r.events.size(); ++i) {
        const uint64_t seq = r.events[i].seq;
        if (seq != i && first_dense_gap == 0) first_dense_gap = i;
        if (!seen_seq.insert(seq).second && first_duplicate_seq == 0) {
          first_duplicate_seq = seq;
        }
      }
      reporter.Check(first_duplicate_seq == 0,
                     std::string(tag) + ": no retire_seq is emitted twice (first "
                     "duplicate seq " + std::to_string(first_duplicate_seq) + ")");
      reporter.Check(first_dense_gap == 0,
                     std::string(tag) + ": the retire sequence numbers are dense with "
                     "no gap (first gap at record " + std::to_string(first_dense_gap) +
                         ")");
      std::map<std::pair<uint64_t, uint64_t>, uint64_t> identity;
      uint64_t first_repeat_identity_cycle = 0;
      for (size_t i = 0; i < r.events.size(); ++i) {
        const auto key = std::make_pair(r.events[i].seq, r.events[i].pc);
        if (!identity.insert({key, i}).second && first_repeat_identity_cycle == 0) {
          first_repeat_identity_cycle = r.annotations[i].cycle;
        }
      }
      reporter.Check(first_repeat_identity_cycle == 0,
                     std::string(tag) + ": no (retire_seq, pc) identity is recorded "
                     "twice (first repeat at cycle " +
                         std::to_string(first_repeat_identity_cycle) + ")");
      // The other half of repeated sampling: the loop itself must not look at
      // the same settled state twice.  A second sample inside one cycle sees
      // the same level again, so that cycle's tally jumps -- and it is named.
      reporter.Check(r.first_double_if_cycle == 0 && r.first_double_d_cycle == 0 &&
                         r.first_double_retire_cycle == 0,
                     std::string(tag) + ": no cycle was sampled twice for one "
                     "handshake (fetch=" + std::to_string(r.first_double_if_cycle) +
                         ", data=" + std::to_string(r.first_double_d_cycle) +
                         ", retire=" + std::to_string(r.first_double_retire_cycle) + ")");

      // ---- (c) the three tallies agree -----------------------------------
      reporter.Check(r.events.size() == expected.events,
                     std::string(tag) + ": the tap recorded one event per instruction "
                     "the reference says the program retires (" +
                         std::to_string(r.events.size()) + " records vs " +
                         std::to_string(expected.events) + " reference instructions)");
      reporter.Check(static_cast<uint64_t>(r.events.size()) == r.retire_cycles,
                     std::string(tag) + ": the tap's record count equals the "
                     "harness's own retire tally (" + std::to_string(r.events.size()) +
                         " vs " + std::to_string(r.retire_cycles) + ")");
      reporter.Check(r.if_accept_edges == expected.events,
                     std::string(tag) + ": one instruction fetch per instruction "
                     "retired, per the core's one-outstanding-request contract (" +
                         std::to_string(r.if_accept_edges) + " fetch accepts vs " +
                         std::to_string(expected.events) + " instructions)");
      reporter.Check(r.d_accept_cycles == expected_mem,
                     std::string(tag) + ": one data handshake per memory operation in "
                     "the reference's stream (" + std::to_string(r.d_accept_cycles) +
                         " accepts vs " + std::to_string(expected.loads) + "+" +
                         std::to_string(expected.stores) + ")");
      reporter.Check(r.d_write_accept_cycles == expected.stores,
                     std::string(tag) + ": the write handshakes are exactly the "
                     "reference's stores (" + std::to_string(r.d_write_accept_cycles) +
                         " vs " + std::to_string(expected.stores) + ")");
      reporter.Check(r.d_read_accept_cycles == expected.loads,
                     std::string(tag) + ": the read handshakes are exactly the "
                     "reference's loads (" + std::to_string(r.d_read_accept_cycles) +
                         " vs " + std::to_string(expected.loads) + ")");

      // The tally the case is really about: the DUT's own stream must contain
      // exactly the stores the harness handed to memory.
      uint64_t dut_stores = 0;
      for (const mosaic::RetireEvent& event : r.events) {
        if (event.is_store) ++dut_stores;
      }
      reporter.Check(dut_stores == expected.stores,
                     std::string(tag) + ": the DUT's own stream marks exactly the "
                     "reference's store count (" + std::to_string(dut_stores) + " vs " +
                         std::to_string(expected.stores) + ")");

      // ---- (d) the shape of each handshake and the request/ack pairing ----
      // One leading edge, one cycle high.  A level held across two cycles is
      // visible both ways, which is the defect a level-counting environment
      // cannot see.
      reporter.Check(r.if_accept_edges == r.if_accept_cycles,
                     std::string(tag) + ": the fetch ack is a one-cycle pulse (" +
                         std::to_string(r.if_accept_edges) + " leading edges vs " +
                         std::to_string(r.if_accept_cycles) + " cycles high)");
      reporter.Check(r.d_accept_edges == r.d_accept_cycles,
                     std::string(tag) + ": the data ack is a one-cycle pulse (" +
                         std::to_string(r.d_accept_edges) + " leading edges vs " +
                         std::to_string(r.d_accept_cycles) + " cycles high)");
      reporter.Check(r.retire_edges == r.retire_cycles,
                     std::string(tag) + ": the retire pulse is a one-cycle pulse (" +
                         std::to_string(r.retire_edges) + " leading edges vs " +
                         std::to_string(r.retire_cycles) + " cycles high)");
      reporter.Check(r.first_if_stretch_cycle == 0,
                     std::string(tag) + ": no fetch ack was held across two cycles "
                     "(first at cycle " + std::to_string(r.first_if_stretch_cycle) + ")");
      reporter.Check(r.first_d_stretch_cycle == 0,
                     std::string(tag) + ": no data ack was held across two cycles "
                     "(first at cycle " + std::to_string(r.first_d_stretch_cycle) + ")");

      // The pairing between a request and the ack that answers it: one
      // ack per request, none without one, none while one is in flight.
      reporter.Check(r.if_requests_overrun == 0 && r.d_requests_overrun == 0,
                     std::string(tag) + ": no port posted a second request while one "
                     "was in flight (fetch=" + std::to_string(r.if_requests_overrun) +
                         ", data=" + std::to_string(r.d_requests_overrun) +
                         ", first at cycle " +
                         std::to_string(r.first_request_overrun_cycle) + ")");
      reporter.Check(r.if_acks_without_request == 0 && r.d_acks_without_request == 0,
                     std::string(tag) + ": no ack arrived without a request to answer "
                     "(fetch=" + std::to_string(r.if_acks_without_request) + ", data=" +
                         std::to_string(r.d_acks_without_request) + ", first at cycle " +
                         std::to_string(r.first_ack_without_request_cycle) + ")");

      // ---- (e) the repeated pc really does appear, so (b) is not vacuous --
      std::map<uint64_t, uint64_t> pc_counts;
      for (const mosaic::RetireEvent& event : r.events) ++pc_counts[event.pc];
      uint64_t repeated_pcs = 0;
      for (const auto& entry : pc_counts) {
        if (entry.second > 1) ++repeated_pcs;
      }
      reporter.Check(repeated_pcs > 0,
                     std::string(tag) + ": the fixture's loop retires from a repeated "
                     "pc, so keying the detector on pc alone would false-fire (" +
                         std::to_string(repeated_pcs) + " pcs retired more than once)");

      // ---- (f) the run's end-of-run final check ---------------------------
      r.final_check_ran = true;
      ++final_checks;
      return r;
    };

    const RunResult run_a = run_once(plan_a, "zero-latency");
    const RunResult run_b = run_once(plan_b, "plan B");
    const RunResult run_c = run_once(plan_c, "plan C");

    // ---- 5. the architectural stream is invariant under backpressure ------
    reporter.Check(run_b.Stream() == run_a.Stream(),
                   "plan B's retire stream is identical to the zero-latency run's, "
                   "so item-by-item stalls change timing and nothing architectural");
    reporter.Check(run_c.Stream() == run_a.Stream(),
                   "plan C's retire stream is identical to the zero-latency run's");
    reporter.Check(run_b.events.size() == expected.events &&
                       run_c.events.size() == expected.events,
                   "both backpressured runs retire exactly the reference's instruction "
                   "count");

    // ---- 6. the backpressure was real -------------------------------------
    reporter.Check(run_b.cycles > run_a.cycles && run_c.cycles > run_a.cycles,
                   "the backpressured runs take strictly more cycles than the "
                   "zero-latency run (" + std::to_string(run_a.cycles) + " vs " +
                       std::to_string(run_b.cycles) + " and " +
                       std::to_string(run_c.cycles) + ")");
    reporter.Check(run_b.wait_cycles > run_a.wait_cycles &&
                       run_c.wait_cycles > run_a.wait_cycles,
                   "the extra cycles are the core genuinely waiting for memory "
                   "(wait-cycles " + std::to_string(run_a.wait_cycles) + " vs " +
                       std::to_string(run_b.wait_cycles) + " and " +
                       std::to_string(run_c.wait_cycles) + ")");
    reporter.Check(run_a.wait_cycles == expected.events + expected.loads + expected.stores,
                   "with zero latency the core waits exactly one cycle per fetch and "
                   "per memory operation (" + std::to_string(run_a.wait_cycles) +
                       " wait-cycles vs " + std::to_string(expected.events) + "+" +
                       std::to_string(expected.loads) + "+" +
                       std::to_string(expected.stores) + ")");
    reporter.Check(run_b.cycles != run_c.cycles || run_b.wait_cycles != run_c.wait_cycles,
                   "the two plans are different latency patterns, so the runs differ "
                   "in timing while agreeing in architecture");

    // ---- 7. the unit of integer time is the harness's, and nothing advanced it
    //
    // The harness never enables --timing, never calls VerilatedContext::time()
    // to advance anything, and never schedules a delayed event.  If that were
    // false, an eval() would no longer be a phase of this case's clock and the
    // integer cycle would stop being the unit of time.  These two are the
    // checkable statements of that.
    reporter.Check(dut.context()->time() == 0,
                   "Verilator's own integer time never advanced: the unit of time in "
                   "this case is the harness's cycle count and nothing else (time=" +
                       std::to_string(dut.context()->time()) + ")");
    reporter.Check(!dut.eventsPending(),
                   "no delayed event was left pending at the end of the run, so "
                   "--timing is not in use anywhere in this case");

    // ---- 8. the end-of-run final check actually ran -----------------------
    reporter.Check(final_checks == runs && runs == 3,
                   "every run ended by running its end-of-run final check (" +
                       std::to_string(final_checks) + " final checks for " +
                       std::to_string(runs) + " runs)");

#ifndef MOSAIC_SAMPLING_MUTANT_SKIP_FINAL
    dut.Final();
    reporter.Check(!dut.eventsPending(),
                   "the end-of-run final phase still has no pending Verilator events "
                   "after final()");
#else
    // MUTANT (V-010 control): the end-of-run final phase is never entered.
#endif

    detail = "3 runs (zero-latency and two item-by-item latency plans), " +
             std::to_string(total_cycles) + " cycles, " +
             std::to_string(static_cast<unsigned long long>(expected.events)) +
             " instructions per run, reference events == tap records == fetch "
             "accepts, " + std::to_string(reporter.checks()) + " checks";
  } catch (const Failure& failure) {
    passed = false;
    detail = "check failed: " + failure.what;
  }

  const bool ok = passed && (reporter.failures() == 0);
  reporter.Check(ok, "no check failed");
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
