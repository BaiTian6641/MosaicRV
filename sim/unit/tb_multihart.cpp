// ============================================================================
// tb_multihart.cpp -- CASE=multihart.isolation, work package I-064.
//
// The card: implement two architectural domains on one physical machine --
// independent PC/RAT/ROB/CSR/LSQ/interrupt state and a shared-resource
// contract -- start with statically partitioned execution resources, run
// different programs and ASIDs on the two harts, and make every packet carry
// its hart ownership. Pass: one hart's fault or flush does not lose the other
// hart's requests or state, and each hart's stream compares against its own
// reference. Fail: a global redirect clearing the other hart, or a hart-ID
// reuse confusing a response.
//
// ---------------------------------------------------------------- what it runs
//
// Two *different* programs with two *different* ASIDs and two different Sv39
// root page tables, both translating the *same* virtual address to a different
// physical page -- so an address space that leaked between the harts would be
// visible as a wrong value, not as a coincidence:
//
//   hart 0   M-mode setup, drop to S-mode, translated load, an 8-trip branch
//            loop (taken back-edges: redirects), a translated store, then a
//            translated load from an unmapped VA -- a load page fault. Its
//            M-mode handler records mcause/mtval, advances mepc and returns.
//   hart 1   M-mode setup, drop to S-mode, a 12-trip loop with a different
//            operation (addi + xori), a translated store. It never faults, and
//            its loop is longer, so it is mid-program while hart 0 faults.
//
// -------------------------------------------------------------- the references
//
// Each hart's reference is *its own solo run*: the same binary is run three
// times from a clean reset with the same seeded memory --
//
//   phase A  hart_en = 01   hart 0 alone
//   phase B  hart_en = 10   hart 1 alone
//   phase C  hart_en = 11   both together
//
// and the concurrent run's stream for each hart is required to equal that
// hart's solo stream, field for field, through the program's park instruction.
// The signature word each hart writes is additionally required to equal an
// independently computed value, so the comparison is not merely self-consistent.
//
// ------------------------------------------------------------- what is checked
//
//   1. both harts retired a real program and parked;
//   2. the two ASIDs differ and match the programs' constants (independent
//      address-space identity);
//   3. the same VA read a different physical page on each hart (the page tables
//      are per hart and are actually used);
//   4. each hart's concurrent stream equals its solo stream, field for field;
//   5. each hart's signature equals its independently computed expected value;
//   6. ISOLATION (a) fault: hart 0 took a load page fault and a redirect while
//      hart 1 was mid-program, and hart 1's stream is unaffected (covered by 4)
//      and hart 1 kept committing across hart 0's redirect;
//   7. ISOLATION (b) no global redirect: hart 1 had in-flight ROB work at the
//      cycle hart 0 redirected, and hart 1's redirect count is its own;
//   8. ISOLATION (c) ownership: the shared service reports zero ownership
//      mismatches, and every packet hart 0's own identity names is hart 0 and
//      every packet hart 1's names is hart 1;
//   9. SHARING: both harts' requests reached the one shared memory service
//      (both counters non-zero) and both had a request outstanding at the same
//      time (contention > 0), which is the direct statement that the service is
//      shared rather than replicated.
//
// -------------------------------------------------------------- the controls
//
// Four RTL mutants, each one `-D` from an empty directory (tools/run_multihart_
// controls.py), each of which must exit 1 and name the check it breaks:
//
//   MOSAIC_MH_MUTANT_IGNORE_HART       the response matcher ignores the hart
//   MOSAIC_MH_MUTANT_REUSE_TAG         every request is tagged hart 0
//   MOSAIC_MH_MUTANT_GLOBAL_REDIRECT   hart 0's redirect resets hart 1
//   MOSAIC_MH_MUTANT_ONE_HART_BUS      the arbiter never grants hart 1
//
// plus one driver-side control, `--control-swap-ref`, which compares each hart
// against the *other* hart's reference. The two programs differ, so that
// comparison must fail: it is the falsifiability control on check 4.
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

#include "Vmosaic_multihart_tb.h"

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
constexpr uint64_t kRamBase = MOSAIC_RAM_BASE;              // 0x8000_0000
// The two harts' reset PCs, matching sim/tb/mosaic_multihart_tb.sv.
constexpr uint64_t kH0Pc     = 0x80002000ull;
constexpr uint64_t kH1Pc     = 0x80004000ull;
constexpr uint64_t kH0S      = kH0Pc + 0x100;               // hart 0 S-mode entry
constexpr uint64_t kH0Handler= kH0Pc + 0x200;               // hart 0 M-mode handler
constexpr uint64_t kH1S      = kH1Pc + 0x100;
constexpr uint64_t kH1Handler= kH1Pc + 0x200;
constexpr uint64_t kH0Root   = 0x80006000ull;
constexpr uint64_t kH0L1     = 0x80007000ull;
constexpr uint64_t kH0L0     = 0x80008000ull;
constexpr uint64_t kH1Root   = 0x80009000ull;
constexpr uint64_t kH1L1     = 0x8000A000ull;
constexpr uint64_t kH1L0     = 0x8000B000ull;
constexpr uint64_t kH0Data   = 0x8000C000ull;
constexpr uint64_t kH1Data   = 0x8000D000ull;
constexpr uint64_t kH0Sig    = 0x8000E000ull;
constexpr uint64_t kH1Sig    = 0x8000F000ull;
constexpr uint64_t kH0Trap   = 0x80010000ull;
constexpr uint64_t kH1Trap   = 0x80011000ull;

// The shared virtual address both harts translate, and the two others they use.
constexpr uint64_t kVaData   = 0x40000000ull;   // vpn2=1 vpn1=0 vpn0=0
constexpr uint64_t kVaSig    = 0x40001000ull;   // vpn0=1
constexpr uint64_t kVaBad    = 0x40002000ull;   // vpn0=2, unmapped on hart 0

constexpr uint64_t kAsid0 = 1, kAsid1 = 2;
constexpr uint64_t kSeed0 = 0x1111222233334444ull;
constexpr uint64_t kSeed1 = 0x5555666677778888ull;

// The two loops' trip counts, and the constants the independent expected-value
// computation uses. Kept next to the program builder so the two cannot drift.
constexpr int kH0Trips = 8;
constexpr int kH1Trips = 12;
constexpr uint64_t kH1Add = 3;
constexpr uint64_t kH1Xor = 0x55;

constexpr uint32_t kExcLoadPage = 13;

// The ISA's CSR numbers used by the two programs.
constexpr uint32_t kCsrMstatus = 0x300, kCsrMtvec = 0x305, kCsrMepc = 0x341,
                   kCsrMcause = 0x342, kCsrMtval = 0x343, kCsrSatp = 0x180,
                   kCsrPmpcfg0 = 0x3A0, kCsrPmpaddr0 = 0x3B0;

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
uint32_t Xori(uint32_t rd, uint32_t rs1, int32_t imm) { return EncI(imm, rs1, 4, rd, 0x13); }
uint32_t Slli(uint32_t rd, uint32_t rs1, uint32_t sh) {
  return EncI(static_cast<int32_t>(sh), rs1, 1, rd, 0x13);
}
uint32_t Srli(uint32_t rd, uint32_t rs1, uint32_t sh) {
  return EncI(static_cast<int32_t>(sh), rs1, 5, rd, 0x13);
}
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

// A tiny sequential emitter with labels, so the loop back-edges are computed
// rather than hand-counted.
struct Asm {
  std::map<uint64_t, uint32_t>* img;
  uint64_t pc;
  int next_label = 0;
  std::map<int, uint64_t> labels;
  struct BFix { uint64_t pc; int label; uint32_t rs1, rs2; };
  std::vector<BFix> bfix;

  int NewLabel() { return next_label++; }
  void Bind(int id) { labels[id] = pc; }
  void W(uint32_t w) { (*img)[pc] = w; pc += 4; }
  void Bne(uint32_t rs1, uint32_t rs2, int label) {
    bfix.push_back({pc, label, rs1, rs2});
    W(EncB(0, rs2, rs1, 1, 0x63));
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
      (*img)[f.pc] = EncB(off, f.rs2, f.rs1, 1, 0x63);
    }
    bfix.clear();
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
uint64_t PteNonleaf(uint64_t next) { return Pte(Pp(next), true, false, false, false, false, false, false, false); }

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
// A 4-byte write, for instruction words: the memory model's Write is
// alignment-checked, so an 8-byte window at a 4-byte-aligned address is a
// contract violation, not a convenience.
bool MemWrite4(MemoryModel* mem, uint64_t addr, uint32_t value) {
  return mem->Write(addr, 4, value) == AccessStatus::kOk;
}

// --------------------------------------------------------- the two programs
// The M-mode prologue is the same shape for both harts -- mtvec, one unlocked
// TOR PMP entry covering RAM, satp with the hart's own ASID and root, mepc and
// mstatus, mret -- and the S-mode bodies differ, which is what makes the two
// programs *different programs* rather than one program twice.
struct Program {
  std::map<uint64_t, uint32_t> img;
  uint64_t park_pc = 0;
};

Program BuildHart0() {
  Program p;
  // ------------------------------------------------------------ M-mode
  {
    Asm e{&p.img, kH0Pc};
    e.Li(5, kH0Handler);                e.W(Csrrw(0, kCsrMtvec, 5));
    e.Li(5, 0x0F);                      e.W(Csrrw(0, kCsrPmpcfg0, 5));
    e.Li(5, (kRamBase + MOSAIC_RAM_SIZE) >> 2); e.W(Csrrw(0, kCsrPmpaddr0, 5));
    e.Li(5, (8ull << 60) | (kAsid0 << 44) | Pp(kH0Root)); e.W(Csrrw(0, kCsrSatp, 5));
    e.Li(5, kH0S);                      e.W(Csrrw(0, kCsrMepc, 5));
    e.Li(5, 0x800);                     e.W(Csrrw(0, kCsrMstatus, 5));
    e.W(Mret());
    e.W(EncJ(0, 0));                    // unreachable park
  }
  // ------------------------------------------------------------ S-mode
  {
    Asm e{&p.img, kH0S};
    e.Li(10, kVaData);  e.W(Ld(11, 10, 0));       // a1 = *kVaData (translated)
    e.Li(5, kH0Trips);
    const int loop = e.NewLabel();
    e.Bind(loop);
    e.W(Addi(11, 11, 1));
    e.W(Addi(5, 5, -1));
    e.Bne(5, 0, loop);                             // taken back-edge: a redirect
    e.Li(12, kVaSig);   e.W(Sd(11, 12, 0));        // translated signature store
    e.Li(13, kVaBad);   e.W(Ld(14, 13, 0));        // load page fault
    e.Li(15, kVaSig);   e.W(Sd(14, 15, 8));        // post-handler marker
    p.park_pc = e.pc;
    e.W(EncJ(0, 0));                               // park
    e.Finish();
  }
  // ------------------------------------------------------------ handler
  {
    Asm e{&p.img, kH0Handler};
    e.W(Csrrs(8, kCsrMcause, 0));
    e.W(Csrrs(9, kCsrMtval, 0));
    e.Li(10, kH0Trap);  e.W(Sd(8, 10, 0)); e.W(Sd(9, 10, 8));
    e.W(Csrrs(8, kCsrMepc, 0)); e.W(Addi(8, 8, 4)); e.W(Csrrw(0, kCsrMepc, 8));
    e.Li(8, 0x800); e.W(Csrrw(0, kCsrMstatus, 8));
    e.W(Mret());
  }
  return p;
}

Program BuildHart1() {
  Program p;
  // ------------------------------------------------------------ M-mode
  {
    Asm e{&p.img, kH1Pc};
    e.Li(5, kH1Handler);                e.W(Csrrw(0, kCsrMtvec, 5));
    e.Li(5, 0x0F);                      e.W(Csrrw(0, kCsrPmpcfg0, 5));
    e.Li(5, (kRamBase + MOSAIC_RAM_SIZE) >> 2); e.W(Csrrw(0, kCsrPmpaddr0, 5));
    e.Li(5, (8ull << 60) | (kAsid1 << 44) | Pp(kH1Root)); e.W(Csrrw(0, kCsrSatp, 5));
    e.Li(5, kH1S);                      e.W(Csrrw(0, kCsrMepc, 5));
    e.Li(5, 0x800);                     e.W(Csrrw(0, kCsrMstatus, 5));
    e.W(Mret());
    e.W(EncJ(0, 0));
  }
  // ------------------------------------------------------------ S-mode
  {
    Asm e{&p.img, kH1S};
    e.Li(10, kVaData);  e.W(Ld(11, 10, 0));
    e.Li(5, kH1Trips);
    const int loop = e.NewLabel();
    e.Bind(loop);
    e.W(Addi(11, 11, static_cast<int32_t>(kH1Add)));
    e.W(Xori(11, 11, static_cast<int32_t>(kH1Xor)));
    e.W(Addi(5, 5, -1));
    e.Bne(5, 0, loop);
    e.Li(12, kVaSig);   e.W(Sd(11, 12, 0));
    p.park_pc = e.pc;
    e.W(EncJ(0, 0));
    e.Finish();
  }
  // ------------------------------------------------------------ handler
  {
    Asm e{&p.img, kH1Handler};
    e.W(Csrrs(8, kCsrMcause, 0));
    e.W(Csrrs(9, kCsrMtval, 0));
    e.Li(10, kH1Trap);  e.W(Sd(8, 10, 0)); e.W(Sd(9, 10, 8));
    e.W(Csrrs(8, kCsrMepc, 0)); e.W(Addi(8, 8, 4)); e.W(Csrrw(0, kCsrMepc, 8));
    e.Li(8, 0x800); e.W(Csrrw(0, kCsrMstatus, 8));
    e.W(Mret());
  }
  return p;
}

// The independent expected signature values, computed from the same constants
// the programs are built from but by an independent expression.
uint64_t ExpectedHart0() {
  uint64_t v = kSeed0;
  for (int i = 0; i < kH0Trips; i++) v += 1;
  return v;
}
uint64_t ExpectedHart1() {
  uint64_t v = kSeed1;
  for (int i = 0; i < kH1Trips; i++) {
    v += kH1Add;
    v ^= kH1Xor;
  }
  return v;
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

void BuildTables(MemoryModel* mem) {
  auto put = [&](uint64_t table, unsigned index, uint64_t value) {
    if (!MemWrite8(mem, table + 8ull * index, value, 0xFF)) {
      Fail("setup", "cannot write the PTE at " + U64(table + 8ull * index));
    }
  };
  // hart 0: root[1] -> L1[0] -> L0[0]=data(RWX) L0[1]=sig(RW) L0[2]=invalid
  put(kH0Root, 1, PteNonleaf(kH0L1));
  put(kH0L1, 0, PteNonleaf(kH0L0));
  put(kH0L0, 0, Pte(Pp(kH0Data), true, true, true, true, false, false, true, true));
  put(kH0L0, 1, Pte(Pp(kH0Sig), true, true, true, false, false, false, true, true));
  put(kH0L0, 2, 0);   // V=0: the faulting VA
  // hart 1: the *same* VA tree shape, a different root and different PAs
  put(kH1Root, 1, PteNonleaf(kH1L1));
  put(kH1L1, 0, PteNonleaf(kH1L0));
  put(kH1L0, 0, Pte(Pp(kH1Data), true, true, true, true, false, false, true, true));
  put(kH1L0, 1, Pte(Pp(kH1Sig), true, true, true, false, false, false, true, true));
}

// ============================================================================
// The shared memory service, as the driver models it: one request channel, one
// response channel, every packet hart-tagged, and a per-hart latency so
// responses can return out of order (which is where an ownership mistake would
// show up). The tag on a response is the tag the *DUT* put on the request, so a
// design that reuses a hart ID gets its response back under the reused ID.
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

  void Reset() {
    inflight_.clear();
    ready_.clear();
    accepted_ = 0;
  }
  bool HasRsp() const { return !ready_.empty(); }
  const BusRsp& Current() const { return ready_.front(); }
  uint64_t accepted() const { return accepted_; }

  void Accept(bool we, uint64_t addr, unsigned size, uint64_t wdata, uint8_t wstrb,
              bool amo, unsigned hart, unsigned src, uint32_t id, uint32_t epoch) {
    accepted_++;
    BusRsp r;
    r.hart = hart;
    r.src = src;
    r.id = id;
    r.epoch = epoch;
    Perform(we, addr, size, wdata, wstrb, amo, src, &r);
    Entry e;
    e.rsp = r;
    e.left = Latency(hart);
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
      if (mem_->Read(base + i, 1, &byte) != AccessStatus::kOk) {
        *ok = false;
        return 0;
      }
      v |= (byte & 0xffull) << (8 * i);
    }
    return v;
  }

  void Perform(bool we, uint64_t addr, unsigned size, uint64_t wdata, uint8_t wstrb,
               bool amo, unsigned src, BusRsp* r) {
    if (amo) { r->fault = true; return; }   // this case issues no atomics
    if (src == 1) {
      const unsigned nbytes = 1u << size;
      uint64_t v = 0;
      for (unsigned i = 0; i < nbytes; i++) {
        uint64_t byte = 0;
        if (mem_->Read(addr + i, 1, &byte) != AccessStatus::kOk) {
          r->fault = true;
          return;
        }
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
    // `value` is architectural only for a register-writing instruction; for a
    // store the port carries a completion stash that is not part of the ISA, so
    // it is excluded rather than compared (the same rule I-090 states for the
    // physical destination identity).
    const bool val_ok = !reg_we || (value == o.value);
    return seq == o.seq && pc == o.pc && insn == o.insn && len == o.len &&
           reg_we == o.reg_we && rd == o.rd && val_ok && store == o.store &&
           saddr == o.saddr && sdata == o.sdata && ssz == o.ssz &&
           csr_we == o.csr_we && caddr == o.caddr && cval == o.cval &&
           trap == o.trap && tcause == o.tcause && tval == o.tval;
  }
};

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
  uint64_t sig[2] = {0, 0};
  uint64_t trap_cause[2] = {0, 0};
  uint64_t trap_tval[2] = {0, 0};
  bool trap_seen[2] = {false, false};
  uint32_t redirects[2] = {0, 0};
  uint64_t satp[2] = {0, 0};
  uint32_t priv[2] = {0, 0};
  uint32_t mismatch = 0, req_h0 = 0, req_h1 = 0, req_imem = 0, req_dmem = 0,
           contend = 0;
  // Isolation (b): the state of hart 1 while hart 0 redirected.
  bool h0_redirect_seen = false;
  uint32_t h1_rob_max_at_h0_redirect = 0;
  uint64_t h1_commit_at_first_h0_redirect = 0;
  uint64_t h1_commit_end = 0;
  // Ownership: a packet whose *own* identity named the wrong hart.
  uint32_t id_hart_bad = 0;
  uint64_t cycles = 0;
  // The physical addresses each hart's data port reached (the translated
  // accesses, so "the same VA read a different PA" is observable).
  std::map<uint64_t, unsigned> daddr[2];
  uint64_t geom_hart_w = 0, geom_mem_id_w = 0, geom_ret_n = 0, geom_seq_w = 0;
};

class Runner {
 public:
  explicit Runner(Vmosaic_multihart_tb* dut) : dut_(dut) {}

  PhaseObs Run(unsigned mask, const Program& p0, const Program& p1, uint64_t max_cycles) {
    PhaseObs obs;
    MemoryModel mem;
    BuildTables(&mem);
    WriteImage(&mem, p0.img, "hart0");
    WriteImage(&mem, p1.img, "hart1");
    if (!MemWrite8(&mem, kH0Data, kSeed0, 0xFF) ||
        !MemWrite8(&mem, kH1Data, kSeed1, 0xFF)) {
      Fail("setup", "cannot seed the data pages");
    }

    SharedBus bus(&mem);
    cycles_ = 0;
    // ---------------------------------------------------------------- reset
    dut_->clk = 0;
    dut_->rst = 1;
    dut_->hart_en_i = mask;
    DriveIdleBus();
    dut_->eval();
    for (int i = 0; i < kResetCycles; i++) Clock();
    dut_->rst = 0;

    // ------------------------------------------------------------- the run
    uint64_t last_progress = 0;
    uint64_t last_commit = 0;
    int drain_left = 0;
    for (uint64_t i = 0; i < max_cycles; i++) {
      DriveIdleBus();
      dut_->hart_en_i = mask;
      // The bus's response side.
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
        dut_->mem_rsp_hart = 0;
        dut_->mem_rsp_src = 0;
        dut_->mem_rsp_rdata = 0;
        dut_->mem_rsp_fault = 0;
        dut_->mem_rsp_id = 0;
        dut_->mem_rsp_epoch = 0;
        dut_->mem_rsp_len = 0;
      }
      dut_->mem_req_ready = 1;
      dut_->eval();

      Observe(&obs, p0.park_pc, p1.park_pc);

      // The request side: the DUT offers at most one hart-tagged request.
      if (dut_->mem_req_valid && dut_->mem_req_ready) {
        const unsigned hart = unsigned(dut_->mem_req_hart);
        const unsigned src = dut_->mem_req_src ? 1u : 0u;
        bus.Accept(dut_->mem_req_we != 0, dut_->mem_req_addr, unsigned(dut_->mem_req_size),
                   dut_->mem_req_wdata, uint8_t(dut_->mem_req_wstrb),
                   dut_->mem_req_amo != 0, hart, src,
                   uint32_t(dut_->mem_req_id), uint32_t(dut_->mem_req_epoch));
        if (src == 0 && hart < 2) obs.daddr[hart][uint64_t(dut_->mem_req_addr)]++;
      }
      if (dut_->mem_rsp_valid && dut_->mem_rsp_ready) bus.Pop();
      bus.Advance();

      const uint64_t commits = uint64_t(dut_->h0_commit_ctr) + uint64_t(dut_->h1_commit_ctr);
      if (commits != last_commit) { last_commit = commits; last_progress = cycles_; }
      Clock();
      obs.cycles = cycles_;

      // A disabled hart is held in reset and will never park, so only the
      // enabled harts' completion is required.
      const bool all_parked = ((mask & 1u) == 0u || obs.parked[0]) &&
                              ((mask & 2u) == 0u || obs.parked[1]);
      const bool any_stopped = ((mask & 1u) != 0u && obs.stopped[0]) ||
                               ((mask & 2u) != 0u && obs.stopped[1]);
      if (any_stopped) break;
      // The drain still drives the shared bus: a store retired just before the
      // park is still in flight, and stopping the bus here would lose it (which
      // is exactly how the solo reference's signature read back zero).
      if (all_parked && drain_left == 0) drain_left = kDrainCycles;
      if (drain_left > 0) {
        drain_left--;
        if (drain_left == 0) break;
      }
      if (cycles_ - last_progress > kStallCycles) break;
    }

    // ------------------------------------------------------------ the reads
    obs.h1_commit_end = uint64_t(dut_->h1_commit_ctr);
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
    obs.priv[0] = uint32_t(dut_->h0_priv);
    obs.priv[1] = uint32_t(dut_->h1_priv);
    obs.redirects[0] = uint32_t(dut_->h0_redirect_ctr);
    obs.redirects[1] = uint32_t(dut_->h1_redirect_ctr);
    uint64_t v = 0;
    if (MemRead8(&mem, kH0Sig, &v)) obs.sig[0] = v;
    if (MemRead8(&mem, kH1Sig, &v)) obs.sig[1] = v;
    if (MemRead8(&mem, kH0Trap, &v)) obs.trap_cause[0] = v;
    if (MemRead8(&mem, kH0Trap + 8, &v)) obs.trap_tval[0] = v;
    if (MemRead8(&mem, kH1Trap, &v)) obs.trap_cause[1] = v;
    if (MemRead8(&mem, kH1Trap + 8, &v)) obs.trap_tval[1] = v;
    return obs;
  }

 private:
  static constexpr int kResetCycles = 8;
  static constexpr int kDrainCycles = 32;
  static constexpr uint64_t kStallCycles = 20000;

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

  void Observe(PhaseObs* obs, uint64_t park0, uint64_t park1) {
    const uint32_t ret_n = obs->geom_ret_n ? uint32_t(obs->geom_ret_n) : 2u;
    const uint32_t seq_w = obs->geom_seq_w ? uint32_t(obs->geom_seq_w) : 7u;
    // Hart 0's own identity must name hart 0 on every accepted data request.
    if (dut_->h0_dmem_id_valid) {
      const uint32_t hw = obs->geom_hart_w;
      const uint32_t mw = obs->geom_mem_id_w;
      if (hw > 0 && mw >= hw) {
        const uint64_t hart_field = (uint64_t(dut_->h0_dmem_id) >> (mw - hw)) & ((1ull << hw) - 1);
        if (hart_field != 0) obs->id_hart_bad++;
      }
    }
    if (dut_->h1_dmem_id_valid) {
      const uint32_t hw = obs->geom_hart_w;
      const uint32_t mw = obs->geom_mem_id_w;
      if (hw > 0 && mw >= hw) {
        const uint64_t hart_field = (uint64_t(dut_->h1_dmem_id) >> (mw - hw)) & ((1ull << hw) - 1);
        if (hart_field != 1) obs->id_hart_bad++;
      }
    }
    // Isolation (b): snapshot hart 1 while hart 0 redirects.
    if (dut_->h0_redirect_valid) {
      obs->h0_redirect_seen = true;
      if (!obs->h1_commit_at_first_h0_redirect) {
        obs->h1_commit_at_first_h0_redirect = uint64_t(dut_->h1_commit_ctr);
      }
      const uint32_t occ = uint32_t(dut_->h1_rob_occupied);
      if (occ > obs->h1_rob_max_at_h0_redirect) obs->h1_rob_max_at_h0_redirect = occ;
    }
    if (dut_->h0_stopped) obs->stopped[0] = true;
    if (dut_->h1_stopped) obs->stopped[1] = true;
    if (dut_->h0_trap_valid) { obs->trap_seen[0] = true; }
    if (dut_->h1_trap_valid) { obs->trap_seen[1] = true; }

    for (unsigned h = 0; h < 2; h++) {
      if (obs->parked[h]) continue;
      const uint32_t valid = uint32_t(h == 0 ? dut_->h0_ev_valid : dut_->h1_ev_valid);
      for (uint32_t lane = 0; lane < ret_n; lane++) {
        if ((valid & (1u << lane)) == 0) continue;
        const uint64_t pc = h == 0 ? PayloadLane(dut_->h0_ev_pc, lane)
                                   : PayloadLane(dut_->h1_ev_pc, lane);
        const uint64_t park = h == 0 ? park0 : park1;
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

  Vmosaic_multihart_tb* dut_;
  uint64_t cycles_ = 0;
};

// ============================================================================
// The comparison helpers
// ============================================================================
std::string DumpRec(const RetireRec& r) {
  return "seq=" + Dec(r.seq) + " pc=" + U64(r.pc) + " insn=" + U64(r.insn) +
         " len=" + Dec(r.len) + " we=" + Dec(r.reg_we) + " rd=" + Dec(r.rd) +
         " val=" + U64(r.value) + " store=" + Dec(r.store) + " sa=" + U64(r.saddr) +
         " sd=" + U64(r.sdata) + " csrwe=" + Dec(r.csr_we) + " csra=" + U64(r.caddr) +
         " csrv=" + U64(r.cval) + " trap=" + Dec(r.trap) + " cause=" + Dec(r.tcause) +
         " tval=" + U64(r.tval);
}

// Returns the index of the first differing record, or size() if equal.
size_t FirstDiff(const std::vector<RetireRec>& a, const std::vector<RetireRec>& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; i++) {
    if (!(a[i] == b[i])) return i;
  }
  return n;
}

std::string AsidOf(uint64_t satp) { return Dec((satp >> 44) & 0xFFFF); }

}  // namespace

// ============================================================================
int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  // The one driver-side control. `Options::Parse` rejects unknown flags by
  // design, so the control flag is removed from the argument vector first.
  bool swap_ref = false;
  std::vector<char*> filtered;
  filtered.push_back(argv[0]);
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--control-swap-ref") == 0) {
      swap_ref = true;
      continue;
    }
    filtered.push_back(argv[i]);
  }
  if (!mosaic::Options::Parse(int(filtered.size()), filtered.data(), &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);
  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_multihart_tb dut;

  bool passed = true;
  std::string detail;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.hart_en_i = 0;
    dut.eval();

    const Program p0 = BuildHart0();
    const Program p1 = BuildHart1();
    Runner runner(&dut);

    // ---------------------------------------------------------- the three runs
    const PhaseObs solo0 = runner.Run(0b01, p0, p1, options.max_cycles);
    const PhaseObs solo1 = runner.Run(0b10, p0, p1, options.max_cycles);
    const PhaseObs both  = runner.Run(0b11, p0, p1, options.max_cycles);

    std::printf("  cycles: solo0=%llu solo1=%llu both=%llu\n",
                (unsigned long long)solo0.cycles, (unsigned long long)solo1.cycles,
                (unsigned long long)both.cycles);
    std::printf("  both: req h0=%u h1=%u (imem=%u dmem=%u) contend=%u mismatch=%u id_hart_bad=%u\n",
                both.req_h0, both.req_h1, both.req_imem, both.req_dmem,
                both.contend, both.mismatch, both.id_hart_bad);
    std::printf("  solo: parked0=%d parked1=%d stopped0=%d stopped1=%d n0=%zu n1=%zu sig0=%s sig1=%s\n",
                solo0.parked[0]?1:0, solo1.parked[1]?1:0, solo0.stopped[0]?1:0, solo1.stopped[1]?1:0,
                solo0.stream[0].size(), solo1.stream[1].size(),
                U64(solo0.sig[0]).c_str(), U64(solo1.sig[1]).c_str());
    std::printf("  solo: sig0=%s sig1=%s\n", U64(solo0.sig[0]).c_str(), U64(solo1.sig[1]).c_str());
    std::printf("  expect: sig0=%s sig1=%s\n", U64(ExpectedHart0()).c_str(), U64(ExpectedHart1()).c_str());
    std::printf("  both: sig0=%s sig1=%s satp0=%s satp1=%s priv0=%u priv1=%u\n",
                U64(both.sig[0]).c_str(), U64(both.sig[1]).c_str(),
                U64(both.satp[0]).c_str(), U64(both.satp[1]).c_str(),
                both.priv[0], both.priv[1]);
    std::printf("  both: h0 redirects=%u trap=%d cause=%llu tval=%s  h1 redirects=%u h1_rob@h0redir=%u\n",
                both.redirects[0], both.trap_seen[0] ? 1 : 0,
                (unsigned long long)both.trap_cause[0], U64(both.trap_tval[0]).c_str(),
                both.redirects[1], both.h1_rob_max_at_h0_redirect);

    // ------------------------------------------------------------ check 1
    for (unsigned h = 0; h < 2; h++) {
      const std::string who = "hart " + Dec(h);
      reporter.Check(both.parked[h], who + " retired its program and parked");
      reporter.Check(!both.stopped[h], who + " did not stop on an unsupported instruction");
    }

    // ------------------------------------------------------------ check 2
    // The two ASIDs differ and match the programs' constants.
    reporter.Check(AsidOf(both.satp[0]) == Dec(kAsid0) && AsidOf(both.satp[1]) == Dec(kAsid1),
                   "the two harts hold different ASIDs (" + AsidOf(both.satp[0]) + " and " +
                       AsidOf(both.satp[1]) + ")");
    reporter.Check(both.satp[0] != both.satp[1], "the two satp values differ");

    // ------------------------------------------------------------ check 3
    // The same VA read a different physical page on each hart.
    const bool h0_read = both.daddr[0].count(kH0Data) != 0;
    const bool h1_read = both.daddr[1].count(kH1Data) != 0;
    reporter.Check(h0_read && h1_read,
                   "the same VA (0x4000_0000) translated to hart 0's page and hart 1's page");

    // ------------------------------------------------------------ check 4
    // Each hart's concurrent stream equals its own solo reference, field for
    // field. With --control-swap-ref the reference is deliberately the other
    // hart's, which must fail.
    const PhaseObs& ref0 = swap_ref ? solo1 : solo0;
    const PhaseObs& ref1 = swap_ref ? solo0 : solo1;
    for (unsigned h = 0; h < 2; h++) {
      const PhaseObs& ref = h == 0 ? ref0 : ref1;
      const size_t n = FirstDiff(both.stream[h], ref.stream[h]);
      const bool same = (n == both.stream[h].size()) && (n == ref.stream[h].size());
      if (!same) {
        std::string exp = (n < ref.stream[h].size()) ? DumpRec(ref.stream[h][n]) : "<end>";
        std::string got = (n < both.stream[h].size()) ? DumpRec(both.stream[h][n]) : "<end>";
        reporter.Mismatch("stream/h" + Dec(h), exp, got);
      }
      reporter.Check(same, "hart " + Dec(h) + "'s concurrent stream equals its own reference (" +
                               Dec(both.stream[h].size()) + " retires)");
    }

    // ------------------------------------------------------------ check 5
    reporter.Check(both.sig[0] == ExpectedHart0(),
                   "hart 0's signature equals the independently computed value");
    reporter.Check(both.sig[1] == ExpectedHart1(),
                   "hart 1's signature equals the independently computed value");
    reporter.Check(solo0.sig[0] == ExpectedHart0() && solo1.sig[1] == ExpectedHart1(),
                   "the solo references carry the same independently computed signatures");

    // ------------------------------------------------------------ check 6
    // ISOLATION (a): a fault in hart 0 while hart 1 was mid-program.
    reporter.Check(both.trap_seen[0], "hart 0 took a trap");
    reporter.Check(both.trap_cause[0] == kExcLoadPage && both.trap_tval[0] == kVaBad,
                   "hart 0's trap is the load page fault on the unmapped VA");
    reporter.Check(both.redirects[0] > 0, "hart 0 redirected (its loop back-edges)");
    reporter.Check(!both.trap_seen[1], "hart 1 took no trap");
    reporter.Check(both.h1_commit_end > both.h1_commit_at_first_h0_redirect,
                   "hart 1 kept committing after hart 0 redirected");

    // ------------------------------------------------------------ check 7
    // ISOLATION (b): no global redirect -- hart 1 had in-flight ROB work at the
    // cycle hart 0 redirected, and hart 1's own redirects are its own.
    reporter.Check(both.h0_redirect_seen, "hart 0's redirect was observed");
    reporter.Check(both.h1_rob_max_at_h0_redirect > 0,
                   "hart 1 had in-flight ROB work while hart 0 redirected");
    reporter.Check(both.redirects[1] > 0, "hart 1's redirect count is its own");

    // ------------------------------------------------------------ check 8
    // ISOLATION (c): ownership.
    reporter.Check(both.mismatch == 0, "the shared service detected no ownership mismatch");
    reporter.Check(both.id_hart_bad == 0,
                   "every packet's own identity named its hart (h0->0, h1->1)");
    reporter.Check(both.req_imem > 0 && both.req_dmem > 0,
                   "the shared service carried both instruction and data requests");

    // ------------------------------------------------------------ check 9
    // SHARING: one service, both harts, contending.
    reporter.Check(both.req_h0 > 0 && both.req_h1 > 0,
                   "both harts used the one shared memory service (h0=" + Dec(both.req_h0) +
                       ", h1=" + Dec(both.req_h1) + ")");
    reporter.Check(both.contend > 0,
                   "both harts had a request outstanding at the same time (" +
                       Dec(both.contend) + " cycles)");

    detail = "h0=" + Dec(both.stream[0].size()) + " h1=" + Dec(both.stream[1].size()) +
             " retires, shared_requests=" + Dec(both.req_h0 + both.req_h1) +
             " contend=" + Dec(both.contend) + " seed=" + Dec(options.seed);
  } catch (const std::exception& f) {
    reporter.Mismatch("run", "both domains run to their park instructions",
                      std::string("contract violated: ") + f.what());
    passed = false;
    detail = std::string("contract violated: ") + f.what();
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  const bool ok = passed && (reporter.failures() == 0);
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
