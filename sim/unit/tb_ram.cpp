// ============================================================================
// tb_ram.cpp -- CASE=ram.collision_matrix, work package I-006.
//
// The point of this testbench is that the DUT is never its own oracle. Every
// value the RAM returns is compared against an independent C++ shadow array
// with little-endian byte packing, maintained from the stimulus this file
// generated. The shadow models the *contract* documented in
// rtl/common/mosaic_ram.sv, not the RTL:
//
//   * a read issued in cycle N returns the array as it was at the START of
//     cycle N -- so a same-address same-cycle write is read-first, byte by
//     byte;
//   * `rvalid` is high in cycle N+1 if and only if `rst` was low in cycle N;
//   * a write offered while `rst` is high is dropped.
//
// Two checks run on every cycle of the campaign: a latency check (valid is
// exactly one cycle behind reset) and a data check against the shadow. The
// targeted phases below add named checks for each case the card enumerates.
// ============================================================================

#include <verilated.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "sim_common.h"
#include "Vmosaic_ram_tb.h"

namespace {

// Geometry mirrored by sim/tb/mosaic_ram_tb.sv. The wrapper fails to
// elaborate if the two ever disagree, so these constants cannot silently drift
// away from the DUT.
constexpr int      kDataWidth = 64;
constexpr int      kBytes     = 8;
constexpr int      kDepth     = 64;
constexpr int      kAddrWidth = 6;
constexpr uint8_t  kBankBXor  = 0x2a;  // bank B's fixed address permutation

static_assert(kDepth == (1 << kAddrWidth), "address width must match depth");
static_assert(kDataWidth == kBytes * 8, "byte count must match data width");

// Thrown on the first failed check. The run stops there so the report shows one
// defect rather than a thousand consequences of it.
struct Failure {
  std::string what;
};

// Little-endian masked merge: the shadow's only write rule.
uint64_t MergeBytes(uint64_t old_value, uint64_t new_value, uint8_t mask) {
  uint64_t result = old_value;
  for (int b = 0; b < kBytes; ++b) {
    if (mask & (1u << b)) {
      const uint64_t byte_mask = 0xffull << (8 * b);
      result = (result & ~byte_mask) | (new_value & byte_mask);
    }
  }
  return result;
}

// Non-zero patterns throughout, so a mutant that zeroes the array is always
// observable rather than accidentally matching.
uint64_t ValueOf(uint32_t index, uint32_t tag) {
  return 0xa5a5f00d5a5a0000ull ^ (static_cast<uint64_t>(index) * 0x0000000100000001ull) ^
         (static_cast<uint64_t>(tag) << 17);
}

// ---------------------------------------------------------------------------
// One cycle of stimulus.
struct Stim {
  uint8_t  waddr = 0;
  uint64_t wdata = 0;
  uint8_t  wmask = 0;
  bool     we    = false;
  uint8_t  raddr = 0;
};

// What the read issued in the previous cycle promised to return.
struct Pending {
  bool     valid   = false;  // rst was low when it was issued
  bool     compare = false;  // every entry has been written at least once
  uint64_t a       = 0;
  uint64_t b       = 0;
};

class RamHarness {
 public:
  RamHarness(Vmosaic_ram_tb* dut, mosaic::ClockDriver* clk, mosaic::Reporter* rep,
             uint64_t max_cycles)
      : dut_(dut), clk_(clk), rep_(rep), max_cycles_(max_cycles) {
    std::memset(shadow_a_, 0, sizeof(shadow_a_));
    std::memset(shadow_b_, 0, sizeof(shadow_b_));
  }

  void Phase(const char* name) { phase_ = name; }
  // Assert `rst` for `cycles` rising edges. A write is offered on every reset
  // cycle, which pins down the rule that a write offered during reset is
  // dropped rather than committed.
  void Reset(int cycles) {
    for (int i = 0; i < cycles; ++i) {
      Cycle(Stim{0, ValueOf(0, 0xbadu), 0xff, true, 0}, /*rst=*/true);
    }
  }


  // Drive one full clock period and check the contract.
  //
  // Returns the value the read issued *in this cycle* returned, i.e. the
  // contents of `rdata` after this call. Throws Failure on any violation.
  uint64_t Cycle(const Stim& s, bool rst) {
    if (cycles_ >= max_cycles_) {
      Fail(std::string(phase_) + ": max-cycles (" + std::to_string(max_cycles_) +
               ") exhausted before the campaign finished",
           "campaign fits in the cycle budget", "ran out of cycles");
    }

    // --- present this cycle's stimulus while the clock is low ---------------
    dut_->clk   = 0;
    dut_->rst   = rst ? 1 : 0;
    dut_->waddr = s.waddr;
    dut_->wdata = s.wdata;
    dut_->wmask = s.wmask;
    dut_->we    = s.we ? 1 : 0;
    dut_->raddr = s.raddr;
    dut_->eval();

    // --- the outputs visible now belong to the PREVIOUS cycle's read --------
    // This is the latency check. `rvalid` must be exactly one cycle behind
    // `rst`, and the data must be what the shadow held before that cycle's
    // write landed.
    if (have_pending_) {
      CheckValid(dut_->rvalid_a != 0, pending_.valid, "rvalid_a");
      CheckValid(dut_->rvalid_b != 0, pending_.valid, "rvalid_b");
      if (pending_.valid && pending_.compare) {
        if (dut_->rdata_a != pending_.a) {
          Fail(Where() + ": bank A read data", mosaic::Hex(pending_.a),
               mosaic::Hex(dut_->rdata_a));
        }
        if (dut_->rdata_b != pending_.b) {
          Fail(Where() + ": bank B read data", mosaic::Hex(pending_.b),
               mosaic::Hex(dut_->rdata_b));
        }
        ++data_checks_;
      }
    }

    // --- this cycle's read, evaluated against the array before the write ----
    Pending next;
    next.valid   = !rst;
    next.compare = data_check_;
    next.a       = shadow_a_[s.raddr];
    next.b       = shadow_b_[s.raddr ^ kBankBXor];
    pending_     = next;
    have_pending_ = true;

    // --- the write lands at this edge ---------------------------------------
    if (!rst && s.we) {
      shadow_a_[s.waddr] = MergeBytes(shadow_a_[s.waddr], s.wdata, s.wmask);
      shadow_b_[s.waddr ^ kBankBXor] =
          MergeBytes(shadow_b_[s.waddr ^ kBankBXor], s.wdata, s.wmask);
    }

    // --- rising edge --------------------------------------------------------
    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;
    return dut_->rdata_a;
  }

  // From here on the shadow predicts every returned value, so the array's
  // undefined power-up contents can neither mask a defect nor create one.
  void EnableDataChecks() { data_check_ = true; }

  void Write(uint8_t addr, uint64_t value, uint8_t mask = 0xff, uint8_t read_addr = 0) {
    Cycle(Stim{addr, value, mask, true, read_addr}, false);
  }

  uint64_t Read(uint8_t addr) {
    const uint64_t expected = shadow_a_[addr];
    const uint64_t got = Cycle(Stim{0, 0, 0, false, addr}, false);
    if (!data_check_) return got;
    Check(got == expected, "read of addr " + std::to_string(addr) + " returned " +
                               mosaic::Hex(got) + ", shadow holds " +
                               mosaic::Hex(expected));
    return got;
  }

  // Same-address same-cycle read/write. Returns the value the read returned;
  // the contract says it is the value from before the write.
  uint64_t Collide(uint8_t addr, uint64_t value, uint8_t mask) {
    const uint64_t expected = shadow_a_[addr];
    const uint64_t got = Cycle(Stim{addr, value, mask, true, addr}, false);
    Check(got == expected,
          "same-address read/write at addr " + std::to_string(addr) + " with mask " +
              mosaic::Hex(mask, 2) + " returned " + mosaic::Hex(got) +
              " (post-write), shadow holds " + mosaic::Hex(expected) + " (pre-write)");
    return got;
  }

  uint64_t cycles() const { return cycles_; }
  uint64_t data_checks() const { return data_checks_; }

 private:
  std::string Where() const {
    return std::string(phase_) + ": cycle " + std::to_string(cycles_ + 1);
  }

  void Check(bool ok, const std::string& what) {
    if (ok) return;
    Fail(Where() + ": " + what, "contract holds", "contract violated");
  }

  void CheckValid(bool actual, bool expected, const std::string& signal) {
    if (actual == expected) return;
    Fail(Where() + ": " + signal, expected ? "high" : "low", actual ? "high" : "low");
  }

  void Fail(const std::string& what, const std::string& expected,
            const std::string& actual) {
    rep_->Mismatch(what, expected, actual);
    rep_->Check(false, what);
    throw Failure{what};
  }

  Vmosaic_ram_tb*      dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter*    rep_;
  uint64_t             max_cycles_;

  uint64_t    shadow_a_[kDepth];
  uint64_t    shadow_b_[kDepth];
  Pending     pending_;
  bool        have_pending_ = false;
  bool        data_check_   = false;
  uint64_t    cycles_       = 0;
  uint64_t    data_checks_  = 0;
  const char* phase_        = "startup";
};

// ---------------------------------------------------------------------------
// Phase 1: reset, then write a distinct known pattern into every entry of both
// banks (bank B is written by the same cycle through its permuted address, so
// one sweep covers both). Nothing is compared until the array holds no
// undefined contents; after this, every later phase is compared.
//
// The first compared cycle is deliberately the headline demonstration of the
// register boundary: one same-address read/write cycle, which must return the
// value from before the write. A RAM that reads combinationally returns the
// new value here and nowhere else can hide it.
void PhaseInitialise(RamHarness* h) {
  h->Phase("init");
  h->Reset(4);
  for (int addr = 0; addr < kDepth; ++addr) {
    const uint8_t a = static_cast<uint8_t>(addr);
    h->Write(a, ValueOf(static_cast<uint32_t>(addr), 0x000u), 0xff, a);
  }
  h->EnableDataChecks();
  // Read address 0 while writing address 0 in the same cycle: the register
  // boundary and the read-first rule in one check.
  h->Collide(0, ValueOf(0, 0xfffu), 0xff);
  h->Read(0);
  for (int addr = 1; addr < kDepth; ++addr) {
    h->Read(static_cast<uint8_t>(addr));
  }
}

// ---------------------------------------------------------------------------
// Phase 2: same-address read/write collision at EVERY address. Three
// consecutive cycles in which the read address and the write address are the
// same address -- this is the "all-address collision" case -- then a partial
// mask collision, then a clean read proving the final value landed.
void PhaseCollisionEveryAddress(RamHarness* h) {
  h->Phase("collision-every-address");
  for (int addr = 0; addr < kDepth; ++addr) {
    const uint8_t a = static_cast<uint8_t>(addr);
    for (int k = 0; k < 3; ++k) {
      h->Collide(a, ValueOf(static_cast<uint32_t>(addr), 0x100u + k), 0xff);
    }
    // Only the two low lanes change; all eight must read back pre-write.
    h->Collide(a, ValueOf(static_cast<uint32_t>(addr), 0x200u), 0x03);
    // A mask of zero writes nothing at all, collision or not.
    h->Collide(a, ValueOf(static_cast<uint32_t>(addr), 0x201u), 0x00);
    h->Read(a);
  }
}

// ---------------------------------------------------------------------------
// Phase 3: every one of the 256 byte-mask values, at a rotating address so
// that every address sees four different masks. Each write is a same-address
// collision (the read address is the written address) followed immediately by
// a plain read-back, which is the read-after-write ordering case.
void PhaseByteMaskSweep(RamHarness* h) {
  h->Phase("byte-mask-sweep");
  for (uint32_t mask = 0; mask < 256; ++mask) {
    const uint8_t addr = static_cast<uint8_t>(mask % kDepth);
    h->Write(addr, ValueOf(mask, 0x300u), static_cast<uint8_t>(mask), addr);
    h->Read(addr);
  }
  // Corner masks at the three addresses the card calls out.
  const uint8_t corner_masks[4] = {0x00, 0xff, 0x0f, 0xf0};
  for (int c = 0; c < 3; ++c) {
    const uint8_t addr = (c == 0) ? 0 : (c == 1 ? kDepth / 2 - 1 : kDepth - 1);
    for (int m = 0; m < 4; ++m) {
      h->Write(addr, ValueOf(addr, 0x400u + m), corner_masks[m], addr);
      h->Read(addr);
    }
  }
}

// ---------------------------------------------------------------------------
// Phase 4: back-to-back and interleaved traffic at addresses 0, DEPTH/2-1 and
// DEPTH-1: back-to-back reads, back-to-back writes, read-after-write,
// write-after-read, and two distinct addresses written on the same edge.
void PhaseEdgesAndInterleave(RamHarness* h) {
  h->Phase("edges-interleave");
  for (int e = 0; e < 3; ++e) {
    const uint8_t a = static_cast<uint8_t>((e == 0) ? 0 : (e == 1 ? kDepth / 2 - 1 : kDepth - 1));
    const uint8_t b = static_cast<uint8_t>((e == 0) ? 7 : (e == 1 ? 12 : kDepth - 1));
    const uint8_t c = static_cast<uint8_t>((e == 0) ? kDepth - 1 : (e == 1 ? 0 : kDepth / 2 - 1));

    // Back-to-back reads of three different addresses, no writes.
    h->Read(a);
    h->Read(b);
    h->Read(c);
    h->Read(a);

    // Back-to-back writes with the reads walking elsewhere: both must land.
    h->Write(a, ValueOf(a, 0x500u), 0xff, b);
    h->Write(b, ValueOf(a, 0x501u), 0xff, c);
    h->Read(a);
    h->Read(b);

    // Write-after-read: the read returns the pre-write value.
    h->Collide(a, ValueOf(a, 0x600u), 0xff);

    // Read-after-write: the read one cycle later returns the value written.
    h->Read(a);

    // Two different addresses written on the same edge (bank A writes `a`,
    // bank B writes `a ^ 0x2a`), then both read back.
    h->Write(a, ValueOf(a, 0x700u), 0xff, b);
    h->Write(b, ValueOf(a, 0x701u), 0xff, c);
    h->Read(a);
    h->Read(b);
  }
}

// ---------------------------------------------------------------------------
// Phase 5: reset must not disturb the array. Fill every address with a
// pattern, hold reset for four cycles while offering a write to address 0 on
// every one of them (the RAM must ignore writes during reset), release, and
// read the entire array back.
void PhaseResetPreservesArray(RamHarness* h) {
  h->Phase("reset-preserves-array");
  for (int round = 0; round < 2; ++round) {
    for (int addr = 0; addr < kDepth; ++addr) {
      const uint8_t a = static_cast<uint8_t>(addr);
      h->Write(a, ValueOf(static_cast<uint32_t>(addr), 0x800u + round), 0xff, a);
    }
    for (int r = 0; r < 4; ++r) {
      h->Cycle(Stim{0, ValueOf(0, 0xdeadu), 0xff, true, 0}, /*rst=*/true);
    }
    for (int addr = 0; addr < kDepth; ++addr) {
      const uint8_t a = static_cast<uint8_t>(addr);
      h->Cycle(Stim{a, 0, 0, false, a}, /*rst=*/false);
      h->Read(a);
    }
  }
}

// ---------------------------------------------------------------------------
// Phase 6: seeded soak. Addresses, byte masks, write enables and data all
// vary; half the cycles deliberately read the address being written, which is
// the collision case the mixed-port contract has to survive.
void PhaseSoak(RamHarness* h, mosaic::Rng& rng) {
  h->Phase("soak");
  for (int i = 0; i < 4000; ++i) {
    const uint8_t addr = static_cast<uint8_t>(rng.Below(kDepth));
    const uint8_t raddr =
        rng.Chance(50) ? addr : static_cast<uint8_t>(rng.Below(kDepth));
    h->Cycle(Stim{addr, rng.Next(), static_cast<uint8_t>(rng.Below(256)),
                  rng.Chance(70), raddr},
             false);
  }
}

}  // namespace

int main(int argc, char** argv) {
  mosaic::Options options;
  std::string error;
  if (!mosaic::Options::Parse(argc, argv, &options, &error)) {
    std::fprintf(stderr, "usage error: %s\n", error.c_str());
    return mosaic::kExitUsage;
  }
  Verilated::commandArgs(argc, argv);

  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  mosaic::ClockDriver clk;
  Vmosaic_ram_tb dut;
  RamHarness harness(&dut, &clk, &reporter, options.max_cycles);
  mosaic::Rng rng(options.seed);

  std::string detail;
  bool passed = true;
  try {
    PhaseInitialise(&harness);
    PhaseCollisionEveryAddress(&harness);
    PhaseByteMaskSweep(&harness);
    PhaseEdgesAndInterleave(&harness);
    PhaseResetPreservesArray(&harness);
    PhaseSoak(&harness, rng);
    detail = "collision matrix complete: " + std::to_string(harness.data_checks()) +
             " shadow comparisons, " + std::to_string(harness.cycles()) +
             " cycles, seed " + std::to_string(options.seed);
  } catch (const Failure& f) {
    passed = false;
    detail = "contract violated: " + f.what;
  }

  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
