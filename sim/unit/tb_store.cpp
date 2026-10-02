// ============================================================================
// tb_store.cpp -- CASE=store.wrong_path_visibility, work package I-034.
//
// The DUT is never its own oracle. The shadow below is an independent model of
// the *contract* documented in rtl/core/mosaic_store_queue.sv: an ordered list
// of entries in program order, a pointer that separates authorised from
// unauthorised entries, a squash that removes a suffix of the unauthorised ones,
// and a drain that takes position 0 only when it is authorised and both its
// address and its data have been captured. It is written from that prose, not
// from the RTL's structure, and it shares no code with it.
//
// The expectation that matters most is not a signal at all: the **bytes that
// landed in the memory model**. The driver derives, from the sequence of
// allocations/commits/squashes/fills it drives, which stores *should* have
// reached memory and what they should have written there, applies them to a
// second memory of its own, and requires the model behind mosaic_lsu_endpoint to
// agree byte for byte. A wrong-path store is a store that must contribute
// exactly zero of those bytes and exactly zero of those transactions, and that
// is what the wrong-path phase counts.
//
// Geometry is read out of the elaborated DUT (`o_dut_*`), so this file contains
// no depth, no identity width and no field offset. The wrapper's own
// re-expression of the same numbers (`o_tb_*`) is required to agree field by
// field, so a profile change is a named failure here rather than a silently
// narrowed port.
//
// Phases, each of which resets first and fails on its own:
//
//   1. reset-state   the documented cold state: empty, watermark zero, nothing
//                    offered, every counter zero, no memory transaction.
//   2. wrong-path    the card's Pass criterion, first half. A store allocated on
//                    a speculated path with its address and data *ready* is
//                    execution-complete and not authorised; it must produce zero
//                    memory transactions and zero bytes, be withdrawn by the
//                    squash of its path, and the queue must still work afterwards.
//   3. flush-safety  the card's Pass criterion, second half. An authorised store
//                    that has not drained yet survives a whole-queue flush (and
//                    is counted as spared), reaches memory exactly once after it,
//                    and a second flush does not re-issue it.
//   4. in-order      two stores drain in program order; a younger store whose
//                    authorisation names it while an older one is unauthorised is
//                    refused, and nothing behind an unauthorised head drains.
//   5. capacity      the queue fills, refuses further allocations *without
//                    accepting them*, and the conservation identity
//                    (allocated == drained + squashed + resident) is asserted
//                    every cycle of every phase, not just here.
//   6. readiness     a store whose address is captured and data is not (and the
//                    reverse) never becomes visible early; the fill that
//                    completes it does.
//   7. forward       the I-035 query port, which this package defines but does
//                    not hand to anyone yet: its rule is exercised directly.
//   8. soak          random allocation, fills, commits, squashes and memory
//                    back-pressure, shadow-compared every cycle, with the
//                    coverage counters asserted at the end so a campaign that
//                    did nothing cannot pass.
//
// Standing invariants, checked on every cycle of every phase rather than in one
// place:
//
//   * `o_alloc_ctr == o_drain_ctr + o_squash_ctr + o_count`.
//   * Every exported counter equals the driver's own tally of what it observed.
//   * `alloc_ready_o` is exactly "the shadow has room".
//   * `drain_req_valid_o` is high exactly when the shadow's head is authorised,
//     captured and not withdrawn by this cycle's squash, and the payload is that
//     entry.
//   * The whole entry vector, the occupancy bitmap and the authorised bitmap
//     equal the shadow's, position by position.
//   * No store reaches the memory model that the queue did not offer, and every
//     store that reaches it carries the specification's address, strobes and
//     lane-shifted data for the store the shadow says was offered.
// ============================================================================

#include <verilated.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_store_queue_tb.h"

namespace {

// ------------------------------------------------------------------- failure
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

void Require(bool condition, const std::string& where, const std::string& detail) {
  if (!condition) Fail(where, detail);
}

std::string Bool(bool value) { return value ? "1" : "0"; }
std::string Dec(uint64_t value) { return std::to_string(value); }
std::string U64(uint64_t value) { return mosaic::Hex(value, 16); }
std::string U32(uint32_t value) { return mosaic::Hex(value, 8); }

// ---------------------------------------------------------- the specification
// The load/store rules, written here and not read out of any RTL: a size code
// covers a number of bytes, an access is misaligned when it crosses the boundary
// that size demands, and a store's strobes and payload are the size mask and the
// store operand shifted to the addressed lane.
uint32_t SizeBytes(uint32_t size) {
  switch (size) {
    case 0:
      return 1;
    case 1:
      return 2;
    case 2:
      return 4;
    default:
      return 8;
  }
}

bool SpecMisaligned(uint64_t addr, uint32_t size) {
  return (addr & (uint64_t(SizeBytes(size)) - 1u)) != 0;
}

uint32_t SpecWstrb(uint32_t size, uint64_t addr) {
  const uint32_t mask = (1u << SizeBytes(size)) - 1u;
  return uint32_t((mask << (addr & 7u)) & 0xffu);
}

uint64_t SpecWdata(uint64_t store_data, uint64_t addr) {
  return store_data << (8u * (addr & 7u));
}

// The exception codes the endpoint reports for a store, from mosaic_pkg.
constexpr uint64_t kExcStoreMisaligned = 6;
constexpr uint64_t kExcStoreAccess = 7;

// --------------------------------------------------------------- the identity
// One instruction's identity, kept as fields so the driver can compute the
// squash window against it, plus the packed word it actually drives.
struct Ident {
  uint32_t hart = 0;
  uint32_t rob_index = 0;
  uint32_t rob_gen = 0;
  uint32_t uop_index = 0;
};

std::string DescribeIdent(const Ident& id) {
  return "{h" + Dec(id.hart) + ",i" + Dec(id.rob_index) + ",g" + Dec(id.rob_gen) + ",u" +
         Dec(id.uop_index) + "}";
}

// ---------------------------------------------------------------- the entry
struct Entry {
  Ident id;
  uint32_t packed = 0;
  uint64_t base = 0;
  uint64_t imm = 0;
  uint64_t data = 0;
  uint32_t size = 3;
  bool addr_valid = false;
  bool data_valid = false;
  bool authorised = false;

  uint64_t addr() const { return base + imm; }

  // The four facts the queue records, in the order the observation port lays
  // them out. Two entries are the same store only if all of them agree.
  std::string str() const {
    return "{i" + Dec(packed) + ",a0x" + U64(addr()) + ",s" + Dec(size) + ",d0x" + U64(data) +
           ",av" + Bool(addr_valid) + ",dv" + Bool(data_valid) + ",auth" + Bool(authorised) +
           "}";
  }
};

// ------------------------------------------------------------------ geometry
struct Geom {
  uint32_t entries = 0;
  uint32_t entry_w = 0;
  uint32_t cnt_w = 0;
  uint32_t idx_w = 0;
  uint32_t xlen = 0;
  uint32_t id_w = 0;
  uint32_t rob_index_w = 0;
  uint32_t rob_gen_w = 0;
  uint32_t rob_slots = 0;
  uint32_t id_mask = 0;
  uint32_t rob_index_mask = 0;
  uint32_t rob_gen_mask = 0;

  // Observation layout, least significant field first, as the RTL header
  // documents it. `entry_w` is checked against the sum once, at startup.
  int off_data_valid = 0;
  int off_addr_valid = 0;
  int off_data = 0;
  int off_size = 0;
  int off_imm = 0;
  int off_base = 0;
  int off_id = 0;
  int off_entry = 0;
};

void DeriveOffsets(Geom* g) {
  g->off_data_valid = 0;
  g->off_addr_valid = g->off_data_valid + 1;
  g->off_data = g->off_addr_valid + 1;
  g->off_size = g->off_data + int(g->xlen);
  g->off_imm = g->off_size + 3;
  g->off_base = g->off_imm + int(g->xlen);
  g->off_id = g->off_base + int(g->xlen);
  g->off_entry = g->off_id + int(g->id_w);
}

template <std::size_t N>
std::vector<uint32_t> WordsOf(const VlWide<N>& value) {
  std::vector<uint32_t> out(N);
  for (std::size_t i = 0; i < N; i++) out[i] = value[i];
  return out;
}

// The observation word, decoded back into the fields the shadow reasons about.
class Codec {
 public:
  explicit Codec(const Geom& g) : g_(g) {}

  uint64_t Bits(const std::vector<uint32_t>& w, int off, int width) const {
    uint64_t value = 0;
    for (int b = 0; b < width; b++) {
      const int bit = off + b;
      const size_t word = size_t(bit) / 32u;
      value |= uint64_t((w[word] >> (uint32_t(bit) % 32u)) & 1u) << b;
    }
    return value;
  }

  Entry Get(const std::vector<uint32_t>& w, int base) const {
    Entry e;
    e.data_valid = Bits(w, base + g_.off_data_valid, 1) != 0;
    e.addr_valid = Bits(w, base + g_.off_addr_valid, 1) != 0;
    e.data = Bits(w, base + g_.off_data, int(g_.xlen));
    e.size = uint32_t(Bits(w, base + g_.off_size, 3));
    e.imm = Bits(w, base + g_.off_imm, int(g_.xlen));
    e.base = Bits(w, base + g_.off_base, int(g_.xlen));
    e.packed = uint32_t(Bits(w, base + g_.off_id, int(g_.id_w)));
    return e;
  }

 private:
  Geom g_;
};

// ID 64-bit packing is not needed anywhere; the 32-bit word is the widest the
// driver ports carry and `o_id_w` is checked to fit it at startup.

// ------------------------------------------------------------ memory model
// A byte-addressed region behind the endpoint's memory port. It has a
// programmable request latency and request-side back-pressure. It does **not**
// check alignment: the profile's trap policy lives in mosaic_lsu_endpoint, and a
// model that trapped misaligned accesses would be a second implementation of the
// thing under test.
class MemoryModel {
 public:
  MemoryModel() : mem_(kRegionBytes) { PatternFill(); }

  void PatternFill() {
    for (uint64_t i = 0; i < kRegionBytes; i++) {
      mem_[i] = uint8_t((i * 0x9dull + 0x37ull) & 0xffull);
    }
  }

  bool InRegionByte(uint64_t addr) const {
    return addr >= kRegionBase && (addr - kRegionBase) < kRegionBytes;
  }
  bool InAccess(uint64_t addr, uint32_t size) const {
    if (addr < kRegionBase) return false;
    const uint64_t offset = addr - kRegionBase;
    return offset + size <= kRegionBytes;
  }

  uint8_t Peek(uint64_t addr) const {
    return InRegionByte(addr) ? mem_[addr - kRegionBase] : uint8_t(0);
  }
  void Poke(uint64_t addr, uint8_t value) {
    if (InRegionByte(addr)) mem_[addr - kRegionBase] = value;
  }

  void SetLatency(uint32_t cycles) { latency_ = cycles; }
  void SetNextFault(bool fault) { next_fault_ = fault; }
  // A fresh memory: the pattern back, nothing in flight, and the counters the
  // driver compares against back to zero (they are per phase, like the driver's
  // own tallies).
  void Reset() {
    PatternFill();
    pending_ = false;
    delay_ = 0;
    next_fault_ = false;
    accepted_ = 0;
    last_fault_ = false;
    last_consume_cycle_ = 0;
    request_log_.clear();
  }
  void FlushPending() {
    pending_ = false;
    delay_ = 0;
  }
  bool ReqReady(bool program_ready) const { return program_ready && !pending_; }
  bool RspValid() const { return pending_ && delay_ == 0; }
  uint64_t RspRdata() const { return pending_ ? pending_rdata_ : 0; }
  bool RspFault() const { return pending_ && pending_fault_; }

  struct EdgeResult {
    bool accepted = false;
    bool consumed = false;
  };

  // Called once per cycle with what the DUT presented *before* the edge. The
  // write uses the lane convention the endpoint documents: the byte at address
  // `a` is lane `a[2:0]` of the payload.
  EdgeResult Edge(uint64_t cycle, bool mem_valid, bool program_ready, bool we, uint64_t addr,
                  uint32_t size_bytes, uint64_t wdata, bool rsp_ready) {
    EdgeResult result;
    if (!pending_ && mem_valid && program_ready) {
      result.accepted = true;
      accepted_++;
      request_log_.push_back(addr);
      const bool fault = next_fault_ || !InAccess(addr, size_bytes);
      next_fault_ = false;
      if (we) {
        for (uint32_t i = 0; i < size_bytes; i++) {
          const uint32_t lane = uint32_t((addr + i) & 7u);
          Poke(addr + i, uint8_t(wdata >> (8u * lane)));
        }
      }
      pending_ = true;
      pending_fault_ = fault;
      pending_rdata_ = InAccess(addr, size_bytes) ? LaneAlignedRead(addr) : 0;
      delay_ = latency_;
      return result;
    }
    if (pending_) {
      if (delay_ > 0) {
        delay_--;
      } else if (RspValid() && rsp_ready) {
        pending_ = false;
        last_fault_ = pending_fault_;
        last_consume_cycle_ = cycle;
        result.consumed = true;
      }
    }
    return result;
  }

  uint64_t accepted() const { return accepted_; }
  const std::vector<uint64_t>& request_log() const { return request_log_; }
  bool busy() const { return pending_; }

 private:
  uint64_t LaneAlignedRead(uint64_t addr) const {
    const uint64_t block = addr & ~7ull;
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; i++) value |= uint64_t(Peek(block + i)) << (8u * i);
    return value;
  }

  static constexpr uint64_t kRegionBase = 0x80000000ull;
  static constexpr uint64_t kRegionBytes = 0x10000;

  std::vector<uint8_t> mem_;
  bool pending_ = false;
  uint32_t delay_ = 0;
  uint64_t pending_rdata_ = 0;
  bool pending_fault_ = false;
  uint32_t latency_ = 0;
  bool next_fault_ = false;
  uint64_t accepted_ = 0;
  uint64_t last_consume_cycle_ = 0;
  bool last_fault_ = false;
  std::vector<uint64_t> request_log_;
};

// ------------------------------------------------------------------- shadow
// The queue as the contract describes it. Written from the module header's
// "order and authorisation", "squash rule" and "drain" sections.
class Shadow {
 public:
  void Configure(const Geom& g) { g_ = g; }
  void Reset() { e_.clear(); }

  size_t size() const { return e_.size(); }
  bool empty() const { return e_.empty(); }
  bool full() const { return e_.size() >= g_.entries; }
  bool AllocReady() const { return e_.size() < g_.entries; }
  const std::vector<Entry>& entries() const { return e_; }
  const Entry& at(size_t i) const { return e_[i]; }

  // Authorised entries are always a prefix; the driver checks that the DUT's
  // bitmap agrees with this and that the shadow's own flags are a prefix, which
  // is the watermark's invariant stated as an assertion on the model itself.
  size_t AuthCount() const {
    size_t n = 0;
    while (n < e_.size() && e_[n].authorised) n++;
    return n;
  }

  void Alloc(const Entry& e) {
    Entry copy = e;
    copy.authorised = false;
    e_.push_back(copy);
  }

  // The oldest resident entry carrying `packed`, which is the one a fill or an
  // authorisation names.
  bool FindOldest(uint32_t packed, size_t* idx) const {
    for (size_t i = 0; i < e_.size(); i++) {
      if (e_[i].packed == packed) {
        *idx = i;
        return true;
      }
    }
    return false;
  }

  bool CommitOk(uint32_t packed) const {
    size_t idx = 0;
    if (!FindOldest(packed, &idx)) return false;
    return idx == AuthCount();
  }

  void Commit(uint32_t packed) {
    size_t idx = 0;
    if (!FindOldest(packed, &idx)) return;
    // The contract authorises the entry only when it is the next one; the
    // driver only calls this after predicting CommitOk, and this guard keeps the
    // model honest even if a caller did not.
    if (idx != AuthCount()) return;
    e_[idx].authorised = true;
  }

  // The squash region, as the RTL header states it: the same generation and an
  // index inside [from, tail) in the ROB's own modulus.
  bool InRegion(const Entry& e, uint32_t from, uint32_t tail, uint32_t gen) const {
    if (e.id.rob_gen != gen) return false;
    const uint32_t slots = g_.rob_slots;
    const uint32_t dist = (e.id.rob_index - from) & (slots - 1u);
    const uint32_t len = (tail - from) & (slots - 1u);
    return dist < len;
  }

  // What a squash removes and what it spares. A full flush takes every
  // unauthorised entry; an authorised one is spared in both cases, because it
  // has retired and is past the point recovery may take it back.
  void SquashEffect(bool all, uint32_t from, uint32_t tail, uint32_t gen,
                    std::vector<bool>* drop, std::vector<bool>* spared) const {
    drop->assign(e_.size(), false);
    spared->assign(e_.size(), false);
    for (size_t i = 0; i < e_.size(); i++) {
      const bool region = all || InRegion(e_[i], from, tail, gen);
      if (!region) continue;
      if (e_[i].authorised) {
        (*spared)[i] = true;
      } else {
        (*drop)[i] = true;
      }
    }
  }

  void ApplyDrop(const std::vector<bool>& drop) {
    std::vector<Entry> keep;
    keep.reserve(e_.size());
    for (size_t i = 0; i < e_.size(); i++) {
      if (!drop[i]) keep.push_back(e_[i]);
    }
    e_ = keep;
  }

  // The drain offer: position 0, authorised, both facts captured, and not
  // withdrawn by this cycle's squash.
  bool PredictOffer(const std::vector<bool>& drop, Entry* head) const {
    if (e_.empty()) return false;
    if (!e_[0].authorised) return false;
    if (!e_[0].addr_valid || !e_[0].data_valid) return false;
    if (drop[0]) return false;
    if (head != nullptr) *head = e_[0];
    return true;
  }

  void RemoveHead() {
    if (!e_.empty()) e_.erase(e_.begin());
  }

  void Fill(size_t idx, bool addr_valid, uint64_t base, uint64_t imm, bool data_valid,
            uint64_t data) {
    if (addr_valid) {
      e_[idx].base = base;
      e_[idx].imm = imm;
    }
    if (data_valid) e_[idx].data = data;
    e_[idx].addr_valid = e_[idx].addr_valid || addr_valid;
    e_[idx].data_valid = e_[idx].data_valid || data_valid;
  }

  // The forwarding query, exactly as the RTL header states the rule: the
  // youngest resident entry with both facts captured that fully contains the
  // query's byte range in the doubleword the address selects. `blocked` is the
  // conservative "some resident store has no address" flag.
  static bool Covers(const Entry& e, uint64_t q, uint32_t qsize) {
    const uint64_t ea = e.addr();
    if ((ea >> 3) != (q >> 3)) return false;
    const uint32_t e_lo = uint32_t(ea & 7u);
    const uint32_t q_lo = uint32_t(q & 7u);
    return e_lo <= q_lo && (q_lo + SizeBytes(qsize)) <= (e_lo + SizeBytes(e.size));
  }

  void Forward(uint64_t q, uint32_t qsize, bool* found, uint64_t* data, bool* blocked) const {
    *found = false;
    *data = 0;
    *blocked = false;
    for (size_t i = 0; i < e_.size(); i++) {
      if (!e_[i].addr_valid) {
        *blocked = true;
        continue;
      }
      if (!e_[i].data_valid) continue;
      if (Covers(e_[i], q, qsize)) {
        *found = true;
        *data = e_[i].data << (8u * (e_[i].addr() & 7u));
      }
    }
  }

  const Geom& geom() const { return g_; }

 private:
  Geom g_;
  std::vector<Entry> e_;
};

// ------------------------------------------------------------------ stimulus
struct Stim {
  bool alloc_valid = false;
  uint32_t alloc_id = 0;
  Ident alloc_ident;
  uint64_t alloc_base = 0;
  uint64_t alloc_imm = 0;
  uint32_t alloc_size = 3;
  bool alloc_addr_valid = false;
  uint64_t alloc_data = 0;
  bool alloc_data_valid = false;

  bool fill_valid = false;
  uint32_t fill_id = 0;
  uint64_t fill_base = 0;
  uint64_t fill_imm = 0;
  bool fill_addr_valid = false;
  uint64_t fill_data = 0;
  bool fill_data_valid = false;

  bool commit_valid = false;
  uint32_t commit_id = 0;

  bool squash_valid = false;
  bool squash_all = false;
  uint32_t squash_from = 0;
  uint32_t squash_tail = 0;
  uint32_t squash_gen = 0;

  bool mem_req_ready = true;
  uint32_t latency = 0;

  uint64_t fwd_addr = 0;
  uint32_t fwd_size = 0;
};

// The pre-edge snapshot of every DUT output the case reads.
struct DutOut {
  bool alloc_ready = false;
  bool fill_hit = false;
  bool fill_stale = false;
  bool commit_ok = false;
  bool commit_stale = false;

  bool drain_valid = false;
  bool drain_ready = false;
  uint32_t drain_id = 0;
  uint64_t drain_base = 0;
  uint64_t drain_imm = 0;
  uint32_t drain_size = 0;
  uint64_t drain_data = 0;

  bool mem_req_valid = false;
  bool mem_req_ready = false;
  bool mem_req_we = false;
  uint64_t mem_req_addr = 0;
  uint32_t mem_req_size = 0;
  uint32_t mem_req_wstrb = 0;
  uint64_t mem_req_wdata = 0;
  bool mem_rsp_ready = false;

  bool fwd_valid = false;
  uint64_t fwd_data = 0;
  bool fwd_blocked = false;

  uint32_t count = 0;
  uint32_t auth_cnt = 0;
  uint32_t occ = 0;
  uint32_t entry_auth = 0;
  uint32_t alloc_ctr = 0;
  uint32_t fill_ctr = 0;
  uint32_t fill_stale_ctr = 0;
  uint32_t commit_ctr = 0;
  uint32_t commit_stale_ctr = 0;
  uint32_t drain_ctr = 0;
  uint32_t fault_ctr = 0;
  uint32_t squash_ctr = 0;
  uint32_t squash_spared_ctr = 0;
  uint64_t last_fault_tval = 0;
  uint64_t last_fault_cause = 0;
  uint32_t last_fault_id = 0;

  uint32_t ep_txn = 0;
  uint32_t ep_store = 0;
  uint32_t ep_misaligned = 0;
  uint32_t ep_rsp = 0;
  bool ep_busy = false;

  std::vector<Entry> entries;
};

// -------------------------------------------------------------------- bench
class Bench {
 public:
  Bench(Vmosaic_store_queue_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  uint64_t checks() const { return checks_; }
  uint64_t cycles() const { return clk_->cycle(); }
  const DutOut& seen() const { return last_; }
  uint64_t accepted_allocs() const { return alloc_seen_; }
  uint64_t accepted_drains() const { return drain_seen_; }
  uint64_t squashed() const { return squash_seen_; }
  uint64_t faults() const { return fault_seen_; }
  uint64_t memory_txns() const { return expected_txn_; }
  const MemoryModel& mem() const { return mem_; }
  const Shadow& shadow() const { return shadow_; }

  // ------------------------------------------------------------- geometry
  void ReadGeometry() {
    Geom g;
    g.entries = dut_->o_dut_entries;
    g.entry_w = dut_->o_dut_entry_w;
    g.cnt_w = dut_->o_dut_cnt_w;
    g.idx_w = dut_->o_dut_idx_w;
    g.xlen = dut_->o_dut_xlen;
    g.id_w = dut_->o_dut_id_w;
    g.rob_index_w = dut_->o_dut_rob_index_w;
    g.rob_gen_w = dut_->o_dut_rob_gen_w;
    Require(dut_->o_tb_entries == g.entries, "geometry",
            "wrapper entries (" + Dec(dut_->o_tb_entries) + ") != DUT entries (" +
                Dec(g.entries) + ")");
    Require(dut_->o_tb_entry_w == g.entry_w, "geometry",
            "wrapper entry width (" + Dec(dut_->o_tb_entry_w) + ") != DUT (" +
                Dec(g.entry_w) + ")");
    Require(dut_->o_tb_cnt_w == g.cnt_w, "geometry",
            "wrapper count width (" + Dec(dut_->o_tb_cnt_w) + ") != DUT (" + Dec(g.cnt_w) + ")");
    Require(dut_->o_tb_idx_w == g.idx_w, "geometry",
            "wrapper index width (" + Dec(dut_->o_tb_idx_w) + ") != DUT (" + Dec(g.idx_w) + ")");
    Require(dut_->o_tb_xlen == g.xlen, "geometry",
            "wrapper XLEN (" + Dec(dut_->o_tb_xlen) + ") != DUT (" + Dec(g.xlen) + ")");
    Require(dut_->o_tb_id_w == g.id_w, "geometry",
            "wrapper identity width (" + Dec(dut_->o_tb_id_w) + ") != DUT (" + Dec(g.id_w) + ")");
    Require(dut_->o_tb_rob_index_w == g.rob_index_w, "geometry",
            "wrapper ROB index width (" + Dec(dut_->o_tb_rob_index_w) + ") != DUT (" +
                Dec(g.rob_index_w) + ")");
    Require(dut_->o_tb_rob_gen_w == g.rob_gen_w, "geometry",
            "wrapper ROB generation width (" + Dec(dut_->o_tb_rob_gen_w) + ") != DUT (" +
                Dec(g.rob_gen_w) + ")");

    Require(g.xlen == 64, "geometry", "XLEN is " + Dec(g.xlen));
    Require(g.entries >= 2, "geometry", "the queue has " + Dec(g.entries) + " entry/entries");
    Require(g.id_w <= 32, "geometry", "the identity is wider than the driver port");
    Require(g.rob_index_w >= 2, "geometry", "the ROB index is " + Dec(g.rob_index_w) + " bit(s)");

    g.rob_slots = 1u << g.rob_index_w;
    g.id_mask = (1u << g.id_w) - 1u;
    g.rob_index_mask = (1u << g.rob_index_w) - 1u;
    g.rob_gen_mask = (1u << g.rob_gen_w) - 1u;
    DeriveOffsets(&g);
    Require(uint32_t(g.off_entry) == g.entry_w, "geometry",
            "the documented entry layout sums to " + Dec(uint64_t(g.off_entry)) +
                ", the DUT reports " + Dec(g.entry_w));

    geometry_ = g;
    shadow_.Configure(g);
    codec_ = Codec(g);
  }

  const Geom& geom() const { return geometry_; }

  // An identity, packed the way mosaic_id_pkg's macro_id_t is declared:
  // hart, rob_index, rob_gen, uop_index, hart most significant.
  uint32_t MakeId(const Ident& id) const {
    const Geom& g = geometry_;
    uint32_t value = id.uop_index & ((1u << 3) - 1u);
    value |= (id.rob_gen & g.rob_gen_mask) << 3;
    value |= (id.rob_index & g.rob_index_mask) << (3 + g.rob_gen_w);
    value |= (id.hart & 0x1u) << (3 + g.rob_gen_w + g.rob_index_w);
    return value & g.id_mask;
  }

  // ---------------------------------------------------------------- checks
  void Check(bool ok, const std::string& where, const std::string& detail) {
    checks_++;
    phase_checks_++;
    if (!ok) {
      Fail("[" + phase_name_ + "] " + where, detail + " @ cycle " + Dec(cycle_));
    }
  }

  void Require(bool ok, const std::string& where, const std::string& detail) {
    Check(ok, where, detail);
  }

  void Phase(const std::string& name) {
    phase_name_ = name;
    phase_checks_ = 0;
    std::printf("PHASE %-22s start @cycle %llu\n", name.c_str(),
                static_cast<unsigned long long>(clk_->cycle()));
  }

  // A fresh queue and a fresh memory: reset, then let it settle with nothing
  // driven. Everything the driver tracks is reset here so a phase cannot inherit
  // a previous phase's tallies.
  void Fresh() {
    shadow_.Reset();
    sent_.clear();
    sent_mem_.clear();
    alloc_seen_ = 0;
    fill_seen_ = 0;
    fill_stale_seen_ = 0;
    commit_seen_ = 0;
    commit_stale_seen_ = 0;
    drain_seen_ = 0;
    fault_seen_ = 0;
    squash_seen_ = 0;
    squash_spared_seen_ = 0;
    expected_txn_ = 0;
    ep_rsp_seen_ = 0;
    exp_mem_.assign(kShadowBytes, 0);
    for (uint64_t i = 0; i < kShadowBytes; i++) exp_mem_[i] = uint8_t((i * 0x9dull + 0x37ull) & 0xffull);
    mem_.Reset();
    mem_.SetLatency(0);
    const Stim idle;
    for (int i = 0; i < 2; i++) Cycle(idle, true);
    for (int i = 0; i < 2; i++) Cycle(idle, false);
  }

  void EndPhase() {
    const Stim idle;
    Cycle(idle, false);
    Cycle(idle, false);
    Require(sent_.empty(), "phase-end", "a store is still in flight inside the endpoint");
    Require(sent_mem_.empty(), "phase-end", "the in-flight store bookkeeping is not empty");
    run_allocs_ += alloc_seen_;
    run_drains_ += drain_seen_;
    run_squashes_ += squash_seen_;
    run_faults_ += fault_seen_;
    std::printf("PHASE %-22s end   checks=%llu cycles=%llu allocs=%llu drains=%llu "
                "squashes=%llu faults=%llu txns=%llu\n",
                phase_name_.c_str(), static_cast<unsigned long long>(phase_checks_),
                static_cast<unsigned long long>(clk_->cycle()),
                static_cast<unsigned long long>(alloc_seen_),
                static_cast<unsigned long long>(drain_seen_),
                static_cast<unsigned long long>(squash_seen_),
                static_cast<unsigned long long>(fault_seen_),
                static_cast<unsigned long long>(expected_txn_));
  }

  // The memory model's contents against the driver's own expectation, byte for
  // byte, plus the transaction count. This is the check that makes "zero writes
  // on a wrong path" mean something: the expectation is built from the stores
  // the shadow says should have been drained, not from the model's own log.
  void CheckMemory() {
    Require(mem_.accepted() == expected_txn_, "memory-txns",
            "the memory accepted " + Dec(mem_.accepted()) + " transaction(s), the driver " +
                "expected " + Dec(expected_txn_));
    uint64_t bad = 0;
    uint64_t first_bad = 0;
    for (uint64_t i = 0; i < kShadowBytes; i++) {
      const uint64_t addr = kShadowBase + i;
      if (mem_.Peek(addr) != exp_mem_[i]) {
        if (bad == 0) first_bad = addr;
        bad++;
      }
    }
    // The message is built unconditionally, so its indices must be safe when
    // there is no mismatch: a first_bad of zero would otherwise index the
    // expectation at `0 - kShadowBase`.
    const uint64_t shown = (bad == 0) ? kShadowBase : first_bad;
    const uint64_t shown_idx = shown - kShadowBase;
    Require(bad == 0, "memory-bytes",
            Dec(bad) + " byte(s) differ from the expected memory, first at 0x" + U64(shown) +
                ": memory 0x" + mosaic::Hex(mem_.Peek(shown), 2) + ", expected 0x" +
                mosaic::Hex(exp_mem_[shown_idx], 2));
  }

  // The bytes the model should hold, restricted to what has been driven: used by
  // the wrong-path phase to state "and nothing at all was written" directly.
  bool MemoryUntouched() const {
    for (uint64_t i = 0; i < kShadowBytes; i++) {
      if (mem_.Peek(kShadowBase + i) != exp_mem_[i]) return false;
    }
    return true;
  }

  // ---------------------------------------------------------------- helpers
  void RunIdle(int n) {
    for (int i = 0; i < n; i++) Cycle(Stim(), false);
  }

  // Idle cycles with the memory's response delayed, so the endpoint is busy and a
  // store that would otherwise leave stays where it is. Stalling only the request
  // side is not enough: the response the model already owes is still presented,
  // and the endpoint finishes and frees itself.
  void RunIdleSlow(int n, uint32_t latency) {
    for (int i = 0; i < n; i++) {
      Stim s;
      s.latency = latency;
      Cycle(s, false);
    }
  }

  // Run with the memory side responsive until the queue and the endpoint are
  // both empty. Every store has then been drained and answered for, which is the
  // only state in which the memory comparison means "this is all of it".
  void Settle(int max_cycles) {
    for (int i = 0; i < max_cycles; i++) {
      Cycle(Stim(), false);
      if (shadow_.empty() && sent_.empty()) return;
    }
    Fail("settle", "the queue did not settle within " + Dec(uint64_t(max_cycles)) +
                       " cycles (shadow holds " + Dec(shadow_.size()) + ", " +
                       Dec(sent_.size()) + " in flight)");
  }

  // Offer an allocation until the queue takes it. The per-cycle checks have
  // already compared `alloc_ready_o` against the shadow, so a refusal here means
  // the shadow says full as well; the guard turns a stuck queue into a failure
  // rather than a hang.
  void AllocStore(const Entry& e, const Ident& id) {
    const uint32_t packed = MakeId(id);
    for (int guard = 0; guard < 4 * int(geometry_.entries) + 8; guard++) {
      Stim s;
      s.alloc_valid = true;
      s.alloc_id = packed;
      s.alloc_ident = id;
      s.alloc_base = e.base;
      s.alloc_imm = e.imm;
      s.alloc_size = e.size;
      s.alloc_addr_valid = e.addr_valid;
      s.alloc_data = e.data;
      s.alloc_data_valid = e.data_valid;
      Cycle(s, false);
      if (seen().alloc_ready) {
        Require(shadow_.size() > 0, "alloc", "the queue accepted an allocation the shadow dropped");
        const Entry& stored = shadow_.at(shadow_.size() - 1);
        Require(stored.base == e.base && stored.imm == e.imm && stored.size == e.size &&
                    stored.addr_valid == e.addr_valid && stored.data_valid == e.data_valid,
                "alloc", "the shadow stored a different entry than the one offered: " +
                              stored.str());
        return;
      }
    }
    Fail("alloc", "the queue refused " + DescribeIdent(id) + " for " +
                      Dec(4 * uint64_t(geometry_.entries) + 8) + " cycles");
  }

  void FillEntry(bool addr_valid, uint64_t base, uint64_t imm, bool data_valid, uint64_t data,
                 const Ident& id) {
    FillEntryPacked(MakeId(id), addr_valid, base, imm, data_valid, data);
  }

  void FillEntryPacked(uint32_t packed, bool addr_valid, uint64_t base, uint64_t imm,
                       bool data_valid, uint64_t data) {
    Stim s;
    s.fill_valid = true;
    s.fill_id = packed & geometry_.id_mask;
    s.fill_base = base;
    s.fill_imm = imm;
    s.fill_addr_valid = addr_valid;
    s.fill_data = data;
    s.fill_data_valid = data_valid;
    Cycle(s, false);
  }

  // One cycle with an authorisation. `expect_ok` is the driver's own prediction:
  // the per-cycle check compares the DUT against the shadow, and this states
  // what the phase requires.
  // The commit port carries the packed identity word, which is what the caller
  // has at hand (either from MakeId or from an observed entry).
  // `latency` is the memory response delay driven in the same cycle. It matters
  // because a store becomes offerable at this cycle's edge, and the endpoint can
  // accept it in this very cycle: a phase that wants the endpoint busy from the
  // start must make the cycle that authorises the store slow as well.
  void CommitStore(uint32_t packed, bool expect_ok, uint32_t latency = 0) {
    Stim s;
    s.commit_valid = true;
    s.commit_id = packed & geometry_.id_mask;
    s.latency = latency;
    Cycle(s, false);
    Require(seen().commit_ok == expect_ok, "commit-outcome",
            "the authorisation of " + U32(packed) + " was " +
                (seen().commit_ok ? "accepted" : "refused") + ", expected " +
                (expect_ok ? "accepted" : "refused"));
  }

  void SquashAll(uint32_t gen) {
    Stim s;
    s.squash_valid = true;
    s.squash_all = true;
    s.squash_gen = gen;
    Cycle(s, false);
  }

  void SquashWindow(uint32_t from, uint32_t tail, uint32_t gen) {
    Stim s;
    s.squash_valid = true;
    s.squash_all = false;
    s.squash_from = from;
    s.squash_tail = tail;
    s.squash_gen = gen;
    Cycle(s, false);
  }

  // --------------------------------------------------------------- cycles
  void Cycle(const Stim& s, bool rst) {
    cycle_ = clk_->cycle();
    Require(clk_->cycle() <= max_cycles_, "cycle-budget",
            "the run exceeded --max-cycles");

    dut_->rst = rst ? 1 : 0;
    dut_->alloc_valid = s.alloc_valid ? 1 : 0;
    dut_->alloc_id = s.alloc_id & geometry_.id_mask;
    dut_->alloc_base = s.alloc_base;
    dut_->alloc_imm = s.alloc_imm;
    dut_->alloc_size = s.alloc_size;
    dut_->alloc_addr_valid = s.alloc_addr_valid ? 1 : 0;
    dut_->alloc_data = s.alloc_data;
    dut_->alloc_data_valid = s.alloc_data_valid ? 1 : 0;

    dut_->fill_valid = s.fill_valid ? 1 : 0;
    dut_->fill_id = s.fill_id & geometry_.id_mask;
    dut_->fill_base = s.fill_base;
    dut_->fill_imm = s.fill_imm;
    dut_->fill_addr_valid = s.fill_addr_valid ? 1 : 0;
    dut_->fill_data = s.fill_data;
    dut_->fill_data_valid = s.fill_data_valid ? 1 : 0;

    dut_->commit_valid = s.commit_valid ? 1 : 0;
    dut_->commit_id = s.commit_id & geometry_.id_mask;

    dut_->squash_valid = s.squash_valid ? 1 : 0;
    dut_->squash_all = s.squash_all ? 1 : 0;
    dut_->squash_from_index = s.squash_from;
    dut_->squash_tail_index = s.squash_tail;
    dut_->squash_gen = s.squash_gen;

    mem_.SetLatency(s.latency);
    dut_->mem_req_ready = mem_.ReqReady(s.mem_req_ready) ? 1 : 0;
    dut_->mem_rsp_valid = mem_.RspValid() ? 1 : 0;
    dut_->mem_rsp_rdata = mem_.RspRdata();
    dut_->mem_rsp_fault = mem_.RspFault() ? 1 : 0;

    dut_->fwd_query_addr = s.fwd_addr;
    dut_->fwd_query_size = s.fwd_size;

    // The pre-edge snapshot: one read, used by the checks and by the shadow
    // update alike. Nothing is re-read after the edge.
    dut_->clk = 0;
    dut_->eval();
    last_ = Read();

    // The squash this cycle, computed from the contract's region rule against
    // the shadow's *pre-edge* contents. Both the checks and the update use it.
    if (s.squash_valid) {
      shadow_.SquashEffect(s.squash_all, s.squash_from, s.squash_tail, s.squash_gen, &cur_drop_,
                           &cur_spared_);
    } else {
      cur_drop_.assign(shadow_.size(), false);
      cur_spared_.assign(shadow_.size(), false);
    }

    if (rst) {
      Commit(last_, s, true);
      dut_->clk = 1;
      dut_->eval();
      dut_->clk = 0;
      dut_->eval();
      clk_->Tick();
      return;
    }

    CheckCycle(last_, s);
    Commit(last_, s, false);

    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    clk_->Tick();
    cycle_ = clk_->cycle();
  }

 private:
  // ------------------------------------------------------------- readback
  DutOut Read() {
    DutOut o;
    o.alloc_ready = dut_->alloc_ready != 0;
    o.fill_hit = dut_->fill_hit != 0;
    o.fill_stale = dut_->fill_stale != 0;
    o.commit_ok = dut_->commit_ok != 0;
    o.commit_stale = dut_->commit_stale != 0;

    o.drain_valid = dut_->drain_req_valid != 0;
    o.drain_ready = dut_->drain_req_ready != 0;
    o.drain_id = dut_->drain_req_id & geometry_.id_mask;
    o.drain_base = dut_->drain_req_base;
    o.drain_imm = dut_->drain_req_imm;
    o.drain_size = dut_->drain_req_size;
    o.drain_data = dut_->drain_req_data;

    o.mem_req_valid = dut_->mem_req_valid != 0;
    o.mem_req_ready = dut_->mem_req_ready != 0;
    o.mem_req_we = dut_->mem_req_we != 0;
    o.mem_req_addr = dut_->mem_req_addr;
    o.mem_req_size = dut_->mem_req_size;
    o.mem_req_wstrb = dut_->mem_req_wstrb;
    o.mem_req_wdata = dut_->mem_req_wdata;
    o.mem_rsp_ready = dut_->mem_rsp_ready != 0;

    o.fwd_valid = dut_->fwd_valid != 0;
    o.fwd_data = dut_->fwd_data;
    o.fwd_blocked = dut_->fwd_blocked != 0;

    o.count = dut_->o_count;
    o.auth_cnt = dut_->o_auth_cnt;
    o.occ = dut_->o_occ;
    o.entry_auth = dut_->o_entry_authorised;
    o.alloc_ctr = dut_->o_alloc_ctr;
    o.fill_ctr = dut_->o_fill_ctr;
    o.fill_stale_ctr = dut_->o_fill_stale_ctr;
    o.commit_ctr = dut_->o_commit_ctr;
    o.commit_stale_ctr = dut_->o_commit_stale_ctr;
    o.drain_ctr = dut_->o_drain_ctr;
    o.fault_ctr = dut_->o_fault_ctr;
    o.squash_ctr = dut_->o_squash_ctr;
    o.squash_spared_ctr = dut_->o_squash_spared_ctr;
    o.last_fault_tval = dut_->o_last_fault_tval;
    o.last_fault_cause = dut_->o_last_fault_cause;
    o.last_fault_id = dut_->o_last_fault_id & geometry_.id_mask;

    o.ep_txn = dut_->o_ep_txn_ctr;
    o.ep_store = dut_->o_ep_store_ctr;
    o.ep_misaligned = dut_->o_ep_misaligned_ctr;
    o.ep_rsp = dut_->o_ep_rsp_ctr;
    o.ep_busy = dut_->o_ep_busy != 0;

    const std::vector<uint32_t> words = WordsOf(dut_->o_entry_pay);
    o.entries.clear();
    for (uint32_t i = 0; i < geometry_.entries; i++) {
      o.entries.push_back(codec_.Get(words, int(i) * int(geometry_.entry_w)));
    }
    return o;
  }

  // ------------------------------------------------------- the per-cycle check
  void CheckCycle(const DutOut& o, const Stim& s) {
    const std::vector<bool>& drop = cur_drop_;

    // 0. the fault a misaligned store owed, one cycle after the endpoint
    // composed it (the queue's own counter is registered on the following edge).
    if (pending_fault_) {
      Check(o.fault_ctr == fault_seen_, "endpoint-fault",
            "a misaligned store did not raise a fault: o_fault_ctr=" + Dec(o.fault_ctr) +
                ", expected " + Dec(fault_seen_));
      Check(o.last_fault_cause == kExcStoreMisaligned || o.last_fault_cause == kExcStoreAccess,
            "endpoint-fault-cause",
            "expected a store exception cause (" + Dec(kExcStoreMisaligned) + "/" +
                Dec(kExcStoreAccess) + "), got " + Dec(o.last_fault_cause));
      Check(o.last_fault_tval == pending_fault_entry_.addr(), "endpoint-fault-tval",
            "expected tval 0x" + U64(pending_fault_entry_.addr()) + ", got 0x" +
                U64(o.last_fault_tval));
      Check(o.last_fault_id == pending_fault_entry_.packed, "endpoint-fault-id",
            "expected the faulting store " + U32(pending_fault_entry_.packed) + ", got " +
                U32(o.last_fault_id));
    }

    // 1. allocation, fill and authorisation outcomes, against the shadow.
    Check(o.alloc_ready == shadow_.AllocReady(), "alloc-ready",
          "alloc_ready_o=" + Bool(o.alloc_ready) + " with " + Dec(shadow_.size()) +
              " of " + Dec(geometry_.entries) + " entries resident");
    size_t fill_idx = 0;
    const bool fill_found = s.fill_valid && shadow_.FindOldest(s.fill_id, &fill_idx);
    Check(o.fill_hit == fill_found, "fill-hit",
          "fill_hit_o=" + Bool(o.fill_hit) + " for id " + U32(s.fill_id & geometry_.id_mask) +
              ", the shadow " + (fill_found ? "found an entry" : "found none"));
    Check(o.fill_stale == (s.fill_valid && !fill_found), "fill-stale",
          "fill_stale_o=" + Bool(o.fill_stale) + " for id " +
              U32(s.fill_id & geometry_.id_mask));
    const bool commit_ok = s.commit_valid && shadow_.CommitOk(s.commit_id);
    Check(o.commit_ok == commit_ok, "commit-ok",
          "commit_ok_o=" + Bool(o.commit_ok) + " for id " +
              U32(s.commit_id & geometry_.id_mask) + " with " + Dec(shadow_.AuthCount()) +
              " authorised of " + Dec(shadow_.size()));
    Check(o.commit_stale == (s.commit_valid && !commit_ok), "commit-stale",
          "commit_stale_o=" + Bool(o.commit_stale) + " for id " +
              U32(s.commit_id & geometry_.id_mask));

    // 2. the drain offer. This is the visibility rule itself: the offer is
    // exactly "position 0, authorised, captured and not withdrawn".
    Entry head;
    const bool offer = shadow_.PredictOffer(drop, &head);
    Check(o.drain_valid == offer, "drain-valid",
          "drain_req_valid_o=" + Bool(o.drain_valid) + ", the contract asks for " + Bool(offer) +
              " (head " + (shadow_.empty() ? std::string("none") : shadow_.at(0).str()) +
              ", authorised " + Dec(shadow_.AuthCount()) + ")");
    if (offer) {
      Check(o.drain_id == head.packed, "drain-id",
            "expected " + U32(head.packed) + ", got " + U32(o.drain_id));
      Check(o.drain_base == head.base, "drain-base",
            "expected 0x" + U64(head.base) + ", got 0x" + U64(o.drain_base));
      Check(o.drain_imm == head.imm, "drain-imm",
            "expected 0x" + U64(head.imm) + ", got 0x" + U64(o.drain_imm));
      Check(o.drain_size == head.size, "drain-size",
            "expected " + Dec(head.size) + ", got " + Dec(o.drain_size));
      Check(o.drain_data == head.data, "drain-data",
            "expected 0x" + U64(head.data) + ", got 0x" + U64(o.drain_data));
    }

    // 3. the queue state, position by position.
    Check(o.count == shadow_.size(), "count",
          "o_count=" + Dec(o.count) + ", the shadow holds " + Dec(shadow_.size()));
    Check(o.auth_cnt == shadow_.AuthCount(), "auth-count",
          "o_auth_cnt=" + Dec(o.auth_cnt) + ", the shadow says " + Dec(shadow_.AuthCount()));
    for (uint32_t i = 0; i < geometry_.entries; i++) {
      const bool resident = i < shadow_.size();
      const bool dut_resident = ((o.occ >> i) & 1u) != 0;
      const bool dut_auth = ((o.entry_auth >> i) & 1u) != 0;
      Check(dut_resident == resident, "occupancy",
            "position " + Dec(i) + ": o_occ=" + Bool(dut_resident) + ", the shadow says " +
                Bool(resident));
      if (!resident) {
        Check(dut_auth == false, "auth-bitmap",
              "position " + Dec(i) + " is not resident but is marked authorised");
        Check(o.entries[i].base == 0 && o.entries[i].imm == 0 && o.entries[i].data == 0 &&
                  o.entries[i].packed == 0 && !o.entries[i].addr_valid &&
                  !o.entries[i].data_valid,
              "observation-zero",
              "position " + Dec(i) + " is not resident but the observation word is not zero");
        continue;
      }
      const Entry& want = shadow_.at(i);
      Check(dut_auth == want.authorised, "auth-bitmap",
            "position " + Dec(i) + ": authorised " + Bool(dut_auth) + ", the shadow says " +
                Bool(want.authorised));
      Check(o.entries[i].packed == want.packed, "entry-id",
            "position " + Dec(i) + ": id " + U32(o.entries[i].packed) + ", expected " +
                U32(want.packed));
      Check(o.entries[i].base == want.base, "entry-base",
            "position " + Dec(i) + ": base 0x" + U64(o.entries[i].base) + ", expected 0x" +
                U64(want.base));
      Check(o.entries[i].imm == want.imm, "entry-imm",
            "position " + Dec(i) + ": imm 0x" + U64(o.entries[i].imm) + ", expected 0x" +
                U64(want.imm));
      Check(o.entries[i].size == want.size, "entry-size",
            "position " + Dec(i) + ": size " + Dec(o.entries[i].size) + ", expected " +
                Dec(want.size));
      Check(o.entries[i].data == want.data, "entry-data",
            "position " + Dec(i) + ": data 0x" + U64(o.entries[i].data) + ", expected 0x" +
                U64(want.data));
      Check(o.entries[i].addr_valid == want.addr_valid, "entry-addr-valid",
            "position " + Dec(i) + ": addr_valid " + Bool(o.entries[i].addr_valid) +
                ", expected " + Bool(want.addr_valid));
      Check(o.entries[i].data_valid == want.data_valid, "entry-data-valid",
            "position " + Dec(i) + ": data_valid " + Bool(o.entries[i].data_valid) +
                ", expected " + Bool(want.data_valid));
    }

    // 4. conservation and the counter tallies.
    Check(o.alloc_ctr == alloc_seen_, "alloc-counter",
          "o_alloc_ctr=" + Dec(o.alloc_ctr) + ", the driver counted " + Dec(alloc_seen_));
    Check(o.fill_ctr == fill_seen_, "fill-counter",
          "o_fill_ctr=" + Dec(o.fill_ctr) + ", the driver counted " + Dec(fill_seen_));
    Check(o.fill_stale_ctr == fill_stale_seen_, "fill-stale-counter",
          "o_fill_stale_ctr=" + Dec(o.fill_stale_ctr) + ", the driver counted " +
              Dec(fill_stale_seen_));
    Check(o.commit_ctr == commit_seen_, "commit-counter",
          "o_commit_ctr=" + Dec(o.commit_ctr) + ", the driver counted " + Dec(commit_seen_));
    Check(o.commit_stale_ctr == commit_stale_seen_, "commit-stale-counter",
          "o_commit_stale_ctr=" + Dec(o.commit_stale_ctr) + ", the driver counted " +
              Dec(commit_stale_seen_));
    Check(o.drain_ctr == drain_seen_, "drain-counter",
          "o_drain_ctr=" + Dec(o.drain_ctr) + ", the driver counted " + Dec(drain_seen_));
    Check(o.fault_ctr == fault_seen_, "fault-counter",
          "o_fault_ctr=" + Dec(o.fault_ctr) + ", the driver counted " + Dec(fault_seen_));
    Check(o.squash_ctr == squash_seen_, "squash-counter",
          "o_squash_ctr=" + Dec(o.squash_ctr) + ", the driver counted " + Dec(squash_seen_));
    Check(o.squash_spared_ctr == squash_spared_seen_, "spared-counter",
          "o_squash_spared_ctr=" + Dec(o.squash_spared_ctr) + ", the driver counted " +
              Dec(squash_spared_seen_));
    Check(o.alloc_ctr == o.drain_ctr + o.squash_ctr + o.count, "conservation",
          "allocated " + Dec(o.alloc_ctr) + " != drained " + Dec(o.drain_ctr) + " + squashed " +
              Dec(o.squash_ctr) + " + resident " + Dec(o.count));

    // 4b. the I-035 query, which is a pure function of the queue and the query.
    {
      bool fwd_found = false;
      bool fwd_blocked = false;
      uint64_t fwd_data = 0;
      shadow_.Forward(s.fwd_addr, s.fwd_size, &fwd_found, &fwd_data, &fwd_blocked);
      Check(o.fwd_valid == fwd_found, "fwd-valid",
            "fwd_valid_o=" + Bool(o.fwd_valid) + " for 0x" + U64(s.fwd_addr) + "/" +
                Dec(s.fwd_size) + ", the rule says " + Bool(fwd_found));
      Check(o.fwd_blocked == fwd_blocked, "fwd-blocked",
            "fwd_blocked_o=" + Bool(o.fwd_blocked) + ", the rule says " + Bool(fwd_blocked));
      if (fwd_found) {
        Check(o.fwd_data == fwd_data, "fwd-data",
              "expected 0x" + U64(fwd_data) + ", got 0x" + U64(o.fwd_data));
      }
    }

    // 5. the memory side. A request is only allowed to carry the store the queue
    // offered, and it must carry it with the specification's strobes and lanes.
    if (o.mem_req_valid) {
      Check(!sent_.empty(), "memory-request",
            "a memory request appeared with no store offered by the queue");
      if (!sent_.empty()) {
        const Entry& w = sent_.front();
        Check(o.mem_req_we, "memory-we", "a store drain presented we=0");
        Check(o.mem_req_addr == w.addr(), "memory-addr",
              "expected 0x" + U64(w.addr()) + ", got 0x" + U64(o.mem_req_addr));
        Check(o.mem_req_size == w.size, "memory-size",
              "expected " + Dec(w.size) + ", got " + Dec(o.mem_req_size));
        const uint32_t wstrb = SpecWstrb(w.size, w.addr());
        const uint64_t wdata = SpecWdata(w.data, w.addr());
        Check(o.mem_req_wstrb == wstrb, "memory-wstrb",
              "expected 0x" + U32(wstrb) + ", got 0x" + U32(o.mem_req_wstrb));
        Check(o.mem_req_wdata == wdata, "memory-wdata",
              "expected 0x" + U64(wdata) + ", got 0x" + U64(o.mem_req_wdata));
      }
    }
  }

  // ------------------------------------------- the one bus-model advance (V-010)
  // The reset-traffic rule lives in one place (`mosaic::BusResetGate`): while
  // reset is asserted the memory model accepts nothing and delivers nothing. The
  // shipping cycle loop and the reset-traffic control both come through here, so
  // the control exercises the real accept path. With the gate in its shipping
  // state this is statement-for-statement the pre-rule behaviour.
  MemoryModel::EdgeResult StepMemory(bool rst, bool mem_valid, bool program_ready, bool we,
                                     uint64_t addr, uint32_t size_bytes, uint64_t wdata,
                                     bool rsp_ready) {
    if (!bus_gate_.MayDeliver(rst)) return MemoryModel::EdgeResult{};
    const bool accept = bus_gate_.MayAccept(rst, mem_valid && program_ready);
    return mem_.Edge(cycle_, accept, program_ready, we, addr, size_bytes, wdata, rsp_ready);
  }

  // ------------------------------------------------------- shadow/edge update
  void Commit(const DutOut& o, const Stim& s, bool rst) {
    if (rst) {
      // Hold the memory model in reset too: the queue releases nothing, so a
      // transaction in flight at reset is a defect the phase checks for.
      shadow_.Reset();
      sent_.clear();
      return;
    }

    // A deferred fault report has just been checked (CheckCycle runs before
    // this), so it is no longer pending.
    pending_fault_ = false;

    const std::vector<bool>& drop = cur_drop_;

    // 1. the fill, applied before the drain so a fill and a drain in one cycle
    // cannot disagree about what was captured.
    if (s.fill_valid) {
      size_t idx = 0;
      if (shadow_.FindOldest(s.fill_id, &idx)) {
        shadow_.Fill(idx, s.fill_addr_valid, s.fill_base, s.fill_imm, s.fill_data_valid, s.fill_data);
      }
    }

    // 2. the authorisation.
    if (s.commit_valid && shadow_.CommitOk(s.commit_id)) {
      shadow_.Commit(s.commit_id);
    }

    // 3. the squash: entries the region takes, minus the ones the watermark
    // spares.
    if (s.squash_valid) {
      shadow_.ApplyDrop(drop);
    }

    // 4. the drain: accepted entries leave, and the store they carried becomes
    // the driver's expectation of what the memory side must now do with it.
    if (o.drain_valid && o.drain_ready) {
      Entry sent = shadow_.at(0);
      sent_.push_back(sent);
      sent_mem_.push_back(false);
      shadow_.RemoveHead();
    }

    // 5. the memory model, driven by the *pre-edge* memory request, and the
    // expected bytes written from the specification rather than from the DUT.
    // The model writes exactly what the endpoint presented: the payload is
    // already in the lane convention (the byte at the address is the lane the
    // address selects), and the per-cycle check above has already compared it --
    // and the strobes, the address and the size -- against the specification for
    // the store the shadow says is in flight. Shifting it again here would put a
    // second lane convention in the model, which is how a store to a
    // non-zero lane would land in the wrong bytes.
    const uint32_t size_bytes = o.mem_req_valid ? SizeBytes(uint32_t(o.mem_req_size)) : 0;
    MemoryModel::EdgeResult edge =
        StepMemory(rst, o.mem_req_valid, s.mem_req_ready, o.mem_req_we, o.mem_req_addr,
                   size_bytes, o.mem_req_wdata, o.mem_rsp_ready);
    Require(!edge.accepted || !sent_.empty(), "memory-accept",
            "the memory accepted a transaction the queue never offered");
    if (edge.accepted && !sent_.empty()) {
      sent_mem_[0] = true;
      const Entry& w = sent_.front();
      if (!SpecMisaligned(w.addr(), w.size)) {
        // The store operand is a little-endian value: its byte i lands at
        // address+i. The lane convention is how the *bus* carries that byte
        // (byte at `addr` is lane `addr[2:0]`), and the endpoint's shift is what
        // turns one into the other. Writing `data >> 8*lane` here would instead
        // take the store's byte at the lane index, which is a different byte
        // whenever the store does not start at lane 0.
        for (uint32_t i = 0; i < SizeBytes(w.size); i++) {
          const uint64_t byte = (w.data >> (8u * i)) & 0xffull;
          const uint64_t addr = w.addr() + i;
          if (addr >= kShadowBase && (addr - kShadowBase) < kShadowBytes) {
            exp_mem_[addr - kShadowBase] = uint8_t(byte);
          }
        }
        expected_txn_++;
      }
    }

    // 6. the endpoint's response, which pops the in-flight store and is where a
    // misaligned store's fault is accounted for.
    if (o.ep_rsp > ep_rsp_seen_) {
      Require(!sent_.empty(), "endpoint-response",
              "the endpoint completed a transaction the queue never offered");
      if (!sent_.empty()) {
        const Entry w = sent_.front();
        const bool misaligned = SpecMisaligned(w.addr(), w.size);
        const bool reached = sent_mem_[0];
        Require(reached == !misaligned, "endpoint-response",
                "a store completed " + std::string(reached ? "after" : "without") +
                    " reaching the memory model, and is " +
                    std::string(misaligned ? "misaligned" : "aligned"));
        if (misaligned) {
          // The endpoint composed the fault this cycle; the queue consumes it on
          // the next edge and counts it there, so the driver's tally is advanced
          // by the same rule (one per completing misaligned store) and the
          // report itself is checked in the next cycle, when the queue's
          // registered counters have caught up.
          fault_seen_++;
          pending_fault_ = true;
          pending_fault_entry_ = w;
        } else {
          Require(o.fault_ctr == fault_seen_, "endpoint-response",
                  "an aligned store raised a fault: o_fault_ctr=" + Dec(o.fault_ctr));
        }
        sent_.erase(sent_.begin());
        sent_mem_.erase(sent_mem_.begin());
      }
      ep_rsp_seen_ = o.ep_rsp;
    }

    // 7. the driver's tallies, taken from what it observed.
    if (o.alloc_ready && s.alloc_valid) {
      alloc_seen_++;
      shadow_.Alloc(EntryOf(s));
    }
    if (o.fill_hit) fill_seen_++;
    if (o.fill_stale) fill_stale_seen_++;
    if (o.commit_ok) commit_seen_++;
    if (o.commit_stale) commit_stale_seen_++;
    if (o.drain_valid && o.drain_ready) drain_seen_++;
    // The squash tallies come from the rule, not from the DUT's counters, so the
    // counter comparison above is a real comparison.
    for (size_t i = 0; i < drop.size(); i++) {
      if (drop[i]) squash_seen_++;
    }
    for (size_t i = 0; i < cur_spared_.size(); i++) {
      if (cur_spared_[i]) squash_spared_seen_++;
    }
  }

  Entry EntryOf(const Stim& s) const {
    Entry e;
    e.packed = s.alloc_id & geometry_.id_mask;
    e.id = s.alloc_ident;
    e.base = s.alloc_base;
    e.imm = s.alloc_imm;
    e.size = s.alloc_size;
    e.data = s.alloc_data;
    e.addr_valid = s.alloc_addr_valid;
    e.data_valid = s.alloc_data_valid;
    return e;
  }

  // Members.
  Vmosaic_store_queue_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  uint64_t checks_ = 0;
  uint64_t phase_checks_ = 0;
  uint64_t cycle_ = 0;
  std::string phase_name_ = "init";

  Geom geometry_;
  Codec codec_ = Codec(Geom());
  Shadow shadow_;
  MemoryModel mem_;
  mosaic::BusResetGate bus_gate_;
  DutOut last_;

  // The stores the queue offered, in order. The front is the one the endpoint is
  // serving; a memory request must carry exactly it.
  std::vector<Entry> sent_;
  // Whether the store in flight has actually been accepted by the memory model,
  // per position of `sent_`. A store that completes without it is a store that
  // never reached memory.
  std::vector<bool> sent_mem_;

  // This cycle's squash effect, computed from the rule once and used by both the
  // per-cycle checks and the shadow update, so the two cannot disagree.
  std::vector<bool> cur_drop_;
  std::vector<bool> cur_spared_;

  // A misaligned store's report is composed by the endpoint one cycle before the
  // queue consumes it and counts it, so the assertion about the fault is made in
  // the next cycle, when the registered counters have caught up.
  bool pending_fault_ = false;
  Entry pending_fault_entry_;

  uint64_t alloc_seen_ = 0;
  uint64_t fill_seen_ = 0;
  uint64_t fill_stale_seen_ = 0;
  uint64_t commit_seen_ = 0;
  uint64_t commit_stale_seen_ = 0;
  uint64_t drain_seen_ = 0;
  uint64_t fault_seen_ = 0;
  uint64_t squash_seen_ = 0;
  uint64_t squash_spared_seen_ = 0;
  uint64_t expected_txn_ = 0;
  uint32_t ep_rsp_seen_ = 0;

  uint64_t run_allocs_ = 0;
  uint64_t run_drains_ = 0;
  uint64_t run_squashes_ = 0;
  uint64_t run_faults_ = 0;

  // The driver's own memory.
  static constexpr uint64_t kShadowBase = 0x80000000ull;
  static constexpr uint64_t kShadowBytes = 0x10000;
  std::vector<uint8_t> exp_mem_;

 public:
  // ---- reset-traffic control (V-010) --------------------------------------
  // Offers one request to the memory model with reset asserted, through the
  // same gate the cycle loop uses, and returns true iff the model accepted it.
  // In the shipping configuration it must refuse; with the accept-during-reset
  // control engaged the same path must accept, which is what proves the rule
  // check can fail rather than passing because nothing was ever offered.
  bool PresentDuringReset() {
    mem_.FlushPending();
    const uint64_t before = mem_.accepted();
    StepMemory(/*rst=*/true, /*mem_valid=*/true, /*program_ready=*/true, /*we=*/true,
               kShadowBase, 8, 0x1122334455667788ull, /*rsp_ready=*/false);
    return mem_.accepted() != before;
  }
  void SetAcceptDuringResetControl(bool on) {
    bus_gate_.SetAcceptDuringResetControl(on);
  }

  uint64_t run_allocs() const { return run_allocs_; }
  uint64_t run_drains() const { return run_drains_; }
  uint64_t run_squashes() const { return run_squashes_; }
  uint64_t run_faults() const { return run_faults_; }
};


// ============================================================================
// Phases
// ============================================================================

constexpr uint64_t kRamBase = 0x80000000ull;
constexpr uint32_t kByte = 0;
constexpr uint32_t kHalf = 1;
constexpr uint32_t kWord = 2;
constexpr uint32_t kDbl = 3;

Ident IdentOf(uint32_t rob_index, uint32_t rob_gen, uint32_t uop_index = 0) {
  Ident id;
  id.hart = 0;
  id.rob_index = rob_index;
  id.rob_gen = rob_gen;
  id.uop_index = uop_index;
  return id;
}

// A store with both facts captured.
Entry StoreAt(uint64_t addr, uint64_t data, uint32_t size) {
  Entry e;
  e.base = addr;
  e.imm = 0;
  e.data = data;
  e.size = size;
  e.addr_valid = true;
  e.data_valid = true;
  return e;
}

// ------------------------------------------------------- 1. the cold state
void PhaseResetState(Bench* b) {
  b->Phase("reset-state");
  b->Fresh();
  const DutOut& o = b->seen();
  b->Require(o.count == 0, "reset", "o_count is " + Dec(o.count));
  b->Require(o.auth_cnt == 0, "reset", "o_auth_cnt is " + Dec(o.auth_cnt));
  b->Require(o.occ == 0, "reset", "the occupancy bitmap is not empty");
  b->Require(o.entry_auth == 0, "reset", "the authorised bitmap is not empty");
  b->Require(o.drain_valid == false, "reset", "the drain port offers a store after reset");
  b->Require(o.alloc_ready == true, "reset", "the queue refuses an allocation after reset");
  b->Require(o.alloc_ctr == 0 && o.fill_ctr == 0 && o.fill_stale_ctr == 0 && o.commit_ctr == 0 &&
                  o.commit_stale_ctr == 0 && o.drain_ctr == 0 && o.fault_ctr == 0 &&
                  o.squash_ctr == 0 && o.squash_spared_ctr == 0,
              "reset", "a counter is not zero after reset");
  b->Require(o.ep_txn == 0 && o.ep_rsp == 0, "reset", "the endpoint is not idle after reset");
  b->RunIdle(4);
  b->Require(b->mem().accepted() == 0, "reset", "the memory saw a transaction after reset");
  b->CheckMemory();
  b->EndPhase();
}

// ------------------------------------------------------- 1b. the reset rule (V-010)
// The bus model refuses a request presented while reset is asserted, and the
// guard has been seen to fire. The model is flushed at reset for a reason the
// gate does not subsume -- the DUT's transaction state is cleared by the reset
// and the model must resynchronise with it -- so the flush stays and the gate is
// added on the accept/deliver decision.
void PhaseResetTraffic(Bench* b) {
  b->Phase("reset-traffic");
  b->Fresh();
  b->Require(!b->PresentDuringReset(), "reset-traffic",
             "the memory model accepted a request while reset was asserted");
  b->SetAcceptDuringResetControl(true);
  const bool control_accepted = b->PresentDuringReset();
  b->SetAcceptDuringResetControl(false);
  b->Require(control_accepted, "reset-traffic",
             "the accept-during-reset control did not fire: the reset-traffic "
             "guard is untested");
  b->Require(!b->PresentDuringReset(), "reset-traffic",
             "the accept-during-reset control was left engaged");
  b->EndPhase();
}

// --------------------------------------------- 2. the wrong-path store
// The card's Pass criterion, first half: a store on a speculated path writes
// nothing and produces no memory transaction, even though its address and data
// are ready -- because execution-complete is not an authorisation.
void PhaseWrongPath(Bench* b) {
  b->Phase("wrong-path");
  b->Fresh();

  const Ident spec = IdentOf(10, 0);
  const Entry spec_store = StoreAt(kRamBase + 0x1000, 0x1122334455667788ull, kDbl);
  b->AllocStore(spec_store, spec);
  b->Require(b->shadow().size() == 1, "wrong-path", "the speculated store is not resident");
  b->Require(b->shadow().at(0).addr_valid && b->shadow().at(0).data_valid, "wrong-path",
             "the speculated store is not execution-complete, so the phase proves nothing");
  b->Require(b->shadow().AuthCount() == 0, "wrong-path", "an uncommitted store is authorised");

  // Its address and data are ready and the memory is idle: nothing about the
  // state of the world except the missing authorisation can hold it back.
  b->RunIdle(8);
  b->Require(b->seen().drain_valid == false, "wrong-path",
             "an uncommitted store is being offered to the memory side");
  b->Require(b->seen().ep_txn == 0, "wrong-path",
             "an uncommitted store reached the endpoint's memory port");
  b->Require(b->mem().accepted() == 0, "wrong-path",
             "an uncommitted store reached the memory (" + Dec(b->mem().accepted()) +
                 " transaction(s))");
  b->Require(b->MemoryUntouched(), "wrong-path",
             "the memory changed with no store authorised");
  b->CheckMemory();

  // The path it was speculated on turns out to be wrong.
  b->SquashWindow(10, 12, 0);
  b->RunIdle(1);
  b->Require(b->seen().squash_ctr == 1, "wrong-path",
             "the squash withdrew " + Dec(b->seen().squash_ctr) + " entr(y/ies), expected 1");
  b->Require(b->shadow().size() == 0, "wrong-path", "the squashed store is still resident");

  b->RunIdle(8);
  b->Require(b->seen().ep_txn == 0, "wrong-path",
             "a squashed store still reached the endpoint after the squash");
  b->Require(b->mem().accepted() == 0, "wrong-path",
             "a squashed store still reached memory after the squash");
  b->CheckMemory();

  // And the queue is not broken by having held a wrong-path store: a store that
  // really is authorised leaves, with its bytes, exactly once.
  const Ident good = IdentOf(11, 0);
  const Entry good_store = StoreAt(kRamBase + 0x2000, 0xdeadbeefcafebabeull, kDbl);
  b->AllocStore(good_store, good);
  b->CommitStore(b->MakeId(good), true);
  b->Settle(64);
  b->Require(b->mem().accepted() == 1, "wrong-path",
             "the memory saw " + Dec(b->mem().accepted()) + " transaction(s), expected 1");
  b->Require(b->seen().ep_txn == 1, "wrong-path",
             "the endpoint counted " + Dec(b->seen().ep_txn) + " transaction(s), expected 1");
  b->CheckMemory();

  // The other half of the visibility rule, and the other half of the card's
  // Pass criterion: a store that IS authorised still writes nothing when it is
  // not non-faulting. A misaligned store is trapped by mosaic_lsu_endpoint
  // before the memory sees it, so the queue drains it (it retired), the fault is
  // reported with its identity and address, and not one byte lands.
  const Ident mis = IdentOf(12, 0);
  Entry mis_store = StoreAt(kRamBase + 0x2010 + 4, 0xffffffffffffffffull, kDbl);
  b->AllocStore(mis_store, mis);
  b->CommitStore(b->MakeId(mis), true);
  b->Settle(64);
  // The queue consumes the endpoint's response on the edge *after* the endpoint
  // composes it, so its registered fault counter is one cycle behind `Settle`'s
  // exit condition.
  b->RunIdle(2);
  b->Require(b->seen().fault_ctr == 1, "wrong-path",
             "the misaligned store did not report a fault (" + Dec(b->seen().fault_ctr) + ")");
  b->Require(b->seen().ep_misaligned == 1, "wrong-path",
             "the endpoint's misaligned counter is " + Dec(b->seen().ep_misaligned));
  b->Require(b->seen().ep_txn == 1, "wrong-path",
             "the misaligned store reached the memory port (" + Dec(b->seen().ep_txn) +
                 " transaction(s))");
  b->Require(b->mem().accepted() == 1, "wrong-path",
             "the misaligned store reached the memory (" + Dec(b->mem().accepted()) + ")");
  b->Require(b->seen().last_fault_cause == kExcStoreMisaligned, "wrong-path",
             "the fault cause is " + Dec(b->seen().last_fault_cause) + ", expected " +
                 Dec(kExcStoreMisaligned));
  b->Require(b->seen().last_fault_tval == mis_store.base, "wrong-path",
             "the fault address is 0x" + U64(b->seen().last_fault_tval) + ", expected 0x" +
                 U64(mis_store.base));
  b->Require(b->seen().last_fault_id == b->MakeId(mis), "wrong-path",
             "the fault names " + U32(b->seen().last_fault_id) + ", expected " +
                 U32(b->MakeId(mis)));
  b->CheckMemory();
  b->EndPhase();
}

// ------------------------------- 3. an authorised store survives a flush
// The card's Pass criterion, second half, and the failure the RTL header names:
// a flush must not delete a store that already retired. The store has to be
// *resident and authorised* when the flush arrives, so the memory is held so its
// predecessor cannot leave the endpoint and it cannot leave the queue.
void PhaseFlushSafety(Bench* b) {
  b->Phase("flush-safety");
  b->Fresh();

  const Ident a0 = IdentOf(20, 0);
  const Ident a1 = IdentOf(21, 0);
  const Ident a2 = IdentOf(22, 0);
  const Entry s0 = StoreAt(kRamBase + 0x3000, 0x0f0e0d0c0b0a0908ull, kDbl);
  const Entry s1 = StoreAt(kRamBase + 0x3008, 0x0706050403020100ull, kDbl);
  const Entry s2 = StoreAt(kRamBase + 0x3010, 0x8877665544332211ull, kDbl);
  b->AllocStore(s0, a0);
  b->AllocStore(s1, a1);
  b->AllocStore(s2, a2);

  b->CommitStore(b->MakeId(a0), true, 20);
  b->CommitStore(b->MakeId(a1), true, 20);
  // a2 stays uncommitted: it is the younger speculative work the flush kills.

  // Let a0 leave the queue into the endpoint, then hold the memory so it stays
  // there and a1 -- which is authorised and next -- cannot drain either.
  b->RunIdleSlow(8, 20);
  b->Require(b->shadow().size() == 2, "flush",
             "the expected two stores are not resident (" + Dec(b->shadow().size()) + " held, " +
                 Dec(b->mem().accepted()) + " transaction(s) so far)");
  b->Require(b->shadow().AuthCount() == 1, "flush",
             "the resident stores' authorisation mask is not the expected {a1} (" +
                 Dec(b->shadow().AuthCount()) + " leading authorised)");
  b->Require(b->mem().accepted() == 1, "flush",
             "the memory accepted " + Dec(b->mem().accepted()) +
                 " transaction(s); expected the first store to be inside the endpoint");

  // The whole speculative region dies. a1 has retired and is spared; a2 has not
  // and is withdrawn.
  b->SquashAll(0);
  b->RunIdle(1);
  b->Require(b->seen().squash_ctr == 1, "flush",
             "the flush withdrew " + Dec(b->seen().squash_ctr) + " entries, expected 1 (a2)");
  b->Require(b->seen().squash_spared_ctr == 1, "flush",
             "the flush spared " + Dec(b->seen().squash_spared_ctr) +
                 " authorised entries, expected 1 (a1)");
  b->Require(b->shadow().size() == 1, "flush",
             "the authorised store did not survive the flush");
  b->Require(b->shadow().at(0).packed == b->MakeId(a1), "flush",
             "the survivor is not the authorised store a1");

  // Release the memory: a0 completes and a1 follows it, exactly once each.
  b->Settle(200);
  b->Require(b->shadow().size() == 0, "flush", "the authorised store never drained");
  b->Require(b->mem().accepted() == 2, "flush",
             "the memory saw " + Dec(b->mem().accepted()) + " transaction(s), expected 2");
  b->CheckMemory();

  // A second flush must not re-issue anything.
  b->SquashAll(1);
  b->RunIdle(8);
  b->Require(b->seen().drain_ctr == 2, "flush",
             "the queue drained " + Dec(b->seen().drain_ctr) + " stores, expected 2");
  b->Require(b->mem().accepted() == 2, "flush",
             "a second flush re-issued a store (" + Dec(b->mem().accepted()) + " transactions)");
  b->CheckMemory();
  b->EndPhase();
}

// ------------------------------------------------- 4. in-order drain
void PhaseInOrder(Bench* b) {
  b->Phase("in-order");
  b->Fresh();

  const Ident s0 = IdentOf(30, 0);
  const Ident s1 = IdentOf(31, 0);
  const Entry e0 = StoreAt(kRamBase + 0x4000, 0x0123456789abcdefull, kDbl);
  const Entry e1 = StoreAt(kRamBase + 0x4008, 0xfedcba9876543210ull, kDbl);
  b->AllocStore(e0, s0);
  b->AllocStore(e1, s1);

  // The younger store cannot be authorised while the older one is not.
  b->CommitStore(b->MakeId(s1), false);
  b->Require(b->seen().commit_stale, "in-order", "an out-of-order authorisation was accepted");
  b->RunIdle(4);
  b->Require(b->seen().drain_valid == false, "in-order",
             "a store was offered while its older sibling is unauthorised");
  b->Require(b->mem().accepted() == 0, "in-order",
             "a store reached memory behind an unauthorised head");

  // The older one commits; it drains first, and the younger one still waits.
  b->CommitStore(b->MakeId(s0), true);
  b->RunIdle(6);
  b->Require(b->shadow().size() == 1, "in-order",
             "expected exactly the younger store to still be resident");
  b->Require(b->shadow().at(0).packed == b->MakeId(s1), "in-order",
             "the store that drained was not the older one");
  b->Require(b->seen().drain_valid == false, "in-order",
             "the unauthorised younger store is being offered");
  b->Require(b->mem().accepted() == 1, "in-order",
             "the memory saw " + Dec(b->mem().accepted()) + " transaction(s), expected 1");

  b->CommitStore(b->MakeId(s1), true);
  b->Settle(64);
  b->Require(b->mem().accepted() == 2, "in-order",
             "the memory saw " + Dec(b->mem().accepted()) + " transaction(s), expected 2");
  const std::vector<uint64_t>& log = b->mem().request_log();
  b->Require(log.size() == 2 && log[0] == e0.base && log[1] == e1.base, "in-order",
             "the memory saw the stores out of program order");
  b->CheckMemory();

  // A committed offer is held, unchanged, while the endpoint is busy.
  b->Fresh();
  const Ident h0 = IdentOf(40, 0);
  const Ident h1 = IdentOf(41, 0);
  b->AllocStore(StoreAt(kRamBase + 0x4100, 0x1111111111111111ull, kDbl), h0);
  b->AllocStore(StoreAt(kRamBase + 0x4108, 0x2222222222222222ull, kDbl), h1);
  b->CommitStore(b->MakeId(h0), true, 20);
  b->CommitStore(b->MakeId(h1), true, 20);
  b->RunIdleSlow(10, 20);
  b->Require(b->seen().drain_valid, "in-order",
             "the authorised head stopped being offered while the endpoint was busy");
  b->Require(b->seen().drain_base == kRamBase + 0x4108, "in-order",
             "the held offer is not the next authorised store (0x" +
                 U64(b->seen().drain_base) + ")");
  b->RunIdleSlow(4, 20);
  b->Settle(200);
  b->CheckMemory();
  b->EndPhase();
}

// -------------------------------------------- 5. capacity and backpressure
void PhaseCapacity(Bench* b) {
  b->Phase("capacity");
  b->Fresh();

  const uint32_t depth = b->geom().entries;
  std::vector<Ident> ids;
  for (uint32_t i = 0; i < depth; i++) {
    Ident id = IdentOf(50 + i, 0);
    ids.push_back(id);
    b->AllocStore(StoreAt(kRamBase + 0x5000 + 8 * i, 0x1000ull + i, kDbl), id);
  }
  b->Require(b->shadow().size() == depth, "capacity", "the queue did not fill");
  // One more cycle so the observation is of the state *after* the last
  // allocation's edge: `seen()` is the pre-edge snapshot, and comparing it with
  // the shadow (already past that edge) would mix two states. Nothing can drain
  // here, so the extra cycle changes nothing but the snapshot.
  b->RunIdle(1);
  b->Require(b->seen().alloc_ready == false, "capacity", "the queue reports room at capacity");
  const uint32_t allocs_at_capacity = b->seen().alloc_ctr;

  // Keep offering: every offer must be refused, and refused means not stored.
  const Entry extra = StoreAt(kRamBase + 0x5800, 0x9999999999999999ull, kDbl);
  for (int i = 0; i < 6; i++) {
    Stim s;
    s.alloc_valid = true;
    s.alloc_id = b->MakeId(IdentOf(200, 0));
    s.alloc_ident = IdentOf(200, 0);
    s.alloc_base = extra.base;
    s.alloc_imm = extra.imm;
    s.alloc_size = extra.size;
    s.alloc_addr_valid = true;
    s.alloc_data = extra.data;
    s.alloc_data_valid = true;
    b->Cycle(s, false);
    b->Require(b->seen().alloc_ready == false, "capacity",
               "the full queue accepted an offer (alloc_ready_o high)");
    b->Require(b->shadow().size() == depth, "capacity",
               "a refused allocation entered the shadow");
  }
  b->Require(b->seen().alloc_ctr == allocs_at_capacity, "capacity",
             "a refused allocation was counted as accepted");
  b->RunIdle(4);
  b->Require(b->mem().accepted() == 0, "capacity",
             "an unauthorised full queue reached memory");

  // One commit drains one store, one slot appears, and the refused store is
  // then accepted -- nothing was lost while it was refused.
  b->CommitStore(b->MakeId(ids[0]), true);
  b->RunIdle(6);
  b->Require(b->shadow().size() == depth - 1, "capacity", "the committed store did not drain");
  b->Require(b->seen().alloc_ready == true, "capacity", "the freed slot is not offered");
  b->AllocStore(extra, IdentOf(200, 0));
  b->Require(b->shadow().size() == depth, "capacity", "the refused store was not admitted later");

  // The conservation identity, stated once more against the driver's own tally.
  const DutOut& o = b->seen();
  b->Require(o.alloc_ctr == o.drain_ctr + o.squash_ctr + o.count, "capacity",
             "conservation broken: " + Dec(o.alloc_ctr) + " != " + Dec(o.drain_ctr) + " + " +
                 Dec(o.squash_ctr) + " + " + Dec(o.count));

  // Everything unauthorised dies with a full flush; the one committed store has
  // already drained, so the queue empties.
  b->CommitStore(o.entries[0].packed, true);
  b->RunIdle(4);
  b->SquashAll(1);
  b->RunIdle(1);
  b->Settle(64);
  b->Require(b->shadow().size() == 0, "capacity", "the flush did not empty the queue");
  b->CheckMemory();
  b->EndPhase();
}

// -------------------------------------- 6. address and data readiness
void PhaseReadiness(Bench* b) {
  b->Phase("readiness");
  b->Fresh();

  // (a) data ready, address not: it must be held even once authorised.
  const Ident p = IdentOf(60, 0);
  const uint64_t p_addr = kRamBase + 0x6000;
  const uint64_t p_data = 0xa1a2a3a4a5a6a7a8ull;
  Entry pe = StoreAt(p_addr, p_data, kDbl);
  pe.addr_valid = false;
  b->AllocStore(pe, p);
  b->CommitStore(b->MakeId(p), true);
  b->RunIdle(6);
  b->Require(b->seen().drain_valid == false, "readiness",
             "a store with no captured address was offered to memory");
  b->Require(b->mem().accepted() == 0, "readiness",
             "a store with no captured address reached memory");
  b->FillEntry(true, p_addr, 0, false, 0, p);
  b->Require(b->seen().fill_hit, "readiness", "the address fill did not find its entry");
  b->Settle(32);
  b->Require(b->mem().accepted() == 1, "readiness",
             "the store did not drain after its address arrived");
  b->CheckMemory();

  // (b) address ready, data not.
  const Ident q = IdentOf(61, 0);
  const uint64_t q_addr = kRamBase + 0x6010;
  const uint64_t q_data = 0xb1b2b3b4b5b6b7b8ull;
  Entry qe = StoreAt(q_addr, 0, kDbl);
  qe.data_valid = false;
  b->AllocStore(qe, q);
  b->CommitStore(b->MakeId(q), true);
  b->RunIdle(6);
  b->Require(b->seen().drain_valid == false, "readiness",
             "a store with no captured data was offered to memory");
  b->Require(b->mem().accepted() == 1, "readiness",
             "a store with no captured data reached memory");
  b->FillEntry(false, 0, 0, true, q_data, q);
  b->Require(b->seen().fill_hit, "readiness", "the data fill did not find its entry");
  b->Settle(32);
  b->Require(b->mem().accepted() == 2, "readiness",
             "the store did not drain after its data arrived");
  b->CheckMemory();

  // (c) neither fact yet: allocated, committed, and still invisible.
  const Ident r = IdentOf(62, 0);
  Entry re = StoreAt(kRamBase + 0x6020, 0xc1c2c3c4c5c6c7c8ull, kDbl);
  re.addr_valid = false;
  re.data_valid = false;
  b->AllocStore(re, r);
  b->CommitStore(b->MakeId(r), true);
  b->RunIdle(6);
  b->Require(b->mem().accepted() == 2, "readiness",
             "a store with neither fact captured reached memory");
  b->FillEntry(false, 0, 0, true, re.data, r);
  b->RunIdle(4);
  b->Require(b->mem().accepted() == 2, "readiness",
             "a store with only its data arrived early");
  b->FillEntry(true, re.base, 0, false, 0, r);
  b->Settle(32);
  b->Require(b->mem().accepted() == 3, "readiness", "the store never completed");
  b->CheckMemory();

  // (d) a fill that names no resident entry is stale, and a fill in the
  // allocation cycle is stale too: the fill port serves entries that are already
  // resident.
  const Ident g = IdentOf(70, 0);
  b->FillEntry(true, kRamBase, 0, false, 0, g);
  b->Require(b->seen().fill_stale, "readiness", "a fill for a non-resident entry was not stale");
  b->Require(b->seen().fill_hit == false, "readiness", "a stale fill reported a hit");

  const Ident same = IdentOf(71, 0);
  Stim both;
  both.alloc_valid = true;
  both.alloc_id = b->MakeId(same);
  both.alloc_ident = same;
  both.alloc_base = kRamBase + 0x7000;
  both.alloc_size = kDbl;
  both.alloc_addr_valid = true;
  both.alloc_data = 0x5a5a5a5a5a5a5a5aull;
  both.alloc_data_valid = false;
  both.fill_valid = true;
  both.fill_id = b->MakeId(same);
  both.fill_data = 0x5a5a5a5a5a5a5a5aull;
  both.fill_data_valid = true;
  b->Cycle(both, false);
  b->Require(b->seen().fill_stale, "readiness",
             "a fill in the allocation cycle was accepted as if the entry were resident");
  b->Require(b->seen().alloc_ready, "readiness", "the allocation in that cycle was refused");
  b->RunIdle(4);
  b->Require(b->shadow().at(0).data_valid == false, "readiness",
             "the same-cycle fill reached the entry");
  b->Require(b->seen().drain_valid == false, "readiness",
             "the partially captured store is being offered");
  // The queue is not stuck: a real fill completes it and it never drains.
  b->FillEntry(false, 0, 0, true, both.alloc_data, same);
  b->RunIdle(4);
  b->Require(b->mem().accepted() == 3, "readiness",
             "an uncommitted store drained after its fill");
  b->EndPhase();
}

// ------------------------------- 7. the I-035 forwarding query (rule only)
// The port is this package's interface decision for I-035 and is not wired to
// anything yet; this phase exercises the rule directly so it is not shipped
// untried. It carries no mutant: the visibility contract is the case.
void PhaseForward(Bench* b) {
  b->Phase("forward");
  b->Fresh();

  const Ident f0 = IdentOf(80, 0);
  const Ident f1 = IdentOf(81, 0);
  const uint64_t d0 = 0x0011223344556677ull;
  const uint64_t d1 = 0x8899aabbccddeeffull;
  b->AllocStore(StoreAt(kRamBase + 0x8000, d0, kDbl), f0);
  b->AllocStore(StoreAt(kRamBase + 0x8008, d1, kDbl), f1);
  // Deliberately uncommitted: forwarding must not need an authorisation.

  auto query = [&](uint64_t addr, uint32_t size, bool want_valid, bool want_blocked,
                   uint64_t want_data) {
    Stim s;
    s.fwd_addr = addr;
    s.fwd_size = size;
    b->Cycle(s, false);
    b->Require(b->seen().fwd_valid == want_valid, "forward",
               "query 0x" + U64(addr) + "/" + Dec(size) + ": fwd_valid_o=" +
                   Bool(b->seen().fwd_valid) + ", expected " + Bool(want_valid));
    b->Require(b->seen().fwd_blocked == want_blocked, "forward",
               "query 0x" + U64(addr) + "/" + Dec(size) + ": fwd_blocked_o=" +
                   Bool(b->seen().fwd_blocked) + ", expected " + Bool(want_blocked));
    if (want_valid) {
      b->Require(b->seen().fwd_data == want_data, "forward",
                 "query 0x" + U64(addr) + "/" + Dec(size) + ": data 0x" +
                     U64(b->seen().fwd_data) + ", expected 0x" + U64(want_data));
    }
  };

  // A whole doubleword from the store that owns it.
  query(kRamBase + 0x8008, kDbl, true, false, d1 << 0);
  // A byte inside that store. The payload comes back in the memory lane
  // convention -- aligned to the *store's* address, which is lane 0 here -- so
  // the value is the store's data unchanged and the load takes byte 1 of it.
  query(kRamBase + 0x8009, kByte, true, false, d1);
  // A word that the store covers.
  query(kRamBase + 0x8008, kWord, true, false, d1 << 0);
  // A halfword inside the same doubleword, an unaligned start inside it.
  query(kRamBase + 0x800a, kHalf, true, false, d1 << 0);
  // A query that crosses the doubleword boundary is not covered by any one
  // store, so the answer is "no".
  query(kRamBase + 0x8004, kDbl, false, false, 0);
  // Partial coverage is not answered either.
  b->AllocStore(StoreAt(kRamBase + 0x8018, 0x77ull, kByte), IdentOf(82, 0));
  query(kRamBase + 0x8018, kDbl, false, false, 0);
  query(kRamBase + 0x8018, kByte, true, false, 0x77ull);

  // An entry whose address is not captured blocks any answer.
  const Ident f3 = IdentOf(83, 0);
  Entry unknown = StoreAt(kRamBase + 0x8020, 0x1234ull, kByte);
  unknown.addr_valid = false;
  b->AllocStore(unknown, f3);
  query(kRamBase + 0x8008, kDbl, true, true, d1 << 0);

  // The youngest store wins when two overlap.
  const uint64_t d4 = 0x0102030405060708ull;
  b->AllocStore(StoreAt(kRamBase + 0x8008, d4, kDbl), IdentOf(84, 0));
  query(kRamBase + 0x8008, kDbl, true, true, d4);
  b->EndPhase();
}

// ---------------------------------------------------------------- 8. soak
void PhaseSoak(Bench* b, uint64_t seed, int cycles) {
  b->Phase("soak");
  b->Fresh();
  mosaic::Rng rng(seed);

  uint32_t alloc_ptr = 100;
  uint32_t gen = 0;
  uint64_t data_src = 0x51ed270b;
  bool alloc_pending = false;
  Ident pending_ident;
  Entry pending_entry;
  std::vector<Ident> incomplete;

  for (int i = 0; i < cycles; i++) {
    Stim s;

    // A new allocation is offered until it is accepted.
    if (!alloc_pending && rng.Chance(70)) {
      alloc_pending = true;
      pending_ident = IdentOf(alloc_ptr & b->geom().rob_index_mask, gen);
      alloc_ptr++;
      data_src = data_src * 6364136223846793005ull + 1442695040888963407ull;
      const uint32_t size = rng.Below(4);
      uint64_t addr = kRamBase + 0x900 + 8 * rng.Below(48);
      if (rng.Chance(10)) addr += 1 + rng.Below(7);  // sometimes misaligned
      pending_entry = StoreAt(addr, data_src, size);
      if (rng.Chance(20)) pending_entry.addr_valid = false;
      if (rng.Chance(20)) pending_entry.data_valid = false;
    }
    if (alloc_pending) {
      s.alloc_valid = true;
      s.alloc_id = b->MakeId(pending_ident);
      s.alloc_ident = pending_ident;
      s.alloc_base = pending_entry.base;
      s.alloc_imm = pending_entry.imm;
      s.alloc_size = pending_entry.size;
      s.alloc_addr_valid = pending_entry.addr_valid;
      s.alloc_data = pending_entry.data_valid ? pending_entry.data : 0;
      s.alloc_data_valid = pending_entry.data_valid;
    }

    // A late fact arrives for something allocated earlier.
    if (!incomplete.empty() && rng.Chance(35)) {
      const size_t pick = rng.Below(uint32_t(incomplete.size()));
      s.fill_valid = true;
      s.fill_id = b->MakeId(incomplete[pick]);
      if (rng.Chance(50)) {
        s.fill_addr_valid = true;
        s.fill_base = kRamBase + 0x900 + 8 * rng.Below(48);
        s.fill_imm = 0;
      } else {
        s.fill_data_valid = true;
        s.fill_data = data_src ^ uint64_t(rng.Next());
      }
      incomplete.erase(incomplete.begin() + long(pick));
    }

    // An authorisation: usually the head, which must be accepted, and sometimes a
    // younger entry, which must be refused. Both are part of the contract, and a
    // soak that only ever named the head would never exercise the refusal.
    if (rng.Chance(45) && b->shadow().size() > 0) {
      const size_t pick = rng.Chance(70) ? 0 : rng.Below(uint32_t(b->shadow().size()));
      s.commit_valid = true;
      s.commit_id = b->shadow().at(pick).packed;
    }

    // A squash: a whole-window flush, or a region starting at some resident
    // store and running to the allocation pointer.
    if (rng.Chance(8) && b->shadow().size() > 0) {
      s.squash_valid = true;
      if (rng.Chance(40)) {
        s.squash_all = true;
        s.squash_gen = gen;
        gen++;
      } else {
        const size_t pick = rng.Below(uint32_t(b->shadow().size()));
        s.squash_from = b->shadow().at(pick).id.rob_index;
        s.squash_tail = alloc_ptr & b->geom().rob_index_mask;
        s.squash_gen = b->shadow().at(pick).id.rob_gen;
      }
    }

    // Memory back-pressure and latency.
    s.mem_req_ready = rng.Chance(75);
    s.latency = rng.Below(3);
    s.fwd_addr = kRamBase + 0x900 + 8 * rng.Below(48);
    s.fwd_size = rng.Below(4);

    b->Cycle(s, false);
    if (alloc_pending && b->seen().alloc_ready && s.alloc_valid) {
      alloc_pending = false;
      if (!(pending_entry.addr_valid && pending_entry.data_valid)) {
        incomplete.push_back(pending_ident);
      }
    }
  }

  // Drain what the soak left. A store can be short of a fact and unable to drain
  // at all, so first hand every such entry both facts (a fill can carry both at
  // once), then flush: the flush withdraws the unauthorised work and spares
  // everything that retired, so what remains reaches the memory path rather than
  // being abandoned.
  // The list is read out of the shadow first: a fill can make an authorised entry
  // drainable, and draining removes it from the list the loop would be walking.
  b->RunIdle(16);
  std::vector<uint32_t> short_of_a_fact;
  for (size_t i = 0; i < b->shadow().size(); i++) {
    const Entry& e = b->shadow().at(i);
    if (!(e.addr_valid && e.data_valid)) short_of_a_fact.push_back(e.packed);
  }
  for (size_t i = 0; i < short_of_a_fact.size(); i++) {
    b->FillEntryPacked(short_of_a_fact[i], true, kRamBase + 0x900 + 8 * uint64_t(i % 48u), 0,
                       true, (uint64_t(i) + 1) * 0x0101010101010101ull);
  }
  b->SquashAll(gen);
  b->RunIdle(1);
  std::fprintf(stderr, "DBG after flush: held=%zu auth=%zu dut_squash=%u spared=%u drains=%llu\n",
               b->shadow().size(), b->shadow().AuthCount(), b->seen().squash_ctr,
               b->seen().squash_spared_ctr, (unsigned long long)b->accepted_drains());
  b->Settle(512);
  b->Require(b->shadow().size() == 0, "soak", "the queue did not empty");
  b->CheckMemory();

  b->Require(b->run_allocs() > 0 && b->accepted_allocs() > 0, "soak",
             "the soak never allocated a store");
  b->Require(b->accepted_drains() > 0, "soak", "the soak never drained a store");
  b->Require(b->squashed() > 0, "soak", "the soak never squashed a store");
  b->EndPhase();
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
  mosaic::ClockDriver clk;
  Vmosaic_store_queue_tb dut;

  std::string detail;
  bool passed = true;
  Bench bench(&dut, &clk, options.max_cycles);

  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    bench.ReadGeometry();

    PhaseResetState(&bench);
    PhaseResetTraffic(&bench);
    PhaseWrongPath(&bench);
    PhaseFlushSafety(&bench);
    PhaseInOrder(&bench);
    PhaseCapacity(&bench);
    PhaseReadiness(&bench);
    PhaseForward(&bench);
    PhaseSoak(&bench, options.seed, 3000);

    Require(bench.run_allocs() > 0, "coverage", "no store was ever allocated");
    Require(bench.run_drains() > 0, "coverage", "no store was ever drained");
    Require(bench.run_squashes() > 0, "coverage", "no store was ever squashed");
    Require(bench.run_faults() > 0, "coverage", "no faulting store was ever drained");

    detail = "store queue contract holds: " + std::to_string(bench.checks()) +
             " checks over " + std::to_string(clk.cycle()) + " cycles; " +
             std::to_string(bench.run_allocs()) + " stores allocated, " +
             std::to_string(bench.run_drains()) + " drained, " +
             std::to_string(bench.run_squashes()) + " squashed, " +
             std::to_string(bench.run_faults()) + " faulted; seed " +
             std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated after " + std::to_string(bench.checks()) + " checks: " + f.what;
    std::printf("FAILED AFTER %llu CHECKS\n", static_cast<unsigned long long>(bench.checks()));
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
