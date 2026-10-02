// ============================================================================
// tb_core_mmio.cpp -- CASE=mmio.exactly_once, work package I-038.
//
// The DUT is the integrated p0 core, the same top CASE=core.mem_program,
// CASE=fence.code_and_data_order and CASE=core.trap_csr_program drive, with the
// memory path wired in (I-033..I-035) and the device serializer (I-038) between
// the queue request mux and mosaic_lsu_endpoint.
//
// ------------------------------------------------------------- the two runs
//
// Two hand-encoded programs run from the reset vector, each after a full reset
// of the same DUT, each with its own memory image and its own device model.
//
//   Run A -- the correct path. Two FIFO reads (each one pop) and two UART
//   writes, every popped value published so the numbers encode the pop order;
//   then a load from the error device, which must raise a load access fault with
//   the faulting PC in `mepc`, cause 5 in `mcause` and the address in `mtval`.
//   The handler publishes all three and ends the run; the fall-through (taken
//   only if the access does *not* trap) publishes a marker instead, so "the
//   error was swallowed" is a named failure and not a hang.
//
//   Run B -- the wrong path. An iterative `div` keeps the ROB head busy for
//   sixty-odd cycles while a trapping `ecall` sits behind it and two loads are
//   dispatched behind the `ecall`: an ordinary RAM load first, then a FIFO read.
//   Nothing here waits on a branch -- this profile's dispatch barrier stops
//   allocation behind an unresolved branch, so a branch could not create the
//   window -- but a trap is not a branch: the younger loads are dispatched and
//   the RAM load, being the load queue's head, is issued and performed before
//   the `div` retires and the `ecall` traps and flushes both. That is the
//   harness's proof that the window is real. The FIFO read behind it is the
//   wrong-path *device* access: it must not be presented to the memory system,
//   because a device access is presented only when it is the ROB head.
//
// ------------------------------------------------------------ the device model
//
// The device lives in this file, on the memory system's side of the data port:
//
//   * `kFifo` (uart region)     -- a read pops a queue and returns the value, so
//       a read is a side effect and the value identifies which pop it was;
//   * `kUart` (uart region)     -- a write appends a byte and is counted;
//   * `kErr`  (clint region)    -- any access is answered with `fault`: the
//       error response the core must trap on.
//
// The model is addressed by *transaction*, not by byte: one accepted data-port
// access is one side effect whatever its width. That is the granularity the rule
// is stated at, and why a duplicated transaction shows up as a second pop rather
// than as eight byte reads.
//
// The device classification is the profile's own, from config/memory/p0.json:
// the three regions that are not idempotent (uart, test_harness, clint).
// `IsDevice` below writes that map out here, so the expectation does not come
// from the DUT.
//
// -------------------------------------------------------------- what it checks
//
//   per run:
//   1. the retirement stream equals the reference, in order, including every
//      loaded value and every value the program publishes in memory;
//   2. every access the core presents to the memory system carries the device
//      attribute its address's region demands (`o_mem_dmem_dev_o`) -- the
//      attribute a coalescer keys on when it refuses to merge MMIO with RAM;
//   3. every device transaction has a distinct identity (`o_mem_dmem_id_o`), so
//      no access was presented twice;
//   4. the device model performed exactly the accesses the reference's program
//      performs -- no more (a speculative device access) and no fewer (a dropped
//      one);
//   5. the core's device/RAM classification counters partition the data-port
//      transactions and agree with the memory system's address-based counts;
//   6. any trap is precise: exactly one trap event, its PC is the trapping
//      instruction's, and the published mepc/mcause/mtval are the reference's;
//   7. the program reached its exit protocol (TOHOST = 1) and no macro was
//      refused as unsupported;
//   run B additionally:
//   8. the wrong-path ordinary load reached memory (the window is real) and the
//      wrong-path device load did not, and the serializer refused the device
//      offer until the redirect (`o_mem_dev_wait_o`).
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
// The memory's response latency. More than one cycle on purpose: the memory
// system is a slave that does not answer in the cycle it is asked, and the
// serializer holds a device transaction across that window, so the run exercises
// the hold under endpoint backpressure instead of always accepting on the cycle
// after the take.
constexpr int kMemLatency = 4;
// The runs are a few hundred cycles (the divides alone are ~64); these bounds
// are only reached if the machine stopped making progress.
constexpr int kStallCycles = 8000;

// ------------------------------------------------------- the profile's map
// From config/memory/p0.json through the generated mosaic_cfg_pkg: the p0
// physical map this profile froze.
constexpr uint64_t kRamBase     = 0x80000000ull;
constexpr uint64_t kRamSize     = 0x200000ull;   // 2 MiB
constexpr uint64_t kUartBase    = 0x100000ull;
constexpr uint64_t kUartSize    = 0x100ull;
constexpr uint64_t kHarnessBase = 0x102000ull;
constexpr uint64_t kHarnessSize = 0x10ull;
constexpr uint64_t kClintBase   = 0x2000000ull;  // 33554432
constexpr uint64_t kClintSize   = 0x1000ull;

// The three registers this case uses.
constexpr uint64_t kFifo = kUartBase;            // a read pops
constexpr uint64_t kUart = kUartBase + 8;        // a write appends
constexpr uint64_t kErr  = kClintBase;           // any access errors

// Where the programs publish, the wrong-path RAM target and the exit address.
constexpr uint64_t kSig     = 0x80001400ull;
constexpr uint64_t kSpecRam = 0x80002000ull;
constexpr uint64_t kTohost  = 0x80001000ull;

// The map's device predicate, written out: the regions that are not idempotent.
bool IsDevice(uint64_t addr) {
  return (addr >= kUartBase && addr - kUartBase < kUartSize) ||
         (addr >= kHarnessBase && addr - kHarnessBase < kHarnessSize) ||
         (addr >= kClintBase && addr - kClintBase < kClintSize);
}

// The FIFO's contents. Distinct values, so a duplicated or dropped pop moves the
// numbers the programs publish.
constexpr uint64_t kFifoValues[4] = {
    0x1111111111111111ull, 0x2222222222222222ull,
    0x3333333333333333ull, 0x4444444444444444ull};

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
uint32_t EncDiv(uint32_t rd, uint32_t rs1, uint32_t rs2) {
  return EncR(0x01u, rs2, rs1, 0x4u, rd, 0x33u);
}
uint32_t EncCsrr(uint32_t rd, uint32_t csr) {   // csrrs rd, csr, x0
  return EncI(static_cast<int32_t>(csr), 0u, 0x2u, rd, 0x73u);
}
uint32_t EncCsrw(uint32_t csr, uint32_t rs1) {  // csrrw x0, csr, rs1
  return EncI(static_cast<int32_t>(csr), rs1, 0x1u, 0u, 0x73u);
}
uint32_t EncEcall() { return 0x00000073u; }

// Register names used below.
enum { X0 = 0, T0 = 5, T1 = 6, T2 = 7, S0 = 8, T3 = 28, T4 = 29, T5 = 30, T6 = 31 };
enum { CSR_MTVEC = 0x305, CSR_MEPC = 0x341, CSR_MCAUSE = 0x342, CSR_MTVAL = 0x343 };

// Exception codes, from mosaic_pkg.
constexpr uint64_t kExcLoadAccess = 5;
constexpr uint64_t kExcEcallM = 11;

// ============================================================================
// The program images
// ============================================================================
struct Program {
  std::vector<uint32_t> words;
  uint64_t wrong_ram_pc = 0;   // run B: the wrong-path ordinary load
  uint64_t wrong_dev_pc = 0;   // run B: the wrong-path FIFO read
  uint64_t bad_pc       = 0;   // run A: the load from the error device
  uint64_t ecall_pc     = 0;   // run B: the trapping ecall
  uint64_t no_trap_pc   = 0;   // run A: the fall-through taken only without a trap
  uint64_t handler_pc   = 0;   // where mtvec points
};

// `addr` materialises an absolute address PC-relatively: auipc carries the upper
// twenty bits of (target - pc) and the addi the signed low twelve. It is the only
// way to reach an address >= 0x80000000 with two instructions, because `lui`
// sign-extends its 32-bit result.
struct Builder {
  Program p;
  size_t handler_addr_at = 0;

  uint64_t emit(uint32_t w) {
    p.words.push_back(w);
    return kRamBase + 4 * (p.words.size() - 1);
  }
  void addr(uint32_t rd, uint64_t target) {
    const size_t at = p.words.size();
    const int64_t pc = static_cast<int64_t>(kRamBase + 4 * at);
    const int64_t delta = static_cast<int64_t>(target) - pc;
    const int64_t hi = (delta + 0x800) >> 12;
    const int64_t lo = delta - (hi << 12);
    emit(EncAuipc(rd, static_cast<uint32_t>(hi) & 0xFFFFFu));
    emit(EncAddi(rd, rd, static_cast<int32_t>(lo)));
  }
  // The same pair with the target patched later: the handler is emitted after
  // the body that names it.
  void addr_handler(uint32_t rd) {
    handler_addr_at = p.words.size();
    emit(EncAuipc(rd, 0));
    emit(EncAddi(rd, rd, 0));
  }
  void fixup_handler(uint32_t rd) {
    const int64_t pc = static_cast<int64_t>(kRamBase + 4 * handler_addr_at);
    const int64_t delta = static_cast<int64_t>(p.handler_pc) - pc;
    const int64_t hi = (delta + 0x800) >> 12;
    const int64_t lo = delta - (hi << 12);
    p.words[handler_addr_at] = EncAuipc(rd, static_cast<uint32_t>(hi) & 0xFFFFFu);
    p.words[handler_addr_at + 1] = EncAddi(rd, rd, static_cast<int32_t>(lo));
  }
};

// ---- run A: the correct path, and the error device's trap ----
Program BuildCorrectProgram() {
  Builder b;
  b.addr(T0, kFifo);
  b.emit(EncLd(T3, T0, 0));               // FIFO pop #1
  b.addr(T4, kUart);
  b.emit(EncSd(T3, T4, 0));               // UART write #1 = pop #1
  b.addr(T4, kSig);
  b.emit(EncSd(T3, T4, 0));               // publish pop #1
  b.addr(T0, kFifo);
  b.emit(EncLd(T3, T0, 0));               // FIFO pop #2
  b.emit(EncSd(T3, T4, 8));               // publish pop #2
  b.addr(T0, kUart);
  b.emit(EncSd(T3, T0, 0));               // UART write #2 = pop #2
  b.addr_handler(T0);                     // t0 = &handler (patched below)
  b.emit(EncCsrw(CSR_MTVEC, T0));
  b.addr(T0, kErr);
  b.p.bad_pc = b.emit(EncLd(T1, T0, 0));  // the error load: must trap
  // The fall-through, taken only when the access does *not* trap.
  b.p.no_trap_pc = b.emit(EncAddi(T1, X0, 0x777));  // the "no trap" marker
  b.addr(T0, kSig + 16);
  b.emit(EncSd(T1, T0, 0));
  b.addr(T0, kTohost);
  b.emit(EncAddi(T1, X0, 1));
  b.emit(EncSw(T1, T0, 0));               // TOHOST = PASS
  b.emit(EncJ(0, X0));                    // park
  // The handler.
  b.p.handler_pc = kRamBase + 4 * b.p.words.size();
  b.emit(EncCsrr(T1, CSR_MEPC));
  b.emit(EncCsrr(T2, CSR_MCAUSE));
  b.emit(EncCsrr(T3, CSR_MTVAL));
  b.addr(T4, kSig + 16);
  b.emit(EncSd(T1, T4, 0));               // publish mepc
  b.emit(EncSd(T2, T4, 8));               // publish mcause
  b.emit(EncSd(T3, T4, 16));              // publish mtval
  b.addr(T0, kTohost);
  b.emit(EncAddi(T1, X0, 1));
  b.emit(EncSw(T1, T0, 0));               // TOHOST = PASS
  b.emit(EncJ(0, X0));                    // park
  b.fixup_handler(T0);
  return b.p;
}

// ---- run B: the wrong path, squashed by a trap rather than by a branch ----
Program BuildWrongPathProgram() {
  Builder b;
  b.addr(T3, kFifo);
  b.emit(EncLd(T1, T3, 0));               // the one legitimate FIFO pop
  b.addr(T4, kSpecRam);                   // the wrong-path RAM target
  b.addr_handler(T0);                     // t0 = &handler (patched below)
  b.emit(EncCsrw(CSR_MTVEC, T0));         // arm the vector before the ecall
  // An ECALL is refused at dispatch until the write to mtvec has *retired*
  // (mosaic_dispatch's `l0_trap_unarmed`), and the front end runs ahead, so the
  // program spends sixteen instructions -- more than the front end's window --
  // after the install. This is the idiom CASE=core.trap_csr_program documents.
  for (int i = 0; i < 16; i++) b.emit(EncAddi(0, 0, 0));
  b.emit(EncLui(T6, 0x10000));            // t6 = 0x10000000
  b.emit(EncAddi(T6, T6, 1));             // t6 = 0x10000001
  b.emit(EncAddi(T5, X0, 3));             // t5 = 3
  b.emit(EncDiv(S0, T6, T5));             // slow: keeps the ecall from the head
  b.p.ecall_pc = b.emit(EncEcall());      // traps once it reaches the head
  b.p.wrong_ram_pc = b.emit(EncLd(T1, T4, 0));  // WRONG PATH: an ordinary load
  b.p.wrong_dev_pc = b.emit(EncLd(T2, T3, 0));  // WRONG PATH: the FIFO read
  b.emit(EncJ(0, X0));                    // park
  // The handler: publish the trap and end the run.
  b.p.handler_pc = kRamBase + 4 * b.p.words.size();
  b.emit(EncCsrr(T1, CSR_MEPC));
  b.emit(EncCsrr(T2, CSR_MCAUSE));
  b.addr(T4, kSig);
  b.emit(EncSd(T1, T4, 0));               // publish mepc
  b.emit(EncSd(T2, T4, 8));               // publish mcause
  b.addr(T0, kTohost);
  b.emit(EncAddi(T1, X0, 1));
  b.emit(EncSw(T1, T0, 0));               // TOHOST = PASS
  b.emit(EncJ(0, X0));                    // park
  b.fixup_handler(T0);
  return b.p;
}

// ============================================================================
// The one memory: data and instruction
// ============================================================================
class Mem {
 public:
  explicit Mem(const Program& p) : ram_(kRamSize, 0) {
    // Everything not written is ECALL, so a runaway fetch stops the machine
    // cleanly instead of reading a don't-care.
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
  uint64_t Window(uint64_t addr) const {
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
  bool InRange(uint64_t addr) const { return addr >= kRamBase && addr - kRamBase < ram_.size(); }

 private:
  std::vector<uint8_t> ram_;
};

// ============================================================================
// The device: one side effect per accepted transaction
// ============================================================================
class Device {
 public:
  uint64_t Pop() {
    pops_++;
    const uint64_t value = (fifo_at_ < 4) ? kFifoValues[fifo_at_] : 0;
    fifo_at_++;
    return value;
  }
  void Push(uint64_t value) {
    writes_++;
    uart_.push_back(static_cast<char>(value & 0xff));
  }
  void Error() { errors_++; }

  uint64_t pops() const { return pops_; }
  uint64_t writes() const { return writes_; }
  uint64_t errors() const { return errors_; }
  const std::string& uart() const { return uart_; }

 private:
  uint64_t pops_ = 0;
  uint64_t writes_ = 0;
  uint64_t errors_ = 0;
  uint64_t fifo_at_ = 0;
  std::string uart_;
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
  void Reset() { inflight_.clear(); ready_.clear(); }
  bool HasResponse() const { return !ready_.empty(); }
  const Request& Response() const { return ready_.front(); }
  uint32_t ResponseWord() const { return mem_->Word(ready_.front().addr); }
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
  Mem* mem_;
  std::deque<Entry> inflight_;
  std::deque<Request> ready_;
};

// ============================================================================
// Data memory: the memory system's side of the LSU endpoint's protocol, with
// the device model attached
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
    bool dev = false;          // the attribute the core presented
    uint64_t id = 0;           // the identity the core presented
    uint64_t cycle = 0;
  };

  Dmem(Mem* mem, Device* dev, int latency = kMemLatency)
      : mem_(mem), dev_(dev), latency_(latency < 0 ? 0 : latency) {}

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

  // What the memory system saw, for the address-based counts the case compares
  // the core's classification against.
  uint64_t device_txns() const {
    uint64_t n = 0;
    for (const Txn& t : txns_) if (IsDevice(t.addr)) n++;
    return n;
  }
  uint64_t ram_txns() const {
    uint64_t n = 0;
    for (const Txn& t : txns_) if (!IsDevice(t.addr)) n++;
    return n;
  }

 private:
  struct Entry {
    Rsp rsp;
    int left = 0;
  };

  Rsp Perform(const Txn& t) {
    Rsp rsp;
    // The device registers are eight-byte aligned, so the register's window is
    // the access's window and one transaction is one side effect.
    if (t.addr == kFifo && !t.we) {
      rsp.rdata = dev_->Pop();
      return rsp;
    }
    if (t.addr == kFifo || t.addr == kUart) {
      if (t.we) dev_->Push(t.wdata & 0xff);
      return rsp;
    }
    if (t.addr == kErr) {
      dev_->Error();
      rsp.fault = true;
      return rsp;
    }
    if (!mem_->InRange(t.addr)) {
      rsp.fault = true;
      return rsp;
    }
    if (t.we) {
      mem_->StoreWindow(t.addr, t.wstrb, t.wdata);
      return rsp;
    }
    rsp.rdata = mem_->Window(t.addr & ~static_cast<uint64_t>(7));
    return rsp;
  }

  Mem* mem_;
  Device* dev_;
  int latency_;
  std::deque<Entry> inflight_;
  std::deque<Rsp> ready_;
  std::vector<Txn> txns_;
};

// ============================================================================
// The independent reference: an RV64IM interpreter for the programs above
// ============================================================================
struct RefInsn {
  uint64_t pc = 0;
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
};

struct RefResult {
  std::vector<RefInsn> trace;     // retirements only: the trapping instruction is not one
  uint64_t trap_pc = 0;
  uint64_t trap_cause = 0;
  uint64_t trap_tval = 0;
  uint64_t sig[4] = {0, 0, 0, 0};
  uint64_t pops = 0;
  uint64_t writes = 0;
  uint64_t errors = 0;
  uint64_t ram_accesses = 0;
  bool exited = false;
  uint64_t exit_code = 0;
};

RefResult RunReference(const Program& p) {
  RefResult out;
  Mem mem(p);
  Device dev;
  uint64_t regs[32] = {};
  uint64_t csr_mtvec = 0, csr_mepc = 0, csr_mcause = 0, csr_mtval = 0;
  uint64_t pc = kRamBase;

  for (int step = 0; step < 20000; step++) {
    const uint32_t w = mem.Word(pc);
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
    bool trapped = false;
    // A branch's or a store's immediate occupies the encoding's rd field, so
    // the architectural destination is written down here rather than read out of
    // a bit position that means something else for that opcode.
    uint32_t dst_rd = rd;

    if (op == 0x37u) {                                   // LUI
      value = static_cast<uint64_t>(static_cast<int64_t>(imm_u));
      reg_we = true;
    } else if (op == 0x17u) {                            // AUIPC
      value = pc + static_cast<uint64_t>(static_cast<int64_t>(imm_u));
      reg_we = true;
    } else if (op == 0x13u && f3 == 0x0u) {              // ADDI
      value = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_i));
      reg_we = true;
    } else if (op == 0x03u && f3 == 0x3u) {              // LD
      const uint64_t addr = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_i));
      if (addr == kFifo) {
        value = dev.Pop();
        out.pops++;
      } else if (addr == kErr) {
        out.errors++;
        out.trap_pc = pc;
        out.trap_cause = kExcLoadAccess;
        out.trap_tval = addr;
        trapped = true;
        next = csr_mtvec;
      } else {
        uint64_t v = 0;
        for (unsigned i = 0; i < 8; i++) {
          v |= static_cast<uint64_t>(mem.Byte(addr + i)) << (8 * i);
        }
        value = v;
        out.ram_accesses++;
      }
      if (!trapped) reg_we = true;
    } else if (op == 0x23u && f3 == 0x3u) {              // SD
      dst_rd = 0;
      const uint64_t addr = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_s));
      if (addr == kFifo || addr == kUart) {
        dev.Push(regs[rs2] & 0xff);
        out.writes++;
      } else if (addr == kErr) {
        out.errors++;
        out.trap_pc = pc;
        out.trap_cause = 7;
        out.trap_tval = addr;
        trapped = true;
        next = csr_mtvec;
      } else {
        mem.StoreWindow(addr, 0xFFu, regs[rs2]);
        out.ram_accesses++;
      }
    } else if (op == 0x23u && f3 == 0x2u) {              // SW
      dst_rd = 0;
      const uint64_t addr = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_s));
      if (addr == kFifo || addr == kUart) {
        dev.Push(regs[rs2] & 0xff);
        out.writes++;
      } else if (addr == kErr) {
        out.errors++;
        out.trap_pc = pc;
        out.trap_cause = 7;
        out.trap_tval = addr;
        trapped = true;
        next = csr_mtvec;
      } else {
        mem.StoreWindow(addr, 0x0Fu, regs[rs2]);
        out.ram_accesses++;
        if (addr == kTohost) {
          out.exited = true;
          out.exit_code = regs[rs2] & 0xFFFFFFFFull;
        }
      }
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
    } else if (op == 0x6Fu) {                            // JAL
      next = pc + static_cast<uint64_t>(static_cast<int64_t>(imm_j));
      if (rd != 0) {
        value = pc + 4;
        reg_we = true;
      }
    } else if (op == 0x73u && f3 == 0x0u) {              // ECALL
      // The privileged-architecture trap: mepc is the ecall's own PC and the
      // handler is reached through mtvec.
      out.trap_pc = pc;
      out.trap_cause = kExcEcallM;
      out.trap_tval = 0;
      trapped = true;
      next = csr_mtvec;
    } else if (op == 0x73u && (f3 == 0x1u || f3 == 0x2u)) {  // CSRRW / CSRRS
      const uint32_t csr = (w >> 20) & 0xFFFu;
      uint64_t old = 0;
      switch (csr) {
        case CSR_MTVEC: old = csr_mtvec; break;
        case CSR_MEPC: old = csr_mepc; break;
        case CSR_MCAUSE: old = csr_mcause; break;
        case CSR_MTVAL: old = csr_mtval; break;
        default: break;
      }
      if (f3 == 0x1u) {                                  // csrrw: write, read old
        if (csr == CSR_MTVEC) csr_mtvec = regs[rs1];
        value = old;
        reg_we = (rd != 0);
      } else {                                           // csrrs: the program only reads
        if (rs1 != 0 && csr == CSR_MTVEC) csr_mtvec |= regs[rs1];
        value = old;
        reg_we = (rd != 0);
      }
    } else {
      break;                                             // anything else: stop
    }

    if (trapped) {
      // The trapping instruction does not retire: the trap redirects instead and
      // writes mepc/mcause/mtval the way the CSR file must. The handler then
      // runs, and its csrr reads are compared against values this model
      // computed rather than values it was told.
      csr_mepc = out.trap_pc;
      csr_mcause = out.trap_cause;
      csr_mtval = out.trap_tval;
      pc = next;
      continue;
    }

    rec.rd = dst_rd;
    rec.reg_we = reg_we && dst_rd != 0;
    rec.value = value;
    out.trace.push_back(rec);
    if (dst_rd != 0 && reg_we) regs[dst_rd] = value;
    pc = next;

    for (int i = 0; i < 4; i++) out.sig[i] = mem.Window(kSig + 8 * i);
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
          Device* dev)
      : dut_(dut), reporter_(reporter), max_cycles_(max_cycles), imem_(mem),
        dmem_(mem, dev) {}

  // Instructions that must never retire. The correct-path program's faulting
  // load is one -- it has to trap -- and its no-trap fall-through is another: the
  // fall-through exists only to publish a marker if the error device's response
  // is swallowed. Retiring either one *is* the defect, and naming it here is
  // more useful than a retirement-stream divergence one instruction later.
  void SetForbiddenPcs(const std::vector<uint64_t>& pcs) { forbidden_pcs_ = pcs; }

  uint64_t cycles() const { return cycles_; }
  const std::vector<Retire>& retires() const { return retires_; }
  const std::vector<Dmem::Txn>& txns() const { return dmem_.txns(); }
  uint64_t device_txns() const { return dmem_.device_txns(); }
  uint64_t ram_txns() const { return dmem_.ram_txns(); }
  const std::vector<uint64_t>& device_ids() const { return device_ids_; }
  uint64_t trap_events() const { return trap_events_; }
  uint64_t trap_event_pc() const { return trap_event_pc_; }
  bool exited() const { return exited_; }
  uint64_t exit_code() const { return exit_code_; }

  void Reset() {
    imem_.Reset();
    dmem_.Reset();
    for (int i = 0; i < kResetCycles; i++) Cycle(true);
    for (int unsigned i = 0; i < 8; i++) Cycle(false);
  }

  void Run() {
    uint64_t guard = 0;
    while (!exited()) {
      Cycle(false);
      if (++guard > max_cycles_) {
        Fail("run", "max-cycles exhausted: the program never wrote TOHOST");
      }
    }
  }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, what + (ok ? "" : " -- " + detail));
    if (!ok) Fail("cycle " + Dec(cycles_), what + ": " + detail);
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
    // MOSAIC_MMIO_CYCLES prints the memory-path state every cycle, which is what
    // the serializer's hold and the response owner were debugged with; it is
    // separate from MOSAIC_MMIO_TRACE so a whole-run trace can stay short.
    if (rst == 0 && std::getenv("MOSAIC_MMIO_CYCLES") != nullptr) {
      const uint32_t d = static_cast<uint32_t>(dut_->o_dbg_mmio_o);
      std::printf("  [cyc %llu] lq_rsp_valid=%u sq_rsp_valid=%u ser_out=%u ser_hold=%u "
                  "ep_busy=%u ep_rsp_valid=%u ep_owner=%u sq_drain=%u lq_req=%u "
                  "lq_done=%u sq_occ=%u lq_occ=%u\n",
                  static_cast<unsigned long long>(cycles_), (d >> 31) & 1u, (d >> 30) & 1u,
                  (d >> 29) & 1u, (d >> 28) & 1u, (d >> 27) & 1u, (d >> 26) & 1u,
                  (d >> 25) & 1u, (d >> 24) & 1u, (d >> 23) & 1u, d & 0x7FFFFFu,
                  dut_->o_mem_sq_occupied_o, dut_->o_mem_lq_occupied_o);
    }
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
      t.dev = dut_->o_mem_dmem_dev_o != 0;
      t.id = static_cast<uint64_t>(dut_->o_mem_dmem_id_o);
      t.cycle = cycles_;

      // The card's boundary rule, checked on every transaction: the attribute
      // the core presents must be the map's classification of the address. This
      // is the signal a coalescer keys on when it refuses to merge an MMIO
      // access with an ordinary one.
      const bool want_dev = IsDevice(t.addr);
      Check("every access carries the device attribute its region demands",
            t.dev == want_dev,
            "cycle " + Dec(cycles_) + ": access to " + U64(t.addr) + " (" +
                (want_dev ? "a device region" : "ordinary memory") +
                ") presented with device attribute " + Dec(t.dev ? 1 : 0));
      if (t.dev) {
        if (std::getenv("MOSAIC_MMIO_TRACE") != nullptr) {
          std::printf("  [dev] cycle=%llu addr=%s we=%u id=%llu\n",
                      static_cast<unsigned long long>(cycles_), U64(t.addr).c_str(),
                      t.we ? 1u : 0u, static_cast<unsigned long long>(t.id));
        }
        // The identity discipline, checked the moment a second transaction
        // claims an identity an earlier one already used: that is "one side
        // effect per transaction identity", and it fires here rather than only
        // at the end of the run, so a duplicate is named even if the duplicate
        // also wedges the machine.
        for (size_t i = 0; i < device_ids_.size(); i++) {
          if (device_ids_[i] == t.id) {
            Check("every device transaction has a distinct identity", false,
                  "cycle " + Dec(cycles_) + ": identity " + Dec(t.id) +
                      " appears at device transactions " + Dec(i) + " and " +
                      Dec(device_ids_.size()));
          }
        }
        device_ids_.push_back(t.id);
      }
      if (t.we && t.addr == kTohost && !exited_) {
        uint64_t value = 0;
        for (unsigned i = 0; i < 4; i++) {
          if ((t.wstrb >> i) & 1u) value |= ((t.wdata >> (8 * i)) & 0xFF) << (8 * i);
        }
        exited_ = true;
        exit_code_ = value;
      }
      if (std::getenv("MOSAIC_MMIO_TRACE") != nullptr) {
        std::printf("  [mem] cycle=%llu accept addr=%s we=%u lat=%d outstanding=%zu\n",
                    static_cast<unsigned long long>(cycles_), U64(t.addr).c_str(),
                    t.we ? 1u : 0u, kMemLatency, dmem_.txns().size());
      }
      dmem_.Accept(t);
    }
    if (bus_reset_.MayDeliver(rst) && (dut_->dmem_rsp_valid_i != 0) &&
        (dut_->dmem_rsp_ready_o != 0)) {
      if (std::getenv("MOSAIC_MMIO_TRACE") != nullptr) {
        std::printf("  [mem] cycle=%llu rsp consumed rdata=%s\n",
                    static_cast<unsigned long long>(cycles_),
                    U64(dmem_.CurrentResponse().rdata).c_str());
      }
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
    const uint32_t ev_mask = static_cast<uint32_t>(dut_->ev_valid_o);
    if (ev_mask == 0) return;
    for (uint32_t lane = 0; lane < 2; lane++) {
      if ((ev_mask & (1u << lane)) == 0) continue;
      const bool is_trap = PackedLane(dut_->ev_trap_o, lane, 1) != 0;
      if (is_trap) {
        // A trap event: the trapping instruction does not retire, and the
        // reference's trace excludes it. Its PC is checked.
        trap_events_++;
        trap_event_pc_ = PayloadLane(dut_->ev_pc_o, lane);
        if (std::getenv("MOSAIC_MMIO_TRACE") != nullptr) {
          std::printf("  [trap] cycle=%llu pc=%s cause=%s\n",
                      static_cast<unsigned long long>(cycles_),
                      U64(trap_event_pc_).c_str(),
                      U64(PayloadLane(dut_->ev_trap_cause_o, lane)).c_str());
        }
        continue;
      }
      Retire r;
      r.pc = PayloadLane(dut_->ev_pc_o, lane);
      for (uint64_t forbidden : forbidden_pcs_) {
        if (r.pc == forbidden) {
          Fail("cycle " + Dec(cycles_),
               "the error device's response did not trap: the instruction at " +
                   U64(r.pc) + " retires instead of trapping");
        }
      }
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.cycle = cycles_;
      if (std::getenv("MOSAIC_MMIO_TRACE") != nullptr) {
        std::printf("  [retire] cycle=%llu pc=%s rd=%u we=%u val=%s lq_occ=%u lq_iss=%u "
                    "lq_done=%u lq_replay=%u busy=%u\n",
                    static_cast<unsigned long long>(cycles_), U64(r.pc).c_str(), r.rd,
                    r.reg_we ? 1u : 0u, U64(r.value).c_str(), dut_->o_mem_lq_occupied_o,
                    dut_->o_mem_lq_issue_o, dut_->o_mem_lq_done_o, dut_->o_mem_lq_replay_o,
                    dut_->o_mem_lsu_busy_o);
        const uint32_t d = static_cast<uint32_t>(dut_->o_dbg_mmio_o);
        std::printf("            dbg: lq_rsp_valid=%u sq_rsp_valid=%u ser_out=%u ser_hold=%u "
                    "ep_busy=%u ep_rsp_valid=%u ep_owner=%u sq_drain=%u lq_req=%u "
                    "lq_done=%u\n",
                    (d >> 31) & 1u, (d >> 30) & 1u, (d >> 29) & 1u, (d >> 28) & 1u,
                    (d >> 27) & 1u, (d >> 26) & 1u, (d >> 25) & 1u, (d >> 24) & 1u,
                    (d >> 23) & 1u, d & 0x7FFFFFu);
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
           " lsu_busy=" + Dec(dut_->o_mem_lsu_busy_o) +
           " dev_txn=" + Dec(dut_->o_mem_dev_txn_o) +
           " dev_wait=" + Dec(dut_->o_mem_dev_wait_o);
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
  Imem imem_;
  Dmem dmem_;
  // The reset-traffic rule (V-010, sim/common/bus_reset_gate.h): while reset
  // is asserted this model accepts nothing, so no reset-time response can be
  // queued ahead of a fresh post-reset one.
  mosaic::BusResetGate bus_reset_;
  std::vector<Retire> retires_;
  std::vector<uint64_t> device_ids_;
  const std::vector<RefInsn>* expected_ = nullptr;
  uint64_t cycles_ = 0;
  uint64_t last_commit_ = 0;
  uint64_t last_alloc_ = 0;
  uint64_t last_progress_ = 0;
  uint64_t trap_events_ = 0;
  uint64_t trap_event_pc_ = 0;
  bool exited_ = false;
  uint64_t exit_code_ = 0;
  std::vector<uint64_t> forbidden_pcs_;

 public:
  void SetExpected(const std::vector<RefInsn>* trace) { expected_ = trace; }
};

// ============================================================================
// The checks, once per run
// ============================================================================
void CheckStream(Harness* harness, const RefResult& ref) {
  harness->Check("the machine retires every reference instruction: got " +
                     Dec(harness->retires().size()) + ", reference " +
                     Dec(ref.trace.size()),
                 harness->retires().size() >= ref.trace.size(), "fewer retirements");
  for (size_t i = 0; i < ref.trace.size() && i < harness->retires().size(); i++) {
    if (harness->retires()[i].pc != ref.trace[i].pc) {
      Fail("run", "retire " + Dec(i) + " pc expected " + U64(ref.trace[i].pc) + ", got " +
                      U64(harness->retires()[i].pc));
    }
  }
  harness->Check("the per-instruction retirement stream matches the RV64IM reference (" +
                     Dec(ref.trace.size()) + " instructions)",
                 true, "");
}

void CheckExactlyOnce(Vmosaic_core_tb* dut, Harness* harness, Device* dev,
                      const RefResult& ref) {
  const uint64_t seen_dev = harness->device_txns();
  const uint64_t seen_ram = harness->ram_txns();

  harness->Check("the device performed exactly the retired FIFO pops: model " +
                     Dec(dev->pops()) + ", reference " + Dec(ref.pops),
                 dev->pops() == ref.pops, "pop counts differ");
  harness->Check("the device performed exactly the retired UART writes: model " +
                     Dec(dev->writes()) + ", reference " + Dec(ref.writes),
                 dev->writes() == ref.writes, "write counts differ");
  harness->Check("the device answered exactly the retired error accesses: model " +
                     Dec(dev->errors()) + ", reference " + Dec(ref.errors),
                 dev->errors() == ref.errors, "error counts differ");
  harness->Check("the memory system saw exactly the device accesses the program "
                 "performs: " + Dec(seen_dev) + " vs " +
                     Dec(ref.pops + ref.writes + ref.errors),
                 seen_dev == ref.pops + ref.writes + ref.errors, "counts differ");
  harness->Check("the core's device-transaction count equals the memory system's "
                 "address-based device count: core " + Dec(dut->o_mem_dev_txn_o) +
                     ", memory " + Dec(seen_dev),
                 dut->o_mem_dev_txn_o == seen_dev, "counts differ");
  harness->Check("the core's RAM-transaction count equals the memory system's "
                 "address-based count: core " + Dec(dut->o_mem_ram_txn_o) + ", memory " +
                     Dec(seen_ram),
                 dut->o_mem_ram_txn_o == seen_ram, "counts differ");
  harness->Check("the device and RAM counters partition every transaction: " +
                     Dec(dut->o_mem_dev_txn_o + dut->o_mem_ram_txn_o) + " vs " +
                     Dec(harness->txns().size()),
                 dut->o_mem_dev_txn_o + dut->o_mem_ram_txn_o == harness->txns().size(),
                 "counts differ");

  // No device identity may appear twice: that is "one side effect per
  // transaction identity". A replay or a retry presents the same identity again
  // and is caught here.
  bool unique = true;
  std::string dup;
  for (size_t i = 0; i < harness->device_ids().size() && unique; i++) {
    for (size_t j = i + 1; j < harness->device_ids().size(); j++) {
      if (harness->device_ids()[i] == harness->device_ids()[j]) {
        unique = false;
        dup = "identity " + Dec(harness->device_ids()[i]) + " appears at device "
              "transactions " + Dec(i) + " and " + Dec(j);
        break;
      }
    }
  }
  harness->Check("every device transaction has a distinct identity",
                 unique, dup);
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

  mosaic::Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    // A check that fails must fail the run: Reporter::Check only records, its
    // verdict comes from the string this file passes to Finish, so every check
    // that matters goes through one of these two throwing wrappers.
    auto expect = [&](bool ok, const std::string& what) {
      reporter.Check(ok, what);
      if (!ok) Fail("harness", what);
    };

    // The identity of the wrong-path device load in run B, for the message.
    uint64_t runb_wrong_dev_pc = 0;
    uint64_t runb_dev_wait = 0;
    uint64_t runb_dev_txn = 0;
    uint64_t runb_ram_txn = 0;
    size_t runa_retires = 0;
    size_t runb_retires = 0;
    uint64_t runa_dev_txn = 0;
    uint64_t runa_ram_txn = 0;
    uint64_t runa_dev_wait = 0;
    uint64_t runa_dev_hold = 0;
    uint64_t runb_dev_hold = 0;
    uint64_t runa_cycles = 0;
    uint64_t runb_cycles = 0;

    // ======================================================================
    // Run A: the correct path, and the error device's precise trap
    // ======================================================================
    {
      const Program program = BuildCorrectProgram();
      Mem mem(program);
      Device dev;
      const RefResult ref = RunReference(program);

      // The reference's own expectations of the program, so the DUT is not
      // compared against a model that merely agrees with it.
      expect(ref.exited, "run A reference: the program reaches its exit protocol");
      expect(ref.pops == 2, "run A reference: the correct path pops twice (got " +
                                Dec(ref.pops) + ")");
      expect(ref.writes == 2, "run A reference: the correct path writes twice (got " +
                                  Dec(ref.writes) + ")");
      expect(ref.errors == 1, "run A reference: the error device is accessed once");
      expect(ref.trap_cause == kExcLoadAccess,
             "run A reference: the trap is a load access fault, cause 5 (got " +
                 Dec(ref.trap_cause) + ")");
      expect(ref.trap_pc == program.bad_pc,
             "run A reference: the trap names the faulting load's PC");
      expect(ref.trap_tval == kErr,
             "run A reference: the trap's tval is the device address");

      Harness harness(&dut, &reporter, options.max_cycles, &mem, &dev);
      harness.SetExpected(&ref.trace);
      harness.SetForbiddenPcs({program.bad_pc, program.no_trap_pc});
      harness.Reset();
      harness.Run();
      runa_retires = harness.retires().size();
      runa_dev_txn = dut.o_mem_dev_txn_o;
      runa_ram_txn = dut.o_mem_ram_txn_o;
      runa_dev_wait = dut.o_mem_dev_wait_o;
      runa_dev_hold = dut.o_mem_dev_hold_o;
      runa_cycles = harness.cycles();

      CheckStream(&harness, ref);
      CheckExactlyOnce(&dut, &harness, &dev, ref);

      harness.Check("the program's device traffic is exactly the retired accesses",
                    dut.o_mem_dev_txn_o == ref.pops + ref.writes + ref.errors,
                    "core " + Dec(dut.o_mem_dev_txn_o) + ", reference " +
                        Dec(ref.pops + ref.writes + ref.errors));
      harness.Check("the ordinary traffic is exactly the retired ordinary accesses in "
                    "this run (nothing speculates on the correct path): core " +
                        Dec(dut.o_mem_ram_txn_o) + ", reference " + Dec(ref.ram_accesses),
                    dut.o_mem_ram_txn_o == ref.ram_accesses,
                    "core " + Dec(dut.o_mem_ram_txn_o) + " vs " + Dec(ref.ram_accesses));
      harness.Check("exactly one trap event is taken (got " + Dec(harness.trap_events()) + ")",
                    harness.trap_events() == 1, "trap events " + Dec(harness.trap_events()));
      harness.Check("the trap event names the faulting load",
                    harness.trap_event_pc() == program.bad_pc,
                    U64(harness.trap_event_pc()) + " vs " + U64(program.bad_pc));
      harness.Check("the CSR file took exactly one trap (got " + Dec(dut.o_csr_trap_o) + ")",
                    dut.o_csr_trap_o == 1, "trap count " + Dec(dut.o_csr_trap_o));
      harness.Check("the handler published mepc = the faulting load's PC",
                    mem.Window(kSig + 16) == ref.trap_pc,
                    U64(mem.Window(kSig + 16)) + " vs " + U64(ref.trap_pc));
      harness.Check("the handler published mcause = 5 (load access fault)",
                    mem.Window(kSig + 24) == kExcLoadAccess,
                    U64(mem.Window(kSig + 24)));
      harness.Check("the handler published mtval = the device address",
                    mem.Window(kSig + 32) == kErr, U64(mem.Window(kSig + 32)));
      harness.Check("the first pop's value published in memory is the model's",
                    mem.Window(kSig) == ref.sig[0],
                    "memory " + U64(mem.Window(kSig)) + ", model " + U64(ref.sig[0]));
      harness.Check("the second pop's value published in memory is the model's",
                    mem.Window(kSig + 8) == ref.sig[1],
                    "memory " + U64(mem.Window(kSig + 8)) + ", model " + U64(ref.sig[1]));
      harness.Check("the UART received the two values the program wrote, in order",
                    dev.uart() == std::string(1, static_cast<char>(ref.sig[0] & 0xff)) +
                                      std::string(1, static_cast<char>(ref.sig[1] & 0xff)),
                    "uart has " + Dec(dev.uart().size()) + " bytes");
      harness.Check("no macro was refused as unsupported: " + Dec(dut.o_unsupported_o),
                    dut.o_unsupported_o == 0, "unsupported " + Dec(dut.o_unsupported_o));
      harness.Check("the program wrote TOHOST = 1 (got " + Dec(harness.exit_code()) + ")",
                    harness.exit_code() == 1, "exit code " + Dec(harness.exit_code()));
    }

    // ======================================================================
    // Run B: the wrong path, squashed by a trap rather than by a branch
    // ======================================================================
    {
      const Program program = BuildWrongPathProgram();
      Mem mem(program);
      Device dev;
      const RefResult ref = RunReference(program);
      runb_wrong_dev_pc = program.wrong_dev_pc;

      expect(ref.exited, "run B reference: the program reaches its exit protocol");
      expect(ref.pops == 1, "run B reference: the program pops the FIFO once (got " +
                                Dec(ref.pops) + ")");
      expect(ref.trap_cause == kExcEcallM,
             "run B reference: the trap is an ecall from M, cause 11 (got " +
                 Dec(ref.trap_cause) + ")");
      expect(ref.trap_pc == program.ecall_pc,
             "run B reference: the trap names the ecall's PC");

      Harness harness(&dut, &reporter, options.max_cycles, &mem, &dev);
      harness.SetExpected(&ref.trace);
      harness.Reset();
      harness.Run();
      runb_retires = harness.retires().size();
      runb_cycles = harness.cycles();
      runb_dev_wait = dut.o_mem_dev_wait_o;
      runb_dev_hold = dut.o_mem_dev_hold_o;
      runb_dev_txn = dut.o_mem_dev_txn_o;
      runb_ram_txn = dut.o_mem_ram_txn_o;

      CheckStream(&harness, ref);
      CheckExactlyOnce(&dut, &harness, &dev, ref);

      // The wrong-path window is real: the ordinary load that the ecall's flush
      // discards reached memory, and the reference (which never executes it)
      // does not account for it.
      bool spec_ram_seen = false;
      for (const Dmem::Txn& t : harness.txns()) {
        if (!t.we && t.addr == kSpecRam) spec_ram_seen = true;
      }
      harness.Check("the wrong-path ordinary load reached memory (the window is real, "
                    "and ordinary loads may run in it)",
                    spec_ram_seen, "no read of " + U64(kSpecRam));
      harness.Check("ordinary RAM traffic exceeds the retired accesses by the speculative "
                    "one: core " + Dec(runb_ram_txn) + ", reference " +
                        Dec(ref.ram_accesses),
                    runb_ram_txn > ref.ram_accesses,
                    "core " + Dec(runb_ram_txn) + " vs " + Dec(ref.ram_accesses));
      harness.Check("the wrong-path device load did not reach the device (the device "
                    "popped " + Dec(dev.pops()) + " times, the program pops " +
                        Dec(ref.pops) + ")",
                    dev.pops() == ref.pops,
                    "pops " + Dec(dev.pops()) + " vs " + Dec(ref.pops));
      harness.Check("the non-speculation gate refused the wrong-path device offer "
                    "(dev_wait=" + Dec(dut.o_mem_dev_wait_o) + ")",
                    dut.o_mem_dev_wait_o > 0, "dev_wait=" + Dec(dut.o_mem_dev_wait_o));
      harness.Check("the ecall's trap is precise (PC " + U64(harness.trap_event_pc()) + ")",
                    harness.trap_events() == 1 && harness.trap_event_pc() == program.ecall_pc,
                    "events " + Dec(harness.trap_events()) + " pc " +
                        U64(harness.trap_event_pc()) + " vs " + U64(program.ecall_pc));
      harness.Check("the handler published mcause = 11 (ecall from M)",
                    mem.Window(kSig + 8) == kExcEcallM, U64(mem.Window(kSig + 8)));
      harness.Check("the handler published mepc = the ecall's PC",
                    mem.Window(kSig) == program.ecall_pc,
                    U64(mem.Window(kSig)) + " vs " + U64(program.ecall_pc));
      harness.Check("no macro was refused as unsupported: " + Dec(dut.o_unsupported_o),
                    dut.o_unsupported_o == 0, "unsupported " + Dec(dut.o_unsupported_o));
      harness.Check("the program wrote TOHOST = 1 (got " + Dec(harness.exit_code()) + ")",
                    harness.exit_code() == 1, "exit code " + Dec(harness.exit_code()));
    }

    std::printf("  run A: retires=%zu dev_txn=%llu ram_txn=%llu dev_hold=%llu dev_wait=%llu "
                "cycles=%llu\n",
                runa_retires, static_cast<unsigned long long>(runa_dev_txn),
                static_cast<unsigned long long>(runa_ram_txn),
                static_cast<unsigned long long>(runa_dev_hold),
                static_cast<unsigned long long>(runa_dev_wait),
                static_cast<unsigned long long>(runa_cycles));
    std::printf("  run B: retires=%zu dev_txn=%llu ram_txn=%llu dev_hold=%llu dev_wait=%llu "
                "cycles=%llu wrong_dev_pc=%s\n",
                runb_retires, static_cast<unsigned long long>(runb_dev_txn),
                static_cast<unsigned long long>(runb_ram_txn),
                static_cast<unsigned long long>(runb_dev_hold),
                static_cast<unsigned long long>(runb_dev_wait),
                static_cast<unsigned long long>(runb_cycles),
                U64(runb_wrong_dev_pc).c_str());
    detail = "checks=" + Dec(reporter.checks()) + " runA_retires=" + Dec(runa_retires) +
             " runA_dev_txn=" + Dec(runa_dev_txn) + " runA_ram_txn=" + Dec(runa_ram_txn) +
             " runB_retires=" + Dec(runb_retires) +
             " runB_dev_txn=" + Dec(runb_dev_txn) + " runB_ram_txn=" + Dec(runb_ram_txn) +
             " runA_dev_hold=" + Dec(runa_dev_hold) + " runB_dev_hold=" +
             Dec(runb_dev_hold) + " runB_dev_wait=" + Dec(runb_dev_wait) + " runA_cycles=" +
             Dec(runa_cycles) + " runB_cycles=" + Dec(runb_cycles) + " seed=" +
             Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the MMIO contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
