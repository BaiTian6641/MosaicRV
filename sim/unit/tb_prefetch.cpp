// ============================================================================
// tb_prefetch.cpp -- CASE=prefetch.fault_and_pollution, work package I-063.
//
// The DUT is `mosaic_prefetch` (the bounded, switchable stride predictor) plus
// the I-060 `mosaic_llb` it is allowed to populate, wrapped by
// `mosaic_prefetch_tb`. The driver is the machine around them: it owns the page
// table, the demand stream, the platform map's read side and the memory that
// answers a prefetch, and it is an independent oracle for every value.
//
// ------------------------------------------------------------------ the case
//
// The card asks for a reuse predictor and a switchable prefetch, with the
// workload's PC/stride/reuse characteristics recorded *before* prediction is
// enabled, and a harm bound with two halves:
//
//   (a) a wrong prediction may only add latency or traffic -- never a fault,
//       never an irreversible device read, never an architectural change;
//   (b) the predictor is never the only source of correctness -- with the
//       switch off every architectural result is identical, and a cancelled
//       prefetch leaves nothing behind.
//
// ----------------------------------------------------------------- the checks
//
//   observation   the PC/stride/reuse record is taken with `en_i = 0`, and it
//                 is taken from the DUT's own counters; prediction is enabled
//                 only after the record is printed. The card's first sentence
//                 as an executable ordering.
//   gate          every egress read the prefetcher presents is to mapped,
//                 idempotent, non-device memory. A read to an unmapped address
//                 or a device is a named failure, caught where it happens.
//   useful        a correct prediction lands before the demand, and the demand
//                 then hits the locality buffer. The delivered value is
//                 compared with the driver's own memory, word for word.
//   pollution     a wrong prediction lands a line that is never demanded: the
//                 counter shows it and the architectural result is unchanged.
//   cancel        a cancelled prefetch's response must not land: the locality
//                 buffer is read out through its debug port and shows nothing.
//   permission    a faulting demand spawns no hint, and a prefetch does not
//                 populate a context the demand path would have refused.
//   alias         one physical line reached through two virtual aliases shares
//                 one copy: the fill is physical, never virtual.
//   identity      a fixed trace is replayed with the switch off and on; the
//                 delivered values and the fault set are identical, and only
//                 the memory traffic differs.
//   histogram     useful / useless / late / cancelled are printed, and the
//                 conservation `issued == useful + useless + late + cancelled
//                 + in-flight` holds.
//
// The driver's memory model is the oracle: every fill is taken from it, every
// delivered value is compared against it, and a prefetch read is classified
// against it. No check compares the DUT with itself.
// ============================================================================

#include <verilated.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "Vmosaic_prefetch_tb.h"
#include "mosaic_platform.h"
#include "sim_common.h"

namespace {

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw std::runtime_error(where + ": " + detail);
}

void Require(bool condition, const std::string& where, const std::string& detail) {
  if (!condition) Fail(where, detail);
}

std::string Dec(uint64_t v) { return std::to_string(v); }
std::string Hex(uint64_t v) { return mosaic::Hex(v); }

// ------------------------------------------------------------- geometry
constexpr uint32_t kLineBytes = 32;
constexpr uint32_t kOffsetBits = 5;
constexpr uint32_t kLanes = 8;      // 32-bit lanes in a 32-byte line
constexpr uint32_t kEntries = 8;    // LLB entries
constexpr uint32_t kTableEntries = 16;
constexpr uint32_t kDepth = 4;

// Leaf permission classes, {x, w, r, u} -- the same encoding the LLB keys on.
constexpr uint8_t kPermRW = 0x7;    // U: r+w
constexpr uint8_t kPermRO = 0x3;    // U: r
constexpr uint8_t kPermWO = 0x5;    // U: w-only (no read): a prefetch may not use it

// The platform map, taken from the generated header -- never a hand list.
constexpr uint32_t kRamBase   = static_cast<uint32_t>(MOSAIC_RAM_BASE);
constexpr uint32_t kUartBase  = static_cast<uint32_t>(MOSAIC_UART_BASE);
constexpr uint32_t kRomBase   = static_cast<uint32_t>(MOSAIC_BOOT_ROM_BASE);
constexpr uint32_t kClintBase = static_cast<uint32_t>(MOSAIC_CLINT_BASE);
constexpr bool     kRamCacheable = (MOSAIC_RAM_CACHEABLE != 0);

constexpr uint32_t kRamSize   = 2u * 1024 * 1024;
constexpr uint32_t kUartSize  = 256;
constexpr uint32_t kClintSize = 4096;
constexpr uint32_t kRomSize   = 4096;

using Lanes = std::array<uint32_t, kLanes>;

constexpr uint32_t LineOf(uint32_t pa) { return pa >> kOffsetBits; }
constexpr uint32_t BaseOf(uint32_t line) { return line << kOffsetBits; }

// One PC per independent stream. The predictor's index is `pc[5:2]`, so the
// streams must differ there; kPc(k) puts stream k in its own index.
constexpr uint64_t kPc(uint32_t k) { return 0x80000000ull + static_cast<uint64_t>(k) * 4ull; }

bool InRange(uint32_t pa, uint32_t base, uint32_t size) {
  return pa >= base && pa < base + size;
}
bool IsUart(uint32_t pa)  { return InRange(pa, kUartBase, kUartSize); }
bool IsClint(uint32_t pa) { return InRange(pa, kClintBase, kClintSize); }
bool IsRom(uint32_t pa)   { return InRange(pa, kRomBase, kRomSize); }
bool IsRam(uint32_t pa)   { return InRange(pa, kRamBase, kRamSize); }
bool IsDevice(uint32_t pa) { return IsUart(pa) || IsClint(pa); }
bool IsMapped(uint32_t pa) { return IsRom(pa) || IsDevice(pa) || IsRam(pa); }
bool IsIdempotent(uint32_t pa) { return IsRom(pa) || IsRam(pa); }

bool PermitsRead(uint8_t perms) { return (perms & 0x3u) == 0x3u; }

struct Obs {
  bool llb_hit = false;
  bool llb_bypass = false;
  Lanes llb_data{};
  bool llb_fill_ok = false;

  bool pf_valid = false;
  uint64_t pf_pa = 0;
  bool mem_req_valid = false;
  uint64_t mem_req_pa = 0;
  uint32_t mem_req_id = 0;

  bool pf_fill_valid = false;
  uint64_t pf_fill_pa = 0;
  uint32_t pf_fill_vpn = 0;
  uint32_t pf_fill_asid = 0;
  uint32_t pf_fill_perms = 0;
  Lanes pf_fill_data{};

  bool dbg_valid = false;
  uint32_t dbg_line = 0;
  uint32_t dbg_asid = 0;
  uint32_t dbg_perms = 0;
};

// A prefetch read the model memory accepted, awaiting its response.
struct PfReq {
  uint32_t id = 0;
  uint32_t pa = 0;
};

struct Read {
  bool hit = false;
  Lanes data{};
};

struct Signature {
  std::vector<uint32_t> values;
  std::vector<uint32_t> faults;
  uint64_t demand_reads = 0;
  uint64_t prefetch_reads = 0;
};

// ============================================================================
// the harness: the DUT plus the environment it is measured in
// ============================================================================
class Harness {
 public:
  Harness(Vmosaic_prefetch_tb* dut, mosaic::ClockDriver* clk, mosaic::Reporter* rep,
          uint64_t max_cycles)
      : dut_(dut), clk_(clk), rep_(rep), max_cycles_(max_cycles) {
    ResetPins();
  }

  mosaic::Reporter* Rep() { return rep_; }
  uint64_t Cycles() const { return clk_->cycle(); }

  // ------------------------------------------------------------ the pins
  void ResetPins() {
    rst_ = false;
    pf_en_ = false;
    conf_thresh_ = 1;
    dem_valid_ = false;
    dem_pc_ = 0;
    dem_pa_ = 0;
    dem_vpn_ = 0;
    dem_asid_ = 0;
    dem_perms_ = 0;
    dem_hit_ = false;
    dem_fault_ = false;
    gate_perm_ok_ = true;
    lreq_valid_ = false;
    lreq_pa_ = 0;
    lreq_vpn_ = 0;
    lreq_asid_ = 0;
    lreq_perms_ = 0;
    lreq_atomic_ = false;
    lfill_valid_ = false;
    lfill_pa_ = 0;
    lfill_vpn_ = 0;
    lfill_asid_ = 0;
    lfill_perms_ = 0;
    lfill_data_ = Lanes{};
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
    dbg_index_ = 0;
    mem_ready_ = true;
    resp_valid_ = false;
    resp_id_ = 0;
    resp_data_ = Lanes{};
    cancel_ = false;
    cancel_id_ = 0;
    flush_ = false;
    release_valid_ = false;
    release_line_ = 0;
  }

  void Apply() {
    dut_->rst = rst_ ? 1 : 0;
    dut_->pf_en = pf_en_ ? 1 : 0;
    dut_->pf_conf_thresh = conf_thresh_;
    dut_->dem_valid = dem_valid_ ? 1 : 0;
    dut_->dem_pc = dem_pc_;
    dut_->dem_pa = dem_pa_;
    dut_->dem_vpn = dem_vpn_;
    dut_->dem_asid = dem_asid_;
    dut_->dem_perms = dem_perms_;
    dut_->dem_hit = dem_hit_ ? 1 : 0;
    dut_->dem_fault = dem_fault_ ? 1 : 0;
    dut_->gate_perm_ok = gate_perm_ok_ ? 1 : 0;
    dut_->llb_req_valid = lreq_valid_ ? 1 : 0;
    dut_->llb_req_pa = lreq_pa_;
    dut_->llb_req_vpn = lreq_vpn_;
    dut_->llb_req_asid = lreq_asid_;
    dut_->llb_req_perms = lreq_perms_;
    dut_->llb_req_atomic = lreq_atomic_ ? 1 : 0;
    dut_->llb_fill_valid = lfill_valid_ ? 1 : 0;
    dut_->llb_fill_pa = lfill_pa_;
    dut_->llb_fill_vpn = lfill_vpn_;
    dut_->llb_fill_asid = lfill_asid_;
    dut_->llb_fill_perms = lfill_perms_;
    for (uint32_t i = 0; i < kLanes; i++) dut_->llb_fill_data[i] = lfill_data_[i];
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
    dut_->llb_dbg_index = dbg_index_;
    dut_->mem_req_ready = mem_ready_ ? 1 : 0;
    dut_->mem_resp_valid = resp_valid_ ? 1 : 0;
    dut_->mem_resp_id = resp_id_;
    for (uint32_t i = 0; i < kLanes; i++) dut_->mem_resp_data[i] = resp_data_[i];
    dut_->pf_cancel = cancel_ ? 1 : 0;
    dut_->pf_cancel_id = cancel_id_;
    dut_->pf_flush = flush_ ? 1 : 0;
    dut_->pf_release_valid = release_valid_ ? 1 : 0;
    dut_->pf_release_line = release_line_;
  }

  void Capture(Obs* o) {
    o->llb_hit = dut_->llb_req_hit != 0;
    o->llb_bypass = dut_->llb_req_bypass != 0;
    o->llb_fill_ok = dut_->llb_fill_ok != 0;
    for (uint32_t i = 0; i < kLanes; i++) o->llb_data[i] = dut_->llb_req_data[i];
    o->pf_valid = dut_->pf_valid != 0;
    o->pf_pa = dut_->pf_pa;
    o->mem_req_valid = dut_->mem_req_valid != 0;
    o->mem_req_pa = dut_->mem_req_pa;
    o->mem_req_id = dut_->mem_req_id;
    o->pf_fill_valid = dut_->pf_fill_valid != 0;
    o->pf_fill_pa = dut_->pf_fill_pa;
    o->pf_fill_vpn = dut_->pf_fill_vpn;
    o->pf_fill_asid = dut_->pf_fill_asid;
    o->pf_fill_perms = dut_->pf_fill_perms;
    for (uint32_t i = 0; i < kLanes; i++) o->pf_fill_data[i] = dut_->pf_fill_data[i];
    o->dbg_valid = dut_->llb_dbg_valid != 0;
    o->dbg_line = dut_->llb_dbg_line;
    o->dbg_asid = dut_->llb_dbg_asid;
    o->dbg_perms = dut_->llb_dbg_perms;
  }

  Obs Peek() {
    Apply();
    dut_->eval();
    Obs o;
    Capture(&o);
    return o;
  }

  Obs Step() {
    if (clk_->cycle() >= max_cycles_) {
      Fail("cycles", "max-cycles (" + Dec(max_cycles_) + ") exhausted");
    }
    Apply();
    dut_->eval();
    Obs o;
    Capture(&o);
    AcceptRead(o);
    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    ClearPulses();
    return o;
  }

  void ClearPulses() {
    dem_valid_ = false;
    lreq_valid_ = false;
    lfill_valid_ = false;
    inv_store_valid_ = false;
    inv_refill_valid_ = false;
    inv_snoop_valid_ = false;
    inv_snoop_all_ = false;
    fence_valid_ = false;
    ctx_valid_ = false;
    resp_valid_ = false;
    cancel_ = false;
    flush_ = false;
    release_valid_ = false;
  }

  // A prefetch read is admitted by the memory only if it is to mapped,
  // idempotent, non-device memory -- the property the harm bound rests on. A
  // read to an unmapped address or a device is the mutant the controls inject,
  // and it is named here, at the read.
  void AcceptRead(const Obs& o) {
    if (!o.mem_req_valid || !mem_ready_) return;
    const uint32_t pa = static_cast<uint32_t>(o.mem_req_pa);
    prefetch_reads_++;
    if (!IsMapped(pa)) {
      prefetch_unmapped_++;
      Fail("unmapped-prefetch", "a prefetch read was presented for unmapped pa " + Hex(pa) +
                                    " -- a prediction may not fault");
    }
    if (IsDevice(pa)) {
      prefetch_device_reads_++;
      Fail("device-read", "a prefetch read was presented for device pa " + Hex(pa) +
                              " -- a prediction may not perform an irreversible read");
    }
    if (!IsIdempotent(pa)) {
      prefetch_nonidempotent_++;
      Fail("non-idempotent-prefetch",
           "a prefetch read was presented for non-idempotent pa " + Hex(pa));
    }
    if (IsRam(pa)) prefetch_ram_reads_++;
    PfReq r;
    r.id = o.mem_req_id & 0xffu;
    r.pa = pa;
    accepted_.push_back(r);
  }

  // ------------------------------------------------------- host memory oracle
  uint64_t ReadWord(uint32_t addr) {
    auto it = mem_.find(addr);
    return it == mem_.end() ? 0 : it->second;
  }
  void WriteWord(uint32_t addr, uint64_t value) { mem_[addr] = value; }
  void WriteLine(uint32_t pa, uint64_t base) {
    for (uint32_t w = 0; w < kLanes / 2; w++) WriteWord(pa + 8 * w, base + w);
  }
  Lanes MemLine(uint32_t pa) {
    Lanes out{};
    for (uint32_t w = 0; w < kLanes / 2; w++) {
      const uint64_t v = ReadWord(pa + 8 * w);
      out[2 * w] = static_cast<uint32_t>(v & 0xffffffffull);
      out[2 * w + 1] = static_cast<uint32_t>(v >> 32);
    }
    return out;
  }

  // --------------------------------------------------------------- the reset
  void ResetDut() {
    ResetPins();
    rst_ = true;
    for (int i = 0; i < 4; i++) {
      dut_->rst = 1;
      Apply();
      dut_->eval();
      dut_->clk = 1;
      dut_->eval();
      clk_->Tick();
      dut_->clk = 0;
      dut_->eval();
    }
    rst_ = false;
    Apply();
    dut_->eval();
    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
    accepted_.clear();
  }

  // --------------------------------------------------------------- LLB probes
  bool LlbHolds(uint32_t pa, uint16_t asid, uint8_t perms) {
    const uint32_t line = LineOf(pa) & 0x7ffffffu;
    for (uint32_t i = 0; i < kEntries; i++) {
      dbg_index_ = static_cast<uint8_t>(i);
      Obs o = Peek();
      if (o.dbg_valid && o.dbg_line == line && o.dbg_asid == asid && o.dbg_perms == perms) {
        dbg_index_ = 0;
        return true;
      }
    }
    dbg_index_ = 0;
    return false;
  }

  uint32_t LlbCount() { return dut_->llb_count; }

  uint32_t TableEntries() { return dut_->o_table_entries; }
  uint32_t Depth() { return dut_->o_depth; }
  uint32_t LlbEntries() { return dut_->llb_entries; }
  uint32_t LlbLineBytes() { return dut_->llb_line_bytes; }
  bool GeometryReadback() {
    return TableEntries() == kTableEntries && Depth() == kDepth && LlbEntries() == kEntries &&
           LlbLineBytes() == kLineBytes;
  }

  // ------------------------------------------------------------ the demand
  void Lookup(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms, bool atomic) {
    lreq_valid_ = true;
    lreq_pa_ = pa;
    lreq_vpn_ = vpn & 0x7ffffffu;
    lreq_asid_ = asid;
    lreq_perms_ = perms;
    lreq_atomic_ = atomic;
  }

  Obs PeekLookup(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms) {
    Lookup(pa, vpn, asid, perms, false);
    Obs o = Peek();
    lreq_valid_ = false;
    return o;
  }

  void SetFill(uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms, const Lanes& data) {
    lfill_valid_ = true;
    lfill_pa_ = pa;
    lfill_vpn_ = vpn & 0x7ffffffu;
    lfill_asid_ = asid;
    lfill_perms_ = perms;
    lfill_data_ = data;
  }

  // One demand access, with a refill from the driver's memory on a miss. The
  // delivered line is the architectural value: the locality buffer's copy on a
  // hit, the driver's memory on a miss. A hit whose copy differs from memory is
  // an architectural failure and is named.
  Read Demand(uint64_t pc, uint32_t pa, uint32_t vpn, uint16_t asid, uint8_t perms,
              bool fault, bool refill) {
    Lookup(pa, vpn, asid, perms, false);
    dem_valid_ = false;
    Obs p = Peek();
    Read out;
    out.hit = p.llb_hit;
    const Lanes mem = MemLine(pa);
    if (out.hit) {
      out.data = p.llb_data;
      if (out.data != mem) {
        Fail("architectural-hit",
             "a demand hit a copy that differs from memory at pa " + Hex(pa));
      }
    } else {
      out.data = mem;
      if (!fault) {
        demand_reads_++;
        if (IsDevice(pa)) demand_device_reads_++;
        if (refill) SetFill(pa, vpn, asid, perms, mem);
      }
    }
    dem_valid_ = true;
    dem_pc_ = pc;
    dem_pa_ = pa;
    dem_vpn_ = vpn & 0x7ffffffu;
    dem_asid_ = asid;
    dem_perms_ = perms;
    dem_hit_ = out.hit;
    dem_fault_ = fault;
    gate_perm_ok_ = !fault && PermitsRead(perms);
    Step();
    return out;
  }

  // ------------------------------------------------------ prefetch lifecycle
  // Delivers the response for an accepted prefetch read. The prefetcher's fill
  // is routed to the LLB fill port here, which is the wrapper's decision.
  bool DeliverResponse(uint32_t id, uint32_t pa) {
    resp_valid_ = true;
    resp_id_ = static_cast<uint8_t>(id);
    resp_data_ = MemLine(pa);
    Obs p = Peek();
    const bool presented_fill = p.pf_fill_valid;
    if (presented_fill) {
      SetFill(static_cast<uint32_t>(p.pf_fill_pa), p.pf_fill_vpn, p.pf_fill_asid,
              p.pf_fill_perms, p.pf_fill_data);
    }
    Obs o = Step();
    resp_valid_ = false;
    lfill_valid_ = false;
    return presented_fill && o.llb_fill_ok;
  }

  // Deliver every accepted read, then release the lines they targeted: a
  // prefetch whose window closes with no demand is useless, and the slots must
  // be freed or later phases find the table full.
  void Settle() {
    for (PfReq& p : accepted_) {
      DeliverResponse(p.id, p.pa);
      last_lines_.push_back(LineOf(p.pa));
    }
    accepted_.clear();
    for (uint32_t line : last_lines_) Release(line);
    last_lines_.clear();
  }

  void DrainAccepted() {
    for (PfReq& p : accepted_) DeliverResponse(p.id, p.pa);
    accepted_.clear();
  }

  void Cancel(uint32_t id) {
    cancel_ = true;
    cancel_id_ = static_cast<uint8_t>(id);
    Step();
  }

  void Release(uint32_t line) {
    release_valid_ = true;
    release_line_ = BaseOf(line);
    Step();
  }

  void Flush() {
    flush_ = true;
    Step();
  }

  // -------------------------------------------------- counters and accessors
  uint32_t ObsAccesses() { return dut_->o_obs_accesses; }
  uint32_t ObsNewPc() { return dut_->o_obs_new_pc; }
  uint32_t ObsRepeatPc() { return dut_->o_obs_repeat_pc; }
  uint32_t ObsStrideMatch() { return dut_->o_obs_stride_match; }
  uint32_t ObsStrideMismatch() { return dut_->o_obs_stride_mismatch; }
  uint32_t ObsStrideZero() { return dut_->o_obs_stride_zero; }
  uint32_t ObsReuseHit() { return dut_->o_obs_reuse_hit; }
  uint32_t Issued() { return dut_->o_issued; }
  uint32_t Useful() { return dut_->o_useful; }
  uint32_t Useless() { return dut_->o_useless; }
  uint32_t Late() { return dut_->o_late; }
  uint32_t Cancelled() { return dut_->o_cancelled; }
  uint32_t Admitted() { return dut_->o_admitted; }
  uint32_t GateRefuse() { return dut_->o_gate_refuse; }
  uint32_t FullStall() { return dut_->o_full_stall; }
  uint32_t FillCtr() { return dut_->o_fill_ctr; }
  uint32_t FillRefused() { return dut_->o_fill_refused_ctr; }
  uint32_t Dropped() { return dut_->o_dropped_ctr; }
  uint32_t Inflight() { return dut_->o_inflight; }

  uint64_t PrefetchReads() const { return prefetch_reads_; }
  uint64_t PrefetchRamReads() const { return prefetch_ram_reads_; }
  uint64_t PrefetchDeviceReads() const { return prefetch_device_reads_; }
  uint64_t PrefetchUnmapped() const { return prefetch_unmapped_; }
  uint64_t DemandReads() const { return demand_reads_; }
  uint64_t DemandDeviceReads() const { return demand_device_reads_; }

  std::vector<PfReq>& Accepted() { return accepted_; }

  void SetEnable(bool en) { pf_en_ = en; }
  void SetThresh(uint8_t t) { conf_thresh_ = t; }

  bool Conserved() {
    return static_cast<uint64_t>(Issued()) == static_cast<uint64_t>(Useful()) + Useless() +
                                                 Late() + Cancelled() + Inflight();
  }

 private:
  Vmosaic_prefetch_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* rep_;
  uint64_t max_cycles_;

  bool rst_ = false;
  bool pf_en_ = false;
  uint8_t conf_thresh_ = 1;

  bool dem_valid_ = false;
  uint64_t dem_pc_ = 0;
  uint64_t dem_pa_ = 0;
  uint32_t dem_vpn_ = 0;
  uint32_t dem_asid_ = 0;
  uint8_t dem_perms_ = 0;
  bool dem_hit_ = false;
  bool dem_fault_ = false;
  bool gate_perm_ok_ = true;

  bool lreq_valid_ = false;
  uint64_t lreq_pa_ = 0;
  uint32_t lreq_vpn_ = 0;
  uint32_t lreq_asid_ = 0;
  uint8_t lreq_perms_ = 0;
  bool lreq_atomic_ = false;

  bool lfill_valid_ = false;
  uint64_t lfill_pa_ = 0;
  uint32_t lfill_vpn_ = 0;
  uint32_t lfill_asid_ = 0;
  uint8_t lfill_perms_ = 0;
  Lanes lfill_data_{};

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
  uint32_t fence_asid_ = 0;
  bool fence_has_asid_ = false;
  bool ctx_valid_ = false;
  uint8_t dbg_index_ = 0;

  bool mem_ready_ = true;
  bool resp_valid_ = false;
  uint32_t resp_id_ = 0;
  Lanes resp_data_{};

  bool cancel_ = false;
  uint32_t cancel_id_ = 0;
  bool flush_ = false;
  bool release_valid_ = false;
  uint64_t release_line_ = 0;

  std::map<uint32_t, uint64_t> mem_;
  std::vector<PfReq> accepted_;
  std::vector<uint32_t> last_lines_;

  uint64_t prefetch_reads_ = 0;
  uint64_t prefetch_ram_reads_ = 0;
  uint64_t prefetch_device_reads_ = 0;
  uint64_t prefetch_unmapped_ = 0;
  uint64_t prefetch_nonidempotent_ = 0;
  uint64_t demand_reads_ = 0;
  uint64_t demand_device_reads_ = 0;
};

// ============================================================================
// the phases
// ============================================================================

constexpr uint16_t kAsid = 7;

// ---- 0. geometry ----------------------------------------------------------
void PhaseGeometry(Harness* h, mosaic::Reporter* rep) {
  Require(h->ObsAccesses() == 0, "geometry", "observation counters are not clear after reset");
  Require(h->Useful() == 0 && h->Useless() == 0 && h->Issued() == 0, "geometry",
          "the histogram is not clear after reset");
  Require(h->GeometryReadback(), "geometry",
          "the elaborated geometry is not the one this driver was written for");
  rep->Check(true, "geometry: table=" + Dec(h->TableEntries()) + " depth=" + Dec(h->Depth()) +
                       " llb=" + Dec(h->LlbEntries()) + "x" + Dec(h->LlbLineBytes()) + "B");
}

// ---- 1. the characteristics record, with prediction OFF -------------------
// The card's first sentence: record PC/stride/reuse *before* enabling. The
// record is the DUT's own observation counters, taken with `en_i = 0`.
void PhaseCharacteristics(Harness* h, mosaic::Reporter* rep, uint32_t base_line) {
  h->SetEnable(false);
  h->SetThresh(1);
  const uint32_t before_accesses = h->ObsAccesses();

  // (a) a strict +1 line stride at one PC, ten accesses.
  for (int i = 0; i < 10; i++) {
    h->Demand(kPc(0), BaseOf(base_line + i), 0x11, kAsid, kPermRW, false, true);
  }
  // (b) a repeat of one line at a second PC: stride zero.
  for (int i = 0; i < 4; i++) {
    h->Demand(kPc(1), BaseOf(base_line), 0x11, kAsid, kPermRW, false, true);
  }
  // (c) an irregular stream at a third PC: strides that do not repeat.
  const uint32_t jump[4] = {0, 3, 1, 7};
  for (int i = 0; i < 4; i++) {
    h->Demand(kPc(2), BaseOf(base_line + jump[i]), 0x11, kAsid, kPermRW, false, true);
  }
  h->DrainAccepted();

  const uint32_t accesses = h->ObsAccesses() - before_accesses;
  Require(accesses == 18, "characteristics",
          "18 demands were presented, the DUT counted " + Dec(accesses));
  Require(h->ObsNewPc() >= 3, "characteristics", "the three distinct PCs were not all new");
  Require(h->ObsRepeatPc() >= 14, "characteristics",
          "the repeat count missed the repeats of the same PCs");
  Require(h->ObsStrideMatch() >= 8, "characteristics",
          "the +1 stride repeat was not observed as a stride match");
  Require(h->ObsStrideZero() >= 3, "characteristics",
          "the same-line repeats were not observed as stride zero");
  Require(h->ObsStrideMismatch() >= 1, "characteristics",
          "the irregular stream produced no stride mismatch");
  Require(h->Issued() == 0, "characteristics",
          "prediction was off, yet a prefetch was issued -- the record must come first");

  std::printf("  [record] en=0 accesses=%u new_pc=%u repeat_pc=%u stride_match=%u "
              "stride_mismatch=%u stride_zero=%u reuse_hit=%u issued=%u\n",
              accesses, h->ObsNewPc(), h->ObsRepeatPc(), h->ObsStrideMatch(),
              h->ObsStrideMismatch(), h->ObsStrideZero(), h->ObsReuseHit(), h->Issued());
  rep->Check(true, "characteristics: with prediction off the DUT recorded " + Dec(accesses) +
                       " demands, " + Dec(h->ObsNewPc()) + " new PCs, " +
                       Dec(h->ObsStrideMatch()) + " stride matches, " +
                       Dec(h->ObsStrideMismatch()) + " mismatches, " +
                       Dec(h->ObsStrideZero()) + " zero strides, " +
                       Dec(h->ObsReuseHit()) + " reuse hits");
  if (kRamCacheable) {
    Require(h->ObsReuseHit() >= 3, "characteristics",
            "a cacheable profile must show reuse hits from the demand refills");
  }
}

// ---- 2. a correct prediction is useful ------------------------------------
void PhaseUseful(Harness* h, mosaic::Reporter* rep, uint32_t base_line) {
  h->SetEnable(true);
  h->SetThresh(1);
  const uint32_t useful_before = h->Useful();
  uint32_t delivered_ok = 0;

  // Five accesses of a +1 stream: the third predicts the fourth, and the
  // prefetch is answered before the demand for the fourth arrives.
  for (int i = 0; i < 5; i++) {
    h->Demand(kPc(3), BaseOf(base_line + i), 0x11, kAsid, kPermRW, false, true);
    for (PfReq& p : h->Accepted()) {
      if (h->DeliverResponse(p.id, p.pa)) delivered_ok++;
    }
    h->Accepted().clear();
  }
  h->Settle();

  const uint32_t useful_events = h->Useful() - useful_before;
  if (kRamCacheable) {
    Require(useful_events >= 1, "useful",
            "a correct prediction was answered before the demand, yet no demand hit it");
    Require(delivered_ok >= 1, "useful", "no prefetch fill was accepted by the locality buffer");
  } else {
    // The map declares RAM non-cacheable, so the buffer refuses every fill and
    // a prefetch can only add traffic -- the honest result for this profile.
    Require(h->PrefetchRamReads() > 0, "useful",
            "a correct prediction on normal memory must still issue its read");
    Require(useful_events == 0, "useful", "a non-cacheable profile cannot produce a useful hit");
  }
  std::printf("  [useful] profile_cacheable=%d prefetch_ram_reads=%llu fills_ok=%u "
              "fill_refused=%u useful=%u\n",
              kRamCacheable ? 1 : 0,
              static_cast<unsigned long long>(h->PrefetchRamReads()), delivered_ok,
              h->FillRefused(), h->Useful());
  rep->Check(true, "useful: a correct prediction was delivered before the demand (" +
                       Dec(useful_events) + " useful)");
}

// ---- 3. a wrong prediction only pollutes ----------------------------------
void PhasePollution(Harness* h, mosaic::Reporter* rep, uint32_t base_line) {
  h->SetEnable(true);
  const uint32_t useless_before = h->Useless();

  // A +1 stream for three accesses predicts line base+0x43; the stream then
  // jumps away and the predicted line is never demanded.
  for (int i = 0; i < 3; i++) {
    h->Demand(kPc(4), BaseOf(base_line + 0x40 + i), 0x22, kAsid, kPermRW, false, true);
  }
  uint32_t predicted_line = 0;
  for (PfReq& p : h->Accepted()) {
    h->DeliverResponse(p.id, p.pa);
    predicted_line = LineOf(p.pa);
  }
  h->Accepted().clear();
  Require(predicted_line == base_line + 0x43, "pollution",
          "the third access predicted line " + Dec(predicted_line) + ", expected base+0x43");
  h->Demand(kPc(4), BaseOf(base_line + 0x50), 0x22, kAsid, kPermRW, false, true);
  h->DrainAccepted();
  h->Release(predicted_line);   // the window closes with no demand

  const uint32_t useless_events = h->Useless() - useless_before;
  Require(useless_events >= 1, "pollution",
          "a prefetch whose line was never demanded was not reported useless");
  rep->Check(true, "pollution: a wrong prediction landed line " + Dec(predicted_line) +
                       " and was reported useless (" + Dec(useless_events) + ")");
}

// ---- 4. a prefetch into unmapped memory -----------------------------------
void PhaseUnmapped(Harness* h, mosaic::Reporter* rep) {
  h->SetEnable(true);
  const uint32_t refuse_before = h->GateRefuse();
  const uint64_t reads_before = h->PrefetchReads();

  // The last two lines of RAM in a +1 stream predict the first line past RAM.
  const uint32_t last = LineOf(kRamBase + kRamSize) - 1;
  for (int i = 0; i < 3; i++) {
    h->Demand(kPc(5), BaseOf(last - 2 + i), 0x33, kAsid, kPermRW, false, true);
  }
  Require(h->PrefetchReads() == reads_before, "unmapped-prefetch",
          "a read was counted for an unmapped prediction");
  Require(h->GateRefuse() > refuse_before, "unmapped-prefetch", "the refusal was not counted");
  Require(h->Accepted().empty(), "unmapped-prefetch", "an unmapped prediction was admitted");
  h->Accepted().clear();
  rep->Check(true, "unmapped-prefetch: a prediction past the end of RAM was refused, not "
                   "issued (" + Dec(h->GateRefuse() - refuse_before) + " refusals)");
}

// ---- 5. a prefetch into a device ------------------------------------------
void PhaseDevice(Harness* h, mosaic::Reporter* rep) {
  h->SetEnable(true);
  const uint32_t refuse_before = h->GateRefuse();
  const uint64_t dev_before = h->PrefetchDeviceReads();
  const uint64_t dem_dev_before = h->DemandDeviceReads();

  // The firmware polls the UART; the demand reads are real and expected. The
  // predictor's read of the next UART line would be an irreversible read.
  const uint32_t uart_line = LineOf(kUartBase);
  for (int i = 0; i < 3; i++) {
    h->Demand(kPc(6), BaseOf(uart_line + i), 0x44, kAsid, kPermRW, false, true);
  }
  Require(h->PrefetchDeviceReads() == dev_before, "device-read",
          "a device read was counted for a prediction");
  Require(h->DemandDeviceReads() > dem_dev_before, "device-read",
          "the poll's own device reads did not stay on the demand path");
  Require(h->GateRefuse() > refuse_before, "device-read", "the device refusal was not counted");
  Require(h->Accepted().empty(), "device-read", "a device prediction was admitted");
  h->Accepted().clear();
  rep->Check(true, "device-read: the predictor refused the UART line; the poll's own device "
                   "reads stayed on the demand path");
}

// ---- 6. a faulting demand spawns no hint ----------------------------------
void PhaseFaultingDemand(Harness* h, mosaic::Reporter* rep, uint32_t base_line) {
  h->SetEnable(true);
  const uint32_t l0 = base_line + 0x100;

  // Positive control: the third access is a normal demand, and it predicts.
  h->Demand(kPc(7), BaseOf(l0 + 0), 0x55, kAsid, kPermRW, false, true);
  h->Demand(kPc(7), BaseOf(l0 + 1), 0x55, kAsid, kPermRW, false, true);
  h->Demand(kPc(7), BaseOf(l0 + 2), 0x55, kAsid, kPermRW, false, true);
  Require(!h->Accepted().empty(), "faulting-demand",
          "the positive control produced no prediction");
  h->DrainAccepted();

  // The same shape, but the third access faults (a store to a read-only page).
  h->Demand(kPc(8), BaseOf(l0 + 4), 0x55, kAsid, kPermRW, false, true);
  h->Demand(kPc(8), BaseOf(l0 + 5), 0x55, kAsid, kPermRW, false, true);
  const uint64_t reads_before = h->PrefetchReads();
  h->Demand(kPc(8), BaseOf(l0 + 6), 0x55, kAsid, kPermRO, /*fault=*/true, false);
  Require(h->PrefetchReads() == reads_before, "faulting-demand",
          "a faulting demand spawned a prefetch -- the prediction bypassed the permission "
          "check");
  Require(h->Accepted().empty(), "faulting-demand",
          "a faulting demand was admitted as a prediction");
  h->DrainAccepted();
  rep->Check(true, "faulting-demand: a faulting demand trained nothing and produced no hint");
}

// ---- 7. a cancelled prefetch leaves nothing -------------------------------
void PhaseCancel(Harness* h, mosaic::Reporter* rep, uint32_t base_line) {
  h->SetEnable(true);
  const uint32_t cancelled_before = h->Cancelled();
  const uint32_t dropped_before = h->Dropped();
  const uint32_t filled_before = h->FillCtr();
  const uint32_t l0 = base_line + 0x200;

  for (int i = 0; i < 3; i++) {
    h->Demand(kPc(9), BaseOf(l0 + i), 0x66, kAsid, kPermRW, false, true);
  }
  Require(!h->Accepted().empty(), "cancel", "the stream produced no prediction");
  PfReq p = h->Accepted().front();
  h->Accepted().clear();
  const uint32_t predicted_line = LineOf(p.pa);

  // Cancel before the response, then deliver it: the data must not land.
  h->Cancel(p.id);
  const bool landed = h->DeliverResponse(p.id, p.pa);

  Require(h->Cancelled() > cancelled_before, "cancel", "the cancellation was not reported");
  Require(h->Dropped() > dropped_before, "cancel", "the cancelled response was not dropped");
  Require(h->FillCtr() == filled_before, "cancel", "a cancelled prefetch installed a fill");
  Require(!landed, "cancel", "a cancelled prefetch's data landed in the locality buffer");
  Require(!h->LlbHolds(p.pa, kAsid, kPermRW), "cancel",
          "the cancelled line is resident in the locality buffer");
  std::printf("  [cancel] line=%u landed=0 cancelled=%u dropped=%u\n", predicted_line,
              h->Cancelled(), h->Dropped());
  rep->Check(true, "cancel: a cancelled prefetch left the locality buffer unchanged");
}

// ---- 8. the alias and permission rules ------------------------------------
void PhaseAliasAndPermission(Harness* h, mosaic::Reporter* rep, uint32_t base_line) {
  h->SetEnable(true);
  const uint32_t l0 = base_line + 0x300;
  const uint32_t vpn_a = 0x100;
  const uint32_t vpn_b = 0x200;   // a second virtual alias of the same PA

  for (int i = 0; i < 3; i++) {
    h->Demand(kPc(10), BaseOf(l0 + i), vpn_a, kAsid, kPermRW, false, true);
  }
  Require(!h->Accepted().empty(), "alias", "the stream produced no prediction");
  PfReq p = h->Accepted().front();
  h->Accepted().clear();
  h->DeliverResponse(p.id, p.pa);

  if (kRamCacheable) {
    Obs alias = h->PeekLookup(p.pa, vpn_b, kAsid, kPermRW);
    Require(alias.llb_hit, "alias",
            "a prefetch fill through one alias was not visible through the second alias");
    Obs ro = h->PeekLookup(p.pa, vpn_b, kAsid, kPermRO);
    Require(!ro.llb_hit, "permission",
            "a prefetch populated a context the demand path would not have reused");
    Obs wo = h->PeekLookup(p.pa, vpn_b, kAsid, kPermWO);
    Require(!wo.llb_hit, "permission", "a prefetch populated a write-only context");
    rep->Check(true, "alias: one physical line, two aliases, one copy; a different permission "
                     "context does not reuse it");
  } else {
    Require(!h->LlbHolds(p.pa, kAsid, kPermRW), "alias",
            "a non-cacheable profile must not hold the line");
    rep->Check(true, "alias: the map refuses the fill, so no alias or context can reuse it");
  }
  h->Release(LineOf(p.pa));
}

// ---- 9. a late demand is reported late ------------------------------------
void PhaseLate(Harness* h, mosaic::Reporter* rep, uint32_t base_line) {
  h->SetEnable(true);
  const uint32_t late_before = h->Late();
  const uint32_t l0 = base_line + 0x400;

  for (int i = 0; i < 3; i++) {
    h->Demand(kPc(11), BaseOf(l0 + i), 0x88, kAsid, kPermRW, false, true);
  }
  Require(!h->Accepted().empty(), "late", "the stream produced no prediction");
  PfReq p = h->Accepted().front();
  h->Accepted().clear();
  // The demand for the predicted line arrives before its response.
  h->Demand(kPc(11), BaseOf(l0 + 3), 0x88, kAsid, kPermRW, false, true);
  h->DeliverResponse(p.id, p.pa);
  Require(h->Late() > late_before, "late",
          "a demand that arrived before the prefetch landed was not reported late");
  rep->Check(true, "late: the demand for line " + Dec(LineOf(p.pa)) +
                       " arrived before its prefetch and was reported late");
}

// ---- 10. the architectural identity with prediction off and on ------------
Signature RunIdentityTrace(Harness* h, bool enable, uint32_t base_line) {
  h->SetEnable(enable);
  Signature s;
  const uint32_t lines[12] = {0, 1, 2, 3, 7, 8, 9, 2, 3, 4, 5, 6};
  const uint64_t reads0 = h->DemandReads();
  const uint64_t pf0 = h->PrefetchReads();
  for (int i = 0; i < 12; i++) {
    const uint32_t pa = BaseOf(base_line + lines[i]);
    const uint64_t pc = kPc(12 + static_cast<uint32_t>(i / 4));
    Read r = h->Demand(pc, pa, 0x77, kAsid, kPermRW, false, true);
    s.values.push_back(r.data[0]);
    s.faults.push_back(0);
    // Answer any prefetch promptly, so the on run can hit.
    for (PfReq& p : h->Accepted()) h->DeliverResponse(p.id, p.pa);
    h->Accepted().clear();
  }
  h->Settle();
  s.demand_reads = h->DemandReads() - reads0;
  s.prefetch_reads = h->PrefetchReads() - pf0;
  return s;
}

void PhaseIdentity(Harness* h, mosaic::Reporter* rep, uint32_t base_line) {
  Signature off = RunIdentityTrace(h, false, base_line);
  Require(off.prefetch_reads == 0, "identity",
          "prediction was off, yet a prefetch read was issued");
  Signature on = RunIdentityTrace(h, true, base_line);

  Require(off.values == on.values, "identity",
          "the delivered values differ with prediction on and off -- a prediction changed an "
          "architectural result");
  Require(off.faults == on.faults, "identity",
          "the fault set differs with prediction on and off");
  std::printf("  [identity] demands=%zu values_equal=1 faults_equal=1 demand_reads_off=%llu "
              "demand_reads_on=%llu prefetch_reads_on=%llu\n",
              off.values.size(), static_cast<unsigned long long>(off.demand_reads),
              static_cast<unsigned long long>(on.demand_reads),
              static_cast<unsigned long long>(on.prefetch_reads));
  rep->Check(true, "identity: " + Dec(off.values.size()) +
                       " demands delivered identical values with prediction off and on; only "
                       "the traffic differs (" + Dec(off.demand_reads) + " -> " +
                       Dec(on.demand_reads) + " demand reads)");
}

// ---- 11. the histogram ----------------------------------------------------
void PhaseHistogram(Harness* h, mosaic::Reporter* rep) {
  Require(h->Conserved(), "histogram",
          "issued != useful + useless + late + cancelled + in-flight: " + Dec(h->Issued()) +
              " != " + Dec(h->Useful()) + " + " + Dec(h->Useless()) + " + " + Dec(h->Late()) +
              " + " + Dec(h->Cancelled()) + " + " + Dec(h->Inflight()));
  Require(h->Issued() >= 4, "histogram", "too few prefetches were issued to report a histogram");
  std::printf("  [hist] issued=%u useful=%u useless=%u late=%u cancelled=%u inflight=%u "
              "admitted=%u gate_refuse=%u full_stall=%u fill=%u fill_refused=%u dropped=%u\n",
              h->Issued(), h->Useful(), h->Useless(), h->Late(), h->Cancelled(), h->Inflight(),
              h->Admitted(), h->GateRefuse(), h->FullStall(), h->FillCtr(), h->FillRefused(),
              h->Dropped());
  rep->Check(true, "histogram: issued=" + Dec(h->Issued()) + " useful=" + Dec(h->Useful()) +
                       " useless=" + Dec(h->Useless()) + " late=" + Dec(h->Late()) +
                       " cancelled=" + Dec(h->Cancelled()) + " in-flight=" + Dec(h->Inflight()) +
                       " conserves");
}

}  // namespace

// ============================================================================
// main
// ============================================================================
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
  Vmosaic_prefetch_tb dut;
  dut.clk = 0;
  dut.rst = 0;
  dut.eval();

  bool passed = true;
  std::string detail;
  try {
    Harness h(&dut, &clk, &reporter, options.max_cycles);
    h.ResetDut();

    // A small RAM window the whole case works in.
    const uint32_t base_line = LineOf(kRamBase) + 0x100;
    for (uint32_t i = 0; i < 0x800; i++) {
      h.WriteLine(BaseOf(base_line + i), 0x1000000ull + i * 0x10ull);
    }

    reporter.Check(true, "profile: RAM cacheable = " + Dec(kRamCacheable ? 1 : 0));

    PhaseGeometry(&h, &reporter);
    PhaseCharacteristics(&h, &reporter, base_line);   // record BEFORE enabling
    PhaseUseful(&h, &reporter, base_line);
    PhasePollution(&h, &reporter, base_line);
    PhaseUnmapped(&h, &reporter);
    PhaseDevice(&h, &reporter);
    PhaseFaultingDemand(&h, &reporter, base_line);
    PhaseCancel(&h, &reporter, base_line);
    PhaseAliasAndPermission(&h, &reporter, base_line);
    PhaseLate(&h, &reporter, base_line);
    PhaseIdentity(&h, &reporter, base_line);
    PhaseHistogram(&h, &reporter);

    detail = "prefetch harm bound holds: " + Dec(h.Issued()) + " prefetches (useful " +
             Dec(h.Useful()) + ", useless " + Dec(h.Useless()) + ", late " + Dec(h.Late()) +
             ", cancelled " + Dec(h.Cancelled()) + "), " + Dec(h.DemandReads()) +
             " demand reads, over " + Dec(h.Cycles()) + " cycles";
  } catch (const std::exception& f) {
    passed = false;
    detail = "first failure: " + std::string(f.what());
    reporter.Mismatch("run", "a wrong prediction adds only latency or traffic",
                      std::string(f.what()));
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed && reporter.failures() == 0 ? "PASS" : "FAIL", detail);
}
