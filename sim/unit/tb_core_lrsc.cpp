// ============================================================================
// tb_core_lrsc.cpp -- CASE=lrsc.reservation_progress, work package I-040.
//
// The DUT is the integrated p0 core with the LR/SC path wired in: `lr`/`sc` are
// decoded in the core's front end (so CASE=decode.rv64im_reserved's reserved set
// is untouched), issued through the load queue as one more atomic class (which is
// what stops a younger load from overtaking them and what stops the queue from
// answering them out of the store queue), serialized at the ROB head, and
// performed by mosaic_lsu_endpoint together with mosaic_reservation. The endpoint
// owns the hart's reservation, the granule rule, the four invalidation sources,
// and the one-write-or-no-write behaviour of a store-conditional.
//
// ------------------------------------------------------------ the expectation
//
// Every expected value in this file is derived from the ISA, not from the DUT:
// the program is assembled here instruction by instruction and the value each LR
// and each SC must produce is written down next to it. There is no interpreter
// and no comparison against a previously recorded run. Four independent views
// are then required to agree:
//
//   1. the **retirement stream** -- the LR at a given PC retires with rd = the
//      value the memory held, the SC at a given PC retires with rd = 0 or 1;
//   2. the **data-port history**, replayed against a shadow memory built from
//      the same ISA rules -- every read returns what the history holds, every
//      write lands where the instruction said, and an AMO's write finds the
//      location unchanged since its read;
//   3. the **number of writes**, which must equal the number of SCs the
//      architecture says succeeded -- an SC that fails performs no memory access
//      at all, so the architectural status and the memory traffic cannot
//      disagree;
//   4. the **reservation state**, sampled while it stands, whose granule must be
//      the declared naturally aligned 64-byte block.
//
// ------------------------------------------------------------------- the runs
//
//   run A -- the reservation semantics, no interference. A hand-built program
//            exercises, in order: an SC with no reservation at all (it must not
//            write); an LR that returns the old value and establishes a
//            reservation; the SC that succeeds exactly once and writes exactly
//            one value; a second SC that fails because the first consumed the
//            reservation; a store *inside* the granule that breaks it; a store
//            *outside* the granule that must not; an AMO that breaks it; and an
//            ECALL taken between the LR and the SC (an exception) that breaks it.
//
//   run B -- the constrained LR/SC loop and its progress condition. The loop is
//            `lr; nops; sc; bnez x2, loop`: the exact shape the ISA permits to
//            fail but requires to succeed eventually. The harness plays the
//            second agent -- whenever the hart holds a reservation and the
//            memory port is free it performs one ordinary 8-byte write to the
//            granule and notifies the DUT through `ext_write_valid_i` (the
//            coherence notification; with one hart and no cache this port is the
//            invalidation interface a fabric would otherwise supply). The
//            stimulus is bounded, and the loop must then succeed, so the run's
//            own numbers decide whether the progress condition held:
//
//              attempts == interfering writes + 1,   attempts <= N
//
//            with N = 16 the declared bound. The case fails loudly if the
//            stimulus itself injected N or more conflicting writes -- that would
//            violate the assumption the bound is stated under -- and if the loop
//            did not terminate inside the bound.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic_ref::DataMem;

namespace {

constexpr int kResetCycles = 4;
constexpr int kMaxRunCycles = 200000;
constexpr int kStallCycles = 20000;

// The declared forward-progress bound: a constrained LR/SC loop with no other
// agent writing the granule must succeed within N attempts. This implementation
// never fails spuriously, so the observed bound is one attempt per interfering
// write plus the final one; N is the ceiling the case holds it to.
constexpr uint64_t kProgressBound = 16;

// The program's layout. RAM base plus a small offset; the base is built once
// with a shift because `lui` sign-extends on RV64 and cannot materialise
// 0x80000000.
constexpr uint64_t kRamBase    = 0x80000000ull;  // = MOSAIC_RAM_BASE
constexpr uint64_t kAOff       = 0x400;          // the reserved / SC address
constexpr uint64_t kA8Off      = 0x408;          // inside A's 64-byte granule
constexpr uint64_t kA64Off     = 0x440;          // the next granule
constexpr uint64_t kAttOff     = 0x4C0;          // the attempt counter (run B)
constexpr uint64_t kHandlerOff = 0x600;          // the trap handler
constexpr uint64_t kA     = kRamBase + kAOff;
constexpr uint64_t kA8    = kRamBase + kA8Off;
constexpr uint64_t kA64   = kRamBase + kA64Off;
constexpr uint64_t kAtt   = kRamBase + kAttOff;
constexpr uint64_t kTohost = 0x80001000;         // the frozen exit protocol

// The values the program moves around, named so an expectation is readable.
constexpr uint64_t kSeedVal  = 0x11;
constexpr uint64_t kVal2     = 0x22;
constexpr uint64_t kInVal    = 0x33;
constexpr uint64_t kOutVal   = 0x44;
constexpr uint64_t kVal5     = 0x55;
constexpr uint64_t kAmoDelta = 0x1;
constexpr uint64_t kLoopVal  = 0x77;

// Both programs write architectural register x2 *only* from an SC, so counting
// the retirements of x2 counts the store-conditionals and their statuses.
constexpr uint32_t kScRd = 2;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// Instruction encoding
// ============================================================================
uint32_t EncI(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm) {
  return op | (rd << 7) | (f3 << 12) | (rs1 << 15) |
         ((static_cast<uint32_t>(imm) & 0xFFFu) << 20);
}
uint32_t EncS(uint32_t op, uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return op | ((u & 0x1Fu) << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) |
         (((u >> 5) & 0x7Fu) << 25);
}
uint32_t EncB(uint32_t op, uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off);
  return op | (((u >> 11) & 1u) << 7) | (((u >> 1) & 0xFu) << 8) | (f3 << 12) |
         (rs1 << 15) | (rs2 << 20) | (((u >> 5) & 0x3Fu) << 25) |
         (((u >> 12) & 1u) << 31);
}
uint32_t EncJ(uint32_t op, uint32_t rd, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off);
  return op | (rd << 7) | (((u >> 12) & 0xFFu) << 12) | (((u >> 11) & 1u) << 20) |
         (((u >> 1) & 0x3FFu) << 21) | (((u >> 20) & 1u) << 31);
}
uint32_t EncR(uint32_t op, uint32_t f3, uint32_t rd, uint32_t rs1, uint32_t rs2,
              uint32_t f7) {
  return op | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (f7 << 25);
}
uint32_t EncAmo(uint32_t f5, uint32_t aq, uint32_t rl, uint32_t f3, uint32_t rd,
                uint32_t rs1, uint32_t rs2) {
  return 0x2Fu | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) |
         (rl << 25) | (aq << 26) | (f5 << 27);
}
uint32_t EncCsr(uint32_t f3, uint32_t rd, uint32_t rs1, uint32_t csr) {
  return 0x73u | (rd << 7) | (f3 << 12) | (rs1 << 15) | (csr << 20);
}

constexpr uint32_t OP_IMM    = 0x13u;
constexpr uint32_t OP_STORE  = 0x23u;
constexpr uint32_t OP_BRANCH = 0x63u;
constexpr uint32_t OP_JAL    = 0x6Fu;
constexpr uint32_t OP_ALU    = 0x33u;

constexpr uint32_t F3_D = 3;
constexpr uint32_t F5_LR = 0x02, F5_SC = 0x03, F5_AMOADD = 0x00;
constexpr uint32_t CSR_MTVEC = 0x305, CSR_MEPC = 0x341;

// ============================================================================
// The program image and the expectations derived from the ISA
// ============================================================================
struct Expect {
  uint32_t rd = 0;
  uint64_t value = 0;
};

class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    if (it == words_.end()) return 0x00000073u;  // ECALL: stop cleanly
    return it->second;
  }
  bool Has(uint64_t addr) const { return words_.count(addr) != 0; }

 private:
  std::map<uint64_t, uint32_t> words_;
};

// A tiny assembler that also records what the ISA says each interesting
// instruction must produce.
class Asm {
 public:
  Asm(ProgImage* img, std::map<uint64_t, Expect>* expect, uint64_t start)
      : img_(img), expect_(expect), pc_(start) {}

  uint64_t pc() const { return pc_; }

  void emit(uint32_t word) {
    img_->Put(pc_, word);
    pc_ += 4;
  }
  void emit_expect(uint32_t word, uint32_t rd, uint64_t value) {
    (*expect_)[pc_] = Expect{rd, value};
    emit(word);
  }
  void EmitI(uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm) {
    emit(EncI(OP_IMM, f3, rd, rs1, imm));
  }
  void EmitStoreD(uint32_t rs1, uint32_t rs2, int32_t imm) {
    emit(EncS(OP_STORE, F3_D, rs1, rs2, imm));
  }
  void EmitLrD(uint32_t rd, uint32_t rs1, uint64_t expect_value) {
    emit_expect(EncAmo(F5_LR, 0, 0, F3_D, rd, rs1, 0), rd, expect_value);
  }
  // `rd` = the status, `rs2` = the data, `rs1` = the address.
  void EmitScD(uint32_t rd, uint32_t rs2, uint32_t rs1, uint64_t expect_status) {
    emit_expect(EncAmo(F5_SC, 0, 0, F3_D, rd, rs1, rs2), rd, expect_status);
  }
  void EmitExit(uint32_t code) {
    EmitI(0, 28, 0, 0x100);                 // addi x28, x0, 0x100
    EmitI(1, 28, 28, 4);                    // slli x28, x28, 4  -> 0x1000
    emit(EncR(OP_ALU, 0, 28, 5, 28, 0));    // add  x28, x5, x28
    EmitI(0, 29, 0, static_cast<int32_t>(code));
    EmitStoreD(28, 29, 0);                  // sd x29, 0(x28): TOHOST = code
    // The end of the program is a WFI, not a self-jump. A self-jump is a
    // *redirect*, and the redirect is issued when the jump executes -- which,
    // out of order, can be before the store that precedes it has retired, and a
    // redirect flushes the whole store queue. WFI is a system macro the core
    // stages and takes only at the ROB head, so the exit store has retired and
    // drained before the machine halts, and the halt is what the harness waits
    // for.
    emit(0x10500073u);                      // wfi
  }

 private:
  ProgImage* img_;
  std::map<uint64_t, Expect>* expect_;
  uint64_t pc_;
};

// The prologue shared by both programs: x5 = the RAM base, x6 = the trap
// handler, mtvec = x6.
void EmitPrologue(Asm* a) {
  a->EmitI(0, 5, 0, 1);                 // addi x5, x0, 1
  a->EmitI(1, 5, 5, 31);                // slli x5, x5, 31 -> x5 = 0x80000000
  a->EmitI(0, 6, 5, static_cast<int32_t>(kHandlerOff));
  a->emit(EncCsr(0b001, 0, 6, CSR_MTVEC));  // csrw mtvec, x6
}

// The trap handler at base + kHandlerOff: skip the trapping instruction and
// return. It uses x31 so it cannot disturb the program's own registers.
void EmitHandler(ProgImage* img) {
  uint64_t pc = kRamBase + kHandlerOff;
  img->Put(pc, EncCsr(0b010, 31, 0, CSR_MEPC));        // csrr x31, mepc
  img->Put(pc + 4, EncI(OP_IMM, 0, 31, 31, 4));        // addi x31, x31, 4
  img->Put(pc + 8, EncCsr(0b001, 0, 31, CSR_MEPC));    // csrw mepc, x31
  img->Put(pc + 12, 0x30200073u);                      // mret
}

struct Program {
  ProgImage img;
  std::map<uint64_t, Expect> retires;   // PC -> (rd, value), from the ISA
  std::vector<uint64_t> watch;          // addresses to read back after the run
  uint64_t lr_count = 0;
  uint64_t sc_count = 0;
  uint64_t sc_success = 0;
  uint64_t sc_fail = 0;
  uint64_t ext_inval_expected = 0;
};

// ============================================================================
// Run A: the reservation semantics
// ============================================================================
Program BuildSemantics() {
  Program p;
  Asm a(&p.img, &p.retires, kRamBase);
  EmitPrologue(&a);

  a.EmitI(0, 10, 5, static_cast<int32_t>(kAOff));   // addi x10, x5, A

  // ---- 1. an SC with no reservation writes nothing ------------------------
  a.EmitI(0, 3, 0, static_cast<int32_t>(kSeedVal));
  a.EmitStoreD(10, 3, 0);            // sd x3, 0(x10): A = 0x11
  a.EmitScD(2, 3, 10, 1);            // sc.d x2, x3, (x10) -> x2 = 1, no write

  // ---- 2. LR returns the old value and reserves; SC succeeds exactly once --
  a.EmitI(0, 3, 0, static_cast<int32_t>(kVal2));
  a.EmitLrD(1, 10, kSeedVal);        // lr.d x1, (x10) -> x1 = 0x11
  a.EmitScD(2, 3, 10, 0);            // sc.d x2, x3, (x10) -> x2 = 0, A = 0x22

  // ---- 3. a second SC pairs with nothing ----------------------------------
  a.EmitScD(2, 3, 10, 1);            // sc.d x2, x3, (x10) -> x2 = 1, no write

  // ---- 4. a store inside the granule breaks the reservation ---------------
  a.EmitLrD(1, 10, kVal2);           // lr.d x1, (x10) -> x1 = 0x22
  a.EmitI(0, 11, 5, static_cast<int32_t>(kA8Off));
  a.EmitI(0, 12, 0, static_cast<int32_t>(kInVal));
  a.EmitStoreD(11, 12, 0);           // sd x12, 0(x11): A+8 = 0x33, in granule
  a.EmitScD(2, 3, 10, 1);            // sc.d x2, x3, (x10) -> x2 = 1, no write

  // ---- 5. a store outside the granule leaves it standing ------------------
  a.EmitI(0, 3, 0, static_cast<int32_t>(kVal5));
  a.EmitLrD(1, 10, kVal2);           // lr.d x1, (x10) -> x1 = 0x22
  a.EmitI(0, 13, 5, static_cast<int32_t>(kA64Off));
  a.EmitI(0, 14, 0, static_cast<int32_t>(kOutVal));
  a.EmitStoreD(13, 14, 0);           // sd x14, 0(x13): A+64 = 0x44, next granule
  a.EmitScD(2, 3, 10, 0);            // sc.d x2, x3, (x10) -> x2 = 0, A = 0x55

  // ---- 6. an AMO breaks the reservation -----------------------------------
  a.EmitLrD(1, 10, kVal5);           // lr.d x1, (x10) -> x1 = 0x55
  a.EmitI(0, 15, 0, static_cast<int32_t>(kAmoDelta));
  a.emit(EncAmo(F5_AMOADD, 0, 0, F3_D, 16, 10, 15));  // amoadd.d x16, x15, (x10)
  a.EmitScD(2, 3, 10, 1);            // sc.d x2, x3, (x10) -> x2 = 1, no write

  // ---- 7. an exception between the LR and the SC breaks it ----------------
  a.EmitLrD(1, 10, kVal5 + kAmoDelta);  // lr.d x1, (x10) -> x1 = 0x56
  a.emit(0x00000073u);               // ecall -> trap -> handler -> mret
  a.EmitScD(2, 3, 10, 1);            // sc.d x2, x3, (x10) -> x2 = 1, no write

  a.EmitExit(1);

  // Derived from the program above, by hand, from the ISA: the LRs are blocks
  // 2, 4, 5, 6 and 7; the SCs are blocks 1, 2, 3, 4, 5, 6 and 7, of which 2 and 5
  // must succeed (their reservations were never broken) and 1, 3, 4, 6 and 7
  // must fail.
  p.lr_count = 5;
  p.sc_count = 7;
  p.sc_success = 2;
  p.sc_fail = 5;
  p.ext_inval_expected = 0;
  p.watch = {kA, kA8, kA64};

  EmitHandler(&p.img);
  return p;
}

// ============================================================================
// Run B: the constrained LR/SC loop
// ============================================================================
Program BuildProgress(uint64_t injections) {
  Program p;
  Asm a(&p.img, &p.retires, kRamBase);
  EmitPrologue(&a);

  a.EmitI(0, 10, 5, static_cast<int32_t>(kAOff));
  a.EmitI(0, 20, 5, static_cast<int32_t>(kAttOff));
  a.EmitI(0, 3, 0, static_cast<int32_t>(kLoopVal));
  a.EmitI(0, 4, 0, 0);                                     // attempts = 0
  a.EmitI(0, 7, 0, static_cast<int32_t>(kProgressBound));  // the declared bound

  const uint64_t loop_pc = a.pc();
  a.EmitI(0, 4, 4, 1);                               // addi x4, x4, 1
  a.emit(EncB(OP_BRANCH, 0b100, 7, 4, 0));           // blt x7, x4, fail  (patched)
  const uint64_t fail_branch_pc = a.pc() - 4;
  // The LR's value here depends on the harness's own interference, so no fixed
  // expectation is recorded for it; the SC statuses are checked by count.
  a.emit(EncAmo(F5_LR, 0, 0, F3_D, 1, 10, 0));       // lr.d x1, (x10)
  // The constrained loop permits instructions between the LR and the SC that are
  // neither memory accesses nor control transfers; the padding widens the window
  // in which the second agent's write can be ordered between them.
  a.EmitI(0, 0, 0, 0);
  a.EmitI(0, 0, 0, 0);
  a.EmitI(0, 0, 0, 0);
  a.EmitI(0, 0, 0, 0);
  // The loop's SC succeeds only once the second agent has stopped, so no fixed
  // status is recorded for it; the statuses are checked by count.
  a.emit(EncAmo(F5_SC, 0, 0, F3_D, 2, 10, 3));       // sc.d x2, x3, (x10)
  a.emit(EncB(OP_BRANCH, 0b001, 2, 0, 0));           // bnez x2, loop     (patched)
  const uint64_t retry_branch_pc = a.pc() - 4;
  a.EmitStoreD(20, 4, 0);                            // sd x4, 0(x20): attempts
  a.EmitExit(1);                                     // success
  const uint64_t fail_pc = a.pc();
  a.EmitExit(2);                                     // attempts exceeded the bound

  p.img.Put(fail_branch_pc, EncB(OP_BRANCH, 0b100, 7, 4,
                                 static_cast<int32_t>(fail_pc - fail_branch_pc)));
  p.img.Put(retry_branch_pc, EncB(OP_BRANCH, 0b001, 2, 0,
                                  static_cast<int32_t>(loop_pc - retry_branch_pc)));
  // The ISA's accounting for this run, in terms of the stimulus: every injected
  // conflicting write breaks exactly one reservation, so the loop needs one
  // attempt per injection plus the attempt that succeeds.
  p.lr_count = injections + 1;
  p.sc_count = injections + 1;
  p.sc_success = 1;
  p.sc_fail = injections;
  p.ext_inval_expected = injections;
  p.watch = {kA, kAtt};

  EmitHandler(&p.img);
  return p;
}

// ============================================================================
// Geometry and retire lanes
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint64_t reset_vector = 0;
  uint32_t seq_w = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  g.seq_w = dut->o_geom_seq_w_o;
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
// One data-port beat, as the memory system saw it
// ============================================================================
struct TxnRec {
  uint64_t cycle = 0;
  bool we = false;
  uint32_t kind = 0;          // 0 ordinary, 1 AMO, 2 LR, 3 SC
  uint64_t addr = 0;
  uint32_t size = 0;
  uint32_t wstrb = 0;
  uint64_t wdata = 0;
  uint64_t rdata = 0;
  bool interloper = false;    // an injected second-agent store
};

constexpr uint32_t KIND_AMO = 1, KIND_LR = 2, KIND_SC = 3;

// ============================================================================
// The shadow the data-port history is replayed against
// ============================================================================
class Shadow {
 public:
  void Set(uint64_t addr, uint64_t value) { win_[addr & ~UINT64_C(7)] = value; }
  uint64_t Get(uint64_t addr) const {
    auto it = win_.find(addr & ~UINT64_C(7));
    return it == win_.end() ? 0 : it->second;
  }
  void Store(uint64_t addr, uint32_t wstrb, uint64_t wdata) {
    const uint64_t base = addr & ~UINT64_C(7);
    uint64_t win = Get(base);
    for (unsigned i = 0; i < 8; i++) {
      if (((wstrb >> i) & 1u) != 0u) {
        win = (win & ~(UINT64_C(0xff) << (8 * i))) |
              (((wdata >> (8 * i)) & UINT64_C(0xff)) << (8 * i));
      }
    }
    win_[base] = win;
  }

 private:
  std::map<uint64_t, uint64_t> win_;
};

// ============================================================================
// The run
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, const ProgImage* img,
          mosaic::MemoryModel* mem, uint64_t max_cycles)
      : dut_(dut), reporter_(reporter), imem_(img), dmem_(mem),
        max_cycles_(max_cycles) {}

  void Configure(const Geometry& g) { g_ = g; }

  struct Retire {
    uint64_t pc = 0;
    uint32_t rd = 0;
    bool reg_we = false;
    uint64_t value = 0;
    bool trap = false;
  };

  const std::vector<Retire>& retires() const { return retires_; }
  const std::vector<TxnRec>& txns() const { return txns_; }
  uint64_t cycles() const { return cycles_; }
  uint64_t granule_samples() const { return granule_samples_; }
  uint64_t granule_violations() const { return granule_violations_; }
  uint64_t injected() const { return injected_; }
  bool saw_stop() const { return saw_stop_; }


  // Arm the second agent: up to `budget` ordinary 8-byte writes to `addr`,
  // performed only while the hart holds a reservation and the port is free.
  void EnableSecondAgent(uint64_t addr, uint64_t budget) {
    agent_ = true;
    agent_addr_ = addr;
    agent_budget_ = budget;
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  // The exit protocol's store has retired by the time the spin retires, but a
  // retired store still has to *drain*: the store queue writes memory after the
  // authorisation watermark, so the harness waits for the queue to empty before
  // it reads the final image. (The spin's redirect spares an authorised store,
  // so the wait terminates.)
  void DrainStores(int max_cycles) {
    for (int i = 0; i < max_cycles && dut_->o_mem_sq_occupied_o != 0 &&
                    !dmem_.model()->finished(); i++) {
      Cycle(false);
    }
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail("run", "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + State());
    }
    dut_->rst = rst ? 1 : 0;
    dut_->imem_req_ready_i = 1;
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = 0;
    // The coherence notification is a one-cycle strobe; it is cleared here and
    // raised again below only when the second agent acts.
    dut_->ext_write_valid_i = 0;
    dut_->ext_write_addr_i = 0;
    dut_->ext_write_bytes_i = 0;

    if (imem_.HasResponse()) {
      const Imem::Request& r = imem_.Response();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = static_cast<uint32_t>(imem_.ResponseWord());
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

    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;
    dut_->eval();

    if (!rst) Observe();
    if (!rst && (dut_->o_wfi_halt_o != 0)) saw_stop_ = true;

    if (!rst) {
      if ((dut_->imem_req_valid_o != 0) && (dut_->imem_req_ready_i != 0)) {
        imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
      }
      if ((dut_->imem_rsp_valid_i != 0) && (dut_->imem_rsp_ready_o != 0)) {
        imem_.PopResponse();
      }
    }
    imem_.Advance();

    const bool core_taking = !rst && (dut_->dmem_req_valid_o != 0) &&
                             (dut_->dmem_req_ready_i != 0);
    if (core_taking) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(r, cycles_);

      TxnRec t;
      t.cycle = cycles_;
      t.we = r.we;
      // The class the endpoint published beside the transaction. It is sampled
      // in the cycle the request is accepted, where it names the transaction the
      // port is carrying -- the same convention the AMO case uses for its
      // `amo` bit.
      t.kind = dut_->o_mem_dmem_kind_o;
      t.addr = r.addr;
      t.size = r.size;
      t.wstrb = r.wstrb;
      t.wdata = r.wdata;
      txns_.push_back(t);
      pending_idx_ = static_cast<long>(txns_.size()) - 1;
    }
    if (!rst && (dut_->dmem_rsp_valid_i != 0) && (dut_->dmem_rsp_ready_o != 0)) {
      if (pending_idx_ >= 0) {
        txns_[static_cast<size_t>(pending_idx_)].rdata = dut_->dmem_rsp_rdata_i;
        pending_idx_ = -1;
      }
      dmem_.PopResponse();
    }
    dmem_.Advance();

    // ---------------------------------------------------------- second agent
    // A cooperative second master on the single-port memory: it acts exactly
    // when the hart holds a reservation and the port is free, which is the only
    // window in which a conflicting write can be ordered between the LR and the
    // SC without racing the port. The write is performed in the shared memory
    // model *and* notified to the DUT: the model alone would leave the hart's
    // reservation standing, which is the failure this run exists to catch.
    if (agent_ && !rst && (agent_budget_ > 0) &&
        (dut_->o_mem_res_valid_o != 0) && (dut_->dmem_req_valid_o == 0) &&
        (dut_->o_mem_lsu_busy_o == 0)) {
      agent_budget_--;
      injected_++;
      agent_value_ += UINT64_C(0x0100000000000001);
      if (dmem_.model()->Write(agent_addr_, 8, agent_value_) !=
          mosaic::AccessStatus::kOk) {
        Fail("second agent", "the injected store's address is not writable RAM");
      }
      dut_->ext_write_valid_i = 1;
      dut_->ext_write_addr_i = agent_addr_;
      dut_->ext_write_bytes_i = 8;
      TxnRec t;
      t.cycle = cycles_;
      t.interloper = true;
      t.we = true;
      t.addr = agent_addr_;
      t.size = 8;
      t.wstrb = 0xFF;
      t.wdata = agent_value_;
      t.rdata = agent_value_;
      txns_.push_back(t);
    }

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
           " retired=" + Dec(dut_->o_commit_o) + " stopped=" + Dec(dut_->o_stopped_o) +
           " res_valid=" + Dec(dut_->o_mem_res_valid_o) +
           " lr=" + Dec(dut_->o_mem_lr_ctr_o) + " sc_ok=" + Dec(dut_->o_mem_sc_ok_ctr_o) +
           " sc_fail=" + Dec(dut_->o_mem_sc_fail_ctr_o);
  }

 private:
  struct Imem {
    struct Request {
      uint64_t addr = 0;
      uint32_t id = 0;
      uint32_t epoch = 0;
    };
    explicit Imem(const ProgImage* img) : img_(img) {}
    bool HasResponse() const { return !ready_.empty(); }
    const Request& Response() const { return ready_.front(); }
    uint64_t ResponseWord() const { return img_->Word(ready_.front().addr); }
    void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
      inflight_.push_back(Entry{Request{addr, id, epoch}, 1});
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
    struct Entry {
      Request req;
      int left = 0;
    };
    const ProgImage* img_;
    std::deque<Entry> inflight_;
    std::deque<Request> ready_;
  };

  void Observe() {
    const uint32_t mask =
        (g_.retire_width >= 32) ? 0xFFFFFFFFu : ((1u << g_.retire_width) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.trap = PackedLane(dut_->ev_trap_o, lane, 1) != 0;
      retires_.push_back(r);
    }
    // The declared granule is checked as state: whenever a reservation stands,
    // the granule the DUT reports must be the 64-byte block containing the
    // address the LR reserved.
    if (dut_->o_mem_res_valid_o != 0) {
      granule_samples_++;
      if (dut_->o_mem_res_granule_o != (kA & ~UINT64_C(63))) granule_violations_++;
    }
  }

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  Imem imem_;
  DataMem dmem_;
  uint64_t max_cycles_;
  Geometry g_;
  uint64_t cycles_ = 0;
  long pending_idx_ = -1;
  std::vector<Retire> retires_;
  std::vector<TxnRec> txns_;
  uint64_t granule_samples_ = 0;
  uint64_t granule_violations_ = 0;
  bool agent_ = false;
  uint64_t agent_addr_ = 0;
  uint64_t agent_budget_ = 0;
  uint64_t agent_value_ = 0;
  uint64_t injected_ = 0;
  bool saw_stop_ = false;
};

// ============================================================================
// The replay: a legal history of the data port
// ============================================================================
struct ReplayResult {
  int amo_pairs = 0;
  int lr_reads = 0;
  int sc_writes = 0;
  int ordinary_writes = 0;
};

ReplayResult Replay(const std::vector<TxnRec>& txns, Shadow* shadow,
                    mosaic::Reporter* reporter, const std::string& label) {
  ReplayResult out;
  bool have_pending = false;
  uint64_t pending_addr = 0;
  uint64_t pending_old = 0;
  uint32_t pending_size = 0;

  for (const TxnRec& t : txns) {
    if (t.interloper) {
      // The second agent's write is part of the same history; it is folded in so
      // the ordering of the hart's accesses around it is checked.
      shadow->Set(t.addr, t.wdata);
      continue;
    }
    if (t.kind == KIND_AMO) {
      if (!t.we) {
        const uint64_t win = shadow->Get(t.addr);
        reporter->Check(win == t.rdata,
                        "the atomic read returns the value the history holds [" + label +
                            "] (cyc " + Dec(t.cycle) + " at " + U64(t.addr) + ")");
        have_pending = true;
        pending_addr = t.addr;
        pending_old = win;
        pending_size = t.size;
      } else {
        reporter->Check(have_pending && t.addr == pending_addr && t.size == pending_size,
                        "the atomic write beat follows its read beat at the same address "
                        "and width");
        const uint64_t win = shadow->Get(t.addr);
        reporter->Check(win == pending_old,
                        "the atomic write finds the location unchanged since its read");
        shadow->Store(t.addr, t.wstrb, t.wdata);
        have_pending = false;
        out.amo_pairs++;
      }
      continue;
    }
    if (t.kind == KIND_LR) {
      // An LR is a read and nothing else at the port: it must not write, and the
      // value it returns must be the value the history holds at that point.
      reporter->Check(!t.we, "an LR presents a read, never a write [" + label + "]");
      const uint64_t win = shadow->Get(t.addr);
      reporter->Check(win == t.rdata,
                      "the LR returns the value the history holds [" + label + "] (cyc " +
                          Dec(t.cycle) + " at " + U64(t.addr) + " shadow " + U64(win) +
                          " got " + U64(t.rdata) + ")");
      out.lr_reads++;
      continue;
    }
    if (t.kind == KIND_SC) {
      reporter->Check(t.we, "an SC presents a write when it reaches the port [" + label + "]");
      shadow->Store(t.addr, t.wstrb, t.wdata);
      out.sc_writes++;
      continue;
    }
    if (t.we) {
      shadow->Store(t.addr, t.wstrb, t.wdata);
      out.ordinary_writes++;
      continue;
    }
    const uint64_t lwin = shadow->Get(t.addr);
    reporter->Check(lwin == t.rdata,
                    "an ordinary load returns the value the history holds [" + label +
                        "] (cyc " + Dec(t.cycle) + " at " + U64(t.addr) + ")");
  }
  return out;
}

// ============================================================================
// One whole run
// ============================================================================
struct RunResult {
  uint64_t retires = 0;
  uint64_t cycles = 0;
  uint64_t txns = 0;
  ReplayResult replay;
  uint64_t granule_samples = 0;
  uint64_t granule_violations = 0;
  uint64_t injected = 0;
  std::vector<uint64_t> watched;
};

RunResult RunOnce(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                  const Geometry& geometry, const Program& prog,
                  uint64_t agent_budget, const std::string& label) {
  mosaic::MemoryModel dut_mem;

  Harness harness(dut, reporter, &prog.img, &dut_mem, kMaxRunCycles);
  harness.Configure(geometry);
  harness.Reset(kResetCycles);
  if (agent_budget > 0) harness.EnableSecondAgent(kA, agent_budget);

  // The frozen exit protocol: the program writes its code to TOHOST and then
  // halts with WFI; the halt is the end of the run.
  uint64_t last_retires = 0;
  uint64_t last_progress = 0;
  while (!harness.saw_stop()) {
    harness.Cycle(false);
    if (harness.retires().size() != last_retires) {
      last_retires = harness.retires().size();
      last_progress = harness.cycles();
    } else if (harness.cycles() - last_progress > kStallCycles) {
      Fail(label, "stalled before the exit protocol: " + harness.State());
    }
  }

  const std::vector<Harness::Retire>& retires = harness.retires();

  // The exit protocol's store is authorised (retired) but not yet necessarily in
  // memory; wait for the store queue to drain before reading the image.
  harness.DrainStores(4096);
  reporter->Check(dut->o_mem_sq_occupied_o == 0,
                  label + ": every retired store drained to memory before the image is "
                  "read (queue holds " + Dec(dut->o_mem_sq_occupied_o) + ")");

  // --------------------------------------------- 1. the retirement stream
  // Each LR/SC's architectural result, at its own PC, against the ISA's value.
  for (const auto& entry : prog.retires) {
    const uint64_t pc = entry.first;
    const Expect& e = entry.second;
    bool found = false;
    for (const Harness::Retire& r : retires) {
      if (r.pc != pc || r.trap) continue;
      found = true;
      if (r.rd != e.rd || !r.reg_we || r.value != e.value) {
        Fail(label, "the instruction at " + U64(pc) + " retired rd=" + Dec(r.rd) +
                        " we=" + Dec(r.reg_we) + " value=" + U64(r.value) +
                        ", the ISA requires rd=" + Dec(e.rd) + " value=" + U64(e.value));
      }
      break;
    }
    reporter->Check(found, label + ": the instruction at " + U64(pc) + " retires");
  }

  // ------------------------------------------------- 2. the port's own numbers
  // The order here is deliberate. The *stimulus* check comes first, because in
  // the progress run an external write that did not invalidate makes everything
  // after it meaningless; then the two facts the card's Pass criterion turns on,
  // which are statements about the memory traffic itself -- one write beat per
  // successful SC and one read beat per LR; then the endpoint's class counters,
  // which are an independent account of the same two facts; and last the
  // retirement stream's own count of the store-conditionals.
  const uint64_t dut_lr = dut->o_mem_lr_ctr_o;
  const uint64_t dut_sc_ok = dut->o_mem_sc_ok_ctr_o;
  const uint64_t dut_sc_fail = dut->o_mem_sc_fail_ctr_o;
  const uint64_t dut_ext_inval = dut->o_mem_res_ext_inval_ctr_o;

  reporter->Check(dut_ext_inval == prog.ext_inval_expected,
                  label + ": " + Dec(prog.ext_inval_expected) + " external writes must "
                  "clear the reservation, the endpoint counted " + Dec(dut_ext_inval));

  Shadow shadow;
  ReplayResult replay = Replay(harness.txns(), &shadow, reporter, label);

  // The heart of the card: an SC that succeeds performs exactly one write, and an
  // SC that fails performs none. The architectural statuses and the port's write
  // count are two independent accounts of the same fact.
  reporter->Check(replay.sc_writes == static_cast<int>(prog.sc_success),
                  label + ": exactly one write beat per successful SC -- the ISA says " +
                      Dec(prog.sc_success) + " succeed, the port saw " +
                      Dec(replay.sc_writes) + " SC write beats");
  reporter->Check(replay.lr_reads == static_cast<int>(prog.lr_count),
                  label + ": exactly one read beat per LR -- the ISA says " +
                      Dec(prog.lr_count) + ", the port saw " + Dec(replay.lr_reads));

  reporter->Check(dut_lr == prog.lr_count,
                  label + ": the hart executed " + Dec(prog.lr_count) + " LRs, the "
                  "endpoint counted " + Dec(dut_lr));
  reporter->Check(dut_sc_ok == prog.sc_success,
                  label + ": " + Dec(prog.sc_success) + " SCs must succeed, the endpoint "
                  "counted " + Dec(dut_sc_ok));
  reporter->Check(dut_sc_fail == prog.sc_fail,
                  label + ": " + Dec(prog.sc_fail) + " SCs must fail, the endpoint counted " +
                  Dec(dut_sc_fail));

  // x2 is written only by an SC in both programs, so the retirements of x2 are
  // the store-conditionals, and their values are the statuses the ISA defines.
  uint64_t sc_retires = 0, sc_zero = 0, sc_bad = 0;
  for (const Harness::Retire& r : retires) {
    if (r.trap || !r.reg_we || r.rd != kScRd) continue;
    sc_retires++;
    if (r.value == 0) sc_zero++;
    else if (r.value != 1) sc_bad++;
  }
  reporter->Check(sc_bad == 0,
                  label + ": every SC retired a status of 0 or 1 (" + Dec(sc_bad) +
                      " of " + Dec(sc_retires) + " outside the encoding)");
  reporter->Check(sc_retires == prog.sc_count,
                  label + ": the program executes " + Dec(prog.sc_count) +
                      " store-conditionals, the retire stream shows " + Dec(sc_retires));
  reporter->Check(sc_zero == prog.sc_success,
                  label + ": " + Dec(prog.sc_success) + " store-conditionals must report "
                  "success, the retire stream shows " + Dec(sc_zero));

  // ------------------------------------------------------- 3. the granule rule
  reporter->Check(harness.granule_samples() > 0,
                  label + ": the reservation was observed standing at least once");
  reporter->Check(harness.granule_violations() == 0,
                  label + ": every observed reservation covered the declared 64-byte "
                  "granule (" + Dec(harness.granule_violations()) + " violations in " +
                      Dec(harness.granule_samples()) + " samples)");

  // ------------------------------------------------------- 4. the memory image
  // The exit protocol is read through the memory model's own state, not by
  // reading TOHOST: a TOHOST *read* answers "still running" by construction.
  reporter->Check(dut_mem.finished() && dut_mem.passed() && dut_mem.exit_code() == 1,
                  label + ": the program reported success through the exit protocol "
                  "(finished=" + Dec(dut_mem.finished() ? 1 : 0) + " passed=" +
                      Dec(dut_mem.passed() ? 1 : 0) + " code=" + Dec(dut_mem.exit_code()) +
                      ")");

  RunResult out;
  out.retires = retires.size();
  out.cycles = harness.cycles();
  out.txns = harness.txns().size();
  out.replay = replay;
  out.granule_samples = harness.granule_samples();
  out.granule_violations = harness.granule_violations();
  out.injected = harness.injected();
  for (uint64_t addr : prog.watch) {
    uint64_t value = 0;
    if (dut_mem.Read(addr, 8, &value) != mosaic::AccessStatus::kOk) {
      Fail(label, "the watched address " + U64(addr) + " is not readable RAM");
    }
    out.watched.push_back(value);
  }
  return out;
}

// ============================================================================
// The two runs
// ============================================================================
void RunSemantics(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                  const Geometry& geometry) {
  const Program prog = BuildSemantics();
  const RunResult r = RunOnce(dut, reporter, geometry, prog, 0, "semantics");

  // The image the ISA requires, address by address:
  //   A    -- block 2's SC wrote 0x22, block 5's SC wrote 0x55, then the AMO
  //           between block 6's LR and SC added 1, and block 6's and 7's SCs
  //           failed, so the location ends at 0x56;
  //   A+8  -- block 4's in-granule store wrote 0x33 and no SC ever touched it;
  //   A+64 -- block 5's out-of-granule store wrote 0x44.
  const uint64_t expect_a = kVal5 + kAmoDelta;
  reporter->Check(r.watched[0] == expect_a,
                  "semantics: the reserved location holds " + U64(expect_a) + " (saw " +
                      U64(r.watched[0]) + ")");
  reporter->Check(r.watched[1] == kInVal,
                  "semantics: the in-granule conflicting location holds " + U64(kInVal) +
                      " (saw " + U64(r.watched[1]) + ")");
  reporter->Check(r.watched[2] == kOutVal,
                  "semantics: the out-of-granule location holds " + U64(kOutVal) + " (saw " +
                      U64(r.watched[2]) + ")");

  reporter->Check(dut->o_mem_res_valid_o == 0,
                  "semantics: the last SC consumed the reservation");
  reporter->Check(r.replay.amo_pairs == 1,
                  "semantics: the AMO between the LR and the SC appeared as one atomic "
                  "pair (saw " + Dec(r.replay.amo_pairs) + ")");

  std::printf("lrsc run A: %llu retires, %llu txns, %llu cycles, lr=%llu sc_ok=%llu "
              "sc_fail=%llu sc_beats=%d lr_beats=%d granule_samples=%llu\n",
              static_cast<unsigned long long>(r.retires),
              static_cast<unsigned long long>(r.txns),
              static_cast<unsigned long long>(r.cycles),
              static_cast<unsigned long long>(dut->o_mem_lr_ctr_o),
              static_cast<unsigned long long>(dut->o_mem_sc_ok_ctr_o),
              static_cast<unsigned long long>(dut->o_mem_sc_fail_ctr_o),
              r.replay.sc_writes, r.replay.lr_reads,
              static_cast<unsigned long long>(r.granule_samples));
}

void RunProgress(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                 const Geometry& geometry, uint64_t injections) {
  // The declared assumption, checked before anything else: the stimulus must not
  // itself violate the progress condition. If it injected N or more conflicting
  // writes, the loop would be entitled to fail and a "pass" would be meaningless.
  reporter->Check(injections < kProgressBound,
                  "progress: the stimulus injected " + Dec(injections) +
                      " conflicting writes, below the declared bound " +
                      Dec(kProgressBound));

  const Program prog = BuildProgress(injections);
  const RunResult r = RunOnce(dut, reporter, geometry, prog, injections, "progress");

  reporter->Check(r.injected == injections,
                  "progress: the second agent performed exactly " + Dec(injections) +
                      " writes (saw " + Dec(r.injected) + ")");

  // The run's own numbers decide the progress condition: the loop needed one
  // attempt per interfering write plus the one that succeeded, and never reached
  // the declared bound.
  const uint64_t attempts = r.watched[1];
  reporter->Check(attempts == injections + 1,
                  "progress: the constrained loop succeeded on attempt " + Dec(attempts) +
                      ", one per interfering write plus one (" + Dec(injections + 1) +
                      " expected)");
  reporter->Check(attempts <= kProgressBound,
                  "progress: the loop terminated inside the declared bound " +
                      Dec(kProgressBound) + " (took " + Dec(attempts) + " attempts)");
  reporter->Check(r.watched[0] == kLoopVal,
                  "progress: the loop's successful SC wrote " + U64(kLoopVal) + " (saw " +
                      U64(r.watched[0]) + ")");
  reporter->Check(dut->o_mem_res_valid_o == 0,
                  "progress: the successful SC consumed the reservation");

  std::printf("lrsc run B: %llu retires, %llu txns, %llu cycles, injected=%llu "
              "attempts=%llu lr=%llu sc_ok=%llu sc_fail=%llu sc_beats=%d\n",
              static_cast<unsigned long long>(r.retires),
              static_cast<unsigned long long>(r.txns),
              static_cast<unsigned long long>(r.cycles),
              static_cast<unsigned long long>(r.injected),
              static_cast<unsigned long long>(attempts),
              static_cast<unsigned long long>(dut->o_mem_lr_ctr_o),
              static_cast<unsigned long long>(dut->o_mem_sc_ok_ctr_o),
              static_cast<unsigned long long>(dut->o_mem_sc_fail_ctr_o),
              r.replay.sc_writes);
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
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    const Geometry geometry = ReadGeometry(&dut);
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    if (geometry.reset_vector != 0x80000000ull) {
      Fail("geometry", "the reset vector is not the link base");
    }

    RunSemantics(&dut, &reporter, geometry);
    RunProgress(&dut, &reporter, geometry, 3);

    detail = "checks=" + Dec(reporter.checks()) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the LR/SC reservation contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  const bool ok = passed && (reporter.failures() == 0);
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
