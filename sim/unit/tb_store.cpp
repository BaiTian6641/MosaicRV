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

std::string DescribeSeq(const std::vector<Entry>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); i++) {
    if (i != 0) out += ", ";
    out += v[i].str();
  }
  return out + "]";
}

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
  static constexpr uint64_t kRegionBytes = 4096;

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
    mem_.PatternFill();
    mem_.SetLatency(0);
    mem_.FlushPending();
    const Stim idle;
    for (int i = 0; i < 2; i++) Cycle(idle, true);
    for (int i = 0; i < 2; i++) Cycle(idle, false);
  }

  void EndPhase() {
    const Stim idle;
    Cycle(idle, false);
    Cycle(idle, false);
    Require(sent_.empty(), "phase-end", "a store is still in flight inside the endpoint");
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
    Require(bad == 0, "memory-bytes",
            Dec(bad) + " byte(s) differ from the expected memory, first at 0x" + U64(first_bad) +
                ": memory 0x" + mosaic::Hex(mem_.Peek(first_bad), 2) + ", expected 0x" +
                mosaic::Hex(exp_mem_[first_bad - kShadowBase], 2));
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

  // Idle cycles with the memory refusing requests, so the endpoint stalls and a
  // store that would otherwise leave stays where it is.
  void RunIdleStalled(int n) {
    for (int i = 0; i < n; i++) {
      Stim s;
      s.mem_req_ready = false;
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
    Stim s;
    s.fill_valid = true;
    s.fill_id = MakeId(id);
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
  void CommitStore(const Ident& id, bool expect_ok) {
    Stim s;
    s.commit_valid = true;
    s.commit_id = MakeId(id);
    Cycle(s, false);
    Require(seen().commit_ok == expect_ok, "commit-outcome",
            "the authorisation of " + DescribeIdent(id) + " was " +
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
    // The squash this cycle, computed from the rule. Both the checks and the
    // shadow update use this one computation.
    std::vector<bool> drop;
    std::vector<bool> spared;
    if (s.squash_valid) {
      // A full flush ignores the window; the shadow also ignores the generation
      // for it, exactly as the RTL's predicate does.
      shadow_.SquashEffect(s.squash_all, s.squash_from, s.squash_tail, s.squash_gen, &drop,
                           &spared);
    } else {
      drop.assign(shadow_.size(), false);
      spared.assign(shadow_.size(), false);
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
    // The response side of the endpoint is tied to the queue's unconditional
    // accept, so it is always ready while the endpoint has something to give.
    Check(!(o.mem_rsp_ready == false && o.ep_busy == false), "memory-rsp-ready",
          "the endpoint reports idle but does not accept a response");
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

    // The squash this cycle (the same computation the checks used).
    std::vector<bool> drop;
    std::vector<bool> spared;
    if (s.squash_valid) {
      shadow_.SquashEffect(s.squash_all, s.squash_from, s.squash_tail, s.squash_gen, &drop,
                           &spared);
    } else {
      drop.assign(shadow_.size(), false);
      spared.assign(shadow_.size(), false);
    }

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
      shadow_.RemoveHead();
    }

    // 5. the memory model, driven by the *pre-edge* memory request, and the
    // expected bytes written from the specification rather than from the DUT.
    const uint32_t size_bytes = o.mem_req_valid ? SizeBytes(uint32_t(o.mem_req_size)) : 0;
    MemoryModel::EdgeResult edge =
        mem_.Edge(cycle_, o.mem_req_valid, s.mem_req_ready, o.mem_req_we, o.mem_req_addr,
                  size_bytes, SpecWdata(o.mem_req_wdata, o.mem_req_addr), o.mem_rsp_ready);
    if (edge.accepted && !sent_.empty()) {
      const Entry& w = sent_.front();
      if (!SpecMisaligned(w.addr(), w.size)) {
        for (uint32_t i = 0; i < SizeBytes(w.size); i++) {
          const uint32_t lane = uint32_t((w.addr() + i) & 7u);
          const uint64_t byte = (w.data >> (8u * lane)) & 0xffull;
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
        const Entry& w = sent_.front();
        const bool misaligned = SpecMisaligned(w.addr(), w.size);
        Require(misaligned, "endpoint-response",
                "a non-misaligned store completed without a memory transaction");
        Require(o.fault_ctr == fault_seen_ + 1, "endpoint-response",
                "a misaligned store did not raise a fault: o_fault_ctr=" + Dec(o.fault_ctr));
        Require(o.last_fault_cause == kExcStoreMisaligned, "endpoint-fault-cause",
                "expected the store-misaligned cause " + Dec(kExcStoreMisaligned) + ", got " +
                    Dec(o.last_fault_cause));
        Require(o.last_fault_tval == w.addr(), "endpoint-fault-tval",
                "expected tval 0x" + U64(w.addr()) + ", got 0x" + U64(o.last_fault_tval));
        Require(o.last_fault_id == w.packed, "endpoint-fault-id",
                "expected the faulting store " + U32(w.packed) + ", got " + U32(o.last_fault_id));
        sent_.erase(sent_.begin());
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
    if (o.fault_ctr != fault_seen_) fault_seen_ = o.fault_ctr;
    squash_seen_ = o.squash_ctr;
    squash_spared_seen_ = o.squash_spared_ctr;
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
  DutOut last_;

  // The stores the queue offered, in order. The front is the one the endpoint is
  // serving; a memory request must carry exactly it.
  std::vector<Entry> sent_;

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
  static constexpr uint64_t kShadowBytes = 4096;
  std::vector<uint8_t> exp_mem_;

 public:
  uint64_t run_allocs() const { return run_allocs_; }
  uint64_t run_drains() const { return run_drains_; }
  uint64_t run_squashes() const { return run_squashes_; }
  uint64_t run_faults() const { return run_faults_; }
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
  Vmosaic_store_queue_tb dut;

  std::string detail;
  bool passed = true;

  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    Bench bench(&dut, &clk, options.max_cycles);
    bench.ReadGeometry();
    (void)bench;
  } catch (const Failure& f) {
    std::fprintf(stderr, "setup failed: %s\n", f.what.c_str());
    dut.final();
    return mosaic::kExitFail;
  }

  dut.final();
  return mosaic::kExitFail;
}
