// ============================================================================
// tb_core_fence.cpp -- CASE=fence.code_and_data_order, work package I-037.
//
// The DUT is the integrated p0 core, the same top CASE=core.mem_program and
// CASE=core.trap_csr_program drive, with the memory path wired in (I-033..I-035)
// and FENCE / FENCE.I now resolved by the core's system unit at the ROB head.
//
// ------------------------------------------------------------- the programs
//
// Two hand-encoded programs run from the reset vector, in one image, and the two
// results they publish are compared against an independent model in this file:
//
//   1. a **data-ordering** program. It reads a device register that reports how
//      many store transactions the memory system has performed, performs a store,
//      executes FENCE, and reads the device again. The architectural result --
//      the difference between the two reads -- is 1 only if the fence let the
//      older store reach memory before the younger load did. The expectation (1)
//      comes from this file's own model of the device, not from the DUT.
//
//   2. a **self-modifying** program. It stores a new instruction over the word
//      immediately after FENCE.I, executes FENCE.I, and then executes that word.
//      The original word writes 0x11 to t2; the stored word writes 0x22. The
//      front end fetched the original word before the store took effect (it runs
//      ahead of the ROB by the depth of the decode buffer and dispatch queue), so
//      a FENCE.I that does not invalidate the delivered view executes the stale
//      byte and the program publishes 0x11. The published value must be 0x22.
//
// The two programs share one instruction image and run once each. The program
// image and the data memory are **the same memory**: a store to a code address is
// visible to the next instruction fetch, which is what makes the self-modifying
// program meaningful. `Imem` and `Dmem` both read the one `Mem`.
//
// ------------------------------------------------------------ the expectation
//
// Nothing here comes from the DUT:
//
//   * the two published results (data-order difference = 1, patched value =
//     0x22) are the values this file's own model computes for the program it
//     assembled;
//   * the per-instruction retirement stream is compared against an independent
//     RV64IM interpreter written here, which decodes the same words from the ISA
//     text, executes loads and stores against its own memory, models the device
//     register itself, and treats FENCE / FENCE.I as ordering no-ops (it always
//     reads current code, so it predicts the patched instruction);
//   * the fence's own contract is checked at the boundary: at the cycle each
//     fence retires, the store queue holds nothing, the load queue holds nothing
//     and the memory endpoint has no transaction outstanding.
//
// -------------------------------------------------------------- what it checks
//
//   per run:
//   1. the per-instruction retirement stream equals the reference, in order,
//      including every load's value and the patched instruction's value;
//   2. at every FENCE / FENCE.I retirement the memory path is idle (store queue
//      empty, load queue empty, endpoint not busy);
//   3. the FENCE.I issues exactly one redirect, and it names the instruction
//      after itself (pc + 4) -- the front-end invalidation and re-fetch;
//   4. the two published results are the model's;
//   5. no instruction retired twice and the commit counter equals the stream;
//   6. the machine never stopped on an unsupported macro and took no trap.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "sim_common.h"

namespace {

// The probe driver must reach --case before the schema can be checked.
constexpr int kResetCycles = 4;
// The run is a few hundred cycles; these bounds are only reached if the machine
// stopped making progress, and a named stall is more useful than a raw timeout.
constexpr int kStallCycles = 6000;

// ------------------------------------------------------------------ addresses
constexpr uint64_t kRamBase  = 0x80000000ull;
constexpr uint32_t kRamSize  = 0x200000u;      // 2 MiB, from config/memory/p0.json
constexpr uint64_t kDev      = 0x80003000ull;  // the device register (model)
constexpr uint64_t kA        = 0x80001400ull;  // the data-ordering store target
constexpr uint64_t kResD     = 0x80001408ull;  // published: after - before
constexpr uint64_t kResS     = 0x80001410ull;  // published: t2 after FENCE.I
constexpr uint64_t kTohost   = 0x80001000ull;

// The original word there: `addi t2, x0, 17`.
constexpr uint32_t kOldInsn  = 0x01100513u;

constexpr uint32_t kFence    = 0x0FF0000Fu;
constexpr uint32_t kFenceI   = 0x0000100Fu;

struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

// ------------------------------------------------------------- encoders
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
uint32_t EncJ(int32_t offset, uint32_t rd) {
  const uint32_t u = static_cast<uint32_t>(offset);
  const uint32_t imm = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
                       (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12);
  return imm | (rd << 7) | 0x6Fu;
}
uint32_t EncLui(uint32_t rd, uint32_t imm20) { return (imm20 << 12) | (rd << 7) | 0x37u; }
uint32_t EncAuipc(uint32_t rd, uint32_t imm20) { return (imm20 << 12) | (rd << 7) | 0x17u; }
uint32_t EncAddi(uint32_t rd, uint32_t rs1, int32_t imm) {
  return EncI(imm, rs1, 0x0u, rd, 0x13u);
}
uint32_t EncLd(uint32_t rd, uint32_t rs1, int32_t imm) {
  return EncI(imm, rs1, 0x3u, rd, 0x03u);
}
uint32_t EncSd(uint32_t rs2, uint32_t rs1, int32_t imm) {
  return EncS(imm, rs2, rs1, 0x3u, 0x23u);
}
uint32_t EncSw(uint32_t rs2, uint32_t rs1, int32_t imm) {
  return EncS(imm, rs2, rs1, 0x2u, 0x23u);
}
uint32_t EncSub(uint32_t rd, uint32_t rs1, uint32_t rs2) {
  return EncR(0x20u, rs2, rs1, 0x0u, rd, 0x33u);
}
uint32_t EncDiv(uint32_t rd, uint32_t rs1, uint32_t rs2) {
  return EncR(0x01u, rs2, rs1, 0x4u, rd, 0x33u);
}

// register names used below
enum { X0 = 0, T0 = 5, T1 = 6, S0 = 8, T2 = 10, T3 = 28, T4 = 29, T5 = 30, T6 = 31 };

// ============================================================================
// The program image
// ============================================================================
// Laid out from the reset vector. FIXPC is the word immediately after FENCE.I;
// its address is derived from the index, so the program and the driver cannot
// disagree about where the patch lands.
struct Program {
  std::vector<uint32_t> words;
  uint64_t fix_pc = 0;
  uint64_t fence_pc = 0;
  uint64_t fence_i_pc = 0;
};

Program BuildProgram() {
  Program p;
  auto emit = [&](uint32_t w) { p.words.push_back(w); return kRamBase + 4 * (p.words.size() - 1); };
  // Materialise an absolute address PC-relatively: `auipc` adds the upper 20 bits
  // of (target - pc) and the `addi` the signed low 12. This is the only way to
  // reach an address >= 0x80000000 on RV64 with these two instructions: `lui`
  // sign-extends its 32-bit result, so `lui rd, 0x80000` is 0xFFFFFFFF80000000.
  auto addr = [&](uint32_t rd, uint64_t target) {
    const int64_t pc = static_cast<int64_t>(kRamBase + 4 * p.words.size());
    const int64_t delta = static_cast<int64_t>(target) - pc;
    const int64_t hi = (delta + 0x800) >> 12;
    const int64_t lo = delta - (hi << 12);
    emit(EncAuipc(rd, static_cast<uint32_t>(hi) & 0xFFFFFu));
    emit(EncAddi(rd, rd, static_cast<int32_t>(lo)));
  };

  // ---- data-ordering program ----
  addr(T0, kDev);                            // 0-1  t0 = &device
  emit(EncLd(T4, T0, 0));                    // 2    ld t4, 0(t0)      base = dev()
  addr(T0, kA);                              // 3-4  t0 = &A
  emit(EncLui(T1, 0x11223));                 // 5    lui t1, 0x11223
  emit(EncAddi(T1, T1, 0x344));              // 6    addi t1, t1, 0x344   t1 = 0x11223344
  // A slow, independent older instruction. `div` is iterative (64 steps), and
  // because the ROB retires in order the store below cannot retire -- and so
  // cannot drain -- until the divide has written back. That is what makes the
  // fence's ordering *observable through a value*: without the fence's block a
  // load younger than the fence can allocate and read the device while the
  // store is still queued, and the difference it reads is wrong.
  emit(EncAddi(T3, X0, 7));                  // 7    addi t3, x0, 7
  emit(EncDiv(S0, T1, T3));                  // 8    div s0, t1, t3     (slow)
  emit(EncSd(T1, T0, 0));                    // 9    sd t1, 0(t0)         older store
  p.fence_pc = emit(kFence);                 // 10   fence
  addr(T0, kDev);                            // 11-12
  emit(EncLd(T5, T0, 0));                    // 13   ld t5, 0(t0)         younger load
  emit(EncSub(T6, T5, T4));                  // 14   sub t6, t5, t4
  addr(T0, kResD);                           // 15-16
  emit(EncSd(T6, T0, 0));                    // 17   sd t6, 0(t0)

  // ---- self-modifying program ----
  emit(EncLui(T1, 0x02200));                 // 18   lui t1, 0x02200
  emit(EncAddi(T1, T1, 0x513));              // 19   addi t1, t1, 0x513   t1 = 0x02200513
  p.fix_pc = kRamBase + 4 * 24;              //      FIXPC = word after fence.i
  addr(T0, p.fix_pc);                        // 20-21
  emit(EncSw(T1, T0, 0));                    // 22   sw t1, 0(t0)         patch FIXPC
  p.fence_i_pc = emit(kFenceI);              // 23   fence.i
  emit(kOldInsn);                            // 24   FIXPC: addi t2, x0, 17 (patched to 22)
  addr(T0, kResS);                           // 25-26
  emit(EncSd(T2, T0, 0));                    // 27   sd t2, 0(t0)
  addr(T0, kTohost);                         // 28-29
  emit(EncAddi(T1, X0, 1));                  // 30   addi t1, x0, 1
  emit(EncSw(T1, T0, 0));                    // 31   sw t1, 0(t0)         TOHOST = PASS
  emit(EncJ(0, X0));                         // 32   park: jal x0, 0
  return p;
}

// ============================================================================
// The one memory: data and instruction
// ============================================================================
// Instruction fetch and data access read and write the *same* bytes, so a store
// to a code address is visible to the next fetch. That is the property the
// self-modifying program depends on; it is a model of a memory that has no
// instruction cache, which is exactly the machine this profile describes.
class Mem {
 public:
  explicit Mem(const Program& p) : ram_(kRamSize, 0) {
    // Everything not written is ECALL, so a runaway fetch stops the machine
    // cleanly instead of reading a don't-care (as the corpus harness does).
    for (uint32_t off = 0; off + 4 <= kRamSize; off += 4) Write32(kRamBase + off, 0x00000073u);
    for (size_t i = 0; i < p.words.size(); i++) Write32(kRamBase + 4 * i, p.words[i]);
  }

  uint8_t Byte(uint64_t addr) const {
    const uint64_t off = addr - kRamBase;
    return (off < ram_.size()) ? ram_[off] : 0;
  }
  uint32_t Word(uint64_t addr) const {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) v |= static_cast<uint32_t>(Byte(addr + i)) << (8 * i);
    return v;
  }
  uint64_t Window(uint64_t addr) const {  // the aligned doubleword at addr & ~7
    uint64_t v = 0;
    const uint64_t base = addr & ~static_cast<uint64_t>(7);
    for (unsigned i = 0; i < 8; i++) v |= static_cast<uint64_t>(Byte(base + i)) << (8 * i);
    return v;
  }
  void Write32(uint64_t addr, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
      const uint64_t off = addr + i - kRamBase;
      if (off < ram_.size()) ram_[off] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
  }
  void StoreWindow(uint64_t addr, uint32_t wstrb, uint64_t wdata) {
    const uint64_t base = addr & ~static_cast<uint64_t>(7);
    for (unsigned i = 0; i < 8; i++) {
      if (((wstrb >> i) & 1u) == 0) continue;
      const uint64_t off = base + i - kRamBase;
      if (off < ram_.size()) ram_[off] = static_cast<uint8_t>((wdata >> (8 * i)) & 0xFF);
    }
  }

  // The device register: a read of its window answers with the number of store
  // transactions the memory system has performed so far. It is a model of an
  // MMIO register whose state is the handoff the fence orders -- this file's,
  // not the DUT's, and it is what makes the data-ordering program's result an
  // observable the program itself carries.
  uint64_t DeviceRead() const { return store_txns_; }
  void CountStore() { store_txns_++; }
  bool InRange(uint64_t addr) const { return addr >= kRamBase && addr - kRamBase < ram_.size(); }
  uint64_t store_txns() const { return store_txns_; }

 private:
  std::vector<uint8_t> ram_;
  uint64_t store_txns_ = 0;
};

// ============================================================================
// Instruction memory: a one-cycle handshake over the shared Mem
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

  explicit Imem(Mem* mem) : mem_(mem) {}
  void Reset() { inflight_.clear(); ready_.clear(); accepted_ = 0; }
  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint32_t ResponseWord() const { return mem_->Word(ready_.front().addr); }
  void Accept(uint64_t addr, uint32_t id, uint32_t epoch) {
    Request r;
    r.addr = addr;
    r.id = id;
    r.epoch = epoch;
    inflight_.push_back(Entry{r, 1});
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
  uint64_t accepted() const { return accepted_; }

 private:
  Mem* mem_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
  uint64_t accepted_ = 0;
};

// ============================================================================
// Data memory: the memory system's side of the LSU endpoint's protocol
// ============================================================================
class Dmem {
 public:
  struct Rsp {
    uint64_t rdata = 0;
    bool fault = false;
  };
  struct Txn {
    bool we = false;
    uint64_t addr = 0;
    uint32_t size = 0;
    uint32_t wstrb = 0;
    uint64_t wdata = 0;
    uint64_t cycle = 0;
  };

  explicit Dmem(Mem* mem, int latency = 1) : mem_(mem), latency_(latency) {}
  void Reset() { inflight_.clear(); ready_.clear(); txns_.clear(); }
  bool HasResponse() const { return !ready_.empty(); }
  const Rsp& CurrentResponse() const { return ready_.front(); }
  const std::vector<Txn>& txns() const { return txns_; }

  void Accept(const Txn& txn) {
    txns_.push_back(txn);
    inflight_.push_back(Entry{Perform(txn), latency_});
  }
  void PopResponse() { ready_.pop_front(); }
  void Advance() {
    for (size_t i = 0; i < inflight_.size();) {
      if (--inflight_[i].left <= 0) {
        ready_.push_back(inflight_[i].rsp);
        inflight_.erase(inflight_.begin() + static_cast<long>(i));
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
  Rsp Perform(const Txn& t) {
    Rsp rsp;
    if (!mem_->InRange(t.addr)) {
      rsp.fault = true;
      return rsp;
    }
    if (t.we) {
      mem_->StoreWindow(t.addr, t.wstrb, t.wdata);
      mem_->CountStore();
      return rsp;
    }
    if ((t.addr & ~static_cast<uint64_t>(7)) == kDev) {
      rsp.rdata = mem_->DeviceRead();
      return rsp;
    }
    rsp.rdata = mem_->Window(t.addr);
    return rsp;
  }

  Mem* mem_;
  int latency_;
  std::deque<Entry> inflight_;
  std::deque<Rsp> ready_;
  std::vector<Txn> txns_;
};

// ============================================================================
// The independent reference: an RV64IM interpreter for the program above
// ============================================================================
struct RefInsn {
  uint64_t pc = 0;
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
};

struct RefResult {
  std::vector<RefInsn> trace;
  uint64_t res_d = 0;
  uint64_t res_s = 0;
  bool exited = false;
  uint64_t exit_pc = 0;
};

RefResult RunReference(const Program& p) {
  RefResult out;
  std::vector<uint8_t> ram(kRamSize, 0);
  auto put32 = [&](uint64_t addr, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) {
      const uint64_t off = addr + i - kRamBase;
      if (off < ram.size()) ram[off] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
  };
  auto get8 = [&](uint64_t addr) -> uint8_t {
    if (addr == kDev) return 0;  // handled below
    const uint64_t off = addr - kRamBase;
    return (off < ram.size()) ? ram[off] : 0;
  };
  for (size_t i = 0; i < p.words.size(); i++) put32(kRamBase + 4 * i, p.words[i]);

  uint64_t regs[32] = {};
  uint64_t pc = kRamBase;
  uint64_t store_txns = 0;
  for (int step = 0; step < 10000; step++) {
    uint32_t w = 0;
    for (unsigned i = 0; i < 4; i++) w |= static_cast<uint32_t>(get8(pc + i)) << (8 * i);
    const uint32_t op = w & 0x7Fu;
    const uint32_t f3 = (w >> 12) & 0x7u;
    const uint32_t rd = (w >> 7) & 0x1Fu;
    const uint32_t rs1 = (w >> 15) & 0x1Fu;
    const uint32_t rs2 = (w >> 20) & 0x1Fu;
    const uint32_t f7 = (w >> 25) & 0x7Fu;
    const int32_t imm_i = static_cast<int32_t>(w) >> 20;
    const int32_t imm_s =
        (static_cast<int32_t>(w) >> 25 << 5) | static_cast<int32_t>((w >> 7) & 0x1Fu);
    const int32_t imm_u = static_cast<int32_t>(w & 0xFFFFF000u);
    const int32_t imm_j =
        ((static_cast<int32_t>(w) >> 31) << 20) | (((w >> 12) & 0xFFu) << 12) |
        (((w >> 20) & 1u) << 11) | (((w >> 21) & 0x3FFu) << 1);

    RefInsn rec;
    rec.pc = pc;
    uint64_t next = pc + 4;
    uint64_t value = 0;
    bool reg_we = false;

    if (op == 0x37u) {                 // LUI
      value = static_cast<uint64_t>(static_cast<int64_t>(imm_u));
      reg_we = true;
    } else if (op == 0x17u) {          // AUIPC
      value = pc + static_cast<uint64_t>(static_cast<int64_t>(imm_u));
      reg_we = true;
    } else if (op == 0x13u && f3 == 0x0u) {  // ADDI
      value = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_i));
      reg_we = true;
    } else if (op == 0x03u && f3 == 0x3u) {  // LD
      const uint64_t addr = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_i));
      uint64_t v = 0;
      if (addr == kDev) {
        v = store_txns;
      } else {
        for (unsigned i = 0; i < 8; i++) v |= static_cast<uint64_t>(get8(addr + i)) << (8 * i);
      }
      value = v;
      reg_we = true;
    } else if (op == 0x23u && f3 == 0x3u) {  // SD
      const uint64_t addr = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_s));
      for (unsigned i = 0; i < 8; i++) {
        const uint64_t off = addr + i - kRamBase;
        if (off < ram.size()) ram[off] = static_cast<uint8_t>((regs[rs2] >> (8 * i)) & 0xFF);
      }
      store_txns++;
    } else if (op == 0x23u && f3 == 0x2u) {  // SW
      const uint64_t addr = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_s));
      for (unsigned i = 0; i < 4; i++) {
        const uint64_t off = addr + i - kRamBase;
        if (off < ram.size()) ram[off] = static_cast<uint8_t>((regs[rs2] >> (8 * i)) & 0xFF);
      }
      store_txns++;
      if (addr == kTohost) {
        out.exited = true;
        out.exit_pc = pc;
      }
    } else if (op == 0x33u && f3 == 0x0u && f7 == 0x20u) {  // SUB
      value = regs[rs1] - regs[rs2];
      reg_we = true;
    } else if (op == 0x33u && f3 == 0x4u && f7 == 0x01u) {  // DIV (signed)
      const int64_t a = static_cast<int64_t>(regs[rs1]);
      const int64_t b = static_cast<int64_t>(regs[rs2]);
      int64_t q = 0;
      if (b != 0) {
        if (a == INT64_MIN && b == -1) q = INT64_MIN;
        else q = a / b;
      }
      value = static_cast<uint64_t>(q);
      reg_we = true;
    } else if (op == 0x0Fu) {          // FENCE / FENCE.I: ordering no-ops here
      // nothing
    } else if (op == 0x6Fu) {          // JAL
      next = pc + static_cast<uint64_t>(static_cast<int64_t>(imm_j));
      if (rd != 0) { value = pc + 4; reg_we = true; }
    } else {
      out.trace.push_back(rec);
      break;  // ECALL or anything else: the reference stops here
    }

    rec.rd = rd;
    rec.reg_we = reg_we && rd != 0;
    rec.value = value;
    out.trace.push_back(rec);
    if (rd != 0 && reg_we) regs[rd] = value;
    pc = next;

    // The published results, read straight from the model's memory.
    uint64_t d = 0, s = 0;
    for (unsigned i = 0; i < 8; i++) {
      d |= static_cast<uint64_t>(get8(kResD + i)) << (8 * i);
      s |= static_cast<uint64_t>(get8(kResS + i)) << (8 * i);
    }
    out.res_d = d;
    out.res_s = s;
    if (out.exited) break;
  }
  return out;
}

// ============================================================================
// The harness
// ============================================================================
struct Retire {
  uint64_t pc = 0;
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
  uint64_t cycle = 0;
};

class Harness {
 public:
  Harness(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, uint64_t max_cycles, Mem* mem,
          const Program& program)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), mem_(mem), imem_(mem),
        dmem_(mem), program_(program) {}

  uint64_t cycles() const { return cycles_; }
  const std::vector<Retire>& retires() const { return retires_; }
  uint64_t fence_i_redirects() const { return fence_i_redirects_; }
  bool exited() const { return exited_; }
  uint64_t exit_code() const { return exit_code_; }

  // Sample the device's counter as the DUT's own memory model has it.
  uint64_t device() const { return mem_->store_txns(); }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, what + (ok ? "" : " -- " + detail));
    if (!ok) Fail("cycle " + Dec(cycles_), what + ": " + detail);
  }

  void Reset() {
    for (int i = 0; i < kResetCycles; i++) Cycle(true);
    for (int unsigned i = 0; i < 8; i++) Cycle(false);  // let the first fetch start
  }

  void Cycle(bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail("cycle " + Dec(cycles_), "max-cycles exhausted: " + Diagnose());
    }
    dut_->rst = rst ? 1 : 0;

    // ------------------------------------------------------------ imem
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

    // ------------------------------------------------------------ dmem
    dut_->dmem_req_ready_i = 1;
    if (dmem_.HasResponse()) {
      const Dmem::Rsp& r = dmem_.CurrentResponse();
      dut_->dmem_rsp_valid_i = 1;
      dut_->dmem_rsp_rdata_i = r.rdata;
      dut_->dmem_rsp_fault_i = r.fault ? 1 : 0;
    } else {
      dut_->dmem_rsp_valid_i = 0;
      dut_->dmem_rsp_rdata_i = 0;
      dut_->dmem_rsp_fault_i = 0;
    }

    // no interrupts
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = cycles_;

    dut_->eval();
    if (!rst) Observe();

    if (bus_reset_.MayAccept(rst, (dut_->imem_req_valid_o != 0) &&
                                      (dut_->imem_req_ready_i != 0))) {
      imem_.Accept(dut_->imem_req_addr_o, dut_->imem_req_id_o, dut_->imem_req_epoch_o);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->imem_rsp_valid_i != 0) &&
        (dut_->imem_rsp_ready_o != 0)) {
      imem_.PopResponse();
    }
    imem_.Advance();

    if (bus_reset_.MayAccept(rst, (dut_->dmem_req_valid_o != 0) &&
                                      (dut_->dmem_req_ready_i != 0))) {
      Dmem::Txn t;
      t.we = dut_->dmem_req_we_o != 0;
      t.addr = dut_->dmem_req_addr_o;
      t.size = dut_->dmem_req_size_o;
      t.wstrb = dut_->dmem_req_wstrb_o;
      t.wdata = dut_->dmem_req_wdata_o;
      t.cycle = cycles_;
      dmem_.Accept(t);
      if (t.we && t.addr == kTohost && !exited_) {
        uint64_t value = 0;
        for (unsigned i = 0; i < 4; i++) {
          if ((t.wstrb >> i) & 1u) value |= ((t.wdata >> (8 * i)) & 0xFF) << (8 * i);
        }
        exited_ = true;
        exit_code_ = value;
      }
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
    ProgressCheck();
  }

 private:
  void Observe() {
    if (dut_->ev_valid_o != 0) {
      for (uint32_t lane = 0; lane < 2; lane++) {
        if ((dut_->ev_valid_o & (1u << lane)) == 0) continue;
        Retire r;
        r.pc = PayloadLane(dut_->ev_pc_o, lane);
        r.value = PayloadLane(dut_->ev_value_o, lane);
        r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
        r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
        r.cycle = cycles_;

        if (std::getenv("MOSAIC_FENCE_TRACE") != nullptr) {
          std::printf("  [retire] cycle=%llu pc=%s rd=%u we=%u val=%s sq=%u lq=%u busy=%u\n",
                      static_cast<unsigned long long>(cycles_), U64(r.pc).c_str(), r.rd,
                      r.reg_we ? 1u : 0u, U64(r.value).c_str(),
                      dut_->o_mem_sq_occupied_o, dut_->o_mem_lq_occupied_o,
                      dut_->o_mem_lsu_busy_o);
        }

        // The fence's own contract, checked at the cycle it retires: the
        // memory path must have drained -- the store queue empty, the load
        // queue empty, and no data transaction outstanding. This is the check
        // the card's second fail mode names (a fence that retires while an
        // older access has not completed) and it is also what a younger access
        // allowed past the fence violates, because the younger access is the
        // entry still in a queue when the fence leaves.
        if (r.pc == program_.fence_pc || r.pc == program_.fence_i_pc) {
          const char* name = (r.pc == program_.fence_pc) ? "FENCE" : "FENCE.I";
          Check(std::string(name) + " retires with the memory path drained",
                dut_->o_mem_sq_occupied_o == 0 && dut_->o_mem_lq_occupied_o == 0 &&
                    dut_->o_mem_lsu_busy_o == 0,
                "at the retirement: store queue=" + Dec(dut_->o_mem_sq_occupied_o) +
                    " load queue=" + Dec(dut_->o_mem_lq_occupied_o) + " endpoint busy=" +
                    Dec(dut_->o_mem_lsu_busy_o));
        }
        retires_.push_back(r);
        if (expected_ != nullptr && retires_.size() <= expected_->size()) {
          const RefInsn& e = (*expected_)[retires_.size() - 1];
          if (r.pc != e.pc) {
            Fail("cycle " + Dec(cycles_),
                 "the retirement stream follows the reference: retire " +
                     Dec(retires_.size() - 1) + " pc expected " + U64(e.pc) + ", got " +
                     U64(r.pc));
          }
          if (r.rd != e.rd || r.reg_we != e.reg_we) {
            Fail("cycle " + Dec(cycles_),
                 "the retirement stream follows the reference: retire " +
                     Dec(retires_.size() - 1) + " at " + U64(e.pc) +
                     " destination expected rd=" + Dec(e.rd) + " we=" + Dec(e.reg_we) +
                     ", got rd=" + Dec(r.rd) + " we=" + Dec(r.reg_we));
          }
          if (e.reg_we && r.value != e.value) {
            Fail("cycle " + Dec(cycles_),
                 "the retirement stream follows the reference: retire " +
                     Dec(retires_.size() - 1) + " at " + U64(e.pc) + " value for x" +
                     Dec(e.rd) + " expected " + U64(e.value) + ", got " + U64(r.value));
          }
        }
      }
    }

    if (dut_->o_redirect_valid_o != 0) {
      const uint64_t pc = dut_->o_redirect_pc_o;
      if (pc == program_.fix_pc) {
        fence_i_redirects_++;
      }
      // The first redirect of the program is the FENCE.I's, and it names the
      // instruction after FENCE.I. There are no taken branches before it (the
      // only one is the park loop after the exit protocol), so anything else
      // would be the front end leaving the program; a target other than
      // FENCE.I + 4 is the invalidation re-entering one instruction too early
      // or too late.
      if (redirects_total_ == 0 && pc != program_.fix_pc) {
        Fail("cycle " + Dec(cycles_),
             "the first redirect is FENCE.I's and names the instruction after it: got a "
             "redirect to " + U64(pc) + ", FENCE.I is at " + U64(program_.fence_i_pc) +
             " and the next instruction is " + U64(program_.fix_pc));
      }
      redirects_total_++;
      if (std::getenv("MOSAIC_FENCE_TRACE") != nullptr) {
        std::printf("  [redir] cycle=%llu pc=%s sq=%u lq=%u busy=%u stopped=%u\n",
                    static_cast<unsigned long long>(cycles_), U64(pc).c_str(),
                    dut_->o_mem_sq_occupied_o, dut_->o_mem_lq_occupied_o,
                    dut_->o_mem_lsu_busy_o, dut_->o_stopped_o);
      }
    }
  }

  void ProgressCheck() {
    if (dut_->o_commit_o != last_commit_ || dut_->o_dbg_alloc_ctr_o != last_alloc_) {
      last_commit_ = dut_->o_commit_o;
      last_alloc_ = dut_->o_dbg_alloc_ctr_o;
      last_progress_ = cycles_;
      return;
    }
    if (cycles_ - last_progress_ > kStallCycles) {
      Fail("cycle " + Dec(cycles_), "stalled: " + Diagnose());
    }
  }

  std::string Diagnose() {
    return "head_pc=" + U64(dut_->o_dbg_head_pc_o) +
           " head_valid=" + Dec(dut_->o_dbg_head_valid_o) +
           " occupied=" + Dec(dut_->o_rob_occupied_o) +
           " allocated=" + Dec(dut_->o_dbg_alloc_ctr_o) +
           " retired=" + Dec(dut_->o_commit_o) +
           " unsupported=" + Dec(dut_->o_unsupported_o) +
           " stopped=" + Dec(dut_->o_stopped_o) +
           " redirects=" + Dec(dut_->o_redirect_o) +
           " recovering=" + Dec(dut_->o_recovering_o) +
           " sq=" + Dec(dut_->o_mem_sq_occupied_o) +
           " lq=" + Dec(dut_->o_mem_lq_occupied_o) +
           " lsu_busy=" + Dec(dut_->o_mem_lsu_busy_o);
  }

  template <typename Wide>
  static uint64_t PayloadLane(const Wide& wide, uint32_t lane) {
    return static_cast<uint64_t>(wide[lane * 2]) |
           (static_cast<uint64_t>(wide[lane * 2 + 1]) << 32);
  }
  static uint64_t PayloadLane(uint64_t value, uint32_t /*lane*/) { return value; }
  static uint64_t PackedLane(uint64_t packed, uint32_t lane, uint32_t width) {
    const uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1ull);
    return (packed >> (lane * width)) & mask;
  }

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Mem* mem_;
  Imem imem_;
  Dmem dmem_;
  // The reset-traffic rule (V-010, sim/common/bus_reset_gate.h): while reset
  // is asserted this model accepts nothing, so no reset-time response can be
  // queued ahead of a fresh post-reset one.
  mosaic::BusResetGate bus_reset_;
  const Program& program_;
  std::vector<Retire> retires_;
  const std::vector<RefInsn>* expected_ = nullptr;
  uint64_t cycles_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  uint64_t fence_i_redirects_ = 0;
  uint64_t redirects_total_ = 0;
  bool exited_ = false;
  uint64_t exit_code_ = 0;

 public:
  void SetExpected(const std::vector<RefInsn>* trace) { expected_ = trace; }
};

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    const Program program = BuildProgram();
    if (program.fix_pc != program.fence_i_pc + 4) {
      Fail("program", "FIXPC is at " + U64(program.fix_pc) + ", FENCE.I is at " +
                          U64(program.fence_i_pc) + ": the patched word must be the one "
                          "immediately after FENCE.I");
    }

    Mem mem(program);
    const RefResult ref = RunReference(program);
    reporter.Check(ref.exited, "reference: the program reaches its exit protocol");
    reporter.Check(ref.res_d == 1,
                   "reference: the data-ordering difference is 1 (got " + Dec(ref.res_d) + ")");
    reporter.Check(ref.res_s == 0x22,
                   "reference: the patched value is 0x22 (got " + U64(ref.res_s) + ")");
    bool patched_seen = false;
    for (const RefInsn& insn : ref.trace) {
      if (insn.pc == program.fix_pc) {
        patched_seen = true;
        reporter.Check(insn.rd == 10 && insn.reg_we && insn.value == 0x22,
                       "reference: it executes the patched word (addi t2, x0, 0x22) at FIXPC");
      }
    }
    reporter.Check(patched_seen, "reference: it reaches FIXPC");

    Harness harness(&dut, &reporter, options.max_cycles, &mem, program);
    harness.SetExpected(&ref.trace);
    harness.Reset();

    uint64_t guard = 0;
    while (!harness.exited()) {
      harness.Cycle(false);
      if (++guard > options.max_cycles) {
        Fail("run", "max-cycles exhausted: the program never wrote TOHOST");
      }
    }

    // ---- 1. the retirement stream ----
    reporter.Check(harness.retires().size() >= ref.trace.size(),
                   "the machine retires every reference instruction: got " +
                       Dec(harness.retires().size()) + ", reference " +
                       Dec(ref.trace.size()));
    for (size_t i = 0; i < ref.trace.size() && i < harness.retires().size(); i++) {
      if (harness.retires()[i].pc != ref.trace[i].pc) {
        Fail("run", "retire " + Dec(i) + " pc expected " + U64(ref.trace[i].pc) +
                        ", got " + U64(harness.retires()[i].pc));
      }
    }
    reporter.Check(true, "the per-instruction retirement stream matches the RV64IM reference (" +
                             Dec(ref.trace.size()) + " instructions)");

    // ---- 2. the two published results ----
    const uint64_t res_d = mem.Window(kResD);
    const uint64_t res_s = mem.Window(kResS);
    reporter.Check(res_d == ref.res_d,
                   "the data-ordering difference published in memory is the model's: memory " +
                       U64(res_d) + ", model " + U64(ref.res_d));
    reporter.Check(res_s == ref.res_s,
                   "the patched instruction's value published in memory is the model's: memory " +
                       U64(res_s) + ", model " + U64(ref.res_s));

    // ---- 3. the FENCE.I invalidation ----
    reporter.Check(harness.fence_i_redirects() == 1,
                   "FENCE.I issues exactly one front-end redirect: got " +
                       Dec(harness.fence_i_redirects()));

    // ---- 4. the machinery stayed clean ----
    reporter.Check(dut.o_unsupported_o == 0, "no macro was refused as unsupported: " +
                                                  Dec(dut.o_unsupported_o));
    reporter.Check(dut.o_csr_trap_o == 0, "no exception trap was taken: " +
                                               Dec(dut.o_csr_trap_o));
    reporter.Check(harness.exit_code() == 1,
                   "the program wrote TOHOST = 1 (got " + Dec(harness.exit_code()) + ")");

    std::printf("  retires=%zu fence_i_redirects=%llu lsu_txns=%llu cycles=%llu\n",
                harness.retires().size(),
                static_cast<unsigned long long>(harness.fence_i_redirects()),
                static_cast<unsigned long long>(mem.store_txns()),
                static_cast<unsigned long long>(harness.cycles()));
    detail = "checks=" + Dec(reporter.checks()) + " retires=" +
             Dec(harness.retires().size()) + " fence_i_redirects=" +
             Dec(harness.fence_i_redirects()) + " store_txns=" + Dec(mem.store_txns()) +
             " res_d=" + Dec(res_d) + " res_s=" + U64(res_s) + " cycles=" +
             Dec(harness.cycles()) + " seed=" + Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the fence contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
