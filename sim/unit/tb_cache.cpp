// ============================================================================
// tb_cache.cpp -- CASE=cache.refill_evict_fault, work package I-042.
//
// The DUT is never its own oracle. Every CPU access is run twice:
//
//   * through a `mosaic_cache` instance in the DUT (cache-on), and
//   * through an independent C++ model of the *contract* (cache-off): a flat
//     byte array addressed directly, with the same line-level fault policy.
//
// The two results -- fault flag and, for reads, the returned word -- must match
// for every access. That is the "cache-on vs cache-off architectural trace"
// equivalence the card asks for, established op by op rather than only at the
// end. The DUT's dirty lines are its own until it evicts them; the reference
// writes its stores straight through, so after the final flush the two backing
// arrays must be byte-identical too.
//
// The reference is the *contract*, not the RTL: it knows nothing about tags,
// sets, hits or misses. It knows "a read returns the memory word unless this
// line's first touch faults" and "a write merges the selected bytes". A test
// whose oracle re-implemented the cache would agree with a wrong cache.
//
// Specific cases the card names, each a phase below:
//
//   refill          a cold line is fetched; the second access hits
//   conflict        two addresses in the same set evict each other
//   dirty-eviction  a dirty victim is written back and its bytes survive
//   self-modify     a store is read back through the same cache, including
//                   partial-byte stores and different words of one line
//   refill-fault    a failed refill is NOT marked valid and is answered with a
//                   fault, and the next access retries the refill
//   icache          the instruction cache reads, and a store to it faults
//   backpressure    a stalled memory port does not change the trace
//   valid-init      reset clears the valid bits; a stale line is not served
//
// Traceability is asserted, not just observed: every accepted memory read must
// end in exactly one ev_refill or ev_fault; every accepted memory write must be
// exactly one ev_writeback; every miss must end in a refill or a fault; and the
// program must actually exercise hit, miss, refill, writeback and fault at
// least once. A rule no stimulus reaches would fail here rather than be skipped.
//
// Mutation hooks: the shipping build passes. Each `-DMOSAIC_CACHE_MUTANT_*`
// build must fail on the named check. See results/reports/I-042-cache.md.
// ============================================================================

#include <verilated.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_cache_tb.h"

namespace {

// Geometry mirrored by sim/tb/mosaic_cache_tb.sv. The wrapper fails to
// elaborate if the two disagree, so these constants cannot silently drift.
constexpr int      kLineBytes = 32;
constexpr int      kSets      = 8;
constexpr int      kIndexBits = 3;
constexpr int      kOffsetBits = 5;
constexpr int      kCpuBytes  = 8;
constexpr uint32_t kLineMask  = kLineBytes - 1;
constexpr size_t   kMemBytes  = 1u << 16;
constexpr int      kLineWords = kLineBytes / 4;  // VlWide elements for 256 bits

static_assert((1 << kIndexBits) == kSets, "index width must match set count");
static_assert((1 << kOffsetBits) == kLineBytes, "offset width must match line");

// Thrown on the first failed check. The run stops there so the report shows one
// defect rather than a thousand consequences of it.
struct Failure {
  std::string what;
};

// ------------------------------------------------------------- little-endian
uint64_t ReadWordLE(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < kCpuBytes; ++i) {
    value |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return value;
}

void WriteWordLE(uint8_t* p, uint64_t value) {
  for (int i = 0; i < kCpuBytes; ++i) {
    p[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

// Deterministic, non-zero initial memory so a mutant that zeroes a line is
// always observable rather than accidentally correct.
uint8_t PatternByte(uint32_t addr) {
  return static_cast<uint8_t>((addr * 131u) ^ 0x5au ^ (addr >> 8));
}

void FillPattern(std::vector<uint8_t>* mem) {
  for (uint32_t i = 0; i < mem->size(); ++i) {
    (*mem)[i] = PatternByte(i);
  }
}

// A physical address assembled from a tag, set and word index. `tag` is the
// part above the index, so a different tag with the same set is a conflict.
uint32_t Addr(uint32_t tag, uint32_t set, uint32_t word) {
  return 0x1000u + (tag << (kIndexBits + kOffsetBits)) + (set << kOffsetBits) +
         (word * kCpuBytes);
}

uint32_t LineOf(uint32_t addr) { return addr & ~kLineMask; }
int SetOf(uint32_t addr) { return static_cast<int>((addr >> kOffsetBits) & (kSets - 1)); }

// ----------------------------------------------------------------- counters
struct Counters {
  uint64_t hit = 0, miss = 0, refill = 0, writeback = 0, fault = 0;
};

// The DUT's CPU-visible answer to one access.
struct Result {
  bool     fault = false;
  uint64_t rdata = 0;
};

// Unpack a 256-bit Verilator wide port into little-endian bytes.
void ExtractLine(const VlWide<kLineWords>& src, uint8_t* out) {
  for (int w = 0; w < kLineWords; ++w) {
    const uint32_t word = src[w];
    for (int b = 0; b < 4; ++b) {
      out[w * 4 + b] = static_cast<uint8_t>(word >> (8 * b));
    }
  }
}

// Pack little-endian bytes into a 256-bit Verilator wide port.
void AssignLine(VlWide<kLineWords>* dst, const uint8_t* data) {
  for (int w = 0; w < kLineWords; ++w) {
    uint32_t word = 0;
    for (int b = 0; b < 4; ++b) {
      word |= static_cast<uint32_t>(data[w * 4 + b]) << (8 * b);
    }
    (*dst)[w] = word;
  }
}

// The two campaigns share one Verilator top (`mosaic_cache_tb`) that now holds
// three instances: the two blocking caches (I-042) and the non-blocking read L1
// (I-043). Each campaign drives its own half and holds the other half quiet, so
// `--x-initial unique` cannot feed an idle instance random traffic.
void QuietBlockingCaches(Vmosaic_cache_tb* dut) {
  dut->dc_req_valid = 0; dut->dc_req_we = 0; dut->dc_req_addr = 0;
  dut->dc_req_wdata = 0; dut->dc_req_wmask = 0;
  dut->ic_req_valid = 0; dut->ic_req_we = 0; dut->ic_req_addr = 0;
  dut->ic_req_wdata = 0; dut->ic_req_wmask = 0;
  dut->dfl_valid = 0; dut->ifl_valid = 0;
  dut->ddbg_index = 0; dut->idbg_index = 0;
  dut->dm_req_ready = 0; dut->dm_resp_valid = 0; dut->dm_resp_fault = 0;
  dut->im_req_ready = 0; dut->im_resp_valid = 0; dut->im_resp_fault = 0;
  uint8_t zero[kLineBytes] = {};
  AssignLine(&dut->dm_resp_rdata, zero);
  AssignLine(&dut->im_resp_rdata, zero);
}

void QuietMshr(Vmosaic_cache_tb* dut) {
  dut->nb_req_valid = 0; dut->nb_req_addr = 0; dut->nb_req_id = 0;
  dut->nb_cancel_valid = 0; dut->nb_cancel_id = 0;
  dut->nb_mem_req_ready = 0; dut->nb_mem_resp_valid = 0;
  dut->nb_mem_resp_addr = 0; dut->nb_mem_resp_fault = 0;
  dut->nb_dbg_index = 0;
  uint8_t zero[kLineBytes] = {};
  AssignLine(&dut->nb_mem_resp_rdata, zero);
}

class Harness {
 public:
  Harness(Vmosaic_cache_tb* dut, mosaic::ClockDriver* clk, mosaic::Reporter* rep,
          uint64_t max_cycles)
      : dut_(dut), clk_(clk), rep_(rep), max_cycles_(max_cycles) {
    mem_.assign(kMemBytes, 0);
    ref_mem_.assign(kMemBytes, 0);
    FillPattern(&mem_);
    FillPattern(&ref_mem_);
  }

  void Phase(const char* name) { phase_ = name; }
  uint64_t cycles() const { return clk_->cycle(); }

  // --------------------------------------------------------- memory model
  uint64_t MemWord(uint32_t addr) const { return ReadWordLE(&mem_[addr]); }
  void MemPokeWord(uint32_t addr, uint64_t value) {
    WriteWordLE(&mem_[addr], value);
    WriteWordLE(&ref_mem_[addr], value);
  }

  // Poison a line's first touch on both sides.
  void Poison(uint32_t line) {
    poison_.insert(line);
    ref_poison_.insert(line);
  }

  // Hold the memory port's ready low for `cycles` ticks (a deliberate stall).
  void StallMemory(int cycles) { mem_stall_ = cycles; }

  // ------------------------------------------------------------- reset
  void Reset(int cycles) {
    ClearCpuRequests();
    i_fl_valid_m_ = d_fl_valid_m_ = false;
    i_pend_ = d_pend_ = false;
    for (int i = 0; i < cycles; ++i) Tick(/*rst=*/true);
    Tick(/*rst=*/false);
  }

  // ---- reset-traffic control (V-010) --------------------------------------
  // Drives one instruction-side memory request into the DUT and runs the same
  // ServiceMem the cycle loop runs, with reset asserted. Returns true iff the
  // model accepted it. The shipping configuration must refuse; the control must
  // accept, which is what proves the rule check can fail.
  bool PresentDuringReset() {
    const uint64_t before = i_mem_reads_;
    const std::set<uint32_t> saved_poison = poison_;
    const auto saved_valid = dut_->im_req_valid;
    const auto saved_we = dut_->im_req_we;
    const auto saved_addr = dut_->im_req_addr;
    const auto saved_ready = dut_->im_req_ready;
    dut_->im_req_valid = 1;
    dut_->im_req_we = 0;
    dut_->im_req_addr = 0x1000;  // a line inside mem_
    dut_->im_req_ready = 1;
    dut_->eval();
    ServiceMem(/*ic=*/true, /*rst=*/true);
    dut_->im_req_valid = saved_valid;
    dut_->im_req_we = saved_we;
    dut_->im_req_addr = saved_addr;
    dut_->im_req_ready = saved_ready;
    dut_->eval();
    const bool accepted = i_mem_reads_ != before;
    // The control must not perturb the model's tallies or its fault policy:
    // later checks compare them against the expected counts.
    i_mem_reads_ = before;
    i_pend_ = false;
    poison_ = saved_poison;
    return accepted;
  }
  void SetAcceptDuringResetControl(bool on) { bus_gate_.SetAcceptDuringResetControl(on); }

  // -------------------------------------------------------- cache access
  Result Access(bool ic, bool we, uint32_t addr, uint64_t wdata = 0,
                uint8_t wmask = 0xff) {
    SetRequest(ic, true, we, addr, wdata, wmask);
    while (true) {
      Tick(/*rst=*/false);
      if (Accepted(ic)) break;
    }
    SetRequest(ic, false, we, addr, wdata, wmask);
    while (true) {
      Tick(/*rst=*/false);
      if (ResponseValid(ic)) break;
    }
    Result result;
    result.fault = ic ? i_resp_fault_ : d_resp_fault_;
    result.rdata = ic ? i_resp_rdata_ : d_resp_rdata_;
    return result;
  }

  // One access through the DUT and the reference; fails on the first
  // disagreement. `ctx` names the case in the failure text.
  Result Do(bool ic, bool we, uint32_t addr, uint64_t wdata = 0,
            uint8_t wmask = 0xff, const char* ctx = "") {
    const Result dut = Access(ic, we, addr, wdata, wmask);
    const Result ref = Reference(ic, we, addr, wdata, wmask);

    if (dut.fault != ref.fault) {
      Fail(std::string(phase_) + ": " + ctx + " fault response",
           ref.fault ? "1" : "0", dut.fault ? "1" : "0");
    }
    if (!we && !dut.fault && !ref.fault && dut.rdata != ref.rdata) {
      Fail(std::string(phase_) + ": " + ctx + " read data",
           mosaic::Hex(ref.rdata), mosaic::Hex(dut.rdata));
    }
    return dut;
  }

  Result Store(bool ic, uint32_t addr, uint64_t wdata, uint8_t wmask = 0xff,
               const char* ctx = "store") {
    return Do(ic, true, addr, wdata, wmask, ctx);
  }
  Result Load(bool ic, uint32_t addr, const char* ctx = "load") {
    return Do(ic, false, addr, 0, 0, ctx);
  }

  // ------------------------------------- non-blocking campaign primitives
  // `Access`/`Do` are strictly sequential: they wait for each response before
  // offering the next request. The non-blocking campaign needs to have more
  // than one request in flight, so these expose the handshake one cycle at a
  // time. `Offer` leaves the request asserted; `Withdraw` drops it.
  void Offer(bool ic, bool we, uint32_t addr, uint64_t wdata = 0, uint8_t wmask = 0xff) {
    SetRequest(ic, true, we, addr, wdata, wmask);
  }
  void Withdraw(bool ic) { SetRequest(ic, false, false, 0, 0, 0); }
  void TickNow() { Tick(false); }
  bool TickAccepted(bool ic) { Tick(false); return Accepted(ic); }
  bool RespValidNow(bool ic) const { return ic ? i_resp_valid_ : d_resp_valid_; }
  bool RespFaultNow(bool ic) const { return ic ? i_resp_fault_ : d_resp_fault_; }
  uint64_t RespDataNow(bool ic) const { return ic ? i_resp_rdata_ : d_resp_rdata_; }
  uint32_t OutstandingNow() { dut_->eval(); return dut_->dev_outstanding; }
  uint64_t DcCoalesce() const { return d_coalesce_; }

  // ------------------------------------------------------------- flush
  void Flush(bool ic) {
    if (ic) i_fl_valid_m_ = true; else d_fl_valid_m_ = true;
    while (true) {
      Tick(/*rst=*/false);
      if (ic ? i_fl_accept_ : d_fl_accept_) break;
    }
    if (ic) i_fl_valid_m_ = false; else d_fl_valid_m_ = false;
    while (true) {
      Tick(/*rst=*/false);
      if (ic ? i_fl_done_ : d_fl_done_) break;
    }
  }

  // ------------------------------------------------------- debug state view
  bool DebugValid(bool ic, uint32_t addr) {
    const int index = SetOf(addr);
    if (ic) { i_dbg_index_m_ = index; dut_->idbg_index = index; }
    else    { d_dbg_index_m_ = index; dut_->ddbg_index = index; }
    dut_->eval();
    return (ic ? dut_->idbg_valid : dut_->ddbg_valid) != 0;
  }

  void CheckAllInvalid(bool ic, const char* ctx) {
    for (int index = 0; index < kSets; ++index) {
      if (ic) { i_dbg_index_m_ = index; dut_->idbg_index = index; }
      else    { d_dbg_index_m_ = index; dut_->ddbg_index = index; }
      dut_->eval();
      const bool valid = (ic ? dut_->idbg_valid : dut_->ddbg_valid) != 0;
      if (valid) {
        Fail(std::string(phase_) + ": " + ctx + " set " + std::to_string(index) +
                 " valid after reset", "0", "1");
      }
    }
  }

  void CheckMemoryIdentical() {
    for (uint32_t i = 0; i < kMemBytes; ++i) {
      if (mem_[i] != ref_mem_[i]) {
        Fail(std::string(phase_) + ": backing memory byte " + mosaic::Hex(i, 4),
             mosaic::Hex(ref_mem_[i], 2), mosaic::Hex(mem_[i], 2));
      }
    }
  }

  void CheckMemoryLine(uint32_t line, const char* ctx) {
    for (int i = 0; i < kLineBytes; ++i) {
      const uint32_t addr = line + i;
      if (mem_[addr] != ref_mem_[addr]) {
        Fail(std::string(phase_) + ": " + ctx + " byte " + mosaic::Hex(addr, 4),
             mosaic::Hex(ref_mem_[addr], 2), mosaic::Hex(mem_[addr], 2));
      }
    }
  }

  // ------------------------------------------------------------- counters
  const Counters& Dc() const { return d_ev_; }
  const Counters& Ic() const { return i_ev_; }
  uint64_t DcMemReads() const { return d_mem_reads_; }
  uint64_t DcMemWrites() const { return d_mem_writes_; }
  uint64_t IcMemReads() const { return i_mem_reads_; }
  uint64_t IcMemWrites() const { return i_mem_writes_; }

 private:
  // ------------------------------------------------------------- reference
  // Cache-off: a flat memory read/write with a line-level fault policy. The
  // policy is "the first touch of a poisoned line faults once", identical for
  // the DUT (a refill) and the reference (a word access), so the two agree on
  // *which* access faults even though the DUT issues fewer memory transactions.
  Result Reference(bool ic, bool we, uint32_t addr, uint64_t wdata, uint8_t wmask) {
    Result result;
    if (ic && we) {  // a store to an instruction cache is always a fault
      result.fault = true;
      return result;
    }
    const uint32_t line = LineOf(addr);
    if (ref_poison_.count(line) != 0) {
      ref_poison_.erase(line);
      result.fault = true;
      return result;
    }
    if (we) {
      for (int b = 0; b < kCpuBytes; ++b) {
        if (wmask & (1u << b)) ref_mem_[addr + b] = static_cast<uint8_t>(wdata >> (8 * b));
      }
      return result;
    }
    result.rdata = ReadWordLE(&ref_mem_[addr]);
    return result;
  }

  // ------------------------------------------------------------- plumbing
  void SetRequest(bool ic, bool valid, bool we, uint32_t addr, uint64_t wdata,
                  uint8_t wmask) {
    if (ic) {
      i_req_valid_m_ = valid; i_req_we_m_ = we; i_req_addr_m_ = addr;
      i_req_wdata_m_ = wdata; i_req_wmask_m_ = wmask;
    } else {
      d_req_valid_m_ = valid; d_req_we_m_ = we; d_req_addr_m_ = addr;
      d_req_wdata_m_ = wdata; d_req_wmask_m_ = wmask;
    }
  }
  void ClearCpuRequests() {
    i_req_valid_m_ = d_req_valid_m_ = false;
    i_req_we_m_ = d_req_we_m_ = false;
    i_req_addr_m_ = d_req_addr_m_ = 0;
    i_req_wdata_m_ = d_req_wdata_m_ = 0;
    i_req_wmask_m_ = d_req_wmask_m_ = 0;
  }
  bool Accepted(bool ic) const { return ic ? i_accept_ : d_accept_; }
  bool ResponseValid(bool ic) const { return ic ? i_resp_valid_ : d_resp_valid_; }

  [[noreturn]] void Fail(const std::string& where, const std::string& expected,
                         const std::string& actual) {
    rep_->Mismatch(where, expected, actual);
    throw Failure{where};
  }

  void ServiceMem(bool ic, bool rst) {
    const bool req_valid = ic ? (dut_->im_req_valid != 0) : (dut_->dm_req_valid != 0);
    const bool req_we    = ic ? (dut_->im_req_we != 0) : (dut_->dm_req_we != 0);
    const uint32_t addr  = ic ? dut_->im_req_addr : dut_->dm_req_addr;
    const bool ready     = ic ? (dut_->im_req_ready != 0) : (dut_->dm_req_ready != 0);

    bool next_pend = false;
    // The reset-traffic rule lives in one place (mosaic::BusResetGate): a request
    // presented while reset is asserted is refused. This was an ad-hoc `!rst`
    // guard; the gate is strictly equivalent here and is the shared mechanism.
    if (bus_gate_.MayAccept(rst, req_valid && ready)) {
      if (req_we) {
        uint8_t line_data[kLineBytes];
        if (ic) ExtractLine(dut_->im_req_wdata, line_data);
        else    ExtractLine(dut_->dm_req_wdata, line_data);
        for (int i = 0; i < kLineBytes; ++i) mem_[addr + i] = line_data[i];
        if (ic) ++i_mem_writes_; else ++d_mem_writes_;
      } else {
        if (ic) ++i_mem_reads_; else ++d_mem_reads_;
        const uint32_t line = LineOf(addr);
        const bool fault = poison_.count(line) != 0;
        if (fault) poison_.erase(line);
        uint8_t data[kLineBytes];
        for (int i = 0; i < kLineBytes; ++i) data[i] = fault ? 0 : mem_[line + i];
        next_pend = true;
        if (ic) { i_pend_fault_ = fault; std::memcpy(i_pend_data_, data, kLineBytes); }
        else    { d_pend_fault_ = fault; std::memcpy(d_pend_data_, data, kLineBytes); }
      }
    }
    if (ic) i_pend_ = next_pend; else d_pend_ = next_pend;
  }

  // One full clock period: drive, evaluate, sample outputs, service the memory
  // ports, then toggle the clock.
  void Tick(bool rst) {
    if (clk_->cycle() >= max_cycles_) {
      Fail(std::string(phase_) + ": max-cycles (" + std::to_string(max_cycles_) +
               ") exhausted before the campaign finished",
           "campaign fits in the cycle budget", "ran out of cycles");
    }

    // --- drive inputs -------------------------------------------------------
    dut_->rst = rst ? 1 : 0;

    dut_->dc_req_valid = d_req_valid_m_ ? 1 : 0;
    dut_->dc_req_we    = d_req_we_m_ ? 1 : 0;
    dut_->dc_req_addr  = d_req_addr_m_;
    dut_->dc_req_wdata = d_req_wdata_m_;
    dut_->dc_req_wmask = d_req_wmask_m_;
    dut_->ic_req_valid = i_req_valid_m_ ? 1 : 0;
    dut_->ic_req_we    = i_req_we_m_ ? 1 : 0;
    dut_->ic_req_addr  = i_req_addr_m_;
    dut_->ic_req_wdata = i_req_wdata_m_;
    dut_->ic_req_wmask = i_req_wmask_m_;

    dut_->dfl_valid = d_fl_valid_m_ ? 1 : 0;
    dut_->ifl_valid = i_fl_valid_m_ ? 1 : 0;

    dut_->ddbg_index = d_dbg_index_m_;
    dut_->idbg_index = i_dbg_index_m_;
    QuietMshr(dut_);

    // Memory-side response, one cycle after a read was accepted. Delivery goes
    // through the same gate: no response crosses a reset.
    const bool deliver = bus_gate_.MayDeliver(rst);
    dut_->dm_resp_valid = (deliver && d_pend_) ? 1 : 0;
    dut_->dm_resp_fault = d_pend_fault_ ? 1 : 0;
    AssignLine(&dut_->dm_resp_rdata, d_pend_data_);
    dut_->im_resp_valid = (deliver && i_pend_) ? 1 : 0;
    dut_->im_resp_fault = i_pend_fault_ ? 1 : 0;
    AssignLine(&dut_->im_resp_rdata, i_pend_data_);

    const bool ready = (mem_stall_ == 0);
    dut_->dm_req_ready = ready ? 1 : 0;
    dut_->im_req_ready = ready ? 1 : 0;
    if (mem_stall_ > 0) --mem_stall_;

    // --- evaluate -----------------------------------------------------------
    dut_->eval();

    // --- sample CPU-side handshake ------------------------------------------
    d_accept_     = d_req_valid_m_ && (dut_->dc_req_ready != 0);
    i_accept_     = i_req_valid_m_ && (dut_->ic_req_ready != 0);
    d_resp_valid_ = dut_->dc_resp_valid != 0;
    i_resp_valid_ = dut_->ic_resp_valid != 0;
    d_resp_fault_ = dut_->dc_resp_fault != 0;
    i_resp_fault_ = dut_->ic_resp_fault != 0;
    d_resp_rdata_ = dut_->dc_resp_rdata;
    i_resp_rdata_ = dut_->ic_resp_rdata;
    d_fl_accept_  = d_fl_valid_m_ && (dut_->dfl_ready != 0);
    i_fl_accept_  = i_fl_valid_m_ && (dut_->ifl_ready != 0);
    d_fl_done_    = dut_->dfl_done != 0;
    i_fl_done_    = dut_->ifl_done != 0;

    // --- sample trace pulses -------------------------------------------------
    d_ev_.hit       += dut_->dev_hit != 0;
    d_ev_.miss      += dut_->dev_miss != 0;
    d_ev_.refill    += dut_->dev_refill != 0;
    d_ev_.writeback += dut_->dev_writeback != 0;
    d_ev_.fault     += dut_->dev_fault != 0;
    d_coalesce_     += dut_->dev_coalesce != 0;
    i_ev_.hit       += dut_->iev_hit != 0;
    i_ev_.miss      += dut_->iev_miss != 0;
    i_ev_.refill    += dut_->iev_refill != 0;
    i_ev_.writeback += dut_->iev_writeback != 0;
    i_ev_.fault     += dut_->iev_fault != 0;

    // --- service both memory line ports -------------------------------------
    ServiceMem(false, rst);
    ServiceMem(true, rst);

    // --- rising edge --------------------------------------------------------
    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();
  }

  Vmosaic_cache_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* rep_;
  uint64_t max_cycles_;
  std::string phase_;

  std::vector<uint8_t> mem_;       // DUT backing store
  std::vector<uint8_t> ref_mem_;   // reference backing store
  mosaic::BusResetGate bus_gate_;  // V-010 reset-traffic rule
  std::set<uint32_t> poison_;      // DUT-side first-touch fault policy
  std::set<uint32_t> ref_poison_;  // reference-side first-touch fault policy

  bool     d_req_valid_m_ = false, i_req_valid_m_ = false;
  bool     d_req_we_m_ = false, i_req_we_m_ = false;
  uint32_t d_req_addr_m_ = 0, i_req_addr_m_ = 0;
  uint64_t d_req_wdata_m_ = 0, i_req_wdata_m_ = 0;
  uint8_t  d_req_wmask_m_ = 0, i_req_wmask_m_ = 0;
  bool     d_fl_valid_m_ = false, i_fl_valid_m_ = false;
  int      d_dbg_index_m_ = 0, i_dbg_index_m_ = 0;

  bool    d_pend_ = false, i_pend_ = false;
  bool    d_pend_fault_ = false, i_pend_fault_ = false;
  uint8_t d_pend_data_[kLineBytes] = {};
  uint8_t i_pend_data_[kLineBytes] = {};

  bool     d_accept_ = false, i_accept_ = false;
  bool     d_resp_valid_ = false, i_resp_valid_ = false;
  bool     d_resp_fault_ = false, i_resp_fault_ = false;
  uint64_t d_resp_rdata_ = 0, i_resp_rdata_ = 0;
  bool     d_fl_accept_ = false, i_fl_accept_ = false;
  bool     d_fl_done_ = false, i_fl_done_ = false;

  int mem_stall_ = 0;

  Counters d_ev_, i_ev_;
  uint64_t d_coalesce_ = 0;
  uint64_t d_mem_reads_ = 0, d_mem_writes_ = 0;
  uint64_t i_mem_reads_ = 0, i_mem_writes_ = 0;
};

// ---------------------------------------------------------------------------
// The campaign.
class Campaign {
 public:
  Campaign(Harness* h, mosaic::Reporter* rep) : h_(h), rep_(rep) {}

  std::string Run() {
    ColdReset();
    ResetTraffic();
    RefillAndHit();
    Conflict();
    DirtyEviction();
    SelfModify();
    RefillFault();
    InstructionCache();
    Backpressure();
    ValidInit();
    return FinalDrain();
  }

 private:
  void Check(bool passed, const std::string& what) { rep_->Check(passed, what); }

  // V-010: the bus model refuses a request presented while reset is asserted,
  // and the guard has been seen to fire.
  void ResetTraffic() {
    h_->Phase("reset-traffic");
    h_->Reset(2);
    Check(!h_->PresentDuringReset(),
          "reset-traffic: the memory model accepted a request while reset was asserted");
    h_->SetAcceptDuringResetControl(true);
    const bool control_accepted = h_->PresentDuringReset();
    h_->SetAcceptDuringResetControl(false);
    Check(control_accepted,
          "reset-traffic: the accept-during-reset control did not fire: the guard is untested");
    Check(!h_->PresentDuringReset(),
          "reset-traffic: the accept-during-reset control was left engaged");
  }

  void ColdReset() {
    h_->Phase("cold-reset");
    h_->Reset(4);
    h_->CheckAllInvalid(false, "dcache");
    h_->CheckAllInvalid(true, "icache");
    Check(true, "cold-reset: both caches have no valid lines after reset");
  }

  void RefillAndHit() {
    h_->Phase("refill");
    const uint32_t a = Addr(2, 3, 0);
    const Counters before = h_->Dc();
    h_->Load(false, a, "first load");
    const Counters after_miss = h_->Dc();
    Check(after_miss.miss == before.miss + 1, "refill: first load is a miss");
    Check(after_miss.refill == before.refill + 1, "refill: first load refills");
    Check(after_miss.hit == before.hit, "refill: first load is not a hit");

    h_->Load(false, a, "second load");
    const Counters after_hit = h_->Dc();
    Check(after_hit.hit == after_miss.hit + 1, "refill: second load hits");
    Check(after_hit.refill == after_miss.refill, "refill: hit does not refill");

    for (uint32_t w = 1; w < static_cast<uint32_t>(kLineBytes / kCpuBytes); ++w) {
      h_->Load(false, Addr(2, 3, w), "resident word");
    }
    Check(h_->Dc().refill == after_miss.refill, "refill: one line, one refill");
  }

  void Conflict() {
    h_->Phase("conflict");
    const uint32_t a = Addr(4, 1, 0);
    const uint32_t b = Addr(5, 1, 0);
    Check(SetOf(a) == SetOf(b) && a != b, "conflict: addresses share a set");

    h_->Store(false, a, 0x1111222233334444ull, 0xff, "store A");
    const Result back = h_->Load(false, a, "reload A");
    Check(back.rdata == 0x1111222233334444ull, "conflict: A reads back its store");

    const Counters before = h_->Dc();
    h_->Load(false, b, "load B (evicts A)");
    const Counters after = h_->Dc();
    Check(after.miss == before.miss + 1, "conflict: B misses");
    Check(after.refill == before.refill + 1, "conflict: B refills");

    h_->Load(false, a, "reload A after eviction");
  }

  void DirtyEviction() {
    h_->Phase("dirty-eviction");
    const uint32_t a = Addr(6, 2, 0);
    const uint32_t b = Addr(7, 2, 0);
    const uint64_t v0 = 0xdeadbeefcafef00dull;
    const uint64_t v1 = 0x0123456789abcdefull;

    h_->Store(false, a, v0, 0xff, "store A word0");
    h_->Store(false, a + kCpuBytes, v1, 0xff, "store A word1");
    const Counters before = h_->Dc();
    h_->Load(false, b, "load B (evicts dirty A)");
    const Counters after = h_->Dc();
    Check(after.writeback == before.writeback + 1,
          "dirty-eviction: the eviction writes back");
    h_->CheckMemoryLine(LineOf(a), "dirty-eviction: dirty bytes survived");
    const Result r0 = h_->Load(false, a, "reload A word0");
    Check(r0.rdata == v0, "dirty-eviction: word0 survived the writeback");
    const Result r1 = h_->Load(false, a + kCpuBytes, "reload A word1");
    Check(r1.rdata == v1, "dirty-eviction: word1 survived the writeback");
  }

  void SelfModify() {
    h_->Phase("self-modify");
    const uint32_t a = Addr(8, 5, 0);
    const uint64_t v = 0x5566778899aabbccull;

    h_->Store(false, a, v, 0xff, "self-modify store");
    const Result r = h_->Load(false, a, "self-modify load");
    Check(r.rdata == v, "self-modify: a store is visible to the next load");

    h_->Store(false, a, 0x0000000000000042ull, 0x01, "byte store");
    const Result rb = h_->Load(false, a, "byte store load");
    Check(rb.rdata == ((v & ~0xffull) | 0x42ull),
          "self-modify: a byte store changes exactly one byte");

    h_->Store(false, a + 2 * kCpuBytes, 0x000000000000beefull, 0x03, "halfword store");
    const Result rw = h_->Load(false, a, "word0 after word2 store");
    Check(rw.rdata == ((v & ~0xffull) | 0x42ull),
          "self-modify: a store to another word leaves word0 alone");
  }

  void RefillFault() {
    h_->Phase("refill-fault");
    const uint32_t a = Addr(9, 4, 0);

    h_->Poison(LineOf(a));
    const Counters before = h_->Dc();
    const Result first = h_->Load(false, a, "poisoned load");
    const Counters after = h_->Dc();
    Check(first.fault, "refill-fault: a failed refill answers with a fault");
    Check(after.fault == before.fault + 1, "refill-fault: ev_fault pulsed");
    Check(!h_->DebugValid(false, a),
          "refill-fault: the failed line is NOT marked valid");

    const Result second = h_->Load(false, a, "retry load");
    Check(!second.fault, "refill-fault: the retry succeeds");
    const Result third = h_->Load(false, a, "third load");
    Check(!third.fault, "refill-fault: the retried line hits");

    const uint32_t c = Addr(10, 4, 0);
    h_->Poison(LineOf(c));
    const Result store = h_->Store(false, c, 0xffffffffffffffffull, 0xff, "poisoned store");
    Check(store.fault, "refill-fault: a failed store refill faults the store");
    h_->Store(false, c, 0x1122334455667788ull, 0xff, "store retry");
  }

  void InstructionCache() {
    h_->Phase("icache");
    const uint32_t a = Addr(3, 6, 0);
    const Counters before = h_->Ic();
    h_->Load(true, a, "icache fetch");
    const Counters after = h_->Ic();
    Check(after.miss == before.miss + 1, "icache: a cold fetch misses");
    Check(after.refill == before.refill + 1, "icache: a cold fetch refills");
    h_->Load(true, a, "icache hit");
    Check(h_->Ic().hit == after.hit + 1, "icache: the second fetch hits");

    const uint64_t before_word = h_->MemWord(a);
    const Result st = h_->Store(true, a, 0, 0xff, "icache store");
    Check(st.fault, "icache: a store to the instruction cache faults");
    Check(h_->MemWord(a) == before_word && h_->IcMemWrites() == 0,
          "icache: a faulting store leaves memory unchanged");

    const uint32_t b = Addr(3, 7, 0);
    const Result st2 = h_->Store(true, b, 0, 0xff, "icache store cold");
    Check(st2.fault, "icache: a store to a cold line faults");
    Check(!h_->DebugValid(true, b), "icache: a faulting store allocates nothing");
  }

  void Backpressure() {
    h_->Phase("backpressure");
    h_->StallMemory(3);
    const uint32_t a = Addr(11, 0, 0);
    h_->Load(false, a, "stalled load");
    h_->Load(false, a, "load after stall");
  }

  void ValidInit() {
    h_->Phase("valid-init");
    // Drain dirty lines first: a reset discards cache contents by design, so
    // anything still dirty would be dropped on the DUT side but not in the
    // reference, which has no cache to reset. Flushing makes the two agree
    // before the reset's own property is measured.
    h_->Flush(false);
    h_->Flush(true);
    h_->Reset(4);
    const uint32_t a = Addr(12, 7, 0);
    h_->Load(false, a, "pre-reset load");
    Check(h_->DebugValid(false, a), "valid-init: the loaded line is valid");
    h_->Reset(4);
    h_->CheckAllInvalid(false, "dcache");
    h_->MemPokeWord(a, 0x0000000012345678ull);
    const Result r = h_->Load(false, a, "post-reset load");
    Check(r.rdata == 0x0000000012345678ull,
          "valid-init: a reset line is refetched, not served stale");
  }

  std::string FinalDrain() {
    h_->Phase("final-drain");
    h_->Flush(false);
    h_->Flush(true);
    h_->CheckMemoryIdentical();

    const Counters d = h_->Dc();
    const Counters i = h_->Ic();
    Check(d.miss == d.refill + d.fault, "invariant: dcache miss ends in refill or fault");
    Check(d.hit > 0 && d.miss > 0 && d.refill > 0 && d.writeback > 0 && d.fault > 0,
          "coverage: dcache exercised hit/miss/refill/writeback/fault");
    Check(h_->DcMemReads() == d.refill + d.fault,
          "invariant: every dcache memory read is a refill or a fault");
    Check(h_->DcMemWrites() == d.writeback,
          "invariant: every dcache memory write is a writeback");
    Check(i.miss == i.refill + i.fault, "invariant: icache miss ends in refill or fault");
    Check(i.hit > 0 && i.miss > 0 && i.refill > 0,
          "coverage: icache exercised hit/miss/refill");
    Check(i.writeback == 0, "invariant: a read-only cache never writes back");
    Check(h_->IcMemReads() == i.refill + i.fault,
          "invariant: every icache memory read is a refill or a fault");
    Check(h_->IcMemWrites() == 0, "invariant: a read-only cache never writes memory");

    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "trace equivalent: d(hit=%llu miss=%llu refill=%llu wb=%llu fault=%llu) "
                  "i(hit=%llu miss=%llu refill=%llu fault=%llu) cycles=%llu",
                  (unsigned long long)d.hit, (unsigned long long)d.miss,
                  (unsigned long long)d.refill, (unsigned long long)d.writeback,
                  (unsigned long long)d.fault, (unsigned long long)i.hit,
                  (unsigned long long)i.miss, (unsigned long long)i.refill,
                  (unsigned long long)i.fault, (unsigned long long)h_->cycles());
    return std::string(detail);
  }

  Harness* h_;
  mosaic::Reporter* rep_;
};

// ===========================================================================
// I-043 -- CASE=cache.mshr_nonblocking.
//
// The DUT is the non-blocking read L1 (`mosaic_mshr`) in the shared wrapper.
// The oracle is again the contract, not the RTL:
//
//   * every accepted request that is not a hit is expected to be answered
//     exactly once, and the answer's data is the memory word at the requested
//     address (read out of a flat byte array, not out of any cache model);
//   * a request issued while its line is poisoned is expected to be answered
//     with a fault, and a fault is line-level: every waiter coalesced on the
//     same refill faults with it;
//   * a request that was cancelled must never be answered, and its refill must
//     not install the line;
//   * conservation is checked every cycle: `miss_accepted == responses +
//     cancels + dbg_waiters`, so a leaked entry or a double-answered request
//     cannot pass even if every directed check happens to miss it.
//
// The memory model here holds *several* reads and lets the campaign choose
// which one to answer, so "different lines return out of order" is real
// stimulus rather than an assertion about a single outstanding miss.
// ===========================================================================

struct PendingRead {
  uint32_t addr = 0;
  bool     fault = false;
  uint8_t  data[kLineBytes] = {};
};

struct Expectation {
  uint32_t addr = 0;
  bool     fault = false;
};

class MshrHarness {
 public:
  MshrHarness(Vmosaic_cache_tb* dut, mosaic::ClockDriver* clk,
              mosaic::Reporter* rep, uint64_t max_cycles)
      : dut_(dut), clk_(clk), rep_(rep), max_cycles_(max_cycles) {
    mem_.assign(kMemBytes, 0);
    ref_mem_.assign(kMemBytes, 0);
    FillPattern(&mem_);
    FillPattern(&ref_mem_);
  }

  void Phase(const char* name) { phase_ = name; }
  uint64_t cycles() const { return clk_->cycle(); }

  // ------------------------------------------------------------- reference
  uint64_t RefWord(uint32_t addr) const { return ReadWordLE(&ref_mem_[addr]); }
  uint32_t LineOf(uint32_t addr) const { return addr & ~kLineMask; }
  int SetOf(uint32_t addr) const { return static_cast<int>((addr >> kOffsetBits) & (kSets - 1)); }
  void PoisonLine(uint32_t line) { poison_.insert(line); }

  // ------------------------------------------------------------- reset
  void Reset(int cycles) {
    req_valid_m_ = cancel_valid_m_ = false;
    resp_drive_ = false;
    dbg_index_m_ = 0;
    for (int i = 0; i < cycles; ++i) Tick(/*rst=*/true);
    Tick(/*rst=*/false);
  }

  // The one place a memory read is accepted, so the reset-traffic control below
  // exercises the real accept path. The rule lives in mosaic::BusResetGate: a
  // request presented while reset is asserted is refused. This was an ad-hoc
  // `!rst` guard; the gate is strictly equivalent here and is the shared
  // mechanism.
  void AcceptMemRead(bool rst) {
    if (!bus_gate_.MayAccept(rst, (dut_->nb_mem_req_valid != 0) &&
                                     (dut_->nb_mem_req_ready != 0))) {
      return;
    }
    PendingRead r;
    r.addr = dut_->nb_mem_req_addr;
    const uint32_t line = LineOf(r.addr);
    r.fault = poison_.count(line) != 0;
    if (r.fault) poison_.erase(line);
    for (int i = 0; i < kLineBytes; ++i) {
      r.data[i] = r.fault ? 0 : mem_[line + i];
    }
    pending_reads_.push_back(r);
    ++mem_reads_;
  }

  // ---- reset-traffic control (V-010) --------------------------------------
  // Drives one memory request into the DUT and runs the same AcceptMemRead the
  // cycle loop runs, with reset asserted. Returns true iff the model accepted
  // it. The shipping configuration must refuse; the control must accept, which
  // is what proves the rule check can fail.
  bool PresentDuringReset() {
    const size_t before = pending_reads_.size();
    const uint64_t reads_before = mem_reads_;
    const std::set<uint32_t> saved_poison = poison_;
    const auto saved_valid = dut_->nb_mem_req_valid;
    const auto saved_addr = dut_->nb_mem_req_addr;
    const auto saved_ready = dut_->nb_mem_req_ready;
    dut_->nb_mem_req_valid = 1;
    dut_->nb_mem_req_addr = 0x1000;  // a line inside mem_
    dut_->nb_mem_req_ready = 1;
    dut_->eval();
    AcceptMemRead(/*rst=*/true);
    dut_->nb_mem_req_valid = saved_valid;
    dut_->nb_mem_req_addr = saved_addr;
    dut_->nb_mem_req_ready = saved_ready;
    dut_->eval();
    const bool accepted = pending_reads_.size() != before;
    // The control must not perturb the model's tallies or its fault policy:
    // later checks compare them against the expected counts.
    if (accepted) pending_reads_.pop_back();
    mem_reads_ = reads_before;
    poison_ = saved_poison;
    return accepted;
  }
  void SetAcceptDuringResetControl(bool on) { bus_gate_.SetAcceptDuringResetControl(on); }

  // ------------------------------------------------------------- stimulus
  void Issue(uint32_t addr, uint32_t id, bool expect_fault, const char* ctx) {
    if (exp_.count(id) != 0 || cancelled_.count(id) != 0) {
      Fail(std::string(phase_) + ": " + ctx + " reuses a live request id",
           "a fresh id", std::to_string(id));
    }
    req_valid_m_ = true;
    req_addr_m_ = addr;
    req_id_m_ = id;
    int guard = 0;
    while (true) {
      Tick(/*rst=*/false);
      if (req_accept_) break;
      if (++guard > 1000) {
        Fail(std::string(phase_) + ": " + ctx + " was never accepted",
             "accepted", "back-pressured forever");
      }
    }
    req_valid_m_ = false;
    Expectation e;
    e.addr = addr;
    e.fault = expect_fault;
    exp_[id] = e;
  }

  void Cancel(uint32_t id) {
    if (exp_.count(id) == 0) {
      Fail(std::string(phase_) + ": cancel of an id that is not live",
           "a live id", std::to_string(id));
    }
    cancel_valid_m_ = true;
    cancel_id_m_ = id;
    Tick(/*rst=*/false);
    cancel_valid_m_ = false;
    exp_.erase(id);
    cancelled_.insert(id);
  }

  // Offer a request for exactly one cycle and report whether the DUT accepted
  // it. Used to observe back-pressure when the table is full, where `Issue`
  // (which waits for acceptance) would spin.
  bool TryIssue(uint32_t addr, uint32_t id, bool expect_fault) {
    req_valid_m_ = true;
    req_addr_m_ = addr;
    req_id_m_ = id;
    Tick(/*rst=*/false);
    const bool accepted = req_accept_;
    req_valid_m_ = false;
    if (accepted) {
      Expectation e;
      e.addr = addr;
      e.fault = expect_fault;
      exp_[id] = e;
    }
    return accepted;
  }

  // Deliver every queued read and wait for every entry to drain.
  void DrainAll() {
    int guard = 0;
    while (Outstanding() > 0 || !pending_reads_.empty()) {
      if (!pending_reads_.empty()) DeliverRead(0);
      else Tick(/*rst=*/false);
      if (++guard > 10000) {
        Fail(std::string(phase_) + ": drain did not finish", "drained", "still busy");
      }
    }
  }

  // Answer one queued memory read. `which` selects the queue entry (0 = the
  // oldest, -1 = the newest), so the campaign can return responses out of
  // order. Ticks until at least one read is queued.
  void DeliverRead(int which) {
    int guard = 0;
    while (pending_reads_.empty()) {
      Tick(/*rst=*/false);
      if (++guard > 10000) {
        Fail(std::string(phase_) + ": no pending read to deliver", "a read", "none");
      }
    }
    int idx = (which < 0) ? static_cast<int>(pending_reads_.size()) - 1 : which;
    if (idx < 0 || idx >= static_cast<int>(pending_reads_.size())) {
      Fail(std::string(phase_) + ": deliver index out of range", "in range", std::to_string(idx));
    }
    const PendingRead r = pending_reads_[idx];
    pending_reads_.erase(pending_reads_.begin() + idx);
    resp_drive_ = true;
    resp_drive_addr_ = r.addr;
    resp_drive_fault_ = r.fault;
    std::memcpy(resp_drive_data_, r.data, kLineBytes);
    Tick(/*rst=*/false);
  }

  void StallMemory(int cycles) { mem_stall_ = cycles; }
  void TickFor(int n) { for (int i = 0; i < n; ++i) Tick(/*rst=*/false); }
  void WaitReads(uint64_t target) {
    int guard = 0;
    while (mem_reads_ < target) {
      Tick(/*rst=*/false);
      if (++guard > 10000) {
        Fail(std::string(phase_) + ": timed out waiting for memory reads",
             std::to_string(target), std::to_string(mem_reads_));
      }
    }
  }
  void WaitResponses(uint64_t target) {
    int guard = 0;
    while (total_responses_ < target) {
      Tick(/*rst=*/false);
      if (++guard > 10000) {
        Fail(std::string(phase_) + ": timed out waiting for responses",
             "responses", "none");
      }
    }
  }

  // ------------------------------------------------------------- accessors
  uint64_t MemReads() const { return mem_reads_; }
  uint64_t TotalResponses() const { return total_responses_; }
  uint64_t DrainResponses() const { return drain_responses_; }
  uint64_t MissAccepted() const { return miss_accepted_; }
  uint64_t Cancels() const { return cancels_; }
  uint32_t Outstanding() { dut_->eval(); return dut_->nb_dbg_outstanding; }
  uint32_t Waiters() { dut_->eval(); return dut_->nb_dbg_waiters; }
  uint32_t MaxOutstanding() const { return max_outstanding_; }
  size_t   PendingReads() const { return pending_reads_.size(); }
  bool     MemReqValid() { dut_->eval(); return dut_->nb_mem_req_valid != 0; }
  bool     ExpEmpty() const { return exp_.empty(); }
  const std::vector<uint32_t>& RespOrder() const { return resp_order_; }
  uint64_t EvHit() const { return ev_hit_; }
  uint64_t EvMiss() const { return ev_miss_; }
  uint64_t EvCoalesce() const { return ev_coalesce_; }
  uint64_t EvRefill() const { return ev_refill_; }
  uint64_t EvFault() const { return ev_fault_; }
  uint64_t EvCancel() const { return ev_cancel_; }
  uint64_t EvDrop() const { return ev_drop_; }

  bool DebugValid(uint32_t addr) {
    dbg_index_m_ = SetOf(addr);
    dut_->nb_dbg_index = dbg_index_m_;
    dut_->eval();
    return dut_->nb_dbg_valid != 0;
  }
  void CheckAllInvalid(const char* ctx) {
    for (int index = 0; index < kSets; ++index) {
      dbg_index_m_ = index;
      dut_->nb_dbg_index = index;
      dut_->eval();
      if (dut_->nb_dbg_valid != 0) {
        Fail(std::string(phase_) + ": " + ctx + " set " + std::to_string(index) +
                 " is valid", "0", "1");
      }
    }
  }

  // ------------------------------------------------------------- one cycle
  void Tick(bool rst) {
    if (clk_->cycle() >= max_cycles_) {
      Fail(std::string(phase_) + ": max-cycles (" + std::to_string(max_cycles_) +
               ") exhausted", "campaign fits in the cycle budget", "ran out of cycles");
    }

    // --- drive -------------------------------------------------------------
    dut_->rst = rst ? 1 : 0;
    QuietBlockingCaches(dut_);
    dut_->nb_req_valid  = req_valid_m_ ? 1 : 0;
    dut_->nb_req_addr   = req_addr_m_;
    dut_->nb_req_id     = req_id_m_;
    dut_->nb_cancel_valid = cancel_valid_m_ ? 1 : 0;
    dut_->nb_cancel_id  = cancel_id_m_;
    dut_->nb_mem_req_ready = (mem_stall_ == 0) ? 1 : 0;
    if (mem_stall_ > 0) --mem_stall_;
    dut_->nb_mem_resp_valid = resp_drive_ ? 1 : 0;
    dut_->nb_mem_resp_addr  = resp_drive_addr_;
    dut_->nb_mem_resp_fault = resp_drive_fault_ ? 1 : 0;
    AssignLine(&dut_->nb_mem_resp_rdata, resp_drive_data_);
    dut_->nb_dbg_index = dbg_index_m_;
    dut_->eval();

    // --- sample ------------------------------------------------------------
    req_accept_ = req_valid_m_ && (dut_->nb_req_ready != 0);
    AcceptMemRead(rst);

    const bool resp_valid = !rst && (dut_->nb_resp_valid != 0);
    const uint32_t resp_id = dut_->nb_resp_id;
    const uint64_t resp_data = dut_->nb_resp_rdata;
    const bool resp_fault = (dut_->nb_resp_fault != 0);

    if (resp_valid) {
      ++total_responses_;
      resp_order_.push_back(resp_id);
      CheckResponse(resp_id, resp_data, resp_fault);
      if (hit_resp_pending_ > 0) --hit_resp_pending_;
      else ++drain_responses_;
    }

    // --- edge --------------------------------------------------------------
    dut_->clk = 1;
    dut_->eval();
    clk_->Tick();
    dut_->clk = 0;
    dut_->eval();

    // --- post-edge: events, occupancy and the conservation identity --------
    // The DUT's event pulses and its waiter bits are both registered, so they
    // are sampled *after* the edge: a pulse and the state change it describes
    // are then observed in the same instant. (A response is combinational, so
    // it is sampled before the edge; its waiter bit is cleared at this edge.)
    if (!rst) {
      if (dut_->nb_ev_hit != 0) { ++ev_hit_; ++hit_resp_pending_; }
      if (dut_->nb_ev_miss != 0) { ++ev_miss_; ++miss_accepted_; }
      if (dut_->nb_ev_coalesce != 0) ++ev_coalesce_;
      if (dut_->nb_ev_refill != 0) ++ev_refill_;
      if (dut_->nb_ev_fault != 0) ++ev_fault_;
      if (dut_->nb_ev_cancel != 0) { ++ev_cancel_; ++cancels_; }
      if (dut_->nb_ev_drop != 0) ++ev_drop_;

      const uint32_t outstanding = dut_->nb_dbg_outstanding;
      if (outstanding > max_outstanding_) max_outstanding_ = outstanding;
      if (outstanding > 4u) {
        Fail(std::string(phase_) + ": more live entries than the table has",
             "<= 4", std::to_string(outstanding));
      }
      const uint64_t waiters = dut_->nb_dbg_waiters;
      if (miss_accepted_ != drain_responses_ + cancels_ + waiters) {
        Fail(std::string(phase_) +
                 ": conservation: accepted misses != responses + cancels + waiters",
             std::to_string(miss_accepted_),
             std::to_string(drain_responses_ + cancels_ + waiters));
      }
    }
    resp_drive_ = false;
  }

 private:
  [[noreturn]] void Fail(const std::string& where, const std::string& expected,
                         const std::string& actual) {
    rep_->Mismatch(where, expected, actual);
    throw Failure{where};
  }

  void CheckResponse(uint32_t id, uint64_t data, bool fault) {
    const auto it = exp_.find(id);
    if (it == exp_.end()) {
      if (cancelled_.count(id) != 0) {
        Fail(std::string(phase_) + ": a cancelled request received a response (id " +
                 std::to_string(id) + ")", "no response", "a response");
      }
      Fail(std::string(phase_) + ": response for an id that was never accepted (id " +
               std::to_string(id) + ")", "no response", "a response");
    }
    if (it->second.fault != fault) {
      Fail(std::string(phase_) + ": response fault for id " + std::to_string(id),
           it->second.fault ? "1" : "0", fault ? "1" : "0");
    }
    if (!fault) {
      const uint64_t want = RefWord(it->second.addr);
      if (data != want) {
        Fail(std::string(phase_) + ": response data for id " + std::to_string(id),
             mosaic::Hex(want), mosaic::Hex(data));
      }
    }
    exp_.erase(it);
  }

  Vmosaic_cache_tb* dut_;
  mosaic::ClockDriver* clk_;
  mosaic::Reporter* rep_;
  uint64_t max_cycles_;
  std::string phase_;

  std::vector<uint8_t> mem_;
  std::vector<uint8_t> ref_mem_;
  mosaic::BusResetGate bus_gate_;  // V-010 reset-traffic rule
  std::set<uint32_t>   poison_;
  std::vector<PendingRead> pending_reads_;
  std::map<uint32_t, Expectation> exp_;
  std::set<uint32_t>   cancelled_;
  std::vector<uint32_t> resp_order_;

  bool     req_valid_m_ = false;
  uint32_t req_addr_m_ = 0;
  uint32_t req_id_m_ = 0;
  bool     cancel_valid_m_ = false;
  uint32_t cancel_id_m_ = 0;
  bool     req_accept_ = false;
  bool     resp_drive_ = false;
  uint32_t resp_drive_addr_ = 0;
  bool     resp_drive_fault_ = false;
  uint8_t  resp_drive_data_[kLineBytes] = {};
  int      dbg_index_m_ = 0;
  int      mem_stall_ = 0;

  uint64_t mem_reads_ = 0;
  uint64_t total_responses_ = 0;
  uint64_t drain_responses_ = 0;
  uint64_t miss_accepted_ = 0;
  uint64_t cancels_ = 0;
  uint64_t hit_resp_pending_ = 0;
  uint64_t ev_hit_ = 0, ev_miss_ = 0, ev_coalesce_ = 0, ev_refill_ = 0,
           ev_fault_ = 0, ev_cancel_ = 0, ev_drop_ = 0;
  uint32_t max_outstanding_ = 0;
};

class MshrCampaign {
 public:
  MshrCampaign(MshrHarness* h, mosaic::Reporter* rep) : h_(h), rep_(rep) {}

  std::string Run() {
    ColdReset();
    ResetTraffic();
    RefillAndHit();
    DuplicateMiss();
    DifferentLines();
    CancelOutstanding();
    CancelBeforeIssue();
    FaultingRefill();
    CoalescedFault();
    MshrFull();
    CoalescedCancel();
    IdReuse();
    return FinalCheck();
  }

 private:
  void Check(bool passed, const std::string& what) { rep_->Check(passed, what); }

  // V-010: the bus model refuses a request presented while reset is asserted,
  // and the guard has been seen to fire.
  void ResetTraffic() {
    h_->Phase("reset-traffic");
    h_->Reset(2);
    Check(!h_->PresentDuringReset(),
          "reset-traffic: the memory model accepted a request while reset was asserted");
    h_->SetAcceptDuringResetControl(true);
    const bool control_accepted = h_->PresentDuringReset();
    h_->SetAcceptDuringResetControl(false);
    Check(control_accepted,
          "reset-traffic: the accept-during-reset control did not fire: the guard is untested");
    Check(!h_->PresentDuringReset(),
          "reset-traffic: the accept-during-reset control was left engaged");
  }

  void ColdReset() {
    h_->Phase("cold-reset");
    h_->Reset(4);
    h_->CheckAllInvalid("mshr");
    Check(h_->Outstanding() == 0, "cold-reset: no outstanding entries after reset");
    Check(h_->Waiters() == 0, "cold-reset: no waiters after reset");
  }

  void RefillAndHit() {
    h_->Phase("refill-and-hit");
    const uint32_t a = Addr(2, 3, 0);
    const uint64_t reads = h_->MemReads();

    h_->Issue(a, 0, false, "cold load");
    h_->DeliverRead(0);
    h_->WaitResponses(1);
    Check(h_->MemReads() == reads + 1, "refill-and-hit: the cold load reads memory once");
    Check(h_->EvMiss() == 1 && h_->EvRefill() == 1, "refill-and-hit: miss then refill");
    Check(h_->DebugValid(a), "refill-and-hit: the refilled line is valid");
    Check(h_->Outstanding() == 0, "refill-and-hit: the entry is freed after the response");

    const uint64_t reads2 = h_->MemReads();
    const uint64_t hits = h_->EvHit();
    h_->Issue(a, 1, false, "second load");
    h_->WaitResponses(2);
    Check(h_->EvHit() == hits + 1, "refill-and-hit: the second load hits");
    Check(h_->MemReads() == reads2, "refill-and-hit: a hit does not read memory");
    Check(h_->Outstanding() == 0, "refill-and-hit: a hit touches no entry");
  }

  void DuplicateMiss() {
    h_->Phase("duplicate-miss");
    const uint32_t a = Addr(3, 6, 0);
    const uint32_t word0 = a;
    const uint32_t word2 = a + 2 * kCpuBytes;
    const uint64_t reads = h_->MemReads();
    const uint64_t responses = h_->TotalResponses();

    h_->Issue(word0, 0, false, "duplicate load A");
    h_->Issue(word2, 1, false, "duplicate load B");
    h_->WaitReads(reads + 1);
    Check(h_->MemReads() == reads + 1,
          "duplicate-miss: two requests to one line issue ONE memory read");
    Check(h_->EvCoalesce() >= 1, "duplicate-miss: the second request coalesced");
    Check(h_->Outstanding() == 1, "duplicate-miss: one entry serves both waiters");

    h_->DeliverRead(0);
    h_->WaitResponses(responses + 2);
    Check(h_->MemReads() == reads + 1,
          "duplicate-miss: no second read appears after the responses");
    Check(h_->Outstanding() == 0, "duplicate-miss: the entry is freed after both answers");
    Check(h_->DebugValid(a), "duplicate-miss: the line is installed");
  }

  void DifferentLines() {
    h_->Phase("different-lines");
    // Two lines that share a set: their refills evict each other, and the
    // campaign answers the newer read first, so installation order is reversed
    // relative to issue order as well.
    const uint32_t b = Addr(4, 1, 0);
    const uint32_t c = Addr(5, 1, 0);
    const uint64_t reads = h_->MemReads();
    const uint64_t responses = h_->TotalResponses();

    h_->Issue(b, 2, false, "line B");
    h_->Issue(c, 3, false, "line C");
    h_->WaitReads(reads + 2);
    Check(h_->MemReads() == reads + 2, "different-lines: two lines, two reads");
    Check(h_->Outstanding() == 2, "different-lines: both misses are outstanding");
    Check(h_->MaxOutstanding() >= 2, "different-lines: MLP reaches two");

    h_->DeliverRead(-1);   // answer C first
    h_->DeliverRead(0);    // then B
    h_->WaitResponses(responses + 2);

    const std::vector<uint32_t>& order = h_->RespOrder();
    Check(order.size() >= 2, "different-lines: two responses arrived");
    Check(order[order.size() - 2] == 3 && order[order.size() - 1] == 2,
          "different-lines: responses returned out of issue order with their own ids");
    Check(h_->Outstanding() == 0, "different-lines: both entries freed");
  }

  void CancelOutstanding() {
    h_->Phase("cancel-outstanding");
    const uint32_t d = Addr(6, 2, 0);
    const uint64_t reads = h_->MemReads();

    h_->Issue(d, 4, false, "load to be cancelled");
    while (h_->PendingReads() < 1) h_->TickFor(1);
    const uint64_t cancels = h_->Cancels();
    h_->Cancel(4);

    // Deliver the refill that was already in flight when the request died.
    h_->DeliverRead(0);
    h_->TickFor(4);
    Check(h_->Outstanding() == 0,
          "cancel-outstanding: a response after a cancellation frees the entry (no leak)");
    Check(!h_->DebugValid(d),
          "cancel-outstanding: a cancelled refill is NOT installed as valid");
    Check(h_->Cancels() == cancels + 1,
          "cancel-outstanding: exactly one cancellation was applied");

    // The line was not installed, so the next request to it must miss again.
    const uint64_t reads2 = h_->MemReads();
    const uint64_t responses = h_->TotalResponses();
    h_->Issue(d, 5, false, "retry after cancellation");
    h_->WaitReads(reads2 + 1);
    Check(h_->MemReads() == reads2 + 1,
          "cancel-outstanding: the retry refills the line a second time");
    h_->DeliverRead(0);
    h_->WaitResponses(responses + 1);
    Check(h_->DebugValid(d), "cancel-outstanding: the retried refill installs the line");
    Check(h_->MemReads() == reads + 2, "cancel-outstanding: exactly two reads for the line");
  }

  void CancelBeforeIssue() {
    h_->Phase("cancel-before-issue");
    const uint32_t e = Addr(7, 3, 0);
    const uint64_t reads = h_->MemReads();

    h_->StallMemory(1000);
    h_->Issue(e, 6, false, "load held before its read");
    Check(h_->MemReqValid(), "cancel-before-issue: the read is waiting to issue");
    const uint64_t cancels = h_->Cancels();
    h_->Cancel(6);
    Check(h_->Cancels() == cancels + 1, "cancel-before-issue: the cancel was applied");
    h_->TickFor(2);
    Check(!h_->MemReqValid(),
          "cancel-before-issue: a request cancelled before issue withdraws its read");
    Check(h_->MemReads() == reads, "cancel-before-issue: no memory read was issued");
    Check(h_->Outstanding() == 0, "cancel-before-issue: the entry is freed");
    h_->StallMemory(0);
  }

  void FaultingRefill() {
    h_->Phase("faulting-refill");
    const uint32_t f = Addr(8, 4, 0);
    h_->PoisonLine(h_->LineOf(f));
    const uint64_t responses = h_->TotalResponses();
    const uint64_t faults = h_->EvFault();

    h_->Issue(f, 7, true, "load to a poisoned line");
    h_->DeliverRead(0);
    h_->WaitResponses(responses + 1);
    Check(h_->EvFault() == faults + 1, "faulting-refill: ev_fault pulsed once");
    Check(!h_->DebugValid(f), "faulting-refill: a faulted refill is NOT installed");
    Check(h_->Outstanding() == 0, "faulting-refill: the faulted entry is freed");

    const uint64_t responses2 = h_->TotalResponses();
    h_->Issue(f, 0, false, "retry after a faulted refill");
    h_->DeliverRead(0);
    h_->WaitResponses(responses2 + 1);
    Check(h_->DebugValid(f), "faulting-refill: the retry succeeds and installs the line");
  }

  void CoalescedFault() {
    h_->Phase("coalesced-fault");
    const uint32_t g = Addr(9, 5, 0);
    h_->PoisonLine(h_->LineOf(g));
    const uint64_t reads = h_->MemReads();
    const uint64_t responses = h_->TotalResponses();

    h_->Issue(g, 1, true, "coalesced fault A");
    h_->Issue(g, 2, true, "coalesced fault B");
    h_->WaitReads(reads + 1);
    Check(h_->MemReads() == reads + 1,
          "coalesced-fault: one read for the coalesced pair");
    Check(h_->Outstanding() == 1, "coalesced-fault: one entry serves both");
    h_->DeliverRead(0);
    h_->WaitResponses(responses + 2);
    Check(h_->Outstanding() == 0, "coalesced-fault: the entry is freed");
    Check(!h_->DebugValid(g), "coalesced-fault: the faulted line is NOT installed");
  }

  void MshrFull() {
    h_->Phase("mshr-full");
    const uint32_t a0 = Addr(10, 0, 0);
    const uint32_t a1 = Addr(11, 1, 0);
    const uint32_t a2 = Addr(12, 2, 0);
    const uint32_t a3 = Addr(13, 3, 0);
    const uint32_t a4 = Addr(14, 4, 0);
    const uint64_t reads = h_->MemReads();
    const uint64_t responses = h_->TotalResponses();

    h_->Issue(a0, 0, false, "fill 0");
    h_->Issue(a1, 1, false, "fill 1");
    h_->Issue(a2, 2, false, "fill 2");
    h_->Issue(a3, 3, false, "fill 3");
    h_->WaitReads(reads + 4);
    Check(h_->Outstanding() == 4, "mshr-full: all four entries are occupied");

    Check(!h_->TryIssue(a4, 5, false),
          "mshr-full: a fifth miss is back-pressured, not silently dropped");
    Check(h_->Outstanding() == 4, "mshr-full: the refused request allocated nothing");

    h_->DeliverRead(0);
    h_->WaitResponses(responses + 1);
    Check(h_->Outstanding() == 3, "mshr-full: one response frees exactly one entry");

    Check(h_->TryIssue(a4, 5, false),
          "mshr-full: the miss is accepted once an entry frees");
    h_->DrainAll();
    Check(h_->Outstanding() == 0, "mshr-full: the table drains fully (no deadlock)");
  }

  void CoalescedCancel() {
    h_->Phase("coalesced-cancel");
    const uint32_t h = Addr(15, 5, 0);
    const uint64_t reads = h_->MemReads();
    const uint64_t responses = h_->TotalResponses();

    h_->Issue(h, 0, false, "coalesced cancel A");
    h_->Issue(h, 1, false, "coalesced cancel B");
    h_->WaitReads(reads + 1);
    Check(h_->Outstanding() == 1, "coalesced-cancel: one entry serves both consumers");

    h_->Cancel(0);
    h_->DeliverRead(0);
    h_->WaitResponses(responses + 1);
    Check(h_->Outstanding() == 0,
          "coalesced-cancel: the entry frees after the surviving consumer is answered");
    Check(h_->DebugValid(h), "coalesced-cancel: the surviving consumer installed the line");
  }

  void IdReuse() {
    h_->Phase("id-reuse");
    const uint32_t x = Addr(16, 6, 0);
    const uint32_t y = Addr(17, 7, 0);
    const uint64_t responses = h_->TotalResponses();

    h_->Issue(x, 2, false, "id reuse first");
    h_->DeliverRead(0);
    h_->WaitResponses(responses + 1);
    h_->Issue(y, 2, false, "id reuse second");
    h_->DeliverRead(0);
    h_->WaitResponses(responses + 2);
    Check(h_->Outstanding() == 0,
          "id-reuse: a completed id may be reused with no stale-response aliasing");
  }

  std::string FinalCheck() {
    h_->Phase("final");
    Check(h_->Outstanding() == 0, "final: no live entries");
    Check(h_->Waiters() == 0, "final: no live waiters");
    Check(h_->ExpEmpty(), "final: every accepted request was answered or cancelled");
    Check(h_->MissAccepted() == h_->DrainResponses() + h_->Cancels(),
          "final: conservation at rest (misses == responses + cancels)");
    Check(h_->EvHit() > 0 && h_->EvMiss() > 0 && h_->EvCoalesce() > 0 &&
          h_->EvRefill() > 0 && h_->EvFault() > 0 && h_->EvCancel() > 0,
          "coverage: hit/miss/coalesce/refill/fault/cancel all reached");
    Check(h_->MaxOutstanding() >= 4,
          "coverage: the MSHR table was filled (four misses outstanding at once)");
    Check(h_->EvDrop() >= 1, "coverage: a cancelled refill was absorbed");
    Check(h_->MemReads() == h_->EvRefill() + h_->EvFault() + h_->EvDrop(),
          "invariant: every memory read ends in a refill, a fault or an absorbed drop");

    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "mshr: hit=%llu miss=%llu coalesce=%llu refill=%llu fault=%llu "
                  "cancel=%llu reads=%llu max_outstanding=%u cycles=%llu",
                  (unsigned long long)h_->EvHit(), (unsigned long long)h_->EvMiss(),
                  (unsigned long long)h_->EvCoalesce(), (unsigned long long)h_->EvRefill(),
                  (unsigned long long)h_->EvFault(), (unsigned long long)h_->EvCancel(),
                  (unsigned long long)h_->MemReads(), h_->MaxOutstanding(),
                  (unsigned long long)h_->cycles());
    return std::string(detail);
  }

  MshrHarness* h_;
  mosaic::Reporter* rep_;
};

// ===========================================================================
// The non-blocking L1D campaign (this deliverable). The write-back L1 is now
// non-blocking: a demand that HITS is answered while a miss is outstanding, a
// demand that misses a line already in flight COALESCES onto it, and the
// outstanding-miss table holds more than one entry. The four controls in
// tools/run_memscale_controls.py each break one clause and must be caught here.
class NbCacheCampaign {
 public:
  NbCacheCampaign(Harness* h, mosaic::Reporter* rep) : h_(h), rep_(rep) {}
  void Check(bool ok, const std::string& what) { rep_->Check(ok, what); }

  std::string Run() {
    h_->Reset(4);
    ColdReset();
    RefillFault();
    DirtyEvict();
    HitDuringMiss();
    CoalesceWords();
    MultiEntry();
    return Final();
  }

 private:
  void ColdReset() {
    h_->Phase("nb-cold-reset");
    h_->CheckAllInvalid(false, "nb-cold-reset");
  }

  void RefillFault() {
    h_->Phase("nb-refill-fault");
    const uint32_t a = Addr(20, 1, 0);
    h_->Poison(LineOf(a));
    const Counters before = h_->Dc();
    const Result first = h_->Load(false, a, "poisoned");
    Check(first.fault, "nb-refill-fault: a failed refill answers with a fault");
    Check(!h_->DebugValid(false, a),
          "nb-refill-fault: a faulted refill never marks a line valid");
    Check(h_->Dc().fault == before.fault + 1, "nb-refill-fault: ev_fault pulsed");
    const Result second = h_->Load(false, a, "retry");
    Check(!second.fault, "nb-refill-fault: the retry succeeds");
    Check(h_->DebugValid(false, a), "nb-refill-fault: the retried refill installs the line");
  }

  void DirtyEvict() {
    h_->Phase("nb-dirty-evict");
    const uint32_t a = Addr(21, 2, 0);
    const uint32_t b = Addr(22, 2, 0);   // same set, different tag
    const uint64_t v = 0x1122334455667788ull;
    h_->Store(false, a, v, 0xff, "store");
    const Counters before = h_->Dc();
    (void)h_->Load(false, b, "evict");
    Check(h_->Dc().writeback == before.writeback + 1,
          "nb-dirty-evict: the dirty victim was written back");
    Check(h_->MemWord(a) == v, "nb-dirty-evict: the victim lost no byte on eviction");
  }

  void HitDuringMiss() {
    h_->Phase("nb-hit-during-miss");
    const uint32_t hot  = Addr(23, 3, 0);
    const uint32_t cold = Addr(24, 5, 0);
    const uint64_t hot_data = h_->MemWord(hot);
    (void)h_->Load(false, hot, "warm");
    // Start a miss on `cold` and hold the memory port so it stays outstanding.
    h_->StallMemory(16);
    h_->Offer(false, false, cold);
    Check(h_->TickAccepted(false), "nb-hit-during-miss: the miss is accepted");
    h_->Withdraw(false);
    Check(h_->OutstandingNow() > 0, "nb-hit-during-miss: a miss is outstanding");
    // A hit must be accepted and answered now, before the miss completes.
    h_->Offer(false, false, hot);
    const bool accepted = h_->TickAccepted(false);
    h_->Withdraw(false);
    Check(accepted, "nb-hit-during-miss: a hit is accepted while a miss is outstanding");
    bool got = false;
    for (int i = 0; i < 64 && !got; ++i) { h_->TickNow(); got = h_->RespValidNow(false); }
    Check(got, "nb-hit-during-miss: the hit is answered while the miss is still outstanding");
    Check(!h_->RespFaultNow(false) && h_->RespDataNow(false) == hot_data,
          "nb-hit-during-miss: the hit returns the resident line");
    Check(h_->OutstandingNow() > 0, "nb-hit-during-miss: an unrelated miss was still in flight");
    h_->StallMemory(0);
    bool done = false;
    for (int i = 0; i < 200 && !done; ++i) { h_->TickNow(); done = h_->RespValidNow(false); }
    Check(done, "nb-hit-during-miss: the miss eventually completes");
    Check(h_->OutstandingNow() == 0, "nb-hit-during-miss: the table is empty at rest");
  }

  void CoalesceWords() {
    h_->Phase("nb-coalesce-words");
    const uint32_t base = Addr(25, 6, 0);
    const uint64_t w0 = h_->MemWord(base + 0);
    const uint64_t w1 = h_->MemWord(base + 8);
    const uint64_t w2 = h_->MemWord(base + 16);
    h_->StallMemory(32);
    const uint64_t reads_before = h_->DcMemReads();
    h_->Offer(false, false, base + 0);
    Check(h_->TickAccepted(false), "nb-coalesce-words: first miss accepted");
    h_->Offer(false, false, base + 8);
    Check(h_->TickAccepted(false), "nb-coalesce-words: second request coalesces onto the entry");
    h_->Offer(false, false, base + 16);
    Check(h_->TickAccepted(false), "nb-coalesce-words: third request coalesces onto the entry");
    h_->Withdraw(false);
    Check(h_->OutstandingNow() == 1, "nb-coalesce-words: one line, one entry");
    h_->StallMemory(0);
    uint64_t seen[3] = {0, 0, 0};
    int n = 0;
    for (int i = 0; i < 400 && n < 3; ++i) {
      h_->TickNow();
      if (h_->RespValidNow(false)) seen[n++] = h_->RespDataNow(false);
    }
    Check(n == 3, "nb-coalesce-words: one response per coalesced request");
    Check(seen[0] == w0 && seen[1] == w1 && seen[2] == w2,
          "nb-coalesce-words: every coalesced waiter gets its own word");
    Check(h_->DcMemReads() == reads_before + 1,
          "nb-coalesce-words: a coalesced miss issues exactly one memory read");
  }

  void MultiEntry() {
    h_->Phase("nb-multi-entry");
    const uint32_t a[4] = {Addr(26, 0, 0), Addr(27, 1, 0), Addr(28, 4, 0), Addr(29, 7, 0)};
    h_->StallMemory(48);
    for (int i = 0; i < 4; ++i) {
      h_->Offer(false, false, a[i]);
      Check(h_->TickAccepted(false), "nb-multi-entry: each miss is accepted");
    }
    h_->Withdraw(false);
    Check(h_->OutstandingNow() == 4, "nb-multi-entry: four misses are outstanding at once");
    h_->StallMemory(0);
    uint64_t seen[4] = {0, 0, 0, 0};
    int n = 0;
    for (int i = 0; i < 800 && n < 4; ++i) {
      h_->TickNow();
      if (h_->RespValidNow(false)) seen[n++] = h_->RespDataNow(false);
    }
    Check(n == 4, "nb-multi-entry: four responses");
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
      std::fprintf(stderr, "DBG multi[%d] seen=%016llx exp=%016llx addr=%08x\n",
                   i, (unsigned long long)seen[i], (unsigned long long)h_->MemWord(a[i]), a[i]);
      ok = ok && (seen[i] == h_->MemWord(a[i]));
    }
    Check(ok, "nb-multi-entry: each line returns its own word");
    Check(h_->OutstandingNow() == 0, "nb-multi-entry: the table drains");
  }

  std::string Final() {
    h_->Phase("nb-final");
    Check(h_->OutstandingNow() == 0, "nb-final: no outstanding entries at rest");
    const Counters d = h_->Dc();
    Check(h_->DcMemReads() == d.refill + d.fault,
          "nb-final: every memory read is a refill or a fault");
    Check(d.hit > 0 && d.miss > 0 && d.refill > 0 && d.fault > 0 && d.writeback > 0,
          "coverage: the non-blocking L1D exercised hit/miss/refill/fault/writeback");
    Check(h_->DcCoalesce() > 0, "coverage: at least one miss coalesced");
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "nb-l1d: hit=%llu miss=%llu coalesce=%llu refill=%llu fault=%llu "
                  "wb=%llu reads=%llu cycles=%llu",
                  (unsigned long long)d.hit, (unsigned long long)d.miss,
                  (unsigned long long)h_->DcCoalesce(), (unsigned long long)d.refill,
                  (unsigned long long)d.fault, (unsigned long long)d.writeback,
                  (unsigned long long)h_->DcMemReads(),
                  (unsigned long long)h_->cycles());
    return std::string(detail);
  }

  Harness* h_;
  mosaic::Reporter* rep_;
};

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
  Vmosaic_cache_tb dut;

  bool passed = true;
  std::string detail = "campaign did not start";
  const bool mshr_case = options.case_id == "cache.mshr_nonblocking";
  const bool cache_case = options.case_id == "cache.refill_evict_fault";
  if (!mshr_case && !cache_case) {
    std::fprintf(stderr, "unknown case %s\n", options.case_id.c_str());
    return mosaic::kExitUsage;
  }

  try {
    if (mshr_case) {
      MshrHarness harness(&dut, &clk, &reporter, options.max_cycles);
      MshrCampaign campaign(&harness, &reporter);
      detail = campaign.Run();
      reporter.Check(passed, "every accepted request is answered or cancelled exactly once");
      // The same case also carries the non-blocking L1D campaign: the wrapper's
      // data cache is the write-back L1 this deliverable made non-blocking.
      Harness nb_harness(&dut, &clk, &reporter, options.max_cycles);
      NbCacheCampaign nb_campaign(&nb_harness, &reporter);
      detail += " | " + nb_campaign.Run();
    } else {
      Harness harness(&dut, &clk, &reporter, options.max_cycles);
      Campaign campaign(&harness, &reporter);
      detail = campaign.Run();
      reporter.Check(passed, "cache-on and cache-off architectural traces agree");
    }
  } catch (const Failure& f) {
    passed = false;
    detail = "first failure: " + f.what;
  }

  const bool ok = passed && reporter.failures() == 0;
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
