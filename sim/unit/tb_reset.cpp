// ============================================================================
// tb_reset.cpp -- CASE=reset.replay_determinism, work package V-009.
//
// The claim this case exists to make falsifiable is
// docs/validation-plan.md section 5 V-009:
//
//     "run the same program under 1/2/5/17 valid-clock reset lengths and
//      several release phases; a reset interrupts a simulation in progress and
//      restarts it; check the SRAM non-reset initialisation contract
//      separately.  PASS: no commit and no device write during reset; the PC,
//      privilege, CSR and queue-ownership state after release is deterministic;
//      the same seed replays identically.  FAIL: depending on C++ memory
//      happening to be zero, a spurious commit during reset, an old
//      transaction crossing reset and polluting the new run."
//
// WHAT IS DRIVEN, AND WHERE THE EXPECTATION COMES FROM
// ---------------------------------------------------
// The DUT is `mosaic_bringup_core` through `sim/tb/mosaic_bringup_tb.sv`, the
// same pair the registered I-008 case uses.  The expectation is *not* read from
// the DUT and *not* read from a reference stream: it is read from
//   * the configuration (config/profiles/p0.json, config/csr/mode_m.json,
//     config/memory/p0.json) through build/p0/sim/mosaic_platform.h, for the
//     reset vector, the protocol addresses and the CSR reset values;
//   * the core's published interface contract in mosaic_bringup_core.sv, which
//     says reset is *synchronous and active high* and holds the machine in
//     S_FETCH with the PC at the reset vector;
//   * the frozen platform contract in docs/platform-plan.md section 2: "SRAM/
//     BRAM data array is not fully reset; valid/owner/ECC state is reset, and
//     only legally initialised or written data may be read";
//   * the fixture program's own bytes, which are embedded and hash-pinned.
// Nothing here compares the DUT against itself: every check is a relation
// between two independent things (two reset lengths, two release instants, two
// runs, the configuration, or the published contract).
//
// THE ONE THING THAT IS NOT INDEPENDENT
// -------------------------------------
// The "same seed replays identically" check compares the DUT with the DUT, so
// on its own it cannot detect a defect that is deterministic -- a dead machine
// replays perfectly.  It is therefore paired with, and never used instead of,
// the reset-length and restart comparisons, which do have an independent
// expectation.  This is stated here so the report can be read honestly.
//
// THE AXES THAT MATTER, AND THE ONE THAT TURNS OUT NOT TO
// ------------------------------------------------------
// reset length: 1, 2, 5 and 17 rising edges at which the core samples
//   rst_i = 1.  This is a real axis: it moves the first live edge.
// release instant: four physically distinct points inside the clock period at
//   which rst_i is dropped (while the clock is high right after the last reset
//   edge, at the falling edge, in the low phase before the first live edge, and
//   late in that low phase).  The RTL is edge-triggered, so these four must be
//   *identical*; the case measures that and the report calls three of them
//   redundancy probes rather than counting them as four independent data points.
//   What the four instants do prove is that the release point is unambiguously
//   "the first rising edge that samples rst_i = 0", which is the property a
//   harness that models reset from a waveform has to get right.
//
// THE FIXTURE
// -----------
// sim/unit/reset_program.S, 148 bytes, embedded below and pinned by size and by
// an FNV-1a hash of the exact bytes.  It is loaded through the testbench's
// word-granular image port at the reset vector.  It reads a RAM word nothing
// has written, stores and reloads a RAM word, performs exactly one UART write,
// writes and reads mscratch, fills the four signature words, and writes a
// non-zero TOHOST -- the frozen end-of-run signal.  Every store is
// unconditional and writes a constant, so its effect on memory is idempotent
// and an abort-and-restart can be compared as a transaction question rather
// than as a leftover-data question.
// ============================================================================

#include <verilated.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
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
  while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) ||
                               text[end] == 'x' || text[end] == 'X' ||
                               (std::isxdigit(static_cast<unsigned char>(text[end])) &&
                                text[end] != '"'))) {
    ++end;
  }
  const std::string token = text.substr(pos, end - pos);
  if (token.empty()) return false;
  *out = (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
             ? std::strtoull(token.c_str(), nullptr, 16)
             : std::strtoull(token.c_str(), nullptr, 10);
  return true;
}

uint64_t CsrResetFromJson(const std::string& csr_text, const std::string& name) {
  const std::string needle = "\"name\": \"" + name + "\"";
  const size_t at = csr_text.find(needle);
  if (at == std::string::npos) Fail("config/csr/mode_m.json has no entry named " + name);
  uint64_t value = 0;
  if (!JsonUint(csr_text.substr(at + needle.size()), "reset", &value)) {
    Fail("config/csr/mode_m.json entry " + name + " has no readable reset value");
  }
  return value;
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
// Constants
// ===========================================================================

// sim/unit/reset_program.S, assembled and objcopy'd to a flat binary.  The
// recipe, the disassembly and the SHA-256 are in results/reports/V-009-reset.md.
const char* kFixtureBase64 =
    "twIIAJuCEgCTksIANwMQALcDCACbgzMAk5PDABsOEAATHv4BEw4OQLcOBACbjh4Ak57eAAO1"
    "AwAjMK4At9XSApuFNS2TlcUAk4XV0pOVxQCThTUtk5XVAJOFpaUjsL4AA7YOACM0zgCTBhAE"
    "IwDTAHMQBjRzJwA0IzjuALdnAACbh9cAIzz+ABMIEAAjsAIBbwAAAA==";
constexpr size_t kFixtureBytes = 148;
constexpr uint64_t kFixtureFnv1a = UINT64_C(0x3c01b62787c4c669);

// Addresses the fixture uses, transcribed from reset_program.S and cross-
// checked against the configuration at run time.
constexpr uint64_t kSigBase   = 0x80000400ull;  // == test_protocol.signature
constexpr uint64_t kScratch   = 0x80002000ull;  // RAM the fixture stores to
constexpr uint64_t kUntouched = 0x80003000ull;  // RAM the fixture only reads
constexpr uint64_t kSentinel  = 0x80004000ull;  // RAM nothing ever touches
constexpr uint64_t kUartThr   = 0x00100000ull;  // == memory/p0.json uart base

// The core's state encoding, from mosaic_bringup_core.sv `state_e`.
constexpr uint32_t kStateFetch = 0;
constexpr uint32_t kStateFwait = 1;
constexpr uint32_t kStateDwait = 4;

// The CSRs the state snapshot compares.  mcycle is deliberately absent: it
// counts clock cycles, so it is *supposed* to differ between reset lengths and
// is not architectural state that a reset has to make equal
// (docs/validation-plan.md section 3 rule 4).
struct CsrState {
  uint64_t mstatus = 0, misa = 0, medeleg = 0, mideleg = 0, mie = 0, mtvec = 0,
           mscratch = 0, mepc = 0, mcause = 0, mtval = 0, mip = 0, minstret = 0;

  bool operator==(const CsrState& other) const {
    return mstatus == other.mstatus && misa == other.misa &&
           medeleg == other.medeleg && mideleg == other.mideleg && mie == other.mie &&
           mtvec == other.mtvec && mscratch == other.mscratch && mepc == other.mepc &&
           mcause == other.mcause && mtval == other.mtval && mip == other.mip &&
           minstret == other.minstret;
  }
};

std::string DescribeCsr(const CsrState& state) {
  std::ostringstream out;
  out << "mstatus=" << mosaic::Hex(state.mstatus)
      << " misa=" << mosaic::Hex(state.misa)
      << " medeleg=" << mosaic::Hex(state.medeleg)
      << " mideleg=" << mosaic::Hex(state.mideleg)
      << " mie=" << mosaic::Hex(state.mie)
      << " mtvec=" << mosaic::Hex(state.mtvec)
      << " mscratch=" << mosaic::Hex(state.mscratch)
      << " mepc=" << mosaic::Hex(state.mepc)
      << " mcause=" << mosaic::Hex(state.mcause)
      << " mtval=" << mosaic::Hex(state.mtval)
      << " mip=" << mosaic::Hex(state.mip)
      << " minstret=" << state.minstret;
  return out.str();
}

// One architectural event, as the testbench's single-lane producer reports it.
struct Event {
  uint64_t pc = 0, next_pc = 0, rd_value = 0, cause = 0, tval = 0, epc = 0;
  uint64_t store_addr = 0, store_data = 0;
  uint32_t insn = 0, rd = 0, store_size = 0;
  bool has_rd = false, is_trap = false, is_store = false;
};

// The canonical text of a stream.  The physical cycle is deliberately NOT part
// of it: two runs that differ only in reset length must produce identical text,
// and the cycle offset is reported separately.
std::string SerialiseEvents(const std::vector<Event>& events) {
  std::ostringstream out;
  char line[512];
  for (size_t i = 0; i < events.size(); ++i) {
    const Event& e = events[i];
    std::snprintf(line, sizeof(line),
                  "%zu pc=%016llx next=%016llx insn=%08llx rd=%s%02u val=%016llx "
                  "trap=%u cause=%llu tval=%016llx epc=%016llx store=%u "
                  "addr=%016llx data=%016llx size=%u\n",
                  i, static_cast<unsigned long long>(e.pc),
                  static_cast<unsigned long long>(e.next_pc),
                  static_cast<unsigned long long>(e.insn),
                  e.has_rd ? "x" : "-", static_cast<unsigned>(e.rd),
                  static_cast<unsigned long long>(e.rd_value), e.is_trap ? 1u : 0u,
                  static_cast<unsigned long long>(e.cause),
                  static_cast<unsigned long long>(e.tval),
                  static_cast<unsigned long long>(e.epc), e.is_store ? 1u : 0u,
                  static_cast<unsigned long long>(e.store_addr),
                  static_cast<unsigned long long>(e.store_data),
                  static_cast<unsigned>(e.store_size));
    out << line;
  }
  return out.str();
}

// ===========================================================================
// The clock and the DUT
// ===========================================================================
//
// Phase order is the project convention (sim/common/sim_common.h): inputs with
// the clock low, then the rising edge, then the falling edge, one eval per
// phase so no phase is ever observed half-updated.

enum class Release {
  kHighAfterLastEdge,   // rst_i drops while the clock is still high
  kFallingEdge,         // rst_i drops one settled eval later, still high
  kLowBeforeFirstEdge,  // rst_i drops in the low phase before the first live edge
  kLateLowPhase,        // rst_i drops at the end of that low phase
};

const char* ReleaseName(Release release) {
  switch (release) {
    case Release::kHighAfterLastEdge:  return "high-after-last-edge";
    case Release::kFallingEdge:        return "falling-edge";
    case Release::kLowBeforeFirstEdge: return "low-before-first-edge";
    case Release::kLateLowPhase:       return "late-low-phase";
  }
  return "?";
}

enum class Stop {
  kTohost,        // run until the program writes TOHOST (or the edge cap)
  kEdges,         // stop after `stop_edges` live edges
  kFirstFwait,    // stop the moment the core is waiting for an ifetch ack
  kFirstDwait,    // stop the moment the core is waiting for a dmem ack
  kTohostRequest, // stop the moment a store to TOHOST is posted
};

struct RunSpec {
  uint64_t reset_edges = 5;
  Release release = Release::kLowBeforeFirstEdge;
  Stop stop = Stop::kTohost;
  uint64_t stop_edges = 0;
  uint64_t max_live_edges = 20000;
  bool check_during_reset = true;
};

struct RunResult {
  std::vector<Event> events;
  std::string stream;
  std::string outcome = "cycle_limit";
  uint64_t tohost = 0;
  uint64_t uart_writes = 0;
  uint64_t signature[4] = {0, 0, 0, 0};

  // reset, measured
  uint64_t reset_edges = 0;         // rising edges that sampled rst_i = 1
  uint64_t live_edges = 0;          // rising edges after release
  uint64_t first_live_edge = 0;     // absolute physical index, 0 = none
  uint64_t first_event_edge = 0;    // absolute physical index, 0 = no event
  uint64_t live_edges_to_first_event = 0;

  // "no commit and no device write during reset", counted per reset edge
  uint64_t reset_event_violations = 0;
  uint64_t reset_dmem_violations = 0;
  uint64_t reset_pending_violations = 0;
  uint64_t reset_pc_violations = 0;

  // state at the instant of release, and at the end
  uint64_t release_pc = 0, release_state = 0;
  bool release_tohost_written = false;
  bool first_live_tohost_written = false;
  CsrState release_csrs, final_csrs;

  // the environment's transaction ownership at the stop instant
  bool stop_if_pending = false, stop_d_pending = false;
  bool stop_dmem_we = false;
  uint64_t stop_dmem_addr = 0;

  // the synchronous-reset probe: pc sampled before and after rst_i is asserted
  // with no clock edge in between
  uint64_t sync_probe_pc_before = 0, sync_probe_pc_after_assert = 0;
};

class Dut {
 public:
  Dut() { ZeroInputs(); }

  // ---- clock primitives ---------------------------------------------------
  void LowPhase()    { dut_.h_clk = 0; dut_.eval(); }
  void RisingEdge()  { dut_.h_clk = 1; dut_.eval(); ++edges_; }
  void FallingEdge() { dut_.h_clk = 0; dut_.eval(); }

  uint64_t edges() const { return edges_; }

  // ---- harness actions ----------------------------------------------------
  //
  // reset is asserted, never released, by everything except Run(): the image is
  // pushed in and the memory is cleared while the core is held, so no fetch can
  // be lost and no store can happen behind the harness's back.
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

  uint64_t ReadCsr(uint16_t address) {
    dut_.h_dbg_csr_addr = address;
    dut_.eval();
    return dut_.c_dbg_csr_data;
  }

  CsrState ReadCsrState() {
    CsrState state;
    state.mstatus   = ReadCsr(0x300);
    state.misa      = ReadCsr(0x301);
    state.medeleg   = ReadCsr(0x302);
    state.mideleg   = ReadCsr(0x303);
    state.mie       = ReadCsr(0x304);
    state.mtvec     = ReadCsr(0x305);
    state.mscratch  = ReadCsr(0x340);
    state.mepc      = ReadCsr(0x341);
    state.mcause    = ReadCsr(0x342);
    state.mtval     = ReadCsr(0x343);
    state.mip       = ReadCsr(0x344);
    state.minstret  = ReadCsr(0xB02);
    return state;
  }

  uint64_t tohost_value() const { return dut_.h_tohost_value; }
  uint64_t uart_writes() const { return dut_.h_uart_count; }
  bool tohost_written() const { return dut_.h_tohost_written != 0; }
  uint64_t pc() const { return dut_.c_dbg_pc; }

  // Asserts rst_i with the clock low and *without* taking an edge.  Returns the
  // pc before and after, which a synchronous reset must leave equal.
  void AssertResetWithoutEdge(uint64_t* before, uint64_t* after) {
    *before = dut_.c_dbg_pc;
    dut_.h_rst = 1;
    dut_.eval();
    *after = dut_.c_dbg_pc;
  }

  RunResult Run(const RunSpec& spec) {
    RunResult r;
    std::vector<Event> events;

    // ---- reset ------------------------------------------------------------
    HoldReset();
    for (uint64_t i = 0; i < spec.reset_edges; ++i) {
      LowPhase();
      RisingEdge();
      ++r.reset_edges;

      if (spec.check_during_reset) {
        // The machine is held: no architectural event, no data request, and no
        // transaction of the environment's outstanding.  pc and state are
        // checked from the second edge on, because before the first edge the
        // registers hold whatever the host simulator initialised them to, which
        // is not a claim this contract makes.
        if (dut_.c_evt_valid) r.reset_event_violations++;
        if (dut_.c_dmem_req_o) r.reset_dmem_violations++;
        if (dut_.h_if_pending || dut_.h_d_pending) r.reset_pending_violations++;
        if (i > 0 && (dut_.c_dbg_pc != MOSAIC_RESET_VECTOR ||
                      dut_.c_dbg_state != kStateFetch)) {
          r.reset_pc_violations++;
        }
      }

      const bool last = (i + 1 == spec.reset_edges);
      if (last && spec.release == Release::kHighAfterLastEdge) {
        dut_.h_rst = 0;
        dut_.eval();
      } else if (last && spec.release == Release::kFallingEdge) {
        dut_.eval();
        dut_.h_rst = 0;
        dut_.eval();
      }
      FallingEdge();
    }
    if (spec.release == Release::kLowBeforeFirstEdge) {
      dut_.h_rst = 0;
      dut_.eval();
    } else if (spec.release == Release::kLateLowPhase) {
      dut_.eval();
      dut_.eval();
      dut_.h_rst = 0;
      dut_.eval();
    }

    // The clock is low and rst_i is low.  Everything the machine holds now is
    // what the reset put there, before a single live edge has been taken.
    r.release_pc = dut_.c_dbg_pc;
    r.release_state = dut_.c_dbg_state;
    r.release_tohost_written = dut_.h_tohost_written != 0;
    r.release_csrs = ReadCsrState();

    // ---- run --------------------------------------------------------------
    bool stopped = false;
    while (r.live_edges < spec.max_live_edges) {
      LowPhase();
      RisingEdge();
      ++r.live_edges;
      if (r.first_live_edge == 0) {
        r.first_live_edge = edges_;
        r.first_live_tohost_written = dut_.h_tohost_written != 0;
      }
      const bool tohost_now = dut_.h_tohost_written != 0;
      if (dut_.c_evt_valid) {
        if (r.first_event_edge == 0) {
          r.first_event_edge = edges_;
          r.live_edges_to_first_event = r.live_edges;
        }
        events.push_back(CurrentEvent());
      }

      bool stop_now = false;
      switch (spec.stop) {
        case Stop::kEdges:
          stop_now = r.live_edges >= spec.stop_edges;
          break;
        case Stop::kFirstFwait:
          stop_now = dut_.c_dbg_state == kStateFwait;
          break;
        case Stop::kFirstDwait:
          stop_now = dut_.c_dbg_state == kStateDwait;
          break;
        case Stop::kTohostRequest:
          // The instant that matters is the cycle *after* the request is
          // posted, when the memory model has accepted the store and latched
          // its end-of-run signal.  Triggering on the request itself would land
          // one cycle too early, before the model has seen anything, and the
          // abort would then be vacuous.
          stop_now = dut_.h_d_pending != 0 && dut_.c_dmem_we_o &&
                     ((dut_.c_dmem_addr_o & ~UINT64_C(7)) == MOSAIC_TOHOST);
          break;
        case Stop::kTohost:
          stop_now = false;
          break;
      }
      if (stop_now) {
        r.stop_if_pending = dut_.h_if_pending != 0;
        r.stop_d_pending = dut_.h_d_pending != 0;
        r.stop_dmem_we = dut_.c_dmem_we_o != 0;
        r.stop_dmem_addr = dut_.c_dmem_addr_o;
        r.outcome = "aborted";
        FallingEdge();
        stopped = true;
        break;
      }

      FallingEdge();
      // The end-of-run signal is checked after the edge's event is recorded, so
      // the store that ends the run is the last line of the stream.
      if (tohost_now) {
        r.outcome = "tohost";
        stopped = true;
        break;
      }
      if (!InMappedRange(dut_.c_dbg_pc)) {
        r.outcome = "pc_escaped";
        stopped = true;
        break;
      }
    }
    if (!stopped) r.outcome = "cycle_limit";

    r.tohost = dut_.h_tohost_value;
    r.uart_writes = dut_.h_uart_count;
    r.final_csrs = ReadCsrState();
    if (r.outcome != "aborted") {
      for (int i = 0; i < 4; ++i) {
        r.signature[i] = ReadWord(kSigBase + 8 * static_cast<uint64_t>(i));
      }
    }
    r.events = events;
    r.stream = SerialiseEvents(events);
    return r;
  }

 private:
  Event CurrentEvent() {
    Event e;
    e.pc = dut_.c_evt_pc;
    e.next_pc = dut_.c_evt_next_pc;
    e.insn = dut_.c_evt_insn;
    e.has_rd = dut_.c_evt_has_rd != 0;
    e.rd = dut_.c_evt_rd;
    e.rd_value = dut_.c_evt_rd_value;
    e.is_trap = dut_.c_evt_trap != 0;
    e.cause = dut_.c_evt_cause;
    e.tval = dut_.c_evt_tval;
    e.epc = dut_.c_evt_epc;
    e.is_store = dut_.c_evt_is_store != 0;
    e.store_addr = dut_.c_evt_store_addr;
    e.store_data = dut_.c_evt_store_data;
    e.store_size = static_cast<uint32_t>(dut_.c_evt_store_size);
    return e;
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
    dut_.eval();
  }

  Vmosaic_bringup_tb dut_;
  uint64_t edges_ = 0;
};

// ===========================================================================
// main
// ===========================================================================

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
  mosaic::Rng rng(options.seed);

  std::string detail;
  bool passed = true;
  uint64_t total_reset_edges = 0;
  uint64_t total_live_edges = 0;
  uint64_t configs_run = 0;
  uint64_t aborts_run = 0;

  try {
    const std::string repo = FindRepoRoot();

    // ---- 1. configuration --------------------------------------------------
    std::string profile_text, csr_text;
    {
      std::ifstream in(repo + "/config/profiles/p0.json");
      if (!in) Fail("cannot read config/profiles/p0.json");
      std::ostringstream buffer;
      buffer << in.rdbuf();
      profile_text = buffer.str();
    }
    {
      std::ifstream in(repo + "/config/csr/mode_m.json");
      if (!in) Fail("cannot read config/csr/mode_m.json");
      std::ostringstream buffer;
      buffer << in.rdbuf();
      csr_text = buffer.str();
    }

    uint64_t reset_vector = 0, tohost = 0, signature = 0, pass_code = 0;
    auto need = [&](const char* key, uint64_t* out) {
      if (!JsonUint(profile_text, key, out)) {
        Fail(std::string("config/profiles/p0.json has no readable \"") + key + "\"");
      }
    };
    need("reset_vector", &reset_vector);
    need("tohost", &tohost);
    need("signature", &signature);
    need("pass_code", &pass_code);

    reporter.Check(reset_vector == MOSAIC_RESET_VECTOR,
                   "config reset_vector matches build/p0/sim/mosaic_platform.h");
    reporter.Check(tohost == MOSAIC_TOHOST, "config TOHOST matches the generated header");
    reporter.Check(signature == MOSAIC_SIGNATURE_ADDR,
                   "config signature address matches the generated header");
    reporter.Check(kSigBase == signature,
                   "the fixture's signature address is the profile's signature address");
    reporter.Check(kUartThr == MOSAIC_UART_BASE,
                   "the fixture's UART address is the profile's UART base");
    reporter.Check(kScratch >= MOSAIC_RAM_BASE &&
                       kScratch + 8 <= MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE &&
                       kUntouched >= MOSAIC_RAM_BASE &&
                       kUntouched + 8 <= MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE &&
                       kSentinel >= MOSAIC_RAM_BASE &&
                       kSentinel + 8 <= MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE,
                   "every address the fixture uses is inside the profile's RAM region");

    const uint64_t mstatus_reset = CsrResetFromJson(csr_text, "mstatus");
    const uint64_t misa_reset = CsrResetFromJson(csr_text, "misa");
    reporter.Check(mstatus_reset == UINT64_C(0x1800),
                   "config/csr/mode_m.json mstatus reset is 0x1800");
    reporter.Check(misa_reset == UINT64_C(0x8000000000001100),
                   "config/csr/mode_m.json misa reset is 0x8000000000001100");

    // ---- 2. the fixture ----------------------------------------------------
    const std::string fixture = Base64Decode(kFixtureBase64);
    reporter.Check(fixture.size() == kFixtureBytes,
                   "the embedded fixture is " + std::to_string(kFixtureBytes) +
                       " bytes, got " + std::to_string(fixture.size()));
    // The hash is checked as well as printed, so a rebuild that changes the
    // fixture's bytes fails the case by name instead of quietly changing what
    // the reset measurements are about.
    const uint64_t fixture_hash = Fnv1a64(fixture);
    std::printf("  fixture %zu bytes, fnv1a-64 0x%016llx (sha256 in the report)\n",
                fixture.size(), static_cast<unsigned long long>(fixture_hash));
    reporter.Check(fixture_hash == kFixtureFnv1a,
                   "the embedded fixture is the bytes sim/unit/reset_program.S "
                   "produces (fnv1a-64 0x" +
                       mosaic::Hex(kFixtureFnv1a, 1).substr(2) + ", got " +
                       mosaic::Hex(fixture_hash, 1).substr(2) + ")");

    Dut dut;

    // The state a machine is in immediately after a reset, before any live
    // edge: PC at the reset vector, in S_FETCH, and every CSR at its reset
    // value.  This is the independent expectation the reset runs are compared
    // against -- it comes from the configuration and the published contract,
    // not from the DUT's own post-release output.
    CsrState reset_state;
    reset_state.mstatus = mstatus_reset;
    reset_state.misa = misa_reset;

    // ---- 3. the reset-length and release-instant sweep ---------------------
    const uint64_t lengths[] = {1, 2, 5, 17};
    const Release releases[] = {Release::kHighAfterLastEdge, Release::kFallingEdge,
                                Release::kLowBeforeFirstEdge, Release::kLateLowPhase};
    constexpr size_t kReleaseCount = 4;

    std::string reference_stream;
    bool have_reference = false;
    RunResult reference;

    for (const uint64_t length : lengths) {
      for (size_t index = 0; index < kReleaseCount; ++index) {
        const std::string tag = "reset " + std::to_string(length) + " edges, release " +
                                ReleaseName(releases[index]);
        RunSpec spec;
        spec.reset_edges = length;
        spec.release = releases[index];
        spec.max_live_edges = options.max_cycles;

        dut.Clear();
        dut.LoadFixture(fixture);
        const RunResult r = dut.Run(spec);
        total_reset_edges += r.reset_edges;
        total_live_edges += r.live_edges;
        ++configs_run;

        // The same configuration run twice, in the same process, from a fresh
        // clear: the same seed must replay bit-identically.
        dut.Clear();
        dut.LoadFixture(fixture);
        const RunResult again = dut.Run(spec);
        total_reset_edges += again.reset_edges;
        total_live_edges += again.live_edges;
        ++configs_run;

        // -- the machine is held while reset is asserted ----------------------
        reporter.Check(r.reset_edges == length,
                       tag + ": the core sampled rst_i = 1 on exactly " +
                           std::to_string(length) + " rising edges");
        reporter.Check(r.reset_event_violations == 0,
                       tag + ": no architectural event is reported while reset is "
                             "asserted (" + std::to_string(r.reset_event_violations) +
                           " violations over " + std::to_string(r.reset_edges) + " edges)");
        reporter.Check(r.reset_dmem_violations == 0,
                       tag + ": no data request is posted while reset is asserted (" +
                           std::to_string(r.reset_dmem_violations) + " violations)");
        reporter.Check(r.reset_pending_violations == 0,
                       tag + ": the environment holds no outstanding transaction while "
                             "reset is asserted (" +
                           std::to_string(r.reset_pending_violations) + " violations)");
        reporter.Check(r.reset_pc_violations == 0,
                       tag + ": the core sits at the reset vector in S_FETCH on every "
                             "reset edge after the first (" +
                           std::to_string(r.reset_pc_violations) + " violations)");

        // -- the state a reset leaves, before any live edge -------------------
        reporter.Check(r.release_pc == MOSAIC_RESET_VECTOR,
                       tag + ": the PC is the reset vector when reset releases, got " +
                           mosaic::Hex(r.release_pc));
        reporter.Check(r.release_state == kStateFetch,
                       tag + ": the machine is in S_FETCH when reset releases, got state " +
                           std::to_string(r.release_state));
        reporter.Check(r.release_csrs == reset_state,
                       tag + ": every compared CSR holds its reset value when reset "
                             "releases (" + DescribeCsr(r.release_csrs) + ")");
        reporter.Check(!r.release_tohost_written,
                       tag + ": no end-of-run latch survived the reset");

        // -- the program actually ran ----------------------------------------
        reporter.Check(r.outcome == "tohost",
                       tag + ": the program ran to TOHOST (outcome " +
                           r.outcome + ")");
        reporter.Check(!r.events.empty() && r.events.front().pc == MOSAIC_RESET_VECTOR,
                       tag + ": the first architectural event is the instruction at the "
                             "reset vector");
        reporter.Check(r.tohost == pass_code,
                       tag + ": TOHOST is the profile pass code " +
                           std::to_string(pass_code) + ", got " + mosaic::Hex(r.tohost));
        reporter.Check(r.uart_writes == 1,
                       tag + ": the run performs exactly one UART device write, got " +
                           std::to_string(r.uart_writes));
        reporter.Check(r.signature[1] == UINT64_C(0x5a5a5a5a5a5a5a5a),
                       tag + ": the RAM store/load round-trip in the fixture is intact");
        reporter.Check(r.signature[2] == UINT64_C(0x5a5a5a5a5a5a5a5a),
                       tag + ": the mscratch write/read round-trip in the fixture is intact");

        // -- the same configuration replays bit-identically -------------------
        reporter.Check(again.stream == r.stream,
                       tag + ": a second run of the same configuration produces the same "
                             "architectural event stream");
        reporter.Check(again.signature[0] == r.signature[0] &&
                           again.signature[1] == r.signature[1] &&
                           again.signature[2] == r.signature[2] &&
                           again.signature[3] == r.signature[3],
                       tag + ": a second run produces the same signature");
        reporter.Check(again.final_csrs == r.final_csrs,
                       tag + ": a second run leaves the same CSR state");
        reporter.Check(again.live_edges == r.live_edges,
                       tag + ": a second run takes the same number of live clock edges");

        // -- across configurations -------------------------------------------
        if (!have_reference) {
          reference = r;
          reference_stream = r.stream;
          have_reference = true;
        } else {
          reporter.Check(r.stream == reference_stream,
                         tag + ": the architectural event stream is identical to the "
                               "reference configuration's");
          reporter.Check(r.live_edges == reference.live_edges,
                         tag + ": the program takes the same number of live clock edges "
                               "as the reference configuration (" +
                             std::to_string(r.live_edges) + " vs " +
                             std::to_string(reference.live_edges) + ")");
          reporter.Check(r.live_edges_to_first_event == reference.live_edges_to_first_event,
                         tag + ": the first event appears the same number of live edges "
                               "after release as in the reference configuration");
          reporter.Check(r.release_csrs == reference.release_csrs &&
                             r.signature[0] == reference.signature[0] &&
                             r.signature[1] == reference.signature[1] &&
                             r.signature[2] == reference.signature[2] &&
                             r.signature[3] == reference.signature[3] &&
                             r.tohost == reference.tohost,
                         tag + ": the post-run architectural state is identical to the "
                               "reference configuration's");
          reporter.Check(r.final_csrs == reference.final_csrs,
                         tag + ": the post-run CSR state is identical to the reference "
                               "configuration's (" + DescribeCsr(r.final_csrs) + ")");
        }

        std::printf("  %-28s %-11s reset=%2llu live=%4llu events=%3zu tohost=%s "
                    "sig0=%s\n",
                    tag.c_str(), r.outcome.c_str(),
                    static_cast<unsigned long long>(r.reset_edges),
                    static_cast<unsigned long long>(r.live_edges), r.events.size(),
                    mosaic::Hex(r.tohost).c_str(),
                    mosaic::Hex(r.signature[0], 1).c_str());
      }
    }
    // The cycle-count statement the RESULT line carries: the live clock count is
    // a property of the program and the memory latency alone, so it is the same
    // for every reset length and every release instant.
    reporter.Check(reference.live_edges > 0,
                   "the reference run took at least one live clock edge");

    // ---- 4. reset interrupts a run in progress and restarts it -------------
    //
    // Each abort rewinds to a fresh clear+load, runs without a reset until the
    // named transaction state appears, asserts reset at that instant (with the
    // synchronous-reset probe), and then releases.  What must happen next is the
    // whole program, bit for bit, with no end-of-run latch and no outstanding
    // transaction carried across.
    struct AbortPoint {
      const char* name;
      Stop stop;
      uint64_t edges;
    };
    const uint64_t random_edges = 20 + rng.Below(80);
    const AbortPoint abort_points[] = {
        {"waiting for an instruction fetch", Stop::kFirstFwait, 0},
        {"waiting for a data access", Stop::kFirstDwait, 0},
        {"posting the TOHOST store", Stop::kTohostRequest, 0},
        {"after a seeded number of edges", Stop::kEdges, random_edges},
    };

    for (const AbortPoint& point : abort_points) {
      const std::string tag = std::string("abort at ") + point.name;
      dut.Clear();
      dut.LoadFixture(fixture);
      RunSpec spec;
      spec.reset_edges = 5;
      spec.release = Release::kLowBeforeFirstEdge;
      spec.stop = point.stop;
      spec.stop_edges = point.edges;
      spec.max_live_edges = options.max_cycles;
      const RunResult aborted = dut.Run(spec);
      ++aborts_run;
      total_reset_edges += aborted.reset_edges;
      total_live_edges += aborted.live_edges;

      reporter.Check(aborted.outcome == "aborted",
                     tag + ": the run stops at the named transaction state (outcome " +
                         aborted.outcome + ")");
      // Non-vacuity: the abort has to land while the machine really is holding
      // that state, or the restart proves nothing about transactions crossing
      // reset.
      switch (point.stop) {
        case Stop::kFirstFwait:
          reporter.Check(aborted.stop_if_pending,
                         tag + ": an instruction fetch is outstanding at the abort");
          break;
        case Stop::kFirstDwait:
          reporter.Check(aborted.stop_d_pending,
                         tag + ": a data access is outstanding at the abort");
          break;
        case Stop::kTohostRequest:
          reporter.Check(aborted.stop_d_pending && aborted.stop_dmem_we &&
                             (aborted.stop_dmem_addr & ~UINT64_C(7)) ==
                                 MOSAIC_TOHOST,
                         tag + ": the memory model has accepted a store to TOHOST and "
                               "its end-of-run signal is latched when the reset lands");
          break;
        case Stop::kEdges:
          reporter.Check(aborted.live_edges == point.edges,
                         tag + ": the run stops after exactly " +
                             std::to_string(point.edges) + " live edges");
          break;
        case Stop::kTohost:
          break;
      }

      // The synchronous-reset probe: asserting rst_i with the clock low must not
      // change the PC until a rising edge samples it.
      uint64_t pc_before = 0, pc_after_assert = 0;
      dut.AssertResetWithoutEdge(&pc_before, &pc_after_assert);
      reporter.Check(pc_after_assert == pc_before,
                     tag + ": asserting rst_i with no clock edge does not move the PC (" +
                         mosaic::Hex(pc_before) + " -> " + mosaic::Hex(pc_after_assert) +
                         "), so the reset is synchronous");

      // Restart: five reset edges from the state we left, no clear and no
      // reload, so the only thing that can make this run differ from the
      // reference is state that survived the reset.
      RunSpec restart;
      restart.reset_edges = 5;
      restart.release = Release::kLowBeforeFirstEdge;
      restart.stop = Stop::kTohost;
      restart.max_live_edges = options.max_cycles;
      const RunResult restarted = dut.Run(restart);
      total_reset_edges += restarted.reset_edges;
      total_live_edges += restarted.live_edges;

      reporter.Check(restarted.reset_edges == 5,
                     tag + ": the restart takes five reset edges from where it was");
      reporter.Check(!restarted.first_live_tohost_written,
                     tag + ": no end-of-run latch is reported on the first live edge "
                           "after the restart");
      reporter.Check(!restarted.events.empty() &&
                         restarted.events.front().pc == MOSAIC_RESET_VECTOR,
                     tag + ": the restarted run retires the instruction at the reset "
                           "vector first, so it really restarted and did not end early");
      reporter.Check(restarted.stream == reference_stream,
                     tag + ": the restarted run's event stream is identical to a run "
                           "that was never interrupted");
      reporter.Check(restarted.live_edges == reference.live_edges,
                     tag + ": the restarted run takes the same number of live clock "
                           "edges as an uninterrupted run");
      reporter.Check(restarted.signature[0] == reference.signature[0] &&
                         restarted.signature[1] == reference.signature[1] &&
                         restarted.signature[2] == reference.signature[2] &&
                         restarted.signature[3] == reference.signature[3] &&
                         restarted.tohost == reference.tohost,
                     tag + ": the restarted run ends in the same architectural state");
      reporter.Check(restarted.final_csrs == reference.final_csrs,
                     tag + ": the restarted run leaves the same CSR state");

      std::printf("  %-32s aborted after %4llu live edges (if_pending=%d d_pending=%d "
                  "tohost_req=%d), restarted %llu events in %llu edges, "
                  "first_live_tohost=%d, tohost=%s\n",
                  tag.c_str(), static_cast<unsigned long long>(aborted.live_edges),
                  aborted.stop_if_pending ? 1 : 0, aborted.stop_d_pending ? 1 : 0,
                  aborted.stop_dmem_we ? 1 : 0,
                  static_cast<unsigned long long>(restarted.events.size()),
                  static_cast<unsigned long long>(restarted.live_edges),
                  restarted.first_live_tohost_written ? 1 : 0,
                  mosaic::Hex(restarted.tohost).c_str());
    }

    // ---- 5. the SRAM non-reset initialisation contract ---------------------
    //
    // docs/platform-plan.md section 2: the SRAM data array is not fully reset,
    // and only legally initialised or written data may be read.  The consequence
    // for this harness is that "a word nobody wrote reads as zero" is a property
    // of the clear pass, not of the reset and not of the host process -- and
    // that is checkable in both directions.
    const uint64_t sentinel = rng.Next();
    {
      // 5a. reset alone does not re-initialise the array
      dut.Clear();
      dut.LoadFixture(fixture);
      dut.WriteWord(kSentinel, sentinel);
      RunSpec spec;
      spec.reset_edges = 5;
      spec.release = Release::kLowBeforeFirstEdge;
      spec.max_live_edges = options.max_cycles;
      const RunResult r = dut.Run(spec);
      total_reset_edges += r.reset_edges;
      total_live_edges += r.live_edges;
      const uint64_t after = dut.ReadWord(kSentinel);
      reporter.Check(after == sentinel,
                     "the RAM data array is not re-initialised by reset: a word written "
                     "before five reset edges still holds its value after them (expected " +
                         mosaic::Hex(sentinel) + ", got " + mosaic::Hex(after) + ")");

      // 5b. what the program reads from an address nobody wrote, and that the
      // image and the device really are there -- read before the clear, because
      // these are properties of the run that just finished.
      reporter.Check(r.signature[0] == 0,
                     "the fixture reads the defined zero the clear pass left in RAM "
                     "(signature[0], got " + mosaic::Hex(r.signature[0]) + ")");
      const uint64_t untouched = dut.ReadWord(kUntouched);
      reporter.Check(untouched == 0,
                     "a RAM word neither the image nor the program writes reads zero (" +
                         mosaic::Hex(untouched) + ")");
      const uint64_t untouched_again = dut.ReadWord(kUntouched);
      reporter.Check(untouched_again == untouched,
                     "reading that word again gives the same defined value (it is not "
                     "an uninitialised read)");

      uint64_t first_word = 0;
      for (size_t i = 0; i < 8 && i < fixture.size(); ++i) {
        first_word |= static_cast<uint64_t>(static_cast<unsigned char>(fixture[i]))
                      << (8 * i);
      }
      const uint64_t loaded = dut.ReadWord(MOSAIC_RESET_VECTOR);
      reporter.Check(loaded == first_word,
                     "the fixture's first word is in RAM at the reset vector (expected " +
                         mosaic::Hex(first_word) + ", got " + mosaic::Hex(loaded) + ")");
      const uint64_t uart_byte = dut.ReadWord(kUartThr);
      reporter.Check((uart_byte & 0xFF) == 0x41,
                     "the fixture's one UART write reached the device: the UART holds "
                     "'A' (" + mosaic::Hex(uart_byte) + ")");

      // 5c. the clear pass is what makes an unwritten word zero: the word that
      // held the sentinel, and the image itself, both go away with it.
      dut.Clear();
      const uint64_t cleared = dut.ReadWord(kSentinel);
      reporter.Check(cleared == 0,
                     "the harness's clear pass re-initialises the RAM data array: the "
                     "word that held " + mosaic::Hex(sentinel) + " reads " +
                         mosaic::Hex(cleared) + " after it");
      const uint64_t image_gone = dut.ReadWord(MOSAIC_RESET_VECTOR);
      reporter.Check(image_gone == 0,
                     "the clear pass removes the image as well, so the zero at " +
                         mosaic::Hex(kUntouched) + " is the clear pass's doing and not "
                         "an artefact of nothing ever being written (" +
                         mosaic::Hex(image_gone) + ")");

      // 5d. loading the image again puts it back, so 5c is not "the loader is
      // broken and nothing ever lands in RAM".
      dut.Clear();
      dut.LoadFixture(fixture);
      const uint64_t reloaded = dut.ReadWord(MOSAIC_RESET_VECTOR);
      reporter.Check(reloaded == first_word,
                     "loading the fixture again puts its first word back at the reset "
                     "vector (" + mosaic::Hex(reloaded) + ")");
    }

    detail = "reset sweep " + std::to_string(configs_run) + " runs over " +
             std::to_string(sizeof(lengths) / sizeof(lengths[0])) + " reset lengths x " +
             std::to_string(kReleaseCount) + " release instants, " +
             std::to_string(aborts_run) + " aborts, " +
             std::to_string(total_reset_edges) + " reset edges and " +
             std::to_string(total_live_edges) +
             " live edges driven, " + std::to_string(reporter.checks()) +
             " checks, every stream bit-identical";
  } catch (const Failure& failure) {
    passed = false;
    detail = "check failed: " + failure.what;
  }

  const bool ok = passed && (reporter.failures() == 0);
  reporter.Check(ok, "no check failed");
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
