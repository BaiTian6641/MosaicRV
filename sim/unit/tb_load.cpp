// ============================================================================
// tb_load.cpp -- CASE=lsu.byte_forwarding, work package I-035.
//
// The DUT is never its own oracle. The shadow below is an independent model of
// the *contract* documented in rtl/core/mosaic_load_queue.sv and on the card:
//
//   * an ordered load queue, position 0 oldest, whose head is the load being
//     resolved;
//   * an ordered store list (the store queue's contents as the driver drove
//     them), each entry carrying address, size, data, both readiness bits and a
//     **program-order sequence number** -- the order the driver dispatched them;
//   * the card's rule applied per load byte: the byte comes from the youngest
//     store whose sequence number is smaller (older) and whose byte range covers
//     it, or from memory.
//
// The expectation is therefore built from the driver's own order, not from the
// RTL's generation arithmetic, so a defect in that arithmetic shows up as a
// divergence between the two. The store-queue entries and the load-queue entries
// are also compared field by field against the driver's model every cycle, so a
// drift in either shadow is caught rather than silently changing the
// expectation.
//
// A load's *memory* byte is the memory model's byte at that address. The memory
// model is written only by committed stores that the store queue drained through
// the second mosaic_lsu_endpoint, and the driver keeps its own expectation of
// those writes and compares them at the end of every phase.
//
// Geometry is read out of the elaborated DUT (`o_dut_*`), so this file contains
// no depth, no identity width and no field offset. The wrapper's own
// re-expression of the same numbers (`o_tb_*`) is required to agree field by
// field, so a profile change is a named failure here rather than a silently
// narrowed port.
//
// Phases, each of which resets first and fails on its own:
//
//   1. reset-state    the documented cold state.
//   2. narrow         a load narrower than a store, at every byte offset and for
//                     every size -- the "aligned word only" fail mode's arena.
//   3. wide           a load wider than a store, and a load spanning two stores,
//                     merged byte by byte.
//   4. youngest       three overlapping stores: the youngest *older* one wins,
//                     and a store younger than the load is never a source.
//   5. aliasing       different-size aliasing, checked byte by byte.
//   6. unknown        a resident store with no address blocks the load from
//                     memory, and the load completes with the right bytes once
//                     the address resolves.
//   7. same-cycle     a store's address arriving in the same cycle as the load's
//                     resolution gives the same bytes as one cycle earlier.
//   8. drain          a store that reached memory is read from memory
//                     afterwards, with no forwarded bytes.
//   9. faults         a misaligned load is reported as the endpoint's trap with
//                     no forwarded bytes.
//  10. soak           random allocation, fills, squashes, loads and memory
//                     back-pressure, shadow-compared every cycle.
//
// Standing invariants, checked on every cycle of every phase: the load queue's
// occupancy, entries and offer; its registered counters against the driver's
// tallies; its `alloc == done + count + held` identity; the store queue's
// entries and its forwarding query against the rule; and
// `o_lq_query_mismatch_ctr == 0` -- the two views of the same fact never
// disagree.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_load_queue_tb.h"

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
uint32_t SizeBytes(uint32_t size) {
  switch (size) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    default: return 8;
  }
}

// The load-side exception codes the endpoint reports, from mosaic_pkg.
constexpr uint64_t kExcLoadMisaligned = 4;

// --------------------------------------------------------------- the identity
// The order the driver dispatched an operation. The identity the DUT sees is
// derived from it the way mosaic_rob derives one (`rob_gen` advances once per
// allocation, `rob_index` is the allocation pointer), so the RTL's generation
// comparison and this file's sequence comparison are two encodings of one
// program order.
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

// --------------------------------------------------------------- the entries
struct SqEnt {
  uint32_t packed = 0;
  Ident id;
  uint64_t base = 0;
  uint64_t imm = 0;
  uint64_t data = 0;
  uint32_t size = 3;
  bool addr_valid = false;
  bool data_valid = false;
  bool authorised = false;
  uint64_t seq = 0;

  uint64_t addr() const { return base + imm; }
  uint64_t end() const { return addr() + SizeBytes(size); }
  bool Covers(uint64_t x) const { return x >= addr() && x < end(); }
};

struct LqEnt {
  uint32_t packed = 0;
  Ident id;
  uint64_t base = 0;
  uint64_t imm = 0;
  uint32_t size = 3;
  bool sign = false;
  bool fault = false;  // the driver's expectation for a trapping load
  uint64_t seq = 0;

  uint64_t addr() const { return base + imm; }
  uint32_t bytes() const { return SizeBytes(size); }
};

// ------------------------------------------------------------------ geometry
struct Geom {
  uint32_t lq_entries = 0;
  uint32_t lq_entry_w = 0;
  uint32_t lq_cnt_w = 0;
  uint32_t lq_idx_w = 0;
  uint32_t sq_entries = 0;
  uint32_t sq_entry_w = 0;
  uint32_t sq_cnt_w = 0;
  uint32_t xlen = 0;
  uint32_t id_w = 0;
  uint32_t rob_index_w = 0;
  uint32_t rob_gen_w = 0;
  uint32_t rob_slots = 0;
  uint32_t rob_gens = 0;

  // Load queue observation layout: size, signed, imm, base, id.
  int lq_off_size = 0;
  int lq_off_signed = 3;
  int lq_off_imm = 4;
  int lq_off_base = 0;
  int lq_off_id = 0;
  // Store queue observation layout: data_valid, addr_valid, data, size, imm, base, id.
  int sq_off_data_valid = 0;
  int sq_off_addr_valid = 1;
  int sq_off_data = 2;
  int sq_off_size = 0;
  int sq_off_imm = 0;
  int sq_off_base = 0;
  int sq_off_id = 0;
};

void DeriveOffsets(Geom* g) {
  g->lq_off_base = g->lq_off_imm + int(g->xlen);
  g->lq_off_id = g->lq_off_base + int(g->xlen);
  g->sq_off_size = g->sq_off_data + int(g->xlen);
  g->sq_off_imm = g->sq_off_size + 3;
  g->sq_off_base = g->sq_off_imm + int(g->xlen);
  g->sq_off_id = g->sq_off_base + int(g->xlen);
}

template <std::size_t N>
std::vector<uint32_t> WordsOf(const VlWide<N>& value) {
  std::vector<uint32_t> out(N);
  for (std::size_t i = 0; i < N; i++) out[i] = value[i];
  return out;
}

uint64_t Bits(const std::vector<uint32_t>& w, int off, int width) {
  uint64_t value = 0;
  for (int b = 0; b < width; b++) {
    const int bit = off + b;
    value |= uint64_t((w[size_t(bit) / 32u] >> (uint32_t(bit) % 32u)) & 1u) << b;
  }
  return value;
}

LqEnt DecodeLq(const Geom& g, const std::vector<uint32_t>& w, int base) {
  LqEnt e;
  e.size = uint32_t(Bits(w, base + g.lq_off_size, 3));
  e.sign = Bits(w, base + g.lq_off_signed, 1) != 0;
  e.imm = Bits(w, base + g.lq_off_imm, int(g.xlen));
  e.base = Bits(w, base + g.lq_off_base, int(g.xlen));
  e.packed = uint32_t(Bits(w, base + g.lq_off_id, int(g.id_w)));
  return e;
}

SqEnt DecodeSq(const Geom& g, const std::vector<uint32_t>& w, int base) {
  SqEnt e;
  e.data_valid = Bits(w, base + g.sq_off_data_valid, 1) != 0;
  e.addr_valid = Bits(w, base + g.sq_off_addr_valid, 1) != 0;
  e.data = Bits(w, base + g.sq_off_data, int(g.xlen));
  e.size = uint32_t(Bits(w, base + g.sq_off_size, 3));
  e.imm = Bits(w, base + g.sq_off_imm, int(g.xlen));
  e.base = Bits(w, base + g.sq_off_base, int(g.xlen));
  e.packed = uint32_t(Bits(w, base + g.sq_off_id, int(g.id_w)));
  return e;
}

// ------------------------------------------------------------ memory model
// A byte-addressed region behind the two endpoints, with two independent ports
// (one per endpoint) over one address space, programmable latency and request
// back-pressure. It does **not** check alignment: that policy lives in
// mosaic_lsu_endpoint.
class Memory {
 public:
  static constexpr uint64_t kBase = 0x80000000ull;
  static constexpr uint64_t kBytes = 0x4000;

  Memory() : mem_(kBytes) { Reset(); }

  static uint8_t Pattern(uint64_t i) { return uint8_t((i * 0x9dull + 0x37ull) & 0xffull); }

  void Reset() {
    for (uint64_t i = 0; i < kBytes; i++) mem_[i] = Pattern(i);
    lq = Port();
    sq = Port();
  }

  bool InAccess(uint64_t addr, uint64_t n) const {
    return addr >= kBase && (addr - kBase) + n <= kBytes;
  }
  uint8_t Peek(uint64_t addr) const { return InAccess(addr, 1) ? mem_[addr - kBase] : 0; }
  void Poke(uint64_t addr, uint8_t v) {
    if (InAccess(addr, 1)) mem_[addr - kBase] = v;
  }
  uint64_t LaneRead(uint64_t addr) const {
    const uint64_t block = addr & ~7ull;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= uint64_t(Peek(block + i)) << (8 * i);
    return v;
  }

  struct Port {
    bool pending = false;
    uint32_t delay = 0;
    bool fault = false;
    uint64_t rdata = 0;
    uint64_t accepted = 0;
    bool ReqReady(bool program_ready) const { return program_ready && !pending; }
    bool RspValid() const { return pending && delay == 0; }
  };

  struct EdgeResult {
    bool accepted = false;
    bool consumed = false;
  };

  // One cycle of the handshake, using the request signals presented *before*
  // the edge.
  EdgeResult Step(Port* p, uint32_t latency, bool mem_valid, bool program_ready, bool we,
                  uint64_t addr, uint64_t nbytes, uint64_t wdata, bool rsp_ready,
                  bool inject_fault) {
    EdgeResult r;
    if (!p->pending && mem_valid && program_ready) {
      r.accepted = true;
      p->accepted++;
      const bool bad = inject_fault || !InAccess(addr, nbytes);
      if (we) {
        for (uint64_t i = 0; i < nbytes; i++) {
          const uint32_t lane = uint32_t((addr + i) & 7u);
          Poke(addr + i, uint8_t(wdata >> (8 * lane)));
        }
      }
      p->pending = true;
      p->fault = bad;
      p->rdata = InAccess(addr, nbytes) ? LaneRead(addr) : 0;
      p->delay = latency;
      return r;
    }
    if (p->pending) {
      if (p->delay > 0) {
        p->delay--;
      } else if (p->RspValid() && rsp_ready) {
        p->pending = false;
        r.consumed = true;
      }
    }
    return r;
  }

  Port lq;
  Port sq;

 private:
  std::vector<uint8_t> mem_;
};

// ------------------------------------------------------------------- stimulus
struct Stim {
  bool lq_alloc_valid = false;
  uint32_t lq_alloc_id = 0;
  uint64_t lq_alloc_seq = 0;
  uint64_t lq_alloc_base = 0;
  uint64_t lq_alloc_imm = 0;
  uint32_t lq_alloc_size = 3;
  bool lq_alloc_signed = false;
  bool lq_result_ready = true;
  bool lq_mem_req_ready = true;
  uint32_t lq_latency = 0;
  bool lq_mem_fault = false;

  bool sq_alloc_valid = false;
  uint32_t sq_alloc_id = 0;
  uint64_t sq_alloc_seq = 0;
  uint64_t sq_alloc_base = 0;
  uint64_t sq_alloc_imm = 0;
  uint32_t sq_alloc_size = 3;
  bool sq_alloc_addr_valid = false;
  uint64_t sq_alloc_data = 0;
  bool sq_alloc_data_valid = false;

  bool sq_fill_valid = false;
  uint32_t sq_fill_id = 0;
  uint64_t sq_fill_base = 0;
  uint64_t sq_fill_imm = 0;
  bool sq_fill_addr_valid = false;
  uint64_t sq_fill_data = 0;
  bool sq_fill_data_valid = false;

  bool sq_commit_valid = false;
  uint32_t sq_commit_id = 0;

  bool sq_squash_valid = false;
  bool sq_squash_all = false;
  uint32_t sq_squash_from = 0;
  uint32_t sq_squash_tail = 0;
  uint32_t sq_squash_gen = 0;

  bool sq_mem_req_ready = true;
  uint32_t sq_latency = 0;
  bool sq_mem_fault = false;
};

// --------------------------------------------------------- the DUT snapshot
struct DutOut {
  bool lq_alloc_ready = false;
  bool lq_result_valid = false;
  uint32_t lq_result_id = 0;
  uint64_t lq_result_data = 0;
  uint64_t lq_result_cause = 0;
  uint64_t lq_result_tval = 0;
  bool lq_result_fault = false;
  uint32_t lq_result_fwd_mask = 0;

  bool lq_req_valid = false;
  uint32_t lq_req_id = 0;
  uint64_t lq_req_base = 0;
  uint64_t lq_req_imm = 0;
  uint32_t lq_req_size = 0;
  bool lq_req_signed = false;
  bool lq_ep_req_ready = false;
  bool lq_ep_rsp_valid = false;
  bool lq_rsp_ready = false;
  bool sq_ep_req_ready = false;

  bool lq_mem_req_valid = false;
  bool lq_mem_req_we = false;
  uint64_t lq_mem_req_addr = 0;
  uint32_t lq_mem_req_size = 0;
  uint32_t lq_mem_req_wstrb = 0;
  uint64_t lq_mem_req_wdata = 0;
  bool lq_mem_rsp_ready = false;

  bool sq_mem_req_valid = false;
  bool sq_mem_req_we = false;
  bool sq_mem_rsp_ready = false;
  uint64_t sq_mem_req_addr = 0;
  uint32_t sq_mem_req_size = 0;
  uint32_t sq_mem_req_wstrb = 0;
  uint64_t sq_mem_req_wdata = 0;
  bool sq_alloc_ready = false;
  bool sq_drain_req_valid = false;
  uint32_t sq_drain_req_id = 0;
  uint64_t sq_drain_req_base = 0;
  uint64_t sq_drain_req_imm = 0;
  uint32_t sq_drain_req_size = 0;
  uint64_t sq_drain_req_data = 0;

  uint32_t lq_count = 0;
  uint32_t lq_occ = 0;
  uint32_t lq_alloc_ctr = 0;
  uint32_t lq_issue_ctr = 0;
  uint32_t lq_rsp_ctr = 0;
  uint32_t lq_done_ctr = 0;
  uint32_t lq_replay_ctr = 0;
  uint32_t lq_blocked_ctr = 0;
  uint32_t lq_fwd_byte_ctr = 0;
  uint32_t lq_mem_byte_ctr = 0;
  uint32_t lq_fault_ctr = 0;
  uint32_t lq_query_mismatch_ctr = 0;
  uint64_t lq_last_fault_cause = 0;
  uint64_t lq_last_fault_tval = 0;
  std::vector<LqEnt> lq_entries;

  uint32_t sq_count = 0;
  uint32_t sq_auth_cnt = 0;
  uint32_t sq_occ = 0;
  uint32_t sq_entry_auth = 0;
  uint32_t sq_alloc_ctr = 0;
  uint32_t sq_fill_ctr = 0;
  uint32_t sq_commit_ctr = 0;
  uint32_t sq_drain_ctr = 0;
  uint32_t sq_squash_ctr = 0;
  std::vector<SqEnt> sq_entries;

  bool sq_fwd_valid = false;
  uint64_t sq_fwd_data = 0;
  bool sq_fwd_blocked = false;
  uint64_t sq_query_addr = 0;
  uint32_t sq_query_size = 0;

  bool sq_fill_hit = false;
  bool sq_commit_ok = false;
};

// --------------------------------------------------------- expected result
struct Expected {
  bool valid = false;
  uint32_t id = 0;
  uint64_t data = 0;
  uint32_t mask = 0;
  bool fault = false;
  uint64_t cause = 0;
  uint64_t tval = 0;
};

uint64_t Extend(uint64_t merged, uint32_t size, bool sign) {
  switch (size) {
    case 0:
      return sign ? uint64_t(int64_t(int8_t(uint8_t(merged)))) : uint64_t(uint8_t(merged));
    case 1:
      return sign ? uint64_t(int64_t(int16_t(uint16_t(merged)))) : uint64_t(uint16_t(merged));
    case 2:
      return sign ? uint64_t(int64_t(int32_t(uint32_t(merged)))) : uint64_t(uint32_t(merged));
    default:
      return merged;
  }
}

// -------------------------------------------------------------------- bench
class Bench {
 public:
  Bench(Vmosaic_load_queue_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  uint64_t checks() const { return checks_; }
  const DutOut& seen() const { return last_; }
  const Memory& mem() const { return mem_; }
  const Geom& geom() const { return g_; }
  const std::vector<SqEnt>& sq() const { return shadow_sq_; }
  const std::vector<LqEnt>& lq() const { return shadow_lq_; }
  uint64_t run_loads() const { return run_loads_; }
  uint64_t run_stores() const { return run_stores_; }
  uint64_t run_replays() const { return run_replays_; }
  uint64_t run_fwd_bytes() const { return run_fwd_bytes_; }
  uint64_t run_mem_bytes() const { return run_mem_bytes_; }
  uint64_t run_faults() const { return run_faults_; }

  // ------------------------------------------------------------- geometry
  void ReadGeometry() {
    Geom g;
    g.lq_entries = dut_->o_dut_lq_entries;
    g.lq_entry_w = dut_->o_dut_lq_entry_w;
    g.lq_cnt_w = dut_->o_dut_lq_cnt_w;
    g.lq_idx_w = dut_->o_dut_lq_idx_w;
    g.sq_entries = dut_->o_dut_sq_entries;
    g.sq_entry_w = dut_->o_dut_sq_entry_w;
    g.sq_cnt_w = dut_->o_dut_sq_cnt_w;
    g.xlen = dut_->o_dut_xlen;
    g.id_w = dut_->o_dut_id_w;
    g.rob_index_w = dut_->o_dut_rob_index_w;
    g.rob_gen_w = dut_->o_dut_rob_gen_w;
    Require(dut_->o_tb_lq_entries == g.lq_entries, "geometry", "wrapper LQ entries != DUT");
    Require(dut_->o_tb_lq_entry_w == g.lq_entry_w, "geometry", "wrapper LQ entry width != DUT");
    Require(dut_->o_tb_lq_cnt_w == g.lq_cnt_w, "geometry", "wrapper LQ count width != DUT");
    Require(dut_->o_tb_lq_idx_w == g.lq_idx_w, "geometry", "wrapper LQ index width != DUT");
    Require(dut_->o_tb_sq_entries == g.sq_entries, "geometry", "wrapper SQ entries != DUT");
    Require(dut_->o_tb_sq_entry_w == g.sq_entry_w, "geometry", "wrapper SQ entry width != DUT");
    Require(dut_->o_tb_sq_cnt_w == g.sq_cnt_w, "geometry", "wrapper SQ count width != DUT");
    Require(dut_->o_tb_xlen == g.xlen, "geometry", "wrapper XLEN != DUT");
    Require(dut_->o_tb_id_w == g.id_w, "geometry", "wrapper identity width != DUT");
    Require(dut_->o_tb_rob_index_w == g.rob_index_w, "geometry", "wrapper ROB index width != DUT");
    Require(dut_->o_tb_rob_gen_w == g.rob_gen_w, "geometry", "wrapper ROB gen width != DUT");

    Require(g.xlen == 64, "geometry", "XLEN is " + Dec(g.xlen));
    Require(g.lq_entries >= 2 && g.sq_entries >= 2, "geometry", "a queue is too shallow");
    Require(g.id_w <= 32, "geometry", "the identity is wider than the driver port");

    g.rob_slots = 1u << g.rob_index_w;
    g.rob_gens = 1u << g.rob_gen_w;
    DeriveOffsets(&g);
    Require(uint32_t(g.lq_off_id + int(g.id_w)) == g.lq_entry_w, "geometry",
            "the documented load entry layout does not sum to the DUT width");
    Require(uint32_t(g.sq_off_id + int(g.id_w)) == g.sq_entry_w, "geometry",
            "the documented store entry layout does not sum to the DUT width");
    g_ = g;
  }

  // An identity derived from the program-order sequence, the way mosaic_rob
  // derives one. Every operation in a phase is within 64 allocations of every
  // other live one (checked below), so the generation distance is unambiguous.
  Ident MakeIdent(uint64_t seq) const {
    Ident id;
    id.hart = 0;
    id.rob_index = uint32_t(seq % g_.rob_slots);
    id.rob_gen = uint32_t(seq % g_.rob_gens);
    id.uop_index = 0;
    return id;
  }

  uint32_t PackId(const Ident& id) const {
    uint32_t v = id.uop_index & 7u;
    v |= (id.rob_gen & (g_.rob_gens - 1u)) << 3;
    v |= (id.rob_index & (g_.rob_slots - 1u)) << (3 + g_.rob_gen_w);
    v |= (id.hart & 1u) << (3 + g_.rob_gen_w + g_.rob_index_w);
    return v;
  }

  // ---------------------------------------------------------------- checks
  void Require(bool ok, const std::string& where, const std::string& detail) {
    checks_++;
    phase_checks_++;
    if (!ok) Fail("[" + phase_name_ + "] " + where, detail + " @ cycle " + Dec(cycle_));
  }

  void Phase(const std::string& name) {
    phase_name_ = name;
    phase_checks_ = 0;
    std::printf("PHASE %-12s start @cycle %llu\n", name.c_str(),
                static_cast<unsigned long long>(clk_->cycle()));
  }

  Stim DefaultStim() const {
    Stim s;
    s.lq_result_ready = lq_result_ready_;
    s.lq_mem_req_ready = lq_mem_ready_;
    s.lq_latency = lq_lat_;
    s.sq_mem_req_ready = sq_mem_ready_;
    s.sq_latency = sq_lat_;
    return s;
  }

  void Fresh() {
    shadow_lq_.clear();
    shadow_sq_.clear();
    inflight_ = false;
    inflight_load_ = LqEnt();
    result_pending_ = false;
    pending_ = Expected();
    next_seq_ = 0;
    tallies_ = DutOut();
    mem_.Reset();
    exp_mem_.assign(Memory::kBytes, 0);
    for (uint64_t i = 0; i < Memory::kBytes; i++) exp_mem_[i] = Memory::Pattern(i);
    lq_lat_ = 0;
    lq_result_ready_ = true;
    lq_mem_ready_ = true;
    sq_lat_ = 0;
    sq_mem_ready_ = true;
    for (int i = 0; i < 2; i++) Cycle(Stim(), true);
    for (int i = 0; i < 4; i++) Cycle(DefaultStim(), false);
  }

  void EndPhase() {
    Cycle(DefaultStim(), false);
    Cycle(DefaultStim(), false);
    Require(shadow_lq_.empty(), "phase-end", "a load is still resident");
    Require(!result_pending_, "phase-end", "a result is still pending");
    Require(last_.lq_query_mismatch_ctr == 0, "phase-end",
            "the store queue's forwarding query disagreed with its entry view " +
                Dec(last_.lq_query_mismatch_ctr) + " time(s)");
    CheckMemory();
    run_loads_ += tallies_.lq_alloc_ctr;
    run_stores_ += tallies_.sq_alloc_ctr;
    run_replays_ += tallies_.lq_replay_ctr;
    run_fwd_bytes_ += last_.lq_fwd_byte_ctr;
    run_mem_bytes_ += last_.lq_mem_byte_ctr;
    run_faults_ += last_.lq_fault_ctr;
    std::printf("PHASE %-12s end   checks=%llu cycles=%llu loads=%u stores=%u replays=%u "
                "fwd_bytes=%u mem_bytes=%u\n",
                phase_name_.c_str(), static_cast<unsigned long long>(phase_checks_),
                static_cast<unsigned long long>(clk_->cycle()), tallies_.lq_alloc_ctr,
                tallies_.sq_alloc_ctr, tallies_.lq_replay_ctr, last_.lq_fwd_byte_ctr,
                last_.lq_mem_byte_ctr);
  }

  void CheckMemory() {
    uint64_t bad = 0;
    uint64_t first = 0;
    for (uint64_t i = 0; i < Memory::kBytes; i++) {
      if (mem_.Peek(Memory::kBase + i) != exp_mem_[i]) {
        if (bad == 0) first = Memory::kBase + i;
        bad++;
      }
    }
    const uint64_t shown = (bad == 0) ? Memory::kBase : first;
    Require(bad == 0, "memory-bytes",
            Dec(bad) + " byte(s) differ from the expected memory, first at 0x" + U64(shown) +
                ": memory 0x" + mosaic::Hex(mem_.Peek(shown), 2) + ", expected 0x" +
                mosaic::Hex(exp_mem_[shown - Memory::kBase], 2));
  }

  // ---------------------------------------------------------------- helpers
  void RunIdle(int n) {
    for (int i = 0; i < n; i++) Cycle(DefaultStim(), false);
  }

  void Settle(int max_cycles) {
    for (int i = 0; i < max_cycles; i++) {
      Cycle(DefaultStim(), false);
      if (shadow_lq_.empty() && !result_pending_ && !inflight_) return;
    }
    Fail("settle", "the load queue did not settle within " + Dec(uint64_t(max_cycles)) +
                       " cycles (" + Dec(shadow_lq_.size()) + " resident, result_pending=" +
                       Bool(result_pending_) + ")");
  }

  void AllocStore(const SqEnt& e) {
    for (int guard = 0; guard < 4 * int(g_.sq_entries) + 8; guard++) {
      Stim s = DefaultStim();
      s.sq_alloc_valid = true;
      s.sq_alloc_id = e.packed;
      s.sq_alloc_seq = e.seq;
      s.sq_alloc_base = e.base;
      s.sq_alloc_imm = e.imm;
      s.sq_alloc_size = e.size;
      s.sq_alloc_addr_valid = e.addr_valid;
      s.sq_alloc_data = e.data;
      s.sq_alloc_data_valid = e.data_valid;
      Cycle(s, false);
      if (last_.sq_alloc_ready) return;
    }
    Fail("alloc-store", "the store queue refused for too long");
  }

  void FillStore(uint32_t packed, bool addr_valid, uint64_t base, uint64_t imm, bool data_valid,
                 uint64_t data) {
    Stim s = DefaultStim();
    s.sq_fill_valid = true;
    s.sq_fill_id = packed;
    s.sq_fill_base = base;
    s.sq_fill_imm = imm;
    s.sq_fill_addr_valid = addr_valid;
    s.sq_fill_data = data;
    s.sq_fill_data_valid = data_valid;
    Cycle(s, false);
  }

  void CommitStore(uint32_t packed) {
    Stim s = DefaultStim();
    s.sq_commit_valid = true;
    s.sq_commit_id = packed;
    Cycle(s, false);
  }

  void SquashAll(uint32_t gen) {
    Stim s = DefaultStim();
    s.sq_squash_valid = true;
    s.sq_squash_all = true;
    s.sq_squash_gen = gen;
    Cycle(s, false);
  }

  SqEnt NewStore(uint64_t addr, uint64_t data, uint32_t size, bool addr_valid, bool data_valid) {
    SqEnt e;
    e.seq = next_seq_++;
    e.id = MakeIdent(e.seq);
    e.packed = PackId(e.id);
    e.base = addr;
    e.imm = 0;
    e.data = data;
    e.size = size;
    e.addr_valid = addr_valid;
    e.data_valid = data_valid;
    return e;
  }

  LqEnt NewLoad(uint64_t addr, uint32_t size, bool sign, bool fault = false) {
    LqEnt e;
    e.seq = next_seq_++;
    e.id = MakeIdent(e.seq);
    e.packed = PackId(e.id);
    e.base = addr;
    e.imm = 0;
    e.size = size;
    e.sign = sign;
    // The profile traps a misaligned access, so a load that crosses the
    // boundary its size demands is an expectation of a trap, not of a value.
    e.fault = fault || ((addr & (uint64_t(SizeBytes(size)) - 1u)) != 0);
    return e;
  }

  void OfferLoad(const LqEnt& e) {
    for (int guard = 0; guard < 4 * int(g_.lq_entries) + 8; guard++) {
      Stim s = DefaultStim();
      s.lq_alloc_valid = true;
      s.lq_alloc_id = e.packed;
      s.lq_alloc_seq = e.seq;
      s.lq_alloc_base = e.base;
      s.lq_alloc_imm = e.imm;
      s.lq_alloc_size = e.size;
      s.lq_alloc_signed = e.sign;
      Cycle(s, false);
      if (last_.lq_alloc_ready) return;
    }
    Fail("offer-load", "the load queue refused for too long");
  }

  void AllocLoadRun(const LqEnt& e) {
    OfferLoad(e);
    Settle(1024);
  }

  // ------------------------------------------- the driver's own expectations
  uint64_t LiveOldestSeq() const {
    uint64_t best = std::numeric_limits<uint64_t>::max();
    bool any = false;
    for (const SqEnt& s : shadow_sq_) { best = std::min(best, s.seq); any = true; }
    for (const LqEnt& l : shadow_lq_) { best = std::min(best, l.seq); any = true; }
    return any ? best : 0;
  }

  bool BlockedFor(const LqEnt& l) const {
    for (const SqEnt& s : shadow_sq_) {
      if (!s.addr_valid) return true;  // any resident store with no address
    }
    for (uint32_t i = 0; i < l.bytes(); i++) {
      const uint64_t x = l.addr() + i;
      int governing = -1;
      for (size_t j = 0; j < shadow_sq_.size(); j++) {
        const SqEnt& s = shadow_sq_[j];
        if (s.seq < l.seq && s.addr_valid && s.Covers(x)) governing = int(j);
      }
      if (governing >= 0 && !shadow_sq_[size_t(governing)].data_valid) return true;
    }
    return false;
  }

  Expected ExpectedFor(const LqEnt& l) const {
    Expected r;
    r.valid = true;
    r.id = l.packed;
    if (l.fault) {
      r.fault = true;
      r.cause = kExcLoadMisaligned;
      r.tval = l.addr();
      r.mask = 0;
      return r;
    }
    uint64_t merged = 0;
    uint32_t mask = 0;
    for (uint32_t i = 0; i < l.bytes(); i++) {
      const uint64_t x = l.addr() + i;
      int src = -1;
      for (size_t j = 0; j < shadow_sq_.size(); j++) {
        const SqEnt& s = shadow_sq_[j];
        if (s.seq < l.seq && s.addr_valid && s.data_valid && s.Covers(x)) src = int(j);
      }
      uint8_t byte = 0;
      if (src >= 0) {
        const SqEnt& s = shadow_sq_[size_t(src)];
        byte = uint8_t(s.data >> (8 * (x - s.addr())));
        mask |= 1u << i;
      } else {
        byte = exp_mem_[x - Memory::kBase];
      }
      merged |= uint64_t(byte) << (8 * i);
    }
    r.data = Extend(merged, l.size, l.sign);
    r.mask = mask;
    return r;
  }

  // The store queue's forwarding rule, applied to the entries the driver drove.
  void QueryExpect(uint64_t qaddr, uint32_t qsize, bool* valid, uint64_t* data,
                   bool* blocked) const {
    *valid = false;
    *data = 0;
    *blocked = false;
    const uint32_t qb = SizeBytes(qsize);
    for (const SqEnt& s : shadow_sq_) {
      if (!s.addr_valid) *blocked = true;
      if (!s.addr_valid || !s.data_valid) continue;
      if ((s.addr() >> 3) != (qaddr >> 3)) continue;
      const uint32_t e_lo = uint32_t(s.addr() & 7u);
      const uint32_t q_lo = uint32_t(qaddr & 7u);
      if (e_lo <= q_lo && (q_lo + qb) <= (e_lo + SizeBytes(s.size))) {
        *valid = true;
        *data = s.data << (8 * (s.addr() & 7u));
      }
    }
  }

  // ------------------------------------------------------------------ cycles
  void Cycle(const Stim& s, bool rst) {
    cycle_ = clk_->cycle();
    Require(clk_->cycle() <= max_cycles_, "cycle-budget", "the run exceeded --max-cycles");

    dut_->rst = rst ? 1 : 0;

    dut_->lq_alloc_valid = s.lq_alloc_valid ? 1 : 0;
    dut_->lq_alloc_id = s.lq_alloc_id;
    dut_->lq_alloc_base = s.lq_alloc_base;
    dut_->lq_alloc_imm = s.lq_alloc_imm;
    dut_->lq_alloc_size = s.lq_alloc_size;
    dut_->lq_alloc_signed = s.lq_alloc_signed ? 1 : 0;
    dut_->lq_result_ready = s.lq_result_ready ? 1 : 0;

    dut_->sq_alloc_valid = s.sq_alloc_valid ? 1 : 0;
    dut_->sq_alloc_id = s.sq_alloc_id;
    dut_->sq_alloc_base = s.sq_alloc_base;
    dut_->sq_alloc_imm = s.sq_alloc_imm;
    dut_->sq_alloc_size = s.sq_alloc_size;
    dut_->sq_alloc_addr_valid = s.sq_alloc_addr_valid ? 1 : 0;
    dut_->sq_alloc_data = s.sq_alloc_data;
    dut_->sq_alloc_data_valid = s.sq_alloc_data_valid ? 1 : 0;

    dut_->sq_fill_valid = s.sq_fill_valid ? 1 : 0;
    dut_->sq_fill_id = s.sq_fill_id;
    dut_->sq_fill_base = s.sq_fill_base;
    dut_->sq_fill_imm = s.sq_fill_imm;
    dut_->sq_fill_addr_valid = s.sq_fill_addr_valid ? 1 : 0;
    dut_->sq_fill_data = s.sq_fill_data;
    dut_->sq_fill_data_valid = s.sq_fill_data_valid ? 1 : 0;

    dut_->sq_commit_valid = s.sq_commit_valid ? 1 : 0;
    dut_->sq_commit_id = s.sq_commit_id;
    dut_->sq_squash_valid = s.sq_squash_valid ? 1 : 0;
    dut_->sq_squash_all = s.sq_squash_all ? 1 : 0;
    dut_->sq_squash_from = s.sq_squash_from;
    dut_->sq_squash_tail = s.sq_squash_tail;
    dut_->sq_squash_gen = s.sq_squash_gen;

    // The memory models' current state drives the response side and the request
    // readiness, before the edge.
    dut_->lq_mem_req_ready = mem_.lq.ReqReady(s.lq_mem_req_ready) ? 1 : 0;
    dut_->lq_mem_rsp_valid = mem_.lq.RspValid() ? 1 : 0;
    dut_->lq_mem_rsp_rdata = mem_.lq.rdata;
    dut_->lq_mem_rsp_fault = mem_.lq.fault ? 1 : 0;

    dut_->sq_mem_req_ready = mem_.sq.ReqReady(s.sq_mem_req_ready) ? 1 : 0;
    dut_->sq_mem_rsp_valid = mem_.sq.RspValid() ? 1 : 0;
    dut_->sq_mem_rsp_rdata = mem_.sq.rdata;
    dut_->sq_mem_rsp_fault = mem_.sq.fault ? 1 : 0;

    dut_->clk = 0;
    dut_->eval();
    last_ = Read();

    if (rst) {
      CommitEdge(s, true);
      dut_->clk = 1;
      dut_->eval();
      dut_->clk = 0;
      dut_->eval();
      clk_->Tick();
      return;
    }

    CheckCycle(last_, s);
    CommitEdge(s, false);

    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    clk_->Tick();
    cycle_ = clk_->cycle();
  }

 private:
  DutOut Read() {
    DutOut o;
    o.lq_alloc_ready = dut_->lq_alloc_ready != 0;
    o.lq_result_valid = dut_->lq_result_valid != 0;
    o.lq_result_id = dut_->lq_result_id;
    o.lq_result_data = dut_->lq_result_data;
    o.lq_result_cause = dut_->lq_result_cause;
    o.lq_result_tval = dut_->lq_result_tval;
    o.lq_result_fault = dut_->lq_result_fault != 0;
    o.lq_result_fwd_mask = dut_->lq_result_fwd_mask;

    o.lq_req_valid = dut_->o_lq_req_valid != 0;
    o.lq_req_id = dut_->o_lq_req_id;
    o.lq_req_base = dut_->o_lq_req_base;
    o.lq_req_imm = dut_->o_lq_req_imm;
    o.lq_req_size = dut_->o_lq_req_size;
    o.lq_req_signed = dut_->o_lq_req_signed != 0;
    o.lq_ep_req_ready = dut_->o_lq_ep_req_ready != 0;
    o.lq_ep_rsp_valid = dut_->o_lq_ep_rsp_valid != 0;
    o.lq_rsp_ready = dut_->o_lq_rsp_ready != 0;
    o.sq_ep_req_ready = dut_->o_sq_ep_req_ready != 0;

    o.lq_mem_req_valid = dut_->lq_mem_req_valid != 0;
    o.lq_mem_req_we = dut_->lq_mem_req_we != 0;
    o.lq_mem_req_addr = dut_->lq_mem_req_addr;
    o.lq_mem_req_size = dut_->lq_mem_req_size;
    o.lq_mem_req_wstrb = dut_->lq_mem_req_wstrb;
    o.lq_mem_req_wdata = dut_->lq_mem_req_wdata;
    o.lq_mem_rsp_ready = dut_->lq_mem_rsp_ready != 0;

    o.sq_mem_req_valid = dut_->sq_mem_req_valid != 0;
    o.sq_mem_req_we = dut_->sq_mem_req_we != 0;
    o.sq_mem_rsp_ready = dut_->sq_mem_rsp_ready != 0;
    o.sq_mem_req_addr = dut_->sq_mem_req_addr;
    o.sq_mem_req_size = dut_->sq_mem_req_size;
    o.sq_mem_req_wstrb = dut_->sq_mem_req_wstrb;
    o.sq_mem_req_wdata = dut_->sq_mem_req_wdata;
    o.sq_alloc_ready = dut_->sq_alloc_ready != 0;
    o.sq_drain_req_valid = dut_->sq_drain_req_valid != 0;
    o.sq_drain_req_id = dut_->sq_drain_req_id;
    o.sq_drain_req_base = dut_->sq_drain_req_base;
    o.sq_drain_req_imm = dut_->sq_drain_req_imm;
    o.sq_drain_req_size = dut_->sq_drain_req_size;
    o.sq_drain_req_data = dut_->sq_drain_req_data;

    o.lq_count = dut_->o_lq_count;
    o.lq_occ = dut_->o_lq_occ;
    o.lq_alloc_ctr = dut_->o_lq_alloc_ctr;
    o.lq_issue_ctr = dut_->o_lq_issue_ctr;
    o.lq_rsp_ctr = dut_->o_lq_rsp_ctr;
    o.lq_done_ctr = dut_->o_lq_done_ctr;
    o.lq_replay_ctr = dut_->o_lq_replay_ctr;
    o.lq_blocked_ctr = dut_->o_lq_blocked_ctr;
    o.lq_fwd_byte_ctr = dut_->o_lq_fwd_byte_ctr;
    o.lq_mem_byte_ctr = dut_->o_lq_mem_byte_ctr;
    o.lq_fault_ctr = dut_->o_lq_fault_ctr;
    o.lq_query_mismatch_ctr = dut_->o_lq_query_mismatch_ctr;
    o.lq_last_fault_cause = dut_->o_lq_last_fault_cause;
    o.lq_last_fault_tval = dut_->o_lq_last_fault_tval;

    const std::vector<uint32_t> lqw = WordsOf(dut_->o_lq_entry_pay);
    o.lq_entries.clear();
    for (uint32_t i = 0; i < g_.lq_entries; i++) {
      o.lq_entries.push_back(DecodeLq(g_, lqw, int(i) * int(g_.lq_entry_w)));
    }

    o.sq_count = dut_->o_sq_count;
    o.sq_auth_cnt = dut_->o_sq_auth_cnt;
    o.sq_occ = dut_->o_sq_occ;
    o.sq_entry_auth = dut_->o_sq_entry_authorised;
    o.sq_alloc_ctr = dut_->o_sq_alloc_ctr;
    o.sq_fill_ctr = dut_->o_sq_fill_ctr;
    o.sq_commit_ctr = dut_->o_sq_commit_ctr;
    o.sq_drain_ctr = dut_->o_sq_drain_ctr;
    o.sq_squash_ctr = dut_->o_sq_squash_ctr;
    o.sq_fill_hit = dut_->sq_fill_hit != 0;
    o.sq_commit_ok = dut_->sq_commit_ok != 0;

    const std::vector<uint32_t> sqw = WordsOf(dut_->o_sq_entry_pay);
    o.sq_entries.clear();
    for (uint32_t i = 0; i < g_.sq_entries; i++) {
      o.sq_entries.push_back(DecodeSq(g_, sqw, int(i) * int(g_.sq_entry_w)));
    }

    o.sq_fwd_valid = dut_->o_sq_fwd_valid != 0;
    o.sq_fwd_data = dut_->o_sq_fwd_data;
    o.sq_fwd_blocked = dut_->o_sq_fwd_blocked != 0;
    o.sq_query_addr = dut_->o_sq_query_addr;
    o.sq_query_size = dut_->o_sq_query_size;
    return o;
  }

  // ------------------------------------------------------- the per-cycle check
  void CheckCycle(const DutOut& o, const Stim& s) {
    // Program order must be readable from the generation field: keep every live
    // operation inside half the generation modulus, or the RTL's half-modulus
    // comparison would be ambiguous and the test would be checking nothing.
    {
      uint64_t newest = 0;
      for (const SqEnt& e : shadow_sq_) newest = std::max(newest, e.seq);
      for (const LqEnt& e : shadow_lq_) newest = std::max(newest, e.seq);
      if (!shadow_sq_.empty() || !shadow_lq_.empty()) {
        Require(newest - LiveOldestSeq() < 64, "seq-window",
                "the scenario spans " + Dec(newest - LiveOldestSeq()) +
                    " allocations, more than the generation can order unambiguously");
      }
    }

    // 1. the load queue's occupancy, entries and offer.
    Require(o.lq_count == shadow_lq_.size(), "lq-count",
            "count " + Dec(o.lq_count) + ", model " + Dec(shadow_lq_.size()));
    for (uint32_t i = 0; i < g_.lq_entries; i++) {
      const bool resident = i < shadow_lq_.size();
      Require(((o.lq_occ >> i) & 1u) == (resident ? 1u : 0u), "lq-occ",
              "position " + Dec(i) + " occupancy " + Bool(((o.lq_occ >> i) & 1u) != 0));
      if (!resident) continue;
      const LqEnt& m = shadow_lq_[i];
      const LqEnt& d = o.lq_entries[i];
      Require(d.packed == m.packed && d.base == m.base && d.imm == m.imm && d.size == m.size &&
                  d.sign == m.sign,
              "lq-entry", "position " + Dec(i) + ": DUT " + DescribeIdent(d.id) + " addr 0x" +
                              U64(d.addr()) + "/" + Dec(d.size) + ", model " +
                              DescribeIdent(m.id) + " addr 0x" + U64(m.addr()) + "/" + Dec(m.size));
    }
    Require(o.lq_alloc_ready == (shadow_lq_.size() < g_.lq_entries), "lq-alloc-ready",
            "alloc_ready_o=" + Bool(o.lq_alloc_ready) + " with " + Dec(shadow_lq_.size()) +
                " resident");

    const bool expect_offer = !shadow_lq_.empty() && !inflight_ && !result_pending_;
    Require(o.lq_req_valid == expect_offer, "lq-offer",
            "req_valid_o=" + Bool(o.lq_req_valid) + ", expected " + Bool(expect_offer));
    if (o.lq_req_valid) {
      const LqEnt& m = shadow_lq_[0];
      Require(o.lq_req_id == m.packed && o.lq_req_base == m.base && o.lq_req_imm == m.imm &&
                  o.lq_req_size == m.size && o.lq_req_signed == m.sign,
              "lq-offer-payload", "the offer is not the head load");
    }

    // 2. the result, when presented, is the pending expectation.
    Require(o.lq_result_valid == result_pending_, "lq-result-valid",
            "result_valid_o=" + Bool(o.lq_result_valid) + ", model " + Bool(result_pending_));
    if (o.lq_result_valid) {
      const Expected& e = pending_;
      if (std::getenv("MOSAIC_DEBUG") != nullptr) {
        std::fprintf(stderr, "DBG result load seq=%llu addr=0x%llx/%u mask=0x%02x expect=0x%02x",
                     static_cast<unsigned long long>(pending_load_.seq),
                     static_cast<unsigned long long>(pending_load_.addr()), pending_load_.size,
                     o.lq_result_fwd_mask, e.mask);
        for (const SqEnt& s : shadow_sq_) {
          std::fprintf(stderr, " sq[seq=%llu a0x%llx/%u av%d dv%d]",
                       static_cast<unsigned long long>(s.seq),
                       static_cast<unsigned long long>(s.addr()), s.size, s.addr_valid ? 1 : 0,
                       s.data_valid ? 1 : 0);
        }
        for (const LqEnt& l : shadow_lq_) {
          std::fprintf(stderr, " lq[seq=%llu a0x%llx/%u]",
                       static_cast<unsigned long long>(l.seq),
                       static_cast<unsigned long long>(l.addr()), l.size);
        }
        std::fprintf(stderr, "\n");
      }
      Require(o.lq_result_id == e.id, "lq-result-id",
              "result id " + U32(o.lq_result_id) + ", expected " + U32(e.id));
      Require(o.lq_result_fwd_mask == e.mask, "lq-result-mask",
              "forward mask " + U32(o.lq_result_fwd_mask) + ", expected " + U32(e.mask));
      if (e.fault) {
        Require(o.lq_result_fault, "lq-result-fault", "expected a trapping load");
        Require(o.lq_result_cause == e.cause, "lq-result-cause",
                "cause " + Dec(o.lq_result_cause) + ", expected " + Dec(e.cause));
        Require(o.lq_result_tval == e.tval, "lq-result-tval", "tval differs");
      } else {
        Require(!o.lq_result_fault, "lq-result-fault", "an unexpected fault");
        Require(o.lq_result_data == e.data, "lq-result-data",
                "data 0x" + U64(o.lq_result_data) + ", expected 0x" + U64(e.data));
      }
    }

    // 3. the load's registered counters and its conservation identity.
    Require(o.lq_alloc_ctr == tallies_.lq_alloc_ctr, "lq-alloc-ctr",
            "alloc counter " + Dec(o.lq_alloc_ctr) + ", model " + Dec(tallies_.lq_alloc_ctr));
    Require(o.lq_done_ctr == tallies_.lq_done_ctr, "lq-done-ctr",
            "done counter " + Dec(o.lq_done_ctr) + ", model " + Dec(tallies_.lq_done_ctr));
    Require(o.lq_alloc_ctr == o.lq_done_ctr + o.lq_count, "lq-conservation",
            "alloc " + Dec(o.lq_alloc_ctr) + " != done " + Dec(o.lq_done_ctr) + " + count " +
                Dec(o.lq_count));

    // 4. the store queue's entries against the driver's own model, so the
    //    forwarding expectation is built on a verified store list.
    Require(o.sq_count == shadow_sq_.size(), "sq-count",
            "count " + Dec(o.sq_count) + ", model " + Dec(shadow_sq_.size()));
    for (uint32_t i = 0; i < g_.sq_entries; i++) {
      const bool resident = i < shadow_sq_.size();
      if (!resident) continue;
      const SqEnt& m = shadow_sq_[i];
      const SqEnt& d = o.sq_entries[i];
      Require(d.packed == m.packed && d.base == m.base && d.imm == m.imm && d.size == m.size &&
                  d.addr_valid == m.addr_valid && d.data_valid == m.data_valid &&
                  d.data == m.data,
              "sq-entry", "position " + Dec(i) + " differs from the driver's store list");
    }

    // 5. the store queue's forwarding query against its rule, applied to the
    //    entries the driver drove. The load queue drives the query, so the
    //    address and size under test are the ones it actually issued.
    {
      bool qv = false;
      uint64_t qd = 0;
      bool qb = false;
      QueryExpect(o.sq_query_addr, o.sq_query_size, &qv, &qd, &qb);
      Require(o.sq_fwd_valid == qv, "sq-fwd-valid",
              "fwd_valid_o=" + Bool(o.sq_fwd_valid) + ", expected " + Bool(qv) + " for addr 0x" +
                  U64(o.sq_query_addr) + "/" + Dec(o.sq_query_size));
      Require(o.sq_fwd_blocked == qb, "sq-fwd-blocked",
              "fwd_blocked_o=" + Bool(o.sq_fwd_blocked) + ", expected " + Bool(qb));
      if (qv) {
        Require(o.sq_fwd_data == qd, "sq-fwd-data",
                "fwd_data_o=0x" + U64(o.sq_fwd_data) + ", expected 0x" + U64(qd));
      }
    }
    Require(o.lq_query_mismatch_ctr == 0, "lq-query-cross-check",
            "the load queue's query cross-check fired " + Dec(o.lq_query_mismatch_ctr) +
                " time(s)");

    // 6. the load endpoint's request belongs to the load in flight.
    if (o.lq_mem_req_valid && inflight_) {
      Require(!o.lq_mem_req_we, "lq-mem", "the load path issued a store request");
      Require(o.lq_mem_req_addr == inflight_load_.addr(), "lq-mem-addr",
              "the load request address is not the in-flight load's");
      Require(o.lq_mem_req_size == inflight_load_.size, "lq-mem-size",
              "the load request size is not the in-flight load's");
    }

    // 7. the store queue's drain offer is position 0 when it is offered.
    if (o.sq_drain_req_valid && !shadow_sq_.empty()) {
      const SqEnt& m = shadow_sq_[0];
      Require(o.sq_drain_req_id == m.packed && o.sq_drain_req_base == m.base &&
                  o.sq_drain_req_imm == m.imm && o.sq_drain_req_size == m.size &&
                  o.sq_drain_req_data == m.data,
              "sq-drain", "the drain offer is not position 0");
    }
  }

  // --------------------------------------------- apply the cycle's edge
  void CommitEdge(const Stim& s, bool rst) {
    if (rst) {
      shadow_lq_.clear();
      shadow_sq_.clear();
      inflight_ = false;
      result_pending_ = false;
      pending_ = Expected();
      tallies_ = DutOut();
      return;
    }

    // The memory ports advance with the pre-edge request signals. The consumer's
    // response readiness is the endpoint's own `mem_rsp_ready_o`.
    mem_.Step(&mem_.lq, s.lq_latency, last_.lq_mem_req_valid, s.lq_mem_req_ready,
              last_.lq_mem_req_we, last_.lq_mem_req_addr, SizeBytes(last_.lq_mem_req_size),
              last_.lq_mem_req_wdata, last_.lq_mem_rsp_ready, s.lq_mem_fault);
    mem_.Step(&mem_.sq, s.sq_latency, last_.sq_mem_req_valid, s.sq_mem_req_ready,
              last_.sq_mem_req_we, last_.sq_mem_req_addr, SizeBytes(last_.sq_mem_req_size),
              last_.sq_mem_req_wdata, last_.sq_mem_rsp_ready, s.sq_mem_fault);

    // ---- load queue ----
    const bool rsp_now = last_.lq_ep_rsp_valid && last_.lq_rsp_ready;
    bool blocked = false;
    if (!shadow_lq_.empty()) blocked = BlockedFor(shadow_lq_[0]);
    const bool complete = rsp_now && !shadow_lq_.empty() && !blocked;
    const bool issue = last_.lq_req_valid && last_.lq_ep_req_ready;
    const bool result_taken = last_.lq_result_valid && s.lq_result_ready;

    if (issue) {
      inflight_ = true;
      inflight_load_ = shadow_lq_[0];
    }
    if (rsp_now) {
      inflight_ = false;
      if (complete) {
        pending_ = ExpectedFor(shadow_lq_[0]);
        pending_load_ = shadow_lq_[0];
        shadow_lq_.erase(shadow_lq_.begin());
        result_pending_ = true;
        tallies_.lq_done_ctr++;
      } else {
        tallies_.lq_replay_ctr++;
      }
    }
    if (result_taken) result_pending_ = false;
    if (s.lq_alloc_valid && last_.lq_alloc_ready) {
      LqEnt e;
      e.packed = s.lq_alloc_id;
      e.seq = s.lq_alloc_seq;
      e.id = MakeIdent(e.seq);
      e.base = s.lq_alloc_base;
      e.imm = s.lq_alloc_imm;
      e.size = s.lq_alloc_size;
      e.sign = s.lq_alloc_signed;
      e.fault = ((e.base & (uint64_t(SizeBytes(e.size)) - 1u)) != 0);
      shadow_lq_.push_back(e);
      tallies_.lq_alloc_ctr++;
    }

    // ---- store queue ----
    std::vector<SqEnt> filled = shadow_sq_;
    if (last_.sq_fill_hit) {
      for (size_t i = 0; i < filled.size(); i++) {
        if (filled[i].packed == s.sq_fill_id) {
          if (s.sq_fill_addr_valid) {
            filled[i].base = s.sq_fill_base;
            filled[i].imm = s.sq_fill_imm;
          }
          if (s.sq_fill_data_valid) filled[i].data = s.sq_fill_data;
          filled[i].addr_valid = filled[i].addr_valid || s.sq_fill_addr_valid;
          filled[i].data_valid = filled[i].data_valid || s.sq_fill_data_valid;
          break;
        }
      }
    }

    if (last_.sq_commit_ok && !filled.empty()) {
      filled[0].authorised = true;  // a commit names the first unauthorised entry
      tallies_.sq_commit_ctr++;
    }

    std::vector<bool> drop(filled.size(), false);
    if (s.sq_squash_valid) {
      for (size_t i = 0; i < filled.size(); i++) {
        bool region = s.sq_squash_all;
        if (!region) {
          region = (filled[i].id.rob_gen == s.sq_squash_gen) &&
                   (((filled[i].id.rob_index - s.sq_squash_from) & (g_.rob_slots - 1u)) <
                    ((s.sq_squash_tail - s.sq_squash_from) & (g_.rob_slots - 1u)));
        }
        if (region && !filled[i].authorised) drop[i] = true;
      }
    }

    const bool drain_accept = last_.sq_drain_req_valid && last_.sq_ep_req_ready;
    std::vector<SqEnt> next_sq;
    for (size_t i = 0; i < filled.size(); i++) {
      if (drop[i]) {
        tallies_.sq_squash_ctr++;
        continue;
      }
      if (drain_accept && i == 0) {
        ExpectedWrite(filled[i]);
        tallies_.sq_drain_ctr++;
        continue;
      }
      next_sq.push_back(filled[i]);
    }
    if (s.sq_alloc_valid && last_.sq_alloc_ready) {
      SqEnt e;
      e.packed = s.sq_alloc_id;
      e.seq = s.sq_alloc_seq;
      e.id = MakeIdent(e.seq);
      e.base = s.sq_alloc_base;
      e.imm = s.sq_alloc_imm;
      e.data = s.sq_alloc_data;
      e.size = s.sq_alloc_size;
      e.addr_valid = s.sq_alloc_addr_valid;
      e.data_valid = s.sq_alloc_data_valid;
      e.authorised = false;
      next_sq.push_back(e);
      tallies_.sq_alloc_ctr++;
    }
    shadow_sq_ = next_sq;
  }

  // The bytes a drained store should write, applied to the driver's memory
  // expectation.
  void ExpectedWrite(const SqEnt& e) {
    if (!e.addr_valid || !e.data_valid) return;
    const uint32_t n = SizeBytes(e.size);
    for (uint32_t i = 0; i < n; i++) {
      const uint64_t a = e.addr() + i;
      if (a < Memory::kBase || (a - Memory::kBase) >= Memory::kBytes) continue;
      exp_mem_[a - Memory::kBase] = uint8_t(e.data >> (8 * i));
    }
  }

  Vmosaic_load_queue_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;
  Geom g_;
  Memory mem_;
  std::vector<uint8_t> exp_mem_;

  std::vector<LqEnt> shadow_lq_;
  std::vector<SqEnt> shadow_sq_;
  bool inflight_ = false;
  LqEnt inflight_load_;
  bool result_pending_ = false;
  Expected pending_;
  LqEnt pending_load_;
  uint64_t next_seq_ = 0;

  DutOut last_;
  DutOut tallies_;
  std::string phase_name_;
  uint64_t checks_ = 0;
  uint64_t phase_checks_ = 0;
  uint64_t cycle_ = 0;

  uint32_t lq_lat_ = 0;
  bool lq_result_ready_ = true;
  bool lq_mem_ready_ = true;
  uint32_t sq_lat_ = 0;
  bool sq_mem_ready_ = true;

  uint64_t run_loads_ = 0, run_stores_ = 0, run_replays_ = 0;
  uint64_t run_fwd_bytes_ = 0, run_mem_bytes_ = 0, run_faults_ = 0;

 public:
  // ------------------------------------------------------------- phases
  void PhaseResetState() {
    Phase("reset-state");
    Fresh();
    Require(last_.lq_count == 0 && last_.lq_occ == 0, "reset", "the load queue is not empty");
    Require(!last_.lq_req_valid && !last_.lq_result_valid, "reset", "something is offered");
    Require(last_.lq_alloc_ctr == 0 && last_.lq_issue_ctr == 0 && last_.lq_rsp_ctr == 0 &&
                last_.lq_done_ctr == 0 && last_.lq_replay_ctr == 0 && last_.lq_fault_ctr == 0,
            "reset", "a load counter is not zero");
    Require(last_.sq_count == 0, "reset", "the store queue is not empty");
    Require(mem_.lq.accepted == 0 && mem_.sq.accepted == 0, "reset",
            "a memory transaction was issued from the cold state");
    EndPhase();
  }

  // 2. a load narrower than a store, at every byte offset and size.
  void PhaseNarrow() {
    Phase("narrow");
    Fresh();
    const uint64_t kAddr = Memory::kBase + 0x100;
    const uint64_t kData = 0x8899aabbccddeeffull;
    AllocStore(NewStore(kAddr, kData, 3, true, true));
    for (uint32_t size = 0; size < 4; size++) {
      for (uint32_t off = 0; off + SizeBytes(size) <= 8; off += SizeBytes(size)) {
        AllocLoadRun(NewLoad(kAddr + off, size, false));
        AllocLoadRun(NewLoad(kAddr + off, size, true));
      }
    }
    // Every byte came from the store.
    Require(last_.lq_mem_byte_ctr == 0, "narrow", "a byte was taken from memory");
    Require(last_.lq_fwd_byte_ctr > 0, "narrow", "nothing was forwarded");
    EndPhase();
  }

  // 3. a load wider than a store, and a load spanning two stores.
  void PhaseWide() {
    Phase("wide");
    Fresh();
    const uint64_t kAddr = Memory::kBase + 0x200;
    const uint64_t d0 = 0x1111111122222222ull;  // bytes 0..3
    const uint64_t d1 = 0x3333333344444444ull;  // bytes 4..7
    AllocStore(NewStore(kAddr, d0, 2, true, true));      // word at +0
    AllocStore(NewStore(kAddr + 4, d1, 2, true, true));  // word at +4
    // A doubleword load spans both stores; a word stays inside the first.
    AllocLoadRun(NewLoad(kAddr, 3, false));
    AllocLoadRun(NewLoad(kAddr, 2, false));
    AllocLoadRun(NewLoad(kAddr + 6, 1, false));
    AllocLoadRun(NewLoad(kAddr + 4, 2, false));
    // Two one-byte stores under one halfword load: a byte from each.
    SquashAll(0);
    RunIdle(2);
    AllocStore(NewStore(kAddr + 6, 0xabull, 0, true, true));
    AllocStore(NewStore(kAddr + 7, 0xcdull, 0, true, true));
    AllocLoadRun(NewLoad(kAddr + 6, 1, false));  // bytes 6 and 7, one store each
    AllocLoadRun(NewLoad(kAddr, 3, false));      // only bytes 6 and 7 forwarded
    EndPhase();
  }

  // 4. the youngest older store wins; a younger store never does.
  void PhaseYoungest() {
    Phase("youngest");
    Fresh();
    const uint64_t kAddr = Memory::kBase + 0x300;
    const uint64_t d0 = 0x0000000000000011ull;
    const uint64_t d1 = 0x0000000000000022ull;
    const uint64_t d2 = 0x0000000000000033ull;
    const uint64_t d3 = 0x0000000000000044ull;
    AllocStore(NewStore(kAddr, d0, 3, true, true));
    AllocStore(NewStore(kAddr, d1, 3, true, true));
    AllocStore(NewStore(kAddr, d2, 3, true, true));
    AllocLoadRun(NewLoad(kAddr, 3, false));  // must take d2

    // A store allocated after the load is younger and must not forward. The
    // address is one no older store covers, so any forwarded byte could only
    // have come from the younger store. A non-zero memory latency keeps the
    // load in flight while the store lands.
    const uint64_t kOther = kAddr + 0x40;
    lq_lat_ = 8;
    OfferLoad(NewLoad(kOther, 3, false));
    AllocStore(NewStore(kOther, d3, 3, true, true));
    Settle(1024);
    Require(!shadow_sq_.empty(), "youngest", "the younger store had already left the queue");
    lq_lat_ = 0;
    Require(last_.lq_mem_byte_ctr >= 8, "youngest", "the younger store was forwarded");
    EndPhase();
  }

  // 5. different-size aliasing, byte by byte.
  void PhaseAliasing() {
    Phase("aliasing");
    Fresh();
    const uint64_t kAddr = Memory::kBase + 0x400;
    AllocStore(NewStore(kAddr + 3, 0x5aull, 0, true, true));  // 1-byte store
    AllocLoadRun(NewLoad(kAddr, 3, false));                   // 8-byte load
    SquashAll(0);
    RunIdle(2);

    AllocStore(NewStore(kAddr, 0xdeadbeefull, 2, true, true));  // 4-byte store
    AllocLoadRun(NewLoad(kAddr + 2, 1, false));                 // 2-byte load, in the middle
    AllocLoadRun(NewLoad(kAddr, 3, false));                     // 8-byte load, store narrow
    SquashAll(0);
    RunIdle(2);

    AllocStore(NewStore(kAddr + 6, 0xbeefull, 1, true, true));  // 2-byte store at +6
    AllocLoadRun(NewLoad(kAddr + 4, 2, false));                 // 4-byte load spanning it
    AllocLoadRun(NewLoad(kAddr + 6, 1, false));                 // exactly it
    EndPhase();
  }

  // 6. a resident store with no address blocks the load, and the load
  //    completes correctly once the address resolves.
  void PhaseUnknown() {
    Phase("unknown");
    Fresh();
    const uint64_t kAddr = Memory::kBase + 0x500;
    const uint64_t kData = 0x0123456789abcdefull;
    SqEnt s = NewStore(kAddr, kData, 3, /*addr_valid=*/false, /*data_valid=*/true);
    AllocStore(s);
    AllocLoadRun_NoSettle(NewLoad(kAddr, 3, false));

    for (int i = 0; i < 24; i++) {
      Cycle(DefaultStim(), false);
      Require(!last_.lq_result_valid, "unknown",
              "the load completed while a store's address was still unknown");
    }
    Require(last_.lq_replay_ctr > 0, "unknown", "the load was never replayed");

    FillStore(s.packed, true, kAddr, 0, false, 0);
    Settle(1024);
    Require(last_.lq_count == 0, "unknown", "the load never completed after the resolution");
    Require(last_.lq_mem_byte_ctr == 0, "unknown", "a byte came from memory while the store covered it");
    EndPhase();
  }

  // 7. a store's address arriving in the same cycle as the load's resolution
  //    gives the same bytes as one cycle earlier.
  void PhaseSameCycle() {
    Phase("same-cycle");
    Fresh();
    const uint64_t kAddr = Memory::kBase + 0x600;
    const uint64_t kData = 0xfedcba9876543210ull;

    // (a) the address resolves while the load is blocked at the resolution
    //     boundary.
    SqEnt s1 = NewStore(kAddr, kData, 3, false, true);
    AllocStore(s1);
    OfferLoad(NewLoad(kAddr, 3, false));
    RunIdle(3);
    FillStore(s1.packed, true, kAddr, 0, false, 0);  // "same cycle" as the load's check
    Settle(1024);
    const uint64_t early = last_.lq_result_data;
    const uint32_t early_mask = last_.lq_result_fwd_mask;

    // (b) the same store, resolved two cycles before the load is offered.
    SquashAll(0);
    Cycle(DefaultStim(), false);
    SqEnt s2 = NewStore(kAddr, kData, 3, false, true);
    AllocStore(s2);
    FillStore(s2.packed, true, kAddr, 0, false, 0);
    RunIdle(2);
    OfferLoad(NewLoad(kAddr, 3, false));
    Settle(1024);
    const uint64_t late = last_.lq_result_data;
    const uint32_t late_mask = last_.lq_result_fwd_mask;

    Require(early == kData && late == kData, "same-cycle",
            "the two resolutions gave different bytes: 0x" + U64(early) + " vs 0x" + U64(late));
    Require(early_mask == 0xff && late_mask == 0xff, "same-cycle",
            "a resolved store was not forwarded (" + U32(early_mask) + "/" + U32(late_mask) +
                ")");
    EndPhase();
  }

  // 8. a store that reached memory is read from memory afterwards.
  void PhaseDrain() {
    Phase("drain");
    Fresh();
    const uint64_t kAddr = Memory::kBase + 0x700;
    const uint64_t kData = 0x0f1e2d3c4b5a6978ull;
    SqEnt s = NewStore(kAddr, kData, 3, true, true);
    AllocStore(s);
    CommitStore(s.packed);
    // Run past the drain: the store leaves the queue at the acceptance edge and
    // then has to make it through the endpoint to memory.
    for (int i = 0; i < 32; i++) Cycle(DefaultStim(), false);
    Require(shadow_sq_.empty(), "drain", "the committed store never drained");
    if (std::getenv("MOSAIC_DEBUG") != nullptr) {
      std::fprintf(stderr, "DBG drain: sq.accepted=%llu lq.accepted=%llu peek=0x%02x want=0x%02x\n",
                   static_cast<unsigned long long>(mem_.sq.accepted),
                   static_cast<unsigned long long>(mem_.lq.accepted), mem_.Peek(kAddr),
                   uint8_t(kData));
    }
    Require(mem_.Peek(kAddr) == uint8_t(kData), "drain", "the store did not reach memory");
    CheckMemory();

    AllocLoadRun(NewLoad(kAddr, 3, false));
    Require(last_.lq_mem_byte_ctr == 8, "drain", "the load did not read all eight bytes from memory");
    Require(last_.lq_fwd_byte_ctr == 0, "drain", "a drained store was forwarded from");
    EndPhase();
  }

  // 9. a misaligned load is the endpoint's trap, with no forwarded bytes.
  void PhaseFault() {
    Phase("fault");
    Fresh();
    const uint64_t kAddr = Memory::kBase + 0x800;
    AllocLoadRun(NewLoad(kAddr + 1, 2, false, /*fault=*/true));
    Require(last_.lq_fault_ctr == 1, "fault", "the trap was not counted");
    Require(last_.lq_last_fault_cause == kExcLoadMisaligned, "fault", "the wrong cause");
    Require(last_.lq_fwd_byte_ctr == 0, "fault", "a trapping load forwarded bytes");
    EndPhase();
  }

  // 10. random operation, shadow-compared every cycle.
  void PhaseSoak(uint64_t seed, int cycles) {
    Phase("soak");
    Fresh();
    mosaic::Rng rng(seed);
    std::vector<uint32_t> need_addr;
    std::vector<uint32_t> need_data;

    for (int i = 0; i < cycles; i++) {
      Stim s = DefaultStim();
      if (rng.Chance(25)) s.lq_result_ready = false;
      if (rng.Chance(15)) s.lq_latency = rng.Below(3);
      if (rng.Chance(15)) s.sq_latency = rng.Below(3);
      if (rng.Chance(10)) s.lq_mem_req_ready = false;
      if (rng.Chance(10)) s.sq_mem_req_ready = false;

      // Free the store queue without draining: a whole-queue squash, which the
      // store queue spares nothing from because nothing is committed here.
      if (!shadow_sq_.empty() &&
          (shadow_sq_.size() >= g_.sq_entries - 1 || rng.Chance(12))) {
        s.sq_squash_valid = true;
        s.sq_squash_all = true;
        s.sq_squash_gen = 0;
        need_addr.clear();
        need_data.clear();
      }

      if (rng.Chance(55) && !s.sq_squash_valid && shadow_sq_.size() < g_.sq_entries) {
        const uint32_t size = rng.Below(4);
        const uint32_t align = SizeBytes(size);
        const uint64_t addr = Memory::kBase + 0x1000 + 8 * rng.Below(16) + align * rng.Below(8 / align);
        const uint64_t data = (uint64_t(rng.Next()) << 17) ^ rng.Next();
        const bool av = !rng.Chance(25);
        const bool dv = !rng.Chance(25);
        SqEnt e = NewStore(addr, data, size, av, dv);
        s.sq_alloc_valid = true;
        s.sq_alloc_id = e.packed;
        s.sq_alloc_seq = e.seq;
        s.sq_alloc_base = e.base;
        s.sq_alloc_imm = e.imm;
        s.sq_alloc_size = e.size;
        s.sq_alloc_addr_valid = e.addr_valid;
        s.sq_alloc_data = e.data;
        s.sq_alloc_data_valid = e.data_valid;
        if (!av) need_addr.push_back(e.packed);
        if (!dv) need_data.push_back(e.packed);
      }

      if (!need_addr.empty() && rng.Chance(45)) {
        const size_t pick = rng.Below(uint32_t(need_addr.size()));
        const uint32_t packed = need_addr[pick];
        const uint64_t addr = Memory::kBase + 0x1000 + 8 * rng.Below(16);
        s.sq_fill_valid = true;
        s.sq_fill_id = packed;
        s.sq_fill_base = addr;
        s.sq_fill_addr_valid = true;
        need_addr.erase(need_addr.begin() + long(pick));
      }
      if (!need_data.empty() && rng.Chance(45)) {
        const size_t pick = rng.Below(uint32_t(need_data.size()));
        const uint32_t packed = need_data[pick];
        s.sq_fill_valid = true;
        s.sq_fill_id = packed;
        s.sq_fill_data = (uint64_t(rng.Next()) << 21) ^ rng.Next();
        s.sq_fill_data_valid = true;
        need_data.erase(need_data.begin() + long(pick));
      }

      if (rng.Chance(50) && shadow_lq_.size() < g_.lq_entries && !s.lq_alloc_valid) {
        const uint32_t size = rng.Below(4);
        const uint32_t align = SizeBytes(size);
        const uint64_t addr =
            Memory::kBase + 0x1000 + 8 * rng.Below(16) + align * rng.Below(8 / align);
        const bool sign = rng.Chance(50);
        LqEnt e = NewLoad(addr, size, sign);
        s.lq_alloc_valid = true;
        s.lq_alloc_id = e.packed;
        s.lq_alloc_seq = e.seq;
        s.lq_alloc_base = e.base;
        s.lq_alloc_imm = e.imm;
        s.lq_alloc_size = e.size;
        s.lq_alloc_signed = e.sign;
      }

      Cycle(s, false);
    }

    // Drain the queue: resolve every outstanding address and data, let the load
    // queue finish, then remove what is left.
    lq_lat_ = 0;
    lq_result_ready_ = true;
    lq_mem_ready_ = true;
    for (size_t i = 0; i < shadow_sq_.size(); i++) {
      const SqEnt& e = shadow_sq_[i];
      if (!e.addr_valid || !e.data_valid) {
        FillStore(e.packed, true, Memory::kBase + 0x1000 + 8 * uint64_t(i), 0, true,
                  (uint64_t(i) + 1) * 0x0101010101010101ull);
      }
    }
    Settle(4096);
    Require(shadow_lq_.empty(), "soak", "the load queue did not empty");
    SquashAll(0);
    RunIdle(2);
    Require(shadow_sq_.empty(), "soak", "the store queue did not empty");
    EndPhase();
  }

 private:
  // Offer a load without waiting for it: used where the load must be blocked.
  void AllocLoadRun_NoSettle(const LqEnt& e) { OfferLoad(e); }
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

  mosaic::Reporter reporter(options, std::string("Verilator ") + Verilated::productVersion());
  mosaic::ClockDriver clk;
  Vmosaic_load_queue_tb dut;

  std::string detail;
  bool passed = true;
  Bench bench(&dut, &clk, options.max_cycles);

  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();
    bench.ReadGeometry();

    bench.PhaseResetState();
    bench.PhaseNarrow();
    bench.PhaseWide();
    bench.PhaseYoungest();
    bench.PhaseAliasing();
    bench.PhaseUnknown();
    bench.PhaseSameCycle();
    bench.PhaseDrain();
    bench.PhaseFault();
    bench.PhaseSoak(options.seed, 3000);

    Require(bench.run_loads() > 0, "coverage", "no load was ever allocated");
    Require(bench.run_stores() > 0, "coverage", "no store was ever allocated");
    Require(bench.run_replays() > 0, "coverage", "no load was ever replayed");
    Require(bench.run_fwd_bytes() > 0, "coverage", "no byte was ever forwarded");
    Require(bench.run_mem_bytes() > 0, "coverage", "no byte was ever read from memory");
    Require(bench.run_faults() > 0, "coverage", "no load ever faulted");

    detail = "load queue and store-to-load forwarding contract holds: " +
             std::to_string(bench.checks()) + " checks over " +
             std::to_string(clk.cycle()) + " cycles; " + std::to_string(bench.run_loads()) +
             " loads, " + std::to_string(bench.run_stores()) + " stores, " +
             std::to_string(bench.run_replays()) + " replays, " +
             std::to_string(bench.run_fwd_bytes()) + " forwarded bytes, " +
             std::to_string(bench.run_mem_bytes()) + " memory bytes, seed " +
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
