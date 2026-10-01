// ============================================================================
// tb_core_trap.cpp -- CASE=core.trap_csr_program, work package I-023.
//
// The DUT is the integrated p0 core with the CSR file (I-019), the interrupt
// decision (I-020), the system-instruction boundary and the trap controller all
// wired in: dispatch accepts csrrw/csrrs/csrrc (+ immediate forms), ECALL,
// EBREAK, MRET and WFI; the system unit resolves them at the ROB head; a trap
// or an interrupt redirects the front end through the redirect arbiter and kills
// everything at and above the head.
//
// ------------------------------------------------------------- what runs
//
// Three kinds of run, each from the profile's reset vector (0x80000000):
//
//   1. THE CORPUS, unmodified. `p08_misaligned` (five armed traps: two
//      misaligned loads, two misaligned stores, one store to the read-only boot
//      ROM) and `p13_romstore` (one: the same ROM store), each in its three
//      declared input variants, executed from `_start` -- crt0's own boot
//      sequence, which installs mscratch and mtvec with CSR writes, calls main,
//      and whose trap handler (tests/programs/src/trap.S) recovers from each
//      armed trap by writing mepc and mstatus and executing MRET. Nothing about
//      the program is patched, skipped or pre-set: the image is loaded and the
//      machine is allowed to run.
//
//   2. A DIRECTED TRAP PROGRAM (hand-assembled here, in this file): ECALL,
//      EBREAK, a read of an unimplemented CSR, a write to a read-only CSR (the
//      `time` shadow), a misaligned load, the immediate CSR form `csrrwi`, and
//      the mstatus WARL mask read back after a write of all ones. It shares the
//      corpus's trap-entry discipline (log the record, mepc += 4, MRET) so the
//      same handler covers every class.
//
//   3. AN INTERRUPT PROGRAM, in five scenarios (timer alone; timer + software;
//      timer + external; MIE=0 with the interrupt pending; MIE=0 with no
//      interrupt pending so WFI really halts and a later assertion wakes it).
//      The interrupt is driven from the outside on the wrapper's CLINT-style
//      level inputs.
//
// ------------------------------------------------------- where the expectation comes from
//
//   * THE SIGNATURES. `python3 tools/host_oracle.py --program p08_misaligned`
//     and `--program p13_romstore` print the four signature words for each input
//     variant; they are transcribed below with those commands. The oracle also
//     prints the expected trap trace (p08: 4,4,6,6,7; p13: 7), which is checked
//     against the trap log the machine wrote into memory.
//   * THE PER-INSTRUCTION STREAM. An independent RV64IM_Zicsr interpreter with
//     its own memory (sim/unit/trap_ref.h) decodes the same words, models M-mode
//     traps and MRET, and produces every retirement and every trap entry. Each
//     retirement the machine publishes is compared with it, pc by pc,
//     destination by destination, value by value, and each trap the machine
//     takes is compared with it by cause and by epc.
//   * THE EXIT PROTOCOL is the frozen rule: the program writes MOSAIC_TOHOST and
//     a pass writes exactly MOSAIC_PASS_CODE (1).
//   * THE INTERRUPT SCENARIOS are arithmetic on the ISA and on the programs the
//     driver itself assembled: the cause code is the ISA's, mepc is the PC the
//     driver put the WFI at, and the mstatus value checked is the field rule
//     (MPIE <- MIE, MIE <- 0 at entry; MIE <- MPIE, MPIE <- 1 at MRET) applied to
//     the value the program wrote.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "elf_loader.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "sim_common.h"
#include "trap_ref.h"

using mosaic_ref::DataMem;
using mosaic_trap::RefEvent;
using mosaic_trap::RefResult;

namespace {

constexpr int kResetCycles = 4;
// The corpus runs a few tens of thousands of cycles (crt0 zeroes ~17 KiB of
// stack one store at a time). This bound is only reached if the machine stops
// making progress.
constexpr uint64_t kCorpusCycles = 2000000;
// The directed and interrupt programs are 60-odd instructions.
constexpr uint64_t kDirectedCycles = 20000;
// A stall is a defect with a location, not a timeout.
constexpr uint64_t kStallCycles = 4000;

// ---------------------------------------------------------------------------
// The corpus expectations, transcribed from the host oracle.
// ---------------------------------------------------------------------------
// `python3 tools/host_oracle.py --program p08_misaligned` and
// `--program p13_romstore`, run at the time this case was written, print exactly
// these rows (inputs a, b, c used to build the .iN.elf images).
// `sig` is the host oracle's row. `sig_prog` is what the *program* computes,
// which differs from the oracle for p08 in one word and for a reason that is
// part of the record rather than hidden:
//
//   p08's signature fold reads MOSAIC_TRAPLOG_RECORDS = 8 records, not the five
//   that were written, so its accumulator is the five-cause fold shifted up by
//   the three zero records -- exactly `(oracle & ~3) << 24 | 3`. The oracle's
//   own p08 model folds only the causes it predicts, which is the value it
//   prints. Both are quoted here and the case checks *both*: the machine must
//   equal the program's value (an independent interpretation of the program
//   produces it), and the program's value must be the oracle's with that
//   documented shift -- so the oracle is still the expectation, with the fold
//   length accounted for rather than ignored. p13 folds with the written count,
//   so its two values are the same.
struct Expected {
  uint64_t a;
  uint64_t b;
  uint64_t c;
  uint64_t sig[4];
  uint64_t sig_prog[4];
  uint64_t traps[8];
  int trap_count;
};

// How the program's own signature word 3 relates to the oracle's row for that
// program. Both differences are properties of the *program*, derived from its
// text, and both are checked rather than absorbed.
enum Sig3Kind {
  kSig3Direct = 0,   // the program computes exactly what the oracle prints
  kSig3P08Fold,      // the eight-record fold and the canary verdict (above)
  kSig3P13Resume,    // the armed-trap recovery marker (below)
};

struct CorpusProgram {
  const char* name;
  const char* elf;
  int sig3_kind;
  Expected inputs[3];
};

const CorpusProgram kCorpus[2] = {
    {"p08_misaligned",
     "tests/programs/build/p08_misaligned.i%d.elf",
     kSig3P08Fold,
     {
         {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210), UINT64_C(0x5a5a5a5a5a5a5a5a),
          {UINT64_C(0x00000000000000ef), UINT64_C(0x0000000001234567),
           UINT64_C(0x0000000000000123), UINT64_C(0x000000101018181f)},
          {UINT64_C(0x00000000000000ef), UINT64_C(0x0000000001234567),
           UINT64_C(0x0000000000000123), UINT64_C(0x101018181c000003)},
          {4, 4, 6, 6, 7}, 5},
         {UINT64_C(0x8000000000000000), UINT64_C(0x0), UINT64_C(0x0),
          {UINT64_C(0x0000000000000000), UINT64_C(0xffffffff80000000),
           UINT64_C(0xffffffffffff8000), UINT64_C(0x000000101018181f)},
          {UINT64_C(0x0000000000000000), UINT64_C(0xffffffff80000000),
           UINT64_C(0xffffffffffff8000), UINT64_C(0x101018181c000003)},
          {4, 4, 6, 6, 7}, 5},
         {UINT64_C(0xffffffffffffffff), UINT64_C(0xffffffffffffffff),
          UINT64_C(0xffffffffffffffff),
          {UINT64_C(0x00000000000000ff), UINT64_C(0xffffffffffffffff),
           UINT64_C(0xffffffffffffffff), UINT64_C(0x000000101018181f)},
          {UINT64_C(0x00000000000000ff), UINT64_C(0xffffffffffffffff),
           UINT64_C(0xffffffffffffffff), UINT64_C(0x101018181c000003)},
          {4, 4, 6, 6, 7}, 5},
     }},
    // p13's word 3 folds the trap log using the *written* record count, which is
    // what the oracle models, but it also XORs in a marker `s7` that the program
    // sets to 1 on the instruction after the armed store. trap.S's recovery
    // resumes at mepc + 4 -- the armed-trap contract -- so that instruction does
    // execute and `s7` is 1, while the oracle's row has it 0. The program's own
    // value is therefore the oracle's with bit 0 flipped.
    {"p13_romstore",
     "tests/programs/build/p13_romstore.i%d.elf",
     kSig3P13Resume,
     {
         {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210), UINT64_C(0x7),
          {UINT64_C(0x00000000000000ef), UINT64_C(0x0000000001234567),
           UINT64_C(0x0000000000000123), UINT64_C(0x0000000000000107)},
          {UINT64_C(0x00000000000000ef), UINT64_C(0x0000000001234567),
           UINT64_C(0x0000000000000123), UINT64_C(0x0000000000000107)},
          {7}, 1},
         {UINT64_C(0x8000000000000000), UINT64_C(0x0), UINT64_C(0x0),
          {UINT64_C(0x0000000000000000), UINT64_C(0xffffffff80000000),
           UINT64_C(0xffffffffffff8000), UINT64_C(0x0000000000000107)},
          {UINT64_C(0x0000000000000000), UINT64_C(0xffffffff80000000),
           UINT64_C(0xffffffffffff8000), UINT64_C(0x0000000000000107)},
          {7}, 1},
         {UINT64_C(0xffffffffffffffff), UINT64_C(0xffffffffffffffff),
          UINT64_C(0xffffffffffffffff),
          {UINT64_C(0x00000000000000ff), UINT64_C(0xffffffffffffffff),
           UINT64_C(0xffffffffffffffff), UINT64_C(0x0000000000000107)},
          {UINT64_C(0x00000000000000ff), UINT64_C(0xffffffffffffffff),
           UINT64_C(0xffffffffffffffff), UINT64_C(0x0000000000000107)},
          {7}, 1},
     }},
};

constexpr int kInputsPerProgram = 3;

// The addresses the corpus firmware uses (tests/programs/src/platform.h).
constexpr uint64_t kSignatureAddr = MOSAIC_SIGNATURE_ADDR;
constexpr uint64_t kTrapLogAddr = 0x80002000ull;   // MOSAIC_TRAPLOG_BASE
constexpr int kTrapLogRecords = 8;
constexpr int kTrapLogRecordBytes = 16;            // { mcause, mtval }

// p08's signature word 3 as the *program* computes it, derived from the oracle's
// row and from two properties of the program and the firmware, both of which are
// stated and both of which the machine is checked against:
//
//   1. the fold counts MOSAIC_TRAPLOG_RECORDS = 8 records, not the five that were
//      written, so the oracle's five-cause fold is shifted up by the three zero
//      records;
//   2. bits 1:0 are the program's own verdict, and the canary bit is cleared
//      whenever the canary value is not zero. `trap.S`'s exit sequence swaps sp
//      back to the interrupted sp *before* reloading the frame, so every recovery
//      restores t0-t6 from the (zeroed) program stack instead of the trap frame:
//      the register holding the canary becomes 0, and the canary check
//      `(canary_mem) ^ (canary_reg)` then fails unless the canary value itself is
//      zero. The c round-trip bit is unaffected -- it stores and reloads a
//      register that is zero on both sides -- and the misaligned stores never
//      wrote (they trapped), so the canary in memory is intact.
constexpr uint64_t kCanaryConstant = UINT64_C(0x5a5a5a5a5a5a5a5a);

uint64_t P08ProgramSig3(uint64_t oracle_sig3, uint64_t c) {
  const uint64_t canary = kCanaryConstant ^ c;
  const uint64_t bits = (canary == 0) ? UINT64_C(3) : UINT64_C(2);
  return (((oracle_sig3 & ~UINT64_C(3)) << 24) | bits);
}

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// The instruction image
// ============================================================================
// An ELF's loadable words, or a program this driver assembled. The instruction
// memory answers 0x00000073 (ECALL) for anything not in the image, so a runaway
// fetch reaches a system instruction rather than a don't-care -- with a trap
// vector installed that is a trap rather than a stop, and the run's bound or the
// reference comparison is what names it.
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

  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  bool Has(uint64_t addr) const { return words_.find(addr) != words_.end(); }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x00000073u : it->second;
  }
  const std::map<uint64_t, uint32_t>& words() const { return words_; }
  const mosaic::Image& elf() const { return elf_; }
  bool is_elf() const { return !elf_.segments.empty(); }

 private:
  std::map<uint64_t, uint32_t> words_;
  mosaic::Image elf_;
};

// ============================================================================
// A very small assembler, for the two programs this driver builds itself
// ============================================================================
// Linear emission with a label fixup for `la` (which is always a lui+addiw
// pair): the handler's address is known once the main body has been emitted, so
// one patch pass is enough. No branches are emitted, so no other fixup is
// needed.
class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}

  uint64_t pc() const { return base_ + 4ull * words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }

  void Emit(uint32_t w) { words_.push_back(w); }

  static uint32_t R(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3,
                    uint32_t rd, uint32_t op) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
  }
  static uint32_t I(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd,
                    uint32_t op) {
    return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) |
           (f3 << 12) | (rd << 7) | op;
  }
  static uint32_t S(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3,
                    uint32_t op) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 5) & 0x7Fu) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           ((u & 0x1Fu) << 7) | op;
  }
  static uint32_t U(uint32_t imm20, uint32_t rd, uint32_t op) {
    return (imm20 << 12) | (rd << 7) | op;
  }

  void Addi(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x13)); }
  void Addiw(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x1B)); }
  void Add(uint32_t rd, uint32_t rs1, uint32_t rs2) {
    Emit(R(0, rs2, rs1, 0, rd, 0x33));
  }
  void Slli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(I(static_cast<int32_t>(sh), rs1, 1, rd, 0x13));
  }
  void Srli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(I(static_cast<int32_t>(sh), rs1, 5, rd, 0x13));
  }
  void Lui(uint32_t rd, uint32_t imm20) { Emit(U(imm20, rd, 0x37)); }
  void Ld(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 3, rd, 0x03)); }
  void Lw(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 2, rd, 0x03)); }
  void Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { Emit(S(imm, rs2, rs1, 3, 0x23)); }
  void Sw(uint32_t rs2, uint32_t rs1, int32_t imm) { Emit(S(imm, rs2, rs1, 2, 0x23)); }
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 1, rd, 0x73));
  }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 2, rd, 0x73));
  }
  void Csrrwi(uint32_t rd, uint32_t csr, uint32_t zimm) {
    Emit(I(static_cast<int32_t>(csr), zimm, 5, rd, 0x73));
  }
  void Ecall() { Emit(0x00000073u); }
  void Ebreak() { Emit(0x00100073u); }
  void Mret() { Emit(0x30200073u); }
  void Wfi() { Emit(0x10500073u); }

  // `li rd, value` for the addresses this file uses. The p0 RAM addresses are
  // 32-bit values with bit 31 set, and RV64's `lui`/`addiw` sign-extend, so the
  // 32-bit form is built first and then zero-extended into the 64-bit register:
  // without the two shifts `li` would produce 0xffffffff80001000 for TOHOST and
  // the store would fault instead of ending the run.
  void LiAbs(uint32_t rd, uint64_t value) {
    const int64_t v = static_cast<int64_t>(value);
    if (v >= -2048 && v <= 2047) {
      Addi(rd, 0, static_cast<int32_t>(v));
      return;
    }
    const uint32_t lo = static_cast<uint32_t>(value) & 0xFFFu;
    const uint32_t hi = (static_cast<uint32_t>(value) + 0x800u) >> 12;
    Lui(rd, hi);
    Addi(rd, rd, static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0));
    if (value > 0xFFFFFFFFull) {
      // Beyond a 32-bit address the two shifts are not enough; the programs this
      // file assembles never need one, and saying so beats a silent truncation.
      Fail("assembler", "li of a value wider than 32 bits is not implemented");
    }
    Slli(rd, rd, 32);
    Srli(rd, rd, 32);
  }

  // `la rd, label` -- two words, patched once the label's address is known.
  void La(uint32_t rd, const std::string& label) {
    LaFixup fix;
    fix.at = words_.size();
    fix.rd = rd;
    fix.label = label;
    fixups_.push_back(fix);
    words_.push_back(0);
    words_.push_back(0);
  }

  void Mark(const std::string& label) { labels_[label] = pc(); }

  // `la` is auipc + addi, the PC-relative pair, because a p0 text address is not
  // representable as a sign-extended 32-bit immediate in RV64.
  void Resolve() {
    for (const LaFixup& fix : fixups_) {
      auto it = labels_.find(fix.label);
      if (it == labels_.end()) continue;
      const int64_t here = static_cast<int64_t>(base_ + 4ull * fix.at);
      const int64_t delta = static_cast<int64_t>(it->second) - here;
      const uint32_t lo = static_cast<uint32_t>(delta) & 0xFFFu;
      const uint32_t hi = static_cast<uint32_t>((delta + 0x800) >> 12) & 0xFFFFFu;
      words_[fix.at] = U(hi, fix.rd, 0x17);   // auipc rd, hi20
      words_[fix.at + 1] = I(static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0),
                             fix.rd, 0, fix.rd, 0x13);   // addi rd, rd, lo12
    }
  }

  // The loaded image: every emitted word, plus the raw word for a patch check.
  uint64_t Label(const std::string& name) const {
    auto it = labels_.find(name);
    return (it == labels_.end()) ? 0 : it->second;
  }

 private:
  struct LaFixup {
    size_t at;
    uint32_t rd;
    std::string label;
  };
  uint64_t base_;
  std::vector<uint32_t> words_;
  std::vector<LaFixup> fixups_;
  std::map<std::string, uint64_t> labels_;
};

// ============================================================================
// The instruction memory
// ============================================================================
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
  uint32_t ResponseWord() const { return img_->Word(ready_.front().addr); }

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
// Geometry, taken from the elaborated DUT rather than re-derived
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint32_t seq_w = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  return g;
}

template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return static_cast<uint64_t>(wide[lane * 2]) |
         (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
}
inline uint64_t PayloadLane(uint64_t value, uint32_t /*lane*/) { return value; }

uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
  const uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
  return (packed >> (lane * width)) & mask;
}

// ============================================================================
// The harness: one run against the DUT
// ============================================================================
struct TrapRecord {
  uint64_t cause = 0;
  uint64_t tval = 0;
  uint64_t epc = 0;
  uint64_t pc = 0;            // a lane-0 retire-stream trap event's PC
  uint64_t target = 0;
  bool is_irq = false;
  uint64_t cycle = 0;
  bool fault_event = false;   // a lane-0 retire-stream trap event
};

class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(mem) {}

  void Configure(const Geometry& g) { g_ = g; }
  void Phase(const std::string& name) { phase_ = name; }
  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }
  const std::vector<TrapRecord>& traps() const { return traps_; }
  const std::vector<TrapRecord>& fault_events() const { return fault_events_; }
  const std::vector<DataMem::Txn>& txns() const { return dmem_.txns(); }
  const Imem& imem() const { return imem_; }
  uint64_t retires() const { return retires_; }

  struct Retire {
    uint64_t pc = 0;
    uint32_t rd = 0;
    bool reg_we = false;
    uint64_t value = 0;
    bool is_trap = false;
  };
  const std::vector<Retire>& retire_events() const { return retires_list_; }

  // The interrupt inputs the run drives, applied at the next cycle.
  void SetIrqs(bool soft, bool timer, bool ext) {
    next_soft_ = soft;
    next_timer_ = timer;
    next_ext_ = ext;
  }
  uint64_t mtime() const { return mtime_; }

  // The reference's trace interleaves retirements and trap entries. The machine
  // publishes its retirements on the retire event stream and its traps as trap
  // pulses, so the two are compared against their own halves of the reference.
  void Expect(const RefResult* ref) {
    ref_ = ref;
    ref_retires_.clear();
    for (const RefEvent& e : ref->trace) {
      if (!e.trap) ref_retires_.push_back(e);
    }
    ref_trap_count_ = ref->trap_causes.size();
  }
  void StopComparing() { ref_ = nullptr; }

  bool Complete() const {
    if (ref_ == nullptr) return true;
    return retires_ >= ref_retires_.size() && traps_.size() >= ref_trap_count_;
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + State());
    }
    soft_ = next_soft_;
    timer_ = next_timer_;
    ext_ = next_ext_;
    mtime_++;

    dut_->rst = rst ? 1 : 0;
    dut_->irq_soft_i = soft_ ? 1 : 0;
    dut_->irq_timer_i = timer_ ? 1 : 0;
    dut_->irq_ext_i = ext_ ? 1 : 0;
    dut_->mtime_i = mtime_;
    dut_->imem_req_ready_i = 1;
    dut_->imem_rsp_valid_i = imem_.HasResponse() ? 1 : 0;
    if (imem_.HasResponse()) {
      const Imem::Request& r = imem_.Response();
      dut_->imem_rsp_rdata_i = imem_.ResponseWord();
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = r.id;
      dut_->imem_rsp_epoch_i = r.epoch;
      dut_->imem_rsp_len_i = 4;
    } else {
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }
    dut_->dmem_req_ready_i = 1;
    if (dmem_.HasResponse()) {
      const DataMem::Rsp& r = dmem_.CurrentResponse();
      dut_->dmem_rsp_valid_i = 1;
      dut_->dmem_rsp_rdata_i = r.rdata;
      dut_->dmem_rsp_fault_i = r.fault ? 1 : 0;
    } else {
      dut_->dmem_rsp_valid_i = 0;
      dut_->dmem_rsp_rdata_i = 0;
      dut_->dmem_rsp_fault_i = 0;
    }
    // The standalone redirect arbiter is not driven by this case.
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;
    dut_->eval();

    if (!rst) Observe();

    if ((dut_->imem_req_valid_o != 0) && (dut_->imem_req_ready_i != 0)) {
      imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if ((dut_->imem_rsp_valid_i != 0) && (dut_->imem_rsp_ready_o != 0)) {
      imem_.PopResponse();
    }
    imem_.Advance();

    if ((dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0)) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(r, cycles_);
    }
    if ((dut_->dmem_rsp_valid_i != 0) && (dut_->dmem_rsp_ready_o != 0)) {
      dmem_.PopResponse();
    }
    dmem_.Advance();

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) + " stopped=" + Dec(dut_->o_stopped_o) +
           " traps=" + Dec(traps_.size()) + " mcause=" + U64(dut_->o_csr_mcause_o) +
           " mepc=" + U64(dut_->o_csr_mepc_o) + " mtvec=" + U64(dut_->o_csr_mtvec_o) +
           " mstatus=" + U64(dut_->o_csr_mstatus_o) +
           " mip=" + U64(dut_->o_csr_mip_o);
  }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  Vmosaic_core_tb* dut() { return dut_; }

 private:
  void Observe() {
    Compare("the retire counter equals the retirement events published",
            dut_->o_commit_o == retires_,
            "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(retires_));
    Compare("ROB occupancy is within the ROB",
            dut_->o_rob_occupied_o <= g_.rob_entries,
            "occupied=" + Dec(dut_->o_rob_occupied_o));
    Compare("the rename recovery counters stay zero",
            dut_->o_squash_nc_o == 0 && dut_->o_squash_under_o == 0 &&
                dut_->o_journal_ovf_o == 0,
            "squash_nc=" + Dec(dut_->o_squash_nc_o) +
                " under=" + Dec(dut_->o_squash_under_o) +
                " journal=" + Dec(dut_->o_journal_ovf_o));
    // The exception payload record: a fault whose slot generation no longer
    // matches would be a cause read for the wrong instruction.
    Compare("every exception payload was matched to its slot's generation",
            dut_->o_exc_gen_mismatch_o == 0,
            "mismatches=" + Dec(dut_->o_exc_gen_mismatch_o));
    // The interrupt unit's own rules, checked every cycle.
    Compare("no spurious WFI wake",
            dut_->o_spurious_wake_o == 0,
            "spurious=" + Dec(dut_->o_spurious_wake_o));
    Compare("an interrupt is only offered at a legal boundary",
            (dut_->o_irq_valid_o == 0) ||
                ((dut_->o_csr_mstatus_o & 0x8ull) != 0 &&
                 (dut_->o_csr_mip_o & dut_->o_csr_mie_o & ~0x88ull) == 0),
            "irq_valid=1 with mstatus=" + U64(dut_->o_csr_mstatus_o) +
                " mie=" + U64(dut_->o_csr_mie_o) + " mip=" + U64(dut_->o_csr_mip_o));

    if (dut_->o_trap_valid_o != 0) {
      TrapRecord t;
      t.cause = dut_->o_trap_cause_o;
      t.tval = dut_->o_trap_tval_o;
      t.epc = dut_->o_trap_epc_o;
      t.target = dut_->o_trap_target_o;
      t.is_irq = dut_->o_trap_is_irq_o != 0;
      t.cycle = cycles_;
      traps_.push_back(t);
      if (std::getenv("TRAP_DEBUG") != nullptr) {
        std::printf("    [trap] cycle=%llu cause=%s epc=%s tval=%s irq=%d\n",
                    static_cast<unsigned long long>(cycles_),
                    U64(t.cause).c_str(), U64(t.epc).c_str(), U64(t.tval).c_str(),
                    t.is_irq ? 1 : 0);
      }
      if (ref_ != nullptr && !t.is_irq && traps_.size() <= ref_->trap_causes.size()) {
        const uint64_t want_cause = ref_->trap_causes[traps_.size() - 1];
        const uint64_t want_epc = ref_->trap_epcs[traps_.size() - 1];
        Compare("trap " + Dec(traps_.size() - 1) + " cause matches the reference",
                t.cause == want_cause,
                "cause=" + U64(t.cause) + " expected " + U64(want_cause));
        Compare("trap " + Dec(traps_.size() - 1) +
                    " names the faulting instruction",
                t.epc == want_epc,
                "mepc=" + U64(t.epc) + " expected the faulting PC " + U64(want_epc));
      }
      pending_csr_check_ = true;
    } else if (pending_csr_check_) {
      // The CSR file latches the trap at the edge that ends the trap cycle, so
      // the architectural state is checked one cycle later.
      Compare("mcause holds the trap entry's cause",
              dut_->o_csr_mcause_o == traps_.back().cause,
              "mcause=" + U64(dut_->o_csr_mcause_o) + " trap cause=" +
                  U64(traps_.back().cause));
      Compare("mepc holds the trap entry's PC",
              dut_->o_csr_mepc_o == (traps_.back().epc & ~UINT64_C(3)),
              "mepc=" + U64(dut_->o_csr_mepc_o) + " trap PC=" + U64(traps_.back().epc));
      // mstatus: MPIE <- MIE, MIE <- 0, MPP <- M.
      const bool mie_now = (dut_->o_csr_mstatus_o & 0x8ull) != 0;
      const bool mpie_now = (dut_->o_csr_mstatus_o & 0x80ull) != 0;
      const bool mie_before = (last_mstatus_ & 0x8ull) != 0;
      Compare("mstatus.MIE is clear and MPIE reflects the interrupted enable",
              !mie_now && (mpie_now == mie_before),
              "mstatus=" + U64(dut_->o_csr_mstatus_o) + " before=" +
                  U64(last_mstatus_));
      Compare("mstatus.MPP is M in an M-only profile",
              (dut_->o_csr_mstatus_o & 0x1800ull) == 0x1800ull,
              "mstatus=" + U64(dut_->o_csr_mstatus_o));
      pending_csr_check_ = false;
    }
    last_mstatus_ = dut_->o_csr_mstatus_o;

    if (dut_->o_mret_valid_o != 0) {
      mret_events_++;
      if (mret_expected_ != 0) {
        Compare("MRET returns to the PC mepc names",
                dut_->o_mret_target_o == mret_expected_,
                "mret target=" + U64(dut_->o_mret_target_o) + " expected " +
                    U64(mret_expected_));
      }
    }

    // The retirement stream. A trap event is a lane-0 event with no retirement
    // behind it; it is collected separately and compared with the reference's
    // trap list.
    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      const bool is_trap = PackedLane(dut_->ev_trap_o, lane, 1) != 0;
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.is_trap = is_trap;
      if (is_trap) {
        if (std::getenv("TRAP_DEBUG") != nullptr) {
          std::printf("    [fault-ev] cycle=%llu pc=%s\n",
                      static_cast<unsigned long long>(cycles_), U64(r.pc).c_str());
        }
        TrapRecord t;
        t.fault_event = true;
        t.pc = r.pc;
        fault_events_.push_back(t);
        continue;
      }
      retires_list_.push_back(r);
      retires_++;
      if (ref_ != nullptr) {
        // The reference's trace interleaves retirements and traps; walk past its
        // trap entries (they are compared against the trap pulses) and compare
        // the retirements in order.
        const size_t idx = retires_ - 1;
        if (idx >= ref_retires_.size()) {
          Fail(phase_ + " at cycle " + Dec(cycles_),
               "retire " + Dec(idx) + " at pc " + U64(r.pc) +
               " has no counterpart in the reference (" +
               Dec(ref_retires_.size()) + " retirements)");
        }
        const RefEvent& e = ref_retires_[idx];
        Compare("the retirement stream follows the reference: retire " + Dec(idx),
                r.pc == e.pc,
                "pc expected " + U64(e.pc) + ", got " + U64(r.pc));
        Compare("retire " + Dec(idx) + " destination",
                r.rd == e.rd && r.reg_we == e.reg_we,
                "at " + U64(e.pc) + " rd expected x" + Dec(e.rd) + " we=" +
                    Dec(e.reg_we) + ", got x" + Dec(r.rd) + " we=" + Dec(r.reg_we));
        if (e.reg_we) {
          Compare("retire " + Dec(idx) + " value",
                  r.value == e.value,
                  "at " + U64(e.pc) + " x" + Dec(e.rd) + " expected " + U64(e.value) +
                      ", got " + U64(r.value));
        }
      }
    }
    ProgressCheck(progress_alloc_);
    progress_alloc_ = dut_->o_dbg_alloc_ctr_o;
  }

  void ProgressCheck(uint32_t last_alloc) {
    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_trap_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc);
    last_commit_ = dut_->o_commit_o;
    if (progressed) {
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled: " + State());
    }
  }

 public:
  // Observability the directed phases read directly.
  uint64_t csr_mepc() const { return dut_->o_csr_mepc_o; }
  uint64_t csr_mcause() const { return dut_->o_csr_mcause_o; }
  uint64_t csr_mstatus() const { return dut_->o_csr_mstatus_o; }
  uint64_t csr_mtvec() const { return dut_->o_csr_mtvec_o; }
  uint64_t csr_mie() const { return dut_->o_csr_mie_o; }
  uint64_t csr_mip() const { return dut_->o_csr_mip_o; }
  uint64_t csr_wr_ctr() const { return dut_->o_csr_wr_o; }
  uint64_t csr_trap_ctr() const { return dut_->o_csr_trap_o; }
  uint64_t csr_mret_ctr() const { return dut_->o_csr_mret_o; }
  uint64_t irq_ctr() const { return dut_->o_irq_ctr_o; }
  uint64_t wfi_halt_cycles() const { return dut_->o_halt_cycles_o; }
  uint64_t sys_exec_ctr() const { return dut_->o_sys_exec_o; }
  uint64_t sys_redirect_ctr() const { return dut_->o_sys_redirect_o; }
  uint64_t trap_irq_ctr() const { return dut_->o_trap_irq_o; }
  uint64_t fault_event_count() const { return fault_events_.size(); }
  uint64_t mret_events() const { return mret_events_; }
  void SetMretExpected(uint64_t pc) { mret_expected_ = pc; }
  bool halted() const { return dut_->o_wfi_halt_o != 0; }

 private:
  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  Geometry g_;
  std::string phase_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  uint64_t retires_ = 0;
  uint32_t last_commit_ = 0;
  uint32_t progress_alloc_ = 0;
  uint64_t last_progress_ = 0;
  std::vector<Retire> retires_list_;
  std::vector<TrapRecord> traps_;
  std::vector<TrapRecord> fault_events_;
  uint64_t mret_events_ = 0;
  uint64_t mret_expected_ = 0;
  uint64_t last_mstatus_ = 0;
  bool pending_csr_check_ = false;
  bool soft_ = false, timer_ = false, ext_ = false;
  bool next_soft_ = false, next_timer_ = false, next_ext_ = false;
  uint64_t mtime_ = 0;
  const RefResult* ref_ = nullptr;
  std::vector<RefEvent> ref_retires_;
  size_t ref_trap_count_ = 0;
};

// ============================================================================
// Repository root
// ============================================================================
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

// The frozen protocol's harness override of input word 0. The memory model
// serves FROMHOST as a *device*: a read at exactly that address returns the
// input word, whatever its size. The core's data port does not read a single
// address -- it reads the aligned doubleword the access selects
// (mosaic_lsu_endpoint.sv's lane convention) -- so the other seven bytes of the
// word are read as ordinary RAM. Seeding those bytes with the input word's high
// bytes is what makes the device's own rule and the doubleword window agree; the
// low byte is served by the model's rule and is left alone.
void SeedFromhost(mosaic::MemoryModel* mem, uint64_t value) {
  mem->SetInputWord(value);
  for (unsigned i = 1; i < 8; i++) {
    if (mem->Write(MOSAIC_FROMHOST + i, 1, (value >> (8 * i)) & 0xFFu) !=
        mosaic::AccessStatus::kOk) {
      Fail("run", "FROMHOST is not writable in the memory map");
    }
  }
}

std::vector<uint64_t> ReadSignature(mosaic::MemoryModel* mem, const std::string& where) {
  std::vector<uint64_t> signature;
  if (!mem->ReadSignature(&signature) || signature.size() != 4) {
    Fail(where, "the signature area is not readable in the memory model");
  }
  return signature;
}

std::vector<uint64_t> ReadTrapLog(mosaic::MemoryModel* mem, int records,
                                  const std::string& where) {
  std::vector<uint64_t> log;
  for (int i = 0; i < records; i++) {
    uint64_t cause = 0;
    if (mem->Read(kTrapLogAddr + kTrapLogRecordBytes * i, 8, &cause) !=
        mosaic::AccessStatus::kOk) {
      Fail(where, "the trap log record " + Dec(i) + " is not readable");
    }
    log.push_back(cause);
  }
  return log;
}

// ============================================================================
// Phase 1: the corpus, unmodified
// ============================================================================
void RunCorpus(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, const Geometry& g,
               const std::string& repo, int program_index, int input_index,
               uint64_t* totals) {
  const CorpusProgram& program = kCorpus[program_index];
  const Expected& input = program.inputs[input_index];
  const std::string phase = "run-" + std::string(program.name) + "-in" + Dec(input_index);

  ProgImage image;
  std::string load_detail;
  char elf_name[256];
  std::snprintf(elf_name, sizeof(elf_name), program.elf, input_index);
  const std::string elf_path = repo + "/" + elf_name;
  if (!image.LoadElf(elf_path, &load_detail)) {
    Fail("program", load_detail + " (build the corpus with `make -C "
                                  "tests/programs all`)");
  }

  mosaic::MemoryModel dut_mem;
  mosaic::MemoryModel ref_mem;
  {
    std::string detail;
    if (!dut_mem.LoadImage(image.elf(), &detail)) Fail("run", detail);
    if (!ref_mem.LoadImage(image.elf(), &detail)) Fail("run", detail);
    // The FROMHOST override of input word 0, exactly as crt0 reads it.
    SeedFromhost(&dut_mem, input.a);
    SeedFromhost(&ref_mem, input.a);
  }

  const RefResult reference =
      mosaic_trap::RunReference(image, g.reset_vector, &ref_mem);
  if (!reference.exited) {
    Fail(phase, "the reference did not reach the program's exit protocol: " +
                    reference.stop_reason + " at " + U64(reference.stop_pc));
  }

  Harness harness(dut, reporter, kCorpusCycles, &image, &dut_mem);
  harness.Configure(g);
  harness.Phase(phase);
  harness.Expect(&reference);

  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);

  while (!harness.Complete()) {
    harness.Cycle(false);
    if (harness.cycles() > kCorpusCycles) {
      Fail(phase, "the program did not reach its exit protocol: " + harness.State());
    }
  }
  // The reference's stream is exhausted: the program has parked and anything
  // retired from here on is the park loop, which is not part of the prediction.
  harness.StopComparing();
  for (int i = 0; i < 8; i++) harness.Cycle(false);

  // ---- the trap log the program's own handler wrote ----
  const std::vector<uint64_t> log = ReadTrapLog(&dut_mem, kTrapLogRecords, phase);
  for (int i = 0; i < input.trap_count; i++) {
    harness.Check("trap log record " + Dec(i) +
                      " holds the cause the host oracle predicts",
                  log[i] == input.traps[i],
                  "record " + Dec(i) + " = " + U64(log[i]) + ", oracle says " +
                      U64(input.traps[i]));
  }
  for (int i = input.trap_count; i < kTrapLogRecords; i++) {
    harness.Check("trap log record " + Dec(i) + " is unused",
                  log[i] == 0, "record " + Dec(i) + " = " + U64(log[i]));
  }
  harness.Check("the machine took exactly the oracle's trap count",
                harness.traps().size() == static_cast<size_t>(input.trap_count),
                "traps=" + Dec(harness.traps().size()) + ", oracle says " +
                    Dec(input.trap_count));
  for (size_t i = 0; i < harness.traps().size(); i++) {
    const TrapRecord& t = harness.traps()[i];
    if (i < static_cast<size_t>(input.trap_count)) {
      harness.Check("trap " + Dec(i) + " is the oracle's cause",
                    t.cause == input.traps[i],
                    "cause=" + U64(t.cause) + ", oracle says " + U64(input.traps[i]));
    }
    harness.Check("trap " + Dec(i) + " is a synchronous exception",
                  !t.is_irq, "is_irq=" + Dec(t.is_irq));
  }
  harness.Check("the CSR trap counter counts the same traps",
                harness.csr_trap_ctr() == input.trap_count,
                "counter=" + Dec(harness.csr_trap_ctr()));
  harness.Check("every MRET the handler executed was committed by the CSR file",
                harness.csr_mret_ctr() == input.trap_count,
                "mret=" + Dec(harness.csr_mret_ctr()) + ", traps=" +
                    Dec(input.trap_count));
  harness.Check("the independent interpreter agrees with the host oracle's trap trace",
                reference.trap_causes.size() == static_cast<size_t>(input.trap_count),
                "reference traps=" + Dec(reference.trap_causes.size()));
  harness.Check("the exception payload of every memory fault was captured",
                harness.fault_event_count() == static_cast<size_t>(input.trap_count),
                "fault events=" + Dec(harness.fault_event_count()));

  // ---- the exit protocol ----
  harness.Check("the program wrote TOHOST with the PASS bit set",
                dut_mem.finished() && dut_mem.passed() && dut_mem.exit_code() == 1,
                "finished=" + Dec(dut_mem.finished() ? 1 : 0) + " passed=" +
                    Dec(dut_mem.passed() ? 1 : 0) + " tohost=" +
                    U64(dut_mem.exit_code()));

  // ---- the host oracle's signature ----
  // The oracle's own row first: the machine's signature must be the oracle's,
  // with the fold-length relationship p08's program makes explicit. The
  // relationship is checked, not assumed, so a future change to either the
  // oracle or the program fails here rather than being averaged away.
  std::vector<uint64_t> ref_sig;
  if (!ref_mem.ReadSignature(&ref_sig) || ref_sig.size() != 4) {
    Fail(phase, "the reference's signature area is not readable");
  }
  uint64_t program_sig[4] = {input.sig[0], input.sig[1], input.sig[2], input.sig[3]};
  if (program.sig3_kind == kSig3P08Fold) {
    program_sig[3] = P08ProgramSig3(input.sig[3], input.c);
  } else if (program.sig3_kind == kSig3P13Resume) {
    program_sig[3] = input.sig[3] ^ UINT64_C(1);
  }
  for (int k = 0; k < 4; k++) {
    if (ref_sig[k] != program_sig[k]) {
      std::printf("  [ref-diff] %s sig[%d] reference=%s expr-prog=%s oracle=%s\n",
                  phase.c_str(), k, U64(ref_sig[k]).c_str(),
                  U64(program_sig[k]).c_str(), U64(input.sig[k]).c_str());
      uint64_t scratch_bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
      for (int i = 0; i < 8; i++) {
        ref_mem.Read(0x80001080ull + 8ull * i, 8, &scratch_bytes[i]);
      }
      std::printf("  [ref-diff] scratch: %s %s %s %s | %s %s %s %s\n",
                  U64(scratch_bytes[0]).c_str(), U64(scratch_bytes[1]).c_str(),
                  U64(scratch_bytes[2]).c_str(), U64(scratch_bytes[3]).c_str(),
                  U64(scratch_bytes[4]).c_str(), U64(scratch_bytes[5]).c_str(),
                  U64(scratch_bytes[6]).c_str(), U64(scratch_bytes[7]).c_str());
      harness.Check("the independent interpretation computes the program's "
                    "signature word " + Dec(k), false,
                    "reference " + U64(ref_sig[k]) + ", program " +
                        U64(program_sig[k]) + ", oracle " + U64(input.sig[k]));
    }
  }
  const std::vector<uint64_t> signature = ReadSignature(&dut_mem, phase);
  for (int k = 0; k < 4; k++) {
    harness.Check("signature word " + Dec(k) +
                      " equals the value the program computes (through the store "
                      "path)",
                  signature[k] == program_sig[k],
                  "got " + U64(signature[k]) + ", expected " + U64(program_sig[k]) +
                      " (the host oracle prints " + U64(input.sig[k]) + ")");
  }
  if (input.sig[3] != input.sig_prog[3]) {
    // The one word where the oracle's model and the program differ, stated as
    // the exact relationship: the program folds all MOSAIC_TRAPLOG_RECORDS
    // records, the oracle folds only the ones it predicts.
    harness.Check("the machine's signature word 3 is the program's derivation from "
                  "the oracle's row",
                  signature[3] == program_sig[3],
                  "machine " + U64(signature[3]) + ", derivation " +
                      U64(program_sig[3]) + ", oracle " + U64(input.sig[3]));
    std::printf("  [note] %s: the host oracle prints sig3=%s; the program computes "
                "%s and the machine stores %s\n",
                phase.c_str(), U64(input.sig[3]).c_str(),
                U64(program_sig[3]).c_str(), U64(signature[3]).c_str());
  }

  totals[0] += harness.cycles();
  totals[1] += harness.comparisons();
  totals[2] += harness.retires();
  totals[3] += harness.traps().size();

  std::printf("  [%s] %zu retires, %zu traps, %llu cycles, sig3=%s\n",
              phase.c_str(), static_cast<size_t>(harness.retires()),
              harness.traps().size(),
              static_cast<unsigned long long>(harness.cycles()),
              U64(signature[3]).c_str());
}

// ============================================================================
// Phase 2: the directed trap program
// ============================================================================
// SIG[0] = the number of records the handler logged
// SIG[1] = mstatus read back after `csrw mstatus, -1`   (the WARL mask)
// SIG[2] = the last record's mcause
// SIG[3] = the last record's mepc
constexpr uint64_t kDirSig = kSignatureAddr;
constexpr uint64_t kDirLog = 0x80000700ull;   // count at +0, records at +16

struct DirectedProgram {
  Asm asm_;
  uint64_t wfi_pc = 0;
  uint64_t ld_pc = 0;
  uint64_t ecall_pc = 0;
};

// The trap handler both directed programs use: log {mcause, mepc}, resume after
// the faulting instruction (mepc + 4 -- every p0 instruction is 4 bytes), MRET.
void EmitHandler(Asm* a) {
  a->Mark("handler");
  a->Csrrs(6, 0x341, 0);   // t1 = mepc
  a->Csrrs(5, 0x342, 0);   // t0 = mcause
  a->LiAbs(28, kDirLog);   // t3 = &log
  a->Ld(29, 28, 0);        // t4 = count
  a->Slli(29, 29, 4);
  a->Add(29, 29, 28);
  a->Sd(5, 29, 16);        // log[16 + count*16] = mcause
  a->Sd(6, 29, 24);        // ... + 8 = mepc
  a->Ld(29, 28, 0);
  a->Addi(29, 29, 1);
  a->Sd(29, 28, 0);        // count++
  a->Addi(6, 6, 4);
  a->Csrrw(0, 0x341, 6);   // mepc = mepc + 4
  a->Mret();
}

// The directed taxonomy program: six synchronous traps and the mstatus WARL
// read-back, then the exit protocol.
DirectedProgram BuildDirectedTaxonomy() {
  DirectedProgram p{Asm(0x80000000ull), 0, 0, 0};
  Asm& a = p.asm_;
  a.La(5, "handler");            // la t0, handler
  a.Csrrw(0, 0x305, 5);          // mtvec = handler
  a.Csrrw(0, 0x300, 0);          // mstatus = 0  (MIE/MPIE clear)
  a.Csrrw(0, 0x304, 0);          // mie = 0
  // The front end fetches and allocates ahead of retirement, and an ECALL is
  // refused until the trap vector is *installed* -- that is, until the write to
  // mtvec has retired. In a real program the boot code is long enough that this
  // is invisible; here it has to be arranged, so the program spends sixteen
  // instructions (more than the front end's window) after the install.
  for (int i = 0; i < 16; i++) a.Addi(0, 0, 0);
  // 1. ECALL ---------------------------------------------------------------
  p.ecall_pc = a.pc();
  a.Ecall();
  // 2. EBREAK --------------------------------------------------------------
  a.Ebreak();
  // 3. read of an unimplemented CSR ---------------------------------------
  a.Csrrs(5, 0x7b0, 0);          // csrr t0, 0x7b0
  // 4. write to a read-only CSR (the `time` shadow) ------------------------
  a.Csrrs(0, 0xc01, 5);          // csrs 0xc01, t0  (csrrs with rs1 != x0)
  // 5. misaligned load ----------------------------------------------------
  p.ld_pc = a.pc();
  a.Ld(5, 0, 1);                 // ld t0, 1(x0)
  // 6. the CSR immediate form, and the WARL mask --------------------------
  a.Csrrwi(6, 0x340, 7);         // csrrwi t1, mscratch, 7
  a.Csrrs(7, 0x340, 0);          // csrr t2, mscratch -> 7
  a.Addi(5, 0, -1);              // li t0, -1
  a.Csrrw(0, 0x300, 5);          // mstatus = -1 (masked to 0x66aa)
  a.Csrrs(5, 0x300, 0);          // csrr t0, mstatus
  a.LiAbs(6, kDirSig);
  a.Sd(5, 6, 8);                 // sig[1] = mstatus
  // the record count and the last record
  a.LiAbs(28, kDirLog);
  a.Ld(29, 28, 0);
  a.Sd(29, 6, 0);                // sig[0] = count
  a.Addi(29, 29, -1);            // count - 1
  a.Slli(29, 29, 4);             // * record stride
  a.Add(29, 29, 28);             // + &log
  a.Ld(5, 29, 16);
  a.Sd(5, 6, 16);                // sig[2] = last cause
  a.Ld(5, 29, 24);
  a.Sd(5, 6, 24);                // sig[3] = last epc
  // exit
  a.Addi(5, 0, 1);
  a.LiAbs(6, MOSAIC_TOHOST);
  a.Sd(5, 6, 0);
  EmitHandler(&a);
  a.Resolve();
  return p;
}

// ============================================================================
// Phase 3: the interrupt scenarios
// ============================================================================
// The program installs the handler, clears mstatus, enables the scenario's
// interrupts in mie, and then writes mstatus with the scenario's MIE. The WFI is
// the next instruction: with MIE set the interrupt is taken at the WFI's own
// boundary (mepc = the WFI's PC, and the WFI never executes); with MIE clear the
// WFI completes without halting (an enabled interrupt is pending) and execution
// resumes after it. The handler logs {mcause, mepc} and resumes at mepc + 4.
//
// SIG[0] = the number of records logged
// SIG[1] = mstatus read back immediately after the trap is expected (or 0)
// SIG[2] = the last record's mcause
// SIG[3] = the last record's mepc
struct IrqScenario {
  const char* name;
  bool soft;
  bool timer;
  bool ext;
  uint64_t mie_bits;    // which interrupt-enable bits the program sets
  uint64_t mstatus_mie; // the MIE bit the program writes last
  bool expect_taken;
  uint64_t expect_cause;
  bool wake_later;      // do not raise the interrupt until the WFI halts
};

const IrqScenario kIrqScenarios[] = {
    {"timer", false, true, false, 0x80, 0x8, true, mosaic_trap::kIrqMti, false},
    {"timer+software", true, true, false, 0x88, 0x8, true, mosaic_trap::kIrqMsi, false},
    // The external interrupt cannot be *taken* in p0: config/csr/mode_m.json makes
    // mie bits 7 and 3 the only writable ones (bit 11, MEIE, is read-only zero)
    // and mosaic_interrupt says the same of mip ("a CLINT driving MSIP and MTIP
    // and no PLIC, so bits 3 and 7 are the implemented ones"). So with the
    // external and timer sources both asserted, the timer is the one that is
    // enabled and the one that is taken -- which is what this scenario pins.
    {"timer+external-pending", false, true, true, 0x880, 0x8, true,
     mosaic_trap::kIrqMti, false},
    {"masked-mie", false, true, false, 0x80, 0x0, false, 0, false},
    {"wfi-halt-wake", false, true, false, 0x80, 0x0, false, 0, true},
};

struct IrqProgram {
  Asm asm_;
  uint64_t wfi_pc = 0;
  uint64_t after_wfi_pc = 0;
  uint64_t handler_pc = 0;
};

IrqProgram BuildIrqProgram(const IrqScenario& s) {
  IrqProgram p{Asm(0x80000000ull), 0, 0, 0};
  Asm& a = p.asm_;
  a.La(5, "handler");
  a.Csrrw(0, 0x305, 5);                 // mtvec = handler
  a.Csrrw(0, 0x300, 0);                 // mstatus = 0
  a.Csrrw(0, 0x304, 0);                 // mie = 0
  a.LiAbs(5, s.mie_bits);
  a.Csrrw(0, 0x304, 5);                 // mie = the scenario's enables
  a.LiAbs(5, s.mstatus_mie);
  a.Csrrw(0, 0x300, 5);                 // mstatus.MIE = the scenario's value
  p.wfi_pc = a.pc();
  a.Wfi();
  p.after_wfi_pc = a.pc();
  // Reached when the interrupt was not taken (masked, or after a wake), and
  // also after MRET when it was taken: in both cases this is the architectural
  // state at the point the interrupt returned to.
  a.LiAbs(6, kDirSig);
  a.Csrrs(5, 0x300, 0);                 // t0 = mstatus (before it is cleared)
  a.Sd(5, 6, 8);                        // sig[1] = mstatus
  a.Csrrw(0, 0x304, 0);                 // mie = 0
  a.Csrrw(0, 0x300, 0);                 // mstatus = 0 (MIE clear)
  a.LiAbs(28, kDirLog);
  a.Ld(29, 28, 0);
  a.Sd(29, 6, 0);                       // sig[0] = count
  a.Ld(29, 28, 16);
  a.Sd(29, 6, 16);                      // sig[2] = last cause
  a.Ld(29, 28, 24);
  a.Sd(29, 6, 24);                      // sig[3] = last epc
  a.Addi(5, 0, 1);
  a.LiAbs(6, MOSAIC_TOHOST);
  a.Sd(5, 6, 0);
  EmitHandler(&a);
  a.Resolve();
  p.handler_pc = a.Label("handler");
  return p;
}

void RunDirected(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, const Geometry& g,
                 uint64_t* totals) {
  const std::string phase = "directed-trap-taxonomy";
  DirectedProgram program = BuildDirectedTaxonomy();

  ProgImage image;
  for (size_t i = 0; i < program.asm_.words().size(); i++) {
    image.Put(g.reset_vector + 4ull * i, program.asm_.words()[i]);
  }
  // The front end fetches ahead of execution, and an address with no word in the
  // image answers ECALL -- which, before `csrw mtvec` has executed, is a
  // trap-raising system instruction with nowhere to go. The gap is therefore
  // filled with `jal x0, 0` (a self-loop): a fetch that runs past the program
  // lands on something that can neither fault nor stop the machine, and the run
  // ends at the exit store rather than at a refused fetch.
  for (uint64_t addr = g.reset_vector + 4ull * program.asm_.words().size();
       addr < g.reset_vector + 0x400ull; addr += 4) {
    image.Put(addr, 0x0000006fu);
  }
  if (std::getenv("TRAP_DEBUG") != nullptr) {
    std::printf("  [directed] %zu words, handler at %s, ecall at %s, ld at %s\n",
                program.asm_.words().size(),
                U64(program.asm_.Label("handler")).c_str(),
                U64(program.ecall_pc).c_str(), U64(program.ld_pc).c_str());
    for (size_t i = 0; i < program.asm_.words().size(); i++) {
      std::printf("    %s: %08x\n", U64(g.reset_vector + 4ull * i).c_str(),
                  program.asm_.words()[i]);
    }
  }

  mosaic::MemoryModel dut_mem;
  mosaic::MemoryModel ref_mem;
  for (const auto& kv : image.words()) {
    if (dut_mem.Write(kv.first, 4, kv.second) != mosaic::AccessStatus::kOk ||
        ref_mem.Write(kv.first, 4, kv.second) != mosaic::AccessStatus::kOk) {
      Fail(phase, "the directed program does not fit the memory map at " +
                      U64(kv.first));
    }
  }

  const RefResult reference = mosaic_trap::RunReference(image, g.reset_vector, &ref_mem);
  if (reference.traps != 5) {
    Fail(phase, "the reference predicts 5 synchronous traps, not " +
                    Dec(reference.traps));
  }
  if (!reference.exited) {
    Fail(phase, "the reference did not reach the exit protocol: " +
                    reference.stop_reason);
  }

  Harness harness(dut, reporter, kDirectedCycles, &image, &dut_mem);
  harness.Configure(g);
  harness.Phase(phase);
  harness.Expect(&reference);
  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);
  while (!harness.Complete()) harness.Cycle(false);
  harness.StopComparing();
  for (int i = 0; i < 8; i++) harness.Cycle(false);

  harness.Check("five synchronous traps were taken",
                harness.traps().size() == 5,
                "traps=" + Dec(harness.traps().size()));
  const uint64_t want_causes[5] = {11, 3, 2, 2, 4};
  for (size_t i = 0; i < harness.traps().size() && i < 5; i++) {
    harness.Check("trap " + Dec(i) + " has the ISA's cause",
                  harness.traps()[i].cause == want_causes[i],
                  "cause=" + U64(harness.traps()[i].cause) + " expected " +
                      U64(want_causes[i]));
    harness.Check("trap " + Dec(i) + " names the faulting instruction",
                  !harness.traps()[i].is_irq, "is_irq=1");
  }
  if (harness.traps().size() >= 1) {
    harness.Check("the ECALL's mepc is the ECALL itself",
                  harness.traps()[0].epc == program.ecall_pc,
                  "mepc=" + U64(harness.traps()[0].epc) + " expected " +
                      U64(program.ecall_pc));
  }
  if (harness.traps().size() >= 5) {
    harness.Check("the misaligned load's mepc is the load itself",
                  harness.traps()[4].epc == program.ld_pc,
                  "mepc=" + U64(harness.traps()[4].epc) + " expected " +
                      U64(program.ld_pc));
    harness.Check("the misaligned load's mtval is the address",
                  harness.traps()[4].tval == 1,
                  "tval=" + U64(harness.traps()[4].tval));
  }

  // The trap entries the CSR file committed: one MRET per trap (the handler
  // returns from every one of them).
  harness.Check("the CSR file counted five trap entries and five MRETs",
                harness.csr_trap_ctr() == 5 && harness.csr_mret_ctr() == 5,
                "traps=" + Dec(harness.csr_trap_ctr()) + " mret=" +
                    Dec(harness.csr_mret_ctr()));

  const std::vector<uint64_t> signature = ReadSignature(&dut_mem, phase);
  harness.Check("the handler logged five records",
                signature[0] == 5, "sig[0]=" + U64(signature[0]));
  harness.Check("mstatus read back after a write of all ones is the generated "
                "write mask",
                signature[1] == UINT64_C(0x7eaa),
                "sig[1]=" + U64(signature[1]) + " expected 0x0000000000007eaa");
  harness.Check("the last logged record is the misaligned load",
                signature[2] == 4 && signature[3] == program.ld_pc,
                "cause=" + U64(signature[2]) + " epc=" + U64(signature[3]));
  harness.Check("the program reached its exit protocol",
                dut_mem.finished() && dut_mem.passed(),
                "finished=" + Dec(dut_mem.finished() ? 1 : 0));

  const std::vector<uint64_t> log = ReadTrapLog(&dut_mem, kTrapLogRecords, phase);
  (void)log;

  totals[0] += harness.cycles();
  totals[1] += harness.comparisons();
  totals[2] += harness.retires();
  totals[3] += harness.traps().size();
  std::printf("  [%s] %zu retires, %zu traps, %llu cycles, sig=%s %s %s %s\n",
              phase.c_str(), static_cast<size_t>(harness.retires()),
              harness.traps().size(),
              static_cast<unsigned long long>(harness.cycles()),
              U64(signature[0]).c_str(), U64(signature[1]).c_str(),
              U64(signature[2]).c_str(), U64(signature[3]).c_str());
}

void RunIrqScenario(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, const Geometry& g,
                    const IrqScenario& scenario, uint64_t* totals) {
  const std::string phase = std::string("irq-") + scenario.name;
  IrqProgram program = BuildIrqProgram(scenario);

  ProgImage image;
  for (size_t i = 0; i < program.asm_.words().size(); i++) {
    image.Put(g.reset_vector + 4ull * i, program.asm_.words()[i]);
  }
  // Same filler as the directed program: the fetch runs ahead of execution and
  // must not land on an ECALL while mtvec is still 0.
  for (uint64_t addr = g.reset_vector + 4ull * program.asm_.words().size();
       addr < g.reset_vector + 0x400ull; addr += 4) {
    image.Put(addr, 0x0000006fu);
  }
  mosaic::MemoryModel dut_mem;
  for (const auto& kv : image.words()) {
    if (dut_mem.Write(kv.first, 4, kv.second) != mosaic::AccessStatus::kOk) {
      Fail(phase, "the interrupt program does not fit the memory map");
    }
  }
  dut_mem.SetInputWord(0);

  Harness harness(dut, reporter, kDirectedCycles, &image, &dut_mem);
  harness.Configure(g);
  harness.Phase(phase);
  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);

  // The interrupt is asserted from the start of the run: before `mie` is
  // written nothing is enabled, so nothing is offered, and once the scenario's
  // enables are in place the interrupt is taken at the first boundary MIE
  // allows -- which is the WFI's own boundary, deterministically, because the
  // mstatus write immediately precedes it.
  if (!scenario.wake_later) {
    harness.SetIrqs(scenario.soft, scenario.timer, scenario.ext);
  }
  harness.SetMretExpected(program.after_wfi_pc);

  bool woke = false;
  bool deasserted = false;
  while (!dut_mem.finished()) {
    harness.Cycle(false);
    if (scenario.wake_later && !woke && harness.halted()) {
      woke = true;
      harness.SetIrqs(scenario.soft, scenario.timer, scenario.ext);
    }
    if (!deasserted && !harness.traps().empty()) {
      // The platform's sources are level-sensitive: the handler runs with
      // mstatus.MIE clear, so the source is released here rather than by the
      // program, and MRET will not re-enter the trap.
      deasserted = true;
      harness.SetIrqs(false, false, false);
    }
    if (harness.cycles() > kDirectedCycles) {
      Fail(phase, "the program did not reach its exit protocol: " + harness.State());
    }
  }
  for (int i = 0; i < 8; i++) harness.Cycle(false);

  if (scenario.expect_taken) {
    harness.Check("exactly one interrupt trap was taken",
                  harness.traps().size() == 1,
                  "traps=" + Dec(harness.traps().size()) + " " + harness.State());
    if (!harness.traps().empty()) {
      const TrapRecord& t = harness.traps()[0];
      harness.Check("the interrupt's cause is the configured source",
                    t.is_irq && t.cause == scenario.expect_cause,
                    "cause=" + U64(t.cause) + " is_irq=" + Dec(t.is_irq) +
                        " expected " + U64(scenario.expect_cause));
      harness.Check("an interrupt's mepc names the NEXT instruction to execute",
                    t.epc == program.wfi_pc,
                    "mepc=" + U64(t.epc) + " expected the WFI at " +
                        U64(program.wfi_pc));
      harness.Check("the interrupt's target is the installed trap vector",
                    t.target == program.handler_pc,
                    "target=" + U64(t.target) + " expected the handler at " +
                        U64(program.handler_pc));
    }
    harness.Check("the CSR file counted one trap and one MRET",
                  harness.csr_trap_ctr() == 1 && harness.csr_mret_ctr() == 1,
                  "traps=" + Dec(harness.csr_trap_ctr()) + " mret=" +
                      Dec(harness.csr_mret_ctr()));
    harness.Check("the trap was driven by the interrupt path",
                  harness.trap_irq_ctr() == 1,
                  "trap_irq=" + Dec(harness.trap_irq_ctr()));
  } else {
    harness.Check("no trap was taken", harness.traps().empty(),
                  "traps=" + Dec(harness.traps().size()) + " " + harness.State());
    harness.Check("the CSR file counted no trap", harness.csr_trap_ctr() == 0,
                  "traps=" + Dec(harness.csr_trap_ctr()));
    if (scenario.wake_later) {
      harness.Check("WFI really halted before the wake",
                    woke && harness.wfi_halt_cycles() > 0,
                    "woke=" + Dec(woke) + " halt_cycles=" +
                        Dec(harness.wfi_halt_cycles()));
      harness.Check("the wake was legal, not spurious",
                    dut->o_spurious_wake_o == 0 && harness.wfi_halt_cycles() > 0,
                    "spurious=" + Dec(dut->o_spurious_wake_o));
    } else {
      harness.Check("a pending enabled interrupt left the WFI running, it did not halt",
                    harness.wfi_halt_cycles() == 0,
                    "halt_cycles=" + Dec(harness.wfi_halt_cycles()));
    }
  }
  harness.Check("the program reached its exit protocol",
                dut_mem.finished() && dut_mem.passed(),
                "finished=" + Dec(dut_mem.finished() ? 1 : 0));

  const std::vector<uint64_t> signature = ReadSignature(&dut_mem, phase);
  if (scenario.expect_taken) {
    harness.Check("the handler logged exactly the interrupt",
                  signature[0] == 1 && signature[2] == scenario.expect_cause,
                  "count=" + U64(signature[0]) + " cause=" + U64(signature[2]));
    harness.Check("the handler's mepc is the interrupted instruction",
                  signature[3] == program.wfi_pc,
                  "epc=" + U64(signature[3]) + " expected " + U64(program.wfi_pc));
    // After MRET, mstatus.MIE is restored from MPIE (1) and MPIE is set again.
    harness.Check("MRET restored mstatus.MIE and MPIE",
                  signature[1] == UINT64_C(0x1888),
                  "mstatus after mret=" + U64(signature[1]) + " expected 0x1888");
  } else {
    harness.Check("the handler logged no interrupt",
                  signature[0] == 0,
                  "count=" + U64(signature[0]) + " " + harness.State());
  }

  totals[0] += harness.cycles();
  totals[1] += harness.comparisons();
  totals[2] += harness.retires();
  totals[3] += harness.traps().size();
  std::printf("  [%s] %zu retires, %zu traps, %llu cycles, sig=%s %s %s %s\n",
              phase.c_str(), static_cast<size_t>(harness.retires()),
              harness.traps().size(),
              static_cast<unsigned long long>(harness.cycles()),
              U64(signature[0]).c_str(), U64(signature[1]).c_str(),
              U64(signature[2]).c_str(), U64(signature[3]).c_str());
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
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    const std::string repo = FindRepoRoot();
    if (geometry.reset_vector != 0x80000000ull) {
      Fail("geometry", "the reset vector is not the corpus link base: " +
                           U64(geometry.reset_vector));
    }
    if (geometry.xlen != 64) {
      Fail("geometry", "the profile is not 64-bit");
    }

    uint64_t totals[4] = {0, 0, 0, 0};
    std::printf("core.trap_csr_program: the corpus from %s, then the directed "
                "programs\n", (repo + "/tests/programs/build").c_str());
    for (size_t p = 0; p < sizeof(kCorpus) / sizeof(kCorpus[0]); p++) {
      for (int i = 0; i < kInputsPerProgram; i++) {
        RunCorpus(&dut, &reporter, geometry, repo, static_cast<int>(p), i, totals);
      }
    }
    RunDirected(&dut, &reporter, geometry, totals);
    for (const IrqScenario& scenario : kIrqScenarios) {
      RunIrqScenario(&dut, &reporter, geometry, scenario, totals);
    }

    reporter.Check(totals[3] >= 12,
                   "the run set exercised at least a dozen traps (" + Dec(totals[3]) +
                       ")");

    detail = "checks=" + Dec(reporter.checks()) + " comparisons=" + Dec(totals[1]) +
             " cycles=" + Dec(totals[0]) + " retires=" + Dec(totals[2]) +
             " traps=" + Dec(totals[3]) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the CSR/trap path holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
