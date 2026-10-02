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
    if (!rst && req_valid && ready) {
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

    // Memory-side response, one cycle after a read was accepted.
    dut_->dm_resp_valid = d_pend_ ? 1 : 0;
    dut_->dm_resp_fault = d_pend_fault_ ? 1 : 0;
    AssignLine(&dut_->dm_resp_rdata, d_pend_data_);
    dut_->im_resp_valid = i_pend_ ? 1 : 0;
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
  Harness harness(&dut, &clk, &reporter, options.max_cycles);

  bool passed = true;
  std::string detail = "campaign did not start";
  try {
    Campaign campaign(&harness, &reporter);
    detail = campaign.Run();
  } catch (const Failure& f) {
    passed = false;
    detail = "first failure: " + f.what;
  }

  reporter.Check(passed, "cache-on and cache-off architectural traces agree");
  const bool ok = passed && reporter.failures() == 0;
  return reporter.Finish(ok ? "PASS" : "FAIL", detail);
}
