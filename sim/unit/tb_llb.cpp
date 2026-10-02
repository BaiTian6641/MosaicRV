// ============================================================================
// tb_llb.cpp -- CASE=llb.stale_copy_invalidation, work package I-060.
//
// The DUT is `mosaic_llb`, driven directly through `mosaic_llb_tb`. The rule
// this file enforces is the card's: **after a concurrent writer, an alias, or
// an ASID switch, a stale value must never be returned**, and the ordering
// between an invalidation and an in-flight hit must hold.
//
// The oracle is an independent host memory model, `mem_`, and nothing else.
// Every value the LLB returns is compared against the *current* content of the
// host's memory at that line -- the memory's own history, not the LLB's state.
// A fill is always taken from `mem_`; a writer (this hart's store, an external
// snoop, an L1 refill) always changes `mem_` first. So a hit that returns
// anything other than what `mem_` holds now is a stale read, and the case says
// so by name.
//
// Two things are asserted for every invalidation source the case exercises:
//
//   1. **the LLB was hit on that line before the invalidation** (a probe that
//      must report a hit, checked against `mem_`); and
//   2. **the invalidation removed the entry**, read out of the LLB's *contents*
//      through the debug port (valid/line/asid/perms), not inferred from the
//      returned data. An LLB that "returns the right answer" because the
//      consumer happened to re-read memory is not doing its job.
//
// Phases, each failable on its own:
//
//   geometry          the elaborated geometry is the one this driver was
//                     written for, and reset leaves the buffer empty.
//   fill-hit          a filled line hits and returns the memory's value.
//   store-invalidates this hart's store removes the line it wrote (the card's
//                     central failure), and leaves other lines alone.
//   alias             the same physical line reached through a second virtual
//                     alias hits, while a different permission context does
//                     not; a store removes every context's copy.
//   mmio-bypass       a device/ROM line is refused a fill and never hits: the
//                     generated platform map decides, not a hand-written list.
//   atomic-bypass     an atomic neither hits nor destroys the entry, and the
//                     line it wrote is invalidated afterwards.
//   in-flight-race    a hit in the same cycle as the invalidating store is
//                     refused, for every invalidation source.
//   refill            an L1 refill of the line removes the copy.
//   snoop             an external writer, and a broadcast, remove the copies.
//   fence             FENCE and FENCE.I remove everything; SFENCE.VMA removes
//                     exactly the context it names and nothing else.
//   asid-switch       a `satp` write or an ASID switch makes every entry
//                     unreachable, even under a reused ASID.
//   conservation      counters, occupancy and coverage agree at rest.
//
// Mutant switches (also listed in rtl/core/mosaic_llb.sv):
//   MOSAIC_LLB_MUTANT_STORE_NO_INVALIDATE / MMIO_HIT / RACE_STALE_HIT /
//   ASID_SWITCH_NO_INVALIDATE / VA_KEYED
// ============================================================================

#include <verilated.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

#include "Vmosaic_llb_tb.h"
#include "mosaic_platform.h"
#include "sim_common.h"

namespace {

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw std::runtime_error(where + ": " + detail);
}

std::string U64(uint64_t v) { return mosaic::Hex(v); }
std::string Dec(uint64_t v) { return std::to_string(v); }

// The geometry this driver was written for; the elaborated DUT is asserted
// against it in the `geometry` phase.
constexpr uint32_t kLineBytes = 32;
constexpr uint32_t kEntries = 8;
constexpr uint32_t kLineWords = 4;                 // 64-bit words per line
constexpr uint32_t kLanes = 8;                     // 32-bit lanes in a line
constexpr uint32_t kOffsetMask = kLineBytes - 1;

// Leaf permission classes, {x, w, r, u} -- the same encoding as
// mosaic_pkg::leaf_perm_ok. The LLB never interprets them; they are the
// permission context of the key.
constexpr uint8_t kPermRW = 0x7;      // U: r+w
constexpr uint8_t kPermRO = 0x3;      // U: r
constexpr uint8_t kPermRW_S = 0x6;    // S: r+w

// The platform map, taken from the generated header -- never a hand-written
// list of device addresses.
constexpr uint32_t kRamBase = static_cast<uint32_t>(MOSAIC_RAM_BASE);
constexpr uint32_t kUartBase = static_cast<uint32_t>(MOSAIC_UART_BASE);
constexpr uint32_t kRomBase = static_cast<uint32_t>(MOSAIC_BOOT_ROM_BASE);
constexpr uint32_t kTestHarnessBase = static_cast<uint32_t>(MOSAIC_TEST_HARNESS_BASE);
constexpr uint32_t kClintBase = static_cast<uint32_t>(MOSAIC_CLINT_BASE);
constexpr bool kRamCacheable = (MOSAIC_RAM_CACHEABLE != 0);

constexpr uint8_t kFenceMem = 0;
constexpr uint8_t kFenceI = 1;
constexpr uint8_t kFenceSfence = 2;

using Lanes = std::array<uint32_t, kLanes>;

struct Entry {
  bool valid = false;
  uint32_t line = 0;
  uint16_t asid = 0;
  uint8_t perms = 0;
  uint32_t vpn = 0;
  Lanes data{};
};

struct Probe {
  bool hit = false;
  bool bypass = false;
  Lanes data{};
};

class Harness {
 public:
  Harness(Vmosaic_llb_tb* dut, mosaic::ClockDriver* clk, mosaic::Reporter* rep,
          uint64_t max_cycles)
      : dut_(dut), clk_(clk), rep_(rep), max_cycles_(max_cycles) {
    Idle();
    rst_ = true;
    for (int i = 0; i < 4; ++i) Edge();
    rst_ = false;
    Edge();
    Eval();
  }

  // ------------------------------------------------------------ primitives
  void Idle() {
    req_valid_ = false;
    req_pa_ = 0;
    req_vpn_ = 0;
    req_asid_ = 0;
    req_perms_ = 0;
    req_atomic_ = false;
    fill_valid_ = false;
    fill_pa_ = 0;
    fill_vpn_ = 0;
    fill_asid_ = 0;
    fill_perms_ = 0;
    fill_data_ = Lanes{};
    inv_store_valid_ = false;
    inv_store_pa_ = 0;
    inv_refill_valid_ = false;
    inv_refill_pa_ = 0;
    inv_snoop_valid_ = false;
    inv_snoop_pa_ = 0;
    inv_snoop_all_ = false;
    fence_valid_ = false;
    fence_kind_ = 0;
    fence_vpn_ = 0;
    fence_has_vpn_ = false;
    fence_asid_ = 0;
    fence_has_asid_ = false;
    ctx_valid_ = false;
  }

  void Apply() {
    dut_->rst = rst_ ? 1 : 0;
    dut_->req_valid = req_valid_ ? 1 : 0;
    dut_->req_pa = req_pa_;
    dut_->req_vpn = req_vpn_;
    dut_->req_asid = req_asid_;
    dut_->req_perms = req_perms_;
    dut_->req_atomic = req_atomic_ ? 1 : 0;
    dut_->fill_valid = fill_valid_ ? 1 : 0;
    dut_->fill_pa = fill_pa_;
    dut_->fill_vpn = fill_vpn_;
    dut_->fill_asid = fill_asid_;
    dut_->fill_perms = fill_perms_;
    for (uint32_t i = 0; i < kLanes; ++i) dut_->fill_data[i] = fill_data_[i];
    dut_->inv_store_valid = inv_store_valid_ ? 1 : 0;
    dut_->inv_store_pa = inv_store_pa_;
    dut_->inv_refill_valid = inv_refill_valid_ ? 1 : 0;
    dut_->inv_refill_pa = inv_refill_pa_;
    dut_->inv_snoop_valid = inv_snoop_valid_ ? 1 : 0;
    dut_->inv_snoop_pa = inv_snoop_pa_;
    dut_->inv_snoop_all = inv_snoop_all_ ? 1 : 0;
    dut_->fence_valid = fence_valid_ ? 1 : 0;
    dut_->fence_kind = fence_kind_;
    dut_->fence_vpn = fence_vpn_;
    dut_->fence_has_vpn = fence_has_vpn_ ? 1 : 0;
    dut_->fence_asid = fence_asid_;
    dut_->fence_has_asid = fence_has_asid_ ? 1 : 0;
    dut_->ctx_valid = ctx_valid_ ? 1 : 0;
    dut_->dbg_index = dbg_index_;
  }

  void Eval() {
    Apply();
    dut_->eval();
  }

  // One rising edge; state changes are sampled after it.
  void Edge() {
    if (clk_->cycle() >= max_cycles_) {
      Fail("cycles", "max-cycles (" + Dec(max_cycles_) + ") exhausted");
    }
    Apply();
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
  }

  // ------------------------------------------------------- host memory oracle
  uint64_t ReadWord(uint32_t addr) {
    auto it = mem_.find(addr);
    return it == mem_.end() ? 0 : it->second;
  }

  void WriteWord(uint32_t addr, uint64_t value) { mem_[addr] = value; }

  // A writer (this hart, another hart, DMA, or an L1 refill) stores a fresh
  // value into the line; every word differs so a wrong slice is visible too.
  void WriteLine(uint32_t pa, uint64_t base) {
    for (uint32_t w = 0; w < kLineWords; ++w) {
      WriteWord(LineBase(pa) + 8 * w, base + w);
    }
  }

  Lanes MemLine(uint32_t pa) {
    Lanes out{};
    for (uint32_t w = 0; w < kLineWords; ++w) {
      const uint64_t v = ReadWord(LineBase(pa) + 8 * w);
      out[2 * w] = static_cast<uint32_t>(v & 0xffffffffull);
      out[2 * w + 1] = static_cast<uint32_t>(v >> 32);
    }
    return out;
  }

  static uint32_t LineBase(uint32_t pa) { return pa & ~kOffsetMask; }

  // ------------------------------------------------------------- operations
  void SetLookup(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms,
                 bool atomic) {
    Idle();
    req_valid_ = true;
    req_pa_ = pa;
    req_vpn_ = vpn & 0x7ffffffu;
    req_asid_ = asid;
    req_perms_ = perms;
    req_atomic_ = atomic;
  }

  void SetFill(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms) {
    Idle();
    fill_valid_ = true;
    fill_pa_ = pa;
    fill_vpn_ = vpn & 0x7ffffffu;
    fill_asid_ = asid;
    fill_perms_ = perms;
    fill_data_ = MemLine(pa);
  }

  void SetStore(uint32_t pa) {
    inv_store_valid_ = true;
    inv_store_pa_ = pa;
  }

  // The presented (pre-edge) outputs of the current drive.
  Probe Present() {
    Eval();
    Probe p;
    p.hit = dut_->req_hit != 0;
    p.bypass = dut_->req_bypass != 0;
    for (uint32_t i = 0; i < kLanes; ++i) p.data[i] = dut_->req_data[i];
    return p;
  }

  bool PresentFillOk() {
    Eval();
    return dut_->fill_ok != 0;
  }

  Probe ProbeLookup(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms,
                    bool atomic) {
    SetLookup(pa, vpn, asid, perms, atomic);
    return Present();
  }

  // Commit the presented lookup with an edge (so the counters move), then
  // return to idle so no later edge sees the request.
  void CommitLookup() {
    Edge();
    Idle();
    Eval();
  }

  // Fill from the host memory's current content (never from the LLB).
  void Fill(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms) {
    Idle();
    fill_valid_ = true;
    fill_pa_ = pa;
    fill_vpn_ = vpn & 0x7ffffffu;
    fill_asid_ = asid;
    fill_perms_ = perms;
    fill_data_ = MemLine(pa);
    Edge();
    Idle();
    Eval();
  }

  bool FillExpectRefused(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms) {
    Idle();
    fill_valid_ = true;
    fill_pa_ = pa;
    fill_vpn_ = vpn & 0x7ffffffu;
    fill_asid_ = asid;
    fill_perms_ = perms;
    fill_data_ = MemLine(pa);
    Eval();
    const bool ok = dut_->fill_ok != 0;
    Edge();
    Idle();
    Eval();
    return ok;
  }

  // A speculative (prefetch-style) fill presented in the *same cycle* as an
  // invalidation of the same line. `source` selects the invalidation exactly as
  // ConflictProbe does: store, refill, snoop, snoop-all, fence, fence-i,
  // sfence, ctx. Returns the `fill_ok_o` the DUT presents in that cycle; the
  // caller requires it to be 0 (the racing fill must not install).
  bool FillRacingInvalidate(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms,
                            int source) {
    Idle();
    fill_valid_ = true;
    fill_pa_ = pa;
    fill_vpn_ = vpn & 0x7ffffffu;
    fill_asid_ = asid;
    fill_perms_ = perms;
    fill_data_ = MemLine(pa);
    switch (source) {
      case 0: inv_store_valid_ = true; inv_store_pa_ = pa; break;
      case 1: inv_refill_valid_ = true; inv_refill_pa_ = pa; break;
      case 2: inv_snoop_valid_ = true; inv_snoop_pa_ = pa; break;
      case 3: inv_snoop_valid_ = true; inv_snoop_all_ = true; break;
      case 4: fence_valid_ = true; fence_kind_ = kFenceMem; break;
      case 5: fence_valid_ = true; fence_kind_ = kFenceI; break;
      case 6: fence_valid_ = true; fence_kind_ = kFenceSfence;
              fence_has_asid_ = true; fence_asid_ = asid; break;
      default: ctx_valid_ = true; break;
    }
    Eval();
    const bool ok = dut_->fill_ok != 0;
    Edge();
    Idle();
    Eval();
    return ok;
  }

  void PulseStore(uint32_t pa) {
    Idle();
    inv_store_valid_ = true;
    inv_store_pa_ = pa;
    Edge();
    Idle();
    Eval();
  }

  void PulseRefill(uint32_t pa) {
    Idle();
    inv_refill_valid_ = true;
    inv_refill_pa_ = pa;
    Edge();
    Idle();
    Eval();
  }

  void PulseSnoop(uint32_t pa) {
    Idle();
    inv_snoop_valid_ = true;
    inv_snoop_pa_ = pa;
    Edge();
    Idle();
    Eval();
  }

  void PulseSnoopAll() {
    Idle();
    inv_snoop_valid_ = true;
    inv_snoop_all_ = true;
    Edge();
    Idle();
    Eval();
  }

  void PulseFence(uint8_t kind, bool has_asid, uint16_t asid, bool has_vpn,
                  uint32_t vpn) {
    Idle();
    fence_valid_ = true;
    fence_kind_ = kind;
    fence_has_asid_ = has_asid;
    fence_asid_ = asid;
    fence_has_vpn_ = has_vpn;
    fence_vpn_ = vpn & 0x7ffffffu;
    Edge();
    Idle();
    Eval();
  }

  void PulseCtx() {
    Idle();
    ctx_valid_ = true;
    Edge();
    Idle();
    Eval();
  }

  // The same-cycle ordering rule: present a lookup on `pa` while `arm` asserts
  // the invalidation. Returns the hit the DUT *presents* in that cycle; the
  // entry must then be gone.
  Probe ConflictProbe(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms,
                      int source) {
    Idle();
    req_valid_ = true;
    req_pa_ = pa;
    req_vpn_ = vpn & 0x7ffffffu;
    req_asid_ = asid;
    req_perms_ = perms;
    switch (source) {
      case 0: inv_store_valid_ = true; inv_store_pa_ = pa; break;
      case 1: inv_refill_valid_ = true; inv_refill_pa_ = pa; break;
      case 2: inv_snoop_valid_ = true; inv_snoop_pa_ = pa; break;
      case 3: inv_snoop_valid_ = true; inv_snoop_all_ = true; break;
      case 4: fence_valid_ = true; fence_kind_ = kFenceMem; break;
      case 5: fence_valid_ = true; fence_kind_ = kFenceI; break;
      case 6: fence_valid_ = true; fence_kind_ = kFenceSfence;
              fence_has_asid_ = true; fence_asid_ = asid; break;
      default: ctx_valid_ = true; break;
    }
    Eval();
    Probe p;
    p.hit = dut_->req_hit != 0;
    p.data = Lanes{};
    for (uint32_t i = 0; i < kLanes; ++i) p.data[i] = dut_->req_data[i];
    // Register the invalidation (with the request still presented).
    Edge();
    Idle();
    Eval();
    return p;
  }

  // ------------------------------------------------------------- observation
  Entry Dbg(uint32_t index) {
    Idle();
    dbg_index_ = index;
    Eval();
    Entry e;
    e.valid = dut_->dbg_valid != 0;
    e.line = static_cast<uint32_t>(dut_->dbg_line) << 5;
    e.asid = static_cast<uint16_t>(dut_->dbg_asid);
    e.perms = static_cast<uint8_t>(dut_->dbg_perms);
    e.vpn = dut_->dbg_vpn;
    for (uint32_t i = 0; i < kLanes; ++i) e.data[i] = dut_->dbg_data[i];
    dbg_index_ = 0;
    return e;
  }

  int FindEntry(uint32_t pa, uint16_t asid, uint8_t perms) {
    for (uint32_t i = 0; i < kEntries; ++i) {
      Entry e = Dbg(i);
      if (e.valid && e.line == LineBase(pa) && e.asid == asid && e.perms == perms) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  uint32_t Count() {
    Eval();
    return static_cast<uint32_t>(dut_->o_count);
  }

  uint32_t HitCtr() { Eval(); return dut_->o_hit_ctr; }
  uint32_t MissCtr() { Eval(); return dut_->o_miss_ctr; }
  uint32_t BypassCtr() { Eval(); return dut_->o_bypass_ctr; }
  uint32_t FillCtr() { Eval(); return dut_->o_fill_ctr; }
  uint32_t FillRefusedCtr() { Eval(); return dut_->o_fill_refused_ctr; }
  uint32_t InvCtr() { Eval(); return dut_->o_inv_ctr; }
  uint32_t RaceRefuseCtr() { Eval(); return dut_->o_race_refuse_ctr; }
  uint32_t Entries() { Eval(); return dut_->o_entries; }
  uint32_t LineBytes() { Eval(); return dut_->o_line_bytes; }
  uint64_t Cycles() { return clk_->cycle(); }
  void Phase(const std::string& name) {
    phase_ = name;
    if (verbose_) std::fprintf(stderr, "  phase %s\n", name.c_str());
  }
  const std::string& phase() const { return phase_; }

  mosaic::Reporter* rep() { return rep_; }

  // A failed requirement: recorded by name and thrown, so the report names one
  // defect rather than a thousand consequences of it.
  void Require(bool ok, const std::string& where, const std::string& expected,
               const std::string& actual) {
    if (ok) return;
    rep_->Mismatch(where.empty() ? phase_ : where, expected, actual);
    throw std::runtime_error((where.empty() ? phase_ : where) + ": expected " +
                             expected + ", got " + actual);
  }

 private:
  Vmosaic_llb_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* rep_;
  uint64_t max_cycles_;
  bool rst_ = false;
  bool verbose_ = false;
  std::string phase_;

  bool req_valid_ = false;
  uint64_t req_pa_ = 0;
  uint32_t req_vpn_ = 0;
  uint16_t req_asid_ = 0;
  uint8_t req_perms_ = 0;
  bool req_atomic_ = false;
  bool fill_valid_ = false;
  uint64_t fill_pa_ = 0;
  uint32_t fill_vpn_ = 0;
  uint16_t fill_asid_ = 0;
  uint8_t fill_perms_ = 0;
  Lanes fill_data_{};
  bool inv_store_valid_ = false;
  uint64_t inv_store_pa_ = 0;
  bool inv_refill_valid_ = false;
  uint64_t inv_refill_pa_ = 0;
  bool inv_snoop_valid_ = false;
  uint64_t inv_snoop_pa_ = 0;
  bool inv_snoop_all_ = false;
  bool fence_valid_ = false;
  uint8_t fence_kind_ = 0;
  uint32_t fence_vpn_ = 0;
  bool fence_has_vpn_ = false;
  uint16_t fence_asid_ = 0;
  bool fence_has_asid_ = false;
  bool ctx_valid_ = false;
  uint32_t dbg_index_ = 0;

  std::map<uint32_t, uint64_t> mem_;   // the independent oracle
};

// ============================================================================
// The campaign
// ============================================================================
class Campaign {
 public:
  Campaign(Harness* h, mosaic::Reporter* rep) : h_(h), rep_(rep) {}

  std::string Run() {
    Geometry();
    FillAndHit();
    StoreInvalidates();
    Alias();
    BypassRules();
    AtomicBypass();
    ContextSwitch();
    InFlightRace();
    RefillSource();
    SnoopSource();
    FenceSource();
    Conservation();
    return Summary();
  }

  // A profile whose platform map declares RAM non-cacheable has nothing the LLB
  // is permitted to hold, so the freshness campaign has no subject. What is
  // still real -- and still checked -- is the map-driven bypass: nothing may be
  // cached anywhere, and the case must not silently pass by caching anyway.
  std::string RunNonCacheableProfile() {
    Geometry();
    BypassRules();
    NonCacheableRam();
    return Summary();
  }

  void NonCacheableRam() {
    h_->Phase("non-cacheable-ram");
    const uint32_t line = kRamBase + 0x1000;
    h_->WriteLine(line, 0xA100);
    Require(!h_->FillExpectRefused(line, kVpnA, 1, kPermRW), "",
            "a RAM fill is refused under a non-cacheable map",
            "a RAM fill was accepted");
    Require(h_->FindEntry(line, 1, kPermRW) < 0, "",
            "no RAM line is resident", "a RAM line is resident");
    Probe p = h_->ProbeLookup(line, kVpnA, 1, kPermRW, false);
    Require(!p.hit && p.bypass, "", "a RAM lookup is bypassed",
            "a RAM lookup was served");
    h_->CommitLookup();
    Require(h_->Count() == 0, "", "the buffer is empty",
            Dec(h_->Count()) + " entries were cached");
    Require(h_->BypassCtr() > 0 && h_->FillRefusedCtr() > 0, "",
            "bypass and refusal are covered", "a counter did not move");
  }

  // --------------------------------------------------------------- geometry
  void Geometry() {
    h_->Phase("geometry");
    Require(h_->Entries() == kEntries, "",
            "o_entries=" + Dec(kEntries), "o_entries=" + Dec(h_->Entries()));
    Require(h_->LineBytes() == kLineBytes, "",
            "o_line_bytes=" + Dec(kLineBytes), "o_line_bytes=" + Dec(h_->LineBytes()));
    Require(h_->Count() == 0, "", "0 valid entries after reset",
            Dec(h_->Count()) + " valid entries after reset");
  }

  // -------------------------------------------------------------- fill+hit
  void FillAndHit() {
    h_->Phase("fill-hit");
    const uint32_t l1 = kRamBase + 0x1000;
    h_->WriteLine(l1, 0xA100);
    h_->Fill(l1, kVpnA, 1, kPermRW);
    Require(h_->FindEntry(l1, 1, kPermRW) >= 0, "",
            "the filled line is resident", "the filled line is not resident");
    ExpectHitWithMem(l1, kVpnA, 1, kPermRW, false);
    ExpectHitWithMem(l1, kVpnA, 1, kPermRW, false);   // a second access hits
    Require(h_->HitCtr() >= 2, "", "at least two hits", Dec(h_->HitCtr()) + " hits");
    Require(h_->FillCtr() == 1, "", "one fill", Dec(h_->FillCtr()) + " fills");
  }

  // ------------------------------------------------------- store invalidates
  void StoreInvalidates() {
    h_->Phase("store-invalidates");
    const uint32_t l1 = kRamBase + 0x1000;
    const uint32_t l2 = kRamBase + 0x1020;
    h_->WriteLine(l2, 0xB200);
    h_->Fill(l2, kVpnC, 1, kPermRW);
    h_->Fill(l1, kVpnA, 1, kPermRW);

    // The LLB is hit on the line *before* the store, so the invalidation below
    // has something to remove.
    ExpectHitWithMem(l1, kVpnA, 1, kPermRW, false);
    const uint32_t inv_before = h_->InvCtr();

    // The writer stores: memory changes first, then the invalidation arrives.
    h_->WriteLine(l1, 0xA200);
    h_->PulseStore(l1);

    Require(h_->FindEntry(l1, 1, kPermRW) < 0, "",
            "the store removed the line it wrote",
            "the written line is still resident");
    Require(h_->InvCtr() > inv_before, "", "the store counted as an invalidation",
            "no invalidation was counted");
    // A store to another line leaves this one alone.
    Require(h_->FindEntry(l2, 1, kPermRW) >= 0, "",
            "a store to another line does not remove this one",
            "an unrelated entry was removed");

    // A miss after the store cannot be a stale read: the value the LLB would
    // have to return is the *new* memory content, which it never saw.
    Probe p = h_->ProbeLookup(l1, kVpnA, 1, kPermRW, false);
    Require(!p.hit, "", "a miss after the store", "a hit after the store");
    h_->CommitLookup();
    h_->Fill(l1, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l1, kVpnA, 1, kPermRW, false);
  }

  // ------------------------------------------------------------------- alias
  void Alias() {
    h_->Phase("alias");
    const uint32_t l2 = kRamBase + 0x1020;
    const uint32_t l4 = kRamBase + 0x1040;
    h_->WriteLine(l2, 0xB300);

    // Two virtual aliases, one physical line: the copy is physical, so the
    // second alias hits. A virtual-address-keyed LLB misses here (VA_KEYED).
    h_->Fill(l2, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l2, kVpnA, 1, kPermRW, false);
    ExpectHitWithMem(l2, kVpnB, 1, kPermRW, false);

    // A second permission context on the same line is a different key: it must
    // not be served the other context's copy.
    Probe ro = h_->ProbeLookup(l2, kVpnA, 1, kPermRO, false);
    Require(!ro.hit, "", "no hit across permission contexts",
            "a hit across permission contexts");
    h_->CommitLookup();
    h_->Fill(l2, kVpnA, 1, kPermRO);     // now two entries for the one line
    Require(h_->FindEntry(l2, 1, kPermRO) >= 0 && h_->FindEntry(l2, 1, kPermRW) >= 0,
            "", "two permission contexts resident for one line",
            "a context went missing");

    // A store is a *physical* event: it removes every context's copy, because
    // they are copies of the same physical line.
    h_->WriteLine(l2, 0xB400);
    h_->PulseStore(l2);
    Require(h_->FindEntry(l2, 1, kPermRW) < 0 && h_->FindEntry(l2, 1, kPermRO) < 0,
            "", "a store removed every context's copy of the physical line",
            "a context's copy survived the store");

    // A store observed under a *different* context's address still removes it:
    // the invalidation port carries no context, by construction.
    h_->WriteLine(l4, 0xC100);
    h_->Fill(l4, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l4, kVpnA, 1, kPermRW, false);
    // A third permission class is a third context: it must not be served the
    // user-mode copy, and once filled it is removed by the same store.
    Probe sv = h_->ProbeLookup(l4, kVpnA, 1, kPermRW_S, false);
    Require(!sv.hit, "", "no hit across a third permission class",
            "a hit across a third permission class");
    h_->CommitLookup();
    h_->Fill(l4, kVpnA, 1, kPermRW_S);
    Require(h_->FindEntry(l4, 1, kPermRW_S) >= 0, "",
            "the third context filled", "the third context did not fill");
    h_->WriteLine(l4, 0xC200);
    h_->PulseStore(l4);
    Require(h_->FindEntry(l4, 1, kPermRW) < 0 && h_->FindEntry(l4, 1, kPermRW_S) < 0,
            "", "the store removed the line regardless of the reader's context",
            "the line is still resident");
  }

  // ------------------------------------------------------------- bypass rules
  void BypassRules() {
    h_->Phase("mmio-bypass");
    // A device fill is refused outright -- a device line may not be resident.
    const uint32_t dev_refused_before = h_->FillRefusedCtr();
    h_->WriteLine(kUartBase, 0xDE00);
    bool ok = h_->FillExpectRefused(kUartBase, kVpnA, 1, kPermRW);
    Require(!ok, "", "a device fill is refused", "a device fill was accepted");
    Require(h_->FillRefusedCtr() > dev_refused_before, "",
            "the refused fill is counted", "the refusal was not counted");
    Require(h_->FindEntry(kUartBase, 1, kPermRW) < 0, "",
            "no device line is resident", "a device line is resident");
    // A device lookup is bypassed, whatever the buffer holds.
    const uint32_t bypass_before = h_->BypassCtr();
    Probe p = h_->ProbeLookup(kUartBase, kVpnA, 1, kPermRW, false);
    Require(!p.hit && p.bypass, "", "a device lookup is bypassed",
            "a device lookup was served");
    h_->CommitLookup();
    Require(h_->BypassCtr() > bypass_before, "", "the bypass is counted",
            "the bypass was not counted");

    // The boot ROM is non-cacheable in this platform's map: also refused. The
    // card permits an immutable region to be enabled -- when the map says it is
    // cacheable. It says it is not, so the map decides.
    bool rom_ok = h_->FillExpectRefused(kRomBase, kVpnA, 1, kPermRW);
    Require(!rom_ok, "", "a non-cacheable ROM fill is refused",
            "a non-cacheable ROM fill was accepted");
    Probe rp = h_->ProbeLookup(kRomBase, kVpnA, 1, kPermRW, false);
    Require(!rp.hit, "", "no ROM hit", "a ROM hit");
    h_->CommitLookup();
  }

  void AtomicBypass() {
    h_->Phase("atomic-bypass");
    const uint32_t l3 = kRamBase + 0x1030;
    h_->WriteLine(l3, 0xD100);
    h_->Fill(l3, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l3, kVpnA, 1, kPermRW, false);
    // An atomic neither hits nor destroys the entry.
    Probe a = h_->ProbeLookup(l3, kVpnA, 1, kPermRW, true);
    Require(!a.hit && a.bypass, "", "an atomic is bypassed",
            "an atomic was served from the LLB");
    h_->CommitLookup();
    Require(h_->FindEntry(l3, 1, kPermRW) >= 0, "",
            "an atomic does not evict the entry it bypassed",
            "the bypassed entry disappeared");
    ExpectHitWithMem(l3, kVpnA, 1, kPermRW, false);
    // The atomic wrote the line: it is invalidated like any store.
    h_->WriteLine(l3, 0xD200);
    h_->PulseStore(l3);
    Require(h_->FindEntry(l3, 1, kPermRW) < 0, "",
            "the atomic's line was invalidated",
            "the atomic's line is still resident");
  }

  // ------------------------------------------------------------ in-flight race
  void InFlightRace() {
    h_->Phase("in-flight-race");
    const uint32_t base = kRamBase + 0x1100;
    const char* names[8] = {"store", "refill", "snoop", "snoop-all",
                            "fence", "fence-i", "sfence", "ctx"};
    const uint32_t race_before = h_->RaceRefuseCtr();
    const uint32_t inv_before = h_->InvCtr();

    for (int source = 0; source < 8; ++source) {
      const uint32_t line = base + 0x40 * static_cast<uint32_t>(source);
      const uint16_t asid = 1;
      h_->WriteLine(line, 0xE000 + 0x100 * static_cast<uint32_t>(source));
      h_->Fill(line, kVpnA, asid, kPermRW);
      ExpectHitWithMem(line, kVpnA, asid, kPermRW, false);   // hit before

      // The writer makes the new value visible, then the invalidation and a
      // lookup on the same line are presented in the *same* cycle.
      h_->WriteLine(line, 0xE800 + 0x100 * static_cast<uint32_t>(source));
      Probe p = h_->ConflictProbe(line, kVpnA, asid, kPermRW, source);

      const std::string where = std::string("in-flight-race/") + names[source];
      if (p.hit) {
        // If a hit was nonetheless granted, it must already carry the new
        // value: a hit *after* the store is visible is the bug the card names.
        const Lanes want = h_->MemLine(line);
        const bool fresh = (p.data == want);
        h_->Require(fresh, where,
                    "no hit in the invalidating cycle (or the post-store value)",
                    "a hit with the pre-store value");
      }
      h_->Require(h_->FindEntry(line, 1, kPermRW) < 0, where,
                  "the invalidation removed the entry", "the entry survived");
    }

    Require(h_->RaceRefuseCtr() > race_before, "",
            "the refusal is counted", "no refusal was counted");
    Require(h_->InvCtr() >= inv_before + 8, "",
            "every source invalidated", "a source did not invalidate");

    // The refusal is specific: a lookup on a *different* line still hits in the
    // same cycle as the store.
    const uint32_t keep = kRamBase + 0x1200;
    const uint32_t other = kRamBase + 0x1220;
    h_->WriteLine(keep, 0xF000);
    h_->WriteLine(other, 0xF100);
    h_->Fill(keep, kVpnA, 1, kPermRW);
    h_->Fill(other, kVpnA, 1, kPermRW);
    ExpectHitWithMem(keep, kVpnA, 1, kPermRW, false);
    h_->SetLookup(keep, kVpnA, 1, kPermRW, false);
    h_->SetStore(other);
    Probe q = h_->Present();
    h_->Edge();
    h_->Idle();
    h_->Eval();
    Require(q.hit, "in-flight-race/other-line",
            "a hit on an unrelated line is granted",
            "the unrelated hit was refused");
    Require(h_->FindEntry(keep, 1, kPermRW) >= 0, "in-flight-race/other-line",
            "an unrelated line survives another line's store",
            "the unrelated entry was removed");
  }

  // ------------------------------------------------------------------ refill
  void RefillSource() {
    h_->Phase("refill");
    const uint32_t l5 = kRamBase + 0x1300;
    const uint32_t l6 = kRamBase + 0x1320;
    h_->WriteLine(l5, 0x5100);
    h_->WriteLine(l6, 0x5200);
    h_->Fill(l5, kVpnA, 1, kPermRW);
    h_->Fill(l6, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l5, kVpnA, 1, kPermRW, false);   // hit before

    h_->WriteLine(l5, 0x5300);            // the refill brings fresh data
    h_->PulseRefill(l5);
    Require(h_->FindEntry(l5, 1, kPermRW) < 0, "",
            "the refill removed the line", "the refilled line is still resident");
    Require(h_->FindEntry(l6, 1, kPermRW) >= 0, "",
            "an unrelated line survives another line's refill",
            "an unrelated entry was removed");
    Probe p = h_->ProbeLookup(l5, kVpnA, 1, kPermRW, false);
    Require(!p.hit, "", "a miss after the refill", "a hit after the refill");
    h_->CommitLookup();
    h_->Fill(l5, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l5, kVpnA, 1, kPermRW, false);
  }

  // ------------------------------------------------------------------- snoop
  void SnoopSource() {
    h_->Phase("snoop");
    const uint32_t l7 = kRamBase + 0x1400;
    const uint32_t l8 = kRamBase + 0x1420;
    const uint32_t l9 = kRamBase + 0x1440;
    h_->WriteLine(l7, 0x7100);
    h_->WriteLine(l8, 0x7200);
    h_->WriteLine(l9, 0x7300);
    h_->Fill(l7, kVpnA, 1, kPermRW);
    h_->Fill(l8, kVpnA, 1, kPermRW);
    h_->Fill(l9, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l7, kVpnA, 1, kPermRW, false);   // hit before

    h_->WriteLine(l7, 0x7400);            // another hart / DMA wrote
    h_->PulseSnoop(l7);
    Require(h_->FindEntry(l7, 1, kPermRW) < 0, "",
            "the snoop removed the line", "the snooped line is still resident");
    Require(h_->FindEntry(l8, 1, kPermRW) >= 0, "",
            "an unrelated line survives a line snoop", "an unrelated entry was removed");
    Probe p = h_->ProbeLookup(l7, kVpnA, 1, kPermRW, false);
    Require(!p.hit, "", "a miss after the snoop", "a hit after the snoop");
    h_->CommitLookup();

    // A broadcast removes everything.
    ExpectHitWithMem(l8, kVpnA, 1, kPermRW, false);
    h_->PulseSnoopAll();
    Require(h_->Count() == 0, "", "the broadcast removed every entry",
            Dec(h_->Count()) + " entries survived");
  }

  // ------------------------------------------------------------------- fence
  void FenceSource() {
    h_->Phase("fence");
    const uint32_t l10 = kRamBase + 0x1500;
    const uint32_t l11 = kRamBase + 0x1520;
    h_->WriteLine(l10, 0xA100);
    h_->WriteLine(l11, 0xA200);
    h_->Fill(l10, kVpnA, 1, kPermRW);
    h_->Fill(l11, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l10, kVpnA, 1, kPermRW, false);   // hit before
    h_->PulseFence(kFenceMem, false, 0, false, 0);
    Require(h_->Count() == 0, "", "FENCE removed every entry",
            Dec(h_->Count()) + " entries survived");

    h_->Fill(l10, kVpnA, 1, kPermRW);
    h_->Fill(l11, kVpnA, 1, kPermRW);
    ExpectHitWithMem(l10, kVpnA, 1, kPermRW, false);
    h_->PulseFence(kFenceI, false, 0, false, 0);
    Require(h_->Count() == 0, "", "FENCE.I removed every entry",
            Dec(h_->Count()) + " entries survived");

    // SFENCE.VMA is a *translation* fence: it removes the context it names and
    // nothing else. It is never relied on as a memory-data fence.
    const uint32_t l12 = kRamBase + 0x1560;
    const uint32_t l13 = kRamBase + 0x1580;
    h_->WriteLine(l12, 0xA300);
    h_->WriteLine(l13, 0xA400);
    h_->Fill(l12, kVpnA, 1, kPermRW);
    h_->Fill(l13, kVpnA, 2, kPermRW);
    h_->Fill(l10, kVpnA, 3, kPermRW);            // an unrelated context survives
    ExpectHitWithMem(l12, kVpnA, 1, kPermRW, false);   // hit before
    h_->PulseFence(kFenceSfence, true, 1, false, 0);
    Require(h_->FindEntry(l12, 1, kPermRW) < 0, "",
            "SFENCE.VMA removed the named ASID's entry",
            "the named ASID's entry survived");
    Require(h_->FindEntry(l13, 2, kPermRW) >= 0, "",
            "SFENCE.VMA left another ASID alone", "another ASID's entry was removed");
    Require(h_->FindEntry(l10, 3, kPermRW) >= 0, "",
            "SFENCE.VMA left an unrelated context alone",
            "an unrelated context's entry was removed");

    // By page: only the named page of the named ASID goes.
    const uint32_t l14 = kRamBase + 0x15A0;
    const uint32_t l15 = kRamBase + 0x15C0;
    h_->WriteLine(l14, 0xA500);
    h_->WriteLine(l15, 0xA600);
    h_->Fill(l14, kVpnA, 1, kPermRW);
    h_->Fill(l15, kVpnB, 1, kPermRW);
    ExpectHitWithMem(l14, kVpnA, 1, kPermRW, false);   // hit before
    h_->PulseFence(kFenceSfence, true, 1, true, kVpnA);
    Require(h_->FindEntry(l14, 1, kPermRW) < 0, "",
            "SFENCE.VMA removed the named page", "the named page survived");
    Require(h_->FindEntry(l15, 1, kPermRW) >= 0, "",
            "SFENCE.VMA left another page alone", "another page was removed");

    // The all-zero form removes everything.
    h_->PulseFence(kFenceSfence, false, 0, false, 0);
    Require(h_->Count() == 0, "", "the all-contexts fence removed every entry",
            Dec(h_->Count()) + " entries survived");
  }

  // ----------------------------------------------------------- context switch
  void ContextSwitch() {
    h_->Phase("asid-switch");
    const uint32_t l16 = kRamBase + 0x1600;
    h_->WriteLine(l16, 0x6100);
    h_->Fill(l16, kVpnA, 7, kPermRW);
    ExpectHitWithMem(l16, kVpnA, 7, kPermRW, false);   // hit before

    // The address space changes (a `satp` write or an ASID switch), and the
    // new context's physical page at the same PA now holds different content.
    h_->WriteLine(l16, 0x6200);
    h_->PulseCtx();
    Require(h_->FindEntry(l16, 7, kPermRW) < 0, "",
            "the context change left no translation-backed copy reachable",
            "an entry survived the context change");
    Probe p = h_->ProbeLookup(l16, kVpnA, 7, kPermRW, false);
    Require(!p.hit, "", "a miss after the context change",
            "a stale hit after the context change");
    h_->CommitLookup();

    // The same ASID number, reused for the new address space, still sees fresh
    // data: the flush is what protects it, not the key.
    h_->Fill(l16, kVpnA, 7, kPermRW);
    ExpectHitWithMem(l16, kVpnA, 7, kPermRW, false);
  }

  // ------------------------------------------------------------ conservation
  void Conservation() {
    h_->Phase("conservation");
    // A fill while the same line is being invalidated is refused.
    const uint32_t l17 = kRamBase + 0x1700;
    h_->WriteLine(l17, 0x7100);
    h_->Fill(l17, kVpnA, 1, kPermRW);
    const uint32_t refused_before = h_->FillRefusedCtr();
    h_->SetFill(l17, kVpnA, 1, kPermRW);
    h_->SetStore(l17);
    const bool ok = h_->PresentFillOk();
    h_->Edge();
    h_->Idle();
    h_->Eval();
    Require(!ok, "", "a fill racing an invalidation is refused", "the fill was accepted");
    Require(h_->FillRefusedCtr() > refused_before, "",
            "the racing fill was counted as refused", "it was not counted");
    Require(h_->FindEntry(l17, 1, kPermRW) < 0, "",
            "the racing fill installed nothing", "something was installed");

    // Occupancy: every valid entry is findable through the debug port.
    uint32_t valid = 0;
    for (uint32_t i = 0; i < kEntries; ++i) {
      if (h_->Dbg(i).valid) ++valid;
    }
    Require(valid == h_->Count(), "",
            "o_count equals the debug port's live entries",
            "o_count=" + Dec(h_->Count()) + " debug=" + Dec(valid));

    // Coverage: every source this case claims to exercise moved a counter.
    Require(h_->HitCtr() > 0, "", "hits covered", "no hits");
    Require(h_->MissCtr() > 0, "", "misses covered", "no misses");
    Require(h_->BypassCtr() > 0, "", "bypasses covered", "no bypasses");
    Require(h_->FillCtr() > 0, "", "fills covered", "no fills");
    Require(h_->FillRefusedCtr() > 0, "", "refused fills covered", "none");
    Require(h_->InvCtr() > 0, "", "invaldations covered", "none");
  }

  std::string Summary() {
    char detail[320];
    std::snprintf(detail, sizeof(detail),
                  "llb: hit=%u miss=%u bypass=%u fill=%u fill_refused=%u inv=%u "
                  "race_refuse=%u checks=%d cycles=%llu",
                  h_->HitCtr(), h_->MissCtr(), h_->BypassCtr(), h_->FillCtr(),
                  h_->FillRefusedCtr(), h_->InvCtr(), h_->RaceRefuseCtr(),
                  rep_->checks(), static_cast<unsigned long long>(h_->Cycles()));
    return std::string(detail);
  }

 private:
  void Require(bool ok, const std::string& where, const std::string& expected,
               const std::string& actual) {
    h_->Require(ok, where, expected, actual);
  }

  // A hit that must equal the host memory's current content, word for word.
  void ExpectHitWithMem(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms,
                        bool atomic) {
    Probe p = h_->ProbeLookup(pa, vpn, asid, perms, atomic);
    h_->Require(p.hit, "", "a hit on " + U64(pa), "a miss on " + U64(pa));
    const Lanes want = h_->MemLine(pa);
    h_->Require(p.data == want, "",
                "the hit carries the memory's current line",
                "the hit carries a different line (stale)");
    h_->CommitLookup();
  }

  Harness* h_;
  mosaic::Reporter* rep_;

  // Distinct virtual pages for the aliases. These live in different VAs and
  // map to the same PA in this driver's translation model; only the alias phase
  // cares, because the identity is the physical line.
  static constexpr uint32_t kVpnA = 0x0000001;
  static constexpr uint32_t kVpnB = 0x0000002;
  static constexpr uint32_t kVpnC = 0x0000003;
};

// ============================================================================
// V-061 -- llb.freshness_and_ownership
//
// `llb.stale_copy_invalidation` (I-060) proves the invalidation *protocol* of
// `mosaic_llb`. This campaign is the V-061 card over the same DUT, and reuses
// the same Harness, host-memory oracle and primitives rather than standing up a
// second model. It adds the directed sequences the card enumerates that the
// I-060 case does not carry as its subject: another hart's store, a DMA write,
// same-address aliasing in both directions, the PMA non-cacheable regions, the
// translation-context dimension of ownership, and a speculative
// (prefetch-style) fill or hit racing an invalidation.
//
// The invariant every phase leans on is stated once: **the LLB is a locality
// structure, not a source of truth.** Under this driver's single-hart
// sequential memory model the only observation a load to `pa` is allowed to see
// is the *current* content of `mem_` at that line, so every hit is compared
// word for word against `mem_` at the moment of the hit, and at rest every
// resident entry is compared against `mem_` again (`provenance-invariant`).
// ============================================================================
class FreshnessCampaign {
 public:
  FreshnessCampaign(Harness* h, mosaic::Reporter* rep) : h_(h), rep_(rep) {}

  std::string Run() {
    Geometry();
    DirectedHitMissBypass();
    LocalWriterFreshness();
    RemoteHartStore();
    DmaWrite();
    SameAddressAliasing();
    PmaNonCacheable();
    OwnershipContextChange();
    MispredictionRace();
    ProvenanceInvariant();
    Conservation();
    return Summary();
  }

  // A profile whose map declares RAM non-cacheable has nothing the LLB is
  // permitted to hold, so the freshness sequences have no subject. What is
  // still real -- and still checked -- is that nothing is cached anyway.
  std::string RunNonCacheableProfile() {
    Geometry();
    BypassAllRegions();
    return Summary();
  }

 private:
  void Require(bool ok, const std::string& where, const std::string& expected,
               const std::string& actual) {
    h_->Require(ok, where, expected, actual);
  }

  // A hit that must equal the host memory's current content, word for word:
  // the "allowed memory observation" the card's Pass criterion names.
  void ExpectHitCurrent(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms,
                        bool atomic) {
    Probe p = h_->ProbeLookup(pa, vpn, asid, perms, atomic);
    h_->Require(p.hit, "", "a hit on " + U64(pa), "a miss on " + U64(pa));
    const Lanes want = h_->MemLine(pa);
    h_->Require(p.data == want, "",
                "the hit carries the memory's current line",
                "the hit carries a different line (stale)");
    h_->CommitLookup();
  }

  void Geometry() {
    h_->Phase("geometry");
    Require(h_->Entries() == kEntries, "",
            "o_entries=" + Dec(kEntries), "o_entries=" + Dec(h_->Entries()));
    Require(h_->LineBytes() == kLineBytes, "",
            "o_line_bytes=" + Dec(kLineBytes), "o_line_bytes=" + Dec(h_->LineBytes()));
    Require(h_->Count() == 0, "", "0 valid entries after reset",
            Dec(h_->Count()) + " valid entries after reset");
  }

  // ---------------------------------------------------- hit / miss / bypass
  // The three architectural outcomes of a lookup, each driven directly and
  // checked against the oracle rather than inferred from a counter.
  void DirectedHitMissBypass() {
    h_->Phase("hit-miss-bypass");
    const uint32_t l1 = kRamBase + 0x2000;
    h_->WriteLine(l1, 0x1100);

    // miss: nothing has been filled.
    const uint32_t miss_before = h_->MissCtr();
    Probe m = h_->ProbeLookup(l1, kVpnA, 1, kPermRW, false);
    Require(!m.hit && !m.bypass, "hit-miss-bypass",
            "a miss on an unfilled line", "a hit on an unfilled line");
    h_->CommitLookup();
    Require(h_->MissCtr() > miss_before, "hit-miss-bypass",
            "the miss is counted", "the miss was not counted");

    // hit: the fill took the memory's current content, and the hit returns it.
    h_->Fill(l1, kVpnA, 1, kPermRW);
    const uint32_t hit_before = h_->HitCtr();
    ExpectHitCurrent(l1, kVpnA, 1, kPermRW, false);
    Require(h_->HitCtr() > hit_before, "hit-miss-bypass",
            "the hit is counted", "the hit was not counted");

    // bypass: an atomic and a non-cacheable line are never served.
    Probe a = h_->ProbeLookup(l1, kVpnA, 1, kPermRW, true);
    Require(!a.hit && a.bypass, "hit-miss-bypass",
            "an atomic is bypassed", "an atomic was served");
    h_->CommitLookup();
    Probe d = h_->ProbeLookup(kUartBase, kVpnA, 1, kPermRW, false);
    Require(!d.hit && d.bypass, "hit-miss-bypass",
            "a device lookup is bypassed", "a device lookup was served");
    h_->CommitLookup();

    // A bypassed atomic must not have evicted the line it bypassed.
    Require(h_->FindEntry(l1, 1, kPermRW) >= 0, "hit-miss-bypass",
            "an atomic does not evict the line it bypassed",
            "the bypassed line disappeared");
    ExpectHitCurrent(l1, kVpnA, 1, kPermRW, false);
  }

  // --------------------------------------------------- local writer freshness
  // The card's central fail mode: a clean copy treated as needing no coherence.
  // A local store is a writer; the copy is stale the moment it commits.
  void LocalWriterFreshness() {
    h_->Phase("local-writer-freshness");
    const uint32_t l = kRamBase + 0x2040;
    const uint32_t other = kRamBase + 0x2060;
    h_->WriteLine(l, 0x1200);
    h_->WriteLine(other, 0x1300);
    h_->Fill(l, kVpnA, 1, kPermRW);
    h_->Fill(other, kVpnA, 1, kPermRW);
    ExpectHitCurrent(l, kVpnA, 1, kPermRW, false);   // hit before

    h_->WriteLine(l, 0x1400);       // the store makes the new value visible
    h_->PulseStore(l);
    Require(h_->FindEntry(l, 1, kPermRW) < 0, "local-writer-freshness",
            "the store removed the clean copy",
            "the clean copy survived the store");
    Require(h_->FindEntry(other, 1, kPermRW) >= 0, "local-writer-freshness",
            "an unrelated copy survives the store",
            "an unrelated copy was removed");
    Probe p = h_->ProbeLookup(l, kVpnA, 1, kPermRW, false);
    Require(!p.hit, "local-writer-freshness",
            "a miss after the store", "a hit after the store");
    h_->CommitLookup();
    h_->Fill(l, kVpnA, 1, kPermRW);
    ExpectHitCurrent(l, kVpnA, 1, kPermRW, false);   // the fresh copy hits
  }

  // ------------------------------------------------------ another hart's store
  // A remote hart's store reaches this hart's LLB as a coherence snoop. The
  // value is visible to memory before the invalidation, so a survivor here is a
  // stale read.
  void RemoteHartStore() {
    h_->Phase("remote-hart-store");
    const uint32_t l = kRamBase + 0x2080;
    const uint32_t other = kRamBase + 0x20a0;
    h_->WriteLine(l, 0x2100);
    h_->WriteLine(other, 0x2200);
    h_->Fill(l, kVpnA, 1, kPermRW);
    h_->Fill(other, kVpnA, 1, kPermRW);
    ExpectHitCurrent(l, kVpnA, 1, kPermRW, false);   // hit before

    h_->WriteLine(l, 0x2300);       // hart 1 stores
    h_->PulseSnoop(l);
    Require(h_->FindEntry(l, 1, kPermRW) < 0, "remote-hart-store",
            "hart 1's store removed this hart's copy",
            "the copy survived hart 1's store");
    Require(h_->FindEntry(other, 1, kPermRW) >= 0, "remote-hart-store",
            "an unrelated copy survives another hart's store",
            "an unrelated copy was removed");
    Probe p = h_->ProbeLookup(l, kVpnA, 1, kPermRW, false);
    Require(!p.hit, "remote-hart-store",
            "a miss after hart 1's store", "a stale hit after hart 1's store");
    h_->CommitLookup();
    h_->Fill(l, kVpnA, 1, kPermRW);
    ExpectHitCurrent(l, kVpnA, 1, kPermRW, false);   // equals hart 1's value
  }

  // ------------------------------------------------------------------- DMA
  // A DMA engine is an external writer with no hart context: it is the same
  // snoop interface, and a DMA flush broadcast is a shootdown.
  void DmaWrite() {
    h_->Phase("dma-write");
    const uint32_t l = kRamBase + 0x20c0;
    const uint32_t other = kRamBase + 0x20e0;
    h_->WriteLine(l, 0x3100);
    h_->WriteLine(other, 0x3200);
    h_->Fill(l, kVpnA, 1, kPermRW);
    h_->Fill(other, kVpnA, 1, kPermRW);
    ExpectHitCurrent(l, kVpnA, 1, kPermRW, false);   // hit before

    h_->WriteLine(l, 0x3300);       // the DMA engine wrote the line
    h_->PulseSnoop(l);
    Require(h_->FindEntry(l, 1, kPermRW) < 0, "dma-write",
            "the DMA's line was removed", "the DMA's line is still resident");
    Require(h_->FindEntry(other, 1, kPermRW) >= 0, "dma-write",
            "an unrelated line survives a DMA write", "an unrelated line was removed");
    Probe p = h_->ProbeLookup(l, kVpnA, 1, kPermRW, false);
    Require(!p.hit && !p.bypass, "dma-write",
            "a miss after the DMA write", "a hit with the pre-DMA value");
    h_->CommitLookup();
    h_->Fill(l, kVpnA, 1, kPermRW);
    ExpectHitCurrent(l, kVpnA, 1, kPermRW, false);

    // A DMA flush / shootdown broadcast removes every copy.
    Require(h_->Count() >= 2, "dma-write",
            "copies are resident before the flush",
            Dec(h_->Count()) + " copies before the flush");
    h_->PulseSnoopAll();
    Require(h_->Count() == 0, "dma-write",
            "the DMA flush removed every copy",
            Dec(h_->Count()) + " copies survived");
  }

  // ------------------------------------------------- same-address aliasing
  // Both directions: the same physical line through two VAs shares one copy,
  // and the same VA to a different PA is a different line. The identity is the
  // physical line, and neither the VA nor the sub-line offset may enter it.
  void SameAddressAliasing() {
    h_->Phase("same-address-aliasing");
    const uint32_t l1 = kRamBase + 0x2100;
    const uint32_t l2 = kRamBase + 0x2120;
    h_->WriteLine(l1, 0x4100);
    h_->WriteLine(l2, 0x4200);

    // Same PA, two VAs: the second alias hits the one physical copy.
    h_->Fill(l1, kVpnA, 1, kPermRW);
    ExpectHitCurrent(l1, kVpnA, 1, kPermRW, false);
    ExpectHitCurrent(l1, kVpnB, 1, kPermRW, false);

    // Same VA, a different PA: a VA-keyed LLB would serve l1's data for l2.
    Probe wrong = h_->ProbeLookup(l2, kVpnA, 1, kPermRW, false);
    Require(!wrong.hit, "same-address-aliasing",
            "same VA, different PA misses",
            "an alias hit that would serve another line's data");
    h_->CommitLookup();

    // A sub-line address of the filled line still hits its line (the offset is
    // not part of the identity), while the same offset of another line does
    // not (the offset alone must not match).
    ExpectHitCurrent(l1 + 0x18, kVpnA, 1, kPermRW, false);
    Probe off = h_->ProbeLookup(l2 + 0x18, kVpnA, 1, kPermRW, false);
    Require(!off.hit, "same-address-aliasing",
            "a same-offset different line misses",
            "a hit on low address bits alone");
    h_->CommitLookup();

    // Same PA, a different ASID: a different address space is a different
    // owner and must not be served the other's copy.
    Probe asid = h_->ProbeLookup(l1, kVpnA, 2, kPermRW, false);
    Require(!asid.hit, "same-address-aliasing",
            "same PA, different ASID misses", "a hit across address spaces");
    h_->CommitLookup();
  }

  // ------------------------------------------------- PMA non-cacheable
  // The map agreement the bypass rule rests on: every region this profile
  // declares non-idempotent is also non-cacheable, so gating a fill on
  // cacheability keeps non-idempotent addresses out. Constants come from the
  // generated header, not a hand-written list.
  void PmaNonCacheable() {
    h_->Phase("pma-non-cacheable");
    Require(MOSAIC_UART_CACHEABLE == 0 && MOSAIC_TEST_HARNESS_CACHEABLE == 0 &&
                MOSAIC_CLINT_CACHEABLE == 0,
            "pma-non-cacheable",
            "every non-idempotent (device) region is declared non-cacheable",
            "a non-idempotent region is declared cacheable");
    Require(MOSAIC_BOOT_ROM_CACHEABLE == 0, "pma-non-cacheable",
            "the non-cacheable ROM is declared non-cacheable",
            "the ROM is declared cacheable");
    Require(MOSAIC_RAM_CACHEABLE == 1, "pma-non-cacheable",
            "a cacheable RAM region exists in this profile",
            "this profile has no cacheable region");

    // A cacheable region is accepted, so the refusals below are not vacuous.
    const uint32_t ram = kRamBase + 0x2200;
    h_->WriteLine(ram, 0x5100);
    h_->Fill(ram, kVpnA, 1, kPermRW);
    Require(h_->FindEntry(ram, 1, kPermRW) >= 0, "pma-non-cacheable",
            "a cacheable RAM line is filled", "a cacheable RAM line was refused");
    ExpectHitCurrent(ram, kVpnA, 1, kPermRW, false);

    struct Region { const char* name; uint32_t base; };
    const Region regions[] = {
        {"uart", kUartBase},
        {"test-harness", kTestHarnessBase},
        {"clint", kClintBase},
        {"boot-rom", kRomBase},
    };
    for (const Region& r : regions) {
      h_->WriteLine(r.base, 0x6000 + r.base);
      const bool ok = h_->FillExpectRefused(r.base, kVpnA, 1, kPermRW);
      Require(!ok, "pma-non-cacheable",
              std::string(r.name) + " is refused a fill",
              std::string(r.name) + " was accepted");
      Require(h_->FindEntry(r.base, 1, kPermRW) < 0, "pma-non-cacheable",
              std::string(r.name) + " is not resident",
              std::string(r.name) + " became resident");
      Probe p = h_->ProbeLookup(r.base, kVpnA, 1, kPermRW, false);
      Require(!p.hit && p.bypass, "pma-non-cacheable",
              std::string(r.name) + " lookups are bypassed",
              std::string(r.name) + " lookup was served");
      h_->CommitLookup();
    }
  }

  // ---------------------------------------- ownership: translation context
  // A `satp` write / ASID switch reconfigures the *translation* that a copy is
  // reachable through, so every translation-backed copy becomes unreachable --
  // even under a reused ASID number. (A *lane* reassignment / owner-domain
  // transfer is a different mechanism this module does not model; see the
  // report's not-covered list.)
  void OwnershipContextChange() {
    h_->Phase("ownership-context-change");
    const uint32_t l = kRamBase + 0x2300;
    h_->WriteLine(l, 0x7100);
    h_->Fill(l, kVpnA, 7, kPermRW);
    ExpectHitCurrent(l, kVpnA, 7, kPermRW, false);   // hit before

    h_->WriteLine(l, 0x7200);
    h_->PulseCtx();
    Require(h_->FindEntry(l, 7, kPermRW) < 0, "ownership-context-change",
            "the context change left no copy reachable",
            "a copy survived the context change");
    Probe p = h_->ProbeLookup(l, kVpnA, 7, kPermRW, false);
    Require(!p.hit, "ownership-context-change",
            "a miss under the new context", "a stale hit under the new context");
    h_->CommitLookup();
    // The reused ASID number sees fresh data: the flush is what protects it.
    h_->Fill(l, kVpnA, 7, kPermRW);
    ExpectHitCurrent(l, kVpnA, 7, kPermRW, false);
  }

  // ------------------------------------- misprediction racing invalidation
  // A prefetch/predictor installs speculative copies. A wrong prediction may
  // change latency and traffic -- it must never change the value a demand
  // access sees. Two races are driven: a speculative *fill* arriving in the
  // same cycle as an invalidation of its line (must be refused, so no
  // about-to-be-superseded line is installed), and a speculative *hit* in the
  // same cycle as the invalidating writer (refused, or already the new value).
  void MispredictionRace() {
    h_->Phase("misprediction-race");
    const uint32_t base = kRamBase + 0x2400;
    const char* names[8] = {"store",  "refill",  "snoop",   "snoop-all",
                            "fence",  "fence-i", "sfence",  "ctx"};

    for (int source = 0; source < 8; ++source) {
      const uint32_t line = base + 0x40 * static_cast<uint32_t>(source);
      h_->WriteLine(line, 0x8000 + 0x100 * static_cast<uint32_t>(source));
      h_->Fill(line, kVpnA, 1, kPermRW);             // the speculative copy
      ExpectHitCurrent(line, kVpnA, 1, kPermRW, false);   // hit before

      // The writer makes a new value visible; the predictor's fill for the same
      // line arrives in the invalidating cycle.
      h_->WriteLine(line, 0x8800 + 0x100 * static_cast<uint32_t>(source));
      const bool installed = h_->FillRacingInvalidate(line, kVpnA, 1, kPermRW, source);
      const std::string twe = std::string("misprediction-race/fill-") + names[source];
      Require(!installed, twe,
              "a speculative fill racing an invalidation is refused",
              "the racing fill was installed");
      Require(h_->FindEntry(line, 1, kPermRW) < 0, twe,
              "no copy is resident after the racing fill", "a copy is resident");

      // The demand access that follows must miss (latency), and the value it
      // finally sees is memory's, never the predictor's speculative copy.
      Probe p = h_->ProbeLookup(line, kVpnA, 1, kPermRW, false);
      if (p.hit) {
        Require(p.data == h_->MemLine(line), twe,
                "no hit after the refused fill (or the post-write value)",
                "a hit with the pre-write value");
      }
      h_->CommitLookup();
      h_->Fill(line, kVpnA, 1, kPermRW);
      ExpectHitCurrent(line, kVpnA, 1, kPermRW, false);
    }

    // A speculative hit racing the invalidating writer.
    const uint32_t hline = kRamBase + 0x2800;
    h_->WriteLine(hline, 0x9000);
    h_->Fill(hline, kVpnA, 1, kPermRW);
    ExpectHitCurrent(hline, kVpnA, 1, kPermRW, false);   // hit before
    h_->WriteLine(hline, 0x9100);
    Probe race = h_->ConflictProbe(hline, kVpnA, 1, kPermRW, 0);   // store source
    if (race.hit) {
      Require(race.data == h_->MemLine(hline), "misprediction-race/hit-store",
              "no hit in the invalidating cycle (or the post-store value)",
              "a hit with the pre-store value");
    }
    Require(h_->FindEntry(hline, 1, kPermRW) < 0, "misprediction-race/hit-store",
            "the invalidating store removed the line", "the line survived");
  }

  // ------------------------------------------------------- provenance at rest
  // The global form of "every hit is consistent with an allowed memory
  // observation": when the campaign is at rest, every resident copy must equal
  // memory's current content for its line. A clean copy that was treated as
  // needing no coherence shows up here as a mismatch.
  void ProvenanceInvariant() {
    h_->Phase("provenance-invariant");
    uint32_t checked = 0;
    for (uint32_t i = 0; i < kEntries; ++i) {
      Entry e = h_->Dbg(i);
      if (!e.valid) continue;
      ++checked;
      const Lanes want = h_->MemLine(e.line);
      Require(e.data == want, "provenance-invariant",
              "resident entry " + Dec(i) + " equals memory's line",
              "resident entry " + Dec(i) + " holds a stale line");
    }
    Require(checked > 0, "provenance-invariant",
            "at least one resident copy was compared against memory",
            "no copy was resident to compare");
  }

  // ----------------------------------------------------------- conservation
  void Conservation() {
    h_->Phase("conservation");
    uint32_t valid = 0;
    for (uint32_t i = 0; i < kEntries; ++i) {
      if (h_->Dbg(i).valid) ++valid;
    }
    Require(valid == h_->Count(), "",
            "o_count equals the debug port's live entries",
            "o_count=" + Dec(h_->Count()) + " debug=" + Dec(valid));
    Require(h_->HitCtr() > 0, "", "hits covered", "no hits");
    Require(h_->MissCtr() > 0, "", "misses covered", "no misses");
    Require(h_->BypassCtr() > 0, "", "bypasses covered", "no bypasses");
    Require(h_->FillCtr() > 0, "", "fills covered", "no fills");
    Require(h_->FillRefusedCtr() > 0, "", "refused fills covered", "none");
    Require(h_->InvCtr() > 0, "", "invalidations covered", "none");
    Require(h_->RaceRefuseCtr() > 0, "", "racing-hit refusals covered", "none");
  }

  void BypassAllRegions() {
    h_->Phase("non-cacheable-ram");
    const uint32_t line = kRamBase + 0x1000;
    h_->WriteLine(line, 0xA100);
    Require(!h_->FillExpectRefused(line, kVpnA, 1, kPermRW), "non-cacheable-ram",
            "a RAM fill is refused under a non-cacheable map",
            "a RAM fill was accepted");
    Require(h_->FindEntry(line, 1, kPermRW) < 0, "non-cacheable-ram",
            "no RAM line is resident", "a RAM line is resident");
    Probe p = h_->ProbeLookup(line, kVpnA, 1, kPermRW, false);
    Require(!p.hit && p.bypass, "non-cacheable-ram",
            "a RAM lookup is bypassed", "a RAM lookup was served");
    h_->CommitLookup();
    Require(h_->Count() == 0, "non-cacheable-ram",
            "the buffer is empty",
            Dec(h_->Count()) + " entries were cached");
    Require(h_->BypassCtr() > 0 && h_->FillRefusedCtr() > 0, "non-cacheable-ram",
            "bypass and refusal are covered", "a counter did not move");
  }

  std::string Summary() {
    char detail[360];
    std::snprintf(detail, sizeof(detail),
                  "llb-fresh: hit=%u miss=%u bypass=%u fill=%u fill_refused=%u "
                  "inv=%u race_refuse=%u checks=%d cycles=%llu",
                  h_->HitCtr(), h_->MissCtr(), h_->BypassCtr(), h_->FillCtr(),
                  h_->FillRefusedCtr(), h_->InvCtr(), h_->RaceRefuseCtr(),
                  rep_->checks(), static_cast<unsigned long long>(h_->Cycles()));
    return std::string(detail);
  }

  Harness* h_;
  mosaic::Reporter* rep_;

  static constexpr uint32_t kVpnA = 0x0000001;
  static constexpr uint32_t kVpnB = 0x0000002;
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
  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  mosaic::ClockDriver clk;

  Vmosaic_llb_tb dut;
  dut.clk = 0;
  dut.rst = 0;
  dut.eval();

  bool passed = true;
  std::string detail;
  try {
    Harness harness(&dut, &clk, &reporter, options.max_cycles);
    if (options.case_id == "llb.freshness_and_ownership") {
      // The V-061 campaign over the same DUT and the same oracle.
      FreshnessCampaign campaign(&harness, &reporter);
      if (!kRamCacheable) {
        detail = campaign.RunNonCacheableProfile() +
                 " (profile declares RAM non-cacheable: the map-driven bypass is "
                 "verified; the freshness rules need a cacheable region)";
      } else {
        detail = campaign.Run();
      }
    } else {
      Campaign campaign(&harness, &reporter);
      if (!kRamCacheable) {
        // This profile's platform map declares RAM non-cacheable, so the
        // freshness campaign has nothing it is permitted to cache. The bypass
        // rules are still real and are still checked -- a profile where nothing
        // may be cached must not silently cache anyway.
        detail = campaign.RunNonCacheableProfile() +
                 " (profile declares RAM non-cacheable: the map-driven bypass is "
                 "verified; the freshness rules need a cacheable region)";
      } else {
        detail = campaign.Run();
      }
    }
  } catch (const std::exception& f) {
    passed = false;
    detail = "first failure: " + std::string(f.what());
    reporter.Mismatch("run", "every rule of the invalidation protocol holds",
                      "a rule was violated");
  }

  dut.final();
  reporter.Check(passed, "no rule of the invalidation protocol was violated");
  return reporter.Finish(passed && reporter.failures() == 0 ? "PASS" : "FAIL", detail);
}
