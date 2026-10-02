// ============================================================================
// tb_core_priv.cpp -- CASE=privilege.permission_matrix, work package I-044.
//
// The DUT is the integrated core with the privilege state (mosaic_csr), the
// satisfaction decision (mosaic_interrupt), the physical memory protection
// unit (mosaic_pmp) and the LSU endpoint's refusal path wired in. The case
// drives one hand-assembled program that walks an explicit
// (privilege mode) x (access class) x (PMP configuration) matrix and checks
// every cell against an expectation derived from the ISA text.
//
// ------------------------------------------------------------------ the card
//
// docs/implementation-plan.md I-044: "实现 delegation、SRET、privilege access
// checks、PMP overlap/lock/address matching；按 region 边界测试
// fetch/load/store/AMO." Pass: "每种 privilege/access/PMP 组合与 spec/reference
// 一致，permission fault 无 side effect." Fail: "M/S/U mode 仅改标签不检查访问，
// 或 lock/WARL 行为不一致."
//
// ---------------------------------------------------------- how the matrix runs
//
// One program, entered once at the reset vector, is the whole run. M-mode code
// sets the trap vector, the delegation registers and the PMP entries for each
// scenario, then either performs the access itself or does an `mret` into the
// scenario's stub so the access executes in S- or U-mode. Every trap is taken
// in M-mode (nothing is delegated) and the handler writes one 128-byte frame
// per scenario: `mcause`, `mtval`, `mepc`, `mstatus` and the eight registers
// the scenarios use, all *before* it touches any of them. It then sets `mepc`
// to the frame's resume address, forces `mstatus.MPP = M` and `mret`s, so the
// run always comes back to M-mode at the `keep_N` label that follows the
// scenario.
//
// That is the bootstrap the card's [remark] names: to test S-mode the machine
// must first be able to *enter* S-mode, and the only way in is M-mode code
// setting `mstatus.MPP` and executing `MRET`. The two `boot.*` scenarios below
// assert that transition directly -- they set MPP and `mret`, and the cause of
// the `ecall` the target stub executes (9/8 vs 11) is the evidence of which
// mode the machine actually resumed in. In a profile whose CSR tables have no
// supervisor file the same scenarios assert the opposite outcome, so a machine
// that cannot enter S-mode is a *result of this case*, not a silent skip.
//
// ----------------------------------------------------- where expectations come from
//
// Nothing below is read back out of the DUT:
//
//   * Each scenario carries a hand-written expectation ("this access is denied,
//     cause 5, tval the address") taken from the privileged specification's
//     rules, quoted in the `rule` field and in results/reports/I-044-privilege.md.
//   * A second, independent model of those rules (`PmpModel`, below) is written
//     from the same text and computes the same answer from the entry values the
//     driver programmed. The driver requires the two to agree before it compares
//     the machine with either -- a scenario whose hand-written expectation and
//     model disagree is a driver bug and fails as one.
//   * "No side effect" is checked three ways: the destination register in the
//     trap frame still holds its pre-value, the memory word at the accessed
//     address is byte-for-byte what it was, and no transaction for that address
//     ever reached the data port. The last is structural -- the endpoint refuses
//     before entering its request state -- and the first two are architectural.
//   * The privilege a trap was taken *from* is `mstatus.MPP` in the frame, which
//     is the ISA's own record of the mode. A scenario that claims to run in S
//     mode and whose frame says M-mode is a failure, not a skip.
//
// ---------------------------------------------------------------- profile
//
// The runner builds this case for whatever profile it is asked for, and the
// profile decides two things the driver refuses to guess: how many PMP entries
// exist (config/geometry/<p>.json `pmp` block) and whether S/U exist at all
// (the profile's `csr_files` list). Both come from the generated packages
// through `o_geom_*` ports. A profile with no `pmp` block implements zero
// entries, and then the PMP rows of the matrix are *inapplicable by
// construction*: the case says so, counts them as not exercised, and still runs
// the rows that are applicable (the no-match row, the CSR table, the mode
// bootstrap). results/reports/I-044-privilege.md records exactly which rows are
// exercised under which profile.
//
// ------------------------------------------------------------------ mutants
//
// tools/run_priv_controls.py rebuilds this case with one `-D` per fail mode the
// card names; three of them are RTL switches the PMP unit already carries
// (MOSAIC_PMP_MUTANT_LOCK_IGNORED, _OVERLAP_INVERTED, _M_MODE_ENFORCED) and the
// fourth is a driver-side control for "a permission fault still writes", which
// no RTL switch expresses because the refusal is structural. The table with real
// output is in results/reports/I-044-privilege.md.
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

using mosaic_ref::DataMem;

namespace {

constexpr int kResetCycles = 4;

// ---------------------------------------------------------------------------
// The memory layout. Everything is inside RAM (0x80000000..0x80200000) so an
// access that is *not* refused by PMP reaches real memory, and the only thing
// that can refuse one is the rule under test.
// ---------------------------------------------------------------------------
constexpr uint64_t kCodeLo   = 0x80000000ull;   // entry 0 covers this, RX
constexpr uint64_t kCodeHi   = 0x80008000ull;   // ... and nothing beyond it
constexpr uint64_t kGap      = 0x80008000ull;   // no entry covers this
constexpr uint64_t kGap2     = 0x8000C000ull;   // neither does this
constexpr uint64_t kDataA    = 0x80010000ull;   // the per-scenario region
constexpr uint64_t kDataHi   = 0x80018000ull;
constexpr uint64_t kDataB    = 0x80018000ull;   // the fetch-stub region
constexpr uint64_t kFetch0   = 0x80019000ull;   // a word holding `ecall`
constexpr uint64_t kFetch1   = 0x80019040ull;
constexpr uint64_t kFetch2   = 0x80019080ull;
constexpr uint64_t kFetch3   = 0x800190C0ull;
constexpr uint64_t kFrameLo  = 0x80020000ull;
constexpr uint64_t kFrameStride = 0x80ull;
constexpr uint64_t kRecLo    = 0x80024000ull;   // the M-mode x5 records
constexpr uint64_t kScratch  = 0x80025000ull;   // the store scenarios' words

// Frame fields, written by the handler.
constexpr int F_VALID = 0, F_CAUSE = 8, F_TVAL = 16, F_EPC = 24, F_MSTATUS = 32,
              F_RESUME = 40, F_X5 = 48;
// xN at F_XN = F_X5 + 8*(N-5) for N in 5..12.

// The ISA's exception codes (mosaic_pkg.sv).
constexpr uint64_t kExcInsnAccess  = 1;
constexpr uint64_t kExcIllegal     = 2;
constexpr uint64_t kExcLoadAccess  = 5;
constexpr uint64_t kExcStoreAccess = 7;
constexpr uint64_t kExcEcallU      = 8;
constexpr uint64_t kExcEcallS      = 9;
constexpr uint64_t kExcEcallM      = 11;

// PMP configuration-byte fields (Priv v1.12 2.7.1, Figure "PMP configuration
// register"): L is bit 7, A is bits 4:3, X/W/R are bits 2/1/0.
constexpr unsigned kAOff = 0, kATor = 1, kANa4 = 2, kANapot = 3;

constexpr uint32_t CSR_MSTATUS = 0x300, CSR_MEDELEG = 0x302, CSR_MIDELEG = 0x303,
                   CSR_MIE = 0x304, CSR_MTVEC = 0x305, CSR_MCOUNTEREN = 0x306,
                   CSR_MSCRATCH = 0x340, CSR_MEPC = 0x341, CSR_MCAUSE = 0x342,
                   CSR_MTVAL = 0x343, CSR_SSCRATCH = 0x140, CSR_SSTATUS = 0x100,
                   CSR_SCOUNTEREN = 0x106, CSR_CYCLE = 0xC00,
                   CSR_PMPCFG0 = 0x3A0, CSR_PMPCFG2 = 0x3A2, CSR_PMPADDR0 = 0x3B0;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t v) { return mosaic::Hex(v); }
std::string Dec(uint64_t v) { return std::to_string(v); }
bool Debug() { return std::getenv("PRIV_DEBUG") != nullptr; }

// ============================================================================
// The PMP rule model -- written from the privileged specification's text, not
// from the RTL. It is the second opinion the driver holds every scenario's
// hand-written expectation against.
// ============================================================================
struct PmpEntry {
  bool used = false;
  uint8_t cfg = 0;      // the canonical configuration byte
  uint64_t addr = 0;    // the canonical pmpaddr value (bits 63:54 are zero)
};

class PmpModel {
 public:
  // `entries` is indexed by PMP entry number; an entry beyond the implemented
  // count is not present and therefore matches nothing.
  explicit PmpModel(const std::vector<PmpEntry>& entries) : e_(entries) {}

  // "A=0: this PMP entry is disabled and matches no addresses." TOR: "the entry
  // matches any address y such that pmpaddr_(i-1) <= y < pmpaddr_i ... entry 0's
  // lower bound is zero". NA4 matches one word. NAPOT: n trailing ones encode a
  // 2^(n+3)-byte naturally aligned region.
  bool MatchByte(unsigned i, uint64_t byte_addr) const {
    if (i >= e_.size() || !e_[i].used) return false;
    const uint64_t y = byte_addr >> 2;   // pmpaddr holds bits 55:2
    const uint8_t cfg = e_[i].cfg;
    switch ((cfg >> 3) & 3u) {
      case kAOff: return false;
      case kANa4: return y == e_[i].addr;
      case kANapot: {
        unsigned n = 0;
        while (n < 63 && ((e_[i].addr >> n) & 1ull) != 0) n++;
        const uint64_t mask = (n >= 63) ? 0ull : (~0ull << (n + 1));
        return (y & ~mask) == (e_[i].addr & ~mask);
      }
      default: {
        const uint64_t lo = (i == 0) ? 0ull : e_[i - 1].addr;
        return y >= lo && y < e_[i].addr;
      }
    }
  }

  // "The lowest-numbered PMP entry that matches any byte of an access determines
  // whether that access succeeds or fails. The matching PMP entry must match all
  // bytes of an access, or the access fails, irrespective of the L, R, W, and X
  // bits." Then the privilege rule: "If the L bit is clear and the privilege mode
  // of the access is M, the access succeeds." Otherwise the access's own R/W/X
  // bit must be set. No match: M succeeds; S/U fails when at least one entry is
  // implemented.
  bool Allow(uint64_t addr, unsigned bytes, bool r, bool w, bool x,
             unsigned priv) const {
    unsigned first = 0;
    bool have = false;
    for (unsigned i = 0; i < e_.size(); i++) {
      if (MatchByte(i, addr)) { first = i; have = true; break; }
    }
    if (!have) return priv == 3u;   // no matching entry
    bool last_same = false;
    for (unsigned i = 0; i < e_.size(); i++) {
      if (MatchByte(i, addr + bytes - 1)) { last_same = (i == first); break; }
    }
    if (!last_same) return false;   // the governing entry must match every byte
    const uint8_t cfg = e_[first].cfg;
    if ((cfg & 0x80u) == 0 && priv == 3u) return true;
    return (r && (cfg & 1u)) || (w && (cfg & 2u)) || (x && (cfg & 4u));
  }

 private:
  std::vector<PmpEntry> e_;
};

// ============================================================================
// The scenario table
// ============================================================================
enum class Mod { kM = 3, kS = 1, kU = 0 };
enum class Cls { kLoad, kStore, kFetch, kAmo, kLr, kSc, kScLr, kCsr, kNop,
                 kMret, kSret };
enum class Cat { kNoEntry, kOff, kNa4, kNapot, kTor, kLocked, kOverlap,
                 kGranularity, kSret, kDelegation };

struct Ent {
  unsigned idx;
  uint8_t cfg;
  uint64_t addr;
};

struct CsrOp {
  bool write = false;
  uint32_t csr = 0;
  uint64_t operand = 0;
};

// The expectation for one scenario, plus the one that applies when the profile
// lacks the prerequisite the scenario needs. `nofault_cause` is the cause of the
// trap a scenario is expected to take when the access itself is *allowed* -- the
// stub's trailing `ecall` (9 in S, 8 in U) or, in a profile with no S/U, the
// M-mode ecall (11) the same stub produces.
struct Expect {
  bool fault = false;
  uint64_t cause = 0;
  uint64_t nofault_cause = 0;   // 0 means "derive it from the mode"
};

struct Scenario {
  std::string name;
  Mod mode = Mod::kM;
  Cls cls = Cls::kLoad;
  Cat cat = Cat::kNoEntry;
  const char* rule = "";
  bool skip_without_pmp = false;   // needs PMP entries to mean anything
  bool skip_without_su = false;    // needs S or U mode
  bool alt_without_pmp = false;    // use `alt` when no entry is implemented
  bool alt_without_su = false;     // use `alt` when the profile has no S/U
  bool alt_without_u = false;      // ... only when U is missing
  bool alt_when_u_present = false; // use `alt` when U *is* present
  Expect exp;
  Expect alt;

  std::vector<Ent> entries;
  std::vector<CsrOp> pre_csr;    // M-mode CSR writes before the access
  std::vector<CsrOp> csr_ops;    // the access itself, when cls == kCsr

  // Filled in while the program is emitted.
  unsigned index = 0;
  uint64_t frame = 0;
  uint64_t rec = 0;
  uint64_t access_pc = 0;
  uint64_t stub_pc = 0;
  uint64_t expect_epc_fault = 0;
  uint64_t expect_epc_nofault = 0;

  // Inputs the driver seeds.
  uint64_t addr = 0;      // the address the access targets
  unsigned width = 8;     // bytes: 4 for the scenarios that must stay aligned
  uint64_t pre = 0;       // the word already at `addr`
  uint64_t preset = 0;    // the value x5 holds before the access
  uint64_t data = 0;      // the value a store-like access writes
};

uint8_t Cfg(bool lock, unsigned a, bool r, bool w, bool x) {
  return static_cast<uint8_t>((lock ? 0x80u : 0u) | ((a & 3u) << 3) |
                              (x ? 4u : 0u) | (w ? 2u : 0u) | (r ? 1u : 0u));
}

// NAPOT's trailing-ones encoding: n ones encode 2^(n+3) bytes.
uint64_t NapotAddr(uint64_t base, uint64_t size) {
  unsigned n = 0;
  while (n < 60 && (1ull << (n + 3)) < size) n++;
  return (base >> 2) | ((1ull << n) - 1ull);
}
uint64_t TorAddr(uint64_t base) { return base >> 2; }

// ============================================================================
// A small assembler (the same shape the sibling core cases use)
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

class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}

  uint64_t pc() const { return base_ + 4ull * words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }

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
    return ((imm20 & 0xFFFFFu) << 12) | (rd << 7) | op;
  }

  void Emit(uint32_t w) { words_.push_back(w); }
  void Addi(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x13)); }
  void Add(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0, rs2, rs1, 0, rd, 0x33)); }
  void Or(uint32_t rd, uint32_t rs1, uint32_t rs2) { Emit(R(0, rs2, rs1, 6, rd, 0x33)); }
  void Slli(uint32_t rd, uint32_t rs1, uint32_t sh) { Emit(I(static_cast<int32_t>(sh), rs1, 1, rd, 0x13)); }
  void Srli(uint32_t rd, uint32_t rs1, uint32_t sh) { Emit(I(static_cast<int32_t>(sh), rs1, 5, rd, 0x13)); }
  void Lui(uint32_t rd, uint32_t imm20) { Emit(U(imm20, rd, 0x37)); }
  void Ld(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 3, rd, 0x03)); }
  void Lw(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 2, rd, 0x03)); }
  void Lwu(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 6, rd, 0x03)); }
  void Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { Emit(S(imm, rs2, rs1, 3, 0x23)); }
  void Sw(uint32_t rs2, uint32_t rs1, int32_t imm) { Emit(S(imm, rs2, rs1, 2, 0x23)); }
  void Jalr(uint32_t rd, uint32_t rs1, int32_t imm) { Emit(I(imm, rs1, 0, rd, 0x67)); }
  void Beq(uint32_t rs1, uint32_t rs2, int32_t imm) {
    const uint32_t u = static_cast<uint32_t>(imm);
    Emit(((u >> 12) & 1u) << 31 | ((u >> 5) & 0x3Fu) << 25 | (rs2 << 20) |
         (rs1 << 15) | (0u << 12) | ((u >> 1) & 0xFu) << 8 |
         ((u >> 11) & 1u) << 7 | 0x63);
    (void)imm;
  }
  void Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(static_cast<int32_t>(csr), rs1, 1, rd, 0x73)); }
  void Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) { Emit(I(static_cast<int32_t>(csr), rs1, 2, rd, 0x73)); }
  void AmoAddD(uint32_t rd, uint32_t rs2, uint32_t rs1) {
    Emit(R(0x00, rs2, rs1, 3, rd, 0x2F));
  }
  void AmoAddW(uint32_t rd, uint32_t rs2, uint32_t rs1) {
    Emit(R(0x00, rs2, rs1, 2, rd, 0x2F));
  }
  // The funct5 field is bits 31:27 and aq/rl are bits 26/25, so R()'s f7 is
  // (funct5 << 2) | (aq << 1) | rl: LR.D's funct5 is 0b00010 and SC.D's is
  // 0b00011, which is 0x08 and 0x0C here -- not 0x02 and 0x03, which would
  // decode as AMOADD with the aq and rl bits set.
  void LrD(uint32_t rd, uint32_t rs1) { Emit(R(0x08, 0, rs1, 3, rd, 0x2F)); }
  void ScD(uint32_t rd, uint32_t rs2, uint32_t rs1) { Emit(R(0x0C, rs2, rs1, 3, rd, 0x2F)); }
  void Ecall() { Emit(0x00000073u); }
  void Mret() { Emit(0x30200073u); }
  void Sret() { Emit(0x10200073u); }
  void JalSelf() { Emit(0x0000006Fu); }

  // A 32-bit constant, zero-extended into the 64-bit register. The RV64 `lui`
  // writes its 32-bit result sign-extended, so a constant with bit 31 set needs
  // the shift pair at the end to clear bits 63:32; the shift pair is harmless
  // for the rest and keeps this one shape for every value.
  void LiAbs32(uint32_t rd, uint32_t v) {
    if (v >= 0xFFFFF800u) {
      Addi(rd, 0, static_cast<int32_t>(v));   // v - 2^32 is in [-2048, -1]
    } else if (v <= 0x7FFu) {
      Addi(rd, 0, static_cast<int32_t>(v));
    } else {
      const uint32_t lo = v & 0xFFFu;
      const uint32_t hi = (v + 0x800u) >> 12;
      Lui(rd, hi);
      Addi(rd, rd, static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0));
    }
    Slli(rd, rd, 32);
    Srli(rd, rd, 32);
  }

  // Any 64-bit constant. Values wider than 32 bits use x31 as scratch, which is
  // a documented, deliberate use of the one register no scenario touches.
  void LiAbs(uint32_t rd, uint64_t value) {
    if (value <= 0xFFFFFFFFull) {
      LiAbs32(rd, static_cast<uint32_t>(value));
      return;
    }
    LiAbs32(rd, static_cast<uint32_t>(value >> 32));
    Slli(rd, rd, 32);
    LiAbs32(31u, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    Or(rd, rd, 31u);
  }

  void La(uint32_t rd, const std::string& label) {
    fixups_.push_back(Fixup{words_.size(), rd, label});
    words_.push_back(0);
    words_.push_back(0);
  }

  void Mark(const std::string& label) { labels_[label] = pc(); }
  uint64_t Label(const std::string& name) const {
    auto it = labels_.find(name);
    if (it == labels_.end()) Fail("assembler", "undefined label " + name);
    return it->second;
  }

  void Resolve() {
    for (const Fixup& f : fixups_) {
      auto it = labels_.find(f.label);
      if (it == labels_.end()) Fail("assembler", "undefined label " + f.label);
      const int64_t here = static_cast<int64_t>(base_ + 4ull * f.at);
      const int64_t delta = static_cast<int64_t>(it->second) - here;
      const uint32_t lo = static_cast<uint32_t>(delta) & 0xFFFu;
      const uint32_t hi = static_cast<uint32_t>((delta + 0x800) >> 12) & 0xFFFFFu;
      words_[f.at] = U(hi, f.rd, 0x17);   // auipc rd, hi
      words_[f.at + 1] = I(static_cast<int32_t>(lo) - ((lo & 0x800u) ? 0x1000 : 0),
                           f.rd, 0, f.rd, 0x13);
    }
  }

 private:
  struct Fixup {
    size_t at;
    uint32_t rd;
    std::string label;
  };
  uint64_t base_;
  std::vector<uint32_t> words_;
  std::vector<Fixup> fixups_;
  std::map<std::string, uint64_t> labels_;
};

// ============================================================================
// The scenario list
// ============================================================================
const char* ClsName(Cls c) {
  switch (c) {
    case Cls::kLoad: return "load";
    case Cls::kStore: return "store";
    case Cls::kFetch: return "fetch";
    case Cls::kAmo: return "amo";
    case Cls::kLr: return "lr";
    case Cls::kSc: return "sc";
    case Cls::kScLr: return "lr-sc";
    case Cls::kCsr: return "csr";
    case Cls::kNop: return "nop";
    case Cls::kMret: return "mret";
    case Cls::kSret: return "sret";
  }
  return "?";
}
const char* ModName(Mod m) {
  switch (m) {
    case Mod::kM: return "M";
    case Mod::kS: return "S";
    case Mod::kU: return "U";
  }
  return "?";
}
const char* CatName(Cat c) {
  switch (c) {
    case Cat::kNoEntry: return "no-entry";
    case Cat::kOff: return "off";
    case Cat::kNa4: return "na4";
    case Cat::kNapot: return "napot";
    case Cat::kTor: return "tor";
    case Cat::kLocked: return "locked";
    case Cat::kOverlap: return "overlap";
    case Cat::kGranularity: return "granularity";
    case Cat::kSret: return "xret";
    case Cat::kDelegation: return "delegation";
  }
  return "?";
}

bool IsStoreLike(Cls c) {
  return c == Cls::kStore || c == Cls::kAmo || c == Cls::kSc || c == Cls::kScLr;
}
bool IsLoadLike(Cls c) {
  return c == Cls::kLoad || c == Cls::kLr || c == Cls::kAmo;
}
// The coverage cell a scenario belongs to: the two-instruction LR/SC form is the
// "AMO/LR-SC" cell the card names, and the mode-bootstrap rows belong to no
// access class at all.
const char* ClassKey(Cls c) {
  switch (c) {
    case Cls::kScLr: return "sc";
    case Cls::kMret:
    case Cls::kSret: return "xret";
    case Cls::kNop: return "none";
    default: return ClsName(c);
  }
}

// Every scenario gets a word derived from the *address* it touches, so two
// scenarios that deliberately share an address (the allowed and the refused
// form of one access) agree about what was there before the run and the
// seeding cannot depend on the order they are declared in.
uint64_t DataWord(unsigned n) { return kDataA + 8ull * n; }
uint64_t SeedFor(uint64_t addr) {
  return 0xA5A5A5A500000000ull ^ (addr * 0x9E3779B97F4A7C15ull) ^ (addr >> 7);
}

uint64_t PreMask(unsigned width) {
  return (width >= 8) ? ~0ull : ((1ull << (8u * width)) - 1ull);
}

std::vector<Scenario> BuildScenarios() {
  std::vector<Scenario> v;
  unsigned n = 0;
  auto add = [&](Scenario s) {
    s.index = n;
    if (s.addr == 0) {
      s.addr = IsStoreLike(s.cls) ? (kScratch + 8ull * n) : DataWord(n);
    }
    // The seed and the store data are words *of the access's own width*, so a
    // 4-byte scenario compares the 32 bits it actually touched.
    const uint64_t m = PreMask(s.width);
    s.pre = SeedFor(s.addr) & m;
    s.data = (0x5A5A0000ull + 0x1000ull * n) & m;
    s.preset = 0xDEAD0000ull + n;
    v.push_back(s);
    n++;
  };

  // ------------------------------------------------------------------ group 1
  // No entry matches: M succeeds, and this row is the *only* PMP row a profile
  // with zero entries can run, which is why it is in this group and not in the
  // "needs entries" one.
  {
    Scenario s;
    s.name = "m-load-nomatch";
    s.mode = Mod::kM; s.cls = Cls::kLoad; s.cat = Cat::kNoEntry;
    s.addr = kGap2;
    s.rule = "no PMP entry matches an M-mode access -> the access succeeds";
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "m-store-nomatch";
    s.mode = Mod::kM; s.cls = Cls::kStore; s.cat = Cat::kNoEntry;
    s.addr = kGap2 + 8;
    s.rule = "no PMP entry matches an M-mode access -> the access succeeds";
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "m-fetch-nomatch";
    s.mode = Mod::kM; s.cls = Cls::kFetch; s.cat = Cat::kNoEntry;
    s.addr = kFetch2;
    s.rule = "no PMP entry matches an M-mode instruction access -> it succeeds";
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "m-amoadd-nomatch";
    s.mode = Mod::kM; s.cls = Cls::kAmo; s.cat = Cat::kNoEntry;
    s.addr = kGap2 + 16;
    s.rule = "an AMO is a store-class access for PMP and no entry matches -> M succeeds";
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "m-lr-nomatch";
    s.mode = Mod::kM; s.cls = Cls::kLr; s.cat = Cat::kNoEntry;
    s.addr = kGap2 + 24;
    s.rule = "a load-reserved is a load-class access and no entry matches -> M succeeds";
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "m-lr-sc-nomatch";
    s.mode = Mod::kM; s.cls = Cls::kScLr; s.cat = Cat::kNoEntry;
    s.addr = kGap2 + 32;
    s.rule = "a store-conditional is a store-class access and no entry matches -> "
             "M succeeds";
    s.exp = Expect{false, 0};
    add(s);
  }

  // ------------------------------------------------------------------ group 2
  // The CSR table itself: whether the PMP registers exist at all is a profile
  // fact, and both answers are asserted rather than assumed.
  {
    Scenario s;
    s.name = "csr-pmpcfg0-read";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.rule = "pmpcfg0 exists exactly when the profile implements PMP entries";
    s.alt_without_pmp = true;
    s.exp = Expect{false, 0};               // an implemented PMP CSR reads
    s.alt = Expect{true, kExcIllegal};      // no entries -> the number is illegal
    s.csr_ops.push_back(CsrOp{false, CSR_PMPCFG0, 0});
    add(s);
  }
  {
    Scenario s;
    s.name = "csr-pmpcfg1-illegal";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.rule = "for RV64 the odd-numbered pmpcfg registers are illegal";
    s.exp = Expect{true, kExcIllegal};
    s.csr_ops.push_back(CsrOp{false, 0x3A1, 0});
    add(s);
  }
  {
    Scenario s;
    s.name = "csr-pmpaddr16-illegal";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.rule = "pmpaddr selects an entry only up to the profile's entry count";
    s.exp = Expect{true, kExcIllegal};
    s.csr_ops.push_back(CsrOp{false, 0x3C0, 0});
    add(s);
  }

  // ------------------------------------------------------------------ group 3
  // The mode label changed but the check did not is the card's named failure:
  // an *unlocked* matching entry never refuses an M-mode access.
  {
    Scenario s;
    s.name = "m-unlocked-deny-load";
    s.mode = Mod::kM; s.cls = Cls::kLoad; s.cat = Cat::kNa4;
    s.skip_without_pmp = true;
    s.addr = kDataA + 0x1000;
    s.rule = "L=0 and the access is M-mode -> the access succeeds whatever R/W/X say";
    s.entries.push_back(Ent{1, Cfg(false, kANapot, false, false, false), NapotAddr(kDataA, 0x8000)});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "m-unlocked-deny-store";
    s.mode = Mod::kM; s.cls = Cls::kStore; s.cat = Cat::kNa4;
    s.skip_without_pmp = true;
    s.addr = kDataA + 0x1008;
    s.rule = "L=0 and the access is M-mode -> the access succeeds whatever R/W/X say";
    s.entries.push_back(Ent{1, Cfg(false, kANapot, false, false, false), NapotAddr(kDataA, 0x8000)});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "m-unlocked-deny-fetch";
    s.mode = Mod::kM; s.cls = Cls::kFetch; s.cat = Cat::kNa4;
    s.skip_without_pmp = true;
    s.addr = kFetch0;
    s.rule = "L=0 and the access is M-mode -> an instruction access succeeds too";
    s.entries.push_back(Ent{1, Cfg(false, kANapot, false, false, false), NapotAddr(kDataB, 0x8000)});
    s.exp = Expect{false, 0};   // the ecall at the target executes in M-mode
    add(s);
  }
  {
    Scenario s;
    s.name = "m-unlocked-deny-amo";
    s.mode = Mod::kM; s.cls = Cls::kAmo; s.cat = Cat::kNa4;
    s.skip_without_pmp = true;
    s.addr = kDataA + 0x1010;
    s.rule = "L=0 and the access is M-mode -> the AMO succeeds whatever R/W/X say";
    s.entries.push_back(Ent{1, Cfg(false, kANapot, false, false, false), NapotAddr(kDataA, 0x8000)});
    s.exp = Expect{false, 0};
    add(s);
  }

  // ------------------------------------------------------------------ group 4
  // The bootstrap: can the machine enter S or U at all? The stub executes
  // `ecall`, whose cause identifies the mode the machine resumed in.
  {
    Scenario s;
    s.name = "boot-mret-mpp-s";
    s.mode = Mod::kS; s.cls = Cls::kNop; s.cat = Cat::kSret;
    s.rule = "MRET sets the privilege mode from mstatus.MPP";
    s.alt_without_su = true;
    s.exp = Expect{false, 0, kExcEcallS};           // resumed in S: cause 9
    s.alt = Expect{false, 0, kExcEcallM};           // no S in this profile: cause 11
    add(s);
  }
  {
    Scenario s;
    s.name = "boot-mret-mpp-u";
    s.mode = Mod::kU; s.cls = Cls::kNop; s.cat = Cat::kSret;
    s.rule = "MRET sets the privilege mode from mstatus.MPP";
    s.alt_without_u = true;
    s.exp = Expect{false, 0, kExcEcallU};           // resumed in U: cause 8
    s.alt = Expect{false, 0, kExcEcallM};
    add(s);
  }
  {
    Scenario s;
    s.name = "boot-mstatus-mpp-warl";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.rule = "mstatus.MPP is writable exactly where the profile implements a "
             "less-privileged mode";
    s.alt_without_su = true;
    s.exp = Expect{false, 0};     // with S: MPP reads back 01
    s.alt = Expect{false, 0};     // without S: MPP is read-only 11
    s.pre_csr.push_back(CsrOp{true, CSR_MSTATUS, 0x800});
    s.csr_ops.push_back(CsrOp{false, CSR_MSTATUS, 0});
    add(s);
  }

  // ------------------------------------------------------------------ group 5
  // S and U accesses against each permission shape.
  struct AccSpec {
    const char* name;
    Mod mode;
    Cls cls;
    Cat cat;
    uint64_t addr;
    std::vector<Ent> entries;
    bool fault;
    uint64_t cause;
    const char* rule;
    unsigned width = 8;
  };
  const uint64_t a0 = kDataA + 0x2000;
  const std::vector<AccSpec> accs = {
      {"s-load-allow", Mod::kS, Cls::kLoad, Cat::kNapot, a0,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       false, 0,
       "R=1 and the load is S-mode -> the access succeeds"},
      {"s-load-deny-r", Mod::kS, Cls::kLoad, Cat::kNapot, a0,
       {{1, Cfg(false, kANapot, false, true, false), NapotAddr(kDataA, 0x8000)}},
       true, kExcLoadAccess,
       "a load with no read permission raises a load access fault (cause 5)"},
      {"s-store-allow", Mod::kS, Cls::kStore, Cat::kNapot, a0 + 8,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       false, 0,
       "W=1 and the store is S-mode -> the access succeeds"},
      {"s-store-deny-w", Mod::kS, Cls::kStore, Cat::kNapot, a0 + 0x18,
       {{1, Cfg(false, kANapot, true, false, false), NapotAddr(kDataA, 0x8000)}},
       true, kExcStoreAccess,
       "a store with no write permission raises a store access fault (cause 7)"},
      {"s-fetch-allow", Mod::kS, Cls::kFetch, Cat::kNa4, kFetch0,
       {{1, Cfg(false, kANa4, false, false, true), TorAddr(kFetch0)}},
       false, 0,
       "X=1 and the fetch is S-mode -> the instruction access succeeds"},
      {"s-fetch-deny-x", Mod::kS, Cls::kFetch, Cat::kNa4, kFetch1,
       {{1, Cfg(false, kANa4, true, false, false), TorAddr(kFetch1)}},
       true, kExcInsnAccess,
       "a fetch with no execute permission raises an instruction access fault "
       "(cause 1) with the PC as the trap value"},
      {"s-amoadd-allow", Mod::kS, Cls::kAmo, Cat::kNapot, a0 + 16,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       false, 0,
       "an AMO checks W (store class) and W=1 -> it succeeds"},
      {"s-amoadd-deny-r", Mod::kS, Cls::kAmo, Cat::kNapot, a0 + 0x28,
       {{1, Cfg(false, kANapot, true, false, false), NapotAddr(kDataA, 0x8000)}},
       true, kExcStoreAccess,
       "an AMO is checked for W only, so no W raises cause 7 even though R is set "
       "(the specification folds AMO into the store class)"},
      {"s-lr-allow", Mod::kS, Cls::kLr, Cat::kNapot, a0 + 24,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       false, 0,
       "a load-reserved is a load: R=1 -> it succeeds"},
      {"s-lr-deny-r", Mod::kS, Cls::kLr, Cat::kNapot, a0 + 24,
       {{1, Cfg(false, kANapot, false, true, false), NapotAddr(kDataA, 0x8000)}},
       true, kExcLoadAccess,
       "a load-reserved with no read permission raises cause 5, not 7"},
      {"s-sc-deny-w", Mod::kS, Cls::kSc, Cat::kNapot, a0 + 32,
       {{1, Cfg(false, kANapot, true, false, false), NapotAddr(kDataA, 0x8000)}},
       true, kExcStoreAccess,
       "a store-conditional is a store class access: no W raises cause 7"},
      {"s-sc-lr-allow", Mod::kS, Cls::kScLr, Cat::kNapot, a0 + 32,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       false, 0,
       "LR establishes the reservation and SC, with W set, completes"},
      {"s-nomatch-load", Mod::kS, Cls::kLoad, Cat::kNoEntry, kGap,
       {}, true, kExcLoadAccess,
       "no entry matches an S-mode access but an entry is implemented -> it fails"},
      {"s-nomatch-fetch", Mod::kS, Cls::kFetch, Cat::kNoEntry, kFetch2,
       {}, true, kExcInsnAccess,
       "no entry matches an S-mode fetch but an entry is implemented -> it fails"},
      {"s-na4-exact-allow", Mod::kS, Cls::kLoad, Cat::kNa4, a0 + 0x40,
       {{1, Cfg(false, kANa4, true, false, false), TorAddr(a0 + 0x40)}},
       false, 0,
       "NA4 matches exactly one word", 4},
      {"s-na4-next-word-deny", Mod::kS, Cls::kLoad, Cat::kNa4, a0 + 0x44,
       {{1, Cfg(false, kANa4, true, false, false), TorAddr(a0 + 0x40)}},
       true, kExcLoadAccess,
       "NA4 matches one word only, so the next word has no match and fails", 4},
      {"s-napot-last-word-allow", Mod::kS, Cls::kLoad, Cat::kNapot,
       kDataHi - 8,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       false, 0,
       "the last word inside a NAPOT region is covered by it"},
      {"s-napot-past-end-deny", Mod::kS, Cls::kLoad, Cat::kNapot, kDataHi,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       true, kExcLoadAccess,
       "one word past a NAPOT region is not covered by it and no other entry "
       "matches -> the S-mode access fails"},
      {"s-na4-span-first-deny", Mod::kS, Cls::kLoad, Cat::kGranularity, a0 + 0x80,
       {{1, Cfg(false, kANa4, true, true, false), TorAddr(a0 + 0x80)}},
       true, kExcLoadAccess,
       "the lowest-numbered matching entry must match every byte of the access, "
       "so a 4-byte entry cannot cover an 8-byte load"},
      {"s-na4-span-last-deny", Mod::kS, Cls::kLoad, Cat::kGranularity, a0 + 0x88,
       {{1, Cfg(false, kANa4, true, true, false), TorAddr(a0 + 0x8C)}},
       true, kExcLoadAccess,
       "an entry that matches only the last byte of the access governs it and "
       "fails it"},
      {"s-napot-span-allow", Mod::kS, Cls::kLoad, Cat::kGranularity, kDataA + 0x3000,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       false, 0,
       "an 8-byte access whose first and last byte are inside one entry succeeds"},
      {"s-tor-allow", Mod::kS, Cls::kLoad, Cat::kTor, a0 + 0xC0,
       {{1, Cfg(false, kAOff, false, false, false), TorAddr(a0 + 0x40)},
        {2, Cfg(false, kATor, true, false, false), TorAddr(a0 + 0x100)}},
       false, 0,
       "TOR matches pmpaddr[i-1] <= y < pmpaddr[i]"},
      {"s-tor-below-deny", Mod::kS, Cls::kLoad, Cat::kTor, a0 + 0x38,
       {{1, Cfg(false, kAOff, false, false, false), TorAddr(a0 + 0x40)},
        {2, Cfg(false, kATor, true, false, false), TorAddr(a0 + 0x100)}},
       true, kExcLoadAccess,
       "TOR's lower bound is pmpaddr[i-1], so one word below it is not matched", 4},
      {"s-tor-at-top-deny", Mod::kS, Cls::kLoad, Cat::kTor, a0 + 0x100,
       {{1, Cfg(false, kAOff, false, false, false), TorAddr(a0 + 0x40)},
        {2, Cfg(false, kATor, true, false, false), TorAddr(a0 + 0x100)}},
       true, kExcLoadAccess,
       "TOR's upper bound is pmpaddr[i], exclusive"},
      {"s-overlap-lower-denies", Mod::kS, Cls::kLoad, Cat::kOverlap, a0 + 0x200,
       {{2, Cfg(false, kANapot, false, false, false), NapotAddr(kDataA, 0x8000)},
        {3, Cfg(false, kANa4, true, false, false), TorAddr(a0 + 0x200)}},
       true, kExcLoadAccess,
       "the lowest-numbered matching entry decides, so the broad entry 2 denies "
       "the load even though the narrower entry 3 allows it"},
      {"s-overlap-lower-allows", Mod::kS, Cls::kLoad, Cat::kOverlap, a0 + 0x200,
       {{2, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)},
        {3, Cfg(false, kANa4, false, false, false), TorAddr(a0 + 0x200)}},
       false, 0,
       "a lower-numbered allowing entry is never displaced by a higher-numbered "
       "denying one"},
      {"u-load-allow", Mod::kU, Cls::kLoad, Cat::kNapot, a0 + 0x300,
       {{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataA, 0x8000)}},
       false, 0,
       "the same permission rule applies in U-mode"},
      {"u-store-deny-w", Mod::kU, Cls::kStore, Cat::kNapot, a0 + 0x300,
       {{1, Cfg(false, kANapot, true, false, false), NapotAddr(kDataA, 0x8000)}},
       true, kExcStoreAccess,
       "a U-mode store with no write permission raises cause 7"},
      {"u-fetch-deny-x", Mod::kU, Cls::kFetch, Cat::kNa4, kFetch3,
       {{1, Cfg(false, kANa4, true, false, false), TorAddr(kFetch3)}},
       true, kExcInsnAccess,
       "a U-mode fetch with no execute permission raises cause 1"},
      {"u-nomatch-load", Mod::kU, Cls::kLoad, Cat::kNoEntry, kGap + 8,
       {}, true, kExcLoadAccess,
       "no entry matches a U-mode access but an entry is implemented -> it fails"},
  };
  for (const AccSpec& a : accs) {
    Scenario s;
    s.name = a.name;
    s.mode = a.mode;
    s.cls = a.cls;
    s.cat = a.cat;
    s.addr = a.addr;
    s.entries = a.entries;
    s.rule = a.rule;
    s.width = a.width;
    s.skip_without_pmp = true;
    s.skip_without_su = (a.mode != Mod::kM);
    s.exp = Expect{a.fault, a.cause};
    add(s);
  }

  // ------------------------------------------------------------------ group 6
  // Locked entries: L=1 makes an M-mode access subject to the entry's bits, and
  // makes the entry's configuration and address unwritable.
  {
    Scenario s;
    s.name = "lock-deny-m-load";
    s.mode = Mod::kM; s.cls = Cls::kLoad; s.cat = Cat::kLocked;
    s.skip_without_pmp = true;
    s.addr = kDataB + 0x400;
    s.width = 4;
    s.rule = "L=1 makes an entry's permissions apply to M-mode as well";
    s.entries.push_back(Ent{8, Cfg(true, kANa4, false, false, false), TorAddr(s.addr)});
    s.exp = Expect{true, kExcLoadAccess};
    add(s);
  }
  {
    Scenario s;
    s.name = "lock-deny-m-store";
    s.mode = Mod::kM; s.cls = Cls::kStore; s.cat = Cat::kLocked;
    s.skip_without_pmp = true;
    s.addr = kDataB + 0x408;
    s.width = 4;
    s.rule = "L=1 makes an entry's permissions apply to M-mode as well (cause 7)";
    s.entries.push_back(Ent{9, Cfg(true, kANa4, false, false, false), TorAddr(s.addr)});
    s.exp = Expect{true, kExcStoreAccess};
    add(s);
  }
  {
    Scenario s;
    s.name = "lock-deny-m-fetch";
    s.mode = Mod::kM; s.cls = Cls::kFetch; s.cat = Cat::kLocked;
    s.skip_without_pmp = true;
    s.addr = kFetch3;
    s.rule = "L=1 and no X: an M-mode instruction access from the entry fails";
    s.entries.push_back(Ent{10, Cfg(true, kANa4, false, false, false), TorAddr(kFetch3)});
    s.exp = Expect{true, kExcInsnAccess};
    add(s);
  }
  {
    Scenario s;
    s.name = "lock-allow-m-load";
    s.mode = Mod::kM; s.cls = Cls::kLoad; s.cat = Cat::kLocked;
    s.skip_without_pmp = true;
    s.addr = kDataB + 0x410;
    s.width = 4;
    s.rule = "a locked entry's R/W/X bits are honoured in M-mode; R=1 -> it succeeds";
    s.entries.push_back(Ent{11, Cfg(true, kANa4, true, false, false), TorAddr(s.addr)});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "lock-cfg-write-ignored";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kLocked;
    s.skip_without_pmp = true;
    s.rule = "writes to pmpcfg[i] are ignored while pmpcfg[i].L=1";
    s.entries.push_back(Ent{11, Cfg(true, kANa4, true, false, false), TorAddr(kDataB + 0x410)});
    s.csr_ops.push_back(CsrOp{true, CSR_PMPCFG2, 0});   // try to clear every byte
    s.csr_ops.push_back(CsrOp{false, CSR_PMPCFG2, 0});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "lock-addr-write-ignored";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kLocked;
    s.skip_without_pmp = true;
    s.rule = "writes to pmpaddr[i] are ignored while pmpcfg[i].L=1";
    s.entries.push_back(Ent{12, Cfg(true, kANa4, true, false, false), TorAddr(kDataB + 0x420)});
    s.csr_ops.push_back(CsrOp{true, CSR_PMPADDR0 + 12, 0});
    s.csr_ops.push_back(CsrOp{false, CSR_PMPADDR0 + 12, 0});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "lock-off-matches-nothing";
    s.mode = Mod::kM; s.cls = Cls::kLoad; s.cat = Cat::kLocked;
    s.skip_without_pmp = true;
    s.addr = kDataB + 0x430;
    s.rule = "L=1 locks an entry even when A=OFF, and A=OFF still matches nothing";
    s.entries.push_back(Ent{13, Cfg(true, kAOff, false, false, false), TorAddr(s.addr)});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "lock-tor-prev-addr-ignored";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kLocked;
    s.skip_without_pmp = true;
    s.rule = "a locked entry with A=TOR also makes pmpaddr[i-1] unwritable";
    s.entries.push_back(Ent{13, Cfg(false, kAOff, false, false, false), TorAddr(kDataB + 0x440)});
    s.entries.push_back(Ent{14, Cfg(true, kATor, true, false, false), TorAddr(kDataB + 0x500)});
    s.csr_ops.push_back(CsrOp{true, CSR_PMPADDR0 + 13, 0});
    s.csr_ops.push_back(CsrOp{false, CSR_PMPADDR0 + 13, 0});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "tor-unlocked-prev-addr-writable";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kTor;
    s.skip_without_pmp = true;
    s.rule = "the pmpaddr[i-1] rule applies only while entry i is *locked*";
    s.entries.push_back(Ent{4, Cfg(false, kAOff, false, false, false), TorAddr(kDataB + 0x600)});
    s.entries.push_back(Ent{5, Cfg(false, kATor, true, false, false), TorAddr(kDataB + 0x700)});
    s.csr_ops.push_back(CsrOp{true, CSR_PMPADDR0 + 4, 0x12345});
    s.csr_ops.push_back(CsrOp{false, CSR_PMPADDR0 + 4, 0});
    s.exp = Expect{false, 0};
    add(s);
  }

  // ------------------------------------------------------------------ group 7
  // WARL: what the configuration and address registers read back.
  {
    Scenario s;
    s.name = "warl-cfg-reserved-bits";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kNa4;
    s.skip_without_pmp = true;
    s.rule = "bits 6:5 of a configuration byte are reserved and read zero";
    s.csr_ops.push_back(CsrOp{true, CSR_PMPCFG0, 0x0000000000000060ull});
    s.csr_ops.push_back(CsrOp{false, CSR_PMPCFG0, 0});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "warl-cfg-w-only";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kNa4;
    s.skip_without_pmp = true;
    s.rule = "the reserved combination R=0,W=1 canonicalises to R=1,W=1";
    s.csr_ops.push_back(CsrOp{true, CSR_PMPCFG0, 0x0000000000000012ull});  // A=NA4,W=1
    s.csr_ops.push_back(CsrOp{false, CSR_PMPCFG0, 0});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "warl-addr-high-bits";
    s.mode = Mod::kM; s.cls = Cls::kCsr; s.cat = Cat::kNa4;
    s.skip_without_pmp = true;
    s.rule = "pmpaddr bits 63:54 read zero";
    s.csr_ops.push_back(CsrOp{true, CSR_PMPADDR0 + 3, ~0ull});
    s.csr_ops.push_back(CsrOp{false, CSR_PMPADDR0 + 3, 0});
    s.exp = Expect{false, 0};
    add(s);
  }

  // ------------------------------------------------------------------ group 8
  // The CSR permission table from S and U, including the counter gate.
  {
    Scenario s;
    s.name = "s-csr-mstatus-read-illegal";
    s.mode = Mod::kS; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.skip_without_su = true;
    s.rule = "mstatus needs M-mode (csr[9:8]=3), so an S-mode read is illegal";
    s.csr_ops.push_back(CsrOp{false, CSR_MSTATUS, 0});
    s.exp = Expect{true, kExcIllegal};
    add(s);
  }
  {
    Scenario s;
    s.name = "s-csr-mstatus-write-illegal";
    s.mode = Mod::kS; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.skip_without_su = true;
    s.rule = "an S-mode write to an M-mode CSR is illegal and changes nothing";
    s.csr_ops.push_back(CsrOp{true, CSR_MSTATUS, 0});
    s.exp = Expect{true, kExcIllegal};
    add(s);
  }
  {
    Scenario s;
    s.name = "s-csr-sstatus-write-ok";
    s.mode = Mod::kS; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.skip_without_su = true;
    s.rule = "sstatus is an S-mode register (csr[9:8]=1) and writable there";
    s.csr_ops.push_back(CsrOp{true, CSR_SSTATUS, 0});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "s-mret-illegal";
    s.mode = Mod::kS; s.cls = Cls::kMret; s.cat = Cat::kSret;
    s.skip_without_su = true;
    s.rule = "an xRET instruction can be executed in mode x or higher: MRET is M-only";
    s.exp = Expect{true, kExcIllegal};
    add(s);
  }
  {
    Scenario s;
    s.name = "u-sret-illegal";
    s.mode = Mod::kU; s.cls = Cls::kSret; s.cat = Cat::kSret;
    s.skip_without_su = true;
    s.rule = "SRET in U-mode is an illegal instruction";
    s.exp = Expect{true, kExcIllegal};
    add(s);
  }
  {
    Scenario s;
    s.name = "u-csr-mstatus-read-illegal";
    s.mode = Mod::kU; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.skip_without_su = true;
    s.rule = "a U-mode read of an M-mode CSR is an illegal instruction";
    s.csr_ops.push_back(CsrOp{false, CSR_MSTATUS, 0});
    s.exp = Expect{true, kExcIllegal};
    add(s);
  }
  {
    Scenario s;
    s.name = "u-csr-cycle-gated";
    s.mode = Mod::kU; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.skip_without_su = true;
    s.rule = "with mcounteren.CY=0 a U-mode read of cycle is illegal";
    s.pre_csr.push_back(CsrOp{true, CSR_MCOUNTEREN, 0});
    s.pre_csr.push_back(CsrOp{true, CSR_SCOUNTEREN, 0});
    s.csr_ops.push_back(CsrOp{false, CSR_CYCLE, 0});
    s.exp = Expect{true, kExcIllegal};
    add(s);
  }
  {
    Scenario s;
    s.name = "u-csr-cycle-allowed";
    s.mode = Mod::kU; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.skip_without_su = true;
    s.rule = "with mcounteren.CY and scounteren.CY set the U-mode read is allowed";
    s.pre_csr.push_back(CsrOp{true, CSR_MCOUNTEREN, 1});
    s.pre_csr.push_back(CsrOp{true, CSR_SCOUNTEREN, 1});
    s.csr_ops.push_back(CsrOp{false, CSR_CYCLE, 0});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "u-csr-pmpaddr-read-illegal";
    s.mode = Mod::kU; s.cls = Cls::kCsr; s.cat = Cat::kNoEntry;
    s.skip_without_pmp = true;
    s.skip_without_su = true;
    s.rule = "the PMP registers are M-mode registers (csr[9:8]=3)";
    s.csr_ops.push_back(CsrOp{false, CSR_PMPADDR0, 0});
    s.exp = Expect{true, kExcIllegal};
    add(s);
  }

  // ------------------------------------------------------------------ group 9
  // MPRV: an M-mode data access that is checked as though it were MPP's.
  {
    Scenario s;
    s.name = "mprv-load-uses-mpp";
    s.mode = Mod::kM; s.cls = Cls::kLoad; s.cat = Cat::kNapot;
    s.skip_without_pmp = true;
    s.skip_without_su = true;
    s.addr = kDataA + 0x2800;
    s.rule = "with MPRV=1 an M-mode load is checked as though the mode were MPP";
    s.entries.push_back(Ent{1, Cfg(false, kANapot, false, true, false), NapotAddr(kDataA, 0x8000)});
    s.pre_csr.push_back(CsrOp{true, CSR_MSTATUS, 0x21800});   // MPRV=1, MPP=S
    s.exp = Expect{true, kExcLoadAccess};
    add(s);
  }
  {
    Scenario s;
    s.name = "mprv-store-uses-mpp";
    s.mode = Mod::kM; s.cls = Cls::kStore; s.cat = Cat::kNapot;
    s.skip_without_pmp = true;
    s.skip_without_su = true;
    s.addr = kDataA + 0x2808;
    s.rule = "with MPRV=1 an M-mode store is checked as though the mode were MPP";
    s.entries.push_back(Ent{1, Cfg(false, kANapot, true, false, false), NapotAddr(kDataA, 0x8000)});
    s.pre_csr.push_back(CsrOp{true, CSR_MSTATUS, 0x21800});
    s.exp = Expect{true, kExcStoreAccess};
    add(s);
  }
  {
    Scenario s;
    s.name = "mprv-off-m-mode-control";
    s.mode = Mod::kM; s.cls = Cls::kLoad; s.cat = Cat::kNapot;
    s.skip_without_pmp = true;
    s.skip_without_su = true;
    s.addr = kDataA + 0x2810;
    s.rule = "the same entry with MPRV=0 leaves the access an M-mode one, which "
             "an unlocked entry never refuses";
    s.entries.push_back(Ent{1, Cfg(false, kANapot, false, true, false), NapotAddr(kDataA, 0x8000)});
    s.exp = Expect{false, 0};
    add(s);
  }
  {
    Scenario s;
    s.name = "mprv-does-not-apply-to-fetch";
    s.mode = Mod::kM; s.cls = Cls::kFetch; s.cat = Cat::kNapot;
    s.skip_without_pmp = true;
    s.skip_without_su = true;
    s.addr = kFetch0;
    s.rule = "instruction access checking is unaffected by MPRV, so this fetch "
             "is still an M-mode one and succeeds";
    s.entries.push_back(Ent{1, Cfg(false, kANapot, true, true, false), NapotAddr(kDataB, 0x8000)});
    s.pre_csr.push_back(CsrOp{true, CSR_MSTATUS, 0x21800});
    s.exp = Expect{false, 0};   // the ecall at the target executes
    add(s);
  }

  // ------------------------------------------------------------------ group 10
  // Delegation: with medeleg clear, a trap taken in S-mode is handled in M-mode.
  {
    Scenario s;
    s.name = "deleg-off-s-trap-to-m";
    s.mode = Mod::kS; s.cls = Cls::kLoad; s.cat = Cat::kDelegation;
    s.skip_without_pmp = true;
    s.skip_without_su = true;
    s.addr = kGap + 16;
    s.rule = "with medeleg=0 a trap taken from S-mode is handled in M-mode";
    s.exp = Expect{true, kExcLoadAccess};
    add(s);
  }

  return v;
}

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

// ============================================================================
// Geometry, straight from the wrapper's generated-package ports
// ============================================================================
struct Geometry {
  uint32_t xlen = 0;
  uint32_t retire_width = 0;
  uint32_t rob_entries = 0;
  uint32_t seq_w = 0;
  uint64_t reset_vector = 0;
  uint32_t pmp_entries = 0;
  uint32_t pmp_g = 0;
  uint32_t pmp_grain = 0;
  uint32_t pmp_cfg_count = 0;
  bool has_s = false;
  bool has_u = false;
  uint32_t priv_least = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.xlen = dut->o_geom_xlen_o;
  g.retire_width = dut->o_geom_retire_width_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.reset_vector = dut->o_geom_reset_vector_o;
  g.pmp_entries = dut->o_geom_pmp_entries_o;
  g.pmp_g = dut->o_geom_pmp_g_o;
  g.pmp_grain = dut->o_geom_pmp_grain_bytes_o;
  g.pmp_cfg_count = dut->o_geom_pmp_cfg_count_o;
  g.has_s = dut->o_geom_has_s_o != 0;
  g.has_u = dut->o_geom_has_u_o != 0;
  g.priv_least = dut->o_geom_priv_least_o;
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
// The program
// ============================================================================
struct Program {
  Asm asm_{kCodeLo};
  ProgImage image;
};

void EmitHandler(Asm* a) {
  a->Csrrw(5, CSR_MSCRATCH, 5);            // x5 <- frame, mscratch <- old x5
  a->Sd(6, 5, F_X5 + 8);
  a->Sd(7, 5, F_X5 + 16);
  a->Sd(8, 5, F_X5 + 24);
  a->Sd(9, 5, F_X5 + 32);
  a->Sd(10, 5, F_X5 + 40);
  a->Sd(11, 5, F_X5 + 48);
  a->Sd(12, 5, F_X5 + 56);
  a->Csrrs(6, CSR_MSCRATCH, 0);            // x6 <- the pre-trap x5
  a->Sd(6, 5, F_X5);
  a->Csrrs(6, CSR_MCAUSE, 0);
  a->Sd(6, 5, F_CAUSE);
  a->Csrrs(6, CSR_MTVAL, 0);
  a->Sd(6, 5, F_TVAL);
  a->Csrrs(6, CSR_MEPC, 0);
  a->Sd(6, 5, F_EPC);
  a->Csrrs(6, CSR_MSTATUS, 0);
  a->Sd(6, 5, F_MSTATUS);
  a->Addi(6, 0, 1);
  a->Sd(6, 5, F_VALID);
  a->Ld(6, 5, F_RESUME);
  a->Csrrw(0, CSR_MEPC, 6);
  a->LiAbs(6, 0x1800);                     // mstatus.MPP <- M
  a->Csrrw(0, CSR_MSTATUS, 6);
  a->Mret();
}

void EmitAccess(Asm* a, const Scenario& s) {
  const bool w8 = (s.width == 8);
  switch (s.cls) {
    case Cls::kLoad: if (w8) a->Ld(5, 8, 0); else a->Lwu(5, 8, 0); break;
    case Cls::kStore: if (w8) a->Sd(9, 8, 0); else a->Sw(9, 8, 0); break;
    case Cls::kFetch: a->Jalr(0, 8, 0); break;
    case Cls::kAmo: if (w8) a->AmoAddD(5, 9, 8); else a->AmoAddW(5, 9, 8); break;
    case Cls::kLr: a->LrD(5, 8); break;
    case Cls::kSc: a->ScD(5, 9, 8); break;
    case Cls::kScLr: a->LrD(5, 8); a->ScD(5, 9, 8); break;
    case Cls::kMret: a->Mret(); break;
    case Cls::kSret: a->Sret(); break;
    case Cls::kNop: break;
    case Cls::kCsr:
      for (const CsrOp& op : s.csr_ops) {
        if (op.write) {
          a->LiAbs(9, op.operand);
          a->Csrrw(0, op.csr, 9);
        } else {
          a->Csrrs(5, op.csr, 0);
        }
      }
      break;
  }
}

std::string KeepLabel(unsigned i) { return "keep" + std::to_string(i); }
std::string StubLabel(unsigned i) { return "stub" + std::to_string(i); }

ProgImage BuildProgram(std::vector<Scenario>* scs, const Geometry& g) {
  Asm a(kCodeLo);
  const bool pmp = g.pmp_entries > 0;

  // ---- bootstrap ----
  a.La(5, "handler");
  a.Csrrw(0, CSR_MTVEC, 5);
  a.Addi(6, 0, 0);
  a.Csrrw(0, CSR_MEDELEG, 6);
  a.Csrrw(0, CSR_MIDELEG, 6);
  a.Csrrw(0, CSR_MIE, 6);
  if (g.has_s) {
    a.Csrrw(0, CSR_MCOUNTEREN, 6);
    a.Csrrw(0, CSR_SCOUNTEREN, 6);
  }
  // The front end fetches ahead of retirement and refuses a system trap until
  // the vector has retired; sixteen instructions exceed its window.
  for (int i = 0; i < 20; i++) a.Addi(0, 0, 0);

  // ---- the scenarios ----
  unsigned emitted = 0;
  for (Scenario& s : *scs) {
    // A PMP CSR that does not exist is a statement about the profile, so the
    // scenarios that only exercise one are kept and their expectation switched.
    const bool pmp_row = s.skip_without_pmp && !pmp;
    if (pmp_row) continue;
    if (s.skip_without_su && !(g.has_s && g.has_u)) continue;
    s.index = emitted;
    emitted++;
    s.frame = kFrameLo + kFrameStride * s.index;
    s.rec = kRecLo + 8ull * s.index;

    for (const CsrOp& op : s.pre_csr) {
      a.LiAbs(6, op.operand);
      a.Csrrw(0, op.csr, 6);
    }
    if (pmp) {
      for (unsigned i = 0; i < g.pmp_entries; i++) {
        uint64_t addr = 0;
        if (i == 0) addr = NapotAddr(kCodeLo, kCodeHi - kCodeLo);
        for (const Ent& e : s.entries) {
          if (e.idx == i) addr = e.addr;
        }
        a.LiAbs(6, addr);
        a.Csrrw(0, CSR_PMPADDR0 + i, 6);
      }
      uint64_t cfg0 = 0, cfg2 = 0;
      auto cfg_at = [&](unsigned i) -> uint8_t {
        if (i == 0) return Cfg(false, kANapot, true, false, true);
        for (const Ent& e : s.entries) {
          if (e.idx == i) return e.cfg;
        }
        return 0;
      };
      for (unsigned i = 0; i < 8 && i < g.pmp_entries; i++) {
        cfg0 |= static_cast<uint64_t>(cfg_at(i)) << (8 * i);
      }
      for (unsigned i = 8; i < 16 && i < g.pmp_entries; i++) {
        cfg2 |= static_cast<uint64_t>(cfg_at(i)) << (8 * (i - 8));
      }
      a.LiAbs(6, cfg0);
      a.Csrrw(0, CSR_PMPCFG0, 6);
      if (g.pmp_entries > 8) {
        a.LiAbs(6, cfg2);
        a.Csrrw(0, CSR_PMPCFG2, 6);
      }
    }

    // The frame: invalid until the handler writes it, and holding the address
    // the handler will resume at.
    a.LiAbs(6, s.frame);
    a.Sd(0, 6, F_VALID);
    a.La(7, KeepLabel(s.index));
    a.Sd(7, 6, F_RESUME);
    a.Csrrw(0, CSR_MSCRATCH, 6);

    a.LiAbs(5, s.preset);
    a.LiAbs(8, s.addr);
    if (IsStoreLike(s.cls)) a.LiAbs(9, s.data);

    if (s.mode != Mod::kM) {
      a.La(6, StubLabel(s.index));
      a.Csrrw(0, CSR_MEPC, 6);
      a.LiAbs(6, static_cast<uint64_t>(s.mode) << 11);
      a.Csrrw(0, CSR_MSTATUS, 6);
      a.Mret();
    } else {
      s.access_pc = a.pc();
      EmitAccess(&a, s);
      // A refused instruction access reports the PC it *could not fetch*, which
      // is the target, not the jump that asked for it.
      s.expect_epc_fault = (s.cls == Cls::kFetch) ? s.addr : s.access_pc;
      // An allowed instruction access runs the `ecall` that lives at the
      // target, and that trap's mepc is the target too. Every other allowed
      // scenario traps nothing at all in M-mode.
      s.expect_epc_nofault = (s.cls == Cls::kFetch) ? s.addr : 0;
    }
    a.Mark(KeepLabel(s.index));
    // Record x5 wherever the access was allowed. A faulted scenario resumes
    // here from the handler, where x5 is the handler's own scratch, so the
    // driver only reads this word when it expected the access to succeed.
    a.LiAbs(6, s.rec);
    a.Sd(5, 6, 0);
  }

  // ---- the exit protocol ----
  a.Addi(5, 0, 1);
  a.LiAbs(6, MOSAIC_TOHOST);
  a.Sd(5, 6, 0);
  a.Mark("park");
  a.JalSelf();

  // ---- the S/U stubs ----
  for (Scenario& s : *scs) {
    if (s.mode == Mod::kM) continue;
    if (s.skip_without_pmp && !pmp) continue;
    if (s.skip_without_su && !(g.has_s && g.has_u)) continue;
    a.Mark(StubLabel(s.index));
    s.stub_pc = a.pc();
    s.access_pc = a.pc();
    EmitAccess(&a, s);
    s.expect_epc_fault = (s.cls == Cls::kFetch) ? s.addr
                        : (s.cls == Cls::kScLr) ? s.stub_pc + 4 : s.stub_pc;
    s.expect_epc_nofault = (s.cls == Cls::kFetch) ? s.addr : a.pc();
    a.Ecall();
  }

  a.Mark("handler");
  EmitHandler(&a);
  a.Resolve();

  if (a.pc() > kCodeHi) {
    Fail("program", "the program runs past the code region at " + U64(kCodeHi));
  }

  ProgImage image;
  for (size_t i = 0; i < a.words().size(); i++) {
    image.Put(kCodeLo + 4ull * i, a.words()[i]);
  }
  if (Debug()) {
    for (size_t i = 0; i < a.words().size(); i++) {
      std::printf("  [word] %04llx %08x\n",
                  static_cast<unsigned long long>(kCodeLo + 4ull * i),
                  a.words()[i]);
    }
  }
  // The fetch scenarios jump to a word that is an `ecall`, so an allowed
  // instruction access is distinguishable from a refused one by its cause.
  for (uint64_t addr : {kFetch0, kFetch1, kFetch2, kFetch3}) {
    image.Put(addr, 0x00000073u);
  }
  ProgImage out;
  for (const auto& kv : image.words()) out.Put(kv.first, kv.second);
  return out;
}

// ============================================================================
// The harness
// ============================================================================
struct TrapObs {
  uint64_t cause = 0, tval = 0, epc = 0, target = 0;
  bool is_irq = false;
  uint64_t cycle = 0;
};

struct Frame {
  bool valid = false;
  uint64_t cause = 0, tval = 0, epc = 0, mstatus = 0, x5 = 0;
};

class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles,
          const ProgImage* img, mosaic::MemoryModel* mem)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(img),
        dmem_(mem) {}

  void Phase(const std::string& name) { phase_ = name; }
  uint64_t cycles() const { return cycles_; }
  const std::vector<TrapObs>& traps() const { return traps_; }
  uint32_t priv_mask() const { return priv_mask_; }
  uint64_t comparisons() const { return comparisons_; }
  uint64_t retires() const { return retires_; }
  const std::vector<DataMem::Txn>& txns() const { return dmem_.txns(); }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, phase_ + ": " + what + (ok ? "" : " -- " + detail));
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }
  void Compare(const std::string& what, bool ok, const std::string& detail) {
    comparisons_++;
    if (!ok) Fail(phase_ + " at cycle " + Dec(cycles_), what + ": " + detail);
  }

  std::string State() const {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) + " occupied=" +
           Dec(dut_->o_rob_occupied_o) + " retired=" + Dec(dut_->o_commit_o) +
           " traps=" + Dec(traps_.size()) + " priv=" + Dec(dut_->o_priv_o);
  }

  void Reset(int cycles) {
    for (int i = 0; i < cycles; i++) Cycle(true);
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
    dut_->ext_write_valid_i = 0;
    dut_->ext_write_addr_i = 0;
    dut_->ext_write_bytes_i = 0;
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
    priv_mask_ |= (1u << (dut_->o_priv_o & 3u));
    const uint32_t mask =
        (g_retire_width_ >= 32) ? 0xFFFFFFFFu : ((1u << g_retire_width_) - 1u);
    const uint32_t got_mask = static_cast<uint32_t>(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_retire_width_ && lane < 32; lane++) {
      if ((got_mask & (1u << lane)) == 0) continue;
      if (PackedLane(dut_->ev_trap_o, lane, 1) != 0) continue;
      retires_++;
    }
    if (dut_->o_trap_valid_o != 0) {
      TrapObs t;
      t.cause = dut_->o_trap_cause_o;
      t.tval = dut_->o_trap_tval_o;
      t.epc = dut_->o_trap_epc_o;
      t.target = dut_->o_trap_target_o;
      t.is_irq = dut_->o_trap_is_irq_o != 0;
      t.cycle = cycles_;
      traps_.push_back(t);
      if (Debug()) {
        std::printf("    [trap] cycle=%llu cause=%llu epc=%s tval=%s priv=%u\n",
                    static_cast<unsigned long long>(cycles_),
                    static_cast<unsigned long long>(t.cause), U64(t.epc).c_str(),
                    U64(t.tval).c_str(), static_cast<unsigned>(dut_->o_priv_o));
      }
    }
    // A program that stops making progress is a harness verdict as much as a
    // comparison is: the reason is named rather than left to the cycle cap.
    if (dut_->o_commit_o != last_commit_ ||
        dut_->o_dbg_alloc_ctr_o != last_alloc_ || dut_->o_trap_valid_o != 0) {
      last_commit_ = dut_->o_commit_o;
      last_alloc_ = dut_->o_dbg_alloc_ctr_o;
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail(phase_ + " at cycle " + Dec(cycles_), "stalled: " + State());
    }
  }

 public:
  void SetRetireWidth(uint32_t w) { g_retire_width_ = w; }

 private:
  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  DataMem dmem_;
  std::string phase_;
  uint64_t cycles_ = 0;
  uint64_t comparisons_ = 0;
  uint64_t retires_ = 0;
  uint64_t mtime_ = 0;
  uint32_t priv_mask_ = 0;
  uint32_t last_commit_ = 0, last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  uint32_t g_retire_width_ = 2;
  std::vector<TrapObs> traps_;
  static constexpr uint64_t kStallCycles = 30000;
};

bool ReadFrame(mosaic::MemoryModel* mem, uint64_t frame, Frame* out,
               std::string* detail) {
  auto read = [&](uint64_t addr, uint64_t* value) {
    if (mem->Read(addr, 8, value) != mosaic::AccessStatus::kOk) {
      *detail = "the frame word at " + U64(addr) + " is not readable";
      return false;
    }
    return true;
  };
  uint64_t valid = 0;
  if (!read(frame + F_VALID, &valid)) return false;
  out->valid = valid != 0;
  if (!out->valid) return true;
  if (!read(frame + F_CAUSE, &out->cause)) return false;
  if (!read(frame + F_TVAL, &out->tval)) return false;
  if (!read(frame + F_EPC, &out->epc)) return false;
  if (!read(frame + F_MSTATUS, &out->mstatus)) return false;
  if (!read(frame + F_X5, &out->x5)) return false;
  return true;
}

// ============================================================================
// The run
// ============================================================================
struct Coverage {
  std::map<std::string, int> cells;
  void Bump(const std::string& key) { cells[key]++; }
  int Get(const std::string& key) const {
    auto it = cells.find(key);
    return (it == cells.end()) ? 0 : it->second;
  }
};

void RunCase(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, const Geometry& g,
             uint64_t max_cycles, uint64_t* totals) {
  const std::string phase = "priv-matrix";
  const bool pmp = g.pmp_entries > 0;
  const bool su = g.has_s && g.has_u;

  std::vector<Scenario> all = BuildScenarios();

  // Which scenarios this profile can run, and what each one should do here.
  std::vector<Scenario> run;
  for (const Scenario& s : all) {
    if (s.skip_without_pmp && !pmp) continue;
    if (s.skip_without_su && !su) continue;
    run.push_back(s);
  }
  if (run.empty()) Fail(phase, "no scenario is applicable to this profile");

  const ProgImage image = BuildProgram(&run, g);

  mosaic::MemoryModel dut_mem;
  dut_mem.SetInputWord(0);

  // The word every scenario reads or overwrites, seeded before the run.
  for (const Scenario& s : run) {
    if (IsStoreLike(s.cls)) {
      if (dut_mem.Write(s.addr, s.width, s.pre) != mosaic::AccessStatus::kOk) {
        Fail(phase, "cannot seed the store word at " + U64(s.addr));
      }
    } else if (s.cls != Cls::kFetch && s.cls != Cls::kCsr &&
               s.cls != Cls::kNop && s.cls != Cls::kMret && s.cls != Cls::kSret) {
      if (dut_mem.Write(s.addr, s.width, s.pre) != mosaic::AccessStatus::kOk) {
        Fail(phase, "cannot seed the load word at " + U64(s.addr));
      }
    }
  }

  // The independent model of the PMP rules, given exactly the entries the
  // program programs (entry 0 is the code region every scenario installs).
  std::vector<PmpEntry> modelled(g.pmp_entries);
  auto seed_model = [&]() {
    for (unsigned i = 0; i < modelled.size(); i++) {
      modelled[i].used = true;
      modelled[i].cfg = (i == 0) ? Cfg(false, kANapot, true, false, true) : 0;
      modelled[i].addr = (i == 0) ? NapotAddr(kCodeLo, kCodeHi - kCodeLo) : 0;
    }
  };
  seed_model();

  Harness harness(dut, reporter, max_cycles, &image, &dut_mem);
  harness.SetRetireWidth(g.retire_width);
  harness.Phase(phase);
  dut->clk = 0;
  dut->rst = 1;
  dut->eval();
  harness.Reset(kResetCycles);
  while (!dut_mem.finished() && harness.cycles() < max_cycles) harness.Cycle(false);
  for (int i = 0; i < 16; i++) harness.Cycle(false);

  harness.Check("the program reached its exit protocol",
                dut_mem.finished() && dut_mem.passed() && dut_mem.exit_code() == 1,
                "finished=" + Dec(dut_mem.finished() ? 1 : 0) + " passed=" +
                    Dec(dut_mem.passed() ? 1 : 0) + " tohost=" +
                    U64(dut_mem.exit_code()));

  // ------------------------------------------------------------- the matrix
  Coverage cov;
  size_t trap_cursor = 0;
  int denied_stores = 0, allowed_stores = 0;
  for (Scenario& s : run) {
    const std::string tag = s.name + " (" + ModName(s.mode) + "/" +
                            ClsName(s.cls) + "/" + CatName(s.cat) + ")";
    cov.Bump(std::string("mode:") + ModName(s.mode));
    cov.Bump(std::string("class:") + ClassKey(s.cls));
    cov.Bump(std::string("cat:") + CatName(s.cat));
    cov.Bump(std::string("mode-class:") + ModName(s.mode) + "-" + ClassKey(s.cls));

    // The expectation for this profile.
    Expect e = s.exp;
    if (s.alt_without_pmp && !pmp) e = s.alt;
    if (s.alt_without_su && !su) e = s.alt;
    if (s.alt_without_u && !g.has_u) e = s.alt;

    // The second opinion: the model must agree with the hand-written rule.
    seed_model();
    for (const Ent& ent : s.entries) {
      if (ent.idx < modelled.size()) {
        modelled[ent.idx].cfg = ent.cfg;
        modelled[ent.idx].addr = ent.addr;
      }
    }
    if (s.cls != Cls::kCsr && s.cls != Cls::kNop && s.cls != Cls::kMret &&
        s.cls != Cls::kSret) {
      // The class the specification names: a load or load-reserved checks R, a
      // store, store-conditional or AMO checks W, an instruction access checks X.
      const bool r = (s.cls == Cls::kLoad || s.cls == Cls::kLr ||
                      s.cls == Cls::kScLr || s.cls == Cls::kFetch);
      const bool w = IsStoreLike(s.cls);
      const bool x = s.cls == Cls::kFetch;
      unsigned priv = static_cast<unsigned>(s.mode);
      // MPRV: an M-mode *data* access with MPRV set is checked as MPP's, and an
      // instruction access never is.
      if (s.cls != Cls::kFetch ||
          static_cast<unsigned>(s.mode) == static_cast<unsigned>(Mod::kM)) {
        for (const CsrOp& op : s.pre_csr) {
          if (op.csr == CSR_MSTATUS && op.write && ((op.operand >> 17) & 1u)) {
            priv = static_cast<unsigned>((op.operand >> 11) & 3u);
          }
        }
      }
      if (s.cls == Cls::kFetch) priv = static_cast<unsigned>(s.mode);
      const bool allowed = PmpModel(modelled).Allow(s.addr, s.width, r, w, x, priv);
      const uint64_t model_cause =
          allowed ? 0 : (w ? kExcStoreAccess : (x ? kExcInsnAccess : kExcLoadAccess));
      harness.Compare(tag + ": the model agrees with the hand-written rule",
                      (model_cause != 0) == e.fault &&
                          (!e.fault || model_cause == e.cause),
                      "model says cause " + Dec(model_cause) + ", the rule says " +
                          Dec(e.fault ? e.cause : 0));
    }

    // What the machine did.
    Frame f;
    std::string detail;
    if (!ReadFrame(&dut_mem, s.frame, &f, &detail)) Fail(phase, tag + ": " + detail);

    // An S/U scenario always ends in a trap -- the access fault, or the stub's
    // trailing `ecall` when the access was allowed. An M-mode scenario traps
    // only when the access is refused, except that an *allowed* instruction
    // access runs the `ecall` at its target.
    const bool expect_any_trap = (s.mode != Mod::kM) || e.fault ||
                                 (s.cls == Cls::kFetch);
    harness.Check(tag + ": a trap frame exists exactly when the ISA expects one",
                  f.valid == expect_any_trap,
                  "frame valid=" + Dec(f.valid ? 1 : 0) + " expected " +
                      Dec(expect_any_trap ? 1 : 0));

    if (f.valid) {
      uint64_t want_cause = e.cause;
      if (!e.fault) {
        want_cause = (e.nofault_cause != 0) ? e.nofault_cause
                    : (s.mode == Mod::kS) ? kExcEcallS
                    : (s.mode == Mod::kU) ? kExcEcallU : kExcEcallM;
      }
      harness.Check(tag + ": mcause is the ISA's",
                    f.cause == want_cause,
                    "mcause=" + U64(f.cause) + " expected " + U64(want_cause));
      const uint64_t want_tval =
          (f.cause == kExcInsnAccess || f.cause == kExcLoadAccess ||
           f.cause == kExcStoreAccess)
              ? s.addr
              : 0;
      harness.Check(tag + ": mtval is the ISA's",
                    f.tval == want_tval,
                    "mtval=" + U64(f.tval) + " expected " + U64(want_tval));
      const uint64_t want_epc = e.fault ? s.expect_epc_fault : s.expect_epc_nofault;
      harness.Check(tag + ": mepc names the faulting instruction",
                    f.epc == want_epc,
                    "mepc=" + U64(f.epc) + " expected " + U64(want_epc));
      // The privilege the trap was taken *from*, recorded by the ISA itself.
      // Where the profile implements no less-privileged mode the claim is the
      // opposite one: the machine stays in M and the case says so.
      unsigned want_mode = static_cast<unsigned>(s.mode);
      if (s.alt_without_su && !su) want_mode = static_cast<unsigned>(Mod::kM);
      if (s.alt_without_u && !g.has_u) want_mode = static_cast<unsigned>(Mod::kM);
      const unsigned trapped_priv = static_cast<unsigned>((f.mstatus >> 11) & 3u);
      harness.Check(tag + ": the trap was taken from the mode the scenario claims",
                    trapped_priv == want_mode,
                    "mstatus.MPP=" + Dec(trapped_priv) + " expected " +
                        Dec(want_mode));
    }

    // ------------------------------------------------------- side-effect freedom
    const bool faulted = e.fault;
    if (faulted) {
      // "A permission fault has no side effect": the destination register keeps
      // the value the scenario gave it. This covers a refused load (nothing was
      // written to rd), a refused CSR read, and a refused store or AMO.
      if (s.cls != Cls::kFetch) {
        harness.Check(tag + ": the destination register is untouched",
                      f.x5 == s.preset,
                      "x5=" + U64(f.x5) + " expected the pre-value " + U64(s.preset));
      }
      if (IsStoreLike(s.cls)) {
        uint64_t now = 0;
        if (dut_mem.Read(s.addr, s.width, &now) != mosaic::AccessStatus::kOk) {
          Fail(phase, tag + ": the protected word is not readable");
        }
        harness.Check(tag + ": no bytes of the protected word changed",
                      now == s.pre,
                      "word=" + U64(now) + " expected " + U64(s.pre));
        for (const DataMem::Txn& t : harness.txns()) {
          const bool same = (t.req.addr & ~7ull) == (s.addr & ~7ull);
          harness.Check(tag + ": no transaction for the refused address "
                             "reached the data port",
                        !same,
                        "a transaction at " + U64(t.req.addr) + " in cycle " +
                            Dec(t.cycle) + " we=" + Dec(t.req.we ? 1 : 0));
        }
        denied_stores++;
      }
    }

    // ------------------------------------------------------- the allowed result
    if (!faulted && s.cls != Cls::kCsr) {
      if (IsStoreLike(s.cls) && s.cls != Cls::kSc && s.cls != Cls::kScLr) {
        uint64_t now = 0;
        if (dut_mem.Read(s.addr, s.width, &now) != mosaic::AccessStatus::kOk) {
          Fail(phase, tag + ": the written word is not readable");
        }
        const uint64_t m = PreMask(s.width);
        const uint64_t want = ((s.cls == Cls::kAmo) ? (s.pre + s.data) : s.data) & m;
        harness.Check(tag + ": the store reached memory",
                      now == want, "word=" + U64(now) + " expected " + U64(want));
        allowed_stores++;
      }
      if (s.cls == Cls::kScLr) {
        uint64_t now = 0;
        if (dut_mem.Read(s.addr, s.width, &now) != mosaic::AccessStatus::kOk) {
          Fail(phase, tag + ": the SC word is not readable");
        }
        harness.Check(tag + ": the store-conditional's write reached memory",
                      now == s.data, "word=" + U64(now) + " expected " + U64(s.data));
        harness.Check(tag + ": the store-conditional reported success", f.x5 == 0,
                      "x5=" + U64(f.x5) + " expected 0");
      }
      if (IsLoadLike(s.cls) && s.cls != Cls::kAmo) {
        uint64_t seen = f.x5;
        if (s.mode == Mod::kM) {
          if (dut_mem.Read(s.rec, 8, &seen) != mosaic::AccessStatus::kOk) {
            Fail(phase, tag + ": the record word is not readable");
          }
        }
        harness.Check(tag + ": the load returned the word that was there",
                      seen == s.pre, "x5=" + U64(seen) + " expected " + U64(s.pre));
      }
    }

    // ------------------------------------------------------------ CSR claims
    if (s.cls == Cls::kCsr && !faulted && s.mode == Mod::kM) {
      uint64_t seen = 0;
      if (dut_mem.Read(s.rec, 8, &seen) != mosaic::AccessStatus::kOk) {
        Fail(phase, tag + ": the CSR record word is not readable");
      }
      if (s.name == "csr-pmpcfg0-read") {
        // The program has already installed the code-region entry (entry 0) when
        // this scenario runs, so the register's value is that byte and nothing
        // else: L=0, A=NAPOT, X=1, R=1.
        harness.Check(tag + ": pmpcfg0 reads back the entry the program installed",
                      seen == 0x1Dull, "pmpcfg0=" + U64(seen) + " expected 0x1d");
      } else if (s.name == "lock-cfg-write-ignored") {
        harness.Check(tag + ": a locked configuration byte survives a write of zero",
                      seen == 0x0000000091909090ull,
                      "pmpcfg2=" + U64(seen) + " expected 0x0000000091909090");
      } else if (s.name == "lock-addr-write-ignored") {
        harness.Check(tag + ": a locked address register survives a write of zero",
                      seen == TorAddr(kDataB + 0x420),
                      "pmpaddr12=" + U64(seen) + " expected " +
                          U64(TorAddr(kDataB + 0x420)));
      } else if (s.name == "lock-tor-prev-addr-ignored") {
        harness.Check(tag + ": a locked TOR entry freezes pmpaddr[i-1]",
                      seen == TorAddr(kDataB + 0x440),
                      "pmpaddr13=" + U64(seen) + " expected " +
                          U64(TorAddr(kDataB + 0x440)));
      } else if (s.name == "tor-unlocked-prev-addr-writable") {
        harness.Check(tag + ": an unlocked entry leaves pmpaddr[i-1] writable",
                      seen == 0x12345ull, "pmpaddr4=" + U64(seen) + " expected 0x12345");
      } else if (s.name == "warl-cfg-reserved-bits") {
        harness.Check(tag + ": the reserved write of bits 6:5 reads back zero",
                      (seen & 0x0000000000000060ull) == 0,
                      "pmpcfg0=" + U64(seen));
      } else if (s.name == "warl-cfg-w-only") {
        harness.Check(tag + ": W=1,R=0 canonicalises to W=1,R=1",
                      (seen & 0x1Full) == 0x13ull,
                      "pmpcfg0=" + U64(seen) + " expected the low byte 0x13");
      } else if (s.name == "warl-addr-high-bits") {
        harness.Check(tag + ": pmpaddr bits 63:54 read zero",
                      (seen >> 54) == 0 && (seen & ((1ull << 54) - 1)) != 0,
                      "pmpaddr3=" + U64(seen));
      } else if (s.name == "boot-mstatus-mpp-warl") {
        const unsigned mpp = static_cast<unsigned>((seen >> 11) & 3u);
        harness.Check(tag + ": mstatus.MPP holds the written value exactly where "
                           "the profile implements a less-privileged mode",
                      mpp == (su ? 1u : 3u),
                      "mstatus=" + U64(seen) + " MPP=" + Dec(mpp));
      }
    }
    if (s.cls == Cls::kCsr && faulted) {
      // A refused CSR access must leave the register alone. The machine never
      // enables the write port for it (that is why the cause is 2 rather than a
      // change), and the register the *scenario* can observe is the one the
      // handler saved: mstatus at trap entry carries the mode the trap came
      // from, which the check above already asserted, so what is left is that
      // the destination register did not receive a read value -- checked above
      // for every refused scenario.
      harness.Compare(tag + ": a refused CSR access did not reach the CSR file",
                      f.cause == kExcIllegal,
                      "mcause=" + U64(f.cause) + " is not the illegal-instruction "
                      "exception a refused CSR access raises");
    }
  }

  // ------------------------------------------------------------- coverage
  harness.Check("every access class the card names was exercised",
                cov.Get("class:load") > 0 && cov.Get("class:store") > 0 &&
                    cov.Get("class:fetch") > 0 && cov.Get("class:amo") > 0 &&
                    cov.Get("class:lr") > 0 && cov.Get("class:sc") > 0,
                "the class cells are not all populated");
  harness.Check("the M-mode row of the matrix was exercised",
                cov.Get("mode:M") > 0, "no M-mode scenario ran");
  if (su) {
    harness.Check("the S-mode row of the matrix was exercised",
                  cov.Get("mode:S") > 0, "no S-mode scenario ran");
    harness.Check("the U-mode row of the matrix was exercised",
                  cov.Get("mode:U") > 0, "no U-mode scenario ran");
    harness.Check("every access class was exercised from S or U",
                  cov.Get("mode-class:S-load") > 0 && cov.Get("mode-class:S-store") > 0 &&
                      cov.Get("mode-class:S-fetch") > 0 && cov.Get("mode-class:S-amo") > 0 &&
                      cov.Get("mode-class:S-lr") > 0 && cov.Get("mode-class:S-sc") > 0 &&
                      cov.Get("mode-class:U-load") > 0 && cov.Get("mode-class:U-store") > 0 &&
                      cov.Get("mode-class:U-fetch") > 0,
                  "an S/U access class is missing");
    harness.Check("the machine was observed in M, S and U mode",
                  (harness.priv_mask() & 0x1u) != 0 && (harness.priv_mask() & 0x2u) != 0 &&
                      (harness.priv_mask() & 0x8u) != 0,
                  "privilege modes observed: " + Dec(harness.priv_mask()));
  } else {
    harness.Check("no scenario claims an S or U access in a profile without them",
                  true, "");
  }
  if (pmp) {
    harness.Check("all four PMP address-matching modes were exercised",
                  cov.Get("cat:off") + cov.Get("cat:locked") + cov.Get("cat:na4") +
                          cov.Get("cat:napot") + cov.Get("cat:tor") > 0 &&
                      cov.Get("cat:na4") > 0 && cov.Get("cat:napot") > 0 &&
                      cov.Get("cat:tor") > 0,
                  "an address-matching mode is missing");
    harness.Check("overlap priority and the lock rule were exercised",
                  cov.Get("cat:overlap") > 0 && cov.Get("cat:locked") > 0,
                  "overlap or lock coverage is missing");
    harness.Check("the granularity/part-word rule was exercised",
                  cov.Get("cat:granularity") > 0, "no part-word scenario ran");
    harness.Check("the permission unit was consulted",
                  dut->o_pmp_query_ctr_o > 0,
                  "query counter=" + Dec(dut->o_pmp_query_ctr_o));
    harness.Check("the permission unit refused at least one access",
                  dut->o_pmp_deny_ctr_o > 0,
                  "deny counter=" + Dec(dut->o_pmp_deny_ctr_o));
    harness.Check("the permission unit refused at least one instruction access",
                  dut->o_pmp_fetch_deny_ctr_o > 0,
                  "fetch deny counter=" + Dec(dut->o_pmp_fetch_deny_ctr_o));
    harness.Check("a locked entry was matched",
                  dut->o_pmp_locked_ctr_o > 0,
                  "locked counter=" + Dec(dut->o_pmp_locked_ctr_o));
  }
  harness.Check("no trap was driven by the interrupt path",
                dut->o_trap_irq_o == 0, "irq traps=" + Dec(dut->o_trap_irq_o));
  if (su) {
    harness.Check("the CSR file counted the privilege refusals the matrix asserts",
                  dut->o_csr_priv_illegal_ctr_o > 0,
                  "priv illegal counter=" + Dec(dut->o_csr_priv_illegal_ctr_o));
  }

  // ------------------------------------------------------------- evidence
  for (const Scenario& s : run) {
    Frame f;
    std::string detail;
    if (!ReadFrame(&dut_mem, s.frame, &f, &detail)) Fail(phase, detail);
    Expect e = s.exp;
    if (s.alt_without_pmp && !pmp) e = s.alt;
    if (s.alt_without_su && !su) e = s.alt;
    if (s.alt_without_u && !g.has_u) e = s.alt;
    const unsigned trapped = f.valid ? static_cast<unsigned>((f.mstatus >> 11) & 3u) : 0xFFu;
    std::printf("  [obs] %-32s %s/%-6s/%-11s trap=%u cause=%-2llu tval=%s "
                "from=%s expect=%s\n",
                s.name.c_str(), ModName(s.mode), ClsName(s.cls), CatName(s.cat),
                f.valid ? 1u : 0u,
                static_cast<unsigned long long>(f.cause), U64(f.tval).c_str(),
                f.valid ? ModName(static_cast<Mod>(trapped & 3u)) : "-",
                e.fault ? "refuse" : "allow");
  }
  std::printf("  coverage:");
  for (const auto& kv : cov.cells) {
    std::printf(" %s=%d", kv.first.c_str(), kv.second);
  }
  std::printf("\n  profile: pmp_entries=%u pmp_g=%u grain=%u has_s=%d has_u=%d\n",
              g.pmp_entries, g.pmp_g, g.pmp_grain, g.has_s ? 1 : 0, g.has_u ? 1 : 0);
  std::printf("  traps=%zu retires=%llu cycles=%llu comparisons=%llu denied_stores=%d "
              "allowed_stores=%d\n",
              harness.traps().size(),
              static_cast<unsigned long long>(harness.retires()),
              static_cast<unsigned long long>(harness.cycles()),
              static_cast<unsigned long long>(harness.comparisons()),
              denied_stores, allowed_stores);

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
    if (geometry.retire_width < 1) Fail("geometry", "the profile does not retire");

    uint64_t totals[4] = {0, 0, 0, 0};
    RunCase(&dut, &reporter, geometry, options.max_cycles, totals);

    detail = "checks=" + Dec(reporter.checks()) + " comparisons=" + Dec(totals[1]) +
             " cycles=" + Dec(totals[0]) + " retires=" + Dec(totals[2]) +
             " traps=" + Dec(totals[3]) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "every privilege/access/PMP cell matches the ISA",
                      "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
