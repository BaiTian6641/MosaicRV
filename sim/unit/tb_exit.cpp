// ============================================================================
// tb_exit.cpp -- CASE=exit.protocol_termination, work package V-012
// (docs/validation-plan.md section 5 V-012, "固定 signature 与终止判断").
//
// WHAT THIS CASE IS
// -----------------
// It is the test of the harness's *termination state machine*: given a program
// that uses the frozen TOHOST/signature protocol, decide -- from the DUT's own
// observable behaviour -- whether the whole protocol completed, whether the
// checker found a mismatch, and whether the required store drain finished.
// Only a run that is clean on all three counts may be reported as exit status
// 0; every way of failing gets its own documented non-zero status, and a
// timeout is never one of the ways a run succeeds.
//
// The DUT is `mosaic_bringup_tb` (the I-008 bring-up core plus its memory
// model), driven through the ports that testbench already publishes.  This file
// is a harness, not RTL, and it is not the processor's testbench: nothing here
// measures the out-of-order fabric.
//
// WHERE THE EXPECTATIONS COME FROM -- AND WHAT IS NOT READ
// ------------------------------------------------------
//   * The addresses (reset vector, TOHOST, signature base, signature width,
//     pass code, RAM bounds) come from config/profiles/p0.json and
//     config/memory/p0.json, re-read at run time and cross-checked against the
//     generated build/p0/sim/mosaic_platform.h.  A literal that stops matching
//     its configuration fails by name before anything runs.
//   * The operand values {a, b, c} live in exactly one place: the scenario table
//     below.  The harness writes them into the program buffer
//     (MOSAIC_SCRATCH_BASE) through the image-loader port before reset is
//     released; they are NOT compiled into the programs, so a program cannot
//     disagree with the table about what it was asked to compute.
//   * The expected signature is the four expressions
//         sig = { a+b, a-b, a^b, (a<<3)^c }
//     evaluated in C++ by this harness from those same operands.  It is a second
//     implementation of the four instructions the programs execute, and it is
//     never read from the DUT, from the DUT's memory, from the ELF image or from
//     any file the DUT writes.  The one thing the case does read back from the
//     DUT is the signature itself, which is what it compares.
//   * The expected *status* of each scenario is part of the scenario's
//     documentation below, not derived from the run: the point of the case is to
//     assert that a given way of using the protocol produces a given class.
//   * The harness never reads the UART and never looks for text anywhere.  A run
//     ends because a non-zero store to the frozen TOHOST address was observed by
//     the memory model, and for no other reason; h_uart_count is recorded (and
//     is zero for every scenario) only to make that statement checkable.
//
// THE TERMINATION STATUS CODES (documented, stable, per class)
// -----------------------------------------------------------
//     0  PASS                   whole protocol completed, no mismatch, drained
//     1  PROGRAM_FAIL           TOHOST written, but the word was not exactly 1
//                               (the frozen rule: PASS is 1, anything else is a
//                               FAIL whose code is bits [63:1])
//     2  TIMEOUT                the cycle budget ran out with no exit and no WFI
//     3  WFI_STALL              WFI was executed and p0 has no wake source
//     4  SIGNATURE_MISMATCH     the four signature words disagree with the model
//     5  SIGNATURE_OVERRUN      a store wrote past the frozen four-word window
//     6  UNDRAINED_STORE        the exit was signalled while a store was still
//                               outstanding, or with the TOHOST store not
//                               observed to retire, or with the data port busy
//     7  IMAGE_REJECTED         the image was refused, or its entry point is not
//                               the profile reset vector
//
// The *process* exit code is the project convention (sim_common.h): 0 when every
// scenario produced exactly its documented status, 1 when any did not (naming
// the scenario, phase and cycle of the first deviation), 2 for a usage error.
//
// SCENARIOS
// ---------
//   x01_normal              completes normally                     -> PASS
//   x01_normal (inputs 2)   same program, different operands       -> PASS
//   x02_fail                explicit FAIL word (code 0x2A)         -> PROGRAM_FAIL
//   x03_spin                never writes TOHOST                    -> TIMEOUT
//   x04_wfi                 WFI, no wake source, parks             -> WFI_STALL
//   x05_signature_mismatch  PASS word, one wrong signature bit     -> SIGNATURE_MISMATCH
//   x06_signature_overrun   a fifth signature word past the window  -> SIGNATURE_OVERRUN
//   x07_pending_store       exit signalled before the last store    -> UNDRAINED_STORE
//   corrupt magic           loader must refuse the image            -> IMAGE_REJECTED
//   wrong entry point       image must be refused before it runs    -> IMAGE_REJECTED
//
// The programs are in tests/programs/exit/ (see exit.h) and are built by
// `make -C tests/programs exit`.  They are deliberately NOT part of the
// p01..p13 firmware corpus: they are not in corpus.json, tools/host_oracle.py
// does not see them, and tests/programs/golden.json is untouched by them.
//
// MUTANTS (the checks are themselves under test)
// ----------------------------------------------
// Each `-D MOSAIC_EXIT_MUTANT_*` below disables exactly one classification, so
// the scenario that names it must then come back with the wrong status and the
// case must fail by name.  A build with one of these defined is built from an
// empty build directory and observed to exit 1; the verbatim commands and
// observed output are in results/reports/V-012-exit.md.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "sim_common.h"
#include "elf_loader.h"

#include "mosaic_platform.h"
#include "Vmosaic_bringup_tb.h"

namespace {

using mosaic::Hex;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& what) { throw Failure{what}; }

// ===========================================================================
// The documented termination classes
// ===========================================================================

enum ExitStatus {
  kExitPass = 0,
  kExitProgramFail = 1,
  kExitTimeout = 2,
  kExitWfiStall = 3,
  kExitSignatureMismatch = 4,
  kExitSignatureOverrun = 5,
  kExitUndrainedStore = 6,
  kExitImageRejected = 7,
};

const char* StatusName(ExitStatus status) {
  switch (status) {
    case kExitPass: return "PASS";
    case kExitProgramFail: return "PROGRAM_FAIL";
    case kExitTimeout: return "TIMEOUT";
    case kExitWfiStall: return "WFI_STALL";
    case kExitSignatureMismatch: return "SIGNATURE_MISMATCH";
    case kExitSignatureOverrun: return "SIGNATURE_OVERRUN";
    case kExitUndrainedStore: return "UNDRAINED_STORE";
    case kExitImageRejected: return "IMAGE_REJECTED";
  }
  return "UNKNOWN";
}

// "TIMEOUT(2)" -- the label always carries the code, so a reader of the report
// can match an observed status to the table above without a lookup.
std::string StatusLabel(ExitStatus status) {
  return std::string(StatusName(status)) + "(" + std::to_string(static_cast<int>(status)) + ")";
}

// ===========================================================================
// Documented, frozen constants of this case
// ===========================================================================

// The I-008 bring-up core's reset contract, as every other case in this tree
// drives it.
constexpr uint64_t kResetCycles = 5;

// Per-scenario cycle budget.  The longest scenario is a single instruction
// after reset (x03/x04 park immediately), so 20,000 cycles is three orders of
// magnitude of margin; it is fixed and documented rather than inherited from
// the runner's (much larger) case budget, because the timeout scenarios have to
// actually reach it.  min(this, --max-cycles) is what is enforced, and the
// value used is printed in the RESULT line.
constexpr uint64_t kExitBudget = 20000;

// The required drain: after the exit pulse, the harness keeps simulating this
// many cycles before reading the signature, and during that window it requires
// (a) no store into the signature window or its guard band, (b) the store that
// wrote TOHOST to be observed to retire, and (c) the hart to leave the
// data-request states.  One store in this memory model takes three cycles from
// request to retire, so 32 cycles is a very wide margin; the window is fixed so
// that a failure is reproducible.
constexpr uint64_t kDrainWindow = 32;

// WFI: SYSTEM, funct12 = 0x105, rd = rs1 = 0.  p0 has no interrupt controller.
constexpr uint32_t kWfiInsn = 0x10500073u;

// The bring-up core's state encoding (dbg_state_o); 3 = S_DREQ, 4 = S_DWAIT.
constexpr uint8_t kStateDataReq = 3;
constexpr uint8_t kStateDataWait = 4;

// Where the harness deposits the scenario operands.  MOSAIC_SCRATCH_BASE is the
// firmware-owned 128-byte program buffer in tests/programs/src/platform.h; no
// exit program uses it for anything else.
constexpr uint64_t kOperandBase = 0x80001080ull;

// ===========================================================================
// Configuration, read from the frozen files
// ===========================================================================

struct Protocol {
  uint64_t reset_vector = 0;
  uint64_t tohost = 0;
  uint64_t fromhost = 0;
  uint64_t signature = 0;
  uint64_t signature_words = 0;
  uint64_t pass_code = 1;
  uint64_t ram_base = 0;
  uint64_t ram_size = 0;

  uint64_t signature_bytes() const { return signature_words * 8; }
  // The signature guard band: the frozen window is four words, and the rest of
  // the 512-byte page that holds it is otherwise unassigned RAM in the frozen
  // map (.scratch starts at 0x80001080, .bss at 0x80001100).  A store that
  // lands there is a signature write that ran past the region.
  uint64_t guard_end() const { return (signature & ~UINT64_C(0x1ff)) + 0x200; }

  bool InRam(uint64_t address) const {
    return address >= ram_base && (address - ram_base) < ram_size;
  }
};

std::string FindRepoRoot() {
  std::string dir = ".";
  for (int depth = 0; depth < 8; ++depth) {
    std::ifstream probe(dir + "/config/profiles/p0.json");
    if (probe) {
      char resolved[4096];
      if (realpath(dir.c_str(), resolved) != nullptr) return std::string(resolved);
      return dir;
    }
    dir += "/..";
  }
  Fail("cannot find the repository root: no config/profiles/p0.json above the "
       "working directory");
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *out = buffer.str();
  return true;
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

// Reads one scalar key out of a flat JSON object, failing by name.
uint64_t Need(const std::string& text, const char* key, const char* file) {
  uint64_t value = 0;
  if (!JsonUint(text, key, &value)) {
    Fail(std::string(file) + " has no readable \"" + key + "\"");
  }
  return value;
}

// Reads one scalar key out of the entry of config/memory/p0.json whose "name"
// is `region`.  A plain JsonUint would find the *first* "base" in the file,
// which belongs to boot_rom and would make every program's PC look unmapped.
uint64_t RegionField(const std::string& text, const char* region, const char* key,
                     const char* file) {
  const std::string needle = "\"name\": \"" + std::string(region) + "\"";
  const size_t at = text.find(needle);
  if (at == std::string::npos) {
    Fail(std::string(file) + " has no region named " + region);
  }
  uint64_t value = 0;
  if (!JsonUint(text.substr(at + needle.size()), key, &value)) {
    Fail(std::string(file) + " region " + region + " has no readable \"" + key + "\"");
  }
  return value;
}

// ===========================================================================
// The expected signature -- the model this case compares against
// ===========================================================================
//
// Four expressions, written from the comment in tests/programs/exit/exit.h.
// Kept deliberately trivial so that the model cannot itself be the thing under
// test: what is under test is the harness's *decision* to compare, not the
// arithmetic.  The programs compute the same four expressions in RISC-V
// assembly from operands the harness supplied.

uint64_t ExpectedSignatureWord(uint64_t a, uint64_t b, uint64_t c, int index) {
  switch (index) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a ^ b;
    case 3: return (a << 3) ^ c;
    default: Fail("signature index out of range");
  }
}

// ===========================================================================
// One architectural event, with the cycle it was observed on
// ===========================================================================
//
// The event tap in sim/common carries no cycle number on purpose (a stream that
// depends on timing cannot be replayed).  This harness needs the cycle for its
// own termination reasoning, so it records it beside the event and never lets
// it into a comparison.

struct Event {
  uint64_t cycle = 0;
  uint64_t pc = 0;
  uint32_t insn = 0;
  bool is_trap = false;
  uint64_t cause = 0;
  bool has_rd = false;
  uint8_t rd = 0;
  uint64_t rd_value = 0;
  bool is_store = false;
  uint64_t store_addr = 0;
  uint64_t store_data = 0;
  uint64_t store_bytes = 0;
};

// ===========================================================================
// The DUT wrapper
// ===========================================================================

class Dut {
 public:
  explicit Dut(mosaic::ClockDriver* clock) : clock_(clock) { ZeroInputs(); }

  Vmosaic_bringup_tb* raw() { return &dut_; }

  // One clock period in the project's phase order: drive with the clock low,
  // rise, settle, fall.  Sampling after this call sees post-edge values.
  void Tick() {
    dut_.h_clk = 0;
    dut_.eval();
    dut_.h_clk = 1;
    dut_.eval();
    clock_->Tick();
    dut_.h_clk = 0;
    dut_.eval();
  }

  void ClearMemory() {
    dut_.h_clear_mem = 1;
    Tick();
    dut_.h_clear_mem = 0;
  }

  void WriteWord(uint64_t address, uint64_t value) {
    dut_.h_img_we = 1;
    dut_.h_img_addr = address;
    dut_.h_img_data = value;
    Tick();
    dut_.h_img_we = 0;
    dut_.h_img_addr = 0;
    dut_.h_img_data = 0;
  }

  // Bytes are accumulated into 8-byte words against a shadow that starts at
  // zero (the model was just cleared), so an unaligned segment is correct
  // without any byte-merge logic.
  void LoadSegment(uint64_t vaddr, const uint8_t* data, size_t size) {
    std::map<uint64_t, uint64_t> words;
    for (size_t i = 0; i < size; ++i) {
      const uint64_t address = vaddr + i;
      words[address & ~UINT64_C(7)] |=
          static_cast<uint64_t>(data[i]) << (8 * (address & 7));
    }
    for (const auto& entry : words) WriteWord(entry.first, entry.second);
  }

  // Reset is held for `cycles` ticks, then released on the falling edge, so the
  // next rising edge is the first one the core sees out of reset.
  void Reset(uint64_t cycles) {
    dut_.h_rst = 1;
    dut_.eval();
    for (uint64_t i = 0; i < cycles; ++i) Tick();
    dut_.h_rst = 0;
    dut_.eval();
  }

  // Two ticks: the readback port answers the cycle after the request.
  uint64_t ReadWord(uint64_t address) {
    dut_.h_rb_req = 1;
    dut_.h_rb_addr = address;
    dut_.eval();
    Tick();
    dut_.h_rb_req = 0;
    dut_.h_rb_addr = 0;
    dut_.eval();
    return dut_.h_rb_data;
  }

  bool evt_valid() const { return dut_.c_evt_valid != 0; }
  bool tohost_written() const { return dut_.h_tohost_written != 0; }
  uint64_t tohost() const { return dut_.h_tohost_value; }
  uint64_t uart_writes() const { return dut_.h_uart_count; }
  uint64_t pc() const { return dut_.c_dbg_pc; }
  uint8_t state() const { return static_cast<uint8_t>(dut_.c_dbg_state); }

  Event Sample() const {
    Event event;
    event.pc = dut_.c_evt_pc;
    event.insn = dut_.c_evt_insn;
    event.is_trap = dut_.c_evt_trap != 0;
    event.cause = dut_.c_evt_cause;
    event.has_rd = dut_.c_evt_has_rd != 0;
    event.rd = static_cast<uint8_t>(dut_.c_evt_rd);
    event.rd_value = dut_.c_evt_rd_value;
    event.is_store = dut_.c_evt_is_store != 0;
    event.store_addr = dut_.c_evt_store_addr;
    event.store_data = dut_.c_evt_store_data;
    // The bring-up core publishes the access width in bytes (size_in_bytes in
    // mosaic_bringup_core.sv), not the encoded SZ_* form.
    event.store_bytes = dut_.c_evt_store_size;
    return event;
  }

 private:
  void ZeroInputs() {
    dut_.h_clk = 0;
    // Reset is asserted from the first tick: the image and the operands are
    // pushed in while the core is held, so nothing executes before the image is
    // complete.
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
  mosaic::ClockDriver* clock_;
};

// ===========================================================================
// The termination state machine
// ===========================================================================

struct ScenarioResult {
  ExitStatus status = kExitTimeout;
  uint64_t code = 0;                 // program-defined FAIL code, when PROGRAM_FAIL
  std::string phase;                 // IMAGE / RUN / DRAIN / EXIT / OVERRUN / SIGNATURE
  std::string detail;                // first failure, named by phase and cycle

  uint64_t cycles = 0;
  uint64_t budget = 0;
  uint64_t exit_pulse_cycle = 0;
  uint64_t exit_retire_cycle = 0;
  uint64_t late_store_cycle = 0;
  uint64_t overrun_cycle = 0;
  uint64_t wfi_cycle = 0;
  uint64_t tohost = 0;
  uint64_t uart_writes = 0;
  uint8_t state_at_drain_end = 0;
  bool wfi_seen = false;
  bool pc_escaped = false;
  bool drain_skipped = false;
  uint64_t signature[4] = {0, 0, 0, 0};
  uint64_t expected_signature[4] = {0, 0, 0, 0};
  std::vector<Event> stream;
};

// True when the store covers any byte of the signature window or its guard band.
bool StoreTouchesBand(const Event& event, const Protocol& proto) {
  if (!event.is_store || event.store_bytes == 0) return false;
  const uint64_t first = proto.signature;
  const uint64_t last = proto.guard_end();  // exclusive
  const uint64_t end = event.store_addr + event.store_bytes;
  return event.store_addr < last && end > first;
}

// True when the store writes inside the band but is not wholly inside the
// frozen four-word window: it either starts in the guard band or runs past the
// window's end.  Both are "the signature write ran past the signature region".
bool StoreOverrunsWindow(const Event& event, const Protocol& proto) {
  if (!StoreTouchesBand(event, proto)) return false;
  const uint64_t window_end = proto.signature + proto.signature_bytes();
  return event.store_addr < proto.signature ||
         (event.store_addr + event.store_bytes) > window_end;
}

// The state machine.  Every path sets `status`, `phase` and `detail`; `detail`
// always names the phase and the cycle, so a failure can be located without a
// waveform.
ScenarioResult RunScenario(Dut* dut, const Protocol& proto, const mosaic::Image& image,
                           uint64_t a, uint64_t b, uint64_t c, uint64_t budget) {
  ScenarioResult result;
  result.budget = budget;

  for (int i = 0; i < 4; ++i) {
    result.expected_signature[i] = ExpectedSignatureWord(a, b, c, i);
  }

  dut->ClearMemory();
  for (const mosaic::Segment& segment : image.segments) {
    dut->LoadSegment(segment.vaddr, segment.data.data(), segment.data.size());
  }
  // The operands: one source of truth, pushed in through the image port.
  dut->WriteWord(kOperandBase + 0, a);
  dut->WriteWord(kOperandBase + 8, b);
  dut->WriteWord(kOperandBase + 16, c);
  dut->Reset(kResetCycles);

  // ---- phase RUN ---------------------------------------------------------
  bool saw_pulse = false;
  uint64_t cycle = 0;
  while (cycle < budget) {
    dut->Tick();
    ++cycle;
    if (dut->evt_valid()) {
      Event event = dut->Sample();
      event.cycle = cycle;
      if (event.insn == kWfiInsn) {
        if (!result.wfi_seen) result.wfi_cycle = cycle;
        result.wfi_seen = true;
      }
      result.stream.push_back(event);
    }
    if (dut->tohost_written()) {
      saw_pulse = true;
      result.exit_pulse_cycle = cycle;
      break;
    }
    if (!proto.InRam(dut->pc())) {
      result.pc_escaped = true;
      break;
    }
  }
  result.cycles = cycle;

  if (!saw_pulse) {
    // The protocol never completed.  The signature is still read out, as
    // evidence that the program did or did not publish one, but it is never
    // compared: a run that never reached TOHOST has no protocol result to
    // check.
    for (uint64_t i = 0; i < proto.signature_words && i < 4; ++i) {
      result.signature[i] = dut->ReadWord(proto.signature + 8 * i);
    }
    result.uart_writes = dut->uart_writes();
    // A WFI with no wake source is a stall with
    // its own class; anything else is a plain budget timeout.  Neither is a
    // pass, and neither can be turned into one.
    if (result.pc_escaped) {
      result.phase = "RUN/PC_ESCAPED";
      result.detail = "phase RUN cycle " + std::to_string(cycle) +
                      ": the hart left mapped RAM at pc " + Hex(dut->pc()) +
                      " before writing TOHOST";
    } else if (result.wfi_seen) {
      result.phase = "RUN/WFI";
      result.detail = "phase RUN cycle " + std::to_string(result.wfi_cycle) +
                      ": WFI executed and p0 has no wake source, so the hart can "
                      "never reach TOHOST (budget " + std::to_string(budget) + ")";
    } else {
      result.phase = "RUN/TIMEOUT";
      result.detail = "phase RUN cycle " + std::to_string(cycle) +
                      ": the cycle budget expired with TOHOST never written";
    }
    result.status = result.wfi_seen ? kExitWfiStall : kExitTimeout;
#ifdef MOSAIC_EXIT_MUTANT_TIMEOUT_IS_PASS
    result.status = kExitPass;
    result.detail += " [MOSAIC_EXIT_MUTANT_TIMEOUT_IS_PASS: a timeout was treated as success]";
#endif
#ifdef MOSAIC_EXIT_MUTANT_WFI_AS_TIMEOUT
    result.status = kExitTimeout;
#endif
    return result;
  }

  // ---- phase DRAIN -------------------------------------------------------
  //
  // The pulse says the memory model accepted a non-zero store to TOHOST.  It
  // does NOT say the hart has retired that store, and it does not say the
  // program has finished writing its signature.  The drain keeps simulating a
  // fixed, documented window and requires all three of:
  //   (a) the store that wrote TOHOST is observed to retire at or after the
  //       pulse (the two are the same cycle in this design, which has no store
  //       buffer; the rule is stated as "at or after" so that it also holds for
  //       a design where they separate),
  //   (b) no store touches the signature window or its guard band,
  //   (c) the hart is out of the data-request states when the window closes.
#ifndef MOSAIC_EXIT_MUTANT_NO_DRAIN
  const uint64_t drain_end = cycle + kDrainWindow;
  while (cycle < drain_end) {
    dut->Tick();
    ++cycle;
    if (dut->evt_valid()) {
      Event event = dut->Sample();
      event.cycle = cycle;
      result.stream.push_back(event);
    }
  }
  result.state_at_drain_end = dut->state();
#else
  result.drain_skipped = true;
  result.state_at_drain_end = dut->state();
#endif
  result.cycles = cycle;

  // The two store facts the drain is about, taken from the whole stream at or
  // after the pulse.  The memory model performs the TOHOST store and raises the
  // pulse on the same edge, so the hart's retirement of that store is observed
  // on the pulse cycle itself in this design: the rule is "observed at or after
  // the pulse", not "strictly later".  In a design with a store buffer the two
  // can separate, which is exactly what the rule has to survive.
  for (const Event& event : result.stream) {
    if (!event.is_store || event.cycle < result.exit_pulse_cycle) continue;
    if (StoreTouchesBand(event, proto) && result.late_store_cycle == 0) {
      result.late_store_cycle = event.cycle;
    }
    if (event.store_addr == proto.tohost && result.exit_retire_cycle == 0) {
      result.exit_retire_cycle = event.cycle;
    }
  }

  // ---- the signature, read only after the drain --------------------------
  for (uint64_t i = 0; i < proto.signature_words && i < 4; ++i) {
    result.signature[i] = dut->ReadWord(proto.signature + 8 * i);
  }
  result.tohost = dut->tohost();
  result.uart_writes = dut->uart_writes();

#ifndef MOSAIC_EXIT_MUTANT_NO_DRAIN
  if (result.late_store_cycle != 0) {
    result.status = kExitUndrainedStore;
    result.phase = "DRAIN/COMPLETENESS";
    result.detail = "phase DRAIN cycle " + std::to_string(result.late_store_cycle) +
                    ": a store to the signature window or its guard band was "
                    "observed at or after the exit pulse at cycle " +
                    std::to_string(result.exit_pulse_cycle) +
                    ", so the window was not complete when the program signalled "
                    "completion";
    return result;
  }
  if (result.exit_retire_cycle == 0) {
    result.status = kExitUndrainedStore;
    result.phase = "DRAIN/ORDERING";
    result.detail = "phase DRAIN cycle " + std::to_string(cycle) +
                    ": the store that wrote TOHOST was never observed to retire "
                    "within the " + std::to_string(kDrainWindow) +
                    "-cycle drain window opened by the pulse at cycle " +
                    std::to_string(result.exit_pulse_cycle);
    return result;
  }
  if (result.state_at_drain_end == kStateDataReq ||
      result.state_at_drain_end == kStateDataWait) {
    result.status = kExitUndrainedStore;
    result.phase = "DRAIN/BUSY";
    result.detail = "phase DRAIN cycle " + std::to_string(cycle) +
                    ": the data port was still busy (dbg_state " +
                    std::to_string(result.state_at_drain_end) +
                    ") when the drain window closed";
    return result;
  }
#endif

  // ---- phase OVERRUN -----------------------------------------------------
  //
  // Checked over the whole stream, not just the drain: a program may write past
  // the frozen window before it signals completion (x06), and that is a
  // protocol violation whether or not the exit itself was clean.
#ifndef MOSAIC_EXIT_MUTANT_NO_OVERRUN_CHECK
  for (const Event& event : result.stream) {
    if (StoreOverrunsWindow(event, proto)) {
      result.overrun_cycle = event.cycle;
      break;
    }
  }
  if (result.overrun_cycle != 0) {
    result.status = kExitSignatureOverrun;
    result.phase = "OVERRUN";
    // Name the offending store exactly.
    for (const Event& event : result.stream) {
      if (StoreOverrunsWindow(event, proto)) {
        result.detail = "phase OVERRUN cycle " + std::to_string(event.cycle) +
                        ": a " + std::to_string(event.store_bytes) +
                        "-byte store at " + Hex(event.store_addr) +
                        " leaves the frozen signature window [" +
                        Hex(proto.signature) + ", " +
                        Hex(proto.signature + proto.signature_bytes()) + ")";
        break;
      }
    }
    return result;
  }
#endif

  // ---- phase EXIT --------------------------------------------------------
  //
  // The frozen rule (tests/programs/src/platform.h): TOHOST == 1 is PASS; any
  // other non-zero value is a FAIL whose code is bits [63:1].  The memory model
  // only raises the pulse for a non-zero write, so a value of 0 here would mean
  // the harness's own understanding of the pulse is wrong.
#ifndef MOSAIC_EXIT_MUTANT_FAIL_IS_PASS
  if (result.tohost != proto.pass_code) {
    result.status = kExitProgramFail;
    result.code = result.tohost >> 1;
    result.phase = "EXIT/PROGRAM_FAIL";
    result.detail = "phase EXIT cycle " + std::to_string(result.exit_pulse_cycle) +
                    ": the program wrote " + Hex(result.tohost) +
                    " to TOHOST; the frozen protocol accepts only " +
                    Hex(proto.pass_code) + " as PASS, so this is FAIL with code " +
                    Hex(result.code);
    return result;
  }
#endif

  // ---- phase SIGNATURE ---------------------------------------------------
#ifndef MOSAIC_EXIT_MUTANT_NO_SIGNATURE_CHECK
  for (int i = 0; i < 4; ++i) {
    if (result.signature[i] != result.expected_signature[i]) {
      result.status = kExitSignatureMismatch;
      result.phase = "SIGNATURE";
      result.detail = "phase SIGNATURE cycle " + std::to_string(result.cycles) +
                      ": signature word " + std::to_string(i) + " is " +
                      Hex(result.signature[i]) + ", the model says " +
                      Hex(result.expected_signature[i]);
      return result;
    }
  }
#endif

  result.status = kExitPass;
  result.phase = "COMPLETE";
  result.detail = "protocol complete at cycle " +
                  std::to_string(result.exit_pulse_cycle) +
                  ", drained at " + std::to_string(result.cycles) +
                  ", signature matches the model";
  return result;
}

// ===========================================================================
// Scenarios
// ===========================================================================

struct Scenario {
  const char* name;
  const char* elf;      // built into tests/programs/build/exit/
  uint64_t a, b, c;
  ExitStatus expected;
  uint64_t expected_code;
  bool check_signature;  // the run completed the protocol, so its signature is evidence
};

const Scenario kScenarios[] = {
    {"x01_normal", "x01_normal.elf", UINT64_C(0x0123456789ABCDEF),
     UINT64_C(0xFEDCBA9876543212), UINT64_C(0x0D), kExitPass, 0, true},
    {"x02_fail", "x02_fail.elf", UINT64_C(0x7FFFFFFFFFFFFFFF),
     UINT64_C(0x0000000000000001), UINT64_C(0x01), kExitProgramFail, 0x2A, true},
    {"x03_spin", "x03_spin.elf", UINT64_C(0x8000000000000000),
     UINT64_C(0xFFFFFFFFFFFFFFFE), UINT64_C(0x3F), kExitTimeout, 0, false},
    {"x04_wfi", "x04_wfi.elf", UINT64_C(0xDEADBEEFCAFEF00D),
     UINT64_C(0x1234567890ABCDEF), UINT64_C(0x2A), kExitWfiStall, 0, false},
    {"x05_signature_mismatch", "x05_signature_mismatch.elf",
     UINT64_C(0x0F0F0F0F0F0F0F0F), UINT64_C(0xF0F0F0F0F0F0F0F0),
     UINT64_C(0x07), kExitSignatureMismatch, 0, false},
    {"x06_signature_overrun", "x06_signature_overrun.elf",
     UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222),
     UINT64_C(0x33), kExitSignatureOverrun, 0, false},
    {"x07_pending_store", "x07_pending_store.elf", UINT64_C(0xAAAAAAAAAAAAAAAA),
     UINT64_C(0x5555555555555555), UINT64_C(0x1F), kExitUndrainedStore, 0, false},
};

// The same program with a different operand triple: the signature must change
// with the operands, which shows the comparison is not wired to a constant.
const Scenario kSecondInputs = {"x01_normal(second inputs)", "x01_normal.elf",
                                UINT64_C(0xFFFF0000FFFF0000), UINT64_C(0x0000FFFF0000FFFF),
                                UINT64_C(0x11), kExitPass, 0, true};

// ===========================================================================

// The runner creates the output directory before it starts a case; a by-hand
// run may not.  A silently unwritable output directory would throw away the
// evidence, so it is asked for here; EEXIST is the normal case.
void EnsureOutDir(const std::string& path) {
  if (!path.empty()) mkdir(path.c_str(), 0777);
}

void SaveStream(const std::string& path, const ScenarioResult& result) {
  std::ofstream out(path);
  if (!out) return;
  out << "# cycle pc insn trap cause store_addr store_bytes rd value\n";
  for (const Event& event : result.stream) {
    out << event.cycle << " " << Hex(event.pc) << " " << Hex(event.insn, 8) << " "
        << (event.is_trap ? 1 : 0) << " " << event.cause << " "
        << (event.is_store ? Hex(event.store_addr) : std::string("-")) << " "
        << (event.is_store ? std::to_string(event.store_bytes) : std::string("-")) << " "
        << (event.has_rd ? std::to_string(event.rd) : std::string("-")) << " "
        << (event.has_rd ? Hex(event.rd_value) : std::string("-")) << "\n";
  }
}

std::string SignatureText(const uint64_t words[4]) {
  std::ostringstream text;
  for (int i = 0; i < 4; ++i) {
    if (i != 0) text << ",";
    text << Hex(words[i], 1);
  }
  return text.str();
}

mosaic::Image LoadExitImage(const std::string& path) {
  mosaic::Image image;
  std::string detail;
  const mosaic::LoadStatus status = mosaic::LoadElf(path, &image, &detail);
  if (status != mosaic::LoadStatus::kOk) {
    Fail(path + ": " + mosaic::LoadStatusName(status) + " -- " + detail);
  }
  return image;
}

}  // namespace

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "usage error: %s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  if (options.case_id != "exit.protocol_termination") {
    std::fprintf(stderr, "unknown case %s\n", options.case_id.c_str());
    return mosaic::kExitUsage;
  }

  EnsureOutDir(options.out_dir);
  mosaic::Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());

  const uint64_t budget = std::min(options.max_cycles, kExitBudget);
  int scenarios = 0;
  int matched = 0;
  uint64_t total_cycles = 0;
  bool codes_seen[8] = {false, false, false, false, false, false, false, false};
  std::vector<std::string> table;

  try {
    const std::string repo = FindRepoRoot();

    // ---- 1. configuration ------------------------------------------------
    std::string profile_text, memory_text;
    if (!ReadWholeFile(repo + "/config/profiles/p0.json", &profile_text)) {
      Fail("cannot read config/profiles/p0.json");
    }
    if (!ReadWholeFile(repo + "/config/memory/p0.json", &memory_text)) {
      Fail("cannot read config/memory/p0.json");
    }

    Protocol proto;
    proto.reset_vector = Need(profile_text, "reset_vector", "config/profiles/p0.json");
    proto.tohost = Need(profile_text, "tohost", "config/profiles/p0.json");
    proto.fromhost = Need(profile_text, "fromhost", "config/profiles/p0.json");
    proto.signature = Need(profile_text, "signature", "config/profiles/p0.json");
    proto.signature_words =
        Need(profile_text, "signature_words", "config/profiles/p0.json");
    proto.pass_code = Need(profile_text, "pass_code", "config/profiles/p0.json");
    proto.ram_base = RegionField(memory_text, "ram", "base", "config/memory/p0.json");
    proto.ram_size = RegionField(memory_text, "ram", "size", "config/memory/p0.json");

    reporter.Check(proto.reset_vector == MOSAIC_RESET_VECTOR,
                   "config reset_vector matches build/p0/sim/mosaic_platform.h");
    reporter.Check(proto.tohost == MOSAIC_TOHOST,
                   "config TOHOST matches the generated header");
    reporter.Check(proto.fromhost == MOSAIC_FROMHOST,
                   "config FROMHOST matches the generated header");
    reporter.Check(proto.signature == MOSAIC_SIGNATURE_ADDR,
                   "config signature address matches the generated header");
    reporter.Check(proto.signature_words == MOSAIC_SIGNATURE_WORDS,
                   "config signature word count matches the generated header");
    reporter.Check(proto.pass_code == MOSAIC_TEST_PASS_CODE,
                   "config pass_code matches the generated header");
    reporter.Check(proto.ram_base == MOSAIC_RAM_BASE && proto.ram_size == MOSAIC_RAM_SIZE,
                   "config RAM matches the generated header");
    reporter.Check(proto.guard_end() <= proto.ram_base + proto.ram_size,
                   "the signature guard band lies inside RAM");
    reporter.Check(kOperandBase >= proto.ram_base &&
                       kOperandBase + 24 <= proto.ram_base + proto.ram_size,
                   "the operand area lies inside RAM");

    // ---- 2. the exit programs -------------------------------------------
    const std::string exit_dir = repo + "/tests/programs/build/exit";
    for (const Scenario& scenario : kScenarios) {
      std::ifstream probe(exit_dir + "/" + scenario.elf);
      if (!probe) {
        Fail("no " + std::string(scenario.elf) + " in " + exit_dir +
             "; build the exit programs first with `make -C tests/programs exit`");
      }
    }

    mosaic::ClockDriver clock;
    Dut dut(&clock);
    uint64_t first_signature[4] = {0, 0, 0, 0};

    // ---- 3. every scenario ----------------------------------------------
    for (const Scenario& scenario : kScenarios) {
      const std::string name = scenario.name;
      const std::string path = exit_dir + "/" + scenario.elf;
      mosaic::Image image = LoadExitImage(path);

      // IMAGE phase: the entry point must be the profile's reset vector.  A
      // well-formed ELF that starts somewhere else is refused by name before a
      // single instruction runs.
      ScenarioResult result;
      if (image.entry != proto.reset_vector) {
        result.status = kExitImageRejected;
        result.phase = "IMAGE/ENTRY";
        result.detail = "phase IMAGE: entry point " + Hex(image.entry) +
                        " is not the profile reset vector " + Hex(proto.reset_vector);
      } else {
        result = RunScenario(&dut, proto, image, scenario.a, scenario.b, scenario.c, budget);
      }

      ++scenarios;
      total_cycles += result.cycles;
      codes_seen[static_cast<int>(result.status)] = true;

      std::ostringstream line;
      line << "  " << scenario.name << " status=" << StatusLabel(result.status)
           << " expected=" << StatusLabel(scenario.expected)
           << " cycles=" << result.cycles << " phase=" << result.phase
           << " tohost=" << Hex(result.tohost)
           << " sig=" << SignatureText(result.signature)
           << " want=" << SignatureText(result.expected_signature)
           << " uart=" << result.uart_writes;
      table.push_back(line.str());
      std::printf("%s\n", line.str().c_str());
      SaveStream(options.out_dir + "/" + name + ".events.txt", result);

      // The status is the assertion.  Everything else in the line is evidence.
      if (result.status != scenario.expected) {
        reporter.Mismatch(name + " termination status",
                          StatusLabel(scenario.expected), StatusLabel(result.status));
        Fail(name + ": expected " + StatusLabel(scenario.expected) +
             ", observed " + StatusLabel(result.status) + " -- " + result.detail);
      }
      if (scenario.expected == kExitProgramFail && result.code != scenario.expected_code) {
        reporter.Mismatch(name + " program code",
                          Hex(scenario.expected_code), Hex(result.code));
        Fail(name + ": expected program code " +
             Hex(scenario.expected_code) + ", observed " + Hex(result.code));
      }
      if (scenario.check_signature) {
        for (int i = 0; i < 4; ++i) {
          if (result.signature[i] != result.expected_signature[i]) {
            reporter.Mismatch(name + " signature word " + std::to_string(i),
                              Hex(result.expected_signature[i]),
                              Hex(result.signature[i]));
            Fail(name + ": signature word " + std::to_string(i) +
                 " disagrees with the model");
          }
        }
        reporter.Check(result.cycles > 0, name + " ran");
      }
      reporter.Check(result.uart_writes == 0,
                     name + ": the termination decision did not come from UART text");
      if (std::string(scenario.name) == "x01_normal") {
        for (int i = 0; i < 4; ++i) first_signature[i] = result.signature[i];
      }
      ++matched;
    }

    // ---- 4. input sensitivity -------------------------------------------
    //
    // The same program with different operands must produce a different
    // signature.  A comparison wired to a constant would pass x01 either way;
    // this shows the signature follows the operands the harness supplied.
    {
      mosaic::Image image = LoadExitImage(exit_dir + "/" + std::string(kSecondInputs.elf));
      const ScenarioResult result = RunScenario(&dut, proto, image, kSecondInputs.a,
                                                kSecondInputs.b, kSecondInputs.c, budget);
      ++scenarios;
      total_cycles += result.cycles;
      codes_seen[static_cast<int>(result.status)] = true;

      std::ostringstream line;
      line << "  " << kSecondInputs.name << " status=" << StatusLabel(result.status)
           << " expected=" << StatusLabel(kSecondInputs.expected)
           << " cycles=" << result.cycles << " phase=" << result.phase
           << " tohost=" << Hex(result.tohost)
           << " sig=" << SignatureText(result.signature)
           << " want=" << SignatureText(result.expected_signature);
      table.push_back(line.str());
      std::printf("%s\n", line.str().c_str());
      SaveStream(options.out_dir + "/x01_normal_inputs2.events.txt", result);

      if (result.status != kSecondInputs.expected) {
        reporter.Mismatch("x01_normal(second inputs) termination status",
                          StatusLabel(kSecondInputs.expected), StatusLabel(result.status));
        Fail("x01_normal(second inputs): expected " + StatusLabel(kSecondInputs.expected) +
             ", observed " + StatusLabel(result.status) + " -- " + result.detail);
      }
      for (int i = 0; i < 4; ++i) {
        if (result.signature[i] != result.expected_signature[i]) {
          reporter.Mismatch("x01_normal(second inputs) signature word " + std::to_string(i),
                            Hex(result.expected_signature[i]), Hex(result.signature[i]));
          Fail("x01_normal(second inputs): signature word " + std::to_string(i) +
               " disagrees with the model");
        }
      }
      // The comparison is not wired to a constant: different operands, a
      // different signature, both checked against the model.
      bool differs = false;
      for (int i = 0; i < 4; ++i) {
        if (result.signature[i] != first_signature[i]) differs = true;
      }
      reporter.Check(differs,
                     "x01_normal with different operands publishes a different "
                     "signature, so the comparison follows the operands");
      ++matched;
    }

    // ---- 5. image controls ----------------------------------------------
    //
    // Both of these must be refused *before* anything runs: a corrupted ELF by
    // the loader, and a well-formed ELF whose entry point is not the reset
    // vector by the IMAGE phase above.
    {
      std::string image_bytes;
      if (!ReadWholeFile(exit_dir + "/x01_normal.elf", &image_bytes)) {
        Fail("cannot read the x01_normal image for the corrupted-image controls");
      }

      struct Control {
        const char* name;
        std::string bytes;
        const char* expected_reason;
      };
      std::vector<Control> controls;

      std::string bad_magic = image_bytes;
      bad_magic[0] = 'X';
      controls.push_back({"corrupt magic", bad_magic, "the loader refuses the image"});

      std::string wrong_entry = image_bytes;
      // e_entry is the 8-byte little-endian field at offset 24.  0x80000004 is
      // inside the text segment, so the image is structurally valid and it is
      // the reset-vector rule that must catch it.
      const uint64_t entry = UINT64_C(0x80000004);
      for (int i = 0; i < 8; ++i) {
        wrong_entry[24 + i] = static_cast<char>((entry >> (8 * i)) & 0xff);
      }
      controls.push_back({"wrong entry point", wrong_entry,
                          "the image phase refuses the entry point"});

      for (const Control& control : controls) {
        const std::string path = options.out_dir + "/control_" + control.name + ".elf";
        std::ofstream out(path, std::ios::binary);
        if (!out) Fail("cannot write " + path);
        out.write(control.bytes.data(), static_cast<std::streamsize>(control.bytes.size()));
        out.close();

        ScenarioResult result;
        mosaic::Image image;
        std::string load_detail;
        const mosaic::LoadStatus status = mosaic::LoadElf(path, &image, &load_detail);
        if (status != mosaic::LoadStatus::kOk) {
          result.status = kExitImageRejected;
          result.phase = "IMAGE/LOADER";
          result.detail = std::string(mosaic::LoadStatusName(status)) + ": " + load_detail;
        } else if (image.entry != proto.reset_vector) {
          result.status = kExitImageRejected;
          result.phase = "IMAGE/ENTRY";
          result.detail = "entry point " + Hex(image.entry) +
                          " is not the profile reset vector " + Hex(proto.reset_vector);
        } else {
          // The control failed to be a control: the image was accepted.
          result = RunScenario(&dut, proto, image, kScenarios[0].a, kScenarios[0].b,
                               kScenarios[0].c, budget);
        }

        ++scenarios;
        codes_seen[static_cast<int>(result.status)] = true;
        std::ostringstream line;
        line << "  control:" << control.name << " status=" << StatusLabel(result.status)
             << " expected=" << StatusLabel(kExitImageRejected) << " phase="
             << result.phase << " (" << result.detail << ")";
        table.push_back(line.str());
        std::printf("%s\n", line.str().c_str());

        if (result.status != kExitImageRejected) {
          reporter.Mismatch(std::string("control:") + control.name, "IMAGE_REJECTED(7)",
                            StatusLabel(result.status));
          Fail(std::string("control:") + control.name +
               " was not refused before it ran: " + result.detail);
        }
        ++matched;
      }
    }

    // ---- 6. the classes are distinct ------------------------------------
    //
    // Every documented non-zero class is produced by exactly the scenario that
    // names it, and the set of observed classes is exactly the documented set:
    // a single catch-all failure could not have produced this table.
    for (int i = 0; i < 8; ++i) {
      reporter.Check(codes_seen[i],
                     std::string("exit class ") + StatusLabel(static_cast<ExitStatus>(i)) +
                         " was produced by the scenario that documents it");
    }

    // ---- 7. summary -----------------------------------------------------
    std::ostringstream summary;
    summary << "MosaicRV V-012 exit.protocol_termination\n"
            << "profile " << MOSAIC_PROFILE_NAME << ", DUT mosaic_bringup_tb\n"
            << "reset_vector " << Hex(proto.reset_vector) << ", TOHOST "
            << Hex(proto.tohost) << ", signature " << Hex(proto.signature) << " x "
            << proto.signature_words << " words, guard band ends "
            << Hex(proto.guard_end()) << "\n"
            << "budget " << budget << " cycles/scenario, drain window "
            << kDrainWindow << " cycles\n"
            << "operands " << Hex(kOperandBase) << " (written by the harness)\n";
    for (const std::string& line : table) summary << line << "\n";
    summary << "checks " << reporter.checks() << ", failures " << reporter.failures()
            << ", total cycles " << total_cycles << "\n";
    std::ofstream out(options.out_dir + "/summary.txt");
    if (out) out << summary.str();

    if (matched != scenarios) {
      Fail("only " + std::to_string(matched) + " of " + std::to_string(scenarios) +
           " scenarios produced their documented status");
    }

    std::ostringstream detail;
    detail << scenarios << "/" << scenarios << " scenarios matched their documented "
           << "status, 8 distinct exit classes, checks=" << reporter.checks()
           << " cycles=" << total_cycles << " budget=" << budget
           << " drain=" << kDrainWindow;
    return reporter.Finish("PASS", detail.str());
  } catch (const Failure& failure) {
    std::ostringstream summary;
    for (const std::string& line : table) summary << line << "\n";
    std::printf("%s", summary.str().c_str());
    return reporter.Finish("FAIL", failure.what);
  }
}
