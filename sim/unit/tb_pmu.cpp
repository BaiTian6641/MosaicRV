// ============================================================================
// tb_pmu.cpp -- CASE=pmu.trace_accounting, work package I-076.
//
// The card: count per-hart retired instructions, uOPs, useful elements, stall
// reasons, occupancy, remote traffic, WB conflicts, replays and MPKI; align
// every counter against a *hand-computable* trace. Pass is that the counters
// reconcile with the trace and that the sums and the classification show no
// under-counting and no double-counting, with CSR visibility and
// reference-comparability stated explicitly. Fail is judging speed by vector IPC
// against scalar IPC, or presenting toggle counts as measured power -- neither
// is done here; see the report's not-covered list.
//
// ------------------------------------------------------------- what it runs
//
// One scalar program with a *fixed* trip count, run twice through the same
// elaborated core:
//
//   phase "cache_off"  cache_en_i low  -- the cacheless observation point. The
//                      memory counters (LSU transactions, fetch requests) are
//                      the endpoint's and reconcile with the harness's own bus
//                      beat count.
//   phase "cache_on"   cache_en_i high -- the L1D is engaged, so the data-cache
//                      hit/miss/refill/writeback counters and MPKI can be
//                      reconciled against a hand-counted set of cache lines.
//
// The program (LOOPS = 6, so 48 retired instructions before the park) is:
//
//      la   x10, DATA_A         2   auipc+addi   ALU
//      la   x11, DATA_B         2   auipc+addi   ALU
//      addi x1, x0, 0           1   ALU
//      addi x12, x0, 6          1   ALU
//   L: ld   x3, 0(x10)          1   LOAD
//      add  x1, x1, x3          1   ALU
//      addi x1, x1, 1           1   ALU
//      addi x12, x12, -1        1   ALU
//      bne  x12, x0, L          1   CTRL
//      sd   x1, 0(x11)          1   STORE
//      csrr x6, minstret        1   SYS    (A)
//      addi x7, x0, 0           1   ALU
//      addi x7, x7, 1           1   ALU
//      csrr x8, minstret        1   SYS    (B)
//      sd   x6, 8(x11)          1   STORE
//      sd   x8, 16(x11)         1   STORE
//      la   x9, TOHOST          2   auipc+addi   ALU
//      addi x5, x0, 1           1   ALU
//      sd   x5, 0(x9)           1   STORE
//      fence.i                  1   SYS
//   PARK: jal x0, 0             1   CTRL
//
// The measurement stops when the self-loop park first retires (its retire
// acknowledgement is not yet in the committed counter at that edge, so the park
// is not part of the hand total). With six loop iterations the hand derivation
// is:
//
//   retired instructions       18 + 5*6 = 48   (park excluded)
//   class ALU                  11 + 3*6  = 29
//   class LOAD                 6          =  6
//   class STORE                4          =  4
//   class CTRL                 6          =  6
//   class SYS (2x csrr, fence.i)          =  3
//   register writes            ALU 29 + LOAD 6 + csrr 2 = 37
//   stores                     4         loads          6
//   distinct L1D lines touched: DATA_A (1), DATA_B (1), TOHOST (1)
//     -> cache_on: txn 10, hits 7, misses 3, refills 3, writebacks 2
//     -> MPKI = 3 / 48 * 1000 = 62.50 (L1D demand misses per kilo-instruction)
//
// Every number above comes from the program text and the profile's cache
// geometry (64 B line, p1), not from the DUT. The DUT counters must equal them,
// and the domain-accounting identities must close:
//
//   retire total  == sum of the classes (no under/double count)
//   uop insert    == uop issue + uop kill + live IQ occupancy (mosaic_iq's
//                    exactly-once conservation)
//   sq_alloc      == sq_drain + sq_squash + sq_occupied (mosaic_store_queue)
//   dcache hit + miss == dcache txn          (each accepted CPU request is a
//                    hit or a miss, never both, never neither)
//   dcache refill == dcache miss             (a demand miss starts one fill)
//
// ----------------------------------------------------------- CSR visibility
//
// The case prints, and the report repeats, which counters are architectural.
// minstret (0xC02/0xB02), cycle (0xC00/0xB00) and time (0xC01) are the Zicntr
// counters; minstret is read *by the program itself* through CSRR at two points,
// and the values are required to equal the retire-stream index of each CSRR.
// That is architectural and an ISA reference can reproduce it. Everything
// o_pmu_*/o_dbg_*/o_wb_*/o_mem_*/o_loc_*/o_fab_* is implementation
// observability: not CSR-visible and not reference-comparable, and it is
// labelled so. Zihpm is present in the profile (0xC03..0xC1F decode) but reads
// back zero -- a discoverable shadow, not a mapping of these events.
//
// ------------------------------------------------------------------ mutants
//
// tools/run_pmu_controls.py, each from a deleted build directory:
//   MOSAIC_PMU_MUTANT_DOUBLE_COMMIT  (rtl)  a squashed uop is counted as retired
//       (every redirect adds one to the committed count): the hand total must
//       catch it.
//   MOSAIC_PMU_MUTANT_SKIP_CLASS     (driver) the classification drops the CTRL
//       class, so the parts no longer sum to the whole: the classification
//       identity must catch it.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <tuple>
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
constexpr int kDrainCycles = 2048;
constexpr int kStallCycles = 60000;
constexpr uint64_t kRamBase = 0x80000000ull;
constexpr int kLoops = 6;

// --------------------------------------------------------------- hand counts
// The snapshot is taken in the cycle the park instruction first appears in the
// retire stream, and at that point the committed counter has not yet counted the
// park (the retire acknowledgement lands on the next edge). So the hand total is
// the program *up to but not including* the self-loop park: 18 straight-line
// instructions plus 5 per loop iteration.
constexpr uint64_t kHandRetired = 18 + 5 * kLoops;       // 48
constexpr uint64_t kHandAlu     = 11 + 3 * kLoops;       // 29
constexpr uint64_t kHandLoad    = kLoops;                // 6
constexpr uint64_t kHandStore   = 4;                     // 4
constexpr uint64_t kHandCtrl    = kLoops;                // 6
constexpr uint64_t kHandSys     = 3;                     // 3
constexpr uint64_t kHandWrites  = kHandAlu + kHandLoad + 2;  // 37
constexpr uint64_t kHandLoads   = kHandLoad;
constexpr uint64_t kHandStores  = kHandStore;
// cache_on: three distinct 64 B lines (DATA_A, DATA_B, TOHOST).
constexpr uint64_t kHandDcTxn    = kHandLoads + kHandStores;  // 10
constexpr uint64_t kHandDcMiss   = 3;
constexpr uint64_t kHandDcHit    = kHandDcTxn - kHandDcMiss;  // 7
constexpr uint64_t kHandDcRefill = kHandDcMiss;               // 3
constexpr uint64_t kHandDcWb     = 2;                         // DATA_B, TOHOST

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string Dec(uint64_t value) { return std::to_string(value); }
std::string U64(uint64_t value) { return mosaic::Hex(value); }

// ============================================================================
// The assembler (the subset this program needs)
// ============================================================================
constexpr uint32_t EncI(int imm, int rs1, int f3, int rd, int op) {
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
constexpr int kOpImm = 0x13, kOpOp = 0x33, kOpSys = 0x73;
constexpr int kF3Ld = 3, kF3Sd = 3, kF3Addi = 0, kF3Add = 0, kF3Bne = 1;

constexpr uint32_t kCsrrX6Minstret = EncI(0xC02, 0, 2, 6, kOpSys);  // csrr x6, minstret
constexpr uint32_t kCsrrX8Minstret = EncI(0xC02, 0, 2, 8, kOpSys);  // csrr x8, minstret

class Asm {
 public:
  explicit Asm(uint64_t base) : base_(base) {}
  int Label() { return next_label_++; }
  void Bind(int label) { labels_[label] = off(); }
  int off() const { return int(code_.size()) * 4; }
  uint64_t AddrOf(int byte_off) const { return base_ + uint64_t(byte_off); }

  void Addi(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Addi, rd, kOpImm)); }
  void Add(int rd, int rs1, int rs2) { Emit(EncR(0, rs2, rs1, kF3Add, rd, kOpOp)); }
  void Ld(int rd, int rs1, int imm) { Emit(EncI(imm, rs1, kF3Ld, rd, kOpLoad)); }
  void Sd(int rs2, int rs1, int imm) { Emit(EncS(imm, rs2, rs1, kF3Sd, kOpStore)); }
  void FenceI() { Emit(0x0000100fu); }
  void Csrr(int rd, int csr) { Emit(EncI(csr, 0, 2, rd, kOpSys)); }
  void Bne(int rs1, int rs2, int label) {
    nfix_.push_back({int(code_.size()), label, rs1, rs2});
    code_.push_back(0);
  }
  void JalSelf() { Emit(0x0000006fu); }

  // Load an absolute 64-bit address PC-relative (auipc + addi), two words.
  void LaAbs(int rd, uint64_t target) {
    const int64_t delta = int64_t(target) - int64_t(base_ + uint64_t(off()));
    const uint32_t lo = uint32_t(delta) & 0xFFFu;
    const uint32_t hi = uint32_t((delta + 0x800) >> 12) & 0xFFFFFu;
    Emit(EncU(int(hi), rd, kOpAuipc));
    Emit(EncI(int(lo) - ((lo & 0x800u) ? 0x1000 : 0), rd, 0, rd, kOpImm));
  }

  std::vector<uint8_t> Bytes() const {
    std::vector<uint32_t> fixed = code_;
    for (const auto& f : nfix_) {
      fixed[size_t(f.word)] =
          EncB(labels_.at(f.label) - f.word * 4, f.rs2, f.rs1, kF3Bne, kOpBr);
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(fixed.size() * 4);
    for (uint32_t w : fixed) {
      for (int b = 0; b < 4; b++) bytes.push_back(uint8_t((w >> (8 * b)) & 0xff));
    }
    return bytes;
  }

 private:
  struct NFix { int word, label, rs1, rs2; };
  void Emit(uint32_t w) { code_.push_back(w); }
  uint64_t base_;
  std::map<int, int> labels_;
  std::vector<NFix> nfix_;
  std::vector<uint32_t> code_;
  int next_label_ = 0;
};

// ============================================================================
// The program image and its data addresses
// ============================================================================
constexpr uint64_t kDataA = kRamBase + 0x800;   // 0x80000800
constexpr uint64_t kDataB = kRamBase + 0x900;   // 0x80000900

struct Program {
  std::vector<uint8_t> image;
  uint64_t park_pc = 0;
};

void Put64(std::vector<uint8_t>* image, uint64_t off, uint64_t value) {
  if (off + 8 > image->size()) image->resize(size_t(off) + 8, 0);
  for (int b = 0; b < 8; b++)
    (*image)[size_t(off) + size_t(b)] = uint8_t((value >> (8 * b)) & 0xff);
}

Program BuildProgram() {
  Program p;
  const int x0 = 0, x1 = 1, x3 = 3, x5 = 5, x6 = 6, x7 = 7, x8 = 8, x9 = 9,
            x10 = 10, x11 = 11, x12 = 12;
  Asm a(kRamBase);
  a.LaAbs(x10, kDataA);
  a.LaAbs(x11, kDataB);
  a.Addi(x1, x0, 0);
  a.Addi(x12, x0, kLoops);
  const int loop = a.Label();
  a.Bind(loop);
  a.Ld(x3, x10, 0);
  a.Add(x1, x1, x3);
  a.Addi(x1, x1, 1);
  a.Addi(x12, x12, -1);
  a.Bne(x12, x0, loop);
  a.Sd(x1, x11, 0);
  a.Csrr(x6, 0xC02);
  a.Addi(x7, x0, 0);
  a.Addi(x7, x7, 1);
  a.Csrr(x8, 0xC02);
  a.Sd(x6, x11, 8);
  a.Sd(x8, x11, 16);
  a.LaAbs(x9, MOSAIC_TOHOST);
  a.Addi(x5, x0, 1);
  a.Sd(x5, x9, 0);
  a.FenceI();
  const int park = a.Label();
  a.Bind(park);
  p.park_pc = a.AddrOf(a.off());
  a.JalSelf();

  p.image = a.Bytes();
  // Data lives in RAM, cacheable, and the two blocks are in different 64 B lines.
  p.image.resize(size_t(kDataB - kRamBase) + 64, 0);
  Put64(&p.image, kDataA - kRamBase, 7);
  return p;
}

// ============================================================================
// The memory port: one request/response port with a fixed latency.
// ============================================================================
struct Rsp {
  uint64_t rdata = 0;
  bool fault = false;
  // The instruction port matches a response to its request by id and epoch, so
  // the harness must echo what it accepted -- a cacheless fetch that sees a
  // wrong id/epoch treats the response as stale and waits forever. With the
  // cache enabled the path supplies its own identity and these are unused.
  uint32_t id = 0;
  uint32_t epoch = 0;
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
    uint32_t id = 0;
    uint32_t epoch = 0;
  };

  Bus(MemoryModel* mem, int latency) : mem_(mem), latency_(latency < 0 ? 0 : latency) {}

  void Reset() {
    inflight_.clear();
    ready_.clear();
    accepted_ = 0;
  }
  bool HasRsp() const { return !ready_.empty(); }
  const Rsp& Current() const { return ready_.front(); }
  uint64_t accepted() const { return accepted_; }

  void Accept(const Req& r) {
    accepted_++;
    Rsp rsp = Perform(r);
    rsp.id = r.id;
    rsp.epoch = r.epoch;
    inflight_.push_back({rsp, latency_});
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
};

// ============================================================================
// One retired instruction, as the frozen event port published it
// ============================================================================
struct RetireRec {
  uint64_t seq = 0, pc = 0, insn = 0, len = 0, reg_we = 0, rd = 0, value = 0;
  uint64_t csr_we = 0, csr_addr = 0, csr_value = 0;
  uint64_t trap = 0;
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

enum class OpClass { kAlu, kLoad, kStore, kCtrl, kSys, kOther };
const char* ClassName(OpClass c) {
  switch (c) {
    case OpClass::kAlu: return "ALU";
    case OpClass::kLoad: return "LOAD";
    case OpClass::kStore: return "STORE";
    case OpClass::kCtrl: return "CTRL";
    case OpClass::kSys: return "SYS";
    default: return "OTHER";
  }
}
OpClass Classify(uint64_t insn) {
  const uint32_t op = uint32_t(insn) & 0x7fu;
  switch (op) {
    case 0x13: case 0x17: case 0x1b: case 0x33: case 0x37: case 0x3b:
      return OpClass::kAlu;
    case 0x03: case 0x07: return OpClass::kLoad;
    case 0x23: case 0x27: return OpClass::kStore;
    case 0x63: case 0x67: case 0x6f: return OpClass::kCtrl;
    case 0x73: case 0x0f: case 0x2f: return OpClass::kSys;
    default: return OpClass::kOther;
  }
}

// ============================================================================
// The counter snapshot, taken in the cycle the park instruction retires
// ============================================================================
struct Snapshot {
  bool taken = false;
  uint64_t commit = 0, cycle = 0;
  uint32_t uop_insert = 0, uop_issue = 0, uop_kill = 0;
  uint32_t c0_count = 0, c1_count = 0;
  uint32_t dc_txn = 0, dc_hit = 0, dc_miss = 0, dc_refill = 0, dc_wb = 0;
  uint32_t ic_miss = 0;
  uint32_t wb_wr = 0, wb_wake = 0, wb_stale = 0, wb_dup = 0, wb_collision = 0,
           wb_drop = 0;
  uint32_t redirect = 0, recovering = 0, squash_acc = 0, ckpt = 0;
  uint32_t disp_occ_sum = 0, iq_occ_sum = 0, iqueue_occ_sum = 0;
  uint32_t fetch_req = 0, fetch_rsp = 0;
  uint32_t lsu_txn = 0;
  uint32_t lq_alloc = 0, lq_done = 0, lq_replay = 0, lq_occupied = 0;
  uint32_t sq_alloc = 0, sq_drain = 0, sq_squash = 0, sq_occupied = 0;
  uint64_t imem_beats = 0;   // the harness's own instruction-beat count
};

struct RunRec {
  Snapshot snap;
  std::vector<RetireRec> retires;   // truncated at and including the first park
  uint64_t imem_beats = 0;
  uint64_t dmem_beats = 0;
  bool finished = false;
  bool passed = false;
  uint64_t sim_cycles = 0;
};

// ============================================================================
// The runner
// ============================================================================
class Runner {
 public:
  Runner(Vmosaic_core_tb* dut) : dut_(dut) {}

  RunRec Run(const Program& prog, bool cache_on, uint64_t max_cycles) {
    RunRec rec;
    MemoryModel mem;
    mosaic::Image img;
    mosaic::Segment seg;
    seg.vaddr = kRamBase;
    seg.memsz = prog.image.size();
    seg.filesz = prog.image.size();
    seg.flags = 7;
    seg.data = prog.image;
    img.segments.push_back(seg);
    img.entry = kRamBase;
    std::string detail;
    if (!mem.LoadImage(img, &detail)) Fail("setup", detail);

    Bus imem(&mem, 2);
    Bus dmem(&mem, 2);
    imem.Reset();
    dmem.Reset();

    cycles_ = 0;
    retires_.clear();
    snap_ = Snapshot();
    cache_on_ = cache_on;
    park_pc_ = prog.park_pc;

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
      if (snap_.taken && cycles_ > snap_cycle_ + uint64_t(kDrainCycles)) break;
    }
    for (int d = 0; d < kDrainCycles; d++) Cycle(&imem, &dmem, false);

    if (!snap_.taken) {
      Fail("run", "the park instruction at " + U64(prog.park_pc) +
                      " never retired (commits=" + Dec(dut_->o_commit_o) +
                      " stopped=" + Dec(dut_->o_stopped_o) +
                      " unsupported=" + Dec(dut_->o_unsupported_o) +
                      " illegal=" + Dec(dut_->o_illegal_o) +
                      " redirects=" + Dec(dut_->o_redirect_o) +
                      " recovering=" + Dec(dut_->o_recovering_o) +
                      " fetch_pc=" + U64(dut_->o_fetch_pc_o) + ")");
    }
    rec.snap = snap_;
    // `snap_end_` includes the park record observed in the snapshot cycle; the
    // hand trace is the program before the park, so drop that one record.
    const size_t keep = (snap_end_ > 0) ? snap_end_ - 1 : 0;
    rec.retires.assign(retires_.begin(), retires_.begin() + long(keep));
    rec.imem_beats = imem.accepted();
    rec.dmem_beats = dmem.accepted();
    rec.finished = finished;
    rec.passed = mem.passed();
    rec.sim_cycles = cycles_;
    return rec;
  }

 private:
  void Capture() {
    snap_.taken = true;
    snap_cycle_ = cycles_;
    snap_.commit = dut_->o_commit_o;
    snap_.cycle = dut_->o_cycle_o;
    snap_.uop_insert = dut_->o_pmu_uop_insert_ctr_o;
    snap_.uop_issue = dut_->o_pmu_uop_issue_ctr_o;
    snap_.uop_kill = dut_->o_pmu_uop_kill_ctr_o;
    snap_.c0_count = dut_->o_c0_count_o;
    snap_.c1_count = dut_->o_c1_count_o;
    snap_.dc_txn = dut_->o_pmu_dcache_txn_ctr_o;
    snap_.dc_hit = dut_->o_pmu_dcache_hit_ctr_o;
    snap_.dc_miss = dut_->o_pmu_dcache_miss_ctr_o;
    snap_.dc_refill = dut_->o_pmu_dcache_refill_ctr_o;
    snap_.dc_wb = dut_->o_pmu_dcache_wb_ctr_o;
    snap_.ic_miss = dut_->o_pmu_icache_miss_ctr_o;
    snap_.wb_wr = dut_->o_wb_wr_o;
    snap_.wb_wake = dut_->o_wb_wake_o;
    snap_.wb_stale = dut_->o_wb_stale_o;
    snap_.wb_dup = dut_->o_wb_dup_o;
    snap_.wb_collision = dut_->o_wb_collision_o;
    snap_.wb_drop = dut_->o_wb_drop_o;
    snap_.redirect = dut_->o_redirect_o;
    snap_.recovering = dut_->o_recovering_o;
    snap_.squash_acc = dut_->o_squash_acc_o;
    snap_.ckpt = dut_->o_ckpt_o;
    snap_.disp_occ_sum = dut_->o_dbg_disp_occ_sum_o;
    snap_.iq_occ_sum = dut_->o_dbg_iq_occ_sum_o;
    snap_.iqueue_occ_sum = dut_->o_dbg_iqueue_occ_sum_o;
    snap_.fetch_req = dut_->o_dbg_fetch_req_ctr_o;
    snap_.fetch_rsp = dut_->o_dbg_fetch_rsp_ctr_o;
    snap_.lsu_txn = dut_->o_mem_lsu_txn_o;
    snap_.lq_alloc = dut_->o_mem_lq_alloc_o;
    snap_.lq_done = dut_->o_mem_lq_done_o;
    snap_.lq_replay = dut_->o_mem_lq_replay_o;
    snap_.lq_occupied = dut_->o_mem_lq_occupied_o;
    snap_.sq_alloc = dut_->o_mem_sq_alloc_o;
    snap_.sq_drain = dut_->o_mem_sq_drain_o;
    snap_.sq_squash = dut_->o_mem_sq_squash_o;
    snap_.sq_occupied = dut_->o_mem_sq_occupied_o;
    snap_end_ = retires_.size();
    snap_.imem_beats = imem_beats_cycle_;
  }

  void Observe() {
    const uint32_t valid = uint32_t(dut_->ev_valid_o);
    const uint32_t retire_width = 2;
    for (uint32_t lane = 0; lane < retire_width; lane++) {
      if ((valid & (1u << lane)) == 0) continue;
      RetireRec r;
      r.seq = ScalarField(dut_->ev_seq_o, lane, uint32_t(dut_->o_geom_seq_w_o));
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      r.insn = ScalarField(dut_->ev_insn_o, lane, 32);
      r.len = ScalarField(dut_->ev_len_o, lane, 3);
      r.reg_we = ScalarField(dut_->ev_reg_we_o, lane, 1);
      r.rd = ScalarField(dut_->ev_rd_o, lane, 5);
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.csr_we = ScalarField(dut_->ev_csr_we_o, lane, 1);
      r.csr_addr = ScalarField(dut_->ev_csr_addr_o, lane, 12);
      r.csr_value = PayloadLane(dut_->ev_csr_value_o, lane);
      r.trap = ScalarField(dut_->ev_trap_o, lane, 1);
      retires_.push_back(r);
      if (!snap_.taken && r.pc == park_pc_) Capture();
    }
  }

  void Cycle(Bus* imem, Bus* dmem, bool rst) {
    dut_->rst = rst ? 1 : 0;
    dut_->fab_dyn_i = 0;
    dut_->cache_en_i = cache_on_ ? 1 : 0;
    dut_->loc_llb_en_i = 0;
    dut_->loc_pf_en_i = 0;
    dut_->loc_pf_conf_thresh_i = 0;
    dut_->loc_snoop_all_i = 0;
    dut_->vec_coalesce_dis_i = 1;
    dut_->lane_quota_req_i = 0;
    dut_->lane_quota_val_i = 0;

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
    imem_beats_cycle_ = imem->accepted();
    if (!rst) Observe();

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      Bus::Req r;
      r.addr = dut_->imem_req_addr_o;
      r.size = dut_->imem_req_size_o;
      r.inst = true;
      r.id = dut_->imem_req_id_o;
      r.epoch = dut_->imem_req_epoch_o;
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
  uint64_t cycles_ = 0;
  bool cache_on_ = false;
  uint64_t park_pc_ = 0;
  uint64_t snap_cycle_ = 0;
  uint64_t snap_end_ = 0;
  uint64_t imem_beats_cycle_ = 0;
  Snapshot snap_;
  std::vector<RetireRec> retires_;
};

// ============================================================================
// Reporting helpers
// ============================================================================
void CounterRow(const char* name, uint64_t value, const char* unit, bool csr,
                bool comparable) {
  std::printf("    %-30s %10llu  %-10s  %-3s  %s\n", name,
              (unsigned long long)value, unit, csr ? "yes" : "no",
              comparable ? "yes" : "no");
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

  std::string detail;
  try {
    dut.clk = 0;
    dut.rst = 1;
    dut.eval();
    const Program prog = BuildProgram();

    Runner runner(&dut);
    RunRec off = runner.Run(prog, /*cache_on=*/false, options.max_cycles);
    RunRec on = runner.Run(prog, /*cache_on=*/true, options.max_cycles);

    std::printf("pmu.trace_accounting: hand-derived retired=%llu (ALU=%llu LOAD=%llu "
                "STORE=%llu CTRL=%llu SYS=%llu), park=%s\n",
                (unsigned long long)kHandRetired, (unsigned long long)kHandAlu,
                (unsigned long long)kHandLoad, (unsigned long long)kHandStore,
                (unsigned long long)kHandCtrl, (unsigned long long)kHandSys,
                U64(prog.park_pc).c_str());

    // ------------------------------------------------------------- helpers
    auto ClassifyRun = [&](const RunRec& rec, const char* phase) {
      uint64_t counts[6] = {0, 0, 0, 0, 0, 0};
      uint64_t writes = 0, traps = 0;
      int64_t idx_csrr6 = -1, idx_csrr8 = -1;
      uint64_t val_csrr6 = 0, val_csrr8 = 0;
      for (size_t i = 0; i < rec.retires.size(); i++) {
        const RetireRec& r = rec.retires[i];
        OpClass c = Classify(r.insn);
#ifdef MOSAIC_PMU_MUTANT_SKIP_CLASS
        // NEGATIVE CONTROL: the CTRL class is dropped from the classification
        // entirely, so the parts no longer sum to the whole. The classification
        // identity must catch the under-count.
        if (c == OpClass::kCtrl) continue;
#endif
        counts[int(c)]++;
        if (r.reg_we) writes++;
        if (r.trap) traps++;
        if (r.insn == kCsrrX6Minstret && idx_csrr6 < 0) {
          idx_csrr6 = int64_t(i);
          val_csrr6 = r.value;
        }
        if (r.insn == kCsrrX8Minstret && idx_csrr8 < 0) {
          idx_csrr8 = int64_t(i);
          val_csrr8 = r.value;
        }
      }
      return std::make_tuple(counts[0], counts[1], counts[2], counts[3], counts[4],
                             counts[5], writes, traps, idx_csrr6, val_csrr6,
                             idx_csrr8, val_csrr8);
    };

    if (std::getenv("PMU_DUMP_STREAM") != nullptr) {
      std::printf("retire stream (%zu):\n", on.retires.size());
      for (size_t i = 0; i < on.retires.size(); i++) {
        std::printf("  [%2zu] pc=%s insn=%08llx we=%llu rd=%llu val=%llu\n", i,
                    U64(on.retires[i].pc).c_str(),
                    (unsigned long long)on.retires[i].insn,
                    (unsigned long long)on.retires[i].reg_we,
                    (unsigned long long)on.retires[i].rd,
                    (unsigned long long)on.retires[i].value);
      }
    }

    // The cache_off phase's trace-derived architectural counts, for the CSR
    // visibility table below (the table must show what the DUT published, not
    // the hand constants the checks used).
    uint64_t t_alu = 0, t_load = 0, t_store = 0, t_ctrl = 0, t_sys = 0, t_writes = 0;
    uint64_t t_csrr_read = 0;

    for (int phase = 0; phase < 2; phase++) {
      const RunRec& rec = phase == 0 ? off : on;
      const char* name = phase == 0 ? "cache_off" : "cache_on";
      const Snapshot& s = rec.snap;

      std::printf("\nphase %s: retired=%llu stream=%zu cycles=%llu (sim %llu) "
                  "finished=%d passed=%d imem_beats=%llu dmem_beats=%llu\n",
                  name, (unsigned long long)s.commit, rec.retires.size(),
                  (unsigned long long)s.cycle, (unsigned long long)rec.sim_cycles,
                  rec.finished ? 1 : 0, rec.passed ? 1 : 0,
                  (unsigned long long)s.imem_beats,
                  (unsigned long long)rec.dmem_beats);

      auto [alu, load, store, ctrl, sys, other, writes, traps, idx6, val6, idx8,
            val8] = ClassifyRun(rec, name);
      const uint64_t cls_sum = alu + load + store + ctrl + sys + other;
      if (phase == 0) {
        t_alu = alu; t_load = load; t_store = store; t_ctrl = ctrl; t_sys = sys;
        t_writes = writes; t_csrr_read = val6;
      }

      // 1. the aggregate equals the hand total.
      reporter.Check(s.commit == kHandRetired,
                     std::string(name) + ": retired counter " + Dec(s.commit) +
                         " == hand " + Dec(kHandRetired));
      reporter.Check(rec.retires.size() == kHandRetired,
                     std::string(name) + ": retire trace length " +
                         Dec(rec.retires.size()) + " == hand " + Dec(kHandRetired));
      reporter.Check(traps == 0, std::string(name) + ": no trap retired (" +
                                    Dec(traps) + ")");

      // 2. the classification sums to the whole and each part is hand-derived.
      reporter.Check(cls_sum == s.commit,
                     std::string(name) + ": class sum " + Dec(cls_sum) +
                         " == retired " + Dec(s.commit) + " (no under/double count)");
      reporter.Check(other == 0,
                     std::string(name) + ": no unclassified retiring instruction (" +
                         Dec(other) + ")");
      reporter.Check(alu == kHandAlu && load == kHandLoad && store == kHandStore &&
                         ctrl == kHandCtrl && sys == kHandSys,
                     std::string(name) + ": classes ALU=" + Dec(alu) + " LOAD=" +
                         Dec(load) + " STORE=" + Dec(store) + " CTRL=" + Dec(ctrl) +
                         " SYS=" + Dec(sys) + " == hand (" + Dec(kHandAlu) + "," +
                         Dec(kHandLoad) + "," + Dec(kHandStore) + "," +
                         Dec(kHandCtrl) + "," + Dec(kHandSys) + ")");
      reporter.Check(writes == kHandWrites,
                     std::string(name) + ": retiring register writes " + Dec(writes) +
                         " == hand " + Dec(kHandWrites));

      // 3. the CSR-visible architectural counter, read by the program itself.
      reporter.Check(idx6 >= 0 && idx8 > idx6,
                     std::string(name) + ": both minstret CSRRs retired");
      if (idx6 >= 0 && idx8 > idx6) {
        const uint64_t expected_a = uint64_t(idx6);
        const uint64_t expected_b = uint64_t(idx8);
        reporter.Check(val6 == expected_a,
                       std::string(name) + ": minstret at first CSRR " + Dec(val6) +
                           " == retire-stream index " + Dec(expected_a));
        reporter.Check(val8 == expected_b,
                       std::string(name) + ": minstret at second CSRR " + Dec(val8) +
                           " == retire-stream index " + Dec(expected_b));
        reporter.Check(val8 - val6 == uint64_t(idx8 - idx6),
                       std::string(name) + ": minstret delta " + Dec(val8 - val6) +
                           " == instructions between the two CSRRs " +
                           Dec(uint64_t(idx8 - idx6)));
      }

      // 4. the uop domain closes (mosaic_iq's exactly-once conservation).
      const uint64_t live = uint64_t(s.c0_count) + uint64_t(s.c1_count);
      reporter.Check(uint64_t(s.uop_issue) + uint64_t(s.uop_kill) + live ==
                         uint64_t(s.uop_insert),
                     std::string(name) + ": uop insert " + Dec(s.uop_insert) +
                         " == issue " + Dec(s.uop_issue) + " + kill " +
                         Dec(s.uop_kill) + " + live " + Dec(live));
      // Every inserted uop is issued, killed or still live: the account closes
      // with no uop lost and none counted twice. This is the uop-domain
      // companion to the retire-domain classification identity above.
      reporter.Check(s.uop_insert > 0,
                     std::string(name) + ": uops were inserted (" +
                         Dec(s.uop_insert) + ")");

      // 5. the store queue closes; the loads all completed.
      reporter.Check(uint64_t(s.sq_drain) + uint64_t(s.sq_squash) +
                         uint64_t(s.sq_occupied) == uint64_t(s.sq_alloc),
                     std::string(name) + ": sq alloc " + Dec(s.sq_alloc) +
                         " == drain " + Dec(s.sq_drain) + " + squash " +
                         Dec(s.sq_squash) + " + occupied " + Dec(s.sq_occupied));
      reporter.Check(s.sq_drain == kHandStores,
                     std::string(name) + ": sq drained stores " + Dec(s.sq_drain) +
                         " == hand stores " + Dec(kHandStores));
      reporter.Check(s.lq_alloc == kHandLoads && s.lq_done == kHandLoads,
                     std::string(name) + ": lq alloc " + Dec(s.lq_alloc) +
                         " done " + Dec(s.lq_done) + " == hand loads " +
                         Dec(kHandLoads));
      reporter.Check(uint64_t(s.lq_done) + uint64_t(s.lq_occupied) <=
                         uint64_t(s.lq_alloc),
                     std::string(name) + ": lq done " + Dec(s.lq_done) +
                         " + occupied " + Dec(s.lq_occupied) + " <= alloc " +
                         Dec(s.lq_alloc) + " (no lost load)");

      // 6. the memory path: endpoint transactions (cache_off) and the L1D
      //    decomposition (cache_on).
      if (phase == 0) {
        reporter.Check(s.dc_txn == 0,
                       std::string(name) + ": L1D disengaged (txn " + Dec(s.dc_txn) +
                           " == 0)");
        reporter.Check(s.lsu_txn == kHandLoads + kHandStores,
                       std::string(name) + ": LSU endpoint txns " + Dec(s.lsu_txn) +
                           " == hand memory ops " + Dec(kHandLoads + kHandStores));
        reporter.Check(s.fetch_req == s.imem_beats,
                       std::string(name) + ": fetch requests " + Dec(s.fetch_req) +
                           " == harness imem beats " + Dec(s.imem_beats));
      } else {
        reporter.Check(s.dc_txn == kHandDcTxn,
                       std::string(name) + ": L1D CPU txns " + Dec(s.dc_txn) +
                           " == hand " + Dec(kHandDcTxn));
        reporter.Check(uint64_t(s.dc_hit) + uint64_t(s.dc_miss) ==
                           uint64_t(s.dc_txn),
                       std::string(name) + ": dcache hit " + Dec(s.dc_hit) +
                           " + miss " + Dec(s.dc_miss) + " == txn " + Dec(s.dc_txn) +
                           " (no double count)");
        reporter.Check(s.dc_miss == kHandDcMiss && s.dc_hit == kHandDcHit,
                       std::string(name) + ": dcache miss " + Dec(s.dc_miss) +
                           " hit " + Dec(s.dc_hit) + " == hand (" +
                           Dec(kHandDcMiss) + "," + Dec(kHandDcHit) + ")");
        reporter.Check(s.dc_refill == s.dc_miss,
                       std::string(name) + ": refills " + Dec(s.dc_refill) +
                           " == misses " + Dec(s.dc_miss));
        reporter.Check(s.dc_wb == kHandDcWb,
                       std::string(name) + ": writebacks " + Dec(s.dc_wb) +
                           " == hand dirty lines " + Dec(kHandDcWb));
        // MPKI, from the DUT's own miss counter and retired count.
        const double mpki = double(s.dc_miss) * 1000.0 / double(s.commit);
        std::printf("  %s: MPKI(L1D demand) = %u / %llu * 1000 = %.2f\n", name,
                    s.dc_miss, (unsigned long long)s.commit, mpki);
      }

      // 7. occupancy sums are cycle-integrated and cannot exceed capacity.
      const uint64_t iq_capacity = 2 * 8;      // 2 clusters x 8 IQ entries (p1)
      const uint64_t disp_capacity = 8;        // decoded-instruction queue depth
      reporter.Check(s.iq_occ_sum <= iq_capacity * s.cycle,
                     std::string(name) + ": IQ occupancy sum " + Dec(s.iq_occ_sum) +
                         " <= capacity*cycles " + Dec(iq_capacity * s.cycle));
      reporter.Check(s.disp_occ_sum <= disp_capacity * s.cycle,
                     std::string(name) + ": dispatch occupancy sum " +
                         Dec(s.disp_occ_sum) + " <= capacity*cycles " +
                         Dec(disp_capacity * s.cycle));
      reporter.Check(s.iq_occ_sum > 0 && s.disp_occ_sum > 0,
                     std::string(name) + ": occupancy sums are non-zero");
      std::printf("  %s: mean depth disp=%.2f iq=%.2f iqueue=%.2f (cycles=%llu)\n",
                  name, double(s.disp_occ_sum) / double(s.cycle),
                  double(s.iq_occ_sum) / double(s.cycle),
                  double(s.iqueue_occ_sum) / double(s.cycle),
                  (unsigned long long)s.cycle);
    }

    // ------------------------------------------------- CSR-visibility table
    std::printf("\nCSR visibility and reference-comparability (I-076 pass clause):\n");
    std::printf("    %-30s %10s  %-10s  %-3s  %s\n", "counter", "value", "unit",
                "CSR", "ref-comparable");
    CounterRow("minstret (CSRR, read by prog)", t_csrr_read, "instr", true, true);
    CounterRow("mcycle/cycle (Zicntr)", off.snap.cycle, "cycles", true, true);
    CounterRow("retired class ALU (trace)", t_alu, "instr", false, true);
    CounterRow("retired class LOAD (trace)", t_load, "instr", false, true);
    CounterRow("retired class STORE (trace)", t_store, "instr", false, true);
    CounterRow("retired class CTRL (trace)", t_ctrl, "instr", false, true);
    CounterRow("retired class SYS (trace)", t_sys, "instr", false, true);
    CounterRow("retiring register writes (trace)", t_writes, "writes", false, true);
    CounterRow("o_pmu_uop_insert_ctr", on.snap.uop_insert, "uops", false, false);
    CounterRow("o_pmu_uop_issue_ctr", on.snap.uop_issue, "uops", false, false);
    CounterRow("o_pmu_uop_kill_ctr", on.snap.uop_kill, "uops", false, false);
    CounterRow("o_pmu_dcache_txn_ctr", on.snap.dc_txn, "requests", false, false);
    CounterRow("o_pmu_dcache_hit_ctr", on.snap.dc_hit, "requests", false, false);
    CounterRow("o_pmu_dcache_miss_ctr", on.snap.dc_miss, "requests", false, false);
    CounterRow("o_pmu_dcache_refill_ctr", on.snap.dc_refill, "lines", false, false);
    CounterRow("o_pmu_dcache_wb_ctr", on.snap.dc_wb, "lines", false, false);
    CounterRow("o_pmu_icache_miss_ctr", on.snap.ic_miss, "requests", false, false);
    CounterRow("o_wb_wr_ctr (PRF writes)", on.snap.wb_wr, "writes", false, false);
    CounterRow("o_wb_collision_ctr", on.snap.wb_collision, "conflicts", false, false);
    CounterRow("o_mem_lsu_txn (cache_off)", off.snap.lsu_txn, "txns", false, false);
    CounterRow("o_mem_lq_replay", on.snap.lq_replay, "replays", false, false);
    CounterRow("o_dbg_iq_occ_sum", on.snap.iq_occ_sum, "uop-cycles", false, false);
    CounterRow("o_redirect_ctr", on.snap.redirect, "redirects", false, false);
    std::printf("    (Zihpm 0xC03..0xC1F decode but read zero: a discoverable shadow,\n"
                "     not a mapping of these events.)\n");

    std::printf("\ncontrol observations (no vector IPC vs scalar IPC; no toggle "
                "count as power):\n");
    std::printf("    retired=%llu  WB conflicts=%u  redirects=%u  recover cycles=%u  "
                "replays=%u\n",
                (unsigned long long)on.snap.commit, on.snap.wb_collision,
                on.snap.redirect, on.snap.recovering, on.snap.lq_replay);

    const bool ok = reporter.failures() == 0;
    if (!ok) std::printf("\npmu.trace_accounting: %d check(s) failed\n",
                         reporter.failures());
    return reporter.Finish(ok ? "PASS" : "FAIL",
                           ok ? "counters reconcile with the hand trace"
                              : "counter/trace mismatch");
  } catch (const Failure& f) {
    return reporter.Finish("FAIL", f.what);
  }
}
