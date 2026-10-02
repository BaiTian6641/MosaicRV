// ============================================================================
// tb_core_sweep.cpp -- CASE=core.corpus_sweep, work package I-023.
//
// The whole corpus, on the out-of-order core, against the host oracle.
//
// The other corpus cases each run a handful of programs: CASE=core.corpus_branch
// runs p02 through the control path, CASE=core.mem_program runs p03/p09 through
// the memory path, CASE=core.trap_csr_program runs p08/p13 through the trap
// path. This case closes the gap: it loads **every** (program, input) image the
// corpus builds -- `tests/programs/build/<program>.iN.elf`, one ELF per declared
// input pattern -- and runs each from the profile's reset vector through the
// same integrated core, then reads the program's four signature words out of
// the memory model and compares them with the value `tools/host_oracle.py`
// computes on the host.
//
// ------------------------------------------------------- where the truth comes from
//
// The expectation is the host oracle's, and the driver invokes the oracle as a
// subprocess (`python3 tools/host_oracle.py --all`) at the start of every run so
// the numbers are the oracle's live arithmetic, not a transcription that could
// have drifted from it. That invocation is also the oracle's own self-check:
// `--all` exits 0 only when the oracle agrees with the corpus declaration
// (`corpus.json` -> `expect_sig`/`expect_traps`), with the recorded golden file
// (`golden.json`, which only `--record` writes), and with itself about input
// sensitivity (every program's three inputs must give a distinct signature). A
// non-zero exit fails this case, because then the thing being compared against
// is not the oracle.
//
// The DUT is compared against that expectation word by word, never against
// itself: nothing the machine retires or writes is used to build the expected
// value.
//
// ------------------------------------------------- the corpus programs the driver knows about
//
// The program list and the input patterns are not hardcoded here. They come
// from the oracle's own table, which is built from `tests/programs/corpus.json`;
// the ELF path follows the corpus's own `<program>.iN.elf` naming. Adding a
// program to the corpus therefore adds it to this sweep.
//
// Two programs' published signatures are not the oracle's row, for reasons that
// are properties of the *program text* and are stated and checked here rather
// than absorbed silently (the same two facts CASE=core.trap_csr_program records):
//
//   * `p08_misaligned` folds MOSAIC_TRAPLOG_RECORDS = 8 trap-log records, not
//     the five the oracle predicts, and its low two bits are its own canary
//     verdict. Its program value is `((oracle_sig3 & ~3) << 24) | bits`.
//   * `p13_romstore` folds with the written record count (which the oracle
//     models) but XORs in a marker set on the instruction *after* the armed
//     store -- which `trap.S`'s `mepc + 4` recovery does execute -- so its
//     program value is `oracle_sig3 ^ 1`.
//
// In both cases the driver also runs an independent host interpretation of the
// loaded image (`sim/unit/trap_ref.h`: an RV64IM_Zicsr interpreter that models
// the p0 trap policy and is given its own memory) and requires it to produce the
// same derived value. The relation is therefore checked, not asserted, and the
// oracle remains the expectation.
//
// ------------------------------------------------------- what a run has to reach
//
// A run is over when the program writes its own TOHOST (the frozen exit
// protocol, handled inside the shared memory model), the machine refuses an
// instruction and stops (an illegal decode, or an ECALL/EBREAK with no trap
// vector yet installed), it stalls, or it runs past the cycle bound. A stop, a
// stall or a timeout is reported with the instruction, the PC and the cycle, so
// a program that cannot run is named rather than skipped. The case fails if any
// (program, input) fails, stops, stalls or times out.
//
// ------------------------------------------------------- what it also checks
//
// Every cycle of every run holds the same invariants the other core cases
// assert: the divergent-recovery counters stay zero, ROB occupancy stays within
// the ROB, the load queue's forwarding-query cross-check stays zero, and the
// store queue's authorised prefix never exceeds its occupancy. A run that trips
// one fails where it happened rather than only at the signature.
//
// `SWEEP_DEBUG=1` prints, per run, the independent interpretation's signature
// beside the oracle-derived expectation and a byte-for-byte diff of the
// signature, scratch and program-input areas between the DUT's memory model and
// the reference's.
//
// --------------------------------------------------------------- controls
//
// Three command-line controls exist so the failure path is evidence, not an
// assertion (they are not used by the registered run):
//
//   `--control-expect <program> <input> <word>`
//       perturbs the oracle-derived expectation for one case by one bit.
//   `--control-input <program> <input>`
//       seeds the DUT's FROMHOST register with `a ^ 1`, so crt0 boots the
//       program on a different input than the one the oracle's row describes.
//   `--control-illegal <program> <input>`
//       patches a zero word (an illegal encoding) over the image's reset vector
//       in the driver's copy only, so the STOP diagnosis path is shown to fire
//       and name the instruction, the PC and the cycle.
//
// All three must make the case fail and name the right location;
// results/reports/I-023-sweep.md records what they produced.
//
// `--only <program>` restricts the sweep to one program, for iteration.
// ============================================================================

#include <verilated.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vmosaic_core_tb.h"

#include "elf_loader.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"
#include "trap_ref.h"

using mosaic_ref::DataMem;

namespace {

// Reset is held for this many rising edges before the machine is considered
// alive; the same number every other core case uses.
constexpr int kResetCycles = 4;
// After the program writes TOHOST, a few cycles so the counters that describe
// the last recovery/authorisation land before they are read.
constexpr int kSettleCycles = 8;
// No commit and no allocation for this long is a hang with a location, not a
// timeout. crt0 zeroes ~17 KiB of stack in a store loop, so forward progress is
// visible almost every cycle when the machine is healthy.
constexpr uint64_t kStallCycles = 50000;

// A bound on the delivered-instruction stream the diagnostics keep. The corpus
// runs deliver ~71k instructions; the cap only stops a runaway from growing the
// log without bound.
constexpr size_t kMaxDeliveries = 400000;

// The corpus's own constant, from tests/programs/src/platform.h. Only p08's
// canary verdict uses it.
constexpr uint64_t kCanaryConstant = UINT64_C(0x5a5a5a5a5a5a5a5a);

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// The loaded program image
// ============================================================================
// An ELF's loadable words, exactly as CASE=core.trap_csr_program builds them.
// Instruction memory answers ECALL (0x00000073) for an address the image does
// not cover, so a runaway fetch reaches a system instruction rather than a
// don't-care: with a trap vector installed that is a trap, and the run's bound
// or the comparison names it.
class ProgImage {
 public:
  bool LoadElf(const std::string& path, std::string* detail) {
    mosaic::Image image;
    const mosaic::LoadStatus status = mosaic::LoadElf(path, &image, detail);
    if (status != mosaic::LoadStatus::kOk) {
      *detail = std::string("ELF refused: ") + mosaic::LoadStatusName(status) + ": " +
                *detail;
      return false;
    }
    elf_ = image;
    entry_ = image.entry;
    for (const mosaic::Segment& seg : image.segments) {
      for (uint64_t off = 0; off + 4 <= seg.memsz; off += 4) {
        uint32_t word = 0;
        for (uint64_t b = 0; b < 4; b++) {
          if (off + b < seg.filesz) {
            word |= static_cast<uint32_t>(seg.data[off + b]) << (8 * b);
          }
        }
        words_[seg.vaddr + off] = word;
      }
    }
    return !words_.empty();
  }

  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x00000073u : it->second;
  }

  // Used only by the `--control-illegal` control, which plants a zero word (an
  // illegal encoding in RISC-V) so the STOP diagnosis path can be shown to fire.
  void Patch(uint64_t addr, uint32_t word) { words_[addr] = word; }

  uint64_t entry() const { return entry_; }
  const mosaic::Image& elf() const { return elf_; }

 private:
  std::map<uint64_t, uint32_t> words_;
  mosaic::Image elf_;
  uint64_t entry_ = 0;
};

// ============================================================================
// The instruction memory
// ============================================================================
// The endpoint's protocol, one outstanding request at a time, with the
// response presented until the fetch takes it (the same model every core case
// uses, so "cycle N" means the same thing here).
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };

  explicit Imem(const ProgImage* img) : img_(img) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
    accepted_ = 0;
  }

  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint64_t ResponseWord() const { return img_->Word(ready_.front().addr); }
  uint64_t accepted() const { return accepted_; }

  void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
    Request r;
    r.addr = addr;
    r.id = id;
    r.epoch = epoch;
    inflight_.push_back(Entry{r, 1});
    accepted_++;
  }

  void PopResponse() { ready_.pop_front(); }

  void Advance() {
    for (size_t i = 0; i < inflight_.size();) {
      if (--inflight_[i].left == 0) {
        ready_.push_back(inflight_[i].req);
        inflight_.erase(inflight_.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
  }

 private:
  struct Entry {
    Request req;
    int left = 0;
  };
  const ProgImage* img_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
  uint64_t accepted_ = 0;
};

// ============================================================================
// Geometry, read from the elaborated DUT rather than re-derived
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

// Verilator hands a wide port over as a `VlWide` indexed in 32-bit words, two per
// 64-bit lane; narrow ports arrive as plain scalars (the convention every core
// case uses).
template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return static_cast<uint64_t>(wide[lane * 2]) |
         (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
}
[[maybe_unused]] inline uint64_t PayloadLane(uint64_t value, uint32_t /*lane*/) {
  return value;
}

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
  Fail("setup", "cannot find the repository root: no config/profiles/p0.json above the "
                "working directory");
}

// ============================================================================
// The host oracle, run as a subprocess
// ============================================================================
// `python3 tools/host_oracle.py --all` prints one row per (program, input). The
// driver parses those rows and uses them as the expectation; the subprocess's
// exit status is the oracle's own three-way cross-check, so a non-zero exit is
// a setup failure of the sweep, not a DUT result.
struct OracleCase {
  std::string program;
  int input = 0;
  uint64_t a = 0;
  uint64_t b = 0;
  uint64_t c = 0;
  uint64_t sig[4] = {0, 0, 0, 0};
  std::string traps;   // the oracle's trap causes, e.g. "4,4,6,6,7" or "-"
};

bool IsProgramRow(const std::string& token) {
  if (token.size() < 5) return false;
  if (token[0] != 'p') return false;
  if (!std::isdigit(static_cast<unsigned char>(token[1]))) return false;
  if (!std::isdigit(static_cast<unsigned char>(token[2]))) return false;
  return token[3] == '_';
}

bool RunOracle(const std::string& repo, std::vector<OracleCase>* cases,
               std::string* detail) {
  const std::string command = "python3 " + repo + "/tools/host_oracle.py --all 2>&1";
  std::FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    *detail = "cannot start `" + command + "`";
    return false;
  }
  std::string output;
  char line[4096];
  while (std::fgets(line, sizeof(line), pipe) != nullptr) output += line;
  const int status = pclose(pipe);
  if (status != 0) {
    *detail = "`tools/host_oracle.py --all` exited " + Dec(static_cast<uint64_t>(status)) +
              "; the oracle disagrees with the corpus declaration, the golden file or "
              "its own input-sensitivity check:\n" + output;
    return false;
  }

  std::istringstream text(output);
  std::string row_text;
  while (std::getline(text, row_text)) {
    std::istringstream row(row_text);
    std::string name;
    if (!(row >> name)) continue;
    if (!IsProgramRow(name)) continue;
    std::string index, a, b, c, s0, s1, s2, s3, traps;
    if (!(row >> index >> a >> b >> c >> s0 >> s1 >> s2 >> s3 >> traps)) continue;
    OracleCase entry;
    entry.program = name;
    entry.input = std::atoi(index.c_str());
    entry.a = std::strtoull(a.c_str(), nullptr, 16);
    entry.b = std::strtoull(b.c_str(), nullptr, 16);
    entry.c = std::strtoull(c.c_str(), nullptr, 16);
    entry.sig[0] = std::strtoull(s0.c_str(), nullptr, 16);
    entry.sig[1] = std::strtoull(s1.c_str(), nullptr, 16);
    entry.sig[2] = std::strtoull(s2.c_str(), nullptr, 16);
    entry.sig[3] = std::strtoull(s3.c_str(), nullptr, 16);
    entry.traps = traps;
    cases->push_back(entry);
  }
  if (cases->empty()) {
    *detail = "the oracle printed no rows:\n" + output;
    return false;
  }
  return true;
}

// ============================================================================
// The two program-derived relations between a program's published signature and
// the oracle's row (see the header comment).
// ============================================================================
const char* kRelationIdentity = "identity: the program publishes the oracle's row";
const char* kRelationP08 =
    "p08: the program folds all 8 trap-log records and its own canary verdict";
const char* kRelationP13 =
    "p13: the program folds the written record count and XORs its armed-trap resume marker";

// p08_misaligned's program value for signature word 3, derived from the oracle's
// row and two properties of the program and the firmware:
//
//   1. the fold reads MOSAIC_TRAPLOG_RECORDS = 8 records, not the five written,
//      so the oracle's five-cause fold is shifted up by the three zero records;
//   2. bits 1:0 are the program's canary verdict, and the canary bit is set only
//      when the canary value (MOSAIC_CANARY ^ c) is zero.
uint64_t P08ProgramSig3(uint64_t oracle_sig3, uint64_t c) {
  const uint64_t canary = kCanaryConstant ^ c;
  const uint64_t bits = (canary == 0) ? UINT64_C(3) : UINT64_C(2);
  return ((oracle_sig3 & ~UINT64_C(3)) << 24) | bits;
}

void ApplyProgramRelation(const std::string& program, uint64_t c, uint64_t sig[4],
                          const char** relation) {
  if (program == "p08_misaligned") {
    sig[3] = P08ProgramSig3(sig[3], c);
    *relation = kRelationP08;
  } else if (program == "p13_romstore") {
    sig[3] = sig[3] ^ UINT64_C(1);
    *relation = kRelationP13;
  } else {
    *relation = kRelationIdentity;
  }
}

// A mnemonic for the instruction the machine refused. The corpus audit
// (tests/programs/audit/check_p0_isa.py) proves every instruction in every image
// is in rv64im_zicsr_zifencei, so a refusal here is either an ECALL/EBREAK
// before the trap vector is armed, or a decode the machine does not accept.
std::string DescribeStop(uint32_t bits) {
  if (bits == 0x00000073u) return "ECALL";
  if (bits == 0x00100073u) return "EBREAK";
  char text[32];
  std::snprintf(text, sizeof(text), "0x%08x", bits);
  return std::string("instruction word ") + text;
}

// The frozen protocol's harness override of input word 0, seeded exactly as
// CASE=core.trap_csr_program seeds it. The memory model serves FROMHOST as a
// *device*: a read at exactly that address returns the input word, whatever its
// size. But the core's data port does not read a single address -- it reads the
// aligned doubleword the access selects (mosaic_lsu_endpoint.sv's lane
// convention) -- so the memory model's byte-wise view of the other seven bytes of
// the word is ordinary RAM. Seeding those bytes with the input word's high bytes
// is what makes the device's own rule and the doubleword window agree. Without
// it, a `ld` of FROMHOST returns only the input's low byte, which is exactly the
// kind of harness defect this sweep exists to not fool itself with.
void SeedFromhost(mosaic::MemoryModel* mem, uint64_t value) {
  mem->SetInputWord(value);
  for (unsigned i = 1; i < 8; i++) {
    if (mem->Write(MOSAIC_FROMHOST + i, 1, (value >> (8 * i)) & 0xFFu) !=
        mosaic::AccessStatus::kOk) {
      Fail("run", "FROMHOST is not writable in the memory map");
    }
  }
}

// ============================================================================
// The run
// ============================================================================
enum class RunStatus { kPass, kFail, kStopped, kTimeout, kStalled, kSetupError };

const char* StatusName(RunStatus status) {
  switch (status) {
    case RunStatus::kPass: return "PASS";
    case RunStatus::kFail: return "FAIL";
    case RunStatus::kStopped: return "STOP";
    case RunStatus::kTimeout: return "TIMEOUT";
    case RunStatus::kStalled: return "STALL";
    case RunStatus::kSetupError: return "SETUP";
  }
  return "?";
}

struct RunRecord {
  std::string program;
  int input = 0;
  uint64_t a = 0, b = 0, c = 0;
  std::string traps;
  const char* relation = kRelationIdentity;
  uint64_t oracle_sig[4] = {0, 0, 0, 0};
  uint64_t expect[4] = {0, 0, 0, 0};
  uint64_t got[4] = {0, 0, 0, 0};
  uint64_t ref_sig[4] = {0, 0, 0, 0};
  bool ref_exited = false;
  bool ref_matches = false;
  bool elf_loaded = false;
  bool signature_readable = false;
  RunStatus status = RunStatus::kSetupError;
  uint64_t cycles = 0;
  uint64_t retires = 0;
  uint64_t redirects = 0;
  uint64_t traps_taken = 0;
  int first_diverge = -1;
  uint64_t stop_pc = 0;
  uint32_t stop_bits = 0;
  uint64_t stop_cycle = 0;
  uint64_t stop_rob_occupied = 0;
  bool delivery_tracking_overflowed = false;
  uint64_t unsupported = 0;
  uint64_t illegal = 0;
  uint64_t invariant_violations = 0;
  std::string invariant_first;
  std::string note;
};

// The driver, so the same cycle function drives every run.
class Sweep {
 public:
  Sweep(Vmosaic_core_tb* dut, const Geometry& g) : dut_(dut), g_(g) {}

  RunRecord Run(const OracleCase& entry, const std::string& elf_path,
                uint64_t seed_word, uint64_t max_cycles, bool control_illegal) {
    RunRecord record;
    record.program = entry.program;
    record.input = entry.input;
    record.a = entry.a;
    record.b = entry.b;
    record.c = entry.c;
    record.traps = entry.traps;
    for (int k = 0; k < 4; k++) record.oracle_sig[k] = entry.sig[k];
    // `expect` is the oracle row with the program relation applied: the value
    // the program publishes. `oracle_sig` keeps the oracle's raw row so the
    // diagnosis can show both for the two programs where they differ.
    for (int k = 0; k < 4; k++) record.expect[k] = entry.sig[k];
    ApplyProgramRelation(entry.program, entry.c, record.expect, &record.relation);

    ProgImage image;
    std::string load_detail;
    if (!image.LoadElf(elf_path, &load_detail)) {
      record.status = RunStatus::kSetupError;
      record.note = load_detail + " (build the corpus with `make -C tests/programs all`)";
      return record;
    }
    record.elf_loaded = true;
    if (control_illegal) {
      // 0x0000007F is opcode 1111111 with insn[1:0] = 11: a well-formed 32-bit
      // instruction the decoder has no arm for, so it is refused as illegal. (An
      // all-zero word is also illegal, but insn[1:0] = 00 does not reach the
      // decoder -- the front end is documented as decompressing before it, and
      // I-041 is not in yet -- so it is not a clean control.)
      image.Patch(g_.reset_vector, 0x0000007Fu);
    }

    mosaic::MemoryModel dut_mem;
    mosaic::MemoryModel ref_mem;
    {
      std::string detail;
      if (!dut_mem.LoadImage(image.elf(), &detail)) {
        record.status = RunStatus::kSetupError;
        record.note = detail;
        return record;
      }
      if (!ref_mem.LoadImage(image.elf(), &detail)) {
        record.status = RunStatus::kSetupError;
        record.note = detail;
        return record;
      }
      // crt0 reads FROMHOST and, when non-zero, overwrites input word 0 with it.
      // Seeding the memory model with the declared `a` is that protocol; because
      // the corpus also compiles `a` into the image, seeding is an identity on
      // the healthy path and the only thing it changes is the control
      // (`--control-input`), which seeds only the DUT.
      SeedFromhost(&dut_mem, seed_word);
      SeedFromhost(&ref_mem, entry.a);
    }

    // ---- the independent host interpretation of the same image ----
    const mosaic_trap::RefResult reference =
        mosaic_trap::RunReference(image, g_.reset_vector, &ref_mem);
    record.ref_exited = reference.exited;
    if (!reference.exited && !control_illegal) {
      record.status = RunStatus::kSetupError;
      record.note = "the independent interpreter did not reach the exit protocol: " +
                    reference.stop_reason + " at " + U64(reference.stop_pc);
      return record;
    }
    if (!reference.exited) {
      // `--control-illegal` plants an illegal word, so the interpreter stops
      // there too; the DUT still runs, which is the point of the control.
      record.note = "CONTROL: the image was patched with an illegal word at the reset "
                    "vector";
    }
    {
      std::vector<uint64_t> words;
      if (ref_mem.ReadSignature(&words) && words.size() == 4) {
        for (int k = 0; k < 4; k++) record.ref_sig[k] = words[k];
      }
    }
    record.ref_matches = true;
    for (int k = 0; k < 4; k++) {
      if (record.ref_sig[k] != record.expect[k]) record.ref_matches = false;
    }
    if (std::getenv("SWEEP_DEBUG") != nullptr) {
      std::printf("  [debug] %s in%d ref=%s %s %s %s oracle-derived=%s %s %s %s match=%d "
                  "ref_retires=%zu exit_pc=%s\n",
                  entry.program.c_str(), entry.input, U64(record.ref_sig[0]).c_str(),
                  U64(record.ref_sig[1]).c_str(), U64(record.ref_sig[2]).c_str(),
                  U64(record.ref_sig[3]).c_str(), U64(record.expect[0]).c_str(),
                  U64(record.expect[1]).c_str(), U64(record.expect[2]).c_str(),
                  U64(record.expect[3]).c_str(), record.ref_matches ? 1 : 0,
                  reference.trace.size(), U64(reference.exit_pc).c_str());
    }

    // ---- the DUT ----
    Imem imem(&image);
    DataMem dmem(&dut_mem);
    imem.Reset();
    dmem.Reset();
    cycles_ = 0;
    violations_ = 0;
    invariant_first_.clear();
    last_unsupported_ = 0;
    stop_pc_ = 0;
    stop_bits_ = 0;
    stop_cycle_ = 0;
    saw_unsupported_ = false;
    last_commit_ = 0;
    last_alloc_ = 0;
    last_progress_ = 0;
    delivered_.clear();
    retired_pcs_.clear();
    delivery_tracking_overflowed_ = false;

    dut_->clk = 0;
    dut_->rst = 1;
    dut_->eval();
    for (int i = 0; i < kResetCycles; i++) Cycle(true, &imem, &dmem);

    while (true) {
      Cycle(false, &imem, &dmem);
      if (dut_mem.finished()) {
        record.status = RunStatus::kPass;
        break;
      }
      if (dut_->o_stopped_o != 0) {
        record.status = RunStatus::kStopped;
        // The refusal is registered (`stop_q`) and the counter that counts it
        // rises on the same edge, so Observe() cannot see that transition. The
        // refused instruction is named from the delivered/retired streams: it is
        // the oldest instruction the front end delivered whose PC never retired,
        // because the machine quiesces with an empty ROB (everything dispatched
        // before the refusal retires) and the dispatch consumes in order.
        if (!saw_unsupported_) {
          saw_unsupported_ = true;
          stop_cycle_ = cycles_;
          for (const Delivery& d : delivered_) {
            if (retired_pcs_.find(d.pc) == retired_pcs_.end()) {
              stop_pc_ = d.pc;
              stop_bits_ = d.bits;
              break;
            }
          }
        }
        break;
      }
      if (cycles_ >= max_cycles) {
        record.status = RunStatus::kTimeout;
        break;
      }
      if (cycles_ - last_progress_ > kStallCycles) {
        record.status = RunStatus::kStalled;
        break;
      }
    }
    const bool finished = (record.status == RunStatus::kPass);
    if (finished) {
      for (int i = 0; i < kSettleCycles; i++) Cycle(false, &imem, &dmem);
    } else if (record.status == RunStatus::kStopped) {
      // Let the machine quiesce (the refusal stops dispatch; the ROB drains) so
      // the ROB occupancy describes the stop and the delivered/retired streams
      // agree about what never retired.
      for (int i = 0; i < 8 * kSettleCycles; i++) Cycle(false, &imem, &dmem);
    }

    record.cycles = cycles_;
    record.retires = dut_->o_commit_o;
    record.redirects = dut_->o_redirect_o;
    record.traps_taken = dut_->o_csr_trap_o;
    record.unsupported = dut_->o_unsupported_o;
    record.illegal = dut_->o_illegal_o;
    record.stop_pc = stop_pc_;
    record.stop_bits = stop_bits_;
    record.stop_cycle = stop_cycle_;
    record.stop_rob_occupied = dut_->o_rob_occupied_o;
    record.delivery_tracking_overflowed = delivery_tracking_overflowed_;
    record.invariant_violations = violations_;
    record.invariant_first = invariant_first_;

    if (finished) {
      std::vector<uint64_t> words;
      if (dut_mem.ReadSignature(&words) && words.size() == 4) {
        record.signature_readable = true;
        for (int k = 0; k < 4; k++) record.got[k] = words[k];
      }
    }
    if (record.status == RunStatus::kPass) {
      record.status = RunStatus::kFail;  // provisionally; PASS only if nothing diverges
      bool ok = true;
      if (!record.signature_readable) {
        ok = false;
        record.note = "the signature area is not readable in the memory model";
      } else if (!record.ref_matches) {
        ok = false;
        record.note = "the independent interpretation disagrees with the oracle-derived "
                      "expectation";
      } else {
        for (int k = 0; k < 4; k++) {
          if (record.got[k] != record.expect[k]) {
            record.first_diverge = k;
            ok = false;
            record.note = "signature word " + Dec(static_cast<uint64_t>(k)) +
                          " diverges from the oracle-derived expectation";
            break;
          }
        }
      }
      if (ok) record.status = RunStatus::kPass;
    }

    // The DUT must not stall before it reaches its exit protocol.
    if (std::getenv("SWEEP_DEBUG") != nullptr && record.signature_readable) {
      const struct { const char* name; uint64_t base; uint64_t bytes; } areas[] = {
          {"signature", MOSAIC_SIGNATURE_ADDR, 32},
          {"scratch", 0x80001080ull, 128},
          {"prog_inputs", 0x800002b8ull, 24},
      };
      uint64_t mismatches = 0;
      for (const auto& area : areas) {
        for (uint64_t off = 0; off < area.bytes; off++) {
          uint64_t want = 0, got = 0;
          ref_mem.Read(area.base + off, 1, &want);
          dut_mem.Read(area.base + off, 1, &got);
          if (want != got) {
            if (mismatches < 12) {
              std::printf("  [debug] %s in%d memory differs at %s (%s+%llu): ref=%02llx "
                          "dut=%02llx\n",
                          entry.program.c_str(), entry.input,
                          U64(area.base + off).c_str(), area.name,
                          static_cast<unsigned long long>(off),
                          static_cast<unsigned long long>(want),
                          static_cast<unsigned long long>(got));
            }
            mismatches++;
          }
        }
      }
      std::printf("  [debug] %s in%d memory mismatching bytes in signature+scratch+inputs: "
                  "%llu; dut_retires=%llu\n",
                  entry.program.c_str(), entry.input,
                  static_cast<unsigned long long>(mismatches),
                  static_cast<unsigned long long>(record.retires));
    }
    return record;
  }

 private:
  void Observe() {
    // The front end's delivered stream, in order. A new delivery is the fetch
    // output register holding an instruction it did not hold the cycle before.
    if (dut_->o_dbg_deliver_valid_o != 0) {
      const uint64_t pc = dut_->o_dbg_deliver_pc_o;
      const uint32_t bits = static_cast<uint32_t>(dut_->o_dbg_deliver_bits_o);
      if (delivered_.empty() || delivered_.back().pc != pc ||
          delivered_.back().bits != bits) {
        if (delivered_.size() < kMaxDeliveries) {
          delivered_.push_back(Delivery{pc, bits});
        } else {
          delivery_tracking_overflowed_ = true;
        }
      }
    }
    {
      const uint32_t mask =
          (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
      const uint32_t got = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
      for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
        if ((got & (1u << lane)) != 0) {
          retired_pcs_.insert(PayloadLane(dut_->ev_pc_o, lane));
        }
      }
    }

    if (dut_->o_unsupported_o != last_unsupported_) {
      if (!saw_unsupported_) {
        saw_unsupported_ = true;
        stop_pc_ = dut_->o_dbg_deliver_pc_o;
        stop_bits_ = static_cast<uint32_t>(dut_->o_dbg_deliver_bits_o);
        stop_cycle_ = cycles_;
      }
      last_unsupported_ = dut_->o_unsupported_o;
    }

    // Invariants every other core case asserts, kept here so a sweep run that
    // corrupts the machine's bookkeeping fails where it happened.
    if (dut_->o_squash_nc_o != 0 || dut_->o_squash_under_o != 0 ||
        dut_->o_journal_ovf_o != 0) {
      Note("recovery counters: squash_nc=" + Dec(dut_->o_squash_nc_o) + " under=" +
           Dec(dut_->o_squash_under_o) + " journal=" + Dec(dut_->o_journal_ovf_o));
    }
    if (dut_->o_rob_occupied_o > g_.rob_entries) {
      Note("ROB occupancy " + Dec(dut_->o_rob_occupied_o) + " exceeds " +
           Dec(g_.rob_entries));
    }
    if (dut_->o_mem_lq_query_mismatch_o != 0) {
      Note("the load queue's forwarding-query cross-check tripped: " +
           Dec(dut_->o_mem_lq_query_mismatch_o));
    }
    if (dut_->o_mem_sq_auth_o > dut_->o_mem_sq_occupied_o) {
      Note("store-queue authorised prefix " + Dec(dut_->o_mem_sq_auth_o) +
           " exceeds occupancy " + Dec(dut_->o_mem_sq_occupied_o));
    }

    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_wb_pub_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) last_progress_ = cycles_;
  }

  void Note(const std::string& text) {
    violations_++;
    if (invariant_first_.empty()) {
      invariant_first_ = "cycle " + Dec(cycles_) + ": " + text;
    }
  }

  void Cycle(bool rst, Imem* imem, DataMem* dmem) {
    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = 1;
    if (imem->HasResponse()) {
      const Imem::Request& r = imem->Response();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem->ResponseWord());
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = r.id;
      dut_->imem_rsp_epoch_i = r.epoch;
      dut_->imem_rsp_len_i = 4;
    } else {
      dut_->imem_rsp_valid_i = 0;
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }

    dut_->dmem_req_ready_i = 1;
    if (dmem->HasResponse()) {
      const DataMem::Rsp& r = dmem->CurrentResponse();
      dut_->dmem_rsp_valid_i = 1;
      dut_->dmem_rsp_rdata_i = r.rdata;
      dut_->dmem_rsp_fault_i = r.fault ? 1 : 0;
    } else {
      dut_->dmem_rsp_valid_i = 0;
      dut_->dmem_rsp_rdata_i = 0;
      dut_->dmem_rsp_fault_i = 0;
    }

    // The standalone redirect arbiter DUT is driven by CASE=fabric.fixed_two_cluster.
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;
    // No interrupt is asserted for any corpus program.
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = 0;

    dut_->eval();

    if (!rst) Observe();

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      imem->Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem->PopResponse();
    }
    imem->Advance();

    if (bus_reset_.MayAccept(rst, (dut_->dmem_req_valid_o != 0) &&
                                      (dut_->dmem_req_ready_i != 0))) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem->Accept(r, cycles_);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
      dmem->PopResponse();
    }
    dmem->Advance();

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  Vmosaic_core_tb* dut_;
  // The reset-traffic rule (V-010, sim/common/bus_reset_gate.h): while reset
  // is asserted this model accepts nothing, so no reset-time response can be
  // queued ahead of a fresh post-reset one.
  mosaic::BusResetGate bus_reset_;
  Geometry g_;
  uint64_t cycles_ = 0;
  uint64_t violations_ = 0;
  uint64_t last_unsupported_ = 0;
  uint64_t stop_pc_ = 0;
  uint32_t stop_bits_ = 0;
  uint64_t stop_cycle_ = 0;
  bool saw_unsupported_ = false;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  std::string invariant_first_;
  // For the STOP diagnosis: the front end's delivered instruction stream, in
  // order, and the set of PCs that retired. The instruction the dispatch refused
  // is the oldest delivery whose PC never retired (see the diagnosis in main()).
  struct Delivery {
    uint64_t pc;
    uint32_t bits;
  };
  std::vector<Delivery> delivered_;
  std::set<uint64_t> retired_pcs_;   // needs <set>
  bool delivery_tracking_overflowed_ = false;
};

// ============================================================================
// The command-line controls (not used by the registered run)
// ============================================================================
struct Controls {
  std::string only;
  bool list = false;
  std::string control_expect_program;
  int control_expect_input = -1;
  int control_expect_word = -1;
  std::string control_input_program;
  int control_input_input = -1;
  std::string control_illegal_program;
  int control_illegal_input = -1;
};

bool Matches(const std::string& program, int input, const std::string& want_program,
             int want_input) {
  return !want_program.empty() && program == want_program && input == want_input;
}

}  // namespace

int main(int argc, char** argv) {
  Controls controls;
  std::vector<char*> filtered;
  filtered.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = (i + 1) < argc;
    if (flag == "--only" && has_value) {
      controls.only = argv[++i];
    } else if (flag == "--list") {
      controls.list = true;
    } else if (flag == "--control-expect" && (i + 3) < argc) {
      controls.control_expect_program = argv[++i];
      controls.control_expect_input = std::atoi(argv[++i]);
      controls.control_expect_word = std::atoi(argv[++i]);
    } else if (flag == "--control-input" && (i + 2) < argc) {
      controls.control_input_program = argv[++i];
      controls.control_input_input = std::atoi(argv[++i]);
    } else if (flag == "--control-illegal" && (i + 2) < argc) {
      controls.control_illegal_program = argv[++i];
      controls.control_illegal_input = std::atoi(argv[++i]);
    } else {
      filtered.push_back(argv[i]);
      if (has_value && (flag == "--case" || flag == "--out" || flag == "--image" ||
                        flag == "--expect" || flag == "--seed" ||
                        flag == "--max-cycles")) {
        filtered.push_back(argv[++i]);
      }
    }
  }

  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(static_cast<int>(filtered.size()), filtered.data(),
                              &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    const std::string repo = FindRepoRoot();

    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    if (geometry.reset_vector != 0x80000000ull) {
      Fail("geometry", "the reset vector is not the corpus link base: " +
                           U64(geometry.reset_vector));
    }

    std::vector<OracleCase> oracle;
    std::string oracle_detail;
    if (!RunOracle(repo, &oracle, &oracle_detail)) {
      Fail("oracle", oracle_detail);
    }

    std::printf("CASE core.corpus_sweep -- the whole p0 corpus on the out-of-order core\n");
    std::printf("  reset vector %s, signature area %s, tohost %s\n",
                U64(geometry.reset_vector).c_str(), U64(MOSAIC_SIGNATURE_ADDR).c_str(),
                U64(MOSAIC_TOHOST).c_str());
    std::printf("  oracle: `python3 tools/host_oracle.py --all` exit 0, %zu rows; the "
                "oracle agrees with the corpus declaration, the golden file and its "
                "own input-sensitivity check\n", oracle.size());
    std::printf("  DUT: %s\n", (repo + "/tests/programs/build").c_str());

    if (!controls.only.empty()) {
      std::vector<OracleCase> kept;
      for (const OracleCase& entry : oracle) {
        if (entry.program == controls.only) kept.push_back(entry);
      }
      if (kept.empty()) Fail("--only", "no program named " + controls.only);
      oracle = kept;
      std::printf("  --only %s: %zu rows\n", controls.only.c_str(), oracle.size());
    }

    if (controls.list) {
      for (const OracleCase& entry : oracle) {
        uint64_t sig[4] = {entry.sig[0], entry.sig[1], entry.sig[2], entry.sig[3]};
        const char* relation = kRelationIdentity;
        ApplyProgramRelation(entry.program, entry.c, sig, &relation);
        std::printf("  %-14s in%d a=%s b=%s c=%s -> %s %s %s %s  traps=%s\n",
                    entry.program.c_str(), entry.input, U64(entry.a).c_str(),
                    U64(entry.b).c_str(), U64(entry.c).c_str(), U64(sig[0]).c_str(),
                    U64(sig[1]).c_str(), U64(sig[2]).c_str(), U64(sig[3]).c_str(),
                    entry.traps.c_str());
      }
      return reporter.Finish("PASS", "listed " + Dec(oracle.size()) + " oracle rows");
    }

    // The programs whose relation to the oracle is not the identity are stated
    // once, so the matrix below cannot be read as if all rows were the same kind.
    for (const OracleCase& entry : oracle) {
      if (entry.input != 0) continue;
      uint64_t sig[4] = {entry.sig[0], entry.sig[1], entry.sig[2], entry.sig[3]};
      const char* relation = kRelationIdentity;
      ApplyProgramRelation(entry.program, entry.c, sig, &relation);
      std::printf("  relation: %-14s %s\n", entry.program.c_str(), relation);
    }

    Sweep sweep(&dut, geometry);
    std::vector<RunRecord> records;
    records.reserve(oracle.size());

    int failures = 0;
    int stopped = 0;
    uint64_t total_cycles = 0;
    uint64_t total_retires = 0;

    for (const OracleCase& entry : oracle) {
      const std::string elf_path = repo + "/tests/programs/build/" + entry.program +
                                   ".i" + Dec(static_cast<uint64_t>(entry.input)) + ".elf";

      // The controls. `--control-expect` perturbs the expected value after the
      // oracle relation; `--control-input` boots the DUT on a different input
      // word than the oracle's row describes.
      const bool expect_perturbed =
          Matches(entry.program, entry.input, controls.control_expect_program,
                  controls.control_expect_input);
      const bool input_misseeded =
          Matches(entry.program, entry.input, controls.control_input_program,
                  controls.control_input_input);
      const bool illegal_planted =
          Matches(entry.program, entry.input, controls.control_illegal_program,
                  controls.control_illegal_input);
      const uint64_t seed_word = input_misseeded ? (entry.a ^ UINT64_C(1)) : entry.a;

      RunRecord record;
      try {
        record = sweep.Run(entry, elf_path, seed_word, options.max_cycles, illegal_planted);
      } catch (const Failure& f) {
        record.program = entry.program;
        record.input = entry.input;
        record.status = RunStatus::kSetupError;
        record.note = f.what;
      }
      if (illegal_planted) {
        std::printf("  ** CONTROL --control-illegal %s in%d: the illegal 32-bit word "
                    "0x0000007f was planted at the reset vector\n", entry.program.c_str(),
                    entry.input);
      }

      if (expect_perturbed) {
        if (controls.control_expect_word < 0 || controls.control_expect_word > 3) {
          Fail("--control-expect", "the word index must be 0..3");
        }
        record.expect[controls.control_expect_word] ^= UINT64_C(1);
        if (record.first_diverge < 0 && record.signature_readable) {
          for (int k = 0; k < 4; k++) {
            if (record.got[k] != record.expect[k]) {
              record.first_diverge = k;
              break;
            }
          }
        }
        record.status = RunStatus::kFail;
        record.note = "CONTROL: the oracle-derived expectation was perturbed by one bit in "
                      "word " + Dec(static_cast<uint64_t>(controls.control_expect_word));
        std::printf("  ** CONTROL --control-expect %s in%d word %d: the expectation is "
                    "deliberately wrong\n", entry.program.c_str(), entry.input,
                    controls.control_expect_word);
      }
      if (input_misseeded) {
        if (record.first_diverge < 0 && record.signature_readable) {
          for (int k = 0; k < 4; k++) {
            if (record.got[k] != record.expect[k]) {
              record.first_diverge = k;
              break;
            }
          }
        }
        record.status = RunStatus::kFail;
        record.note = "CONTROL: the DUT was booted on a mis-set input (FROMHOST = a ^ 1)";
        std::printf("  ** CONTROL --control-input %s in%d: the DUT is booted on a "
                    "different input than the oracle's row\n", entry.program.c_str(),
                    entry.input);
      }

      total_cycles += record.cycles;
      total_retires += record.retires;
      if (record.status == RunStatus::kPass) {
        std::printf("  %-14s in%d  PASS     cycles=%-8llu retires=%-7llu redir=%-4llu "
                    "traps=%-3llu\n",
                    record.program.c_str(), record.input,
                    static_cast<unsigned long long>(record.cycles),
                    static_cast<unsigned long long>(record.retires),
                    static_cast<unsigned long long>(record.redirects),
                    static_cast<unsigned long long>(record.traps_taken));
      } else {
        std::printf("  %-14s in%d  %-7s  cycles=%-8llu retires=%-7llu redir=%-4llu "
                    "traps=%-3llu first_divergent_word=%s\n",
                    record.program.c_str(), record.input, StatusName(record.status),
                    static_cast<unsigned long long>(record.cycles),
                    static_cast<unsigned long long>(record.retires),
                    static_cast<unsigned long long>(record.redirects),
                    static_cast<unsigned long long>(record.traps_taken),
                    record.first_diverge < 0 ? "-"
                                             : Dec(static_cast<uint64_t>(record.first_diverge))
                                                   .c_str());
        if (record.status == RunStatus::kStopped) {
          ++stopped;
        } else {
          ++failures;
        }
      }
      records.push_back(record);
    }

    // ---------------------------------------------------------- diagnoses
    std::printf("\n--- diagnoses for the runs that did not pass ---\n");
    int diagnoses = 0;
    for (const RunRecord& record : records) {
      if (record.status == RunStatus::kPass) continue;
      ++diagnoses;
      const std::string label =
          record.program + " in" + Dec(static_cast<uint64_t>(record.input));
      if (record.status == RunStatus::kSetupError) {
        std::printf("SETUP %s: %s\n", label.c_str(), record.note.c_str());
      } else if (record.status == RunStatus::kStopped) {
        std::printf(
            "STOP  %s: the machine refused %s at pc=%s (cycle %llu) and stopped "
            "(unsupported=%llu, illegal=%llu, rob_occupied=%llu%s). The refused "
            "instruction is the oldest the front end delivered whose PC never "
            "retired.\n",
            label.c_str(), DescribeStop(record.stop_bits).c_str(),
            U64(record.stop_pc).c_str(),
            static_cast<unsigned long long>(record.stop_cycle),
            static_cast<unsigned long long>(record.unsupported),
            static_cast<unsigned long long>(record.illegal),
            static_cast<unsigned long long>(record.stop_rob_occupied),
            record.delivery_tracking_overflowed ? ", delivery stream truncated" : "");
        std::printf("      oracle expects sig = %s %s %s %s (traps=%s)\n",
                    U64(record.expect[0]).c_str(), U64(record.expect[1]).c_str(),
                    U64(record.expect[2]).c_str(), U64(record.expect[3]).c_str(),
                    record.traps.c_str());
      } else if (record.status == RunStatus::kTimeout ||
                 record.status == RunStatus::kStalled) {
        std::printf(
            "%s %s: the run did not reach the program's TOHOST within %llu cycles "
            "(retires=%llu, redirects=%llu, rob_head=%s)\n",
            StatusName(record.status), label.c_str(),
            static_cast<unsigned long long>(record.cycles),
            static_cast<unsigned long long>(record.retires),
            static_cast<unsigned long long>(record.redirects),
            U64(dut.o_dbg_head_pc_o).c_str());
      } else {
        std::printf(
            "FAIL  %s: first divergent signature word %d -- oracle-derived expectation "
            "%s, DUT %s\n",
            label.c_str(), record.first_diverge,
            record.first_diverge < 0 ? "?" : U64(record.expect[record.first_diverge]).c_str(),
            record.first_diverge < 0 ? "?" : U64(record.got[record.first_diverge]).c_str());
        std::printf("      oracle row (a=%s b=%s c=%s, traps=%s): %s %s %s %s\n",
                    U64(record.a).c_str(), U64(record.b).c_str(), U64(record.c).c_str(),
                    record.traps.c_str(),
                    U64(record.oracle_sig[0]).c_str(), U64(record.oracle_sig[1]).c_str(),
                    U64(record.oracle_sig[2]).c_str(), U64(record.oracle_sig[3]).c_str());
        std::printf("      expectation after the program relation (%s): %s %s %s %s\n",
                    record.relation, U64(record.expect[0]).c_str(),
                    U64(record.expect[1]).c_str(), U64(record.expect[2]).c_str(),
                    U64(record.expect[3]).c_str());
        std::printf("      DUT signature: %s %s %s %s; independent interpretation: %s %s "
                    "%s %s\n",
                    U64(record.got[0]).c_str(), U64(record.got[1]).c_str(),
                    U64(record.got[2]).c_str(), U64(record.got[3]).c_str(),
                    U64(record.ref_sig[0]).c_str(), U64(record.ref_sig[1]).c_str(),
                    U64(record.ref_sig[2]).c_str(), U64(record.ref_sig[3]).c_str());
        std::printf("      machine counters: unsupported=%llu illegal=%llu stopped=%d\n",
                    static_cast<unsigned long long>(record.unsupported),
                    static_cast<unsigned long long>(record.illegal),
                    dut.o_stopped_o != 0 ? 1 : 0);
      }
      if (!record.note.empty()) {
        std::printf("      note: %s\n", record.note.c_str());
      }
      if (record.invariant_violations != 0) {
        std::printf("      invariants: %llu violation(s); first: %s\n",
                    static_cast<unsigned long long>(record.invariant_violations),
                    record.invariant_first.c_str());
      }
    }
    if (diagnoses == 0) {
      std::printf("  none: every (program, input) reached its TOHOST and its signature "
                  "equals the oracle-derived expectation.\n");
    }

    // ------------------------------------------------------------- summary
    std::printf("\nsummary: %zu runs (%zu programs x %d inputs): %d PASS, %d FAIL, %d "
                "STOPPED; cycles=%llu retires=%llu\n",
                records.size(), oracle.size() / 3, 3,
                static_cast<int>(records.size()) - failures - stopped, failures, stopped,
                static_cast<unsigned long long>(total_cycles),
                static_cast<unsigned long long>(total_retires));

    // A stop is a corpus program the machine cannot run, and the machine is
    // documented to run the whole corpus; so it fails the case too, but with its
    // own name in the summary rather than being averaged into "FAIL".
    const int bad = failures + stopped;
    passed = (bad == 0) && (reporter.failures() == 0);

    // The relations this sweep applies are stated as checks so that a program
    // text (or oracle) change that breaks them fails here rather than silently
    // redefining the expectation.
    for (const RunRecord& record : records) {
      if (!record.elf_loaded || !record.ref_exited) continue;
      const std::string label =
          record.program + " in" + Dec(static_cast<uint64_t>(record.input));
      bool relates = true;
      if (!record.ref_matches) relates = false;
      reporter.Check(relates,
                     label + ": the independent interpretation of the image equals the "
                             "oracle-derived expectation (" + record.relation + ")");
    }

    reporter.Check(bad == 0, "every corpus (program, input) runs on the core and matches "
                             "the host oracle (" + Dec(static_cast<uint64_t>(records.size())) +
                                 " runs)");

    detail = "programs=" + Dec(oracle.size() / 3) + " inputs=3 runs=" +
             Dec(static_cast<uint64_t>(records.size())) + " pass=" +
             Dec(static_cast<uint64_t>(records.size()) - static_cast<uint64_t>(bad)) +
             " fail=" + Dec(static_cast<uint64_t>(failures)) + " stopped=" +
             Dec(static_cast<uint64_t>(stopped)) + " cycles=" + Dec(total_cycles) +
             " retires=" + Dec(total_retires) + " oracle=tools/host_oracle.py";
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the sweep runs", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
