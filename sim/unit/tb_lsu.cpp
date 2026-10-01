// ============================================================================
// tb_lsu.cpp -- CASE=lsu.size_fault_boundaries, work package I-033.
//
// The DUT is `rtl/core/mosaic_lsu_endpoint.sv`: the ordered memory endpoint.
// This file is its *independent* verification. The DUT is a candidate under
// test, not a specification; every expected value below is written from the
// RISC-V load/store rules and from the module's own documented contract
// (rtl/core/mosaic_lsu_endpoint.sv, "the policy" / "byte lanes" / "the state
// machine" sections), never from the RTL's expressions.
//
// ---------------------------------------------------------------- the oracle
//
// Half of the verification is the memory model, and it is shared by nothing:
//
//   * `MemoryModel` is a byte-addressed region behind the DUT's memory port.
//     It implements the *lane convention* the endpoint documents -- a response
//     returns a doubleword whose byte at `addr` is lane `addr[2:0]`, and a store
//     writes exactly the bytes whose strobes are set -- with a programmable
//     latency, programmable back-pressure on both directions and injectable
//     faults. It does NOT check alignment: an unaligned access is assembled and
//     served, which is exactly why the misalignment trap has to live in the DUT
//     and why the case asserts on the model's own accepted-request counter.
//   * `SpecExtract`/`SpecWdata`/`SpecWstrb` are the value rules from the ISA:
//     lane shift, sign or zero extension, byte strobes, store shift.
//
// The DUT is never compared against itself. `SpecExtract` is fed the model's
// memory, not the DUT's `rdata`; `SpecWstrb`/`SpecWdata` are compared against
// what the DUT presented; the traps are compared against the exception codes in
// mosaic_pkg.
//
// ------------------------------------------------------- every-cycle checks
//
// The driver runs one `Cycle()` per clock and, on every cycle of every phase,
// checks (see `CheckCycle`):
//
//   * `req_ready_o` is asserted exactly when the endpoint holds no outstanding
//     request -- the "one transaction at a time" contract in its observable form;
//   * the response owed to the driver is presented, and every field of it
//     (`id`, `fault`, `cause`, `tval`, and for a successful load `data`) matches
//     the model; a response that starts being presented is held, unchanged,
//     until it is accepted, and no response appears when none is owed;
//   * the memory request carries the right address/size/kind and, for a store,
//     the strobes and shifted data the specification demands, and its payload
//     does not change while `valid && !ready`;
//   * `o_busy`, `o_inflight_addr/size` and the six counters equal the driver's
//     own tally of what it observed, so a counter that drifts from the traffic
//     is caught in the cycle it drifts.
//
// The two-snapshot rule is honoured by construction: `Cycle()` reads every DUT
// output once, before the rising edge, and *only* that snapshot is used by the
// checks and by the state update. Nothing is re-read after the edge and mixed
// into a comparison.
//
// ------------------------------------------------------------------- phases
//
//   1. reset-state       the documented cold state.
//   2. size-sign-matrix  4 sizes x {signed,unsigned} x every aligned lane, with
//                        a value chosen so sign extension differs; the store
//                        side checked byte by byte across the whole doubleword.
//   3. misalignment      every misaligned offset for every size and both kinds:
//                        the response must be a trap with cause 4/6 and tval ==
//                        address, and the model must observe ZERO transactions.
//                        Plus the precedence rule: the same unmapped address
//                        aligned faults with 5/7 and unaligned traps with 4/6.
//   4. access-fault      the memory reports a fault: cause 5/7, tval == address.
//   5. backpressure      the consumer refuses the response for many cycles and
//                        must be offered the identical one; the memory refuses
//                        the request and the DUT must hold its payload.
//   6. ordering-identity two requests in acceptance order, a second offered
//                        while the first is in flight, and ids that differ only
//                        in the generation field.
//   7. reset-in-flight   reset while a transaction is in the memory system.
//   8. random-soak       random sizes, addresses, faults and back-pressure,
//                        shadow-compared and conservation-checked every cycle.
//
// Every phase resets first, ends with the counters compared to the driver's
// tally, and reports its own check count on stdout so the evidence is in the
// run log.
// ============================================================================

#include <verilated.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sim_common.h"
#include "Vmosaic_lsu_endpoint_tb.h"

namespace {

// ---------------------------------------------------------------- failures
struct Failure {
  std::string what;
};

[[noreturn]] void Fail(const std::string& where, const std::string& detail) {
  throw Failure{where + ": " + detail};
}

std::string Bool(bool value) { return value ? "1" : "0"; }
std::string U64(uint64_t value) { return mosaic::Hex(value); }
std::string U32(uint32_t value) { return mosaic::Hex(value, 8); }

// ------------------------------------------------------- the specification
// Size codes, from mosaic_pkg.
enum { kSizeByte = 0, kSizeHalf = 1, kSizeWord = 2, kSizeDbl = 3 };
// Exception causes, from mosaic_pkg (RISC-V privileged spec v1.12).
enum {
  kCauseLoadMisaligned = 4,
  kCauseLoadAccess = 5,
  kCauseStoreMisaligned = 6,
  kCauseStoreAccess = 7
};

uint32_t SizeBytes(uint32_t size) {
  static const uint32_t kBytes[4] = {1, 2, 4, 8};
  return kBytes[size & 3u];
}

// An access is misaligned exactly when it crosses the boundary its size
// demands: the low bits must be zero for all bytes the access covers.
bool SpecMisaligned(uint64_t addr, uint32_t size) {
  return (addr & (uint64_t(SizeBytes(size)) - 1u)) != 0;
}

// The byte strobes an access of this size at this address owns. Written from
// the specification (which bytes belong to the access), not from the RTL.
uint8_t SpecWstrb(uint32_t size, uint64_t addr) {
  const uint32_t mask = (1u << SizeBytes(size)) - 1u;
  return uint8_t((mask << (addr & 7u)) & 0xffu);
}

// A store's payload, shifted so byte i of the operand lands in the lane the
// address selects.
uint64_t SpecWdata(uint64_t store_data, uint64_t addr) {
  return store_data << (8u * (addr & 7u));
}

// The load's architectural value: the addressed lanes of the lane-aligned
// doubleword, sign- or zero-extended to XLEN.
uint64_t SpecExtract(uint64_t lane_aligned_rdata, uint64_t addr, uint32_t size,
                     bool is_signed) {
  const uint32_t bytes = SizeBytes(size);
  const uint64_t mask = (bytes == 8) ? ~0ull : ((1ull << (8u * bytes)) - 1ull);
  uint64_t value = (lane_aligned_rdata >> (8u * (addr & 7u))) & mask;
  if (is_signed && bytes < 8) {
    const uint64_t sign_bit = 1ull << (8u * bytes - 1u);
    if (value & sign_bit) value |= ~mask;
  }
  return value;
}

// Sign- or zero-extend a `size`-byte operand. Used for a value the driver
// poked into its own memory, so the expected load result does not pass through
// the memory model at all.
uint64_t SpecExtend(uint64_t operand, uint32_t size, bool is_signed) {
  const uint32_t bytes = SizeBytes(size);
  const uint64_t mask = (bytes == 8) ? ~0ull : ((1ull << (8u * bytes)) - 1ull);
  uint64_t value = operand & mask;
  if (is_signed && bytes < 8) {
    const uint64_t sign_bit = 1ull << (8u * bytes - 1u);
    if (value & sign_bit) value |= ~mask;
  }
  return value;
}

const uint64_t kRegionBase = 0x80000000ull;
const uint64_t kRegionBytes = 0x1000ull;  // 4 KiB. Outside is an access fault.

// ------------------------------------------------------------- the memory
// An independent byte-addressed region behind the DUT's memory port.
//
// The lane convention is the model's interface contract, not the DUT's
// business: a response returns the doubleword whose byte at `addr` is lane
// `addr[2:0]` (i.e. the aligned block containing `addr`), and a store writes the
// bytes the specification says the access owns, taking their values from the
// data the DUT presented. A real memory does the second part with the strobes
// the DUT presents; the strobes are instead checked against the specification
// by the driver and the write here uses the specified bytes, so the model's
// contents never depend on a DUT output.
//
// Deliberately absent: any alignment check. An unaligned access is served, as
// the RTL's own comment says the bring-up model would.
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
  void PokeValue(uint64_t addr, uint32_t size, uint64_t value) {
    for (uint32_t i = 0; i < size; i++) Poke(addr + i, uint8_t(value >> (8u * i)));
  }

  // The doubleword whose byte at `addr` is lane `addr[2:0]`.
  uint64_t LaneAlignedRead(uint64_t addr) const {
    const uint64_t block = addr & ~7ull;
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; i++) {
      value |= uint64_t(Peek(block + i)) << (8u * i);
    }
    return value;
  }

  // ------------------------------------------------------- configuration
  void SetLatency(uint32_t cycles) { latency_ = cycles; }
  void SetNextFault(bool fault) { next_fault_ = fault; }
  void FlushPending() {
    pending_ = false;
    delay_ = 0;
  }
  // A single-outstanding memory cannot accept while it is answering.
  bool ReqReady(bool program_ready) const { return program_ready && !pending_; }

  // ------------------------------------------------------- response side
  bool RspValid() const { return pending_ && delay_ == 0; }
  uint64_t RspRdata() const { return pending_ ? pending_rdata_ : 0; }
  bool RspFault() const { return pending_ && pending_fault_; }

  // --------------------------------------------------------- the edge
  struct EdgeResult {
    bool accepted = false;
    bool consumed = false;
  };

  // Called once per cycle with the request the DUT presented *before* the edge.
  // `wdata` is the lane-aligned store payload (checked against the
  // specification by the driver before this runs). The model writes through the
  // lane convention: the byte at address `a` is lane `a[2:0]` of the payload.
  EdgeResult Edge(uint64_t cycle, bool mem_valid, bool program_ready, bool we,
                  uint64_t addr, uint32_t size_bytes, uint64_t wdata,
                  bool rsp_ready) {
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
        last_rdata_ = pending_rdata_;
        last_fault_ = pending_fault_;
        last_consume_cycle_ = cycle;
        result.consumed = true;
      }
    }
    return result;
  }

  uint64_t accepted() const { return accepted_; }
  uint64_t last_rdata() const { return last_rdata_; }
  bool last_fault() const { return last_fault_; }
  uint64_t last_consume_cycle() const { return last_consume_cycle_; }
  const std::vector<uint64_t>& request_log() const { return request_log_; }
  bool busy() const { return pending_; }

 private:
  std::vector<uint8_t> mem_;
  bool pending_ = false;
  uint32_t delay_ = 0;
  uint64_t pending_rdata_ = 0;
  bool pending_fault_ = false;
  uint32_t latency_ = 0;
  bool next_fault_ = false;
  uint64_t accepted_ = 0;
  uint64_t last_rdata_ = 0;
  bool last_fault_ = false;
  uint64_t last_consume_cycle_ = ~0ull;
  std::vector<uint64_t> request_log_;
};

// ------------------------------------------------------------------ traffic
struct Req {
  uint32_t id = 0;
  bool we = false;
  uint64_t base = 0;
  uint64_t imm = 0;
  uint64_t store_data = 0;
  uint32_t size = kSizeByte;
  bool is_signed = false;

  uint64_t addr() const { return base + imm; }
};

struct Stim {
  bool req_valid = false;
  uint32_t id = 0;
  bool we = false;
  uint64_t base = 0;
  uint64_t imm = 0;
  uint64_t store_data = 0;
  uint32_t size = 0;
  bool is_signed = false;
  bool rsp_ready = true;
  bool mem_req_ready = true;
};

struct DutOut {
  bool req_ready = false;
  bool rsp_valid = false;
  uint32_t rsp_id = 0;
  bool rsp_fault = false;
  uint64_t rsp_cause = 0;
  uint64_t rsp_tval = 0;
  uint64_t rsp_data = 0;
  bool mem_req_valid = false;
  bool mem_req_we = false;
  uint64_t mem_req_addr = 0;
  uint32_t mem_req_size = 0;
  uint32_t mem_req_wstrb = 0;
  uint64_t mem_req_wdata = 0;
  bool mem_rsp_ready = false;
  bool busy = false;
  uint32_t load_ctr = 0;
  uint32_t store_ctr = 0;
  uint32_t txn_ctr = 0;
  uint32_t misaligned_ctr = 0;
  uint32_t access_fault_ctr = 0;
  uint32_t rsp_ctr = 0;
  uint64_t last_fault_cause = 0;
  uint64_t last_fault_tval = 0;
  uint64_t inflight_addr = 0;
  uint32_t inflight_size = 0;
};

// The response the driver is owed for the request it has had accepted.
struct ExpRsp {
  bool active = false;
  uint32_t id = 0;
  bool we = false;
  bool is_load = false;
  uint32_t size = 0;
  bool is_signed = false;
  uint64_t store_data = 0;
  uint64_t addr = 0;
  bool fault = false;
  uint64_t cause = 0;
  uint64_t tval = 0;
  uint64_t data = 0;
  uint64_t valid_from = ~0ull;   // cycle the response must start being offered
  uint64_t offer_from = ~0ull;   // cycle the memory request must start being offered
  uint64_t addr_valid_from = ~0ull;
  bool mem_accepted = false;
  uint64_t mem_accept_cycle = ~0ull;
  bool misaligned = false;
  uint64_t accepted_baseline = 0;
};

struct Tallies {
  uint64_t loads = 0;
  uint64_t stores = 0;
  uint64_t txn = 0;
  uint64_t misaligned = 0;
  uint64_t access_fault = 0;
  uint64_t rsp = 0;
};

struct Resp {
  uint32_t id = 0;
  bool fault = false;
  uint64_t cause = 0;
  uint64_t tval = 0;
  uint64_t data = 0;
};

struct RunOpts {
  uint32_t latency = 0;
  bool fault = false;
  uint32_t req_stall = 0;
  uint32_t rsp_stall = 0;
  bool hold_next = false;
  Req next;
};

// The memory request payload, snapshotted for the transport-rule check.
struct MemPayload {
  bool we = false;
  uint64_t addr = 0;
  uint32_t size = 0;
  uint32_t wstrb = 0;
  uint64_t wdata = 0;
  bool operator==(const MemPayload& o) const {
    return we == o.we && addr == o.addr && size == o.size && wstrb == o.wstrb &&
           wdata == o.wdata;
  }
};

// ============================================================== the harness
class Bench {
 public:
  Bench(Vmosaic_lsu_endpoint_tb* dut, mosaic::ClockDriver* clk, uint64_t max_cycles)
      : dut_(dut), clk_(clk), max_cycles_(max_cycles) {}

  uint64_t checks() const { return checks_; }
  uint64_t req_stall_cycles() const { return req_stall_cycles_; }
  uint64_t rsp_stall_cycles() const { return rsp_stall_cycles_; }
  Tallies run_totals() const { return run_totals_; }

  // ------------------------------------------------------------- geometry
  void ReadGeometry() {
    id_w_ = dut_->o_id_w;
    strb_w_ = dut_->o_strb_w;
    size_w_ = dut_->o_size_w;
    hart_w_ = dut_->o_hart_w;
    rob_index_w_ = dut_->o_rob_index_w;
    rob_gen_w_ = dut_->o_rob_gen_w;
    uop_index_w_ = dut_->o_uop_index_w;
    xlen_w_ = dut_->o_xlen_w;
    Require(xlen_w_ == 64, "geometry", "XLEN is " + std::to_string(xlen_w_));
    Require(strb_w_ == xlen_w_ / 8, "geometry",
            "the strobe width is not XLEN/8: " + std::to_string(strb_w_));
    Require(size_w_ == 3, "geometry",
            "the size code is " + std::to_string(size_w_) + " bits wide");
    Require(id_w_ == hart_w_ + rob_index_w_ + rob_gen_w_ + uop_index_w_, "geometry",
            "the identity width " + std::to_string(id_w_) +
                " is not the sum of hart " + std::to_string(hart_w_) +
                " + rob_index " + std::to_string(rob_index_w_) + " + rob_gen " +
                std::to_string(rob_gen_w_) + " + uop_index " +
                std::to_string(uop_index_w_));
    Require(id_w_ <= 32, "geometry", "the identity is wider than the driver port");
    id_mask_ = (id_w_ >= 32) ? 0xffffffffu : uint32_t((1u << id_w_) - 1u);
  }

  uint32_t IdW() const { return id_w_; }
  uint32_t HartW() const { return hart_w_; }
  uint32_t RobIndexW() const { return rob_index_w_; }
  uint32_t RobGenW() const { return rob_gen_w_; }
  uint32_t UopIndexW() const { return uop_index_w_; }

  // One identity, packed in the order mosaic_id_pkg declares macro_id_t:
  // hart, rob_index, rob_gen, uop_index, with hart most significant.
  uint32_t MakeId(uint32_t hart, uint32_t rob_index, uint32_t rob_gen,
                  uint32_t uop_index) const {
    uint32_t value = uop_index & ((1u << uop_index_w_) - 1u);
    value |= (rob_gen & ((1u << rob_gen_w_) - 1u)) << uop_index_w_;
    value |= (rob_index & ((1u << rob_index_w_) - 1u)) << (uop_index_w_ + rob_gen_w_);
    value |= (hart & ((1u << hart_w_) - 1u)) << (uop_index_w_ + rob_gen_w_ + rob_index_w_);
    return value & id_mask_;
  }

  // ---------------------------------------------------------------- checks
  void Check(bool ok, const std::string& where, const std::string& detail) {
    checks_++;
    phase_checks_++;
    if (!ok) {
      Fail("[" + phase_name_ + "] " + where,
           detail + " @ cycle " + std::to_string(cycle_));
    }
  }

  void Require(bool ok, const std::string& where, const std::string& detail) {
    Check(ok, where, detail);
  }

  // ---------------------------------------------------------------- phases
  void Phase(const std::string& name) {
    phase_name_ = name;
    phase_checks_ = 0;
    std::printf("PHASE %-22s start @cycle %llu\n", name.c_str(),
                static_cast<unsigned long long>(clk_->cycle()));
  }

  void Fresh() {
    mem_.PatternFill();
    const Stim idle;
    for (int i = 0; i < 2; i++) Cycle(idle, true);
    for (int i = 0; i < 2; i++) Cycle(idle, false);
  }

  void EndPhase() {
    // Two idle cycles first: the cycle that accepted the last response still
    // presented it, and the endpoint needs one edge to drop it. Whatever the
    // endpoint does with two cycles of silence is observable, and an
    // out-of-place response or memory request here is a defect.
    const Stim idle;
    Cycle(idle, false);
    Cycle(idle, false);
    Require(!exp_.active, "phase-end", "a transaction is still outstanding");
    Require(in_flight_ == 0, "phase-end", "in-flight count is not zero");
    Require(!last_.rsp_valid, "phase-end", "a response is still being offered");
    Require(!last_.mem_req_valid, "phase-end", "a memory request is still being offered");
    Require(presented_ == t_.rsp, "phase-end",
            "composed " + std::to_string(t_.rsp) + " responses but offered " +
                std::to_string(presented_) + " to the consumer");
    run_totals_.loads += t_.loads;
    run_totals_.stores += t_.stores;
    run_totals_.txn += t_.txn;
    run_totals_.misaligned += t_.misaligned;
    run_totals_.access_fault += t_.access_fault;
    run_totals_.rsp += t_.rsp;
    std::printf("PHASE %-22s end   checks=%llu cycles=%llu loads=%llu stores=%llu "
                "txn=%llu misaligned=%llu access_fault=%llu rsp=%llu\n",
                phase_name_.c_str(),
                static_cast<unsigned long long>(phase_checks_),
                static_cast<unsigned long long>(clk_->cycle()),
                static_cast<unsigned long long>(t_.loads),
                static_cast<unsigned long long>(t_.stores),
                static_cast<unsigned long long>(t_.txn),
                static_cast<unsigned long long>(t_.misaligned),
                static_cast<unsigned long long>(t_.access_fault),
                static_cast<unsigned long long>(t_.rsp));
  }

  // -------------------------------------------------------------- stimulus
  static Stim StimFrom(const Req& r) {
    Stim s;
    s.req_valid = true;
    s.id = r.id;
    s.we = r.we;
    s.base = r.base;
    s.imm = r.imm;
    s.store_data = r.store_data;
    s.size = r.size;
    s.is_signed = r.is_signed;
    return s;
  }

  // ---------------------------------------------------------------- cycles
  void Cycle(const Stim& s, bool rst) {
    cycle_ = clk_->cycle();  // number of edges already applied == this cycle's index

    dut_->rst = rst ? 1 : 0;
    dut_->up_req_valid = s.req_valid ? 1 : 0;
    dut_->up_req_id = s.id & id_mask_;
    dut_->up_req_we = s.we ? 1 : 0;
    dut_->up_req_base = s.base;
    dut_->up_req_imm = s.imm;
    dut_->up_req_size = s.size;
    dut_->up_req_signed = s.is_signed ? 1 : 0;
    dut_->up_req_store_data = s.store_data;
    dut_->up_rsp_ready = s.rsp_ready ? 1 : 0;
    dut_->dn_req_ready = s.mem_req_ready ? 1 : 0;
    dut_->dn_rsp_valid = mem_.RspValid() ? 1 : 0;
    dut_->dn_rsp_rdata = mem_.RspRdata();
    dut_->dn_rsp_fault = mem_.RspFault() ? 1 : 0;

    // The pre-edge snapshot. Everything below compares against this one read.
    dut_->clk = 0;
    dut_->eval();
    seen_ = Read();
    last_ = seen_;
    last_up_accept_ = s.req_valid && seen_.req_ready;
    if (seen_.mem_req_valid && !s.mem_req_ready) req_stall_cycles_++;
    if (seen_.rsp_valid && !s.rsp_ready) rsp_stall_cycles_++;

    const bool first_rst_cycle = rst && !rst_was_high_;
    if (!first_rst_cycle) CheckCycle(seen_, s);

    // The edge.
    dut_->clk = 1;
    dut_->eval();
    dut_->clk = 0;
    dut_->eval();
    clk_->Tick();
    Commit(seen_, s, rst);
    cycle_ = clk_->cycle();  // the index of the cycle that comes next
  }

  const DutOut& seen() const { return last_; }
  bool LastUpAccept() const { return last_up_accept_; }
  bool ExpectedPresented() const {
    return exp_.active && cycle_ >= exp_.valid_from;
  }
  bool outstanding() const { return exp_.active; }
  uint64_t rsp_takes() const { return rsp_takes_; }
  const Resp& last_taken() const { return last_taken_; }
  Tallies tallies() const { return t_; }
  const MemoryModel& mem() const { return mem_; }

  // --------------------------------------------------------- RunTxn
  // Offers `r` until it is accepted, then waits for its response to be
  // accepted by the consumer, running the per-cycle checks throughout.
  Resp RunTxn(const Req& r, const RunOpts& opt) {
    Require(!exp_.active, "run-txn", "a transaction is already outstanding");
    mem_.SetLatency(opt.latency);
    mem_.SetNextFault(opt.fault);
    const Stim idle;
    const uint64_t takes_before = rsp_takes_;
    bool accepted = false;
    int req_stall_left = int(opt.req_stall);
    int rsp_stall_left = -1;

    for (uint32_t guard = 0; guard < 4000; guard++) {
      Stim s = accepted ? (opt.hold_next ? StimFrom(opt.next) : idle) : StimFrom(r);
      // The memory refuses the request for `req_stall` cycles *after the
      // endpoint has accepted it*, so the count is exactly what the phase
      // requires and not one cycle less.
      if (accepted && req_stall_left > 0) {
        s.mem_req_ready = false;
        req_stall_left--;
      }
      if (opt.rsp_stall > 0 && ExpectedPresented()) {
        if (rsp_stall_left < 0) rsp_stall_left = int(opt.rsp_stall);
        if (rsp_stall_left > 0) {
          s.rsp_ready = false;
          rsp_stall_left--;
        }
      }
      Cycle(s, false);
      if (LastUpAccept()) accepted = true;
      if (rsp_takes_ > takes_before) return last_taken_;
      Require(clk_->cycle() <= max_cycles_, "run-txn",
              "the run exceeded --max-cycles");
    }
    Fail("run-txn", "the transaction did not complete within 4000 cycles");
  }

 private:
  // ------------------------------------------------------------ readback
  DutOut Read() {
    DutOut o;
    o.req_ready = dut_->up_req_ready != 0;
    o.rsp_valid = dut_->up_rsp_valid != 0;
    o.rsp_id = dut_->up_rsp_id & id_mask_;
    o.rsp_fault = dut_->up_rsp_fault != 0;
    o.rsp_cause = dut_->up_rsp_cause;
    o.rsp_tval = dut_->up_rsp_tval;
    o.rsp_data = dut_->up_rsp_data;
    o.mem_req_valid = dut_->dn_req_valid != 0;
    o.mem_req_we = dut_->dn_req_we != 0;
    o.mem_req_addr = dut_->dn_req_addr;
    o.mem_req_size = dut_->dn_req_size;
    o.mem_req_wstrb = dut_->dn_req_wstrb;
    o.mem_req_wdata = dut_->dn_req_wdata;
    o.mem_rsp_ready = dut_->dn_rsp_ready != 0;
    o.busy = dut_->o_busy != 0;
    o.load_ctr = dut_->o_load_ctr;
    o.store_ctr = dut_->o_store_ctr;
    o.txn_ctr = dut_->o_txn_ctr;
    o.misaligned_ctr = dut_->o_misaligned_ctr;
    o.access_fault_ctr = dut_->o_access_fault_ctr;
    o.rsp_ctr = dut_->o_rsp_ctr;
    o.last_fault_cause = dut_->o_last_fault_cause;
    o.last_fault_tval = dut_->o_last_fault_tval;
    o.inflight_addr = dut_->o_inflight_addr;
    o.inflight_size = dut_->o_inflight_size;
    return o;
  }

  // ------------------------------------------------------- the per-cycle check
  void CheckCycle(const DutOut& o, const Stim& s) {
    const uint64_t n = cycle_;
    const bool mem_ready_now = s.mem_req_ready && !mem_.busy();

    // 1. capacity: ready exactly when no request is outstanding.
    Check(o.req_ready == (in_flight_ == 0), "request-ready",
          "req_ready_o=" + Bool(o.req_ready) + " with " +
              std::to_string(in_flight_) + " accepted request(s) outstanding");
    Check(o.busy == (in_flight_ != 0), "busy",
          "o_busy=" + Bool(o.busy) + " with " + std::to_string(in_flight_) +
              " accepted request(s) outstanding");

    // 2. the response the driver is owed.
    const bool due = exp_.active && n >= exp_.valid_from;
    if (due) {
      Check(o.rsp_valid, "response-offered",
            "no response presented while one is owed (id=" + U32(exp_.id) + ")");
      Check(o.rsp_id == exp_.id, "response-id",
            "expected " + U32(exp_.id) + ", got " + U32(o.rsp_id));
      Check(o.rsp_fault == exp_.fault, "response-fault",
            "expected fault=" + Bool(exp_.fault) + ", got " + Bool(o.rsp_fault));
      if (exp_.fault) {
        Check(o.rsp_cause == exp_.cause, "response-cause",
              "expected cause " + std::to_string(exp_.cause) + ", got " +
                  std::to_string(o.rsp_cause));
        Check(o.rsp_tval == exp_.tval, "response-tval",
              "expected tval " + U64(exp_.tval) + ", got " + U64(o.rsp_tval));
      } else if (exp_.is_load) {
        Check(o.rsp_data == exp_.data, "response-data",
              "expected " + U64(exp_.data) + ", got " + U64(o.rsp_data));
      }
    } else {
      Check(!o.rsp_valid, "unexpected-response",
            "a response was offered when none was owed");
    }

    // 3. a misaligned access is a trap decided from the address alone, so the
    //    memory model must never observe it. This is asserted on the model's
    //    own counter, not on any DUT signal, and on every cycle of the
    //    transaction rather than only at the end of it.
    if (exp_.active && exp_.misaligned) {
      Check(mem_.accepted() == exp_.accepted_baseline, "misaligned-touches-memory",
            "the memory model accepted " +
                std::to_string(mem_.accepted() - exp_.accepted_baseline) +
                " transaction(s) for a misaligned access");
    }

    // 4. the memory request the driver is owed.
    if (exp_.active && o.mem_req_valid) {
      Check(o.mem_req_addr == exp_.addr, "memory-address",
            "expected " + U64(exp_.addr) + ", got " + U64(o.mem_req_addr));
      Check(o.mem_req_size == exp_.size, "memory-size",
            "expected size " + std::to_string(exp_.size) + ", got " +
                std::to_string(o.mem_req_size));
      Check(o.mem_req_we == exp_.we, "memory-kind",
            "expected we=" + Bool(exp_.we) + ", got " + Bool(o.mem_req_we));
      if (exp_.we) {
        const uint8_t wstrb = SpecWstrb(exp_.size, exp_.addr);
        const uint64_t wdata = SpecWdata(exp_.store_data, exp_.addr);
        Check(o.mem_req_wstrb == wstrb, "memory-wstrb",
              "expected " + U32(wstrb) + ", got " + U32(o.mem_req_wstrb));
        Check(o.mem_req_wdata == wdata, "memory-wdata",
              "expected " + U64(wdata) + ", got " + U64(o.mem_req_wdata));
      }
      MemPayload payload{o.mem_req_we, o.mem_req_addr, o.mem_req_size,
                         o.mem_req_wstrb, o.mem_req_wdata};
      if (!mem_ready_now) {
        if (hold_valid_) {
          Check(payload == hold_, "memory-request-stable",
                "the request payload changed while valid && !ready");
        } else {
          hold_valid_ = true;
          hold_ = payload;
        }
      } else {
        hold_valid_ = false;
      }
    } else {
      hold_valid_ = false;
      if (!exp_.active) {
        Check(!o.mem_req_valid, "unexpected-memory-request",
              "a memory request was offered while the endpoint is idle");
      }
    }
    if (exp_.active && !exp_.mem_accepted && n >= exp_.offer_from) {
      Check(o.mem_req_valid, "memory-request-offered",
            "the accepted request was not offered to memory");
    }
    if (exp_.mem_accepted && n > exp_.mem_accept_cycle) {
      Check(!o.mem_req_valid, "memory-request-withdrawn",
            "the memory request is still offered after it was accepted");
    }

    // 5. the observable address of the access being served.
    if (exp_.active && n >= exp_.addr_valid_from) {
      Check(o.inflight_addr == exp_.addr, "inflight-address",
            "expected " + U64(exp_.addr) + ", got " + U64(o.inflight_addr));
      Check(o.inflight_size == exp_.size, "inflight-size",
            "expected size " + std::to_string(exp_.size) + ", got " +
                std::to_string(o.inflight_size));
    }

    // 6. the counters, against the driver's own tally of what it saw.
    CheckCounters(o);

    // 7. the last fault pair the endpoint reported.
    Check(o.last_fault_cause == last_fault_cause_, "last-fault-cause",
          "expected " + U64(last_fault_cause_) + ", got " + U64(o.last_fault_cause));
    Check(o.last_fault_tval == last_fault_tval_, "last-fault-tval",
          "expected " + U64(last_fault_tval_) + ", got " + U64(o.last_fault_tval));
  }

  void CheckCounters(const DutOut& o) {
    Check(o.load_ctr == t_.loads, "load-counter",
          "expected " + std::to_string(t_.loads) + ", got " + std::to_string(o.load_ctr));
    Check(o.store_ctr == t_.stores, "store-counter",
          "expected " + std::to_string(t_.stores) + ", got " + std::to_string(o.store_ctr));
    Check(o.txn_ctr == t_.txn, "txn-counter",
          "expected " + std::to_string(t_.txn) + ", got " + std::to_string(o.txn_ctr));
    Check(o.misaligned_ctr == t_.misaligned, "misaligned-counter",
          "expected " + std::to_string(t_.misaligned) + ", got " +
              std::to_string(o.misaligned_ctr));
    Check(o.access_fault_ctr == t_.access_fault, "access-fault-counter",
          "expected " + std::to_string(t_.access_fault) + ", got " +
              std::to_string(o.access_fault_ctr));
    Check(o.rsp_ctr == t_.rsp, "response-counter",
          "expected " + std::to_string(t_.rsp) + ", got " + std::to_string(o.rsp_ctr));
  }

  // ------------------------------------------------------------ the update
  void Commit(const DutOut& o, const Stim& s, bool rst) {
    const uint64_t n = cycle_;
    rst_was_high_ = rst;
    if (rst) {
      ClearDriverState();
      mem_.FlushPending();
      mem_.SetLatency(0);
      mem_.SetNextFault(false);
      return;
    }

    // 1. the memory's edge. `spec_wdata` is the payload the specification
    //    demands, so the model's contents never come from a DUT output.
    if (o.rsp_valid && !prev_rsp_valid_) presented_++;
    prev_rsp_valid_ = o.rsp_valid;

    const uint64_t spec_wdata = exp_.active ? SpecWdata(exp_.store_data, exp_.addr) : 0;
    // The memory's own accepted-request counter, read before this edge; a
    // misaligned access may not move it.
    const uint64_t accepted_pre = mem_.accepted();
    const MemoryModel::EdgeResult me = mem_.Edge(
        n, o.mem_req_valid, s.mem_req_ready, o.mem_req_we, o.mem_req_addr,
        SizeBytes(o.mem_req_size), spec_wdata, o.mem_rsp_ready);
    if (me.accepted) {
      t_.txn++;
      exp_.mem_accepted = true;
      exp_.mem_accept_cycle = n;
    }
    if (me.consumed) {
      // The memory answered; the endpoint composes the response this edge and
      // offers it from the next one.
      t_.rsp++;
      exp_.fault = mem_.last_fault();
      exp_.cause = exp_.we ? kCauseStoreAccess : kCauseLoadAccess;
      exp_.tval = exp_.addr;
      exp_.data = SpecExtract(mem_.last_rdata(), exp_.addr, exp_.size, exp_.is_signed);
      exp_.valid_from = n + 1;
      if (exp_.fault) {
        t_.access_fault++;
        last_fault_cause_ = exp_.cause;
        last_fault_tval_ = exp_.tval;
      }
    }

    // 2. an upstream request accepted this edge.
    if (o.req_ready && s.req_valid) {
      Require(!exp_.active, "request-accepted-while-busy",
              "a request was accepted while another was outstanding");
      const uint64_t addr = s.base + s.imm;
      t_.loads += s.we ? 0 : 1;
      t_.stores += s.we ? 1 : 0;
      in_flight_++;
      exp_ = ExpRsp();
      exp_.active = true;
      exp_.id = s.id & id_mask_;
      exp_.we = s.we;
      exp_.is_load = !s.we;
      exp_.size = s.size;
      exp_.is_signed = s.is_signed;
      exp_.store_data = s.store_data;
      exp_.addr = addr;
      exp_.addr_valid_from = n + 1;
      if (SpecMisaligned(addr, s.size)) {
        // The trap path: the response is composed from the address alone and
        // the memory must never be asked.
        t_.misaligned++;
        t_.rsp++;
        exp_.misaligned = true;
        exp_.accepted_baseline = accepted_pre;
        exp_.fault = true;
        exp_.cause = s.we ? kCauseStoreMisaligned : kCauseLoadMisaligned;
        exp_.tval = addr;
        exp_.valid_from = n + 1;
        last_fault_cause_ = exp_.cause;
        last_fault_tval_ = exp_.tval;
      } else {
        exp_.offer_from = n + 1;
        exp_.valid_from = ~0ull;
      }
    }

    // 3. the consumer took the response this edge.
    if (o.rsp_valid && s.rsp_ready) {
      Require(exp_.active, "response-taken-when-none-owed",
              "the consumer accepted a response the driver did not expect");
      last_taken_.id = o.rsp_id;
      last_taken_.fault = o.rsp_fault;
      last_taken_.cause = o.rsp_cause;
      last_taken_.tval = o.rsp_tval;
      last_taken_.data = o.rsp_data;
      in_flight_--;
      exp_.active = false;
      rsp_takes_++;
    }
  }

  void ClearDriverState() {
    exp_ = ExpRsp();
    in_flight_ = 0;
    t_ = Tallies();
    presented_ = 0;
    prev_rsp_valid_ = false;
    hold_valid_ = false;
    last_fault_cause_ = 0;
    last_fault_tval_ = 0;
  }

 public:
  MemoryModel& memory() { return mem_; }

 private:
  Vmosaic_lsu_endpoint_tb* dut_;
  mosaic::ClockDriver* clk_;
  uint64_t max_cycles_;

  MemoryModel mem_;

  // Geometry learned from the elaborated wrapper.
  uint32_t id_w_ = 0, strb_w_ = 0, size_w_ = 0, xlen_w_ = 0;
  uint32_t hart_w_ = 0, rob_index_w_ = 0, rob_gen_w_ = 0, uop_index_w_ = 0;
  uint32_t id_mask_ = 0;
  uint64_t checks_ = 0;

  // Phase bookkeeping.
  std::string phase_name_ = "init";
  uint64_t phase_checks_ = 0;

  // Cycle bookkeeping. `cycle_` is the index of the cycle whose pre-edge
  // snapshot was just taken, or (after `Cycle` returns) the next one.
  uint64_t cycle_ = 0;
  bool rst_was_high_ = false;

  DutOut seen_;
  DutOut last_;
  bool last_up_accept_ = false;

  ExpRsp exp_;
  Tallies t_;
  Tallies run_totals_;
  uint64_t in_flight_ = 0;
  uint64_t presented_ = 0;
  bool prev_rsp_valid_ = false;
  uint64_t rsp_takes_ = 0;
  uint64_t req_stall_cycles_ = 0;
  uint64_t rsp_stall_cycles_ = 0;
  uint64_t last_fault_cause_ = 0;
  uint64_t last_fault_tval_ = 0;
  Resp last_taken_;
  bool hold_valid_ = false;
  MemPayload hold_;
};

// ============================================================== the phases

// -------------------------------------------------------------- phase 1
void PhaseResetState(Bench* b) {
  b->Phase("reset-state");
  b->Fresh();
  const DutOut& o = b->seen();
  b->Require(!o.rsp_valid, "reset-state", "rsp_valid_o is high after reset");
  b->Require(!o.busy, "reset-state", "o_busy is high after reset");
  b->Require(o.req_ready, "reset-state", "req_ready_o is low after reset");
  b->Require(o.load_ctr == 0 && o.store_ctr == 0 && o.txn_ctr == 0 &&
                 o.misaligned_ctr == 0 && o.access_fault_ctr == 0 && o.rsp_ctr == 0,
             "reset-state", "a counter is not zero after reset");
  b->Require(!o.mem_req_valid, "reset-state", "a memory request is offered after reset");
  // The reset also clears the last-fault pair (checked every cycle against the
  // driver's own zeroed tally, but named here so the phase states it).
  b->Require(o.last_fault_cause == 0 && o.last_fault_tval == 0, "reset-state",
             "the last-fault pair is not zero after reset");
  b->EndPhase();
}

// -------------------------------------------------------------- phase 2
void PhaseSizeSign(Bench* b) {
  b->Phase("size-sign-matrix");
  b->Fresh();
  MemoryModel& mem = b->memory();

  // A value per size whose top byte has its sign bit set, so a signed and an
  // unsigned load of the same address cannot agree.
  static const uint64_t kOperand[4] = {0x80ull, 0xff80ull, 0x80000000ull,
                                       0x8000000000000000ull};
  // A store operand with eight distinct byte values, so a store that lands in
  // the wrong lane cannot coincide with the right one.
  const uint64_t kStoreData = 0x123456789abcdef0ull;

  uint32_t index = 0;
  for (uint32_t size = 0; size < 4; size++) {
    const uint32_t bytes = SizeBytes(size);
    for (uint32_t lane = 0; lane < 8; lane += bytes) {
      const uint64_t operand = kOperand[size];

      // ---- load: the poke is the expectation, with no memory read in between
      const uint64_t load_addr = kRegionBase + 0x100 + 0x40 * size + lane;
      mem.PokeValue(load_addr, bytes, operand);
      for (int sgn = 0; sgn < 2; sgn++) {
        const uint64_t expected = SpecExtend(operand, size, sgn != 0);
        Req r;
        r.id = b->MakeId(0, uint32_t(index & 0x3f), uint32_t((index * 7) & 0x7f),
                         uint32_t(index & 0x7));
        r.we = false;
        r.base = load_addr;
        r.imm = 0;
        r.size = size;
        r.is_signed = sgn != 0;
        const Resp got = b->RunTxn(r, RunOpts{});
        b->Require(got.id == r.id, "size-sign", "the response id is wrong");
        b->Require(!got.fault, "size-sign", "a successful load reported a fault");
        b->Require(got.data == expected, "size-sign",
                   "load size " + std::to_string(size) + " lane " +
                       std::to_string(lane) + (sgn ? " signed" : " unsigned") +
                       ": expected " + U64(expected) + ", got " + U64(got.data));
        index++;
      }

      // ---- store: exactly the intended bytes, whole doubleword checked
      const uint64_t store_addr = kRegionBase + 0x400 + 0x40 * size + lane;
      const uint64_t block = store_addr & ~7ull;
      uint8_t fill[8];
      for (uint32_t j = 0; j < 8; j++) {
        fill[j] = uint8_t(0xa0u + 0x11u * j);
        mem.Poke(block + j, fill[j]);
      }
      Req sr;
      sr.id = b->MakeId(0, uint32_t(index & 0x3f), uint32_t((index * 5) & 0x7f),
                        uint32_t(index & 0x7));
      sr.we = true;
      sr.base = store_addr;
      sr.imm = 0;
      sr.size = size;
      sr.store_data = kStoreData;
      const Resp sgot = b->RunTxn(sr, RunOpts{});
      b->Require(sgot.id == sr.id, "size-sign", "the response id is wrong");
      b->Require(!sgot.fault, "size-sign", "a successful store reported a fault");
      for (uint32_t j = 0; j < 8; j++) {
        const bool touched = (j >= lane) && (j < lane + bytes);
        const uint8_t expected =
            touched ? uint8_t(kStoreData >> (8u * (j - lane))) : fill[j];
        b->Require(mem.Peek(block + j) == expected, "store-bytes",
                   "size " + std::to_string(size) + " lane " + std::to_string(lane) +
                       ": byte " + std::to_string(j) + " of the doubleword is " +
                       U32(mem.Peek(block + j)) + ", expected " + U32(expected));
      }
      // And the readback the store implies: a load of the same access returns
      // the operand, which proves the bytes and their lanes agree.
      Req rr = sr;
      rr.we = false;
      rr.is_signed = false;
      rr.id = sr.id ^ 1u;
      const Resp rgot = b->RunTxn(rr, RunOpts{});
      b->Require(rgot.data == SpecExtend(kStoreData, size, false), "store-readback",
                 "the stored value did not read back for size " +
                     std::to_string(size) + " lane " + std::to_string(lane) +
                     ": expected " + U64(SpecExtend(kStoreData, size, false)) +
                     ", got " + U64(rgot.data));
      index++;
    }
  }
  b->EndPhase();
}

// -------------------------------------------------------------- phase 3
void PhaseMisaligned(Bench* b) {
  b->Phase("misalignment");
  b->Fresh();
  MemoryModel& mem = b->memory();
  uint32_t index = 0;

  for (uint32_t size = 0; size < 4; size++) {
    const uint32_t bytes = SizeBytes(size);
    for (uint32_t lane = 0; lane < 8; lane++) {
      if ((lane & (bytes - 1u)) == 0) continue;  // aligned
      for (int store = 0; store < 2; store++) {
        const uint64_t addr = kRegionBase + 0x800 + lane;
        const uint64_t accepted_before = mem.accepted();
        Req r;
        r.id = b->MakeId(0, uint32_t(index & 0x3f), uint32_t((index * 3) & 0x7f),
                         uint32_t(index & 0x7));
        r.we = store != 0;
        r.base = addr - 0x40;  // split so the adder is exercised
        r.imm = 0x40;
        r.size = size;
        r.is_signed = true;
        r.store_data = 0xffffffffffffffffull;
        const Resp got = b->RunTxn(r, RunOpts{});
        b->Require(got.id == r.id, "misalignment", "the response id is wrong");
        b->Require(got.fault, "misalignment",
                   "a misaligned access did not trap");
        b->Require(got.cause == (r.we ? kCauseStoreMisaligned : kCauseLoadMisaligned),
                   "misalignment",
                   "expected cause " +
                       std::to_string(r.we ? kCauseStoreMisaligned : kCauseLoadMisaligned) +
                       ", got " + std::to_string(got.cause));
        b->Require(got.tval == addr, "misalignment",
                   "expected tval " + U64(addr) + ", got " + U64(got.tval));
        // Two idle cycles, then the model's own counter: the memory must never
        // have been asked, and must not be asked late either.
        const Stim idle;
        b->Cycle(idle, false);
        b->Cycle(idle, false);
        b->Require(mem.accepted() == accepted_before, "misalignment",
                   "the memory accepted " + std::to_string(mem.accepted() -
                                                           accepted_before) +
                       " transaction(s) for a misaligned access of size " +
                       std::to_string(size) + " at lane " + std::to_string(lane));
        index++;
      }
    }
  }

  // ---- precedence: misalignment is decided from the address alone ---------
  // The same unmapped address, accessed aligned and unaligned, must report the
  // access fault and the misalignment respectively. The aligned one does reach
  // the memory (and the model answers with a fault for being out of region).
  const uint64_t unmapped = kRegionBase + 0x100000;  // outside the 4 KiB region
  {
    Req aligned;
    aligned.id = b->MakeId(0, 63, 1, 1);
    aligned.we = false;
    aligned.base = unmapped;
    aligned.imm = 0;
    aligned.size = kSizeWord;
    const Resp got = b->RunTxn(aligned, RunOpts{});
    b->Require(got.fault && got.cause == kCauseLoadAccess, "precedence",
               "an aligned access to an unmapped address did not report cause 5");
    b->Require(got.tval == unmapped, "precedence", "the access-fault tval is wrong");
  }
  {
    const uint64_t accepted_before = mem.accepted();
    Req bad;
    bad.id = b->MakeId(0, 63, 1, 2);
    bad.we = false;
    bad.base = unmapped + 5;  // misaligned by 5
    bad.imm = 0;
    bad.size = kSizeWord;
    const Resp got = b->RunTxn(bad, RunOpts{});
    b->Require(got.fault && got.cause == kCauseLoadMisaligned, "precedence",
               "a misaligned access to an unmapped address did not report cause 4");
    b->Require(got.tval == unmapped + 5, "precedence", "the trap tval is wrong");
    const Stim idle;
    b->Cycle(idle, false);
    b->Cycle(idle, false);
    b->Require(mem.accepted() == accepted_before, "precedence",
               "the memory was asked about a misaligned unmapped access");
  }
  b->EndPhase();
}

// -------------------------------------------------------------- phase 4
void PhaseAccessFault(Bench* b) {
  b->Phase("access-fault");
  b->Fresh();
  MemoryModel& mem = b->memory();
  uint32_t index = 0;

  // The model is told to fault; the endpoint must carry the fault out with the
  // access-fault cause and the address as tval. The load data is deliberately
  // not checked on a fault: `fault` is the authoritative field and the value of
  // a faulting load is architecturally never consumed.
  for (uint32_t size = 0; size < 4; size++) {
    for (int store = 0; store < 2; store++) {
      const uint64_t addr = kRegionBase + 0x300 + 0x40 * size;
      const uint64_t accepted_before = mem.accepted();
      Req r;
      r.id = b->MakeId(0, uint32_t(index & 0x3f), uint32_t((index * 11) & 0x7f),
                       uint32_t(index & 0x7));
      r.we = store != 0;
      r.base = addr;
      r.imm = 0;
      r.size = size;
      r.store_data = 0x0123456789abcdefull;
      RunOpts opt;
      opt.fault = true;
      const Resp got = b->RunTxn(r, opt);
      b->Require(got.id == r.id, "access-fault", "the response id is wrong");
      b->Require(got.fault, "access-fault",
                 "the memory faulted but the response did not carry a fault");
      b->Require(got.cause == (r.we ? kCauseStoreAccess : kCauseLoadAccess),
                 "access-fault",
                 "expected cause " +
                     std::to_string(r.we ? kCauseStoreAccess : kCauseLoadAccess) +
                     ", got " + std::to_string(got.cause));
      b->Require(got.tval == addr, "access-fault",
                 "expected tval " + U64(addr) + ", got " + U64(got.tval));
      b->Require(mem.accepted() == accepted_before + 1, "access-fault",
                 "the faulting access did not reach the memory exactly once");
      index++;
    }
  }

  // The counters the fault path owns, stated at the end of the phase.
  const Tallies t = b->tallies();
  b->Require(t.access_fault == 8, "access-fault",
             "expected 8 faulted responses, tallied " + std::to_string(t.access_fault));
  b->Require(t.misaligned == 0, "access-fault",
             "a directed aligned phase reported a misaligned access");
  b->EndPhase();
}

// -------------------------------------------------------------- phase 5
void PhaseBackpressure(Bench* b) {
  b->Phase("backpressure");
  b->Fresh();
  MemoryModel& mem = b->memory();

  // (a) The consumer refuses the response for many cycles. The identical
  //     response must still be offered -- the per-cycle check compares every
  //     field in every one of those cycles -- and the next request must still
  //     be served correctly.
  {
    const uint64_t addr = kRegionBase + 0x200;
    mem.PokeValue(addr, 8, 0x0011223344556677ull);
    Req r;
    r.id = b->MakeId(0, 5, 5, 5);
    r.we = false;
    r.base = addr;
    r.imm = 0;
    r.size = kSizeDbl;
    RunOpts opt;
    opt.latency = 2;
    opt.rsp_stall = 25;
    const uint64_t stalls_before = b->rsp_stall_cycles();
    const Resp got = b->RunTxn(r, opt);
    b->Require(got.data == 0x0011223344556677ull, "backpressure",
               "the held response changed while the consumer was stalled");
    b->Require(b->rsp_stall_cycles() - stalls_before >= 25, "backpressure",
               "the consumer did not actually hold the response off for 25 cycles");
  }
  {
    Req r;
    r.id = b->MakeId(0, 7, 7, 7);
    r.we = true;
    r.base = kRegionBase + 0x280;
    r.imm = 0;
    r.size = kSizeWord;
    r.store_data = 0xdeadbeefcafef00dull;
    const Resp got = b->RunTxn(r, RunOpts{});
    // A store's response carries no architectural value, so its `data` field is
    // don't-care here (recorded in the report); only the fault bit is checked.
    b->Require(!got.fault, "backpressure", "a store reported a fault");
    Req back = r;
    back.we = false;
    back.id = r.id ^ 1u;
    const Resp rb = b->RunTxn(back, RunOpts{});
    b->Require(rb.data == 0x00000000cafef00dull, "backpressure",
               "the request after a stalled response was served wrongly");
  }

  // (b) The memory refuses the request. The DUT must hold its payload (checked
  //     every cycle while valid && !ready) and must not accept another request.
  {
    const uint64_t addr = kRegionBase + 0x2c0;
    mem.PokeValue(addr, 4, 0x89abcdefull);
    Req r;
    r.id = b->MakeId(0, 9, 9, 9);
    r.we = false;
    r.base = addr - 0x11;
    r.imm = 0x11;
    r.size = kSizeWord;
    RunOpts opt;
    opt.req_stall = 20;
    const uint64_t accepted_before = mem.accepted();
    const uint64_t stalls_before = b->req_stall_cycles();
    const Resp got = b->RunTxn(r, opt);
    b->Require(b->req_stall_cycles() - stalls_before >= 20, "backpressure",
               "the endpoint held its request for only " +
                   std::to_string(b->req_stall_cycles() - stalls_before) + " cycles");
    b->Require(mem.accepted() == accepted_before + 1, "backpressure",
               "the memory accepted a request while it was refusing, or lost it");
    b->Require(got.data == 0x89abcdefull, "backpressure",
               "the request was not served correctly after the stall");
  }

  // (c) A long memory latency: nothing may be offered early, and the endpoint
  //     must still be ready to take the response when it arrives.
  {
    const uint64_t addr = kRegionBase + 0x2e0;
    mem.PokeValue(addr, 2, 0xff80ull);
    Req r;
    r.id = b->MakeId(0, 11, 11, 11);
    r.we = false;
    r.base = addr;
    r.imm = 0;
    r.size = kSizeHalf;
    r.is_signed = true;
    RunOpts opt;
    opt.latency = 12;
    const Resp got = b->RunTxn(r, opt);
    b->Require(got.data == 0xffffffffffffff80ull, "backpressure",
               "a long-latency load returned " + U64(got.data) + " instead of " +
                   U64(0xffffffffffffff80ull));
  }
  b->EndPhase();
}

// -------------------------------------------------------------- phase 6
void PhaseOrderingIdentity(Bench* b) {
  b->Phase("ordering-identity");
  b->Fresh();
  MemoryModel& mem = b->memory();

  // Two identities that differ only in the generation field. If the endpoint
  // (or this wrapper) dropped generation bits, these two would be
  // indistinguishable and one of the checks below would fail.
  const uint32_t id_a = b->MakeId(0, 21, 40, 3);
  const uint32_t id_b = b->MakeId(0, 21, 41, 3);
  b->Require(id_a != id_b, "identity",
             "the two generation-distinct identities collapsed to one value");

  const uint64_t addr_a = kRegionBase + 0x500;
  const uint64_t addr_b = kRegionBase + 0x540;
  mem.PokeValue(addr_a, 8, 0x0123456789abcdefull);
  mem.PokeValue(addr_b, 8, 0xfedcba9876543210ull);

  Req a;
  a.id = id_a;
  a.we = false;
  a.base = addr_a;
  a.imm = 0;
  a.size = kSizeDbl;

  Req b_req;
  b_req.id = id_b;
  b_req.we = false;
  b_req.base = addr_b;
  b_req.imm = 0;
  b_req.size = kSizeDbl;

  // B is offered while A is in the memory (a latency), so the acceptance order
  // is observable in the model's own request log.
  const uint64_t accepted_before = mem.accepted();
  RunOpts opt;
  opt.latency = 6;
  opt.hold_next = true;
  opt.next = b_req;
  const Resp ra = b->RunTxn(a, opt);
  b->Require(ra.id == id_a, "identity",
             "the first response carried " + U32(ra.id) + ", expected " + U32(id_a));
  b->Require(ra.data == 0x0123456789abcdefull, "identity",
             "the first response data is wrong");
  b->Require(mem.accepted() == accepted_before + 1, "ordering",
             "the second request was accepted while the first was in flight");

  // B was held valid across A's whole transaction; it must be accepted now.
  const Resp rb = b->RunTxn(b_req, RunOpts{});
  b->Require(rb.id == id_b, "identity",
             "the second response carried " + U32(rb.id) + ", expected " + U32(id_b));
  b->Require(rb.data == 0xfedcba9876543210ull, "identity",
             "the second response data is wrong");

  const std::vector<uint64_t>& log = mem.request_log();
  const size_t n_log = log.size();
  b->Require(n_log >= 2 && log[n_log - 2] == addr_a && log[n_log - 1] == addr_b,
             "ordering", "the memory saw the requests in the wrong order");

  // A held response blocks a new request: offer one while the response is
  // stalled and require the endpoint to refuse it.
  {
    Req c;
    c.id = b->MakeId(0, 33, 1, 1);
    c.we = false;
    c.base = kRegionBase + 0x580;
    c.imm = 0;
    c.size = kSizeByte;
    mem.PokeValue(c.base, 1, 0x5aull);
    RunOpts hold;
    hold.rsp_stall = 10;
    hold.hold_next = true;
    hold.next = b_req;  // valid, and must not be taken while C's response is held
    const Resp rc = b->RunTxn(c, hold);
    b->Require(rc.data == 0x5aull, "ordering", "the stalled byte load is wrong");
    // A, B and C each reached the memory; the fourth request offered while C's
    // response was held must not have been accepted.
    b->Require(mem.accepted() == accepted_before + 3, "ordering",
               "a request was accepted while a response was being held");
  }

  // Identity corner cases, including the full-width ones.
  const uint32_t corner[4] = {0, b->IdW() >= 32 ? 0xffffffffu : ((1u << b->IdW()) - 1u),
                              b->MakeId(0, 0, 0, 1), b->MakeId(0, 63, 127, 7)};
  for (int i = 0; i < 4; i++) {
    Req r;
    r.id = corner[i];
    r.we = false;
    r.base = kRegionBase + 0x5c0 + uint64_t(i);
    r.imm = 0;
    r.size = kSizeByte;
    mem.PokeValue(r.base, 1, uint8_t(0x30 + i));
    const Resp got = b->RunTxn(r, RunOpts{});
    b->Require(got.id == corner[i], "identity",
               "expected id " + U32(corner[i]) + ", got " + U32(got.id));
    b->Require(got.data == uint64_t(uint8_t(0x30 + i)), "identity",
               "the corner-case load data is wrong");
  }
  b->EndPhase();
}

// -------------------------------------------------------------- phase 7
void PhaseResetInFlight(Bench* b) {
  b->Phase("reset-in-flight");
  b->Fresh();
  MemoryModel& mem = b->memory();

  Req r;
  r.id = b->MakeId(0, 44, 44, 4);
  r.we = false;
  r.base = kRegionBase + 0x600;
  r.imm = 0;
  r.size = kSizeDbl;
  mem.PokeValue(r.base, 8, 0x5555aaaa5555aaaauLL);

  mem.SetLatency(9);
  mem.SetNextFault(false);
  const uint64_t accepted_before = mem.accepted();
  bool accepted = false;
  for (uint32_t i = 0; i < 50 && !accepted; i++) {
    b->Cycle(Bench::StimFrom(r), false);
    if (b->LastUpAccept()) accepted = true;
  }
  b->Require(accepted, "reset-in-flight", "the request was never accepted");
  // Let the memory take it, so the reset lands on a transaction that is in the
  // memory system and not merely queued at the endpoint.
  const Stim idle;
  b->Cycle(idle, false);
  b->Cycle(idle, false);
  b->Require(mem.accepted() == accepted_before + 1, "reset-in-flight",
             "the transaction never reached the memory");
  b->Require(mem.busy(), "reset-in-flight", "the memory has no outstanding access");

  // Reset while it is in flight. The model is flushed with the DUT: it is not a
  // real memory and cannot be reset by the harness's clock, so the two are
  // resynchronised deliberately (recorded in the NOT-verified list).
  b->Cycle(idle, true);
  b->Cycle(idle, true);
  b->Cycle(idle, false);
  b->Cycle(idle, false);
  const DutOut& o = b->seen();
  b->Require(!o.rsp_valid, "reset-in-flight", "rsp_valid_o is high after reset");
  b->Require(!o.busy, "reset-in-flight", "o_busy is high after reset");
  b->Require(o.req_ready, "reset-in-flight", "req_ready_o is low after reset");
  b->Require(o.load_ctr == 0 && o.store_ctr == 0 && o.txn_ctr == 0 &&
                 o.misaligned_ctr == 0 && o.access_fault_ctr == 0 && o.rsp_ctr == 0,
             "reset-in-flight", "a counter survived the reset");
  // The half-answered transaction must not have been completed by the reset.
  b->Require(mem.accepted() == accepted_before + 1, "reset-in-flight",
             "the memory saw an extra transaction across the reset");

  // The first new transaction after the reset is served correctly.
  Req again;
  again.id = b->MakeId(0, 45, 45, 5);
  again.we = false;
  again.base = r.base;
  again.imm = 0;
  again.size = kSizeDbl;
  const Resp got = b->RunTxn(again, RunOpts{});
  b->Require(got.id == again.id, "reset-in-flight", "the post-reset id is wrong");
  b->Require(got.data == 0x5555aaaa5555aaaauLL, "reset-in-flight",
             "the first post-reset load returned the wrong value");
  b->Require(mem.accepted() == accepted_before + 2, "reset-in-flight",
             "the post-reset transaction did not reach the memory exactly once");
  b->EndPhase();
}

// -------------------------------------------------------------- phase 8
Req RandomReq(Bench* b, mosaic::Rng* rng, uint32_t* index) {
  Req r;
  r.size = rng->Below(4);
  const uint32_t bytes = SizeBytes(r.size);
  uint64_t addr;
  if (bytes > 1 && rng->Chance(50)) {
    // A misaligned address: a lane the size cannot start on.
    uint32_t lane = 1 + rng->Below(7);
    if ((lane & (bytes - 1u)) == 0) lane = 1;  // bytes == 2 fixes the odd lanes
    addr = kRegionBase + 0x100 + (rng->Below(8) * 8) + lane;
  } else {
    addr = kRegionBase + 0x100 + (rng->Below(0x100) & ~uint64_t(bytes - 1u));
  }
  r.imm = rng->Next();
  r.base = addr - r.imm;  // random split, so the adder is exercised
  r.we = rng->Chance(40);
  r.is_signed = rng->Chance(50);
  r.store_data = rng->Next();
  r.id = uint32_t(rng->Next()) & ((1u << b->IdW()) - 1u);
  (*index)++;
  return r;
}

void PhaseSoak(Bench* b, uint64_t seed, uint32_t transactions) {
  b->Phase("random-soak");
  b->Fresh();
  mosaic::Rng rng(seed ^ 0x243f6a8885a308d3ull);

  const uint64_t loads_before = b->tallies().loads;
  const uint64_t stores_before = b->tallies().stores;
  const uint64_t mis_before = b->tallies().misaligned;
  const uint64_t fault_before = b->tallies().access_fault;
  uint32_t index = 0;
  uint32_t req_stall_cycles = 0;
  uint32_t rsp_stall_cycles = 0;

  Req cur = RandomReq(b, &rng, &index);
  for (uint32_t i = 0; i < transactions; i++) {
    RunOpts opt;
    opt.latency = rng.Below(6);
    opt.fault = rng.Chance(12);
    opt.req_stall = rng.Below(5);
    opt.rsp_stall = rng.Below(7);
    opt.hold_next = rng.Chance(60);
    opt.next = RandomReq(b, &rng, &index);
    req_stall_cycles += opt.req_stall;
    rsp_stall_cycles += opt.rsp_stall;
    const Resp got = b->RunTxn(cur, opt);
    b->Require(got.id == cur.id, "soak", "the response id is wrong");
    // The random fault can only be observed if the request was not misaligned
    // (a misaligned access never reaches the memory).
    cur = opt.hold_next ? opt.next : RandomReq(b, &rng, &index);
  }

  const Tallies t = b->tallies();
  b->Require(t.loads - loads_before > 0, "soak", "the soak issued no load");
  b->Require(t.stores - stores_before > 0, "soak", "the soak issued no store");
  b->Require(t.misaligned - mis_before > 0, "soak",
             "the soak never produced a misaligned access");
  b->Require(t.access_fault - fault_before > 0, "soak",
             "the soak never produced an access fault");
  b->Require(t.txn > 0, "soak", "the soak never reached the memory");
  b->Require(req_stall_cycles > 0, "soak", "the soak never stalled the request side");
  b->Require(rsp_stall_cycles > 0, "soak", "the soak never stalled the response side");
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
  Vmosaic_lsu_endpoint_tb dut;

  std::string detail;
  bool passed = true;
  Bench bench(&dut, &clk, options.max_cycles);
  try {
    dut.clk = 0;
    dut.rst = 0;
    dut.eval();

    bench.ReadGeometry();

    PhaseResetState(&bench);
    PhaseSizeSign(&bench);
    // The access-fault phase runs before the misalignment phase so that each
    // behaviour is caught by the phase that owns it: the misalignment phase's
    // precedence sub-test (an aligned access to an unmapped page) is itself a
    // genuine access fault and would otherwise be the first place a
    // fault-reporting defect showed up.
    PhaseAccessFault(&bench);
    PhaseMisaligned(&bench);
    PhaseBackpressure(&bench);
    PhaseOrderingIdentity(&bench);
    PhaseResetInFlight(&bench);
    PhaseSoak(&bench, options.seed, 400);

    const Tallies t = bench.run_totals();
    detail = "lsu endpoint contract holds: " + std::to_string(bench.checks()) +
             " checks over " + std::to_string(clk.cycle()) + " cycles; " +
             std::to_string(t.loads) + " loads, " + std::to_string(t.stores) +
             " stores, " + std::to_string(t.txn) + " memory transactions, " +
             std::to_string(t.misaligned) + " misaligned traps, " +
             std::to_string(t.access_fault) + " access faults, " +
             std::to_string(t.rsp) + " responses; seed " +
             std::to_string(options.seed);
  } catch (const Failure& f) {
    reporter.Mismatch(f.what, "contract holds", "contract violated");
    passed = false;
    detail = "contract violated after " + std::to_string(bench.checks()) +
             " checks: " + f.what;
    std::printf("FAILED AFTER %llu CHECKS\n",
                static_cast<unsigned long long>(bench.checks()));
  }

  dut.final();
  reporter.Check(passed, "no contract violation in any phase");
  return reporter.Finish(passed ? "PASS" : "FAIL", detail);
}
