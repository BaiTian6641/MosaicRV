// ============================================================================
// tb_core_perf.cpp -- CASE=perf.equal_resource_compare, work package I-084.
//
// The card: change exactly one variable per measurement -- a scheduling,
// locality or fusion variable -- and report the legal signature, the actual work
// rate and the per-workload result *including negative ones*, on equal
// resources. Pure IPC is not a signoff.
//
// -------------------------------------------------------------- what it runs
//
// A fixed set of three in-memory programs is run through the same elaborated
// core under a fixed set of configurations. Every configuration differs from
// the baseline by exactly one switch:
//
//   baseline      every policy switch off
//   +steering     fab_dyn_i high (I-090's one strategy bit: dynamic steering +
//                 the bank preference it drives + the cluster bypass)
//   +locality     loc_llb_en_i high (I-060's clean-copy locality buffer)
//   +prefetch     loc_pf_en_i high (I-060's bounded prefetcher)
//   +both         loc_llb_en_i and loc_pf_en_i high -- the one *interaction*
//                 the locality step table says behaves differently from either
//                 structure alone, so it is measured rather than assumed
//   +coalescing   vec_coalesce_dis_i low (I-061's line coalescing in the vector
//                 memory path; the baseline runs with it disabled)
//   +lanequota    the lane broker is asked for a share of 4 (I-059)
//
// The L1 cache enable (`cache_en_i`) is held high in *every* configuration. It
// is not one of the seven variables: the LLB and the prefetcher live inside the
// cache path, so a comparison that flipped the cache while flipping the LLB
// would be measuring two things at once. It is part of the fixed platform, and
// the equal-resource assertion below covers its geometry.
//
// The three workloads:
//
//   alu_chain   a dependent ALU chain with a non-commutative op, a branch loop
//               and a couple of loads -- what the scheduler/steering acts on.
//   stream      I-060's own streaming program, whose measured step table
//               already contains the negative this package must carry forward:
//               the prefetcher *alone* is slower than the baseline with zero
//               useful prefetches, while with the LLB every issue is useful.
//   vec_stream  a vector contiguous load/add/store at e16,m1 (vl = 8): the
//               workload on which coalescing and the lane quota are variables.
//
// ----------------------------------------------------------- what it checks
//
//   1. EQUAL RESOURCES. Every configuration is run on the same elaborated core,
//      and the harness reads the resource geometry back from the DUT -- ROB
//      entries, issue-queue entries, PRF entries and banks, clusters, the L1
//      line/set/way geometry, and the lane budget the broker itself reports --
//      and requires every cell's bundle to be byte-identical. A configuration
//      that secretly changed a resource count would fail here.
//   2. ARCHITECTURAL IDENTITY, per configuration, on the retire stream and the
//      signatures. Dynamic scheduling and locality may change *when* work
//      executes; they must never change *what* is computed. The stream is
//      compared field by field through the program's park instruction, exactly
//      as the locality integration compared it.
//   3. THE FEATURE ENGAGED. Each configuration's own evidence counter is
//      required to be non-zero on the workload that exercises it -- steering
//      granted macros, the LLB hit, the prefetcher issued, the coalescer merged,
//      the broker published a share. This is the check that a "configuration"
//      which is really the baseline under another name fails: it is the I-090
//      `NO_DELTA` idea, generalised to every variable.
//   4. WORK RATE, NOT IPC. The table reports cycles, retired instructions, IPC
//      *and* the raw work counters (data-port beats, vector elements, per-lane
//      element counts) per workload per configuration. Nothing in this case is
//      a threshold on IPC; a configuration that is slower is reported as slower.
//
// -------------------------------------------------------------- the controls
//
// Three negative controls, each built with one `-D` from an empty directory (see
// tools/run_perf_controls.py):
//
//   MOSAIC_PERF_MUTANT_RESOURCE_DRIFT  (driver)  the resource bundle the harness
//       reads is drifted for every configuration after the first, as if the
//       dynamic build had more ROB entries and lanes. Check 1 must catch it.
//   MOSAIC_FAB_MUTANT_DYN_SWAP_SRC     (RTL)     the dynamic route's two
//       operand-value wires cross, so a non-commutative ALU op computes a
//       different result in the dynamic configuration only. Check 2 must catch
//       it. The workload is built with a `sub` precisely so this control has
//       something to break.
//   MOSAIC_FAB_MUTANT_NO_DELTA         (RTL)     the strategy the core sees is
//       held low, so the "+steering" configuration *is* the baseline. Check 3
//       must catch it: the fabric never granted a macro.
//
// The controls are not part of the registered run.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"

#include "elf_loader.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic::AccessStatus;
using mosaic::MemoryModel;
using mosaic::Options;
using mosaic::Reporter;

namespace {

constexpr int kResetCycles = 8;
constexpr int kDrainCycles = 64;
constexpr int kStallCycles = 60000;
constexpr uint64_t kRamBase = 0x80000000ull;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string Dec(uint64_t value) { return std::to_string(value); }
std::string U64(uint64_t value) { return mosaic::Hex(value); }

// ============================================================================
// The assembler
// ============================================================================
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
constexpr int kOpImm = 0x13, kOpOp = 0x33, kOpAmo = 0x2f, kOpVec = 0x57,
              kOpVecMem = 0x07, kOpVecMemS = 0x27;
constexpr int kF3Ld = 3, kF3Sd = 3, kF3Sw = 2, kF3Lw = 2, kF3Addi = 0, kF3Add = 0,
              kF3Blt = 4, kF3Bne = 1, kF3Beq = 0;

uint32_t AmoAddD(int rd, int rs2, int rs1) {
  return (uint32_t(0x00) << 27) | (uint32_t(rs2) << 20) | (uint32_t(rs1) << 15) |
         (uint32_t(3) << 12) | (uint32_t(rd) << 7) | uint32_t(kOpAmo);
}

class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}

  int Label() { return next_label_++; }
  void Bind(int label) { labels_[label] = off(); }
  int off() const { return int(code_.size()) * 4; }
  uint64_t AddrOf(int byte_off) const { return base_ + uint64_t(byte_off); }

  void Auipc(int rd, int imm20) { Emit(EncU(imm20, rd, kOpAuipc)); }
  void Lui(int rd, int imm20) { Emit(EncU(imm20, rd, 0x37)); }
  void Addi(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Addi, rd, kOpImm)); }
  void Ori(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, 6, rd, kOpImm)); }
  void Add(int rd, int rs1, int rs2) { Emit(EncR(0, rs2, rs1, kF3Add, rd, kOpOp)); }
  void Sub(int rd, int rs1, int rs2) { Emit(EncR(0x20, rs2, rs1, 0, rd, kOpOp)); }
  void Xor(int rd, int rs1, int rs2) { Emit(EncR(0, rs2, rs1, 4, rd, kOpOp)); }
  void Ld(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Ld, rd, kOpLoad)); }
  void Lw(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Lw, rd, kOpLoad)); }
  void Sd(int rs2, int rs1, int imm) { Emit(EncS(imm, rs2, rs1, kF3Sd, kOpStore)); }
  void Sw(int rs2, int rs1, int imm) { Emit(EncS(imm, rs2, rs1, kF3Sw, kOpStore)); }
  void Amo(int rd, int rs2, int rs1) { Emit(AmoAddD(rd, rs2, rs1)); }
  void FenceI() { Emit(0x0000100fu); }
  void Csrrw(int rd, int csr, int rs1) { Emit(EncI(csr, rs1, 1, rd, 0x73)); }
  void Csrrs(int rd, int csr, int rs1) { Emit(EncI(csr, rs1, 2, rd, 0x73)); }
  void Csrr(int rd, int csr) { Csrrs(rd, csr, 0); }
  void JalSelf() { Emit(0x0000006fu); }
  void Blt(int rs1, int rs2, int label) {
    bfix_.push_back({int(code_.size()), label, rs1, rs2});
    code_.push_back(0);
  }
  void Bne(int rs1, int rs2, int label) {
    nfix_.push_back({int(code_.size()), label, rs1, rs2});
    code_.push_back(0);
  }
  void Beq(int rs1, int rs2, int label) {
    efix_.push_back({int(code_.size()), label, rs1, rs2});
    code_.push_back(0);
  }
  void Jal(int rd, int label) {
    jfix_.push_back({int(code_.size()), label, rd});
    code_.push_back(0);
  }

  // Load an absolute 64-bit address PC-relative (auipc + addi).
  void LaAbs(int rd, uint64_t target) {
    const int64_t delta = int64_t(target) - int64_t(base_ + uint64_t(off()));
    const uint32_t lo = uint32_t(delta) & 0xFFFu;
    const uint32_t hi = uint32_t((delta + 0x800) >> 12) & 0xFFFFFu;
    Emit(EncU(int(hi), rd, kOpAuipc));
    Emit(EncI(int(lo) - ((lo & 0x800u) ? 0x1000 : 0), rd, 0, rd, kOpImm));
  }

  // ------------------------------------------------------------- V encodings
  void Vsetvli(int rd, int rs1, uint32_t vtypei) {
    Emit(((vtypei & 0x7FFu) << 20) | (uint32_t(rs1) << 15) | (7u << 12) |
         (uint32_t(rd) << 7) | uint32_t(kOpVec));
  }
  void Vle16(int vd, int rs1) {
    Emit((1u << 25) | (uint32_t(rs1) << 15) | (5u << 12) | (uint32_t(vd) << 7) |
         uint32_t(kOpVecMem));
  }
  void Vse16(int vs3, int rs1) {
    Emit((1u << 25) | (uint32_t(rs1) << 15) | (5u << 12) | (uint32_t(vs3) << 7) |
         uint32_t(kOpVecMemS));
  }
  void VaddVv(int vd, int vs2, int vs1) {
    Emit((1u << 25) | (uint32_t(vs2) << 20) | (uint32_t(vs1) << 15) | (0u << 12) |
         (uint32_t(vd) << 7) | uint32_t(kOpVec));
  }

  std::vector<uint8_t> Bytes() const {
    std::vector<uint32_t> fixed = code_;
    for (const auto& f : bfix_) {
      fixed[size_t(f.word)] =
          EncB(labels_.at(f.label) - f.word * 4, f.rs2, f.rs1, kF3Blt, kOpBr);
    }
    for (const auto& f : nfix_) {
      fixed[size_t(f.word)] =
          EncB(labels_.at(f.label) - f.word * 4, f.rs2, f.rs1, kF3Bne, kOpBr);
    }
    for (const auto& f : efix_) {
      fixed[size_t(f.word)] =
          EncB(labels_.at(f.label) - f.word * 4, f.rs2, f.rs1, kF3Beq, kOpBr);
    }
    for (const auto& f : jfix_) {
      fixed[size_t(f.word)] = EncJ(labels_.at(f.label) - f.word * 4, f.rd);
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
  uint64_t base_;
  std::map<int, int> labels_;
  std::vector<BFix> bfix_, nfix_, efix_;
  std::vector<JFix> jfix_;
  std::vector<uint32_t> code_;
  int next_label_ = 0;
};

// ============================================================================
// The memory port: one request/response port with a fixed latency.
// ============================================================================
struct Rsp {
  uint64_t rdata = 0;
  bool fault = false;
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

  Bus(MemoryModel* mem, int latency) : mem_(mem), latency_(latency < 0 ? 0 : latency) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
    accepted_ = 0;
    last_addr_ = 0;
  }
  bool HasRsp() const { return !ready_.empty(); }
  const Rsp& Current() const { return ready_.front(); }
  uint64_t accepted() const { return accepted_; }
  uint64_t last_addr() const { return last_addr_; }

  void Accept(const Req& r) {
    accepted_++;
    last_addr_ = r.addr;
    inflight_.push_back({Perform(r), latency_});
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
  uint64_t accepted_ = 0;
  uint64_t last_addr_ = 0;
};

// ============================================================================
// The workloads
// ============================================================================
struct Workload {
  const char* name = "";
  std::vector<uint8_t> image;   // bytes, relative to kRamBase
  uint64_t park_pc = 0;
  // An architectural result the program leaves in memory (not in a register),
  // read back after the run and compared across configurations. The vector
  // workload uses it so the comparison does not depend on a scalar load racing
  // a vector store -- which is a property of the *program*, not of coalescing.
  uint64_t result_addr = 0;
  uint32_t result_len = 0;
};

void Put64(std::vector<uint8_t>* image, uint64_t off, uint64_t value) {
  if (off + 8 > image->size()) image->resize(size_t(off) + 8, 0);
  for (int b = 0; b < 8; b++) (*image)[size_t(off) + size_t(b)] = uint8_t((value >> (8 * b)) & 0xff);
}

// W1: a dependent ALU chain. The `sub` is deliberate: it is non-commutative, so
// the dynamic-route operand-swap negative control has something to break, which
// a chain of `add`/`xor` would not.
Workload BuildAluChain() {
  Workload w;
  w.name = "alu_chain";
  const int x0 = 0, t0 = 5, t1 = 6, t2 = 7, t3 = 28, t4 = 29, s1 = 9, a2 = 12,
            a3 = 13;
  Asm a(kRamBase);
  const uint64_t off_a = 0x800, off_b = 0x808;

  a.LaAbs(s1, kRamBase + 0x800);       // data base
  a.Addi(a2, x0, 0);                    // accumulator
  a.Addi(a3, x0, 48);                   // iterations, counted down
  const int loop = a.Label();
  a.Bind(loop);
  a.Ld(t0, s1, int(off_a - 0x800));
  a.Ld(t1, s1, int(off_b - 0x800));
  a.Sub(t2, t0, t1);                    // non-commutative: the swap control's target
  a.Add(t3, t2, t0);
  a.Add(t4, t1, t1);                    // independent work for the router
  a.Xor(t2, t3, t4);
  a.Add(a2, a2, t2);
  a.Add(a2, a2, t3);
  a.Addi(a3, a3, -1);
  // `bne` to x0 is symmetric under the dynamic-route operand swap, so the swap
  // control changes only the accumulator (the `sub`), not the loop trip count --
  // otherwise the run would derail into unmapped memory and the *identity* check
  // would never be reached.
  a.Bne(a3, x0, loop);
  a.LaAbs(t0, MOSAIC_SIGNATURE_ADDR);
  a.Sd(a2, t0, 0);
  a.LaAbs(t0, MOSAIC_TOHOST);
  a.Addi(t1, x0, 1);
  a.Sd(t1, t0, 0);
  a.FenceI();
  const int park = a.Label();
  a.Bind(park);
  w.park_pc = a.AddrOf(a.off());
  a.JalSelf();

  w.image = a.Bytes();
  Put64(&w.image, 0x800 + 0, 0x0000000C00000007ull);
  Put64(&w.image, 0x800 + 8, 0x0000000500000003ull);
  return w;
}

// W2: I-060's own streaming program, copied so the locality comparison is the
// same measurement the locality report made -- including its negative.
Workload BuildStream() {
  const uint64_t kOffA = 0x000, kOffB = 0x100;
  const uint64_t kOffC = 0x0A0, kOffD = 0x1A0;
  const uint64_t kOffE = 0x0E0, kOffF = 0x1E0;
  const uint64_t kOffPfBase = 0x0C0;
  const uint64_t kPfBase = 0x1FF000;
  const int kPfLines = 16;

  Workload w;
  w.name = "stream";
  Asm a(kRamBase);
  const int x0 = 0, t0 = 5, t1 = 6, t2 = 7, s0 = 8, s1 = 9, a1 = 11, a2 = 12;
  const int a3 = 13, a5 = 15, s2 = 18, s3 = 19, s4 = 20, s5 = 21, t3 = 28, t5 = 30,
            t6 = 31, t7 = 26, t8 = 27;

  a.Auipc(s0, 0);
  a.Addi(s0, s0, 0x7ff);
  a.Addi(s0, s0, 1);                    // s0 = 0x80000800
  const int t4 = 29;
  a.Lui(t4, 0x00100);
  a.Addi(t4, t4, 0x00C);                // the UART scratch marker port

  a.Addi(a2, x0, 0);
  a.Addi(s1, x0, 0);
  a.Addi(a1, x0, 32);
  const int reuse = a.Label();
  a.Bind(reuse);
  a.Ld(t0, s0, int(kOffA));
  a.Add(a2, a2, t0);
  a.Ld(t1, s0, int(kOffB));
  a.Add(a2, a2, t1);
  a.Addi(s1, s1, 1);
  a.Blt(s1, a1, reuse);

  a.Ld(t0, s0, int(kOffF));
  a.Ld(t0, s0, int(kOffE));
  a.Ld(t0, s0, int(kOffF));
  a.Addi(s2, s0, int(kOffE));
  a.Addi(t1, x0, 5);
  a.Amo(x0, t1, s2);
  a.Ld(t2, s0, int(kOffE));

  a.Ld(s3, s0, int(kOffPfBase));
  a.Addi(s2, x0, 0);
  a.Addi(a3, x0, kPfLines);
  const int pf = a.Label();
  a.Bind(pf);
  a.Ld(t0, s3, 0);
  a.Add(a2, a2, t0);
  a.Addi(s3, s3, 0x100);
  a.Addi(s2, s2, 1);
  a.Blt(s2, a3, pf);

  a.Ld(t0, s0, int(kOffC));
  a.Ld(t0, s0, int(kOffD));
  a.Ld(t0, s0, int(kOffC));
  a.Ld(t0, s0, int(kOffD));
  a.Lw(t5, t4, 0);
  a.Xor(t6, t5, t5);
  a.Add(t6, t6, s0);
  a.Ld(t0, t6, int(kOffC));
  a.Xor(t7, t0, t0);
  a.Add(t7, t7, t4);
  a.Lw(t5, t7, 0);
  a.Xor(t8, t5, t5);
  a.Add(t8, t8, s0);
  a.Ld(t0, t8, int(kOffD));
  a.Lw(t5, t4, 0);
  a.Xor(t6, t5, t5);
  a.Add(t6, t6, s0);
  a.Sd(x0, t6, int(kOffC));
  a.Addi(t3, x0, 0x21);
  a.Sw(t3, t4, 0);

  a.Addi(a5, s0, -0x400);
  a.Sd(a2, a5, 0);
  a.Sd(t2, a5, 8);
  a.Addi(s4, s0, 0x7ff);
  a.Addi(s4, s4, 1);
  a.Addi(s5, x0, 1);
  a.Sd(s5, s4, 0);
  a.FenceI();
  const int hang = a.Label();
  a.Bind(hang);
  w.park_pc = a.AddrOf(a.off());
  a.JalSelf();

  w.image = a.Bytes();
  const size_t need = size_t(kPfBase) + size_t(kPfLines) * 0x100;
  w.image.resize(std::max(w.image.size(), need), 0);
  auto put = [&w](uint64_t off, uint64_t value) {
    for (int b = 0; b < 8; b++) w.image[size_t(off) + size_t(b)] = uint8_t((value >> (8 * b)) & 0xff);
  };
  const uint64_t kDataBase = 0x800;
  put(kDataBase + kOffA, 7);
  put(kDataBase + kOffB, 9);
  put(kDataBase + kOffC, 11);
  put(kDataBase + kOffD, 13);
  put(kDataBase + kOffE, 100);
  put(kDataBase + kOffF, 17);
  put(kDataBase + kOffPfBase, 0x801FF000ull);
  for (int i = 0; i < kPfLines; i++) put(kPfBase + uint64_t(i) * 0x100, uint64_t((i + 1) * 3));
  return w;
}

// W3: a vector contiguous load/add/store at e16,m1 (vl = 8). Eight halfwords
// span two 8-byte beats, so coalescing has something to merge; eight elements
// across eight lanes is exactly the shape the lane quota reallocates.
Workload BuildVecStream() {
  Workload w;
  w.name = "vec_stream";
  const uint64_t data = 0x80020000ull;
  const uint64_t sig = MOSAIC_SIGNATURE_ADDR;
  const int x0 = 0, t0 = 5, t1 = 6, a0 = 10, a1 = 11, a2 = 12;
  Asm a(kRamBase);
  a.LaAbs(a0, data + 0);
  a.LaAbs(a1, data + 16);
  a.LaAbs(a2, data + 32);
  a.Csrr(t0, 0x300);
  a.Ori(t0, t0, 0x200);                 // mstatus.VS = Initial
  a.Csrrw(x0, 0x300, t0);
  a.Vsetvli(t1, x0, 0x20);              // e16, m1 -> vl = 8
  a.Vle16(0, a0);
  a.Vle16(1, a1);
  a.VaddVv(2, 0, 1);
  a.Vse16(2, a2);
  // The result is observed from memory, not by a scalar load issued behind the
  // vector store: with no dependence between them, a scalar load may execute
  // before the store drains, and which value it sees is then a property of the
  // program's ordering (or lack of it), not of coalescing. The FENCE.I below
  // flushes the write-back cache so the merged (or unmerged) store reaches
  // memory, and the harness reads it back.
  a.LaAbs(t0, sig);
  a.Sd(t1, t0, 0);                      // vl
  a.LaAbs(t0, MOSAIC_TOHOST);
  a.Addi(t1, x0, 1);
  a.Sd(t1, t0, 0);
  a.FenceI();
  const int park = a.Label();
  a.Bind(park);
  w.park_pc = a.AddrOf(a.off());
  a.JalSelf();

  w.result_addr = data + 32;            // C
  w.result_len = 16;
  w.image = a.Bytes();
  w.image.resize(size_t(data - kRamBase) + 64, 0);
  auto put16 = [&w](uint64_t addr, uint16_t value) {
    const uint64_t off = addr - kRamBase;
    w.image[size_t(off)] = uint8_t(value & 0xff);
    w.image[size_t(off) + 1] = uint8_t((value >> 8) & 0xff);
  };
  for (int i = 0; i < 8; i++) {
    put16(data + uint64_t(i) * 2, uint16_t(i + 1));
    put16(data + 16 + uint64_t(i) * 2, uint16_t(2 * (i + 1)));
  }
  return w;
}

// ============================================================================
// The configurations
// ============================================================================
enum class Feature { kNone, kSteering, kLocality, kPrefetch, kBoth, kCoalesce, kLaneQuota };

struct Config {
  const char* name;
  Feature feature;
  bool steering;      // fab_dyn_i
  bool llb;           // loc_llb_en_i
  bool pf;            // loc_pf_en_i
  bool coalescing;    // !vec_coalesce_dis_i
  bool lane_quota;    // request a share of 4
  int primary;        // the workload index that exercises the feature
};

constexpr int kWorkloads = 3;
const Config kConfigs[] = {
    {"baseline", Feature::kNone, false, false, false, false, false, -1},
    {"+steering", Feature::kSteering, true, false, false, false, false, 0},
    {"+locality", Feature::kLocality, false, true, false, false, false, 1},
    {"+prefetch", Feature::kPrefetch, false, false, true, false, false, 1},
    {"+both", Feature::kBoth, false, true, true, false, false, 1},
    {"+coalescing", Feature::kCoalesce, false, false, false, true, false, 2},
    {"+lanequota", Feature::kLaneQuota, false, false, false, false, true, 2},
};
constexpr int kConfigCount = int(sizeof(kConfigs) / sizeof(kConfigs[0]));
constexpr uint32_t kLaneQuotaVal = 4;

// ============================================================================
// Geometry and the resource bundle
// ============================================================================
struct Geometry {
  uint32_t retire_width = 0;
  uint32_t seq_w = 0;
  uint32_t ret_id_w = 0;
  uint32_t rob_entries = 0;
};

Geometry ReadGeometry(Vmosaic_core_tb* dut) {
  Geometry g;
  g.retire_width = dut->o_geom_retire_width_o;
  g.seq_w = dut->o_geom_seq_w_o;
  g.ret_id_w = dut->o_geom_ret_id_w_o;
  g.rob_entries = dut->o_geom_rob_entries_o;
  return g;
}

// The resources a performance comparison could secretly change. Read from the
// DUT's own geometry/evidence ports -- the core single-sources them -- so this
// is the machine that was built, not a number the harness restated.
struct Resources {
  uint32_t rob_entries = 0;
  uint32_t iq_entries = 0;
  uint32_t prf_entries = 0;
  uint32_t prf_banks = 0;
  uint32_t clusters = 0;
  uint32_t cache_line_bytes = 0;
  uint32_t cache_sets = 0;
  uint32_t cache_ways = 0;
  uint32_t lane_quota_max = 0;
  uint32_t lane_quota_reset = 0;
  uint32_t vec_vlenb = 0;

  bool operator==(const Resources& o) const {
    return rob_entries == o.rob_entries && iq_entries == o.iq_entries &&
           prf_entries == o.prf_entries && prf_banks == o.prf_banks &&
           clusters == o.clusters && cache_line_bytes == o.cache_line_bytes &&
           cache_sets == o.cache_sets && cache_ways == o.cache_ways &&
           lane_quota_max == o.lane_quota_max &&
           lane_quota_reset == o.lane_quota_reset && vec_vlenb == o.vec_vlenb;
  }
};

Resources ReadResources(Vmosaic_core_tb* dut, int config_index) {
  Resources r;
  r.rob_entries = dut->o_geom_rob_entries_o;
  r.iq_entries = dut->o_geom_iq_entries_o;
  r.prf_entries = dut->o_geom_prf_entries_o;
  r.prf_banks = dut->o_geom_prf_banks_o;
  r.clusters = dut->o_geom_clusters_o;
  r.cache_line_bytes = dut->o_geom_cache_line_bytes_o;
  r.cache_sets = dut->o_geom_cache_sets_o;
  r.cache_ways = dut->o_geom_cache_ways_o;
  r.lane_quota_max = dut->o_lane_quota_max_o;
  r.lane_quota_reset = dut->o_lane_quota_reset_o;
  r.vec_vlenb = uint32_t(dut->o_vec_vlenb_o);
#ifdef MOSAIC_PERF_MUTANT_RESOURCE_DRIFT
  // NEGATIVE CONTROL: every configuration after the first is reported with a
  // bigger ROB and a wider lane budget, as if the "dynamic" build had secretly
  // been given more resources. The equal-resource assertion must catch it. The
  // drift is injected here, at the one place a real geometry change could enter
  // the comparison.
  if (config_index > 0) {
    r.rob_entries += 16;
    r.lane_quota_max += 4;
  }
#else
  (void)config_index;
#endif
  return r;
}

std::string DescribeResources(const Resources& r) {
  return "rob=" + Dec(r.rob_entries) + " iq=" + Dec(r.iq_entries) +
         " prf=" + Dec(r.prf_entries) + " banks=" + Dec(r.prf_banks) +
         " clusters=" + Dec(r.clusters) + " l1=" + Dec(r.cache_line_bytes) + "B/" +
         Dec(r.cache_sets) + "sets/" + Dec(r.cache_ways) + "way lanes=" +
         Dec(r.lane_quota_max) + " reset=" + Dec(r.lane_quota_reset) +
         " vlenb=" + Dec(r.vec_vlenb);
}

// ============================================================================
// One retired instruction, as the frozen event port published it
// ============================================================================
struct RetireRec {
  uint64_t seq = 0, pc = 0, insn = 0, len = 0, reg_we = 0, rd = 0, value = 0;
  uint64_t store = 0, store_addr = 0, store_data = 0, store_size = 0;
  uint64_t csr_we = 0, csr_addr = 0, csr_value = 0;
  uint64_t trap = 0, trap_cause = 0, trap_tval = 0;

  bool operator==(const RetireRec& o) const {
    return seq == o.seq && pc == o.pc && insn == o.insn && len == o.len &&
           reg_we == o.reg_we && rd == o.rd && value == o.value && store == o.store &&
           store_addr == o.store_addr && store_data == o.store_data &&
           store_size == o.store_size && csr_we == o.csr_we && csr_addr == o.csr_addr &&
           csr_value == o.csr_value && trap == o.trap && trap_cause == o.trap_cause &&
           trap_tval == o.trap_tval;
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

std::string DumpRec(const RetireRec& r) {
  return "seq=" + Dec(r.seq) + " pc=" + U64(r.pc) + " insn=" + U64(r.insn) +
         " len=" + Dec(r.len) + " we=" + Dec(r.reg_we) + " rd=" + Dec(r.rd) +
         " val=" + U64(r.value) + " store=" + Dec(r.store) + " sa=" + U64(r.store_addr) +
         " sd=" + U64(r.store_data) + " ss=" + Dec(r.store_size) + " csrwe=" +
         Dec(r.csr_we) + " csra=" + U64(r.csr_addr) + " csrv=" + U64(r.csr_value) +
         " trap=" + Dec(r.trap) + " cause=" + Dec(r.trap_cause) +
         " tval=" + U64(r.trap_tval);
}

// ============================================================================
// One run's record
// ============================================================================
struct RunRec {
  bool finished = false;
  bool passed = false;
  uint64_t cycles = 0;
  uint64_t commits = 0;
  uint64_t imem_beats = 0;
  uint64_t dmem_beats = 0;
  uint64_t invariant_violations = 0;
  std::string invariant_first;
  std::vector<RetireRec> retires;
  bool sig_readable = false;
  uint64_t sig[4] = {0, 0, 0, 0};
  std::vector<uint8_t> result;   // the workload's memory result window
  Resources res;

  uint32_t fab_grant = 0, fab_stall = 0, fab_reject = 0, fab_bp_captured = 0,
           fab_bp_hit = 0, fab_alloc_bank0 = 0, fab_alloc_bank1 = 0;
  uint32_t llb_hit = 0, llb_miss = 0, llb_fill = 0, llb_inv = 0;
  uint32_t pf_issued = 0, pf_useful = 0, pf_useless = 0, pf_late = 0, pf_cancelled = 0;
  uint32_t vec_macro = 0, vec_elem = 0, vec_retire = 0, vec_lsu_req = 0,
           vec_lsu_merge = 0, vec_alu_elems = 0;
  uint32_t lane_quota = 0, lane_publish = 0, lane_ack = 0, lane_gen = 0;
  uint32_t lane_elem[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t c0_alu = 0, c1_alu = 0, c0_br = 0, c1_br = 0, muldiv = 0;

  uint64_t lane_elem_total() const {
    uint64_t t = 0;
    for (uint32_t l = 0; l < 8; l++) t += lane_elem[l];
    return t;
  }
};

// ============================================================================
// The runner: one configuration of one workload, from reset to the exit protocol
// ============================================================================
class Runner {
 public:
  Runner(Vmosaic_core_tb* dut, const Geometry& g) : dut_(dut), g_(g) {}

  RunRec Run(const Workload& w, const Config& cfg, int config_index, uint64_t max_cycles) {
    RunRec rec;
    MemoryModel mem;
    mosaic::Image img;
    mosaic::Segment seg;
    seg.vaddr = kRamBase;
    seg.memsz = w.image.size();
    seg.filesz = w.image.size();
    seg.flags = 7;
    seg.data = w.image;
    img.segments.push_back(seg);
    img.entry = kRamBase;
    std::string detail;
    if (!mem.LoadImage(img, &detail)) {
      Fail("setup", detail);
    }

    Bus imem(&mem, 2);
    Bus dmem(&mem, 2);
    imem.Reset();
    dmem.Reset();

    cycles_ = 0;
    violations_ = 0;
    invariant_first_.clear();
    retires_.clear();
    last_commit_ = 0;
    last_progress_ = 0;
    cfg_ = &cfg;

    dut_->clk = 0;
    dut_->rst = 1;
    dut_->eval();
    for (int i = 0; i < kResetCycles; i++) Cycle(&imem, &dmem, true);

    bool finished = false;
    for (uint64_t i = 0; i < max_cycles; i++) {
      Cycle(&imem, &dmem, false);
      if (mem.finished()) {
        finished = true;
        break;
      }
      if (dut_->o_stopped_o != 0) break;
      if (cycles_ - last_progress_ > uint64_t(kStallCycles)) break;
    }
    for (int d = 0; d < kDrainCycles; d++) Cycle(&imem, &dmem, false);

    rec.finished = finished;
    rec.passed = mem.passed();
    rec.cycles = cycles_;
    rec.commits = dut_->o_commit_o;
    rec.imem_beats = imem.accepted();
    rec.dmem_beats = dmem.accepted();
    rec.invariant_violations = violations_;
    rec.invariant_first = invariant_first_;
    rec.retires = retires_;
#ifdef MOSAIC_PERF_MUTANT_ARCH_DRIFT
    // NEGATIVE CONTROL: every configuration after the first is reported with the
    // first retiring register-write's value perturbed, as if the "dynamic" path
    // computed a different result. The architectural-identity check must catch
    // it. Like the resource-drift control, the mutation is injected at the
    // comparison's input -- the observed retire stream -- which is the only place
    // a wrong result could reach the check.
    if (config_index > 0) {
      for (RetireRec& r : rec.retires) {
        if (r.reg_we) {
          r.value ^= 1;
          break;
        }
      }
    }
#endif
    {
      std::vector<uint64_t> words;
      if (mem.ReadSignature(&words) && words.size() == 4) {
        rec.sig_readable = true;
        for (int k = 0; k < 4; k++) rec.sig[k] = words[k];
      }
    }
    for (uint32_t i = 0; i < w.result_len; i++) {
      uint64_t byte = 0;
      if (mem.Read(w.result_addr + i, 1, &byte) == AccessStatus::kOk) {
        rec.result.push_back(uint8_t(byte & 0xff));
      } else {
        rec.result.push_back(0xff);
      }
    }
    rec.res = ReadResources(dut_, config_index);
    rec.fab_grant = dut_->o_fab_grant_ctr_o;
    rec.fab_stall = dut_->o_fab_stall_ctr_o;
    rec.fab_reject = dut_->o_fab_reject_ctr_o;
    rec.fab_bp_captured = dut_->o_fab_bp_captured_o;
    rec.fab_bp_hit = dut_->o_fab_bp_hit_o;
    rec.fab_alloc_bank0 = alloc_bank0_;
    rec.fab_alloc_bank1 = alloc_bank1_;
    rec.llb_hit = dut_->o_loc_llb_hit_o;
    rec.llb_miss = dut_->o_loc_llb_miss_o;
    rec.llb_fill = dut_->o_loc_llb_fill_o;
    rec.llb_inv = dut_->o_loc_llb_inv_o;
    rec.pf_issued = dut_->o_loc_pf_issued_o;
    rec.pf_useful = dut_->o_loc_pf_useful_o;
    rec.pf_useless = dut_->o_loc_pf_useless_o;
    rec.pf_late = dut_->o_loc_pf_late_o;
    rec.pf_cancelled = dut_->o_loc_pf_cancelled_o;
    rec.vec_macro = dut_->o_vec_macro_ctr_o;
    rec.vec_elem = dut_->o_vec_elem_ctr_o;
    rec.vec_retire = dut_->o_vec_retire_ctr_o;
    rec.vec_lsu_req = dut_->o_vec_lsu_req_ctr_o;
    rec.vec_lsu_merge = dut_->o_vec_lsu_merge_ctr_o;
    rec.vec_alu_elems = dut_->o_vec_alu_elems_o;
    rec.lane_quota = dut_->o_lane_quota_o;
    rec.lane_publish = dut_->o_lane_publish_ctr_o;
    rec.lane_ack = dut_->o_lane_ack_ctr_o;
    rec.lane_gen = dut_->o_lane_gen_o;
    for (uint32_t l = 0; l < 8; l++) rec.lane_elem[l] = dut_->o_lane_elem_ctr_o[l];
    rec.c0_alu = dut_->o_c0_alu_o;
    rec.c1_alu = dut_->o_c1_alu_o;
    rec.c0_br = dut_->o_c0_br_o;
    rec.c1_br = dut_->o_c1_br_o;
    rec.muldiv = dut_->o_muldiv_o;

    if (!rec.finished) {
      std::string why = "the run did not reach the exit protocol";
      if (dut_->o_stopped_o != 0) {
        why = "the machine stopped on an instruction it refuses (unsupported=" +
              Dec(dut_->o_unsupported_o) + " illegal=" + Dec(dut_->o_illegal_o) + ")";
      } else if (cycles_ >= max_cycles) {
        why = "the cycle bound was reached";
      } else {
        why = "no commit and no allocation for " + Dec(kStallCycles) + " cycles";
      }
      Fail(std::string(cfg.name) + " on " + w.name, why);
    }
    return rec;
  }

 private:
  void Note(const std::string& text) {
    violations_++;
    if (invariant_first_.empty()) {
      invariant_first_ = "cycle " + Dec(cycles_) + ": " + text;
    }
  }

  void Observe() {
    const uint32_t mask = (g_.retire_width >= 32)
                              ? 0xFFFFFFFFu
                              : ((1u << g_.retire_width) - 1u);
    const uint32_t valid = uint32_t(dut_->ev_valid_o) & mask;
    for (uint32_t lane = 0; lane < g_.retire_width && lane < 32; lane++) {
      if ((valid & (1u << lane)) == 0) continue;
      RetireRec r;
      r.seq = ScalarField(dut_->ev_seq_o, lane, g_.seq_w);
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.insn = ScalarField(dut_->ev_insn_o, lane, 32);
      r.len = ScalarField(dut_->ev_len_o, lane, 3);
      r.reg_we = ScalarField(dut_->ev_reg_we_o, lane, 1);
      r.rd = ScalarField(dut_->ev_rd_o, lane, 5);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.store = ScalarField(dut_->ev_store_o, lane, 1);
      r.store_addr = PayloadLane(dut_->ev_store_addr_o, lane);
      r.store_data = PayloadLane(dut_->ev_store_data_o, lane);
      r.store_size = ScalarField(dut_->ev_store_size_o, lane, 3);
      r.csr_we = ScalarField(dut_->ev_csr_we_o, lane, 1);
      r.csr_addr = ScalarField(dut_->ev_csr_addr_o, lane, 12);
      r.csr_value = PayloadLane(dut_->ev_csr_value_o, lane);
      r.trap = ScalarField(dut_->ev_trap_o, lane, 1);
      r.trap_cause = PayloadLane(dut_->ev_trap_cause_o, lane);
      r.trap_tval = PayloadLane(dut_->ev_trap_tval_o, lane);
      retires_.push_back(r);
    }

    if (dut_->o_squash_nc_o != 0 || dut_->o_squash_under_o != 0 ||
        dut_->o_journal_ovf_o != 0) {
      Note("recovery counters: squash_nc=" + Dec(dut_->o_squash_nc_o) + " under=" +
           Dec(dut_->o_squash_under_o) + " journal=" + Dec(dut_->o_journal_ovf_o));
    }
    if (dut_->o_rob_occupied_o > g_.rob_entries) {
      Note("ROB occupancy " + Dec(dut_->o_rob_occupied_o) + " exceeds " +
           Dec(g_.rob_entries));
    }
    if (dut_->o_mem_lq_query_mismatch_o != 0) {
      Note("the load queue's forwarding-query cross-check tripped: " +
           Dec(dut_->o_mem_lq_query_mismatch_o));
    }
    if (dut_->o_mem_sq_auth_o > dut_->o_mem_sq_occupied_o) {
      Note("store-queue authorised prefix " + Dec(dut_->o_mem_sq_auth_o) +
           " exceeds occupancy " + Dec(dut_->o_mem_sq_occupied_o));
    }

    if (cfg_ != nullptr && cfg_->steering && dut_->o_dbg_alloc_ctr_o != last_alloc_ctr_) {
      last_alloc_ctr_ = dut_->o_dbg_alloc_ctr_o;
      if (dut_->o_fab_alloc_bank_o == 0) alloc_bank0_++;
      if (dut_->o_fab_alloc_bank_o == 1) alloc_bank1_++;
    }

    const bool progressed = (dut_->o_commit_o != last_commit_) ||
                            (dut_->o_wb_pub_valid_o != 0) ||
                            (dut_->o_dbg_alloc_ctr_o != last_alloc_);
    last_commit_ = dut_->o_commit_o;
    last_alloc_ = dut_->o_dbg_alloc_ctr_o;
    if (progressed) last_progress_ = cycles_;
  }

  void Cycle(Bus* imem, Bus* dmem, bool rst) {
    dut_->rst = rst ? 1 : 0;
    dut_->fab_dyn_i = (cfg_ != nullptr && cfg_->steering) ? 1 : 0;
    // The L1 path is the fixed platform: every configuration runs with it on,
    // because the locality structures live inside it. See the header.
    dut_->cache_en_i = 1;
    dut_->loc_llb_en_i = (cfg_ != nullptr && cfg_->llb) ? 1 : 0;
    dut_->loc_pf_en_i = (cfg_ != nullptr && cfg_->pf) ? 1 : 0;
    dut_->loc_pf_conf_thresh_i = 1;
    dut_->loc_snoop_all_i = 0;
    dut_->vec_coalesce_dis_i = (cfg_ != nullptr && cfg_->coalescing) ? 0 : 1;
    dut_->lane_quota_req_i = (cfg_ != nullptr && cfg_->lane_quota) ? 1 : 0;
    dut_->lane_quota_val_i = kLaneQuotaVal;

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
    if (!rst) Observe();

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      Bus::Req r;
      r.addr = dut_->imem_req_addr_o;
      r.size = dut_->imem_req_size_o;
      r.inst = true;
      imem->Accept(r);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem->Pop();
    }
    imem->Advance();

    if (bus_reset_.MayAccept(rst, (dut_->dmem_req_valid_o != 0) &&
                                      (dut_->dmem_req_ready_i != 0))) {
      Bus::Req r;
      r.we = dut_->dmem_req_we_o != 0;
      r.addr = dut_->dmem_req_addr_o;
      r.size = dut_->dmem_req_size_o;
      r.wstrb = uint8_t(dut_->dmem_req_wstrb_o);
      r.wdata = dut_->dmem_req_wdata_o;
      dmem->Accept(r);
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
    ++cycles_;
  }

  Vmosaic_core_tb* dut_;
  mosaic::BusResetGate bus_reset_;
  Geometry g_;
  const Config* cfg_ = nullptr;
  uint64_t cycles_ = 0;
  uint64_t violations_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_alloc_ctr_ = 0;
  uint64_t last_progress_ = 0;
  uint32_t alloc_bank0_ = 0;
  uint32_t alloc_bank1_ = 0;
  std::string invariant_first_;
  std::vector<RetireRec> retires_;
};

// ============================================================================
// The report's own helpers
// ============================================================================
std::string Ipc(uint64_t insns, uint64_t cycles) {
  if (cycles == 0) return "n/a";
  const uint64_t whole = insns / cycles;
  const uint64_t frac = (insns % cycles) * 1000 / cycles;
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%llu.%03llu", (unsigned long long)whole,
                (unsigned long long)frac);
  return std::string(buf);
}

size_t ParkIndex(const std::vector<RetireRec>& stream, uint64_t park_pc) {
  for (size_t i = 0; i < stream.size(); i++) {
    if (stream[i].pc == park_pc) return i;
  }
  return size_t(-1);
}

const char* FeatureName(Feature f) {
  switch (f) {
    case Feature::kNone: return "none";
    case Feature::kSteering: return "steering";
    case Feature::kLocality: return "llb";
    case Feature::kPrefetch: return "prefetch";
    case Feature::kBoth: return "llb+prefetch";
    case Feature::kCoalesce: return "coalescing";
    case Feature::kLaneQuota: return "lane quota";
  }
  return "?";
}

// The feature's own evidence that it did work. This is what a "configuration"
// that is really the baseline cannot fake.
bool Engaged(Feature f, const RunRec& r) {
  switch (f) {
    case Feature::kNone: return true;
    case Feature::kSteering: return r.fab_grant > 0;
    case Feature::kLocality: return (r.llb_hit + r.llb_miss) > 0;
    case Feature::kPrefetch: return r.pf_issued > 0;
    case Feature::kBoth: return (r.llb_hit + r.llb_miss) > 0 && r.pf_issued > 0;
    case Feature::kCoalesce: return r.vec_lsu_merge > 0;
    case Feature::kLaneQuota: return r.lane_publish > 0 && r.lane_quota == kLaneQuotaVal;
  }
  return false;
}

std::string EngagementDetail(Feature f, const RunRec& r) {
  switch (f) {
    case Feature::kSteering:
      return "fabric grants=" + Dec(r.fab_grant);
    case Feature::kLocality:
      return "llb hit+miss=" + Dec(r.llb_hit + r.llb_miss);
    case Feature::kPrefetch:
      return "prefetches issued=" + Dec(r.pf_issued);
    case Feature::kBoth:
      return "llb hit+miss=" + Dec(r.llb_hit + r.llb_miss) +
             " prefetches issued=" + Dec(r.pf_issued);
    case Feature::kCoalesce:
      return "elements merged out of the request stream=" + Dec(r.vec_lsu_merge);
    case Feature::kLaneQuota:
      return "quota=" + Dec(r.lane_quota) + " publishes=" + Dec(r.lane_publish);
    default:
      return "n/a";
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  bool passed = true;
  std::string detail;
  try {
    dut.clk = 0;
    dut.rst = 1;
    dut.eval();
    const Geometry geom = ReadGeometry(&dut);
    if (geom.retire_width == 0 || geom.rob_entries == 0) {
      Fail("setup", "the wrapper published no geometry");
    }

    std::vector<Workload> workloads = {BuildAluChain(), BuildStream(), BuildVecStream()};

    std::printf("perf.equal_resource_compare: retire_width=%u rob=%u workloads=%d "
                "configurations=%d\n",
                geom.retire_width, geom.rob_entries, kWorkloads, kConfigCount);

    Runner runner(&dut, geom);
    std::vector<std::vector<RunRec>> runs(kWorkloads, std::vector<RunRec>(kConfigCount));
    for (int wi = 0; wi < kWorkloads; wi++) {
      for (int ci = 0; ci < kConfigCount; ci++) {
        runs[wi][ci] = runner.Run(workloads[wi], kConfigs[ci], ci, options.max_cycles);
      }
    }

    // ------------------------------------------------------------- the table
    std::printf("\nper-workload table (cycles, retired instructions, IPC, work "
                "counters):\n");
    for (int wi = 0; wi < kWorkloads; wi++) {
      const Workload& w = workloads[wi];
      std::printf("  workload %s (park_pc=%s)\n", w.name, U64(w.park_pc).c_str());
      std::printf("    %-12s %10s %8s %7s %8s %8s %10s %10s\n", "config", "cycles",
                  "insns", "IPC", "dmem", "imem", "vec_elem", "merged");
      for (int ci = 0; ci < kConfigCount; ci++) {
        const RunRec& r = runs[wi][ci];
        std::printf("    %-12s %10llu %8zu %7s %8llu %8llu %10u %10u\n",
                    kConfigs[ci].name, (unsigned long long)r.cycles,
                    r.retires.size(), Ipc(r.retires.size(), r.cycles).c_str(),
                    (unsigned long long)r.dmem_beats, (unsigned long long)r.imem_beats,
                    r.vec_elem, r.vec_lsu_merge);
      }
    }

    // ----------------------------------------------- the per-config features
    std::printf("\nfeature activity on the workload that exercises each "
                "configuration:\n");
    for (int ci = 1; ci < kConfigCount; ci++) {
      const Config& c = kConfigs[ci];
      const RunRec& r = runs[c.primary][ci];
      std::printf("  %-12s on %-11s %-14s %s\n", c.name, workloads[c.primary].name,
                  FeatureName(c.feature), EngagementDetail(c.feature, r).c_str());
    }

    // ------------------------------------------------- check 1: equal resources
    const Resources& base_res = runs[0][0].res;
    std::printf("\nequal resources: %s\n", DescribeResources(base_res).c_str());
    for (int wi = 0; wi < kWorkloads; wi++) {
      for (int ci = 0; ci < kConfigCount; ci++) {
        const Resources& res = runs[wi][ci].res;
        const bool same = (res == base_res);
        if (!same) {
          reporter.Mismatch("the resource bundle of " + std::string(kConfigs[ci].name) +
                                " on " + workloads[wi].name,
                            DescribeResources(base_res), DescribeResources(res));
        }
        reporter.Check(same,
                       "every configuration runs on the same resources: " +
                           std::string(kConfigs[ci].name) + " on " + workloads[wi].name +
                           " reads back the same ROB/IQ/PRF/L1/lane geometry");
      }
    }

    // ------------------------------ check 2: architectural identity per config
    for (int wi = 0; wi < kWorkloads; wi++) {
      const Workload& w = workloads[wi];
      const RunRec& base = runs[wi][0];
      reporter.Check(base.finished && base.passed,
                     std::string(w.name) + ": the baseline reaches the exit protocol and "
                     "reports PASS");
      const size_t base_park = ParkIndex(base.retires, w.park_pc);
      reporter.Check(base_park != size_t(-1),
                     std::string(w.name) + ": the baseline reaches the park instruction");
      for (int ci = 1; ci < kConfigCount; ci++) {
        const RunRec& r = runs[wi][ci];
        const size_t park = ParkIndex(r.retires, w.park_pc);
        bool same = (park != size_t(-1)) && (park == base_park);
        size_t at = 0;
        if (same) {
          for (size_t i = 0; i <= base_park; i++) {
            if (!(base.retires[i] == r.retires[i])) {
              same = false;
              at = i;
              break;
            }
          }
        } else {
          at = std::min(base.retires.size(), r.retires.size());
        }
        if (!same) {
          reporter.Mismatch(std::string(kConfigs[ci].name) + " on " + w.name +
                                " retirement " + Dec(at),
                            "the same architecture as the baseline",
                            "a different retire stream");
          if (at < base.retires.size() && at < r.retires.size()) {
            std::printf("    baseline:   %s\n", DumpRec(base.retires[at]).c_str());
            std::printf("    %-11s %s\n", kConfigs[ci].name, DumpRec(r.retires[at]).c_str());
          } else {
            std::printf("    baseline retired %zu, %s retired %zu\n", base.retires.size(),
                        kConfigs[ci].name, r.retires.size());
          }
        }
        reporter.Check(same,
                       std::string(kConfigs[ci].name) + " on " + w.name +
                           ": retires the same architecture as the baseline, field by "
                           "field through the park");
        bool sig_same = base.sig_readable && r.sig_readable;
        for (int k = 0; k < 4 && sig_same; k++) {
          if (base.sig[k] != r.sig[k]) sig_same = false;
        }
        if (!sig_same) {
          reporter.Mismatch(std::string(kConfigs[ci].name) + " on " + w.name + " signature",
                            "the baseline's four signature words",
                            "different words");
        }
        reporter.Check(sig_same,
                       std::string(kConfigs[ci].name) + " on " + w.name +
                           ": leaves the same four signature words as the baseline");
        const bool result_same = (r.result == base.result);
        if (!result_same) {
          reporter.Mismatch(std::string(kConfigs[ci].name) + " on " + w.name +
                                " memory result",
                            "the baseline's memory result", "different bytes");
        }
        reporter.Check(result_same,
                       std::string(kConfigs[ci].name) + " on " + w.name +
                           ": leaves the same memory result as the baseline");
      }
    }

    // ------------------------------------- check 3: the variable actually engaged
    for (int ci = 1; ci < kConfigCount; ci++) {
      const Config& c = kConfigs[ci];
      const RunRec& r = runs[c.primary][ci];
      const bool engaged = Engaged(c.feature, r);
      if (!engaged) {
        reporter.Mismatch(std::string(c.name) + " on " + workloads[c.primary].name,
                          "the " + std::string(FeatureName(c.feature)) +
                              " variable did work",
                          EngagementDetail(c.feature, r));
      }
      reporter.Check(engaged,
                     std::string(c.name) + " actually engaged: the " +
                         FeatureName(c.feature) + " variable did work on " +
                         workloads[c.primary].name + " (" +
                         EngagementDetail(c.feature, r) + ")");
    }
    // The baseline must be inert: none of the variables may be doing work.
    for (int wi = 0; wi < kWorkloads; wi++) {
      const RunRec& b = runs[wi][0];
      reporter.Check(b.fab_grant == 0 && b.llb_hit == 0 && b.llb_fill == 0 &&
                         b.pf_issued == 0 && b.vec_lsu_merge == 0,
                     std::string(workloads[wi].name) +
                         ": the baseline is inert -- no grants, no LLB fills, no "
                         "prefetches, no merges");
    }

    // ------------------------------------------- check 4: the machine's invariants
    for (int wi = 0; wi < kWorkloads; wi++) {
      for (int ci = 0; ci < kConfigCount; ci++) {
        const RunRec& r = runs[wi][ci];
        reporter.Check(r.invariant_violations == 0,
                       std::string(kConfigs[ci].name) + " on " + workloads[wi].name +
                           ": the machine's invariants hold every cycle");
        reporter.Check(r.commits == r.retires.size(),
                       std::string(kConfigs[ci].name) + " on " + workloads[wi].name +
                           ": the committed-instruction count equals the retire events "
                           "published");
      }
    }

    // --------------------------------------------- the honest per-workload deltas
    std::printf("\ndelta against the baseline (negative = faster than the baseline):\n");
    for (int wi = 0; wi < kWorkloads; wi++) {
      const RunRec& base = runs[wi][0];
      std::printf("  workload %s:\n", workloads[wi].name);
      for (int ci = 1; ci < kConfigCount; ci++) {
        const RunRec& r = runs[wi][ci];
        const long long dcyc = (long long)r.cycles - (long long)base.cycles;
        const long long ddmem = (long long)r.dmem_beats - (long long)base.dmem_beats;
        std::printf("    %-12s cycles %+8lld (%s) dmem %+8lld  work: ", kConfigs[ci].name,
                    dcyc, dcyc < 0 ? "faster" : (dcyc > 0 ? "slower" : "same"), ddmem);
        switch (kConfigs[ci].feature) {
          case Feature::kSteering:
            std::printf("grants=%u bp_hit=%u banks=%u/%u\n", r.fab_grant, r.fab_bp_hit,
                        r.fab_alloc_bank0, r.fab_alloc_bank1);
            break;
          case Feature::kLocality:
            std::printf("llb hit=%u miss=%u fill=%u\n", r.llb_hit, r.llb_miss, r.llb_fill);
            break;
          case Feature::kPrefetch:
            std::printf("pf issued=%u useful=%u useless=%u late=%u\n", r.pf_issued,
                        r.pf_useful, r.pf_useless, r.pf_late);
            break;
          case Feature::kBoth:
            std::printf("llb hit=%u pf issued=%u useful=%u\n", r.llb_hit, r.pf_issued,
                        r.pf_useful);
            break;
          case Feature::kCoalesce:
            std::printf("vec req=%u merged=%u elems=%u\n", r.vec_lsu_req,
                        r.vec_lsu_merge, r.vec_elem);
            break;
          case Feature::kLaneQuota:
            std::printf("quota=%u lane_elems=[%u %u %u %u %u %u %u %u] total=%llu\n",
                        r.lane_quota, r.lane_elem[0], r.lane_elem[1], r.lane_elem[2],
                        r.lane_elem[3], r.lane_elem[4], r.lane_elem[5], r.lane_elem[6],
                        r.lane_elem[7], (unsigned long long)r.lane_elem_total());
            break;
          default:
            std::printf("\n");
            break;
        }
      }
    }

    // The architectural result each workload leaves in memory, so the identity
    // column of the report has the bytes it compared.
    for (int wi = 0; wi < kWorkloads; wi++) {
      if (workloads[wi].result_len == 0) continue;
      std::printf("\nmemory result of %s at %s:\n", workloads[wi].name,
                  U64(workloads[wi].result_addr).c_str());
      for (int ci = 0; ci < kConfigCount; ci++) {
        std::printf("    %-12s ", kConfigs[ci].name);
        for (uint8_t b : runs[wi][ci].result) std::printf("%02x", b);
        std::printf("\n");
      }
    }

    // The negative the plan already measured, restated on this harness: the
    // prefetcher *alone* on the stream workload.
    {
      const RunRec& base = runs[1][0];
      const RunRec& pf = runs[1][3];
      std::printf("\nnegative carried forward: on stream, +prefetch alone is %s than the "
                  "baseline (%lld cycles) with %u useful of %u issued prefetches; "
                  "+both is %s (%lld cycles)\n",
                  pf.cycles > base.cycles ? "slower" : (pf.cycles < base.cycles ? "faster" : "the same"),
                  (long long)pf.cycles - (long long)base.cycles, pf.pf_useful, pf.pf_issued,
                  runs[1][4].cycles > base.cycles
                      ? "slower"
                      : (runs[1][4].cycles < base.cycles ? "faster" : "the same"),
                  (long long)runs[1][4].cycles - (long long)base.cycles);
    }

    passed = (reporter.failures() == 0);
    detail = "workloads=3 configs=7 baseline_cycles=" + Dec(runs[0][0].cycles) + "/" +
             Dec(runs[1][0].cycles) + "/" + Dec(runs[2][0].cycles) +
             " resources=" + DescribeResources(base_res) +
             " arch_identical=yes";
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the equal-resource comparison", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
