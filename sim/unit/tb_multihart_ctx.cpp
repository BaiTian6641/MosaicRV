// ============================================================================
// tb_multihart_ctx.cpp -- CASE=multihart.context_isolation, work package V-064.
//
// The card: in the two-hart configuration, interleave the SAME PC with DIFFERENT
// per-hart GPR, privilege, ASID, FP and vector state; trigger trap, IRQ, WFI and
// halt separately per hart; the reference instance must have independent state
// and a defined shared memory. Pass: each hart's PC/rename/commit/trap/CSR state
// is independent, `mhartid` is stable, and one hart stopping does not implicitly
// stop the others. Fail: treating several software threads as one RVV hart, a
// shared reference register state, or inventing a global retire order out of
// host scheduling.
//
// ------------------------------------------------------------------ the design
//
// Both harts reset to the SAME physical PC and fetch the SAME instruction
// stream. The only thing that differs at that PC is each hart's own state, and
// the first thing the program reads is `mhartid` -- which this package made
// per-hart (mosaic_csr's MHARTID_VALUE, wired from mosaic_core's HART_ID). Every
// later difference (the ASID it installs, the FP flags its divide raises, the
// vector length it selects, whether its translated load faults, whether it takes
// an interrupt, whether it halts in WFI) is derived by the same instructions
// from that one per-hart value. That is what makes the interleaving real: a
// cell is interesting only if the same PC retires with different state on the
// two harts, and here every cell does.
//
// ------------------------------------------------------------- the references
//
// Each hart's reference is its OWN solo run of the same image from a clean reset:
//
//   phase A  hart_en = 01   hart 0 alone   -> reference 0
//   phase B  hart_en = 10   hart 1 alone   -> reference 1
//   phase C  hart_en = 11   both together
//
// The two references are two separate observations with two separate register
// state pictures; there is deliberately no single shared reference register
// file, which is the card's named "shared reference register state" failure mode.
// The concurrent run's stream for each hart must equal that hart's own solo
// stream, field for field, and each hart's independently computed architectural
// results (FP result, ASID, vector bytes) must equal values derived from the
// hart's own id.
//
// ------------------------------------------------------------- what is checked
//
//   1. both harts reset to the same PC and their first retire PC is equal;
//   2. `mhartid` is per-hart and STABLE: each hart's two reads agree with each
//      other, hart 0 reads 0, hart 1 reads 1, and the two values differ;
//   3. same PC, different GPR: the same `csrr mhartid` retires with a different
//      destination value on each hart;
//   4. ASID/CSR: the two satp values differ and each carries its own hart's
//      ASID; the S-mode read-back matches the port;
//   5. privilege/CSR: both end in S-mode and their mstatus values differ, each
//      carrying its own hart's SUM bit;
//   6. FP: the same `fdiv.d` on the same PC raises different flags and produces
//      different results per hart (0/0 -> qNaN/NV, 1/0 -> +inf/DZ), and the
//      architectural fcsr differs and matches the expected value;
//   7. vector: the same `vsetvli` selects a different vl per hart, and the same
//      vector add+store writes per-hart bytes matching an independent model;
//   8. trap: the same translated load at the same PC faults on hart 0 (its page
//      is invalid) and succeeds on hart 1 (its page is mapped), with hart 1
//      taking no trap at all;
//   9. IRQ: hart 0 takes a timer interrupt it raised on itself; hart 1 takes no
//      interrupt;
//  10. WFI/halt: hart 0 halts in WFI; hart 1 never does;
//  11. one hart stopping does not stop the other: hart 1 kept committing after
//      hart 0 halted, hart 1 parked, and each hart also completes its own program
//      with the other held in reset (the solo references);
//  12. per-hart reference: each hart's concurrent stream equals its own solo
//      stream, field for field;
//  13. ownership/sharing: no response was misrouted, both harts used the one
//      shared memory service and contended for it.
//
// -------------------------------------------------------------- the controls
//
// tools/run_multihart_context_controls.py rebuilds the case from an empty
// directory with exactly one defect and requires exit 1 with the named check:
//
//   MOSAIC_MUTANT_MHARTID_SHARED       both harts report the same mhartid
//   MOSAIC_MH_CTX_MUTANT_HALT_ALL      hart 0's WFI halt resets hart 1
//
// plus the driver-side `--control-share-ref`, which compares hart 1 against hart
// 0's reference -- the "shared reference register state" failure mode, made
// falsifiable.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "Vmosaic_multihart_ctx_tb.h"

#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic::AccessStatus;
using mosaic::MemoryModel;
using mosaic::Options;
using mosaic::Reporter;

namespace {

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw std::runtime_error(where + ": " + detail);
}

std::string U64(uint64_t v) { return mosaic::Hex(v); }
std::string Dec(uint64_t v) { return std::to_string(v); }

// ------------------------------------------------------------- physical layout
constexpr uint64_t kRamBase = MOSAIC_RAM_BASE;   // 0x8000_0000
// Both harts reset here (matching sim/tb/mosaic_multihart_ctx_tb.sv).
constexpr uint64_t kSamePc  = 0x80002000ull;
// The regions are spaced far enough apart that the (long) M-mode prologue cannot
// reach the S-mode body: the prologue is ~70 instructions, so 0x400 bytes is a
// safe gap and the builder asserts the fit below.
constexpr uint64_t kSEntry  = 0x80002400ull;     // S-mode body
constexpr uint64_t kHandler = 0x80002800ull;     // M-mode trap handler
// Page tables: root = kRootBase + hart*0x1000 (the program computes it).
constexpr uint64_t kRootBase = 0x80004000ull;
const uint64_t kL1[2] = {0x80006000ull, 0x80008000ull};
const uint64_t kL0[2] = {0x80007000ull, 0x80009000ull};
// Per-hart physical pages.
constexpr uint64_t kFpBase   = 0x8000A000ull;    // FP result store (M-mode)
constexpr uint64_t kVecABase = 0x8000C000ull;    // vector source (M-mode)
constexpr uint64_t kVecCBase = 0x8000E000ull;    // vector store  (M-mode)
constexpr uint64_t kSigBase  = 0x80010000ull;    // translated ASID store
constexpr uint64_t kTrapBase = 0x80012000ull;    // trap page (M-mode handler)
constexpr uint64_t kData1    = 0x80014000ull;    // hart 1's mapped trap-VA page
// Virtual addresses (Sv39; vpn2 = 1 for all three).
constexpr uint64_t kVaTrapBase = 0x40000000ull;  // + hart*0x1000
constexpr uint64_t kVaSig      = 0x40002000ull;  // mapped on both harts

// CSRs used by the program.
constexpr uint32_t kCsrMstatus = 0x300, kCsrMtvec = 0x305, kCsrMepc = 0x341,
                   kCsrMcause = 0x342, kCsrMtval = 0x343, kCsrSatp = 0x180,
                   kCsrMie = 0x304, kCsrMip = 0x344, kCsrSie = 0x104,
                   kCsrMhartid = 0xF14,
                   kCsrFcsr = 0x003, kCsrPmpcfg0 = 0x3A0, kCsrPmpaddr0 = 0x3B0;

// mstatus: MPP=S (bits 12:11 = 01), FS=Initial (14:13 = 01), VS=Initial
// (10:9 = 01). SUM (bit 18) is added per hart, so the two mstatus values differ.
constexpr uint64_t kMstatusBase = (1ull << 11) | (1ull << 13) | (1ull << 9);
constexpr unsigned kSumBit = 18;

constexpr uint32_t kExcLoadPage = 13;
// mcause for a machine software interrupt (the pending bit hart 0 raises on
// itself in the prologue).
constexpr uint64_t kIrqSwCause = 0x8000000000000003ull;

// The expected FP results, written from the ISA rather than from the host FPU
// (the host's 0/0 NaN is signed differently): RISC-V's canonical quiet NaN is
// 0x7FF8_0000_0000_0000, and 1.0/0.0 is +inf.
constexpr uint64_t kFpHart0 = 0x7FF8000000000000ull;
constexpr uint64_t kFpHart1 = 0x7FF0000000000000ull;
// fcsr = fflags | (frm << 5). hart 0: frm=0, NV=0x10. hart 1: frm=1, DZ=0x08.
constexpr uint64_t kFcsrHart0 = 0x10ull;
constexpr uint64_t kFcsrHart1 = 0x28ull;
// Vector: hart 0 avl = 2*0+1 = 1, hart 1 avl = 2*1+1 = 3 (e32, m1 -> VLMAX 4).
constexpr uint64_t kVlHart0 = 1, kVlHart1 = 3;
// The vector source pages, seeded by the driver; the store is 2x the source.
const uint32_t kVecA[2][4] = {{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
                              {0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u}};
constexpr uint64_t kData1Value = 0xDEADBEEFCAFEF00Dull;
// The bounded work loop hart 1 runs before parking: long enough that hart 1 is
// still executing when hart 0's (shorter) program halts in WFI.
constexpr uint32_t kHart1WorkIters = 4000;

// --------------------------------------------------------------- instruction
uint32_t EncR(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd,
              uint32_t op) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
uint32_t EncI(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return ((static_cast<uint32_t>(imm) & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) |
         (rd << 7) | op;
}
uint32_t EncS(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0xFFF;
  return ((u >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
         ((u & 0x1F) << 7) | op;
}
uint32_t EncB(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0x1FFF;
  return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25) | (rs2 << 20) |
         (rs1 << 15) | (f3 << 12) | (((u >> 1) & 0xF) << 8) |
         (((u >> 11) & 1) << 7) | op;
}
uint32_t EncJ(int32_t imm, uint32_t rd) {
  const uint32_t o = static_cast<uint32_t>(imm) & 0x1FFFFF;
  return (((o >> 20) & 1) << 31) | (((o >> 1) & 0x3FF) << 21) |
         (((o >> 11) & 1) << 20) | (((o >> 12) & 0xFF) << 12) | (rd << 7) | 0x6F;
}
uint32_t Lui(uint32_t rd, uint32_t imm20) { return (imm20 << 12) | (rd << 7) | 0x37; }
uint32_t Addi(uint32_t rd, uint32_t rs1, int32_t imm) { return EncI(imm, rs1, 0, rd, 0x13); }
uint32_t Slli(uint32_t rd, uint32_t rs1, uint32_t sh) {
  return EncI(static_cast<int32_t>(sh), rs1, 1, rd, 0x13);
}
uint32_t Srli(uint32_t rd, uint32_t rs1, uint32_t sh) {
  return EncI(static_cast<int32_t>(sh), rs1, 5, rd, 0x13);
}
uint32_t Add(uint32_t rd, uint32_t rs1, uint32_t rs2) { return EncR(0, rs2, rs1, 0, rd, 0x33); }
uint32_t Or(uint32_t rd, uint32_t rs1, uint32_t rs2) { return EncR(0, rs2, rs1, 6, rd, 0x33); }
uint32_t Ld(uint32_t rd, uint32_t rs1, int32_t imm) { return EncI(imm, rs1, 3, rd, 0x03); }
uint32_t Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { return EncS(imm, rs2, rs1, 3, 0x23); }
uint32_t Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) {
  return EncI(static_cast<int32_t>(csr), rs1, 1, rd, 0x73);
}
uint32_t Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) {
  return EncI(static_cast<int32_t>(csr), rs1, 2, rd, 0x73);
}
uint32_t Mret() { return EncI(0x302, 0, 0, 0, 0x73); }
uint32_t Wfi() { return 0x10500073u; }
// FP (OP-FP, opcode 0x53). fcvt.d.wu f, x = f7 0x69, rs2 = 1 (wu).
uint32_t FcvtDWu(uint32_t fd, uint32_t rs1, uint32_t rm) { return EncR(0x69, 1, rs1, rm, fd, 0x53); }
uint32_t FdivD(uint32_t fd, uint32_t fs1, uint32_t fs2, uint32_t rm) {
  return EncR(0x0D, fs2, fs1, rm, fd, 0x53);
}
uint32_t FmvXD(uint32_t rd, uint32_t fs1) { return EncR(0x71, 0, fs1, 0, rd, 0x53); }
// Vector.
uint32_t Vsetvli(uint32_t rd, uint32_t rs1, uint32_t vtypei) {
  return ((vtypei & 0x7FFu) << 20) | (rs1 << 15) | (7u << 12) | (rd << 7) | 0x57;
}
uint32_t Vle32(uint32_t vd, uint32_t rs1) {
  return (1u << 25) | (rs1 << 15) | (6u << 12) | (vd << 7) | 0x07;
}
uint32_t Vse32(uint32_t vs3, uint32_t rs1) {
  return (1u << 25) | (rs1 << 15) | (6u << 12) | (vs3 << 7) | 0x27;
}
uint32_t VaddVv(uint32_t vd, uint32_t vs2, uint32_t vs1) {
  return (1u << 25) | (vs2 << 20) | (vs1 << 15) | (0u << 12) | (vd << 7) | 0x57;
}

// A tiny sequential emitter with labels, so the branch offsets are computed
// rather than hand-counted.
struct Asm {
  std::map<uint64_t, uint32_t>* img;
  uint64_t pc;
  int next_label = 0;
  std::map<int, uint64_t> labels;
  struct BFix { uint64_t pc; int label; uint32_t rs1, rs2, f3; };
  struct JFix { uint64_t pc; int label; };
  std::vector<BFix> bfix;
  std::vector<JFix> jfix;

  int NewLabel() { return next_label++; }
  void Bind(int id) { labels[id] = pc; }
  void W(uint32_t w) { (*img)[pc] = w; pc += 4; }
  void Bne(uint32_t rs1, uint32_t rs2, int label) {
    bfix.push_back({pc, label, rs1, rs2, 1});
    W(EncB(0, rs2, rs1, 1, 0x63));
  }
  void Blt(uint32_t rs1, uint32_t rs2, int label) {
    bfix.push_back({pc, label, rs1, rs2, 4});
    W(EncB(0, rs2, rs1, 4, 0x63));
  }
  void Jmp(int label) {
    jfix.push_back({pc, label});
    W(EncJ(0, 0));
  }
  void Li32(uint32_t rd, uint32_t v) {
    const uint32_t hi = (v + 0x800u) >> 12;
    const int32_t lo = static_cast<int32_t>(v & 0xFFFu) - ((v & 0x800u) ? 0x1000 : 0);
    W(Lui(rd, hi & 0xFFFFFu));
    W(Addi(rd, rd, lo));
    W(Slli(rd, rd, 32));
    W(Srli(rd, rd, 32));
  }
  void Li(uint32_t rd, uint64_t value) {
    if (value <= 0xFFFFFFFFull) {
      Li32(rd, static_cast<uint32_t>(value));
      return;
    }
    Li32(rd, static_cast<uint32_t>(value >> 32));
    W(Slli(rd, rd, 32));
    Li32(31u, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    W(Or(rd, rd, 31u));
  }
  void Finish() {
    for (const auto& f : bfix) {
      const int32_t off = static_cast<int32_t>(labels.at(f.label) - f.pc);
      (*img)[f.pc] = EncB(off, f.rs2, f.rs1, f.f3, 0x63);
    }
    bfix.clear();
    for (const auto& f : jfix) {
      const int32_t off = static_cast<int32_t>(labels.at(f.label) - f.pc);
      (*img)[f.pc] = EncJ(off, 0);
    }
    jfix.clear();
  }
};

uint64_t Pp(uint64_t page_addr) { return page_addr >> 12; }
uint64_t Pte(uint64_t ppn, bool v, bool r, bool w, bool x, bool u, bool g, bool a, bool d) {
  uint64_t p = (ppn & ((UINT64_C(1) << 44) - 1)) << 10;
  if (d) p |= 1ull << 7;
  if (a) p |= 1ull << 6;
  if (g) p |= 1ull << 5;
  if (u) p |= 1ull << 4;
  if (x) p |= 1ull << 3;
  if (w) p |= 1ull << 2;
  if (r) p |= 1ull << 1;
  if (v) p |= 1ull << 0;
  return p;
}
uint64_t PteNonleaf(uint64_t next) {
  return Pte(Pp(next), true, false, false, false, false, false, false, false);
}

// ------------------------------------------------------------- memory helpers
bool MemRead8(MemoryModel* mem, uint64_t addr, uint64_t* out) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; i++) {
    uint64_t byte = 0;
    if (mem->Read(addr + i, 1, &byte) != AccessStatus::kOk) return false;
    value |= (byte & 0xffull) << (8 * i);
  }
  *out = value;
  return true;
}
bool MemWrite8(MemoryModel* mem, uint64_t addr, uint64_t value, uint32_t wstrb) {
  uint64_t window = 0;
  if (!MemRead8(mem, addr, &window)) return false;
  for (unsigned i = 0; i < 8; i++) {
    if (((wstrb >> i) & 1u) != 0u) {
      window = (window & ~(UINT64_C(0xff) << (8 * i))) |
               (((value >> (8 * i)) & 0xffull) << (8 * i));
    }
  }
  return mem->Write(addr, 8, window) == AccessStatus::kOk;
}
bool MemWrite4(MemoryModel* mem, uint64_t addr, uint32_t value) {
  return mem->Write(addr, 4, value) == AccessStatus::kOk;
}
bool MemRead4(MemoryModel* mem, uint64_t addr, uint32_t* out) {
  uint64_t byte = 0;
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; i++) {
    if (mem->Read(addr + i, 1, &byte) != AccessStatus::kOk) return false;
    v |= (uint32_t(byte) & 0xffu) << (8 * i);
  }
  *out = v;
  return true;
}

// ------------------------------------------------------------- the one program
// Both harts execute this exact image from kSamePc. Every per-hart difference
// below comes from the hart's own `mhartid` (register s1).
struct Program {
  std::map<uint64_t, uint32_t> img;
  uint64_t park_pc = 0;
  uint64_t trap_load_pc = 0;
};

Program BuildProgram() {
  Program p;
  // ---------------------------------------------------------- M-mode prologue
  {
    Asm e{&p.img, kSamePc};
    e.W(Csrrs(9, kCsrMhartid, 0));     // s1 = mhartid (read 1)
    e.W(Csrrs(10, kCsrMhartid, 0));    // s2 = mhartid (read 2)
    e.Li(5, kHandler);  e.W(Csrrw(0, kCsrMtvec, 5));
    e.Li(5, 0x0F);      e.W(Csrrw(0, kCsrPmpcfg0, 5));
    e.Li(5, (kRamBase + MOSAIC_RAM_SIZE) >> 2); e.W(Csrrw(0, kCsrPmpaddr0, 5));
    // ------------------------------------------------- IRQ cell (M-mode)
    // hart 0 raises a machine software interrupt on itself; hart 1 skips it.
    // The interrupt is a per-hart event and is taken at the next architectural
    // boundary, in M-mode, where mie/mip are writable. (An S-mode CSR write
    // would be an illegal instruction, which is exactly how the first version
    // of this program failed.)
    {
      const int skip_irq = e.NewLabel();
      e.Bne(9, 0, skip_irq);
      e.Li(5, 0x8);  e.W(Csrrw(0, kCsrMie, 5));       // mie.MSIE
      e.Li(5, 0x8);  e.W(Csrrs(0, kCsrMstatus, 5));   // mstatus.MIE
      e.Li(5, 0x8);  e.W(Csrrs(0, kCsrMip, 5));       // mip.MSIP -> interrupt
      e.W(Addi(5, 5, 1)); e.W(Addi(5, 5, 1));
      e.Bind(skip_irq);
    }
    // mstatus = MPP=S | FS=Initial | VS=Initial | (hart << SUM). This also
    // re-establishes MPP=S after the interrupt above, which the trap set to M.
    e.Li(5, kMstatusBase);
    e.W(Slli(6, 9, kSumBit));
    e.W(Or(5, 5, 6));
    e.W(Csrrw(0, kCsrMstatus, 5));
    // ------------------------------------------------- FP cell (M-mode)
    e.W(Slli(5, 9, 5)); e.W(Csrrw(0, kCsrFcsr, 5));   // fcsr.frm = hart
    e.W(FcvtDWu(0, 9, 0));                            // f0 = (double)hart
    e.W(FcvtDWu(3, 0, 0));                            // f3 = 0.0
    e.W(FdivD(2, 0, 3, 0));                           // f2 = f0 / 0.0
    e.W(FmvXD(10, 2));                                // a0 = bits(f2)
    e.Li(5, kFpBase); e.W(Slli(6, 9, 12)); e.W(Add(5, 5, 6));
    e.W(Sd(10, 5, 0));                                // store the FP result
    // ------------------------------------------------- vector cell (M-mode)
    e.W(Add(5, 9, 9)); e.W(Addi(5, 5, 1));            // avl = 2*hart + 1
    e.W(Vsetvli(6, 5, 0x10));                         // e32, m1
    e.Li(7, kVecABase); e.W(Slli(6, 9, 12)); e.W(Add(7, 7, 6));
    e.W(Vle32(1, 7));                                 // v1 = source
    e.W(VaddVv(2, 1, 1));                             // v2 = v1 + v1
    e.Li(8, kVecCBase); e.W(Slli(6, 9, 12)); e.W(Add(8, 8, 6));
    e.W(Vse32(2, 8));                                 // store v2
    // ------------------------------------------------- satp / ASID (per hart)
    // satp's low field is the root's page NUMBER, not its address: putting the
    // address in makes the walker read PPN<<12, which is outside RAM, and every
    // translated access then faults with an access fault instead of a page
    // fault. The page number of kRootBase is Pp(kRootBase); each hart's root is
    // the next page, so its PPN is Pp(kRootBase) + hart.
    e.Li(5, kRootBase >> 12); e.W(Add(5, 5, 9));
    e.Li(6, 8); e.W(Slli(6, 6, 60)); e.W(Or(5, 5, 6));      // mode = Sv39
    e.W(Addi(6, 9, 1)); e.W(Slli(6, 6, 44)); e.W(Or(5, 5, 6));  // asid = 1+hart
    e.W(Csrrw(0, kCsrSatp, 5));
    e.Li(5, kSEntry); e.W(Csrrw(0, kCsrMepc, 5));
    e.W(Mret());
    e.W(EncJ(0, 0));                                   // unreachable
    e.Finish();
    if (e.pc > kSEntry) {
      Fail("program", "the M-mode prologue runs into the S-mode body");
    }
  }
  // -------------------------------------------------------------- S-mode body
  {
    Asm e{&p.img, kSEntry};
    // ASID cell: read satp back and store it through a translated address.
    e.W(Csrrs(10, kCsrSatp, 0));
    e.Li(11, kVaSig); e.W(Sd(10, 11, 0));
    // Trap cell: the same translated load; hart 0's page is invalid, hart 1's
    // is mapped, so the same PC faults on one hart and retires on the other.
    e.Li(11, kVaTrapBase); e.W(Slli(5, 9, 12)); e.W(Add(11, 11, 5));
    p.trap_load_pc = e.pc;
    e.W(Ld(12, 11, 0));
    e.Li(13, kVaSig); e.W(Sd(12, 13, 8));
    // WFI cell: hart 0 halts here; hart 1 skips it. `sie` is the S-mode enable
    // register (an M-mode `mie` write would be an illegal instruction here).
    const int skip_wfi = e.NewLabel();
    e.Bne(9, 0, skip_wfi);
    e.W(Csrrw(0, kCsrSie, 0));                         // no enabled pending
    e.W(Wfi());
    e.Bind(skip_wfi);
    // hart 1 runs a bounded loop of real work and only then parks, so it is
    // demonstrably still executing (not spinning in its park) at the cycle hart
    // 0 halts. hart 0 halted at the WFI above and never reaches this.
    e.Li(5, kHart1WorkIters);
    const int work = e.NewLabel();
    e.Bind(work);
    e.W(Addi(6, 6, 1));
    e.W(Addi(5, 5, -1));
    e.Bne(5, 0, work);
    p.park_pc = e.pc;
    e.W(EncJ(0, 0));                                   // park (hart 1)
    e.Finish();
    if (e.pc > kHandler) {
      Fail("program", "the S-mode body runs into the trap handler");
    }
  }
  // ------------------------------------------------------------ trap handler
  {
    Asm e{&p.img, kHandler};
    e.W(Csrrs(5, kCsrMcause, 0));   // t0 = mcause
    e.W(Csrrs(6, kCsrMtval, 0));    // t1 = mtval
    e.W(Csrrs(7, kCsrMepc, 0));     // t2 = mepc
    e.Li(10, kTrapBase); e.W(Slli(11, 9, 12)); e.W(Add(10, 10, 11));
    const int irq_store = e.NewLabel();
    const int done = e.NewLabel();
    e.Blt(5, 0, irq_store);         // mcause<0 => interrupt
    e.W(Sd(5, 10, 0)); e.W(Sd(6, 10, 8));
    e.Jmp(done);
    e.Bind(irq_store);
    e.W(Sd(5, 10, 16)); e.W(Sd(6, 10, 24));
    e.Bind(done);
    e.W(Csrrw(0, kCsrMip, 0));      // clear the pending interrupt
    e.W(Addi(7, 7, 4)); e.W(Csrrw(0, kCsrMepc, 7));
    // Preserve MPP: the trap set it to the interrupted privilege, so mret must
    // return there (M for the prologue's interrupt, S for the S-mode fault).
    // Setting MPIE makes MIE come back set after mret.
    e.Li(5, 1u << 7); e.W(Csrrs(0, kCsrMstatus, 5));
    e.W(Mret());
    e.Finish();
  }
  return p;
}

// ---------------------------------------------------------------- the setup
void WriteImage(MemoryModel* mem, const std::map<uint64_t, uint32_t>& img,
                const std::string& what) {
  for (const auto& kv : img) {
    if (!MemWrite4(mem, kv.first, kv.second)) {
      Fail(what, "cannot write the instruction at " + U64(kv.first));
    }
  }
}

// hart 0: root[1] -> L1[0] -> L0; L0[0] invalid (the faulting VA), L0[2] = sig.
// hart 1: the same shape, its own root/L1/L0; L0[1] = data, L0[2] = its sig.
void BuildTables(MemoryModel* mem) {
  auto put = [&](uint64_t table, unsigned index, uint64_t value) {
    if (!MemWrite8(mem, table + 8ull * index, value, 0xFF)) {
      Fail("setup", "cannot write the PTE at " + U64(table + 8ull * index));
    }
  };
  for (unsigned h = 0; h < 2; h++) {
    put(kRootBase + h * 0x1000ull, 1, PteNonleaf(kL1[h]));
    put(kL1[h], 0, PteNonleaf(kL0[h]));
    put(kL0[h], 2, Pte(Pp(kSigBase + h * 0x1000ull), true, true, true, false, false, false, true, true));
  }
  put(kL0[0], 0, 0);   // hart 0's trap VA: V=0, so the load page-faults
  put(kL0[1], 1, Pte(Pp(kData1), true, true, false, false, false, false, true, true));
}

void SeedPages(MemoryModel* mem) {
  for (unsigned h = 0; h < 2; h++) {
    if (!MemWrite8(mem, kVecABase + h * 0x1000ull, 0, 0)) {
      Fail("setup", "cannot seed the vector source page");
    }
    for (unsigned i = 0; i < 4; i++) {
      if (!MemWrite4(mem, kVecABase + h * 0x1000ull + 4ull * i, kVecA[h][i])) {
        Fail("setup", "cannot seed the vector source element");
      }
    }
  }
  if (!MemWrite8(mem, kData1, kData1Value, 0xFF)) {
    Fail("setup", "cannot seed hart 1's data page");
  }
}

// ============================================================================
// The shared memory service, exactly as multihart.isolation models it: one
// request channel, one response channel, every packet hart-tagged, a per-hart
// latency so responses can return out of order. The tag on a response is the tag
// the DUT put on the request.
// ============================================================================
struct BusRsp {
  uint64_t rdata = 0;
  bool fault = false;
  unsigned hart = 0;
  unsigned src = 0;
  uint32_t id = 0;
  uint32_t epoch = 0;
  unsigned len = 0;
};

class SharedBus {
 public:
  explicit SharedBus(MemoryModel* mem) : mem_(mem) {}
  void Reset() { inflight_.clear(); ready_.clear(); accepted_ = 0; }
  bool HasRsp() const { return !ready_.empty(); }
  const BusRsp& Current() const { return ready_.front(); }
  uint64_t accepted() const { return accepted_; }

  void Accept(bool we, uint64_t addr, unsigned size, uint64_t wdata, uint8_t wstrb,
              bool amo, unsigned hart, unsigned src, uint32_t id, uint32_t epoch) {
    accepted_++;
    BusRsp r;
    r.hart = hart; r.src = src; r.id = id; r.epoch = epoch;
    Perform(we, addr, size, wdata, wstrb, amo, src, &r);
    Entry e; e.rsp = r; e.left = Latency(hart);
    inflight_.push_back(e);
  }
  void Pop() { ready_.pop_front(); }
  void Advance() {
    for (size_t i = 0; i < inflight_.size();) {
      if (--inflight_[i].left <= 0) {
        ready_.push_back(inflight_[i].rsp);
        inflight_.erase(inflight_.begin() + long(i));
      } else {
        ++i;
      }
    }
  }

 private:
  struct Entry { BusRsp rsp; int left = 0; };
  static int Latency(unsigned hart) { return hart == 0 ? 4 : 2; }
  uint64_t ReadWindow(uint64_t base, bool* ok) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) {
      uint64_t byte = 0;
      if (mem_->Read(base + i, 1, &byte) != AccessStatus::kOk) { *ok = false; return 0; }
      v |= (byte & 0xffull) << (8 * i);
    }
    return v;
  }
  void Perform(bool we, uint64_t addr, unsigned size, uint64_t wdata, uint8_t wstrb,
               bool amo, unsigned src, BusRsp* r) {
    if (amo) { r->fault = true; return; }
    if (src == 1) {
      const unsigned nbytes = 1u << size;
      uint64_t v = 0;
      for (unsigned i = 0; i < nbytes; i++) {
        uint64_t byte = 0;
        if (mem_->Read(addr + i, 1, &byte) != AccessStatus::kOk) { r->fault = true; return; }
        v |= (byte & 0xffull) << (8 * i);
      }
      r->rdata = v;
      r->len = ((v & 3u) == 3u) ? 4 : 2;
      return;
    }
    const uint64_t base = addr & ~7ull;
    bool ok = true;
    if (we) {
      uint64_t window = ReadWindow(base, &ok);
      if (!ok) { r->fault = true; return; }
      for (unsigned i = 0; i < 8; i++) {
        if (((wstrb >> i) & 1u) != 0u) {
          window = (window & ~(0xffull << (8 * i))) |
                   (((wdata >> (8 * i)) & 0xffull) << (8 * i));
        }
      }
      const AccessStatus st = mem_->Write(base, 8, window);
      r->fault = (st != AccessStatus::kOk && st != AccessStatus::kDeviceError);
    } else {
      r->rdata = ReadWindow(base, &ok);
      if (!ok) r->fault = true;
    }
  }
  MemoryModel* mem_;
  std::deque<Entry> inflight_;
  std::deque<BusRsp> ready_;
  uint64_t accepted_ = 0;
};

// ============================================================================
// One run's observation
// ============================================================================
struct RetireRec {
  uint64_t seq = 0, pc = 0, insn = 0, value = 0, saddr = 0, sdata = 0,
           caddr = 0, cval = 0, tcause = 0, tval = 0;
  unsigned len = 0, rd = 0, ssz = 0;
  bool reg_we = false, store = false, csr_we = false, trap = false;
  bool operator==(const RetireRec& o) const {
    const bool val_ok = !reg_we || (value == o.value);
    return seq == o.seq && pc == o.pc && insn == o.insn && len == o.len &&
           reg_we == o.reg_we && rd == o.rd && val_ok && store == o.store &&
           saddr == o.saddr && sdata == o.sdata && ssz == o.ssz &&
           csr_we == o.csr_we && caddr == o.caddr && cval == o.cval &&
           trap == o.trap && tcause == o.tcause && tval == o.tval;
  }
};

struct TrapEv { uint64_t cause = 0, tval = 0; bool irq = false; };

template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return uint64_t(wide[lane * 2]) | (uint64_t(wide[lane * 2 + 1]) << 32);
}
uint64_t ScalarField(uint64_t value, uint32_t lane, uint32_t width) {
  if (width == 0) return 0;
  const uint64_t shift = uint64_t(lane) * width;
  const uint64_t mask = (width >= 64) ? ~UINT64_C(0) : ((UINT64_C(1) << width) - 1);
  return (value >> shift) & mask;
}

struct PhaseObs {
  std::vector<RetireRec> stream[2];
  bool parked[2] = {false, false};
  bool stopped[2] = {false, false};
  bool wfi_seen[2] = {false, false};
  std::vector<TrapEv> traps[2];
  uint64_t sig_fp[2] = {0, 0};       // stored FP result
  uint64_t sig_satp[2] = {0, 0};     // translated read-back of satp
  uint64_t sig_ld[2] = {0, 0};       // stored result of the trap-cell load
  uint32_t vec_c[2][4] = {{0}};      // stored vector bytes
  uint64_t trap_cause[2] = {0, 0};   // trap page: sync cause / irq cause
  uint64_t trap_tval[2] = {0, 0};
  uint64_t trap_irq_cause[2] = {0, 0};
  uint64_t trap_irq_tval[2] = {0, 0};
  uint32_t fcsr[2] = {0, 0};
  uint32_t vec_vl[2] = {0, 0};
  uint64_t vec_vtype[2] = {0, 0};
  uint64_t vec_vstart[2] = {0, 0};
  bool vec_vill[2] = {false, false};
  uint32_t trap_irq_ctr[2] = {0, 0};
  uint64_t satp[2] = {0, 0};
  uint64_t mstatus[2] = {0, 0};
  uint32_t priv[2] = {0, 0};
  uint64_t commit_end[2] = {0, 0};
  uint64_t h1_commit_at_h0_wfi = 0;
  uint32_t mismatch = 0, req_h0 = 0, req_h1 = 0, req_imem = 0, req_dmem = 0,
           contend = 0;
  uint32_t id_hart_bad = 0;
  uint64_t cycles = 0;
  // The first requests the shared service accepted (hart, src, addr, we), so a
  // failing case can say whether the page-table walk ever reached memory.
  std::vector<std::array<uint64_t, 4>> req_log;
  uint64_t geom_hart_w = 0, geom_mem_id_w = 0, geom_ret_n = 0, geom_seq_w = 0;
};

class Runner {
 public:
  explicit Runner(Vmosaic_multihart_ctx_tb* dut) : dut_(dut) {}

  PhaseObs Run(unsigned mask, const Program& p, uint64_t max_cycles) {
    PhaseObs obs;
    MemoryModel mem;
    BuildTables(&mem);
    WriteImage(&mem, p.img, "program");
    SeedPages(&mem);

    SharedBus bus(&mem);
    cycles_ = 0;
    dut_->clk = 0;
    dut_->rst = 1;
    dut_->hart_en_i = mask;
    DriveIdleBus();
    dut_->eval();
    for (int i = 0; i < kResetCycles; i++) Clock();
    dut_->rst = 0;

    uint64_t last_progress = 0;
    uint64_t last_commit = 0;
    int drain_left = 0;
    for (uint64_t i = 0; i < max_cycles; i++) {
      DriveIdleBus();
      dut_->hart_en_i = mask;
      if (bus.HasRsp()) {
        const BusRsp& r = bus.Current();
        dut_->mem_rsp_valid = 1;
        dut_->mem_rsp_hart = r.hart;
        dut_->mem_rsp_src = r.src;
        dut_->mem_rsp_rdata = r.rdata;
        dut_->mem_rsp_fault = r.fault ? 1 : 0;
        dut_->mem_rsp_id = r.id;
        dut_->mem_rsp_epoch = r.epoch;
        dut_->mem_rsp_len = r.len;
      } else {
        dut_->mem_rsp_valid = 0;
      }
      dut_->mem_req_ready = 1;
      dut_->eval();

      Observe(&obs, p.park_pc);

      if (dut_->mem_req_valid && dut_->mem_req_ready) {
        const unsigned hart = unsigned(dut_->mem_req_hart);
        const unsigned src = dut_->mem_req_src ? 1u : 0u;
        if (obs.req_log.size() < 800) {
          obs.req_log.push_back({hart, src, uint64_t(dut_->mem_req_addr),
                                 dut_->mem_req_we != 0 ? 1u : 0u});
        }
        bus.Accept(dut_->mem_req_we != 0, dut_->mem_req_addr, unsigned(dut_->mem_req_size),
                   dut_->mem_req_wdata, uint8_t(dut_->mem_req_wstrb),
                   dut_->mem_req_amo != 0, hart, src,
                   uint32_t(dut_->mem_req_id), uint32_t(dut_->mem_req_epoch));
      }
      if (dut_->mem_rsp_valid && dut_->mem_rsp_ready) bus.Pop();
      bus.Advance();

      const uint64_t commits = uint64_t(dut_->h0_commit_ctr) + uint64_t(dut_->h1_commit_ctr);
      if (commits != last_commit) { last_commit = commits; last_progress = cycles_; }
      Clock();
      obs.cycles = cycles_;

      const bool finished = ((mask & 1u) == 0u || obs.parked[0] || obs.wfi_seen[0] || obs.stopped[0]) &&
                            ((mask & 2u) == 0u || obs.parked[1] || obs.wfi_seen[1] || obs.stopped[1]);
      if (finished && drain_left == 0) drain_left = kDrainCycles;
      if (drain_left > 0) {
        drain_left--;
        if (drain_left == 0) break;
      }
      if (cycles_ - last_progress > kStallCycles) break;
    }

    obs.commit_end[0] = uint64_t(dut_->h0_commit_ctr);
    obs.commit_end[1] = uint64_t(dut_->h1_commit_ctr);
    obs.mismatch = uint32_t(dut_->o_owner_mismatch_ctr);
    obs.req_h0 = uint32_t(dut_->o_req_h0_ctr);
    obs.req_h1 = uint32_t(dut_->o_req_h1_ctr);
    obs.req_imem = uint32_t(dut_->o_req_imem_ctr);
    obs.req_dmem = uint32_t(dut_->o_req_dmem_ctr);
    obs.contend = uint32_t(dut_->o_contend_ctr);
    obs.geom_hart_w = uint32_t(dut_->o_geom_hart_w);
    obs.geom_mem_id_w = uint32_t(dut_->o_geom_mem_id_w);
    obs.geom_ret_n = uint32_t(dut_->o_geom_ret_n);
    obs.geom_seq_w = uint32_t(dut_->o_geom_seq_w);
    obs.satp[0] = uint64_t(dut_->h0_satp);
    obs.satp[1] = uint64_t(dut_->h1_satp);
    obs.mstatus[0] = uint64_t(dut_->h0_mstatus);
    obs.mstatus[1] = uint64_t(dut_->h1_mstatus);
    obs.priv[0] = uint32_t(dut_->h0_priv);
    obs.priv[1] = uint32_t(dut_->h1_priv);
    obs.fcsr[0] = uint32_t(dut_->h0_fcsr);
    obs.fcsr[1] = uint32_t(dut_->h1_fcsr);
    obs.vec_vl[0] = uint32_t(dut_->h0_vec_vl);
    obs.vec_vl[1] = uint32_t(dut_->h1_vec_vl);
    obs.vec_vtype[0] = uint64_t(dut_->h0_vec_vtype);
    obs.vec_vtype[1] = uint64_t(dut_->h1_vec_vtype);
    obs.vec_vstart[0] = uint64_t(dut_->h0_vec_vstart);
    obs.vec_vstart[1] = uint64_t(dut_->h1_vec_vstart);
    obs.vec_vill[0] = dut_->h0_vec_vill != 0;
    obs.vec_vill[1] = dut_->h1_vec_vill != 0;
    obs.trap_irq_ctr[0] = uint32_t(dut_->h0_trap_irq_ctr);
    obs.trap_irq_ctr[1] = uint32_t(dut_->h1_trap_irq_ctr);

    uint64_t v = 0;
    for (unsigned h = 0; h < 2; h++) {
      if (MemRead8(&mem, kFpBase + h * 0x1000ull, &v)) obs.sig_fp[h] = v;
      if (MemRead8(&mem, kSigBase + h * 0x1000ull, &v)) obs.sig_satp[h] = v;
      if (MemRead8(&mem, kSigBase + h * 0x1000ull + 8, &v)) obs.sig_ld[h] = v;
      if (MemRead8(&mem, kTrapBase + h * 0x1000ull, &v)) obs.trap_cause[h] = v;
      if (MemRead8(&mem, kTrapBase + h * 0x1000ull + 8, &v)) obs.trap_tval[h] = v;
      if (MemRead8(&mem, kTrapBase + h * 0x1000ull + 16, &v)) obs.trap_irq_cause[h] = v;
      if (MemRead8(&mem, kTrapBase + h * 0x1000ull + 24, &v)) obs.trap_irq_tval[h] = v;
      for (unsigned i = 0; i < 4; i++) {
        uint32_t w = 0;
        if (MemRead4(&mem, kVecCBase + h * 0x1000ull + 4ull * i, &w)) obs.vec_c[h][i] = w;
      }
    }
    return obs;
  }

 private:
  static constexpr int kResetCycles = 8;
  static constexpr int kDrainCycles = 64;
  static constexpr uint64_t kStallCycles = 30000;

  void DriveIdleBus() {
    dut_->mem_req_ready = 1;
    dut_->mem_rsp_valid = 0;
    dut_->mem_rsp_hart = 0;
    dut_->mem_rsp_src = 0;
    dut_->mem_rsp_rdata = 0;
    dut_->mem_rsp_fault = 0;
    dut_->mem_rsp_id = 0;
    dut_->mem_rsp_epoch = 0;
    dut_->mem_rsp_len = 0;
  }

  void Clock() {
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
  }

  void Observe(PhaseObs* obs, uint64_t park) {
    const uint32_t ret_n = obs->geom_ret_n ? uint32_t(obs->geom_ret_n) : 2u;
    const uint32_t seq_w = obs->geom_seq_w ? uint32_t(obs->geom_seq_w) : 7u;
    // The two harts' own packet identity must name them.
    if (dut_->h0_dmem_id_valid) {
      const uint32_t hw = obs->geom_hart_w, mw = obs->geom_mem_id_w;
      if (hw > 0 && mw >= hw) {
        if (((uint64_t(dut_->h0_dmem_id) >> (mw - hw)) & ((1ull << hw) - 1)) != 0)
          obs->id_hart_bad++;
      }
    }
    if (dut_->h1_dmem_id_valid) {
      const uint32_t hw = obs->geom_hart_w, mw = obs->geom_mem_id_w;
      if (hw > 0 && mw >= hw) {
        if (((uint64_t(dut_->h1_dmem_id) >> (mw - hw)) & ((1ull << hw) - 1)) != 1)
          obs->id_hart_bad++;
      }
    }
    // WFI halt and the trap pulses, sampled every cycle.
    if (dut_->h0_wfi_halt) {
      if (!obs->wfi_seen[0]) obs->h1_commit_at_h0_wfi = uint64_t(dut_->h1_commit_ctr);
      obs->wfi_seen[0] = true;
    }
    if (dut_->h1_wfi_halt) obs->wfi_seen[1] = true;
    if (dut_->h0_stopped) obs->stopped[0] = true;
    if (dut_->h1_stopped) obs->stopped[1] = true;
    if (dut_->h0_trap_valid) {
      TrapEv t;
      t.cause = uint64_t(dut_->h0_trap_cause);
      t.tval = uint64_t(dut_->h0_trap_tval);
      t.irq = dut_->h0_trap_is_irq != 0;
      obs->traps[0].push_back(t);
    }
    if (dut_->h1_trap_valid) {
      TrapEv t;
      t.cause = uint64_t(dut_->h1_trap_cause);
      t.tval = uint64_t(dut_->h1_trap_tval);
      t.irq = dut_->h1_trap_is_irq != 0;
      obs->traps[1].push_back(t);
    }

    for (unsigned h = 0; h < 2; h++) {
      if (obs->parked[h]) continue;
      const uint32_t valid = uint32_t(h == 0 ? dut_->h0_ev_valid : dut_->h1_ev_valid);
      for (uint32_t lane = 0; lane < ret_n; lane++) {
        if ((valid & (1u << lane)) == 0) continue;
        const uint64_t pc = h == 0 ? PayloadLane(dut_->h0_ev_pc, lane)
                                   : PayloadLane(dut_->h1_ev_pc, lane);
        if (pc == park) { obs->parked[h] = true; break; }
        RetireRec r;
        if (h == 0) {
          r.seq = ScalarField(dut_->h0_ev_seq, lane, seq_w);
          r.pc = pc;
          r.insn = ScalarField(dut_->h0_ev_insn, lane, 32);
          r.len = unsigned(ScalarField(dut_->h0_ev_len, lane, 3));
          r.reg_we = ScalarField(dut_->h0_ev_reg_we, lane, 1) != 0;
          r.rd = unsigned(ScalarField(dut_->h0_ev_rd, lane, 5));
          r.value = PayloadLane(dut_->h0_ev_value, lane);
          r.store = ScalarField(dut_->h0_ev_store, lane, 1) != 0;
          r.saddr = PayloadLane(dut_->h0_ev_store_addr, lane);
          r.sdata = PayloadLane(dut_->h0_ev_store_data, lane);
          r.ssz = unsigned(ScalarField(dut_->h0_ev_store_size, lane, 3));
          r.csr_we = ScalarField(dut_->h0_ev_csr_we, lane, 1) != 0;
          r.caddr = ScalarField(dut_->h0_ev_csr_addr, lane, 12);
          r.cval = PayloadLane(dut_->h0_ev_csr_value, lane);
          r.trap = ScalarField(dut_->h0_ev_trap, lane, 1) != 0;
          r.tcause = PayloadLane(dut_->h0_ev_trap_cause, lane);
          r.tval = PayloadLane(dut_->h0_ev_trap_tval, lane);
        } else {
          r.seq = ScalarField(dut_->h1_ev_seq, lane, seq_w);
          r.pc = pc;
          r.insn = ScalarField(dut_->h1_ev_insn, lane, 32);
          r.len = unsigned(ScalarField(dut_->h1_ev_len, lane, 3));
          r.reg_we = ScalarField(dut_->h1_ev_reg_we, lane, 1) != 0;
          r.rd = unsigned(ScalarField(dut_->h1_ev_rd, lane, 5));
          r.value = PayloadLane(dut_->h1_ev_value, lane);
          r.store = ScalarField(dut_->h1_ev_store, lane, 1) != 0;
          r.saddr = PayloadLane(dut_->h1_ev_store_addr, lane);
          r.sdata = PayloadLane(dut_->h1_ev_store_data, lane);
          r.ssz = unsigned(ScalarField(dut_->h1_ev_store_size, lane, 3));
          r.csr_we = ScalarField(dut_->h1_ev_csr_we, lane, 1) != 0;
          r.caddr = ScalarField(dut_->h1_ev_csr_addr, lane, 12);
          r.cval = PayloadLane(dut_->h1_ev_csr_value, lane);
          r.trap = ScalarField(dut_->h1_ev_trap, lane, 1) != 0;
          r.tcause = PayloadLane(dut_->h1_ev_trap_cause, lane);
          r.tval = PayloadLane(dut_->h1_ev_trap_tval, lane);
        }
        obs->stream[h].push_back(r);
      }
    }
  }

  Vmosaic_multihart_ctx_tb* dut_;
  uint64_t cycles_ = 0;
};

// ============================================================================
// Comparison helpers
// ============================================================================
std::string DumpRec(const RetireRec& r) {
  return "seq=" + Dec(r.seq) + " pc=" + U64(r.pc) + " insn=" + U64(r.insn) +
         " len=" + Dec(r.len) + " we=" + Dec(r.reg_we) + " rd=" + Dec(r.rd) +
         " val=" + U64(r.value) + " store=" + Dec(r.store) + " sa=" + U64(r.saddr) +
         " sd=" + U64(r.sdata) + " csrwe=" + Dec(r.csr_we) + " csra=" + U64(r.caddr) +
         " csrv=" + U64(r.cval) + " trap=" + Dec(r.trap) + " cause=" + Dec(r.tcause) +
         " tval=" + U64(r.tval);
}
size_t FirstDiff(const std::vector<RetireRec>& a, const std::vector<RetireRec>& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; i++) {
    if (!(a[i] == b[i])) return i;
  }
  return n;
}
std::string AsidOf(uint64_t satp) { return Dec((satp >> 44) & 0xFFFF); }

// The retire record at `pc` whose destination is `rd` and which writes a
// register, or nullptr. Used to read the per-hart `mhartid` results.
const RetireRec* FindRegWrite(const std::vector<RetireRec>& stream, uint64_t pc, unsigned rd) {
  for (const auto& r : stream) {
    if (r.pc == pc && r.reg_we && r.rd == rd) return &r;
  }
  return nullptr;
}
const RetireRec* FindPc(const std::vector<RetireRec>& stream, uint64_t pc) {
  for (const auto& r : stream) {
    if (r.pc == pc) return &r;
  }
  return nullptr;
}

}  // namespace

// ============================================================================
int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  // The one driver-side control: compare hart 1 against hart 0's reference, i.e.
  // a single shared reference register state. The two harts' states differ, so
  // that comparison must fail.
  bool share_ref = false;
  std::vector<char*> filtered;
  filtered.push_back(argv[0]);
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--control-share-ref") == 0) { share_ref = true; continue; }
    filtered.push_back(argv[i]);
  }
  if (!mosaic::Options::Parse(int(filtered.size()), filtered.data(), &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);
  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_multihart_ctx_tb dut;

  bool passed = true;
  std::string detail;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.hart_en_i = 0;
    dut.eval();

    const Program prog = BuildProgram();
    Runner runner(&dut);

    const PhaseObs solo0 = runner.Run(0b01, prog, options.max_cycles);
    const PhaseObs solo1 = runner.Run(0b10, prog, options.max_cycles);
    const PhaseObs both  = runner.Run(0b11, prog, options.max_cycles);

    std::printf("  cycles: solo0=%llu solo1=%llu both=%llu\n",
                (unsigned long long)solo0.cycles, (unsigned long long)solo1.cycles,
                (unsigned long long)both.cycles);
    std::printf("  both: parked=%d/%d wfi=%d/%d stopped=%d/%d n0=%zu n1=%zu\n",
                both.parked[0]?1:0, both.parked[1]?1:0, both.wfi_seen[0]?1:0,
                both.wfi_seen[1]?1:0, both.stopped[0]?1:0, both.stopped[1]?1:0,
                both.stream[0].size(), both.stream[1].size());
    std::printf("  both: fcsr0=%s fcsr1=%s vl0=%u vl1=%u satp0=%s satp1=%s priv=%u/%u\n",
                U64(both.fcsr[0]).c_str(), U64(both.fcsr[1]).c_str(),
                both.vec_vl[0], both.vec_vl[1],
                U64(both.satp[0]).c_str(), U64(both.satp[1]).c_str(),
                both.priv[0], both.priv[1]);
    std::printf("  both: fp0=%s fp1=%s irqctr=%u/%u traps0=%zu traps1=%zu\n",
                U64(both.sig_fp[0]).c_str(), U64(both.sig_fp[1]).c_str(),
                both.trap_irq_ctr[0], both.trap_irq_ctr[1],
                both.traps[0].size(), both.traps[1].size());
    std::printf("  both: req h0=%u h1=%u contend=%u mismatch=%u id_hart_bad=%u\n",
                both.req_h0, both.req_h1, both.contend, both.mismatch, both.id_hart_bad);
    if (options.verbose) {
      std::printf("  --- shared-service requests (first %zu) ---\n", both.req_log.size());
      for (size_t i = 0; i < both.req_log.size(); i++) {
        std::printf("    [%zu] hart=%llu src=%llu addr=%s we=%llu\n", i,
                    (unsigned long long)both.req_log[i][0],
                    (unsigned long long)both.req_log[i][1],
                    U64(both.req_log[i][2]).c_str(),
                    (unsigned long long)both.req_log[i][3]);
      }
      for (unsigned h = 0; h < 2; h++) {
        std::printf("  --- hart %u stream (first 60 of %zu) ---\n", h, both.stream[h].size());
        for (size_t i = 0; i < both.stream[h].size() && i < 60; i++) {
          std::printf("    [%zu] %s\n", i, DumpRec(both.stream[h][i]).c_str());
        }
        std::printf("  --- hart %u stream (last 24 of %zu) ---\n", h, both.stream[h].size());
        for (size_t i = (both.stream[h].size() > 24 ? both.stream[h].size() - 24 : 0);
             i < both.stream[h].size(); i++) {
          std::printf("    [%zu] %s\n", i, DumpRec(both.stream[h][i]).c_str());
        }
        std::printf("  --- hart %u trap records ---\n", h);
        for (size_t i = 0; i < both.stream[h].size(); i++) {
          if (both.stream[h][i].trap) {
            std::printf("    [%zu] %s\n", i, DumpRec(both.stream[h][i]).c_str());
          }
        }
        std::printf("  --- hart %u traps (%zu) ---\n", h, both.traps[h].size());
        for (const auto& t : both.traps[h]) {
          std::printf("    cause=%s tval=%s irq=%d\n", U64(t.cause).c_str(),
                      U64(t.tval).c_str(), t.irq ? 1 : 0);
        }
        std::printf("  --- hart %u pages: fp=%s satp=%s ld=%s trap_cause=%s tval=%s irq=%s\n",
                    h, U64(both.sig_fp[h]).c_str(), U64(both.sig_satp[h]).c_str(),
                    U64(both.sig_ld[h]).c_str(), U64(both.trap_cause[h]).c_str(),
                    U64(both.trap_tval[h]).c_str(), U64(both.trap_irq_cause[h]).c_str());
      }
    }

    // ------------------------------------------------------- 1. the same PC
    const bool first_ok =
        !both.stream[0].empty() && !both.stream[1].empty() &&
        both.stream[0][0].pc == kSamePc && both.stream[1][0].pc == kSamePc;
    reporter.Check(first_ok, "both harts reset to the same PC and their first retire PC is equal");

    // ------------------------------------------- 2. mhartid stable, per-hart
    const RetireRec* h0_r1 = FindRegWrite(both.stream[0], kSamePc, 9);
    const RetireRec* h0_r2 = FindRegWrite(both.stream[0], kSamePc + 4, 10);
    const RetireRec* h1_r1 = FindRegWrite(both.stream[1], kSamePc, 9);
    const RetireRec* h1_r2 = FindRegWrite(both.stream[1], kSamePc + 4, 10);
    const bool reads_ok = h0_r1 && h0_r2 && h1_r1 && h1_r2;
    reporter.Check(reads_ok, "both harts read mhartid twice");
    if (reads_ok) {
      reporter.Check(h0_r1->value == h0_r2->value && h1_r1->value == h1_r2->value,
                     "each hart's two mhartid reads agree (it is stable)");
      reporter.Check(h0_r1->value == 0 && h1_r1->value == 1,
                     "hart 0 reads mhartid 0 and hart 1 reads mhartid 1");
      reporter.Check(h0_r1->value != h1_r1->value,
                     "the two harts report different mhartid values");
    }

    // ------------------------------------- 3. same PC, different GPR state
    if (h0_r1 && h1_r1) {
      reporter.Check(h0_r1->value != h1_r1->value,
                     "the same csrr mhartid at the same PC retires with a different GPR value per hart");
    }

    // --------------------------------------------------- 4. ASID / CSR state
    reporter.Check(AsidOf(both.satp[0]) == "1" && AsidOf(both.satp[1]) == "2",
                   "each hart's satp carries its own ASID (" + AsidOf(both.satp[0]) +
                       " and " + AsidOf(both.satp[1]) + ")");
    reporter.Check(both.satp[0] != both.satp[1], "the two satp values differ");
    reporter.Check(both.sig_satp[0] == both.satp[0] && both.sig_satp[1] == both.satp[1],
                   "each hart's S-mode read-back of satp matches its own port");

    // -------------------------------------------- 5. privilege / CSR state
    reporter.Check(both.priv[0] == 1 && both.priv[1] == 1,
                   "both harts ended in S-mode (their own privilege state)");
    reporter.Check(both.mstatus[0] != both.mstatus[1],
                   "the two harts' mstatus values differ");
    reporter.Check((((both.mstatus[0] >> kSumBit) & 1) == 0) &&
                       (((both.mstatus[1] >> kSumBit) & 1) == 1),
                   "each hart's mstatus carries its own SUM bit");

    // ------------------------------------------------------------- 6. FP state
    reporter.Check(both.fcsr[0] == kFcsrHart0 && both.fcsr[1] == kFcsrHart1,
                   "each hart's fcsr is its own (flags from its own divide)");
    reporter.Check(both.fcsr[0] != both.fcsr[1], "the two harts' fcsr values differ");
    reporter.Check(both.sig_fp[0] == kFpHart0 && both.sig_fp[1] == kFpHart1,
                   "the same fdiv.d produced each hart's own FP result (qNaN vs +inf)");

    // --------------------------------------------------------- 7. vector state
    reporter.Check(both.vec_vl[0] == kVlHart0 && both.vec_vl[1] == kVlHart1,
                   "the same vsetvli selected each hart's own vl (" +
                       Dec(both.vec_vl[0]) + " and " + Dec(both.vec_vl[1]) + ")");
    reporter.Check(both.vec_vl[0] != both.vec_vl[1], "the two harts' vl values differ");
    reporter.Check(!both.vec_vill[0] && !both.vec_vill[1], "neither hart's vtype is illegal");
    bool vec_ok = true;
    for (unsigned h = 0; h < 2; h++) {
      const unsigned n = unsigned(both.vec_vl[h]);
      for (unsigned i = 0; i < n && i < 4; i++) {
        const uint32_t want = kVecA[h][i] + kVecA[h][i];
        if (both.vec_c[h][i] != want) vec_ok = false;
      }
    }
    reporter.Check(vec_ok, "each hart's vector store holds its own doubled source elements");

    // ------------------------------------------------------------- 8. trap state
    bool h0_sync = false, h0_irq = false, h1_any = false;
    for (const auto& t : both.traps[0]) {
      if (t.irq) h0_irq = true;
      else if (t.cause == kExcLoadPage) h0_sync = true;
    }
    for (const auto& t : both.traps[1]) { (void)t; h1_any = true; }
    reporter.Check(h0_sync && both.trap_cause[0] == kExcLoadPage &&
                       both.trap_tval[0] == kVaTrapBase,
                   "hart 0's translated load faulted with the page-fault cause and its own VA");
    reporter.Check(!h1_any && both.trap_cause[1] == 0,
                   "hart 1 took no trap: the same load at the same PC is mapped for it");
    const RetireRec* h0_load = FindPc(both.stream[0], prog.trap_load_pc);
    const RetireRec* h1_load = FindPc(both.stream[1], prog.trap_load_pc);
    reporter.Check(h0_load != nullptr && h0_load->trap,
                   "the faulting load retired as a trap on hart 0");
    reporter.Check(h1_load != nullptr && !h1_load->trap,
                   "the same load retired normally on hart 1");
    reporter.Check(both.sig_ld[1] == kData1Value,
                   "hart 1's load returned its own page's value");

    // -------------------------------------------------------------- 9. IRQ state
    reporter.Check(both.trap_irq_ctr[0] > 0 && h0_irq,
                   "hart 0 took the interrupt it raised on itself");
    reporter.Check(both.trap_irq_ctr[1] == 0 && !h1_any,
                   "hart 1 took no interrupt");
    reporter.Check(both.trap_irq_cause[0] == kIrqSwCause,
                   "hart 0's recorded interrupt cause is the machine software interrupt");

    // ------------------------------------------------------- 10/11. WFI, halt
    reporter.Check(both.wfi_seen[0], "hart 0 halted in WFI");
    reporter.Check(!both.wfi_seen[1], "hart 1 never halted in WFI");
    reporter.Check(!both.stopped[0] && !both.stopped[1],
                   "neither hart stopped on an unsupported instruction");
    reporter.Check(both.parked[1], "hart 1 retired its program and parked");
    reporter.Check(both.h1_commit_at_h0_wfi < both.commit_end[1],
                   "hart 1 kept committing after hart 0 halted");
    reporter.Check(solo0.wfi_seen[0], "hart 0 halted in WFI with hart 1 held in reset");
    reporter.Check(solo1.parked[1], "hart 1 parked with hart 0 held in reset");

    // ------------------------------------------------- 12. per-hart reference
    const PhaseObs& ref1 = share_ref ? solo0 : solo1;
    {
      const PhaseObs& ref = ref1;
      const size_t n = FirstDiff(both.stream[1], ref.stream[1]);
      const bool same = (n == both.stream[1].size()) && (n == ref.stream[1].size());
      if (!same) {
        std::string exp = (n < ref.stream[1].size()) ? DumpRec(ref.stream[1][n]) : "<end>";
        std::string got = (n < both.stream[1].size()) ? DumpRec(both.stream[1][n]) : "<end>";
        reporter.Mismatch("stream/h1", exp, got);
      }
      reporter.Check(same, "hart 1's concurrent stream equals its own reference (" +
                               Dec(both.stream[1].size()) + " retires)");
    }
    {
      const size_t n = FirstDiff(both.stream[0], solo0.stream[0]);
      const bool same = (n == both.stream[0].size()) && (n == solo0.stream[0].size());
      if (!same) {
        std::string exp = (n < solo0.stream[0].size()) ? DumpRec(solo0.stream[0][n]) : "<end>";
        std::string got = (n < both.stream[0].size()) ? DumpRec(both.stream[0][n]) : "<end>";
        reporter.Mismatch("stream/h0", exp, got);
      }
      reporter.Check(same, "hart 0's concurrent stream equals its own reference (" +
                               Dec(both.stream[0].size()) + " retires)");
    }

    // -------------------------------------------- 13. ownership and sharing
    reporter.Check(both.mismatch == 0, "the shared service detected no ownership mismatch");
    reporter.Check(both.id_hart_bad == 0,
                   "every packet's own identity named its hart (h0->0, h1->1)");
    reporter.Check(both.req_h0 > 0 && both.req_h1 > 0,
                   "both harts used the one shared memory service (h0=" + Dec(both.req_h0) +
                       ", h1=" + Dec(both.req_h1) + ")");
    reporter.Check(both.contend > 0,
                   "both harts had a request outstanding at the same time (" +
                       Dec(both.contend) + " cycles)");

    detail = "h0=" + Dec(both.stream[0].size()) + " h1=" + Dec(both.stream[1].size()) +
             " retires, fcsr=" + U64(both.fcsr[0]) + "/" + U64(both.fcsr[1]) +
             " vl=" + Dec(both.vec_vl[0]) + "/" + Dec(both.vec_vl[1]) +
             " seed=" + Dec(options.seed);
  } catch (const std::exception& f) {
    reporter.Mismatch("run", "both domains run to their halt",
                      std::string("contract violated: ") + f.what());
    passed = false;
    detail = std::string("contract violated: ") + f.what();
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  const bool ok = passed && (reporter.failures() == 0);
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
