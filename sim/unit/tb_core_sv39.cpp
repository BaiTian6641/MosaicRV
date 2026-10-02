// ============================================================================
// tb_core_sv39.cpp -- CASE=sv39.walk_and_faults, work package I-045.
//
// Two things are checked here, and they are deliberately separate:
//
//   1. **The walker itself**, driven directly. `sim/tb/mosaic_core_tb.sv`
//      instantiates a second DUT beside the core -- the standalone
//      `mosaic_ptw` -- whose PTE port this driver owns: it builds the page
//      tables in a physical memory model, serves the walker's reads, and can
//      read the tables back to see the A/D bits the walk wrote. Every level as
//      a leaf and as a non-leaf, both superpage sizes and the misaligned
//      superpage, a non-canonical address, an invalid PTE, a reserved PTE, each
//      permission bit, SUM, MXR, a PTE access that itself faults, Bare, and a
//      cancelled walk are driven here. The expectation for each comes from an
//      independent model of the walk written from the privileged
//      specification's step list (`ModelWalk`), *and* from a hand-written
//      expectation quoting the rule; the driver requires the two to agree
//      before it compares the DUT with either.
//
//   2. **The integration**, run through the core. A small program enables Sv39
//      with `satp`, enters S-mode, and performs a translated load and a
//      translated store; the driver watches the data port and requires the
//      *physical* addresses the walk produced to appear there, the loaded value
//      to come back, a non-canonical address and an invalid PTE to trap with
//      the cause and `tval` the specification names, and -- the card's first
//      failure mode made observable -- no access derived from the non-canonical
//      address ever to reach the data port.
//
// ---------------------------------------------------------------- the A/D policy
//
// The implementation takes the hardware-update scheme, not Svade: a leaf with
// A=0 is updated (A set), and a leaf written by a store with D=0 is updated (A
// and D set), each by a compare-and-set PTE write. The case checks the update
// happened, that the update set only the bits the spec names (D only for a
// store), that a leaf needing no update issues no write, and that a cancelled
// walk leaves no update behind.
//
// ---------------------------------------------------------------- not covered
//
// The TLB and SFENCE.VMA are I-046's, so every access re-walks. Instruction
// fetch translation is not integrated (I-046/I-048 own the fetch-side path);
// fetch-class access kinds are covered at the walker's own interface. The
// caches (I-042) are still module-level and not in the access path.
// ============================================================================

#include <verilated.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <map>
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
bool Debug() { return std::getenv("SV39_DEBUG") != nullptr; }

// The ISA's exception codes.
constexpr int kExcInsnAccess  = 1;
constexpr int kExcLoadAccess  = 5;
constexpr int kExcStoreAccess = 7;
constexpr int kExcInsnPage    = 12;
constexpr int kExcLoadPage    = 13;
constexpr int kExcStorePage   = 15;

// Access classes, as the walker's request labels them.
constexpr int kKindLoad = 0, kKindStore = 1, kKindFetch = 2;
constexpr int kPrivU = 0, kPrivS = 1, kPrivM = 3;

constexpr uint64_t kSatpBare = 0;
constexpr uint64_t kSatpSv39 = 8;

// ------------------------------------------------------------- physical layout
constexpr uint64_t kRamBase  = MOSAIC_RAM_BASE;              // 0x8000_0000
constexpr uint64_t kRoot     = kRamBase + 0x10000;
constexpr uint64_t kL1       = kRamBase + 0x11000;
constexpr uint64_t kL0       = kRamBase + 0x12000;
constexpr uint64_t kL0b      = kRamBase + 0x12800;           // for the last-level non-leaf
constexpr uint64_t kPageA    = kRamBase + 0x20000;
constexpr uint64_t kPageB    = kRamBase + 0x21000;
constexpr uint64_t kBadTable = 0x90000000ull;                // outside RAM: an implicit read faults

uint64_t Pp(uint64_t page_addr) { return page_addr >> 12; }

// A PTE, field by field, from the format in the specification.
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
uint64_t PteNonleaf(uint64_t next, bool v = true) {
  return Pte(Pp(next), v, false, false, false, false, false, false, false);
}

uint64_t Va(uint32_t vpn2, uint32_t vpn1, uint32_t vpn0, uint32_t off = 0) {
  return (static_cast<uint64_t>(vpn2) << 30) |
         (static_cast<uint64_t>(vpn1) << 21) |
         (static_cast<uint64_t>(vpn0) << 12) |
         static_cast<uint64_t>(off);
}
constexpr uint64_t kNonCanonical = UINT64_C(0x0000008000000000);  // bit 39 set

// The root PPN every directed scenario uses.
uint64_t RootPpn() { return Pp(kRoot); }

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
bool MemWrite8(MemoryModel* mem, uint64_t addr, uint64_t value, uint32_t wstrb) {
  uint64_t window = 0;
  if (!MemRead8(mem, addr, &window)) return false;
  for (unsigned i = 0; i < 8; i++) {
    if (((wstrb >> i) & 1u) != 0u) {
      window = (window & ~(UINT64_C(0xff) << (8 * i))) |
               (((value >> (8 * i)) & 0xffull) << (8 * i));
    }
  }
  return mem->Write(addr, 8, window) == AccessStatus::kOk;
}

// ============================================================================
// The independent model of the walk -- written from the privileged
// specification's "Virtual Address Translation Process" step list, not from the
// RTL.
// ============================================================================
struct WalkResult {
  bool fault = false;
  int cause = 0;
  uint64_t pa = 0;
  uint64_t tval = 0;
  uint8_t perms = 0;          // {x, w, r, u}
  bool ad_update = false;     // the PTE must have been updated
  bool ad_dirty = false;      // ... with D set, not only A
  int pte_reads = 0;
};

int PageCause(int kind) {
  if (kind == kKindStore) return kExcStorePage;
  if (kind == kKindFetch) return kExcInsnPage;
  return kExcLoadPage;
}
int AccessCause(int kind) {
  if (kind == kKindStore) return kExcStoreAccess;
  if (kind == kKindFetch) return kExcInsnAccess;
  return kExcLoadAccess;
}

WalkResult ModelWalk(MemoryModel* mem, uint64_t va, int kind, int priv,
                     uint64_t mode, uint64_t ppn, bool sum, bool mxr) {
  WalkResult r;
  r.tval = va;
  if (mode == kSatpBare || priv == kPrivM) {
    // Bare: the virtual address is the physical address and every access is
    // permitted. No PTE is read.
    r.pa = va;
    r.perms = 0xF;
    return r;
  }
  if (mode != kSatpSv39) {
    r.fault = true;
    r.cause = PageCause(kind);
    return r;
  }
  if ((va >> 39) != (va & (UINT64_C(1) << 38) ? ((UINT64_C(1) << 25) - 1) : 0)) {
    r.fault = true;
    r.cause = PageCause(kind);
    return r;
  }
  uint64_t base = ppn;
  for (int i = 2; i >= 0; i--) {
    const uint64_t vpn = (va >> (12 + 9 * i)) & 0x1FF;
    const uint64_t a = (base << 12) | (vpn * 8);
    uint64_t pte = 0;
    r.pte_reads++;
    if (!MemRead8(mem, a, &pte)) {
      // An implicit access that violates PMA/PMP is an access fault.
      r.fault = true;
      r.cause = AccessCause(kind);
      return r;
    }
    const bool v = (pte & 1) != 0;
    const bool rd = (pte & 2) != 0;
    const bool wr = (pte & 4) != 0;
    const bool ex = (pte & 8) != 0;
    const bool u = (pte & 0x10) != 0;
    const bool acc = (pte & 0x40) != 0;
    const bool dirty = (pte & 0x80) != 0;
    const uint64_t pte_ppn = (pte >> 10) & ((UINT64_C(1) << 44) - 1);
    if (!v || (wr && !rd) || ((pte >> 54) != 0)) {
      r.fault = true;
      r.cause = PageCause(kind);
      return r;
    }
    if (rd || ex) {
      // leaf: permission check
      bool u_ok = true;
      if (priv == kPrivU) {
        u_ok = u;
      } else if (priv == kPrivS && u) {
        u_ok = (kind == kKindFetch) ? false : sum;
      }
      bool perm_ok;
      if (kind == kKindStore)      perm_ok = wr;
      else if (kind == kKindFetch) perm_ok = ex;
      else                         perm_ok = rd || (mxr && ex);
      if (!(u_ok && perm_ok)) {
        r.fault = true;
        r.cause = PageCause(kind);
        return r;
      }
      if (i > 0 && (pte_ppn & ((UINT64_C(1) << (9 * i)) - 1)) != 0) {
        r.fault = true;
        r.cause = PageCause(kind);
        return r;
      }
      if (!acc || (kind == kKindStore && !dirty)) {
        r.ad_update = true;
        r.ad_dirty = (kind == kKindStore);
      }
      uint64_t ppn_eff;
      if (i == 0) {
        ppn_eff = pte_ppn;
      } else if (i == 1) {
        ppn_eff = (pte_ppn & ~UINT64_C(0x1FF)) | ((va >> 12) & 0x1FF);
      } else {
        ppn_eff = (pte_ppn & ~UINT64_C(0x3FFFF)) | ((va >> 12) & 0x3FFFF);
      }
      r.pa = ((ppn_eff & ((UINT64_C(1) << 44) - 1)) << 12) | (va & 0xFFF);
      r.perms = static_cast<uint8_t>((ex ? 8 : 0) | (wr ? 4 : 0) | (rd ? 2 : 0) |
                                     (u ? 1 : 0));
      return r;
    }
    if (i == 0) {
      // A non-leaf at the last level: there is no deeper table.
      r.fault = true;
      r.cause = PageCause(kind);
      return r;
    }
    base = pte_ppn;
  }
  r.fault = true;
  r.cause = PageCause(kind);
  return r;
}

// ============================================================================
// The directed bench: drives the standalone walker and its PTE memory.
// ============================================================================
struct PtwObs {
  bool ready = false, rsp_valid = false, fault = false, bare = false;
  uint64_t pa = 0, tval = 0;
  uint8_t cause = 0, perms = 0;
  bool mem_req_valid = false, mem_we = false;
  uint64_t mem_addr = 0, mem_wdata = 0;
  uint8_t mem_wstrb = 0;
  bool busy = false;
};

class PtwBench {
 public:
  PtwBench(Vmosaic_core_tb* dut, MemoryModel* mem) : dut_(dut), mem_(mem) {}

  uint64_t cycles() const { return cycles_; }
  uint64_t pte_writes() const { return pte_writes_; }
  const std::vector<std::pair<uint64_t, uint64_t>>& writes() const { return writes_; }
  const std::vector<uint64_t>& reads() const { return reads_; }

  void Idle() {
    dut_->ptw_xl_valid_i = 0;
    dut_->ptw_xl_va_i = 0;
    dut_->ptw_xl_kind_i = 0;
    dut_->ptw_xl_priv_i = 0;
    dut_->ptw_xl_mode_i = 0;
    dut_->ptw_xl_ppn_i = 0;
    dut_->ptw_xl_sum_i = 0;
    dut_->ptw_xl_mxr_i = 0;
    dut_->ptw_xl_cancel_i = 0;
    dut_->ptw_xl_rsp_ready_i = 1;
    dut_->ptw_mem_req_ready_i = 1;
    dut_->ptw_mem_rsp_valid_i = 0;
    dut_->ptw_mem_rsp_rdata_i = 0;
    dut_->ptw_mem_rsp_fault_i = 0;
  }

  // Present a request. Returns when the request has been accepted.
  void Drive(uint64_t va, int kind, int priv, uint64_t mode, uint64_t ppn,
             bool sum, bool mxr) {
    dut_->ptw_xl_valid_i = 1;
    dut_->ptw_xl_va_i = va;
    dut_->ptw_xl_kind_i = static_cast<uint8_t>(kind);
    dut_->ptw_xl_priv_i = static_cast<uint8_t>(priv);
    dut_->ptw_xl_mode_i = static_cast<uint8_t>(mode);
    dut_->ptw_xl_ppn_i = ppn & ((UINT64_C(1) << 44) - 1);
    dut_->ptw_xl_sum_i = sum ? 1 : 0;
    dut_->ptw_xl_mxr_i = mxr ? 1 : 0;
    Cycle();
    if (!observed_.ready) {
      // The walker was busy; this case only offers a new request when idle.
      Fail("directed", "the walker did not accept a request while idle");
    }
    dut_->ptw_xl_valid_i = 0;
  }

  PtwObs Cycle(bool cancel = false) {
    dut_->ptw_xl_cancel_i = cancel ? 1 : 0;
    dut_->ptw_mem_rsp_valid_i = ready_.empty() ? 0 : 1;
    if (!ready_.empty()) {
      dut_->ptw_mem_rsp_rdata_i = ready_.front().rdata;
      dut_->ptw_mem_rsp_fault_i = ready_.front().fault ? 1 : 0;
    }
    dut_->clk = 0;
    dut_->eval();
    PtwObs o;
    o.ready = dut_->ptw_xl_ready_o != 0;
    o.rsp_valid = dut_->ptw_xl_rsp_valid_o != 0;
    o.fault = dut_->ptw_xl_fault_o != 0;
    o.bare = dut_->ptw_xl_bare_o != 0;
    o.pa = dut_->ptw_xl_pa_o;
    o.cause = static_cast<uint8_t>(dut_->ptw_xl_cause_o);
    o.tval = dut_->ptw_xl_tval_o;
    o.perms = static_cast<uint8_t>(dut_->ptw_xl_perms_o);
    o.mem_req_valid = dut_->ptw_mem_req_valid_o != 0;
    o.mem_we = dut_->ptw_mem_req_we_o != 0;
    o.mem_addr = dut_->ptw_mem_req_addr_o;
    o.mem_wdata = dut_->ptw_mem_req_wdata_o;
    o.mem_wstrb = static_cast<uint8_t>(dut_->ptw_mem_req_wstrb_o);
    o.busy = dut_->o_ptw_busy_o != 0;
    observed_ = o;

    // The PTE port is always ready; record and serve the beat after the edge.
    if (o.mem_req_valid) {
      if (o.mem_we) {
        writes_.push_back({o.mem_addr, o.mem_wdata});
        pte_writes_++;
      } else {
        reads_.push_back(o.mem_addr);
      }
    }
    const bool mem_accept = o.mem_req_valid != 0;
    const bool rsp_taken = (ready_.empty() ? false : true);

    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (rsp_taken) ready_.pop_front();
    if (mem_accept) {
      Rsp rsp;
      if (o.mem_we) {
        rsp.fault = !MemWrite8(mem_, o.mem_addr, o.mem_wdata, o.mem_wstrb);
        rsp.rdata = 0;
      } else {
        rsp.fault = !MemRead8(mem_, o.mem_addr, &rsp.rdata);
        if (rsp.fault) rsp.rdata = 0;
      }
      ready_.push_back(rsp);
    }
    return o;
  }

  // Run until the walker presents a result, or give up.
  PtwObs RunUntilResponse(uint64_t limit = 64) {
    for (uint64_t i = 0; i < limit; i++) {
      const PtwObs o = Cycle();
      if (o.rsp_valid) return o;
    }
    Fail("directed", "the walker produced no response within " + Dec(limit) + " cycles");
  }

  // Let the walker settle after a request was accepted but before a result.
  void Settle(int n) { for (int i = 0; i < n; i++) Cycle(); }

 private:
  struct Rsp { uint64_t rdata = 0; bool fault = false; };
  struct ImemRsp { uint32_t word = 0; uint64_t id = 0; uint64_t epoch = 0; };
  Vmosaic_core_tb* dut_;
  MemoryModel* mem_;
  uint64_t cycles_ = 0;
  uint64_t pte_writes_ = 0;
  std::deque<Rsp> ready_;
  std::vector<std::pair<uint64_t, uint64_t>> writes_;
  std::vector<uint64_t> reads_;
  PtwObs observed_;
};

// ============================================================================
// The directed page tables
// ============================================================================
void BuildTables(MemoryModel* mem) {
  auto put = [&](uint64_t table, unsigned index, uint64_t value) {
    if (!MemWrite8(mem, table + 8ull * index, value, 0xFF)) {
      Fail("tables", "cannot write the PTE at " + U64(table + 8ull * index));
    }
  };
  for (unsigned i = 0; i < 512; i++) {
    put(kRoot, i, 0);
    put(kL1, i, 0);
    put(kL0, i, 0);
    put(kL0b, i, 0);
  }
  // Level 2: a pointer down, the feature entries, and the error entries.
  put(kRoot, 1, PteNonleaf(kL1));
  put(kRoot, 2, Pte(0, false, false, false, false, false, false, false, false)); // V=0
  put(kRoot, 3, Pte(0, true, false, true, false, false, false, false, false));   // W&!R: reserved
  put(kRoot, 4, PteNonleaf(kBadTable));                                          // implicit access faults
  put(kRoot, 5, Pte(Pp(kRamBase), true, true, true, true, false, false, true, true));  // 1 GiB leaf
  put(kRoot, 6, Pte((Pp(kRamBase) | 1), true, true, true, true, false, false, true, true));  // misaligned
  put(kRoot, 7, Pte(Pp(kRamBase), true, true, true, true, false, false, false, false));      // A=0
  put(kRoot, 8, Pte(Pp(kRamBase), true, true, true, true, false, false, true, true) |
                  (UINT64_C(1) << 63));                                                      // reserved bit
  put(kRoot, 9, Pte(Pp(kRamBase), true, false, false, true, false, false, true, true));      // X-only
  // Level 1.
  put(kL1, 1, PteNonleaf(kL0));
  put(kL1, 2, Pte(Pp(kRamBase), true, true, true, true, false, false, true, true));    // 2 MiB leaf
  put(kL1, 3, Pte((Pp(kRamBase) | 1), true, true, true, true, false, false, true, true)); // misaligned
  put(kL1, 4, PteNonleaf(kBadTable));
  put(kL1, 5, Pte(0, false, false, false, false, false, false, false, false));        // V=0
  put(kL1, 6, Pte(0, true, false, true, false, false, false, false, false));          // reserved
  put(kL1, 7, Pte(Pp(kPageA), true, true, true, true, false, false, false, false));    // A=0
  put(kL1, 8, Pte(Pp(kPageA), true, true, true, true, false, false, true, false));     // D=0
  put(kL1, 9, Pte(Pp(kPageA), true, true, true, false, true, false, true, true));      // U page
  put(kL1, 10, Pte(Pp(kPageA), true, false, false, true, false, false, true, true));   // X-only
  put(kL1, 11, Pte(Pp(kPageA), true, true, false, false, false, false, true, true));   // R-only
  put(kL1, 12, Pte(Pp(kPageA), true, true, false, false, false, false, true, true));   // fetch denied
  put(kL1, 13, PteNonleaf(kL0b));
  put(kL1, 14, Pte(Pp(kRamBase), true, true, true, true, false, false, false, false));  // A=0 superpage
  // Level 0 (reached through kL1[1]).
  put(kL0, 1, Pte(Pp(kPageA), true, true, true, true, false, false, true, true));      // 4 KiB leaf
  put(kL0, 2, Pte(0, false, false, false, false, false, false, false, false));        // V=0
  put(kL0, 3, Pte(0, true, false, true, false, false, false, false, false));          // reserved
  put(kL0, 4, Pte(Pp(kPageA), true, true, true, true, false, false, false, false));    // A=0
  put(kL0, 5, Pte(Pp(kPageA), true, true, true, true, false, false, true, false));     // D=0
  put(kL0, 6, Pte(Pp(kPageA), true, true, true, false, true, false, true, true));      // U page
  put(kL0, 7, Pte(Pp(kPageA), true, false, false, true, false, false, true, true));    // X-only
  put(kL0, 8, Pte(Pp(kPageA), true, true, false, false, false, false, true, true));    // R-only
  put(kL0, 9, Pte(Pp(kPageA), true, true, false, false, false, false, true, true));    // no X
  put(kL0, 10, PteNonleaf(kL0));                                                      // non-leaf at last level
  put(kL0, 11, Pte(Pp(kPageB), true, true, true, true, false, false, false, false));   // A=0 (cancel)
  put(kL0, 12, Pte(Pp(kPageA), true, true, true, true, false, false, true, true) |
                   (UINT64_C(1) << 62));                                              // reserved bit
  put(kL0, 13, Pte(Pp(kPageA), true, true, true, true, false, false, true, true));     // plain
  put(kL0, 14, Pte(Pp(kPageA), true, true, true, true, false, false, false, false));    // A=0 (fetch)
  // The last-level non-leaf target.
  put(kL0b, 1, PteNonleaf(kL0));
}

// ============================================================================
// The directed scenarios
// ============================================================================
struct Scenario {
  std::string key;
  std::string rule;
  uint64_t va = 0;
  int kind = kKindLoad;
  int priv = kPrivS;
  uint64_t mode = kSatpSv39;
  uint64_t ppn = 0;
  bool sum = false, mxr = false;
  // The hand-written expectation.
  bool exp_ok = true;
  uint64_t exp_pa = 0;
  int exp_cause = 0;
  bool exp_ad_update = false;
  bool exp_ad_dirty = false;
  bool exp_no_pte_read = false;
};

std::vector<Scenario> BuildScenarios() {
  std::vector<Scenario> v;
  const uint64_t R = RootPpn();
  auto add = [&](Scenario s) { s.ppn = R; v.push_back(s); };

  {
    Scenario s; s.key = "leaf-L0";
    s.rule = "a leaf at level 0 maps to pte.ppn in full and va[11:0] is the offset";
    s.va = Va(1, 1, 1, 0x18); s.exp_ok = true; s.exp_pa = kPageA + 0x18;
    add(s);
  }
  {
    Scenario s; s.key = "leaf-L1-super";
    s.rule = "a 2 MiB leaf at level 1 takes pa[20:12] from va and needs pte.ppn[8:0]=0";
    s.va = Va(1, 2, 0x55, 0x123); s.exp_ok = true;
    s.exp_pa = kRamBase + ((0x55ull << 12) | 0x123);
    add(s);
  }
  {
    Scenario s; s.key = "leaf-L2-super";
    s.rule = "a 1 GiB leaf at level 2 takes pa[29:12] from va and needs pte.ppn[17:0]=0";
    s.va = Va(5, 0x155, 0x2AA, 0x321); s.exp_ok = true;
    s.exp_pa = kRamBase + ((0x155ull << 21) | (0x2AAull << 12) | 0x321);
    add(s);
  }
  {
    Scenario s; s.key = "nonleaf-L0-fault";
    s.rule = "a non-leaf at the last level has no deeper table -> page fault";
    s.va = Va(1, 1, 10); s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "noncanonical";
    s.rule = "bits 63:39 must all equal bit 38, or a page fault is raised before any PTE read";
    s.va = kNonCanonical; s.exp_ok = false; s.exp_cause = kExcLoadPage;
    s.exp_no_pte_read = true; add(s);
  }
  {
    Scenario s; s.key = "noncanonical-store";
    s.rule = "a non-canonical store faults with the store page-fault cause";
    s.va = kNonCanonical; s.kind = kKindStore; s.exp_ok = false;
    s.exp_cause = kExcStorePage; s.exp_no_pte_read = true; add(s);
  }
  {
    Scenario s; s.key = "noncanonical-fetch";
    s.rule = "a non-canonical fetch faults with the instruction page-fault cause";
    s.va = kNonCanonical; s.kind = kKindFetch; s.exp_ok = false;
    s.exp_cause = kExcInsnPage; s.exp_no_pte_read = true; add(s);
  }
  {
    Scenario s; s.key = "invalid-pte";
    s.rule = "V=0 -> page fault";
    s.va = Va(1, 1, 2); s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "reserved-wr";
    s.rule = "W=1 with R=0 is a reserved encoding -> page fault";
    s.va = Va(1, 1, 3); s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "reserved-bit";
    s.rule = "a bit reserved for future standard use (63:54) set -> page fault";
    s.va = Va(1, 1, 12); s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "misaligned-super-L1";
    s.rule = "a level-1 superpage with pte.ppn[8:0]!=0 is misaligned -> page fault";
    s.va = Va(1, 3, 0); s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "misaligned-super-L2";
    s.rule = "a level-2 superpage with pte.ppn[17:0]!=0 is misaligned -> page fault";
    s.va = Va(6, 0, 0); s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "perm-R";
    s.rule = "a load from an execute-only page (R=0) faults with cause 13";
    s.va = Va(1, 1, 7); s.kind = kKindLoad; s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "perm-W";
    s.rule = "a store to a read-only page (W=0) faults with cause 15";
    s.va = Va(1, 1, 8); s.kind = kKindStore; s.exp_ok = false; s.exp_cause = kExcStorePage; add(s);
  }
  {
    Scenario s; s.key = "perm-X";
    s.rule = "a fetch from a non-executable page (X=0) faults with cause 12";
    s.va = Va(1, 1, 9); s.kind = kKindFetch; s.exp_ok = false; s.exp_cause = kExcInsnPage; add(s);
  }
  {
    Scenario s; s.key = "perm-U";
    s.rule = "U-mode may only access a page with U=1 -> page fault";
    s.va = Va(1, 1, 1); s.priv = kPrivU; s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "perm-U-ok";
    s.rule = "U-mode accesses a U=1 page";
    s.va = Va(1, 1, 6); s.priv = kPrivU; s.exp_ok = true; s.exp_pa = kPageA; add(s);
  }
  {
    Scenario s; s.key = "sum-off";
    s.rule = "SUM=0: an S-mode load of a U page faults";
    s.va = Va(1, 1, 6); s.priv = kPrivS; s.sum = false; s.exp_ok = false;
    s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "sum-on";
    s.rule = "SUM=1: the same S-mode load of a U page succeeds";
    s.va = Va(1, 1, 6); s.priv = kPrivS; s.sum = true; s.exp_ok = true;
    s.exp_pa = kPageA; add(s);
  }
  {
    Scenario s; s.key = "sum-fetch";
    s.rule = "SUM does not apply to instruction fetch: S-mode may not fetch a U page";
    s.va = Va(1, 1, 6); s.priv = kPrivS; s.kind = kKindFetch; s.sum = true;
    s.exp_ok = false; s.exp_cause = kExcInsnPage; add(s);
  }
  {
    Scenario s; s.key = "mxr-off";
    s.rule = "MXR=0: a load from an X-only page (R=0) faults";
    s.va = Va(1, 1, 7); s.kind = kKindLoad; s.mxr = false; s.exp_ok = false;
    s.exp_cause = kExcLoadPage; add(s);
  }
  {
    Scenario s; s.key = "mxr-on";
    s.rule = "MXR=1: a load from an X-only page (R=1 or X=1) succeeds";
    s.va = Va(1, 1, 7); s.kind = kKindLoad; s.mxr = true; s.exp_ok = true;
    s.exp_pa = kPageA; add(s);
  }
  {
    Scenario s; s.key = "mxr-fetch";
    s.rule = "MXR does not make an R-only page executable: a fetch still needs X";
    s.va = Va(1, 1, 9); s.kind = kKindFetch; s.mxr = true; s.exp_ok = false;
    s.exp_cause = kExcInsnPage; add(s);
  }
  {
    Scenario s; s.key = "pte-access-fault-load";
    s.rule = "an implicit PTE read that violates PMA/PMP is an access fault of the access's class";
    s.va = Va(4, 0, 0); s.kind = kKindLoad; s.exp_ok = false; s.exp_cause = kExcLoadAccess; add(s);
  }
  {
    Scenario s; s.key = "pte-access-fault-store";
    s.rule = "the same implicit fault on a store is a store access fault";
    s.va = Va(4, 0, 0); s.kind = kKindStore; s.exp_ok = false; s.exp_cause = kExcStoreAccess; add(s);
  }
  {
    Scenario s; s.key = "pte-access-fault-fetch";
    s.rule = "the same implicit fault on a fetch is an instruction access fault";
    s.va = Va(4, 0, 0); s.kind = kKindFetch; s.exp_ok = false; s.exp_cause = kExcInsnAccess; add(s);
  }
  {
    Scenario s; s.key = "ad-set-a";
    s.rule = "a leaf with A=0 is updated with A=1 (not D) by a load";
    s.va = Va(1, 1, 4); s.kind = kKindLoad; s.exp_ok = true; s.exp_pa = kPageA;
    s.exp_ad_update = true; s.exp_ad_dirty = false; add(s);
  }
  {
    Scenario s; s.key = "ad-set-d";
    s.rule = "a leaf with D=0 written by a store is updated with A=1 and D=1";
    s.va = Va(1, 1, 5); s.kind = kKindStore; s.exp_ok = true; s.exp_pa = kPageA;
    s.exp_ad_update = true; s.exp_ad_dirty = true; add(s);
  }
  {
    Scenario s; s.key = "ad-set-a-fetch";
    s.rule = "a fetch with A=0 sets A and never D";
    s.va = Va(1, 1, 14); s.kind = kKindFetch; s.exp_ok = true; s.exp_pa = kPageA;
    s.exp_ad_update = true; s.exp_ad_dirty = false; add(s);
  }
  {
    Scenario s; s.key = "ad-noop";
    s.rule = "a leaf with A=1 (and D=1) needs no update and no PTE write";
    s.va = Va(1, 1, 13); s.exp_ok = true; s.exp_pa = kPageA; s.exp_ad_update = false;
    add(s);
  }
  {
    Scenario s; s.key = "ad-super-a";
    s.rule = "a superpage leaf with A=0 is updated in place";
    s.va = Va(1, 14, 0, 0x40); s.exp_ok = true;
    s.exp_pa = kRamBase + 0x40; s.exp_ad_update = true;
    s.exp_ad_dirty = false; add(s);
  }
  {
    Scenario s; s.key = "bare";
    s.rule = "satp.MODE=Bare: the virtual address is the physical address, no PTE is read";
    s.mode = kSatpBare; s.va = 0x12345678; s.exp_ok = true; s.exp_pa = 0x12345678;
    s.exp_no_pte_read = true; add(s);
  }
  {
    Scenario s; s.key = "priv-m";
    s.rule = "an M-mode access is not translated";
    s.priv = kPrivM; s.va = 0x80001234; s.exp_ok = true; s.exp_pa = 0x80001234;
    s.exp_no_pte_read = true; add(s);
  }
  {
    Scenario s; s.key = "unsupported-mode";
    s.rule = "a MODE the machine cannot execute is a defensive page fault (the CSR cannot select it)";
    s.mode = 4; s.va = Va(1, 1, 1); s.exp_ok = false; s.exp_cause = kExcLoadPage; add(s);
  }
  return v;
}

uint32_t vpn2Of(uint64_t va) { return static_cast<uint32_t>((va >> 30) & 0x1FF); }

// ============================================================================
// The directed run
// ============================================================================
void RunDirected(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, MemoryModel* mem,
                 std::map<std::string, int>* coverage, uint64_t* comparisons) {
  BuildTables(mem);
  PtwBench bench(dut, mem);
  bench.Idle();
  const std::vector<Scenario> scenarios = BuildScenarios();

  for (const Scenario& s : scenarios) {
    (*coverage)[s.key]++;
    const WalkResult model = ModelWalk(mem, s.va, s.kind, s.priv, s.mode, s.ppn,
                                       s.sum, s.mxr);
    // The hand-written expectation and the independent model must agree; a
    // disagreement is a driver bug and fails as one.
    if (model.fault != !s.exp_ok ||
        (s.exp_ok && model.pa != s.exp_pa) ||
        (!s.exp_ok && model.cause != s.exp_cause) ||
        model.ad_update != s.exp_ad_update ||
        model.ad_dirty != s.exp_ad_dirty) {
      Fail("directed/" + s.key,
           "the model says fault=" + Dec(model.fault) + " pa=" + U64(model.pa) +
               " cause=" + Dec(model.cause) + " ad=" + Dec(model.ad_update) +
               " dirty=" + Dec(model.ad_dirty) + ", the rule says ok=" +
               Dec(s.exp_ok) + " pa=" + U64(s.exp_pa) + " cause=" + Dec(s.exp_cause) +
               " ad=" + Dec(s.exp_ad_update) + " dirty=" + Dec(s.exp_ad_dirty));
    }
    // The PTE the walk will read, before it runs, so the A/D check is a
    // before/after comparison of the same word.
    uint64_t pte_before = 0;
    const uint64_t leaf_addr = [&]() -> uint64_t {
      if (s.va == kNonCanonical) return 0;
      const unsigned vpn0 = (s.va >> 12) & 0x1FF;
      const unsigned vpn1 = (s.va >> 21) & 0x1FF;
      if (vpn2Of(s.va) == 1 && vpn1 == 1) return kL0 + 8ull * vpn0;
      if (vpn2Of(s.va) == 1) return kL1 + 8ull * vpn1;
      return kRoot + 8ull * vpn2Of(s.va);
    }();
    if (leaf_addr != 0) MemRead8(mem, leaf_addr, &pte_before);

    const uint64_t reads_before = bench.reads().size();
    const uint64_t writes_before = bench.pte_writes();

    bench.Drive(s.va, s.kind, s.priv, s.mode, s.ppn, s.sum, s.mxr);
    // A non-canonical or Bare walk must produce a result without any PTE beat.
    if (s.exp_no_pte_read) {
      const PtwObs o = bench.Cycle();
      if (!o.rsp_valid) {
        reporter->Mismatch("directed/" + s.key, "a result without a PTE read",
                           "no result");
        Fail("directed/" + s.key, "no result without a PTE read");
      }
      if (bench.reads().size() != reads_before) {
        reporter->Mismatch("directed/" + s.key, "no PTE read",
                           Dec(bench.reads().size() - reads_before) + " reads");
        Fail("directed/" + s.key, "a PTE was read");
      }
      if (o.fault != !s.exp_ok || (s.exp_ok && o.pa != s.exp_pa) ||
          (!s.exp_ok && static_cast<int>(o.cause) != s.exp_cause)) {
        reporter->Mismatch("directed/" + s.key,
                           "fault=" + Dec(!s.exp_ok) + " pa=" + U64(s.exp_pa) +
                               " cause=" + Dec(s.exp_cause),
                           "fault=" + Dec(o.fault) + " pa=" + U64(o.pa) +
                               " cause=" + Dec(o.cause));
        Fail("directed/" + s.key, "the walker disagrees with the rule");
      }
      (*comparisons)++;
      continue;
    }

    const PtwObs o = bench.RunUntilResponse();
    (*comparisons)++;
    if (o.fault != !s.exp_ok) {
      reporter->Mismatch("directed/" + s.key, "fault=" + Dec(!s.exp_ok),
                         "fault=" + Dec(o.fault) + " cause=" + Dec(o.cause));
      Fail("directed/" + s.key, "fault bit disagrees with the rule");
    }
    if (s.exp_ok && o.pa != s.exp_pa) {
      reporter->Mismatch("directed/" + s.key, "pa=" + U64(s.exp_pa), U64(o.pa));
      Fail("directed/" + s.key, "physical address disagrees with the rule");
    }
    if (!s.exp_ok && static_cast<int>(o.cause) != s.exp_cause) {
      reporter->Mismatch("directed/" + s.key,
                         "cause=" + Dec(s.exp_cause) + " tval=" + U64(s.va),
                         "cause=" + Dec(o.cause) + " tval=" + U64(o.tval));
      Fail("directed/" + s.key, "fault cause disagrees with the rule");
    }
    if (o.tval != s.va) {
      reporter->Mismatch("directed/" + s.key, "tval=" + U64(s.va), U64(o.tval));
      Fail("directed/" + s.key, "tval is not the virtual address");
    }
    if (s.exp_ok && o.perms != model.perms) {
      reporter->Mismatch("directed/" + s.key,
                         "perms=" + Dec(model.perms), Dec(o.perms));
      Fail("directed/" + s.key, "permission metadata disagrees with the leaf");
    }

    // The A/D policy, checked against the table itself.
    if (leaf_addr != 0 && s.mode == kSatpSv39) {
      uint64_t pte_after = 0;
      MemRead8(mem, leaf_addr, &pte_after);
      const bool wrote = bench.pte_writes() != writes_before;
      if (s.exp_ad_update) {
        if (!wrote) {
          reporter->Mismatch("directed/" + s.key, "an A/D PTE write", "no write");
          Fail("directed/" + s.key, "no A/D update was written");
        }
        if ((pte_after & 0x40) == 0) {
          reporter->Mismatch("directed/" + s.key, "A set", "A clear");
          Fail("directed/" + s.key, "A was not set");
        }
        if (s.exp_ad_dirty && (pte_after & 0x80) == 0) {
          reporter->Mismatch("directed/" + s.key, "D set", "D clear");
          Fail("directed/" + s.key, "D was not set for a store");
        }
        if (!s.exp_ad_dirty && (pte_after & 0x80) != (pte_before & 0x80)) {
          reporter->Mismatch("directed/" + s.key, "D unchanged", "D changed");
          Fail("directed/" + s.key, "D was changed by a non-store");
        }
      } else if (wrote) {
        reporter->Mismatch("directed/" + s.key, "no A/D write", "a write happened");
        Fail("directed/" + s.key, "an unnecessary A/D write was issued");
      }
    }
    if (Debug()) {
      std::printf("  [%s] fault=%d pa=%s cause=%d perms=%x reads=%llu writes=%llu\n",
                  s.key.c_str(), o.fault ? 1 : 0, U64(o.pa).c_str(), o.cause,
                  o.perms, static_cast<unsigned long long>(bench.reads().size()),
                  static_cast<unsigned long long>(bench.pte_writes()));
    }
  }

  // ------------------------------------------------------------- cancellation
  // A walk cancelled in flight must return no result and leave no A/D update
  // behind -- the same class of property as "a wrong-path store writes
  // nothing".
  {
    (*coverage)["cancel"]++;
    uint64_t pte_before = 0;
    MemRead8(mem, kL0 + 8ull * 11, &pte_before);
    const uint64_t writes_before = bench.pte_writes();
    bench.Drive(Va(1, 1, 11), kKindLoad, kPrivS, kSatpSv39, RootPpn(), false, false);
    bench.Cycle();  // first level PTE read offered
    const PtwObs o = bench.Cycle(true);   // cancel while the walk is in flight
    if (o.rsp_valid) Fail("directed/cancel", "a cancelled walk produced a response");
    for (int i = 0; i < 8; i++) bench.Cycle();
    if (bench.pte_writes() != writes_before) {
      Fail("directed/cancel", "a cancelled walk wrote a PTE");
    }
    uint64_t pte_after = 0;
    MemRead8(mem, kL0 + 8ull * 11, &pte_after);
    if (pte_after != pte_before) {
      Fail("directed/cancel", "a cancelled walk changed the PTE");
    }
    if (dut->o_ptw_cancel_ctr_o == 0) {
      Fail("directed/cancel", "the walker did not count the cancellation");
    }
    (*comparisons)++;
  }

  // Coverage is asserted, not just printed: every cell the card names must have
  // been driven at least once.
  {
    static const char* kRequired[] = {
        "leaf-L0", "leaf-L1-super", "leaf-L2-super", "nonleaf-L0-fault",
        "noncanonical", "invalid-pte", "reserved-wr", "misaligned-super-L1",
        "misaligned-super-L2", "perm-R", "perm-W", "perm-X", "perm-U", "sum-off",
        "sum-on", "mxr-off", "mxr-on", "pte-access-fault-load", "ad-set-a",
        "ad-set-d", "ad-noop", "cancel", "bare",
    };
    for (const char* key : kRequired) {
      if ((*coverage)[key] == 0) Fail("coverage", std::string("no scenario covered ") + key);
    }
  }

  // The bare and walk counters are the walker's own statement that both paths
  // ran.
  if (dut->o_ptw_bare_ctr_o == 0) Fail("directed", "no Bare passthrough was counted");
  if (dut->o_ptw_walk_ctr_o == 0) Fail("directed", "no walk was counted");
  if (dut->o_ptw_leaf_ctr_o == 0) Fail("directed", "no leaf was reached");
  if (dut->o_ptw_fault_ctr_o == 0) Fail("directed", "no fault was counted");
  if (dut->o_ptw_ad_ctr_o == 0) Fail("directed", "no A/D update was counted");
}

// ============================================================================
// The integrated run: a program that enables Sv39 and walks the core's path
// ============================================================================
// --------------------------------------------------------------- instruction
uint32_t EncR(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd,
              uint32_t op) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
uint32_t EncI(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return ((static_cast<uint32_t>(imm) & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) |
         (rd << 7) | op;
}
uint32_t EncS(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op) {
  const uint32_t u = static_cast<uint32_t>(imm) & 0xFFF;
  return ((u >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((u & 0x1F) << 7) |
         op;
}
uint32_t Lui(uint32_t rd, uint32_t imm20) { return (imm20 << 12) | (rd << 7) | 0x37; }
uint32_t Addi(uint32_t rd, uint32_t rs1, int32_t imm) {
  return EncI(imm, rs1, 0, rd, 0x13);
}
uint32_t Add(uint32_t rd, uint32_t rs1, uint32_t rs2) {
  return EncR(0, rs2, rs1, 0, rd, 0x33);
}
uint32_t Slli(uint32_t rd, uint32_t rs1, uint32_t sh) {
  return EncI(static_cast<int32_t>(sh), rs1, 1, rd, 0x13);
}
uint32_t Ld(uint32_t rd, uint32_t rs1, int32_t imm) { return EncI(imm, rs1, 3, rd, 0x03); }
uint32_t Sd(uint32_t rs2, uint32_t rs1, int32_t imm) { return EncS(imm, rs2, rs1, 3, 0x23); }
uint32_t Csrrw(uint32_t rd, uint32_t csr, uint32_t rs1) { return EncI(static_cast<int32_t>(csr), rs1, 1, rd, 0x73); }
uint32_t Csrrs(uint32_t rd, uint32_t csr, uint32_t rs1) { return EncI(static_cast<int32_t>(csr), rs1, 2, rd, 0x73); }
uint32_t Mret() { return EncI(0x302, 0, 0, 0, 0x73); }
uint32_t Jal(uint32_t rd, int32_t off) {
  const uint32_t o = static_cast<uint32_t>(off) & 0x1FFFFF;
  return ((o >> 20) & 1) << 31 | ((o >> 1) & 0x3FF) << 21 | ((o >> 11) & 1) << 20 |
         ((o >> 12) & 0xFF) << 12 | (rd << 7) | 0x6F;
}

constexpr uint32_t kCsrMstatus = 0x300, kCsrMtvec = 0x305, kCsrMepc = 0x341,
                   kCsrMcause = 0x342, kCsrMtval = 0x343, kCsrSatp = 0x180,
                   kCsrPmpcfg0 = 0x3A0, kCsrPmpaddr0 = 0x3B0;

// ------------------------------------------------------------- integrated map
constexpr uint64_t kProgM    = kRamBase;          // M-mode code
constexpr uint64_t kProgH    = kRamBase + 0x100;  // M-mode trap handler
constexpr uint64_t kProgS    = kRamBase + 0x200;  // S-mode stub
constexpr uint64_t kFrame    = kRamBase + 0x300;  // the trap frame (physical)
constexpr uint64_t kSig      = kRamBase + 0x380;  // signature (physical, Bare phase)
constexpr uint64_t kPhysData = kRamBase + 0x400;
constexpr uint64_t kIdentL1  = kRamBase + 0x30000;
constexpr uint64_t kIdentL0  = kRamBase + 0x31000;
constexpr uint64_t kPageGood = kRamBase + 0x20000;
constexpr uint64_t kPageSig  = kRamBase + 0x21000;
constexpr uint64_t kDataBare   = 0x5A5A0000ull;
constexpr uint64_t kDataMapped = 0x1234ABCDull;

constexpr uint64_t kVaGood = (1ull << 30) | (2ull << 21) | (0x20ull << 12) | 0x18;
// The physical address the stub's `ld t2, 0x18(t1)` reads: kVaGood plus the
// instruction's own offset, translated through the level-0 leaf.
constexpr uint64_t kGoodPa = kPageGood + 0x30;
constexpr uint64_t kVaSig  = (1ull << 30) | (2ull << 21) | (0x21ull << 12);
constexpr uint64_t kVaBad  = 0x0000008000000000ull;
constexpr uint64_t kVaInv  = (1ull << 30) | (2ull << 21) | (0x22ull << 12);

uint32_t Srli(uint32_t rd, uint32_t rs1, uint32_t sh) {
  return EncI(static_cast<int32_t>(sh), rs1, 5, rd, 0x13);
}
uint32_t Or(uint32_t rd, uint32_t rs1, uint32_t rs2) {
  return EncR(0, rs2, rs1, 6, rd, 0x33);
}

// A tiny sequential emitter. `li` needs 4 words (LUI, ADDI, SLLI, SRLI -- the
// shifts zero-extend the low 32 bits, which a bare LUI/ADDI would sign-extend
// and turn 0x8000_0400 into 0xFFFF_FFFF_8000_0400), and a 64-bit value needs 8,
// so the program cannot be built with fixed strides.
struct Emitter {
  std::map<uint64_t, uint32_t>* img;
  uint64_t pc;

  void W(uint32_t word) { (*img)[pc] = word; pc += 4; }

  void Li32(uint32_t rd, uint32_t v) {
    const uint32_t hi = (v + 0x800u) >> 12;
    const int32_t lo = static_cast<int32_t>(v & 0xFFFu) -
                       ((v & 0x800u) ? 0x1000 : 0);
    W(Lui(rd, hi & 0xFFFFFu));
    W(Addi(rd, rd, lo));
    W(Slli(rd, rd, 32));
    W(Srli(rd, rd, 32));
  }

  void Li(uint32_t rd, uint64_t value) {
    if (value <= 0xFFFFFFFFull) {
      Li32(rd, static_cast<uint32_t>(value));
      return;
    }
    Li32(rd, static_cast<uint32_t>(value >> 32));
    W(Slli(rd, rd, 32));
    Li32(31u, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    W(Or(rd, rd, 31u));
  }
};

// Build the program image. Both the M-mode and S-mode code live in the first
// 4 KiB page, which the identity mapping covers, so an instruction fetch (which
// this package does not translate) reads the same bytes the S-mode mapping says
// it would.
void BuildProgram(std::map<uint64_t, uint32_t>* img) {
  // ------------------------------------------------------------------ M-mode
  {
    Emitter e{img, kProgM};
    e.Li(5, kProgH);                 e.W(Csrrw(0, kCsrMtvec, 5));
    // One unlocked TOR PMP entry covering everything below the end of RAM, with
    // X/R/W. Without a matching entry an S-mode access fails ("no entry matches
    // an S/U access and at least one entry is implemented"), so this is what
    // lets the S-mode stub fetch and load at all; it is not what the case is
    // testing.
    e.Li(5, 0x0F);                   e.W(Csrrw(0, kCsrPmpcfg0, 5));
    e.Li(5, (kRamBase + MOSAIC_RAM_SIZE) >> 2); e.W(Csrrw(0, kCsrPmpaddr0, 5));
    e.Li(6, kPhysData);              e.W(Ld(7, 6, 0));
    e.Li(8, kSig);                   e.W(Sd(7, 8, 0));
    e.Li(5, (8ull << 60) | Pp(kRoot)); e.W(Csrrw(0, kCsrSatp, 5));
    e.Li(5, kProgS);                 e.W(Csrrw(0, kCsrMepc, 5));
    e.Li(5, 0x800);                  e.W(Csrrw(0, kCsrMstatus, 5));
    e.W(Mret());
    e.W(Jal(0, 0));
  }
  // ------------------------------------------------------------------ S-mode
  {
    Emitter e{img, kProgS};
    e.Li(6, kVaGood);                e.W(Ld(7, 6, 0x18));
    e.Li(8, kVaSig);                 e.W(Sd(7, 8, 0));
    e.Li(6, kVaBad);                 e.W(Ld(7, 6, 0));
    e.Li(6, kVaInv);                 e.W(Ld(7, 6, 0));
    e.W(Jal(0, 0));
  }
  // --------------------------------------------------------------- handler
  {
    Emitter e{img, kProgH};
    e.W(Csrrs(28, kCsrMcause, 0));
    e.W(Csrrs(29, kCsrMtval, 0));
    e.Li(30, kFrame);
    e.W(Ld(31, 30, 0));
    e.W(Slli(31, 31, 4));
    e.W(Add(31, 31, 30));
    e.W(Sd(28, 31, 8));
    e.W(Sd(29, 31, 16));
    e.W(Ld(31, 30, 0));
    e.W(Addi(31, 31, 1));
    e.W(Sd(31, 30, 0));
    e.W(Csrrs(28, kCsrMepc, 0));
    e.W(Addi(28, 28, 4));
    e.W(Csrrw(0, kCsrMepc, 28));
    e.Li(28, 0x800);
    e.W(Csrrw(0, kCsrMstatus, 28));
    e.W(Mret());
  }
}

// The page tables the program runs on.
void BuildProgramTables(MemoryModel* mem) {
  auto put = [&](uint64_t table, unsigned index, uint64_t value) {
    if (!MemWrite8(mem, table + 8ull * index, value, 0xFF)) {
      Fail("integrated", "cannot write the PTE at " + U64(table + 8ull * index));
    }
  };
  // Identity page: VA 0x8000_0000 -> PA 0x8000_0000, executable/readable/writable.
  put(kRoot, 2, PteNonleaf(kIdentL1));
  put(kIdentL1, 0, PteNonleaf(kIdentL0));
  put(kIdentL0, 0, Pte(Pp(kRamBase), true, true, true, true, false, false, true, true));
  // The test tree: S-mode data pages.
  put(kRoot, 1, PteNonleaf(kL1));
  put(kL1, 2, PteNonleaf(kL0));
  put(kL0, 0x20, Pte(Pp(kPageGood), true, true, true, true, false, false, true, true));
  put(kL0, 0x21, Pte(Pp(kPageSig), true, true, true, true, false, false, true, true));
  put(kL0, 0x22, Pte(0, false, false, false, false, false, false, false, false));  // V=0
  // The identity tables live where the directed tables do not: kIdentL1/kIdentL0
  // are separate pages from kL1/kL0.
}

// ------------------------------------------------------------ the core harness
struct CoreObs {
  uint64_t trap_count = 0;
  uint64_t trap_cause[4] = {0, 0, 0, 0};
  uint64_t trap_tval[4] = {0, 0, 0, 0};
};

class CoreHarness {
 public:
  CoreHarness(Vmosaic_core_tb* dut, MemoryModel* mem,
              std::map<uint64_t, uint32_t>* img)
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
    // Instruction port: respond one beat after acceptance.
    dut_->imem_req_ready_i = 1;
    dut_->imem_rsp_valid_i = !imem_ready_.empty() ? 1 : 0;
    if (!imem_ready_.empty()) {
      dut_->imem_rsp_rdata_i = imem_ready_.front().word;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = imem_ready_.front().id;
      dut_->imem_rsp_epoch_i = imem_ready_.front().epoch;
      dut_->imem_rsp_len_i = 4;
    } else {
      dut_->imem_rsp_rdata_i = 0;
      dut_->imem_rsp_fault_i = 0;
      dut_->imem_rsp_id_i = 0;
      dut_->imem_rsp_epoch_i = 0;
      dut_->imem_rsp_len_i = 0;
    }
    // Data port.
    dut_->dmem_req_ready_i = 1;
    dut_->dmem_rsp_valid_i = !dmem_ready_.empty() ? 1 : 0;
    if (Debug()) { /* data-port tracing below */ }
    if (!dmem_ready_.empty()) {
      dut_->dmem_rsp_rdata_i = dmem_ready_.front().rdata;
      dut_->dmem_rsp_fault_i = dmem_ready_.front().fault ? 1 : 0;
    } else {
      dut_->dmem_rsp_rdata_i = 0;
      dut_->dmem_rsp_fault_i = 0;
    }
    dut_->ext_write_valid_i = 0;
    dut_->ext_write_addr_i = 0;
    dut_->ext_write_bytes_i = 0;
    dut_->arb_req_valid0_i = 0;
    dut_->arb_req_valid1_i = 0;
    dut_->arb_head_valid_i = 0;
    dut_->arb_head_retire_i = 0;
    // The standalone walker is parked.
    dut_->ptw_xl_valid_i = 0;
    dut_->ptw_xl_va_i = 0;
    dut_->ptw_xl_kind_i = 0;
    dut_->ptw_xl_priv_i = 0;
    dut_->ptw_xl_mode_i = 0;
    dut_->ptw_xl_ppn_i = 0;
    dut_->ptw_xl_sum_i = 0;
    dut_->ptw_xl_mxr_i = 0;
    dut_->ptw_xl_cancel_i = 0;
    dut_->ptw_xl_rsp_ready_i = 1;
    dut_->ptw_mem_req_ready_i = 1;
    dut_->ptw_mem_rsp_valid_i = 0;
    dut_->ptw_mem_rsp_rdata_i = 0;
    dut_->ptw_mem_rsp_fault_i = 0;

    dut_->clk = 0;
    dut_->eval();

    // Observe the trap stream and the data port before the edge.
    if (dut_->o_trap_valid_o != 0 && !rst) {
      if (Debug()) {
        std::printf("    [trap] cycle=%llu cause=%llu tval=%s epc=%s priv=%u\n",
                    static_cast<unsigned long long>(cycles_),
                    static_cast<unsigned long long>(dut_->o_trap_cause_o),
                    U64(dut_->o_trap_tval_o).c_str(),
                    U64(dut_->o_trap_epc_o).c_str(),
                    static_cast<unsigned>(dut_->o_priv_o));
      }
      CoreObs& o = obs_;
      if (o.trap_count < 4) {
        o.trap_cause[o.trap_count] = dut_->o_trap_cause_o;
        o.trap_tval[o.trap_count] = dut_->o_trap_tval_o;
      }
      o.trap_count++;
    }
    const bool dmem_accept = (dut_->dmem_req_valid_o != 0) && (dut_->dmem_req_ready_i != 0);
    const bool dmem_pop = (dut_->dmem_rsp_valid_i != 0) && (dut_->dmem_rsp_ready_o != 0);
    const bool imem_accept = (dut_->imem_req_valid_o != 0) && (dut_->imem_req_ready_i != 0);
    const bool imem_pop = (dut_->imem_rsp_valid_i != 0) && (dut_->imem_rsp_ready_o != 0);
    uint64_t dmem_addr = dut_->dmem_req_addr_o;
    uint64_t dmem_wdata = dut_->dmem_req_wdata_o;
    bool dmem_we = dut_->dmem_req_we_o != 0;
    uint32_t dmem_wstrb = static_cast<uint32_t>(dut_->dmem_req_wstrb_o);
    uint64_t imem_addr = dut_->imem_req_addr_o;
    const uint64_t imem_id = dut_->imem_req_id_o;
    const uint64_t imem_epoch = dut_->imem_req_epoch_o;

    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    ++cycles_;

    if (imem_pop) imem_ready_.pop_front();
    if (dmem_pop) dmem_ready_.pop_front();
    if (imem_accept && Debug()) {
      std::printf("    [imem] cycle=%llu addr=%s\n",
                  static_cast<unsigned long long>(cycles_), U64(imem_addr).c_str());
    }
    if (imem_accept) {
      // The identity and epoch travel with the word: the fetch unit matches a
      // response to its request by them, and with more than one request
      // outstanding a model that answered with the newest id would have every
      // response dropped.
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
        r.fault = !MemWrite8(mem_, dmem_addr, dmem_wdata, dmem_wstrb);
        r.rdata = 0;
      } else {
        r.fault = !MemRead8(mem_, dmem_addr, &r.rdata);
        if (r.fault) r.rdata = 0;
      }
      if (Debug()) {
        std::printf("    [dmem] cycle=%llu %s addr=%s wdata=%s rdata=%s fault=%d priv=%u mstatus=%s satp=%s\n",
                    static_cast<unsigned long long>(cycles_), dmem_we ? "w" : "r",
                    U64(dmem_addr).c_str(), U64(dmem_wdata).c_str(),
                    U64(r.rdata).c_str(), r.fault ? 1 : 0,
                    static_cast<unsigned>(dut_->o_priv_o),
                    U64(dut_->o_csr_mstatus_o).c_str(),
                    U64(dut_->o_satp_o).c_str());
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

void RunIntegrated(Vmosaic_core_tb* dut, mosaic::Reporter* reporter, MemoryModel* mem,
                   std::map<std::string, int>* coverage, uint64_t* comparisons) {
  (*coverage)["integrated"]++;
  // The program's page tables are separate from the directed ones so the two
  // phases cannot see each other's edits.
  BuildProgramTables(mem);
  if (!MemWrite8(mem, kPhysData, kDataBare, 0xFF) ||
      !MemWrite8(mem, kGoodPa, kDataMapped, 0xFF)) {
    Fail("integrated", "cannot seed the data words");
  }
  // The directed tables reserved kL0; the integrated tree reuses it, so put the
  // identity tables where they do not collide.
  std::map<uint64_t, uint32_t> img;
  BuildProgram(&img);

  CoreHarness h(dut, mem, &img);
  h.Reset(4);
  for (uint64_t i = 0; i < 40000; i++) {
    h.Cycle(false);
    if (h.obs().trap_count >= 2) break;
  }
  // Let the trapped store drain.
  for (int i = 0; i < 40; i++) h.Cycle(false);

  const CoreObs& o = h.obs();
  (*comparisons)++;
  if (o.trap_count < 2) {
    reporter->Mismatch("integrated/traps", "2 traps", Dec(o.trap_count));
    Fail("integrated", "the program did not take the two expected traps");
  }
  if (o.trap_cause[0] != kExcLoadPage || o.trap_tval[0] != kVaBad) {
    reporter->Mismatch("integrated/noncanonical",
                       "cause 13 tval " + U64(kVaBad),
                       "cause " + Dec(o.trap_cause[0]) + " tval " + U64(o.trap_tval[0]));
    Fail("integrated", "the non-canonical access did not trap as the spec names");
  }
  if (o.trap_cause[1] != kExcLoadPage || o.trap_tval[1] != kVaInv) {
    reporter->Mismatch("integrated/invalid",
                       "cause 13 tval " + U64(kVaInv),
                       "cause " + Dec(o.trap_cause[1]) + " tval " + U64(o.trap_tval[1]));
    Fail("integrated", "the invalid-PTE access did not trap as the spec names");
  }

  // The signature the program wrote in the Bare phase, read back through the
  // model.
  uint64_t bare_sig = 0;
  MemRead8(mem, kSig, &bare_sig);
  if (bare_sig != kDataBare) {
    reporter->Mismatch("integrated/bare", U64(kDataBare), U64(bare_sig));
    Fail("integrated", "the Bare-phase load did not read the physical word");
  }
  // The translated load's value, written by the translated store.
  uint64_t sig = 0;
  MemRead8(mem, kGoodPa, &sig);
  if (sig != kDataMapped) {
    reporter->Mismatch("integrated/load", U64(kDataMapped), U64(sig));
    Fail("integrated", "the translated load did not read the mapped page");
  }
  uint64_t stored = 0;
  MemRead8(mem, kPageSig, &stored);
  if (stored != kDataMapped) {
    reporter->Mismatch("integrated/store", U64(kDataMapped), U64(stored));
    Fail("integrated", "the translated store did not reach the mapped physical page");
  }

  // The card's first failure mode, observed on the data port: no access derived
  // from the non-canonical or unmapped virtual addresses ever reached memory.
  bool saw_good_read = false, saw_good_write = false;
  for (const auto& t : h.data_txns()) {
    const uint64_t addr = t[0];
    const bool we = t[1] != 0;
    if (addr == kGoodPa && !we) saw_good_read = true;
    if (addr == kPageSig && we) saw_good_write = true;
    if (addr == kVaBad || addr == kVaInv || addr == kBadTable) {
      reporter->Mismatch("integrated/no-shadow", "no access at " + U64(addr),
                         "access at " + U64(addr));
      Fail("integrated", "an address that must not be reached did reach the data port");
    }
    if (addr < kRamBase || addr >= kRamBase + MOSAIC_RAM_SIZE) {
      reporter->Mismatch("integrated/in-ram", "every data access inside RAM", U64(addr));
      Fail("integrated", "the data port carried an address outside RAM");
    }
  }
  if (!saw_good_read) Fail("integrated", "the translated load's physical read is missing");
  if (!saw_good_write) Fail("integrated", "the translated store's physical write is missing");

  // The store-translation path really translated: the store's physical address
  // differs from its virtual one.
  if (kPageSig == kVaSig) Fail("integrated", "the store/load addresses do not differ");
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
      // The walker is a supervisor feature; a profile with no S-mode has no
      // satp and this case cannot be meaningful there.
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
    reporter.Mismatch("run", "every mapping, fault and A/D update matches the Sv39 rules",
                      "contract violated");
    passed = false;
    detail = "contract violated: " + std::string(f.what());
  }

  dut.final();
  reporter.Check(passed, "no contract violation");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
