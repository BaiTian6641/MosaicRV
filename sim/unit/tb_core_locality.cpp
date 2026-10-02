// ============================================================================
// tb_core_locality.cpp -- CASE=locality.integrated_path, work package I-060.
//
// The DUT is the integrated core (the same `mosaic_core_tb` every core case
// drives) with the data cache enabled and the two locality structures wired into
// its line port: the clean-copy locality buffer (rtl/core/mosaic_llb.sv) and the
// bounded prefetcher (rtl/core/mosaic_prefetch.sv), through
// rtl/core/mosaic_locality_path.sv, inside rtl/core/mosaic_l1_cache_path.sv.
//
// ---------------------------------------------------------------- the phases
//
// 1. **On/off equivalence, one variable at a time.** The same program runs four
//    times from a fresh reset: baseline (both off), LLB only, prefetcher only,
//    both. The *architectural* results must be identical in all four: the
//    retirement stream (pc, length, destination, value, in order) and the
//    signature words. That is the case's central claim -- a locality structure
//    may change the traffic and the cycles, never the answer.
//
// 2. **The structures are used.** The LLB's hit counter and the prefetcher's
//    issued/useful counters are read out of the DUT and *asserted* non-zero in
//    the runs that switch them on. A structure wired in but never engaged is the
//    silent failure this case exists to make loud -- exactly as a cache that
//    never hits was.
//
// 3. **The per-step table.** Traffic (memory-side beats and cache line
//    transactions) and cycles are printed for the four runs, so the comparison
//    is one variable at a time on equal resources. A structure that is neutral
//    or harmful on this program is reported as such rather than averaged away.
//
// 4. **The access-class rule is checkable.** The program brackets a store's
//    read-for-ownership with marker device writes; the case reads the LLB's
//    lookup counters at each marker and requires the store's line fill *not* to
//    have consulted the buffer, with the neighbouring load as its own positive
//    control. That is what `MOSAIC_LOC_MUTANT_PERM_BYPASS` breaks.
//
// 5. **The store rule is checkable architecturally.** The program fills a line
//    into the buffer, cleanly evicts it from the L1, writes the line with an AMO
//    (which bypasses the cache and writes memory), and then loads it. The load
//    must see memory. A store that left its line resident in the buffer
//    (`MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE`) serves the pre-store value, and the
//    signature says so.
//
// 6. **A prefetch never touches a device.** Every data-port request the run makes
//    is checked: no prefetch read is ever presented for an address outside the
//    platform map's normal memory. `MOSAIC_PREFETCH_MUTANT_DEVICE_READ` is caught
//    here; `MOSAIC_PREFETCH_MUTANT_ARCH_DATA`, a corrupted fill, is caught by
//    phase 1 (the signature differs when the structures are on).
//
// ------------------------------------------------------------ what this is not
//
// The numbers are a measurement *of this program on this geometry*: one hart,
// one 8-entry fully associative 64-byte locality buffer, one 16-entry stride
// table of depth 4, a 32-set 4-way 64-byte L1, and a directed stride/locality
// workload. No general hit rate, no energy claim, and no multi-hart coherence
// claim is made or inferable. See results/reports/I-060-locality-integration.md,
// "not covered".
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
#include <vector>

#include "Vmosaic_core_tb.h"
#include "memory_model.h"
#include "sim_common.h"

using mosaic::AccessStatus;
using mosaic::MemoryModel;
using mosaic::Options;
using mosaic::Reporter;

namespace {

constexpr int kResetCycles = 8;
constexpr int kMaxRunCycles = 400000;
// The exit word and the signature are *cacheable* stores: with the cache on the
// harness sees them only when the data cache is written back, which the program
// asks for with a FENCE.I at the end. That flush now walks 32 sets x 4 ways and
// issues the writebacks it finds, so the drain must cover the whole flush for
// both the exit word and the signature to be visible. 64 cycles covered the
// original 8-set direct-mapped L1; the enlarged L1 needs more.
constexpr int kDrainCycles = 1024;

constexpr uint64_t kRamBase = 0x80000000ull;

// The program's data lines. The L1D is 32 sets of 64 bytes, 4 ways, so the set
// of an offset is (off >> 6) & 31, and adding 0x800 (bit 11) to an offset keeps
// the set and names a different line. Four ways means a line leaves the L1 only
// when a fifth distinct line of the same set is installed (round-robin), so the
// program puts every line below in L1 set 17 (0x440 + 0x800*k):
//   reuse loop   A..E   0x440 0xC40 0x1440 0x1C40 0x2440
//   store check  amo    0x2C40 (memory value 100)
//   class check  Ck     0x3440, with fresh same-set fillers 0x3C40..0x5440
// Five lines in a four-way set means every turn of the reuse loop installs the
// line the previous turn evicted, and that access is served by the buffer. The
// class-check subject is then filled into the buffer, evicted from the L1 by
// four fresh fillers (four installs cycle every way), and loaded again.
//
// The prefetch stream is base 0x1FF000 + i*0x100: all set 0, a stride of four
// lines, so every access misses the L1 and the stride is learnable -- and its
// last candidate falls one line past the top of RAM (0x80200000), which is the
// address a prefetch must never reach.
constexpr uint64_t kOffA = 0x440, kOffB = 0xC40, kOffC = 0x1440, kOffD = 0x1C40,
                   kOffE = 0x2440;
constexpr uint64_t kOffAmo = 0x2C40;
constexpr uint64_t kOffCk = 0x3440;
constexpr uint64_t kOffCkF0 = 0x3C40, kOffCkF1 = 0x4440, kOffCkF2 = 0x4C40,
                   kOffCkF3 = 0x5440;
constexpr uint64_t kOffPfBase = 0x0C0;
constexpr uint64_t kPfBase = 0x1FF000;
constexpr int kPfLines = 16;

constexpr uint64_t kUartScratch = 0x0010000Cull;

uint64_t g_park_pc = 0;

// ---------------------------------------------------------------- assembler
uint32_t EncI(int imm, int rs1, int f3, int rd, int op) {
  return (uint32_t(imm & 0xfff) << 20) | (uint32_t(rs1) << 15) | (uint32_t(f3) << 12) |
         (uint32_t(rd) << 7) | uint32_t(op);
}
uint32_t EncS(int imm, int rs2, int rs1, int f3, int op) {
  return (uint32_t((imm >> 5) & 0x7f) << 25) | (uint32_t(rs2) << 20) |
         (uint32_t(rs1) << 15) | (uint32_t(f3) << 12) | (uint32_t(imm & 0x1f) << 7) |
         uint32_t(op);
}
uint32_t EncB(int imm, int rs2, int rs1, int f3, int op) {
  return (uint32_t((imm >> 12) & 1) << 31) | (uint32_t((imm >> 5) & 0x3f) << 25) |
         (uint32_t(rs2) << 20) | (uint32_t(rs1) << 15) | (uint32_t(f3) << 12) |
         (uint32_t((imm >> 1) & 0xf) << 8) | (uint32_t((imm >> 11) & 1) << 7) |
         uint32_t(op);
}
uint32_t EncU(int imm20, int rd, int op) {
  return (uint32_t(imm20) << 12) | (uint32_t(rd) << 7) | uint32_t(op);
}
uint32_t EncR(int f7, int rs2, int rs1, int f3, int rd, int op) {
  return (uint32_t(f7) << 25) | (uint32_t(rs2) << 20) | (uint32_t(rs1) << 15) |
         (uint32_t(f3) << 12) | (uint32_t(rd) << 7) | uint32_t(op);
}

constexpr int kOpAuipc = 0x17, kOpBr = 0x63, kOpLoad = 0x03, kOpStore = 0x23;
constexpr int kOpImm = 0x13, kOpOp = 0x33, kOpAmo = 0x2f;
constexpr int kF3Ld = 3, kF3Sd = 3, kF3Sw = 2, kF3Addi = 0, kF3Add = 0, kF3Blt = 4;

// The AMO: `amoadd.d rd, rs2, (rs1)` -- funct5=00000, aq=0, rl=0, funct3=011.
uint32_t AmoAddD(int rd, int rs2, int rs1) {
  return (uint32_t(0x00) << 27) | (uint32_t(rs2) << 20) | (uint32_t(rs1) << 15) |
         (uint32_t(3) << 12) | (uint32_t(rd) << 7) | uint32_t(kOpAmo);
}

class Asm {
 public:
  int Label() { return next_label_++; }
  void Bind(int label) { labels_[label] = off(); }
  int off() const { return int(code_.size()) * 4; }

  void Auipc(int rd, int imm20) { Emit(EncU(imm20, rd, kOpAuipc)); }
  void Lui(int rd, int imm20) { Emit(EncU(imm20, rd, 0x37)); }
  void Addi(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Addi, rd, kOpImm)); }
  void Add(int rd, int rs1, int rs2) { Emit(EncR(0, rs2, rs1, kF3Add, rd, kOpOp)); }
  void Xor(int rd, int rs1, int rs2) { Emit(EncR(0, rs2, rs1, 4, rd, kOpOp)); }
  void Ld(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Ld, rd, kOpLoad)); }
  void Lw(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, 2, rd, kOpLoad)); }
  void Sd(int rs2, int rs1, int imm) { Emit(EncS(imm, rs2, rs1, kF3Sd, kOpStore)); }
  void Sw(int rs2, int rs1, int imm) { Emit(EncS(imm, rs2, rs1, kF3Sw, kOpStore)); }
  void Amo(int rd, int rs2, int rs1) { Emit(AmoAddD(rd, rs2, rs1)); }
  void FenceI() { Emit(0x0000100fu); }
  void Blt(int rs1, int rs2, int label) {
    bfix_.push_back({int(code_.size()), label, rs1, rs2});
    code_.push_back(0);
  }
  void Jal(int rd, int label) {
    jfix_.push_back({int(code_.size()), label, rd});
    code_.push_back(0);
  }
  void PadTo(int byte_off) {
    while (off() < byte_off) code_.push_back(0x00000013u);  // addi x0,x0,0
  }

  std::vector<uint8_t> Bytes() const {
    std::vector<uint32_t> fixed = code_;
    for (const auto& f : bfix_) {
      const int at = f.word;
      fixed[size_t(at)] = EncB(labels_.at(f.label) - at * 4, f.rs2, f.rs1, kF3Blt, kOpBr);
    }
    for (const auto& f : jfix_) {
      const int at = f.word;
      fixed[size_t(at)] = EncJ(labels_.at(f.label) - at * 4, f.rd);
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(fixed.size() * 4);
    for (uint32_t w : fixed) {
      for (int b = 0; b < 4; b++) bytes.push_back(uint8_t((w >> (8 * b)) & 0xff));
    }
    return bytes;
  }

 private:
  struct BFix { int word, label, rs1, rs2; };
  struct JFix { int word, label, rd; };
  static uint32_t EncJ(int imm, int rd) {
    return (uint32_t((imm >> 20) & 1) << 31) | (uint32_t((imm >> 1) & 0x3ff) << 21) |
           (uint32_t((imm >> 11) & 1) << 20) | (uint32_t((imm >> 12) & 0xff) << 12) |
           (uint32_t(rd) << 7) | uint32_t(0x6f);
  }
  void Emit(uint32_t w) { code_.push_back(w); }
  std::map<int, int> labels_;
  std::vector<BFix> bfix_;
  std::vector<JFix> jfix_;
  std::vector<uint32_t> code_;
  int next_label_ = 0;
};

// ============================================================================
// The bus: one request/response port with a fixed latency, no fault injection.
// ============================================================================
struct Rsp {
  uint64_t rdata = 0;
  bool fault = false;
  uint64_t addr = 0;
};

class Bus {
 public:
  struct Req {
    bool we = false;
    uint64_t addr = 0;
    unsigned size = 0;
    uint64_t wdata = 0;
    uint8_t wstrb = 0;
    bool inst = false;
  };
  struct Txn { Req req; uint64_t cycle = 0; };

  Bus(MemoryModel* mem, int latency) : mem_(mem), latency_(latency < 0 ? 0 : latency) {}

  void Reset() { inflight_.clear(); ready_.clear(); txns_.clear(); }
  bool HasRsp() const { return !ready_.empty(); }
  const Rsp& Current() const { return ready_.front(); }
  const std::vector<Txn>& txns() const { return txns_; }
  uint64_t accepted() const { return txns_.size(); }

  void Accept(const Req& r, uint64_t cycle) {
    txns_.push_back({r, cycle});
    Rsp resp = Perform(r);
    inflight_.push_back({resp, latency_});
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
  struct Entry { Rsp rsp; int left = 0; };

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

  Rsp Perform(const Req& r) {
    Rsp resp;
    resp.addr = r.addr;
    if (r.inst) {
      const unsigned nbytes = 1u << r.size;
      uint64_t v = 0;
      for (unsigned i = 0; i < nbytes; i++) {
        uint64_t byte = 0;
        if (mem_->Read(r.addr + i, 1, &byte) != AccessStatus::kOk) {
          resp.fault = true;
          return resp;
        }
        v |= (byte & 0xffull) << (8 * i);
      }
      resp.rdata = v;
      return resp;
    }
    const uint64_t base = r.addr & ~7ull;
    bool ok = true;
    if (r.we) {
      uint64_t window = ReadWindow(base, &ok);
      if (!ok) {
        resp.fault = true;
        return resp;
      }
      for (unsigned i = 0; i < 8; i++) {
        if (((r.wstrb >> i) & 1u) != 0u) {
          window = (window & ~(0xffull << (8 * i))) |
                   (((r.wdata >> (8 * i)) & 0xffull) << (8 * i));
        }
      }
      const AccessStatus st = mem_->Write(base, 8, window);
      resp.fault = (st != AccessStatus::kOk && st != AccessStatus::kDeviceError);
    } else {
      resp.rdata = ReadWindow(base, &ok);
      if (!ok) resp.fault = true;
    }
    return resp;
  }

  MemoryModel* mem_;
  int latency_;
  std::deque<Entry> inflight_;
  std::deque<Rsp> ready_;
  std::vector<Txn> txns_;
};

// ============================================================================
// The program.
// ============================================================================
std::vector<uint8_t> BuildProgram() {
  Asm a;
  const int x0 = 0, t0 = 5, t1 = 6, t2 = 7, s0 = 8, s1 = 9, a1 = 11, a2 = 12;
  const int a3 = 13, a5 = 15, s2 = 18, s3 = 19, s4 = 20, s5 = 21, t3 = 28, t5 = 30,
            t6 = 31;
  const int t7 = 26, t8 = 27;
  const int s6 = 22, s7 = 23, s8 = 24, s9 = 25;

  // s0 = 0x80000800 (the data base). `auipc` gives the PC-relative high part;
  // the 0x800 adjustment is 0x7ff + 1 because an immediate of 0x800 has bit 11
  // set and is therefore negative.
  a.Auipc(s0, 0);
  a.Addi(s0, s0, 0x7ff);
  a.Addi(s0, s0, 1);

  const int t4 = 29;
  a.Lui(t4, 0x00100);          // t4 = 0x00100000
  a.Addi(t4, t4, 0x00C);       // t4 = 0x0010000C (the UART scratch: the marker port)

  // ---- phase 1: the reuse loop (five lines, one L1 set) -------------------
  // Five lines in a four-way L1 set: the first turn installs A..E and leaves
  // four resident; every access after that installs the line the previous
  // access evicted, so each misses the L1 and is served by the buffer.
  // Eight turns of (7+9+11+13+15) = 440.
  //
  // Each line's address lives in its own register: the lines are 0x800 apart
  // and the Ld immediate only reaches +/-2 KB.
  a.Addi(s6, s0, 0x440);       // A
  a.Addi(s7, s6, 0x7ff);       // B = A + 0x800
  a.Addi(s7, s7, 1);
  a.Addi(s8, s7, 0x7ff);       // C
  a.Addi(s8, s8, 1);
  a.Addi(s9, s8, 0x7ff);       // D
  a.Addi(s9, s9, 1);
  a.Addi(t8, s9, 0x7ff);       // E
  a.Addi(t8, t8, 1);
  a.Addi(a2, x0, 0);           // accumulator
  a.Addi(s1, x0, 0);
  a.Addi(a1, x0, 8);
  const int reuse = a.Label();
  a.Bind(reuse);
  a.Ld(t0, s6, 0);
  a.Add(a2, a2, t0);
  a.Ld(t0, s7, 0);
  a.Add(a2, a2, t0);
  a.Ld(t0, s8, 0);
  a.Add(a2, a2, t0);
  a.Ld(t0, s9, 0);
  a.Add(a2, a2, t0);
  a.Ld(t0, t8, 0);
  a.Add(a2, a2, t0);
  a.Addi(s1, s1, 1);
  a.Blt(s1, a1, reuse);

  // ---- phase 5: the store-invalidation check -----------------------------
  // The load fills the AMO's line into the L1 and the buffer. The AMO bypasses
  // the cache, so the core invalidates that line in both the L1 and the buffer;
  // the load after it must then miss both and see memory (100 + 5 = 105). With
  // MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE the buffer keeps its clean copy and
  // the load returns 100.
  a.Addi(s6, t8, 0x7ff);                 // s6 = the next line of the same set
  a.Addi(s6, s6, 1);                     // s6 = s0 + 0x2C40 (the AMO's line)
  a.Ld(t0, s6, 0);                       // L1 miss -> buffer copy of the AMO's line
  a.Addi(s2, s6, 0);                     // s2 = &AMO line
  a.Addi(t1, x0, 5);
  a.Amo(x0, t1, s2);                     // memory[AMO] += 5; kills L1 and buffer copies
  a.Ld(t2, s6, 0);                       // must see memory (105), not the clean copy

  // ---- phase 2: the prefetch stream (base 0x801FF000, stride 0x100) -------
  // The stream's base is a data word in the image (0x801FF000: near the top of
  // RAM, so the stream's last candidate falls one line past the end).
  a.Ld(s3, s0, int(kOffPfBase));         // s3 = 0x801FF000
  a.Addi(s2, x0, 0);                     // s2 = the iteration index
  a.Addi(a3, x0, kPfLines);
  const int pf = a.Label();
  a.Bind(pf);
  a.Ld(t0, s3, 0);
  a.Add(a2, a2, t0);
  a.Addi(s3, s3, 0x100);
  a.Addi(s2, s2, 1);
  a.Blt(s2, a3, pf);

  // ---- phase 4: the access-class markers ----------------------------------
  // The class check comes last, so that no cacheable *load* can execute after
  // its closing marker and leak into the counted window. Each window is bounded
  // so that out-of-order execution cannot move the bracketed access out of it:
  //
  //   * an opening marker is a device **load**, and the bracketed access's
  //     address is computed from its data (`xor t6,t5,t5` is a real dependency
  //     on t5), so the access cannot execute before the marker's request; and
  //     the marker is serialized, so every older access has already retired.
  //   * a closing marker is a device **load** whose address depends on the
  //     bracketed *load*'s result, or a device **store**, which the store queue
  //     drains in order -- after the bracketed store's own transaction.
  //
  // Window 1 brackets a load that must consult the buffer and hit. Window 2
  // brackets a store whose read-for-ownership must not consult it at all.
  // The subject and its four fillers are the next five lines of the same set,
  // each in its own register (the Ld immediate only reaches +/-2 KB).
  a.Addi(s6, s6, 0x7ff);                 // s6 = s0 + 0x3440 (the subject)
  a.Addi(s6, s6, 1);
  a.Addi(s7, s6, 0x7ff);                 // filler 0
  a.Addi(s7, s7, 1);
  a.Addi(s8, s7, 0x7ff);                 // filler 1
  a.Addi(s8, s8, 1);
  a.Addi(s9, s8, 0x7ff);                 // filler 2
  a.Addi(s9, s9, 1);
  a.Addi(t8, s9, 0x7ff);                 // filler 3
  a.Addi(t8, t8, 1);
  a.Ld(t0, s6, 0);                       // fresh line -> L1 miss, buffer fill
  a.Ld(t0, s7, 0);                       // four fresh same-set fillers: four
  a.Ld(t0, s8, 0);                       // installs cycle every way, so the
  a.Ld(t0, s9, 0);                       // subject leaves the L1 (it stays in
  a.Ld(t0, t8, 0);                       // the buffer)
  a.Lw(t5, t4, 0);                       // M1: opening marker (device load)
  a.Xor(t6, t5, t5);                     // t6 = 0, but a real dependency on t5
  a.Add(t6, t6, s6);                     // t6 = &subject
  a.Ld(t0, t6, 0);                       // L1 miss -> buffer hit (positive control)
  a.Xor(t7, t0, t0);                     // t7 = 0, depends on the control load
  a.Add(t7, t7, t4);                     // t7 = the marker address
  a.Lw(t5, t7, 0);                       // M2: closing marker (device load)
  a.Ld(t0, s7, 0);                       // four same-set installs again: the
  a.Ld(t0, s8, 0);                       // subject leaves the L1 once more, so
  a.Ld(t0, s9, 0);                       // the store below misses and takes a
  a.Ld(t0, t8, 0);                       // read for ownership
  a.Lw(t5, t4, 0);                       // M3: opening marker (device load)
  a.Xor(t6, t5, t5);
  a.Add(t6, t6, s6);                     // t6 = &subject
  a.Sd(x0, t6, 0);                       // store: L1 miss -> read for ownership
  a.Addi(t3, x0, 0x21);
  a.Sw(t3, t4, 0);                       // M4: closing marker (device store)

  // ---- signatures and exit ------------------------------------------------
  a.Addi(a5, s0, -0x400);                // a5 = 0x80000400 (the signature)
  a.Sd(a2, a5, 0);                       // sig[0]: both accumulators
  a.Sd(t2, a5, 8);                       // sig[1]: the load after the AMO
  a.Addi(s4, s0, 0x7ff);
  a.Addi(s4, s4, 1);                     // s4 = 0x80001000 (tohost)
  a.Addi(s5, x0, 1);
  a.Sd(s5, s4, 0);
  // The exit word and the signature live in cacheable RAM, so a write-back data
  // cache holds them until something writes the cache back. This core's only
  // data-cache writeback a program can ask for is its FENCE.I (which flushes the
  // data cache and then invalidates the instruction cache), and the harness
  // observes memory -- without it the run never reports finished.
  a.FenceI();
  const int hang = a.Label();
  a.Bind(hang);
  g_park_pc = kRamBase + uint64_t(a.off());
  a.Jal(x0, hang);

  std::vector<uint8_t> image = a.Bytes();
  // The data. The image covers the whole region the program touches, so no store
  // is needed to set up a value -- and a store would dirty a line the locality
  // story needs clean.
  if (std::getenv("LOCALITY_DUMP") != nullptr) {
    for (size_t i = 0; i < 48 && i * 4 + 4 <= image.size(); i++) {
      const uint32_t w = uint32_t(image[i * 4]) | (uint32_t(image[i * 4 + 1]) << 8) |
                         (uint32_t(image[i * 4 + 2]) << 16) |
                         (uint32_t(image[i * 4 + 3]) << 24);
      std::printf("    [prog] 0x%02zx: %08x\n", i * 4, w);
    }
    std::printf("    [prog] park_pc=0x%llx code_bytes=%zu\n", (unsigned long long)g_park_pc,
                image.size());
  }

  const size_t need = size_t(kPfBase) + size_t(kPfLines) * 0x100;
  image.resize(std::max(image.size(), need), 0);
  auto put = [&image](uint64_t off, uint64_t value) {
    for (int b = 0; b < 8; b++) image[size_t(off) + size_t(b)] = uint8_t((value >> (8 * b)) & 0xff);
  };
  // The offsets are relative to s0 = 0x80000800; the image is relative to the
  // RAM base, so the data lives 0x800 further in -- and beyond the code, which
  // the earlier version of this function did not respect.
  const uint64_t kDataBase = 0x800;
  put(kDataBase + kOffA, 7);
  put(kDataBase + kOffB, 9);
  put(kDataBase + kOffC, 11);
  put(kDataBase + kOffD, 13);
  put(kDataBase + kOffE, 15);
  put(kDataBase + kOffAmo, 100);
  put(kDataBase + kOffCk, 11);
  put(kDataBase + kOffCkF0, 1);
  put(kDataBase + kOffCkF1, 2);
  put(kDataBase + kOffCkF2, 3);
  put(kDataBase + kOffCkF3, 4);
  put(kDataBase + kOffPfBase, 0x801FF000ull);
  for (int i = 0; i < kPfLines; i++) put(kPfBase + uint64_t(i) * 0x100, uint64_t((i + 1) * 3));
  return image;
}

// ============================================================================
// One run of the core.
// ============================================================================
struct Retire {
  uint64_t pc = 0;
  uint32_t rd = 0;
  bool we = false;
  uint64_t value = 0;
  uint32_t len = 0;
};

struct Counters {
  uint32_t llb_hit = 0, llb_miss = 0, llb_bypass = 0, llb_fill = 0, llb_fill_refused = 0;
  uint32_t llb_inv = 0, mem_line = 0;
  uint32_t pf_issued = 0, pf_useful = 0, pf_useless = 0, pf_late = 0, pf_cancelled = 0;
  uint32_t pf_admitted = 0, pf_fill = 0, pf_fill_refused = 0, pf_mem = 0;
};

struct MarkerSample {
  uint32_t value = 0;
  bool is_store = false;
  Counters at;
};

struct RunRec {
  std::vector<Retire> retires;
  uint64_t cycles = 0;
  uint64_t imem_beats = 0;
  uint64_t dmem_beats = 0;
  std::vector<uint64_t> sig;
  std::vector<MarkerSample> markers;
  std::vector<uint64_t> dmem_addrs;   // every data-port request address
  Counters ctr;
  bool finished = false;
  bool passed = false;
};

template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return uint64_t(wide[lane * 2]) | (uint64_t(wide[lane * 2 + 1]) << 32);
}
uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
  const uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
  return (packed >> (lane * width)) & mask;
}

class CoreRun {
 public:
  CoreRun(Vmosaic_core_tb* dut, Reporter* rep) : dut_(dut), rep_(rep) {}

  RunRec Run(bool llb_en, bool pf_en, uint32_t retire_n) {
    MemoryModel mem;
    std::vector<uint8_t> image = BuildProgram();
    mosaic::Image img;
    mosaic::Segment seg;
    seg.vaddr = kRamBase;
    seg.memsz = image.size();
    seg.filesz = image.size();
    seg.flags = 7;
    seg.data = image;
    img.segments.push_back(seg);
    img.entry = kRamBase;
    std::string detail;
    if (!mem.LoadImage(img, &detail)) {
      rep_->Mismatch("setup", "the program loads", detail);
      return RunRec{};
    }

    Bus imem(&mem, 2);
    Bus dmem(&mem, 2);
    imem.Reset();
    dmem.Reset();

    RunRec rec;
    llb_en_ = llb_en;
    pf_en_ = pf_en;
    for (int i = 0; i < kResetCycles; i++) Cycle(&imem, &dmem, true, retire_n);
    uint32_t last_commit = 0;
    int stalled = 0;
    for (int i = 0; i < kMaxRunCycles; i++) {
      Cycle(&imem, &dmem, false, retire_n);
      rec.cycles++;
      // A watchdog that says *where* the machine stopped, not just that it did.
      if (debug_ < 12 && dut_->o_commit_o == last_commit) {
        if (++stalled == 3000) {
          std::printf("    [stall] cycle=%llu commit=%u stopped=%u illegal=%u unsup=%u "
                      "head_pc=0x%llx lq=%u sq=%u lsu_busy=%u\n",
                      (unsigned long long)rec.cycles, dut_->o_commit_o, dut_->o_stopped_o,
                      dut_->o_illegal_o, dut_->o_unsupported_o,
                      (unsigned long long)dut_->o_dbg_head_pc_o, dut_->o_mem_lq_occupied_o,
                      dut_->o_mem_sq_occupied_o, dut_->o_mem_lsu_busy_o);
          debug_++;
          stalled = 0;
        }
      } else {
        stalled = 0;
        last_commit = dut_->o_commit_o;
      }
      if (mem.finished()) {
        for (int d = 0; d < kDrainCycles; d++) Cycle(&imem, &dmem, false, retire_n);
        break;
      }
    }
    rec.finished = mem.finished();
    rec.passed = mem.passed();
    rec.imem_beats = imem.accepted();
    rec.dmem_beats = dmem.accepted();
    for (const auto& t : dmem.txns()) rec.dmem_addrs.push_back(t.req.addr);
    if (std::getenv("LOCALITY_DUMP") != nullptr) {
      for (const auto& t : dmem.txns()) {
        if (t.req.addr == kUartScratch) {
          std::printf("    [uart] cycle=%llu we=%d size=%u wstrb=0x%x wdata=0x%llx\n",
                      (unsigned long long)t.cycle, t.req.we ? 1 : 0, t.req.size,
                      unsigned(t.req.wstrb), (unsigned long long)t.req.wdata);
        }
      }
    }
    mem.ReadSignature(&rec.sig);
    if (std::getenv("LOCALITY_DUMP") != nullptr) {
      const uint64_t addrs[] = {0x80000000ull, 0x80000400ull, 0x80000C40ull,
                                0x80001440ull, 0x80001C40ull, 0x80002440ull,
                                0x80003440ull, 0x801FF000ull};
      std::printf("    [mem] (llb=%d pf=%d) park_pc=0x%llx retires=%zu\n", llb_en ? 1 : 0,
                  pf_en ? 1 : 0, (unsigned long long)g_park_pc, observed_.size());
      for (uint64_t a : addrs) {
        uint64_t v = 0;
        const AccessStatus st = mem.Read(a, 8, &v);
        std::printf("    [mem] 0x%llx = 0x%llx (status=%d)\n", (unsigned long long)a,
                    (unsigned long long)v, int(st));
      }
    }
    rec.markers = markers_;
    rec.ctr = Snapshot();
    return rec;
  }

 private:
  Counters Snapshot() const {
    Counters c;
    c.llb_hit = dut_->o_loc_llb_hit_o;
    c.llb_miss = dut_->o_loc_llb_miss_o;
    c.llb_bypass = dut_->o_loc_llb_bypass_o;
    c.llb_fill = dut_->o_loc_llb_fill_o;
    c.llb_fill_refused = dut_->o_loc_llb_fill_refused_o;
    c.llb_inv = dut_->o_loc_llb_inv_o;
    c.mem_line = dut_->o_loc_mem_line_o;
    c.pf_issued = dut_->o_loc_pf_issued_o;
    c.pf_useful = dut_->o_loc_pf_useful_o;
    c.pf_useless = dut_->o_loc_pf_useless_o;
    c.pf_late = dut_->o_loc_pf_late_o;
    c.pf_cancelled = dut_->o_loc_pf_cancelled_o;
    c.pf_admitted = dut_->o_loc_pf_admitted_o;
    c.pf_fill = dut_->o_loc_pf_fill_o;
    c.pf_fill_refused = dut_->o_loc_pf_fill_refused_o;
    c.pf_mem = dut_->o_loc_pf_mem_o;
    return c;
  }

  void Cycle(Bus* imem, Bus* dmem, bool rst, uint32_t retire_n) {
    dut_->rst = rst ? 1 : 0;
    dut_->fab_dyn_i = 0;
    dut_->cache_en_i = 1;
    dut_->loc_llb_en_i = llb_en_ ? 1 : 0;
    dut_->loc_pf_en_i = pf_en_ ? 1 : 0;
    dut_->loc_pf_conf_thresh_i = 1;
    dut_->loc_snoop_all_i = 0;

    dut_->imem_req_ready_i = 1;
    if (imem->HasRsp()) {
      const Rsp& r = imem->Current();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = r.rdata;
      dut_->imem_rsp_fault_i = r.fault ? 1 : 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = (r.rdata & 3u) == 3u ? 4 : 2;
    } else {
      dut_->imem_rsp_valid_i = 0;
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }

    dut_->dmem_req_ready_i = 1;
    if (dmem->HasRsp()) {
      const Rsp& r = dmem->Current();
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
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = 0;
    dut_->ext_write_valid_i = 0;
    dut_->ext_write_addr_i = 0;
    dut_->ext_write_bytes_i = 0;
    dut_->cb_cpu_req_valid_i = 0;
    dut_->cb_cpu_req_we_i = 0;
    dut_->cb_cpu_req_addr_i = 0;
    dut_->cb_cpu_req_size_i = 0;
    dut_->cb_cpu_req_wstrb_i = 0;
    dut_->cb_cpu_req_wdata_i = 0;
    dut_->cb_cpu_req_amo_i = 0;
    dut_->cb_flush_i = 0;
    dut_->cb_mem_req_ready_i = 0;
    dut_->cb_mem_rsp_valid_i = 0;
    dut_->cb_mem_rsp_rdata_i = 0;
    dut_->cb_mem_rsp_fault_i = 0;
    dut_->cb_dbg_index_i = 0;

    dut_->eval();

    if (!rst && debug_ < 20 && dut_->o_trap_valid_o != 0) {
      std::printf("    [trap] cycle=%llu cause=%llu epc=0x%llx tval=0x%llx\n",
                  (unsigned long long)rec_cycle_, (unsigned long long)dut_->o_trap_cause_o,
                  (unsigned long long)dut_->o_trap_epc_o,
                  (unsigned long long)dut_->o_trap_tval_o);
      debug_++;
    }

    if (!rst) Observe(retire_n);

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      Bus::Req r;
      r.addr = dut_->imem_req_addr_o;
      r.size = dut_->imem_req_size_o;
      r.inst = true;
      imem->Accept(r, rec_cycle_);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem->Pop();
    }
    imem->Advance();

    const bool dmem_go = (dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0);
    if (bus_reset_.MayAccept(rst, dmem_go)) {
      Bus::Req r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = uint8_t(dut_->dmem_req_wstrb_o);
      r.wdata = dut_->dmem_req_wdata_o;
      // A marker: a word store to the UART scratch. The value rides in the lanes
      // the endpoint aligned it to (the byte at `addr` is lane addr[2:0], and
      // addr[2:0] is 4 here), so it is bits 32..63 of the port's data.
      if (r.addr == kUartScratch) {
        MarkerSample s;
        s.value = r.we ? uint32_t((r.wdata >> 32) & 0xffffffffull) : 0;
        s.is_store = r.we;
        s.at = Snapshot();
        markers_.push_back(s);
      }
      dmem->Accept(r, rec_cycle_);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
      dmem->Pop();
    }
    dmem->Advance();

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    rec_cycle_++;
  }

  void Observe(uint32_t retire_n) {
    const uint32_t mask = (retire_n >= 32) ? 0xffffffffu : ((1u << retire_n) - 1u);
    const uint32_t got = uint32_t(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < retire_n && lane < 32; lane++) {
      if ((got & (1u << lane)) == 0) continue;
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = uint32_t(PackedLane(dut_->ev_rd_o, lane, 5));
      r.we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.len = uint32_t(PackedLane(dut_->ev_len_o, lane, 3));
      observed_.push_back(r);
    }
  }

 public:
  const std::vector<Retire>& observed() const { return observed_; }

 private:
  Vmosaic_core_tb* dut_;
  mosaic::BusResetGate bus_reset_;
  Reporter* rep_;
  std::vector<Retire> observed_;
  std::vector<MarkerSample> markers_;
  uint64_t rec_cycle_ = 0;
  bool llb_en_ = false;
  bool pf_en_ = false;
  int debug_ = 0;
};

bool IsDevice(uint64_t addr) {
  return (addr >= 0x00100000ull && addr < 0x00100100ull) ||
         (addr >= 0x00102000ull && addr < 0x00102010ull) ||
         (addr >= 0x000C0000ull && addr < 0x000C0004ull) ||
         (addr >= 0x02000000ull && addr < 0x02001000ull);
}

std::string Hexu(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
  return buf;
}

size_t ParkIndex(const std::vector<Retire>& stream) {
  for (size_t i = 0; i < stream.size(); i++) {
    if (stream[i].pc == g_park_pc) return i;
  }
  return size_t(-1);
}

struct Config {
  const char* name;
  bool llb;
  bool pf;
  RunRec rec;
  std::vector<Retire> stream;
};

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "usage error: %s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);
  Vmosaic_core_tb dut;
  Reporter reporter(options, "verilator");

  dut.clk = 0;
  dut.rst = 1;
  dut.cache_en_i = 0;
  dut.loc_llb_en_i = 0;
  dut.loc_pf_en_i = 0;
  dut.loc_pf_conf_thresh_i = 1;
  dut.loc_snoop_all_i = 0;
  dut.eval();
  const uint32_t retire_n = dut.o_geom_retire_width_o;

  // ------------------------------------------------------------- the four runs
  Config cfgs[4] = {{"baseline", false, false, {}, {}},
                    {"llb", true, false, {}, {}},
                    {"prefetch", false, true, {}, {}},
                    {"both", true, true, {}, {}}};
  for (auto& c : cfgs) {
    CoreRun run(&dut, &reporter);
    c.rec = run.Run(c.llb, c.pf, retire_n);
    c.stream = run.observed();
  }

  std::printf("locality.integrated_path: retire_width=%u park_pc=0x%llx\n", retire_n,
              (unsigned long long)g_park_pc);
  for (const auto& c : cfgs) {
    std::printf("locality.integrated_path: %-8s cycles=%llu imem=%llu dmem=%llu "
                "llb{hit=%u miss=%u bypass=%u fill=%u fill_refused=%u inv=%u} "
                "line{mem=%u} pf{issued=%u useful=%u useless=%u late=%u cancelled=%u "
                "admitted=%u fill=%u fill_refused=%u mem=%u} sig=[%llx %llx]\n",
                c.name, (unsigned long long)c.rec.cycles,
                (unsigned long long)c.rec.imem_beats, (unsigned long long)c.rec.dmem_beats,
                c.rec.ctr.llb_hit, c.rec.ctr.llb_miss, c.rec.ctr.llb_bypass,
                c.rec.ctr.llb_fill, c.rec.ctr.llb_fill_refused, c.rec.ctr.llb_inv,
                c.rec.ctr.mem_line, c.rec.ctr.pf_issued, c.rec.ctr.pf_useful,
                c.rec.ctr.pf_useless, c.rec.ctr.pf_late, c.rec.ctr.pf_cancelled,
                c.rec.ctr.pf_admitted, c.rec.ctr.pf_fill, c.rec.ctr.pf_fill_refused,
                c.rec.ctr.pf_mem,
                (unsigned long long)(c.rec.sig.size() > 0 ? c.rec.sig[0] : 0),
                (unsigned long long)(c.rec.sig.size() > 1 ? c.rec.sig[1] : 0));
  }

  // ------------------------------------------------------ phase 1: identity
  const Config& base = cfgs[0];
  reporter.Check(base.rec.finished, "the baseline run reaches the exit protocol");
  for (const auto& c : cfgs) {
    reporter.Check(c.rec.finished, std::string("the ") + c.name + " run reaches the exit "
                                   "protocol");
    reporter.Check(c.rec.passed, std::string("the ") + c.name + " run reports PASS through "
                                   "tohost");
  }

  const size_t base_park = ParkIndex(base.stream);
  reporter.Check(base_park != size_t(-1), "the baseline run reaches the program's park loop");
  for (size_t ci = 1; ci < 4; ci++) {
    const Config& c = cfgs[ci];
    const size_t park = ParkIndex(c.stream);
    bool equal = (park != size_t(-1)) && (park == base_park);
    size_t at = 0;
    if (equal) {
      for (size_t i = 0; i <= base_park; i++) {
        const Retire& a = base.stream[i];
        const Retire& b = c.stream[i];
        const bool head_ok = a.pc == b.pc && a.len == b.len && a.we == b.we;
        const bool body_ok = !a.we || (a.rd == b.rd && a.value == b.value);
        if (!head_ok || !body_ok) {
          equal = false;
          at = i;
          break;
        }
      }
    } else {
      at = std::min(base.stream.size(), c.stream.size());
    }
    if (!equal) {
      std::string detail = "retirement " + std::to_string(at) + ": baseline ";
      if (at < base.stream.size()) {
        detail += "pc=" + Hexu(base.stream[at].pc) + " rd=" + std::to_string(base.stream[at].rd) +
                  " we=" + std::to_string(base.stream[at].we) +
                  " val=" + Hexu(base.stream[at].value);
      } else {
        detail += "<end>";
      }
      detail += ", " + std::string(c.name) + " ";
      if (at < c.stream.size()) {
        detail += "pc=" + Hexu(c.stream[at].pc) + " rd=" + std::to_string(c.stream[at].rd) +
                  " we=" + std::to_string(c.stream[at].we) +
                  " val=" + Hexu(c.stream[at].value);
      } else {
        detail += "<end>";
      }
      reporter.Mismatch(std::string("locality-") + c.name + " retirement stream",
                        "identical to the baseline", detail);
    }
    reporter.Check(equal, std::string("the ") + c.name + " run retires the same instructions "
                          "through the park as the baseline");
    reporter.Check(c.rec.sig == base.rec.sig,
                   std::string("the ") + c.name + " run leaves the same signature words");
    // Every run stays in the park after reaching it.
    bool parked_clean = true;
    for (size_t i = (park == size_t(-1) ? c.stream.size() : park + 1); i < c.stream.size(); i++) {
      if (c.stream[i].pc != g_park_pc) parked_clean = false;
    }
    reporter.Check(parked_clean, std::string("the ") + c.name + " run stays in the park once "
                                "it reaches it");
  }

  // The deterministic values, so "all four agree" is not the only check.
  auto sigword = [](const RunRec& r, size_t i) -> uint64_t {
    return r.sig.size() > i ? r.sig[i] : 0;
  };
  // phase 1: 8 * (7 + 9 + 11 + 13 + 15) = 440. phase 2: 3*(1+..+16) = 408. Total 848.
  reporter.Check(sigword(base.rec, 0) == 848, "the loop accumulators reach 848");
  // phase 5: the AMO adds 5 to its line (which starts at 100) and the load must
  // see 105.
  reporter.Check(sigword(base.rec, 1) == 105,
                 "the load after the AMO sees memory (105), not a stale clean copy");

  // -------------------------------------------------- phase 2: the structures
  const Config& llb_c = cfgs[1];
  const Config& pf_c = cfgs[2];
  const Config& both_c = cfgs[3];
  reporter.Check(llb_c.rec.ctr.llb_hit > 0,
                 "the LLB is engaged: its hit counter is non-zero with it on");
  reporter.Check(llb_c.rec.ctr.llb_fill > 0, "the LLB is filled");
  reporter.Check(both_c.rec.ctr.llb_hit > 0,
                 "the LLB is engaged when both structures are on");
  reporter.Check(base.rec.ctr.llb_hit == 0 && base.rec.ctr.llb_fill == 0,
                 "the LLB is inert when it is off");
  reporter.Check(pf_c.rec.ctr.pf_issued > 0,
                 "the prefetcher is engaged: it issued prefetches with it on");
  reporter.Check(both_c.rec.ctr.pf_issued > 0,
                 "the prefetcher is engaged when both structures are on");
  reporter.Check(both_c.rec.ctr.pf_useful > 0,
                 "a prefetched line is later demanded: the useful counter is non-zero");
  reporter.Check(base.rec.ctr.pf_issued == 0,
                 "the prefetcher issues nothing when it is off");

  // -------------------------------------------------- phase 3: the class rule
  // The first marker pair brackets one load that must consult the buffer and
  // hit; the second brackets one store whose read-for-ownership must not
  // consult it at all.
  if (std::getenv("LOCALITY_DUMP") != nullptr) {
    for (size_t i = 0; i < llb_c.rec.markers.size(); i++) {
      const MarkerSample& m = llb_c.rec.markers[i];
      std::printf("    [marker] %zu store=%d value=0x%x hit=%u miss=%u fill=%u inv=%u\n", i,
                  m.is_store ? 1 : 0, m.value, m.at.llb_hit, m.at.llb_miss, m.at.llb_fill,
                  m.at.llb_inv);
    }
  }
  const bool markers_ok = llb_c.rec.markers.size() == 4;
  reporter.Check(markers_ok, "the four class-check markers reach the data port");
  if (markers_ok) {
    const MarkerSample& m1 = llb_c.rec.markers[0];
    const MarkerSample& m2 = llb_c.rec.markers[1];
    const MarkerSample& m3 = llb_c.rec.markers[2];
    const MarkerSample& m4 = llb_c.rec.markers[3];
    reporter.Check(!m1.is_store && !m2.is_store && !m3.is_store && m4.is_store &&
                   m4.value == 0x21,
                   "the class-check markers arrive in order, load/load/load/store");
    const uint32_t load_lookups = (m2.at.llb_hit - m1.at.llb_hit) +
                                  (m2.at.llb_miss - m1.at.llb_miss);
    const uint32_t store_lookups = (m4.at.llb_hit - m3.at.llb_hit) +
                                   (m4.at.llb_miss - m3.at.llb_miss);
    if (load_lookups != 1) {
      reporter.Mismatch("the class-check positive control", "one buffer lookup for the load",
                        std::to_string(load_lookups) + " lookups");
    }
    reporter.Check(load_lookups == 1,
                   "the load between the first marker pair consults the buffer");
    if (store_lookups != 0) {
      reporter.Mismatch("the store's read-for-ownership",
                        "the buffer is not consulted",
                        std::to_string(store_lookups) + " lookups ("
                        + std::to_string(m4.at.llb_hit - m3.at.llb_hit) + " hits, "
                        + std::to_string(m4.at.llb_miss - m3.at.llb_miss) + " misses)");
    }
    reporter.Check(store_lookups == 0,
                   "a store's read-for-ownership does not consult the locality buffer");
  }

  // ------------------------------------------- phase 6: no prefetch to a device
  for (const auto& c : cfgs) {
    bool device_req = false;
    uint64_t bad = 0;
    for (uint64_t addr : c.rec.dmem_addrs) {
      if (IsDevice(addr)) {
        // The program's four markers are the only device accesses: they are
        // 4-byte stores to the UART scratch, and they are the program's own, not
        // a prefetch's. Anything else is a locality structure reaching a device.
        if (addr != kUartScratch) {
          device_req = true;
          bad = addr;
        }
      } else if (addr < kRamBase || addr >= kRamBase + 0x200000ull) {
        device_req = true;
        bad = addr;
      }
    }
    if (device_req) {
      reporter.Mismatch(std::string(c.name) + " data-port addresses",
                        "only the program's own accesses", "unexpected " + Hexu(bad));
    }
    reporter.Check(!device_req,
                   std::string(c.name) + ": no locality structure reaches an address outside "
                   "the program's own normal memory");
  }

  // ----------------------------------------------------------- the per-step table
  std::printf("locality.integrated_path: step table (this program, this geometry)\n");
  std::printf("  %-8s %10s %10s %10s %10s %10s %10s\n", "step", "cycles", "dmem", "imem",
              "line_mem", "llb_hit", "pf_useful");
  for (const auto& c : cfgs) {
    std::printf("  %-8s %10llu %10llu %10llu %10llu %10llu %10llu\n", c.name,
                (unsigned long long)c.rec.cycles, (unsigned long long)c.rec.dmem_beats,
                (unsigned long long)c.rec.imem_beats,
                (unsigned long long)c.rec.ctr.mem_line,
                (unsigned long long)c.rec.ctr.llb_hit,
                (unsigned long long)c.rec.ctr.pf_useful);
  }

  const std::string verdict = reporter.failures() == 0 ? "PASS" : "FAIL";
  return reporter.Finish(verdict, "locality integration: on/off identical, structures used, "
                                  "class rule checked, no prefetch reaches a device");
}
