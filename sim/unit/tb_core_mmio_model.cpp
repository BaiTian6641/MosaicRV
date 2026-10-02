// ============================================================================
// tb_core_mmio_model.cpp -- CASE=mmio.side_effect_model, work package V-019.
//
// The DUT is the integrated p0 core with the memory path (I-033..I-035), the
// device serializer (I-038) and the translation stub (I-045): the same top
// CASE=mmio.exactly_once, CASE=core.mem_program and CASE=fence.code_and_data_order
// drive. What this case adds is a *device*: the p0 map's non-idempotent regions
// are modelled register by register, on the memory system's side of the data
// port, with the properties a device has and ordinary memory does not.
//
// ---------------------------------------------------------------- the model
//
// The locked event script (`LockedEvents`, one const object) is the device's
// whole input state: the UART receive stream, the status word, the scratch and
// timer reset values and the exit register. The DUT's device and the
// independent reference's device are two instances of the *same* class built
// from that one object, so "the same locked event is given to both sides" is a
// property of the construction and not a promise.
//
// The p0 map's three non-idempotent regions (config/memory/p0.json: uart,
// test_harness, clint) carry these registers:
//
//   uart  +0x00  RX       read  -- DESTRUCTIVE: pops one word of the locked RX
//                                    stream, so reading it twice is two side
//                                    effects and two different values;
//   uart  +0x04  TX       write -- appends the byte lanes the strobes select;
//   uart  +0x08  STATUS   read  -- idempotent: reading it twice reads the same
//                                    word and changes nothing;
//   uart  +0x0C  SCRATCH  r/w   -- a byte-addressable register, so a byte or
//                                    halfword store changes only the bytes its
//                                    strobes select;
//   harness +0x00 ID      read  -- the test-harness window's identity register.
//                                    The window is readable and, by the core's
//                                    own PMA store map, not writable, so a write
//                                    to it is refused before memory;
//   clint +0x00  MTIME    read  -- idempotent 8-byte timer (locked value);
//   clint +0x08  MTIMECMP write -- 8-byte compare register, strobed writes;
//   clint +0x10  EXIT     write -- the test-end register: records the exit code
//                                    and ends the run. The p0 protocol's TOHOST
//                                    lives in RAM; the test-end *device* register
//                                    is modelled here because a register in
//                                    test_harness could never be written.
//
// Every other address in a device region is *not modelled* and is answered with
// a fault -- never with a silent zero. An access whose width does not fit its
// register (a doubleword of a four-byte register), an unaligned access, an
// address no region covers, and an unmodelled register all get the same answer:
// the device rejects the access, it has no side effect, and the core must trap
// on it with the architectural cause and tval.
//
// ---------------------------------------------------------------- the checks
//
// Against an independent RV64IM_Zicsr interpreter in this file -- it decodes the
// same words, runs against its own RAM and its own device instance from the same
// locked events, and dispatches loads and stores to the same `Device` class --
// each run checks:
//
//   1. the retirement stream equals the reference, in order, with every value;
//   2. the *device accesses the program made*, in order, field by field: the
//      address, read/write, size, byte strobes and data. This is the check that
//      makes the case more than a data comparison: a device access whose read
//      data happens to be the same is still a different access if its address
//      differs, and the check names the field;
//   3. the actual side effects -- pops, transmit bytes, register writes, the
//      exit write -- happen exactly once each, in order, and the final device
//      state (scratch, mtimecmp, UART output, exit code) equals the reference's;
//   4. the memory system's device/ordinary classification and transaction
//      identity: the core's counters partition the data-port transactions, every
//      access carries the device attribute its region demands and every device
//      identity is distinct;
//   5. a cancelled access has no side effect: a device load or store on the
//      wrong path of a trap never reaches the device (run B), while the
//      wrong-path *ordinary* load does reach memory -- the window is real;
//   6. every trap the illegal-width / unaligned / unmapped / unmodelled scenes
//      require is precise: exactly one trap, the right PC, and the published
//      mepc/mcause/mtval equal the reference's.
//
// ---------------------------------------------------------------- the controls
//
// tools/run_mmio_model_controls.py builds this case from a deleted directory
// with each control's `-D` on the build's own command line and requires the
// binary to differ from the shipping one and to exit 1 naming the check it
// breaks. The mutants are existing RTL mutations (this lane does not edit RTL)
// plus two harness-side switches that this file implements:
//
//   * MOSAIC_MMIO_MODEL_MUTANT_STATUS_AS_SCRATCH -- the DUT observation presents
//     a STATUS read at SCRATCH's address. The two registers hold the same word
//     from the locked events, so the read *data* and every published value are
//     identical; only the address in the access log differs. The case must still
//     fail and name the address, which is the card's "identical MMIO read data
//     must not mask a wrong address";
//   * MOSAIC_MMIO_MODEL_RD_ONLY -- a weakening control: the driver drops the
//     access-log, attribute and device-state comparisons and keeps only the
//     published-data comparison, so the STATUS_AS_SCRATCH injection then passes.
//     That is what makes the address/attribute comparison load-bearing rather
//     than decorative.
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

// ------------------------------------------------------------- plan constants
constexpr int kResetCycles = 4;
// More than one cycle on purpose: the memory system is a slave that does not
// answer in the cycle it is asked, so the device serializer holds the access
// across that window and the case exercises the hold under backpressure.
constexpr int kMemLatency = 4;
// A few hundred cycles per run; only a stopped machine reaches this.
constexpr int kStallCycles = 12000;

// config/memory/p0.json, through mosaic_cfg_pkg: the frozen p0 map. The device
// predicate below is the same map written out here, so the expectation does not
// come from the DUT.
constexpr uint64_t kBootRomBase = 0x00000000ull;
constexpr uint64_t kBootRomSize = 0x1000ull;
constexpr uint64_t kRamBase     = 0x80000000ull;
constexpr uint64_t kRamSize     = 0x200000ull;    // 2 MiB
constexpr uint64_t kUartBase    = 0x00100000ull;
constexpr uint64_t kUartSize    = 0x100ull;
constexpr uint64_t kHarnessBase = 0x00102000ull;
constexpr uint64_t kHarnessSize = 0x10ull;
constexpr uint64_t kClintBase   = 0x02000000ull;
constexpr uint64_t kClintSize   = 0x1000ull;

// The modelled registers.
constexpr uint64_t kUartRx      = kUartBase + 0x00;
constexpr uint64_t kUartTx      = kUartBase + 0x04;
constexpr uint64_t kUartStatus  = kUartBase + 0x08;
constexpr uint64_t kUartScratch = kUartBase + 0x0C;
// The test_harness window: mapped and readable, and -- by the core's own PMA
// store map (mosaic_core.sv, `store_fault_kind`) -- not writable, so it carries
// an identity register and no writable one. config/memory/p0.json marks the
// region `writable: true`; the RTL's map disagrees. This case drives the
// behaviour the core implements: a read is a modelled register, a write is
// refused before memory. The disagreement is recorded in the report.
constexpr uint64_t kHarnessId   = kHarnessBase + 0x00;
constexpr uint64_t kClintMtime  = kClintBase + 0x00;
constexpr uint64_t kClintMtimecmp = kClintBase + 0x08;
// The test-end register. The p0 protocol's TOHOST lives in RAM; the card's
// "test-end device register" is modelled here, in the clint device region,
// because a device register in test_harness could never be written.
constexpr uint64_t kClintExit   = kClintBase + 0x10;
// Addresses the map does not cover, and device addresses it does not model.
constexpr uint64_t kUnmappedGap   = 0x00100100ull;  // between uart and harness
constexpr uint64_t kUnmodelledReg = kUartBase + 0x40;
constexpr uint64_t kMisalignedScratch = kUartScratch + 2;
// A store into the readable device window the core's map refuses: it faults at
// dispatch, before memory, exactly as the map says.
constexpr uint64_t kHarnessWriteAddr = kHarnessBase + 0x04;

// Where the programs publish, a wrong-path ordinary load's target and the exit
// register (a device, so the exit is itself a modelled side effect).
constexpr uint64_t kSig     = kRamBase + 0x1400ull;
constexpr uint64_t kSpecRam = kRamBase + 0x2000ull;

// The memory system's response latency and the interrupt/timer lines the case
// drives: no interrupt is asserted, and the driven `mtime` input is the locked
// timer value so the platform's timer line cannot drift from the model's.
constexpr uint64_t kMtimeDriven = 0x0000000000001000ull;

bool IsDevice(uint64_t addr) {
  return (addr >= kUartBase && addr - kUartBase < kUartSize) ||
         (addr >= kHarnessBase && addr - kHarnessBase < kHarnessSize) ||
         (addr >= kClintBase && addr - kClintBase < kClintSize);
}

// ---------------------------------------------------------- locked event data
// The device's whole input state. Both devices are built from one instance.
struct LockedEvents {
  static constexpr unsigned kRxCount = 4;
  uint64_t rx[kRxCount] = {0x00000041ull, 0x00000042ull, 0x00000043ull, 0x00000044ull};
  uint64_t status = 0x0000005Aull;
  // The scratch reset equals the status word on purpose: the wrong-address
  // control needs two registers that return the same data.
  uint64_t scratch = 0x0000005Aull;
  uint64_t harness_id = 0x54455354ull;  // "TEST"
  uint64_t mtime = kMtimeDriven;
  uint64_t mtimecmp = 0x0000000000002000ull;
  uint64_t exit_value = 1;
};

// ------------------------------------------------------------------- encoders
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
  return ((u >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
         ((u & 0x1Fu) << 7) | op;
}
uint32_t EncJ(int32_t offset, uint32_t rd) {
  const uint32_t u = static_cast<uint32_t>(offset);
  const uint32_t imm = (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3FFu) << 21) |
                       (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xFFu) << 12);
  return imm | (rd << 7) | 0x6Fu;
}
uint32_t EncLui(uint32_t rd, uint32_t imm20) { return (imm20 << 12) | (rd << 7) | 0x37u; }
uint32_t EncAuipc(uint32_t rd, uint32_t imm20) { return (imm20 << 12) | (rd << 7) | 0x17u; }

// Register names used below.
enum { X0 = 0, T0 = 5, T1 = 6, T2 = 7, S0 = 8, T3 = 28, T4 = 29, T5 = 30, T6 = 31 };
enum { CSR_MTVEC = 0x305, CSR_MEPC = 0x341, CSR_MCAUSE = 0x342, CSR_MTVAL = 0x343 };

// Exception codes, from mosaic_pkg.
constexpr uint64_t kExcLoadMisaligned  = 4;
constexpr uint64_t kExcLoadAccess      = 5;
constexpr uint64_t kExcStoreMisaligned = 6;
constexpr uint64_t kExcStoreAccess     = 7;
constexpr uint64_t kExcEcallM          = 11;

// ------------------------------------------------------------- small helpers
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string Dec(uint64_t value) { return std::to_string(value); }

unsigned SizeBytes(unsigned code) { return (code < 3) ? (1u << code) : 8u; }

uint64_t MaskBytes(unsigned bytes) {
  return (bytes >= 8) ? ~UINT64_C(0) : ((UINT64_C(1) << (8 * bytes)) - 1);
}

// The byte strobes for an access of `size` bytes at `addr`, laid in the
// doubleword window the data port carries -- mosaic_uop_pkg::expected_wstrb.
// The strobe mask is one *bit per byte*, not the byte-value mask above.
uint32_t StrobeMask(unsigned size) {
  switch (size) {
    case 1: return 0x01u;
    case 2: return 0x03u;
    case 4: return 0x0Fu;
    default: return 0xFFu;
  }
}

uint32_t ExpectedWstrb(unsigned size, uint64_t addr) {
  return StrobeMask(size) << (addr & 7u);
}

uint64_t Extract(uint64_t window, uint64_t addr, unsigned size, bool sign) {
  uint64_t value = (window >> (8 * (addr & 7u))) & MaskBytes(size);
  if (sign && size < 8) {
    const int shift = 64 - static_cast<int>(8 * size);
    value = static_cast<uint64_t>(static_cast<int64_t>(value << shift) >> shift);
  }
  return value;
}

// ============================================================================
// The device: the p0 map's non-idempotent regions, register by register
// ============================================================================
enum class EffectKind { kPop, kTx, kScratchWrite, kMtimecmpWrite, kExit };
const char* EffectName(EffectKind kind) {
  switch (kind) {
    case EffectKind::kPop: return "RX pop";
    case EffectKind::kTx: return "TX append";
    case EffectKind::kScratchWrite: return "SCRATCH write";
    case EffectKind::kMtimecmpWrite: return "MTIMECMP write";
    case EffectKind::kExit: return "EXIT write";
  }
  return "?";
}

class Device {
 public:
  // One access the program made to the device. `performed` false means the
  // device rejected it: the response is a fault, the core must trap and there is
  // no side effect.
  struct Access {
    uint64_t addr = 0;
    bool we = false;
    unsigned size = 0;      // bytes
    uint32_t wstrb = 0;
    uint64_t wdata = 0;
    bool performed = false;
  };
  // A performed access that changed device state.
  struct Effect {
    EffectKind kind;
    uint64_t addr = 0;
    uint64_t value = 0;
  };

  explicit Device(const LockedEvents& events)
      : ev_(events), scratch_(events.scratch & 0xFFFFFFFFull),
        mtimecmp_(events.mtimecmp) {}

  // `log_addr` is the address the access is *reported* at; it defaults to the
  // address it is performed at. The wrong-address control uses it to present a
  // read at a different register's address while performing the real one, so the
  // read data is identical and only the recorded address differs.
  bool Perform(uint64_t addr, bool we, unsigned size, uint32_t wstrb, uint64_t wdata,
               uint64_t* rdata, uint64_t log_addr = UINT64_MAX) {
    Access a;
    a.addr = (log_addr == UINT64_MAX) ? addr : log_addr;
    a.we = we;
    a.size = size;
    a.wstrb = wstrb;
    a.wdata = wdata;
    uint64_t window = 0;
    a.performed = Apply(&window, addr, we, size, wstrb, wdata);
    accesses_.push_back(a);
    if (a.performed) {
      *rdata = window;
    } else {
      *rdata = 0;
      faults_++;
    }
    return a.performed;
  }

  const std::vector<Access>& accesses() const { return accesses_; }
  const std::vector<Effect>& effects() const { return effects_; }
  uint64_t pops() const { return pops_; }
  uint64_t tx_accesses() const { return tx_accesses_; }
  uint64_t exit_writes() const { return exit_writes_; }
  uint64_t faults() const { return faults_; }
  uint64_t scratch() const { return scratch_; }
  uint64_t mtimecmp() const { return mtimecmp_; }
  const std::string& uart() const { return uart_; }
  bool exited() const { return exited_; }
  uint64_t exit_code() const { return exit_code_; }

 private:
  // True when [addr, addr+size) lies inside the register window [base, base+width)
  // and the access is aligned to its own size.
  static bool Fits(uint64_t addr, unsigned size, uint64_t base, uint64_t width) {
    return addr >= base && (addr + size) <= (base + width) && (addr % size) == 0;
  }

  static uint64_t Slice(uint64_t reg, uint64_t offset, unsigned size) {
    return (reg >> (8 * offset)) & MaskBytes(size);
  }

  static void ApplyStrobes(uint64_t* reg, uint64_t base, uint64_t addr, unsigned size,
                           uint32_t wstrb, uint64_t wdata) {
    for (unsigned i = 0; i < size; i++) {
      const unsigned bit = static_cast<unsigned>((addr + i) & 7u);
      if (((wstrb >> bit) & 1u) == 0) continue;
      const uint64_t index = addr - base + i;
      const uint64_t byte = (wdata >> (8 * bit)) & 0xFFu;
      *reg = (*reg & ~(UINT64_C(0xFF) << (8 * index))) | (byte << (8 * index));
    }
  }

  bool Apply(uint64_t* window, uint64_t addr, bool we, unsigned size, uint32_t wstrb,
             uint64_t wdata) {
    if (size == 0 || size > 8 || (size & (size - 1)) != 0) return false;
    const uint64_t lane = addr & 7u;

    if (addr >= kUartBase && addr - kUartBase < kUartSize) {
      if (!we) {
        if (Fits(addr, size, kUartRx, 4)) {
          const uint64_t value = Pop();
          *window = Slice(value, addr - kUartRx, size) << (8 * lane);
          return true;
        }
        if (Fits(addr, size, kUartStatus, 4)) {
          *window = Slice(ev_.status, addr - kUartStatus, size) << (8 * lane);
          return true;
        }
        if (Fits(addr, size, kUartScratch, 4)) {
          *window = Slice(scratch_, addr - kUartScratch, size) << (8 * lane);
          return true;
        }
        return false;
      }
      if (Fits(addr, size, kUartTx, 4)) {
        uint64_t appended = 0;
        unsigned count = 0;
        for (unsigned i = 0; i < size; i++) {
          const unsigned bit = static_cast<unsigned>((addr + i) & 7u);
          if (((wstrb >> bit) & 1u) == 0) continue;
          const uint64_t byte = (wdata >> (8 * bit)) & 0xFFu;
          uart_.push_back(static_cast<char>(byte));
          appended |= byte << (8 * count++);
        }
        tx_accesses_++;
        effects_.push_back(Effect{EffectKind::kTx, addr, appended});
        return true;
      }
      if (Fits(addr, size, kUartScratch, 4)) {
        ApplyStrobes(&scratch_, kUartScratch, addr, size, wstrb, wdata);
        effects_.push_back(Effect{EffectKind::kScratchWrite, addr, scratch_});
        return true;
      }
      return false;
    }

    if (addr >= kHarnessBase && addr - kHarnessBase < kHarnessSize) {
      // The test_harness window carries the identity register and nothing else;
      // a write here never arrives (the core's store map refuses it at dispatch)
      // and would be refused here anyway.
      if (!we && Fits(addr, size, kHarnessId, 4)) {
        *window = Slice(ev_.harness_id, addr - kHarnessId, size) << (8 * lane);
        return true;
      }
      return false;
    }

    if (addr >= kClintBase && addr - kClintBase < kClintSize) {
      if (!we && Fits(addr, size, kClintMtime, 8)) {
        *window = Slice(ev_.mtime, addr - kClintMtime, size) << (8 * lane);
        return true;
      }
      if (we && Fits(addr, size, kClintMtimecmp, 8)) {
        ApplyStrobes(&mtimecmp_, kClintMtimecmp, addr, size, wstrb, wdata);
        effects_.push_back(Effect{EffectKind::kMtimecmpWrite, addr, mtimecmp_});
        return true;
      }
      if (we && Fits(addr, size, kClintExit, 4)) {
        uint64_t value = 0;
        for (unsigned i = 0; i < size; i++) {
          const unsigned bit = static_cast<unsigned>((addr + i) & 7u);
          if (((wstrb >> bit) & 1u) == 0) continue;
          value |= ((wdata >> (8 * bit)) & 0xFFu) << (8 * i);
        }
        exit_writes_++;
        exit_code_ = value;
        exited_ = (value != 0);
        effects_.push_back(Effect{EffectKind::kExit, addr, value});
        return true;
      }
      return false;
    }
    return false;
  }

  uint64_t Pop() {
    const uint64_t value = (rx_at_ < LockedEvents::kRxCount) ? ev_.rx[rx_at_] : 0;
    rx_at_++;
    pops_++;
    effects_.push_back(Effect{EffectKind::kPop, kUartRx, value});
    return value;
  }

  LockedEvents ev_;
  uint64_t scratch_;
  uint64_t mtimecmp_;
  uint64_t rx_at_ = 0;
  uint64_t pops_ = 0;
  uint64_t tx_accesses_ = 0;
  uint64_t exit_writes_ = 0;
  uint64_t faults_ = 0;
  bool exited_ = false;
  uint64_t exit_code_ = 0;
  std::string uart_;
  std::vector<Access> accesses_;
  std::vector<Effect> effects_;
};

// ============================================================================
// The program images
// ============================================================================
struct Program {
  std::vector<uint32_t> words;
  uint64_t fault_pc   = 0;   // trap scenes: the faulting access
  uint64_t marker_pc  = 0;   // trap scenes: the fall-through marker (must not retire)
  uint64_t handler_pc = 0;
};

struct Builder {
  Program p;
  size_t handler_addr_at = 0;

  uint64_t pc(size_t index) const { return kRamBase + 4 * index; }
  uint64_t emit(uint32_t word) {
    p.words.push_back(word);
    return pc(p.words.size() - 1);
  }
  // An absolute address PC-relatively: auipc carries the upper twenty bits of
  // (target - pc) and the addi the signed low twelve.
  void addr(uint32_t rd, uint64_t target) {
    const size_t at = p.words.size();
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(pc(at));
    const int64_t hi = (delta + 0x800) >> 12;
    const int64_t lo = delta - (hi << 12);
    emit(EncAuipc(rd, static_cast<uint32_t>(hi) & 0xFFFFFu));
    emit(EncI(static_cast<int32_t>(lo), rd, 0x0u, rd, 0x13u));
  }
  void li(uint32_t rd, uint32_t value) {
    if (value < 0x800u) {
      emit(EncI(static_cast<int32_t>(value), X0, 0x0u, rd, 0x13u));
      return;
    }
    const int32_t sv = static_cast<int32_t>(value);
    const int32_t hi = (sv + 0x800) >> 12;
    const int32_t lo = sv - (hi << 12);
    emit(EncLui(rd, static_cast<uint32_t>(hi) & 0xFFFFFu));
    if (lo != 0) emit(EncI(lo, rd, 0x0u, rd, 0x13u));
  }
  void addi(uint32_t rd, uint32_t rs1, int32_t imm) {
    emit(EncI(imm, rs1, 0x0u, rd, 0x13u));
  }
  void lui(uint32_t rd, uint32_t imm20) { emit(EncLui(rd, imm20)); }
  void lw(uint32_t rd, uint32_t rs1, int32_t imm) { emit(EncI(imm, rs1, 0x2u, rd, 0x03u)); }
  void lh(uint32_t rd, uint32_t rs1, int32_t imm) { emit(EncI(imm, rs1, 0x1u, rd, 0x03u)); }
  void lb(uint32_t rd, uint32_t rs1, int32_t imm) { emit(EncI(imm, rs1, 0x0u, rd, 0x03u)); }
  void ld(uint32_t rd, uint32_t rs1, int32_t imm) { emit(EncI(imm, rs1, 0x3u, rd, 0x03u)); }
  void sw(uint32_t rs2, uint32_t rs1, int32_t imm) { emit(EncS(imm, rs2, rs1, 0x2u, 0x23u)); }
  void sh(uint32_t rs2, uint32_t rs1, int32_t imm) { emit(EncS(imm, rs2, rs1, 0x1u, 0x23u)); }
  void sb(uint32_t rs2, uint32_t rs1, int32_t imm) { emit(EncS(imm, rs2, rs1, 0x0u, 0x23u)); }
  void sd(uint32_t rs2, uint32_t rs1, int32_t imm) { emit(EncS(imm, rs2, rs1, 0x3u, 0x23u)); }
  void div(uint32_t rd, uint32_t rs1, uint32_t rs2) {
    emit(EncR(0x01u, rs2, rs1, 0x4u, rd, 0x33u));
  }
  void csrw(uint32_t csr, uint32_t rs1) {
    emit(EncI(static_cast<int32_t>(csr), rs1, 0x1u, X0, 0x73u));
  }
  void csrr(uint32_t rd, uint32_t csr) {
    emit(EncI(static_cast<int32_t>(csr), X0, 0x2u, rd, 0x73u));
  }
  void ecall() { emit(0x00000073u); }
  void j(int32_t offset) { emit(EncJ(offset, X0)); }
  void nop() { addi(X0, X0, 0); }
  // Write the test-end device register with the value 1.
  void exit() {
    addr(T0, kClintExit);
    li(T1, 1);
    sw(T1, T0, 0);
  }
  void addr_handler(uint32_t rd) {
    handler_addr_at = p.words.size();
    emit(EncAuipc(rd, 0));
    emit(EncI(0, rd, 0x0u, rd, 0x13u));
  }
  void fixup_handler(uint32_t rd) {
    const int64_t delta =
        static_cast<int64_t>(p.handler_pc) - static_cast<int64_t>(pc(handler_addr_at));
    const int64_t hi = (delta + 0x800) >> 12;
    const int64_t lo = delta - (hi << 12);
    p.words[handler_addr_at] = EncAuipc(rd, static_cast<uint32_t>(hi) & 0xFFFFFu);
    p.words[handler_addr_at + 1] =
        EncI(static_cast<int32_t>(lo), rd, 0x0u, rd, 0x13u);
  }
};

// ---- run A: the device contract on the correct path ----
// Every side effect the card names, on the path that must perform it:
// destructive reads, an idempotent read beside them, the timer, byte- and
// halfword-strobed writes to a byte-addressable register, a full-word overwrite,
// transmit bytes, and the exit register. Every device read is published, so a
// duplicated or dropped side effect moves the numbers.
Program BuildContractProgram() {
  Builder b;
  b.addr(T2, kSig);

  // Two destructive reads: the second must be a second pop with the second value.
  b.addr(T0, kUartRx);
  b.lw(T1, T0, 0);
  b.sd(T1, T2, 0);
  b.lw(T3, T0, 0);
  b.sd(T3, T2, 8);

  // The idempotent status register: two reads, the same word, no side effect.
  b.addr(T0, kUartStatus);
  b.lw(T1, T0, 0);
  b.sd(T1, T2, 16);
  b.lw(T3, T0, 0);
  b.sd(T3, T2, 24);

  // The readable test-harness window: also idempotent, and a different region.
  b.addr(T0, kHarnessId);
  b.lw(T1, T0, 0);
  b.sd(T1, T2, 32);
  b.lw(T3, T0, 0);
  b.sd(T3, T2, 40);

  // The idempotent timer: two doubleword reads, the same locked value.
  b.addr(T0, kClintMtime);
  b.ld(T1, T0, 0);
  b.sd(T1, T2, 48);
  b.ld(T3, T0, 0);
  b.sd(T3, T2, 56);

  // Strobed writes: a byte at offset 3, then a halfword at offset 0, each
  // changing only the bytes its strobes select.
  b.addr(T0, kUartScratch);
  b.li(T1, 0x77);
  b.sb(T1, T0, 3);
  b.li(T1, 0x1234);
  b.sh(T1, T0, 0);
  b.lw(T3, T0, 0);
  b.sd(T3, T2, 64);
  // A full-word overwrite of the same register.
  b.li(T1, 0xBEEF);
  b.sw(T1, T0, 0);
  b.lw(T3, T0, 0);
  b.sd(T3, T2, 72);

  // Transmit bytes, in order.
  b.addr(T0, kUartTx);
  b.li(T1, 0x41);
  b.sb(T1, T0, 0);
  b.li(T1, 0x42);
  b.sb(T1, T0, 0);

  b.exit();
  b.j(0);
  return b.p;
}

// ---- run B: a cancelled access has no side effect ----
// An iterative div keeps the ROB head busy while a trapping ecall sits behind
// it; behind the ecall are a wrong-path ordinary load (which *does* reach
// memory, so the window is real), a wrong-path device read, a wrong-path device
// write and a second wrong-path device read. Every device access must not be
// presented, because a device access is presented only when it cannot be
// squashed -- at the ROB head or after its store retired.
Program BuildCancelledProgram() {
  Builder b;
  b.addr(T3, kUartRx);
  b.lw(T1, T3, 0);                    // the one legitimate pop
  b.addr(T5, kUartStatus);            // wrong path: a device read
  b.addr(T4, kSpecRam);               // wrong path: an ordinary load's target
  b.addr_handler(T0);
  b.csrw(CSR_MTVEC, T0);              // arm the vector before the ecall
  // An ECALL is refused at dispatch until the mtvec write has retired and the
  // front end has run past its window; this is the idiom CASE=core.trap_csr_program
  // documents.
  for (int i = 0; i < 16; i++) b.nop();
  b.lui(T6, 0x10000);                 // t6 = 0x10000000
  b.addi(T6, T6, 1);                  // t6 = 0x10000001
  b.addi(T0, X0, 3);                  // divisor
  b.div(S0, T6, T0);                  // slow: keeps the ecall from the head
  b.p.fault_pc = b.emit(EncI(0, 0, 0x0u, X0, 0x73u));  // ECALL: traps
  b.lw(T1, T4, 0);                    // WRONG PATH: an ordinary load (runs)
  b.addr(T2, kUartTx);                // WRONG PATH: a device write target
  b.li(T1, 0x5A);
  b.sb(T1, T2, 0);                    // WRONG PATH: a device store
  b.lw(T2, T5, 0);                    // WRONG PATH: a device read
  b.lw(T2, T3, 0);                    // WRONG PATH: a second pop
  b.j(0);
  // The handler: publish the trap and end the run.
  b.p.handler_pc = b.pc(b.p.words.size());
  b.csrr(T1, CSR_MEPC);
  b.csrr(T2, CSR_MCAUSE);
  b.addr(T4, kSig);
  b.sd(T1, T4, 0);
  b.sd(T2, T4, 8);
  b.exit();
  b.j(0);
  b.fixup_handler(T0);
  return b.p;
}

// ---- the trap scenes ----
enum class TrapKind {
  kIllegalWidth,     // a doubleword read of a four-byte register
  kUnmappedRead,     // a load no region covers
  kUnmodelledReg,    // a device register the model does not define
  kMisalignedRead,   // a device access the endpoint refuses before memory
  kUnmappedWrite,    // a store no region covers
  kHarnessWrite,     // a store into the readable device window the map refuses
};

struct Scene {
  std::string name;
  Program program;
  uint64_t fault_pc = 0;
  uint64_t marker_pc = 0;
  uint64_t expect_cause = 0;
  uint64_t expect_tval = 0;
  // The endpoint accepts a misaligned access and then refuses it before the
  // memory sees it, so the core's device counter is one above the memory's.
  uint64_t core_only_dev = 0;
  bool reaches_device = false;  // the faulting access is presented to the device
};

Scene BuildTrapScene(TrapKind kind) {
  Scene s;
  uint64_t target = 0;
  switch (kind) {
    case TrapKind::kIllegalWidth:
      s.name = "illegal width";
      target = kUartStatus;
      s.expect_cause = kExcLoadAccess;
      break;
    case TrapKind::kUnmappedRead:
      s.name = "unmapped read";
      target = kUnmappedGap;
      s.expect_cause = kExcLoadAccess;
      break;
    case TrapKind::kUnmodelledReg:
      s.name = "unmodelled device register";
      target = kUnmodelledReg;
      s.expect_cause = kExcLoadAccess;
      break;
    case TrapKind::kMisalignedRead:
      s.name = "misaligned device read";
      target = kMisalignedScratch;
      s.expect_cause = kExcLoadMisaligned;
      s.core_only_dev = 1;
      break;
    case TrapKind::kUnmappedWrite:
      s.name = "unmapped write";
      target = kUnmappedGap;
      s.expect_cause = kExcStoreAccess;
      break;
    case TrapKind::kHarnessWrite:
      s.name = "write to the test-harness window";
      target = kHarnessWriteAddr;
      s.expect_cause = kExcStoreAccess;
      break;
  }
  s.expect_tval = target;

  Builder b;
  b.addr(T0, target);
  b.li(T1, 0x1234);                  // a store scene needs data in the register
  b.nop();
  b.addr_handler(T5);
  b.csrw(CSR_MTVEC, T5);
  for (int i = 0; i < 4; i++) b.nop();
  s.fault_pc = b.pc(b.p.words.size());
  if (kind == TrapKind::kUnmappedWrite || kind == TrapKind::kHarnessWrite) {
    b.sw(T1, T0, 0);
    s.reaches_device = false;
  } else {
    if (kind == TrapKind::kIllegalWidth) {
      b.ld(T1, T0, 0);
    } else {
      b.lw(T1, T0, 0);
    }
    s.reaches_device = IsDevice(target) && kind != TrapKind::kMisalignedRead;
  }
  // The fall-through, taken only if the access does *not* trap.
  s.marker_pc = b.pc(b.p.words.size());
  b.li(T1, 0x7AD);
  b.addr(T2, kSig + 64);
  b.sd(T1, T2, 0);
  b.exit();
  b.j(0);
  // The handler: publish mepc/mcause/mtval and end the run.
  b.p.handler_pc = b.pc(b.p.words.size());
  b.csrr(T1, CSR_MEPC);
  b.csrr(T2, CSR_MCAUSE);
  b.csrr(T3, CSR_MTVAL);
  b.addr(T4, kSig);
  b.sd(T1, T4, 0);
  b.sd(T2, T4, 8);
  b.sd(T3, T4, 16);
  b.exit();
  b.j(0);
  b.fixup_handler(T5);
  s.program = b.p;
  s.program.fault_pc = s.fault_pc;
  s.program.marker_pc = s.marker_pc;
  s.program.handler_pc = b.p.handler_pc;
  return s;
}

// ============================================================================
// The one memory: data and instruction
// ============================================================================
class Mem {
 public:
  explicit Mem(const Program& p) : ram_(kRamSize, 0) {
    // Everything not written is ECALL, so a runaway fetch stops cleanly.
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
    const uint64_t base = addr & ~UINT64_C(7);
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
    const uint64_t base = addr & ~UINT64_C(7);
    for (unsigned i = 0; i < 8; i++) {
      if (((wstrb >> i) & 1u) == 0) continue;
      const uint64_t off = base + i - kRamBase;
      if (off < ram_.size()) ram_[off] = static_cast<uint8_t>((wdata >> (8 * i)) & 0xFF);
    }
  }
  bool InRange(uint64_t addr) const {
    return addr >= kRamBase && addr - kRamBase < ram_.size();
  }

 private:
  std::vector<uint8_t> ram_;
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
// Data memory: the memory system's side of the data port, with the device and
// its access log attached
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
    uint32_t size = 0;         // the size code the port carries
    uint32_t wstrb = 0;
    uint64_t wdata = 0;
    bool dev = false;
    uint64_t id = 0;
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
  Device* device() const { return dev_; }

 private:
  struct Entry {
    Rsp rsp;
    int left = 0;
  };

  Rsp Perform(const Txn& t) {
    Rsp rsp;
    if (t.size > 3) {
      rsp.fault = true;
      return rsp;
    }
    const unsigned size = SizeBytes(t.size);
#ifdef MOSAIC_MMIO_MODEL_MUTANT_STATUS_AS_SCRATCH
    // HARNESS-SIDE CONTROL (not an RTL mutation): a STATUS read is *reported* at
    // SCRATCH's address. The two registers hold the same word from the locked
    // events, so the read data and every published value are identical; only the
    // address in the access log differs.
    const uint64_t log_addr = (!t.we && t.addr == kUartStatus) ? kUartScratch : t.addr;
#else
    const uint64_t log_addr = t.addr;
#endif
    if (IsDevice(t.addr)) {
      uint64_t window = 0;
      const bool performed =
          dev_->Perform(t.addr, t.we, size, t.wstrb, t.wdata, &window, log_addr);
      rsp.fault = !performed;
      rsp.rdata = performed ? window : 0;
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
    rsp.rdata = mem_->Window(t.addr & ~UINT64_C(7));
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
// The independent reference: an RV64IM_Zicsr interpreter for the programs
// above, dispatching loads and stores to its own device from the same locked
// events and to its own RAM.
// ============================================================================
// The core's own physical-memory store map, as mosaic_core.sv's
// `store_fault_kind` implements it: a store whose whole extent is not inside one
// writable region is refused at dispatch, before memory, and the device never
// sees it. boot_rom and test_harness are readable and not writable.
bool StoreWritable(uint64_t addr, unsigned size) {
  struct Region {
    uint64_t base;
    uint64_t size;
    bool writable;
  };
  static const Region kRegions[] = {
      {kBootRomBase, kBootRomSize, false},
      {kUartBase, kUartSize, true},
      {kHarnessBase, kHarnessSize, false},
      {kClintBase, kClintSize, true},
      {kRamBase, kRamSize, true},
  };
  for (const Region& r : kRegions) {
    if (addr >= r.base && addr - r.base < r.size) {
      return r.writable && ((addr - r.base) + size) <= r.size;
    }
  }
  return false;
}

struct RefInsn {
  uint64_t pc = 0;
  uint32_t rd = 0;
  bool reg_we = false;
  uint64_t value = 0;
};

struct RefResult {
  std::vector<RefInsn> trace;      // retirements only; a trapping instruction is not one
  std::vector<uint64_t> trap_causes;
  std::vector<uint64_t> trap_pcs;
  uint64_t sig[10] = {0};
  uint64_t mem_accesses = 0;   // every non-device access presented to the memory
  uint64_t ram_accesses = 0;   // the subset that is ordinary RAM
  bool exited = false;
};

RefResult RunReference(const Program& p, Device* dev) {
  RefResult out;
  Mem mem(p);
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
    uint64_t cause = 0;
    uint64_t tval = 0;
    uint32_t dst_rd = rd;

    if (op == 0x37u) {                                   // LUI
      value = static_cast<uint64_t>(static_cast<int64_t>(imm_u));
      reg_we = true;
    } else if (op == 0x17u) {                            // AUIPC
      value = pc + static_cast<uint64_t>(static_cast<int64_t>(imm_u));
      reg_we = true;
    } else if (op == 0x13u && f3 == 0x0u) {              // ADDI
      value = regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_i));
      reg_we = (rd != 0);
    } else if (op == 0x03u) {                            // LOAD
      unsigned size = 0;
      bool sign = false;
      switch (f3) {
        case 0x0u: size = 1; sign = true; break;
        case 0x1u: size = 2; sign = true; break;
        case 0x2u: size = 4; sign = true; break;
        case 0x3u: size = 8; sign = true; break;
        case 0x4u: size = 1; sign = false; break;
        case 0x5u: size = 2; sign = false; break;
        case 0x6u: size = 4; sign = false; break;
        default: Fail("reference", "unsupported load encoding");
      }
      const uint64_t addr =
          regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_i));
      if (addr % size != 0) {
        trapped = true;
        cause = kExcLoadMisaligned;
        tval = addr;
      } else if (IsDevice(addr)) {
        uint64_t window = 0;
        if (!dev->Perform(addr, false, size, ExpectedWstrb(size, addr), 0, &window)) {
          trapped = true;
          cause = kExcLoadAccess;
          tval = addr;
        } else {
          value = Extract(window, addr, size, sign);
          reg_we = true;
        }
      } else {
        // A load has no dispatch-time region check: it is always presented to
        // the memory, which either serves it (RAM) or faults it (no region).
        out.mem_accesses++;
        if (mem.InRange(addr)) {
          value = Extract(mem.Window(addr & ~UINT64_C(7)), addr, size, sign);
          reg_we = true;
          out.ram_accesses++;
        } else {
          trapped = true;
          cause = kExcLoadAccess;
          tval = addr;
        }
      }
      if (!trapped) reg_we = (rd != 0);
    } else if (op == 0x23u) {                            // STORE
      dst_rd = 0;
      unsigned size = 0;
      switch (f3) {
        case 0x0u: size = 1; break;
        case 0x1u: size = 2; break;
        case 0x2u: size = 4; break;
        case 0x3u: size = 8; break;
        default: Fail("reference", "unsupported store encoding");
      }
      const uint64_t addr =
          regs[rs1] + static_cast<uint64_t>(static_cast<int64_t>(imm_s));
      const uint64_t wdata = regs[rs2] << (8 * (addr & 7u));
      const uint32_t wstrb = ExpectedWstrb(size, addr);
      if (addr % size != 0) {
        trapped = true;
        cause = kExcStoreMisaligned;
        tval = addr;
      } else if (!StoreWritable(addr, size)) {
        // The core's dispatch-time map: the store is refused before memory, so
        // the device never sees it.
        trapped = true;
        cause = kExcStoreAccess;
        tval = addr;
      } else if (IsDevice(addr)) {
        uint64_t window = 0;
        if (!dev->Perform(addr, true, size, wstrb, wdata, &window)) {
          trapped = true;
          cause = kExcStoreAccess;
          tval = addr;
        }
      } else {
        // Past the dispatch-time map, a non-device writable address is RAM.
        out.mem_accesses++;
        out.ram_accesses++;
        mem.StoreWindow(addr, wstrb, wdata);
      }
    } else if (op == 0x33u && f3 == 0x4u && f7 == 0x01u) {  // DIV (signed)
      const int64_t a = static_cast<int64_t>(regs[rs1]);
      const int64_t b = static_cast<int64_t>(regs[rs2]);
      int64_t q = 0;
      if (b != 0) q = (a == INT64_MIN && b == -1) ? INT64_MIN : a / b;
      value = static_cast<uint64_t>(q);
      reg_we = (rd != 0);
    } else if (op == 0x6Fu) {                            // JAL
      next = pc + static_cast<uint64_t>(static_cast<int64_t>(imm_j));
      if (rd != 0) {
        value = pc + 4;
        reg_we = true;
      }
    } else if (op == 0x73u && f3 == 0x0u) {              // ECALL
      trapped = true;
      cause = kExcEcallM;
      tval = 0;
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
      if (f3 == 0x1u) {                                  // csrrw
        if (csr == CSR_MTVEC) csr_mtvec = regs[rs1];
        value = old;
        reg_we = (rd != 0);
      } else {                                           // csrrs (the program reads)
        if (rs1 != 0 && csr == CSR_MTVEC) csr_mtvec |= regs[rs1];
        value = old;
        reg_we = (rd != 0);
      }
    } else {
      Fail("reference", "unsupported instruction word " + U64(w) + " at " + U64(pc));
    }

    if (trapped) {
      out.trap_causes.push_back(cause);
      out.trap_pcs.push_back(pc);
      csr_mepc = pc;
      csr_mcause = cause;
      csr_mtval = tval;
      pc = csr_mtvec;
      continue;
    }

    rec.rd = dst_rd;
    rec.reg_we = reg_we && dst_rd != 0;
    rec.value = value;
    out.trace.push_back(rec);
    if (dst_rd != 0 && reg_we) regs[dst_rd] = value;
    pc = next;

    if (dev->exited()) {
      out.exited = true;
      break;
    }
  }
  for (int i = 0; i < 10; i++) out.sig[i] = mem.Window(kSig + 8 * i);
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

  void SetForbiddenPcs(const std::vector<uint64_t>& pcs) { forbidden_pcs_ = pcs; }
  void SetExpected(const std::vector<RefInsn>* trace) { expected_ = trace; }

  uint64_t cycles() const { return cycles_; }
  const std::vector<Retire>& retires() const { return retires_; }
  const std::vector<Dmem::Txn>& txns() const { return dmem_.txns(); }
  uint64_t device_txns() const { return dmem_.device_txns(); }
  uint64_t ram_txns() const { return dmem_.ram_txns(); }
  const std::vector<uint64_t>& device_ids() const { return device_ids_; }
  uint64_t trap_events() const { return trap_events_; }
  uint64_t trap_event_pc() const { return trap_event_pc_; }
  uint64_t exit_code() const { return exit_code_; }

  void Reset() {
    imem_.Reset();
    dmem_.Reset();
    for (int i = 0; i < kResetCycles; i++) Cycle(true);
    for (unsigned i = 0; i < 8; i++) Cycle(false);
  }

  void Run() {
    while (!exited_) {
      Cycle(false);
      if (cycles_ > max_cycles_) {
        Fail("run", "max-cycles exhausted: the program never wrote the exit register");
      }
    }
  }

  void Check(const std::string& what, bool ok, const std::string& detail) {
    reporter_->Check(ok, what + (ok ? "" : " -- " + detail));
    if (!ok) Fail("cycle " + Dec(cycles_), what + ": " + detail);
  }

  bool saw_txn(uint64_t addr, bool we) const {
    for (const Dmem::Txn& t : dmem_.txns()) {
      if (t.addr == addr && t.we == we) return true;
    }
    return false;
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

    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = kMtimeDriven;

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
      Dmem::Txn t;
      t.we = dut_->dmem_req_we_o != 0;
      t.addr = dut_->dmem_req_addr_o;
      t.size = dut_->dmem_req_size_o;
      t.wstrb = dut_->dmem_req_wstrb_o;
      t.wdata = dut_->dmem_req_wdata_o;
      t.dev = dut_->o_mem_dmem_dev_o != 0;
      t.id = static_cast<uint64_t>(dut_->o_mem_dmem_id_o);
      t.cycle = cycles_;

#ifndef MOSAIC_MMIO_MODEL_RD_ONLY
      // The card's boundary rule, checked on every transaction: the attribute
      // the core presents must be the map's classification of the address -- the
      // signal a coalescer keys on when it refuses to merge MMIO with RAM.
      const bool want_dev = IsDevice(t.addr);
      Check("every access carries the device attribute its region demands",
            t.dev == want_dev,
            "cycle " + Dec(cycles_) + ": access to " + U64(t.addr) + " (" +
                (want_dev ? "a device region" : "ordinary memory") +
                ") presented with device attribute " + Dec(t.dev ? 1 : 0));
#endif
      if (t.dev) {
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
      dmem_.Accept(t);
      if (std::getenv("MOSAIC_MMIO_MODEL_TRACE") != nullptr) {
        std::printf("  [txn] cycle=%llu %s addr=%s size=%u wstrb=%02x wdata=%s dev=%u id=%llu\n",
                    static_cast<unsigned long long>(cycles_), t.we ? "write" : "read ",
                    U64(t.addr).c_str(), t.size, t.wstrb, U64(t.wdata).c_str(),
                    t.dev ? 1u : 0u, static_cast<unsigned long long>(t.id));
      }
      if (dev_exited() && !exited_) {
        exited_ = true;
        exit_code_ = dev_exit_code();
      }
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
    ProgressCheck();
  }

  Device* device() { return dmem_.device(); }

 private:
  bool dev_exited() const { return dmem_.device()->exited(); }
  uint64_t dev_exit_code() const { return dmem_.device()->exit_code(); }

  void Observe() {
    const uint32_t ev_mask = static_cast<uint32_t>(dut_->ev_valid_o);
    if (ev_mask == 0) return;
    for (uint32_t lane = 0; lane < 2; lane++) {
      if ((ev_mask & (1u << lane)) == 0) continue;
      const bool is_trap = PackedLane(dut_->ev_trap_o, lane, 1) != 0;
      if (is_trap) {
        trap_events_++;
        trap_event_pc_ = PayloadLane(dut_->ev_pc_o, lane);
        if (std::getenv("MOSAIC_MMIO_MODEL_TRACE") != nullptr) {
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
               "the access did not trap: the instruction at " + U64(r.pc) +
                   " retires instead of trapping");
        }
      }
      r.value = PayloadLane(dut_->ev_value_o, lane);
      r.rd = static_cast<uint32_t>(PackedLane(dut_->ev_rd_o, lane, 5));
      r.reg_we = PackedLane(dut_->ev_reg_we_o, lane, 1) != 0;
      r.cycle = cycles_;
      if (std::getenv("MOSAIC_MMIO_MODEL_TRACE") != nullptr) {
        std::printf("  [retire] cycle=%llu pc=%s rd=%u we=%u val=%s\n",
                    static_cast<unsigned long long>(cycles_), U64(r.pc).c_str(), r.rd,
                    r.reg_we ? 1u : 0u, U64(r.value).c_str());
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

  std::string Diagnose() const {
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
    const uint64_t mask = (width >= 64) ? ~UINT64_C(0) : ((UINT64_C(1) << width) - 1);
    return (packed >> (lane * width)) & mask;
  }

  Vmosaic_core_tb* dut_;
  mosaic::Reporter* reporter_;
  uint64_t max_cycles_;
  Imem imem_;
  Dmem dmem_;
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
};

// ============================================================================
// The checks
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

// The device accesses the program made, in order, field by field. This is the
// check that keeps the case from being a data comparison: an access whose read
// data is the same is still a different access if its address differs.
void CheckDeviceLog(Harness* harness, const Device* dut_dev, const Device* ref_dev) {
#ifndef MOSAIC_MMIO_MODEL_RD_ONLY
  const std::vector<Device::Access>& d = dut_dev->accesses();
  const std::vector<Device::Access>& r = ref_dev->accesses();
  harness->Check("the device accesses the program made follow the reference, in order: "
                 "device " + Dec(d.size()) + ", reference " + Dec(r.size()),
                 d.size() == r.size(), "counts differ");
  const size_t n = (d.size() < r.size()) ? d.size() : r.size();
  for (size_t i = 0; i < n; i++) {
    if (d[i].addr != r[i].addr) {
      Fail("run", "the device accesses the program made follow the reference, in order: "
                  "access " + Dec(i) + " address expected " + U64(r[i].addr) + ", got " +
                      U64(d[i].addr));
    }
    if (d[i].we != r[i].we || d[i].size != r[i].size || d[i].wstrb != r[i].wstrb ||
        d[i].wdata != r[i].wdata || d[i].performed != r[i].performed) {
      Fail("run",
           "the device accesses the program made follow the reference, in order: "
           "access " + Dec(i) + " at " + U64(r[i].addr) + " expected " +
               (r[i].we ? "write" : "read") + "/size " + Dec(r[i].size) + "/wstrb " +
               U64(r[i].wstrb) + "/wdata " + U64(r[i].wdata) + "/performed " +
               Dec(r[i].performed ? 1 : 0) + ", got " + (d[i].we ? "write" : "read") +
               "/size " + Dec(d[i].size) + "/wstrb " + U64(d[i].wstrb) + "/wdata " +
               U64(d[i].wdata) + "/performed " + Dec(d[i].performed ? 1 : 0));
    }
  }
  harness->Check("every device access carries the reference's address, size and byte "
                 "enables",
                 true, "");

  // The side effects, exactly once each, in order.
  const std::vector<Device::Effect>& de = dut_dev->effects();
  const std::vector<Device::Effect>& re = ref_dev->effects();
  harness->Check("the device performed exactly the reference's side effects, in order: "
                 "device " + Dec(de.size()) + ", reference " + Dec(re.size()),
                 de.size() == re.size(), "counts differ");
  const size_t en = (de.size() < re.size()) ? de.size() : re.size();
  for (size_t i = 0; i < en; i++) {
    if (de[i].kind != re[i].kind || de[i].addr != re[i].addr || de[i].value != re[i].value) {
      Fail("run",
           "every side effect happens exactly once with the reference's order and "
           "value: effect " + Dec(i) + " (" + EffectName(re[i].kind) + " at " +
               U64(re[i].addr) + " value " + U64(re[i].value) + ") got (" +
               EffectName(de[i].kind) + " at " + U64(de[i].addr) + " value " +
               U64(de[i].value) + ")");
    }
  }

  harness->Check("the modelled device state equals the reference's: SCRATCH",
                 dut_dev->scratch() == ref_dev->scratch(),
                 U64(dut_dev->scratch()) + " vs " + U64(ref_dev->scratch()));
  harness->Check("the modelled device state equals the reference's: MTIMECMP",
                 dut_dev->mtimecmp() == ref_dev->mtimecmp(),
                 U64(dut_dev->mtimecmp()) + " vs " + U64(ref_dev->mtimecmp()));
  harness->Check("the modelled device state equals the reference's: UART output",
                 dut_dev->uart() == ref_dev->uart(), "uart streams differ");
  harness->Check("the modelled device state equals the reference's: exit code",
                 dut_dev->exit_code() == ref_dev->exit_code(),
                 Dec(dut_dev->exit_code()) + " vs " + Dec(ref_dev->exit_code()));
#endif
}

// The core's own classification, the memory system's address-based count, and
// the partition of every data-port transaction. `core_only_dev`/`core_only_ram`
// are the accesses the endpoint *accepted* and then refused before the memory
// saw them (a misaligned access): the core counts them, the memory does not.
void CheckCounters(Vmosaic_core_tb* dut, Harness* harness, uint64_t core_only_dev,
                   uint64_t core_only_ram) {
#ifndef MOSAIC_MMIO_MODEL_RD_ONLY
  const uint64_t seen_dev = harness->device_txns();
  const uint64_t seen_ram = harness->ram_txns();
  harness->Check("the core's device-transaction count equals the memory system's "
                 "address-based device count plus the accesses it refused before "
                 "memory: core " + Dec(dut->o_mem_dev_txn_o) + ", memory " +
                     Dec(seen_dev) + " + " + Dec(core_only_dev),
                 dut->o_mem_dev_txn_o == seen_dev + core_only_dev, "counts differ");
  harness->Check("the core's ordinary-transaction count equals the memory system's "
                 "address-based count: core " + Dec(dut->o_mem_ram_txn_o) + ", memory " +
                     Dec(seen_ram) + " + " + Dec(core_only_ram),
                 dut->o_mem_ram_txn_o == seen_ram + core_only_ram, "counts differ");
  harness->Check("the device and ordinary counters partition every transaction: " +
                     Dec(dut->o_mem_dev_txn_o + dut->o_mem_ram_txn_o) + " vs " +
                     Dec(harness->txns().size() + core_only_dev + core_only_ram),
                 dut->o_mem_dev_txn_o + dut->o_mem_ram_txn_o ==
                     harness->txns().size() + core_only_dev + core_only_ram,
                 "counts differ");
#endif
}

// ============================================================================
// main
// ============================================================================
struct RunStats {
  uint64_t retires = 0;
  uint64_t cycles = 0;
  uint64_t device_txns = 0;
  uint64_t ram_txns = 0;
  uint64_t side_effects = 0;
  uint64_t traps = 0;
};

// The checks every run shares: the retirement stream, the device access log, the
// side-effect log and the core's own classification.
void CheckCommon(Vmosaic_core_tb* dut, Harness* h, const Device& dut_dev,
                 const Device& ref_dev, const RefResult& ref, const LockedEvents& ev,
                 uint64_t core_only_dev = 0, uint64_t core_only_ram = 0) {
  CheckStream(h, ref);
  CheckDeviceLog(h, &dut_dev, &ref_dev);
  CheckCounters(dut, h, core_only_dev, core_only_ram);
  h->Check("no macro was refused as unsupported: " + Dec(dut->o_unsupported_o),
           dut->o_unsupported_o == 0, "unsupported " + Dec(dut->o_unsupported_o));
  h->Check("the program wrote the exit register with the locked pass code: " +
               Dec(dut_dev.exit_code()),
           dut_dev.exit_code() == ev.exit_value, "exit code " + Dec(dut_dev.exit_code()));
}

// Run A: the device contract on the correct path.
RunStats RunContract(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                     const mosaic::Options& options, const LockedEvents& ev) {
  RunStats stats;
  const Program p = BuildContractProgram();
  Mem mem(p);
  Device dut_dev(ev);
  Device ref_dev(ev);
  const RefResult ref = RunReference(p, &ref_dev);

  reporter->Check(ref.exited, "contract: the reference reaches the exit protocol");
  reporter->Check(ref.trap_causes.empty(), "contract: the reference takes no trap");
  if (!ref.exited || !ref.trap_causes.empty()) {
    Fail("harness", "contract: the reference did not run the correct path");
  }

  Harness h(dut, reporter, options.max_cycles, &mem, &dut_dev);
  h.SetExpected(&ref.trace);
  h.Reset();
  h.Run();
  stats.retires = h.retires().size();
  stats.cycles = h.cycles();
  stats.device_txns = h.device_txns();
  stats.ram_txns = h.ram_txns();
  stats.side_effects = dut_dev.effects().size();
  stats.traps = h.trap_events();

  CheckCommon(dut, &h, dut_dev, ref_dev, ref, ev);

  // The run's own expectations, none read from the DUT.
  h.Check("the correct path takes no trap (got " + Dec(h.trap_events()) + ")",
          h.trap_events() == 0, "trap events " + Dec(h.trap_events()));
  h.Check("two destructive reads are two pops with the two locked values",
          ref_dev.pops() == 2 && ref.sig[0] == ev.rx[0] && ref.sig[1] == ev.rx[1],
          "pops " + Dec(ref_dev.pops()));
  h.Check("a destructive read is not idempotent: the two values differ",
          ref.sig[0] != ref.sig[1], U64(ref.sig[0]) + " == " + U64(ref.sig[1]));
  h.Check("an idempotent read returns the same word twice: " + U64(ref.sig[2]),
          ref.sig[2] == ref.sig[3] && ref.sig[2] == ev.status,
          U64(ref.sig[2]) + " / " + U64(ref.sig[3]));
  h.Check("the readable test-harness window is idempotent: " + U64(ref.sig[4]),
          ref.sig[4] == ref.sig[5] && ref.sig[4] == ev.harness_id,
          U64(ref.sig[4]) + " / " + U64(ref.sig[5]));
  h.Check("the timer reads return the same locked value twice: " + U64(ref.sig[6]),
          ref.sig[6] == ref.sig[7] && ref.sig[6] == ev.mtime,
          U64(ref.sig[6]) + " / " + U64(ref.sig[7]));
  // The byte at offset 3 then the halfword at offset 0: only the strobed bytes
  // may change, and the full-word overwrite then replaces all four.
  h.Check("a byte and a halfword write change only the bytes their strobes select",
          ref.sig[8] == 0x77001234ull && mem.Window(kSig + 64) == 0x77001234ull,
          "reference " + U64(ref.sig[8]) + ", memory " + U64(mem.Window(kSig + 64)));
  h.Check("a full-word write replaces all four bytes",
          ref.sig[9] == 0x0000BEEFull && mem.Window(kSig + 72) == 0x0000BEEFull,
          "reference " + U64(ref.sig[9]) + ", memory " + U64(mem.Window(kSig + 72)));
  h.Check("the UART received the transmitted bytes in order: '" + dut_dev.uart() + "'",
          dut_dev.uart() == "AB", "uart has " + Dec(dut_dev.uart().size()) + " bytes");
  bool published = true;
  std::string where;
  for (int i = 0; i < 10; i++) {
    if (mem.Window(kSig + 8 * i) != ref.sig[i]) {
      published = false;
      where = "word " + Dec(i) + " memory " + U64(mem.Window(kSig + 8 * i)) +
              ", reference " + U64(ref.sig[i]);
      break;
    }
  }
  h.Check("every published word equals the reference's", published, where);
  h.Check("the non-device data-port traffic equals the reference's presented accesses: core " +
              Dec(h.ram_txns()) + ", reference " + Dec(ref.mem_accesses),
          h.ram_txns() == ref.mem_accesses,
          Dec(h.ram_txns()) + " vs " + Dec(ref.mem_accesses));
  return stats;
}

// Run B: an access the wrong path cancels has no side effect.
RunStats RunCancelled(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                      const mosaic::Options& options, const LockedEvents& ev) {
  RunStats stats;
  const Program p = BuildCancelledProgram();
  Mem mem(p);
  Device dut_dev(ev);
  Device ref_dev(ev);
  const RefResult ref = RunReference(p, &ref_dev);

  reporter->Check(ref.exited, "cancelled: the reference reaches the exit protocol");
  reporter->Check(ref.trap_causes.size() == 1 && ref.trap_causes[0] == kExcEcallM,
                  "cancelled: the reference takes exactly the ecall's trap");
  if (!ref.exited || ref.trap_causes.size() != 1) {
    Fail("harness", "cancelled: the reference did not run the scene");
  }

  Harness h(dut, reporter, options.max_cycles, &mem, &dut_dev);
  h.SetExpected(&ref.trace);
  h.Reset();
  h.Run();
  stats.retires = h.retires().size();
  stats.cycles = h.cycles();
  stats.device_txns = h.device_txns();
  stats.ram_txns = h.ram_txns();
  stats.side_effects = dut_dev.effects().size();
  stats.traps = h.trap_events();

  CheckCommon(dut, &h, dut_dev, ref_dev, ref, ev);

  h.Check("the wrong-path ordinary load reached memory (the window is real)",
          h.saw_txn(kSpecRam, false), "no read of " + U64(kSpecRam));
  h.Check("ordinary traffic exceeds the retired accesses by the speculative one: core " +
              Dec(h.ram_txns()) + ", reference " + Dec(ref.mem_accesses),
          h.ram_txns() > ref.mem_accesses,
          Dec(h.ram_txns()) + " vs " + Dec(ref.mem_accesses));
  h.Check("the cancelled device accesses performed no side effect: the device popped " +
              Dec(dut_dev.pops()) + " time(s), the program pops " + Dec(ref_dev.pops()),
          dut_dev.pops() == ref_dev.pops() && dut_dev.pops() == 1,
          "pops " + Dec(dut_dev.pops()) + " vs " + Dec(ref_dev.pops()));
  h.Check("no device access to the wrong-path read target was presented",
          !h.saw_txn(kUartStatus, false), "a read of " + U64(kUartStatus) + " appeared");
  h.Check("the wrong-path device store transmitted nothing: '" + dut_dev.uart() + "'",
          dut_dev.uart().empty() && dut_dev.tx_accesses() == 0,
          "uart has " + Dec(dut_dev.uart().size()) + " bytes, " +
              Dec(dut_dev.tx_accesses()) + " transmit accesses");
  h.Check("the non-speculation gate refused device offers (dev_wait=" +
              Dec(dut->o_mem_dev_wait_o) + ")",
          dut->o_mem_dev_wait_o > 0, "dev_wait=" + Dec(dut->o_mem_dev_wait_o));
  h.Check("exactly one trap event is taken (got " + Dec(h.trap_events()) + ")",
          h.trap_events() == 1, "trap events " + Dec(h.trap_events()));
  h.Check("the trap event names the ecall",
          h.trap_event_pc() == p.fault_pc,
          U64(h.trap_event_pc()) + " vs " + U64(p.fault_pc));
  h.Check("the handler published mepc = the ecall's PC",
          mem.Window(kSig) == p.fault_pc,
          U64(mem.Window(kSig)) + " vs " + U64(p.fault_pc));
  h.Check("the handler published mcause = 11 (ecall from M)",
          mem.Window(kSig + 8) == kExcEcallM, U64(mem.Window(kSig + 8)));
  return stats;
}

// Runs C..G: each trap scene, with the trap's precision and the absence of a
// side effect from the faulting access.
RunStats RunTrapScene(Vmosaic_core_tb* dut, mosaic::Reporter* reporter,
                      const mosaic::Options& options, const LockedEvents& ev,
                      TrapKind kind) {
  RunStats stats;
  const Scene s = BuildTrapScene(kind);
  Mem mem(s.program);
  Device dut_dev(ev);
  Device ref_dev(ev);
  const RefResult ref = RunReference(s.program, &ref_dev);
  const std::string tag = s.name;

  reporter->Check(ref.exited, tag + ": the reference reaches the exit protocol");
  reporter->Check(ref.trap_causes.size() == 1, tag + ": the reference takes one trap");
  if (!ref.exited || ref.trap_causes.size() != 1) {
    Fail("harness", tag + ": the reference did not run the scene");
  }
  reporter->Check(ref.trap_causes[0] == s.expect_cause,
                  tag + ": the reference's trap cause is " + Dec(s.expect_cause));
  reporter->Check(ref.trap_pcs[0] == s.fault_pc,
                  tag + ": the reference's trap names the faulting access");

  Harness h(dut, reporter, options.max_cycles, &mem, &dut_dev);
  h.SetExpected(&ref.trace);
  h.SetForbiddenPcs({s.marker_pc, s.fault_pc});
  h.Reset();
  h.Run();
  stats.retires = h.retires().size();
  stats.cycles = h.cycles();
  stats.device_txns = h.device_txns();
  stats.ram_txns = h.ram_txns();
  stats.side_effects = dut_dev.effects().size();
  stats.traps = h.trap_events();

  CheckCommon(dut, &h, dut_dev, ref_dev, ref, ev, s.core_only_dev, 0);

  h.Check(tag + ": exactly one trap event is taken (got " + Dec(h.trap_events()) + ")",
          h.trap_events() == 1, "trap events " + Dec(h.trap_events()));
  h.Check(tag + ": the trap event names the faulting access",
          h.trap_event_pc() == s.fault_pc,
          U64(h.trap_event_pc()) + " vs " + U64(s.fault_pc));
  h.Check(tag + ": the handler published mepc = the faulting access's PC",
          mem.Window(kSig) == s.fault_pc,
          U64(mem.Window(kSig)) + " vs " + U64(s.fault_pc));
  h.Check(tag + ": the handler published mcause = " + Dec(s.expect_cause),
          mem.Window(kSig + 8) == s.expect_cause, U64(mem.Window(kSig + 8)));
  h.Check(tag + ": the handler published mtval = " + U64(s.expect_tval),
          mem.Window(kSig + 16) == s.expect_tval, U64(mem.Window(kSig + 16)));
  h.Check(tag + ": the faulting access performed no side effect (the only side effect "
                  "is the exit write)",
          ref_dev.effects().size() == 1 && dut_dev.effects().size() == 1,
          "device " + Dec(dut_dev.effects().size()) + ", reference " +
              Dec(ref_dev.effects().size()));
  h.Check(tag + ": the non-device data-port traffic equals the reference's presented "
                "accesses",
          h.ram_txns() == ref.mem_accesses,
          Dec(h.ram_txns()) + " vs " + Dec(ref.mem_accesses));
  return stats;
}

std::string Summarise(const std::vector<std::pair<std::string, RunStats>>& runs) {
  std::string text;
  for (const auto& entry : runs) {
    if (!text.empty()) text += " ";
    text += entry.first + "(retires=" + Dec(entry.second.retires) +
            ",dev=" + Dec(entry.second.device_txns) +
            ",ram=" + Dec(entry.second.ram_txns) +
            ",effects=" + Dec(entry.second.side_effects) +
            ",traps=" + Dec(entry.second.traps) +
            ",cycles=" + Dec(entry.second.cycles) + ")";
  }
  return text;
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
  std::vector<std::pair<std::string, RunStats>> runs;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    const LockedEvents events;

    runs.emplace_back("contract", RunContract(&dut, &reporter, options, events));
    runs.emplace_back("cancelled", RunCancelled(&dut, &reporter, options, events));
    runs.emplace_back("illegal_width",
                      RunTrapScene(&dut, &reporter, options, events, TrapKind::kIllegalWidth));
    runs.emplace_back("unmapped_read",
                      RunTrapScene(&dut, &reporter, options, events, TrapKind::kUnmappedRead));
    runs.emplace_back("unmodelled_reg",
                      RunTrapScene(&dut, &reporter, options, events, TrapKind::kUnmodelledReg));
    runs.emplace_back("misaligned",
                      RunTrapScene(&dut, &reporter, options, events, TrapKind::kMisalignedRead));
    runs.emplace_back("unmapped_write",
                      RunTrapScene(&dut, &reporter, options, events, TrapKind::kUnmappedWrite));
    runs.emplace_back("harness_write",
                      RunTrapScene(&dut, &reporter, options, events, TrapKind::kHarnessWrite));

    detail = "checks=" + Dec(reporter.checks()) + " " + Summarise(runs) + " seed=" +
             Dec(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "the MMIO device contract holds", "contract violated");
    passed = false;
    detail = "contract violated: " + f.what;
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
