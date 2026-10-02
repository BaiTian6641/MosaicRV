// ============================================================================
// tb_core_event.cpp -- CASE=core.event_payload, work package I-017.
//
// The DUT is the integrated p0 core with the retire unit (I-017), the CSR file
// (I-019), the system-instruction boundary and the trap controller wired in.
// The case exists because the frozen architectural event interface
// (`config/contracts/event_v1.json`, V-008) declares four groups of fields --
// identity, register, CSR and memory -- and the *memory* and *CSR* groups had
// no DUT-side producer through the integrated core: `ev_store`, `ev_store_size`
// and `ev_csr_*` were tied to zero in `mosaic_core.sv`, and a trap the system
// unit resolved published no event at all. V-013 reported all three (its
// results/reports/V-013-retire.md sections 6.1-6.3); this package fixes them
// and this case is the proof that the fields are real.
//
// ------------------------------------------------------------- what runs
//
// One hand-assembled program, from the profile's reset vector, that produces
// every shape the schema distinguishes:
//
//   * a handler at the reset vector, jumped over by word 0, installed in mtvec
//     by a `csrrw` (so the CSR *write* of the trap vector is itself an event);
//   * seven CSR writes on `mscratch` -- csrrw, csrrs, csrrc and the three
//     immediate forms -- plus two CSR instructions that do *not* write
//     (`csrrs rd, csr, x0`), which must publish no CSR payload at all;
//   * stores of all four sizes (sd/sw/sh/sb) with distinguishable data, one
//     64-bit datum built by a shift so a truncation to 32 bits cannot pass, and
//     a load between them that must carry no store payload;
//   * a dual-retire cycle with a store in lane 0 and a store in lane 1, so the
//     per-lane payloads have to be distinct rather than one snapshot;
//   * an `ecall`, which is a trap the *system unit* resolves: the trap entry
//     publishes cause/tval/epc on the core's trap ports and the event stream
//     must carry the same identity in the same cycle;
//   * the exit protocol: a store to MOSAIC_TOHOST of the pass code.
//
// --------------------------------------------------- where the expectation comes from
//
//   * THE PAYLOAD TABLES ARE DERIVED, NOT TRANSCRIBED. The assembler below
//     evaluates every instruction it emits as it emits it -- register values,
//     the store address (`rs1 + imm`) and data (`rs2`), the store size, and the
//     CSR file's own post-write value -- and records the architectural effect
//     of each store and each CSR write. The comparison therefore uses a second
//     implementation of the ISA's addressing and Zicsr rules, written from the
//     program's own text, not a copy of what the machine is expected to say.
//   * THE RETIREMENT STREAM has a third opinion: `sim/unit/trap_ref.h`, an
//     independent RV64IM_Zicsr interpreter with its own memory, executes the
//     same words and predicts every retirement (pc, rd, reg_we, value) and
//     every trap entry (cause, epc). The DUT's stream is compared with it in
//     order.
//   * THE END STATE closes the loop: the CSR file's `mscratch`/`mepc` read out
//     of the core must equal the values the assembler's CSR model computed, and
//     the data region of the platform memory must equal the reference's byte
//     for byte -- so the payloads the events claimed are the effects that
//     actually happened.
//
// The schema's *validity* rules are checked as rules, not inferred: a
// retirement that is not a store must carry `mem_is_store = 0` and zero in
// mem_addr/mem_data/mem_size; a retirement that is not a CSR write must carry
// `csr_write_valid = 0` and zero in csr_addr/csr_value; and a TRAP record must
// carry no register, CSR or store payload at all.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "mem_ref.h"
#include "memory_model.h"
#include "sim_common.h"
#include "trap_ref.h"

using mosaic_ref::DataMem;
using mosaic_trap::RefEvent;
using mosaic_trap::RefResult;

namespace {

constexpr int kResetCycles = 4;
// The program is under 60 instructions and the divide holds the head for ~64
// cycles; this bound is only reached if the machine stops making progress.
constexpr uint64_t kRunCycles = 20000;
// A stall is a defect with a location, not a timeout.
constexpr uint64_t kStallCycles = 4000;

// The store queue's 3-bit size encoding, from mosaic_pkg (SZ_BYTE/SZ_HALF/
// SZ_WORD/SZ_DBL). Repeated here because this driver has no RTL package; the
// schema freezes the same encoding (`mem_size`'s `valid_when`).
constexpr uint32_t kSzByte = 0;
constexpr uint32_t kSzHalf = 1;
constexpr uint32_t kSzWord = 2;
constexpr uint32_t kSzDbl = 3;

constexpr uint32_t kCsrMtvec = 0x305;
constexpr uint32_t kCsrMscratch = 0x340;
constexpr uint32_t kCsrMepc = 0x341;

constexpr uint64_t kExcEcallM = 11;

// The data region the program stores into: inside RAM, clear of the program
// text, the signature words and the test-harness addresses.
constexpr uint64_t kDataBase = 0x80000800;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

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
// The assembler, and the architectural model it carries
// ============================================================================
// Every instruction this file emits is also *evaluated*: the register file, the
// three CSRs the program touches, and the effect record of each store and each
// CSR write. The comparison tables are therefore a function of the program
// text, computed by this file, and not a transcription of an observed run.
//
// It is not a general assembler: only the forms the program uses exist, and an
// instruction whose result the model cannot compute marks its destination
// unknown, which makes a later store that reads it a build-time failure rather
// than a silently wrong expectation.
struct StoreExpect {
  uint64_t pc = 0;
  uint64_t addr = 0;
  uint64_t data = 0;
  uint32_t size = 0;
};

struct CsrExpect {
  uint64_t pc = 0;
  uint32_t addr = 0;
  uint64_t value = 0;   // the post-write value of the register
};

class Asm {
 public:
  explicit Asm(uint64_t base) : pc_(base) {
    for (int i = 0; i < 32; i++) {
      regs_[i] = 0;
      known_[i] = true;   // reset state: every register is zero, x0 stays zero
    }
    for (uint32_t addr = 0; addr < 0x1000; addr++) csr_[addr] = 0;
  }

  uint64_t pc() const { return pc_; }
  // Move the emission point. The handler is assembled *after* the main body --
  // it is evaluated with the trap entry applied, which needs the ecall's PC --
  // so it is placed at a fixed address rather than appended.
  void SetPc(uint64_t addr) { pc_ = addr; }
  const std::vector<uint32_t>& words() const { return words_; }
  const std::vector<uint64_t>& addresses() const { return addrs_; }

  // The trap entry's own rule for the register the handler reads: a synchronous
  // trap writes the trapping PC into mepc. Applying it to the model is what
  // lets the handler's CSR write be evaluated as the program will execute it.
  void ModelTrapEntry(uint64_t pc) { csr_[kCsrMepc] = pc & ~3ull; }

  // ------------------------------------------------------------- encoding
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
  static uint32_t J(int32_t imm, uint32_t rd) {
    const uint32_t u = static_cast<uint32_t>(imm);
    return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
           (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12) | (rd << 7) |
           0x6Fu;
  }

  // --------------------------------------------------- the tracked forms
  void Addi(uint32_t rd, uint32_t rs1, int32_t imm) {
    Emit(I(imm, rs1, 0, rd, 0x13));
    Set(rd, Value(rs1) + static_cast<uint64_t>(static_cast<int64_t>(imm)));
  }
  void Add(uint32_t rd, uint32_t rs1, uint32_t rs2) {
    Emit(R(0, rs2, rs1, 0, rd, 0x33));
    Set(rd, Value(rs1) + Value(rs2));
  }
  void Slli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(I(static_cast<int32_t>(sh), rs1, 1, rd, 0x13));
    Set(rd, Value(rs1) << sh);
  }
  void Srli(uint32_t rd, uint32_t rs1, uint32_t sh) {
    Emit(I(static_cast<int32_t>(sh), rs1, 5, rd, 0x13));
    Set(rd, Value(rs1) >> sh);
  }
  void Lui(uint32_t rd, uint32_t imm20) {
    Emit(U(imm20, rd, 0x37));
    Set(rd, static_cast<uint64_t>(static_cast<int64_t>(
                static_cast<int32_t>(imm20 << 12))));
  }
  void Jal(uint32_t rd, int32_t imm) {
    Emit(J(imm, rd));
    if (rd != 0) Set(rd, pc());   // the link value is the instruction's PC + 4
  }
  void Div(uint32_t rd, uint32_t rs1, uint32_t rs2) {
    Emit(R(0x01, rs2, rs1, 0x4, rd, 0x33));
    Unknown(rd);                          // a divide result is not modelled
  }
  void Ld(uint32_t rd, uint32_t rs1, int32_t imm) {
    Emit(I(imm, rs1, 3, rd, 0x03));
    Unknown(rd);
  }
  // `lwu`: LOAD funct3 110, the zero-extending word load. It is in this file
  // because the decoder refusing it was one of the three defects this package
  // fixes: a decoder that accepts the encoding but a load path that
  // sign-extends it would still be wrong, and the value this instruction
  // retires with is what says which.
  void Lwu(uint32_t rd, uint32_t rs1, int32_t imm) {
    Emit(I(imm, rs1, 6, rd, 0x03));
    Unknown(rd);
  }
  void Mret() { Emit(0x30200073u); }
  void Ecall() { Emit(0x00000073u); }

  // `li rd, value` for any 64-bit value: the standard lui/addi pair, widened by
  // two shifts when the value does not fit a sign-extended 32-bit immediate,
  // and split at bit 32 when it does not fit 32 bits at all. p0's addresses have
  // bit 31 set, so the zero-extension is what keeps `li` from producing
  // 0xffffffff80000800 -- the same rule sim/unit/tb_core_trap.cpp documents.
  void Li(uint32_t rd, uint64_t value) {
    if (value < 0x800ull) {
      Addi(rd, 0, static_cast<int32_t>(value));
      return;
    }
    if (value <= 0xFFFFFFFFull) {
      Li32(rd, static_cast<uint32_t>(value));
      return;
    }
    Li32(rd, static_cast<uint32_t>(value >> 32));
    Slli(rd, rd, 32);
    Li32(kScratch, static_cast<uint32_t>(value));
    Add(rd, rd, kScratch);
  }

  // ------------------------------------------------------ stores and CSRs
  void Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { Store(rs2, rs1, imm, 3, kSzDbl); }
  void Sw(uint32_t rs2, uint32_t rs1, int32_t imm) { Store(rs2, rs1, imm, 2, kSzWord); }
  void Sh(uint32_t rs2, uint32_t rs1, int32_t imm) { Store(rs2, rs1, imm, 1, kSzHalf); }
  void Sb(uint32_t rs2, uint32_t rs1, int32_t imm) { Store(rs2, rs1, imm, 0, kSzByte); }

  // The six Zicsr register/immediate forms. `writes` is the decoder's rule
  // (mosaic_decoder.sv): csrrw always writes, csrrs/csrrc write only when the
  // operand field is non-zero. A form that does not write records nothing --
  // and the case checks that the machine publishes no CSR payload for it.
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 1, rd, 0x73));
    CsrEffect(1, csr, Value(rs1), rd);
  }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 2, rd, 0x73));
    CsrEffect(2, csr, Value(rs1), rd);
  }
  void Csrrc(uint32_t rd, uint32_t csr, uint32_t rs1) {
    Emit(I(static_cast<int32_t>(csr), rs1, 3, rd, 0x73));
    CsrEffect(3, csr, Value(rs1), rd);
  }
  void Csrrwi(uint32_t rd, uint32_t csr, uint32_t zimm) {
    Emit(I(static_cast<int32_t>(csr), zimm & 0x1Fu, 5, rd, 0x73));
    CsrEffect(1, csr, zimm & 0x1Fu, rd);
  }
  void Csrrsi(uint32_t rd, uint32_t csr, uint32_t zimm) {
    Emit(I(static_cast<int32_t>(csr), zimm & 0x1Fu, 6, rd, 0x73));
    CsrEffect(2, csr, zimm & 0x1Fu, rd);
  }
  void Csrrci(uint32_t rd, uint32_t csr, uint32_t zimm) {
    Emit(I(static_cast<int32_t>(csr), zimm & 0x1Fu, 7, rd, 0x73));
    CsrEffect(3, csr, zimm & 0x1Fu, rd);
  }

  // -------------------------------------------------------------- results
  void Mark(const std::string& name) { labels_[name] = pc(); }
  uint64_t Label(const std::string& name) const {
    auto it = labels_.find(name);
    if (it == labels_.end()) Fail("assembler", "no such label: " + name);
    return it->second;
  }
  const std::vector<StoreExpect>& stores() const { return stores_; }
  const std::vector<CsrExpect>& csr_writes() const { return csr_writes_; }
  uint64_t csr_value(uint32_t addr) const { return csr_[addr]; }


 private:
  static constexpr uint32_t kScratch = 31;

  void Emit(uint32_t w) {
    addrs_.push_back(pc_);
    words_.push_back(w);
    pc_ += 4;
  }

  uint64_t Value(uint32_t reg) const {
    if (!known_[reg]) {
      Fail("assembler", "register x" + Dec(reg) +
                            " is used as an operand after its value became "
                            "unknown, so the expectation cannot be derived");
    }
    return regs_[reg];
  }
  void Set(uint32_t reg, uint64_t value) {
    regs_[reg] = value;
    known_[reg] = true;
    if (reg == 0) regs_[0] = 0;
  }
  void Unknown(uint32_t reg) {
    if (reg == 0) return;   // x0 is zero and stays zero
    known_[reg] = false;
    regs_[reg] = 0;
  }

  // A 32-bit constant, zero-extended into the 64-bit register.
  void Li32(uint32_t rd, uint32_t value) {
    if (value < 0x800u) {
      Addi(rd, 0, static_cast<int32_t>(value));
      return;
    }
    const uint32_t lo = value & 0xFFFu;
    const uint32_t hi = (value + 0x800u) >> 12;
    Lui(rd, hi);
    Addi(rd, rd, static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0));
    Slli(rd, rd, 32);
    Srli(rd, rd, 32);
    Set(rd, value);
  }

  void Store(uint32_t rs2, uint32_t rs1, int32_t imm, uint32_t f3, uint32_t size) {
    const uint64_t here = pc();
    Emit(S(imm, rs2, rs1, f3, 0x23));
    StoreExpect e;
    e.pc = here;
    e.addr = Value(rs1) + static_cast<uint64_t>(static_cast<int64_t>(imm));
    e.data = Value(rs2);
    e.size = size;
    stores_.push_back(e);
  }

  void CsrEffect(uint32_t op, uint32_t csr, uint64_t operand, uint32_t rd) {
    const uint64_t here = pc() - 4;
    const bool writes = (op == 1u) || (operand != 0u);
    const bool reads = (op != 1u) || (rd != 0u);
    if (writes) {
      uint64_t next = csr_[csr];
      if (op == 1u) next = operand;
      else if (op == 2u) next = csr_[csr] | operand;
      else next = csr_[csr] & ~operand;
      csr_[csr] = Canonical(csr, next);
      CsrExpect e;
      e.pc = here;
      e.addr = csr;
      e.value = csr_[csr];
      csr_writes_.push_back(e);
    }
    if (reads) Set(rd, csr_[csr]);
    else Unknown(rd);
  }

  // The two WARL rules the profile declares for the registers this program
  // touches: mtvec's mode field is canonicalised (reserved encodings become
  // Direct), mepc's low two bits are read-only zero.
  static uint64_t Canonical(uint32_t addr, uint64_t value) {
    if (addr == kCsrMtvec) {
      const uint64_t mode = value & 3ull;
      return (value & ~3ull) | (mode <= 1ull ? mode : 0ull);
    }
    if (addr == kCsrMepc) return value & ~3ull;
    return value;
  }

  uint64_t pc_;
  std::vector<uint32_t> words_;
  std::vector<uint64_t> addrs_;
  std::map<std::string, uint64_t> labels_;
  uint64_t regs_[32] = {};
  bool known_[32] = {};
  uint64_t csr_[0x1000] = {};
  std::vector<StoreExpect> stores_;
  std::vector<CsrExpect> csr_writes_;
};

// ============================================================================
// The program
// ============================================================================
// The handler sits at a fixed address well clear of the main body, so `mtvec`
// is a constant this file knows before it assembles the program that writes it,
// and the handler itself can be assembled *last* -- after the ecall's PC exists,
// with the trap entry applied to the model, so the CSR write it performs is
// evaluated as the program will execute it rather than as it was emitted.
constexpr uint64_t kHandlerOffset = 0x200;

struct DirectedProgram {
  std::vector<uint64_t> addrs;
  std::vector<uint32_t> words;
  uint64_t handler_pc = 0;
  uint64_t ecall_pc = 0;
  uint64_t exit_pc = 0;
  uint64_t park_pc = 0;
  uint64_t lwu_pc = 0;
  std::vector<StoreExpect> stores;
  std::vector<CsrExpect> csr_writes;
  uint64_t mscratch_final = 0;
  uint64_t mepc_final = 0;
  uint64_t mtvec_final = 0;
};

DirectedProgram BuildProgram(uint64_t reset) {
  DirectedProgram p;
  Asm a(reset);
  const uint64_t handler_pc = reset + kHandlerOffset;

  // ------------------------------------------------------------- main body
  a.Li(6, handler_pc);          // t1 = the handler address
  a.Csrrw(0, kCsrMtvec, 6);     // mtvec = t1
  a.Li(7, 0x1234);
  a.Csrrw(0, kCsrMscratch, 7);  // mscratch = 0x1234
  a.Li(28, 0x00F0);
  a.Csrrs(0, kCsrMscratch, 28); // mscratch = 0x12F4
  a.Li(29, 0x0030);
  a.Csrrc(0, kCsrMscratch, 29); // mscratch = 0x12C4
  a.Csrrwi(0, kCsrMscratch, 7); // mscratch = 7
  a.Csrrsi(0, kCsrMscratch, 0x18);  // mscratch = 0x1F
  a.Csrrci(0, kCsrMscratch, 0x01);  // mscratch = 0x1E
  a.Csrrs(5, kCsrMscratch, 0);  // t0 = mscratch      (a CSR read, no write)

  a.Li(8, kDataBase);           // s0 = the data region
  a.Li(10, 0xDEADBEEFu);
  a.Sd(10, 8, 0);               // 8 bytes at +0
  a.Sw(10, 8, 8);               // 4 bytes at +8
  a.Sh(10, 8, 16);              // 2 bytes at +16
  a.Sb(10, 8, 24);              // 1 byte  at +24
  a.Ld(11, 8, 0);               // a load: must carry no store payload
  a.Li(12, 0x00FFu);
  a.Sd(12, 8, 40);              // 8 bytes at +40, a different datum
  // A 64-bit datum built by a shift: a payload truncated to 32 bits cannot
  // report this value.
  a.Li(13, 1);
  a.Slli(13, 13, 63);
  a.Addi(13, 13, -1);           // a3 = 0x7FFFFFFFFFFFFFFF
  a.Sd(13, 8, 64);              // 8 bytes at +64
  // LWU over a word whose bit 31 is set: the value must come back zero-extended
  // (0x00000000DEADBEEF), so a load path that sign-extends it fails here.
  a.Mark("lwu");
  a.Lwu(19, 8, 8);              // x19 = lwu 8(s0)
  p.lwu_pc = a.Label("lwu");

  // A divide holds the head for ~64 cycles, during which the two stores behind
  // it complete; one filler instruction takes the divide's second lane, so the
  // next cycle presents the two stores as lanes 0 and 1 -- the dual-retire
  // shape in which the two payloads have to be distinct. Their operands are set
  // up *before* the divide, so both stores are allocated and complete while the
  // divide is still the head: a store whose base or datum was still being
  // computed would enter the reorder buffer late and miss the pair.
  a.Li(14, 0xABCDu);
  a.Li(15, 0x1234u);
  a.Li(30, 1);
  a.Li(20, 3);
  a.Div(30, 30, 20);
  a.Addi(29, 0, 0x111);         // the filler
  a.Sh(14, 8, 48);              // 2 bytes at +48  (lane 0 of the pair)
  a.Sb(15, 8, 56);              // 1 byte  at +56  (lane 1 of the pair)

  a.Mark("ecall");
  a.Ecall();                    // the system-unit trap
  p.ecall_pc = a.Label("ecall");

  a.Li(16, 1);                  // a6 = the pass code
  a.Li(17, MOSAIC_TOHOST);
  a.Sd(16, 17, 0);              // the exit store
  p.exit_pc = a.pc() - 4;
  a.Mark("park");
  a.Jal(0, 0);                  // park: fetch stays in the image
  p.park_pc = a.Label("park");

  // ------------------------------------------------------------- the handler
  // Assembled at its fixed address, with the trap entry already applied to the
  // model: `csrrs t0, mepc, x0` reads the trapping PC, and the write that
  // follows is the value the event must report.
  a.ModelTrapEntry(p.ecall_pc);
  a.SetPc(handler_pc);
  a.Mark("handler");
  a.Csrrs(5, kCsrMepc, 0);      // t0 = mepc          (a CSR read, no write)
  a.Addi(5, 5, 4);              // t0 = mepc + 4
  a.Csrrw(0, kCsrMepc, 5);      // mepc = t0          (CSR write)
  a.Mret();
  p.handler_pc = a.Label("handler");

  p.addrs = a.addresses();
  p.words = a.words();
  p.stores = a.stores();
  // Execution order is emission order: the main body first, the handler last.
  p.csr_writes = a.csr_writes();
  p.mscratch_final = a.csr_value(kCsrMscratch);
  p.mepc_final = a.csr_value(kCsrMepc);
  p.mtvec_final = a.csr_value(kCsrMtvec);
  // The handler's write of mepc is the trap's epc + 4, which is the value the
  // event must report for that write.
  return p;
}

// ============================================================================
// The instruction image
// ============================================================================
class ProgImage {
 public:
  void Put(uint64_t addr, uint32_t word) { words_[addr] = word; }
  uint32_t Word(uint64_t addr) const {
    auto it = words_.find(addr);
    // A fetch that runs past the program lands on a self-loop rather than on a
    // don't-care: `jal x0, 0` can neither fault nor stop the machine.
    return (it == words_.end()) ? 0x0000006fu : it->second;
  }
  bool Has(uint64_t addr) const { return words_.find(addr) != words_.end(); }
  const std::map<uint64_t, uint32_t>& words() const { return words_; }

 private:
  std::map<uint64_t, uint32_t> words_;
};

// ============================================================================
// The instruction memory (one cycle of latency, in order)
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
    inflight_.push_back(Entry{Request{addr, id, epoch}, 1});
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

// ============================================================================
// One event record, read from the DUT's own ports
// ============================================================================
struct EvRecord {
  uint64_t cycle = 0;
  uint32_t lane = 0;
  bool trap = false;
  uint64_t seq = 0;
  uint64_t pc = 0;
  uint64_t id = 0;
  bool reg_we = false;
  uint32_t rd = 0;
  uint64_t value = 0;
  bool store = false;
  uint64_t store_addr = 0;
  uint64_t store_data = 0;
  uint32_t store_size = 0;
  bool csr_we = false;
  uint32_t csr_addr = 0;
  uint64_t csr_value = 0;
  uint64_t trap_cause = 0;
  uint64_t trap_tval = 0;
};

// ============================================================================
// The harness
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem,
          const DirectedProgram* program)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(mem), mem_(mem), img_(img), program_(program) {}

  void Configure(const Geometry& g) { g_ = g; }
  void Phase(const std::string& name) { phase_ = name; }
  void Expect(const RefResult* ref) { ref_ = ref; }
  void StopComparing() { compare_ = false; }

  // The interpreter's trace interleaves retirements and trap entries; the
  // retirements are compared here, the trap entries against the trap pulse.
  void AddRefRetire(const RefEvent& e) { ref_retires_.push_back(e); }

  uint64_t cycles() const { return cycles_; }
  uint64_t checks() const { return checks_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t retires() const { return retires_; }
  uint64_t trap_events() const { return trap_events_; }
  uint64_t store_events() const { return store_events_; }
  uint64_t csr_events() const { return csr_events_; }
  uint64_t dual_store_cycles() const { return dual_store_cycles_; }
  uint64_t post_exit_retires() const { return post_exit_retires_; }
  size_t ref_retires_consumed() const { return ref_retires_consumed_; }
  size_t ref_retires_total() const { return ref_retires_.size(); }
  const std::vector<EvRecord>& records() const { return records_; }
  const std::vector<EvRecord>& trap_records() const { return trap_records_; }
  const std::vector<EvRecord>& store_records() const { return store_records_; }
  const std::vector<EvRecord>& csr_records() const { return csr_records_; }

  // ------------------------------------------------------------- checks
  void Check(const std::string& what, bool ok, const std::string& detail) {
    checks_++;
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  bool Complete() const {
    if (ref_ == nullptr) return dut_->o_stopped_o != 0;
    return ref_retires_consumed_ >= ref_retires_.size() &&
           ref_traps_consumed_ >= ref_->trap_causes.size();
  }
  bool Finished() const { return mem_->finished(); }
  uint64_t exit_code() const { return mem_->exit_code(); }
  bool Passed() const { return mem_->passed(); }

  // ------------------------------------------------------------ stepping
  void Reset(int cycles) {
    dut_->rst = 1;
    for (int i = 0; i < cycles; i++) Cycle(true);
    dut_->rst = 0;
    imem_.Reset();
    dmem_.Reset();
    cycles_ = 0;
  }

  void Cycle(bool rst) {
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = 0;
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

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o,
                   dut_->imem_req_epoch_o);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem_.PopResponse();
    }
    imem_.Advance();

    if (bus_reset_.MayAccept(rst, (dut_->dmem_req_valid_o != 0) &&
                                      (dut_->dmem_req_ready_i != 0))) {
      DataMem::Request r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = dut_->dmem_req_wstrb_o;
      r.wdata = dut_->dmem_req_wdata_o;
      dmem_.Accept(r, cycles_);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
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
           " events=" + Dec(records_.size()) + " traps=" + Dec(trap_events_) +
           " mcause=" + U64(dut_->o_csr_mcause_o) + " mepc=" + U64(dut_->o_csr_mepc_o);
  }

 private:
  // ------------------------------------------------------ the observation
  void Observe() {
    ObserveTrapEntry();
    ObserveEvents();
    ProgressCheck();
  }

  // The trap entry the core presents this cycle: cause, tval, epc and target.
  // The event record for a trap is compared against *this*, not against a
  // re-derivation, because the schema's trap_epc rule makes the event's
  // pc_before the trap's epc for a synchronous trap and the entry is the
  // authority for the other two.
  void ObserveTrapEntry() {
    if (dut_->o_trap_valid_o == 0) return;
    trap_entry_valid_ = true;
    trap_entry_cause_ = dut_->o_trap_cause_o;
    trap_entry_tval_ = dut_->o_trap_tval_o;
    trap_entry_epc_ = dut_->o_trap_epc_o;
    trap_entry_irq_ = dut_->o_trap_is_irq_o != 0;
    if (trap_entry_irq_) {
      Compare("this case drives no interrupt, so no trap is asynchronous",
              false, "an asynchronous trap was taken");
    }
    if (ref_ != nullptr && ref_traps_consumed_ < ref_->trap_causes.size()) {
      const uint64_t want_cause = ref_->trap_causes[ref_traps_consumed_];
      const uint64_t want_epc = ref_->trap_epcs[ref_traps_consumed_];
      Compare("trap entry " + Dec(ref_traps_consumed_) +
                  " matches the independent interpreter's cause",
              trap_entry_cause_ == want_cause,
              "cause=" + U64(trap_entry_cause_) + " expected " + U64(want_cause));
      Compare("trap entry " + Dec(ref_traps_consumed_) +
                  " names the faulting instruction",
              trap_entry_epc_ == want_epc,
              "epc=" + U64(trap_entry_epc_) + " expected " + U64(want_epc));
      ref_traps_consumed_++;
    }
  }

  void ObserveEvents() {
    const uint32_t mask = (g_.retire_width >= 32)
                              ? 0xFFFFFFFFu
                              : ((1u << g_.retire_width) - 1u);
    const uint32_t valid = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    if (valid == 0) return;

    // The lane rule: lane i only exists on top of lane i-1.
    if (g_.retire_width >= 2) {
      Compare("lane 1 is never published without lane 0",
              ((valid & 2u) == 0) || ((valid & 1u) != 0),
              "ev_valid=" + Dec(valid));
    }

    uint32_t lanes = 0;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((valid & (1u << lane)) == 0) continue;
      lanes++;
    }

    uint32_t store_lanes_this_cycle = 0;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((valid & (1u << lane)) == 0) continue;
      EvRecord e;
      e.cycle = cycles_;
      e.lane = lane;
      e.trap = PackedLane(dut_->ev_trap_o, lane, 1) != 0;
      e.seq = PackedLane(dut_->ev_seq_o, lane, g_.seq_w);
      e.pc = PayloadLane(dut_->ev_pc_o, lane);
      e.id = PackedLane(static_cast<uint64_t>(dut_->ev_id_o), lane, 2 * g_.seq_w);
      e.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      e.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      e.value = PayloadLane(dut_->ev_value_o, lane);
      e.store = PackedLane(dut_->ev_store_o, lane, 1) != 0;
      e.store_addr = PayloadLane(dut_->ev_store_addr_o, lane);
      e.store_data = PayloadLane(dut_->ev_store_data_o, lane);
      e.store_size =
          static_cast<uint32_t>(PackedLane(dut_->ev_store_size_o, lane, 3));
      e.csr_we = PackedLane(dut_->ev_csr_we_o, lane, 1) != 0;
      e.csr_addr = static_cast<uint32_t>(PackedLane(dut_->ev_csr_addr_o, lane, 12));
      e.csr_value = PayloadLane(dut_->ev_csr_value_o, lane);
      e.trap_cause = PayloadLane(dut_->ev_trap_cause_o, lane);
      e.trap_tval = PayloadLane(dut_->ev_trap_tval_o, lane);
      records_.push_back(e);

      // ---- identity: the sequence number is dense and lane i is counter+i
      const uint64_t modulus = (g_.seq_w >= 64) ? 0ull : (1ull << g_.seq_w);
      const uint64_t want_seq =
          (modulus == 0) ? (seq_expected_ + lane)
                         : ((seq_expected_ + lane) & (modulus - 1ull));
      Compare("event " + Dec(records_.size() - 1) +
                  " carries the dense retirement order",
              e.seq == want_seq,
              "pc=" + U64(e.pc) + " lane=" + Dec(lane) + " seq=" + Dec(e.seq) +
                  " expected " + Dec(want_seq) + " at cycle " + Dec(cycles_));

      if (e.trap) {
        CheckTrapRecord(e);
        continue;
      }
      if (e.store) store_lanes_this_cycle++;
      CheckRetireRecord(e);
    }
    seq_expected_ += lanes;
    if (store_lanes_this_cycle == 2) dual_store_cycles_++;
  }

  // A TRAP record: the schema's trap_rule -- no register, no CSR and no store
  // payload, even though the payload bus may have been carrying one -- plus the
  // cause/tval/epc identity the trap entry published.
  void CheckTrapRecord(const EvRecord& e) {
    trap_events_++;
    trap_records_.push_back(e);
    Compare("the trap event is lane 0", e.lane == 0,
            "lane=" + Dec(e.lane));
    Compare("the trap event carries the trap entry's cause in the same cycle",
            trap_entry_valid_ && e.trap_cause == trap_entry_cause_,
            "event cause=" + U64(e.trap_cause) + " entry cause=" +
                U64(trap_entry_cause_) + " (entry valid=" +
                Dec(trap_entry_valid_ ? 1 : 0) + ")");
    Compare("the trap event carries the trap entry's tval in the same cycle",
            trap_entry_valid_ && e.trap_tval == trap_entry_tval_,
            "event tval=" + U64(e.trap_tval) + " entry tval=" +
                U64(trap_entry_tval_));
    Compare("the trap event's pc_before is the trap entry's epc",
            trap_entry_valid_ && e.pc == trap_entry_epc_,
            "event pc=" + U64(e.pc) + " entry epc=" + U64(trap_entry_epc_));
    Compare("a TRAP record carries no register write",
            !e.reg_we && e.rd == 0,
            "reg_we=" + Dec(e.reg_we ? 1 : 0) + " rd=x" + Dec(e.rd));
    Compare("a TRAP record carries no CSR payload",
            !e.csr_we && e.csr_addr == 0 && e.csr_value == 0,
            "csr_we=" + Dec(e.csr_we ? 1 : 0) + " addr=" + U64(e.csr_addr) +
                " value=" + U64(e.csr_value));
    Compare("a TRAP record carries no store payload",
            !e.store && e.store_addr == 0 && e.store_data == 0 && e.store_size == 0,
            "store=" + Dec(e.store ? 1 : 0) + " addr=" + U64(e.store_addr) +
                " data=" + U64(e.store_data) + " size=" + Dec(e.store_size));
    Compare("the trap event's cause is the ISA's ecall-from-M",
            e.trap_cause == kExcEcallM, "cause=" + U64(e.trap_cause));
    Compare("the trap event's pc_before is the ecall",
            e.pc == program_->ecall_pc,
            "pc=" + U64(e.pc) + " expected " + U64(program_->ecall_pc));
  }

  // A RETIRE record: the register group, plus the two payload groups with their
  // validity rules. Which payload a retirement owes is decided from the
  // instruction word at its committed PC -- read from the same image the
  // machine is fed, from the ISA text.
  void CheckRetireRecord(const EvRecord& e) {
    retires_++;
    const uint32_t word = img_->Word(e.pc);
    const bool is_store = (word & 0x7Fu) == 0x23u;
    const bool is_csr = (word & 0x7Fu) == 0x73u && ((word >> 12) & 0x7u) != 0u;
    const uint32_t csr_f3 = (word >> 12) & 0x7u;
    const uint32_t csr_rs1 = (word >> 15) & 0x1Fu;
    const bool csr_writes =
        is_csr && (((csr_f3 & 0x3u) == 1u) || (csr_rs1 != 0u));

    if (compare_ && ref_ != nullptr && ref_retires_consumed_ < ref_retires_.size()) {
      const RefEvent& want = ref_retires_[ref_retires_consumed_];
      Compare("retire " + Dec(ref_retires_consumed_) +
                  " is the independent interpreter's next retirement",
              e.pc == want.pc,
              "pc=" + U64(e.pc) + " expected " + U64(want.pc));
      Compare("retire " + Dec(ref_retires_consumed_) + " destination",
              e.rd == want.rd && e.reg_we == want.reg_we,
              "at " + U64(want.pc) + " rd expected x" + Dec(want.rd) + " we=" +
                  Dec(want.reg_we ? 1 : 0) + ", got x" + Dec(e.rd) + " we=" +
                  Dec(e.reg_we ? 1 : 0));
      if (want.reg_we) {
        Compare("retire " + Dec(ref_retires_consumed_) + " value",
                e.value == want.value,
                "at " + U64(want.pc) + " x" + Dec(want.rd) + " expected " +
                    U64(want.value) + ", got " + U64(e.value));
      }
      ref_retires_consumed_++;
    } else {
      post_exit_retires_++;
      Compare("a retirement after the interpreter's trace is the park loop",
              e.pc == program_->park_pc,
              "pc=" + U64(e.pc) + " expected the park at " + U64(program_->park_pc));
    }

    // ---- the memory group
    if (is_store) {
      store_events_++;
      store_records_.push_back(e);
      Compare("the store payload is present exactly for a store instruction",
              e.store, "pc=" + U64(e.pc) + " ev_store=0");
      if (store_index_ < program_->stores.size()) {
        const StoreExpect& want = program_->stores[store_index_];
        Compare("store event " + Dec(store_index_) + " is the program's store",
                e.pc == want.pc,
                "pc=" + U64(e.pc) + " expected " + U64(want.pc));
        Compare("store event " + Dec(store_index_) + " carries mem_addr",
                e.store_addr == want.addr,
                "pc=" + U64(e.pc) + " addr=" + U64(e.store_addr) + " expected " +
                    U64(want.addr));
        Compare("store event " + Dec(store_index_) + " carries mem_size",
                e.store_size == want.size,
                "pc=" + U64(e.pc) + " size=" + Dec(e.store_size) + " expected " +
                    Dec(want.size));
        // mem_data's meaning is "the low mem_size bytes are the value", so the
        // comparison is byte-exact over exactly those bytes.
        const uint64_t bytes = 1ull << want.size;
        const uint64_t mask = (bytes >= 8) ? ~0ull : ((1ull << (8 * bytes)) - 1ull);
        Compare("store event " + Dec(store_index_) +
                    " carries mem_data byte-exactly",
                (e.store_data & mask) == (want.data & mask),
                "pc=" + U64(e.pc) + " data=" + U64(e.store_data) + " expected " +
                    U64(want.data) + " over " + Dec(bytes) + " byte(s)");
        store_index_++;
      } else {
        Fail(phase_, "a store retired that the program did not assemble: pc " +
                         U64(e.pc));
      }
    } else {
      Compare("a non-store retirement carries no store payload",
              !e.store && e.store_addr == 0 && e.store_data == 0 &&
                  e.store_size == 0,
              "pc=" + U64(e.pc) + " store=" + Dec(e.store ? 1 : 0) + " addr=" +
                  U64(e.store_addr) + " data=" + U64(e.store_data) + " size=" +
                  Dec(e.store_size));
    }

    // ---- the CSR group
    if (csr_writes) {
      csr_events_++;
      csr_records_.push_back(e);
      Compare("the CSR payload is present exactly for a CSR instruction that writes",
              e.csr_we, "pc=" + U64(e.pc) + " ev_csr_we=0");
      if (csr_index_ < program_->csr_writes.size()) {
        const CsrExpect& want = program_->csr_writes[csr_index_];
        Compare("CSR event " + Dec(csr_index_) + " is the program's CSR write",
                e.pc == want.pc,
                "pc=" + U64(e.pc) + " expected " + U64(want.pc));
        Compare("CSR event " + Dec(csr_index_) + " carries csr_addr",
                e.csr_addr == want.addr,
                "pc=" + U64(e.pc) + " addr=" + U64(e.csr_addr) + " expected " +
                    U64(want.addr));
        Compare("CSR event " + Dec(csr_index_) + " carries csr_value",
                e.csr_value == want.value,
                "pc=" + U64(e.pc) + " value=" + U64(e.csr_value) + " expected " +
                    U64(want.value));
        csr_index_++;
      } else {
        Fail(phase_, "a CSR write retired that the program did not assemble: pc " +
                         U64(e.pc));
      }
    } else {
      Compare("a non-CSR retirement carries no CSR payload",
              !e.csr_we && e.csr_addr == 0 && e.csr_value == 0,
              "pc=" + U64(e.pc) + " csr_we=" + Dec(e.csr_we ? 1 : 0) + " addr=" +
                  U64(e.csr_addr) + " value=" + U64(e.csr_value));
    }
    (void)is_csr;
  }

  void ProgressCheck() {
    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_trap_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_) ||
                            (dut_->ev_valid_o != 0);
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

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  // The reset-traffic rule (V-010, sim/common/bus_reset_gate.h): while reset
  // is asserted this model accepts nothing, so no reset-time response can be
  // queued ahead of a fresh post-reset one.
  mosaic::BusResetGate bus_reset_;
  mosaic::MemoryModel* mem_;
  Geometry g_;
  std::string phase_;
  const ProgImage* img_;
  const DirectedProgram* program_;
  const RefResult* ref_ = nullptr;
  bool compare_ = true;
  uint64_t cycles_ = 0;
  uint64_t checks_ = 0;
  uint64_t comparisons_ = 0;
  uint64_t retires_ = 0;
  uint64_t trap_events_ = 0;
  uint64_t store_events_ = 0;
  uint64_t csr_events_ = 0;
  uint64_t dual_store_cycles_ = 0;
  uint64_t post_exit_retires_ = 0;
  uint64_t seq_expected_ = 0;
  size_t store_index_ = 0;
  size_t csr_index_ = 0;
  size_t ref_retires_consumed_ = 0;
  size_t ref_traps_consumed_ = 0;
  uint32_t last_commit_ = 0;
  uint32_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  bool trap_entry_valid_ = false;
  bool trap_entry_irq_ = false;
  uint64_t trap_entry_cause_ = 0;
  uint64_t trap_entry_tval_ = 0;
  uint64_t trap_entry_epc_ = 0;
  std::vector<EvRecord> records_;
  std::vector<EvRecord> trap_records_;
  std::vector<EvRecord> store_records_;
  std::vector<EvRecord> csr_records_;
  std::vector<RefEvent> ref_retires_;
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
  Fail("setup", "cannot find the repository root: no config/profiles/p0.json above "
                "the working directory");
}

// The words of the data region the program writes, compared between the DUT's
// memory and the independent interpreter's: the payloads the events claimed are
// checked against the effects that actually landed.
constexpr int kDataWords = 12;   // 96 bytes, covering every store above

}  // namespace

// ============================================================================
// The run
// ============================================================================
static uint64_t RunCase(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                        const Geometry& g) {
  const std::string phase = "event-payload";
  const DirectedProgram program = BuildProgram(g.reset_vector);

  ProgImage image;
  for (size_t i = 0; i < program.words.size(); i++) {
    image.Put(program.addrs[i], program.words[i]);
  }

  mosaic::MemoryModel dut_mem;
  mosaic::MemoryModel ref_mem;
  for (const auto& kv : image.words()) {
    if (dut_mem.Write(kv.first, 4, kv.second) != mosaic::AccessStatus::kOk ||
        ref_mem.Write(kv.first, 4, kv.second) != mosaic::AccessStatus::kOk) {
      Fail(phase, "the program does not fit the memory map at " + U64(kv.first));
    }
  }

  const RefResult reference =
      mosaic_trap::RunReference(image, g.reset_vector, &ref_mem);
  if (!reference.exited) {
    Fail(phase, "the independent interpreter did not reach the exit protocol: " +
                    reference.stop_reason);
  }
  if (reference.traps != 1) {
    Fail(phase, "the interpreter predicts one trap, not " + Dec(reference.traps));
  }

  Harness harness(dut, reporter, kRunCycles, &image, &dut_mem, &program);
  harness.Configure(g);
  harness.Phase(phase);
  harness.Expect(&reference);
  for (const RefEvent& e : reference.trace) {
    if (!e.trap) harness.AddRefRetire(e);
  }

  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);
  while (!harness.Finished() && !dut->o_stopped_o && harness.cycles() < kRunCycles) {
    harness.Cycle(false);
  }
  // The machine may retire a few more instructions between the exit store and
  // the memory system seeing it; those are observed (they are park-loop
  // instructions) but no longer compared with the interpreter's trace.
  harness.StopComparing();
  for (int i = 0; i < 8; i++) harness.Cycle(false);

  // ---------------------------------------------------------- the exit
  // The stop is checked first: a machine that refused a macro has not reached
  // the exit protocol either, and "it stopped on this instruction" is the more
  // specific diagnosis of the two.
  harness.Check("the machine never stopped on a refused macro",
                dut->o_unsupported_o == 0 && dut->o_illegal_o == 0 &&
                    dut->o_stopped_o == 0,
                "unsupported=" + Dec(dut->o_unsupported_o) + " illegal=" +
                    Dec(dut->o_illegal_o) + " stopped=" + Dec(dut->o_stopped_o) +
                    " (" + harness.State() + ")");
  harness.Check("the program wrote TOHOST with the pass code",
                harness.Finished() && harness.Passed() &&
                    harness.exit_code() == 1,
                "finished=" + Dec(harness.Finished() ? 1 : 0) + " passed=" +
                    Dec(harness.Passed() ? 1 : 0) + " tohost=" +
                    Dec(harness.exit_code()));

  // ------------------------------------------- the payloads, one by one
  // Printed, not asserted in prose: every record the machine published, so the
  // field-for-field comparison above can be read against the program's own
  // expectation.
  for (const EvRecord& e : harness.records()) {
    std::printf("  [ev]    cycle=%llu lane=%u %-6s pc=%s seq=%llu", 
                static_cast<unsigned long long>(e.cycle), e.lane,
                e.trap ? "TRAP" : "RETIRE", U64(e.pc).c_str(),
                static_cast<unsigned long long>(e.seq));
    if (e.trap) {
      std::printf(" cause=%s tval=%s", U64(e.trap_cause).c_str(),
                  U64(e.trap_tval).c_str());
    } else {
      std::printf(" rd=x%u we=%u value=%s", e.rd, e.reg_we ? 1 : 0,
                  U64(e.value).c_str());
    }
    std::printf("\n");
  }
  for (const EvRecord& e : harness.store_records()) {
    std::printf("  [store] cycle=%llu lane=%u pc=%s addr=%s data=%s size=%u\n",
                static_cast<unsigned long long>(e.cycle), e.lane, U64(e.pc).c_str(),
                U64(e.store_addr).c_str(), U64(e.store_data).c_str(), e.store_size);
  }
  for (const EvRecord& e : harness.csr_records()) {
    std::printf("  [csr]   cycle=%llu lane=%u pc=%s addr=%s value=%s\n",
                static_cast<unsigned long long>(e.cycle), e.lane, U64(e.pc).c_str(),
                U64(e.csr_addr).c_str(), U64(e.csr_value).c_str());
  }
  for (const EvRecord& e : harness.trap_records()) {
    std::printf("  [trap]  cycle=%llu lane=%u pc=%s cause=%s tval=%s\n",
                static_cast<unsigned long long>(e.cycle), e.lane, U64(e.pc).c_str(),
                U64(e.trap_cause).c_str(), U64(e.trap_tval).c_str());
  }

  // ------------------------------------------------- the stream, in totals
  harness.Check("the retire counter equals the retirement events published",
                dut->o_commit_o == harness.retires(),
                "counter=" + Dec(dut->o_commit_o) + " events=" +
                    Dec(harness.retires()));
  harness.Check("every store the program assembled published a store payload",
                harness.store_events() == program.stores.size(),
                "store events=" + Dec(harness.store_events()) + ", the program "
                "assembled " + Dec(program.stores.size()));
  harness.Check("every CSR write the program assembled published a CSR payload",
                harness.csr_events() == program.csr_writes.size(),
                "CSR events=" + Dec(harness.csr_events()) + ", the program "
                "assembled " + Dec(program.csr_writes.size()));
  harness.Check("exactly one trap event was published, for the ecall",
                harness.trap_events() == 1,
                "trap events=" + Dec(harness.trap_events()));
  harness.Check("the store queue authorised exactly the retiring stores",
                dut->o_mem_sq_commit_o == program.stores.size(),
                "sq commits=" + Dec(dut->o_mem_sq_commit_o) + ", the program "
                "assembled " + Dec(program.stores.size()));
  // The dual-retire shape: the case is written to produce it, so its absence is
  // a statement about the program rather than a silent gap. (The two stores in
  // one cycle are the only way to prove the payloads are per-lane.)
  harness.Check("a cycle retired a store in lane 0 and a store in lane 1",
                harness.dual_store_cycles() > 0,
                "dual store cycles=" + Dec(harness.dual_store_cycles()));
  harness.Check("every retirement the interpreter predicted was published",
                harness.ref_retires_consumed() == harness.ref_retires_total(),
                "compared " + Dec(harness.ref_retires_consumed()) + " of " +
                    Dec(harness.ref_retires_total()) +
                    " predicted retirements (post-exit park retires: " +
                    Dec(harness.post_exit_retires()) + ")");

  // ------------------------------------------------------ the end state
  harness.Check("the CSR file holds the mscratch the program's writes compute",
                dut->o_csr_mscratch_o == program.mscratch_final,
                "mscratch=" + U64(dut->o_csr_mscratch_o) + " expected " +
                    U64(program.mscratch_final));
  harness.Check("the handler's mepc write is the value the event reported",
                dut->o_csr_mepc_o == program.mepc_final,
                "mepc=" + U64(dut->o_csr_mepc_o) + " expected " +
                    U64(program.mepc_final));
  harness.Check("mtvec holds the handler address the program installed",
                dut->o_csr_mtvec_o == program.mtvec_final,
                "mtvec=" + U64(dut->o_csr_mtvec_o) + " expected " +
                    U64(program.mtvec_final));
  // The LWU the program executes: the decoder accepting the encoding is not
  // enough, the value has to come back zero-extended. The independent
  // interpreter's value comparison above covers this too; this check names it.
  {
    bool found = false;
    uint64_t got = 0;
    for (const EvRecord& e : harness.records()) {
      if (!e.trap && e.pc == program.lwu_pc) {
        found = true;
        got = e.value;
      }
    }
    harness.Check("lwu retires with the zero-extended word it loaded",
                  found && got == 0xDEADBEEFull,
                  "found=" + Dec(found ? 1 : 0) + " value=" + U64(got) +
                      " expected " + U64(0xDEADBEEFull));
  }
  harness.Check("the CSR trap counter counts the one trap",
                dut->o_csr_trap_o == 1, "traps=" + Dec(dut->o_csr_trap_o));
  harness.Check("the handler's MRET was committed by the CSR file",
                dut->o_csr_mret_o == 1, "mret=" + Dec(dut->o_csr_mret_o));

  // The data region, DUT memory against the independent interpreter's memory:
  // byte for byte, so a payload that named the right address and the wrong
  // bytes cannot pass.
  for (int i = 0; i < kDataWords; i++) {
    uint64_t dut_word = 0;
    uint64_t ref_word = 0;
    const uint64_t addr = kDataBase + 8ull * i;
    if (dut_mem.Read(addr, 8, &dut_word) != mosaic::AccessStatus::kOk ||
        ref_mem.Read(addr, 8, &ref_word) != mosaic::AccessStatus::kOk) {
      Fail(phase, "the data region is not readable at " + U64(addr));
    }
    harness.Check("memory word " + Dec(i) + " at " + U64(addr) +
                      " equals the interpreter's",
                  dut_word == ref_word,
                  "dut=" + U64(dut_word) + " reference=" + U64(ref_word));
  }

  std::printf("core.event_payload: events=%llu retires=%llu stores=%llu csr=%llu "
              "traps=%llu dual_store=%llu post_exit=%llu cycles=%llu\n",
              static_cast<unsigned long long>(harness.records().size()),
              static_cast<unsigned long long>(harness.retires()),
              static_cast<unsigned long long>(harness.store_events()),
              static_cast<unsigned long long>(harness.csr_events()),
              static_cast<unsigned long long>(harness.trap_events()),
              static_cast<unsigned long long>(harness.dual_store_cycles()),
              static_cast<unsigned long long>(harness.post_exit_retires()),
              static_cast<unsigned long long>(harness.cycles()));
  return harness.comparisons();
}

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
    if (geometry.reset_vector != 0x80000000ull) {
      Fail("geometry", "the reset vector is not the p0 link base: " +
                           U64(geometry.reset_vector));
    }
    if (geometry.xlen != 64) {
      Fail("geometry", "the profile is not 64-bit");
    }
    if (geometry.retire_width < 2) {
      Fail("geometry", "the profile does not retire two lanes per cycle, so the "
                       "per-lane payload rule cannot be exercised");
    }
    const uint64_t comparisons = RunCase(&dut, &reporter, geometry);
    detail = "checks=" + Dec(reporter.checks()) + " comparisons=" +
             Dec(comparisons) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the event payload contract holds",
                      "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
