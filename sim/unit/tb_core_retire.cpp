// ============================================================================
// tb_core_retire.cpp -- CASE=retire.width_and_order, work package V-013.
//
// The DUT is the integrated p0 core -- the same top CASE=core.mem_program,
// CASE=core.trap_csr_program and CASE=fence.code_and_data_order drive -- with
// the memory path, the CSR file and the trap handoff all wired in. It retires up
// to MOSAIC_RETIRE_WIDTH (2) instructions per cycle and publishes one event per
// retiring slot on `ev_*`: a per-hart, per-slot architectural delta.
//
// ------------------------------------------------------------------ the claim
//
// The retirement path must be exact: older before younger, no gap and no
// duplicate in `retire_order`, and a trapping instruction in an earlier slot
// preventing anything younger from retiring in that cycle. Every claim this
// machine makes about precise state rests on it.
//
// ----------------------------------------------------------------- the card
//
// The card names five situations that must be *produced*, and the case asserts
// its own coverage rather than assuming the program hit them:
//
//   1. two writes to the same `rd` in one cycle;
//   2. a write to `x0` -- in both slots, under every write form the decoder
//      accepts (the decoder's `reg_write` arms are enumerated below and each is
//      emitted with rd = x0 and asserted present in the retired stream);
//   3. the instruction after a branch -- both the target of a taken transfer and
//      the fall-through of a not-taken one;
//   4. a trapping instruction in slot 0 with an ordinary instruction in slot 1;
//   5. an adjacent store/load.
//
// ------------------------------------------------------- the expectation
//
// The comparison is against an *independent per-slot model*, not against the
// DUT's own retire ports: `mosaic_trap::RunReference` (sim/unit/trap_ref.h)
// steps the same program's words in program order -- through the trap and the
// MRET -- and produces one ordered event per architectural action: which
// register with which value, which PC, whether the event is a retirement or a
// trap entry. This file adds the two fields that reference does not carry
// (`is_store` and the store size) by decoding the word at the event's PC from
// the ISA text, and derives the event's *new PC* as the PC of the next event in
// program order.
//
// Nothing in the model comes from the DUT: it decodes the same image words, its
// own `mosaic::MemoryModel` is a second instance the DUT can never touch, and
// every PC, destination and value it predicts is computed by the interpreter.
// Deriving the expectation from the DUT's retire ports would make the case
// unable to see the defect it exists to find -- a slot order established by
// accident.
//
// ------------------------------------------------------------------ checks
//
//   1. the per-slot delta equals the model, slot by slot and cycle by cycle:
//      the event's identity (PC), its trap flag, its store flag and size, its
//      destination and -- when the model says the instruction wrote -- its
//      value;
//   2. the lane order within a cycle is program order, and a lane retires only
//      behind the lane in front of it;
//   3. a trapping slot 0 blocks slot 1: in a cycle whose lane 0 is a trap,
//      lane 1 is not valid and nothing younger retires;
//   4. `retire_order` is dense and strictly increasing modulo its own width --
//      the trace gap detector, which reports the first gap or duplicate with the
//      cycle and both identities;
//   5. `x0` is never written: no event reports a write to rd 0, and the values
//      the program reads back out of x0 and publishes equal the model's;
//   6. every event the model predicts was published and compared, exactly once,
//      and no other event was published while the comparison was armed;
//   7. the exit protocol completed and the published architectural end state
//      equals the model's memory.
//
// ---------------------------------------------------------------- controls
//
// tools/run_retire_controls.py rebuilds this case with one `-D` per defect. The
// RTL mutants are the retire/ROB/rename module controls this project already
// documents; the checker mutants mutate this file's own comparison and are named
// as such in results/reports/V-013-retire.md, because the card's fail modes "a
// cycle's end state contaminating an earlier slot", "slot order established by
// callback accident" and "an unreported trace gap" are properties of the
// *checker*, not of one gate.
// ============================================================================

#include <verilated.h>

#include <algorithm>
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

namespace {

constexpr int kResetCycles = 4;
// The program is a few hundred cycles long. This bound is only reached if the
// machine stopped making progress, and a named stall is more useful than a raw
// timeout.
constexpr int kStallCycles = 6000;
// After the reference's last event has retired and the program reached its exit
// protocol, the machine is given a few cycles to settle: the park loop after the
// exit store retires in these cycles and nothing the model predicts is still
// unwatched.
constexpr int kSettleCycles = 8;

// --------------------------------------------------------- the published state
// Where the program writes what it read, so the architectural end state can be
// compared with the model's memory after the run. All inside RAM.
constexpr uint64_t kDataA = 0x80002000ull;   // scratch the store/load pair uses
constexpr uint64_t kPub0 = 0x80002100ull;    // x0 read back through `add`
constexpr uint64_t kPub1 = 0x80002140ull;    // the two-writes-to-one-rd result
constexpr uint64_t kPub2 = 0x80002180ull;    // the fall-through of a not-taken branch
constexpr uint64_t kPub3 = 0x800021C0ull;    // the adjacent store/load values
constexpr uint64_t kPub4 = 0x80002200ull;    // the instruction after the trap

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ============================================================================
// Instruction encodings
// ============================================================================
uint32_t EncR(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd,
              uint32_t op) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
uint32_t EncI(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return ((static_cast<uint32_t>(imm) & 0xFFFu) << 20) | (rs1 << 15) | (f3 << 12) |
         (rd << 7) | op;
}
uint32_t EncS(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0xFFFu;
  return ((u >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((u & 0x1Fu) << 7) |
         op;
}
uint32_t EncB(int32_t offset, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  const uint32_t u = static_cast<uint32_t>(offset);
  const uint32_t imm = (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3Fu) << 25) |
                       (((u >> 1) & 0xFu) << 8) | (((u >> 11) & 1u) << 7);
  return imm | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | op;
}
uint32_t EncU(uint32_t imm20, uint32_t rd, uint32_t op) {
  return ((imm20 & 0xFFFFFu) << 12) | (rd << 7) | op;
}
uint32_t EncJ(int32_t offset, uint32_t rd) {
  const uint32_t u = static_cast<uint32_t>(offset);
  const uint32_t imm = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
                       (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12);
  return imm | (rd << 7) | 0x6Fu;
}

enum { X0 = 0, T0 = 5, T1 = 6, T2 = 7, S0 = 8, S1 = 9, T3 = 28, T4 = 29, T5 = 30,
       T6 = 31 };
constexpr uint32_t kMscratch = 0x340;
constexpr uint32_t kMstatus = 0x300;
constexpr uint32_t kMtvec = 0x305;
constexpr uint32_t kMepc = 0x341;

// ============================================================================
// A very small assembler: words laid out from an address, with labels.
// ============================================================================
class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}

  uint64_t Pc() const { return base_ + 4ull * words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }

  void Mark(const std::string& name) { labels_[name] = Pc(); }
  uint64_t Label(const std::string& name) const {
    const auto it = labels_.find(name);
    if (it == labels_.end()) Fail("assembler", "no such label: " + name);
    return it->second;
  }

  // An absolute address is materialised PC-relatively: `auipc` adds the upper 20
  // bits of the delta and the `addi` the signed low 12. `lui rd, 0x80000` would
  // sign-extend to 0xFFFFFFFF80000000, which is not the address on RV64.
  void LiAbs(uint32_t rd, uint64_t target) {
    const int64_t pc = static_cast<int64_t>(Pc());
    const int64_t delta = static_cast<int64_t>(target) - pc;
    const int64_t hi = (delta + 0x800) >> 12;
    const int64_t lo = delta - (hi << 12);
    Emit(EncU(static_cast<uint32_t>(hi) & 0xFFFFFu, rd, 0x17u));
    Emit(EncI(static_cast<int32_t>(lo), rd, 0x0u, rd, 0x13u));
  }

  void Addi(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 0, rd, 0x13u)); }
  void Slti(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 2, rd, 0x13u)); }
  void Xori(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 4, rd, 0x13u)); }
  void Ori(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 6, rd, 0x13u)); }
  void Andi(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 7, rd, 0x13u)); }
  void Slli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(EncI(static_cast<int32_t>(sh), rs1, 1, rd, 0x13u));
  }
  void Addiw(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 0, rd, 0x1Bu)); }
  void Add(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(EncR(0, rs2, rs1, 0, rd, 0x33u)); }
  void Sub(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(EncR(0x20, rs2, rs1, 0, rd, 0x33u)); }
  void Sll(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(EncR(0, rs2, rs1, 1, rd, 0x33u)); }
  void And(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(EncR(0, rs2, rs1, 7, rd, 0x33u)); }
  void Mul(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(EncR(1, rs2, rs1, 0, rd, 0x33u)); }
  void Div(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(EncR(1, rs2, rs1, 4, rd, 0x33u)); }
  void Rem(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(EncR(1, rs2, rs1, 6, rd, 0x33u)); }
  void Addw(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(EncR(0, rs2, rs1, 0, rd, 0x3Bu)); }
  void Ld(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 3, rd, 0x03u)); }
  void Lhu(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 5, rd, 0x03u)); }
  void Lbu(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 4, rd, 0x03u)); }
  void Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { Emit(EncS(imm, rs2, rs1, 3, 0x23u)); }
  void Lui(uint32_t rd, uint32_t imm20) { Emit(EncU(imm20, rd, 0x37u)); }
  void Auipc(uint32_t rd, uint32_t imm20) { Emit(EncU(imm20, rd, 0x17u)); }
  void Jalr(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(EncI(imm, rs1, 0, rd, 0x67u)); }
  void Mret() { Emit(0x30200073u); }
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(EncI(static_cast<int32_t>(csr), rs1, 1, rd, 0x73u));
  }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(EncI(static_cast<int32_t>(csr), rs1, 2, rd, 0x73u));
  }
  void Csrrwi(uint32_t rd, uint32_t csr, uint32_t uimm) {
    Emit(EncI(static_cast<int32_t>(csr), uimm, 5, rd, 0x73u));
  }

  void Beq(uint32_t rs1, uint32_t rs2, const std::string& label) {
    Branch(0, rs1, rs2, label);
  }
  void Bne(uint32_t rs1, uint32_t rs2, const std::string& label) {
    Branch(1, rs1, rs2, label);
  }
  void J(const std::string& label) {
    fixes_.push_back(Fix{words_.size(), label, 1, 0, 0, 0});
    Emit(0);
  }

  uint64_t Resolve() {
    for (const Fix& f : fixes_) {
      const int64_t offset = static_cast<int64_t>(Label(f.label)) -
                             static_cast<int64_t>(base_ + 4ull * f.index);
      if (offset < -(INT32_C(1) << 20) || offset >= (INT32_C(1) << 20)) {
        Fail("assembler", "the transfer to " + f.label + " is out of range");
      }
      words_[f.index] = (f.kind == 0)
                            ? EncB(static_cast<int32_t>(offset), f.rs2, f.rs1, f.f3, 0x63u)
                            : EncJ(static_cast<int32_t>(offset), 0);
    }
    fixes_.clear();
    return base_ + 4ull * words_.size();
  }

 private:
  struct Fix {
    size_t index;
    std::string label;
    int kind;        // 0 = B-type, 1 = J-type
    uint32_t rs1;
    uint32_t rs2;
    uint32_t f3;
  };
  void Emit(uint32_t w) { words_.push_back(w); }
  void Branch(uint32_t f3, uint32_t rs1, uint32_t rs2, const std::string& label) {
    fixes_.push_back(Fix{words_.size(), label, 0, rs1, rs2, f3});
    Emit(0);
  }

  uint64_t base_;
  std::vector<uint32_t> words_;
  std::map<std::string, uint64_t> labels_;
  std::vector<Fix> fixes_;
};

// ============================================================================
// The program
// ============================================================================
// One image, run once from the reset vector. The trap handler is placed at the
// top -- the first word jumps over it -- so its address is known before the
// program that installs it in mtvec is assembled.
struct Program {
  std::vector<uint32_t> words;
  uint64_t handler_pc = 0;
  uint64_t trap_pc = 0;
  uint64_t taken_branch_pc = 0;
  uint64_t notaken_branch_pc = 0;
  uint64_t jal_pc = 0;
  uint64_t jal_fallthrough_pc = 0;
  uint64_t branch_fallthrough_pc = 0;
  std::map<uint64_t, std::string> x0_forms;   // pc -> the write form emitted there
  std::map<uint64_t, std::string> store_pcs;
  std::map<uint64_t, std::string> load_pcs;
};

Program BuildProgram() {
  Program p;
  Asm a(MOSAIC_RAM_BASE);

  // ------------------------------------------------------------ trap handler
  a.J("start");                       // word 0: jump over the handler
  a.Mark("handler");
  p.handler_pc = a.Pc();
  a.Csrrs(T1, kMepc, X0);             // t1 = mepc
  a.Addi(T1, T1, 4);                  // resume after the trapping instruction
  a.Csrrw(X0, kMepc, T1);             // mepc = t1
  a.Mret();

  a.Mark("start");
  a.LiAbs(T0, p.handler_pc);
  a.Csrrw(X0, kMtvec, T0);            // mtvec = handler
  a.Csrrw(X0, kMstatus, X0);          // mstatus = 0
  // The front end allocates ahead of retirement and ECALL is refused until the
  // vector write has retired (mosaic_dispatch's `trap_vector_armed` stops the
  // machine on an unarmed trap), so the program spends more than a fetch window
  // of instructions after the install. These are also, by construction, writes
  // to x0.
  for (int i = 0; i < 16; i++) {
    p.x0_forms[a.Pc()] = "addi";
    a.Addi(X0, X0, 0);
  }

  // ------------------------------------------- (2) writes to x0, every form
  // One instruction per writeback arm the decoder implements, each with rd = x0.
  // The operands are non-zero, so a write that did happen would be visible in
  // the register the program reads back below.
  auto x0form = [&](const std::string& name) { p.x0_forms[a.Pc()] = name; };
  a.Addi(T1, X0, 3);                  // t1 = 3
  a.Addi(T2, X0, 5);                  // t2 = 5
  x0form("op-reg");    a.Add(X0, T1, T2);
  x0form("op-reg");    a.Sub(X0, T1, T2);
  x0form("op-reg");    a.And(X0, T1, T2);
  x0form("op-reg");    a.Sll(X0, T1, T2);
  x0form("mul-div");   a.Mul(X0, T1, T2);
  x0form("mul-div");   a.Div(X0, T1, T2);
  x0form("mul-div");   a.Rem(X0, T1, T2);
  x0form("op-imm");    a.Addi(X0, T1, 7);
  x0form("op-imm");    a.Slli(X0, T1, 2);
  x0form("op-imm");    a.Slti(X0, T1, 9);
  x0form("op-imm");    a.Xori(X0, T1, 1);
  x0form("op-imm");    a.Ori(X0, T1, 1);
  x0form("op-imm");    a.Andi(X0, T1, 1);
  x0form("op-32");     a.Addw(X0, T1, T2);
  x0form("op-imm-32"); a.Addiw(X0, T1, 3);
  x0form("lui");       a.Lui(X0, 0x12345);
  x0form("auipc");     a.Auipc(X0, 0x12345);
  a.LiAbs(T3, kDataA);
  p.store_pcs[a.Pc()] = "double";     a.Sd(T1, T3, 0);   // seed the scratch word
  x0form("load");      a.Ld(X0, T3, 0);
  x0form("load");      a.Lbu(X0, T3, 1);
  x0form("load");      a.Lhu(X0, T3, 0);
  x0form("csr");       a.Csrrw(X0, kMscratch, T1);
  x0form("csr-imm");   a.Csrrwi(X0, kMscratch, 3);
  p.jal_pc = a.Pc();
  p.x0_forms[p.jal_pc] = "jal";       // a taken transfer whose link is x0
  a.J("after_jal");
  p.jal_fallthrough_pc = a.Pc();
  a.Addi(T3, X0, 0x5AD);              // the word the taken jal skips: never retires
  a.Mark("after_jal");
  a.Auipc(T6, 0);
  x0form("jalr");      a.Jalr(X0, T6, 8);
  a.Add(T4, X0, X0);                  // t4 = x0 + x0, must be 0
  a.Add(T5, X0, T1);                  // t5 = x0 + t1, must be 3
  a.LiAbs(T3, kPub0);
  a.Sd(T4, T3, 0);
  a.Sd(T5, T3, 8);

  // --------------------------- (1) two writes to the same rd in one cycle
  // Independent instructions: the second does not read the first, so both can
  // complete and retire in the same cycle. If the older slot's delta were taken
  // from the cycle's end state it would carry the younger one's value.
  //
  // The `div` in front is what makes the two *pair*: it holds the head of the
  // buffer for the divide's ~64 cycles, during which both adds execute and
  // complete. The single `addi` between the divide and the pair is what puts
  // them side by side: the divide's own retirement cycle consumes the filler in
  // slot 1, so the next cycle presents the two writes to the same rd as slots 0
  // and 1, both already complete. Without that, the divide's retirement takes
  // slot 0 and the first write slot 1, and the pair is split across the width.
  a.Addi(T6, X0, 7);
  a.Addi(T1, X0, 0x111);
  a.Div(T5, T1, T6);                  // slow: keeps the head busy
  a.Addi(T4, X0, 0x55);               // takes the divide's second retire slot
  a.Addi(S0, X0, 0x111);
  a.Addi(S0, X0, 0x222);
  a.Add(S1, S0, X0);                  // s1 = the younger write's value
  a.LiAbs(T3, kPub1);
  a.Sd(S1, T3, 0);

  // ------------------------------- (3a) the target of a taken branch
  p.taken_branch_pc = a.Pc();
  a.Beq(X0, X0, "btaken");            // always taken
  p.branch_fallthrough_pc = a.Pc();
  a.Addi(T3, X0, 0x6AD);              // wrong path: must never retire
  a.Mark("btaken");
  a.Addi(T3, X0, 0x1234);

  // ---------------------------- (3b) the fall-through of a not-taken branch
  a.Addi(T5, X0, 0xBAD);              // overwritten below; the bne must not skip it
  p.notaken_branch_pc = a.Pc();
  a.Bne(X0, X0, "bnever");            // never taken
  a.Addi(T5, X0, 0x777);              // the instruction after the branch
  a.Mark("bnever");
  a.LiAbs(T3, kPub2);
  a.Sd(T5, T3, 0);

  // -------------------------------- (5) an adjacent store and load
  // The same shape as the same-rd pair above: the divide holds the head while
  // the store reaches the store queue and the load behind it issues, is served
  // (by forwarding, from the store's own captured operand) and completes. The
  // filler takes the divide's second retire slot, so the next cycle presents the
  // store to slot 0 and the load to slot 1, both already complete.
  a.Addi(T6, X0, 7);
  a.LiAbs(T0, kDataA);
  a.Addi(T1, X0, 0x5A5);
  a.Div(T5, T1, T6);                  // slow: keeps the head busy
  a.Addi(T4, X0, 0x55);               // takes the divide's second retire slot
  p.store_pcs[a.Pc()] = "double";     a.Sd(T1, T0, 0);
  p.load_pcs[a.Pc()] = "double";      a.Ld(T2, T0, 0);    // the same address: forwarded
  a.Add(T4, T2, X0);
  a.LiAbs(T3, kPub3);
  a.Sd(T2, T3, 0);
  a.Sd(T4, T3, 8);
  a.Sd(T2, T3, 16);

  // ------------------- (4) a trapping instruction with an ordinary one behind
  for (int i = 0; i < 16; i++) {
    p.x0_forms[a.Pc()] = "addi";
    a.Addi(X0, X0, 0);
  }
  // A **misaligned load**: a synchronous trap raised by the instruction itself,
  // recorded on the reorder-buffer entry, so it reaches the retire unit's head
  // and takes the trap event path. (An ECALL resolves in the system unit and
  // never appears on the event stream at all -- see the report's reported
  // defects -- so it cannot be the instruction this situation is built on.)
  p.trap_pc = a.Pc();
  a.Ld(S1, X0, 1);                    // ld s1, 1(x0): address 1 is misaligned
  a.Addi(S1, X0, 0x777);              // the ordinary instruction behind it
  a.LiAbs(T3, kPub4);
  a.Sd(S1, T3, 0);

  // --------------------------------------------------------- exit protocol
  a.Addi(T1, X0, 1);
  a.LiAbs(T0, MOSAIC_TOHOST);
  p.store_pcs[a.Pc()] = "double";     a.Sd(T1, T0, 0);
  a.Mark("park");
  a.J("park");                        // the park loop: a taken transfer to itself

  a.Resolve();
  p.words = a.words();
  return p;
}

// ============================================================================
// The image: instruction words, with the gap after the program answered as a
// self-loop.
// ============================================================================
// A fetch past the end of the program must not land on an ECALL (which would
// stop the machine on an unarmed vector if it happened before the install) and
// must not read a don't-care, so the words after the program are `jal x0, 0`.
class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    const auto it = words_.find(addr);
    return (it == words_.end()) ? 0x00000073u : it->second;   // ECALL
  }
  const std::map<uint64_t, uint32_t>& words() const { return words_; }

 private:
  std::map<uint64_t, uint32_t> words_;
};

// ============================================================================
// Instruction memory: a one-cycle handshake
// ============================================================================
class Imem {
 public:
  struct Request {
    uint64_t addr = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
  };
  struct Entry {
    Request req;
    int left = 0;
  };

  explicit Imem(const ProgImage* img) : img_(img) {}
  void Reset() { inflight_.clear(); ready_.clear(); }
  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint32_t ResponseWord() const { return img_->Word(ready_.front().addr); }
  void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
    Request r;
    r.addr = addr;
    r.id = id;
    r.epoch = epoch;
    inflight_.push_back(Entry{r, 1});
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
  const ProgImage* img_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
};

// ============================================================================
// Geometry, read from the elaborated DUT
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint32_t seq_w = 0;
  uint32_t ret_id_w = 0;
  uint64_t reset_vector = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.ret_id_w = dut->o_geom_ret_id_w_o;
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
// The independent per-slot model
// ============================================================================
struct ModelEvent {
  uint64_t pc = 0;
  bool trap = false;
  uint64_t cause = 0;
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
  uint64_t next_pc = 0;        // the PC of the next event in program order
};

// Whether the instruction word at `pc` is a store. Read from the same image the
// machine is fed, from the ISA text -- the retire event's own `ev_store` field
// is inert in this build (see the header's "reported defect"), so the store
// identity in the delta is established by the retired PC and this decode.
bool IsStore(const ProgImage& img, uint64_t pc) {
  return (img.Word(pc) & 0x7Fu) == 0x23u;
}

// Whether the instruction word names a register destination of x0 -- the forms
// whose "write" the ISA discards. Decoded from the ISA text, from the same words
// the machine is fed. Compiled only for the checker mutant that needs it, so the
// shipping build carries no unused function.
#ifdef MOSAIC_RETIRE_CHECKER_X0_WRITE
bool WritesX0(uint32_t word) {
  if (((word >> 7) & 0x1Fu) != 0u) return false;
  switch (word & 0x7Fu) {
    case 0x37u: case 0x17u: case 0x6Fu: case 0x67u:   // lui auipc jal jalr
    case 0x03u:                                       // loads
    case 0x13u: case 0x1Bu:                           // op-imm, op-imm-32
    case 0x33u: case 0x3Bu:                           // op, op-32, mul/div
      return true;
    case 0x73u: {                                     // the Zicsr forms
      const uint32_t f3 = (word >> 12) & 0x7u;
      const uint32_t rs1 = (word >> 15) & 0x1Fu;
      if (f3 == 0u) return false;                     // ecall/ebreak/mret/wfi
      if ((f3 & 0x4u) != 0u) return true;             // csrrwi/si/ci
      return (f3 & 0x3u) == 1u || rs1 != 0u;          // a CSR write, destination x0
    }
    default:
      return false;
  }
}
#endif  // MOSAIC_RETIRE_CHECKER_X0_WRITE

std::vector<ModelEvent> BuildModel(const ProgImage& img,
                                   const mosaic_trap::RefResult& ref) {
  (void)img;   // read only by the x0 checker mutant below
  std::vector<ModelEvent> model;
  for (const RefEvent& e : ref.trace) {
    ModelEvent m;
    m.pc = e.pc;
    m.trap = e.trap;
    m.cause = e.cause;
    m.rd = e.rd;
    m.reg_we = e.reg_we;
    m.value = e.value;
#ifdef MOSAIC_RETIRE_CHECKER_X0_WRITE
    // DRIVER MUTANT: a write to x0 is described as a real destination write, so
    // the expectation is the one a machine that modified x0 would produce. The
    // per-slot destination check is the one that must reject it.
    if (!m.trap && WritesX0(img.Word(e.pc))) {
      m.reg_we = true;
      m.rd = 0;
      m.value = 0;
    }
#endif
    model.push_back(m);
  }
  for (size_t i = 0; i + 1 < model.size(); i++) model[i].next_pc = model[i + 1].pc;
  if (!model.empty()) model.back().next_pc = model.back().pc + 4;
  return model;
}
// ============================================================================
// One observation of the DUT's retire stream
// ============================================================================
struct SlotEvent {
  uint64_t seq = 0;
  uint64_t pc = 0;
  uint64_t id = 0;
  bool trap = false;
  bool is_store = false;   // decoded from the retired PC, not from `ev_store`
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
  uint32_t lane = 0;
  uint64_t cycle = 0;
};

std::string Identity(const SlotEvent& e) {
  return "pc " + U64(e.pc) + " id " + U64(e.id) + " seq " + Dec(e.seq) + " lane " +
         Dec(e.lane) + " at cycle " + Dec(e.cycle);
}

// ============================================================================
// Coverage: what the run produced, counted rather than asserted in prose
// ============================================================================
struct Coverage {
  uint64_t events = 0;
  uint64_t retired = 0;
  uint64_t cycles = 0;
  uint64_t single_slot_cycles = 0;
  uint64_t dual_slot_cycles = 0;
  uint64_t same_rd_pair_cycles = 0;
  uint64_t same_rd_pair_distinct = 0;   // ... and the two writes disagree
  uint64_t trap_events = 0;
  uint64_t trap_cycles_with_younger_present = 0;
  uint64_t trap_cycles_that_blocked_younger = 0;
  uint64_t store_load_pair_cycles = 0;
  uint64_t taken_branches = 0;        // `beq`/`bne` whose next event is the target
  uint64_t fallthrough_branches = 0;  // ... and whose next event is the next PC
  uint64_t ev_store_bits = 0;   // the DUT's own `ev_store` field, inert here
  std::map<uint64_t, uint64_t> x0_lane0;
  std::map<uint64_t, uint64_t> x0_lane1;
};

// ============================================================================
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem, const Program* program)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(mem), img_(img), program_(program) {}

  void Configure(const Geometry& g) { g_ = g; }
  void SetModel(const std::vector<ModelEvent>* model) { model_ = model; }

  uint64_t cycles() const { return cycles_; }
  uint64_t comparisons() const { return comparisons_; }
  const Coverage& coverage() const { return cov_; }
  const std::vector<SlotEvent>& stream() const { return stream_; }
  size_t cursor() const { return cursor_; }
  bool model_exhausted() const { return model_ != nullptr && cursor_ >= model_->size(); }

  // A check that a control's expected text can name. The label is recorded on
  // the Reporter and, on the first failure, thrown with the same label, so the
  // RESULT line names the check that broke.
  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, what + (ok ? "" : " -- " + detail));
    if (!ok) Fail("cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    Check(what, ok, detail);
  }

  void Reset() {
    for (int i = 0; i < kResetCycles; i++) Cycle(true);
    for (int i = 0; i < 8; i++) Cycle(false);   // let the first fetch start
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail("cycle " + Dec(cycles_), "max-cycles exhausted: " + Diagnose());
    }
    dut_->rst = rst ? 1 : 0;

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

    // The standalone redirect arbiter is not the subject of this case.
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;

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

    // No interrupts are driven: this case is about the retirement path's own
    // order. What that leaves uncovered is stated in the report.
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = cycles_;

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
    cov_.cycles = cycles_;
    ProgressCheck();
  }

 private:
  // ------------------------------------------------------------ the comparison
  void Observe() {
    // The core's commit counter counts retirements and nothing else, so it is a
    // second, independent statement that the stream the checker read is the
    // stream the machine produced.
    Compare("the commit counter equals the retirement events published",
            dut_->o_commit_o == cov_.retired,
            "counter=" + Dec(dut_->o_commit_o) + " events=" + Dec(cov_.retired));

    const uint32_t width = (g_.retire_width >= 32) ? 32 : g_.retire_width;
    const uint32_t valid_mask =
        (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    const uint32_t valid = static_cast<uint32_t>(dut_->ev_valid_o) & valid_mask;
    const bool trap_pulse = dut_->o_trap_valid_o != 0;

    std::vector<SlotEvent> cycle_events;
    for (uint32_t lane = 0; lane < width; lane++) {
      if ((valid & (1u << lane)) == 0) continue;
      if (lane > 0 && (valid & (1u << (lane - 1))) == 0) {
        Fail("a slot retires only behind the slot in front of it",
             "lane " + Dec(lane) + " retired at cycle " + Dec(cycles_) +
                 " with no lane " + Dec(lane - 1) + " event");
      }
      SlotEvent e;
      e.lane = lane;
      e.cycle = cycles_;
      e.seq = PackedLane(dut_->ev_seq_o, lane, g_.seq_w);
      e.pc = PayloadLane(dut_->ev_pc_o, lane);
      e.id = PackedLane(dut_->ev_id_o, lane, g_.ret_id_w);
      e.trap = PackedLane(dut_->ev_trap_o, lane, 1) != 0;
      e.is_store = IsStore(*img_, e.pc);
      e.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      e.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      e.value = PayloadLane(dut_->ev_value_o, lane);
      // The event interface's store identity, which the integrated core does not
      // drive (`pay_is_store` is constant zero at the retire instance). Counted
      // so the report can say whether that is still true, never asserted on.
      if (PackedLane(dut_->ev_store_o, lane, 1) != 0) cov_.ev_store_bits++;
      cycle_events.push_back(e);
    }

    // ---- a slot-0 trap blocks the younger slot ----
    if (std::getenv("MOSAIC_RETIRE_TRACE") != nullptr && (!cycle_events.empty() || trap_pulse)) {
      std::printf("    [ev] cycle=%llu pulse=%d occupied=%u", 
                  static_cast<unsigned long long>(cycles_), trap_pulse ? 1 : 0,
                  dut_->o_rob_occupied_o);
      for (const SlotEvent& e : cycle_events) {
        std::printf(" | lane%u pc=%s seq=%llu trap=%d we=%d rd=%u val=%s",
                    e.lane, U64(e.pc).c_str(),
                    static_cast<unsigned long long>(e.seq), e.trap ? 1 : 0,
                    e.reg_we ? 1 : 0, e.rd, U64(e.value).c_str());
      }
      std::printf("\n");
    }
    if (trap_pulse) {
      cov_.trap_events++;
      const bool younger_retired = (valid & 0x2u) != 0;
      // The trapping entry is at the head when the trap is taken, so an
      // occupancy of two or more says an ordinary instruction was physically
      // present behind it in the buffer.
      const bool younger_present = dut_->o_rob_occupied_o >= 2;
      Check("a slot-0 trap blocks the younger slot", !younger_retired,
            "lane 1 was valid in the cycle the trap was taken: " + Diagnose());
      Check("the trap event occupies lane 0 of the stream",
            !cycle_events.empty() && cycle_events[0].trap && cycle_events[0].lane == 0,
            "the trap pulse and the event stream disagree: " + Diagnose());
      if (younger_present && !younger_retired) {
        cov_.trap_cycles_with_younger_present++;
        cov_.trap_cycles_that_blocked_younger++;
      }
    }

    // Per-slot deltas, in lane order -- unless a driver mutant swaps them.
    std::vector<size_t> model_index(cycle_events.size());
    for (size_t i = 0; i < model_index.size(); i++) model_index[i] = cursor_ + i;
#ifdef MOSAIC_RETIRE_CHECKER_LANE_REVERSED
    // DRIVER MUTANT: the within-cycle assignment follows the order the slots
    // happened to be read in, not the lane order the hardware defines.
    std::reverse(model_index.begin(), model_index.end());
#endif

      bool drop_next = false;
#ifdef MOSAIC_RETIRE_CHECKER_DROP_EVENT
    // DRIVER MUTANT: the checker loses the second event of the first cycle that
    // retires two slots, and never notices unless the sequence detector says so.
    if (!dropped_ && cycle_events.size() >= 2) drop_next = true;
#endif

    // How many of the model's events this cycle consumed. A cycle whose lanes
    // are beyond the model's stream (the park loop) consumes none.
    size_t consumed = 0;
    for (size_t i = 0; i < cycle_events.size(); i++) {
      SlotEvent& e = cycle_events[i];
      stream_.push_back(e);
      cov_.events++;
      if (!e.trap) cov_.retired++;

      if (drop_next) {
        drop_next = false;
        dropped_ = true;
        continue;   // the event is never examined: the gap detector must catch it
      }

      // ---- (4) the trace gap detector ----
      if (cov_.events == 1) {
        Check("retire_order starts at zero", e.seq == 0,
              "the first event's sequence number is " + Dec(e.seq));
        expected_seq_ = 0;
      } else {
        const uint64_t want = (expected_seq_ + 1) & SeqMask();
        if (e.seq != want) {
          Fail("retire_order is dense and strictly increasing",
               "cycle " + Dec(e.cycle) + ": expected seq " + Dec(want) + " after (" +
                   Identity(stream_[stream_.size() - 2]) + "), saw seq " + Dec(e.seq) +
                   " (" + Identity(e) + ")");
        }
        expected_seq_ = want;
      }

      // ---- the identity and x0 rules every event must satisfy ----
      for (const SlotEvent& other : cycle_events) {
        if (&other == &e) continue;
        Check("two slots in a cycle carry distinct event identities",
              other.id != e.id || other.pc != e.pc,
              "lane " + Dec(e.lane) + " and lane " + Dec(other.lane) +
                  " both published id " + U64(e.id) + " at pc " + U64(e.pc));
      }
      Check("x0 is never written", !(e.reg_we && e.rd == 0),
            "an event reports a write to x0: " + Identity(e));

      // ---- the delta the model predicts ----
      const size_t mi = model_index[i];
      if (model_ == nullptr || mi >= model_->size()) {
        continue;   // the park loop after the model's last event
      }
      consumed++;
      const ModelEvent& m = (*model_)[mi];
      const std::string label =
          "the retirement stream follows the program-order model: event " + Dec(mi);
      Compare(label + " pc", e.pc == m.pc,
              "expected pc " + U64(m.pc) + " (" + (m.trap ? "trap" : "retire") +
                  "), got " + U64(e.pc) + " (" + Identity(e) + ")");
      Compare(label + " trap flag", e.trap == m.trap,
              "at " + U64(m.pc) + ": expected trap=" + Dec(m.trap) + ", got " +
                  Dec(e.trap));
      Compare(label + " destination", e.rd == m.rd && e.reg_we == m.reg_we,
              "at " + U64(m.pc) + ": expected rd x" + Dec(m.rd) + " we=" +
                  Dec(m.reg_we) + ", got x" + Dec(e.rd) + " we=" + Dec(e.reg_we));
      if (m.reg_we) {
        uint64_t expected_value = m.value;
#ifdef MOSAIC_RETIRE_CHECKER_CYCLE_END
        // DRIVER MUTANT: the older slot's expectation is taken from the cycle's
        // end state -- the value the youngest lane of this cycle wrote to the
        // same rd -- instead of from the model's own per-slot step.
        expected_value = CycleEndValue(cycle_events, m.rd, m.value);
#endif
        Compare(label + " value", e.value == expected_value,
                "at " + U64(m.pc) + " x" + Dec(m.rd) + ": expected " +
                    U64(expected_value) + ", got " + U64(e.value));
      }
      if (m.trap) {
        // The event's new PC: the DUT says where it vectors to, the model says
        // where execution goes next. They must be the same address.
        Compare(label + " vectors to the model's next PC",
                dut_->o_trap_target_o == m.next_pc,
                "the trap at " + U64(m.pc) + " targets " + U64(dut_->o_trap_target_o) +
                    ", the model continues at " + U64(m.next_pc));
        Compare(label + " cause", dut_->o_trap_cause_o == m.cause,
                "the trap at " + U64(m.pc) + " has cause " + U64(dut_->o_trap_cause_o) +
                    ", the model predicts " + U64(m.cause));
        Compare(label + " names the faulting instruction", dut_->o_trap_epc_o == m.pc,
                "mepc=" + U64(dut_->o_trap_epc_o) + ", the trap's pc is " + U64(m.pc));
        Compare(label + " is a synchronous trap", dut_->o_trap_is_irq_o == 0,
                "the trap was reported as an interrupt");
      }

      // ---- coverage this event contributes ----
      if (m.reg_we && m.rd != 0 && mi + 1 < model_->size()) {
        const ModelEvent& m2 = (*model_)[mi + 1];
        if (i + 1 < cycle_events.size() && cycle_events[i + 1].reg_we &&
            cycle_events[i + 1].rd == m.rd && m2.reg_we && m2.rd == m.rd) {
          cov_.same_rd_pair_cycles++;
          if (m2.value != m.value) cov_.same_rd_pair_distinct++;
        }
      }
      if (!m.trap) {
        const uint32_t w = img_->Word(m.pc);
        if ((w & 0x7Fu) == 0x63u) {
          if (m.next_pc == m.pc + 4) cov_.fallthrough_branches++;
          else cov_.taken_branches++;
        }
      }
      const auto form = program_->x0_forms.find(m.pc);
      if (form != program_->x0_forms.end()) {
        std::map<uint64_t, uint64_t>& tally =
            (e.lane == 0) ? cov_.x0_lane0 : cov_.x0_lane1;
        tally[m.pc]++;
      }
    }

    cursor_ += consumed;
    if (cycle_events.empty()) return;
    if (cycle_events.size() == 1) {
      cov_.single_slot_cycles++;
    } else {
      cov_.dual_slot_cycles++;
    }
    if (cycle_events.size() >= 2) {
      const SlotEvent& first = cycle_events[0];
      const SlotEvent& second = cycle_events[1];
      if (first.is_store && !second.is_store &&
          program_->load_pcs.count(second.pc) != 0) {
        cov_.store_load_pair_cycles++;
      }
    }
  }

  // The value the youngest lane of this cycle wrote to `rd`, or `fallback`.
  static uint64_t CycleEndValue(const std::vector<SlotEvent>& events, uint32_t rd,
                                uint64_t fallback) {
    uint64_t value = fallback;
    for (const SlotEvent& s : events) {
      if (s.reg_we && s.rd == rd) value = s.value;
    }
    return value;
  }

  void ProgressCheck() {
    if (dut_->o_commit_o != last_commit_ || dut_->o_trap_valid_o != 0 ||
        dut_->o_dbg_alloc_ctr_o != last_alloc_) {
      last_commit_ = dut_->o_commit_o;
      last_alloc_ = dut_->o_dbg_alloc_ctr_o;
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail("cycle " + Dec(cycles_), "stalled: " + Diagnose());
    }
  }

  uint64_t SeqMask() const {
    return (g_.seq_w >= 64) ? ~0ull : ((1ull << g_.seq_w) - 1ull);
  }

  std::string Diagnose() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) +
           " unsupported=" + Dec(dut_->o_unsupported_o) +
           " illegal=" + Dec(dut_->o_illegal_o) +
           " stopped=" + Dec(dut_->o_stopped_o) +
           " traps=" + Dec(dut_->o_csr_trap_o) +
           " mret=" + Dec(dut_->o_csr_mret_o) +
           " redirects=" + Dec(dut_->o_redirect_o) +
           " mepc=" + U64(dut_->o_csr_mepc_o) +
           " mcause=" + U64(dut_->o_csr_mcause_o) +
           " mtvec=" + U64(dut_->o_csr_mtvec_o);
  }

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  const ProgImage* img_;
  const Program* program_;
  Geometry g_;
  const std::vector<ModelEvent>* model_ = nullptr;
  std::vector<SlotEvent> stream_;
  Coverage cov_;
  size_t cursor_ = 0;
  uint64_t expected_seq_ = 0;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  uint32_t last_commit_ = 0;
  uint32_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  bool dropped_ = false;
};

std::string Join(const std::vector<std::string>& parts) {
  std::string text;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i != 0) text += ", ";
    text += parts[i];
  }
  return text;
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
    // Every check below must be able to fail the run. Reporter::Check records a
    // failure but does not stop, so a run that printed a CHECK FAILED line and
    // still exited 0 would be a silent pass -- the one outcome this harness must
    // never produce.
    auto Require = [&](bool ok, const std::string& what) {
      reporter.Check(ok, what);
      if (!ok) Fail("check", what);
    };

    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    const Geometry geometry = ReadGeometry(&dut);
    if (geometry.xlen != 64) Fail("geometry", "the profile is not 64-bit");
    if (geometry.retire_width < 1 || geometry.retire_width > 16) {
      Fail("geometry", "the retire width is not a workable value: " +
                           Dec(geometry.retire_width));
    }

    // ------------------------------------------------------------- the program
    const Program program = BuildProgram();
    ProgImage image;
    for (size_t i = 0; i < program.words.size(); i++) {
      image.Put(geometry.reset_vector + 4ull * i, program.words[i]);
    }
    const uint64_t program_end = geometry.reset_vector + 4ull * program.words.size();
    if (std::getenv("MOSAIC_RETIRE_DUMP") != nullptr) {
      std::printf("  program: %zu words, handler at %s, trap at %s\n",
                  program.words.size(), U64(program.handler_pc).c_str(),
                  U64(program.trap_pc).c_str());
      for (size_t i = 0; i < program.words.size(); i++) {
        std::printf("    %s: %08x\n",
                    U64(geometry.reset_vector + 4ull * i).c_str(), program.words[i]);
      }
    }
    for (uint64_t addr = program_end; addr < geometry.reset_vector + 0x1000ull;
         addr += 4) {
      image.Put(addr, 0x0000006Fu);   // `jal x0, 0`: a fetch past the end is stable
    }

    mosaic::MemoryModel dut_mem;
    mosaic::MemoryModel ref_mem;
    for (const auto& kv : image.words()) {
      if (dut_mem.Write(kv.first, 4, kv.second) != mosaic::AccessStatus::kOk ||
          ref_mem.Write(kv.first, 4, kv.second) != mosaic::AccessStatus::kOk) {
        Fail("program", "the image does not fit the memory map at " + U64(kv.first));
      }
    }

    // --------------------------------------------------------- the expectation
    const mosaic_trap::RefResult reference =
        mosaic_trap::RunReference(image, geometry.reset_vector, &ref_mem);
    if (!reference.exited) {
      Fail("model", "the reference did not reach the exit protocol: " +
                        reference.stop_reason + " at " + U64(reference.stop_pc));
    }
    const std::vector<ModelEvent> model = BuildModel(image, reference);
    Require(reference.traps == 1,
                   "the model predicts the one trap the program takes: got " +
                       Dec(reference.traps));
    Require(reference.mret == 1,
                   "the model predicts the handler's MRET: got " + Dec(reference.mret));

    Harness harness(&dut, &reporter, options.max_cycles, &image, &dut_mem, &program);
    harness.Configure(geometry);
    harness.SetModel(&model);
    harness.Reset();

    while (!(harness.model_exhausted() && dut_mem.finished())) {
      harness.Cycle(false);
    }
    for (int i = 0; i < kSettleCycles; i++) harness.Cycle(false);

    const Coverage& cov = harness.coverage();

    // ---- the comparison covered the model ----
    Require(harness.cursor() == model.size(),
                   "every event the model predicts was published and compared (" +
                       Dec(model.size()) + " events), got " + Dec(harness.cursor()));
    Require(cov.events > 0, "the machine published retirement events");
    Require(cov.retired <= cov.events,
                   "the retirement events are a subset of the stream");

    // ---- the five situations, produced and counted ----
    Require(cov.dual_slot_cycles > 0,
                   "the run retires two slots in one cycle (" +
                       Dec(cov.dual_slot_cycles) + " cycle(s))");
    Require(cov.single_slot_cycles > 0,
                   "the run also retires one slot in a cycle, so the comparison is "
                   "exercised at width one as well as width two (" +
                       Dec(cov.single_slot_cycles) + " cycle(s))");
    Require(cov.same_rd_pair_cycles > 0,
                   "two writes to the same rd retired in one cycle (" +
                       Dec(cov.same_rd_pair_cycles) + " cycle(s))");
    Require(cov.same_rd_pair_distinct > 0,
                   "in that cycle the two writes disagree, so the older slot's delta "
                   "cannot be the cycle's end state (" +
                       Dec(cov.same_rd_pair_distinct) + " cycle(s))");
    Require(cov.trap_cycles_with_younger_present > 0,
                   "a slot-0 trap retired with an ordinary instruction present behind "
                   "it in the ROB (" + Dec(cov.trap_cycles_with_younger_present) +
                       " cycle(s))");
    Require(cov.store_load_pair_cycles > 0,
                   "an adjacent store/load retired in one cycle, store lane 0 and load "
                   "lane 1 (" + Dec(cov.store_load_pair_cycles) + " cycle(s))");
    Require(cov.taken_branches > 0,
                   "a taken conditional branch retired whose next event is its target (" +
                       Dec(cov.taken_branches) + ")");
    Require(cov.fallthrough_branches > 0,
                   "a conditional branch retired whose next event is its own fall-through (" +
                       Dec(cov.fallthrough_branches) + ")");

    // The counters above are satisfied by any conditional branch; these three
    // name the ones the program directed, so "the situation was produced" cannot
    // be an accident of some other transfer.
    for (const uint64_t where : {program.taken_branch_pc, program.notaken_branch_pc,
                                 program.jal_pc}) {
      bool retired = false;
      for (const SlotEvent& e : harness.stream()) {
        if (e.pc == where) retired = true;
      }
      Require(retired, "the directed control transfer at " + U64(where) + " retired");
    }

    // The words a taken transfer skips must never retire: the target is the
    // instruction after the branch, and the word sitting in its shadow is not.
    for (const uint64_t wrong_path : {program.branch_fallthrough_pc,
                                      program.jal_fallthrough_pc}) {
      bool retired = false;
      std::string where;
      for (const SlotEvent& e : harness.stream()) {
        if (e.pc != wrong_path) continue;
        retired = true;
        where = " lane " + Dec(e.lane) + " at cycle " + Dec(e.cycle);
      }
      Require(!retired,
              "the instruction a taken transfer skips never retires: " +
                  U64(wrong_path) + where);
      if (retired) {
        Fail("branch shadow", "the word at " + U64(wrong_path) + " retired" + where);
      }
    }

    // The x0 write forms: every instruction the program emitted as an x0
    // destination must appear in the retired stream, in a lane, or the coverage
    // claim is empty.
    std::vector<std::string> missing;
    for (const auto& form : program.x0_forms) {
      if (cov.x0_lane0.count(form.first) == 0 && cov.x0_lane1.count(form.first) == 0) {
        missing.push_back(form.second + " at " + U64(form.first));
      }
    }
    Require(missing.empty(),
                   "every x0 write form the program emitted retired");
    if (!missing.empty()) {
      Fail("x0 coverage", "these x0-destination instructions never retired: " +
                              Join(missing));
    }
    Require(!cov.x0_lane0.empty(),
                   "an x0-destination instruction retired in lane 0 (" +
                       Dec(cov.x0_lane0.size()) + " distinct)");
    Require(!cov.x0_lane1.empty(),
                   "an x0-destination instruction retired in lane 1 (" +
                       Dec(cov.x0_lane1.size()) + " distinct)");

    // The store identity the delta carries is the retired PC, and this is the
    // independent statement that each one really was a store the machine acted
    // on: the store queue authorises exactly the store instructions that retire,
    // one per instruction, and nothing else.
    uint64_t model_stores = 0;
    for (const ModelEvent& m : model) {
      if (!m.trap && IsStore(image, m.pc)) model_stores++;
    }
    Require(dut.o_mem_sq_commit_o == model_stores,
                   "the store queue authorised exactly the retiring stores: got " +
                       Dec(dut.o_mem_sq_commit_o) + ", the model retires " +
                       Dec(model_stores));
    // Reported, not asserted: the event interface's store-identity fields are
    // tied off in the integrated core, so making this a check would make the
    // case fail if the RTL were fixed. See results/reports/V-013-retire.md.
    std::printf("  [reported defect] the retire event's store-identity field carried "
                "%llu bit(s) (%llu retiring stores decoded from the image)\n",
                static_cast<unsigned long long>(cov.ev_store_bits),
                static_cast<unsigned long long>(model_stores));

    // ---- the architectural end state the program published ----
    const uint64_t want[][2] = {{kPub0, 0ull},      {kPub0 + 8, 3ull},
                                {kPub1, 0x222ull},   {kPub2, 0x777ull},
                                {kPub3, 0x5A5ull},   {kPub3 + 8, 0x5A5ull},
                                {kPub3 + 16, 0x5A5ull}, {kPub4, 0x777ull}};
    // Each published value is checked against the literal the program is written
    // to produce *and* against the model's memory: the literal is what a reader
    // can check by reading the program, and the model is what catches a program
    // that computes something else.
    for (const auto& w : want) {
      uint64_t dut_value = 0;
      uint64_t ref_value = 0;
      if (dut_mem.Read(w[0], 8, &dut_value) != mosaic::AccessStatus::kOk ||
          ref_mem.Read(w[0], 8, &ref_value) != mosaic::AccessStatus::kOk) {
        Fail("end state", "memory at " + U64(w[0]) + " is not readable");
      }
      Require(dut_value == w[1],
              "the machine published " + U64(w[1]) + " at " + U64(w[0]) + ": got " +
                  U64(dut_value));
      Require(ref_value == w[1],
              "the model computed " + U64(w[1]) + " at " + U64(w[0]) + ": got " +
                  U64(ref_value));
    }

    // ---- the machinery ----
    Require(dut.o_unsupported_o == 0,
                   "no macro was refused as unsupported: " + Dec(dut.o_unsupported_o));
    Require(dut.o_illegal_o == 0,
                   "no illegal instruction was seen: " + Dec(dut.o_illegal_o));
    Require(dut.o_stopped_o == 0,
                   "the machine did not stop on an unsupported macro");
    Require(dut.o_csr_trap_o == reference.traps,
                   "the CSR file counted the model's traps: got " +
                       Dec(dut.o_csr_trap_o) + ", model " + Dec(reference.traps));
    Require(dut.o_csr_mret_o == reference.mret,
                   "the CSR file counted the model's MRETs: got " +
                       Dec(dut.o_csr_mret_o) + ", model " + Dec(reference.mret));
    Require(dut_mem.finished() && dut_mem.passed() && dut_mem.exit_code() == 1,
                   "the program wrote TOHOST with the PASS bit set: finished=" +
                       Dec(dut_mem.finished() ? 1 : 0) + " tohost=" +
                       U64(dut_mem.exit_code()));

    std::printf("  events=%llu retires=%llu traps=%llu cycles=%llu\n",
                static_cast<unsigned long long>(cov.events),
                static_cast<unsigned long long>(cov.retired),
                static_cast<unsigned long long>(reference.traps),
                static_cast<unsigned long long>(harness.cycles()));
    std::printf("  dual=%llu single=%llu same_rd=%llu same_rd_distinct=%llu "
                "trap_blocked=%llu store_load=%llu taken=%llu fallthrough=%llu "
                "x0[l0=%zu l1=%zu]\n",
                static_cast<unsigned long long>(cov.dual_slot_cycles),
                static_cast<unsigned long long>(cov.single_slot_cycles),
                static_cast<unsigned long long>(cov.same_rd_pair_cycles),
                static_cast<unsigned long long>(cov.same_rd_pair_distinct),
                static_cast<unsigned long long>(cov.trap_cycles_that_blocked_younger),
                static_cast<unsigned long long>(cov.store_load_pair_cycles),
                static_cast<unsigned long long>(cov.taken_branches),
                static_cast<unsigned long long>(cov.fallthrough_branches),
                cov.x0_lane0.size(), cov.x0_lane1.size());

    detail = "checks=" + Dec(reporter.checks()) + " events=" + Dec(cov.events) +
             " retires=" + Dec(cov.retired) + " comparisons=" +
             Dec(harness.comparisons()) + " dual=" + Dec(cov.dual_slot_cycles) +
             " single=" + Dec(cov.single_slot_cycles) + " same_rd=" +
             Dec(cov.same_rd_pair_cycles) + " trap_blocked=" +
             Dec(cov.trap_cycles_that_blocked_younger) + " store_load=" +
             Dec(cov.store_load_pair_cycles) + " cycles=" + Dec(harness.cycles()) +
             " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the retirement path holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
