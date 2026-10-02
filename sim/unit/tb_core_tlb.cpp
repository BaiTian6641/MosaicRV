// ============================================================================
// tb_core_tlb.cpp -- CASE=tlb.sfence_vma, work package I-046.
//
// Two halves, and they are deliberately separate:
//
//   1. **The translation cache itself**, driven directly. `sim/tb/mosaic_core_tb.sv`
//      instantiates a standalone `mosaic_tlb` beside the core; this driver owns
//      its physical PTE port, builds the page tables in a memory model, serves
//      the walker's PTE reads and writes, and can read the tables back. Every
//      property the card names is checked here because it is observable here:
//      * a hit *is* the absence of a PTE read (the only honest proof that a
//        cache is used) and a miss is the presence of the walk's reads;
//      * a modified PTE is stale until the matching SFENCE.VMA, and is not
//        picked up without one;
//      * all four SFENCE.VMA operand forms, what each invalidates, and the
//        non-over-invalidation direction (a per-ASID fence keeps another
//        address space's entry, and keeps a global mapping even at the same
//        ASID);
//      * two address spaces with the same virtual address, a satp->ASID change
//        and ASID 0;
//      * a cancelled walk installs nothing;
//      * a walk that spans a fence installs nothing (the late/stale response
//        rule);
//      * the walker's A/D update is what the re-installed entry carries, so a
//        store is never served from an entry whose D is clear.
//
//   2. **The SFENCE.VMA instruction**, through the core. A small M-mode setup
//      programs `satp` (Sv39, a nonzero ASID) and enters S-mode; the S-mode
//      stub loads, loads again (a hit), patches the PTE, executes a real
//      `SFENCE.VMA x0, x0`, and loads a third time. The third load must observe
//      the new mapping, which is exactly what a TLB that treated the fence as a
//      no-op would get wrong.
//
// ---------------------------------------------------------------- not covered
//
// Instruction fetch is not translated (I-045's report says so and this package
// does not change it), so the TLB is exercised on the data path only. It is not
// *shared* with fetch yet -- there is no fetch translation to share with -- and
// the report says what was done instead. The cache is single-hart; cross-hart
// shootdown is I-046's later, multi-hart work.
// ============================================================================

#include <verilated.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "Vmosaic_core_tb.h"
#include "memory_model.h"
#include "mosaic_platform.h"
#include "sim_common.h"

using mosaic::AccessStatus;
using mosaic::MemoryModel;

namespace {

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw std::runtime_error(where + ": " + detail);
}

std::string U64(uint64_t v) { return mosaic::Hex(v); }
std::string Dec(uint64_t v) { return std::to_string(v); }
bool Debug() { return std::getenv("TLB_DEBUG") != nullptr; }

constexpr int kExcInsnPage = 12;
constexpr int kExcLoadPage = 13;
constexpr int kExcStorePage = 15;

constexpr int kKindLoad = 0, kKindStore = 1, kKindFetch = 2;
constexpr int kPrivU = 0, kPrivS = 1, kPrivM = 3;

constexpr uint64_t kSatpBare = 0;
constexpr uint64_t kSatpSv39 = 8;

// ------------------------------------------------------------- physical layout
constexpr uint64_t kRamBase = MOSAIC_RAM_BASE;               // 0x8000_0000
constexpr uint64_t kRoot     = kRamBase + 0x10000;
constexpr uint64_t kL1       = kRamBase + 0x11000;
constexpr uint64_t kL0       = kRamBase + 0x12000;
constexpr uint64_t kRoot2    = kRamBase + 0x13000;
constexpr uint64_t kL1b      = kRamBase + 0x14000;
constexpr uint64_t kL0b      = kRamBase + 0x15000;
constexpr uint64_t kPageA    = kRamBase + 0x20000;
constexpr uint64_t kPageB    = kRamBase + 0x21000;
constexpr uint64_t kPageC    = kRamBase + 0x22000;
constexpr uint64_t kPageD    = kRamBase + 0x23000;
constexpr uint64_t kPageE    = kRamBase + 0x24000;
constexpr uint64_t kPageZ    = kRamBase + 0x25000;

uint64_t Pp(uint64_t page_addr) { return page_addr >> 12; }

uint64_t Pte(uint64_t ppn, bool v, bool r, bool w, bool x, bool u, bool g,
             bool a, bool d) {
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
uint64_t PteNonleaf(uint64_t next) {
  return Pte(Pp(next), true, false, false, false, false, false, false, false);
}

// vpn2 = 1, vpn1 = 1, and vpn0 selects the 4 KiB page; the set index of the
// cache is va[15:13], which is vpn0[3:1].
uint64_t Va(uint32_t vpn0, uint32_t off = 0) {
  return (1ull << 30) | (1ull << 21) | (static_cast<uint64_t>(vpn0) << 12) | off;
}
constexpr uint64_t kNonCanonical = UINT64_C(0x0000008000000000);

uint64_t RootPpn() { return Pp(kRoot); }
uint64_t Root2Ppn() { return Pp(kRoot2); }

// ---------------------------------------------------------------- memory helpers
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
bool MemWrite8(MemoryModel* mem, uint64_t addr, uint64_t value) {
  uint64_t window = 0;
  if (!MemRead8(mem, addr, &window)) return false;
  return mem->Write(addr, 8, value) == AccessStatus::kOk;
}

// ============================================================================
// The directed bench: drives the standalone cache and its PTE memory.
// ============================================================================
struct TlbObs {
  bool ready = false, rsp_valid = false, fault = false, bare = false, hit = false;
  uint64_t pa = 0, tval = 0;
  uint8_t cause = 0, perms = 0, attr = 0;
  bool mem_req_valid = false, mem_we = false;
  uint64_t mem_addr = 0, mem_wdata = 0;
  uint8_t mem_wstrb = 0;
};

class TlbBench {
 public:
  TlbBench(Vmosaic_core_tb* dut, MemoryModel* mem) : dut_(dut), mem_(mem) {}

  uint64_t pte_reads() const { return reads_; }
  const std::vector<uint64_t>& writes() const { return writes_; }

  void Idle() {
    dut_->tlb_xl_valid_i = 0;
    dut_->tlb_xl_va_i = 0;
    dut_->tlb_xl_kind_i = 0;
    dut_->tlb_xl_priv_i = 0;
    dut_->tlb_xl_mode_i = 0;
    dut_->tlb_xl_ppn_i = 0;
    dut_->tlb_xl_asid_i = 0;
    dut_->tlb_xl_sum_i = 0;
    dut_->tlb_xl_mxr_i = 0;
    dut_->tlb_xl_cancel_i = 0;
    dut_->tlb_xl_rsp_ready_i = 1;
    dut_->tlb_sfence_valid_i = 0;
    dut_->tlb_sfence_va_i = 0;
    dut_->tlb_sfence_has_va_i = 0;
    dut_->tlb_sfence_asid_i = 0;
    dut_->tlb_sfence_has_asid_i = 0;
    dut_->tlb_satp_write_i = 0;
    dut_->tlb_mem_req_ready_i = 1;
    dut_->tlb_mem_rsp_valid_i = 0;
    dut_->tlb_mem_rsp_rdata_i = 0;
    dut_->tlb_mem_rsp_fault_i = 0;
  }

  void Reset(int n) {
    Idle();
    dut_->rst = 1;
    for (int i = 0; i < n; i++) Cycle();
    dut_->rst = 0;
    Cycle();
  }

  // Present a request and return once the cache has accepted it. The acceptance
  // is sampled combinationally each cycle, so the request may wait for the
  // previous response to be consumed.
  void Drive(uint64_t va, int kind, int priv, uint64_t mode, uint64_t ppn,
             uint16_t asid, bool sum, bool mxr) {
    dut_->tlb_xl_valid_i = 1;
    dut_->tlb_xl_va_i = va;
    dut_->tlb_xl_kind_i = static_cast<uint8_t>(kind);
    dut_->tlb_xl_priv_i = static_cast<uint8_t>(priv);
    dut_->tlb_xl_mode_i = static_cast<uint8_t>(mode);
    dut_->tlb_xl_ppn_i = ppn & ((UINT64_C(1) << 44) - 1);
    dut_->tlb_xl_asid_i = asid;
    dut_->tlb_xl_sum_i = sum ? 1 : 0;
    dut_->tlb_xl_mxr_i = mxr ? 1 : 0;
    bool accepted = false;
    for (int i = 0; i < 16 && !accepted; i++) {
      Cycle();
      accepted = observed_.ready;
    }
    if (!accepted) Fail("directed", "the cache did not accept a request");
    dut_->tlb_xl_valid_i = 0;
  }

  TlbObs Cycle(bool cancel = false) {
    dut_->tlb_xl_cancel_i = cancel ? 1 : 0;
    dut_->tlb_mem_rsp_valid_i = ready_.empty() ? 0 : 1;
    if (!ready_.empty()) {
      dut_->tlb_mem_rsp_rdata_i = ready_.front().rdata;
      dut_->tlb_mem_rsp_fault_i = ready_.front().fault ? 1 : 0;
    }
    dut_->clk = 0;
    dut_->eval();
    TlbObs o;
    o.ready = dut_->tlb_xl_ready_o != 0;
    o.rsp_valid = dut_->tlb_xl_rsp_valid_o != 0;
    o.fault = dut_->tlb_xl_fault_o != 0;
    o.bare = dut_->tlb_xl_bare_o != 0;
    o.hit = dut_->o_tlb_hit_o != 0;
    o.pa = dut_->tlb_xl_pa_o;
    o.cause = static_cast<uint8_t>(dut_->tlb_xl_cause_o);
    o.tval = dut_->tlb_xl_tval_o;
    o.perms = static_cast<uint8_t>(dut_->tlb_xl_perms_o);
    o.attr = static_cast<uint8_t>(dut_->tlb_xl_attr_o);
    o.mem_req_valid = dut_->tlb_mem_req_valid_o != 0;
    o.mem_we = dut_->tlb_mem_req_we_o != 0;
    o.mem_addr = dut_->tlb_mem_req_addr_o;
    o.mem_wdata = dut_->tlb_mem_req_wdata_o;
    o.mem_wstrb = static_cast<uint8_t>(dut_->tlb_mem_req_wstrb_o);
    observed_ = o;

    if (o.mem_req_valid) {
      if (o.mem_we) {
        writes_.push_back(o.mem_wdata);
      } else {
        reads_++;
      }
    }
    const bool mem_accept = o.mem_req_valid != 0;
    const bool rsp_taken = !ready_.empty();

    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();

    if (rsp_taken) ready_.pop_front();
    if (mem_accept) {
      Rsp rsp;
      if (o.mem_we) {
        rsp.fault = !MemWrite8(mem_, o.mem_addr, o.mem_wdata);
        rsp.rdata = 0;
      } else {
        rsp.fault = !MemRead8(mem_, o.mem_addr, &rsp.rdata);
        if (rsp.fault) rsp.rdata = 0;
      }
      ready_.push_back(rsp);
    }
    // A fence or satp pulse is a one-cycle event on this bench.
    dut_->tlb_sfence_valid_i = 0;
    dut_->tlb_satp_write_i = 0;
    return o;
  }

  TlbObs RunUntilResponse(uint64_t limit = 64) {
    for (uint64_t i = 0; i < limit; i++) {
      const TlbObs o = Cycle();
      if (o.rsp_valid) return o;
    }
    Fail("directed", "the cache produced no response within " + Dec(limit) + " cycles");
  }

  void Settle(int n) { for (int i = 0; i < n; i++) Cycle(); }

  // A composed request: present, wait for the result.
  TlbObs Request(uint64_t va, int kind, int priv, uint64_t mode, uint64_t ppn,
                 uint16_t asid, bool sum, bool mxr) {
    Drive(va, kind, priv, mode, ppn, asid, sum, mxr);
    return RunUntilResponse();
  }

  void Fence(bool has_va, uint64_t va, bool has_asid, uint16_t asid) {
    dut_->tlb_sfence_has_va_i = has_va ? 1 : 0;
    dut_->tlb_sfence_va_i = va;
    dut_->tlb_sfence_has_asid_i = has_asid ? 1 : 0;
    dut_->tlb_sfence_asid_i = asid;
    dut_->tlb_sfence_valid_i = 1;
    Cycle();
  }

  void SatpWrite() {
    dut_->tlb_satp_write_i = 1;
    Cycle();
  }

  uint32_t hit_ctr() const { return dut_->o_tlb_hit_ctr_o; }
  uint32_t miss_ctr() const { return dut_->o_tlb_miss_ctr_o; }
  uint32_t install_ctr() const { return dut_->o_tlb_install_ctr_o; }
  uint32_t evict_ctr() const { return dut_->o_tlb_evict_ctr_o; }
  uint32_t stale_ctr() const { return dut_->o_tlb_stale_ctr_o; }
  uint32_t sfence_ctr() const { return dut_->o_tlb_sfence_ctr_o; }
  uint32_t satp_flush_ctr() const { return dut_->o_tlb_satp_flush_ctr_o; }
  uint32_t cancel_ctr() const { return dut_->o_tlb_cancel_ctr_o; }
  uint32_t walk_ctr() const { return dut_->o_tlb_walk_ctr_o; }
  uint16_t gen() const { return dut_->o_tlb_gen_o; }

 private:
  struct Rsp { uint64_t rdata = 0; bool fault = false; };
  Vmosaic_core_tb* dut_;
  MemoryModel* mem_;
  uint64_t reads_ = 0;
  std::deque<Rsp> ready_;
  std::vector<uint64_t> writes_;
  TlbObs observed_;
};

// ============================================================================
// The directed page tables
// ============================================================================
void SetLeaf(MemoryModel* mem, uint64_t table, unsigned index, uint64_t page,
             bool g, bool a, bool d) {
  const uint64_t pte = Pte(Pp(page), true, true, true, true, false, g, a, d);
  if (!MemWrite8(mem, table + 8ull * index, pte)) {
    Fail("tables", "cannot write the PTE at " + U64(table + 8ull * index));
  }
}

void BuildTables(MemoryModel* mem) {
  auto zero = [&](uint64_t table) {
    for (unsigned i = 0; i < 512; i++) {
      if (!MemWrite8(mem, table + 8ull * i, 0)) Fail("tables", "zeroing " + U64(table));
    }
  };
  zero(kRoot);
  zero(kL1);
  zero(kL0);
  zero(kRoot2);
  zero(kL1b);
  zero(kL0b);
  if (!MemWrite8(mem, kRoot + 8, PteNonleaf(kL1)) ||
      !MemWrite8(mem, kL1 + 8, PteNonleaf(kL0))) {
    Fail("tables", "cannot build the root/L1 non-leaves");
  }
  // Root 2 is a second address space: kVa(0x10) maps to pageZ under it, so a
  // satp write that only changes the root is observable as a stale entry.
  if (!MemWrite8(mem, kRoot2 + 8, PteNonleaf(kL1b)) ||
      !MemWrite8(mem, kL1b + 8, PteNonleaf(kL0b))) {
    Fail("tables", "cannot build the second root/L1 non-leaves");
  }
  // The leaves. vpn0 is chosen so that va[15:13] -- the cache's set index -- is
  // distinct per entry used together: 0x10->0, 0x12->1, 0x14->2, 0x16->3,
  // 0x18->4, 0x1A->5, 0x1C->6, 0x1E->7.
  SetLeaf(mem, kL0, 0x10, kPageA, false, true, true);
  SetLeaf(mem, kL0, 0x12, kPageB, false, true, true);
  SetLeaf(mem, kL0, 0x14, kPageC, false, true, true);
  SetLeaf(mem, kL0, 0x16, kPageD, false, true, true);
  SetLeaf(mem, kL0, 0x18, kPageE, true, true, true);     // global
  SetLeaf(mem, kL0, 0x1A, kPageA, false, true, false);   // D clear: the A/D check
  SetLeaf(mem, kL0, 0x1C, kPageC, false, true, true);
  SetLeaf(mem, kL0, 0x1E, kPageB, false, true, true);    // the stale test changes this
  SetLeaf(mem, kL0b, 0x10, kPageZ, false, true, true);
}

// ============================================================================
// The directed scenarios
// ============================================================================
void RunDirected(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, MemoryModel* mem,
                 std::map<std::string, int>* coverage, uint64_t* comparisons) {
  BuildTables(mem);
  TlbBench bench(dut, mem);
  bench.Reset(4);

  // ---- 1. a miss walks, a second access to the same page hits ---------------
  {
    (*coverage)["miss"]++;
    (*coverage)["hit"]++;
    const uint64_t va = Va(0x10, 0x18);
    const uint64_t r0 = bench.pte_reads();
    const TlbObs a = bench.Request(va, kKindLoad, kPrivS, kSatpSv39, RootPpn(), 0, false, false);
    const uint64_t walked = bench.pte_reads() - r0;
    (*comparisons)++;
    if (a.fault || a.pa != kPageA + 0x18) {
      reporter->Mismatch("directed/miss", U64(kPageA + 0x18) + " fault=0",
                         U64(a.pa) + " fault=" + Dec(a.fault));
      Fail("directed/miss", "the first load did not translate through the walk");
    }
    if (walked < 3) {
      reporter->Mismatch("directed/miss", "at least 3 PTE reads", Dec(walked));
      Fail("directed/miss", "the first load did not walk the page table");
    }
    const uint64_t r1 = bench.pte_reads();
    const TlbObs b = bench.Request(va, kKindLoad, kPrivS, kSatpSv39, RootPpn(), 0, false, false);
    const uint64_t second = bench.pte_reads() - r1;
    (*comparisons)++;
    if (b.pa != a.pa || b.fault) {
      reporter->Mismatch("directed/hit", U64(a.pa), U64(b.pa));
      Fail("directed/hit", "the second load's translation differed");
    }
    if (second != 0) {
      reporter->Mismatch("directed/hit", "no PTE read on a hit", Dec(second) + " reads");
      Fail("directed/hit", "the second access re-walked: the cache did not hit");
    }
    if (bench.hit_ctr() == 0 || bench.miss_ctr() == 0) {
      Fail("directed/miss", "the cache did not count both a miss and a hit");
    }
    if (Debug()) std::printf("  [hit] first=%llu walk=%llu second=%llu\n",
                             (unsigned long long)r0, (unsigned long long)walked,
                             (unsigned long long)second);
  }

  // ---- 2. stale after a PTE change: used without a fence, gone with one -----
  {
    (*coverage)["stale-without-fence"]++;
    (*coverage)["stale-after-fence"]++;
    const uint64_t va = Va(0x1E);
    const TlbObs a = bench.Request(va, kKindLoad, kPrivS, kSatpSv39, RootPpn(), 0, false, false);
    if (a.fault || a.pa != kPageB) Fail("directed/stale", "setup: load did not map pageB");
    // Change the mapping *without* a fence. The architecture permits either the
    // old or the new translation; this implementation keeps the cached one, and
    // a cache that silently picked up the new one would be the other error.
    SetLeaf(mem, kL0, 0x1E, kPageC, false, true, true);
    const uint64_t r0 = bench.pte_reads();
    const TlbObs b = bench.Request(va, kKindLoad, kPrivS, kSatpSv39, RootPpn(), 0, false, false);
    (*comparisons)++;
    if (b.pa != kPageB || bench.pte_reads() != r0) {
      reporter->Mismatch("directed/stale-without-fence", "the cached mapping " + U64(kPageB),
                         U64(b.pa));
      Fail("directed/stale-without-fence",
           "a modified PTE was picked up without an SFENCE.VMA");
    }
    // The required fence form: rs1 = the address, rs2 = x0 (all address spaces).
    bench.Fence(true, va, false, 0);
    const uint64_t r1 = bench.pte_reads();
    const TlbObs c = bench.Request(va, kKindLoad, kPrivS, kSatpSv39, RootPpn(), 0, false, false);
    (*comparisons)++;
    if (c.pa != kPageC || (bench.pte_reads() - r1) < 3) {
      reporter->Mismatch("directed/stale-after-fence", U64(kPageC), U64(c.pa));
      Fail("directed/stale-after-fence", "the fence did not make the new mapping visible");
    }
  }

  // ---- 3. the four SFENCE.VMA forms, and the conservative direction ---------
  struct Ent { uint64_t va; uint16_t asid; uint64_t page; bool g; };
  struct Form {
    const char* name;
    bool has_va;
    uint64_t va;
    bool has_asid;
    uint16_t asid;
    std::vector<Ent> ents;
    std::vector<size_t> miss;
  };
  // Distinct VAs (hence distinct cache sets) for the whole-cache forms; the
  // same VA with two ASIDs (so two ways) for the address forms.
  const std::vector<Ent> spread = {
      {Va(0x10), 1, kPageA, false},
      {Va(0x12), 2, kPageB, false},
      {Va(0x14), 1, kPageC, false},
      {Va(0x16), 2, kPageD, false},
      {Va(0x18), 2, kPageE, true},   // global, tagged with ASID 2
  };
  const std::vector<Ent> sameva = {
      {Va(0x10), 1, kPageA, false},
      {Va(0x10), 2, kPageA, false},
  };
  const std::vector<Form> forms = {
      {"all", false, 0, false, 0, spread, {0, 1, 2, 3, 4}},
      {"by-asid-1", false, 0, true, 1, spread, {0, 2}},
      {"by-asid-2", false, 0, true, 2, spread, {1, 3}},
      {"by-addr", true, Va(0x10), false, 0, sameva, {0, 1}},
      {"by-addr-asid", true, Va(0x10), true, 2, sameva, {1}},
  };
  for (const Form& f : forms) {
    (*coverage)[std::string("fence-") + f.name]++;
    // Start from an empty cache.
    bench.Fence(false, 0, false, 0);
    for (size_t i = 0; i < f.ents.size(); i++) {
      const Ent& e = f.ents[i];
      const TlbObs o = bench.Request(e.va, kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                     e.asid, false, false);
      if (o.fault || o.pa != e.page) {
        Fail(std::string("directed/fence-") + f.name,
             "install " + Dec(i) + ": got " + U64(o.pa));
      }
    }
    // A global entry matches any ASID: probe with a different one.
    if (strcmp(f.name, "by-asid-1") == 0) {
      const TlbObs g = bench.Request(f.ents[4].va, kKindLoad, kPrivS, kSatpSv39,
                                     RootPpn(), 7, false, false);
      if (g.fault || g.pa != f.ents[4].page) {
        Fail("directed/global", "a global entry missed an ASID");
      }
    }
    bench.Fence(f.has_va, f.va, f.has_asid, f.asid);
    for (size_t i = 0; i < f.ents.size(); i++) {
      const Ent& e = f.ents[i];
      const bool expect_miss = std::find(f.miss.begin(), f.miss.end(), i) != f.miss.end();
      const uint64_t r0 = bench.pte_reads();
      const TlbObs o = bench.Request(e.va, kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                     e.asid, false, false);
      const uint64_t reads = bench.pte_reads() - r0;
      (*comparisons)++;
      if (o.pa != e.page) {
        reporter->Mismatch(std::string("directed/fence-") + f.name,
                           "entry " + Dec(i) + " maps " + U64(e.page), U64(o.pa));
        Fail(std::string("directed/fence-") + f.name, "entry " + Dec(i) + " moved");
      }
      if (expect_miss && reads < 3) {
        reporter->Mismatch(std::string("directed/fence-") + f.name,
                           "entry " + Dec(i) + " invalidated (a walk)",
                           Dec(reads) + " PTE reads");
        Fail(std::string("directed/fence-") + f.name,
             "entry " + Dec(i) + " survived a fence that must have invalidated it");
      }
      if (!expect_miss && reads != 0) {
        reporter->Mismatch(std::string("directed/fence-") + f.name,
                           "entry " + Dec(i) + " survives (a hit)", Dec(reads) + " PTE reads");
        Fail(std::string("directed/fence-") + f.name,
             "entry " + Dec(i) + " was invalidated by a fence that must not have");
      }
    }
  }
  // A non-canonical rs1 makes SFENCE.VMA have no effect.
  {
    (*coverage)["fence-noncanonical"]++;
    bench.Fence(false, 0, false, 0);
    const TlbObs in = bench.Request(Va(0x10), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                    1, false, false);
    if (in.fault) Fail("directed/fence-noncanonical", "setup load faulted");
    bench.Fence(true, kNonCanonical, false, 0);
    const uint64_t r0 = bench.pte_reads();
    const TlbObs o = bench.Request(Va(0x10), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                   1, false, false);
    (*comparisons)++;
    if (o.fault || bench.pte_reads() != r0) {
      reporter->Mismatch("directed/fence-noncanonical", "no effect", "an invalidation");
      Fail("directed/fence-noncanonical", "a non-canonical rs1 invalidated something");
    }
  }

  // ---- 4. two address spaces with the same VA, and ASID 0 -------------------
  {
    (*coverage)["asid-pair"]++;
    (*coverage)["asid-zero"]++;
    bench.Fence(false, 0, false, 0);
    // Same virtual address, two roots: under root 1 it is pageA, under root 2 it
    // is pageZ. The two translations coexist because the tag carries the ASID.
    const TlbObs a1 = bench.Request(Va(0x10), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                    1, false, false);
    const TlbObs a2 = bench.Request(Va(0x10), kKindLoad, kPrivS, kSatpSv39, Root2Ppn(),
                                    2, false, false);
    (*comparisons)++;
    if (a1.pa != kPageA || a2.pa != kPageZ || a1.pa == a2.pa) {
      reporter->Mismatch("directed/asid-pair", U64(kPageA) + " vs " + U64(kPageZ),
                         U64(a1.pa) + " vs " + U64(a2.pa));
      Fail("directed/asid-pair", "the two address spaces did not translate differently");
    }
    // Both coexist: each is a hit with no PTE read.
    const uint64_t r0 = bench.pte_reads();
    const TlbObs b1 = bench.Request(Va(0x10), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                    1, false, false);
    const TlbObs b2 = bench.Request(Va(0x10), kKindLoad, kPrivS, kSatpSv39, Root2Ppn(),
                                    2, false, false);
    (*comparisons)++;
    if (bench.pte_reads() != r0 || b1.pa != kPageA || b2.pa != kPageZ) {
      reporter->Mismatch("directed/asid-pair", "both entries cached",
                         Dec(bench.pte_reads() - r0) + " PTE reads");
      Fail("directed/asid-pair", "the two same-VA entries did not coexist");
    }
    // ASID 0 is a normal address space.
    const TlbObs z = bench.Request(Va(0x1C), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                   0, false, false);
    const uint64_t r1 = bench.pte_reads();
    const TlbObs z2 = bench.Request(Va(0x1C), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                    0, false, false);
    (*comparisons)++;
    if (z.pa != kPageC || z2.pa != kPageC || bench.pte_reads() != r1) {
      Fail("directed/asid-zero", "ASID 0 did not behave as an ordinary address space");
    }
  }

  // ---- 5. a satp write invalidates ------------------------------------------
  {
    (*coverage)["satp-write"]++;
    bench.Fence(false, 0, false, 0);
    // Under root1 the page maps to pageA; under root2, to pageZ.
    const TlbObs a = bench.Request(Va(0x10), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                    3, false, false);
    if (a.fault || a.pa != kPageA) Fail("directed/satp-write", "setup load did not map pageA");
    // The same ASID, a new root: without a flush the cached pageA would stand.
    const uint64_t r0 = bench.pte_reads();
    bench.SatpWrite();
    if (bench.satp_flush_ctr() == 0) Fail("directed/satp-write", "the satp write did not flush");
    const TlbObs b = bench.Request(Va(0x10), kKindLoad, kPrivS, kSatpSv39, Root2Ppn(),
                                    3, false, false);
    (*comparisons)++;
    if (b.fault || b.pa != kPageZ || (bench.pte_reads() - r0) < 3) {
      reporter->Mismatch("directed/satp-write", U64(kPageZ), U64(b.pa));
      Fail("directed/satp-write", "an entry survived the satp write");
    }
  }

  // ---- 6. a cancelled walk installs nothing ---------------------------------
  {
    (*coverage)["cancel"]++;
    bench.Fence(false, 0, false, 0);
    bench.Drive(Va(0x1C), kKindLoad, kPrivS, kSatpSv39, RootPpn(), 5, false, false);
    bench.Cycle();
    bench.Cycle(true);              // cancel with the walk in flight
    bench.Settle(8);
    if (bench.cancel_ctr() == 0) Fail("directed/cancel", "the cache did not count the cancel");
    const uint64_t r1 = bench.pte_reads();
    const TlbObs o = bench.Request(Va(0x1C), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                   5, false, false);
    (*comparisons)++;
    if (o.fault || o.pa != kPageC || (bench.pte_reads() - r1) < 3) {
      reporter->Mismatch("directed/cancel", "a fresh walk on the next access",
                         Dec(bench.pte_reads() - r1) + " PTE reads");
      Fail("directed/cancel", "a cancelled walk installed an entry");
    }
  }

  // ---- 7. a walk that spans a fence installs nothing ------------------------
  {
    (*coverage)["late-walk"]++;
    bench.Fence(false, 0, false, 0);
    const uint32_t stale0 = bench.stale_ctr();
    bench.Drive(Va(0x16), kKindLoad, kPrivS, kSatpSv39, RootPpn(), 5, false, false);
    bench.Cycle();                  // the level-2 PTE read is issued
    bench.Fence(false, 0, false, 0);   // the fence lands mid-walk
    bench.Settle(12);               // let the walk finish and its result be taken
    if (bench.stale_ctr() == stale0) {
      Fail("directed/late-walk", "the cache did not report a walk dropped by a fence");
    }
    const uint64_t r1 = bench.pte_reads();
    const TlbObs o = bench.Request(Va(0x16), kKindLoad, kPrivS, kSatpSv39, RootPpn(),
                                   5, false, false);
    (*comparisons)++;
    if (o.fault || o.pa != kPageD || (bench.pte_reads() - r1) < 3) {
      reporter->Mismatch("directed/late-walk", "a fresh walk on the next access",
                         Dec(bench.pte_reads() - r1) + " PTE reads");
      Fail("directed/late-walk", "a walk that spanned a fence installed a stale entry");
    }
  }

  // ---- 8. the walker's A/D update is what the entry carries -----------------
  {
    (*coverage)["ad-dirty"]++;
    bench.Fence(false, 0, false, 0);
    // kL0[0x1A] has A=1, D=0. A load installs with D clear; a store must not be
    // served from that entry -- it must walk so the walker sets D.
    const uint64_t va = Va(0x1A);
    const TlbObs l = bench.Request(va, kKindLoad, kPrivS, kSatpSv39, RootPpn(), 0, false, false);
    if (l.fault || l.pa != kPageA) Fail("directed/ad-dirty", "setup load failed");
    uint64_t pte = 0;
    MemRead8(mem, kL0 + 8ull * 0x1A, &pte);
    if ((pte & 0x40) == 0 || (pte & 0x80) != 0) {
      Fail("directed/ad-dirty", "setup PTE is not A=1,D=0");
    }
    // A store must miss (D clear), walk, and set D.
    const uint64_t r0 = bench.pte_reads();
    const TlbObs s = bench.Request(va, kKindStore, kPrivS, kSatpSv39, RootPpn(), 0, false, false);
    (*comparisons)++;
    if (s.fault || s.pa != kPageA || (bench.pte_reads() - r0) < 3) {
      reporter->Mismatch("directed/ad-dirty", "a store walk (D clear)",
                         Dec(bench.pte_reads() - r0) + " PTE reads");
      Fail("directed/ad-dirty", "a store was served from an entry whose D was clear");
    }
    MemRead8(mem, kL0 + 8ull * 0x1A, &pte);
    if ((pte & 0x80) == 0) {
      reporter->Mismatch("directed/ad-dirty", "the PTE's D set by the store", "D clear");
      Fail("directed/ad-dirty", "the store did not mark the page dirty");
    }
    // Now the entry is dirty and a second store hits.
    const uint64_t r1 = bench.pte_reads();
    const TlbObs s2 = bench.Request(va, kKindStore, kPrivS, kSatpSv39, RootPpn(), 0, false, false);
    (*comparisons)++;
    if (s2.fault || bench.pte_reads() != r1) {
      reporter->Mismatch("directed/ad-dirty", "a hit on the now-dirty entry",
                         Dec(bench.pte_reads() - r1) + " PTE reads");
      Fail("directed/ad-dirty", "a store re-walked an entry the walker had made dirty");
    }
  }

  // ---- coverage, asserted not printed ---------------------------------------
  {
    static const char* kRequired[] = {
        "miss", "hit", "stale-without-fence", "stale-after-fence", "fence-all",
        "fence-by-asid-1", "fence-by-asid-2", "fence-by-addr", "fence-by-addr-asid",
        "fence-noncanonical", "asid-pair", "asid-zero", "satp-write", "cancel",
        "late-walk", "ad-dirty",
    };
    for (const char* key : kRequired) {
      if ((*coverage)[key] == 0) Fail("coverage", std::string("no scenario covered ") + key);
    }
  }
  if (bench.install_ctr() == 0) Fail("directed", "no entry was ever installed");
  if (bench.walk_ctr() == 0) Fail("directed", "no walk was ever performed");
}

// ============================================================================
// Part 2: the SFENCE.VMA instruction, through the core
// ============================================================================
uint32_t EncI(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return ((static_cast<uint32_t>(imm) & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) |
         (rd << 7) | op;
}
uint32_t EncS(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  const uint32_t u = static_cast<uint32_t>(imm);
  return (((u >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
         ((u & 0x1F) << 7) | op;
}
uint32_t Lui(uint32_t rd, uint32_t imm20) { return (imm20 << 12) | (rd << 7) | 0x37; }
uint32_t Addi(uint32_t rd, uint32_t rs1, int32_t imm) { return EncI(imm, rs1, 0, rd, 0x13); }
uint32_t Add(uint32_t rd, uint32_t rs1, uint32_t rs2) {
  return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33;
}
uint32_t Slli(uint32_t rd, uint32_t rs1, uint32_t sh) { return EncI(sh, rs1, 1, rd, 0x13); }
uint32_t Srli(uint32_t rd, uint32_t rs1, uint32_t sh) { return EncI(sh, rs1, 5, rd, 0x13); }
uint32_t Or(uint32_t rd, uint32_t rs1, uint32_t rs2) { return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33 | (6 << 12); }
uint32_t Ld(uint32_t rd, uint32_t rs1, int32_t imm) { return EncI(imm, rs1, 3, rd, 0x03); }
uint32_t Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { return EncS(imm, rs2, rs1, 3, 0x23); }
uint32_t Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { return EncI(static_cast<int32_t>(csr), rs1, 1, rd, 0x73); }
uint32_t Mret() { return EncI(0x302, 0, 0, 0, 0x73); }
uint32_t Jal(uint32_t rd, int32_t off) {
  const uint32_t u = static_cast<uint32_t>(off);
  return (((u >> 20) & 1) << 31) | (((u >> 1) & 0x3FF) << 21) | (((u >> 11) & 1) << 20) |
         (((u >> 12) & 0xFF) << 12) | (rd << 7) | 0x6F;
}
// SFENCE.VMA rs1, rs2: funct7 0001001, funct3 000, rd 00000.
uint32_t SfenceVma(uint32_t rs1, uint32_t rs2) {
  return EncI(0x120 | static_cast<int32_t>(rs2), rs1, 0, 0, 0x73);
}

constexpr uint32_t kCsrMtvec = 0x305, kCsrMepc = 0x341, kCsrMstatus = 0x300;
constexpr uint32_t kCsrPmpcfg0 = 0x3A0, kCsrPmpaddr0 = 0x3B0, kCsrSatp = 0x180;

constexpr uint64_t kProgM = kRamBase;      // the reset vector
constexpr uint64_t kProgS = kRamBase + 0x200;
constexpr uint64_t kRes0  = kRamBase + 0x400;
constexpr uint64_t kRes1  = kRamBase + 0x408;
constexpr uint64_t kSeed0 = kRamBase + 0x410;
constexpr uint64_t kSeed1 = kRamBase + 0x418;
constexpr uint64_t kData0 = 0x1111AAAAull;
constexpr uint64_t kData1 = 0x2222BBBBull;

constexpr uint64_t kVx = (1ull << 30) | (1ull << 21) | (0x20ull << 12);

struct Emitter {
  std::map<uint64_t, uint32_t>* img;
  uint64_t pc;
  void W(uint32_t word) { (*img)[pc] = word; pc += 4; }
  void Li32(uint32_t rd, uint32_t v) {
    const uint32_t hi = (v + 0x800u) >> 12;
    const int32_t lo = static_cast<int32_t>(v & 0xFFFu) - ((v & 0x800u) ? 0x1000 : 0);
    W(Lui(rd, hi & 0xFFFFFu));
    W(Addi(rd, rd, lo));
    W(Slli(rd, rd, 32));
    W(Srli(rd, rd, 32));
  }
  void Li(uint32_t rd, uint64_t value) {
    if (value <= 0xFFFFFFFFull) { Li32(rd, static_cast<uint32_t>(value)); return; }
    Li32(rd, static_cast<uint32_t>(value >> 32));
    W(Slli(rd, rd, 32));
    Li32(31u, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    W(Or(rd, rd, 31u));
  }
};

void BuildProgramTables(MemoryModel* mem) {
  auto put = [&](uint64_t table, unsigned index, uint64_t value) {
    if (!MemWrite8(mem, table + 8ull * index, value)) {
      Fail("integrated", "cannot write the PTE at " + U64(table + 8ull * index));
    }
  };
  for (unsigned i = 0; i < 512; i++) {
    put(kRoot, i, 0);
    put(kL1, i, 0);
    put(kL0, i, 0);
  }
  // A 2 MiB identity leaf covering RAM's base: the S-mode code, its data and the
  // page tables themselves are all reachable as VA == PA, so the stub can patch
  // a PTE with an ordinary store.
  put(kRoot, 2, Pte(Pp(kRamBase), true, true, true, true, false, false, true, true));
  // The test tree: kVx maps to pageGood until the stub patches the PTE.
  put(kRoot, 1, PteNonleaf(kL1));
  put(kL1, 1, PteNonleaf(kL0));
  put(kL0, 0x20, Pte(Pp(kPageA), true, true, true, true, false, false, true, true));
}

void BuildProgram(std::map<uint64_t, uint32_t>* img) {
  {
    Emitter e{img, kProgM};
    e.Li(5, kProgM);                 e.W(Csrrw(0, kCsrMtvec, 5));
    e.Li(5, 0x0F);                   e.W(Csrrw(0, kCsrPmpcfg0, 5));
    e.Li(5, (kRamBase + MOSAIC_RAM_SIZE) >> 2); e.W(Csrrw(0, kCsrPmpaddr0, 5));
    // satp = Sv39, ASID 1, root kRoot.
    e.Li(5, (8ull << 60) | (1ull << 44) | Pp(kRoot)); e.W(Csrrw(0, kCsrSatp, 5));
    e.Li(5, kProgS);                 e.W(Csrrw(0, kCsrMepc, 5));
    e.Li(5, 0x800);                  e.W(Csrrw(0, kCsrMstatus, 5));
    e.W(Mret());
    e.W(Jal(0, 0));
  }
  {
    Emitter e{img, kProgS};
    e.Li(6, kVx);
    e.W(Ld(7, 6, 0));                    // load 1: walks
    e.W(Ld(8, 6, 0));                    // load 2: a hit
    e.Li(9, kRes0); e.W(Sd(7, 9, 0));    // remember load 1's value
    // Patch the kVx PTE to point at kPageB with A=1,D=1.
    e.Li(10, Pte(Pp(kPageB), true, true, true, true, false, false, true, true));
    e.Li(11, kL0 + 8ull * 0x20);
    e.W(Sd(10, 11, 0));
    e.W(SfenceVma(0, 0));                // SFENCE.VMA x0, x0 (all)
    e.W(Ld(12, 6, 0));                   // load 3: must see the new mapping
    e.Li(13, kRes1); e.W(Sd(12, 13, 0));
    e.W(Jal(0, 0));
  }
}

// ------------------------------------------------------------ the core harness
struct CoreObs {
  uint64_t trap_count = 0;
  uint64_t trap_cause[4] = {0, 0, 0, 0};
};

class CoreHarness {
 public:
  CoreHarness(Vmosaic_core_tb* dut, MemoryModel* mem, std::map<uint64_t, uint32_t>* img)
      : dut_(dut), mem_(mem), img_(img) {}

  uint64_t cycles() const { return cycles_; }
  const std::vector<std::array<uint64_t, 3>>& data_txns() const { return data_txns_; }
  const CoreObs& obs() const { return obs_; }

  void Reset(int n) { for (int i = 0; i < n; i++) Cycle(true); }

  void Cycle(bool rst) {
    if (cycles_ >= 200000) Fail("integrated", "the program did not finish");
    dut_->rst = rst ? 1 : 0;
    dut_->irq_soft_i = 0;
    dut_->irq_timer_i = 0;
    dut_->irq_ext_i = 0;
    dut_->mtime_i = mtime_++;
    dut_->imem_req_ready_i = 1;
    dut_->imem_rsp_valid_i = !imem_ready_.empty() ? 1 : 0;
    if (!imem_ready_.empty()) {
      dut_->imem_rsp_rdata_i = imem_ready_.front().word;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = imem_ready_.front().id;
      dut_->imem_rsp_epoch_i = imem_ready_.front().epoch;
      dut_->imem_rsp_len_i = 4;
    } else {
      dut_->imem_rsp_rdata_i = 0; dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0; dut_->imem_rsp_epoch_i = 0; dut_->imem_rsp_len_i = 0;
    }
    dut_->dmem_req_ready_i = 1;
    dut_->dmem_rsp_valid_i = !dmem_ready_.empty() ? 1 : 0;
    if (!dmem_ready_.empty()) {
      dut_->dmem_rsp_rdata_i = dmem_ready_.front().rdata;
      dut_->dmem_rsp_fault_i = dmem_ready_.front().fault ? 1 : 0;
    } else {
      dut_->dmem_rsp_rdata_i = 0; dut_->dmem_rsp_fault_i = 0;
    }
    dut_->ext_write_valid_i = 0; dut_->ext_write_addr_i = 0; dut_->ext_write_bytes_i = 0;
    dut_->arb_req_valid0_i = 0; dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0; dut_->arb_head_retire_i = 0;
    // Park the standalone PTW and TLB.
    dut_->ptw_xl_valid_i = 0; dut_->ptw_xl_va_i = 0; dut_->ptw_xl_kind_i = 0;
    dut_->ptw_xl_priv_i = 0; dut_->ptw_xl_mode_i = 0; dut_->ptw_xl_ppn_i = 0;
    dut_->ptw_xl_sum_i = 0; dut_->ptw_xl_mxr_i = 0; dut_->ptw_xl_cancel_i = 0;
    dut_->ptw_xl_rsp_ready_i = 1; dut_->ptw_mem_req_ready_i = 1;
    dut_->ptw_mem_rsp_valid_i = 0; dut_->ptw_mem_rsp_rdata_i = 0; dut_->ptw_mem_rsp_fault_i = 0;
    dut_->tlb_xl_valid_i = 0; dut_->tlb_xl_va_i = 0; dut_->tlb_xl_kind_i = 0;
    dut_->tlb_xl_priv_i = 0; dut_->tlb_xl_mode_i = 0; dut_->tlb_xl_ppn_i = 0;
    dut_->tlb_xl_asid_i = 0; dut_->tlb_xl_sum_i = 0; dut_->tlb_xl_mxr_i = 0;
    dut_->tlb_xl_cancel_i = 0; dut_->tlb_xl_rsp_ready_i = 1;
    dut_->tlb_sfence_valid_i = 0; dut_->tlb_sfence_va_i = 0;
    dut_->tlb_sfence_has_va_i = 0; dut_->tlb_sfence_asid_i = 0;
    dut_->tlb_sfence_has_asid_i = 0; dut_->tlb_satp_write_i = 0;
    dut_->tlb_mem_req_ready_i = 1; dut_->tlb_mem_rsp_valid_i = 0;
    dut_->tlb_mem_rsp_rdata_i = 0; dut_->tlb_mem_rsp_fault_i = 0;

    dut_->clk = 0;
    dut_->eval();

    if (dut_->o_trap_valid_o != 0 && !rst) {
      CoreObs& o = obs_;
      if (o.trap_count < 4) {
        o.trap_cause[o.trap_count] = dut_->o_trap_cause_o;
      }
      o.trap_count++;
    }
    const bool dmem_accept = (dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0);
    const bool dmem_pop = (dut_->dmem_rsp_valid_i != 0) && (dut_->dmem_rsp_ready_o != 0);
    const bool imem_accept = (dut_->imem_req_valid_o != 0) && (dut_->imem_req_ready_i != 0);
    const bool imem_pop = (dut_->imem_rsp_valid_i != 0) && (dut_->imem_rsp_ready_o != 0);
    const uint64_t dmem_addr = dut_->dmem_req_addr_o;
    const uint64_t dmem_wdata = dut_->dmem_req_wdata_o;
    const bool dmem_we = dut_->dmem_req_we_o != 0;
    const uint32_t dmem_wstrb = static_cast<uint32_t>(dut_->dmem_req_wstrb_o);
    const uint64_t imem_addr = dut_->imem_req_addr_o;
    const uint64_t imem_id = dut_->imem_req_id_o;
    const uint64_t imem_epoch = dut_->imem_req_epoch_o;

    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (imem_pop) imem_ready_.pop_front();
    if (dmem_pop) dmem_ready_.pop_front();
    if (imem_accept) {
      auto it = img_->find(imem_addr);
      ImemRsp r;
      r.word = (it == img_->end()) ? 0x0000006Fu : it->second;
      r.id = imem_id;
      r.epoch = imem_epoch;
      imem_ready_.push_back(r);
    }
    if (dmem_accept) {
      data_txns_.push_back({dmem_addr, dmem_we ? 1ull : 0ull, dmem_wdata});
      Rsp r;
      if (dmem_we) {
        r.fault = !MemWrite8(mem_, dmem_addr, dmem_wdata);
        r.rdata = 0;
      } else {
        r.fault = !MemRead8(mem_, dmem_addr, &r.rdata);
        if (r.fault) r.rdata = 0;
      }
      dmem_ready_.push_back(r);
    }
  }

 private:
  struct Rsp { uint64_t rdata = 0; bool fault = false; };
  struct ImemRsp { uint32_t word = 0; uint64_t id = 0; uint64_t epoch = 0; };
  Vmosaic_core_tb* dut_;
  MemoryModel* mem_;
  std::map<uint64_t, uint32_t>* img_;
  uint64_t cycles_ = 0;
  uint64_t mtime_ = 0;
  std::deque<ImemRsp> imem_ready_;
  std::deque<Rsp> dmem_ready_;
  std::vector<std::array<uint64_t, 3>> data_txns_;
  CoreObs obs_;
};

bool IsPteAddr(uint64_t addr) {
  return (addr >= kRoot && addr < kRoot + 0x1000) ||
         (addr >= kL1 && addr < kL1 + 0x1000) ||
         (addr >= kL0 && addr < kL0 + 0x1000);
}

void RunIntegrated(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, MemoryModel* mem,
                   std::map<std::string, int>* coverage, uint64_t* comparisons) {
  (*coverage)["integrated"]++;
  BuildProgramTables(mem);
  if (!MemWrite8(mem, kPageA, kData0) || !MemWrite8(mem, kPageB, kData1)) {
    Fail("integrated", "cannot seed the mapped pages");
  }
  std::map<uint64_t, uint32_t> img;
  BuildProgram(&img);
  CoreHarness h(dut, mem, &img);
  h.Reset(4);
  for (uint64_t i = 0; i < 20000; i++) {
    h.Cycle(false);
    uint64_t res = 0;
    if (dut->o_core_tlb_sfence_ctr_o != 0 && MemRead8(mem, kRes1, &res) && res != 0) break;
  }
  for (int i = 0; i < 60; i++) h.Cycle(false);

  if (Debug()) {
    std::printf("  [integrated] traps=%llu cause0=%llu hit=%u miss=%u inst=%u fence=%u satp=%u gen=%u walk=%u\n",
                (unsigned long long)h.obs().trap_count,
                (unsigned long long)h.obs().trap_cause[0],
                dut->o_core_tlb_hit_ctr_o, dut->o_core_tlb_miss_ctr_o,
                dut->o_core_tlb_install_ctr_o, dut->o_core_tlb_sfence_ctr_o,
                dut->o_core_tlb_satp_flush_ctr_o, dut->o_core_tlb_gen_o,
                dut->o_core_tlb_walk_ctr_o);
    for (const auto& t : h.data_txns()) {
      std::printf("    [dmem] %s %s\n", t[1] ? "w" : "r", U64(t[0]).c_str());
    }
  }

  (*comparisons)++;
  if (h.obs().trap_count != 0) {
    reporter->Mismatch("integrated/traps", "no trap",
                       Dec(h.obs().trap_count) + " (first cause " +
                           Dec(h.obs().trap_cause[0]) + ")");
    Fail("integrated", "the SFENCE.VMA program trapped");
  }
  uint64_t v0 = 0, v1 = 0;
  MemRead8(mem, kRes0, &v0);
  MemRead8(mem, kRes1, &v1);
  if (v0 != kData0) {
    reporter->Mismatch("integrated/first-load", U64(kData0), U64(v0));
    Fail("integrated", "the first translated load read the wrong page");
  }
  if (v1 != kData1) {
    reporter->Mismatch("integrated/fence", U64(kData1), U64(v1));
    Fail("integrated", "the load after SFENCE.VMA did not observe the new mapping");
  }
  // The cache's own view: the fence ran, entries were installed, and at least
  // one access was served without a walk.
  if (dut->o_core_tlb_sfence_ctr_o == 0) Fail("integrated", "SFENCE.VMA never executed");
  if (dut->o_core_tlb_install_ctr_o < 2) {
    Fail("integrated", "fewer than two translations were installed");
  }
  if (dut->o_core_tlb_hit_ctr_o == 0) Fail("integrated", "no access was served from the cache");
  if (dut->o_core_tlb_satp_flush_ctr_o == 0) {
    Fail("integrated", "the satp write did not invalidate the cache");
  }
  // The dmem port: the walks' PTE reads are visible, and no data access ever
  // lands outside RAM.
  uint64_t pte_reads = 0;
  for (const auto& t : h.data_txns()) {
    const uint64_t addr = t[0];
    const bool we = t[1] != 0;
    if (!we && IsPteAddr(addr)) pte_reads++;
    if (addr < kRamBase || addr >= kRamBase + MOSAIC_RAM_SIZE) {
      reporter->Mismatch("integrated/in-ram", "every data access inside RAM", U64(addr));
      Fail("integrated", "the data port carried an address outside RAM");
    }
  }
  if (pte_reads < 6) {
    reporter->Mismatch("integrated/walks", "at least two walks' PTE reads",
                       Dec(pte_reads) + " PTE reads");
    Fail("integrated", "too few PTE reads for the walks the program requires");
  }
  if (Debug()) {
    std::printf("  [integrated] v0=%s v1=%s pte_reads=%llu hit=%u miss=%u inst=%u fence=%u satp=%u gen=%u\n",
                U64(v0).c_str(), U64(v1).c_str(), (unsigned long long)pte_reads,
                dut->o_core_tlb_hit_ctr_o, dut->o_core_tlb_miss_ctr_o,
                dut->o_core_tlb_install_ctr_o, dut->o_core_tlb_sfence_ctr_o,
                dut->o_core_tlb_satp_flush_ctr_o, dut->o_core_tlb_gen_o);
  }
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
  mosaic::Reporter reporter(options,
                            std::string("Verilator ") + Verilated::productVersion());
  Vmosaic_core_tb dut;

  std::string detail;
  bool passed = true;
  std::map<std::string, int> coverage;
  uint64_t comparisons = 0;
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    if (dut.o_geom_has_s_o == 0) {
      detail = "profile has no supervisor mode";
      reporter.Check(true, "the profile has no S-mode: nothing to check");
      dut.final();
      return reporter.Finish("PASS", detail);
    }

    MemoryModel dir_mem;
    RunDirected(&dut, &reporter, &dir_mem, &coverage, &comparisons);

    MemoryModel int_mem;
    RunIntegrated(&dut, &reporter, &int_mem, &coverage, &comparisons);

    std::printf("  coverage:");
    for (const auto& kv : coverage) std::printf(" %s=%d", kv.first.c_str(), kv.second);
    std::printf("\n  comparisons=%llu checks=%d\n",
                static_cast<unsigned long long>(comparisons), reporter.checks());
    detail = "comparisons=" + Dec(comparisons) + " checks=" + Dec(reporter.checks()) +
             " seed=" + Dec(options.seed);
  } catch (const std::exception& f) {
    reporter.Mismatch("run", "every hit, fence form and invalidation matches the rules",
                      "contract violated");
    passed = false;
    detail = "contract violated: " + std::string(f.what());
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
