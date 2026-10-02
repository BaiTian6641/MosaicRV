// ============================================================================
// tb_core_cache_path.cpp -- CASE=cache.integrated_path, work package I-042.
//
// The DUT is the integrated core (the same `mosaic_core_tb` every core case
// drives) with the L1 caches wired into the access path: the instruction cache
// between fetch and the instruction port, the data cache between the LSU
// endpoint and the data port, cacheability taken from the platform memory map,
// MMIO bypassing the data cache, and FENCE.I flushing the data cache back and
// invalidating the instruction cache.
//
// ---------------------------------------------------------------- the phases
//
// 1. **On/off equivalence.** One hand-built program runs twice from a fresh
//    reset: once with `cache_en_i` low (the cacheless machine every other case
//    builds) and once with it high. The *architectural* results must be
//    identical: the retirement stream (pc, destination, value, length, in
//    order) and the four signature words the program leaves in memory. The
//    comparison is the case's central claim -- "the caches do not change what
//    the program computes".
//
// 2. **The caches are used.** The memory-side transaction count is measured on
//    the DUT's own ports (instruction and data) for both runs and the case
//    *asserts* the enabled run is cheaper. A cache that is wired in but never
//    hits is the integration's silent failure, and this is what makes it loud.
//
// 3. **FENCE.I makes the new bytes visible.** The program stores a new
//    instruction over its own code, executes FENCE.I, and jumps to it. The
//    patched instruction stores a marker; the marker must reach the signature.
//    (This is the self-modifying-code hazard, with a write-back data cache in
//    front of it: the D-cache writeback-then-I-cache-invalidate order is what
//    makes it correct.)
//
// 4. **A device access is not cached.** The program performs three UART accesses
//    (write, read, write); the case requires them to appear on the data port
//    exactly once, with their own size, and never as a cache line refill.
//
// 5. **Directed control phases on the standalone cache path.** The same wrapper
//    the core instantiates is also exposed directly (u_tb_cache_path), so a
//    refill can be made to fault, a dirty line evicted on demand, and a device
//    address presented -- deterministically. These phases are what the four
//    negative controls are aimed at.
//
// --------------------------------------------------------- what this does not do
//
// The caches are blocking on both sides: no MSHR is wired in, so a miss costs
// the whole cache and there is no coalescing of duplicate misses. See
// results/reports/I-042-cache-path.md, "not covered".
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
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
constexpr int kDrainCycles = 64;

constexpr uint64_t kRamBase = 0x80000000ull;
constexpr uint64_t kToHost = 0x80001000ull;
constexpr uint64_t kSigBase = 0x80000400ull;

// The program's data addresses. The set index of a direct-mapped 32-byte line is
// (addr >> 5) & 7; the loop's instructions live in lines 0 and 1 (0x00..0x3f), so
// the data addresses are chosen in indices 2..6 to avoid a conflict that would
// turn every iteration into a miss.
constexpr uint64_t kData0 = 0x80000840ull;   // index 2
constexpr uint64_t kData1 = 0x80000860ull;   // index 3
constexpr uint64_t kMagic = 0x80000880ull;   // index 4
constexpr uint64_t kIoRec = 0x800008C0ull;   // index 6
constexpr uint64_t kSlot = 0x80000100ull;    // the self-modifying code slot

constexpr uint64_t kUartScratch = 0x0010000Cull;

// The pc of the program's park loop (`jal x0, .`), recorded while the program is
// assembled. Both runs retire it once and then spin there; the retirement stream
// is compared up to and including that instruction, because how many times a
// parked machine is *observed* to go round is a property of when the harness
// stopped sampling, not of the machine.
uint64_t g_park_pc = 0;

// The instruction encodings this program uses (all 32-bit; no compressed
// instruction is emitted, so every PC is 4-aligned).
uint32_t EncR(int f7, int rs2, int rs1, int f3, int rd, int op) {
  return (uint32_t(f7) << 25) | (uint32_t(rs2) << 20) | (uint32_t(rs1) << 15) |
         (uint32_t(f3) << 12) | (uint32_t(rd) << 7) | uint32_t(op);
}
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
uint32_t EncJ(int imm, int rd, int op) {
  return (uint32_t((imm >> 20) & 1) << 31) | (uint32_t((imm >> 1) & 0x3ff) << 21) |
         (uint32_t((imm >> 11) & 1) << 20) | (uint32_t((imm >> 12) & 0xff) << 12) |
         (uint32_t(rd) << 7) | uint32_t(op);
}

constexpr int kOpLui = 0x37, kOpAuipc = 0x17, kOpJal = 0x6f, kOpJalr = 0x67, kOpBr = 0x63;
constexpr int kOpLoad = 0x03, kOpStore = 0x23, kOpImm = 0x13;
constexpr int kF3Sd = 3, kF3Sw = 2, kF3Ld = 3, kF3Lw = 2, kF3Addi = 0, kF3Jalr = 0;
constexpr int kF3Blt = 4;
constexpr uint32_t kFenceI = 0x0000100fu;

// A very small assembler: labels are resolved once at the end.
class Asm {
 public:
  int Label() { return next_label_++; }
  void Bind(int label) { labels_[label] = off(); }
  int off() const { return int(code_.size()) * 4; }

  void Lui(int rd, int imm20) { Emit(EncU(imm20, rd, kOpLui)); }
  void Auipc(int rd, int imm20) { Emit(EncU(imm20, rd, kOpAuipc)); }
  void Addi(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Addi, rd, kOpImm)); }
  void Ld(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Ld, rd, kOpLoad)); }
  void Lw(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Lw, rd, kOpLoad)); }
  void Sd(int rs2, int rs1, int imm) { Emit(EncS(imm, rs2, rs1, kF3Sd, kOpStore)); }
  void Sw(int rs2, int rs1, int imm) { Emit(EncS(imm, rs2, rs1, kF3Sw, kOpStore)); }
  void Jalr(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Jalr, rd, kOpJalr)); }
  void FenceI() { Emit(kFenceI); }

  void Jal(int rd, int label) {
    jfix_.push_back({int(code_.size()), label, rd});
    code_.push_back(0);
  }
  void Blt(int rs1, int rs2, int label) {
    bfix_.push_back({int(code_.size()), label, rs1, rs2});
    code_.push_back(0);
  }

  void PadTo(int byte_off) {
    while (off() < byte_off) code_.push_back(0x00000013u);  // addi x0,x0,0
  }

  std::vector<uint8_t> Bytes() const {
    std::vector<uint32_t> fixed = code_;
    for (const auto& f : bfix_) {
      const int at = f.word;
      const int delta = labels_.at(f.label) - at * 4;
      fixed[size_t(at)] = EncB(delta, f.rs2, f.rs1, kF3Blt, kOpBr);
    }
    for (const auto& f : jfix_) {
      const int at = f.word;
      const int delta = labels_.at(f.label) - at * 4;
      fixed[size_t(at)] = EncJ(delta, f.rd, kOpJal);
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(fixed.size() * 4);
    for (uint32_t w : fixed) {
      for (int b = 0; b < 4; b++) bytes.push_back(uint8_t((w >> (8 * b)) & 0xff));
    }
    return bytes;
  }

 private:
  struct BFix {
    int word;
    int label;
    int rs1;
    int rs2;
  };
  struct JFix {
    int word;
    int label;
    int rd;
  };
  void Emit(uint32_t w) { code_.push_back(w); }
  std::map<int, int> labels_;
  std::vector<BFix> bfix_;
  std::vector<JFix> jfix_;
  std::vector<uint32_t> code_;
  int next_label_ = 0;
};

// ============================================================================
// The bus: one request/response port with a fixed latency and optional read
// fault injection. Reads return the *lane-aligned* window the endpoint's
// convention expects (the byte at `addr` is lane `addr[2:0]`).
// ============================================================================
struct Rsp {
  uint64_t rdata = 0;
  bool fault = false;
  uint32_t id = 0;
  uint32_t epoch = 0;
  uint64_t addr = 0;   // the request this response belongs to (trace only)
};

class Bus {
 public:
  struct Req {
    bool we = false;
    uint64_t addr = 0;
    unsigned size = 0;
    uint64_t wdata = 0;
    uint8_t wstrb = 0;
    uint32_t id = 0;
    uint32_t epoch = 0;
    bool inst = false;   // instruction port: return the word at `addr`, not a window
  };
  struct Txn {
    Req req;
    uint64_t cycle = 0;
  };

  Bus(MemoryModel* mem, int latency) : mem_(mem), latency_(latency < 0 ? 0 : latency) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
    txns_.clear();
  }
  void SetFaultRead(uint64_t base, uint64_t mask) {
    fault_base_ = base;
    fault_mask_ = mask;
    fault_en_ = true;
  }
  void ClearFault() { fault_en_ = false; }

  bool HasRsp() const { return !ready_.empty(); }
  const Rsp& Current() const { return ready_.front(); }
  const std::vector<Txn>& txns() const { return txns_; }
  uint64_t accepted() const { return txns_.size(); }

  void Accept(const Req& r, uint64_t cycle) {
    txns_.push_back({r, cycle});
    Rsp resp = Perform(r);
    resp.id = r.id;
    resp.epoch = r.epoch;
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
  struct Entry {
    Rsp rsp;
    int left = 0;
  };

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
    if (fault_en_ && !r.we && (r.addr & ~fault_mask_) == fault_base_) {
      resp.fault = true;
      return resp;
    }
    if (r.inst) {
      // The instruction port is byte-addressed and returns the bytes at `addr`,
      // so a 4-byte fetch returns the word at the PC and an 8-byte refill the
      // doubleword at the line beat. `size` is the ISA encoding (0/1/2/3 for
      // 1/2/4/8 bytes), not a byte count.
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
      resp.fault = mem_->Write(base, 8, window) != AccessStatus::kOk;
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
  bool fault_en_ = false;
  uint64_t fault_base_ = 0;
  uint64_t fault_mask_ = 0;
};

// ============================================================================
// The program. It is hand-assembled so the case owns every byte and no external
// ELF build stands between the claim and what the machine is fed.
// ============================================================================
std::vector<uint8_t> BuildProgram() {
  Asm a;
  const int x0 = 0, ra = 1, t0 = 5, t1 = 6, t2 = 7, s0 = 8, s1 = 9, a0 = 10, a1 = 11,
            a2 = 12, a3 = 13, a4 = 14, a5 = 15, a6 = 16, s4 = 20, s5 = 21;

  // Addresses are formed PC-relatively (`auipc`) or from small positive
  // offsets: a bare `lui 0x80002` sign-extends bit 31 to 0xFFFFFFFF80002000,
  // which is not the physical address the memory map covers. The 0x800
  // adjustment is split into 0x7ff + 1 because a 12-bit immediate of 0x800 has
  // bit 11 set, and is therefore *negative* (-2048): `addi s0, s0, 0x800` is
  // `s0 - 0x800`, not `s0 + 0x800`.
  // ---- phase 1: the cache-use loop -------------------------------------
  a.Auipc(s0, 0);              // s0 = 0x80000000
  a.Addi(s0, s0, 0x7ff);       // s0 = 0x800007ff
  a.Addi(s0, s0, 1);           // s0 = 0x80000800 (data base)
  a.Addi(s1, x0, 0);           // s1 = 0 (iteration counter)
  a.Addi(a1, x0, 256);         // a1 = 256 (iterations)
  const int loop = a.Label();
  a.Bind(loop);
  a.Ld(t0, s0, 0x40);          // t0 = DATA0
  a.Addi(t0, t0, 1);
  a.Sd(t0, s0, 0x40);          // DATA0 = t0
  a.Ld(t1, s0, 0x60);          // t1 = DATA1
  a.Addi(t1, t1, 3);
  a.Sd(t1, s0, 0x60);          // DATA1 = t1
  a.Addi(s1, s1, 1);
  a.Blt(s1, a1, loop);         // loop while s1 < 256

  // ---- phase 2: self-modifying code + FENCE.I --------------------------
  a.Addi(t1, s0, -0x700);      // t1 = 0x80000100 (the slot)
  a.Jalr(ra, t1, 0);           // first call: caches the slot line (nop)
  a.Lui(t2, 0x05A00);          // t2 = 0x05A00000
  a.Addi(t2, t2, 0x513);       // t2 = 0x05A00513  (addi a0,x0,0x5a)
  // A *word* store: the patched instruction is four bytes and the `jalr` that
  // follows it in the slot is the next word. An eight-byte store here would
  // overwrite that `jalr` with zeros and the slot would return nowhere.
  a.Sw(t2, t1, 0);             // patch the slot
  a.FenceI();                  // make the store visible to fetch
  a.Jalr(ra, t1, 0);           // second call: must execute the new bytes
  a.Sd(a0, s0, 0x80);          // MAGIC = a0 (0x5a iff the patch was seen)

  // ---- phase 3: a device access (must not be cached) -------------------
  a.Lui(a2, 0x00100);          // a2 = 0x00100000
  a.Addi(a2, a2, 0x00C);       // a2 = 0x0010000C (UART scratch)
  a.Addi(a3, x0, 42);
  a.Sw(a3, a2, 0);             // device write #1
  a.Lw(a4, a2, 0);             // device read
  a.Sd(a4, s0, 0xC0);          // IO_REC = the read-back
  a.Sw(a3, a2, 0);             // device write #2

  // ---- phase 4: signatures and exit ------------------------------------
  a.Addi(a5, s0, -0x400);      // a5 = 0x80000400 (signature base)
  a.Ld(a6, s0, 0x40);
  a.Sd(a6, a5, 0);
  a.Ld(a6, s0, 0x60);
  a.Sd(a6, a5, 8);
  a.Ld(a6, s0, 0x80);
  a.Sd(a6, a5, 16);
  a.Ld(a6, s0, 0xC0);
  a.Sd(a6, a5, 24);
  a.Addi(s4, s0, 0x7ff);       // s4 = 0x80000fff
  a.Addi(s4, s4, 1);           // s4 = 0x80001000 (tohost)
  a.Addi(s5, x0, 1);
  a.Sd(s5, s4, 0);             // tohost = PASS
  // The exit word lives in cacheable RAM, so a write-back data cache holds it
  // until something writes the cache back. This core's only data-cache writeback
  // that a program can ask for is its FENCE.I (which flushes the data cache and
  // then invalidates the instruction cache); with the caches disabled it is an
  // ordinary retired fence. Without it the harness -- which observes memory --
  // would never see the PASS word while the caches are on.
  a.FenceI();
  const int hang = a.Label();
  a.Bind(hang);
  g_park_pc = kRamBase + uint64_t(a.off());
  a.Jal(x0, hang);             // park

  // The self-modifying slot sits at a fixed address so the program can store to
  // it and jump to it. It is placed after the park loop, which the flow never
  // reaches except by the two JALRs.
  a.PadTo(int(kSlot - kRamBase));
  a.Addi(a0, x0, 0);           // placeholder: a0 = 0
  a.Jalr(x0, ra, 0);           // return to the caller
  // The front end fetches ahead of the slot's `jalr` and delivers what it finds
  // before that jump's redirect lands. The image must therefore hold *valid*
  // encodings past the slot: unfilled memory reads as zero, and a zero word is a
  // compressed illegal instruction, which stops the core (`disp_unsupported`)
  // even though the flow never falls through to it.
  a.PadTo(int(kSlot - kRamBase) + 0x40);
  std::vector<uint8_t> bytes = a.Bytes();
  if (std::getenv("CACHE_PATH_DUMP") != nullptr) {
    for (size_t i = 0; i < bytes.size() / 4 && i < 40; i++) {
      uint32_t w = uint32_t(bytes[i * 4]) | (uint32_t(bytes[i * 4 + 1]) << 8) |
                   (uint32_t(bytes[i * 4 + 2]) << 16) | (uint32_t(bytes[i * 4 + 3]) << 24);
      std::printf("    [prog] 0x%02zx: %08x\n", i * 4, w);
    }
  }
  return bytes;
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
  uint32_t seq = 0;
};

struct RunRec {
  std::vector<Retire> retires;
  uint64_t cycles = 0;
  uint64_t imem_beats = 0;
  uint64_t dmem_beats = 0;
  uint64_t dev_reqs = 0;
  uint64_t dev_dbl_reqs = 0;
  std::vector<uint64_t> sig;
  std::vector<uint64_t> words;      // DATA0, DATA1, MAGIC, IO_REC
  bool finished = false;
  bool passed = false;
  uint64_t exit_code = 0;
};

template <typename Wide>
uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
  return uint64_t(wide[lane * 2]) | (uint64_t(wide[lane * 2 + 1]) << 32);
}
inline uint64_t PayloadLane(uint64_t value, uint32_t) { return value; }
uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
  const uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
  return (packed >> (lane * width)) & mask;
}

constexpr int kRetN = 2;   // p1 retire width; taken from the DUT at runtime below

class CoreRun {
 public:
  CoreRun(Vmosaic_core_tb* dut, Reporter* rep) : dut_(dut), rep_(rep) {
    const char* t = std::getenv("CACHE_PATH_TRACE");
    if (t != nullptr) trace_ = std::atoi(t);
    const char* tn = std::getenv("CACHE_PATH_TRACE_N");
    if (tn != nullptr) trace_n_ = std::atoi(tn);
  }

  RunRec Run(bool cache_en, uint32_t retire_n) {
    MemoryModel mem;
    std::vector<uint8_t> image = BuildProgram();
    mosaic::Image img;
    mosaic::Segment seg;
    seg.vaddr = kRamBase;
    seg.memsz = 0x2000;
    seg.filesz = image.size();
    seg.flags = 7;
    seg.data = image;
    img.segments.push_back(seg);
    img.entry = kRamBase;
    std::string detail;
    if (!mem.LoadImage(img, &detail)) {
      rep_->Mismatch("setup", "the program loads", detail);
      RunRec bad;
      return bad;
    }

    Bus imem(&mem, 2);
    Bus dmem(&mem, 2);
    imem.Reset();
    dmem.Reset();

    // Reset the DUT.
    for (int i = 0; i < kResetCycles; i++) Cycle(&mem, &imem, &dmem, cache_en, true, retire_n);
    RunRec rec;
    uint32_t last_commit = 0;
    int stalled = 0;
    for (int i = 0; i < kMaxRunCycles; i++) {
      Cycle(&mem, &imem, &dmem, cache_en, false, retire_n);
      rec.cycles++;
      if (debug_ < 20 && dut_->o_commit_o == last_commit) {
        if (++stalled == 400) {
          std::printf("    [stall] cycle=%llu commit=%u stopped=%u illegal=%u unsup=%u "
                      "head_pc=0x%llx deliver=%u deliver_pc=0x%llx deliver_bits=0x%x "
                      "trap=%u trap_cause=%llu trap_epc=0x%llx redir_pc=0x%llx redir=%u "
                      "lq=%u sq=%u lsu_busy=%u\n",
                      (unsigned long long)rec.cycles, dut_->o_commit_o, dut_->o_stopped_o,
                      dut_->o_illegal_o, dut_->o_unsupported_o,
                      (unsigned long long)dut_->o_dbg_head_pc_o, dut_->o_dbg_deliver_valid_o,
                      (unsigned long long)dut_->o_dbg_deliver_pc_o, dut_->o_dbg_deliver_bits_o,
                      dut_->o_trap_valid_o, (unsigned long long)dut_->o_trap_cause_o,
                      (unsigned long long)dut_->o_trap_epc_o,
                      (unsigned long long)dut_->o_redirect_pc_o, dut_->o_redirect_o,
                      dut_->o_mem_lq_occupied_o, dut_->o_mem_sq_occupied_o,
                      dut_->o_mem_lsu_busy_o);
          debug_++;
          stalled = 0;
        }
      } else {
        stalled = 0;
        last_commit = dut_->o_commit_o;
      }
      if (mem.finished()) {
        for (int d = 0; d < kDrainCycles; d++) Cycle(&mem, &imem, &dmem, cache_en, false, retire_n);
        break;
      }
    }
    rec.finished = mem.finished();
    rec.passed = mem.passed();
    rec.exit_code = mem.exit_code();
    rec.imem_beats = imem.accepted();
    rec.dmem_beats = dmem.accepted();
    for (const auto& t : dmem.txns()) {
      if (IsDevice(t.req.addr)) {
        rec.dev_reqs++;
        if (t.req.size == 3) rec.dev_dbl_reqs++;
      }
    }
    mem.ReadSignature(&rec.sig);
    rec.words = {ReadWord(&mem, kData0), ReadWord(&mem, kData1), ReadWord(&mem, kMagic),
                 ReadWord(&mem, kIoRec)};
    return rec;
  }

 private:
  static bool IsDevice(uint64_t addr) {
    return (addr >= 0x00100000ull && addr < 0x00100100ull) ||   // uart
           (addr >= 0x00102000ull && addr < 0x00102010ull) ||   // test_harness
           (addr >= 0x000C0000ull && addr < 0x000C0004ull) ||   // clint msip
           (addr >= 0x02000000ull && addr < 0x02001000ull);     // clint
  }
  static uint64_t ReadWord(MemoryModel* mem, uint64_t addr) {
    uint64_t v = 0;
    if (mem->Read(addr, 8, &v) != AccessStatus::kOk) return 0;
    return v;
  }

  void Cycle(MemoryModel* mem, Bus* imem, Bus* dmem, bool cache_en, bool rst,
             uint32_t retire_n) {
    dut_->rst = rst ? 1 : 0;
    dut_->fab_dyn_i = 0;
    dut_->cache_en_i = cache_en ? 1 : 0;

    dut_->imem_req_ready_i = 1;
    if (imem->HasRsp()) {
      const Rsp& r = imem->Current();
      dut_->imem_rsp_valid_i = 1;
      dut_->imem_rsp_rdata_i = r.rdata;
      dut_->imem_rsp_fault_i = r.fault ? 1 : 0;
      dut_->imem_rsp_id_i = r.id;
      dut_->imem_rsp_epoch_i = r.epoch;
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
    // The standalone cache path is idle during the program runs.
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
      std::printf("    [trap] cycle=%llu cause=%llu epc=0x%llx tval=0x%llx target=0x%llx\n",
                  (unsigned long long)rec_cycle_, (unsigned long long)dut_->o_trap_cause_o,
                  (unsigned long long)dut_->o_trap_epc_o,
                  (unsigned long long)dut_->o_trap_tval_o,
                  (unsigned long long)dut_->o_trap_target_o);
      debug_++;
    }

    if (!rst) Observe(retire_n);

    // The memory bus is only driven outside reset. While `rst` is high the fetch
    // unit still presents a request for the reset vector (its PC register is
    // held there and its slot has not been recorded busy), and accepting those
    // requests queues responses that the core discards at reset -- leaving them
    // in front of the post-reset responses with the *same* request id and epoch,
    // so the fetch unit matches a stale response to a fresh request and the
    // instruction stream shifts by one instruction. Only a redirect changes the
    // epoch, so a program that does not branch early would see it. Nothing is
    // accepted, presented or counted during reset for that reason.
    if (!rst && (dut_->imem_req_valid_o != 0) && (dut_->imem_req_ready_i != 0)) {
      Bus::Req r;
      r.we = false;
      r.addr = dut_->imem_req_addr_o;
      r.size = dut_->imem_req_size_o;
      r.id = dut_->imem_req_id_o;
      r.epoch = dut_->imem_req_epoch_o;
      r.inst = true;
      if (trace_ > 0 && rec_cycle_ >= uint64_t(trace_) &&
        rec_cycle_ < uint64_t(trace_) + uint64_t(trace_n_)) {
        std::printf("    [imem-req] cyc=%llu addr=%llx size=%u id=%u epoch=%u\n",
                    (unsigned long long)rec_cycle_, (unsigned long long)r.addr,
                    unsigned(r.size), unsigned(r.id), unsigned(r.epoch));
      }
      imem->Accept(r, rec_cycle_);
    }
    if (!rst && (dut_->imem_rsp_valid_i != 0) && (dut_->imem_rsp_ready_o != 0)) {
      if (trace_ > 0 && rec_cycle_ >= uint64_t(trace_) &&
        rec_cycle_ < uint64_t(trace_) + uint64_t(trace_n_)) {
        std::printf("    [imem-pop] cyc=%llu for=%llx data=%08llx fault=%u ready=%u\n",
                    (unsigned long long)rec_cycle_,
                    (unsigned long long)imem->Current().addr,
                    (unsigned long long)(dut_->imem_rsp_rdata_i & 0xffffffffull),
                    unsigned(dut_->imem_rsp_fault_i),
                    unsigned(dut_->imem_rsp_ready_o));
      }
      imem->Pop();
    }
    imem->Advance();

    if (!rst && (dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0)) {
      Bus::Req r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = uint8_t(dut_->dmem_req_wstrb_o);
      r.wdata = dut_->dmem_req_wdata_o;
      if (std::getenv("CACHE_PATH_DUMP") != nullptr && dbg_mem_ < 20) {
        std::printf("    [dmem] cycle=%llu we=%d addr=0x%llx size=%u\n",
                    (unsigned long long)rec_cycle_, r.we ? 1 : 0,
                    (unsigned long long)r.addr, r.size);
        dbg_mem_++;
      }
      dmem->Accept(r, rec_cycle_);
    }
    if (!rst && (dut_->dmem_rsp_valid_i != 0) && (dut_->dmem_rsp_ready_o != 0)) dmem->Pop();
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
    if (trace_ > 0 && rec_cycle_ >= uint64_t(trace_) &&
        rec_cycle_ < uint64_t(trace_) + uint64_t(trace_n_)) {
      std::printf("    [trace] cyc=%llu valid=%x rd=%x pc=[%llx %llx] val=[%llx %llx] "
                  "seq=[%llx %llx] len=[%llx %llx] we=%x commit=%u headv=%u headpc=%llx "
                  "drd0=%u drd1=%u trap=%u cause=%llu epc=%llx tval=%llx redir=%u rpc=%llx\n",
                  (unsigned long long)rec_cycle_, got,
                  unsigned(dut_->ev_rd_o),
                  (unsigned long long)PayloadLane(dut_->ev_pc_o, 0),
                  (unsigned long long)PayloadLane(dut_->ev_pc_o, 1),
                  (unsigned long long)PayloadLane(dut_->ev_value_o, 0),
                  (unsigned long long)PayloadLane(dut_->ev_value_o, 1),
                  (unsigned long long)uint64_t(dut_->ev_seq_o), 0ull,
                  (unsigned long long)uint64_t(dut_->ev_len_o), 0ull,
                  unsigned(dut_->ev_reg_we_o),
                  unsigned(dut_->o_commit_o), unsigned(dut_->o_dbg_head_valid_o),
                  (unsigned long long)dut_->o_dbg_head_pc_o,
                  unsigned(dut_->o_dbg_desc_rd0_o), unsigned(dut_->o_dbg_desc_rd1_o),
                  unsigned(dut_->o_trap_valid_o),
                  (unsigned long long)dut_->o_trap_cause_o,
                  (unsigned long long)dut_->o_trap_epc_o,
                  (unsigned long long)dut_->o_trap_tval_o,
                  unsigned(dut_->o_redirect_o),
                  (unsigned long long)dut_->o_redirect_pc_o);
    }
    if (debug_ < 6 && got != 0) {
      std::printf("    [dbg] valid=%x rd=0x%llx pc0=0x%llx pc1=0x%llx v0=0x%llx v1=0x%llx\n",
                  got, (unsigned long long)dut_->ev_rd_o,
                  (unsigned long long)PayloadLane(dut_->ev_pc_o, 0),
                  (unsigned long long)PayloadLane(dut_->ev_pc_o, 1),
                  (unsigned long long)PayloadLane(dut_->ev_value_o, 0),
                  (unsigned long long)PayloadLane(dut_->ev_value_o, 1));
      debug_++;
    }
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
  Reporter* rep_;
  std::vector<Retire> observed_;
  uint64_t rec_cycle_ = 0;
  int debug_ = 0;
  int dbg_mem_ = 0;
  int trace_ = 0;
  int trace_n_ = 120;
};

// ============================================================================
// Directed phases on the standalone cache path
// ============================================================================
class Directed {
 public:
  Directed(Vmosaic_core_tb* dut, MemoryModel* mem) : dut_(dut), bus_(mem, 2) {}

  void Reset() {
    bus_.Reset();
    dut_->cb_cpu_req_valid_i = 0;
    dut_->cb_cpu_req_we_i = 0;
    dut_->cb_cpu_req_addr_i = 0;
    dut_->cb_cpu_req_size_i = 0;
    dut_->cb_cpu_req_wstrb_i = 0;
    dut_->cb_cpu_req_wdata_i = 0;
    dut_->cb_cpu_req_amo_i = 0;
    dut_->cb_flush_i = 0;
    dut_->cb_dbg_index_i = 0;
    for (int i = 0; i < kResetCycles; i++) Tick(/*rst=*/true, /*cache_en=*/true);
    for (int i = 0; i < 2; i++) Tick(false, true);
  }

  // One directed request through the standalone path. Returns the response.
  Rsp Transact(bool we, uint64_t addr, unsigned size, uint64_t wdata, uint8_t wstrb,
               unsigned dbg_index, bool* timed_out, uint64_t* beats_before,
               uint64_t* beats_after) {
    if (beats_before) *beats_before = bus_.accepted();
    Rsp out;
    dut_->cb_cpu_req_valid_i = 1;
    dut_->cb_cpu_req_we_i = we ? 1 : 0;
    dut_->cb_cpu_req_addr_i = addr;
    dut_->cb_cpu_req_size_i = size;
    dut_->cb_cpu_req_wstrb_i = wstrb;
    dut_->cb_cpu_req_wdata_i = wdata;
    dut_->cb_cpu_req_amo_i = 0;
    dut_->cb_dbg_index_i = dbg_index;
    for (int i = 0; i < 2000; i++) {
      Tick(false, true);
      if (dut_->cb_cpu_rsp_valid_o != 0) {
        out.rdata = dut_->cb_cpu_rsp_rdata_o;
        out.fault = dut_->cb_cpu_rsp_fault_o != 0;
        dut_->cb_cpu_req_valid_i = 0;
        if (beats_after) *beats_after = bus_.accepted();
        return out;
      }
    }
    if (timed_out) *timed_out = true;
    if (beats_after) *beats_after = bus_.accepted();
    return out;
  }

  void Flush() {
    dut_->cb_flush_i = 1;
    for (int i = 0; i < 2000 && dut_->cb_flush_done_o == 0; i++) Tick(false, true);
    dut_->cb_flush_i = 0;
    Tick(false, true);
  }

  uint64_t bus_beats() const { return bus_.accepted(); }
  const Rsp& Current() const { return bus_.Current(); }
  bool HasRsp() const { return bus_.HasRsp(); }
  const std::vector<Bus::Txn>& txns() const { return bus_.txns(); }
  void SetFault(uint64_t base, uint64_t mask) { bus_.SetFaultRead(base, mask); }
  void ClearFault() { bus_.ClearFault(); }

 private:
  void Tick(bool rst, bool cache_en) {
    dut_->rst = rst ? 1 : 0;
    dut_->cache_en_i = cache_en ? 1 : 0;
    // The core's own memory ports are idle (the core is in reset).
    dut_->imem_req_ready_i = 1;
    dut_->imem_rsp_valid_i = 0;
    dut_->imem_rsp_rdata_i = 0;
    dut_->imem_rsp_fault_i = 0;
    dut_->imem_rsp_id_i = 0;
    dut_->imem_rsp_epoch_i = 0;
    dut_->imem_rsp_len_i = 0;
    dut_->dmem_req_ready_i = 1;
    dut_->dmem_rsp_valid_i = 0;
    dut_->dmem_rsp_rdata_i = 0;
    dut_->dmem_rsp_fault_i = 0;
    dut_->fab_dyn_i = 0;
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = 0;
    dut_->ext_write_valid_i = 0;
    dut_->ext_write_addr_i = 0;
    dut_->ext_write_bytes_i = 0;
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;

    dut_->cb_mem_req_ready_i = 1;
    if (bus_.HasRsp()) {
      dut_->cb_mem_rsp_valid_i = 1;
      dut_->cb_mem_rsp_rdata_i = bus_.Current().rdata;
      dut_->cb_mem_rsp_fault_i = bus_.Current().fault ? 1 : 0;
    } else {
      dut_->cb_mem_rsp_valid_i = 0;
      dut_->cb_mem_rsp_rdata_i = 0;
      dut_->cb_mem_rsp_fault_i = 0;
    }

    dut_->eval();

    if ((dut_->cb_mem_req_valid_o != 0) && (dut_->cb_mem_req_ready_i != 0)) {
      Bus::Req r;
      r.we = dut_->cb_mem_req_we_o != 0;
      r.addr = dut_->cb_mem_req_addr_o;
      r.size = dut_->cb_mem_req_size_o;
      r.wstrb = uint8_t(dut_->cb_mem_req_wstrb_o);
      r.wdata = dut_->cb_mem_req_wdata_o;
      bus_.Accept(r, cycle_);
    }
    if ((dut_->cb_mem_rsp_valid_i != 0) && (dut_->cb_mem_rsp_ready_o != 0)) bus_.Pop();
    bus_.Advance();

    dut_->clk = 0;
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    cycle_++;
  }

  Vmosaic_core_tb* dut_;
  Bus bus_;
  uint64_t cycle_ = 0;
};

std::string Hexu(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
  return buf;
}

std::vector<std::string> Split(const std::string& text, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text) {
    if (c == sep) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

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
  dut.eval();
  const uint32_t retire_n = dut.o_geom_retire_width_o;

  // ---------------------------------------------------------------- phase 1+2
  // Debug hook (CACHE_PATH_RUNS): select which runs execute and in what order,
  // so an anomaly can be attributed to `cache_en_i` or to run order.
  CoreRun off_run(&dut, &reporter);
  CoreRun on_run(&dut, &reporter);
  const char* runs_sel = std::getenv("CACHE_PATH_RUNS");
  const std::string sel = (runs_sel != nullptr) ? runs_sel : "off,on";

  RunRec off;
  RunRec on;
  std::vector<Retire> off_stream;
  std::vector<Retire> on_stream;
  for (const std::string& tok : Split(sel, ',')) {
    if (tok == "off") {
      off = off_run.Run(false, retire_n);
      off_stream = off_run.observed();
    } else if (tok == "on") {
      on = on_run.Run(true, retire_n);
      on_stream = on_run.observed();
    }
  }
  const bool full_compare = off_stream.size() != 0 && on_stream.size() != 0;

  {
    std::printf("cache.integrated_path: retire_width=%u off{cycles=%llu retires=%zu "
                "imem=%llu dmem=%llu finished=%d words=[%llx %llx %llx %llx]}\n",
                retire_n, (unsigned long long)off.cycles, off_stream.size(),
                (unsigned long long)off.imem_beats, (unsigned long long)off.dmem_beats,
                off.finished ? 1 : 0,
                (unsigned long long)(off.words.size() > 0 ? off.words[0] : 0),
                (unsigned long long)(off.words.size() > 1 ? off.words[1] : 0),
                (unsigned long long)(off.words.size() > 2 ? off.words[2] : 0),
                (unsigned long long)(off.words.size() > 3 ? off.words[3] : 0));
    for (size_t i = 0; i < off_stream.size() && i < 8; i++) {
      std::printf("  off retire[%zu] pc=%llx rd=%u we=%d val=%llx len=%u\n", i,
                  (unsigned long long)off_stream[i].pc, off_stream[i].rd,
                  off_stream[i].we ? 1 : 0, (unsigned long long)off_stream[i].value,
                  off_stream[i].len);
    }
    for (size_t i = 0; i < on_stream.size() && i < 8; i++) {
      std::printf("  on retire[%zu] pc=%llx rd=%u we=%d val=%llx len=%u\n", i,
                  (unsigned long long)on_stream[i].pc, on_stream[i].rd,
                  on_stream[i].we ? 1 : 0, (unsigned long long)on_stream[i].value,
                  on_stream[i].len);
    }
    for (size_t k = 0; k < 6; k++) {
      if (off_stream.size() > k) {
        const size_t i = off_stream.size() - 1 - k;
        std::printf("  off tail[%zu] pc=%llx rd=%u we=%d val=%llx len=%u\n", i,
                    (unsigned long long)off_stream[i].pc, off_stream[i].rd,
                    off_stream[i].we ? 1 : 0, (unsigned long long)off_stream[i].value,
                    off_stream[i].len);
      }
      if (on_stream.size() > k) {
        const size_t i = on_stream.size() - 1 - k;
        std::printf("  on tail[%zu] pc=%llx rd=%u we=%d val=%llx len=%u\n", i,
                    (unsigned long long)on_stream[i].pc, on_stream[i].rd,
                    on_stream[i].we ? 1 : 0, (unsigned long long)on_stream[i].value,
                    on_stream[i].len);
      }
    }
  }
  if (!full_compare) return 0;

  reporter.Check(off.finished, "the cache-off run reaches the exit protocol");
  reporter.Check(on.finished, "the cache-on run reaches the exit protocol");
  reporter.Check(off.passed && on.passed, "both runs report PASS through tohost");

  // The architectural result: the retirement stream, compared up to and
  // including the program's own park loop. Both runs retire the park and then
  // spin in it; how many times the park is *sampled* before the harness stops is
  // a property of the cycle in which the exit word became visible, not of the
  // machine. Everything after the park entry must still be the park itself in
  // both runs -- a machine that ran off past it would be caught by that -- and
  // the park must be reached at the same retirement index in both.
  const size_t off_park = ParkIndex(off_stream);
  const size_t on_park = ParkIndex(on_stream);
  bool stream_equal = off_park != kNoPark && off_park == on_park;
  size_t mismatch_at = 0;
  if (stream_equal) {
    for (size_t i = 0; i <= off_park; i++) {
      const Retire& a = off_stream[i];
      const Retire& b = on_stream[i];
      if (a.pc != b.pc || a.rd != b.rd || a.we != b.we || a.value != b.value ||
          a.len != b.len) {
        stream_equal = false;
        mismatch_at = i;
        break;
      }
    }
  } else {
    mismatch_at = std::min(off_stream.size(), on_stream.size());
  }
  if (!stream_equal) {
    const size_t i = mismatch_at;
    std::string detail = "retirement " + std::to_string(i) + ": off ";
    if (i < off_stream.size()) {
      detail += "pc=" + Hexu(off_stream[i].pc) + " rd=" + std::to_string(off_stream[i].rd) +
                " we=" + std::to_string(off_stream[i].we) +
                " val=" + Hexu(off_stream[i].value);
    } else {
      detail += "<end>";
    }
    detail += ", on ";
    if (i < on_stream.size()) {
      detail += "pc=" + Hexu(on_stream[i].pc) + " rd=" + std::to_string(on_stream[i].rd) +
                " we=" + std::to_string(on_stream[i].we) +
                " val=" + Hexu(on_stream[i].value);
    } else {
      detail += "<end>";
    }
    reporter.Mismatch("cache-on retirement stream", "identical to cache-off", detail);
  }
  reporter.Check(off_park == on_park && off_park != kNoPark,
                 "both runs retire the program's park loop at the same index");
  reporter.Check(stream_equal,
                 "cache-on and cache-off retire the same instructions through the park");

  // ... and then both stay in it: the park is `jal x0, .`, so every later
  // retirement must be that same pc. This is what says the streams do not differ
  // *after* the park for any reason other than how long the harness watched.
  bool parked_clean = true;
  for (size_t i = (off_park == kNoPark ? off_stream.size() : off_park + 1);
       i < off_stream.size(); i++) {
    if (off_stream[i].pc != g_park_pc) parked_clean = false;
  }
  for (size_t i = (on_park == kNoPark ? on_stream.size() : on_park + 1);
       i < on_stream.size(); i++) {
    if (on_stream[i].pc != g_park_pc) parked_clean = false;
  }
  reporter.Check(parked_clean,
                 "both runs stay in the program's park loop once they reach it");

  // The architectural result: the signatures the program wrote.
  bool sig_equal = on.sig == off.sig && on.sig.size() == 4;
  reporter.Check(sig_equal, "cache-on and cache-off leave the same four signature words");
  reporter.Check(on.words == off.words, "cache-on and cache-off leave the same data words");

  // Deterministic values the program computes (so "both runs agree" is not the
  // only thing checked: they must also agree with the arithmetic).
  reporter.Check(off.words.size() == 4 && off.words[0] == 256,
                 "the loop counts 256 increments into DATA0 (off)");
  reporter.Check(on.words.size() == 4 && on.words[0] == 256,
                 "the loop counts 256 increments into DATA0 (on)");
  reporter.Check(off.words.size() == 4 && off.words[1] == 768,
                 "the second accumulator reaches 768 (off)");
  reporter.Check(on.words.size() == 4 && on.words[1] == 768,
                 "the second accumulator reaches 768 (on)");

  // FENCE.I visibility: the patched instruction (addi a0,x0,0x5a) must have
  // executed, in both runs.
  reporter.Check(off.sig.size() == 4 && off.sig[2] == 0x5a,
                 "SMC: the patched instruction executed (cache-off)");
  reporter.Check(on.sig.size() == 4 && on.sig[2] == 0x5a,
                 "SMC: FENCE.I made the patched instruction visible (cache-on)");

  // MMIO is never cached: every device access appears exactly once, with its own
  // size, and never as a 32-byte line refill.
  reporter.Check(off.dev_reqs == 3, "the three UART accesses reach memory once (off)");
  reporter.Check(on.dev_reqs == 3,
                 "the three UART accesses reach memory once with the cache on (on)");
  reporter.Check(on.dev_dbl_reqs == 0,
                 "no UART access is presented as a cache-line refill (on)");
  reporter.Check(on.sig.size() == 4 && on.sig[3] == off.sig[3],
                 "the device read-back is identical with the cache on");

  // The caches are used: the enabled run costs strictly fewer memory-side beats.
  const uint64_t off_beats = off.imem_beats + off.dmem_beats;
  const uint64_t on_beats = on.imem_beats + on.dmem_beats;
  const bool fell = on_beats < off_beats;
  if (!fell) {
    reporter.Mismatch("the caches are used", "cache-on beats < cache-off beats",
                      "off=" + std::to_string(off_beats) + " on=" +
                          std::to_string(on_beats));
  }
  reporter.Check(fell, "the cache-on run issues fewer memory transactions than cache-off");
  reporter.Check(on_beats * 2 <= off_beats,
                 "the memory transaction count at least halves with the caches on");
  std::printf("cache.integrated_path: beats off=%llu (i=%llu d=%llu) on=%llu (i=%llu d=%llu) "
              "ratios on/off=%.3f\n",
              (unsigned long long)off_beats, (unsigned long long)off.imem_beats,
              (unsigned long long)off.dmem_beats, (unsigned long long)on_beats,
              (unsigned long long)on.imem_beats, (unsigned long long)on.dmem_beats,
              off_beats ? double(on_beats) / double(off_beats) : 0.0);

  const uint64_t off_ic = off.imem_beats;
  const uint64_t on_ic = on.imem_beats;
  const uint64_t off_dc = off.dmem_beats;
  const uint64_t on_dc = on.dmem_beats;
  reporter.Check(on_ic < off_ic, "the instruction cache reduces instruction-port beats");
  reporter.Check(on_dc < off_dc, "the data cache reduces data-port beats");

  // ---------------------------------------------------------------- phase 5
  // Directed control phases. A fresh MemoryModel: these write to RAM.
  MemoryModel dmem;
  Directed dir(&dut, &dmem);

  // 5a. A device access is not cached.
  dir.Reset();
  {
    uint64_t beats = 0, beats2 = 0;
    bool timed_out = false;
    const uint64_t before = dir.bus_beats();
    Rsp r = dir.Transact(false, kUartScratch, 2, 0, 0, 0, &timed_out, &beats, &beats2);
    reporter.Check(!timed_out, "the directed device access completes");
    reporter.Check(dir.bus_beats() == before + 1,
                   "the device access costs exactly one memory request (bypass)");
    bool saw_dbl_to_device = false, saw_exact = false;
    for (const auto& t : dir.txns()) {
      if (t.req.addr >= 0x00100000ull && t.req.addr < 0x00100100ull) {
        if (t.req.size == 3) saw_dbl_to_device = true;
        if (t.req.addr == kUartScratch && t.req.size == 2) saw_exact = true;
      }
    }
    reporter.Check(saw_exact, "the device request is the exact word access, not a refill");
    reporter.Check(!saw_dbl_to_device, "the device address never reaches the cache as a line");
    (void)r;
  }

  // 5b. A dirty eviction writes its bytes back.
  dir.Reset();
  {
    const uint64_t a = 0x80003040ull;   // index 2
    const uint64_t b = 0x80003140ull;   // index 2, different tag
    const uint64_t value = 0x1122334455667788ull;
    bool timed_out = false;
    uint64_t b0 = 0, b1 = 0;
    dir.Transact(true, a, 3, value, 0xff, 2, &timed_out, &b0, &b1);
    reporter.Check(!timed_out, "the directed store completes");
    // Accessing B evicts A's dirty line; the writeback must carry A's bytes.
    dir.Transact(false, b, 3, 0, 0, 2, &timed_out, &b0, &b1);
    reporter.Check(!timed_out, "the directed evicting load completes");
    uint64_t stored = 0;
    const AccessStatus st = dmem.Read(a, 8, &stored);
    reporter.Check(st == AccessStatus::kOk && stored == value,
                   "the dirty line was written back byte for byte");
    // Reading A again must return the stored value (through a fresh refill).
    uint64_t a0 = 0, a1 = 0;
    Rsp r = dir.Transact(false, a, 3, 0, 0, 2, &timed_out, &a0, &a1);
    reporter.Check(!timed_out && !r.fault && r.rdata == value,
                   "the evicted line reads back the stored value");
  }

  // 5c. A refill that faults is never marked valid, and is retried.
  dir.Reset();
  {
    const uint64_t c = 0x80004000ull;   // index 0
    dir.SetFault(c & ~0x1full, ~0x1full);
    bool timed_out = false;
    uint64_t b0 = 0, b1 = 0;
    Rsp r1 = dir.Transact(false, c, 3, 0, 0, 0, &timed_out, &b0, &b1);
    reporter.Check(!timed_out && r1.fault, "a faulted refill reaches the CPU as a fault");
    reporter.Check(dut.cb_dbg_valid_o == 0, "the faulted refill did not mark the line valid");
    const bool refilled_once = b1 > b0;
    // A second access must try the refill again (it did not become a hit).
    uint64_t c0 = 0, c1 = 0;
    Rsp r2 = dir.Transact(false, c, 3, 0, 0, 0, &timed_out, &c0, &c1);
    reporter.Check(refilled_once && c1 > c0,
                   "the next access retries the refill rather than hitting a bad line");
    reporter.Check(!timed_out && r2.fault, "the retried refill faults again while poisoned");
    // Clear the fault: the third access must succeed and install the line.
    dir.ClearFault();
    uint64_t d0 = 0, d1 = 0;
    Rsp r3 = dir.Transact(false, c, 3, 0, 0, 0, &timed_out, &d0, &d1);
    reporter.Check(!timed_out && !r3.fault, "the refill succeeds once the memory is good");
    reporter.Check(dut.cb_dbg_valid_o != 0, "the successful refill installs the line valid");
  }

  const std::string verdict = reporter.failures() == 0 ? "PASS" : "FAIL";
  return reporter.Finish(verdict, "cache integration: on/off identical, caches used, "
                                  "FENCE.I visible, MMIO bypassed, directed controls clean");
}
