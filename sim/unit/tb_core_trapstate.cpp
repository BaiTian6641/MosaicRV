// ============================================================================
// tb_core_trapstate.cpp -- CASE=trap.precise_state, work package V-014.
//
// The DUT is the integrated p0 core with the memory path, the CSR file
// (I-019), the interrupt decision (I-020) and the trap controller wired in. It
// retires up to MOSAIC_RETIRE_WIDTH (2) instructions per cycle; a synchronous
// exception is *not* a retirement: the faulting instruction is dropped and an
// architectural event of its own is published in lane 0.
//
// ------------------------------------------------------------------ the card
//
// docs/validation-plan.md §5 V-014, "compare synchronous traps and precise
// state". Inputs: programs that inject illegal instructions, `ecall`, `ebreak`,
// alignment faults and access faults, plus an M-mode trap implementation.
// Action: place *each kind of trap* at the ROB head and away from it, before and
// after a long-latency instruction, and in same-cycle-completion situations;
// check the faulting PC, `mcause`, `mtval`, `mepc`, `mstatus` (MPIE/MPP save and
// restore), the handler PC and the `mret` restoration. Outputs: the pre/post
// state for each trap kind and evidence of what was squashed.
//
// Pass: the faulting instruction does not retire as a success; older effects are
// complete and younger ones invisible; every specified CSR and PC is exactly
// right.
//
// Fail (the four fail modes the controls inject): an exception's writeback
// polluting `rd` first; a wrong `mtval` being masked wholesale; the reference
// model taking one instruction too many; an instruction being lost on replay
// after the trap.
//
// ------------------------------------------------------------- what runs
//
// One hand-assembled M-mode program at the reset vector, 7 trap kinds x 5
// placements = 35 directed scenarios, then a register dump and the exit
// protocol. The trap handler is the machine's own: it services every trap the
// program raises, and it is what *observes* the architectural state -- the
// machine publishes no committed register file, so the only honest way to read
// precise state is to have the trap handler save it before it touches anything:
//
//   * `csrrw t0, mscratch, t0` swaps one register with a CSR the program set
//     before the scenario, so all 32 GPRs survive into the frame (the swapped
//     value is recovered from `mscratch` after x1..x4 and x6..x31 are stored);
//   * the frame then holds the whole register file, `mepc`, `mcause`, `mtval`,
//     `mstatus`, `mtvec`, `mie`, `mip` and the word at a fixed probe address
//     (which a *younger* store of the scenario targets, so the saved word proves
//     whether that store had become visible);
//   * `mepc += 4`, `mret` -- the p0 resumption discipline every instruction of
//     the program is 4 bytes long, so the faulting instruction is skipped and
//     the instructions behind it re-execute after the trap. That re-execution is
//     the "no instruction is lost on replay" path.
//
// Each scenario seeds: five older canary registers (x10..x14), two younger
// registers (x18, x19) and the faulting load's destination (x20) with known
// values; a probe word with a known pre-value; and the operands of a `div`. It
// then arranges the placement, raises the fault, and leaves younger work behind
// it (a younger `div` for "before a long-latency instruction"; register writes
// and a store to the probe for every scenario).
//
// ------------------------------------------------------- where the expectation comes from
//
// Nothing in the comparison is taken from the DUT.
//
//   * THE PER-INSTRUCTION STREAM is `mosaic_trap::RunReference`
//     (sim/unit/trap_ref.h), an independent RV64IM_Zicsr interpreter with its
//     own `mosaic::MemoryModel` -- a second instance the DUT can never touch.
//     It decodes the same words, models M-mode traps and `mret`, and produces
//     one ordered event per architectural action. Every retirement the machine
//     publishes is compared with it, pc by pc, destination by destination,
//     value by value; every trap entry is compared by cause, tval and epc.
//   * THE ARCHITECTURAL REGISTER FILE AT EACH TRAP is the *fold* of that
//     interpreter's retirements in program order: the register file at the
//     moment the faulting instruction is at the head -- after every older
//     instruction and before any younger one. That fold is what the handler's
//     frame must equal, field by field.
//   * THE TRAP CSRs follow the ISA text, not the DUT: `mepc` is the faulting
//     PC (its low two bits read-only zero), `mcause` is the kind's exception
//     code, `mtval` is the faulting address for a memory fault and zero for a
//     system trap (mosaic_core.sv's own rule, checked against the independent
//     value the interpreter carries), `mstatus` is MPIE <- MIE, MIE <- 0,
//     MPP <- M in an M-only profile, and the handler PC is the installed
//     vector (`mtvec` Direct). The `mret` restoration is the mirror rule.
//   * THE PLACEMENTS are asserted from what the run *produced*, not from what
//     the program intended: the exception-capture counter says whether the
//     faulting entry was at the head when its fault completed, the ROB
//     occupancy says how much younger work was behind it, the writeback
//     collision and commit counters say whether another instruction completed
//     in the same cycle, and the frame says whether an older long-latency
//     instruction had completed.
//
// ---------------------------------------------------------------- controls
//
// tools/run_trapstate_controls.py rebuilds this case with one `-D` per fail mode
// the card names. All four are `-CFLAGS` mutations of this file's own comparison
// (they are properties of the checker, not of one gate), and they are labelled
// DRIVER in the report rather than passed off as RTL mutants.
// ============================================================================

#include <verilated.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"
#include "trap_ref.h"

using mosaic_ref::DataMem;
using mosaic_trap::RefEvent;
using mosaic_trap::RefResult;

namespace {

constexpr int kResetCycles = 4;
constexpr uint64_t kProgramCycles = 400000;
constexpr uint64_t kStallCycles = 20000;

// ---------------------------------------------------------------------------
// The memory layout the program uses. All of it is RAM (0x80000000..0x80200000),
// so every store is an ordinary writable access and only the addresses the
// scenario *chooses* to fault on are outside the map.
// ---------------------------------------------------------------------------
constexpr uint64_t kProbe = 0x80004000ull;       // one word: the younger-store probe
constexpr uint64_t kDump = 0x80006000ull;        // the final register dump (32 * 8)
constexpr uint64_t kFrameBase = 0x80010000ull;   // one frame per scenario
constexpr uint64_t kFrameStride = 0x200ull;
constexpr uint64_t kUncovered = 0x90000000ull;   // outside the frozen map
// The long-latency pair: `div` is `width + 1` = 65 cycles in mosaic_muldiv, the
// longest instruction this core has, so "before/after a long-latency
// instruction" is a real window rather than a one-cycle accident.
constexpr uint64_t kDivA = 0x100000ull;
constexpr uint64_t kDivB = 7ull;
constexpr uint64_t kDivQuotient = kDivA / kDivB;   // 0x24924

// Frame fields (byte offsets), written by the trap handler.
constexpr int F_MEPC = 0;
constexpr int F_MCAUSE = 8;
constexpr int F_MTVAL = 16;
constexpr int F_MSTATUS = 24;
constexpr int F_MSCRATCH = 32;
constexpr int F_MTVEC = 40;
constexpr int F_MIE = 48;
constexpr int F_MIP = 56;
constexpr int F_PROBE = 64;
constexpr int F_GPR = 128;   // gpr[i] at F_GPR + 8*i

// The ISA's M-mode exception codes (mosaic_pkg.sv, from the Privileged Spec).
constexpr uint64_t kExcIllegal = 2;
constexpr uint64_t kExcBreakpoint = 3;
constexpr uint64_t kExcLoadMisaligned = 4;
constexpr uint64_t kExcLoadAccess = 5;
constexpr uint64_t kExcStoreMisaligned = 6;
constexpr uint64_t kExcStoreAccess = 7;
constexpr uint64_t kExcEcallM = 11;

// mstatus after a trap entry with MIE set: MPIE <- MIE(1), MIE <- 0, MPP <- M.
constexpr uint64_t kMstatusTrapEntry = 0x1880ull;
// mstatus after MRET: MPIE <- 1, MIE <- MPIE(1), MPP <- M.
constexpr uint64_t kMstatusAfterMret = 0x1888ull;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }
bool Debug() { return std::getenv("TRAPSTATE_DEBUG") != nullptr; }

// ============================================================================
// The instruction image
// ============================================================================
class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    return (it == words_.end()) ? 0x00000073u : it->second;
  }
  const std::map<uint64_t, uint32_t>& words() const { return words_; }

 private:
  std::map<uint64_t, uint32_t> words_;
};

// ============================================================================
// A very small assembler
// ============================================================================
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
  void Add(uint32_t rd, uint32_t rs1, uint32_t rs2) {
    Emit(R(0, rs2, rs1, 0, rd, 0x33));
  }
  void Mul(uint32_t rd, uint32_t rs1, uint32_t rs2) {
    Emit(R(0x01, rs2, rs1, 0, rd, 0x33));
  }
  void Div(uint32_t rd, uint32_t rs1, uint32_t rs2) {
    Emit(R(0x01, rs2, rs1, 4, rd, 0x33));
  }
  void Slli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(I(static_cast<int32_t>(sh), rs1, 1, rd, 0x13));
  }
  void Srli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(I(static_cast<int32_t>(sh), rs1, 5, rd, 0x13));
  }
  void Lui(uint32_t rd, uint32_t imm20) { Emit(U(imm20, rd, 0x37)); }
  void Ld(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 3, rd, 0x03)); }
  void Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { Emit(S(imm, rs2, rs1, 3, 0x23)); }
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 1, rd, 0x73));
  }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 2, rd, 0x73));
  }
  void Ecall() { Emit(0x00000073u); }
  void Ebreak() { Emit(0x00100073u); }
  void Mret() { Emit(0x30200073u); }
  void JalSelf() { Emit(0x0000006fu); }   // jal x0, 0

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
      Fail("assembler", "li of a value wider than 32 bits is not implemented");
    }
    // The p0 addresses are 32-bit values with bit 31 clear; the shifts
    // zero-extend the low 32 bits so the register holds the address, not its
    // sign extension.
    Slli(rd, rd, 32);
    Srli(rd, rd, 32);
  }

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
  uint64_t Label(const std::string& name) const {
    auto it = labels_.find(name);
    return (it == labels_.end()) ? 0 : it->second;
  }

  void Resolve() {
    for (const LaFixup& fix : fixups_) {
      auto it = labels_.find(fix.label);
      if (it == labels_.end()) continue;
      const int64_t here = static_cast<int64_t>(base_ + 4ull * fix.at);
      const int64_t delta = static_cast<int64_t>(it->second) - here;
      const uint32_t lo = static_cast<uint32_t>(delta) & 0xFFFu;
      const uint32_t hi = static_cast<uint32_t>((delta + 0x800) >> 12) & 0xFFFFFu;
      words_[fix.at] = U(hi, fix.rd, 0x17);
      words_[fix.at + 1] = I(static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0),
                             fix.rd, 0, fix.rd, 0x13);
    }
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
// The trap taxonomy and the placements
// ============================================================================
enum class Kind {
  kIllegal,
  kEcall,
  kEbreak,
  kLdMisalign,
  kStMisalign,
  kLdAccess,
  kStAccess,
};

enum class Place {
  kHead,
  kAway,
  kBeforeLong,
  kAfterLong,
  kSameCycle,
};

const char* KindName(Kind k) {
  switch (k) {
    case Kind::kIllegal: return "illegal";
    case Kind::kEcall: return "ecall";
    case Kind::kEbreak: return "ebreak";
    case Kind::kLdMisalign: return "ld-misalign";
    case Kind::kStMisalign: return "st-misalign";
    case Kind::kLdAccess: return "ld-access";
    case Kind::kStAccess: return "st-access";
  }
  return "?";
}

const char* PlaceName(Place p) {
  switch (p) {
    case Place::kHead: return "head";
    case Place::kAway: return "away";
    case Place::kBeforeLong: return "before-long";
    case Place::kAfterLong: return "after-long";
    case Place::kSameCycle: return "same-cycle";
  }
  return "?";
}

bool IsMemoryKind(Kind k) {
  return k == Kind::kLdMisalign || k == Kind::kStMisalign ||
         k == Kind::kLdAccess || k == Kind::kStAccess;
}
// Used by the RD_POLLUTED control, which is compiled out of the shipping build.
[[maybe_unused]] bool IsLoadKind(Kind k) {
  return k == Kind::kLdMisalign || k == Kind::kLdAccess;
}

uint64_t KindCause(Kind k) {
  switch (k) {
    case Kind::kIllegal: return kExcIllegal;
    case Kind::kEcall: return kExcEcallM;
    case Kind::kEbreak: return kExcBreakpoint;
    case Kind::kLdMisalign: return kExcLoadMisaligned;
    case Kind::kStMisalign: return kExcStoreMisaligned;
    case Kind::kLdAccess: return kExcLoadAccess;
    case Kind::kStAccess: return kExcStoreAccess;
  }
  return 0;
}

// The ISA's `mtval` rule, taken from the spec and from mosaic_core.sv's own
// statement: a memory fault reports the faulting address, a system trap reports
// zero.
uint64_t KindTval(Kind k) {
  switch (k) {
    case Kind::kLdMisalign:
    case Kind::kStMisalign:
      return 1;   // the misaligned address the program uses
    case Kind::kLdAccess:
    case Kind::kStAccess:
      return kUncovered;
    default:
      return 0;
  }
}

struct Scenario {
  Kind kind;
  Place place;
  int index;
  uint64_t frame;
  uint64_t fault_pc;
  uint64_t probe_pre;
  uint64_t probe_post;
};

// ============================================================================
// The program
// ============================================================================
struct Program {
  Asm asm_{MOSAIC_RESET_VECTOR};
  std::vector<Scenario> scenarios;
  uint64_t handler_pc = 0;
  uint64_t exit_store_pc = 0;
};

// The trap handler: save the architectural state *before* touching anything,
// then skip the faulting instruction and return. See the file header.
void EmitHandler(Asm* a) {
  // t0 (x5) <- the frame the program put in mscratch; mscratch <- the old x5.
  a->Csrrw(5, 0x340, 5);
  // Every register but the base and x0, saved unmodified.
  const uint32_t order[] = {1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                            16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                            28, 29, 30, 31};
  for (uint32_t r : order) a->Sd(r, 5, F_GPR + 8 * static_cast<int>(r));
  a->Sd(0, 5, F_GPR);                    // x0 is zero
  a->Csrrs(6, 0x340, 0);                 // t1 <- old x5 (x6 was saved above)
  a->Sd(6, 5, F_GPR + 8 * 5);
  a->Sd(5, 5, F_MSCRATCH);              // t0 still holds the pre-trap mscratch
  a->Csrrs(6, 0x341, 0); a->Sd(6, 5, F_MEPC);
  a->Csrrs(6, 0x342, 0); a->Sd(6, 5, F_MCAUSE);
  a->Csrrs(6, 0x343, 0); a->Sd(6, 5, F_MTVAL);
  a->Csrrs(6, 0x300, 0); a->Sd(6, 5, F_MSTATUS);
  a->Csrrs(6, 0x305, 0); a->Sd(6, 5, F_MTVEC);
  a->Csrrs(6, 0x304, 0); a->Sd(6, 5, F_MIE);
  a->Csrrs(6, 0x344, 0); a->Sd(6, 5, F_MIP);
  // The probe word a younger store of this scenario targets: whatever is here
  // now is what was architecturally visible at the trap.
  a->LiAbs(6, kProbe);
  a->Ld(7, 6, 0);
  a->Sd(7, 5, F_PROBE);
  // Resume after the faulting instruction (every p0 instruction is 4 bytes).
  a->Csrrs(6, 0x341, 0);
  a->Addi(6, 6, 4);
  a->Csrrw(0, 0x341, 6);
  a->Mret();
}

// One scenario. Everything the driver needs to know about it is recorded in
// `sc`; the emission order is the dynamic order.
void EmitScenario(Asm* a, Kind kind, Place place, int index,
                  std::vector<Scenario>* out, const char* handler_label) {
  Scenario sc;
  sc.kind = kind;
  sc.place = place;
  sc.index = index;
  sc.frame = kFrameBase + kFrameStride * static_cast<uint64_t>(index);
  sc.probe_pre = 0x5000ull + static_cast<uint64_t>(index);
  sc.probe_post = 0x6000ull + static_cast<uint64_t>(index);

  // ---- setup (older than the fault) ----
  a->LiAbs(5, sc.frame);
  a->Csrrw(0, 0x340, 5);                       // mscratch = this scenario's frame
  for (int j = 0; j < 5; j++) {
    a->LiAbs(10 + j, 0x1000ull + 0x10ull * index + j);
  }
  a->LiAbs(18, 0x2000ull + index);             // younger registers, old values
  a->LiAbs(19, 0x2001ull + index);
  a->LiAbs(20, 0x4000ull + index);             // the faulting load's destination
  a->LiAbs(6, kProbe);
  a->LiAbs(7, sc.probe_pre);
  a->Sd(7, 6, 0);                              // probe := pre
  a->LiAbs(8, kDivA);
  a->LiAbs(9, kDivB);

  // ---- placement-specific older work ----
  switch (place) {
    case Place::kAway:
      // A dependent chain of 24 older instructions: the faulting instruction
      // sits behind them in the reorder buffer and is resolved (captured) before
      // they have all retired.
      for (int k = 0; k < 24; k++) a->Addi(25, 25, 1);
      break;
    case Place::kSameCycle:
      // Exactly one older independent instruction immediately ahead of the
      // fault, so an older retirement can share the cycle the fault completes in.
      a->Addi(25, 25, 1);
      break;
    case Place::kAfterLong:
      a->Div(22, 8, 9);                        // older long-latency instruction
      break;
    case Place::kHead:
    case Place::kBeforeLong:
      break;
  }
  if (kind == Kind::kLdAccess || kind == Kind::kStAccess) {
    a->LiAbs(27, kUncovered);
  }

  // ---- the fault ----
  sc.fault_pc = a->pc();
  switch (kind) {
    case Kind::kIllegal:
      // A read of a CSR this profile does not implement: the decoder accepts the
      // Zicsr form, the CSR file reports it unimplemented, and the system unit
      // raises illegal-instruction.
      a->Csrrs(6, 0x7b0, 0);
      break;
    case Kind::kEcall: a->Ecall(); break;
    case Kind::kEbreak: a->Ebreak(); break;
    case Kind::kLdMisalign: a->Ld(20, 0, 1); break;
    case Kind::kStMisalign: a->Sd(20, 0, 1); break;
    case Kind::kLdAccess: a->Ld(20, 27, 0); break;
    case Kind::kStAccess: a->Sd(20, 27, 0); break;
  }

  // ---- younger work ----
  if (place == Place::kBeforeLong) {
    a->Div(24, 8, 9);                          // younger long-latency instruction
  }
  a->LiAbs(18, 0x3000ull + index);             // younger register writes
  a->LiAbs(19, 0x3001ull + index);
  a->LiAbs(6, kProbe);
  a->LiAbs(7, sc.probe_post);
  a->Sd(7, 6, 0);                              // younger store to the probe

  (void)handler_label;
  out->push_back(sc);
}

Program BuildProgram() {
  Program p;
  Asm& a = p.asm_;
  a.La(5, "handler");
  a.Csrrw(0, 0x305, 5);          // mtvec = handler (Direct)
  // mstatus.MIE is *set* for the whole run (no interrupt source is ever
  // asserted), so the trap entry's MPIE <- MIE saves a one and the MRET
  // restores it: the save/restore is exercised with a value that differs on the
  // two sides rather than with the trivial zero-to-zero.
  a.Addi(7, 0, 8);
  a.Csrrw(0, 0x300, 7);          // mstatus = 8 (MIE), MPP forced to M
  a.Csrrw(0, 0x304, 0);          // mie = 0
  // The front end fetches ahead of retirement, and a system trap is refused
  // until the vector is installed -- until the `csrw mtvec` has retired. Sixteen
  // instructions exceed the front end's window, exactly as CASE=core.trap_csr
  // _program arranges.
  for (int i = 0; i < 16; i++) a.Addi(0, 0, 0);

  const Kind kinds[] = {Kind::kIllegal, Kind::kEcall, Kind::kEbreak,
                        Kind::kLdMisalign, Kind::kStMisalign,
                        Kind::kLdAccess, Kind::kStAccess};
  const Place places[] = {Place::kHead, Place::kAway, Place::kBeforeLong,
                          Place::kAfterLong, Place::kSameCycle};
  int index = 0;
  for (Kind k : kinds) {
    for (Place pl : places) {
      EmitScenario(&a, k, pl, index, &p.scenarios, "handler");
      index++;
    }
  }

  // The final register dump: x1..x31 into kDump, so the end state (including
  // everything the replayed instructions produced) is in memory and is compared
  // against the reference's own memory.
  a.LiAbs(5, kDump);
  for (uint32_t r = 1; r < 32; r++) {
    a.Sd(r, 5, 8 * static_cast<int>(r));
  }
  a.Sd(0, 5, 0);

  // The exit protocol.
  a.Addi(5, 0, 1);
  a.LiAbs(6, MOSAIC_TOHOST);
  p.exit_store_pc = a.pc();
  a.Sd(5, 6, 0);
  a.Mark("park");
  a.JalSelf();

  a.Mark("handler");
  EmitHandler(&a);
  p.handler_pc = a.Label("handler");
  a.Resolve();
  return p;
}

// ============================================================================
// The instruction memory (a fixed-latency fetch, as in the sibling core cases)
// ============================================================================
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };
  explicit Imem(const ProgImage* img) : img_(img) {}
  void Reset() { inflight_.clear(); ready_.clear(); }
  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint32_t ResponseWord() const { return img_->Word(ready_.front().addr); }
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

 private:
  struct Entry {
    Request req;
    int left = 0;
  };
  const ProgImage* img_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
};

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
uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
  const uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
  return (packed >> (lane * width)) & mask;
}

// ============================================================================
// The architectural state the trap handler saved
// ============================================================================
struct Snapshot {
  uint64_t mepc = 0, mcause = 0, mtval = 0, mstatus = 0;
  uint64_t mscratch = 0, mtvec = 0, mie = 0, mip = 0, probe = 0;
  uint64_t gpr[32] = {};
};

bool ReadSnapshot(mosaic::MemoryModel* mem, uint64_t frame, Snapshot* out,
                  std::string* detail) {
  auto read = [&](uint64_t addr, uint64_t* value) {
    if (mem->Read(addr, 8, value) != mosaic::AccessStatus::kOk) {
      *detail = "the frame word at " + U64(addr) + " is not readable";
      return false;
    }
    return true;
  };
  if (!read(frame + F_MEPC, &out->mepc)) return false;
  if (!read(frame + F_MCAUSE, &out->mcause)) return false;
  if (!read(frame + F_MTVAL, &out->mtval)) return false;
  if (!read(frame + F_MSTATUS, &out->mstatus)) return false;
  if (!read(frame + F_MSCRATCH, &out->mscratch)) return false;
  if (!read(frame + F_MTVEC, &out->mtvec)) return false;
  if (!read(frame + F_MIE, &out->mie)) return false;
  if (!read(frame + F_MIP, &out->mip)) return false;
  if (!read(frame + F_PROBE, &out->probe)) return false;
  for (int i = 0; i < 32; i++) {
    if (!read(frame + F_GPR + 8ull * i, &out->gpr[i])) return false;
  }
  return true;
}

// ============================================================================
// The observed trap record and its placement evidence
// ============================================================================
struct TrapRecord {
  uint64_t cause = 0, tval = 0, epc = 0, target = 0;
  bool is_irq = false;
  uint64_t cycle = 0;
  uint64_t occupied = 0;
  bool capture_same_sample = false;    // the exception was captured in this cycle
  uint64_t capture_cycle = UINT64_MAX;
  uint64_t capture_commit_delta = 0;   // retirements in the capture cycle
  uint64_t capture_collision_delta = 0;// writeback collisions in the capture cycle
  uint64_t sys_commit_delta = 0;       // retirements in the system macro's cycle
};

// ============================================================================
// The harness
// ============================================================================
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
  uint64_t retires() const { return retires_; }
  const std::vector<TrapRecord>& traps() const { return traps_; }
  const std::vector<TrapRecord>& fault_events() const { return fault_events_; }

  struct Retire {
    uint64_t pc = 0;
    uint32_t rd = 0;
    bool reg_we = false;
    uint64_t value = 0;
    bool is_trap = false;
    uint64_t cycle = 0;
  };
  const std::vector<Retire>& retire_events() const { return retires_list_; }

  void Expect(const RefResult* ref) {
    ref_ = ref;
    ref_retires_.clear();
    ref_traps_.clear();
    for (const RefEvent& e : ref->trace) {
      if (!e.trap) {
        ref_retires_.push_back(e);
        continue;
      }
#ifdef MOSAIC_TRAPSTATE_MUTANT_REF_EXTRA_INSN
      // NEGATIVE CONTROL (card fail mode 3): the reference takes one
      // instruction too many -- it treats the trap as an ordinary retirement,
      // so its stream expects the faulting instruction to retire where the
      // machine (correctly) publishes a trap and no retirement. The retirement
      // comparison then fails at the faulting PC.
      RefEvent r = e;
      r.trap = false;
      r.rd = 0;
      r.reg_we = false;
      r.value = 0;
      ref_retires_.push_back(r);
#else
      ref_traps_.push_back(e);
#endif
    }
  }
  void StopComparing() { ref_ = nullptr; }

  bool Complete() const {
    if (ref_ == nullptr) return true;
    return retires_ >= ref_retires_.size() && traps_.size() >= ref_traps_.size();
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
  }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " retired=" + Dec(dut_->o_commit_o) + " traps=" + Dec(traps_.size()) +
           " mcause=" + U64(dut_->o_csr_mcause_o) + " mepc=" + U64(dut_->o_csr_mepc_o);
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(phase_ + " at cycle " + Dec(cycles_),
           "max-cycles (" + Dec(max_cycles_) + ") exhausted: " + State());
    }
    mtime_++;
    dut_->rst = rst ? 1 : 0;
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
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

 private:
  void Observe() {
    Compare("the retire counter equals the retirement events published",
            dut_->o_commit_o == retires_,
            "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(retires_));
    Compare("ROB occupancy is within the ROB",
            dut_->o_rob_occupied_o <= g_.rob_entries,
            "occupied=" + Dec(dut_->o_rob_occupied_o));
    // The rename recovery counters that report a *wrong* restore: a squash to a
    // checkpoint that was not at a committed boundary, or a squash with no
    // checkpoint at all. Both are defects and stay zero.
    Compare("the rename recovery counters report no invalid squash",
            dut_->o_squash_nc_o == 0 && dut_->o_squash_under_o == 0,
            "squash_nc=" + Dec(dut_->o_squash_nc_o) +
                " under=" + Dec(dut_->o_squash_under_o));
    if (dut_->o_journal_ovf_o != 0) journal_overflow_seen_ = true;
    Compare("every exception payload was matched to its slot's generation",
            dut_->o_exc_gen_mismatch_o == 0,
            "mismatches=" + Dec(dut_->o_exc_gen_mismatch_o));

    // Per-cycle deltas: the exception-capture counter, the commit counter and
    // the writeback collision counter are registered, so an event in the cycle
    // just ended is visible in this sample.
    const uint64_t commit_delta = dut_->o_commit_o - last_commit_sample_;
    const uint64_t capture_delta = dut_->o_exc_capture_o - last_capture_sample_;
    const uint64_t collision_delta = dut_->o_wb_collision_o - last_collision_sample_;
    const uint64_t sys_delta = dut_->o_sys_exec_o - last_sys_sample_;
    last_commit_sample_ = dut_->o_commit_o;
    last_capture_sample_ = dut_->o_exc_capture_o;
    last_collision_sample_ = dut_->o_wb_collision_o;
    last_sys_sample_ = dut_->o_sys_exec_o;

    if (sys_delta != 0) {
      last_sys_cycle_ = cycles_;
      last_sys_commit_delta_ = commit_delta;
    }

    if (capture_delta != 0) {
      last_capture_cycle_ = cycles_;
      last_capture_commit_delta_ = commit_delta;
      last_capture_collision_delta_ = collision_delta;
    }
    trap_sample_ = false;

    if (dut_->o_trap_valid_o != 0) {
      TrapRecord t;
      t.cause = dut_->o_trap_cause_o;
      t.tval = dut_->o_trap_tval_o;
      t.epc = dut_->o_trap_epc_o;
      t.target = dut_->o_trap_target_o;
      t.is_irq = dut_->o_trap_is_irq_o != 0;
      t.cycle = cycles_;
      t.occupied = dut_->o_rob_occupied_o;
      t.capture_same_sample = (capture_delta != 0);
      t.capture_cycle = last_capture_cycle_;
      t.capture_commit_delta = last_capture_commit_delta_;
      t.capture_collision_delta = last_capture_collision_delta_;
      t.sys_commit_delta =
          (last_sys_cycle_ == cycles_) ? last_sys_commit_delta_ : 0;
      traps_.push_back(t);
      if (Debug()) {
        std::printf("    [trap] cycle=%llu cause=%s epc=%s tval=%s target=%s "
                    "occupied=%s cap_same=%d cap_cycle=%llu cap_gap=%llu "
                    "cap_commit=%llu cap_coll=%llu\n",
                    static_cast<unsigned long long>(cycles_), U64(t.cause).c_str(),
                    U64(t.epc).c_str(), U64(t.tval).c_str(), U64(t.target).c_str(),
                    Dec(t.occupied).c_str(), t.capture_same_sample ? 1 : 0,
                    static_cast<unsigned long long>(t.capture_cycle),
                    static_cast<unsigned long long>(
                        t.capture_cycle == UINT64_MAX ? 0 : cycles_ - t.capture_cycle),
                    static_cast<unsigned long long>(t.capture_commit_delta),
                    static_cast<unsigned long long>(t.capture_collision_delta));
      }
      // A trap cycle retires nothing ordinary: lane 0 is the trap.
      if (ref_ != nullptr && !t.is_irq && traps_.size() <= ref_traps_.size()) {
        const RefEvent& e = ref_traps_[traps_.size() - 1];
        Compare("trap " + Dec(traps_.size() - 1) + " cause matches the reference",
                t.cause == e.cause,
                "cause=" + U64(t.cause) + " expected " + U64(e.cause));
        Compare("trap " + Dec(traps_.size() - 1) +
                    " names the faulting instruction",
                t.epc == e.epc,
                "mepc=" + U64(t.epc) + " expected " + U64(e.epc));
        Compare("trap " + Dec(traps_.size() - 1) + " tval matches the reference",
                t.tval == e.tval,
                "mtval=" + U64(t.tval) + " expected " + U64(e.tval));
      }
      pending_csr_check_ = true;
      trap_sample_ = true;
      mret_expected_ = t.epc + 4;   // the handler resumes after the fault
      last_capture_cycle_ = UINT64_MAX;   // consumed by this trap
      last_capture_commit_delta_ = 0;
      last_capture_collision_delta_ = 0;
    } else if (pending_csr_check_) {
      // The CSR file latches the trap at the edge that ends the trap cycle.
      Compare("mcause holds the trap entry's cause",
              dut_->o_csr_mcause_o == traps_.back().cause,
              "mcause=" + U64(dut_->o_csr_mcause_o) + " trap cause=" +
                  U64(traps_.back().cause));
      Compare("mepc holds the trap entry's PC",
              dut_->o_csr_mepc_o == (traps_.back().epc & ~UINT64_C(3)),
              "mepc=" + U64(dut_->o_csr_mepc_o) + " trap PC=" + U64(traps_.back().epc));
      Compare("mtval holds the trap entry's tval",
              dut_->o_csr_mtval_o == traps_.back().tval,
              "mtval=" + U64(dut_->o_csr_mtval_o) + " trap tval=" +
                  U64(traps_.back().tval));
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
      mret_expected_ = 0;
    }

    // The retirement stream.
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
      r.cycle = cycles_;
      if (is_trap) {
        TrapRecord t;
        t.epc = r.pc;
        t.cycle = cycles_;
        fault_events_.push_back(t);
        if (Debug()) {
          std::printf("    [fault-ev] cycle=%llu pc=%s\n",
                      static_cast<unsigned long long>(cycles_), U64(r.pc).c_str());
        }
        continue;
      }
      Compare("an ordinary instruction retired in the trap cycle",
              !trap_sample_,
              "pc " + U64(r.pc) + " retired while a trap was being taken");
      retires_list_.push_back(r);
      retires_++;
      if (ref_ != nullptr) {
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
          Compare("retire " + Dec(idx) + " value", r.value == e.value,
                  "at " + U64(e.pc) + " x" + Dec(e.rd) + " expected " +
                      U64(e.value) + ", got " + U64(r.value));
        }
      }
    }
    ProgressCheck();
  }

  void ProgressCheck() {
    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_trap_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) {
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled: " + State());
    }
  }

 public:
  void SetMretExpected(uint64_t pc) { mret_expected_ = pc; }
  uint64_t mret_events() const { return mret_events_; }
  uint64_t csr_trap_ctr() const { return dut_->o_csr_trap_o; }
  uint64_t csr_mret_ctr() const { return dut_->o_csr_mret_o; }
  uint64_t trap_irq_ctr() const { return dut_->o_trap_irq_o; }
  uint64_t sys_exec_ctr() const { return dut_->o_sys_exec_o; }
  uint64_t muldiv_ctr() const { return dut_->o_muldiv_o; }
  bool journal_overflow_seen() const { return journal_overflow_seen_; }
  uint64_t squash_acc() const { return dut_->o_squash_acc_o; }
  uint64_t lq_alloc() const { return dut_->o_mem_lq_alloc_o; }
  const std::vector<DataMem::Txn>& txns() const { return dmem_.txns(); }
  uint64_t sq_alloc() const { return dut_->o_mem_sq_alloc_o; }

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
  uint32_t last_commit_ = 0, last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  std::vector<Retire> retires_list_;
  std::vector<TrapRecord> traps_;
  std::vector<TrapRecord> fault_events_;
  uint64_t mret_events_ = 0;
  uint64_t mret_expected_ = 0;
  uint64_t last_mstatus_ = 0;
  bool pending_csr_check_ = false;
  uint64_t mtime_ = 0;
  uint64_t last_commit_sample_ = 0, last_capture_sample_ = 0, last_collision_sample_ = 0;
  uint64_t last_capture_cycle_ = UINT64_MAX;
  uint64_t last_capture_commit_delta_ = 0, last_capture_collision_delta_ = 0;
  uint64_t last_sys_sample_ = 0, last_sys_cycle_ = UINT64_MAX;
  uint64_t last_sys_commit_delta_ = 0;
  bool trap_sample_ = false;
  bool journal_overflow_seen_ = false;
  const RefResult* ref_ = nullptr;
  std::vector<RefEvent> ref_retires_;
  std::vector<RefEvent> ref_traps_;
};

// ============================================================================
// The independent fold: the architectural register file at each trap
// ============================================================================
struct ArchFold {
  std::vector<std::array<uint64_t, 32>> at_traps;
  std::array<uint64_t, 32> after_exit_store{};
};

ArchFold FoldReference(const RefResult& ref, uint64_t exit_store_pc) {
  ArchFold fold;
  std::array<uint64_t, 32> regs{};
  bool captured_exit = false;
  for (const RefEvent& e : ref.trace) {
    if (e.trap) {
      fold.at_traps.push_back(regs);
      continue;
    }
    if (!captured_exit && e.pc == exit_store_pc) {
      fold.after_exit_store = regs;
      captured_exit = true;
    }
    if (e.reg_we && e.rd != 0) regs[e.rd] = e.value;
    if (!captured_exit && e.pc == exit_store_pc) fold.after_exit_store = regs;
  }
  return fold;
}

// ============================================================================
// The run
// ============================================================================
void RunCase(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, const Geometry& g,
             uint64_t* totals) {
  const std::string phase = "trap-precise-state";
  Program program = BuildProgram();
  if (program.scenarios.size() != 35) {
    Fail(phase, "the program has " + Dec(program.scenarios.size()) + " scenarios");
  }

  ProgImage image;
  const uint64_t program_end =
      g.reset_vector + 4ull * program.asm_.words().size();
  for (size_t i = 0; i < program.asm_.words().size(); i++) {
    image.Put(g.reset_vector + 4ull * i, program.asm_.words()[i]);
  }
  // A fetch that runs past the program lands on a self-loop rather than an
  // ECALL, so it can neither fault nor stop the machine before the exit store.
  for (uint64_t addr = program_end; addr < program_end + 0x800ull; addr += 4) {
    image.Put(addr, 0x0000006fu);
  }
  if (Debug()) {
    std::printf("  [build] %zu words, handler=%s exit_store=%s\n",
                program.asm_.words().size(), U64(program.handler_pc).c_str(),
                U64(program.exit_store_pc).c_str());
    for (const Scenario& s : program.scenarios) {
      std::printf("    [scenario] %2d %-12s %-11s fault=%s frame=%s\n", s.index,
                  KindName(s.kind), PlaceName(s.place), U64(s.fault_pc).c_str(),
                  U64(s.frame).c_str());
    }
  }

  mosaic::MemoryModel dut_mem;
  mosaic::MemoryModel ref_mem;
  // The instruction image is served to the fetch port (and to the reference
  // interpreter's own fetch) from `image`; it is deliberately **not** copied into
  // the two data memories. The program's text spans `MOSAIC_TOHOST`
  // (0x80001000), and the platform's data model treats a write there as the exit
  // protocol rather than as memory, so copying the image into the data model
  // would report a FALSE exit with an instruction word as the exit code. Nothing
  // in this program loads from its own text, so the data model needs no code.
  dut_mem.SetInputWord(0);
  ref_mem.SetInputWord(0);

  const RefResult reference = mosaic_trap::RunReference(image, g.reset_vector, &ref_mem);
  const ArchFold fold = FoldReference(reference, program.exit_store_pc);
  if (!reference.exited) {
    Fail(phase, "the reference did not reach the exit protocol: " +
                    reference.stop_reason + " at " + U64(reference.stop_pc));
  }
  if (reference.trap_causes.size() != program.scenarios.size()) {
    Fail(phase, "the reference predicts " + Dec(reference.trap_causes.size()) +
                    " traps, the program raises " + Dec(program.scenarios.size()));
  }
  if (fold.at_traps.size() != program.scenarios.size()) {
    Fail(phase, "the reference fold has " + Dec(fold.at_traps.size()) + " traps");
  }

  Harness harness(dut, reporter, kProgramCycles, &image, &dut_mem);
  harness.Configure(g);
  harness.Phase(phase);
  harness.Expect(&reference);
  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);
  while (!harness.Complete()) harness.Cycle(false);
  // The reference's stream is exhausted once the exit store has retired -- but
  // retirement authorises a store, it does not perform it. The store queue still
  // has to drain to the data port, and the exit protocol is only complete when
  // the write reaches the platform model. So the comparison is disarmed (the
  // park loop behind the exit store has no counterpart) and the run continues
  // until the store lands.
  harness.StopComparing();
  while (!dut_mem.finished() && harness.cycles() < kProgramCycles) {
    harness.Cycle(false);
  }
  for (int i = 0; i < 8; i++) harness.Cycle(false);

  // ---- 1. every scenario raised exactly one trap, in order, with the ISA's fields
  const size_t n = program.scenarios.size();
  harness.Check("the machine took one trap per scenario",
                harness.traps().size() == n,
                "traps=" + Dec(harness.traps().size()) + " expected " + Dec(n));
  if (harness.traps().size() != n) {
    Fail(phase, "cannot compare the trap list: " + harness.State());
  }

  // What the run observed about the placements, for the coverage table.
  std::vector<const TrapRecord*> obs(n, nullptr);
  for (size_t i = 0; i < n; i++) obs[i] = &harness.traps()[i];

  for (size_t i = 0; i < n; i++) {
    const Scenario& s = program.scenarios[i];
    const TrapRecord& t = harness.traps()[i];
    const std::string tag = "scenario " + Dec(i) + " (" + KindName(s.kind) + "/" +
                            PlaceName(s.place) + ")";
    harness.Check(tag + ": the trap is synchronous", !t.is_irq, "is_irq=1");
    harness.Check(tag + ": mcause is the ISA's exception code",
                  t.cause == KindCause(s.kind),
                  "cause=" + U64(t.cause) + " expected " + U64(KindCause(s.kind)));
    harness.Check(tag + ": mepc is the faulting PC", t.epc == s.fault_pc,
                  "mepc=" + U64(t.epc) + " expected " + U64(s.fault_pc));
    uint64_t want_tval = KindTval(s.kind);
#ifdef MOSAIC_TRAPSTATE_MUTANT_MTVAL_MASKED
    // NEGATIVE CONTROL (card fail mode 2): the checker is mutated to the
    // wholesale-masked mtval the fail mode describes -- it expects zero for
    // every trap. The correct machine then fails this field check.
    want_tval = 0;
#endif
    harness.Check(tag + ": mtval is the ISA's value", t.tval == want_tval,
                  "mtval=" + U64(t.tval) + " expected " + U64(want_tval));
    harness.Check(tag + ": the handler PC is the installed vector",
                  t.target == program.handler_pc,
                  "target=" + U64(t.target) + " expected " +
                      U64(program.handler_pc));
  }

  // ---- 2. the faulting instruction never retires as a success
  for (size_t i = 0; i < n; i++) {
    const Scenario& s = program.scenarios[i];
    for (const Harness::Retire& r : harness.retire_events()) {
      harness.Check("scenario " + Dec(i) + " (" + KindName(s.kind) + "/" +
                        PlaceName(s.place) + "): the faulting instruction is not "
                        "in the retirement stream",
                    r.pc != s.fault_pc,
                    "a retirement was published at " + U64(s.fault_pc) +
                        " with rd=x" + Dec(r.rd) + " we=" + Dec(r.reg_we ? 1 : 0));
    }
  }
  // Every trap also occupies lane 0 of the retirement stream as a trap event.
  for (size_t i = 0; i < n; i++) {
    const Scenario& s = program.scenarios[i];
    bool found = false;
    for (const TrapRecord& f : harness.fault_events()) {
      if (f.epc == s.fault_pc) found = true;
    }
    harness.Check("scenario " + Dec(i) + ": the trap event carries the faulting PC",
                  found, "no lane-0 trap event at " + U64(s.fault_pc));
  }

  // ---- 3. the handler's frame, field by field
  for (size_t i = 0; i < n; i++) {
    const Scenario& s = program.scenarios[i];
    const std::string tag = "scenario " + Dec(i) + " (" + KindName(s.kind) + "/" +
                            PlaceName(s.place) + ")";
    Snapshot snap;
    std::string detail;
    if (!ReadSnapshot(&dut_mem, s.frame, &snap, &detail)) Fail(phase, detail);
    harness.Check(tag + ": the handler saved mepc", snap.mepc == s.fault_pc,
                  "mepc=" + U64(snap.mepc) + " expected " + U64(s.fault_pc));
    harness.Check(tag + ": the handler saved mcause",
                  snap.mcause == KindCause(s.kind),
                  "mcause=" + U64(snap.mcause) + " expected " +
                      U64(KindCause(s.kind)));
    uint64_t want_tval = KindTval(s.kind);
#ifdef MOSAIC_TRAPSTATE_MUTANT_MTVAL_MASKED
    want_tval = 0;
#endif
    harness.Check(tag + ": the handler saved mtval", snap.mtval == want_tval,
                  "mtval=" + U64(snap.mtval) + " expected " + U64(want_tval));
    harness.Check(tag + ": mstatus at trap entry is the ISA's",
                  snap.mstatus == kMstatusTrapEntry,
                  "mstatus=" + U64(snap.mstatus) + " expected " +
                      U64(kMstatusTrapEntry));
    harness.Check(tag + ": the handler saved the installed vector",
                  snap.mtvec == program.handler_pc,
                  "mtvec=" + U64(snap.mtvec) + " expected " +
                      U64(program.handler_pc));
    harness.Check(tag + ": the handler saved the pre-trap mscratch (= its frame)",
                  snap.mscratch == s.frame,
                  "mscratch=" + U64(snap.mscratch) + " expected " + U64(s.frame));
    harness.Check(tag + ": the probe word is the scenario's pre-value",
                  snap.probe == s.probe_pre,
                  "probe=" + U64(snap.probe) + " expected " + U64(s.probe_pre));

    // The whole register file, field by field, against the independent fold.
    const std::array<uint64_t, 32>& want = fold.at_traps[i];
    size_t mismatches = 0;
    for (int r = 0; r < 32; r++) {
      uint64_t expect = want[r];
#ifdef MOSAIC_TRAPSTATE_MUTANT_RD_POLLUTED
      // NEGATIVE CONTROL (card fail mode 1): the checker is mutated to believe
      // the faulting instruction's writeback polluted its destination first, so
      // it expects the destination register to hold the load's (zero) result.
      if (IsLoadKind(s.kind) && r == 20) expect = 0;
#endif
      const bool ok = snap.gpr[r] == expect;
      harness.Check(tag + ": x" + Dec(r) + " at trap entry", ok,
                    "x" + Dec(r) + "=" + U64(snap.gpr[r]) + " expected " +
                        U64(expect));
      if (!ok) mismatches++;
    }
    harness.Check(tag + ": the register file has no unexpected field",
                  mismatches == 0, Dec(mismatches) + " register(s) differ");

    // The card's three precise-state rules, stated against constants this file
    // owns rather than only against the model: the faulting instruction's
    // destination is untouched, the *younger* register writes are invisible, and
    // the *older* canaries completed.
    harness.Check(tag + ": the faulting instruction left its destination untouched",
                  snap.gpr[20] == 0x4000ull + static_cast<uint64_t>(i),
                  "x20=" + U64(snap.gpr[20]) + " expected the pre-fault value " +
                      U64(0x4000ull + static_cast<uint64_t>(i)));
    harness.Check(tag + ": the younger register write to x18 is invisible",
                  snap.gpr[18] == 0x2000ull + static_cast<uint64_t>(i),
                  "x18=" + U64(snap.gpr[18]));
    harness.Check(tag + ": the younger register write to x19 is invisible",
                  snap.gpr[19] == 0x2001ull + static_cast<uint64_t>(i),
                  "x19=" + U64(snap.gpr[19]));
    for (int j = 0; j < 5; j++) {
      harness.Check(tag + ": the older canary x" + Dec(10 + j) + " completed",
                    snap.gpr[10 + j] == 0x1000ull + 0x10ull * i + j,
                    "x" + Dec(10 + j) + "=" + U64(snap.gpr[10 + j]));
    }
    if (s.place == Place::kAfterLong) {
      harness.Check(tag + ": the older long-latency result is complete",
                    snap.gpr[22] == kDivQuotient,
                    "x22=" + U64(snap.gpr[22]) + " expected " +
                        U64(kDivQuotient));
    }
  }

  // ---- 4. the placements, from what the run produced
  //
  // The measurements are the machine's own: `cap_gap` is the cycles between the
  // exception being captured (the LSU completion accepted) and the trap being
  // taken, `occupied` is the ROB depth at the trap, and `cap_commit` /
  // `cap_collision` are the retirements and writeback collisions in the capture
  // cycle. A memory fault taken within a few cycles of its capture was resolved
  // at the head; one taken twenty or more cycles later had older work in front
  // of it and had to wait. A system trap is resolved at the head by the system
  // unit, so its depth is the evidence: a shallow ROB when it is taken, a deep
  // one when older work is still in flight.
  auto cap_gap = [](const TrapRecord& t) -> uint64_t {
    return (t.capture_cycle == UINT64_MAX) ? 0 : t.cycle - t.capture_cycle;
  };
  // The final register dump's x24 is the replayed result of the last
  // "before a long-latency instruction" `div`: the instruction the trap squashed
  // and `mret` re-ran.
  uint64_t dump_lat = 0;
  if (dut_mem.Read(kDump + 8ull * 24, 8, &dump_lat) != mosaic::AccessStatus::kOk) {
    Fail(phase, "the final register dump is not readable");
  }
  harness.Check("the squashed long-latency instruction re-executed after the trap",
                dump_lat == kDivQuotient,
                "dump x24=" + U64(dump_lat) + " expected " + U64(kDivQuotient));
  int cover_head = 0, cover_away = 0, cover_before = 0, cover_after = 0,
      cover_same = 0;
  for (size_t i = 0; i < n; i++) {
    const Scenario& s = program.scenarios[i];
    const TrapRecord& t = harness.traps()[i];
    const bool head_like = IsMemoryKind(s.kind) ? (cap_gap(t) <= 8)
                                                : (t.occupied <= 12);
    const bool away_like = IsMemoryKind(s.kind) ? (cap_gap(t) >= 16)
                                                : (t.occupied >= 20);
    switch (s.place) {
      case Place::kHead:
        if (head_like) cover_head++;
        break;
      case Place::kAway:
        if (away_like) cover_away++;
        break;
      case Place::kBeforeLong:
        // The frame check proved the younger `div` had no architectural effect
        // at the trap (its result is not in the frame), and the final register
        // dump carries its replayed result -- the evidence the squashed
        // instruction was re-executed rather than lost.
        if (dump_lat == kDivQuotient) cover_before++;
        break;
      case Place::kAfterLong:
        // The older `div`'s result is in the trap frame (checked above) and the
        // trap's own delay (cap_gap) is the evidence the trap waited for it.
        if (away_like) cover_after++;
        break;
      case Place::kSameCycle:
        if (IsMemoryKind(s.kind)) {
          // An older instruction retired, or two completions collided, in the
          // very cycle the fault completed.
          if (t.capture_commit_delta > 0 || t.capture_collision_delta > 0) {
            cover_same++;
          }
        } else {
          // A system macro is resolved at the head; the same-cycle situation is
          // the macro completing with an ordinary instruction retiring in that
          // cycle, which `sys_commit_delta` records.
          if (t.sys_commit_delta > 0) cover_same++;
        }
        break;
    }
  }
  harness.Check("the ROB-head placement was produced", cover_head > 0,
                "count=" + Dec(cover_head));
  harness.Check("the away-from-head placement was produced", cover_away > 0,
                "count=" + Dec(cover_away));
  harness.Check("the before-a-long-latency placement was produced", cover_before > 0,
                "count=" + Dec(cover_before));
  harness.Check("the after-a-long-latency placement was produced", cover_after > 0,
                "count=" + Dec(cover_after));
  harness.Check("the same-cycle-completion placement was produced", cover_same > 0,
                "count=" + Dec(cover_same));

  // ---- 5. the CSR file's own counters
  harness.Check("the CSR file counted one trap and one MRET per scenario",
                harness.csr_trap_ctr() == n && harness.csr_mret_ctr() == n,
                "traps=" + Dec(harness.csr_trap_ctr()) + " mret=" +
                    Dec(harness.csr_mret_ctr()));
  harness.Check("no trap was driven by the interrupt path",
                harness.trap_irq_ctr() == 0, "irq traps=" + Dec(harness.trap_irq_ctr()));

  // The mret restoration: the trap entry left mstatus = 0x1800 and the mret
  // restored MIE from MPIE and set MPIE, so the architectural mstatus between
  // traps is 0x1880.
  harness.Check("mstatus after every MRET is the ISA's restoration",
                (dut->o_csr_mstatus_o & ~0x1800ull) == (kMstatusAfterMret & ~0x1800ull),
                "mstatus=" + U64(dut->o_csr_mstatus_o));

  // ---- 6. replay: the post-trap store reached memory and the shadow memory agrees
  // The final probe value is the *last* scenario's post-value: the younger store
  // was squashed by the trap and re-executed after `mret`, so it must have landed.
  uint64_t probe_now = 0;
  if (dut_mem.Read(kProbe, 8, &probe_now) != mosaic::AccessStatus::kOk) {
    Fail(phase, "the probe word is not readable");
  }
  uint64_t expect_probe = program.scenarios.back().probe_post;
#ifdef MOSAIC_TRAPSTATE_MUTANT_REPLAY_LOST
  // NEGATIVE CONTROL (card fail mode 4): the checker is mutated to believe the
  // instruction after the trap was lost on replay, so it expects the probe to
  // still hold the pre-value. The store actually re-executed, so this check
  // fails -- which is what a machine that lost the instruction would trip.
  expect_probe = program.scenarios.back().probe_pre;
#endif
  harness.Check("the post-trap store reached memory (nothing was lost on replay)",
                probe_now == expect_probe,
                "probe=" + U64(probe_now) + " expected " + U64(expect_probe));

  // The machine's whole RAM must equal the independent model's, which owns its
  // own copy and executed the same words (including every replayed store).
  uint64_t mem_mismatch = 0;
  uint64_t first_bad = 0;
  for (uint64_t addr = MOSAIC_RAM_BASE;
       addr < MOSAIC_RAM_BASE + MOSAIC_RAM_SIZE; addr += 8) {
    uint64_t a = 0, b = 0;
    if (dut_mem.Read(addr, 8, &a) != mosaic::AccessStatus::kOk ||
        ref_mem.Read(addr, 8, &b) != mosaic::AccessStatus::kOk) {
      continue;
    }
    if (a != b) {
      if (mem_mismatch == 0) first_bad = addr;
      mem_mismatch++;
    }
  }
  harness.Check("the machine's RAM equals the independent model's",
                mem_mismatch == 0,
                Dec(mem_mismatch) + " differing word(s), first at " + U64(first_bad));

  // ---- 7. the exit protocol
  harness.Check("the program reached its exit protocol",
                dut_mem.finished() && dut_mem.passed() && dut_mem.exit_code() == 1,
                "finished=" + Dec(dut_mem.finished() ? 1 : 0) + " passed=" +
                    Dec(dut_mem.passed() ? 1 : 0) + " tohost=" +
                    U64(dut_mem.exit_code()));

  // ---- evidence
  for (size_t i = 0; i < n; i++) {
    const Scenario& s = program.scenarios[i];
    const TrapRecord& t = harness.traps()[i];
    std::printf("  [obs] %2zu %-12s %-11s cause=%2llu mepc=%s mtval=%s "
                "occupied=%2llu cap_gap=%3llu cap_commit=%llu cap_coll=%llu "
                "sys_commit=%llu\n",
                i, KindName(s.kind), PlaceName(s.place),
                static_cast<unsigned long long>(t.cause),
                U64(t.epc).c_str(), U64(t.tval).c_str(),
                static_cast<unsigned long long>(t.occupied),
                static_cast<unsigned long long>(cap_gap(t)),
                static_cast<unsigned long long>(t.capture_commit_delta),
                static_cast<unsigned long long>(t.capture_collision_delta),
                static_cast<unsigned long long>(t.sys_commit_delta));
  }
  std::printf("  coverage: head=%d away=%d before-long=%d after-long=%d "
              "same-cycle=%d\n", cover_head, cover_away, cover_before, cover_after,
              cover_same);
  std::printf("  traps=%zu retires=%llu cycles=%llu comparisons=%llu squash=%llu "
              "muldiv=%llu journal_ovf_seen=%d\n",
              harness.traps().size(),
              static_cast<unsigned long long>(harness.retires()),
              static_cast<unsigned long long>(harness.cycles()),
              static_cast<unsigned long long>(harness.comparisons()),
              static_cast<unsigned long long>(harness.squash_acc()),
              static_cast<unsigned long long>(harness.muldiv_ctr()),
              harness.journal_overflow_seen() ? 1 : 0);

  totals[0] += harness.cycles();
  totals[1] += harness.comparisons();
  totals[2] += harness.retires();
  totals[3] += harness.traps().size();
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
    if (geometry.reset_vector != MOSAIC_RESET_VECTOR) {
      Fail("geometry", "the reset vector is not the profile's: " +
                           U64(geometry.reset_vector));
    }
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    if (geometry.retire_width < 2) {
      Fail("geometry", "the profile does not retire two instructions per cycle");
    }

    uint64_t totals[4] = {0, 0, 0, 0};
    RunCase(&dut, &reporter, geometry, totals);

    detail = "checks=" + Dec(reporter.checks()) + " comparisons=" + Dec(totals[1]) +
             " cycles=" + Dec(totals[0]) + " retires=" + Dec(totals[2]) +
             " traps=" + Dec(totals[3]) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the synchronous trap path holds precise state",
                      "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
